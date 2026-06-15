#include "MonitorDock.h"

#include "../ArkDriverClient/ArkDriverClient.h"
#include "../UI/TableColumnAutoFit.h"
#include "../theme.h"

#include <QAbstractItemView>
#include <QCheckBox>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileDialog>
#include <QHeaderView>
#include <QHBoxLayout>
#include <QIcon>
#include <QIODevice>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QMetaObject>
#include <QPointer>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QRunnable>
#include <QSignalBlocker>
#include <QSplitter>
#include <QStringList>
#include <QTableWidget>
#include <QTableWidgetItem>
#include <QThreadPool>
#include <QVariant>
#include <QVBoxLayout>

#include <algorithm>
#include <cstdint>
#include <utility>
#include <vector>

namespace
{
    enum class RiskColumn : int { Score = 0, Source, Category, Title, Detail, Count };

    int riskColumnIndex(const RiskColumn column)
    {
        // Input: risk-center logical column. Processing: cast to Qt table column. Return: integer index.
        return static_cast<int>(column);
    }

    QString hex64(const std::uint64_t value)
    {
        // Input: 64-bit address/hash/mask. Processing: render fixed-width uppercase hex. Return: display text.
        return QStringLiteral("0x%1").arg(static_cast<qulonglong>(value), 16, 16, QChar('0')).toUpper();
    }

    QString hex32(const std::uint32_t value)
    {
        // Input: 32-bit flags/status. Processing: render fixed-width uppercase hex. Return: display text.
        return QStringLiteral("0x%1").arg(value, 8, 16, QChar('0')).toUpper();
    }

    QString wideText(const std::wstring& value)
    {
        // Input: ArkDriverClient wide string. Processing: convert to QString. Return: empty or display text.
        return value.empty() ? QString() : QString::fromStdWString(value);
    }

    QString narrowText(const std::string& value)
    {
        // Input: ArkDriverClient narrow string. Processing: convert to QString. Return: empty or display text.
        return value.empty() ? QString() : QString::fromStdString(value);
    }

    double clampScore(const double score)
    {
        // Input: arbitrary risk score. Processing: clamp to the risk-center range. Return: 0..100 score.
        return std::max(0.0, std::min(100.0, score));
    }

    QString scoreText(const double score)
    {
        // Input: normalized risk score. Processing: keep one decimal place. Return: sortable display text.
        return QString::number(clampScore(score), 'f', 1);
    }

    QString ioSummary(const ksword::ark::IoResult& io)
    {
        // Input: ArkDriverClient I/O result. Processing: compact transport/protocol diagnostics. Return: one line.
        return QStringLiteral("ok=%1 win32=%2 nt=%3 bytes=%4 %5")
            .arg(io.ok ? QStringLiteral("true") : QStringLiteral("false"))
            .arg(io.win32Error)
            .arg(hex32(static_cast<std::uint32_t>(io.ntStatus)))
            .arg(io.bytesReturned)
            .arg(QString::fromStdString(io.message));
    }

    QJsonObject payloadBase(const QString& source, const QString& category, const QString& title, const QString& detail, const double score)
    {
        // Input: common risk fields. Processing: build JSON root. Return: payload object for export/details.
        QJsonObject payload;
        payload.insert(QStringLiteral("source"), source);
        payload.insert(QStringLiteral("category"), category);
        payload.insert(QStringLiteral("title"), title);
        payload.insert(QStringLiteral("detail"), detail);
        payload.insert(QStringLiteral("riskScore"), clampScore(score));
        return payload;
    }

    MonitorDock::ArkRiskCenterEntry makeEntry(
        const QString& source,
        const QString& category,
        const QString& title,
        const QString& detail,
        const double score,
        QJsonObject payload)
    {
        // Input: display fields and structured payload. Processing: fill cache row. Return: risk-center entry.
        MonitorDock::ArkRiskCenterEntry entry;
        entry.sourceName = source;
        entry.category = category;
        entry.title = title;
        entry.detail = detail;
        entry.riskScore = clampScore(score);
        entry.riskScoreText = scoreText(entry.riskScore);
        payload.insert(QStringLiteral("source"), source);
        payload.insert(QStringLiteral("category"), category);
        payload.insert(QStringLiteral("title"), title);
        payload.insert(QStringLiteral("detail"), detail);
        payload.insert(QStringLiteral("riskScore"), entry.riskScore);
        entry.payload = std::move(payload);
        return entry;
    }

    class ScoreItem final : public QTableWidgetItem
    {
    public:
        explicit ScoreItem(const double score)
            : QTableWidgetItem(scoreText(score))
        {
            // Input: risk score. Processing: store numeric UserRole for sorting. Return: constructor has no return.
            setData(Qt::UserRole, clampScore(score));
            setTextAlignment(Qt::AlignVCenter | Qt::AlignRight);
        }

        bool operator<(const QTableWidgetItem& other) const override
        {
            // Input: another table item. Processing: prefer numeric UserRole comparison. Return: sort decision.
            bool leftOk = false;
            bool rightOk = false;
            const double left = data(Qt::UserRole).toDouble(&leftOk);
            const double right = other.data(Qt::UserRole).toDouble(&rightOk);
            return (leftOk && rightOk) ? left < right : QTableWidgetItem::operator<(other);
        }
    };

    QTableWidgetItem* textItem(const QString& value)
    {
        // Input: display text. Processing: create read-only table item. Return: item owned by QTableWidget.
        QTableWidgetItem* item = new QTableWidgetItem(value);
        item->setTextAlignment(Qt::AlignVCenter | Qt::AlignLeft);
        return item;
    }

    QString csvEscape(const QString& value)
    {
        // Input: CSV field. Processing: quote when needed and double embedded quotes. Return: CSV-safe text.
        QString escaped = value;
        escaped.replace(QChar('"'), QStringLiteral("\"\""));
        if (escaped.contains(QChar(',')) || escaped.contains(QChar('"')) || escaped.contains(QChar('\n')) || escaped.contains(QChar('\r')))
        {
            escaped = QStringLiteral("\"%1\"").arg(escaped);
        }
        return escaped;
    }

    bool matchesFilter(const MonitorDock::ArkRiskCenterEntry& entry, const QString& filter)
    {
        // Input: cache row and filter text. Processing: case-insensitive scan across display fields and JSON. Return: match flag.
        if (filter.isEmpty())
        {
            return true;
        }
        const QString payloadText = QString::fromUtf8(QJsonDocument(entry.payload).toJson(QJsonDocument::Compact));
        const QStringList fields{ entry.sourceName, entry.category, entry.title, entry.detail, entry.riskScoreText, payloadText };
        for (const QString& field : fields)
        {
            if (field.contains(filter, Qt::CaseInsensitive))
            {
                return true;
            }
        }
        return false;
    }

