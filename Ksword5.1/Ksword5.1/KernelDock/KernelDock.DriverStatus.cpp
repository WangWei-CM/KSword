#include "KernelDock.h"

#include "../ArkDriverClient/ArkDriverClient.h"
#include "../UI/CodeEditorWidget.h"
#include "../theme.h"

#include <QAbstractItemView>
#include <QApplication>
#include <QBrush>
#include <QClipboard>
#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QHeaderView>
#include <QHBoxLayout>
#include <QIcon>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>
#include <QJsonValue>
#include <QLabel>
#include <QLineEdit>
#include <QMetaObject>
#include <QPointer>
#include <QPushButton>
#include <QRegularExpression>
#include <QStringList>
#include <QSplitter>
#include <QTableWidget>
#include <QTableWidgetItem>
#include <QVBoxLayout>

#include <algorithm>
#include <array>
#include <limits>
#include <thread>
#include <utility>

namespace
{
    // DriverSummaryColumn and DriverCapabilityColumn keep table layout explicit.
    enum class DriverSummaryColumn : int { Name = 0, Value, Count };
    enum class DriverCapabilityColumn : int { Feature = 0, State, Policy, RequiredDyn, PresentDyn, Dependency, Reason, Count };

    struct CapabilityDisplay
    {
        std::uint64_t mask = 0;
        const char* name = nullptr;
        const wchar_t* title = nullptr;
    };

    struct PolicyDisplay
    {
        std::uint32_t mask = 0;
        const char* name = nullptr;
        const wchar_t* title = nullptr;
    };

    // LocalPdbPackMatch：
    // - 输入来源：程序目录或环境变量指定的 compact profile pack；
    // - 处理逻辑：只做本地 pack 的 identity 精确匹配和轻量诊断，不应用 profile；
    // - 返回行为：由 queryDriverStatusSnapshot 填入 KernelDriverStatusSummary 展示。
    struct LocalPdbPackMatch
    {
        bool scanned = false;                 // scanned：是否实际检查过至少一个候选 pack。
        bool matched = false;                 // matched：是否有 class/machine/timestamp/size 完全命中的 profile。
        bool valid = false;                   // valid：命中 profile 是否包含可用字段数组。
        std::uint32_t existingPackCount = 0;  // existingPackCount：存在且可读前尝试解析的 pack 文件数。
        std::uint32_t profileCount = 0;       // profileCount：pack 内 profile 总数。
        std::uint32_t scannedProfileCount = 0; // scannedProfileCount：本次扫描过的 profile 数。
        std::uint32_t fieldCount = 0;         // fieldCount：命中 profile 声明字段数。
        QString profileNameText;              // profileNameText：命中 profileName。
        QString versionText;                  // versionText：从 profileName 提取的 Windows 版本号。
        QString pathText;                     // pathText：命中或最后诊断的 pack 路径。
        QString messageText;                  // messageText：给 UI 展示的匹配/失败原因。
    };

    // kDynCapabilities：
    // - 作用：列出所有可由 R0 DynData 暴露的 capability bit；
    // - 处理逻辑：驱动状态页、能力详情和筛选都复用该表。
    constexpr std::array<CapabilityDisplay, 12> kDynCapabilities{ {
        { KSW_CAP_DYN_NTOS_ACTIVE, "KSW_CAP_DYN_NTOS_ACTIVE", L"ntoskrnl profile 已激活" },
        { KSW_CAP_DYN_LXCORE_ACTIVE, "KSW_CAP_DYN_LXCORE_ACTIVE", L"lxcore profile 已激活" },
        { KSW_CAP_OBJECT_TYPE_FIELDS, "KSW_CAP_OBJECT_TYPE_FIELDS", L"对象类型字段" },
        { KSW_CAP_HANDLE_TABLE_DECODE, "KSW_CAP_HANDLE_TABLE_DECODE", L"句柄表解码" },
        { KSW_CAP_PROCESS_OBJECT_TABLE, "KSW_CAP_PROCESS_OBJECT_TABLE", L"进程 ObjectTable" },
        { KSW_CAP_THREAD_STACK_FIELDS, "KSW_CAP_THREAD_STACK_FIELDS", L"线程栈字段" },
        { KSW_CAP_THREAD_IO_COUNTERS, "KSW_CAP_THREAD_IO_COUNTERS", L"线程 I/O 计数" },
        { KSW_CAP_ALPC_FIELDS, "KSW_CAP_ALPC_FIELDS", L"ALPC 字段" },
        { KSW_CAP_SECTION_CONTROL_AREA, "KSW_CAP_SECTION_CONTROL_AREA", L"Section/ControlArea" },
        { KSW_CAP_PROCESS_PROTECTION_PATCH, "KSW_CAP_PROCESS_PROTECTION_PATCH", L"进程保护修改" },
        { KSW_CAP_WSL_LXCORE_FIELDS, "KSW_CAP_WSL_LXCORE_FIELDS", L"WSL/lxcore 字段" },
        { KSW_CAP_ETW_GUID_FIELDS, "KSW_CAP_ETW_GUID_FIELDS", L"ETW GUID/Registration 字段" }
    } };

    // kSecurityPolicies lists every policy bit currently surfaced by Phase 1.
    constexpr std::array<PolicyDisplay, 6> kSecurityPolicies{ {
        { KSWORD_ARK_SECURITY_POLICY_FLAG_ACTIVE, "POLICY_ACTIVE", L"安全策略启用" },
        { KSWORD_ARK_SECURITY_POLICY_ALLOW_MUTATING_ACTIONS, "ALLOW_MUTATING_ACTIONS", L"允许进程修改动作" },
        { KSWORD_ARK_SECURITY_POLICY_ALLOW_FILE_DELETE, "ALLOW_FILE_DELETE", L"允许文件删除" },
        { KSWORD_ARK_SECURITY_POLICY_ALLOW_CALLBACK_CONTROL, "ALLOW_CALLBACK_CONTROL", L"允许回调控制" },
        { KSWORD_ARK_SECURITY_POLICY_ALLOW_PROCESS_PROTECTION, "ALLOW_PROCESS_PROTECTION", L"允许进程保护修改" },
        { KSWORD_ARK_SECURITY_POLICY_ALLOW_KERNEL_SNAPSHOTS, "ALLOW_KERNEL_SNAPSHOTS", L"允许内核快照" }
    } };

    QString blueButtonStyle() { return KswordTheme::ThemedButtonStyle(); }

    QString blueInputStyle()
    {
        return QStringLiteral(
            "QLineEdit{border:1px solid %2;border-radius:2px;background:%3;color:%4;padding:2px 6px;}"
            "QLineEdit:focus{border:1px solid %1;}")
            .arg(KswordTheme::PrimaryBlueHex)
            .arg(KswordTheme::BorderHex())
            .arg(KswordTheme::SurfaceHex())
            .arg(KswordTheme::TextPrimaryHex());
    }

    QString headerStyle()
    {
        return QStringLiteral("QHeaderView::section{color:%1;background:%2;border:1px solid %3;font-weight:600;}")
            .arg(KswordTheme::PrimaryBlueHex)
            .arg(KswordTheme::SurfaceHex())
            .arg(KswordTheme::BorderHex());
    }

    QString itemSelectionStyle()
    {
        return QStringLiteral("QTableWidget::item:selected{background:%1;color:#FFFFFF;}").arg(KswordTheme::PrimaryBlueHex);
    }

    QString statusLabelStyle(const QString& colorHex)
    {
        return QStringLiteral("color:%1;font-weight:600;").arg(colorHex);
    }

    QString safeText(const QString& valueText, const QString& fallbackText = QStringLiteral("<空>"))
    {
        return valueText.trimmed().isEmpty() ? fallbackText : valueText;
    }

    QString stringToQString(const std::string& valueText)
    {
        return QString::fromUtf8(valueText.c_str(), static_cast<int>(valueText.size()));
    }

    // wideStringToQString：
    // - 输入 valueText：ArkDriverClient 返回的宽字符串；
    // - 处理：按 wchar_t 数组安全转换为 QString；
    // - 返回：Qt UI 可直接展示的文本。
    QString wideStringToQString(const std::wstring& valueText)
    {
        return QString::fromWCharArray(valueText.c_str(), static_cast<int>(valueText.size()));
    }

    QString formatHex32(const std::uint32_t value) { return QStringLiteral("0x%1").arg(value, 8, 16, QChar('0')).toUpper(); }
    QString formatHex64(const std::uint64_t value) { return QStringLiteral("0x%1").arg(value, 16, 16, QChar('0')).toUpper(); }
    QString formatNtStatus(const long value) { return formatHex32(static_cast<std::uint32_t>(value)); }
    QString boolText(const bool value) { return value ? QStringLiteral("是") : QStringLiteral("否"); }
    bool flagEnabled(const std::uint32_t flags, const std::uint32_t flag) { return (flags & flag) == flag; }

