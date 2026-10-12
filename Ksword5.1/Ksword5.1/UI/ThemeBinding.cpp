#include "ThemeBinding.h"
#include "./MainSurfaceStyle.h"
#include "../theme.h"
#include <QAbstractSpinBox>
#include <QLineEdit>

#include <QApplication>
#include <QEvent>
#include <QElapsedTimer>
#include <QList>
#include <QObject>
#include <QPalette>
#include <QPointer>
#include <QSet>
#include <QThread>
#include <QTimer>
#include <QVariant>
#include <QWidget>
#include <QStackedWidget>
#include <QVector>

#include <algorithm>
#include <deque>
#include <list>
#include <utility>

namespace
{
    // kBindingProperty 保存控件自身的绑定对象；该对象由控件的 QObject 子树持有。
    constexpr char kBindingProperty[] = "KSWORD_EXPLICIT_THEME_BINDING";
    constexpr char kSearchFieldProperty[] = "KSWORD_SEARCH_FIELD_THEME"; // 已明确登记的搜索框。
    constexpr char kLineFieldProperty[] = "KSWORD_LINE_FIELD_THEME"; // 独立单行字段共用输入角色。
    constexpr char kSearchStyleBegin[] = "/* KSWORD_SEARCH_FIELD_BEGIN */"; // 本组件样式起点。
    constexpr char kSearchStyleEnd[] = "/* KSWORD_SEARCH_FIELD_END */"; // 本组件样式终点。
    constexpr char kSpinStyleBegin[] = "/* KSWORD_SPIN_FIELD_BEGIN */"; // 数值框独有颜色片段起点。
    constexpr char kSpinStyleEnd[] = "/* KSWORD_SPIN_FIELD_END */"; // 页面其它样式不属于此片段。
    class WidgetThemeBinding;

    // 只替换本组件拥有的颜色片段，页面的尺寸、padding 和其它规则继续保留。
    QString replaceFieldStyle(QString style, const QString& replacement,
        const char* beginMarker, const char* endMarker)
    {
        const qsizetype begin = style.indexOf(QLatin1String(beginMarker));
        const qsizetype end = style.indexOf(QLatin1String(endMarker), begin);
        if (begin >= 0 && end >= begin)
        {
            style.replace(begin, end + qstrlen(endMarker) - begin, replacement);
        }
        else
        {
            style += replacement;
        }
        return style;
    }

    // ThemeBindingRegistry 只维护已登记对象和合并队列，不猜测颜色含义或扫描业务控件树。
    class ThemeBindingRegistry final : public QObject
    {
    public:
        // application 是 GUI 应用对象，负责发布器寿命；没有额外所有权转移。
        explicit ThemeBindingRegistry(QApplication* application)
            : QObject(application)
        {
            application->installEventFilter(this);
        }

        // add 登记唯一绑定；schedule 只向下一轮事件循环提交一次刷新。
        void add(WidgetThemeBinding* binding);
        void schedule(WidgetThemeBinding* binding);
        void scheduleAll();
        void applyPending(WidgetThemeBinding* binding); // 首帧入口消费同一登记项，不另建主题绑定。

    protected:
        // 应用调色板更新只排队刷新；各控件的局部 palette 事件由自己的绑定观察。
        bool eventFilter(QObject* watched, QEvent* event) override
        {
            if (watched == parent() && event != nullptr &&
                event->type() == QEvent::ApplicationPaletteChange)
            {
                scheduleAll();
            }
            return QObject::eventFilter(watched, event);
        }

    private:
        // drain 限时处理弱引用队列；回调可创建/删除绑定，不影响未消费项的去重身份。
        void drain();
        std::list<QPointer<WidgetThemeBinding>> m_bindings; // 稳定迭代器，登记/销毁无需扫描全部绑定。
        std::deque<QPointer<WidgetThemeBinding>> m_pending; // 常数时间取队首，保持登记顺序。
        QSet<WidgetThemeBinding*> m_pendingMembership; // 仅作常数时间去重；实际队列仍持弱引用。
        bool m_scheduled = false; // 合并同一轮重复主题事件的调度门禁。
    };

