// ============================================================
// MinidumpParser.Streams.cpp
// 作用：
// - 实现 MDMP 用户态转储各具体流的解析：
//   系统信息、杂项信息、异常、线程（含补充流）、模块、已卸载模块、
//   内存（三种来源）、句柄与注释流，以及转储类型标志文本；
// - 与 MinidumpParser.cpp 通过 MinidumpParser.Internal.h 共享
//   辅助函数与解析上限常量；
// - 所有偏移访问经 DumpFileView 边界检查，畸形样本安全降级。
// ============================================================

#include "MinidumpParser.Internal.h"

#include "DumpContextRegisters.h"
#include "MinidumpCodeText.h"

#include <QDateTime>

#include <algorithm>

namespace ks::minidump::detail
{
    namespace
    {
        // MinidumpThreadNameHeader/Entry：ThreadNamesStream(24) 的本地定义。
        // 旧 SDK 可能没有 MINIDUMP_THREAD_NAME_LIST，这里按官方布局 pack(4) 声明。
#pragma pack(push, 4)
        struct MinidumpThreadNameHeader
        {
            ULONG32 NumberOfThreadNames; // NumberOfThreadNames：线程名条目数。
        };
        struct MinidumpThreadNameEntry
        {
            ULONG32 ThreadId;        // ThreadId：线程 ID。
            ULONG64 RvaOfThreadName; // RvaOfThreadName：MINIDUMP_STRING 的 64 位 RVA。
        };
#pragma pack(pop)
        static_assert(sizeof(MinidumpThreadNameEntry) == 12, "线程名条目必须是 pack(4) 的 12 字节");

#pragma pack(push, 4)
        // MinidumpProcessVmCounters1：ProcessVmCountersStream(22) 的 Revision 1 布局。
        // 旧 SDK 未必有 MINIDUMP_PROCESS_VM_COUNTERS_1，按官方 pack(4) 布局本地声明。
        struct MinidumpProcessVmCounters1
        {
            std::uint16_t Revision;                  // Revision：结构版本，1 为基础版。
            std::uint32_t PageFaultCount;            // PageFaultCount：进程累计页错误次数。
            std::uint64_t PeakWorkingSetSize;        // PeakWorkingSetSize：工作集峰值。
            std::uint64_t WorkingSetSize;            // WorkingSetSize：崩溃时的工作集。
            std::uint64_t QuotaPeakPagedPoolUsage;   // QuotaPeakPagedPoolUsage：分页池用量峰值。
            std::uint64_t QuotaPagedPoolUsage;       // QuotaPagedPoolUsage：当前分页池用量。
            std::uint64_t QuotaPeakNonPagedPoolUsage; // QuotaPeakNonPagedPoolUsage：非分页池峰值。
            std::uint64_t QuotaNonPagedPoolUsage;    // QuotaNonPagedPoolUsage：当前非分页池用量。
            std::uint64_t PagefileUsage;             // PagefileUsage：当前提交量。
            std::uint64_t PeakPagefileUsage;         // PeakPagefileUsage：提交量峰值。
            std::uint64_t PrivateUsage;              // PrivateUsage：私有字节数。
        };
        static_assert(sizeof(MinidumpProcessVmCounters1) == 80,
            "MINIDUMP_PROCESS_VM_COUNTERS_1 必须是 pack(4) 的 80 字节");

        // MinidumpProcessVmCounters2：Revision 2 布局。
        // 关键差异：PeakPagefileUsage 之后**插入了** PeakVirtualSize / VirtualSize，
        // PrivateUsage 因此整体后移了 16 字节。按 Revision 1 的布局去读 Revision 2 的流，
        // 会把“虚拟地址空间峰值”当成“私有字节”报出来——对 Chromium/WebView2 这类
        // 预留海量地址空间的进程，会显示成上百 GB 的假私有内存。
        struct MinidumpProcessVmCounters2
        {
            std::uint16_t Revision;                  // Revision：结构版本，2 及以上用本布局。
            std::uint16_t Flags;                     // Flags：哪些字段组有效（MINIDUMP_PROCESS_VM_COUNTERS_*）。
            std::uint32_t PageFaultCount;            // PageFaultCount：进程累计页错误次数。
            std::uint64_t PeakWorkingSetSize;        // PeakWorkingSetSize：工作集峰值。
            std::uint64_t WorkingSetSize;            // WorkingSetSize：崩溃时的工作集。
            std::uint64_t QuotaPeakPagedPoolUsage;   // QuotaPeakPagedPoolUsage：分页池用量峰值。
            std::uint64_t QuotaPagedPoolUsage;       // QuotaPagedPoolUsage：当前分页池用量。
            std::uint64_t QuotaPeakNonPagedPoolUsage; // QuotaPeakNonPagedPoolUsage：非分页池峰值。
            std::uint64_t QuotaNonPagedPoolUsage;    // QuotaNonPagedPoolUsage：当前非分页池用量。
            std::uint64_t PagefileUsage;             // PagefileUsage：当前提交量。
            std::uint64_t PeakPagefileUsage;         // PeakPagefileUsage：提交量峰值。
            std::uint64_t PeakVirtualSize;           // PeakVirtualSize：虚拟地址空间峰值（VIRTUALSIZE 组）。
            std::uint64_t VirtualSize;               // VirtualSize：当前虚拟地址空间大小（VIRTUALSIZE 组）。
            std::uint64_t PrivateUsage;              // PrivateUsage：私有提交量（EX 组）。
            std::uint64_t PrivateWorkingSetSize;     // PrivateWorkingSetSize：私有工作集（EX2 组）。
            std::uint64_t SharedCommitUsage;         // SharedCommitUsage：共享提交量（EX2 组）。
        };
        static_assert(sizeof(MinidumpProcessVmCounters2) == 112,
            "MINIDUMP_PROCESS_VM_COUNTERS_2 前 112 字节必须与官方布局一致");
        static_assert(offsetof(MinidumpProcessVmCounters2, PrivateUsage) == 88,
            "Revision 2 的 PrivateUsage 必须位于 0x58，比 Revision 1 后移 16 字节");
#pragma pack(pop)

        // kVmCountersVirtualSize/kVmCountersEx/kVmCountersEx2：
        // Revision 2 的 Flags 位，说明对应字段组是否真的被填过。
        constexpr std::uint16_t kVmCountersVirtualSize = 0x0002;
        constexpr std::uint16_t kVmCountersEx = 0x0004;
        constexpr std::uint16_t kVmCountersEx2 = 0x0008;

        // GuidToText 作用：把 GUID 格式化为大括号十六进制文本。
        // 传入 guid；返回 {XXXXXXXX-XXXX-XXXX-XXXX-XXXXXXXXXXXX} 格式文本。
        QString GuidToText(const GUID& guid)
        {
            return QStringLiteral("{%1-%2-%3-%4%5-%6%7%8%9%10%11}")
                .arg(guid.Data1, 8, 16, QLatin1Char('0'))
                .arg(guid.Data2, 4, 16, QLatin1Char('0'))
                .arg(guid.Data3, 4, 16, QLatin1Char('0'))
                .arg(guid.Data4[0], 2, 16, QLatin1Char('0'))
                .arg(guid.Data4[1], 2, 16, QLatin1Char('0'))
                .arg(guid.Data4[2], 2, 16, QLatin1Char('0'))
                .arg(guid.Data4[3], 2, 16, QLatin1Char('0'))
                .arg(guid.Data4[4], 2, 16, QLatin1Char('0'))
                .arg(guid.Data4[5], 2, 16, QLatin1Char('0'))
                .arg(guid.Data4[6], 2, 16, QLatin1Char('0'))
                .arg(guid.Data4[7], 2, 16, QLatin1Char('0'))
                .toUpper();
        }

