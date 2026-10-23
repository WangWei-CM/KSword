#pragma once

// ============================================================
// DumpSymbolResolver.h
// 作用：
// - 用 DbgHelp 加载 PDB，把调用栈从“模块+0x偏移”升级成
//   “模块!函数+0x偏移”，能取到行号时再补上“源文件:行号”；
// - 在符号化之前先校验“磁盘上的映像是不是崩溃时加载的那一份”：
//   比对转储里记录的 PE TimeDateStamp / SizeOfImage 与磁盘文件的同名字段，
//   不一致就判定为映像不匹配，该模块的函数名与行号一律标为不可信。
//
// 为什么这道校验必须有（实战教训）：
// - 定位一次驱动蓝屏时，驱动在崩溃之后被重新编译过，磁盘上的 PDB 已经换代。
//   符号化工具照样给出了函数名和行号，只是行号整体错位——而差一行就足以
//   把排查引到完全无关的代码上。工具若不主动报“不匹配”，使用者根本没有
//   机会发现自己读的是假数据。所以本模块宁可拒绝给出行号，也不给不带
//   匹配结论的行号。
//
// 局限：
// - DbgHelp 不是线程安全的，本模块内部用进程级互斥串行化，只在解析 worker 用；
// - 不联网下载符号：搜索路径只走本地目录，避免解析卡在网络上；
// - 内核转储的模块表来自加载器链表，通常没有 CodeView 记录，因此匹配判定
//   主要依赖 TimeDateStamp/SizeOfImage 这一对，而它们已足以识别“重编译过”。
// 调用方式：
// - 解析完成后在同一个 worker 线程里调用 ApplySymbols()，就地升级 result。
// ============================================================

#include "MinidumpFormat.h"

namespace ks::minidump
{
    // SymbolMatchState 与 ModuleSymbolStatus 定义在 MinidumpFormat.h：
    // DumpParseResult 要直接持有逐模块结论，放这里会形成头文件循环依赖。

    // ResolvedSymbol：一次地址→符号的解析结果。
    struct ResolvedSymbol
    {
        bool valid = false;             // valid：是否解析出了函数名。
        QString moduleName;             // moduleName：所属模块名。
        QString functionName;           // functionName：函数名（已 undecorate）。
        std::uint64_t displacement = 0; // displacement：相对函数入口的字节偏移。
        QString sourceFile;             // sourceFile：源文件路径，可为空。
        std::uint32_t sourceLine = 0;   // sourceLine：源码行号，0 表示未取到。
        SymbolMatchState match = SymbolMatchState::NotChecked; // match：所属模块的匹配结论。

        // functionText 作用：拼出“模块!函数+0x偏移”。
        // 无函数名时返回空串；调用方应回退到已有的“模块+偏移”。
        QString functionText() const;

        // sourceText 作用：拼出“文件:行号”。
        // 没有行号信息时返回空串。映像不匹配时同样返回空串——错的行号比没有行号更有害。
        QString sourceText() const;
    };

    // SymbolResolver 作用：一次解析期间的 DbgHelp 会话，析构时自动 SymCleanup。
    // 生命周期内串行持有进程级 DbgHelp 互斥，因此不要长期保留实例。
    class SymbolResolver
    {
    public:
        SymbolResolver();
        ~SymbolResolver();

        SymbolResolver(const SymbolResolver&) = delete;
        SymbolResolver& operator=(const SymbolResolver&) = delete;

        // begin 作用：建立 DbgHelp 会话并按模块表逐个校验映像、加载符号。
        // 传入 searchPath 符号搜索路径（分号分隔）、modules 转储里的模块表；
        // 返回是否成功建立会话（失败时 resolve 一律返回无效结果）。
        bool begin(const QString& searchPath, const std::vector<ModuleEntry>& modules);

        // resolve 作用：把一个代码地址翻译成函数名与源码位置。
        // 传入 address 目标机虚拟地址；返回解析结果，未命中时 valid=false。
        ResolvedSymbol resolve(std::uint64_t address) const;

        // moduleStatus 作用：取每个模块的符号加载与匹配结论。
        // 返回内部状态表的只读引用，顺序与传入的模块表一致。
        const std::vector<ModuleSymbolStatus>& moduleStatus() const;

    private:
        // Loaded：一个已登记模块的地址区间与匹配结论。
        struct Loaded
        {
            std::uint64_t base = 0; // base：加载基址。
            std::uint64_t end = 0;  // end：结束地址（不含）。
            QString name;           // name：模块名。
            SymbolMatchState state = SymbolMatchState::NotChecked; // state：匹配结论。
        };

        void* m_handle = nullptr;                    // m_handle：SymInitialize 用的伪进程句柄。
        std::unique_lock<std::mutex> m_dbgHelpLock; // Owns serialization even before SymInitialize.
        bool m_initialized = false;                  // m_initialized：会话是否已建立。
        std::vector<Loaded> m_loaded;                // m_loaded：按 base 升序的已登记模块。
        std::vector<ModuleSymbolStatus> m_status;    // m_status：逐模块结论，供 UI 展示。
    };

    // BuildDefaultSymbolSearchPath 作用：拼出默认的本地符号搜索路径。
    // 传入 dumpFilePath 转储文件路径（用于把它所在目录也纳入搜索）；
    // 返回分号分隔的路径串，只含本地目录，不含任何符号服务器。
    QString BuildDefaultSymbolSearchPath(const QString& dumpFilePath);

    // ApplySymbols 作用：对解析结果做符号化，就地升级调用栈与肇事候选。
    // 传入 searchPath 符号搜索路径（空串则用默认路径）、result 解析结果（就地修改）；
    // 会填充 result.symbolStatus / symbolSearchPath，并在发现映像不匹配时追加
    // 一条 diagnostics 告警——这类告警必须让用户看见，否则他会拿错行号去改代码。
    void ApplySymbols(
        const QString& searchPath, DumpParseResult& result,
        const DumpCancellationToken& cancellation = {});

    // SymbolMatchStateText 作用：把匹配结论转成中文短语。
    // 传入 state；返回可直接进表格的文本。
    QString SymbolMatchStateText(SymbolMatchState state);
}
