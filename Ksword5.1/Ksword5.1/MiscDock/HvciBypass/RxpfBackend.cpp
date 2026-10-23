// ============================================================
// RxpfBackend.cpp
// 作用：
// - 实现 RxpfBackend.h 声明的 10 个 IOCTL 包装函数
//   和 5 个文本格式化辅助函数；
// - 每个 IOCTL 函数构建请求体、通过 ArkDriverClient 发送、
//   将 R0 响应转译为面向 UI 的结构体；
// - 本文件不含 QWidget，UI 逻辑在 RxpfPage 中。
// ============================================================

#include "RxpfBackend.h"

// ArkDriverClient 提供 DriverClient、IoResult 和共享协议结构体。
#include "../../ArkDriverClient/ArkDriverClient.h"

// 诊断文件输出所需的 Qt 头文件。
#include <QFile>
#include <QCoreApplication>
#include <QDateTime>
#include <cstdio>

#include <QStringList>

// Framework.h 是主程序统一入口，拉入 Qt、日志和 ksword 工具链。
#include "../../Framework.h"

#include <cstring>

namespace
{
    // buildRequestHeader：
    // - 作用：填充 RXPF 请求公共头；
    // - confirmed：调用方是否已通过 UI 确认——变更操作必须显式传 true。
    // - extraFlags：调用方附加标志（如 CAPTURE_BACKUP）。
    KSWORD_ARK_RXPF_REQUEST_HEADER buildRequestHeader(
        const bool confirmed = true,
        const unsigned long extraFlags = 0UL)
    {
        KSWORD_ARK_RXPF_REQUEST_HEADER header{};
        header.version = KSWORD_ARK_RXPF_PROTOCOL_VERSION;
        header.size = sizeof(KSWORD_ARK_RXPF_REQUEST_HEADER);
        // flags：仅在调用方确认后才包含 UI_CONFIRMED 位。
        header.flags = extraFlags
            | (confirmed ? KSWORD_ARK_RXPF_FLAG_UI_CONFIRMED : 0UL);
        header.confirmationToken = KSWORD_ARK_RXPF_CONFIRMATION_TOKEN;
        return header;
    }

    // formatNtStatus：把 NTSTATUS 格式化成 "0x00000000" 形式。
    QString formatNtStatus(const long status)
    {
        return QStringLiteral("0x%1")
            .arg(static_cast<unsigned long>(status), 8, 16, QChar('0'));
    }

    // makeIoErrorText：构造 IOCTL 传输失败的错误文本。
    QString makeIoErrorText(const ksword::ark::IoResult& io)
    {
        return QStringLiteral("IOCTL 传输失败：Win32=%1 信息=%2")
            .arg(io.win32Error)
            .arg(QString::fromStdString(io.message));
    }

    // fillPageInfo：
    // - 作用：从 R0 的 PAGE_RESPONSE 结构转译到 UI 的 PageInfo；
    // - 统一用于 register/change/query/write/setEmulation/unregister。
    ks::misc::rxpf::PageInfo fillPageInfo(
        const KSWORD_ARK_RXPF_PAGE_RESPONSE& resp)
    {
        ks::misc::rxpf::PageInfo info;
        info.ioSuccess = true;
        info.state = resp.state;
        info.targetKind = resp.targetKind;
        info.flags = resp.flags;
        info.generation = resp.generation;
        info.referenceCount = resp.referenceCount;
        info.emulationEnabled = resp.emulationEnabled;
        info.recordId = resp.recordId;
        info.pageBase = resp.pageBase;
        info.writableAlias = resp.writableAlias;
        info.pfn = resp.pfn;
        info.ownerImageBase = resp.ownerImageBase;
        info.faultCount = resp.faultCount;
        info.emulatedCount = resp.emulatedCount;
        info.unsupportedCount = resp.unsupportedCount;
        info.lastStatus = resp.lastStatus;
        info.lastFailureReason = resp.lastFailureReason;
        info.originalProtection = resp.originalProtection;
        info.currentProtection = resp.currentProtection;
        info.writableAliasProtection = resp.writableAliasProtection;
        return info;
    }

    // 只把完整且兼容的响应解释成页面状态，传输成功仍不代表 lastStatus 成功。
    ks::misc::rxpf::PageInfo decodePageInfo(
        const ksword::ark::IoResult& io,
        const KSWORD_ARK_RXPF_PAGE_RESPONSE& response)
    {
        ks::misc::rxpf::PageInfo result;
        if (io.bytesReturned != sizeof(response)
            || response.version != KSWORD_ARK_RXPF_PROTOCOL_VERSION
            || response.size != sizeof(response))
        {
            result.ioErrorText = QStringLiteral("实验响应不完整或版本不兼容。");
            return result;
        }
        return fillPageInfo(response);
    }
}

