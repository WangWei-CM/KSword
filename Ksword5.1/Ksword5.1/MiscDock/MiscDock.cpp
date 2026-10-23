#include "MiscDock.h"
#include "../UI/PageControlStyle.h"
#include "../UI/PrimaryPageStyle.h"
#include "../UI/ThemeBinding.h"
#include "../UI/AsyncOperation.h"

#include "BootEditor/BootEditorTab.h"
#include "ApplicationControlPage.h"
#include "ClipboardGuard/ClipboardGuardPage.h"
#include "ContextMenuCleaner/ContextMenuCleanerTab.h"
#include "DisableDse/DisableDsePage.h"
#include "CKS/CksPage.h"
#include "./ThemeCheckPage.h"
#include "Experimental/BugcheckGuardPage.h"
#include "DiskEditor/DiskEditorTab.h"
#include "RenderBenchmark/RenderBenchmarkPage.h"
#include "DesktopDrawing/DesktopDrawingPage.h"
#include "HvciBypass/HvciBypassPage.h"
#include "HvciBypass/RxpfPage.h"
#include "SoundSource/SoundSourcePage.h"
#include "SystemTime/SystemTimePage.h"
#include "VirtualLocation/VirtualLocationPage.h"

// 扫描器 / 转储分析 / 插件原本是三个顶层 Dock，为精简 dock 栏入口已并入本页。
#include "../MinidumpDock/MinidumpDock.h"
#include "../ScannerDock/ScannerDock.h"
#include "../PluginHost.h"
#include "../OtherDock/WindowInputControl.h"

#include "../Internationalization/LanguageManager.h"
#include <QIcon>
#include <QPointer>
#include <QShowEvent>
#include <QTabBar>
#include <QTabWidget>
#include <QTimer>
#include <QVBoxLayout>
#include <QWidget>

namespace {
const ks::ui::PageNavigationTabDefinition kMiscNavigation[] = {
    {"misc.boot", "", "", "引导", true},
    {"misc.sound", "", "", "声音来源", false},
    {"misc.time", "", "", "系统变速", false},
    {"misc.location", "", "misc.virtual_location.tab", "虚拟定位", false},
    {"misc.bugcheck", "", "misc.bugcheck_diagnostics.tab", "蓝屏诊断", false},
    {"misc.dse", "", "misc.disable_dse.tab", "禁用签名验证", false},
    {"misc.cks", "", "misc.cks.tab", "CKS 管理", false},
    {"misc.shell", "", "", "Shell 关联管理", false},
    {"misc.disk", "", "", "磁盘编辑", false},
    {"misc.app", "", "", "应用控制", false},
    {"misc.clipboard", "", "misc.clipboard_guard.tab", "剪贴板保护", false},
    {"misc.render", "", "misc.render_benchmark.tab", "渲染基准", false},
    {"misc.drawing", "", "misc.desktop_drawing.tab", "桌面绘制", false},
    {"misc.injection", "", "misc.window_injection.tab", "DWM / Win32k 注入", false},
    {"misc.hvci", "", "misc.hvci_bypass.tab", "HVCI 绕过", false},
    {"misc.rxpf", "", "misc.rxpf.tab", "RXPF 模拟", false},
    {"misc.scanner", "", "misc.scanner.tab", "扫描器", false},
    {"misc.minidump", "", "misc.minidump.tab", "转储分析", false},
    {"misc.plugin", "", "misc.plugin.tab", "插件", false},
    {"misc.theme", "", "misc.theme_check.tab", "主题检查", false}
};
const char* const kMiscChildKeys[] = {
    nullptr,
    nullptr,
    nullptr,
    nullptr,
    nullptr,
    nullptr,
    "misc.cks",
    "misc.context",
    "misc.disk_editor",
    "misc.app_control",
    nullptr,
    nullptr,
    nullptr,
    nullptr,
    nullptr,
    nullptr,
    "misc.scanner",
    "misc.minidump",
    "misc.plugin",
    "misc.theme_check"
};
}

namespace
{
    // buildHostLayout：
    // - 作用：为页签占位控件建立统一的承载布局，边距与间距全部归零，
    //   让真实子页占满原来直接作为页签页时的同一块区域；
    // - 入参 hostWidget：页签占位控件，必须非空；
    // - 返回：已挂到占位控件上的垂直布局。
    QVBoxLayout* buildHostLayout(QWidget* hostWidget)
    {
        hostWidget->setProperty("ksword_page_frame_host", true); // 明确的单页惰性壳允许框架读取真实页外围。
        QVBoxLayout* hostLayout = new QVBoxLayout(hostWidget);
        hostLayout->setContentsMargins(0, 0, 0, 0);
        hostLayout->setSpacing(0);
        return hostLayout;
    }
}

MiscDock::MiscDock(QWidget* parent, const bool initializeCurrentPage)
    : QWidget(parent), m_initializeCurrentPageOnShow(!initializeCurrentPage)
{
    initializeUi(initializeCurrentPage);

    kLogEvent initEvent;
    info << initEvent << "[MiscDock] 杂项页面初始化完成。" << eol;
}

MiscDock::~MiscDock()
{
    if (m_disableDseSearchOperation) m_disableDseSearchOperation->close();
    if (m_desktopDrawingSearchOperation) m_desktopDrawingSearchOperation->close();
    if (m_windowInjectionSearchOperation) m_windowInjectionSearchOperation->close();
}

