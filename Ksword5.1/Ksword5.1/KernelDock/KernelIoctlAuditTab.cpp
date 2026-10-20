#include "KernelIoctlAuditTab.h"
#include "../UI/TableContextMenu.h"
#include "../UI/PageControlStyle.h"
#include "../UI/ToolbarMetrics.h"

#include "KernelDeviceDriverObjectsWorker.h"
#include "KernelDock.h"
#include "../ArkDriverClient/ArkDriverClient.h"
#include "../UI/TableInteractionSupport.h"
#include "../UI/TableHeaderSortingSupport.h"
#include "../UI/TableRowFilterSupport.h"
#include "../UI/VisibleTableWidget.h"
#include "../theme.h"

#include <QAbstractItemView>
#include <QAbstractTableModel>
#include <QSortFilterProxyModel>
#include <QShowEvent>
#include <QEvent>
#include <QTimer>
#include <QHash>
#include <QAction>
#include <QApplication>
#include <QClipboard>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QMetaObject>
#include <QModelIndex>
#include <QPointer>
#include <QPersistentModelIndex>
#include <QPushButton>
#include <QSet>
#include <QTableView>
#include <QTableWidgetItem>
#include <QTabWidget>
#include <QVBoxLayout>

#include <algorithm>
#include <sstream>
#include <thread>
#include <utility>

using ksword::kernel_dock_internal::kernelText;

namespace
{
    QString buttonStyle()
    {
        return KswordTheme::ThemedButtonStyle();
    }

    QString headerStyle()
    {
        return QStringLiteral("QHeaderView::section{color:%1;background:%2;border:0;border-bottom:1px solid %3;font-weight:500;padding:4px 8px;}")
            .arg(KswordTheme::TextPrimaryHex(), KswordTheme::SurfaceAltHex(), KswordTheme::BorderHex());
    }


}

