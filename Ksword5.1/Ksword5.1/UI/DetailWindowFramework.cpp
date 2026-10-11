#include "./DetailPresentation.h"
#include "./DetailDialogChrome.h"
#include "./StructuredFieldView.h"
#include "./PrimaryPageStyle.h"
#include "./ToolbarMetrics.h"
#include "./ThemeBinding.h"
#include "../theme.h"
#include <QAbstractScrollArea>
#include <QBoxLayout>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QGroupBox>
#include <QLabel>
#include <QLineEdit>
#include <QPointer>
#include <QSet>
#include <QSplitter>
#include <QTimer>
#include <algorithm>

namespace
{
    // 使用占位保留原布局位置；构造共享宿主会重新挂父对象，不能依赖旧layout项继续存在。
    template<class Factory>
    bool replaceDetailWidget(QWidget* original, Factory create)
    {
        QWidget* parent = original ? original->parentWidget() : nullptr;
        if (auto* splitter = qobject_cast<QSplitter*>(parent))
        {
            const int index = splitter->indexOf(original);
            if (index < 0)
            {
                return false;
            }
            const QList<int> sizes = splitter->sizes();
            auto* placeholder = new QWidget();
            splitter->replaceWidget(index, placeholder);
            original->setParent(nullptr);
            // 暂不挂到Splitter，避免构造过程被其ChildAdded自动插到末尾。
            QWidget* replacement = create(nullptr);
            splitter->replaceWidget(index, replacement);
            delete placeholder;
            if (std::any_of(sizes.cbegin(), sizes.cend(), [](int size) { return size > 0; }))
            {
                splitter->setSizes(sizes); // 隐藏页首次布局前的全零尺寸不能当成用户的折叠比例。
            }
            return true;
        }
        QLayout* layout = parent ? parent->layout() : nullptr;
        if (!layout)
        {
            return false;
        }
        auto* placeholder = new QWidget(parent);
        QLayoutItem* replaced = layout->replaceWidget(original, placeholder);
        if (!replaced)
        {
            delete placeholder;
            return false;
        }
        delete replaced;
        QWidget* replacement = create(parent);
        delete layout->replaceWidget(placeholder, replacement);
        delete placeholder;
        return true;
    }

    bool insideDetailScroll(QWidget* widget, QWidget* root)
    {
        for (QWidget* parent = widget->parentWidget(); parent && parent != root; parent = parent->parentWidget())
        {
            if (qobject_cast<QScrollArea*>(parent))
            {
                return true;
            }
        }
        return false;
    }

