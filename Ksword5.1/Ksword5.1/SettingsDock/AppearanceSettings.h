#pragma once

// ============================================================
// AppearanceSettings.h
// 作用：
// - 定义“外观设置”数据结构（主题模式、背景图路径、透明度）；
// - 定义 JSON 读写与路径解析函数，供 SettingsDock/MainWindow 复用；
// - 统一默认值，避免多处硬编码。
// ============================================================

#include <QString>

namespace ks::settings
{
    // ThemeMode：主题模式枚举。
    // FollowSystem：跟随系统；Light：浅色；Dark：深色。
    enum class ThemeMode
    {
        FollowSystem = 0,
        Light = 1,
        Dark = 2
    };

    // AppearanceSettings：外观设置结构体。
    // themeMode：当前主题策略；
    // backgroundImagePath：背景图路径（可相对可绝对）；
    // backgroundOpacityPercent：背景图透明度（0~100）。
    struct AppearanceSettings
    {
        ThemeMode themeMode = ThemeMode::FollowSystem;
        QString backgroundImagePath = QStringLiteral("style/ksword_background.png");
        int backgroundOpacityPercent = 35;
    };

    // themeModeToJsonText 作用：
    // - 把主题枚举转成 JSON 存档文本。
    // 调用方式：保存配置前调用。
    // 入参 mode：主题枚举值。
    // 返回：可写入 JSON 的字符串。
    QString themeModeToJsonText(ThemeMode mode);

    // themeModeFromJsonText 作用：
    // - 把 JSON 文本还原为主题枚举。
    // 调用方式：读取配置后调用。
    // 入参 jsonText：JSON 中的主题字段。
    // 返回：解析后的主题枚举，非法值回退 FollowSystem。
    ThemeMode themeModeFromJsonText(const QString& jsonText);

    // appearanceSettingsJsonRelativePath 作用：
    // - 返回外观配置 JSON 的默认相对路径。
    // 调用方式：构建读写路径时调用。
    // 返回：例如 style/appearance_settings.json。
    QString appearanceSettingsJsonRelativePath();

    // resolveSettingsJsonPathForRead 作用：
    // - 获取读取 JSON 时优先使用的绝对路径。
    // 调用方式：loadAppearanceSettings 内部调用。
    // 返回：最终用于读文件的绝对路径。
    QString resolveSettingsJsonPathForRead();

    // resolveSettingsJsonPathForWrite 作用：
    // - 获取保存 JSON 时使用的绝对路径。
    // 调用方式：saveAppearanceSettings 内部调用。
    // 返回：最终用于写文件的绝对路径。
    QString resolveSettingsJsonPathForWrite();

    // resolveBackgroundImagePathForLoad 作用：
    // - 把“用户输入路径”解析为可加载图片的绝对路径。
    // 调用方式：MainWindow 应用背景图时调用。
    // 入参 imagePathText：配置中的路径文本（相对或绝对）。
    // 返回：绝对路径（若原路径为空则返回空字符串）。
    QString resolveBackgroundImagePathForLoad(const QString& imagePathText);

    // loadAppearanceSettings 作用：
    // - 从 JSON 读取外观设置；
    // - 文件不存在或解析失败时回退默认值。
    // 调用方式：程序启动时调用。
    // 返回：外观配置结构体。
    AppearanceSettings loadAppearanceSettings();

    // saveAppearanceSettings 作用：
    // - 把外观设置写入 JSON 文件（自动创建目录）。
    // 调用方式：用户在设置页修改后调用。
    // 入参 settings：待保存配置；
    // 入参 errorTextOut：可选错误文本输出指针。
    // 返回：true=保存成功；false=保存失败。
    bool saveAppearanceSettings(const AppearanceSettings& settings, QString* errorTextOut = nullptr);
}

