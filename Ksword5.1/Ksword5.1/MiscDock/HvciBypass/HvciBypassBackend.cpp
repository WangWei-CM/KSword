// ============================================================
// HvciBypassBackend.cpp
// 作用：
// - 实现 HvciBypassBackend.h 声明的四个后端函数；
// - 查询 CI/HVCI 姿态、枚举内核模块、下发映像保护变更实验、
//   以及把实验结果翻译成面向用户的可读判定文本；
// - 本文件不含 QWidget，UI 逻辑在 HvciBypassPage 中。
// ============================================================

#include "HvciBypassBackend.h"

// ArkDriverClient 提供 DriverClient、IoResult 和共享协议结构体。
#include "../../ArkDriverClient/ArkDriverClient.h"
#include "../../../../shared/driver/KswordArkMemoryResult.h"

// Framework.h 是主程序统一入口，拉入 Qt、日志和 ksword 工具链。
#include "../../Framework.h"

#include <windows.h>

#include <algorithm>
#include <cstring>
#include <vector>

namespace
{
    // ================================================================
    // NtQuerySystemInformation 本地定义，避免依赖 DDK 头。
    // ================================================================

    // kSystemModuleInformationClass：SystemModuleInformation 的类号。
    constexpr unsigned long kSystemModuleInformationClass = 11UL;

    // kStatusInfoLengthMismatch：缓冲区不够时的 NTSTATUS 返回值。
    constexpr long kStatusInfoLengthMismatch = static_cast<long>(0xC0000004L);

    // NtQuerySystemInformationFn：NtQuerySystemInformation 的调用约定签名。
    using NtQuerySystemInformationFn = long(NTAPI*)(
        unsigned long systemInformationClass,
        void* systemInformation,
        unsigned long systemInformationLength,
        unsigned long* returnLength);

    // KernelModuleRow：RTL_PROCESS_MODULE_INFORMATION 的本地镜像（x64 下 296 字节）。
    struct KernelModuleRow
    {
        void* section;             // section：节句柄。
        void* mappedBase;          // mappedBase：用户态映射基址。
        void* imageBase;           // imageBase：内核加载基址。
        unsigned long imageSize;   // imageSize：映像大小。
        unsigned long flags;       // flags：模块标志。
        unsigned short loadOrderIndex;  // loadOrderIndex：加载顺序索引。
        unsigned short initOrderIndex;  // initOrderIndex：初始化顺序索引。
        unsigned short loadCount;       // loadCount：引用计数。
        unsigned short fileNameOffset;  // fileNameOffset：fullPathName 中文件名起始偏移。
        unsigned char fullPathName[256]; // fullPathName：完整 NT 路径。
    };

    // KernelModuleList：RTL_PROCESS_MODULES 的本地镜像（变长数组头 + 第一行）。
    struct KernelModuleList
    {
        unsigned long count;       // count：模块总数。
        KernelModuleRow rows[1];   // rows：变长数组，实际长度由 count 决定。
    };

    // resolveNtQuery：
    // - 作用：从 ntdll 解析 NtQuerySystemInformation 函数指针；
    // - 返回：函数指针；ntdll 不存在时返回 nullptr。
    NtQuerySystemInformationFn resolveNtQuery()
    {
        // hNtdll：ntdll.dll 模块句柄，进程启动后常驻，无需 FreeLibrary。
        const HMODULE hNtdll = ::GetModuleHandleW(L"ntdll.dll");
        if (hNtdll == nullptr)
        {
            return nullptr;
        }
        return reinterpret_cast<NtQuerySystemInformationFn>(
            ::GetProcAddress(hNtdll, "NtQuerySystemInformation"));
    }

    // formatNtStatus：
    // - 输入 status：一个 NTSTATUS 值；
    // - 作用：把 NTSTATUS 格式化成十六进制字符串；
    // - 返回："0x00000000" 形式的 8 位十六进制。
    QString formatNtStatus(const long status)
    {
        return QStringLiteral("0x%1")
            .arg(static_cast<unsigned long>(status), 8, 16, QChar('0'));
    }