namespace ks::misc::rxpf
{
    // ================================================================
    // querySupport：查询 RXPF 子系统能力与构建匹配状态。
    // ================================================================
    SupportInfo querySupport()
    {
        SupportInfo result;

        // request：查询请求只需公共头。
        auto header = buildRequestHeader();

        // response：R0 填充的支持状态响应。
        KSWORD_ARK_RXPF_QUERY_SUPPORT_RESPONSE response{};

        // client：通过 ArkDriverClient 发送 IOCTL。
        const ksword::ark::DriverClient client;
        const auto io = client.deviceIoControl(
            IOCTL_KSWORD_ARK_RXPF_QUERY_SUPPORT,
            &header,
            static_cast<unsigned long>(sizeof(header)),
            &response,
            static_cast<unsigned long>(sizeof(response)));

        if (!io.ok)
        {
            result.ioErrorText = makeIoErrorText(io);
            return result;
        }

        // 转译响应字段到 SupportInfo。
        result.ioSuccess = true;
        result.supportFlags = response.supportFlags;
        result.buildStatus = response.buildStatus;
        result.ntBuildNumber = response.ntBuildNumber;
        result.imageTimeDateStamp = response.imageTimeDateStamp;
        result.imageSize = response.imageSize;
        result.imageCheckSum = response.imageCheckSum;
        result.functionRva = response.functionRva;
        result.functionParameterCount = response.functionParameterCount;
        result.processorCount = response.processorCount;
        result.pdbAge = response.pdbAge;
        result.lastStatus = response.lastStatus;
        return result;
    }

    // ================================================================
    // registerPage：注册一个内核页面用于 #PF 模拟实验。
    // ================================================================
    PageInfo registerPage(
        const unsigned long targetKind,
        const std::uint64_t targetAddress)
    {
        PageInfo result;

        // request：注册请求体。
        KSWORD_ARK_RXPF_REGISTER_PAGE_REQUEST request{};
        request.header = buildRequestHeader(true, KSWORD_ARK_RXPF_FLAG_CAPTURE_BACKUP);
        request.header.size = sizeof(request);
        request.targetKind = targetKind;
        request.targetAddress = targetAddress;

        // response：R0 填充的页面状态响应。
        KSWORD_ARK_RXPF_PAGE_RESPONSE response{};

        const ksword::ark::DriverClient client;
        const auto io = client.deviceIoControl(
            IOCTL_KSWORD_ARK_RXPF_REGISTER_PAGE,
            &request,
            static_cast<unsigned long>(sizeof(request)),
            &response,
            static_cast<unsigned long>(sizeof(response)));

        // 诊断：记录 REGISTER 结果到文件。
        {
            char buf[512];
            std::snprintf(buf, sizeof(buf),
                "=== rxpf::registerPage %s ===\ntargetKind=%lu targetAddress=0x%llx sizeof=%zu\nok=%d win32=%lu bytesReturned=%lu\n",
                QDateTime::currentDateTime().toString(Qt::ISODate).toUtf8().constData(),
                targetKind, targetAddress, sizeof(request),
                static_cast<int>(io.ok),
                static_cast<unsigned long>(io.win32Error),
                static_cast<unsigned long>(io.bytesReturned));
            QByteArray msg(buf);
            if (io.ok) {
                std::snprintf(buf, sizeof(buf),
                    "response::state=%u recordId=%llu origProt=0x%x curProt=0x%x pageBase=0x%llx\n",
                    response.state, response.recordId,
                    response.originalProtection, response.currentProtection,
                    response.pageBase);
                msg += buf;
            }
            msg += '\n';
            QFile diagFile(QCoreApplication::applicationDirPath()
                + QStringLiteral("/rxpf_diag.log"));
            diagFile.open(QIODevice::Append | QIODevice::Text);
            diagFile.write(msg);
            diagFile.close();
        }

        if (!io.ok)
        {
            result.ioErrorText = makeIoErrorText(io);
            return result;
        }

        return decodePageInfo(io, response);
    }

