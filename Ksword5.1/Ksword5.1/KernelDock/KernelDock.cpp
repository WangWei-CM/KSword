
#include "KernelDock.h"

// ============================================================
// KernelDock.cpp
// 作用说明：
// 1) 实现内核 Dock 的顶层页签 UI（对象命名空间 / 原子表 / SSDT / 历史 NtQuery / 驱动回调）；
// 2) 实现异步刷新、筛选、详情联动与右键菜单；
// 3) 具体底层枚举逻辑放在 Worker 文件，当前文件仅做界面和交互编排。
// ============================================================

#include "../UI/CodeEditorWidget.h"
#include "../theme.h"

#include <QAbstractItemView>
#include <QApplication>
#include <QEventLoop>
#include <QComboBox>
#include <QHeaderView>
#include <QHBoxLayout>
#include <QIcon>
#include <QLabel>
#include <QLineEdit>
#include <QPainter>
#include <QPixmap>
#include <QFrame>
#include <QProgressBar>
#include <QPushButton>
#include <QScrollArea>
#include <QSplitter>
#include <QSvgRenderer>
#include <QTableWidget>
#include <QTabWidget>
#include <QTimer>
#include <QTreeWidget>
#include <QVBoxLayout>

namespace
{
    // blueButtonStyle：
    // - 作用：统一图标按钮样式（带悬停与按下态）。
    QString blueButtonStyle()
    {
        return QStringLiteral(
            "QPushButton{color:%1;background:%5;border:1px solid %2;border-radius:2px;padding:3px 8px;}"
            "QPushButton:hover{background:%3;color:#FFFFFF;border:1px solid %3;}"
            "QPushButton:pressed{background:%4;color:#FFFFFF;}")
            .arg(KswordTheme::PrimaryBlueHex)
            .arg(KswordTheme::PrimaryBlueBorderHex)
            .arg(KswordTheme::PrimaryBlueSolidHoverHex())
            .arg(KswordTheme::PrimaryBluePressedHex)
            .arg(KswordTheme::SurfaceHex());
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
            "QTableWidget::item:selected{background:%1;color:#FFFFFF;}"
            "QTreeWidget::item:selected{background:%1;color:#FFFFFF;}")
            .arg(KswordTheme::PrimaryBlueHex);
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
        return tintedSvgIcon(iconPath, QColor(255, 255, 255));
    }
}

KernelDock::KernelDock(QWidget* parent)
    : QWidget(parent)
{
    kLogEvent initEvent;
    info << initEvent << "[KernelDock] 构造开始，准备初始化五页内核视图。" << eol;

    initializeUi();
    initializeConnections();

    // 首次延迟加载当前页，避免切到内核 Dock 时同步初始化全部页签造成卡顿。
    QTimer::singleShot(0, this, [this]() {
        ensureTabInitialized(m_tabWidget != nullptr ? m_tabWidget->currentIndex() : -1);
    });

    info << initEvent << "[KernelDock] 构造完成。" << eol;
}

