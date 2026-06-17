#include "NetworkFirewallPage.h"

// ============================================================
// NetworkFirewallPage.cpp
// 作用：
// 1) 以 System Informer 的 fwmon/fwtab 思路为参照，展示 WFP 防火墙事件；
// 2) 动态解析 fwpuclnt.dll，支持历史枚举和实时订阅；
// 3) 只做 UI 展示和只读监控，不向 KswordARK 驱动发送 IOCTL。
// ============================================================

#include "../theme.h"
#include "../UI/GlobalDialogTheme.h"

#include <QAbstractItemView>
#include <QCheckBox>
#include <QComboBox>
#include <QDateTime>
#include <QDialog>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QHeaderView>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QMetaObject>
#include <QPointer>
#include <QPushButton>
#include <QTabWidget>
#include <QTableWidget>
#include <QTableWidgetItem>
#include <QThread>
#include <QTimer>
#include <QVBoxLayout>

#include <algorithm>
#include <array>
#include <cstring>
#include <thread>

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#include <comdef.h>
#include <fwpmu.h>
#include <fwpsu.h>
#include <netfw.h>
#include <Objbase.h>
#include <Rpc.h>
#include <Ws2tcpip.h>

#pragma comment(lib, "Ole32.lib")
#pragma comment(lib, "OleAut32.lib")
#pragma comment(lib, "Rpcrt4.lib")
#pragma comment(lib, "Ws2_32.lib")

namespace
{
    // FirewallTableColumn：
    // - 作用：定义防火墙事件表列索引；
    // - 处理逻辑：插入、过滤和高亮时统一引用；
    // - 返回行为：枚举本身无返回值。
    enum FirewallTableColumn : int
    {
        ColumnName = 0,
        ColumnAction,
        ColumnDirection,
        ColumnRule,
        ColumnDescription,
        ColumnLocalAddress,
        ColumnLocalPort,
        ColumnLocalHost,
        ColumnRemoteAddress,
        ColumnRemotePort,
        ColumnRemoteHost,
        ColumnProtocol,
        ColumnTimestamp,
        ColumnCount
    };

    // FirewallRuleTableColumn：
    // - 作用：定义规则管理表列索引；
    // - 处理逻辑：插入、过滤和启停按钮状态同步时统一引用；
    // - 返回行为：枚举本身无返回值。
    enum FirewallRuleTableColumn : int
    {
        RuleColumnName = 0,
        RuleColumnEnabled,
        RuleColumnAction,
        RuleColumnDirection,
        RuleColumnProfiles,
        RuleColumnProtocol,
        RuleColumnLocalPorts,
        RuleColumnRemotePorts,
        RuleColumnApplication,
        RuleColumnService,
        RuleColumnGrouping,
        RuleColumnDescription,
        RuleColumnCount
    };

    // WfpApi：
    // - 作用：保存动态解析出的 fwpuclnt.dll 函数指针；
    // - 处理逻辑：NetworkFirewallPage::ensureWfpApiLoaded 填充；
    // - 返回行为：纯结构体，无函数返回。
    struct WfpApi
    {
        using FwpmEngineOpen0Fn = DWORD(WINAPI*)(
            const wchar_t*,
            UINT32,
            SEC_WINNT_AUTH_IDENTITY_W*,
            const FWPM_SESSION0*,
            HANDLE*);
        using FwpmEngineClose0Fn = DWORD(WINAPI*)(HANDLE);
        using FwpmEngineSetOption0Fn = DWORD(WINAPI*)(HANDLE, FWPM_ENGINE_OPTION, const FWP_VALUE0*);
        using FwpmFreeMemory0Fn = void(WINAPI*)(void**);
        using FwpmFilterGetById0Fn = DWORD(WINAPI*)(HANDLE, UINT64, FWPM_FILTER0**);
        using FwpmNetEventCreateEnumHandle0Fn = DWORD(WINAPI*)(HANDLE, const FWPM_NET_EVENT_ENUM_TEMPLATE0*, HANDLE*);
        using FwpmNetEventDestroyEnumHandle0Fn = DWORD(WINAPI*)(HANDLE, HANDLE);
        using WfpNetEventCallbackFn = void(CALLBACK*)(void*, const void*);
        using FwpmNetEventEnumGenericFn = DWORD(WINAPI*)(HANDLE, HANDLE, UINT32, void***, UINT32*);
        using FwpmNetEventSubscribeGenericFn = DWORD(WINAPI*)(
            HANDLE,
            const FWPM_NET_EVENT_SUBSCRIPTION0*,
            WfpNetEventCallbackFn,
            void*,
            HANDLE*);
        using FwpmNetEventUnsubscribe0Fn = DWORD(WINAPI*)(HANDLE, HANDLE);

        FwpmEngineOpen0Fn engineOpen = nullptr;
        FwpmEngineClose0Fn engineClose = nullptr;
        FwpmEngineSetOption0Fn engineSetOption = nullptr;
        FwpmFreeMemory0Fn freeMemory = nullptr;
        FwpmFilterGetById0Fn filterGetById = nullptr;
        FwpmNetEventCreateEnumHandle0Fn eventCreateEnumHandle = nullptr;
        FwpmNetEventDestroyEnumHandle0Fn eventDestroyEnumHandle = nullptr;
        FwpmNetEventEnumGenericFn eventEnum = nullptr;
        FwpmNetEventSubscribeGenericFn eventSubscribe = nullptr;
        FwpmNetEventUnsubscribe0Fn eventUnsubscribe = nullptr;
    };

    WfpApi g_wfpApi;

    // ScopedBstr：
    // - 作用：托管 BSTR 生命周期，减少 SysFreeString 漏释放；
    // - 处理逻辑：构造时接收 BSTR，析构自动释放；
    // - 返回行为：可通过 get()/release() 传递底层句柄。
    class ScopedBstr final
    {
    public:
        explicit ScopedBstr(BSTR value = nullptr)
            : m_value(value)
        {
        }

        ~ScopedBstr()
        {
            if (m_value != nullptr)
            {
                SysFreeString(m_value);
                m_value = nullptr;
            }
        }

        ScopedBstr(const ScopedBstr&) = delete;
        ScopedBstr& operator=(const ScopedBstr&) = delete;

        ScopedBstr(ScopedBstr&& other) noexcept
            : m_value(other.m_value)
        {
            other.m_value = nullptr;
        }

        ScopedBstr& operator=(ScopedBstr&& other) noexcept
        {
            if (this != &other)
            {
                if (m_value != nullptr)
                {
                    SysFreeString(m_value);
                }
                m_value = other.m_value;
                other.m_value = nullptr;
            }
            return *this;
        }

        BSTR get() const
        {
            return m_value;
        }

        BSTR* put()
        {
            if (m_value != nullptr)
            {
                SysFreeString(m_value);
                m_value = nullptr;
            }
            return &m_value;
        }

        BSTR release()
        {
            BSTR value = m_value;
            m_value = nullptr;
            return value;
        }

    private:
        BSTR m_value = nullptr;
    };

    // ScopedVariant：
    // - 作用：托管 VARIANT 生命周期；
    // - 处理逻辑：构造时 VariantInit，析构时 VariantClear；
    // - 返回行为：通过 get() 暴露给 COM API。
    class ScopedVariant final
    {
    public:
        ScopedVariant()
        {
            VariantInit(&m_value);
        }

        ~ScopedVariant()
        {
            VariantClear(&m_value);
        }

        ScopedVariant(const ScopedVariant&) = delete;
        ScopedVariant& operator=(const ScopedVariant&) = delete;

        VARIANT* get()
        {
            return &m_value;
        }

        const VARIANT* get() const
        {
            return &m_value;
        }

    private:
        VARIANT m_value{};
    };

    // ScopedComInitialize：
    // - 作用：在当前线程内初始化/收束 COM；
    // - 处理逻辑：构造时 CoInitializeEx，成功时析构自动 CoUninitialize；
    // - 返回行为：通过 succeeded()/result() 暴露初始化状态。
    class ScopedComInitialize final
    {
    public:
        explicit ScopedComInitialize(const DWORD coinitFlags)
            : m_result(CoInitializeEx(nullptr, coinitFlags))
        {
            m_shouldUninitialize = SUCCEEDED(m_result);
        }

        ~ScopedComInitialize()
        {
            if (m_shouldUninitialize)
            {
                CoUninitialize();
            }
        }

        bool succeeded() const
        {
            return SUCCEEDED(m_result) || m_result == RPC_E_CHANGED_MODE;
        }

        HRESULT result() const
        {
            return m_result;
        }

    private:
        HRESULT m_result = E_FAIL;
        bool m_shouldUninitialize = false;
    };

    // releaseComPointer 作用：
    // - 输入：任意 COM 接口指针；
    // - 处理：非空时执行 Release；
    // - 返回：无。
    template <typename T>
    void releaseComPointer(T*& pointerValue)
    {
        if (pointerValue != nullptr)
        {
            pointerValue->Release();
            pointerValue = nullptr;
        }
    }

    // safeText 作用：
    // - 输入：候选文本；
    // - 处理：空文本统一显示为 '-'；
    // - 返回：可显示文本。
    QString safeText(const QString& valueText)
    {
        return valueText.trimmed().isEmpty() ? QStringLiteral("-") : valueText.trimmed();
    }

    // rawTextOrEmpty 作用：
    // - 输入：候选文本；
    // - 处理：仅做 trimmed，保留真正空字符串；
    // - 返回：适合写回规则对象的原始文本。
    QString rawTextOrEmpty(const QString& valueText)
    {
        return valueText.trimmed();
    }

    // qStringFromBstr 作用：
    // - 输入：COM BSTR；
    // - 处理：nullptr 时返回空 QString；
    // - 返回：Qt 文本。
    QString qStringFromBstr(BSTR valueText)
    {
        return valueText == nullptr ? QString() : QString::fromWCharArray(valueText);
    }

    // bstrFromQString 作用：
    // - 输入：Qt 文本；
    // - 处理：为空时返回 nullptr，否则分配 BSTR；
    // - 返回：调用方负责释放的 BSTR。
    BSTR bstrFromQString(const QString& valueText)
    {
        const QString trimmedText = valueText.trimmed();
        if (trimmedText.isEmpty())
        {
            return nullptr;
        }
        return SysAllocString(reinterpret_cast<const OLECHAR*>(trimmedText.utf16()));
    }

    // win32ErrorText 作用：
    // - 输入：Win32 错误码；
    // - 处理：调用 FormatMessage 获取系统说明；
    // - 返回：包含错误码和说明的文本。
    QString win32ErrorText(const DWORD errorCode)
    {
        wchar_t* messageBuffer = nullptr;
        const DWORD length = FormatMessageW(
            FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
            nullptr,
            errorCode,
            0,
            reinterpret_cast<LPWSTR>(&messageBuffer),
            0,
            nullptr);
        QString messageText;
        if (length > 0 && messageBuffer != nullptr)
        {
            messageText = QString::fromWCharArray(messageBuffer).trimmed();
            LocalFree(messageBuffer);
        }
        if (messageText.isEmpty())
        {
            messageText = QStringLiteral("未知错误");
        }
        return QStringLiteral("%1 (%2)").arg(messageText).arg(errorCode);
    }

    // fileTimeToText 作用：
    // - 输入：WFP 事件 FILETIME；
    // - 处理：转为本地时间；
    // - 返回：时间文本，失败时返回 '-'。
    QString fileTimeToText(const FILETIME& fileTime)
    {
        FILETIME localFileTime{};
        SYSTEMTIME systemTime{};
        if (!FileTimeToLocalFileTime(&fileTime, &localFileTime)
            || !FileTimeToSystemTime(&localFileTime, &systemTime))
        {
            return QStringLiteral("-");
        }
        return QStringLiteral("%1-%2-%3 %4:%5:%6.%7")
            .arg(systemTime.wYear, 4, 10, QLatin1Char('0'))
            .arg(systemTime.wMonth, 2, 10, QLatin1Char('0'))
            .arg(systemTime.wDay, 2, 10, QLatin1Char('0'))
            .arg(systemTime.wHour, 2, 10, QLatin1Char('0'))
            .arg(systemTime.wMinute, 2, 10, QLatin1Char('0'))
            .arg(systemTime.wSecond, 2, 10, QLatin1Char('0'))
            .arg(systemTime.wMilliseconds, 3, 10, QLatin1Char('0'));
    }

    // protocolText 作用：
    // - 输入：IP 协议号；
    // - 处理：转换常见协议；
    // - 返回：TCP/UDP/ICMP 等可读文本。
    QString protocolText(const UINT8 protocol)
    {
        switch (protocol)
        {
        case IPPROTO_TCP:
            return QStringLiteral("TCP");
        case IPPROTO_UDP:
            return QStringLiteral("UDP");
        case IPPROTO_ICMP:
            return QStringLiteral("ICMP");
        case IPPROTO_ICMPV6:
            return QStringLiteral("ICMPv6");
        default:
            return protocol == 0
                ? QStringLiteral("-")
                : QStringLiteral("%1").arg(static_cast<unsigned int>(protocol));
        }
    }

    // firewallRuleProtocolText 作用：
    // - 输入：Windows Firewall 协议值；
    // - 处理：转换为规则管理页可读文本；
    // - 返回：TCP/UDP/Any/数字协议。
    QString firewallRuleProtocolText(const long protocolValue)
    {
        switch (protocolValue)
        {
        case NET_FW_IP_PROTOCOL_TCP:
            return QStringLiteral("TCP");
        case NET_FW_IP_PROTOCOL_UDP:
            return QStringLiteral("UDP");
        case NET_FW_IP_PROTOCOL_ANY:
            return QStringLiteral("Any");
        case 1:
            return QStringLiteral("ICMPv4");
        case 58:
            return QStringLiteral("ICMPv6");
        default:
            return QString::number(protocolValue);
        }
    }

    // firewallRuleDirectionText 作用：
    // - 输入：规则方向值；
    // - 处理：转换为 In/Out；
    // - 返回：可读文本。
    QString firewallRuleDirectionText(const long directionValue)
    {
        switch (directionValue)
        {
        case NET_FW_RULE_DIR_IN:
            return QStringLiteral("In");
        case NET_FW_RULE_DIR_OUT:
            return QStringLiteral("Out");
        default:
            return QStringLiteral("Unknown");
        }
    }

