#include "HardwareDeviceManagerPage.h"

// ============================================================
// HardwareDeviceManagerPage.cpp
// 作用：
// 1) 使用 SetupAPI/CfgMgr 枚举 PnP 设备；
// 2) 按 InstanceId/Parent 构建设备树，展示类似 System Informer 的列布局；
// 3) 支持当前/全部设备切换、异常高亮、搜索过滤与详情查看。
// ============================================================

#include "../theme.h"
#include "../UI/CodeEditorWidget.h"

#include <QCheckBox>
#include <QDateTime>
#include <QHeaderView>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QMetaObject>
#include <QPointer>
#include <QPushButton>
#include <QHash>
#include <QSignalBlocker>
#include <QSplitter>
#include <QTreeWidget>
#include <QTreeWidgetItem>
#include <QVBoxLayout>
#include <QVariant>

#include <algorithm>
#include <memory>
#include <thread>

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#include <Cfgmgr32.h>
#include <initguid.h>
#include <devpkey.h>
#include <Objbase.h>
#include <SetupAPI.h>

#pragma comment(lib, "Cfgmgr32.lib")
#pragma comment(lib, "Setupapi.lib")

namespace
{
    // DeviceTreeColumn：
    // - 作用：定义设备树表格列索引；
    // - 处理逻辑：插入和更新节点时统一引用；
    // - 返回行为：枚举本身无返回值。
    enum DeviceTreeColumn : int
    {
        ColumnName = 0,
        ColumnManufacturer,
        ColumnService,
        ColumnClass,
        ColumnEnumerator,
        ColumnInstalled,
        ColumnCount
    };

    // propertyString 作用：
    // - 输入：SetupAPI 设备信息句柄、设备数据、属性键；
    // - 处理：读取 DEVPROP_TYPE_STRING 属性；
    // - 返回：成功时返回字符串，失败或类型不匹配时返回空字符串。
    QString propertyString(
        HDEVINFO deviceInfoSet,
        SP_DEVINFO_DATA* deviceInfoData,
        const DEVPROPKEY& propertyKey)
    {
        DEVPROPTYPE propertyType = 0;
        DWORD requiredSize = 0;
        SetupDiGetDevicePropertyW(
            deviceInfoSet,
            deviceInfoData,
            &propertyKey,
            &propertyType,
            nullptr,
            0,
            &requiredSize,
            0);
        if (requiredSize == 0 || propertyType != DEVPROP_TYPE_STRING)
        {
            return QString();
        }

        std::vector<BYTE> buffer(requiredSize + sizeof(wchar_t), 0);
        if (!SetupDiGetDevicePropertyW(
            deviceInfoSet,
            deviceInfoData,
            &propertyKey,
            &propertyType,
            buffer.data(),
            static_cast<DWORD>(buffer.size()),
            nullptr,
            0))
        {
            return QString();
        }

        return QString::fromWCharArray(reinterpret_cast<const wchar_t*>(buffer.data())).trimmed();
    }

    // propertyStringList 作用：
    // - 输入：SetupAPI 设备信息句柄、设备数据、属性键；
    // - 处理：读取 DEVPROP_TYPE_STRING_LIST 多字符串属性；
    // - 返回：使用换行连接的字符串列表，失败时返回空字符串。
    QString propertyStringList(
        HDEVINFO deviceInfoSet,
        SP_DEVINFO_DATA* deviceInfoData,
        const DEVPROPKEY& propertyKey)
    {
        DEVPROPTYPE propertyType = 0;
        DWORD requiredSize = 0;
        SetupDiGetDevicePropertyW(
            deviceInfoSet,
            deviceInfoData,
            &propertyKey,
            &propertyType,
            nullptr,
            0,
            &requiredSize,
            0);
        if (requiredSize == 0 || propertyType != DEVPROP_TYPE_STRING_LIST)
        {
            return QString();
        }

        std::vector<BYTE> buffer(requiredSize + sizeof(wchar_t), 0);
        if (!SetupDiGetDevicePropertyW(
            deviceInfoSet,
            deviceInfoData,
            &propertyKey,
            &propertyType,
            buffer.data(),
            static_cast<DWORD>(buffer.size()),
            nullptr,
            0))
        {
            return QString();
        }

        QStringList valueList;
        const wchar_t* cursor = reinterpret_cast<const wchar_t*>(buffer.data());
        while (cursor != nullptr && *cursor != L'\0')
        {
            const QString valueText = QString::fromWCharArray(cursor).trimmed();
            if (!valueText.isEmpty())
            {
                valueList.append(valueText);
            }
            cursor += wcslen(cursor) + 1;
        }
        return valueList.join(QStringLiteral("\n"));
    }

