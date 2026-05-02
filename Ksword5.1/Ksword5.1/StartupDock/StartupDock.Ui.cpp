#include "StartupDock.Internal.h"

#include <QColor>

using namespace startup_dock_detail;

namespace
{
    // kToolbarIconSize 作用：
    // - 统一启动项页顶部图标按钮尺寸。
    constexpr QSize kToolbarIconSize(16, 16);

    // kUntrustedRowHighlightColor 作用：
    // - 定义不受信任启动项整行背景色；
    // - 采用半透明红色，确保风险可见且不遮挡文本。
    const QColor kUntrustedRowHighlightColor(255, 64, 64, 76);

    // isUntrustedStartupEntry 作用：
    // - 判断某条启动项是否属于不受信任目标；
    // - 调用方法：在行渲染前传入 StartupEntry；
    // - 传入参数 entry：当前待渲染的启动项数据；
    // - 返回值 true：需要按风险项做红色高亮，false：保持普通样式。
    bool isUntrustedStartupEntry(const StartupDock::StartupEntry& entry)
    {
        return entry.publisherText.contains(QStringLiteral("(Untrusted)"), Qt::CaseInsensitive);
    }

    // createStartupTable 作用：
    // - 创建统一列结构的启动项表格；
    // - 供六个分类页复用。
    QTableWidget* createStartupTable(QWidget* parentWidget)
    {
        QTableWidget* tableWidget = new QTableWidget(parentWidget);
        tableWidget->setColumnCount(StartupDock::toStartupColumn(StartupDock::StartupColumn::Count));
        tableWidget->setHorizontalHeaderLabels({
            QStringLiteral("名称"),
            QStringLiteral("发布者"),
            QStringLiteral("镜像路径"),
            QStringLiteral("命令"),
            QStringLiteral("来源位置"),
            QStringLiteral("用户"),
            QStringLiteral("状态"),
            QStringLiteral("类型"),
            QStringLiteral("详情")
            });
        tableWidget->setSelectionBehavior(QAbstractItemView::SelectRows);
        tableWidget->setSelectionMode(QAbstractItemView::SingleSelection);
        tableWidget->setEditTriggers(QAbstractItemView::NoEditTriggers);
        tableWidget->setContextMenuPolicy(Qt::CustomContextMenu);
        tableWidget->setWordWrap(false);
        tableWidget->verticalHeader()->setVisible(false);
        tableWidget->horizontalHeader()->setSectionResizeMode(QHeaderView::Interactive);
        tableWidget->horizontalHeader()->setSectionResizeMode(
            StartupDock::toStartupColumn(StartupDock::StartupColumn::Detail),
            QHeaderView::Stretch);
        tableWidget->setColumnWidth(StartupDock::toStartupColumn(StartupDock::StartupColumn::Name), 180);
        tableWidget->setColumnWidth(StartupDock::toStartupColumn(StartupDock::StartupColumn::Publisher), 170);
        tableWidget->setColumnWidth(StartupDock::toStartupColumn(StartupDock::StartupColumn::ImagePath), 260);
        tableWidget->setColumnWidth(StartupDock::toStartupColumn(StartupDock::StartupColumn::Command), 300);
        tableWidget->setColumnWidth(StartupDock::toStartupColumn(StartupDock::StartupColumn::Location), 260);
        tableWidget->setColumnWidth(StartupDock::toStartupColumn(StartupDock::StartupColumn::User), 120);
        tableWidget->setColumnWidth(StartupDock::toStartupColumn(StartupDock::StartupColumn::Enabled), 70);
        tableWidget->setColumnWidth(StartupDock::toStartupColumn(StartupDock::StartupColumn::Type), 110);
        return tableWidget;
    }

    // createSingleTablePage 作用：
    // - 创建只承载单张表格的标签页容器。
    QWidget* createSingleTablePage(QTableWidget** tableOut, QWidget* parentWidget)
    {
        QWidget* pageWidget = new QWidget(parentWidget);
        QVBoxLayout* pageLayout = new QVBoxLayout(pageWidget);
        pageLayout->setContentsMargins(4, 4, 4, 4);
        pageLayout->setSpacing(4);
        QTableWidget* tableWidget = createStartupTable(pageWidget);
        pageLayout->addWidget(tableWidget, 1);
        if (tableOut != nullptr)
        {
            *tableOut = tableWidget;
        }
        return pageWidget;
    }