    // registry 仅供 GUI 线程使用；QPointer 在 QApplication 销毁后自动失效。
    ThemeBindingRegistry* registry()
    {
        auto* application = qobject_cast<QApplication*>(QCoreApplication::instance());
        if (application == nullptr || QThread::currentThread() != application->thread())
        {
            return nullptr;
        }
        static QPointer<ThemeBindingRegistry> installedRegistry;
        if (installedRegistry == nullptr)
        {
            installedRegistry = new ThemeBindingRegistry(application);
        }
        return installedRegistry.data();
    }

    class WidgetThemeBinding final : public QObject
    {
    public:
        // widget 持有绑定；ownerRegistry 仅管理刷新调度，不改变 QWidget 父级或布局。
        WidgetThemeBinding(QWidget* widget, ThemeBindingRegistry* ownerRegistry,
            std::function<void()> refresh, ks::ui::ThemePalettePolicy palettePolicy)
            : QObject(widget), m_widget(widget), m_registry(ownerRegistry),
              m_refresh(std::move(refresh)), m_palettePolicy(palettePolicy)
        {
            widget->installEventFilter(this);
            widget->setProperty(kBindingProperty, QVariant::fromValue<QObject*>(this));
        }

        // replace 在同一控件上替换角色策略，不重复安装过滤器或增加登记项。
        void replace(std::function<void()> refresh, ks::ui::ThemePalettePolicy palettePolicy)
        {
            m_refresh = std::move(refresh);
            m_palettePolicy = palettePolicy;
        }

        // 首显与普通分片共用待刷新标记；同步首显后，旧队列节点只负责释放 membership。
        void requestRefresh()
        {
            m_refreshPending = true;
        }
        bool takePendingRefresh()
        {
            return std::exchange(m_refreshPending, false);
        }

        // apply 只在 GUI 线程调用；先复制函数，再执行可能销毁自身的样式更新。
        void apply()
        {
            if (m_widget == nullptr || !m_refresh)
            {
                return;
            }
            if (m_applying)
            {
                // 嵌套事件循环可能提前消费新策略的队列；外层退出后仍须保留这次刷新。
                m_refreshRequested = true;
                return;
            }
            m_applying = true;
            const QPointer<WidgetThemeBinding> lifetime(this); // 刷新后探活，避免访问已删对象。
            const QPointer<QWidget> widget(m_widget); // palette 更新本身也可能使控件关闭。
            const bool preserveLocal = m_palettePolicy == ks::ui::ThemePalettePolicy::PreserveLocal;
            const QPalette localPalette = widget->palette(); // 保留独立窗口声明的当前局部角色。
            const auto refresh = m_refresh; // 回调中重新登记也不会销毁正在执行的函数。
            refresh();
            if (lifetime != nullptr && widget != nullptr && preserveLocal)
            {
                // Qt setStyleSheet 会把后设的局部 palette 还原为旧 QSS 基线，因此需要显式恢复。
                if (widget->palette() != localPalette)
                {
                    widget->setPalette(localPalette); // 无变化时不再次广播整棵子树的PaletteChange。
                }
            }
            if (lifetime != nullptr)
            {
                lifetime->m_hasApplied = true; // 首绘前只补尚未应用的绑定，不把后续换肤改成同步全树。
                lifetime->m_applying = false;
                if (lifetime->m_refreshRequested && lifetime->m_registry != nullptr)
                {
                    lifetime->m_refreshRequested = false;
                    lifetime->m_registry->schedule(lifetime.data());
                }
            }
        }