    // ================================================================
    // changePage：将已注册页面的保护从 RX 改为 RW/NX。
    // ================================================================
    PageInfo changePage(const std::uint64_t recordId)
    {
        PageInfo result;

        // request：基于 recordId 的操作请求。
        KSWORD_ARK_RXPF_RECORD_REQUEST request{};
        request.header = buildRequestHeader();
        request.header.size = sizeof(request);
        request.recordId = recordId;

        // 诊断：把请求原始字节写到本地文件。
        {
            const auto* raw =
                reinterpret_cast<const unsigned char*>(&request);
            QByteArray hex;
            for (std::size_t i = 0; i < sizeof(request); ++i)
                hex += QByteArray::number(raw[i], 16).rightJustified(2, '0') + ' ';
            char buf[512];
            std::snprintf(buf, sizeof(buf),
                "=== rxpf::changePage %s ===\nsizeof=%zu IOCTL=0x%x recordId=%llu\nraw=[%s]\n",
                QDateTime::currentDateTime().toString(Qt::ISODate).toUtf8().constData(),
                sizeof(request),
                static_cast<unsigned>(IOCTL_KSWORD_ARK_RXPF_CHANGE_PAGE),
                recordId, hex.trimmed().constData());
            QFile diagFile(QCoreApplication::applicationDirPath()
                + QStringLiteral("/rxpf_diag.log"));
            diagFile.open(QIODevice::Append | QIODevice::Text);
            diagFile.write(buf);
            diagFile.close();
        }

        KSWORD_ARK_RXPF_PAGE_RESPONSE response{};

        const ksword::ark::DriverClient client;
        const auto io = client.deviceIoControl(
            IOCTL_KSWORD_ARK_RXPF_CHANGE_PAGE,
            &request,
            static_cast<unsigned long>(sizeof(request)),
            &response,
            static_cast<unsigned long>(sizeof(response)));

        // 诊断：追加 DeviceIoControl 结果到文件。
        {
            char buf[512];
            std::snprintf(buf, sizeof(buf),
                "rxpf::changePage result ok=%d win32=%lu bytesReturned=%lu sizeof=%zu\n",
                static_cast<int>(io.ok),
                static_cast<unsigned long>(io.win32Error),
                static_cast<unsigned long>(io.bytesReturned),
                sizeof(response));
            QByteArray msg(buf);
            if (io.ok) {
                std::snprintf(buf, sizeof(buf),
                    "response::state=%u lastStatus=0x%x origProt=0x%x curProt=0x%x alias=0x%llx\n",
                    response.state, response.lastStatus,
                    response.originalProtection, response.currentProtection,
                    response.writableAlias);
                msg += buf;
            }
            msg += '\n';
            QFile diagFile(QCoreApplication::applicationDirPath()
                + QStringLiteral("/rxpf_diag.log"));
            diagFile.open(QIODevice::Append | QIODevice::Text);
            diagFile.write(msg);
            diagFile.close();
        }

        if (!io.ok)
        {
            result.ioErrorText = makeIoErrorText(io);
            return result;
        }

        return decodePageInfo(io, response);
    }

    // ================================================================
    // queryPage：查询已注册页面的当前状态与计数器。
    // ================================================================
    PageInfo queryPage(const std::uint64_t recordId)
    {
        PageInfo result;

        KSWORD_ARK_RXPF_RECORD_REQUEST request{};
        request.header = buildRequestHeader();
        request.header.size = sizeof(request);
        request.recordId = recordId;

        KSWORD_ARK_RXPF_PAGE_RESPONSE response{};

        const ksword::ark::DriverClient client;
        const auto io = client.deviceIoControl(
            IOCTL_KSWORD_ARK_RXPF_QUERY_PAGE,
            &request,
            static_cast<unsigned long>(sizeof(request)),
            &response,
            static_cast<unsigned long>(sizeof(response)));

        if (!io.ok)
        {
            result.ioErrorText = makeIoErrorText(io);
            return result;
        }

        return decodePageInfo(io, response);
    }

    // ================================================================
    // writePage：向已注册页面的可写别名写入数据。
    // ================================================================
    PageInfo writePage(
        const std::uint64_t recordId,
        const unsigned long offset,
        const QByteArray& bytes)
    {
        PageInfo result;

        // request：写入请求体。
        KSWORD_ARK_RXPF_WRITE_PAGE_REQUEST request{};
        request.header = buildRequestHeader();
        request.header.size = sizeof(request);
        request.recordId = recordId;
        request.offset = offset;

        // length：实际写入长度，截断到协议上限 64 字节。
        const unsigned long length = static_cast<unsigned long>(
            std::min<int>(bytes.size(), KSWORD_ARK_RXPF_MAX_WRITE_BYTES));
        request.length = length;

        // 拷贝写入数据到请求体。
        std::memcpy(request.bytes, bytes.constData(), length);

        KSWORD_ARK_RXPF_PAGE_RESPONSE response{};

        const ksword::ark::DriverClient client;
        const auto io = client.deviceIoControl(
            IOCTL_KSWORD_ARK_RXPF_WRITE_PAGE,
            &request,
            static_cast<unsigned long>(sizeof(request)),
            &response,
            static_cast<unsigned long>(sizeof(response)));

        if (!io.ok)
        {
            result.ioErrorText = makeIoErrorText(io);
            return result;
        }

        result = decodePageInfo(io, response);
        result.requestedWriteLength = static_cast<std::uint64_t>(bytes.size());
        if (result.ioSuccess && result.lastStatus >= 0)
        {
            // 保留原来的单次64字节上限；显式报告实际范围，不能把截断当成完整写入。
            result.writtenLength = response.lastWriteLength;
        }
        return result;
    }