    // stageResultLine：
    // - 输入 stageName：阶段名称；status：该阶段的 NTSTATUS；
    // - 作用：为单个实验阶段生成一行状态报告文本；
    // - 返回：形如 "[阶段] 映像保护变更  成功 (0x00000000)" 的文本。
    QString stageResultLine(const QString& stageName, const long status)
    {
        // 本协议为同步阶段，PENDING 初值不表示已经执行完成。
        const bool succeeded = (status == 0);
        if (succeeded)
        {
            return QStringLiteral("[阶段] %1  成功 (%2)")
                .arg(stageName, formatNtStatus(status));
        }
        if (!ksword::memory_result::attempted(status))
        {
            return QStringLiteral("[阶段] %1  未完成或未执行 (NTSTATUS=%2)")
                .arg(stageName, formatNtStatus(status));
        }
        return QStringLiteral("[阶段] %1  失败 (NTSTATUS=%2)")
            .arg(stageName, formatNtStatus(status));
    }
}

namespace ks::misc::hvci_bypass
{
    // ================================================================
    // queryCodeIntegrityPosture：委托 DisableDse 已有的双通道姿态查询。
    // ================================================================
    disable_dse::CodeIntegrityPosture queryCodeIntegrityPosture()
    {
        return disable_dse::queryPosture();
    }

    // ================================================================
    // enumerateKernelModules：
    // 通过 R3 NtQuerySystemInformation(SystemModuleInformation=11)
    // 枚举所有已加载的内核模块，供用户在 UI 中选择实验目标。
    // ================================================================
    ModuleEnumResult enumerateKernelModules()
    {
        // result：最终返回的枚举结果。
        ModuleEnumResult result;

        // ntQuery：NtQuerySystemInformation 函数指针。
        const NtQuerySystemInformationFn ntQuery = resolveNtQuery();
        if (ntQuery == nullptr)
        {
            result.errorText = QStringLiteral(
                "无法解析 NtQuerySystemInformation 函数地址");
            return result;
        }

        // 第一次调用：探测所需缓冲区大小。
        // required：R0 报告的实际缓冲区需求。
        unsigned long required = 0UL;
        ntQuery(kSystemModuleInformationClass, nullptr, 0UL, &required);
        if (required < sizeof(KernelModuleList))
        {
            // 兜底：至少分配 1 MB，防止返回值为零的边界情况。
            required = 1024UL * 1024UL;
        }

        // kMaxAttempts：缓冲区大小追赶的最大重试次数。
        constexpr int kMaxAttempts = 4;

        // lastStatus：记录最后一次失败的 NTSTATUS，用于错误报告。
        long lastStatus = 0;

        for (int attempt = 0; attempt < kMaxAttempts; ++attempt)
        {
            // buffer：查询缓冲区，在 required 基础上多留 64 KB 余量。
            std::vector<unsigned char> buffer(
                static_cast<std::size_t>(required) + 64UL * 1024UL, 0U);

            // returned：本次实际写回的字节数。
            unsigned long returned = 0UL;

            // status：NtQuerySystemInformation 的返回 NTSTATUS。
            const long status = ntQuery(
                kSystemModuleInformationClass,
                buffer.data(),
                static_cast<unsigned long>(buffer.size()),
                &returned);

            if (status == kStatusInfoLengthMismatch)
            {
                // 缓冲区不够，按返回值或 2 倍扩大后重试。
                required = std::max<unsigned long>(
                    returned,
                    static_cast<unsigned long>(buffer.size() * 2UL));
                lastStatus = status;
                continue;
            }
            if (status < 0)
            {
                // 枚举失败（通常是权限不足），携带错误信息返回。
                result.errorText = QStringLiteral(
                    "NtQuerySystemInformation 失败：0x%1")
                    .arg(static_cast<unsigned long>(status), 8, 16,
                         QLatin1Char('0'));
                return result;
            }

            // list：解析后的模块列表头。
            const auto* list =
                reinterpret_cast<const KernelModuleList*>(buffer.data());

            // maxRows：缓冲区最多容纳的行数，防止越界。
            const std::size_t maxRows =
                (buffer.size() - offsetof(KernelModuleList, rows))
                / sizeof(KernelModuleRow);

            // rowCount：实际要遍历的行数。
            const std::size_t rowCount =
                std::min<std::size_t>(list->count, maxRows);

            result.modules.reserve(rowCount);

            for (std::size_t i = 0; i < rowCount; ++i)
            {
                const KernelModuleRow& row = list->rows[i];

                // fullPath：模块的 NT 完整路径字符串。
                const auto* fullPath =
                    reinterpret_cast<const char*>(row.fullPathName);

                // pathLen：路径字符串的有效长度。
                const std::size_t pathLen =
                    ::strnlen_s(fullPath, sizeof(row.fullPathName));

                // ntPath：转为 QString 的完整路径。
                const QString ntPath =
                    QString::fromLocal8Bit(fullPath, static_cast<int>(pathLen));

                // nameOffset：文件名在完整路径中的起始位置。
                const int nameOffset =
                    std::min<int>(row.fileNameOffset, ntPath.size());

                // fileName：纯文件名（如 ntoskrnl.exe、CI.dll）。
                const QString fileName = ntPath.mid(nameOffset);

                // info：填充一条模块记录。
                KernelModuleInfo info;
                info.name = fileName;
                info.baseAddress = reinterpret_cast<std::uint64_t>(row.imageBase);
                info.imageSize = row.imageSize;

                result.modules.push_back(std::move(info));
            }

            // 枚举成功，标记并跳出重试循环。
            result.ok = true;
            return result;
        }

        // 重试耗尽仍未成功。
        result.errorText = QStringLiteral(
            "缓冲区重试 %1 次后仍不足（最后状态：0x%2）")
            .arg(kMaxAttempts)
            .arg(static_cast<unsigned long>(lastStatus), 8, 16,
                 QLatin1Char('0'));
        return result;
    }

