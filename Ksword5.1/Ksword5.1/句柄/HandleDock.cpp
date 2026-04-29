#include "HandleDock.h"

#include "../theme.h"

#include <QAbstractItemView>
#include <QApplication>
#include <QCheckBox>
#include <QClipboard>
#include <QComboBox>
#include <QHeaderView>
#include <QHBoxLayout>
#include <QIcon>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QMessageBox>
#include <QMetaObject>
#include <QPointer>
#include <QPushButton>
#include <QRunnable>
#include <QShowEvent>
#include <QSignalBlocker>
#include <QSpinBox>
#include <QStringList>
#include <QTabWidget>
#include <QThreadPool>
#include <QTreeWidget>
#include <QTreeWidgetItem>
#include <QVBoxLayout>

#include <algorithm>
#include <set>
#include <string>

namespace
{
    // buildBlueButtonStyle 作用：
    // - 生成项目统一蓝色按钮样式；
    // - iconOnly=true 时使用紧凑尺寸，适合图标按钮。
    QString buildBlueButtonStyle(const bool iconOnly)
    {
        const QString paddingText = iconOnly ? QStringLiteral("4px") : QStringLiteral("4px 10px");
        return QStringLiteral(
            "QPushButton {"
            "  color: %1;"
            "  background: %6;"
            "  border: 1px solid %2;"
            "  border-radius: 3px;"
            "  padding: %5;"
            "}"
            "QPushButton:hover {"
            "  background: %3;"
            "  color: #FFFFFF;"
            "  border: 1px solid %3;"
            "}"
            "QPushButton:pressed {"
            "  background: %4;"
            "  color: #FFFFFF;"
            "}")
            .arg(KswordTheme::PrimaryBlueHex)
            .arg(KswordTheme::PrimaryBlueBorderHex)
            .arg(KswordTheme::PrimaryBlueSolidHoverHex())
            .arg(KswordTheme::PrimaryBluePressedHex)
            .arg(paddingText)
            .arg(KswordTheme::SurfaceHex());
    }

    // buildLineEditStyle 作用：统一过滤输入框视觉风格。
    QString buildLineEditStyle()
    {
        return QStringLiteral(
            "QLineEdit {"
            "  border: 1px solid %1;"
            "  border-radius: 3px;"
            "  background: %2;"
            "  color: %3;"
            "  padding: 3px 6px;"
            "}"
            "QLineEdit:focus {"
            "  border: 1px solid %4;"
            "}")
            .arg(KswordTheme::BorderHex())
            .arg(KswordTheme::SurfaceHex())
            .arg(KswordTheme::TextPrimaryHex())
            .arg(KswordTheme::PrimaryBlueHex);
    }

    // buildComboAndSpinStyle 作用：统一下拉框与数字框样式。
    QString buildComboAndSpinStyle()
    {
        return QStringLiteral(
            "QComboBox, QSpinBox {"
            "  border: 1px solid %1;"
            "  border-radius: 3px;"
            "  background: %2;"
            "  color: %3;"
            "  padding: 2px 6px;"
            "}"
            "QComboBox:hover, QSpinBox:hover {"
            "  border-color: %4;"
            "}")
            .arg(KswordTheme::BorderHex())
            .arg(KswordTheme::SurfaceHex())
            .arg(KswordTheme::TextPrimaryHex())
            .arg(KswordTheme::PrimaryBlueHex);
    }

    // buildHeaderStyle 作用：统一表头样式，保持主题一致。
    QString buildHeaderStyle()
    {
        return QStringLiteral(
            "QHeaderView::section{"
            "  color:%1;"
            "  background:%2;"
            "  border:1px solid %3;"
            "  font-weight:600;"
            "}")
            .arg(KswordTheme::PrimaryBlueHex)
            .arg(KswordTheme::SurfaceHex())
            .arg(KswordTheme::BorderHex());
    }

    // boolText 作用：把布尔值统一转为中文“是/否”文本。
    QString boolText(const bool value)
    {
        return value ? QStringLiteral("是") : QStringLiteral("否");
    }

    // kHandleRenderSplashThreshold 作用：
    // - 控制何时弹出启动页提示“渲染句柄列表”；
    // - 仅在超大数据量渲染时启用，避免小列表频繁闪烁。
    constexpr std::size_t kHandleRenderSplashThreshold = 50000;

    // kHandleRenderProgressStep 作用：
    // - 控制进度回写频率；
    // - 通过分批更新降低额外开销。
    constexpr std::size_t kHandleRenderProgressStep = 4096;

    // HandleRenderSplashScope 作用：
    // - 在超大句柄表渲染期间显示启动页；
    // - 持续回写“渲染句柄列表到图形界面”进度，缓解“程序卡死”感知。
    class HandleRenderSplashScope final
    {
    public:
        explicit HandleRenderSplashScope(const std::size_t totalRowCount)
            : m_totalRowCount(totalRowCount)
        {
            if (m_totalRowCount < kHandleRenderSplashThreshold)
            {
                return;
            }

            m_visible = kSplash.show();
            if (!m_visible)
            {
                return;
            }

            kSplash.progress("渲染句柄列表到图形界面", 1);
        }

