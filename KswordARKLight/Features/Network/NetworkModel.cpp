#include "NetworkModel.h"

#include "../../../Ksword5.1/Ksword5.1/ArkDriverClient/ArkDriverClient.h"
#include "../../Core/Win32Lean.h"

#include <commctrl.h>
#include <iphlpapi.h>

#include <algorithm>
#include <cstdint>
#include <iomanip>
#include <sstream>
#include <string>
#include <vector>

namespace Ksword::Features::Network {
namespace {
constexpr ULONG kNetworkAfInet = 2UL;

// Column 创建一个 Network 表格列。
// 输入：列标题、宽度和 ListView 对齐方式。
// 处理：只打包 UI 元数据，不访问 R0。
// 返回：NetworkAuditColumn 值对象。
NetworkAuditColumn Column(const wchar_t* title, const int width, const int format = LVCFMT_LEFT) {
    return NetworkAuditColumn{ width, format, title };
}

// RowVec 创建一行动态文本。
// 输入：已经格式化好的单元格数组。
// 处理：移动到 NetworkAuditRow，避免 const wchar_t* 生命周期问题。
// 返回：NetworkAuditRow 值对象。
NetworkAuditRow RowVec(std::vector<std::wstring> cells) {
    NetworkAuditRow row;
    row.cells = std::move(cells);
    return row;
}

// Utf8ToWideLossy 把 ArkDriverClient 的窄字符诊断转换为宽字符。
// 输入：通常是 ASCII/UTF-8 风格的 io.message。
// 处理：逐字节提升，诊断文本只用于表格展示。
// 返回：std::wstring；空输入返回空字符串。
std::wstring Utf8ToWideLossy(const std::string& text) {
    std::wstring wide;
    wide.reserve(text.size());
    for (const char ch : text) {
        wide.push_back(static_cast<unsigned char>(ch));
    }
    return wide;
}

// HexText 格式化 64 位整数。
// 输入：诊断地址、标志或计数。
// 处理：统一使用 0x + 大写十六进制。
// 返回：供 UI 表格直接显示的字符串。
std::wstring HexText(const std::uint64_t value) {
    std::wostringstream stream;
    stream << L"0x" << std::hex << std::uppercase << value;
    return stream.str();
}

// Ipv4Text 将 IP Helper 返回的 IPv4 DWORD 转成点分十进制。
// 输入：addr 是 MIB_* 行中的 IPv4 地址字段。
// 处理：按 Windows IP Helper 公开表的字节顺序逐字节展开。
// 返回：可显示的 IPv4 文本。
std::wstring Ipv4Text(const DWORD addr) {
    std::wostringstream stream;
    stream << static_cast<unsigned int>(addr & 0xFFU) << L'.'
           << static_cast<unsigned int>((addr >> 8U) & 0xFFU) << L'.'
           << static_cast<unsigned int>((addr >> 16U) & 0xFFU) << L'.'
           << static_cast<unsigned int>((addr >> 24U) & 0xFFU);
    return stream.str();
}

// NetworkPortText 将网络字节序端口转成本机可读十进制。
// 输入：portValue 来自 MIB_TCPROW_OWNER_PID / MIB_UDPROW_OWNER_PID。
// 处理：只取低 16 位并交换高低字节，不依赖 ws2_32 链接库。
// 返回：端口号文本。
std::wstring NetworkPortText(const DWORD portValue) {
    const DWORD port = ((portValue & 0xFF00U) >> 8U) | ((portValue & 0x00FFU) << 8U);
    return std::to_wstring(port & 0xFFFFU);
}

// Win32StatusText 格式化 Win32/IP Helper 错误码。
// 输入：apiName 是失败 API 名，status 是返回码。
// 处理：保留十进制和十六进制，便于现场定位权限/平台差异。
// 返回：可放入表格说明列的文本。
std::wstring Win32StatusText(const wchar_t* apiName, const DWORD status) {
    std::wostringstream stream;
    stream << apiName << L" failed, status=" << status << L" (" << HexText(status) << L")";
    return stream.str();
}

// IpHelperApi 保存动态解析的 IP Helper 只读入口。
// 输入：LoadIpHelperApi 填充字段。
// 处理：NetworkModel 通过函数指针调用，避免新增 .vcxproj 链接库依赖。
// 返回：结构体本身无行为。
struct IpHelperApi {
    using GetExtendedTcpTableFn = DWORD(WINAPI*)(PVOID, PDWORD, BOOL, ULONG, TCP_TABLE_CLASS, ULONG);
    using GetExtendedUdpTableFn = DWORD(WINAPI*)(PVOID, PDWORD, BOOL, ULONG, UDP_TABLE_CLASS, ULONG);
    using GetIfTableFn = DWORD(WINAPI*)(PMIB_IFTABLE, PULONG, BOOL);
    using GetIpAddrTableFn = DWORD(WINAPI*)(PMIB_IPADDRTABLE, PULONG, BOOL);
    using GetIpForwardTableFn = DWORD(WINAPI*)(PMIB_IPFORWARDTABLE, PULONG, BOOL);

