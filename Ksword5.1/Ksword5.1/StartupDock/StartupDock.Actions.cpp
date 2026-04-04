#include "StartupDock.Internal.h"

#include <winsvc.h>

#pragma comment(lib, "Advapi32.lib")

using namespace startup_dock_detail;

namespace
{
    // splitRegistryLocation 作用：
    // - 把 "HKLM\\..." 形式位置拆成根键与子键。
    bool splitRegistryLocation(const QString& locationText, HKEY* rootKeyOut, QString* subKeyOut)
    {
        if (rootKeyOut == nullptr || subKeyOut == nullptr)
        {
            return false;
        }

        const int slashIndex = locationText.indexOf('\\');
        if (slashIndex <= 0)
        {
            return false;
        }

        const QString rootKeyText = locationText.left(slashIndex).trimmed().toUpper();
        *subKeyOut = locationText.mid(slashIndex + 1).trimmed();
        if (rootKeyText == QStringLiteral("HKCU"))
        {
            *rootKeyOut = HKEY_CURRENT_USER;
            return true;
        }
        if (rootKeyText == QStringLiteral("HKLM"))
        {
            *rootKeyOut = HKEY_LOCAL_MACHINE;
            return true;
        }
        return false;
    }

    // deleteRegistryValueByPath 作用：
    // - 删除指定注册表值。
    bool deleteRegistryValueByPath(HKEY rootKey, const QString& subKeyText, const QString& valueNameText, QString* errorTextOut)
    {
        HKEY openedKey = nullptr;
        const LONG openResult = ::RegOpenKeyExW(
            rootKey,
            reinterpret_cast<LPCWSTR>(subKeyText.utf16()),
            0,
            KEY_SET_VALUE,
            &openedKey);
        if (openResult != ERROR_SUCCESS || openedKey == nullptr)
        {
            if (errorTextOut != nullptr)
            {
                *errorTextOut = winErrorText(static_cast<DWORD>(openResult));
            }
            return false;
        }

        const LONG deleteResult = ::RegDeleteValueW(
            openedKey,
            valueNameText.trimmed().isEmpty() ? nullptr : reinterpret_cast<LPCWSTR>(valueNameText.utf16()));
        ::RegCloseKey(openedKey);
        if (deleteResult != ERROR_SUCCESS)
        {
            if (errorTextOut != nullptr)
            {
                *errorTextOut = winErrorText(static_cast<DWORD>(deleteResult));
            }
            return false;
        }
        return true;
    }

    // deleteRegistryTreeByPath 作用：
    // - 删除指定注册表子树。
    bool deleteRegistryTreeByPath(HKEY rootKey, const QString& subKeyText, QString* errorTextOut)
    {
        const LONG deleteResult = ::RegDeleteTreeW(
            rootKey,
            reinterpret_cast<LPCWSTR>(subKeyText.utf16()));
        if (deleteResult != ERROR_SUCCESS)
        {
            if (errorTextOut != nullptr)
            {
                *errorTextOut = winErrorText(static_cast<DWORD>(deleteResult));
            }
            return false;
        }
        return true;
    }

    // deleteScmObjectByName 作用：
    // - 删除服务或驱动服务注册项。
    bool deleteScmObjectByName(const QString& serviceNameText, QString* errorTextOut)
    {
        SC_HANDLE scmHandle = ::OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT);
        if (scmHandle == nullptr)
        {
            if (errorTextOut != nullptr)
            {
                *errorTextOut = winErrorText(::GetLastError());
            }
            return false;
        }

        SC_HANDLE serviceHandle = ::OpenServiceW(
            scmHandle,
            reinterpret_cast<LPCWSTR>(serviceNameText.utf16()),
            DELETE);
        if (serviceHandle == nullptr)
        {
            if (errorTextOut != nullptr)
            {
                *errorTextOut = winErrorText(::GetLastError());
            }
            ::CloseServiceHandle(scmHandle);
            return false;
        }