    void addStatusEntry(std::vector<MonitorDock::ArkRiskCenterEntry>& entries, QStringList& statusLines, const QString& source, const QString& category, const bool ok, const bool unsupported, const ksword::ark::IoResult& io)
    {
        // Input: one read-only query status. Processing: append status text and optional low-risk failure row. Return: no value.
        const QString summary = unsupported ? QStringLiteral("%1: 未集成/驱动过旧/能力缺失").arg(source) : QStringLiteral("%1: %2").arg(source, ioSummary(io));
        statusLines << summary;
        if (ok)
        {
            return;
        }
        const double risk = unsupported ? 0.0 : 5.0;
        QJsonObject payload = payloadBase(source, category, unsupported ? QStringLiteral("等待 R0 支持") : QStringLiteral("查询失败"), summary, risk);
        payload.insert(QStringLiteral("unsupported"), unsupported);
        payload.insert(QStringLiteral("win32Error"), static_cast<int>(io.win32Error));
        payload.insert(QStringLiteral("ntStatus"), hex32(static_cast<std::uint32_t>(io.ntStatus)));
        entries.push_back(makeEntry(source, category, unsupported ? QStringLiteral("未集成/驱动过旧") : QStringLiteral("查询失败"), summary, risk, payload));
    }

    double memoryScore(const ksword::ark::KernelMemoryEvidenceEntry& row)
    {
        // Input: memory evidence row. Processing: score RWX/non-module/pool/text risks and confidence. Return: risk score.
        double score = 0.0;
        if (row.riskFlags & KSWORD_ARK_MEMORY_EVIDENCE_RISK_RWX) score += 35.0;
        if (row.riskFlags & KSWORD_ARK_MEMORY_EVIDENCE_RISK_NONMODULE_EXECUTABLE) score += 35.0;
        if (row.riskFlags & KSWORD_ARK_MEMORY_EVIDENCE_RISK_MODULE_NON_TEXT_EXECUTABLE) score += 25.0;
        if (row.riskFlags & KSWORD_ARK_MEMORY_EVIDENCE_RISK_EXECUTABLE_POOL) score += 25.0;
        if (row.riskFlags & KSWORD_ARK_MEMORY_EVIDENCE_RISK_LARGE_EXECUTABLE) score += 12.0;
        if (row.riskFlags & KSWORD_ARK_MEMORY_EVIDENCE_RISK_OWNER_MISSING) score += 12.0;
        score += std::min<double>(15.0, static_cast<double>(row.confidence) / 10.0);
        return clampScore(score);
    }

    QString memoryRiskText(const std::uint32_t flags)
    {
        // Input: memory evidence risk bits. Processing: map known bits. Return: compact text.
        QStringList parts;
        if (flags & KSWORD_ARK_MEMORY_EVIDENCE_RISK_RWX) parts << QStringLiteral("RWX");
        if (flags & KSWORD_ARK_MEMORY_EVIDENCE_RISK_NONMODULE_EXECUTABLE) parts << QStringLiteral("非模块执行");
        if (flags & KSWORD_ARK_MEMORY_EVIDENCE_RISK_MODULE_NON_TEXT_EXECUTABLE) parts << QStringLiteral("模块非text执行");
        if (flags & KSWORD_ARK_MEMORY_EVIDENCE_RISK_EXECUTABLE_POOL) parts << QStringLiteral("执行池");
        if (flags & KSWORD_ARK_MEMORY_EVIDENCE_RISK_LARGE_EXECUTABLE) parts << QStringLiteral("大页执行");
        if (flags & KSWORD_ARK_MEMORY_EVIDENCE_RISK_OWNER_MISSING) parts << QStringLiteral("Owner缺失");
        return parts.isEmpty() ? QStringLiteral("正常") : parts.join(QStringLiteral(" | "));
    }

    double crossViewScore(const std::uint32_t flags, const std::uint32_t confidence)
    {
        // Input: cross-view anomaly bits and confidence. Processing: score DKOM-style mismatches. Return: risk score.
        double score = 0.0;
        if (flags & KSWORD_ARK_CROSSVIEW_ANOMALY_MISSING_FROM_ACTIVE_LIST) score += 40.0;
        if (flags & KSWORD_ARK_CROSSVIEW_ANOMALY_MISSING_FROM_CID_TABLE) score += 35.0;
        if (flags & KSWORD_ARK_CROSSVIEW_ANOMALY_THREAD_ORPHAN) score += 35.0;
        if (flags & KSWORD_ARK_CROSSVIEW_ANOMALY_CID_ONLY) score += 30.0;
        if (flags & KSWORD_ARK_CROSSVIEW_ANOMALY_START_ADDRESS_OUTSIDE_MODULE) score += 30.0;
        if (flags & KSWORD_ARK_CROSSVIEW_ANOMALY_DANGLING_OBJECT) score += 30.0;
        if (flags & KSWORD_ARK_CROSSVIEW_ANOMALY_THREAD_NOT_IN_PROCESS_LIST) score += 28.0;
        if (flags & KSWORD_ARK_CROSSVIEW_ANOMALY_ACTIVE_ONLY) score += 20.0;
        score += std::min<double>(15.0, static_cast<double>(confidence) / 10.0);
        return clampScore(score);
    }

    QString crossViewText(const std::uint32_t flags)
    {
        // Input: cross-view anomaly bits. Processing: map known DKOM indicators. Return: compact text.
        QStringList parts;
        if (flags & KSWORD_ARK_CROSSVIEW_ANOMALY_CID_ONLY) parts << QStringLiteral("CID-only");
        if (flags & KSWORD_ARK_CROSSVIEW_ANOMALY_ACTIVE_ONLY) parts << QStringLiteral("Active-only");
        if (flags & KSWORD_ARK_CROSSVIEW_ANOMALY_MISSING_FROM_ACTIVE_LIST) parts << QStringLiteral("缺ActiveList");
        if (flags & KSWORD_ARK_CROSSVIEW_ANOMALY_MISSING_FROM_CID_TABLE) parts << QStringLiteral("缺CID");
        if (flags & KSWORD_ARK_CROSSVIEW_ANOMALY_THREAD_ORPHAN) parts << QStringLiteral("孤儿线程");
        if (flags & KSWORD_ARK_CROSSVIEW_ANOMALY_THREAD_NOT_IN_PROCESS_LIST) parts << QStringLiteral("线程进程缺失");
        if (flags & KSWORD_ARK_CROSSVIEW_ANOMALY_START_ADDRESS_OUTSIDE_MODULE) parts << QStringLiteral("入口出模块");
        if (flags & KSWORD_ARK_CROSSVIEW_ANOMALY_DANGLING_OBJECT) parts << QStringLiteral("悬空对象");
        return parts.isEmpty() ? QStringLiteral("正常") : parts.join(QStringLiteral(" | "));
    }

    double driverScore(const std::uint32_t flags, const std::uint32_t confidence)
    {
        // Input: driver integrity risk bits and confidence. Processing: score CPU/IDT/owner/dispatch risks. Return: risk score.
        double score = 0.0;
        if (flags & KSWORD_ARK_DRIVER_INTEGRITY_RISK_IDT_NON_CORE_OWNER) score += 40.0;
        if (flags & KSWORD_ARK_DRIVER_INTEGRITY_RISK_OWNER_MISMATCH) score += 35.0;
        if (flags & KSWORD_ARK_DRIVER_INTEGRITY_RISK_OUTSIDE_DRIVER_IMAGE) score += 35.0;
        if (flags & KSWORD_ARK_DRIVER_INTEGRITY_RISK_CPU_WP_DISABLED) score += 35.0;
        if (flags & KSWORD_ARK_DRIVER_INTEGRITY_RISK_DESCRIPTOR_INVALID) score += 25.0;
        if (flags & KSWORD_ARK_DRIVER_INTEGRITY_RISK_CPU_NXE_DISABLED) score += 25.0;
        if (flags & KSWORD_ARK_DRIVER_INTEGRITY_RISK_CPU_SMEP_DISABLED) score += 22.0;
        if (flags & KSWORD_ARK_DRIVER_INTEGRITY_RISK_SECTION_MISMATCH) score += 18.0;
        if (flags & KSWORD_ARK_DRIVER_INTEGRITY_RISK_CROSS_DRIVER_ATTACH) score += 18.0;
        if (flags & KSWORD_ARK_DRIVER_INTEGRITY_RISK_CPU_SMAP_DISABLED) score += 18.0;
        score += std::min<double>(15.0, static_cast<double>(confidence) / 10.0);
        return clampScore(score);
    }

