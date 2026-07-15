
#include "KernelDock.h"
#include "../UI/VisibleTableWidget.h"

// ============================================================
// KernelDock.cpp
// 作用说明：
// 1) 实现内核 Dock 的顶层页签 UI（对象命名空间 / 原子表 / SSDT / 历史 NtQuery / 驱动回调）；
// 2) 实现异步刷新、筛选、详情联动与右键菜单；
// 3) 具体底层枚举逻辑放在 Worker 文件，当前文件仅做界面和交互编排。
// ============================================================

#include "../UI/CodeEditorWidget.h"
#include "KernelBaseNamedObjectsTab.h"
#include "KernelCommunicationEndpointTab.h"
#include "KernelDockCidTab.h"
#include "KernelDockIpcTab.h"
#include "KernelDeviceDriverObjectsTab.h"
#include "KernelNamedPipeTab.h"
#include "KernelObjectDirectoryDeepTab.h"
#include "KernelObjectTypeMatrixTab.h"
#include "KernelSymbolicLinkTab.h"
#include "../SettingsDock/AppearanceSettings.h"
#include "../theme.h"

#include <QAbstractItemView>
#include <QAction>
#include <QApplication>
#include <QClipboard>
#include <QComboBox>
#include <QCoreApplication>
#include <QDir>
#include <QEventLoop>
#include <QFileInfo>
#include <QHeaderView>
#include <QHBoxLayout>
#include <QIcon>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QModelIndex>
#include <QPainter>
#include <QPixmap>
#include <QProgressBar>
#include <QPushButton>
#include <QSplitter>
#include <QShowEvent>
#include <QSvgRenderer>
#include <QTableWidget>
#include <QTabWidget>
#include <QTimer>
#include <QTreeWidget>
#include <QVBoxLayout>

using ksword::kernel_dock_internal::kernelText;

namespace
{
    // blueButtonStyle：
    // - 作用：统一图标按钮样式（带悬停与按下态）。
    QString blueButtonStyle()
    {
        return KswordTheme::ThemedButtonStyle();
    }

    // blueInputStyle：
    // - 作用：统一筛选输入框样式。
    QString blueInputStyle()
    {
        return QStringLiteral(
            "QLineEdit{border:1px solid %2;border-radius:2px;background:%3;color:%4;padding:2px 6px;}"
            "QLineEdit:focus{border:1px solid %1;}")
            .arg(KswordTheme::PrimaryBlueHex)
            .arg(KswordTheme::BorderHex())
            .arg(KswordTheme::SurfaceHex())
            .arg(KswordTheme::TextPrimaryHex());
    }

    // headerStyle：
    // - 作用：统一表头样式，强化列标题可读性。
    QString headerStyle()
    {
        return QStringLiteral(
            "QHeaderView::section{color:%1;background:%2;border:1px solid %3;font-weight:600;}")
            .arg(KswordTheme::PrimaryBlueHex)
            .arg(KswordTheme::SurfaceHex())
            .arg(KswordTheme::BorderHex());
    }

    // itemSelectionStyle：
    // - 作用：统一表格/树控件选中高亮为主题蓝，避免系统默认配色差异。
    QString itemSelectionStyle()
    {
        return QStringLiteral(
            "QTableWidget::item:selected{background:%1;color:palette(highlighted-text);}"
            "QTreeWidget::item:selected{background:%1;color:palette(highlighted-text);}")
            .arg(KswordTheme::PrimaryBlueHex);
    }

    // tableRowAsTsv：
    // - 输入 tableWidget 为只读表格，rowIndex 为当前可视行；
    // - 处理：按列顺序提取单元格文本，空值用 <空> 占位，字段使用 Tab 分隔；
    // - 返回：适合粘贴到文本编辑器或电子表格的 TSV；输入无效时返回空字符串。
    QString tableRowAsTsv(const QTableWidget* tableWidget, const int rowIndex)
    {
        if (tableWidget == nullptr || rowIndex < 0 || rowIndex >= tableWidget->rowCount())
        {
            return QString();
        }

        QStringList fieldList;
        fieldList.reserve(tableWidget->columnCount());
        for (int columnIndex = 0; columnIndex < tableWidget->columnCount(); ++columnIndex)
        {
            const QTableWidgetItem* cellItem = tableWidget->item(rowIndex, columnIndex);
            fieldList.push_back(cellItem != nullptr && !cellItem->text().trimmed().isEmpty()
                ? cellItem->text()
                : kernelText("kernel.main.placeholder.empty", QStringLiteral("<空>")));
        }
        return fieldList.join('\t');
    }

    // showCopyRowContextMenu：
    // - 输入 tableWidget 为目标表格，localPosition 为右键位置，selectClickedRow 控制是否同步当前行；
    // - 处理：显示显式主题样式菜单，并将当前/点击行复制为 TSV；
    // - 返回：无返回值；剪贴板不可用或行无效时静默跳过。
    void showCopyRowContextMenu(
        QTableWidget* tableWidget,
        const QPoint& localPosition,
        const bool selectClickedRow)
    {
        if (tableWidget == nullptr)
        {
            return;
        }

        const QModelIndex clickedIndex = tableWidget->indexAt(localPosition);
        int rowIndex = clickedIndex.isValid() ? clickedIndex.row() : tableWidget->currentRow();
        if (selectClickedRow && clickedIndex.isValid())
        {
            tableWidget->setCurrentCell(clickedIndex.row(), clickedIndex.column());
            rowIndex = clickedIndex.row();
        }

        QMenu contextMenu(tableWidget);
        contextMenu.setStyleSheet(KswordTheme::ContextMenuStyle());

        QAction* copyRowAction = contextMenu.addAction(
            QIcon(QStringLiteral(":/Icon/process_copy_row.svg")),
            kernelText("kernel.main.menu.copy_row", QStringLiteral("复制当前行")));
        copyRowAction->setEnabled(rowIndex >= 0);

        const QAction* selectedAction = contextMenu.exec(tableWidget->viewport()->mapToGlobal(localPosition));
        if (selectedAction != copyRowAction || rowIndex < 0)
        {
            return;
        }

        QClipboard* clipboard = QApplication::clipboard();
        const QString rowText = tableRowAsTsv(tableWidget, rowIndex);
        if (clipboard != nullptr && !rowText.isEmpty())
        {
            clipboard->setText(rowText);
        }
    }

    // kernelDockBackgroundImageReady 作用：
    // - 输入 rawImagePath：外观设置中的背景图路径，可为绝对路径或相对 exe 目录路径；
    // - 处理：只做轻量存在性/文件性检查，避免 KernelDock 根控件在背景图模式下强行刷实底；
    // - 返回：背景图可用返回 true，否则返回 false。
    bool kernelDockBackgroundImageReady(const QString& rawImagePath)
    {
        const QString trimmedPath = rawImagePath.trimmed();
        if (trimmedPath.isEmpty())
        {
            return false;
        }

        const QString resolvedPath = QDir::isAbsolutePath(trimmedPath)
            ? QDir::cleanPath(trimmedPath)
            : QDir(QCoreApplication::applicationDirPath()).absoluteFilePath(trimmedPath);
        const QFileInfo imageFileInfo(QDir::cleanPath(resolvedPath));
        return imageFileInfo.exists() && imageFileInfo.isFile();
    }