        const BOOL deleteOk = ::DeleteService(serviceHandle);
        if (deleteOk == FALSE && errorTextOut != nullptr)
        {
            *errorTextOut = winErrorText(::GetLastError());
        }
        ::CloseServiceHandle(serviceHandle);
        ::CloseServiceHandle(scmHandle);
        return deleteOk != FALSE;
    }
}

void StartupDock::initializeConnections()
{
    connect(m_refreshButton, &QPushButton::clicked, this, [this]()
        {
            requestAsyncRefresh(true);
        });
    connect(m_exportButton, &QPushButton::clicked, this, [this]()
        {
            exportCurrentView();
        });
    connect(m_copyButton, &QPushButton::clicked, this, [this]()
        {
            copySelectedRow(currentCategory(), currentCategoryTable());
        });
    connect(m_filterEdit, &QLineEdit::textChanged, this, [this](const QString&)
        {
            applyFilterAndRefresh();
        });
    connect(m_hideMicrosoftCheck, &QCheckBox::toggled, this, [this](const bool)
        {
            applyFilterAndRefresh();
        });

    const auto bindTableContextMenu =
        [this](const StartupCategory category, QTableWidget* tableWidget)
        {
            if (tableWidget == nullptr)
            {
                return;
            }
            connect(tableWidget, &QWidget::customContextMenuRequested, this, [this, category, tableWidget](const QPoint& localPos)
                {
                    showEntryContextMenu(category, tableWidget, localPos);
                });
            connect(tableWidget, &QTableWidget::cellDoubleClicked, this, [this, category, tableWidget](const int row, const int /*column*/)
                {
                    if (row < 0)
                    {
                        return;
                    }
                    openSelectedFileLocation(category, tableWidget);
                });
        };

    bindTableContextMenu(StartupCategory::All, m_allTable);
    bindTableContextMenu(StartupCategory::Logon, m_logonTable);
    bindTableContextMenu(StartupCategory::Services, m_servicesTable);
    bindTableContextMenu(StartupCategory::Drivers, m_driversTable);
    bindTableContextMenu(StartupCategory::Tasks, m_tasksTable);

    if (m_registryTree != nullptr)
    {
        connect(m_registryTree, &QWidget::customContextMenuRequested, this, [this](const QPoint& localPos)
            {
                showRegistryContextMenu(localPos);
            });
        connect(m_registryTree, &QTreeWidget::itemDoubleClicked, this, [this](QTreeWidgetItem* treeItem, int)
            {
                if (treeItem == nullptr)
                {
                    return;
                }

                const StartupTreeNodeKind nodeKind = static_cast<StartupTreeNodeKind>(
                    treeItem->data(0, kStartupTreeNodeKindRole).toInt());
                if (nodeKind == StartupTreeNodeKind::Entry)
                {
                    openSelectedFileLocation(StartupCategory::Registry, nullptr);
                }
                else if (nodeKind == StartupTreeNodeKind::Group
                    || nodeKind == StartupTreeNodeKind::Placeholder)
                {
                    openSelectedRegistryLocation(StartupCategory::Registry, nullptr);
                }
            });
    }
}

void StartupDock::refreshAllStartupEntries()
{
    requestAsyncRefresh(true);
}