    // ================================================================
    // setEmulation：启用或禁用已注册页面的指令模拟。
    // ================================================================
    PageInfo setEmulation(
        const std::uint64_t recordId,
        const bool enable)
    {
        PageInfo result;

        // request：模拟控制请求体。
        KSWORD_ARK_RXPF_SET_EMULATION_REQUEST request{};
        request.header = buildRequestHeader();
        request.header.size = sizeof(request);
        request.recordId = recordId;
        // enable：1=启用，0=禁用。
        request.enable = enable ? 1UL : 0UL;

        KSWORD_ARK_RXPF_PAGE_RESPONSE response{};

        const ksword::ark::DriverClient client;
        const auto io = client.deviceIoControl(
            IOCTL_KSWORD_ARK_RXPF_SET_EMULATION,
            &request,
            static_cast<unsigned long>(sizeof(request)),
            &response,
            static_cast<unsigned long>(sizeof(response)));

        if (!io.ok)
        {
            result.ioErrorText = makeIoErrorText(io);
            return result;
        }

        return decodePageInfo(io, response);
    }

    // ================================================================
    // queryStats：查询 RXPF 全局统计信息。
    // ================================================================
    StatsInfo queryStats()
    {
        StatsInfo result;

        auto header = buildRequestHeader();

        // response：全局统计响应。
        KSWORD_ARK_RXPF_STATS_RESPONSE response{};

        const ksword::ark::DriverClient client;
        const auto io = client.deviceIoControl(
            IOCTL_KSWORD_ARK_RXPF_QUERY_STATS,
            &header,
            static_cast<unsigned long>(sizeof(header)),
            &response,
            static_cast<unsigned long>(sizeof(response)));

        if (!io.ok)
        {
            result.ioErrorText = makeIoErrorText(io);
            return result;
        }

        // 转译统计字段。
        result.ioSuccess = true;
        result.generation = response.generation;
        result.registeredPages = response.registeredPages;
        result.enabledPages = response.enabledPages;
        result.idtInstalled = response.idtInstalled;
        result.processorCount = response.processorCount;
        result.activeHandlers = response.activeHandlers;
        result.totalFaults = response.totalFaults;
        result.managedFaults = response.managedFaults;
        result.emulatedInstructions = response.emulatedInstructions;
        result.chainedFaults = response.chainedFaults;
        result.recursiveFaults = response.recursiveFaults;
        result.unsupportedInstructions = response.unsupportedInstructions;
        result.droppedEvents = response.droppedEvents;
        result.lastStatus = response.lastStatus;
        return result;
    }

    // ================================================================
    // drainEvents：拉取事件环形缓冲区中的模拟事件记录。
    // ================================================================
    EventsInfo drainEvents(
        const std::uint64_t afterSequence,
        const unsigned long maxRows)
    {
        EventsInfo result;

        // request：事件拉取请求体。
        KSWORD_ARK_RXPF_DRAIN_EVENTS_REQUEST request{};
        request.header = buildRequestHeader();
        request.header.size = sizeof(request);
        request.afterSequence = afterSequence;
        // maxRows：截断到协议上限 64 行。
        request.maxRows = std::min<unsigned long>(
            maxRows, KSWORD_ARK_RXPF_MAX_EVENT_ROWS);

        // response：事件拉取响应（含固定大小事件数组）。
        KSWORD_ARK_RXPF_DRAIN_EVENTS_RESPONSE response{};

        const ksword::ark::DriverClient client;
        const auto io = client.deviceIoControl(
            IOCTL_KSWORD_ARK_RXPF_DRAIN_EVENTS,
            &request,
            static_cast<unsigned long>(sizeof(request)),
            &response,
            static_cast<unsigned long>(sizeof(response)));

        if (!io.ok)
        {
            result.ioErrorText = makeIoErrorText(io);
            return result;
        }

        // 转译响应头。
        result.ioSuccess = true;
        result.returnedRows = response.returnedRows;
        result.availableRows = response.availableRows;
        result.newestSequence = response.newestSequence;
        result.droppedRows = response.droppedRows;

        // safeRows：实际可安全读取的行数。
        const unsigned long safeRows = std::min<unsigned long>(
            response.returnedRows, KSWORD_ARK_RXPF_MAX_EVENT_ROWS);

        result.rows.reserve(safeRows);

        // 逐行转译事件记录。
        for (unsigned long i = 0; i < safeRows; ++i)
        {
            const auto& src = response.rows[i];
            EventRow row;
            row.sequence = src.sequence;
            row.timestamp = src.timestamp;
            row.cr2 = src.cr2;
            row.rip = src.rip;
            row.errorCode = src.errorCode;
            row.recordId = src.recordId;
            row.newRip = src.newRip;
            row.processorIndex = src.processorIndex;
            row.decodedInstruction = src.decodedInstruction;
            row.emulationResult = src.emulationResult;
            row.status = src.status;
            result.rows.push_back(row);
        }

        return result;
    }

