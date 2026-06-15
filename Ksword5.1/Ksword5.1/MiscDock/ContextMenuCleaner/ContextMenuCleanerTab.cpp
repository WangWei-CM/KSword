#include "ContextMenuCleanerTab.h"

#include "ContextMenuCleanerTab.Internal.h"
#include "../../theme.h"

#include <QAbstractItemView>
#include <QAction>
#include <QApplication>
#include <QClipboard>
#include <QHeaderView>
#include <QHBoxLayout>
#include <QIcon>
#include <QItemSelectionModel>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QMessageBox>
#include <QPushButton>
#include <QTableWidget>
#include <QTableWidgetItem>
#include <QTabWidget>
#include <QVBoxLayout>
#include <QStringList>

namespace ks::misc
{

using namespace context_menu_cleaner_detail;

ContextMenuCleanerTab::ContextMenuCleanerTab(QWidget* parent)
    : QWidget(parent)
{
    initializeUi();
    refreshAllAreas();

    kLogEvent event;
    info << event << "[ContextMenuCleanerTab] 右键菜单清理页初始化完成。" << eol;
}

void ContextMenuCleanerTab::initializeUi()
{
    // 根布局：上方风险说明，下方三类子页。
    m_rootLayout = new QVBoxLayout(this);
    m_rootLayout->setContentsMargins(6, 6, 6, 6);
    m_rootLayout->setSpacing(6);

    m_hintLabel = new QLabel(
        QStringLiteral("提示：本页枚举常见 IE/桌面/文件右键菜单注册表入口。删除会移除对应注册表子树，"
                       "请确认条目来源后再操作；系统项或第三方 Shell 扩展删除后通常需要重启 Explorer 才能完全刷新。"),
        this);
    m_hintLabel->setWordWrap(true);
    m_hintLabel->setStyleSheet(QStringLiteral("color:%1;").arg(KswordTheme::TextSecondaryHex()));
    m_rootLayout->addWidget(m_hintLabel);

    m_areaTabWidget = new QTabWidget(this);
    m_areaTabWidget->setObjectName(QStringLiteral("ksContextMenuCleanerAreaTabs"));
    m_rootLayout->addWidget(m_areaTabWidget, 1);

    createAreaPage(MenuArea::InternetExplorer);
    createAreaPage(MenuArea::Desktop);
    createAreaPage(MenuArea::File);
}

void ContextMenuCleanerTab::createAreaPage(const MenuArea area)
{
    AreaWidgets* areaWidgets = widgetsForArea(area);
    if (areaWidgets == nullptr)
    {
        return;
    }

    areaWidgets->page = new QWidget(m_areaTabWidget);
    areaWidgets->layout = new QVBoxLayout(areaWidgets->page);
    areaWidgets->layout->setContentsMargins(0, 0, 0, 0);
    areaWidgets->layout->setSpacing(6);

    areaWidgets->toolbarWidget = new QWidget(areaWidgets->page);
    QHBoxLayout* toolbarLayout = new QHBoxLayout(areaWidgets->toolbarWidget);
    toolbarLayout->setContentsMargins(0, 0, 0, 0);
    toolbarLayout->setSpacing(6);
    areaWidgets->layout->addWidget(areaWidgets->toolbarWidget);

    areaWidgets->refreshButton = new QPushButton(QIcon(QStringLiteral(":/Icon/process_refresh.svg")), QStringLiteral("刷新"), areaWidgets->toolbarWidget);
    areaWidgets->refreshButton->setToolTip(QStringLiteral("重新枚举当前分类的右键菜单注册表项"));
    areaWidgets->deleteButton = new QPushButton(QIcon(QStringLiteral(":/Icon/process_terminate.svg")), QStringLiteral("删除选中"), areaWidgets->toolbarWidget);
    areaWidgets->deleteButton->setToolTip(QStringLiteral("删除表格选中项对应的注册表子树"));
    areaWidgets->copyButton = new QPushButton(QIcon(QStringLiteral(":/Icon/log_copy.svg")), QStringLiteral("复制路径"), areaWidgets->toolbarWidget);
    areaWidgets->copyButton->setToolTip(QStringLiteral("复制选中项的注册表路径"));
    areaWidgets->filterEdit = new QLineEdit(areaWidgets->toolbarWidget);
    areaWidgets->filterEdit->setClearButtonEnabled(true);
    areaWidgets->filterEdit->setPlaceholderText(QStringLiteral("筛选：名称/显示名/命令/注册表路径/CLSID"));
    areaWidgets->filterEdit->setStyleSheet(buildInputStyle());

    areaWidgets->refreshButton->setStyleSheet(KswordTheme::ThemedButtonStyle());
    areaWidgets->deleteButton->setStyleSheet(KswordTheme::ThemedButtonStyle());
    areaWidgets->copyButton->setStyleSheet(KswordTheme::ThemedButtonStyle());

    toolbarLayout->addWidget(areaWidgets->refreshButton);
    toolbarLayout->addWidget(areaWidgets->deleteButton);
    toolbarLayout->addWidget(areaWidgets->copyButton);
    toolbarLayout->addWidget(areaWidgets->filterEdit, 1);

    areaWidgets->table = new QTableWidget(areaWidgets->page);
    areaWidgets->table->setColumnCount(kColumnCount);
    areaWidgets->table->setHorizontalHeaderLabels(QStringList{
        QStringLiteral("名称"),
        QStringLiteral("显示名"),
        QStringLiteral("类型"),
        QStringLiteral("来源"),
        QStringLiteral("命令/处理器"),
        QStringLiteral("注册表位置"),
        QStringLiteral("状态"),
        QStringLiteral("详情") });
    areaWidgets->table->setSelectionBehavior(QAbstractItemView::SelectRows);
    areaWidgets->table->setSelectionMode(QAbstractItemView::ExtendedSelection);
    areaWidgets->table->setEditTriggers(QAbstractItemView::NoEditTriggers);
    areaWidgets->table->setAlternatingRowColors(true);
    areaWidgets->table->setContextMenuPolicy(Qt::CustomContextMenu);
    areaWidgets->table->setHorizontalScrollMode(QAbstractItemView::ScrollPerPixel);
    areaWidgets->table->setSortingEnabled(true);
    areaWidgets->table->verticalHeader()->setVisible(false);
    areaWidgets->table->horizontalHeader()->setStyleSheet(buildHeaderStyle());
    areaWidgets->table->horizontalHeader()->setSectionResizeMode(kColumnName, QHeaderView::ResizeToContents);
    areaWidgets->table->horizontalHeader()->setSectionResizeMode(kColumnDisplayName, QHeaderView::ResizeToContents);
    areaWidgets->table->horizontalHeader()->setSectionResizeMode(kColumnKind, QHeaderView::ResizeToContents);
    areaWidgets->table->horizontalHeader()->setSectionResizeMode(kColumnSource, QHeaderView::ResizeToContents);
    areaWidgets->table->horizontalHeader()->setSectionResizeMode(kColumnCommandOrHandler, QHeaderView::Stretch);
    areaWidgets->table->horizontalHeader()->setSectionResizeMode(kColumnRegistryPath, QHeaderView::Stretch);
    areaWidgets->table->horizontalHeader()->setSectionResizeMode(kColumnStatus, QHeaderView::ResizeToContents);
    areaWidgets->table->horizontalHeader()->setSectionResizeMode(kColumnDetail, QHeaderView::Stretch);
    areaWidgets->layout->addWidget(areaWidgets->table, 1);

    areaWidgets->statusLabel = new QLabel(QStringLiteral("尚未刷新。"), areaWidgets->page);
    areaWidgets->statusLabel->setWordWrap(true);
    areaWidgets->layout->addWidget(areaWidgets->statusLabel);

    connect(areaWidgets->refreshButton, &QPushButton::clicked, this, [this, area]() {
        refreshArea(area);
    });
    connect(areaWidgets->deleteButton, &QPushButton::clicked, this, [this, area]() {
        deleteSelectedEntries(area);
    });
    connect(areaWidgets->copyButton, &QPushButton::clicked, this, [this, area]() {
        copySelectedEntries(area);
    });
    connect(areaWidgets->filterEdit, &QLineEdit::textChanged, this, [this, area](const QString&) {
        rebuildAreaTable(area);
    });
    connect(areaWidgets->table, &QTableWidget::customContextMenuRequested, this, [this, area](const QPoint& localPosition) {
        showAreaContextMenu(area, localPosition);
    });

    m_areaTabWidget->addTab(areaWidgets->page, QIcon(areaIconPath(area)), areaTitle(area));
}

void ContextMenuCleanerTab::refreshAllAreas()
{
    refreshArea(MenuArea::InternetExplorer);
    refreshArea(MenuArea::Desktop);
    refreshArea(MenuArea::File);
}

void ContextMenuCleanerTab::refreshArea(const MenuArea area)
{
    AreaWidgets* areaWidgets = widgetsForArea(area);
    if (areaWidgets == nullptr)
    {
        return;
    }

    const QVector<ContextMenuEntry> newEntries = enumerateEntriesForArea(area);
    areaWidgets->entries = newEntries;
    rebuildAreaTable(area);

    kLogEvent event;
    info << event
        << "[ContextMenuCleanerTab] 刷新分类完成, area="
        << areaTitle(area).toStdString()
        << ", count="
        << newEntries.size()
        << eol;
}

void ContextMenuCleanerTab::rebuildAreaTable(const MenuArea area)
{
    AreaWidgets* areaWidgets = widgetsForArea(area);
    if (areaWidgets == nullptr || areaWidgets->table == nullptr)
    {
        return;
    }

    const QString filterText = areaWidgets->filterEdit != nullptr
        ? areaWidgets->filterEdit->text().trimmed().toLower()
        : QString();

    areaWidgets->table->setSortingEnabled(false);
    areaWidgets->table->setRowCount(0);

    int visibleCount = 0;
    for (int entryIndex = 0; entryIndex < areaWidgets->entries.size(); ++entryIndex)
    {
        const ContextMenuEntry& entry = areaWidgets->entries.at(entryIndex);
        const QString searchableText = QStringList{
            entry.itemName,
            entry.displayName,
            entry.entryKind,
            entry.sourceGroup,
            entry.commandOrHandler,
            rootPathText(entry.rootLabel, entry.subKeyPath),
            entry.statusText,
            entry.detailText,
            entry.clsidText }.join('\n').toLower();
        if (!filterText.isEmpty() && !searchableText.contains(filterText))
        {
            continue;
        }

        const int row = visibleCount++;
        areaWidgets->table->insertRow(row);

        const auto makeItem = [entryIndex](const QString& text) -> QTableWidgetItem*
        {
            QTableWidgetItem* item = new QTableWidgetItem(text);
            item->setData(Qt::UserRole, entryIndex);
            item->setToolTip(text);
            return item;
        };

        areaWidgets->table->setItem(row, kColumnName, makeItem(entry.itemName));
        areaWidgets->table->setItem(row, kColumnDisplayName, makeItem(entry.displayName));
        areaWidgets->table->setItem(row, kColumnKind, makeItem(entry.entryKind));
        areaWidgets->table->setItem(row, kColumnSource, makeItem(entry.sourceGroup));
        areaWidgets->table->setItem(row, kColumnCommandOrHandler, makeItem(entry.commandOrHandler));
        areaWidgets->table->setItem(row, kColumnRegistryPath, makeItem(rootPathText(entry.rootLabel, entry.subKeyPath)));
        areaWidgets->table->setItem(row, kColumnStatus, makeItem(entry.statusText));
        areaWidgets->table->setItem(row, kColumnDetail, makeItem(entry.detailText));
    }

    areaWidgets->table->setSortingEnabled(true);
    if (areaWidgets->statusLabel != nullptr)
    {
        areaWidgets->statusLabel->setText(QStringLiteral("%1：共枚举 %2 项，当前显示 %3 项。")
            .arg(areaTitle(area))
            .arg(areaWidgets->entries.size())
            .arg(visibleCount));
    }
}

void ContextMenuCleanerTab::showAreaContextMenu(const MenuArea area, const QPoint& localPosition)
{
    AreaWidgets* areaWidgets = widgetsForArea(area);
    if (areaWidgets == nullptr || areaWidgets->table == nullptr)
    {
        return;
    }

    QMenu menu(areaWidgets->table);
    menu.setStyleSheet(KswordTheme::ContextMenuStyle());
    QAction* copyAction = menu.addAction(QIcon(QStringLiteral(":/Icon/log_copy.svg")), QStringLiteral("复制注册表路径"));
    QAction* deleteAction = menu.addAction(QIcon(QStringLiteral(":/Icon/process_terminate.svg")), QStringLiteral("删除选中项"));

    const QVector<int> selectedIndexes = selectedEntryIndexes(area);
    copyAction->setEnabled(!selectedIndexes.isEmpty());
    deleteAction->setEnabled(!selectedIndexes.isEmpty());

    QAction* selectedAction = menu.exec(areaWidgets->table->viewport()->mapToGlobal(localPosition));
    if (selectedAction == copyAction)
    {
        copySelectedEntries(area);
    }
    else if (selectedAction == deleteAction)
    {
        deleteSelectedEntries(area);
    }
}

void ContextMenuCleanerTab::deleteSelectedEntries(const MenuArea area)
{
    AreaWidgets* areaWidgets = widgetsForArea(area);
    if (areaWidgets == nullptr)
    {
        return;
    }

    const QVector<int> selectedIndexes = selectedEntryIndexes(area);
    if (selectedIndexes.isEmpty())
    {
        QMessageBox::information(this, QStringLiteral("右键菜单清理"), QStringLiteral("请先选择需要删除的右键菜单项。"));
        return;
    }

    QStringList targetPaths;
    for (const int entryIndex : selectedIndexes)
    {
        if (entryIndex < 0 || entryIndex >= areaWidgets->entries.size())
        {
            continue;
        }
        const ContextMenuEntry& entry = areaWidgets->entries.at(entryIndex);
        targetPaths.push_back(rootPathText(entry.rootLabel, entry.subKeyPath));
    }

    const QString previewText = targetPaths.mid(0, 8).join('\n');
    const QString moreText = targetPaths.size() > 8
        ? QStringLiteral("\n... 另有 %1 项").arg(targetPaths.size() - 8)
        : QString();
    const QMessageBox::StandardButton confirmButton = QMessageBox::warning(
        this,
        QStringLiteral("确认删除右键菜单项"),
        QStringLiteral("将删除 %1 个注册表子树。此操作不会自动备份，删除后通常需要重启 Explorer 或相关程序才会完全生效。\n\n%2%3")
            .arg(targetPaths.size())
            .arg(previewText)
            .arg(moreText),
        QMessageBox::Yes | QMessageBox::No,
        QMessageBox::No);
    if (confirmButton != QMessageBox::Yes)
    {
        return;
    }

    QStringList failedMessages;
    int successCount = 0;
    for (const int entryIndex : selectedIndexes)
    {
        if (entryIndex < 0 || entryIndex >= areaWidgets->entries.size())
        {
            continue;
        }
        const ContextMenuEntry& entry = areaWidgets->entries.at(entryIndex);
        QString errorText;
        const bool deleteOk = deleteRegistryTreeWithView(entry.rootKey, entry.subKeyPath, entry.viewFlag, &errorText);
        if (deleteOk)
        {
            ++successCount;
            kLogEvent event;
            warn << event
                << "[ContextMenuCleanerTab] 删除右键菜单注册表项成功, path="
                << rootPathText(entry.rootLabel, entry.subKeyPath).toStdString()
                << eol;
        }
        else
        {
            failedMessages.push_back(QStringLiteral("%1：%2")
                .arg(rootPathText(entry.rootLabel, entry.subKeyPath), errorText));
            kLogEvent event;
            err << event
                << "[ContextMenuCleanerTab] 删除右键菜单注册表项失败, path="
                << rootPathText(entry.rootLabel, entry.subKeyPath).toStdString()
                << ", error="
                << errorText.toStdString()
                << eol;
        }
    }

    refreshArea(area);

    if (failedMessages.isEmpty())
    {
        QMessageBox::information(
            this,
            QStringLiteral("右键菜单清理"),
            QStringLiteral("已删除 %1 项。建议重启 Explorer 后确认右键菜单变化。").arg(successCount));
    }
    else
    {
        QMessageBox::warning(
            this,
            QStringLiteral("右键菜单清理"),
            QStringLiteral("成功删除 %1 项，失败 %2 项：\n\n%3")
                .arg(successCount)
                .arg(failedMessages.size())
                .arg(failedMessages.join('\n')));
    }
}

void ContextMenuCleanerTab::copySelectedEntries(const MenuArea area) const
{
    const AreaWidgets* areaWidgets = widgetsForArea(area);
    if (areaWidgets == nullptr)
    {
        return;
    }

    const QVector<int> selectedIndexes = selectedEntryIndexes(area);
    if (selectedIndexes.isEmpty())
    {
        QMessageBox::information(const_cast<ContextMenuCleanerTab*>(this), QStringLiteral("复制注册表路径"), QStringLiteral("请先选择需要复制的行。"));
        return;
    }

    QStringList lines;
    for (const int entryIndex : selectedIndexes)
    {
        if (entryIndex < 0 || entryIndex >= areaWidgets->entries.size())
        {
            continue;
        }
        const ContextMenuEntry& entry = areaWidgets->entries.at(entryIndex);
        lines.push_back(rootPathText(entry.rootLabel, entry.subKeyPath));
    }

    if (QClipboard* clipboard = QApplication::clipboard())
    {
        clipboard->setText(lines.join('\n'));
    }
}


QVector<int> ContextMenuCleanerTab::selectedEntryIndexes(const MenuArea area) const
{
    const AreaWidgets* areaWidgets = widgetsForArea(area);
    QVector<int> indexes;
    if (areaWidgets == nullptr || areaWidgets->table == nullptr || areaWidgets->table->selectionModel() == nullptr)
    {
        return indexes;
    }

    const QModelIndexList selectedRows = areaWidgets->table->selectionModel()->selectedRows();
    for (const QModelIndex& modelIndex : selectedRows)
    {
        const QTableWidgetItem* item = areaWidgets->table->item(modelIndex.row(), kColumnName);
        if (item == nullptr)
        {
            continue;
        }
        const int entryIndex = item->data(Qt::UserRole).toInt();
        if (!indexes.contains(entryIndex))
        {
            indexes.push_back(entryIndex);
        }
    }
    return indexes;
}

ContextMenuCleanerTab::AreaWidgets* ContextMenuCleanerTab::widgetsForArea(const MenuArea area)
{
    switch (area)
    {
    case MenuArea::InternetExplorer:
        return &m_ieWidgets;
    case MenuArea::Desktop:
        return &m_desktopWidgets;
    case MenuArea::File:
        return &m_fileWidgets;
    }
    return &m_fileWidgets;
}

const ContextMenuCleanerTab::AreaWidgets* ContextMenuCleanerTab::widgetsForArea(const MenuArea area) const
{
    switch (area)
    {
    case MenuArea::InternetExplorer:
        return &m_ieWidgets;
    case MenuArea::Desktop:
        return &m_desktopWidgets;
    case MenuArea::File:
        return &m_fileWidgets;
    }
    return &m_fileWidgets;
}

QString ContextMenuCleanerTab::areaTitle(const MenuArea area)
{
    switch (area)
    {
    case MenuArea::InternetExplorer:
        return QStringLiteral("IE右键菜单");
    case MenuArea::Desktop:
        return QStringLiteral("桌面右键菜单");
    case MenuArea::File:
        return QStringLiteral("文件右键菜单");
    }
    return QStringLiteral("文件右键菜单");
}

QString ContextMenuCleanerTab::areaIconPath(const MenuArea area)
{
    switch (area)
    {
    case MenuArea::InternetExplorer:
        return QStringLiteral(":/Icon/process_list.svg");
    case MenuArea::Desktop:
        return QStringLiteral(":/Icon/desktop_switch.svg");
    case MenuArea::File:
        return QStringLiteral(":/Icon/process_open_folder.svg");
    }
    return QStringLiteral(":/Icon/process_open_folder.svg");
}

} // namespace ks::misc