class KernelIoctlAuditTab::AuditModel final : public QAbstractTableModel
{
public:
    explicit AuditModel(int kind, QObject* parent) : QAbstractTableModel(parent), m_kind(kind) {}
    void setHeaders(QStringList headers) { beginResetModel(); m_headers = std::move(headers); endResetModel(); }
    void setSnapshot(std::shared_ptr<const Snapshot> snapshot)
    {
        beginResetModel();
        m_snapshot = std::move(snapshot);
        m_texts.clear();
        endResetModel();
    }
    int rowCount(const QModelIndex& parent = {}) const override
    {
        if (parent.isValid() || !m_snapshot) return 0;
        switch (m_kind)
        {
        case 0: return static_cast<int>(m_snapshot->driverRows.size());
        case 1: return static_cast<int>(m_snapshot->deviceRows.size());
        case 2: return static_cast<int>(m_snapshot->dispatchRows.size());
        case 3: return static_cast<int>(m_snapshot->registryRows.size());
        default: return 0;
        }
    }
    int columnCount(const QModelIndex& parent = {}) const override { return parent.isValid() ? 0 : m_headers.size(); }
    QVariant headerData(int section, Qt::Orientation orientation, int role = Qt::DisplayRole) const override
    {
        if (orientation != Qt::Horizontal || section < 0 || section >= m_headers.size()) return {};
        return role == Qt::DisplayRole ? QVariant(m_headers.value(section)) : m_headerRoles.value(section).value(role);
    }
    bool setHeaderData(int section, Qt::Orientation orientation, const QVariant& value, int role) override
    {
        if (orientation != Qt::Horizontal || section < 0 || section >= m_headers.size()) return false;
        if (role == Qt::DisplayRole || role == Qt::EditRole) m_headers[section] = value.toString();
        else m_headerRoles[section][role] = value;
        emit headerDataChanged(orientation, section, section);
        return true;
    }
    void clearFormattedRows()
    {
        m_texts.clear();
        if (rowCount() > 0 && columnCount() > 0)
            emit dataChanged(index(0, 0), index(rowCount() - 1, columnCount() - 1), {Qt::DisplayRole, Qt::ToolTipRole});
    }
    QVariant data(const QModelIndex& index, int role = Qt::DisplayRole) const override
    {
        if (!index.isValid() || index.row() < 0 || index.row() >= rowCount() || index.column() < 0 || index.column() >= columnCount()) return {};
        if (role == Qt::UserRole) return index.row();
        if (role != Qt::DisplayRole && role != Qt::ToolTipRole) return {};
        return rowTexts(index.row()).value(index.column());
    }
    bool matches(int row, const QString& needle) const
    {
        if (needle.isEmpty()) return true;
        for (const auto& value : rowTexts(row)) if (value.contains(needle, Qt::CaseInsensitive)) return true;
        return false;
    }
private:
    const QStringList& rowTexts(int sourceRow) const
    {
        auto found = m_texts.constFind(sourceRow);
        if (found != m_texts.cend()) return found.value();
        QStringList values;
        switch (m_kind)
        {
        case 0:
        {
            const auto& row = m_snapshot->driverRows.at(static_cast<std::size_t>(sourceRow));
QString statusText;

        if (!row.status.isEmpty())

        {

            statusText = kernelText(

                "kernel.ioctl_audit.query.failed",

                QStringLiteral("查询失败：%1")).arg(row.status);

        }

        else if (row.queryStatus == KSWORD_ARK_DRIVER_OBJECT_QUERY_STATUS_PARTIAL)

        {

            statusText = kernelText("kernel.ioctl_audit.query.partial", QStringLiteral("部分结果"));

        }

        else if (row.queryStatus == KSWORD_ARK_DRIVER_OBJECT_QUERY_STATUS_OK)

        {

            statusText = kernelText("kernel.ioctl_audit.query.ok", QStringLiteral("正常"));

        }

        else

        {

            statusText = QStringLiteral("QueryStatus=%1").arg(row.queryStatus);

        }
            values = { row.driverName,
                hex64(row.driverObjectAddress),
                hex64(row.driverStart),
                hex32(row.driverSize),
                hex32(row.driverFlags),


            QStringLiteral("%1/%2").arg(row.returnedDeviceCount).arg(row.totalDeviceCount),
                QString::number(row.majorFunctionCount),
                statusText,
                hex32(static_cast<std::uint32_t>(row.lastStatus)) };
            break;
        }
        case 1:
        {
            const auto& row = m_snapshot->deviceRows.at(static_cast<std::size_t>(sourceRow));
const QString relation = row.relationDepth == 0

            ? kernelText("kernel.ioctl_audit.device.root", QStringLiteral("根设备"))

            : kernelText("kernel.ioctl_audit.device.attached", QStringLiteral("附加 +%1")).arg(row.relationDepth);
            values = { row.driverName,
                relation,
                hex64(row.deviceObjectAddress),
                row.deviceName.isEmpty()

            ? kernelText("kernel.ioctl_audit.device.unnamed", QStringLiteral("（未命名）"))

            : row.deviceName,
                hex32(row.deviceType),
                hex32(row.flags),
                hex32(row.characteristics),
                QString::number(row.stackSize),
                hex64(row.rootDeviceObjectAddress),
                hex64(row.nextDeviceObjectAddress),
                hex64(row.attachedDeviceObjectAddress),
                hex64(row.ownerDriverObjectAddress),
                hex32(static_cast<std::uint32_t>(row.nameStatus)) };
            break;
        }
        case 2:
        {
            const auto& row = m_snapshot->dispatchRows.at(static_cast<std::size_t>(sourceRow));
QString dispatchStatus = (row.flags & KSWORD_ARK_DRIVER_DISPATCH_FLAG_OWN_IMAGE) != 0U

            ? kernelText("kernel.ioctl_audit.dispatch.own_image", QStringLiteral("本驱动镜像"))

            : ((row.flags & KSWORD_ARK_DRIVER_DISPATCH_FLAG_MODULE_RESOLVED) != 0U

                ? kernelText("kernel.ioctl_audit.dispatch.external_module", QStringLiteral("外部模块"))

                : kernelText("kernel.ioctl_audit.dispatch.unresolved", QStringLiteral("模块未解析")));

        if (row.isStartIo && row.startIoState == KSWORD_ARK_DRIVER_START_IO_STATE_NULL)

        {

            dispatchStatus = kernelText("kernel.ioctl_audit.dispatch.start_io_null", QStringLiteral("空值（未使用 StartIo）"));

        }

        else if (row.isStartIo && row.startIoState == KSWORD_ARK_DRIVER_START_IO_STATE_READ_FAILED)

        {

            dispatchStatus = kernelText("kernel.ioctl_audit.dispatch.start_io_read_failed", QStringLiteral("读取失败"));

        }

        const bool hasStartIoAddress = row.isStartIo && row.startIoState == KSWORD_ARK_DRIVER_START_IO_STATE_PRESENT;
            values = { row.driverName,
                hex64(row.driverObjectAddress),
                row.isStartIo

            ? QStringLiteral("DriverStartIo")

            : majorFunctionName(row.majorFunction),
                row.isStartIo

            ? QStringLiteral("-")

            : QString::number(row.majorFunction),
                row.isStartIo && !hasStartIoAddress

            ? QStringLiteral("-")

            : hex64(row.dispatchAddress),
                row.isStartIo && !hasStartIoAddress

            ? QStringLiteral("-")

            : hex64(row.moduleBase),
                row.moduleName.isEmpty() ? QStringLiteral("-") : row.moduleName,
                hex32(row.flags),
                dispatchStatus };
            break;
        }
        case 3:
        {
            const auto& row = m_snapshot->registryRows.at(static_cast<std::size_t>(sourceRow));

            values = { hex32(row.ioControlCode),
                QString::number(row.functionNumber),
                QStringLiteral("%1 / %2").arg(row.method).arg(row.access),
                hex64(row.requiredCapability),
                hex64(row.handlerAddress),
                QString::fromStdString(row.name),
                hex32(row.flags) };
            break;
        }
        }
        return m_texts.insert(sourceRow, std::move(values)).value();
    }
    int m_kind = 0;
    QStringList m_headers;
    QHash<int, QHash<int, QVariant>> m_headerRoles;
    std::shared_ptr<const Snapshot> m_snapshot;
    mutable QHash<int, QStringList> m_texts;
};

