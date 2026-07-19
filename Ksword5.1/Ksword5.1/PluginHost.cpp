#include "PluginHost.h"
#include "UI/VisibleTableWidget.h"

#include "theme.h"
#include "Internationalization/LanguageManager.h"

#include <QAbstractItemView>
#include <QBrush>
#include <QCheckBox>
#include <QCloseEvent>
#include <QColor>
#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDesktopServices>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDir>
#include <QEvent>
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QIcon>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QMenu>
#include <QMessageBox>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QPlainTextEdit>
#include <QProcess>
#include <QProcessEnvironment>
#include <QProgressBar>
#include <QPointer>
#include <QPushButton>
#include <QRegularExpression>
#include <QSaveFile>
#include <QShowEvent>
#include <QStandardPaths>
#include <QTableWidget>
#include <QTableWidgetItem>
#include <QTabWidget>
#include <QTextDocument>
#include <QTimer>
#include <QUrl>
#include <QUuid>
#include <QVBoxLayout>
#include <QWidget>

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>

namespace
{
    constexpr qint64 kMaxManifestBytes = 64 * 1024;
    constexpr qint64 kMaxMarketplaceArchiveBytes = 256LL * 1024LL * 1024LL;
    constexpr int kMaxVisualizationColumns = 8;
    constexpr int kMaxVisualizationSummaryItems = 8;
    constexpr int kMaxVisualizationRows = 10000;
    constexpr int kMaxBufferedStdoutBytes = 1024 * 1024;
    constexpr char kMarketplaceCatalogUrl[] = "https://raw.githubusercontent.com/KSwordDEV/Plugins/main/catalog.json";

    struct VisualizationValueStyle
    {
        QString label;
        QString tone;
    };

    struct VisualizationField
    {
        QString field;
        QString label;
        QString format;
        QHash<QString, VisualizationValueStyle> valueStyles;
    };

    struct PluginVisualization
    {
        bool enabled = false;
        QString type;
        QString title;
        QString startEvent;
        QString resultEvent;
        QString completeEvent;
        QString totalField;
        QList<VisualizationField> columns;
        QList<VisualizationField> summary;
    };

    struct PluginTabPresentation
    {
        bool enabled = false;
        QString title;
        QString readyEvent;
        int startupTimeoutMs = 15000;
    };

    struct PluginDescriptor
    {
        QString id;
        QString name;
        QString version;
        QString description;
        QString pluginType = QStringLiteral("command");
        QString runtime;
        QString entrypointPath;
        QString defaultCommand;
        QString pluginDirectory;
        QStringList targets;
        PluginVisualization visualization;
        PluginTabPresentation tabPresentation;
    };

    struct PluginListResult
    {
        QList<PluginDescriptor> plugins;
        QString pluginRoot;
        QStringList ignoredManifests;
    };

    struct MarketplacePlugin
    {
        QString id;
        QString name;
        QString version;
        QString description;
        QStringList targets;
        QString installDirectory;
        QUrl archiveUrl;
        QString sha256;
        QString licenseName;
        QUrl licenseUrl;
    };

    bool isValidPluginId(const QString& id)
    {
        if (id.isEmpty() || id.size() > 64 || id.front() == QChar('-') || id.back() == QChar('-'))
        {
            return false;
        }
        for (const QChar character : id)
        {
            const bool lower = character >= QChar('a') && character <= QChar('z');
            const bool digit = character >= QChar('0') && character <= QChar('9');
            if (!lower && !digit && character != QChar('-'))
            {
                return false;
            }
        }
        return true;
    }

    bool isSafeRelativePath(const QString& value)
    {
        if (value.isEmpty() || value.size() > 240 || QDir::isAbsolutePath(value) || value.contains(QChar(':')))
        {
            return false;
        }
        QString normalized = value;
        normalized.replace(QChar('\\'), QChar('/'));
        const QStringList components = normalized.split(QChar('/'), Qt::SkipEmptyParts);
        if (components.isEmpty())
        {
            return false;
        }
        for (const QString& component : components)
        {
            if (component == QStringLiteral(".") || component == QStringLiteral(".."))
            {
                return false;
            }
        }
        return !value.startsWith(QChar('/')) && !value.startsWith(QChar('\\'));
    }

    bool isSafeCommandToken(const QString& value)
    {
        return !value.isEmpty() && value.size() <= 64 &&
            !value.contains(QChar('/')) && !value.contains(QChar('\\')) &&
            !value.contains(QChar(':')) && value != QStringLiteral(".") && value != QStringLiteral("..");
    }

    QString findPluginRoot()
    {
        const QString configuredRoot = qEnvironmentVariable("KSWORD_PLUGIN_ROOT").trimmed();
        if (!configuredRoot.isEmpty() && QDir(configuredRoot).exists())
        {
            return QDir(configuredRoot).absolutePath();
        }

        const QString currentCandidate = QDir::current().filePath(QStringLiteral("plugin"));
        if (QDir(currentCandidate).exists())
        {
            return QDir(currentCandidate).absolutePath();
        }

        QDir searchDirectory(QCoreApplication::applicationDirPath());
        for (int depth = 0; depth < 7; ++depth)
        {
            const QString candidate = searchDirectory.filePath(QStringLiteral("plugin"));
            if (QDir(candidate).exists())
            {
                return QDir(candidate).absolutePath();
            }
            if (!searchDirectory.cdUp())
            {
                break;
            }
        }
        return {};
    }

    QString resolvePluginInstallRoot()
    {
        const QString configuredRoot = qEnvironmentVariable("KSWORD_PLUGIN_ROOT").trimmed();
        if (!configuredRoot.isEmpty())
        {
            return QDir(configuredRoot).absolutePath();
        }

        const QString existingRoot = findPluginRoot();
        if (!existingRoot.isEmpty())
        {
            return existingRoot;
        }

        // 新安装的发行版尚未拥有 plugin\ 目录时，首个商城插件安装到主程序旁边。
        return QDir(QCoreApplication::applicationDirPath()).filePath(QStringLiteral("plugin"));
    }

    QString quotePowerShellLiteral(const QString& value)
    {
        QString escaped = value;
        escaped.replace(QChar('\''), QStringLiteral("''"));
        return QChar('\'') + escaped + QChar('\'');
    }

    bool readRequiredString(const QJsonObject& object, const char* key, QString* valueOut, QString* errorOut)
    {
        const QString value = object.value(QLatin1String(key)).toString().trimmed();
        if (value.isEmpty())
        {
            *errorOut = QStringLiteral("缺少或无效的清单字符串字段：%1").arg(QLatin1String(key));
            return false;
        }
        *valueOut = value;
        return true;
    }

    bool isValidProtocolName(const QString& value)
    {
        static const QRegularExpression pattern(QStringLiteral("^[A-Za-z_][A-Za-z0-9_.-]{0,63}$"));
        return pattern.match(value).hasMatch();
    }

    bool isAllowedVisualizationFormat(const QString& format)
    {
        return format == QStringLiteral("text") ||
            format == QStringLiteral("path") ||
            format == QStringLiteral("percent") ||
            format == QStringLiteral("integer") ||
            format == QStringLiteral("number") ||
            format == QStringLiteral("badge");
    }

    bool isAllowedVisualizationTone(const QString& tone)
    {
        return tone == QStringLiteral("success") ||
            tone == QStringLiteral("danger") ||
            tone == QStringLiteral("warning") ||
            tone == QStringLiteral("info") ||
            tone == QStringLiteral("muted");
    }

    bool parseVisualizationField(
        const QJsonValue& value,
        VisualizationField* fieldOut,
        QString* errorOut)
    {
        if (fieldOut == nullptr || errorOut == nullptr || !value.isObject())
        {
            if (errorOut != nullptr) *errorOut = QStringLiteral("visualization 字段定义必须是对象。");
            return false;
        }

        const QJsonObject object = value.toObject();
        VisualizationField field;
        if (!readRequiredString(object, "field", &field.field, errorOut) ||
            !readRequiredString(object, "label", &field.label, errorOut) ||
            !readRequiredString(object, "format", &field.format, errorOut))
        {
            return false;
        }
        field.format = field.format.toLower();
        if (!isValidProtocolName(field.field) || field.label.size() > 64 ||
            !isAllowedVisualizationFormat(field.format))
        {
            *errorOut = QStringLiteral("visualization 字段名、标签或 format 不合法。");
            return false;
        }

        const QJsonValue valuesValue = object.value(QStringLiteral("values"));
        if (!valuesValue.isUndefined())
        {
            if (field.format != QStringLiteral("badge") || !valuesValue.isObject())
            {
                *errorOut = QStringLiteral("visualization.values 仅允许用于 badge，且必须是对象。");
                return false;
            }
            const QJsonObject values = valuesValue.toObject();
            if (values.size() > 32)
            {
                *errorOut = QStringLiteral("visualization.values 最多允许 32 个映射。");
                return false;
            }
            for (auto iterator = values.constBegin(); iterator != values.constEnd(); ++iterator)
            {
                if (!isValidProtocolName(iterator.key()) || !iterator.value().isObject())
                {
                    *errorOut = QStringLiteral("visualization.values 的键或值不合法。");
                    return false;
                }
                VisualizationValueStyle style;
                const QJsonObject styleObject = iterator.value().toObject();
                if (!readRequiredString(styleObject, "label", &style.label, errorOut) ||
                    !readRequiredString(styleObject, "tone", &style.tone, errorOut))
                {
                    return false;
                }
                style.tone = style.tone.toLower();
                if (style.label.size() > 64 || !isAllowedVisualizationTone(style.tone))
                {
                    *errorOut = QStringLiteral("visualization.values 的 label 或 tone 不合法。");
                    return false;
                }
                field.valueStyles.insert(iterator.key(), style);
            }
        }
        if (field.format == QStringLiteral("badge") && field.valueStyles.isEmpty())
        {
            *errorOut = QStringLiteral("badge 字段必须提供非空 values 映射。");
            return false;
        }

        *fieldOut = field;
        return true;
    }

    bool parseVisualization(
        const QJsonObject& manifest,
        PluginVisualization* visualizationOut,
        QString* errorOut)
    {
        if (visualizationOut == nullptr || errorOut == nullptr)
        {
            return false;
        }

        *visualizationOut = {};
        const QJsonValue visualizationValue = manifest.value(QStringLiteral("visualization"));
        if (visualizationValue.isUndefined())
        {
            return true;
        }
        if (!visualizationValue.isObject())
        {
            *errorOut = QStringLiteral("visualization 必须是对象。");
            return false;
        }

        const QJsonObject object = visualizationValue.toObject();
        PluginVisualization visualization;
        if (!readRequiredString(object, "type", &visualization.type, errorOut) ||
            !readRequiredString(object, "title", &visualization.title, errorOut) ||
            !readRequiredString(object, "start_event", &visualization.startEvent, errorOut) ||
            !readRequiredString(object, "result_event", &visualization.resultEvent, errorOut) ||
            !readRequiredString(object, "complete_event", &visualization.completeEvent, errorOut) ||
            !readRequiredString(object, "total_field", &visualization.totalField, errorOut))
        {
            return false;
        }
        visualization.type = visualization.type.toLower();
        if (visualization.type != QStringLiteral("scan-table") ||
            visualization.title.size() > 96 ||
            !isValidProtocolName(visualization.startEvent) ||
            !isValidProtocolName(visualization.resultEvent) ||
            !isValidProtocolName(visualization.completeEvent) ||
            !isValidProtocolName(visualization.totalField))
        {
            *errorOut = QStringLiteral("visualization 的类型、标题、事件名或 total_field 不合法。");
            return false;
        }

        const QJsonArray columns = object.value(QStringLiteral("columns")).toArray();
        if (columns.isEmpty() || columns.size() > kMaxVisualizationColumns)
        {
            *errorOut = QStringLiteral("scan-table 必须定义 1 到 %1 个 columns。").arg(kMaxVisualizationColumns);
            return false;
        }
        QStringList columnNames;
        for (const QJsonValue& columnValue : columns)
        {
            VisualizationField field;
            if (!parseVisualizationField(columnValue, &field, errorOut))
            {
                return false;
            }
            if (columnNames.contains(field.field))
            {
                *errorOut = QStringLiteral("visualization.columns 不能包含重复 field。");
                return false;
            }
            columnNames.push_back(field.field);
            visualization.columns.push_back(field);
        }

        const QJsonValue summaryValue = object.value(QStringLiteral("summary"));
        if (!summaryValue.isUndefined())
        {
            if (!summaryValue.isArray() || summaryValue.toArray().size() > kMaxVisualizationSummaryItems)
            {
                *errorOut = QStringLiteral("visualization.summary 必须是数组，且最多 %1 项。")
                    .arg(kMaxVisualizationSummaryItems);
                return false;
            }
            QStringList summaryNames;
            for (const QJsonValue& summaryFieldValue : summaryValue.toArray())
            {
                VisualizationField field;
                if (!parseVisualizationField(summaryFieldValue, &field, errorOut))
                {
                    return false;
                }
                if (summaryNames.contains(field.field))
                {
                    *errorOut = QStringLiteral("visualization.summary 不能包含重复 field。");
                    return false;
                }
                summaryNames.push_back(field.field);
                visualization.summary.push_back(field);
            }
        }

        visualization.enabled = true;
        *visualizationOut = visualization;
        return true;
    }

