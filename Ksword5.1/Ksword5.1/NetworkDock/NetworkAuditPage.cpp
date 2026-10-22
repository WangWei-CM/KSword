#include "NetworkDock.NavigationData.h"
#include "NetworkAuditPage.h"
#include "../UI/TableContextMenu.h"
#include "../UI/FlatButtonTheme.h"
#include "../UI/TableInteractionSupport.h"
#include "../UI/VisibleTableWidget.h"
#include "../UI/FlatTableModel.h"
#include "NetworkValueTableModel.h"
#include "../UI/AsyncOperation.h"
#include "../UI/TableRowFilterSupport.h"
#include <QTableView>
#include "../UI/ThemeBinding.h"
#include "../UI/TablePresentation.h"
#include "../UI/ToolbarMetrics.h"
#include "../UI/PageControlStyle.h"
#include "../UI/PrimaryPageStyle.h"
#include "../Internationalization/LanguageManager.h"

// ============================================================
// NetworkAuditPage.cpp
// 作用：
// 1) 提供网络审计页的 UI 与快照刷新逻辑；
// 2) TCP/UDP Cross-View 合并连接刷新、PID 筛选、复制与 TCP 终止动作；
// 3) AFD/WFP/NDIS/NSI 保持只读审计边界。
// ============================================================

#include "../theme.h"
#include "../ArkDriverClient/ArkDriverClient.h"
#include "../Framework/PrivilegeElevationPrompt.h"
#include "../ksword/file/file_handle_tools.h"
#include "../ksword/network/network.h"
#include "../ksword/network/network_connection_tools.h"
#include "../ksword/process/process.h"
#include "../ksword/log/log.h"
#include "../OnlineScan/SandboxUploadActions.h"

#include <QDateTime>
#include <QAbstractItemView>
#include <QAction>
#include <QApplication>
#include <QClipboard>
#include <QJsonParseError>
#include <QGridLayout>
#include <QHeaderView>
#include <QHBoxLayout>
#include <QIcon>
#include <QImage>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QMessageBox>
#include <QMetaObject>
#include <QModelIndex>
#include <QPixmap>
#include <QProcess>
#include <QPushButton>
#include <QSizePolicy>
#include <QShowEvent>
#include <QEvent>
#include <QSortFilterProxyModel>
#include <QScopeGuard>
#include <QItemSelectionModel>
#include <QSplitter>
#include <QStringList>
#include <QTabWidget>
#include <QTableWidget>
#include <QTableWidgetItem>
#include <QThread>
#include <QThreadPool>
#include <QTimer>
#include <QVBoxLayout>

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <exception>
#include <memory>
#include <mutex>
#include <set>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <utility>

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#include <objbase.h>
#include <Rpc.h>
#include <Shellapi.h>
#include <fwpmu.h>

#pragma comment(lib, "Ws2_32.lib")

// NetworkAuditAsyncState 把 detached worker 的回投目标放在共享互斥状态中。
// 析构先清空 owner；worker 只有在同一把锁保护下才能提交 queued 调用，
// GUI 回调再复核 owner，并在调用页面成员前释放锁。
struct NetworkAuditAsyncState
{
    std::mutex mutex;
    NetworkAuditPage* owner = nullptr;
    std::atomic_bool closing{false};
};

namespace
{
    // kAuditProcessIdColumn / kAuditProcessNameColumn：
    // - TCP、UDP 与 Cross-View 三张表都把 PID 放在第 0 列、进程名放在第 1 列；
    // - 图标异步回补时按这两个列号定位单元格。
    constexpr int kAuditProcessIdColumn = 0;
    constexpr int kAuditProcessNameColumn = 1;

    // auditProcessPlaceholderIcon 作用：
    // - 返回审计页统一的进程占位图标，图标未解析完成或解析失败时顶上；
    // - 入参：无；
    // - 返回：共享 QIcon 引用，只能在 UI 线程使用。
    const QIcon& auditProcessPlaceholderIcon()
    {
        static const QIcon placeholderIcon(QStringLiteral(":/Icon/process_main.svg"));
        return placeholderIcon;
    }

    // extractProcessIconImageForPid 作用：
    // - 在线程池工作线程里按 PID 解析可执行路径并向 Shell 查询小图标；
    // - SHGetFileInfoW 依赖 COM，工作线程必须自己成对 CoInitializeEx / CoUninitialize；
    // - 入参 processId：目标进程 PID；
    // - 返回：可跨线程传递的 QImage，失败时返回空 QImage（调用方回退占位图标）。
    QImage extractProcessIconImageForPid(const quint32 processId)
    {
        const std::string processPath = ks::process::QueryProcessPathByPid(processId);
        if (processPath.empty())
        {
            return QImage();
        }

        const HRESULT comInitializeResult = ::CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
        const bool comInitializedHere = SUCCEEDED(comInitializeResult);

        const QString processPathText = QString::fromUtf8(processPath.c_str());
        SHFILEINFOW shellFileInfo{};
        const DWORD_PTR shellQueryResult = ::SHGetFileInfoW(
            reinterpret_cast<const wchar_t*>(processPathText.utf16()),
            0,
            &shellFileInfo,
            sizeof(shellFileInfo),
            SHGFI_ICON | SHGFI_SMALLICON);

        QImage processIconImage;
        if (shellQueryResult != 0 && shellFileInfo.hIcon != nullptr)
        {
            // QImage::fromHICON 会复制像素数据，转换完成后必须归还 Shell 分配的 HICON。
            processIconImage = QImage::fromHICON(shellFileInfo.hIcon);
            ::DestroyIcon(shellFileInfo.hIcon);
        }

        if (comInitializedHere)
        {
            ::CoUninitialize();
        }
        return processIconImage;
    }

    std::string crossViewIdentity(const char kind, const QStringList& fields)
    {
        std::string key(1, kind);
        for (const auto& field : fields)
        {
            const auto bytes = field.toUtf8();
            key += std::to_string(bytes.size());
            key.push_back(':');
            key.append(bytes.constData(), static_cast<std::size_t>(bytes.size()));
        }
        return key;
    }

    template<class Model>
    typename Model::RowChangeResolver crossViewValueChanges(bool tcp)
    {
        return [tcp](const typename Model::RowType& left, const typename Model::RowType& right) {
            const auto& a = *left;
            const auto& b = *right;
            std::vector<typename Model::ColumnChange> changes;
            for (int column = 0; column < b.fields.size(); ++column)
                if (a.fields.value(column) != b.fields.at(column))
                    changes.push_back({column, column, {Qt::DisplayRole}});
            if (a.searchFields != b.searchFields)
                changes.push_back({0, 0, {Qt::UserRole + 170}});
            if (a.processId != b.processId)
            {
                changes.push_back({0, 0, {Qt::UserRole + 171}});
                changes.push_back({kAuditProcessNameColumn, kAuditProcessNameColumn, {Qt::DecorationRole}});
            }
            if (tcp && a.sourceIndex != b.sourceIndex)
                changes.push_back({0, 0, {Qt::UserRole}});
            return changes;
        };
    }

    // createReadOnlyCell 作用：
    // - 创建不可编辑的表格单元格；
    // - 输入 cellText 为待显示文本；
    // - 返回可直接写入表格的 item 指针。
    QTableWidgetItem* createReadOnlyCell(const QString& cellText)
    {
        QTableWidgetItem* item = new QTableWidgetItem(cellText);
        item->setFlags(item->flags() & ~Qt::ItemIsEditable);
        return item;
    }

    // tableMenuStyle 作用：
    // - 为网络审计页新增右键菜单提供不透明背景；
    // - 输入：无；
    // - 返回：QMenu 样式表，避免浅色模式黑底黑字。
    QString tableMenuStyle()
    {
        // 网络审计页菜单：
        // - 输入：无；
        // - 处理：统一复用全局不透明菜单样式，避免 palette role 在 Dock 透明背景下被错误继承；
        // - 返回：可直接应用到 QMenu 的样式文本。
        return KswordTheme::ContextMenuStyle();
    }

    // copyCurrentTableRow 作用：
    // - 把当前 QTableWidget 行复制为 TSV；
    // - 输入 table：目标表格；
    // - 返回：无，剪贴板不可用或未选中时直接返回。
    void copyCurrentTableRow(QTableView* table)
    {
        if (table == nullptr || QApplication::clipboard() == nullptr)
        {
            return;
        }

        const int rowIndex = table->currentIndex().row();
        if (rowIndex < 0 || rowIndex >= table->model()->rowCount())
        {
            return;
        }

        QStringList fields;
        fields.reserve(table->model()->columnCount());
        for (int columnIndex = 0; columnIndex < table->model()->columnCount(); ++columnIndex)
        {
            fields.push_back(table->model()->index(rowIndex, columnIndex).data().toString());
        }
        QApplication::clipboard()->setText(fields.join('\t'));
    }

    // installCopyMenu 作用：
    // - 给只读审计表格安装“复制当前行”菜单；
    // - processIdColumn 为显式指定的 PID 列，-1 表示该表没有 PID；
    // - 返回：无，菜单只读，不改变网络栈状态。
    void installCopyMenu(QTableView* table, const int processIdColumn = -1, const bool crossFilter = false)
    {
        if (table == nullptr)
        {
            return;
        }

        table->setContextMenuPolicy(Qt::CustomContextMenu);
        QObject::connect(table, &QWidget::customContextMenuRequested, table, [table, processIdColumn, crossFilter](const QPoint& localPosition)
        {
            const QModelIndex clickedIndex = table->indexAt(localPosition);
            if (crossFilter && clickedIndex.isValid()
                && ks::ui::IsTableRowExcludedByFilter(table, clickedIndex.row())) return;
            if (clickedIndex.isValid())
            {
                table->setCurrentIndex(clickedIndex);
                table->selectRow(clickedIndex.row());
            }

            QMenu menu(table);
            menu.setStyleSheet(tableMenuStyle());
            QAction* copyRowAction = menu.addAction(QIcon(QStringLiteral(":/Icon/process_copy_row.svg")), QStringLiteral("复制当前行"));
            // 声明业务菜单已提供此能力，避免共享表格菜单追加重复项。
            ks::ui::SetTableContextActionRole(copyRowAction, ks::ui::TableContextActionRole::CopyRows);
            copyRowAction->setEnabled(table->currentIndex().row() >= 0
                && (!crossFilter || !ks::ui::IsTableRowExcludedByFilter(table, table->currentIndex().row())));
            quint32 processId = 0;
            if (processIdColumn >= 0 && processIdColumn < table->model()->columnCount() &&
                table->currentIndex().row() >= 0 && table->currentIndex().row() < table->model()->rowCount()
                && (!crossFilter || !ks::ui::IsTableRowExcludedByFilter(table, table->currentIndex().row())))
            {
                const auto processIdText = table->model()->index(table->currentIndex().row(), processIdColumn).data().toString();
                bool parseOk = false;
                const uint parsedProcessId = processIdText.toUInt(&parseOk, 10);
                if (parseOk && parsedProcessId != 0U)
                {
                    processId = static_cast<quint32>(parsedProcessId);
                }
            }
            QAction* openProcessDetailAction = nullptr;
            if (processIdColumn >= 0)
            {
                openProcessDetailAction = menu.addAction(
                    QIcon(QStringLiteral(":/Icon/process_details.svg")),
                    QStringLiteral("转到进程详细信息"));
                openProcessDetailAction->setEnabled(processId != 0U);
            }

            const QAction* selectedAction = menu.exec(table->viewport()->mapToGlobal(localPosition));
            if (selectedAction == copyRowAction)
            {
                if (!crossFilter || !ks::ui::IsTableRowExcludedByFilter(table, table->currentIndex().row()))
                    copyCurrentTableRow(table);
            }
            else if (selectedAction == openProcessDetailAction)
            {
                ks::ui::OpenProcessDetailByPid(processId);
            }
        });
    }

    void setAuditValueHeaders(QTableView* table, const QStringList& headers)
    {
        using Model = ks::ui::FlatTableModel<QStringList>;
        std::vector<Model::ColumnSpec> columns;
        for (const auto& header : headers) columns.push_back({header});
        table->setProperty("ksword_audit_value_headers", headers);
        table->setProperty("ks_i18n_preserve_model_data", true);
        table->setModel(new Model(std::move(columns), [](const QStringList&, int, int) -> QVariant { return {}; }, table));
    }

    template<class Snapshot, class Row, class Resolver>
    void presentAuditValueRows(QTableView* table, const std::shared_ptr<Snapshot>& snapshot,
        const std::vector<Row>& rows, Resolver resolve)
    {
        using SharedRow = std::shared_ptr<const Row>;
        using Model = ks::ui::FlatTableModel<SharedRow>;
        auto* model = dynamic_cast<Model*>(table->model());
        if (!model)
        {
            std::vector<typename Model::ColumnSpec> columns;
            for (const auto& header : table->property("ksword_audit_value_headers").toStringList()) columns.push_back({header});
            model = new Model(std::move(columns), [resolve](const SharedRow& value, int column, int role) -> QVariant
            {
                if (role != Qt::DisplayRole && role != Qt::ToolTipRole) return {};
                return resolve(*value, column);
            }, table);
            auto* old = table->model();
            table->setModel(model);
            if (old && old->parent() == table) old->deleteLater();
        }
        std::vector<SharedRow> projected;
        projected.reserve(rows.size());
        // Aliasing pointers keep the accepted immutable snapshot as the only
        // field source; no GUI cell objects or shadow formatted document.
        for (const auto& row : rows) projected.emplace_back(snapshot, &row);
        model->setRows(std::move(projected));
    }

    // joinCompactLines 作用：
    // - 把若干短文本合并成单行摘要；
    // - 适合用于 cross-view 和 summary 页。
    QString joinCompactLines(const QStringList& lines)
    {
        QStringList filteredLines;
        filteredLines.reserve(lines.size());
        for (const QString& line : lines)
        {
            if (!line.trimmed().isEmpty())
            {
                filteredLines.push_back(line.trimmed());
            }
        }
        return filteredLines.join(QStringLiteral(" | "));
    }

    // ioMessageToText 作用：
    // - 把 ArkDriverClient 的 UTF-8 message 转为 Qt 文本；
    // - 输入 messageText：IoResult::message；
    // - 返回：可展示的 QString，空消息会归一化为“无附加信息”。
    QString ioMessageToText(const std::string& messageText)
    {
        if (messageText.empty())
        {
            return QStringLiteral("无额外驱动消息");
        }
        const QString rawText = QString::fromUtf8(messageText.c_str()).trimmed();
        if (rawText.contains(QStringLiteral("DeviceIoControl"), Qt::CaseInsensitive))
        {
            return QStringLiteral("驱动接口调用失败或当前驱动版本不支持该网络审计入口");
        }
        if (rawText.contains(QStringLiteral("unsupported"), Qt::CaseInsensitive) ||
            rawText.contains(QStringLiteral("not supported"), Qt::CaseInsensitive))
        {
            // 旧驱动兼容：
            // - 输入：ArkDriverClient 返回的 unsupported/not supported 文本；
            // - 处理：折叠为网络审计页统一的人读状态；
            // - 返回：不暴露底层 IOCTL 名称，方便用户直接判断需要更新驱动。
            return QStringLiteral("当前驱动不支持网络审计，请更新为匹配版本");
        }
        if (rawText.contains(QStringLiteral("capability"), Qt::CaseInsensitive) ||
            rawText.contains(QStringLiteral("DynData"), Qt::CaseInsensitive) ||
            rawText.contains(QStringLiteral("profile"), Qt::CaseInsensitive))
        {
            // 动态偏移诊断：
            // - 输入：驱动返回的 capability/DynData/profile 相关说明；
            // - 处理：转换成用户能理解的偏移能力缺口；
            // - 返回：指向内核 DynData 页继续排查。
            return QStringLiteral("当前驱动缺少网络审计所需能力，请更新驱动或相关数据文件");
        }
        if (rawText.contains(QStringLiteral("access denied"), Qt::CaseInsensitive) ||
            rawText.contains(QStringLiteral("privilege"), Qt::CaseInsensitive))
        {
            return QStringLiteral("权限不足，无法完成网络审计查询");
        }
        if (rawText.contains(QStringLiteral("invalid parameter"), Qt::CaseInsensitive) ||
            rawText.contains(QStringLiteral("version mismatch"), Qt::CaseInsensitive))
        {
            return QStringLiteral("R0/R3 网络审计协议参数不兼容，请同步 shared 协议与驱动版本");
        }
        if (rawText.contains(QStringLiteral("too small"), Qt::CaseInsensitive) ||
            rawText.contains(QStringLiteral("entrySize"), Qt::CaseInsensitive) ||
            rawText.contains(QStringLiteral("buffer"), Qt::CaseInsensitive))
        {
            return QStringLiteral("驱动返回数据格式不完整，已保留 R3 审计结果");
        }
        if (rawText.contains(QStringLiteral("timeout"), Qt::CaseInsensitive))
        {
            return QStringLiteral("R0 网络审计查询超时，已保留现有 R3 审计结果");
        }
        if (rawText.contains(QStringLiteral("invalid response"), Qt::CaseInsensitive) ||
            rawText.contains(QStringLiteral("invalid endpoint row"), Qt::CaseInsensitive) ||
            rawText.contains(QStringLiteral("invalid WFP row"), Qt::CaseInsensitive) ||
            rawText.contains(QStringLiteral("invalid NDIS row"), Qt::CaseInsensitive) ||
            rawText.contains(QStringLiteral("returned rows exceed"), Qt::CaseInsensitive))
        {
            return QStringLiteral("驱动返回的网络审计响应未通过协议校验，已丢弃 R0 数据");
        }
        if (rawText.contains(QStringLiteral("protocolStatus="), Qt::CaseInsensitive))
        {
            return QStringLiteral("驱动已返回并通过校验的结构化网络审计数据");
        }
        if (rawText.startsWith(QStringLiteral("version="), Qt::CaseInsensitive))
        {
            return QStringLiteral("驱动已返回结构化网络审计数据");
        }
        return rawText;
    }

    // r0AuditStatusText 作用：
    // - 将 R0 wrapper 的 ok/partial/unsupported/unavailable 状态转成 UI 文本；
    // - 输入 result：任意带 io/unsupported 字段的 ArkDriverClient 审计结果；
    // - 返回：验收要求中的 ok / partial / unsupported / unavailable。
    template <typename TResult>
    QString r0AuditStatusText(const TResult& result)
    {
        if (!result.io.ok)
        {
            return result.unsupported
                ? QStringLiteral("unsupported")
                : QStringLiteral("unavailable");
        }

        // DeviceIoControl 成功只代表拿到了结构化响应头；只有 APPLIED 且 total==returned 才是完整快照。
        // ArkDriverClient 已严格筛选 partial，其他 OPERATION_FAILED 不保留明细行。
        if (result.partial ||
            result.truncated ||
            result.totalCount > result.returnedCount)
        {
            return QStringLiteral("partial");
        }
        if (result.status != KSWORD_ARK_NETWORK_STATUS_APPLIED)
        {
            return QStringLiteral("unavailable");
        }
        return result.sourceFlags != KSWORD_ARK_NETWORK_AUDIT_SOURCE_NONE
            ? QStringLiteral("ok")
            : QStringLiteral("unavailable");
    }

    // r0AuditTruncatedText 作用：
    // - 根据 wrapper completeness 与 total/returned 判断 R0 结果是否被截断；
    // - 输入 result：任意 VariableAuditResultBase 派生结果；
    // - 返回：true/false 文本，方便摘要表直接展示。
    template <typename TResult>
    QString r0AuditTruncatedText(const TResult& result)
    {
        return result.truncated || result.totalCount > result.returnedCount
            ? QStringLiteral("true")
            : QStringLiteral("false");
    }

    // r0Hex32 作用：
    // - 输入 value：R0 协议中的 32 位 flags/status/fieldMask；
    // - 处理：统一补零十六进制展示；
    // - 返回：适合表格详情列复制的文本。
    QString r0Hex32(const std::uint32_t value)
    {
        return QStringLiteral("0x%1")
            .arg(static_cast<qulonglong>(value), 8, 16, QChar('0'))
            .toUpper();
    }

    // r0Hex64 作用：
    // - 输入 value：R0 协议中的对象地址、LUID 或 image base；
    // - 处理：统一补零十六进制展示；
    // - 返回：适合表格详情列复制的文本。
    QString r0Hex64(const std::uint64_t value)
    {
        return QStringLiteral("0x%1")
            .arg(static_cast<qulonglong>(value), 16, 16, QChar('0'))
            .toUpper();
    }

    // fixedNetworkWideText 作用：
    // - 输入 buffer/maxChars：shared/driver 固定长度 wchar_t 字符串；
    // - 处理：遇到 NUL 终止，避免把填充区显示到 UI；
    // - 返回：可读文本，空字符串用 fallback 兜底。
    QString fixedNetworkWideText(const wchar_t* buffer, const std::size_t maxChars, const QString& fallback = QStringLiteral("<空>"))
    {
        if (buffer == nullptr || maxChars == 0U)
        {
            return fallback;
        }

        std::size_t length = 0U;
        while (length < maxChars && buffer[length] != L'\0')
        {
            ++length;
        }
        if (length == 0U)
        {
            return fallback;
        }
        return QString::fromWCharArray(buffer, static_cast<int>(length));
    }