    QString driverRiskText(const std::uint32_t flags)
    {
        // Input: driver integrity risk bits. Processing: map high-value known bits. Return: compact text.
        QStringList parts;
        if (flags & KSWORD_ARK_DRIVER_INTEGRITY_RISK_OWNER_MISMATCH) parts << QStringLiteral("Owner不匹配");
        if (flags & KSWORD_ARK_DRIVER_INTEGRITY_RISK_OUTSIDE_DRIVER_IMAGE) parts << QStringLiteral("外跳");
        if (flags & KSWORD_ARK_DRIVER_INTEGRITY_RISK_IDT_NON_CORE_OWNER) parts << QStringLiteral("IDT外部Owner");
        if (flags & KSWORD_ARK_DRIVER_INTEGRITY_RISK_CPU_WP_DISABLED) parts << QStringLiteral("WP关闭");
        if (flags & KSWORD_ARK_DRIVER_INTEGRITY_RISK_CPU_NXE_DISABLED) parts << QStringLiteral("NXE关闭");
        if (flags & KSWORD_ARK_DRIVER_INTEGRITY_RISK_CPU_SMEP_DISABLED) parts << QStringLiteral("SMEP关闭");
        if (flags & KSWORD_ARK_DRIVER_INTEGRITY_RISK_CPU_SMAP_DISABLED) parts << QStringLiteral("SMAP关闭");
        if (flags & KSWORD_ARK_DRIVER_INTEGRITY_RISK_DESCRIPTOR_INVALID) parts << QStringLiteral("描述符异常");
        if (flags & KSWORD_ARK_DRIVER_INTEGRITY_RISK_SECTION_MISMATCH) parts << QStringLiteral("Section不匹配");
        if (flags & KSWORD_ARK_DRIVER_INTEGRITY_RISK_CROSS_DRIVER_ATTACH) parts << QStringLiteral("跨驱动挂接");
        if (flags & KSWORD_ARK_DRIVER_INTEGRITY_RISK_MODULE_UNRESOLVED) parts << QStringLiteral("模块未解析");
        if (flags & KSWORD_ARK_DRIVER_INTEGRITY_RISK_DYNDATA_UNAVAILABLE) parts << QStringLiteral("DynData缺失");
        return parts.isEmpty() ? hex32(flags) : parts.join(QStringLiteral(" | "));
    }

    QString hookStatusText(const std::uint32_t status)
    {
        // Input: kernel hook status. Processing: map known status codes. Return: display text.
        switch (status)
        {
        case KSWORD_ARK_KERNEL_HOOK_STATUS_CLEAN: return QStringLiteral("Clean");
        case KSWORD_ARK_KERNEL_HOOK_STATUS_SUSPICIOUS: return QStringLiteral("Suspicious");
        case KSWORD_ARK_KERNEL_HOOK_STATUS_INTERNAL_BRANCH: return QStringLiteral("InternalBranch");
        case KSWORD_ARK_KERNEL_HOOK_STATUS_READ_FAILED: return QStringLiteral("ReadFailed");
        case KSWORD_ARK_KERNEL_HOOK_STATUS_PARSE_FAILED: return QStringLiteral("ParseFailed");
        case KSWORD_ARK_KERNEL_HOOK_STATUS_FORCE_REQUIRED: return QStringLiteral("ForceRequired");
        default: return QStringLiteral("Status(%1)").arg(status);
        }
    }

    double hookScore(const std::uint32_t status, const bool hasPatchOrDiff)
    {
        // Input: hook status and patch/diff presence. Processing: score suspicious hook evidence. Return: risk score.
        double score = hasPatchOrDiff ? 25.0 : 0.0;
        if (status == KSWORD_ARK_KERNEL_HOOK_STATUS_SUSPICIOUS) score += 60.0;
        if (status == KSWORD_ARK_KERNEL_HOOK_STATUS_FORCE_REQUIRED) score += 55.0;
        if (status == KSWORD_ARK_KERNEL_HOOK_STATUS_READ_FAILED) score += 15.0;
        if (status == KSWORD_ARK_KERNEL_HOOK_STATUS_PARSE_FAILED) score += 15.0;
        if (status == KSWORD_ARK_KERNEL_HOOK_STATUS_INTERNAL_BRANCH) score += 8.0;
        return clampScore(score);
    }

    QString callbackClassText(const std::uint32_t callbackClass)
    {
        // Input: callback class id. Processing: map shared protocol constants. Return: display text.
        switch (callbackClass)
        {
        case KSWORD_ARK_CALLBACK_ENUM_CLASS_REGISTRY: return QStringLiteral("Registry");
        case KSWORD_ARK_CALLBACK_ENUM_CLASS_PROCESS: return QStringLiteral("Process");
        case KSWORD_ARK_CALLBACK_ENUM_CLASS_THREAD: return QStringLiteral("Thread");
        case KSWORD_ARK_CALLBACK_ENUM_CLASS_IMAGE: return QStringLiteral("Image");
        case KSWORD_ARK_CALLBACK_ENUM_CLASS_OBJECT: return QStringLiteral("Object");
        case KSWORD_ARK_CALLBACK_ENUM_CLASS_MINIFILTER: return QStringLiteral("Minifilter");
        case KSWORD_ARK_CALLBACK_ENUM_CLASS_WFP_CALLOUT: return QStringLiteral("WFP");
        case KSWORD_ARK_CALLBACK_ENUM_CLASS_ETW_PROVIDER: return QStringLiteral("ETW");
        default: return QStringLiteral("Callback(%1)").arg(callbackClass);
        }
    }

    double callbackScore(const ksword::ark::CallbackEnumEntry& row)
    {
        // Input: callback enumeration row. Processing: score private/unresolved/untrusted rows. Return: risk score.
        double score = 0.0;
        if (row.status == KSWORD_ARK_CALLBACK_ENUM_STATUS_QUERY_FAILED) score += 20.0;
        if (row.source == KSWORD_ARK_CALLBACK_ENUM_SOURCE_PRIVATE_UNSUPPORTED ||
            row.source == KSWORD_ARK_CALLBACK_ENUM_SOURCE_PRIVATE_PATTERN_SCAN ||
            row.source == KSWORD_ARK_CALLBACK_ENUM_SOURCE_PRIVATE_NOTIFY_ARRAY ||
            row.source == KSWORD_ARK_CALLBACK_ENUM_SOURCE_PRIVATE_REGISTRY_LIST ||
            row.source == KSWORD_ARK_CALLBACK_ENUM_SOURCE_PRIVATE_OBJECT_TYPE_LIST)
        {
            score += 25.0;
        }
        if (row.moduleBase == 0U && row.callbackAddress != 0U) score += 30.0;
        if ((row.fieldFlags & KSWORD_ARK_CALLBACK_ENUM_FIELD_MODULE) == 0U) score += 15.0;
        if ((row.trustFlags & (KSWORD_ARK_CALLBACK_TRUST_REVALIDATED | KSWORD_ARK_CALLBACK_TRUST_PUBLIC_API | KSWORD_ARK_CALLBACK_TRUST_PDB_PROFILE)) == 0U) score += 12.0;
        return clampScore(score);
    }