void StartupDock::showEntryContextMenu(
    const StartupCategory category,
    QTableWidget* tableWidget,
    const QPoint& localPos)
{
    if (tableWidget == nullptr)
    {
        return;
    }

    QTableWidgetItem* clickedItem = tableWidget->itemAt(localPos);
    if (clickedItem == nullptr)
    {
        return;
    }

    const int entryIndex = findEntryIndexByTableRow(category, clickedItem->row());
    if (entryIndex < 0 || entryIndex >= static_cast<int>(m_entryList.size()))
    {
        return;
    }

    const StartupEntry& entry = m_entryList[static_cast<std::size_t>(entryIndex)];
    QMenu contextMenu(this);
    QAction* copyAction = contextMenu.addAction(createBlueIcon(":/Icon/log_copy.svg"), QStringLiteral("复制整行"));
    QAction* openFileAction = contextMenu.addAction(createBlueIcon(":/Icon/process_open_folder.svg"), QStringLiteral("打开文件位置"));
    QAction* openRegistryAction = contextMenu.addAction(createBlueIcon(":/Icon/file_find.svg"), QStringLiteral("打开注册表位置"));
    QAction* deleteAction = contextMenu.addAction(createBlueIcon(":/Icon/log_clear.svg"), QStringLiteral("删除项"));
    openFileAction->setEnabled(entry.canOpenFileLocation);
    openRegistryAction->setEnabled(entry.canOpenRegistryLocation);
    deleteAction->setEnabled(entry.canDelete);

    QAction* selectedAction = contextMenu.exec(tableWidget->viewport()->mapToGlobal(localPos));
    if (selectedAction == copyAction)
    {
        copySelectedRow(category, tableWidget);
    }
    else if (selectedAction == openFileAction)
    {
        openSelectedFileLocation(category, tableWidget);
    }
    else if (selectedAction == openRegistryAction)
    {
        openSelectedRegistryLocation(category, tableWidget);
    }
    else if (selectedAction == deleteAction)
    {
        deleteSelectedEntry(category, tableWidget);
    }
}

void StartupDock::showRegistryContextMenu(const QPoint& localPos)
{
    if (m_registryTree == nullptr)
    {
        return;
    }

    QTreeWidgetItem* treeItem = m_registryTree->itemAt(localPos);
    if (treeItem == nullptr)
    {
        return;
    }

    m_registryTree->setCurrentItem(treeItem);

    const StartupTreeNodeKind nodeKind = static_cast<StartupTreeNodeKind>(
        treeItem->data(0, kStartupTreeNodeKindRole).toInt());
    const int entryIndex = findEntryIndexByRegistryTreeItem(treeItem);
    const QString locationText = treeItem->data(0, kStartupTreeLocationRole).toString().trimmed();

    QMenu contextMenu(this);
    QAction* copyAction = contextMenu.addAction(createBlueIcon(":/Icon/log_copy.svg"), QStringLiteral("复制"));
    QAction* openFileAction = contextMenu.addAction(createBlueIcon(":/Icon/process_open_folder.svg"), QStringLiteral("打开文件位置"));
    QAction* openRegistryAction = contextMenu.addAction(createBlueIcon(":/Icon/file_find.svg"), QStringLiteral("打开注册表位置"));
    QAction* deleteAction = contextMenu.addAction(createBlueIcon(":/Icon/log_clear.svg"), QStringLiteral("删除项"));

    if (nodeKind == StartupTreeNodeKind::Group
        || nodeKind == StartupTreeNodeKind::Placeholder)
    {
        openFileAction->setEnabled(false);
        openRegistryAction->setEnabled(!locationText.isEmpty());
        deleteAction->setEnabled(false);
    }
    else if (entryIndex >= 0 && entryIndex < static_cast<int>(m_entryList.size()))
    {
        const StartupEntry& entry = m_entryList[static_cast<std::size_t>(entryIndex)];
        openFileAction->setEnabled(entry.canOpenFileLocation);
        openRegistryAction->setEnabled(entry.canOpenRegistryLocation);
        deleteAction->setEnabled(entry.canDelete);
    }
    else
    {
        openFileAction->setEnabled(false);
        openRegistryAction->setEnabled(false);
        deleteAction->setEnabled(false);
    }

    QAction* selectedAction = contextMenu.exec(m_registryTree->viewport()->mapToGlobal(localPos));
    if (selectedAction == copyAction)
    {
        copySelectedRow(StartupCategory::Registry, nullptr);
    }
    else if (selectedAction == openFileAction)
    {
        openSelectedFileLocation(StartupCategory::Registry, nullptr);
    }
    else if (selectedAction == openRegistryAction)
    {
        openSelectedRegistryLocation(StartupCategory::Registry, nullptr);
    }
    else if (selectedAction == deleteAction)
    {
        deleteSelectedEntry(StartupCategory::Registry, nullptr);
    }
}

