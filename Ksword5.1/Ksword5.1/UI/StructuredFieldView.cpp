#include "StructuredFieldView.h"
#include "FieldTreeModel.h"
#include "PageNavigationCatalog.h"
#include "./PageControlStyle.h"
#include "./ToolbarMetrics.h"
#include "../Internationalization/LanguageManager.h"
#include "ThemeBinding.h"
#include "../theme.h"

#include <QApplication>
#include <QClipboard>
#include <QComboBox>
#include <QEvent>
#include <QFileDialog>
#include <QSaveFile>
#include <QMessageBox>
#include <QHeaderView>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QPainter>
#include <QPointer>
#include <QResizeEvent>
#include <QScrollBar>
#include <QSet>
#include <QShortcut>
#include <QShowEvent>
#include <QSignalBlocker>
#include <QStyle>
#include <QStyledItemDelegate>
#include <QTextLayout>
#include <QTextOption>
#include <QTimer>
#include <QToolButton>
#include <QTreeWidget>
#include <QTreeView>
#include <QElapsedTimer>
#include <QThreadPool>
#include <QCoreApplication>
#include <QVBoxLayout>
#include <QtMath>

#include <algorithm>
#include <functional>

namespace ks::ui
{
    namespace
    {
        enum PropertyRole { KindRole = Qt::UserRole + 176, KeyRole, RowRole, ValueRole };

        void retireFieldProjection(std::shared_ptr<const FieldTreeProjection> snapshot,
            QHash<QString, bool> expansion = {}, std::function<QString(const QString&)> translate = {})
        {
            if (snapshot || !expansion.isEmpty() || translate)
                QThreadPool::globalInstance()->start([snapshot = std::move(snapshot), expansion = std::move(expansion),
                    translate = std::move(translate)]() mutable
                    { snapshot.reset(); expansion.clear(); translate = {}; }); // 旧译者与旧投影同一次后台退休。
        }

        void retireFieldSelection(std::shared_ptr<FieldTreeSelection> selection)
        {
            if (selection) QThreadPool::globalInstance()->start([selection = std::move(selection)]() mutable { selection.reset(); });
        }

        bool writeDocumentText(const FieldDocument& document, const QString& filePath, QString* error,
            const std::function<QString(const QString&)>& translate = {})
        {
            QSaveFile file(filePath);
            const QByteArray bytes = (translate ? document.toPlainText(translate) : document.toPlainText(true)).toUtf8();
            if (!file.open(QIODevice::WriteOnly) || file.write(bytes) != bytes.size() || !file.commit())
            {
                if (error) *error = file.errorString();
                return false;
            }
            return true;
        }

        StructuredFieldView::Presentation& sessionPresentation()
        {
            static auto presentation = StructuredFieldView::Presentation::Sections;
            return presentation;
        }

        QString translated(const QString& source)
        {
            return ks::i18n::sourceText(source);
        }

        enum class InitialSearchText { Search, Sections, Tree, Copy, Name, Value };
        struct InitialSearchEntry
        {
            const char* source;
            const char* suffix;
            bool mnemonic;
        };
        constexpr InitialSearchEntry kInitialSearchTexts[] = {
            {"搜索属性", "_search", false},
            {"分区", "_style", false},
            {"树形", "_style", false},
            {"复制全部", "_copy", true},
            {"属性", "_tree", false},
            {"值", "_tree", false}
        };
        QString initialSearchText(InitialSearchText text)
        {
            return translated(QString::fromUtf8(kInitialSearchTexts[static_cast<int>(text)].source));
        }
        QString initialSearchTarget(const QString& prefix, InitialSearchText text)
        {
            return prefix + QLatin1String(kInitialSearchTexts[static_cast<int>(text)].suffix);
        }

        void walkItems(QTreeWidgetItem* parent,
            const std::function<void(QTreeWidgetItem*)>& visitor)
        {
            for (int index = 0; index < parent->childCount(); ++index)
            {
                auto* item = parent->child(index);
                visitor(item);
                walkItems(item, visitor);
            }
        }

        // QTextLayout is only the native delegate's plain text glyph layout.
        // There is no text document, rich text engine, editor or per-row widget.
        qreal layoutPlainText(const QString& text, const QFont& font, const qreal width,
            QPainter* painter = nullptr, const QPointF& origin = QPointF(), const bool wrap = true)
        {
            qreal height = 0;
            const qreal available = std::max<qreal>(24, width);
            const auto paragraphs = text.split(QLatin1Char('\n'));
            for (const auto& paragraph : paragraphs)
            {
                const qreal paragraphStart = height;
                QTextLayout layout(paragraph, font);
                QTextOption option;
                option.setWrapMode(wrap ? QTextOption::WrapAtWordBoundaryOrAnywhere : QTextOption::NoWrap);
                layout.setTextOption(option);
                layout.beginLayout();
                for (;;)
                {
                    QTextLine line = layout.createLine();
                    if (!line.isValid()) break;
                    line.setLineWidth(available);
                    line.setPosition(QPointF(0, height));
                    height += line.height();
                }
                layout.endLayout();
                if (height == paragraphStart) height += QFontMetricsF(font).height();
                if (painter) layout.draw(painter, origin);
            }
            return height;
        }

        class FieldTreeView final : public QTreeView
        {
        public:
            using QTreeView::QTreeView;
            std::function<void()> clearLogicalSelection;
        protected:
            QItemSelectionModel::SelectionFlags selectionCommand(const QModelIndex& index, const QEvent* event) const override
            {
                const auto command = QTreeView::selectionCommand(index, event);
                if (command.testFlag(QItemSelectionModel::Clear) && clearLogicalSelection) clearLogicalSelection();
                return command;
            }
        };

        class PropertyDelegate final : public QStyledItemDelegate
        {
        public:
            explicit PropertyDelegate(QTreeView* tree) : QStyledItemDelegate(tree), m_tree(tree) {}
            bool sections = true;

            void paint(QPainter* painter, const QStyleOptionViewItem& supplied,
                const QModelIndex& index) const override
            {
                QStyleOptionViewItem option(supplied);
                initStyleOption(&option, index);
                const auto kind = static_cast<FieldNode::Kind>(index.data(KindRole).toInt());
                const bool section = kind == FieldNode::Kind::Section;
                const bool selected = option.state.testFlag(QStyle::State_Selected);
                const auto text = option.text;
                option.text.clear();
                // 分组底色由语义表面派生，不能依赖可能被祖先透明 QSS 污染的行画刷。
                const QColor sectionSurface = KswordTheme::SurfaceAltColor();
                if (section && !selected) option.backgroundBrush = sectionSurface;
                const QWidget* widget = option.widget;
                auto* style = widget ? widget->style() : QApplication::style();
                style->drawControl(QStyle::CE_ItemViewItem, &option, painter, widget);
                QFont font = option.font;
                if (section) font.setBold(true);
                const int insetX = sections ? 10 : 6;
                const int insetY = section ? 4 : 3;
                const QRect area = option.rect.adjusted(insetX, insetY, -insetX, -insetY);
                painter->save();
                painter->setClipRect(option.rect);
                if (section && !selected)
                {
                    painter->fillRect(option.rect, sectionSurface);
                }
                const auto group = option.state.testFlag(QStyle::State_Enabled)
                    ? (option.state.testFlag(QStyle::State_Active) ? QPalette::Active : QPalette::Inactive)
                    : QPalette::Disabled;
                // 属性名是可读信息而非占位提示；次级文字按真实行底校准，不再呈现禁用态灰字。
                const bool secondary = kind == FieldNode::Kind::Note || (!section && index.column() == 0);
                QColor background = selected ? option.palette.color(group, QPalette::Highlight)
                    : section ? sectionSurface : option.palette.color(group,
                        option.features.testFlag(QStyleOptionViewItem::Alternate) ? QPalette::AlternateBase : QPalette::Base);
                if (background.alpha() < 255)
                {
                    background = KswordTheme::SurfaceColor();
                }
                const QColor preferredText = selected ? option.palette.color(group, QPalette::HighlightedText)
                    : secondary ? KswordTheme::TextSecondaryColor() : KswordTheme::TextPrimaryColor();
                const QColor readableText = KswordTheme::EnsureTextContrast(preferredText, background,
                    group == QPalette::Disabled ? 3.0 : 4.5);
                painter->setPen(readableText);
                layoutPlainText(text, font, area.width(), painter, area.topLeft(), m_tree->wordWrap());
                if (section && !selected)
                {
                    // 单条分隔标记分组边界，不为每个属性添加网格线。
                    painter->setPen(KswordTheme::EnsureTextContrast(KswordTheme::BorderColor(), sectionSurface, 1.6));
                    painter->drawLine(option.rect.bottomLeft(), option.rect.bottomRight());
                }
                painter->restore();
            }

