#include "pch.h"
#include "HookTargets.h"
#include "HookEngine.h"
#include "../MonitorAgent.h"
#include "../core/MonitorPipe.h"

#include <WinReg.h>

namespace apimon
{
    namespace
    {
        using CreateFileWFn = HANDLE(WINAPI*)(LPCWSTR, DWORD, DWORD, LPSECURITY_ATTRIBUTES, DWORD, DWORD, HANDLE);
        using ReadFileFn = BOOL(WINAPI*)(HANDLE, LPVOID, DWORD, LPDWORD, LPOVERLAPPED);
        using WriteFileFn = BOOL(WINAPI*)(HANDLE, LPCVOID, DWORD, LPDWORD, LPOVERLAPPED);
        using CreateProcessWFn = BOOL(WINAPI*)(LPCWSTR, LPWSTR, LPSECURITY_ATTRIBUTES, LPSECURITY_ATTRIBUTES, BOOL, DWORD, LPVOID, LPCWSTR, LPSTARTUPINFOW, LPPROCESS_INFORMATION);
        using LoadLibraryWFn = HMODULE(WINAPI*)(LPCWSTR);
        using LoadLibraryExWFn = HMODULE(WINAPI*)(LPCWSTR, HANDLE, DWORD);
        using RegOpenKeyExWFn = LSTATUS(WINAPI*)(HKEY, LPCWSTR, DWORD, REGSAM, PHKEY);
        using RegCreateKeyExWFn = LSTATUS(WINAPI*)(HKEY, LPCWSTR, DWORD, LPWSTR, DWORD, REGSAM, const LPSECURITY_ATTRIBUTES, PHKEY, LPDWORD);
        using RegQueryValueExWFn = LSTATUS(WINAPI*)(HKEY, LPCWSTR, LPDWORD, LPDWORD, LPBYTE, LPDWORD);
        using RegGetValueWFn = LSTATUS(WINAPI*)(HKEY, LPCWSTR, LPCWSTR, DWORD, LPDWORD, PVOID, LPDWORD);
        using RegSetValueExWFn = LSTATUS(WINAPI*)(HKEY, LPCWSTR, DWORD, DWORD, const BYTE*, DWORD);
        using RegDeleteValueWFn = LSTATUS(WINAPI*)(HKEY, LPCWSTR);
        using RegDeleteKeyWFn = LSTATUS(WINAPI*)(HKEY, LPCWSTR);
        using RegDeleteKeyExWFn = LSTATUS(WINAPI*)(HKEY, LPCWSTR, REGSAM, DWORD);
        using RegEnumKeyExWFn = LSTATUS(WINAPI*)(HKEY, DWORD, LPWSTR, LPDWORD, LPDWORD, LPWSTR, LPDWORD, PFILETIME);
        using RegEnumValueWFn = LSTATUS(WINAPI*)(HKEY, DWORD, LPWSTR, LPDWORD, LPDWORD, LPDWORD, LPBYTE, LPDWORD);
        using RegCloseKeyFn = LSTATUS(WINAPI*)(HKEY);
        using ConnectFn = int (WSAAPI*)(SOCKET, const sockaddr*, int);
        using WSAConnectFn = int (WSAAPI*)(SOCKET, const sockaddr*, int, LPWSABUF, LPWSABUF, LPQOS, LPQOS);
        using SendFn = int (WSAAPI*)(SOCKET, const char*, int, int);
        using WSASendFn = int (WSAAPI*)(SOCKET, LPWSABUF, DWORD, LPDWORD, DWORD, LPWSAOVERLAPPED, LPWSAOVERLAPPED_COMPLETION_ROUTINE);
        using SendToFn = int (WSAAPI*)(SOCKET, const char*, int, int, const sockaddr*, int);
        using RecvFn = int (WSAAPI*)(SOCKET, char*, int, int);
        using WSARecvFn = int (WSAAPI*)(SOCKET, LPWSABUF, DWORD, LPDWORD, LPDWORD, LPWSAOVERLAPPED, LPWSAOVERLAPPED_COMPLETION_ROUTINE);
        using RecvFromFn = int (WSAAPI*)(SOCKET, char*, int, int, sockaddr*, int*);
        using BindFn = int (WSAAPI*)(SOCKET, const sockaddr*, int);
        using ListenFn = int (WSAAPI*)(SOCKET, int);
        using AcceptFn = SOCKET(WSAAPI*)(SOCKET, sockaddr*, int*);

        InlineHookRecord g_createFileWHook{};
        InlineHookRecord g_readFileHook{};
        InlineHookRecord g_writeFileHook{};
        InlineHookRecord g_createProcessWHook{};
        InlineHookRecord g_loadLibraryWHook{};
        InlineHookRecord g_loadLibraryExWHook{};
        InlineHookRecord g_regOpenKeyExWHook{};
        InlineHookRecord g_regCreateKeyExWHook{};
        InlineHookRecord g_regQueryValueExWHook{};
        InlineHookRecord g_regGetValueWHook{};
        InlineHookRecord g_regSetValueExWHook{};
        InlineHookRecord g_regDeleteValueWHook{};
        InlineHookRecord g_regDeleteKeyWHook{};
        InlineHookRecord g_regDeleteKeyExWHook{};
        InlineHookRecord g_regEnumKeyExWHook{};
        InlineHookRecord g_regEnumValueWHook{};
        InlineHookRecord g_regCloseKeyHook{};
        InlineHookRecord g_connectHook{};
        InlineHookRecord g_wsaConnectHook{};
        InlineHookRecord g_sendHook{};
        InlineHookRecord g_wsaSendHook{};
        InlineHookRecord g_sendToHook{};
        InlineHookRecord g_recvHook{};
        InlineHookRecord g_wsaRecvHook{};
        InlineHookRecord g_recvFromHook{};
        InlineHookRecord g_bindHook{};
        InlineHookRecord g_listenHook{};
        InlineHookRecord g_acceptHook{};
        std::mutex g_hookOperationMutex;

        CreateFileWFn g_createFileWOriginal = nullptr;
        ReadFileFn g_readFileOriginal = nullptr;
        WriteFileFn g_writeFileOriginal = nullptr;
        CreateProcessWFn g_createProcessWOriginal = nullptr;
        LoadLibraryWFn g_loadLibraryWOriginal = nullptr;
        LoadLibraryExWFn g_loadLibraryExWOriginal = nullptr;
        RegOpenKeyExWFn g_regOpenKeyExWOriginal = nullptr;
        RegCreateKeyExWFn g_regCreateKeyExWOriginal = nullptr;
        RegQueryValueExWFn g_regQueryValueExWOriginal = nullptr;
        RegGetValueWFn g_regGetValueWOriginal = nullptr;
        RegSetValueExWFn g_regSetValueExWOriginal = nullptr;
        RegDeleteValueWFn g_regDeleteValueWOriginal = nullptr;
        RegDeleteKeyWFn g_regDeleteKeyWOriginal = nullptr;
        RegDeleteKeyExWFn g_regDeleteKeyExWOriginal = nullptr;
        RegEnumKeyExWFn g_regEnumKeyExWOriginal = nullptr;
        RegEnumValueWFn g_regEnumValueWOriginal = nullptr;
        RegCloseKeyFn g_regCloseKeyOriginal = nullptr;
        ConnectFn g_connectOriginal = nullptr;
        WSAConnectFn g_wsaConnectOriginal = nullptr;
        SendFn g_sendOriginal = nullptr;
        WSASendFn g_wsaSendOriginal = nullptr;
        SendToFn g_sendToOriginal = nullptr;
        RecvFn g_recvOriginal = nullptr;
        WSARecvFn g_wsaRecvOriginal = nullptr;
        RecvFromFn g_recvFromOriginal = nullptr;
        BindFn g_bindOriginal = nullptr;
        ListenFn g_listenOriginal = nullptr;
        AcceptFn g_acceptOriginal = nullptr;

        thread_local bool g_hookReentryGuard = false;

        class ScopedHookGuard
        {
        public:
            ScopedHookGuard()
            {
                m_bypass = g_hookReentryGuard;
                if (!m_bypass)
                {
                    g_hookReentryGuard = true;
                }
            }

            ~ScopedHookGuard()
            {
                if (!m_bypass)
                {
                    g_hookReentryGuard = false;
                }
            }

            bool bypass() const
            {
                return m_bypass;
            }

        private:
            bool m_bypass = false;
        };

        struct HookBinding
        {
            const wchar_t* moduleName;                              // moduleName：导出所在模块名。
            const char* procName;                                   // procName：导出函数名。
            ks::winapi_monitor::EventCategory categoryValue;        // categoryValue：对应监控分类。
            InlineHookRecord* hookRecord;                           // hookRecord：该 API 对应的 Hook 状态记录。
            void* detourAddress;                                    // detourAddress：Detour 函数地址。
            void** originalOut;                                     // originalOut：Trampoline 返回地址。
        };

        std::wstring ProcNameToWide(const char* procNamePointer)
        {
            if (procNamePointer == nullptr)
            {
                return std::wstring();
            }

            std::wstring wideText;
            while (*procNamePointer != '\0')
            {
                wideText.push_back(static_cast<wchar_t>(*procNamePointer));
                ++procNamePointer;
            }
            return wideText;
        }

        void AppendHookFailureText(
            std::wstring* detailTextOut,
            const HookBinding& bindingValue,
            const InlineHookInstallResult installResult,
            const std::wstring& errorText)
        {
            if (detailTextOut == nullptr)
            {
                return;
            }

            if (!detailTextOut->empty())
            {
                detailTextOut->append(L" | ");
            }

            detailTextOut->append(bindingValue.moduleName != nullptr ? bindingValue.moduleName : L"<module>");
            detailTextOut->append(L"!");
            detailTextOut->append(ProcNameToWide(bindingValue.procName));
            detailTextOut->append(
                installResult == InlineHookInstallResult::RetryableFailure
                ? L": retryable failure - "
                : L": disabled - ");
            detailTextOut->append(errorText.empty() ? L"unknown reason" : errorText);
        }

        std::wstring SafeWideText(const wchar_t* textPointer)
        {
            return textPointer != nullptr ? std::wstring(textPointer) : std::wstring();
        }

        std::wstring HexValue(const std::uint64_t value)
        {
            wchar_t textBuffer[32] = {};
            ::swprintf_s(textBuffer, L"0x%llX", static_cast<unsigned long long>(value));
            return std::wstring(textBuffer);
        }

        std::wstring HandleText(const HANDLE handleValue)
        {
            return HexValue(reinterpret_cast<std::uint64_t>(handleValue));
        }

        // AppendWideText 作用：
        // - 输入：targetBuffer 为栈上定长缓冲，textPointer 为可空宽字符串；
        // - 处理：从当前 NUL 结尾处追加最多 maxInputChars 个字符，溢出时安全截断；
        // - 返回：无返回值，调用者直接使用 targetBuffer。
        template <std::size_t kCount>
        void AppendWideText(
            wchar_t(&targetBuffer)[kCount],
            const wchar_t* textPointer,
            const std::size_t maxInputChars = static_cast<std::size_t>(-1))
        {
            if (kCount == 0 || textPointer == nullptr)
            {
                return;
            }

            std::size_t writeOffset = 0;
            while (writeOffset < kCount && targetBuffer[writeOffset] != L'\0')
            {
                ++writeOffset;
            }
            if (writeOffset >= kCount)
            {
                targetBuffer[kCount - 1] = L'\0';
                return;
            }

            std::size_t inputOffset = 0;
            while (writeOffset + 1 < kCount
                && inputOffset < maxInputChars
                && textPointer[inputOffset] != L'\0')
            {
                targetBuffer[writeOffset++] = textPointer[inputOffset++];
            }
            targetBuffer[writeOffset] = L'\0';
        }

