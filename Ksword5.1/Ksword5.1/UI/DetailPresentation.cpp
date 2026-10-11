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
#include <QHBoxLayout>
#include <QPainter>

namespace
{
    // 卡片实底由自身绘制，避免祖先滚动区QSS/透明兼容规则在圆角内外叠出色块。
    class DetailCard final : public QWidget
    {
    public:
        explicit DetailCard(QWidget* parent) : QWidget(parent)
        {
            setAutoFillBackground(false);
        }
    protected:
        void paintEvent(QPaintEvent*) override
        {
            QPainter painter(this);
            painter.setRenderHint(QPainter::Antialiasing);
            painter.setPen(Qt::NoPen);
            painter.setBrush(KswordTheme::SurfaceColor());
            painter.drawRoundedRect(QRectF(rect()), 8, 8);
        }
    };

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
    QWidget* CreateDetailSummaryHeader(QWidget* parent, const QString& title,
        const QString& subtitle, const QIcon& icon)
    {
        auto* header = new QWidget(parent);
        header->setProperty("ksword_detail_header", true);
        header->setAttribute(Qt::WA_StyledBackground);
        header->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Maximum);
        auto* row = new QHBoxLayout(header);
        row->setContentsMargins(20, 16, 20, 16);
        row->setSpacing(14);
        if (!icon.isNull())
        {
            auto* image = new QLabel(header);
            image->setFixedSize(48, 48);
            image->setPixmap(icon.pixmap(48, 48));
            row->addWidget(image, 0, Qt::AlignTop);
        }
        auto* identity = new QWidget(header);
        identity->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
        auto* text = new QVBoxLayout(identity);
        text->setContentsMargins(0, 0, 0, 0);
        text->setSpacing(4);
        auto* name = new QLabel(title, identity);
        name->setObjectName(QLatin1String("ksword_detail_summary_title"));
        name->setWordWrap(true);
        name->setTextFormat(Qt::PlainText);
        name->setTextInteractionFlags(Qt::TextSelectableByMouse);
        name->setProperty("ks_i18n_preserve_data_text", true);
        QFont heading = name->font();
        heading.setBold(true);
        if (heading.pixelSize() > 0)
        {
            heading.setPixelSize(qMax(18, heading.pixelSize() + 3));
        }
        else
        {
            heading.setPointSizeF(qMax(12.0, heading.pointSizeF() + 2.0));
        }
        name->setFont(heading);
        text->addWidget(name);
        auto* description = new QLabel(subtitle, identity);
        description->setTextFormat(Qt::PlainText);
        description->setTextInteractionFlags(Qt::TextSelectableByMouse);
        description->setWordWrap(true);
        description->setMinimumWidth(0);
        description->setProperty("ks_i18n_preserve_data_text", true);
        description->setVisible(!subtitle.isEmpty());
        text->addWidget(description);
        row->addWidget(identity, 1);
        const QPointer<QWidget> safe(header);
        const QPointer<QLabel> safeDescription(description);
        BindWidgetTheme(header, [safe, safeDescription]()
        {
            if (!safe)
            {
                return;
            }
            const QString style = QStringLiteral(
                "QWidget[ksword_detail_header=\"true\"]{background:%1;color:%2;border:0;border-bottom:1px solid %3;}"
                "QLabel{background:transparent;color:%2;border:0;}")
                .arg(KswordTheme::WindowColorHex(), KswordTheme::TextPrimaryHex(),
                    KswordTheme::BlendColors(KswordTheme::WindowColor(), KswordTheme::BorderColor(), 70).name());
            if (safe->styleSheet() != style)
            {
                safe->setStyleSheet(style);
            }
            if (safeDescription)
            {
                const QString descriptionStyle = QStringLiteral("color:%1;background:transparent;")
                    .arg(KswordTheme::TextSecondaryHex());
                if (safeDescription->styleSheet() != descriptionStyle)
                {
                    safeDescription->setStyleSheet(descriptionStyle);
                }
            }
        });
        return header;
    }

    QWidget* CreateDetailCard(const QString& title, QWidget* parent)
    {
        auto* card = new DetailCard(parent);
        card->setProperty("ksword_detail_card", true);
        card->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Maximum);
        auto* layout = new QVBoxLayout(card);
        layout->setContentsMargins(16, 14, 16, 14);
        layout->setSpacing(10);
        auto* heading = new QLabel(title, card);
        QFont headingFont = heading->font(); // 初始字体已与最终主题一致，不在首显后改变标题几何。
        headingFont.setWeight(QFont::DemiBold);
        heading->setFont(headingFont);
        heading->setProperty("ksword_detail_card_heading", true);
        heading->setWordWrap(true);
        heading->setTextFormat(Qt::PlainText);
        heading->setVisible(!title.isEmpty());
        layout->addWidget(heading);
        const QPointer<QWidget> safeCard(card); // 主题变化只重绘现有卡片，不重建字段。
        BindWidgetTheme(card, [safeCard]()
        {
            if (!safeCard)
            {
                return;
            }
            const QString style = QStringLiteral(
                "QWidget[ksword_detail_card=\"true\"]{background:transparent;border:0;}"
                "QLabel{background:transparent;border:0;}"
                "QLabel[ksword_detail_card_heading=\"true\"]{background:transparent;color:%1;font-weight:600;}")
                .arg(KswordTheme::TextPrimaryHex());
            if (safeCard->styleSheet() != style)
            {
                safeCard->setStyleSheet(style);
            }
            safeCard->update();
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
        // 布局面板只承载字段，圆角实底归唯一外卡片；不再生成独立矩形背景。
        for (auto* pane : {firstPane, secondPane})
        {
            pane->setAutoFillBackground(false);
            pane->setProperty("ksword_detail_field_pane", true);
            pane->setStyleSheet(QStringLiteral("QWidget[ksword_detail_field_pane=\"true\"]{background:transparent;border:0;}"));
        }
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
