#include "AppearanceSettings.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QStringList>

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>

#include <cmath>

namespace
{
    // kPrimaryStyleDirectoryName / kFallbackStyleDirectoryName 作用：
    // - 同时兼容发行版常见的 "Style" 目录与开发期历史遗留的 "style" 目录；
    // - 读写配置时优先使用发行版目录名，避免把客户配置写回源码树。
    constexpr auto kPrimaryStyleDirectoryName = "Style";
    constexpr auto kFallbackStyleDirectoryName = "style";

    // appendUniquePath 作用：
    // - 向候选路径列表追加“去重后的规范路径”；
    // - 使用大小写不敏感比较，兼容 Windows 路径规则。
    // 调用方式：各类候选路径收集函数内部调用。
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

    // resolveExecutableDirectoryPath 作用：
    // - 在 QApplication 尚未创建时，仍可直接拿到当前 exe 所在目录；
    // - 供启动前读取配置文件时参与候选路径探测。
    // 返回：exe 所在目录绝对路径；失败时返回空字符串。
    QString resolveExecutableDirectoryPath()
    {
        wchar_t executablePathBuffer[MAX_PATH] = {};
        const DWORD pathLength = ::GetModuleFileNameW(nullptr, executablePathBuffer, MAX_PATH);
        if (pathLength == 0 || pathLength >= MAX_PATH)
        {
            return QString();
        }

        // executablePathText 作用：承载当前进程 exe 的完整绝对路径文本。
        const QString executablePathText = QString::fromWCharArray(executablePathBuffer, static_cast<int>(pathLength));
        return QFileInfo(executablePathText).absolutePath();
    }

    // directoryContainsStyleFolder 作用：
    // - 判断指定根目录下是否存在 Style/style 目录；
    // - 用于发行版根目录探测与资源根目录选择。
    // 入参 rootDirectoryPath：待检查的根目录。
    // 返回：true=存在样式目录；false=不存在。
    bool directoryContainsStyleFolder(const QString& rootDirectoryPath)
    {
        const QDir rootDirectory(rootDirectoryPath);
        const QFileInfo primaryStyleInfo(rootDirectory.absoluteFilePath(QString::fromLatin1(kPrimaryStyleDirectoryName)));
        if (primaryStyleInfo.exists() && primaryStyleInfo.isDir())
        {
            return true;
        }

        const QFileInfo fallbackStyleInfo(rootDirectory.absoluteFilePath(QString::fromLatin1(kFallbackStyleDirectoryName)));
        return fallbackStyleInfo.exists() && fallbackStyleInfo.isDir();
    }

    // resolveStyleDirectoryNameForRoot 作用：
    // - 在给定根目录下确定实际使用的样式目录名；
    // - 发行版优先 "Style"，开发期回退 "style"。
    // 入参 rootDirectoryPath：样式资源根目录。
    // 返回：目录名；若都不存在则默认返回 "Style"。
    QString resolveStyleDirectoryNameForRoot(const QString& rootDirectoryPath)
    {
        const QDir rootDirectory(rootDirectoryPath);
        const QFileInfo primaryStyleInfo(rootDirectory.absoluteFilePath(QString::fromLatin1(kPrimaryStyleDirectoryName)));
        if (primaryStyleInfo.exists() && primaryStyleInfo.isDir())
        {
            return QString::fromLatin1(kPrimaryStyleDirectoryName);
        }

        const QFileInfo fallbackStyleInfo(rootDirectory.absoluteFilePath(QString::fromLatin1(kFallbackStyleDirectoryName)));
        if (fallbackStyleInfo.exists() && fallbackStyleInfo.isDir())
        {
            return QString::fromLatin1(kFallbackStyleDirectoryName);
        }
        return QString::fromLatin1(kPrimaryStyleDirectoryName);
    }

