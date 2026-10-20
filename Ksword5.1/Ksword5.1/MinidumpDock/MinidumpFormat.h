#pragma once

// ============================================================
// MinidumpFormat.h
// 作用：
// - 定义转储解析页（MinidumpDock）使用的格式中立数据模型；
// - 同一套模型同时承载用户态 MDMP minidump 与内核 PAGEDUMP/PAGEDU64 转储；
// - 提供带边界检查的只读文件视图 DumpFileView：
//   所有按 RVA/文件偏移的访问都必须经过它，防止畸形转储导致越界读取。
// 调用方式：
// - 解析入口见 MinidumpParser.h 的 ks::minidump::ParseDumpFile；
// - UI 渲染只消费本文件里的结构体，不接触原始文件字节。
// ============================================================

#include <QString>
#include <QStringList>

// CrashHistoryEntry 在这里：解析结果要顺带带上系统事件日志的崩溃时间线。
// CrashHistory.h 只依赖 Qt 与标准库，不会形成循环依赖。
#include "CrashHistory.h"
#include "DumpCancellation.h"

#include <cstdint>
#include <cstring>
#include <vector>

namespace ks::minidump
{
    // DumpKind：被解析文件的总体类别，由文件签名判定。
    enum class DumpKind
    {
        Unknown,      // Unknown：签名无法识别，不是受支持的转储文件。
        UserMinidump, // UserMinidump：用户态 MDMP 小型转储（MiniDumpWriteDump 产物）。
        KernelDump64, // KernelDump64：64 位内核转储（签名 PAGEDU64，蓝屏产物）。
        KernelDump32, // KernelDump32：32 位内核转储（签名 PAGEDUMP，旧系统蓝屏产物）。
    };

    // DumpProperty：概览/详情表里的一行“属性-值”。
    // name/value 均为中文规范文本，渲染层通过 ks::i18n::sourceText 按整串翻译。
    struct DumpProperty
    {
        QString name;  // name：属性名（中文规范文本）。
        QString value; // value：属性值文本（动态部分保持原样，状态词可翻译）。
    };

    // StreamEntry：MDMP 流目录中的一项；内核转储时用来展示 TRIAGE 数据块布局。
    struct StreamEntry
    {
        std::uint32_t type = 0; // type：流类型编号（MINIDUMP_STREAM_TYPE 数值）。
        QString typeName;       // typeName：流类型名（如 ThreadListStream）。
        std::uint64_t rva = 0;  // rva：流数据在文件内的偏移。
        std::uint64_t size = 0; // size：流数据字节数。
        QString note;           // note：中文说明（该流承载什么信息）。
    };

    // ModuleEntry：一行已加载模块（用户态模块或内核驱动）。
    struct ModuleEntry
    {
        QString name;                   // name：模块完整路径或名称。
        std::uint64_t base = 0;         // base：加载基址。
        std::uint64_t size = 0;         // size：映像大小（字节）。
        std::uint32_t checksum = 0;     // checksum：PE 头 CheckSum 字段。
        std::uint32_t timeDateStamp = 0; // timeDateStamp：PE 头时间戳（time_t）。
        QString timestampText;          // timestampText：时间戳的本地时间文本。
        QString version;                // version：文件版本（VS_FIXEDFILEINFO），可为空。
        QString pdbName;                // pdbName：CodeView 记录里的 PDB 路径，可为空。
        QString pdbGuidAge;             // pdbGuidAge：PDB GUID 与 Age 组合文本，可为空。
    };

    // AddressKind：一个数值被当作地址看待时的性质，用于给参数/寄存器加注解。
    enum class AddressKind
    {
        Unknown,      // Unknown：无法判定。
        NullPage,     // NullPage：落在 NULL 页（< 64KB），几乎必然是空指针解引用。
        UserSpace,    // UserSpace：用户态地址范围。
        KernelSpace,  // KernelSpace：内核地址范围（x64 高半区）。
        Poison,       // Poison：调试器/分配器填充的哨兵值（未初始化或释放后使用）。
    };