    // firewallRuleActionText 作用：
    // - 输入：规则动作值；
    // - 处理：转换为 Allow/Block；
    // - 返回：可读文本。
    QString firewallRuleActionText(const long actionValue)
    {
        switch (actionValue)
        {
        case NET_FW_ACTION_ALLOW:
            return QStringLiteral("Allow");
        case NET_FW_ACTION_BLOCK:
            return QStringLiteral("Block");
        default:
            return QStringLiteral("Unknown");
        }
    }

    // firewallProfilesText 作用：
    // - 输入：Profile 位掩码；
    // - 处理：展开为 Domain/Private/Public 文本；
    // - 返回：拼接后的可读文本。
    QString firewallProfilesText(const long profilesValue)
    {
        if (profilesValue == NET_FW_PROFILE2_ALL)
        {
            return QStringLiteral("All");
        }

        QStringList profileTextList;
        if ((profilesValue & NET_FW_PROFILE2_DOMAIN) != 0)
        {
            profileTextList.push_back(QStringLiteral("Domain"));
        }
        if ((profilesValue & NET_FW_PROFILE2_PRIVATE) != 0)
        {
            profileTextList.push_back(QStringLiteral("Private"));
        }
        if ((profilesValue & NET_FW_PROFILE2_PUBLIC) != 0)
        {
            profileTextList.push_back(QStringLiteral("Public"));
        }
        return profileTextList.isEmpty()
            ? QStringLiteral("-")
            : profileTextList.join(QStringLiteral(" | "));
    }

    // composeRuleFingerprint 作用：
    // - 输入：规则关键展示字段；
    // - 处理：生成当前快照内稳定匹配键；
    // - 返回：规则匹配指纹。
    QString composeRuleFingerprint(
        const QString& nameText,
        const QString& applicationText,
        const QString& serviceText,
        const QString& localPortsText,
        const QString& remotePortsText,
        const QString& localAddressesText,
        const QString& remoteAddressesText,
        const long protocolValue,
        const long directionValue,
        const long actionValue,
        const long profilesValue)
    {
        return QStringLiteral("%1||%2||%3||%4||%5||%6||%7||%8||%9||%10||%11")
            .arg(rawTextOrEmpty(nameText))
            .arg(rawTextOrEmpty(applicationText))
            .arg(rawTextOrEmpty(serviceText))
            .arg(rawTextOrEmpty(localPortsText))
            .arg(rawTextOrEmpty(remotePortsText))
            .arg(rawTextOrEmpty(localAddressesText))
            .arg(rawTextOrEmpty(remoteAddressesText))
            .arg(protocolValue)
            .arg(directionValue)
            .arg(actionValue)
            .arg(profilesValue);
    }

    // directionText 作用：
    // - 输入：WFP 方向值；
    // - 处理：兼容普通 FWP_DIRECTION_* 与 System Informer 使用的 DirectionMap；
    // - 返回：In/Out/FWD/BI/Unknown。
    QString directionText(const UINT32 direction)
    {
        constexpr UINT32 DirectionMapInbound = 0x3900;
        constexpr UINT32 DirectionMapOutbound = 0x3901;
        constexpr UINT32 DirectionMapForward = 0x3902;
        constexpr UINT32 DirectionMapBidirectional = 0x3903;
        switch (direction)
        {
        case FWP_DIRECTION_INBOUND:
        case DirectionMapInbound:
            return QStringLiteral("In");
        case FWP_DIRECTION_OUTBOUND:
        case DirectionMapOutbound:
            return QStringLiteral("Out");
        case DirectionMapForward:
            return QStringLiteral("FWD");
        case DirectionMapBidirectional:
            return QStringLiteral("BI");
        default:
            return QStringLiteral("Unknown");
        }
    }

    // actionText 作用：
    // - 输入：WFP 事件类型；
    // - 处理：映射为 System Informer 风格动作名；
    // - 返回：动作文本。
    QString actionText(const FWPM_NET_EVENT_TYPE type)
    {
        switch (type)
        {
        case FWPM_NET_EVENT_TYPE_CLASSIFY_DROP:
            return QStringLiteral("DROP");
        case FWPM_NET_EVENT_TYPE_IPSEC_KERNEL_DROP:
            return QStringLiteral("IPsec Block");
        case FWPM_NET_EVENT_TYPE_IPSEC_DOSP_DROP:
            return QStringLiteral("Flood Protection");
        case FWPM_NET_EVENT_TYPE_CLASSIFY_ALLOW:
            return QStringLiteral("Allowed");
        case FWPM_NET_EVENT_TYPE_CAPABILITY_DROP:
            return QStringLiteral("DROP (AppContainer)");
        case FWPM_NET_EVENT_TYPE_CAPABILITY_ALLOW:
            return QStringLiteral("Allowed (AppContainer)");
        case FWPM_NET_EVENT_TYPE_CLASSIFY_DROP_MAC:
            return QStringLiteral("DROP (MAC)");
        case FWPM_NET_EVENT_TYPE_LPM_PACKET_ARRIVAL:
            return QStringLiteral("QoS Policy Packet");
        case FWPM_NET_EVENT_TYPE_IKEEXT_MM_FAILURE:
            return QStringLiteral("VPN Failure (Phase 1)");
        case FWPM_NET_EVENT_TYPE_IKEEXT_QM_FAILURE:
            return QStringLiteral("VPN Failure (Phase 2)");
        case FWPM_NET_EVENT_TYPE_IKEEXT_EM_FAILURE:
            return QStringLiteral("VPN Auth Failure");
        default:
            return QStringLiteral("Unknown");
        }
    }

    // isDropEvent 作用：
    // - 输入：WFP 事件类型；
    // - 处理：判断该事件是否应以红色突出；
    // - 返回：DROP/Block 类事件返回 true。
    bool isDropEvent(const FWPM_NET_EVENT_TYPE type)
    {
        return type == FWPM_NET_EVENT_TYPE_CLASSIFY_DROP
            || type == FWPM_NET_EVENT_TYPE_IPSEC_KERNEL_DROP
            || type == FWPM_NET_EVENT_TYPE_IPSEC_DOSP_DROP
            || type == FWPM_NET_EVENT_TYPE_CAPABILITY_DROP
            || type == FWPM_NET_EVENT_TYPE_CLASSIFY_DROP_MAC;
    }

    // addressTextFromHeader 作用：
    // - 输入：WFP 事件头、本地/远端标志；
    // - 处理：按 IPv4/IPv6 转换地址；
    // - 返回：地址文本，字段未设置时返回空字符串。
    QString addressTextFromHeader(const FWPM_NET_EVENT_HEADER3& header, const bool localAddress)
    {
        const UINT32 addressFlag = localAddress
            ? FWPM_NET_EVENT_FLAG_LOCAL_ADDR_SET
            : FWPM_NET_EVENT_FLAG_REMOTE_ADDR_SET;
        if ((header.flags & addressFlag) == 0)
        {
            return QString();
        }

        wchar_t buffer[INET6_ADDRSTRLEN] = {};
        if (header.ipVersion == FWP_IP_VERSION_V4)
        {
            IN_ADDR address{};
            address.S_un.S_addr = htonl(localAddress ? header.localAddrV4 : header.remoteAddrV4);
            if (InetNtopW(AF_INET, &address, buffer, static_cast<DWORD>(std::size(buffer))) != nullptr)
            {
                return QString::fromWCharArray(buffer);
            }
        }
        else if (header.ipVersion == FWP_IP_VERSION_V6)
        {
            IN6_ADDR address{};
            const FWP_BYTE_ARRAY16& sourceBytes = localAddress ? header.localAddrV6 : header.remoteAddrV6;
            std::memcpy(address.u.Byte, sourceBytes.byteArray16, 16);
            if (InetNtopW(AF_INET6, &address, buffer, static_cast<DWORD>(std::size(buffer))) != nullptr)
            {
                return QString::fromWCharArray(buffer);
            }
        }
        return QString();
    }

    // portTextFromHeader 作用：
    // - 输入：WFP 事件头、本地/远端标志；
    // - 处理：提取端口字段；
    // - 返回：端口文本，字段未设置时返回空字符串。
    QString portTextFromHeader(const FWPM_NET_EVENT_HEADER3& header, const bool localPort)
    {
        const UINT32 portFlag = localPort
            ? FWPM_NET_EVENT_FLAG_LOCAL_PORT_SET
            : FWPM_NET_EVENT_FLAG_REMOTE_PORT_SET;
        if ((header.flags & portFlag) == 0)
        {
            return QString();
        }
        return QString::number(localPort ? header.localPort : header.remotePort);
    }

    // appNameFromHeader 作用：
    // - 输入：WFP 事件头；
    // - 处理：从 appId byte blob 中提取 NT/Win32 路径并取文件名；
    // - 返回：应用名或空字符串。
    QString appNameFromHeader(const FWPM_NET_EVENT_HEADER3& header)
    {
        if ((header.flags & FWPM_NET_EVENT_FLAG_APP_ID_SET) == 0
            || header.appId.data == nullptr
            || header.appId.size <= sizeof(wchar_t))
        {
            return QString();
        }

        const int charCount = static_cast<int>((header.appId.size / sizeof(wchar_t)) - 1U);
        QString pathText = QString::fromWCharArray(
            reinterpret_cast<const wchar_t*>(header.appId.data),
            std::max(0, charCount));
        pathText = pathText.replace(QLatin1Char('\\'), QLatin1Char('/'));
        const int slashIndex = pathText.lastIndexOf(QLatin1Char('/'));
        return slashIndex >= 0 ? pathText.mid(slashIndex + 1) : pathText;
    }

    // resolveHostnameText 作用：
    // - 输入：IP 地址文本；
    // - 处理：对空地址、回环/本地场景保持轻量；调用 getnameinfo 做反查；
    // - 返回：解析成功时返回主机名，否则返回空字符串。
    QString resolveHostnameText(const QString& addressText)
    {
        if (addressText.isEmpty()
            || addressText == QStringLiteral("0.0.0.0")
            || addressText == QStringLiteral("::"))
        {
            return QString();
        }

        sockaddr_storage storage{};
        int family = AF_UNSPEC;
        if (InetPtonW(AF_INET, reinterpret_cast<PCWSTR>(addressText.utf16()), &reinterpret_cast<sockaddr_in*>(&storage)->sin_addr) == 1)
        {
            family = AF_INET;
            reinterpret_cast<sockaddr_in*>(&storage)->sin_family = AF_INET;
        }
        else if (InetPtonW(AF_INET6, reinterpret_cast<PCWSTR>(addressText.utf16()), &reinterpret_cast<sockaddr_in6*>(&storage)->sin6_addr) == 1)
        {
            family = AF_INET6;
            reinterpret_cast<sockaddr_in6*>(&storage)->sin6_family = AF_INET6;
        }
        if (family == AF_UNSPEC)
        {
            return QString();
        }

        wchar_t hostBuffer[NI_MAXHOST] = {};
        const int length = family == AF_INET ? sizeof(sockaddr_in) : sizeof(sockaddr_in6);
        const int status = GetNameInfoW(
            reinterpret_cast<sockaddr*>(&storage),
            length,
            hostBuffer,
            static_cast<DWORD>(std::size(hostBuffer)),
            nullptr,
            0,
            NI_NAMEREQD);
        return status == 0 ? QString::fromWCharArray(hostBuffer) : QString();
    }

    // procAddress 作用：
    // - 输入：模块句柄和导出名；
    // - 处理：GetProcAddress 并转换为目标函数指针；
    // - 返回：目标函数指针或 nullptr。
    template <typename T>
    T procAddress(HMODULE moduleHandle, const char* name)
    {
        return reinterpret_cast<T>(GetProcAddress(moduleHandle, name));
    }

    // FirewallRuleEditorDialog：
    // - 作用：统一承载新增/编辑防火墙规则输入；
    // - 处理逻辑：把常见字段映射为轻量表单，校验必要项后输出规则快照；
    // - 返回行为：accept 时通过 ruleEntry() 返回用户输入。
    class FirewallRuleEditorDialog final : public QDialog
    {
    public:
        explicit FirewallRuleEditorDialog(
            const NetworkFirewallPage::FirewallRuleEntry* initialRuleEntry,
            QWidget* parent = nullptr)
            : QDialog(parent)
        {
            setWindowTitle(initialRuleEntry == nullptr ? QStringLiteral("新增防火墙规则") : QStringLiteral("编辑防火墙规则"));
            resize(620, 460);
            ks::ui::RefreshGlobalDialogTheme();

            QVBoxLayout* rootLayout = new QVBoxLayout(this);
            rootLayout->setContentsMargins(12, 12, 12, 12);
            rootLayout->setSpacing(10);

            QLabel* tipLabel = new QLabel(
                QStringLiteral("规则修改将直接写入 Windows Firewall。程序路径、端口和地址支持留空。"),
                this);
            tipLabel->setWordWrap(true);
            rootLayout->addWidget(tipLabel, 0);

            QFormLayout* formLayout = new QFormLayout();
            formLayout->setLabelAlignment(Qt::AlignRight | Qt::AlignVCenter);
            formLayout->setFormAlignment(Qt::AlignTop);
            formLayout->setSpacing(8);

            m_nameEdit = new QLineEdit(this);
            m_descriptionEdit = new QLineEdit(this);
            m_applicationEdit = new QLineEdit(this);
            m_serviceEdit = new QLineEdit(this);
            m_localPortsEdit = new QLineEdit(this);
            m_remotePortsEdit = new QLineEdit(this);
            m_localAddressesEdit = new QLineEdit(this);
            m_remoteAddressesEdit = new QLineEdit(this);
            m_groupingEdit = new QLineEdit(this);

            m_protocolCombo = new QComboBox(this);
            m_protocolCombo->addItem(QStringLiteral("Any"), NET_FW_IP_PROTOCOL_ANY);
            m_protocolCombo->addItem(QStringLiteral("TCP"), NET_FW_IP_PROTOCOL_TCP);
            m_protocolCombo->addItem(QStringLiteral("UDP"), NET_FW_IP_PROTOCOL_UDP);
            m_protocolCombo->addItem(QStringLiteral("ICMPv4"), 1);
            m_protocolCombo->addItem(QStringLiteral("ICMPv6"), 58);

            m_directionCombo = new QComboBox(this);
            m_directionCombo->addItem(QStringLiteral("Inbound"), NET_FW_RULE_DIR_IN);
            m_directionCombo->addItem(QStringLiteral("Outbound"), NET_FW_RULE_DIR_OUT);

            m_actionCombo = new QComboBox(this);
            m_actionCombo->addItem(QStringLiteral("Allow"), NET_FW_ACTION_ALLOW);
            m_actionCombo->addItem(QStringLiteral("Block"), NET_FW_ACTION_BLOCK);

            m_enabledCheck = new QCheckBox(QStringLiteral("启用规则"), this);
            m_enabledCheck->setChecked(true);
            m_profileDomainCheck = new QCheckBox(QStringLiteral("Domain"), this);
            m_profilePrivateCheck = new QCheckBox(QStringLiteral("Private"), this);
            m_profilePublicCheck = new QCheckBox(QStringLiteral("Public"), this);

            QWidget* profileWidget = new QWidget(this);
            QHBoxLayout* profileLayout = new QHBoxLayout(profileWidget);
            profileLayout->setContentsMargins(0, 0, 0, 0);
            profileLayout->setSpacing(10);
            profileLayout->addWidget(m_profileDomainCheck);
            profileLayout->addWidget(m_profilePrivateCheck);
            profileLayout->addWidget(m_profilePublicCheck);
            profileLayout->addStretch(1);

            formLayout->addRow(QStringLiteral("名称"), m_nameEdit);
            formLayout->addRow(QStringLiteral("描述"), m_descriptionEdit);
            formLayout->addRow(QStringLiteral("程序"), m_applicationEdit);
            formLayout->addRow(QStringLiteral("服务"), m_serviceEdit);
            formLayout->addRow(QStringLiteral("方向"), m_directionCombo);
            formLayout->addRow(QStringLiteral("动作"), m_actionCombo);
            formLayout->addRow(QStringLiteral("协议"), m_protocolCombo);
            formLayout->addRow(QStringLiteral("本地端口"), m_localPortsEdit);
            formLayout->addRow(QStringLiteral("远端端口"), m_remotePortsEdit);
            formLayout->addRow(QStringLiteral("本地地址"), m_localAddressesEdit);
            formLayout->addRow(QStringLiteral("远端地址"), m_remoteAddressesEdit);
            formLayout->addRow(QStringLiteral("分组"), m_groupingEdit);
            formLayout->addRow(QStringLiteral("配置文件"), profileWidget);
            formLayout->addRow(QStringLiteral("状态"), m_enabledCheck);
            rootLayout->addLayout(formLayout, 1);

            QDialogButtonBox* buttonBox = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
            rootLayout->addWidget(buttonBox, 0);

            connect(buttonBox, &QDialogButtonBox::accepted, this, [this]()
            {
                if (!buildRuleEntryFromUi(&m_ruleEntry))
                {
                    return;
                }
                accept();
            });
            connect(buttonBox, &QDialogButtonBox::rejected, this, &QDialog::reject);

            if (initialRuleEntry != nullptr)
            {
                loadRuleEntry(*initialRuleEntry);
            }
            else
            {
                m_profileDomainCheck->setChecked(true);
                m_profilePrivateCheck->setChecked(true);
                m_profilePublicCheck->setChecked(true);
            }
        }

