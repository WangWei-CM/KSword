#include "../Framework.h"
#include "./ThemeCheckPage.h"
#include "../Internationalization/LanguageManager.h"
#include "../UI/MainSurfaceStyle.h"
#include "../UI/ThemeBinding.h"
#include "../UI/VisibleTableWidget.h"
#include "../UI/ParameterWorkspace.h"
#include "../UI/PrimaryPageStyle.h"
#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QDoubleSpinBox>
#include <QDateTime>
#include <QDir>
#include <QFormLayout>
#include <QGroupBox>
#include <QHeaderView>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPlainTextEdit>
#include <QProgressBar>
#include <QPushButton>
#include <QRadioButton>
#include <QScrollArea>
#include <QShowEvent>
#include <QSlider>
#include <QSpinBox>
#include <QTabWidget>
#include <QTableWidget>
#include <QTimer>
#include <QVBoxLayout>

namespace {
const ks::ui::PageNavigationTabDefinition kThemeCheckPageNavigation[] = {
    {"table", "", "", "表格", true},
    {"text", "", "", "文本", false}
};
}

namespace {
enum class ThemeCheckText {Description, Inputs, Placeholder, OptionOne, OptionTwo, OptionThree, SingleLine, Integer, Decimal, Combo, Check, Radio, Disabled, Values, Card, CardNote, Dynamic, Add, Refresh, Capture, HeaderName, HeaderState};
const char* const kThemeCheckTexts[] = {
    "本页使用普通 Qt 控件，未逐个设置样式。请检查深浅主题、透明背景、悬停和下拉弹窗。",
    "默认输入与选择",
    "输入示例文字",
    "选项一",
    "选项二",
    "选项三",
    "单行输入",
    "整数",
    "小数",
    "下拉选择",
    "复选状态",
    "单选状态",
    "禁用输入",
    "默认滑块与进度",
    "默认分组卡片",
    "分组标题位于卡片内部，参数分区使用标题与分隔线。",
    "动态新建控件",
    "新增控件",
    "重新检查",
    "保存检查截图",
    "名称",
    "状态"
};
// The cold body and the actual controls share target identities as well as text.
// The three options use their one real combo; both headers use their real table.
const char* const kThemeCheckTargets[] = {
    "ks_theme_check_description",
    "ks_theme_check_inputs",
    "ks_theme_check_input",
    "ks_theme_check_combo",
    "ks_theme_check_combo",
    "ks_theme_check_combo",
    "ks_theme_check_single_line_label",
    "ks_theme_check_integer_label",
    "ks_theme_check_decimal_label",
    "ks_theme_check_combo_label",
    "ks_theme_check_checkbox",
    "ks_theme_check_radio",
    "ks_theme_check_disabled_label",
    "ks_theme_check_values",
    "ks_theme_check_card",
    "ks_theme_check_card_note",
    "ks_theme_check_dynamic",
    "ks_theme_check_add",
    "ks_theme_check_refresh",
    "ks_theme_check_capture",
    "ks_theme_check_table",
    "ks_theme_check_table"
};
static_assert(sizeof(kThemeCheckTargets) / sizeof(kThemeCheckTargets[0])
    == sizeof(kThemeCheckTexts) / sizeof(kThemeCheckTexts[0]));
constexpr char kThemeCheckTextPageTarget[] = "ks_theme_check_text";
QString ThemeCheckTextValue(const ThemeCheckText key)
{
    return ks::i18n::sourceText(QString::fromUtf8(kThemeCheckTexts[static_cast<int>(key)]));
}
QString ThemeCheckTargetName(const ThemeCheckText key)
{
    return QString::fromLatin1(kThemeCheckTargets[static_cast<int>(key)]);
}
}

