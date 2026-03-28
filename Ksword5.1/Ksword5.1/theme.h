#pragma once

// ==============================
// theme.h
// 统一管理项目主题色常量，避免各处硬编码颜色值。
// 本文件当前先收敛“欢迎页同款蓝色”及其常用衍生色。
// ==============================

#include <QColor>   // QColor：用于 Qt 绘制与样式着色。
#include <QString>  // QString：用于拼接样式字符串。
#include <QApplication> // QApplication：读取/写入全局主题状态属性。

namespace KswordTheme
{
    // PrimaryBlueHex：项目主蓝色（欢迎页与主界面同款）。
    // 说明：用户要求“欢迎页出现的那种蓝色”，即 #43A0FF。
    inline const QString PrimaryBlueHex = QStringLiteral("#43A0FF");

    // PrimaryBlueColor：主蓝色 QColor 形式，便于图标重着色与绘图。
    inline const QColor PrimaryBlueColor(67, 160, 255);

    // PrimaryBlueHoverHex：主蓝色悬停背景（浅蓝）。
    inline const QString PrimaryBlueHoverHex = QStringLiteral("#EAF4FF");

    // PrimaryBluePressedHex：主蓝色按下背景（更深蓝）。
    inline const QString PrimaryBluePressedHex = QStringLiteral("#2880DD");

    // PrimaryBlueBorderHex：统一边框蓝（与主蓝一致，便于语义命名）。
    inline const QString PrimaryBlueBorderHex = PrimaryBlueHex;

    // DarkModePropertyKey：应用级主题状态属性键。
    inline const char* DarkModePropertyKey = "ksword_dark_mode_enabled";

    // SetDarkModeEnabled 作用：
    // - 把“当前是否深色模式”写入 QApplication 属性；
    // - 供各 Dock 在绘制颜色/样式时统一读取。
    // 调用方式：MainWindow 每次应用主题后调用。
    // 入参 enabled：true=深色模式，false=浅色模式。
    inline void SetDarkModeEnabled(const bool enabled)
    {
        if (qApp != nullptr)
        {
            qApp->setProperty(DarkModePropertyKey, enabled);
        }
    }

    // IsDarkModeEnabled 作用：
    // - 读取 QApplication 中记录的主题状态；
    // - 若应用尚未初始化则回退 false。
    // 调用方式：任意模块按需调用，做深浅色分支。
    // 返回：true=深色模式；false=浅色模式。
    inline bool IsDarkModeEnabled()
    {
        if (qApp == nullptr)
        {
            return false;
        }
        return qApp->property(DarkModePropertyKey).toBool();
    }

    // SurfaceHex 作用：返回基础面板背景色（浅色白、深色深灰）。
    inline QString SurfaceHex()
    {
        return QStringLiteral("palette(base)");
    }

    // SurfaceAltHex 作用：返回次级面板背景色（用于 hover/分隔块）。
    inline QString SurfaceAltHex()
    {
        return QStringLiteral("palette(alternate-base)");
    }

    // BorderHex 作用：返回中性边框色，适配深浅色。
    inline QString BorderHex()
    {
        return QStringLiteral("palette(mid)");
    }

    // TextPrimaryHex 作用：返回主文本色（深色白字，浅色深灰字）。
    inline QString TextPrimaryHex()
    {
        return QStringLiteral("palette(text)");
    }

    // TextSecondaryHex 作用：返回次级文本色（说明文字、状态文字）。
    inline QString TextSecondaryHex()
    {
        return QStringLiteral("palette(mid)");
    }

    // NewRowBackgroundColor 作用：返回“新增行”高亮色（深色为墨绿色）。
    inline QColor NewRowBackgroundColor()
    {
        return IsDarkModeEnabled() ? QColor(34, 66, 44) : QColor(218, 255, 226);
    }

    // ExitedRowBackgroundColor 作用：返回“退出行”背景色（深色为深灰）。
    inline QColor ExitedRowBackgroundColor()
    {
        return IsDarkModeEnabled() ? QColor(52, 52, 52) : QColor(236, 236, 236);
    }

    // ExitedRowForegroundColor 作用：返回“退出行”文字色。
    inline QColor ExitedRowForegroundColor()
    {
        return IsDarkModeEnabled() ? QColor(170, 170, 170) : QColor(88, 88, 88);
    }

    // WarningAccentColor 作用：返回警告文本色，深色下更高亮。
    inline QColor WarningAccentColor()
    {
        return IsDarkModeEnabled() ? QColor(255, 200, 120) : QColor(166, 52, 52);
    }
} // namespace KswordTheme