    // AddressNote：对某个数值的完整解读结果（模块归属 + 性质说明）。
    struct AddressNote
    {
        AddressKind kind = AddressKind::Unknown; // kind：地址性质分类。
        QString moduleName;      // moduleName：命中的模块名，未命中时为空。
        std::uint64_t moduleBase = 0; // moduleBase：命中模块的基址。
        std::uint64_t offset = 0;     // offset：地址相对模块基址的偏移。
        bool unloadedModule = false;  // unloadedModule：命中的是已卸载模块（高度可疑）。
        QString symbolText;      // symbolText："模块名+0x偏移"，未命中模块时为空。
        QString description;     // description：中文性质说明，可为空。
    };

    // StackFrameEntry：一帧“疑似调用栈”。本工程不加载 PDB，也不做 unwind，
    // 帧来自栈内存扫描，因此顺序近似、且必然含误报，UI 上必须如实标注。
    struct StackFrameEntry
    {
        std::uint32_t threadId = 0;   // threadId：所属线程 ID；内核转储用 0 表示崩溃线程。
        int index = 0;                // index：帧序号，0 为当前指令指针。
        std::uint64_t stackAddress = 0; // stackAddress：该返回地址所在的栈虚拟地址。
        std::uint64_t address = 0;    // address：返回地址数值。
        QString symbolText;           // symbolText："模块名+0x偏移"。
        QString moduleName;           // moduleName：归属模块名，可为空。
        QString functionText;         // functionText："模块!函数+0x偏移"，无符号时为空。
        QString sourceText;           // sourceText："源文件:行号"，仅在映像与 PDB 都匹配时才填。
        bool fromContext = false;     // fromContext：true 表示取自 CONTEXT 的 IP（可信帧）。
        bool unloadedModule = false;  // unloadedModule：归属到已卸载模块。
    };

    // SymbolMatchState：一个模块的符号可用性与匹配结论。
    // 单独立出来是因为“函数名可不可信”本身就是结论的一部分：转储记录的是崩溃
    // 当时那一份映像，而磁盘上的那份完全可能已经重新编译过，此时行号会整体错位。
    enum class SymbolMatchState
    {
        NotChecked,    // NotChecked：未尝试（模块缺基址/大小等）。
        ImageMissing,  // ImageMissing：磁盘上找不到该映像，无法符号化。
        ImageMismatch, // ImageMismatch：找到了但不是崩溃时那一份，已拒绝符号化。
        NoSymbols,     // NoSymbols：映像匹配但没有配套 PDB，只能给到模块+偏移。
        Matched,       // Matched：映像与 PDB 均匹配，函数名与行号可信。
    };

    // PoolTagCandidate：一个被识别出来的池标记，以及它的归属线索。
    // 池损坏类停止码里，“被损坏的块归谁所有”往往比调用栈更能指向肇事者——
    // 栈上出现的通常只是下一个来分配内存、因而撞上坏链表的“发现者”。
    struct PoolTagCandidate
    {
        std::uint32_t rawValue = 0; // rawValue：原始 32 位值。
        QString tagText;            // tagText：还原出的 4 字符标记，如 KsFi。
        QString source;             // source：来自哪个停止码参数或哪个寄存器。
        QString knownPurpose;       // knownPurpose：pooltag.txt 里的已知用途，可为空。
        QStringList ownerModules;   // ownerModules：磁盘映像里出现过该标记的模块，可为空。
    };

    // ModuleSymbolStatus：单个模块的符号加载结果，UI 上如实展示，不藏不匹配。
    struct ModuleSymbolStatus
    {
        QString moduleName;                                    // moduleName：模块名（无路径）。
        SymbolMatchState state = SymbolMatchState::NotChecked;  // state：匹配结论。
        QString imagePath;                                     // imagePath：实际使用的磁盘映像路径，可为空。
        QString pdbPath;                                       // pdbPath：实际加载的 PDB 路径，可为空。
        QString detail;                                        // detail：中文说明，含具体差异。
    };

    // RegisterEntry：崩溃点 CONTEXT 里的一个寄存器及其值解读。
    struct RegisterEntry
    {
        std::uint32_t threadId = 0; // threadId：所属线程 ID；内核转储用 0。
        QString name;               // name：寄存器名（Rip/Rsp/Rax…）。
        std::uint64_t value = 0;    // value：寄存器值。
        QString note;               // note：值的性质解读（模块归属 / poison / 空指针）。
    };