    QString mutationOperationText(const std::uint32_t operation)
    {
        // Input: mutation operation id. Processing: map known audit operation. Return: display text.
        switch (operation)
        {
        case KSWORD_ARK_MUTATION_OPERATION_PREPARE: return QStringLiteral("Prepare");
        case KSWORD_ARK_MUTATION_OPERATION_COMMIT: return QStringLiteral("Commit");
        case KSWORD_ARK_MUTATION_OPERATION_ROLLBACK: return QStringLiteral("Rollback");
        case KSWORD_ARK_MUTATION_OPERATION_QUERY_AUDIT: return QStringLiteral("QueryAudit");
        default: return QStringLiteral("Operation(%1)").arg(operation);
        }
    }

    QString mutationStatusText(const std::uint32_t status)
    {
        // Input: mutation status id. Processing: map common audit status. Return: display text.
        switch (status)
        {
        case KSWORD_ARK_MUTATION_STATUS_PREPARED: return QStringLiteral("Prepared");
        case KSWORD_ARK_MUTATION_STATUS_DRY_RUN: return QStringLiteral("DryRun");
        case KSWORD_ARK_MUTATION_STATUS_COMMITTED: return QStringLiteral("Committed");
        case KSWORD_ARK_MUTATION_STATUS_ROLLED_BACK: return QStringLiteral("RolledBack");
        case KSWORD_ARK_MUTATION_STATUS_REJECTED_SAFETY_POLICY: return QStringLiteral("RejectedSafetyPolicy");
        case KSWORD_ARK_MUTATION_STATUS_REJECTED_BEFORE_MISMATCH: return QStringLiteral("RejectedBeforeMismatch");
        case KSWORD_ARK_MUTATION_STATUS_REJECTED_TARGET_CHANGED: return QStringLiteral("RejectedTargetChanged");
        case KSWORD_ARK_MUTATION_STATUS_WRITE_FAILED: return QStringLiteral("WriteFailed");
        case KSWORD_ARK_MUTATION_STATUS_READ_FAILED: return QStringLiteral("ReadFailed");
        default: return QStringLiteral("Status(%1)").arg(status);
        }
    }

    double mutationScore(const ksword::ark::MutationAuditEntry& row)
    {
        // Input: mutation audit row. Processing: score commit/write-fail/rejected changes; dry-run lowers score. Return: risk score.
        double score = 0.0;
        if (row.operation == KSWORD_ARK_MUTATION_OPERATION_COMMIT) score += 45.0;
        if (row.status == KSWORD_ARK_MUTATION_STATUS_COMMITTED) score += 30.0;
        if (row.status == KSWORD_ARK_MUTATION_STATUS_WRITE_FAILED) score += 25.0;
        if (row.status == KSWORD_ARK_MUTATION_STATUS_REJECTED_TARGET_CHANGED) score += 18.0;
        if (row.status == KSWORD_ARK_MUTATION_STATUS_REJECTED_BEFORE_MISMATCH) score += 16.0;
        if (row.operation == KSWORD_ARK_MUTATION_OPERATION_ROLLBACK) score += 12.0;
        if (row.riskFlags != 0U) score += 20.0;
        if ((row.flags & KSWORD_ARK_MUTATION_FLAG_DRY_RUN) != 0U) score -= 20.0;
        return clampScore(score);
    }

    void appendMemory(std::vector<MonitorDock::ArkRiskCenterEntry>& entries, const ksword::ark::KernelMemoryEvidenceResult& result)
    {
        // Input: memory evidence result. Processing: append only non-zero risk rows. Return: no value.
        for (const auto& row : result.entries)
        {
            if (row.riskFlags == 0U) continue;
            const double risk = memoryScore(row);
            const QString title = QStringLiteral("%1 %2").arg(memoryRiskText(row.riskFlags), hex64(row.virtualAddress));
            const QString detail = QStringLiteral("owner=%1 size=%2 confidence=%3 %4")
                .arg(wideText(row.ownerName), hex64(row.regionSize)).arg(row.confidence).arg(wideText(row.detail));
            QJsonObject payload = payloadBase(QStringLiteral("Memory Evidence"), QStringLiteral("Memory"), title, detail, risk);
            payload.insert(QStringLiteral("virtualAddress"), hex64(row.virtualAddress));
            payload.insert(QStringLiteral("regionSize"), hex64(row.regionSize));
            payload.insert(QStringLiteral("riskFlags"), hex32(row.riskFlags));
            payload.insert(QStringLiteral("permissionFlags"), hex32(row.permissionFlags));
            payload.insert(QStringLiteral("ownerName"), wideText(row.ownerName));
            payload.insert(QStringLiteral("contentHash"), hex64(row.contentHash));
            entries.push_back(makeEntry(QStringLiteral("Memory Evidence"), QStringLiteral("Memory"), title, detail, risk, payload));
        }
    }

    void appendProcessCrossView(std::vector<MonitorDock::ArkRiskCenterEntry>& entries, const ksword::ark::ProcessCrossViewResult& result)
    {
        // Input: process cross-view result. Processing: append anomalous process rows. Return: no value.
        for (const auto& row : result.entries)
        {
            if (row.anomalyFlags == 0U) continue;
            const double risk = crossViewScore(row.anomalyFlags, row.confidence);
            const QString title = QStringLiteral("PID %1 %2").arg(row.processId).arg(crossViewText(row.anomalyFlags));
            const QString detail = QStringLiteral("image=%1 object=%2 source=%3 %4")
                .arg(narrowText(row.imageName), hex64(row.objectAddress), hex32(row.sourceMask), narrowText(row.detail));
            QJsonObject payload = payloadBase(QStringLiteral("Process Cross-View"), QStringLiteral("Process"), title, detail, risk);
            payload.insert(QStringLiteral("processId"), static_cast<int>(row.processId));
            payload.insert(QStringLiteral("imageName"), narrowText(row.imageName));
            payload.insert(QStringLiteral("objectAddress"), hex64(row.objectAddress));
            payload.insert(QStringLiteral("anomalyFlags"), hex32(row.anomalyFlags));
            payload.insert(QStringLiteral("sourceMask"), hex32(row.sourceMask));
            entries.push_back(makeEntry(QStringLiteral("Process Cross-View"), QStringLiteral("Process"), title, detail, risk, payload));
        }
    }

