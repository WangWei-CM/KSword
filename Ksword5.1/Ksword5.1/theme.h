#pragma once

// ==============================
// theme.h
// 统一管理项目主题色常量，避免各处硬编码颜色值。
// 本文件当前先收敛“欢迎页同款蓝色”及其常用衍生色。
// ==============================

#include <QColor>   // QColor：用于 Qt 绘制与样式着色。
#include <QString>  // QString：用于拼接样式字符串。

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
} // namespace KswordTheme

