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

        // 各分支可能独立完成，直接报告实际回执，不从前一阶段推断后续未执行。
        lines.append(stageResultLine(QStringLiteral("函数解析"), result.resolverStatus));
        lines.append(stageResultLine(QStringLiteral("MDL 分配"), result.mdlAllocStatus));
        lines.append(stageResultLine(QStringLiteral("页面锁定"), result.lockStatus));
        lines.append(stageResultLine(QStringLiteral("映像保护变更"), result.imageProtectStatus));
        if ((result.flags & kFlagWriteTest) != 0UL)
        {
            lines.append(stageResultLine(QStringLiteral("别名映射"), result.aliasMapStatus));
            lines.append(stageResultLine(QStringLiteral("写入测试"), result.writeTestStatus));
        }
        if ((result.flags & kFlagRestore) != 0UL)
        {
            lines.append(stageResultLine(QStringLiteral("RX 恢复"), result.restoreStatus));
        }
        if (result.resolvedFunctionVa != 0ULL)
        {
            lines.append(QStringLiteral("         MmChangeImageProtection VA = 0x%1")
                .arg(result.resolvedFunctionVa, 16, 16, QChar('0')));
        }
        if (result.aliasVa != 0ULL)
        {
            lines.append(QStringLiteral("         别名 VA = 0x%1")
                .arg(result.aliasVa, 16, 16, QChar('0')));
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

        // 阶段结果与局部读回仅描述本次观测，不能推出整体安全保护状态。
        lines.append(QStringLiteral("[结论] 已报告各请求阶段的状态与有效读回；整体保护状态仍需独立核验。"));

        return lines.join(QChar('\n'));
    }

} // namespace ks::misc::hvci_bypass
