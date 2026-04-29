#pragma once

// ==============================
// theme.h
// 统一管理项目主题色常量，避免各处硬编码颜色值。
// 本文件收敛全局深浅主题色板、主色与常用语义色，避免各处硬编码颜色值。
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

    // PrimaryBlueHoverHex：主蓝色弱悬停背景（浅蓝）。
    inline const QString PrimaryBlueHoverHex = QStringLiteral("#DFF0FF");

    // PrimaryBluePressedHex：主蓝色按下背景（更深蓝）。
    inline const QString PrimaryBluePressedHex = QStringLiteral("#1F7FD9");

    // PrimaryBlueBorderHex：统一边框蓝（与主蓝一致，便于语义命名）。
    inline const QString PrimaryBlueBorderHex = PrimaryBlueHex;

    // PrimaryBlueActiveHex：主色实底悬停色，适合蓝底白字控件。
    inline const QString PrimaryBlueActiveHex = QStringLiteral("#2F92FF");

    // PrimaryBlueSubtleDarkHex：深色模式下主色弱背景。
    inline const QString PrimaryBlueSubtleDarkHex = QStringLiteral("#173553");

    // PrimaryBlueSubtleLightHex：浅色模式下主色弱背景。
    inline const QString PrimaryBlueSubtleLightHex = QStringLiteral("#EAF4FF");

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

    // WindowColor 作用：返回应用窗口底色。
    inline QColor WindowColor()
    {
        return IsDarkModeEnabled() ? QColor(10, 15, 22) : QColor(248, 251, 255);
    }

    // SurfaceColor 作用：返回基础内容面板色。
    inline QColor SurfaceColor()
    {
        return IsDarkModeEnabled() ? QColor(17, 25, 36) : QColor(255, 255, 255);
    }

    // SurfaceAltColor 作用：返回次级内容面板色。
    inline QColor SurfaceAltColor()
    {
        return IsDarkModeEnabled() ? QColor(24, 35, 50) : QColor(243, 248, 255);
    }

    // SurfaceMutedColor 作用：返回弱层次背景色。
    inline QColor SurfaceMutedColor()
    {
        return IsDarkModeEnabled() ? QColor(30, 43, 60) : QColor(226, 241, 255);
    }

    // BorderColor 作用：返回中性边框色。
    inline QColor BorderColor()
    {
        return IsDarkModeEnabled() ? QColor(55, 80, 106) : QColor(190, 211, 233);
    }

    // BorderStrongColor 作用：返回强调边框色。
    inline QColor BorderStrongColor()
    {
        return IsDarkModeEnabled() ? QColor(72, 105, 138) : QColor(151, 190, 231);
    }

    // TextPrimaryColor 作用：返回主文本色。
    inline QColor TextPrimaryColor()
    {
        return IsDarkModeEnabled() ? QColor(237, 246, 255) : QColor(16, 35, 54);
    }

    // TextSecondaryColor 作用：返回次级文本色。
    inline QColor TextSecondaryColor()
    {
        return IsDarkModeEnabled() ? QColor(179, 198, 218) : QColor(79, 99, 120);
    }

    // TextDisabledColor 作用：返回禁用文本色。
    inline QColor TextDisabledColor()
    {
        return IsDarkModeEnabled() ? QColor(124, 146, 169) : QColor(126, 142, 160);
    }

    // ThemeColorName 作用：统一 QColor -> #RRGGBB 文本。
    inline QString ThemeColorName(const QColor& colorValue)
    {
        return colorValue.name(QColor::HexRgb).toUpper();
    }

    inline QString WindowColorHex()
    {
        return ThemeColorName(WindowColor());
    }

    inline QString SurfaceColorHex()
    {
        return ThemeColorName(SurfaceColor());
    }

    inline QString SurfaceAltColorHex()
    {
        return ThemeColorName(SurfaceAltColor());
    }

    inline QString SurfaceMutedColorHex()
    {
        return ThemeColorName(SurfaceMutedColor());
    }

    inline QString BorderColorHex()
    {
        return ThemeColorName(BorderColor());
    }

    inline QString BorderStrongColorHex()
    {
        return ThemeColorName(BorderStrongColor());
    }

    inline QString TextPrimaryColorHex()
    {
        return ThemeColorName(TextPrimaryColor());
    }

    inline QString TextSecondaryColorHex()
    {
        return ThemeColorName(TextSecondaryColor());
    }

    inline QString TextDisabledColorHex()
    {
        return ThemeColorName(TextDisabledColor());
    }

    // PrimaryBlueSubtleHex 作用：返回深浅色适配的主色弱背景。
    inline QString PrimaryBlueSubtleHex()
    {
        return IsDarkModeEnabled() ? PrimaryBlueSubtleDarkHex : PrimaryBlueSubtleLightHex;
    }

    // PrimaryBlueSolidHoverHex 作用：返回蓝底白字控件的悬停色。
    inline QString PrimaryBlueSolidHoverHex()
    {
        return PrimaryBlueActiveHex;
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

    // ThemedButtonStyle 作用：生成统一按钮样式，按深浅色输出可读前景/背景。
    inline QString ThemedButtonStyle()
    {
        return QStringLiteral(
            "QPushButton,QToolButton{"
            "  background-color:%1 !important;"
            "  color:%2 !important;"
            "  border:1px solid %3 !important;"
            "  border-radius:3px;"
            "  padding:4px 10px;"
            "  font-weight:600;"
            "}"
            "QPushButton:hover,QToolButton:hover{"
            "  background-color:%4 !important;"
            "  color:#FFFFFF !important;"
            "  border-color:%4 !important;"
            "}"
            "QPushButton:pressed,QToolButton:pressed{"
            "  background-color:%5 !important;"
            "  color:#FFFFFF !important;"
            "  border-color:%5 !important;"
            "}"
            "QPushButton:disabled,QToolButton:disabled{"
            "  background-color:%6 !important;"
            "  color:%7 !important;"
            "  border-color:%3 !important;"
            "}")
            .arg(SurfaceAltHex())
            .arg(TextPrimaryHex())
            .arg(BorderHex())
            .arg(PrimaryBlueSolidHoverHex())
            .arg(PrimaryBluePressedHex)
            .arg(SurfaceAltHex())
            .arg(TextSecondaryHex());
    }

    // ContextMenuStyle 作用：
    // - 统一生成右键菜单样式，确保菜单背景始终显式填充；
    // - 重点规避浅色模式下菜单继承透明背景而显示黑底、文字不可读的问题。
    // 调用方式：menu.setStyleSheet(KswordTheme::ContextMenuStyle())。
    // 返回：可直接应用到 QMenu 的样式文本。
    inline QString ContextMenuStyle()
    {
        const QString menuBackgroundColor = SurfaceColorHex();
        const QString menuTextColor = TextPrimaryColorHex();
        const QString menuBorderColor = BorderColorHex();
        const QString disabledTextColor = TextDisabledColorHex();

        return QStringLiteral(
            "QMenu{"
            "  background-color:%1 !important;"
            "  color:%2 !important;"
            "  border:1px solid %3 !important;"
            "  padding:3px;"
            "}"
            "QMenu::item{"
            "  color:%2 !important;"
            "  padding:5px 18px 5px 14px;"
            "  background-color:transparent !important;"
            "}"
            "QMenu::item:selected{"
            "  background-color:%4 !important;"
            "  color:#FFFFFF !important;"
            "}"
            "QMenu::item:disabled{"
            "  color:%5 !important;"
            "  background-color:transparent !important;"
            "}"
            "QMenu::separator{"
            "  height:1px;"
            "  background-color:%3;"
            "  margin:2px 6px;"
            "}")
            .arg(menuBackgroundColor)
            .arg(menuTextColor)
            .arg(menuBorderColor)
            .arg(PrimaryBlueHex)
            .arg(disabledTextColor);
    }

    // OpaqueDialogStyle 作用：
    // - 为独立弹窗显式设置不透明背景，避免父级透明样式导致黑底；
    // - 主要用于“详情查看”类弹窗（如启动项详情、ETW 返回详情）。
    // 调用方式：dialog.setStyleSheet(KswordTheme::OpaqueDialogStyle(dialog.objectName()))。
    // 入参 dialogObjectName：目标 QDialog 的 objectName。
    // 返回：可直接应用到 QDialog 的样式文本；objectName 为空时返回空串。
    inline QString OpaqueDialogStyle(const QString& dialogObjectName)
    {
        if (dialogObjectName.trimmed().isEmpty())
        {
            return QString();
        }

        return QStringLiteral(
            "QDialog#%1{"
            "  background-color:palette(window) !important;"
            "  color:palette(text) !important;"
            "}"
            "QDialog#%1 QPlainTextEdit,"
            "QDialog#%1 QTextEdit,"
            "QDialog#%1 QTreeWidget,"
            "QDialog#%1 QTableWidget,"
            "QDialog#%1 QAbstractScrollArea,"
            "QDialog#%1 QAbstractScrollArea::viewport{"
            "  background-color:palette(base) !important;"
            "  color:palette(text) !important;"
            "}"
            "QDialog#%1 QHeaderView::section{"
            "  background-color:palette(window) !important;"
            "  color:palette(text) !important;"
            "}"
            "QDialog#%1 QMenu{"
            "  background-color:palette(base) !important;"
            "  color:palette(text) !important;"
            "  border:1px solid palette(mid) !important;"
            "}"
            "QDialog#%1 QMenu::item:selected{"
            "  background-color:%2 !important;"
            "  color:#FFFFFF !important;"
            "}"
            "QDialog#%1 QMenu::separator{"
            "  height:1px;"
            "  background-color:palette(mid) !important;"
            "}")
            .arg(dialogObjectName)
            .arg(PrimaryBlueHex);
    }

    // NewRowBackgroundColor 作用：返回“新增行”高亮色（深色为墨绿色）。
    inline QColor NewRowBackgroundColor()
    {
        return IsDarkModeEnabled() ? QColor(30, 70, 48) : QColor(218, 255, 226);
    }

    // ExitedRowBackgroundColor 作用：返回“退出行”背景色（深色为深灰）。
    inline QColor ExitedRowBackgroundColor()
    {
        return IsDarkModeEnabled() ? QColor(43, 53, 64) : QColor(236, 242, 248);
    }

    // ExitedRowForegroundColor 作用：返回“退出行”文字色。
    inline QColor ExitedRowForegroundColor()
    {
        return IsDarkModeEnabled() ? QColor(172, 190, 208) : QColor(88, 100, 114);
    }

    // WarningAccentColor 作用：返回警告文本色，深色下更高亮。
    inline QColor WarningAccentColor()
    {
        return IsDarkModeEnabled() ? QColor(255, 200, 120) : QColor(166, 52, 52);
    }
} // namespace KswordTheme
