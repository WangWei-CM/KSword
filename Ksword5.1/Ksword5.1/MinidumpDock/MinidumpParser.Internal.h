#pragma once

// ============================================================
// MinidumpParser.Internal.h
// 作用：
// - MinidumpParser.cpp 与 MinidumpParser.Streams.cpp 之间的内部共享头；
// - 声明解析上限常量、通用格式化/读取辅助函数与各流解析函数；
// - 仅供 MinidumpDock 解析实现文件包含，不对外暴露。
// 调用方式：
// - 两个实现文件 include 本头；辅助函数实现在 MinidumpParser.cpp，
//   流解析函数实现在 MinidumpParser.Streams.cpp。
// ============================================================

#include "DumpMemoryReader.h"
#include "DumpStackWalker.h"
#include "DumpSymbolIndex.h"
#include "MinidumpFormat.h"

// windows.h 必须先于 minidumpapiset.h（后者依赖基础类型定义）。
#include <windows.h>
#include <minidumpapiset.h>

#include <unordered_map>

namespace ks::minidump::detail
{
    // kMaxListEntries：线程/模块/句柄等列表的解析上限，防止畸形计数拖垮 UI。
    constexpr std::uint64_t kMaxListEntries = 100000;
    // kMaxMemoryRows：内存区域表最多填充的行数，超出部分只计数不展示。
    constexpr std::uint64_t kMaxMemoryRows = 20000;
    // kMaxStringBytes：单个 MINIDUMP_STRING 允许的最大字节数（畸形长度防线）。
    constexpr std::uint32_t kMaxStringBytes = 64 * 1024;
    // kMaxCommentBytes：注释流最多读取的字节数，避免超长注释撑爆概览。
    constexpr std::uint32_t kMaxCommentBytes = 4096;

    // Hex 作用：把无符号整数格式化成 0x 大写十六进制文本。
    // 传入 value 数值；返回如 0x7FF6A1B2 的字符串。
    QString Hex(std::uint64_t value);

    // TimeTToText 作用：把 time_t 秒值转成本地时间文本。
    // 传入 secondsSince1970；返回 yyyy-MM-dd HH:mm:ss 文本，0 时返回空串。
    QString TimeTToText(std::uint64_t secondsSince1970);

    // ByteCountText 作用：把字节数格式化成“数值 + 单位”的可读文本。
    // 传入 bytes 字节数；返回如 “1.5 MB (1572864 字节)” 的文本。
    QString ByteCountText(std::uint64_t bytes);

    // ReadMinidumpString 作用：按 RVA 读取 MINIDUMP_STRING（UTF-16）。
    // 传入 view 文件视图与 rva 字符串位置；返回解码文本，非法时返回空串。
    QString ReadMinidumpString(const DumpFileView& view, std::uint64_t rva);

    // FindStream 作用：在流目录中查找指定类型的第一个流。
    // 传入 view/目录参数与目标类型；找到时返回 true 并输出位置描述。
    bool FindStream(
        const DumpFileView& view,
        std::uint64_t directoryRva,
        std::uint32_t streamCount,
        std::uint32_t streamType,
        MINIDUMP_LOCATION_DESCRIPTOR* locationOut);

    // InstructionPointerFromContext 作用：按 CPU 架构从线程 CONTEXT 里取指令指针。
    // 传入 view、上下文位置与 SYSTEM_INFO 架构编号；失败返回 0。
    std::uint64_t InstructionPointerFromContext(
        const DumpFileView& view,
        const MINIDUMP_LOCATION_DESCRIPTOR& contextLocation,
        std::uint16_t architecture);

    // ================= 流解析函数（实现见 MinidumpParser.Streams.cpp） =================

    // ParseSystemInfo 作用：解析 SystemInfoStream 并填充概览；
    // 同时输出架构编号供线程上下文解析使用。
    void ParseSystemInfo(
        const DumpFileView& view,
        const MINIDUMP_LOCATION_DESCRIPTOR& location,
        DumpParseResult& result,
        std::uint16_t* architectureOut);

    // ParseMiscInfo 作用：解析 MiscInfoStream 的全部版本（v1..v4）：
    // 进程 ID/时间、CPU 频率、完整性级别、DEP 策略、受保护进程、时区与系统构建串。
    void ParseMiscInfo(
        const DumpFileView& view,
        const MINIDUMP_LOCATION_DESCRIPTOR& location,
        DumpParseResult& result);