    // fieldOffsetPresent：
    // - 输入 flags/offset：R0 返回的字段状态位和偏移；
    // - 处理：同时检查 PRESENT bit 和不可用哨兵；
    // - 返回：true 表示该字段当前有可信可用偏移值。
    bool fieldOffsetPresent(const std::uint32_t flags, const std::uint32_t offset)
    {
        return (flags & KSW_DYN_FIELD_FLAG_PRESENT) != 0U &&
            offset != 0xFFFFFFFFU &&
            offset != 0x0000FFFFU;
    }

    // parseProfileUInt32：
    // - 输入 value：pack JSON 中可能是数字或 0x 字符串的值；
    // - 处理：按 32-bit 无符号整数解析并做范围校验；
    // - 返回：成功解析 true，失败 false，同时写入 valueOut。
    bool parseProfileUInt32(const QJsonValue& value, std::uint32_t& valueOut)
    {
        bool ok = false;
        qulonglong parsedValue = 0ULL;
        valueOut = 0U;

        if (value.isString())
        {
            QString text = value.toString().trimmed();
            int base = 10;
            if (text.startsWith(QStringLiteral("0x"), Qt::CaseInsensitive))
            {
                text = text.mid(2);
                base = 16;
            }
            parsedValue = text.toULongLong(&ok, base);
        }
        else if (value.isDouble())
        {
            const double numericValue = value.toDouble();
            if (numericValue >= 0.0 && numericValue <= static_cast<double>(std::numeric_limits<std::uint32_t>::max()))
            {
                parsedValue = static_cast<qulonglong>(numericValue);
                ok = true;
            }
        }

        if (!ok || parsedValue > static_cast<qulonglong>(std::numeric_limits<std::uint32_t>::max()))
        {
            return false;
        }

        valueOut = static_cast<std::uint32_t>(parsedValue);
        return true;
    }

    // appendUniquePath：
    // - 输入 paths/pathText：候选路径列表与待添加路径；
    // - 处理：清理路径并做大小写不敏感去重；
    // - 返回：无返回值，paths 会按需追加。
    void appendUniquePath(QStringList& paths, const QString& pathText)
    {
        const QString trimmed = pathText.trimmed();
        if (trimmed.isEmpty())
        {
            return;
        }

        const QString cleanedPath = QDir::cleanPath(trimmed);
        if (!cleanedPath.isEmpty() && !paths.contains(cleanedPath, Qt::CaseInsensitive))
        {
            paths << cleanedPath;
        }
    }

    // profilePackSearchPaths：
    // - 输入：无；
    // - 处理：按 Release 默认路径、调试环境变量、当前目录兜底构造候选 pack；
    // - 返回：候选文件路径列表，调用方仍需检查是否存在。
    QStringList profilePackSearchPaths()
    {
        QStringList paths;
        appendUniquePath(paths, QDir(QCoreApplication::applicationDirPath()).filePath(QStringLiteral("profiles/ark_dyndata_pack_v1.json")));
        appendUniquePath(paths, qEnvironmentVariable("KSWORD_ARK_PROFILE_PACK"));
        appendUniquePath(paths, QDir::current().filePath(QStringLiteral("profiles/ark_dyndata_pack_v1.json")));
        return paths;
    }

    // moduleClassText：
    // - 输入 classId：KSW_DYN_PROFILE_CLASS_*；
    // - 处理：转换为用户能理解的内核模块类别；
    // - 返回：ntoskrnl/ntkrla57/lxcore 或 unknown(...) 文本。
    QString moduleClassText(const std::uint32_t classId)
    {
        switch (classId)
        {
        case KSW_DYN_PROFILE_CLASS_NTOSKRNL:
            return QStringLiteral("ntoskrnl");
        case KSW_DYN_PROFILE_CLASS_NTKRLA57:
            return QStringLiteral("ntkrla57");
        case KSW_DYN_PROFILE_CLASS_LXCORE:
            return QStringLiteral("lxcore");
        default:
            return QStringLiteral("unknown(%1)").arg(classId);
        }
    }

    // extractVersionFromProfileName：
    // - 输入 profileName：生成器写入的 profileName；
    // - 处理：提取第一个 Windows 四段版本号；
    // - 返回：命中返回版本文本，否则返回空字符串。
    QString extractVersionFromProfileName(const QString& profileName)
    {
        static const QRegularExpression kVersionPattern(QStringLiteral("(\\d+\\.\\d+\\.\\d+\\.\\d+)"));
        const QRegularExpressionMatch match = kVersionPattern.match(profileName);
        return match.hasMatch() ? match.captured(1) : QString();
    }

    // findMatchingLocalPdbProfilePack：
    // - 输入 currentIdentity：R0 识别到的当前 ntoskrnl identity；
    // - 处理：解析 compact JSON pack 并用 class/machine/timestamp/size 精确匹配；
    // - 返回：匹配结果和诊断；不会修改驱动状态，也不会应用 profile。
    LocalPdbPackMatch findMatchingLocalPdbProfilePack(const ksword::ark::ArkDynModuleIdentity& currentIdentity)
    {
        LocalPdbPackMatch result;
        QStringList diagnostics;

        if (!currentIdentity.present)
        {
            result.messageText = QStringLiteral("当前 ntoskrnl identity 不可用，无法匹配本地 PDB profile pack。");
            return result;
        }

        for (const QString& candidatePath : profilePackSearchPaths())
        {
            const QFileInfo fileInfo(candidatePath);
            if (!fileInfo.exists() || !fileInfo.isFile())
            {
                diagnostics << QStringLiteral("pack 不存在: %1").arg(QDir::toNativeSeparators(candidatePath));
                continue;
            }

            result.scanned = true;
            result.existingPackCount += 1U;
            result.pathText = QDir::toNativeSeparators(fileInfo.absoluteFilePath());

            QFile file(fileInfo.absoluteFilePath());
            if (!file.open(QIODevice::ReadOnly))
            {
                diagnostics << QStringLiteral("无法打开 pack: %1 (%2)").arg(result.pathText, file.errorString());
                continue;
            }

            QJsonParseError parseError{};
            const QJsonDocument document = QJsonDocument::fromJson(file.readAll(), &parseError);
            if (parseError.error != QJsonParseError::NoError || !document.isObject())
            {
                diagnostics << QStringLiteral("pack JSON 解析失败: %1 (%2)").arg(result.pathText, parseError.errorString());
                continue;
            }

            const QJsonObject rootObject = document.object();
            std::uint32_t schemaVersion = 0U;
            std::uint32_t packVersion = 0U;
            if (!parseProfileUInt32(rootObject.value(QStringLiteral("schemaVersion")), schemaVersion) ||
                !parseProfileUInt32(rootObject.value(QStringLiteral("packVersion")), packVersion) ||
                schemaVersion != 1U ||
                packVersion != 1U)
            {
                diagnostics << QStringLiteral("pack schemaVersion/packVersion 不支持: %1").arg(result.pathText);
                continue;
            }

            const QJsonArray profilesArray = rootObject.value(QStringLiteral("profiles")).toArray();
            result.profileCount = static_cast<std::uint32_t>(profilesArray.size());
            if (profilesArray.isEmpty())
            {
                diagnostics << QStringLiteral("pack profile 列表为空: %1").arg(result.pathText);
                continue;
            }

            for (const QJsonValue& profileValue : profilesArray)
            {
                if (!profileValue.isObject())
                {
                    continue;
                }

                result.scannedProfileCount += 1U;
                const QJsonObject profileObject = profileValue.toObject();
                std::uint32_t moduleClassId = 0U;
                std::uint32_t machine = 0U;
                std::uint32_t timeDateStamp = 0U;
                std::uint32_t sizeOfImage = 0U;
                if (!parseProfileUInt32(profileObject.value(QStringLiteral("moduleClassId")), moduleClassId) ||
                    !parseProfileUInt32(profileObject.value(QStringLiteral("machine")), machine) ||
                    !parseProfileUInt32(profileObject.value(QStringLiteral("timeDateStamp")), timeDateStamp) ||
                    !parseProfileUInt32(profileObject.value(QStringLiteral("sizeOfImage")), sizeOfImage))
                {
                    continue;
                }

                if (currentIdentity.classId != moduleClassId ||
                    currentIdentity.machine != machine ||
                    currentIdentity.timeDateStamp != timeDateStamp ||
                    currentIdentity.sizeOfImage != sizeOfImage)
                {
                    continue;
                }

                const QJsonArray fieldsArray = profileObject.value(QStringLiteral("fields")).toArray();
                result.matched = true;
                result.valid = !fieldsArray.isEmpty();
                result.fieldCount = static_cast<std::uint32_t>(fieldsArray.size());
                result.profileNameText = profileObject.value(QStringLiteral("profileName")).toString(QStringLiteral("pack-profile"));
                result.versionText = extractVersionFromProfileName(result.profileNameText);
                result.messageText = result.valid
                    ? QStringLiteral("本地 PDB profile pack 命中；profiles=%1，扫描=%2，字段=%3。")
                        .arg(result.profileCount)
                        .arg(result.scannedProfileCount)
                        .arg(result.fieldCount)
                    : QStringLiteral("本地 PDB profile pack 命中 identity，但字段数组为空，已视为无效。");
                return result;
            }

            diagnostics << QStringLiteral("pack 未命中: %1 (profiles=%2)").arg(result.pathText).arg(result.profileCount);
        }

        result.messageText = QStringLiteral("未找到匹配 PDB profile pack；检查 %1 个存在的 pack。%2")
            .arg(result.existingPackCount)
            .arg(diagnostics.join(QStringLiteral(" | ")));
        return result;
    }