        ~HandleRenderSplashScope()
        {
            if (m_visible)
            {
                kSplash.hide();
            }
        }

        void update(const std::size_t renderedRowCount) const
        {
            if (!m_visible || m_totalRowCount == 0)
            {
                return;
            }

            const std::size_t normalizedRenderedCount = std::min(renderedRowCount, m_totalRowCount);
            const int progressPercent = std::max(
                1,
                std::min(99, static_cast<int>((normalizedRenderedCount * 100) / m_totalRowCount)));
            kSplash.progress("渲染句柄列表到图形界面", progressPercent);
        }

        void finish() const
        {
            if (m_visible)
            {
                kSplash.progress("渲染句柄列表到图形界面", 100);
            }
        }

    private:
        std::size_t m_totalRowCount = 0; // m_totalRowCount：当前渲染总行数。
        bool m_visible = false;          // m_visible：是否成功显示启动页。
    };
}

HandleDock::HandleDock(QWidget* parent)
    : QWidget(parent)
{
    // 构造阶段按“UI -> 连接”顺序初始化。
    initializeUi();
    initializeConnections();
}

void HandleDock::focusProcessId(const std::uint32_t processId, const bool triggerRefresh)
{
    if (m_tabWidget != nullptr && m_handleListPage != nullptr)
    {
        m_tabWidget->setCurrentWidget(m_handleListPage);
    }
    if (m_pidFilterEdit != nullptr)
    {
        m_pidFilterEdit->setText(QString::number(processId));
    }
    if (triggerRefresh)
    {
        requestAsyncRefresh(true);
    }
}

void HandleDock::showEvent(QShowEvent* event)
{
    QWidget::showEvent(event);
    if (m_initialRefreshDone)
    {
        return;
    }

    m_initialRefreshDone = true;
    requestObjectTypeRefreshAsync(true);
    requestAsyncRefresh(true);
}

void HandleDock::initializeUi()
{
    setObjectName(QStringLiteral("HandleDockRoot"));
    m_rootLayout = new QVBoxLayout(this);
    m_rootLayout->setContentsMargins(8, 8, 8, 8);
    m_rootLayout->setSpacing(6);

    m_tabWidget = new QTabWidget(this);
    m_rootLayout->addWidget(m_tabWidget, 1);

    initializeHandleListTab();
    initializeObjectTypeTab();
}