        // ReadCodeViewRecord 作用：解析模块的 CodeView 调试记录（RSDS/NB10）。
        // 传入 view 与记录位置；输出 PDB 路径与 GUID/Age 文本，解析不了则都留空。
        void ReadCodeViewRecord(
            const DumpFileView& view,
            const MINIDUMP_LOCATION_DESCRIPTOR& location,
            QString* const pdbNameOut,
            QString* const pdbGuidAgeOut)
        {
            // kRsdsSignature/kNb10Signature：两种 CodeView 记录的头部魔数。
            constexpr std::uint32_t kRsdsSignature = 0x53445352; // 'RSDS'
            constexpr std::uint32_t kNb10Signature = 0x3031424E; // 'NB10'
            std::uint32_t signature = 0;
            if (location.DataSize < sizeof(std::uint32_t) ||
                !view.readStruct(location.Rva, &signature))
            {
                return;
            }
            if (signature == kRsdsSignature && location.DataSize >= 24)
            {
                // RSDS 布局：DWORD 魔数 + GUID(16) + DWORD Age + UTF-8 路径。
                GUID pdbGuid{};
                std::uint32_t age = 0;
                if (!view.readStruct(location.Rva + 4, &pdbGuid) ||
                    !view.readStruct(location.Rva + 20, &age))
                {
                    return;
                }
                *pdbGuidAgeOut = QStringLiteral("%1 / %2").arg(GuidToText(pdbGuid)).arg(age);
                // nameBytes：路径区最大长度；按 NUL 截断成 UTF-8 文本。
                const std::uint64_t nameBytes = location.DataSize - 24;
                const unsigned char* const nameData = view.at(location.Rva + 24, nameBytes);
                if (nameData != nullptr && nameBytes > 0)
                {
                    std::uint64_t nameLength = 0;
                    while (nameLength < nameBytes && nameData[nameLength] != 0)
                    {
                        ++nameLength;
                    }
                    *pdbNameOut = QString::fromUtf8(
                        reinterpret_cast<const char*>(nameData),
                        static_cast<qsizetype>(nameLength));
                }
            }
            else if (signature == kNb10Signature && location.DataSize >= 16)
            {
                // NB10 布局：魔数 + Offset + 时间戳签名 + Age + ANSI 路径。
                std::uint32_t timeSignature = 0;
                std::uint32_t age = 0;
                if (!view.readStruct(location.Rva + 8, &timeSignature) ||
                    !view.readStruct(location.Rva + 12, &age))
                {
                    return;
                }
                *pdbGuidAgeOut = QStringLiteral("NB10 %1 / %2").arg(Hex(timeSignature)).arg(age);
                const std::uint64_t nameBytes = location.DataSize - 16;
                const unsigned char* const nameData = view.at(location.Rva + 16, nameBytes);
                if (nameData != nullptr && nameBytes > 0)
                {
                    std::uint64_t nameLength = 0;
                    while (nameLength < nameBytes && nameData[nameLength] != 0)
                    {
                        ++nameLength;
                    }
                    *pdbNameOut = QString::fromLocal8Bit(
                        reinterpret_cast<const char*>(nameData),
                        static_cast<qsizetype>(nameLength));
                }
            }
        }

        // ReadFixedWideString 作用：从定长 UTF-16 数组里取出以 NUL 截断的文本。
        // 传入 buffer 数组首指针与 maxChars 数组容量；返回去除首尾空白的文本。
        QString ReadFixedWideString(const wchar_t* const buffer, const std::size_t maxChars)
        {
            if (buffer == nullptr || maxChars == 0)
            {
                return QString();
            }
            std::size_t length = 0;
            while (length < maxChars && buffer[length] != L'\0')
            {
                ++length;
            }
            return QString::fromWCharArray(buffer, static_cast<qsizetype>(length)).trimmed();
        }

        // IntegrityLevelText 作用：把强制完整性级别 RID 转成中文说明。
        // 传入 level（SECURITY_MANDATORY_*_RID）；返回“级别名（数值）”文本。
        QString IntegrityLevelText(const std::uint32_t level)
        {
            // 级别名沿用 Windows 强制完整性控制的标准分档。
            const char* name = nullptr;
            if (level >= 0x5000u)       { name = "受保护进程"; }
            else if (level >= 0x4000u)  { name = "系统 (System)"; }
            else if (level >= 0x3000u)  { name = "高 (High，已提权)"; }
            else if (level >= 0x2000u)  { name = "中 (Medium，普通用户进程)"; }
            else if (level >= 0x1000u)  { name = "低 (Low，沙箱进程)"; }
            else                        { name = "不可信 (Untrusted)"; }
            return QStringLiteral("%1（0x%2）")
                .arg(QString::fromUtf8(name))
                .arg(QString::number(level, 16).toUpper());
        }

        // ExecuteFlagsText 作用：解释进程的 DEP（数据执行保护）策略位。
        // 传入 flags（MEM_EXECUTE_OPTION_*）；返回中文说明。
        QString ExecuteFlagsText(const std::uint32_t flags)
        {
            // 位 0 置位表示允许执行（即 DEP 关闭），位 1 置位表示禁止执行（DEP 开启）。
            QStringList parts;
            if ((flags & 0x1u) != 0)
            {
                parts.append(QStringLiteral("允许执行数据页（DEP 已关闭）"));
            }
            if ((flags & 0x2u) != 0)
            {
                parts.append(QStringLiteral("禁止执行数据页（DEP 已开启）"));
            }
            if ((flags & 0x4u) != 0)
            {
                parts.append(QStringLiteral("禁用 ATL thunk 模拟"));
            }
            if ((flags & 0x8u) != 0)
            {
                parts.append(QStringLiteral("策略已锁定，运行期不可更改"));
            }
            if (parts.isEmpty())
            {
                parts.append(QStringLiteral("使用系统默认策略"));
            }
            return QStringLiteral("0x%1（%2）")
                .arg(QString::number(flags, 16).toUpper())
                .arg(parts.join(QStringLiteral("；")));
        }

        // AppendMemoryRow 作用：受 kMaxMemoryRows 限制地追加一行内存区域。
        void AppendMemoryRow(DumpParseResult& result, MemoryRegionEntry entry)
        {
            ++result.memoryRegionTotal;
            if (result.memoryRegionShown >= kMaxMemoryRows)
            {
                return;
            }
            ++result.memoryRegionShown;
            result.memoryRegions.push_back(std::move(entry));
        }
    }

