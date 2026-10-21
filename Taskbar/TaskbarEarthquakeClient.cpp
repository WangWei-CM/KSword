#include "TaskbarEarthquakeClient.h"
#include "TaskbarEarthquakeAudioPlayer.h"

#include <QJsonDocument>
#include <QJsonObject>
#include <QMetaObject>

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#include <winhttp.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <mutex>
#include <condition_variable>
#include <chrono>
#include <memory>
#include <limits>
#include <unordered_map>
#include <utility>
#include <thread>

#pragma comment(lib, "winhttp.lib")

namespace
{
    // 地震源定义沿用 WindowsMarker 已验证的多源配置，默认只连接三个中国实时来源。
    enum class WarningProvider
    {
        FanStudio, // FanStudio 聚合源的 update 报文。
        Wolfx      // Wolfx 各区域源的标准 EEW 报文。
    };

    struct SourceConfig
    {
        const char* id;             // 稳定来源标识。
        const char* name;           // 面向设置窗口的来源名称。
        const wchar_t* host;        // WebSocket 主机名。
        const wchar_t* path;        // WebSocket HTTPS 路径。
        WarningProvider provider;   // 当前来源的 JSON 解析器类型。
        bool enabledByDefault;      // 是否默认建立连接。
    };

    constexpr SourceConfig kSources[] = {
        { "fanstudio-cea", "FanStudio CEA", L"ws.fanstudio.tech", L"/all", WarningProvider::FanStudio, true },
        { "wolfx-cenc", "Wolfx CENC", L"ws-api.wolfx.jp", L"/cenc_eew", WarningProvider::Wolfx, true },
        { "wolfx-sc", "Wolfx Sichuan", L"ws-api.wolfx.jp", L"/sc_eew", WarningProvider::Wolfx, true },
        { "wolfx-fj", "Wolfx Fujian", L"ws-api.wolfx.jp", L"/fj_eew", WarningProvider::Wolfx, false },
        { "wolfx-cq", "Wolfx Chongqing", L"ws-api.wolfx.jp", L"/cq_eew", WarningProvider::Wolfx, false },
        { "wolfx-jma", "Wolfx JMA", L"ws-api.wolfx.jp", L"/jma_eew", WarningProvider::Wolfx, false }
    };
    constexpr int kSourceCount = static_cast<int>(std::size(kSources));
    constexpr ULONGLONG kActiveNoUpdateTimeoutMs = 90ull * 1000ull;
    constexpr DWORD kReconnectBaseMs = 2000;
    constexpr DWORD kReconnectMaxMs = 30000;
    constexpr DWORD kServerBusyReconnectMs = 60000;
    constexpr DWORD kKeepalivePingIntervalMs = 30000;

    // ParsedWarningMessage 是不同来源 JSON 统一后的内部报文表示。
    struct ParsedWarningMessage
    {
        bool hasEvent = false;      // 是否包含有效地震事件字段。
        bool heartbeat = false;     // 是否仅为心跳/ping/pong。
        bool finalReport = false;   // 当前来源是否已发布最终报。
        bool canceled = false;      // 当前来源是否已取消该预警。
        QString eventId;            // 原始事件标识，当前仅保留用于诊断。
        QString source;             // 服务端提供的来源文本。
        QString originTime;         // 地震发生时间。
        QString hypoCenter;         // 震中位置文本。
        double magnitude = 0.0;     // 报文震级。
        double depth = 0.0;         // 报文深度，单位公里。
        double latitude = 0.0;      // 震中纬度。
        double longitude = 0.0;     // 震中经度。
        int reportNum = 0;          // 当前报次。
    };

    // 读取 JSON 字符串时按多个候选字段名兼容 WindowsMarker 已支持的服务端变体。
    QString firstStringValue(const QJsonObject& object, std::initializer_list<const char*> keys)
    {
        for (const char* key : keys)
        {
            const QJsonValue value = object.value(QLatin1String(key));
            if (value.isString())
            {
                const QString text = value.toString().trimmed();
                if (!text.isEmpty())
                {
                    return text;
                }
            }
            if (value.isDouble())
            {
                return QString::number(value.toDouble(), 'f', 0);
            }
        }
        return QString();
    }

    // 读取 JSON 数值时保留 0 作为未提供或无效字段，避免用不可靠数据触发高等级音频。
    double firstDoubleValue(const QJsonObject& object, std::initializer_list<const char*> keys)
    {
        for (const char* key : keys)
        {
            const QJsonValue value = object.value(QLatin1String(key));
            if (value.isDouble())
            {
                return value.toDouble();
            }
            if (value.isString())
            {
                bool converted = false;
                const double number = value.toString().toDouble(&converted);
                if (converted)
                {
                    return number;
                }
            }
        }
        return 0.0;
    }

    // 读取 JSON 整数时容忍服务端把报次编码成字符串的情况。
    int firstIntValue(const QJsonObject& object, std::initializer_list<const char*> keys)
    {
        for (const char* key : keys)
        {
            const QJsonValue value = object.value(QLatin1String(key));
            if (value.isDouble())
            {
                return value.toInt();
            }
            if (value.isString())
            {
                bool converted = false;
                const int number = value.toString().toInt(&converted);
                if (converted)
                {
                    return number;
                }
            }
        }
        return 0;
    }

    // 读取 JSON 布尔值时同时识别布尔值和常见文本状态。
    bool firstBoolValue(const QJsonObject& object, std::initializer_list<const char*> keys)
    {
        for (const char* key : keys)
        {
            const QJsonValue value = object.value(QLatin1String(key));
            if (value.isBool())
            {
                return value.toBool();
            }
            if (value.isString())
            {
                const QString text = value.toString().trimmed().toLower();
                if (text == QStringLiteral("true") || text == QStringLiteral("yes") ||
                    text == QStringLiteral("final") || text == QStringLiteral("cancel") ||
                    text == QStringLiteral("cancelled"))
                {
                    return true;
                }
            }
        }
        return false;
    }

    // containsInsensitive 用于识别各种来源自定义的 status/type 字段而不依赖大小写。
    bool containsInsensitive(const QString& text, const QString& needle)
    {
        return text.contains(needle, Qt::CaseInsensitive);
    }

