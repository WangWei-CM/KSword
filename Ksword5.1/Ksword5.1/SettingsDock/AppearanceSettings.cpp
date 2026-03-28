#include "AppearanceSettings.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QStringList>

namespace
{
    // appendUniquePath 作用：
    // - 向候选路径列表追加“去重后的规范路径”；
    // - 使用大小写不敏感比较，兼容 Windows 路径规则。
    // 调用方式：collectCandidateRootPaths 内部调用。
    // 入参 pathList：候选路径列表指针。
    // 入参 rawPath：待追加路径文本。
    void appendUniquePath(QStringList* pathList, const QString& rawPath)
    {
        if (pathList == nullptr)
        {
            return;
        }

        // normalizedPath 作用：把输入路径规范化，避免“..”或分隔符差异导致重复。
        const QString normalizedPath = QDir::cleanPath(rawPath.trimmed());
        if (normalizedPath.isEmpty())
        {
            return;
        }

        if (!pathList->contains(normalizedPath, Qt::CaseInsensitive))
        {
            pathList->append(normalizedPath);
        }
    }

    // collectCandidateRootPaths 作用：
    // - 生成资源路径解析用的根目录候选集；
    // - 兼容“工作目录在仓库根目录”与“可执行文件在 x64/Debug”两类启动方式。
    // 调用方式：配置/背景路径解析前调用。
    // 返回：按优先级排序的根目录列表。
    QStringList collectCandidateRootPaths()
    {
        QStringList candidateRootPaths;

        // appendFromStartPath 作用：
        // - 从起点目录开始向上回溯若干层；
        // - 每层同时追加“本层目录”和“本层/Ksword5.1 子目录”两个候选。
        const auto appendFromStartPath = [&candidateRootPaths](const QString& startPath) {
            QString walkingPath = QDir::cleanPath(startPath.trimmed());
            if (walkingPath.isEmpty())
            {
                return;
            }

            for (int depthIndex = 0; depthIndex < 8; ++depthIndex)
            {
                appendUniquePath(&candidateRootPaths, walkingPath);
                appendUniquePath(
                    &candidateRootPaths,
                    QDir(walkingPath).absoluteFilePath(QStringLiteral("Ksword5.1")));

                QDir walkingDir(walkingPath);
                if (!walkingDir.cdUp())
                {
                    break;
                }
                walkingPath = QDir::cleanPath(walkingDir.absolutePath());
            }
        };

        appendFromStartPath(QDir::currentPath());
        appendFromStartPath(QCoreApplication::applicationDirPath());
        return candidateRootPaths;
    }

    // resolvePreferredResourceRootPath 作用：
    // - 选出“包含 style 目录”的优先资源根目录；
    // - 保证 style/ksword_background.png 默认相对路径可被稳定解析。
    // 调用方式：读写设置文件、回退路径解析时调用。
    // 返回：资源根目录绝对路径。
    QString resolvePreferredResourceRootPath()
    {
        // candidateRootPaths 作用：候选资源根目录列表。
        const QStringList candidateRootPaths = collectCandidateRootPaths();
        for (const QString& candidateRootPath : candidateRootPaths)
        {
            const QFileInfo styleDirectoryInfo(
                QDir(candidateRootPath).absoluteFilePath(QStringLiteral("style")));
            if (styleDirectoryInfo.exists() && styleDirectoryInfo.isDir())
            {
                return candidateRootPath;
            }
        }

        return QDir::cleanPath(QDir::currentPath());
    }

    // clampOpacityPercent 作用：
    // - 把透明度限制在 0~100，避免非法值污染配置。
    // 调用方式：读写 JSON 前后统一调用。
    // 入参 opacityPercent：待校正透明度值。
    // 返回：校正后的透明度值。
    int clampOpacityPercent(const int opacityPercent)
    {
        if (opacityPercent < 0)
        {
            return 0;
        }
        if (opacityPercent > 100)
        {
            return 100;
        }
        return opacityPercent;
    }

