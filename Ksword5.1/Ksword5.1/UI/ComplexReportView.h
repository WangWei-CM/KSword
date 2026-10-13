#pragma once

#include "./StructuredFieldView.h"
#include <QSet>
#include <QWidget>

class QTreeView;
class QToolButton;
class QSortFilterProxyModel;
class QHBoxLayout;
class QLabel;
class QMenu;

namespace ks::ui
{
    class ComplexReportModel;
    class ComplexReportDelegate;
    // 明确复杂报告的共享正文：固定摘要、树形分区和行内展开；只由业务源显式登记接入。
    class ComplexReportView final : public QWidget
    {
    public:
        explicit ComplexReportView(QWidget* parent = nullptr);
        void setDocument(const FieldDocument& document);
        void setSearchText(const QString& query);
        void refreshTheme();
        QToolButton* expandAllButton() const { return m_expandAll; }
        QTreeView* tree() const { return m_tree; }
    protected:
        void resizeEvent(QResizeEvent* event) override;
        void showEvent(QShowEvent* event) override;
    private:
        void updateSummary();
        void fitSummary();
        void updateEmptyState();
        void jumpToSection(const QString& path);
        void copyNode(const QModelIndex& index);
        QSet<QString> expandedPaths() const;
        void restoreExpansion(const QSet<QString>& paths);
        void showContextMenu(const QPoint& position);
        ComplexReportModel* m_model = nullptr; // 完整规范快照；搜索不改变其数据身份。
        QSortFilterProxyModel* m_proxy = nullptr; // 过滤折叠后代，父项命中时保留整组。
        ComplexReportDelegate* m_delegate = nullptr; // 仅绘制当前可见行。
        QTreeView* m_tree = nullptr; // 唯一正文滚动视口。
        QLabel* m_empty = nullptr; // 搜索无命中与没有文档使用明确状态，不显示成功空表。
        QToolButton* m_expandAll = nullptr; // 安装到原搜索/复制/导出工具行的固定动作。
        QWidget* m_summary = nullptr; // 只创建顶层分区摘要，不逐字段创建 QWidget。
        QHBoxLayout* m_summaryLayout = nullptr;
        QLabel* m_summaryLabel = nullptr;
        QToolButton* m_more = nullptr;
        QMenu* m_moreMenu = nullptr;
        QList<QToolButton*> m_sections; // 少量真实顶层分区的跳转按钮。
        QSet<QString> m_beforeSearch; // 清空搜索时恢复用户原有的展开状态。
        QString m_beforeSearchSelected; // 清空后返回原规范选中项。
        int m_beforeSearchScroll = 0; // 清空后保留用户进入搜索前的阅读位置。
        QString m_query; // 原字段搜索控件的最新过滤词。
        QString m_language; // 热换语言只更新显示文本，不重建业务文档。
        bool m_bulkExpansion = false; // 合并全部展开/恢复过程的工具行状态更新。
        bool m_resizeLayoutPending = false; // 只合并当前报告宿主的连续尺寸重测。
    };
}