namespace ks::misc
{
    QVector<ks::ui::PageSearchBodyText> ThemeCheckPage::coldSearchBody()
    {
        QVector<ks::ui::PageSearchBodyText> body;
        body.reserve(24);
        const auto append = [&body](ThemeCheckText key, const QStringList& groups = {}, bool mnemonic = false)
        {
            body.append({ThemeCheckTextValue(key), ThemeCheckTargetName(key), groups, mnemonic});
        };
        // Match the real QObject traversal: description, parameter canvas,
        // action footer, then result card and tabs. Empty cold status/capture,
        // zero table rows and the plain editor document have no original hits.
        append(ThemeCheckText::Description);
        append(ThemeCheckText::Inputs);
        const QStringList inputs{ThemeCheckTextValue(ThemeCheckText::Inputs)};
        for (const auto key : {ThemeCheckText::Placeholder, ThemeCheckText::OptionOne,
            ThemeCheckText::OptionTwo, ThemeCheckText::OptionThree, ThemeCheckText::SingleLine,
            ThemeCheckText::Integer, ThemeCheckText::Decimal, ThemeCheckText::Combo})
            append(key, inputs);
        append(ThemeCheckText::Check, inputs, true);
        append(ThemeCheckText::Radio, inputs, true);
        append(ThemeCheckText::Disabled, inputs);
        append(ThemeCheckText::Values);
        append(ThemeCheckText::Dynamic);
        append(ThemeCheckText::Add, {ThemeCheckTextValue(ThemeCheckText::Dynamic)}, true);
        append(ThemeCheckText::Refresh, {}, true);
        append(ThemeCheckText::Capture, {}, true);
        append(ThemeCheckText::Card);
        append(ThemeCheckText::CardNote, {ThemeCheckTextValue(ThemeCheckText::Card)});
        const QString tableTitle = ks::ui::PageNavigationTabDescriptor(kThemeCheckPageNavigation[0]).title;
        const QString textTitle = ks::ui::PageNavigationTabDescriptor(kThemeCheckPageNavigation[1]).title;
        const QStringList tablePath{ks::ui::StripUiSearchMnemonicMarkers(tableTitle)};
        const QStringList textPath{ks::ui::StripUiSearchMnemonicMarkers(textTitle)};
        // A tab-title hit targets its real page, whose path already contains
        // that tab ancestor; no extra navigation page is introduced here.
        body.append({tableTitle, ThemeCheckTargetName(ThemeCheckText::HeaderName), tablePath, true});
        body.append({textTitle, QString::fromLatin1(kThemeCheckTextPageTarget), textPath, true});
        append(ThemeCheckText::HeaderName, tablePath);
        append(ThemeCheckText::HeaderState, tablePath);
        return body;
    }