void HandleDock::initializeHandleListTab()
{
    m_handleListPage = new QWidget(m_tabWidget);
    m_handleListLayout = new QVBoxLayout(m_handleListPage);
    m_handleListLayout->setContentsMargins(6, 6, 6, 6);
    m_handleListLayout->setSpacing(6);

    m_toolbarLayout = new QHBoxLayout();
    m_toolbarLayout->setContentsMargins(0, 0, 0, 0);
    m_toolbarLayout->setSpacing(6);

    // 刷新按钮采用“图标+tooltip”模式，符合简短语义按钮图标化规范。
    m_refreshButton = new QPushButton(m_handleListPage);
    m_refreshButton->setIcon(QIcon(":/Icon/handle_refresh.svg"));
    m_refreshButton->setIconSize(QSize(16, 16));
    m_refreshButton->setFixedSize(28, 28);
    m_refreshButton->setToolTip(QStringLiteral("刷新句柄列表"));
    m_refreshButton->setStyleSheet(buildBlueButtonStyle(true));

    m_pidFilterEdit = new QLineEdit(m_handleListPage);
    m_pidFilterEdit->setPlaceholderText(QStringLiteral("PID 过滤（留空=全部）"));
    m_pidFilterEdit->setClearButtonEnabled(true);
    m_pidFilterEdit->setToolTip(QStringLiteral("输入目标进程 PID，仅展示该进程句柄。"));
    m_pidFilterEdit->setStyleSheet(buildLineEditStyle());

    m_keywordFilterEdit = new QLineEdit(m_handleListPage);
    m_keywordFilterEdit->setPlaceholderText(QStringLiteral("关键字（进程/类型/名称/句柄）"));
    m_keywordFilterEdit->setClearButtonEnabled(true);
    m_keywordFilterEdit->setToolTip(QStringLiteral("按进程名、对象类型、对象名、句柄值等关键字过滤。"));
    m_keywordFilterEdit->setStyleSheet(buildLineEditStyle());

    m_typeFilterCombo = new QComboBox(m_handleListPage);
    m_typeFilterCombo->setToolTip(QStringLiteral("按对象类型过滤（如 File/Key/Event）。"));
    m_typeFilterCombo->setStyleSheet(buildComboAndSpinStyle());
    m_typeFilterCombo->setMinimumWidth(260);
    m_typeFilterCombo->addItem(QStringLiteral("全部类型"));

    m_onlyNamedCheckBox = new QCheckBox(QStringLiteral("仅命名对象"), m_handleListPage);
    m_onlyNamedCheckBox->setToolTip(QStringLiteral("只显示对象名非空的句柄。"));
    m_onlyNamedCheckBox->setStyleSheet(
        QStringLiteral("QCheckBox{color:%1;font-weight:600;}").arg(KswordTheme::TextPrimaryHex()));

    m_resolveNameCheckBox = new QCheckBox(QStringLiteral("解析对象名"), m_handleListPage);
    m_resolveNameCheckBox->setChecked(true);
    m_resolveNameCheckBox->setToolTip(QStringLiteral("启用后会尝试解析对象名称（更耗时）。"));
    m_resolveNameCheckBox->setStyleSheet(
        QStringLiteral("QCheckBox{color:%1;font-weight:600;}").arg(KswordTheme::TextPrimaryHex()));

    m_nameBudgetSpinBox = new QSpinBox(m_handleListPage);
    m_nameBudgetSpinBox->setRange(0, 10000);
    m_nameBudgetSpinBox->setValue(1000);
    m_nameBudgetSpinBox->setSingleStep(100);
    m_nameBudgetSpinBox->setSuffix(QStringLiteral(" 条"));
    m_nameBudgetSpinBox->setToolTip(QStringLiteral("对象名解析预算，预算越大越接近全量解析。"));
    m_nameBudgetSpinBox->setStyleSheet(buildComboAndSpinStyle());

    m_toolbarLayout->addWidget(m_refreshButton);
    m_toolbarLayout->addWidget(m_pidFilterEdit, 0);
    m_toolbarLayout->addWidget(m_keywordFilterEdit, 1);
    m_toolbarLayout->addWidget(m_typeFilterCombo);
    m_toolbarLayout->addWidget(m_onlyNamedCheckBox);
    m_toolbarLayout->addWidget(m_resolveNameCheckBox);
    m_toolbarLayout->addWidget(m_nameBudgetSpinBox);

    m_statusLabel = new QLabel(QStringLiteral("● 等待首次刷新"), m_handleListPage);
    m_statusLabel->setStyleSheet(QStringLiteral("color:%1;font-weight:600;").arg(KswordTheme::TextSecondaryHex()));

    m_tableWidget = new QTreeWidget(m_handleListPage);
    initializeHandleTable();

    m_handleListLayout->addLayout(m_toolbarLayout);
    m_handleListLayout->addWidget(m_statusLabel);
    m_handleDetailStatusLabel = new QLabel(QStringLiteral("● 请选择一个句柄查看详情"), m_handleListPage);
    m_handleDetailStatusLabel->setStyleSheet(
        QStringLiteral("color:%1;font-weight:600;").arg(KswordTheme::TextSecondaryHex()));
    m_handleDetailTable = new QTreeWidget(m_handleListPage);
    m_handleDetailTable->setColumnCount(2);
    m_handleDetailTable->setHeaderLabels(QStringList{ QStringLiteral("字段"), QStringLiteral("值") });
    m_handleDetailTable->setRootIsDecorated(false);
    m_handleDetailTable->setItemsExpandable(false);
    m_handleDetailTable->setAlternatingRowColors(true);
    m_handleDetailTable->setSelectionMode(QAbstractItemView::NoSelection);
    m_handleDetailTable->setEditTriggers(QAbstractItemView::NoEditTriggers);
    if (m_handleDetailTable->header() != nullptr)
    {
        m_handleDetailTable->header()->setStyleSheet(buildHeaderStyle());
        m_handleDetailTable->header()->setSectionResizeMode(0, QHeaderView::ResizeToContents);
        m_handleDetailTable->header()->setSectionResizeMode(1, QHeaderView::Stretch);
    }

    m_handleListLayout->addWidget(m_tableWidget, 3);
    m_handleListLayout->addWidget(m_handleDetailStatusLabel);
    m_handleListLayout->addWidget(m_handleDetailTable, 2);

    m_tabWidget->addTab(m_handleListPage, QIcon(":/Icon/process_list.svg"), QStringLiteral("句柄列表"));
}