    // r0AddressText 作用：
    // - 输入 family/address：R0 endpoint 行中的原始地址族和 16 字节地址；
    // - 处理：IPv4 按点分十进制，IPv6 按 8 组十六进制展示；
    // - 返回：可读 IP 地址，未知地址族保留诊断。
    QString r0AddressText(const unsigned long family, const unsigned char address[16])
    {
        if (address == nullptr)
        {
            return QStringLiteral("<无地址>");
        }
        if (family == KSWORD_ARK_NETWORK_ADDRESS_FAMILY_IPV4)
        {
            return QStringLiteral("%1.%2.%3.%4")
                .arg(static_cast<unsigned int>(address[0]))
                .arg(static_cast<unsigned int>(address[1]))
                .arg(static_cast<unsigned int>(address[2]))
                .arg(static_cast<unsigned int>(address[3]));
        }
        if (family == KSWORD_ARK_NETWORK_ADDRESS_FAMILY_IPV6)
        {
            QStringList groups;
            groups.reserve(8);
            for (int index = 0; index < 16; index += 2)
            {
                const unsigned int groupValue =
                    (static_cast<unsigned int>(address[index]) << 8U) |
                    static_cast<unsigned int>(address[index + 1]);
                groups.push_back(QStringLiteral("%1").arg(groupValue, 4, 16, QChar('0')));
            }
            return groups.join(':').toUpper();
        }
        return QStringLiteral("<AF=%1>").arg(family);
    }

    // r0EndpointText 作用：
    // - 输入 family/address/port：R0 endpoint 的地址和端口；
    // - 处理：把地址和端口合并为单个端点文本；
    // - 返回：IPv6 地址自动加方括号，便于和端口区分。
    QString r0EndpointText(const unsigned long family, const unsigned char address[16], const unsigned short port)
    {
        const QString addressText = r0AddressText(family, address);
        if (family == KSWORD_ARK_NETWORK_ADDRESS_FAMILY_IPV6)
        {
            return QStringLiteral("[%1]:%2").arg(addressText).arg(port);
        }
        return QStringLiteral("%1:%2").arg(addressText).arg(port);
    }

    // r0TcpStateText 作用：
    // - 输入 state：KSWORD_ARK_NETWORK_TCP_STATE_*；
    // - 处理：常见 TCP 状态转为英文枚举名，未知值保留数字；
    // - 返回：TCP 明细表“状态”列文本。
    QString r0TcpStateText(const unsigned long state)
    {
        switch (state)
        {
        case KSWORD_ARK_NETWORK_TCP_STATE_CLOSED: return QStringLiteral("CLOSED");
        case KSWORD_ARK_NETWORK_TCP_STATE_LISTEN: return QStringLiteral("LISTEN");
        case KSWORD_ARK_NETWORK_TCP_STATE_SYN_SENT: return QStringLiteral("SYN_SENT");
        case KSWORD_ARK_NETWORK_TCP_STATE_SYN_RCVD: return QStringLiteral("SYN_RCVD");
        case KSWORD_ARK_NETWORK_TCP_STATE_ESTABLISHED: return QStringLiteral("ESTABLISHED");
        case KSWORD_ARK_NETWORK_TCP_STATE_FIN_WAIT_1: return QStringLiteral("FIN_WAIT_1");
        case KSWORD_ARK_NETWORK_TCP_STATE_FIN_WAIT_2: return QStringLiteral("FIN_WAIT_2");
        case KSWORD_ARK_NETWORK_TCP_STATE_CLOSE_WAIT: return QStringLiteral("CLOSE_WAIT");
        case KSWORD_ARK_NETWORK_TCP_STATE_CLOSING: return QStringLiteral("CLOSING");
        case KSWORD_ARK_NETWORK_TCP_STATE_LAST_ACK: return QStringLiteral("LAST_ACK");
        case KSWORD_ARK_NETWORK_TCP_STATE_TIME_WAIT: return QStringLiteral("TIME_WAIT");
        case KSWORD_ARK_NETWORK_TCP_STATE_DELETE_TCB: return QStringLiteral("DELETE_TCB");
        default: return QStringLiteral("STATE(%1)").arg(state);
        }
    }

    // r0GuidText 作用：
    // - 输入 bytes：R0 WFP 行中的 16 字节 GUID；
    // - 处理：复制到 GUID 后复用 Qt/Windows 格式化路径；
    // - 返回：标准 GUID 文本。
    QString r0GuidText(const unsigned char bytes[16])
    {
        if (bytes == nullptr)
        {
            return QStringLiteral("<无GUID>");
        }
        GUID guid{};
        std::memcpy(&guid, bytes, sizeof(guid));
        return QStringLiteral("{%1-%2-%3-%4%5-%6%7%8%9%10%11}")
            .arg(guid.Data1, 8, 16, QChar('0'))
            .arg(guid.Data2, 4, 16, QChar('0'))
            .arg(guid.Data3, 4, 16, QChar('0'))
            .arg(guid.Data4[0], 2, 16, QChar('0'))
            .arg(guid.Data4[1], 2, 16, QChar('0'))
            .arg(guid.Data4[2], 2, 16, QChar('0'))
            .arg(guid.Data4[3], 2, 16, QChar('0'))
            .arg(guid.Data4[4], 2, 16, QChar('0'))
            .arg(guid.Data4[5], 2, 16, QChar('0'))
            .arg(guid.Data4[6], 2, 16, QChar('0'))
            .arg(guid.Data4[7], 2, 16, QChar('0'))
            .toUpper();
    }

    // r0WfpObjectKindText 作用：
    // - 输入 kind：R0 WFP objectKind；
    // - 返回：Provider/Sublayer/Filter/Callout 等可读分类。
    QString r0WfpObjectKindText(const unsigned long kind)
    {
        switch (kind)
        {
        case KSWORD_ARK_NETWORK_WFP_OBJECT_PROVIDER: return QStringLiteral("Provider");
        case KSWORD_ARK_NETWORK_WFP_OBJECT_SUBLAYER: return QStringLiteral("Sublayer");
        case KSWORD_ARK_NETWORK_WFP_OBJECT_FILTER: return QStringLiteral("Filter");
        case KSWORD_ARK_NETWORK_WFP_OBJECT_CALLOUT: return QStringLiteral("Callout");
        default: return QStringLiteral("WFP(%1)").arg(kind);
        }
    }

    // r0NdisObjectKindText 作用：
    // - 输入 kind：R0 NDIS objectKind；
    // - 返回：未知对象明确标为 Unknown/Unproven，不把公开 device-stack 证据伪装成 LWF。
    QString r0NdisObjectKindText(const unsigned long kind)
    {
        switch (kind)
        {
        case KSWORD_ARK_NETWORK_NDIS_OBJECT_UNKNOWN:
            return QStringLiteral("未知/未证明");
        case KSWORD_ARK_NETWORK_NDIS_OBJECT_MINIPORT: return QStringLiteral("Miniport");
        case KSWORD_ARK_NETWORK_NDIS_OBJECT_FILTER: return QStringLiteral("Filter");
        case KSWORD_ARK_NETWORK_NDIS_OBJECT_PROTOCOL: return QStringLiteral("Protocol");
        case KSWORD_ARK_NETWORK_NDIS_OBJECT_BINDING: return QStringLiteral("Binding");
        default: return QStringLiteral("NDIS(%1)").arg(kind);
        }
    }

    // WfpApi：保存动态解析到的 WFP 只读枚举入口。
    struct WfpApi
    {
        using FwpmEngineOpen0Fn = DWORD(WINAPI*)(const wchar_t*, UINT32, SEC_WINNT_AUTH_IDENTITY_W*, const FWPM_SESSION0*, HANDLE*);
        using FwpmEngineClose0Fn = DWORD(WINAPI*)(HANDLE);
        using FwpmFreeMemory0Fn = void(WINAPI*)(void**);
        using FwpmProviderCreateEnumHandle0Fn = DWORD(WINAPI*)(HANDLE, const FWPM_PROVIDER_ENUM_TEMPLATE0*, HANDLE*);
        using FwpmProviderEnum0Fn = DWORD(WINAPI*)(HANDLE, HANDLE, UINT32, FWPM_PROVIDER0***, UINT32*);
        using FwpmProviderDestroyEnumHandle0Fn = DWORD(WINAPI*)(HANDLE, HANDLE);
        using FwpmSubLayerCreateEnumHandle0Fn = DWORD(WINAPI*)(HANDLE, const FWPM_SUBLAYER_ENUM_TEMPLATE0*, HANDLE*);
        using FwpmSubLayerEnum0Fn = DWORD(WINAPI*)(HANDLE, HANDLE, UINT32, FWPM_SUBLAYER0***, UINT32*);
        using FwpmSubLayerDestroyEnumHandle0Fn = DWORD(WINAPI*)(HANDLE, HANDLE);
        using FwpmCalloutCreateEnumHandle0Fn = DWORD(WINAPI*)(HANDLE, const FWPM_CALLOUT_ENUM_TEMPLATE0*, HANDLE*);
        using FwpmCalloutEnum0Fn = DWORD(WINAPI*)(HANDLE, HANDLE, UINT32, FWPM_CALLOUT0***, UINT32*);
        using FwpmCalloutDestroyEnumHandle0Fn = DWORD(WINAPI*)(HANDLE, HANDLE);
        using FwpmFilterCreateEnumHandle0Fn = DWORD(WINAPI*)(HANDLE, const FWPM_FILTER_ENUM_TEMPLATE0*, HANDLE*);
        using FwpmFilterEnum0Fn = DWORD(WINAPI*)(HANDLE, HANDLE, UINT32, FWPM_FILTER0***, UINT32*);
        using FwpmFilterDestroyEnumHandle0Fn = DWORD(WINAPI*)(HANDLE, HANDLE);

        HMODULE moduleHandle = nullptr;
        FwpmEngineOpen0Fn engineOpen = nullptr;
        FwpmEngineClose0Fn engineClose = nullptr;
        FwpmFreeMemory0Fn freeMemory = nullptr;
        FwpmProviderCreateEnumHandle0Fn providerCreateEnumHandle = nullptr;
        FwpmProviderEnum0Fn providerEnum = nullptr;
        FwpmProviderDestroyEnumHandle0Fn providerDestroyEnumHandle = nullptr;
        FwpmSubLayerCreateEnumHandle0Fn subLayerCreateEnumHandle = nullptr;
        FwpmSubLayerEnum0Fn subLayerEnum = nullptr;
        FwpmSubLayerDestroyEnumHandle0Fn subLayerDestroyEnumHandle = nullptr;
        FwpmCalloutCreateEnumHandle0Fn calloutCreateEnumHandle = nullptr;
        FwpmCalloutEnum0Fn calloutEnum = nullptr;
        FwpmCalloutDestroyEnumHandle0Fn calloutDestroyEnumHandle = nullptr;
        FwpmFilterCreateEnumHandle0Fn filterCreateEnumHandle = nullptr;
        FwpmFilterEnum0Fn filterEnum = nullptr;
        FwpmFilterDestroyEnumHandle0Fn filterDestroyEnumHandle = nullptr;
    };

    WfpApi& wfpApi()
    {
        static WfpApi api;
        return api;
    }

    // loadWfpApi 作用：
    // - 只读加载 fwpuclnt.dll 并解析 WFP 枚举入口；
    // - errorTextOut 记录失败原因；
    // - 返回 true 表示可继续枚举。
    bool loadWfpApi(QString* errorTextOut)
    {
        WfpApi& api = wfpApi();
        if (api.moduleHandle == nullptr)
        {
            api.moduleHandle = ::LoadLibraryW(L"fwpuclnt.dll");
        }
        if (api.moduleHandle == nullptr)
        {
            if (errorTextOut != nullptr)
            {
                *errorTextOut = QStringLiteral("无法加载 fwpuclnt.dll。");
            }
            return false;
        }

        auto procAddress = [moduleHandle = api.moduleHandle](const char* nameText) -> FARPROC
        {
            return ::GetProcAddress(moduleHandle, nameText);
        };

        api.engineOpen = reinterpret_cast<WfpApi::FwpmEngineOpen0Fn>(procAddress("FwpmEngineOpen0"));
        api.engineClose = reinterpret_cast<WfpApi::FwpmEngineClose0Fn>(procAddress("FwpmEngineClose0"));
        api.freeMemory = reinterpret_cast<WfpApi::FwpmFreeMemory0Fn>(procAddress("FwpmFreeMemory0"));
        api.providerCreateEnumHandle = reinterpret_cast<WfpApi::FwpmProviderCreateEnumHandle0Fn>(procAddress("FwpmProviderCreateEnumHandle0"));
        api.providerEnum = reinterpret_cast<WfpApi::FwpmProviderEnum0Fn>(procAddress("FwpmProviderEnum0"));
        api.providerDestroyEnumHandle = reinterpret_cast<WfpApi::FwpmProviderDestroyEnumHandle0Fn>(procAddress("FwpmProviderDestroyEnumHandle0"));
        api.subLayerCreateEnumHandle = reinterpret_cast<WfpApi::FwpmSubLayerCreateEnumHandle0Fn>(procAddress("FwpmSubLayerCreateEnumHandle0"));
        api.subLayerEnum = reinterpret_cast<WfpApi::FwpmSubLayerEnum0Fn>(procAddress("FwpmSubLayerEnum0"));
        api.subLayerDestroyEnumHandle = reinterpret_cast<WfpApi::FwpmSubLayerDestroyEnumHandle0Fn>(procAddress("FwpmSubLayerDestroyEnumHandle0"));
        api.calloutCreateEnumHandle = reinterpret_cast<WfpApi::FwpmCalloutCreateEnumHandle0Fn>(procAddress("FwpmCalloutCreateEnumHandle0"));
        api.calloutEnum = reinterpret_cast<WfpApi::FwpmCalloutEnum0Fn>(procAddress("FwpmCalloutEnum0"));
        api.calloutDestroyEnumHandle = reinterpret_cast<WfpApi::FwpmCalloutDestroyEnumHandle0Fn>(procAddress("FwpmCalloutDestroyEnumHandle0"));
        api.filterCreateEnumHandle = reinterpret_cast<WfpApi::FwpmFilterCreateEnumHandle0Fn>(procAddress("FwpmFilterCreateEnumHandle0"));
        api.filterEnum = reinterpret_cast<WfpApi::FwpmFilterEnum0Fn>(procAddress("FwpmFilterEnum0"));
        api.filterDestroyEnumHandle = reinterpret_cast<WfpApi::FwpmFilterDestroyEnumHandle0Fn>(procAddress("FwpmFilterDestroyEnumHandle0"));

        if (api.engineOpen == nullptr || api.engineClose == nullptr || api.freeMemory == nullptr ||
            api.providerCreateEnumHandle == nullptr || api.providerEnum == nullptr || api.providerDestroyEnumHandle == nullptr ||
            api.subLayerCreateEnumHandle == nullptr || api.subLayerEnum == nullptr || api.subLayerDestroyEnumHandle == nullptr ||
            api.calloutCreateEnumHandle == nullptr || api.calloutEnum == nullptr || api.calloutDestroyEnumHandle == nullptr ||
            api.filterCreateEnumHandle == nullptr || api.filterEnum == nullptr || api.filterDestroyEnumHandle == nullptr)
        {
            if (errorTextOut != nullptr)
            {
                *errorTextOut = QStringLiteral("fwpuclnt.dll 缺少必要导出。");
            }
            return false;
        }

        return true;
    }

    // openWfpEngine 作用：
    // - 打开一个只读 WFP engine 会话；
    // - 失败时返回 false，并写入错误文本。
    bool openWfpEngine(HANDLE& engineHandleOut, QString* errorTextOut)
    {
        engineHandleOut = nullptr;
        if (!loadWfpApi(errorTextOut))
        {
            return false;
        }

        FWPM_SESSION0 session{};
        session.flags = FWPM_SESSION_FLAG_DYNAMIC;
        session.displayData.name = const_cast<wchar_t*>(L"KswordNetworkAudit");
        session.displayData.description = const_cast<wchar_t*>(L"Ksword network readonly audit session");

        const DWORD status = wfpApi().engineOpen(nullptr, RPC_C_AUTHN_DEFAULT, nullptr, &session, &engineHandleOut);
        if (status != ERROR_SUCCESS)
        {
            engineHandleOut = nullptr;
            if (errorTextOut != nullptr)
            {
                *errorTextOut = QStringLiteral("FwpmEngineOpen0 失败：%1").arg(status);
            }
            return false;
        }

        return true;
    }

    QString displayDataText(const FWPM_DISPLAY_DATA0* displayData)
    {
        if (displayData == nullptr)
        {
            return QString();
        }
        QStringList list;
        if (displayData->name != nullptr)
        {
            list.push_back(QString::fromWCharArray(displayData->name));
        }
        if (displayData->description != nullptr)
        {
            list.push_back(QString::fromWCharArray(displayData->description));
        }
        return joinCompactLines(list);
    }

    QString wfpFlagsText(const std::uint64_t flags)
    {
        return QStringLiteral("0x%1").arg(QString::number(flags, 16));
    }

    // normalizeJsonArray 作用：
    // - 把 PowerShell ConvertTo-Json 的“单对象/数组”两种输出统一成数组；
    // - 返回的数组可直接遍历。
    QJsonArray normalizeJsonArray(const QJsonValue& value)
    {
        if (value.isArray())
        {
            return value.toArray();
        }
        if (value.isObject())
        {
            QJsonArray array;
            array.push_back(value.toObject());
            return array;
        }
        return {};
    }
}

NetworkAuditPage::NetworkAuditPage(QWidget* parent)
    : QWidget(parent),
      m_asyncState(std::make_shared<NetworkAuditAsyncState>())
{
    m_asyncState->owner = this;
    m_crossPreparation = new ks::ui::AsyncOperation(this);
    initializeUi();
    m_crossAutoRefreshTimer = new QTimer(this);
    m_crossAutoRefreshTimer->setInterval(2200);
    initializeConnections();
}

NetworkAuditPage::~NetworkAuditPage()
{
    m_asyncState->closing.store(true);
    if (m_crossPreparation) m_crossPreparation->close();
    if (m_crossFilterOperation) m_crossFilterOperation->close();
    if (m_crossDiffOperation) m_crossDiffOperation->close();
    if (m_asyncState)
    {
        std::lock_guard<std::mutex> lock(m_asyncState->mutex);
        m_asyncState->owner = nullptr;
    }
    if (m_crossSearchTimer) m_crossSearchTimer->stop();
    if (m_tcpTable && m_tcpTable->selectionModel())
        QObject::disconnect(m_tcpTable->selectionModel(), nullptr, this, nullptr);
}

void NetworkAuditPage::showEvent(QShowEvent* event)
{
    QWidget::showEvent(event);
    presentVisibleSnapshot();
}

void NetworkAuditPage::changeEvent(QEvent* event)
{
    QWidget::changeEvent(event);
    if (!event || event->type() != QEvent::LanguageChange) return;
    ++m_crossLanguageGeneration;
    m_crossFilterDirty = true;
    requestCrossViewPreparation();
    for (auto* table : {m_tcpTable, m_udpTable, m_crossSummaryTable})
        if (table)
        {
            table->viewport()->update();
            table->horizontalHeader()->viewport()->update();
        }
}

void NetworkAuditPage::requestInitialRefresh()
{
    bool expected = false;
    if (m_initialRefreshRequested.compare_exchange_strong(expected, true))
    {
        refreshAllSnapshotsAsync(false);
    }
}

void NetworkAuditPage::focusProcessIds(const QSet<quint32>& processIds)
{
    m_processFilterSet = processIds;
    m_crossFilterDirty = true;
    activateCrossView();
    updateProcessFilterLabel();
    applyCrossViewSearchFilter();
    updateCrossViewActionState();
    refreshAllSnapshotsAsync(true);
}

void NetworkAuditPage::updateProcessFilterLabel()
{
    if (m_crossFilterLabel != nullptr)
    {
        QStringList processIdTextList;
        processIdTextList.reserve(m_processFilterSet.size());
        for (const quint32 processId : m_processFilterSet)
        {
            processIdTextList.push_back(QString::number(processId));
        }
        processIdTextList.sort();
        m_crossFilterLabel->setText(
            m_processFilterSet.isEmpty()
                ? QStringLiteral("PID 筛选：无")
                : QStringLiteral("PID 筛选：%1 个进程")
                    .arg(m_processFilterSet.size()));
        m_crossFilterLabel->setToolTip(
            m_processFilterSet.isEmpty()
                ? QString()
                : QStringLiteral("PID：%1")
                    .arg(processIdTextList.join(',')));
    }
}

void NetworkAuditPage::activateCrossView()
{
    if (m_sectionTabWidget != nullptr && m_crossViewPage != nullptr)
    {
        m_sectionTabWidget->setCurrentWidget(m_crossViewPage);
    }
}

void NetworkAuditPage::setTrackProcessHandler(ProcessActionHandler handler)
{
    m_trackProcessHandler = std::move(handler);
}

void NetworkAuditPage::setOpenProcessDetailHandler(ProcessActionHandler handler)
{
    m_openProcessDetailHandler = std::move(handler);
}

void NetworkAuditPage::setUdpEndpointBlockRuleHandler(UdpEndpointBlockRuleHandler handler)
{
    m_udpEndpointBlockRuleHandler = std::move(handler);
}