        // AppendUnsignedText 作用：
        // - 输入：value 为要追加的无符号整数；
        // - 处理：先格式化到小栈缓冲，再追加到目标缓冲；
        // - 返回：无返回值，失败时保持已有文本并追加空串。
        template <std::size_t kCount>
        void AppendUnsignedText(wchar_t(&targetBuffer)[kCount], const unsigned long long value)
        {
            wchar_t numberBuffer[32] = {};
            (void)::swprintf_s(numberBuffer, L"%llu", value);
            AppendWideText(targetBuffer, numberBuffer);
        }

        // AppendHexText 作用：
        // - 输入：value 为要追加的指针/掩码值；
        // - 处理：格式化为 0x 前缀十六进制字符串；
        // - 返回：无返回值，目标缓冲空间不足时安全截断。
        template <std::size_t kCount>
        void AppendHexText(wchar_t(&targetBuffer)[kCount], const std::uint64_t value)
        {
            wchar_t numberBuffer[32] = {};
            (void)::swprintf_s(numberBuffer, L"0x%llX", static_cast<unsigned long long>(value));
            AppendWideText(targetBuffer, numberBuffer);
        }

        // AppendRegistryRootText 作用：
        // - 输入：rootKey 为注册表根键或普通 HKEY 句柄；
        // - 处理：常见根键输出 HKxx，普通句柄输出十六进制；
        // - 返回：无返回值，追加到调用者提供的详情缓冲。
        template <std::size_t kCount>
        void AppendRegistryRootText(wchar_t(&targetBuffer)[kCount], const HKEY rootKey)
        {
            if (rootKey == HKEY_CLASSES_ROOT) { AppendWideText(targetBuffer, L"HKCR"); return; }
            if (rootKey == HKEY_CURRENT_USER) { AppendWideText(targetBuffer, L"HKCU"); return; }
            if (rootKey == HKEY_LOCAL_MACHINE) { AppendWideText(targetBuffer, L"HKLM"); return; }
            if (rootKey == HKEY_USERS) { AppendWideText(targetBuffer, L"HKU"); return; }
            if (rootKey == HKEY_CURRENT_CONFIG) { AppendWideText(targetBuffer, L"HKCC"); return; }
            AppendHexText(targetBuffer, reinterpret_cast<std::uint64_t>(rootKey));
        }

        // BuildRegOpenDetail 作用：
        // - 输入：注册表打开 API 的关键参数；
        // - 处理：在固定栈缓冲中拼接 key/sam 详情，不触发堆分配；
        // - 返回：无返回值，detailBuffer 保存可直接发送的 NUL 结尾文本。
        template <std::size_t kCount>
        void BuildRegOpenDetail(
            wchar_t(&detailBuffer)[kCount],
            const HKEY rootKey,
            const wchar_t* const subKeyPointer,
            const REGSAM samDesired)
        {
            detailBuffer[0] = L'\0';
            AppendWideText(detailBuffer, L"key=");
            AppendRegistryRootText(detailBuffer, rootKey);
            AppendWideText(detailBuffer, L"\\");
            AppendWideText(detailBuffer, subKeyPointer);
            AppendWideText(detailBuffer, L" sam=");
            AppendHexText(detailBuffer, static_cast<std::uint64_t>(samDesired));
        }

        // BuildRegCreateDetail 作用：
        // - 输入：注册表创建 API 的关键参数与 disposition；
        // - 处理：只记录稳定小字段，避免在注册表锁上下文中做复杂解析；
        // - 返回：无返回值，detailBuffer 保存固定长度详情。
        template <std::size_t kCount>
        void BuildRegCreateDetail(
            wchar_t(&detailBuffer)[kCount],
            const HKEY rootKey,
            const wchar_t* const subKeyPointer,
            const DWORD optionsValue,
            const DWORD dispositionValue)
        {
            detailBuffer[0] = L'\0';
            AppendWideText(detailBuffer, L"key=");
            AppendRegistryRootText(detailBuffer, rootKey);
            AppendWideText(detailBuffer, L"\\");
            AppendWideText(detailBuffer, subKeyPointer);
            AppendWideText(detailBuffer, L" options=");
            AppendHexText(detailBuffer, static_cast<std::uint64_t>(optionsValue));
            AppendWideText(detailBuffer, L" disposition=");
            AppendUnsignedText(detailBuffer, static_cast<unsigned long long>(dispositionValue));
        }

        // BuildRegSetValueDetail 作用：
        // - 输入：注册表写值 API 的句柄、值名、类型和数据长度；
        // - 处理：不读取 dataPointer 内容，避免触发页错误或复制敏感大数据；
        // - 返回：无返回值，detailBuffer 保存可发送的摘要文本。
        template <std::size_t kCount>
        void BuildRegSetValueDetail(
            wchar_t(&detailBuffer)[kCount],
            const HKEY keyHandle,
            const wchar_t* const valueNamePointer,
            const DWORD typeValue,
            const DWORD dataSize)
        {
            detailBuffer[0] = L'\0';
            AppendWideText(detailBuffer, L"hkey=");
            AppendHexText(detailBuffer, reinterpret_cast<std::uint64_t>(keyHandle));
            AppendWideText(detailBuffer, L" value=");
            AppendWideText(detailBuffer, valueNamePointer);
            AppendWideText(detailBuffer, L" type=");
            AppendUnsignedText(detailBuffer, static_cast<unsigned long long>(typeValue));
            AppendWideText(detailBuffer, L" size=");
            AppendUnsignedText(detailBuffer, static_cast<unsigned long long>(dataSize));
        }

        // BuildRegValueDetail 作用：
        // - 输入：注册表值操作常见参数；
        // - 处理：拼接 hkey/value/type/size 摘要，适用于查询、删除和枚举值；
        // - 返回：无返回值，detailBuffer 保存 NUL 结尾详情。
        template <std::size_t kCount>
        void BuildRegValueDetail(
            wchar_t(&detailBuffer)[kCount],
            const wchar_t* const prefixText,
            const HKEY keyHandle,
            const wchar_t* const valueNamePointer,
            const DWORD typeValue,
            const DWORD dataSize)
        {
            detailBuffer[0] = L'\0';
            AppendWideText(detailBuffer, prefixText != nullptr ? prefixText : L"hkey=");
            AppendHexText(detailBuffer, reinterpret_cast<std::uint64_t>(keyHandle));
            AppendWideText(detailBuffer, L" value=");
            AppendWideText(detailBuffer, valueNamePointer);
            AppendWideText(detailBuffer, L" type=");
            AppendUnsignedText(detailBuffer, static_cast<unsigned long long>(typeValue));
            AppendWideText(detailBuffer, L" size=");
            AppendUnsignedText(detailBuffer, static_cast<unsigned long long>(dataSize));
        }

        // BuildRegGetValueDetail 作用：
        // - 输入：RegGetValueW 的 hkey/subkey/value/flags/type/size；
        // - 处理：拼接查询来源和输出摘要，不读取返回数据内容；
        // - 返回：无返回值，detailBuffer 保存 NUL 结尾文本。
        template <std::size_t kCount>
        void BuildRegGetValueDetail(
            wchar_t(&detailBuffer)[kCount],
            const HKEY keyHandle,
            const wchar_t* const subKeyPointer,
            const wchar_t* const valueNamePointer,
            const DWORD flagsValue,
            const DWORD typeValue,
            const DWORD dataSize)
        {
            detailBuffer[0] = L'\0';
            AppendWideText(detailBuffer, L"hkey=");
            AppendHexText(detailBuffer, reinterpret_cast<std::uint64_t>(keyHandle));
            AppendWideText(detailBuffer, L" subkey=");
            AppendWideText(detailBuffer, subKeyPointer);
            AppendWideText(detailBuffer, L" value=");
            AppendWideText(detailBuffer, valueNamePointer);
            AppendWideText(detailBuffer, L" flags=");
            AppendHexText(detailBuffer, static_cast<std::uint64_t>(flagsValue));
            AppendWideText(detailBuffer, L" type=");
            AppendUnsignedText(detailBuffer, static_cast<unsigned long long>(typeValue));
            AppendWideText(detailBuffer, L" size=");
            AppendUnsignedText(detailBuffer, static_cast<unsigned long long>(dataSize));
        }

        // BuildRegSubKeyDetail 作用：
        // - 输入：注册表子键操作的根键、子键名和访问掩码；
        // - 处理：输出 key=<root>\<subkey> view=<sam> 的短文本；
        // - 返回：无返回值，目标缓冲空间不足时自动截断。
        template <std::size_t kCount>
        void BuildRegSubKeyDetail(
            wchar_t(&detailBuffer)[kCount],
            const HKEY rootKey,
            const wchar_t* const subKeyPointer,
            const REGSAM viewValue)
        {
            detailBuffer[0] = L'\0';
            AppendWideText(detailBuffer, L"key=");
            AppendRegistryRootText(detailBuffer, rootKey);
            AppendWideText(detailBuffer, L"\\");
            AppendWideText(detailBuffer, subKeyPointer);
            AppendWideText(detailBuffer, L" view=");
            AppendHexText(detailBuffer, static_cast<std::uint64_t>(viewValue));
        }

        // BuildRegEnumKeyDetail 作用：
        // - 输入：枚举子键结果中的索引、名称和名称长度；
        // - 处理：输出 hkey/index/name/nameLen 摘要；
        // - 返回：无返回值，失败时名称可能为空但仍保留索引。
        template <std::size_t kCount>
        void BuildRegEnumKeyDetail(
            wchar_t(&detailBuffer)[kCount],
            const HKEY keyHandle,
            const DWORD indexValue,
            const wchar_t* const namePointer,
            const DWORD nameLength)
        {
            detailBuffer[0] = L'\0';
            AppendWideText(detailBuffer, L"hkey=");
            AppendHexText(detailBuffer, reinterpret_cast<std::uint64_t>(keyHandle));
            AppendWideText(detailBuffer, L" index=");
            AppendUnsignedText(detailBuffer, static_cast<unsigned long long>(indexValue));
            AppendWideText(detailBuffer, L" name=");
            AppendWideText(detailBuffer, namePointer, nameLength);
            AppendWideText(detailBuffer, L" nameLen=");
            AppendUnsignedText(detailBuffer, static_cast<unsigned long long>(nameLength));
        }

        // SumWsaBufferLength 作用：
        // - 输入：Winsock WSABUF 数组和元素数量；
        // - 处理：累加 len 字段并防止空指针访问；
        // - 返回：总请求字节数，超过 uint64 时自然截断到 uint64 范围。
        std::uint64_t SumWsaBufferLength(const WSABUF* const bufferPointer, const DWORD bufferCount)
        {
            std::uint64_t totalLength = 0;
            if (bufferPointer == nullptr)
            {
                return totalLength;
            }

            for (DWORD indexValue = 0; indexValue < bufferCount; ++indexValue)
            {
                totalLength += static_cast<std::uint64_t>(bufferPointer[indexValue].len);
            }
            return totalLength;
        }

