#pragma once
#include "./StructuredFieldView.h"
#include <QPointer>
#include <QSet>
#include <QHash>
#include <QWidget>

class QTreeView;
class QToolButton;
class QComboBox;
class QEvent;
class QLineEdit;
class QLabel;
class QTabBar;
class QVBoxLayout;
class CodeEditorWidget;

namespace ks::ui
{
    class ComplexReportModel;
    class ComplexReportFilter;
    // 同一规范文档的紧凑浏览器：范围/类型直达，字段一行三列，完整原文随时可读。
    class ComplexReportView final : public QWidget
    {
    public:
        explicit ComplexReportView(QWidget* parent = nullptr);
        void setDocument(const FieldDocument& document);
        void setSearchText(const QString& query);
        void bindSearchField(QLineEdit* search);
        void refreshTheme();
        QToolButton* actionsButton() const { return m_actions; }
        QTreeView* tree() const { return m_tree; }
    protected:
        void changeEvent(QEvent* event) override;
    private:
        void updateNavigation(bool firstDocument);
        void updateTypes(bool selectFirst);
        void applyNavigation();
        void updatePresentation();
        void updateRawText();
        QString currentScopeText() const;
        void expandSearchGroups();
        void setCurrentExpanded(bool expanded);
        void showContextMenu(const QPoint& position);
        void copyCell(const QModelIndex& index, bool wholeRecord);
        QModelIndex scopeIndex() const;
        QModelIndex displayRoot() const;
        QSet<QString> expandedPaths() const;
        void restoreExpansion(const QSet<QString>& paths);
        ComplexReportModel* m_model = nullptr; // 唯一完整规范快照，原文和复制均从这里读取。
        ComplexReportFilter* m_proxy = nullptr; // 元数据去重与多词匹配，不重建第二份字段数据。
        QTreeView* m_tree = nullptr; // 三列正文视口，使用共享紧凑表格外观。
        QVBoxLayout* m_layout = nullptr; // 模式切换复用唯一正文区域。
        QComboBox* m_scopes = nullptr; // 直接选择业务范围，不在长报告里滚动寻找。
        QComboBox* m_types = nullptr; // 直接选择范围内类型，保留每个范围的上次选择。
        QLabel* m_count = nullptr; // 当前可浏览行数与当前搜索范围的反馈。
        QLabel* m_empty = nullptr; // 没有资料和没有匹配使用不同提示。
        QTabBar* m_modes = nullptr; // 共享分段控制：字段/原文。
        QToolButton* m_diagnostics = nullptr; // 从结构浏览一键进入真实诊断。
        QToolButton* m_actions = nullptr; // 展开、收起与范围复制收拢为低频菜单。
        CodeEditorWidget* m_editor = nullptr; // 原文模式首次使用时才创建，共用完整文档。
        QPointer<QLineEdit> m_search; // 原投影的搜索字段与快捷键入口。
        QHash<QString, QString> m_lastTypes; // 各业务范围的上次类型身份。
        QString m_scopePath; // 空为全部报告，特殊值为诊断，其余为规范分区路径。
        QString m_typePath; // 当前类型的规范身份；空表示当前范围的全部类型。
        QString m_query; // 当前搜索词，搜索遍及所选范围的所有类型。
        QString m_language; // 本地化显示代次，不改变规范节点身份。
        QSet<QString> m_beforeSearch; // 搜索前的展开状态。
        QString m_beforeSearchSelected; // 搜索前选中的规范字段。
        int m_beforeSearchColumn = 0; // 搜索前实际选中的值列。
        int m_beforeSearchScroll = 0; // 搜索前的阅读位置。
        bool m_initialized = false; // 首份目录到达时选择默认范围和类型。
        bool m_updatingNavigation = false; // 批量重填组合框时不触发业务导航。
        bool m_rawDirty = true; // 隐藏原文只记脏，避免每次更新重复序列化大报告。
        bool m_diagnosticColumns = false; // 两列诊断和三列目录分别保存名称列宽。
        int m_catalogNameWidth = 240; // 目录名称保留稳定扫描列，类型列接收剩余宽度。
        int m_diagnosticNameWidth = 160; // 诊断名称短，更多横向空间留给原因正文。
    };
}