    protected:
        // 自身setStyleSheet的局部Palette重入不排队；真实语言/应用主题广播仍保留后续刷新。
        bool eventFilter(QObject* watched, QEvent* event) override
        {
            // 完整挂载入口负责提前准备；动态创建的叶控件在首次 Paint 派发前补本地颜色。
            // 初次 Show 不执行未知回调；已应用过的隐藏页重显时先消费待刷新颜色。
            // Popup/Tooltip 保持既有延迟约束，常驻页面的换肤继续走分片队列。
            const bool initialPaint = event != nullptr && event->type() == QEvent::Paint && !m_hasApplied;
            const bool reshow = event != nullptr && event->type() == QEvent::Show && m_hasApplied;
            if ((initialPaint || reshow)
                && m_refreshPending && !m_applying && m_widget != nullptr && m_registry != nullptr
                && m_widget->window()->windowType() != Qt::Popup
                && m_widget->window()->windowType() != Qt::ToolTip)
            {
                const QPointer<QWidget> owner(m_widget); // 本地样式回调可能关闭所属控件。
                const QPointer<WidgetThemeBinding> lifetime(this); // 也允许单独删除绑定而保留控件。
                m_registry->applyPending(this);
                if (lifetime.isNull())
                {
                    return owner.isNull();
                }
                if (owner.isNull())
                {
                    return true;
                }
            }
            if (m_registry != nullptr && event != nullptr &&
                (event->type() == QEvent::ApplicationPaletteChange ||
                    event->type() == QEvent::LanguageChange ||
                    (!m_applying && event->type() == QEvent::PaletteChange)))
            {
                m_registry->schedule(this);
            }
            return QObject::eventFilter(watched, event);
        }

    private:
        QPointer<QWidget> m_widget; // 明确拥有样式的单个控件，不隐式接管子控件。
        QPointer<ThemeBindingRegistry> m_registry; // 应用销毁后停止调度。
        std::function<void()> m_refresh; // 根据稳定语义角色重新求值的完整刷新函数。
        ks::ui::ThemePalettePolicy m_palettePolicy; // 是否保留独立窗口的局部调色板。
        bool m_applying = false; // 禁止刷新过程中因自身样式事件递归调度。
        bool m_refreshRequested = false; // 嵌套循环消费的刷新在外层结束后重新排队。
        bool m_refreshPending = false; // 首显或分片尚未消费的同一刷新请求。
        bool m_hasApplied = false; // 首次完成后恢复正常分片，不在每次 Paint 执行主题函数。
    };

    // add 将对象放入唯一登记表，并安排首次刷新；不在控件构造期间同步执行样式函数。
    void ThemeBindingRegistry::add(WidgetThemeBinding* binding)
    {
        // 每个绑定只增加一个节点；销毁时原位移除，创建大页不再形成逐项全表扫描。
        const auto registration = m_bindings.emplace(m_bindings.end(), binding);
        connect(binding, &QObject::destroyed, this, [this, registration, binding]()
        {
            m_bindings.erase(registration);
            m_pendingMembership.remove(binding); // 防止释放后同址新对象被旧待刷新地址误判为已排队。
        });
        schedule(binding);
    }

    void ThemeBindingRegistry::schedule(WidgetThemeBinding* binding)
    {
        if (binding == nullptr)
        {
            return;
        }
        binding->requestRefresh();
        // weakBinding 为当前对象的弱引用；重复事件只保留一次待刷新记录。
        const QPointer<WidgetThemeBinding> weakBinding(binding);
        if (!m_pendingMembership.contains(binding))
        {
            m_pendingMembership.insert(binding);
            m_pending.push_back(weakBinding);
        }
        if (m_scheduled)
        {
            return;
        }
        m_scheduled = true;
        QTimer::singleShot(0, this, [this]() { drain(); });
    }

    void ThemeBindingRegistry::scheduleAll()
    {
        // 只遍历存活登记项一次，排队去重由哈希表负责；换肤由二次方查找变为线性处理。
        for (const auto& binding : m_bindings)
        {
            schedule(binding.data());
        }
    }

    // 节点仍留在 FIFO 中去重；若之后到达新主题请求，该节点可消费新请求而不丢更新。
    void ThemeBindingRegistry::applyPending(WidgetThemeBinding* binding)
    {
        if (binding != nullptr && binding->takePendingRefresh())
        {
            binding->apply();
        }
    }