    // resolveRelativePath 作用：
    // - 基于根目录把相对路径转成绝对路径；
    // - 绝对路径保持不变。
    // 调用方式：路径解析内部复用。
    // 入参 rootDirPath：根目录绝对路径；
    // 入参 maybeRelativePath：待解析路径。
    // 返回：解析后的绝对路径。
    QString resolveRelativePath(const QString& rootDirPath, const QString& maybeRelativePath)
    {
        if (maybeRelativePath.isEmpty())
        {
            return QString();
        }
        const QFileInfo inputInfo(maybeRelativePath);
        if (inputInfo.isAbsolute())
        {
            return QDir::cleanPath(maybeRelativePath);
        }
        const QDir rootDir(rootDirPath);
        return QDir::cleanPath(rootDir.absoluteFilePath(maybeRelativePath));
    }

    // resolvePathAgainstCandidateRoots 作用：
    // - 对相对路径做多根目录探测（工作目录/可执行目录/上级目录/Ksword5.1 子目录）；
    // - 首个存在路径即返回，不存在时回退到“优先资源根目录”拼接结果。
    // 调用方式：读取 JSON、加载背景图时调用。
    // 入参 maybeRelativePath：相对或绝对路径文本。
    // 返回：可用于后续文件访问的绝对路径。
    QString resolvePathAgainstCandidateRoots(const QString& maybeRelativePath)
    {
        if (maybeRelativePath.trimmed().isEmpty())
        {
            return QString();
        }

        const QFileInfo inputPathInfo(maybeRelativePath);
        if (inputPathInfo.isAbsolute())
        {
            return QDir::cleanPath(maybeRelativePath);
        }

        // candidateRootPaths 作用：多种启动路径下的候选根目录集合。
        const QStringList candidateRootPaths = collectCandidateRootPaths();
        for (const QString& candidateRootPath : candidateRootPaths)
        {
            const QString resolvedPath = resolveRelativePath(candidateRootPath, maybeRelativePath);
            if (QFileInfo::exists(resolvedPath))
            {
                return resolvedPath;
            }
        }

        // preferredRootPath 作用：当候选集中暂未命中时，提供稳定回退位置。
        const QString preferredRootPath = resolvePreferredResourceRootPath();
        return resolveRelativePath(preferredRootPath, maybeRelativePath);
    }

    // buildDefaultSettings 作用：
    // - 统一创建默认外观设置，避免重复硬编码。
    // 调用方式：读取失败或字段缺失时使用。
    // 返回：默认配置结构体。
    ks::settings::AppearanceSettings buildDefaultSettings()
    {
        ks::settings::AppearanceSettings defaultSettings;
        defaultSettings.themeMode = ks::settings::ThemeMode::FollowSystem;
        defaultSettings.backgroundImagePath = QStringLiteral("style/ksword_background.png");
        defaultSettings.backgroundOpacityPercent = 35;
        return defaultSettings;
    }
}

QString ks::settings::themeModeToJsonText(const ThemeMode mode)
{
    switch (mode)
    {
    case ThemeMode::Light:
        return QStringLiteral("light");
    case ThemeMode::Dark:
        return QStringLiteral("dark");
    case ThemeMode::FollowSystem:
    default:
        return QStringLiteral("follow_system");
    }
}

ks::settings::ThemeMode ks::settings::themeModeFromJsonText(const QString& jsonText)
{
    const QString normalizedText = jsonText.trimmed().toLower();
    if (normalizedText == QStringLiteral("light"))
    {
        return ThemeMode::Light;
    }
    if (normalizedText == QStringLiteral("dark"))
    {
        return ThemeMode::Dark;
    }
    return ThemeMode::FollowSystem;
}

QString ks::settings::appearanceSettingsJsonRelativePath()
{
    return QStringLiteral("style/appearance_settings.json");
}

QString ks::settings::resolveSettingsJsonPathForRead()
{
    const QString relativePath = appearanceSettingsJsonRelativePath();
    return resolvePathAgainstCandidateRoots(relativePath);
}

QString ks::settings::resolveSettingsJsonPathForWrite()
{
    // preferredRootPath 作用：定位包含 style 目录的资源根目录，确保写入路径稳定。
    const QString preferredRootPath = resolvePreferredResourceRootPath();
    return resolveRelativePath(preferredRootPath, appearanceSettingsJsonRelativePath());
}