        // AppendSocketAddress 作用：
        // - 输入：sockaddr 与长度；
        // - 处理：尽量用 GetNameInfoW 转成 host:port，失败时输出 <unknown>；
        // - 返回：无返回值，直接追加到 detailBuffer。
        template <std::size_t kCount>
        void AppendSocketAddress(wchar_t(&detailBuffer)[kCount], const sockaddr* const addressPointer, const int addressLength)
        {
            if (addressPointer == nullptr || addressLength <= 0)
            {
                AppendWideText(detailBuffer, L"<null>");
                return;
            }

            wchar_t hostBuffer[NI_MAXHOST] = {};
            wchar_t serviceBuffer[NI_MAXSERV] = {};
            const int resultValue = ::GetNameInfoW(
                addressPointer,
                addressLength,
                hostBuffer,
                NI_MAXHOST,
                serviceBuffer,
                NI_MAXSERV,
                NI_NUMERICHOST | NI_NUMERICSERV);
            if (resultValue != 0)
            {
                AppendWideText(detailBuffer, L"<unknown>");
                return;
            }

            AppendWideText(detailBuffer, hostBuffer);
            AppendWideText(detailBuffer, L":");
            AppendWideText(detailBuffer, serviceBuffer);
        }

        // BuildSocketDetail 作用：
        // - 输入：socket、请求长度、实际传输长度、flags 和可选地址；
        // - 处理：生成网络事件摘要，不读取网络缓冲内容；
        // - 返回：无返回值，detailBuffer 保存可发送文本。
        template <std::size_t kCount>
        void BuildSocketDetail(
            wchar_t(&detailBuffer)[kCount],
            const wchar_t* const verbText,
            const SOCKET socketValue,
            const std::uint64_t requestLength,
            const long long transferLength,
            const DWORD flagsValue,
            const sockaddr* const addressPointer = nullptr,
            const int addressLength = 0)
        {
            detailBuffer[0] = L'\0';
            AppendWideText(detailBuffer, L"socket=");
            AppendHexText(detailBuffer, static_cast<std::uint64_t>(socketValue));
            if (verbText != nullptr && verbText[0] != L'\0')
            {
                AppendWideText(detailBuffer, L" ");
                AppendWideText(detailBuffer, verbText);
            }
            AppendWideText(detailBuffer, L" request=");
            AppendUnsignedText(detailBuffer, static_cast<unsigned long long>(requestLength));
            AppendWideText(detailBuffer, L" transferred=");
            AppendUnsignedText(detailBuffer, static_cast<unsigned long long>(transferLength < 0 ? 0 : transferLength));
            AppendWideText(detailBuffer, L" flags=");
            AppendHexText(detailBuffer, static_cast<std::uint64_t>(flagsValue));
            if (addressPointer != nullptr)
            {
                AppendWideText(detailBuffer, L" remote=");
                AppendSocketAddress(detailBuffer, addressPointer, addressLength);
            }
        }

        bool CategoryEnabled(const ks::winapi_monitor::EventCategory categoryValue)
        {
            const MonitorConfig& configValue = ActiveConfig();
            switch (categoryValue)
            {
            case ks::winapi_monitor::EventCategory::File:
                return configValue.enableFile;
            case ks::winapi_monitor::EventCategory::Registry:
                return configValue.enableRegistry;
            case ks::winapi_monitor::EventCategory::Network:
                return configValue.enableNetwork;
            case ks::winapi_monitor::EventCategory::Process:
                return configValue.enableProcess;
            case ks::winapi_monitor::EventCategory::Loader:
                // Loader hook 还承担“后加载模块补装”职责：
                // - enableLoader 控制是否上报 LoadLibrary 事件；
                // - 注册表/网络模块可能晚于 Agent 注入加载，因此启用这些分类时也要安装加载器 hook。
                return configValue.enableLoader || configValue.enableRegistry || configValue.enableNetwork;
            default:
                break;
            }
            return true;
        }

        std::wstring TrimDetail(const std::wstring& detailText)
        {
            const std::size_t detailLimit = std::min<std::size_t>(
                ActiveConfig().detailLimitChars,
                ks::winapi_monitor::kMaxDetailChars - 1);
            return detailText.size() > detailLimit ? detailText.substr(0, detailLimit) : detailText;
        }

        std::wstring FormatRegistryRoot(const HKEY rootKey)
        {
            if (rootKey == HKEY_CLASSES_ROOT) { return L"HKCR"; }
            if (rootKey == HKEY_CURRENT_USER) { return L"HKCU"; }
            if (rootKey == HKEY_LOCAL_MACHINE) { return L"HKLM"; }
            if (rootKey == HKEY_USERS) { return L"HKU"; }
            if (rootKey == HKEY_CURRENT_CONFIG) { return L"HKCC"; }
            return HandleText(rootKey);
        }

        std::wstring FormatSocketAddress(const sockaddr* addressPointer, const int addressLength)
        {
            if (addressPointer == nullptr || addressLength <= 0)
            {
                return L"<null>";
            }

            wchar_t hostBuffer[NI_MAXHOST] = {};
            wchar_t serviceBuffer[NI_MAXSERV] = {};
            const int resultValue = ::GetNameInfoW(
                addressPointer,
                addressLength,
                hostBuffer,
                NI_MAXHOST,
                serviceBuffer,
                NI_MAXSERV,
                NI_NUMERICHOST | NI_NUMERICSERV);
            if (resultValue != 0)
            {
                return L"<unknown>";
            }
            return std::wstring(hostBuffer) + L":" + serviceBuffer;
        }

        bool TryInstallBinding(HookBinding& bindingValue, std::wstring* detailTextOut)
        {
            if (!CategoryEnabled(bindingValue.categoryValue)
                || bindingValue.hookRecord->installed
                || bindingValue.hookRecord->permanentlyDisabled)
            {
                return bindingValue.hookRecord->installed;
            }

            std::wstring errorText;
            const InlineHookInstallResult installResult = InstallInlineHook(
                bindingValue.moduleName,
                bindingValue.procName,
                bindingValue.detourAddress,
                bindingValue.hookRecord,
                bindingValue.originalOut,
                &errorText);
            if (installResult == InlineHookInstallResult::Installed)
            {
                return true;
            }
            if (installResult == InlineHookInstallResult::PermanentFailure)
            {
                bindingValue.hookRecord->permanentlyDisabled = true;
            }
            AppendHookFailureText(detailTextOut, bindingValue, installResult, errorText);
            return false;
        }

        // SendLoaderEventIfEnabled 作用：
        // - 输入：LoadLibrary API 名、路径、结果和错误码；
        // - 处理：仅在 UI 勾选加载器分类时上报，避免“补装所需加载器 hook”强制制造事件噪声；
        // - 返回：无返回值，保留调用者负责恢复 LastError。
        void SendLoaderEventIfEnabled(
            const wchar_t* const apiName,
            const std::wstring& fileNameText,
            const HMODULE moduleHandle,
            const DWORD lastError,
            const std::wstring& extraText)
        {
            if (!ActiveConfig().enableLoader)
            {
                return;
            }

            SendMonitorEvent(
                ks::winapi_monitor::EventCategory::Loader,
                L"Kernel32",
                apiName,
                moduleHandle != nullptr ? 0 : static_cast<std::int32_t>(lastError),
                TrimDetail(L"path=" + fileNameText + extraText));
        }

        // WriteChildMonitorConfig 作用：
        // - 输入：childPidValue 为新子进程 PID，configValue 为父进程当前监控配置；
        // - 处理：生成子进程专属 INI，沿用当前分类开关、DLL 路径和自动注入策略；
        // - 返回：成功写入返回 true，失败返回 false 并填充 errorTextOut。
        bool WriteChildMonitorConfig(
            const DWORD childPidValue,
            const MonitorConfig& configValue,
            std::wstring* const errorTextOut)
        {
            if (errorTextOut != nullptr)
            {
                errorTextOut->clear();
            }
            if (childPidValue == 0 || configValue.agentDllPath.empty())
            {
                if (errorTextOut != nullptr)
                {
                    *errorTextOut = L"child pid or agent dll path is empty.";
                }
                return false;
            }

            const std::wstring sessionDirectory = ks::winapi_monitor::buildSessionDirectory();
            (void)::CreateDirectoryW(sessionDirectory.c_str(), nullptr);

            const std::wstring childConfigPath = ks::winapi_monitor::buildConfigPathForPid(childPidValue);
            const std::wstring childStopPath = ks::winapi_monitor::buildStopFlagPathForPid(childPidValue);
            (void)::DeleteFileW(childStopPath.c_str());

            HANDLE fileHandle = ::CreateFileW(
                childConfigPath.c_str(),
                GENERIC_WRITE,
                FILE_SHARE_READ,
                nullptr,
                CREATE_ALWAYS,
                FILE_ATTRIBUTE_NORMAL,
                nullptr);
            if (fileHandle == INVALID_HANDLE_VALUE)
            {
                if (errorTextOut != nullptr)
                {
                    *errorTextOut = L"CreateFileW child config failed. error=" + std::to_wstring(::GetLastError());
                }
                return false;
            }

            const std::wstring configText =
                L"[monitor]\r\n"
                L"pipe_name=" + ks::winapi_monitor::buildPipeNameForPid(childPidValue) + L"\r\n"
                L"stop_flag_path=" + childStopPath + L"\r\n"
                L"agent_dll_path=" + configValue.agentDllPath + L"\r\n"
                L"enable_file=" + std::to_wstring(configValue.enableFile ? 1 : 0) + L"\r\n"
                L"enable_registry=" + std::to_wstring(configValue.enableRegistry ? 1 : 0) + L"\r\n"
                L"enable_network=" + std::to_wstring(configValue.enableNetwork ? 1 : 0) + L"\r\n"
                L"enable_process=" + std::to_wstring(configValue.enableProcess ? 1 : 0) + L"\r\n"
                L"enable_loader=" + std::to_wstring(configValue.enableLoader ? 1 : 0) + L"\r\n"
                L"auto_inject_child=" + std::to_wstring(configValue.autoInjectChild ? 1 : 0) + L"\r\n"
                L"detail_limit=" + std::to_wstring(configValue.detailLimitChars) + L"\r\n";

            const wchar_t unicodeBom = static_cast<wchar_t>(0xFEFF);
            DWORD bomBytesWritten = 0;
            const BOOL bomWriteOk = ::WriteFile(
                fileHandle,
                &unicodeBom,
                sizeof(unicodeBom),
                &bomBytesWritten,
                nullptr);

            DWORD bytesWritten = 0;
            const BOOL writeOk = ::WriteFile(
                fileHandle,
                configText.data(),
                static_cast<DWORD>(configText.size() * sizeof(wchar_t)),
                &bytesWritten,
                nullptr);
            const DWORD writeError = ::GetLastError();
            ::CloseHandle(fileHandle);

            if (bomWriteOk == FALSE
                || bomBytesWritten != sizeof(unicodeBom)
                || writeOk == FALSE
                || bytesWritten != configText.size() * sizeof(wchar_t))
            {
                if (errorTextOut != nullptr)
                {
                    *errorTextOut = L"WriteFile child config failed. error=" + std::to_wstring(writeError);
                }
                return false;
            }
            return true;
        }

