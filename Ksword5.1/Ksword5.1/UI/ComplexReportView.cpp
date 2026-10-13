#include "../Framework.h"
#include "./ComplexReportView.h"
#include "./ComplexReportModel.h"
#include "./ComplexReportDelegate.h"
#include "./FlatButtonTheme.h"
#include "./ThemeBinding.h"
#include "./ToolbarMetrics.h"
#include "./VisibleTableWidget.h"
#include "../Internationalization/LanguageManager.h"
#include "../theme.h"
#include <QApplication>
#include <QClipboard>
#include <QElapsedTimer>
#include <QHBoxLayout>
#include <QLabel>
#include <QMenu>
#include <QPainter>
#include <QPointer>
#include <QResizeEvent>
#include <QScrollBar>
#include <QShortcut>
#include <QShowEvent>
#include <QSignalBlocker>
#include <QSortFilterProxyModel>
#include <QTimer>
#include <QToolButton>
#include <QTreeView>
#include <QVBoxLayout>

namespace
{
    // 摘要只呈现来源显式状态；静态目录的中性状态没有虚假的成功标记。
    QIcon reportStatusIcon(ks::ui::FieldNode::Status status)
    {
        if (status == ks::ui::FieldNode::Status::Neutral) return QIcon();
        QColor color = KswordTheme::InfoColor(); // 未知之外的普通信息状态。
        if (status == ks::ui::FieldNode::Status::Success) color = KswordTheme::SuccessColor();
        if (status == ks::ui::FieldNode::Status::Warning) color = KswordTheme::WarningColor();
        if (status == ks::ui::FieldNode::Status::Error) color = KswordTheme::ErrorColor();
        color = KswordTheme::EnsureTextContrast(color, KswordTheme::SurfaceAltColor(), 3.0);
        QPixmap pixmap(12, 12); // 真实语义色的小圆点，不把整条摘要染成风险色。
        pixmap.fill(Qt::transparent);
        QPainter painter(&pixmap);
        painter.setRenderHint(QPainter::Antialiasing);
        painter.setPen(Qt::NoPen);
        painter.setBrush(color);
        painter.drawEllipse(QRectF(3, 3, 6, 6));
        return QIcon(pixmap);
    }

    // 有展开子项的模型索引统一按原树顺序遍历；没有逐行 QWidget，也不依赖可见窗口截取。
    void visitReport(const QAbstractItemModel* model, const QModelIndex& parent,
        const std::function<void(const QModelIndex&)>& visitor)
    {
        for (int row = 0; row < model->rowCount(parent); ++row)
        {
            const QModelIndex index = model->index(row, 0, parent);
            visitor(index);
            if (model->rowCount(index) > 0) visitReport(model, index, visitor);
        }
    }
}

namespace ks::ui
{
    ComplexReportView::ComplexReportView(QWidget* parent) : QWidget(parent)
    {
        setObjectName(QStringLiteral("ksword_complex_report"));
        setMinimumSize(0, 0);
        setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Ignored);
        auto* root = new QVBoxLayout(this); // 摘要固定，树视口使用剩余高度。
        root->setContentsMargins(0, 0, 0, 0);
        root->setSpacing(8);
        m_summary = new QWidget(this);
        m_summary->setObjectName(QStringLiteral("ksword_report_summary"));
        m_summaryLayout = new QHBoxLayout(m_summary);
        m_summaryLayout->setContentsMargins(8, 4, 8, 4);
        m_summaryLayout->setSpacing(6);
        m_summaryLabel = new QLabel(m_summary);
        m_summaryLayout->addWidget(m_summaryLabel);
        m_more = new QToolButton(m_summary);
        m_more->setPopupMode(QToolButton::InstantPopup);
        m_moreMenu = new QMenu(m_more);
        m_more->setMenu(m_moreMenu);
        m_summaryLayout->addWidget(m_more);
        root->addWidget(m_summary);