    // createStartupTree 作用：
    // - 创建高级注册表页专用树控件；
    // - 一级节点对应注册表位置，二级节点对应具体启动项。
    QTreeWidget* createStartupTree(QWidget* parentWidget)
    {
        QTreeWidget* treeWidget = new QTreeWidget(parentWidget);
        treeWidget->setColumnCount(StartupDock::toStartupColumn(StartupDock::StartupColumn::Count));
        treeWidget->setHeaderLabels({
            QStringLiteral("名称"),
            QStringLiteral("发布者"),
            QStringLiteral("镜像路径"),
            QStringLiteral("命令"),
            QStringLiteral("来源位置"),
            QStringLiteral("用户"),
            QStringLiteral("状态"),
            QStringLiteral("类型"),
            QStringLiteral("详情")
            });
        treeWidget->setSelectionMode(QAbstractItemView::SingleSelection);
        treeWidget->setSelectionBehavior(QAbstractItemView::SelectRows);
        treeWidget->setContextMenuPolicy(Qt::CustomContextMenu);
        treeWidget->setWordWrap(false);
        treeWidget->setRootIsDecorated(true);
        treeWidget->setAlternatingRowColors(true);
        treeWidget->setUniformRowHeights(true);
        treeWidget->header()->setSectionResizeMode(QHeaderView::Interactive);
        treeWidget->header()->setSectionResizeMode(
            StartupDock::toStartupColumn(StartupDock::StartupColumn::Detail),
            QHeaderView::Stretch);
        treeWidget->setColumnWidth(StartupDock::toStartupColumn(StartupDock::StartupColumn::Name), 260);
        treeWidget->setColumnWidth(StartupDock::toStartupColumn(StartupDock::StartupColumn::Publisher), 170);
        treeWidget->setColumnWidth(StartupDock::toStartupColumn(StartupDock::StartupColumn::ImagePath), 260);
        treeWidget->setColumnWidth(StartupDock::toStartupColumn(StartupDock::StartupColumn::Command), 280);
        treeWidget->setColumnWidth(StartupDock::toStartupColumn(StartupDock::StartupColumn::Location), 280);
        treeWidget->setColumnWidth(StartupDock::toStartupColumn(StartupDock::StartupColumn::User), 120);
        treeWidget->setColumnWidth(StartupDock::toStartupColumn(StartupDock::StartupColumn::Enabled), 70);
        treeWidget->setColumnWidth(StartupDock::toStartupColumn(StartupDock::StartupColumn::Type), 130);
        return treeWidget;
    }

    // createRegistryTreePage 作用：
    // - 创建高级注册表页；
    // - 页内唯一主控件为按注册表位置分组的树。
    QWidget* createRegistryTreePage(QTreeWidget** treeOut, QWidget* parentWidget)
    {
        QWidget* pageWidget = new QWidget(parentWidget);
        QVBoxLayout* pageLayout = new QVBoxLayout(pageWidget);
        pageLayout->setContentsMargins(4, 4, 4, 4);
        pageLayout->setSpacing(4);
        QTreeWidget* treeWidget = createStartupTree(pageWidget);
        pageLayout->addWidget(treeWidget, 1);
        if (treeOut != nullptr)
        {
            *treeOut = treeWidget;
        }
        return pageWidget;
    }
}

void StartupDock::initializeUi()
{
    m_rootLayout = new QVBoxLayout(this);
    m_rootLayout->setContentsMargins(4, 4, 4, 4);
    m_rootLayout->setSpacing(6);

    initializeToolbar();
    initializeTabs();

    m_rootLayout->addWidget(m_toolbarWidget, 0);
    m_rootLayout->addWidget(m_sideTabWidget, 1);
}