    // propertyBool 作用：
    // - 输入：SetupAPI 设备信息句柄、设备数据、属性键；
    // - 处理：读取 DEVPROP_TYPE_BOOLEAN 属性；
    // - 返回：成功读取且值为 TRUE 时返回 true。
    bool propertyBool(
        HDEVINFO deviceInfoSet,
        SP_DEVINFO_DATA* deviceInfoData,
        const DEVPROPKEY& propertyKey)
    {
        DEVPROPTYPE propertyType = 0;
        DEVPROP_BOOLEAN value = DEVPROP_FALSE;
        if (!SetupDiGetDevicePropertyW(
            deviceInfoSet,
            deviceInfoData,
            &propertyKey,
            &propertyType,
            reinterpret_cast<PBYTE>(&value),
            sizeof(value),
            nullptr,
            0))
        {
            return false;
        }
        return propertyType == DEVPROP_TYPE_BOOLEAN && value == DEVPROP_TRUE;
    }

    // propertyUInt32 作用：
    // - 输入：SetupAPI 设备信息句柄、设备数据、属性键；
    // - 处理：读取 DEVPROP_TYPE_UINT32 属性；
    // - 返回：成功时返回数值，否则返回 0。
    unsigned long propertyUInt32(
        HDEVINFO deviceInfoSet,
        SP_DEVINFO_DATA* deviceInfoData,
        const DEVPROPKEY& propertyKey)
    {
        DEVPROPTYPE propertyType = 0;
        unsigned long value = 0;
        if (!SetupDiGetDevicePropertyW(
            deviceInfoSet,
            deviceInfoData,
            &propertyKey,
            &propertyType,
            reinterpret_cast<PBYTE>(&value),
            sizeof(value),
            nullptr,
            0))
        {
            return 0;
        }
        return propertyType == DEVPROP_TYPE_UINT32 ? value : 0;
    }

    // propertyFileTimeText 作用：
    // - 输入：SetupAPI 设备信息句柄、设备数据、属性键；
    // - 处理：读取 DEVPROP_TYPE_FILETIME 并转成本地时间文本；
    // - 返回：成功时返回 yyyy-MM-dd HH:mm:ss，失败时返回空字符串。
    QString propertyFileTimeText(
        HDEVINFO deviceInfoSet,
        SP_DEVINFO_DATA* deviceInfoData,
        const DEVPROPKEY& propertyKey)
    {
        DEVPROPTYPE propertyType = 0;
        FILETIME fileTime{};
        if (!SetupDiGetDevicePropertyW(
            deviceInfoSet,
            deviceInfoData,
            &propertyKey,
            &propertyType,
            reinterpret_cast<PBYTE>(&fileTime),
            sizeof(fileTime),
            nullptr,
            0)
            || propertyType != DEVPROP_TYPE_FILETIME)
        {
            return QString();
        }

        FILETIME localFileTime{};
        SYSTEMTIME systemTime{};
        if (!FileTimeToLocalFileTime(&fileTime, &localFileTime)
            || !FileTimeToSystemTime(&localFileTime, &systemTime))
        {
            return QString();
        }

        return QStringLiteral("%1-%2-%3 %4:%5:%6")
            .arg(systemTime.wYear, 4, 10, QLatin1Char('0'))
            .arg(systemTime.wMonth, 2, 10, QLatin1Char('0'))
            .arg(systemTime.wDay, 2, 10, QLatin1Char('0'))
            .arg(systemTime.wHour, 2, 10, QLatin1Char('0'))
            .arg(systemTime.wMinute, 2, 10, QLatin1Char('0'))
            .arg(systemTime.wSecond, 2, 10, QLatin1Char('0'));
    }