    // kernelDockAllowWallpaperThroughRoot 作用：
    // - 输入：无，直接读取当前外观设置；
    // - 处理：判断全局背景图是否可用；
    // - 返回：true 表示 KernelDock 根/页容器应透明，false 表示保持主题实底以规避 ADS 黑底。
    bool kernelDockAllowWallpaperThroughRoot()
    {
        const ks::settings::AppearanceSettings settings = ks::settings::loadAppearanceSettings();
        return kernelDockBackgroundImageReady(settings.backgroundImagePath);
    }

    // statusLabelStyle：
    // - 作用：统一状态标签的颜色与字重。
    QString statusLabelStyle(const QString& colorHex)
    {
        return QStringLiteral("color:%1;font-weight:600;").arg(colorHex);
    }

    // ObjectNamespaceColumn：对象命名空间树列索引。
    enum class ObjectNamespaceColumn : int
    {
        Name = 0,
        Type,
        PathOrScope,
        Status,
        SymbolicTarget,
        Count
    };

    // AtomColumn：原子表列索引。
    enum class AtomColumn : int
    {
        Value = 0,
        Hex,
        Name,
        Source,
        Status,
        Count
    };

    // NtQueryColumn：历史 NtQuery 表列索引。
    enum class NtQueryColumn : int
    {
        Category = 0,
        Function,
        QueryItem,
        Status,
        Summary,
        Count
    };

    // tintedSvgIcon：
    // - 作用：把资源 SVG 渲染为指定颜色图标，用于 Tab 选中态高对比显示。
    // - 参数 iconPath：资源路径；参数 tintColor：目标颜色；参数 iconSize：输出尺寸。
    QIcon tintedSvgIcon(const QString& iconPath, const QColor& tintColor, const QSize& iconSize = QSize(16, 16))
    {
        QSvgRenderer svgRenderer(iconPath);
        if (!svgRenderer.isValid())
        {
            return QIcon(iconPath);
        }

        QPixmap tintedPixmap(iconSize);
        tintedPixmap.fill(Qt::transparent);

        QPainter painter(&tintedPixmap);
        painter.setRenderHint(QPainter::Antialiasing, true);
        svgRenderer.render(&painter, QRectF(0, 0, iconSize.width(), iconSize.height()));
        painter.setCompositionMode(QPainter::CompositionMode_SourceIn);
        painter.fillRect(tintedPixmap.rect(), tintColor);
        painter.end();

        return QIcon(tintedPixmap);
    }

    // tabIcon：
    // - 作用：返回普通 Tab 图标，保持未选中态与项目图标资源一致。
    // - 变量 iconPath：图标资源路径，统一放在调用点便于审阅。
    QIcon tabIcon(const QString& iconPath)
    {
        return tintedSvgIcon(iconPath, KswordTheme::PrimaryBlueColor);
    }

    // selectedTabIcon：
    // - 作用：返回白色 Tab 图标，避免选中蓝底时出现蓝底蓝图标。
    QIcon selectedTabIcon(const QString& iconPath)
    {
        return tintedSvgIcon(iconPath, KswordTheme::OnAccentColor());
    }

}

KernelDock::KernelDock(QWidget* parent)
    : QWidget(parent)
{
    kLogEvent initEvent;
    info << initEvent << "[KernelDock] 构造开始，准备初始化五页内核视图。" << eol;

    initializeUi();
    initializeConnections();

    // 首屏页必须同步初始化：
    // - KernelDock 常被 ADS 恢复为当前 Dock，此时 show/currentChanged 时序可能不会再触发内部页初始化；
    // - 只初始化当前页，不会全量构建其它重页面，能彻底避免“内核 Dock 黑屏/无 UI”。
    ensureTabInitialized(m_tabWidget != nullptr ? m_tabWidget->currentIndex() : -1);

    // 再保留一次 0ms 兜底，覆盖主题/ADS 延迟恢复导致 currentIndex 稍后改变的情况。
    QTimer::singleShot(0, this, [this]() {
        ensureTabInitialized(m_tabWidget != nullptr ? m_tabWidget->currentIndex() : -1);
    });

    info << initEvent << "[KernelDock] 构造完成。" << eol;
}

void KernelDock::showEvent(QShowEvent* event)
{
    QWidget::showEvent(event);

    // ADS 恢复布局后，KernelDock 的内部 QTabWidget 可能已经显示但首页尚未初始化。
    // showEvent 里做幂等兜底，保证当前内部页至少有真实 UI 内容。
    ensureCurrentTabReadyForDisplay();
    QTimer::singleShot(0, this, [this]() {
        ensureCurrentTabReadyForDisplay();
    });
}

void KernelDock::ensureCurrentTabReadyForDisplay()
{
    if (m_tabWidget == nullptr)
    {
        return;
    }

    // ADS restoreState 会恢复外层 Dock 激活状态，但不一定会再次触发内部 QTabWidget
    // currentChanged。这里由 MainWindow/showEvent 主动调用，确保当前页已有真实子控件。
    ensureTabInitialized(m_tabWidget->currentIndex());
    m_tabWidget->updateGeometry();
    m_tabWidget->update();
    updateGeometry();
    update();
}

QString KernelDock::displayStateSummary() const
{
    if (m_tabWidget == nullptr)
    {
        return QStringLiteral("tabWidget=null");
    }

    QWidget* currentPage = m_tabWidget->currentWidget();
    const QSize tabSize = m_tabWidget->size();
    const QSize pageSize = currentPage != nullptr ? currentPage->size() : QSize();
    const int pageChildCount = currentPage != nullptr
        ? currentPage->findChildren<QWidget*>(QString(), Qt::FindDirectChildrenOnly).size()
        : 0;
    const int innerTabCount = m_objectNamespaceInnerTabWidget != nullptr
        ? m_objectNamespaceInnerTabWidget->count()
        : -1;

    // 返回值保持单行，便于日志面板和文件日志直接 grep KernelDockRepair。
    return QStringLiteral(
        "tabCount=%1,current=%2,tabSize=%3x%4,page=%5,pageVisible=%6,pageSize=%7x%8,"
        "pageChildren=%9,objectNsReady=%10,innerTabs=%11")
        .arg(m_tabWidget->count())
        .arg(m_tabWidget->currentIndex())
        .arg(tabSize.width())
        .arg(tabSize.height())
        .arg(currentPage != nullptr ? currentPage->objectName() : QStringLiteral("<null>"))
        .arg(currentPage != nullptr && currentPage->isVisible() ? QStringLiteral("true") : QStringLiteral("false"))
        .arg(pageSize.width())
        .arg(pageSize.height())
        .arg(pageChildCount)
        .arg(m_objectNamespaceTabInitialized ? QStringLiteral("true") : QStringLiteral("false"))
        .arg(innerTabCount);
}

