#include "./DetailPresentation.h"
#include "./ParameterWorkspace.h"
#include "ThemeBinding.h"
#include "../theme.h"

#include <QEvent>
#include <QFontMetrics>
#include <QFormLayout>
#include <QLabel>
#include <QPointer>
#include <QScrollArea>
#include <QTimer>
#include <QVBoxLayout>

namespace
{
    // 只观察明确登记的侧栏及其顶层窗口，字体/窗口变化后合并更新几何。
    class DetailSidebarSizing final : public QObject
    {
    public:
        DetailSidebarSizing(QScrollArea* sidebar, int maximumWidth)
            : QObject(sidebar), m_sidebar(sidebar), m_window(sidebar->window()), m_maximum(maximumWidth)
        {
            sidebar->installEventFilter(this);
            m_window->installEventFilter(this);
            apply();
        }
    protected:
        bool eventFilter(QObject* source, QEvent* event) override
        {
            if (event->type() == QEvent::Resize || event->type() == QEvent::Show
                || event->type() == QEvent::FontChange || event->type() == QEvent::ApplicationFontChange)
            {
                if (!m_pending)
                {
                    m_pending = true;
                    QTimer::singleShot(0, this, [this]()
                    {
                        m_pending = false;
                        apply();
                    });
                }
            }
            return QObject::eventFilter(source, event);
        }
    private:
        void apply()
        {
            if (!m_sidebar || !m_window)
            {
                return;
            }
            const int minimum = qMin(m_maximum, qMax(140, QFontMetrics(m_sidebar->font()).height() * 10));
            const int width = qBound(minimum, m_window->width() / 5, m_maximum);
            if (m_sidebar->minimumWidth() != width || m_sidebar->maximumWidth() != width)
            {
                m_sidebar->setFixedWidth(width);
            }
        }
        QPointer<QScrollArea> m_sidebar; // 保留原导航滚动区。
        QPointer<QWidget> m_window; // 详情顶层窗口提供可用宽度。
        int m_maximum = 240; // 宽屏时的侧栏宽度上限。
        bool m_pending = false; // 避免Resize反馈重复安排布局。
    };
}

namespace ks::ui
{
    QWidget* CreateDetailCard(const QString& title, QWidget* parent)
    {
        auto* card = new QWidget(parent);
        card->setProperty("ksword_detail_card", true);
        card->setAttribute(Qt::WA_StyledBackground);
        card->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Maximum);
        auto* layout = new QVBoxLayout(card);
        layout->setContentsMargins(16, 14, 16, 14);
        layout->setSpacing(10);
        auto* heading = new QLabel(title, card);
        heading->setProperty("ksword_detail_card_heading", true);
        heading->setWordWrap(true);
        heading->setTextFormat(Qt::PlainText);
        layout->addWidget(heading);
        const QPointer<QWidget> safeCard(card); // 主题变化只重绘现有卡片，不重建字段。
        BindWidgetTheme(card, [safeCard]()
        {
            if (!safeCard)
            {
                return;
            }
            safeCard->setStyleSheet(QStringLiteral(
                "QWidget[ksword_detail_card=\"true\"]{background:%1;border:0;border-radius:8px;}"
                "QLabel[ksword_detail_card_heading=\"true\"]{background:transparent;color:%2;font-weight:600;}")
                .arg(KswordTheme::SurfaceHex(), KswordTheme::TextPrimaryHex()));
        });
        return card;
    }

    void ConfigureDetailForm(QFormLayout* form)
    {
        if (!form)
        {
            return;
        }
        form->setContentsMargins(0, 0, 0, 0);
        form->setLabelAlignment(Qt::AlignLeft | Qt::AlignVCenter);
        form->setFormAlignment(Qt::AlignTop);
        form->setHorizontalSpacing(16);
        form->setVerticalSpacing(6);
        form->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);
        form->setRowWrapPolicy(QFormLayout::WrapLongRows);
    }

    void AddDetailFieldColumns(QWidget* card, QFormLayout*& first, QFormLayout*& second)
    {
        auto* layout = qobject_cast<QVBoxLayout*>(card ? card->layout() : nullptr);
        if (!layout)
        {
            first = nullptr;
            second = nullptr;
            return;
        }
        auto* firstPane = new QWidget(card);
        auto* secondPane = new QWidget(card);
        first = new QFormLayout(firstPane);
        second = new QFormLayout(secondPane);
        ConfigureDetailForm(first);
        ConfigureDetailForm(second);
        layout->addWidget(firstPane);
        layout->addWidget(secondPane);
        // 复用已有的响应式分栏布局，只重排这两个明确字段面板，不建立第二层滚动区。
        ArrangeParameterColumns(layout, {firstPane}, {secondPane}, 300);
    }

    void InstallDetailSidebarSizing(QScrollArea* sidebar, int maximumWidth)
    {
        if (!sidebar || sidebar->property("ksword_detail_sidebar_sizing").toBool())
        {
            return;
        }
        sidebar->setProperty("ksword_detail_sidebar_sizing", true);
        new DetailSidebarSizing(sidebar, qMax(140, maximumWidth));
    }
}