    void ParseSystemInfo(
        const DumpFileView& view,
        const MINIDUMP_LOCATION_DESCRIPTOR& location,
        DumpParseResult& result,
        std::uint16_t* const architectureOut)
    {
        MINIDUMP_SYSTEM_INFO info{};
        if (location.DataSize < sizeof(info) || !view.readStruct(location.Rva, &info))
        {
            result.diagnostics.append(QStringLiteral("系统信息流长度不足，已跳过。"));
            return;
        }
        *architectureOut = info.ProcessorArchitecture;
        result.overview.push_back({ QStringLiteral("CPU 架构"),
            ProcessorArchitectureText(info.ProcessorArchitecture) });
        result.overview.push_back({ QStringLiteral("逻辑处理器数"),
            QString::number(info.NumberOfProcessors) });
        // versionText：主.次.构建号；CSD（Service Pack 文本）非空时追加。
        QString versionText = QStringLiteral("%1.%2.%3")
            .arg(info.MajorVersion)
            .arg(info.MinorVersion)
            .arg(info.BuildNumber);
        const QString csdText = ReadMinidumpString(view, info.CSDVersionRva);
        if (!csdText.isEmpty())
        {
            versionText += QStringLiteral(" (%1)").arg(csdText);
        }
        result.overview.push_back({ QStringLiteral("操作系统版本"), versionText });
        // productText：区分工作站与服务器（VER_NT_* 常量）。
        QString productText;
        switch (info.ProductType)
        {
        case 1: productText = QStringLiteral("工作站 (VER_NT_WORKSTATION)"); break;
        case 2: productText = QStringLiteral("域控制器 (VER_NT_DOMAIN_CONTROLLER)"); break;
        case 3: productText = QStringLiteral("服务器 (VER_NT_SERVER)"); break;
        default: productText = QString::number(info.ProductType); break;
        }
        result.overview.push_back({ QStringLiteral("产品类型"), productText });
    }

    void ParseMiscInfo(
        const DumpFileView& view,
        const MINIDUMP_LOCATION_DESCRIPTOR& location,
        DumpParseResult& result)
    {
        // baseInfo：所有 MISC_INFO 版本的公共前缀；旧版本只保证前缀字段有效。
        MINIDUMP_MISC_INFO baseInfo{};
        if (location.DataSize < sizeof(baseInfo) || !view.readStruct(location.Rva, &baseInfo))
        {
            result.diagnostics.append(QStringLiteral("杂项信息流长度不足，已跳过。"));
            return;
        }
        if ((baseInfo.Flags1 & MINIDUMP_MISC1_PROCESS_ID) != 0)
        {
            result.overview.push_back({ QStringLiteral("进程 ID"),
                QString::number(baseInfo.ProcessId) });
        }
        if ((baseInfo.Flags1 & MINIDUMP_MISC1_PROCESS_TIMES) != 0)
        {
            result.overview.push_back({ QStringLiteral("进程创建时间"),
                TimeTToText(baseInfo.ProcessCreateTime) });
            result.overview.push_back({ QStringLiteral("进程 CPU 时间"),
                QStringLiteral("用户态 %1 秒 / 内核态 %2 秒")
                    .arg(baseInfo.ProcessUserTime)
                    .arg(baseInfo.ProcessKernelTime) });
        }
        // 处理器频率属于 MISC_INFO_2 扩展；流长度够时才读取。
        if (location.DataSize >= sizeof(MINIDUMP_MISC_INFO_2))
        {
            MINIDUMP_MISC_INFO_2 info2{};
            if (view.readStruct(location.Rva, &info2) &&
                (info2.Flags1 & MINIDUMP_MISC1_PROCESSOR_POWER_INFO) != 0)
            {
                result.overview.push_back({ QStringLiteral("处理器频率"),
                    QStringLiteral("当前 %1 MHz / 最大 %2 MHz")
                        .arg(info2.ProcessorCurrentMhz)
                        .arg(info2.ProcessorMaxMhz) });
            }
        }

#if defined(MINIDUMP_MISC3_TIMEZONE)
        // MISC_INFO_3 扩展：完整性级别、DEP 策略、受保护进程与时区。
        // 完整性级别能直接说明崩溃进程是不是提权运行的，排查权限类问题很有用。
        if (location.DataSize >= sizeof(MINIDUMP_MISC_INFO_3))
        {
            MINIDUMP_MISC_INFO_3 info3{};
            if (view.readStruct(location.Rva, &info3))
            {
                if ((info3.Flags1 & MINIDUMP_MISC3_PROCESS_INTEGRITY) != 0)
                {
                    result.overview.push_back({ QStringLiteral("进程完整性级别"),
                        IntegrityLevelText(info3.ProcessIntegrityLevel) });
                }
                if ((info3.Flags1 & MINIDUMP_MISC3_PROCESS_EXECUTE_FLAGS) != 0)
                {
                    result.overview.push_back({ QStringLiteral("DEP 策略"),
                        ExecuteFlagsText(info3.ProcessExecuteFlags) });
                }
                if ((info3.Flags1 & MINIDUMP_MISC3_PROTECTED_PROCESS) != 0)
                {
                    result.overview.push_back({ QStringLiteral("受保护进程"),
                        info3.ProtectedProcess != 0
                            ? QStringLiteral("是（PPL/PP，调试受限）")
                            : QStringLiteral("否") });
                }
                if ((info3.Flags1 & MINIDUMP_MISC3_TIMEZONE) != 0)
                {
                    // Bias 单位为分钟，定义是 UTC = 本地时间 + Bias，
                    // 因此东八区的 Bias 是 -480，显示时要取反才是常见的 UTC+08:00 写法。
                    const int biasMinutes = -static_cast<int>(info3.TimeZone.Bias);
                    result.overview.push_back({ QStringLiteral("时区偏移"),
                        QStringLiteral("UTC%1%2:%3")
                            .arg(biasMinutes < 0 ? QStringLiteral("-") : QStringLiteral("+"))
                            .arg(qAbs(biasMinutes) / 60, 2, 10, QLatin1Char('0'))
                            .arg(qAbs(biasMinutes) % 60, 2, 10, QLatin1Char('0')) });
                }
            }
        }
#endif

#if defined(MINIDUMP_MISC4_BUILDSTRING)
        // MISC_INFO_4 扩展：系统构建串，比 SystemInfo 的三段版本号精确得多，
        // 能直接对上具体的 Windows 版本与更新分支。
        if (location.DataSize >= sizeof(MINIDUMP_MISC_INFO_4))
        {
            MINIDUMP_MISC_INFO_4 info4{};
            if (view.readStruct(location.Rva, &info4) &&
                (info4.Flags1 & MINIDUMP_MISC4_BUILDSTRING) != 0)
            {
                const QString buildString = ReadFixedWideString(
                    info4.BuildString,
                    sizeof(info4.BuildString) / sizeof(info4.BuildString[0]));
                if (!buildString.isEmpty())
                {
                    result.overview.push_back({ QStringLiteral("系统构建串"), buildString });
                }
                const QString debuggerBuild = ReadFixedWideString(
                    info4.DbgBldStr,
                    sizeof(info4.DbgBldStr) / sizeof(info4.DbgBldStr[0]));
                if (!debuggerBuild.isEmpty())
                {
                    result.overview.push_back({ QStringLiteral("调试器构建串"), debuggerBuild });
                }
            }
        }
#endif
    }