void KernelDock::initializeUi()
{
    setObjectName(QStringLiteral("KernelDockRoot"));
    const bool allowWallpaperThroughRoot = kernelDockAllowWallpaperThroughRoot();
    setAutoFillBackground(!allowWallpaperThroughRoot);
    setAttribute(Qt::WA_StyledBackground, !allowWallpaperThroughRoot);

    // rootContainerStyle 作用：
    // - 无背景图时使用主题实底，继续规避 ADS 恢复布局后的黑色父容器；
    // - 有背景图时只让根/页容器透明，数据视图仍保留实底，避免文字压在图片上不可读。
    const QString rootContainerStyle = allowWallpaperThroughRoot
        ? QStringLiteral(
            "KernelDock#KernelDockRoot{background:transparent !important;color:%1 !important;}"
            "KernelDock#KernelDockRoot QTabWidget::pane{background:transparent !important;border:1px solid %2;}"
            "KernelDock#KernelDockRoot QStackedWidget,"
            "KernelDock#KernelDockRoot QStackedWidget > QWidget{background:transparent !important;color:%1 !important;}")
            .arg(KswordTheme::TextPrimaryHex())
            .arg(KswordTheme::BorderHex())
        : QStringLiteral(
            "KernelDock#KernelDockRoot{background:%1 !important;color:%2 !important;}"
            "KernelDock#KernelDockRoot QTabWidget::pane{background:%1 !important;border:1px solid %3;}"
            "KernelDock#KernelDockRoot QStackedWidget,"
            "KernelDock#KernelDockRoot QStackedWidget > QWidget{background:%1 !important;color:%2 !important;}")
            .arg(KswordTheme::SurfaceHex())
            .arg(KswordTheme::TextPrimaryHex())
            .arg(KswordTheme::BorderHex());

    // contentViewStyle 作用：
    // - 背景图模式下表格/树/列表透明，避免内核 Dock 内部形成大块遮罩；
    // - 无背景图时保持主题实底。
    const QString contentViewStyle = allowWallpaperThroughRoot
        ? QStringLiteral(
            "KernelDock#KernelDockRoot QTableView,"
            "KernelDock#KernelDockRoot QTableWidget,"
            "KernelDock#KernelDockRoot QTreeView,"
            "KernelDock#KernelDockRoot QTreeWidget,"
            "KernelDock#KernelDockRoot QListView,"
            "KernelDock#KernelDockRoot QListWidget,"
            "KernelDock#KernelDockRoot QAbstractScrollArea,"
            "KernelDock#KernelDockRoot QAbstractScrollArea > QWidget,"
            "KernelDock#KernelDockRoot QAbstractScrollArea::viewport{"
            "  background:transparent !important;"
            "  background-color:transparent !important;"
            "  alternate-background-color:transparent !important;"
            "  color:%1 !important;"
            "}")
            .arg(KswordTheme::TextPrimaryHex())
        : QStringLiteral(
            "KernelDock#KernelDockRoot QTableView,"
            "KernelDock#KernelDockRoot QTableWidget,"
            "KernelDock#KernelDockRoot QTreeView,"
            "KernelDock#KernelDockRoot QTreeWidget,"
            "KernelDock#KernelDockRoot QListView,"
            "KernelDock#KernelDockRoot QListWidget{"
            "  background:%1 !important;"
            "  alternate-background-color:%3 !important;"
            "  color:%2 !important;"
            "}")
            .arg(KswordTheme::SurfaceHex())
            .arg(KswordTheme::TextPrimaryHex())
            .arg(KswordTheme::SurfaceAltHex());

    setStyleSheet(rootContainerStyle + contentViewStyle);

    m_rootLayout = new QVBoxLayout(this);
    m_rootLayout->setContentsMargins(6, 6, 6, 6);
    m_rootLayout->setSpacing(4);

    // 初始化进度条默认隐藏，仅在惰性 Tab 开始构建时短暂显示。
    m_tabInitializingStatusLabel = new QLabel(kernelText("kernel.main.status.ready", QStringLiteral("页面就绪")), this);
    m_tabInitializingStatusLabel->setStyleSheet(statusLabelStyle(KswordTheme::TextSecondaryHex()));
    m_tabInitializingStatusLabel->setVisible(false);

    m_tabInitializingProgressBar = new QProgressBar(this);
    m_tabInitializingProgressBar->setRange(0, 0);
    m_tabInitializingProgressBar->setFixedHeight(4);
    m_tabInitializingProgressBar->setTextVisible(false);
    m_tabInitializingProgressBar->setVisible(false);
    m_tabInitializingProgressBar->setStyleSheet(QStringLiteral(
        "QProgressBar{border:none;background:%1;border-radius:1px;}"
        "QProgressBar::chunk{background:%2;border-radius:1px;}")
        .arg(KswordTheme::BorderHex())
        .arg(KswordTheme::PrimaryBlueHex));
    m_rootLayout->addWidget(m_tabInitializingStatusLabel, 0);
    m_rootLayout->addWidget(m_tabInitializingProgressBar, 0);

    m_tabWidget = new QTabWidget(this);
    m_tabWidget->setIconSize(QSize(16, 16));
    m_rootLayout->addWidget(m_tabWidget, 1);

    m_objectNamespacePage = new QWidget(m_tabWidget);
    m_atomPage = new QWidget(m_tabWidget);
    m_ssdtPage = new QWidget(m_tabWidget);
    m_dynDataPage = new QWidget(m_tabWidget);
    m_driverStatusPage = new QWidget(m_tabWidget);
    m_ntQueryPage = new QWidget(m_tabWidget);
    m_callbackInterceptPage = new QWidget(m_tabWidget);
    m_callbackEnumPage = new QWidget(m_tabWidget);
    m_shadowSsdtPage = new QWidget(m_tabWidget);
    m_inlineHookPage = new QWidget(m_tabWidget);
    m_iatEatHookPage = new QWidget(m_tabWidget);
    m_crossViewPage = new QWidget(m_tabWidget);
    m_ipcPage = new QWidget(m_tabWidget);

    m_objectNamespaceTabIndex = m_tabWidget->addTab(
        m_objectNamespacePage,
        tabIcon(QStringLiteral(":/Icon/process_tree.svg")),
        kernelText("kernel.main.tab.object_namespace.title", QStringLiteral("对象命名空间")));
    m_tabWidget->setTabToolTip(m_objectNamespaceTabIndex, kernelText("kernel.main.tab.object_namespace.tooltip", QStringLiteral("对象管理器命名空间遍历（默认页）")));

    m_atomTabIndex = m_tabWidget->addTab(
        m_atomPage,
        tabIcon(QStringLiteral(":/Icon/process_threads.svg")),
        kernelText("kernel.main.tab.atom.title", QStringLiteral("原子表遍历")));
    m_tabWidget->setTabToolTip(m_atomTabIndex, kernelText("kernel.main.tab.atom.tooltip", QStringLiteral("遍历全局原子范围并提供校验操作")));

    m_ntQueryTabIndex = m_tabWidget->addTab(
        m_ntQueryPage,
        tabIcon(QStringLiteral(":/Icon/process_details.svg")),
        kernelText("kernel.main.tab.nt_query.title", QStringLiteral("历史NtQuery")));
    m_tabWidget->setTabToolTip(m_ntQueryTabIndex, kernelText("kernel.main.tab.nt_query.tooltip", QStringLiteral("旧版内核 NtQuery 信息页")));

    m_ssdtTabIndex = m_tabWidget->addTab(
        m_ssdtPage,
        tabIcon(QStringLiteral(":/Icon/process_list.svg")),
        QStringLiteral("SSDT"));
    m_tabWidget->setTabToolTip(m_ssdtTabIndex, kernelText("kernel.main.tab.ssdt.tooltip", QStringLiteral("驱动侧 SSDT 服务索引遍历结果")));

    m_shadowSsdtTabIndex = m_tabWidget->addTab(
        m_shadowSsdtPage,
        tabIcon(QStringLiteral(":/Icon/process_list.svg")),
        QStringLiteral("SSSDT"));
    m_tabWidget->setTabToolTip(m_shadowSsdtTabIndex, kernelText("kernel.main.tab.shadow_ssdt.tooltip", QStringLiteral("参考 System Informer 的 win32k/win32u shadow syscall 解析")));

    m_inlineHookTabIndex = m_tabWidget->addTab(
        m_inlineHookPage,
        tabIcon(QStringLiteral(":/Icon/process_critical.svg")),
        QStringLiteral("Inline Hook"));
    m_tabWidget->setTabToolTip(m_inlineHookTabIndex, kernelText("kernel.main.tab.inline_hook.tooltip", QStringLiteral("扫描内核模块导出函数头部跳转补丁，并提供 force 后 NOP 摘除")));

    m_iatEatHookTabIndex = m_tabWidget->addTab(
        m_iatEatHookPage,
        tabIcon(QStringLiteral(":/Icon/process_details.svg")),
        QStringLiteral("IAT/EAT"));
    m_tabWidget->setTabToolTip(m_iatEatHookTabIndex, kernelText("kernel.main.tab.iat_eat.tooltip", QStringLiteral("检测内核模块导入表和导出表可疑目标指针")));

    m_crossViewTabIndex = m_tabWidget->addTab(
        m_crossViewPage,
        tabIcon(QStringLiteral(":/Icon/process_list.svg")),
        kernelText("kernel.main.tab.cid.title", QStringLiteral("CID表")));
    m_tabWidget->setTabToolTip(m_crossViewTabIndex, kernelText("kernel.main.tab.cid.tooltip", QStringLiteral("只读 CID / cross-view 证据聚合")));

    m_ipcTabIndex = m_tabWidget->addTab(
        m_ipcPage,
        tabIcon(QStringLiteral(":/Icon/process_details.svg")),
        QStringLiteral("IPC"));
    m_tabWidget->setTabToolTip(m_ipcTabIndex, kernelText("kernel.main.tab.ipc.tooltip", QStringLiteral("只读 NamedPipe / ALPC / 通信对象")));

    m_dynDataTabIndex = m_tabWidget->addTab(
        m_dynDataPage,
        tabIcon(QStringLiteral(":/Icon/process_priority.svg")),
        kernelText("kernel.main.tab.dyn_data.title", QStringLiteral("动态偏移")));
    m_tabWidget->setTabToolTip(m_dynDataTabIndex, kernelText("kernel.main.tab.dyn_data.tooltip", QStringLiteral("System Informer DynData 精确匹配状态与字段列表")));

    m_driverStatusTabIndex = m_tabWidget->addTab(
        m_driverStatusPage,
        tabIcon(QStringLiteral(":/Icon/process_details.svg")),
        kernelText("kernel.main.tab.driver_status.title", QStringLiteral("驱动状态")));
    m_tabWidget->setTabToolTip(m_driverStatusTabIndex, kernelText("kernel.main.tab.driver_status.tooltip", QStringLiteral("KswordARK 驱动加载、协议、安全策略、DynData 和功能能力矩阵")));

    m_callbackTabIndex = m_tabWidget->addTab(
        m_callbackInterceptPage,
        tabIcon(QStringLiteral(":/Icon/process_critical.svg")),
        kernelText("kernel.main.tab.callback.title", QStringLiteral("驱动回调")));
    m_tabWidget->setTabToolTip(m_callbackTabIndex, kernelText("kernel.main.tab.callback.tooltip", QStringLiteral("驱动回调拦截规则管理与询问事件处理")));

    m_callbackEnumTabIndex = m_tabWidget->addTab(
        m_callbackEnumPage,
        tabIcon(QStringLiteral(":/Icon/process_list.svg")),
        kernelText("kernel.main.tab.callback_enum.title", QStringLiteral("回调遍历")));
    m_tabWidget->setTabToolTip(m_callbackEnumTabIndex, kernelText("kernel.main.tab.callback_enum.tooltip", QStringLiteral("遍历 KswordARK 可见的系统回调、minifilter 和 System Informer DynData 诊断项")));

    m_tabWidget->setCurrentIndex(m_objectNamespaceTabIndex);
    updateTabIconContrast();
}

