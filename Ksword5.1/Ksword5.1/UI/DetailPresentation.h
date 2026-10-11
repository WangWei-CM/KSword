#pragma once

#include <QString>
#include <QIcon>

class QWidget;
class QFormLayout;
class QScrollArea;

namespace ks::ui
{
    class StructuredFieldView;
    // 创建全宽对象摘要；返回头部，其QHBoxLayout可在右侧继续添加原业务动作。
    QWidget* CreateDetailSummaryHeader(QWidget* parent, const QString& title,
        const QString& subtitle = QString(), const QIcon& icon = QIcon());
    // 创建带内部标题的无描边详情卡片；返回卡片，其QVBoxLayout可继续添加字段或说明。
    QWidget* CreateDetailCard(const QString& title, QWidget* parent);
    // 为卡片添加两列字段；窄窗按原左右阅读顺序变为单列，返回原QFormLayout供调用方填值。
    void AddDetailFieldColumns(QWidget* card, QFormLayout*& first, QFormLayout*& second);
    // 详情字段按真实宽度换行；只配置表单几何，不改变字段内容或信号。
    void ConfigureDetailForm(QFormLayout* form);
    // 侧栏随所属窗口宽度收缩，maximumWidth为宽屏上限，不改变导航和页签索引。
    void InstallDetailSidebarSizing(QScrollArea* sidebar, int maximumWidth);
    // 原结构化视图继续持有文档与导出逻辑；卡片投影展示同一数据，scrollBody=false用于已有正文滚动区。
    QWidget* CreateDetailFieldProjection(StructuredFieldView* source, QWidget* parent = nullptr,
        bool scrollBody = true);
    // 统一安装窗口布局框架；保留原页签/字段/动作，首次与惰性页面创建后合并整理。
    void InstallDetailWindowFramework(QWidget* window);
    // 构造完成的首次Show安全点调用，首帧绘制前一次性准备结构；普通刷新不重挂布局。
    void PrepareDetailWindowForShow(QWidget* window);
}
