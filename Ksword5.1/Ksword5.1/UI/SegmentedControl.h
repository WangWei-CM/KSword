#pragma once

#include <QList>
class QAbstractButton;
class QBoxLayout;
class QTabBar;
class QWidget;

namespace ks::ui
{
    // 仅用于页面明确登记的互斥选项；保留原按钮、信号、提示、启用状态及自定义空选态。
    // layout 中须直接包含 buttons；返回统一圆角底面与滑动选中面的宿主，不扫描页面。
    QWidget* InstallSegmentedButtons(QBoxLayout* layout, const QList<QAbstractButton*>& buttons);
    // 紧凑范围页签共用相同绘制与动画；QTabBar 保留原索引和键盘/鼠标处理。
    void InstallSegmentedTabBar(QTabBar* bar);
    // 旧列预设仅用 QSS 表示选择时明确登记状态；不依赖样式文本推断选项语义。
    void SetSegmentedButtonSelected(QAbstractButton* button, bool selected);
}