    // parseWolfxMessage 解析 Wolfx CENC、四川、福建、重庆和 JMA 的统一 EEW 报文。
    ParsedWarningMessage parseWolfxMessage(const QByteArray& payload)
    {
        ParsedWarningMessage parsed;
        const QJsonDocument document = QJsonDocument::fromJson(payload);
        if (!document.isObject())
        {
            return parsed;
        }

        const QJsonObject object = document.object();
        const QString type = firstStringValue(object, { "type", "Type" });
        parsed.heartbeat = containsInsensitive(type, QStringLiteral("heart")) ||
            containsInsensitive(type, QStringLiteral("ping")) ||
            containsInsensitive(type, QStringLiteral("pong")) ||
            object.contains(QStringLiteral("heartbeat"));
        parsed.eventId = firstStringValue(object, { "EventID", "EventId", "event_id", "ID", "id" });
        parsed.source = firstStringValue(object, { "Source", "source" });
        parsed.originTime = firstStringValue(object, { "OriginTime", "originTime", "origin_time" });
        parsed.hypoCenter = firstStringValue(object, { "HypoCenter", "Hypocenter", "hypocenter", "placeName" });
        parsed.magnitude = firstDoubleValue(object, { "Magnitude", "Magunitude", "magnitude" });
        parsed.depth = firstDoubleValue(object, { "Depth", "depth" });
        parsed.latitude = firstDoubleValue(object, { "Latitude", "latitude" });
        parsed.longitude = firstDoubleValue(object, { "Longitude", "longitude" });
        parsed.reportNum = firstIntValue(object, { "ReportNum", "Serial", "reportNum", "updates" });
        const QString status = firstStringValue(object, { "ReportStatus", "Status", "status" });
        parsed.finalReport = firstBoolValue(object, { "Final", "isFinal", "is_final" }) ||
            containsInsensitive(status, QStringLiteral("final"));
        parsed.canceled = firstBoolValue(object, { "Cancel", "Canceled", "Cancelled", "isCancel", "is_cancel" }) ||
            containsInsensitive(status, QStringLiteral("cancel"));
        parsed.hasEvent = !parsed.eventId.isEmpty() || !parsed.hypoCenter.isEmpty() || parsed.magnitude > 0.0;
        return parsed;
    }

    // parseFanStudioMessage 只接受 CEA/CEA-PR 的实时 update 数据，过滤其它聚合频道事件。
    ParsedWarningMessage parseFanStudioMessage(const QByteArray& payload)
    {
        ParsedWarningMessage parsed;
        const QJsonDocument document = QJsonDocument::fromJson(payload);
        if (!document.isObject())
        {
            return parsed;
        }

        const QJsonObject object = document.object();
        const QString type = firstStringValue(object, { "type", "Type" });
        parsed.heartbeat = containsInsensitive(type, QStringLiteral("heart")) ||
            containsInsensitive(type, QStringLiteral("ping")) ||
            containsInsensitive(type, QStringLiteral("pong"));
        if (parsed.heartbeat || !containsInsensitive(type, QStringLiteral("update")))
        {
            return parsed;
        }

        parsed.source = firstStringValue(object, { "source", "Source" }).toLower();
        if (parsed.source != QStringLiteral("cea") && parsed.source != QStringLiteral("cea-pr"))
        {
            return parsed;
        }

        parsed.eventId = firstStringValue(object, { "eventId", "id", "EventID" });
        parsed.originTime = firstStringValue(object, { "shockTime", "originTime", "OriginTime" });
        parsed.hypoCenter = firstStringValue(object, { "placeName", "HypoCenter" });
        parsed.magnitude = firstDoubleValue(object, { "magnitude", "Magnitude" });
        parsed.depth = firstDoubleValue(object, { "depth", "Depth" });
        parsed.latitude = firstDoubleValue(object, { "latitude", "Latitude" });
        parsed.longitude = firstDoubleValue(object, { "longitude", "Longitude" });
        parsed.reportNum = firstIntValue(object, { "updates", "ReportNum" });
        const QString status = firstStringValue(object, { "status", "Status" });
        parsed.finalReport = firstBoolValue(object, { "Final", "isFinal", "final" }) ||
            containsInsensitive(status, QStringLiteral("final"));
        parsed.canceled = firstBoolValue(object, { "Cancel", "isCancel", "cancel" }) ||
            containsInsensitive(status, QStringLiteral("cancel"));
        parsed.hasEvent = !parsed.eventId.isEmpty() || !parsed.hypoCenter.isEmpty();
        return parsed;
    }

    // parseMessageForProvider 分派各来源的 JSON 适配器，输出统一的预警字段。
    ParsedWarningMessage parseMessageForProvider(WarningProvider provider, const QByteArray& payload)
    {
        return provider == WarningProvider::FanStudio ? parseFanStudioMessage(payload) : parseWolfxMessage(payload);
    }

    // makeLocationKey 以经纬度 0.01 度精度合并多源的同一震中，缺坐标时回退到地点文本。
    QString makeLocationKey(const ParsedWarningMessage& parsed)
    {
        if (!qFuzzyIsNull(parsed.latitude) || !qFuzzyIsNull(parsed.longitude))
        {
            return QStringLiteral("%1,%2")
                .arg(parsed.latitude, 0, 'f', 2)
                .arg(parsed.longitude, 0, 'f', 2);
        }
        return parsed.hypoCenter.trimmed();
    }

    // AUX010 async transport core. No QObject or caller address enters WinHTTP context.
    // Native callbacks only copy completion values and wake worker-side waiters.
    class EarthquakeAsyncHandle;
    struct EarthquakeHandleRegistry
    {
        std::mutex mutex;
        DWORD_PTR nextId = 1;
        std::unordered_map<DWORD_PTR, std::shared_ptr<EarthquakeAsyncHandle>> handles;
    };
    EarthquakeHandleRegistry& earthquakeHandleRegistry()
    {
        // Normally empty after stop. A failed native CloseHandle must retain its
        // buffers/context for any late callback; the process reclaims quarantine.
        static auto* registry = new EarthquakeHandleRegistry;
        return *registry;
    }

    class EarthquakeAsyncHandle final
    {
    public:
        enum class Kind { Request, WebSocket };
        enum class Handshake { None, Send, Headers };
        struct Result
        {
            DWORD error = ERROR_WINHTTP_OPERATION_CANCELLED;
            DWORD bytes = 0;
            WINHTTP_WEB_SOCKET_BUFFER_TYPE type = WINHTTP_WEB_SOCKET_CLOSE_BUFFER_TYPE;
        };
        struct Slot
        {
            bool pending = false;
            bool done = false;
            Result result;
        };

        explicit EarthquakeAsyncHandle(Kind kind) : m_kind(kind) {}

        static std::shared_ptr<EarthquakeAsyncHandle> create(Kind kind)
        {
            auto value = std::make_shared<EarthquakeAsyncHandle>(kind);
            auto& registry = earthquakeHandleRegistry();
            std::lock_guard<std::mutex> lock(registry.mutex);
            if (registry.nextId == 0) { ::SetLastError(ERROR_ARITHMETIC_OVERFLOW); return {}; }
            value->m_id = registry.nextId;
            registry.nextId = registry.nextId == std::numeric_limits<DWORD_PTR>::max() ? 0 : registry.nextId + 1;
            registry.handles.emplace(value->m_id, value);
            return value;
        }
        DWORD_PTR id() const { return m_id; }

        static void CALLBACK statusCallback(HINTERNET handle, DWORD_PTR id, DWORD status, void* info, DWORD size) noexcept
        {
            if (id == 0 || status == WINHTTP_CALLBACK_STATUS_HANDLE_CREATED) return;
            try
            {
                std::shared_ptr<EarthquakeAsyncHandle> state;
                {
                    auto& registry = earthquakeHandleRegistry();
                    std::lock_guard<std::mutex> lock(registry.mutex);
                    const auto found = registry.handles.find(id);
                    if (found == registry.handles.end()) return;
                    state = found->second;
                }
                state->onStatus(handle, status, info, size);
            }
            catch (...) { /* Never unwind into WinHTTP; all normal paths allocate no memory. */ }
        }