    QString capabilityNames(const std::uint64_t mask)
    {
        QStringList names;
        for (const CapabilityDisplay& item : kDynCapabilities)
        {
            if ((mask & item.mask) == item.mask)
            {
                names << QStringLiteral("%1 (%2)").arg(QString::fromLatin1(item.name)).arg(QString::fromWCharArray(item.title));
            }
        }
        return names.isEmpty() ? QStringLiteral("None") : names.join(QStringLiteral(", "));
    }

    QString policyNames(const std::uint32_t mask)
    {
        QStringList names;
        for (const PolicyDisplay& item : kSecurityPolicies)
        {
            if ((mask & item.mask) == item.mask)
            {
                names << QStringLiteral("%1 (%2)").arg(QString::fromLatin1(item.name)).arg(QString::fromWCharArray(item.title));
            }
        }
        return names.isEmpty() ? QStringLiteral("None") : names.join(QStringLiteral(", "));
    }

    // kernelIdentityText：
    // - 输入 summary：驱动状态摘要；
    // - 处理：把 ntoskrnl identity 的关键 PE 字段压成单行；
    // - 返回：可放入摘要表和诊断详情的当前内核身份文本。
    QString kernelIdentityText(const KernelDriverStatusSummary& summary)
    {
        if (!summary.ntoskrnlIdentityPresent)
        {
            return QStringLiteral("<未识别>");
        }

        return QStringLiteral("%1，Class=%2 (%3)，Machine=%4，TimeDateStamp=%5，SizeOfImage=%6，Base=%7")
            .arg(safeText(summary.ntoskrnlModuleNameText))
            .arg(moduleClassText(summary.ntoskrnlClassId))
            .arg(summary.ntoskrnlClassId)
            .arg(formatHex32(summary.ntoskrnlMachine))
            .arg(formatHex32(summary.ntoskrnlTimeDateStamp))
            .arg(formatHex32(summary.ntoskrnlSizeOfImage))
            .arg(formatHex64(summary.ntoskrnlImageBase));
    }

    // kernelVersionText：
    // - 输入 summary：驱动状态摘要；
    // - 处理：优先使用本地 PDB profileName 中的四段 Windows 版本号；
    // - 返回：识别到的 Windows 内核版本，未命中时返回明确占位。
    QString kernelVersionText(const KernelDriverStatusSummary& summary)
    {
        if (!summary.localPdbProfileVersionText.trimmed().isEmpty())
        {
            return summary.localPdbProfileVersionText;
        }
        return summary.ntoskrnlIdentityPresent
            ? QStringLiteral("<identity 已识别，版本号需匹配 PDB profile 后确认>")
            : QStringLiteral("<未识别>");
    }

    // fieldCoverageText：
    // - 输入 summary：驱动状态摘要；
    // - 处理：汇总 R0 字段总数、返回字段和可用字段；
    // - 返回：用户可读字段覆盖率文本。
    QString fieldCoverageText(const KernelDriverStatusSummary& summary)
    {
        return QStringLiteral("可用 %1 / 返回 %2 / R0声明 %3，必需缺失 %4")
            .arg(summary.dynDataPresentFieldCount)
            .arg(summary.dynDataReturnedFieldCount)
            .arg(summary.dynDataFieldCount)
            .arg(summary.dynDataRequiredMissingCount);
    }

    // fieldSourceSummaryText：
    // - 输入 summary：驱动状态摘要；
    // - 处理：按当前可用字段的来源分类计数；
    // - 返回：字段来源分布文本。
    QString fieldSourceSummaryText(const KernelDriverStatusSummary& summary)
    {
        return QStringLiteral("PDB=%1，RuntimePattern=%2，SystemInformer=%3，Extra=%4，不可用=%5")
            .arg(summary.dynDataPdbProfileFieldCount)
            .arg(summary.dynDataRuntimePatternFieldCount)
            .arg(summary.dynDataSystemInformerFieldCount)
            .arg(summary.dynDataExtraTableFieldCount)
            .arg(summary.dynDataUnavailableFieldCount);
    }

    // localPdbProfileText：
    // - 输入 summary：驱动状态摘要；
    // - 处理：把本地 pack 命中、版本、字段和路径合成单行；
    // - 返回：摘要表展示的本地 profile 状态文本。
    QString localPdbProfileText(const KernelDriverStatusSummary& summary)
    {
        if (summary.localPdbProfileMatched)
        {
            return QStringLiteral("命中：%1，版本=%2，字段=%3，packProfiles=%4，路径=%5")
                .arg(safeText(summary.localPdbProfileNameText))
                .arg(safeText(summary.localPdbProfileVersionText, QStringLiteral("<未提取>")))
                .arg(summary.localPdbProfileFieldCount)
                .arg(summary.localPdbProfilePackProfileCount)
                .arg(safeText(summary.localPdbProfilePathText));
        }

        return safeText(summary.localPdbProfileMessageText, QStringLiteral("未命中或未扫描。"));
    }

    // trustedOffsetText：
    // - 输入 summary：驱动状态摘要；
    // - 处理：根据 R0 PDB profile active 标志、字段来源和 pack 命中状态判断可信偏移；
    // - 返回：用户层面的可信偏移状态说明。
    QString trustedOffsetText(const KernelDriverStatusSummary& summary)
    {
        if (summary.trustedPdbOffsetsActive)
        {
            return QStringLiteral("已启用可信 PDB 偏移；PDB字段 %1 / 可用字段 %2，pack=%3。")
                .arg(summary.dynDataPdbProfileFieldCount)
                .arg(summary.dynDataPresentFieldCount)
                .arg(summary.localPdbProfileMatched ? QStringLiteral("命中") : QStringLiteral("未确认"));
        }
        if (summary.localPdbProfileMatched)
        {
            return QStringLiteral("本地 pack 已匹配当前内核，但 R0 当前字段来源尚未切换到 PDB profile。");
        }
        if (summary.dynDataPresentFieldCount != 0U)
        {
            return QStringLiteral("当前有可用 DynData 字段，但未发现 PDB profile 字段；来源=%1。")
                .arg(fieldSourceSummaryText(summary));
        }
        return QStringLiteral("暂无可用可信偏移。");
    }

    // dynDataIoText：
    // - 输入 summary：驱动状态摘要；
    // - 处理：拼接 DynData status/fields 两个 IOCTL 的结果；
    // - 返回：R3/R0 通信诊断文本。
    QString dynDataIoText(const KernelDriverStatusSummary& summary)
    {
        return QStringLiteral("Status=%1 (%2)；Fields=%3 (%4)")
            .arg(boolText(summary.dynDataStatusQueryOk))
            .arg(safeText(summary.dynDataStatusIoMessageText))
            .arg(boolText(summary.dynDataFieldsQueryOk))
            .arg(safeText(summary.dynDataFieldsIoMessageText));
    }

    QString statusBadges(const KernelDriverStatusSummary& summary)
    {
        QStringList badges;
        badges << (summary.driverLoaded ? QStringLiteral("Driver Loaded") : QStringLiteral("Driver Missing"));
        if (!summary.protocolOk) { badges << QStringLiteral("Protocol Mismatch"); }
        if (summary.dynDataMissing) { badges << QStringLiteral("DynData Missing"); }
        if (summary.pdbProfileActive) { badges << QStringLiteral("PDB Profile Active"); }
        if (summary.localPdbProfileMatched) { badges << QStringLiteral("Pack Matched"); }
        if (summary.trustedPdbOffsetsActive) { badges << QStringLiteral("Trusted Offsets"); }
        if (summary.dynDataRequiredMissingCount != 0U) { badges << QStringLiteral("Required Offsets Missing"); }
        if (summary.limited) { badges << QStringLiteral("Limited"); }
        return badges.join(QStringLiteral(", "));
    }

