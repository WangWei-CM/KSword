#include "./DetailPresentation.h"
#include "./StructuredFieldView.h"
#include "./PrimaryPageStyle.h"
#include "./ThemeBinding.h"
#include "./ToolbarMetrics.h"
#include "../Internationalization/LanguageManager.h"
#include "../theme.h"
#include <QApplication>
#include <QClipboard>
#include <QComboBox>
#include <QFormLayout>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QPointer>
#include <QScrollArea>
#include <QScrollBar>
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
            setProperty("ksword_detail_projection", true);
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
                connect(m_search, &QLineEdit::textChanged, this, [this]() { filter(); });
            }
            auto* presentation = source->findChild<QComboBox*>(QString(), Qt::FindDirectChildrenOnly);
            if (presentation)
            {
                tools->addWidget(presentation);
                ks::ui::StylePrimaryCombo(presentation);
                presentation->show();
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
                m_cardHost = m_scroll;
            }
            else
            {
                root->addWidget(m_body);
                m_cardHost = m_body;
            }
            source->setProperty("ksword_detail_projection_source", true);
            source->setParent(this);
            source->hide();
            if (presentation)
            {
                source->setMinimumSize(0, 0);
                source->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Ignored);
                root->addWidget(source, 1);
                connect(source, &ks::ui::StructuredFieldView::presentationChanged, this, [this, scrollBody]()
                {
                    const bool tree = m_source && m_source->presentation() == ks::ui::StructuredFieldView::Presentation::Tree;
                    m_cardHost->setVisible(!tree);
                    if (m_source)
                    {
                        m_source->setVisible(tree);
                    }
                    // 原树自己滚动；自然高度卡片则继续使用原外层正文滚动区。
                    setProperty("ksword_detail_projection_scroll", scrollBody || tree);
                    setSizePolicy(QSizePolicy::Ignored, scrollBody || tree ? QSizePolicy::Ignored : QSizePolicy::Preferred);
                });
                m_cardHost->setVisible(presentation->currentIndex() != 1);
                source->setVisible(presentation->currentIndex() == 1);
                const bool tree = presentation->currentIndex() == 1;
                setProperty("ksword_detail_projection_scroll", scrollBody || tree);
                setSizePolicy(QSizePolicy::Ignored, scrollBody || tree ? QSizePolicy::Ignored : QSizePolicy::Preferred);
            }
            connect(source, &ks::ui::StructuredFieldView::documentChanged, this, [this]() { refresh(); });
            const QPointer<DetailFieldProjection> safe(this);
            ks::ui::BindWidgetTheme(this, [safe]()
            {
                if (safe)
                {
                    safe->setStyleSheet(QStringLiteral(
                        "QWidget[ksword_detail_projection=\"true\"],QWidget[ksword_detail_document_body=\"true\"]{background:%1;border:0;}"
                        "QScrollArea{background:%1;border:0;}")
                        .arg(KswordTheme::WindowColorHex()));
                    safe->refresh();
                }
            });
            refresh();
        }
    protected:
        void changeEvent(QEvent* event) override
        {
            QWidget::changeEvent(event);
            if (event->type() == QEvent::LanguageChange)
            {
                // 等语言广播结束再更新卡片，避免广播遍历中同步销毁旧标题/字段控件。
                QTimer::singleShot(0, this, [this]() { refresh(); });
            }
        }
    private:
        struct Row
        {
            QPointer<QLabel> label; // 字段名；说明没有字段名标签。
            QPointer<QLabel> value; // 可选择/复制的值，数据刷新时原位更新。
            QPointer<QFormLayout> form; // 显示隐藏由原表单拥有的行处理。
            QString searchContext;
        };
        struct Card
        {
            QPointer<QWidget> widget; // 一个分区仅有一层圆角实底。
            QVector<Row> rows; // 与当前文档分区的字段顺序一一对应。
            QString title; // 搜索标题时保留原结构化视图的整组匹配语义。
        };
        void refresh()
        {
            if (!m_source)
            {
                return;
            }
            const auto sections = displaySections(m_source->document());
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
            if (structure != m_structure)
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
                            label->setWordWrap(true);
                            label->setMinimumWidth(0);
                            label->setStyleSheet(QStringLiteral("background:transparent;color:%1;")
                                .arg(KswordTheme::TextSecondaryHex()));
                            form->addRow(label, value);
                        }
                        binding.rows.append({label, value, form, field.searchContext});
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
                    if (row.value && row.value->text() != field.value)
                    {
                        row.value->setText(field.value);
                        row.value->setToolTip(field.value);
                    }
                    if (row.value)
                    {
                        const QColor color = field.semantic == QLatin1String("success") ? KswordTheme::SuccessColor()
                            : field.semantic == QLatin1String("error") ? KswordTheme::ErrorColor() : KswordTheme::TextPrimaryColor();
                        row.value->setStyleSheet(QStringLiteral("background:transparent;color:%1;")
                            .arg(KswordTheme::EnsureTextContrast(color, KswordTheme::SurfaceColor(), 4.5).name()));
                    }
                    if (row.label)
                    {
                        row.label->setStyleSheet(QStringLiteral("background:transparent;color:%1;")
                            .arg(KswordTheme::TextSecondaryHex()));
                    }
                }
            }
            filter();
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
                    if (row.form && row.value)
                    {
                        row.form->setRowVisible(row.value, visible);
                    }
                    any |= visible;
                }
                if (card.widget)
                {
                    card.widget->setVisible(any);
                }
            }
        }
        QPointer<ks::ui::StructuredFieldView> m_source; // 保留规范数据源及原导出入口。
        QPointer<QLineEdit> m_search; // 原搜索控件，不维护平行查询字符串。
        QWidget* m_body = nullptr;
        QVBoxLayout* m_cards = nullptr;
        QScrollArea* m_scroll = nullptr; // 嵌入已有正文滚动时为空。
        QWidget* m_cardHost = nullptr; // 卡片与原树视图互斥显示，不生成嵌套滚动。
        QStringList m_structure; // 仅记录结构身份，值更新不会重建卡片和滚动区。
        QVector<Card> m_bindings;
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
        return source->parentWidget(); // 重复接入复用同一宿主，保留原搜索、模式和导出控件。
    }
    return new DetailFieldProjection(source, parent ? parent : source->parentWidget(), scrollBody);
}