    // ================================================================
    // runExperiment：
    // 向 R0 发送 IOCTL_KSWORD_ARK_CHANGE_IMAGE_PAGE_PROTECTION，
    // 执行完整的映像保护变更实验流程。
    // ================================================================
    ExperimentResult runExperiment(
        const std::uint64_t targetVa,
        const unsigned long flags,
        const std::uint64_t functionOverride)
    {
        // result：最终返回给 UI 的实验结果。
        ExperimentResult result;
        result.flags = flags;

        // ----------------------------------------------------------
        // 构建 IOCTL 请求。
        // operation 固定为 READONLY_NX：将 RX 页变为只读不可执行，
        // 这是 HVCI 实验的核心操作——测试 VTL1 是否拦截此变更。
        // ----------------------------------------------------------
        KSWORD_ARK_CHANGE_IMAGE_PAGE_PROTECTION_REQUEST request{};
        request.targetVirtualAddress = targetVa;
        request.operation = KSWORD_ARK_IMAGE_PROT_OP_READONLY_NX;
        request.flags = flags;
        request.functionAddressOverride = functionOverride;

        // response：R0 填充的响应，包含各阶段 NTSTATUS 和读回值。
        KSWORD_ARK_CHANGE_IMAGE_PAGE_PROTECTION_RESPONSE response{};

        // ----------------------------------------------------------
        // 通过 ArkDriverClient 发送 IOCTL。
        // ----------------------------------------------------------
        const ksword::ark::DriverClient client;
        const ksword::ark::IoResult io = client.deviceIoControl(
            IOCTL_KSWORD_ARK_CHANGE_IMAGE_PAGE_PROTECTION,
            &request,
            static_cast<unsigned long>(sizeof(request)),
            &response,
            static_cast<unsigned long>(sizeof(response)));

        // ----------------------------------------------------------
        // 填充 IOCTL 传输层结果。
        // ----------------------------------------------------------
        result.ioSuccess = io.ok;
        if (!io.ok)
        {
            // ioErrorText：IOCTL 传输失败时的错误描述。
            result.ioErrorText = QStringLiteral(
                "IOCTL 传输失败：Win32=%1 信息=%2")
                .arg(io.win32Error)
                .arg(QString::fromStdString(io.message));
            return result;
        }

        // 完整固定响应才有可解释的阶段和字节字段；传输成功不代替协议核验。
        if (io.bytesReturned < sizeof(response) || response.version != 1UL
            || response.size != sizeof(response))
        {
            result.ioSuccess = false;
            result.ioErrorText = QStringLiteral("实验响应不完整或版本不兼容。");
            return result;
        }

        // ----------------------------------------------------------
        // 转译各阶段 NTSTATUS。
        // ----------------------------------------------------------
        result.resolverStatus = response.resolverStatus;
        result.mdlAllocStatus = response.mdlAllocStatus;
        result.lockStatus = response.lockStatus;
        result.imageProtectStatus = response.imageProtectStatus;
        result.aliasMapStatus = response.aliasMapStatus;
        result.writeTestStatus = response.writeTestStatus;
        result.restoreStatus = response.restoreStatus;

        // ----------------------------------------------------------
        // 复制地址级字段。
        // ----------------------------------------------------------
        result.resolvedFunctionVa = response.resolvedFunctionVa;
        result.aliasVa = response.aliasVa;

        // ----------------------------------------------------------
        // 从 PTE 槽位提取单字节读回值。
        // R0 把字节读回值存在 PTE 命名的 8 字节字段的低位，
        // 这样不用为三个字节额外扩展协议结构。
        // ----------------------------------------------------------
        result.preModifyByte =
            static_cast<std::uint8_t>(response.originalPte & 0xFFULL);
        result.postWriteByte =
            static_cast<std::uint8_t>(response.afterImageProtectPte & 0xFFULL);
        result.postRestoreByte =
            static_cast<std::uint8_t>(response.afterRestorePte & 0xFFULL);
        // 先核对完整槽位和实际执行分支，不能把未执行零值或0xDEAD截成字节证据。
        const auto evidence = ksword::memory_result::imageByteEvidence(response, flags);
        result.hasPreModifyByte = evidence.before;
        result.hasPostWriteByte = evidence.afterWrite;
        result.hasPostRestoreByte = evidence.afterRestore;

        return result;
    }