    class DetailWindowFramework final : public QObject
    {
    public:
        explicit DetailWindowFramework(QWidget* window) : QObject(window), m_window(window)
        {
            window->installEventFilter(this);
            schedule();
        }
        void schedule()
        {
            if (m_pending || m_applying)
            {
                return;
            }
            m_pending = true;
            QTimer::singleShot(0, this, [this]()
            {
                m_pending = false;
                apply();
            });
        }
    protected:
        bool eventFilter(QObject* object, QEvent* event) override
        {
            // 惰性页和动态字段在完成当前构造后加入共享布局，不在ChildAdded半构造状态访问。
            if (!m_applying && (event->type() == QEvent::ChildAdded || event->type() == QEvent::Show
                || event->type() == QEvent::FontChange || event->type() == QEvent::ApplicationFontChange
                || (object == m_window && (event->type() == QEvent::StyleChange || event->type() == QEvent::PaletteChange))))
            {
                schedule();
            }
            if (!m_applying && event->type() == QEvent::DynamicPropertyChange
                && static_cast<QDynamicPropertyChangeEvent*>(event)->propertyName() == "ksword_detail_projection_scroll")
            {
                schedule();
            }
            return QObject::eventFilter(object, event);
        }
    private:
        void adoptRoot()
        {
            QLayout* original = m_window->layout();
            if (!original || m_window->property("ksword_detail_framework_root").toBool())
            {
                return;
            }
            m_window->setProperty("ksword_detail_framework_root", true);
            const auto headers = m_window->findChildren<QWidget*>(QString(), Qt::FindDirectChildrenOnly);
            for (auto* widget : headers)
            {
                if (widget->property("ksword_detail_header").toBool())
                {
                    return; // 原窗口已提供真实对象身份，保留其原业务摘要和动作。
                }
            }
            // 现有布局整体转交正文宿主；复用原布局和控件，不重写业务页面。
            auto* body = new QWidget(m_window);
            body->setProperty("ksword_detail_framework_body", true);
            body->setMinimumSize(0, 0);
            body->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Ignored);
            body->setLayout(original);
            original->setContentsMargins(20, 16, 20, 16);
            if (auto* box = qobject_cast<QBoxLayout*>(original))
            {
                box->setSpacing(12);
            }
            auto* root = new QVBoxLayout(m_window);
            root->setContentsMargins(0, 0, 0, 0);
            root->setSpacing(0);
            auto* summary = ks::ui::CreateDetailSummaryHeader(m_window, m_window->windowTitle(),
                QString(), m_window->windowIcon());
            root->addWidget(summary);
            if (auto* title = summary->findChild<QLabel*>(QLatin1String("ksword_detail_summary_title")))
            {
                connect(m_window, &QWidget::windowTitleChanged, title, &QLabel::setText);
            }
            root->addWidget(body, 1);
            // 对话框按钮盒保留原角色和信号，放到固定底部，正文滚动时始终可达。
            const auto footers = body->findChildren<QDialogButtonBox*>(QString(), Qt::FindDirectChildrenOnly);
            for (auto* footer : footers)
            {
                original->removeWidget(footer);
                footer->setContentsMargins(20, 10, 20, 10);
                root->addWidget(footer);
            }
        }
        void prepareTabs()
        {
            const auto tabsList = m_window->findChildren<QTabWidget*>();
            for (auto* tabs : tabsList)
            {
                if (tabs->property("ksword_detail_tabs").toBool())
                {
                    continue;
                }
                bool nested = false;
                for (QWidget* parent = tabs->parentWidget(); parent && parent != m_window; parent = parent->parentWidget())
                {
                    if (qobject_cast<QTabWidget*>(parent))
                    {
                        nested = true;
                        break;
                    }
                }
                if (!nested)
                {
                    replaceDetailWidget(tabs, [tabs](QWidget* parent)
                    {
                        return ks::ui::CreateDetailTabShell(tabs, parent);
                    });
                }
                else
                {
                    tabs->setMinimumSize(0, 0);
                    tabs->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Ignored);
                }
            }
        }
        void prepareFields()
        {
            for (auto* source : m_window->findChildren<ks::ui::StructuredFieldView*>())
            {
                if (source->property("ksword_detail_projection_source").toBool())
                {
                    continue;
                }
                const bool scrollBody = !insideDetailScroll(source, m_window);
                replaceDetailWidget(source, [source, scrollBody](QWidget* parent)
                {
                    return ks::ui::CreateDetailFieldProjection(source, parent, scrollBody);
                });
            }
            for (auto* form : m_window->findChildren<QFormLayout*>())
            {
                if (form->property("ksword_detail_framework_form").toBool())
                {
                    continue;
                }
                form->setProperty("ksword_detail_framework_form", true);
                ks::ui::ConfigureDetailForm(form);
                for (int index = 0; index < form->rowCount(); ++index)
                {
                    auto* labelItem = form->itemAt(index, QFormLayout::LabelRole);
                    if (auto* label = labelItem ? qobject_cast<QLabel*>(labelItem->widget()) : nullptr)
                    {
                        label->setMinimumWidth(0);
                        label->setWordWrap(true);
                    }
                    auto* fieldItem = form->itemAt(index, QFormLayout::FieldRole);
                    if (auto* widget = fieldItem ? fieldItem->widget() : nullptr)
                    {
                        ks::ui::StyleStandaloneInput(widget);
                    }
                }
            }
        }
        void prepareScrollAreas()
        {
            for (auto* scroll : m_window->findChildren<QScrollArea*>())
            {
                if (scroll->property("ksword_detail_sidebar").toBool() || !scroll->widget())
                {
                    continue;
                }
                scroll->setFrameShape(QFrame::NoFrame);
                scroll->setWidgetResizable(true);
                scroll->setMinimumSize(0, 0);
                scroll->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Ignored);
                scroll->widget()->setMinimumWidth(0);
                scroll->widget()->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
                // 只处理单正文视图的重复滚动壳，参数+多个独立数据视图保留业务滚动边界。
                QLayout* layout = scroll->widget()->layout();
                if (layout && layout->count() == 1)
                {
                    QWidget* content = layout->itemAt(0)->widget();
                    const bool projection = content && content->property("ksword_detail_projection").toBool();
                    if (qobject_cast<QAbstractScrollArea*>(content)
                        || (projection && content->property("ksword_detail_projection_scroll").toBool()))
                    {
                        scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
                        scroll->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
                        scroll->widget()->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Ignored);
                        content->setMinimumSize(0, 0);
                        content->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Ignored);
                    }
                    else if (projection)
                    {
                        scroll->setVerticalScrollBarPolicy(Qt::ScrollBarAsNeeded);
                        content->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
                    }
                }
            }
        }
        void apply()
        {
            if (!m_window || !m_window->layout())
            {
                return;
            }
            m_applying = true;
            adoptRoot();
            prepareTabs();
            prepareFields();
            prepareScrollAreas();
            // 根窗口仍可刷新业务主题；共享外框只维护自己的末尾块，避免旧QSS覆盖卡片间底色。
            const QString begin = QStringLiteral("/* KSWORD_DETAIL_FRAMEWORK_BEGIN */");
            const QString end = QStringLiteral("/* KSWORD_DETAIL_FRAMEWORK_END */");
            QString style = m_window->styleSheet();
            const qsizetype first = style.indexOf(begin);
            const qsizetype last = first < 0 ? -1 : style.indexOf(end, first);
            if (first >= 0 && last >= first)
            {
                style.remove(first, last + end.size() - first);
            }
            style = style.trimmed() + QLatin1Char('\n') + begin
                + ks::ui::BuildDetailDialogChromeStyle(m_window->objectName()) + end;
            // 普通QDialog根QSS由GlobalDialogTheme唯一持有，不能与它轮流移除/追加样式块。
            if (!m_window->property("ksword_global_dialog_theme").toBool() && m_window->styleSheet() != style)
            {
                m_window->setStyleSheet(style);
            }
            // 禁止旧固定窗口尺寸限制目标布局；字段本身的值宽/编辑器行为由其原控件继续负责。
            m_window->setMaximumSize(QWIDGETSIZE_MAX, QWIDGETSIZE_MAX);
            m_window->setMinimumSize(qMin(m_window->minimumWidth(), 640), qMin(m_window->minimumHeight(), 440));
            const auto widgets = m_window->findChildren<QWidget*>();
            for (auto* widget : widgets)
            {
                if (!m_observed.contains(widget))
                {
                    m_observed.insert(widget);
                    widget->installEventFilter(this);
                    connect(widget, &QObject::destroyed, this, [this, widget]() { m_observed.remove(widget); });
                }
            }
            m_applying = false;
        }
        QPointer<QWidget> m_window; // 原窗口保留身份、模态关系和业务关闭语义。
        QSet<QWidget*> m_observed; // 只观察已登记窗口内控件，惰性页增量加入。
        bool m_pending = false;
        bool m_applying = false; // 防止重挂父对象期间重复整理同一布局。
    };
}

void ks::ui::InstallDetailWindowFramework(QWidget* window)
{
    if (!window || !window->isWindow())
    {
        return;
    }
    const auto controller = window->property("ksword_detail_framework_controller").value<QObject*>();
    if (controller)
    {
        static_cast<DetailWindowFramework*>(controller)->schedule();
        return;
    }
    window->setProperty("ksword_detail_shell", true);
    auto* framework = new DetailWindowFramework(window);
    window->setProperty("ksword_detail_framework_controller", QVariant::fromValue<QObject*>(framework));
}
