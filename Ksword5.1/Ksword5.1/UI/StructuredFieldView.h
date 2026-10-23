#pragma once

#include <QHash>
#include <QModelIndex>
#include <QSet>
#include <QString>
#include <QVector>
#include <QWidget>
#include <functional>
#include <atomic>
#include <memory>

class QComboBox;
class QLabel;
class QLineEdit;
class QShowEvent;
class QToolButton;
class QTreeWidget;
class QTreeView;
class QTreeWidgetItem;

namespace ks::ui
{
    class FieldTreeModel;
    class FieldTreeSelection;
    struct FieldTreeProjection;
    struct StructuredFieldViewState;
    struct PageSearchBodyText;
    // Values come from the actual metadata/PE/driver result. They are never
    // reconstructed by parsing a generated report or interpreted as HTML.
    struct FieldNode
    {
        enum class Kind { Section, Field, Note };
        // 报告摘要只使用生产者明确提供的状态，不从任意中英文说明中猜测成功或失败。
        enum class Status { Neutral, Info, Success, Warning, Error };
        enum class ReportRole { Content, Catalog, Collection, Record, Metadata };
        Kind kind = Kind::Field;
        QString name;
        QString value;
        bool translateValue = false;
        QVector<FieldNode> children;
        bool initiallyExpanded = true;
        bool translateName = true;
        Status status = Status::Neutral; // 独立来源的真实状态；未提供时保持中性。
        QString summaryLabel; // 摘要条的短标题；为空时显示原分区名，不改变规范文档。
        ReportRole reportRole = ReportRole::Content; // 生产者明确区分目录、类型、字段及已投影到列的元数据。
        int reportedCount = -1; // 原始资料声明的字段数，不能用属性子项数冒充。
        bool preferredScope = false; // 当前业务对象默认进入的报告范围。
        bool operator==(const FieldNode& other) const
        {
            if (kind != other.kind || name != other.name || value != other.value
                || translateValue != other.translateValue || initiallyExpanded != other.initiallyExpanded
                || translateName != other.translateName || status != other.status || summaryLabel != other.summaryLabel
                || reportRole != other.reportRole || reportedCount != other.reportedCount
                || preferredScope != other.preferredScope || children.size() != other.children.size()) return false;
            for (qsizetype index = 0; index < children.size(); ++index)
                if (!(children.at(index) == other.children.at(index))) return false;
            return true;
        }
    };

    struct FieldDocument
    {
        QString title;
        QVector<FieldNode> nodes;
        bool operator==(const FieldDocument&) const = default;

        // Fields and notes append to the most recently started section, or to
        // the document root before the first section. Nested data can be built
        // directly through FieldNode::children without retaining UI pointers.
        FieldDocument& section(const QString& sectionTitle);
        FieldDocument& field(const QString& name, const QString& value,
            bool translateValue = false);
        FieldDocument& note(const QString& body);
        QString toPlainText(bool localize = false) const;
        // 冻结的纯值翻译回调供后台导出使用；空回调保留规范源文本。
        // 标题、可翻译字段和 Note 使用同一格式规则，不访问任何控件或全局语言状态。
        QString toPlainText(const std::function<QString(const QString&)>& translate) const;
        bool isEmpty() const { return title.isEmpty() && nodes.isEmpty(); }
    };

    // Copy payloads can be inspected without touching the system clipboard.
    struct FieldCopySnapshot
    {
        QString value;
        QString row;
        QString all;
        bool hasRow = false;
    };

    class StructuredFieldView final : public QWidget
    {
        Q_OBJECT
    public:
        enum class Presentation { Sections, Tree };

        explicit StructuredFieldView(QWidget* parent = nullptr);
        ~StructuredFieldView() override;
        // Empty initial view only: the same six toolbar/header values used by
        // the real controls. Pages opt into unique targets before installation.
        static QVector<PageSearchBodyText> coldSearchBody(const QString& targetPrefix);
        void setSearchTargetPrefix(const QString& targetPrefix);
        // 普通只读字段页显式选择后台值投影和按需行模型；单向切换后 tree() 返回 nullptr。
        // 专业工作台不调用此入口，保留原生 QTreeWidget 对象及同步访问契约。
        void setValueModelProjection();
        // 详情卡片接入后仅保存规范文档与搜索/复制/导出入口，销毁旧树和格式选择器。
        // 单向切换，不影响未接入卡片的列表详情和专业工作台；此模式tree()返回nullptr。
        void setDocumentOnly();
        bool isDocumentOnly() const { return m_documentOnly; }
        // 普通页面可显式延后隐藏树投影；规范文档、复制和导出始终立即可用。
        // 默认关闭，专业工作台仍保持 tree() 的同步模型契约。
        void setHiddenProjectionDeferred(bool enabled = true);
        // 普通刷新保留已发布画面；显式换对象可重置显示，空文档始终按清空处理。
        void setDocument(const FieldDocument& document, bool resetPublished = false);
        const FieldDocument& document() const { return m_document; }
        // 规范文档代次供卡片合并通知；未变化的document-only快照不增加代次。
        quint64 documentRevision() const { return m_documentRevision; }
        QString plainText() const;
        QString selectedText() const;
        bool exportText(const QString& filePath, QString* error = nullptr) const;
        FieldCopySnapshot captureCopy(const QModelIndex& clicked) const;
        void setSearchText(const QString& text);
        QString searchText() const;
        // Opaque value-model reading state for a bounded widget pool. Captures
        // compact canonical selection, not a scan of all tree rows. The same
        // implicitly shared document must be installed before restoring it.
        std::shared_ptr<const StructuredFieldViewState> captureViewState() const;
        // Null/foreign-document state clears prior pool occupant reading state.
        // Legacy/professional QTreeWidget projections remain unsupported.
        bool restoreViewState(const std::shared_ptr<const StructuredFieldViewState>& state);
        void setPresentation(Presentation presentation);
        Presentation presentation() const { return m_presentation; }
        QTreeWidget* tree() const { return m_tree; }