    // BlameEntry：一个“肇事模块”候选。多个证据命中同一模块会累加权重。
    struct BlameEntry
    {
        QString moduleName;        // moduleName：候选模块名。
        std::uint64_t moduleBase = 0; // moduleBase：模块基址。
        std::uint64_t address = 0;    // address：命中的代表性地址。
        std::uint64_t offset = 0;     // offset：address 相对基址的偏移。
        QString functionText;         // functionText："模块!函数+0x偏移"，无符号时为空。
        int weight = 0;               // weight：证据权重合计，越大越可疑。
        bool unloadedModule = false;  // unloadedModule：命中的是已卸载模块。
        QStringList evidence;         // evidence：支撑该候选的证据描述（中文）。
    };

    // AnalysisConfidence：诊断结论的可信度。
    enum class AnalysisConfidence
    {
        None,   // None：没有形成结论。
        Low,    // Low：仅凭停止码分类推断，无地址级证据。
        Medium, // Medium：有地址级证据但归属到系统模块或存在多个候选。
        High,   // High：崩溃指令地址直接归属到某个第三方模块。
    };

    // DumpAnalysis：转储的综合诊断结论，是“报告”与“诊断”页的数据源。
    struct DumpAnalysis
    {
        AnalysisConfidence confidence = AnalysisConfidence::None; // confidence：结论可信度。
        QString headline;             // headline：一句话结论。
        QString category;             // category：故障归类（驱动/硬件/文件系统/电源/软件…）。
        QStringList findings;         // findings：逐条发现（证据链）。
        QStringList suggestions;      // suggestions：下一步排查建议。
        std::vector<BlameEntry> blame; // blame：肇事模块候选，已按权重降序排序。
    };

    // ThreadEntry：一行线程信息（含 ThreadInfo/ThreadNames 流的补充字段）。
    struct ThreadEntry
    {
        std::uint32_t threadId = 0;      // threadId：线程 ID。
        QString name;                    // name：线程名（ThreadNamesStream），可为空。
        std::uint32_t suspendCount = 0;  // suspendCount：挂起计数。
        std::uint32_t priorityClass = 0; // priorityClass：优先级类。
        std::uint32_t priority = 0;      // priority：基础优先级。
        std::uint64_t teb = 0;           // teb：TEB 地址。
        std::uint64_t stackBase = 0;     // stackBase：转储捕获的栈内存起始地址。
        std::uint64_t stackSize = 0;     // stackSize：转储捕获的栈字节数。
        std::uint64_t instructionPointer = 0; // instructionPointer：上下文中的指令指针（Rip/Eip/Pc）。
        std::uint64_t startAddress = 0;  // startAddress：线程起始地址（ThreadInfoListStream）。
        QString cpuTimeText;             // cpuTimeText：用户态/内核态 CPU 时间文本，可为空。
        QString ipSymbolText;            // ipSymbolText：指令指针的“模块名+0x偏移”，可为空。
        QString startSymbolText;         // startSymbolText：起始地址的“模块名+0x偏移”，可为空。
        bool faulting = false;           // faulting：是否为异常流指向的崩溃线程。
    };

    // MemoryRegionEntry：一行内存区域。来源可能是 MemoryList、Memory64List
    // 或 MemoryInfoList，三者字段覆盖面不同，缺失字段留空。
    struct MemoryRegionEntry
    {
        std::uint64_t base = 0;  // base：区域起始虚拟地址。
        std::uint64_t size = 0;  // size：区域字节数。
        QString state;           // state：MEM_COMMIT/RESERVE/FREE 文本（仅 MemoryInfoList 有）。
        QString protect;         // protect：PAGE_* 保护属性文本（仅 MemoryInfoList 有）。
        QString type;            // type：MEM_IMAGE/MAPPED/PRIVATE 文本（仅 MemoryInfoList 有）。
        QString source;          // source：数据来源（中文：内存列表/64 位内存列表/内存信息列表）。
    };

    // DumpMemoryRange：一段可从原始转储文件重新读取的虚拟内存。
    // 与 memoryRegions 的区别是：后者可以只来自 MemoryInfoList（只含属性），
    // 本结构只保存确实捕获了字节、且文件偏移通过解析期边界校验的范围。
    // UI 用它在不保留整份 DMP 映射的前提下，安全地按虚拟地址重开文件读取。
    struct DumpMemoryRange
    {
        std::uint64_t virtualAddress = 0; // virtualAddress：目标机中首字节的虚拟地址。
        std::uint64_t fileOffset = 0;     // fileOffset：首字节在 DMP 内的文件偏移。
        std::uint64_t bytes = 0;          // bytes：连续且实际捕获的字节数。
        QString source;                   // source：捕获来源，如 TRIAGE 数据块或线程栈。
    };