            QSize sizeHint(const QStyleOptionViewItem& supplied, const QModelIndex& index) const override
            {
                QStyleOptionViewItem option(supplied);
                initStyleOption(&option, index);
                const auto kind = static_cast<FieldNode::Kind>(index.data(KindRole).toInt());
                const bool span = kind != FieldNode::Kind::Field;
                QFont font = option.font;
                if (kind == FieldNode::Kind::Section) font.setBold(true);
                int width = m_tree->columnWidth(index.column());
                if (span && index.column() == 0)
                    width = m_tree->viewport()->width();
                if (index.column() == 0)
                {
                    int depth = m_tree->rootIsDecorated() ? 1 : 0;
                    for (auto ancestor = index.parent(); ancestor.isValid(); ancestor = ancestor.parent()) ++depth;
                    width -= depth * m_tree->indentation();
                }
                const int insetX = sections ? 10 : 6;
                const int insetY = kind == FieldNode::Kind::Section ? 4 : 3;
                const int height = qCeil(layoutPlainText(option.text, font, width - 2 * insetX,
                    nullptr, QPointF(), m_tree->wordWrap())) + 2 * insetY;
                return QSize(std::max(24, width), std::max(height, QFontMetrics(font).height() + 2 * insetY));
            }

        private:
            QTreeView* m_tree;
        };
    }

    QVector<PageSearchBodyText> StructuredFieldView::coldSearchBody(const QString& targetPrefix)
    {
        if (targetPrefix.isEmpty()) return {};
        QVector<PageSearchBodyText> body;
        body.reserve(6);
        for (int index = 0; index < 6; ++index)
        {
            const auto text = static_cast<InitialSearchText>(index);
            body.append({initialSearchText(text), initialSearchTarget(targetPrefix, text), {},
                kInitialSearchTexts[index].mnemonic});
        }
        return body;
    }

    void StructuredFieldView::setSearchTargetPrefix(const QString& targetPrefix)
    {
        if (targetPrefix.isEmpty()) return;
        m_search->setObjectName(initialSearchTarget(targetPrefix, InitialSearchText::Search));
        if (m_style) m_style->setObjectName(initialSearchTarget(targetPrefix, InitialSearchText::Sections));
        m_copy->setObjectName(initialSearchTarget(targetPrefix, InitialSearchText::Copy));
        if (m_tree) m_tree->setObjectName(initialSearchTarget(targetPrefix, InitialSearchText::Name));
        if (m_valueTree) m_valueTree->setObjectName(initialSearchTarget(targetPrefix, InitialSearchText::Name));
    }