void StartupDock::initializeToolbar()
{
    m_toolbarWidget = new QWidget(this);
    m_toolbarLayout = new QHBoxLayout(m_toolbarWidget);
    m_toolbarLayout->setContentsMargins(0, 0, 0, 0);
    m_toolbarLayout->setSpacing(6);

    m_refreshButton = new QPushButton(m_toolbarWidget);
    m_refreshButton->setIcon(createBlueIcon(":/Icon/process_refresh.svg", kToolbarIconSize));
    m_refreshButton->setToolTip(QStringLiteral("刷新当前启动项视图"));
    m_refreshButton->setFixedSize(28, 28);

    m_exportButton = new QPushButton(m_toolbarWidget);
    m_exportButton->setIcon(createBlueIcon(":/Icon/log_export.svg", kToolbarIconSize));
    m_exportButton->setToolTip(QStringLiteral("导出当前视图为制表符文本"));
    m_exportButton->setFixedSize(28, 28);

    m_copyButton = new QPushButton(m_toolbarWidget);
    m_copyButton->setIcon(createBlueIcon(":/Icon/log_copy.svg", kToolbarIconSize));
    m_copyButton->setToolTip(QStringLiteral("复制当前选中启动项"));
    m_copyButton->setFixedSize(28, 28);

    m_filterEdit = new QLineEdit(m_toolbarWidget);
    m_filterEdit->setPlaceholderText(QStringLiteral("过滤名称/发布者/路径/位置"));
    m_filterEdit->setToolTip(QStringLiteral("按名称、发布者、路径、位置和类型做模糊筛选。"));

    m_hideMicrosoftCheck = new QCheckBox(QStringLiteral("隐藏微软项"), m_toolbarWidget);
    m_hideMicrosoftCheck->setToolTip(QStringLiteral("隐藏发布者包含 Microsoft/Windows 的条目。"));

    m_hideEmptyPathCheck = new QCheckBox(QStringLiteral("隐藏空路径"), m_toolbarWidget);
    m_hideEmptyPathCheck->setToolTip(QStringLiteral("在高级注册表树中隐藏当前没有任何条目的注册表位置。"));

    m_statusLabel = new QLabel(QStringLiteral("状态：首次打开该页时加载启动项"), m_toolbarWidget);
    m_statusLabel->setWordWrap(true);
    m_statusLabel->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);

    m_toolbarLayout->addWidget(m_refreshButton);
    m_toolbarLayout->addWidget(m_exportButton);
    m_toolbarLayout->addWidget(m_copyButton);
    m_toolbarLayout->addWidget(m_filterEdit, 1);
    m_toolbarLayout->addWidget(m_hideMicrosoftCheck);
    m_toolbarLayout->addWidget(m_hideEmptyPathCheck);
    m_toolbarLayout->addWidget(m_statusLabel, 1);
}

void StartupDock::initializeTabs()
{
    m_sideTabWidget = new QTabWidget(this);
    m_sideTabWidget->setTabPosition(QTabWidget::West);

    m_allPage = createSingleTablePage(&m_allTable, m_sideTabWidget);
    m_logonPage = createSingleTablePage(&m_logonTable, m_sideTabWidget);
    m_servicesPage = createSingleTablePage(&m_servicesTable, m_sideTabWidget);
    m_driversPage = createSingleTablePage(&m_driversTable, m_sideTabWidget);
    m_tasksPage = createSingleTablePage(&m_tasksTable, m_sideTabWidget);
    m_registryPage = createRegistryTreePage(&m_registryTree, m_sideTabWidget);
    m_wmiPage = createSingleTablePage(&m_wmiTable, m_sideTabWidget);

    m_sideTabWidget->addTab(m_allPage, QIcon(":/Icon/process_list.svg"), QStringLiteral("总览"));
    m_sideTabWidget->addTab(m_logonPage, QIcon(":/Icon/process_main.svg"), QStringLiteral("登录"));
    m_sideTabWidget->addTab(m_servicesPage, QIcon(":/Icon/process_start.svg"), QStringLiteral("服务"));
    m_sideTabWidget->addTab(m_driversPage, QIcon(":/Icon/process_details.svg"), QStringLiteral("驱动"));
    m_sideTabWidget->addTab(m_tasksPage, QIcon(":/Icon/process_refresh.svg"), QStringLiteral("计划任务"));
    m_sideTabWidget->addTab(m_registryPage, QIcon(":/Icon/file_find.svg"), QStringLiteral("高级注册表"));
    m_sideTabWidget->addTab(m_wmiPage, QIcon(":/Icon/process_tree.svg"), QStringLiteral("WMI"));
}