void HandleDock::initializeObjectTypeTab()
{
    m_objectTypePage = new QWidget(m_tabWidget);
    m_objectTypeLayout = new QVBoxLayout(m_objectTypePage);
    m_objectTypeLayout->setContentsMargins(6, 6, 6, 6);
    m_objectTypeLayout->setSpacing(6);

    m_objectTypeToolLayout = new QHBoxLayout();
    m_objectTypeToolLayout->setContentsMargins(0, 0, 0, 0);
    m_objectTypeToolLayout->setSpacing(6);

    m_refreshObjectTypeButton = new QPushButton(m_objectTypePage);
    m_refreshObjectTypeButton->setIcon(QIcon(":/Icon/handle_refresh.svg"));
    m_refreshObjectTypeButton->setIconSize(QSize(16, 16));
    m_refreshObjectTypeButton->setFixedSize(28, 28);
    m_refreshObjectTypeButton->setToolTip(QStringLiteral("刷新对象类型快照"));
    m_refreshObjectTypeButton->setStyleSheet(buildBlueButtonStyle(true));

    m_objectTypeFilterEdit = new QLineEdit(m_objectTypePage);
    m_objectTypeFilterEdit->setPlaceholderText(QStringLiteral("对象类型过滤（类型名或编号）"));
    m_objectTypeFilterEdit->setClearButtonEnabled(true);
    m_objectTypeFilterEdit->setToolTip(QStringLiteral("输入类型名或编号，过滤对象类型表。"));
    m_objectTypeFilterEdit->setStyleSheet(buildLineEditStyle());

    m_objectTypeStatusLabel = new QLabel(QStringLiteral("● 等待首次刷新"), m_objectTypePage);
    m_objectTypeStatusLabel->setStyleSheet(
        QStringLiteral("color:%1;font-weight:600;").arg(KswordTheme::TextSecondaryHex()));

    m_objectTypeToolLayout->addWidget(m_refreshObjectTypeButton);
    m_objectTypeToolLayout->addWidget(m_objectTypeFilterEdit, 1);
    m_objectTypeToolLayout->addWidget(m_objectTypeStatusLabel);

    m_objectTypeTable = new QTreeWidget(m_objectTypePage);
    m_objectTypeDetailTable = new QTreeWidget(m_objectTypePage);
    initializeObjectTypeTable();

    m_objectTypeLayout->addLayout(m_objectTypeToolLayout);
    m_objectTypeLayout->addWidget(m_objectTypeTable, 3);
    m_objectTypeLayout->addWidget(m_objectTypeDetailTable, 2);

    m_tabWidget->addTab(m_objectTypePage, QIcon(":/Icon/process_tree.svg"), QStringLiteral("对象类型"));
}

void HandleDock::initializeHandleTable()
{
    const QStringList headers{
        QStringLiteral("PID"),
        QStringLiteral("进程名"),
        QStringLiteral("句柄"),
        QStringLiteral("TypeIndex/类型"),
        QStringLiteral("对象名"),
        QStringLiteral("对象地址"),
        QStringLiteral("访问掩码"),
        QStringLiteral("属性"),
        QStringLiteral("HandleCount"),
        QStringLiteral("PointerCount")
    };

    m_tableWidget->setColumnCount(static_cast<int>(HandleTableColumn::Count));
    m_tableWidget->setHeaderLabels(headers);
    m_tableWidget->setRootIsDecorated(false);
    m_tableWidget->setItemsExpandable(false);
    m_tableWidget->setAlternatingRowColors(true);
    m_tableWidget->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_tableWidget->setSelectionMode(QAbstractItemView::SingleSelection);
    m_tableWidget->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_tableWidget->setSortingEnabled(true);
    m_tableWidget->setContextMenuPolicy(Qt::CustomContextMenu);

    QHeaderView* headerView = m_tableWidget->header();
    if (headerView != nullptr)
    {
        headerView->setStyleSheet(buildHeaderStyle());
        headerView->setSectionResizeMode(QHeaderView::Interactive);
        headerView->setStretchLastSection(false);
        headerView->setContextMenuPolicy(Qt::CustomContextMenu);
    }
    m_tableWidget->setColumnWidth(static_cast<int>(HandleTableColumn::ProcessId), 80);
    m_tableWidget->setColumnWidth(static_cast<int>(HandleTableColumn::ProcessName), 170);
    m_tableWidget->setColumnWidth(static_cast<int>(HandleTableColumn::HandleValue), 100);
    m_tableWidget->setColumnWidth(static_cast<int>(HandleTableColumn::TypeIndex), 180);
    m_tableWidget->setColumnWidth(static_cast<int>(HandleTableColumn::ObjectName), 360);
    m_tableWidget->setColumnWidth(static_cast<int>(HandleTableColumn::ObjectAddress), 130);
    m_tableWidget->setColumnWidth(static_cast<int>(HandleTableColumn::GrantedAccess), 120);
    m_tableWidget->setColumnWidth(static_cast<int>(HandleTableColumn::Attributes), 120);
    m_tableWidget->setColumnWidth(static_cast<int>(HandleTableColumn::HandleCount), 105);
    m_tableWidget->setColumnWidth(static_cast<int>(HandleTableColumn::PointerCount), 110);
}

