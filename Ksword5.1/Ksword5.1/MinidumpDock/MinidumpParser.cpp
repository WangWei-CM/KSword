// ============================================================
// MinidumpParser.cpp
// 作用：
// - 实现转储解析总入口 ParseDumpFile：映射文件、判别签名并分发；
// - 实现 MDMP 用户态转储的主解析流程（头、流目录与概览组装），
//   各具体流的解析实现位于 MinidumpParser.Streams.cpp；
// - 实现两文件共享的通用辅助函数（十六进制/时间格式化、
//   MINIDUMP_STRING 读取、流目录查找、上下文取指令指针）；
// - 所有文件访问都经 DumpFileView 边界检查，畸形样本只会得到
//   诊断信息而不会越界。
// 结构体来源：
// - MDMP 相关结构一律使用 SDK 官方定义（minidumpapiset.h），
//   避免手写布局出错。
// ============================================================

#include "MinidumpParser.h"

#include "DumpAnalyzer.h"
#include "DumpContextRegisters.h"
#include "KernelDumpParser.h"
#include "MinidumpCodeText.h"
#include "MinidumpParser.Internal.h"

#include <QDateTime>
#include <QFile>
#include <QFileInfo>

#include <algorithm>

namespace ks::minidump
{
    namespace detail
    {
        QString Hex(const std::uint64_t value)
        {
            return QStringLiteral("0x%1").arg(QString::number(value, 16).toUpper());
        }

        QString TimeTToText(const std::uint64_t secondsSince1970)
        {
            if (secondsSince1970 == 0)
            {
                return QString();
            }
            return QDateTime::fromSecsSinceEpoch(
                static_cast<qint64>(secondsSince1970))
                .toString(QStringLiteral("yyyy-MM-dd HH:mm:ss"));
        }

        QString ByteCountText(const std::uint64_t bytes)
        {
            // units：从字节到 TB 的单位表；index 为最终选中的单位下标。
            const char* const units[] = { "B", "KB", "MB", "GB", "TB" };
            double scaled = static_cast<double>(bytes);
            int index = 0;
            while (scaled >= 1024.0 && index < 4)
            {
                scaled /= 1024.0;
                ++index;
            }
            if (index == 0)
            {
                return QStringLiteral("%1 B").arg(bytes);
            }
            return QStringLiteral("%1 %2 (%3 字节)")
                .arg(QString::number(scaled, 'f', 1))
                .arg(QString::fromLatin1(units[index]))
                .arg(bytes);
        }

        QString ReadMinidumpString(const DumpFileView& view, const std::uint64_t rva)
        {
            if (rva == 0)
            {
                return QString();
            }
            // lengthBytes：字符串字节数（不含终止符），先读长度再校验数据区。
            ULONG32 lengthBytes = 0;
            if (!view.readStruct(rva, &lengthBytes) || lengthBytes > kMaxStringBytes)
            {
                return QString();
            }
            const unsigned char* const textData = view.at(rva + sizeof(ULONG32), lengthBytes);
            if (textData == nullptr)
            {
                return QString();
            }
            return QString::fromUtf16(
                reinterpret_cast<const char16_t*>(textData),
                static_cast<qsizetype>(lengthBytes / sizeof(char16_t)));
        }

        bool FindStream(
            const DumpFileView& view,
            const std::uint64_t directoryRva,
            const std::uint32_t streamCount,
            const std::uint32_t streamType,
            MINIDUMP_LOCATION_DESCRIPTOR* const locationOut)
        {
            for (std::uint32_t index = 0; index < streamCount; ++index)
            {
                // entry：第 index 个目录项；目录本身也要边界校验。
                MINIDUMP_DIRECTORY entry{};
                if (!view.readStruct(directoryRva + static_cast<std::uint64_t>(index) * sizeof(MINIDUMP_DIRECTORY), &entry))
                {
                    return false;
                }
                if (entry.StreamType == streamType && entry.Location.Rva != 0)
                {
                    *locationOut = entry.Location;
                    return true;
                }
            }
            return false;
        }