    StructuredFieldView::StructuredFieldView(QWidget* parent) : QWidget(parent)
    {
        setMinimumSize(0, 0);
        setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Ignored);
        auto* layout = new QVBoxLayout(this);
        layout->setContentsMargins(0, 0, 0, 0);
        layout->setSpacing(8);
        m_title = new QLabel(this);
        m_title->setTextFormat(Qt::PlainText);
        m_title->setProperty("ks_i18n_preserve_data_text", true);
        m_title->setWordWrap(true);
        m_title->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
        m_title->hide();
        layout->addWidget(m_title);
        auto* toolbar = new QHBoxLayout;
        toolbar->setSpacing(6);
        m_search = new QLineEdit(this);
        // 结构属性树搜索是结果过滤，不属于代码编辑器查找/替换输入。
        StyleSearchField(m_search);
        m_search->setClearButtonEnabled(true);
        m_search->setMinimumWidth(0);
        m_search->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Fixed);
        toolbar->addWidget(m_search, 1);
        m_style = new QComboBox(this);
        m_style->setSizeAdjustPolicy(QComboBox::AdjustToContents);
        m_style->addItem(QString());
        m_style->addItem(QString());
        toolbar->addWidget(m_style);
        m_copy = new QToolButton(this);
        toolbar->addWidget(m_copy);
        m_export = new QToolButton(this);
        m_export->setIcon(QIcon(QStringLiteral(":/Icon/log_export.svg")));
        toolbar->addWidget(m_export);
        NormalizeToolbarRow(toolbar);
        layout->addLayout(toolbar);
        m_tree = new QTreeWidget(this);
        m_tree->setColumnCount(2);
        m_tree->setProperty("ks_i18n_preserve_model_data", true);
        m_tree->setProperty("ks_preserve_table_presentation", true);
        m_tree->setHeaderHidden(true);
        m_tree->setFrameShape(QFrame::NoFrame);
        m_tree->setSelectionMode(QAbstractItemView::ExtendedSelection);
        m_tree->setSelectionBehavior(QAbstractItemView::SelectRows);
        m_tree->setEditTriggers(QAbstractItemView::NoEditTriggers);
        m_tree->setWordWrap(true);
        m_tree->setUniformRowHeights(false);
        m_tree->setTextElideMode(Qt::ElideNone);
        m_tree->setVerticalScrollMode(QAbstractItemView::ScrollPerPixel);
        m_tree->setHorizontalScrollMode(QAbstractItemView::ScrollPerPixel);
        m_tree->setContextMenuPolicy(Qt::CustomContextMenu);
        m_tree->setItemDelegate(new PropertyDelegate(m_tree));
        m_tree->viewport()->installEventFilter(this);
        m_tree->header()->setMinimumSectionSize(48);
        m_tree->header()->setSectionResizeMode(0, QHeaderView::Interactive);
        m_tree->header()->setSectionResizeMode(1, QHeaderView::Stretch);
        m_tree->header()->setStretchLastSection(true);
        layout->addWidget(m_tree, 1);
        connect(m_search, &QLineEdit::textChanged, this, [this] { applySearch(); });
        connect(m_style, &QComboBox::currentIndexChanged, this, [this](const int index)
        {
            setPresentation(index == 1 ? Presentation::Tree : Presentation::Sections);
        });
        connect(m_copy, &QToolButton::clicked, this, [this] { copyText(actionPlainText()); });
        connect(m_export, &QToolButton::clicked, this, [this]
        {
            const QPointer<StructuredFieldView> self(this);
            const FieldDocument snapshot = actionDocument();
            const auto translate = actionTranslator(); // 对话框可重入，文档和译者一起冻结。
            const QString path = QFileDialog::getSaveFileName(this,
                translated(QStringLiteral("导出文本")), QString(),
                translated(QStringLiteral("文本文件 (*.txt);;所有文件 (*)")));
            if (!self || path.isEmpty()) return;
            QString error;
            if (!writeDocumentText(snapshot, path, &error, translate))
                QMessageBox::warning(self.data(), translated(QStringLiteral("导出失败")), error);
        });
        connect(m_tree, &QTreeWidget::customContextMenuRequested,
            this, [this](const QPoint& position) { showCopyMenu(position); });
        auto* copyShortcut = new QShortcut(QKeySequence::Copy, m_tree);
        copyShortcut->setContext(Qt::WidgetWithChildrenShortcut);
        connect(copyShortcut, &QShortcut::activated, this, [this]
        {
            const auto selected = selectedText();
            copyText(selected.isEmpty() ? actionPlainText() : selected);
        });
        updateTranslations();
        setPresentation(sessionPresentation());
    }

    StructuredFieldView::~StructuredFieldView()
    {
        retireFieldSelection(std::move(m_valueSelection));
        retireFieldProjection({}, std::move(m_valueExpansion));
        if (m_valueCancel) m_valueCancel->store(true, std::memory_order_relaxed);
        if (m_valueModel)
        {
            auto old = m_valueModel->takeSnapshotForDisposal();
            retireFieldProjection(std::move(old), {}, std::move(m_valuePublishedTranslate));
        }
    }

    void StructuredFieldView::setValueModelProjection()
    {
        if (m_documentOnly || m_valueTree) return;
        m_deferHiddenProjection = true;
        m_valueSelection = std::make_shared<FieldTreeSelection>();
        QTreeWidget* obsolete = m_tree;
        m_tree = nullptr;
        obsolete->viewport()->removeEventFilter(this);
        delete obsolete;
        auto* valueTree = new FieldTreeView(this);
        m_valueTree = valueTree;
        valueTree->clearLogicalSelection = [this]
        {
            if (m_valueApplying || !m_valueTree->isVisible()) return;
            ++m_valueReadingRevision;
            auto old = std::move(m_valueSelection);
            m_valueSelection = std::make_shared<FieldTreeSelection>();
            retireFieldSelection(std::move(old));
            m_valueClearPending = true;
            // ClearAndSelect can leave the same physical range and emit no delta.
            // Flush after Qt applies the command, or synchronously before copy.
            QTimer::singleShot(0, this, [this] { flushValueSelection(); });
        };
        m_valueTree->setProperty("ks_i18n_preserve_model_data", true);
        m_valueTree->setProperty("ks_preserve_table_presentation", true);
        m_valueTree->setHeaderHidden(true);
        m_valueTree->setFrameShape(QFrame::NoFrame);
        m_valueTree->setSelectionMode(QAbstractItemView::ExtendedSelection);
        m_valueTree->setSelectionBehavior(QAbstractItemView::SelectRows);
        m_valueTree->setEditTriggers(QAbstractItemView::NoEditTriggers);
        m_valueTree->setWordWrap(true);
        m_valueTree->setUniformRowHeights(false);
        m_valueTree->setTextElideMode(Qt::ElideNone);
        m_valueTree->setVerticalScrollMode(QAbstractItemView::ScrollPerPixel);
        m_valueTree->setHorizontalScrollMode(QAbstractItemView::ScrollPerPixel);
        m_valueTree->setContextMenuPolicy(Qt::CustomContextMenu);
        m_valueTree->setItemDelegate(new PropertyDelegate(m_valueTree));
        m_valueModel = new FieldTreeModel(this);
        m_valueTree->setModel(m_valueModel);
        m_valueTree->viewport()->installEventFilter(this);
        m_valueTree->header()->setMinimumSectionSize(48);
        m_valueTree->header()->setSectionResizeMode(0, QHeaderView::Interactive);
        m_valueTree->header()->setSectionResizeMode(1, QHeaderView::Stretch);
        static_cast<QVBoxLayout*>(layout())->addWidget(m_valueTree, 1);
        m_valueLoading = new QLabel(this);
        m_valueLoading->setAlignment(Qt::AlignCenter);
        m_valueLoading->setWordWrap(true);
        static_cast<QVBoxLayout*>(layout())->addWidget(m_valueLoading, 1);
        m_valueLoading->hide();
        connect(m_valueTree, &QTreeView::customContextMenuRequested, this,
            [this](const QPoint& position) { showValueCopyMenu(position); });
        const auto recordExpansion = [this](const QModelIndex& index, bool expanded)
        {
            const auto displayed = m_valueModel->snapshot();
            if (!m_valueApplying && displayed && displayed->query.isEmpty())
            {
                ++m_valueReadingRevision;
                m_valueExpansion.insert(index.data(KeyRole).toString(), expanded);
            }
        };
        connect(m_valueTree, &QTreeView::expanded, this,
            [recordExpansion](const QModelIndex& index) { recordExpansion(index, true); });
        connect(m_valueTree, &QTreeView::collapsed, this,
            [recordExpansion](const QModelIndex& index) { recordExpansion(index, false); });
        connect(m_valueTree->verticalScrollBar(), &QScrollBar::valueChanged, this, [this](int value)
        {
            if (!m_valueApplying && m_valueTree->isVisible()) m_valueScroll = value;
        });
        connect(m_valueTree->selectionModel(), &QItemSelectionModel::currentChanged, this,
            [this](const QModelIndex& current)
        {
            if (!m_valueApplying && m_valueTree->isVisible() && current.isValid())
            { m_valueSelected = current.data(KeyRole).toString(); m_valueColumn = current.column(); }
        });
        connect(m_valueTree->selectionModel(), &QItemSelectionModel::selectionChanged, this,
            [this](const QItemSelection& selected, const QItemSelection& deselected)
        {
            if (m_valueApplying || !m_valueTree->isVisible() || !m_valueSelection) return;
            ++m_valueReadingRevision;
            const auto apply = [this](const QItemSelection& ranges, bool state)
            {
                for (const auto& range : ranges)
                    for (int row = range.top(); row <= range.bottom(); ++row)
                        m_valueSelection->setSelected(m_valueModel->index(row, 0, range.parent()).data(KeyRole).toString(), state);
            };
            apply(deselected, false);
            apply(selected, true);
        });
        connect(m_valueModel, &QAbstractItemModel::rowsInserted, this,
            [this](const QModelIndex& parent, int first, int last)
        {
            for (int row = first; row <= last; ++row)
                m_valueRows.append(m_valueModel->index(row, 0, parent));
            prepareValueRows();
        });
        auto* shortcut = new QShortcut(QKeySequence::Copy, m_valueTree);
        shortcut->setContext(Qt::WidgetWithChildrenShortcut);
        connect(shortcut, &QShortcut::activated, this, [this]
        {
            const QString selected = selectedText();
            copyText(selected.isEmpty() ? actionPlainText() : selected);
        });
        auto* selectAll = new QShortcut(QKeySequence::SelectAll, m_valueTree);
        selectAll->setContext(Qt::WidgetWithChildrenShortcut);
        connect(selectAll, &QShortcut::activated, this, [this]
        {
            if (m_valueModel->rowCount() == 0) return;
            ++m_valueReadingRevision;
            auto old = std::move(m_valueSelection);
            m_valueSelection = std::make_shared<FieldTreeSelection>();
            const auto displayed = m_valueModel->snapshot();
            m_valueSelection->selectAll(displayed, m_valueExpansion, displayed && !displayed->query.isEmpty());
            m_valueClearPending = false;
            retireFieldSelection(std::move(old));
            m_valueApplying = true;
            m_valueTree->selectAll();
            m_valueApplying = false;
        });
        applyPresentation();
        scheduleValueProjection();
    }

    void StructuredFieldView::scheduleValueProjection()
    {
        if (!m_valueTree || m_documentOnly) return;
        flushValueSelection();
        if (m_valueCancel) m_valueCancel->store(true, std::memory_order_relaxed);
        const quint64 ticket = ++m_valueTicket;
        const QPointer<StructuredFieldView> guard(this);
        const auto current = [&guard, ticket]
        {
            return guard && guard->m_valueTree && guard->m_valueTicket == ticket;
        };
        m_valueRows.clear();
        m_valueRowCursor = 0;
        m_valueRootNext = m_valueRootTotal = 0;
        if (shouldDeferHiddenTree())
        { m_hiddenTreeDirty = true; return; }
        m_hiddenTreeDirty = false;
        m_hiddenTreeLayoutDirty = false;
        const QModelIndex selected = m_valueTree->currentIndex();
        if (selected.isValid() && !m_valueRestoreSelection)
        {
            m_valueSelected = selected.data(KeyRole).toString();
            m_valueColumn = selected.column();
            m_valueScroll = m_valueTree->verticalScrollBar()->value();
        }
        m_valueRestoreSelection = false;
        // 已发布的标题和正文保持同一份快照，更新时不切换布局槽或闪回加载页。
        // 首份快照（包括池换对象后的首份）仍明确显示原有加载状态。
        if (!m_valuePublished)
        {
            m_title->hide();
            if (!current()) return;
            m_valueTree->hide();
            if (!current()) return;
            m_valueLoading->setText(ks::i18n::text(QStringLiteral("shared.fields.loading"), QStringLiteral("正在准备字段...")));
            m_valueLoading->show();
            if (!current()) return;
        }
        const FieldDocument document = m_document;
        const QString query = m_search->text();
        const auto translator = ks::i18n::LanguageManager::instance().sourceTextSnapshot();
        const auto previous = m_valueModel->snapshot();
        const auto expansion = m_valueExpansion;
        const auto selection = std::make_shared<FieldTreeSelection>(*m_valueSelection);
        const auto cancelled = std::make_shared<std::atomic_bool>(false);
        m_valueCancel = cancelled;
        const quint64 readingRevision = m_valueReadingRevision;
        QThreadPool::globalInstance()->start([document, query, translator, previous, expansion, selection, cancelled, guard, ticket, readingRevision]
        {
            const QString title = translator.sourceText(document.title); // 与正文使用同一冻结翻译快照。
            const bool titleVisible = !document.title.isEmpty();
            auto projection = PrepareFieldTreeProjection(document,
                [translator](const QString& source) { return translator.sourceText(source); }, query, previous, cancelled);
            if (!projection) return;
            selection->retain(*projection);
            if (cancelled->load(std::memory_order_relaxed)) return;
            QHash<QString, bool> keptExpansion;
            for (auto it = expansion.cbegin(); it != expansion.cend(); ++it)
                if (projection->keys.contains(it.key())) keptExpansion.insert(it.key(), it.value());
            QMetaObject::invokeMethod(QCoreApplication::instance(),
                [guard, projection = std::move(projection), keptExpansion = std::move(keptExpansion), selection = selection,
                    ticket, readingRevision, title, titleVisible, translator]() mutable
            {
                if (!guard || !guard->m_valueTree || guard->m_valueTicket != ticket)
                { retireFieldProjection(std::move(projection), std::move(keptExpansion)); retireFieldSelection(std::move(selection)); return; }
                if (guard->shouldDeferHiddenTree())
                { guard->m_hiddenTreeDirty = true; retireFieldProjection(std::move(projection), std::move(keptExpansion)); retireFieldSelection(std::move(selection)); return; }
                const bool prepare = [&]
                {
                    const auto before = guard->m_valueModel->snapshot();
                    return !guard->m_valuePrepared || !projection->sameStructure
                        || projection->base.lock() != before || !before || before->query != projection->query;
                }();
                // 旧正文在后台准备时仍可交互；在模型重置前收口待清选的真实状态。
                guard->flushValueSelection();
                guard->m_valueApplying = true;
                guard->m_valueRows.clear();
                guard->m_valueRowCursor = 0;
                auto old = guard->m_valueModel->applySnapshot(std::move(projection));
                if (!guard || !guard->m_valueTree || guard->m_valueTicket != ticket)
                {
                    retireFieldProjection(std::move(old), std::move(keptExpansion));
                    retireFieldSelection(std::move(selection));
                    return;
                }
                auto oldTranslate = std::move(guard->m_valuePublishedTranslate);
                guard->m_valuePublishedTranslate = [translator](const QString& source) { return translator.sourceText(source); };
                retireFieldProjection(std::move(old), {}, std::move(oldTranslate));
                if (guard->m_valueReadingRevision == readingRevision)
                {
                    retireFieldProjection({}, std::move(guard->m_valueExpansion));
                    guard->m_valueExpansion = std::move(keptExpansion);
                    retireFieldSelection(std::move(guard->m_valueSelection));
                    guard->m_valueSelection = std::move(selection);
                }
                else
                {
                    // 等待期间的新选择/展开优先；过期键仍由下一轮既有 worker 裁剪。
                    // 这里只释放后台旧副本，不在 GUI 线程全文遍历，也不另起准备任务。
                    retireFieldProjection({}, std::move(keptExpansion));
                    retireFieldSelection(std::move(selection));
                }
                guard->m_valueApplying = false;
                guard->m_valueRows.clear();
                guard->m_valueRowCursor = 0;
                guard->m_valueRootNext = 0;
                guard->m_valueRootTotal = prepare ? guard->m_valueModel->rowCount() : 0;
                if (prepare) guard->m_valuePrepared = false;
                guard->applyPresentation();
                if (!guard || !guard->m_valueTree || guard->m_valueTicket != ticket) return;
                if (prepare) guard->consumeValueRows(ticket); // unchanged topology never rescans expanded rows
                if (!guard || !guard->m_valueTree || guard->m_valueTicket != ticket) return;
                guard->m_title->setText(title);
                if (!guard || !guard->m_valueTree || guard->m_valueTicket != ticket) return;
                guard->m_title->setVisible(titleVisible);
                if (!guard || !guard->m_valueTree || guard->m_valueTicket != ticket) return;
                guard->m_valuePublished = true;
                guard->m_valueLoading->hide();
                if (!guard || !guard->m_valueTree || guard->m_valueTicket != ticket) return;
                guard->m_valueTree->show();
            }, Qt::QueuedConnection);
        });
    }

    void StructuredFieldView::prepareValueRows()
    {
        if (m_valueRowsPending || !m_valueTree) return;
        m_valueRowsPending = true;
        const quint64 ticket = m_valueTicket;
        QTimer::singleShot(0, this, [this, ticket]
        {
            m_valueRowsPending = false;
            consumeValueRows(ticket);
        });
    }

    void StructuredFieldView::consumeValueRows(quint64 ticket)
    {
        const QPointer<StructuredFieldView> alive(this);
        if (!m_valueTree || shouldDeferHiddenTree()) return;
        if (ticket != m_valueTicket)
        { if (!m_valueRows.isEmpty() || m_valueRootNext < m_valueRootTotal) prepareValueRows(); return; }
        QElapsedTimer elapsed;
        elapsed.start();
        m_valueApplying = true;
        while (m_valueRowCursor < m_valueRows.size() || m_valueRootNext < m_valueRootTotal)
        {
            if (m_valueRowCursor == m_valueRows.size())
            {
                m_valueRows.clear();
                m_valueRowCursor = 0;
                const int last = std::min(m_valueRootNext + 128, m_valueRootTotal);
                while (m_valueRootNext < last)
                    m_valueRows.append(m_valueModel->index(m_valueRootNext++, 0));
            }
            const QPersistentModelIndex index = m_valueRows.at(m_valueRowCursor++);
            if (!index.isValid()) continue;
            if (valueRowSelected(index.data(KeyRole).toString()))
                m_valueTree->selectionModel()->select(index, QItemSelectionModel::Select | QItemSelectionModel::Rows);
            if (!alive || ticket != m_valueTicket) return;
            const auto kind = static_cast<FieldNode::Kind>(index.data(KindRole).toInt());
            m_valueTree->setFirstColumnSpanned(index.row(), index.parent(), kind != FieldNode::Kind::Field);
            if (m_valueModel->hasChildren(index))
            {
                const QString key = index.data(KeyRole).toString();
                if (!m_valueSearchActive && !m_valueExpansion.contains(key))
                    m_valueExpansion.insert(key, index.data(FieldTreeInitiallyExpandedRole).toBool());
                const bool expanded = m_valueSearchActive || m_valueExpansion.value(key, false);
                m_valueTree->setExpanded(index, expanded);
                if (!alive || ticket != m_valueTicket) return;
                if (expanded)
                {
                    const int count = m_valueModel->rowCount(index);
                    for (int row = 0; row < count; ++row)
                        m_valueRows.append(m_valueModel->index(row, 0, index));
                }
            }
            if (elapsed.nsecsElapsed() >= 4000000) break;
        }
        m_valueApplying = false;
        const QModelIndex selected = m_valueModel->indexForKey(m_valueSelected);
        if (selected.isValid())
            m_valueTree->selectionModel()->setCurrentIndex(selected.siblingAtColumn(std::clamp(m_valueColumn, 0, 1)),
                QItemSelectionModel::NoUpdate);
        if (!alive || ticket != m_valueTicket) return;
        if (m_valueRowCursor < m_valueRows.size() || m_valueRootNext < m_valueRootTotal) prepareValueRows();
        else
        {
            m_valueRows.clear();
            m_valueRowCursor = 0;
            m_valuePrepared = true;
            m_valueTree->verticalScrollBar()->setValue(m_valueScroll);
            if (!alive || ticket != m_valueTicket) return;
            if (m_valueRestoreHorizontalScroll)
            {
                m_valueRestoreHorizontalScroll = false;
                m_valueTree->horizontalScrollBar()->setValue(m_valueHorizontalScroll);
            }
        }
    }

    void StructuredFieldView::updateValueColumns()
    {
        if (!m_valueTree) return;
        if (shouldDeferHiddenTree()) { m_hiddenTreeLayoutDirty = true; return; }
        const int width = m_valueTree->viewport()->width();
        const int key = std::min(std::clamp(width / 3, 112, 260), std::max(64, width - 80));
        if (m_valueTree->columnWidth(0) != key) m_valueTree->setColumnWidth(0, key);
        m_valueTree->header()->setStretchLastSection(true);
    }

    void StructuredFieldView::flushValueSelection() const
    {
        if (!m_valueClearPending || !m_valueTree) return;
        m_valueClearPending = false;
        auto replacement = std::make_shared<FieldTreeSelection>();
        for (const QModelIndex& row : m_valueTree->selectionModel()->selectedRows())
            replacement->setSelected(row.data(KeyRole).toString(), true);
        auto old = std::move(m_valueSelection);
        m_valueSelection = std::move(replacement);
        retireFieldSelection(std::move(old));
    }

    bool StructuredFieldView::valueRowSelected(const QString& key) const
    {
        flushValueSelection();
        return m_valueSelection && m_valueSelection->contains(key);
    }

    void StructuredFieldView::showValueCopyMenu(const QPoint& position)
    {
        if (!m_valueTree) return;
        const QModelIndex clicked = m_valueTree->indexAt(position);
        if (clicked.isValid() && !m_valueTree->selectionModel()->isSelected(clicked)) m_valueTree->setCurrentIndex(clicked);
        const auto snapshot = captureCopy(clicked);
        const QString selected = selectedText();
        QMenu menu;
        menu.setPalette(palette());
        menu.setFont(font());
        connect(this, &QObject::destroyed, &menu, &QMenu::close);
        auto* value = menu.addAction(translated(QStringLiteral("复制值")));
        auto* row = menu.addAction(translated(QStringLiteral("复制整行")));
        auto* selection = menu.addAction(translated(QStringLiteral("复制选中项")));
        menu.addSeparator();
        auto* all = menu.addAction(translated(QStringLiteral("复制全部")));
        value->setEnabled(snapshot.hasRow);
        row->setEnabled(snapshot.hasRow);
        selection->setEnabled(!selected.isEmpty());
        all->setEnabled(!snapshot.all.isEmpty());
        const QPoint global = m_valueTree->viewport()->mapToGlobal(position);
        const QPointer<StructuredFieldView> guard(this);
        const auto chosen = menu.exec(global);
        if (!guard) return;
        if (chosen == value) copyText(snapshot.value);
        else if (chosen == row) copyText(snapshot.row);
        else if (chosen == selection) copyText(selected);
        else if (chosen == all) copyText(snapshot.all);
    }

    // 卡片详情保留原报告对象和工具信号，但不再同时维护第二份隐藏树模型。
    void StructuredFieldView::setDocumentOnly()
    {
        if (m_documentOnly)
        {
            return;
        }
        m_documentOnly = true;
        m_valueClearPending = false;
        retireFieldSelection(std::move(m_valueSelection));
        retireFieldProjection({}, std::move(m_valueExpansion));
        m_valueRows.clear();
        ++m_valueTicket;
        if (m_valueCancel) m_valueCancel->store(true, std::memory_order_relaxed);
        delete m_valueTree;
        m_valueTree = nullptr;
        if (m_valueModel)
        {
            auto old = m_valueModel->takeSnapshotForDisposal();
            retireFieldProjection(std::move(old), {}, std::move(m_valuePublishedTranslate));
            delete m_valueModel;
            m_valueModel = nullptr;
        }
        delete m_valueLoading;
        m_valueLoading = nullptr;
        m_hiddenTreeDirty = false;
        m_hiddenTreeLayoutDirty = false;
        m_presentation = Presentation::Sections;
        m_searchActive = false;
        m_expansionBeforeSearch.clear();
        if (m_tree)
        {
            QTreeWidget* obsoleteTree = m_tree; // 先断开成员指针，析构事件或排队回调不得再拿旧树。
            m_tree = nullptr;
            obsoleteTree->viewport()->removeEventFilter(this);
            delete obsoleteTree;
        }
        QComboBox* obsoleteStyle = m_style; // 旧格式控件永久退出详情投影。
        m_style = nullptr;
        delete obsoleteStyle;
        m_title->hide();
    }

    void StructuredFieldView::setHiddenProjectionDeferred(const bool enabled)
    {
        if (m_deferHiddenProjection == enabled) return;
        m_deferHiddenProjection = enabled;
        if (!enabled)
        {
            // 恢复同步策略时立即补齐模型，不能把隐藏阶段的旧树暴露给 tree() 用户。
            if (m_hiddenTreeDirty) rebuild();
            else if (m_hiddenTreeLayoutDirty)
            {
                m_hiddenTreeLayoutDirty = false;
                applyPresentation();
            }
        }
    }

    void StructuredFieldView::setDocument(const FieldDocument& document, const bool resetPublished)
    {
        const bool sameDocument = (m_document.isEmpty() && document.isEmpty())
            || (m_document.title == document.title
                && (m_document.nodes.isSharedWith(document.nodes)
                    || (!m_valueTree && m_document.nodes == document.nodes)));
        if (sameDocument && !resetPublished)
        {
            if (!m_valueTree || !document.isEmpty() || !m_valuePublished) return;
            const auto displayed = m_valueModel->snapshot();
            if (!displayed || displayed->document.isEmpty()) return; // 已发布空结果仍保持等值 no-op。
        }
        // 清空/业务换对象是显式语义，不从字段数量或内容猜测身份；判断只有 O(1)。
        const bool resetValueProjection = m_valueTree && (resetPublished || document.isEmpty());
        if (resetValueProjection) m_valuePublished = false;
        if (sameDocument)
        {
            if (resetValueProjection) rebuild(); // 只在明确重置或仍展示非空旧结果时取消旧回执。
            return;
        }
        if (m_valueTree)
        {
            // 新值模型的比较和旧大文档释放都在 worker；隐藏页只交换隐式共享规范值。
            FieldDocument retired = std::move(m_document);
            m_document = document;
            QThreadPool::globalInstance()->start([retired = std::move(retired)]() mutable
                { retired = {}; });
        }
        else m_document = document;
        ++m_documentRevision;
        if (!m_documentOnly)
        {
            rebuild();
        }
        if (!m_documentNotificationPending)
        {
            m_documentNotificationPending = true;
            QTimer::singleShot(0, this, [this]
            {
                m_documentNotificationPending = false;
                emit documentChanged();
            });
        }
    }

    void StructuredFieldView::rebuild()
    {
        if (m_valueTree)
        {
            scheduleValueProjection();
            return;
        }
        if (m_documentOnly || !m_tree)
        {
            return;
        }
        if (shouldDeferHiddenTree())
        {
            m_hiddenTreeDirty = true;
            return; // 文档/导出代次仍即时更新，隐藏Tab不构造旧模型和逐字段测量。
        }
        m_hiddenTreeDirty = false;
        m_hiddenTreeLayoutDirty = false;
        QHash<QString, bool> expanded;
        QSet<QString> selected;
        QString current;
        const int scroll = m_tree->verticalScrollBar()->value();
        walkItems(m_tree->invisibleRootItem(), [&](QTreeWidgetItem* item)
        {
            const auto key = item->data(0, KeyRole).toString();
            expanded.insert(key, item->isExpanded());
            if (item->isSelected()) selected.insert(key);
            if (item == m_tree->currentItem()) current = key;
        });
        QSignalBlocker blocker(m_tree);
        m_tree->setUpdatesEnabled(false);
        m_tree->clear();
        QTreeWidgetItem* newCurrent = nullptr;
        std::function<void(const QVector<FieldNode>&, QTreeWidgetItem*, const QString&)> append;
        append = [&](const QVector<FieldNode>& nodes, QTreeWidgetItem* parent, const QString& parentKey)
        {
            QHash<QString, int> occurrences;
            for (const auto& node : nodes)
            {
                const QString sourceKey = QString::number(static_cast<int>(node.kind)) + QLatin1Char(':')
                    + (node.kind == FieldNode::Kind::Note ? node.value : node.name);
                const int occurrence = occurrences[sourceKey]++;
                const QString key = parentKey + QString::number(sourceKey.size()) + QLatin1Char(':')
                    + sourceKey + QLatin1Char(':') + QString::number(occurrence) + QLatin1Char('/');
                auto* item = new QTreeWidgetItem(parent);
                const QString name = node.translateName ? translated(node.name) : node.name;
                const QString value = node.translateValue ? translated(node.value) : node.value;
                QString row;
                if (node.kind == FieldNode::Kind::Section)
                {
                    item->setText(0, name);
                    row = QLatin1Char('[') + name + QLatin1Char(']');
                }
                else if (node.kind == FieldNode::Kind::Note)
                {
                    item->setText(0, translated(node.value));
                    row = translated(node.value);
                }
                else
                {
                    item->setText(0, name);
                    item->setText(1, value);
                    row = name + QStringLiteral(": ") + value;
                }
                for (int column = 0; column < 2; ++column)
                {
                    item->setData(column, KindRole, static_cast<int>(node.kind));
                    item->setData(column, KeyRole, key);
                    item->setData(column, RowRole, row);
                    item->setData(column, ValueRole, node.kind == FieldNode::Kind::Field ? value : item->text(0));
                    item->setToolTip(column, column == 0 ? item->text(0) : value);
                }
                item->setFirstColumnSpanned(node.kind != FieldNode::Kind::Field);
                append(node.children, item, key);
                const bool defaultExpanded = !node.children.isEmpty() && node.initiallyExpanded;
                item->setExpanded(expanded.value(key, defaultExpanded));
                if (m_searchActive && !m_expansionBeforeSearch.contains(key))
                    m_expansionBeforeSearch.insert(key, defaultExpanded);
                if (selected.contains(key)) item->setSelected(true);
                if (key == current) newCurrent = item;
            }
        };
        append(m_document.nodes, m_tree->invisibleRootItem(), QString());
        if (newCurrent) m_tree->setCurrentItem(newCurrent, 0, QItemSelectionModel::NoUpdate);
        m_title->setText(translated(m_document.title));
        m_title->setVisible(!m_document.title.isEmpty());
        applySearch();
        updateColumns();
        m_tree->setUpdatesEnabled(true);
        m_tree->doItemsLayout();
        m_tree->verticalScrollBar()->setValue(scroll);
    }

    FieldDocument StructuredFieldView::actionDocument() const
    {
        if (m_valueTree && m_valuePublished)
        {
            const auto displayed = m_valueModel->snapshot();
            if (displayed) return displayed->document; // COW 值引用既有快照，不遍历或另建规范源。
        }
        return m_document; // 原生树、document-only 和首份加载保留原有动作语义。
    }

    std::function<QString(const QString&)> StructuredFieldView::actionTranslator() const
    {
        return m_valueTree && m_valuePublished ? m_valuePublishedTranslate : std::function<QString(const QString&)>{};
    }

    QString StructuredFieldView::actionPlainText() const
    {
        const auto translate = actionTranslator();
        return translate ? actionDocument().toPlainText(translate) : actionDocument().toPlainText(true);
    }

    QString StructuredFieldView::plainText() const
    {
        return m_document.toPlainText(true);
    }

    bool StructuredFieldView::exportText(const QString& filePath, QString* error) const
    {
        return writeDocumentText(m_document, filePath, error);
    }

    QString StructuredFieldView::exportItems(const bool selectedOnly) const
    {
        if (m_valueTree)
        {
            if (!selectedOnly) return actionPlainText();
            flushValueSelection();
            const auto snapshot = m_valueModel->snapshot();
            return snapshot && m_valueSelection ? m_valueSelection->text(*snapshot) : QString();
        }
        if (m_documentOnly || !m_tree)
        {
            return selectedOnly ? QString() : plainText();
        }
        QStringList lines;
        walkItems(m_tree->invisibleRootItem(), [&](QTreeWidgetItem* item)
        {
            if (!selectedOnly || item->isSelected()) lines.append(item->data(0, RowRole).toString());
        });
        return lines.join(QLatin1Char('\n'));
    }

    QString StructuredFieldView::selectedText() const { return exportItems(true); }

    FieldCopySnapshot StructuredFieldView::captureCopy(const QModelIndex& clicked) const
    {
        FieldCopySnapshot snapshot;
        snapshot.all = actionPlainText();
        if (clicked.isValid() && ((m_tree && clicked.model() == m_tree->model())
            || (m_valueTree && clicked.model() == m_valueTree->model())))
        {
            snapshot.hasRow = true;
            snapshot.value = clicked.data(ValueRole).toString();
            snapshot.row = clicked.data(RowRole).toString();
        }
        return snapshot;
    }

    QString StructuredFieldView::searchText() const { return m_search->text(); }
    void StructuredFieldView::setSearchText(const QString& text) { m_search->setText(text); }

    void StructuredFieldView::applySearch()
    {
        if (m_valueTree)
        {
            const bool active = !m_search->text().trimmed().isEmpty();
            if (active && !m_valueSearchActive)
            {
                m_valueBeforeSearchSelected = m_valueTree->currentIndex().data(KeyRole).toString();
                m_valueBeforeSearchColumn = std::max(0, m_valueTree->currentIndex().column());
                m_valueBeforeSearchScroll = m_valueTree->verticalScrollBar()->value();
            }
            else if (!active && m_valueSearchActive)
            {
                m_valueRestoreSelection = true;
                m_valueSelected = m_valueBeforeSearchSelected;
                m_valueColumn = m_valueBeforeSearchColumn;
                m_valueScroll = m_valueBeforeSearchScroll;
            }
            m_valueSearchActive = active;
            scheduleValueProjection();
            return;
        }
        if (m_documentOnly || !m_tree)
        {
            return; // 搜索信号仍由同一输入发出，筛选由唯一卡片宿主处理。
        }
        if (shouldDeferHiddenTree())
        {
            m_hiddenTreeDirty = true;
            return; // 保留搜索框的最新查询，显示时从最新文档一次重建并筛选。
        }
        const QString query = m_search->text().trimmed();
        const bool active = !query.isEmpty();
        if (active && !m_searchActive)
        {
            m_expansionBeforeSearch.clear();
            walkItems(m_tree->invisibleRootItem(), [&](QTreeWidgetItem* item)
            {
                m_expansionBeforeSearch.insert(item->data(0, KeyRole).toString(), item->isExpanded());
            });
        }
        std::function<bool(QTreeWidgetItem*, bool)> filter = [&](QTreeWidgetItem* item, const bool parentMatches)
        {
            const bool matches = !active || parentMatches ||
                item->data(0, RowRole).toString().contains(query, Qt::CaseInsensitive);
            bool childMatches = false;
            for (int index = 0; index < item->childCount(); ++index)
                childMatches = filter(item->child(index), matches) || childMatches;
            const bool visible = matches || childMatches;
            item->setHidden(!visible);
            if (active && childMatches) item->setExpanded(true);
            else if (!active && m_searchActive)
                item->setExpanded(m_expansionBeforeSearch.value(item->data(0, KeyRole).toString(), item->isExpanded()));
            return visible;
        };
        for (int index = 0; index < m_tree->topLevelItemCount(); ++index)
            filter(m_tree->topLevelItem(index), false);
        m_searchActive = active;
        if (!active) m_expansionBeforeSearch.clear();
    }

    void StructuredFieldView::setPresentation(const Presentation presentation)
    {
        if (m_documentOnly)
        {
            return; // 已删除的旧视图不再参与会话格式状态，也不重建被淘汰的树。
        }
        m_presentation = presentation;
        sessionPresentation() = presentation;
        QSignalBlocker blocker(m_style);
        m_style->setCurrentIndex(presentation == Presentation::Tree ? 1 : 0);
        applyPresentation();
        emit presentationChanged();
    }

    void StructuredFieldView::applyPresentation()
    {
        if (m_valueTree)
        {
            if (shouldDeferHiddenTree()) { m_hiddenTreeLayoutDirty = true; return; }
            const bool sections = m_presentation == Presentation::Sections;
            m_valueTree->setRootIsDecorated(!sections);
            m_valueTree->setIndentation(sections ? 14 : 20);
            m_valueTree->setAlternatingRowColors(!sections);
            static_cast<PropertyDelegate*>(m_valueTree->itemDelegate())->sections = sections;
            updateValueColumns();
            return;
        }
        if (m_documentOnly || !m_tree)
        {
            return;
        }
        if (shouldDeferHiddenTree())
        {
            m_hiddenTreeLayoutDirty = true;
            return;
        }
        const bool sections = m_presentation == Presentation::Sections;
        m_tree->setRootIsDecorated(!sections);
        m_tree->setIndentation(sections ? 14 : 20);
        m_tree->setAlternatingRowColors(!sections);
        static_cast<PropertyDelegate*>(m_tree->itemDelegate())->sections = sections;
        if (m_hiddenTreeDirty)
        {
            return; // Show随后重建最新文档，不先为已过期的旧行安排一次布局。
        }
        updateColumns();
        m_tree->doItemsLayout();
        m_tree->viewport()->update();
    }

    void StructuredFieldView::updateColumns()
    {
        if (m_valueTree) { updateValueColumns(); return; }
        if (m_documentOnly || !m_tree)
        {
            return;
        }
        if (shouldDeferHiddenTree())
        {
            m_hiddenTreeLayoutDirty = true;
            return;
        }
        const int width = m_tree->viewport()->width();
        const int desired = std::clamp(width / 3, 112, 260);
        if (!m_tree->wordWrap())
        {
            // A native no-wrap projection keeps all text available by horizontal
            // scrolling. Model values, selection and copy/export stay unchanged.
            m_tree->header()->setStretchLastSection(false);
            m_tree->header()->setSectionResizeMode(1, QHeaderView::Interactive);
            const QFontMetrics metrics(m_tree->font());
            const auto lineWidth = [&metrics](const QString& text)
            {
                int longest = 0;
                for (const auto& line : text.split(QLatin1Char('\n')))
                    longest = std::max(longest, metrics.horizontalAdvance(line));
                return longest;
            };
            int keyWidth = desired;
            int valueWidth = 80;
            int spanWidth = 0;
            walkItems(m_tree->invisibleRootItem(), [&](QTreeWidgetItem* item)
            {
                const auto kind = static_cast<FieldNode::Kind>(item->data(0, KindRole).toInt());
                if (kind == FieldNode::Kind::Field)
                {
                    int depth = m_tree->rootIsDecorated() ? 1 : 0;
                    for (auto* ancestor = item->parent(); ancestor; ancestor = ancestor->parent()) ++depth;
                    keyWidth = std::max(keyWidth, lineWidth(item->text(0)) + depth * m_tree->indentation() + 24);
                    valueWidth = std::max(valueWidth, lineWidth(item->text(1)) + 24);
                }
                else spanWidth = std::max(spanWidth, lineWidth(item->text(0)) + 24);
            });
            valueWidth = std::max({valueWidth, width - keyWidth, spanWidth - keyWidth});
            m_tree->setColumnWidth(0, keyWidth);
            m_tree->setColumnWidth(1, valueWidth);
            return;
        }
        const int keyWidth = std::min(desired, std::max(64, width - 80));
        m_tree->setColumnWidth(0, keyWidth);
        if (m_tree->header()->sectionResizeMode(1) != QHeaderView::Stretch)
        {
            // Reset the last section before enabling stretch; Qt otherwise
            // retains the large no-wrap width as its stretch minimum.
            m_tree->setColumnWidth(1, std::max(48, width - keyWidth));
            m_tree->header()->setSectionResizeMode(1, QHeaderView::Stretch);
        }
        m_tree->header()->setStretchLastSection(true);
    }

    void StructuredFieldView::updateTranslations()
    {
        if (!m_export || !m_search || !m_copy)
        {
            return; // 构造阶段的主题/语言广播尚未创建工具控件时不访问空指针。
        }
        m_export->setToolTip(translated(QStringLiteral("导出文本")));
        m_export->setAccessibleName(translated(QStringLiteral("导出文本")));
        m_search->setPlaceholderText(initialSearchText(InitialSearchText::Search));
        m_search->setAccessibleName(initialSearchText(InitialSearchText::Search));
        if (m_style)
        {
            m_style->setItemText(0, initialSearchText(InitialSearchText::Sections));
            m_style->setItemText(1, initialSearchText(InitialSearchText::Tree));
            m_style->setAccessibleName(translated(QStringLiteral("显示样式")));
        }
        m_copy->setText(initialSearchText(InitialSearchText::Copy));
        if (m_tree)
        {
            m_tree->setHeaderLabels({initialSearchText(InitialSearchText::Name), initialSearchText(InitialSearchText::Value)});
        }
    }

    void StructuredFieldView::changeEvent(QEvent* event)
    {
        QWidget::changeEvent(event);
        if (event->type() == QEvent::LanguageChange)
        {
            updateTranslations();
            rebuild();
        }
        else if (!m_documentOnly && (m_tree || m_valueTree) &&
            (event->type() == QEvent::FontChange || event->type() == QEvent::ApplicationFontChange ||
                event->type() == QEvent::PaletteChange || event->type() == QEvent::ApplicationPaletteChange))
        {
            if (shouldDeferHiddenTree())
            {
                m_hiddenTreeLayoutDirty = true;
                return; // QApplication换肤仍广播到隐藏旧树，但不触发其全文测量和doItemsLayout。
            }
            updateColumns();
            QTreeView* view = m_valueTree ? m_valueTree : m_tree;
            view->doItemsLayout();
            view->viewport()->update();
        }
    }

    bool StructuredFieldView::belongsToSharedDetail() const
    {
        const QWidget* owner = window(); // 仅共享详情根实际登记后启用，不推断普通隐藏编辑器用途。
        for (const QWidget* parent = this; parent; parent = parent->parentWidget())
        {
            if (parent->property("ksword_detail_keep_layout").toBool()
                || parent->property("ksword_parameter_workspace").toBool())
            {
                return false; // 专业工作台和未投影参数字段仍依赖原生树的同步首显。
            }
            if (parent == owner)
            {
                break;
            }
        }
        return owner && owner->property("ksword_detail_framework_controller").value<QObject*>() != nullptr;
    }

    bool StructuredFieldView::shouldDeferHiddenTree() const
    {
        // 切页Show后的业务currentChanged槽也可能提交新文档；排队fallback前仍不重建待淘汰树。
        return !m_documentOnly && (m_tree || m_valueTree) && (!isVisible() || m_treeShowPending)
            && (m_deferHiddenProjection || belongsToSharedDetail());
    }

    void StructuredFieldView::flushDeferredTree()
    {
        if (m_documentOnly || (!m_tree && !m_valueTree) || !isVisible()
            || (!m_hiddenTreeDirty && !m_hiddenTreeLayoutDirty))
        {
            return;
        }
        m_hiddenTreeLayoutDirty = false;
        applyPresentation();
        if (m_hiddenTreeDirty)
        {
            rebuild();
        }
    }

    // Qt先Show新Stack页再发currentChanged；让同步共享投影先消费文档，避免刚建旧树就删除。
    void StructuredFieldView::showEvent(QShowEvent* event)
    {
        QWidget::showEvent(event);
        if (m_valueTree && (!m_valueRows.isEmpty() || m_valueRootNext < m_valueRootTotal)) prepareValueRows();
        if (m_documentOnly || (!m_tree && !m_valueTree) || (!m_hiddenTreeDirty && !m_hiddenTreeLayoutDirty))
        {
            return;
        }
        if (!belongsToSharedDetail())
        {
            flushDeferredTree(); // 显式普通页在首帧前同步消费最新快照；专业工作台仍按原契约。
            return;
        }
        if (m_treeShowPending)
        {
            return;
        }
        m_treeShowPending = true;
        QTimer::singleShot(0, this, [this]()
        {
            m_treeShowPending = false;
            flushDeferredTree(); // source被销毁则Qt取消回调；已投影、隐藏或清脏时不恢复旧树。
        });
    }

    void StructuredFieldView::resizeEvent(QResizeEvent* event)
    {
        QWidget::resizeEvent(event);
        if (m_valueTree) { updateValueColumns(); return; }
        if (m_documentOnly || !m_tree)
        {
            return;
        }
        if (shouldDeferHiddenTree())
        {
            m_hiddenTreeLayoutDirty = true;
            return;
        }
        updateColumns();
        m_tree->doItemsLayout();
    }

    bool StructuredFieldView::eventFilter(QObject* watched, QEvent* event)
    {
        if (m_valueTree && watched == m_valueTree->viewport() && event->type() == QEvent::Resize)
        { updateValueColumns(); return QWidget::eventFilter(watched, event); }
        // The layout gives the tree its final geometry after the parent's
        // resizeEvent. Refresh from that actual viewport width on the next
        // event turn, once Qt has finished its own resize/layout work.
        if (m_tree && watched == m_tree->viewport() && event->type() == QEvent::Resize && !m_layoutPending)
        {
            if (shouldDeferHiddenTree())
            {
                m_hiddenTreeLayoutDirty = true;
                return QWidget::eventFilter(watched, event);
            }
            m_layoutPending = true;
            QTimer::singleShot(0, this, [this]
            {
                m_layoutPending = false;
                if (m_documentOnly || !m_tree)
                {
                    return; // 排队回调可能发生在接入卡片并销毁旧树之后。
                }
                if (shouldDeferHiddenTree())
                {
                    m_hiddenTreeLayoutDirty = true;
                    return; // 排队期间已切走该Tab时，不能继续为隐藏旧树安排布局。
                }
                updateColumns();
                m_tree->doItemsLayout();
            });
        }
        return QWidget::eventFilter(watched, event);
    }

    void StructuredFieldView::copyText(const QString& text)
    {
        if (!text.isEmpty()) QApplication::clipboard()->setText(text);
    }

    void StructuredFieldView::showCopyMenu(const QPoint& position)
    {
        if (m_documentOnly || !m_tree)
        {
            return;
        }
        const auto clicked = m_tree->indexAt(position);
        if (clicked.isValid())
        {
            auto* item = m_tree->itemAt(position);
            if (item && !item->isSelected()) m_tree->setCurrentItem(item);
        }
        const auto snapshot = captureCopy(clicked);
        const auto selection = selectedText();
        // A parentless stack menu survives a nested event loop that deletes the
        // owning dialog. Only frozen payloads are accessed after exec().
        QMenu menu;
        menu.setPalette(palette());
        menu.setFont(font());
        connect(this, &QObject::destroyed, &menu, &QMenu::close);
        auto* copyValue = menu.addAction(translated(QStringLiteral("复制值")));
        auto* copyRow = menu.addAction(translated(QStringLiteral("复制整行")));
        auto* copySelected = menu.addAction(translated(QStringLiteral("复制选中项")));
        menu.addSeparator();
        auto* copyAll = menu.addAction(translated(QStringLiteral("复制全部")));
        copyValue->setEnabled(snapshot.hasRow);
        copyRow->setEnabled(snapshot.hasRow);
        copySelected->setEnabled(!selection.isEmpty());
        copyAll->setEnabled(!snapshot.all.isEmpty());
        QPointer<StructuredFieldView> guard(this);
        const auto action = menu.exec(m_tree->viewport()->mapToGlobal(position));
        if (!guard) return;
        if (action == copyValue) copyText(snapshot.value);
        else if (action == copyRow) copyText(snapshot.row);
        else if (action == copySelected) copyText(selection);
        else if (action == copyAll) copyText(snapshot.all);
    }
}