        // InjectAgentIntoChildProcess 作用：
        // - 输入：childPidValue 为子进程 PID，dllPath 为 APIMonitor_x64.dll 路径；
        // - 处理：使用 VirtualAllocEx/WriteProcessMemory/CreateRemoteThread(LoadLibraryW) 注入；
        // - 返回：注入成功返回 true，失败返回 false 并填充 errorTextOut。
        bool InjectAgentIntoChildProcess(
            const DWORD childPidValue,
            const std::wstring& dllPath,
            std::wstring* const errorTextOut)
        {
            if (errorTextOut != nullptr)
            {
                errorTextOut->clear();
            }
            if (childPidValue == 0 || dllPath.empty())
            {
                if (errorTextOut != nullptr)
                {
                    *errorTextOut = L"child pid or dll path is empty.";
                }
                return false;
            }

            HANDLE processHandle = ::OpenProcess(
                PROCESS_CREATE_THREAD | PROCESS_QUERY_INFORMATION | PROCESS_VM_OPERATION | PROCESS_VM_WRITE | PROCESS_VM_READ,
                FALSE,
                childPidValue);
            if (processHandle == nullptr)
            {
                if (errorTextOut != nullptr)
                {
                    *errorTextOut = L"OpenProcess child failed. error=" + std::to_wstring(::GetLastError());
                }
                return false;
            }

            const std::size_t byteCount = (dllPath.size() + 1) * sizeof(wchar_t);
            void* remotePathMemory = ::VirtualAllocEx(
                processHandle,
                nullptr,
                byteCount,
                MEM_COMMIT | MEM_RESERVE,
                PAGE_READWRITE);
            if (remotePathMemory == nullptr)
            {
                if (errorTextOut != nullptr)
                {
                    *errorTextOut = L"VirtualAllocEx child failed. error=" + std::to_wstring(::GetLastError());
                }
                ::CloseHandle(processHandle);
                return false;
            }

            SIZE_T bytesWritten = 0;
            const BOOL writeOk = ::WriteProcessMemory(
                processHandle,
                remotePathMemory,
                dllPath.c_str(),
                byteCount,
                &bytesWritten);
            if (writeOk == FALSE || bytesWritten != byteCount)
            {
                if (errorTextOut != nullptr)
                {
                    *errorTextOut = L"WriteProcessMemory child failed. error=" + std::to_wstring(::GetLastError());
                }
                ::VirtualFreeEx(processHandle, remotePathMemory, 0, MEM_RELEASE);
                ::CloseHandle(processHandle);
                return false;
            }

            HMODULE kernelModule = ::GetModuleHandleW(L"kernel32.dll");
            FARPROC loadLibraryPointer = kernelModule != nullptr ? ::GetProcAddress(kernelModule, "LoadLibraryW") : nullptr;
            if (loadLibraryPointer == nullptr)
            {
                if (errorTextOut != nullptr)
                {
                    *errorTextOut = L"GetProcAddress LoadLibraryW failed.";
                }
                ::VirtualFreeEx(processHandle, remotePathMemory, 0, MEM_RELEASE);
                ::CloseHandle(processHandle);
                return false;
            }

            HANDLE remoteThread = ::CreateRemoteThread(
                processHandle,
                nullptr,
                0,
                reinterpret_cast<LPTHREAD_START_ROUTINE>(loadLibraryPointer),
                remotePathMemory,
                0,
                nullptr);
            if (remoteThread == nullptr)
            {
                if (errorTextOut != nullptr)
                {
                    *errorTextOut = L"CreateRemoteThread child failed. error=" + std::to_wstring(::GetLastError());
                }
                ::VirtualFreeEx(processHandle, remotePathMemory, 0, MEM_RELEASE);
                ::CloseHandle(processHandle);
                return false;
            }

            (void)::WaitForSingleObject(remoteThread, 10000);
            DWORD exitCode = 0;
            (void)::GetExitCodeThread(remoteThread, &exitCode);
            ::CloseHandle(remoteThread);
            ::VirtualFreeEx(processHandle, remotePathMemory, 0, MEM_RELEASE);
            ::CloseHandle(processHandle);

            if (exitCode == 0)
            {
                if (errorTextOut != nullptr)
                {
                    *errorTextOut = L"Remote LoadLibraryW returned NULL.";
                }
                return false;
            }
            return true;
        }

        // AutoInjectChildIfRequested 作用：
        // - 输入：CreateProcessW 结果和 PROCESS_INFORMATION；
        // - 处理：配置启用时为子进程写配置并注入当前 Agent；
        // - 返回：无返回值，成功/失败均以内部事件上报。
        void AutoInjectChildIfRequested(
            const BOOL createResult,
            const PROCESS_INFORMATION* const processInfoPointer)
        {
            const MonitorConfig& configValue = ActiveConfig();
            if (createResult == FALSE
                || !configValue.autoInjectChild
                || processInfoPointer == nullptr
                || processInfoPointer->dwProcessId == 0
                || configValue.agentDllPath.empty())
            {
                return;
            }

            std::wstring errorText;
            bool successValue = WriteChildMonitorConfig(processInfoPointer->dwProcessId, configValue, &errorText);
            if (successValue)
            {
                successValue = InjectAgentIntoChildProcess(processInfoPointer->dwProcessId, configValue.agentDllPath, &errorText);
            }

            SendMonitorEvent(
                ks::winapi_monitor::EventCategory::Internal,
                L"Agent",
                successValue ? L"AutoInjectChild" : L"AutoInjectChildFailed",
                successValue ? 0 : 1,
                TrimDetail(
                    L"childPid=" + std::to_wstring(processInfoPointer->dwProcessId)
                    + (successValue ? L" injected" : (L" error=" + errorText))));
        }

        HMODULE WINAPI HookedLoadLibraryW(LPCWSTR fileNamePointer);
        HMODULE WINAPI HookedLoadLibraryExW(LPCWSTR fileNamePointer, HANDLE fileHandle, DWORD flagsValue);
        HANDLE WINAPI HookedCreateFileW(LPCWSTR fileNamePointer, DWORD desiredAccess, DWORD shareMode, LPSECURITY_ATTRIBUTES securityAttributes, DWORD creationDisposition, DWORD flagsAndAttributes, HANDLE templateFile);
        BOOL WINAPI HookedReadFile(HANDLE fileHandle, LPVOID bufferPointer, DWORD bytesToRead, LPDWORD bytesReadPointer, LPOVERLAPPED overlappedPointer);
        BOOL WINAPI HookedWriteFile(HANDLE fileHandle, LPCVOID bufferPointer, DWORD bytesToWrite, LPDWORD bytesWrittenPointer, LPOVERLAPPED overlappedPointer);
        BOOL WINAPI HookedCreateProcessW(LPCWSTR applicationNamePointer, LPWSTR commandLinePointer, LPSECURITY_ATTRIBUTES processAttributes, LPSECURITY_ATTRIBUTES threadAttributes, BOOL inheritHandles, DWORD creationFlags, LPVOID environmentPointer, LPCWSTR currentDirectoryPointer, LPSTARTUPINFOW startupInfoPointer, LPPROCESS_INFORMATION processInfoPointer);
        LSTATUS WINAPI HookedRegOpenKeyExW(HKEY rootKey, LPCWSTR subKeyPointer, DWORD optionsValue, REGSAM samDesired, PHKEY resultKeyPointer);
        LSTATUS WINAPI HookedRegCreateKeyExW(HKEY rootKey, LPCWSTR subKeyPointer, DWORD reservedValue, LPWSTR classPointer, DWORD optionsValue, REGSAM samDesired, const LPSECURITY_ATTRIBUTES securityAttributes, PHKEY resultKeyPointer, LPDWORD dispositionPointer);
        LSTATUS WINAPI HookedRegQueryValueExW(HKEY keyHandle, LPCWSTR valueNamePointer, LPDWORD reservedPointer, LPDWORD typePointer, LPBYTE dataPointer, LPDWORD dataSizePointer);
        LSTATUS WINAPI HookedRegGetValueW(HKEY keyHandle, LPCWSTR subKeyPointer, LPCWSTR valueNamePointer, DWORD flagsValue, LPDWORD typePointer, PVOID dataPointer, LPDWORD dataSizePointer);
        LSTATUS WINAPI HookedRegSetValueExW(HKEY keyHandle, LPCWSTR valueNamePointer, DWORD reservedValue, DWORD typeValue, const BYTE* dataPointer, DWORD dataSize);
        LSTATUS WINAPI HookedRegDeleteValueW(HKEY keyHandle, LPCWSTR valueNamePointer);
        LSTATUS WINAPI HookedRegDeleteKeyW(HKEY rootKey, LPCWSTR subKeyPointer);
        LSTATUS WINAPI HookedRegDeleteKeyExW(HKEY rootKey, LPCWSTR subKeyPointer, REGSAM samDesired, DWORD reservedValue);
        LSTATUS WINAPI HookedRegEnumKeyExW(HKEY keyHandle, DWORD indexValue, LPWSTR namePointer, LPDWORD nameLengthPointer, LPDWORD reservedPointer, LPWSTR classPointer, LPDWORD classLengthPointer, PFILETIME lastWriteTimePointer);
        LSTATUS WINAPI HookedRegEnumValueW(HKEY keyHandle, DWORD indexValue, LPWSTR valueNamePointer, LPDWORD valueNameLengthPointer, LPDWORD reservedPointer, LPDWORD typePointer, LPBYTE dataPointer, LPDWORD dataSizePointer);
        LSTATUS WINAPI HookedRegCloseKey(HKEY keyHandle);
        int WSAAPI HookedConnect(SOCKET socketValue, const sockaddr* namePointer, int nameLength);
        int WSAAPI HookedWSAConnect(SOCKET socketValue, const sockaddr* namePointer, int nameLength, LPWSABUF callerDataPointer, LPWSABUF calleeDataPointer, LPQOS socketQosPointer, LPQOS groupQosPointer);
        int WSAAPI HookedSend(SOCKET socketValue, const char* bufferPointer, int bufferLength, int flagsValue);
        int WSAAPI HookedWSASend(SOCKET socketValue, LPWSABUF buffersPointer, DWORD bufferCount, LPDWORD bytesSentPointer, DWORD flagsValue, LPWSAOVERLAPPED overlappedPointer, LPWSAOVERLAPPED_COMPLETION_ROUTINE completionRoutinePointer);
        int WSAAPI HookedSendTo(SOCKET socketValue, const char* bufferPointer, int bufferLength, int flagsValue, const sockaddr* toPointer, int toLength);
        int WSAAPI HookedRecv(SOCKET socketValue, char* bufferPointer, int bufferLength, int flagsValue);
        int WSAAPI HookedWSARecv(SOCKET socketValue, LPWSABUF buffersPointer, DWORD bufferCount, LPDWORD bytesReceivedPointer, LPDWORD flagsPointer, LPWSAOVERLAPPED overlappedPointer, LPWSAOVERLAPPED_COMPLETION_ROUTINE completionRoutinePointer);
        int WSAAPI HookedRecvFrom(SOCKET socketValue, char* bufferPointer, int bufferLength, int flagsValue, sockaddr* fromPointer, int* fromLengthPointer);
        int WSAAPI HookedBind(SOCKET socketValue, const sockaddr* namePointer, int nameLength);
        int WSAAPI HookedListen(SOCKET socketValue, int backlogValue);
        SOCKET WSAAPI HookedAccept(SOCKET socketValue, sockaddr* addressPointer, int* addressLengthPointer);

