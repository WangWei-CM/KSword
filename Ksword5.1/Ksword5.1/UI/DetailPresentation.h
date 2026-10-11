#pragma once

#include <QString>

class QWidget;
class QFormLayout;
class QScrollArea;

namespace ks::ui
{
    // 创建带内部标题的无描边详情卡片；返回卡片，其QVBoxLayout可继续添加字段或说明。
    QWidget* CreateDetailCard(const QString& title, QWidget* parent);
    // 为卡片添加两列字段；窄窗按原左右阅读顺序变为单列，返回原QFormLayout供调用方填值。
    void AddDetailFieldColumns(QWidget* card, QFormLayout*& first, QFormLayout*& second);
    // 详情字段按真实宽度换行；只配置表单几何，不改变字段内容或信号。
    void ConfigureDetailForm(QFormLayout* form);
    // 侧栏随所属窗口宽度收缩，maximumWidth为宽屏上限，不改变导航和页签索引。
    void InstallDetailSidebarSizing(QScrollArea* sidebar, int maximumWidth);
}
