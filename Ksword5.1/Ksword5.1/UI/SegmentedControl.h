#pragma once

#include <QList>
#include <QRectF>
#include <QIcon>
#include <QString>
#include <functional>
class QAbstractButton;
class QBoxLayout;
class QTabBar;
class QWidget;

namespace ks::ui
{
    struct SegmentedSurfaceItem
    {
        QRectF bounds;       // 使用原控件的命中矩形，不能另建一套点击映射。
        QString text;        // 原控件当前语言的完整段文字。
        QIcon icon;          // 可选单色工具图形。
        bool enabled = true; // 原控件独立段禁用状态。
    };
    struct SegmentedSurfaceSnapshot
    {
        QList<SegmentedSurfaceItem> items;
        int selected = -1; // -1 保留自定义/空选态。
    };
    // 仅用于页面明确登记的互斥选项；保留原按钮、信号、提示、启用状态及自定义空选态。
    // layout 中须直接包含 buttons；返回统一圆角底面与滑动选中面的宿主，不扫描页面。
    QWidget* InstallSegmentedButtons(QBoxLayout* layout, const QList<QAbstractButton*>& buttons);
    // 紧凑范围页签共用相同绘制与动画；QTabBar 保留原索引和键盘/鼠标处理。
    void InstallSegmentedTabBar(QTabBar* bar);
    // 自绘编辑器只接入共享外观，继续拥有命中、键盘、提示和信号；回调仅在 UI 线程调用。
    void InstallSegmentedSurface(QWidget* owner, std::function<SegmentedSurfaceSnapshot()> readState);
    // 旧列预设仅用 QSS 表示选择时明确登记状态；不依赖样式文本推断选项语义。
    void SetSegmentedButtonSelected(QAbstractButton* button, bool selected);
}