void MiscDock::initializeUi(const bool initializeCurrentPage)
{
    // 根布局负责承载整个“杂项”容器。
    m_rootLayout = new QVBoxLayout(this);
    m_rootLayout->setContentsMargins(0, 0, 0, 0);
    m_rootLayout->setSpacing(0);

    // 主 Tab 承载所有杂项工具，保持 Dock 外层只暴露一个统一入口。
    m_mainTabWidget = new QTabWidget(this);
    // 只有杂项主Tab承接ADS方框，惰性子页的结果区域不另造外圆。
    ks::ui::StyleSquarePageTabsFrame(m_mainTabWidget);
    m_mainTabWidget->setObjectName(QStringLiteral("ksMiscDockMainTab"));
    m_rootLayout->addWidget(m_mainTabWidget, 1);

    // 懒加载策略：
    // - 七个页签先用空占位控件占住，页签顺序、标题与图标与旧实现逐一对齐；
    // - 真实子页在页签第一次被选中时才构造，避免“杂项”页一被创建就把磁盘、
    //   Shell 关联、应用控制等重页面全部建出来（每一个都会自带首轮枚举）。
    m_bootEditorHostWidget = new QWidget(m_mainTabWidget);
    m_soundSourceHostWidget = new QWidget(m_mainTabWidget);
    m_systemTimeHostWidget = new QWidget(m_mainTabWidget);
    m_virtualLocationHostWidget = new QWidget(m_mainTabWidget);
    m_bugcheckGuardHostWidget = new QWidget(m_mainTabWidget);
    m_disableDseHostWidget = new QWidget(m_mainTabWidget);
    m_cksHostWidget = new QWidget(m_mainTabWidget);
    m_contextMenuCleanerHostWidget = new QWidget(m_mainTabWidget);
    m_diskEditorHostWidget = new QWidget(m_mainTabWidget);
    m_applicationControlHostWidget = new QWidget(m_mainTabWidget);
    m_clipboardGuardHostWidget = new QWidget(m_mainTabWidget);
    m_renderBenchmarkHostWidget = new QWidget(m_mainTabWidget);
    m_desktopDrawingHostWidget = new QWidget(m_mainTabWidget);
    m_windowInjectionHostWidget = new QWidget(m_mainTabWidget);
    m_hvciBypassHostWidget = new QWidget(m_mainTabWidget);
    m_rxpfHostWidget = new QWidget(m_mainTabWidget);
    m_scannerHostWidget = new QWidget(m_mainTabWidget);
    m_minidumpHostWidget = new QWidget(m_mainTabWidget);
    m_pluginHostWidget = new QWidget(m_mainTabWidget);
    // 插件宿主必须能被主题样式单独锚定：Tab 插件用原生子窗口承载，
    // 父链一旦被刷成透明，插件画面就会变成黑块或不刷新。
    m_pluginHostWidget->setObjectName(QStringLiteral("ksMiscPluginHost"));

    m_bootEditorTabIndex = addNavigationTab(0, m_bootEditorHostWidget);

    // 声音来源页：
    // - R3 连续采样 Core Audio 输出会话峰值并归因到 PID；
    // - R0 复用进程 Cross-View 与 Runtime Detail 交叉核验候选 PID。
    m_soundSourceTabIndex = addNavigationTab(1, m_soundSourceHostWidget, QIcon(QStringLiteral(":/Icon/sound_source.svg")));

    // 系统全局变速页：
    // - 通过 ArkDriverClient 控制 R0 性能计数器连续倍率映射；
    // - 永久展示失稳风险，并在每次启用前执行双重确认。
    m_systemTimeTabIndex = addNavigationTab(2, m_systemTimeHostWidget, QIcon(QStringLiteral(":/Icon/system_time.svg")));

    // 虚拟定位页：
    // - 把任意坐标写成 Windows 位置服务的“系统默认位置”，R3 被 ACL 拒绝时回退 R0 注册表 IOCTL；
    // - 只影响走 Windows 定位 API 的调用方，页面内长期展示这条生效边界。
    m_virtualLocationTabIndex = addNavigationTab(3, m_virtualLocationHostWidget, QIcon(QStringLiteral(":/Icon/virtual_location.svg")));

    // 蓝屏诊断入口默认隐藏。R3 配置自动安装或本次成功安装后，MainWindow 才会显式显示它。
    m_bugcheckGuardTabIndex = addNavigationTab(4, m_bugcheckGuardHostWidget, QIcon(QStringLiteral(":/Icon/codeeditor_replace.svg")));
    // i18n 绑定挂在占位控件上：LanguageManager 按 QTabWidget::widget(index) 回读属性，
    // 占位控件才是常驻页签页，真实子页只是它的孩子。
    setBugcheckDiagnosticsVisible(false);

    // 驱动签名强制页：
    // - 运行时反汇编定位 CI.dll!g_CiOptions，用 R0 事务化内核写临时关闭 DSE；
    // - 写入前校验读回值的强制签名位与系统自报状态自洽，
    //   HVCI 开启时直接禁用操作，页面析构还会兜底把原值写回。
    m_disableDseTabIndex = addNavigationTab(5, m_disableDseHostWidget, QIcon(QStringLiteral(":/Icon/codeeditor_replace.svg")));

    // CKS 是带签名策略的驱动信任管理，与临时关闭 DSE 的操作完全分开。
    m_cksTabIndex = addNavigationTab(6, m_cksHostWidget, QIcon(QStringLiteral(":/Icon/process_details.svg")));

    // Shell 关联管理页：
    // - 覆盖右键菜单、URL 绑定、打开方式和 Explorer 第三方主页项；
    // - 仅在用户确认后删除表格绑定的精确注册表子树或值。
    m_contextMenuCleanerTabIndex = addNavigationTab(7, m_contextMenuCleanerHostWidget, QIcon(QStringLiteral(":/Icon/log_track.svg")));

    // 磁盘编辑页：
    // - 参考 DiskGenius 类工具布局，提供横向柱形分区图；
    // - 默认只读，用户显式解锁后才允许写回物理磁盘。
    m_diskEditorTabIndex = addNavigationTab(8, m_diskEditorHostWidget, QIcon(QStringLiteral(":/Icon/disk_storage.svg")));

    // 应用控制页：
    // - 第一版仅做 AppLocker / WDAC / Defender / 事件日志只读诊断；
    // - 不修改、不删除、不禁用任何策略。
    m_applicationControlTabIndex = addNavigationTab(9, m_applicationControlHostWidget, QIcon(QStringLiteral(":/Icon/process_details.svg")));

    // 剪贴板保护页：
    // - 按 PID/映像名/路径管理剪贴板读/写/枚举策略，规则整表下发到驱动持久保存；
    // - 实际拦截发生在被注入到受管进程里的 APIMonitor_x64.dll（复用现有 hook 引擎）；
    // - 驱动本身只存表、不判定拦截，详见 shared/driver/KswordArkClipboardPolicyIoctl.h。
    m_clipboardGuardTabIndex = addNavigationTab(10, m_clipboardGuardHostWidget, QIcon(QStringLiteral(":/Icon/clipboard_guard.svg")));

    // 渲染基准页：
    // - 量化主窗口整树重绘、拖动掉帧、DWM 合成与目标窗口响应；
    // - 全部测量都在 UI 线程执行，只在用户显式点击时运行。
    m_renderBenchmarkTabIndex = addNavigationTab(11, m_renderBenchmarkHostWidget, QIcon(QStringLiteral(":/Icon/codeeditor_replace.svg")));

    // 页面懒加载且默认不绘制；用户开始后可跨页运行，用全局热键停止。
    m_desktopDrawingTabIndex = addNavigationTab(12, m_desktopDrawingHostWidget, QIcon(QStringLiteral(":/Icon/codeeditor_replace.svg")));

    m_windowInjectionTabIndex = addNavigationTab(13, m_windowInjectionHostWidget);

    // HVCI 绕过实验页：
    // - 在 HVCI 启用的环境下测试 MmChangeImageProtection 能否将映像页改成 NX、
    //   通过 MDL 别名写入修改后的内容、再恢复 RX 权限使修改持久化。
    m_hvciBypassTabIndex = addNavigationTab(14, m_hvciBypassHostWidget, QIcon(QStringLiteral(":/Icon/codeeditor_replace.svg")));

    // RXPF 模拟执行实验页：
    // - 接管影子 IDT 的 #PF 向量，对被标记为 NX 的内核映像页进行 x64 指令模拟；
    // - 无需恢复 RX 权限即可在 HVCI 下执行被修改的代码页。
    m_rxpfTabIndex = addNavigationTab(15, m_rxpfHostWidget, QIcon(QStringLiteral(":/Icon/development/terminal_line.svg")));

    // 以下三页原本是顶层 Dock，为精简 dock 栏入口并入杂项：
    // - 页签顺序沿用它们在原 dock 栏中的相对先后（扫描器 -> 转储分析 -> 插件）；
    // - 三页都保持懒加载，打开“杂项”本身不会把扫描器/转储解析/插件宿主一起建出来。
    m_scannerTabIndex = addNavigationTab(16, m_scannerHostWidget, QIcon(QStringLiteral(":/Icon/disk_analyze.svg")));

    m_minidumpTabIndex = addNavigationTab(17, m_minidumpHostWidget, QIcon(QStringLiteral(":/Icon/log_track.svg")));

    m_pluginTabIndex = addNavigationTab(18, m_pluginHostWidget, QIcon(QStringLiteral(":/Icon/process_details.svg")));

    // 主题检查使用真实主程序中的惰性页面，样例控件不逐个手工适配。
    m_themeCheckHostWidget = new QWidget(m_mainTabWidget);
    m_themeCheckTabIndex = addNavigationTab(19, m_themeCheckHostWidget);

    // 页签切换：按需初始化对应子页。
    connect(
        m_mainTabWidget,
        &QTabWidget::currentChanged,
        this,
        [this](const int tabIndex)
        {
            ensureTabInitialized(tabIndex);
        });

    // 首屏页必须同步初始化：
    // - ADS 可能直接把“杂项”恢复为当前 Dock，此时不会再触发一次 currentChanged；
    // - 只初始化当前页，其余页面仍然保持懒加载。
    if (!initializeCurrentPage) return; // Full search retains only proven cold body shells.
    ensureTabInitialized(m_mainTabWidget->currentIndex());

    // 再补一次 0ms 兜底，覆盖主题/ADS 延迟恢复导致 currentIndex 稍后才变化的情况。
    QTimer::singleShot(0, this, [this]()
        {
            ensureTabInitialized(m_mainTabWidget != nullptr ? m_mainTabWidget->currentIndex() : -1);
        });
}