        NetworkFirewallPage::FirewallRuleEntry ruleEntry() const
        {
            return m_ruleEntry;
        }

    private:
        void loadRuleEntry(const NetworkFirewallPage::FirewallRuleEntry& ruleEntry)
        {
            m_ruleEntry = ruleEntry;
            m_nameEdit->setText(ruleEntry.nameText);
            m_descriptionEdit->setText(ruleEntry.descriptionText);
            m_applicationEdit->setText(ruleEntry.applicationText);
            m_serviceEdit->setText(ruleEntry.serviceText);
            m_localPortsEdit->setText(ruleEntry.localPortsText);
            m_remotePortsEdit->setText(ruleEntry.remotePortsText);
            m_localAddressesEdit->setText(ruleEntry.localAddressesText);
            m_remoteAddressesEdit->setText(ruleEntry.remoteAddressesText);
            m_groupingEdit->setText(ruleEntry.groupingText);
            m_enabledCheck->setChecked(ruleEntry.enabled);

            const int protocolIndex = m_protocolCombo->findData(QVariant::fromValue(static_cast<qlonglong>(ruleEntry.protocolValue)));
            if (protocolIndex >= 0)
            {
                m_protocolCombo->setCurrentIndex(protocolIndex);
            }

            const int directionIndex = m_directionCombo->findData(QVariant::fromValue(static_cast<qlonglong>(ruleEntry.directionValue)));
            if (directionIndex >= 0)
            {
                m_directionCombo->setCurrentIndex(directionIndex);
            }

            const int actionIndex = m_actionCombo->findData(QVariant::fromValue(static_cast<qlonglong>(ruleEntry.actionValue)));
            if (actionIndex >= 0)
            {
                m_actionCombo->setCurrentIndex(actionIndex);
            }

            m_profileDomainCheck->setChecked((ruleEntry.profilesValue & NET_FW_PROFILE2_DOMAIN) != 0);
            m_profilePrivateCheck->setChecked((ruleEntry.profilesValue & NET_FW_PROFILE2_PRIVATE) != 0);
            m_profilePublicCheck->setChecked((ruleEntry.profilesValue & NET_FW_PROFILE2_PUBLIC) != 0);
        }

        bool buildRuleEntryFromUi(NetworkFirewallPage::FirewallRuleEntry* ruleEntryOut)
        {
            if (ruleEntryOut == nullptr)
            {
                return false;
            }

            const QString nameText = m_nameEdit->text().trimmed();
            if (nameText.isEmpty())
            {
                QMessageBox::warning(this, QStringLiteral("规则校验"), QStringLiteral("规则名称不能为空。"));
                m_nameEdit->setFocus();
                return false;
            }

            long profilesValue = 0;
            if (m_profileDomainCheck->isChecked())
            {
                profilesValue |= NET_FW_PROFILE2_DOMAIN;
            }
            if (m_profilePrivateCheck->isChecked())
            {
                profilesValue |= NET_FW_PROFILE2_PRIVATE;
            }
            if (m_profilePublicCheck->isChecked())
            {
                profilesValue |= NET_FW_PROFILE2_PUBLIC;
            }
            if (profilesValue == 0)
            {
                QMessageBox::warning(this, QStringLiteral("规则校验"), QStringLiteral("至少需要选择一个配置文件。"));
                return false;
            }

            NetworkFirewallPage::FirewallRuleEntry ruleEntry = m_ruleEntry;
            ruleEntry.nameText = nameText;
            ruleEntry.descriptionText = rawTextOrEmpty(m_descriptionEdit->text());
            ruleEntry.applicationText = rawTextOrEmpty(m_applicationEdit->text());
            ruleEntry.serviceText = rawTextOrEmpty(m_serviceEdit->text());
            ruleEntry.localPortsText = rawTextOrEmpty(m_localPortsEdit->text());
            ruleEntry.remotePortsText = rawTextOrEmpty(m_remotePortsEdit->text());
            ruleEntry.localAddressesText = rawTextOrEmpty(m_localAddressesEdit->text());
            ruleEntry.remoteAddressesText = rawTextOrEmpty(m_remoteAddressesEdit->text());
            ruleEntry.groupingText = rawTextOrEmpty(m_groupingEdit->text());
            ruleEntry.enabled = m_enabledCheck->isChecked();
            ruleEntry.protocolValue = m_protocolCombo->currentData().toLongLong();
            ruleEntry.directionValue = m_directionCombo->currentData().toLongLong();
            ruleEntry.actionValue = m_actionCombo->currentData().toLongLong();
            ruleEntry.profilesValue = profilesValue;
            ruleEntry.protocolText = firewallRuleProtocolText(ruleEntry.protocolValue);
            ruleEntry.directionText = firewallRuleDirectionText(ruleEntry.directionValue);
            ruleEntry.actionText = firewallRuleActionText(ruleEntry.actionValue);
            ruleEntry.profilesText = firewallProfilesText(ruleEntry.profilesValue);
            ruleEntry.fingerprintText = composeRuleFingerprint(
                ruleEntry.nameText,
                ruleEntry.applicationText,
                ruleEntry.serviceText,
                ruleEntry.localPortsText,
                ruleEntry.remotePortsText,
                ruleEntry.localAddressesText,
                ruleEntry.remoteAddressesText,
                ruleEntry.protocolValue,
                ruleEntry.directionValue,
                ruleEntry.actionValue,
                ruleEntry.profilesValue);
            *ruleEntryOut = std::move(ruleEntry);
            return true;
        }

    private:
        QLineEdit* m_nameEdit = nullptr;
        QLineEdit* m_descriptionEdit = nullptr;
        QLineEdit* m_applicationEdit = nullptr;
        QLineEdit* m_serviceEdit = nullptr;
        QLineEdit* m_localPortsEdit = nullptr;
        QLineEdit* m_remotePortsEdit = nullptr;
        QLineEdit* m_localAddressesEdit = nullptr;
        QLineEdit* m_remoteAddressesEdit = nullptr;
        QLineEdit* m_groupingEdit = nullptr;
        QComboBox* m_protocolCombo = nullptr;
        QComboBox* m_directionCombo = nullptr;
        QComboBox* m_actionCombo = nullptr;
        QCheckBox* m_enabledCheck = nullptr;
        QCheckBox* m_profileDomainCheck = nullptr;
        QCheckBox* m_profilePrivateCheck = nullptr;
        QCheckBox* m_profilePublicCheck = nullptr;
        NetworkFirewallPage::FirewallRuleEntry m_ruleEntry;
    };
}

NetworkFirewallPage::NetworkFirewallPage(QWidget* parent)
    : QWidget(parent)
{
    initializeUi();
    initializeConnections();
    refreshHistoryAsync(false);
}

NetworkFirewallPage::~NetworkFirewallPage()
{
    stopLiveMonitor();
    if (m_fwpuclntModule != nullptr)
    {
        FreeLibrary(static_cast<HMODULE>(m_fwpuclntModule));
        m_fwpuclntModule = nullptr;
    }
}

void NetworkFirewallPage::initializeUi()
{
    m_rootLayout = new QVBoxLayout(this);
    m_rootLayout->setContentsMargins(6, 6, 6, 6);
    m_rootLayout->setSpacing(6);

    QHBoxLayout* headerLayout = new QHBoxLayout();
    headerLayout->setContentsMargins(0, 0, 0, 0);
    headerLayout->setSpacing(8);

    QLabel* titleLabel = new QLabel(QStringLiteral("防火墙"), this);
    titleLabel->setStyleSheet(
        QStringLiteral("font-size:18px;font-weight:700;color:%1;")
        .arg(KswordTheme::TextPrimaryHex()));
    headerLayout->addWidget(titleLabel, 0);

    m_statusLabel = new QLabel(QStringLiteral("正在加载 WFP 事件..."), this);
    m_statusLabel->setStyleSheet(
        QStringLiteral("font-size:13px;color:%1;").arg(KswordTheme::TextSecondaryHex()));
    m_statusLabel->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
    headerLayout->addWidget(m_statusLabel, 1);
    m_rootLayout->addLayout(headerLayout, 0);

    m_innerTabWidget = new QTabWidget(this);
    m_rootLayout->addWidget(m_innerTabWidget, 1);

    initializeEventMonitorUi();
    initializeRuleManagerUi();

    m_liveFlushTimer = new QTimer(this);
    m_liveFlushTimer->setInterval(250);
}

void NetworkFirewallPage::initializeEventMonitorUi()
{
    m_eventMonitorPage = new QWidget(this);
    QVBoxLayout* pageLayout = new QVBoxLayout(m_eventMonitorPage);
    pageLayout->setContentsMargins(0, 0, 0, 0);
    pageLayout->setSpacing(6);

    QHBoxLayout* toolbarLayout = new QHBoxLayout();
    toolbarLayout->setContentsMargins(0, 0, 0, 0);
    toolbarLayout->setSpacing(8);

    m_refreshHistoryButton = new QPushButton(QStringLiteral("刷新历史"), m_eventMonitorPage);
    m_refreshHistoryButton->setToolTip(QStringLiteral("枚举当前 BFE 会话可见的 WFP net event 历史记录"));
    toolbarLayout->addWidget(m_refreshHistoryButton, 0);

    m_startLiveButton = new QPushButton(QStringLiteral("启动实时"), m_eventMonitorPage);
    m_startLiveButton->setToolTip(QStringLiteral("开启 WFP net event collection 并订阅实时事件，需要管理员权限。"));
    toolbarLayout->addWidget(m_startLiveButton, 0);

    m_stopLiveButton = new QPushButton(QStringLiteral("停止实时"), m_eventMonitorPage);
    m_stopLiveButton->setEnabled(false);
    toolbarLayout->addWidget(m_stopLiveButton, 0);

    m_clearButton = new QPushButton(QStringLiteral("清空"), m_eventMonitorPage);
    toolbarLayout->addWidget(m_clearButton, 0);

    m_searchEdit = new QLineEdit(m_eventMonitorPage);
    m_searchEdit->setPlaceholderText(QStringLiteral("搜索 Name/Action/Rule/地址/端口/协议..."));
    m_searchEdit->setMinimumWidth(240);
    toolbarLayout->addWidget(m_searchEdit, 1);

    m_dropOnlyCheck = new QCheckBox(QStringLiteral("仅 DROP"), m_eventMonitorPage);
    toolbarLayout->addWidget(m_dropOnlyCheck, 0);
    pageLayout->addLayout(toolbarLayout, 0);

    m_eventTable = new QTableWidget(m_eventMonitorPage);
    m_eventTable->setColumnCount(ColumnCount);
    m_eventTable->setHorizontalHeaderLabels({
        QStringLiteral("Name"),
        QStringLiteral("Action"),
        QStringLiteral("Direction"),
        QStringLiteral("Rule"),
        QStringLiteral("Description"),
        QStringLiteral("Local address"),
        QStringLiteral("Local port"),
        QStringLiteral("Local host"),
        QStringLiteral("Remote address"),
        QStringLiteral("Remote port"),
        QStringLiteral("Remote host"),
        QStringLiteral("Protocol"),
        QStringLiteral("Timestamp")
        });
    m_eventTable->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_eventTable->setSelectionMode(QAbstractItemView::ExtendedSelection);
    m_eventTable->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_eventTable->setAlternatingRowColors(true);
    m_eventTable->verticalHeader()->setVisible(false);
    m_eventTable->horizontalHeader()->setStretchLastSection(false);
    m_eventTable->horizontalHeader()->setSectionResizeMode(QHeaderView::Interactive);
    m_eventTable->setColumnWidth(ColumnName, 150);
    m_eventTable->setColumnWidth(ColumnAction, 125);
    m_eventTable->setColumnWidth(ColumnDirection, 70);
    m_eventTable->setColumnWidth(ColumnRule, 230);
    m_eventTable->setColumnWidth(ColumnDescription, 210);
    m_eventTable->setColumnWidth(ColumnLocalAddress, 130);
    m_eventTable->setColumnWidth(ColumnRemoteAddress, 130);
    m_eventTable->setColumnWidth(ColumnTimestamp, 170);
    pageLayout->addWidget(m_eventTable, 1);

    if (m_innerTabWidget != nullptr)
    {
        m_innerTabWidget->addTab(m_eventMonitorPage, QStringLiteral("事件监控"));
    }
}