class KernelIoctlAuditTab::AuditFilter final : public QSortFilterProxyModel
{
public:
    explicit AuditFilter(AuditModel* source, QObject* parent) : QSortFilterProxyModel(parent), m_source(source)
    {
        // QTableWidgetItem text ordering is a case-sensitive locale comparison.
        // Preserve its one-shot policy instead of re-sorting on each source edit.
        setDynamicSortFilter(false);
        setSortRole(Qt::DisplayRole);
        setSortCaseSensitivity(Qt::CaseSensitive);
        setSortLocaleAware(true);
        setSourceModel(source);
    }
    void setNeedle(const QString& needle)
    {
        if (m_needle == needle) return;
        m_needle = needle;
        invalidateFilter();
    }
protected:
    bool filterAcceptsRow(int sourceRow, const QModelIndex&) const override { return m_source->matches(sourceRow, m_needle); }
private:
    AuditModel* m_source = nullptr;
    QString m_needle;
};

KernelIoctlAuditTab::KernelIoctlAuditTab(QWidget* parent)
    : QWidget(parent)
{
    initializeUi();
}

void KernelIoctlAuditTab::requestInitialRefresh()
{
    if (m_initialRefreshRequested)
    {
        return;
    }

    m_initialRefreshRequested = true;
    refreshAsync();
}