void StartupDock::rebuildAllTables()
{
    rebuildTableForCategory(StartupCategory::All, m_allTable);
    rebuildTableForCategory(StartupCategory::Logon, m_logonTable);
    rebuildTableForCategory(StartupCategory::Services, m_servicesTable);
    rebuildTableForCategory(StartupCategory::Drivers, m_driversTable);
    rebuildTableForCategory(StartupCategory::Tasks, m_tasksTable);
    rebuildRegistryTree();
    rebuildTableForCategory(StartupCategory::Wmi, m_wmiTable);
}

void StartupDock::rebuildTableForCategory(const StartupCategory category, QTableWidget* tableWidget)
{
    if (tableWidget == nullptr)
    {
        return;
    }

    tableWidget->setRowCount(0);

    // visibleEntryIndexList 用途：记录当前分类下、过滤后仍可见的缓存索引。
    std::vector<int> visibleEntryIndexList;
    visibleEntryIndexList.reserve(m_entryList.size());
    for (int index = 0; index < static_cast<int>(m_entryList.size()); ++index)
    {
        const StartupEntry& entry = m_entryList[static_cast<std::size_t>(index)];
        if (category != StartupCategory::All && entry.category != category)
        {
            continue;
        }
        if (!entryMatchesCurrentFilter(entry))
        {
            continue;
        }
        visibleEntryIndexList.push_back(index);
    }

    tableWidget->setRowCount(static_cast<int>(visibleEntryIndexList.size()));
    for (int rowIndex = 0; rowIndex < static_cast<int>(visibleEntryIndexList.size()); ++rowIndex)
    {
        const int entryIndex = visibleEntryIndexList[static_cast<std::size_t>(rowIndex)];
        appendEntryRow(
            tableWidget,
            rowIndex,
            m_entryList[static_cast<std::size_t>(entryIndex)],
            entryIndex);
    }
}

void StartupDock::appendEntryRow(
    QTableWidget* tableWidget,
    const int rowIndex,
    const StartupEntry& entry,
    const int entryIndex)
{
    if (tableWidget == nullptr || rowIndex < 0)
    {
        return;
    }

    QTableWidgetItem* nameItem = createReadOnlyItem(entry.itemNameText);
    nameItem->setData(Qt::UserRole, entryIndex);
    nameItem->setIcon(resolveEntryIcon(entry));
    tableWidget->setItem(rowIndex, toStartupColumn(StartupColumn::Name), nameItem);
    tableWidget->setItem(rowIndex, toStartupColumn(StartupColumn::Publisher), createReadOnlyItem(entry.publisherText));
    tableWidget->setItem(rowIndex, toStartupColumn(StartupColumn::ImagePath), createReadOnlyItem(entry.imagePathText));
    tableWidget->setItem(rowIndex, toStartupColumn(StartupColumn::Command), createReadOnlyItem(entry.commandText));
    tableWidget->setItem(rowIndex, toStartupColumn(StartupColumn::Location), createReadOnlyItem(entry.locationText));
    tableWidget->setItem(rowIndex, toStartupColumn(StartupColumn::User), createReadOnlyItem(entry.userText));
    tableWidget->setItem(rowIndex, toStartupColumn(StartupColumn::Enabled), createReadOnlyItem(buildStatusText(entry.enabled)));
    tableWidget->setItem(rowIndex, toStartupColumn(StartupColumn::Type), createReadOnlyItem(entry.sourceTypeText));
    tableWidget->setItem(rowIndex, toStartupColumn(StartupColumn::Detail), createReadOnlyItem(entry.detailText));

    // shouldHighlightUntrusted 用途：记录当前条目是否命中不受信任高亮条件。
    const bool shouldHighlightUntrusted = isUntrustedStartupEntry(entry);
    if (shouldHighlightUntrusted)
    {
        for (int columnIndex = 0; columnIndex < toStartupColumn(StartupColumn::Count); ++columnIndex)
        {
            // currentItem 用途：定位当前行每一列单元格并统一套用半透明红底。
            QTableWidgetItem* currentItem = tableWidget->item(rowIndex, columnIndex);
            if (currentItem != nullptr)
            {
                currentItem->setBackground(kUntrustedRowHighlightColor);
            }
        }
    }
}