        // The session callback is inherited before this handle can issue async work.
        // A socket context is supplied atomically by CompleteUpgrade; request context
        // is armed here before publishing the handle to the stopping thread.
        bool adopt(HINTERNET handle, bool contextAlreadySet)
        {
            std::lock_guard<std::mutex> api(m_apiMutex);
            {
                std::lock_guard<std::mutex> lock(m_mutex);
                m_identity = handle;
            }
            m_handle = handle;
            m_everAdopted = true;
            m_contextArmed = contextAlreadySet;
            if (!contextAlreadySet)
            {
                DWORD_PTR context = m_id;
                if (!::WinHttpSetOption(handle, WINHTTP_OPTION_CONTEXT_VALUE, &context, sizeof(context)))
                {
                    closeLocked();
                    return false;
                }
                m_contextArmed = true;
            }
            if (m_closeRequested) { closeLocked(); return false; }
            return true;
        }

        bool configureRequest(bool bypassProxy, DWORD* error)
        {
            std::lock_guard<std::mutex> api(m_apiMutex);
            if (!availableLocked()) { *error = ERROR_WINHTTP_OPERATION_CANCELLED; return false; }
            if (bypassProxy)
            {
                WINHTTP_PROXY_INFO proxy = {};
                proxy.dwAccessType = WINHTTP_ACCESS_TYPE_NO_PROXY;
                proxy.lpszProxy = WINHTTP_NO_PROXY_NAME;
                proxy.lpszProxyBypass = WINHTTP_NO_PROXY_BYPASS;
                if (!::WinHttpSetOption(m_handle, WINHTTP_OPTION_PROXY, &proxy, sizeof(proxy)))
                { *error = ::GetLastError(); return false; }
            }
            if (!::WinHttpSetOption(m_handle, WINHTTP_OPTION_UPGRADE_TO_WEB_SOCKET, nullptr, 0))
            { *error = ::GetLastError(); return false; }
            return true;
        }

        DWORD sendRequest()
        {
            {
                std::lock_guard<std::mutex> api(m_apiMutex);
                if (!availableLocked()) return ERROR_WINHTTP_OPERATION_CANCELLED;
                { std::lock_guard<std::mutex> lock(m_mutex); if (!begin(m_request)) return ERROR_INVALID_OPERATION; m_handshake = Handshake::Send; }
                if (!::WinHttpSendRequest(m_handle, WINHTTP_NO_ADDITIONAL_HEADERS, 0,
                    WINHTTP_NO_REQUEST_DATA, 0, 0, m_id))
                    immediateFailure(m_request, ::GetLastError());
            }
            return await(m_request).error;
        }
        DWORD receiveHeaders()
        {
            {
                std::lock_guard<std::mutex> api(m_apiMutex);
                if (!availableLocked()) return ERROR_WINHTTP_OPERATION_CANCELLED;
                { std::lock_guard<std::mutex> lock(m_mutex); if (!begin(m_request)) return ERROR_INVALID_OPERATION; m_handshake = Handshake::Headers; }
                if (!::WinHttpReceiveResponse(m_handle, nullptr)) immediateFailure(m_request, ::GetLastError());
            }
            return await(m_request).error;
        }
        bool queryStatus(DWORD* status, DWORD* error)
        {
            std::lock_guard<std::mutex> api(m_apiMutex);
            if (!availableLocked()) { *error = ERROR_WINHTTP_OPERATION_CANCELLED; return false; }
            DWORD bytes = sizeof(*status);
            if (!::WinHttpQueryHeaders(m_handle, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                WINHTTP_HEADER_NAME_BY_INDEX, status, &bytes, WINHTTP_NO_HEADER_INDEX))
            { *error = ::GetLastError(); return false; }
            return true;
        }
        HINTERNET upgrade(DWORD_PTR context, DWORD* error)
        {
            std::lock_guard<std::mutex> api(m_apiMutex);
            if (!availableLocked()) { *error = ERROR_WINHTTP_OPERATION_CANCELLED; return nullptr; }
            HINTERNET socket = ::WinHttpWebSocketCompleteUpgrade(m_handle, context);
            if (socket == nullptr) *error = ::GetLastError();
            return socket;
        }

        Result receive()
        {
            {
                std::lock_guard<std::mutex> api(m_apiMutex);
                if (!availableLocked()) return {};
                { std::lock_guard<std::mutex> lock(m_mutex); if (!begin(m_read)) return {ERROR_INVALID_OPERATION}; }
                const DWORD error = ::WinHttpWebSocketReceive(m_handle, m_receiveBuffer.data(),
                    static_cast<DWORD>(m_receiveBuffer.size()), nullptr, nullptr);
                if (error != ERROR_SUCCESS) immediateFailure(m_read, error);
            }
            return await(m_read);
        }
        DWORD sendPing()
        {
            {
                std::lock_guard<std::mutex> api(m_apiMutex);
                if (!availableLocked()) return ERROR_WINHTTP_OPERATION_CANCELLED;
                { std::lock_guard<std::mutex> lock(m_mutex); if (!begin(m_write)) return ERROR_INVALID_OPERATION; }
                const DWORD error = ::WinHttpWebSocketSend(m_handle, WINHTTP_WEB_SOCKET_UTF8_MESSAGE_BUFFER_TYPE,
                    m_pingBuffer.data(), static_cast<DWORD>(m_pingBuffer.size()));
                if (error != ERROR_SUCCESS) immediateFailure(m_write, error);
            }
            return await(m_write).error;
        }
        const unsigned char* receivedData() const { return m_receiveBuffer.data(); }

        void close()
        {
            std::lock_guard<std::mutex> api(m_apiMutex);
            m_closeRequested = true;
            closeLocked();
        }
        // A reserved upgrade context may have been canceled before a native handle
        // existed. No callback is promised for a failed creation.
        void abandonUncreated()
        {
            std::lock_guard<std::mutex> api(m_apiMutex);
            if (m_everAdopted) return;
            markTerminal(false);
            eraseContext();
        }
        bool waitClosed()
        {
            std::unique_lock<std::mutex> lock(m_mutex);
            m_changed.wait(lock, [this]() { return m_handleClosing || m_closeFailed; });
            return !m_closeFailed;
        }