void NetworkFirewallPage::initializeRuleManagerUi()
{
    m_ruleManagerPage = new QWidget(this);
    QVBoxLayout* pageLayout = new QVBoxLayout(m_ruleManagerPage);
    pageLayout->setContentsMargins(0, 0, 0, 0);
    pageLayout->setSpacing(6);

    QHBoxLayout* toolbarLayout = new QHBoxLayout();
    toolbarLayout->setContentsMargins(0, 0, 0, 0);
    toolbarLayout->setSpacing(8);

    m_refreshRulesButton = new QPushButton(QStringLiteral("刷新规则"), m_ruleManagerPage);
    toolbarLayout->addWidget(m_refreshRulesButton, 0);

    m_addRuleButton = new QPushButton(QStringLiteral("新增"), m_ruleManagerPage);
    toolbarLayout->addWidget(m_addRuleButton, 0);

    m_editRuleButton = new QPushButton(QStringLiteral("编辑"), m_ruleManagerPage);
    m_editRuleButton->setEnabled(false);
    toolbarLayout->addWidget(m_editRuleButton, 0);

    m_toggleRuleButton = new QPushButton(QStringLiteral("启用/禁用"), m_ruleManagerPage);
    m_toggleRuleButton->setEnabled(false);
    toolbarLayout->addWidget(m_toggleRuleButton, 0);

    m_deleteRuleButton = new QPushButton(QStringLiteral("删除"), m_ruleManagerPage);
    m_deleteRuleButton->setEnabled(false);
    toolbarLayout->addWidget(m_deleteRuleButton, 0);

    m_ruleSearchEdit = new QLineEdit(m_ruleManagerPage);
    m_ruleSearchEdit->setPlaceholderText(QStringLiteral("搜索 Name/Application/Port/Protocol/Group..."));
    m_ruleSearchEdit->setMinimumWidth(240);
    toolbarLayout->addWidget(m_ruleSearchEdit, 1);

    m_ruleEnabledOnlyCheck = new QCheckBox(QStringLiteral("仅启用"), m_ruleManagerPage);
    toolbarLayout->addWidget(m_ruleEnabledOnlyCheck, 0);
    pageLayout->addLayout(toolbarLayout, 0);

    m_ruleTable = new QTableWidget(m_ruleManagerPage);
    m_ruleTable->setColumnCount(RuleColumnCount);
    m_ruleTable->setHorizontalHeaderLabels({
        QStringLiteral("Name"),
        QStringLiteral("Enabled"),
        QStringLiteral("Action"),
        QStringLiteral("Direction"),
        QStringLiteral("Profiles"),
        QStringLiteral("Protocol"),
        QStringLiteral("Local Ports"),
        QStringLiteral("Remote Ports"),
        QStringLiteral("Application"),
        QStringLiteral("Service"),
        QStringLiteral("Grouping"),
        QStringLiteral("Description")
        });
    m_ruleTable->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_ruleTable->setSelectionMode(QAbstractItemView::ExtendedSelection);
    m_ruleTable->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_ruleTable->setAlternatingRowColors(true);
    m_ruleTable->verticalHeader()->setVisible(false);
    m_ruleTable->horizontalHeader()->setStretchLastSection(false);
    m_ruleTable->horizontalHeader()->setSectionResizeMode(QHeaderView::Interactive);
    m_ruleTable->setColumnWidth(RuleColumnName, 180);
    m_ruleTable->setColumnWidth(RuleColumnApplication, 220);
    m_ruleTable->setColumnWidth(RuleColumnGrouping, 150);
    m_ruleTable->setColumnWidth(RuleColumnDescription, 220);
    pageLayout->addWidget(m_ruleTable, 1);

    if (m_innerTabWidget != nullptr)
    {
        m_innerTabWidget->addTab(m_ruleManagerPage, QStringLiteral("规则管理"));
    }
}

void NetworkFirewallPage::initializeConnections()
{
    connect(m_refreshHistoryButton, &QPushButton::clicked, this, [this]()
    {
        refreshHistoryAsync(true);
    });
    connect(m_startLiveButton, &QPushButton::clicked, this, [this]()
    {
        startLiveMonitor();
    });
    connect(m_stopLiveButton, &QPushButton::clicked, this, [this]()
    {
        stopLiveMonitor();
    });
    connect(m_clearButton, &QPushButton::clicked, this, [this]()
    {
        if (m_eventTable != nullptr)
        {
            m_eventTable->setRowCount(0);
        }
    });
    connect(m_searchEdit, &QLineEdit::textChanged, this, [this]()
    {
        applyFilterToRows();
    });
    connect(m_dropOnlyCheck, &QCheckBox::toggled, this, [this]()
    {
        applyFilterToRows();
    });
    connect(m_liveFlushTimer, &QTimer::timeout, this, [this]()
    {
        flushLiveEventsToUi();
    });
    connect(m_refreshRulesButton, &QPushButton::clicked, this, [this]()
    {
        refreshRulesAsync(true);
    });
    connect(m_addRuleButton, &QPushButton::clicked, this, [this]()
    {
        addFirewallRule();
    });
    connect(m_editRuleButton, &QPushButton::clicked, this, [this]()
    {
        editSelectedFirewallRule();
    });
    connect(m_toggleRuleButton, &QPushButton::clicked, this, [this]()
    {
        toggleSelectedFirewallRuleEnabled();
    });
    connect(m_deleteRuleButton, &QPushButton::clicked, this, [this]()
    {
        deleteSelectedFirewallRules();
    });
    connect(m_ruleSearchEdit, &QLineEdit::textChanged, this, [this]()
    {
        applyRuleFilterToRows();
    });
    connect(m_ruleEnabledOnlyCheck, &QCheckBox::toggled, this, [this]()
    {
        applyRuleFilterToRows();
    });
    connect(m_ruleTable, &QTableWidget::itemSelectionChanged, this, [this]()
    {
        updateRuleActionButtons();
    });
    connect(m_ruleTable, &QTableWidget::cellDoubleClicked, this, [this](const int, const int)
    {
        editSelectedFirewallRule();
    });
    m_liveFlushTimer->start();
    refreshRulesAsync(false);
}

void NetworkFirewallPage::refreshHistoryAsync(const bool forceRefresh)
{
    bool expectedValue = false;
    if (!m_refreshingHistory.compare_exchange_strong(expectedValue, true))
    {
        if (forceRefresh)
        {
            setStatusText(QStringLiteral("历史事件正在刷新，请稍候。"));
        }
        return;
    }

    if (m_refreshHistoryButton != nullptr)
    {
        m_refreshHistoryButton->setEnabled(false);
    }
    setStatusText(QStringLiteral("正在枚举 WFP 历史事件..."));

    QPointer<NetworkFirewallPage> safeThis(this);
    std::thread([safeThis]()
    {
        std::vector<FirewallEventEntry> eventList;
        QString errorText;
        if (!safeThis.isNull())
        {
            void* engineHandle = nullptr;
            if (safeThis->openWfpEngine(false, &engineHandle, &errorText))
            {
                eventList = safeThis->enumerateHistoryWithEngine(engineHandle, &errorText);
                safeThis->closeWfpEngine(engineHandle, false);
            }
        }

        if (safeThis.isNull())
        {
            return;
        }

        QMetaObject::invokeMethod(
            safeThis.data(),
            [safeThis, eventList = std::move(eventList), errorText]() mutable
            {
                if (safeThis.isNull())
                {
                    return;
                }
                if (errorText.isEmpty())
                {
                    safeThis->appendEventsToTable(eventList, true);
                    safeThis->setStatusText(
                        QStringLiteral("历史事件：%1 条，刷新：%2")
                        .arg(static_cast<int>(eventList.size()))
                        .arg(QDateTime::currentDateTime().toString(QStringLiteral("HH:mm:ss"))));
                }
                else
                {
                    safeThis->setStatusText(errorText);
                }
                if (safeThis->m_refreshHistoryButton != nullptr)
                {
                    safeThis->m_refreshHistoryButton->setEnabled(true);
                }
                safeThis->m_refreshingHistory.store(false);
            },
            Qt::QueuedConnection);
    }).detach();
}

void NetworkFirewallPage::startLiveMonitor()
{
    if (m_liveRunning.load())
    {
        setStatusText(QStringLiteral("实时防火墙监控已经启动。"));
        return;
    }

    QString errorText;
    void* engineHandle = nullptr;
    if (!openWfpEngine(true, &engineHandle, &errorText))
    {
        setStatusText(errorText);
        return;
    }

    FWPM_NET_EVENT_ENUM_TEMPLATE0 enumTemplate{};
    FWPM_NET_EVENT_SUBSCRIPTION0 subscription{};
    subscription.enumTemplate = &enumTemplate;
    CoCreateGuid(&subscription.sessionKey);

    HANDLE subscriptionHandle = nullptr;
    const DWORD status = g_wfpApi.eventSubscribe(
        static_cast<HANDLE>(engineHandle),
        &subscription,
        &NetworkFirewallPage::liveEventCallback,
        this,
        &subscriptionHandle);
    if (status != ERROR_SUCCESS)
    {
        closeWfpEngine(engineHandle, true);
        setStatusText(QStringLiteral("启动实时防火墙事件订阅失败：%1。请确认以管理员运行。").arg(win32ErrorText(status)));
        return;
    }

    m_liveEngineHandle = engineHandle;
    m_liveSubscriptionHandle = subscriptionHandle;
    m_liveRunning.store(true);
    if (m_startLiveButton != nullptr)
    {
        m_startLiveButton->setEnabled(false);
    }
    if (m_stopLiveButton != nullptr)
    {
        m_stopLiveButton->setEnabled(true);
    }
    setStatusText(QStringLiteral("实时防火墙监控已启动。"));
}

void NetworkFirewallPage::stopLiveMonitor()
{
    if (m_liveSubscriptionHandle != nullptr && g_wfpApi.eventUnsubscribe != nullptr && m_liveEngineHandle != nullptr)
    {
        g_wfpApi.eventUnsubscribe(static_cast<HANDLE>(m_liveEngineHandle), static_cast<HANDLE>(m_liveSubscriptionHandle));
        m_liveSubscriptionHandle = nullptr;
    }
    if (m_liveEngineHandle != nullptr)
    {
        closeWfpEngine(m_liveEngineHandle, true);
        m_liveEngineHandle = nullptr;
    }

    const bool wasRunning = m_liveRunning.exchange(false);
    if (m_startLiveButton != nullptr)
    {
        m_startLiveButton->setEnabled(true);
    }
    if (m_stopLiveButton != nullptr)
    {
        m_stopLiveButton->setEnabled(false);
    }
    if (wasRunning)
    {
        setStatusText(QStringLiteral("实时防火墙监控已停止。"));
    }
}

void NetworkFirewallPage::appendEventsToTable(
    const std::vector<FirewallEventEntry>& eventList,
    const bool clearBeforeAppend)
{
    if (m_eventTable == nullptr)
    {
        return;
    }
    if (clearBeforeAppend)
    {
        m_eventTable->setRowCount(0);
    }

    const QColor dropColor = QColor(235, 77, 92);
    const QColor allowColor = KswordTheme::IsDarkModeEnabled() ? QColor(190, 230, 190) : QColor(25, 115, 45);

    m_eventTable->setUpdatesEnabled(false);
    for (const FirewallEventEntry& entry : eventList)
    {
        const int row = m_eventTable->rowCount();
        m_eventTable->insertRow(row);
        const std::array<QString, ColumnCount> values = {
            safeText(entry.nameText),
            safeText(entry.actionText),
            safeText(entry.directionText),
            safeText(entry.ruleText),
            safeText(entry.descriptionText),
            safeText(entry.localAddressText),
            safeText(entry.localPortText),
            safeText(entry.localHostText),
            safeText(entry.remoteAddressText),
            safeText(entry.remotePortText),
            safeText(entry.remoteHostText),
            safeText(entry.protocolText),
            safeText(entry.timestampText)
        };

        for (int column = 0; column < ColumnCount; ++column)
        {
            QTableWidgetItem* item = new QTableWidgetItem(values[static_cast<std::size_t>(column)]);
            item->setData(Qt::UserRole, entry.isDrop);
            if (entry.isDrop)
            {
                item->setForeground(dropColor);
            }
            else if (column == ColumnAction && entry.actionText.contains(QStringLiteral("Allowed"), Qt::CaseInsensitive))
            {
                item->setForeground(allowColor);
            }
            m_eventTable->setItem(row, column, item);
        }
    }
    while (m_eventTable->rowCount() > 5000)
    {
        m_eventTable->removeRow(0);
    }
    m_eventTable->setUpdatesEnabled(true);
    applyFilterToRows();
}

void NetworkFirewallPage::applyFilterToRows()
{
    if (m_eventTable == nullptr)
    {
        return;
    }
    const QString filterText = m_searchEdit != nullptr ? m_searchEdit->text().trimmed().toLower() : QString();
    const bool dropOnly = m_dropOnlyCheck != nullptr && m_dropOnlyCheck->isChecked();

    for (int row = 0; row < m_eventTable->rowCount(); ++row)
    {
        bool isDrop = false;
        QString rowText;
        for (int column = 0; column < ColumnCount; ++column)
        {
            QTableWidgetItem* item = m_eventTable->item(row, column);
            if (item == nullptr)
            {
                continue;
            }
            rowText.append(item->text()).append(QLatin1Char('\n'));
            if (column == ColumnAction)
            {
                isDrop = item->data(Qt::UserRole).toBool();
            }
        }
        const bool textMatched = filterText.isEmpty() || rowText.toLower().contains(filterText);
        const bool dropMatched = !dropOnly || isDrop;
        m_eventTable->setRowHidden(row, !(textMatched && dropMatched));
    }
}

void NetworkFirewallPage::flushLiveEventsToUi()
{
    std::vector<FirewallEventEntry> eventList;
    {
        std::lock_guard<std::mutex> guard(m_liveEventMutex);
        if (m_liveEventQueue.empty())
        {
            return;
        }
        eventList.swap(m_liveEventQueue);
    }
    appendEventsToTable(eventList, false);
    setStatusText(QStringLiteral("实时事件：追加 %1 条，总计 %2 条")
        .arg(static_cast<int>(eventList.size()))
        .arg(m_eventTable != nullptr ? m_eventTable->rowCount() : 0));
}