    // ================================================================
    // unregisterPage：注销已注册的页面并恢复原始保护。
    // skipBackupRestore=true 时附带 FLAG_SKIP_BACKUP_RESTORE，让 R0
    // ReleaseRecord 跳过 Backup → page 的自动恢复，保留 R3 写入字节
    // 直到 PTE 回 RX（Phase G 路径）。
    // ================================================================
    PageInfo unregisterPage(const std::uint64_t recordId,
        const bool skipBackupRestore)
    {
        PageInfo result;

        // 根据 skipBackupRestore 选择附加 flag：默认不加（走 Phase F 安全路径），
        // 显式打开时加 KSWORD_ARK_RXPF_FLAG_SKIP_BACKUP_RESTORE。
        const unsigned long extraFlags = skipBackupRestore
            ? KSWORD_ARK_RXPF_FLAG_SKIP_BACKUP_RESTORE
            : 0UL;

        KSWORD_ARK_RXPF_RECORD_REQUEST request{};
        request.header = buildRequestHeader(true, extraFlags);
        request.header.size = sizeof(request);
        request.recordId = recordId;

        KSWORD_ARK_RXPF_PAGE_RESPONSE response{};

        const ksword::ark::DriverClient client;
        const auto io = client.deviceIoControl(
            IOCTL_KSWORD_ARK_RXPF_UNREGISTER_PAGE,
            &request,
            static_cast<unsigned long>(sizeof(request)),
            &response,
            static_cast<unsigned long>(sizeof(response)));

        if (!io.ok)
        {
            result.ioErrorText = makeIoErrorText(io);
            return result;
        }

        return decodePageInfo(io, response);
    }

    // ================================================================
    // runSelfTest：运行驱动内置的端到端自检。
    // 调用方必须先 registerPage + changePage 使记录处于 RW_NX 状态。
    // ================================================================
    SelfTestInfo runSelfTest(const std::uint64_t recordId)
    {
        SelfTestInfo result;

        // request：自检需要完整的 RecordRequest（header + recordId）。
        KSWORD_ARK_RXPF_RECORD_REQUEST request{};
        request.header = buildRequestHeader();
        request.header.size = sizeof(request);
        request.recordId = recordId;

        // response：自检结果响应。
        KSWORD_ARK_RXPF_SELF_TEST_RESPONSE response{};

        const ksword::ark::DriverClient client;
        const auto io = client.deviceIoControl(
            IOCTL_KSWORD_ARK_RXPF_RUN_SELF_TEST,
            &request,
            static_cast<unsigned long>(sizeof(request)),
            &response,
            static_cast<unsigned long>(sizeof(response)));

        if (!io.ok)
        {
            result.ioErrorText = makeIoErrorText(io);
            return result;
        }

        // 转译自检结果。
        result.ioSuccess = true;
        result.result = response.result;
        result.instructionCount = response.instructionCount;
        result.recordId = response.recordId;
        result.returnedValue = response.returnedValue;
        result.expectedValue = response.expectedValue;
        result.faultsObserved = response.faultsObserved;
        result.lastStatus = response.lastStatus;
        return result;
    }

    // ================================================================
    // atomicCommit：原子提交——Register+Change(mode=2)+Write+Unregister(SKIP)
    // 在单次 IOCTL 内完成，把 NX 窗口压到驱动函数内部。
    // 仅支持 EXTERNAL_IMAGE 目标。
    // ================================================================
    PageInfo atomicCommit(
        const std::uint64_t targetAddress,
        const unsigned long offset,
        const QByteArray& bytes)
    {
        PageInfo result;

        // request：原子提交请求体。
        KSWORD_ARK_RXPF_ATOMIC_COMMIT_REQUEST request{};
        request.header = buildRequestHeader();
        request.header.size = sizeof(request);
        // targetKind：固定 EXTERNAL_IMAGE（原子提交唯一场景）。
        request.targetKind = KSWORD_ARK_RXPF_TARGET_EXTERNAL_IMAGE;
        request.reserved = 0UL;
        request.targetAddress = targetAddress;
        request.offset = offset;

        // length：实际写入长度，截断到协议上限 64 字节。
        const unsigned long length = static_cast<unsigned long>(
            std::min<int>(bytes.size(), KSWORD_ARK_RXPF_MAX_WRITE_BYTES));
        request.length = length;

        // 拷贝写入数据到请求体。
        std::memcpy(request.bytes, bytes.constData(), length);

        // response：复用 PAGE_RESPONSE（记录在 ReleaseRecord 后的最终状态）。
        KSWORD_ARK_RXPF_PAGE_RESPONSE response{};

        // client：通过 ArkDriverClient 发送 IOCTL。
        const ksword::ark::DriverClient client;
        const auto io = client.deviceIoControl(
            IOCTL_KSWORD_ARK_RXPF_ATOMIC_COMMIT,
            &request,
            static_cast<unsigned long>(sizeof(request)),
            &response,
            static_cast<unsigned long>(sizeof(response)));

        if (!io.ok)
        {
            result.ioErrorText = makeIoErrorText(io);
            return result;
        }

        return decodePageInfo(io, response);
    }