void KernelIoctlAuditTab::initializeUi()
{
    auto* rootLayout = new QVBoxLayout(this);
    rootLayout->setContentsMargins(6, 6, 6, 6);
    rootLayout->setSpacing(5);

    // 查询头与结果页签组成一个连续区域，外围留白仍只由根布局提供。
    auto* resultGroup = new QWidget(this);
    auto* resultLayout = new QVBoxLayout(resultGroup);
    resultLayout->setContentsMargins(0, 0, 0, 0);
    resultLayout->setSpacing(0);
    rootLayout->addWidget(resultGroup, 1);
    auto* queryHeader = new QWidget(resultGroup);
    auto* toolbar = new QHBoxLayout(queryHeader);
    toolbar->setContentsMargins(8, 4, 8, 4);
    m_refreshButton = new QPushButton(kernelText("kernel.ioctl_audit.refresh", QStringLiteral("刷新派遣表")), this);
    m_refreshButton->setStyleSheet(buttonStyle());
    m_filterEdit = new QLineEdit(this);
    ks::ui::StyleSearchField(m_filterEdit);
    m_filterEdit->setClearButtonEnabled(true);
    m_filterEdit->setPlaceholderText(kernelText(
        "kernel.ioctl_audit.filter.placeholder",
        QStringLiteral("筛选驱动、设备、MajorFunction 或地址...")));
    m_filterEdit->setMinimumWidth(280);
    m_clearFilterButton = new QPushButton(
        kernelText("kernel.ioctl_audit.filter.clear", QStringLiteral("清除筛选")),
        this);
    m_clearFilterButton->setStyleSheet(buttonStyle());
    m_statusLabel = new QLabel(kernelText("kernel.ioctl_audit.loading", QStringLiteral("正在加载全局 DriverObject 与 KswordARK IOCTL registry...")), this);
    m_statusLabel->setStyleSheet(QStringLiteral("color:%1;font-weight:600;").arg(KswordTheme::TextSecondaryHex()));
    toolbar->addWidget(m_refreshButton);
    toolbar->addWidget(m_filterEdit);
    toolbar->addWidget(m_clearFilterButton);
    toolbar->addWidget(m_statusLabel, 1);
    ks::ui::NormalizeToolbarRow(toolbar);
    resultLayout->addWidget(queryHeader);

    m_innerTabs = new QTabWidget(this);
    ks::ui::StyleJoinedResultTabs(m_innerTabs, queryHeader);
    m_driverPage = new QWidget(m_innerTabs);
    m_devicePage = new QWidget(m_innerTabs);
    m_dispatchPage = new QWidget(m_innerTabs);
    m_registryPage = new QWidget(m_innerTabs);
    m_driverTable = new ks::ui::TableActionTableView(m_driverPage);
    // 全局驱动对象清单需要留存新增和消失对象。
    ks::ui::SetTableActionBarMode(m_driverTable, ks::ui::TableActionBarMode::Full);
    m_deviceTable = new ks::ui::TableActionTableView(m_devicePage);
    // 全局设备链是现场枚举，保留完整快照比较。
    ks::ui::SetTableActionBarMode(m_deviceTable, ks::ui::TableActionBarMode::Full);
    m_dispatchTable = new ks::ui::TableActionTableView(m_dispatchPage);
    // MajorFunction 地址变化是核心排查目标。
    ks::ui::SetTableActionBarMode(m_dispatchTable, ks::ui::TableActionBarMode::Full);
    m_registryTable = new ks::ui::TableActionTableView(m_registryPage);
    // 自身 IOCTL 注册清单是静态协议映射，不需要快照对比。
    ks::ui::SetTableActionBarMode(m_registryTable, ks::ui::TableActionBarMode::None);
    const std::array<QTableView*, 4> tables{m_driverTable, m_deviceTable, m_dispatchTable, m_registryTable};
    for (std::size_t index = 0; index < tables.size(); ++index)
    {
        m_models[index] = new AuditModel(static_cast<int>(index), tables[index]);
        m_filters[index] = new AuditFilter(m_models[index], tables[index]);
        tables[index]->setModel(m_filters[index]);
        ks::ui::InstallTableHeaderClickSorting(tables[index]);
        ks::ui::SetTableRowFilterBusinessOwnership(tables[index], ks::ui::TableRowFilterBusinessOwnership::ExclusiveSharedIdentity);
        tables[index]->horizontalHeader()->setResizeContentsPrecision(100);
    }
    for (QTableView* table : {m_driverTable, m_deviceTable, m_dispatchTable, m_registryTable})
    {
        table->setSelectionBehavior(QAbstractItemView::SelectRows);
        table->setSelectionMode(QAbstractItemView::ExtendedSelection);
        table->setEditTriggers(QAbstractItemView::NoEditTriggers);
        table->setAlternatingRowColors(true);
        table->setContextMenuPolicy(Qt::CustomContextMenu);
        table->setStyleSheet(QStringLiteral("QTableView{background:transparent;color:%1;}" ).arg(KswordTheme::TextPrimaryHex()));
        table->horizontalHeader()->setStyleSheet(headerStyle());
        table->horizontalHeader()->setStretchLastSection(true);
        table->verticalHeader()->setVisible(false);
    }
    m_models[0]->setHeaders({
        kernelText("kernel.ioctl_audit.header.driver", QStringLiteral("DriverObject")),
        kernelText("kernel.ioctl_audit.header.object_address", QStringLiteral("对象地址")),
        QStringLiteral("DriverStart"),
        kernelText("kernel.ioctl_audit.header.image_size", QStringLiteral("镜像大小")),
        kernelText("kernel.ioctl_audit.header.flags", QStringLiteral("Flags")),
        kernelText("kernel.ioctl_audit.header.devices", QStringLiteral("设备数")),
        kernelText("kernel.ioctl_audit.header.major_count", QStringLiteral("MajorFunction 数")),
        kernelText("kernel.ioctl_audit.header.query_status", QStringLiteral("查询状态")),
        QStringLiteral("NTSTATUS")});
    m_models[1]->setHeaders({
        kernelText("kernel.ioctl_audit.header.driver", QStringLiteral("DriverObject")),
        kernelText("kernel.ioctl_audit.header.relation", QStringLiteral("关系")),
        QStringLiteral("DeviceObject"),
        kernelText("kernel.ioctl_audit.header.device_name", QStringLiteral("设备名称")),
        kernelText("kernel.ioctl_audit.header.device_type", QStringLiteral("设备类型")),
        kernelText("kernel.ioctl_audit.header.flags", QStringLiteral("Flags")),
        QStringLiteral("Characteristics"),
        QStringLiteral("StackSize"),
        QStringLiteral("RootDevice"),
        QStringLiteral("NextDevice"),
        QStringLiteral("AttachedDevice"),
        kernelText("kernel.ioctl_audit.header.owner_driver", QStringLiteral("归属 DriverObject")),
        QStringLiteral("NameStatus")});
    m_models[2]->setHeaders({
        kernelText("kernel.ioctl_audit.header.driver", QStringLiteral("DriverObject")),
        kernelText("kernel.ioctl_audit.header.object_address", QStringLiteral("对象地址")),
        kernelText("kernel.ioctl_audit.header.major", QStringLiteral("MajorFunction")),
        kernelText("kernel.ioctl_audit.header.index", QStringLiteral("编号")),
        kernelText("kernel.ioctl_audit.header.dispatch", QStringLiteral("派遣地址")),
        kernelText("kernel.ioctl_audit.header.module_base", QStringLiteral("模块基址")),
        kernelText("kernel.ioctl_audit.header.module", QStringLiteral("归属模块")),
        kernelText("kernel.ioctl_audit.header.flags", QStringLiteral("Flags")),
        kernelText("kernel.ioctl_audit.header.status", QStringLiteral("状态"))});
    m_models[3]->setHeaders({
        kernelText("kernel.ioctl_audit.header.code", QStringLiteral("控制码")),
        kernelText("kernel.ioctl_audit.header.function", QStringLiteral("Function")),
        kernelText("kernel.ioctl_audit.header.method_access", QStringLiteral("Method/Access")),
        kernelText("kernel.ioctl_audit.header.capability", QStringLiteral("能力门槛")),
        kernelText("kernel.ioctl_audit.header.handler", QStringLiteral("Handler")),
        kernelText("kernel.ioctl_audit.header.name", QStringLiteral("名称")),
        kernelText("kernel.ioctl_audit.header.flags", QStringLiteral("Flags"))});

    auto* driverLayout = new QVBoxLayout(m_driverPage);
    driverLayout->setContentsMargins(2, 2, 2, 2);
    driverLayout->addWidget(m_driverTable);
    auto* deviceLayout = new QVBoxLayout(m_devicePage);
    deviceLayout->setContentsMargins(2, 2, 2, 2);
    deviceLayout->addWidget(m_deviceTable);
    auto* dispatchLayout = new QVBoxLayout(m_dispatchPage);
    dispatchLayout->setContentsMargins(2, 2, 2, 2);
    dispatchLayout->addWidget(m_dispatchTable);
    auto* registryLayout = new QVBoxLayout(m_registryPage);
    registryLayout->setContentsMargins(2, 2, 2, 2);
    registryLayout->addWidget(m_registryTable);
    m_innerTabs->addTab(m_driverPage, kernelText("kernel.ioctl_audit.tab.drivers", QStringLiteral("驱动概览")));
    m_innerTabs->addTab(m_devicePage, kernelText("kernel.ioctl_audit.tab.devices", QStringLiteral("设备对象")));
    m_innerTabs->addTab(m_dispatchPage, kernelText("kernel.ioctl_audit.tab.dispatch", QStringLiteral("MajorFunction")));
    m_innerTabs->addTab(m_registryPage, kernelText("kernel.ioctl_audit.tab.registry", QStringLiteral("KswordARK IOCTL 注册表")));
    resultLayout->addWidget(m_innerTabs, 1);

    connect(m_refreshButton, &QPushButton::clicked, this, [this]() { refreshAsync(); });
    m_filterTimer = new QTimer(this);
    m_filterTimer->setSingleShot(true);
    m_filterTimer->setInterval(120);
    connect(m_filterTimer, &QTimer::timeout, this, [this]() { applyFilter(); });
    connect(m_filterEdit, &QLineEdit::textChanged, this, [this]() { m_filterTimer->start(); });
    connect(m_innerTabs, &QTabWidget::currentChanged, this, [this]() { applyFilter(); });
    connect(m_clearFilterButton, &QPushButton::clicked, m_filterEdit, &QLineEdit::clear);
    connect(m_driverTable, &QTableView::customContextMenuRequested, this, [this](const QPoint& position) {
        showCopyMenu(m_driverTable, position);
    });
    connect(m_deviceTable, &QTableView::customContextMenuRequested, this, [this](const QPoint& position) {
        showCopyMenu(m_deviceTable, position);
    });
    connect(m_dispatchTable, &QTableView::customContextMenuRequested, this, [this](const QPoint& position) {
        showCopyMenu(m_dispatchTable, position);
    });
    connect(m_registryTable, &QTableView::customContextMenuRequested, this, [this](const QPoint& position) {
        showCopyMenu(m_registryTable, position);
    });
}