    void ThemeBindingRegistry::drain()
    {
        // 尚未消费的子控件继续保持membership，父样式的PaletteChange不再将它们重复入队。
        // 只在实际取出时解除去重，保留回调重登记及真正嵌套更新的后续刷新。
        QElapsedTimer slice;
        slice.start();
        const std::size_t initialCount = m_pending.size();
        for (std::size_t index = 0; index < initialCount && !m_pending.empty(); ++index)
        {
            const QPointer<WidgetThemeBinding> binding = m_pending.front();
            m_pending.pop_front();
            if (binding != nullptr)
            {
                m_pendingMembership.remove(binding.data());
                applyPending(binding.data());
            }
            // 不在同一次GUI回调里刷新全部页面，给输入、布局和绘制留出事件循环机会。
            // 单个业务回调仍必须自己有界；这里不能中断正在执行的setStyleSheet。
            if (slice.elapsed() >= 6)
            {
                break;
            }
        }
        // 处理期间始终保留scheduled，嵌套事件循环不会启动第二个drain消费同一队列。
        m_scheduled = false;
        if (!m_pending.empty())
        {
            m_scheduled = true;
            QTimer::singleShot(0, this, [this]() { drain(); });
        }
    }

    // bindingForWidget 仅从仍存活的直接子对象中确认缓存地址，避免手动删子对象后的悬空属性。
    WidgetThemeBinding* bindingForWidget(const QWidget* widget)
    {
        if (widget == nullptr)
        {
            return nullptr;
        }
        QObject* cached = widget->property(kBindingProperty).value<QObject*>(); // 只比较缓存地址。
        if (cached == nullptr)
        {
            return nullptr;
        }
        for (QObject* child : widget->children()) // child 由 GUI 线程中的现有 QObject 子树持有。
        {
            if (child == cached)
            {
                return dynamic_cast<WidgetThemeBinding*>(child);
            }
        }
        return nullptr;
    }

    // 只收当前可显示分支和现成控件，不提前创建其它 Tab、独立窗口或 Popup。
    void collectShownBranch(QWidget* widget, QVector<QPointer<QWidget>>& branch)
    {
        if (widget == nullptr)
        {
            return;
        }
        branch.append(widget);
        const auto* stack = qobject_cast<QStackedWidget*>(widget); // 隐藏 Tab 不能进入首帧工作量。
        for (QObject* object : widget->children())
        {
            auto* child = qobject_cast<QWidget*>(object); // 布局/模型 QObject 无需参与颜色准备。
            if (child == nullptr || child->isWindow() || !child->isVisibleTo(widget)
                || (stack != nullptr && child != stack->currentWidget()))
            {
                continue;
            }
            collectShownBranch(child, branch);
        }
    }
}

// 构造完成并建立最终父级后，由页面挂载/详情 Show 入口调用；仅消费已登记的局部主题。
void ks::ui::PrepareWidgetThemeBindingsForShow(QWidget* scope)
{
    const QPointer<ThemeBindingRegistry> ownerRegistry(registry()); // 同时确认应用寿命与 GUI 线程。
    if (ownerRegistry == nullptr || scope == nullptr || scope->thread() != QThread::currentThread()
        || scope->window()->windowType() == Qt::Popup || scope->window()->windowType() == Qt::ToolTip)
    {
        return;
    }
    QVector<QPointer<QWidget>> branch; // 回调可析构或重挂控件，收集后全部通过弱引用探活。
    collectShownBranch(scope, branch);
    for (const QPointer<QWidget>& widget : branch)
    {
        if (widget != nullptr && ownerRegistry != nullptr)
        {
            ownerRegistry->applyPending(bindingForWidget(widget.data()));
        }
    }
}

ks::ui::ScopedWidgetThemePreparation::ScopedWidgetThemePreparation(QWidget* scope)
    : m_scope(scope)
{
}

ks::ui::ScopedWidgetThemePreparation::~ScopedWidgetThemePreparation()
{
    // 页面工厂已返回业务构造/挂载的所有步骤；构造期不可见与后台预建都保持原分片。
    if (m_scope != nullptr && m_scope->thread() == QThread::currentThread() && m_scope->isVisible())
    {
        PrepareWidgetThemeBindingsForShow(m_scope.data());
    }
}