    // ================================================================
    // setHookPoint：设置或清除已注册页面上的执行钩点。
    // - confirmed：调用方必须在用户明确确认后传 true；
    //   R0 端在 confirmed=false 时拒绝该变更操作。
    // ================================================================
    HookPointInfo setHookPoint(
        const std::uint64_t recordId,
        const std::uint64_t hookAddress,
        const unsigned long hookAction,
        const std::uint64_t redirectTarget,
        const bool confirmed)
    {
        HookPointInfo result;

        // request：钩点设置请求体。
        KSWORD_ARK_RXPF_SET_HOOK_POINT_REQUEST request{};
        auto& hdr = request.header;
        hdr.version = KSWORD_ARK_RXPF_PROTOCOL_VERSION;
        // size：整个请求结构体的大小。
        hdr.size = sizeof(request);
        // flags：仅在调用方确认后才包含 UI_CONFIRMED 位。
        hdr.flags = confirmed ? KSWORD_ARK_RXPF_FLAG_UI_CONFIRMED : 0UL;
        hdr.confirmationToken = KSWORD_ARK_RXPF_CONFIRMATION_TOKEN;

        request.recordId = recordId;
        request.hookAddress = hookAddress;
        request.hookAction = hookAction;
        request.redirectTarget = redirectTarget;

        // response：钩点操作响应。
        KSWORD_ARK_RXPF_HOOK_POINT_RESPONSE response{};

        const ksword::ark::DriverClient client;
        const auto io = client.deviceIoControl(
            IOCTL_KSWORD_ARK_RXPF_SET_HOOK_POINT,
            &request,
            static_cast<unsigned long>(sizeof(request)),
            &response,
            static_cast<unsigned long>(sizeof(response)));

        if (!io.ok)
        {
            result.ioErrorText = makeIoErrorText(io);
            return result;
        }

        // 转译响应到 HookPointInfo。
        result.ioSuccess = true;
        result.recordId = response.recordId;
        result.hookAddress = response.hookAddress;
        result.hookAction = response.hookAction;
        result.state = response.state;
        result.redirectTarget = response.redirectTarget;
        result.redirectCount = response.redirectCount;
        result.lastStatus = response.lastStatus;
        return result;
    }

    // ================================================================
    // 文本格式化辅助函数
    // ================================================================

    // supportFlagsText：将支持标志位掩码翻译为逐行可读文本。
    QString supportFlagsText(const unsigned long flags)
    {
        // flagNames：位值到名称的映射表。
        static const struct { unsigned long bit; const char* name; } flagNames[] = {
            { KSWORD_ARK_RXPF_SUPPORT_INITIALIZED,          "已初始化" },
            { KSWORD_ARK_RXPF_SUPPORT_BUILD_MATCH,          "构建匹配" },
            { KSWORD_ARK_RXPF_SUPPORT_ABI_VERIFIED,         "ABI 已验证" },
            { KSWORD_ARK_RXPF_SUPPORT_ALLOCATED_TEST_PAGE,  "测试页已分配" },
            { KSWORD_ARK_RXPF_SUPPORT_SELF_IMAGE_TEST_PAGE, "自身映像测试页" },
            { KSWORD_ARK_RXPF_SUPPORT_IDT_SHADOW,           "影子 IDT 就绪" },
            { KSWORD_ARK_RXPF_SUPPORT_EMULATOR,             "模拟器就绪" },
            { KSWORD_ARK_RXPF_SUPPORT_EXTERNAL_IMAGE_COMPILED, "外部映像支持" },
            { KSWORD_ARK_RXPF_SUPPORT_IDT_INSTALLED,        "IDT 已安装" },
            { KSWORD_ARK_RXPF_SUPPORT_ALLOCATED_TEST_PASSED, "测试页自检通过" },
        };

        QStringList parts;
        for (const auto& [bit, name] : flagNames)
        {
            if ((flags & bit) != 0UL)
            {
                parts.append(QString::fromUtf8(name));
            }
        }

        if (parts.isEmpty())
        {
            return QStringLiteral("无（0x%1）")
                .arg(flags, 8, 16, QChar('0'));
        }

        return parts.join(QStringLiteral(" | "));
    }