    private:
        bool availableLocked() const { return m_handle != nullptr && !m_closeRequested; }
        static bool begin(Slot& slot)
        {
            if (slot.pending || slot.done) return false;
            slot = {}; slot.pending = true;
            return true;
        }
        static void complete(Slot& slot, const Result& result)
        {
            if (!slot.pending) return;
            slot.pending = false; slot.done = true; slot.result = result;
        }
        Result await(Slot& slot)
        {
            std::unique_lock<std::mutex> lock(m_mutex);
            m_changed.wait(lock, [this, &slot]() { return slot.done || m_handleClosing || m_closeFailed; });
            const Result result = slot.done ? slot.result : Result();
            slot.done = false; // The caller consumes the receipt before this lane may be reused.
            return result;
        }
        void immediateFailure(Slot& slot, DWORD error)
        {
            if (error == ERROR_IO_PENDING) return;
            std::lock_guard<std::mutex> lock(m_mutex);
            complete(slot, {error});
            m_changed.notify_all();
        }
        void eraseContext()
        {
            auto& registry = earthquakeHandleRegistry();
            std::lock_guard<std::mutex> lock(registry.mutex);
            registry.handles.erase(m_id);
        }
        void markTerminal(bool failed)
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            m_closeFailed = failed;
            m_handleClosing = !failed;
            complete(m_request, {}); complete(m_read, {}); complete(m_write, {});
            m_changed.notify_all();
        }
        void closeLocked()
        {
            // Serialize only API invocations, never completion waits. No native API
            // may reuse the raw value once we remove it before CloseHandle.
            {
                std::lock_guard<std::mutex> lock(m_mutex);
                complete(m_request, {}); complete(m_read, {}); complete(m_write, {});
                m_changed.notify_all();
            }
            HINTERNET handle = m_handle;
            m_handle = nullptr;
            if (handle == nullptr) return;
            const BOOL closed = ::WinHttpCloseHandle(handle);
            if (!m_contextArmed)
            {
                markTerminal(closed == FALSE);
                if (closed != FALSE) eraseContext();
            }
            else if (closed == FALSE)
            {
                // A failed close cannot justify freeing buffers or retrying a raw
                // handle. Retain the context independently of the Qt owner.
                markTerminal(true);
            }
        }
        void onStatus(HINTERNET handle, DWORD status, void* info, DWORD size)
        {
            bool terminal = false;
            {
                std::lock_guard<std::mutex> lock(m_mutex);
                if (handle != m_identity) return; // Stable ID plus identity rejects raw-handle ABA.
                if (status == WINHTTP_CALLBACK_STATUS_HANDLE_CLOSING)
                {
                    m_handleClosing = true;
                    complete(m_request, {}); complete(m_read, {}); complete(m_write, {});
                    terminal = true;
                }
                else if (status == WINHTTP_CALLBACK_STATUS_SENDREQUEST_COMPLETE && m_handshake == Handshake::Send)
                    complete(m_request, {ERROR_SUCCESS});
                else if (status == WINHTTP_CALLBACK_STATUS_HEADERS_AVAILABLE && m_handshake == Handshake::Headers)
                    complete(m_request, {ERROR_SUCCESS});
                else if ((status == WINHTTP_CALLBACK_STATUS_READ_COMPLETE || status == WINHTTP_CALLBACK_STATUS_WRITE_COMPLETE)
                    && m_kind == Kind::WebSocket)
                {
                    Result result{ERROR_WINHTTP_INVALID_SERVER_RESPONSE};
                    if (info != nullptr && size >= sizeof(WINHTTP_WEB_SOCKET_STATUS))
                    {
                        const auto& value = *static_cast<const WINHTTP_WEB_SOCKET_STATUS*>(info);
                        result = {ERROR_SUCCESS, value.dwBytesTransferred, value.eBufferType};
                        if (status == WINHTTP_CALLBACK_STATUS_READ_COMPLETE && result.bytes > m_receiveBuffer.size())
                            result.error = ERROR_WINHTTP_INVALID_SERVER_RESPONSE;
                    }
                    complete(status == WINHTTP_CALLBACK_STATUS_READ_COMPLETE ? m_read : m_write, result);
                }
                else if (status == WINHTTP_CALLBACK_STATUS_REQUEST_ERROR)
                {
                    DWORD error = ERROR_WINHTTP_INTERNAL_ERROR;
                    if (info != nullptr && size >= sizeof(WINHTTP_ASYNC_RESULT))
                        error = static_cast<const WINHTTP_ASYNC_RESULT*>(info)->dwError;
                    if (m_kind == Kind::Request) complete(m_request, {error});
                    else if (info != nullptr && size >= sizeof(WINHTTP_WEB_SOCKET_ASYNC_RESULT))
                    {
                        const auto operation = static_cast<const WINHTTP_WEB_SOCKET_ASYNC_RESULT*>(info)->Operation;
                        if (operation == WINHTTP_WEB_SOCKET_RECEIVE_OPERATION) complete(m_read, {error});
                        else if (operation == WINHTTP_WEB_SOCKET_SEND_OPERATION) complete(m_write, {error});
                        else { complete(m_read, {error}); complete(m_write, {error}); }
                    }
                    else { complete(m_read, {error}); complete(m_write, {error}); }
                }
                m_changed.notify_all();
            }
            if (terminal) eraseContext(); // HANDLE_CLOSING is WinHTTP's final callback.
        }

        const Kind m_kind;
        DWORD_PTR m_id = 0;
        std::mutex m_apiMutex;
        std::mutex m_mutex;
        std::condition_variable m_changed;
        HINTERNET m_handle = nullptr; // m_apiMutex only, removed before the first close call.
        HINTERNET m_identity = nullptr; // m_mutex; immutable after initial adoption.
        bool m_contextArmed = false;
        bool m_everAdopted = false;
        bool m_closeRequested = false;
        bool m_handleClosing = false;
        bool m_closeFailed = false;
        Handshake m_handshake = Handshake::None;
        Slot m_request, m_read, m_write;
        std::array<unsigned char, 8192> m_receiveBuffer{};
        std::array<char, 4> m_pingBuffer{'p', 'i', 'n', 'g'};
    };

    struct EarthquakeNativeHandle
    {
        HINTERNET handle = nullptr;
        explicit EarthquakeNativeHandle(HINTERNET value) : handle(value) {}
        ~EarthquakeNativeHandle() { if (handle != nullptr) ::WinHttpCloseHandle(handle); }
        HINTERNET release() { return std::exchange(handle, nullptr); }
        EarthquakeNativeHandle(const EarthquakeNativeHandle&) = delete;
        EarthquakeNativeHandle& operator=(const EarthquakeNativeHandle&) = delete;
    };
    struct EarthquakeAsyncHandleCleanup
    {
        std::shared_ptr<EarthquakeAsyncHandle> handle;
        ~EarthquakeAsyncHandleCleanup()
        {
            if (handle) { handle->close(); handle->abandonUncreated(); handle->waitClosed(); }
        }
    };
    // End AUX010 async transport core.

}

// Private 将 WinHTTP 线程、事件聚合和可选音频队列收束到实现文件，保持 Taskbar 公共头稳定。
class TaskbarEarthquakeClient::Private
{
public:
    struct SourceReading
    {
        bool present = false;      // 该来源是否曾报告该事件。
        bool ended = false;        // 该来源是否发布最终报或取消。
        bool canceled = false;     // 该来源是否明确取消。
        double magnitude = 0.0;    // 该来源的最近震级。
        double depth = 0.0;        // 该来源的最近深度。
        int reportNum = 0;         // 该来源的最近报次。
    };