        HookBinding g_bindings[] = {
            { L"KernelBase.dll", "CreateFileW", ks::winapi_monitor::EventCategory::File, &g_createFileWHook, reinterpret_cast<void*>(&HookedCreateFileW), reinterpret_cast<void**>(&g_createFileWOriginal) },
            { L"KernelBase.dll", "ReadFile", ks::winapi_monitor::EventCategory::File, &g_readFileHook, reinterpret_cast<void*>(&HookedReadFile), reinterpret_cast<void**>(&g_readFileOriginal) },
            { L"KernelBase.dll", "WriteFile", ks::winapi_monitor::EventCategory::File, &g_writeFileHook, reinterpret_cast<void*>(&HookedWriteFile), reinterpret_cast<void**>(&g_writeFileOriginal) },
            { L"KernelBase.dll", "CreateProcessW", ks::winapi_monitor::EventCategory::Process, &g_createProcessWHook, reinterpret_cast<void*>(&HookedCreateProcessW), reinterpret_cast<void**>(&g_createProcessWOriginal) },
            { L"Kernel32.dll", "LoadLibraryW", ks::winapi_monitor::EventCategory::Loader, &g_loadLibraryWHook, reinterpret_cast<void*>(&HookedLoadLibraryW), reinterpret_cast<void**>(&g_loadLibraryWOriginal) },
            { L"Kernel32.dll", "LoadLibraryExW", ks::winapi_monitor::EventCategory::Loader, &g_loadLibraryExWHook, reinterpret_cast<void*>(&HookedLoadLibraryExW), reinterpret_cast<void**>(&g_loadLibraryExWOriginal) },
            // RegOpenKeyExW：注册表读取入口 API，必须纳入绑定表，否则“注册表打开/读取”事件会缺失。
            { L"Advapi32.dll", "RegOpenKeyExW", ks::winapi_monitor::EventCategory::Registry, &g_regOpenKeyExWHook, reinterpret_cast<void*>(&HookedRegOpenKeyExW), reinterpret_cast<void**>(&g_regOpenKeyExWOriginal) },
            { L"Advapi32.dll", "RegCreateKeyExW", ks::winapi_monitor::EventCategory::Registry, &g_regCreateKeyExWHook, reinterpret_cast<void*>(&HookedRegCreateKeyExW), reinterpret_cast<void**>(&g_regCreateKeyExWOriginal) },
            { L"Advapi32.dll", "RegQueryValueExW", ks::winapi_monitor::EventCategory::Registry, &g_regQueryValueExWHook, reinterpret_cast<void*>(&HookedRegQueryValueExW), reinterpret_cast<void**>(&g_regQueryValueExWOriginal) },
            { L"Advapi32.dll", "RegGetValueW", ks::winapi_monitor::EventCategory::Registry, &g_regGetValueWHook, reinterpret_cast<void*>(&HookedRegGetValueW), reinterpret_cast<void**>(&g_regGetValueWOriginal) },
            { L"Advapi32.dll", "RegSetValueExW", ks::winapi_monitor::EventCategory::Registry, &g_regSetValueExWHook, reinterpret_cast<void*>(&HookedRegSetValueExW), reinterpret_cast<void**>(&g_regSetValueExWOriginal) },
            { L"Advapi32.dll", "RegDeleteValueW", ks::winapi_monitor::EventCategory::Registry, &g_regDeleteValueWHook, reinterpret_cast<void*>(&HookedRegDeleteValueW), reinterpret_cast<void**>(&g_regDeleteValueWOriginal) },
            { L"Advapi32.dll", "RegDeleteKeyW", ks::winapi_monitor::EventCategory::Registry, &g_regDeleteKeyWHook, reinterpret_cast<void*>(&HookedRegDeleteKeyW), reinterpret_cast<void**>(&g_regDeleteKeyWOriginal) },
            { L"Advapi32.dll", "RegDeleteKeyExW", ks::winapi_monitor::EventCategory::Registry, &g_regDeleteKeyExWHook, reinterpret_cast<void*>(&HookedRegDeleteKeyExW), reinterpret_cast<void**>(&g_regDeleteKeyExWOriginal) },
            { L"Advapi32.dll", "RegEnumKeyExW", ks::winapi_monitor::EventCategory::Registry, &g_regEnumKeyExWHook, reinterpret_cast<void*>(&HookedRegEnumKeyExW), reinterpret_cast<void**>(&g_regEnumKeyExWOriginal) },
            { L"Advapi32.dll", "RegEnumValueW", ks::winapi_monitor::EventCategory::Registry, &g_regEnumValueWHook, reinterpret_cast<void*>(&HookedRegEnumValueW), reinterpret_cast<void**>(&g_regEnumValueWOriginal) },
            { L"Advapi32.dll", "RegCloseKey", ks::winapi_monitor::EventCategory::Registry, &g_regCloseKeyHook, reinterpret_cast<void*>(&HookedRegCloseKey), reinterpret_cast<void**>(&g_regCloseKeyOriginal) },
            { L"Ws2_32.dll", "connect", ks::winapi_monitor::EventCategory::Network, &g_connectHook, reinterpret_cast<void*>(&HookedConnect), reinterpret_cast<void**>(&g_connectOriginal) },
            { L"Ws2_32.dll", "WSAConnect", ks::winapi_monitor::EventCategory::Network, &g_wsaConnectHook, reinterpret_cast<void*>(&HookedWSAConnect), reinterpret_cast<void**>(&g_wsaConnectOriginal) },
            { L"Ws2_32.dll", "send", ks::winapi_monitor::EventCategory::Network, &g_sendHook, reinterpret_cast<void*>(&HookedSend), reinterpret_cast<void**>(&g_sendOriginal) },
            { L"Ws2_32.dll", "WSASend", ks::winapi_monitor::EventCategory::Network, &g_wsaSendHook, reinterpret_cast<void*>(&HookedWSASend), reinterpret_cast<void**>(&g_wsaSendOriginal) },
            { L"Ws2_32.dll", "sendto", ks::winapi_monitor::EventCategory::Network, &g_sendToHook, reinterpret_cast<void*>(&HookedSendTo), reinterpret_cast<void**>(&g_sendToOriginal) },
            { L"Ws2_32.dll", "recv", ks::winapi_monitor::EventCategory::Network, &g_recvHook, reinterpret_cast<void*>(&HookedRecv), reinterpret_cast<void**>(&g_recvOriginal) },
            { L"Ws2_32.dll", "WSARecv", ks::winapi_monitor::EventCategory::Network, &g_wsaRecvHook, reinterpret_cast<void*>(&HookedWSARecv), reinterpret_cast<void**>(&g_wsaRecvOriginal) },
            { L"Ws2_32.dll", "recvfrom", ks::winapi_monitor::EventCategory::Network, &g_recvFromHook, reinterpret_cast<void*>(&HookedRecvFrom), reinterpret_cast<void**>(&g_recvFromOriginal) },
            { L"Ws2_32.dll", "bind", ks::winapi_monitor::EventCategory::Network, &g_bindHook, reinterpret_cast<void*>(&HookedBind), reinterpret_cast<void**>(&g_bindOriginal) },
            { L"Ws2_32.dll", "listen", ks::winapi_monitor::EventCategory::Network, &g_listenHook, reinterpret_cast<void*>(&HookedListen), reinterpret_cast<void**>(&g_listenOriginal) },
            { L"Ws2_32.dll", "accept", ks::winapi_monitor::EventCategory::Network, &g_acceptHook, reinterpret_cast<void*>(&HookedAccept), reinterpret_cast<void**>(&g_acceptOriginal) }
        };

        // RetryPendingHooksUnlocked 作用：
        // - 输入：无，使用全局绑定表；
        // - 处理：在调用者已经持有 g_hookOperationMutex 时补装尚未安装的可重试 Hook；
        // - 返回：无返回值，失败细节在后续 InstallConfiguredHooks 诊断中体现。
        void RetryPendingHooksUnlocked()
        {
            for (HookBinding& bindingValue : g_bindings)
            {
                (void)TryInstallBinding(bindingValue, nullptr);
            }
        }

        // RetryPendingHooksFromHook 作用：
        // - 输入：无；
        // - 处理：从 LoadLibrary detour 返回后尝试获取 hook 锁，避免同线程重入死锁；
        // - 返回：无返回值，锁正忙时跳过本轮补装。
        void RetryPendingHooksFromHook()
        {
            if (g_hookOperationMutex.try_lock())
            {
                RetryPendingHooksUnlocked();
                g_hookOperationMutex.unlock();
            }
        }

        HMODULE WINAPI HookedLoadLibraryW(const LPCWSTR fileNamePointer)
        {
            ScopedHookGuard guardValue;
            if (guardValue.bypass())
            {
                return g_loadLibraryWOriginal(fileNamePointer);
            }

            const std::wstring fileNameText = SafeWideText(fileNamePointer);
            HMODULE moduleHandle = g_loadLibraryWOriginal(fileNamePointer);
            const DWORD lastError = ::GetLastError();
            if (moduleHandle != nullptr)
            {
                RetryPendingHooksFromHook();
            }
            SendLoaderEventIfEnabled(L"LoadLibraryW", fileNameText, moduleHandle, lastError, std::wstring());
            ::SetLastError(lastError);
            return moduleHandle;
        }

        HMODULE WINAPI HookedLoadLibraryExW(const LPCWSTR fileNamePointer, HANDLE fileHandle, DWORD flagsValue)
        {
            ScopedHookGuard guardValue;
            if (guardValue.bypass())
            {
                return g_loadLibraryExWOriginal(fileNamePointer, fileHandle, flagsValue);
            }

            const std::wstring fileNameText = SafeWideText(fileNamePointer);
            HMODULE moduleHandle = g_loadLibraryExWOriginal(fileNamePointer, fileHandle, flagsValue);
            const DWORD lastError = ::GetLastError();
            if (moduleHandle != nullptr)
            {
                RetryPendingHooksFromHook();
            }
            SendLoaderEventIfEnabled(L"LoadLibraryExW", fileNameText, moduleHandle, lastError, L" flags=" + HexValue(flagsValue));
            ::SetLastError(lastError);
            return moduleHandle;
        }

        HANDLE WINAPI HookedCreateFileW(
            LPCWSTR fileNamePointer,
            DWORD desiredAccess,
            DWORD shareMode,
            LPSECURITY_ATTRIBUTES securityAttributes,
            DWORD creationDisposition,
            DWORD flagsAndAttributes,
            HANDLE templateFile)
        {
            (void)securityAttributes;
            ScopedHookGuard guardValue;
            if (guardValue.bypass())
            {
                return g_createFileWOriginal(fileNamePointer, desiredAccess, shareMode, securityAttributes, creationDisposition, flagsAndAttributes, templateFile);
            }

            const std::wstring fileNameText = SafeWideText(fileNamePointer);
            HANDLE resultHandle = g_createFileWOriginal(fileNamePointer, desiredAccess, shareMode, securityAttributes, creationDisposition, flagsAndAttributes, templateFile);
            const DWORD lastError = ::GetLastError();
            SendMonitorEvent(
                ks::winapi_monitor::EventCategory::File,
                L"KernelBase",
                L"CreateFileW",
                resultHandle != INVALID_HANDLE_VALUE ? 0 : static_cast<std::int32_t>(lastError),
                TrimDetail(
                    L"path=" + fileNameText
                    + L" access=" + HexValue(desiredAccess)
                    + L" share=" + HexValue(shareMode)
                    + L" disposition=" + std::to_wstring(creationDisposition)
                    + L" flags=" + HexValue(flagsAndAttributes)
                    + L" handle=" + HandleText(resultHandle)));
            ::SetLastError(lastError);
            return resultHandle;
        }