void NetworkAuditPage::initializeUi()
{
    m_rootLayout = new QVBoxLayout(this);
    m_rootLayout->setContentsMargins(10, 10, 10, 10);
    m_rootLayout->setSpacing(10);

    // 审计状态行与其结果分类组成连续区域；其它页内工具行保留自己的间隔。
    QWidget* const auditResults = new QWidget(this);
    auto* const auditResultsLayout = new QVBoxLayout(auditResults);
    auditResultsLayout->setContentsMargins(0, 0, 0, 0);
    auditResultsLayout->setSpacing(0);
    m_rootLayout->addWidget(auditResults, 1);
    QWidget* const auditHeader = new QWidget(auditResults);
    m_headerLayout = new QHBoxLayout(auditHeader);
    m_headerLayout->setContentsMargins(8, 6, 8, 6);
    m_headerLayout->setSpacing(6);

    QLabel* titleLabel = new QLabel(QStringLiteral("网络只读审计"), this);
    titleLabel->setStyleSheet(QStringLiteral("font-size:16px;font-weight:700;color:%1;").arg(KswordTheme::TextPrimaryHex()));
    m_headerLayout->addWidget(titleLabel);

    m_statusLabel = new QLabel(QStringLiteral("状态：等待刷新"), this);
    m_statusLabel->setWordWrap(true);
    m_statusLabel->setMinimumWidth(0);
    m_statusLabel->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    m_headerLayout->addWidget(m_statusLabel, 1);

    m_refreshButton = new QPushButton(QStringLiteral("刷新"), this);
    // 此动作使用纯色主题，保留页面原有图标、状态和业务连接。
    ks::ui::ApplyFlatButtonTheme(m_refreshButton, ks::ui::FlatButtonTone::Neutral,
        ks::ui::FlatButtonAppearance::Solid);
    m_refreshButton->setIcon(QIcon(QStringLiteral(":/Icon/process_refresh.svg")));
    m_headerLayout->addWidget(m_refreshButton);
    ks::ui::NormalizeToolbarRow(m_headerLayout);

    auditResultsLayout->addWidget(auditHeader);

    m_sectionTabWidget = new QTabWidget(this);
    ks::ui::StyleJoinedResultTabs(m_sectionTabWidget, auditHeader);
    m_sectionTabWidget->setTabPosition(QTabWidget::North);
    auditResultsLayout->addWidget(m_sectionTabWidget, 1);

    // TCP/UDP cross-view。
    m_crossViewPage = new QWidget(this);
    m_auditPageFactories.insert(m_crossViewPage, [this]()
    {
    QVBoxLayout* crossLayout = new QVBoxLayout(m_crossViewPage);
    crossLayout->setContentsMargins(6, 10, 6, 6);
    crossLayout->setSpacing(8);

    // 连接管理动作已合并到 Cross-View，不再单独占用顶层 Tab。
    QWidget* const crossToolbar = new QWidget(m_crossViewPage);
    // 两条操作行使用与其他网络工具相同的工具面，不再与结果面混为同层。
    ks::ui::StylePrimaryToolbar(crossToolbar);
    QVBoxLayout* const crossToolbarLayout = new QVBoxLayout(crossToolbar);
    crossToolbarLayout->setContentsMargins(12, 10, 12, 10);
    crossToolbarLayout->setSpacing(8);
    QHBoxLayout* crossSearchLayout = new QHBoxLayout();
    crossSearchLayout->setContentsMargins(0, 0, 0, 0);
    crossSearchLayout->setSpacing(6);
    m_crossSearchEdit = new QLineEdit(m_crossViewPage);
    ks::ui::StyleSearchField(m_crossSearchEdit);
    m_crossSearchEdit->setClearButtonEnabled(true);
    m_crossSearchEdit->setPlaceholderText(QStringLiteral("搜索 PID / 进程 / 端点 / 状态 / 来源 / 明细 / 摘要"));
    m_crossSearchEdit->setMinimumWidth(220);
    crossSearchLayout->addWidget(m_crossSearchEdit, 1);
    ks::ui::NormalizeToolbarRow(crossSearchLayout);
    crossToolbarLayout->addLayout(crossSearchLayout);

    m_crossControlLayout = new QHBoxLayout();
    m_crossControlLayout->setContentsMargins(0, 0, 0, 0);
    m_crossControlLayout->setSpacing(6);

    m_crossAutoRefreshButton = new QPushButton(QStringLiteral("自动刷新"), m_crossViewPage);
    // 此动作使用纯色主题，保留页面原有图标、状态和业务连接。
    ks::ui::ApplyFlatButtonTheme(m_crossAutoRefreshButton, ks::ui::FlatButtonTone::Neutral,
        ks::ui::FlatButtonAppearance::Solid);
    m_crossAutoRefreshButton->setIcon(QIcon(QStringLiteral(":/Icon/process_refresh.svg")));
    m_crossAutoRefreshButton->setToolTip(QStringLiteral("每 2.2 秒自动刷新 TCP/UDP Cross-View"));
    m_crossAutoRefreshButton->setCheckable(true);
    m_crossAutoRefreshButton->setChecked(true);
    m_crossControlLayout->addWidget(m_crossAutoRefreshButton);

    m_crossTerminateButton = new QPushButton(QStringLiteral("终止 TCP"), m_crossViewPage);
    // 此动作使用纯色主题，保留页面原有图标、状态和业务连接。
    ks::ui::ApplyFlatButtonTheme(m_crossTerminateButton, ks::ui::FlatButtonTone::Danger,
        ks::ui::FlatButtonAppearance::Solid);
    m_crossTerminateButton->setIcon(QIcon(QStringLiteral(":/Icon/process_terminate.svg")));
    m_crossTerminateButton->setToolTip(QStringLiteral("终止选中的 R3 IPv4 TCP 活动连接"));
    m_crossTerminateButton->setEnabled(false);
    m_crossControlLayout->addWidget(m_crossTerminateButton);

    m_clearProcessFilterButton = new QPushButton(QStringLiteral("清除 PID 筛选"), m_crossViewPage);
    // 此动作使用纯色主题，保留页面原有图标、状态和业务连接。
    ks::ui::ApplyFlatButtonTheme(m_clearProcessFilterButton, ks::ui::FlatButtonTone::Neutral,
        ks::ui::FlatButtonAppearance::Solid);
    m_clearProcessFilterButton->setIcon(QIcon(QStringLiteral(":/Icon/log_clear.svg")));
    m_clearProcessFilterButton->setToolTip(QStringLiteral("显示全部进程的 TCP/UDP 连接"));
    m_clearProcessFilterButton->setEnabled(false);
    m_crossControlLayout->addWidget(m_clearProcessFilterButton);

    m_crossFilterLabel = new QLabel(QStringLiteral("PID 筛选：无"), m_crossViewPage);
    m_crossFilterLabel->setMinimumWidth(0);
    m_crossFilterLabel->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    m_crossControlLayout->addWidget(m_crossFilterLabel, 1);
    ks::ui::NormalizeToolbarRow(m_crossControlLayout);
    // 搜索与动作共享一个紧凑区域，三个结果表仍由原分割器分配空间。
    crossToolbarLayout->addLayout(m_crossControlLayout);
    crossLayout->addWidget(crossToolbar);

    m_crossViewSplitter = new QSplitter(Qt::Vertical, m_crossViewPage);
    m_crossViewTopSplitter = new QSplitter(Qt::Horizontal, m_crossViewSplitter);
    m_crossViewSplitter->setHandleWidth(10);
    m_crossViewTopSplitter->setHandleWidth(10);
    m_crossViewSplitter->setChildrenCollapsible(false);
    m_crossViewTopSplitter->setChildrenCollapsible(false);

    m_tcpTable = new ks::ui::TableActionTableView(m_crossViewPage);
    // 并排端点表保留复制/导出，快照和对比入口不重复占据每张表。
    ks::ui::SetTableActionBarMode(m_tcpTable, ks::ui::TableActionBarMode::Compact);
    configureCrossViewValueTable(m_tcpTable, {
        QStringLiteral("PID"),
        QStringLiteral("进程"),
        QStringLiteral("本地端点"),
        QStringLiteral("远端端点"),
        QStringLiteral("状态")
    }, true);
    m_tcpTable->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_tcpTable->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_tcpTable->setSelectionMode(QAbstractItemView::SingleSelection);
    m_tcpTable->verticalHeader()->setVisible(false);
    m_tcpTable->horizontalHeader()->setStretchLastSection(true);
    m_tcpTable->horizontalHeader()->setSectionResizeMode(QHeaderView::Stretch);
    m_tcpTable->setContextMenuPolicy(Qt::CustomContextMenu);

    m_udpTable = new ks::ui::TableActionTableView(m_crossViewPage);
    ks::ui::SetTableActionBarMode(m_udpTable, ks::ui::TableActionBarMode::Compact);
    configureCrossViewValueTable(m_udpTable, {
        QStringLiteral("PID"),
        QStringLiteral("进程"),
        QStringLiteral("本地端点"),
        QStringLiteral("来源")
    }, false);
    m_udpTable->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_udpTable->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_udpTable->setSelectionMode(QAbstractItemView::SingleSelection);
    m_udpTable->verticalHeader()->setVisible(false);
    m_udpTable->horizontalHeader()->setStretchLastSection(true);
    m_udpTable->horizontalHeader()->setSectionResizeMode(QHeaderView::Stretch);
    m_udpTable->setContextMenuPolicy(Qt::CustomContextMenu);

    m_crossSummaryTable = new ks::ui::TableActionTableView(m_crossViewPage);
    // 汇总由上方端点数据派生，自有复制菜单足够，无需独立快照栏。
    ks::ui::SetTableActionBarMode(m_crossSummaryTable, ks::ui::TableActionBarMode::None);
    configureCrossViewValueTable(m_crossSummaryTable, { QStringLiteral("PID"), QStringLiteral("进程"), QStringLiteral("TCP"), QStringLiteral("UDP") }, false);
    m_crossSummaryTable->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_crossSummaryTable->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_crossSummaryTable->setSelectionMode(QAbstractItemView::SingleSelection);
    m_crossSummaryTable->verticalHeader()->setVisible(false);
    m_crossSummaryTable->horizontalHeader()->setStretchLastSection(true);
    m_crossSummaryTable->horizontalHeader()->setSectionResizeMode(QHeaderView::Stretch);
    installCopyMenu(m_crossSummaryTable, 0, true);

    // 三个数据集使用独立标题和轻圆角表面；并排两表等宽、共享同一高度。
    auto addPanel = [](QTableView* table, const QString& key, const QString& title, QSplitter* splitter)
    {
        QWidget* panel = ks::ui::CreateTitledTablePanel(table, title, splitter);
        ks::i18n::LanguageManager::instance().bindText(
            panel->findChild<QLabel*>(QStringLiteral("ksword_table_panel_title")), key, title);
        splitter->addWidget(panel);
    };
    addPanel(m_tcpTable, QStringLiteral("network.cross.tcp_title"), QStringLiteral("TCP 连接"), m_crossViewTopSplitter);
    addPanel(m_udpTable, QStringLiteral("network.cross.udp_title"), QStringLiteral("UDP 端点"), m_crossViewTopSplitter);
    addPanel(m_crossSummaryTable, QStringLiteral("network.cross.summary_title"), QStringLiteral("进程汇总"), m_crossViewSplitter);
    m_crossViewTopSplitter->setStretchFactor(0, 1);
    m_crossViewTopSplitter->setStretchFactor(1, 1);
    m_crossViewTopSplitter->setSizes({1, 1});
    m_crossViewSplitter->setStretchFactor(0, 3);
    m_crossViewSplitter->setStretchFactor(1, 2);
    crossLayout->addWidget(m_crossViewSplitter, 1);
    initializeCrossViewConnections();
    updateProcessFilterLabel();
    for (auto* table : {m_tcpTable, m_udpTable, m_crossSummaryTable}) m_dirtyAuditTables.insert(table);
    });
    network_navigation_detail::AddTab(m_sectionTabWidget, m_crossViewPage, QStringLiteral("audit.cross-view"));

    // AFD。
    m_afdPage = new QWidget(this);
    m_auditPageFactories.insert(m_afdPage, [this]()
    {
    QVBoxLayout* afdLayout = new QVBoxLayout(m_afdPage);
    afdLayout->setContentsMargins(4, 4, 4, 4);
    afdLayout->setSpacing(6);
    m_afdTable = new ks::ui::TableActionTableView(m_afdPage);
    // AFD 是可独立保留现场的对象清单，保留完整操作栏。
    ks::ui::SetTableActionBarMode(m_afdTable, ks::ui::TableActionBarMode::Full);
    setAuditValueHeaders(m_afdTable, {
        QStringLiteral("PID"),
        QStringLiteral("进程"),
        QStringLiteral("句柄"),
        QStringLiteral("类型"),
        QStringLiteral("对象名"),
        QStringLiteral("来源"),
        QStringLiteral("交叉视图"),
        QStringLiteral("详情")
    });
    m_afdTable->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_afdTable->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_afdTable->setSelectionMode(QAbstractItemView::ExtendedSelection);
    m_afdTable->verticalHeader()->setVisible(false);
    m_afdTable->horizontalHeader()->setStretchLastSection(true);
    m_afdTable->horizontalHeader()->setSectionResizeMode(QHeaderView::Stretch);
    installCopyMenu(m_afdTable, 0);
    afdLayout->addWidget(m_afdTable, 1);
    m_dirtyAuditTables.insert(m_afdTable);
    });
    network_navigation_detail::AddTab(m_sectionTabWidget, m_afdPage, QStringLiteral("audit.afd"));

    // WFP。
    m_wfpPage = new QWidget(this);
    QVBoxLayout* wfpLayout = new QVBoxLayout(m_wfpPage);
    wfpLayout->setContentsMargins(4, 4, 4, 4);
    wfpLayout->setSpacing(6);
    m_wfpTabWidget = new QTabWidget(m_wfpPage);
    ks::ui::StylePageTabsContentFrame(m_wfpTabWidget);
    m_wfpTabWidget->setTabPosition(QTabWidget::North);
    wfpLayout->addWidget(m_wfpTabWidget, 1);

    addLazyAuditTableTab(m_wfpTabWidget, &NetworkAuditPage::m_wfpProviderTable,
        { QStringLiteral("名称"), QStringLiteral("描述"), QStringLiteral("GUID"), QStringLiteral("Flags"), QStringLiteral("Service"), QStringLiteral("数据大小") }, QStringLiteral("audit.wfp.provider"), false);
    addLazyAuditTableTab(m_wfpTabWidget, &NetworkAuditPage::m_wfpSubLayerTable,
        { QStringLiteral("名称"), QStringLiteral("描述"), QStringLiteral("GUID"), QStringLiteral("Flags"), QStringLiteral("Provider"), QStringLiteral("Weight") }, QStringLiteral("audit.wfp.sublayer"), false);
    addLazyAuditTableTab(m_wfpTabWidget, &NetworkAuditPage::m_wfpCalloutTable,
        { QStringLiteral("名称"), QStringLiteral("描述"), QStringLiteral("GUID"), QStringLiteral("Flags"), QStringLiteral("Provider"), QStringLiteral("Layer"), QStringLiteral("CalloutId") }, QStringLiteral("audit.wfp.callout"), false);
    addLazyAuditTableTab(m_wfpTabWidget, &NetworkAuditPage::m_wfpFilterTable,
        { QStringLiteral("名称"), QStringLiteral("描述"), QStringLiteral("GUID"), QStringLiteral("Flags"), QStringLiteral("Provider"), QStringLiteral("Layer"), QStringLiteral("Sublayer"), QStringLiteral("Weight"), QStringLiteral("Action"), QStringLiteral("Conditions"), QStringLiteral("FilterId") }, QStringLiteral("audit.wfp.filter"), true);
    network_navigation_detail::AddTab(m_sectionTabWidget, m_wfpPage, QStringLiteral("audit.wfp"));

    // NDIS。
    m_ndisPage = new QWidget(this);
    QVBoxLayout* ndisLayout = new QVBoxLayout(m_ndisPage);
    ndisLayout->setContentsMargins(4, 4, 4, 4);
    ndisLayout->setSpacing(6);
    m_ndisTabWidget = new QTabWidget(m_ndisPage);
    ks::ui::StylePageTabsContentFrame(m_ndisTabWidget);
    m_ndisTabWidget->setTabPosition(QTabWidget::North);
    ndisLayout->addWidget(m_ndisTabWidget, 1);
    addLazyAuditTableTab(m_ndisTabWidget, &NetworkAuditPage::m_ndisAdapterTable,
        { QStringLiteral("名称"), QStringLiteral("描述"), QStringLiteral("IfIndex"), QStringLiteral("状态"), QStringLiteral("MAC"), QStringLiteral("速率"), QStringLiteral("连接") }, QStringLiteral("audit.ndis.miniport"), false);
    addLazyAuditTableTab(m_ndisTabWidget, &NetworkAuditPage::m_ndisBindingTable,
        { QStringLiteral("网卡"), QStringLiteral("显示名"), QStringLiteral("ComponentId"), QStringLiteral("启用"), QStringLiteral("InstanceId") }, QStringLiteral("audit.ndis.binding"), false);
    addLazyAuditTableTab(m_ndisTabWidget, &NetworkAuditPage::m_ndisProtocolTable,
        { QStringLiteral("别名"), QStringLiteral("IfIndex"), QStringLiteral("地址族"), QStringLiteral("连接"), QStringLiteral("Metric"), QStringLiteral("MTU") }, QStringLiteral("audit.ndis.protocol"), false);
    addLazyAuditTableTab(m_ndisTabWidget, &NetworkAuditPage::m_ndisUnknownTable,
        { QStringLiteral("类型"), QStringLiteral("组件"), QStringLiteral("所属模块"), QStringLiteral("对象地址"), QStringLiteral("详情") }, QStringLiteral("audit.ndis.unknown"), true);
    network_navigation_detail::AddTab(m_sectionTabWidget, m_ndisPage, QStringLiteral("audit.ndis"));

    // NSI。
    m_nsiPage = new QWidget(this);
    m_auditPageFactories.insert(m_nsiPage, [this]()
    {
    QVBoxLayout* nsiLayout = new QVBoxLayout(m_nsiPage);
    nsiLayout->setContentsMargins(4, 4, 4, 4);
    nsiLayout->setSpacing(6);
    m_nsiSummaryTable = new ks::ui::VisibleTableWidget(m_nsiPage);
    // NSI 仅给出固定指标和能力边界，复制菜单保留，隐藏无用对比栏。
    ks::ui::SetTableActionBarMode(m_nsiSummaryTable, ks::ui::TableActionBarMode::None);
    m_nsiSummaryTable->setColumnCount(5);
    m_nsiSummaryTable->setHorizontalHeaderLabels({
        QStringLiteral("指标"),
        QStringLiteral("状态/数值"),
        QStringLiteral("返回情况"),
        QStringLiteral("是否截断"),
        QStringLiteral("说明")
    });
    m_nsiSummaryTable->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_nsiSummaryTable->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_nsiSummaryTable->setSelectionMode(QAbstractItemView::SingleSelection);
    m_nsiSummaryTable->verticalHeader()->setVisible(false);
    m_nsiSummaryTable->horizontalHeader()->setStretchLastSection(true);
    m_nsiSummaryTable->horizontalHeader()->setSectionResizeMode(QHeaderView::Stretch);
    installCopyMenu(m_nsiSummaryTable);
    nsiLayout->addWidget(m_nsiSummaryTable, 1);
    m_dirtyAuditTables.insert(m_nsiSummaryTable);
    });
    network_navigation_detail::AddTab(m_sectionTabWidget, m_nsiPage, QStringLiteral("audit.nsi"));

    // 证据模型和专业分割器保留，只统一各列表页的阅读基线。
    for (QWidget* page : {m_crossViewPage, m_afdPage, m_wfpPage, m_ndisPage, m_nsiPage})
        ks::ui::StyleWorkspacePage(page);

}

void NetworkAuditPage::addLazyAuditTableTab(QTabWidget* tabs,
    QTableView* NetworkAuditPage::* member, const QStringList& headers,
    const QString& id, bool fullToolbar)
{
    QWidget* const page = new QWidget(tabs);
    auto* const layout = new QVBoxLayout(page);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);
    m_auditPageFactories.insert(page, [this, page, layout, member, headers, fullToolbar]()
    {
        auto* const table = new ks::ui::TableActionTableView(page);
        this->*member = table;
        setAuditValueHeaders(table, headers);
        table->setEditTriggers(QAbstractItemView::NoEditTriggers);
        table->setSelectionBehavior(QAbstractItemView::SelectRows);
        table->setSelectionMode(QAbstractItemView::SingleSelection);
        table->verticalHeader()->setVisible(false);
        table->horizontalHeader()->setStretchLastSection(true);
        table->horizontalHeader()->setSectionResizeMode(QHeaderView::Stretch);
        installCopyMenu(table);
        ks::ui::SetTableActionBarMode(table, fullToolbar
            ? ks::ui::TableActionBarMode::Full : ks::ui::TableActionBarMode::Compact);
        layout->addWidget(table);
        m_dirtyAuditTables.insert(table);
    });
    network_navigation_detail::AddTab(tabs, page, id);
}

bool NetworkAuditPage::ensureCurrentAuditPage()
{
    const QPointer<NetworkAuditPage> guard(this);
    while (isVisible() && m_sectionTabWidget != nullptr)
    {
        QWidget* page = m_sectionTabWidget->currentWidget();
        if (page == m_wfpPage && m_wfpTabWidget != nullptr) page = m_wfpTabWidget->currentWidget();
        else if (page == m_ndisPage && m_ndisTabWidget != nullptr) page = m_ndisTabWidget->currentWidget();
        auto factory = m_auditPageFactories.take(page);
        if (!factory) return true;
        m_constructingAuditPage = true;
        {
            ks::ui::ScopedWidgetThemePreparation prepare(page);
            factory();
        }
        if (guard.isNull()) return false;
        m_constructingAuditPage = false;
        // Theme preparation can change the active branch. Revalidate its identity
        // before the first data projection; never build an unselected sibling.
    }
    return !guard.isNull();
}