void MiscDock::showEvent(QShowEvent* event)
{
    const QPointer<MiscDock> owner(this);
    QWidget::showEvent(event);
    if (!owner || !m_mainTabWidget) return;
    if (std::exchange(m_initializeCurrentPageOnShow, false))
    {
        ensureTabInitialized(m_mainTabWidget->currentIndex());
        return;
    }
    // Qt 先显示当前子分支再分派父 Show；复用已建页只消费 dirty，不触发业务激活。
    const QPointer<QTabWidget> tabs(m_mainTabWidget);
    const QPointer<QWidget> page(tabs->currentWidget());
    if (!page || !page->isVisible()) return;
    ks::ui::PrepareWidgetThemeBindingsForShow(page.data());
    if (!owner || !tabs || !page || tabs->currentWidget() != page || !page->isVisible()) return;
    if (QLayout* layout = page->layout()) layout->activate(); // Qt 已激活的热页在此常数时间返回。
}

void MiscDock::ensureTabInitialized(const int tabIndex, const bool activatePage)
{
    if (tabIndex < 0)
    {
        return;
    }

    // 真正创建/挂载时才持有首帧守卫；复用页在业务激活后仅消费真实 dirty。
    const QPointer<QTabWidget> tabs(m_mainTabWidget); // 只准备本次已选中的真实分支。
    const auto prepareMountedPage = [tabs](const QPointer<QWidget>& page)
    {
        const auto current = [&page, &tabs]
        {
            return page && tabs && tabs->currentWidget() == page && page->isVisible();
        };
        if (!current()) return;
        // addWidget 默认把新子页 Show 排队；完整工厂返回后只显示明确单页壳的唯一正文。
        QLayout* const hostLayout = page->layout();
        const QPointer<QWidget> content(page->property("ksword_page_frame_host").toBool()
            && hostLayout && hostLayout->count() == 1 ? hostLayout->itemAt(0)->widget() : nullptr);
        if (content && content->parentWidget() == page && content->isHidden()
            && !content->testAttribute(Qt::WA_WState_ExplicitShowHide))
        {
            content->show(); // 保留真正业务 hide；隐藏 Tab/后台构造不进入此分支。
        }
        if (!current()) return;
        ks::ui::PrepareWidgetThemeBindingsForShow(page.data());
        if (!current()) return;
        // 构造/复用均收口同一个当前宿主；Qt 已激活的热页在此常数时间返回。
        if (QLayout* layout = page->layout()) layout->activate();
    };
    const auto ensurePage = [prepareMountedPage](QWidget* host, const bool needsConstruction, const auto& initialize)
    {
        if (needsConstruction)
        {
            const QPointer<QWidget> page(host); // 工厂、Show 和主题回调均允许销毁/换页。
            initialize();
            prepareMountedPage(page);
            return;
        }
        const QPointer<QWidget> page(host); // 激活回调可能关闭或重挂宿主。
        initialize();
        prepareMountedPage(page);
    };

    if (tabIndex == m_themeCheckTabIndex)
    {
        ensurePage(m_themeCheckHostWidget, m_themeCheckPage == nullptr, [this]() {
            if (!m_themeCheckPage)
            {
                auto* layout = buildHostLayout(m_themeCheckHostWidget);
                m_themeCheckPage = new ks::misc::ThemeCheckPage(m_themeCheckHostWidget);
                const QPointer<QWidget> host(m_themeCheckHostWidget);
                layout->addWidget(m_themeCheckPage);
                if (host) host->setProperty("ksword_search_body_page_id", QVariant());
            }
        });
        return;
    }

    if (tabIndex == m_bootEditorTabIndex)
    {
        ensurePage(m_bootEditorHostWidget, m_bootEditorTab == nullptr && m_bootEditorHostWidget != nullptr, [this]() {
            initializeBootEditorTab();
        });
        return;
    }
    if (tabIndex == m_soundSourceTabIndex)
    {
        ensurePage(m_soundSourceHostWidget, m_soundSourcePage == nullptr && m_soundSourceHostWidget != nullptr, [this]() {
            initializeSoundSourcePage();
        });
        return;
    }
    if (tabIndex == m_systemTimeTabIndex)
    {
        ensurePage(m_systemTimeHostWidget, m_systemTimePage == nullptr && m_systemTimeHostWidget != nullptr, [this]() {
            initializeSystemTimePage();
        });
        return;
    }
    if (tabIndex == m_virtualLocationTabIndex)
    {
        ensurePage(m_virtualLocationHostWidget, m_virtualLocationPage == nullptr && m_virtualLocationHostWidget != nullptr, [this]() {
            initializeVirtualLocationPage();
        });
        return;
    }
    if (tabIndex == m_bugcheckGuardTabIndex)
    {
        if (m_bugcheckDiagnosticsVisible)
        {
            ensurePage(m_bugcheckGuardHostWidget, m_bugcheckGuardPage == nullptr && m_bugcheckGuardHostWidget != nullptr, [this]() {
                initializeBugcheckGuardPage();
            });
        }
        return;
    }
    if (tabIndex == m_disableDseTabIndex)
    {
        ensurePage(m_disableDseHostWidget, m_disableDsePage == nullptr && m_disableDseHostWidget != nullptr, [this]() {
            initializeDisableDsePage();
        });
        return;
    }
    if (tabIndex == m_cksTabIndex)
    {
        ensurePage(m_cksHostWidget, m_cksPage == nullptr && m_cksHostWidget != nullptr, [this]() {
            initializeCksPage();
        });
        return;
    }
    if (tabIndex == m_contextMenuCleanerTabIndex)
    {
        ensurePage(m_contextMenuCleanerHostWidget, m_contextMenuCleanerTab == nullptr && m_contextMenuCleanerHostWidget != nullptr, [this]() {
            initializeContextMenuCleanerTab();
        });
        return;
    }
    if (tabIndex == m_diskEditorTabIndex)
    {
        ensurePage(m_diskEditorHostWidget, m_diskEditorTab == nullptr && m_diskEditorHostWidget != nullptr, [this]() {
            initializeDiskEditorTab();
        });
        return;
    }
    if (tabIndex == m_applicationControlTabIndex)
    {
        ensurePage(m_applicationControlHostWidget, m_applicationControlPage == nullptr && m_applicationControlHostWidget != nullptr, [this]() {
            initializeApplicationControlPage();
        });
        return;
    }
    if (tabIndex == m_clipboardGuardTabIndex)
    {
        ensurePage(m_clipboardGuardHostWidget,
            m_clipboardGuardPage == nullptr && m_clipboardGuardHostWidget != nullptr,
            [this, activatePage]() {
                initializeClipboardGuardPage();
                if (activatePage && m_clipboardGuardPage != nullptr)
                {
                    m_clipboardGuardPage->notifyPageActivated();
                }
            });
        return;
    }
    if (tabIndex == m_renderBenchmarkTabIndex)
    {
        ensurePage(m_renderBenchmarkHostWidget, m_renderBenchmarkPage == nullptr && m_renderBenchmarkHostWidget != nullptr, [this]() {
            initializeRenderBenchmarkPage();
        });
        return;
    }
    if (tabIndex == m_desktopDrawingTabIndex)
    {
        ensurePage(m_desktopDrawingHostWidget, m_desktopDrawingPage == nullptr && m_desktopDrawingHostWidget != nullptr, [this]() {
            initializeDesktopDrawingPage();
        });
        return;
    }
    if (tabIndex == m_windowInjectionTabIndex)
    {
        ensurePage(m_windowInjectionHostWidget, m_windowInjectionPage == nullptr && m_windowInjectionHostWidget != nullptr, [this]() {
            initializeWindowInjectionPage();
        });
        return;
    }
    if (tabIndex == m_hvciBypassTabIndex)
    {
        ensurePage(m_hvciBypassHostWidget, m_hvciBypassPage == nullptr && m_hvciBypassHostWidget != nullptr, [this]() {
            initializeHvciBypassPage();
        });
        return;
    }
    if (tabIndex == m_rxpfTabIndex)
    {
        ensurePage(m_rxpfHostWidget, m_rxpfPage == nullptr && m_rxpfHostWidget != nullptr, [this]() {
            initializeRxpfPage();
        });
        return;
    }
    if (tabIndex == m_scannerTabIndex)
    {
        ensurePage(m_scannerHostWidget, m_scannerPage == nullptr && m_scannerHostWidget != nullptr, [this]() {
            initializeScannerPage();
        });
        return;
    }
    if (tabIndex == m_minidumpTabIndex)
    {
        ensurePage(m_minidumpHostWidget, m_minidumpPage == nullptr && m_minidumpHostWidget != nullptr, [this]() {
            initializeMinidumpPage();
        });
        return;
    }
    if (tabIndex == m_pluginTabIndex)
    {
        ensurePage(m_pluginHostWidget, m_pluginPage == nullptr && m_pluginHostWidget != nullptr, [this]() {
            initializePluginPage();
        });
        return;
    }
}