        BOOL WINAPI HookedReadFile(HANDLE fileHandle, LPVOID bufferPointer, DWORD bytesToRead, LPDWORD bytesReadPointer, LPOVERLAPPED overlappedPointer)
        {
            if (IsMonitorPipeHandle(fileHandle))
            {
                return g_readFileOriginal(fileHandle, bufferPointer, bytesToRead, bytesReadPointer, overlappedPointer);
            }

            ScopedHookGuard guardValue;
            if (guardValue.bypass())
            {
                return g_readFileOriginal(fileHandle, bufferPointer, bytesToRead, bytesReadPointer, overlappedPointer);
            }

            const BOOL resultValue = g_readFileOriginal(fileHandle, bufferPointer, bytesToRead, bytesReadPointer, overlappedPointer);
            const DWORD lastError = ::GetLastError();
            const DWORD bytesReadValue = bytesReadPointer != nullptr ? *bytesReadPointer : 0;
            SendMonitorEvent(
                ks::winapi_monitor::EventCategory::File,
                L"KernelBase",
                L"ReadFile",
                resultValue != FALSE ? 0 : static_cast<std::int32_t>(lastError),
                TrimDetail(L"handle=" + HandleText(fileHandle) + L" request=" + std::to_wstring(bytesToRead) + L" read=" + std::to_wstring(bytesReadValue)));
            ::SetLastError(lastError);
            return resultValue;
        }

        BOOL WINAPI HookedWriteFile(HANDLE fileHandle, LPCVOID bufferPointer, DWORD bytesToWrite, LPDWORD bytesWrittenPointer, LPOVERLAPPED overlappedPointer)
        {
            if (IsMonitorPipeHandle(fileHandle))
            {
                return g_writeFileOriginal(fileHandle, bufferPointer, bytesToWrite, bytesWrittenPointer, overlappedPointer);
            }

            ScopedHookGuard guardValue;
            if (guardValue.bypass())
            {
                return g_writeFileOriginal(fileHandle, bufferPointer, bytesToWrite, bytesWrittenPointer, overlappedPointer);
            }

            const BOOL resultValue = g_writeFileOriginal(fileHandle, bufferPointer, bytesToWrite, bytesWrittenPointer, overlappedPointer);
            const DWORD lastError = ::GetLastError();
            const DWORD bytesWrittenValue = bytesWrittenPointer != nullptr ? *bytesWrittenPointer : 0;
            SendMonitorEvent(
                ks::winapi_monitor::EventCategory::File,
                L"KernelBase",
                L"WriteFile",
                resultValue != FALSE ? 0 : static_cast<std::int32_t>(lastError),
                TrimDetail(L"handle=" + HandleText(fileHandle) + L" request=" + std::to_wstring(bytesToWrite) + L" written=" + std::to_wstring(bytesWrittenValue)));
            ::SetLastError(lastError);
            return resultValue;
        }

        BOOL WINAPI HookedCreateProcessW(
            LPCWSTR applicationNamePointer,
            LPWSTR commandLinePointer,
            LPSECURITY_ATTRIBUTES processAttributes,
            LPSECURITY_ATTRIBUTES threadAttributes,
            BOOL inheritHandles,
            DWORD creationFlags,
            LPVOID environmentPointer,
            LPCWSTR currentDirectoryPointer,
            LPSTARTUPINFOW startupInfoPointer,
            LPPROCESS_INFORMATION processInfoPointer)
        {
            ScopedHookGuard guardValue;
            if (guardValue.bypass())
            {
                return g_createProcessWOriginal(applicationNamePointer, commandLinePointer, processAttributes, threadAttributes, inheritHandles, creationFlags, environmentPointer, currentDirectoryPointer, startupInfoPointer, processInfoPointer);
            }

            const std::wstring appNameText = SafeWideText(applicationNamePointer);
            const std::wstring commandLineText = commandLinePointer != nullptr ? std::wstring(commandLinePointer) : std::wstring();
            const std::wstring currentDirectoryText = SafeWideText(currentDirectoryPointer);

            const BOOL resultValue = g_createProcessWOriginal(
                applicationNamePointer,
                commandLinePointer,
                processAttributes,
                threadAttributes,
                inheritHandles,
                creationFlags,
                environmentPointer,
                currentDirectoryPointer,
                startupInfoPointer,
                processInfoPointer);
            const DWORD lastError = ::GetLastError();
            const DWORD childPid = (resultValue != FALSE && processInfoPointer != nullptr) ? processInfoPointer->dwProcessId : 0;
            AutoInjectChildIfRequested(resultValue, processInfoPointer);
            SendMonitorEvent(
                ks::winapi_monitor::EventCategory::Process,
                L"KernelBase",
                L"CreateProcessW",
                resultValue != FALSE ? 0 : static_cast<std::int32_t>(lastError),
                TrimDetail(
                    L"app=" + appNameText
                    + L" cmd=" + commandLineText
                    + L" cwd=" + currentDirectoryText
                    + L" flags=" + HexValue(creationFlags)
                    + L" inherit=" + std::to_wstring(inheritHandles != FALSE)
                    + L" childPid=" + std::to_wstring(childPid)));
            ::SetLastError(lastError);
            return resultValue;
        }

        LSTATUS WINAPI HookedRegOpenKeyExW(HKEY rootKey, LPCWSTR subKeyPointer, DWORD optionsValue, REGSAM samDesired, PHKEY resultKeyPointer)
        {
            ScopedHookGuard guardValue;
            if (guardValue.bypass())
            {
                return g_regOpenKeyExWOriginal(rootKey, subKeyPointer, optionsValue, samDesired, resultKeyPointer);
            }

            const LSTATUS statusValue = g_regOpenKeyExWOriginal(rootKey, subKeyPointer, optionsValue, samDesired, resultKeyPointer);
            const DWORD lastError = ::GetLastError();
            wchar_t detailBuffer[ks::winapi_monitor::kMaxDetailChars] = {};
            BuildRegOpenDetail(detailBuffer, rootKey, subKeyPointer, samDesired);
            SendMonitorEventRaw(
                ks::winapi_monitor::EventCategory::Registry,
                L"Advapi32",
                L"RegOpenKeyExW",
                static_cast<std::int32_t>(statusValue),
                detailBuffer);
            ::SetLastError(lastError);
            return statusValue;
        }

        LSTATUS WINAPI HookedRegCreateKeyExW(HKEY rootKey, LPCWSTR subKeyPointer, DWORD reservedValue, LPWSTR classPointer, DWORD optionsValue, REGSAM samDesired, const LPSECURITY_ATTRIBUTES securityAttributes, PHKEY resultKeyPointer, LPDWORD dispositionPointer)
        {
            (void)reservedValue;
            (void)classPointer;
            (void)securityAttributes;
            ScopedHookGuard guardValue;
            if (guardValue.bypass())
            {
                return g_regCreateKeyExWOriginal(rootKey, subKeyPointer, reservedValue, classPointer, optionsValue, samDesired, securityAttributes, resultKeyPointer, dispositionPointer);
            }

            const LSTATUS statusValue = g_regCreateKeyExWOriginal(rootKey, subKeyPointer, reservedValue, classPointer, optionsValue, samDesired, securityAttributes, resultKeyPointer, dispositionPointer);
            const DWORD lastError = ::GetLastError();
            const DWORD dispositionValue = dispositionPointer != nullptr ? *dispositionPointer : 0;
            wchar_t detailBuffer[ks::winapi_monitor::kMaxDetailChars] = {};
            BuildRegCreateDetail(detailBuffer, rootKey, subKeyPointer, optionsValue, dispositionValue);
            SendMonitorEventRaw(
                ks::winapi_monitor::EventCategory::Registry,
                L"Advapi32",
                L"RegCreateKeyExW",
                static_cast<std::int32_t>(statusValue),
                detailBuffer);
            ::SetLastError(lastError);
            return statusValue;
        }

        LSTATUS WINAPI HookedRegQueryValueExW(HKEY keyHandle, LPCWSTR valueNamePointer, LPDWORD reservedPointer, LPDWORD typePointer, LPBYTE dataPointer, LPDWORD dataSizePointer)
        {
            ScopedHookGuard guardValue;
            if (guardValue.bypass())
            {
                return g_regQueryValueExWOriginal(keyHandle, valueNamePointer, reservedPointer, typePointer, dataPointer, dataSizePointer);
            }

            const LSTATUS statusValue = g_regQueryValueExWOriginal(keyHandle, valueNamePointer, reservedPointer, typePointer, dataPointer, dataSizePointer);
            const DWORD lastError = ::GetLastError();
            const DWORD typeValue = typePointer != nullptr ? *typePointer : 0;
            const DWORD dataSize = dataSizePointer != nullptr ? *dataSizePointer : 0;
            wchar_t detailBuffer[ks::winapi_monitor::kMaxDetailChars] = {};
            BuildRegValueDetail(detailBuffer, L"hkey=", keyHandle, valueNamePointer, typeValue, dataSize);
            SendMonitorEventRaw(
                ks::winapi_monitor::EventCategory::Registry,
                L"Advapi32",
                L"RegQueryValueExW",
                static_cast<std::int32_t>(statusValue),
                detailBuffer);
            ::SetLastError(lastError);
            return statusValue;
        }

        LSTATUS WINAPI HookedRegGetValueW(HKEY keyHandle, LPCWSTR subKeyPointer, LPCWSTR valueNamePointer, DWORD flagsValue, LPDWORD typePointer, PVOID dataPointer, LPDWORD dataSizePointer)
        {
            ScopedHookGuard guardValue;
            if (guardValue.bypass())
            {
                return g_regGetValueWOriginal(keyHandle, subKeyPointer, valueNamePointer, flagsValue, typePointer, dataPointer, dataSizePointer);
            }

            const LSTATUS statusValue = g_regGetValueWOriginal(keyHandle, subKeyPointer, valueNamePointer, flagsValue, typePointer, dataPointer, dataSizePointer);
            const DWORD lastError = ::GetLastError();
            const DWORD typeValue = typePointer != nullptr ? *typePointer : 0;
            const DWORD dataSize = dataSizePointer != nullptr ? *dataSizePointer : 0;
            wchar_t detailBuffer[ks::winapi_monitor::kMaxDetailChars] = {};
            BuildRegGetValueDetail(detailBuffer, keyHandle, subKeyPointer, valueNamePointer, flagsValue, typeValue, dataSize);
            SendMonitorEventRaw(
                ks::winapi_monitor::EventCategory::Registry,
                L"Advapi32",
                L"RegGetValueW",
                static_cast<std::int32_t>(statusValue),
                detailBuffer);
            ::SetLastError(lastError);
            return statusValue;
        }

        LSTATUS WINAPI HookedRegSetValueExW(HKEY keyHandle, LPCWSTR valueNamePointer, DWORD reservedValue, DWORD typeValue, const BYTE* dataPointer, DWORD dataSize)
        {
            (void)reservedValue;
            (void)dataPointer;
            ScopedHookGuard guardValue;
            if (guardValue.bypass())
            {
                return g_regSetValueExWOriginal(keyHandle, valueNamePointer, reservedValue, typeValue, dataPointer, dataSize);
            }

            const LSTATUS statusValue = g_regSetValueExWOriginal(keyHandle, valueNamePointer, reservedValue, typeValue, dataPointer, dataSize);
            const DWORD lastError = ::GetLastError();
            wchar_t detailBuffer[ks::winapi_monitor::kMaxDetailChars] = {};
            BuildRegSetValueDetail(detailBuffer, keyHandle, valueNamePointer, typeValue, dataSize);
            SendMonitorEventRaw(
                ks::winapi_monitor::EventCategory::Registry,
                L"Advapi32",
                L"RegSetValueExW",
                static_cast<std::int32_t>(statusValue),
                detailBuffer);
            ::SetLastError(lastError);
            return statusValue;
        }