void NetworkAuditPage::initializeConnections()
{
    connect(m_refreshButton, &QPushButton::clicked, this, [this]()
    {
        refreshAllSnapshotsAsync(true);
    });

    connect(m_crossAutoRefreshTimer, &QTimer::timeout, this, [this]()
    {
        if (isVisible()
            && m_sectionTabWidget != nullptr
            && m_sectionTabWidget->currentWidget() == m_crossViewPage)
        {
            refreshCrossViewAsync();
        }
    });
    // 采集与投影分离：切页只消费当前分支，尚未查看的表继续保留规范快照。
    for (QTabWidget* tabs : { m_sectionTabWidget, m_wfpTabWidget, m_ndisTabWidget })
    {
        connect(tabs, &QTabWidget::currentChanged, this, [this](int)
        {
            presentVisibleSnapshot();
        });
    }


}

void NetworkAuditPage::configureCrossViewValueTable(
    QTableView* table, const QStringList& headers, const bool tcp)
{
    using Model = network_value_detail::Model<CrossViewValue>;
    std::vector<Model::ColumnSpec> columns;
    for (const auto& header : headers) columns.push_back({header});
    const auto asyncState = m_asyncState;
    auto* model = new Model(std::move(columns),
        [asyncState, tcp](const CrossViewValue& value, int column, int role) -> QVariant {
            const auto& row = *value;
            if (role == Qt::DisplayRole) return row.fields.value(column);
            if (column == 0 && role == Qt::UserRole && tcp) return row.sourceIndex;
            if (column == 0 && role == Qt::UserRole + 170) return row.searchFields;
            if (column == 0 && role == Qt::UserRole + 171) return row.processId;
            if (column == kAuditProcessNameColumn && role == Qt::DecorationRole)
            {
                NetworkAuditPage* owner = nullptr;
                { std::lock_guard<std::mutex> guard(asyncState->mutex); owner = asyncState->owner; }
                if (owner) return owner->resolveProcessIcon(row.processId);
            }
            return {};
        }, table, Model::FlagsResolver{},
        [](const CrossViewValue& row) { return row->identity; },
        crossViewValueChanges<Model>(tcp));
    auto* proxy = new network_value_detail::SortProxy(table);
    proxy->request = [this]() { m_crossDiffRequestedSource.reset(); refreshCrossViewTable(m_preparedCrossView); };
    proxy->setSourceModel(model);
    table->setProperty("ks_i18n_preserve_model_data", true);
    table->setModel(proxy);
    ks::ui::SetTableRowFilterBusinessOwnership(table,
        ks::ui::TableRowFilterBusinessOwnership::ExclusiveSharedIdentity);
}

void NetworkAuditPage::initializeCrossViewConnections()
{
    connect(m_crossAutoRefreshButton, &QPushButton::toggled, this, [this](const bool enabled)
    {
        if (m_crossAutoRefreshTimer == nullptr)
        {
            return;
        }
        enabled ? m_crossAutoRefreshTimer->start() : m_crossAutoRefreshTimer->stop();
    });
    if (m_crossAutoRefreshButton != nullptr && m_crossAutoRefreshButton->isChecked())
    {
        m_crossAutoRefreshTimer->start();
    }

    m_crossSearchTimer = new QTimer(this);
    m_crossSearchTimer->setSingleShot(true);
    m_crossSearchTimer->setInterval(120);
    connect(m_crossSearchEdit, &QLineEdit::textChanged, this, [this]() {
        m_crossFilterDirty = true;
        m_crossSearchTimer->start();
    });
    connect(m_crossSearchTimer, &QTimer::timeout, this, [this]() { applyCrossViewSearchFilter(); });

    connect(m_crossTerminateButton, &QPushButton::clicked, this, [this]()
    {
        terminateSelectedTcpConnection();
    });
    connect(m_clearProcessFilterButton, &QPushButton::clicked, this, [this]()
    {
        focusProcessIds({});
    });
    connect(m_tcpTable->selectionModel(), &QItemSelectionModel::selectionChanged, this, [this]()
    {
        updateCrossViewActionState();
    });
    connect(m_tcpTable, &QTableView::customContextMenuRequested, this, [this](const QPoint& position)
    {
        showCrossViewContextMenu(m_tcpTable, position);
    });
    connect(m_udpTable, &QTableView::customContextMenuRequested, this, [this](const QPoint& position)
    {
        showCrossViewContextMenu(m_udpTable, position);
    });
}

void NetworkAuditPage::refreshAllSnapshotsAsync(const bool forceRefresh)
{
    bool expected = false;
    if (!m_refreshInProgress.compare_exchange_strong(expected, true))
    {
        if (forceRefresh && m_statusLabel != nullptr)
        {
            m_statusLabel->setText(QStringLiteral("状态：已有刷新任务在运行"));
        }
        return;
    }

    if (m_refreshButton != nullptr)
    {
        m_refreshButton->setEnabled(false);
    }
    if (m_statusLabel != nullptr)
    {
        m_statusLabel->setText(forceRefresh ? QStringLiteral("状态：正在重新采集...") : QStringLiteral("状态：正在采集..."));
    }

    const std::shared_ptr<NetworkAuditAsyncState> asyncState = m_asyncState;
    const auto language = ks::i18n::LanguageManager::instance().sourceTextSnapshot();
    const auto sourceGeneration = m_crossSourceGeneration + 1;
    const auto languageGeneration = m_crossLanguageGeneration;
    const QString previousTcpStatus = m_r0TcpStatusText;
    const QString previousUdpStatus = m_r0UdpStatusText;

    std::thread([asyncState, language, sourceGeneration, languageGeneration, previousTcpStatus, previousUdpStatus]()
    {
        AuditSnapshot snapshot;
        QString failureText;
        try
        {
            snapshot = buildAuditSnapshot();
        }
        catch (const std::exception& exception)
        {
            failureText = QStringLiteral("刷新失败：%1").arg(QString::fromUtf8(exception.what()));
        }
        catch (...)
        {
            failureText = QStringLiteral("刷新失败");
        }

        std::shared_ptr<AuditSnapshot> accepted;
        std::shared_ptr<const CrossViewPresentation> prepared;
        QString preparationFailureText;
        if (failureText.isEmpty())
        {
            try { accepted = network_value_detail::retainMutable(std::make_shared<AuditSnapshot>(std::move(snapshot))); }
            catch (const std::exception& exception) { failureText = QStringLiteral("刷新失败：%1").arg(QString::fromUtf8(exception.what())); }
            catch (...) { failureText = QStringLiteral("刷新失败"); }
            try
            {
                if (accepted) prepared = prepareCrossViewValues(accepted, language, sourceGeneration, languageGeneration,
                    accepted->r0TcpStatusText.isEmpty() ? previousTcpStatus : accepted->r0TcpStatusText,
                    accepted->r0UdpStatusText.isEmpty() ? previousUdpStatus : accepted->r0UdpStatusText,
                    [asyncState]() { return asyncState->closing.load(); });
            }
            catch (const std::exception& exception) { preparationFailureText = QString::fromUtf8(exception.what()); }
            catch (...) { preparationFailureText = QStringLiteral("刷新失败"); }
        }

        std::lock_guard<std::mutex> dispatchLock(asyncState->mutex);
        NetworkAuditPage* const receiver = asyncState->owner;
        if (receiver == nullptr)
        {
            return;
        }
        QMetaObject::invokeMethod(receiver, [asyncState, accepted = std::move(accepted), prepared = std::move(prepared), failureText, preparationFailureText]() mutable
        {
            NetworkAuditPage* page = nullptr;
            {
                std::lock_guard<std::mutex> stateLock(asyncState->mutex);
                page = asyncState->owner;
            }
            if (page == nullptr)
            {
                return;
            }
            const QPointer<NetworkAuditPage> guard(page);
            if (failureText.isEmpty())
            {
                if (!preparationFailureText.isEmpty())
                {
                    kLogEvent event;
                    warn << event << "[NetworkAuditPage] cross-view refresh failed, detail="
                        << preparationFailureText.toStdString() << eol;
                }
                page->applySnapshot(std::move(accepted), std::move(prepared));
            }
            else
            {
                kLogEvent failureEvent;
                warn << failureEvent
                    << "[NetworkAuditPage] refresh failed, detail="
                    << failureText.toStdString()
                    << eol;
                if (page->m_statusLabel != nullptr)
                {
                    page->m_statusLabel->setText(QStringLiteral(
                        "状态：刷新失败；详情已写入日志。"));
                }
            }
            if (guard.isNull()) return;
            if (page->m_refreshButton != nullptr)
            {
                page->m_refreshButton->setEnabled(true);
            }
            if (guard.isNull()) return;
            page->m_refreshInProgress.store(false);
        }, Qt::QueuedConnection);
    }).detach();
}

void NetworkAuditPage::refreshCrossViewAsync()
{
    bool expected = false;
    if (!m_refreshInProgress.compare_exchange_strong(expected, true))
    {
        return;
    }

    const std::shared_ptr<NetworkAuditAsyncState> asyncState = m_asyncState;
    const auto language = ks::i18n::LanguageManager::instance().sourceTextSnapshot();
    const auto sourceGeneration = m_crossSourceGeneration + 1;
    const auto languageGeneration = m_crossLanguageGeneration;
    const QString previousTcpStatus = m_r0TcpStatusText;
    const QString previousUdpStatus = m_r0UdpStatusText;

    const std::shared_ptr<const AuditSnapshot> previous = m_latestAuditSnapshot;
    std::thread([asyncState, previous, language, sourceGeneration, languageGeneration, previousTcpStatus, previousUdpStatus]()
    {
        AuditSnapshot snapshot;
        QString failureText;
        try
        {
            snapshot = buildAuditSnapshot(true);
            if (previous)
            {
                // Same R3-only merge, now on the existing collection worker.
                for (const auto& row : previous->tcpEndpointRows)
                    if (row.isR0Snapshot) snapshot.tcpEndpointRows.push_back(row);
                for (const auto& row : previous->udpEndpointRows)
                    if (row.sourceText.startsWith(QStringLiteral("R0"))) snapshot.udpEndpointRows.push_back(row);
                AuditSnapshot merged = *previous;
                merged.tcpEndpointRows = std::move(snapshot.tcpEndpointRows);
                merged.udpEndpointRows = std::move(snapshot.udpEndpointRows);
                merged.crossViewRows = std::move(snapshot.crossViewRows);
                snapshot = std::move(merged);
            }
        }
        catch (const std::exception& exception)
        {
            failureText = QStringLiteral("刷新失败：%1").arg(QString::fromUtf8(exception.what()));
        }
        catch (...)
        {
            failureText = QStringLiteral("刷新失败");
        }

        std::shared_ptr<AuditSnapshot> accepted;
        std::shared_ptr<const CrossViewPresentation> prepared;
        QString preparationFailureText;
        if (failureText.isEmpty())
        {
            try { accepted = network_value_detail::retainMutable(std::make_shared<AuditSnapshot>(std::move(snapshot))); }
            catch (const std::exception& exception) { failureText = QStringLiteral("刷新失败：%1").arg(QString::fromUtf8(exception.what())); }
            catch (...) { failureText = QStringLiteral("刷新失败"); }
            try
            {
                if (accepted) prepared = prepareCrossViewValues(accepted, language, sourceGeneration, languageGeneration,
                    accepted->r0TcpStatusText.isEmpty() ? previousTcpStatus : accepted->r0TcpStatusText,
                    accepted->r0UdpStatusText.isEmpty() ? previousUdpStatus : accepted->r0UdpStatusText,
                    [asyncState]() { return asyncState->closing.load(); });
            }
            catch (const std::exception& exception) { preparationFailureText = QString::fromUtf8(exception.what()); }
            catch (...) { preparationFailureText = QStringLiteral("刷新失败"); }
        }

        std::lock_guard<std::mutex> dispatchLock(asyncState->mutex);
        NetworkAuditPage* const receiver = asyncState->owner;
        if (receiver == nullptr)
        {
            return;
        }
        QMetaObject::invokeMethod(
            receiver,
            [asyncState, accepted = std::move(accepted), prepared = std::move(prepared), failureText, preparationFailureText]() mutable
            {
                NetworkAuditPage* page = nullptr;
                {
                    std::lock_guard<std::mutex> stateLock(asyncState->mutex);
                    page = asyncState->owner;
                }
                if (page == nullptr)
                {
                    return;
                }

                const QPointer<NetworkAuditPage> guard(page);
                if (failureText.isEmpty())
                {
                    if (!preparationFailureText.isEmpty())
                    {
                        kLogEvent event;
                        warn << event << "[NetworkAuditPage] cross-view refresh failed, detail="
                            << preparationFailureText.toStdString() << eol;
                    }
                    page->applyCrossViewSnapshot(std::move(accepted), std::move(prepared));
                }
                else
                {
                    kLogEvent failureEvent;
                    warn << failureEvent
                        << "[NetworkAuditPage] cross-view refresh failed, detail="
                        << failureText.toStdString()
                        << eol;
                    if (page->m_statusLabel != nullptr)
                    {
                        page->m_statusLabel->setText(QStringLiteral(
                            "状态：刷新失败；详情已写入日志。"));
                    }
                }
                if (guard.isNull()) return;
                page->m_refreshInProgress.store(false);
            },
            Qt::QueuedConnection);
    }).detach();
}