    // buildStatusText：将构建匹配状态枚举翻译为可读文本。
    QString buildStatusText(const unsigned long status)
    {
        switch (status)
        {
        case KSWORD_ARK_RXPF_BUILD_STATUS_UNINITIALIZED:
            return QStringLiteral("未初始化");
        case KSWORD_ARK_RXPF_BUILD_STATUS_SUPPORTED:
            return QStringLiteral("支持");
        case KSWORD_ARK_RXPF_BUILD_STATUS_OS_BUILD_MISMATCH:
            return QStringLiteral("OS 构建不匹配");
        case KSWORD_ARK_RXPF_BUILD_STATUS_IMAGE_MISMATCH:
            return QStringLiteral("映像不匹配");
        case KSWORD_ARK_RXPF_BUILD_STATUS_RSDS_MISMATCH:
            return QStringLiteral("RSDS 不匹配");
        case KSWORD_ARK_RXPF_BUILD_STATUS_SIGNATURE_MISMATCH:
            return QStringLiteral("签名不匹配");
        case KSWORD_ARK_RXPF_BUILD_STATUS_SECTION_INVALID:
            return QStringLiteral("节信息无效");
        case KSWORD_ARK_RXPF_BUILD_STATUS_SYMBOL_UNAVAILABLE:
            return QStringLiteral("符号不可用");
        default:
            return QStringLiteral("未知(%1)").arg(status);
        }
    }

    // pageStateText：将页面状态枚举翻译为可读文本。
    QString pageStateText(const unsigned long state)
    {
        switch (state)
        {
        case KSWORD_ARK_RXPF_PAGE_STATE_EMPTY:
            return QStringLiteral("空");
        case KSWORD_ARK_RXPF_PAGE_STATE_RX:
            return QStringLiteral("RX（可执行）");
        case KSWORD_ARK_RXPF_PAGE_STATE_RW_NX:
            return QStringLiteral("RW/NX（可写不可执行）");
        case KSWORD_ARK_RXPF_PAGE_STATE_TERMINATING:
            return QStringLiteral("正在终止");
        case KSWORD_ARK_RXPF_PAGE_STATE_ERROR:
            return QStringLiteral("错误");
        default:
            return QStringLiteral("未知(%1)").arg(state);
        }
    }

    // decodedInstructionText：将指令类型枚举翻译为助记符。
    QString decodedInstructionText(const unsigned long instruction)
    {
        // instructionNames：枚举值到助记符的映射（0-16 连续，17 单独处理）。
        static const char* instructionNames[] = {
            "NONE", "NOP", "MOV", "LEA", "PUSH", "POP",
            "ADD", "SUB", "XOR", "AND", "OR",
            "CMP", "TEST", "JMP", "JCC", "CALL", "RET"
        };

        if (instruction < sizeof(instructionNames) / sizeof(instructionNames[0]))
        {
            return QString::fromLatin1(instructionNames[instruction]);
        }

        // KSWORD_ARK_RXPF_DECODE_HOOK_REDIRECT = 17，不连续，单独匹配。
        if (instruction == 17UL)
        {
            return QStringLiteral("HOOK_REDIRECT");
        }

        return QStringLiteral("0x%1").arg(instruction, 0, 16);
    }

    // emulationResultText：将模拟结果枚举翻译为可读文本。
    QString emulationResultText(const unsigned long result)
    {
        switch (result)
        {
        case KSWORD_ARK_RXPF_EMULATION_NONE:
            return QStringLiteral("无");
        case KSWORD_ARK_RXPF_EMULATION_SUCCESS:
            return QStringLiteral("成功");
        case KSWORD_ARK_RXPF_EMULATION_NOT_MANAGED:
            return QStringLiteral("非托管页");
        case KSWORD_ARK_RXPF_EMULATION_NOT_EXECUTE_FAULT:
            return QStringLiteral("非执行错误");
        case KSWORD_ARK_RXPF_EMULATION_USER_FAULT:
            return QStringLiteral("用户态错误");
        case KSWORD_ARK_RXPF_EMULATION_RECURSIVE:
            return QStringLiteral("递归");
        case KSWORD_ARK_RXPF_EMULATION_UNSUPPORTED_INSTRUCTION:
            return QStringLiteral("不支持的指令");
        case KSWORD_ARK_RXPF_EMULATION_CROSS_PAGE:
            return QStringLiteral("跨页");
        case KSWORD_ARK_RXPF_EMULATION_INVALID_ADDRESS:
            return QStringLiteral("无效地址");
        case KSWORD_ARK_RXPF_EMULATION_STACK_RANGE:
            return QStringLiteral("栈越界");
        case KSWORD_ARK_RXPF_EMULATION_TOPOLOGY_CHANGED:
            return QStringLiteral("拓扑变更");
        case KSWORD_ARK_RXPF_EMULATION_INTERNAL_ERROR:
            return QStringLiteral("内部错误");
        case KSWORD_ARK_RXPF_EMULATION_REDIRECTED:
            return QStringLiteral("已重定向");
        default:
            return QStringLiteral("未知(%1)").arg(result);
        }
    }