    // collectExecutableNearbyRootPaths 作用：
    // - 生成“以 exe 所在目录为中心”的候选根目录列表；
    // - 首项永远是 exe 当前目录，保证客户版固定优先使用 exe 同级配置；
    // - 后续再向上回溯若干层，兼容历史包把 Style/style 放在上级目录的情况。
    // 返回：按优先级排序的 exe 邻近候选根目录。
    QStringList collectExecutableNearbyRootPaths()
    {
        QStringList preferredRootPaths;
        QString walkingPath = QDir::cleanPath(resolveExecutableDirectoryPath());
        if (walkingPath.isEmpty())
        {
            return preferredRootPaths;
        }

        appendUniquePath(&preferredRootPaths, walkingPath);
        for (int depthIndex = 0; depthIndex < 8; ++depthIndex)
        {
            if (depthIndex > 0)
            {
                appendUniquePath(&preferredRootPaths, walkingPath);
            }

            QDir walkingDirectory(walkingPath);
            if (!walkingDirectory.cdUp())
            {
                break;
            }
            walkingPath = QDir::cleanPath(walkingDirectory.absolutePath());
        }
        return preferredRootPaths;
    }

    // collectDevelopmentFallbackRootPaths 作用：
    // - 生成开发期兼容读取用的回退根目录集合；
    // - 仅用于“读旧配置/旧背景图”，避免客户版没有源码时误命中工程目录；
    // - 不参与写入目标决策。
    // 返回：按优先级排序的开发期回退根目录列表。
    QStringList collectDevelopmentFallbackRootPaths()
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

