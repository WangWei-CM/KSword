#include "KernelDock.h"

#include "../ArkDriverClient/ArkDriverClient.h"
#include "../FileDock/FilePropertyPeAnalyzer.h"
#include "../UI/CodeEditorWidget.h"
#include "../theme.h"

#include <QAbstractItemView>
#include <QAction>
#include <QApplication>
#include <QBrush>
#include <QClipboard>
#include <QColor>
#include <QDateTime>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDir>
#include <QFileInfo>
#include <QHeaderView>
#include <QHBoxLayout>
#include <QIcon>
#include <QItemSelectionModel>
#include <QLabel>
#include <QLineEdit>
#include <QList>
#include <QMenu>
#include <QMetaObject>
#include <QMessageBox>
#include <QPoint>
#include <QPointer>
#include <QProcess>
#include <QPushButton>
#include <QStringList>
#include <QSplitter>
#include <QTabWidget>
#include <QTableWidget>
#include <QTableWidgetItem>
#include <QVBoxLayout>

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>

#include <algorithm>
#include <cstdint>
#include <memory>
#include <thread>
#include <utility>
#include <vector>

namespace
{
    QString callbackEnumButtonStyle()
    {
        return KswordTheme::ThemedButtonStyle();
    }

    QString callbackEnumInputStyle()
    {
        return QStringLiteral(
            "QLineEdit{border:1px solid %2;border-radius:2px;background:%3;color:%4;padding:2px 6px;}"
            "QLineEdit:focus{border:1px solid %1;}")
            .arg(KswordTheme::PrimaryBlueHex)
            .arg(KswordTheme::BorderHex())
            .arg(KswordTheme::SurfaceHex())
            .arg(KswordTheme::TextPrimaryHex());
    }

    QString callbackEnumHeaderStyle()
    {
        return QStringLiteral(
            "QHeaderView::section{color:%1;background:%2;border:1px solid %3;font-weight:600;}")
            .arg(KswordTheme::PrimaryBlueHex)
            .arg(KswordTheme::SurfaceHex())
            .arg(KswordTheme::BorderHex());
    }

    QString callbackEnumSelectionStyle()
    {
        return QStringLiteral("QTableWidget::item:selected{background:%1;color:#FFFFFF;}")
            .arg(KswordTheme::PrimaryBlueHex);
    }

    QString callbackEnumStatusLabelStyle(const QString& colorHex)
    {
        return QStringLiteral("color:%1;font-weight:600;").arg(colorHex);
    }

    QString callbackEnumSafeText(const QString& valueText, const QString& fallbackText = QStringLiteral("<空>"))
    {
        return valueText.trimmed().isEmpty() ? fallbackText : valueText;
    }

    QString callbackEnumFormatAddress(const std::uint64_t value)
    {
        return QStringLiteral("0x%1")
            .arg(static_cast<qulonglong>(value), 16, 16, QChar('0'))
            .toUpper();
    }

    QString callbackEnumWindowsDirectoryPath()
    {
        // 作用：解析 Windows 目录，用于把 \SystemRoot\xxx 转成可打开的 Win32 路径。
        // 返回：Windows 目录绝对路径；失败时回退环境变量。
        wchar_t windowsPathBuffer[MAX_PATH] = {};
        const UINT copiedChars = ::GetWindowsDirectoryW(windowsPathBuffer, MAX_PATH);
        if (copiedChars > 0U && copiedChars < MAX_PATH)
        {
            return QDir::toNativeSeparators(QString::fromWCharArray(windowsPathBuffer));
        }

        const QString envPath = qEnvironmentVariable("SystemRoot");
        return envPath.isEmpty()
            ? QStringLiteral("C:\\Windows")
            : QDir::toNativeSeparators(envPath);
    }

    QString callbackEnumSystemDrivePrefix()
    {
        // 作用：从 Windows 目录中提取系统盘符，处理 \Windows\xxx 这类内核路径。
        // 返回：形如 C: 的盘符；无法判断时返回 C:。
        const QString windowsPath = callbackEnumWindowsDirectoryPath();
        if (windowsPath.size() >= 2 && windowsPath.at(1) == QLatin1Char(':'))
        {
            return windowsPath.left(2);
        }
        return QStringLiteral("C:");
    }

    QString callbackEnumMapNtDevicePathToDosPath(const QString& ntPathText)
    {
        // 作用：尝试把 \Device\HarddiskVolumeX\... 映射为 C:\...。
        // 返回：映射成功返回 Win32 路径；失败返回空字符串。
        const QString normalizedNtPath = QDir::toNativeSeparators(ntPathText.trimmed());
        if (!normalizedNtPath.startsWith(QStringLiteral("\\Device\\"), Qt::CaseInsensitive))
        {
            return QString();
        }

        for (wchar_t driveLetter = L'A'; driveLetter <= L'Z'; ++driveLetter)
        {
            const QString driveName = QStringLiteral("%1:").arg(QChar(driveLetter));
            wchar_t deviceNameBuffer[1024] = {};
            const DWORD copiedChars = ::QueryDosDeviceW(
                reinterpret_cast<LPCWSTR>(driveName.utf16()),
                deviceNameBuffer,
                static_cast<DWORD>(sizeof(deviceNameBuffer) / sizeof(deviceNameBuffer[0])));
            if (copiedChars == 0U)
            {
                continue;
            }

            const QString deviceName = QDir::toNativeSeparators(QString::fromWCharArray(deviceNameBuffer));
            if (deviceName.isEmpty() || !normalizedNtPath.startsWith(deviceName, Qt::CaseInsensitive))
            {
                continue;
            }

            const QString suffixText = normalizedNtPath.mid(deviceName.size());
            return QDir::toNativeSeparators(driveName + suffixText);
        }

        return QString();
    }

    QString callbackEnumNormalizeModulePath(const QString& modulePathText)
    {
        // 作用：把 R0 返回的模块路径规范化为 R3 可访问的 Win32 文件路径。
        // 返回：可访问 Win32 路径；无法转换时返回空字符串。
        QString pathText = modulePathText.trimmed();
        if (pathText.isEmpty() || pathText == QStringLiteral("<未解析>"))
        {
            return QString();
        }

        pathText = QDir::toNativeSeparators(pathText);
        if (pathText.startsWith(QStringLiteral("\\??\\"), Qt::CaseInsensitive))
        {
            pathText = pathText.mid(4);
        }
        if (pathText.startsWith(QStringLiteral("\\SystemRoot\\"), Qt::CaseInsensitive))
        {
            pathText = callbackEnumWindowsDirectoryPath() + pathText.mid(QStringLiteral("\\SystemRoot").size());
        }
        else if (pathText.startsWith(QStringLiteral("SystemRoot\\"), Qt::CaseInsensitive))
        {
            pathText = callbackEnumWindowsDirectoryPath() + QStringLiteral("\\") + pathText.mid(QStringLiteral("SystemRoot\\").size());
        }
        else if (pathText.startsWith(QStringLiteral("\\Windows\\"), Qt::CaseInsensitive))
        {
            pathText = callbackEnumSystemDrivePrefix() + pathText;
        }
        else if (pathText.startsWith(QStringLiteral("\\Device\\"), Qt::CaseInsensitive))
        {
            pathText = callbackEnumMapNtDevicePathToDosPath(pathText);
        }

        if (pathText.size() >= 2 && pathText.at(1) == QLatin1Char(':'))
        {
            const QFileInfo fileInfo(pathText);
            return fileInfo.exists() ? fileInfo.absoluteFilePath() : QDir::toNativeSeparators(pathText);
        }
        return QString();
    }

    QString callbackEnumBuildModuleFileGeneralText(const QString& filePath)
    {
        // 作用：生成模块文件详情窗口的常规信息页。
        // 返回：包含路径、大小和时间戳的纯文本。
        const QFileInfo fileInfo(filePath);
        QString detailText;
        detailText += QStringLiteral("文件路径：%1\n").arg(QDir::toNativeSeparators(fileInfo.absoluteFilePath()));
        detailText += QStringLiteral("文件名：%1\n").arg(fileInfo.fileName());
        detailText += QStringLiteral("所在目录：%1\n").arg(QDir::toNativeSeparators(fileInfo.absolutePath()));
        detailText += QStringLiteral("是否存在：%1\n").arg(fileInfo.exists() ? QStringLiteral("是") : QStringLiteral("否"));
        detailText += QStringLiteral("大小：%1 字节\n").arg(fileInfo.exists() ? QString::number(fileInfo.size()) : QStringLiteral("<不可用>"));
        detailText += QStringLiteral("创建时间：%1\n").arg(fileInfo.birthTime().isValid() ? fileInfo.birthTime().toString(QStringLiteral("yyyy-MM-dd HH:mm:ss.zzz")) : QStringLiteral("<不可用>"));
        detailText += QStringLiteral("修改时间：%1\n").arg(fileInfo.lastModified().isValid() ? fileInfo.lastModified().toString(QStringLiteral("yyyy-MM-dd HH:mm:ss.zzz")) : QStringLiteral("<不可用>"));
        detailText += QStringLiteral("访问时间：%1\n").arg(fileInfo.lastRead().isValid() ? fileInfo.lastRead().toString(QStringLiteral("yyyy-MM-dd HH:mm:ss.zzz")) : QStringLiteral("<不可用>"));
        detailText += QStringLiteral("可读：%1\n").arg(fileInfo.isReadable() ? QStringLiteral("是") : QStringLiteral("否"));
        detailText += QStringLiteral("可写：%1\n").arg(fileInfo.isWritable() ? QStringLiteral("是") : QStringLiteral("否"));
        detailText += QStringLiteral("可执行：%1\n").arg(fileInfo.isExecutable() ? QStringLiteral("是") : QStringLiteral("否"));
        return detailText;
    }

    void callbackEnumShowModuleFileDetailDialog(QWidget* parentWidget, const QString& filePath)
    {
        // 作用：弹出模块文件详细信息窗口，复用 FileDock 的 PE 解析报告。
        // 返回：无；窗口为模态，关闭后自动释放局部对象。
        QDialog detailDialog(parentWidget);
        detailDialog.setObjectName(QStringLiteral("CallbackEnumModuleFileDetailDialog"));
        detailDialog.setWindowTitle(QStringLiteral("模块文件详细信息 - %1").arg(QFileInfo(filePath).fileName()));
        detailDialog.resize(980, 680);
        detailDialog.setStyleSheet(KswordTheme::OpaqueDialogStyle(detailDialog.objectName()));

        QVBoxLayout* rootLayout = new QVBoxLayout(&detailDialog);
        QTabWidget* tabWidget = new QTabWidget(&detailDialog);
        rootLayout->addWidget(tabWidget, 1);

        CodeEditorWidget* generalEditor = new CodeEditorWidget(&detailDialog);
        generalEditor->setReadOnly(true);
        generalEditor->setText(callbackEnumBuildModuleFileGeneralText(filePath));
        tabWidget->addTab(generalEditor, QStringLiteral("常规信息"));

        CodeEditorWidget* peEditor = new CodeEditorWidget(&detailDialog);
        peEditor->setReadOnly(true);
        peEditor->setText(file_dock_detail::buildPeAnalysisText(filePath));
        tabWidget->addTab(peEditor, QStringLiteral("PE信息"));

        QDialogButtonBox* buttonBox = new QDialogButtonBox(QDialogButtonBox::Close, &detailDialog);
        QObject::connect(buttonBox, &QDialogButtonBox::rejected, &detailDialog, &QDialog::reject);
        QObject::connect(buttonBox, &QDialogButtonBox::accepted, &detailDialog, &QDialog::accept);
        rootLayout->addWidget(buttonBox, 0);
        detailDialog.exec();
    }