    void appendThreadCrossView(std::vector<MonitorDock::ArkRiskCenterEntry>& entries, const ksword::ark::ThreadCrossViewResult& result)
    {
        // Input: thread cross-view result. Processing: append anomalous thread rows. Return: no value.
        for (const auto& row : result.entries)
        {
            if (row.anomalyFlags == 0U) continue;
            const double risk = crossViewScore(row.anomalyFlags, row.confidence);
            const QString title = QStringLiteral("TID %1/PID %2 %3").arg(row.threadId).arg(row.processId).arg(crossViewText(row.anomalyFlags));
            const QString detail = QStringLiteral("thread=%1 process=%2 start=%3 %4")
                .arg(hex64(row.objectAddress), hex64(row.processObjectAddress), hex64(row.startAddress), narrowText(row.detail));
            QJsonObject payload = payloadBase(QStringLiteral("Thread Cross-View"), QStringLiteral("Thread"), title, detail, risk);
            payload.insert(QStringLiteral("threadId"), static_cast<int>(row.threadId));
            payload.insert(QStringLiteral("processId"), static_cast<int>(row.processId));
            payload.insert(QStringLiteral("objectAddress"), hex64(row.objectAddress));
            payload.insert(QStringLiteral("anomalyFlags"), hex32(row.anomalyFlags));
            payload.insert(QStringLiteral("sourceMask"), hex32(row.sourceMask));
            entries.push_back(makeEntry(QStringLiteral("Thread Cross-View"), QStringLiteral("Thread"), title, detail, risk, payload));
        }
    }

    void appendDriverIntegrity(std::vector<MonitorDock::ArkRiskCenterEntry>& entries, const QString& source, const ksword::ark::DriverIntegrityResult& result)
    {
        // Input: driver or CPU integrity result. Processing: append rows with risk flags. Return: no value.
        for (const auto& row : result.entries)
        {
            if (row.riskFlags == 0U) continue;
            const double risk = driverScore(row.riskFlags, row.confidence);
            const QString title = QStringLiteral("%1 %2").arg(source, driverRiskText(row.riskFlags));
            const QString detail = QStringLiteral("owner=%1 object=%2 target=%3 cpu=G%4/%5/V%6 %7")
                .arg(wideText(row.ownerModule), hex64(row.objectAddress), hex64(row.targetAddress))
                .arg(row.processorGroup).arg(row.processorNumber).arg(row.vector).arg(wideText(row.detail));
            QJsonObject payload = payloadBase(source, QStringLiteral("Driver"), title, detail, risk);
            payload.insert(QStringLiteral("riskFlags"), hex32(row.riskFlags));
            payload.insert(QStringLiteral("objectAddress"), hex64(row.objectAddress));
            payload.insert(QStringLiteral("targetAddress"), hex64(row.targetAddress));
            payload.insert(QStringLiteral("ownerModule"), wideText(row.ownerModule));
            payload.insert(QStringLiteral("processorGroup"), static_cast<int>(row.processorGroup));
            payload.insert(QStringLiteral("processorNumber"), static_cast<int>(row.processorNumber));
            payload.insert(QStringLiteral("vector"), static_cast<int>(row.vector));
            entries.push_back(makeEntry(source, QStringLiteral("Driver"), title, detail, risk, payload));
        }
    }

    void appendInlineHooks(std::vector<MonitorDock::ArkRiskCenterEntry>& entries, const ksword::ark::KernelInlineHookScanResult& result)
    {
        // Input: inline hook scan result. Processing: append suspicious or patched rows. Return: no value.
        for (const auto& row : result.entries)
        {
            const bool hasPatch = row.hookType != KSWORD_ARK_INLINE_HOOK_TYPE_NONE;
            const double risk = hookScore(row.status, hasPatch);
            if (risk <= 0.0) continue;
            const QString functionName = narrowText(row.functionName).trimmed();
            const QString title = QStringLiteral("%1 %2").arg(hookStatusText(row.status), functionName.isEmpty() ? hex64(row.functionAddress) : functionName);
            const QString detail = QStringLiteral("module=%1 targetModule=%2 function=%3 target=%4")
                .arg(wideText(row.moduleName), wideText(row.targetModuleName), hex64(row.functionAddress), hex64(row.targetAddress));
            QJsonObject payload = payloadBase(QStringLiteral("Inline Hook"), QStringLiteral("Hook"), title, detail, risk);
            payload.insert(QStringLiteral("status"), hookStatusText(row.status));
            payload.insert(QStringLiteral("hookType"), static_cast<int>(row.hookType));
            payload.insert(QStringLiteral("functionAddress"), hex64(row.functionAddress));
            payload.insert(QStringLiteral("targetAddress"), hex64(row.targetAddress));
            payload.insert(QStringLiteral("moduleName"), wideText(row.moduleName));
            entries.push_back(makeEntry(QStringLiteral("Inline Hook"), QStringLiteral("Hook"), title, detail, risk, payload));
        }
    }

    void appendIatEatHooks(std::vector<MonitorDock::ArkRiskCenterEntry>& entries, const ksword::ark::KernelIatEatHookScanResult& result)
    {
        // Input: IAT/EAT hook scan result. Processing: append suspicious or pointer-diff rows. Return: no value.
        for (const auto& row : result.entries)
        {
            const bool differs = row.currentTarget != 0U && row.expectedTarget != 0U && row.currentTarget != row.expectedTarget;
            const double risk = hookScore(row.status, differs);
            if (risk <= 0.0) continue;
            const QString hookClass = row.hookClass == KSWORD_ARK_IAT_EAT_HOOK_CLASS_EAT ? QStringLiteral("EAT") : QStringLiteral("IAT");
            const QString title = QStringLiteral("%1 %2 %3").arg(hookClass, hookStatusText(row.status), narrowText(row.functionName));
            const QString detail = QStringLiteral("module=%1 import=%2 current=%3 expected=%4")
                .arg(wideText(row.moduleName), wideText(row.importModuleName), hex64(row.currentTarget), hex64(row.expectedTarget));
            QJsonObject payload = payloadBase(QStringLiteral("IAT/EAT Hook"), QStringLiteral("Hook"), title, detail, risk);
            payload.insert(QStringLiteral("hookClass"), hookClass);
            payload.insert(QStringLiteral("status"), hookStatusText(row.status));
            payload.insert(QStringLiteral("thunkAddress"), hex64(row.thunkAddress));
            payload.insert(QStringLiteral("currentTarget"), hex64(row.currentTarget));
            payload.insert(QStringLiteral("expectedTarget"), hex64(row.expectedTarget));
            payload.insert(QStringLiteral("moduleName"), wideText(row.moduleName));
            entries.push_back(makeEntry(QStringLiteral("IAT/EAT Hook"), QStringLiteral("Hook"), title, detail, risk, payload));
        }
    }

    void appendCallbacks(std::vector<MonitorDock::ArkRiskCenterEntry>& entries, const ksword::ark::CallbackEnumResult& result)
    {
        // Input: callback enumeration result. Processing: append private/unresolved/untrusted callback rows. Return: no value.
        for (const auto& row : result.entries)
        {
            const double risk = callbackScore(row);
            if (risk < 20.0) continue;
            const QString callbackClass = callbackClassText(row.callbackClass);
            const QString title = QStringLiteral("%1 %2").arg(callbackClass, hex64(row.callbackAddress));
            const QString detail = QStringLiteral("module=%1 name=%2 altitude=%3 source=%4 trust=%5")
                .arg(wideText(row.modulePath), wideText(row.name), wideText(row.altitude)).arg(row.source).arg(hex32(row.trustFlags));
            QJsonObject payload = payloadBase(QStringLiteral("Callback"), QStringLiteral("Callback"), title, detail, risk);
            payload.insert(QStringLiteral("callbackClass"), callbackClass);
            payload.insert(QStringLiteral("source"), static_cast<int>(row.source));
            payload.insert(QStringLiteral("fieldFlags"), hex32(row.fieldFlags));
            payload.insert(QStringLiteral("trustFlags"), hex32(row.trustFlags));
            payload.insert(QStringLiteral("callbackAddress"), hex64(row.callbackAddress));
            payload.insert(QStringLiteral("modulePath"), wideText(row.modulePath));
            payload.insert(QStringLiteral("name"), wideText(row.name));
            payload.insert(QStringLiteral("altitude"), wideText(row.altitude));
            entries.push_back(makeEntry(QStringLiteral("Callback"), QStringLiteral("Callback"), title, detail, risk, payload));
        }
    }