    // ================================================================
    // verdictText：
    // 根据实验各阶段状态生成面向用户的多行可读判定文本。
    // ================================================================
    QString verdictText(const ExperimentResult& result)
    {
        // lines：逐行收集输出文本。
        QStringList lines;

        // ----------------------------------------------------------
        // 如果 IOCTL 传输层本身失败，直接返回错误。
        // ----------------------------------------------------------
        if (!result.ioSuccess)
        {
            lines.append(QStringLiteral("[失败] IOCTL 通信失败：%1")
                .arg(result.ioErrorText));
            return lines.join(QChar('\n'));
        }

        // ----------------------------------------------------------
        // 各阶段状态报告。
        // chainedStages：按驱动执行顺序排列的阶段列表。
        // 前一个阶段失败后，后续阶段标记"跳过"而非误报成功。
        // ----------------------------------------------------------
        struct StageEntry
        {
            QString name;
            long status;
            bool conditional;   // 是否有条件显示。
            bool show;          // conditional=true 时是否显示。
            bool breaksChain;   // 失败时是否中断后续阶段的"已执行"标记。
        };

        // stages：驱动按顺序执行的链式阶段。
        // breaksChain=false：驱动不以此阶段的返回值门控后续阶段。
        // MmChangeImageProtection 返回错误时，驱动仍继续执行别名映射和写入。
        const StageEntry stages[] = {
            { QStringLiteral("函数解析"), result.resolverStatus,
              false, true, true },
            { QStringLiteral("MDL 分配"), result.mdlAllocStatus,
              false, true, true },
            { QStringLiteral("页面锁定"), result.lockStatus,
              false, true, true },
            { QStringLiteral("映像保护变更"), result.imageProtectStatus,
              false, true, false },
            { QStringLiteral("别名映射"), result.aliasMapStatus,
              false, true, true },
            { QStringLiteral("写入测试"), result.writeTestStatus,
              true, (result.flags & kFlagWriteTest) != 0UL, true },
            { QStringLiteral("RX 恢复"), result.restoreStatus,
              true, (result.flags & kFlagRestore) != 0UL, true },
        };

        // chainBroken：前面某个阶段失败，后续阶段未被执行。
        bool chainBroken = false;
        for (const auto& stage : stages)
        {
            if (stage.conditional && !stage.show)
            {
                continue;
            }

            // 恢复独立于别名/写测试失败执行，不能被前面的链断裂掩盖。
            if (stage.name == QStringLiteral("RX 恢复"))
            {
                lines.append(stage.status == 0 ? stageResultLine(stage.name, stage.status)
                    : QStringLiteral("[阶段] %1  未完成或未执行 (NTSTATUS=%2)")
                        .arg(stage.name, formatNtStatus(stage.status)));
            }
            else if (chainBroken)
            {
                // 前置阶段已失败——本阶段未执行。
                lines.append(QStringLiteral("[阶段] %1  跳过（前置失败）")
                    .arg(stage.name));
            }
            else
            {
                lines.append(stageResultLine(stage.name, stage.status));
                // 仅在 breaksChain 阶段失败时，标记后续链断裂。
                if (stage.status < 0 && stage.breaksChain)
                {
                    chainBroken = true;
                }
            }

            // 特殊行：函数解析后显示 VA。
            if (stage.name == QStringLiteral("函数解析")
                && result.resolvedFunctionVa != 0ULL)
            {
                lines.append(QStringLiteral(
                    "         MmChangeImageProtection VA = 0x%1")
                    .arg(result.resolvedFunctionVa, 16, 16, QChar('0')));
            }

            // 特殊行：别名映射后显示别名 VA。
            if (stage.name == QStringLiteral("别名映射")
                && result.aliasVa != 0ULL)
            {
                lines.append(QStringLiteral(
                    "         别名 VA = 0x%1")
                    .arg(result.aliasVa, 16, 16, QChar('0')));
            }
        }

        // ----------------------------------------------------------
        // 字节读回值。
        // ----------------------------------------------------------
        lines.append(QString());
        lines.append(result.hasPreModifyByte
            ? QStringLiteral("[读回] 修改前字节 = 0x%1")
                .arg(result.preModifyByte, 2, 16, QChar('0'))
            : QStringLiteral("[读回] 修改前字节不可用（未读取或读取失败）"));

        if ((result.flags & kFlagWriteTest) != 0UL)
        {
            lines.append(result.hasPostWriteByte
                ? QStringLiteral("[读回] 写入后字节 = 0x%1")
                    .arg(result.postWriteByte, 2, 16, QChar('0'))
                : QStringLiteral("[读回] 写入后字节不可用（未读取或读取失败）"));
        }

        if ((result.flags & kFlagRestore) != 0UL)
        {
            lines.append(result.hasPostRestoreByte
                ? QStringLiteral("[读回] 恢复后字节 = 0x%1")
                    .arg(result.postRestoreByte, 2, 16, QChar('0'))
                : QStringLiteral("[读回] 恢复后字节不可用（未读取或读取失败）"));
        }

        // 请求的恢复没有完成或无有效回读时，不把任何后续判定包装成安全收尾。
        if ((result.flags & kFlagRestore) != 0UL
            && (result.restoreStatus != 0 || !result.hasPostRestoreByte))
        {
            lines.append(QStringLiteral("[结论] 恢复尚未证实完成（%1），请核对阶段状态并处理恢复。")
                .arg(formatNtStatus(result.restoreStatus)));
            return lines.join(QChar('\n'));
        }
        // 没有有效前后读数，不能比较默认值或据此宣称写入效果。
        if ((result.flags & kFlagWriteTest) != 0UL
            && (!result.hasPreModifyByte || !result.hasPostWriteByte))
        {
            lines.append(QStringLiteral("[结论] 字节证据不完整，不能判定写入效果。"));
            return lines.join(QChar('\n'));
        }

        // ----------------------------------------------------------
        // 综合判定。
        // 优先级：IOCTL 失败 > 签名扫描失败 > 映像保护被拒 >
        //          写入被拦截 > 写入静默丢弃 > 绕过确认。
        // ----------------------------------------------------------
        lines.append(QString());

        // requestedWriteTest：本次实验是否包含写入测试。
        const bool requestedWriteTest =
            (result.flags & kFlagWriteTest) != 0UL;

        // requestedRestore：本次实验是否包含 RX 恢复。
        const bool requestedRestore =
            (result.flags & kFlagRestore) != 0UL;

        // writeSucceeded：写入测试阶段的 NTSTATUS 是否成功。
        const bool writeSucceeded =
            requestedWriteTest && result.writeTestStatus >= 0;

        // expectedByte：写入测试使用 XOR 0xFF 反转原始字节。
        const auto expectedByte =
            static_cast<std::uint8_t>(result.preModifyByte ^ 0xFF);

        // byteActuallyChanged：写入后字节是否确实变成了反转值。
        const bool byteActuallyChanged =
            requestedWriteTest && (result.postWriteByte == expectedByte);

        // aliasWriteConfirmed：别名映射成功且 XOR 写入穿透到物理页。
        // 这是最终判定——无论 MmChangeImageProtection 是否成功。
        // 驱动在 imageProtectStatus 返回错误时仍继续执行别名映射，
        // 因此数据页（非 RX）目标也可能写入穿透。
        const bool aliasWriteConfirmed =
            result.aliasMapStatus >= 0
            && writeSucceeded
            && byteActuallyChanged;

        if (result.resolverStatus < 0)
        {
            // 签名扫描失败：该系统可能不存在 MmChangeImageProtection。
            lines.append(QStringLiteral(
                "[结论] MmChangeImageProtection 签名扫描失败（%1），"
                "该系统版本可能不支持此函数。")
                .arg(formatNtStatus(result.resolverStatus)));
        }
        else if (aliasWriteConfirmed)
        {
            // MDL 别名写入穿透确认。
            // MmChangeImageProtection 在数据页上会返回 0xc0000018，
            // 但别名映射和写入仍然穿透 HVCI 保护。
            QString suffix;
            if (result.imageProtectStatus < 0)
            {
                suffix = QStringLiteral(
                    "（MmChangeImageProtection 返回 %1，"
                    "目标可能是数据页而非 RX 代码页）")
                    .arg(formatNtStatus(result.imageProtectStatus));
            }

            // byteStillModified：恢复操作后修改字节是否仍保留。
            const bool byteStillModified =
                result.postRestoreByte == expectedByte;

            if (!requestedRestore)
            {
                lines.append(QStringLiteral(
                    "[结论] 写入穿透已确认：MDL 别名成功修改了内核映像页"
                    "内容（0x%1 → 0x%2）。未请求恢复。%3")
                    .arg(result.preModifyByte, 2, 16, QChar('0'))
                    .arg(expectedByte, 2, 16, QChar('0'))
                    .arg(suffix));
            }
            else if (byteStillModified)
            {
                lines.append(QStringLiteral(
                    "[结论] 本次 MDL 别名写入及恢复后字节已确认："
                    "恢复后修改仍然保留；HVCI 状态与执行权限需独立核验"
                    "（0x%1 → 0x%2）。%3")
                    .arg(result.preModifyByte, 2, 16, QChar('0'))
                    .arg(expectedByte, 2, 16, QChar('0'))
                    .arg(suffix));
            }
            else
            {
                lines.append(QStringLiteral(
                    "[结论] 写入穿透，但恢复后读回不一致"
                    "（期望 0x%1，实际 0x%2）。%3")
                    .arg(expectedByte, 2, 16, QChar('0'))
                    .arg(result.postRestoreByte, 2, 16, QChar('0'))
                    .arg(suffix));
            }
        }
        else if (result.imageProtectStatus < 0)
        {
            // 映像保护变更被拒绝且别名写入也未穿透。
            if (result.aliasMapStatus >= 0 && requestedWriteTest)
            {
                lines.append(QStringLiteral(
                    "[结论] MmChangeImageProtection 被拒绝（%1），"
                    "别名映射成功但写入未穿透目标字节。")
                    .arg(formatNtStatus(result.imageProtectStatus)));
            }
            else
            {
                lines.append(QStringLiteral(
                    "[结论] MmChangeImageProtection 被拒绝（%1），"
                    "别名写入未成功。")
                    .arg(formatNtStatus(result.imageProtectStatus)));
            }
        }
        else if (result.aliasMapStatus >= 0
            && requestedWriteTest && result.writeTestStatus < 0)
        {
            // 别名写入被拦截：保护变更成功但 VTL1 拒绝了写入。
            lines.append(QStringLiteral(
                "[结论] 别名写入被拦截（%1）。"
                "映像保护变更成功，但 VTL1 拒绝了通过别名的写入。")
                .arg(formatNtStatus(result.writeTestStatus)));
        }
        else if (result.aliasMapStatus >= 0
            && writeSucceeded && !byteActuallyChanged)
        {
            // 写入被静默丢弃：IOCTL 报告成功但字节未改变。
            lines.append(QStringLiteral(
                "[结论] 写入被静默丢弃：IOCTL 报告成功，但目标字节"
                "未改变（仍为 0x%1），Hypervisor 可能在 SLAT 层面拦截了写入。")
                .arg(result.postWriteByte, 2, 16, QChar('0')));
        }
        else if (result.resolverStatus < 0
            || result.mdlAllocStatus < 0
            || result.lockStatus < 0
            || result.aliasMapStatus < 0)
        {
            // 前置阶段失败，无法完成实验。
            lines.append(QStringLiteral(
                "[结论] 实验前置阶段失败，请根据各阶段状态判断原因。"));
        }
        else
        {
            // 其它未覆盖的组合。
            lines.append(QStringLiteral(
                "[结论] 实验已完成，请根据各阶段状态和读回值综合判断。"));
        }

        return lines.join(QChar('\n'));
    }

} // namespace ks::misc::hvci_bypass