    bool callbackEnumOpenModuleInExplorer(const QString& filePath)
    {
        // 作用：用 Explorer 定位模块文件，失败时返回 false 让调用方更新状态栏。
        // 返回：成功启动 Explorer 返回 true。
        if (filePath.trimmed().isEmpty())
        {
            return false;
        }
        const QString nativePath = QDir::toNativeSeparators(filePath);
        const QString selectArgument = QStringLiteral("/select,\"%1\"").arg(nativePath);
        return QProcess::startDetached(QStringLiteral("explorer.exe"), QStringList{ selectArgument });
    }

    QString callbackEnumClassText(const std::uint32_t callbackClass)
    {
        switch (callbackClass)
        {
        case KSWORD_ARK_CALLBACK_ENUM_CLASS_REGISTRY:
            return QStringLiteral("注册表 CmCallback");
        case KSWORD_ARK_CALLBACK_ENUM_CLASS_PROCESS:
            return QStringLiteral("进程 Notify");
        case KSWORD_ARK_CALLBACK_ENUM_CLASS_THREAD:
            return QStringLiteral("线程 Notify");
        case KSWORD_ARK_CALLBACK_ENUM_CLASS_IMAGE:
            return QStringLiteral("镜像加载 Notify");
        case KSWORD_ARK_CALLBACK_ENUM_CLASS_OBJECT:
            return QStringLiteral("Object Callback");
        case KSWORD_ARK_CALLBACK_ENUM_CLASS_MINIFILTER:
            return QStringLiteral("Minifilter");
        case KSWORD_ARK_CALLBACK_ENUM_CLASS_WFP_CALLOUT:
            return QStringLiteral("WFP Callout");
        case KSWORD_ARK_CALLBACK_ENUM_CLASS_ETW_PROVIDER:
            return QStringLiteral("ETW Provider/Consumer");
        default:
            return QStringLiteral("未知(%1)").arg(callbackClass);
        }
    }

    QString callbackEnumSourceText(const std::uint32_t source)
    {
        switch (source)
        {
        case KSWORD_ARK_CALLBACK_ENUM_SOURCE_KSWORD_SELF:
            return QStringLiteral("Ksword 自身注册");
        case KSWORD_ARK_CALLBACK_ENUM_SOURCE_FLTMGR_ENUMERATION:
            return QStringLiteral("FltMgr 公开枚举");
        case KSWORD_ARK_CALLBACK_ENUM_SOURCE_PRIVATE_UNSUPPORTED:
            return QStringLiteral("私有结构诊断");
        case KSWORD_ARK_CALLBACK_ENUM_SOURCE_PRIVATE_PATTERN_SCAN:
            return QStringLiteral("私有特征定位");
        case KSWORD_ARK_CALLBACK_ENUM_SOURCE_PRIVATE_NOTIFY_ARRAY:
            return QStringLiteral("Psp Notify 数组");
        case KSWORD_ARK_CALLBACK_ENUM_SOURCE_PRIVATE_REGISTRY_LIST:
            return QStringLiteral("Cm 回调链表");
        case KSWORD_ARK_CALLBACK_ENUM_SOURCE_PRIVATE_OBJECT_TYPE_LIST:
            return QStringLiteral("Ob 对象类型链表");
        case KSWORD_ARK_CALLBACK_ENUM_SOURCE_WFP_MGMT_API:
            return QStringLiteral("WFP 管理 API");
        case KSWORD_ARK_CALLBACK_ENUM_SOURCE_ETW_DYNDATA:
            return QStringLiteral("ETW DynData");
        default:
            return QStringLiteral("未知(%1)").arg(source);
        }
    }

    enum class CallbackEnumRemovePolicyKind : int
    {
        NotRemovable = 0,
        RemovableVerified,
        RemovableCandidate,
        ExperimentalOnly
    };

    bool callbackEnumHasField(const KernelCallbackEnumEntry& entry, const std::uint32_t fieldFlag)
    {
        // Input: one cached callback row and one KSWORD_ARK_CALLBACK_ENUM_FIELD_* bit.
        // Processing: masks the legacy fieldFlags value without looking at future protocol bytes.
        // Return: true only when the existing protocol explicitly marks the field as present.
        return (entry.fieldFlags & fieldFlag) != 0U;
    }

    bool callbackEnumFieldIndicatesTrusted(const KernelCallbackEnumEntry& entry)
    {
        // Input: one cached callback row.
        // Processing: checks the optional trusted field bit only when the shared header has it.
        // Return: true when R0 explicitly marked this row as trusted; false on old headers.
#if defined(KSWORD_ARK_CALLBACK_ENUM_FIELD_TRUSTED)
        return callbackEnumHasField(entry, KSWORD_ARK_CALLBACK_ENUM_FIELD_TRUSTED);
#else
        return false;
#endif
    }

    bool callbackEnumFieldIndicatesVerifiedRemove(const KernelCallbackEnumEntry& entry)
    {
        // Input: one cached callback row.
        // Processing: checks the optional verified-remove field bit when available.
        // Return: true when R0 says the safe remove path is verified; false on old headers.
#if defined(KSWORD_ARK_CALLBACK_ENUM_FIELD_VERIFIED_REMOVE)
        return callbackEnumHasField(entry, KSWORD_ARK_CALLBACK_ENUM_FIELD_VERIFIED_REMOVE);
#else
        return false;
#endif
    }

    bool callbackEnumFieldIndicatesExperimentalRemove(const KernelCallbackEnumEntry& entry)
    {
        // Input: one cached callback row.
        // Processing: checks the optional experimental-remove field bit when available.
        // Return: true when R0 says this row only has an experimental unlink path; false otherwise.
#if defined(KSWORD_ARK_CALLBACK_ENUM_FIELD_EXPERIMENTAL_REMOVE)
        return callbackEnumHasField(entry, KSWORD_ARK_CALLBACK_ENUM_FIELD_EXPERIMENTAL_REMOVE);
#else
        return false;
#endif
    }

    bool callbackEnumTrustFlagsIndicateTrusted(const KernelCallbackEnumEntry& entry)
    {
        // Input: one cached callback row.
        // Processing: reads optional trust flags without making old shared headers incompatible.
        // Return: true for PDB/revalidated trust; false when the flags are absent or unrelated.
#if defined(KSWORD_ARK_CALLBACK_TRUST_PDB_PROFILE) && defined(KSWORD_ARK_CALLBACK_TRUST_REVALIDATED)
        return (entry.trustFlags & (KSWORD_ARK_CALLBACK_TRUST_PDB_PROFILE | KSWORD_ARK_CALLBACK_TRUST_REVALIDATED)) != 0U;
#else
        return entry.trustFlags != 0U;
#endif
    }

    bool callbackEnumTrustFlagsIndicatePublicApi(const KernelCallbackEnumEntry& entry)
    {
        // Input: one cached callback row.
        // Processing: checks optional trust flags for public API provenance.
        // Return: true when R0 explicitly reports public API trust; false on old headers.
#if defined(KSWORD_ARK_CALLBACK_TRUST_PUBLIC_API)
        return (entry.trustFlags & KSWORD_ARK_CALLBACK_TRUST_PUBLIC_API) != 0U;
#else
        return false;
#endif
    }

    bool callbackEnumTrustFlagsIndicateFallbackPattern(const KernelCallbackEnumEntry& entry)
    {
        // Input: one cached callback row.
        // Processing: checks optional trust flags for fallback/pattern provenance.
        // Return: true when R0 explicitly reports fallback evidence; false on old headers.
#if defined(KSWORD_ARK_CALLBACK_TRUST_FALLBACK_PATTERN)
        return (entry.trustFlags & KSWORD_ARK_CALLBACK_TRUST_FALLBACK_PATTERN) != 0U;
#else
        return false;
#endif
    }

    bool callbackEnumRemoveBehaviorIndicatesPublicApi(const KernelCallbackEnumEntry& entry)
    {
        // Input: one cached callback row.
        // Processing: reads optional remove-behavior flags for the safe public API path.
        // Return: true when the future protocol marks public API removal; false on old headers.
#if defined(KSWORD_ARK_CALLBACK_REMOVE_BEHAVIOR_PUBLIC_API)
        return (entry.removeFlags & KSWORD_ARK_CALLBACK_REMOVE_BEHAVIOR_PUBLIC_API) != 0U;
#else
        return false;
#endif
    }

    bool callbackEnumRemoveBehaviorIndicatesExperimentalUnlink(const KernelCallbackEnumEntry& entry)
    {
        // Input: one cached callback row.
        // Processing: reads optional remove-behavior flags for experimental unlink.
        // Return: true when the future protocol marks unlink-only behavior; false on old headers.
#if defined(KSWORD_ARK_CALLBACK_REMOVE_BEHAVIOR_EXPERIMENTAL_UNLINK)
        return (entry.removeFlags & KSWORD_ARK_CALLBACK_REMOVE_BEHAVIOR_EXPERIMENTAL_UNLINK) != 0U;
#else
        return false;
#endif
    }

    bool callbackEnumIsPublicApiSource(const std::uint32_t source)
    {
        // Input: the shared callback enumeration source id.
        // Processing: maps sources that came from documented management/enumeration APIs.
        // Return: true for public API backed sources; false for private/fallback diagnostics.
        return source == KSWORD_ARK_CALLBACK_ENUM_SOURCE_FLTMGR_ENUMERATION
            || source == KSWORD_ARK_CALLBACK_ENUM_SOURCE_WFP_MGMT_API;
    }

    bool callbackEnumIsFallbackPatternSource(const std::uint32_t source)
    {
        // Input: the shared callback enumeration source id.
        // Processing: groups private arrays/lists/pattern probes as fallback-style evidence.
        // Return: true when the source should be presented as fallback/pattern only.
        switch (source)
        {
        case KSWORD_ARK_CALLBACK_ENUM_SOURCE_PRIVATE_PATTERN_SCAN:
        case KSWORD_ARK_CALLBACK_ENUM_SOURCE_PRIVATE_NOTIFY_ARRAY:
        case KSWORD_ARK_CALLBACK_ENUM_SOURCE_PRIVATE_REGISTRY_LIST:
        case KSWORD_ARK_CALLBACK_ENUM_SOURCE_PRIVATE_OBJECT_TYPE_LIST:
        case KSWORD_ARK_CALLBACK_ENUM_SOURCE_ETW_DYNDATA:
            return true;
        default:
            return false;
        }
    }

    bool callbackEnumIsUnsupportedSource(const KernelCallbackEnumEntry& entry)
    {
        // Input: one cached callback row.
        // Processing: combines the explicit unsupported source with unsupported row status.
        // Return: true when UI should show unsupported instead of trusted/public/fallback.
        return entry.source == KSWORD_ARK_CALLBACK_ENUM_SOURCE_PRIVATE_UNSUPPORTED
            || entry.status == KSWORD_ARK_CALLBACK_ENUM_STATUS_UNSUPPORTED;
    }