void NetworkFirewallPage::setStatusText(const QString& statusText)
{
    if (QThread::currentThread() == thread())
    {
        if (m_statusLabel != nullptr)
        {
            m_statusLabel->setText(statusText);
        }
        return;
    }
    QPointer<NetworkFirewallPage> safeThis(this);
    QMetaObject::invokeMethod(
        this,
        [safeThis, statusText]()
        {
            if (!safeThis.isNull() && safeThis->m_statusLabel != nullptr)
            {
                safeThis->m_statusLabel->setText(statusText);
            }
        },
        Qt::QueuedConnection);
}

void NetworkFirewallPage::refreshRulesAsync(const bool forceRefresh)
{
    bool expectedValue = false;
    if (!m_refreshingRules.compare_exchange_strong(expectedValue, true))
    {
        if (forceRefresh)
        {
            setStatusText(QStringLiteral("防火墙规则正在刷新，请稍候。"));
        }
        return;
    }

    if (m_refreshRulesButton != nullptr)
    {
        m_refreshRulesButton->setEnabled(false);
    }
    setStatusText(QStringLiteral("正在枚举 Windows Firewall 规则..."));

    QPointer<NetworkFirewallPage> safeThis(this);
    std::thread([safeThis]()
    {
        std::vector<FirewallRuleEntry> ruleList;
        QString errorText;
        if (!safeThis.isNull())
        {
            ruleList = safeThis->enumerateFirewallRulesSnapshot(&errorText);
        }
        if (safeThis.isNull())
        {
            return;
        }

        QMetaObject::invokeMethod(
            safeThis.data(),
            [safeThis, ruleList = std::move(ruleList), errorText]() mutable
            {
                if (safeThis.isNull())
                {
                    return;
                }

                if (errorText.isEmpty())
                {
                    safeThis->m_ruleEntryList = ruleList;
                    safeThis->appendRulesToTable(safeThis->m_ruleEntryList, true);
                    safeThis->setStatusText(
                        QStringLiteral("防火墙规则：%1 条，刷新：%2")
                        .arg(static_cast<int>(safeThis->m_ruleEntryList.size()))
                        .arg(QDateTime::currentDateTime().toString(QStringLiteral("HH:mm:ss"))));
                }
                else
                {
                    safeThis->setStatusText(errorText);
                }

                if (safeThis->m_refreshRulesButton != nullptr)
                {
                    safeThis->m_refreshRulesButton->setEnabled(true);
                }
                safeThis->updateRuleActionButtons();
                safeThis->m_refreshingRules.store(false);
            },
            Qt::QueuedConnection);
    }).detach();
}

void NetworkFirewallPage::appendRulesToTable(
    const std::vector<FirewallRuleEntry>& ruleList,
    const bool clearBeforeAppend)
{
    if (m_ruleTable == nullptr)
    {
        return;
    }
    if (clearBeforeAppend)
    {
        m_ruleTable->setRowCount(0);
    }

    m_ruleTable->setUpdatesEnabled(false);
    for (const FirewallRuleEntry& ruleEntry : ruleList)
    {
        const int row = m_ruleTable->rowCount();
        m_ruleTable->insertRow(row);

        const std::array<QString, RuleColumnCount> valueList = {
            safeText(ruleEntry.nameText),
            ruleEntry.enabled ? QStringLiteral("Yes") : QStringLiteral("No"),
            safeText(ruleEntry.actionText),
            safeText(ruleEntry.directionText),
            safeText(ruleEntry.profilesText),
            safeText(ruleEntry.protocolText),
            safeText(ruleEntry.localPortsText),
            safeText(ruleEntry.remotePortsText),
            safeText(ruleEntry.applicationText),
            safeText(ruleEntry.serviceText),
            safeText(ruleEntry.groupingText),
            safeText(ruleEntry.descriptionText)
        };

        for (int column = 0; column < RuleColumnCount; ++column)
        {
            QTableWidgetItem* item = new QTableWidgetItem(valueList[static_cast<std::size_t>(column)]);
            item->setData(Qt::UserRole, ruleEntry.fingerprintText);
            item->setData(Qt::UserRole + 1, ruleEntry.enabled);
            if (!ruleEntry.enabled)
            {
                item->setForeground(KswordTheme::TextSecondaryColor());
            }
            else if (column == RuleColumnAction && ruleEntry.actionValue == NET_FW_ACTION_BLOCK)
            {
                item->setForeground(QColor(235, 77, 92));
            }
            m_ruleTable->setItem(row, column, item);
        }
    }
    m_ruleTable->setUpdatesEnabled(true);
    applyRuleFilterToRows();
}

void NetworkFirewallPage::applyRuleFilterToRows()
{
    if (m_ruleTable == nullptr)
    {
        return;
    }

    const QString filterText = m_ruleSearchEdit != nullptr ? m_ruleSearchEdit->text().trimmed().toLower() : QString();
    const bool enabledOnly = m_ruleEnabledOnlyCheck != nullptr && m_ruleEnabledOnlyCheck->isChecked();

    for (int row = 0; row < m_ruleTable->rowCount(); ++row)
    {
        QString rowText;
        bool enabled = false;
        for (int column = 0; column < RuleColumnCount; ++column)
        {
            QTableWidgetItem* item = m_ruleTable->item(row, column);
            if (item == nullptr)
            {
                continue;
            }
            rowText.append(item->text()).append(QLatin1Char('\n'));
            if (column == RuleColumnEnabled)
            {
                enabled = item->data(Qt::UserRole + 1).toBool();
            }
        }

        const bool textMatched = filterText.isEmpty() || rowText.toLower().contains(filterText);
        const bool enabledMatched = !enabledOnly || enabled;
        m_ruleTable->setRowHidden(row, !(textMatched && enabledMatched));
    }
}

void NetworkFirewallPage::updateRuleActionButtons()
{
    int visibleSelectedRowCount = 0;
    if (m_ruleTable != nullptr)
    {
        const QModelIndexList rowIndexList = m_ruleTable->selectionModel() != nullptr
            ? m_ruleTable->selectionModel()->selectedRows()
            : QModelIndexList{};
        for (const QModelIndex& rowIndex : rowIndexList)
        {
            if (!m_ruleTable->isRowHidden(rowIndex.row()))
            {
                ++visibleSelectedRowCount;
            }
        }
    }

    const bool hasSingleSelection = visibleSelectedRowCount == 1;
    const bool hasSelection = visibleSelectedRowCount > 0;

    if (m_editRuleButton != nullptr)
    {
        m_editRuleButton->setEnabled(hasSingleSelection);
    }
    if (m_toggleRuleButton != nullptr)
    {
        m_toggleRuleButton->setEnabled(hasSingleSelection);
    }
    if (m_deleteRuleButton != nullptr)
    {
        m_deleteRuleButton->setEnabled(hasSelection);
    }

    if (m_toggleRuleButton != nullptr && hasSingleSelection)
    {
        FirewallRuleEntry selectedRuleEntryValue;
        if (selectedRuleEntry(&selectedRuleEntryValue))
        {
            m_toggleRuleButton->setText(selectedRuleEntryValue.enabled ? QStringLiteral("禁用") : QStringLiteral("启用"));
        }
    }
    else if (m_toggleRuleButton != nullptr)
    {
        m_toggleRuleButton->setText(QStringLiteral("启用/禁用"));
    }
}

bool NetworkFirewallPage::selectedRuleEntry(FirewallRuleEntry* ruleEntryOut) const
{
    if (ruleEntryOut == nullptr || m_ruleTable == nullptr || m_ruleTable->selectionModel() == nullptr)
    {
        return false;
    }

    const QModelIndexList rowIndexList = m_ruleTable->selectionModel()->selectedRows();
    if (rowIndexList.isEmpty())
    {
        return false;
    }

    const int row = rowIndexList.front().row();
    const QTableWidgetItem* nameItem = m_ruleTable->item(row, RuleColumnName);
    if (nameItem == nullptr)
    {
        return false;
    }

    const QString fingerprintText = nameItem->data(Qt::UserRole).toString();
    const auto it = std::find_if(
        m_ruleEntryList.begin(),
        m_ruleEntryList.end(),
        [&fingerprintText](const FirewallRuleEntry& ruleEntry)
        {
            return ruleEntry.fingerprintText == fingerprintText;
        });
    if (it == m_ruleEntryList.end())
    {
        return false;
    }

    *ruleEntryOut = *it;
    return true;
}

int NetworkFirewallPage::ruleNameDuplicateCount(const QString& ruleNameText) const
{
    return static_cast<int>(std::count_if(
        m_ruleEntryList.begin(),
        m_ruleEntryList.end(),
        [&ruleNameText](const FirewallRuleEntry& ruleEntry)
        {
            return ruleEntry.nameText.compare(ruleNameText, Qt::CaseSensitive) == 0;
        }));
}

std::vector<NetworkFirewallPage::FirewallRuleEntry>
NetworkFirewallPage::enumerateFirewallRulesSnapshot(QString* errorTextOut) const
{
    std::vector<FirewallRuleEntry> resultList;
    ScopedComInitialize comInitializer(COINIT_APARTMENTTHREADED);
    if (!comInitializer.succeeded())
    {
        if (errorTextOut != nullptr)
        {
            *errorTextOut = QStringLiteral("初始化 COM 失败：0x%1")
                .arg(static_cast<unsigned long>(comInitializer.result()), 0, 16);
        }
        return resultList;
    }

    INetFwPolicy2* policyPointer = nullptr;
    HRESULT result = CoCreateInstance(
        __uuidof(NetFwPolicy2),
        nullptr,
        CLSCTX_INPROC_SERVER,
        __uuidof(INetFwPolicy2),
        reinterpret_cast<void**>(&policyPointer));
    if (FAILED(result) || policyPointer == nullptr)
    {
        if (errorTextOut != nullptr)
        {
            *errorTextOut = QStringLiteral("创建 INetFwPolicy2 失败：0x%1")
                .arg(static_cast<unsigned long>(result), 0, 16);
        }
        return resultList;
    }

    INetFwRules* rulesPointer = nullptr;
    result = policyPointer->get_Rules(&rulesPointer);
    if (FAILED(result) || rulesPointer == nullptr)
    {
        releaseComPointer(policyPointer);
        if (errorTextOut != nullptr)
        {
            *errorTextOut = QStringLiteral("获取防火墙规则集合失败：0x%1")
                .arg(static_cast<unsigned long>(result), 0, 16);
        }
        return resultList;
    }

    IUnknown* enumUnknownPointer = nullptr;
    result = rulesPointer->get__NewEnum(&enumUnknownPointer);
    if (FAILED(result) || enumUnknownPointer == nullptr)
    {
        releaseComPointer(rulesPointer);
        releaseComPointer(policyPointer);
        if (errorTextOut != nullptr)
        {
            *errorTextOut = QStringLiteral("获取规则枚举器失败：0x%1")
                .arg(static_cast<unsigned long>(result), 0, 16);
        }
        return resultList;
    }

    IEnumVARIANT* enumVariantPointer = nullptr;
    result = enumUnknownPointer->QueryInterface(IID_IEnumVARIANT, reinterpret_cast<void**>(&enumVariantPointer));
    releaseComPointer(enumUnknownPointer);
    if (FAILED(result) || enumVariantPointer == nullptr)
    {
        releaseComPointer(rulesPointer);
        releaseComPointer(policyPointer);
        if (errorTextOut != nullptr)
        {
            *errorTextOut = QStringLiteral("转换规则枚举器失败：0x%1")
                .arg(static_cast<unsigned long>(result), 0, 16);
        }
        return resultList;
    }

    while (true)
    {
        ScopedVariant currentVariant;
        ULONG fetchedCount = 0;
        result = enumVariantPointer->Next(1, currentVariant.get(), &fetchedCount);
        if (result != S_OK || fetchedCount == 0)
        {
            break;
        }
        if (currentVariant.get()->vt != VT_DISPATCH || currentVariant.get()->pdispVal == nullptr)
        {
            continue;
        }

        INetFwRule* rulePointer = nullptr;
        result = currentVariant.get()->pdispVal->QueryInterface(__uuidof(INetFwRule), reinterpret_cast<void**>(&rulePointer));
        if (FAILED(result) || rulePointer == nullptr)
        {
            continue;
        }

        FirewallRuleEntry ruleEntry;
        ScopedBstr nameText;
        ScopedBstr descriptionText;
        ScopedBstr applicationText;
        ScopedBstr serviceText;
        ScopedBstr localPortsText;
        ScopedBstr remotePortsText;
        ScopedBstr localAddressesText;
        ScopedBstr remoteAddressesText;
        ScopedBstr groupingText;
        VARIANT_BOOL enabledValue = VARIANT_FALSE;
        long protocolValue = 0;
        long directionValue = 0;
        long profilesValue = 0;
        NET_FW_ACTION actionValue = NET_FW_ACTION_BLOCK;

        rulePointer->get_Name(nameText.put());
        rulePointer->get_Description(descriptionText.put());
        rulePointer->get_ApplicationName(applicationText.put());
        rulePointer->get_ServiceName(serviceText.put());
        rulePointer->get_LocalPorts(localPortsText.put());
        rulePointer->get_RemotePorts(remotePortsText.put());
        rulePointer->get_LocalAddresses(localAddressesText.put());
        rulePointer->get_RemoteAddresses(remoteAddressesText.put());
        rulePointer->get_Grouping(groupingText.put());
        rulePointer->get_Enabled(&enabledValue);
        rulePointer->get_Protocol(&protocolValue);
        rulePointer->get_Direction(reinterpret_cast<NET_FW_RULE_DIRECTION*>(&directionValue));
        rulePointer->get_Profiles(&profilesValue);
        rulePointer->get_Action(&actionValue);

        ruleEntry.nameText = rawTextOrEmpty(qStringFromBstr(nameText.get()));
        ruleEntry.descriptionText = rawTextOrEmpty(qStringFromBstr(descriptionText.get()));
        ruleEntry.applicationText = rawTextOrEmpty(qStringFromBstr(applicationText.get()));
        ruleEntry.serviceText = rawTextOrEmpty(qStringFromBstr(serviceText.get()));
        ruleEntry.localPortsText = rawTextOrEmpty(qStringFromBstr(localPortsText.get()));
        ruleEntry.remotePortsText = rawTextOrEmpty(qStringFromBstr(remotePortsText.get()));
        ruleEntry.localAddressesText = rawTextOrEmpty(qStringFromBstr(localAddressesText.get()));
        ruleEntry.remoteAddressesText = rawTextOrEmpty(qStringFromBstr(remoteAddressesText.get()));
        ruleEntry.groupingText = rawTextOrEmpty(qStringFromBstr(groupingText.get()));
        ruleEntry.enabled = enabledValue == VARIANT_TRUE;
        ruleEntry.protocolValue = protocolValue;
        ruleEntry.directionValue = directionValue;
        ruleEntry.profilesValue = profilesValue;
        ruleEntry.actionValue = static_cast<long>(actionValue);
        ruleEntry.protocolText = firewallRuleProtocolText(ruleEntry.protocolValue);
        ruleEntry.directionText = firewallRuleDirectionText(ruleEntry.directionValue);
        ruleEntry.actionText = firewallRuleActionText(ruleEntry.actionValue);
        ruleEntry.profilesText = firewallProfilesText(ruleEntry.profilesValue);
        ruleEntry.fingerprintText = composeRuleFingerprint(
            ruleEntry.nameText,
            ruleEntry.applicationText,
            ruleEntry.serviceText,
            ruleEntry.localPortsText,
            ruleEntry.remotePortsText,
            ruleEntry.localAddressesText,
            ruleEntry.remoteAddressesText,
            ruleEntry.protocolValue,
            ruleEntry.directionValue,
            ruleEntry.actionValue,
            ruleEntry.profilesValue);
        resultList.push_back(std::move(ruleEntry));
        releaseComPointer(rulePointer);
    }

    releaseComPointer(enumVariantPointer);
    releaseComPointer(rulesPointer);
    releaseComPointer(policyPointer);

    std::sort(
        resultList.begin(),
        resultList.end(),
        [](const FirewallRuleEntry& leftEntry, const FirewallRuleEntry& rightEntry)
        {
            const int nameCompare = QString::compare(leftEntry.nameText, rightEntry.nameText, Qt::CaseInsensitive);
            if (nameCompare != 0)
            {
                return nameCompare < 0;
            }
            return QString::compare(leftEntry.applicationText, rightEntry.applicationText, Qt::CaseInsensitive) < 0;
        });
    return resultList;
}

