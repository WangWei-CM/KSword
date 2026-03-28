#include "AppearanceSettings.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>

namespace
{
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

    // firstCandidatePath 作用：优先在“当前工作目录”读取，便于开发态调试。
    const QString firstCandidatePath = resolveRelativePath(QDir::currentPath(), relativePath);
    if (QFileInfo::exists(firstCandidatePath))
    {
        return firstCandidatePath;
    }

    // secondCandidatePath 作用：工作目录无配置时，回退到可执行文件目录读取。
    const QString secondCandidatePath = resolveRelativePath(QCoreApplication::applicationDirPath(), relativePath);
    return secondCandidatePath;
}

QString ks::settings::resolveSettingsJsonPathForWrite()
{
    // 为了让用户能在项目目录直接看到配置，统一写入当前工作目录。
    return resolveRelativePath(QDir::currentPath(), appearanceSettingsJsonRelativePath());
}

QString ks::settings::resolveBackgroundImagePathForLoad(const QString& imagePathText)
{
    if (imagePathText.trimmed().isEmpty())
    {
        return QString();
    }

    const QFileInfo rawPathInfo(imagePathText);
    if (rawPathInfo.isAbsolute())
    {
        return QDir::cleanPath(imagePathText);
    }

    // firstCandidatePath 作用：优先按“当前工作目录”解析相对路径。
    const QString firstCandidatePath = resolveRelativePath(QDir::currentPath(), imagePathText);
    if (QFileInfo::exists(firstCandidatePath))
    {
        return firstCandidatePath;
    }

    // secondCandidatePath 作用：回退到“可执行文件目录”解析相对路径。
    const QString secondCandidatePath = resolveRelativePath(QCoreApplication::applicationDirPath(), imagePathText);
    return secondCandidatePath;
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