void StartupDock::appendRegistryTreeLeaf(
    QTreeWidgetItem* parentItem,
    const StartupEntry& entry,
    const int entryIndex)
{
    if (parentItem == nullptr)
    {
        return;
    }

    QTreeWidgetItem* entryItem = new QTreeWidgetItem(parentItem);
    entryItem->setData(0, kStartupEntryIndexRole, entryIndex);
    entryItem->setData(0, kStartupTreeNodeKindRole, static_cast<int>(StartupTreeNodeKind::Entry));
    entryItem->setData(0, kStartupTreeLocationRole, entry.locationText);
    entryItem->setText(toStartupColumn(StartupColumn::Name), entry.itemNameText);
    entryItem->setText(toStartupColumn(StartupColumn::Publisher), entry.publisherText);
    entryItem->setText(toStartupColumn(StartupColumn::ImagePath), entry.imagePathText);
    entryItem->setText(toStartupColumn(StartupColumn::Command), entry.commandText);
    entryItem->setText(toStartupColumn(StartupColumn::Location), entry.locationText);
    entryItem->setText(toStartupColumn(StartupColumn::User), entry.userText);
    entryItem->setText(toStartupColumn(StartupColumn::Enabled), buildStatusText(entry.enabled));
    entryItem->setText(toStartupColumn(StartupColumn::Type), entry.sourceTypeText);
    entryItem->setText(toStartupColumn(StartupColumn::Detail), entry.detailText);
    entryItem->setIcon(toStartupColumn(StartupColumn::Name), resolveEntryIcon(entry));

    // shouldHighlightUntrusted 用途：树节点模式下沿用表格相同的不受信任着色规则。
    const bool shouldHighlightUntrusted = isUntrustedStartupEntry(entry);
    if (shouldHighlightUntrusted)
    {
        for (int columnIndex = 0; columnIndex < toStartupColumn(StartupColumn::Count); ++columnIndex)
        {
            entryItem->setBackground(columnIndex, kUntrustedRowHighlightColor);
        }
    }

    for (int columnIndex = 0; columnIndex < toStartupColumn(StartupColumn::Count); ++columnIndex)
    {
        entryItem->setToolTip(columnIndex, entryItem->text(columnIndex));
    }
}

bool StartupDock::isRegistryBackedStartupEntry(const StartupEntry& entry) const
{
    return entry.canOpenRegistryLocation
        && !entry.locationText.trimmed().isEmpty()
        && (entry.category == StartupCategory::Logon || entry.category == StartupCategory::Registry);
}

int StartupDock::findEntryIndexByRegistryTreeItem(const QTreeWidgetItem* treeItem) const
{
    if (treeItem == nullptr)
    {
        return -1;
    }

    const StartupTreeNodeKind nodeKind = static_cast<StartupTreeNodeKind>(
        treeItem->data(0, kStartupTreeNodeKindRole).toInt());
    if (nodeKind != StartupTreeNodeKind::Entry)
    {
        return -1;
    }
    return treeItem->data(0, kStartupEntryIndexRole).toInt();
}

