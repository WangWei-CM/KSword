#include "../Framework.h"
#include "./ParameterWorkspace.h"
#include "./PrimaryPageStyle.h"
#include "./ThemeBinding.h"
#include "./MainSurfaceStyle.h"
#include "./ToolbarMetrics.h"
#include "./CodeTextEdit.h"
#include "../theme.h"
#include <QAbstractItemView>
#include <QAbstractSpinBox>
#include <QComboBox>
#include <QApplication>
#include <QCheckBox>
#include <QRadioButton>
#include <QFormLayout>
#include <QLabel>
#include <QMouseEvent>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLineEdit>
#include <QPainter>
#include <QPainterPath>
#include <QPlainTextEdit>
#include <QPointer>
#include <QResizeEvent>
#include <QScrollArea>
#include <QSplitter>
#include <QStyleOptionButton>
#include <QTimer>
#include <QToolButton>
#include <QVBoxLayout>

namespace
{
    // 只作用于明确登记的参数复选框/单选框。保留原文与对象，仅重绘标签并按宽度测量高度。
    class ParameterChoiceWrap final : public QObject
    {
    public:
        explicit ParameterChoiceWrap(QAbstractButton* button) : QObject(button), m_button(button)
        {
            button->setMinimumWidth(0);
            button->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Fixed);
            button->installEventFilter(this);
            schedule();
        }
    protected:
        bool eventFilter(QObject*, QEvent* event) override
        {
            if (!m_button) return false;
            if (event->type() == QEvent::Paint)
            {
                QStyleOptionButton option = styleOption();
                QPainter painter(m_button);
                // 指示器继续由主题 QStyle 绘制，三态、禁用、按下、键盘状态不会变成假图标。
                m_button->style()->drawControl(isRadio() ? QStyle::CE_RadioButton : QStyle::CE_CheckBox,
                    &option, &painter, m_button);
                const QRect label = labelRect();
                const int flags = Qt::AlignVCenter | Qt::AlignLeading | Qt::TextWordWrap
                    | Qt::TextWrapAnywhere | (m_button->style()->styleHint(QStyle::SH_UnderlineShortcut, &option, m_button)
                        ? Qt::TextShowMnemonic : Qt::TextHideMnemonic);
                m_button->style()->drawItemText(&painter, label, flags, option.palette,
                    m_button->isEnabled(), m_button->text(), QPalette::WindowText);
                return true;
            }
            if (!m_forwarding && (event->type() == QEvent::MouseButtonPress
                || event->type() == QEvent::MouseButtonRelease || event->type() == QEvent::MouseMove
                || event->type() == QEvent::MouseButtonDblClick))
            {
                auto* mouse = static_cast<QMouseEvent*>(event);
                if (m_button->rect().contains(mouse->position().toPoint()))
                {
                    // 多行正文同样可点击。将行内命中交回原按钮指示器，原生按下/释放/拖出取消仍保留。
                    const QStyleOptionButton option = styleOption();
                    const QRect indicator = m_button->style()->subElementRect(isRadio()
                        ? QStyle::SE_RadioButtonIndicator : QStyle::SE_CheckBoxIndicator, &option, m_button);
                    const QPointF local = indicator.center();
                    QMouseEvent forwarded(mouse->type(), local, mouse->scenePosition(), mouse->globalPosition(),
                        mouse->button(), mouse->buttons(), mouse->modifiers(), mouse->pointingDevice());
                    forwarded.setTimestamp(mouse->timestamp());
                    m_forwarding = true;
                    QPointer<ParameterChoiceWrap> alive(this);
                    QApplication::sendEvent(m_button, &forwarded);
                    if (alive) m_forwarding = false;
                    event->setAccepted(forwarded.isAccepted());
                    return true;
                }
            }
            if (event->type() == QEvent::Resize || event->type() == QEvent::FontChange
                || event->type() == QEvent::StyleChange || event->type() == QEvent::LayoutRequest
                || event->type() == QEvent::Show)
                schedule();
            return false;
        }
    private:
        bool isRadio() const { return qobject_cast<QRadioButton*>(m_button.data()) != nullptr; }
        QStyleOptionButton styleOption() const
        {
            QStyleOptionButton option;
            option.initFrom(m_button);
            option.icon = m_button->icon();
            option.iconSize = m_button->iconSize();
            if (m_button->isDown()) option.state |= QStyle::State_Sunken;
            else option.state |= QStyle::State_Raised;
            const auto* check = qobject_cast<QCheckBox*>(m_button.data());
            if (check && check->checkState() == Qt::PartiallyChecked) option.state |= QStyle::State_NoChange;
            else option.state |= m_button->isChecked() ? QStyle::State_On : QStyle::State_Off;
            return option;
        }
        QRect labelRect() const
        {
            const QStyleOptionButton option = styleOption();
            const int indicator = m_button->style()->pixelMetric(isRadio()
                ? QStyle::PM_ExclusiveIndicatorWidth : QStyle::PM_IndicatorWidth, &option, m_button);
            return m_button->layoutDirection() == Qt::RightToLeft
                ? m_button->rect().adjusted(0, 3, -indicator - 7, -3)
                : m_button->rect().adjusted(indicator + 7, 3, 0, -3);
        }
        void schedule()
        {
            if (m_pending) return;
            m_pending = true;
            QTimer::singleShot(0, this, [this]()
            {
                m_pending = false;
                if (!m_button || m_button->width() <= 0) return;
                const int width = qMax(1, labelRect().width());
                const int height = m_button->fontMetrics().boundingRect(QRect(0, 0, width, 10000),
                    Qt::TextWordWrap | Qt::TextWrapAnywhere | Qt::TextHideMnemonic, m_button->text()).height() + 6;
                const int needed = qMax(28, height);
                if (m_button->minimumHeight() != needed) m_button->setMinimumHeight(needed);
            });
        }
        QPointer<QAbstractButton> m_button; // 原始业务控件，不复制 checked 或 text 状态。
        bool m_pending = false;
        bool m_forwarding = false;
    };

    // 参数框架的色块自身无描边，只在操作区与正文之间留一条低对比度分隔线。
    void bindWorkspaceSurface(QWidget* widget, bool footer = false)
    {
        widget->setAttribute(Qt::WA_StyledBackground);
        widget->setProperty("ksword_parameter_surface", true);
        const QPointer<QWidget> target(widget);
        ks::ui::BindWidgetTheme(widget, [target, footer]()
        {
            if (!target) return;
            const QColor surface = ks::ui::MainWindowSurfaceColors().content;
            const QColor divider = KswordTheme::BlendColors(surface, KswordTheme::BorderColor(), 70);
            target->setStyleSheet(QStringLiteral(
                "QWidget[ksword_parameter_surface=\"true\"]{background:%1;border:0;border-radius:7px;%2}")
                .arg(surface.name(), footer
                    ? QStringLiteral("border-top:1px solid %1;border-top-left-radius:0;border-top-right-radius:0;").arg(divider.name())
                    : QString()));
        });
    }

    // 绘制小号空心折角，绕开平台 QStyle 默认的亮色大三角；点击与键盘仍由原按钮处理。
    class ParameterFoldButton final : public QToolButton
    {
    public:
        explicit ParameterFoldButton(QWidget* parent) : QToolButton(parent)
        {
            setCheckable(true);
            setMouseTracking(true);
            setAttribute(Qt::WA_Hover);
            setFocusPolicy(Qt::StrongFocus);
            setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
            setMinimumHeight(30);
            connect(this, &QToolButton::toggled, this, [this]() { update(); });
        }
        QSize sizeHint() const override
        {
            return QSize(200, qMax(30, fontMetrics().height() + 10));
        }
        QSize minimumSizeHint() const override
        {
            // 标题已有省略与完整 tooltip，不让原生按钮的长文本宽度撑出横向滚动。
            return QSize(0, qMax(30, fontMetrics().height() + 10));
        }
    protected:
        void paintEvent(QPaintEvent*) override
        {
            const QColor panel = ks::ui::MainWindowSurfaceColors().content;
            QColor fill = KswordTheme::BlendColors(panel, KswordTheme::SurfaceAltColor(), 140);
            if (isDown()) fill = KswordTheme::ControlInputFocusColor(fill);
            else if (underMouse() || hasFocus()) fill = KswordTheme::ControlInputHoverColor(fill);
            const QColor label = KswordTheme::EnsureTextContrast(isEnabled()
                ? KswordTheme::TextSecondaryColor() : KswordTheme::TextDisabledColor(), fill, isEnabled() ? 4.5 : 3.0);
            const QColor chevron = KswordTheme::EnsureTextContrast(
                KswordTheme::BlendColors(fill, label, 150), fill, 3.0);
            QPainter painter(this);
            painter.setRenderHint(QPainter::Antialiasing);
            painter.setPen(Qt::NoPen);
            painter.setBrush(fill);
            painter.drawRoundedRect(rect(), 5, 5);

            // 折角约 4×7 逻辑像素，低于正文的视觉权重，并跟随展开方向旋转。
            const bool rtl = layoutDirection() == Qt::RightToLeft;
            painter.save();
            painter.translate(rtl ? width() - 14.0 : 14.0, height() / 2.0);
            painter.rotate(isChecked() ? 90.0 : rtl ? 180.0 : 0.0);
            QPainterPath arrow;
            arrow.moveTo(-1.5, -3.0);
            arrow.lineTo(1.5, 0.0);
            arrow.lineTo(-1.5, 3.0);
            painter.setBrush(Qt::NoBrush);
            painter.setPen(QPen(chevron, 1.3, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
            painter.drawPath(arrow);
            painter.restore();
            painter.setPen(label);
            painter.setFont(font());
            const QRect textArea = rect().adjusted(rtl ? 10 : 27, 0, rtl ? -27 : -10, 0);
            painter.drawText(textArea, Qt::AlignVCenter | (rtl ? Qt::AlignRight : Qt::AlignLeft),
                fontMetrics().elidedText(text(), Qt::ElideRight, textArea.width()));
        }
    };

    // 编辑输入是独立实底，结果日志与结果面板同底；同时同步 palette，避免行号栏仍沿用黑底。
    void styleWorkspaceEditors(QWidget* container)
    {
        for (auto* editor : container->findChildren<QPlainTextEdit*>())
        {
            QFont textFont = editor->font();
            if (textFont.pixelSize() > 0) textFont.setPixelSize(qMax(14, textFont.pixelSize()));
            else textFont.setPointSizeF(qMax(10.0, textFont.pointSizeF()));
            editor->setFont(textFont);
            editor->setFrameShape(QFrame::NoFrame);
            const QPointer<QPlainTextEdit> target(editor);
            ks::ui::BindWidgetTheme(editor, [target]()
            {
                if (!target) return;
                const QColor panel = ks::ui::MainWindowSurfaceColors().content;
                const QColor base = target->isReadOnly() ? panel : KswordTheme::ControlInputSurfaceColor();
                const QColor text = KswordTheme::EnsureTextContrast(KswordTheme::TextPrimaryColor(), base, 4.5);
                const QColor placeholder = KswordTheme::EnsureTextContrast(KswordTheme::TextSecondaryColor(), base, 4.5);
                const QColor accent = KswordTheme::ControlAccentColor();
                const QColor disabledBase = KswordTheme::BlendColors(panel, base, 90);
                const QColor disabledText = KswordTheme::EnsureTextContrast(KswordTheme::TextDisabledColor(), disabledBase, 3.0);
                target->setStyleSheet(QStringLiteral(
                    "QPlainTextEdit{background:%1;color:%2;border:0;border-radius:5px;"
                    "selection-background-color:%3;selection-color:%4;placeholder-text-color:%5;}"
                    "QPlainTextEdit:disabled{background:%6;color:%7;}")
                    .arg(base.name(), text.name(), accent.name(), KswordTheme::OnAccentColor(accent).name(),
                        placeholder.name(), disabledBase.name(), disabledText.name()));
                QPalette colors = target->palette();
                colors.setColor(QPalette::Base, base);
                colors.setColor(QPalette::Window, base);
                colors.setColor(QPalette::Text, text);
                colors.setColor(QPalette::PlaceholderText, placeholder);
                colors.setColor(QPalette::Disabled, QPalette::Base, disabledBase);
                colors.setColor(QPalette::Disabled, QPalette::Window, disabledBase);
                colors.setColor(QPalette::Disabled, QPalette::Text, disabledText);
                colors.setColor(QPalette::Disabled, QPalette::PlaceholderText, disabledText);
                target->setPalette(colors);
                target->viewport()->setPalette(colors);
                if (auto* code = dynamic_cast<CodeTextEdit*>(target.data())) code->refreshThemeColors();
            });
        }
    }
}

namespace ks::ui
{
    ParameterWorkspace::ParameterWorkspace(QWidget* parent, int parameterWidth)
        : QWidget(parent), m_parameterWidth(parameterWidth)
    {
        // 放大正文而保留用户字体家族；Qt 负责 DPI，不改变应用级字号与其他专业视图。
        QFont body = font();
        if (body.pixelSize() > 0) body.setPixelSize(qMax(14, body.pixelSize()));
        else body.setPointSizeF(qMax(10.0, body.pointSizeF()));
        setFont(body);
        auto* root = new QVBoxLayout(this);
        root->setContentsMargins(0, 0, 0, 0);
        m_splitter = new QSplitter(Qt::Horizontal, this);
        m_splitter->setChildrenCollapsible(false);
        m_splitter->setHandleWidth(10);
        root->addWidget(m_splitter);

        // 参数面板与结果面板处于同一视觉层级，中间留白代替重复的大组框。
        m_parameterPanel = new QWidget(m_splitter);
        m_resultPanel = new QWidget(m_splitter);
        bindWorkspaceSurface(m_parameterPanel);
        bindWorkspaceSurface(m_resultPanel);
        auto* left = new QVBoxLayout(m_parameterPanel);
        left->setContentsMargins(16, 16, 16, 16);
        left->setSpacing(0);
        m_scroll = new QScrollArea(m_parameterPanel);
        m_scroll->setWidgetResizable(true);
        m_scroll->setFrameShape(QFrame::NoFrame);
        m_scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
        auto* form = new QWidget(m_scroll);
        form->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
        form->setMinimumWidth(0);
        m_parameters = new QVBoxLayout(form);
        m_parameters->setContentsMargins(0, 0, 0, 0);
        m_parameters->setSpacing(18);
        m_parameters->setAlignment(Qt::AlignTop);
        m_scroll->setWidget(form);
        left->addWidget(m_scroll, 1);

        auto* footer = new QWidget(m_parameterPanel);
        bindWorkspaceSurface(footer, true);
        m_actions = new QHBoxLayout(footer);
        m_actions->setContentsMargins(0, 12, 0, 0);
        m_actions->setSpacing(8);
        left->addWidget(footer);
        m_results = new QVBoxLayout(m_resultPanel);
        m_results->setContentsMargins(16, 16, 16, 16);
        m_results->setSpacing(8);
        m_splitter->setStretchFactor(0, 0);
        m_splitter->setStretchFactor(1, 1);
        setMinimumSize(0, 0);
    }

    // 仅首次显示和宽窄布局转换重分配，不在每次 resize 中覆盖用户分栏选择。
    void ParameterWorkspace::resizeEvent(QResizeEvent* event)
    {
        QWidget::resizeEvent(event);
        const auto orientation = width() < 860 ? Qt::Vertical : Qt::Horizontal;
        if (!m_positioned || m_splitter->orientation() != orientation)
        {
            m_splitter->setOrientation(orientation);
            if (orientation == Qt::Horizontal)
                m_splitter->setSizes({m_parameterWidth, qMax(320, width() - m_parameterWidth)});
            else
                m_splitter->setSizes({qMax(220, height() / 2), qMax(180, height() / 2)});
            m_positioned = true;
        }
    }

    void ParameterWorkspace::finish()
    {
        StyleParameterFields(m_scroll->widget());
        styleWorkspaceEditors(m_scroll->widget());
        styleWorkspaceEditors(m_resultPanel);
        // 参数区按内容自然高度排列，禁止多行表单把剩余高度摊成巨大行间空白。
        for (auto* section : m_scroll->widget()->findChildren<QGroupBox*>())
        {
            if (section->property("ksword_parameter_section").toBool())
            {
                section->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Maximum);
                if (section->layout())
                    section->layout()->setContentsMargins(0, section->title().isEmpty() ? 4 : 8, 0, 4);
            }
        }
        NormalizeToolbarRow(m_actions);
        m_actions->parentWidget()->setVisible(m_actions->count() > 0);
    }

    void StyleParameterSection(QGroupBox* group)
    {
        if (!group) return;
        group->setProperty("ksword_parameter_section", true);
        StylePrimaryGroup(group);
        if (group->layout())
        {
            group->layout()->setContentsMargins(0, 22, 0, 4);
            group->layout()->setSpacing(8);
        }
    }

    void StyleParameterFields(QWidget* container)
    {
        if (!container) return;
        const auto fields = container->findChildren<QWidget*>();
        for (QWidget* field : fields)
        {
            // 不接管表格内部临时编辑器、连体数值框的内部文本框或业务单元格。
            bool tableEditor = false;
            for (QWidget* ancestor = field->parentWidget(); ancestor && ancestor != container;
                ancestor = ancestor->parentWidget())
                tableEditor = tableEditor || qobject_cast<QAbstractItemView*>(ancestor);
            if (tableEditor) continue;
            if (auto* label = qobject_cast<QLabel*>(field))
            {
                label->setWordWrap(true);
                label->setMinimumWidth(0);
                label->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Preferred);
            }
            else if (qobject_cast<QCheckBox*>(field) || qobject_cast<QRadioButton*>(field))
            {
                if (!field->property("ksword_parameter_wrap_choice").toBool())
                {
                    field->setProperty("ksword_parameter_wrap_choice", true);
                    new ParameterChoiceWrap(static_cast<QAbstractButton*>(field));
                }
            }
            if (auto* combo = qobject_cast<QComboBox*>(field))
            {
                combo->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
                combo->setMinimumContentsLength(8);
                combo->setMinimumWidth(0);
                combo->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
                StylePrimaryCombo(combo);
                StyleStandaloneInput(combo);
            }
            else if (auto* spin = qobject_cast<QAbstractSpinBox*>(field))
            {
                BindSpinBoxTheme(spin);
                NormalizeToolbarControl(spin);
                StyleStandaloneInput(spin);
            }
            else if (auto* edit = qobject_cast<QLineEdit*>(field))
            {
                if (qobject_cast<QComboBox*>(edit->parentWidget())
                    || qobject_cast<QAbstractSpinBox*>(edit->parentWidget())) continue;
                NormalizeToolbarControl(edit);
                const QPointer<QLineEdit> target(edit);
                BindWidgetTheme(edit, [target]()
                {
                    if (!target) return;
                    const QColor base = KswordTheme::ControlInputSurfaceColor();
                    const QColor backgrounds[] = {base, KswordTheme::ControlInputHoverColor(base), KswordTheme::ControlInputFocusColor(base)};
                    const QColor text = KswordTheme::EnsureTextContrastForBackgrounds(KswordTheme::TextPrimaryColor(), backgrounds, 3, 4.5);
                    // 输入面在控件自身声明，优先于祖先 QWidget 的透明规则。
                    target->setStyleSheet(QStringLiteral(
                        "QLineEdit{background:%1;color:%2;border:0;border-radius:5px;padding:0 8px;"
                        "selection-background-color:%3;selection-color:%4;placeholder-text-color:%5;}"
                        "QLineEdit:hover{background:%6;}QLineEdit:focus{background:%7;}"
                        "QLineEdit:disabled{background:%8;color:%9;}")
                        .arg(base.name(), text.name(),
                            KswordTheme::ControlAccentColor().name(),
                            KswordTheme::OnAccentColor(KswordTheme::ControlAccentColor()).name(),
                            KswordTheme::TextSecondaryColor().name(),
                            KswordTheme::ControlInputHoverColor(base).name(), KswordTheme::ControlInputFocusColor(base).name(),
                            KswordTheme::SurfaceMutedColor().name(), KswordTheme::TextDisabledColor().name()));
                });
            }
        }
        for (auto* form : container->findChildren<QFormLayout*>())
        {
            if (form->rowWrapPolicy() == QFormLayout::DontWrapRows)
                form->setRowWrapPolicy(QFormLayout::WrapLongRows);
            form->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);
            form->setLabelAlignment(Qt::AlignLeft | Qt::AlignVCenter);
        }
    }

    QWidget* FoldParameterSection(QGroupBox* group, bool expanded)
    {
        auto* wrapper = new QWidget(group->parentWidget());
        wrapper->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Maximum);
        auto* layout = new QVBoxLayout(wrapper);
        layout->setContentsMargins(0, 0, 0, 0);
        layout->setSpacing(6);
        auto* toggle = new ParameterFoldButton(wrapper);
        toggle->setText(group->title());
        toggle->setToolTip(group->title());
        toggle->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
        toggle->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
        toggle->setCheckable(true);
        toggle->setChecked(expanded);
        toggle->setMinimumHeight(30);
        group->setTitle(QString());
        StyleParameterSection(group);
        group->layout()->setContentsMargins(0, 4, 0, 4);
        layout->addWidget(toggle);
        layout->addWidget(group);
        group->setVisible(expanded);
        QObject::connect(toggle, &QToolButton::toggled, group, [group, toggle](bool open)
        {
            group->setVisible(open);
            toggle->update();
        });
        return wrapper;
    }
}