    signals:
        void documentChanged(); // Emitted once per event turn after a complete value snapshot.
        void presentationChanged(); // 共享卡片宿主同步程序式切换，不依赖被QSignalBlocker阻断的下拉信号。

    protected:
        bool eventFilter(QObject* watched, QEvent* event) override;
        void changeEvent(QEvent* event) override;
        void showEvent(QShowEvent* event) override;
        void resizeEvent(QResizeEvent* event) override;

    private:
        // 只识别已实际登记共享框架且无专业布局保护的详情源，不依据普通隐藏状态猜测用途。
        bool belongsToSharedDetail() const;
        // 显式普通页面或已登记详情可延迟隐藏树；其余专业/旧用户保留同步模型语义。
        bool shouldDeferHiddenTree() const;
        // 回填仍存在的旧树；调用前已确认页面可见，投影接管后此入口直接退出。
        void flushDeferredTree();
        void rebuild();
        void scheduleValueProjection();
        void prepareValueRows();
        void consumeValueRows(quint64 ticket);
        void updateValueColumns();
        void showValueCopyMenu(const QPoint& position);
        bool valueRowSelected(const QString& key) const;
        void flushValueSelection() const;
        void updateTranslations();
        void applyPresentation();
        void applySearch();
        void updateColumns();
        void showCopyMenu(const QPoint& position);
        void copyText(const QString& text);
        // 用户复制/导出跟随当前显示快照；公开 document/plainText/exportText 仍读取规范文档。
        FieldDocument actionDocument() const;
        std::function<QString(const QString&)> actionTranslator() const;
        QString actionPlainText() const;
        QString exportItems(bool selectedOnly) const;

        FieldDocument m_document;
        Presentation m_presentation = Presentation::Sections;
        QLabel* m_title = nullptr;
        QLineEdit* m_search = nullptr;
        QComboBox* m_style = nullptr;
        QToolButton* m_copy = nullptr;
        QToolButton* m_export = nullptr;
        QTreeWidget* m_tree = nullptr;
        QTreeView* m_valueTree = nullptr;
        FieldTreeModel* m_valueModel = nullptr;
        QLabel* m_valueLoading = nullptr;
        std::shared_ptr<std::atomic_bool> m_valueCancel;
        quint64 m_valueTicket = 0;
        quint64 m_valueReadingRevision = 0; // 只在既有选择/展开操作时递增，后台旧副本不得覆盖等待期间的新操作。
        bool m_valuePublished = false; // 合法空结果同样算已发布；池换对象时重新等待首份对应快照。
        std::function<QString(const QString&)> m_valuePublishedTranslate; // 只保留已发布译者，不复制规范文档。
        QVector<QPersistentModelIndex> m_valueRows;
        qsizetype m_valueRowCursor = 0;
        int m_valueRootNext = 0;
        int m_valueRootTotal = 0;
        bool m_valuePrepared = false;
        bool m_valueRowsPending = false;
        bool m_valueApplying = false;
        QHash<QString, bool> m_valueExpansion;
        mutable std::shared_ptr<FieldTreeSelection> m_valueSelection;
        mutable bool m_valueClearPending = false;
        bool m_valueRestoreSelection = false;
        QString m_valueSelected;
        int m_valueColumn = 0;
        int m_valueScroll = 0;
        int m_valueHorizontalScroll = 0;
        bool m_valueRestoreHorizontalScroll = false;
        QString m_valueBeforeSearchSelected;
        int m_valueBeforeSearchColumn = 0;
        int m_valueBeforeSearchScroll = 0;
        bool m_valueSearchActive = false;
        QHash<QString, bool> m_expansionBeforeSearch;
        bool m_searchActive = false;
        bool m_layoutPending = false;
        bool m_documentNotificationPending = false;
        bool m_documentOnly = false; // 卡片宿主只使用文档数据，禁止旧树构建、排序和列重测。
        bool m_deferHiddenProjection = false; // 普通页面显式选择隐藏只保存规范快照，默认不改变 tree() 契约。
        bool m_hiddenTreeDirty = false; // 未访问详情Tab只保存唯一最新文档，不维护即将淘汰的旧树。
        bool m_hiddenTreeLayoutDirty = false; // 隐藏旧树的主题/字体/几何工作合并到下一次显示。
        bool m_treeShowPending = false; // 详情切页的Show先于currentChanged，排队旧树fallback可被投影接管取消。
        quint64 m_documentRevision = 0; // 搜索与主题变化不属于规范文档更新。
    };
}