    void appendMutationAudit(std::vector<MonitorDock::ArkRiskCenterEntry>& entries, const ksword::ark::MutationAuditResult& result)
    {
        // Input: read-only mutation audit result. Processing: append audit rows without exposing write controls. Return: no value.
        for (const auto& row : result.entries)
        {
            const double risk = mutationScore(row);
            if (risk <= 0.0) continue;
            const QString operation = mutationOperationText(row.operation);
            const QString status = mutationStatusText(row.status);
            const QString title = QStringLiteral("TX %1 %2 %3").arg(static_cast<qulonglong>(row.transactionId)).arg(operation, status);
            const QString detail = QStringLiteral("target=%1 bytes=%2 pid=%3 flags=%4 risk=%5")
                .arg(hex64(row.targetAddress)).arg(row.bytes).arg(row.processId).arg(hex32(row.flags), hex32(row.riskFlags));
            QJsonObject payload = payloadBase(QStringLiteral("Mutation Audit"), QStringLiteral("Mutation"), title, detail, risk);
            payload.insert(QStringLiteral("operation"), operation);
            payload.insert(QStringLiteral("status"), status);
            payload.insert(QStringLiteral("transactionId"), QString::number(static_cast<qulonglong>(row.transactionId)));
            payload.insert(QStringLiteral("sequence"), QString::number(static_cast<qulonglong>(row.sequence)));
            payload.insert(QStringLiteral("targetAddress"), hex64(row.targetAddress));
            payload.insert(QStringLiteral("bytes"), static_cast<int>(row.bytes));
            payload.insert(QStringLiteral("flags"), hex32(row.flags));
            payload.insert(QStringLiteral("riskFlags"), hex32(row.riskFlags));
            entries.push_back(makeEntry(QStringLiteral("Mutation Audit"), QStringLiteral("Mutation"), title, detail, risk, payload));
        }
    }
}

void MonitorDock::initializeArkRiskCenterTab()
{
    // 输入：无，由 initializeUi 调用。
    // 处理：创建只读 ARK 风险中心页；所有驱动访问都在 ArkDriverClient 中完成。
    // 返回：无返回值，控件由 Qt 父子树释放。
    m_arkRiskCenterPage = new QWidget(m_sideTabWidget);
    QVBoxLayout* pageLayout = new QVBoxLayout(m_arkRiskCenterPage);
    pageLayout->setContentsMargins(6, 6, 6, 6);
    pageLayout->setSpacing(6);

    QHBoxLayout* toolbarLayout = new QHBoxLayout();
    toolbarLayout->setContentsMargins(0, 0, 0, 0);
    toolbarLayout->setSpacing(8);

    m_arkRiskRefreshButton = new QPushButton(QIcon(QStringLiteral(":/Icon/process_refresh.svg")), QStringLiteral("刷新风险"), m_arkRiskCenterPage);
    m_arkRiskRefreshButton->setToolTip(QStringLiteral("只读聚合 Memory / Process / Driver / Callback / Hook / Mutation 发现"));
    m_arkRiskHighOnlyCheck = new QCheckBox(QStringLiteral("仅高风险"), m_arkRiskCenterPage);
    m_arkRiskHighOnlyCheck->setChecked(true);
    m_arkRiskHighOnlyCheck->setToolTip(QStringLiteral("仅显示 riskScore >= 50 的记录"));
    m_arkRiskFilterEdit = new QLineEdit(m_arkRiskCenterPage);
    m_arkRiskFilterEdit->setClearButtonEnabled(true);
    m_arkRiskFilterEdit->setPlaceholderText(QStringLiteral("过滤来源/分类/标题/详情/JSON"));
    m_arkRiskExportJsonButton = new QPushButton(QStringLiteral("导出 JSON"), m_arkRiskCenterPage);
    m_arkRiskExportCsvButton = new QPushButton(QStringLiteral("导出 CSV"), m_arkRiskCenterPage);
    m_arkRiskStatusLabel = new QLabel(QStringLiteral("状态：等待刷新"), m_arkRiskCenterPage);
    m_arkRiskStatusLabel->setTextInteractionFlags(Qt::TextSelectableByMouse);
    m_arkRiskStatusLabel->setStyleSheet(QStringLiteral("color:%1; font-weight:600;").arg(KswordTheme::TextSecondaryHex()));

    toolbarLayout->addWidget(m_arkRiskRefreshButton);
    toolbarLayout->addWidget(m_arkRiskHighOnlyCheck);
    toolbarLayout->addWidget(m_arkRiskFilterEdit, 1);
    toolbarLayout->addWidget(m_arkRiskExportJsonButton);
    toolbarLayout->addWidget(m_arkRiskExportCsvButton);
    toolbarLayout->addWidget(m_arkRiskStatusLabel);
    pageLayout->addLayout(toolbarLayout);

    QSplitter* splitter = new QSplitter(Qt::Vertical, m_arkRiskCenterPage);
    pageLayout->addWidget(splitter, 1);

    m_arkRiskTable = new QTableWidget(splitter);
    m_arkRiskTable->setColumnCount(riskColumnIndex(RiskColumn::Count));
    m_arkRiskTable->setHorizontalHeaderLabels(QStringList{
        QStringLiteral("riskScore"),
        QStringLiteral("来源"),
        QStringLiteral("分类"),
        QStringLiteral("标题"),
        QStringLiteral("详情")
        });
    m_arkRiskTable->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_arkRiskTable->setSelectionMode(QAbstractItemView::SingleSelection);
    m_arkRiskTable->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_arkRiskTable->setAlternatingRowColors(true);
    m_arkRiskTable->setSortingEnabled(true);
    m_arkRiskTable->verticalHeader()->setVisible(false);
    m_arkRiskTable->horizontalHeader()->setSectionResizeMode(QHeaderView::ResizeToContents);
    m_arkRiskTable->horizontalHeader()->setSectionResizeMode(riskColumnIndex(RiskColumn::Detail), QHeaderView::Stretch);
    splitter->addWidget(m_arkRiskTable);

    m_arkRiskDetailEdit = new QPlainTextEdit(splitter);
    m_arkRiskDetailEdit->setReadOnly(true);
    m_arkRiskDetailEdit->setPlainText(QStringLiteral("ARK 风险中心为只读聚合页。\n不提供任意写、修复、提交 mutation 或驱动卸载按钮；Mutation 仅展示 dry-run/audit/rollback 状态。"));
    splitter->addWidget(m_arkRiskDetailEdit);
    splitter->setStretchFactor(0, 3);
    splitter->setStretchFactor(1, 2);

    connect(m_arkRiskRefreshButton, &QPushButton::clicked, this, [this]() { refreshArkRiskCenterAsync(); });
    connect(m_arkRiskFilterEdit, &QLineEdit::textChanged, this, [this]() { rebuildArkRiskCenterTable(); });
    connect(m_arkRiskHighOnlyCheck, &QCheckBox::toggled, this, [this]() { rebuildArkRiskCenterTable(); });
    connect(m_arkRiskTable, &QTableWidget::currentCellChanged, this, [this](int, int, int, int) { showArkRiskCenterDetailForCurrentRow(); });
    connect(m_arkRiskExportJsonButton, &QPushButton::clicked, this, [this]() { exportArkRiskCenterAsJson(); });
    connect(m_arkRiskExportCsvButton, &QPushButton::clicked, this, [this]() { exportArkRiskCenterAsCsv(); });

    m_sideTabWidget->addTab(m_arkRiskCenterPage, QIcon(QStringLiteral(":/Icon/process_critical.svg")), QStringLiteral("ARK 风险中心"));
}