    // propertyGuidText 作用：
    // - 输入：SetupAPI 设备信息句柄、设备数据、属性键；
    // - 处理：读取 DEVPROP_TYPE_GUID 并格式化为大括号 GUID；
    // - 返回：成功时返回 GUID 文本，否则返回空字符串。
    QString propertyGuidText(
        HDEVINFO deviceInfoSet,
        SP_DEVINFO_DATA* deviceInfoData,
        const DEVPROPKEY& propertyKey)
    {
        DEVPROPTYPE propertyType = 0;
        GUID guidValue{};
        if (!SetupDiGetDevicePropertyW(
            deviceInfoSet,
            deviceInfoData,
            &propertyKey,
            &propertyType,
            reinterpret_cast<PBYTE>(&guidValue),
            sizeof(guidValue),
            nullptr,
            0)
            || propertyType != DEVPROP_TYPE_GUID)
        {
            return QString();
        }

        wchar_t buffer[64] = {};
        if (StringFromGUID2(guidValue, buffer, static_cast<int>(std::size(buffer))) <= 0)
        {
            return QString();
        }
        return QString::fromWCharArray(buffer);
    }

    // instanceIdFromDevInst 作用：
    // - 输入：CfgMgr devinst；
    // - 处理：调用 CM_Get_Device_IDW 获取实例 ID；
    // - 返回：成功时返回实例 ID，否则返回空字符串。
    QString instanceIdFromDevInst(DEVINST devInst)
    {
        ULONG charCount = 0;
        if (CM_Get_Device_ID_Size(&charCount, devInst, 0) != CR_SUCCESS || charCount == 0)
        {
            return QString();
        }

        std::vector<wchar_t> buffer(static_cast<std::size_t>(charCount) + 2U, L'\0');
        if (CM_Get_Device_IDW(devInst, buffer.data(), static_cast<ULONG>(buffer.size()), 0) != CR_SUCCESS)
        {
            return QString();
        }
        return QString::fromWCharArray(buffer.data()).trimmed();
    }

    // deviceStatusText 作用：
    // - 输入：CfgMgr 状态和问题码；
    // - 处理：生成详情区可读状态摘要；
    // - 返回：状态字符串。
    QString deviceStatusText(const ULONG statusFlags, const ULONG problemCode)
    {
        QStringList partList;
        partList.append(QStringLiteral("DN=0x%1").arg(statusFlags, 8, 16, QLatin1Char('0')));
        if ((statusFlags & DN_STARTED) != 0)
        {
            partList.append(QStringLiteral("STARTED"));
        }
        if ((statusFlags & DN_HAS_PROBLEM) != 0)
        {
            partList.append(QStringLiteral("HAS_PROBLEM"));
        }
        if ((statusFlags & DN_DISABLEABLE) != 0)
        {
            partList.append(QStringLiteral("DISABLEABLE"));
        }
        if ((statusFlags & DN_REMOVABLE) != 0)
        {
            partList.append(QStringLiteral("REMOVABLE"));
        }
        if ((statusFlags & DN_PRIVATE_PROBLEM) != 0)
        {
            partList.append(QStringLiteral("PRIVATE_PROBLEM"));
        }
        if (problemCode != 0)
        {
            partList.append(QStringLiteral("Problem=%1").arg(problemCode));
        }
        return partList.join(QStringLiteral(" | "));
    }

    // safeDisplayText 作用：
    // - 输入：候选文本；
    // - 处理：空值统一展示为 '-'，避免表格大片空白难以阅读；
    // - 返回：可展示文本。
    QString safeDisplayText(const QString& valueText)
    {
        return valueText.trimmed().isEmpty() ? QStringLiteral("-") : valueText.trimmed();
    }

    // itemMatchesFilter 作用：
    // - 输入：设备项和小写过滤文本；
    // - 处理：在主要列和实例 ID 中做包含匹配；
    // - 返回：匹配时 true，空过滤条件永远 true。
    bool itemMatchesFilter(
        const HardwareDeviceManagerPage::DeviceEntry& entry,
        const QString& lowerFilterText)
    {
        if (lowerFilterText.isEmpty())
        {
            return true;
        }

        const QString joinedText = QStringLiteral("%1\n%2\n%3\n%4\n%5\n%6\n%7\n%8")
            .arg(entry.nameText)
            .arg(entry.manufacturerText)
            .arg(entry.serviceText)
            .arg(entry.classText)
            .arg(entry.enumeratorText)
            .arg(entry.instanceIdText)
            .arg(entry.parentInstanceIdText)
            .arg(entry.hardwareIdsText)
            .toLower();
        return joinedText.contains(lowerFilterText);
    }
}