NetworkAuditPage::AuditSnapshot NetworkAuditPage::buildAuditSnapshot(const bool crossViewOnly)
{
    AuditSnapshot snapshot;

    // TCP/UDP cross-view：先分别枚举，再按 PID 聚合。
    std::vector<ks::network::TcpConnectionRecord> tcpRecords;
    std::vector<ks::network::UdpEndpointRecord> udpRecords;
    std::string tcpError;
    std::string udpError;
    const bool tcpOk = ks::network::EnumerateTcpConnectionRecords(tcpRecords, &tcpError);
    const bool udpOk = ks::network::EnumerateUdpEndpointRecords(udpRecords, &udpError);

    struct CrossViewAggregate
    {
        CrossViewRow row;
        QStringList tcpSummaryLines;
        QStringList udpSummaryLines;
    };
    std::unordered_map<std::uint32_t, CrossViewAggregate> crossMap;
    snapshot.tcpEndpointRows.reserve(tcpRecords.size());
    for (const ks::network::TcpConnectionRecord& tcpRecord : tcpRecords)
    {
        CrossViewAggregate& aggregate = crossMap[tcpRecord.processId];
        CrossViewRow& row = aggregate.row;
        row.processId = tcpRecord.processId;
        row.processName = QString::fromUtf8(tcpRecord.processName.c_str());
        ++row.tcpCount;
        aggregate.tcpSummaryLines.push_back(
            QStringLiteral("%1:%2 -> %3:%4 (%5)")
            .arg(QString::fromUtf8(tcpRecord.localAddressText.c_str()))
            .arg(tcpRecord.localPort)
            .arg(QString::fromUtf8(tcpRecord.remoteAddressText.c_str()))
            .arg(tcpRecord.remotePort)
            .arg(QString::fromUtf8(tcpRecord.tcpStateText.c_str()))
        );

        TcpEndpointRow endpointRow;
        endpointRow.processId = tcpRecord.processId;
        endpointRow.processName = QString::fromUtf8(tcpRecord.processName.c_str());
        endpointRow.localEndpointText = QStringLiteral("%1:%2")
            .arg(QString::fromUtf8(tcpRecord.localAddressText.c_str()))
            .arg(tcpRecord.localPort);
        endpointRow.remoteEndpointText = QStringLiteral("%1:%2")
            .arg(QString::fromUtf8(tcpRecord.remoteAddressText.c_str()))
            .arg(tcpRecord.remotePort);
        endpointRow.stateText = QString::fromUtf8(tcpRecord.tcpStateText.c_str());
        endpointRow.detailText = QStringLiteral("来源=R3 TCP table；PID=%1；状态=%2")
            .arg(tcpRecord.processId)
            .arg(endpointRow.stateText);
        endpointRow.canTerminate = true;
        endpointRow.connectionRecord = tcpRecord;
        snapshot.tcpEndpointRows.push_back(std::move(endpointRow));
    }
    snapshot.udpEndpointRows.reserve(udpRecords.size());
    for (const ks::network::UdpEndpointRecord& udpRecord : udpRecords)
    {
        CrossViewAggregate& aggregate = crossMap[udpRecord.processId];
        CrossViewRow& row = aggregate.row;
        row.processId = udpRecord.processId;
        row.processName = QString::fromUtf8(udpRecord.processName.c_str());
        ++row.udpCount;
        aggregate.udpSummaryLines.push_back(
            QStringLiteral("%1:%2")
            .arg(QString::fromUtf8(udpRecord.localAddressText.c_str()))
            .arg(udpRecord.localPort)
        );

        UdpEndpointRow endpointRow;
        endpointRow.processId = udpRecord.processId;
        endpointRow.processName = QString::fromUtf8(udpRecord.processName.c_str());
        endpointRow.localEndpointText = QStringLiteral("%1:%2")
            .arg(QString::fromUtf8(udpRecord.localAddressText.c_str()))
            .arg(udpRecord.localPort);
        endpointRow.sourceText = QStringLiteral("R3 UDP table");
        endpointRow.detailText = QStringLiteral("来源=R3 UDP endpoint；PID=%1").arg(udpRecord.processId);
        snapshot.udpEndpointRows.push_back(std::move(endpointRow));
    }
    snapshot.crossViewRows.reserve(crossMap.size());
    for (auto& pair : crossMap)
    {
        pair.second.row.tcpSummary = joinCompactLines(pair.second.tcpSummaryLines);
        pair.second.row.udpSummary = joinCompactLines(pair.second.udpSummaryLines);
        snapshot.crossViewRows.push_back(std::move(pair.second.row));
    }
    std::sort(snapshot.crossViewRows.begin(), snapshot.crossViewRows.end(), [](const CrossViewRow& left, const CrossViewRow& right)
    {
        if (left.processId != right.processId)
        {
            return left.processId < right.processId;
        }
        return left.processName < right.processName;
    });

    if (crossViewOnly)
    {
        return snapshot;
    }

    // AFD：先基于系统句柄做只读枚举，再筛选 AFD 相关对象。
    ks::file::HandleSnapshotOptions handleOptions;
    handleOptions.resolveObjectName = true;
    handleOptions.nameResolveBudget = 220;
    handleOptions.basicInfoQueryBudget = 220;
    handleOptions.enumMode = ks::file::HandleEnumMode::DuplicateHandle;
    const ks::file::HandleSnapshotResult handleSnapshot = ks::file::BuildHandleSnapshot(handleOptions);
    snapshot.afdRows.reserve(handleSnapshot.rows.size());
    for (const ks::file::HandleSnapshotRow& handleRow : handleSnapshot.rows)
    {
        if (handleRow.objectName.empty())
        {
            continue;
        }
        const QString objectNameText = QString::fromWCharArray(handleRow.objectName.c_str());
        if (!compareContainsAfd(objectNameText))
        {
            continue;
        }

        AfdHandleRow row;
        row.processId = handleRow.processId;
        row.processName = QString::fromWCharArray(handleRow.processName.c_str());
        row.handleValueText = QStringLiteral("0x%1").arg(QString::number(handleRow.handleValue, 16));
        row.typeName = QString::fromWCharArray(handleRow.typeName.c_str());
        row.objectName = objectNameText;
        row.sourceText = QStringLiteral("R3 Handle Snapshot");
        row.diffText = handleRow.diffStatus == ks::file::HandleDiffStatus::NotCompared ? QStringLiteral("未对比") : QStringLiteral("已对比");
        row.accessText = QStringLiteral("0x%1").arg(QString::number(handleRow.grantedAccess, 16));
        row.detailText = QStringLiteral("handleCount=%1 pointerCount=%2")
            .arg(handleRow.handleCount)
            .arg(handleRow.pointerCount);
        snapshot.afdRows.push_back(std::move(row));
    }

    // WFP：动态加载 fwpuclnt.dll，直接枚举 provider / sublayer / callout / filter。
    HANDLE wfpEngineHandle = nullptr;
    QString wfpErrorText;
    if (openWfpEngine(wfpEngineHandle, &wfpErrorText))
    {
        auto& api = wfpApi();
        auto enumProviders = [&snapshot, &api, wfpEngineHandle]()
        {
            HANDLE enumHandle = nullptr;
            FWPM_PROVIDER_ENUM_TEMPLATE0 enumTemplate{};
            if (api.providerCreateEnumHandle(wfpEngineHandle, &enumTemplate, &enumHandle) != ERROR_SUCCESS || enumHandle == nullptr)
            {
                return;
            }

            while (true)
            {
                FWPM_PROVIDER0** entries = nullptr;
                UINT32 count = 0;
                const DWORD status = api.providerEnum(wfpEngineHandle, enumHandle, 128, &entries, &count);
                if (status != ERROR_SUCCESS)
                {
                    break;
                }
                if (entries == nullptr || count == 0)
                {
                    if (entries != nullptr)
                    {
                        api.freeMemory(reinterpret_cast<void**>(&entries));
                    }
                    break;
                }

                for (UINT32 index = 0; index < count; ++index)
                {
                    const FWPM_PROVIDER0* provider = entries[index];
                    if (provider == nullptr)
                    {
                        continue;
                    }
                    WfpProviderRow row;
                    row.nameText = displayDataText(&provider->displayData);
                    row.descriptionText = provider->displayData.description != nullptr ? QString::fromWCharArray(provider->displayData.description) : QString();
                    row.guidText = guidToText(provider->providerKey);
                    row.flagsText = wfpFlagsText(provider->flags);
                    row.serviceNameText = provider->serviceName != nullptr ? QString::fromWCharArray(provider->serviceName) : QString();
                    row.dataSizeText = provider->providerData.size > 0 ? QString::number(provider->providerData.size) : QStringLiteral("0");
                    snapshot.wfpProviderRows.push_back(std::move(row));
                }
                api.freeMemory(reinterpret_cast<void**>(&entries));
                if (snapshot.wfpProviderRows.size() >= 256)
                {
                    break;
                }
            }

            api.providerDestroyEnumHandle(wfpEngineHandle, enumHandle);
        };

        auto enumSubLayers = [&snapshot, &api, wfpEngineHandle]()
        {
            HANDLE enumHandle = nullptr;
            FWPM_SUBLAYER_ENUM_TEMPLATE0 enumTemplate{};
            if (api.subLayerCreateEnumHandle(wfpEngineHandle, &enumTemplate, &enumHandle) != ERROR_SUCCESS || enumHandle == nullptr)
            {
                return;
            }

            while (true)
            {
                FWPM_SUBLAYER0** entries = nullptr;
                UINT32 count = 0;
                const DWORD status = api.subLayerEnum(wfpEngineHandle, enumHandle, 128, &entries, &count);
                if (status != ERROR_SUCCESS)
                {
                    break;
                }
                if (entries == nullptr || count == 0)
                {
                    if (entries != nullptr)
                    {
                        api.freeMemory(reinterpret_cast<void**>(&entries));
                    }
                    break;
                }

                for (UINT32 index = 0; index < count; ++index)
                {
                    const FWPM_SUBLAYER0* subLayer = entries[index];
                    if (subLayer == nullptr)
                    {
                        continue;
                    }
                    WfpSubLayerRow row;
                    row.nameText = displayDataText(&subLayer->displayData);
                    row.descriptionText = subLayer->displayData.description != nullptr ? QString::fromWCharArray(subLayer->displayData.description) : QString();
                    row.guidText = guidToText(subLayer->subLayerKey);
                    row.flagsText = wfpFlagsText(subLayer->flags);
                    row.providerGuidText = subLayer->providerKey != nullptr ? guidToText(*subLayer->providerKey) : QString();
                    row.weightText = QString::number(subLayer->weight);
                    snapshot.wfpSubLayerRows.push_back(std::move(row));
                }
                api.freeMemory(reinterpret_cast<void**>(&entries));
                if (snapshot.wfpSubLayerRows.size() >= 256)
                {
                    break;
                }
            }

            api.subLayerDestroyEnumHandle(wfpEngineHandle, enumHandle);
        };

        auto enumCallouts = [&snapshot, &api, wfpEngineHandle]()
        {
            HANDLE enumHandle = nullptr;
            FWPM_CALLOUT_ENUM_TEMPLATE0 enumTemplate{};
            if (api.calloutCreateEnumHandle(wfpEngineHandle, &enumTemplate, &enumHandle) != ERROR_SUCCESS || enumHandle == nullptr)
            {
                return;
            }

            while (true)
            {
                FWPM_CALLOUT0** entries = nullptr;
                UINT32 count = 0;
                const DWORD status = api.calloutEnum(wfpEngineHandle, enumHandle, 128, &entries, &count);
                if (status != ERROR_SUCCESS)
                {
                    break;
                }
                if (entries == nullptr || count == 0)
                {
                    if (entries != nullptr)
                    {
                        api.freeMemory(reinterpret_cast<void**>(&entries));
                    }
                    break;
                }

                for (UINT32 index = 0; index < count; ++index)
                {
                    const FWPM_CALLOUT0* callout = entries[index];
                    if (callout == nullptr)
                    {
                        continue;
                    }
                    WfpCalloutRow row;
                    row.nameText = displayDataText(&callout->displayData);
                    row.descriptionText = callout->displayData.description != nullptr ? QString::fromWCharArray(callout->displayData.description) : QString();
                    row.guidText = guidToText(callout->calloutKey);
                    row.flagsText = wfpFlagsText(callout->flags);
                    row.providerGuidText = callout->providerKey != nullptr ? guidToText(*callout->providerKey) : QString();
                    row.layerGuidText = guidToText(callout->applicableLayer);
                    row.calloutIdText = QString::number(callout->calloutId);
                    snapshot.wfpCalloutRows.push_back(std::move(row));
                }
                api.freeMemory(reinterpret_cast<void**>(&entries));
                if (snapshot.wfpCalloutRows.size() >= 256)
                {
                    break;
                }
            }

            api.calloutDestroyEnumHandle(wfpEngineHandle, enumHandle);
        };

        auto enumFilters = [&snapshot, &api, wfpEngineHandle]()
        {
            HANDLE enumHandle = nullptr;
            FWPM_FILTER_ENUM_TEMPLATE0 enumTemplate{};
            if (api.filterCreateEnumHandle(wfpEngineHandle, &enumTemplate, &enumHandle) != ERROR_SUCCESS || enumHandle == nullptr)
            {
                return;
            }

            while (true)
            {
                FWPM_FILTER0** entries = nullptr;
                UINT32 count = 0;
                const DWORD status = api.filterEnum(wfpEngineHandle, enumHandle, 128, &entries, &count);
                if (status != ERROR_SUCCESS)
                {
                    break;
                }
                if (entries == nullptr || count == 0)
                {
                    if (entries != nullptr)
                    {
                        api.freeMemory(reinterpret_cast<void**>(&entries));
                    }
                    break;
                }

                for (UINT32 index = 0; index < count; ++index)
                {
                    const FWPM_FILTER0* filter = entries[index];
                    if (filter == nullptr)
                    {
                        continue;
                    }
                    WfpFilterRow row;
                    row.nameText = displayDataText(&filter->displayData);
                    row.descriptionText = filter->displayData.description != nullptr ? QString::fromWCharArray(filter->displayData.description) : QString();
                    row.guidText = guidToText(filter->filterKey);
                    row.flagsText = wfpFlagsText(filter->flags);
                    row.providerGuidText = filter->providerKey != nullptr ? guidToText(*filter->providerKey) : QString();
                    row.layerGuidText = guidToText(filter->layerKey);
                    row.subLayerGuidText = guidToText(filter->subLayerKey);
                    row.weightText = QStringLiteral("type=%1").arg(static_cast<int>(filter->weight.type));
                    row.actionText = QStringLiteral("type=%1").arg(static_cast<int>(filter->action.type));
                    row.conditionText = QStringLiteral("conditions=%1").arg(filter->numFilterConditions);
                    row.filterIdText = QString::number(filter->filterId);
                    snapshot.wfpFilterRows.push_back(std::move(row));
                }
                api.freeMemory(reinterpret_cast<void**>(&entries));
                if (snapshot.wfpFilterRows.size() >= 400)
                {
                    break;
                }
            }

            api.filterDestroyEnumHandle(wfpEngineHandle, enumHandle);
        };

        enumProviders();
        enumSubLayers();
        enumCallouts();
        enumFilters();
        api.engineClose(wfpEngineHandle);
    }
    else
    {
        snapshot.wfpProviderRows.push_back({ QStringLiteral("WFP"), wfpErrorText, QString(), QString(), QString(), QString() });
    }

    QString ndisScript = QStringLiteral(
        "$ErrorActionPreference='SilentlyContinue'; "
        "$adapters = Get-NetAdapter | Select-Object -First 200 Name,InterfaceDescription,ifIndex,Status,MacAddress,LinkSpeed; "
        "$bindings = Get-NetAdapterBinding | Select-Object -First 200 Name,DisplayName,ComponentID,Enabled,InstanceID; "
        "$ifaces = Get-NetIPInterface | Select-Object -First 200 InterfaceAlias,ifIndex,AddressFamily,ConnectionState,InterfaceMetric,NlMtu; "
        "[pscustomobject]@{ adapters=$adapters; bindings=$bindings; ifaces=$ifaces } | ConvertTo-Json -Depth 4 -Compress");
    QString ndisErrorText;
    QString ndisJson = runPowerShellTextSync(ndisScript, 12000, &ndisErrorText);
    QJsonParseError parseError{};
    const QJsonDocument ndisDoc = QJsonDocument::fromJson(ndisJson.toUtf8(), &parseError);
    if (parseError.error == QJsonParseError::NoError && ndisDoc.isObject())
    {
        const QJsonArray adaptersArray = normalizeJsonArray(ndisDoc.object().value(QStringLiteral("adapters")));
        for (const QJsonValue& value : adaptersArray)
        {
            const QJsonObject object = value.toObject();
            NdisAdapterRow row;
            row.nameText = object.value(QStringLiteral("Name")).toString();
            row.descriptionText = object.value(QStringLiteral("InterfaceDescription")).toString();
            row.ifIndexText = object.value(QStringLiteral("ifIndex")).toVariant().toString();
            row.statusText = object.value(QStringLiteral("Status")).toString();
            row.macText = object.value(QStringLiteral("MacAddress")).toString();
            row.linkSpeedText = object.value(QStringLiteral("LinkSpeed")).toString();
            row.connectionStateText = QStringLiteral("已枚举");
            snapshot.ndisAdapterRows.push_back(std::move(row));
        }

        const QJsonArray bindingsArray = normalizeJsonArray(ndisDoc.object().value(QStringLiteral("bindings")));
        for (const QJsonValue& value : bindingsArray)
        {
            const QJsonObject object = value.toObject();
            NdisBindingRow row;
            row.adapterNameText = object.value(QStringLiteral("Name")).toString();
            row.displayNameText = object.value(QStringLiteral("DisplayName")).toString();
            row.componentIdText = object.value(QStringLiteral("ComponentID")).toString();
            row.enabledText = object.value(QStringLiteral("Enabled")).toVariant().toString();
            row.instanceIdText = object.value(QStringLiteral("InstanceID")).toString();
            snapshot.ndisBindingRows.push_back(std::move(row));
        }

        const QJsonArray ifacesArray = normalizeJsonArray(ndisDoc.object().value(QStringLiteral("ifaces")));
        for (const QJsonValue& value : ifacesArray)
        {
            const QJsonObject object = value.toObject();
            NdisProtocolRow row;
            row.interfaceAliasText = object.value(QStringLiteral("InterfaceAlias")).toString();
            row.ifIndexText = object.value(QStringLiteral("ifIndex")).toVariant().toString();
            row.addressFamilyText = object.value(QStringLiteral("AddressFamily")).toString();
            row.connectionStateText = object.value(QStringLiteral("ConnectionState")).toString();
            row.interfaceMetricText = object.value(QStringLiteral("InterfaceMetric")).toVariant().toString();
            row.mtuText = object.value(QStringLiteral("NlMtu")).toVariant().toString();
            snapshot.ndisProtocolRows.push_back(std::move(row));
        }
    }
    else
    {
        NdisAdapterRow row;
        row.nameText = QStringLiteral("NDIS");
        row.descriptionText = ndisErrorText;
        snapshot.ndisAdapterRows.push_back(std::move(row));
    }

    // R0 网络审计：
    // - 输入：ArkDriverClient 四个只读 wrapper；
    // - 处理：先采集 ok/unsupported/count/truncated/message 摘要，再把 R0 明细追加到原有表；
    // - 返回：写入 snapshot.r0SummaryRows 与各明细行集合，供 UI 追加展示。
    {
        const ksword::ark::DriverClient driverClient;
        const auto tcpR0 = driverClient.queryNetworkTcpEndpoints();
        const auto udpR0 = driverClient.queryNetworkUdpEndpoints();
        const auto wfpR0 = driverClient.queryNetworkWfpInventory();
        const auto ndisR0 = driverClient.queryNetworkNdisChain();
        snapshot.r0TcpStatusText = r0AuditStatusText(tcpR0);
        snapshot.r0UdpStatusText = r0AuditStatusText(udpR0);

        auto appendR0Summary = [&snapshot](const QString& nameText, const auto& result)
        {
            R0NetworkSummaryRow row;
            row.nameText = nameText;
            row.statusText = r0AuditStatusText(result);
            // returned/total 来自 R0 响应头，parsed 用于提示 ArkDriverClient 实际解析出的行数。
            // 这三个数字并列展示，便于区分“驱动返回数量”和“用户态解析数量”。
            row.countText = QStringLiteral("已解析 %3 行；驱动报告 %1/%2 行")
                .arg(result.returnedCount)
                .arg(result.totalCount)
                .arg(static_cast<qulonglong>(result.entries.size()));
            if (result.partial)
            {
                if (result.status == KSWORD_ARK_NETWORK_STATUS_APPLIED &&
                    result.totalCount > result.returnedCount)
                {
                    row.truncatedText =
                        QStringLiteral("是，达到返回预算（仅展示驱动返回子集）");
                }
                else
                {
                    row.truncatedText =
                        static_cast<std::uint32_t>(result.lastStatus) == 0x80000005UL
                        ? QStringLiteral("是，部分结果（达到遍历或缓冲上限，合法行已保留）")
                        : QStringLiteral("部分结果（部分枚举失败，合法行已保留）");
                }
            }
            else
            {
                row.truncatedText =
                    r0AuditTruncatedText(result) == QStringLiteral("true")
                    ? QStringLiteral("是，结果可能未完整返回")
                    : QStringLiteral("否");
            }
            row.messageText = QStringLiteral("%1；protocolStatus=%2；lastStatus=%3；sourceFlags=%4；generation=%5；partial=%6；truncated=%7；retainedRows=%8")
                .arg(ioMessageToText(result.io.message))
                .arg(result.status)
                .arg(r0Hex32(static_cast<std::uint32_t>(result.lastStatus)))
                .arg(r0Hex32(result.sourceFlags))
                .arg(result.generation)
                .arg(result.partial ? QStringLiteral("true") : QStringLiteral("false"))
                .arg(result.truncated ? QStringLiteral("true") : QStringLiteral("false"))
                .arg(static_cast<qulonglong>(result.entries.size()));
            snapshot.r0SummaryRows.push_back(std::move(row));
        };

        appendR0Summary(QStringLiteral("R0 TCP"), tcpR0);
        appendR0Summary(QStringLiteral("R0 UDP"), udpR0);
        appendR0Summary(QStringLiteral("R0 WFP"), wfpR0);
        appendR0Summary(QStringLiteral("R0 NDIS"), ndisR0);

        // R0 明细展开：
        // - 输入：四个 ArkDriverClient wrapper 的 entries；
        // - 处理：把 endpoint/WFP/NDIS 行追加到既有明细表，不替换 R3 枚举结果；
        // - 返回：无，所有文本仅用于只读审计展示。
        auto appendR0Endpoints = [&snapshot](const ksword::ark::NetworkEndpointAuditResult& result, const bool tcpRows)
        {
            const QString completenessText = result.partial || result.truncated
                ? QStringLiteral("部分/截断子集")
                : QStringLiteral("完整结果");
            for (const KSWORD_ARK_NETWORK_ENDPOINT_ROW& entry : result.entries)
            {
                const QString detailText = QStringLiteral("来源=R0 endpoint；快照=%1；rowId=%2；protocol=%3；AF=%4；compartment=%5；ifIndex=%6；flags=%7；sourceFlags=%8；fieldMask=%9；endpointObject=%10；owningProcessObject=%11；transportObject=%12；interfaceLuid=%13")
                    .arg(completenessText)
                    .arg(entry.rowId)
                    .arg(entry.protocol)
                    .arg(entry.addressFamily)
                    .arg(entry.compartmentId)
                    .arg(entry.interfaceIndex)
                    .arg(r0Hex32(entry.flags))
                    .arg(r0Hex32(entry.sourceFlags))
                    .arg(r0Hex32(entry.fieldMask))
                    .arg(r0Hex64(entry.endpointObject))
                    .arg(r0Hex64(entry.owningProcessObject))
                    .arg(r0Hex64(entry.transportObject))
                    .arg(r0Hex64(entry.interfaceLuid));

                if (tcpRows)
                {
                    TcpEndpointRow row;
                    row.processId = entry.owningPid;
                    row.processName = QStringLiteral("R0 PID %1").arg(entry.owningPid);
                    row.localEndpointText = r0EndpointText(entry.addressFamily, entry.localAddress, entry.localPort);
                    row.remoteEndpointText = r0EndpointText(entry.addressFamily, entry.remoteAddress, entry.remotePort);
                    row.stateText = r0TcpStateText(entry.state);
                    row.detailText = detailText;
                    row.isR0Snapshot = true;
                    snapshot.tcpEndpointRows.push_back(std::move(row));
                }
                else
                {
                    UdpEndpointRow row;
                    row.processId = entry.owningPid;
                    row.processName = QStringLiteral("R0 PID %1").arg(entry.owningPid);
                    row.localEndpointText = r0EndpointText(entry.addressFamily, entry.localAddress, entry.localPort);
                    row.sourceText = QStringLiteral("R0 UDP endpoint");
                    row.detailText = detailText;
                    snapshot.udpEndpointRows.push_back(std::move(row));
                }
            }
        };

        auto appendR0WfpRows = [&snapshot](const ksword::ark::NetworkWfpInventoryResult& result)
        {
            const QString completenessText = result.partial || result.truncated
                ? QStringLiteral("部分/截断子集")
                : QStringLiteral("完整结果");
            for (const KSWORD_ARK_NETWORK_WFP_INVENTORY_ROW& entry : result.entries)
            {
                const QString ownerModuleText = fixedNetworkWideText(entry.ownerModule, KSWORD_ARK_NETWORK_NAME_CHARS, QStringLiteral("<owner unknown>"));
                const QString detailText = QStringLiteral("来源=R0 WFP；快照=%1；kind=%2；rowId=%3；flags=%4；fieldMask=%5；layerId=%6；calloutId=%7；object=%8；classify=%9；notify=%10；flowDelete=%11；ownerBase=%12；ownerModule=%13")
                    .arg(completenessText)
                    .arg(r0WfpObjectKindText(entry.objectKind))
                    .arg(entry.rowId)
                    .arg(r0Hex32(entry.flags))
                    .arg(r0Hex32(entry.fieldMask))
                    .arg(entry.layerId)
                    .arg(entry.calloutId)
                    .arg(r0Hex64(entry.objectAddress))
                    .arg(r0Hex64(entry.classifyAddress))
                    .arg(r0Hex64(entry.notifyAddress))
                    .arg(r0Hex64(entry.flowDeleteAddress))
                    .arg(r0Hex64(entry.ownerImageBase))
                    .arg(ownerModuleText);

                if (entry.objectKind == KSWORD_ARK_NETWORK_WFP_OBJECT_PROVIDER)
                {
                    WfpProviderRow row;
                    row.nameText = QStringLiteral("R0 Provider #%1").arg(entry.rowId);
                    row.descriptionText = detailText;
                    row.guidText = r0GuidText(entry.objectKey);
                    row.flagsText = r0Hex32(entry.flags);
                    row.serviceNameText = ownerModuleText;
                    row.dataSizeText = QStringLiteral("fieldMask=%1").arg(r0Hex32(entry.fieldMask));
                    snapshot.wfpProviderRows.push_back(std::move(row));
                }
                else if (entry.objectKind == KSWORD_ARK_NETWORK_WFP_OBJECT_SUBLAYER)
                {
                    WfpSubLayerRow row;
                    row.nameText = QStringLiteral("R0 Sublayer #%1").arg(entry.rowId);
                    row.descriptionText = detailText;
                    row.guidText = r0GuidText(entry.objectKey);
                    row.flagsText = r0Hex32(entry.flags);
                    row.providerGuidText = r0GuidText(entry.providerKey);
                    row.weightText = QString::number(entry.weight);
                    snapshot.wfpSubLayerRows.push_back(std::move(row));
                }
                else if (entry.objectKind == KSWORD_ARK_NETWORK_WFP_OBJECT_CALLOUT)
                {
                    WfpCalloutRow row;
                    row.nameText = QStringLiteral("R0 Callout #%1").arg(entry.rowId);
                    row.descriptionText = detailText;
                    row.guidText = r0GuidText(entry.objectKey);
                    row.flagsText = r0Hex32(entry.flags);
                    row.providerGuidText = r0GuidText(entry.providerKey);
                    row.layerGuidText = QStringLiteral("layerId=%1").arg(entry.layerId);
                    row.calloutIdText = QString::number(entry.calloutId);
                    snapshot.wfpCalloutRows.push_back(std::move(row));
                }
                else
                {
                    WfpFilterRow row;
                    row.nameText = QStringLiteral("R0 Filter #%1").arg(entry.rowId);
                    row.descriptionText = detailText;
                    row.guidText = r0GuidText(entry.objectKey);
                    row.flagsText = r0Hex32(entry.flags);
                    row.providerGuidText = r0GuidText(entry.providerKey);
                    row.layerGuidText = QStringLiteral("layerId=%1").arg(entry.layerId);
                    row.subLayerGuidText = r0GuidText(entry.subLayerKey);
                    row.weightText = QString::number(entry.weight);
                    row.actionText = QStringLiteral("calloutId=%1").arg(entry.calloutId);
                    row.conditionText = detailText;
                    row.filterIdText = QString::number(entry.filterId);
                    snapshot.wfpFilterRows.push_back(std::move(row));
                }
            }
        };

        auto appendR0NdisRows = [&snapshot](const ksword::ark::NetworkNdisChainResult& result)
        {
            const QString completenessText = result.partial || result.truncated
                ? QStringLiteral("部分/截断子集")
                : QStringLiteral("完整结果");
            for (const KSWORD_ARK_NETWORK_NDIS_CHAIN_ROW& entry : result.entries)
            {
                const QString componentText = fixedNetworkWideText(entry.componentName, KSWORD_ARK_NETWORK_NAME_CHARS, QStringLiteral("<component unknown>"));
                const QString ownerModuleText = fixedNetworkWideText(entry.ownerModule, KSWORD_ARK_NETWORK_NAME_CHARS, QStringLiteral("<owner unknown>"));
                const QString kindText = r0NdisObjectKindText(entry.objectKind);
                const QString detailText = QStringLiteral("来源=R0 NDIS；快照=%1；kind=%2；rowId=%3；flags=%4；fieldMask=%5；ifIndex=%6；filterOrder=%7；adapterLuid=%8；object=%9；parent=%10；driverObject=%11；imageBase=%12；ownerModule=%13")
                    .arg(completenessText)
                    .arg(kindText)
                    .arg(entry.rowId)
                    .arg(r0Hex32(entry.flags))
                    .arg(r0Hex32(entry.fieldMask))
                    .arg(entry.ifIndex)
                    .arg(entry.filterOrder)
                    .arg(r0Hex64(entry.adapterLuid))
                    .arg(r0Hex64(entry.objectAddress))
                    .arg(r0Hex64(entry.parentObjectAddress))
                    .arg(r0Hex64(entry.driverObject))
                    .arg(r0Hex64(entry.imageBase))
                    .arg(ownerModuleText);

                if (entry.objectKind == KSWORD_ARK_NETWORK_NDIS_OBJECT_MINIPORT)
                {
                    NdisAdapterRow row;
                    row.nameText = componentText;
                    row.descriptionText = detailText;
                    row.ifIndexText = QString::number(entry.ifIndex);
                    row.statusText = kindText;
                    row.macText = QStringLiteral("R0");
                    row.linkSpeedText = QStringLiteral("object=%1").arg(r0Hex64(entry.objectAddress));
                    row.connectionStateText = ownerModuleText;
                    snapshot.ndisAdapterRows.push_back(std::move(row));
                }
                else if (entry.objectKind == KSWORD_ARK_NETWORK_NDIS_OBJECT_PROTOCOL)
                {
                    NdisProtocolRow row;
                    row.interfaceAliasText = componentText;
                    row.ifIndexText = QString::number(entry.ifIndex);
                    row.addressFamilyText = kindText;
                    row.connectionStateText = ownerModuleText;
                    row.interfaceMetricText = QStringLiteral("flags=%1").arg(r0Hex32(entry.flags));
                    row.mtuText = detailText;
                    snapshot.ndisProtocolRows.push_back(std::move(row));
                }
                else if (
                    entry.objectKind == KSWORD_ARK_NETWORK_NDIS_OBJECT_FILTER ||
                    entry.objectKind == KSWORD_ARK_NETWORK_NDIS_OBJECT_BINDING)
                {
                    NdisBindingRow row;
                    row.adapterNameText = componentText;
                    row.displayNameText = kindText;
                    row.componentIdText = ownerModuleText;
                    row.enabledText = QStringLiteral("flags=%1").arg(r0Hex32(entry.flags));
                    row.instanceIdText = detailText;
                    snapshot.ndisBindingRows.push_back(std::move(row));
                }
                else
                {
                    NdisUnknownRow row;
                    row.kindText = QStringLiteral("未知/未证明（kind=%1）")
                        .arg(entry.objectKind);
                    row.componentText = componentText;
                    row.ownerModuleText = ownerModuleText;
                    row.objectAddressText = r0Hex64(entry.objectAddress);
                    row.detailText = detailText;
                    snapshot.ndisUnknownRows.push_back(std::move(row));
                }
            }
        };

        appendR0Endpoints(tcpR0, true);
        appendR0Endpoints(udpR0, false);
        appendR0WfpRows(wfpR0);
        appendR0NdisRows(ndisR0);
    }

    snapshot.nsiSummaryRows = {
        { QStringLiteral("TCP 条目"), QString::number(tcpRecords.size()) },
        { QStringLiteral("UDP 端点"), QString::number(udpRecords.size()) },
        { QStringLiteral("AFD 候选句柄"), QString::number(snapshot.afdRows.size()) },
        { QStringLiteral("WFP Provider"), QString::number(snapshot.wfpProviderRows.size()) },
        { QStringLiteral("NDIS Adapter"), QString::number(snapshot.ndisAdapterRows.size()) }
    };

    snapshot.statusText = QStringLiteral("完成：TCP=%1, UDP=%2, AFD=%3, WFP=%4, NDIS=%5")
        .arg(tcpRecords.size())
        .arg(udpRecords.size())
        .arg(snapshot.afdRows.size())
        .arg(snapshot.wfpProviderRows.size())
        .arg(snapshot.ndisAdapterRows.size());
    snapshot.detailText = QStringLiteral("TCP:%1 | UDP:%2")
        .arg(tcpOk ? QStringLiteral("ok") : QString::fromUtf8(tcpError.c_str()))
        .arg(udpOk ? QStringLiteral("ok") : QString::fromUtf8(udpError.c_str()));
    for (const R0NetworkSummaryRow& row : snapshot.r0SummaryRows)
    {
        snapshot.detailText += QStringLiteral(" | %1：%2，%3，截断：%4，说明：%5")
            .arg(row.nameText)
            .arg(row.statusText)
            .arg(row.countText)
            .arg(row.truncatedText)
            .arg(row.messageText);
    }

    return snapshot;
}