        // 指令指针偏移依据各架构官方 CONTEXT 布局：
        // x64 Rip=0xF8，x86 Eip=0xB8，ARM64 Pc=0x108。
        std::uint64_t InstructionPointerFromContext(
            const DumpFileView& view,
            const MINIDUMP_LOCATION_DESCRIPTOR& contextLocation,
            const std::uint16_t architecture)
        {
            if (contextLocation.Rva == 0)
            {
                return 0;
            }
            // ipOffset/ipSize：目标架构指令指针在 CONTEXT 内的偏移与宽度。
            std::uint64_t ipOffset = 0;
            std::uint32_t ipSize = 0;
            switch (architecture)
            {
            case PROCESSOR_ARCHITECTURE_AMD64:
                ipOffset = 0xF8;
                ipSize = 8;
                break;
            case PROCESSOR_ARCHITECTURE_INTEL:
                ipOffset = 0xB8;
                ipSize = 4;
                break;
            case PROCESSOR_ARCHITECTURE_ARM64:
                ipOffset = 0x108;
                ipSize = 8;
                break;
            default:
                return 0;
            }
            if (contextLocation.DataSize < ipOffset + ipSize)
            {
                return 0;
            }
            if (ipSize == 8)
            {
                std::uint64_t value = 0;
                return view.readStruct(contextLocation.Rva + ipOffset, &value) ? value : 0;
            }
            std::uint32_t value32 = 0;
            return view.readStruct(contextLocation.Rva + ipOffset, &value32) ? value32 : 0;
        }
    }

    namespace
    {
        using namespace detail;

        // kMaxFaultingThreadFrames：崩溃线程的调用栈最多重建多少帧。
        constexpr int kMaxFaultingThreadFrames = 64;
        // kMaxOtherThreadFrames：其余线程每个最多重建多少帧。
        constexpr int kMaxOtherThreadFrames = 24;
        // kMaxScannedThreads：最多对多少个线程做栈扫描，避免线程数很多时刷屏。
        constexpr std::size_t kMaxScannedThreads = 32;