    bool callbackEnumIsTrustedSource(const KernelCallbackEnumEntry& entry)
    {
        // Input: one cached callback row with legacy and reserved trust metadata.
        // Processing: treats Ksword-owned rows as trusted today and leaves PDB trust flags
        //             reserved for future protocol parsing.
        // Return: true for rows that are trusted without requiring private fallback evidence.
        return entry.source == KSWORD_ARK_CALLBACK_ENUM_SOURCE_KSWORD_SELF
            || callbackEnumHasField(entry, KSWORD_ARK_CALLBACK_ENUM_FIELD_OWNED_BY_KSWORD)
            || callbackEnumFieldIndicatesTrusted(entry)
            || callbackEnumTrustFlagsIndicateTrusted(entry);
    }

    QString callbackEnumSourceTrustText(const KernelCallbackEnumEntry& entry)
    {
        // Input: one cached callback row.
        // Processing: collapses current source ids plus reserved trust flags into the four
        //             UX buckets requested for PDB trusted callback readiness.
        // Return: display text that includes trusted/fallback/public api/unsupported keywords.
        if (callbackEnumIsUnsupportedSource(entry))
        {
            return QStringLiteral("unsupported（当前协议/平台未支持）");
        }
        if (callbackEnumIsTrustedSource(entry))
        {
            return QStringLiteral("trusted（可信/自有或预留 PDB）");
        }
        if (callbackEnumIsPublicApiSource(entry.source)
            || callbackEnumTrustFlagsIndicatePublicApi(entry)
            || callbackEnumRemoveBehaviorIndicatesPublicApi(entry))
        {
            return QStringLiteral("public api（公开 API）");
        }
        if (callbackEnumIsFallbackPatternSource(entry.source)
            || callbackEnumTrustFlagsIndicateFallbackPattern(entry))
        {
            return QStringLiteral("fallback/pattern（私有结构诊断）");
        }
        return QStringLiteral("fallback（未知来源保守展示）");
    }

    std::uint32_t callbackEnumRemoveTypeForClass(const std::uint32_t callbackClass)
    {
        // Input: callback enum class from the shared enum protocol.
        // Processing: maps enum classes to the old REMOVE_EXTERNAL_CALLBACK request classes.
        // Return: external remove class id; 0 means no compatible old remove request exists.
        switch (callbackClass)
        {
        case KSWORD_ARK_CALLBACK_ENUM_CLASS_REGISTRY:
            return KSWORD_ARK_EXTERNAL_CALLBACK_REMOVE_TYPE_REGISTRY;
        case KSWORD_ARK_CALLBACK_ENUM_CLASS_PROCESS:
            return KSWORD_ARK_EXTERNAL_CALLBACK_REMOVE_TYPE_PROCESS;
        case KSWORD_ARK_CALLBACK_ENUM_CLASS_THREAD:
            return KSWORD_ARK_EXTERNAL_CALLBACK_REMOVE_TYPE_THREAD;
        case KSWORD_ARK_CALLBACK_ENUM_CLASS_IMAGE:
            return KSWORD_ARK_EXTERNAL_CALLBACK_REMOVE_TYPE_IMAGE;
        case KSWORD_ARK_CALLBACK_ENUM_CLASS_OBJECT:
            return KSWORD_ARK_EXTERNAL_CALLBACK_REMOVE_TYPE_OBJECT;
        case KSWORD_ARK_CALLBACK_ENUM_CLASS_MINIFILTER:
            return KSWORD_ARK_EXTERNAL_CALLBACK_REMOVE_TYPE_MINIFILTER;
        case KSWORD_ARK_CALLBACK_ENUM_CLASS_WFP_CALLOUT:
            return KSWORD_ARK_EXTERNAL_CALLBACK_REMOVE_TYPE_WFP_CALLOUT;
        case KSWORD_ARK_CALLBACK_ENUM_CLASS_ETW_PROVIDER:
            return KSWORD_ARK_EXTERNAL_CALLBACK_REMOVE_TYPE_ETW_PROVIDER;
        default:
            return 0U;
        }
    }

    std::uint64_t callbackEnumRemoveRequestValue(const KernelCallbackEnumEntry& entry)
    {
        // Input: one cached callback row.
        // Processing: chooses the value accepted by the legacy remove protocol. The field is
        //             named callbackAddress, but WFP currently carries calloutId there.
        // Return: non-zero request value for removeExternalCallback, or 0 when unavailable.
        if (entry.callbackAddress != 0U
            && (callbackEnumHasField(entry, KSWORD_ARK_CALLBACK_ENUM_FIELD_CALLBACK_ADDRESS)
                || callbackEnumHasField(entry, KSWORD_ARK_CALLBACK_ENUM_FIELD_IDENTIFIER)
                || callbackEnumHasField(entry, KSWORD_ARK_CALLBACK_ENUM_FIELD_HANDLE)
                || (entry.fieldFlags & KSWORD_ARK_CALLBACK_ENUM_FIELD_REMOVABLE_CANDIDATE) != 0U))
        {
            return entry.callbackAddress;
        }
        if (entry.registrationAddress != 0U
            && callbackEnumHasField(entry, KSWORD_ARK_CALLBACK_ENUM_FIELD_IDENTIFIER))
        {
            return entry.registrationAddress;
        }
        return 0U;
    }

    bool callbackEnumHasExperimentalStorageValue(const KernelCallbackEnumEntry& entry)
    {
        // Input: one cached callback row.
        // Processing: checks legacy diagnostic addresses and reserved raw storage metadata.
        // Return: true when UI can describe an unlink-only candidate without sending IOCTLs.
        return entry.rawStorageValue != 0U
            || entry.callbackAddress != 0U
            || entry.registrationAddress != 0U
            || entry.contextAddress != 0U;
    }

    CallbackEnumRemovePolicyKind callbackEnumRemovePolicyKind(const KernelCallbackEnumEntry& entry)
    {
        // Input: one cached callback row.
        // Processing: derives a conservative UI policy from legacy removable-candidate bits,
        //             source trust class, and the presence of a legacy remove request value.
        // Return: the display policy; this does not create any new driver protocol.
        if (callbackEnumIsUnsupportedSource(entry)
            || entry.status != KSWORD_ARK_CALLBACK_ENUM_STATUS_OK
            || callbackEnumRemoveTypeForClass(entry.callbackClass) == 0U)
        {
            return CallbackEnumRemovePolicyKind::NotRemovable;
        }

        const bool removableCandidate =
            callbackEnumHasField(entry, KSWORD_ARK_CALLBACK_ENUM_FIELD_REMOVABLE_CANDIDATE);
        const bool hasLegacyRemoveValue = callbackEnumRemoveRequestValue(entry) != 0U;
        const bool verifiedRemove =
            callbackEnumFieldIndicatesVerifiedRemove(entry)
            || callbackEnumRemoveBehaviorIndicatesPublicApi(entry);
        const bool experimentalRemove =
            callbackEnumFieldIndicatesExperimentalRemove(entry)
            || callbackEnumRemoveBehaviorIndicatesExperimentalUnlink(entry);
        if (hasLegacyRemoveValue
            && (verifiedRemove || (removableCandidate && callbackEnumIsPublicApiSource(entry.source))))
        {
            return CallbackEnumRemovePolicyKind::RemovableVerified;
        }
        if (removableCandidate && hasLegacyRemoveValue)
        {
            return CallbackEnumRemovePolicyKind::RemovableCandidate;
        }
        if ((experimentalRemove
            || callbackEnumIsFallbackPatternSource(entry.source)
            || callbackEnumTrustFlagsIndicateFallbackPattern(entry))
            && callbackEnumHasExperimentalStorageValue(entry))
        {
            return CallbackEnumRemovePolicyKind::ExperimentalOnly;
        }
        return CallbackEnumRemovePolicyKind::NotRemovable;
    }

    QString callbackEnumRemovePolicyText(const KernelCallbackEnumEntry& entry)
    {
        // Input: one cached callback row.
        // Processing: converts the derived policy to stable UX wording.
        // Return: display text containing the requested removable policy keywords.
        switch (callbackEnumRemovePolicyKind(entry))
        {
        case CallbackEnumRemovePolicyKind::RemovableVerified:
            return QStringLiteral("removable verified（公开 API 可验证）");
        case CallbackEnumRemovePolicyKind::RemovableCandidate:
            return QStringLiteral("removable candidate（旧协议候选）");
        case CallbackEnumRemovePolicyKind::ExperimentalOnly:
            return QStringLiteral("experimental only（仅预留 unlink）");
        case CallbackEnumRemovePolicyKind::NotRemovable:
        default:
            return QStringLiteral("not removable（不可移除）");
        }
    }

    bool callbackEnumCanUseLegacySafeRemove(const KernelCallbackEnumEntry& entry)
    {
        // Input: one cached callback row.
        // Processing: allows only verified/candidate policies to call old removeExternalCallback.
        // Return: true when the context menu may invoke ArkDriverClient::removeExternalCallback.
        const CallbackEnumRemovePolicyKind policy = callbackEnumRemovePolicyKind(entry);
        return policy == CallbackEnumRemovePolicyKind::RemovableVerified
            || policy == CallbackEnumRemovePolicyKind::RemovableCandidate;
    }

    bool callbackEnumRequiresSecondConfirmation(const KernelCallbackEnumEntry& entry)
    {
        // Input: one cached callback row.
        // Processing: requires confirmation for every row that can change kernel callback
        //             state, and especially for fallback/pattern or unlink-only rows.
        // Return: true when the detail pane/menu should require a QMessageBox confirmation.
        return callbackEnumRemovePolicyKind(entry) != CallbackEnumRemovePolicyKind::NotRemovable;
    }

    QString callbackEnumYesNoText(const bool value)
    {
        // Input: boolean UI state.
        // Processing: maps it to localized yes/no text.
        // Return: "是" for true, "否" for false.
        return value ? QStringLiteral("是") : QStringLiteral("否");
    }

    QString callbackEnumIdentityHashText(const std::uint64_t identityHash)
    {
        // Input: reserved identity hash from ArkDriverClient.
        // Processing: keeps the current v1 protocol compatible by showing an empty value for 0.
        // Return: hex hash text or an explicit empty placeholder.
        if (identityHash == 0U)
        {
            return QStringLiteral("<空>");
        }
        return callbackEnumFormatAddress(identityHash);
    }

    QString callbackEnumNtStatusText(const long ntstatus)
    {
        // Input: NTSTATUS value returned by the driver response.
        // Processing: formats the signed status as the conventional 8-digit hex value.
        // Return: uppercase NTSTATUS text suitable for labels and detail panes.
        return QStringLiteral("0x%1")
            .arg(static_cast<qulonglong>(static_cast<std::uint32_t>(ntstatus)), 8, 16, QChar('0'))
            .toUpper();
    }