    struct AggregatedEvent
    {
        QString locationKey;                       // 按震中位置聚合的键。
        QString hypoCenter;                        // 第一个有效位置文本。
        QString originTime;                        // 第一个有效发生时间。
        double latitude = 0.0;                      // 聚合后的纬度。
        double longitude = 0.0;                     // 聚合后的经度。
        ULONGLONG lastUpdateTick = 0;               // 最近有效报文时间。
        bool testEvent = false;                     // true 代表本地设置页测试预警。
        ULONGLONG testExpirationTick = 0;           // 测试预警的自动结束时间。
        std::array<SourceReading, kSourceCount> readings; // 每来源的当前读数。
    };

    struct SourceRuntime
    {
        std::thread thread;                  // 该来源唯一接收线程。
        std::mutex handlesMutex; // Only copies current value owners, never held across WinHTTP.
        std::shared_ptr<EarthquakeAsyncHandle> request;
        std::shared_ptr<EarthquakeAsyncHandle> socket;
        std::atomic_bool connected{ false }; // 当前连接状态。
        std::atomic_bool everConnected{ false }; // 本进程中至少一次成功连接。
        std::atomic_bool preferDirect{ false }; // 上次直连成功后优先跳过系统代理。
        std::atomic<ULONGLONG> lastMessageTick{ 0 }; // 最近数据/心跳到达时间。
        std::atomic<ULONGLONG> lastPingTick{ 0 }; // 最近发送 ping 的单调时钟。
        std::atomic<ULONGLONG> latencyMs{ 0 }; // ping/pong 的往返延迟。
    };

    // 构造函数：保存外部 QObject，创建运行目录音频播放器但不加载任何嵌入式音频资源。
    explicit Private(TaskbarEarthquakeClient* owner)
        : owner(owner)
        , audioPlayer(new TaskbarEarthquakeAudioPlayer(owner))
    {
    }

    // 析构函数：调用 stop 后释放成员；线程对象均已 join，不存在悬空 WinHTTP 句柄。
    ~Private()
    {
        stop();
    }

    // start：启动默认来源的工作线程；重复调用不创建第二组连接。
    void start()
    {
        if (started.exchange(true)) return;
        stopRequested.store(false);
        try
        {
            for (int sourceIndex = 0; sourceIndex < kSourceCount; ++sourceIndex)
            {
                if (!kSources[sourceIndex].enabledByDefault) continue;
                runtimes[sourceIndex].thread = std::thread([this, sourceIndex]() {
                    try { sourceThreadMain(sourceIndex); }
                    catch (...)
                    {
                        closeSourceSocket(sourceIndex);
                        runtimes[sourceIndex].connected.store(false);
                        postSourceStatusChange();
                    }
                });
            }
            keepaliveThread = std::thread(&Private::keepaliveThreadMain, this);
        }
        catch (...)
        {
            // Partial thread creation still cancels and joins every accepted worker.
            stop();
            postSourceStatusChange();
        }
    }

    // stop：取消握手与收发，唤醒退避等待，再等待来源线程与最终回调。
    void stop()
    {
        if (!started.exchange(false)) return;
        {
            // Pair the predicate store with waitUntilStop's mutex so a notify
            // cannot be lost between its predicate check and atomic wait.
            std::lock_guard<std::mutex> lock(stopWaitMutex);
            stopRequested.store(true);
        }
        stopChanged.notify_all();
        // Cancel each request/socket, including send/DNS/TLS/header/upgrade phases.
        // Handle close serializes only native invocation return, never a network wait.
        for (int sourceIndex = 0; sourceIndex < kSourceCount; ++sourceIndex) closeSourceSocket(sourceIndex);
        if (keepaliveThread.joinable()) keepaliveThread.join();
        for (SourceRuntime& runtime : runtimes)
            if (runtime.thread.joinable()) runtime.thread.join();
    }

    // warningsSnapshot：清理超时事件后构造 UI 不可变快照，调用方不持有锁。
    QList<TaskbarEarthquakeEvent> warningsSnapshot()
    {
        std::lock_guard<std::mutex> lock(eventsMutex);
        const ULONGLONG now = ::GetTickCount64();
        pruneExpiredEventsLocked(now);
        QList<TaskbarEarthquakeEvent> warnings;
        for (const AggregatedEvent& event : events)
        {
            if (!isEventActiveLocked(event, now))
            {
                continue;
            }
            TaskbarEarthquakeEvent view;
            view.locationKey = event.locationKey;
            view.hypoCenter = event.hypoCenter;
            view.originTime = event.originTime;
            view.magnitudeText = combinedReadingText(event, true);
            view.depthText = combinedReadingText(event, false);
            view.sourcesText = activeSourcesText(event);
            view.reportNum = maxActiveReportNumber(event);
            view.sourceCount = activeSourceCount(event);
            warnings.push_back(view);
        }
        return warnings;
    }

    // statusesSnapshot：从原子连接状态读取诊断信息，不持有网络句柄锁。
    QList<TaskbarEarthquakeSourceStatus> statusesSnapshot() const
    {
        const ULONGLONG now = ::GetTickCount64();
        QList<TaskbarEarthquakeSourceStatus> statuses;
        for (int sourceIndex = 0; sourceIndex < kSourceCount; ++sourceIndex)
        {
            const SourceRuntime& runtime = runtimes[sourceIndex];
            TaskbarEarthquakeSourceStatus status;
            status.id = QLatin1String(kSources[sourceIndex].id);
            status.name = QLatin1String(kSources[sourceIndex].name);
            status.enabled = kSources[sourceIndex].enabledByDefault;
            status.connected = runtime.connected.load();
            status.everConnected = runtime.everConnected.load();
            const ULONGLONG lastMessage = runtime.lastMessageTick.load();
            status.lastMessageAgeMs = lastMessage != 0 && now >= lastMessage ? now - lastMessage : 0;
            status.latencyMs = runtime.latencyMs.load();
            statuses.push_back(status);
        }
        return statuses;
    }

    // injectTestWarning：创建一条十秒测试预警，刻意绕过网络而复用实际活动态和红色主题路径。
    void injectTestWarning()
    {
        const ULONGLONG now = ::GetTickCount64();
        {
            std::lock_guard<std::mutex> lock(eventsMutex);
            events.erase(std::remove_if(events.begin(), events.end(), [](const AggregatedEvent& event) {
                return event.testEvent;
            }), events.end());

            AggregatedEvent testEvent;
            testEvent.locationKey = QStringLiteral("__taskbar_test_earthquake__");
            testEvent.hypoCenter = QStringLiteral("四川省阿坝州汶川县");
            testEvent.originTime = QStringLiteral("测试预警");
            testEvent.latitude = 31.0;
            testEvent.longitude = 103.4;
            testEvent.lastUpdateTick = now;
            testEvent.testEvent = true;
            testEvent.testExpirationTick = now + 10ull * 1000ull;
            SourceReading& reading = testEvent.readings[0];
            reading.present = true;
            reading.magnitude = 6.0;
            reading.depth = 10.0;
            reading.reportNum = 1;
            events.push_back(std::move(testEvent));
        }
        postWarningChange();
        queueAudioForReport(6.0, false);
    }