void KernelDock::showTabInitializingProgress(const int tabIndex, const QString& titleText)
{
    if (m_tabInitializingProgressBar == nullptr || m_tabInitializingStatusLabel == nullptr)
    {
        return;
    }

    // 只在当前正要显示的页签上展示进度，避免后台页初始化干扰用户视线。
    if (m_tabWidget != nullptr && m_tabWidget->currentIndex() != tabIndex)
    {
        return;
    }

    m_tabInitializingStatusLabel->setText(kernelText("kernel.main.status.initializing", QStringLiteral("正在初始化 %1 页面...")).arg(titleText));
    m_tabInitializingStatusLabel->setVisible(true);
    m_tabInitializingProgressBar->setVisible(true);
    QApplication::processEvents(QEventLoop::ExcludeUserInputEvents);
}

void KernelDock::hideTabInitializingProgress()
{
    if (m_tabInitializingProgressBar != nullptr)
    {
        m_tabInitializingProgressBar->setVisible(false);
    }
    if (m_tabInitializingStatusLabel != nullptr)
    {
        m_tabInitializingStatusLabel->setVisible(false);
    }
}

void KernelDock::updateTabIconContrast()
{
    if (m_tabWidget == nullptr)
    {
        return;
    }

    // Tab 选中态为蓝色背景时，图标改用白色资源绘制；未选中保持原图标颜色。
    const int currentIndex = m_tabWidget->currentIndex();
    m_tabWidget->setTabIcon(m_objectNamespaceTabIndex, tabIcon(QStringLiteral(":/Icon/process_tree.svg")));
    m_tabWidget->setTabIcon(m_atomTabIndex, tabIcon(QStringLiteral(":/Icon/process_threads.svg")));
    m_tabWidget->setTabIcon(m_ntQueryTabIndex, tabIcon(QStringLiteral(":/Icon/process_details.svg")));
    m_tabWidget->setTabIcon(m_ssdtTabIndex, tabIcon(QStringLiteral(":/Icon/process_list.svg")));
    m_tabWidget->setTabIcon(m_shadowSsdtTabIndex, tabIcon(QStringLiteral(":/Icon/process_list.svg")));
    m_tabWidget->setTabIcon(m_inlineHookTabIndex, tabIcon(QStringLiteral(":/Icon/process_critical.svg")));
    m_tabWidget->setTabIcon(m_iatEatHookTabIndex, tabIcon(QStringLiteral(":/Icon/process_details.svg")));
    m_tabWidget->setTabIcon(m_crossViewTabIndex, tabIcon(QStringLiteral(":/Icon/process_list.svg")));
    m_tabWidget->setTabIcon(m_ipcTabIndex, tabIcon(QStringLiteral(":/Icon/process_details.svg")));
    m_tabWidget->setTabIcon(m_dynDataTabIndex, tabIcon(QStringLiteral(":/Icon/process_priority.svg")));
    m_tabWidget->setTabIcon(m_driverStatusTabIndex, tabIcon(QStringLiteral(":/Icon/process_details.svg")));
    m_tabWidget->setTabIcon(m_callbackTabIndex, tabIcon(QStringLiteral(":/Icon/process_critical.svg")));
    m_tabWidget->setTabIcon(m_callbackEnumTabIndex, tabIcon(QStringLiteral(":/Icon/process_list.svg")));

    if (currentIndex == m_objectNamespaceTabIndex)
    {
        m_tabWidget->setTabIcon(currentIndex, selectedTabIcon(QStringLiteral(":/Icon/process_tree.svg")));
    }
    else if (currentIndex == m_atomTabIndex)
    {
        m_tabWidget->setTabIcon(currentIndex, selectedTabIcon(QStringLiteral(":/Icon/process_threads.svg")));
    }
    else if (currentIndex == m_ntQueryTabIndex)
    {
        m_tabWidget->setTabIcon(currentIndex, selectedTabIcon(QStringLiteral(":/Icon/process_details.svg")));
    }
    else if (currentIndex == m_ssdtTabIndex)
    {
        m_tabWidget->setTabIcon(currentIndex, selectedTabIcon(QStringLiteral(":/Icon/process_list.svg")));
    }
    else if (currentIndex == m_shadowSsdtTabIndex)
    {
        m_tabWidget->setTabIcon(currentIndex, selectedTabIcon(QStringLiteral(":/Icon/process_list.svg")));
    }
    else if (currentIndex == m_inlineHookTabIndex)
    {
        m_tabWidget->setTabIcon(currentIndex, selectedTabIcon(QStringLiteral(":/Icon/process_critical.svg")));
    }
    else if (currentIndex == m_iatEatHookTabIndex)
    {
        m_tabWidget->setTabIcon(currentIndex, selectedTabIcon(QStringLiteral(":/Icon/process_details.svg")));
    }
    else if (currentIndex == m_crossViewTabIndex)
    {
        m_tabWidget->setTabIcon(currentIndex, selectedTabIcon(QStringLiteral(":/Icon/process_list.svg")));
    }
    else if (currentIndex == m_ipcTabIndex)
    {
        m_tabWidget->setTabIcon(currentIndex, selectedTabIcon(QStringLiteral(":/Icon/process_details.svg")));
    }
    else if (currentIndex == m_dynDataTabIndex)
    {
        m_tabWidget->setTabIcon(currentIndex, selectedTabIcon(QStringLiteral(":/Icon/process_priority.svg")));
    }
    else if (currentIndex == m_driverStatusTabIndex)
    {
        m_tabWidget->setTabIcon(currentIndex, selectedTabIcon(QStringLiteral(":/Icon/process_details.svg")));
    }
    else if (currentIndex == m_callbackTabIndex)
    {
        m_tabWidget->setTabIcon(currentIndex, selectedTabIcon(QStringLiteral(":/Icon/process_critical.svg")));
    }
    else if (currentIndex == m_callbackEnumTabIndex)
    {
        m_tabWidget->setTabIcon(currentIndex, selectedTabIcon(QStringLiteral(":/Icon/process_list.svg")));
    }
}