    HMODULE module = nullptr;
    GetExtendedTcpTableFn getExtendedTcpTable = nullptr;
    GetExtendedUdpTableFn getExtendedUdpTable = nullptr;
    GetIfTableFn getIfTable = nullptr;
    GetIpAddrTableFn getIpAddrTable = nullptr;
    GetIpForwardTableFn getIpForwardTable = nullptr;
};

// LoadIpHelperApi 动态加载 iphlpapi.dll。
// 输入：errorText 接收失败原因，可为空。
// 处理：解析 AFD/NSI 页面需要的 documented 只读 API。
// 返回：成功时 api 可调用，失败时页面显示 unavailable。
bool LoadIpHelperApi(IpHelperApi& api, std::wstring& errorText) {
    api.module = ::LoadLibraryW(L"iphlpapi.dll");
    if (api.module == nullptr) {
        errorText = Win32StatusText(L"LoadLibrary(iphlpapi.dll)", ::GetLastError());
        return false;
    }

    auto resolve = [&api](const char* name) -> FARPROC {
        return ::GetProcAddress(api.module, name);
    };

    api.getExtendedTcpTable = reinterpret_cast<IpHelperApi::GetExtendedTcpTableFn>(resolve("GetExtendedTcpTable"));
    api.getExtendedUdpTable = reinterpret_cast<IpHelperApi::GetExtendedUdpTableFn>(resolve("GetExtendedUdpTable"));
    api.getIfTable = reinterpret_cast<IpHelperApi::GetIfTableFn>(resolve("GetIfTable"));
    api.getIpAddrTable = reinterpret_cast<IpHelperApi::GetIpAddrTableFn>(resolve("GetIpAddrTable"));
    api.getIpForwardTable = reinterpret_cast<IpHelperApi::GetIpForwardTableFn>(resolve("GetIpForwardTable"));

    if (api.getExtendedTcpTable == nullptr ||
        api.getExtendedUdpTable == nullptr ||
        api.getIfTable == nullptr ||
        api.getIpAddrTable == nullptr ||
        api.getIpForwardTable == nullptr) {
        errorText = L"iphlpapi.dll 缺少 Network 页所需的只读枚举入口。";
        ::FreeLibrary(api.module);
        api = {};
        return false;
    }
    return true;
}

// ProtocolStatusText 返回通用协议状态文本。
// 输入：ok/unsupported 两个 ArkDriverClient 结果状态。
// 处理：区分在线、旧驱动不支持和传输失败。
// 返回：中文状态文本。
std::wstring ProtocolStatusText(const bool ok, const bool unsupported) {
    if (ok) {
        return L"OK";
    }
    return unsupported ? L"驱动不支持" : L"驱动不可用/权限不足";
}

// AddressText 格式化网络地址。
// 输入：地址族和共享协议 16 字节地址。
// 处理：IPv4 使用点分十进制，IPv6 使用压缩前十六进制组。
// 返回：可读地址；未知地址族返回 <unknown>。
std::wstring AddressText(const unsigned long family, const unsigned char bytes[16]) {
    if (family == KSWORD_ARK_NETWORK_ADDRESS_FAMILY_IPV4) {
        std::wostringstream stream;
        stream << static_cast<unsigned int>(bytes[0]) << L'.'
               << static_cast<unsigned int>(bytes[1]) << L'.'
               << static_cast<unsigned int>(bytes[2]) << L'.'
               << static_cast<unsigned int>(bytes[3]);
        return stream.str();
    }
    if (family == KSWORD_ARK_NETWORK_ADDRESS_FAMILY_IPV6) {
        std::wostringstream stream;
        stream << std::hex << std::nouppercase;
        for (int index = 0; index < 16; index += 2) {
            if (index != 0) {
                stream << L":";
            }
            const unsigned int word = (static_cast<unsigned int>(bytes[index]) << 8U) |
                static_cast<unsigned int>(bytes[index + 1]);
            stream << word;
        }
        return stream.str();
    }
    return L"<unknown>";
}

// FixedWide 读取共享协议定长宽字符字段。
// 输入：字段指针和最大字符数。
// 处理：扫描到 NUL 或边界，避免旧驱动未写 NUL 时越界。
// 返回：安全字符串，空字段返回 <empty>。
std::wstring FixedWide(const wchar_t* text, const std::size_t maxChars) {
    if (text == nullptr || maxChars == 0U) {
        return L"<empty>";
    }
    std::size_t length = 0U;
    while (length < maxChars && text[length] != L'\0') {
        ++length;
    }
    if (length == 0U) {
        return L"<empty>";
    }
    return std::wstring(text, text + length);
}

// AddEndpointRows 追加 TCP/UDP endpoint 查询结果。
// 输入：协议名、ArkDriverClient endpoint 查询结果和输出 rows。
// 处理：先写 IOCTL 状态行，再逐条写 endpoint 诊断行。
// 返回：无返回值。
void AddEndpointRows(const std::wstring& protocol, const ksword::ark::NetworkEndpointAuditResult& query, std::vector<NetworkAuditRow>& rows) {
    rows.push_back(RowVec({
        protocol,
        ProtocolStatusText(query.io.ok, query.unsupported),
        L"ArkDriverClient",
        L"IOCTL endpoint",
        L"total=" + std::to_wstring(query.totalCount) + L" returned=" + std::to_wstring(query.returnedCount),
        Utf8ToWideLossy(query.io.message),
        L"只读 endpoint 快照，不断开连接。"
    }));
    for (const KSWORD_ARK_NETWORK_ENDPOINT_ROW& entry : query.entries) {
        rows.push_back(RowVec({
            protocol,
            std::to_wstring(entry.state),
            std::to_wstring(entry.owningPid),
            AddressText(entry.addressFamily, entry.localAddress) + L":" + std::to_wstring(entry.localPort),
            AddressText(entry.addressFamily, entry.remoteAddress) + L":" + std::to_wstring(entry.remotePort),
            HexText(entry.endpointObject),
            HexText(entry.flags),
            L"source=" + HexText(entry.sourceFlags) + L" transport=" + HexText(entry.transportObject)
        }));
    }
}

// BuildTcpUdpRows 查询 TCP/UDP R0 endpoint cross-view。
// 输入：无；处理：调用 ArkDriverClient wrapper，不裸 DeviceIoControl。
// 返回：可直接显示的表格行。
std::vector<NetworkAuditRow> BuildTcpUdpRows() {
    std::vector<NetworkAuditRow> rows;
    const ksword::ark::DriverClient client;
    AddEndpointRows(L"TCP", client.queryNetworkTcpEndpoints(), rows);
    AddEndpointRows(L"UDP", client.queryNetworkUdpEndpoints(), rows);
    rows.push_back(RowVec({ L"安全边界", L"只读", L"UI", L"无断连/无阻断", L"-", L"不调用 set-rules，不修改 WFP 规则。", L"展示 R0 endpoint 审计结果，可与 R3 公开 API 视图对照。" }));
    return rows;
}

// BuildWfpRows 查询 WFP provider/filter/callout inventory。
// 输入：无；处理：调用 ArkDriverClient::queryNetworkWfpInventory。
// 返回：WFP 表格行。
std::vector<NetworkAuditRow> BuildWfpRows() {
    std::vector<NetworkAuditRow> rows;
    const ksword::ark::DriverClient client;
    const ksword::ark::NetworkWfpInventoryResult query = client.queryNetworkWfpInventory();
    rows.push_back(RowVec({
        L"IOCTL_KSWORD_ARK_NETWORK_QUERY_WFP_INVENTORY",
        ProtocolStatusText(query.io.ok, query.unsupported),
        L"ArkDriverClient",
        L"total=" + std::to_wstring(query.totalCount) + L" returned=" + std::to_wstring(query.returnedCount),
        Utf8ToWideLossy(query.io.message),
        L"不删除 callout/filter，不关闭 engine。"
    }));
    for (const KSWORD_ARK_NETWORK_WFP_INVENTORY_ROW& entry : query.entries) {
        rows.push_back(RowVec({
            std::to_wstring(entry.objectKind),
            L"layer=" + std::to_wstring(entry.layerId) + L" callout=" + std::to_wstring(entry.calloutId),
            HexText(entry.objectAddress),
            HexText(entry.classifyAddress),
            FixedWide(entry.ownerModule, KSWORD_ARK_NETWORK_NAME_CHARS),
            L"flags=" + HexText(entry.flags) + L" field=" + HexText(entry.fieldMask)
        }));
    }
    return rows;
}

// BuildNdisRows 查询 NDIS 链路审计。
// 输入：无；处理：调用 ArkDriverClient::queryNetworkNdisChain。
// 返回：NDIS 表格行。
std::vector<NetworkAuditRow> BuildNdisRows() {
    std::vector<NetworkAuditRow> rows;
    const ksword::ark::DriverClient client;
    const ksword::ark::NetworkNdisChainResult query = client.queryNetworkNdisChain();
    rows.push_back(RowVec({
        L"IOCTL_KSWORD_ARK_NETWORK_QUERY_NDIS_CHAIN",
        ProtocolStatusText(query.io.ok, query.unsupported),
        L"ArkDriverClient",
        L"total=" + std::to_wstring(query.totalCount) + L" returned=" + std::to_wstring(query.returnedCount),
        Utf8ToWideLossy(query.io.message),
        L"不 pause/restart miniport，不 detach filter。"
    }));
    for (const KSWORD_ARK_NETWORK_NDIS_CHAIN_ROW& entry : query.entries) {
        rows.push_back(RowVec({
            std::to_wstring(entry.objectKind),
            FixedWide(entry.componentName, KSWORD_ARK_NETWORK_NAME_CHARS),
            L"if=" + std::to_wstring(entry.ifIndex) + L" order=" + std::to_wstring(entry.filterOrder),
            HexText(entry.objectAddress),
            HexText(entry.parentObjectAddress),
            FixedWide(entry.ownerModule, KSWORD_ARK_NETWORK_NAME_CHARS),
            L"driver=" + HexText(entry.driverObject) + L" flags=" + HexText(entry.flags)
        }));
    }
    return rows;
}

// AppendTcpAfdProjectionRows 追加 TCP owner table 的 AFD-facing 投影。
// 输入：已解析 IP Helper API、输出行数组和最大返回行数。
// 处理：只调用 documented GetExtendedTcpTable，不读 AFD 私有结构。
// 返回：无返回值；失败时写入诊断行。
void AppendTcpAfdProjectionRows(const IpHelperApi& api, std::vector<NetworkAuditRow>& rows, const std::size_t maxRows) {
    DWORD bufferSize = 0UL;
    DWORD status = api.getExtendedTcpTable(nullptr, &bufferSize, FALSE, kNetworkAfInet, TCP_TABLE_OWNER_PID_ALL, 0UL);
    if (status != ERROR_INSUFFICIENT_BUFFER || bufferSize == 0UL) {
        rows.push_back(RowVec({ L"AFD/TCPv4", L"Unavailable", L"GetExtendedTcpTable", L"-", Win32StatusText(L"GetExtendedTcpTable(size)", status), L"只读；未读取 AFD 私有对象。" }));
        return;
    }

    std::vector<unsigned char> buffer(bufferSize, 0U);
    status = api.getExtendedTcpTable(buffer.data(), &bufferSize, FALSE, kNetworkAfInet, TCP_TABLE_OWNER_PID_ALL, 0UL);
    if (status != ERROR_SUCCESS) {
        rows.push_back(RowVec({ L"AFD/TCPv4", L"Unavailable", L"GetExtendedTcpTable", L"-", Win32StatusText(L"GetExtendedTcpTable(data)", status), L"只读；未读取 AFD 私有对象。" }));
        return;
    }

    const auto* table = reinterpret_cast<const MIB_TCPTABLE_OWNER_PID*>(buffer.data());
    rows.push_back(RowVec({ L"AFD/TCPv4", L"OK", L"GetExtendedTcpTable", L"Owner PID table", L"entries=" + std::to_wstring(table->dwNumEntries), L"R3 documented projection，可与 R0 TCP endpoint 交叉验证。" }));
    const std::size_t count = (std::min)(static_cast<std::size_t>(table->dwNumEntries), maxRows);
    for (std::size_t index = 0U; index < count; ++index) {
        const MIB_TCPROW_OWNER_PID& entry = table->table[index];
        rows.push_back(RowVec({
            L"TCP",
            L"state=" + std::to_wstring(entry.dwState),
            L"pid=" + std::to_wstring(entry.dwOwningPid),
            Ipv4Text(entry.dwLocalAddr) + L":" + NetworkPortText(entry.dwLocalPort),
            Ipv4Text(entry.dwRemoteAddr) + L":" + NetworkPortText(entry.dwRemotePort),
            L"AFD-facing R3 endpoint; no disconnect/no patch"
        }));
    }
}

// AppendUdpAfdProjectionRows 追加 UDP owner table 的 AFD-facing 投影。
// 输入：已解析 IP Helper API、输出行数组和最大返回行数。
// 处理：只调用 documented GetExtendedUdpTable，不读取私有内核对象。
// 返回：无返回值；失败时写入诊断行。
void AppendUdpAfdProjectionRows(const IpHelperApi& api, std::vector<NetworkAuditRow>& rows, const std::size_t maxRows) {
    DWORD bufferSize = 0UL;
    DWORD status = api.getExtendedUdpTable(nullptr, &bufferSize, FALSE, kNetworkAfInet, UDP_TABLE_OWNER_PID, 0UL);
    if (status != ERROR_INSUFFICIENT_BUFFER || bufferSize == 0UL) {
        rows.push_back(RowVec({ L"AFD/UDPv4", L"Unavailable", L"GetExtendedUdpTable", L"-", Win32StatusText(L"GetExtendedUdpTable(size)", status), L"只读；未读取 AFD 私有对象。" }));
        return;
    }

    std::vector<unsigned char> buffer(bufferSize, 0U);
    status = api.getExtendedUdpTable(buffer.data(), &bufferSize, FALSE, kNetworkAfInet, UDP_TABLE_OWNER_PID, 0UL);
    if (status != ERROR_SUCCESS) {
        rows.push_back(RowVec({ L"AFD/UDPv4", L"Unavailable", L"GetExtendedUdpTable", L"-", Win32StatusText(L"GetExtendedUdpTable(data)", status), L"只读；未读取 AFD 私有对象。" }));
        return;
    }

    const auto* table = reinterpret_cast<const MIB_UDPTABLE_OWNER_PID*>(buffer.data());
    rows.push_back(RowVec({ L"AFD/UDPv4", L"OK", L"GetExtendedUdpTable", L"Owner PID table", L"entries=" + std::to_wstring(table->dwNumEntries), L"R3 documented projection，可与 R0 UDP endpoint 交叉验证。" }));
    const std::size_t count = (std::min)(static_cast<std::size_t>(table->dwNumEntries), maxRows);
    for (std::size_t index = 0U; index < count; ++index) {
        const MIB_UDPROW_OWNER_PID& entry = table->table[index];
        rows.push_back(RowVec({
            L"UDP",
            L"listen",
            L"pid=" + std::to_wstring(entry.dwOwningPid),
            Ipv4Text(entry.dwLocalAddr) + L":" + NetworkPortText(entry.dwLocalPort),
            L"-",
            L"AFD-facing R3 endpoint; no disconnect/no patch"
        }));
    }
}

// BuildAfdRows 构建 AFD endpoint 页面。
// 输入：无；处理：使用 documented TCP/UDP owner table 作为 AFD socket 投影。
// 返回：实际 R3 证据行；缺 API 时返回 unavailable 而非 unsupported 占位。
std::vector<NetworkAuditRow> BuildAfdRows() {
    std::vector<NetworkAuditRow> rows;
    IpHelperApi api;
    std::wstring errorText;
    if (!LoadIpHelperApi(api, errorText)) {
        rows.push_back(RowVec({ L"AFD endpoint", L"Unavailable", L"iphlpapi.dll", L"-", errorText, L"未新增 R0 协议；不猜 AFD 私有结构。" }));
        return rows;
    }

    AppendTcpAfdProjectionRows(api, rows, 128U);
    AppendUdpAfdProjectionRows(api, rows, 128U);
    rows.push_back(RowVec({ L"安全边界", L"只读", L"R3 documented API", L"TCP/UDP owner table", L"无 AFD IOCTL 时以公开端点表替代空占位。", L"不 detach/disable/bypass，不读取 AFD 私有对象。" }));
    ::FreeLibrary(api.module);
    return rows;
}

// BuildNsiRows 构建 NSI 摘要页面。
// 输入：无；处理：使用 IP Helper 的接口、地址、路由表作为 NSI-facing 证据。
// 返回：实际摘要行；缺 API 时返回 unavailable 而非 unsupported 占位。
std::vector<NetworkAuditRow> BuildNsiRows() {
    std::vector<NetworkAuditRow> rows;
    IpHelperApi api;
    std::wstring errorText;
    if (!LoadIpHelperApi(api, errorText)) {
        rows.push_back(RowVec({ L"NSI summary", L"Unavailable", L"iphlpapi.dll", L"-", errorText, L"未新增 R0 协议；不猜 NSI 私有表。" }));
        return rows;
    }

    ULONG ifBytes = 0UL;
    DWORD status = api.getIfTable(nullptr, &ifBytes, FALSE);
    if (status == ERROR_INSUFFICIENT_BUFFER && ifBytes != 0UL) {
        std::vector<unsigned char> buffer(ifBytes, 0U);
        auto* ifTable = reinterpret_cast<PMIB_IFTABLE>(buffer.data());
        status = api.getIfTable(ifTable, &ifBytes, FALSE);
        if (status == NO_ERROR) {
            rows.push_back(RowVec({ L"Interface table", L"OK", L"GetIfTable", L"NETIO/NSI public projection", L"entries=" + std::to_wstring(ifTable->dwNumEntries), L"只读接口清单。" }));
            const DWORD count = (std::min)(ifTable->dwNumEntries, 64UL);
            for (DWORD index = 0UL; index < count; ++index) {
                const MIB_IFROW& entry = ifTable->table[index];
                rows.push_back(RowVec({
                    L"Interface",
                    L"ifType=" + std::to_wstring(entry.dwType),
                    L"ifIndex=" + std::to_wstring(entry.dwIndex),
                    std::wstring(entry.wszName),
                    L"mtu=" + std::to_wstring(entry.dwMtu) + L" speed=" + std::to_wstring(entry.dwSpeed),
                    L"documented IP Helper row"
                }));
            }
        }
        else {
            rows.push_back(RowVec({ L"Interface table", L"Unavailable", L"GetIfTable", L"-", Win32StatusText(L"GetIfTable(data)", status), L"只读失败。" }));
        }
    }
    else {
        rows.push_back(RowVec({ L"Interface table", L"Unavailable", L"GetIfTable", L"-", Win32StatusText(L"GetIfTable(size)", status), L"只读失败。" }));
    }

    ULONG addressBytes = 0UL;
    status = api.getIpAddrTable(nullptr, &addressBytes, FALSE);
    if (status == ERROR_INSUFFICIENT_BUFFER && addressBytes != 0UL) {
        std::vector<unsigned char> buffer(addressBytes, 0U);
        auto* addressTable = reinterpret_cast<PMIB_IPADDRTABLE>(buffer.data());
        status = api.getIpAddrTable(addressTable, &addressBytes, FALSE);
        if (status == NO_ERROR) {
            rows.push_back(RowVec({ L"IPv4 address table", L"OK", L"GetIpAddrTable", L"IPv4", L"entries=" + std::to_wstring(addressTable->dwNumEntries), L"只读地址摘要。" }));
        }
        else {
            rows.push_back(RowVec({ L"IPv4 address table", L"Unavailable", L"GetIpAddrTable", L"IPv4", Win32StatusText(L"GetIpAddrTable(data)", status), L"只读失败。" }));
        }
    }
    else {
        rows.push_back(RowVec({ L"IPv4 address table", L"Unavailable", L"GetIpAddrTable", L"IPv4", Win32StatusText(L"GetIpAddrTable(size)", status), L"只读失败。" }));
    }

    ULONG routeBytes = 0UL;
    status = api.getIpForwardTable(nullptr, &routeBytes, FALSE);
    if (status == ERROR_INSUFFICIENT_BUFFER && routeBytes != 0UL) {
        std::vector<unsigned char> buffer(routeBytes, 0U);
        auto* routeTable = reinterpret_cast<PMIB_IPFORWARDTABLE>(buffer.data());
        status = api.getIpForwardTable(routeTable, &routeBytes, FALSE);
        if (status == NO_ERROR) {
            rows.push_back(RowVec({ L"IPv4 route table", L"OK", L"GetIpForwardTable", L"IPv4", L"entries=" + std::to_wstring(routeTable->dwNumEntries), L"只读路由摘要。" }));
        }
        else {
            rows.push_back(RowVec({ L"IPv4 route table", L"Unavailable", L"GetIpForwardTable", L"IPv4", Win32StatusText(L"GetIpForwardTable(data)", status), L"只读失败。" }));
        }
    }
    else {
        rows.push_back(RowVec({ L"IPv4 route table", L"Unavailable", L"GetIpForwardTable", L"IPv4", Win32StatusText(L"GetIpForwardTable(size)", status), L"只读失败。" }));
    }

    rows.push_back(RowVec({ L"安全边界", L"只读", L"R3 documented API", L"IP Helper", L"无 NSI R0 IOCTL 时以公开 NETIO/NSI 投影替代空占位。", L"不读取 NSI 私有结构，不修改接口/路由。" }));
    ::FreeLibrary(api.module);
    return rows;
}

} // namespace

NetworkAuditModel::NetworkAuditModel() {
    // 构造时立即采集一次，保证首次打开就是 wrapper/R3 投影结果或明确 unavailable。
    refresh();
}

void NetworkAuditModel::refresh() {
    // refresh 的输入为空；处理是重建所有页面行数据；返回为空。
    // 所有 R0 调用均通过 ArkDriverClient wrapper 完成。
    pages_ = BuildNetworkAuditPages();
}

const std::vector<NetworkAuditPage>& NetworkAuditModel::pages() const noexcept {
    return pages_;
}

const NetworkAuditPage* NetworkAuditModel::pageAt(const int index) const noexcept {
    if (index < 0 || index >= static_cast<int>(pages_.size())) {
        return nullptr;
    }
    return &pages_[static_cast<std::size_t>(index)];
}

std::vector<NetworkAuditPage> BuildNetworkAuditPages() {
    // BuildNetworkAuditPages 负责 ARKLight Network 的真实 R0 wrapper 接入。
    // 输入为空；处理时调用只读审计 wrapper；返回页面描述数组。
    std::vector<NetworkAuditPage> pages;

    pages.push_back({
        NetworkAuditPageId::TcpUdpCrossView,
        L"TCP/UDP R0 cross-view",
        L"合并 tcpip/netio/runtime 只读 endpoint 快照；驱动未加载时显示 Win32 错误。",
        {
            Column(L"协议", 80),
            Column(L"状态", 120),
            Column(L"PID/来源", 120),
            Column(L"Local", 180),
            Column(L"Remote", 180),
            Column(L"EndpointObject", 170),
            Column(L"Flags", 120),
            Column(L"说明", 420),
        },
        BuildTcpUdpRows()
    });

    pages.push_back({
        NetworkAuditPageId::AfdEndpoint,
        L"AFD endpoint",
        L"使用 documented TCP/UDP owner table 生成 AFD-facing 端点投影；不猜 AFD 私有结构。",
        {
            Column(L"项目", 160),
            Column(L"状态", 180),
            Column(L"来源", 160),
            Column(L"协议/接口", 220),
            Column(L"说明", 420),
            Column(L"安全边界", 360),
        },
        BuildAfdRows()
    });

    pages.push_back({
        NetworkAuditPageId::WfpInventory,
        L"WFP callout/filter/provider",
        L"展示 WFP provider、sublayer、filter、callout 以及 classify/notify/flowDelete owner module。",
        {
            Column(L"ObjectKind/IOCTL", 180),
            Column(L"状态/Layer", 160),
            Column(L"Object", 170),
            Column(L"Classify", 170),
            Column(L"Owner", 220),
            Column(L"Flags/说明", 460),
        },
        BuildWfpRows()
    });

    pages.push_back({
        NetworkAuditPageId::NdisChain,
        L"NDIS protocol/filter",
        L"展示 miniport、filter、protocol、binding 的只读链路表，并保留对象地址诊断。",
        {
            Column(L"Kind/IOCTL", 160),
            Column(L"Component", 240),
            Column(L"If/Order", 140),
            Column(L"Object", 170),
            Column(L"Parent", 170),
            Column(L"Owner", 220),
            Column(L"Flags/Driver", 420),
        },
        BuildNdisRows()
    });

    pages.push_back({
        NetworkAuditPageId::NsiSummary,
        L"NSI 表",
        L"使用 IP Helper 接口/地址/路由表生成 NSI-facing 只读摘要；不猜 NSI 私有结构。",
        {
            Column(L"项目", 160),
            Column(L"状态", 180),
            Column(L"来源", 160),
            Column(L"协议/接口", 220),
            Column(L"说明", 420),
            Column(L"安全边界", 360),
        },
        BuildNsiRows()
    });

    return pages;
}

} // namespace Ksword::Features::Network