void KernelIoctlAuditTab::refreshAsync()
{
    if (m_refreshRunning)
    {
        return;
    }
    m_refreshRunning = true;
    m_refreshButton->setEnabled(false);
    m_statusLabel->setText(kernelText("kernel.ioctl_audit.refreshing", QStringLiteral("正在枚举 DriverObject 派遣入口并读取 IOCTL registry...")));
    QPointer<KernelIoctlAuditTab> safeThis(this);
    std::thread([safeThis]() {
        Snapshot snapshot;
        std::vector<KernelDeviceDriverObjectEntry> objectRows;
        QString workerError;
        const bool workerOk = runKernelDeviceDriverObjectsSnapshotTask(objectRows, workerError);
        if (!workerOk)
        {
            snapshot.errorText = workerError;
        }

        ksword::ark::DriverClient client;
        if (workerOk)
        {
            QSet<QString> seenDriverNames;
            for (const KernelDeviceDriverObjectEntry& objectRow : objectRows)
            {
                if (objectRow.isScopeEntry || objectRow.objectTypeText.compare(QStringLiteral("Driver"), Qt::CaseInsensitive) != 0)
                {
                    continue;
                }
                const QString driverPath = objectRow.fullPathText.trimmed();
                const QString normalizedDriverPath = driverPath.toCaseFolded();
                if (driverPath.isEmpty() || seenDriverNames.contains(normalizedDriverPath))
                {
                    continue;
                }
                seenDriverNames.insert(normalizedDriverPath);

                DriverRow driverRow;
                driverRow.driverName = driverPath;
                const std::wstring driverName = driverPath.toStdWString();
                const ksword::ark::DriverObjectQueryResult query = client.queryDriverObject(driverName);
                if (!query.io.ok)
                {
                    ++snapshot.queryFailureCount;
                    driverRow.lastStatus = static_cast<std::int32_t>(query.lastStatus);
                    driverRow.status = query.io.message.empty()
                        ? QStringLiteral("DriverObject 查询失败")
                        : QString::fromStdString(query.io.message);
                    snapshot.driverRows.push_back(std::move(driverRow));
                    continue;
                }

                driverRow.driverObjectAddress = query.driverObjectAddress;
                driverRow.driverStart = query.driverStart;
                driverRow.driverSize = query.driverSize;
                driverRow.driverFlags = query.driverFlags;
                driverRow.majorFunctionCount = query.majorFunctionCount;
                driverRow.returnedDeviceCount = query.returnedDeviceCount;
                driverRow.totalDeviceCount = query.totalDeviceCount;
                driverRow.queryStatus = query.queryStatus;
                driverRow.lastStatus = static_cast<std::int32_t>(query.lastStatus);
                if (query.queryStatus == KSWORD_ARK_DRIVER_OBJECT_QUERY_STATUS_PARTIAL)
                {
                    ++snapshot.partialDriverCount;
                }
                snapshot.driverRows.push_back(std::move(driverRow));

                for (const ksword::ark::DriverDeviceEntry& device : query.devices)
                {
                    DeviceRow row;
                    row.driverName = driverPath;
                    row.deviceName = QString::fromStdWString(device.deviceName);
                    row.relationDepth = device.relationDepth;
                    row.deviceType = device.deviceType;
                    row.flags = device.flags;
                    row.characteristics = device.characteristics;
                    row.stackSize = device.stackSize;
                    row.nameStatus = static_cast<std::int32_t>(device.nameStatus);
                    row.rootDeviceObjectAddress = device.rootDeviceObjectAddress;
                    row.deviceObjectAddress = device.deviceObjectAddress;
                    row.nextDeviceObjectAddress = device.nextDeviceObjectAddress;
                    row.attachedDeviceObjectAddress = device.attachedDeviceObjectAddress;
                    row.ownerDriverObjectAddress = device.driverObjectAddress;
                    snapshot.deviceRows.push_back(std::move(row));
                }
                for (const ksword::ark::DriverMajorFunctionEntry& major : query.majorFunctions)
                {
                    DispatchRow row;
                    row.driverName = driverPath;
                    row.driverObjectAddress = query.driverObjectAddress;
                    row.majorFunction = major.majorFunction;
                    row.dispatchAddress = major.dispatchAddress;
                    row.moduleBase = major.moduleBase;
                    row.moduleName = QString::fromStdWString(major.moduleName);
                    row.flags = major.flags;
                    snapshot.dispatchRows.push_back(std::move(row));
                }
                if (query.startIo.state != KSWORD_ARK_DRIVER_START_IO_STATE_NOT_QUERIED)
                {
                    DispatchRow row;
                    row.driverName = driverPath;
                    row.driverObjectAddress = query.driverObjectAddress;
                    row.dispatchAddress = query.startIo.address;
                    row.moduleBase = query.startIo.moduleBase;
                    row.moduleName = QString::fromStdWString(query.startIo.moduleName);
                    row.flags = query.startIo.flags;
                    row.isStartIo = true;
                    row.startIoState = query.startIo.state;
                    snapshot.dispatchRows.push_back(std::move(row));
                }
            }
        }

        const ksword::ark::IoctlRegistryQueryResult registry = client.queryIoctlRegistry();
        snapshot.registryOk = registry.io.ok;
        snapshot.registryTotal = registry.totalCount;
        snapshot.registryDuplicate = registry.duplicateCount;
        snapshot.registryRows = registry.entries;
        if (!registry.io.ok && snapshot.errorText.isEmpty())
        {
            snapshot.errorText = registry.io.message.empty()
                ? QStringLiteral("KswordARK IOCTL registry 查询失败")
                : QString::fromStdString(registry.io.message);
        }

        if (safeThis == nullptr)
        {
            return;
        }
        QMetaObject::invokeMethod(safeThis, [safeThis, snapshot = std::move(snapshot)]() mutable {
            if (safeThis != nullptr)
            {
                safeThis->applySnapshot(std::move(snapshot));
            }
        }, Qt::QueuedConnection);
    }).detach();
}