    QString callbackEnumRemoveMappingText(const std::uint32_t mappingFlags)
    {
        // Input: KSWORD_ARK_EXTERNAL_CALLBACK_MAPPING_FLAG_* bits from the old remove response.
        // Processing: expands known bits while keeping unknown future bits visible.
        // Return: human-readable mapping flag summary.
        QStringList flagList;
        if ((mappingFlags & KSWORD_ARK_EXTERNAL_CALLBACK_MAPPING_FLAG_MODULE) != 0U)
        {
            flagList.push_back(QStringLiteral("module"));
        }
        if ((mappingFlags & KSWORD_ARK_EXTERNAL_CALLBACK_MAPPING_FLAG_ENUMERATED) != 0U)
        {
            flagList.push_back(QStringLiteral("enumerated"));
        }
        if ((mappingFlags & KSWORD_ARK_EXTERNAL_CALLBACK_MAPPING_FLAG_PUBLIC_API) != 0U)
        {
            flagList.push_back(QStringLiteral("public api"));
        }
        const std::uint32_t knownFlags =
            KSWORD_ARK_EXTERNAL_CALLBACK_MAPPING_FLAG_MODULE |
            KSWORD_ARK_EXTERNAL_CALLBACK_MAPPING_FLAG_ENUMERATED |
            KSWORD_ARK_EXTERNAL_CALLBACK_MAPPING_FLAG_PUBLIC_API;
        const std::uint32_t unknownFlags = mappingFlags & ~knownFlags;
        if (unknownFlags != 0U)
        {
            flagList.push_back(QStringLiteral("unknown=0x%1")
                .arg(static_cast<qulonglong>(unknownFlags), 8, 16, QChar('0'))
                .toUpper());
        }
        return flagList.isEmpty() ? QStringLiteral("<无>") : flagList.join(QStringLiteral(", "));
    }

    KSWORD_ARK_REMOVE_EXTERNAL_CALLBACK_REQUEST callbackEnumBuildLegacyRemoveRequest(const KernelCallbackEnumEntry& entry)
    {
        // Input: one selected callback enumeration row.
        // Processing: maps enum metadata to the existing v1 removeExternalCallback request.
        // Return: initialized request packet; callbackClass/address are zero when not compatible.
        KSWORD_ARK_REMOVE_EXTERNAL_CALLBACK_REQUEST requestPacket{};
        requestPacket.size = sizeof(requestPacket);
        requestPacket.version = KSWORD_ARK_EXTERNAL_CALLBACK_REMOVE_PROTOCOL_VERSION;
        requestPacket.callbackClass = callbackEnumRemoveTypeForClass(entry.callbackClass);
        requestPacket.flags = KSWORD_ARK_EXTERNAL_CALLBACK_REMOVE_FLAG_NONE;
        requestPacket.callbackAddress = callbackEnumRemoveRequestValue(entry);
        return requestPacket;
    }

    QString callbackEnumLegacyRemoveDetailText(
        const KernelCallbackEnumEntry& entry,
        const KSWORD_ARK_REMOVE_EXTERNAL_CALLBACK_REQUEST& requestPacket,
        const ksword::ark::CallbackRemoveResult& removeResult)
    {
        // Input: selected row, request packet, and ArkDriverClient remove result.
        // Processing: renders both transport and R0 semantic fields without assuming success.
        // Return: full detail text for the callback enum detail pane.
        const KSWORD_ARK_REMOVE_EXTERNAL_CALLBACK_RESPONSE& responsePacket = removeResult.response;
        const QString modulePath = QString::fromWCharArray(responsePacket.modulePath);
        const QString serviceName = QString::fromWCharArray(responsePacket.serviceName);
        return QStringLiteral(
            "安全移除（公开 API）请求已执行。\n"
            "- 类型：%1\n"
            "- 来源：%2\n"
            "- 可信状态：%3\n"
            "- 移除策略：%4\n"
            "- 请求类：%5\n"
            "- 请求值：%6\n"
            "- Win32：%7\n"
            "- 返回字节：%8\n"
            "- NTSTATUS：%9\n"
            "- 映射标志：%10\n"
            "- 模块路径：%11\n"
            "- 模块基址：%12\n"
            "- 模块大小：0x%13\n"
            "- 服务名：%14\n"
            "- ArkDriverClient：%15")
            .arg(entry.classText)
            .arg(entry.sourceText)
            .arg(entry.sourceTrustText)
            .arg(entry.removePolicyText)
            .arg(static_cast<qulonglong>(requestPacket.callbackClass))
            .arg(callbackEnumFormatAddress(requestPacket.callbackAddress))
            .arg(static_cast<qulonglong>(removeResult.io.win32Error))
            .arg(static_cast<qulonglong>(removeResult.io.bytesReturned))
            .arg(callbackEnumNtStatusText(responsePacket.ntstatus))
            .arg(callbackEnumRemoveMappingText(responsePacket.mappingFlags))
            .arg(modulePath.isEmpty() ? QStringLiteral("<未解析>") : modulePath)
            .arg(callbackEnumFormatAddress(responsePacket.moduleBase))
            .arg(QString::number(static_cast<qulonglong>(responsePacket.moduleSize), 16).toUpper())
            .arg(serviceName.isEmpty() ? QStringLiteral("<未匹配>") : serviceName)
            .arg(QString::fromStdString(removeResult.io.message));
    }

    bool callbackEnumConfirmSafeRemove(QWidget* parentWidget, const KernelCallbackEnumEntry& entry)
    {
        // Input: parent widget and selected row.
        // Processing: shows a second confirmation before any legacy remove IOCTL is sent.
        // Return: true when the user explicitly confirms the safe public/API remove action.
        const QString warningText = QStringLiteral(
            "即将执行“安全移除（公开 API）”。\n\n"
            "类别：%1\n"
            "名称：%2\n"
            "来源：%3\n"
            "可信状态：%4\n"
            "移除策略：%5\n"
            "请求值：%6\n\n"
            "该操作会通过 ArkDriverClient 调用旧版 removeExternalCallback；不会使用实验性 unlink。是否继续？")
            .arg(entry.classText)
            .arg(callbackEnumSafeText(entry.nameText))
            .arg(entry.sourceText)
            .arg(entry.sourceTrustText)
            .arg(entry.removePolicyText)
            .arg(callbackEnumFormatAddress(callbackEnumRemoveRequestValue(entry)));
        return QMessageBox::question(
            parentWidget,
            QStringLiteral("安全移除（公开 API）"),
            warningText,
            QMessageBox::Yes | QMessageBox::No,
            QMessageBox::No) == QMessageBox::Yes;
    }

    void callbackEnumExecuteLegacySafeRemove(
        QWidget* parentWidget,
        QLabel* statusLabel,
        CodeEditorWidget* detailEditor,
        const KernelCallbackEnumEntry& entry)
    {
        // Input: UI sinks plus the selected callback row.
        // Processing: validates the v1 packet, asks for confirmation, then calls ArkDriverClient.
        // Return: no return value; status/detail widgets and QMessageBox carry the outcome.
        if (!callbackEnumCanUseLegacySafeRemove(entry))
        {
            QMessageBox::information(
                parentWidget,
                QStringLiteral("安全移除（公开 API）"),
                QStringLiteral("当前记录不是 removable verified/candidate，旧协议安全移除不可用。"));
            return;
        }

        const KSWORD_ARK_REMOVE_EXTERNAL_CALLBACK_REQUEST requestPacket =
            callbackEnumBuildLegacyRemoveRequest(entry);
        if (requestPacket.callbackClass == 0U || requestPacket.callbackAddress == 0U)
        {
            QMessageBox::warning(
                parentWidget,
                QStringLiteral("安全移除（公开 API）"),
                QStringLiteral("当前记录缺少旧 removeExternalCallback 所需的类型或地址/标识值。"));
            return;
        }

        if (!callbackEnumConfirmSafeRemove(parentWidget, entry))
        {
            if (statusLabel != nullptr)
            {
                statusLabel->setText(QStringLiteral("状态：已取消安全移除"));
            }
            return;
        }

        const ksword::ark::DriverClient driverClient;
        const ksword::ark::CallbackRemoveResult removeResult =
            driverClient.removeExternalCallback(requestPacket);
        if (detailEditor != nullptr)
        {
            detailEditor->setText(callbackEnumLegacyRemoveDetailText(entry, requestPacket, removeResult));
        }

        if (!removeResult.io.ok)
        {
            if (statusLabel != nullptr)
            {
                statusLabel->setText(QStringLiteral("状态：安全移除失败，Win32=%1")
                    .arg(static_cast<qulonglong>(removeResult.io.win32Error)));
            }
            QMessageBox::warning(
                parentWidget,
                QStringLiteral("安全移除（公开 API）"),
                QStringLiteral("ArkDriverClient 调用失败，Win32=%1。")
                    .arg(static_cast<qulonglong>(removeResult.io.win32Error)));
            return;
        }

        if (removeResult.response.ntstatus >= 0)
        {
            if (statusLabel != nullptr)
            {
                statusLabel->setText(QStringLiteral("状态：安全移除完成"));
            }
        }
        else
        {
            if (statusLabel != nullptr)
            {
                statusLabel->setText(QStringLiteral("状态：驱动返回失败，NTSTATUS=%1")
                    .arg(callbackEnumNtStatusText(removeResult.response.ntstatus)));
            }
            QMessageBox::warning(
                parentWidget,
                QStringLiteral("安全移除（公开 API）"),
                QStringLiteral("驱动返回失败，NTSTATUS=%1。")
                    .arg(callbackEnumNtStatusText(removeResult.response.ntstatus)));
        }
    }

    void callbackEnumShowExperimentalUnlinkNotice(
        QWidget* parentWidget,
        QLabel* statusLabel,
        CodeEditorWidget* detailEditor,
        const KernelCallbackEnumEntry& entry)
    {
        // Input: UI sinks plus the selected callback row.
        // Processing: presents a strong confirmation and then stops because this UI skeleton
        //             has no ArkDriverClient unlink execution method.
        // Return: no return value; no driver IOCTL is sent from this UI skeleton.
        const QString confirmText = QStringLiteral(
            "实验性强制移除（unlink）不是默认路径，可能破坏内核链表/数组一致性，导致系统不稳定、蓝屏或安全产品状态失真。\n\n"
            "类别：%1\n"
            "名称：%2\n"
            "来源：%3\n"
            "可信状态：%4\n"
            "移除策略：%5\n"
            "RawStorageValue：%6\n\n"
            "当前 UI 只做交互骨架。若继续确认，也只会检查协议启用状态，不会绕过 ArkDriverClient 直接发送 DeviceIoControl。")
            .arg(entry.classText)
            .arg(callbackEnumSafeText(entry.nameText))
            .arg(entry.sourceText)
            .arg(entry.sourceTrustText)
            .arg(entry.removePolicyText)
            .arg(callbackEnumFormatAddress(entry.rawStorageValue));
        const QMessageBox::StandardButton reply = QMessageBox::warning(
            parentWidget,
            QStringLiteral("实验性强制移除（unlink）"),
            confirmText,
            QMessageBox::Yes | QMessageBox::Cancel,
            QMessageBox::Cancel);
        if (reply != QMessageBox::Yes)
        {
            if (statusLabel != nullptr)
            {
                statusLabel->setText(QStringLiteral("状态：已取消实验性 unlink"));
            }
            return;
        }

        const ksword::ark::DriverClient driverClient;
        const bool protocolEnabled = driverClient.supportsExternalCallbackExperimentalUnlink();
        const QString protocolText = protocolEnabled
            ? QStringLiteral("检测到扩展协议宏，但 ArkDriverClient 尚未暴露 unlink 执行方法；UI 不直接 DeviceIoControl。")
            : QStringLiteral("当前驱动协议未启用 REMOVE_EXTERNAL_CALLBACK_EX；实验性 unlink 仅显示骨架，不发送 IOCTL。");
        if (detailEditor != nullptr)
        {
            detailEditor->setText(QStringLiteral(
                "实验性强制移除（unlink）未执行。\n"
                "- 类型：%1\n"
                "- 来源：%2\n"
                "- 可信状态：%3\n"
                "- 移除策略：%4\n"
                "- Generation：%5\n"
                "- IdentityHash：%6\n"
                "- RawStorageValue：%7\n"
                "- 协议状态：%8")
                .arg(entry.classText)
                .arg(entry.sourceText)
                .arg(entry.sourceTrustText)
                .arg(entry.removePolicyText)
                .arg(static_cast<qulonglong>(entry.generation))
                .arg(callbackEnumIdentityHashText(entry.identityHash))
                .arg(callbackEnumFormatAddress(entry.rawStorageValue))
                .arg(protocolText));
        }
        if (statusLabel != nullptr)
        {
            statusLabel->setText(protocolEnabled
                ? QStringLiteral("状态：实验性 unlink 未执行，ArkDriverClient 未暴露执行方法")
                : QStringLiteral("状态：实验性 unlink 未执行，当前协议未启用"));
        }
        QMessageBox::information(
            parentWidget,
            QStringLiteral("实验性强制移除（unlink）"),
            protocolText);
    }