void NetworkAuditPage::applySnapshot(std::shared_ptr<AuditSnapshot> snapshot,
    std::shared_ptr<const CrossViewPresentation> prepared)
{
    // 规范快照始终接收完整采集结果；隐藏与菜单屏障只延后控件投影。
    m_latestAuditSnapshot = network_value_detail::retainMutable(std::move(snapshot));
    ++m_crossSourceGeneration;
    if (prepared && prepared->source == m_latestAuditSnapshot
        && prepared->sourceGeneration == m_crossSourceGeneration
        && prepared->languageGeneration == m_crossLanguageGeneration)
    {
        m_preparedCrossView = std::move(prepared);
        m_crossPrepareRequestedSource = m_latestAuditSnapshot;
        m_crossPrepareRequestedLanguage = m_crossLanguageGeneration;
        m_crossPreparation->cancel(); // Retire any obsolete language-only preparation.
    }
    else requestCrossViewPreparation(); // Language changed in flight, or preparation threw.
    m_dirtyAuditTables = {
        m_tcpTable, m_udpTable, m_crossSummaryTable, m_afdTable,
        m_wfpProviderTable, m_wfpSubLayerTable, m_wfpCalloutTable, m_wfpFilterTable,
        m_ndisAdapterTable, m_ndisBindingTable, m_ndisProtocolTable, m_ndisUnknownTable,
        m_nsiSummaryTable
    };
    const QPointer<NetworkAuditPage> guard(this);
    const auto accepted = m_latestAuditSnapshot;
    const QPointer<QLabel> statusLabel(m_statusLabel);
    if (statusLabel != nullptr)
    {
        statusLabel->setText(accepted->statusText);
        if (guard.isNull()) return;
        if (m_latestAuditSnapshot != accepted)
        {
            presentVisibleSnapshot();
            return;
        }
        if (statusLabel != nullptr) statusLabel->setToolTip(accepted->detailText);
        if (guard.isNull()) return;
    }
    presentVisibleSnapshot();
}

void NetworkAuditPage::applyCrossViewSnapshot(std::shared_ptr<AuditSnapshot> snapshot,
    std::shared_ptr<const CrossViewPresentation> prepared)
{
    // The existing single-flight worker already merged the request's immutable
    // previous canonical value; GUI acceptance does not copy endpoint/text arrays.
    m_latestAuditSnapshot = network_value_detail::retainMutable(std::move(snapshot));
    ++m_crossSourceGeneration;
    if (prepared && prepared->source == m_latestAuditSnapshot
        && prepared->sourceGeneration == m_crossSourceGeneration
        && prepared->languageGeneration == m_crossLanguageGeneration)
    {
        m_preparedCrossView = std::move(prepared);
        m_crossPrepareRequestedSource = m_latestAuditSnapshot;
        m_crossPrepareRequestedLanguage = m_crossLanguageGeneration;
        m_crossPreparation->cancel(); // Retire any obsolete language-only preparation.
    }
    else requestCrossViewPreparation(); // Language changed in flight, or preparation threw.
    m_dirtyAuditTables.insert(m_tcpTable);
    m_dirtyAuditTables.insert(m_udpTable);
    m_dirtyAuditTables.insert(m_crossSummaryTable);
    presentVisibleSnapshot();
}

void NetworkAuditPage::presentVisibleSnapshot()
{
    if (m_projectionInProgress)
    {
        m_projectionRequestedWhileBusy = true;
        return;
    }
    if (m_constructingAuditPage)
    {
        m_projectionRequestedWhileBusy = true;
        return;
    }
    if (!isVisible() || m_sectionTabWidget == nullptr) return;
    if (!ensureCurrentAuditPage()) return;
    if (!isVisible() || m_sectionTabWidget == nullptr || m_latestAuditSnapshot == nullptr) return;

    QTableView* table = nullptr;
    QWidget* const section = m_sectionTabWidget->currentWidget();
    if (section == m_crossViewPage)
    {
        table = m_tcpTable; // 三表共享动作行缓存，始终在同一个提交中替换。
    }
    else if (section == m_afdPage)
    {
        table = m_afdTable;
    }
    else if (section == m_wfpPage && m_wfpTabWidget != nullptr)
    {
        switch (m_wfpTabWidget->currentIndex())
        {
        case 0: table = m_wfpProviderTable; break;
        case 1: table = m_wfpSubLayerTable; break;
        case 2: table = m_wfpCalloutTable; break;
        case 3: table = m_wfpFilterTable; break;
        default: break;
        }
    }
    else if (section == m_ndisPage && m_ndisTabWidget != nullptr)
    {
        switch (m_ndisTabWidget->currentIndex())
        {
        case 0: table = m_ndisAdapterTable; break;
        case 1: table = m_ndisBindingTable; break;
        case 2: table = m_ndisProtocolTable; break;
        case 3: table = m_ndisUnknownTable; break;
        default: break;
        }
    }
    else if (section == m_nsiPage)
    {
        table = m_nsiSummaryTable;
    }
    if (table == nullptr || !m_dirtyAuditTables.contains(table))
    {
        if (section == m_crossViewPage && m_crossFilterDirty) applyCrossViewSearchFilter();
        return;
    }

    if (section == m_crossViewPage
        && (!m_preparedCrossView || m_preparedCrossView->source != m_latestAuditSnapshot
            || m_preparedCrossView->sourceGeneration != m_crossSourceGeneration
            || m_preparedCrossView->languageGeneration != m_crossLanguageGeneration))
    {
        requestCrossViewPreparation();
        return; // Keep the complete displayed generation until the current value is ready.
    }

    // 共用一个 latest-wins 投影键；回调不保存过期快照，解除屏障后重新验证可见分支。
    const QPointer<NetworkAuditPage> safeThis(this);
    const auto deferProjection = section == m_crossViewPage
        ? ks::ui::DeferTablePresentationIfValueCaptureOpen : ks::ui::DeferTableUiCommitIfContextMenuOpen;
    if (deferProjection(
        this,
        QStringLiteral("network-audit-visible-snapshot"),
        { m_tcpTable, m_udpTable, m_crossSummaryTable, m_afdTable,
          m_wfpProviderTable, m_wfpSubLayerTable, m_wfpCalloutTable, m_wfpFilterTable,
          m_ndisAdapterTable, m_ndisBindingTable, m_ndisProtocolTable, m_ndisUnknownTable,
          m_nsiSummaryTable },
        [safeThis]()
        {
            if (!safeThis.isNull())
            {
                safeThis->presentVisibleSnapshot();
            }
        }))
    {
        return;
    }

    m_projectionInProgress = true;
    m_projectionRequestedWhileBusy = false;
    const auto snapshot = m_latestAuditSnapshot;
    const QPointer<QWidget> safeSection(section);
    m_dirtyAuditTables.remove(table);
    if (section == m_crossViewPage)
    {
        m_dirtyAuditTables.remove(m_udpTable);
        m_dirtyAuditTables.remove(m_crossSummaryTable);
        refreshCrossViewTable(m_preparedCrossView);
    }
    else if (section == m_afdPage)
    {
        refreshAfdTable(snapshot);
    }
    else if (section == m_wfpPage)
    {
        refreshWfpTables(snapshot, table);
    }
    else if (section == m_ndisPage)
    {
        refreshNdisTables(snapshot, table);
    }
    else if (section == m_nsiPage)
    {
        refreshNsiSummaryTable(*snapshot);
    }
    if (safeThis.isNull()) return;
    if (safeSection != nullptr && safeThis->isVisible()
        && safeThis->m_sectionTabWidget->currentWidget() == safeSection)
    {
        ks::ui::PrepareWidgetThemeBindingsForShow(safeSection);
        if (safeThis.isNull()) return;
    }
    safeThis->m_projectionInProgress = false;
    if (safeThis->m_projectionRequestedWhileBusy || safeThis->m_latestAuditSnapshot != snapshot)
    {
        QTimer::singleShot(0, safeThis, [safeThis]()
        {
            if (!safeThis.isNull()) safeThis->presentVisibleSnapshot();
        });
    }
}

std::shared_ptr<const NetworkAuditPage::CrossViewPresentation> NetworkAuditPage::prepareCrossViewValues(
    const std::shared_ptr<const AuditSnapshot>& source,
    const ks::i18n::SourceTextSnapshot& language,
    const std::uint64_t sourceGeneration, const std::uint64_t languageGeneration,
    const QString& r0TcpStatusText, const QString& r0UdpStatusText,
    const std::function<bool()>& isCanceled)
{
    if (!source || isCanceled()) return {};
    const auto& tcpEndpointRows = source->tcpEndpointRows;
    const auto& udpEndpointRows = source->udpEndpointRows;
    const auto prepareRow = [&language](CrossViewValueRow value, const QStringList& searchDetails) -> CrossViewValue {
        value.searchFields.reserve(value.fields.size() + searchDetails.size());
        for (const auto& field : value.fields)
            value.searchFields.push_back(language.sourceText(field).toCaseFolded());
        // 删除冗余展示列后，原明细/摘要仍可由现有搜索入口检索。
        for (const auto& field : searchDetails)
            value.searchFields.push_back(language.sourceText(field).toCaseFolded());
        return std::make_shared<const CrossViewValueRow>(std::move(value));
    };
    // Cross-View 必须按来源重新聚合当前缓存。完整刷新提供 R0 行，高频刷新替换
    // R3 行并保留最近一次 R0 行；在这里重算可以避免自动刷新后汇总表退化成 R3-only。
    struct CrossViewAggregate
    {
        CrossViewRow row;
        std::size_t tcpR0Count = 0U;
        std::size_t udpR0Count = 0U;
        QString tcpR0Summary;
        QString udpR0Summary;
        QStringList tcpR3SummaryLines;
        QStringList udpR3SummaryLines;
        QStringList tcpR0SummaryLines;
        QStringList udpR0SummaryLines;
    };
    std::unordered_map<std::uint32_t, CrossViewAggregate> crossMap;
    for (const TcpEndpointRow& endpointRow : tcpEndpointRows)
    {
        if (isCanceled()) return {};
        CrossViewAggregate& aggregate = crossMap[endpointRow.processId];
        aggregate.row.processId = endpointRow.processId;
        if (!endpointRow.isR0Snapshot || aggregate.row.processName.isEmpty())
        {
            aggregate.row.processName = endpointRow.processName;
        }
        const QString endpointSummary = QStringLiteral("%1 -> %2 (%3)")
            .arg(endpointRow.localEndpointText)
            .arg(endpointRow.remoteEndpointText)
            .arg(endpointRow.stateText);
        if (endpointRow.isR0Snapshot)
        {
            ++aggregate.tcpR0Count;
            aggregate.tcpR0SummaryLines.push_back(endpointSummary);
        }
        else
        {
            ++aggregate.row.tcpCount;
            aggregate.tcpR3SummaryLines.push_back(endpointSummary);
        }
    }
    for (const UdpEndpointRow& endpointRow : udpEndpointRows)
    {
        if (isCanceled()) return {};
        CrossViewAggregate& aggregate = crossMap[endpointRow.processId];
        aggregate.row.processId = endpointRow.processId;
        const bool isR0Snapshot = endpointRow.sourceText.startsWith(QStringLiteral("R0"));
        if (!isR0Snapshot || aggregate.row.processName.isEmpty())
        {
            aggregate.row.processName = endpointRow.processName;
        }
        if (isR0Snapshot)
        {
            ++aggregate.udpR0Count;
            aggregate.udpR0SummaryLines.push_back(endpointRow.localEndpointText);
        }
        else
        {
            ++aggregate.row.udpCount;
            aggregate.udpR3SummaryLines.push_back(endpointRow.localEndpointText);
        }
    }
    std::vector<CrossViewAggregate> effectiveCrossRows;
    effectiveCrossRows.reserve(crossMap.size());
    for (auto& pair : crossMap)
    {
        if (isCanceled()) return {};
        pair.second.row.tcpSummary = joinCompactLines(pair.second.tcpR3SummaryLines);
        pair.second.row.udpSummary = joinCompactLines(pair.second.udpR3SummaryLines);
        pair.second.tcpR0Summary = joinCompactLines(pair.second.tcpR0SummaryLines);
        pair.second.udpR0Summary = joinCompactLines(pair.second.udpR0SummaryLines);
        effectiveCrossRows.push_back(std::move(pair.second));
    }
    std::sort(effectiveCrossRows.begin(), effectiveCrossRows.end(), [](const CrossViewAggregate& left, const CrossViewAggregate& right)
    {
        if (left.row.processId != right.row.processId)
        {
            return left.row.processId < right.row.processId;
        }
        return left.row.processName < right.row.processName;
    });

    std::vector<CrossViewValue> tcpRows;
    tcpRows.reserve(tcpEndpointRows.size());
    for (std::size_t index = 0; index < tcpEndpointRows.size(); ++index)
    {
        if (isCanceled()) return {};
        const auto& row = tcpEndpointRows[index];
        CrossViewValueRow value;
        value.processId = row.processId;
        value.sourceIndex = static_cast<qulonglong>(index);
        value.fields = {QString::number(row.processId), row.processName, row.localEndpointText,
            row.remoteEndpointText, row.stateText};
        QStringList keyFields{QString::number(row.processId), QString::number(row.isR0Snapshot),
            row.localEndpointText, row.remoteEndpointText};
        if (row.isR0Snapshot) keyFields.push_back(row.detailText);
        value.identity = crossViewIdentity('T', keyFields);
        tcpRows.push_back(prepareRow(std::move(value), {row.detailText}));
    }
    std::vector<CrossViewValue> udpRows;
    udpRows.reserve(udpEndpointRows.size());
    for (const auto& row : udpEndpointRows)
    {
        if (isCanceled()) return {};
        CrossViewValueRow value;
        value.processId = row.processId;
        value.fields = {QString::number(row.processId), row.processName, row.localEndpointText,
            row.sourceText};
        value.identity = crossViewIdentity('U', {QString::number(row.processId), row.localEndpointText,
            row.sourceText, row.detailText});
        udpRows.push_back(prepareRow(std::move(value), {row.detailText}));
    }
    std::vector<CrossViewValue> summaryRows;
    summaryRows.reserve(effectiveCrossRows.size());
    const auto r0CountText = [](const std::size_t count, const QString& statusText) {
        if (statusText == QStringLiteral("ok")) return QString::number(static_cast<qulonglong>(count));
        if (count != 0U) return QStringLiteral("%1:%2")
            .arg(statusText.isEmpty() ? QStringLiteral("?") : statusText).arg(static_cast<qulonglong>(count));
        return statusText.isEmpty() ? QStringLiteral("?") : statusText;
    };
    const auto countText = [&r0CountText](const std::size_t r3Count,
        const std::size_t r0Count, const QString& statusText) {
        // 只有完整 R0 快照的数值可与 R3 合并，不能把未知/截断状态伪装成相等。
        if (statusText == QStringLiteral("ok") && r3Count == r0Count)
            return QString::number(static_cast<qulonglong>(r3Count));
        return QStringLiteral("R3:%1 / R0:%2").arg(static_cast<qulonglong>(r3Count))
            .arg(r0CountText(r0Count, statusText));
    };
    for (const auto& aggregate : effectiveCrossRows)
    {
        if (isCanceled()) return {};
        const auto& row = aggregate.row;
        CrossViewValueRow value;
        value.processId = row.processId;
        value.identity = crossViewIdentity('S', {QString::number(row.processId)});
        value.fields = {QString::number(row.processId), row.processName,
            countText(row.tcpCount, aggregate.tcpR0Count, r0TcpStatusText),
            countText(row.udpCount, aggregate.udpR0Count, r0UdpStatusText)};
        summaryRows.push_back(prepareRow(std::move(value), {
            QStringLiteral("R3 TCP: %1 | R0 TCP: %2 | R3 UDP: %3 | R0 UDP: %4")
                .arg(row.tcpSummary.isEmpty() ? QStringLiteral("<无>") : row.tcpSummary)
                .arg(aggregate.tcpR0Summary.isEmpty() ? QStringLiteral("<无>") : aggregate.tcpR0Summary)
                .arg(row.udpSummary.isEmpty() ? QStringLiteral("<无>") : row.udpSummary)
                .arg(aggregate.udpR0Summary.isEmpty() ? QStringLiteral("<无>") : aggregate.udpR0Summary)}));
    }
    if (isCanceled()) return {};
    auto prepared = std::make_shared<CrossViewPresentation>();
    prepared->source = source;
    prepared->sourceGeneration = sourceGeneration;
    prepared->languageGeneration = languageGeneration;
    prepared->r0TcpStatusText = r0TcpStatusText;
    prepared->r0UdpStatusText = r0UdpStatusText;
    prepared->tcpRows = std::move(tcpRows);
    prepared->udpRows = std::move(udpRows);
    prepared->summaryRows = std::move(summaryRows);
    return ks::ui::RetainCompleteExportSnapshot(std::shared_ptr<const CrossViewPresentation>(std::move(prepared)));
}