        LSTATUS WINAPI HookedRegDeleteValueW(HKEY keyHandle, LPCWSTR valueNamePointer)
        {
            ScopedHookGuard guardValue;
            if (guardValue.bypass())
            {
                return g_regDeleteValueWOriginal(keyHandle, valueNamePointer);
            }

            const LSTATUS statusValue = g_regDeleteValueWOriginal(keyHandle, valueNamePointer);
            const DWORD lastError = ::GetLastError();
            wchar_t detailBuffer[ks::winapi_monitor::kMaxDetailChars] = {};
            BuildRegValueDetail(detailBuffer, L"hkey=", keyHandle, valueNamePointer, 0, 0);
            SendMonitorEventRaw(
                ks::winapi_monitor::EventCategory::Registry,
                L"Advapi32",
                L"RegDeleteValueW",
                static_cast<std::int32_t>(statusValue),
                detailBuffer);
            ::SetLastError(lastError);
            return statusValue;
        }

        LSTATUS WINAPI HookedRegDeleteKeyW(HKEY rootKey, LPCWSTR subKeyPointer)
        {
            ScopedHookGuard guardValue;
            if (guardValue.bypass())
            {
                return g_regDeleteKeyWOriginal(rootKey, subKeyPointer);
            }

            const LSTATUS statusValue = g_regDeleteKeyWOriginal(rootKey, subKeyPointer);
            const DWORD lastError = ::GetLastError();
            wchar_t detailBuffer[ks::winapi_monitor::kMaxDetailChars] = {};
            BuildRegSubKeyDetail(detailBuffer, rootKey, subKeyPointer, 0);
            SendMonitorEventRaw(
                ks::winapi_monitor::EventCategory::Registry,
                L"Advapi32",
                L"RegDeleteKeyW",
                static_cast<std::int32_t>(statusValue),
                detailBuffer);
            ::SetLastError(lastError);
            return statusValue;
        }

        LSTATUS WINAPI HookedRegDeleteKeyExW(HKEY rootKey, LPCWSTR subKeyPointer, REGSAM samDesired, DWORD reservedValue)
        {
            ScopedHookGuard guardValue;
            if (guardValue.bypass())
            {
                return g_regDeleteKeyExWOriginal(rootKey, subKeyPointer, samDesired, reservedValue);
            }

            const LSTATUS statusValue = g_regDeleteKeyExWOriginal(rootKey, subKeyPointer, samDesired, reservedValue);
            const DWORD lastError = ::GetLastError();
            wchar_t detailBuffer[ks::winapi_monitor::kMaxDetailChars] = {};
            BuildRegSubKeyDetail(detailBuffer, rootKey, subKeyPointer, samDesired);
            SendMonitorEventRaw(
                ks::winapi_monitor::EventCategory::Registry,
                L"Advapi32",
                L"RegDeleteKeyExW",
                static_cast<std::int32_t>(statusValue),
                detailBuffer);
            ::SetLastError(lastError);
            return statusValue;
        }

        LSTATUS WINAPI HookedRegEnumKeyExW(HKEY keyHandle, DWORD indexValue, LPWSTR namePointer, LPDWORD nameLengthPointer, LPDWORD reservedPointer, LPWSTR classPointer, LPDWORD classLengthPointer, PFILETIME lastWriteTimePointer)
        {
            ScopedHookGuard guardValue;
            if (guardValue.bypass())
            {
                return g_regEnumKeyExWOriginal(keyHandle, indexValue, namePointer, nameLengthPointer, reservedPointer, classPointer, classLengthPointer, lastWriteTimePointer);
            }

            const LSTATUS statusValue = g_regEnumKeyExWOriginal(keyHandle, indexValue, namePointer, nameLengthPointer, reservedPointer, classPointer, classLengthPointer, lastWriteTimePointer);
            const DWORD lastError = ::GetLastError();
            const DWORD nameLength = nameLengthPointer != nullptr ? *nameLengthPointer : 0;
            wchar_t detailBuffer[ks::winapi_monitor::kMaxDetailChars] = {};
            BuildRegEnumKeyDetail(
                detailBuffer,
                keyHandle,
                indexValue,
                statusValue == ERROR_SUCCESS ? namePointer : nullptr,
                statusValue == ERROR_SUCCESS ? nameLength : 0);
            SendMonitorEventRaw(
                ks::winapi_monitor::EventCategory::Registry,
                L"Advapi32",
                L"RegEnumKeyExW",
                static_cast<std::int32_t>(statusValue),
                detailBuffer);
            ::SetLastError(lastError);
            return statusValue;
        }

        LSTATUS WINAPI HookedRegEnumValueW(HKEY keyHandle, DWORD indexValue, LPWSTR valueNamePointer, LPDWORD valueNameLengthPointer, LPDWORD reservedPointer, LPDWORD typePointer, LPBYTE dataPointer, LPDWORD dataSizePointer)
        {
            ScopedHookGuard guardValue;
            if (guardValue.bypass())
            {
                return g_regEnumValueWOriginal(keyHandle, indexValue, valueNamePointer, valueNameLengthPointer, reservedPointer, typePointer, dataPointer, dataSizePointer);
            }

            const LSTATUS statusValue = g_regEnumValueWOriginal(keyHandle, indexValue, valueNamePointer, valueNameLengthPointer, reservedPointer, typePointer, dataPointer, dataSizePointer);
            const DWORD lastError = ::GetLastError();
            const DWORD nameLength = valueNameLengthPointer != nullptr ? *valueNameLengthPointer : 0;
            const DWORD typeValue = typePointer != nullptr ? *typePointer : 0;
            const DWORD dataSize = dataSizePointer != nullptr ? *dataSizePointer : 0;
            wchar_t detailBuffer[ks::winapi_monitor::kMaxDetailChars] = {};
            BuildRegValueDetail(
                detailBuffer,
                L"hkey=",
                keyHandle,
                statusValue == ERROR_SUCCESS ? valueNamePointer : nullptr,
                typeValue,
                dataSize);
            AppendWideText(detailBuffer, L" index=");
            AppendUnsignedText(detailBuffer, static_cast<unsigned long long>(indexValue));
            AppendWideText(detailBuffer, L" nameLen=");
            AppendUnsignedText(detailBuffer, static_cast<unsigned long long>(nameLength));
            SendMonitorEventRaw(
                ks::winapi_monitor::EventCategory::Registry,
                L"Advapi32",
                L"RegEnumValueW",
                static_cast<std::int32_t>(statusValue),
                detailBuffer);
            ::SetLastError(lastError);
            return statusValue;
        }

        LSTATUS WINAPI HookedRegCloseKey(HKEY keyHandle)
        {
            ScopedHookGuard guardValue;
            if (guardValue.bypass())
            {
                return g_regCloseKeyOriginal(keyHandle);
            }

            const LSTATUS statusValue = g_regCloseKeyOriginal(keyHandle);
            const DWORD lastError = ::GetLastError();
            wchar_t detailBuffer[ks::winapi_monitor::kMaxDetailChars] = {};
            detailBuffer[0] = L'\0';
            AppendWideText(detailBuffer, L"hkey=");
            AppendHexText(detailBuffer, reinterpret_cast<std::uint64_t>(keyHandle));
            SendMonitorEventRaw(
                ks::winapi_monitor::EventCategory::Registry,
                L"Advapi32",
                L"RegCloseKey",
                static_cast<std::int32_t>(statusValue),
                detailBuffer);
            ::SetLastError(lastError);
            return statusValue;
        }

        int WSAAPI HookedConnect(SOCKET socketValue, const sockaddr* namePointer, int nameLength)
        {
            ScopedHookGuard guardValue;
            if (guardValue.bypass())
            {
                return g_connectOriginal(socketValue, namePointer, nameLength);
            }

            const int resultValue = g_connectOriginal(socketValue, namePointer, nameLength);
            const int errorValue = resultValue == 0 ? 0 : ::WSAGetLastError();
            wchar_t detailBuffer[ks::winapi_monitor::kMaxDetailChars] = {};
            BuildSocketDetail(detailBuffer, L"connect", socketValue, 0, 0, 0, namePointer, nameLength);
            SendMonitorEventRaw(
                ks::winapi_monitor::EventCategory::Network,
                L"Ws2_32",
                L"connect",
                errorValue,
                detailBuffer);
            if (resultValue != 0)
            {
                ::WSASetLastError(errorValue);
            }
            return resultValue;
        }

        int WSAAPI HookedWSAConnect(SOCKET socketValue, const sockaddr* namePointer, int nameLength, LPWSABUF callerDataPointer, LPWSABUF calleeDataPointer, LPQOS socketQosPointer, LPQOS groupQosPointer)
        {
            (void)callerDataPointer;
            (void)calleeDataPointer;
            (void)socketQosPointer;
            (void)groupQosPointer;
            ScopedHookGuard guardValue;
            if (guardValue.bypass())
            {
                return g_wsaConnectOriginal(socketValue, namePointer, nameLength, callerDataPointer, calleeDataPointer, socketQosPointer, groupQosPointer);
            }

            const int resultValue = g_wsaConnectOriginal(socketValue, namePointer, nameLength, callerDataPointer, calleeDataPointer, socketQosPointer, groupQosPointer);
            const int errorValue = resultValue == 0 ? 0 : ::WSAGetLastError();
            wchar_t detailBuffer[ks::winapi_monitor::kMaxDetailChars] = {};
            BuildSocketDetail(detailBuffer, L"connect", socketValue, 0, 0, 0, namePointer, nameLength);
            SendMonitorEventRaw(
                ks::winapi_monitor::EventCategory::Network,
                L"Ws2_32",
                L"WSAConnect",
                errorValue,
                detailBuffer);
            if (resultValue != 0)
            {
                ::WSASetLastError(errorValue);
            }
            return resultValue;
        }

        int WSAAPI HookedSend(SOCKET socketValue, const char* bufferPointer, int bufferLength, int flagsValue)
        {
            (void)bufferPointer;
            ScopedHookGuard guardValue;
            if (guardValue.bypass())
            {
                return g_sendOriginal(socketValue, bufferPointer, bufferLength, flagsValue);
            }

            const int resultValue = g_sendOriginal(socketValue, bufferPointer, bufferLength, flagsValue);
            const int errorValue = resultValue >= 0 ? 0 : ::WSAGetLastError();
            wchar_t detailBuffer[ks::winapi_monitor::kMaxDetailChars] = {};
            BuildSocketDetail(detailBuffer, L"send", socketValue, static_cast<std::uint64_t>(bufferLength < 0 ? 0 : bufferLength), resultValue, static_cast<DWORD>(flagsValue));
            SendMonitorEventRaw(
                ks::winapi_monitor::EventCategory::Network,
                L"Ws2_32",
                L"send",
                errorValue,
                detailBuffer);
            if (resultValue < 0)
            {
                ::WSASetLastError(errorValue);
            }
            return resultValue;
        }

