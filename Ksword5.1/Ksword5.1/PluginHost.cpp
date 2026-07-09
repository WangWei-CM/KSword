#include "PluginHost.h"

#include "theme.h"

#include <QAbstractItemView>
#include <QCheckBox>
#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDesktopServices>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDir>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QHeaderView>
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
#include <QPushButton>
#include <QRegularExpression>
#include <QStandardPaths>
#include <QTableWidget>
#include <QTableWidgetItem>
#include <QTabWidget>
#include <QTemporaryFile>
#include <QUrl>
#include <QUuid>
#include <QVBoxLayout>

#include <memory>

namespace
{
    constexpr qint64 kMaxManifestBytes = 64 * 1024;
    constexpr int kMaxVisibleOutputCharacters = 12000;
    constexpr qint64 kMaxMarketplaceArchiveBytes = 256LL * 1024LL * 1024LL;
    constexpr char kMarketplaceCatalogUrl[] = "https://raw.githubusercontent.com/KSwordDEV/Plugins/main/catalog.json";

    struct PluginDescriptor
    {
        QString id;
        QString name;
        QString version;
        QString description;
        QString runtime;
        QString entrypointPath;
        QString defaultCommand;
        QString pluginDirectory;
        QStringList targets;
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
            if ((target == QStringLiteral("file") || target == QStringLiteral("process")) &&
                !plugin.targets.contains(target))
            {
                plugin.targets.push_back(target);
            }
        }
        if (plugin.targets.isEmpty())
        {
            *errorOut = QStringLiteral("商城条目的 targets 必须包含 file 和/或 process。");
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
            if ((targetText == QStringLiteral("file") || targetText == QStringLiteral("process")) &&
                !descriptor.targets.contains(targetText))
            {
                descriptor.targets.push_back(targetText);
            }
        }
        if (descriptor.targets.isEmpty())
        {
            *errorOut = QStringLiteral("targets 必须包含 file 和/或 process。");
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

    QString formatConsoleOutput(const QString& outputText)
    {
        const QString trimmed = outputText.trimmed();
        if (trimmed.isEmpty())
        {
            return QStringLiteral("<插件未输出内容>");
        }
        if (trimmed.size() <= kMaxVisibleOutputCharacters)
        {
            return trimmed;
        }
        return trimmed.left(9000) + QStringLiteral("\n\n...（插件输出已截断）...\n\n") + trimmed.right(2500);
    }

    QString targetName(const ks::plugin_host::TargetKind targetKind)
    {
        return targetKind == ks::plugin_host::TargetKind::File ? QStringLiteral("file") : QStringLiteral("process");
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
        else
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

        auto* process = new QProcess(owner);
        QProcessEnvironment environment = QProcessEnvironment::systemEnvironment();
        environment.insert(QStringLiteral("KSWORD_PLUGIN_ROOT"), findPluginRoot());
        process->setProcessEnvironment(environment);
        process->setProgram(program);
        process->setArguments(arguments);
        process->setWorkingDirectory(descriptor.pluginDirectory);
        process->setProcessChannelMode(QProcess::SeparateChannels);
        const QString title = QStringLiteral("插件：%1").arg(descriptor.name);
        QObject::connect(process, &QProcess::errorOccurred, owner, [owner, process, title](const QProcess::ProcessError error) {
            if (error == QProcess::FailedToStart)
            {
                QMessageBox::warning(owner, title, QStringLiteral("无法启动插件入口：%1").arg(process->errorString()));
                process->deleteLater();
            }
        });
        QObject::connect(process, qOverload<int, QProcess::ExitStatus>(&QProcess::finished), owner,
            [owner, process, title](const int exitCode, const QProcess::ExitStatus exitStatus) {
                QString detail = formatConsoleOutput(QString::fromUtf8(process->readAllStandardOutput()));
                const QString stderrText = QString::fromLocal8Bit(process->readAllStandardError());
                if (!stderrText.trimmed().isEmpty())
                {
                    detail += QStringLiteral("\n\n[stderr]\n") + formatConsoleOutput(stderrText);
                }
                const bool succeeded = exitStatus == QProcess::NormalExit && exitCode == 0;
                const QString message = QStringLiteral("%1\n退出码：%2\n\n%3")
                    .arg(succeeded ? QStringLiteral("插件已完成。") : QStringLiteral("插件执行失败。"))
                    .arg(exitCode)
                    .arg(detail);
                if (succeeded)
                {
                    QMessageBox::information(owner, title, message);
                }
                else
                {
                    QMessageBox::warning(owner, title, message);
                }
                process->deleteLater();
            });
        process->start();
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
            auto* introduction = new QLabel(
                QStringLiteral("插件是独立进程。Ksword 只读取 plugin.json 发现其命令行入口，并直接启动入口程序；不会经过 KswordCLI，也不会加载插件代码。"), this);
            introduction->setWordWrap(true);
            layout->addWidget(introduction);

            m_networkManager = new QNetworkAccessManager(this);
            auto* tabWidget = new QTabWidget(this);
            auto* localPage = new QWidget(tabWidget);
            auto* localLayout = new QVBoxLayout(localPage);
            m_table = new QTableWidget(this);
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
            m_marketplaceTable = new QTableWidget(marketplacePage);
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
            const QString detailText = QStringLiteral("id=%1\nversion=%2\nruntime=%3\nentrypoint=%4\ndefault_command=%5\ntargets=%6\ndirectory=%7\n\n%8")
                .arg(descriptor.id)
                .arg(descriptor.version)
                .arg(descriptor.runtime)
                .arg(descriptor.entrypointPath)
                .arg(descriptor.defaultCommand)
                .arg(descriptor.targets.join(QStringLiteral(", ")))
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
                    QMessageBox::warning(this, QStringLiteral("插件商城"), QStringLiteral("SHA-256 校验失败，已拒绝保存插件包。"));
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

            auto archiveFile = std::make_shared<QTemporaryFile>(
                QDir(pluginRoot).filePath(QStringLiteral(".ksword-plugin-download-XXXXXX.zip")));
            archiveFile->setAutoRemove(true);
            if (!archiveFile->open() || archiveFile->write(archiveBytes) != archiveBytes.size() || !archiveFile->flush())
            {
                QMessageBox::warning(this, QStringLiteral("插件商城"), QStringLiteral("无法准备已验证的插件包：%1")
                    .arg(archiveFile->errorString()));
                return;
            }
            const QString archivePath = archiveFile->fileName();
            archiveFile->close();

            const QString stagingName = QStringLiteral(".ksword-plugin-stage-%1-%2")
                .arg(plugin.installDirectory, QUuid::createUuid().toString(QUuid::WithoutBraces));
            const QString stagingPath = QDir(pluginRoot).filePath(stagingName);
            if (!QDir().mkpath(stagingPath))
            {
                QMessageBox::warning(this, QStringLiteral("插件商城"), QStringLiteral("无法创建插件安装暂存目录。"));
                return;
            }

            const QString powerShell = QStandardPaths::findExecutable(QStringLiteral("powershell.exe"));
            if (powerShell.isEmpty())
            {
                QDir(stagingPath).removeRecursively();
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
                [this, extractor, archiveFile, stagingPath](const QProcess::ProcessError error) {
                    if (error != QProcess::FailedToStart) return;
                    QDir(stagingPath).removeRecursively();
                    QMessageBox::warning(this, QStringLiteral("插件商城"), QStringLiteral("无法启动插件解压器：%1")
                        .arg(extractor->errorString()));
                    extractor->deleteLater();
                });
            connect(extractor, qOverload<int, QProcess::ExitStatus>(&QProcess::finished), this,
                [this, extractor, archiveFile, plugin, pluginRoot, stagingPath](const int exitCode, const QProcess::ExitStatus exitStatus) {
                    const QString details = QString::fromLocal8Bit(extractor->readAllStandardError()).trimmed();
                    extractor->deleteLater();
                    if (exitStatus != QProcess::NormalExit || exitCode != 0)
                    {
                        QDir(stagingPath).removeRecursively();
                        QMessageBox::warning(this, QStringLiteral("插件商城"), QStringLiteral("插件包解压失败：%1")
                            .arg(details.isEmpty() ? QStringLiteral("PowerShell 退出码 %1").arg(exitCode) : details));
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
        QAction* action = menu->addAction(context.targetKind == TargetKind::File
            ? QStringLiteral("没有声明支持文件目标的插件")
            : QStringLiteral("没有声明支持进程目标的插件"));
        action->setEnabled(false);
    }
}

void ks::plugin_host::showPluginManager(QWidget* owner)
{
    auto* dialog = new PluginManagerDialog(owner);
    dialog->show();
    dialog->raise();
    dialog->activateWindow();
}