void NetworkAuditPage::requestCrossViewPreparation()
{
    if (!m_latestAuditSnapshot || !m_crossPreparation) return;
    if (m_crossPrepareRequestedSource == m_latestAuditSnapshot
        && m_crossPrepareRequestedLanguage == m_crossLanguageGeneration) return;
    struct Request
    {
        std::shared_ptr<const AuditSnapshot> source;
        ks::i18n::SourceTextSnapshot language;
        std::uint64_t sourceGeneration;
        std::uint64_t languageGeneration;
        QString tcpStatus;
        QString udpStatus;
    };
    Request request{m_latestAuditSnapshot,
        ks::i18n::LanguageManager::instance().sourceTextSnapshot(),
        m_crossSourceGeneration, m_crossLanguageGeneration,
        m_latestAuditSnapshot->r0TcpStatusText.isEmpty() ? m_r0TcpStatusText : m_latestAuditSnapshot->r0TcpStatusText,
        m_latestAuditSnapshot->r0UdpStatusText.isEmpty() ? m_r0UdpStatusText : m_latestAuditSnapshot->r0UdpStatusText};
    m_crossPrepareRequestedSource = request.source;
    m_crossPrepareRequestedLanguage = request.languageGeneration;
    m_crossPreparation->submit<Request, std::shared_ptr<const CrossViewPresentation>>(
        std::move(request),
        [](const Request& value, const ks::ui::AsyncOperationToken& token) {
            return prepareCrossViewValues(value.source, value.language, value.sourceGeneration,
                value.languageGeneration, value.tcpStatus, value.udpStatus,
                [&token]() { return token.isCanceled(); });
        },
        [this](const std::shared_ptr<const CrossViewPresentation>& prepared, const std::uint64_t ticket) {
            if (!prepared || !m_crossPreparation->isCurrent(ticket)
                || prepared->source != m_latestAuditSnapshot
                || prepared->sourceGeneration != m_crossSourceGeneration
                || prepared->languageGeneration != m_crossLanguageGeneration) return true;
            m_preparedCrossView = prepared; // Accept complete immutable values even when hidden/capture-blocked.
            m_dirtyAuditTables.insert(m_tcpTable);
            m_dirtyAuditTables.insert(m_udpTable);
            m_dirtyAuditTables.insert(m_crossSummaryTable);
            presentVisibleSnapshot();
            return true;
        },
        [this](std::exception_ptr failure) {
            m_crossPrepareRequestedSource.reset(); // An explicit next source/Show may retry; no polling.
            QString detail;
            try { if (failure) std::rethrow_exception(failure); }
            catch (const std::exception& exception) { detail = QString::fromUtf8(exception.what()); }
            catch (...) { detail = QStringLiteral("刷新失败"); }
            kLogEvent event;
            warn << event << "[NetworkAuditPage] cross-view refresh failed, detail="
                << detail.toStdString() << eol;
            if (m_statusLabel) m_statusLabel->setText(QStringLiteral("状态：刷新失败；详情已写入日志。"));
        });
}

const std::vector<NetworkAuditPage::TcpEndpointRow>& NetworkAuditPage::tcpEndpointCache() const
{
    static const std::vector<TcpEndpointRow> empty;
    return m_displayedCrossView ? m_displayedCrossView->source->tcpEndpointRows : empty;
}

void NetworkAuditPage::refreshCrossViewTable(std::shared_ptr<const CrossViewPresentation> prepared)
{
    if (!prepared || m_crossDiffRequestedSource == prepared) return;
    using Model = network_value_detail::Model<CrossViewValue>;
    struct Input { Model::RowsHandle oldRows; const std::vector<CrossViewValue>* rows; quint64 revision, sortRevision; int columns, sortColumn; Qt::SortOrder sortOrder; };
    struct Request { std::shared_ptr<const CrossViewPresentation> source; std::array<Input, 3> tables; ks::i18n::SourceTextSnapshot language; };
    using Result = std::array<Model::PreparedRows, 3>;
    Request request{prepared, {}, ks::i18n::LanguageManager::instance().sourceTextSnapshot()};
    const std::array<QTableView*, 3> tables{m_tcpTable, m_udpTable, m_crossSummaryTable};
    const std::array<const std::vector<CrossViewValue>*, 3> rows{&prepared->tcpRows, &prepared->udpRows, &prepared->summaryRows};
    std::array<QPointer<Model>, 3> models;
    for (std::size_t i = 0; i < tables.size(); ++i)
    {
        auto* proxy = tables[i] ? dynamic_cast<network_value_detail::SortProxy*>(tables[i]->model()) : nullptr;
        models[i] = proxy ? dynamic_cast<Model*>(proxy->sourceModel()) : nullptr;
        if (!models[i] || !models[i]->canAcceptPreparedRows(models[i]->rowsRevision())) return;
        request.tables[i] = {models[i]->rowsHandle(), rows[i], models[i]->rowsRevision(), proxy->sortRevision(), models[i]->columnCount(), proxy->sortColumn(), proxy->sortOrder()};
    }
    const QPointer<NetworkAuditPage> owner(this);
    if (!m_crossDiffOperation) m_crossDiffOperation = new ks::ui::AsyncOperation(this);
    m_crossDiffRequestedSource = prepared;
    const std::array<quint64, 3> sortRevisions{request.tables[0].sortRevision, request.tables[1].sortRevision, request.tables[2].sortRevision};
    m_crossDiffOperation->submit<Request, Result>(std::move(request),
        [](const Request& value, const ks::ui::AsyncOperationToken& token) {
            Result result;
            for (std::size_t i = 0; i < result.size(); ++i)
            {
                if (token.isCanceled()) return Result{};
                const auto& input = value.tables[i];
                auto rows = *input.rows;
                network_value_detail::sortRows(rows, input.sortColumn, input.sortOrder, value.language,
                    [](const CrossViewValue& row, int column) { return row->fields.value(column); }, input.oldRows.get(),
                    [](const CrossViewValue& row) { return row->identity; });
                result[i] = Model::prepareRows(input.oldRows, std::move(rows), input.columns,
                    [](const CrossViewValue& row) { return row->identity; },
                    crossViewValueChanges<Model>(i == 0), input.revision);
            }
            return result;
        },
        [owner, prepared, models, sortRevisions](Result& result, std::uint64_t ticket) {
            if (!owner || !owner->m_crossDiffOperation->isCurrent(ticket)) return true;
            owner->m_crossDiffRequestedSource.reset();
            const auto retry = [owner]() {
                if (!owner) return;
                owner->m_dirtyAuditTables.insert(owner->m_tcpTable);
                owner->m_dirtyAuditTables.insert(owner->m_udpTable);
                owner->m_dirtyAuditTables.insert(owner->m_crossSummaryTable);
                owner->presentVisibleSnapshot();
            };
            const auto current = [&]() {
                return owner && owner->m_preparedCrossView == prepared
                    && owner->m_latestAuditSnapshot == prepared->source
                    && owner->m_crossSourceGeneration == prepared->sourceGeneration
                    && owner->m_crossLanguageGeneration == prepared->languageGeneration;
            };
            if (!current() || !owner->m_crossViewPage->isVisible()) { retry(); return true; }
            if (owner->m_projectionInProgress) { retry(); return true; }
            const std::array<QTableView*, 3> currentTables{owner->m_tcpTable, owner->m_udpTable, owner->m_crossSummaryTable};
            for (std::size_t i = 0; i < models.size(); ++i)
            {
                auto* proxy = currentTables[i] ? dynamic_cast<network_value_detail::SortProxy*>(currentTables[i]->model()) : nullptr;
                if (!models[i] || !proxy || proxy->sourceModel() != models[i].data()
                    || proxy->sortRevision() != sortRevisions[i]
                    || !models[i]->canAcceptPreparedRows(result[i].baseRevision)) { retry(); return true; }
            }
            if (ks::ui::DeferTablePresentationIfValueCaptureOpen(owner.data(),
                QStringLiteral("network-audit-visible-snapshot"),
                {owner->m_tcpTable, owner->m_udpTable, owner->m_crossSummaryTable}, retry)) return true;
            // Preparation finishes outside presentVisibleSnapshot(): retain its
            // original guard across the actual three-model publication as well.
            owner->m_projectionInProgress = true;
            owner->m_projectionRequestedWhileBusy = false;
            const auto finishProjection = qScopeGuard([owner, source = prepared->source]() {
                if (!owner) return;
                owner->m_projectionInProgress = false;
                if (owner->m_projectionRequestedWhileBusy || owner->m_latestAuditSnapshot != source)
                    QTimer::singleShot(0, owner, [owner]() {
                        if (owner) owner->presentVisibleSnapshot();
                    });
            });
            owner->m_displayedCrossView = prepared;
            owner->m_r0TcpStatusText = prepared->r0TcpStatusText;
            owner->m_r0UdpStatusText = prepared->r0UdpStatusText;
            for (std::size_t i = 0; i < models.size(); ++i)
            {
                if (!current() || !models[i] || !models[i]->setPreparedRows(std::move(result[i]))) { retry(); return true; }
            }
            if (!current()) { retry(); return true; }
            owner->applyCrossViewSearchFilter();
            if (owner) owner->updateCrossViewActionState();
            return true;
        },
        [owner](std::exception_ptr) {
            if (!owner) return;
            owner->m_crossDiffRequestedSource.reset();
            owner->m_dirtyAuditTables.insert(owner->m_tcpTable);
            owner->m_dirtyAuditTables.insert(owner->m_udpTable);
            owner->m_dirtyAuditTables.insert(owner->m_crossSummaryTable);
        });
}

void NetworkAuditPage::applyCrossViewSearchFilter()
{
    m_crossFilterDirty = true;
    if (!m_crossViewPage || !m_crossViewPage->isVisible()) return;
    if (!m_displayedCrossView || m_displayedCrossView->languageGeneration != m_crossLanguageGeneration)
    {
        requestCrossViewPreparation();
        return;
    }
    const QPointer<NetworkAuditPage> owner(this);
    if (ks::ui::DeferTablePresentationIfValueCaptureOpen(this,
        QStringLiteral("network_cross_filter"), {m_tcpTable, m_udpTable, m_crossSummaryTable},
        [owner]() { if (owner) owner->applyCrossViewSearchFilter(); })) return;

    // Capture only immutable values. Proxy order and persistent indexes remain GUI-owned.
    struct Request
    {
        std::shared_ptr<const CrossViewPresentation> source;
        QString filter;
        QSet<quint32> processIds;
        std::array<QVector<int>, 3> columns;
    };
    struct ExcludedKeys
    {
        // Keys borrow immutable source strings; keep that value owner alive.
        std::shared_ptr<const CrossViewPresentation> source;
        std::unordered_set<std::string_view> keys, duplicateKeys;
        std::unordered_set<const CrossViewValueRow*> duplicateRows;
    };
    using Matches = std::array<std::shared_ptr<const ExcludedKeys>, 3>;
    Request request{m_displayedCrossView,
        m_crossSearchEdit ? m_crossSearchEdit->text().trimmed().toCaseFolded() : QString(),
        m_processFilterSet, {}};
    const std::array<QTableView*, 3> tables{m_tcpTable, m_udpTable, m_crossSummaryTable};
    for (std::size_t i = 0; i < tables.size(); ++i)
        if (tables[i])
            for (int column = 0; column < tables[i]->model()->columnCount(); ++column)
                if (!tables[i]->isColumnHidden(column)) request.columns[i].append(column);

    const auto publish = [owner, request](const Matches& matches)
    {
        const auto current = [&]()
        {
            return owner && owner->m_displayedCrossView == request.source
                && owner->m_crossLanguageGeneration == request.source->languageGeneration
                && owner->m_processFilterSet == request.processIds
                && (owner->m_crossSearchEdit ? owner->m_crossSearchEdit->text().trimmed().toCaseFolded() : QString()) == request.filter;
        };
        if (!current()) return true;
        if (!owner->m_crossViewPage || !owner->m_crossViewPage->isVisible()) return true;
        if (owner->m_projectionInProgress)
        {
            owner->m_projectionRequestedWhileBusy = true;
            return true;
        }
        if (ks::ui::DeferTablePresentationIfValueCaptureOpen(owner.data(),
            QStringLiteral("network_cross_filter"), {owner->m_tcpTable, owner->m_udpTable, owner->m_crossSummaryTable},
            [owner]() { if (owner) owner->applyCrossViewSearchFilter(); })) return true;
        const std::array<QTableView*, 3> currentTables{owner->m_tcpTable, owner->m_udpTable, owner->m_crossSummaryTable};
        const bool active = !request.filter.isEmpty() || !request.processIds.isEmpty();
        const std::array<std::size_t, 3> sourceSizes{request.source->tcpRows.size(),
            request.source->udpRows.size(), request.source->summaryRows.size()};
        // Check all captured columns before publishing any table, including a hidden-column change in flight.
        for (std::size_t i = 0; i < currentTables.size(); ++i)
        {
            if (!currentTables[i]) continue;
            auto* proxy = dynamic_cast<network_value_detail::SortProxy*>(currentTables[i]->model());
            if (!proxy || !proxy->sourceModel()
                || proxy->sourceModel()->rowCount() != static_cast<int>(sourceSizes[i])
                || (active && !matches[i])) return true;
            QVector<int> columns;
            for (int column = 0; column < currentTables[i]->model()->columnCount(); ++column)
                if (!currentTables[i]->isColumnHidden(column)) columns.append(column);
            if (columns != request.columns[i])
            {
                if (owner->m_crossSearchTimer) owner->m_crossSearchTimer->start();
                return true;
            }
        }
        for (std::size_t i = 0; i < currentTables.size(); ++i)
        {
            QTableView* const table = currentTables[i];
            if (!table) continue;
            auto* proxy = dynamic_cast<network_value_detail::SortProxy*>(table->model());
            using Model = network_value_detail::Model<CrossViewValue>;
            const QPointer<Model> model = dynamic_cast<Model*>(proxy->sourceModel());
            if (!model) return true;
            const auto excluded = active ? matches[i] : std::shared_ptr<const ExcludedKeys>();
            if (!excluded || excluded->keys.empty())
                ks::ui::ApplyTableRowFilterPredicateIncrementally(table, {});
            else
                ks::ui::ApplyTableRowFilterPredicateIncrementally(table,
                    [proxy = QPointer<network_value_detail::SortProxy>(proxy), model, excluded](const QModelIndex& index) {
                        if (!proxy || !model) return false;
                        const auto* row = model->rowAt(proxy->mapToSource(index).row());
                        if (!row) return false;
                        // The existing keyed model resets duplicate-key sources;
                        // within that source, distinguish each immutable value.
                        if (excluded->duplicateKeys.find((*row)->identity) != excluded->duplicateKeys.end())
                            return excluded->duplicateRows.find(row->get()) != excluded->duplicateRows.end();
                        return excluded->keys.find((*row)->identity) != excluded->keys.end();
                    });
            if (!current()) return true;
            if (auto* selection = table->selectionModel())
            {
                QItemSelection removed;
                for (const auto& range : network_value_detail::selectedRanges(selection))
                    for (int row = range.first; row <= range.second; ++row)
                        if (ks::ui::IsTableRowExcludedByFilter(table, row))
                            removed.select(table->model()->index(row, 0),
                                table->model()->index(row, table->model()->columnCount() - 1));
                if (!removed.isEmpty()) selection->select(removed, QItemSelectionModel::Deselect);
                if (!current()) return true;
                if (ks::ui::IsTableRowExcludedByFilter(table, table->currentIndex().row()))
                    selection->setCurrentIndex(QModelIndex(), QItemSelectionModel::NoUpdate);
                if (!current()) return true;
            }
        }
        owner->m_crossFilterDirty = false;
        owner->updateCrossViewActionState();
        return true;
    };
    if (request.filter.isEmpty() && request.processIds.isEmpty())
    {
        if (m_crossFilterOperation) m_crossFilterOperation->cancel();
        if (owner) publish(Matches{}); // Clearing a filter needs no worker or full row scan.
        return;
    }
    if (!m_crossFilterOperation) m_crossFilterOperation = new ks::ui::AsyncOperation(this);
    m_crossFilterOperation->submit<Request, Matches>(request,
        [](const Request& value, const ks::ui::AsyncOperationToken& token)
        {
            Matches result;
            const std::array<const std::vector<CrossViewValue>*, 3> rows{
                &value.source->tcpRows, &value.source->udpRows, &value.source->summaryRows};
            for (std::size_t i = 0; i < rows.size(); ++i)
            {
                auto excluded = std::make_shared<ExcludedKeys>();
                excluded->source = value.source;
                std::unordered_set<std::string_view> seen;
                seen.reserve(rows[i]->size());
                for (const auto& row : *rows[i])
                {
                    if (token.isCanceled()) return Matches{};
                    if (!seen.insert(row->identity).second) excluded->duplicateKeys.insert(row->identity);
                }
                for (const auto& row : *rows[i])
                {
                    if (token.isCanceled()) return Matches{};
                    bool matched = value.processIds.isEmpty() || value.processIds.contains(row->processId);
                    if (matched && !value.filter.isEmpty())
                    {
                        matched = false;
                        for (int column : value.columns[i])
                            if (column < row->searchFields.size() && row->searchFields.at(column).contains(value.filter))
                            { matched = true; break; }
                    }
                    if (!matched)
                    {
                        excluded->keys.insert(row->identity);
                        if (excluded->duplicateKeys.find(row->identity) != excluded->duplicateKeys.end())
                            excluded->duplicateRows.insert(row.get());
                    }
                }
                result[i] = ks::ui::RetainCompleteExportSnapshot(std::shared_ptr<const ExcludedKeys>(std::move(excluded)));
            }
            return result;
        },
        [owner, publish](const Matches& matches, const std::uint64_t ticket)
        {
            return !owner || !owner->m_crossFilterOperation->isCurrent(ticket) || publish(matches);
        },
        [owner](std::exception_ptr)
        {
            // Keep the accepted filter; the next user search/refresh can retry.
            if (owner) owner->m_crossFilterDirty = true;
        });
}

QIcon NetworkAuditPage::resolveProcessIcon(const std::uint32_t processId)
{
    if (processId == 0U)
    {
        return auditProcessPlaceholderIcon();
    }

    const quint32 processIdKey = static_cast<quint32>(processId);
    const auto cachedIterator = m_processIconCache.constFind(processIdKey);
    if (cachedIterator != m_processIconCache.constEnd())
    {
        return cachedIterator.value();
    }

    // 建表链路只允许做缓存查询：首屏有连接的进程常有上百个，
    // 逐个 OpenProcess + Shell 图标提取会把整段建表拖到秒级。
    scheduleProcessIconResolution(processId);
    return auditProcessPlaceholderIcon();
}

void NetworkAuditPage::scheduleProcessIconResolution(const std::uint32_t processId)
{
    const quint32 processIdKey = static_cast<quint32>(processId);
    if (processIdKey == 0U ||
        m_processIconCache.contains(processIdKey) ||
        m_processIconPendingPidSet.contains(processIdKey))
    {
        return;
    }
    m_processIconPendingPidSet.insert(processIdKey);

    // 复用本页既有的“共享状态 + owner 复核”回投模式：
    // 页面析构时 owner 已置空，工作线程不会向已销毁的 QWidget 投递调用。
    const std::shared_ptr<NetworkAuditAsyncState> asyncState = m_asyncState;
    QThreadPool::globalInstance()->start([asyncState, processIdKey]()
        {
            QImage processIconImage = extractProcessIconImageForPid(processIdKey);

            std::lock_guard<std::mutex> dispatchLock(asyncState->mutex);
            NetworkAuditPage* const receiver = asyncState->owner;
            if (receiver == nullptr)
            {
                return;
            }
            QMetaObject::invokeMethod(
                receiver,
                [asyncState, processIdKey, processIconImage = std::move(processIconImage)]() mutable
                {
                    NetworkAuditPage* page = nullptr;
                    {
                        std::lock_guard<std::mutex> stateLock(asyncState->mutex);
                        page = asyncState->owner;
                    }
                    if (page == nullptr)
                    {
                        return;
                    }
                    page->applyProcessIconResolutionResult(processIdKey, std::move(processIconImage));
                },
                Qt::QueuedConnection);
        });
}

void NetworkAuditPage::applyProcessIconResolutionResult(
    const std::uint32_t processId,
    QImage iconImage)
{
    const quint32 processIdKey = static_cast<quint32>(processId);
    m_processIconPendingPidSet.remove(processIdKey);

    // QPixmap/QIcon 只能在 UI 线程构造；解析失败也写入缓存，避免同一 PID 反复冷查。
    const QIcon resolvedIcon = iconImage.isNull()
        ? auditProcessPlaceholderIcon()
        : QIcon(QPixmap::fromImage(iconImage));
    m_processIconCache.insert(processIdKey, resolvedIcon);

    // Value models read this cache on demand. Repaint coalesces on the exposed
    // viewports; a resolved PID never triggers a scan of every endpoint row.
    for (auto* table : {m_tcpTable, m_udpTable, m_crossSummaryTable})
        if (table && table->isVisible()) table->viewport()->update();
}