    // setAlertAudioEnabled：仅控制可选 sounds/ 播放队列；关闭时清空尚未播放内容并立即静音。
    void setAlertAudioEnabled(bool enabled)
    {
        alertAudioEnabled = enabled;
        if (audioPlayer != nullptr)
        {
            audioPlayer->setEnabled(alertAudioEnabled);
        }
    }

    struct PublishedSourceHandle
    {
        SourceRuntime& runtime;
        std::shared_ptr<EarthquakeAsyncHandle> value;
        bool request;
        bool active = true;
        PublishedSourceHandle(SourceRuntime& source, std::shared_ptr<EarthquakeAsyncHandle> handle, bool isRequest)
            : runtime(source), value(std::move(handle)), request(isRequest)
        {
            std::lock_guard<std::mutex> lock(runtime.handlesMutex);
            (request ? runtime.request : runtime.socket) = value;
        }
        ~PublishedSourceHandle()
        {
            if (!active) return;
            std::lock_guard<std::mutex> lock(runtime.handlesMutex);
            auto& current = request ? runtime.request : runtime.socket;
            if (current == value) current.reset();
        }
    };
    bool waitUntilStop(DWORD milliseconds)
    {
        std::unique_lock<std::mutex> lock(stopWaitMutex);
        return stopChanged.wait_for(lock, std::chrono::milliseconds(milliseconds), [this]() { return stopRequested.load(); });
    }

    // sourceThreadMain：维护单个来源的 WinHTTP WebSocket，失败时指数退避重连。
    void sourceThreadMain(int sourceIndex)
    {
        SourceRuntime& runtime = runtimes[sourceIndex];
        const SourceConfig& config = kSources[sourceIndex];
        EarthquakeNativeHandle session(::WinHttpOpen(L"KSword Taskbar/1.0 EEW", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
            WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, WINHTTP_FLAG_ASYNC));
        if (session.handle == nullptr) return;
        if (::WinHttpSetStatusCallback(session.handle, EarthquakeAsyncHandle::statusCallback,
            WINHTTP_CALLBACK_FLAG_ALL_COMPLETIONS | WINHTTP_CALLBACK_FLAG_HANDLES, 0) == WINHTTP_INVALID_STATUS_CALLBACK) return;
        ::WinHttpSetTimeouts(session.handle, 5000, 5000, 5000, 90000);
        DWORD secureProtocols = WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_2;
#ifdef WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_3
        secureProtocols |= WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_3;
#endif
        ::WinHttpSetOption(session.handle, WINHTTP_OPTION_SECURE_PROTOCOLS, &secureProtocols, sizeof(secureProtocols));

        DWORD reconnectDelayMs = kReconnectBaseMs;
        while (!stopRequested.load())
        {
            DWORD httpStatus = 0, lastError = ERROR_SUCCESS;
            bool usedDirect = runtime.preferDirect.load();
            auto socket = connectSourceWebSocket(sourceIndex, session.handle, config, usedDirect, &httpStatus, &lastError);
            if (!socket && httpStatus == 0 && lastError != ERROR_WINHTTP_TIMEOUT && !stopRequested.load())
            {
                usedDirect = !usedDirect;
                socket = connectSourceWebSocket(sourceIndex, session.handle, config, usedDirect, &httpStatus, &lastError);
            }
            if (!socket)
            {
                runtime.connected.store(false);
                postSourceStatusChange();
                const bool serverBusy = httpStatus == 429 || httpStatus == 503 || lastError == ERROR_WINHTTP_TIMEOUT;
                waitUntilStop(serverBusy ? kServerBusyReconnectMs : reconnectDelayMs);
                reconnectDelayMs = std::min<DWORD>(kReconnectMaxMs, reconnectDelayMs + kReconnectBaseMs);
                continue;
            }

            EarthquakeAsyncHandleCleanup socketCleanup{socket};
            reconnectDelayMs = kReconnectBaseMs;
            runtime.preferDirect.store(usedDirect);
            runtime.lastPingTick.store(0); // A ping from an old transport cannot belong to this epoch.
            if (!stopRequested.load())
            {
                runtime.connected.store(true);
                runtime.everConnected.store(true);
                postSourceStatusChange();
                receiveSourceMessages(sourceIndex, socket);
            }
            closeSourceSocketIfCurrent(sourceIndex, socket);
            runtime.connected.store(false);
            postSourceStatusChange();
            if (!stopRequested.load()) waitUntilStop(reconnectDelayMs);
        }
    }

    // connectSourceWebSocket：完成 HTTPS 升级，返回成功的 WebSocket 或 nullptr，并回传失败类型。
    std::shared_ptr<EarthquakeAsyncHandle> connectSourceWebSocket(int sourceIndex, HINTERNET session,
        const SourceConfig& config, bool bypassProxy, DWORD* httpStatus, DWORD* lastError)
    {
        *httpStatus = 0;
        *lastError = ERROR_SUCCESS;
        if (stopRequested.load()) { *lastError = ERROR_WINHTTP_OPERATION_CANCELLED; return {}; }
        EarthquakeNativeHandle connection(::WinHttpConnect(session, config.host, INTERNET_DEFAULT_HTTPS_PORT, 0));
        if (connection.handle == nullptr) { *lastError = ::GetLastError(); return {}; }
        EarthquakeNativeHandle nativeRequest(::WinHttpOpenRequest(connection.handle, L"GET", config.path, nullptr,
            WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE));
        if (nativeRequest.handle == nullptr) { *lastError = ::GetLastError(); return {}; }
        auto request = EarthquakeAsyncHandle::create(EarthquakeAsyncHandle::Kind::Request);
        if (!request) { *lastError = ::GetLastError(); return {}; }
        EarthquakeAsyncHandleCleanup requestCleanup{request};
        const bool requestAdopted = request->adopt(nativeRequest.handle, false);
        nativeRequest.release(); // adopt owns/closes it even when context setup fails.
        if (!requestAdopted) { *lastError = ::GetLastError(); return {}; }
        PublishedSourceHandle publishedRequest(runtimes[sourceIndex], request, true);
        if (stopRequested.load()) { *lastError = ERROR_WINHTTP_OPERATION_CANCELLED; return {}; }
        if (!request->configureRequest(bypassProxy, lastError)) return {};
        *lastError = request->sendRequest();
        if (*lastError != ERROR_SUCCESS) return {};
        if (stopRequested.load()) { *lastError = ERROR_WINHTTP_OPERATION_CANCELLED; return {}; }
        *lastError = request->receiveHeaders();
        if (*lastError != ERROR_SUCCESS) return {};
        if (!request->queryStatus(httpStatus, lastError) || *httpStatus != 101) return {};

        auto socket = EarthquakeAsyncHandle::create(EarthquakeAsyncHandle::Kind::WebSocket);
        if (!socket) { *lastError = ::GetLastError(); return {}; }
        EarthquakeAsyncHandleCleanup socketCleanup{socket};
        PublishedSourceHandle publishedSocket(runtimes[sourceIndex], socket, false);
        if (stopRequested.load()) { *lastError = ERROR_WINHTTP_OPERATION_CANCELLED; return {}; }
        EarthquakeNativeHandle nativeSocket(request->upgrade(socket->id(), lastError));
        if (nativeSocket.handle == nullptr) return {};
        const bool socketAdopted = socket->adopt(nativeSocket.handle, true);
        nativeSocket.release();
        if (!socketAdopted || stopRequested.load())
        { *lastError = ERROR_WINHTTP_OPERATION_CANCELLED; return {}; }
        // Transfer the upgraded state to the receive loop. Request/connection
        // scope cleanup completes independently before this return is published.
        socketCleanup.handle.reset();
        publishedSocket.active = false;
        return socket;
    }