    void ParseProcessVmCounters(
        const DumpFileView& view,
        const MINIDUMP_LOCATION_DESCRIPTOR& location,
        DumpParseResult& result)
    {
        // revision：结构版本，决定 PrivateUsage 之前是否还夹着两个虚拟大小字段。
        std::uint16_t revision = 0;
        if (location.DataSize < sizeof(MinidumpProcessVmCounters1) ||
            !view.readStruct(location.Rva, &revision))
        {
            return;
        }

        // 先取两版共有的前段字段（偏移完全一致），再按版本取后段。
        MinidumpProcessVmCounters1 base{};
        if (!view.readStruct(location.Rva, &base))
        {
            return;
        }
        std::uint64_t privateUsage = 0;
        std::uint64_t peakVirtualSize = 0;
        std::uint64_t virtualSize = 0;
        std::uint64_t privateWorkingSet = 0;
        if (revision >= 2)
        {
            MinidumpProcessVmCounters2 extended{};
            if (location.DataSize < sizeof(extended) ||
                !view.readStruct(location.Rva, &extended))
            {
                return;
            }
            if ((extended.Flags & kVmCountersVirtualSize) != 0)
            {
                peakVirtualSize = extended.PeakVirtualSize;
                virtualSize = extended.VirtualSize;
            }
            if ((extended.Flags & kVmCountersEx) != 0)
            {
                privateUsage = extended.PrivateUsage;
            }
            if ((extended.Flags & kVmCountersEx2) != 0)
            {
                privateWorkingSet = extended.PrivateWorkingSetSize;
            }
        }
        else
        {
            privateUsage = base.PrivateUsage;
        }

        // 私有字节与提交量是判断“是不是内存耗尽导致崩溃”的第一手数据：
        // 32 位进程的私有字节接近 2GB / 4GB 时基本可以确定是地址空间耗尽。
        if (privateUsage != 0)
        {
            result.overview.push_back({ QStringLiteral("私有字节"),
                ByteCountText(privateUsage) });
        }
        if (privateWorkingSet != 0)
        {
            result.overview.push_back({ QStringLiteral("私有工作集"),
                ByteCountText(privateWorkingSet) });
        }
        result.overview.push_back({ QStringLiteral("提交量 (当前/峰值)"),
            QStringLiteral("%1 / %2")
                .arg(ByteCountText(base.PagefileUsage))
                .arg(ByteCountText(base.PeakPagefileUsage)) });
        result.overview.push_back({ QStringLiteral("工作集 (当前/峰值)"),
            QStringLiteral("%1 / %2")
                .arg(ByteCountText(base.WorkingSetSize))
                .arg(ByteCountText(base.PeakWorkingSetSize)) });
        if (virtualSize != 0 || peakVirtualSize != 0)
        {
            // 虚拟地址空间包含大量“已保留未提交”的区域，数值远大于实际内存占用是正常的，
            // 但 32 位进程接近 2GB 时就是地址空间耗尽的直接证据。
            result.overview.push_back({ QStringLiteral("虚拟地址空间 (当前/峰值)"),
                QStringLiteral("%1 / %2")
                    .arg(ByteCountText(virtualSize))
                    .arg(ByteCountText(peakVirtualSize)) });
        }
        result.overview.push_back({ QStringLiteral("页错误次数"),
            QString::number(base.PageFaultCount) });
        if (base.QuotaNonPagedPoolUsage != 0 || base.QuotaPagedPoolUsage != 0)
        {
            result.overview.push_back({ QStringLiteral("内核池用量 (分页/非分页)"),
                QStringLiteral("%1 / %2")
                    .arg(ByteCountText(base.QuotaPagedPoolUsage))
                    .arg(ByteCountText(base.QuotaNonPagedPoolUsage)) });
        }
    }