    bool parseTabPresentation(
        const QJsonObject& manifest,
        const QString& pluginType,
        PluginTabPresentation* presentationOut,
        QString* errorOut)
    {
        if (presentationOut == nullptr || errorOut == nullptr)
        {
            return false;
        }
        *presentationOut = {};
        const QJsonValue tabValue = manifest.value(QStringLiteral("tab"));
        if (pluginType != QStringLiteral("tab"))
        {
            if (!tabValue.isUndefined())
            {
                *errorOut = QStringLiteral("tab 配置只允许用于 plugin_type=tab 的插件。");
                return false;
            }
            return true;
        }
        if (!tabValue.isObject())
        {
            *errorOut = QStringLiteral("Tab 型插件必须提供 tab 配置对象。");
            return false;
        }

        const QJsonObject tabObject = tabValue.toObject();
        PluginTabPresentation presentation;
        if (!readRequiredString(tabObject, "title", &presentation.title, errorOut) ||
            !readRequiredString(tabObject, "ready_event", &presentation.readyEvent, errorOut))
        {
            return false;
        }
        if (presentation.title.size() > 96 || !isValidProtocolName(presentation.readyEvent))
        {
            *errorOut = QStringLiteral("tab.title 或 tab.ready_event 不合法。");
            return false;
        }
        const QJsonValue timeoutValue = tabObject.value(QStringLiteral("startup_timeout_ms"));
        if (!timeoutValue.isUndefined())
        {
            const int timeoutMs = timeoutValue.toInt(-1);
            if (!timeoutValue.isDouble() || timeoutMs < 1000 || timeoutMs > 60000)
            {
                *errorOut = QStringLiteral("tab.startup_timeout_ms 必须介于 1000 与 60000 毫秒之间。");
                return false;
            }
            presentation.startupTimeoutMs = timeoutMs;
        }
        presentation.enabled = true;
        *presentationOut = presentation;
        return true;
    }

    bool isApprovedMarketplaceUrl(const QUrl& url)
    {
        return url.isValid() && url.scheme().compare(QStringLiteral("https"), Qt::CaseInsensitive) == 0 &&
            url.host().compare(QStringLiteral("raw.githubusercontent.com"), Qt::CaseInsensitive) == 0;
    }

    QString networkReplyErrorText(QNetworkReply* reply)
    {
        const int httpStatus = reply != nullptr
            ? reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt()
            : 0;
        const QString networkError = reply != nullptr ? reply->errorString() : QStringLiteral("未知网络错误");
        return httpStatus > 0
            ? QStringLiteral("HTTP %1：%2").arg(httpStatus).arg(networkError)
            : networkError;
    }

    bool parseMarketplacePlugin(const QJsonObject& object, MarketplacePlugin* pluginOut, QString* errorOut)
    {
        if (pluginOut == nullptr || errorOut == nullptr)
        {
            return false;
        }
        MarketplacePlugin plugin;
        QString archiveUrlText;
        QString licenseUrlText;
        if (!readRequiredString(object, "id", &plugin.id, errorOut) ||
            !readRequiredString(object, "name", &plugin.name, errorOut) ||
            !readRequiredString(object, "version", &plugin.version, errorOut) ||
            !readRequiredString(object, "description", &plugin.description, errorOut) ||
            !readRequiredString(object, "install_directory", &plugin.installDirectory, errorOut) ||
            !readRequiredString(object, "archive_url", &archiveUrlText, errorOut) ||
            !readRequiredString(object, "sha256", &plugin.sha256, errorOut) ||
            !readRequiredString(object, "license_name", &plugin.licenseName, errorOut) ||
            !readRequiredString(object, "license_url", &licenseUrlText, errorOut))
        {
            return false;
        }
        if (!isValidPluginId(plugin.id) || !isValidPluginId(plugin.installDirectory))
        {
            *errorOut = QStringLiteral("商城条目的 id 或 install_directory 不合法。");
            return false;
        }
        plugin.archiveUrl = QUrl(archiveUrlText);
        plugin.licenseUrl = QUrl(licenseUrlText);
        if (!isApprovedMarketplaceUrl(plugin.archiveUrl) || !isApprovedMarketplaceUrl(plugin.licenseUrl))
        {
            *errorOut = QStringLiteral("商城仅接受 raw.githubusercontent.com 的 HTTPS 下载地址。");
            return false;
        }
        if (!QRegularExpression(QStringLiteral("^[0-9A-Fa-f]{64}$")).match(plugin.sha256).hasMatch())
        {
            *errorOut = QStringLiteral("商城条目的 sha256 必须是 64 位十六进制值。");
            return false;
        }
        const QJsonArray targetValues = object.value(QStringLiteral("targets")).toArray();
        for (const QJsonValue& value : targetValues)
        {
            const QString target = value.toString().trimmed().toLower();
            if ((target == QStringLiteral("file") || target == QStringLiteral("process") ||
                target == QStringLiteral("network") || target == QStringLiteral("tab")) &&
                !plugin.targets.contains(target))
            {
                plugin.targets.push_back(target);
            }
        }
        if (plugin.targets.isEmpty())
        {
            *errorOut = QStringLiteral("商城条目的 targets 必须包含 file、process、network 和/或 tab。");
            return false;
        }
        *pluginOut = plugin;
        return true;
    }

    bool loadPluginManifest(
        const QString& pluginRoot,
        const QString& pluginId,
        PluginDescriptor* descriptorOut,
        QString* errorOut)
    {
        if (descriptorOut == nullptr || errorOut == nullptr || !isValidPluginId(pluginId))
        {
            if (errorOut != nullptr)
            {
                *errorOut = QStringLiteral("插件 ID 只能包含小写字母、数字和连字符。");
            }
            return false;
        }

        const QString pluginDirectory = QDir(pluginRoot).filePath(pluginId);
        const QFileInfo manifestInfo(QDir(pluginDirectory).filePath(QStringLiteral("plugin.json")));
        if (!manifestInfo.isFile() || manifestInfo.size() > kMaxManifestBytes)
        {
            *errorOut = QStringLiteral("缺少 plugin.json，或其大小超过 64 KiB。");
            return false;
        }

        QFile manifestFile(manifestInfo.absoluteFilePath());
        if (!manifestFile.open(QIODevice::ReadOnly))
        {
            *errorOut = QStringLiteral("无法读取 plugin.json：%1").arg(manifestFile.errorString());
            return false;
        }
        QJsonParseError parseError;
        const QJsonDocument document = QJsonDocument::fromJson(manifestFile.readAll(), &parseError);
        if (parseError.error != QJsonParseError::NoError || !document.isObject())
        {
            *errorOut = QStringLiteral("plugin.json 不是有效 JSON：%1").arg(parseError.errorString());
            return false;
        }

        const QJsonObject object = document.object();
        if (object.value(QStringLiteral("ksword_plugin_api")).toString() != QStringLiteral("1"))
        {
            *errorOut = QStringLiteral("不支持的 ksword_plugin_api；当前仅支持 \"1\"。");
            return false;
        }

        PluginDescriptor descriptor;
        QString entrypoint;
        if (!readRequiredString(object, "id", &descriptor.id, errorOut) ||
            !readRequiredString(object, "name", &descriptor.name, errorOut) ||
            !readRequiredString(object, "version", &descriptor.version, errorOut) ||
            !readRequiredString(object, "description", &descriptor.description, errorOut) ||
            !readRequiredString(object, "runtime", &descriptor.runtime, errorOut) ||
            !readRequiredString(object, "entrypoint", &entrypoint, errorOut) ||
            !readRequiredString(object, "default_command", &descriptor.defaultCommand, errorOut))
        {
            return false;
        }
        descriptor.pluginType = object.value(QStringLiteral("plugin_type")).toString(QStringLiteral("command")).trimmed().toLower();
        if (descriptor.pluginType != QStringLiteral("command") && descriptor.pluginType != QStringLiteral("tab"))
        {
            *errorOut = QStringLiteral("plugin_type 只能是 command 或 tab。");
            return false;
        }
        if (descriptor.id != pluginId || !isValidPluginId(descriptor.id))
        {
            *errorOut = QStringLiteral("清单 id 必须与插件目录名完全一致。");
            return false;
        }
        if (descriptor.runtime != QStringLiteral("python") && descriptor.runtime != QStringLiteral("executable"))
        {
            *errorOut = QStringLiteral("runtime 只能是 python 或 executable。");
            return false;
        }
        if (!isSafeRelativePath(entrypoint) || !isSafeCommandToken(descriptor.defaultCommand))
        {
            *errorOut = QStringLiteral("entrypoint 或 default_command 含有不安全路径/令牌。");
            return false;
        }

        const QJsonArray targets = object.value(QStringLiteral("targets")).toArray();
        for (const QJsonValue& target : targets)
        {
            const QString targetText = target.toString().trimmed().toLower();
            const bool allowedTarget = descriptor.pluginType == QStringLiteral("tab")
                ? targetText == QStringLiteral("tab")
                : (targetText == QStringLiteral("file") || targetText == QStringLiteral("process") ||
                    targetText == QStringLiteral("network"));
            if (!allowedTarget)
            {
                *errorOut = descriptor.pluginType == QStringLiteral("tab")
                    ? QStringLiteral("Tab 型插件的 targets 只能包含 tab。")
                    : QStringLiteral("命令型插件的 targets 只能包含 file、process 和/或 network。");
                return false;
            }
            if (!descriptor.targets.contains(targetText))
            {
                descriptor.targets.push_back(targetText);
            }
        }
        if (descriptor.targets.isEmpty())
        {
            *errorOut = descriptor.pluginType == QStringLiteral("tab")
                ? QStringLiteral("Tab 型插件的 targets 必须包含 tab。")
                : QStringLiteral("targets 必须包含 file、process 和/或 network。");
            return false;
        }
        if (!parseVisualization(object, &descriptor.visualization, errorOut))
        {
            return false;
        }
        if (!parseTabPresentation(object, descriptor.pluginType, &descriptor.tabPresentation, errorOut))
        {
            return false;
        }
        if (descriptor.pluginType == QStringLiteral("tab") && descriptor.visualization.enabled)
        {
            *errorOut = QStringLiteral("Tab 型插件不能同时声明 scan-table visualization。");
            return false;
        }

        descriptor.pluginDirectory = QDir(pluginDirectory).absolutePath();
        descriptor.entrypointPath = QDir(descriptor.pluginDirectory).filePath(entrypoint);
        if (!QFileInfo(descriptor.entrypointPath).isFile())
        {
            *errorOut = QStringLiteral("入口文件不存在：%1").arg(entrypoint);
            return false;
        }
        *descriptorOut = descriptor;
        return true;
    }

