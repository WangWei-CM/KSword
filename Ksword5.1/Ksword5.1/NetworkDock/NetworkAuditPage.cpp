#include "NetworkAuditPage.h"
#include "../UI/VisibleTableWidget.h"

// ============================================================
// NetworkAuditPage.cpp
// 作用：
// 1) 提供只读网络审计页的 UI 与快照刷新逻辑；
// 2) 复用现有 ks::network / ks::process / handle helper 做 cross-view；
// 3) 所有数据仅用于审计展示，不提供任何修改系统网络状态的动作。
// ============================================================

#include "../theme.h"
#include "../ArkDriverClient/ArkDriverClient.h"
#include "../ksword/file/file_handle_tools.h"
#include "../ksword/network/network.h"
#include "../ksword/network/network_connection_tools.h"
#include "../ksword/process/process.h"

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
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QMenu>
#include <QMetaObject>
#include <QModelIndex>
#include <QPixmap>
#include <QPointer>
#include <QProcess>
#include <QPushButton>
#include <QSplitter>
#include <QTabWidget>
#include <QTableWidget>
#include <QTableWidgetItem>
#include <QThread>
#include <QVBoxLayout>

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <memory>
#include <set>
#include <thread>
#include <unordered_map>

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#include <Rpc.h>
#include <fwpmu.h>

#pragma comment(lib, "Ws2_32.lib")

namespace
{
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
    void copyCurrentTableRow(QTableWidget* table)
    {
        if (table == nullptr || QApplication::clipboard() == nullptr)
        {
            return;
        }

        const int rowIndex = table->currentRow();
        if (rowIndex < 0 || rowIndex >= table->rowCount())
        {
            return;
        }

        QStringList fields;
        fields.reserve(table->columnCount());
        for (int columnIndex = 0; columnIndex < table->columnCount(); ++columnIndex)
        {
            const QTableWidgetItem* item = table->item(rowIndex, columnIndex);
            fields.push_back(item != nullptr ? item->text() : QString());
        }
        QApplication::clipboard()->setText(fields.join('\t'));
    }

    // installCopyMenu 作用：
    // - 给只读审计表格安装“复制当前行”菜单；
    // - 输入 table：需要安装菜单的表格；
    // - 返回：无，菜单只读，不改变网络栈状态。
    void installCopyMenu(QTableWidget* table)
    {
        if (table == nullptr)
        {
            return;
        }

        table->setContextMenuPolicy(Qt::CustomContextMenu);
        QObject::connect(table, &QTableWidget::customContextMenuRequested, table, [table](const QPoint& localPosition)
        {
            const QModelIndex clickedIndex = table->indexAt(localPosition);
            if (clickedIndex.isValid())
            {
                table->setCurrentCell(clickedIndex.row(), clickedIndex.column());
                table->selectRow(clickedIndex.row());
            }

            QMenu menu(table);
            menu.setStyleSheet(tableMenuStyle());
            QAction* copyRowAction = menu.addAction(QIcon(QStringLiteral(":/Icon/process_copy_row.svg")), QStringLiteral("复制当前行"));
            copyRowAction->setEnabled(table->currentRow() >= 0);
            if (menu.exec(table->viewport()->mapToGlobal(localPosition)) == copyRowAction)
            {
                copyCurrentTableRow(table);
            }
        });
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
            return QStringLiteral("当前驱动不支持该 R0 网络审计入口，请同步更新 R0/R3 组件");
        }
        if (rawText.contains(QStringLiteral("capability"), Qt::CaseInsensitive) ||
            rawText.contains(QStringLiteral("DynData"), Qt::CaseInsensitive) ||
            rawText.contains(QStringLiteral("profile"), Qt::CaseInsensitive))
        {
            // 动态偏移诊断：
            // - 输入：驱动返回的 capability/DynData/profile 相关说明；
            // - 处理：转换成用户能理解的偏移能力缺口；
            // - 返回：指向内核 DynData 页继续排查。
            return QStringLiteral("动态偏移或能力组未满足，请先查看“内核 -> DynData/Capability”状态");
        }
        if (rawText.contains(QStringLiteral("access denied"), Qt::CaseInsensitive) ||
            rawText.contains(QStringLiteral("privilege"), Qt::CaseInsensitive))
        {
            return QStringLiteral("权限不足，当前进程无法完成 R0 网络审计查询");
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
        if (rawText.startsWith(QStringLiteral("version="), Qt::CaseInsensitive))
        {
            return QStringLiteral("驱动已返回结构化网络审计数据");
        }
        return rawText;
    }