void MonitorDock::refreshArkRiskCenterAsync()
{
    // 输入：刷新按钮或首次进入页面触发。
    // 处理：后台通过 ArkDriverClient 只读查询多路证据，主线程更新缓存和表格。
    // 返回：无返回值。
    if (m_arkRiskRefreshInProgress)
    {
        return;
    }

    m_arkRiskRefreshInProgress = true;
    const std::uint64_t ticket = ++m_arkRiskRefreshTicket;
    if (m_arkRiskRefreshButton != nullptr)
    {
        m_arkRiskRefreshButton->setEnabled(false);
    }
    if (m_arkRiskStatusLabel != nullptr)
    {
        m_arkRiskStatusLabel->setText(QStringLiteral("状态：聚合查询中..."));
        m_arkRiskStatusLabel->setStyleSheet(QStringLiteral("color:%1; font-weight:700;").arg(KswordTheme::PrimaryBlueHex));
    }

    QPointer<MonitorDock> guardThis(this);
    QRunnable* task = QRunnable::create([guardThis, ticket]() {
        std::vector<MonitorDock::ArkRiskCenterEntry> entries;
        QStringList statusLines;
        const ksword::ark::DriverClient client;

        const unsigned long memoryFlags =
            KSWORD_ARK_MEMORY_EVIDENCE_FLAG_INCLUDE_LOADED_MODULE_EXECUTABLE |
            KSWORD_ARK_MEMORY_EVIDENCE_FLAG_INCLUDE_BIGPOOL |
            KSWORD_ARK_MEMORY_EVIDENCE_FLAG_INCLUDE_TEXT_SECTION_SAMPLES |
            KSWORD_ARK_MEMORY_EVIDENCE_FLAG_INCLUDE_SUSPECTED_BIGPOOL;
        const auto memory = client.queryKernelMemoryEvidence(memoryFlags);
        addStatusEntry(entries, statusLines, QStringLiteral("Memory Evidence"), QStringLiteral("Memory"), memory.io.ok, memory.unsupported, memory.io);
        if (memory.io.ok) appendMemory(entries, memory);

        const auto process = client.queryProcessCrossView();
        addStatusEntry(entries, statusLines, QStringLiteral("Process Cross-View"), QStringLiteral("Process"), process.io.ok, process.unsupported, process.io);
        if (process.io.ok) appendProcessCrossView(entries, process);

        const auto thread = client.queryThreadCrossView();
        addStatusEntry(entries, statusLines, QStringLiteral("Thread Cross-View"), QStringLiteral("Thread"), thread.io.ok, thread.unsupported, thread.io);
        if (thread.io.ok) appendThreadCrossView(entries, thread);

        const auto driver = client.queryDriverIntegrity();
        addStatusEntry(entries, statusLines, QStringLiteral("Driver Integrity"), QStringLiteral("Driver"), driver.io.ok, driver.unsupported, driver.io);
        if (driver.io.ok) appendDriverIntegrity(entries, QStringLiteral("Driver Integrity"), driver);

        const auto cpu = client.queryKernelCpuIntegrity();
        addStatusEntry(entries, statusLines, QStringLiteral("CPU Integrity"), QStringLiteral("Driver"), cpu.io.ok, cpu.unsupported, cpu.io);
        if (cpu.io.ok) appendDriverIntegrity(entries, QStringLiteral("CPU Integrity"), cpu);

        const auto inlineHooks = client.scanInlineHooks();
        addStatusEntry(entries, statusLines, QStringLiteral("Inline Hook"), QStringLiteral("Hook"), inlineHooks.io.ok, false, inlineHooks.io);
        if (inlineHooks.io.ok) appendInlineHooks(entries, inlineHooks);

        const auto iatEatHooks = client.enumerateIatEatHooks();
        addStatusEntry(entries, statusLines, QStringLiteral("IAT/EAT Hook"), QStringLiteral("Hook"), iatEatHooks.io.ok, false, iatEatHooks.io);
        if (iatEatHooks.io.ok) appendIatEatHooks(entries, iatEatHooks);

        const auto callbacks = client.enumerateCallbacks();
        addStatusEntry(entries, statusLines, QStringLiteral("Callback"), QStringLiteral("Callback"), callbacks.io.ok, false, callbacks.io);
        if (callbacks.io.ok) appendCallbacks(entries, callbacks);

        const auto mutation = client.queryMutationAudit();
        addStatusEntry(entries, statusLines, QStringLiteral("Mutation Audit"), QStringLiteral("Mutation"), mutation.io.ok, mutation.unsupported, mutation.io);
        if (mutation.io.ok) appendMutationAudit(entries, mutation);

        std::sort(entries.begin(), entries.end(), [](const auto& left, const auto& right) {
            return left.riskScore > right.riskScore;
        });

        if (guardThis == nullptr)
        {
            return;
        }
        QMetaObject::invokeMethod(
            guardThis.data(),
            [guardThis, ticket, entries = std::move(entries), statusLines = std::move(statusLines)]() mutable {
                if (guardThis == nullptr || guardThis->m_arkRiskRefreshTicket != ticket)
                {
                    return;
                }
                guardThis->m_arkRiskRefreshInProgress = false;
                if (guardThis->m_arkRiskRefreshButton != nullptr)
                {
                    guardThis->m_arkRiskRefreshButton->setEnabled(true);
                }
                guardThis->m_arkRiskCenterEntries = std::move(entries);
                guardThis->rebuildArkRiskCenterTable();
                guardThis->showArkRiskCenterDetailForCurrentRow();
                if (guardThis->m_arkRiskStatusLabel != nullptr)
                {
                    guardThis->m_arkRiskStatusLabel->setText(
                        QStringLiteral("状态：%1 项；%2")
                        .arg(static_cast<qulonglong>(guardThis->m_arkRiskCenterEntries.size()))
                        .arg(statusLines.join(QStringLiteral("；"))));
                    guardThis->m_arkRiskStatusLabel->setStyleSheet(QStringLiteral("color:#2F7D32; font-weight:700;"));
                }
            },
            Qt::QueuedConnection);
    });
    task->setAutoDelete(true);
    QThreadPool::globalInstance()->start(task);
}