bool ks::ui::BindWidgetTheme(QWidget* widget, std::function<void()> refresh,
    ThemePalettePolicy palettePolicy)
{
    ThemeBindingRegistry* ownerRegistry = registry(); // 同时校验 QApplication 和 GUI 线程。
    if (ownerRegistry == nullptr || widget == nullptr || !refresh ||
        widget->thread() != QThread::currentThread())
    {
        return false;
    }
    // 只复用本组件写入、且仍由该控件持有的绑定；不会借用其他属性中的 QObject。
    auto* binding = bindingForWidget(widget); // 只使用现有直接子树中的存活绑定。
    if (binding != nullptr && binding->parent() == widget)
    {
        binding->replace(std::move(refresh), palettePolicy);
        ownerRegistry->schedule(binding);
    }
    else
    {
        binding = new WidgetThemeBinding(widget, ownerRegistry, std::move(refresh), palettePolicy);
        ownerRegistry->add(binding);
    }
    return true;
}

// 数值与单位由原生编辑器维护；主题绑定只修复可见文字与实际底色的对比度。
bool ks::ui::BindSpinBoxTheme(QAbstractSpinBox* spinBox)
{
    if (spinBox == nullptr)
    {
        return false;
    }
    const QPointer<QAbstractSpinBox> guardedSpin(spinBox);
    const bool outerBound = BindWidgetTheme(spinBox, [guardedSpin]()
    {
        if (guardedSpin.isNull())
        {
            return;
        }
        const QColor background = UsesMainSurfaceHierarchy(guardedSpin)
            ? MainWindowSurfaceColors().input : KswordTheme::ControlInputSurfaceColor();
        const QColor hoverBackground = KswordTheme::ControlInputHoverColor(background);
        const QColor focusBackground = KswordTheme::ControlInputFocusColor(background);
        const QColor backgrounds[] = {background, hoverBackground, focusBackground};
        const QColor foreground = KswordTheme::EnsureTextContrastForBackgrounds(
            KswordTheme::TextPrimaryColor(), backgrounds, 3);
        const QColor disabledBackground = KswordTheme::SurfaceMutedColor();
        const QColor disabledForeground = KswordTheme::EnsureTextContrast(
            KswordTheme::TextDisabledColor(), disabledBackground, 3.0);
        const QString block = QString::fromLatin1(kSpinStyleBegin) + QStringLiteral(
            "QAbstractSpinBox{background-color:%1;color:%2;border:none;}"
            "QAbstractSpinBox:hover{background-color:%3;border:none;}"
            "QAbstractSpinBox:focus{background-color:%4;border:none;}"
            "QAbstractSpinBox:disabled{background-color:%5;color:%6;border:none;}")
            .arg(background.name(), foreground.name(), hoverBackground.name(), focusBackground.name(),
                disabledBackground.name(), disabledForeground.name())
            + QString::fromLatin1(kSpinStyleEnd);
        const QString style = replaceFieldStyle(guardedSpin->styleSheet(), block,
            kSpinStyleBegin, kSpinStyleEnd); // 保留页面自己设置的宽度、padding 与步进几何。
        if (guardedSpin->styleSheet() != style)
        {
            guardedSpin->setStyleSheet(style);
        }
    });

    // 内部 QLineEdit 单独登记，旧色值补偿不会再次覆盖已经明确拥有的前景规则。
    QLineEdit* editor = spinBox->findChild<QLineEdit*>(QStringLiteral("qt_spinbox_lineedit"));
    if (editor == nullptr)
    {
        return outerBound;
    }
    const QPointer<QLineEdit> guardedEditor(editor);
    const bool editorBound = BindWidgetTheme(editor, [guardedEditor]()
    {
        if (guardedEditor.isNull())
        {
            return;
        }
        const QColor background = UsesMainSurfaceHierarchy(guardedEditor)
            ? MainWindowSurfaceColors().input : KswordTheme::ControlInputSurfaceColor();
        const QColor backgrounds[] = {background, KswordTheme::ControlInputHoverColor(background),
            KswordTheme::ControlInputFocusColor(background)}; // 内部文字随外层三态底面校准。
        const QColor foreground = KswordTheme::EnsureTextContrastForBackgrounds(
            KswordTheme::TextPrimaryColor(), backgrounds, 3);
        const QColor disabledForeground = KswordTheme::EnsureTextContrast(
            KswordTheme::TextDisabledColor(), KswordTheme::SurfaceMutedColor(), 3.0);
        const QString block = QString::fromLatin1(kSpinStyleBegin) + QStringLiteral(
            "QLineEdit,QLineEdit:hover,QLineEdit:focus,QLineEdit:read-only{background:transparent;color:%1;border:none;"
            "selection-background-color:%2;selection-color:%3;}"
            "QLineEdit:disabled{background:transparent;color:%4;border:none;}")
            .arg(foreground.name(), KswordTheme::ControlAccentColor().name(),
                KswordTheme::OnAccentColor(KswordTheme::ControlAccentColor()).name(), disabledForeground.name())
            + QString::fromLatin1(kSpinStyleEnd);
        const QString style = replaceFieldStyle(guardedEditor->styleSheet(), block,
            kSpinStyleBegin, kSpinStyleEnd);
        if (guardedEditor->styleSheet() != style)
        {
            guardedEditor->setStyleSheet(style);
        }
    });
    return outerBound && editorBound;
}