    QString dynDataStatusText(const std::uint32_t flags)
    {
        QStringList parts;
        if (flagEnabled(flags, KSW_DYN_STATUS_FLAG_INITIALIZED)) { parts << QStringLiteral("Initialized"); }
        if (flagEnabled(flags, KSW_DYN_STATUS_FLAG_NTOS_ACTIVE)) { parts << QStringLiteral("NtosActive"); }
        if (flagEnabled(flags, KSW_DYN_STATUS_FLAG_LXCORE_ACTIVE)) { parts << QStringLiteral("LxcoreActive"); }
        if (flagEnabled(flags, KSW_DYN_STATUS_FLAG_EXTRA_ACTIVE)) { parts << QStringLiteral("ExtraActive"); }
        if (flagEnabled(flags, KSW_DYN_STATUS_FLAG_PDB_PROFILE_ACTIVE)) { parts << QStringLiteral("PdbProfileActive"); }
        return parts.isEmpty() ? QStringLiteral("None") : parts.join(QStringLiteral(", "));
    }

    QString featureFlagText(const std::uint32_t flags)
    {
        QStringList parts;
        if (flagEnabled(flags, KSWORD_ARK_FEATURE_FLAG_REQUIRES_DYNDATA)) { parts << QStringLiteral("Requires DynData"); }
        if (flagEnabled(flags, KSWORD_ARK_FEATURE_FLAG_MUTATING)) { parts << QStringLiteral("Mutating"); }
        if (flagEnabled(flags, KSWORD_ARK_FEATURE_FLAG_KERNEL_ONLY)) { parts << QStringLiteral("Kernel Only"); }
        if (flagEnabled(flags, KSWORD_ARK_FEATURE_FLAG_READ_ONLY)) { parts << QStringLiteral("Read Only"); }
        if (flagEnabled(flags, KSWORD_ARK_FEATURE_FLAG_POLICY_GATED)) { parts << QStringLiteral("Policy Gated"); }
        return parts.isEmpty() ? QStringLiteral("None") : parts.join(QStringLiteral(", "));
    }

    QString stateText(const std::uint32_t state, const QString& fallbackText)
    {
        switch (state)
        {
        case KSWORD_ARK_FEATURE_STATE_AVAILABLE: return QStringLiteral("Available");
        case KSWORD_ARK_FEATURE_STATE_UNAVAILABLE: return QStringLiteral("Unavailable");
        case KSWORD_ARK_FEATURE_STATE_DEGRADED: return QStringLiteral("Degraded");
        case KSWORD_ARK_FEATURE_STATE_DENIED_BY_POLICY: return QStringLiteral("Denied by policy");
        default: return safeText(fallbackText, QStringLiteral("Unknown"));
        }
    }

    QBrush stateBrush(const std::uint32_t state)
    {
        if (state == KSWORD_ARK_FEATURE_STATE_AVAILABLE) { return QBrush(QColor(QStringLiteral("#3A8F3A"))); }
        if (state == KSWORD_ARK_FEATURE_STATE_DEGRADED) { return QBrush(QColor(QStringLiteral("#D77A00"))); }
        if (state == KSWORD_ARK_FEATURE_STATE_DENIED_BY_POLICY) { return QBrush(QColor(QStringLiteral("#7A4DB3"))); }
        return QBrush(QColor(QStringLiteral("#B23A3A")));
    }

    void appendSummaryRow(QTableWidget* table, const QString& nameText, const QString& valueText)
    {
        if (table == nullptr) { return; }
        const int row = table->rowCount();
        table->insertRow(row);
        auto* nameItem = new QTableWidgetItem(nameText);
        auto* valueItem = new QTableWidgetItem(valueText);
        nameItem->setFlags(nameItem->flags() & ~Qt::ItemIsEditable);
        valueItem->setFlags(valueItem->flags() & ~Qt::ItemIsEditable);
        table->setItem(row, static_cast<int>(DriverSummaryColumn::Name), nameItem);
        table->setItem(row, static_cast<int>(DriverSummaryColumn::Value), valueItem);
    }

    void setReadonlyItem(QTableWidget* table, const int row, const DriverCapabilityColumn column, QTableWidgetItem* item)
    {
        if (table == nullptr || item == nullptr) { delete item; return; }
        item->setFlags(item->flags() & ~Qt::ItemIsEditable);
        table->setItem(row, static_cast<int>(column), item);
    }

    QString buildCapabilityDetail(const KernelDriverCapabilityEntry& entry, const KernelDriverStatusSummary& summary)
    {
        QStringList lines;
        lines << QStringLiteral("功能: %1").arg(safeText(entry.featureNameText));
        lines << QStringLiteral("FeatureId: %1").arg(entry.featureId);
        lines << QStringLiteral("状态: %1").arg(stateText(entry.state, entry.stateNameText));
        lines << QStringLiteral("功能标志: %1 (%2)").arg(formatHex32(entry.flags), featureFlagText(entry.flags));
        lines << QStringLiteral("");
        lines << QStringLiteral("依赖字段: %1").arg(safeText(entry.dependencyText, QStringLiteral("None")));
        lines << QStringLiteral("状态原因: %1").arg(safeText(entry.reasonText, QStringLiteral("Feature is available.")));
        lines << QStringLiteral("");
        lines << QStringLiteral("所需安全策略: %1 (%2)").arg(formatHex32(entry.requiredPolicyFlags), policyNames(entry.requiredPolicyFlags));
        lines << QStringLiteral("被拒绝策略位: %1 (%2)").arg(formatHex32(entry.deniedPolicyFlags), policyNames(entry.deniedPolicyFlags));
        lines << QStringLiteral("所需 DynData capability: %1 (%2)").arg(formatHex64(entry.requiredDynDataMask), capabilityNames(entry.requiredDynDataMask));
        lines << QStringLiteral("已满足 DynData capability: %1 (%2)").arg(formatHex64(entry.presentDynDataMask), capabilityNames(entry.presentDynDataMask));
        lines << QStringLiteral("全局 DynData capability: %1 (%2)").arg(formatHex64(summary.dynDataCapabilityMask), capabilityNames(summary.dynDataCapabilityMask));
        lines << QStringLiteral("");
        lines << QStringLiteral("当前内核: %1").arg(kernelIdentityText(summary));
        lines << QStringLiteral("识别版本: %1").arg(kernelVersionText(summary));
        lines << QStringLiteral("本地 PDB profile: %1").arg(localPdbProfileText(summary));
        lines << QStringLiteral("可信偏移: %1").arg(trustedOffsetText(summary));
        lines << QStringLiteral("字段覆盖: %1").arg(fieldCoverageText(summary));
        lines << QStringLiteral("字段来源: %1").arg(fieldSourceSummaryText(summary));
        lines << QStringLiteral("");
        lines << QStringLiteral("驱动状态: %1").arg(statusBadges(summary));
        lines << QStringLiteral("最近 R0 错误: %1 / %2 / %3")
            .arg(formatNtStatus(summary.lastErrorStatus))
            .arg(safeText(summary.lastErrorSourceText, QStringLiteral("None")))
            .arg(safeText(summary.lastErrorSummaryText, QStringLiteral("None")));
        return lines.join(QStringLiteral("\n"));
    }