void HandleDock::initializeObjectTypeTable()
{
    const QStringList headers{
        QStringLiteral("类型编号"),
        QStringLiteral("类型名"),
        QStringLiteral("对象数"),
        QStringLiteral("句柄数"),
        QStringLiteral("访问掩码"),
        QStringLiteral("安全要求"),
        QStringLiteral("维护计数")
    };

    m_objectTypeTable->setColumnCount(static_cast<int>(ObjectTypeTableColumn::Count));
    m_objectTypeTable->setHeaderLabels(headers);
    m_objectTypeTable->setRootIsDecorated(false);
    m_objectTypeTable->setItemsExpandable(false);
    m_objectTypeTable->setAlternatingRowColors(true);
    m_objectTypeTable->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_objectTypeTable->setSelectionMode(QAbstractItemView::SingleSelection);
    m_objectTypeTable->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_objectTypeTable->setSortingEnabled(false);
    QHeaderView* typeHeader = m_objectTypeTable->header();
    if (typeHeader != nullptr)
    {
        typeHeader->setStyleSheet(buildHeaderStyle());
        typeHeader->setSectionResizeMode(QHeaderView::ResizeToContents);
        typeHeader->setSectionResizeMode(static_cast<int>(ObjectTypeTableColumn::TypeName), QHeaderView::Stretch);
    }

    // 对象类型详情区使用键值树，方便快速浏览全部字段。
    m_objectTypeDetailTable->setColumnCount(2);
    m_objectTypeDetailTable->setHeaderLabels(QStringList{ QStringLiteral("字段"), QStringLiteral("值") });
    m_objectTypeDetailTable->setRootIsDecorated(false);
    m_objectTypeDetailTable->setItemsExpandable(false);
    m_objectTypeDetailTable->setAlternatingRowColors(true);
    m_objectTypeDetailTable->setSelectionMode(QAbstractItemView::NoSelection);
    m_objectTypeDetailTable->setEditTriggers(QAbstractItemView::NoEditTriggers);
    QHeaderView* detailHeader = m_objectTypeDetailTable->header();
    if (detailHeader != nullptr)
    {
        detailHeader->setStyleSheet(buildHeaderStyle());
        detailHeader->setSectionResizeMode(0, QHeaderView::ResizeToContents);
        detailHeader->setSectionResizeMode(1, QHeaderView::Stretch);
    }
}

void HandleDock::initializeConnections()
{
    connect(m_refreshButton, &QPushButton::clicked, this, [this]()
        {
            requestAsyncRefresh(true);
        });

    connect(m_pidFilterEdit, &QLineEdit::returnPressed, this, [this]()
        {
            applyLocalHandleFilters();
        });

    connect(m_keywordFilterEdit, &QLineEdit::returnPressed, this, [this]()
        {
            applyLocalHandleFilters();
        });

    connect(m_typeFilterCombo, &QComboBox::currentTextChanged, this, [this](const QString&)
        {
            applyLocalHandleFilters();
        });

    connect(m_onlyNamedCheckBox, &QCheckBox::toggled, this, [this](const bool)
        {
            applyLocalHandleFilters();
        });

    connect(m_resolveNameCheckBox, &QCheckBox::toggled, this, [this](const bool)
        {
            requestAsyncRefresh(false);
        });

    connect(m_nameBudgetSpinBox, &QSpinBox::valueChanged, this, [this](const int)
        {
            requestAsyncRefresh(false);
        });

    connect(m_tableWidget, &QTreeWidget::customContextMenuRequested, this, [this](const QPoint& localPoint)
        {
            showHandleTableContextMenu(localPoint);
        });

    connect(m_tableWidget, &QTreeWidget::currentItemChanged, this, [this](QTreeWidgetItem*, QTreeWidgetItem*)
        {
            requestHandleDetailRefresh(false);
        });

    if (m_tableWidget->header() != nullptr)
    {
        connect(m_tableWidget->header(), &QHeaderView::customContextMenuRequested, this, [this](const QPoint& localPoint)
            {
                showHandleHeaderContextMenu(localPoint);
            });
    }

    connect(m_refreshObjectTypeButton, &QPushButton::clicked, this, [this]()
        {
            requestObjectTypeRefreshAsync(true);
        });

    connect(m_objectTypeFilterEdit, &QLineEdit::textChanged, this, [this](const QString& filterKeyword)
        {
            rebuildObjectTypeTable(filterKeyword.trimmed());
        });

    connect(m_objectTypeTable, &QTreeWidget::currentItemChanged, this, [this](QTreeWidgetItem*, QTreeWidgetItem*)
        {
            showObjectTypeDetailByCurrentRow();
        });
}