    // ParseProcessVmCounters 作用：解析 ProcessVmCountersStream（22），
    // 产出工作集、私有字节与页面文件用量——诊断内存耗尽类崩溃的关键数据。
    void ParseProcessVmCounters(
        const DumpFileView& view,
        const MINIDUMP_LOCATION_DESCRIPTOR& location,
        DumpParseResult& result);

    // ParseExceptionStream 作用：解析 ExceptionStream，产出异常详情与崩溃线程 ID。
    // modules 用于把异常地址翻译成“模块名+偏移”，调用前必须已建好索引；
    // contextLocationOut 输出崩溃线程的 CONTEXT 位置，供寄存器提取使用。
    void ParseExceptionStream(
        const DumpFileView& view,
        const MINIDUMP_LOCATION_DESCRIPTOR& location,
        std::uint16_t architecture,
        const ModuleIndex& modules,
        DumpParseResult& result,
        std::uint32_t* faultingThreadIdOut,
        MINIDUMP_LOCATION_DESCRIPTOR* contextLocationOut);

    // ParseThreadList 作用：解析 ThreadListStream/ThreadExListStream 为线程数组。
    // modules 用于标注线程指令指针的模块归属；
    // scanInputsOut 收集每个线程的栈内存位置，供后续调用栈重建使用。
    void ParseThreadList(
        const DumpFileView& view,
        const MINIDUMP_LOCATION_DESCRIPTOR& location,
        bool isExtendedList,
        std::uint16_t architecture,
        std::uint32_t faultingThreadId,
        const ModuleIndex& modules,
        DumpParseResult& result,
        std::vector<StackScanInput>* scanInputsOut,
        DumpMemoryReader* memory);

    // 补充流共用稳定索引；重复 ThreadId 保留原先的首个匹配语义。
    using ThreadIdIndex = std::unordered_map<std::uint32_t, std::size_t>;
    ThreadIdIndex BuildThreadIdIndex(const std::vector<ThreadEntry>& threads);

    // ParseThreadInfoList 作用：把 ThreadInfoListStream 的起始地址/CPU 时间合入线程表。
    void ParseThreadInfoList(
        const DumpFileView& view,
        const MINIDUMP_LOCATION_DESCRIPTOR& location,
        DumpParseResult& result,
        const ThreadIdIndex& threadIndex);

    // ParseThreadNames 作用：把 ThreadNamesStream 的线程名合入线程表。
    void ParseThreadNames(
        const DumpFileView& view,
        const MINIDUMP_LOCATION_DESCRIPTOR& location,
        DumpParseResult& result,
        const ThreadIdIndex& threadIndex);

    // ParseModuleList 作用：解析 ModuleListStream 为模块数组（含版本与 PDB）。
    void ParseModuleList(
        const DumpFileView& view,
        const MINIDUMP_LOCATION_DESCRIPTOR& location,
        DumpParseResult& result);

    // ParseUnloadedModuleList 作用：解析 UnloadedModuleListStream。
    void ParseUnloadedModuleList(
        const DumpFileView& view,
        const MINIDUMP_LOCATION_DESCRIPTOR& location,
        DumpParseResult& result);

    // ParseMemoryLists 作用：解析内存区域并建立虚拟地址索引。
    // 展示用的区域表按信息量优先取 MemoryInfoList，其次 Memory64List/MemoryList；
    // 而 memory 索引只能来自后两者——MemoryInfoList 只描述区域属性、不含数据，
    // 因此两条路径必须分开走，不能在取到 MemoryInfoList 后就提前返回。
    void ParseMemoryLists(
        const DumpFileView& view,
        std::uint64_t directoryRva,
        std::uint32_t streamCount,
        DumpParseResult& result,
        DumpMemoryReader* memory);

    // ParseHandleData 作用：解析 HandleDataStream 为句柄数组。
    void ParseHandleData(
        const DumpFileView& view,
        const MINIDUMP_LOCATION_DESCRIPTOR& location,
        DumpParseResult& result);

    // ParseComments 作用：读取 ANSI/Unicode 注释流，追加进概览。
    void ParseComments(
        const DumpFileView& view,
        std::uint64_t directoryRva,
        std::uint32_t streamCount,
        DumpParseResult& result);

    // DumpTypeFlagText 作用：把 MINIDUMP_TYPE 位标志拆成常量名列表文本。
    QString DumpTypeFlagText(std::uint64_t flags);
}