HardwareDeviceManagerPage::HardwareDeviceManagerPage(QWidget* parent)
    : QWidget(parent)
{
    initializeUi();
    initializeConnections();
    refreshDevicesAsync(false);
}

void HardwareDeviceManagerPage::initializeUi()
{
    m_rootLayout = new QVBoxLayout(this);
    m_rootLayout->setContentsMargins(6, 6, 6, 6);
    m_rootLayout->setSpacing(6);

    QHBoxLayout* headerLayout = new QHBoxLayout();
    headerLayout->setContentsMargins(0, 0, 0, 0);
    headerLayout->setSpacing(8);

    QLabel* titleLabel = new QLabel(QStringLiteral("设备管理"), this);
    titleLabel->setStyleSheet(
        QStringLiteral("font-size:18px;font-weight:700;color:%1;")
        .arg(KswordTheme::TextPrimaryHex()));
    headerLayout->addWidget(titleLabel, 0);

    m_statusLabel = new QLabel(QStringLiteral("正在枚举当前设备..."), this);
    m_statusLabel->setStyleSheet(
        QStringLiteral("font-size:13px;color:%1;").arg(KswordTheme::TextSecondaryHex()));
    headerLayout->addWidget(m_statusLabel, 1);

    m_showAllDevicesCheck = new QCheckBox(QStringLiteral("显示全部设备"), this);
    m_showAllDevicesCheck->setToolTip(QStringLiteral("关闭时仅枚举当前存在设备；开启后包含历史/非当前设备。"));
    headerLayout->addWidget(m_showAllDevicesCheck, 0);

    m_searchEdit = new QLineEdit(this);
    m_searchEdit->setPlaceholderText(QStringLiteral("搜索名称、厂商、服务、类、枚举器、Instance ID..."));
    m_searchEdit->setMinimumWidth(220);
    headerLayout->addWidget(m_searchEdit, 1);

    m_refreshButton = new QPushButton(QStringLiteral("刷新"), this);
    m_refreshButton->setToolTip(QStringLiteral("重新通过 SetupAPI/CfgMgr 枚举 PnP 设备树"));
    headerLayout->addWidget(m_refreshButton, 0);
    m_rootLayout->addLayout(headerLayout, 0);

    m_splitter = new QSplitter(Qt::Vertical, this);
    m_splitter->setChildrenCollapsible(false);
    m_rootLayout->addWidget(m_splitter, 1);

    m_deviceTree = new QTreeWidget(m_splitter);
    m_deviceTree->setColumnCount(ColumnCount);
    m_deviceTree->setHeaderLabels({
        QStringLiteral("Name"),
        QStringLiteral("Manufacturer"),
        QStringLiteral("Service"),
        QStringLiteral("Class"),
        QStringLiteral("Enumerator"),
        QStringLiteral("Installed")
        });
    m_deviceTree->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_deviceTree->setSelectionMode(QAbstractItemView::SingleSelection);
    m_deviceTree->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_deviceTree->setUniformRowHeights(true);
    m_deviceTree->setAlternatingRowColors(true);
    m_deviceTree->header()->setStretchLastSection(false);
    m_deviceTree->header()->setSectionResizeMode(ColumnName, QHeaderView::Interactive);
    m_deviceTree->header()->setSectionResizeMode(ColumnManufacturer, QHeaderView::Interactive);
    m_deviceTree->header()->setSectionResizeMode(ColumnService, QHeaderView::ResizeToContents);
    m_deviceTree->header()->setSectionResizeMode(ColumnClass, QHeaderView::ResizeToContents);
    m_deviceTree->header()->setSectionResizeMode(ColumnEnumerator, QHeaderView::ResizeToContents);
    m_deviceTree->header()->setSectionResizeMode(ColumnInstalled, QHeaderView::ResizeToContents);
    m_deviceTree->setColumnWidth(ColumnName, 360);
    m_deviceTree->setColumnWidth(ColumnManufacturer, 190);
    m_splitter->addWidget(m_deviceTree);

    m_detailEditor = new CodeEditorWidget(m_splitter);
    m_detailEditor->setReadOnly(true);
    m_detailEditor->setText(QStringLiteral("选择一个设备查看详细属性。"));
    m_splitter->addWidget(m_detailEditor);
    m_splitter->setStretchFactor(0, 4);
    m_splitter->setStretchFactor(1, 1);
}