    QString callbackEnumPrimaryAddressText(const KernelCallbackEnumEntry& entry)
    {
        // 作用：根据 fieldFlags 选择表格主地址，避免把定位/诊断行误显示为 0 地址。
        // 返回：真实回调地址、全局/节点地址、诊断地址或“无回调地址”占位文本。
        if ((entry.fieldFlags & KSWORD_ARK_CALLBACK_ENUM_FIELD_CALLBACK_ADDRESS) != 0U
            && entry.callbackAddress != 0U)
        {
            return callbackEnumFormatAddress(entry.callbackAddress);
        }
        if ((entry.fieldFlags & KSWORD_ARK_CALLBACK_ENUM_FIELD_IDENTIFIER) != 0U
            && entry.callbackAddress != 0U)
        {
            return QStringLiteral("标识 %1").arg(callbackEnumFormatAddress(entry.callbackAddress));
        }
        if ((entry.fieldFlags & KSWORD_ARK_CALLBACK_ENUM_FIELD_HANDLE) != 0U
            && entry.callbackAddress != 0U)
        {
            return QStringLiteral("句柄 %1").arg(callbackEnumFormatAddress(entry.callbackAddress));
        }
        if ((entry.fieldFlags & KSWORD_ARK_CALLBACK_ENUM_FIELD_REGISTRATION_ADDRESS) != 0U
            && entry.registrationAddress != 0U)
        {
            const bool locateRow = entry.source == KSWORD_ARK_CALLBACK_ENUM_SOURCE_PRIVATE_PATTERN_SCAN;
            return QStringLiteral("%1 %2")
                .arg(locateRow ? QStringLiteral("全局") : QStringLiteral("节点"))
                .arg(callbackEnumFormatAddress(entry.registrationAddress));
        }
        if ((entry.fieldFlags & KSWORD_ARK_CALLBACK_ENUM_FIELD_CONTEXT_ADDRESS) != 0U
            && entry.contextAddress != 0U)
        {
            return QStringLiteral("诊断 %1").arg(callbackEnumFormatAddress(entry.contextAddress));
        }
        return QStringLiteral("<无回调地址>");
    }

    QString callbackEnumRowStatusText(const std::uint32_t status, const long lastStatus)
    {
        switch (status)
        {
        case KSWORD_ARK_CALLBACK_ENUM_STATUS_OK:
            return QStringLiteral("可见/成功");
        case KSWORD_ARK_CALLBACK_ENUM_STATUS_NOT_REGISTERED:
            return QStringLiteral("未注册");
        case KSWORD_ARK_CALLBACK_ENUM_STATUS_UNSUPPORTED:
            return QStringLiteral("当前不支持");
        case KSWORD_ARK_CALLBACK_ENUM_STATUS_QUERY_FAILED:
            return QStringLiteral("查询失败(0x%1)")
                .arg(QString::number(static_cast<quint32>(lastStatus), 16).rightJustified(8, QLatin1Char('0')).toUpper());
        case KSWORD_ARK_CALLBACK_ENUM_STATUS_BUFFER_TRUNCATED:
            return QStringLiteral("缓冲截断");
        default:
            return QStringLiteral("未知(%1)").arg(status);
        }
    }

    KernelCallbackEnumEntry callbackEnumConvertEntry(const ksword::ark::CallbackEnumEntry& source)
    {
        KernelCallbackEnumEntry row;
        row.callbackClass = source.callbackClass;
        row.source = source.source;
        row.status = source.status;
        row.fieldFlags = source.fieldFlags;
        row.trustFlags = source.trustFlags;
        row.removeFlags = source.removeFlags;
        row.operationMask = source.operationMask;
        row.objectTypeMask = source.objectTypeMask;
        row.generation = source.generation;
        row.lastStatus = source.lastStatus;
        row.callbackAddress = source.callbackAddress;
        row.contextAddress = source.contextAddress;
        row.registrationAddress = source.registrationAddress;
        row.identityHash = source.identityHash;
        row.rawStorageValue = source.rawStorageValue;
        row.moduleBase = source.moduleBase;
        row.moduleSize = source.moduleSize;
        row.classText = callbackEnumClassText(source.callbackClass);
        row.sourceText = callbackEnumSourceText(source.source);
        row.sourceTrustText = callbackEnumSourceTrustText(row);
        row.removePolicyText = callbackEnumRemovePolicyText(row);
        row.statusText = callbackEnumRowStatusText(source.status, source.lastStatus);
        row.nameText = QString::fromStdWString(source.name);
        row.altitudeText = QString::fromStdWString(source.altitude);
        row.modulePathText = QString::fromStdWString(source.modulePath);
        row.detailText = QString::fromStdWString(source.detail);
        row.requiresSecondConfirmation = callbackEnumRequiresSecondConfirmation(row);
        row.fallbackPatternOnly = callbackEnumIsFallbackPatternSource(row.source);
        return row;
    }

    enum class CallbackEnumColumn : int
    {
        Class = 0,
        Source,
        Trust,
        Status,
        RemovePolicy,
        Name,
        CallbackAddress,
        Module,
        Altitude,
        Count
    };

    QString callbackEnumColumnHeaderText(const CallbackEnumColumn column)
    {
        // 作用：把回调遍历表格列枚举映射为右键菜单和剪贴板表头文本。
        // 返回：该列对应的中文表头；未知列返回“未知列”。
        switch (column)
        {
        case CallbackEnumColumn::Class:
            return QStringLiteral("类别");
        case CallbackEnumColumn::Source:
            return QStringLiteral("来源");
        case CallbackEnumColumn::Trust:
            return QStringLiteral("可信状态");
        case CallbackEnumColumn::Status:
            return QStringLiteral("状态");
        case CallbackEnumColumn::RemovePolicy:
            return QStringLiteral("移除策略");
        case CallbackEnumColumn::Name:
            return QStringLiteral("名称");
        case CallbackEnumColumn::CallbackAddress:
            return QStringLiteral("回调/对象地址");
        case CallbackEnumColumn::Module:
            return QStringLiteral("模块");
        case CallbackEnumColumn::Altitude:
            return QStringLiteral("Altitude");
        default:
            return QStringLiteral("未知列");
        }
    }

    QString callbackEnumEntryColumnText(
        const KernelCallbackEnumEntry& entry,
        const CallbackEnumColumn column)
    {
        // 作用：从缓存行中提取指定表格列文本，保证复制菜单不依赖当前单元格对象。
        // 返回：可直接写入剪贴板的单列文本。
        switch (column)
        {
        case CallbackEnumColumn::Class:
            return entry.classText;
        case CallbackEnumColumn::Source:
            return entry.sourceText;
        case CallbackEnumColumn::Trust:
            return entry.sourceTrustText;
        case CallbackEnumColumn::Status:
            return entry.statusText;
        case CallbackEnumColumn::RemovePolicy:
            return entry.removePolicyText;
        case CallbackEnumColumn::Name:
            return callbackEnumSafeText(entry.nameText);
        case CallbackEnumColumn::CallbackAddress:
            return callbackEnumPrimaryAddressText(entry);
        case CallbackEnumColumn::Module:
            return entry.modulePathText.isEmpty() ? QStringLiteral("<未解析>") : entry.modulePathText;
        case CallbackEnumColumn::Altitude:
            return callbackEnumSafeText(entry.altitudeText);
        default:
            return QString();
        }
    }

    QString callbackEnumEntryAsTsv(const KernelCallbackEnumEntry& entry)
    {
        // 作用：把一条回调遍历记录按表格列顺序序列化为 TSV。
        // 返回：单行 TSV，不包含换行符。
        QStringList fieldList;
        fieldList.reserve(static_cast<int>(CallbackEnumColumn::Count));
        for (int columnIndex = 0; columnIndex < static_cast<int>(CallbackEnumColumn::Count); ++columnIndex)
        {
            fieldList.push_back(callbackEnumEntryColumnText(
                entry,
                static_cast<CallbackEnumColumn>(columnIndex)));
        }
        return fieldList.join('\t');
    }

    QString callbackEnumHeaderAsTsv()
    {
        // 作用：生成回调遍历表头 TSV，配合“复制表头+选中行”使用。
        // 返回：表头单行 TSV。
        QStringList headerList;
        headerList.reserve(static_cast<int>(CallbackEnumColumn::Count));
        for (int columnIndex = 0; columnIndex < static_cast<int>(CallbackEnumColumn::Count); ++columnIndex)
        {
            headerList.push_back(callbackEnumColumnHeaderText(static_cast<CallbackEnumColumn>(columnIndex)));
        }
        return headerList.join('\t');
    }

    std::vector<int> callbackEnumSelectedVisualRows(
        const QTableWidget* tableWidget,
        const int fallbackRow)
    {
        // 作用：收集当前可视表格中所有选中行，按可视行号排序去重。
        // 返回：可视行号数组；没有显式选择时使用 fallbackRow 兜底。
        std::vector<int> selectedRows;
        if (tableWidget == nullptr)
        {
            return selectedRows;
        }

        const QList<QTableWidgetItem*> selectedItems = tableWidget->selectedItems();
        selectedRows.reserve(static_cast<std::size_t>(selectedItems.size()));
        for (QTableWidgetItem* item : selectedItems)
        {
            if (item != nullptr)
            {
                selectedRows.push_back(item->row());
            }
        }

        if (selectedRows.empty() && fallbackRow >= 0)
        {
            selectedRows.push_back(fallbackRow);
        }

        std::sort(selectedRows.begin(), selectedRows.end());
        selectedRows.erase(std::unique(selectedRows.begin(), selectedRows.end()), selectedRows.end());
        return selectedRows;
    }