void MiscDock::activateTabByIndex(const int tabIndex)
{
    if (m_mainTabWidget == nullptr || tabIndex < 0 || tabIndex >= m_mainTabWidget->count())
    {
        return;
    }

    // 先构造再切换：currentChanged 在目标已经是当前页时不会触发，
    // 直接 setCurrentIndex 可能让调用方拿到尚未初始化的占位控件。
    const QPointer<MiscDock> lifetime(this);
    const QPointer<QTabWidget> tabs(m_mainTabWidget);
    ensureTabInitialized(tabIndex);
    if (!lifetime || !tabs || lifetime->m_mainTabWidget != tabs) return;
    tabs->setCurrentIndex(tabIndex);
}

ks::ui::PageSearchPreparation MiscDock::prepareNavigationPages(const bool currentPageOnly,
    const bool singleStep, const bool useColdBodies, std::function<void()> resume)
{
    using Preparation = ks::ui::PageSearchPreparation;
    if (currentPageOnly)
    {
        if (m_mainTabWidget) { ensureTabInitialized(m_mainTabWidget->currentIndex(), false); }
        return Preparation::Complete;
    }
    if (m_navigationPagesPrepared || !m_mainTabWidget) { return Preparation::Complete; }
    if (m_navigationPagePreparing) { return Preparation::MorePages; }
    if (!useColdBodies && m_navigationSkippedColdBodies)
    {
        // A compatibility caller still requests actual widgets, even after a
        // full search has consumed their complete cold body values.
        m_navigationPrepareCursor = 0;
        m_navigationSkippedColdBodies = false;
    }

    const QPointer<MiscDock> owner(this);
    // Preserve the original hidden/disabled policy and never activate a page.
    while (m_navigationPrepareCursor < m_mainTabWidget->count())
    {
        m_navigationPagePreparing = true;
        try
        {
            if (useColdBodies && m_navigationPrepareCursor == m_windowInjectionTabIndex
                && !m_windowInjectionPage && !m_windowInjectionSearchSnapshot
                && !m_windowInjectionSearchFailed && resume)
            {
                m_windowInjectionSearchReady = std::move(resume);
                if (!m_windowInjectionSearchOperation)
                    m_windowInjectionSearchOperation = new ks::ui::AsyncOperation(this);
                if (!m_windowInjectionSearchOperation->isBusy())
                {
                    using Snapshot = std::shared_ptr<const ks::window_input::InjectionInitialSnapshot>;
                    const auto ticket = m_windowInjectionSearchOperation->submit<int, Snapshot>(0,
                        [](const int&, const ks::ui::AsyncOperationToken&) {
                            return std::make_shared<const ks::window_input::InjectionInitialSnapshot>(
                                ks::window_input::ReadInitialInjectionSnapshot());
                        },
                        [this](const Snapshot& snapshot, std::uint64_t) {
                            m_windowInjectionSearchSnapshot = snapshot;
                            auto ready = std::exchange(m_windowInjectionSearchReady, {});
                            if (ready) ready();
                            return true;
                        },
                        [this](std::exception_ptr) {
                            m_windowInjectionSearchFailed = true;
                            auto ready = std::exchange(m_windowInjectionSearchReady, {});
                            if (ready) ready();
                        });
                    if (ticket == 0)
                    {
                        m_windowInjectionSearchFailed = true;
                        m_windowInjectionSearchReady = {};
                        m_navigationPagePreparing = false;
                        return Preparation::MorePages;
                    }
                }
                m_navigationPagePreparing = false;
                return Preparation::Waiting;
            }
            if (useColdBodies && m_navigationPrepareCursor == m_desktopDrawingTabIndex
                && !m_desktopDrawingPage && !m_desktopDrawingSearchDisplays
                && !m_desktopDrawingSearchFailed && resume)
            {
                m_desktopDrawingSearchReady = std::move(resume);
                if (!m_desktopDrawingSearchOperation)
                    m_desktopDrawingSearchOperation = new ks::ui::AsyncOperation(this);
                if (!m_desktopDrawingSearchOperation->isBusy())
                {
                    using Displays = std::vector<ks::misc::desktop_drawing::Display>;
                    const auto ticket = m_desktopDrawingSearchOperation->submit<int, Displays>(0,
                        [](const int&, const ks::ui::AsyncOperationToken&) {
                            // Same read-only physical-pixel query as the original
                            // hidden constructor; no native filter or renderer.
                            return ks::misc::desktop_drawing::enumerateDisplays();
                        },
                        [this](const Displays& displays, std::uint64_t) {
                            m_desktopDrawingSearchDisplays = displays;
                            auto ready = std::exchange(m_desktopDrawingSearchReady, {});
                            if (ready) ready();
                            return true;
                        },
                        [this](std::exception_ptr) {
                            m_desktopDrawingSearchFailed = true;
                            auto ready = std::exchange(m_desktopDrawingSearchReady, {});
                            if (ready) ready();
                        });
                    if (ticket == 0)
                    {
                        m_desktopDrawingSearchFailed = true;
                        m_desktopDrawingSearchReady = {};
                        m_navigationPagePreparing = false;
                        return Preparation::MorePages;
                    }
                }
                m_navigationPagePreparing = false;
                return Preparation::Waiting;
            }
            if (useColdBodies && m_navigationPrepareCursor == m_disableDseTabIndex
                && !m_disableDsePage && !m_disableDseSearchReason && !m_disableDseSearchFailed && resume)
            {
                // The old hidden constructor reads this exact initial state.
                // Keep one value query per cold page; a new search replaces
                // only its ready continuation, never the native query.
                m_disableDseSearchReady = std::move(resume);
                if (!m_disableDseSearchOperation)
                    m_disableDseSearchOperation = new ks::ui::AsyncOperation(this);
                if (!m_disableDseSearchOperation->isBusy())
                {
                    const auto ticket = m_disableDseSearchOperation->submit<int, ks::misc::disable_dse::BlockReason>(0,
                        [](const int&, const ks::ui::AsyncOperationToken&) {
                            return ks::misc::disable_dse::evaluateBlockReason({}, {});
                        },
                        [this](const ks::misc::disable_dse::BlockReason reason, std::uint64_t) {
                            m_disableDseSearchReason = reason;
                            auto ready = std::exchange(m_disableDseSearchReady, {});
                            if (ready) ready();
                            return true;
                        },
                        [this](std::exception_ptr) {
                            // Fall back to the original factory on failure;
                            // a missing result is never an empty complete body.
                            m_disableDseSearchFailed = true;
                            auto ready = std::exchange(m_disableDseSearchReady, {});
                            if (ready) ready();
                        });
                    if (ticket == 0)
                    {
                        m_disableDseSearchFailed = true;
                        m_disableDseSearchReady = {};
                        m_navigationPagePreparing = false;
                        return Preparation::MorePages;
                    }
                }
                m_navigationPagePreparing = false;
                return Preparation::Waiting;
            }
            if (useColdBodies && ((m_navigationPrepareCursor == m_bootEditorTabIndex && !m_bootEditorTab)
                || (m_navigationPrepareCursor == m_minidumpTabIndex && !m_minidumpPage)
                || (m_navigationPrepareCursor == m_themeCheckTabIndex && !m_themeCheckPage)
                || (m_navigationPrepareCursor == m_systemTimeTabIndex && !m_systemTimePage)
                || (m_navigationPrepareCursor == m_virtualLocationTabIndex && !m_virtualLocationPage)
                || (m_navigationPrepareCursor == m_soundSourceTabIndex && !m_soundSourcePage)
                || (m_navigationPrepareCursor == m_bugcheckGuardTabIndex && !m_bugcheckGuardPage)
                || (m_navigationPrepareCursor == m_contextMenuCleanerTabIndex && !m_contextMenuCleanerTab)
                || (m_navigationPrepareCursor == m_windowInjectionTabIndex && !m_windowInjectionPage && m_windowInjectionSearchSnapshot)
                || (m_navigationPrepareCursor == m_desktopDrawingTabIndex && !m_desktopDrawingPage && m_desktopDrawingSearchDisplays)
                || (m_navigationPrepareCursor == m_clipboardGuardTabIndex && !m_clipboardGuardPage)))
                m_navigationSkippedColdBodies = true;
            else ensureTabInitialized(m_navigationPrepareCursor, false);
        }
        catch (...)
        {
            if (owner) { m_navigationPagePreparing = false; }
            throw;
        }
        if (!owner) { return Preparation::Complete; }
        m_navigationPagePreparing = false;
        ++m_navigationPrepareCursor;
        if (singleStep) { break; }
    }
    const bool complete = m_navigationPrepareCursor >= m_mainTabWidget->count();
    m_navigationPagesPrepared = complete && !m_navigationSkippedColdBodies;
    return complete ? Preparation::Complete : Preparation::MorePages;
}