    void ParseExceptionStream(
        const DumpFileView& view,
        const MINIDUMP_LOCATION_DESCRIPTOR& location,
        const std::uint16_t architecture,
        const ModuleIndex& modules,
        DumpParseResult& result,
        std::uint32_t* const faultingThreadIdOut,
        MINIDUMP_LOCATION_DESCRIPTOR* const contextLocationOut)
    {
        MINIDUMP_EXCEPTION_STREAM stream{};
        if (location.DataSize < sizeof(stream) || !view.readStruct(location.Rva, &stream))
        {
            result.diagnostics.append(QStringLiteral("异常流长度不足，已跳过。"));
            return;
        }
        *faultingThreadIdOut = stream.ThreadId;
        *contextLocationOut = stream.ThreadContext;
        // record：异常记录本体；code/address 是诊断的核心。
        const MINIDUMP_EXCEPTION& record = stream.ExceptionRecord;
        const std::uint32_t code = static_cast<std::uint32_t>(record.ExceptionCode);
        result.exceptionCode = code;
        result.faultingThreadId = stream.ThreadId;
        result.exceptionInfo.push_back({ QStringLiteral("异常线程 ID"),
            QString::number(stream.ThreadId) });
        // codeText：数值 + 常量名；meaning 单独一行给出中文解释。
        QString codeText = Hex(code);
        const QString codeName = ExceptionCodeName(code);
        if (!codeName.isEmpty())
        {
            codeText += QStringLiteral(" (%1)").arg(codeName);
        }
        result.exceptionInfo.push_back({ QStringLiteral("异常代码"), codeText });
        const QString meaning = ExceptionCodeMeaning(code);
        if (!meaning.isEmpty())
        {
            result.exceptionInfo.push_back({ QStringLiteral("异常含义"), meaning });
        }
        // 异常地址带上模块归属：光有一个裸地址没法直接指认是谁崩的。
        const QString addressNote = modules.annotate(record.ExceptionAddress);
        result.exceptionInfo.push_back({ QStringLiteral("异常地址"),
            addressNote.isEmpty()
                ? Hex(record.ExceptionAddress)
                : QStringLiteral("%1  —  %2").arg(Hex(record.ExceptionAddress), addressNote) });
        if (record.ExceptionFlags != 0)
        {
            result.exceptionInfo.push_back({ QStringLiteral("异常标志"),
                QStringLiteral("%1%2")
                    .arg(Hex(record.ExceptionFlags))
                    .arg((record.ExceptionFlags & 1) != 0
                        ? QStringLiteral("（不可继续）")
                        : QString()) });
        }

        // 按异常类别做专项解读：这些参数才是真正指向根因的信息。
        const std::uint32_t parameterCount = std::min<std::uint32_t>(
            record.NumberParameters, EXCEPTION_MAXIMUM_PARAMETERS);
        if ((code == 0xC0000005u || code == 0xC0000006u) && parameterCount >= 2)
        {
            // 访问违例 / 换页错误：参数 0 是操作类型，参数 1 是出错地址。
            const std::uint64_t faultAddress = record.ExceptionInformation[1];
            result.exceptionInfo.push_back({ QStringLiteral("违例详情"),
                AccessViolationDetailText(record.ExceptionInformation[0], faultAddress) });
            // 出错地址的性质往往直接定性问题：空指针、哨兵值还是有效模块地址。
            const AddressNote faultNote = modules.resolve(faultAddress);
            QString faultDescription = faultNote.symbolText.isEmpty()
                ? faultNote.description
                : QStringLiteral("%1（%2）").arg(faultNote.symbolText, faultNote.description);
            if (faultNote.kind == AddressKind::NullPage)
            {
                faultDescription = faultAddress == 0
                    ? QStringLiteral("空指针解引用：指针本身为 NULL。")
                    : QStringLiteral("空指针解引用：基指针为 NULL，再加上约 0x%1 的成员偏移。")
                        .arg(QString::number(faultAddress, 16).toUpper());
            }
            if (!faultDescription.isEmpty())
            {
                result.exceptionInfo.push_back({
                    QStringLiteral("出错地址性质"), faultDescription });
            }
            if (code == 0xC0000006u && parameterCount >= 3)
            {
                // 换页错误的参数 2 是底层 I/O 的 NTSTATUS，说明为什么页读不进来。
                const std::uint32_t pageStatus =
                    static_cast<std::uint32_t>(record.ExceptionInformation[2]);
                const QString statusName = ExceptionCodeName(pageStatus);
                result.exceptionInfo.push_back({ QStringLiteral("换页失败的 NTSTATUS"),
                    statusName.isEmpty()
                        ? Hex(pageStatus)
                        : QStringLiteral("%1 (%2)").arg(Hex(pageStatus), statusName) });
            }
        }
        else if ((code == 0xC0000409u || code == 0xC0000602u) && parameterCount >= 1)
        {
            // fast-fail：参数 0 的子码才说明触发了哪条安全检查。
            const std::uint64_t failFastCode = record.ExceptionInformation[0];
            const QString failFastText = FastFailCodeText(failFastCode);
            result.exceptionInfo.push_back({ QStringLiteral("fast-fail 子码"),
                failFastText.isEmpty()
                    ? QStringLiteral("%1（未收录的子码）").arg(Hex(failFastCode))
                    : QStringLiteral("%1  —  %2").arg(Hex(failFastCode), failFastText) });
            result.exceptionInfo.push_back({ QStringLiteral("性质"),
                QStringLiteral("这是进程主动终止，不是被动崩溃：安全检查发现状态已被破坏，"
                    "为避免继续执行造成更大危害而立即退出。") });
        }
        else if (IsCppException(code) && parameterCount >= 2)
        {
            // MSVC C++ 异常：崩溃点是 throw 的位置，而不是缺陷发生的位置。
            const QString magicText = CppExceptionMagicText(record.ExceptionInformation[0]);
            result.exceptionInfo.push_back({ QStringLiteral("C++ 异常记录"),
                magicText.isEmpty()
                    ? QStringLiteral("魔数 %1（未知版本）").arg(Hex(record.ExceptionInformation[0]))
                    : magicText });
            result.exceptionInfo.push_back({ QStringLiteral("异常对象地址"),
                Hex(record.ExceptionInformation[1]) });
            if (parameterCount >= 3)
            {
                const QString throwInfoNote = modules.annotate(record.ExceptionInformation[2]);
                result.exceptionInfo.push_back({ QStringLiteral("ThrowInfo 地址"),
                    throwInfoNote.isEmpty()
                        ? Hex(record.ExceptionInformation[2])
                        : QStringLiteral("%1  —  %2")
                              .arg(Hex(record.ExceptionInformation[2]), throwInfoNote) });
            }
            if (parameterCount >= 4)
            {
                // 64 位下参数 3 是抛出方所在模块的基址，直接点名是谁抛的。
                const QString moduleNote = modules.annotate(record.ExceptionInformation[3]);
                result.exceptionInfo.push_back({ QStringLiteral("抛出方模块基址"),
                    moduleNote.isEmpty()
                        ? Hex(record.ExceptionInformation[3])
                        : QStringLiteral("%1  —  %2")
                              .arg(Hex(record.ExceptionInformation[3]), moduleNote) });
            }
            result.exceptionInfo.push_back({ QStringLiteral("性质"),
                QStringLiteral("未被接住的 C++ 异常：崩溃点是 throw 的位置，"
                    "缺陷通常在更上游；需要 PDB 才能还原异常类型名。") });
        }
        else if (IsManagedException(code))
        {
            result.exceptionInfo.push_back({ QStringLiteral("性质"),
                QStringLiteral("这是 .NET 托管异常：本工具只能解析原生层信息，"
                    "托管调用栈与异常类型需要用 SOS/dotnet-dump 分析同一份转储。") });
            for (std::uint32_t index = 0; index < parameterCount; ++index)
            {
                result.exceptionInfo.push_back({
                    QStringLiteral("参数 %1").arg(index + 1),
                    Hex(record.ExceptionInformation[index]) });
            }
        }
        else
        {
            for (std::uint32_t index = 0; index < parameterCount; ++index)
            {
                // 无固定语义的参数也过一遍模块归属，很多参数其实就是地址。
                const std::uint64_t parameter = record.ExceptionInformation[index];
                const QString note = parameter != 0 ? modules.annotate(parameter) : QString();
                result.exceptionInfo.push_back({
                    QStringLiteral("参数 %1").arg(index + 1),
                    note.isEmpty() ? Hex(parameter)
                                   : QStringLiteral("%1  —  %2").arg(Hex(parameter), note) });
            }
        }

        // 崩溃点指令指针：从异常线程上下文取，异常地址可能被内层分发改写。
        const std::uint64_t contextIp = InstructionPointerFromContext(
            view, stream.ThreadContext, architecture);
        if (contextIp != 0)
        {
            const QString ipNote = modules.annotate(contextIp);
            result.exceptionInfo.push_back({ QStringLiteral("上下文指令指针"),
                ipNote.isEmpty() ? Hex(contextIp)
                                 : QStringLiteral("%1  —  %2").arg(Hex(contextIp), ipNote) });
        }
        // faultingAddress：归因优先用上下文 IP；它拿不到时退回异常记录里的地址。
        result.faultingAddress = contextIp != 0 ? contextIp : record.ExceptionAddress;
    }