    std::vector<std::size_t> callbackEnumSelectedSourceIndices(
        const QTableWidget* tableWidget,
        const std::vector<KernelCallbackEnumEntry>& sourceRows,
        const int fallbackRow)
    {
        // 作用：把表格可视选中行转换成 m_callbackEnumRows 的源索引。
        // 返回：有效源索引数组，顺序与当前排序/筛选后的可视顺序一致。
        std::vector<std::size_t> sourceIndices;
        if (tableWidget == nullptr)
        {
            return sourceIndices;
        }

        const std::vector<int> selectedRows = callbackEnumSelectedVisualRows(tableWidget, fallbackRow);
        sourceIndices.reserve(selectedRows.size());
        for (const int visualRow : selectedRows)
        {
            QTableWidgetItem* classItem = tableWidget->item(
                visualRow,
                static_cast<int>(CallbackEnumColumn::Class));
            if (classItem == nullptr)
            {
                continue;
            }

            const std::size_t sourceIndex =
                static_cast<std::size_t>(classItem->data(Qt::UserRole).toULongLong());
            if (sourceIndex < sourceRows.size())
            {
                sourceIndices.push_back(sourceIndex);
            }
        }
        return sourceIndices;
    }

    void callbackEnumCopyTextToClipboard(const QString& contentText)
    {
        // 作用：统一写入系统剪贴板；QApplication 未就绪时静默跳过。
        // 返回：无。
        QClipboard* clipboard = QApplication::clipboard();
        if (clipboard != nullptr)
        {
            clipboard->setText(contentText);
        }
    }
}

void KernelDock::initializeCallbackEnumTab()
{
    if (m_callbackEnumPage == nullptr || m_callbackEnumLayout != nullptr)
    {
        return;
    }

    m_callbackEnumLayout = new QVBoxLayout(m_callbackEnumPage);
    m_callbackEnumLayout->setContentsMargins(4, 4, 4, 4);
    m_callbackEnumLayout->setSpacing(6);

    m_callbackEnumToolLayout = new QHBoxLayout();
    m_callbackEnumToolLayout->setContentsMargins(0, 0, 0, 0);
    m_callbackEnumToolLayout->setSpacing(6);

    m_refreshCallbackEnumButton = new QPushButton(QIcon(":/Icon/process_refresh.svg"), QString(), m_callbackEnumPage);
    m_refreshCallbackEnumButton->setToolTip(QStringLiteral("刷新回调遍历结果"));
    m_refreshCallbackEnumButton->setStyleSheet(callbackEnumButtonStyle());
    m_refreshCallbackEnumButton->setFixedWidth(34);

    m_callbackEnumFilterEdit = new QLineEdit(m_callbackEnumPage);
    m_callbackEnumFilterEdit->setPlaceholderText(QStringLiteral("按类别/来源/可信状态/移除策略/名称/地址/模块/Altitude筛选"));
    m_callbackEnumFilterEdit->setToolTip(QStringLiteral("输入关键字后实时过滤回调遍历结果"));
    m_callbackEnumFilterEdit->setClearButtonEnabled(true);
    m_callbackEnumFilterEdit->setStyleSheet(callbackEnumInputStyle());

    m_callbackEnumStatusLabel = new QLabel(QStringLiteral("状态：等待刷新"), m_callbackEnumPage);
    m_callbackEnumStatusLabel->setStyleSheet(callbackEnumStatusLabelStyle(KswordTheme::TextSecondaryHex()));

    m_callbackEnumToolLayout->addWidget(m_refreshCallbackEnumButton, 0);
    m_callbackEnumToolLayout->addWidget(m_callbackEnumFilterEdit, 1);
    m_callbackEnumToolLayout->addWidget(m_callbackEnumStatusLabel, 0);
    m_callbackEnumLayout->addLayout(m_callbackEnumToolLayout);

    QSplitter* splitter = new QSplitter(Qt::Vertical, m_callbackEnumPage);
    m_callbackEnumLayout->addWidget(splitter, 1);

    m_callbackEnumTable = new QTableWidget(splitter);
    m_callbackEnumTable->setColumnCount(static_cast<int>(CallbackEnumColumn::Count));
    m_callbackEnumTable->setHorizontalHeaderLabels(QStringList{
        QStringLiteral("类别"),
        QStringLiteral("来源"),
        QStringLiteral("可信状态"),
        QStringLiteral("状态"),
        QStringLiteral("移除策略"),
        QStringLiteral("名称"),
        QStringLiteral("回调/对象地址"),
        QStringLiteral("模块"),
        QStringLiteral("Altitude")
        });
    m_callbackEnumTable->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_callbackEnumTable->setSelectionMode(QAbstractItemView::ExtendedSelection);
    m_callbackEnumTable->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_callbackEnumTable->setAlternatingRowColors(true);
    m_callbackEnumTable->setStyleSheet(callbackEnumSelectionStyle());
    m_callbackEnumTable->setCornerButtonEnabled(false);
    m_callbackEnumTable->setContextMenuPolicy(Qt::CustomContextMenu);
    m_callbackEnumTable->verticalHeader()->setVisible(false);
    m_callbackEnumTable->horizontalHeader()->setStyleSheet(callbackEnumHeaderStyle());
    m_callbackEnumTable->horizontalHeader()->setSectionResizeMode(QHeaderView::ResizeToContents);
    m_callbackEnumTable->horizontalHeader()->setSectionResizeMode(static_cast<int>(CallbackEnumColumn::Name), QHeaderView::Stretch);
    m_callbackEnumTable->setColumnWidth(static_cast<int>(CallbackEnumColumn::Trust), 170);
    m_callbackEnumTable->setColumnWidth(static_cast<int>(CallbackEnumColumn::RemovePolicy), 200);
    m_callbackEnumTable->setColumnWidth(static_cast<int>(CallbackEnumColumn::CallbackAddress), 180);
    m_callbackEnumTable->setColumnWidth(static_cast<int>(CallbackEnumColumn::Module), 220);

    m_callbackEnumDetailEditor = new CodeEditorWidget(splitter);
    m_callbackEnumDetailEditor->setReadOnly(true);
    m_callbackEnumDetailEditor->setText(QStringLiteral("请选择一条回调记录查看详情。"));

    splitter->setStretchFactor(0, 3);
    splitter->setStretchFactor(1, 2);

    connect(m_refreshCallbackEnumButton, &QPushButton::clicked, this, [this]() {
        refreshCallbackEnumAsync();
    });
    connect(m_callbackEnumFilterEdit, &QLineEdit::textChanged, this, [this](const QString& filterText) {
        rebuildCallbackEnumTable(filterText.trimmed());
    });
    connect(m_callbackEnumTable, &QTableWidget::currentCellChanged, this, [this](int, int, int, int) {
        showCallbackEnumDetailByCurrentRow();
    });
    connect(m_callbackEnumTable, &QTableWidget::customContextMenuRequested, this, [this](const QPoint& localPosition) {
        showCallbackEnumContextMenu(localPosition);
    });
}

void KernelDock::refreshCallbackEnumAsync()
{
    if (m_callbackEnumRefreshRunning.exchange(true))
    {
        return;
    }

    if (m_refreshCallbackEnumButton != nullptr)
    {
        m_refreshCallbackEnumButton->setEnabled(false);
    }
    if (m_callbackEnumStatusLabel != nullptr)
    {
        m_callbackEnumStatusLabel->setText(QStringLiteral("状态：刷新中..."));
        m_callbackEnumStatusLabel->setStyleSheet(callbackEnumStatusLabelStyle(KswordTheme::PrimaryBlueHex));
    }

    QPointer<KernelDock> guardThis(this);
    std::thread([guardThis]() {
        std::vector<KernelCallbackEnumEntry> resultRows;
        QString errorText;
        std::uint32_t responseFlags = 0;
        const ksword::ark::DriverClient driverClient;
        const ksword::ark::CallbackEnumResult enumResult = driverClient.enumerateCallbacks();
        const bool success = enumResult.io.ok;

        if (success)
        {
            responseFlags = enumResult.flags;
            resultRows.reserve(enumResult.entries.size());
            for (const ksword::ark::CallbackEnumEntry& entry : enumResult.entries)
            {
                resultRows.push_back(callbackEnumConvertEntry(entry));
            }
        }
        else
        {
            errorText = QStringLiteral("回调遍历 IOCTL 调用失败。\nWin32=%1\n详情=%2")
                .arg(enumResult.io.win32Error)
                .arg(QString::fromStdString(enumResult.io.message));
        }

        QMetaObject::invokeMethod(guardThis, [guardThis, success, errorText, responseFlags, resultRows = std::move(resultRows)]() mutable {
            if (guardThis == nullptr)
            {
                return;
            }

            guardThis->m_callbackEnumRefreshRunning.store(false);
            if (guardThis->m_refreshCallbackEnumButton != nullptr)
            {
                guardThis->m_refreshCallbackEnumButton->setEnabled(true);
            }

            if (!success)
            {
                guardThis->m_callbackEnumStatusLabel->setText(QStringLiteral("状态：刷新失败"));
                guardThis->m_callbackEnumStatusLabel->setStyleSheet(callbackEnumStatusLabelStyle(QStringLiteral("#B23A3A")));
                guardThis->m_callbackEnumDetailEditor->setText(errorText);
                return;
            }

            guardThis->m_callbackEnumRows = std::move(resultRows);
            guardThis->rebuildCallbackEnumTable(guardThis->m_callbackEnumFilterEdit->text().trimmed());

            std::size_t unsupportedCount = 0U;
            for (const KernelCallbackEnumEntry& entry : guardThis->m_callbackEnumRows)
            {
                if (entry.status == KSWORD_ARK_CALLBACK_ENUM_STATUS_UNSUPPORTED)
                {
                    ++unsupportedCount;
                }
            }

            const bool truncated = (responseFlags & KSWORD_ARK_ENUM_CALLBACK_RESPONSE_FLAG_TRUNCATED) != 0U;
            guardThis->m_callbackEnumStatusLabel->setText(
                QStringLiteral("状态：已刷新 %1 项，私有未支持 %2 项%3")
                .arg(guardThis->m_callbackEnumRows.size())
                .arg(unsupportedCount)
                .arg(truncated ? QStringLiteral("，响应截断") : QString()));
            guardThis->m_callbackEnumStatusLabel->setStyleSheet(callbackEnumStatusLabelStyle(
                truncated ? QStringLiteral("#D77A00") : QStringLiteral("#3A8F3A")));

            if (guardThis->m_callbackEnumTable->rowCount() > 0)
            {
                guardThis->m_callbackEnumTable->setCurrentCell(0, 0);
            }
            else
            {
                guardThis->m_callbackEnumDetailEditor->setText(QStringLiteral("当前环境未返回可见回调记录。"));
            }
        }, Qt::QueuedConnection);
    }).detach();
}