MinidumpDock* MiscDock::activateMinidumpTab()
{
    const QPointer<MiscDock> lifetime(this); // 新正文 Show/原 currentChanged 均可关闭整个宿主。
    activateTabByIndex(m_minidumpTabIndex);
    return lifetime ? lifetime->m_minidumpPage : nullptr;
}

void MiscDock::setBugcheckDiagnosticsVisible(const bool visible)
{
    m_bugcheckDiagnosticsVisible = visible;
    if (m_mainTabWidget == nullptr || m_bugcheckGuardTabIndex < 0)
    {
        return;
    }

    // 隐藏页签不移除占位控件，索引保持稳定，重新显示后仍可复用已构造的页面状态。
    QTabBar* const tabBar = m_mainTabWidget->tabBar();
    if (tabBar != nullptr)
    {
        tabBar->setTabVisible(m_bugcheckGuardTabIndex, visible);
    }
    if (!visible && m_mainTabWidget->currentIndex() == m_bugcheckGuardTabIndex)
    {
        m_mainTabWidget->setCurrentIndex(m_bootEditorTabIndex);
    }
    if (visible && m_mainTabWidget->currentIndex() == m_bugcheckGuardTabIndex)
    {
        ensureTabInitialized(m_bugcheckGuardTabIndex);
    }
}