    void ParseThreadList(
        const DumpFileView& view,
        const MINIDUMP_LOCATION_DESCRIPTOR& location,
        const bool isExtendedList,
        const std::uint16_t architecture,
        const std::uint32_t faultingThreadId,
        const ModuleIndex& modules,
        DumpParseResult& result,
        std::vector<StackScanInput>* const scanInputsOut,
        DumpMemoryReader* const memory)
    {
        // threadCount：数组条数；两种流头部都是一个 ULONG32 计数。
        ULONG32 threadCount = 0;
        if (!view.readStruct(location.Rva, &threadCount))
        {
            result.diagnostics.append(QStringLiteral("线程列表流头部读取失败。"));
            return;
        }
        // entrySize：普通线程 48 字节，扩展线程多一个背景存储描述符。
        const std::uint64_t entrySize = isExtendedList
            ? sizeof(MINIDUMP_THREAD_EX)
            : sizeof(MINIDUMP_THREAD);
        const std::uint64_t safeCount = std::min<std::uint64_t>(threadCount, kMaxListEntries);
        if (safeCount != threadCount)
        {
            result.diagnostics.append(
                QStringLiteral("线程数 %1 超过解析上限，仅展示前 %2 条。")
                    .arg(threadCount)
                    .arg(safeCount));
        }
        result.threads.reserve(static_cast<std::size_t>(safeCount));
        for (std::uint64_t index = 0; index < safeCount; ++index)
        {
            // thread：单条线程记录；扩展流的前半部分与普通流布局一致。
            MINIDUMP_THREAD thread{};
            const std::uint64_t entryOffset =
                location.Rva + sizeof(ULONG32) + index * entrySize;
            if (!view.readStruct(entryOffset, &thread))
            {
                result.diagnostics.append(QStringLiteral("线程条目越界，列表提前结束。"));
                break;
            }
            ThreadEntry entry{};
            entry.threadId = thread.ThreadId;
            entry.suspendCount = thread.SuspendCount;
            entry.priorityClass = thread.PriorityClass;
            entry.priority = thread.Priority;
            entry.teb = thread.Teb;
            entry.stackBase = thread.Stack.StartOfMemoryRange;
            entry.stackSize = thread.Stack.Memory.DataSize;
            entry.instructionPointer = InstructionPointerFromContext(
                view, thread.ThreadContext, architecture);
            entry.faulting = (thread.ThreadId == faultingThreadId);
            // 指令指针的模块归属：线程表里直接能看出每个线程停在哪个模块。
            if (entry.instructionPointer != 0)
            {
                entry.ipSymbolText = modules.symbolText(entry.instructionPointer);
            }
            // 栈指针来自同一份 CONTEXT，是栈扫描的起点。
            const ContextArch contextArch =
                ContextArchFromProcessorArchitecture(architecture);
            std::uint64_t threadIp = 0;
            std::uint64_t threadSp = 0;
            ReadContextPointers(
                view,
                thread.ThreadContext.Rva,
                thread.ThreadContext.DataSize,
                contextArch,
                &threadIp,
                &threadSp,
                nullptr);

            // 线程栈本身也是一段已捕获内存，登记进索引供地址读取使用。
            if (memory != nullptr && thread.Stack.Memory.DataSize != 0)
            {
                memory->addRange(
                    thread.Stack.StartOfMemoryRange,
                    thread.Stack.Memory.Rva,
                    thread.Stack.Memory.DataSize,
                    QStringLiteral("线程栈"));
            }
            // 收集栈扫描输入：真正的扫描要等模块索引与内存索引都就绪后再做。
            if (scanInputsOut != nullptr && thread.Stack.Memory.DataSize != 0)
            {
                StackScanInput input{};
                input.stackFileOffset = thread.Stack.Memory.Rva;
                input.stackBytes = thread.Stack.Memory.DataSize;
                input.stackBaseAddress = thread.Stack.StartOfMemoryRange;
                input.stackPointer = threadSp;
                input.instructionPointer = entry.instructionPointer;
                input.pointerSize = ContextPointerSize(contextArch);
                input.threadId = thread.ThreadId;
                scanInputsOut->push_back(input);
            }
            result.threads.push_back(std::move(entry));
        }
    }

    ThreadIdIndex BuildThreadIdIndex(const std::vector<ThreadEntry>& threads)
    {
        ThreadIdIndex result;
        result.reserve(threads.size());
        for (std::size_t index = 0; index < threads.size(); ++index)
        {
            result.emplace(threads[index].threadId, index);
        }
        return result;
    }

    void ParseThreadInfoList(
        const DumpFileView& view,
        const MINIDUMP_LOCATION_DESCRIPTOR& location,
        DumpParseResult& result,
        const ThreadIdIndex& threadIndex)
    {
        MINIDUMP_THREAD_INFO_LIST header{};
        if (!view.readStruct(location.Rva, &header) ||
            header.SizeOfHeader < sizeof(header) ||
            header.SizeOfEntry < sizeof(MINIDUMP_THREAD_INFO))
        {
            return;
        }
        const std::uint64_t count = std::min<std::uint64_t>(
            header.NumberOfEntries, kMaxListEntries);
        for (std::uint64_t index = 0; index < count; ++index)
        {
            MINIDUMP_THREAD_INFO info{};
            if (!view.readStruct(
                    location.Rva + header.SizeOfHeader + index * header.SizeOfEntry,
                    &info))
            {
                break;
            }
            // 命中已解析线程时补充起始地址与 CPU 时间（100ns → 毫秒）。
            const auto found = threadIndex.find(info.ThreadId);
            if (found != threadIndex.end())
            {
                ThreadEntry& entry = result.threads[found->second];
                entry.startAddress = info.StartAddress;
                const double userMs = static_cast<double>(info.UserTime) / 10000.0;
                const double kernelMs = static_cast<double>(info.KernelTime) / 10000.0;
                if (info.UserTime != 0 || info.KernelTime != 0)
                {
                    entry.cpuTimeText = QStringLiteral("%1 / %2 ms")
                        .arg(QString::number(userMs, 'f', 1))
                        .arg(QString::number(kernelMs, 'f', 1));
                }
            }
        }
    }

    void ParseThreadNames(
        const DumpFileView& view,
        const MINIDUMP_LOCATION_DESCRIPTOR& location,
        DumpParseResult& result,
        const ThreadIdIndex& threadIndex)
    {
        MinidumpThreadNameHeader header{};
        if (!view.readStruct(location.Rva, &header))
        {
            return;
        }
        const std::uint64_t count = std::min<std::uint64_t>(
            header.NumberOfThreadNames, kMaxListEntries);
        for (std::uint64_t index = 0; index < count; ++index)
        {
            MinidumpThreadNameEntry entry{};
            if (!view.readStruct(
                    location.Rva + sizeof(header) + index * sizeof(entry),
                    &entry))
            {
                break;
            }
            const QString threadName = ReadMinidumpString(view, entry.RvaOfThreadName);
            if (threadName.isEmpty())
            {
                continue;
            }
            const auto found = threadIndex.find(entry.ThreadId);
            if (found != threadIndex.end())
            {
                result.threads[found->second].name = threadName;
            }
        }
    }

    void ParseModuleList(
        const DumpFileView& view,
        const MINIDUMP_LOCATION_DESCRIPTOR& location,
        DumpParseResult& result)
    {
        ULONG32 moduleCount = 0;
        if (!view.readStruct(location.Rva, &moduleCount))
        {
            result.diagnostics.append(QStringLiteral("模块列表流头部读取失败。"));
            return;
        }
        const std::uint64_t safeCount = std::min<std::uint64_t>(moduleCount, kMaxListEntries);
        if (safeCount != moduleCount)
        {
            result.diagnostics.append(
                QStringLiteral("模块数 %1 超过解析上限，仅展示前 %2 条。")
                    .arg(moduleCount)
                    .arg(safeCount));
        }
        result.modules.reserve(static_cast<std::size_t>(safeCount));
        for (std::uint64_t index = 0; index < safeCount; ++index)
        {
            MINIDUMP_MODULE module{};
            if (!view.readStruct(
                    location.Rva + sizeof(ULONG32) + index * sizeof(MINIDUMP_MODULE),
                    &module))
            {
                result.diagnostics.append(QStringLiteral("模块条目越界，列表提前结束。"));
                break;
            }
            ModuleEntry entry{};
            entry.name = ReadMinidumpString(view, module.ModuleNameRva);
            entry.base = module.BaseOfImage;
            entry.size = module.SizeOfImage;
            entry.checksum = module.CheckSum;
            entry.timeDateStamp = module.TimeDateStamp;
            entry.timestampText = TimeTToText(module.TimeDateStamp);
            // 版本号只有 VS_FIXEDFILEINFO 魔数有效时才可信。
            if (module.VersionInfo.dwSignature == 0xFEEF04BDu)
            {
                entry.version = QStringLiteral("%1.%2.%3.%4")
                    .arg(HIWORD(module.VersionInfo.dwFileVersionMS))
                    .arg(LOWORD(module.VersionInfo.dwFileVersionMS))
                    .arg(HIWORD(module.VersionInfo.dwFileVersionLS))
                    .arg(LOWORD(module.VersionInfo.dwFileVersionLS));
            }
            ReadCodeViewRecord(view, module.CvRecord, &entry.pdbName, &entry.pdbGuidAge);
            result.modules.push_back(std::move(entry));
        }
    }