    bool discoverPlugins(PluginListResult* resultOut, QString* errorOut)
    {
        if (resultOut == nullptr || errorOut == nullptr)
        {
            return false;
        }
        *resultOut = {};
        *errorOut = {};
        const QString pluginRoot = findPluginRoot();
        if (pluginRoot.isEmpty())
        {
            *errorOut = QStringLiteral("找不到 plugin 目录。请在程序目录部署 plugin\\，或设置 KSWORD_PLUGIN_ROOT。");
            return false;
        }

        const QDir rootDirectory(pluginRoot);
        const QStringList directoryNames = rootDirectory.entryList(QDir::Dirs | QDir::NoDotAndDotDot, QDir::Name);
        for (const QString& pluginId : directoryNames)
        {
            if (!isValidPluginId(pluginId))
            {
                continue;
            }
            PluginDescriptor descriptor;
            QString manifestError;
            if (loadPluginManifest(pluginRoot, pluginId, &descriptor, &manifestError))
            {
                resultOut->plugins.push_back(descriptor);
            }
            else if (QFileInfo(rootDirectory.filePath(pluginId + QStringLiteral("/plugin.json"))).isFile())
            {
                resultOut->ignoredManifests.push_back(QStringLiteral("%1：%2").arg(pluginId, manifestError));
            }
        }
        resultOut->pluginRoot = pluginRoot;
        return true;
    }

    QString targetName(const ks::plugin_host::TargetKind targetKind)
    {
        switch (targetKind)
        {
        case ks::plugin_host::TargetKind::File: return QStringLiteral("file");
        case ks::plugin_host::TargetKind::Process: return QStringLiteral("process");
        case ks::plugin_host::TargetKind::Network: return QStringLiteral("network");
        }
        return QString();
    }

    bool isUsableContext(const ks::plugin_host::InvocationContext& context, QString* errorOut)
    {
        if (context.targetKind == ks::plugin_host::TargetKind::File)
        {
            if (QFileInfo(context.filePath).isFile())
            {
                return true;
            }
            *errorOut = QStringLiteral("插件入口仅支持单个常规文件。");
            return false;
        }
        if (context.targetKind == ks::plugin_host::TargetKind::Network)
        {
            return true;
        }
        if (context.processId != 0)
        {
            return true;
        }
        *errorOut = QStringLiteral("当前进程没有有效 PID，不能交给插件。");
        return false;
    }

    bool buildPluginCommand(
        const PluginDescriptor& descriptor,
        const ks::plugin_host::InvocationContext& context,
        QString* programOut,
        QStringList* argumentsOut,
        QString* errorOut)
    {
        if (programOut == nullptr || argumentsOut == nullptr || errorOut == nullptr)
        {
            return false;
        }
        QStringList arguments;
        if (descriptor.runtime == QStringLiteral("python"))
        {
            QString python = qEnvironmentVariable("KSWORD_PLUGIN_PYTHON").trimmed();
            bool usePythonLauncher = false;
            if (!python.isEmpty() && !QFileInfo(python).isFile())
            {
                *errorOut = QStringLiteral("KSWORD_PLUGIN_PYTHON 未指向有效文件：%1").arg(python);
                return false;
            }
            if (python.isEmpty())
            {
                python = QStandardPaths::findExecutable(QStringLiteral("python.exe"));
            }
            if (python.isEmpty())
            {
                python = QStandardPaths::findExecutable(QStringLiteral("py.exe"));
                usePythonLauncher = !python.isEmpty();
            }
            if (python.isEmpty())
            {
                *errorOut = QStringLiteral("找不到 Python 运行时；请安装 python.exe/py.exe 或设置 KSWORD_PLUGIN_PYTHON。");
                return false;
            }
            *programOut = python;
            if (usePythonLauncher)
            {
                arguments << QStringLiteral("-3");
            }
            arguments << descriptor.entrypointPath;
        }
        else
        {
            *programOut = descriptor.entrypointPath;
        }

        arguments << QStringLiteral("--ksword-plugin") << descriptor.defaultCommand << QStringLiteral("--")
                  << QStringLiteral("--target-kind") << targetName(context.targetKind);
        if (context.targetKind == ks::plugin_host::TargetKind::File)
        {
            arguments << QStringLiteral("--path") << context.filePath;
        }
        else if (context.targetKind == ks::plugin_host::TargetKind::Process)
        {
            arguments << QStringLiteral("--pid") << QString::number(context.processId);
            if (!context.filePath.trimmed().isEmpty())
            {
                arguments << QStringLiteral("--path") << context.filePath;
            }
            if (!context.processName.trimmed().isEmpty())
            {
                arguments << QStringLiteral("--process-name") << context.processName;
            }
        }
        *argumentsOut = arguments;
        return true;
    }

    bool buildTabPluginCommand(
        const PluginDescriptor& descriptor,
        const WId parentWindowId,
        QString* programOut,
        QStringList* argumentsOut,
        QString* errorOut)
    {
        if (programOut == nullptr || argumentsOut == nullptr || errorOut == nullptr ||
            descriptor.pluginType != QStringLiteral("tab") || !descriptor.tabPresentation.enabled)
        {
            return false;
        }
        QStringList arguments;
        if (descriptor.runtime == QStringLiteral("python"))
        {
            QString python = qEnvironmentVariable("KSWORD_PLUGIN_PYTHON").trimmed();
            bool usePythonLauncher = false;
            if (!python.isEmpty() && !QFileInfo(python).isFile())
            {
                *errorOut = QStringLiteral("KSWORD_PLUGIN_PYTHON 未指向有效文件：%1").arg(python);
                return false;
            }
            if (python.isEmpty()) python = QStandardPaths::findExecutable(QStringLiteral("python.exe"));
            if (python.isEmpty())
            {
                python = QStandardPaths::findExecutable(QStringLiteral("py.exe"));
                usePythonLauncher = !python.isEmpty();
            }
            if (python.isEmpty())
            {
                *errorOut = QStringLiteral("找不到 Python 运行时；请安装 python.exe/py.exe 或设置 KSWORD_PLUGIN_PYTHON。");
                return false;
            }
            *programOut = python;
            if (usePythonLauncher) arguments << QStringLiteral("-3");
            arguments << descriptor.entrypointPath;
        }
        else
        {
            *programOut = descriptor.entrypointPath;
        }

        arguments << QStringLiteral("--ksword-plugin") << descriptor.defaultCommand << QStringLiteral("--")
            << QStringLiteral("--parent-hwnd") << QString::number(static_cast<qulonglong>(parentWindowId))
            << QStringLiteral("--host-pid") << QString::number(QCoreApplication::applicationPid());
        *argumentsOut = arguments;
        return true;
    }

    QString visualizationValueText(const QJsonValue& value, const QString& format)
    {
        if (value.isUndefined() || value.isNull())
        {
            return QStringLiteral("—");
        }
        if (format == QStringLiteral("percent") && value.isDouble())
        {
            double percent = value.toDouble();
            if (qAbs(percent) <= 1.0) percent *= 100.0;
            if (percent > 0.0 && percent < 0.01) return QStringLiteral("<0.01%");
            return QStringLiteral("%1%").arg(percent, 0, 'f', 2);
        }
        if (format == QStringLiteral("integer") && value.isDouble())
        {
            return QString::number(qRound64(value.toDouble()));
        }
        if (format == QStringLiteral("number") && value.isDouble())
        {
            return QString::number(value.toDouble(), 'g', 8);
        }
        if (value.isString())
        {
            return value.toString();
        }
        if (value.isDouble())
        {
            return QString::number(value.toDouble(), 'g', 12);
        }
        if (value.isBool())
        {
            return value.toBool() ? QStringLiteral("是") : QStringLiteral("否");
        }
        if (value.isArray())
        {
            return QString::fromUtf8(QJsonDocument(value.toArray()).toJson(QJsonDocument::Compact));
        }
        if (value.isObject())
        {
            return QString::fromUtf8(QJsonDocument(value.toObject()).toJson(QJsonDocument::Compact));
        }
        return QStringLiteral("—");
    }

    QColor visualizationToneColor(const QString& tone)
    {
        if (tone == QStringLiteral("success")) return KswordTheme::SuccessColor();
        if (tone == QStringLiteral("danger")) return KswordTheme::ErrorColor();
        if (tone == QStringLiteral("warning")) return KswordTheme::WarningColor();
        if (tone == QStringLiteral("info")) return KswordTheme::InfoColor();
        if (tone == QStringLiteral("muted")) return KswordTheme::TextSecondaryColor();
        return KswordTheme::TextPrimaryColor();
    }