void MiscDock::initializeBootEditorTab()
{
    if (m_bootEditorHostWidget == nullptr || m_bootEditorTab != nullptr)
    {
        return;
    }

    QVBoxLayout* const hostLayout = buildHostLayout(m_bootEditorHostWidget);
    m_bootEditorTab = new BootEditorTab(m_bootEditorHostWidget);
    const QPointer<QWidget> host(m_bootEditorHostWidget);
    hostLayout->addWidget(m_bootEditorTab, 1);
    if (host) host->setProperty("ksword_search_body_page_id", QVariant());
}

void MiscDock::initializeSoundSourcePage()
{
    if (m_soundSourceHostWidget == nullptr || m_soundSourcePage != nullptr)
    {
        return;
    }

    QVBoxLayout* const hostLayout = buildHostLayout(m_soundSourceHostWidget);
    const QPointer<QWidget> host(m_soundSourceHostWidget);
    m_soundSourcePage = new ks::misc::SoundSourcePage(0U, 0U, m_soundSourceHostWidget);
    hostLayout->addWidget(m_soundSourcePage, 1);
    if (host) host->setProperty("ksword_search_body_page_id", QVariant());
}

void MiscDock::initializeSystemTimePage()
{
    if (m_systemTimeHostWidget == nullptr || m_systemTimePage != nullptr)
    {
        return;
    }

    QVBoxLayout* const hostLayout = buildHostLayout(m_systemTimeHostWidget);
    const QPointer<QWidget> host(m_systemTimeHostWidget);
    m_systemTimePage = new ks::misc::SystemTimePage(m_systemTimeHostWidget);
    hostLayout->addWidget(m_systemTimePage, 1);
    if (host) host->setProperty("ksword_search_body_page_id", QVariant());
}

void MiscDock::initializeVirtualLocationPage()
{
    if (m_virtualLocationHostWidget == nullptr || m_virtualLocationPage != nullptr)
    {
        return;
    }

    QVBoxLayout* const hostLayout = buildHostLayout(m_virtualLocationHostWidget);
    const QPointer<QWidget> host(m_virtualLocationHostWidget);
    m_virtualLocationPage = new ks::misc::VirtualLocationPage(m_virtualLocationHostWidget);
    hostLayout->addWidget(m_virtualLocationPage, 1);
    if (host) host->setProperty("ksword_search_body_page_id", QVariant());
}

void MiscDock::initializeBugcheckGuardPage()
{
    if (m_bugcheckGuardHostWidget == nullptr || m_bugcheckGuardPage != nullptr)
    {
        return;
    }

    QVBoxLayout* const hostLayout = buildHostLayout(m_bugcheckGuardHostWidget);
    const QPointer<QWidget> host(m_bugcheckGuardHostWidget);
    m_bugcheckGuardPage = new ks::misc::BugcheckGuardPage(m_bugcheckGuardHostWidget);
    hostLayout->addWidget(m_bugcheckGuardPage, 1);
    if (host) host->setProperty("ksword_search_body_page_id", QVariant());
}

void MiscDock::initializeDisableDsePage()
{
    if (m_disableDseHostWidget == nullptr || m_disableDsePage != nullptr)
    {
        return;
    }

    QVBoxLayout* const hostLayout = buildHostLayout(m_disableDseHostWidget);
    // Direct activation during the read uses the original constructor query;
    // cancel its pending publication so it cannot overwrite a live page.
    if (m_disableDseSearchOperation) m_disableDseSearchOperation->cancel();
    auto ready = std::exchange(m_disableDseSearchReady, {});
    const auto initialReason = m_disableDseSearchReason;
    m_disableDseSearchReason.reset();
    m_disableDsePage = new ks::misc::DisableDsePage(m_disableDseHostWidget, initialReason);
    hostLayout->addWidget(m_disableDsePage, 1);
    if (ready) ready();
}

void MiscDock::initializeCksPage()
{
    if (m_cksHostWidget == nullptr || m_cksPage != nullptr)
    {
        return;
    }

    QVBoxLayout* const hostLayout = buildHostLayout(m_cksHostWidget);
    m_cksPage = new ks::misc::CksPage(m_cksHostWidget);
    hostLayout->addWidget(m_cksPage, 1);
}

void MiscDock::initializeContextMenuCleanerTab()
{
    if (m_contextMenuCleanerHostWidget == nullptr || m_contextMenuCleanerTab != nullptr)
    {
        return;
    }

    QVBoxLayout* const hostLayout = buildHostLayout(m_contextMenuCleanerHostWidget);
    const QPointer<QWidget> host(m_contextMenuCleanerHostWidget);
    m_contextMenuCleanerTab = new ks::misc::ContextMenuCleanerTab(m_contextMenuCleanerHostWidget);
    hostLayout->addWidget(m_contextMenuCleanerTab, 1);
    if (host) host->setProperty("ksword_search_body_page_id", QVariant());
}

void MiscDock::initializeDiskEditorTab()
{
    if (m_diskEditorHostWidget == nullptr || m_diskEditorTab != nullptr)
    {
        return;
    }

    QVBoxLayout* const hostLayout = buildHostLayout(m_diskEditorHostWidget);
    m_diskEditorTab = new ks::misc::DiskEditorTab(m_diskEditorHostWidget);
    hostLayout->addWidget(m_diskEditorTab, 1);
}

void MiscDock::initializeApplicationControlPage()
{
    if (m_applicationControlHostWidget == nullptr || m_applicationControlPage != nullptr)
    {
        return;
    }

    QVBoxLayout* const hostLayout = buildHostLayout(m_applicationControlHostWidget);
    m_applicationControlPage = new ks::misc::ApplicationControlPage(m_applicationControlHostWidget);
    hostLayout->addWidget(m_applicationControlPage, 1);
}

void MiscDock::initializeClipboardGuardPage()
{
    if (m_clipboardGuardHostWidget == nullptr || m_clipboardGuardPage != nullptr)
    {
        return;
    }

    QVBoxLayout* const hostLayout = buildHostLayout(m_clipboardGuardHostWidget);
    const QPointer<QWidget> host(m_clipboardGuardHostWidget);
    m_clipboardGuardPage = new ks::misc::ClipboardGuardPage(m_clipboardGuardHostWidget);
    hostLayout->addWidget(m_clipboardGuardPage, 1);
    if (host) host->setProperty("ksword_search_body_page_id", QVariant());
}

void MiscDock::initializeRenderBenchmarkPage()
{
    if (m_renderBenchmarkHostWidget == nullptr || m_renderBenchmarkPage != nullptr)
    {
        return;
    }

    QVBoxLayout* const hostLayout = buildHostLayout(m_renderBenchmarkHostWidget);
    m_renderBenchmarkPage = new ks::misc::RenderBenchmarkPage(m_renderBenchmarkHostWidget);
    hostLayout->addWidget(m_renderBenchmarkPage, 1);
}