void KernelDock::initializeObjectNamespaceTab()
{
    if (m_objectNamespacePage == nullptr || m_objectNamespaceLayout != nullptr)
    {
        return;
    }

    m_objectNamespaceLayout = new QVBoxLayout(m_objectNamespacePage);
    m_objectNamespaceLayout->setContentsMargins(4, 4, 4, 4);
    m_objectNamespaceLayout->setSpacing(6);

    m_objectNamespaceInnerTabWidget = new QTabWidget(m_objectNamespacePage);
    m_objectNamespaceInnerTabWidget->setIconSize(QSize(16, 16));
    m_objectNamespaceLayout->addWidget(m_objectNamespaceInnerTabWidget, 1);

    m_objectNamespaceOverviewPage = new QWidget(m_objectNamespaceInnerTabWidget);
    m_objectNamespaceOverviewLayout = new QVBoxLayout(m_objectNamespaceOverviewPage);
    m_objectNamespaceOverviewLayout->setContentsMargins(4, 4, 4, 4);
    m_objectNamespaceOverviewLayout->setSpacing(6);

    m_objectNamespaceInnerTabWidget->addTab(
        m_objectNamespaceOverviewPage,
        tabIcon(QStringLiteral(":/Icon/process_tree.svg")),
        kernelText("kernel.main.inner_tab.overview", QStringLiteral("总览")));
    m_objectNamespaceInnerTabWidget->addTab(
        new KernelObjectDirectoryDeepTab(m_objectNamespaceInnerTabWidget),
        tabIcon(QStringLiteral(":/Icon/process_tree.svg")),
        kernelText("kernel.main.inner_tab.directory_recursion", QStringLiteral("目录递归")));
    m_objectNamespaceInnerTabWidget->addTab(
        new KernelNamedPipeTab(m_objectNamespaceInnerTabWidget),
        tabIcon(QStringLiteral(":/Icon/process_details.svg")),
        kernelText("kernel.main.inner_tab.named_pipe", QStringLiteral("命名管道")));
    m_objectNamespaceInnerTabWidget->addTab(
        new KernelBaseNamedObjectsTab(m_objectNamespaceInnerTabWidget),
        tabIcon(QStringLiteral(":/Icon/process_threads.svg")),
        QStringLiteral("BaseNamedObjects"));
    m_objectNamespaceInnerTabWidget->addTab(
        new KernelSymbolicLinkTab(m_objectNamespaceInnerTabWidget),
        tabIcon(QStringLiteral(":/Icon/process_refresh.svg")),
        kernelText("kernel.main.inner_tab.symbolic_link", QStringLiteral("符号链接")));
    m_objectNamespaceInnerTabWidget->addTab(
        new KernelDeviceDriverObjectsTab(m_objectNamespaceInnerTabWidget),
        tabIcon(QStringLiteral(":/Icon/process_details.svg")),
        kernelText("kernel.main.inner_tab.device_driver", QStringLiteral("设备与驱动")));
    m_objectNamespaceInnerTabWidget->addTab(
        new KernelObjectTypeMatrixTab(m_objectNamespaceInnerTabWidget),
        tabIcon(QStringLiteral(":/Icon/process_list.svg")),
        kernelText("kernel.main.inner_tab.object_type", QStringLiteral("对象类型")));
    m_objectNamespaceInnerTabWidget->addTab(
        new KernelCommunicationEndpointTab(m_objectNamespaceInnerTabWidget),
        tabIcon(QStringLiteral(":/Icon/process_critical.svg")),
        kernelText("kernel.main.inner_tab.communication_endpoint", QStringLiteral("通信端点")));

    m_objectNamespaceToolLayout = new QHBoxLayout();
    m_objectNamespaceToolLayout->setContentsMargins(0, 0, 0, 0);
    m_objectNamespaceToolLayout->setSpacing(6);

    m_refreshObjectNamespaceButton = new QPushButton(QIcon(":/Icon/process_refresh.svg"), QString(), m_objectNamespaceOverviewPage);
    m_refreshObjectNamespaceButton->setToolTip(kernelText("kernel.main.object_namespace.refresh.tooltip", QStringLiteral("刷新对象命名空间枚举结果")));
    m_refreshObjectNamespaceButton->setStyleSheet(blueButtonStyle());
    m_refreshObjectNamespaceButton->setFixedWidth(34);

    m_objectNamespaceFilterEdit = new QLineEdit(m_objectNamespaceOverviewPage);
    m_objectNamespaceFilterEdit->setPlaceholderText(kernelText("kernel.main.object_namespace.filter.placeholder", QStringLiteral("按根目录/目录路径/对象名/对象类型/状态筛选")));
    m_objectNamespaceFilterEdit->setToolTip(kernelText("kernel.main.object_namespace.filter.tooltip", QStringLiteral("输入关键字后实时过滤对象命名空间树")));
    m_objectNamespaceFilterEdit->setClearButtonEnabled(true);
    m_objectNamespaceFilterEdit->setStyleSheet(blueInputStyle());

    m_objectNamespaceStatusLabel = new QLabel(kernelText("kernel.main.object_namespace.status.waiting", QStringLiteral("状态：等待刷新")), m_objectNamespaceOverviewPage);
    m_objectNamespaceStatusLabel->setStyleSheet(statusLabelStyle(KswordTheme::TextSecondaryHex()));

    m_objectNamespaceToolLayout->addWidget(m_refreshObjectNamespaceButton, 0);
    m_objectNamespaceToolLayout->addWidget(m_objectNamespaceFilterEdit, 1);
    m_objectNamespaceToolLayout->addWidget(m_objectNamespaceStatusLabel, 0);
    m_objectNamespaceOverviewLayout->addLayout(m_objectNamespaceToolLayout);

    QSplitter* verticalSplitter = new QSplitter(Qt::Vertical, m_objectNamespaceOverviewPage);
    m_objectNamespaceOverviewLayout->addWidget(verticalSplitter, 1);

    QSplitter* horizontalSplitter = new QSplitter(Qt::Horizontal, verticalSplitter);

    m_objectNamespaceTree = new QTreeWidget(horizontalSplitter);
    m_objectNamespaceTree->setColumnCount(static_cast<int>(ObjectNamespaceColumn::Count));
    m_objectNamespaceTree->setHeaderLabels(QStringList{
        kernelText("kernel.main.object_namespace.header.name", QStringLiteral("名称")),
        kernelText("kernel.main.object_namespace.header.type", QStringLiteral("类型")),
        kernelText("kernel.main.object_namespace.header.path_scope", QStringLiteral("路径/说明")),
        kernelText("kernel.main.object_namespace.header.status", QStringLiteral("状态")),
        kernelText("kernel.main.object_namespace.header.symbolic_target", QStringLiteral("符号链接目标"))
        });
    m_objectNamespaceTree->setSelectionMode(QAbstractItemView::SingleSelection);
    m_objectNamespaceTree->setAlternatingRowColors(true);
    m_objectNamespaceTree->setContextMenuPolicy(Qt::CustomContextMenu);
    m_objectNamespaceTree->setStyleSheet(itemSelectionStyle());
    m_objectNamespaceTree->setUniformRowHeights(true);
    m_objectNamespaceTree->setRootIsDecorated(true);
    m_objectNamespaceTree->header()->setStyleSheet(headerStyle());
    // 列宽策略：
    // - 初始布局仍交给全局 TableColumnAutoFit 压入可见宽度；
    // - 不强制隐藏横向滚动条，用户拖宽列后允许 Qt 按需显示。
    m_objectNamespaceTree->header()->setSectionResizeMode(QHeaderView::Stretch);
    m_objectNamespaceTree->setToolTip(kernelText("kernel.main.object_namespace.tree.tooltip", QStringLiteral("文件管理器式对象命名空间树，支持逐级展开与右键操作")));

    m_objectNamespacePropertyTable = new ks::ui::VisibleTableWidget(horizontalSplitter);
    m_objectNamespacePropertyTable->setColumnCount(2);
    m_objectNamespacePropertyTable->setHorizontalHeaderLabels(QStringList{
        kernelText("kernel.main.object_namespace.property.header", QStringLiteral("属性项")),
        kernelText("kernel.main.object_namespace.property.value", QStringLiteral("值"))
        });
    m_objectNamespacePropertyTable->setSelectionMode(QAbstractItemView::NoSelection);
    m_objectNamespacePropertyTable->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_objectNamespacePropertyTable->setAlternatingRowColors(true);
    m_objectNamespacePropertyTable->setContextMenuPolicy(Qt::CustomContextMenu);
    m_objectNamespacePropertyTable->setStyleSheet(itemSelectionStyle());
    m_objectNamespacePropertyTable->setCornerButtonEnabled(false);
    m_objectNamespacePropertyTable->verticalHeader()->setVisible(false);
    m_objectNamespacePropertyTable->horizontalHeader()->setStyleSheet(headerStyle());
    m_objectNamespacePropertyTable->horizontalHeader()->setSectionResizeMode(QHeaderView::ResizeToContents);
    m_objectNamespacePropertyTable->horizontalHeader()->setSectionResizeMode(1, QHeaderView::Stretch);
    m_objectNamespacePropertyTable->setColumnWidth(0, 220);
    m_objectNamespacePropertyTable->setToolTip(kernelText("kernel.main.object_namespace.property.tooltip", QStringLiteral("当前选中节点的字段详情（字段名 + 字段值）")));

    m_objectNamespaceDetailEditor = new CodeEditorWidget(verticalSplitter);
    m_objectNamespaceDetailEditor->setReadOnly(true);
    m_objectNamespaceDetailEditor->setText(kernelText("kernel.main.object_namespace.detail.initial", QStringLiteral("请选择对象命名空间节点查看详情。")));

    horizontalSplitter->setStretchFactor(0, 3);
    horizontalSplitter->setStretchFactor(1, 2);
    verticalSplitter->setStretchFactor(0, 4);
    verticalSplitter->setStretchFactor(1, 2);

    // 对象命名空间页连接：刷新、筛选、详情联动、右键菜单。
    connect(m_refreshObjectNamespaceButton, &QPushButton::clicked, this, [this]() {
        refreshObjectNamespaceAsync();
    });
    connect(m_objectNamespaceFilterEdit, &QLineEdit::textChanged, this, [this](const QString& filterText) {
        rebuildObjectNamespaceTable(filterText.trimmed());
    });
    connect(m_objectNamespaceTree, &QTreeWidget::currentItemChanged, this, [this](QTreeWidgetItem*, QTreeWidgetItem*) {
        showObjectNamespaceDetailByCurrentRow();
    });
    connect(m_objectNamespaceTree, &QTreeWidget::customContextMenuRequested, this, [this](const QPoint& localPosition) {
        showObjectNamespaceContextMenu(localPosition);
    });
    connect(m_objectNamespacePropertyTable, &QTableWidget::customContextMenuRequested, this, [this](const QPoint& localPosition) {
        showCopyRowContextMenu(m_objectNamespacePropertyTable, localPosition, false);
    });
}

