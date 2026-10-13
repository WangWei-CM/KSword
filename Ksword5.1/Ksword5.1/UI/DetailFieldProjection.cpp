#include "./DetailPresentation.h"
#include "./StructuredFieldView.h"
#include "./PrimaryPageStyle.h"
#include "./ThemeBinding.h"
#include "./ToolbarMetrics.h"
#include "./MainSurfaceStyle.h"
#include "../Internationalization/LanguageManager.h"
#include "../theme.h"
#include <QApplication>
#include <QClipboard>
#include <QFormLayout>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QPointer>
#include <QScrollArea>
#include <QScrollBar>
#include <QShowEvent>
#include <QStyle>
#include <QToolButton>
#include <QVBoxLayout>
#include <QTimer>
#include <algorithm>

namespace
{
    struct DisplayField
    {
        QString name; // 已按原文档翻译策略处理的字段名。
        QString value; // 真实字段值或说明，不从控件反向拼装。
        bool note = false; // 说明整行展示，不伪装成可编辑字段。
        QString semantic; // 原字段值的状态，只用于显示颜色，不改变值或业务状态。
        QString searchContext; // 保留父字段匹配继承语义，包括父字段名和值。
    };
    struct DisplaySection
    {
        QString title; // 原始文档分组的本地化标题。
        QVector<DisplayField> fields; // 保留原文档阅读顺序。
    };

    // 文档树展开为共享卡片分区；嵌套字段保留完整名称链，不丢弃原树中的子数据。
    void flattenFields(const QVector<ks::ui::FieldNode>& nodes, QVector<DisplayField>& fields,
        const QString& prefix = QString(), const QString& ancestors = QString())
    {
        for (const auto& node : nodes)
        {
            const QString name = node.translateName ? ks::i18n::sourceText(node.name) : node.name;
            const QString path = prefix.isEmpty() ? name : prefix + QStringLiteral(" / ") + name;
            const QString value = node.translateValue ? ks::i18n::sourceText(node.value) : node.value;
            const QString context = ancestors + QLatin1Char('\n') + name + QLatin1Char(' ') + value;
            if (node.kind == ks::ui::FieldNode::Kind::Field)
            {
                const QString state = node.value == QStringLiteral("Enabled") || node.value == QStringLiteral("Enabled (permanent)")
                    || (node.name == QStringLiteral("管理员") && node.value == QStringLiteral("是")) ? QStringLiteral("success")
                    : node.value == QStringLiteral("Disabled") || (node.name == QStringLiteral("管理员") && node.value == QStringLiteral("否"))
                        ? QStringLiteral("error") : QString();
                fields.append({path, value, false, state, context});
            }
            else if (node.kind == ks::ui::FieldNode::Kind::Note)
            {
                fields.append({QString(), ks::i18n::sourceText(node.value), true, QString(), context});
            }
            flattenFields(node.children, fields,
                node.kind == ks::ui::FieldNode::Kind::Note ? prefix : path, context);
        }
    }

    // 原文档只有字段时也拥有一张卡片；分组标题及所有层级的值都保留。
    QVector<DisplaySection> displaySections(const ks::ui::FieldDocument& document)
    {
        QVector<DisplaySection> sections;
        QVector<ks::ui::FieldNode> loose;
        const auto appendLoose = [&]()
        {
            if (loose.isEmpty())
            {
                return;
            }
            DisplaySection section{ks::i18n::sourceText(document.title), {}};
            flattenFields(loose, section.fields);
            sections.append(section);
            loose.clear();
        };
        for (const auto& node : document.nodes)
        {
            if (node.kind != ks::ui::FieldNode::Kind::Section)
            {
                loose.append(node);
                continue;
            }
            appendLoose();
            DisplaySection section{node.translateName ? ks::i18n::sourceText(node.name) : node.name, {}};
            flattenFields(node.children, section.fields);
            sections.append(section);
        }
        appendLoose();
        return sections;
    }