    ThemeCheckPage::ThemeCheckPage(QWidget* parent) : QWidget(parent)
    {
        setObjectName(QStringLiteral("ks_theme_check_page"));
        ks::ui::StyleWorkspacePage(this);
        auto* root = new QVBoxLayout(this);
        root->setContentsMargins(6, 6, 6, 6);
        root->setSpacing(8);
        auto* description = new QLabel(ThemeCheckTextValue(ThemeCheckText::Description), this);
        description->setObjectName(ThemeCheckTargetName(ThemeCheckText::Description));
        description->setWordWrap(true);
        root->addWidget(description);

        // 页面选用真实参数/结果框架；样本控件仍不调用逐控件适配，验证默认主题本身。
        auto* workspace = new ks::ui::ParameterWorkspace(this, 480);
        workspace->useWideParameterLayout(60); // 宽屏优先双列参数，不让少量检查结果吞掉页面。
        root->addWidget(workspace, 1);
        auto* inputs = new QGroupBox(ThemeCheckTextValue(ThemeCheckText::Inputs), workspace);
        inputs->setObjectName(ThemeCheckTargetName(ThemeCheckText::Inputs));
        auto* form = new QFormLayout(inputs);
        form->setRowWrapPolicy(QFormLayout::WrapLongRows);
        auto* field = new QLineEdit(inputs);
        field->setObjectName(ThemeCheckTargetName(ThemeCheckText::Placeholder));
        field->setPlaceholderText(ThemeCheckTextValue(ThemeCheckText::Placeholder));
        auto* integer = new QSpinBox(inputs);
        integer->setRange(0, 10000);
        integer->setValue(42);
        auto* decimal = new QDoubleSpinBox(inputs);
        decimal->setRange(0, 100);
        decimal->setValue(1.25);
        auto* combo = new QComboBox(inputs);
        combo->setObjectName(ThemeCheckTargetName(ThemeCheckText::OptionOne));
        combo->addItems({ThemeCheckTextValue(ThemeCheckText::OptionOne), ThemeCheckTextValue(ThemeCheckText::OptionTwo), ThemeCheckTextValue(ThemeCheckText::OptionThree)});
        form->addRow(ThemeCheckTextValue(ThemeCheckText::SingleLine), field);
        form->addRow(ThemeCheckTextValue(ThemeCheckText::Integer), integer);
        form->addRow(ThemeCheckTextValue(ThemeCheckText::Decimal), decimal);
        form->addRow(ThemeCheckTextValue(ThemeCheckText::Combo), combo);
        form->labelForField(field)->setObjectName(ThemeCheckTargetName(ThemeCheckText::SingleLine));
        form->labelForField(integer)->setObjectName(ThemeCheckTargetName(ThemeCheckText::Integer));
        form->labelForField(decimal)->setObjectName(ThemeCheckTargetName(ThemeCheckText::Decimal));
        form->labelForField(combo)->setObjectName(ThemeCheckTargetName(ThemeCheckText::Combo));
        m_samples << field << integer << decimal << combo;
        auto* checked = new QCheckBox(ThemeCheckTextValue(ThemeCheckText::Check), inputs);
        checked->setObjectName(ThemeCheckTargetName(ThemeCheckText::Check));
        checked->setChecked(true);
        form->addRow(checked);
        auto* radio = new QRadioButton(ThemeCheckTextValue(ThemeCheckText::Radio), inputs);
        radio->setObjectName(ThemeCheckTargetName(ThemeCheckText::Radio));
        radio->setChecked(true);
        form->addRow(radio);
        auto* disabled = new QLineEdit(QStringLiteral("禁用状态仍应清晰可辨"), inputs);
        disabled->setEnabled(false);
        form->addRow(ThemeCheckTextValue(ThemeCheckText::Disabled), disabled);
        form->labelForField(disabled)->setObjectName(ThemeCheckTargetName(ThemeCheckText::Disabled));
        workspace->parameters()->addWidget(inputs);

        // 原生滑块和进度条不使用专用子类，直接验证全局默认皮肤。
        auto* values = new QGroupBox(ThemeCheckTextValue(ThemeCheckText::Values), workspace);
        values->setObjectName(ThemeCheckTargetName(ThemeCheckText::Values));
        auto* valueLayout = new QVBoxLayout(values);
        auto* slider = new QSlider(Qt::Horizontal, values);
        slider->setObjectName(QStringLiteral("ks_theme_check_slider"));
        slider->setRange(0, 100);
        slider->setValue(60);
        auto* progress = new QProgressBar(values);
        progress->setRange(0, 100);
        progress->setValue(slider->value());
        connect(slider, &QSlider::valueChanged, progress, &QProgressBar::setValue);
        valueLayout->addWidget(slider);
        valueLayout->addWidget(progress);
        workspace->parameters()->addWidget(values);

        // 这里的普通文本控件是刻意保留的皮肤样本，不承担业务日志/代码编辑器职责。
        // 原生Tab和表格只声明数据行为，不手动安装Tab动画、圆角或配色。
        auto* tabs = new QTabWidget(workspace);
        tabs->setObjectName(QStringLiteral("ks_theme_check_tabs"));
        auto* table = new QTableWidget(0, 2, tabs);
        m_table = table;
        table->setObjectName(ThemeCheckTargetName(ThemeCheckText::HeaderName));
        table->setHorizontalHeaderLabels({ThemeCheckTextValue(ThemeCheckText::HeaderName), ThemeCheckTextValue(ThemeCheckText::HeaderState)});
        table->setEditTriggers(QAbstractItemView::NoEditTriggers); // 接入数据由真实控件状态产生。
        table->setAlternatingRowColors(true);
        table->horizontalHeader()->setSectionResizeMode(QHeaderView::Stretch);
        ks::ui::SetTableActionBarMode(table, ks::ui::TableActionBarMode::None);
        auto* text = new QPlainTextEdit(tabs);
        text->setObjectName(QString::fromLatin1(kThemeCheckTextPageTarget));
        text->setPlainText(QStringLiteral("普通多行输入：背景、文字和选区应随主题一致变化。"));
        m_navigationTabs.add(kThemeCheckPageNavigation[0], tabs, table);
        m_navigationTabs.add(kThemeCheckPageNavigation[1], tabs, text);
        tabs->setMinimumHeight(180);
        // 结果侧独立分组验证标题在卡片内部；参数侧同类QGroupBox自动成为分区标题。
        auto* card = new QGroupBox(ThemeCheckTextValue(ThemeCheckText::Card), workspace);
        card->setObjectName(ThemeCheckTargetName(ThemeCheckText::Card));
        auto* cardLayout = new QVBoxLayout(card);
        auto* cardNote = new QLabel(ThemeCheckTextValue(ThemeCheckText::CardNote), card);
        cardNote->setObjectName(ThemeCheckTargetName(ThemeCheckText::CardNote));
        cardNote->setWordWrap(true);
        cardLayout->addWidget(cardNote);
        workspace->results()->addWidget(card);
        workspace->results()->addWidget(tabs, 1);

        auto* dynamic = new QGroupBox(ThemeCheckTextValue(ThemeCheckText::Dynamic), workspace);
        dynamic->setObjectName(ThemeCheckTargetName(ThemeCheckText::Dynamic));
        m_dynamicLayout = new QVBoxLayout(dynamic);
        auto* add = new QPushButton(ThemeCheckTextValue(ThemeCheckText::Add), dynamic);
        add->setObjectName(ThemeCheckTargetName(ThemeCheckText::Add));
        connect(add, &QPushButton::clicked, this, &ThemeCheckPage::addDynamicControls);
        m_dynamicLayout->addWidget(add);
        m_samples << add;
        workspace->parameters()->addWidget(dynamic);
        ks::ui::ArrangeParameterColumns(workspace->parameters(), {inputs}, {values, dynamic});
        auto* check = new QPushButton(ThemeCheckTextValue(ThemeCheckText::Refresh), workspace);
        check->setObjectName(ThemeCheckTargetName(ThemeCheckText::Refresh));
        connect(check, &QPushButton::clicked, this, &ThemeCheckPage::refreshChecks);
        auto* actions = workspace->actions();
        actions->addWidget(check);
        m_samples << check;
        auto* capture = new QPushButton(ThemeCheckTextValue(ThemeCheckText::Capture), workspace);
        capture->setObjectName(ThemeCheckTargetName(ThemeCheckText::Capture));
        auto* captureStatus = new QLabel(this);
        captureStatus->setObjectName(QStringLiteral("ks_theme_check_capture_status"));
        captureStatus->setWordWrap(true);
        captureStatus->setTextInteractionFlags(Qt::TextSelectableByMouse);
        connect(capture, &QPushButton::clicked, this, [this, captureStatus]()
        {
            // 只抓取当前真实页面，不读取其它窗口或桌面；透明像素保留在PNG中。
            const QString file = QDir::tempPath() + QStringLiteral("/ksword-theme-check-")
                + QString::number(QCoreApplication::applicationPid()) + QLatin1Char('-')
                + QString::number(QDateTime::currentMSecsSinceEpoch()) + QStringLiteral(".png");
            captureStatus->setText(grab().save(file) ? file
                : ks::i18n::sourceText(QStringLiteral("检查截图保存失败")));
        });
        actions->addWidget(capture);
        actions->addStretch(1);
        workspace->results()->addWidget(captureStatus);
        m_samples << capture;
        m_status = new QLabel(this);
        m_status->setObjectName(QStringLiteral("ks_theme_check_status"));
        m_status->setWordWrap(true);
        m_status->setTextInteractionFlags(Qt::TextSelectableByMouse);
        workspace->results()->insertWidget(0, m_status);
    }