void HandleDock::requestAsyncRefresh(const bool forceRefresh)
{
    if (m_refreshInProgress)
    {
        if (forceRefresh)
        {
            m_refreshPending = true;
        }
        return;
    }

    // 若对象类型映射尚未就绪，先触发对象类型刷新，再继续句柄刷新。
    if (m_typeNameMapByIndexFromObjectTab.empty() && !m_objectTypeRefreshInProgress)
    {
        requestObjectTypeRefreshAsync(false);
    }

    const HandleRefreshOptions options = collectHandleRefreshOptions();
    const std::uint64_t currentTicket = ++m_refreshTicket;
    m_refreshInProgress = true;
    updateHandleStatusLabel(QStringLiteral("● 正在刷新句柄列表..."), true);

    if (m_refreshProgressPid <= 0)
    {
        m_refreshProgressPid = kPro.add("句柄枚举", "准备读取系统句柄快照");
    }
    kPro.set(m_refreshProgressPid, "后台枚举系统句柄", 0, 20.0f);

    kLogEvent refreshEvent;
    info << refreshEvent
        << "[HandleDock] requestAsyncRefresh: ticket="
        << currentTicket
        << ", pidFilter="
        << (options.hasPidFilter ? std::to_string(options.pidFilter) : std::string("all"))
        << ", keyword="
        << options.keywordText.toStdString()
        << ", typeFilter="
        << options.typeFilterText.toStdString()
        << ", onlyNamed="
        << (options.onlyNamed ? "true" : "false")
        << ", resolveName="
        << (options.resolveObjectName ? "true" : "false")
        << ", nameBudget="
        << options.nameResolveBudget
        << ", objectTypeMapSize="
        << options.typeNameMapFromObjectTab.size()
        << eol;

    QPointer<HandleDock> guardThis(this);
    auto* refreshTask = QRunnable::create([guardThis, currentTicket, options]()
        {
            const HandleRefreshResult refreshResult = buildHandleRefreshResult(options);
            if (guardThis == nullptr)
            {
                return;
            }
            QMetaObject::invokeMethod(
                guardThis,
                [guardThis, currentTicket, refreshResult]()
                {
                    if (guardThis == nullptr)
                    {
                        return;
                    }
                    guardThis->applyHandleRefreshResult(currentTicket, refreshResult);
                },
                Qt::QueuedConnection);
        });
    refreshTask->setAutoDelete(true);
    QThreadPool::globalInstance()->start(refreshTask);
}

void HandleDock::requestObjectTypeRefreshAsync(const bool forceRefresh)
{
    if (m_objectTypeRefreshInProgress)
    {
        if (forceRefresh)
        {
            m_objectTypeRefreshPending = true;
        }
        return;
    }

    const std::uint64_t currentTicket = ++m_objectTypeRefreshTicket;
    m_objectTypeRefreshInProgress = true;
    updateObjectTypeStatusLabel(QStringLiteral("● 正在刷新对象类型..."), true);

    if (m_objectTypeRefreshProgressPid <= 0)
    {
        m_objectTypeRefreshProgressPid = kPro.add("对象类型", "准备读取对象类型快照");
    }
    kPro.set(m_objectTypeRefreshProgressPid, "后台采集对象类型", 0, 20.0f);

    QPointer<HandleDock> guardThis(this);
    auto* refreshTask = QRunnable::create([guardThis, currentTicket]()
        {
            const ObjectTypeRefreshResult refreshResult = buildObjectTypeRefreshResult();
            if (guardThis == nullptr)
            {
                return;
            }
            QMetaObject::invokeMethod(
                guardThis,
                [guardThis, currentTicket, refreshResult]()
                {
                    if (guardThis == nullptr)
                    {
                        return;
                    }
                    guardThis->applyObjectTypeRefreshResult(currentTicket, refreshResult);
                },
                Qt::QueuedConnection);
        });
    refreshTask->setAutoDelete(true);
    QThreadPool::globalInstance()->start(refreshTask);
}

void HandleDock::applyHandleRefreshResult(
    const std::uint64_t refreshTicket,
    const HandleRefreshResult& refreshResult)
{
    if (refreshTicket < m_refreshTicket)
    {
        return;
    }

    m_allRows = refreshResult.rows;
    m_typeNameCacheByIndex = refreshResult.updatedTypeNameCacheByIndex;
    refreshTypeFilterItemsFromAllRows();
    applyLocalHandleFilters();

    QString statusText = QStringLiteral(
        "● 刷新完成 %1 ms | 总句柄:%2 | 显示:%3 | 计数已解析:%4 | 名称已解析:%5 | 名称回退:%6 | 类型映射命中:%7")
        .arg(refreshResult.elapsedMs)
        .arg(refreshResult.totalHandleCount)
        .arg(m_rows.size())
        .arg(refreshResult.basicInfoResolvedCount)
        .arg(refreshResult.resolvedNameCount)
        .arg(refreshResult.fallbackNameCount)
        .arg(refreshResult.objectTypeMappedCount);
    if (!refreshResult.diagnosticText.trimmed().isEmpty())
    {
        statusText += QStringLiteral(" | %1").arg(refreshResult.diagnosticText);
    }
    updateHandleStatusLabel(statusText, false);

    m_refreshInProgress = false;
    kPro.set(m_refreshProgressPid, "句柄刷新完成", 0, 100.0f);

    kLogEvent refreshDoneEvent;
    info << refreshDoneEvent
        << "[HandleDock] applyHandleRefreshResult: ticket="
        << refreshTicket
        << ", total="
        << refreshResult.totalHandleCount
        << ", visible="
        << refreshResult.visibleHandleCount
        << ", mapped="
        << refreshResult.objectTypeMappedCount
        << ", elapsedMs="
        << refreshResult.elapsedMs
        << ", diagnostic="
        << refreshResult.diagnosticText.toStdString()
        << eol;

    if (m_refreshPending)
    {
        m_refreshPending = false;
        QMetaObject::invokeMethod(this, [this]()
            {
                requestAsyncRefresh(true);
            }, Qt::QueuedConnection);
    }
}