void KernelDock::initializeAtomTableTab()
{
    if (m_atomPage == nullptr || m_atomLayout != nullptr)
    {
        return;
    }

    m_atomLayout = new QVBoxLayout(m_atomPage);
    m_atomLayout->setContentsMargins(4, 4, 4, 4);
    m_atomLayout->setSpacing(6);

    m_atomToolLayout = new QHBoxLayout();
    m_atomToolLayout->setContentsMargins(0, 0, 0, 0);
    m_atomToolLayout->setSpacing(6);

    m_refreshAtomButton = new QPushButton(QIcon(":/Icon/process_refresh.svg"), QString(), m_atomPage);
    m_refreshAtomButton->setToolTip(kernelText("kernel.main.atom.refresh.tooltip", QStringLiteral("刷新原子表遍历结果")));
    m_refreshAtomButton->setStyleSheet(blueButtonStyle());
    m_refreshAtomButton->setFixedWidth(34);

    m_atomFilterEdit = new QLineEdit(m_atomPage);
    m_atomFilterEdit->setPlaceholderText(kernelText("kernel.main.atom.filter.placeholder", QStringLiteral("按 Atom 值/十六进制/名称/来源筛选")));
    m_atomFilterEdit->setToolTip(kernelText("kernel.main.atom.filter.tooltip", QStringLiteral("输入关键字后实时过滤原子表")));
    m_atomFilterEdit->setClearButtonEnabled(true);
    m_atomFilterEdit->setStyleSheet(blueInputStyle());

    m_atomStatusLabel = new QLabel(kernelText("kernel.main.atom.status.waiting", QStringLiteral("状态：等待刷新")), m_atomPage);
    m_atomStatusLabel->setStyleSheet(statusLabelStyle(KswordTheme::TextSecondaryHex()));

    m_atomToolLayout->addWidget(m_refreshAtomButton, 0);
    m_atomToolLayout->addWidget(m_atomFilterEdit, 1);
    m_atomToolLayout->addWidget(m_atomStatusLabel, 0);
    m_atomLayout->addLayout(m_atomToolLayout);

    QSplitter* splitter = new QSplitter(Qt::Vertical, m_atomPage);
    m_atomLayout->addWidget(splitter, 1);

    m_atomTable = new ks::ui::VisibleTableWidget(splitter);
    m_atomTable->setColumnCount(static_cast<int>(AtomColumn::Count));
    m_atomTable->setHorizontalHeaderLabels(QStringList{
        kernelText("kernel.main.atom.header.value", QStringLiteral("Atom值")),
        kernelText("kernel.main.atom.header.hex", QStringLiteral("十六进制")),
        kernelText("kernel.main.atom.header.name", QStringLiteral("名称")),
        kernelText("kernel.main.atom.header.source", QStringLiteral("来源")),
        kernelText("kernel.main.atom.header.status", QStringLiteral("状态"))
        });
    m_atomTable->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_atomTable->setSelectionMode(QAbstractItemView::SingleSelection);
    m_atomTable->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_atomTable->setAlternatingRowColors(true);
    m_atomTable->setContextMenuPolicy(Qt::CustomContextMenu);
    m_atomTable->setStyleSheet(itemSelectionStyle());
    m_atomTable->setCornerButtonEnabled(false);
    m_atomTable->verticalHeader()->setVisible(false);
    m_atomTable->horizontalHeader()->setStyleSheet(headerStyle());
    m_atomTable->horizontalHeader()->setSectionResizeMode(QHeaderView::ResizeToContents);
    m_atomTable->horizontalHeader()->setSectionResizeMode(static_cast<int>(AtomColumn::Name), QHeaderView::Stretch);
    m_atomTable->setColumnWidth(static_cast<int>(AtomColumn::Value), 110);
    m_atomTable->setColumnWidth(static_cast<int>(AtomColumn::Hex), 110);
    m_atomTable->setColumnWidth(static_cast<int>(AtomColumn::Source), 220);
    m_atomTable->setColumnWidth(static_cast<int>(AtomColumn::Status), 160);

    m_atomDetailEditor = new CodeEditorWidget(splitter);
    m_atomDetailEditor->setReadOnly(true);
    m_atomDetailEditor->setText(kernelText("kernel.main.atom.detail.initial", QStringLiteral("请选择一条原子记录查看详情。")));

    splitter->setStretchFactor(0, 3);
    splitter->setStretchFactor(1, 2);

    // 原子表页连接：刷新、筛选、详情联动、右键菜单。
    connect(m_refreshAtomButton, &QPushButton::clicked, this, [this]() {
        refreshAtomTableAsync();
    });
    connect(m_atomFilterEdit, &QLineEdit::textChanged, this, [this](const QString& filterText) {
        rebuildAtomTable(filterText.trimmed());
    });
    connect(m_atomTable, &QTableWidget::currentCellChanged, this, [this](int, int, int, int) {
        showAtomDetailByCurrentRow();
    });
    connect(m_atomTable, &QTableWidget::customContextMenuRequested, this, [this](const QPoint& localPosition) {
        showAtomContextMenu(localPosition);
    });
}