    class DetailFieldProjection final : public QWidget
    {
    public:
        DetailFieldProjection(ks::ui::StructuredFieldView* source, QWidget* parent, bool scrollBody)
            : QWidget(parent), m_source(source)
        {
            const bool embeddedSource = parent == source; // 原source本身是Tab/Stack页时保留其业务身份。
            // 唯一详情视图是分组卡片；先销毁旧格式控件与树，停止隐藏树的构建和列测量。
            source->setDocumentOnly();
            setProperty("ksword_detail_projection", true);
            setProperty("ksword_detail_projection_embedded_source", embeddedSource);
            setProperty("ksword_detail_projection_scroll", scrollBody);
            setMinimumSize(0, 0);
            setSizePolicy(QSizePolicy::Ignored, scrollBody ? QSizePolicy::Ignored : QSizePolicy::Preferred);
            auto* root = new QVBoxLayout(this);
            root->setContentsMargins(0, 0, 0, 0);
            root->setSpacing(12);
            auto* tools = new QHBoxLayout();
            root->addLayout(tools);
            // 原搜索、复制和导出控件直接复用，所有既有信号和文档导出语义保留。
            m_search = source->findChild<QLineEdit*>();
            if (m_search)
            {
                tools->addWidget(m_search, 1);
                ks::ui::StyleStandaloneInput(m_search);
                connect(m_search, &QLineEdit::textChanged, this, [this]() { scheduleRefresh(); });
            }
            for (auto* button : source->findChildren<QToolButton*>(QString(), Qt::FindDirectChildrenOnly))
            {
                tools->addWidget(button);
                button->show();
            }
            ks::ui::NormalizeToolbarRow(tools);
            if (m_search)
            {
                m_search->show();
            }
            m_body = new QWidget(this);
            m_body->setProperty("ksword_detail_document_body", true);
            m_body->setMinimumWidth(0);
            m_body->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
            m_cards = new QVBoxLayout(m_body);
            m_cards->setContentsMargins(0, 0, 0, 0);
            m_cards->setSpacing(14);
            m_cards->setAlignment(Qt::AlignTop);
            if (scrollBody)
            {
                m_scroll = new QScrollArea(this);
                m_scroll->setFrameShape(QFrame::NoFrame);
                m_scroll->setWidgetResizable(true);
                m_scroll->setMinimumSize(0, 0);
                m_scroll->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Ignored);
                m_scroll->setWidget(m_body);
                root->addWidget(m_scroll, 1);
            }
            else
            {
                root->addWidget(m_body);
            }
            source->setProperty("ksword_detail_projection_source", true);
            if (!embeddedSource)
            {
                source->setParent(this);
                source->hide();
            }
            connect(source, &ks::ui::StructuredFieldView::documentChanged, this, [this]() { scheduleRefresh(); });
            const QPointer<DetailFieldProjection> safe(this);
            ks::ui::BindWidgetTheme(this, [safe]()
            {
                if (safe)
                {
                    safe->scheduleRefresh();
                }
            });
            scheduleRefresh();
        }
    protected:
        void changeEvent(QEvent* event) override
        {
            QWidget::changeEvent(event);
            if (event->type() == QEvent::LanguageChange)
            {
                // 等语言广播结束再更新卡片，避免广播遍历中同步销毁旧标题/字段控件。
                ++m_translationRevision;
                scheduleRefresh();
            }
        }
        // 首次显示和切回隐藏页时，首帧直接使用最新文档，不先画旧内容再排队重建。
        void showEvent(QShowEvent* event) override
        {
            QWidget::showEvent(event);
            m_dirty = true;
            refresh();
        }
    private:
        struct Row
        {
            QPointer<QLabel> label; // 字段名；说明没有字段名标签。
            QPointer<QLabel> value; // 可选择/复制的值，数据刷新时原位更新。
            QPointer<QFormLayout> form; // 显示隐藏由原表单拥有的行处理。
            QString searchContext;
            QString semantic; // 主题单独变化时无需再次展开规范文档即可校准值颜色。
            bool visible = true; // 过滤可见性缓存，数值刷新不重新安排整张卡片的布局。
        };
        struct Card
        {
            QPointer<QWidget> widget; // 一个分区仅有一层圆角实底。
            QVector<Row> rows; // 与当前文档分区的字段顺序一一对应。
            QString title; // 搜索标题时保留原结构化视图的整组匹配语义。
            bool visible = true; // 只在匹配状态变化时显隐整组。
        };
        // 文档、主题、语言与搜索共同排队；隐藏Tab只保留dirty，不构造或遍历卡片。
        void scheduleRefresh()
        {
            m_dirty = true;
            if (m_refreshPending || m_refreshing || !isVisible())
            {
                return;
            }
            m_refreshPending = true;
            QTimer::singleShot(0, this, [this]()
            {
                m_refreshPending = false;
                refresh();
            });
        }

