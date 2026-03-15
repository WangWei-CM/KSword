#pragma once

// ============================================================
// ksword/network/network_request_tools.h
// 命名空间：ks::network
// 作用：
// 1) 定义“手动构造网络请求”所需的参数/结果结构；
// 2) 支持可配置的 TCP/UDP Winsock 请求执行；
// 3) 支持文本/十六进制两种载荷输入格式。
//
// 设计说明：
// - 头文件内联实现，避免新增 cpp 文件后再改工程文件；
// - 重点面向 NetworkDock“请求构造”Tab 的参数化执行场景。
// ============================================================

#include <algorithm> // std::clamp：接收长度与超时范围约束。
#include <cctype>    // std::isxdigit：十六进制字符校验。
#include <cstdint>   // 固定宽度整数：端口、长度、超时等。
#include <cstring>   // std::memset：sockaddr 结构清零。
#include <exception> // std::exception：十六进制解析异常保护。
#include <string>    // std::string：参数与结果文本。
#include <utility>   // std::move：结果对象移动赋值。
#include <vector>    // std::vector：请求/响应字节容器。

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <WinSock2.h>
#include <Ws2tcpip.h>
#include <Ws2def.h>
#include <Mstcpip.h>

// Ws2_32：WSAStartup/socket/connect/send/recv 等基础网络 API。
#pragma comment(lib, "Ws2_32.lib")

namespace ks::network
{
    // ManualNetworkApiKind：请求构造页提供的 API 方式。
    enum class ManualNetworkApiKind : std::uint8_t
    {
        WinSockTcp = 0, // TCP：典型 connect + send + recv。
        WinSockUdp = 1  // UDP：可 connect 或 sendto/recvfrom。
    };

    // ManualPayloadFormat：载荷输入格式。
    enum class ManualPayloadFormat : std::uint8_t
    {
        AsciiText = 0, // 按 UTF-8 文本字节发送。
        HexBytes = 1   // 按十六进制字节串发送（如 "48 65 6C 6C 6F"）。
    };

    // ManualNetworkRequest：一次手动请求的完整参数对象。
    struct ManualNetworkRequest
    {
        // API 模式：
        // - 默认使用 TCP；
        // - UI 可切换为 UDP。
        ManualNetworkApiKind apiKind = ManualNetworkApiKind::WinSockTcp;

        // Socket 参数：
        // - overrideSocketParameters=false 时，按 apiKind 自动套用 TCP/UDP 默认值；
        // - true 时，直接使用 addressFamily/socketType/protocol/socketFlags。
        bool overrideSocketParameters = false;
        int addressFamily = AF_INET;         // 地址族（默认 AF_INET）。
        int socketType = SOCK_STREAM;        // Socket 类型（SOCK_STREAM / SOCK_DGRAM）。
        int protocol = IPPROTO_TCP;          // 协议（IPPROTO_TCP / IPPROTO_UDP）。
        DWORD socketFlags = WSA_FLAG_OVERLAPPED; // WSASocket flags。

        // 本地/远端端点：
        bool enableLocalBind = false;        // true 时先 bind(localAddress:localPort)。
        std::string localAddress = "0.0.0.0";// bind 地址。
        std::uint16_t localPort = 0;         // bind 端口（0=系统分配）。
        std::string remoteAddress = "127.0.0.1"; // 目标地址。
        std::uint16_t remotePort = 80;       // 目标端口。

        // 请求执行行为：
        bool connectBeforeSend = true;       // 发送前是否 connect（UDP 可关闭）。
        bool enableReuseAddress = false;     // SO_REUSEADDR。
        bool enableNoDelay = false;          // TCP_NODELAY（TCP 下有效）。
        std::uint32_t sendTimeoutMs = 3000;  // SO_SNDTIMEO。
        std::uint32_t receiveTimeoutMs = 3000;// SO_RCVTIMEO。
        int sendFlags = 0;                   // send/sendto flags。
        int receiveFlags = 0;                // recv/recvfrom flags。
        bool receiveAfterSend = true;        // 发送后是否读取响应。
        std::size_t receiveMaxBytes = 4096;  // 读取响应最大字节数。
        bool shutdownSendAfterWrite = false; // TCP 下是否发送后 shutdown(SD_SEND)。

        // 请求载荷：
        ManualPayloadFormat payloadFormat = ManualPayloadFormat::AsciiText;
        std::string payloadText;             // 文本或十六进制串（由 payloadFormat 决定）。
    };

