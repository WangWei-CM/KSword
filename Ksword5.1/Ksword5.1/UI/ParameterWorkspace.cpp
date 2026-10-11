#include "../Framework.h"
#include "./ParameterWorkspace.h"
#include "./PrimaryPageStyle.h"
#include "./ThemeBinding.h"
#include "./MainSurfaceStyle.h"
#include "./ToolbarMetrics.h"
#include "../theme.h"
#include <QAbstractItemView>
#include <QAbstractSpinBox>
#include <QComboBox>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLineEdit>
#include <QPointer>
#include <QResizeEvent>
#include <QScrollArea>
#include <QSplitter>
#include <QToolButton>
#include <QVBoxLayout>

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
        StylePrimaryContentPanel(m_parameterPanel);
        StylePrimaryContentPanel(m_resultPanel);
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
        StylePrimaryToolbar(footer);
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
                    const QColor text = KswordTheme::EnsureTextContrast(KswordTheme::TextPrimaryColor(), base, 4.5);
                    // 输入面在控件自身声明，优先于祖先 QWidget 的透明规则。
                    target->setStyleSheet(QStringLiteral(
                        "QLineEdit{background:%1;color:%2;border:1px solid %3;border-radius:5px;padding:0 8px;"
                        "selection-background-color:%4;selection-color:%5;placeholder-text-color:%6;}"
                        "QLineEdit:hover{background:%7;}QLineEdit:focus{background:%8;border-color:%4;}"
                        "QLineEdit:disabled{background:%9;color:%10;}")
                        .arg(base.name(), text.name(), KswordTheme::BorderColor().name(),
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
        auto* layout = new QVBoxLayout(wrapper);
        layout->setContentsMargins(0, 0, 0, 0);
        layout->setSpacing(6);
        auto* toggle = new QToolButton(wrapper);
        toggle->setText(group->title());
        toggle->setToolTip(group->title());
        toggle->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
        toggle->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
        toggle->setCheckable(true);
        toggle->setChecked(expanded);
        toggle->setArrowType(expanded ? Qt::DownArrow : Qt::RightArrow);
        toggle->setMinimumHeight(30);
        const QPointer<QToolButton> header(toggle);
        BindWidgetTheme(toggle, [header]()
        {
            if (!header) return;
            header->setStyleSheet(QStringLiteral(
                "QToolButton{background:%1;color:%2;border:0;border-radius:5px;text-align:left;padding:4px 8px;}"
                "QToolButton:hover{background:%3;}")
                .arg(KswordTheme::SurfaceAltColor().name(), KswordTheme::TextPrimaryColor().name(),
                    KswordTheme::SurfaceMutedColor().name()));
        });
        group->setTitle(QString());
        StyleParameterSection(group);
        group->layout()->setContentsMargins(0, 4, 0, 4);
        layout->addWidget(toggle);
        layout->addWidget(group);
        group->setVisible(expanded);
        QObject::connect(toggle, &QToolButton::toggled, group, [group, toggle](bool open)
        {
            group->setVisible(open);
            toggle->setArrowType(open ? Qt::DownArrow : Qt::RightArrow);
        });
        return wrapper;
    }
}