void MiscDock::initializeDesktopDrawingPage()
{
    if (m_desktopDrawingHostWidget == nullptr || m_desktopDrawingPage != nullptr)
    {
        return;
    }

    QVBoxLayout* const hostLayout = buildHostLayout(m_desktopDrawingHostWidget);
    if (m_desktopDrawingSearchOperation) m_desktopDrawingSearchOperation->cancel();
    auto ready = std::exchange(m_desktopDrawingSearchReady, {});
    m_desktopDrawingSearchDisplays.reset();
    const QPointer<QWidget> host(m_desktopDrawingHostWidget);
    // Real activation keeps its original fresh query, never an old search snapshot.
    m_desktopDrawingPage = new ks::misc::DesktopDrawingPage(m_desktopDrawingHostWidget);
    hostLayout->addWidget(m_desktopDrawingPage, 1);
    if (host) host->setProperty("ksword_search_body_page_id", QVariant());
    if (ready) ready();
}

void MiscDock::initializeScannerPage()
{
    if (m_scannerHostWidget == nullptr || m_scannerPage != nullptr)
    {
        return;
    }

    QVBoxLayout* const hostLayout = buildHostLayout(m_scannerHostWidget);
    m_scannerPage = new ScannerDock(m_scannerHostWidget);
    hostLayout->addWidget(m_scannerPage, 1);
}

void MiscDock::initializeWindowInjectionPage()
{
    if (!m_windowInjectionHostWidget || m_windowInjectionPage) return;
    auto* layout = buildHostLayout(m_windowInjectionHostWidget);
    if (m_windowInjectionSearchOperation) m_windowInjectionSearchOperation->cancel();
    auto ready = std::exchange(m_windowInjectionSearchReady, {});
    m_windowInjectionSearchSnapshot.reset();
    const QPointer<QWidget> host(m_windowInjectionHostWidget);
    // A real activation retains the original fresh list query and no cached
    // search identity is ever fed into an input/DWM operation.
    m_windowInjectionPage = ks::window_input::CreateInjectionPage(m_windowInjectionHostWidget);
    layout->addWidget(m_windowInjectionPage);
    if (host) host->setProperty("ksword_search_body_page_id", QVariant());
    if (ready) ready();
}

void MiscDock::initializeHvciBypassPage()
{
    if (m_hvciBypassHostWidget == nullptr || m_hvciBypassPage != nullptr)
    {
        return;
    }

    QVBoxLayout* const hostLayout = buildHostLayout(m_hvciBypassHostWidget);
    m_hvciBypassPage = new ks::misc::HvciBypassPage(m_hvciBypassHostWidget);
    hostLayout->addWidget(m_hvciBypassPage, 1);
}

void MiscDock::initializeRxpfPage()
{
    if (m_rxpfHostWidget == nullptr || m_rxpfPage != nullptr)
    {
        return;
    }

    QVBoxLayout* const hostLayout = buildHostLayout(m_rxpfHostWidget);
    m_rxpfPage = new ks::misc::RxpfPage(m_rxpfHostWidget);
    hostLayout->addWidget(m_rxpfPage, 1);
}

void MiscDock::initializeMinidumpPage()
{
    if (m_minidumpHostWidget == nullptr || m_minidumpPage != nullptr)
    {
        return;
    }

    QVBoxLayout* const hostLayout = buildHostLayout(m_minidumpHostWidget);
    m_minidumpPage = new MinidumpDock(m_minidumpHostWidget);
    const QPointer<QWidget> host(m_minidumpHostWidget);
    hostLayout->addWidget(m_minidumpPage, 1);
    if (host) host->setProperty("ksword_search_body_page_id", QVariant());
}

void MiscDock::initializePluginPage()
{
    if (m_pluginHostWidget == nullptr || m_pluginPage != nullptr)
    {
        return;
    }

    QVBoxLayout* const hostLayout = buildHostLayout(m_pluginHostWidget);
    // 插件原生子窗口仍由原宿主持有，只为其外围增加明确的无描边工作面。
    auto* pluginPanel = new QWidget(m_pluginHostWidget); // 外壳不接管插件内部 palette/QSS。
    auto* pluginLayout = new QVBoxLayout(pluginPanel);
    pluginLayout->setSpacing(12);
    ks::ui::StyleWorkspacePage(pluginPanel);
    ks::ui::StylePrimaryContentPanel(pluginPanel, false);
    m_pluginPage = ks::plugin_host::createTabPluginContainer(pluginPanel);
    pluginLayout->addWidget(m_pluginPage, 1);
    hostLayout->addWidget(pluginPanel, 1);
}