void HardwareDeviceManagerPage::initializeConnections()
{
    connect(m_refreshButton, &QPushButton::clicked, this, [this]()
    {
        refreshDevicesAsync(true);
    });

    connect(m_showAllDevicesCheck, &QCheckBox::toggled, this, [this](const bool)
    {
        refreshDevicesAsync(true);
    });

    connect(m_searchEdit, &QLineEdit::textChanged, this, [this](const QString&)
    {
        if (m_deviceTree == nullptr)
        {
            return;
        }
        const QString filterText = m_searchEdit != nullptr ? m_searchEdit->text().trimmed().toLower() : QString();
        for (int index = 0; index < m_deviceTree->topLevelItemCount(); ++index)
        {
            applyFilterToTree(m_deviceTree->topLevelItem(index), filterText);
        }
        if (!filterText.isEmpty())
        {
            m_deviceTree->expandAll();
        }
    });

    connect(
        m_deviceTree,
        &QTreeWidget::currentItemChanged,
        this,
        [this](QTreeWidgetItem* currentItem, QTreeWidgetItem*)
        {
            updateDetailForItem(currentItem);
        });
}

void HardwareDeviceManagerPage::refreshDevicesAsync(const bool forceRefresh)
{
    bool expectedValue = false;
    if (!m_refreshing.compare_exchange_strong(expectedValue, true))
    {
        if (forceRefresh && m_statusLabel != nullptr)
        {
            m_statusLabel->setText(QStringLiteral("正在刷新，请等待当前枚举完成。"));
        }
        return;
    }

    const bool includeAllDevices = m_showAllDevicesCheck != nullptr && m_showAllDevicesCheck->isChecked();
    if (m_statusLabel != nullptr)
    {
        m_statusLabel->setText(includeAllDevices
            ? QStringLiteral("正在枚举全部设备...")
            : QStringLiteral("正在枚举当前设备..."));
    }
    if (m_refreshButton != nullptr)
    {
        m_refreshButton->setEnabled(false);
    }
    if (m_showAllDevicesCheck != nullptr)
    {
        m_showAllDevicesCheck->setEnabled(false);
    }

    QPointer<HardwareDeviceManagerPage> safeThis(this);
    std::thread([safeThis, includeAllDevices]()
    {
        std::vector<DeviceEntry> deviceList = enumerateDevicesSnapshot(includeAllDevices);
        if (safeThis.isNull())
        {
            return;
        }

        const bool invokeOk = QMetaObject::invokeMethod(
            safeThis.data(),
            [safeThis, deviceList = std::move(deviceList), includeAllDevices]() mutable
            {
                if (safeThis.isNull())
                {
                    return;
                }

                safeThis->m_deviceList = std::move(deviceList);
                safeThis->rebuildDeviceTree(safeThis->m_deviceList);

                int problemCount = 0;
                for (const DeviceEntry& entry : safeThis->m_deviceList)
                {
                    if (entry.hasProblem)
                    {
                        ++problemCount;
                    }
                }
                if (safeThis->m_statusLabel != nullptr)
                {
                    safeThis->m_statusLabel->setText(
                        QStringLiteral("%1设备：%2 项，异常：%3，刷新：%4")
                        .arg(includeAllDevices ? QStringLiteral("全部") : QStringLiteral("当前"))
                        .arg(static_cast<int>(safeThis->m_deviceList.size()))
                        .arg(problemCount)
                        .arg(QDateTime::currentDateTime().toString(QStringLiteral("HH:mm:ss"))));
                }
                if (safeThis->m_refreshButton != nullptr)
                {
                    safeThis->m_refreshButton->setEnabled(true);
                }
                if (safeThis->m_showAllDevicesCheck != nullptr)
                {
                    safeThis->m_showAllDevicesCheck->setEnabled(true);
                }
                safeThis->m_refreshing.store(false);
            },
            Qt::QueuedConnection);

        if (!invokeOk && !safeThis.isNull())
        {
            safeThis->m_refreshing.store(false);
        }
    }).detach();
}