void StartupDock::openSelectedFileLocation(const StartupCategory category, QTableWidget* tableWidget)
{
    int entryIndex = -1;
    if (category == StartupCategory::Registry)
    {
        entryIndex = findEntryIndexByRegistryTreeItem(
            m_registryTree != nullptr ? m_registryTree->currentItem() : nullptr);
    }
    else if (tableWidget != nullptr && tableWidget->currentRow() >= 0)
    {
        entryIndex = findEntryIndexByTableRow(category, tableWidget->currentRow());
    }
    if (entryIndex < 0 || entryIndex >= static_cast<int>(m_entryList.size()))
    {
        return;
    }

    const StartupEntry& entry = m_entryList[static_cast<std::size_t>(entryIndex)];
    if (!entry.canOpenFileLocation || entry.imagePathText.trimmed().isEmpty())
    {
        QMessageBox::information(this, QStringLiteral("启动项"), QStringLiteral("该条目没有可打开的文件路径。"));
        return;
    }

    QProcess::startDetached(
        QStringLiteral("explorer.exe"),
        { QStringLiteral("/select,%1").arg(QDir::toNativeSeparators(entry.imagePathText)) });
}

void StartupDock::openSelectedRegistryLocation(const StartupCategory category, QTableWidget* tableWidget)
{
    QString locationText;
    if (category == StartupCategory::Registry)
    {
        QTreeWidgetItem* currentItem = (m_registryTree != nullptr) ? m_registryTree->currentItem() : nullptr;
        if (currentItem == nullptr)
        {
            return;
        }

        const StartupTreeNodeKind nodeKind = static_cast<StartupTreeNodeKind>(
            currentItem->data(0, kStartupTreeNodeKindRole).toInt());
        if (nodeKind == StartupTreeNodeKind::Group || nodeKind == StartupTreeNodeKind::Placeholder)
        {
            locationText = currentItem->data(0, kStartupTreeLocationRole).toString().trimmed();
        }
        else
        {
            const int entryIndex = findEntryIndexByRegistryTreeItem(currentItem);
            if (entryIndex >= 0 && entryIndex < static_cast<int>(m_entryList.size()))
            {
                locationText = m_entryList[static_cast<std::size_t>(entryIndex)].locationText;
            }
        }
    }
    else if (tableWidget != nullptr && tableWidget->currentRow() >= 0)
    {
        const int entryIndex = findEntryIndexByTableRow(category, tableWidget->currentRow());
        if (entryIndex >= 0 && entryIndex < static_cast<int>(m_entryList.size()))
        {
            locationText = m_entryList[static_cast<std::size_t>(entryIndex)].locationText;
        }
    }
    if (locationText.trimmed().isEmpty())
    {
        QMessageBox::information(this, QStringLiteral("启动项"), QStringLiteral("该条目没有可打开的注册表位置。"));
        return;
    }

    QApplication::clipboard()->setText(locationText);
    QProcess::startDetached(QStringLiteral("regedit.exe"), {});
    QMessageBox::information(
        this,
        QStringLiteral("启动项"),
        QStringLiteral("已复制注册表路径到剪贴板，并尝试打开 regedit。"));
}