bool NetworkFirewallPage::addFirewallRuleEntryToSystem(
    const FirewallRuleEntry& ruleEntry,
    QString* errorTextOut) const
{
    ScopedComInitialize comInitializer(COINIT_APARTMENTTHREADED);
    if (!comInitializer.succeeded())
    {
        if (errorTextOut != nullptr)
        {
            *errorTextOut = QStringLiteral("初始化 COM 失败：0x%1")
                .arg(static_cast<unsigned long>(comInitializer.result()), 0, 16);
        }
        return false;
    }

    INetFwPolicy2* policyPointer = nullptr;
    HRESULT result = CoCreateInstance(
        __uuidof(NetFwPolicy2),
        nullptr,
        CLSCTX_INPROC_SERVER,
        __uuidof(INetFwPolicy2),
        reinterpret_cast<void**>(&policyPointer));
    if (FAILED(result) || policyPointer == nullptr)
    {
        if (errorTextOut != nullptr)
        {
            *errorTextOut = QStringLiteral("创建 INetFwPolicy2 失败：0x%1")
                .arg(static_cast<unsigned long>(result), 0, 16);
        }
        return false;
    }

    INetFwRules* rulesPointer = nullptr;
    result = policyPointer->get_Rules(&rulesPointer);
    if (FAILED(result) || rulesPointer == nullptr)
    {
        releaseComPointer(policyPointer);
        if (errorTextOut != nullptr)
        {
            *errorTextOut = QStringLiteral("获取规则集合失败：0x%1")
                .arg(static_cast<unsigned long>(result), 0, 16);
        }
        return false;
    }

    INetFwRule* newRulePointer = nullptr;
    result = CoCreateInstance(
        __uuidof(NetFwRule),
        nullptr,
        CLSCTX_INPROC_SERVER,
        __uuidof(INetFwRule),
        reinterpret_cast<void**>(&newRulePointer));
    if (FAILED(result) || newRulePointer == nullptr)
    {
        releaseComPointer(rulesPointer);
        releaseComPointer(policyPointer);
        if (errorTextOut != nullptr)
        {
            *errorTextOut = QStringLiteral("创建 INetFwRule 失败：0x%1")
                .arg(static_cast<unsigned long>(result), 0, 16);
        }
        return false;
    }

    ScopedBstr nameText(bstrFromQString(ruleEntry.nameText));
    ScopedBstr descriptionText(bstrFromQString(ruleEntry.descriptionText));
    ScopedBstr applicationText(bstrFromQString(ruleEntry.applicationText));
    ScopedBstr serviceText(bstrFromQString(ruleEntry.serviceText));
    ScopedBstr localPortsText(bstrFromQString(ruleEntry.localPortsText));
    ScopedBstr remotePortsText(bstrFromQString(ruleEntry.remotePortsText));
    ScopedBstr localAddressesText(bstrFromQString(ruleEntry.localAddressesText));
    ScopedBstr remoteAddressesText(bstrFromQString(ruleEntry.remoteAddressesText));
    ScopedBstr groupingText(bstrFromQString(ruleEntry.groupingText));

    result = newRulePointer->put_Name(nameText.get());
    if (SUCCEEDED(result) && descriptionText.get() != nullptr)
    {
        result = newRulePointer->put_Description(descriptionText.get());
    }
    if (SUCCEEDED(result) && applicationText.get() != nullptr)
    {
        result = newRulePointer->put_ApplicationName(applicationText.get());
    }
    if (SUCCEEDED(result) && serviceText.get() != nullptr)
    {
        result = newRulePointer->put_ServiceName(serviceText.get());
    }
    if (SUCCEEDED(result))
    {
        result = newRulePointer->put_Protocol(ruleEntry.protocolValue);
    }
    if (SUCCEEDED(result) && localPortsText.get() != nullptr)
    {
        result = newRulePointer->put_LocalPorts(localPortsText.get());
    }
    if (SUCCEEDED(result) && remotePortsText.get() != nullptr)
    {
        result = newRulePointer->put_RemotePorts(remotePortsText.get());
    }
    if (SUCCEEDED(result) && localAddressesText.get() != nullptr)
    {
        result = newRulePointer->put_LocalAddresses(localAddressesText.get());
    }
    if (SUCCEEDED(result) && remoteAddressesText.get() != nullptr)
    {
        result = newRulePointer->put_RemoteAddresses(remoteAddressesText.get());
    }
    if (SUCCEEDED(result) && groupingText.get() != nullptr)
    {
        result = newRulePointer->put_Grouping(groupingText.get());
    }
    if (SUCCEEDED(result))
    {
        result = newRulePointer->put_Direction(static_cast<NET_FW_RULE_DIRECTION>(ruleEntry.directionValue));
    }
    if (SUCCEEDED(result))
    {
        result = newRulePointer->put_Profiles(ruleEntry.profilesValue);
    }
    if (SUCCEEDED(result))
    {
        result = newRulePointer->put_Action(static_cast<NET_FW_ACTION>(ruleEntry.actionValue));
    }
    if (SUCCEEDED(result))
    {
        result = newRulePointer->put_Enabled(ruleEntry.enabled ? VARIANT_TRUE : VARIANT_FALSE);
    }
    if (SUCCEEDED(result))
    {
        result = rulesPointer->Add(newRulePointer);
    }

    releaseComPointer(newRulePointer);
    releaseComPointer(rulesPointer);
    releaseComPointer(policyPointer);

    if (FAILED(result))
    {
        if (errorTextOut != nullptr)
        {
            *errorTextOut = QStringLiteral("写入防火墙规则失败：0x%1")
                .arg(static_cast<unsigned long>(result), 0, 16);
        }
        return false;
    }
    return true;
}

bool NetworkFirewallPage::updateFirewallRuleEntryInSystem(
    const QString& originalFingerprintText,
    const FirewallRuleEntry& updatedRuleEntry,
    QString* errorTextOut) const
{
    ScopedComInitialize comInitializer(COINIT_APARTMENTTHREADED);
    if (!comInitializer.succeeded())
    {
        if (errorTextOut != nullptr)
        {
            *errorTextOut = QStringLiteral("初始化 COM 失败：0x%1")
                .arg(static_cast<unsigned long>(comInitializer.result()), 0, 16);
        }
        return false;
    }

    INetFwPolicy2* policyPointer = nullptr;
    HRESULT result = CoCreateInstance(
        __uuidof(NetFwPolicy2),
        nullptr,
        CLSCTX_INPROC_SERVER,
        __uuidof(INetFwPolicy2),
        reinterpret_cast<void**>(&policyPointer));
    if (FAILED(result) || policyPointer == nullptr)
    {
        if (errorTextOut != nullptr)
        {
            *errorTextOut = QStringLiteral("创建 INetFwPolicy2 失败：0x%1")
                .arg(static_cast<unsigned long>(result), 0, 16);
        }
        return false;
    }

    INetFwRules* rulesPointer = nullptr;
    result = policyPointer->get_Rules(&rulesPointer);
    if (FAILED(result) || rulesPointer == nullptr)
    {
        releaseComPointer(policyPointer);
        if (errorTextOut != nullptr)
        {
            *errorTextOut = QStringLiteral("获取规则集合失败：0x%1")
                .arg(static_cast<unsigned long>(result), 0, 16);
        }
        return false;
    }

    IUnknown* enumUnknownPointer = nullptr;
    result = rulesPointer->get__NewEnum(&enumUnknownPointer);
    if (FAILED(result) || enumUnknownPointer == nullptr)
    {
        releaseComPointer(rulesPointer);
        releaseComPointer(policyPointer);
        if (errorTextOut != nullptr)
        {
            *errorTextOut = QStringLiteral("获取规则枚举器失败：0x%1")
                .arg(static_cast<unsigned long>(result), 0, 16);
        }
        return false;
    }

    IEnumVARIANT* enumVariantPointer = nullptr;
    result = enumUnknownPointer->QueryInterface(IID_IEnumVARIANT, reinterpret_cast<void**>(&enumVariantPointer));
    releaseComPointer(enumUnknownPointer);
    if (FAILED(result) || enumVariantPointer == nullptr)
    {
        releaseComPointer(rulesPointer);
        releaseComPointer(policyPointer);
        if (errorTextOut != nullptr)
        {
            *errorTextOut = QStringLiteral("转换规则枚举器失败：0x%1")
                .arg(static_cast<unsigned long>(result), 0, 16);
        }
        return false;
    }

    bool updated = false;
    while (true)
    {
        ScopedVariant currentVariant;
        ULONG fetchedCount = 0;
        result = enumVariantPointer->Next(1, currentVariant.get(), &fetchedCount);
        if (result != S_OK || fetchedCount == 0)
        {
            break;
        }
        if (currentVariant.get()->vt != VT_DISPATCH || currentVariant.get()->pdispVal == nullptr)
        {
            continue;
        }

        INetFwRule* rulePointer = nullptr;
        result = currentVariant.get()->pdispVal->QueryInterface(__uuidof(INetFwRule), reinterpret_cast<void**>(&rulePointer));
        if (FAILED(result) || rulePointer == nullptr)
        {
            continue;
        }

        ScopedBstr nameText;
        ScopedBstr applicationText;
        ScopedBstr serviceText;
        ScopedBstr localPortsText;
        ScopedBstr remotePortsText;
        ScopedBstr localAddressesText;
        ScopedBstr remoteAddressesText;
        long protocolValue = 0;
        long directionValue = 0;
        long profilesValue = 0;
        NET_FW_ACTION actionValue = NET_FW_ACTION_BLOCK;

        rulePointer->get_Name(nameText.put());
        rulePointer->get_ApplicationName(applicationText.put());
        rulePointer->get_ServiceName(serviceText.put());
        rulePointer->get_LocalPorts(localPortsText.put());
        rulePointer->get_RemotePorts(remotePortsText.put());
        rulePointer->get_LocalAddresses(localAddressesText.put());
        rulePointer->get_RemoteAddresses(remoteAddressesText.put());
        rulePointer->get_Protocol(&protocolValue);
        rulePointer->get_Direction(reinterpret_cast<NET_FW_RULE_DIRECTION*>(&directionValue));
        rulePointer->get_Profiles(&profilesValue);
        rulePointer->get_Action(&actionValue);

        const QString currentFingerprintText = composeRuleFingerprint(
            rawTextOrEmpty(qStringFromBstr(nameText.get())),
            rawTextOrEmpty(qStringFromBstr(applicationText.get())),
            rawTextOrEmpty(qStringFromBstr(serviceText.get())),
            rawTextOrEmpty(qStringFromBstr(localPortsText.get())),
            rawTextOrEmpty(qStringFromBstr(remotePortsText.get())),
            rawTextOrEmpty(qStringFromBstr(localAddressesText.get())),
            rawTextOrEmpty(qStringFromBstr(remoteAddressesText.get())),
            protocolValue,
            directionValue,
            static_cast<long>(actionValue),
            profilesValue);
        if (currentFingerprintText == originalFingerprintText)
        {
            ScopedBstr updatedNameText(bstrFromQString(updatedRuleEntry.nameText));
            ScopedBstr updatedDescriptionText(bstrFromQString(updatedRuleEntry.descriptionText));
            ScopedBstr updatedApplicationText(bstrFromQString(updatedRuleEntry.applicationText));
            ScopedBstr updatedServiceText(bstrFromQString(updatedRuleEntry.serviceText));
            ScopedBstr updatedLocalPortsText(bstrFromQString(updatedRuleEntry.localPortsText));
            ScopedBstr updatedRemotePortsText(bstrFromQString(updatedRuleEntry.remotePortsText));
            ScopedBstr updatedLocalAddressesText(bstrFromQString(updatedRuleEntry.localAddressesText));
            ScopedBstr updatedRemoteAddressesText(bstrFromQString(updatedRuleEntry.remoteAddressesText));
            ScopedBstr updatedGroupingText(bstrFromQString(updatedRuleEntry.groupingText));

            result = rulePointer->put_Name(updatedNameText.get());
            if (SUCCEEDED(result))
            {
                result = rulePointer->put_Description(updatedDescriptionText.get());
            }
            if (SUCCEEDED(result))
            {
                result = rulePointer->put_ApplicationName(updatedApplicationText.get());
            }
            if (SUCCEEDED(result))
            {
                result = rulePointer->put_ServiceName(updatedServiceText.get());
            }
            if (SUCCEEDED(result))
            {
                result = rulePointer->put_Protocol(updatedRuleEntry.protocolValue);
            }
            if (SUCCEEDED(result))
            {
                result = rulePointer->put_LocalPorts(updatedLocalPortsText.get());
            }
            if (SUCCEEDED(result))
            {
                result = rulePointer->put_RemotePorts(updatedRemotePortsText.get());
            }
            if (SUCCEEDED(result))
            {
                result = rulePointer->put_LocalAddresses(updatedLocalAddressesText.get());
            }
            if (SUCCEEDED(result))
            {
                result = rulePointer->put_RemoteAddresses(updatedRemoteAddressesText.get());
            }
            if (SUCCEEDED(result))
            {
                result = rulePointer->put_Grouping(updatedGroupingText.get());
            }
            if (SUCCEEDED(result))
            {
                result = rulePointer->put_Direction(static_cast<NET_FW_RULE_DIRECTION>(updatedRuleEntry.directionValue));
            }
            if (SUCCEEDED(result))
            {
                result = rulePointer->put_Profiles(updatedRuleEntry.profilesValue);
            }
            if (SUCCEEDED(result))
            {
                result = rulePointer->put_Action(static_cast<NET_FW_ACTION>(updatedRuleEntry.actionValue));
            }
            if (SUCCEEDED(result))
            {
                result = rulePointer->put_Enabled(updatedRuleEntry.enabled ? VARIANT_TRUE : VARIANT_FALSE);
            }
            updated = SUCCEEDED(result);
            releaseComPointer(rulePointer);
            break;
        }

        releaseComPointer(rulePointer);
    }

    releaseComPointer(enumVariantPointer);
    releaseComPointer(rulesPointer);
    releaseComPointer(policyPointer);

    if (!updated)
    {
        if (errorTextOut != nullptr)
        {
            *errorTextOut = FAILED(result)
                ? QStringLiteral("更新防火墙规则失败：0x%1").arg(static_cast<unsigned long>(result), 0, 16)
                : QStringLiteral("未找到要编辑的防火墙规则，规则可能已被外部修改。");
        }
        return false;
    }
    return true;
}