    // r0AuditStatusText 作用：
    // - 将 R0 wrapper 的 ok/unsupported/unavailable 状态转成 UI 文本；
    // - 输入 result：任意带 io/unsupported 字段的 ArkDriverClient 审计结果；
    // - 返回：验收要求中的 ok / unsupported / unavailable。
    template <typename TResult>
    QString r0AuditStatusText(const TResult& result)
    {
        if (result.io.ok)
        {
            return QStringLiteral("ok");
        }
        if (result.unsupported)
        {
            return QStringLiteral("unsupported");
        }
        return QStringLiteral("unavailable");
    }

    // r0AuditTruncatedText 作用：
    // - 根据 total/returned 判断 R0 结果是否被截断；
    // - 输入 result：任意 VariableAuditResultBase 派生结果；
    // - 返回：true/false 文本，方便摘要表直接展示。
    template <typename TResult>
    QString r0AuditTruncatedText(const TResult& result)
    {
        return result.totalCount > result.returnedCount
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
    // - 返回：Miniport/Filter/Protocol/Binding 等可读分类。
    QString r0NdisObjectKindText(const unsigned long kind)
    {
        switch (kind)
        {
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
    : QWidget(parent)
{
    initializeUi();
    initializeConnections();
    refreshAllSnapshotsAsync(false);
}

void NetworkAuditPage::initializeUi()
{
    m_rootLayout = new QVBoxLayout(this);
    m_rootLayout->setContentsMargins(6, 6, 6, 6);
    m_rootLayout->setSpacing(6);

    m_headerLayout = new QHBoxLayout();
    m_headerLayout->setContentsMargins(0, 0, 0, 0);
    m_headerLayout->setSpacing(6);

    QLabel* titleLabel = new QLabel(QStringLiteral("网络只读审计"), this);
    titleLabel->setStyleSheet(QStringLiteral("font-size:16px;font-weight:700;color:%1;").arg(KswordTheme::TextPrimaryHex()));
    m_headerLayout->addWidget(titleLabel);

    m_statusLabel = new QLabel(QStringLiteral("状态：等待刷新"), this);
    m_statusLabel->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
    m_headerLayout->addWidget(m_statusLabel, 1);

    m_refreshButton = new QPushButton(QStringLiteral("刷新"), this);
    m_refreshButton->setIcon(QIcon(QStringLiteral(":/Icon/process_refresh.svg")));
    m_headerLayout->addWidget(m_refreshButton);

    m_rootLayout->addLayout(m_headerLayout);

    m_sectionTabWidget = new QTabWidget(this);
    m_sectionTabWidget->setTabPosition(QTabWidget::North);
    m_rootLayout->addWidget(m_sectionTabWidget, 1);

    // TCP/UDP cross-view。
    m_crossViewPage = new QWidget(this);
    QVBoxLayout* crossLayout = new QVBoxLayout(m_crossViewPage);
    crossLayout->setContentsMargins(4, 4, 4, 4);
    crossLayout->setSpacing(6);
    m_crossViewSplitter = new QSplitter(Qt::Vertical, m_crossViewPage);
    m_crossViewTopSplitter = new QSplitter(Qt::Horizontal, m_crossViewSplitter);

    m_tcpTable = new ks::ui::VisibleTableWidget(m_crossViewTopSplitter);
    m_tcpTable->setColumnCount(6);
    m_tcpTable->setHorizontalHeaderLabels({
        QStringLiteral("PID"),
        QStringLiteral("进程"),
        QStringLiteral("本地端点"),
        QStringLiteral("远端端点"),
        QStringLiteral("状态"),
        QStringLiteral("来源/明细")
    });
    m_tcpTable->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_tcpTable->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_tcpTable->setSelectionMode(QAbstractItemView::SingleSelection);
    m_tcpTable->verticalHeader()->setVisible(false);
    m_tcpTable->horizontalHeader()->setStretchLastSection(true);
    m_tcpTable->horizontalHeader()->setSectionResizeMode(QHeaderView::Stretch);
    installCopyMenu(m_tcpTable);

    m_udpTable = new ks::ui::VisibleTableWidget(m_crossViewTopSplitter);
    m_udpTable->setColumnCount(5);
    m_udpTable->setHorizontalHeaderLabels({
        QStringLiteral("PID"),
        QStringLiteral("进程"),
        QStringLiteral("本地端点"),
        QStringLiteral("来源"),
        QStringLiteral("明细")
    });
    m_udpTable->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_udpTable->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_udpTable->setSelectionMode(QAbstractItemView::SingleSelection);
    m_udpTable->verticalHeader()->setVisible(false);
    m_udpTable->horizontalHeader()->setStretchLastSection(true);
    m_udpTable->horizontalHeader()->setSectionResizeMode(QHeaderView::Stretch);
    installCopyMenu(m_udpTable);

    m_crossSummaryTable = new ks::ui::VisibleTableWidget(m_crossViewSplitter);
    m_crossSummaryTable->setColumnCount(5);
    m_crossSummaryTable->setHorizontalHeaderLabels({ QStringLiteral("PID"), QStringLiteral("进程"), QStringLiteral("TCP"), QStringLiteral("UDP"), QStringLiteral("摘要") });
    m_crossSummaryTable->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_crossSummaryTable->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_crossSummaryTable->setSelectionMode(QAbstractItemView::SingleSelection);
    m_crossSummaryTable->verticalHeader()->setVisible(false);
    m_crossSummaryTable->horizontalHeader()->setStretchLastSection(true);
    m_crossSummaryTable->horizontalHeader()->setSectionResizeMode(QHeaderView::Stretch);
    installCopyMenu(m_crossSummaryTable);

    m_crossViewSplitter->addWidget(m_crossViewTopSplitter);
    m_crossViewSplitter->addWidget(m_crossSummaryTable);
    m_crossViewSplitter->setStretchFactor(0, 3);
    m_crossViewSplitter->setStretchFactor(1, 2);
    crossLayout->addWidget(m_crossViewSplitter, 1);
    m_sectionTabWidget->addTab(m_crossViewPage, QStringLiteral("TCP/UDP Cross-View"));

    // AFD。
    m_afdPage = new QWidget(this);
    QVBoxLayout* afdLayout = new QVBoxLayout(m_afdPage);
    afdLayout->setContentsMargins(4, 4, 4, 4);
    afdLayout->setSpacing(6);
    m_afdTable = new ks::ui::VisibleTableWidget(m_afdPage);
    m_afdTable->setColumnCount(8);
    m_afdTable->setHorizontalHeaderLabels({
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
    installCopyMenu(m_afdTable);
    afdLayout->addWidget(m_afdTable, 1);
    m_sectionTabWidget->addTab(m_afdPage, QStringLiteral("AFD"));

    // WFP。
    m_wfpPage = new QWidget(this);
    QVBoxLayout* wfpLayout = new QVBoxLayout(m_wfpPage);
    wfpLayout->setContentsMargins(4, 4, 4, 4);
    wfpLayout->setSpacing(6);
    m_wfpTabWidget = new QTabWidget(m_wfpPage);
    m_wfpTabWidget->setTabPosition(QTabWidget::North);
    wfpLayout->addWidget(m_wfpTabWidget, 1);

    auto buildWfpTable = [](QWidget* parent, const QStringList& headers) -> QTableWidget*
    {
        QTableWidget* table = new ks::ui::VisibleTableWidget(parent);
        table->setColumnCount(headers.size());
        table->setHorizontalHeaderLabels(headers);
        table->setEditTriggers(QAbstractItemView::NoEditTriggers);
        table->setSelectionBehavior(QAbstractItemView::SelectRows);
        table->setSelectionMode(QAbstractItemView::SingleSelection);
        table->verticalHeader()->setVisible(false);
        table->horizontalHeader()->setStretchLastSection(true);
        table->horizontalHeader()->setSectionResizeMode(QHeaderView::Stretch);
        installCopyMenu(table);
        return table;
    };
    m_wfpProviderTable = buildWfpTable(m_wfpPage, { QStringLiteral("名称"), QStringLiteral("描述"), QStringLiteral("GUID"), QStringLiteral("Flags"), QStringLiteral("Service"), QStringLiteral("数据大小") });
    m_wfpSubLayerTable = buildWfpTable(m_wfpPage, { QStringLiteral("名称"), QStringLiteral("描述"), QStringLiteral("GUID"), QStringLiteral("Flags"), QStringLiteral("Provider"), QStringLiteral("Weight") });
    m_wfpCalloutTable = buildWfpTable(m_wfpPage, { QStringLiteral("名称"), QStringLiteral("描述"), QStringLiteral("GUID"), QStringLiteral("Flags"), QStringLiteral("Provider"), QStringLiteral("Layer"), QStringLiteral("CalloutId") });
    m_wfpFilterTable = buildWfpTable(m_wfpPage, { QStringLiteral("名称"), QStringLiteral("描述"), QStringLiteral("GUID"), QStringLiteral("Flags"), QStringLiteral("Provider"), QStringLiteral("Layer"), QStringLiteral("Sublayer"), QStringLiteral("Weight"), QStringLiteral("Action"), QStringLiteral("Conditions"), QStringLiteral("FilterId") });
    m_wfpTabWidget->addTab(m_wfpProviderTable, QStringLiteral("Provider"));
    m_wfpTabWidget->addTab(m_wfpSubLayerTable, QStringLiteral("Sublayer"));
    m_wfpTabWidget->addTab(m_wfpCalloutTable, QStringLiteral("Callout"));
    m_wfpTabWidget->addTab(m_wfpFilterTable, QStringLiteral("Filter"));
    m_sectionTabWidget->addTab(m_wfpPage, QStringLiteral("WFP"));

    // NDIS。
    m_ndisPage = new QWidget(this);
    QVBoxLayout* ndisLayout = new QVBoxLayout(m_ndisPage);
    ndisLayout->setContentsMargins(4, 4, 4, 4);
    ndisLayout->setSpacing(6);
    m_ndisTabWidget = new QTabWidget(m_ndisPage);
    m_ndisTabWidget->setTabPosition(QTabWidget::North);
    ndisLayout->addWidget(m_ndisTabWidget, 1);
    m_ndisAdapterTable = buildWfpTable(m_ndisPage, { QStringLiteral("名称"), QStringLiteral("描述"), QStringLiteral("IfIndex"), QStringLiteral("状态"), QStringLiteral("MAC"), QStringLiteral("速率"), QStringLiteral("连接") });
    m_ndisBindingTable = buildWfpTable(m_ndisPage, { QStringLiteral("网卡"), QStringLiteral("显示名"), QStringLiteral("ComponentId"), QStringLiteral("启用"), QStringLiteral("InstanceId") });
    m_ndisProtocolTable = buildWfpTable(m_ndisPage, { QStringLiteral("别名"), QStringLiteral("IfIndex"), QStringLiteral("地址族"), QStringLiteral("连接"), QStringLiteral("Metric"), QStringLiteral("MTU") });
    m_ndisTabWidget->addTab(m_ndisAdapterTable, QStringLiteral("Miniport"));
    m_ndisTabWidget->addTab(m_ndisBindingTable, QStringLiteral("Binding"));
    m_ndisTabWidget->addTab(m_ndisProtocolTable, QStringLiteral("Protocol"));
    m_sectionTabWidget->addTab(m_ndisPage, QStringLiteral("NDIS"));

    // NSI。
    m_nsiPage = new QWidget(this);
    QVBoxLayout* nsiLayout = new QVBoxLayout(m_nsiPage);
    nsiLayout->setContentsMargins(4, 4, 4, 4);
    nsiLayout->setSpacing(6);
    m_nsiSummaryTable = new ks::ui::VisibleTableWidget(m_nsiPage);
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
    m_sectionTabWidget->addTab(m_nsiPage, QStringLiteral("NSI"));
}

void NetworkAuditPage::initializeConnections()
{
    connect(m_refreshButton, &QPushButton::clicked, this, [this]()
    {
        refreshAllSnapshotsAsync(true);
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

    QPointer<NetworkAuditPage> safeThis(this);
    std::thread([safeThis]()
    {
        const AuditSnapshot snapshot = buildAuditSnapshot();
        if (safeThis.isNull())
        {
            return;
        }

        const bool invokeOk = QMetaObject::invokeMethod(safeThis.data(), [safeThis, snapshot]()
        {
            if (safeThis.isNull())
            {
                return;
            }
            safeThis->applySnapshot(snapshot);
            if (safeThis->m_refreshButton != nullptr)
            {
                safeThis->m_refreshButton->setEnabled(true);
            }
            safeThis->m_refreshInProgress.store(false);
        }, Qt::QueuedConnection);
        if (!invokeOk && !safeThis.isNull())
        {
            safeThis->m_refreshInProgress.store(false);
            if (safeThis->m_refreshButton != nullptr)
            {
                safeThis->m_refreshButton->setEnabled(true);
            }
        }
    }).detach();
}

NetworkAuditPage::AuditSnapshot NetworkAuditPage::buildAuditSnapshot()
{
    AuditSnapshot snapshot;

    // TCP/UDP cross-view：先分别枚举，再按 PID 聚合。
    std::vector<ks::network::TcpConnectionRecord> tcpRecords;
    std::vector<ks::network::UdpEndpointRecord> udpRecords;
    std::string tcpError;
    std::string udpError;
    const bool tcpOk = ks::network::EnumerateTcpConnectionRecords(tcpRecords, &tcpError);
    const bool udpOk = ks::network::EnumerateUdpEndpointRecords(udpRecords, &udpError);

    std::unordered_map<std::uint32_t, CrossViewRow> crossMap;
    snapshot.tcpEndpointRows.reserve(tcpRecords.size());
    for (const ks::network::TcpConnectionRecord& tcpRecord : tcpRecords)
    {
        CrossViewRow& row = crossMap[tcpRecord.processId];
        row.processId = tcpRecord.processId;
        row.processName = QString::fromUtf8(tcpRecord.processName.c_str());
        ++row.tcpCount;
        row.tcpSummary = joinCompactLines({
            row.tcpSummary,
            QStringLiteral("%1:%2 -> %3:%4 (%5)")
            .arg(QString::fromUtf8(tcpRecord.localAddressText.c_str()))
            .arg(tcpRecord.localPort)
            .arg(QString::fromUtf8(tcpRecord.remoteAddressText.c_str()))
            .arg(tcpRecord.remotePort)
            .arg(QString::fromUtf8(tcpRecord.tcpStateText.c_str()))
        });

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
        snapshot.tcpEndpointRows.push_back(std::move(endpointRow));
    }
    snapshot.udpEndpointRows.reserve(udpRecords.size());
    for (const ks::network::UdpEndpointRecord& udpRecord : udpRecords)
    {
        CrossViewRow& row = crossMap[udpRecord.processId];
        row.processId = udpRecord.processId;
        row.processName = QString::fromUtf8(udpRecord.processName.c_str());
        ++row.udpCount;
        row.udpSummary = joinCompactLines({
            row.udpSummary,
            QStringLiteral("%1:%2")
            .arg(QString::fromUtf8(udpRecord.localAddressText.c_str()))
            .arg(udpRecord.localPort)
        });

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
        snapshot.crossViewRows.push_back(pair.second);
    }
    std::sort(snapshot.crossViewRows.begin(), snapshot.crossViewRows.end(), [](const CrossViewRow& left, const CrossViewRow& right)
    {
        if (left.processId != right.processId)
        {
            return left.processId < right.processId;
        }
        return left.processName < right.processName;
    });

    // AFD：先基于系统句柄做只读枚举，再筛选 AFD 相关对象。
    ks::file::HandleSnapshotOptions handleOptions;
    handleOptions.resolveObjectName = true;
    handleOptions.nameResolveBudget = 220;
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
            row.truncatedText = r0AuditTruncatedText(result) == QStringLiteral("true")
                ? QStringLiteral("是，结果可能未完整返回")
                : QStringLiteral("否");
            row.messageText = ioMessageToText(result.io.message);
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
            for (const KSWORD_ARK_NETWORK_ENDPOINT_ROW& entry : result.entries)
            {
                const QString detailText = QStringLiteral(
                    "来源=R0 endpoint；rowId=%1；protocol=%2；AF=%3；compartment=%4；ifIndex=%5；"
                    "flags=%6；sourceFlags=%7；fieldMask=%8；endpointObject=%9；owningProcessObject=%10；"
                    "transportObject=%11；interfaceLuid=%12")
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
            for (const KSWORD_ARK_NETWORK_WFP_INVENTORY_ROW& entry : result.entries)
            {
                const QString ownerModuleText = fixedNetworkWideText(entry.ownerModule, KSWORD_ARK_NETWORK_NAME_CHARS, QStringLiteral("<owner unknown>"));
                const QString detailText = QStringLiteral(
                    "来源=R0 WFP；kind=%1；rowId=%2；flags=%3；fieldMask=%4；layerId=%5；calloutId=%6；"
                    "object=%7；classify=%8；notify=%9；flowDelete=%10；ownerBase=%11；ownerModule=%12")
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
            for (const KSWORD_ARK_NETWORK_NDIS_CHAIN_ROW& entry : result.entries)
            {
                const QString componentText = fixedNetworkWideText(entry.componentName, KSWORD_ARK_NETWORK_NAME_CHARS, QStringLiteral("<component unknown>"));
                const QString ownerModuleText = fixedNetworkWideText(entry.ownerModule, KSWORD_ARK_NETWORK_NAME_CHARS, QStringLiteral("<owner unknown>"));
                const QString kindText = r0NdisObjectKindText(entry.objectKind);
                const QString detailText = QStringLiteral(
                    "来源=R0 NDIS；kind=%1；rowId=%2；flags=%3；fieldMask=%4；ifIndex=%5；filterOrder=%6；"
                    "adapterLuid=%7；object=%8；parent=%9；driverObject=%10；imageBase=%11；ownerModule=%12")
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
                else
                {
                    NdisBindingRow row;
                    row.adapterNameText = componentText;
                    row.displayNameText = kindText;
                    row.componentIdText = ownerModuleText;
                    row.enabledText = QStringLiteral("flags=%1").arg(r0Hex32(entry.flags));
                    row.instanceIdText = detailText;
                    snapshot.ndisBindingRows.push_back(std::move(row));
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

void NetworkAuditPage::applySnapshot(const AuditSnapshot& snapshot)
{
    refreshCrossViewTable(snapshot);
    refreshAfdTable(snapshot.afdRows);
    refreshWfpTables(snapshot);
    refreshNdisTables(snapshot);
    refreshNsiSummaryTable(snapshot);

    if (m_statusLabel != nullptr)
    {
        m_statusLabel->setText(snapshot.statusText);
        m_statusLabel->setToolTip(snapshot.detailText);
    }
}

void NetworkAuditPage::refreshCrossViewTable(const AuditSnapshot& snapshot)
{
    if (m_tcpTable != nullptr)
    {
        m_tcpTable->setRowCount(static_cast<int>(snapshot.tcpEndpointRows.size()));
        int rowIndex = 0;
        for (const TcpEndpointRow& row : snapshot.tcpEndpointRows)
        {
            m_tcpTable->setItem(rowIndex, 0, createReadOnlyCell(QString::number(row.processId)));
            m_tcpTable->setItem(rowIndex, 1, createReadOnlyCell(row.processName));
            m_tcpTable->setItem(rowIndex, 2, createReadOnlyCell(row.localEndpointText));
            m_tcpTable->setItem(rowIndex, 3, createReadOnlyCell(row.remoteEndpointText));
            m_tcpTable->setItem(rowIndex, 4, createReadOnlyCell(row.stateText));
            m_tcpTable->setItem(rowIndex, 5, createReadOnlyCell(row.detailText));
            ++rowIndex;
        }
    }

    if (m_udpTable != nullptr)
    {
        m_udpTable->setRowCount(static_cast<int>(snapshot.udpEndpointRows.size()));
        int rowIndex = 0;
        for (const UdpEndpointRow& row : snapshot.udpEndpointRows)
        {
            m_udpTable->setItem(rowIndex, 0, createReadOnlyCell(QString::number(row.processId)));
            m_udpTable->setItem(rowIndex, 1, createReadOnlyCell(row.processName));
            m_udpTable->setItem(rowIndex, 2, createReadOnlyCell(row.localEndpointText));
            m_udpTable->setItem(rowIndex, 3, createReadOnlyCell(row.sourceText));
            m_udpTable->setItem(rowIndex, 4, createReadOnlyCell(row.detailText));
            ++rowIndex;
        }
    }

    if (m_crossSummaryTable != nullptr)
    {
        m_crossSummaryTable->setRowCount(static_cast<int>(snapshot.crossViewRows.size()));
        int rowIndex = 0;
        for (const CrossViewRow& row : snapshot.crossViewRows)
        {
            m_crossSummaryTable->setItem(rowIndex, 0, createReadOnlyCell(QString::number(row.processId)));
            m_crossSummaryTable->setItem(rowIndex, 1, createReadOnlyCell(row.processName));
            m_crossSummaryTable->setItem(rowIndex, 2, createReadOnlyCell(QString::number(row.tcpCount)));
            m_crossSummaryTable->setItem(rowIndex, 3, createReadOnlyCell(QString::number(row.udpCount)));
            m_crossSummaryTable->setItem(rowIndex, 4, createReadOnlyCell(QStringLiteral("TCP: %1 | UDP: %2")
                .arg(row.tcpSummary.isEmpty() ? QStringLiteral("<无>") : row.tcpSummary)
                .arg(row.udpSummary.isEmpty() ? QStringLiteral("<无>") : row.udpSummary)));
            ++rowIndex;
        }
    }
}

void NetworkAuditPage::refreshAfdTable(const std::vector<AfdHandleRow>& snapshot)
{
    if (m_afdTable == nullptr)
    {
        return;
    }
    m_afdTable->setRowCount(static_cast<int>(snapshot.size()));
    int rowIndex = 0;
    for (const AfdHandleRow& row : snapshot)
    {
        m_afdTable->setItem(rowIndex, 0, createReadOnlyCell(QString::number(row.processId)));
        m_afdTable->setItem(rowIndex, 1, createReadOnlyCell(row.processName));
        m_afdTable->setItem(rowIndex, 2, createReadOnlyCell(row.handleValueText));
        m_afdTable->setItem(rowIndex, 3, createReadOnlyCell(row.typeName));
        m_afdTable->setItem(rowIndex, 4, createReadOnlyCell(row.objectName));
        m_afdTable->setItem(rowIndex, 5, createReadOnlyCell(row.sourceText));
        m_afdTable->setItem(rowIndex, 6, createReadOnlyCell(row.diffText));
        m_afdTable->setItem(rowIndex, 7, createReadOnlyCell(row.detailText + QStringLiteral(" | access=") + row.accessText));
        ++rowIndex;
    }
}

void NetworkAuditPage::refreshWfpTables(const AuditSnapshot& snapshot)
{
    auto fillTable = [](QTableWidget* tableWidget, const auto& rows, const auto& writer)
    {
        if (tableWidget == nullptr)
        {
            return;
        }
        tableWidget->setRowCount(static_cast<int>(rows.size()));
        int rowIndex = 0;
        for (const auto& row : rows)
        {
            writer(tableWidget, rowIndex, row);
            ++rowIndex;
        }
    };

    fillTable(m_wfpProviderTable, snapshot.wfpProviderRows, [](QTableWidget* tableWidget, int rowIndex, const WfpProviderRow& row)
    {
        tableWidget->setItem(rowIndex, 0, createReadOnlyCell(row.nameText));
        tableWidget->setItem(rowIndex, 1, createReadOnlyCell(row.descriptionText));
        tableWidget->setItem(rowIndex, 2, createReadOnlyCell(row.guidText));
        tableWidget->setItem(rowIndex, 3, createReadOnlyCell(row.flagsText));
        tableWidget->setItem(rowIndex, 4, createReadOnlyCell(row.serviceNameText));
        tableWidget->setItem(rowIndex, 5, createReadOnlyCell(row.dataSizeText));
    });

    fillTable(m_wfpSubLayerTable, snapshot.wfpSubLayerRows, [](QTableWidget* tableWidget, int rowIndex, const WfpSubLayerRow& row)
    {
        tableWidget->setItem(rowIndex, 0, createReadOnlyCell(row.nameText));
        tableWidget->setItem(rowIndex, 1, createReadOnlyCell(row.descriptionText));
        tableWidget->setItem(rowIndex, 2, createReadOnlyCell(row.guidText));
        tableWidget->setItem(rowIndex, 3, createReadOnlyCell(row.flagsText));
        tableWidget->setItem(rowIndex, 4, createReadOnlyCell(row.providerGuidText));
        tableWidget->setItem(rowIndex, 5, createReadOnlyCell(row.weightText));
    });

    fillTable(m_wfpCalloutTable, snapshot.wfpCalloutRows, [](QTableWidget* tableWidget, int rowIndex, const WfpCalloutRow& row)
    {
        tableWidget->setItem(rowIndex, 0, createReadOnlyCell(row.nameText));
        tableWidget->setItem(rowIndex, 1, createReadOnlyCell(row.descriptionText));
        tableWidget->setItem(rowIndex, 2, createReadOnlyCell(row.guidText));
        tableWidget->setItem(rowIndex, 3, createReadOnlyCell(row.flagsText));
        tableWidget->setItem(rowIndex, 4, createReadOnlyCell(row.providerGuidText));
        tableWidget->setItem(rowIndex, 5, createReadOnlyCell(row.layerGuidText));
        tableWidget->setItem(rowIndex, 6, createReadOnlyCell(row.calloutIdText));
    });

    fillTable(m_wfpFilterTable, snapshot.wfpFilterRows, [](QTableWidget* tableWidget, int rowIndex, const WfpFilterRow& row)
    {
        tableWidget->setItem(rowIndex, 0, createReadOnlyCell(row.nameText));
        tableWidget->setItem(rowIndex, 1, createReadOnlyCell(row.descriptionText));
        tableWidget->setItem(rowIndex, 2, createReadOnlyCell(row.guidText));
        tableWidget->setItem(rowIndex, 3, createReadOnlyCell(row.flagsText));
        tableWidget->setItem(rowIndex, 4, createReadOnlyCell(row.providerGuidText));
        tableWidget->setItem(rowIndex, 5, createReadOnlyCell(row.layerGuidText));
        tableWidget->setItem(rowIndex, 6, createReadOnlyCell(row.subLayerGuidText));
        tableWidget->setItem(rowIndex, 7, createReadOnlyCell(row.weightText));
        tableWidget->setItem(rowIndex, 8, createReadOnlyCell(row.actionText));
        tableWidget->setItem(rowIndex, 9, createReadOnlyCell(row.conditionText));
        tableWidget->setItem(rowIndex, 10, createReadOnlyCell(row.filterIdText));
    });
}

void NetworkAuditPage::refreshNdisTables(const AuditSnapshot& snapshot)
{
    auto fillTable = [](QTableWidget* tableWidget, const auto& rows, const auto& writer)
    {
        if (tableWidget == nullptr)
        {
            return;
        }
        tableWidget->setRowCount(static_cast<int>(rows.size()));
        int rowIndex = 0;
        for (const auto& row : rows)
        {
            writer(tableWidget, rowIndex, row);
            ++rowIndex;
        }
    };

    fillTable(m_ndisAdapterTable, snapshot.ndisAdapterRows, [](QTableWidget* tableWidget, int rowIndex, const NdisAdapterRow& row)
    {
        tableWidget->setItem(rowIndex, 0, createReadOnlyCell(row.nameText));
        tableWidget->setItem(rowIndex, 1, createReadOnlyCell(row.descriptionText));
        tableWidget->setItem(rowIndex, 2, createReadOnlyCell(row.ifIndexText));
        tableWidget->setItem(rowIndex, 3, createReadOnlyCell(row.statusText));
        tableWidget->setItem(rowIndex, 4, createReadOnlyCell(row.macText));
        tableWidget->setItem(rowIndex, 5, createReadOnlyCell(row.linkSpeedText));
        tableWidget->setItem(rowIndex, 6, createReadOnlyCell(row.connectionStateText));
    });

    fillTable(m_ndisBindingTable, snapshot.ndisBindingRows, [](QTableWidget* tableWidget, int rowIndex, const NdisBindingRow& row)
    {
        tableWidget->setItem(rowIndex, 0, createReadOnlyCell(row.adapterNameText));
        tableWidget->setItem(rowIndex, 1, createReadOnlyCell(row.displayNameText));
        tableWidget->setItem(rowIndex, 2, createReadOnlyCell(row.componentIdText));
        tableWidget->setItem(rowIndex, 3, createReadOnlyCell(row.enabledText));
        tableWidget->setItem(rowIndex, 4, createReadOnlyCell(row.instanceIdText));
    });

    fillTable(m_ndisProtocolTable, snapshot.ndisProtocolRows, [](QTableWidget* tableWidget, int rowIndex, const NdisProtocolRow& row)
    {
        tableWidget->setItem(rowIndex, 0, createReadOnlyCell(row.interfaceAliasText));
        tableWidget->setItem(rowIndex, 1, createReadOnlyCell(row.ifIndexText));
        tableWidget->setItem(rowIndex, 2, createReadOnlyCell(row.addressFamilyText));
        tableWidget->setItem(rowIndex, 3, createReadOnlyCell(row.connectionStateText));
        tableWidget->setItem(rowIndex, 4, createReadOnlyCell(row.interfaceMetricText));
        tableWidget->setItem(rowIndex, 5, createReadOnlyCell(row.mtuText));
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