    // receiveSourceMessages：接收分片文本帧，完整报文才交给 JSON 解析和事件聚合。
    void receiveSourceMessages(int sourceIndex, const std::shared_ptr<EarthquakeAsyncHandle>& socket)
    {
        SourceRuntime& runtime = runtimes[sourceIndex];
        QByteArray message;
        while (!stopRequested.load())
        {
            const auto result = socket->receive();
            if (result.error != ERROR_SUCCESS || result.type == WINHTTP_WEB_SOCKET_CLOSE_BUFFER_TYPE || stopRequested.load()) break;
            const ULONGLONG now = ::GetTickCount64();
            runtime.lastMessageTick.store(now);
            const ULONGLONG lastPing = runtime.lastPingTick.exchange(0);
            if (lastPing != 0 && now >= lastPing) runtime.latencyMs.store(now - lastPing);
            postSourceStatusChange();
            if (result.type != WINHTTP_WEB_SOCKET_UTF8_FRAGMENT_BUFFER_TYPE
                && result.type != WINHTTP_WEB_SOCKET_UTF8_MESSAGE_BUFFER_TYPE) continue;
            message.append(reinterpret_cast<const char*>(socket->receivedData()), static_cast<qsizetype>(result.bytes));
            if (result.type == WINHTTP_WEB_SOCKET_UTF8_FRAGMENT_BUFFER_TYPE) continue;
            const ParsedWarningMessage parsed = parseMessageForProvider(kSources[sourceIndex].provider, message);
            message.clear();
            applySourceEvent(sourceIndex, parsed);
        }
    }

    // applySourceEvent：把来源报文合并至按坐标去重的事件表，并把变化异步通知 GUI 线程。
    void applySourceEvent(int sourceIndex, const ParsedWarningMessage& parsed)
    {
        if (!parsed.hasEvent || parsed.heartbeat)
        {
            return;
        }

        const QString locationKey = makeLocationKey(parsed);
        if (locationKey.isEmpty())
        {
            return;
        }

        const ULONGLONG now = ::GetTickCount64();
        bool stateChanged = false;
        {
            std::lock_guard<std::mutex> lock(eventsMutex);
            auto iterator = std::find_if(events.begin(), events.end(), [&locationKey](const AggregatedEvent& event) {
                return event.locationKey == locationKey;
            });
            if (iterator == events.end())
            {
                if (parsed.canceled)
                {
                    return;
                }
                AggregatedEvent event;
                event.locationKey = locationKey;
                event.hypoCenter = parsed.hypoCenter;
                event.originTime = parsed.originTime;
                event.latitude = parsed.latitude;
                event.longitude = parsed.longitude;
                event.lastUpdateTick = now;
                events.push_back(std::move(event));
                iterator = std::prev(events.end());
            }

            AggregatedEvent& event = *iterator;
            if (event.hypoCenter.isEmpty())
            {
                event.hypoCenter = parsed.hypoCenter;
            }
            if (event.originTime.isEmpty())
            {
                event.originTime = parsed.originTime;
            }
            if (qFuzzyIsNull(event.latitude) && qFuzzyIsNull(event.longitude))
            {
                event.latitude = parsed.latitude;
                event.longitude = parsed.longitude;
            }
            event.lastUpdateTick = now;
            SourceReading& reading = event.readings[sourceIndex];
            reading.present = true;
            reading.ended = parsed.finalReport || parsed.canceled;
            reading.canceled = parsed.canceled;
            reading.magnitude = parsed.magnitude;
            reading.depth = parsed.depth;
            reading.reportNum = parsed.reportNum;
            stateChanged = true;
        }

        if (stateChanged)
        {
            postWarningChange();
            TaskbarEarthquakeClient* target = owner;
            QMetaObject::invokeMethod(target, [target, magnitude = parsed.magnitude, canceled = parsed.canceled]() {
                emit target->reportReceived(magnitude, canceled);
            }, Qt::QueuedConnection);
        }
    }

    // closeSourceSocket：关闭一个当前套接字，WinHTTP receive 将返回并允许对应线程退出或重连。
    void closeSourceSocket(int sourceIndex)
    {
        SourceRuntime& runtime = runtimes[sourceIndex];
        std::shared_ptr<EarthquakeAsyncHandle> request, socket;
        {
            std::lock_guard<std::mutex> lock(runtime.handlesMutex);
            request = runtime.request;
            socket = runtime.socket;
        }
        // No runtime mutex is held during native invocation/cancellation or waits.
        if (request) request->close();
        if (socket) socket->close();
    }

    // closeSourceSocketIfCurrent：仅接收线程仍拥有该句柄时关闭，避免 stop() 与接收线程双重关闭。
    void closeSourceSocketIfCurrent(int sourceIndex, const std::shared_ptr<EarthquakeAsyncHandle>& socket)
    {
        SourceRuntime& runtime = runtimes[sourceIndex];
        {
            std::lock_guard<std::mutex> lock(runtime.handlesMutex);
            if (runtime.socket == socket) runtime.socket.reset();
        }
        // Identity is the shared state/unique context, never a reusable raw value.
        if (socket) socket->close();
    }

    // sendSourcePing：保活线程是唯一发送方，完成等待不占 runtime/API 锁。
    void sendSourcePing(int sourceIndex)
    {
        SourceRuntime& runtime = runtimes[sourceIndex];
        std::shared_ptr<EarthquakeAsyncHandle> socket;
        {
            std::lock_guard<std::mutex> lock(runtime.handlesMutex);
            socket = runtime.socket;
        }
        if (!socket || !runtime.connected.load() || stopRequested.load()) return;
        const ULONGLONG now = ::GetTickCount64();
        const DWORD error = socket->sendPing(); // Completion wait holds no socket/runtime/API lock.
        if (error != ERROR_SUCCESS || stopRequested.load()) return;
        std::lock_guard<std::mutex> lock(runtime.handlesMutex);
        if (runtime.socket == socket && runtime.connected.load()) runtime.lastPingTick.store(now);
    }

    // keepaliveThreadMain：每 30 秒发送一次轻量 ping，接收线程仍独占 WebSocket receive 调用。
    void keepaliveThreadMain()
    {
        while (!stopRequested.load())
        {
            for (int sourceIndex = 0; sourceIndex < kSourceCount; ++sourceIndex)
            {
                if (stopRequested.load())
                {
                    break;
                }
                if (kSources[sourceIndex].enabledByDefault)
                {
                    sendSourcePing(sourceIndex);
                }
            }
            waitUntilStop(kKeepalivePingIntervalMs);
        }
    }