void KernelIoctlAuditTab::applySnapshot(Snapshot snapshot)
{
    const QPointer<KernelIoctlAuditTab> safeThis(this);
    if (ks::ui::DeferTableUiCommitIfContextMenuOpen(
        this,
        QStringLiteral("kernel-ioctl-audit-snapshot"),
        { m_driverTable, m_deviceTable, m_dispatchTable, m_registryTable },
        [safeThis, snapshot]() mutable
        {
            if (!safeThis.isNull())
            {
                safeThis->applySnapshot(std::move(snapshot));
            }
        }))
    {
        return;
    }

    m_refreshRunning = false;
    m_refreshButton->setEnabled(true);
    m_queryFailureCount = snapshot.queryFailureCount;
    m_partialDriverCount = snapshot.partialDriverCount;
    m_registryTotal = snapshot.registryTotal;
    m_registryDuplicate = snapshot.registryDuplicate;
    m_registryOk = snapshot.registryOk;
    m_errorText = snapshot.errorText;
    m_snapshot = std::make_shared<const Snapshot>(std::move(snapshot));
    populateTables();
}

QString KernelIoctlAuditTab::hex64(const std::uint64_t value)
{
    return QStringLiteral("0x%1").arg(value, 16, 16, QLatin1Char('0'));
}