        // 同一规范快照产生模型；代理搜索包括折叠后代，命中分区时保留该分区全部子项。
        m_model = new ComplexReportModel(this);
        m_proxy = new QSortFilterProxyModel(this);
        m_proxy->setSourceModel(m_model);
        m_proxy->setFilterRole(ReportSearchRole);
        m_proxy->setFilterCaseSensitivity(Qt::CaseInsensitive);
        m_proxy->setRecursiveFilteringEnabled(true);
        m_proxy->setAutoAcceptChildRows(true);
        m_tree = new ComplexReportTree(this);
        m_tree->setObjectName(QStringLiteral("ksword_report_tree"));
        m_tree->setProperty("ks_i18n_preserve_model_data", true);
        m_tree->setProperty("ks_preserve_table_presentation", true);
        // 树报告已有规范源工具行；共享 setter 仅接受 QTableView，树显式声明同一模式属性。
        m_tree->setProperty(TableActionBarModeProperty, static_cast<int>(TableActionBarMode::None));
        m_tree->setModel(m_proxy);
        m_tree->setHeaderHidden(true);
        m_tree->setFrameShape(QFrame::NoFrame);
        m_tree->setIndentation(20);
        m_tree->setUniformRowHeights(false);
        m_tree->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
        m_tree->setVerticalScrollMode(QAbstractItemView::ScrollPerPixel);
        m_tree->setSelectionBehavior(QAbstractItemView::SelectRows);
        m_tree->setSelectionMode(QAbstractItemView::SingleSelection);
        m_tree->setMouseTracking(true);
        m_tree->setMinimumSize(0, 0);
        m_tree->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Ignored);
        m_delegate = new ComplexReportDelegate(m_tree);
        m_tree->setItemDelegate(m_delegate);
        m_delegate->setCopyHandler([this](const QModelIndex& index) { copyNode(index); });
        m_tree->setContextMenuPolicy(Qt::CustomContextMenu);
        connect(m_tree, &QTreeView::customContextMenuRequested, this, &ComplexReportView::showContextMenu);
        root->addWidget(m_tree, 1);
        m_empty = new QLabel(this);
        m_empty->setObjectName(QStringLiteral("ksword_report_empty_state"));
        m_empty->setWordWrap(true);
        m_empty->setAlignment(Qt::AlignCenter);
        root->addWidget(m_empty, 1);

        // 原投影把此动作放进同一搜索/复制/导出行，动作不随正文或分区滚走。
        m_expandAll = new QToolButton(this);
        m_expandAll->setObjectName(QStringLiteral("ksword_report_expand_all"));
        m_expandAll->setCheckable(true);
        ApplyFlatButtonTheme(m_expandAll, FlatButtonTone::Neutral, FlatButtonAppearance::Flat);
        connect(m_expandAll, &QToolButton::clicked, this, [this](bool expanded)
        {
            m_bulkExpansion = true;
            if (expanded) m_tree->expandAll();
            else m_tree->collapseAll();
            m_bulkExpansion = false;
            refreshTheme();
        });
        connect(m_tree, &QTreeView::collapsed, this, [this]()
        {
            if (!m_bulkExpansion && m_expandAll->isChecked())
            {
                const QSignalBlocker blocker(m_expandAll);
                m_expandAll->setChecked(false);
                refreshTheme();
            }
        });
        auto* copyShortcut = new QShortcut(QKeySequence::Copy, m_tree); // 仅该树及其内部有焦点时复制。
        copyShortcut->setContext(Qt::WidgetWithChildrenShortcut);
        connect(copyShortcut, &QShortcut::activated, this, [this]() { copyNode(m_tree->currentIndex()); });
        const QPointer<ComplexReportView> safe(this); // 主题刷新保留模型、选区、展开和文档身份。
        BindWidgetTheme(this, [safe]() { if (safe) safe->refreshTheme(); });
        refreshTheme();
    }

    QSet<QString> ComplexReportView::expandedPaths() const
    {
        QSet<QString> result; // 以规范路径保存状态，不持有可能被重置的 QModelIndex。
        visitReport(m_proxy, QModelIndex(), [this, &result](const QModelIndex& index)
        {
            if (m_tree->isExpanded(index)) result.insert(index.data(ReportPathRole).toString());
        });
        return result;
    }

    void ComplexReportView::restoreExpansion(const QSet<QString>& paths)
    {
        m_bulkExpansion = true;
        for (const QString& path : paths)
        {
            const QModelIndex index = m_proxy->mapFromSource(m_model->indexForPath(path));
            if (index.isValid()) m_tree->setExpanded(index, true);
        }
        m_bulkExpansion = false;
    }

    void ComplexReportView::setDocument(const FieldDocument& document)
    {
        QElapsedTimer elapsed; // 计入状态保存、模型提交和摘要更新，不漏掉外围走读成本。
        elapsed.start();
        QSet<QString> expanded;
        const QString selected = m_tree->currentIndex().data(ReportPathRole).toString();
        const int scroll = m_tree->verticalScrollBar()->value();
        QSet<QString> oldPaths;
        // 只有真正重置拓扑时才走读旧树；普通数值/状态刷新不全量保存展开状态。
        const auto resetConnection = connect(m_model, &QAbstractItemModel::modelAboutToBeReset, this, [&]()
        {
            expanded = expandedPaths();
            visitReport(m_model, QModelIndex(), [&oldPaths](const QModelIndex& index)
            {
                oldPaths.insert(index.data(ReportPathRole).toString());
            });
        }, Qt::DirectConnection);
        const bool reset = m_model->setDocument(document);
        disconnect(resetConnection); // 同步重置信号已经结束，不保留局部引用。
        const QString language = ks::i18n::LanguageManager::instance().currentLanguageId();
        if (m_language != language)
        {
            m_model->relocalize();
            m_language = language;
        }
        if (reset)
        {
            restoreExpansion(expanded);
            m_bulkExpansion = true;
            for (const QModelIndex& index : m_model->initialExpansion())
            {
                const QString path = index.data(ReportPathRole).toString();
                if (!oldPaths.contains(path))
                {
                    if (m_query.isEmpty())
                        m_tree->setExpanded(m_proxy->mapFromSource(index), true);
                    else m_beforeSearch.insert(path); // 搜索期间到达的新分区，清空后也保留其默认状态。
                }
            }
            m_bulkExpansion = false;
            const QModelIndex current = m_proxy->mapFromSource(m_model->indexForPath(selected));
            if (current.isValid()) m_tree->setCurrentIndex(current);
            m_tree->verticalScrollBar()->setValue(scroll);
        }
        if (!m_query.isEmpty())
        {
            // 普通值更新也可能产生新的搜索命中，不能只在重置模型时展开匹配祖先。
            m_bulkExpansion = true;
            m_tree->expandAll();
            m_bulkExpansion = false;
        }
        updateSummary();
        updateEmptyState();
        setProperty("ksword_report_update_us", elapsed.nsecsElapsed() / 1000);
    }

    void ComplexReportView::setSearchText(const QString& query)
    {
        const QString next = query.trimmed(); // 原搜索字段仍负责输入和完整文档导出。
        if (next == m_query) return;
        if (m_query.isEmpty() && !next.isEmpty())
        {
            m_beforeSearch = expandedPaths();
            m_beforeSearchSelected = m_tree->currentIndex().data(ReportPathRole).toString();
            m_beforeSearchScroll = m_tree->verticalScrollBar()->value();
        }
        m_query = next;
        m_proxy->setFilterFixedString(next);
        m_bulkExpansion = true;
        if (!next.isEmpty()) m_tree->expandAll();
        else
        {
            m_tree->collapseAll();
            restoreExpansion(m_beforeSearch);
            const QModelIndex selected = m_proxy->mapFromSource(m_model->indexForPath(m_beforeSearchSelected));
            if (selected.isValid()) m_tree->setCurrentIndex(selected);
            m_tree->doItemsLayout(); // 先恢复真实行范围，再恢复阅读位置，避免旧过滤范围把值夹到零。
            m_tree->verticalScrollBar()->setValue(m_beforeSearchScroll);
            m_beforeSearch.clear();
            m_beforeSearchSelected.clear();
        }
        m_bulkExpansion = false;
        updateEmptyState();
    }

    void ComplexReportView::updateEmptyState()
    {
        const bool empty = m_proxy->rowCount() == 0;
        m_empty->setText(ks::i18n::sourceText(m_query.isEmpty()
            ? QStringLiteral("暂无报告数据") : QStringLiteral("没有匹配的属性")));
        m_tree->setVisible(!empty);
        m_empty->setVisible(empty);
    }

    void ComplexReportView::updateSummary()
    {
        const auto sections = m_model->sectionIndexes(); // 仅少量顶层分区创建跳转按钮。
        while (m_sections.size() > sections.size())
        {
            auto* button = m_sections.takeLast();
            m_summaryLayout->removeWidget(button);
            delete button;
        }
        while (m_sections.size() < sections.size())
        {
            auto* button = new QToolButton(m_summary);
            button->setProperty("ksword_theme_icon_managed", true); // 状态点由实际来源着色，通用图标主题不能改成蓝色。
            button->setToolButtonStyle(Qt::ToolButtonTextBesideIcon); // 状态与分区名同时可读，不能退化成孤立色点。
            button->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);
            ApplyFlatButtonTheme(button, FlatButtonTone::Neutral, FlatButtonAppearance::Flat);
            NormalizeToolbarControl(button);
            connect(button, &QToolButton::clicked, this, [this, button]()
            {
                jumpToSection(button->property("ksword_report_section_path").toString());
            });
            m_summaryLayout->insertWidget(m_summaryLayout->count() - 1, button);
            m_sections.append(button);
        }
        m_moreMenu->clear();
        for (int row = 0; row < sections.size(); ++row)
        {
            const QModelIndex index = sections.at(row);
            const QString title = index.data(ReportNameRole).toString();
            const QString path = index.data(ReportPathRole).toString();
            const QString shortTitle = index.data(ReportSummaryRole).toString();
            const QIcon icon = reportStatusIcon(static_cast<FieldNode::Status>(index.data(ReportStatusRole).toInt()));
            auto* button = m_sections.at(row);
            button->setText(button->fontMetrics().elidedText(shortTitle.isEmpty() ? title : shortTitle, Qt::ElideRight, 140));
            button->setToolTip(title);
            button->setIcon(icon);
            button->setProperty("ksword_report_section_path", path);
            auto* action = m_moreMenu->addAction(icon, title);
            connect(action, &QAction::triggered, this, [this, path]() { jumpToSection(path); });
        }
        m_summary->setVisible(!sections.isEmpty());
        fitSummary();
    }

    void ComplexReportView::fitSummary()
    {
        int remaining = m_summary->width() - 16 - m_summaryLabel->sizeHint().width()
            - m_more->sizeHint().width() - 12; // 总览过长时进入同一更多菜单，不出现横向滚动。
        bool overflow = false;
        for (auto* button : m_sections)
        {
            const int needed = button->sizeHint().width() + 6;
            const bool visible = !overflow && needed <= remaining;
            button->setVisible(visible);
            if (visible) remaining -= needed;
            else overflow = true;
        }
        m_more->setVisible(overflow);
    }

    void ComplexReportView::jumpToSection(const QString& path)
    {
        const QModelIndex index = m_proxy->mapFromSource(m_model->indexForPath(path));
        if (!index.isValid()) return; // 被搜索过滤的分区不能跳到错误行。
        m_tree->setExpanded(index, true);
        m_tree->setCurrentIndex(index);
        m_tree->scrollTo(index, QAbstractItemView::PositionAtTop);
    }

    void ComplexReportView::copyNode(const QModelIndex& index)
    {
        if (!index.isValid()) return;
        const QString text = m_model->textForIndex(m_proxy->mapToSource(index)); // 点击时取完整规范子树。
        if (auto* clipboard = QApplication::clipboard()) clipboard->setText(text);
    }

    void ComplexReportView::showContextMenu(const QPoint& position)
    {
        const QModelIndex index = m_tree->indexAt(position);
        if (!index.isValid()) return;
        const QString text = m_model->textForIndex(m_proxy->mapToSource(index)); // 菜单前保存，刷新不能使索引悬空。
        const QPoint globalPosition = m_tree->viewport()->mapToGlobal(position);
        QMenu menu; // 嵌套 exec 中父窗可销毁，栈上菜单不能再交给父对象 delete。
        connect(this, &QObject::destroyed, &menu, &QMenu::close);
        menu.setStyleSheet(KswordTheme::ContextMenuStyle());
        auto* copy = menu.addAction(ks::i18n::sourceText(QStringLiteral("复制")));
        if (menu.exec(globalPosition) == copy)
            if (auto* clipboard = QApplication::clipboard()) clipboard->setText(text);
    }

    void ComplexReportView::refreshTheme()
    {
        m_summaryLabel->setText(ks::i18n::sourceText(QStringLiteral("摘要")));
        m_more->setText(ks::i18n::sourceText(QStringLiteral("更多分区")));
        m_more->setToolTip(m_more->text());
        m_expandAll->setText(ks::i18n::sourceText(m_expandAll->isChecked()
            ? QStringLiteral("全部收起") : QStringLiteral("全部展开")));
        m_expandAll->setToolTip(m_expandAll->text());
        const QString menuStyle = KswordTheme::ContextMenuStyle();
        if (m_moreMenu->styleSheet() != menuStyle) m_moreMenu->setStyleSheet(menuStyle);
        const QString style = QStringLiteral(
            "QWidget#ksword_report_summary{background:%1;border:0;border-radius:8px;}"
            "QTreeView#ksword_report_tree{background:%2;border:0;outline:0;}"
            "QTreeView#ksword_report_tree::branch{background:transparent;}")
            .arg(KswordTheme::SurfaceAltColorHex(), KswordTheme::WindowColorHex());
        // 精确声明共享非按钮表面块，不能让容器底色被误判为页面接管了按钮交互色。
        setProperty("ksword_primary_style_block", style);
        if (styleSheet() != style) setStyleSheet(style);
        QPalette palette = m_tree->palette(); // 原生分支与焦点同样拥有可读前景。
        palette.setColor(QPalette::Base, KswordTheme::WindowColor());
        palette.setColor(QPalette::Text, KswordTheme::TextPrimaryColor());
        palette.setColor(QPalette::WindowText, KswordTheme::TextPrimaryColor());
        if (m_tree->palette() != palette) m_tree->setPalette(palette);
        m_tree->viewport()->update();
        updateSummary();
        updateEmptyState();
    }

    void ComplexReportView::resizeEvent(QResizeEvent* event)
    {
        QWidget::resizeEvent(event);
        if (!m_resizeLayoutPending)
        {
            m_resizeLayoutPending = true;
            QTimer::singleShot(30, this, [this]()
            {
                m_resizeLayoutPending = false;
                m_delegate->invalidateMetrics();
                m_tree->doItemsLayout(); // 连续缩放合并重测，不逐个 Resize 冷测展开全文。
            });
        }
        fitSummary();
    }

    void ComplexReportView::showEvent(QShowEvent* event)
    {
        QWidget::showEvent(event);
        fitSummary();
    }
}