    class PluginRunDialog final : public QDialog
    {
    public:
        PluginRunDialog(
            QWidget* parent,
            const PluginDescriptor& descriptor,
            const ks::plugin_host::InvocationContext& context,
            const QString& program,
            const QStringList& arguments)
            : QDialog(parent),
              m_descriptor(descriptor)
        {
            setAttribute(Qt::WA_DeleteOnClose, true);
            setWindowTitle(descriptor.visualization.enabled
                ? descriptor.visualization.title
                : descriptor.name);
            setWindowIcon(QIcon(QStringLiteral(":/Icon/process_start.svg")));
            resize(920, 620);
            setModal(false);

            auto* rootLayout = new QVBoxLayout(this);
            rootLayout->setContentsMargins(16, 16, 16, 16);
            rootLayout->setSpacing(10);

            auto* titleLabel = new QLabel(
                descriptor.visualization.enabled ? descriptor.visualization.title : descriptor.name,
                this);
            titleLabel->setStyleSheet(QStringLiteral("font-size:18px;font-weight:700;color:%1;")
                .arg(KswordTheme::TextPrimaryHex()));
            rootLayout->addWidget(titleLabel);

            QString targetText;
            if (context.targetKind == ks::plugin_host::TargetKind::File)
            {
                targetText = QStringLiteral("目标文件：%1").arg(QDir::toNativeSeparators(context.filePath));
            }
            else if (context.targetKind == ks::plugin_host::TargetKind::Process)
            {
                targetText = QStringLiteral("目标进程：%1  PID %2")
                    .arg(context.processName.trimmed().isEmpty() ? QStringLiteral("未知进程") : context.processName)
                    .arg(context.processId);
                if (!context.filePath.trimmed().isEmpty())
                {
                    targetText += QStringLiteral("\n映像路径：%1").arg(QDir::toNativeSeparators(context.filePath));
                }
            }
            else
            {
                targetText = QStringLiteral("目标：实时网络流量");
            }
            auto* targetLabel = new QLabel(targetText, this);
            targetLabel->setWordWrap(true);
            targetLabel->setTextInteractionFlags(Qt::TextSelectableByMouse);
            targetLabel->setStyleSheet(QStringLiteral("color:%1;").arg(KswordTheme::TextSecondaryHex()));
            rootLayout->addWidget(targetLabel);

            m_progress = new QProgressBar(this);
            m_progress->setRange(0, 0);
            m_progress->setTextVisible(true);
            rootLayout->addWidget(m_progress);

            if (!descriptor.visualization.summary.isEmpty())
            {
                auto* summaryLayout = new QHBoxLayout();
                summaryLayout->setSpacing(18);
                for (const VisualizationField& field : descriptor.visualization.summary)
                {
                    auto* label = new QLabel(QStringLiteral("%1：—").arg(field.label), this);
                    label->setStyleSheet(QStringLiteral("font-weight:600;color:%1;")
                        .arg(KswordTheme::TextPrimaryHex()));
                    summaryLayout->addWidget(label);
                    m_summaryLabels.insert(field.field, label);
                }
                summaryLayout->addStretch(1);
                rootLayout->addLayout(summaryLayout);
            }

            m_tabs = new QTabWidget(this);
            if (descriptor.visualization.enabled)
            {
                m_resultTable = new ks::ui::VisibleTableWidget(m_tabs);
                m_resultTable->setColumnCount(descriptor.visualization.columns.size());
                QStringList labels;
                for (const VisualizationField& field : descriptor.visualization.columns) labels.push_back(field.label);
                m_resultTable->setHorizontalHeaderLabels(labels);
                m_resultTable->setEditTriggers(QAbstractItemView::NoEditTriggers);
                m_resultTable->setSelectionBehavior(QAbstractItemView::SelectRows);
                m_resultTable->setSelectionMode(QAbstractItemView::SingleSelection);
                m_resultTable->setAlternatingRowColors(true);
                m_resultTable->setWordWrap(false);
                m_resultTable->setSortingEnabled(false);
                m_resultTable->verticalHeader()->setVisible(false);
                QHeaderView* header = m_resultTable->horizontalHeader();
                header->setStretchLastSection(false);
                for (int column = 0; column < descriptor.visualization.columns.size(); ++column)
                {
                    const QString format = descriptor.visualization.columns.at(column).format;
                    if (format == QStringLiteral("path"))
                    {
                        header->setSectionResizeMode(column, QHeaderView::Stretch);
                    }
                    else if (format == QStringLiteral("text"))
                    {
                        header->setSectionResizeMode(column, QHeaderView::Interactive);
                        m_resultTable->setColumnWidth(column, 180);
                    }
                    else
                    {
                        header->setSectionResizeMode(column, QHeaderView::ResizeToContents);
                    }
                }
                m_tabs->addTab(m_resultTable, QStringLiteral("扫描结果"));
            }
            else
            {
                m_plainOutput = new QPlainTextEdit(m_tabs);
                m_plainOutput->setReadOnly(true);
                m_plainOutput->setMaximumBlockCount(2000);
                m_tabs->addTab(m_plainOutput, QStringLiteral("插件输出"));
            }

            m_diagnostics = new QPlainTextEdit(m_tabs);
            m_diagnostics->setReadOnly(true);
            m_diagnostics->setMaximumBlockCount(2000);
            m_diagnostics->setPlaceholderText(QStringLiteral("插件错误和协议诊断会显示在这里。"));
            m_tabs->addTab(m_diagnostics, QStringLiteral("诊断"));
            rootLayout->addWidget(m_tabs, 1);

            auto* footer = new QHBoxLayout();
            m_status = new QLabel(QStringLiteral("正在启动…"), this);
            m_status->setWordWrap(true);
            footer->addWidget(m_status, 1);
            m_closeButton = new QPushButton(QStringLiteral("取消"), this);
            footer->addWidget(m_closeButton);
            rootLayout->addLayout(footer);

            m_process = new QProcess(this);
            m_process->setProgram(program);
            m_process->setArguments(arguments);
            m_process->setWorkingDirectory(descriptor.pluginDirectory);
            m_process->setProcessChannelMode(QProcess::SeparateChannels);
            QProcessEnvironment environment = QProcessEnvironment::systemEnvironment();
            environment.insert(QStringLiteral("KSWORD_PLUGIN_ROOT"), findPluginRoot());
            m_process->setProcessEnvironment(environment);

            connect(m_closeButton, &QPushButton::clicked, this, [this]() {
                if (m_process->state() == QProcess::NotRunning) close();
                else cancelRun();
            });
            connect(m_process, &QProcess::started, this, [this]() {
                setStatus(QStringLiteral("正在扫描…"), QStringLiteral("info"));
            });
            connect(m_process, &QProcess::readyReadStandardOutput, this, [this]() {
                consumeStandardOutput(false);
            });
            connect(m_process, &QProcess::readyReadStandardError, this, [this]() {
                consumeStandardError();
            });
            connect(m_process, &QProcess::errorOccurred, this, [this](const QProcess::ProcessError error) {
                if (error != QProcess::FailedToStart || m_finished) return;
                m_finished = true;
                m_progress->setRange(0, 1);
                m_progress->setValue(0);
                appendDiagnostic(QStringLiteral("无法启动插件入口：%1").arg(m_process->errorString()));
                setStatus(QStringLiteral("启动失败"), QStringLiteral("danger"));
                m_closeButton->setText(QStringLiteral("关闭"));
                m_closeButton->setEnabled(true);
                if (m_tabs->count() > 1) m_tabs->setCurrentIndex(1);
            });
            connect(m_process, qOverload<int, QProcess::ExitStatus>(&QProcess::finished), this,
                [this](const int exitCode, const QProcess::ExitStatus exitStatus) {
                    if (m_finished) return;
                    m_finished = true;
                    consumeStandardOutput(true);
                    consumeStandardError();
                    const bool protocolComplete = !m_descriptor.visualization.enabled || m_completeSeen;
                    const bool succeeded = exitStatus == QProcess::NormalExit && exitCode == 0 &&
                        !m_protocolError && protocolComplete && !m_cancelRequested;
                    if (m_cancelRequested)
                    {
                        setStatus(QStringLiteral("扫描已取消"), QStringLiteral("warning"));
                    }
                    else if (succeeded)
                    {
                        if (m_totalItems > 0)
                        {
                            m_progress->setRange(0, m_totalItems);
                            m_progress->setValue(qMin(m_completedItems, m_totalItems));
                        }
                        else
                        {
                            m_progress->setRange(0, 1);
                            m_progress->setValue(1);
                        }
                        setStatus(QStringLiteral("扫描完成，共处理 %1 项").arg(m_completedItems),
                            QStringLiteral("success"));
                    }
                    else
                    {
                        if (!protocolComplete)
                        {
                            appendDiagnostic(QStringLiteral("插件未发送声明的完成事件：%1")
                                .arg(m_descriptor.visualization.completeEvent));
                        }
                        setStatus(QStringLiteral("扫描失败（退出码 %1）").arg(exitCode),
                            QStringLiteral("danger"));
                        if (m_tabs->count() > 1) m_tabs->setCurrentIndex(1);
                    }
                    m_closeButton->setText(QStringLiteral("关闭"));
                    m_closeButton->setEnabled(true);
                    if (m_resultTable != nullptr) m_resultTable->setSortingEnabled(true);
                });
        }

        void start()
        {
            m_process->start();
        }

    protected:
        void closeEvent(QCloseEvent* event) override
        {
            if (m_process != nullptr && m_process->state() != QProcess::NotRunning)
            {
                cancelRun();
                event->ignore();
                return;
            }
            QDialog::closeEvent(event);
        }

    private:
        void cancelRun()
        {
            if (m_process == nullptr || m_process->state() == QProcess::NotRunning) return;
            m_cancelRequested = true;
            setStatus(QStringLiteral("正在取消扫描…"), QStringLiteral("warning"));
            m_closeButton->setEnabled(false);
            m_process->terminate();
            QTimer::singleShot(2000, m_process, [process = m_process]() {
                if (process->state() != QProcess::NotRunning) process->kill();
            });
        }

        void setStatus(const QString& text, const QString& tone)
        {
            m_status->setText(text);
            m_status->setStyleSheet(QStringLiteral("font-weight:600;color:%1;")
                .arg(visualizationToneColor(tone).name()));
        }

        void appendDiagnostic(const QString& text)
        {
            if (!text.trimmed().isEmpty()) m_diagnostics->appendPlainText(text.trimmed());
        }

        void consumeStandardError()
        {
            const QString text = QString::fromLocal8Bit(m_process->readAllStandardError());
            appendDiagnostic(text);
        }

        void consumeStandardOutput(const bool flushRemainder)
        {
            m_stdoutBuffer += m_process->readAllStandardOutput();
            int newlineIndex = -1;
            while ((newlineIndex = m_stdoutBuffer.indexOf('\n')) >= 0)
            {
                QByteArray line = m_stdoutBuffer.left(newlineIndex);
                m_stdoutBuffer.remove(0, newlineIndex + 1);
                if (line.endsWith('\r')) line.chop(1);
                processOutputLine(line);
            }
            if (m_stdoutBuffer.size() > kMaxBufferedStdoutBytes)
            {
                appendDiagnostic(QStringLiteral("插件输出存在超过 1 MiB 的无换行记录，已拒绝解析。"));
                m_stdoutBuffer.clear();
                m_protocolError = true;
            }
            if (flushRemainder && !m_stdoutBuffer.isEmpty())
            {
                processOutputLine(m_stdoutBuffer);
                m_stdoutBuffer.clear();
            }
        }

        void processOutputLine(const QByteArray& lineBytes)
        {
            const QString line = QString::fromUtf8(lineBytes).trimmed();
            if (line.isEmpty()) return;
            if (!m_descriptor.visualization.enabled)
            {
                m_plainOutput->appendPlainText(line);
                return;
            }

            QJsonParseError parseError;
            const QJsonDocument document = QJsonDocument::fromJson(lineBytes, &parseError);
            if (parseError.error != QJsonParseError::NoError || !document.isObject())
            {
                appendDiagnostic(QStringLiteral("无法解析插件 JSON Lines：%1\n%2")
                    .arg(parseError.errorString(), line));
                m_protocolError = true;
                return;
            }
            const QJsonObject object = document.object();
            if (object.value(QStringLiteral("protocol")).toString() != QStringLiteral("ksword-plugin/1") ||
                object.value(QStringLiteral("plugin_id")).toString() != m_descriptor.id)
            {
                appendDiagnostic(QStringLiteral("插件输出的 protocol 或 plugin_id 与清单不一致。"));
                m_protocolError = true;
                return;
            }

            const QString event = object.value(QStringLiteral("event")).toString();
            if (event == QStringLiteral("error"))
            {
                m_protocolError = true;
                appendDiagnostic(QStringLiteral("%1：%2")
                    .arg(object.value(QStringLiteral("code")).toString(QStringLiteral("plugin_error")),
                         object.value(QStringLiteral("message")).toString(QStringLiteral("插件报告错误"))));
                setStatus(QStringLiteral("插件报告错误"), QStringLiteral("danger"));
                return;
            }
            if (event == m_descriptor.visualization.startEvent)
            {
                m_totalItems = qMax(0, object.value(m_descriptor.visualization.totalField).toInt());
                if (m_totalItems > 0)
                {
                    m_progress->setRange(0, m_totalItems);
                    m_progress->setValue(0);
                    m_progress->setFormat(QStringLiteral("%v / %m"));
                }
                setStatus(m_totalItems > 0
                    ? QStringLiteral("正在扫描 0 / %1").arg(m_totalItems)
                    : QStringLiteral("正在扫描…"), QStringLiteral("info"));
                return;
            }
            if (event == m_descriptor.visualization.resultEvent)
            {
                appendResult(object);
                ++m_completedItems;
                if (m_totalItems > 0)
                {
                    m_progress->setValue(qMin(m_completedItems, m_totalItems));
                    setStatus(QStringLiteral("正在扫描 %1 / %2").arg(m_completedItems).arg(m_totalItems),
                        QStringLiteral("info"));
                }
                else
                {
                    setStatus(QStringLiteral("已处理 %1 项").arg(m_completedItems), QStringLiteral("info"));
                }
                return;
            }
            if (event == m_descriptor.visualization.completeEvent)
            {
                m_completeSeen = true;
                updateSummary(object);
                if (m_totalItems > 0) m_progress->setValue(qMin(m_completedItems, m_totalItems));
                setStatus(QStringLiteral("正在完成扫描…"), QStringLiteral("info"));
            }
        }

        void appendResult(const QJsonObject& object)
        {
            if (m_resultTable == nullptr) return;
            if (m_resultTable->rowCount() >= kMaxVisualizationRows)
            {
                if (!m_rowLimitReported)
                {
                    appendDiagnostic(QStringLiteral("扫描结果超过 %1 行，后续结果不再显示。")
                        .arg(kMaxVisualizationRows));
                    m_rowLimitReported = true;
                }
                return;
            }

            const int row = m_resultTable->rowCount();
            m_resultTable->insertRow(row);
            for (int column = 0; column < m_descriptor.visualization.columns.size(); ++column)
            {
                const VisualizationField& field = m_descriptor.visualization.columns.at(column);
                const QJsonValue value = object.value(field.field);
                QString text = visualizationValueText(value, field.format);
                QString tone;
                if (field.format == QStringLiteral("badge"))
                {
                    const VisualizationValueStyle style = field.valueStyles.value(value.toString());
                    if (!style.label.isEmpty()) text = style.label;
                    tone = style.tone;
                }
                auto* item = new QTableWidgetItem(text);
                if (!tone.isEmpty()) item->setForeground(QBrush(visualizationToneColor(tone)));
                if (field.format == QStringLiteral("path") || text.size() > 80) item->setToolTip(text);
                if (field.format == QStringLiteral("integer") ||
                    field.format == QStringLiteral("number") ||
                    field.format == QStringLiteral("percent"))
                {
                    item->setTextAlignment(Qt::AlignRight | Qt::AlignVCenter);
                }
                m_resultTable->setItem(row, column, item);
            }
        }