bool ks::ui::BindLineEditTheme(QLineEdit* searchField)
{
    if (searchField == nullptr)
    {
        return false;
    }
    // 同一字段重复接入不增加绑定，原有独立业务配色由明确接入方负责清理。
    if (searchField->property(kLineFieldProperty).toBool() && HasWidgetThemeBinding(searchField))
    {
        return true;
    }
    const QPointer<QLineEdit> guardedField(searchField); // 排队刷新不会延长字段的寿命。
    const bool bound = BindWidgetTheme(searchField, [guardedField]()
    {
        if (guardedField.isNull())
        {
            return;
        }
        const QPalette parentColors = guardedField->parentWidget() != nullptr
            ? guardedField->parentWidget()->palette() : QApplication::palette();
        QColor pageSurface = parentColors.color(QPalette::Active, QPalette::Base);
        if (!pageSurface.isValid() || pageSurface.alpha() != 255)
        {
            // 透明背景中的零 alpha 黑并不代表实际黑底，不能直接把它改成不透明。
            pageSurface = KswordTheme::SurfaceColor();
        }
        QColor background = parentColors.color(QPalette::Active, QPalette::AlternateBase);
        if (!background.isValid() || background.alpha() != 255)
        {
            background = KswordTheme::SurfaceAltColor();
        }
        // 主页面固定采用编辑层，不从已变亮的父容器继续计算更亮的底面。
        const bool mainSurface = UsesMainSurfaceHierarchy(guardedField);
        background = mainSurface ? MainWindowSurfaceColors().input
            : KswordTheme::ControlInputSurfaceColor(pageSurface, background);
        const QColor hoverBackground = KswordTheme::ControlInputHoverColor(background);
        const QColor focusBackground = KswordTheme::ControlInputFocusColor(background);
        const QColor backgrounds[] = {background, hoverBackground, focusBackground};
        // 占位提示常有半透明 alpha；先还原明确前景再校准，不能把透明黑当有效首选色。
        const auto opaqueForeground = [](QColor preferred, const QColor& fallback)
        {
            if (!preferred.isValid() || preferred.alpha() == 0)
            {
                preferred = fallback;
            }
            preferred.setAlpha(255);
            return preferred;
        };
        const QColor foreground = KswordTheme::EnsureTextContrastForBackgrounds(opaqueForeground(
            mainSurface ? KswordTheme::TextPrimaryColor() : parentColors.color(QPalette::Active, QPalette::Text),
            KswordTheme::TextPrimaryColor()), backgrounds, 3);
        const QColor placeholder = KswordTheme::EnsureTextContrastForBackgrounds(opaqueForeground(
            mainSurface ? KswordTheme::TextSecondaryColor() : parentColors.color(QPalette::Active, QPalette::PlaceholderText),
            KswordTheme::TextSecondaryColor()), backgrounds, 3);
        const QColor disabledForeground = KswordTheme::EnsureTextContrast(opaqueForeground(
            parentColors.color(QPalette::Disabled, QPalette::Text), KswordTheme::TextDisabledColor()), background, 3.0);
        const QColor disabledPlaceholder = KswordTheme::EnsureTextContrast(opaqueForeground(
            parentColors.color(QPalette::Disabled, QPalette::PlaceholderText), KswordTheme::TextSecondaryColor()), background, 3.0);

        // 搜索与普通输入共享柔和交互填充；保留页面几何与具体的 placeholder 文本。
        // 页面明确选择统一搜索几何后，局部主题重建也必须保留同一留白和圆角。
        const QString geometry = guardedField->property("ksword_page_search_style").toBool()
            ? QStringLiteral("QLineEdit[ksword_page_search_style=\"true\"]{padding:0px 10px;border-radius:5px;}") : QString();
        const QString block = QString::fromLatin1(kSearchStyleBegin) + geometry + QStringLiteral(
            "QLineEdit,QLineEdit:read-only{background-color:%1;color:%2;placeholder-text-color:%6;border:none;}"
            "QLineEdit:hover{background-color:%4;border:none;}"
            "QLineEdit:focus{background-color:%5;border:none;}"
            "QLineEdit:disabled{background-color:%1;color:%3;placeholder-text-color:%7;border:none;}")
            .arg(background.name(), foreground.name(), disabledForeground.name(),
                hoverBackground.name(), focusBackground.name(), placeholder.name(), disabledPlaceholder.name())
            + QString::fromLatin1(kSearchStyleEnd);
        const QString style = replaceFieldStyle(guardedField->styleSheet(), block,
            kSearchStyleBegin, kSearchStyleEnd);
        if (guardedField->styleSheet() != style)
        {
            guardedField->setStyleSheet(style);
        }
        if (guardedField.isNull())
        {
            return;
        }
        // PlaceholderText 由 QLineEdit 实际绘制读取，不依赖父级 color 或半透明默认色。
        QPalette fieldColors = guardedField->palette(); // 只改本字段自己的可读颜色角色。
        for (const QPalette::ColorGroup group : {QPalette::Active, QPalette::Inactive, QPalette::Disabled})
        {
            const bool disabled = group == QPalette::Disabled;
            fieldColors.setColor(group, QPalette::Base, background);
            fieldColors.setColor(group, QPalette::Text, disabled ? disabledForeground : foreground);
            fieldColors.setColor(group, QPalette::PlaceholderText,
                disabled ? disabledPlaceholder : placeholder);
        }
        if (guardedField->palette() != fieldColors)
        {
            guardedField->setPalette(fieldColors);
        }
    });
    if (bound && !guardedField.isNull())
    {
        guardedField->setProperty(kLineFieldProperty, true);
    }
    return bound;
}

// 搜索沿用单行字段完整配色，不改变具体过滤词、输入行为或页面已登记的搜索几何。
bool ks::ui::BindSearchFieldTheme(QLineEdit* searchField)
{
    if (searchField == nullptr) return false;
    const bool bound = BindLineEditTheme(searchField);
    if (bound) searchField->setProperty(kSearchFieldProperty, true);
    return bound;
}

bool ks::ui::HasWidgetThemeBinding(const QWidget* widget)
{
    // 判定只在 GUI 线程的旧样式兼容路径调用；未登记子控件仍允许原补偿机制处理。
    return bindingForWidget(widget) != nullptr;
}

void ks::ui::RefreshWidgetThemeBindings()
{
    if (ThemeBindingRegistry* ownerRegistry = registry())
    {
        ownerRegistry->scheduleAll();
    }
}