void KernelDock::initializeNtQueryTab()
{
    if (m_ntQueryPage == nullptr || m_ntQueryLayout != nullptr)
    {
        return;
    }

    m_ntQueryLayout = new QVBoxLayout(m_ntQueryPage);
    m_ntQueryLayout->setContentsMargins(4, 4, 4, 4);
    m_ntQueryLayout->setSpacing(6);

    m_ntQueryToolLayout = new QHBoxLayout();
    m_ntQueryToolLayout->setContentsMargins(0, 0, 0, 0);
    m_ntQueryToolLayout->setSpacing(6);

    m_refreshNtQueryButton = new QPushButton(QIcon(":/Icon/process_refresh.svg"), QString(), m_ntQueryPage);
    m_refreshNtQueryButton->setToolTip(kernelText("kernel.main.nt_query.refresh.tooltip", QStringLiteral("刷新历史 NtQuery 信息")));
    m_refreshNtQueryButton->setStyleSheet(blueButtonStyle());
    m_refreshNtQueryButton->setFixedWidth(34);

    m_ntQueryStatusLabel = new QLabel(kernelText("kernel.main.nt_query.status.waiting", QStringLiteral("状态：等待刷新")), m_ntQueryPage);
    m_ntQueryStatusLabel->setStyleSheet(statusLabelStyle(KswordTheme::TextSecondaryHex()));

    m_ntQueryToolLayout->addWidget(m_refreshNtQueryButton, 0);
    m_ntQueryToolLayout->addWidget(m_ntQueryStatusLabel, 1);
    m_ntQueryLayout->addLayout(m_ntQueryToolLayout);

    QSplitter* splitter = new QSplitter(Qt::Vertical, m_ntQueryPage);
    m_ntQueryLayout->addWidget(splitter, 1);

    m_ntQueryTable = new ks::ui::VisibleTableWidget(splitter);
    m_ntQueryTable->setColumnCount(static_cast<int>(NtQueryColumn::Count));
    m_ntQueryTable->setHorizontalHeaderLabels(QStringList{
        kernelText("kernel.main.nt_query.header.category", QStringLiteral("类别")),
        kernelText("kernel.main.nt_query.header.function", QStringLiteral("函数")),
        kernelText("kernel.main.nt_query.header.item", QStringLiteral("查询项")),
        kernelText("kernel.main.nt_query.header.status", QStringLiteral("状态")),
        kernelText("kernel.main.nt_query.header.summary", QStringLiteral("摘要"))
        });
    m_ntQueryTable->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_ntQueryTable->setSelectionMode(QAbstractItemView::SingleSelection);
    m_ntQueryTable->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_ntQueryTable->setAlternatingRowColors(true);
    m_ntQueryTable->setContextMenuPolicy(Qt::CustomContextMenu);
    m_ntQueryTable->setStyleSheet(itemSelectionStyle());
    m_ntQueryTable->setCornerButtonEnabled(false);
    m_ntQueryTable->verticalHeader()->setVisible(false);
    m_ntQueryTable->horizontalHeader()->setStyleSheet(headerStyle());
    m_ntQueryTable->horizontalHeader()->setSectionResizeMode(QHeaderView::ResizeToContents);
    m_ntQueryTable->horizontalHeader()->setSectionResizeMode(static_cast<int>(NtQueryColumn::Summary), QHeaderView::Stretch);

    m_ntQueryDetailEditor = new CodeEditorWidget(splitter);
    m_ntQueryDetailEditor->setReadOnly(true);
    m_ntQueryDetailEditor->setText(kernelText("kernel.main.nt_query.detail.initial", QStringLiteral("请选择一条 NtQuery 结果查看详情。")));

    splitter->setStretchFactor(0, 3);
    splitter->setStretchFactor(1, 2);

    // 历史 NtQuery 页连接：刷新与详情联动。
    connect(m_refreshNtQueryButton, &QPushButton::clicked, this, [this]() {
        refreshNtQueryAsync();
    });
    connect(m_ntQueryTable, &QTableWidget::currentCellChanged, this, [this](int, int, int, int) {
        showNtQueryDetailByCurrentRow();
    });
    connect(m_ntQueryTable, &QTableWidget::customContextMenuRequested, this, [this](const QPoint& localPosition) {
        showCopyRowContextMenu(m_ntQueryTable, localPosition, true);
    });
}