QString KernelIoctlAuditTab::hex32(const std::uint32_t value)
{
    return QStringLiteral("0x%1").arg(value, 8, 16, QLatin1Char('0'));
}

QString KernelIoctlAuditTab::majorFunctionName(const std::uint32_t value)
{
    switch (value)
    {
    case 0x00: return QStringLiteral("IRP_MJ_CREATE");
    case 0x01: return QStringLiteral("IRP_MJ_CREATE_NAMED_PIPE");
    case 0x02: return QStringLiteral("IRP_MJ_CLOSE");
    case 0x03: return QStringLiteral("IRP_MJ_READ");
    case 0x04: return QStringLiteral("IRP_MJ_WRITE");
    case 0x05: return QStringLiteral("IRP_MJ_QUERY_INFORMATION");
    case 0x06: return QStringLiteral("IRP_MJ_SET_INFORMATION");
    case 0x07: return QStringLiteral("IRP_MJ_QUERY_EA");
    case 0x08: return QStringLiteral("IRP_MJ_SET_EA");
    case 0x09: return QStringLiteral("IRP_MJ_FLUSH_BUFFERS");
    case 0x0A: return QStringLiteral("IRP_MJ_QUERY_VOLUME_INFORMATION");
    case 0x0B: return QStringLiteral("IRP_MJ_SET_VOLUME_INFORMATION");
    case 0x0C: return QStringLiteral("IRP_MJ_DIRECTORY_CONTROL");
    case 0x0D: return QStringLiteral("IRP_MJ_FILE_SYSTEM_CONTROL");
    case 0x0E: return QStringLiteral("IRP_MJ_DEVICE_CONTROL");
    case 0x0F: return QStringLiteral("IRP_MJ_INTERNAL_DEVICE_CONTROL");
    case 0x10: return QStringLiteral("IRP_MJ_SHUTDOWN");
    case 0x11: return QStringLiteral("IRP_MJ_LOCK_CONTROL");
    case 0x12: return QStringLiteral("IRP_MJ_CLEANUP");
    case 0x13: return QStringLiteral("IRP_MJ_CREATE_MAILSLOT");
    case 0x14: return QStringLiteral("IRP_MJ_QUERY_SECURITY");
    case 0x15: return QStringLiteral("IRP_MJ_SET_SECURITY");
    case 0x16: return QStringLiteral("IRP_MJ_POWER");
    case 0x17: return QStringLiteral("IRP_MJ_SYSTEM_CONTROL");
    case 0x18: return QStringLiteral("IRP_MJ_DEVICE_CHANGE");
    case 0x19: return QStringLiteral("IRP_MJ_QUERY_QUOTA");
    case 0x1A: return QStringLiteral("IRP_MJ_SET_QUOTA");
    case 0x1B: return QStringLiteral("IRP_MJ_PNP");
    default: return QStringLiteral("IRP_MJ_%1").arg(value);
    }
}

void KernelIoctlAuditTab::populateTables()
{
    for (auto* model : m_models) model->setSnapshot(m_snapshot);
    applyFilter();
    const QString summary = kernelText(
        "kernel.ioctl_audit.summary.detailed",
        QStringLiteral("驱动 %1 个（失败 %2，部分 %3），设备 %4 行，MajorFunction %5 行，KswordARK registry %6/%7 行，重复控制码 %8。"))
        .arg(static_cast<qulonglong>(m_snapshot->driverRows.size()))
        .arg(m_queryFailureCount)
        .arg(m_partialDriverCount)
        .arg(static_cast<qulonglong>(m_snapshot->deviceRows.size()))
        // %5 标注的是 "MajorFunction 行"，m_snapshot->dispatchRows 里还混着每个驱动一条的
        // DriverStartIo 行，直接用 size() 会把计数抬高，对不上 28×驱动数。
        .arg(static_cast<qulonglong>(std::count_if(
            m_snapshot->dispatchRows.begin(),
            m_snapshot->dispatchRows.end(),
            [](const DispatchRow& row) { return !row.isStartIo; })))
        .arg(static_cast<qulonglong>(m_snapshot->registryRows.size()))
        .arg(m_registryTotal)
        .arg(m_registryDuplicate);
    m_statusLabel->setText(m_errorText.isEmpty() ? summary : summary + QStringLiteral(" ") + m_errorText);
}