void KernelDock::initializeUi()
{
    m_rootLayout = new QVBoxLayout(this);
    m_rootLayout->setContentsMargins(6, 6, 6, 6);
    m_rootLayout->setSpacing(4);

    // 初始化进度条默认隐藏，仅在惰性 Tab 开始构建时短暂显示。
    m_tabInitializingStatusLabel = new QLabel(QStringLiteral("页面就绪"), this);
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
    m_ntQueryPage = new QWidget(m_tabWidget);
    m_callbackInterceptPage = new QWidget(m_tabWidget);
    m_callbackRemovePage = new QWidget(m_tabWidget);

    m_objectNamespaceTabIndex = m_tabWidget->addTab(
        m_objectNamespacePage,
        tabIcon(QStringLiteral(":/Icon/process_tree.svg")),
        QStringLiteral("对象命名空间"));
    m_tabWidget->setTabToolTip(m_objectNamespaceTabIndex, QStringLiteral("对象管理器命名空间遍历（默认页）"));

    m_atomTabIndex = m_tabWidget->addTab(
        m_atomPage,
        tabIcon(QStringLiteral(":/Icon/process_threads.svg")),
        QStringLiteral("原子表遍历"));
    m_tabWidget->setTabToolTip(m_atomTabIndex, QStringLiteral("遍历全局原子范围并提供校验操作"));

    m_ntQueryTabIndex = m_tabWidget->addTab(
        m_ntQueryPage,
        tabIcon(QStringLiteral(":/Icon/process_details.svg")),
        QStringLiteral("历史NtQuery"));
    m_tabWidget->setTabToolTip(m_ntQueryTabIndex, QStringLiteral("旧版内核 NtQuery 信息页"));

    m_ssdtTabIndex = m_tabWidget->addTab(
        m_ssdtPage,
        tabIcon(QStringLiteral(":/Icon/process_list.svg")),
        QStringLiteral("SSDT遍历"));
    m_tabWidget->setTabToolTip(m_ssdtTabIndex, QStringLiteral("驱动侧 SSDT 服务索引遍历结果"));

    m_callbackTabIndex = m_tabWidget->addTab(
        m_callbackInterceptPage,
        tabIcon(QStringLiteral(":/Icon/process_critical.svg")),
        QStringLiteral("驱动回调"));
    m_tabWidget->setTabToolTip(m_callbackTabIndex, QStringLiteral("驱动回调拦截规则管理与询问事件处理"));

    m_callbackRemoveTabIndex = m_tabWidget->addTab(
        m_callbackRemovePage,
        tabIcon(QStringLiteral(":/Icon/process_terminate.svg")),
        QStringLiteral("回调移除"));
    m_tabWidget->setTabToolTip(m_callbackRemoveTabIndex, QStringLiteral("通过 Ksword 内核驱动移除其他驱动注册的回调"));

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

    m_tabInitializingStatusLabel->setText(QStringLiteral("正在初始化 %1 页面...").arg(titleText));
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

QVBoxLayout* KernelDock::wrapPageInScrollArea(QWidget* pageWidget, QWidget** contentWidgetOut)
{
    if (pageWidget == nullptr)
    {
        return nullptr;
    }

    // 外层只保留一个 QScrollArea，真实控件放入 contentWidget，保证页面空白处滚轮也能滚动。
    auto* outerLayout = new QVBoxLayout(pageWidget);
    outerLayout->setContentsMargins(0, 0, 0, 0);
    outerLayout->setSpacing(0);

    auto* scrollArea = new QScrollArea(pageWidget);
    scrollArea->setWidgetResizable(true);
    scrollArea->setFrameShape(QFrame::NoFrame);
    scrollArea->setHorizontalScrollBarPolicy(Qt::ScrollBarAsNeeded);
    scrollArea->setVerticalScrollBarPolicy(Qt::ScrollBarAsNeeded);
    scrollArea->viewport()->setAttribute(Qt::WA_Hover, true);

    auto* contentWidget = new QWidget(scrollArea);
    contentWidget->setObjectName(QStringLiteral("ksKernelScrollableContent"));
    contentWidget->setAutoFillBackground(false);
    contentWidget->setAttribute(Qt::WA_StyledBackground, false);

    auto* contentLayout = new QVBoxLayout(contentWidget);
    contentLayout->setContentsMargins(4, 4, 4, 4);
    contentLayout->setSpacing(6);

    scrollArea->setWidget(contentWidget);
    outerLayout->addWidget(scrollArea, 1);

    if (contentWidgetOut != nullptr)
    {
        *contentWidgetOut = contentWidget;
    }

    if (pageWidget == m_callbackRemovePage)
    {
        m_callbackRemoveScrollArea = scrollArea;
    }
    return contentLayout;
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
    m_tabWidget->setTabIcon(m_callbackTabIndex, tabIcon(QStringLiteral(":/Icon/process_critical.svg")));
    m_tabWidget->setTabIcon(m_callbackRemoveTabIndex, tabIcon(QStringLiteral(":/Icon/process_terminate.svg")));

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
    else if (currentIndex == m_callbackTabIndex)
    {
        m_tabWidget->setTabIcon(currentIndex, selectedTabIcon(QStringLiteral(":/Icon/process_critical.svg")));
    }
    else if (currentIndex == m_callbackRemoveTabIndex)
    {
        m_tabWidget->setTabIcon(currentIndex, selectedTabIcon(QStringLiteral(":/Icon/process_terminate.svg")));
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

    m_objectNamespaceToolLayout = new QHBoxLayout();
    m_objectNamespaceToolLayout->setContentsMargins(0, 0, 0, 0);
    m_objectNamespaceToolLayout->setSpacing(6);

    m_refreshObjectNamespaceButton = new QPushButton(QIcon(":/Icon/process_refresh.svg"), QString(), m_objectNamespacePage);
    m_refreshObjectNamespaceButton->setToolTip(QStringLiteral("刷新对象命名空间枚举结果"));
    m_refreshObjectNamespaceButton->setStyleSheet(blueButtonStyle());
    m_refreshObjectNamespaceButton->setFixedWidth(34);

    m_objectNamespaceFilterEdit = new QLineEdit(m_objectNamespacePage);
    m_objectNamespaceFilterEdit->setPlaceholderText(QStringLiteral("按根目录/目录路径/对象名/对象类型/状态筛选"));
    m_objectNamespaceFilterEdit->setToolTip(QStringLiteral("输入关键字后实时过滤对象命名空间树"));
    m_objectNamespaceFilterEdit->setClearButtonEnabled(true);
    m_objectNamespaceFilterEdit->setStyleSheet(blueInputStyle());

    m_objectNamespaceStatusLabel = new QLabel(QStringLiteral("状态：等待刷新"), m_objectNamespacePage);
    m_objectNamespaceStatusLabel->setStyleSheet(statusLabelStyle(KswordTheme::TextSecondaryHex()));

    m_objectNamespaceToolLayout->addWidget(m_refreshObjectNamespaceButton, 0);
    m_objectNamespaceToolLayout->addWidget(m_objectNamespaceFilterEdit, 1);
    m_objectNamespaceToolLayout->addWidget(m_objectNamespaceStatusLabel, 0);
    m_objectNamespaceLayout->addLayout(m_objectNamespaceToolLayout);

    QSplitter* verticalSplitter = new QSplitter(Qt::Vertical, m_objectNamespacePage);
    m_objectNamespaceLayout->addWidget(verticalSplitter, 1);

    QSplitter* horizontalSplitter = new QSplitter(Qt::Horizontal, verticalSplitter);

    m_objectNamespaceTree = new QTreeWidget(horizontalSplitter);
    m_objectNamespaceTree->setColumnCount(static_cast<int>(ObjectNamespaceColumn::Count));
    m_objectNamespaceTree->setHeaderLabels(QStringList{
        QStringLiteral("名称"),
        QStringLiteral("类型"),
        QStringLiteral("路径/说明"),
        QStringLiteral("状态"),
        QStringLiteral("符号链接目标")
        });
    m_objectNamespaceTree->setSelectionMode(QAbstractItemView::SingleSelection);
    m_objectNamespaceTree->setAlternatingRowColors(true);
    m_objectNamespaceTree->setContextMenuPolicy(Qt::CustomContextMenu);
    m_objectNamespaceTree->setStyleSheet(itemSelectionStyle());
    m_objectNamespaceTree->setUniformRowHeights(true);
    m_objectNamespaceTree->setRootIsDecorated(true);
    m_objectNamespaceTree->header()->setStyleSheet(headerStyle());
    // 列宽策略：始终按可用宽度自适应，避免出现横向滚动条。
    m_objectNamespaceTree->header()->setSectionResizeMode(QHeaderView::Stretch);
    m_objectNamespaceTree->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    m_objectNamespaceTree->setToolTip(QStringLiteral("文件管理器式对象命名空间树，支持逐级展开与右键操作"));

    m_objectNamespacePropertyTable = new QTableWidget(horizontalSplitter);
    m_objectNamespacePropertyTable->setColumnCount(2);
    m_objectNamespacePropertyTable->setHorizontalHeaderLabels(QStringList{
        QStringLiteral("属性项"),
        QStringLiteral("值")
        });
    m_objectNamespacePropertyTable->setSelectionMode(QAbstractItemView::NoSelection);
    m_objectNamespacePropertyTable->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_objectNamespacePropertyTable->setAlternatingRowColors(true);
    m_objectNamespacePropertyTable->setStyleSheet(itemSelectionStyle());
    m_objectNamespacePropertyTable->setCornerButtonEnabled(false);
    m_objectNamespacePropertyTable->verticalHeader()->setVisible(false);
    m_objectNamespacePropertyTable->horizontalHeader()->setStyleSheet(headerStyle());
    m_objectNamespacePropertyTable->horizontalHeader()->setSectionResizeMode(QHeaderView::ResizeToContents);
    m_objectNamespacePropertyTable->horizontalHeader()->setSectionResizeMode(1, QHeaderView::Stretch);
    m_objectNamespacePropertyTable->setColumnWidth(0, 220);
    m_objectNamespacePropertyTable->setToolTip(QStringLiteral("当前选中节点的字段详情（字段名 + 字段值）"));

    m_objectNamespaceDetailEditor = new CodeEditorWidget(verticalSplitter);
    m_objectNamespaceDetailEditor->setReadOnly(true);
    m_objectNamespaceDetailEditor->setText(QStringLiteral("请选择对象命名空间节点查看详情。"));

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
    m_refreshAtomButton->setToolTip(QStringLiteral("刷新原子表遍历结果"));
    m_refreshAtomButton->setStyleSheet(blueButtonStyle());
    m_refreshAtomButton->setFixedWidth(34);

    m_atomFilterEdit = new QLineEdit(m_atomPage);
    m_atomFilterEdit->setPlaceholderText(QStringLiteral("按 Atom 值/十六进制/名称/来源筛选"));
    m_atomFilterEdit->setToolTip(QStringLiteral("输入关键字后实时过滤原子表"));
    m_atomFilterEdit->setClearButtonEnabled(true);
    m_atomFilterEdit->setStyleSheet(blueInputStyle());

    m_atomStatusLabel = new QLabel(QStringLiteral("状态：等待刷新"), m_atomPage);
    m_atomStatusLabel->setStyleSheet(statusLabelStyle(KswordTheme::TextSecondaryHex()));

    m_atomToolLayout->addWidget(m_refreshAtomButton, 0);
    m_atomToolLayout->addWidget(m_atomFilterEdit, 1);
    m_atomToolLayout->addWidget(m_atomStatusLabel, 0);
    m_atomLayout->addLayout(m_atomToolLayout);

    QSplitter* splitter = new QSplitter(Qt::Vertical, m_atomPage);
    m_atomLayout->addWidget(splitter, 1);

    m_atomTable = new QTableWidget(splitter);
    m_atomTable->setColumnCount(static_cast<int>(AtomColumn::Count));
    m_atomTable->setHorizontalHeaderLabels(QStringList{
        QStringLiteral("Atom值"),
        QStringLiteral("十六进制"),
        QStringLiteral("名称"),
        QStringLiteral("来源"),
        QStringLiteral("状态")
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
    m_atomDetailEditor->setText(QStringLiteral("请选择一条原子记录查看详情。"));

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
    m_refreshNtQueryButton->setToolTip(QStringLiteral("刷新历史 NtQuery 信息"));
    m_refreshNtQueryButton->setStyleSheet(blueButtonStyle());
    m_refreshNtQueryButton->setFixedWidth(34);

    m_ntQueryStatusLabel = new QLabel(QStringLiteral("状态：等待刷新"), m_ntQueryPage);
    m_ntQueryStatusLabel->setStyleSheet(statusLabelStyle(KswordTheme::TextSecondaryHex()));

    m_ntQueryToolLayout->addWidget(m_refreshNtQueryButton, 0);
    m_ntQueryToolLayout->addWidget(m_ntQueryStatusLabel, 1);
    m_ntQueryLayout->addLayout(m_ntQueryToolLayout);

    QSplitter* splitter = new QSplitter(Qt::Vertical, m_ntQueryPage);
    m_ntQueryLayout->addWidget(splitter, 1);

    m_ntQueryTable = new QTableWidget(splitter);
    m_ntQueryTable->setColumnCount(static_cast<int>(NtQueryColumn::Count));
    m_ntQueryTable->setHorizontalHeaderLabels(QStringList{
        QStringLiteral("类别"),
        QStringLiteral("函数"),
        QStringLiteral("查询项"),
        QStringLiteral("状态"),
        QStringLiteral("摘要")
        });
    m_ntQueryTable->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_ntQueryTable->setSelectionMode(QAbstractItemView::SingleSelection);
    m_ntQueryTable->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_ntQueryTable->setAlternatingRowColors(true);
    m_ntQueryTable->setStyleSheet(itemSelectionStyle());
    m_ntQueryTable->setCornerButtonEnabled(false);
    m_ntQueryTable->verticalHeader()->setVisible(false);
    m_ntQueryTable->horizontalHeader()->setStyleSheet(headerStyle());
    m_ntQueryTable->horizontalHeader()->setSectionResizeMode(QHeaderView::ResizeToContents);
    m_ntQueryTable->horizontalHeader()->setSectionResizeMode(static_cast<int>(NtQueryColumn::Summary), QHeaderView::Stretch);

    m_ntQueryDetailEditor = new CodeEditorWidget(splitter);
    m_ntQueryDetailEditor->setReadOnly(true);
    m_ntQueryDetailEditor->setText(QStringLiteral("请选择一条 NtQuery 结果查看详情。"));

    splitter->setStretchFactor(0, 3);
    splitter->setStretchFactor(1, 2);

    // 历史 NtQuery 页连接：刷新与详情联动。
    connect(m_refreshNtQueryButton, &QPushButton::clicked, this, [this]() {
        refreshNtQueryAsync();
    });
    connect(m_ntQueryTable, &QTableWidget::currentCellChanged, this, [this](int, int, int, int) {
        showNtQueryDetailByCurrentRow();
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

void KernelDock::ensureTabInitialized(const int tabIndex)
{
    if (tabIndex == m_objectNamespaceTabIndex && !m_objectNamespaceTabInitialized)
    {
        showTabInitializingProgress(tabIndex, QStringLiteral("对象命名空间"));
        initializeObjectNamespaceTab();
        m_objectNamespaceTabInitialized = true;
        hideTabInitializingProgress();
        refreshObjectNamespaceAsync();
        return;
    }

    if (tabIndex == m_atomTabIndex && !m_atomTabInitialized)
    {
        showTabInitializingProgress(tabIndex, QStringLiteral("全局原子表"));
        initializeAtomTableTab();
        m_atomTabInitialized = true;
        hideTabInitializingProgress();
        refreshAtomTableAsync();
        return;
    }

    if (tabIndex == m_ntQueryTabIndex && !m_ntQueryTabInitialized)
    {
        showTabInitializingProgress(tabIndex, QStringLiteral("历史 NtQuery"));
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

    if (tabIndex == m_callbackTabIndex && !m_callbackTabInitialized)
    {
        showTabInitializingProgress(tabIndex, QStringLiteral("驱动回调"));
        initializeCallbackInterceptTab();
        m_callbackTabInitialized = true;
        hideTabInitializingProgress();
        return;
    }

    if (tabIndex == m_callbackRemoveTabIndex && !m_callbackRemoveTabInitialized)
    {
        showTabInitializingProgress(tabIndex, QStringLiteral("回调移除"));
        initializeCallbackRemoveTab();
        m_callbackRemoveTabInitialized = true;
        hideTabInitializingProgress();
    }
}