void KernelDock::initializeConnections()
{
    // 顶层页签切换：按需初始化对应页面并触发首轮数据加载。
    connect(m_tabWidget, &QTabWidget::currentChanged, this, [this](const int tabIndex) {
        updateTabIconContrast();
        ensureTabInitialized(tabIndex);
    });
}

void KernelDock::initializeCrossViewTab()
{
    if (m_crossViewPage == nullptr)
    {
        return;
    }

    // 只读 CID / cross-view 页直接复用独立组件，避免在主容器里再复制一套采集逻辑。
    auto* layout = new QVBoxLayout(m_crossViewPage);
    layout->setContentsMargins(4, 4, 4, 4);
    layout->setSpacing(0);

    auto* tab = new KernelDockCidTab(m_crossViewPage);
    layout->addWidget(tab, 1);
}

void KernelDock::initializeIpcTab()
{
    if (m_ipcPage == nullptr)
    {
        return;
    }

    // 只读 IPC 页直接复用独立组件，聚合 NamedPipe、ALPC 与通信端点。
    auto* layout = new QVBoxLayout(m_ipcPage);
    layout->setContentsMargins(4, 4, 4, 4);
    layout->setSpacing(0);

    auto* tab = new KernelDockIpcTab(m_ipcPage);
    layout->addWidget(tab, 1);
}

void KernelDock::ensureTabInitialized(const int tabIndex)
{
    if (tabIndex == m_objectNamespaceTabIndex && !m_objectNamespaceTabInitialized)
    {
        showTabInitializingProgress(tabIndex, kernelText("kernel.main.tab.object_namespace.title", QStringLiteral("对象命名空间")));
        initializeObjectNamespaceTab();
        m_objectNamespaceTabInitialized = true;
        hideTabInitializingProgress();
        refreshObjectNamespaceAsync();
        return;
    }

    if (tabIndex == m_atomTabIndex && !m_atomTabInitialized)
    {
        showTabInitializingProgress(tabIndex, kernelText("kernel.main.progress.global_atom", QStringLiteral("全局原子表")));
        initializeAtomTableTab();
        m_atomTabInitialized = true;
        hideTabInitializingProgress();
        refreshAtomTableAsync();
        return;
    }

    if (tabIndex == m_ntQueryTabIndex && !m_ntQueryTabInitialized)
    {
        showTabInitializingProgress(tabIndex, kernelText("kernel.main.tab.nt_query.progress", QStringLiteral("历史 NtQuery")));
        initializeNtQueryTab();
        m_ntQueryTabInitialized = true;
        hideTabInitializingProgress();
        refreshNtQueryAsync();
        return;
    }

    if (tabIndex == m_ssdtTabIndex && !m_ssdtTabInitialized)
    {
        showTabInitializingProgress(tabIndex, QStringLiteral("SSDT"));
        initializeSsdtTab();
        m_ssdtTabInitialized = true;
        hideTabInitializingProgress();
        refreshSsdtAsync();
        return;
    }

    if (tabIndex == m_shadowSsdtTabIndex && !m_shadowSsdtTabInitialized)
    {
        showTabInitializingProgress(tabIndex, QStringLiteral("SSSDT"));
        initializeShadowSsdtTab();
        m_shadowSsdtTabInitialized = true;
        hideTabInitializingProgress();
        refreshShadowSsdtAsync();
        return;
    }

    if (tabIndex == m_inlineHookTabIndex && !m_inlineHookTabInitialized)
    {
        showTabInitializingProgress(tabIndex, QStringLiteral("Inline Hook"));
        initializeInlineHookTab();
        m_inlineHookTabInitialized = true;
        hideTabInitializingProgress();
        refreshInlineHooksAsync();
        return;
    }

    if (tabIndex == m_iatEatHookTabIndex && !m_iatEatHookTabInitialized)
    {
        showTabInitializingProgress(tabIndex, QStringLiteral("IAT/EAT"));
        initializeIatEatHookTab();
        m_iatEatHookTabInitialized = true;
        hideTabInitializingProgress();
        refreshIatEatHooksAsync();
        return;
    }

    if (tabIndex == m_crossViewTabIndex && !m_crossViewTabInitialized)
    {
        showTabInitializingProgress(tabIndex, kernelText("kernel.main.tab.cid.title", QStringLiteral("CID表")));
        initializeCrossViewTab();
        m_crossViewTabInitialized = true;
        hideTabInitializingProgress();
        return;
    }

    if (tabIndex == m_ipcTabIndex && !m_ipcTabInitialized)
    {
        showTabInitializingProgress(tabIndex, QStringLiteral("IPC"));
        initializeIpcTab();
        m_ipcTabInitialized = true;
        hideTabInitializingProgress();
        return;
    }

    if (tabIndex == m_dynDataTabIndex && !m_dynDataTabInitialized)
    {
        showTabInitializingProgress(tabIndex, kernelText("kernel.main.tab.dyn_data.title", QStringLiteral("动态偏移")));
        initializeDynDataTab();
        m_dynDataTabInitialized = true;
        hideTabInitializingProgress();
        refreshDynDataAsync();
        return;
    }

    if (tabIndex == m_driverStatusTabIndex && !m_driverStatusTabInitialized)
    {
        showTabInitializingProgress(tabIndex, kernelText("kernel.main.tab.driver_status.title", QStringLiteral("驱动状态")));
        initializeDriverStatusTab();
        m_driverStatusTabInitialized = true;
        hideTabInitializingProgress();
        refreshDriverStatusAsync();
        return;
    }

    if (tabIndex == m_callbackTabIndex && !m_callbackTabInitialized)
    {
        showTabInitializingProgress(tabIndex, kernelText("kernel.main.tab.callback.title", QStringLiteral("驱动回调")));
        initializeCallbackInterceptTab();
        m_callbackTabInitialized = true;
        hideTabInitializingProgress();
        return;
    }

    if (tabIndex == m_callbackEnumTabIndex && !m_callbackEnumTabInitialized)
    {
        showTabInitializingProgress(tabIndex, kernelText("kernel.main.tab.callback_enum.title", QStringLiteral("回调遍历")));
        initializeCallbackEnumTab();
        m_callbackEnumTabInitialized = true;
        hideTabInitializingProgress();
        refreshCallbackEnumAsync();
        return;
    }

}