void NetworkAuditPage::updateCrossViewActionState()
{
    bool canTerminateSelection = false;
    if (m_tcpTable != nullptr
        && m_tcpTable->currentIndex().row() >= 0
        && !ks::ui::IsTableRowExcludedByFilter(m_tcpTable, m_tcpTable->currentIndex().row()))
    {
        const auto pidIndex = m_tcpTable->model()->index(m_tcpTable->currentIndex().row(), 0);
        bool cacheIndexOk = false;
        const qulonglong cacheIndexValue = pidIndex.data(Qt::UserRole).toULongLong(&cacheIndexOk);
        canTerminateSelection =
            cacheIndexOk
            && cacheIndexValue < tcpEndpointCache().size()
            && tcpEndpointCache()[static_cast<std::size_t>(cacheIndexValue)].canTerminate;
    }
    if (m_crossTerminateButton != nullptr)
    {
        m_crossTerminateButton->setEnabled(canTerminateSelection);
    }
    if (m_clearProcessFilterButton != nullptr)
    {
        m_clearProcessFilterButton->setEnabled(!m_processFilterSet.isEmpty());
    }
}

void NetworkAuditPage::terminateSelectedTcpConnection()
{
    if (m_tcpTable == nullptr || m_tcpTable->currentIndex().row() < 0
        || ks::ui::IsTableRowExcludedByFilter(m_tcpTable, m_tcpTable->currentIndex().row()))
    {
        QMessageBox::information(this, QStringLiteral("网络审计"), QStringLiteral("请先选中一条 TCP 连接。"));
        return;
    }

    const auto pidIndex = m_tcpTable->model()->index(m_tcpTable->currentIndex().row(), 0);
    bool cacheIndexOk = false;
    const qulonglong cacheIndexValue = pidIndex.data(Qt::UserRole).toULongLong(&cacheIndexOk);
    if (!cacheIndexOk || cacheIndexValue >= tcpEndpointCache().size())
    {
        QMessageBox::warning(this, QStringLiteral("网络审计"), QStringLiteral("所选连接已过期，请刷新后重试。"));
        return;
    }

    const TcpEndpointRow endpointRow = tcpEndpointCache()[static_cast<std::size_t>(cacheIndexValue)];
    if (!endpointRow.canTerminate)
    {
        QMessageBox::information(
            this,
            QStringLiteral("网络审计"),
            QStringLiteral("该行来自 R0 只读快照；请选中对应的 R3 TCP 行执行终止。"));
        return;
    }

    const std::string unsupportedReason =
        ks::network::GetTcpTerminationUnsupportedReason(endpointRow.connectionRecord);
    if (!unsupportedReason.empty())
    {
        QMessageBox::information(
            this,
            QStringLiteral("网络审计"),
            QStringLiteral("当前 TCP 行不能终止：%1")
                .arg(QString::fromUtf8(unsupportedReason.c_str())));
        return;
    }

    const int confirmation = QMessageBox::question(
        this,
        QStringLiteral("终止 TCP 连接"),
        QStringLiteral("确认终止连接？\nPID=%1\n本地=%2\n远端=%3")
            .arg(endpointRow.processId)
            .arg(endpointRow.localEndpointText)
            .arg(endpointRow.remoteEndpointText),
        QMessageBox::Yes | QMessageBox::No,
        QMessageBox::No);
    if (confirmation != QMessageBox::Yes)
    {
        return;
    }

    std::string detailText;
    const bool terminateOk =
        ks::network::TerminateTcpConnectionByRecord(endpointRow.connectionRecord, &detailText);
    if (terminateOk)
    {
        QMessageBox::information(this, QStringLiteral("网络审计"), QStringLiteral("连接终止请求已提交。"));
        refreshAllSnapshotsAsync(true);
        return;
    }

    const bool privilegePromptHandled = ks::ui::promptForPrivilegeFailure(
        this,
        QStringLiteral("终止 TCP 连接"),
        QString::fromUtf8(detailText.c_str()));
    if (!privilegePromptHandled)
    {
        QMessageBox::warning(
            this,
            QStringLiteral("网络审计"),
            QStringLiteral("终止连接失败：%1").arg(QString::fromUtf8(detailText.c_str())));
    }
}

void NetworkAuditPage::showCrossViewContextMenu(
    QTableView* tableWidget,
    const QPoint& localPosition)
{
    if (tableWidget == nullptr)
    {
        return;
    }

    const QModelIndex clickedIndex = tableWidget->indexAt(localPosition);
    if (clickedIndex.isValid() && ks::ui::IsTableRowExcludedByFilter(tableWidget, clickedIndex.row())) return;
    if (clickedIndex.isValid())
    {
        tableWidget->setCurrentIndex(clickedIndex);
        tableWidget->selectRow(clickedIndex.row());
    }

    const int selectedRow = tableWidget->currentIndex().row();
    bool processIdOk = false;
    const auto processIdIndex = tableWidget->model()->index(selectedRow, 0);
    const quint32 selectedProcessId = processIdIndex.data().toString().toUInt(&processIdOk);
    const bool validRow = processIdIndex.isValid()
        && !ks::ui::IsTableRowExcludedByFilter(tableWidget, selectedRow);
    const bool hasProcess = validRow && processIdOk && selectedProcessId != 0U;

    bool canTerminateSelection = false;
    if (tableWidget == m_tcpTable && validRow && processIdIndex.isValid())
    {
        bool cacheIndexOk = false;
        const qulonglong cacheIndexValue =
            processIdIndex.data(Qt::UserRole).toULongLong(&cacheIndexOk);
        canTerminateSelection =
            cacheIndexOk
            && cacheIndexValue < tcpEndpointCache().size()
            && tcpEndpointCache()[static_cast<std::size_t>(cacheIndexValue)].canTerminate;
    }

    QMenu menu(tableWidget);
    menu.setStyleSheet(KswordTheme::ContextMenuStyle());
    QAction* terminateAction = menu.addAction(
        QIcon(QStringLiteral(":/Icon/process_terminate.svg")),
        QStringLiteral("终止此 TCP 连接"));
    terminateAction->setVisible(tableWidget == m_tcpTable);
    terminateAction->setEnabled(canTerminateSelection);
    QAction* copyAction = menu.addAction(
        QIcon(QStringLiteral(":/Icon/process_copy_row.svg")),
        QStringLiteral("复制行"));
    // 声明业务菜单已提供此能力，避免共享表格菜单追加重复项。
    ks::ui::SetTableContextActionRole(copyAction, ks::ui::TableContextActionRole::CopyRows);
    copyAction->setEnabled(validRow);
    QAction* trackProcessAction = menu.addAction(
        QIcon(QStringLiteral(":/Icon/log_track.svg")),
        QStringLiteral("跟踪此进程"));
    trackProcessAction->setEnabled(hasProcess && static_cast<bool>(m_trackProcessHandler));
    QAction* openProcessDetailAction = menu.addAction(
        QIcon(QStringLiteral(":/Icon/process_details.svg")),
        QStringLiteral("转到进程详细信息"));
    openProcessDetailAction->setEnabled(
        hasProcess && static_cast<bool>(m_openProcessDetailHandler));
    const QString localEndpointText = tableWidget->model()->index(selectedRow, 2).data().toString();
    const bool canPrefillUdpBlockRule =
        tableWidget == m_udpTable && validRow &&
        !localEndpointText.trimmed().isEmpty() &&
        static_cast<bool>(m_udpEndpointBlockRuleHandler);
    QAction* addUdpBlockRuleAction = menu.addAction(
        QIcon(QStringLiteral(":/Icon/process_terminate.svg")),
        QStringLiteral("预填 UDP 阻断规则"));
    addUdpBlockRuleAction->setVisible(tableWidget == m_udpTable);
    addUdpBlockRuleAction->setEnabled(canPrefillUdpBlockRule);
    const bool isTcpTable = tableWidget == m_tcpTable;
    QAction* uploadVirusTotalAction = ks::online_scan::addVirusTotalSandboxMenu(
        &menu,
        this,
        [isTcpTable, selectedProcessId, hasProcess]() -> ks::online_scan::SandboxUploadTarget
        {
            ks::online_scan::SandboxUploadTarget uploadTarget;
            if (!hasProcess)
            {
                uploadTarget.errorText = isTcpTable
                    ? QStringLiteral("当前 TCP 行没有可解析 PID。")
                    : QStringLiteral("当前 UDP 行没有可解析 PID。");
                return uploadTarget;
            }
            uploadTarget.filePath = QString::fromStdString(
                ks::process::QueryProcessPathByPid(selectedProcessId));
            uploadTarget.sourceText = isTcpTable
                ? QStringLiteral("网络 TCP 连接 PID=%1").arg(selectedProcessId)
                : QStringLiteral("网络 UDP 端点 PID=%1").arg(selectedProcessId);
            return uploadTarget;
        });
    if (uploadVirusTotalAction != nullptr)
    {
        uploadVirusTotalAction->setEnabled(hasProcess);
    }
    menu.addSeparator();
    QAction* refreshAction = menu.addAction(
        QIcon(QStringLiteral(":/Icon/process_refresh.svg")),
        QStringLiteral("刷新 TCP/UDP"));
    QAction* clearFilterAction = menu.addAction(
        QIcon(QStringLiteral(":/Icon/log_clear.svg")),
        QStringLiteral("清除 PID 筛选"));
    clearFilterAction->setEnabled(!m_processFilterSet.isEmpty());

    const QAction* selectedAction = menu.exec(tableWidget->viewport()->mapToGlobal(localPosition));
    if (selectedAction == terminateAction)
    {
        terminateSelectedTcpConnection();
    }
    else if (selectedAction == copyAction)
    {
        copyCurrentTableRow(tableWidget);
    }
    else if (selectedAction == trackProcessAction)
    {
        m_trackProcessHandler(selectedProcessId);
    }
    else if (selectedAction == openProcessDetailAction)
    {
        m_openProcessDetailHandler(selectedProcessId);
    }
    else if (selectedAction == addUdpBlockRuleAction && canPrefillUdpBlockRule)
    {
        m_udpEndpointBlockRuleHandler(selectedProcessId, localEndpointText);
    }
    else if (selectedAction == uploadVirusTotalAction)
    {
        return;
    }
    else if (selectedAction == refreshAction)
    {
        refreshAllSnapshotsAsync(true);
    }
    else if (selectedAction == clearFilterAction)
    {
        focusProcessIds({});
    }
}

void NetworkAuditPage::refreshAfdTable(const std::shared_ptr<AuditSnapshot>& snapshot)
{
    if (!m_afdTable) return;
    presentAuditValueRows(m_afdTable, snapshot, snapshot->afdRows, [](const AfdHandleRow& row, int column) -> QVariant
    {
        switch (column)
        {
        case 0: return QString::number(row.processId);
        case 1: return row.processName;
        case 2: return row.handleValueText;
        case 3: return row.typeName;
        case 4: return row.objectName;
        case 5: return row.sourceText;
        case 6: return row.diffText;
        case 7: return row.detailText + QStringLiteral(" | access=") + row.accessText;
        default: return {};
        }
    });
}

void NetworkAuditPage::refreshWfpTables(const std::shared_ptr<AuditSnapshot>& snapshot, QTableView* targetTable)
{
    if (m_wfpProviderTable && m_wfpProviderTable == targetTable)
        presentAuditValueRows(m_wfpProviderTable, snapshot, snapshot->wfpProviderRows, [](const WfpProviderRow& row, int column) -> QVariant
        {
            switch (column)
            {
            case 0: return row.nameText;
            case 1: return row.descriptionText;
            case 2: return row.guidText;
            case 3: return row.flagsText;
            case 4: return row.serviceNameText;
            case 5: return row.dataSizeText;
            default: return {};
            }
        });
    if (m_wfpSubLayerTable && m_wfpSubLayerTable == targetTable)
        presentAuditValueRows(m_wfpSubLayerTable, snapshot, snapshot->wfpSubLayerRows, [](const WfpSubLayerRow& row, int column) -> QVariant
        {
            switch (column)
            {
            case 0: return row.nameText;
            case 1: return row.descriptionText;
            case 2: return row.guidText;
            case 3: return row.flagsText;
            case 4: return row.providerGuidText;
            case 5: return row.weightText;
            default: return {};
            }
        });
    if (m_wfpCalloutTable && m_wfpCalloutTable == targetTable)
        presentAuditValueRows(m_wfpCalloutTable, snapshot, snapshot->wfpCalloutRows, [](const WfpCalloutRow& row, int column) -> QVariant
        {
            switch (column)
            {
            case 0: return row.nameText;
            case 1: return row.descriptionText;
            case 2: return row.guidText;
            case 3: return row.flagsText;
            case 4: return row.providerGuidText;
            case 5: return row.layerGuidText;
            case 6: return row.calloutIdText;
            default: return {};
            }
        });
    if (m_wfpFilterTable && m_wfpFilterTable == targetTable)
        presentAuditValueRows(m_wfpFilterTable, snapshot, snapshot->wfpFilterRows, [](const WfpFilterRow& row, int column) -> QVariant
        {
            switch (column)
            {
            case 0: return row.nameText;
            case 1: return row.descriptionText;
            case 2: return row.guidText;
            case 3: return row.flagsText;
            case 4: return row.providerGuidText;
            case 5: return row.layerGuidText;
            case 6: return row.subLayerGuidText;
            case 7: return row.weightText;
            case 8: return row.actionText;
            case 9: return row.conditionText;
            case 10: return row.filterIdText;
            default: return {};
            }
        });
}

void NetworkAuditPage::refreshNdisTables(const std::shared_ptr<AuditSnapshot>& snapshot, QTableView* targetTable)
{
    if (m_ndisAdapterTable && m_ndisAdapterTable == targetTable)
        presentAuditValueRows(m_ndisAdapterTable, snapshot, snapshot->ndisAdapterRows, [](const NdisAdapterRow& row, int column) -> QVariant
        {
            switch (column)
            {
            case 0: return row.nameText;
            case 1: return row.descriptionText;
            case 2: return row.ifIndexText;
            case 3: return row.statusText;
            case 4: return row.macText;
            case 5: return row.linkSpeedText;
            case 6: return row.connectionStateText;
            default: return {};
            }
        });
    if (m_ndisBindingTable && m_ndisBindingTable == targetTable)
        presentAuditValueRows(m_ndisBindingTable, snapshot, snapshot->ndisBindingRows, [](const NdisBindingRow& row, int column) -> QVariant
        {
            switch (column)
            {
            case 0: return row.adapterNameText;
            case 1: return row.displayNameText;
            case 2: return row.componentIdText;
            case 3: return row.enabledText;
            case 4: return row.instanceIdText;
            default: return {};
            }
        });
    if (m_ndisProtocolTable && m_ndisProtocolTable == targetTable)
        presentAuditValueRows(m_ndisProtocolTable, snapshot, snapshot->ndisProtocolRows, [](const NdisProtocolRow& row, int column) -> QVariant
        {
            switch (column)
            {
            case 0: return row.interfaceAliasText;
            case 1: return row.ifIndexText;
            case 2: return row.addressFamilyText;
            case 3: return row.connectionStateText;
            case 4: return row.interfaceMetricText;
            case 5: return row.mtuText;
            default: return {};
            }
        });
    if (m_ndisUnknownTable && m_ndisUnknownTable == targetTable)
        presentAuditValueRows(m_ndisUnknownTable, snapshot, snapshot->ndisUnknownRows, [](const NdisUnknownRow& row, int column) -> QVariant
        {
            switch (column)
            {
            case 0: return row.kindText;
            case 1: return row.componentText;
            case 2: return row.ownerModuleText;
            case 3: return row.objectAddressText;
            case 4: return row.detailText;
            default: return {};
            }
        });
}

void NetworkAuditPage::refreshNsiSummaryTable(const AuditSnapshot& snapshot)
{
    if (m_nsiSummaryTable == nullptr)
    {
        return;
    }
    const int r3RowCount = static_cast<int>(snapshot.nsiSummaryRows.size());
    const int r0RowCount = static_cast<int>(snapshot.r0SummaryRows.size());
    m_nsiSummaryTable->setRowCount(r3RowCount + r0RowCount);
    int rowIndex = 0;
    for (const NsiSummaryRow& row : snapshot.nsiSummaryRows)
    {
        m_nsiSummaryTable->setItem(rowIndex, 0, createReadOnlyCell(row.metricText));
        m_nsiSummaryTable->setItem(rowIndex, 1, createReadOnlyCell(row.valueText));
        m_nsiSummaryTable->setItem(rowIndex, 2, createReadOnlyCell(QStringLiteral("-")));
        m_nsiSummaryTable->setItem(rowIndex, 3, createReadOnlyCell(QStringLiteral("-")));
        m_nsiSummaryTable->setItem(rowIndex, 4, createReadOnlyCell(QStringLiteral("R3 summary")));
        ++rowIndex;
    }
    for (const R0NetworkSummaryRow& row : snapshot.r0SummaryRows)
    {
        m_nsiSummaryTable->setItem(rowIndex, 0, createReadOnlyCell(row.nameText));
        m_nsiSummaryTable->setItem(rowIndex, 1, createReadOnlyCell(row.statusText));
        m_nsiSummaryTable->setItem(rowIndex, 2, createReadOnlyCell(row.countText));
        m_nsiSummaryTable->setItem(rowIndex, 3, createReadOnlyCell(row.truncatedText));
        m_nsiSummaryTable->setItem(rowIndex, 4, createReadOnlyCell(row.messageText));
        ++rowIndex;
    }
}

QString NetworkAuditPage::runPowerShellTextSync(const QString& scriptText, const int timeoutMs, QString* errorTextOut)
{
    QProcess process;
    process.setProgram(QStringLiteral("powershell.exe"));
    process.setArguments({
        QStringLiteral("-NoProfile"),
        QStringLiteral("-ExecutionPolicy"),
        QStringLiteral("Bypass"),
        QStringLiteral("-Command"),
        scriptText
    });
    process.start();

    if (!process.waitForStarted(2000))
    {
        if (errorTextOut != nullptr)
        {
            *errorTextOut = QStringLiteral("PowerShell 启动失败：%1").arg(process.errorString());
        }
        return QString();
    }

    if (!process.waitForFinished(timeoutMs))
    {
        process.kill();
        process.waitForFinished(500);
        if (errorTextOut != nullptr)
        {
            *errorTextOut = QStringLiteral("PowerShell 执行超时：%1 ms").arg(timeoutMs);
        }
        return QString();
    }

    const QString stdoutText = QString::fromLocal8Bit(process.readAllStandardOutput()).trimmed();
    const QString stderrText = QString::fromLocal8Bit(process.readAllStandardError()).trimmed();
    if (process.exitStatus() != QProcess::NormalExit || process.exitCode() != 0)
    {
        if (errorTextOut != nullptr)
        {
            *errorTextOut = QStringLiteral("PowerShell 退出异常：%1 / %2").arg(process.exitCode()).arg(stderrText);
        }
        return stdoutText;
    }
    return stdoutText;
}

QTableWidgetItem* NetworkAuditPage::createCell(const QString& cellText)
{
    return createReadOnlyCell(cellText);
}

QString NetworkAuditPage::guidToText(const GUID& guid)
{
    return QStringLiteral("{%1-%2-%3-%4%5%6%7%8%9%10%11}")
        .arg(guid.Data1, 8, 16, QLatin1Char('0'))
        .arg(guid.Data2, 4, 16, QLatin1Char('0'))
        .arg(guid.Data3, 4, 16, QLatin1Char('0'))
        .arg(guid.Data4[0], 2, 16, QLatin1Char('0'))
        .arg(guid.Data4[1], 2, 16, QLatin1Char('0'))
        .arg(guid.Data4[2], 2, 16, QLatin1Char('0'))
        .arg(guid.Data4[3], 2, 16, QLatin1Char('0'))
        .arg(guid.Data4[4], 2, 16, QLatin1Char('0'))
        .arg(guid.Data4[5], 2, 16, QLatin1Char('0'))
        .arg(guid.Data4[6], 2, 16, QLatin1Char('0'))
        .arg(guid.Data4[7], 2, 16, QLatin1Char('0'));
}

QString NetworkAuditPage::bytesToHexText(const std::uint64_t value)
{
    return QStringLiteral("0x%1").arg(QString::number(value, 16));
}

QString NetworkAuditPage::objectToText(const QJsonValue& value)
{
    if (value.isString())
    {
        return value.toString();
    }
    if (value.isBool())
    {
        return value.toBool() ? QStringLiteral("true") : QStringLiteral("false");
    }
    if (value.isDouble())
    {
        return QString::number(value.toDouble());
    }
    if (value.isArray() || value.isObject())
    {
        if (value.isObject())
        {
            return QString::fromUtf8(QJsonDocument(value.toObject()).toJson(QJsonDocument::Compact));
        }
        return QString::fromUtf8(QJsonDocument(value.toArray()).toJson(QJsonDocument::Compact));
    }
    return QString();
}

bool NetworkAuditPage::compareContainsAfd(const QString& objectNameText)
{
    const QString lowered = objectNameText.toLower();
    return lowered.contains(QStringLiteral("\\device\\afd")) || lowered.contains(QStringLiteral("\\device\\winsock"));
}