        void refresh()
        {
            if (!m_source || !m_dirty || !isVisible() || m_refreshing)
            {
                return;
            }
            const quint64 revision = m_source->documentRevision(); // 当前完整规范快照身份。
            const QString language = ks::i18n::LanguageManager::instance().currentLanguageId();
            const QString theme = projectionCanvasColor().name() + KswordTheme::SurfaceColorHex()
                + KswordTheme::TextPrimaryColorHex() + KswordTheme::TextSecondaryColorHex()
                + KswordTheme::SuccessColor().name(QColor::HexArgb)
                + KswordTheme::ErrorColor().name(QColor::HexArgb);
            const QString query = m_search ? m_search->text().trimmed() : QString();
            const quint64 translationRevision = m_translationRevision; // 同语言包重新翻译也只处理一次。
            const bool dataChanged = !m_snapshotReady || revision != m_documentRevision || language != m_language
                || translationRevision != m_renderedTranslationRevision;
            const bool themeChanged = !m_snapshotReady || theme != m_theme;
            const bool searchChanged = !m_snapshotReady || query != m_query;
            m_dirty = false;
            if (!dataChanged && !themeChanged && !searchChanged)
            {
                return; // 首次theme回调或旧documentChanged到达时，当前快照已经展示完毕。
            }
            m_refreshing = true;
            if (themeChanged)
            {
                applyProjectionStyle(); // 同一投影只更新一个主题块，避免逐字段重写QSS造成大量repolish。
            }
            // 同一个文档与语言只展开一次；纯主题和搜索请求直接使用现有行绑定。
            const auto sections = dataChanged ? displaySections(m_source->document()) : QVector<DisplaySection>();
            QStringList structure;
            for (const auto& section : sections)
            {
                structure.append(section.title);
                structure.append(QString::number(section.fields.size()));
                for (const auto& field : section.fields)
                {
                    structure.append(field.note ? QStringLiteral("note") : field.name);
                }
            }
            const bool structureChanged = dataChanged && structure != m_structure;
            if (structureChanged)
            {
                m_structure = structure;
                m_bindings.clear();
                while (auto* item = m_cards->takeAt(0))
                {
                    delete item->widget();
                    delete item;
                }
                for (const auto& section : sections)
                {
                    auto* card = ks::ui::CreateDetailCard(section.title, m_body);
                    QFormLayout* first = nullptr;
                    QFormLayout* second = nullptr;
                    const int fieldCount = static_cast<int>(std::count_if(section.fields.cbegin(), section.fields.cend(),
                        [](const DisplayField& field) { return !field.note; }));
                    if (fieldCount >= 8)
                    {
                        ks::ui::AddDetailFieldColumns(card, first, second);
                    }
                    else if (fieldCount > 0)
                    {
                        first = new QFormLayout();
                        second = first;
                        ks::ui::ConfigureDetailForm(first);
                        static_cast<QVBoxLayout*>(card->layout())->addLayout(first);
                    }
                    QFormLayout* notes = nullptr; // 说明始终全宽，不能被一个空的第二列截成半行。
                    Card binding{card, {}, section.title};
                    const int midpoint = (fieldCount + 1) / 2;
                    int fieldIndex = 0;
                    for (int index = 0; index < section.fields.size(); ++index)
                    {
                        const auto& field = section.fields.at(index);
                        auto* form = fieldIndex < midpoint ? first : second;
                        if (field.note)
                        {
                            if (!notes)
                            {
                                notes = new QFormLayout();
                                ks::ui::ConfigureDetailForm(notes);
                                static_cast<QVBoxLayout*>(card->layout())->addLayout(notes);
                            }
                            form = notes;
                        }
                        else
                        {
                            ++fieldIndex;
                        }
                        auto* value = new QLabel(card);
                        value->setTextFormat(Qt::PlainText);
                        value->setWordWrap(true);
                        value->setMinimumWidth(0);
                        value->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
                        value->setTextInteractionFlags(Qt::TextSelectableByMouse | Qt::TextSelectableByKeyboard);
                        value->setProperty("ks_i18n_preserve_data_text", true);
                        value->setProperty("ksword_detail_field_value", true);
                        value->setProperty("ksword_detail_field_semantic", field.semantic);
                        value->setContextMenuPolicy(Qt::CustomContextMenu);
                        connect(value, &QWidget::customContextMenuRequested, value, [value](const QPoint& point)
                        {
                            const QString snapshot = value->text();
                            QMenu menu;
                            menu.setStyleSheet(KswordTheme::ContextMenuStyle());
                            auto* copy = menu.addAction(ks::i18n::sourceText(QStringLiteral("复制")));
                            if (menu.exec(value->mapToGlobal(point)) == copy && QApplication::clipboard())
                            {
                                QApplication::clipboard()->setText(snapshot);
                            }
                        });
                        QLabel* label = nullptr;
                        if (field.note)
                        {
                            form->addRow(value);
                        }
                        else
                        {
                            label = new QLabel(field.name, card);
                            label->setProperty("ksword_detail_field_label", true);
                            label->setWordWrap(true);
                            label->setMinimumWidth(0);
                            form->addRow(label, value);
                        }
                        binding.rows.append({label, value, form, field.searchContext, field.semantic,
                            true});
                    }
                    m_cards->addWidget(card);
                    m_bindings.append(binding);
                }
            }
            for (int sectionIndex = 0; sectionIndex < sections.size(); ++sectionIndex)
            {
                const auto& section = sections.at(sectionIndex);
                auto& card = m_bindings[sectionIndex];
                for (int rowIndex = 0; rowIndex < section.fields.size(); ++rowIndex)
                {
                    const auto& field = section.fields.at(rowIndex);
                    auto& row = card.rows[rowIndex];
                    row.searchContext = field.searchContext;
                    if (row.semantic != field.semantic)
                    {
                        row.semantic = field.semantic;
                        // 只有真实字段语义发生改变才让这一项重匹配；普通换肤由投影根统一处理。
                        if (row.value)
                        {
                            row.value->setProperty("ksword_detail_field_semantic", field.semantic);
                            row.value->style()->unpolish(row.value);
                            row.value->style()->polish(row.value);
                            row.value->update();
                        }
                    }
                    if (row.value && row.value->text() != field.value)
                    {
                        row.value->setText(field.value);
                        row.value->setToolTip(field.value);
                    }
                }
            }
            if (structureChanged || searchChanged || (dataChanged && !query.isEmpty()))
            {
                filter();
            }
            m_documentRevision = revision;
            m_language = language;
            m_theme = theme;
            m_query = query;
            m_renderedTranslationRevision = translationRevision;
            m_snapshotReady = true;
            m_refreshing = false;
            if (m_source && m_source->documentRevision() != revision)
            {
                m_dirty = true; // 布局回调中若提交新快照，下一轮继续处理，不能漏掉真实变更。
            }
            if (m_dirty)
            {
                scheduleRefresh();
            }
        }

