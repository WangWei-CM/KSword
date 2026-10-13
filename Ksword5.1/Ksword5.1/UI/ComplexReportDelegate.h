#pragma once

#include <QStyledItemDelegate>
#include <QHash>
#include <QFont>
#include <QPersistentModelIndex>
#include <QTreeView>
#include <QMetaObject>
#include <QList>
#include <functional>

namespace ks::ui
{
    // 共享报告树只接管分支绘制；原生鼠标命中、展开、选择与键盘行为保持原对象实现。
    class ComplexReportTree final : public QTreeView
    {
    public:
        explicit ComplexReportTree(QWidget* parent = nullptr);
        void setModel(QAbstractItemModel* model) override;
    protected:
        void drawRow(QPainter* painter, const QStyleOptionViewItem& option, const QModelIndex& index) const override;
        void drawBranches(QPainter* painter, const QRect& rect, const QModelIndex& index) const override;
    private:
        void scheduleReportLayout(); // 展开和过滤后的自然行高合并重测，不在布局过程中递归进入布局。
        QList<QMetaObject::Connection> m_modelConnections; // 换模型时只撤销本视图的报告几何观察。
        bool m_layoutPending = false; // 连续结构变化只排队一次，不为每个展开节点刷新全树。
    };

    // 单列报告的可见行绘制：分区、子组、字段和展开正文共用实际主题表面与字号。
    class ComplexReportDelegate final : public QStyledItemDelegate
    {
    public:
        explicit ComplexReportDelegate(QTreeView* view);
        void paint(QPainter* painter, const QStyleOptionViewItem& option,
            const QModelIndex& index) const override;
        QSize sizeHint(const QStyleOptionViewItem& option, const QModelIndex& index) const override;
        bool editorEvent(QEvent* event, QAbstractItemModel* model,
            const QStyleOptionViewItem& option, const QModelIndex& index) override;
        void setCopyHandler(std::function<void(const QModelIndex&)> handler);
        void invalidateMetrics();
    private:
        // 每个规范路径只保留最新测量；完整文本与字体参与比较，纯颜色变化不清空缓存。
        struct Measurement
        {
            int width = 0; // 扣除树缩进后的真实行宽。
            int kind = 0; // 分区、普通字段与展开正文采用不同的自然高度。
            bool children = false; // 有全文子项时说明使用紧凑预览高度。
            int topSpacing = 0; // 卡间距、属性面四像素间隙和首属性标题的总自然高度。
            int bottomSpacing = 0; // 分区与属性面各自底沿圆角的总自然留白。
            QFont font; // 完整字体属性参与比较，保留字号单位、字距、字重与字体家族。
            QString name; // 名称改变时重算带标题的正文高度。
            QString value; // 完整正文参与缓存身份，不只比较前缀或长度。
            QString actions; // 热换语言后的展开提示宽度也是说明预览的测量输入。
            QSize hint; // 该身份最后求出的自然行高。
        };
        QTreeView* m_view = nullptr; // 只观察所属视图宽度，不扫描全页或创建单元格控件。
        std::function<void(const QModelIndex&)> m_copy; // 视图提供当前规范子树的复制动作。
        mutable QHash<QString, Measurement> m_measurements; // 按需测量的行缓存，不维护平行文档。
        QPersistentModelIndex m_pressedCopy; // 只有同一复制动作内按下并释放才执行复制。
        QPersistentModelIndex m_pressedExpand; // 真实全文预览链接内完整点击才展开或收起。
    };
}
