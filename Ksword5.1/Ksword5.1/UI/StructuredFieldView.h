#pragma once

#include <QHash>
#include <QModelIndex>
#include <QString>
#include <QVector>
#include <QWidget>

class QComboBox;
class QLabel;
class QLineEdit;
class QToolButton;
class QTreeWidget;
class QTreeWidgetItem;

namespace ks::ui
{
    // Values come from the actual metadata/PE/driver result. They are never
    // reconstructed by parsing a generated report or interpreted as HTML.
    struct FieldNode
    {
        enum class Kind { Section, Field, Note };
        Kind kind = Kind::Field;
        QString name;
        QString value;
        bool translateValue = false;
        QVector<FieldNode> children;
        bool initiallyExpanded = true;
        bool translateName = true;
    };

    struct FieldDocument
    {
        QString title;
        QVector<FieldNode> nodes;

        // Fields and notes append to the most recently started section, or to
        // the document root before the first section. Nested data can be built
        // directly through FieldNode::children without retaining UI pointers.
        FieldDocument& section(const QString& title);
        FieldDocument& field(const QString& name, const QString& value,
            bool translateValue = false);
        FieldDocument& note(const QString& body);
        QString toPlainText(bool localize = false) const;
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
        // 详情卡片接入后仅保存规范文档与搜索/复制/导出入口，销毁旧树和格式选择器。
        // 单向切换，不影响未接入卡片的列表详情和专业工作台；此模式tree()返回nullptr。
        void setDocumentOnly();
        bool isDocumentOnly() const { return m_documentOnly; }
        void setDocument(const FieldDocument& document);
        const FieldDocument& document() const { return m_document; }
        // 规范文档代次供卡片合并通知；未变化的document-only快照不增加代次。
        quint64 documentRevision() const { return m_documentRevision; }
        QString plainText() const;
        QString selectedText() const;
        bool exportText(const QString& filePath, QString* error = nullptr) const;
        FieldCopySnapshot captureCopy(const QModelIndex& clicked) const;
        void setSearchText(const QString& text);
        QString searchText() const;
        void setPresentation(Presentation presentation);
        Presentation presentation() const { return m_presentation; }
        QTreeWidget* tree() const { return m_tree; }

    signals:
        void documentChanged(); // Emitted once per event turn after a complete value snapshot.
        void presentationChanged(); // 共享卡片宿主同步程序式切换，不依赖被QSignalBlocker阻断的下拉信号。

    protected:
        bool eventFilter(QObject* watched, QEvent* event) override;
        void changeEvent(QEvent* event) override;
        void resizeEvent(QResizeEvent* event) override;

    private:
        void rebuild();
        void updateTranslations();
        void applyPresentation();
        void applySearch();
        void updateColumns();
        void showCopyMenu(const QPoint& position);
        void copyText(const QString& text);
        QString exportItems(bool selectedOnly) const;

        FieldDocument m_document;
        Presentation m_presentation = Presentation::Sections;
        QLabel* m_title = nullptr;
        QLineEdit* m_search = nullptr;
        QComboBox* m_style = nullptr;
        QToolButton* m_copy = nullptr;
        QToolButton* m_export = nullptr;
        QTreeWidget* m_tree = nullptr;
        QHash<QString, bool> m_expansionBeforeSearch;
        bool m_searchActive = false;
        bool m_layoutPending = false;
        bool m_documentNotificationPending = false;
        bool m_documentOnly = false; // 卡片宿主只使用文档数据，禁止旧树构建、排序和列重测。
        quint64 m_documentRevision = 0; // 搜索与主题变化不属于规范文档更新。
    };
}