        // ParseUserMinidump 作用：MDMP 用户态转储的主解析流程。
        // 传入 view 文件视图与 result 输出；按流依次解析并组装概览。
        // 流的解析顺序不是随意的：模块必须最先解析，因为异常地址、线程指令指针、
        // 调用栈与归因全都依赖模块地址索引；内存索引必须在所有内存来源
        //（内存流 + 线程栈）都登记完之后才能 finalize。
        void ParseUserMinidump(const DumpFileView& view, DumpParseResult& result)
        {
            result.kind = DumpKind::UserMinidump;
            result.recognized = true;

            MINIDUMP_HEADER header{};
            if (!view.readStruct(0, &header))
            {
                result.errorText = QStringLiteral("文件过小，无法读取 MDMP 头。");
                return;
            }
            // 目录必须完整落在文件内，否则视为截断的损坏转储。
            const std::uint64_t directoryBytes =
                static_cast<std::uint64_t>(header.NumberOfStreams) * sizeof(MINIDUMP_DIRECTORY);
            if (!view.contains(header.StreamDirectoryRva, directoryBytes))
            {
                result.errorText = QStringLiteral("流目录越界，转储文件已截断或损坏。");
                return;
            }

            // 概览首先给出文件级信息：类别、版本、流数、写入时间与类型标志。
            result.overview.push_back({ QStringLiteral("转储类别"),
                QStringLiteral("用户态 MDMP 小型转储") });
            result.overview.push_back({ QStringLiteral("文件大小"),
                ByteCountText(result.fileSize) });
            result.overview.push_back({ QStringLiteral("格式版本"),
                QString::number(header.Version & 0xFFFFu) });
            result.overview.push_back({ QStringLiteral("流数量"),
                QString::number(header.NumberOfStreams) });
            result.overview.push_back({ QStringLiteral("写入时间"),
                TimeTToText(header.TimeDateStamp) });
            result.overview.push_back({ QStringLiteral("转储类型标志"),
                DumpTypeFlagText(header.Flags) });

            // 流目录页：列出全部目录项（含未知类型）。
            // 必须和其它列表一样套解析上限：NumberOfStreams 直接来自文件，
            // 上面的 contains 只把它限到 fileSize/12，而每个 StreamEntry 带两个
            // QString（约 72 字节），放大约 6 倍文件体积。解析跑在线程池 worker 里
            // 且没有 try/catch，std::bad_alloc 逃逸会直接 std::terminate 掉整个进程，
            // 不是“解析失败”而是崩溃。
            const std::uint64_t safeStreamCount =
                std::min<std::uint64_t>(header.NumberOfStreams, kMaxListEntries);
            if (safeStreamCount != header.NumberOfStreams)
            {
                result.diagnostics.append(
                    QStringLiteral("流数量 %1 超过解析上限，仅展示前 %2 条。")
                        .arg(header.NumberOfStreams)
                        .arg(safeStreamCount));
            }
            result.streams.reserve(static_cast<std::size_t>(safeStreamCount));
            for (std::uint64_t index = 0; index < safeStreamCount; ++index)
            {
                MINIDUMP_DIRECTORY entry{};
                if (!view.readStruct(
                        header.StreamDirectoryRva +
                            static_cast<std::uint64_t>(index) * sizeof(MINIDUMP_DIRECTORY),
                        &entry))
                {
                    break;
                }
                StreamEntry streamEntry{};
                streamEntry.type = entry.StreamType;
                streamEntry.typeName = StreamTypeName(entry.StreamType);
                if (streamEntry.typeName.isEmpty())
                {
                    streamEntry.typeName = QStringLiteral("未知流 (%1)").arg(entry.StreamType);
                }
                streamEntry.rva = entry.Location.Rva;
                streamEntry.size = entry.Location.DataSize;
                streamEntry.note = StreamTypeNote(entry.StreamType);
                // 数据区越界的流仍然列出，但追加告警方便识别截断样本。
                if (entry.Location.DataSize != 0 &&
                    !view.contains(entry.Location.Rva, entry.Location.DataSize))
                {
                    streamEntry.note = QStringLiteral("数据区越界（文件截断） ") + streamEntry.note;
                    result.diagnostics.append(
                        QStringLiteral("流 %1 的数据区越界，相关内容可能缺失。")
                            .arg(streamEntry.typeName));
                }
                result.streams.push_back(std::move(streamEntry));
            }

            // architecture：线程上下文与异常上下文解析需要的 CPU 架构。
            std::uint16_t architecture = 0xFFFF;
            MINIDUMP_LOCATION_DESCRIPTOR location{};
            if (FindStream(view, header.StreamDirectoryRva, header.NumberOfStreams,
                    SystemInfoStream, &location))
            {
                ParseSystemInfo(view, location, result, &architecture);
            }
            // contextArch/pointerSize：后续 CONTEXT 提取与栈扫描的步长依据。
            const ContextArch contextArch = ContextArchFromProcessorArchitecture(architecture);
            result.pointerSize = ContextPointerSize(contextArch);
            if (FindStream(view, header.StreamDirectoryRva, header.NumberOfStreams,
                    MiscInfoStream, &location))
            {
                ParseMiscInfo(view, location, result);
            }
            if (FindStream(view, header.StreamDirectoryRva, header.NumberOfStreams,
                    ProcessVmCountersStream, &location))
            {
                ParseProcessVmCounters(view, location, result);
            }

            // ---------- 模块最先解析：后面每一步都要用模块地址索引 ----------
            if (FindStream(view, header.StreamDirectoryRva, header.NumberOfStreams,
                    ModuleListStream, &location))
            {
                ParseModuleList(view, location, result);
            }
            if (FindStream(view, header.StreamDirectoryRva, header.NumberOfStreams,
                    UnloadedModuleListStream, &location))
            {
                ParseUnloadedModuleList(view, location, result);
            }
            // modules：地址 → 模块的区间索引。
            ModuleIndex modules;
            modules.build(result.modules, result.unloadedModules, result.pointerSize);

            // ---------- 内存：先登记全部来源，最后统一 finalize ----------
            // memory：已捕获内存的虚拟地址索引，栈扫描的 call 指令校验依赖它。
            DumpMemoryReader memory;
            ParseMemoryLists(
                view, header.StreamDirectoryRva, header.NumberOfStreams, result, &memory);

            // faultingThreadId：异常流指向的崩溃线程，用于线程表标记与栈扫描优先级。
            std::uint32_t faultingThreadId = 0xFFFFFFFFu;
            // faultingContext：崩溃线程的 CONTEXT 位置，寄存器快照的来源。
            MINIDUMP_LOCATION_DESCRIPTOR faultingContext{};
            if (FindStream(view, header.StreamDirectoryRva, header.NumberOfStreams,
                    ExceptionStream, &location))
            {
                ParseExceptionStream(
                    view, location, architecture, modules, result,
                    &faultingThreadId, &faultingContext);
            }

            // 线程：普通列表优先，没有时尝试扩展列表；随后合并补充流。
            // scanInputs：每个线程的栈内存位置，稍后统一做调用栈重建。
            std::vector<StackScanInput> scanInputs;
            if (FindStream(view, header.StreamDirectoryRva, header.NumberOfStreams,
                    ThreadListStream, &location))
            {
                ParseThreadList(view, location, false, architecture, faultingThreadId,
                    modules, result, &scanInputs, &memory);
            }
            else if (FindStream(view, header.StreamDirectoryRva, header.NumberOfStreams,
                    ThreadExListStream, &location))
            {
                ParseThreadList(view, location, true, architecture, faultingThreadId,
                    modules, result, &scanInputs, &memory);
            }
            const ThreadIdIndex threadIndex = BuildThreadIdIndex(result.threads);
            if (FindStream(view, header.StreamDirectoryRva, header.NumberOfStreams,
                    ThreadInfoListStream, &location))
            {
                ParseThreadInfoList(view, location, result, threadIndex);
            }
            if (FindStream(view, header.StreamDirectoryRva, header.NumberOfStreams,
                    static_cast<std::uint32_t>(24) /* ThreadNamesStream */, &location))
            {
                ParseThreadNames(view, location, result, threadIndex);
            }
            // 线程栈全部登记完毕，此时才能定型内存索引。
            memory.finalize(view);
            // 解析期内存索引在函数返回后会销毁；把已校验的轻量映射随结果带回 UI，
            // 让查看器可以稍后只读重开 DMP，而不把整个文件常驻内存。
            memory.appendCapturedRanges(&result.capturedMemoryRanges);

            // 起始地址的模块归属要等 ThreadInfoList 合入之后才能算。
            for (ThreadEntry& thread : result.threads)
            {
                CheckDumpCancellation();
                if (thread.startAddress != 0)
                {
                    thread.startSymbolText = modules.symbolText(thread.startAddress);
                }
            }

            // ---------- 崩溃点寄存器 ----------
            // 只在有异常记录时给寄存器：没有异常的快照转储不存在“崩溃点”，
            // 拿任意线程的寄存器展示反而会误导。
            if (faultingContext.Rva != 0)
            {
                result.registers = ReadContextRegisters(
                    view,
                    faultingContext.Rva,
                    faultingContext.DataSize,
                    contextArch,
                    faultingThreadId,
                    modules);
            }

            // ---------- 调用栈重建 ----------
            // 崩溃线程排在最前并给更多帧；其余线程按上限截断。
            std::stable_sort(
                scanInputs.begin(),
                scanInputs.end(),
                [faultingThreadId](const StackScanInput& left, const StackScanInput& right)
                {
                    const bool leftFaulting = left.threadId == faultingThreadId;
                    const bool rightFaulting = right.threadId == faultingThreadId;
                    return leftFaulting && !rightFaulting;
                });
            const std::size_t scanCount =
                std::min<std::size_t>(scanInputs.size(), kMaxScannedThreads);
            for (std::size_t index = 0; index < scanCount; ++index)
            {
                const StackScanInput& input = scanInputs[index];
                const int frameLimit = input.threadId == faultingThreadId
                    ? kMaxFaultingThreadFrames
                    : kMaxOtherThreadFrames;
                const std::vector<StackFrameEntry> frames =
                    ScanStackFrames(view, input, modules, memory, frameLimit);
                result.stackFrames.insert(
                    result.stackFrames.end(), frames.begin(), frames.end());
            }
            if (scanInputs.size() > scanCount)
            {
                result.diagnostics.append(
                    QStringLiteral("线程数 %1 较多，只对前 %2 个线程重建了调用栈。")
                        .arg(scanInputs.size())
                        .arg(scanCount));
            }

            if (FindStream(view, header.StreamDirectoryRva, header.NumberOfStreams,
                    HandleDataStream, &location))
            {
                ParseHandleData(view, location, result);
            }
            ParseComments(view, header.StreamDirectoryRva, header.NumberOfStreams, result);

            // 概览补充统计行，让用户不点子页也能了解规模。
            result.overview.push_back({ QStringLiteral("线程数"),
                QString::number(result.threads.size()) });
            result.overview.push_back({ QStringLiteral("模块数"),
                QString::number(result.modules.size()) });
            if (result.memoryRegionTotal != 0)
            {
                result.overview.push_back({ QStringLiteral("内存区域数"),
                    QString::number(result.memoryRegionTotal) });
            }
            if (!result.handles.empty())
            {
                result.overview.push_back({ QStringLiteral("句柄数"),
                    QString::number(result.handles.size()) });
            }
            if (memory.rangeCount() != 0)
            {
                result.overview.push_back({ QStringLiteral("已捕获内存块数"),
                    QString::number(memory.rangeCount()) });
            }

            // ---------- 归因结论 ----------
            BuildAnalysis(modules, result);
            result.success = true;
        }
    }