        int WSAAPI HookedWSASend(SOCKET socketValue, LPWSABUF buffersPointer, DWORD bufferCount, LPDWORD bytesSentPointer, DWORD flagsValue, LPWSAOVERLAPPED overlappedPointer, LPWSAOVERLAPPED_COMPLETION_ROUTINE completionRoutinePointer)
        {
            ScopedHookGuard guardValue;
            if (guardValue.bypass())
            {
                return g_wsaSendOriginal(socketValue, buffersPointer, bufferCount, bytesSentPointer, flagsValue, overlappedPointer, completionRoutinePointer);
            }

            const std::uint64_t requestLength = SumWsaBufferLength(buffersPointer, bufferCount);
            const int resultValue = g_wsaSendOriginal(socketValue, buffersPointer, bufferCount, bytesSentPointer, flagsValue, overlappedPointer, completionRoutinePointer);
            const int errorValue = resultValue == 0 ? 0 : ::WSAGetLastError();
            const DWORD sentValue = bytesSentPointer != nullptr ? *bytesSentPointer : 0;
            wchar_t detailBuffer[ks::winapi_monitor::kMaxDetailChars] = {};
            BuildSocketDetail(detailBuffer, L"send", socketValue, requestLength, sentValue, flagsValue);
            SendMonitorEventRaw(
                ks::winapi_monitor::EventCategory::Network,
                L"Ws2_32",
                L"WSASend",
                errorValue,
                detailBuffer);
            if (resultValue != 0)
            {
                ::WSASetLastError(errorValue);
            }
            return resultValue;
        }

        int WSAAPI HookedSendTo(SOCKET socketValue, const char* bufferPointer, int bufferLength, int flagsValue, const sockaddr* toPointer, int toLength)
        {
            (void)bufferPointer;
            ScopedHookGuard guardValue;
            if (guardValue.bypass())
            {
                return g_sendToOriginal(socketValue, bufferPointer, bufferLength, flagsValue, toPointer, toLength);
            }

            const int resultValue = g_sendToOriginal(socketValue, bufferPointer, bufferLength, flagsValue, toPointer, toLength);
            const int errorValue = resultValue >= 0 ? 0 : ::WSAGetLastError();
            wchar_t detailBuffer[ks::winapi_monitor::kMaxDetailChars] = {};
            BuildSocketDetail(detailBuffer, L"sendto", socketValue, static_cast<std::uint64_t>(bufferLength < 0 ? 0 : bufferLength), resultValue, static_cast<DWORD>(flagsValue), toPointer, toLength);
            SendMonitorEventRaw(
                ks::winapi_monitor::EventCategory::Network,
                L"Ws2_32",
                L"sendto",
                errorValue,
                detailBuffer);
            if (resultValue < 0)
            {
                ::WSASetLastError(errorValue);
            }
            return resultValue;
        }

        int WSAAPI HookedRecv(SOCKET socketValue, char* bufferPointer, int bufferLength, int flagsValue)
        {
            (void)bufferPointer;
            ScopedHookGuard guardValue;
            if (guardValue.bypass())
            {
                return g_recvOriginal(socketValue, bufferPointer, bufferLength, flagsValue);
            }

            const int resultValue = g_recvOriginal(socketValue, bufferPointer, bufferLength, flagsValue);
            const int errorValue = resultValue >= 0 ? 0 : ::WSAGetLastError();
            wchar_t detailBuffer[ks::winapi_monitor::kMaxDetailChars] = {};
            BuildSocketDetail(detailBuffer, L"recv", socketValue, static_cast<std::uint64_t>(bufferLength < 0 ? 0 : bufferLength), resultValue, static_cast<DWORD>(flagsValue));
            SendMonitorEventRaw(
                ks::winapi_monitor::EventCategory::Network,
                L"Ws2_32",
                L"recv",
                errorValue,
                detailBuffer);
            if (resultValue < 0)
            {
                ::WSASetLastError(errorValue);
            }
            return resultValue;
        }

        int WSAAPI HookedWSARecv(SOCKET socketValue, LPWSABUF buffersPointer, DWORD bufferCount, LPDWORD bytesReceivedPointer, LPDWORD flagsPointer, LPWSAOVERLAPPED overlappedPointer, LPWSAOVERLAPPED_COMPLETION_ROUTINE completionRoutinePointer)
        {
            ScopedHookGuard guardValue;
            if (guardValue.bypass())
            {
                return g_wsaRecvOriginal(socketValue, buffersPointer, bufferCount, bytesReceivedPointer, flagsPointer, overlappedPointer, completionRoutinePointer);
            }

            const std::uint64_t requestLength = SumWsaBufferLength(buffersPointer, bufferCount);
            const int resultValue = g_wsaRecvOriginal(socketValue, buffersPointer, bufferCount, bytesReceivedPointer, flagsPointer, overlappedPointer, completionRoutinePointer);
            const int errorValue = resultValue == 0 ? 0 : ::WSAGetLastError();
            const DWORD receivedValue = bytesReceivedPointer != nullptr ? *bytesReceivedPointer : 0;
            const DWORD flagsValue = flagsPointer != nullptr ? *flagsPointer : 0;
            wchar_t detailBuffer[ks::winapi_monitor::kMaxDetailChars] = {};
            BuildSocketDetail(detailBuffer, L"recv", socketValue, requestLength, receivedValue, flagsValue);
            SendMonitorEventRaw(
                ks::winapi_monitor::EventCategory::Network,
                L"Ws2_32",
                L"WSARecv",
                errorValue,
                detailBuffer);
            if (resultValue != 0)
            {
                ::WSASetLastError(errorValue);
            }
            return resultValue;
        }

        int WSAAPI HookedRecvFrom(SOCKET socketValue, char* bufferPointer, int bufferLength, int flagsValue, sockaddr* fromPointer, int* fromLengthPointer)
        {
            (void)bufferPointer;
            ScopedHookGuard guardValue;
            if (guardValue.bypass())
            {
                return g_recvFromOriginal(socketValue, bufferPointer, bufferLength, flagsValue, fromPointer, fromLengthPointer);
            }

            const int resultValue = g_recvFromOriginal(socketValue, bufferPointer, bufferLength, flagsValue, fromPointer, fromLengthPointer);
            const int errorValue = resultValue >= 0 ? 0 : ::WSAGetLastError();
            const int fromLength = fromLengthPointer != nullptr ? *fromLengthPointer : 0;
            wchar_t detailBuffer[ks::winapi_monitor::kMaxDetailChars] = {};
            BuildSocketDetail(detailBuffer, L"recvfrom", socketValue, static_cast<std::uint64_t>(bufferLength < 0 ? 0 : bufferLength), resultValue, static_cast<DWORD>(flagsValue), fromPointer, fromLength);
            SendMonitorEventRaw(
                ks::winapi_monitor::EventCategory::Network,
                L"Ws2_32",
                L"recvfrom",
                errorValue,
                detailBuffer);
            if (resultValue < 0)
            {
                ::WSASetLastError(errorValue);
            }
            return resultValue;
        }

        int WSAAPI HookedBind(SOCKET socketValue, const sockaddr* namePointer, int nameLength)
        {
            ScopedHookGuard guardValue;
            if (guardValue.bypass())
            {
                return g_bindOriginal(socketValue, namePointer, nameLength);
            }

            const int resultValue = g_bindOriginal(socketValue, namePointer, nameLength);
            const int errorValue = resultValue == 0 ? 0 : ::WSAGetLastError();
            wchar_t detailBuffer[ks::winapi_monitor::kMaxDetailChars] = {};
            BuildSocketDetail(detailBuffer, L"bind", socketValue, 0, 0, 0, namePointer, nameLength);
            SendMonitorEventRaw(
                ks::winapi_monitor::EventCategory::Network,
                L"Ws2_32",
                L"bind",
                errorValue,
                detailBuffer);
            if (resultValue != 0)
            {
                ::WSASetLastError(errorValue);
            }
            return resultValue;
        }

        int WSAAPI HookedListen(SOCKET socketValue, int backlogValue)
        {
            ScopedHookGuard guardValue;
            if (guardValue.bypass())
            {
                return g_listenOriginal(socketValue, backlogValue);
            }

            const int resultValue = g_listenOriginal(socketValue, backlogValue);
            const int errorValue = resultValue == 0 ? 0 : ::WSAGetLastError();
            wchar_t detailBuffer[ks::winapi_monitor::kMaxDetailChars] = {};
            detailBuffer[0] = L'\0';
            AppendWideText(detailBuffer, L"socket=");
            AppendHexText(detailBuffer, static_cast<std::uint64_t>(socketValue));
            AppendWideText(detailBuffer, L" backlog=");
            AppendUnsignedText(detailBuffer, static_cast<unsigned long long>(backlogValue < 0 ? 0 : backlogValue));
            SendMonitorEventRaw(
                ks::winapi_monitor::EventCategory::Network,
                L"Ws2_32",
                L"listen",
                errorValue,
                detailBuffer);
            if (resultValue != 0)
            {
                ::WSASetLastError(errorValue);
            }
            return resultValue;
        }

        SOCKET WSAAPI HookedAccept(SOCKET socketValue, sockaddr* addressPointer, int* addressLengthPointer)
        {
            ScopedHookGuard guardValue;
            if (guardValue.bypass())
            {
                return g_acceptOriginal(socketValue, addressPointer, addressLengthPointer);
            }

            const SOCKET resultSocket = g_acceptOriginal(socketValue, addressPointer, addressLengthPointer);
            const int errorValue = resultSocket != INVALID_SOCKET ? 0 : ::WSAGetLastError();
            const int addressLength = addressLengthPointer != nullptr ? *addressLengthPointer : 0;
            wchar_t detailBuffer[ks::winapi_monitor::kMaxDetailChars] = {};
            BuildSocketDetail(detailBuffer, L"accept", socketValue, 0, static_cast<long long>(resultSocket == INVALID_SOCKET ? 0 : resultSocket), 0, addressPointer, addressLength);
            SendMonitorEventRaw(
                ks::winapi_monitor::EventCategory::Network,
                L"Ws2_32",
                L"accept",
                errorValue,
                detailBuffer);
            if (resultSocket == INVALID_SOCKET)
            {
                ::WSASetLastError(errorValue);
            }
            return resultSocket;
        }
    }

    bool InstallConfiguredHooks(std::wstring* errorTextOut)
    {
        const std::lock_guard<std::mutex> lock(g_hookOperationMutex);
        if (errorTextOut != nullptr)
        {
            errorTextOut->clear();
        }

        bool hasEnabledCategory = false;
        bool installedAny = false;
        std::wstring failureText;
        for (HookBinding& bindingValue : g_bindings)
        {
            hasEnabledCategory = CategoryEnabled(bindingValue.categoryValue) || hasEnabledCategory;
            installedAny = TryInstallBinding(bindingValue, &failureText) || installedAny;
        }
        if (!hasEnabledCategory)
        {
            if (errorTextOut != nullptr)
            {
                *errorTextOut = L"No hook category is enabled in current config.";
            }
            return false;
        }
        if (!installedAny && errorTextOut != nullptr)
        {
            *errorTextOut = failureText.empty()
                ? std::wstring(L"No hook installed successfully.")
                : failureText;
            return false;
        }
        if (installedAny && errorTextOut != nullptr)
        {
            *errorTextOut = failureText;
        }
        return installedAny;
    }

    void UninstallConfiguredHooks()
    {
        const std::lock_guard<std::mutex> lock(g_hookOperationMutex);
        for (HookBinding& bindingValue : g_bindings)
        {
            UninstallInlineHook(bindingValue.hookRecord);
            if (bindingValue.originalOut != nullptr)
            {
                *bindingValue.originalOut = nullptr;
            }
        }
    }

    void RetryPendingHooks()
    {
        const std::lock_guard<std::mutex> lock(g_hookOperationMutex);
        RetryPendingHooksUnlocked();
    }
}