        void updateSummary(const QJsonObject& object)
        {
            for (const VisualizationField& field : m_descriptor.visualization.summary)
            {
                QLabel* label = m_summaryLabels.value(field.field, nullptr);
                if (label == nullptr) continue;
                const QJsonValue value = object.value(field.field);
                QString text = visualizationValueText(value, field.format);
                QString tone;
                if (field.format == QStringLiteral("badge"))
                {
                    const VisualizationValueStyle style = field.valueStyles.value(value.toString());
                    if (!style.label.isEmpty()) text = style.label;
                    tone = style.tone;
                }
                label->setText(QStringLiteral("%1：%2").arg(field.label, text));
                label->setStyleSheet(QStringLiteral("font-weight:600;color:%1;")
                    .arg(tone.isEmpty()
                        ? QColor(KswordTheme::TextPrimaryHex()).name()
                        : visualizationToneColor(tone).name()));
            }
        }

        PluginDescriptor m_descriptor;
        QProcess* m_process = nullptr;
        QProgressBar* m_progress = nullptr;
        QTabWidget* m_tabs = nullptr;
        QTableWidget* m_resultTable = nullptr;
        QPlainTextEdit* m_plainOutput = nullptr;
        QPlainTextEdit* m_diagnostics = nullptr;
        QLabel* m_status = nullptr;
        QPushButton* m_closeButton = nullptr;
        QHash<QString, QLabel*> m_summaryLabels;
        QByteArray m_stdoutBuffer;
        int m_totalItems = 0;
        int m_completedItems = 0;
        bool m_completeSeen = false;
        bool m_protocolError = false;
        bool m_cancelRequested = false;
        bool m_finished = false;
        bool m_rowLimitReported = false;
    };

    void launchPlugin(QWidget* owner, const PluginDescriptor& descriptor, const ks::plugin_host::InvocationContext& context)
    {
        QString contextError;
        if (!isUsableContext(context, &contextError))
        {
            QMessageBox::warning(owner, QStringLiteral("插件"), contextError);
            return;
        }

        QString program;
        QStringList arguments;
        QString commandError;
        if (!buildPluginCommand(descriptor, context, &program, &arguments, &commandError))
        {
            QMessageBox::warning(owner, QStringLiteral("插件：%1").arg(descriptor.name), commandError);
            return;
        }

        auto* dialog = new PluginRunDialog(owner, descriptor, context, program, arguments);
        dialog->show();
        dialog->raise();
        dialog->activateWindow();
        dialog->start();
    }

    class PluginTabPage final : public QWidget
    {
    public:
        PluginTabPage(QWidget* parent, const PluginDescriptor& descriptor)
            : QWidget(parent), m_descriptor(descriptor)
        {
            setObjectName(QStringLiteral("ksExternalPluginTab_%1").arg(descriptor.id));
            auto* rootLayout = new QVBoxLayout(this);
            rootLayout->setContentsMargins(4, 4, 4, 4);
            rootLayout->setSpacing(4);

            auto* statusRow = new QHBoxLayout();
            m_statusLabel = new QLabel(QStringLiteral("正在启动外部 Tab 插件…"), this);
            m_statusLabel->setWordWrap(true);
            statusRow->addWidget(m_statusLabel, 1);
            m_diagnosticsButton = new QPushButton(QStringLiteral("诊断"), this);
            m_retryButton = new QPushButton(QStringLiteral("重试"), this);
            m_retryButton->setVisible(false);
            statusRow->addWidget(m_diagnosticsButton);
            statusRow->addWidget(m_retryButton);
            rootLayout->addLayout(statusRow);

            m_surface = new QWidget(this);
            m_surface->setObjectName(QStringLiteral("ksExternalPluginNativeSurface"));
            m_surface->setAttribute(Qt::WA_NativeWindow, true);
            m_surface->setFocusPolicy(Qt::StrongFocus);
            m_surface->setStyleSheet(QStringLiteral("background:%1;border:1px solid %2;")
                .arg(KswordTheme::SurfaceHex(), KswordTheme::BorderHex()));
            m_surface->installEventFilter(this);
            rootLayout->addWidget(m_surface, 1);

            m_diagnostics = new QPlainTextEdit(this);
            m_diagnostics->setReadOnly(true);
            m_diagnostics->setMaximumHeight(180);
            m_diagnostics->document()->setMaximumBlockCount(2000);
            m_diagnostics->setVisible(false);
            rootLayout->addWidget(m_diagnostics);

            connect(m_diagnosticsButton, &QPushButton::clicked, this, [this]() {
                m_diagnostics->setVisible(!m_diagnostics->isVisible());
            });
            connect(m_retryButton, &QPushButton::clicked, this, [this]() { start(); });
        }

        ~PluginTabPage() override
        {
            m_stopping = true;
            if (::IsWindow(m_pluginWindow))
            {
                ::PostMessageW(m_pluginWindow, WM_CLOSE, 0, 0);
            }
            if (m_process != nullptr && m_process->state() != QProcess::NotRunning)
            {
                m_process->terminate();
                if (!m_process->waitForFinished(1200))
                {
                    m_process->kill();
                    m_process->waitForFinished(800);
                }
            }
        }

    protected:
        void showEvent(QShowEvent* event) override
        {
            QWidget::showEvent(event);
            if (!m_startScheduled)
            {
                m_startScheduled = true;
                QTimer::singleShot(0, this, [this]() { start(); });
            }
        }

        bool eventFilter(QObject* watched, QEvent* event) override
        {
            if (watched == m_surface &&
                (event->type() == QEvent::Resize || event->type() == QEvent::Show))
            {
                resizePluginWindow();
            }
            if (watched == m_surface && event->type() == QEvent::MouseButtonPress && ::IsWindow(m_pluginWindow))
            {
                ::SetFocus(m_pluginWindow);
            }
            return QWidget::eventFilter(watched, event);
        }

    private:
        void start()
        {
            if (m_process != nullptr && m_process->state() != QProcess::NotRunning)
            {
                return;
            }
            m_failed = false;
            m_ready = false;
            m_pluginWindow = nullptr;
            m_stdoutBuffer.clear();
            m_retryButton->setVisible(false);
            m_statusLabel->setText(QStringLiteral("正在启动外部 Tab 插件…"));
            m_statusLabel->setStyleSheet({});

            QString program;
            QStringList arguments;
            QString errorText;
            const WId surfaceId = m_surface->winId();
            if (!buildTabPluginCommand(m_descriptor, surfaceId, &program, &arguments, &errorText))
            {
                fail(QStringLiteral("Tab 插件启动失败：%1").arg(errorText), false);
                return;
            }

            if (m_process != nullptr)
            {
                m_process->deleteLater();
            }
            m_process = new QProcess(this);
            m_process->setProgram(program);
            m_process->setArguments(arguments);
            m_process->setWorkingDirectory(m_descriptor.pluginDirectory);
            m_process->setProcessChannelMode(QProcess::SeparateChannels);
            QProcessEnvironment environment = QProcessEnvironment::systemEnvironment();
            environment.insert(QStringLiteral("KSWORD_PLUGIN_ROOT"), findPluginRoot());
            environment.insert(QStringLiteral("KSWORD_PLUGIN_ID"), m_descriptor.id);
            // 基础样式注入协议：
            // - 外部进程不会继承 Qt palette/QSS，因此以环境变量提供稳定的主题角色；
            // - 插件可以逐步选择消费这些值，不会因为未实现样式协议而无法启动；
            // - 当前为启动时快照，主题切换后重新打开/重试插件即可获得新值。
            environment.insert(QStringLiteral("KSWORD_PLUGIN_STYLE_API"), QStringLiteral("1"));
            environment.insert(
                QStringLiteral("KSWORD_PLUGIN_THEME"),
                KswordTheme::IsDarkModeEnabled() ? QStringLiteral("dark") : QStringLiteral("light"));
            environment.insert(QStringLiteral("KSWORD_PLUGIN_COLOR_WINDOW"), KswordTheme::WindowColorHex());
            environment.insert(QStringLiteral("KSWORD_PLUGIN_COLOR_SURFACE"), KswordTheme::SurfaceColorHex());
            environment.insert(QStringLiteral("KSWORD_PLUGIN_COLOR_SURFACE_ALT"), KswordTheme::SurfaceAltColorHex());
            environment.insert(QStringLiteral("KSWORD_PLUGIN_COLOR_TEXT_PRIMARY"), KswordTheme::TextPrimaryColorHex());
            environment.insert(QStringLiteral("KSWORD_PLUGIN_COLOR_TEXT_SECONDARY"), KswordTheme::TextSecondaryColorHex());
            environment.insert(QStringLiteral("KSWORD_PLUGIN_COLOR_BORDER"), KswordTheme::BorderColorHex());
            environment.insert(
                QStringLiteral("KSWORD_PLUGIN_COLOR_ACCENT"),
                KswordTheme::AccentHex(KswordTheme::AccentRole::Blue));
            environment.insert(QStringLiteral("KSWORD_PLUGIN_COLOR_ON_ACCENT"), KswordTheme::OnAccentHex());
            m_process->setProcessEnvironment(environment);

            connect(m_process, &QProcess::started, this, [this]() {
                m_statusLabel->setText(QStringLiteral("插件进程已启动，等待窗口握手…"));
            });
            connect(m_process, &QProcess::readyReadStandardOutput, this, [this]() { consumeStdout(); });
            connect(m_process, &QProcess::readyReadStandardError, this, [this]() {
                const QString text = QString::fromUtf8(m_process->readAllStandardError());
                if (!text.isEmpty()) m_diagnostics->appendPlainText(text.trimmed());
            });
            connect(m_process, &QProcess::errorOccurred, this, [this](const QProcess::ProcessError error) {
                if (error == QProcess::FailedToStart)
                {
                    fail(QStringLiteral("Tab 插件启动失败：%1").arg(m_process->errorString()), false);
                }
            });
            connect(m_process, qOverload<int, QProcess::ExitStatus>(&QProcess::finished), this,
                [this](const int exitCode, const QProcess::ExitStatus exitStatus) {
                    if (m_stopping) return;
                    m_pluginWindow = nullptr;
                    const QString exitText = QStringLiteral("插件进程已退出：exit=%1, status=%2")
                        .arg(exitCode)
                        .arg(exitStatus == QProcess::NormalExit ? QStringLiteral("normal") : QStringLiteral("crashed"));
                    m_diagnostics->appendPlainText(exitText);
                    if (!m_failed) fail(exitText, false);
                });
            m_process->start();

            QPointer<PluginTabPage> guard(this);
            QTimer::singleShot(m_descriptor.tabPresentation.startupTimeoutMs, this, [guard]() {
                if (guard != nullptr && !guard->m_ready && !guard->m_failed)
                {
                    guard->fail(QStringLiteral("Tab 插件启动超时，未收到有效的窗口握手。"), true);
                }
            });
        }

        void consumeStdout();
        void processProtocolLine(const QByteArray& line);
        void resizePluginWindow();
        void fail(const QString& message, bool stopProcess);

        PluginDescriptor m_descriptor;
        QProcess* m_process = nullptr;
        QWidget* m_surface = nullptr;
        QLabel* m_statusLabel = nullptr;
        QPushButton* m_diagnosticsButton = nullptr;
        QPushButton* m_retryButton = nullptr;
        QPlainTextEdit* m_diagnostics = nullptr;
        QByteArray m_stdoutBuffer;
        HWND m_pluginWindow = nullptr;
        bool m_ready = false;
        bool m_failed = false;
        bool m_stopping = false;
        bool m_startScheduled = false;
    };