    DumpParseResult ParseDumpFile(
        const QString& filePath, const DumpCancellationToken& cancellation)
    {
        const ScopedDumpCancellation cancellationScope(cancellation);
        CheckDumpCancellation();
        // result：唯一返回值；先记录文件路径与大小，后续逐步填充。
        DumpParseResult result{};
        result.filePath = filePath;

        QFile dumpFile(filePath);
        const QFileInfo fileInfo(filePath);
        if (!fileInfo.exists() || !fileInfo.isFile())
        {
            result.errorText = QStringLiteral("文件不存在或不是普通文件。");
            return result;
        }
        if (!dumpFile.open(QIODevice::ReadOnly))
        {
            result.errorText = QStringLiteral("无法打开文件（可能被占用或权限不足）：%1")
                .arg(dumpFile.errorString());
            return result;
        }
        result.fileSize = static_cast<std::uint64_t>(dumpFile.size());
        result.fileLastModifiedUtcMs = fileInfo.lastModified().toUTC().toMSecsSinceEpoch();
        if (result.fileSize < 8)
        {
            result.errorText = QStringLiteral("文件过小，不可能是有效转储。");
            return result;
        }

        // mapped：整文件只读映射；解析期间 QFile 保持打开，返回前自动解除映射。
        uchar* const mapped = dumpFile.map(0, static_cast<qint64>(result.fileSize));
        if (mapped == nullptr)
        {
            result.errorText = QStringLiteral("文件映射失败（文件过大或系统资源不足）。");
            return result;
        }
        // Unmap exactly once even when a cooperative checkpoint or allocation throws.
        const auto unmap = [&dumpFile](uchar* address) { dumpFile.unmap(address); };
        const std::unique_ptr<uchar, decltype(unmap)> mapping(mapped, unmap);
        CheckDumpCancellation();
        DumpFileView view{};
        view.data = mapped;
        view.size = result.fileSize;

        // signature/validDump：按前 8 字节判别格式。
        std::uint32_t signature = 0;
        std::uint32_t validDump = 0;
        view.readStruct(0, &signature);
        view.readStruct(4, &validDump);
        if (signature == 0x504D444Du) // 'MDMP'
        {
            ParseUserMinidump(view, result);
        }
        else if (signature == 0x45474150u && validDump == 0x34365544u) // 'PAGE' + 'DU64'
        {
            ParseKernelDump64(view, result);
        }
        else if (signature == 0x45474150u && validDump == 0x504D5544u) // 'PAGE' + 'DUMP'
        {
            ParseKernelDump32(view, result);
        }
        else
        {
            result.errorText = QStringLiteral(
                "无法识别的文件签名 %1：既不是 MDMP 用户态转储，也不是 PAGEDUMP/PAGEDU64 内核转储。")
                .arg(detail::Hex(signature));
        }
        CheckDumpCancellation();
        return result;
    }
}