    // DumpByteBlock：一段可在界面中预览的原始转储字节。
    // 数据仅来自已经通过文件边界校验的连续捕获范围，previewBytes 限定在安全的
    // 小窗口内；完整范围及跨页/跨块读取由后续内存查看器负责。
    struct DumpByteBlock
    {
        std::uint64_t address = 0;      // address：预览第一字节的目标机虚拟地址。
        std::uint64_t fileOffset = 0;   // fileOffset：该字节在转储文件中的偏移。
        std::uint64_t capturedBytes = 0; // capturedBytes：该捕获块完整字节数。
        QString source;                 // source：捕获来源，例如 TRIAGE 数据块。
        bool hasVirtualAddress = true; // hasVirtualAddress：false 表示文件内辅助数据，不是虚拟内存。
        std::vector<unsigned char> previewBytes; // previewBytes：受限原始预览字节。
    };

    // HandleEntry：一行句柄信息（HandleDataStream）。
    struct HandleEntry
    {
        std::uint64_t handleValue = 0;   // handleValue：句柄数值。
        QString typeName;                // typeName：对象类型名（如 File、Mutant）。
        QString objectName;              // objectName：对象名，可为空。
        std::uint32_t attributes = 0;    // attributes：句柄属性位。
        std::uint32_t grantedAccess = 0; // grantedAccess：授予的访问掩码。
        std::uint32_t handleCount = 0;   // handleCount：对象句柄计数。
        std::uint32_t pointerCount = 0;  // pointerCount：对象指针计数。
    };

    // UnloadedModuleEntry：一行已卸载模块。
    // 用户态来自 UnloadedModuleListStream，内核来自 TRIAGE 区的已卸载驱动表。
    struct UnloadedModuleEntry
    {
        QString name;                    // name：模块名。
        std::uint64_t base = 0;          // base：卸载前的加载基址。
        std::uint64_t endAddress = 0;    // endAddress：卸载前映像结束地址（内核表直接给出）。
        std::uint32_t size = 0;          // size：映像大小。
        std::uint32_t checksum = 0;      // checksum：PE CheckSum。
        std::uint32_t timeDateStamp = 0; // timeDateStamp：PE 时间戳。
        QString timestampText;           // timestampText：时间戳本地时间文本。
    };

    // DumpParseResult：一次转储解析的全部产物；UI 只消费这一个结构。
    struct DumpParseResult
    {
        bool success = false;    // success：是否解析出可展示的核心信息。
        bool recognized = false; // recognized：签名是否被识别（识别但损坏时 success=false）。
        DumpKind kind = DumpKind::Unknown; // kind：转储类别。
        QString errorText;       // errorText：失败原因（中文规范文本），成功时为空。
        QString filePath;        // filePath：被解析文件完整路径。
        std::uint64_t fileSize = 0; // fileSize：文件字节数。
        // fileLastModifiedUtcMs：解析时文件的 UTC 修改时间戳，用于内存查看器拒绝
        // 读取解析后被同路径替换的 DMP；-1 表示文件系统未提供该时间。
        std::int64_t fileLastModifiedUtcMs = -1;