void MonitorDock::rebuildArkRiskCenterTable()
{
    // 输入：无，读取风险中心缓存和过滤控件。
    // 处理：按 high-only 与关键字投影表格；不访问驱动。
    // 返回：无返回值。
    if (m_arkRiskTable == nullptr)
    {
        return;
    }
    const QString filter = m_arkRiskFilterEdit != nullptr ? m_arkRiskFilterEdit->text().trimmed() : QString();
    const bool highOnly = m_arkRiskHighOnlyCheck != nullptr && m_arkRiskHighOnlyCheck->isChecked();

    std::vector<std::size_t> visibleIndexes;
    visibleIndexes.reserve(m_arkRiskCenterEntries.size());
    for (std::size_t index = 0; index < m_arkRiskCenterEntries.size(); ++index)
    {
        const auto& entry = m_arkRiskCenterEntries[index];
        if (highOnly && entry.riskScore < 50.0)
        {
            continue;
        }
        if (!matchesFilter(entry, filter))
        {
            continue;
        }
        visibleIndexes.push_back(index);
    }

    const QSignalBlocker blocker(m_arkRiskTable);
    m_arkRiskTable->setSortingEnabled(false);
    m_arkRiskTable->setRowCount(static_cast<int>(visibleIndexes.size()));
    for (int row = 0; row < static_cast<int>(visibleIndexes.size()); ++row)
    {
        const std::size_t cacheIndex = visibleIndexes[static_cast<std::size_t>(row)];
        const auto& entry = m_arkRiskCenterEntries[cacheIndex];
        QTableWidgetItem* scoreItem = new ScoreItem(entry.riskScore);
        scoreItem->setData(Qt::UserRole + 1, QVariant::fromValue<qulonglong>(static_cast<qulonglong>(cacheIndex)));
        m_arkRiskTable->setItem(row, riskColumnIndex(RiskColumn::Score), scoreItem);
        m_arkRiskTable->setItem(row, riskColumnIndex(RiskColumn::Source), textItem(entry.sourceName));
        m_arkRiskTable->setItem(row, riskColumnIndex(RiskColumn::Category), textItem(entry.category));
        m_arkRiskTable->setItem(row, riskColumnIndex(RiskColumn::Title), textItem(entry.title));
        m_arkRiskTable->setItem(row, riskColumnIndex(RiskColumn::Detail), textItem(entry.detail));
    }
    if (m_arkRiskTable->rowCount() > 0 && m_arkRiskTable->currentRow() < 0)
    {
        m_arkRiskTable->setCurrentCell(0, riskColumnIndex(RiskColumn::Score));
    }
    m_arkRiskTable->setSortingEnabled(true);
    ks::ui::RequestTableColumnAutoFit(m_arkRiskTable);
}

void MonitorDock::showArkRiskCenterDetailForCurrentRow() const
{
    // 输入：无，读取当前表格选择。
    // 处理：通过缓存索引展开摘要和 JSON payload。
    // 返回：无返回值。
    if (m_arkRiskDetailEdit == nullptr || m_arkRiskTable == nullptr)
    {
        return;
    }
    const int row = m_arkRiskTable->currentRow();
    if (row < 0)
    {
        m_arkRiskDetailEdit->setPlainText(QStringLiteral("请选择一条风险记录。"));
        return;
    }
    const QTableWidgetItem* scoreItem = m_arkRiskTable->item(row, riskColumnIndex(RiskColumn::Score));
    bool ok = false;
    const qulonglong cacheIndex = scoreItem != nullptr ? scoreItem->data(Qt::UserRole + 1).toULongLong(&ok) : 0ULL;
    if (!ok || cacheIndex >= static_cast<qulonglong>(m_arkRiskCenterEntries.size()))
    {
        m_arkRiskDetailEdit->setPlainText(QStringLiteral("当前行缓存索引无效。"));
        return;
    }
    const auto& entry = m_arkRiskCenterEntries[static_cast<std::size_t>(cacheIndex)];
    QString detail;
    detail += QStringLiteral("ARK 风险详情\n");
    detail += QStringLiteral("riskScore: %1\nsource: %2\ncategory: %3\ntitle: %4\ndetail: %5\n\n")
        .arg(entry.riskScoreText, entry.sourceName, entry.category, entry.title, entry.detail);
    detail += QString::fromUtf8(QJsonDocument(entry.payload).toJson(QJsonDocument::Indented));
    m_arkRiskDetailEdit->setPlainText(detail);
}

void MonitorDock::exportArkRiskCenterAsJson() const
{
    // 输入：无，读取风险中心缓存。
    // 处理：用户选择路径后写 JSON 数组；缓存为空时提示。
    // 返回：无返回值。
    if (m_arkRiskCenterEntries.empty())
    {
        QMessageBox::information(const_cast<MonitorDock*>(this), QStringLiteral("ARK 风险中心"), QStringLiteral("当前没有可导出的风险记录。"));
        return;
    }
    const QString defaultPath = QDir::home().filePath(QStringLiteral("ark-risk-center-%1.json").arg(QDateTime::currentDateTime().toString(QStringLiteral("yyyyMMdd-hhmmss"))));
    const QString filePath = QFileDialog::getSaveFileName(const_cast<MonitorDock*>(this), QStringLiteral("导出 ARK 风险 JSON"), defaultPath, QStringLiteral("JSON (*.json)"));
    if (filePath.isEmpty())
    {
        return;
    }
    QJsonArray rows;
    for (const auto& entry : m_arkRiskCenterEntries)
    {
        rows.append(entry.payload);
    }
    QFile file(filePath);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate))
    {
        QMessageBox::warning(const_cast<MonitorDock*>(this), QStringLiteral("导出失败"), file.errorString());
        return;
    }
    file.write(QJsonDocument(rows).toJson(QJsonDocument::Indented));
}

void MonitorDock::exportArkRiskCenterAsCsv() const
{
    // 输入：无，读取风险中心缓存。
    // 处理：用户选择路径后写 CSV；缓存为空时提示。
    // 返回：无返回值。
    if (m_arkRiskCenterEntries.empty())
    {
        QMessageBox::information(const_cast<MonitorDock*>(this), QStringLiteral("ARK 风险中心"), QStringLiteral("当前没有可导出的风险记录。"));
        return;
    }
    const QString defaultPath = QDir::home().filePath(QStringLiteral("ark-risk-center-%1.csv").arg(QDateTime::currentDateTime().toString(QStringLiteral("yyyyMMdd-hhmmss"))));
    const QString filePath = QFileDialog::getSaveFileName(const_cast<MonitorDock*>(this), QStringLiteral("导出 ARK 风险 CSV"), defaultPath, QStringLiteral("CSV (*.csv)"));
    if (filePath.isEmpty())
    {
        return;
    }
    QFile file(filePath);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate | QIODevice::Text))
    {
        QMessageBox::warning(const_cast<MonitorDock*>(this), QStringLiteral("导出失败"), file.errorString());
        return;
    }
    file.write("riskScore,source,category,title,detail,json\n");
    for (const auto& entry : m_arkRiskCenterEntries)
    {
        const QString jsonText = QString::fromUtf8(QJsonDocument(entry.payload).toJson(QJsonDocument::Compact));
        const QString line = QStringLiteral("%1,%2,%3,%4,%5,%6\n")
            .arg(csvEscape(entry.riskScoreText),
                csvEscape(entry.sourceName),
                csvEscape(entry.category),
                csvEscape(entry.title),
                csvEscape(entry.detail),
                csvEscape(jsonText));
        file.write(line.toUtf8());
    }
}