    // ManualNetworkResult：一次手动请求执行结果。
    struct ManualNetworkResult
    {
        bool succeeded = false;              // 总体成功标记。
        int wsaErrorCode = 0;                // 失败时的 WSA 错误码（0 表示无）。
        std::string detailText;              // 细节说明（可直接展示到 UI）。
        std::size_t bytesSent = 0;           // 实际发送字节数。
        std::size_t bytesReceived = 0;       // 实际接收字节数。
        std::vector<std::uint8_t> responseBytes; // 接收响应原始字节。
    };

    namespace request_detail
    {
        // MakeWsaErrorText：把 WSA 错误码组装成可读文本。
        inline std::string MakeWsaErrorText(const int wsaErrorCode)
        {
            return "WSAError=" + std::to_string(wsaErrorCode);
        }

        // WsaSessionGuard：
        // - 负责当前调用范围内 WSAStartup/WSACleanup 生命周期；
        // - 避免调用方忘记清理导致资源泄漏。
        class WsaSessionGuard
        {
        public:
            WsaSessionGuard()
            {
                std::memset(&m_wsaData, 0, sizeof(m_wsaData));
                m_startupResult = ::WSAStartup(MAKEWORD(2, 2), &m_wsaData);
                m_started = (m_startupResult == 0);
            }

            ~WsaSessionGuard()
            {
                if (m_started)
                {
                    ::WSACleanup();
                }
            }

            bool started() const
            {
                return m_started;
            }

            int startupResult() const
            {
                return m_startupResult;
            }

        private:
            WSADATA m_wsaData{};
            int m_startupResult = 0;
            bool m_started = false;
        };

        // SocketGuard：
        // - 简易 RAII 套接字封装；
        // - 确保异常/早退路径也会 closesocket。
        class SocketGuard
        {
        public:
            SocketGuard() = default;
            ~SocketGuard()
            {
                close();
            }

            void reset(const SOCKET socketValue)
            {
                close();
                m_socket = socketValue;
            }

            SOCKET get() const
            {
                return m_socket;
            }

            bool valid() const
            {
                return m_socket != INVALID_SOCKET;
            }

            void close()
            {
                if (m_socket != INVALID_SOCKET)
                {
                    ::closesocket(m_socket);
                    m_socket = INVALID_SOCKET;
                }
            }

        private:
            SOCKET m_socket = INVALID_SOCKET;
        };

        // ResolveIpv4Endpoint：
        // - 把 address:port 解析到 sockaddr_in；
        // - 仅支持 IPv4，失败时返回 false。
        inline bool ResolveIpv4Endpoint(
            const std::string& addressText,
            const std::uint16_t portValue,
            sockaddr_in& endpointOut)
        {
            std::memset(&endpointOut, 0, sizeof(endpointOut));
            endpointOut.sin_family = AF_INET;
            endpointOut.sin_port = htons(portValue);
            const int parseResult = ::inet_pton(AF_INET, addressText.c_str(), &endpointOut.sin_addr);
            return parseResult == 1;
        }

        // ParseHexPayloadText：
        // - 解析十六进制字节串，支持空格/换行/逗号分隔；
        // - 例如 "48 65 6C 6C 6F" -> {'H','e','l','l','o'}。
        inline bool ParseHexPayloadText(
            const std::string& hexPayloadText,
            std::vector<std::uint8_t>& payloadBytesOut,
            std::string* errorTextOut = nullptr)
        {
            payloadBytesOut.clear();
            if (errorTextOut != nullptr)
            {
                errorTextOut->clear();
            }

            // 清洗输入：移除常见分隔符，只保留十六进制字符。
            std::string normalizedHexText;
            normalizedHexText.reserve(hexPayloadText.size());
            for (char currentChar : hexPayloadText)
            {
                if (currentChar == ' ' || currentChar == '\t' || currentChar == '\r' ||
                    currentChar == '\n' || currentChar == ',' || currentChar == ';')
                {
                    continue;
                }
                if (!std::isxdigit(static_cast<unsigned char>(currentChar)))
                {
                    if (errorTextOut != nullptr)
                    {
                        *errorTextOut = "hex payload contains non-hex character.";
                    }
                    return false;
                }
                normalizedHexText.push_back(currentChar);
            }

            // 十六进制字符必须成对出现，奇数长度视为格式错误。
            if ((normalizedHexText.size() % 2) != 0)
            {
                if (errorTextOut != nullptr)
                {
                    *errorTextOut = "hex payload length must be even.";
                }
                return false;
            }

            // 每两个字符解析为 1 字节。
            payloadBytesOut.reserve(normalizedHexText.size() / 2);
            for (std::size_t index = 0; index < normalizedHexText.size(); index += 2)
            {
                const std::string byteHexText = normalizedHexText.substr(index, 2);
                try
                {
                    const unsigned long byteValue = std::stoul(byteHexText, nullptr, 16);
                    payloadBytesOut.push_back(static_cast<std::uint8_t>(byteValue & 0xFFUL));
                }
                catch (const std::exception&)
                {
                    if (errorTextOut != nullptr)
                    {
                        *errorTextOut = "hex payload parse exception occurred.";
                    }
                    payloadBytesOut.clear();
                    return false;
                }
            }

            return true;
        }
    } // namespace request_detail