void StartupDock::copySelectedRow(const StartupCategory category, QTableWidget* tableWidget)
{
    if (category == StartupCategory::Registry)
    {
        QTreeWidgetItem* currentItem = (m_registryTree != nullptr) ? m_registryTree->currentItem() : nullptr;
        if (currentItem == nullptr)
        {
            return;
        }

        const StartupTreeNodeKind nodeKind = static_cast<StartupTreeNodeKind>(
            currentItem->data(0, kStartupTreeNodeKindRole).toInt());
        if (nodeKind == StartupTreeNodeKind::Group || nodeKind == StartupTreeNodeKind::Placeholder)
        {
            QApplication::clipboard()->setText(currentItem->data(0, kStartupTreeLocationRole).toString());
            return;
        }

        const int entryIndex = findEntryIndexByRegistryTreeItem(currentItem);
        if (entryIndex < 0 || entryIndex >= static_cast<int>(m_entryList.size()))
        {
            return;
        }

        const StartupEntry& entry = m_entryList[static_cast<std::size_t>(entryIndex)];
        const QString rowText = QStringLiteral("%1\t%2\t%3\t%4\t%5\t%6\t%7\t%8\t%9")
            .arg(entry.itemNameText)
            .arg(entry.publisherText)
            .arg(entry.imagePathText)
            .arg(entry.commandText)
            .arg(entry.locationText)
            .arg(entry.userText)
            .arg(buildStatusText(entry.enabled))
            .arg(entry.sourceTypeText)
            .arg(entry.detailText);
        QApplication::clipboard()->setText(rowText);
        return;
    }

    if (tableWidget == nullptr || tableWidget->currentRow() < 0)
    {
        return;
    }

    const int entryIndex = findEntryIndexByTableRow(category, tableWidget->currentRow());
    if (entryIndex < 0 || entryIndex >= static_cast<int>(m_entryList.size()))
    {
        return;
    }

    const StartupEntry& entry = m_entryList[static_cast<std::size_t>(entryIndex)];
    const QString rowText = QStringLiteral("%1\t%2\t%3\t%4\t%5\t%6\t%7\t%8\t%9")
        .arg(entry.itemNameText)
        .arg(entry.publisherText)
        .arg(entry.imagePathText)
        .arg(entry.commandText)
        .arg(entry.locationText)
        .arg(entry.userText)
        .arg(buildStatusText(entry.enabled))
        .arg(entry.sourceTypeText)
        .arg(entry.detailText);
    QApplication::clipboard()->setText(rowText);
}

void StartupDock::exportCurrentView()
{
    const QString outputPath = QFileDialog::getSaveFileName(
        this,
        QStringLiteral("导出启动项"),
        QStringLiteral("StartupEntries.txt"),
        QStringLiteral("Text Files (*.txt);;All Files (*.*)"));
    if (outputPath.trimmed().isEmpty())
    {
        return;
    }

    QFile outputFile(outputPath);
    if (!outputFile.open(QIODevice::WriteOnly | QIODevice::Text))
    {
        QMessageBox::warning(this, QStringLiteral("启动项"), QStringLiteral("导出失败：%1").arg(outputFile.errorString()));
        return;
    }

    QTextStream outputStream(&outputFile);
    outputStream.setEncoding(QStringConverter::Utf8);
    outputStream << "名称\t发布者\t镜像路径\t命令\t来源位置\t用户\t状态\t类型\t详情\n";
    if (currentCategory() == StartupCategory::Registry)
    {
        if (m_registryTree == nullptr)
        {
            return;
        }

        for (int rootIndex = 0; rootIndex < m_registryTree->topLevelItemCount(); ++rootIndex)
        {
            QTreeWidgetItem* groupItem = m_registryTree->topLevelItem(rootIndex);
            if (groupItem == nullptr)
            {
                continue;
            }

            for (int childIndex = 0; childIndex < groupItem->childCount(); ++childIndex)
            {
                QTreeWidgetItem* childItem = groupItem->child(childIndex);
                const int entryIndex = findEntryIndexByRegistryTreeItem(childItem);
                if (entryIndex < 0 || entryIndex >= static_cast<int>(m_entryList.size()))
                {
                    continue;
                }

                const StartupEntry& entry = m_entryList[static_cast<std::size_t>(entryIndex)];
                outputStream
                    << entry.itemNameText << '\t'
                    << entry.publisherText << '\t'
                    << entry.imagePathText << '\t'
                    << entry.commandText << '\t'
                    << entry.locationText << '\t'
                    << entry.userText << '\t'
                    << buildStatusText(entry.enabled) << '\t'
                    << entry.sourceTypeText << '\t'
                    << entry.detailText << '\n';
            }
        }
    }
    else
    {
        QTableWidget* tableWidget = currentCategoryTable();
        if (tableWidget == nullptr)
        {
            return;
        }

        for (int rowIndex = 0; rowIndex < tableWidget->rowCount(); ++rowIndex)
        {
            const int entryIndex = findEntryIndexByTableRow(currentCategory(), rowIndex);
            if (entryIndex < 0 || entryIndex >= static_cast<int>(m_entryList.size()))
            {
                continue;
            }

            const StartupEntry& entry = m_entryList[static_cast<std::size_t>(entryIndex)];
            outputStream
                << entry.itemNameText << '\t'
                << entry.publisherText << '\t'
                << entry.imagePathText << '\t'
                << entry.commandText << '\t'
                << entry.locationText << '\t'
                << entry.userText << '\t'
                << buildStatusText(entry.enabled) << '\t'
                << entry.sourceTypeText << '\t'
                << entry.detailText << '\n';
        }
    }
}