void KernelIoctlAuditTab::changeEvent(QEvent* event)
{
    QWidget::changeEvent(event);
    if (event->type() == QEvent::LanguageChange)
    {
        for (auto* model : m_models) if (model != nullptr) model->clearFormattedRows();
    }
}

void KernelIoctlAuditTab::showEvent(QShowEvent* event)
{
    QWidget::showEvent(event);
    applyFilter();
}

void KernelIoctlAuditTab::applyFilter()
{
    const QPointer<KernelIoctlAuditTab> guard(this);
    if (ks::ui::DeferTableUiCommitIfContextMenuOpen(this, QStringLiteral("kernel-ioctl-audit-filter"),
        {m_driverTable, m_deviceTable, m_dispatchTable, m_registryTable},
        [guard]() { if (guard) guard->applyFilter(); })) return;
    const QString filterText = m_filterEdit == nullptr ? QString() : m_filterEdit->text().trimmed();
    if (m_clearFilterButton != nullptr) m_clearFilterButton->setEnabled(!filterText.isEmpty());
    if (!isVisible() || m_innerTabs == nullptr) return;
    const int current = m_innerTabs->currentIndex();
    if (current < 0 || current >= static_cast<int>(m_filters.size())) return;
    m_filters[current]->setNeedle(filterText);
    const std::array<QTableView*, 4> tables{m_driverTable, m_deviceTable, m_dispatchTable, m_registryTable};
    if (!m_columnsSized[current] && m_models[current]->rowCount() > 0)
    {
        m_columnsSized[current] = true;
        tables[current]->resizeColumnsToContents();
    }
}

QString KernelIoctlAuditTab::tableRowText(QTableView* table, const int row, const bool includeHeader, const bool sourceRows)
{
    if (table == nullptr) return {};
    auto* proxy = static_cast<AuditFilter*>(table->model());
    QAbstractItemModel* model = sourceRows ? proxy->sourceModel() : table->model();
    if (row < 0 || row >= model->rowCount())
    {
        return {};
    }
    QStringList values;
    if (includeHeader)
    {
        for (int column = 0; column < model->columnCount(); ++column)
        {
            values << model->headerData(column, Qt::Horizontal).toString();
        }
    }
    QStringList rowValues;
    for (int column = 0; column < model->columnCount(); ++column)
    {
        rowValues << model->data(model->index(row, column)).toString();
    }
    values << rowValues.join(QLatin1Char('\t'));
    return values.join(QLatin1Char('\n'));
}

void KernelIoctlAuditTab::showCopyMenu(QTableView* table, const QPoint& position)
{
    if (table == nullptr)
    {
        return;
    }
    const QModelIndex index = table->indexAt(position);
    const QPersistentModelIndex clickedIndex(index);
    const QPointer<QTableView> tableGuard(table);
    const QPointer<KernelIoctlAuditTab> ownerGuard(this);
    QMenu menu(this);
    QAction* copyRow = menu.addAction(kernelText("kernel.ioctl_audit.copy_row", QStringLiteral("复制当前行")));
    // 声明业务菜单已提供此能力，避免共享表格菜单追加重复项。
    ks::ui::SetTableContextActionRole(copyRow, ks::ui::TableContextActionRole::CopyRows);
    QAction* copyAll = menu.addAction(kernelText("kernel.ioctl_audit.copy_all", QStringLiteral("复制全部行")));
    copyRow->setEnabled(clickedIndex.isValid());
    auto* source = static_cast<AuditFilter*>(table->model())->sourceModel();
    copyAll->setEnabled(source->rowCount() > 0);
    const QPointer<QAbstractItemModel> sourceGuard(source);
    QAction* selected = menu.exec(table->viewport()->mapToGlobal(position));
    if (ownerGuard.isNull() || tableGuard.isNull() || sourceGuard.isNull()
        || static_cast<AuditFilter*>(table->model())->sourceModel() != sourceGuard) return;
    if (selected == copyRow && clickedIndex.isValid() && clickedIndex.model() == table->model())
    {
        QApplication::clipboard()->setText(tableRowText(table, clickedIndex.row(), true));
    }
    else if (selected == copyAll)
    {
        QStringList lines;
        for (int rowIndex = 0; rowIndex < source->rowCount(); ++rowIndex)
        {
            lines << tableRowText(table, rowIndex, rowIndex == 0, true);
        }
        QApplication::clipboard()->setText(lines.join(QLatin1Char('\n')));
    }
}