    void ParseUnloadedModuleList(
        const DumpFileView& view,
        const MINIDUMP_LOCATION_DESCRIPTOR& location,
        DumpParseResult& result)
    {
        MINIDUMP_UNLOADED_MODULE_LIST header{};
        if (!view.readStruct(location.Rva, &header) ||
            header.SizeOfHeader < sizeof(header) ||
            header.SizeOfEntry < sizeof(MINIDUMP_UNLOADED_MODULE))
        {
            return;
        }
        const std::uint64_t count = std::min<std::uint64_t>(
            header.NumberOfEntries, kMaxListEntries);
        result.unloadedModules.reserve(static_cast<std::size_t>(count));
        for (std::uint64_t index = 0; index < count; ++index)
        {
            MINIDUMP_UNLOADED_MODULE module{};
            if (!view.readStruct(
                    location.Rva + header.SizeOfHeader + index * header.SizeOfEntry,
                    &module))
            {
                break;
            }
            UnloadedModuleEntry entry{};
            entry.name = ReadMinidumpString(view, module.ModuleNameRva);
            entry.base = module.BaseOfImage;
            entry.size = module.SizeOfImage;
            entry.checksum = module.CheckSum;
            entry.timeDateStamp = module.TimeDateStamp;
            entry.timestampText = TimeTToText(module.TimeDateStamp);
            result.unloadedModules.push_back(std::move(entry));
        }
    }

    void ParseMemoryLists(
        const DumpFileView& view,
        const std::uint64_t directoryRva,
        const std::uint32_t streamCount,
        DumpParseResult& result,
        DumpMemoryReader* const memory)
    {
        MINIDUMP_LOCATION_DESCRIPTOR location{};
        // hasDataList：Memory64List 是否已经供过展示表数据；
        // MemoryInfoList 命中时它保持 false，展示表走信息更全的那条路。
        bool hasDataList = false;
        // useInfoList：MemoryInfoList 是否可用；可用时展示表用它，
        // 但内存索引仍必须从 Memory64List/MemoryList 建立（前者不含数据）。
        bool useInfoList = false;
        MINIDUMP_LOCATION_DESCRIPTOR infoLocation{};
        MINIDUMP_MEMORY_INFO_LIST infoHeader{};
        if (FindStream(view, directoryRva, streamCount, MemoryInfoListStream, &infoLocation) &&
            view.readStruct(infoLocation.Rva, &infoHeader) &&
            infoHeader.SizeOfHeader >= sizeof(infoHeader) &&
            infoHeader.SizeOfEntry >= sizeof(MINIDUMP_MEMORY_INFO))
        {
            useInfoList = true;
        }

        // ---- 第一步：从带数据的内存流建立虚拟地址索引（并在需要时填展示表）----
        if (FindStream(view, directoryRva, streamCount, Memory64ListStream, &location))
        {
            // 头部为条数 + 数据基偏移共 16 字节；SDK 结构带零长数组成员，
            // 不能按值实例化，这里逐字段读取。
            std::uint64_t numberOfRanges = 0;
            std::uint64_t baseRva = 0;
            constexpr std::uint64_t kMemory64HeaderBytes = 16;
            if (view.readStruct(location.Rva, &numberOfRanges) &&
                view.readStruct(location.Rva + 8, &baseRva))
            {
                // Memory64List 的数据区是紧挨着连续排布的：每段的文件偏移
                // 由 baseRva 起逐段累加，描述符里并不单独记录偏移。
                std::uint64_t dataOffset = baseRva;
                const std::uint64_t safeCount =
                    std::min<std::uint64_t>(numberOfRanges, kMaxListEntries);
                for (std::uint64_t index = 0; index < safeCount; ++index)
                {
                    MINIDUMP_MEMORY_DESCRIPTOR64 descriptor{};
                    if (!view.readStruct(
                            location.Rva + kMemory64HeaderBytes +
                                index * sizeof(MINIDUMP_MEMORY_DESCRIPTOR64),
                            &descriptor))
                    {
                        break;
                    }
                    if (memory != nullptr)
                    {
                        memory->addRange(
                            descriptor.StartOfMemoryRange,
                            dataOffset,
                            descriptor.DataSize,
                            QStringLiteral("64 位内存列表"));
                    }
                    // DataSize 完全由文件控制，逐段累加可能绕回。一旦绕回，
                    // 后续所有段的文件偏移都会指向无关字节，而 addRange 的
                    // 文件边界校验又恰好放行——虚拟地址会被别名到错误数据上，
                    // 栈扫描据此得出的“调用栈”就是彻头彻尾的假象。溢出即停。
                    if (dataOffset + descriptor.DataSize < dataOffset)
                    {
                        result.diagnostics.append(
                            QStringLiteral("64 位内存列表的数据区偏移累加越界，"
                                "其余内存段已跳过。"));
                        break;
                    }
                    dataOffset += descriptor.DataSize;
                    if (!useInfoList)
                    {
                        MemoryRegionEntry entry{};
                        entry.base = descriptor.StartOfMemoryRange;
                        entry.size = descriptor.DataSize;
                        entry.source = QStringLiteral("64 位内存列表");
                        AppendMemoryRow(result, std::move(entry));
                    }
                }
                hasDataList = true;
            }
        }
        if (FindStream(view, directoryRva, streamCount, MemoryListStream, &location))
        {
            ULONG32 rangeCount = 0;
            if (view.readStruct(location.Rva, &rangeCount))
            {
                const std::uint64_t safeCount =
                    std::min<std::uint64_t>(rangeCount, kMaxListEntries);
                for (std::uint64_t index = 0; index < safeCount; ++index)
                {
                    MINIDUMP_MEMORY_DESCRIPTOR descriptor{};
                    if (!view.readStruct(
                            location.Rva + sizeof(ULONG32) +
                                index * sizeof(MINIDUMP_MEMORY_DESCRIPTOR),
                            &descriptor))
                    {
                        break;
                    }
                    if (memory != nullptr)
                    {
                        memory->addRange(
                            descriptor.StartOfMemoryRange,
                            descriptor.Memory.Rva,
                            descriptor.Memory.DataSize,
                            QStringLiteral("内存列表"));
                    }
                    if (!useInfoList && !hasDataList)
                    {
                        MemoryRegionEntry entry{};
                        entry.base = descriptor.StartOfMemoryRange;
                        entry.size = descriptor.Memory.DataSize;
                        entry.source = QStringLiteral("内存列表");
                        AppendMemoryRow(result, std::move(entry));
                    }
                }
            }
        }

        // ---- 第二步：展示表优先用 MemoryInfoList，它带状态/保护/类型三列 ----
        if (useInfoList)
        {
            const std::uint64_t safeCount =
                std::min<std::uint64_t>(infoHeader.NumberOfEntries, kMaxListEntries);
            for (std::uint64_t index = 0; index < safeCount; ++index)
            {
                MINIDUMP_MEMORY_INFO info{};
                if (!view.readStruct(
                        infoLocation.Rva + infoHeader.SizeOfHeader +
                            index * infoHeader.SizeOfEntry,
                        &info))
                {
                    break;
                }
                MemoryRegionEntry entry{};
                entry.base = info.BaseAddress;
                entry.size = info.RegionSize;
                entry.state = MemoryStateText(info.State);
                entry.protect = MemoryProtectText(info.Protect);
                entry.type = MemoryTypeText(info.Type);
                entry.source = QStringLiteral("内存信息列表");
                AppendMemoryRow(result, std::move(entry));
            }
        }
    }