    // isEventActiveLocked：明确取消后结束；最终报仍保留至来源取消或九十秒无更新，保持 WindowsMarker 语义。
    bool isEventActiveLocked(const AggregatedEvent& event, ULONGLONG now) const
    {
        if (event.testEvent)
        {
            return now < event.testExpirationTick;
        }
        if (now >= event.lastUpdateTick && now - event.lastUpdateTick > kActiveNoUpdateTimeoutMs)
        {
            return false;
        }
        return activeSourceCount(event) > 0;
    }

    // pruneExpiredEventsLocked：清除超时、测试到期和所有来源终止的事件，控制内存和误报警。
    void pruneExpiredEventsLocked(ULONGLONG now)
    {
        events.erase(std::remove_if(events.begin(), events.end(), [this, now](const AggregatedEvent& event) {
            return !isEventActiveLocked(event, now);
        }), events.end());
    }

    // activeSourceCount：计算未取消来源数量；最终报只标注来源状态，不会提前结束警报。
    int activeSourceCount(const AggregatedEvent& event) const
    {
        int count = 0;
        for (const SourceReading& reading : event.readings)
        {
            if (reading.present && !reading.canceled)
            {
                ++count;
            }
        }
        return count;
    }

    // maxActiveReportNumber：返回活动来源中的最大报次，方便用户判断预警是否已更新。
    int maxActiveReportNumber(const AggregatedEvent& event) const
    {
        int maximum = 0;
        for (const SourceReading& reading : event.readings)
        {
            if (reading.present && !reading.canceled)
            {
                maximum = std::max(maximum, reading.reportNum);
            }
        }
        return maximum;
    }

    // combinedReadingText：合并当前活动来源的震级或深度，去重后以斜杠分隔。
    QString combinedReadingText(const AggregatedEvent& event, bool magnitude) const
    {
        QStringList values;
        for (const SourceReading& reading : event.readings)
        {
            if (!reading.present || reading.canceled)
            {
                continue;
            }
            const double value = magnitude ? reading.magnitude : reading.depth;
            if (value <= 0.0)
            {
                continue;
            }
            const QString text = QString::number(value, 'f', magnitude ? 1 : 0);
            if (!values.contains(text))
            {
                values.push_back(text);
            }
        }
        return values.join(QLatin1Char('/'));
    }

    // activeSourcesText：把所有当前确认来源的名称拼接为紧凑诊断文本。
    QString activeSourcesText(const AggregatedEvent& event) const
    {
        QStringList names;
        for (int sourceIndex = 0; sourceIndex < kSourceCount; ++sourceIndex)
        {
            const SourceReading& reading = event.readings[sourceIndex];
            if (reading.present && !reading.canceled)
            {
                names.push_back(QLatin1String(kSources[sourceIndex].name));
            }
        }
        return names.join(QStringLiteral(" / "));
    }

    // postWarningChange：把来自网络线程的状态变更排队到 owner 所在线程，避免跨线程 Qt UI 访问。
    void postWarningChange()
    {
        TaskbarEarthquakeClient* target = owner;
        QMetaObject::invokeMethod(target, [target]() {
            emit target->activeWarningsChanged();
        }, Qt::QueuedConnection);
    }

    // postSourceStatusChange：把连接状态刷新排队到 GUI 线程，设置窗口可安全读取快照。
    void postSourceStatusChange()
    {
        TaskbarEarthquakeClient* target = owner;
        QMetaObject::invokeMethod(target, [target]() {
            emit target->sourceStatusesChanged();
        }, Qt::QueuedConnection);
    }

    // queueAudioForReport：转发报告给运行目录 sounds/ 播放器，不复制或嵌入 WindowsMarker 音频资源。
    void queueAudioForReport(double magnitude, bool canceled)
    {
        if (!alertAudioEnabled || audioPlayer == nullptr)
        {
            return;
        }
        audioPlayer->enqueueReport(magnitude, canceled);
    }

    TaskbarEarthquakeClient* owner;          // 外部 QObject，用于安全发布信号。
    std::atomic_bool started{ false };       // start/stop 幂等保护。
    std::atomic_bool stopRequested{ false }; // 后台线程退出标记。
    std::mutex stopWaitMutex;
    std::condition_variable stopChanged; // Wakes keepalive/reconnect immediately, without polling sleeps.
    std::array<SourceRuntime, kSourceCount> runtimes; // 每预警源的线程与连接状态。
    std::thread keepaliveThread;             // 唯一保活发送线程，不与接收线程竞争 send 调用。
    mutable std::mutex eventsMutex;          // 保护 events 的合并和清理操作。
    QList<AggregatedEvent> events;           // 当前及等待清理的聚合地震事件。
    TaskbarEarthquakeAudioPlayer* audioPlayer; // 串行播放 exe 同目录 sounds/ 中的存在文件。
    bool alertAudioEnabled = true;            // 地震开关关闭后禁止声音队列播放。
};

TaskbarEarthquakeClient::TaskbarEarthquakeClient(QObject* parent)
    : QObject(parent)
    , m_private(new Private(this))
{
    // reportReceived 在 GUI 线程触发可选音频队列；网络线程本身从不触碰 Qt Multimedia 对象。
    connect(this, &TaskbarEarthquakeClient::reportReceived, this,
        [this](double magnitude, bool canceled) {
            m_private->queueAudioForReport(magnitude, canceled);
        });
}

TaskbarEarthquakeClient::~TaskbarEarthquakeClient()
{
    // 析构顺序先停止所有网络线程，再释放私有状态和其 Qt Multimedia 子对象。
    m_private->stop();
    delete m_private;
    m_private = nullptr;
}

void TaskbarEarthquakeClient::start()
{
    // Taskbar 启动时调用一次；客户端内部负责过滤不默认启用的来源。
    m_private->start();
}

void TaskbarEarthquakeClient::stop()
{
    // Taskbar 进程退出时调用；本方法可在析构前显式调用，也可由析构兜底。
    m_private->stop();
}

QList<TaskbarEarthquakeEvent> TaskbarEarthquakeClient::activeWarnings() const
{
    // UI 读取的列表是深拷贝快照，因此返回后不依赖网络线程或互斥锁。
    return m_private->warningsSnapshot();
}

QList<TaskbarEarthquakeSourceStatus> TaskbarEarthquakeClient::sourceStatuses() const
{
    // 连接诊断同样以快照形式返回，设置窗口不会长期引用网络线程状态。
    return m_private->statusesSnapshot();
}

void TaskbarEarthquakeClient::injectTestWarning()
{
    // 设置页按钮调用此路径，使用真实的事件聚合和警报主题逻辑而不依赖外部网络。
    m_private->injectTestWarning();
}

void TaskbarEarthquakeClient::setAlertAudioEnabled(bool enabled)
{
    // 设置窗口复用地震通知开关控制可选音频；不存在 sounds/ 时仍保持静默。
    m_private->setAlertAudioEnabled(enabled);
}