QString ks::settings::resolveBackgroundImagePathForLoad(const QString& imagePathText)
{
    return resolvePathAgainstCandidateRoots(imagePathText);
}

ks::settings::AppearanceSettings ks::settings::loadAppearanceSettings()
{
    AppearanceSettings loadedSettings = buildDefaultSettings();
    const QString settingsJsonPath = resolveSettingsJsonPathForRead();
    QFile settingsFile(settingsJsonPath);

    if (!settingsFile.exists())
    {
        return loadedSettings;
    }

    if (!settingsFile.open(QIODevice::ReadOnly | QIODevice::Text))
    {
        return loadedSettings;
    }

    const QByteArray jsonBytes = settingsFile.readAll();
    settingsFile.close();

    QJsonParseError parseError;
    const QJsonDocument jsonDocument = QJsonDocument::fromJson(jsonBytes, &parseError);
    if (parseError.error != QJsonParseError::NoError || !jsonDocument.isObject())
    {
        return loadedSettings;
    }

    const QJsonObject rootObject = jsonDocument.object();

    // themeText 作用：读取主题模式字段，缺失时回退默认值。
    const QString themeText = rootObject.value(QStringLiteral("theme_mode"))
        .toString(themeModeToJsonText(loadedSettings.themeMode));
    loadedSettings.themeMode = themeModeFromJsonText(themeText);

    // backgroundPathText 作用：读取背景图路径字段，缺失时使用默认路径。
    const QString backgroundPathText = rootObject.value(QStringLiteral("background_image_path"))
        .toString(loadedSettings.backgroundImagePath);
    loadedSettings.backgroundImagePath = backgroundPathText.trimmed().isEmpty()
        ? QStringLiteral("style/ksword_background.png")
        : backgroundPathText;

    // backgroundOpacityPercentValue 作用：读取透明度字段并做边界校正。
    const int backgroundOpacityPercentValue = rootObject.value(QStringLiteral("background_opacity_percent"))
        .toInt(loadedSettings.backgroundOpacityPercent);
    loadedSettings.backgroundOpacityPercent = clampOpacityPercent(backgroundOpacityPercentValue);

    return loadedSettings;
}

bool ks::settings::saveAppearanceSettings(const AppearanceSettings& settings, QString* errorTextOut)
{
    const QString settingsJsonPath = resolveSettingsJsonPathForWrite();
    const QFileInfo settingsFileInfo(settingsJsonPath);

    // settingsDirPath 作用：配置文件所在目录，保存前先确保目录存在。
    const QString settingsDirPath = settingsFileInfo.absolutePath();
    QDir settingsDir(settingsDirPath);
    if (!settingsDir.exists())
    {
        if (!settingsDir.mkpath(QStringLiteral(".")))
        {
            if (errorTextOut != nullptr)
            {
                *errorTextOut = QStringLiteral("创建设置目录失败: %1").arg(settingsDirPath);
            }
            return false;
        }
    }

    QJsonObject rootObject;
    rootObject.insert(QStringLiteral("theme_mode"), themeModeToJsonText(settings.themeMode));
    rootObject.insert(QStringLiteral("background_image_path"), settings.backgroundImagePath);
    rootObject.insert(QStringLiteral("background_opacity_percent"), clampOpacityPercent(settings.backgroundOpacityPercent));

    const QJsonDocument jsonDocument(rootObject);
    const QByteArray jsonBytes = jsonDocument.toJson(QJsonDocument::Indented);

    QFile settingsFile(settingsJsonPath);
    if (!settingsFile.open(QIODevice::WriteOnly | QIODevice::Truncate | QIODevice::Text))
    {
        if (errorTextOut != nullptr)
        {
            *errorTextOut = QStringLiteral("打开设置文件失败: %1").arg(settingsJsonPath);
        }
        return false;
    }

    const qint64 writtenBytes = settingsFile.write(jsonBytes);
    settingsFile.close();
    if (writtenBytes < 0)
    {
        if (errorTextOut != nullptr)
        {
            *errorTextOut = QStringLiteral("写入设置文件失败: %1").arg(settingsJsonPath);
        }
        return false;
    }

    return true;
}