void KernelDock::rebuildCallbackEnumTable(const QString& filterKeyword)
{
    if (m_callbackEnumTable == nullptr)
    {
        return;
    }

    m_callbackEnumTable->setSortingEnabled(false);
    m_callbackEnumTable->setRowCount(0);

    for (std::size_t sourceIndex = 0; sourceIndex < m_callbackEnumRows.size(); ++sourceIndex)
    {
        const KernelCallbackEnumEntry& entry = m_callbackEnumRows[sourceIndex];
        const QString addressText = callbackEnumPrimaryAddressText(entry);
        const QString moduleText = entry.modulePathText.isEmpty() ? QStringLiteral("<未解析>") : entry.modulePathText;
        const bool matched = filterKeyword.isEmpty()
            || entry.classText.contains(filterKeyword, Qt::CaseInsensitive)
            || entry.sourceText.contains(filterKeyword, Qt::CaseInsensitive)
            || entry.sourceTrustText.contains(filterKeyword, Qt::CaseInsensitive)
            || entry.statusText.contains(filterKeyword, Qt::CaseInsensitive)
            || entry.removePolicyText.contains(filterKeyword, Qt::CaseInsensitive)
            || entry.nameText.contains(filterKeyword, Qt::CaseInsensitive)
            || addressText.contains(filterKeyword, Qt::CaseInsensitive)
            || moduleText.contains(filterKeyword, Qt::CaseInsensitive)
            || entry.altitudeText.contains(filterKeyword, Qt::CaseInsensitive)
            || entry.detailText.contains(filterKeyword, Qt::CaseInsensitive);
        if (!matched)
        {
            continue;
        }

        const int rowIndex = m_callbackEnumTable->rowCount();
        m_callbackEnumTable->insertRow(rowIndex);

        auto* classItem = new QTableWidgetItem(entry.classText);
        classItem->setData(Qt::UserRole, static_cast<qulonglong>(sourceIndex));
        auto* sourceItem = new QTableWidgetItem(entry.sourceText);
        auto* trustItem = new QTableWidgetItem(entry.sourceTrustText);
        auto* statusItem = new QTableWidgetItem(entry.statusText);
        auto* removePolicyItem = new QTableWidgetItem(entry.removePolicyText);
        auto* nameItem = new QTableWidgetItem(callbackEnumSafeText(entry.nameText));
        auto* addressItem = new QTableWidgetItem(addressText);
        auto* moduleItem = new QTableWidgetItem(moduleText);
        auto* altitudeItem = new QTableWidgetItem(callbackEnumSafeText(entry.altitudeText));

        if (callbackEnumIsTrustedSource(entry))
        {
            trustItem->setForeground(QBrush(QColor(QStringLiteral("#3A8F3A"))));
        }
        else if (entry.fallbackPatternOnly)
        {
            trustItem->setForeground(QBrush(QColor(QStringLiteral("#D77A00"))));
        }
        else if (callbackEnumIsUnsupportedSource(entry))
        {
            trustItem->setForeground(QBrush(QColor(QStringLiteral("#8A8A8A"))));
        }

        if (entry.status == KSWORD_ARK_CALLBACK_ENUM_STATUS_UNSUPPORTED)
        {
            statusItem->setForeground(QBrush(QColor(QStringLiteral("#D77A00"))));
        }
        else if (entry.status == KSWORD_ARK_CALLBACK_ENUM_STATUS_QUERY_FAILED)
        {
            statusItem->setForeground(QBrush(QColor(QStringLiteral("#B23A3A"))));
        }

        switch (callbackEnumRemovePolicyKind(entry))
        {
        case CallbackEnumRemovePolicyKind::RemovableVerified:
            removePolicyItem->setForeground(QBrush(QColor(QStringLiteral("#3A8F3A"))));
            break;
        case CallbackEnumRemovePolicyKind::RemovableCandidate:
        case CallbackEnumRemovePolicyKind::ExperimentalOnly:
            removePolicyItem->setForeground(QBrush(QColor(QStringLiteral("#D77A00"))));
            break;
        case CallbackEnumRemovePolicyKind::NotRemovable:
        default:
            removePolicyItem->setForeground(QBrush(QColor(QStringLiteral("#8A8A8A"))));
            break;
        }

        m_callbackEnumTable->setItem(rowIndex, static_cast<int>(CallbackEnumColumn::Class), classItem);
        m_callbackEnumTable->setItem(rowIndex, static_cast<int>(CallbackEnumColumn::Source), sourceItem);
        m_callbackEnumTable->setItem(rowIndex, static_cast<int>(CallbackEnumColumn::Trust), trustItem);
        m_callbackEnumTable->setItem(rowIndex, static_cast<int>(CallbackEnumColumn::Status), statusItem);
        m_callbackEnumTable->setItem(rowIndex, static_cast<int>(CallbackEnumColumn::RemovePolicy), removePolicyItem);
        m_callbackEnumTable->setItem(rowIndex, static_cast<int>(CallbackEnumColumn::Name), nameItem);
        m_callbackEnumTable->setItem(rowIndex, static_cast<int>(CallbackEnumColumn::CallbackAddress), addressItem);
        m_callbackEnumTable->setItem(rowIndex, static_cast<int>(CallbackEnumColumn::Module), moduleItem);
        m_callbackEnumTable->setItem(rowIndex, static_cast<int>(CallbackEnumColumn::Altitude), altitudeItem);
    }

    m_callbackEnumTable->setSortingEnabled(true);
}

bool KernelDock::currentCallbackEnumSourceIndex(std::size_t& sourceIndexOut) const
{
    sourceIndexOut = 0U;
    if (m_callbackEnumTable == nullptr)
    {
        return false;
    }

    const int currentRow = m_callbackEnumTable->currentRow();
    if (currentRow < 0)
    {
        return false;
    }

    QTableWidgetItem* classItem = m_callbackEnumTable->item(currentRow, static_cast<int>(CallbackEnumColumn::Class));
    if (classItem == nullptr)
    {
        return false;
    }

    sourceIndexOut = static_cast<std::size_t>(classItem->data(Qt::UserRole).toULongLong());
    return sourceIndexOut < m_callbackEnumRows.size();
}

const KernelCallbackEnumEntry* KernelDock::currentCallbackEnumEntry() const
{
    std::size_t sourceIndex = 0U;
    if (!currentCallbackEnumSourceIndex(sourceIndex))
    {
        return nullptr;
    }
    return &m_callbackEnumRows[sourceIndex];
}

void KernelDock::showCallbackEnumDetailByCurrentRow()
{
    if (m_callbackEnumDetailEditor == nullptr)
    {
        return;
    }

    const KernelCallbackEnumEntry* entry = currentCallbackEnumEntry();
    if (entry == nullptr)
    {
        m_callbackEnumDetailEditor->setText(QStringLiteral("请选择一条回调记录查看详情。"));
        return;
    }

    const QString win32ModulePath = callbackEnumNormalizeModulePath(entry->modulePathText);
    const QString detailText = QStringLiteral(
        "类别: %1\n"
        "来源: %2\n"
        "可信状态: %3\n"
        "移除策略: %4\n"
        "是否需要二次确认: %5\n"
        "当前来源是否只是 fallback/pattern: %6\n"
        "状态: %7\n"
        "名称: %8\n"
        "Altitude: %9\n"
        "主地址显示: %10\n"
        "真实回调地址: %11\n"
        "上下文/诊断值: %12\n"
        "注册句柄/Cookie/全局节点: %13\n"
        "模块路径: %14\n"
        "Win32模块路径: %15\n"
        "模块基址: %16\n"
        "模块大小: 0x%17\n"
        "操作掩码: 0x%18\n"
        "对象类型掩码: 0x%19\n"
        "字段标志: 0x%20\n"
        "可信标志(预留): 0x%21\n"
        "移除标志(预留): 0x%22\n"
        "Generation(预留): %23\n"
        "IdentityHash(预留): %24\n"
        "RawStorageValue(预留): %25\n"
        "LastStatus: 0x%26\n\n"
        "说明: 主地址显示会优先显示真实回调函数；定位/诊断行没有真实回调函数时显示全局数组、链表节点、标识符或诊断值。"
        "旧协议尚未返回 generation/identity hash/raw storage value 时保持 0 或空值；实验 unlink 仅为 UI 预留，不作为默认路径。\n\n"
        "详情:\n%27")
        .arg(entry->classText)
        .arg(entry->sourceText)
        .arg(entry->sourceTrustText)
        .arg(entry->removePolicyText)
        .arg(callbackEnumYesNoText(entry->requiresSecondConfirmation))
        .arg(callbackEnumYesNoText(entry->fallbackPatternOnly))
        .arg(entry->statusText)
        .arg(callbackEnumSafeText(entry->nameText))
        .arg(callbackEnumSafeText(entry->altitudeText))
        .arg(callbackEnumPrimaryAddressText(*entry))
        .arg(callbackEnumFormatAddress(entry->callbackAddress))
        .arg(callbackEnumFormatAddress(entry->contextAddress))
        .arg(callbackEnumFormatAddress(entry->registrationAddress))
        .arg(entry->modulePathText.isEmpty() ? QStringLiteral("<未解析>") : entry->modulePathText)
        .arg(win32ModulePath.isEmpty() ? QStringLiteral("<不可映射或不存在>") : win32ModulePath)
        .arg(callbackEnumFormatAddress(entry->moduleBase))
        .arg(QString::number(static_cast<qulonglong>(entry->moduleSize), 16).toUpper())
        .arg(static_cast<qulonglong>(entry->operationMask), 8, 16, QChar('0'))
        .arg(static_cast<qulonglong>(entry->objectTypeMask), 8, 16, QChar('0'))
        .arg(static_cast<qulonglong>(entry->fieldFlags), 8, 16, QChar('0'))
        .arg(static_cast<qulonglong>(entry->trustFlags), 8, 16, QChar('0'))
        .arg(static_cast<qulonglong>(entry->removeFlags), 8, 16, QChar('0'))
        .arg(static_cast<qulonglong>(entry->generation))
        .arg(callbackEnumIdentityHashText(entry->identityHash))
        .arg(callbackEnumFormatAddress(entry->rawStorageValue))
        .arg(static_cast<qulonglong>(static_cast<std::uint32_t>(entry->lastStatus)), 8, 16, QChar('0'))
        .arg(callbackEnumSafeText(entry->detailText, QStringLiteral("<无详情>")));

    m_callbackEnumDetailEditor->setText(detailText);
}