        // 只保留开发期常见起点，供兼容读取旧版 style 目录。
        appendFromStartPath(QDir::currentPath());
        appendFromStartPath(resolveExecutableDirectoryPath());
        appendFromStartPath(QCoreApplication::applicationDirPath());
        return candidateRootPaths;
    }

    // resolveExecutablePrimaryRootPath 作用：
    // - 返回“客户配置应固定落点”的主根目录；
    // - 这里直接使用当前 exe 所在目录，不再把源码树作为默认写入位置。
    // 返回：exe 所在目录；极端失败时回退 applicationDirPath/currentPath。
    QString resolveExecutablePrimaryRootPath()
    {
        const QString executableDirectoryPath = QDir::cleanPath(resolveExecutableDirectoryPath());
        if (!executableDirectoryPath.isEmpty())
        {
            return executableDirectoryPath;
        }

        const QString applicationDirectoryPath = QDir::cleanPath(QCoreApplication::applicationDirPath());
        if (!applicationDirectoryPath.isEmpty())
        {
            return applicationDirectoryPath;
        }

        return QDir::cleanPath(QDir::currentPath());
    }

    // resolvePreferredReadableRootPath 作用：
    // - 选出“读取配置/资源时”优先使用的根目录；
    // - 首先固定检查 exe 当前目录，然后检查 exe 上级目录的历史布局；
    // - 最后才允许回退到开发期源码树中的 style 目录。
    // 调用方式：读取设置文件、解析相对资源路径时调用。
    // 返回：读取侧优先根目录绝对路径。
    QString resolvePreferredReadableRootPath()
    {
        const QStringList executableNearbyRootPaths = collectExecutableNearbyRootPaths();
        for (const QString& preferredRootPath : executableNearbyRootPaths)
        {
            if (directoryContainsStyleFolder(preferredRootPath))
            {
                return preferredRootPath;
            }
        }

        // candidateRootPaths 作用：开发期兼容读取的回退根目录列表。
        const QStringList candidateRootPaths = collectDevelopmentFallbackRootPaths();
        for (const QString& candidateRootPath : candidateRootPaths)
        {
            if (directoryContainsStyleFolder(candidateRootPath))
            {
                return candidateRootPath;
            }
        }

        return resolveExecutablePrimaryRootPath();
    }

    // resolvePreferredWritableRootPath 作用：
    // - 选出“保存配置时”必须落点的根目录；
    // - 始终固定到 exe 当前目录，避免把客户配置写回源码树。
    // 返回：写入侧根目录绝对路径。
    QString resolvePreferredWritableRootPath()
    {
        return resolveExecutablePrimaryRootPath();
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

    // clampWindowScaleFactorInternal 作用：
    // - 把窗口缩放因子约束到 0.50~2.00；
    // - 非法输入回退到 1.0。
    // 调用方式：配置读写与启动前应用缩放时调用。
    // 入参 rawScaleFactor：原始缩放因子。
    // 返回：合法缩放因子。
    double clampWindowScaleFactorInternal(const double rawScaleFactor)
    {
        if (!std::isfinite(rawScaleFactor) || rawScaleFactor <= 0.0)
        {
            return 1.0;
        }
        if (rawScaleFactor < 0.50)
        {
            return 0.50;
        }
        if (rawScaleFactor > 2.00)
        {
            return 2.00;
        }
        return rawScaleFactor;
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
    // - 对相对路径做“exe 邻近目录优先、开发目录回退”的多根目录探测；
    // - 首个存在路径即返回，不存在时回退到“可写根目录”拼接结果。
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

        // candidateRootPaths 作用：读取侧候选根目录集合，先客户目录，后开发目录。
        QStringList candidateRootPaths = collectExecutableNearbyRootPaths();
        const QStringList fallbackRootPaths = collectDevelopmentFallbackRootPaths();
        for (const QString& fallbackRootPath : fallbackRootPaths)
        {
            appendUniquePath(&candidateRootPaths, fallbackRootPath);
        }

        for (const QString& candidateRootPath : candidateRootPaths)
        {
            const QString resolvedPath = resolveRelativePath(candidateRootPath, maybeRelativePath);
            if (QFileInfo::exists(resolvedPath))
            {
                return resolvedPath;
            }
        }

        // preferredRootPath 作用：当候选集中暂未命中时，提供稳定且可写的落点。
        const QString preferredRootPath = resolvePreferredWritableRootPath();
        return resolveRelativePath(preferredRootPath, maybeRelativePath);
    }

    // buildDefaultSettings 作用：
    // - 统一创建默认界面与启动设置，避免重复硬编码。
    // 调用方式：读取失败或字段缺失时使用。
    // 返回：默认配置结构体。
    ks::settings::AppearanceSettings buildDefaultSettings()
    {
        ks::settings::AppearanceSettings defaultSettings;
        defaultSettings.themeMode = ks::settings::ThemeMode::FollowSystem;
        defaultSettings.backgroundImagePath = QStringLiteral("Style/ksword_background.png");
        defaultSettings.backgroundOpacityPercent = 35;
        defaultSettings.startupDefaultTabKey = QStringLiteral("welcome");
        defaultSettings.launchMaximizedOnStartup = false;
        defaultSettings.autoRequestAdminOnStartup = false;
        defaultSettings.startupWindowScaleFactor = 1.0;
        defaultSettings.startupScaleRecommendPromptDisabled = false;
        defaultSettings.unlockerShellContextMenuEnabled = false;
        defaultSettings.useWideScrollBars = false;
        defaultSettings.scrollBarAutoHideEnabled = false;
        defaultSettings.sliderWheelAdjustEnabled = false;
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
    const QString preferredRootPath = resolvePreferredWritableRootPath();
    const QString styleDirectoryName = resolveStyleDirectoryNameForRoot(preferredRootPath);
    return styleDirectoryName + QStringLiteral("/appearance_settings.json");
}

QString ks::settings::resolveSettingsJsonPathForRead()
{
    const QString relativePath = appearanceSettingsJsonRelativePath();
    return resolvePathAgainstCandidateRoots(relativePath);
}

bool ks::settings::settingsJsonFileExistsForRead()
{
    return QFileInfo::exists(resolveSettingsJsonPathForRead());
}

QString ks::settings::resolveSettingsJsonPathForWrite()
{
    // preferredRootPath 作用：固定写入到 exe 同级目录，避免客户配置误写回源码树。
    const QString preferredRootPath = resolvePreferredWritableRootPath();
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
        ? (resolveStyleDirectoryNameForRoot(resolvePreferredReadableRootPath()) + QStringLiteral("/ksword_background.png"))
        : backgroundPathText;

    // backgroundOpacityPercentValue 作用：读取透明度字段并做边界校正。
    const int backgroundOpacityPercentValue = rootObject.value(QStringLiteral("background_opacity_percent"))
        .toInt(loadedSettings.backgroundOpacityPercent);
    loadedSettings.backgroundOpacityPercent = clampOpacityPercent(backgroundOpacityPercentValue);

    // startupDefaultTabKeyText 作用：读取启动默认页签字段，缺失或空值时回退 welcome。
    const QString startupDefaultTabKeyText = rootObject.value(QStringLiteral("startup_default_tab_key"))
        .toString(loadedSettings.startupDefaultTabKey)
        .trimmed()
        .toLower();
    loadedSettings.startupDefaultTabKey = startupDefaultTabKeyText.isEmpty()
        ? QStringLiteral("welcome")
        : startupDefaultTabKeyText;

    // launchMaximizedOnStartup 作用：读取“启动时最大化”开关，并兼容旧版 startup_full_screen 字段。
    if (rootObject.contains(QStringLiteral("startup_maximized")))
    {
        loadedSettings.launchMaximizedOnStartup = rootObject.value(QStringLiteral("startup_maximized"))
            .toBool(loadedSettings.launchMaximizedOnStartup);
    }
    else
    {
        loadedSettings.launchMaximizedOnStartup = rootObject.value(QStringLiteral("startup_full_screen"))
            .toBool(loadedSettings.launchMaximizedOnStartup);
    }

    // autoRequestAdminOnStartup 作用：读取“启动时自动请求管理员权限”开关，缺失时回退 false。
    loadedSettings.autoRequestAdminOnStartup = rootObject.value(QStringLiteral("startup_auto_request_admin"))
        .toBool(loadedSettings.autoRequestAdminOnStartup);

    // startupWindowScaleFactor 作用：读取“启动窗口缩放因子”，兼容旧字段 window_scale_factor。
    double rawWindowScaleFactor = loadedSettings.startupWindowScaleFactor;
    if (rootObject.contains(QStringLiteral("startup_window_scale_factor")))
    {
        rawWindowScaleFactor = rootObject.value(QStringLiteral("startup_window_scale_factor"))
            .toDouble(loadedSettings.startupWindowScaleFactor);
    }
    else if (rootObject.contains(QStringLiteral("window_scale_factor")))
    {
        rawWindowScaleFactor = rootObject.value(QStringLiteral("window_scale_factor"))
            .toDouble(loadedSettings.startupWindowScaleFactor);
    }
    loadedSettings.startupWindowScaleFactor = clampWindowScaleFactorInternal(rawWindowScaleFactor);

    // startupScaleRecommendPromptDisabled 作用：读取“小屏推荐缩放提示不再弹出”开关。
    loadedSettings.startupScaleRecommendPromptDisabled = rootObject
        .value(QStringLiteral("startup_scale_recommend_prompt_disabled"))
        .toBool(loadedSettings.startupScaleRecommendPromptDisabled);
    loadedSettings.unlockerShellContextMenuEnabled = rootObject
        .value(QStringLiteral("unlocker_shell_context_menu_enabled"))
        .toBool(loadedSettings.unlockerShellContextMenuEnabled);
    loadedSettings.useWideScrollBars = rootObject
        .value(QStringLiteral("use_wide_scroll_bars"))
        .toBool(loadedSettings.useWideScrollBars);
    loadedSettings.scrollBarAutoHideEnabled = rootObject
        .value(QStringLiteral("scroll_bar_auto_hide_enabled"))
        .toBool(loadedSettings.scrollBarAutoHideEnabled);
    loadedSettings.sliderWheelAdjustEnabled = rootObject
        .value(QStringLiteral("slider_wheel_adjust_enabled"))
        .toBool(loadedSettings.sliderWheelAdjustEnabled);

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
    rootObject.insert(
        QStringLiteral("startup_default_tab_key"),
        settings.startupDefaultTabKey.trimmed().isEmpty()
        ? QStringLiteral("welcome")
        : settings.startupDefaultTabKey.trimmed().toLower());
    rootObject.insert(QStringLiteral("startup_maximized"), settings.launchMaximizedOnStartup);
    rootObject.insert(QStringLiteral("startup_auto_request_admin"), settings.autoRequestAdminOnStartup);
    rootObject.insert(
        QStringLiteral("startup_window_scale_factor"),
        clampWindowScaleFactorInternal(settings.startupWindowScaleFactor));
    rootObject.insert(
        QStringLiteral("startup_scale_recommend_prompt_disabled"),
        settings.startupScaleRecommendPromptDisabled);
    rootObject.insert(
        QStringLiteral("unlocker_shell_context_menu_enabled"),
        settings.unlockerShellContextMenuEnabled);
    rootObject.insert(
        QStringLiteral("use_wide_scroll_bars"),
        settings.useWideScrollBars);
    rootObject.insert(
        QStringLiteral("scroll_bar_auto_hide_enabled"),
        settings.scrollBarAutoHideEnabled);
    rootObject.insert(
        QStringLiteral("slider_wheel_adjust_enabled"),
        settings.sliderWheelAdjustEnabled);

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

double ks::settings::normalizeWindowScaleFactor(const double rawScaleFactor)
{
    return clampWindowScaleFactorInternal(rawScaleFactor);
}