void HardwareDeviceManagerPage::rebuildDeviceTree(const std::vector<DeviceEntry>& deviceList)
{
    if (m_deviceTree == nullptr)
    {
        return;
    }

    m_deviceTree->clear();
    if (deviceList.empty())
    {
        if (m_detailEditor != nullptr)
        {
            m_detailEditor->setText(QStringLiteral("未枚举到设备。"));
        }
        return;
    }

    QSignalBlocker treeSignalBlocker(m_deviceTree);
    QHash<QString, QTreeWidgetItem*> itemByInstanceId;
    itemByInstanceId.reserve(static_cast<qsizetype>(deviceList.size()));
    std::vector<std::unique_ptr<QTreeWidgetItem>> itemOwnerList;
    itemOwnerList.reserve(deviceList.size());

    const QColor problemForeground = QColor(235, 77, 92);
    const QColor missingForeground = KswordTheme::IsDarkModeEnabled()
        ? QColor(150, 160, 170)
        : QColor(120, 120, 120);
    const QColor normalForeground = QColor();

    for (const DeviceEntry& entry : deviceList)
    {
        std::unique_ptr<QTreeWidgetItem> itemPointer = std::make_unique<QTreeWidgetItem>();
        itemPointer->setText(ColumnName, safeDisplayText(entry.nameText));
        itemPointer->setText(ColumnManufacturer, safeDisplayText(entry.manufacturerText));
        itemPointer->setText(ColumnService, safeDisplayText(entry.serviceText));
        itemPointer->setText(ColumnClass, safeDisplayText(entry.classText));
        itemPointer->setText(ColumnEnumerator, safeDisplayText(entry.enumeratorText));
        itemPointer->setText(ColumnInstalled, safeDisplayText(entry.installedText));
        itemPointer->setToolTip(ColumnName, entry.instanceIdText);
        itemPointer->setData(
            ColumnName,
            Qt::UserRole,
            QVariant::fromValue(reinterpret_cast<quintptr>(&entry)));

        if (entry.hasProblem)
        {
            for (int column = 0; column < ColumnCount; ++column)
            {
                itemPointer->setForeground(column, problemForeground);
            }
            itemPointer->setToolTip(
                ColumnName,
                QStringLiteral("%1\n%2").arg(entry.instanceIdText, entry.problemText));
        }
        else if (!entry.isPresent)
        {
            for (int column = 0; column < ColumnCount; ++column)
            {
                itemPointer->setForeground(column, missingForeground);
            }
        }
        else
        {
            Q_UNUSED(normalForeground);
        }

        if (!entry.instanceIdText.isEmpty())
        {
            itemByInstanceId.insert(entry.instanceIdText.toLower(), itemPointer.get());
        }
        itemOwnerList.push_back(std::move(itemPointer));
    }

    for (std::size_t index = 0; index < deviceList.size(); ++index)
    {
        QTreeWidgetItem* itemPointer = itemOwnerList[index].get();
        const QString parentKey = deviceList[index].parentInstanceIdText.toLower();
        QTreeWidgetItem* parentItem = nullptr;
        if (!parentKey.isEmpty())
        {
            const auto parentIt = itemByInstanceId.constFind(parentKey);
            if (parentIt != itemByInstanceId.constEnd() && parentIt.value() != itemPointer)
            {
                parentItem = parentIt.value();
            }
        }

        if (parentItem != nullptr)
        {
            parentItem->addChild(itemOwnerList[index].release());
        }
        else
        {
            m_deviceTree->addTopLevelItem(itemOwnerList[index].release());
        }
    }

    m_deviceTree->sortItems(ColumnName, Qt::AscendingOrder);

    const QString filterText = m_searchEdit != nullptr ? m_searchEdit->text().trimmed().toLower() : QString();
    for (int index = 0; index < m_deviceTree->topLevelItemCount(); ++index)
    {
        applyFilterToTree(m_deviceTree->topLevelItem(index), filterText);
    }

    if (deviceList.size() <= 300 || !filterText.isEmpty())
    {
        m_deviceTree->expandToDepth(filterText.isEmpty() ? 1 : 99);
    }
    treeSignalBlocker.unblock();
    if (m_deviceTree->topLevelItemCount() > 0)
    {
        m_deviceTree->setCurrentItem(m_deviceTree->topLevelItem(0));
    }
}