    void ThemeCheckPage::showEvent(QShowEvent* event)
    {
        QWidget::showEvent(event);
        QTimer::singleShot(0, this, &ThemeCheckPage::refreshChecks);
    }

    void ThemeCheckPage::addDynamicControls()
    {
        if (m_dynamicCount >= 3) return; // 有界示例，不触发系统或驱动操作。
        ++m_dynamicCount;
        auto* field = new QLineEdit(m_dynamicLayout->parentWidget());
        field->setObjectName(QStringLiteral("ks_theme_check_dynamic_") + QString::number(m_dynamicCount));
        field->setPlaceholderText(ks::i18n::sourceText(QStringLiteral("动态输入 %1")).arg(m_dynamicCount));
        m_dynamicLayout->addWidget(field);
        m_samples << field;
        QTimer::singleShot(0, this, &ThemeCheckPage::refreshChecks);
    }

    void ThemeCheckPage::refreshChecks()
    {
        int bound = 0; // 只统计确实被默认机制接入的真实控件。
        int sized = 0;
        int live = 0;
        m_table->setRowCount(m_samples.size()); // 动态新建样本自然追加到同一份检查结果。
        for (const QPointer<QWidget>& sample : m_samples)
        {
            if (!sample) continue;
            ++live;
            const bool connected = ks::ui::HasWidgetThemeBinding(sample)
                && sample->property("ksword_standard_control").toBool();
            if (connected) ++bound;
            if (sample->height() >= 28) ++sized;
            const QString type = QString::fromLatin1(sample->metaObject()->className());
            const QString state = ks::i18n::sourceText(QStringLiteral("%1 · 高度 %2px"))
                .arg(ks::i18n::sourceText(connected ? QStringLiteral("已接入") : QStringLiteral("未接入")))
                .arg(sample->height());
            for (int column = 0; column < 2; ++column)
            {
                auto* item = m_table->item(live - 1, column);
                if (!item)
                {
                    item = new QTableWidgetItem();
                    m_table->setItem(live - 1, column, item);
                }
                item->setText(column == 0 ? type : state);
            }
        }
        const bool installed = qApp->property("ksword_standard_widget_theme_installed").toBool();
        const QString mode = ks::i18n::sourceText(ks::ui::UsesTransparentMainSurface(this)
            ? QStringLiteral("透明背景") : QStringLiteral("不透明背景"));
        m_status->setText(ks::i18n::sourceText(QStringLiteral("标准主题：%1；自动接入：%2/%3；尺寸：%4/%3；背景：%5。视觉效果请检查悬停、弹窗和选区。"))
            .arg(ks::i18n::sourceText(installed ? QStringLiteral("已安装") : QStringLiteral("未安装")))
            .arg(bound).arg(live).arg(sized).arg(mode));
    }
}