        // 输入是当前宿主链，返回实际画布颜色；独立详情不能穿过顶层窗口继承 owner 的参数表面。
        QColor projectionCanvasColor() const
        {
            QWidget* topLevel = window(); // Qt 的窗口父对象仍可指向另一窗口中的业务控件。
            for (QWidget* ancestor = parentWidget(); ancestor; ancestor = ancestor->parentWidget())
            {
                if (ancestor->property("ksword_parameter_surface").toBool())
                {
                    return ks::ui::MainWindowSurfaceColors().content;
                }
                if (ancestor == topLevel)
                {
                    break;
                }
            }
            return KswordTheme::WindowColor();
        }

        // 集中生成投影自己的精确主题块；状态色仍按卡片实底校准，不覆盖外部额外业务规则。
        void applyProjectionStyle()
        {
            const auto readableColor = [](const QColor& color)
            {
                return KswordTheme::EnsureTextContrast(color, KswordTheme::SurfaceColor(), 4.5).name();
            };
            const QString begin = QStringLiteral("/* KSWORD_DETAIL_PROJECTION_BEGIN */");
            const QString end = QStringLiteral("/* KSWORD_DETAIL_PROJECTION_END */");
            const QString block = begin + QStringLiteral(
                "QWidget[ksword_detail_projection=\"true\"],QWidget[ksword_detail_document_body=\"true\"]{background:%1;border:0;}"
                "QScrollArea,QScrollArea > QWidget{background:%1;border:0;}"
                "QLabel[ksword_detail_field_label=\"true\"]{background:transparent;color:%2;}"
                "QLabel[ksword_detail_field_value=\"true\"]{background:transparent;color:%3;}"
                "QLabel[ksword_detail_field_value=\"true\"][ksword_detail_field_semantic=\"success\"]{color:%4;}"
                "QLabel[ksword_detail_field_value=\"true\"][ksword_detail_field_semantic=\"error\"]{color:%5;}")
                .arg(projectionCanvasColor().name(), KswordTheme::TextSecondaryHex(),
                    readableColor(KswordTheme::TextPrimaryColor()), readableColor(KswordTheme::SuccessColor()),
                    readableColor(KswordTheme::ErrorColor())) + end;
            QString style = styleSheet(); // 保留其它拥有者定点添加的样式，只替换框架拥有的块。
            const qsizetype first = style.indexOf(begin);
            const qsizetype last = first < 0 ? -1 : style.indexOf(end, first);
            if (first >= 0 && last >= first)
            {
                style.remove(first, last + end.size() - first);
            }
            style = style.trimmed();
            if (!style.isEmpty())
            {
                style += QLatin1Char('\n');
            }
            style += block;
            if (styleSheet() != style)
            {
                setStyleSheet(style);
            }
        }
        void filter()
        {
            const QString query = m_search ? m_search->text().trimmed() : QString();
            for (auto& card : m_bindings)
            {
                bool any = false;
                const bool sectionMatches = !query.isEmpty() && card.title.contains(query, Qt::CaseInsensitive);
                for (auto& row : card.rows)
                {
                    const bool visible = query.isEmpty() || sectionMatches || (row.label && row.label->text().contains(query, Qt::CaseInsensitive))
                        || row.searchContext.contains(query, Qt::CaseInsensitive)
                        || (row.value && row.value->text().contains(query, Qt::CaseInsensitive));
                    if (row.visible != visible && row.form && row.value)
                    {
                        row.visible = visible;
                        row.form->setRowVisible(row.value, visible);
                    }
                    any |= visible;
                }
                if (card.visible != any && card.widget)
                {
                    card.visible = any;
                    card.widget->setVisible(any);
                }
            }
        }
        QPointer<ks::ui::StructuredFieldView> m_source; // 保留规范数据源及原导出入口。
        QPointer<QLineEdit> m_search; // 原搜索控件，不维护平行查询字符串。
        QWidget* m_body = nullptr;
        QVBoxLayout* m_cards = nullptr;
        QScrollArea* m_scroll = nullptr; // 嵌入已有正文滚动时为空。
        QStringList m_structure; // 仅记录结构身份，值更新不会重建卡片和滚动区。
        QVector<Card> m_bindings;
        quint64 m_documentRevision = 0; // 最后一帧使用的规范文档代次。
        quint64 m_translationRevision = 0; // LanguageChange只在统一调度器中递增。
        quint64 m_renderedTranslationRevision = 0; // 已展示的翻译代次，兜底主题回调不重复翻译。
        QString m_language; // 相同语言的重复广播不重建字段文本。
        QString m_theme; // 精确记录卡片使用的主题颜色，纯主题更新不展开文档。
        QString m_query; // 相同搜索和数据无需再次遍历可见性。
        bool m_snapshotReady = false; // 初次空文档也需要建立画布与过滤状态。
        bool m_dirty = true; // 隐藏页面保留待更新状态，Show首帧同步处理。
        bool m_refreshPending = false; // 合并同一事件轮内的文档/语言/主题通知。
        bool m_refreshing = false; // 创建字段与套样式期间禁止重入。
    };
}

QWidget* ks::ui::CreateDetailFieldProjection(StructuredFieldView* source, QWidget* parent, bool scrollBody)
{
    if (!source)
    {
        return nullptr;
    }
    if (source->property("ksword_detail_projection_source").toBool() && source->parentWidget()
        && source->parentWidget()->property("ksword_detail_projection").toBool())
    {
        return source->parentWidget(); // 重复接入复用同一宿主，保留原搜索和完整文档导出控件。
    }
    // canonical source仍作为业务Tab页时，投影是其直接子项，不通过换父对象查找宿主。
    for (QWidget* child : source->findChildren<QWidget*>(QString(), Qt::FindDirectChildrenOnly))
    {
        if (child->property("ksword_detail_projection").toBool()
            && child->property("ksword_detail_projection_embedded_source").toBool())
        {
            return child;
        }
    }
    return new DetailFieldProjection(source, parent ? parent : source->parentWidget(), scrollBody);
}