bool NetworkFirewallPage::setFirewallRuleEnabledInSystem(
    const QString& fingerprintText,
    const bool enabled,
    QString* errorTextOut) const
{
    ScopedComInitialize comInitializer(COINIT_APARTMENTTHREADED);
    if (!comInitializer.succeeded())
    {
        if (errorTextOut != nullptr)
        {
            *errorTextOut = QStringLiteral("初始化 COM 失败：0x%1")
                .arg(static_cast<unsigned long>(comInitializer.result()), 0, 16);
        }
        return false;
    }

    INetFwPolicy2* policyPointer = nullptr;
    HRESULT result = CoCreateInstance(
        __uuidof(NetFwPolicy2),
        nullptr,
        CLSCTX_INPROC_SERVER,
        __uuidof(INetFwPolicy2),
        reinterpret_cast<void**>(&policyPointer));
    if (FAILED(result) || policyPointer == nullptr)
    {
        if (errorTextOut != nullptr)
        {
            *errorTextOut = QStringLiteral("创建 INetFwPolicy2 失败：0x%1")
                .arg(static_cast<unsigned long>(result), 0, 16);
        }
        return false;
    }

    INetFwRules* rulesPointer = nullptr;
    result = policyPointer->get_Rules(&rulesPointer);
    if (FAILED(result) || rulesPointer == nullptr)
    {
        releaseComPointer(policyPointer);
        if (errorTextOut != nullptr)
        {
            *errorTextOut = QStringLiteral("获取规则集合失败：0x%1")
                .arg(static_cast<unsigned long>(result), 0, 16);
        }
        return false;
    }

    IUnknown* enumUnknownPointer = nullptr;
    result = rulesPointer->get__NewEnum(&enumUnknownPointer);
    if (FAILED(result) || enumUnknownPointer == nullptr)
    {
        releaseComPointer(rulesPointer);
        releaseComPointer(policyPointer);
        if (errorTextOut != nullptr)
        {
            *errorTextOut = QStringLiteral("获取规则枚举器失败：0x%1")
                .arg(static_cast<unsigned long>(result), 0, 16);
        }
        return false;
    }

    IEnumVARIANT* enumVariantPointer = nullptr;
    result = enumUnknownPointer->QueryInterface(IID_IEnumVARIANT, reinterpret_cast<void**>(&enumVariantPointer));
    releaseComPointer(enumUnknownPointer);
    if (FAILED(result) || enumVariantPointer == nullptr)
    {
        releaseComPointer(rulesPointer);
        releaseComPointer(policyPointer);
        if (errorTextOut != nullptr)
        {
            *errorTextOut = QStringLiteral("转换规则枚举器失败：0x%1")
                .arg(static_cast<unsigned long>(result), 0, 16);
        }
        return false;
    }

    bool updated = false;
    while (true)
    {
        ScopedVariant currentVariant;
        ULONG fetchedCount = 0;
        result = enumVariantPointer->Next(1, currentVariant.get(), &fetchedCount);
        if (result != S_OK || fetchedCount == 0)
        {
            break;
        }
        if (currentVariant.get()->vt != VT_DISPATCH || currentVariant.get()->pdispVal == nullptr)
        {
            continue;
        }

        INetFwRule* rulePointer = nullptr;
        result = currentVariant.get()->pdispVal->QueryInterface(__uuidof(INetFwRule), reinterpret_cast<void**>(&rulePointer));
        if (FAILED(result) || rulePointer == nullptr)
        {
            continue;
        }

        ScopedBstr nameText;
        ScopedBstr applicationText;
        ScopedBstr serviceText;
        ScopedBstr localPortsText;
        ScopedBstr remotePortsText;
        ScopedBstr localAddressesText;
        ScopedBstr remoteAddressesText;
        long protocolValue = 0;
        long directionValue = 0;
        long profilesValue = 0;
        NET_FW_ACTION actionValue = NET_FW_ACTION_BLOCK;

        rulePointer->get_Name(nameText.put());
        rulePointer->get_ApplicationName(applicationText.put());
        rulePointer->get_ServiceName(serviceText.put());
        rulePointer->get_LocalPorts(localPortsText.put());
        rulePointer->get_RemotePorts(remotePortsText.put());
        rulePointer->get_LocalAddresses(localAddressesText.put());
        rulePointer->get_RemoteAddresses(remoteAddressesText.put());
        rulePointer->get_Protocol(&protocolValue);
        rulePointer->get_Direction(reinterpret_cast<NET_FW_RULE_DIRECTION*>(&directionValue));
        rulePointer->get_Profiles(&profilesValue);
        rulePointer->get_Action(&actionValue);

        const QString currentFingerprintText = composeRuleFingerprint(
            rawTextOrEmpty(qStringFromBstr(nameText.get())),
            rawTextOrEmpty(qStringFromBstr(applicationText.get())),
            rawTextOrEmpty(qStringFromBstr(serviceText.get())),
            rawTextOrEmpty(qStringFromBstr(localPortsText.get())),
            rawTextOrEmpty(qStringFromBstr(remotePortsText.get())),
            rawTextOrEmpty(qStringFromBstr(localAddressesText.get())),
            rawTextOrEmpty(qStringFromBstr(remoteAddressesText.get())),
            protocolValue,
            directionValue,
            static_cast<long>(actionValue),
            profilesValue);
        if (currentFingerprintText == fingerprintText)
        {
            result = rulePointer->put_Enabled(enabled ? VARIANT_TRUE : VARIANT_FALSE);
            updated = SUCCEEDED(result);
            releaseComPointer(rulePointer);
            break;
        }

        releaseComPointer(rulePointer);
    }

    releaseComPointer(enumVariantPointer);
    releaseComPointer(rulesPointer);
    releaseComPointer(policyPointer);

    if (!updated)
    {
        if (errorTextOut != nullptr)
        {
            *errorTextOut = FAILED(result)
                ? QStringLiteral("更新规则启用状态失败：0x%1").arg(static_cast<unsigned long>(result), 0, 16)
                : QStringLiteral("未找到要更新的防火墙规则，规则可能已被外部修改。");
        }
        return false;
    }
    return true;
}

bool NetworkFirewallPage::deleteFirewallRuleFromSystem(
    const QString& ruleNameText,
    QString* errorTextOut) const
{
    ScopedComInitialize comInitializer(COINIT_APARTMENTTHREADED);
    if (!comInitializer.succeeded())
    {
        if (errorTextOut != nullptr)
        {
            *errorTextOut = QStringLiteral("初始化 COM 失败：0x%1")
                .arg(static_cast<unsigned long>(comInitializer.result()), 0, 16);
        }
        return false;
    }

    INetFwPolicy2* policyPointer = nullptr;
    HRESULT result = CoCreateInstance(
        __uuidof(NetFwPolicy2),
        nullptr,
        CLSCTX_INPROC_SERVER,
        __uuidof(INetFwPolicy2),
        reinterpret_cast<void**>(&policyPointer));
    if (FAILED(result) || policyPointer == nullptr)
    {
        if (errorTextOut != nullptr)
        {
            *errorTextOut = QStringLiteral("创建 INetFwPolicy2 失败：0x%1")
                .arg(static_cast<unsigned long>(result), 0, 16);
        }
        return false;
    }

    INetFwRules* rulesPointer = nullptr;
    result = policyPointer->get_Rules(&rulesPointer);
    if (FAILED(result) || rulesPointer == nullptr)
    {
        releaseComPointer(policyPointer);
        if (errorTextOut != nullptr)
        {
            *errorTextOut = QStringLiteral("获取规则集合失败：0x%1")
                .arg(static_cast<unsigned long>(result), 0, 16);
        }
        return false;
    }

    ScopedBstr ruleNameBstr(bstrFromQString(ruleNameText));
    result = rulesPointer->Remove(ruleNameBstr.get());
    releaseComPointer(rulesPointer);
    releaseComPointer(policyPointer);

    if (FAILED(result))
    {
        if (errorTextOut != nullptr)
        {
            *errorTextOut = QStringLiteral("删除防火墙规则失败：0x%1")
                .arg(static_cast<unsigned long>(result), 0, 16);
        }
        return false;
    }
    return true;
}

void NetworkFirewallPage::addFirewallRule()
{
    FirewallRuleEditorDialog dialog(nullptr, this);
    if (dialog.exec() != QDialog::Accepted)
    {
        return;
    }

    const FirewallRuleEntry ruleEntry = dialog.ruleEntry();
    QString errorText;
    if (!addFirewallRuleEntryToSystem(ruleEntry, &errorText))
    {
        QMessageBox::warning(this, QStringLiteral("新增规则失败"), errorText);
        setStatusText(errorText);
        return;
    }

    setStatusText(QStringLiteral("已新增防火墙规则：%1").arg(ruleEntry.nameText));
    refreshRulesAsync(true);
}

void NetworkFirewallPage::editSelectedFirewallRule()
{
    FirewallRuleEntry originalRuleEntry;
    if (!selectedRuleEntry(&originalRuleEntry))
    {
        return;
    }

    FirewallRuleEditorDialog dialog(&originalRuleEntry, this);
    if (dialog.exec() != QDialog::Accepted)
    {
        return;
    }

    const FirewallRuleEntry updatedRuleEntry = dialog.ruleEntry();
    QString errorText;
    if (!updateFirewallRuleEntryInSystem(originalRuleEntry.fingerprintText, updatedRuleEntry, &errorText))
    {
        QMessageBox::warning(this, QStringLiteral("编辑规则失败"), errorText);
        setStatusText(errorText);
        return;
    }

    setStatusText(QStringLiteral("已更新防火墙规则：%1").arg(updatedRuleEntry.nameText));
    refreshRulesAsync(true);
}

void NetworkFirewallPage::toggleSelectedFirewallRuleEnabled()
{
    FirewallRuleEntry selectedRuleEntryValue;
    if (!selectedRuleEntry(&selectedRuleEntryValue))
    {
        return;
    }

    QString errorText;
    const bool targetEnabled = !selectedRuleEntryValue.enabled;
    if (!setFirewallRuleEnabledInSystem(selectedRuleEntryValue.fingerprintText, targetEnabled, &errorText))
    {
        QMessageBox::warning(this, QStringLiteral("更新规则状态失败"), errorText);
        setStatusText(errorText);
        return;
    }

    setStatusText(QStringLiteral("规则 %1：%2")
        .arg(selectedRuleEntryValue.nameText)
        .arg(targetEnabled ? QStringLiteral("已启用") : QStringLiteral("已禁用")));
    refreshRulesAsync(true);
}

void NetworkFirewallPage::deleteSelectedFirewallRules()
{
    if (m_ruleTable == nullptr || m_ruleTable->selectionModel() == nullptr)
    {
        return;
    }

    QStringList ruleNameList;
    const QModelIndexList rowIndexList = m_ruleTable->selectionModel()->selectedRows();
    for (const QModelIndex& rowIndex : rowIndexList)
    {
        if (m_ruleTable->isRowHidden(rowIndex.row()))
        {
            continue;
        }
        QTableWidgetItem* nameItem = m_ruleTable->item(rowIndex.row(), RuleColumnName);
        if (nameItem != nullptr)
        {
            ruleNameList.push_back(nameItem->text());
        }
    }
    ruleNameList.removeDuplicates();
    if (ruleNameList.isEmpty())
    {
        return;
    }

    const int duplicateCount = ruleNameList.size() == 1 ? ruleNameDuplicateCount(ruleNameList.front()) : 0;
    QString warningText = ruleNameList.size() == 1
        ? QStringLiteral("确定删除规则“%1”吗？").arg(ruleNameList.front())
        : QStringLiteral("确定删除选中的 %1 条规则吗？").arg(ruleNameList.size());
    if (duplicateCount > 1)
    {
        warningText.append(QStringLiteral("\n注意：同名规则存在 %1 条，Windows Firewall 将按名称删除同名项。").arg(duplicateCount));
    }

    const QMessageBox::StandardButton button = QMessageBox::question(
        this,
        QStringLiteral("删除防火墙规则"),
        warningText,
        QMessageBox::Yes | QMessageBox::No,
        QMessageBox::No);
    if (button != QMessageBox::Yes)
    {
        return;
    }

    for (const QString& ruleNameText : ruleNameList)
    {
        QString errorText;
        if (!deleteFirewallRuleFromSystem(ruleNameText, &errorText))
        {
            QMessageBox::warning(this, QStringLiteral("删除规则失败"), errorText);
            setStatusText(errorText);
            refreshRulesAsync(true);
            return;
        }
    }

    setStatusText(QStringLiteral("已删除 %1 条防火墙规则。").arg(ruleNameList.size()));
    refreshRulesAsync(true);
}