ks::ui::PageNavigationCatalog ks::misc::ThemeCheckPage::navigationCatalog(const ThemeCheckPage* existing)
{
    ks::ui::PageNavigationCatalog schema;
    for (const auto& definition : kThemeCheckPageNavigation)
        schema.append(ks::ui::PageNavigationTabDescriptor(definition));
    schema[0].searchTexts = {ThemeCheckTextValue(ThemeCheckText::HeaderName), ThemeCheckTextValue(ThemeCheckText::HeaderState)};
    return existing ? existing->m_navigationTabs.describe(std::move(schema)) : schema;
}
QWidget* ks::misc::ThemeCheckPage::activateNavigationDescriptor(const QString& id)
{
    return m_navigationTabs.activate(id, navigationCatalog(this));
}
namespace { const bool kThemeCheckPageNavigationRegistered =
    ks::ui::RegisterTypedPageNavigationCatalog<ks::misc::ThemeCheckPage>(QStringLiteral("misc.theme_check")); }

QStringList ks::misc::ThemeCheckPage::navigationRootSearchTexts()
{
    QStringList texts;
    // 表头归table子页；其余cold构造实际索引的标签/按钮/分组/下拉/placeholder归共同页根。
    for (int index = 0; index < static_cast<int>(ThemeCheckText::HeaderName); ++index)
        texts.append(ThemeCheckTextValue(static_cast<ThemeCheckText>(index)));
    return texts;
}