    QString buildDriverStatusReport(const KernelDriverStatusSummary& summary, const std::vector<KernelDriverCapabilityEntry>& rows)
    {
        QStringList lines;
        lines << QStringLiteral("Ksword Driver Capability Diagnostic Report");
        lines << QStringLiteral("Status: %1").arg(statusBadges(summary));
        lines << QStringLiteral("QueryOk: %1").arg(boolText(summary.queryOk));
        lines << QStringLiteral("IoMessage: %1").arg(safeText(summary.ioMessageText));
        lines << QStringLiteral("CapabilityProtocolVersion: %1").arg(summary.version);
        lines << QStringLiteral("DriverProtocolVersion: %1").arg(formatHex32(summary.driverProtocolVersion));
        lines << QStringLiteral("ExpectedDriverProtocolVersion: %1").arg(formatHex32(KSWORD_ARK_DRIVER_PROTOCOL_VERSION));
        lines << QStringLiteral("StatusFlags: %1").arg(formatHex32(summary.statusFlags));
        lines << QStringLiteral("SecurityPolicyFlags: %1 (%2)").arg(formatHex32(summary.securityPolicyFlags)).arg(policyNames(summary.securityPolicyFlags));
        lines << QStringLiteral("DynDataStatusFlags: %1 (%2)").arg(formatHex32(summary.dynDataStatusFlags)).arg(dynDataStatusText(summary.dynDataStatusFlags));
        lines << QStringLiteral("DynDataCapabilityMask: %1 (%2)").arg(formatHex64(summary.dynDataCapabilityMask)).arg(capabilityNames(summary.dynDataCapabilityMask));
        lines << QStringLiteral("DynDataStatusQueryOk: %1").arg(boolText(summary.dynDataStatusQueryOk));
        lines << QStringLiteral("DynDataFieldsQueryOk: %1").arg(boolText(summary.dynDataFieldsQueryOk));
        lines << QStringLiteral("CurrentKernel: %1").arg(kernelIdentityText(summary));
        lines << QStringLiteral("RecognizedVersion: %1").arg(kernelVersionText(summary));
        lines << QStringLiteral("LocalPdbProfileMatched: %1").arg(boolText(summary.localPdbProfileMatched));
        lines << QStringLiteral("LocalPdbProfileName: %1").arg(safeText(summary.localPdbProfileNameText, QStringLiteral("None")));
        lines << QStringLiteral("LocalPdbProfilePath: %1").arg(safeText(summary.localPdbProfilePathText, QStringLiteral("None")));
        lines << QStringLiteral("LocalPdbProfileMessage: %1").arg(safeText(summary.localPdbProfileMessageText, QStringLiteral("None")));
        lines << QStringLiteral("PdbProfileActive: %1").arg(boolText(summary.pdbProfileActive));
        lines << QStringLiteral("TrustedPdbOffsetsActive: %1").arg(boolText(summary.trustedPdbOffsetsActive));
        lines << QStringLiteral("TrustedOffsetSummary: %1").arg(trustedOffsetText(summary));
        lines << QStringLiteral("FieldCoverage: %1").arg(fieldCoverageText(summary));
        lines << QStringLiteral("FieldSources: %1").arg(fieldSourceSummaryText(summary));
        lines << QStringLiteral("SystemInformerData: version=%1 length=%2")
            .arg(summary.dynDataSystemInformerDataVersion)
            .arg(summary.dynDataSystemInformerDataLength);
        lines << QStringLiteral("MatchedProfile: class=%1 (%2) offset=%3 fieldsId=%4")
            .arg(moduleClassText(summary.dynDataMatchedProfileClass))
            .arg(summary.dynDataMatchedProfileClass)
            .arg(formatHex32(summary.dynDataMatchedProfileOffset))
            .arg(summary.dynDataMatchedFieldsId);
        lines << QStringLiteral("DynDataUnavailableReason: %1").arg(safeText(summary.dynDataUnavailableReasonText, QStringLiteral("None")));
        lines << QStringLiteral("DynDataIo: %1").arg(dynDataIoText(summary));
        lines << QStringLiteral("LastError: %1 / %2 / %3").arg(formatNtStatus(summary.lastErrorStatus)).arg(safeText(summary.lastErrorSourceText, QStringLiteral("None"))).arg(safeText(summary.lastErrorSummaryText, QStringLiteral("None")));
        lines << QStringLiteral("FeatureCount: returned=%1 total=%2").arg(summary.returnedFeatureCount).arg(summary.totalFeatureCount);
        lines << QStringLiteral("\nFeatures:");
        for (const KernelDriverCapabilityEntry& entry : rows)
        {
            lines << QStringLiteral("%1\t%2\tpolicy=%3\tdynRequired=%4\tdynPresent=%5\t%6\t%7")
                .arg(safeText(entry.featureNameText)).arg(stateText(entry.state, entry.stateNameText))
                .arg(formatHex32(entry.requiredPolicyFlags)).arg(formatHex64(entry.requiredDynDataMask))
                .arg(formatHex64(entry.presentDynDataMask)).arg(safeText(entry.dependencyText, QStringLiteral("None")))
                .arg(safeText(entry.reasonText, QStringLiteral("None")));
        }
        return lines.join(QStringLiteral("\n"));
    }

    void populateSummaryTable(QTableWidget* table, const KernelDriverStatusSummary& summary, const std::size_t visibleRows)
    {
        if (table == nullptr) { return; }
        table->setSortingEnabled(false);
        table->setRowCount(0);
        appendSummaryRow(table, QStringLiteral("状态栏"), statusBadges(summary));
        appendSummaryRow(table, QStringLiteral("Driver Loaded"), boolText(summary.driverLoaded));
        appendSummaryRow(table, QStringLiteral("Protocol OK"), boolText(summary.protocolOk));
        appendSummaryRow(table, QStringLiteral("DynData Missing"), boolText(summary.dynDataMissing));
        appendSummaryRow(table, QStringLiteral("Limited"), boolText(summary.limited));
        appendSummaryRow(table, QStringLiteral("当前内核"), kernelIdentityText(summary));
        appendSummaryRow(table, QStringLiteral("识别版本"), kernelVersionText(summary));
        appendSummaryRow(table, QStringLiteral("本地 PDB profile"), localPdbProfileText(summary));
        appendSummaryRow(table, QStringLiteral("可信偏移"), trustedOffsetText(summary));
        appendSummaryRow(table, QStringLiteral("字段覆盖"), fieldCoverageText(summary));
        appendSummaryRow(table, QStringLiteral("字段来源"), fieldSourceSummaryText(summary));
        appendSummaryRow(table, QStringLiteral("缺失必需字段"), QString::number(summary.dynDataRequiredMissingCount));
        appendSummaryRow(table, QStringLiteral("能力协议版本"), QString::number(summary.version));
        appendSummaryRow(table, QStringLiteral("驱动协议版本"), formatHex32(summary.driverProtocolVersion));
        appendSummaryRow(table, QStringLiteral("期望协议版本"), formatHex32(KSWORD_ARK_DRIVER_PROTOCOL_VERSION));
        appendSummaryRow(table, QStringLiteral("状态位"), formatHex32(summary.statusFlags));
        appendSummaryRow(table, QStringLiteral("安全策略位"), QStringLiteral("%1 (%2)").arg(formatHex32(summary.securityPolicyFlags)).arg(policyNames(summary.securityPolicyFlags)));
        appendSummaryRow(table, QStringLiteral("DynData 状态位"), QStringLiteral("%1 (%2)").arg(formatHex32(summary.dynDataStatusFlags)).arg(dynDataStatusText(summary.dynDataStatusFlags)));
        appendSummaryRow(table, QStringLiteral("DynData 能力位"), QStringLiteral("%1 (%2)").arg(formatHex64(summary.dynDataCapabilityMask)).arg(capabilityNames(summary.dynDataCapabilityMask)));
        appendSummaryRow(table, QStringLiteral("System Informer 数据"), QStringLiteral("version=%1 length=%2")
            .arg(summary.dynDataSystemInformerDataVersion)
            .arg(summary.dynDataSystemInformerDataLength));
        appendSummaryRow(table, QStringLiteral("匹配内置 Profile"), QStringLiteral("class=%1 (%2) offset=%3 fieldsId=%4")
            .arg(moduleClassText(summary.dynDataMatchedProfileClass))
            .arg(summary.dynDataMatchedProfileClass)
            .arg(formatHex32(summary.dynDataMatchedProfileOffset))
            .arg(summary.dynDataMatchedFieldsId));
        appendSummaryRow(table, QStringLiteral("DynData R3 IO"), dynDataIoText(summary));
        appendSummaryRow(table, QStringLiteral("DynData 不可用原因"), safeText(summary.dynDataUnavailableReasonText, QStringLiteral("None")));
        appendSummaryRow(table, QStringLiteral("功能数"), QStringLiteral("显示 %1 / 返回 %2 / 总计 %3").arg(visibleRows).arg(summary.returnedFeatureCount).arg(summary.totalFeatureCount));
        appendSummaryRow(table, QStringLiteral("最近错误"), QStringLiteral("%1 / %2 / %3").arg(formatNtStatus(summary.lastErrorStatus)).arg(safeText(summary.lastErrorSourceText, QStringLiteral("None"))).arg(safeText(summary.lastErrorSummaryText, QStringLiteral("None"))));
        appendSummaryRow(table, QStringLiteral("R3 IO"), safeText(summary.ioMessageText));
    }

    // buildDriverStatusLabelText：
    // - 输入 summary 和 capability 行数；
    // - 处理：为顶部状态栏生成一句可读摘要；
    // - 返回：适合 QLabel 直接显示的状态文本。
    QString buildDriverStatusLabelText(const KernelDriverStatusSummary& summary, const std::size_t capabilityCount)
    {
        const QString kernelText = summary.ntoskrnlIdentityPresent
            ? QStringLiteral("%1 / %2").arg(safeText(summary.ntoskrnlModuleNameText), kernelVersionText(summary))
            : QStringLiteral("内核未识别");
        const QString offsetText = trustedOffsetText(summary);
        if (!summary.queryOk && !summary.dynDataStatusQueryOk)
        {
            return QStringLiteral("状态：驱动与 DynData 查询均失败");
        }

        return QStringLiteral("状态：%1；%2；功能 %3 项；可信偏移 %4")
            .arg(statusBadges(summary))
            .arg(kernelText)
            .arg(capabilityCount)
            .arg(offsetText);
    }