    // ManualNetworkApiKindToString：API 枚举转文本。
    inline std::string ManualNetworkApiKindToString(const ManualNetworkApiKind apiKind)
    {
        switch (apiKind)
        {
        case ManualNetworkApiKind::WinSockTcp: return "WinSockTcp";
        case ManualNetworkApiKind::WinSockUdp: return "WinSockUdp";
        default:                               return "UnknownApi";
        }
    }

    // ManualPayloadFormatToString：载荷格式枚举转文本。
    inline std::string ManualPayloadFormatToString(const ManualPayloadFormat payloadFormat)
    {
        switch (payloadFormat)
        {
        case ManualPayloadFormat::AsciiText: return "AsciiText";
        case ManualPayloadFormat::HexBytes:  return "HexBytes";
        default:                             return "UnknownFormat";
        }
    }

    // ExecuteManualNetworkRequest：
    // - 按 request 参数执行一次网络请求；
    // - 支持 TCP/UDP、可选 bind/connect、可选接收响应；
    // - 返回 true 表示请求流程成功，false 表示执行失败。
    inline bool ExecuteManualNetworkRequest(
        const ManualNetworkRequest& request,
        ManualNetworkResult* const resultOut)
    {
        if (resultOut == nullptr)
        {
            return false;
        }

        ManualNetworkResult localResult{};
        localResult.detailText.clear();

        // 启动 WSA 会话，失败则直接返回。
        request_detail::WsaSessionGuard wsaGuard;
        if (!wsaGuard.started())
        {
            localResult.succeeded = false;
            localResult.wsaErrorCode = wsaGuard.startupResult();
            localResult.detailText = "WSAStartup failed: " + request_detail::MakeWsaErrorText(localResult.wsaErrorCode);
            *resultOut = std::move(localResult);
            return false;
        }

        // 根据 API 模式决定默认 socket 参数；
        // 当 overrideSocketParameters=true 时使用 UI 显式覆盖值。
        int effectiveAddressFamily = request.addressFamily;
        int effectiveSocketType = request.socketType;
        int effectiveProtocol = request.protocol;
        if (!request.overrideSocketParameters)
        {
            if (request.apiKind == ManualNetworkApiKind::WinSockTcp)
            {
                effectiveAddressFamily = AF_INET;
                effectiveSocketType = SOCK_STREAM;
                effectiveProtocol = IPPROTO_TCP;
            }
            else
            {
                effectiveAddressFamily = AF_INET;
                effectiveSocketType = SOCK_DGRAM;
                effectiveProtocol = IPPROTO_UDP;
            }
        }

        // 当前实现仅支持 IPv4：如果用户手工改成其它 family，直接报错。
        if (effectiveAddressFamily != AF_INET)
        {
            localResult.succeeded = false;
            localResult.detailText = "Only AF_INET is supported in current manual request tool.";
            *resultOut = std::move(localResult);
            return false;
        }

        // 创建套接字。
        request_detail::SocketGuard socketGuard;
        const SOCKET socketValue = ::WSASocketW(
            effectiveAddressFamily,
            effectiveSocketType,
            effectiveProtocol,
            nullptr,
            0,
            request.socketFlags);
        if (socketValue == INVALID_SOCKET)
        {
            localResult.succeeded = false;
            localResult.wsaErrorCode = ::WSAGetLastError();
            localResult.detailText = "WSASocket failed: " + request_detail::MakeWsaErrorText(localResult.wsaErrorCode);
            *resultOut = std::move(localResult);
            return false;
        }
        socketGuard.reset(socketValue);

        // 套接字选项：SO_REUSEADDR、超时、TCP_NODELAY。
        if (request.enableReuseAddress)
        {
            const BOOL reuseAddress = TRUE;
            (void)::setsockopt(
                socketGuard.get(),
                SOL_SOCKET,
                SO_REUSEADDR,
                reinterpret_cast<const char*>(&reuseAddress),
                static_cast<int>(sizeof(reuseAddress)));
        }

        const int sendTimeoutMs = static_cast<int>(std::clamp<std::uint32_t>(request.sendTimeoutMs, 0U, 3600000U));
        const int receiveTimeoutMs = static_cast<int>(std::clamp<std::uint32_t>(request.receiveTimeoutMs, 0U, 3600000U));
        (void)::setsockopt(
            socketGuard.get(),
            SOL_SOCKET,
            SO_SNDTIMEO,
            reinterpret_cast<const char*>(&sendTimeoutMs),
            static_cast<int>(sizeof(sendTimeoutMs)));
        (void)::setsockopt(
            socketGuard.get(),
            SOL_SOCKET,
            SO_RCVTIMEO,
            reinterpret_cast<const char*>(&receiveTimeoutMs),
            static_cast<int>(sizeof(receiveTimeoutMs)));

        if (request.enableNoDelay && effectiveProtocol == IPPROTO_TCP)
        {
            const BOOL noDelay = TRUE;
            (void)::setsockopt(
                socketGuard.get(),
                IPPROTO_TCP,
                TCP_NODELAY,
                reinterpret_cast<const char*>(&noDelay),
                static_cast<int>(sizeof(noDelay)));
        }

        // 解析远端端点，失败时直接返回。
        sockaddr_in remoteEndpoint{};
        if (!request_detail::ResolveIpv4Endpoint(request.remoteAddress, request.remotePort, remoteEndpoint))
        {
            localResult.succeeded = false;
            localResult.detailText = "remote endpoint parse failed (IPv4 only).";
            *resultOut = std::move(localResult);
            return false;
        }

        // 可选绑定本地端点。
        if (request.enableLocalBind)
        {
            sockaddr_in localEndpoint{};
            if (!request_detail::ResolveIpv4Endpoint(request.localAddress, request.localPort, localEndpoint))
            {
                localResult.succeeded = false;
                localResult.detailText = "local bind endpoint parse failed (IPv4 only).";
                *resultOut = std::move(localResult);
                return false;
            }

            const int bindResult = ::bind(
                socketGuard.get(),
                reinterpret_cast<const sockaddr*>(&localEndpoint),
                static_cast<int>(sizeof(localEndpoint)));
            if (bindResult == SOCKET_ERROR)
            {
                localResult.succeeded = false;
                localResult.wsaErrorCode = ::WSAGetLastError();
                localResult.detailText = "bind failed: " + request_detail::MakeWsaErrorText(localResult.wsaErrorCode);
                *resultOut = std::move(localResult);
                return false;
            }
        }

        // connectBeforeSend=true 时先连接远端。
        if (request.connectBeforeSend)
        {
            const int connectResult = ::connect(
                socketGuard.get(),
                reinterpret_cast<const sockaddr*>(&remoteEndpoint),
                static_cast<int>(sizeof(remoteEndpoint)));
            if (connectResult == SOCKET_ERROR)
            {
                localResult.succeeded = false;
                localResult.wsaErrorCode = ::WSAGetLastError();
                localResult.detailText = "connect failed: " + request_detail::MakeWsaErrorText(localResult.wsaErrorCode);
                *resultOut = std::move(localResult);
                return false;
            }
        }

        // 构造发送载荷字节。
        std::vector<std::uint8_t> payloadBytes;
        if (request.payloadFormat == ManualPayloadFormat::AsciiText)
        {
            payloadBytes.assign(request.payloadText.begin(), request.payloadText.end());
        }
        else
        {
            std::string parseHexErrorText;
            if (!request_detail::ParseHexPayloadText(request.payloadText, payloadBytes, &parseHexErrorText))
            {
                localResult.succeeded = false;
                localResult.detailText = "payload parse failed: " + parseHexErrorText;
                *resultOut = std::move(localResult);
                return false;
            }
        }

        // 执行发送：
        // - connected 场景使用 send；
        // - 未 connect 的 UDP 场景使用 sendto。
        int sendResult = 0;
        if (request.connectBeforeSend)
        {
            sendResult = ::send(
                socketGuard.get(),
                reinterpret_cast<const char*>(payloadBytes.data()),
                static_cast<int>(payloadBytes.size()),
                request.sendFlags);
        }
        else
        {
            sendResult = ::sendto(
                socketGuard.get(),
                reinterpret_cast<const char*>(payloadBytes.data()),
                static_cast<int>(payloadBytes.size()),
                request.sendFlags,
                reinterpret_cast<const sockaddr*>(&remoteEndpoint),
                static_cast<int>(sizeof(remoteEndpoint)));
        }
        if (sendResult == SOCKET_ERROR)
        {
            localResult.succeeded = false;
            localResult.wsaErrorCode = ::WSAGetLastError();
            localResult.detailText = "send/sendto failed: " + request_detail::MakeWsaErrorText(localResult.wsaErrorCode);
            *resultOut = std::move(localResult);
            return false;
        }
        localResult.bytesSent = static_cast<std::size_t>(sendResult);

        // TCP 模式可选执行 shutdown(SD_SEND)，用于测试半关闭语义。
        if (request.shutdownSendAfterWrite && effectiveProtocol == IPPROTO_TCP)
        {
            const int shutdownResult = ::shutdown(socketGuard.get(), SD_SEND);
            if (shutdownResult == SOCKET_ERROR)
            {
                // 这里记录警告但不直接判定失败，避免影响后续 recv 调试流程。
                localResult.detailText +=
                    (" | shutdown(SD_SEND) failed: " + request_detail::MakeWsaErrorText(::WSAGetLastError()));
            }
        }

        // 可选接收响应：支持 connected recv 与 unconnected recvfrom。
        if (request.receiveAfterSend)
        {
            const std::size_t receiveLimitBytes = std::clamp<std::size_t>(request.receiveMaxBytes, 1ULL, 1024ULL * 1024ULL);
            localResult.responseBytes.resize(receiveLimitBytes);

            int receiveResult = 0;
            if (request.connectBeforeSend)
            {
                receiveResult = ::recv(
                    socketGuard.get(),
                    reinterpret_cast<char*>(localResult.responseBytes.data()),
                    static_cast<int>(localResult.responseBytes.size()),
                    request.receiveFlags);
            }
            else
            {
                sockaddr_in sourceEndpoint{};
                int sourceEndpointLength = static_cast<int>(sizeof(sourceEndpoint));
                receiveResult = ::recvfrom(
                    socketGuard.get(),
                    reinterpret_cast<char*>(localResult.responseBytes.data()),
                    static_cast<int>(localResult.responseBytes.size()),
                    request.receiveFlags,
                    reinterpret_cast<sockaddr*>(&sourceEndpoint),
                    &sourceEndpointLength);
            }

            if (receiveResult == SOCKET_ERROR)
            {
                const int receiveWsaError = ::WSAGetLastError();
                if (receiveWsaError == WSAETIMEDOUT || receiveWsaError == WSAEWOULDBLOCK)
                {
                    // 超时不算硬失败：请求已发出，但在超时窗口内未收到数据。
                    localResult.bytesReceived = 0;
                    localResult.responseBytes.clear();
                    localResult.succeeded = true;
                    localResult.wsaErrorCode = receiveWsaError;
                    localResult.detailText = "request sent, receive timeout: " + request_detail::MakeWsaErrorText(receiveWsaError);
                    *resultOut = std::move(localResult);
                    return true;
                }

                localResult.succeeded = false;
                localResult.wsaErrorCode = receiveWsaError;
                localResult.detailText = "recv/recvfrom failed: " + request_detail::MakeWsaErrorText(receiveWsaError);
                *resultOut = std::move(localResult);
                return false;
            }

            localResult.bytesReceived = static_cast<std::size_t>(receiveResult);
            localResult.responseBytes.resize(localResult.bytesReceived);
        }
        else
        {
            localResult.responseBytes.clear();
            localResult.bytesReceived = 0;
        }

        // 走到这里表示执行流程成功。
        localResult.succeeded = true;
        if (localResult.detailText.empty())
        {
            localResult.detailText = "manual network request succeeded.";
        }
        *resultOut = std::move(localResult);
        return true;
    }
} // namespace ks::network