    void PluginTabPage::consumeStdout()
    {
        if (m_process == nullptr)
        {
            return;
        }
        m_stdoutBuffer += m_process->readAllStandardOutput();
        if (m_stdoutBuffer.size() > kMaxBufferedStdoutBytes && !m_stdoutBuffer.contains('\n'))
        {
            fail(QStringLiteral("Tab 插件 stdout 单行缓冲超过 1 MiB，已终止插件。"), true);
            return;
        }
        while (true)
        {
            const qsizetype newlineIndex = m_stdoutBuffer.indexOf('\n');
            if (newlineIndex < 0)
            {
                break;
            }
            if (newlineIndex > kMaxBufferedStdoutBytes)
            {
                fail(QStringLiteral("Tab 插件 stdout 单行缓冲超过 1 MiB，已终止插件。"), true);
                return;
            }
            QByteArray line = m_stdoutBuffer.left(newlineIndex);
            m_stdoutBuffer.remove(0, newlineIndex + 1);
            if (line.endsWith('\r')) line.chop(1);
            if (!line.trimmed().isEmpty()) processProtocolLine(line);
            if (m_failed) return;
        }
    }

    void PluginTabPage::processProtocolLine(const QByteArray& line)
    {
        QJsonParseError parseError;
        const QJsonDocument document = QJsonDocument::fromJson(line, &parseError);
        if (parseError.error != QJsonParseError::NoError || !document.isObject())
        {
            m_diagnostics->appendPlainText(QString::fromUtf8(line));
            fail(QStringLiteral("Tab 插件输出不是有效的 JSON Lines 协议事件。"), true);
            return;
        }
        const QJsonObject object = document.object();
        if (object.value(QStringLiteral("protocol")).toString() != QStringLiteral("ksword-plugin/1") ||
            object.value(QStringLiteral("plugin_id")).toString() != m_descriptor.id)
        {
            fail(QStringLiteral("Tab 插件协议版本或 plugin_id 与清单不匹配。"), true);
            return;
        }
        const QString event = object.value(QStringLiteral("event")).toString();
        if (event == QStringLiteral("error"))
        {
            fail(QStringLiteral("Tab 插件报告错误：%1")
                .arg(object.value(QStringLiteral("message")).toString(QStringLiteral("未知错误"))), true);
            return;
        }
        if (event != m_descriptor.tabPresentation.readyEvent)
        {
            m_diagnostics->appendPlainText(QString::fromUtf8(line));
            return;
        }

        const QJsonValue handleValue = object.value(QStringLiteral("hwnd"));
        bool handleOk = false;
        qulonglong numericHandle = 0;
        if (handleValue.isString())
        {
            numericHandle = handleValue.toString().toULongLong(&handleOk, 0);
        }
        else if (handleValue.isDouble() && handleValue.toDouble() > 0.0)
        {
            numericHandle = static_cast<qulonglong>(handleValue.toDouble());
            handleOk = true;
        }
        HWND candidateWindow = handleOk
            ? reinterpret_cast<HWND>(static_cast<quintptr>(numericHandle))
            : nullptr;
        if (!handleOk || !::IsWindow(candidateWindow))
        {
            fail(QStringLiteral("Tab 插件返回的窗口句柄无效。"), true);
            return;
        }

        DWORD owningProcessId = 0;
        ::GetWindowThreadProcessId(candidateWindow, &owningProcessId);
        if (m_process == nullptr || owningProcessId != static_cast<DWORD>(m_process->processId()))
        {
            fail(QStringLiteral("Tab 插件窗口不属于刚启动的插件进程，已拒绝嵌入。"), true);
            return;
        }
        const HWND expectedParent = reinterpret_cast<HWND>(m_surface->winId());
        if (::GetParent(candidateWindow) != expectedParent ||
            (::GetWindowLongPtrW(candidateWindow, GWL_STYLE) & WS_CHILD) == 0)
        {
            fail(QStringLiteral("Tab 插件窗口未作为宿主容器的直接 WS_CHILD 子窗口创建。"), true);
            return;
        }

        m_pluginWindow = candidateWindow;
        m_ready = true;
        m_failed = false;
        m_retryButton->setVisible(false);
        m_statusLabel->setStyleSheet({});
        m_statusLabel->setText(QStringLiteral("Tab 插件已连接（外部进程 PID %1）。")
            .arg(owningProcessId));
        ::ShowWindow(m_pluginWindow, SW_SHOW);
        resizePluginWindow();
    }

    void PluginTabPage::resizePluginWindow()
    {
        if (!::IsWindow(m_pluginWindow) || m_surface == nullptr)
        {
            return;
        }
        const QSize size = m_surface->size();
        ::MoveWindow(m_pluginWindow, 0, 0, qMax(1, size.width()), qMax(1, size.height()), TRUE);
    }

    void PluginTabPage::fail(const QString& message, const bool stopProcess)
    {
        if (m_stopping)
        {
            return;
        }
        m_failed = true;
        m_ready = false;
        m_pluginWindow = nullptr;
        m_statusLabel->setText(message);
        m_statusLabel->setStyleSheet(QStringLiteral("color:%1;font-weight:600;")
            .arg(KswordTheme::ErrorColor().name(QColor::HexRgb)));
        m_retryButton->setVisible(true);
        m_diagnostics->appendPlainText(message);
        if (stopProcess && m_process != nullptr && m_process->state() != QProcess::NotRunning)
        {
            m_process->terminate();
            QPointer<QProcess> processGuard(m_process);
            QTimer::singleShot(1500, this, [processGuard]() {
                if (processGuard != nullptr && processGuard->state() != QProcess::NotRunning)
                {
                    processGuard->kill();
                }
            });
        }
    }

    bool promoteExtractedPlugin(
        const MarketplacePlugin& plugin,
        const QString& pluginRoot,
        const QString& stagingDirectory,
        QString* errorOut)
    {
        if (errorOut == nullptr)
        {
            return false;
        }

        PluginDescriptor extractedDescriptor;
        if (!loadPluginManifest(stagingDirectory, plugin.installDirectory, &extractedDescriptor, errorOut))
        {
            return false;
        }
        if (extractedDescriptor.id != plugin.id)
        {
            *errorOut = QStringLiteral("已解压插件的 id 与商城目录不一致。");
            return false;
        }

        QDir rootDirectory(pluginRoot);
        const QString stagingName = QFileInfo(stagingDirectory).fileName();
        const QString stagedPluginPath = stagingName + QChar('/') + plugin.installDirectory;
        const QString backupName = QStringLiteral(".ksword-plugin-backup-%1-%2")
            .arg(plugin.installDirectory, QUuid::createUuid().toString(QUuid::WithoutBraces));
        const QString targetPath = rootDirectory.filePath(plugin.installDirectory);
        const bool targetExists = QFileInfo::exists(targetPath);

        if (targetExists && !rootDirectory.rename(plugin.installDirectory, backupName))
        {
            *errorOut = QStringLiteral("无法备份现有插件目录：%1").arg(QDir::toNativeSeparators(targetPath));
            return false;
        }

        if (!rootDirectory.rename(stagedPluginPath, plugin.installDirectory))
        {
            if (targetExists)
            {
                rootDirectory.rename(backupName, plugin.installDirectory);
            }
            *errorOut = QStringLiteral("无法将已验证插件安装到：%1").arg(QDir::toNativeSeparators(targetPath));
            return false;
        }

        if (targetExists)
        {
            // 新版本已经就位后，清理失败不应把一次成功安装报告为失败；
            // 遗留备份不会被发现为有效插件（名称不符合插件 ID 规则）。
            QDir(rootDirectory.filePath(backupName)).removeRecursively();
        }
        return true;
    }

    class PluginManagerDialog final : public QDialog
    {
    public:
        explicit PluginManagerDialog(QWidget* parent)
            : QDialog(parent)
        {
            setAttribute(Qt::WA_DeleteOnClose, true);
            setWindowTitle(QStringLiteral("插件管理"));
            resize(900, 520);
            setModal(false);
            auto* layout = new QVBoxLayout(this);

            m_networkManager = new QNetworkAccessManager(this);
            auto* tabWidget = new QTabWidget(this);
            auto* localPage = new QWidget(tabWidget);
            auto* localLayout = new QVBoxLayout(localPage);
            m_table = new ks::ui::VisibleTableWidget(this);
            m_table->setColumnCount(4);
            m_table->setHorizontalHeaderLabels(QStringList{ QStringLiteral("名称"), QStringLiteral("版本"), QStringLiteral("目标"), QStringLiteral("说明") });
            m_table->setEditTriggers(QAbstractItemView::NoEditTriggers);
            m_table->setSelectionBehavior(QAbstractItemView::SelectRows);
            m_table->setSelectionMode(QAbstractItemView::SingleSelection);
            m_table->horizontalHeader()->setStretchLastSection(true);
            m_table->setAlternatingRowColors(true);
            localLayout->addWidget(m_table);
            tabWidget->addTab(localPage, QStringLiteral("已安装"));

            auto* marketplacePage = new QWidget(tabWidget);
            auto* marketplaceLayout = new QVBoxLayout(marketplacePage);
            m_marketplaceTable = new ks::ui::VisibleTableWidget(marketplacePage);
            m_marketplaceTable->setColumnCount(5);
            m_marketplaceTable->setHorizontalHeaderLabels(QStringList{
                QStringLiteral("名称"), QStringLiteral("版本"), QStringLiteral("目标"), QStringLiteral("许可证"), QStringLiteral("说明") });
            m_marketplaceTable->setEditTriggers(QAbstractItemView::NoEditTriggers);
            m_marketplaceTable->setSelectionBehavior(QAbstractItemView::SelectRows);
            m_marketplaceTable->setSelectionMode(QAbstractItemView::SingleSelection);
            m_marketplaceTable->horizontalHeader()->setStretchLastSection(true);
            m_marketplaceTable->setAlternatingRowColors(true);
            marketplaceLayout->addWidget(m_marketplaceTable);
            tabWidget->addTab(marketplacePage, QStringLiteral("插件商城"));
            layout->addWidget(tabWidget, 1);

            m_status = new QLabel(this);
            m_status->setWordWrap(true);
            layout->addWidget(m_status);
            auto* buttons = new QHBoxLayout();
            auto* refreshButton = new QPushButton(QStringLiteral("重新扫描本地"), this);
            auto* refreshMarketplaceButton = new QPushButton(QStringLiteral("刷新商城"), this);
            auto* detailButton = new QPushButton(QStringLiteral("查看清单详情"), this);
            auto* installButton = new QPushButton(QStringLiteral("同意许可证并一键安装"), this);
            m_openFolderButton = new QPushButton(QStringLiteral("打开插件目录"), this);
            auto* closeButton = new QPushButton(QStringLiteral("关闭"), this);
            buttons->addWidget(refreshButton);
            buttons->addWidget(refreshMarketplaceButton);
            buttons->addWidget(detailButton);
            buttons->addWidget(installButton);
            buttons->addWidget(m_openFolderButton);
            buttons->addStretch(1);
            buttons->addWidget(closeButton);
            layout->addLayout(buttons);
            connect(refreshButton, &QPushButton::clicked, this, [this]() { refreshPlugins(); });
            connect(refreshMarketplaceButton, &QPushButton::clicked, this, [this]() { refreshMarketplace(); });
            connect(detailButton, &QPushButton::clicked, this, [this]() { showSelectedDetails(); });
            connect(installButton, &QPushButton::clicked, this, [this]() { requestSelectedMarketplaceLicense(); });
            connect(m_openFolderButton, &QPushButton::clicked, this, [this]() {
                if (!m_pluginRoot.isEmpty()) QDesktopServices::openUrl(QUrl::fromLocalFile(m_pluginRoot));
            });
            connect(closeButton, &QPushButton::clicked, this, &QDialog::close);
            refreshPlugins();
            refreshMarketplace();
        }