    void ParseHandleData(
        const DumpFileView& view,
        const MINIDUMP_LOCATION_DESCRIPTOR& location,
        DumpParseResult& result)
    {
        MINIDUMP_HANDLE_DATA_STREAM header{};
        if (!view.readStruct(location.Rva, &header) ||
            header.SizeOfHeader < sizeof(header) ||
            header.SizeOfDescriptor < sizeof(MINIDUMP_HANDLE_DESCRIPTOR))
        {
            return;
        }
        const std::uint64_t count = std::min<std::uint64_t>(
            header.NumberOfDescriptors, kMaxListEntries);
        result.handles.reserve(static_cast<std::size_t>(count));
        for (std::uint64_t index = 0; index < count; ++index)
        {
            // descriptor：V1 描述符是 V2 的前缀，按 V1 读取对两版都安全。
            MINIDUMP_HANDLE_DESCRIPTOR descriptor{};
            if (!view.readStruct(
                    location.Rva + header.SizeOfHeader + index * header.SizeOfDescriptor,
                    &descriptor))
            {
                break;
            }
            HandleEntry entry{};
            entry.handleValue = descriptor.Handle;
            entry.typeName = ReadMinidumpString(view, descriptor.TypeNameRva);
            entry.objectName = ReadMinidumpString(view, descriptor.ObjectNameRva);
            entry.attributes = descriptor.Attributes;
            entry.grantedAccess = descriptor.GrantedAccess;
            entry.handleCount = descriptor.HandleCount;
            entry.pointerCount = descriptor.PointerCount;
            result.handles.push_back(std::move(entry));
        }
    }

    void ParseComments(
        const DumpFileView& view,
        const std::uint64_t directoryRva,
        const std::uint32_t streamCount,
        DumpParseResult& result)
    {
        MINIDUMP_LOCATION_DESCRIPTOR location{};
        if (FindStream(view, directoryRva, streamCount, CommentStreamW, &location))
        {
            // wideBytes：注释按上限截断；UTF-16 需要偶数字节。
            const std::uint64_t wideBytes =
                std::min<std::uint64_t>(location.DataSize, kMaxCommentBytes) & ~1ull;
            const unsigned char* const wideData = view.at(location.Rva, wideBytes);
            if (wideData != nullptr && wideBytes > 0)
            {
                const QString comment = QString::fromUtf16(
                    reinterpret_cast<const char16_t*>(wideData),
                    static_cast<qsizetype>(wideBytes / 2)).trimmed();
                if (!comment.isEmpty())
                {
                    result.overview.push_back({ QStringLiteral("注释 (Unicode)"), comment });
                }
            }
        }
        if (FindStream(view, directoryRva, streamCount, CommentStreamA, &location))
        {
            const std::uint64_t ansiBytes =
                std::min<std::uint64_t>(location.DataSize, kMaxCommentBytes);
            const unsigned char* const ansiData = view.at(location.Rva, ansiBytes);
            if (ansiData != nullptr && ansiBytes > 0)
            {
                const QString comment = QString::fromLocal8Bit(
                    reinterpret_cast<const char*>(ansiData),
                    static_cast<qsizetype>(ansiBytes)).trimmed();
                if (!comment.isEmpty())
                {
                    result.overview.push_back({ QStringLiteral("注释 (ANSI)"), comment });
                }
            }
        }
    }

    QString DumpTypeFlagText(const std::uint64_t flags)
    {
        if (flags == 0)
        {
            return QStringLiteral("MiniDumpNormal");
        }
        // FlagName：单个位与其常量名的映射，覆盖常用的转储类型位。
        struct FlagName
        {
            std::uint64_t bit; // bit：标志位。
            const char* name;  // name：MiniDumpWith* 常量名。
        };
        static constexpr FlagName kFlags[] = {
            { 0x00000001, "MiniDumpWithDataSegs" },
            { 0x00000002, "MiniDumpWithFullMemory" },
            { 0x00000004, "MiniDumpWithHandleData" },
            { 0x00000008, "MiniDumpFilterMemory" },
            { 0x00000010, "MiniDumpScanMemory" },
            { 0x00000020, "MiniDumpWithUnloadedModules" },
            { 0x00000040, "MiniDumpWithIndirectlyReferencedMemory" },
            { 0x00000080, "MiniDumpFilterModulePaths" },
            { 0x00000100, "MiniDumpWithProcessThreadData" },
            { 0x00000200, "MiniDumpWithPrivateReadWriteMemory" },
            { 0x00000400, "MiniDumpWithoutOptionalData" },
            { 0x00000800, "MiniDumpWithFullMemoryInfo" },
            { 0x00001000, "MiniDumpWithThreadInfo" },
            { 0x00002000, "MiniDumpWithCodeSegs" },
            { 0x00004000, "MiniDumpWithoutAuxiliaryState" },
            { 0x00008000, "MiniDumpWithFullAuxiliaryState" },
            { 0x00010000, "MiniDumpWithPrivateWriteCopyMemory" },
            { 0x00020000, "MiniDumpIgnoreInaccessibleMemory" },
            { 0x00040000, "MiniDumpWithTokenInformation" },
            { 0x00080000, "MiniDumpWithModuleHeaders" },
            { 0x00100000, "MiniDumpFilterTriage" },
            { 0x00200000, "MiniDumpWithAvxXStateContext" },
            { 0x00400000, "MiniDumpWithIptTrace" },
            { 0x00800000, "MiniDumpScanInaccessiblePartialPages" },
        };
        QStringList names;
        for (const FlagName& flag : kFlags)
        {
            if ((flags & flag.bit) != 0)
            {
                names.append(QString::fromLatin1(flag.name));
            }
        }
        if (names.isEmpty())
        {
            return Hex(flags);
        }
        return QStringLiteral("%1 (%2)").arg(Hex(flags)).arg(names.join(QStringLiteral(" | ")));
    }
}