void StartupDock::applyFilterAndRefresh()
{
    rebuildAllTables();
    if (m_statusLabel != nullptr)
    {
        m_statusLabel->setText(QStringLiteral("状态：共 %1 条，当前分类 %2")
            .arg(m_entryList.size())
            .arg(categoryToText(currentCategory())));
    }
}

void StartupDock::deleteSelectedEntry(const StartupCategory category, QTableWidget* tableWidget)
{
    int entryIndex = -1;
    if (category == StartupCategory::Registry)
    {
        entryIndex = findEntryIndexByRegistryTreeItem(
            m_registryTree != nullptr ? m_registryTree->currentItem() : nullptr);
    }
    else if (tableWidget != nullptr && tableWidget->currentRow() >= 0)
    {
        entryIndex = findEntryIndexByTableRow(category, tableWidget->currentRow());
    }
    if (entryIndex < 0 || entryIndex >= static_cast<int>(m_entryList.size()))
    {
        return;
    }

    const StartupEntry& entry = m_entryList[static_cast<std::size_t>(entryIndex)];
    if (!entry.canDelete)
    {
        QMessageBox::information(this, QStringLiteral("启动项"), QStringLiteral("该条目当前不支持删除。"));
        return;
    }

    const QMessageBox::StandardButton confirmButton = QMessageBox::warning(
        this,
        QStringLiteral("删除启动项"),
        QStringLiteral("确定删除以下条目？\n\n%1\n来源：%2")
            .arg(entry.itemNameText)
            .arg(entry.locationText),
        QMessageBox::Yes | QMessageBox::No,
        QMessageBox::No);
    if (confirmButton != QMessageBox::Yes)
    {
        return;
    }

    QString errorText;
    bool deleteOk = false;

    if (entry.sourceTypeText == QStringLiteral("StartupFolder"))
    {
        deleteOk = QFile::remove(entry.imagePathText);
        if (!deleteOk)
        {
            errorText = QStringLiteral("删除文件失败：%1").arg(entry.imagePathText);
        }
    }
    else if (entry.category == StartupCategory::Logon || entry.category == StartupCategory::Registry)
    {
        HKEY rootKey = nullptr;
        QString subKeyText;
        if (!splitRegistryLocation(entry.locationText, &rootKey, &subKeyText))
        {
            errorText = QStringLiteral("解析注册表位置失败。");
            deleteOk = false;
        }
        else if (entry.deleteRegistryTree)
        {
            deleteOk = deleteRegistryTreeByPath(rootKey, subKeyText, &errorText);
        }
        else
        {
            const QString valueNameText = entry.registryValueNameText.trimmed().isEmpty()
                ? entry.itemNameText
                : entry.registryValueNameText;
            deleteOk = deleteRegistryValueByPath(rootKey, subKeyText, valueNameText, &errorText);
        }

        if (!deleteOk && errorText.isEmpty())
        {
            errorText = QStringLiteral("删除注册表启动项失败。");
        }
    }
    else if (entry.sourceTypeText == QStringLiteral("AutoService")
        || entry.sourceTypeText == QStringLiteral("Driver"))
    {
        const int lastSlashIndex = entry.locationText.lastIndexOf('\\');
        const QString serviceNameText = (lastSlashIndex >= 0)
            ? entry.locationText.mid(lastSlashIndex + 1)
            : entry.itemNameText;
        deleteOk = deleteScmObjectByName(serviceNameText, &errorText);
    }
    else if (entry.sourceTypeText == QStringLiteral("ScheduledTask"))
    {
        QProcess processObject;
        processObject.setProgram(QStringLiteral("schtasks.exe"));
        processObject.setArguments({
            QStringLiteral("/Delete"),
            QStringLiteral("/TN"),
            entry.locationText,
            QStringLiteral("/F")
            });
        processObject.start();
        deleteOk = processObject.waitForStarted(1500) && processObject.waitForFinished(10000)
            && processObject.exitStatus() == QProcess::NormalExit
            && processObject.exitCode() == 0;
        if (!deleteOk)
        {
            errorText = QString::fromLocal8Bit(processObject.readAllStandardError()).trimmed();
            if (errorText.isEmpty())
            {
                errorText = QString::fromLocal8Bit(processObject.readAllStandardOutput()).trimmed();
            }
            if (errorText.isEmpty())
            {
                errorText = QStringLiteral("schtasks 删除失败。");
            }
        }
    }

    if (!deleteOk)
    {
        QMessageBox::warning(this, QStringLiteral("启动项"), QStringLiteral("删除失败：%1").arg(errorText));
        return;
    }

    kLogEvent deleteEvent;
    info << deleteEvent
        << "[StartupDock] 删除启动项成功, type="
        << entry.sourceTypeText.toStdString()
        << ", name="
        << entry.itemNameText.toStdString()
        << ", location="
        << entry.locationText.toStdString()
        << eol;

    refreshAllStartupEntries();
}