    bool shouldShowCapability(const KernelDriverCapabilityEntry& entry, const QString& filter)
    {
        if (filter.isEmpty()) { return true; }
        return entry.featureNameText.contains(filter, Qt::CaseInsensitive) ||
            stateText(entry.state, entry.stateNameText).contains(filter, Qt::CaseInsensitive) ||
            entry.dependencyText.contains(filter, Qt::CaseInsensitive) ||
            entry.reasonText.contains(filter, Qt::CaseInsensitive) ||
            featureFlagText(entry.flags).contains(filter, Qt::CaseInsensitive) ||
            policyNames(entry.requiredPolicyFlags).contains(filter, Qt::CaseInsensitive) ||
            capabilityNames(entry.requiredDynDataMask).contains(filter, Qt::CaseInsensitive) ||
            formatHex64(entry.requiredDynDataMask).contains(filter, Qt::CaseInsensitive);
    }

    bool queryDriverStatusSnapshot(KernelDriverStatusSummary& summaryOut, std::vector<KernelDriverCapabilityEntry>& rowsOut)
    {
        ksword::ark::DriverClient client;
        const ksword::ark::DriverCapabilitiesQueryResult queryResult = client.queryDriverCapabilities();
        const ksword::ark::DynDataStatusResult statusResult = client.queryDynDataStatus();
        const ksword::ark::DynDataFieldsResult fieldsResult = client.queryDynDataFields();

        summaryOut = KernelDriverStatusSummary{};
        rowsOut.clear();
        summaryOut.queryOk = queryResult.io.ok;
        summaryOut.driverLoaded = queryResult.io.ok || queryResult.io.win32Error != ERROR_FILE_NOT_FOUND;
        summaryOut.ioMessageText = QString::fromStdString(queryResult.io.message);

        summaryOut.dynDataStatusQueryOk = statusResult.io.ok;
        summaryOut.dynDataFieldsQueryOk = fieldsResult.io.ok;
        summaryOut.dynDataStatusIoMessageText = QString::fromStdString(statusResult.io.message);
        summaryOut.dynDataFieldsIoMessageText = QString::fromStdString(fieldsResult.io.message);

        if (queryResult.io.ok)
        {
            summaryOut.version = queryResult.version;
            summaryOut.driverProtocolVersion = queryResult.driverProtocolVersion;
            summaryOut.statusFlags = queryResult.statusFlags;
            summaryOut.securityPolicyFlags = queryResult.securityPolicyFlags;
            summaryOut.dynDataStatusFlags = queryResult.dynDataStatusFlags;
            summaryOut.lastErrorStatus = queryResult.lastErrorStatus;
            summaryOut.totalFeatureCount = queryResult.totalFeatureCount;
            summaryOut.returnedFeatureCount = queryResult.returnedFeatureCount;
            summaryOut.dynDataCapabilityMask = queryResult.dynDataCapabilityMask;
            summaryOut.lastErrorSourceText = stringToQString(queryResult.lastErrorSource);
            summaryOut.lastErrorSummaryText = stringToQString(queryResult.lastErrorSummary);
        }
        else if (statusResult.io.ok)
        {
            summaryOut.dynDataStatusFlags = statusResult.statusFlags;
            summaryOut.dynDataCapabilityMask = statusResult.capabilityMask;
            summaryOut.lastErrorStatus = statusResult.lastStatus;
        }

        summaryOut.driverLoaded = flagEnabled(summaryOut.statusFlags, KSWORD_ARK_DRIVER_STATUS_FLAG_DRIVER_LOADED) ||
            queryResult.io.ok ||
            statusResult.io.ok ||
            fieldsResult.io.ok;
        summaryOut.protocolOk = (queryResult.io.ok &&
            flagEnabled(summaryOut.statusFlags, KSWORD_ARK_DRIVER_STATUS_FLAG_PROTOCOL_OK) &&
            summaryOut.driverProtocolVersion == KSWORD_ARK_DRIVER_PROTOCOL_VERSION);

        if (statusResult.io.ok)
        {
            summaryOut.dynDataStatusFlags = statusResult.statusFlags;
            summaryOut.dynDataSystemInformerDataVersion = statusResult.systemInformerDataVersion;
            summaryOut.dynDataSystemInformerDataLength = statusResult.systemInformerDataLength;
            summaryOut.dynDataMatchedProfileClass = statusResult.matchedProfileClass;
            summaryOut.dynDataMatchedProfileOffset = statusResult.matchedProfileOffset;
            summaryOut.dynDataMatchedFieldsId = statusResult.matchedFieldsId;
            summaryOut.dynDataFieldCount = statusResult.fieldCount;
            summaryOut.dynDataCapabilityMask = statusResult.capabilityMask;
            summaryOut.ntoskrnlIdentityPresent = statusResult.ntoskrnl.present;
            summaryOut.ntoskrnlClassId = statusResult.ntoskrnl.classId;
            summaryOut.ntoskrnlMachine = statusResult.ntoskrnl.machine;
            summaryOut.ntoskrnlTimeDateStamp = statusResult.ntoskrnl.timeDateStamp;
            summaryOut.ntoskrnlSizeOfImage = statusResult.ntoskrnl.sizeOfImage;
            summaryOut.ntoskrnlImageBase = statusResult.ntoskrnl.imageBase;
            summaryOut.ntoskrnlModuleNameText = wideStringToQString(statusResult.ntoskrnl.moduleName);
            summaryOut.dynDataUnavailableReasonText = wideStringToQString(statusResult.unavailableReason);
        }

        if (fieldsResult.io.ok)
        {
            if (summaryOut.dynDataFieldCount == 0U)
            {
                summaryOut.dynDataFieldCount = fieldsResult.totalCount;
            }
            summaryOut.dynDataReturnedFieldCount = fieldsResult.returnedCount != 0U
                ? fieldsResult.returnedCount
                : static_cast<std::uint32_t>(fieldsResult.entries.size());
            for (const ksword::ark::DynDataFieldEntry& entry : fieldsResult.entries)
            {
                if (fieldOffsetPresent(entry.flags, entry.offset))
                {
                    summaryOut.dynDataPresentFieldCount += 1U;
                    switch (entry.source)
                    {
                    case KSW_DYN_FIELD_SOURCE_PDB_PROFILE:
                        summaryOut.dynDataPdbProfileFieldCount += 1U;
                        break;
                    case KSW_DYN_FIELD_SOURCE_RUNTIME_PATTERN:
                        summaryOut.dynDataRuntimePatternFieldCount += 1U;
                        break;
                    case KSW_DYN_FIELD_SOURCE_SYSTEM_INFORMER:
                        summaryOut.dynDataSystemInformerFieldCount += 1U;
                        break;
                    case KSW_DYN_FIELD_SOURCE_KSWORD_EXTRA_TABLE:
                        summaryOut.dynDataExtraTableFieldCount += 1U;
                        break;
                    default:
                        summaryOut.dynDataUnavailableFieldCount += 1U;
                        break;
                    }
                }
                else
                {
                    summaryOut.dynDataUnavailableFieldCount += 1U;
                }

                if ((entry.flags & KSW_DYN_FIELD_FLAG_REQUIRED) != 0U && !fieldOffsetPresent(entry.flags, entry.offset))
                {
                    summaryOut.dynDataRequiredMissingCount += 1U;
                }
            }
        }

        if (statusResult.io.ok && statusResult.ntoskrnl.present)
        {
            const LocalPdbPackMatch packMatch = findMatchingLocalPdbProfilePack(statusResult.ntoskrnl);
            summaryOut.localPdbProfileMatched = packMatch.matched && packMatch.valid;
            summaryOut.localPdbProfilePackProfileCount = packMatch.profileCount;
            summaryOut.localPdbProfileFieldCount = packMatch.fieldCount;
            summaryOut.localPdbProfileNameText = packMatch.profileNameText;
            summaryOut.localPdbProfileVersionText = packMatch.versionText;
            summaryOut.localPdbProfilePathText = packMatch.pathText;
            summaryOut.localPdbProfileMessageText = packMatch.messageText;
        }

        summaryOut.pdbProfileActive = flagEnabled(summaryOut.dynDataStatusFlags, KSW_DYN_STATUS_FLAG_PDB_PROFILE_ACTIVE);
        summaryOut.trustedPdbOffsetsActive = summaryOut.pdbProfileActive || summaryOut.dynDataPdbProfileFieldCount > 0U;
        summaryOut.dynDataMissing = !summaryOut.dynDataStatusQueryOk ||
            !flagEnabled(summaryOut.dynDataStatusFlags, KSW_DYN_STATUS_FLAG_NTOS_ACTIVE);
        summaryOut.limited = !summaryOut.protocolOk ||
            flagEnabled(summaryOut.statusFlags, KSWORD_ARK_DRIVER_STATUS_FLAG_LIMITED) ||
            !summaryOut.dynDataStatusQueryOk;

        rowsOut.reserve(queryResult.entries.size());
        for (const ksword::ark::DriverFeatureCapabilityEntry& sourceEntry : queryResult.entries)
        {
            KernelDriverCapabilityEntry row{};
            row.featureId = sourceEntry.featureId;
            row.state = sourceEntry.state;
            row.flags = sourceEntry.flags;
            row.requiredPolicyFlags = sourceEntry.requiredPolicyFlags;
            row.deniedPolicyFlags = sourceEntry.deniedPolicyFlags;
            row.requiredDynDataMask = sourceEntry.requiredDynDataMask;
            row.presentDynDataMask = sourceEntry.presentDynDataMask;
            row.featureNameText = stringToQString(sourceEntry.featureName);
            row.stateNameText = stateText(sourceEntry.state, stringToQString(sourceEntry.stateName));
            row.dependencyText = stringToQString(sourceEntry.dependencyText);
            row.reasonText = stringToQString(sourceEntry.reasonText);
            row.detailText = buildCapabilityDetail(row, summaryOut);
            rowsOut.push_back(std::move(row));
        }

        return summaryOut.queryOk && summaryOut.protocolOk;
    }
}

