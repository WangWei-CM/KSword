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
#include <QToolButton>
#include <QVBoxLayout>

namespace
{
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
        left->setContentsMargins(1, 1, 1, 1);
        left->setSpacing(0);
        m_scroll = new QScrollArea(m_parameterPanel);
        m_scroll->setWidgetResizable(true);
        m_scroll->setFrameShape(QFrame::NoFrame);
        auto* form = new QWidget(m_scroll);
        m_parameters = new QVBoxLayout(form);
        m_parameters->setContentsMargins(14, 10, 14, 14);
        m_parameters->setSpacing(18);
        m_parameters->setAlignment(Qt::AlignTop);
        m_scroll->setWidget(form);
        left->addWidget(m_scroll, 1);

        auto* footer = new QWidget(m_parameterPanel);
        bindWorkspaceSurface(footer, true);
        m_actions = new QHBoxLayout(footer);
        m_actions->setContentsMargins(12, 9, 12, 9);
        m_actions->setSpacing(8);
        left->addWidget(footer);
        m_results = new QVBoxLayout(m_resultPanel);
        m_results->setContentsMargins(10, 10, 10, 10);
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
                section->setSizePolicy(section->sizePolicy().horizontalPolicy(), QSizePolicy::Maximum);
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
            if (auto* combo = qobject_cast<QComboBox*>(field))
            {
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