void HandleDock::applyObjectTypeRefreshResult(
    const std::uint64_t refreshTicket,
    const ObjectTypeRefreshResult& refreshResult)
{
    if (refreshTicket < m_objectTypeRefreshTicket)
    {
        return;
    }

    m_objectTypeRows = refreshResult.rows;
    m_typeNameMapByIndexFromObjectTab = refreshResult.typeNameMapByIndex;
    rebuildObjectTypeTable(m_objectTypeFilterEdit->text().trimmed());

    QString statusText = QStringLiteral("● 刷新完成 %1 ms | 类型数:%2")
        .arg(refreshResult.elapsedMs)
        .arg(refreshResult.rows.size());
    if (!refreshResult.diagnosticText.trimmed().isEmpty())
    {
        statusText += QStringLiteral(" | %1").arg(refreshResult.diagnosticText);
    }
    updateObjectTypeStatusLabel(statusText, false);

    m_objectTypeRefreshInProgress = false;
    kPro.set(m_objectTypeRefreshProgressPid, "对象类型刷新完成", 0, 100.0f);
    syncHandleTypeNamesFromObjectTypeMap();

    if (m_objectTypeRefreshPending)
    {
        m_objectTypeRefreshPending = false;
        QMetaObject::invokeMethod(this, [this]()
            {
                requestObjectTypeRefreshAsync(true);
            }, Qt::QueuedConnection);
    }
}

void HandleDock::rebuildHandleTable()
{
    m_tableWidget->clear();
    const std::size_t totalRowCount = m_rows.size();
    HandleRenderSplashScope renderSplashScope(totalRowCount);

    for (std::size_t rowIndex = 0; rowIndex < m_rows.size(); ++rowIndex)
    {
        const HandleRow& row = m_rows[rowIndex];
        auto* item = new QTreeWidgetItem();
        item->setText(static_cast<int>(HandleTableColumn::ProcessId), QString::number(row.processId));
        item->setText(static_cast<int>(HandleTableColumn::ProcessName), row.processName);
        item->setIcon(
            static_cast<int>(HandleTableColumn::ProcessName),
            resolveProcessIconByPid(row.processId));
        item->setText(static_cast<int>(HandleTableColumn::HandleValue), formatHex(row.handleValue, 0));
        item->setText(
            static_cast<int>(HandleTableColumn::TypeIndex),
            formatTypeIndexDisplayText(row.typeIndex, row.typeName));
        const QString objectNameDisplayText = formatObjectNameDisplayText(row);
        item->setText(static_cast<int>(HandleTableColumn::ObjectName), objectNameDisplayText);
        item->setText(static_cast<int>(HandleTableColumn::ObjectAddress), formatHex(row.objectAddress, 0));
        item->setText(static_cast<int>(HandleTableColumn::GrantedAccess), formatHex(row.grantedAccess, 8));
        item->setText(static_cast<int>(HandleTableColumn::Attributes), formatHandleAttributes(row.attributes));
        item->setText(
            static_cast<int>(HandleTableColumn::HandleCount),
            formatOptionalObjectCount(row.handleCount, row.basicInfoAvailable));
        item->setText(
            static_cast<int>(HandleTableColumn::PointerCount),
            formatOptionalObjectCount(row.pointerCount, row.basicInfoAvailable));
        item->setData(static_cast<int>(HandleTableColumn::ProcessId), Qt::UserRole, static_cast<qulonglong>(rowIndex));
        item->setToolTip(
            static_cast<int>(HandleTableColumn::GrantedAccess),
            decodeGrantedAccessText(row.typeName, row.grantedAccess));

        // 占位状态统一弱化显示，并附带 tooltip 解释来源，避免用户把“无名称”和“未查到”误看成同一种状态。
        if (!row.objectNameAvailable || row.objectName.trimmed().isEmpty())
        {
            const QColor secondaryTextColor = KswordTheme::IsDarkModeEnabled()
                ? QColor(178, 178, 178)
                : QColor(92, 92, 92);
            item->setForeground(
                static_cast<int>(HandleTableColumn::ObjectName),
                secondaryTextColor);
        }
        if (!row.basicInfoAvailable)
        {
            const QColor secondaryTextColor = KswordTheme::IsDarkModeEnabled()
                ? QColor(178, 178, 178)
                : QColor(92, 92, 92);
            item->setForeground(static_cast<int>(HandleTableColumn::HandleCount), secondaryTextColor);
            item->setForeground(static_cast<int>(HandleTableColumn::PointerCount), secondaryTextColor);
            item->setToolTip(static_cast<int>(HandleTableColumn::HandleCount), QStringLiteral("ObjectBasicInformation 未查到。"));
            item->setToolTip(static_cast<int>(HandleTableColumn::PointerCount), QStringLiteral("ObjectBasicInformation 未查到。"));
        }
        if (row.objectNameAvailable)
        {
            if (row.objectName.trimmed().isEmpty())
            {
                item->setToolTip(static_cast<int>(HandleTableColumn::ObjectName), QStringLiteral("对象已查询，但该对象没有名称。"));
            }
            else if (row.objectNameFromFallback)
            {
                item->setToolTip(static_cast<int>(HandleTableColumn::ObjectName), QStringLiteral("对象名来自类型专用回退查询。"));
            }
        }
        else if (row.objectNameFailed)
        {
            item->setToolTip(static_cast<int>(HandleTableColumn::ObjectName), QStringLiteral("对象名查询失败。"));
        }
        else
        {
            item->setToolTip(static_cast<int>(HandleTableColumn::ObjectName), QStringLiteral("对象名未查询，可能受预算、类型白名单或开关限制。"));
        }
        m_tableWidget->addTopLevelItem(item);

        if (totalRowCount > 0
            && (((rowIndex + 1) % kHandleRenderProgressStep) == 0 || (rowIndex + 1) == totalRowCount))
        {
            renderSplashScope.update(rowIndex + 1);
        }
    }

    renderSplashScope.finish();
}