void KernelDock::showCallbackEnumContextMenu(const QPoint& localPosition)
{
    if (m_callbackEnumTable == nullptr)
    {
        return;
    }

    // 右键选区规则：
    // - 点在未选中行上时切换为该单行；
    // - 点在已选中行上时保留 Ctrl 多选集合；
    // - 点在空白处时保留现有选择，复制动作继续对当前选择生效。
    QTableWidgetItem* clickedItem = m_callbackEnumTable->itemAt(localPosition);
    const int clickedRow = clickedItem != nullptr ? clickedItem->row() : -1;
    const int clickedColumn = m_callbackEnumTable->columnAt(localPosition.x());
    if (clickedItem != nullptr)
    {
        if (!clickedItem->isSelected())
        {
            m_callbackEnumTable->clearSelection();
            m_callbackEnumTable->setCurrentItem(clickedItem);
            m_callbackEnumTable->selectRow(clickedRow);
        }
        else
        {
            if (QItemSelectionModel* selectionModel = m_callbackEnumTable->selectionModel())
            {
                // 右键点在已选中行时只移动当前单元格，不清空 Ctrl 多选集合。
                selectionModel->setCurrentIndex(
                    m_callbackEnumTable->indexFromItem(clickedItem),
                    QItemSelectionModel::NoUpdate);
            }
        }
    }

    const int fallbackRow = clickedRow >= 0 ? clickedRow : m_callbackEnumTable->currentRow();
    const std::vector<std::size_t> selectedSourceIndices =
        callbackEnumSelectedSourceIndices(m_callbackEnumTable, m_callbackEnumRows, fallbackRow);
    const bool hasSelection = !selectedSourceIndices.empty();
    QString clickedModulePath;
    if (clickedRow >= 0)
    {
        std::vector<std::size_t> clickedSourceIndices =
            callbackEnumSelectedSourceIndices(m_callbackEnumTable, m_callbackEnumRows, clickedRow);
        if (!clickedSourceIndices.empty() && clickedSourceIndices.front() < m_callbackEnumRows.size())
        {
            clickedModulePath = callbackEnumNormalizeModulePath(
                m_callbackEnumRows[clickedSourceIndices.front()].modulePathText);
        }
    }
    if (clickedModulePath.isEmpty() && !selectedSourceIndices.empty() && selectedSourceIndices.front() < m_callbackEnumRows.size())
    {
        clickedModulePath = callbackEnumNormalizeModulePath(
            m_callbackEnumRows[selectedSourceIndices.front()].modulePathText);
    }
    const bool hasModuleFile = !clickedModulePath.isEmpty() && QFileInfo(clickedModulePath).exists();
    const KernelCallbackEnumEntry* actionEntry = nullptr;
    if (selectedSourceIndices.size() == 1U && selectedSourceIndices.front() < m_callbackEnumRows.size())
    {
        actionEntry = &m_callbackEnumRows[selectedSourceIndices.front()];
    }
    const bool hasSingleActionEntry = actionEntry != nullptr;
    const bool canUseLegacySafeRemove =
        hasSingleActionEntry && callbackEnumCanUseLegacySafeRemove(*actionEntry);

    QMenu contextMenu(this);
    contextMenu.setStyleSheet(KswordTheme::ContextMenuStyle());

    QAction* refreshAction = contextMenu.addAction(
        QIcon(":/Icon/process_refresh.svg"),
        QStringLiteral("刷新回调遍历"));
    QAction* openModuleFolderAction = contextMenu.addAction(
        QIcon(":/Icon/process_open_folder.svg"),
        QStringLiteral("打开模块所在目录"));
    QAction* moduleFileDetailAction = contextMenu.addAction(
        QIcon(":/Icon/process_details.svg"),
        QStringLiteral("模块文件详细信息"));
    openModuleFolderAction->setEnabled(hasModuleFile);
    moduleFileDetailAction->setEnabled(hasModuleFile);
    contextMenu.addSeparator();

    QAction* safeRemoveAction = contextMenu.addAction(QStringLiteral("安全移除（公开 API）"));
    safeRemoveAction->setToolTip(QStringLiteral("通过 ArkDriverClient::removeExternalCallback 调用旧协议安全移除路径"));
    safeRemoveAction->setEnabled(canUseLegacySafeRemove);
    QAction* experimentalUnlinkAction = contextMenu.addAction(QStringLiteral("实验性强制移除（unlink）"));
    experimentalUnlinkAction->setToolTip(QStringLiteral("UI 骨架：需要强确认；当前协议未启用时不会发送 IOCTL"));
    experimentalUnlinkAction->setEnabled(hasSingleActionEntry);
    contextMenu.addSeparator();

    QMenu* copyMenu = contextMenu.addMenu(
        QIcon(":/Icon/process_copy_row.svg"),
        QStringLiteral("复制"));
    QAction* copyCurrentColumnAction = copyMenu->addAction(
        QIcon(":/Icon/process_copy_cell.svg"),
        QStringLiteral("复制当前列（选中行）"));
    QAction* copySelectedRowsAction = copyMenu->addAction(
        QIcon(":/Icon/process_copy_row.svg"),
        QStringLiteral("复制选中行（TSV）"));
    QAction* copySelectedRowsWithHeaderAction = copyMenu->addAction(
        QStringLiteral("复制表头+选中行（TSV）"));
    QAction* copyDetailAction = copyMenu->addAction(
        QStringLiteral("复制详情（选中行）"));
    copyMenu->addSeparator();

    QMenu* copyColumnMenu = copyMenu->addMenu(QStringLiteral("复制指定栏目（选中行）"));
    for (int columnIndex = 0; columnIndex < static_cast<int>(CallbackEnumColumn::Count); ++columnIndex)
    {
        const CallbackEnumColumn column = static_cast<CallbackEnumColumn>(columnIndex);
        QAction* columnAction = copyColumnMenu->addAction(callbackEnumColumnHeaderText(column));
        columnAction->setData(columnIndex);
    }

    copyCurrentColumnAction->setEnabled(hasSelection);
    copySelectedRowsAction->setEnabled(hasSelection);
    copySelectedRowsWithHeaderAction->setEnabled(hasSelection);
    copyDetailAction->setEnabled(hasSelection);
    copyColumnMenu->setEnabled(hasSelection);

    QAction* selectedAction = contextMenu.exec(m_callbackEnumTable->viewport()->mapToGlobal(localPosition));
    if (selectedAction == nullptr)
    {
        return;
    }

    if (selectedAction == refreshAction)
    {
        refreshCallbackEnumAsync();
        return;
    }

    if (selectedAction == openModuleFolderAction)
    {
        const bool opened = callbackEnumOpenModuleInExplorer(clickedModulePath);
        if (m_callbackEnumStatusLabel != nullptr)
        {
            m_callbackEnumStatusLabel->setText(opened
                ? QStringLiteral("状态：已打开模块所在目录")
                : QStringLiteral("状态：打开模块所在目录失败"));
        }
        return;
    }

    if (selectedAction == moduleFileDetailAction)
    {
        callbackEnumShowModuleFileDetailDialog(this, clickedModulePath);
        if (m_callbackEnumStatusLabel != nullptr)
        {
            m_callbackEnumStatusLabel->setText(QStringLiteral("状态：已打开模块文件详细信息"));
        }
        return;
    }

    if (selectedAction == safeRemoveAction)
    {
        if (actionEntry != nullptr)
        {
            callbackEnumExecuteLegacySafeRemove(
                this,
                m_callbackEnumStatusLabel,
                m_callbackEnumDetailEditor,
                *actionEntry);
        }
        return;
    }

    if (selectedAction == experimentalUnlinkAction)
    {
        if (actionEntry != nullptr)
        {
            callbackEnumShowExperimentalUnlinkNotice(
                this,
                m_callbackEnumStatusLabel,
                m_callbackEnumDetailEditor,
                *actionEntry);
        }
        return;
    }

    if (!hasSelection)
    {
        return;
    }

    const auto buildColumnText = [this, &selectedSourceIndices](const CallbackEnumColumn column) -> QString
    {
        // 作用：把指定栏目在所有选中行中的值拼成多行文本。
        // 返回：以换行分隔的栏目值。
        QStringList valueList;
        valueList.reserve(static_cast<int>(selectedSourceIndices.size()));
        for (const std::size_t sourceIndex : selectedSourceIndices)
        {
            if (sourceIndex < m_callbackEnumRows.size())
            {
                valueList.push_back(callbackEnumEntryColumnText(m_callbackEnumRows[sourceIndex], column));
            }
        }
        return valueList.join('\n');
    };

    if (selectedAction == copyCurrentColumnAction)
    {
        int activeColumn = clickedColumn >= 0 ? clickedColumn : m_callbackEnumTable->currentColumn();
        if (activeColumn < 0 || activeColumn >= static_cast<int>(CallbackEnumColumn::Count))
        {
            activeColumn = static_cast<int>(CallbackEnumColumn::Class);
        }
        callbackEnumCopyTextToClipboard(buildColumnText(static_cast<CallbackEnumColumn>(activeColumn)));
        if (m_callbackEnumStatusLabel != nullptr)
        {
            m_callbackEnumStatusLabel->setText(QStringLiteral("状态：已复制 %1 行的“%2”栏目")
                .arg(static_cast<qulonglong>(selectedSourceIndices.size()))
                .arg(callbackEnumColumnHeaderText(static_cast<CallbackEnumColumn>(activeColumn))));
        }
        return;
    }

    if (selectedAction == copySelectedRowsAction || selectedAction == copySelectedRowsWithHeaderAction)
    {
        QStringList rowList;
        rowList.reserve(static_cast<int>(selectedSourceIndices.size()) + 1);
        if (selectedAction == copySelectedRowsWithHeaderAction)
        {
            rowList.push_back(callbackEnumHeaderAsTsv());
        }
        for (const std::size_t sourceIndex : selectedSourceIndices)
        {
            if (sourceIndex < m_callbackEnumRows.size())
            {
                rowList.push_back(callbackEnumEntryAsTsv(m_callbackEnumRows[sourceIndex]));
            }
        }
        callbackEnumCopyTextToClipboard(rowList.join('\n'));
        if (m_callbackEnumStatusLabel != nullptr)
        {
            m_callbackEnumStatusLabel->setText(QStringLiteral("状态：已复制 %1 行回调记录")
                .arg(static_cast<qulonglong>(selectedSourceIndices.size())));
        }
        return;
    }

    if (selectedAction == copyDetailAction)
    {
        QStringList detailList;
        detailList.reserve(static_cast<int>(selectedSourceIndices.size()));
        for (const std::size_t sourceIndex : selectedSourceIndices)
        {
            if (sourceIndex >= m_callbackEnumRows.size())
            {
                continue;
            }

            const KernelCallbackEnumEntry& entry = m_callbackEnumRows[sourceIndex];
            detailList.push_back(QStringLiteral("[%1] %2\n%3")
                .arg(entry.classText)
                .arg(callbackEnumSafeText(entry.nameText))
                .arg(callbackEnumSafeText(entry.detailText, QStringLiteral("<无详情>"))));
        }
        callbackEnumCopyTextToClipboard(detailList.join(QStringLiteral("\n\n---\n\n")));
        if (m_callbackEnumStatusLabel != nullptr)
        {
            m_callbackEnumStatusLabel->setText(QStringLiteral("状态：已复制 %1 行详情")
                .arg(static_cast<qulonglong>(selectedSourceIndices.size())));
        }
        return;
    }

    const QList<QAction*> columnActionList = copyColumnMenu->actions();
    if (columnActionList.contains(selectedAction))
    {
        const int columnIndex = selectedAction->data().toInt();
        if (columnIndex >= 0 && columnIndex < static_cast<int>(CallbackEnumColumn::Count))
        {
            const CallbackEnumColumn column = static_cast<CallbackEnumColumn>(columnIndex);
            callbackEnumCopyTextToClipboard(buildColumnText(column));
            if (m_callbackEnumStatusLabel != nullptr)
            {
                m_callbackEnumStatusLabel->setText(QStringLiteral("状态：已复制 %1 行的“%2”栏目")
                    .arg(static_cast<qulonglong>(selectedSourceIndices.size()))
                    .arg(callbackEnumColumnHeaderText(column)));
            }
        }
    }
}
