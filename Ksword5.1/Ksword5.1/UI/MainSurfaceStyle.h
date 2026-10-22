#pragma once
#include <QColor>
#include <QString>

class QWidget;

namespace ks::ui
{
    // 主工作区的结构色；全部从现有主题种子派生，不改应用或二级窗口的色板。
    struct MainSurfaceColors
    {
        QColor canvas;    // 页面最底层，给内容留出可见轮廓。
        QColor content;   // 表格、文本和数据面。
        QColor alternate; // 内容交替行，低于结构层之间的明暗差。
        QColor toolbar;   // 操作带，与数据面相邻但可辨认。
        QColor header;    // 表头层，固定作为读表入口。
        QColor input;     // 编辑与普通动作共用固定层级，不随嵌套父容器继续变亮。
        QColor border;    // 低饱和的结构边界。
    };

    MainSurfaceColors MainWindowSurfaceColors();
    // 只认所属顶层窗口的明确标记，不穿过对话框去推测主窗口祖先。
    bool UsesMainSurfaceHierarchy(const QWidget* widget);
    // 透明主宿主使用独立的数据衬底；只看所属顶层标记，不传染独立对话框或Popup。
    bool UsesTransparentMainSurface(const QWidget* widget);
    int MainDataSurfaceAlpha();
    void SetMainDataSurfaceOpacityPercent(int percent);
    QColor MainControlSurfaceColor(QColor color, const QWidget* widget);
    QString MainDataSurfaceStyleColor(const QColor& color, bool transparent);
    // 返回主窗口 ADS 工作区的底板/列表/文本规则；透明背景模式由原背景策略负责。
    QString BuildMainSurfaceStyle(bool transparentContent);
}