    private:
        void refreshPlugins()
        {
            PluginListResult result;
            QString errorText;
            m_table->setRowCount(0);
            m_plugins.clear();
            m_pluginRoot.clear();
            if (!discoverPlugins(&result, &errorText))
            {
                m_status->setText(QStringLiteral("插件目录不可用：%1").arg(errorText));
                m_openFolderButton->setEnabled(false);
                return;
            }
            m_plugins = result.plugins;
            m_pluginRoot = result.pluginRoot;
            for (const PluginDescriptor& descriptor : m_plugins)
            {
                const int row = m_table->rowCount();
                m_table->insertRow(row);
                m_table->setItem(row, 0, new QTableWidgetItem(descriptor.name));
                m_table->setItem(row, 1, new QTableWidgetItem(descriptor.version));
                m_table->setItem(row, 2, new QTableWidgetItem(descriptor.targets.join(QStringLiteral(", "))));
                m_table->setItem(row, 3, new QTableWidgetItem(descriptor.description));
            }
            if (!m_plugins.isEmpty()) m_table->selectRow(0);
            m_openFolderButton->setEnabled(QDir(m_pluginRoot).exists());
            QString status = QStringLiteral("已发现 %1 个有效插件。插件目录：%2")
                .arg(m_plugins.size())
                .arg(QDir::toNativeSeparators(m_pluginRoot));
            if (!result.ignoredManifests.isEmpty())
            {
                status += QStringLiteral("\n忽略的清单：%1").arg(result.ignoredManifests.join(QStringLiteral("；")));
            }
            m_status->setText(status);
        }

        void showSelectedDetails()
        {
            const int row = m_table->currentRow();
            if (row < 0 || row >= m_plugins.size())
            {
                QMessageBox::information(this, QStringLiteral("插件管理"), QStringLiteral("请先选择一个插件。"));
                return;
            }
            const PluginDescriptor& descriptor = m_plugins.at(row);
            const QString detailText = QStringLiteral("id=%1\nversion=%2\nplugin_type=%3\nruntime=%4\nentrypoint=%5\ndefault_command=%6\ntargets=%7\nvisualization=%8\ndirectory=%9\n\n%10")
                .arg(descriptor.id)
                .arg(descriptor.version)
                .arg(descriptor.pluginType)
                .arg(descriptor.runtime)
                .arg(descriptor.entrypointPath)
                .arg(descriptor.defaultCommand)
                .arg(descriptor.targets.join(QStringLiteral(", ")))
                .arg(descriptor.visualization.enabled ? descriptor.visualization.type : QStringLiteral("none"))
                .arg(descriptor.pluginDirectory)
                .arg(descriptor.description);
            QMessageBox::information(this, QStringLiteral("插件清单：%1").arg(descriptor.name), detailText);
        }

        void refreshMarketplace()
        {
            m_marketplaceTable->setRowCount(0);
            m_marketplacePlugins.clear();
            m_status->setText(QStringLiteral("正在从 KSwordDEV/Plugins 读取插件商城目录…"));
            QNetworkRequest request(QUrl(QString::fromLatin1(kMarketplaceCatalogUrl)));
            request.setHeader(QNetworkRequest::UserAgentHeader, QStringLiteral("KSword-PluginMarketplace/1"));
            request.setAttribute(QNetworkRequest::CacheLoadControlAttribute, QNetworkRequest::PreferNetwork);
            QNetworkReply* reply = m_networkManager->get(request);
            connect(reply, &QNetworkReply::finished, this, [this, reply]() {
                const QByteArray payload = reply->readAll();
                const bool networkOk = reply->error() == QNetworkReply::NoError;
                const QString networkError = networkOk ? QString() : networkReplyErrorText(reply);
                reply->deleteLater();
                if (!networkOk)
                {
                    m_status->setText(QStringLiteral("商城目录读取失败：%1").arg(networkError));
                    return;
                }
                QJsonParseError parseError;
                const QJsonDocument document = QJsonDocument::fromJson(payload, &parseError);
                if (parseError.error != QJsonParseError::NoError || !document.isObject())
                {
                    m_status->setText(QStringLiteral("商城目录不是有效 JSON：%1").arg(parseError.errorString()));
                    return;
                }
                const QJsonObject root = document.object();
                if (root.value(QStringLiteral("ksword_plugin_marketplace_api")).toString() != QStringLiteral("1"))
                {
                    m_status->setText(QStringLiteral("商城目录版本不受支持。"));
                    return;
                }
                QStringList ignoredEntries;
                for (const QJsonValue& value : root.value(QStringLiteral("plugins")).toArray())
                {
                    MarketplacePlugin plugin;
                    QString errorText;
                    if (value.isObject() && parseMarketplacePlugin(value.toObject(), &plugin, &errorText))
                    {
                        m_marketplacePlugins.push_back(plugin);
                    }
                    else
                    {
                        ignoredEntries.push_back(errorText.isEmpty() ? QStringLiteral("无效条目") : errorText);
                    }
                }
                for (const MarketplacePlugin& plugin : m_marketplacePlugins)
                {
                    const int row = m_marketplaceTable->rowCount();
                    m_marketplaceTable->insertRow(row);
                    m_marketplaceTable->setItem(row, 0, new QTableWidgetItem(plugin.name));
                    m_marketplaceTable->setItem(row, 1, new QTableWidgetItem(plugin.version));
                    m_marketplaceTable->setItem(row, 2, new QTableWidgetItem(plugin.targets.join(QStringLiteral(", "))));
                    m_marketplaceTable->setItem(row, 3, new QTableWidgetItem(plugin.licenseName));
                    m_marketplaceTable->setItem(row, 4, new QTableWidgetItem(plugin.description));
                }
                if (!m_marketplacePlugins.isEmpty()) m_marketplaceTable->selectRow(0);
                QString status = QStringLiteral("插件商城已从 KSwordDEV/Plugins 刷新：%1 个可下载插件。").arg(m_marketplacePlugins.size());
                if (!ignoredEntries.isEmpty()) status += QStringLiteral(" 已忽略 %1 个无效条目。").arg(ignoredEntries.size());
                m_status->setText(status);
            });
        }

        void requestSelectedMarketplaceLicense()
        {
            const int row = m_marketplaceTable->currentRow();
            if (row < 0 || row >= m_marketplacePlugins.size())
            {
                QMessageBox::information(this, QStringLiteral("插件商城"), QStringLiteral("请先在“插件商城”中选择一个插件。"));
                return;
            }
            const MarketplacePlugin plugin = m_marketplacePlugins.at(row);
            m_status->setText(QStringLiteral("正在读取 %1 的许可证；同意前不会下载或安装插件。").arg(plugin.name));
            QNetworkRequest request(plugin.licenseUrl);
            request.setHeader(QNetworkRequest::UserAgentHeader, QStringLiteral("KSword-PluginMarketplace/1"));
            QNetworkReply* reply = m_networkManager->get(request);
            connect(reply, &QNetworkReply::finished, this, [this, reply, plugin]() {
                const QByteArray payload = reply->readAll();
                const bool networkOk = reply->error() == QNetworkReply::NoError;
                const QString networkError = networkOk ? QString() : networkReplyErrorText(reply);
                reply->deleteLater();
                if (!networkOk || payload.isEmpty())
                {
                    QMessageBox::warning(this, QStringLiteral("插件商城"), QStringLiteral("无法读取许可证：%1").arg(networkError));
                    return;
                }
                showLicenseAgreement(plugin, QString::fromUtf8(payload));
            });
        }

        void showLicenseAgreement(const MarketplacePlugin& plugin, const QString& licenseText)
        {
            QDialog licenseDialog(this);
            licenseDialog.setWindowTitle(QStringLiteral("许可证：%1").arg(plugin.name));
            licenseDialog.resize(780, 620);
            auto* layout = new QVBoxLayout(&licenseDialog);
            auto* label = new QLabel(QStringLiteral("安装 %1 前，请阅读并同意：%2。未同意不会发起插件 ZIP 下载。")
                .arg(plugin.name, plugin.licenseName), &licenseDialog);
            label->setWordWrap(true);
            layout->addWidget(label);
            auto* text = new QPlainTextEdit(&licenseDialog);
            text->setReadOnly(true);
            text->setPlainText(licenseText);
            layout->addWidget(text, 1);
            auto* agree = new QCheckBox(QStringLiteral("我已阅读并同意上述插件许可证"), &licenseDialog);
            layout->addWidget(agree);
            auto* buttons = new QDialogButtonBox(QDialogButtonBox::Cancel, &licenseDialog);
            QPushButton* acceptButton = buttons->addButton(QStringLiteral("同意并一键安装"), QDialogButtonBox::AcceptRole);
            acceptButton->setEnabled(false);
            layout->addWidget(buttons);
            connect(agree, &QCheckBox::toggled, acceptButton, &QPushButton::setEnabled);
            connect(buttons, &QDialogButtonBox::accepted, &licenseDialog, &QDialog::accept);
            connect(buttons, &QDialogButtonBox::rejected, &licenseDialog, &QDialog::reject);
            if (licenseDialog.exec() != QDialog::Accepted)
            {
                m_status->setText(QStringLiteral("未同意许可证，未下载或安装 %1。").arg(plugin.name));
                return;
            }
            downloadMarketplaceArchive(plugin);
        }

        void downloadMarketplaceArchive(const MarketplacePlugin& plugin)
        {
            m_status->setText(QStringLiteral("正在下载 %1；将校验 SHA-256 后一键安装。").arg(plugin.name));
            QNetworkRequest request(plugin.archiveUrl);
            request.setHeader(QNetworkRequest::UserAgentHeader, QStringLiteral("KSword-PluginMarketplace/1"));
            QNetworkReply* reply = m_networkManager->get(request);
            connect(reply, &QNetworkReply::finished, this, [this, reply, plugin]() {
                const QByteArray archiveBytes = reply->readAll();
                const bool networkOk = reply->error() == QNetworkReply::NoError;
                const QString networkError = networkOk ? QString() : networkReplyErrorText(reply);
                reply->deleteLater();
                if (!networkOk)
                {
                    QMessageBox::warning(this, QStringLiteral("插件商城"), QStringLiteral("插件下载失败：%1").arg(networkError));
                    return;
                }
                if (archiveBytes.isEmpty() || archiveBytes.size() > kMaxMarketplaceArchiveBytes)
                {
                    QMessageBox::warning(this, QStringLiteral("插件商城"), QStringLiteral("插件包为空或超过 256 MiB 限制。"));
                    return;
                }
                const QString actualSha256 = QString::fromLatin1(QCryptographicHash::hash(archiveBytes, QCryptographicHash::Sha256).toHex());
                if (actualSha256.compare(plugin.sha256, Qt::CaseInsensitive) != 0)
                {
                    QMessageBox::warning(this, QStringLiteral("插件商城"), QStringLiteral("SHA-256 校验失败，已拒绝安装插件。"));
                    return;
                }
                installMarketplaceArchive(plugin, archiveBytes);
            });
        }

        void installMarketplaceArchive(const MarketplacePlugin& plugin, const QByteArray& archiveBytes)
        {
            const QString pluginRoot = resolvePluginInstallRoot();
            if (!QDir().mkpath(pluginRoot))
            {
                QMessageBox::warning(this, QStringLiteral("插件商城"), QStringLiteral("无法创建插件目录：%1")
                    .arg(QDir::toNativeSeparators(pluginRoot)));
                return;
            }

            {
                const QString archivePath = QDir(pluginRoot).filePath(
                    QStringLiteral(".ksword-plugin-download-%1.zip")
                        .arg(QUuid::createUuid().toString(QUuid::WithoutBraces)));
                QSaveFile archiveFile(archivePath);
                if (!archiveFile.open(QIODevice::WriteOnly) ||
                    archiveFile.write(archiveBytes) != archiveBytes.size() ||
                    !archiveFile.commit())
                {
                    QMessageBox::warning(this, QStringLiteral("插件商城"),
                        QStringLiteral("无法准备已验证的插件包：%1").arg(archiveFile.errorString()));
                    QFile::remove(archivePath);
                    return;
                }
                installVerifiedMarketplaceArchive(plugin, pluginRoot, archivePath);
            }
        }