void KernelDock::initializeDriverStatusTab()
{
    if (m_driverStatusPage == nullptr || m_driverStatusLayout != nullptr) { return; }

    m_driverStatusLayout = new QVBoxLayout(m_driverStatusPage);
    m_driverStatusLayout->setContentsMargins(4, 4, 4, 4);
    m_driverStatusLayout->setSpacing(6);

    m_driverStatusToolLayout = new QHBoxLayout();
    m_driverStatusToolLayout->setContentsMargins(0, 0, 0, 0);
    m_driverStatusToolLayout->setSpacing(6);

    m_refreshDriverStatusButton = new QPushButton(QIcon(QStringLiteral(":/Icon/process_refresh.svg")), QString(), m_driverStatusPage);
    m_refreshDriverStatusButton->setToolTip(QStringLiteral("刷新 KswordARK 驱动状态、协议、安全策略和能力矩阵"));
    m_refreshDriverStatusButton->setStyleSheet(blueButtonStyle());
    m_refreshDriverStatusButton->setFixedWidth(34);

    m_copyDriverStatusReportButton = new QPushButton(QIcon(QStringLiteral(":/Icon/process_copy_row.svg")), QStringLiteral("复制诊断"), m_driverStatusPage);
    m_copyDriverStatusReportButton->setToolTip(QStringLiteral("复制统一驱动状态和能力矩阵诊断报告"));
    m_copyDriverStatusReportButton->setStyleSheet(blueButtonStyle());

    m_driverStatusFilterEdit = new QLineEdit(m_driverStatusPage);
    m_driverStatusFilterEdit->setPlaceholderText(QStringLiteral("按功能/状态/策略/DynData capability/依赖字段筛选"));
    m_driverStatusFilterEdit->setToolTip(QStringLiteral("输入关键字后实时过滤驱动能力矩阵"));
    m_driverStatusFilterEdit->setClearButtonEnabled(true);
    m_driverStatusFilterEdit->setStyleSheet(blueInputStyle());

    m_driverStatusLabel = new QLabel(QStringLiteral("状态：等待刷新"), m_driverStatusPage);
    m_driverStatusLabel->setStyleSheet(statusLabelStyle(KswordTheme::TextSecondaryHex()));

    m_driverStatusToolLayout->addWidget(m_refreshDriverStatusButton, 0);
    m_driverStatusToolLayout->addWidget(m_copyDriverStatusReportButton, 0);
    m_driverStatusToolLayout->addWidget(m_driverStatusFilterEdit, 1);
    m_driverStatusToolLayout->addWidget(m_driverStatusLabel, 0);
    m_driverStatusLayout->addLayout(m_driverStatusToolLayout);

    QSplitter* verticalSplitter = new QSplitter(Qt::Vertical, m_driverStatusPage);
    m_driverStatusLayout->addWidget(verticalSplitter, 1);

    m_driverStatusSummaryTable = new QTableWidget(verticalSplitter);
    m_driverStatusSummaryTable->setColumnCount(static_cast<int>(DriverSummaryColumn::Count));
    m_driverStatusSummaryTable->setHorizontalHeaderLabels(QStringList{ QStringLiteral("项目"), QStringLiteral("值") });
    m_driverStatusSummaryTable->setSelectionMode(QAbstractItemView::NoSelection);
    m_driverStatusSummaryTable->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_driverStatusSummaryTable->setAlternatingRowColors(true);
    m_driverStatusSummaryTable->setStyleSheet(itemSelectionStyle());
    m_driverStatusSummaryTable->setCornerButtonEnabled(false);
    m_driverStatusSummaryTable->verticalHeader()->setVisible(false);
    m_driverStatusSummaryTable->horizontalHeader()->setStyleSheet(headerStyle());
    m_driverStatusSummaryTable->horizontalHeader()->setSectionResizeMode(QHeaderView::ResizeToContents);
    m_driverStatusSummaryTable->horizontalHeader()->setSectionResizeMode(static_cast<int>(DriverSummaryColumn::Value), QHeaderView::Stretch);
    m_driverStatusSummaryTable->setColumnWidth(static_cast<int>(DriverSummaryColumn::Name), 220);

    QSplitter* lowerSplitter = new QSplitter(Qt::Horizontal, verticalSplitter);
    m_driverCapabilityTable = new QTableWidget(lowerSplitter);
    m_driverCapabilityTable->setColumnCount(static_cast<int>(DriverCapabilityColumn::Count));
    m_driverCapabilityTable->setHorizontalHeaderLabels(QStringList{
        QStringLiteral("功能"), QStringLiteral("状态"), QStringLiteral("策略"),
        QStringLiteral("所需DynData"), QStringLiteral("已满足DynData"), QStringLiteral("依赖字段"), QStringLiteral("原因") });
    m_driverCapabilityTable->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_driverCapabilityTable->setSelectionMode(QAbstractItemView::SingleSelection);
    m_driverCapabilityTable->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_driverCapabilityTable->setAlternatingRowColors(true);
    m_driverCapabilityTable->setStyleSheet(itemSelectionStyle());
    m_driverCapabilityTable->setCornerButtonEnabled(false);
    m_driverCapabilityTable->verticalHeader()->setVisible(false);
    m_driverCapabilityTable->horizontalHeader()->setStyleSheet(headerStyle());
    m_driverCapabilityTable->horizontalHeader()->setSectionResizeMode(QHeaderView::ResizeToContents);
    m_driverCapabilityTable->horizontalHeader()->setSectionResizeMode(static_cast<int>(DriverCapabilityColumn::Feature), QHeaderView::Stretch);
    m_driverCapabilityTable->setColumnWidth(static_cast<int>(DriverCapabilityColumn::State), 140);
    m_driverCapabilityTable->setColumnWidth(static_cast<int>(DriverCapabilityColumn::RequiredDyn), 180);
    m_driverCapabilityTable->setColumnWidth(static_cast<int>(DriverCapabilityColumn::PresentDyn), 180);
    m_driverCapabilityTable->setColumnWidth(static_cast<int>(DriverCapabilityColumn::Dependency), 280);

    m_driverCapabilityDetailEditor = new CodeEditorWidget(lowerSplitter);
    m_driverCapabilityDetailEditor->setReadOnly(true);
    m_driverCapabilityDetailEditor->setText(QStringLiteral("请选择一条驱动功能能力查看依赖字段和诊断详情。"));

    verticalSplitter->setStretchFactor(0, 2);
    verticalSplitter->setStretchFactor(1, 5);
    lowerSplitter->setStretchFactor(0, 3);
    lowerSplitter->setStretchFactor(1, 2);

    connect(m_refreshDriverStatusButton, &QPushButton::clicked, this, [this]() { refreshDriverStatusAsync(); });
    connect(m_copyDriverStatusReportButton, &QPushButton::clicked, this, [this]() {
        QClipboard* clipboard = QApplication::clipboard();
        if (clipboard != nullptr)
        {
            clipboard->setText(buildDriverStatusReport(m_driverStatusSummary, m_driverCapabilityRows));
            m_driverStatusLabel->setText(QStringLiteral("状态：诊断报告已复制"));
        }
    });
    connect(m_driverStatusFilterEdit, &QLineEdit::textChanged, this, [this](const QString& filterText) {
        rebuildDriverCapabilityTable(filterText.trimmed());
    });
    connect(m_driverCapabilityTable, &QTableWidget::currentCellChanged, this, [this](int, int, int, int) {
        showDriverCapabilityDetailByCurrentRow();
    });
}