int StartupDock::findEntryIndexByTableRow(const StartupCategory category, const int row) const
{
    QTableWidget* tableWidget = nullptr;
    switch (category)
    {
    case StartupCategory::All:
        tableWidget = m_allTable;
        break;
    case StartupCategory::Logon:
        tableWidget = m_logonTable;
        break;
    case StartupCategory::Services:
        tableWidget = m_servicesTable;
        break;
    case StartupCategory::Drivers:
        tableWidget = m_driversTable;
        break;
    case StartupCategory::Tasks:
        tableWidget = m_tasksTable;
        break;
    case StartupCategory::Registry:
        return -1;
    default:
        break;
    }

    if (tableWidget == nullptr || row < 0)
    {
        return -1;
    }

    QTableWidgetItem* nameItem = tableWidget->item(row, toStartupColumn(StartupColumn::Name));
    if (nameItem == nullptr)
    {
        return -1;
    }
    return nameItem->data(Qt::UserRole).toInt();
}

bool StartupDock::entryMatchesCurrentFilter(const StartupEntry& entry) const
{
    const QString keywordText = (m_filterEdit != nullptr) ? m_filterEdit->text().trimmed() : QString();
    const bool hideMicrosoft = (m_hideMicrosoftCheck != nullptr) && m_hideMicrosoftCheck->isChecked();

    if (hideMicrosoft)
    {
        const QString publisherLowerText = entry.publisherText.toLower();
        if (publisherLowerText.contains(QStringLiteral("microsoft"))
            || publisherLowerText.contains(QStringLiteral("windows")))
        {
            return false;
        }
    }

    if (keywordText.isEmpty())
    {
        return true;
    }

    const QString haystackText =
        entry.itemNameText + QLatin1Char('\n')
        + entry.publisherText + QLatin1Char('\n')
        + entry.imagePathText + QLatin1Char('\n')
        + entry.commandText + QLatin1Char('\n')
        + entry.locationText + QLatin1Char('\n')
        + entry.sourceTypeText + QLatin1Char('\n')
        + entry.detailText;
    return haystackText.contains(keywordText, Qt::CaseInsensitive);
}