        std::vector<DumpProperty> overview;      // overview：概览页“属性-值”集合。
        std::vector<DumpProperty> exceptionInfo; // exceptionInfo：异常/BugCheck 详情集合，可为空。
        // executionContext：崩溃现场的当前 CPU / KTHREAD / EPROCESS 快照信息。
        // 目前由 x64 TRIAGE_DUMP64 的 KDDEBUGGER_DATA64 动态偏移解析而来；
        // 不把未被转储捕获或无法验证的字段伪装成零值。
        std::vector<DumpProperty> executionContext;
        std::vector<StreamEntry> streams;        // streams：流目录（或内核 TRIAGE 布局）。
        std::vector<ModuleEntry> modules;        // modules：模块/驱动列表。
        std::vector<ThreadEntry> threads;        // threads：线程列表（内核转储通常为空）。
        std::vector<MemoryRegionEntry> memoryRegions; // memoryRegions：内存区域列表。
        // capturedMemoryRanges：可由内存查看器从 DMP 重读的虚拟内存范围。
        // 它不持有任何文件映射，避免异步解析结束后留下悬空文件视图。
        std::vector<DumpMemoryRange> capturedMemoryRanges;
        std::vector<DumpByteBlock> byteBlocks; // byteBlocks：可安全预览的原始内存块。
        std::vector<HandleEntry> handles;        // handles：句柄列表（仅带句柄流的用户态转储）。
        std::vector<UnloadedModuleEntry> unloadedModules; // unloadedModules：已卸载模块列表。
        std::vector<StackFrameEntry> stackFrames; // stackFrames：疑似调用栈（栈扫描产物，含误报）。
        std::vector<RegisterEntry> registers;     // registers：崩溃点寄存器快照。
        DumpAnalysis analysis;                    // analysis：综合诊断结论与肇事模块候选。
        std::vector<ModuleSymbolStatus> symbolStatus; // symbolStatus：逐模块的符号匹配结论。
        std::vector<PoolTagCandidate> poolTags;   // poolTags：识别出的池标记及其归属线索。
        std::vector<CrashHistoryEntry> crashHistory; // crashHistory：系统事件日志里的崩溃时间线。
        QString symbolSearchPath;                 // symbolSearchPath：本次实际使用的符号搜索路径。
        QStringList diagnostics;                 // diagnostics：解析过程中的非致命告警（中文文本）。

        std::uint64_t memoryRegionTotal = 0; // memoryRegionTotal：文件中内存区域总数（含未展示部分）。
        std::uint64_t memoryRegionShown = 0; // memoryRegionShown：实际填入 memoryRegions 的条数。
        std::uint32_t pointerSize = 8;       // pointerSize：目标机指针宽度（4 或 8），栈扫描步长。
        std::uint32_t bugCheckCode = 0;      // bugCheckCode：内核转储停止码，用户态转储为 0。
        std::uint64_t bugCheckParameters[4] = {}; // bugCheckParameters：停止码的四个参数。
        std::uint32_t exceptionCode = 0;     // exceptionCode：用户态异常码，内核转储可为 0。
        std::uint64_t faultingAddress = 0;   // faultingAddress：崩溃指令地址（IP），0 表示未知。
        std::uint32_t faultingThreadId = 0;  // faultingThreadId：崩溃线程 ID，用户态有效。
    };

    // DumpFileView：只读文件视图，负责所有带边界检查的字节访问。
    // 使用方式：parser 先构造 { data, size }，随后一律用 contains/at/readStruct 取数。
    struct DumpFileView
    {
        const unsigned char* data = nullptr; // data：映射后的文件首字节指针。
        std::uint64_t size = 0;              // size：文件总字节数。

        // contains 作用：判断 [offset, offset+bytes) 是否完整落在文件内。
        // 传入 offset 文件偏移与 bytes 长度；返回是否安全可读（自动防加法溢出）。
        bool contains(const std::uint64_t offset, const std::uint64_t bytes) const
        {
            CheckDumpCancellation();
            if (data == nullptr || offset > size || bytes > size)
            {
                return false;
            }
            return offset + bytes <= size;
        }

        // at 作用：取得偏移处的原始指针；越界时返回 nullptr。
        // 传入 offset 文件偏移与 bytes 需要读取的长度；返回可读指针或 nullptr。
        const unsigned char* at(const std::uint64_t offset, const std::uint64_t bytes) const
        {
            return contains(offset, bytes) ? data + offset : nullptr;
        }

        // readStruct 作用：把偏移处的字节安全拷贝进 POD 结构体。
        // 传入 offset 文件偏移与输出指针 valueOut；返回是否读取成功。
        template <typename PodType>
        bool readStruct(const std::uint64_t offset, PodType* const valueOut) const
        {
            const unsigned char* const source = at(offset, sizeof(PodType)); // source：待拷贝区域首指针。
            if (source == nullptr || valueOut == nullptr)
            {
                return false;
            }
            std::memcpy(valueOut, source, sizeof(PodType));
            return true;
        }
    };
}