void KernelDock::refreshDriverStatusAsync()
{
    if (m_driverStatusRefreshRunning.exchange(true))
    {
        kLogEvent skipEvent;
        dbg << skipEvent << "[KernelDock] 驱动状态刷新被忽略：已有任务运行。" << eol;
        return;
    }

    m_refreshDriverStatusButton->setEnabled(false);
    m_driverStatusLabel->setText(QStringLiteral("状态：刷新中..."));
    m_driverStatusLabel->setStyleSheet(statusLabelStyle(KswordTheme::PrimaryBlueHex));

    QPointer<KernelDock> guardThis(this);
    std::thread([guardThis]() {
        KernelDriverStatusSummary summary;
        std::vector<KernelDriverCapabilityEntry> rows;
        const bool success = queryDriverStatusSnapshot(summary, rows);

        QMetaObject::invokeMethod(guardThis, [guardThis, success, summary = std::move(summary), rows = std::move(rows)]() mutable {
            if (guardThis == nullptr) { return; }

            guardThis->m_driverStatusRefreshRunning.store(false);
            guardThis->m_refreshDriverStatusButton->setEnabled(true);
            guardThis->m_driverStatusSummary = std::move(summary);
            guardThis->m_driverCapabilityRows = std::move(rows);
            populateSummaryTable(guardThis->m_driverStatusSummaryTable, guardThis->m_driverStatusSummary, guardThis->m_driverCapabilityRows.size());
            guardThis->rebuildDriverCapabilityTable(guardThis->m_driverStatusFilterEdit->text().trimmed());

            if (!success)
            {
                guardThis->m_driverStatusLabel->setText(buildDriverStatusLabelText(
                    guardThis->m_driverStatusSummary,
                    guardThis->m_driverCapabilityRows.size()));
                guardThis->m_driverStatusLabel->setStyleSheet(statusLabelStyle(QStringLiteral("#B23A3A")));
                guardThis->m_driverCapabilityDetailEditor->setText(buildDriverStatusReport(guardThis->m_driverStatusSummary, guardThis->m_driverCapabilityRows));
                return;
            }

            const std::size_t unavailableCount = static_cast<std::size_t>(std::count_if(
                guardThis->m_driverCapabilityRows.begin(),
                guardThis->m_driverCapabilityRows.end(),
                [](const KernelDriverCapabilityEntry& entry) { return entry.state != KSWORD_ARK_FEATURE_STATE_AVAILABLE; }));
            guardThis->m_driverStatusLabel->setText(buildDriverStatusLabelText(
                guardThis->m_driverStatusSummary,
                guardThis->m_driverCapabilityRows.size()));
            const bool healthyOffsets = guardThis->m_driverStatusSummary.trustedPdbOffsetsActive &&
                guardThis->m_driverStatusSummary.dynDataRequiredMissingCount == 0U;
            guardThis->m_driverStatusLabel->setStyleSheet(statusLabelStyle(
                unavailableCount == 0U && healthyOffsets ? QStringLiteral("#3A8F3A") : QStringLiteral("#D77A00")));

            if (guardThis->m_driverCapabilityTable->rowCount() > 0)
            {
                guardThis->m_driverCapabilityTable->setCurrentCell(0, 0);
            }
            else
            {
                guardThis->m_driverCapabilityDetailEditor->setText(QStringLiteral("当前筛选条件下没有驱动能力记录。"));
            }
        }, Qt::QueuedConnection);
    }).detach();
}

void KernelDock::rebuildDriverCapabilityTable(const QString& filterKeyword)
{
    if (m_driverCapabilityTable == nullptr) { return; }

    m_driverCapabilityTable->setSortingEnabled(false);
    m_driverCapabilityTable->setRowCount(0);

    for (std::size_t sourceIndex = 0U; sourceIndex < m_driverCapabilityRows.size(); ++sourceIndex)
    {
        const KernelDriverCapabilityEntry& entry = m_driverCapabilityRows[sourceIndex];
        if (!shouldShowCapability(entry, filterKeyword)) { continue; }

        const int row = m_driverCapabilityTable->rowCount();
        m_driverCapabilityTable->insertRow(row);
        auto* featureItem = new QTableWidgetItem(safeText(entry.featureNameText));
        auto* stateItem = new QTableWidgetItem(stateText(entry.state, entry.stateNameText));
        auto* policyItem = new QTableWidgetItem(formatHex32(entry.requiredPolicyFlags));
        auto* requiredItem = new QTableWidgetItem(formatHex64(entry.requiredDynDataMask));
        auto* presentItem = new QTableWidgetItem(formatHex64(entry.presentDynDataMask));
        auto* dependencyItem = new QTableWidgetItem(safeText(entry.dependencyText, QStringLiteral("None")));
        auto* reasonItem = new QTableWidgetItem(safeText(entry.reasonText, QStringLiteral("None")));

        featureItem->setData(Qt::UserRole, static_cast<qulonglong>(sourceIndex));
        stateItem->setForeground(stateBrush(entry.state));
        if (entry.deniedPolicyFlags != 0U) { policyItem->setForeground(QBrush(QColor(QStringLiteral("#B23A3A")))); }
        if (entry.requiredDynDataMask != 0ULL && entry.presentDynDataMask != entry.requiredDynDataMask)
        {
            presentItem->setForeground(QBrush(QColor(QStringLiteral("#B23A3A"))));
        }

        setReadonlyItem(m_driverCapabilityTable, row, DriverCapabilityColumn::Feature, featureItem);
        setReadonlyItem(m_driverCapabilityTable, row, DriverCapabilityColumn::State, stateItem);
        setReadonlyItem(m_driverCapabilityTable, row, DriverCapabilityColumn::Policy, policyItem);
        setReadonlyItem(m_driverCapabilityTable, row, DriverCapabilityColumn::RequiredDyn, requiredItem);
        setReadonlyItem(m_driverCapabilityTable, row, DriverCapabilityColumn::PresentDyn, presentItem);
        setReadonlyItem(m_driverCapabilityTable, row, DriverCapabilityColumn::Dependency, dependencyItem);
        setReadonlyItem(m_driverCapabilityTable, row, DriverCapabilityColumn::Reason, reasonItem);
    }

    m_driverCapabilityTable->setSortingEnabled(true);
    populateSummaryTable(m_driverStatusSummaryTable, m_driverStatusSummary, static_cast<std::size_t>(m_driverCapabilityTable->rowCount()));
}

bool KernelDock::currentDriverCapabilitySourceIndex(std::size_t& sourceIndexOut) const
{
    sourceIndexOut = 0U;
    if (m_driverCapabilityTable == nullptr) { return false; }
    const int currentRow = m_driverCapabilityTable->currentRow();
    if (currentRow < 0) { return false; }

    QTableWidgetItem* featureItem = m_driverCapabilityTable->item(currentRow, static_cast<int>(DriverCapabilityColumn::Feature));
    if (featureItem == nullptr) { return false; }

    sourceIndexOut = static_cast<std::size_t>(featureItem->data(Qt::UserRole).toULongLong());
    return sourceIndexOut < m_driverCapabilityRows.size();
}

const KernelDriverCapabilityEntry* KernelDock::currentDriverCapabilityEntry() const
{
    std::size_t sourceIndex = 0U;
    if (!currentDriverCapabilitySourceIndex(sourceIndex)) { return nullptr; }
    return &m_driverCapabilityRows[sourceIndex];
}

void KernelDock::showDriverCapabilityDetailByCurrentRow()
{
    if (m_driverCapabilityDetailEditor == nullptr) { return; }

    const KernelDriverCapabilityEntry* entry = currentDriverCapabilityEntry();
    if (entry == nullptr)
    {
        m_driverCapabilityDetailEditor->setText(buildDriverStatusReport(m_driverStatusSummary, m_driverCapabilityRows));
        return;
    }

    m_driverCapabilityDetailEditor->setText(QStringLiteral(
        "%1\n\n当前状态摘要:\n"
        "  %2\n"
        "  当前内核: %3\n"
        "  识别版本: %4\n"
        "  本地 PDB profile: %5\n"
        "  可信偏移: %6\n"
        "  字段覆盖: %7\n"
        "  字段来源: %8\n"
        "  SecurityPolicy: %9 (%10)\n"
        "  DynDataStatus: %11 (%12)\n"
        "  DynDataCapability: %13 (%14)")
        .arg(entry->detailText)
        .arg(statusBadges(m_driverStatusSummary))
        .arg(kernelIdentityText(m_driverStatusSummary))
        .arg(kernelVersionText(m_driverStatusSummary))
        .arg(localPdbProfileText(m_driverStatusSummary))
        .arg(trustedOffsetText(m_driverStatusSummary))
        .arg(fieldCoverageText(m_driverStatusSummary))
        .arg(fieldSourceSummaryText(m_driverStatusSummary))
        .arg(formatHex32(m_driverStatusSummary.securityPolicyFlags)).arg(policyNames(m_driverStatusSummary.securityPolicyFlags))
        .arg(formatHex32(m_driverStatusSummary.dynDataStatusFlags)).arg(dynDataStatusText(m_driverStatusSummary.dynDataStatusFlags))
        .arg(formatHex64(m_driverStatusSummary.dynDataCapabilityMask)).arg(capabilityNames(m_driverStatusSummary.dynDataCapabilityMask)));
}