    // hookActionText：将钩点动作枚举翻译为可读文本。
    QString hookActionText(const unsigned long action)
    {
        switch (action)
        {
        case KSWORD_ARK_RXPF_HOOK_ACTION_NONE:
            return QStringLiteral("无");
        case KSWORD_ARK_RXPF_HOOK_ACTION_REDIRECT:
            return QStringLiteral("重定向");
        case KSWORD_ARK_RXPF_HOOK_ACTION_LOG:
            return QStringLiteral("记录");
        default:
            return QStringLiteral("未知(%1)").arg(action);
        }
    }

} // namespace ks::misc::rxpf

namespace ks::misc::rxpf
{
    // ================================================================
    // queryBackup：读 Record->Backup 的子段。
    // Phase G wizard 用它在 Unregister(SKIP_RESTORE) 前把原字节缓存到 R3，
    // 以便 payload 触发后能走 Register+Change+Write(backup)+Unregister 恢复。
    // ================================================================
    BackupInfo queryBackup(const std::uint64_t recordId,
        const unsigned long offset, const unsigned long length)
    {
        // 结果对象：失败时 bytes 保持空，调用方据 ioSuccess / hasBackup 判断。
        BackupInfo result;
        result.recordId = recordId;
        result.offset = offset;
        result.length = length;

        // 组装请求：header + recordId + offset + length。
        KSWORD_ARK_RXPF_QUERY_BACKUP_REQUEST request{};
        request.header = buildRequestHeader();
        request.header.size = sizeof(request);
        request.recordId = recordId;
        request.offset = offset;
        request.length = length;

        KSWORD_ARK_RXPF_QUERY_BACKUP_RESPONSE response{};

        // 发 IOCTL；失败路径只填 ioErrorText 返回。
        const ksword::ark::DriverClient client;
        const auto io = client.deviceIoControl(
            IOCTL_KSWORD_ARK_RXPF_QUERY_BACKUP,
            &request,
            static_cast<unsigned long>(sizeof(request)),
            &response,
            static_cast<unsigned long>(sizeof(response)));
        if (!io.ok)
        {
            result.ioErrorText = makeIoErrorText(io);
            return result;
        }

        // 成功：转译 R0 字段到 R3 结构。
        result.ioSuccess = true;
        result.recordId = response.recordId;
        result.offset = response.offset;
        result.length = response.length;
        result.hasBackup = response.hasBackup != 0UL;
        result.lastStatus = response.lastStatus;
        if (result.hasBackup && response.length > 0UL
            && response.length <= KSWORD_ARK_RXPF_MAX_WRITE_BYTES)
        {
            // 把 Backup 字节段作为 QByteArray 返回；避免 R3 直接触碰 C 数组。
            result.bytes = QByteArray(
                reinterpret_cast<const char*>(response.bytes),
                static_cast<int>(response.length));
        }
        return result;
    }
}

namespace ks::misc::rxpf
{
    // 只读回读使用现有ArkDriverClient，不增加新的驱动执行路径。
    ReadbackInfo readPageBytes(const std::uint64_t targetAddress,
        const unsigned long offset, const unsigned long length)
    {
        ReadbackInfo result;
        if (length == 0UL || length > KSWORD_ARK_RXPF_MAX_WRITE_BYTES
            || offset >= 4096UL || length > 4096UL - offset)
        {
            result.errorText = QStringLiteral("回读范围无效。");
            return result;
        }
        const std::uint64_t address = (targetAddress & ~0xFFFULL) + offset;
        const ksword::ark::DriverClient client;
        const auto read = client.readVirtualMemory(0U, address, length,
            KSWORD_ARK_MEMORY_READ_FLAG_KERNEL_ADDRESS);
        if (!read.io.ok || read.readStatus != KSWORD_ARK_MEMORY_READ_STATUS_OK
            || read.data.size() != length)
        {
            result.errorText = QStringLiteral("原字节回读失败，尚未证实恢复。");
            return result;
        }
        result.bytes = QByteArray(reinterpret_cast<const char*>(read.data.data()),
            static_cast<int>(read.data.size()));
        result.ok = true;
        return result;
    }
}