        void installVerifiedMarketplaceArchive(
            const MarketplacePlugin& plugin,
            const QString& pluginRoot,
            const QString& archivePath)
        {
            const QString stagingName = QStringLiteral(".ksword-plugin-stage-%1-%2")
                .arg(plugin.installDirectory, QUuid::createUuid().toString(QUuid::WithoutBraces));
            const QString stagingPath = QDir(pluginRoot).filePath(stagingName);
            if (!QDir().mkpath(stagingPath))
            {
                QFile::remove(archivePath);
                QMessageBox::warning(this, QStringLiteral("插件商城"), QStringLiteral("无法创建插件安装暂存目录。"));
                return;
            }

            const QString powerShell = QStandardPaths::findExecutable(QStringLiteral("powershell.exe"));
            if (powerShell.isEmpty())
            {
                QDir(stagingPath).removeRecursively();
                QFile::remove(archivePath);
                QMessageBox::warning(this, QStringLiteral("插件商城"), QStringLiteral("未找到 Windows PowerShell，无法解压插件包。"));
                return;
            }

            auto* extractor = new QProcess(this);
            extractor->setProcessChannelMode(QProcess::SeparateChannels);
            const QString command = QStringLiteral("$ErrorActionPreference='Stop'; Expand-Archive -LiteralPath %1 -DestinationPath %2 -Force")
                .arg(quotePowerShellLiteral(archivePath), quotePowerShellLiteral(stagingPath));
            extractor->setProgram(powerShell);
            extractor->setArguments(QStringList{
                QStringLiteral("-NoLogo"), QStringLiteral("-NoProfile"), QStringLiteral("-NonInteractive"),
                QStringLiteral("-Command"), command });
            m_status->setText(QStringLiteral("已验证 %1，正在安全解压并安装…").arg(plugin.name));
            connect(extractor, &QProcess::errorOccurred, this,
                [this, extractor, archivePath, stagingPath](const QProcess::ProcessError error) {
                    if (error != QProcess::FailedToStart) return;
                    QDir(stagingPath).removeRecursively();
                    QFile::remove(archivePath);
                    QMessageBox::warning(this, QStringLiteral("插件商城"), QStringLiteral("无法启动插件解压器：%1")
                        .arg(extractor->errorString()));
                    extractor->deleteLater();
                });
            connect(extractor, qOverload<int, QProcess::ExitStatus>(&QProcess::finished), this,
                [this, extractor, archivePath, plugin, pluginRoot, stagingPath](const int exitCode, const QProcess::ExitStatus exitStatus) {
                    const QString details = QString::fromLocal8Bit(extractor->readAllStandardError()).trimmed();
                    extractor->deleteLater();
                    QFile::remove(archivePath);
                    if (exitStatus != QProcess::NormalExit || exitCode != 0)
                    {
                        QDir(stagingPath).removeRecursively();
                        QMessageBox errorBox(QMessageBox::Warning,
                            QStringLiteral("插件商城"),
                            QStringLiteral("插件包解压失败（退出码 %1）。").arg(exitCode),
                            QMessageBox::Ok,
                            this);
                        if (!details.isEmpty()) errorBox.setDetailedText(details);
                        errorBox.exec();
                        return;
                    }

                    QString installError;
                    const bool installed = promoteExtractedPlugin(plugin, pluginRoot, stagingPath, &installError);
                    QDir(stagingPath).removeRecursively();
                    if (!installed)
                    {
                        QMessageBox::warning(this, QStringLiteral("插件商城"), QStringLiteral("插件包已验证，但安装被拒绝：%1").arg(installError));
                        return;
                    }
                    refreshPlugins();
                    m_status->setText(QStringLiteral("已一键安装并验证 %1 到 %2。")
                        .arg(plugin.name, QDir::toNativeSeparators(QDir(pluginRoot).filePath(plugin.installDirectory))));
                    QMessageBox::information(this, QStringLiteral("插件商城"), QStringLiteral("%1 已安装，可立即从“插件”菜单调用。")
                        .arg(plugin.name));
                });
            extractor->start();
        }

        QTableWidget* m_table = nullptr;
        QTableWidget* m_marketplaceTable = nullptr;
        QLabel* m_status = nullptr;
        QPushButton* m_openFolderButton = nullptr;
        QNetworkAccessManager* m_networkManager = nullptr;
        QList<PluginDescriptor> m_plugins;
        QList<MarketplacePlugin> m_marketplacePlugins;
        QString m_pluginRoot;
    };
}

void ks::plugin_host::populateTargetMenu(QMenu* menu, QWidget* owner, const InvocationContext& context)
{
    if (menu == nullptr || owner == nullptr) return;
    menu->clear();
    menu->setToolTipsVisible(true);
    QString contextError;
    if (!isUsableContext(context, &contextError))
    {
        QAction* action = menu->addAction(contextError);
        action->setEnabled(false);
        return;
    }
    PluginListResult result;
    QString errorText;
    if (!discoverPlugins(&result, &errorText))
    {
        QAction* action = menu->addAction(QStringLiteral("插件不可用：%1").arg(errorText));
        action->setEnabled(false);
        return;
    }
    const QString target = targetName(context.targetKind);
    int addedActions = 0;
    for (const PluginDescriptor& descriptor : result.plugins)
    {
        if (!descriptor.targets.contains(target)) continue;
        QAction* action = menu->addAction(descriptor.name);
        action->setToolTip(QStringLiteral("%1\nID：%2\n目标：%3")
            .arg(descriptor.description, descriptor.id, descriptor.targets.join(QStringLiteral(", "))));
        QObject::connect(action, &QAction::triggered, owner, [owner, descriptor, context]() {
            launchPlugin(owner, descriptor, context);
        });
        ++addedActions;
    }
    if (addedActions == 0)
    {
        QString emptyText;
        switch (context.targetKind)
        {
        case TargetKind::File: emptyText = QStringLiteral("没有声明支持文件目标的插件"); break;
        case TargetKind::Process: emptyText = QStringLiteral("没有声明支持进程目标的插件"); break;
        case TargetKind::Network: emptyText = QStringLiteral("没有声明支持网络目标的插件"); break;
        }
        QAction* action = menu->addAction(emptyText);
        action->setEnabled(false);
    }
}

int ks::plugin_host::populateTabPlugins(QTabWidget* tabWidget, QWidget* owner)
{
    if (tabWidget == nullptr || owner == nullptr)
    {
        return 0;
    }
    PluginListResult result;
    QString errorText;
    if (!discoverPlugins(&result, &errorText))
    {
        return 0;
    }
    int addedTabs = 0;
    for (const PluginDescriptor& descriptor : result.plugins)
    {
        if (descriptor.pluginType != QStringLiteral("tab") || !descriptor.tabPresentation.enabled ||
            !descriptor.targets.contains(QStringLiteral("tab")))
        {
            continue;
        }
        auto* page = new PluginTabPage(tabWidget, descriptor);
        page->setProperty("kswordPluginId", descriptor.id);
        tabWidget->addTab(
            page,
            QIcon(QStringLiteral(":/Icon/process_start.svg")),
            descriptor.tabPresentation.title);
        tabWidget->setTabToolTip(tabWidget->indexOf(page), descriptor.description);
        ++addedTabs;
    }
    return addedTabs;
}

QWidget* ks::plugin_host::createTabPluginContainer(QWidget* parent)
{
    auto* container = new QWidget(parent);
    container->setObjectName(QStringLiteral("ksTabPluginContainer"));

    auto* rootLayout = new QVBoxLayout(container);
    rootLayout->setContentsMargins(0, 0, 0, 0);
    rootLayout->setSpacing(0);

    auto* tabWidget = new QTabWidget(container);
    tabWidget->setObjectName(QStringLiteral("ksTabPluginHost"));
    tabWidget->setDocumentMode(true);
    tabWidget->setMovable(false);
    tabWidget->setTabsClosable(false);
    rootLayout->addWidget(tabWidget, 1);

    // 宿主侧基础样式只锚定插件容器，不污染其它 Dock 或插件原生子窗口。
    const QString pluginContainerStyle = QStringLiteral(
        "QWidget#ksTabPluginContainer{background-color:%1;color:%2;}"
        "QTabWidget#ksTabPluginHost::pane{background-color:%1;border:1px solid %3;border-radius:3px;}"
        "QTabWidget#ksTabPluginHost QTabBar::tab{background-color:%4;color:%2;border:1px solid %3;"
        "border-bottom:none;padding:6px 14px;margin-right:2px;}"
        "QTabWidget#ksTabPluginHost QTabBar::tab:selected{background-color:%5;color:%6;border-color:%5;}"
        "QTabWidget#ksTabPluginHost QTabBar::tab:hover:!selected{background-color:%7;}"
        "QWidget#ksTabPluginEmptyState{background-color:%1;color:%2;}"
        "QLabel#ksTabPluginEmptyTitle{color:%2;font-size:16px;font-weight:600;}"
        "QLabel#ksTabPluginEmptyHint{color:%8;}"
        "QPushButton#ksTabPluginManageButton{background-color:%5;color:%6;border:1px solid %5;"
        "border-radius:3px;padding:6px 14px;font-weight:600;}"
        "QPushButton#ksTabPluginManageButton:hover{background-color:%9;border-color:%9;}"
        "QPushButton#ksTabPluginManageButton:pressed{background-color:%10;border-color:%10;}")
        .arg(KswordTheme::SurfaceColorHex())
        .arg(KswordTheme::TextPrimaryColorHex())
        .arg(KswordTheme::BorderColorHex())
        .arg(KswordTheme::SurfaceAltColorHex())
        .arg(KswordTheme::ActiveTabBackgroundHex())
        .arg(KswordTheme::ActiveTabTextHex())
        .arg(KswordTheme::SurfaceMutedColorHex())
        .arg(KswordTheme::TextSecondaryColorHex())
        .arg(KswordTheme::PrimaryBlueSolidHoverHex())
        .arg(KswordTheme::PrimaryBluePressedHex);
    container->setStyleSheet(pluginContainerStyle);

    if (populateTabPlugins(tabWidget, container) == 0)
    {
        auto* emptyPage = new QWidget(tabWidget);
        emptyPage->setObjectName(QStringLiteral("ksTabPluginEmptyState"));
        auto* emptyLayout = new QVBoxLayout(emptyPage);
        emptyLayout->setContentsMargins(24, 24, 24, 24);
        emptyLayout->setSpacing(10);
        emptyLayout->addStretch(1);

        auto* titleLabel = new QLabel(
            ks::i18n::text(QStringLiteral("plugin.tab.empty.title"), QStringLiteral("尚未安装 Tab 型插件")),
            emptyPage);
        titleLabel->setObjectName(QStringLiteral("ksTabPluginEmptyTitle"));
        titleLabel->setAlignment(Qt::AlignCenter);
        emptyLayout->addWidget(titleLabel);

        auto* hintLabel = new QLabel(
            ks::i18n::text(
                QStringLiteral("plugin.tab.empty.hint"),
                QStringLiteral("请从插件管理器安装 Tab 型插件，安装完成后重启 KSword。")),
            emptyPage);
        hintLabel->setObjectName(QStringLiteral("ksTabPluginEmptyHint"));
        hintLabel->setAlignment(Qt::AlignCenter);
        hintLabel->setWordWrap(true);
        emptyLayout->addWidget(hintLabel);

        auto* buttonRow = new QHBoxLayout();
        buttonRow->addStretch(1);
        auto* manageButton = new QPushButton(
            ks::i18n::text(QStringLiteral("plugin.tab.empty.manage"), QStringLiteral("打开插件管理器")),
            emptyPage);
        manageButton->setObjectName(QStringLiteral("ksTabPluginManageButton"));
        buttonRow->addWidget(manageButton);
        buttonRow->addStretch(1);
        emptyLayout->addLayout(buttonRow);
        emptyLayout->addStretch(1);

        QObject::connect(manageButton, &QPushButton::clicked, emptyPage, [emptyPage]() {
            ks::plugin_host::showPluginManager(emptyPage);
        });
        tabWidget->addTab(
            emptyPage,
            ks::i18n::text(QStringLiteral("plugin.tab.empty.overview"), QStringLiteral("概览")));
    }

    return container;
}
void ks::plugin_host::showPluginManager(QWidget* owner)
{
    auto* dialog = new PluginManagerDialog(owner);
    dialog->show();
    dialog->raise();
    dialog->activateWindow();
}