int MiscDock::addNavigationTab(const int index, QWidget* host, const QIcon& icon)
{
    const int tabIndex = m_mainTabWidget->count();
    m_navigationTabs.add(kMiscNavigation[index], m_mainTabWidget, host, icon);
    if (index == 1 || index == 2 || index == 3 || index == 10 || index == 12 || index == 17 || index == 19)
        host->setProperty("ksword_search_body_page_id", QString::fromLatin1(kMiscNavigation[index].id));
    return tabIndex;
}
QWidget* MiscDock::navigationHost(const int index) const
{
    switch (index)
    {
    case 0: return m_bootEditorHostWidget;
    case 1: return m_soundSourceHostWidget;
    case 2: return m_systemTimeHostWidget;
    case 3: return m_virtualLocationHostWidget;
    case 4: return m_bugcheckGuardHostWidget;
    case 5: return m_disableDseHostWidget;
    case 6: return m_cksHostWidget;
    case 7: return m_contextMenuCleanerHostWidget;
    case 8: return m_diskEditorHostWidget;
    case 9: return m_applicationControlHostWidget;
    case 10: return m_clipboardGuardHostWidget;
    case 11: return m_renderBenchmarkHostWidget;
    case 12: return m_desktopDrawingHostWidget;
    case 13: return m_windowInjectionHostWidget;
    case 14: return m_hvciBypassHostWidget;
    case 15: return m_rxpfHostWidget;
    case 16: return m_scannerHostWidget;
    case 17: return m_minidumpHostWidget;
    case 18: return m_pluginHostWidget;
    case 19: return m_themeCheckHostWidget;
    default: return nullptr;
    }
}
QWidget* MiscDock::navigationChild(const int index) const
{
    switch (index)
    {
    case 0: return m_bootEditorTab;
    case 1: return m_soundSourcePage;
    case 2: return m_systemTimePage;
    case 3: return m_virtualLocationPage;
    case 4: return m_bugcheckGuardPage;
    case 5: return m_disableDsePage;
    case 6: return m_cksPage;
    case 7: return m_contextMenuCleanerTab;
    case 8: return m_diskEditorTab;
    case 9: return m_applicationControlPage;
    case 10: return m_clipboardGuardPage;
    case 11: return m_renderBenchmarkPage;
    case 12: return m_desktopDrawingPage;
    case 13: return m_windowInjectionPage;
    case 14: return m_hvciBypassPage;
    case 15: return m_rxpfPage;
    case 16: return m_scannerPage;
    case 17: return m_minidumpPage;
    case 18: return m_pluginPage;
    case 19: return m_themeCheckPage;
    default: return nullptr;
    }
}
ks::ui::PageNavigationCatalog MiscDock::navigationCatalog(const MiscDock* existing,
    const bool includeColdSearchBody)
{
    ks::ui::PageNavigationCatalog main;
    for (const auto& definition : kMiscNavigation) main.append(ks::ui::PageNavigationTabDescriptor(definition));
    if (includeColdSearchBody && (!existing || !existing->m_bootEditorTab))
    {
        main[0].coldBodyTexts = BootEditorTab::coldSearchBody();
        main[0].coldBodyComplete = true;
    }
    if (includeColdSearchBody && (!existing || !existing->m_soundSourcePage))
    {
        main[1].coldBodyTexts = ks::misc::SoundSourcePage::coldSearchBody();
        main[1].coldBodyComplete = true;
    }
    if (includeColdSearchBody && (!existing || !existing->m_systemTimePage))
    {
        main[2].coldBodyTexts = ks::misc::SystemTimePage::coldSearchBody();
        main[2].coldBodyComplete = true;
    }
    if (includeColdSearchBody && (!existing || !existing->m_virtualLocationPage))
    {
        main[3].coldBodyTexts = ks::misc::VirtualLocationPage::coldSearchBody();
        main[3].coldBodyComplete = true;
    }
    if (includeColdSearchBody && (!existing || !existing->m_bugcheckGuardPage))
    {
        main[4].coldBodyTexts = ks::misc::BugcheckGuardPage::coldSearchBody();
        main[4].coldBodyComplete = true;
    }
    if (includeColdSearchBody && (!existing || !existing->m_contextMenuCleanerTab))
    {
        main[7].coldBodyTexts = ks::misc::ContextMenuCleanerTab::coldSearchBody();
        main[7].coldBodyComplete = true;
    }
    if (includeColdSearchBody && (!existing || !existing->m_clipboardGuardPage))
    {
        main[10].coldBodyTexts = ks::misc::ClipboardGuardPage::coldSearchBody();
        main[10].coldBodyComplete = true;
    }
    if (includeColdSearchBody && existing && !existing->m_desktopDrawingPage && existing->m_desktopDrawingSearchDisplays)
    {
        main[12].coldBodyTexts = ks::misc::DesktopDrawingPage::coldSearchBody(*existing->m_desktopDrawingSearchDisplays);
        main[12].coldBodyComplete = true;
    }
    if (includeColdSearchBody && existing && !existing->m_windowInjectionPage && existing->m_windowInjectionSearchSnapshot)
    {
        main[13].coldBodyTexts = ks::window_input::coldSearchBody(*existing->m_windowInjectionSearchSnapshot);
        main[13].coldBodyComplete = true;
    }
    if (includeColdSearchBody && (!existing || !existing->m_minidumpPage))
    {
        main[17].coldBodyTexts = MinidumpDock::coldSearchBody();
        main[17].coldBodyComplete = true;
    }
    if (includeColdSearchBody && (!existing || !existing->m_themeCheckPage))
    {
        main[19].coldBodyTexts = ks::misc::ThemeCheckPage::coldSearchBody();
        main[19].coldBodyComplete = true;
    }
    else if (existing && existing->m_themeCheckPage)
        main[19].searchTexts = ks::misc::ThemeCheckPage::navigationRootSearchTexts();
    if (existing) main = existing->m_navigationTabs.describe(std::move(main));
    else main[4].visible = false; // MainWindow以相同配置/会话状态修正尚未构造的入口。
    ks::ui::PageNavigationCatalog schema;
    for (int index = 0; index < static_cast<int>(std::size(kMiscNavigation)); ++index)
    {
        const auto& parent = main[index];
        schema.append(parent);
        if (!kMiscChildKeys[index]) continue;
        const auto children = ks::ui::RegisteredPageNavigationCatalog(QString::fromLatin1(kMiscChildKeys[index]),
            existing ? existing->navigationChild(index) : nullptr);
        if (!children) continue;
        const QString prefix = parent.id + QLatin1Char(':');
        for (auto child : *children)
        {
            child.id.prepend(prefix);
            child.parentId = child.parentId.isEmpty() ? parent.id : prefix + child.parentId;
            schema.append(std::move(child));
        }
    }
    return schema;
}
QWidget* MiscDock::activateNavigationDescriptor(const QString& id)
{
    const QPointer<MiscDock> self(this);
    const QString parentId = id.section(QLatin1Char(':'), 0, 0);
    int index = -1;
    for (int candidate = 0; candidate < static_cast<int>(std::size(kMiscNavigation)); ++candidate)
        if (parentId == QLatin1String(kMiscNavigation[candidate].id)) { index = candidate; break; }
    if (index < 0) return nullptr;
    const auto permitted = [self, &id](bool current) {
        if (!self) return false;
        const auto resolved = ks::ui::ResolvePageNavigationCatalog(navigationCatalog(self, false), QString());
        if (!resolved) return false;
        for (const auto& page : *resolved)
            if (page.page.id == id) return page.page.visible && page.page.enabled && (!current || page.page.current);
        return false;
    };
    if (!permitted(false)) return nullptr;
    const QPointer<QTabWidget> tabs(m_mainTabWidget);
    const QPointer<QWidget> host(navigationHost(index));
    ensureTabInitialized(tabs ? tabs->indexOf(host) : -1, false);
    if (!self || !tabs || !host || self->m_mainTabWidget != tabs || self->navigationHost(index) != host || !permitted(false)) return nullptr;
    const QPointer<QWidget> child(self->navigationChild(index));
    if (!self->m_navigationTabs.activate(parentId, navigationCatalog(self, false))) return nullptr;
    if (!self || !tabs || !host || self->m_mainTabWidget != tabs || self->navigationHost(index) != host
        || self->navigationChild(index) != child || tabs->currentWidget() != host || !permitted(false)) return nullptr;
    if (id == parentId) return permitted(true) ? host.data() : nullptr;
    if (!child || !kMiscChildKeys[index]) return nullptr;
    const QPointer<QWidget> target(ks::ui::NavigateRegisteredPageNavigationCatalog(
        QString::fromLatin1(kMiscChildKeys[index]), child, id.mid(parentId.size() + 1)));
    if (!self || !tabs || !host || !child || !target || self->m_mainTabWidget != tabs || self->navigationHost(index) != host
        || self->navigationChild(index) != child || tabs->currentWidget() != host || !permitted(true)) return nullptr;
    return target.data();
}
namespace { const bool kMiscNavigationRegistered =
    ks::ui::RegisterTypedPageNavigationCatalog<MiscDock>(QStringLiteral("misc")); }