bool NetworkFirewallPage::ensureWfpApiLoaded(QString* errorTextOut)
{
    if (m_fwpuclntModule == nullptr)
    {
        m_fwpuclntModule = LoadLibraryW(L"fwpuclnt.dll");
    }
    if (m_fwpuclntModule == nullptr)
    {
        if (errorTextOut != nullptr)
        {
            *errorTextOut = QStringLiteral("无法加载 fwpuclnt.dll：%1").arg(win32ErrorText(GetLastError()));
        }
        return false;
    }

    HMODULE moduleHandle = static_cast<HMODULE>(m_fwpuclntModule);
    g_wfpApi.engineOpen = procAddress<WfpApi::FwpmEngineOpen0Fn>(moduleHandle, "FwpmEngineOpen0");
    g_wfpApi.engineClose = procAddress<WfpApi::FwpmEngineClose0Fn>(moduleHandle, "FwpmEngineClose0");
    g_wfpApi.engineSetOption = procAddress<WfpApi::FwpmEngineSetOption0Fn>(moduleHandle, "FwpmEngineSetOption0");
    g_wfpApi.freeMemory = procAddress<WfpApi::FwpmFreeMemory0Fn>(moduleHandle, "FwpmFreeMemory0");
    g_wfpApi.filterGetById = procAddress<WfpApi::FwpmFilterGetById0Fn>(moduleHandle, "FwpmFilterGetById0");
    g_wfpApi.eventCreateEnumHandle =
        procAddress<WfpApi::FwpmNetEventCreateEnumHandle0Fn>(moduleHandle, "FwpmNetEventCreateEnumHandle0");
    g_wfpApi.eventDestroyEnumHandle =
        procAddress<WfpApi::FwpmNetEventDestroyEnumHandle0Fn>(moduleHandle, "FwpmNetEventDestroyEnumHandle0");
    g_wfpApi.eventEnum = procAddress<WfpApi::FwpmNetEventEnumGenericFn>(moduleHandle, "FwpmNetEventEnum5");
    if (g_wfpApi.eventEnum == nullptr)
    {
        g_wfpApi.eventEnum = procAddress<WfpApi::FwpmNetEventEnumGenericFn>(moduleHandle, "FwpmNetEventEnum4");
    }
    if (g_wfpApi.eventEnum == nullptr)
    {
        g_wfpApi.eventEnum = procAddress<WfpApi::FwpmNetEventEnumGenericFn>(moduleHandle, "FwpmNetEventEnum3");
    }

    g_wfpApi.eventSubscribe = procAddress<WfpApi::FwpmNetEventSubscribeGenericFn>(moduleHandle, "FwpmNetEventSubscribe4");
    if (g_wfpApi.eventSubscribe == nullptr)
    {
        g_wfpApi.eventSubscribe = procAddress<WfpApi::FwpmNetEventSubscribeGenericFn>(moduleHandle, "FwpmNetEventSubscribe3");
    }
    g_wfpApi.eventUnsubscribe =
        procAddress<WfpApi::FwpmNetEventUnsubscribe0Fn>(moduleHandle, "FwpmNetEventUnsubscribe0");

    if (g_wfpApi.engineOpen == nullptr
        || g_wfpApi.engineClose == nullptr
        || g_wfpApi.engineSetOption == nullptr
        || g_wfpApi.freeMemory == nullptr
        || g_wfpApi.filterGetById == nullptr
        || g_wfpApi.eventCreateEnumHandle == nullptr
        || g_wfpApi.eventDestroyEnumHandle == nullptr
        || g_wfpApi.eventEnum == nullptr
        || g_wfpApi.eventSubscribe == nullptr
        || g_wfpApi.eventUnsubscribe == nullptr)
    {
        if (errorTextOut != nullptr)
        {
            *errorTextOut = QStringLiteral("fwpuclnt.dll 缺少必要的 WFP 导出，当前系统不支持该防火墙事件视图。");
        }
        return false;
    }
    return true;
}

bool NetworkFirewallPage::openWfpEngine(
    const bool enableCollection,
    void** engineHandleOut,
    QString* errorTextOut)
{
    if (engineHandleOut == nullptr)
    {
        return false;
    }
    *engineHandleOut = nullptr;
    if (!ensureWfpApiLoaded(errorTextOut))
    {
        return false;
    }

    FWPM_SESSION0 session{};
    session.displayData.name = const_cast<wchar_t*>(L"KswordFirewallMonitor");
    session.displayData.description = const_cast<wchar_t*>(L"Ksword WFP firewall event monitor");
    session.flags = FWPM_SESSION_FLAG_DYNAMIC;

    HANDLE engineHandle = nullptr;
    DWORD status = g_wfpApi.engineOpen(nullptr, RPC_C_AUTHN_DEFAULT, nullptr, &session, &engineHandle);
    if (status != ERROR_SUCCESS)
    {
        if (errorTextOut != nullptr)
        {
            *errorTextOut = QStringLiteral("打开 BFE/WFP engine 失败：%1。防火墙事件通常需要管理员权限。")
                .arg(win32ErrorText(status));
        }
        return false;
    }

    if (enableCollection)
    {
        FWP_VALUE0 value{};
        value.type = FWP_UINT32;
        value.uint32 = TRUE;
        status = g_wfpApi.engineSetOption(engineHandle, FWPM_ENGINE_COLLECT_NET_EVENTS, &value);
        if (status != ERROR_SUCCESS)
        {
            g_wfpApi.engineClose(engineHandle);
            if (errorTextOut != nullptr)
            {
                *errorTextOut = QStringLiteral("开启 WFP net event collection 失败：%1。请以管理员运行。")
                    .arg(win32ErrorText(status));
            }
            return false;
        }

        value.type = FWP_UINT32;
        value.uint32 = FWPM_NET_EVENT_KEYWORD_INBOUND_MCAST
            | FWPM_NET_EVENT_KEYWORD_INBOUND_BCAST
            | FWPM_NET_EVENT_KEYWORD_CAPABILITY_DROP
            | FWPM_NET_EVENT_KEYWORD_CAPABILITY_ALLOW
            | FWPM_NET_EVENT_KEYWORD_CLASSIFY_ALLOW;
        g_wfpApi.engineSetOption(engineHandle, FWPM_ENGINE_NET_EVENT_MATCH_ANY_KEYWORDS, &value);
    }

    *engineHandleOut = engineHandle;
    return true;
}

void NetworkFirewallPage::closeWfpEngine(void* engineHandle, const bool disableCollection)
{
    if (engineHandle == nullptr || g_wfpApi.engineClose == nullptr)
    {
        return;
    }
    if (disableCollection && g_wfpApi.engineSetOption != nullptr)
    {
        FWP_VALUE0 value{};
        value.type = FWP_UINT32;
        value.uint32 = FALSE;
        g_wfpApi.engineSetOption(static_cast<HANDLE>(engineHandle), FWPM_ENGINE_COLLECT_NET_EVENTS, &value);
    }
    g_wfpApi.engineClose(static_cast<HANDLE>(engineHandle));
}

std::vector<NetworkFirewallPage::FirewallEventEntry>
NetworkFirewallPage::enumerateHistoryWithEngine(void* engineHandle, QString* errorTextOut)
{
    std::vector<FirewallEventEntry> resultList;
    if (engineHandle == nullptr)
    {
        return resultList;
    }

    FWPM_NET_EVENT_ENUM_TEMPLATE0 enumTemplate{};
    HANDLE enumHandle = nullptr;
    DWORD status = g_wfpApi.eventCreateEnumHandle(static_cast<HANDLE>(engineHandle), &enumTemplate, &enumHandle);
    if (status != ERROR_SUCCESS)
    {
        if (errorTextOut != nullptr)
        {
            *errorTextOut = QStringLiteral("创建 WFP 事件枚举句柄失败：%1").arg(win32ErrorText(status));
        }
        return resultList;
    }

    while (true)
    {
        void** entries = nullptr;
        UINT32 count = 0;
        status = g_wfpApi.eventEnum(static_cast<HANDLE>(engineHandle), enumHandle, 256, &entries, &count);
        if (status != ERROR_SUCCESS)
        {
            if (errorTextOut != nullptr)
            {
                *errorTextOut = QStringLiteral("枚举 WFP 事件失败：%1").arg(win32ErrorText(status));
            }
            break;
        }
        if (count == 0 || entries == nullptr)
        {
            if (entries != nullptr)
            {
                g_wfpApi.freeMemory(reinterpret_cast<void**>(&entries));
            }
            break;
        }

        for (UINT32 index = 0; index < count; ++index)
        {
            if (entries[index] != nullptr)
            {
                resultList.push_back(convertWfpEventToEntry(entries[index], engineHandle));
            }
        }
        g_wfpApi.freeMemory(reinterpret_cast<void**>(&entries));
        if (resultList.size() >= 5000)
        {
            break;
        }
    }

    g_wfpApi.eventDestroyEnumHandle(static_cast<HANDLE>(engineHandle), enumHandle);
    std::reverse(resultList.begin(), resultList.end());
    return resultList;
}

NetworkFirewallPage::FirewallEventEntry NetworkFirewallPage::convertWfpEventToEntry(
    const void* wfpEventPointer,
    void* engineHandle)
{
    FirewallEventEntry entry;
    const FWPM_NET_EVENT5* eventPointer = static_cast<const FWPM_NET_EVENT5*>(wfpEventPointer);
    if (eventPointer == nullptr)
    {
        return entry;
    }

    entry.actionText = actionText(eventPointer->type);
    entry.isDrop = isDropEvent(eventPointer->type);
    entry.descriptionText = entry.actionText;
    entry.timestampText = fileTimeToText(eventPointer->header.timeStamp);
    entry.protocolText = (eventPointer->header.flags & FWPM_NET_EVENT_FLAG_IP_PROTOCOL_SET) != 0
        ? protocolText(eventPointer->header.ipProtocol)
        : QString();
    entry.localAddressText = addressTextFromHeader(eventPointer->header, true);
    entry.localPortText = portTextFromHeader(eventPointer->header, true);
    entry.remoteAddressText = addressTextFromHeader(eventPointer->header, false);
    entry.remotePortText = portTextFromHeader(eventPointer->header, false);
    entry.nameText = appNameFromHeader(eventPointer->header);
    if (entry.nameText.isEmpty())
    {
        entry.nameText = entry.actionText;
    }

    UINT64 filterId = 0;
    UINT32 rawDirection = 0;
    switch (eventPointer->type)
    {
    case FWPM_NET_EVENT_TYPE_CLASSIFY_DROP:
        if (eventPointer->classifyDrop != nullptr)
        {
            filterId = eventPointer->classifyDrop->filterId;
            rawDirection = eventPointer->classifyDrop->msFwpDirection;
        }
        break;
    case FWPM_NET_EVENT_TYPE_CLASSIFY_ALLOW:
        if (eventPointer->classifyAllow != nullptr)
        {
            filterId = eventPointer->classifyAllow->filterId;
            rawDirection = eventPointer->classifyAllow->msFwpDirection;
        }
        break;
    case FWPM_NET_EVENT_TYPE_CAPABILITY_DROP:
        if (eventPointer->capabilityDrop != nullptr)
        {
            filterId = eventPointer->capabilityDrop->filterId;
            rawDirection = FWP_DIRECTION_OUTBOUND;
        }
        break;
    case FWPM_NET_EVENT_TYPE_CAPABILITY_ALLOW:
        if (eventPointer->capabilityAllow != nullptr)
        {
            filterId = eventPointer->capabilityAllow->filterId;
            rawDirection = FWP_DIRECTION_OUTBOUND;
        }
        break;
    case FWPM_NET_EVENT_TYPE_CLASSIFY_DROP_MAC:
        if (eventPointer->classifyDropMac != nullptr)
        {
            filterId = eventPointer->classifyDropMac->filterId;
            rawDirection = eventPointer->classifyDropMac->msFwpDirection;
        }
        break;
    case FWPM_NET_EVENT_TYPE_IPSEC_KERNEL_DROP:
        if (eventPointer->ipsecDrop != nullptr)
        {
            filterId = eventPointer->ipsecDrop->filterId;
            rawDirection = static_cast<UINT32>(eventPointer->ipsecDrop->direction);
        }
        break;
    default:
        break;
    }
    entry.directionText = directionText(rawDirection);

    if (filterId != 0 && engineHandle != nullptr && g_wfpApi.filterGetById != nullptr)
    {
        FWPM_FILTER0* filterPointer = nullptr;
        if (g_wfpApi.filterGetById(static_cast<HANDLE>(engineHandle), filterId, &filterPointer) == ERROR_SUCCESS
            && filterPointer != nullptr)
        {
            if (filterPointer->displayData.name != nullptr)
            {
                entry.ruleText = QString::fromWCharArray(filterPointer->displayData.name);
            }
            if (filterPointer->displayData.description != nullptr)
            {
                entry.descriptionText = QString::fromWCharArray(filterPointer->displayData.description);
            }
            g_wfpApi.freeMemory(reinterpret_cast<void**>(&filterPointer));
        }
        if (entry.ruleText.isEmpty())
        {
            entry.ruleText = QStringLiteral("FilterId=%1").arg(filterId);
        }
    }

    entry.localHostText = resolveHostnameText(entry.localAddressText);
    entry.remoteHostText = resolveHostnameText(entry.remoteAddressText);
    return entry;
}

void NetworkFirewallPage::enqueueLiveEvent(const void* wfpEventPointer)
{
    if (!m_liveRunning.load() || wfpEventPointer == nullptr)
    {
        return;
    }
    FirewallEventEntry entry = convertWfpEventToEntry(wfpEventPointer, m_liveEngineHandle);
    std::lock_guard<std::mutex> guard(m_liveEventMutex);
    if (m_liveEventQueue.size() >= 2000)
    {
        m_liveEventQueue.erase(m_liveEventQueue.begin(), m_liveEventQueue.begin() + 500);
    }
    m_liveEventQueue.push_back(std::move(entry));
}

void __stdcall NetworkFirewallPage::liveEventCallback(void* context, const void* eventPointer)
{
    NetworkFirewallPage* pagePointer = static_cast<NetworkFirewallPage*>(context);
    if (pagePointer != nullptr)
    {
        pagePointer->enqueueLiveEvent(eventPointer);
    }
}