bool HardwareDeviceManagerPage::applyFilterToTree(QTreeWidgetItem* itemPointer, const QString& filterText)
{
    if (itemPointer == nullptr)
    {
        return false;
    }

    bool childMatched = false;
    for (int childIndex = 0; childIndex < itemPointer->childCount(); ++childIndex)
    {
        if (applyFilterToTree(itemPointer->child(childIndex), filterText))
        {
            childMatched = true;
        }
    }

    const quintptr rawPointer = itemPointer->data(ColumnName, Qt::UserRole).value<quintptr>();
    const DeviceEntry* entryPointer = reinterpret_cast<const DeviceEntry*>(rawPointer);
    const bool selfMatched = entryPointer != nullptr && itemMatchesFilter(*entryPointer, filterText);
    const bool visible = filterText.isEmpty() || selfMatched || childMatched;
    itemPointer->setHidden(!visible);
    return visible;
}

void HardwareDeviceManagerPage::updateDetailForItem(QTreeWidgetItem* itemPointer)
{
    if (m_detailEditor == nullptr)
    {
        return;
    }
    if (itemPointer == nullptr)
    {
        m_detailEditor->setText(QStringLiteral("选择一个设备查看详细属性。"));
        return;
    }

    const quintptr rawPointer = itemPointer->data(ColumnName, Qt::UserRole).value<quintptr>();
    const DeviceEntry* entryPointer = reinterpret_cast<const DeviceEntry*>(rawPointer);
    if (entryPointer == nullptr)
    {
        m_detailEditor->setText(QStringLiteral("当前设备节点没有详情。"));
        return;
    }

    const DeviceEntry& entry = *entryPointer;
    const QString detailText = QStringLiteral(
        "Name: %1\n"
        "Manufacturer: %2\n"
        "Service: %3\n"
        "Class: %4\n"
        "Enumerator: %5\n"
        "Installed: %6\n"
        "Present: %7\n"
        "HasProblem: %8\n"
        "Problem: %9\n"
        "Status: %10\n\n"
        "Instance ID:\n%11\n\n"
        "Parent Instance ID:\n%12\n\n"
        "Class GUID:\n%13\n\n"
        "Driver:\n%14\n\n"
        "Location:\n%15\n\n"
        "Hardware IDs:\n%16\n\n"
        "Compatible IDs:\n%17\n")
        .arg(safeDisplayText(entry.nameText))
        .arg(safeDisplayText(entry.manufacturerText))
        .arg(safeDisplayText(entry.serviceText))
        .arg(safeDisplayText(entry.classText))
        .arg(safeDisplayText(entry.enumeratorText))
        .arg(safeDisplayText(entry.installedText))
        .arg(entry.isPresent ? QStringLiteral("Yes") : QStringLiteral("No"))
        .arg(entry.hasProblem ? QStringLiteral("Yes") : QStringLiteral("No"))
        .arg(safeDisplayText(entry.problemText))
        .arg(safeDisplayText(entry.statusText))
        .arg(safeDisplayText(entry.instanceIdText))
        .arg(safeDisplayText(entry.parentInstanceIdText))
        .arg(safeDisplayText(entry.classGuidText))
        .arg(safeDisplayText(entry.driverText))
        .arg(safeDisplayText(entry.locationText))
        .arg(safeDisplayText(entry.hardwareIdsText))
        .arg(safeDisplayText(entry.compatibleIdsText));
    m_detailEditor->setText(detailText);
}