void HandleDock::rebuildObjectTypeTable(const QString& filterKeyword)
{
    m_objectTypeTable->clear();
    std::size_t visibleCount = 0;
    for (std::size_t sourceIndex = 0; sourceIndex < m_objectTypeRows.size(); ++sourceIndex)
    {
        const HandleObjectTypeEntry& row = m_objectTypeRows[sourceIndex];
        const bool matched = filterKeyword.trimmed().isEmpty()
            || row.typeNameText.contains(filterKeyword, Qt::CaseInsensitive)
            || QString::number(row.typeIndex).contains(filterKeyword, Qt::CaseInsensitive);
        if (!matched)
        {
            continue;
        }

        auto* item = new QTreeWidgetItem();
        item->setText(static_cast<int>(ObjectTypeTableColumn::TypeIndex), QString::number(row.typeIndex));
        item->setText(static_cast<int>(ObjectTypeTableColumn::TypeName), row.typeNameText);
        item->setText(static_cast<int>(ObjectTypeTableColumn::ObjectCount), QString::number(row.totalObjectCount));
        item->setText(static_cast<int>(ObjectTypeTableColumn::HandleCount), QString::number(row.totalHandleCount));
        item->setText(static_cast<int>(ObjectTypeTableColumn::AccessMask), formatHex(row.validAccessMask, 0));
        item->setText(static_cast<int>(ObjectTypeTableColumn::SecurityRequired), boolText(row.securityRequired));
        item->setText(static_cast<int>(ObjectTypeTableColumn::MaintainCount), boolText(row.maintainHandleCount));
        item->setData(static_cast<int>(ObjectTypeTableColumn::TypeIndex), Qt::UserRole, static_cast<qulonglong>(sourceIndex));
        m_objectTypeTable->addTopLevelItem(item);
        ++visibleCount;
    }

    if (m_objectTypeTable->topLevelItemCount() > 0)
    {
        m_objectTypeTable->setCurrentItem(m_objectTypeTable->topLevelItem(0));
    }
    else
    {
        m_objectTypeDetailTable->clear();
    }

    kLogEvent rebuildTypeEvent;
    dbg << rebuildTypeEvent
        << "[HandleDock] rebuildObjectTypeTable: total="
        << m_objectTypeRows.size()
        << ", visible="
        << visibleCount
        << ", filter="
        << filterKeyword.toStdString()
        << eol;
}

HandleDock::HandleRefreshOptions HandleDock::collectHandleRefreshOptions() const
{
    HandleRefreshOptions options{};
    options.keywordText = m_keywordFilterEdit->text().trimmed().toLower();
    options.typeFilterText = m_typeFilterCombo->currentText().trimmed();
    options.onlyNamed = m_onlyNamedCheckBox->isChecked();
    options.resolveObjectName = m_resolveNameCheckBox->isChecked();
    options.nameResolveBudget = m_nameBudgetSpinBox->value();
    options.typeNameCacheByIndex = m_typeNameCacheByIndex;
    options.typeNameMapFromObjectTab = m_typeNameMapByIndexFromObjectTab;

    if (options.onlyNamed)
    {
        options.resolveObjectName = true;
    }

    const QString pidText = m_pidFilterEdit->text().trimmed();
    bool parseOk = false;
    const std::uint32_t parsedPid = pidText.toUInt(&parseOk, 10);
    if (!pidText.isEmpty() && parseOk && parsedPid > 0)
    {
        options.hasPidFilter = true;
        options.pidFilter = parsedPid;
    }

    return options;
}