void StartupDock::rebuildRegistryTree()
{
    if (m_registryTree == nullptr)
    {
        return;
    }

    m_registryTree->clear();

    // knownLocationList：注册表树一级节点清单，按预定义位置顺序创建节点。
    QStringList knownLocationList = buildKnownStartupRegistryLocationList();
    const bool hideEmptyPath = (m_hideEmptyPathCheck != nullptr) && m_hideEmptyPathCheck->isChecked();
    QHash<QString, std::vector<int>> totalEntryIndexMap;
    QHash<QString, std::vector<int>> visibleEntryIndexMap;

    for (int entryIndex = 0; entryIndex < static_cast<int>(m_entryList.size()); ++entryIndex)
    {
        const StartupEntry& entry = m_entryList[static_cast<std::size_t>(entryIndex)];
        if (!isRegistryBackedStartupEntry(entry))
        {
            continue;
        }

        const QString groupLocationText = entry.locationGroupText.trimmed().isEmpty()
            ? entry.locationText
            : entry.locationGroupText;
        totalEntryIndexMap[groupLocationText].push_back(entryIndex);
        if (!knownLocationList.contains(groupLocationText))
        {
            knownLocationList.push_back(groupLocationText);
        }
        if (entryMatchesCurrentFilter(entry))
        {
            visibleEntryIndexMap[groupLocationText].push_back(entryIndex);
        }
    }

    for (const QString& groupLocationText : knownLocationList)
    {
        const std::vector<int> totalEntryIndexList = totalEntryIndexMap.value(groupLocationText);
        const std::vector<int> visibleEntryIndexList = visibleEntryIndexMap.value(groupLocationText);
        if (hideEmptyPath && totalEntryIndexList.empty())
        {
            // hideEmptyPath 用途：按用户要求完全跳过无任何条目的注册表位置节点。
            // 这里必须在创建 QTreeWidgetItem 前返回，否则树里会残留没有文字的空白行。
            continue;
        }

        QTreeWidgetItem* groupItem = new QTreeWidgetItem(m_registryTree);
        groupItem->setData(0, kStartupEntryIndexRole, -1);
        groupItem->setData(0, kStartupTreeNodeKindRole, static_cast<int>(StartupTreeNodeKind::Group));
        groupItem->setData(0, kStartupTreeLocationRole, groupLocationText);
        groupItem->setFirstColumnSpanned(true);
        groupItem->setIcon(toStartupColumn(StartupColumn::Name), createBlueIcon(":/Icon/file_find.svg"));

        if (!visibleEntryIndexList.empty())
        {
            groupItem->setText(
                toStartupColumn(StartupColumn::Name),
                QStringLiteral("%1    匹配 %2 项 / 总计 %3 项")
                    .arg(groupLocationText)
                    .arg(visibleEntryIndexList.size())
                    .arg(totalEntryIndexList.size()));
            for (const int entryIndex : visibleEntryIndexList)
            {
                appendRegistryTreeLeaf(
                    groupItem,
                    m_entryList[static_cast<std::size_t>(entryIndex)],
                    entryIndex);
            }
        }
        else
        {
            groupItem->setText(
                toStartupColumn(StartupColumn::Name),
                totalEntryIndexList.empty()
                ? QStringLiteral("%1    无条目").arg(groupLocationText)
                : QStringLiteral("%1    当前过滤下无匹配项（总计 %2 项）")
                    .arg(groupLocationText)
                    .arg(totalEntryIndexList.size()));

            QTreeWidgetItem* placeholderItem = new QTreeWidgetItem(groupItem);
            placeholderItem->setData(0, kStartupEntryIndexRole, -1);
            placeholderItem->setData(0, kStartupTreeNodeKindRole, static_cast<int>(StartupTreeNodeKind::Placeholder));
            placeholderItem->setData(0, kStartupTreeLocationRole, groupLocationText);
            placeholderItem->setText(
                toStartupColumn(StartupColumn::Name),
                totalEntryIndexList.empty() ? QStringLiteral("(无条目)") : QStringLiteral("(无匹配项)"));
            placeholderItem->setText(
                toStartupColumn(StartupColumn::Detail),
                totalEntryIndexList.empty()
                ? QStringLiteral("该位置当前未发现启动项")
                : QStringLiteral("存在条目，但被当前过滤条件隐藏"));
        }

        groupItem->setExpanded(true);
    }
}