std::vector<HardwareDeviceManagerPage::DeviceEntry>
HardwareDeviceManagerPage::enumerateDevicesSnapshot(const bool includeAllDevices)
{
    std::vector<DeviceEntry> resultList;

    const DWORD flags = DIGCF_ALLCLASSES | (includeAllDevices ? 0U : DIGCF_PRESENT);
    HDEVINFO deviceInfoSet = SetupDiGetClassDevsW(nullptr, nullptr, nullptr, flags);
    if (deviceInfoSet == INVALID_HANDLE_VALUE)
    {
        return resultList;
    }

    for (DWORD index = 0;; ++index)
    {
        SP_DEVINFO_DATA deviceInfoData{};
        deviceInfoData.cbSize = sizeof(deviceInfoData);
        if (!SetupDiEnumDeviceInfo(deviceInfoSet, index, &deviceInfoData))
        {
            break;
        }

        DeviceEntry entry;
        entry.nameText = propertyString(deviceInfoSet, &deviceInfoData, DEVPKEY_Device_FriendlyName);
        if (entry.nameText.isEmpty())
        {
            entry.nameText = propertyString(deviceInfoSet, &deviceInfoData, DEVPKEY_Device_DeviceDesc);
        }
        entry.manufacturerText = propertyString(deviceInfoSet, &deviceInfoData, DEVPKEY_Device_Manufacturer);
        entry.serviceText = propertyString(deviceInfoSet, &deviceInfoData, DEVPKEY_Device_Service);
        entry.classText = propertyString(deviceInfoSet, &deviceInfoData, DEVPKEY_Device_Class);
        entry.enumeratorText = propertyString(deviceInfoSet, &deviceInfoData, DEVPKEY_Device_EnumeratorName);
        entry.installedText = propertyFileTimeText(deviceInfoSet, &deviceInfoData, DEVPKEY_Device_InstallDate);
        entry.instanceIdText = propertyString(deviceInfoSet, &deviceInfoData, DEVPKEY_Device_InstanceId);
        entry.parentInstanceIdText = propertyString(deviceInfoSet, &deviceInfoData, DEVPKEY_Device_Parent);
        entry.classGuidText = propertyGuidText(deviceInfoSet, &deviceInfoData, DEVPKEY_Device_ClassGuid);
        entry.driverText = propertyString(deviceInfoSet, &deviceInfoData, DEVPKEY_Device_Driver);
        const QString driverInfText = propertyString(deviceInfoSet, &deviceInfoData, DEVPKEY_Device_DriverInfPath);
        if (!driverInfText.isEmpty())
        {
            entry.driverText = entry.driverText.isEmpty()
                ? driverInfText
                : QStringLiteral("%1 | INF=%2").arg(entry.driverText, driverInfText);
        }
        entry.locationText = propertyString(deviceInfoSet, &deviceInfoData, DEVPKEY_Device_LocationInfo);
        entry.hardwareIdsText = propertyStringList(deviceInfoSet, &deviceInfoData, DEVPKEY_Device_HardwareIds);
        entry.compatibleIdsText = propertyStringList(deviceInfoSet, &deviceInfoData, DEVPKEY_Device_CompatibleIds);
        entry.isPresent = propertyBool(deviceInfoSet, &deviceInfoData, DEVPKEY_Device_IsPresent);
        if (!includeAllDevices)
        {
            entry.isPresent = true;
        }
        entry.problemCode = propertyUInt32(deviceInfoSet, &deviceInfoData, DEVPKEY_Device_ProblemCode);
        entry.hasProblem = propertyBool(deviceInfoSet, &deviceInfoData, DEVPKEY_Device_HasProblem)
            || entry.problemCode != 0;

        ULONG statusFlags = 0;
        ULONG problemCode = entry.problemCode;
        if (CM_Get_DevNode_Status(&statusFlags, &problemCode, deviceInfoData.DevInst, 0) == CR_SUCCESS)
        {
            entry.problemCode = problemCode;
            entry.hasProblem = entry.hasProblem || ((statusFlags & DN_HAS_PROBLEM) != 0) || problemCode != 0;
            entry.statusText = deviceStatusText(statusFlags, problemCode);
        }
        if (entry.problemCode != 0)
        {
            entry.problemText = QStringLiteral("CM_PROB=%1").arg(entry.problemCode);
        }

        if (entry.instanceIdText.isEmpty())
        {
            entry.instanceIdText = instanceIdFromDevInst(deviceInfoData.DevInst);
        }
        if (!entry.instanceIdText.isEmpty())
        {
            resultList.push_back(std::move(entry));
        }
    }

    SetupDiDestroyDeviceInfoList(deviceInfoSet);
    std::sort(
        resultList.begin(),
        resultList.end(),
        [](const DeviceEntry& left, const DeviceEntry& right)
        {
            return QString::localeAwareCompare(left.nameText, right.nameText) < 0;
        });
    return resultList;
}
