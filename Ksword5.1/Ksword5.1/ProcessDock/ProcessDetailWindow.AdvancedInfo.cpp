#include "ProcessDetailWindow.InternalCommon.h"

using namespace process_detail_window_internal;

// ============================================================
// ProcessDetailWindow.AdvancedInfo.cpp
// 作用：
// - 线程细节异步刷新；
// - 令牌详情异步刷新；
// - 令牌开关读取与应用（NtSetInformationToken）；
// - PEB/内存摘要异步刷新。
// ============================================================

namespace
{
    // NtQueryInformationThread 函数签名：
    // - 第二个参数直接用 ULONG，规避 SDK 枚举差异。
    using NtQueryInformationThreadFn = NTSTATUS(NTAPI*)(HANDLE, ULONG, PVOID, ULONG, PULONG);

    // NtQueryInformationProcess 函数签名：
    // - 这里统一使用 ULONG 信息类，避免 SDK 版本差异导致的编译问题。
    using NtQueryInformationProcessFn = NTSTATUS(NTAPI*)(HANDLE, ULONG, PVOID, ULONG, PULONG);
    // NtSetInformationToken 函数签名：
    // - Windows Native API 入口名为 NtSetInformationToken；
    // - 本文件把它作为“NtSetTokenInformation”语义来驱动令牌开关写入。
    using NtSetInformationTokenFn = NTSTATUS(NTAPI*)(HANDLE, TOKEN_INFORMATION_CLASS, PVOID, ULONG);

    // PROCESSINFOCLASS 关键常量：
    // - 这些值用于补齐令牌页/PEB页的深度信息读取；
    // - 若目标系统不支持对应信息类，查询会返回失败并自动降级。
    constexpr ULONG kProcessInfoClassDebugPort = 7;
    constexpr ULONG kProcessInfoClassBreakOnTermination = 29;
    constexpr ULONG kProcessInfoClassExecuteFlags = 34;
    constexpr ULONG kProcessInfoClassCommandLine = 60;
    constexpr ULONG kProcessInfoClassProtection = 61;
    constexpr ULONG kProcessInfoClassSubsystem = 75;

    // MandatoryPolicy 位常量：
    // - 某些 SDK 版本可能没有导出宏；
    // - 这里做本地 constexpr 兜底，保证工程可编译。
#ifndef TOKEN_MANDATORY_POLICY_NO_WRITE_UP
    constexpr DWORD kTokenMandatoryPolicyNoWriteUp = 0x1;
#else
    constexpr DWORD kTokenMandatoryPolicyNoWriteUp = TOKEN_MANDATORY_POLICY_NO_WRITE_UP;
#endif
#ifndef TOKEN_MANDATORY_POLICY_NEW_PROCESS_MIN
    constexpr DWORD kTokenMandatoryPolicyNewProcessMin = 0x2;
#else
    constexpr DWORD kTokenMandatoryPolicyNewProcessMin = TOKEN_MANDATORY_POLICY_NEW_PROCESS_MIN;
#endif

    // TOKEN_ADJUST_SESSIONID 兜底常量：
    // - 某些旧 SDK 可能未定义该访问位；
    // - 这里统一给出数值兜底，避免编译差异。
#ifndef TOKEN_ADJUST_SESSIONID
    constexpr DWORD kTokenAdjustSessionIdAccess = 0x0100;
#else
    constexpr DWORD kTokenAdjustSessionIdAccess = TOKEN_ADJUST_SESSIONID;
#endif

    // 布尔语义 TokenInformationClass 常量：
    // - 使用数值常量规避不同 SDK 头文件的枚举可见性差异；
    // - 与“令牌开关页”新增复选框一一对应。
    constexpr TOKEN_INFORMATION_CLASS kTokenInfoClassHasRestrictions =
        static_cast<TOKEN_INFORMATION_CLASS>(21);
    constexpr TOKEN_INFORMATION_CLASS kTokenInfoClassIsAppContainer =
        static_cast<TOKEN_INFORMATION_CLASS>(29);
    constexpr TOKEN_INFORMATION_CLASS kTokenInfoClassIsRestricted =
        static_cast<TOKEN_INFORMATION_CLASS>(40);
    constexpr TOKEN_INFORMATION_CLASS kTokenInfoClassIsLessPrivilegedAppContainer =
        static_cast<TOKEN_INFORMATION_CLASS>(46);
    constexpr TOKEN_INFORMATION_CLASS kTokenInfoClassIsSandboxed =
        static_cast<TOKEN_INFORMATION_CLASS>(47);
    constexpr TOKEN_INFORMATION_CLASS kTokenInfoClassIsAppSilo =
        static_cast<TOKEN_INFORMATION_CLASS>(51);

    // GetProcessInformation(ProcessPowerThrottling) 所需结构：
    // - 采用本地定义，避免依赖高版本 SDK 才可见的类型声明。
    struct ProcessPowerThrottlingStateNative
    {
        ULONG version = 0;      // 结构版本号。
        ULONG controlMask = 0;  // 可控位掩码。
        ULONG stateMask = 0;    // 当前状态位掩码。
    };

    // GetProcessInformation 动态函数签名：
    // - 第二个参数用 ULONG 表示信息类，兼容不同头文件环境。
    using GetProcessInformationFn = BOOL(WINAPI*)(HANDLE, ULONG, LPVOID, DWORD);

    // ProcessWow64Information：
    // - NtQueryInformationProcess 信息类 26；
    // - 64 位控制端读取 32 位目标时，需要先拿到 Wow64 PEB，再按 32 位结构解析。
    constexpr ULONG kProcessInfoClassWow64Information = 26;

    // PEB 文本读取上限：
    // - UNICODE_STRING 长度来自远程进程，必须先做边界限制；
    // - 防止损坏/伪造的 ProcessParameters 让 UI 后台线程分配超大内存。
    constexpr std::size_t kMaxRemoteUnicodeBytes = 256 * 1024;

    // 环境块预览上限：
    // - PEB 内没有稳定可跨版本依赖的环境块长度字段；
    // - 采用分块读取并在双 NUL 结束或达到上限时停止。
    constexpr std::size_t kMaxEnvironmentPreviewBytes = 128 * 1024;
    constexpr std::size_t kEnvironmentReadChunkBytes = 4096;
    constexpr std::size_t kMaxEnvironmentPreviewLines = 20;
    constexpr std::size_t kMaxEnvironmentLineChars = 4096;

    // RemoteUnicodeString32：
    // - 32 位目标进程内的 UNICODE_STRING 布局；
    // - Buffer 是远程 32 位地址，不能直接使用当前进程的 UNICODE_STRING。
    struct RemoteUnicodeString32 final
    {
        USHORT length = 0;        // 字节长度，不含终止 NUL。
        USHORT maximumLength = 0; // 字节容量，可能大于 length。
        std::uint32_t buffer = 0; // 远程 32 位 PWSTR 地址。
    };

    // Curdir32/Curdir64：
    // - 对应 RTL_USER_PROCESS_PARAMETERS.CurrentDirectory；
    // - 只保留 DosPath 与 Handle，满足详情页显示当前目录。
    struct Curdir32 final
    {
        RemoteUnicodeString32 dosPath{};
        std::uint32_t handle = 0;
    };

    struct Curdir64 final
    {
        UNICODE_STRING dosPath{};
        PVOID handle = nullptr;
    };

    // Peb32Lite/Peb64Lite：
    // - 只覆盖 PEB 起始字段到 ProcessParameters；
    // - 避免读取 SDK PEB 全结构，降低跨版本字段变化带来的失败概率。
    struct Peb32Lite final
    {
        BYTE reserved1[2]{};
        BYTE beingDebugged = 0;
        BYTE reserved2[1]{};
        std::uint32_t mutant = 0;
        std::uint32_t imageBaseAddress = 0;
        std::uint32_t ldr = 0;
        std::uint32_t processParameters = 0;
    };

    struct Peb64Lite final
    {
        BYTE reserved1[2]{};
        BYTE beingDebugged = 0;
        BYTE reserved2[1]{};
        PVOID mutant = nullptr;
        PVOID imageBaseAddress = nullptr;
        PVOID ldr = nullptr;
        PVOID processParameters = nullptr;
    };

    // RtlUserProcessParameters32Lite/RtlUserProcessParameters64Lite：
    // - 按公开稳定偏移覆盖 CurrentDirectory/ImagePathName/CommandLine/Environment；
    // - 32 位与 64 位的指针和 UNICODE_STRING 大小不同，必须分开定义。
    struct RtlUserProcessParameters32Lite final
    {
        BYTE reservedBeforeCurrentDirectory[0x24]{};
        Curdir32 currentDirectory{};
        RemoteUnicodeString32 dllPath{};
        RemoteUnicodeString32 imagePathName{};
        RemoteUnicodeString32 commandLine{};
        std::uint32_t environment = 0;
    };

    struct RtlUserProcessParameters64Lite final
    {
        BYTE reservedBeforeCurrentDirectory[0x38]{};
        Curdir64 currentDirectory{};
        UNICODE_STRING dllPath{};
        UNICODE_STRING imagePathName{};
        UNICODE_STRING commandLine{};
        PVOID environment = nullptr;
    };

    // RemotePebProcessParametersRead：
    // - 承载一次 PEB->ProcessParameters 解析结果；
    // - readOk 表示参数块结构读取成功，字符串为空不一定代表读取失败。
    struct RemotePebProcessParametersRead final
    {
        QString labelText;                         // Native PEB / Wow64 PEB。
        bool readOk = false;                       // 是否成功读到 ProcessParameters。
        std::uint64_t pebAddress = 0;              // PEB 地址。
        std::uint64_t imageBaseAddress = 0;         // PEB.ImageBaseAddress。
        std::uint64_t processParametersAddress = 0;// ProcessParameters 地址。
        std::uint64_t environmentAddress = 0;      // Environment 地址。
        QString commandLineText;                   // PEB 内命令行。
        QString imagePathText;                     // PEB 内映像路径。
        QString currentDirectoryText;              // PEB 内当前目录。
        QString diagnosticText;                    // 失败/降级原因。
    };

    // queryNtProcessInfoFixed：
    // - 读取“固定大小”的 NtQueryInformationProcess 输出结构；
    // - 成功返回 true，失败返回 false。
    template<typename T>
    bool queryNtProcessInfoFixed(
        const NtQueryInformationProcessFn queryFunction,
        HANDLE processHandle,
        const ULONG infoClass,
        T& outValue)
    {
        if (queryFunction == nullptr || processHandle == nullptr)
        {
            return false;
        }

        std::memset(&outValue, 0, sizeof(T));
        const NTSTATUS status = queryFunction(
            processHandle,
            infoClass,
            &outValue,
            static_cast<ULONG>(sizeof(T)),
            nullptr);
        return NT_SUCCESS(status);
    }

    // queryNtProcessInfoBuffer：
    // - 读取“变长缓冲区”类型的 NtQueryInformationProcess 输出；
    // - 例如 ProcessCommandLineInformation 等场景。
    bool queryNtProcessInfoBuffer(
        const NtQueryInformationProcessFn queryFunction,
        HANDLE processHandle,
        const ULONG infoClass,
        std::vector<std::uint8_t>& bufferOut)
    {
        bufferOut.clear();
        if (queryFunction == nullptr || processHandle == nullptr)
        {
            return false;
        }

        ULONG requiredLength = 0;
        NTSTATUS firstStatus = queryFunction(
            processHandle,
            infoClass,
            nullptr,
            0,
            &requiredLength);

        if (!NT_SUCCESS(firstStatus) && requiredLength == 0)
        {
            return false;
        }

        if (requiredLength < sizeof(UNICODE_STRING))
        {
            requiredLength = static_cast<ULONG>(sizeof(UNICODE_STRING) + 512);
        }

        bufferOut.resize(requiredLength + sizeof(wchar_t), 0);
        NTSTATUS secondStatus = queryFunction(
            processHandle,
            infoClass,
            bufferOut.data(),
            static_cast<ULONG>(bufferOut.size()),
            &requiredLength);
        if (!NT_SUCCESS(secondStatus))
        {
            bufferOut.clear();
            return false;
        }
        return true;
    }

    // queryCommandLineTextByNt：
    // - 通过 ProcessCommandLineInformation 读取命令行；
    // - 同时兼容“返回内嵌字符串”与“返回远程指针”两种实现。
    QString queryCommandLineTextByNt(
        const NtQueryInformationProcessFn queryFunction,
        HANDLE processHandle)
    {
        std::vector<std::uint8_t> commandBuffer;
        if (!queryNtProcessInfoBuffer(
            queryFunction,
            processHandle,
            kProcessInfoClassCommandLine,
            commandBuffer))
        {
            return QString();
        }

        const auto* commandUnicode = reinterpret_cast<const UNICODE_STRING*>(commandBuffer.data());
        if (commandUnicode == nullptr || commandUnicode->Length == 0 || commandUnicode->Buffer == nullptr)
        {
            return QString();
        }

        const std::uintptr_t bufferBegin = reinterpret_cast<std::uintptr_t>(commandBuffer.data());
        const std::uintptr_t bufferSize = commandBuffer.size();
        const std::uintptr_t stringPtr = reinterpret_cast<std::uintptr_t>(commandUnicode->Buffer);
        const std::size_t stringLengthBytes = static_cast<std::size_t>(commandUnicode->Length);
        if ((stringLengthBytes % sizeof(wchar_t)) != 0 || stringLengthBytes > kMaxRemoteUnicodeBytes)
        {
            return QString();
        }

        // 情况A：字符串直接位于返回缓冲区中。
        if (stringPtr >= bufferBegin &&
            stringPtr - bufferBegin <= bufferSize &&
            stringLengthBytes <= bufferSize - (stringPtr - bufferBegin))
        {
            return QString::fromWCharArray(
                reinterpret_cast<const wchar_t*>(stringPtr),
                static_cast<int>(commandUnicode->Length / sizeof(wchar_t)));
        }

        // 情况B：返回的是远程指针，需二次 ReadProcessMemory。
        std::vector<wchar_t> remoteChars(
            static_cast<std::size_t>(commandUnicode->Length / sizeof(wchar_t)) + 1,
            L'\0');
        SIZE_T bytesRead = 0;
        const BOOL readOk = ReadProcessMemory(
            processHandle,
            commandUnicode->Buffer,
            remoteChars.data(),
            commandUnicode->Length,
            &bytesRead);
        if (readOk == FALSE || bytesRead == 0)
        {
            return QString();
        }
        return QString::fromWCharArray(remoteChars.data());
    }

    // appendPebDiagnostic：
    // - 把 PEB 解析阶段的非致命错误追加到诊断文本；
    // - 入参 target 为可变诊断文本，message 为本次追加内容；
    // - 返回：无，调用方继续执行后续降级路径。
    void appendPebDiagnostic(QString& target, const QString& message)
    {
        if (message.trimmed().isEmpty())
        {
            return;
        }

        if (!target.trimmed().isEmpty())
        {
            target += QStringLiteral(" | ");
        }
        target += message;
    }

    // readRemoteMemoryExact：
    // - 从远程进程读取固定长度内存；
    // - 入参 processHandle 是目标进程句柄，remoteAddress 是目标地址；
    // - 入参 localBuffer/localSize 是本地接收缓冲；
    // - 返回 true 表示完整读取 localSize 字节，false 表示地址无效或只读到部分数据。
    bool readRemoteMemoryExact(
        HANDLE processHandle,
        const std::uint64_t remoteAddress,
        void* localBuffer,
        const SIZE_T localSize)
    {
        if (processHandle == nullptr || remoteAddress == 0 || localBuffer == nullptr || localSize == 0)
        {
            return false;
        }

        SIZE_T bytesRead = 0;
        const BOOL readOk = ReadProcessMemory(
            processHandle,
            reinterpret_cast<LPCVOID>(static_cast<std::uintptr_t>(remoteAddress)),
            localBuffer,
            localSize,
            &bytesRead);
        return readOk != FALSE && bytesRead == localSize;
    }

    // readRemoteStructure：
    // - 按模板类型读取一个远程结构体；
    // - 读取前先清零输出，避免失败后遗留旧数据；
    // - 返回 true 表示结构体完整读取，false 表示远程页不可读或长度不足。
    template<typename T>
    bool readRemoteStructure(
        HANDLE processHandle,
        const std::uint64_t remoteAddress,
        T& valueOut)
    {
        std::memset(&valueOut, 0, sizeof(T));
        return readRemoteMemoryExact(
            processHandle,
            remoteAddress,
            &valueOut,
            static_cast<SIZE_T>(sizeof(T)));
    }

    // readRemoteUnicodeStringByAddress：
    // - 按“远程地址 + 字节长度”读取 UTF-16 字符串；
    // - 对长度做偶数校验和上限限制，避免坏 PEB 触发超大分配；
    // - 返回读取到的 QString，失败或空串时返回空 QString。
    QString readRemoteUnicodeStringByAddress(
        HANDLE processHandle,
        const std::uint64_t bufferAddress,
        const USHORT lengthBytes)
    {
        if (processHandle == nullptr || bufferAddress == 0 || lengthBytes == 0)
        {
            return QString();
        }

        if ((lengthBytes % sizeof(wchar_t)) != 0 ||
            static_cast<std::size_t>(lengthBytes) > kMaxRemoteUnicodeBytes)
        {
            return QString();
        }

        std::vector<wchar_t> stringBuffer(
            static_cast<std::size_t>(lengthBytes / sizeof(wchar_t)) + 1,
            L'\0');
        SIZE_T bytesRead = 0;
        const BOOL readOk = ReadProcessMemory(
            processHandle,
            reinterpret_cast<LPCVOID>(static_cast<std::uintptr_t>(bufferAddress)),
            stringBuffer.data(),
            static_cast<SIZE_T>(lengthBytes),
            &bytesRead);
        if (readOk == FALSE || bytesRead < sizeof(wchar_t))
        {
            return QString();
        }

        if (bytesRead > lengthBytes)
        {
            bytesRead = lengthBytes;
        }

        const int charCount = static_cast<int>(bytesRead / sizeof(wchar_t));
        if (charCount <= 0)
        {
            return QString();
        }
        stringBuffer[static_cast<std::size_t>(charCount)] = L'\0';
        return QString::fromWCharArray(stringBuffer.data(), charCount);
    }

    // readRemoteUnicodeString64：
    // - 读取 64 位目标结构中的 UNICODE_STRING；
    // - 输入是当前进程位宽的 UNICODE_STRING 快照；
    // - 返回字符串文本，失败返回空。
    QString readRemoteUnicodeString64(
        HANDLE processHandle,
        const UNICODE_STRING& remoteUnicode)
    {
        return readRemoteUnicodeStringByAddress(
            processHandle,
            reinterpret_cast<std::uint64_t>(remoteUnicode.Buffer),
            remoteUnicode.Length);
    }

    // readRemoteUnicodeString32：
    // - 读取 32 位目标结构中的 UNICODE_STRING；
    // - 输入是手工定义的 32 位布局，Buffer 按 32 位地址提升为 64 位再读取；
    // - 返回字符串文本，失败返回空。
    QString readRemoteUnicodeString32(
        HANDLE processHandle,
        const RemoteUnicodeString32& remoteUnicode)
    {
        return readRemoteUnicodeStringByAddress(
            processHandle,
            static_cast<std::uint64_t>(remoteUnicode.buffer),
            remoteUnicode.length);
    }

    // readRemoteEnvironmentPreviewLines：
    // - 分块读取远程环境变量块，直到双 NUL、读取失败或达到上限；
    // - 只返回前 kMaxEnvironmentPreviewLines 行，避免 UI 文本过大；
    // - diagnosticTextOut 可选输出截断/失败提示；
    // - 返回环境变量预览行列表，读取不到时返回空列表。
    QStringList readRemoteEnvironmentPreviewLines(
        HANDLE processHandle,
        const std::uint64_t environmentAddress,
        QString* diagnosticTextOut)
    {
        if (diagnosticTextOut != nullptr)
        {
            diagnosticTextOut->clear();
        }
        if (processHandle == nullptr || environmentAddress == 0)
        {
            return {};
        }

        std::vector<wchar_t> environmentChars;
        environmentChars.reserve(kEnvironmentReadChunkBytes / sizeof(wchar_t));

        bool foundDoubleNull = false;
        bool readStoppedByError = false;
        std::size_t offsetBytes = 0;
        while (offsetBytes < kMaxEnvironmentPreviewBytes)
        {
            const std::size_t bytesRemaining = kMaxEnvironmentPreviewBytes - offsetBytes;
            const std::size_t requestBytes = std::min<std::size_t>(
                kEnvironmentReadChunkBytes,
                bytesRemaining);
            std::vector<std::uint8_t> chunkBuffer(requestBytes, 0);

            SIZE_T bytesRead = 0;
            const BOOL readOk = ReadProcessMemory(
                processHandle,
                reinterpret_cast<LPCVOID>(
                    static_cast<std::uintptr_t>(environmentAddress + offsetBytes)),
                chunkBuffer.data(),
                static_cast<SIZE_T>(chunkBuffer.size()),
                &bytesRead);
            if (readOk == FALSE || bytesRead < sizeof(wchar_t))
            {
                readStoppedByError = true;
                break;
            }

            const std::size_t charCount = static_cast<std::size_t>(bytesRead / sizeof(wchar_t));
            const std::size_t scanStartIndex = environmentChars.empty()
                ? 1
                : environmentChars.size();
            const auto* chunkChars = reinterpret_cast<const wchar_t*>(chunkBuffer.data());
            environmentChars.insert(environmentChars.end(), chunkChars, chunkChars + charCount);

            for (std::size_t index = scanStartIndex; index < environmentChars.size(); ++index)
            {
                if (environmentChars[index - 1] == L'\0' && environmentChars[index] == L'\0')
                {
                    environmentChars.resize(index);
                    foundDoubleNull = true;
                    break;
                }
            }
            if (foundDoubleNull)
            {
                break;
            }

            const std::size_t consumedBytes = charCount * sizeof(wchar_t);
            if (consumedBytes == 0 || consumedBytes < requestBytes)
            {
                readStoppedByError = true;
                break;
            }
            offsetBytes += consumedBytes;
        }

        QStringList lines;
        std::size_t cursorIndex = 0;
        while (cursorIndex < environmentChars.size() &&
            lines.size() < static_cast<int>(kMaxEnvironmentPreviewLines))
        {
            const wchar_t* currentLine = environmentChars.data() + cursorIndex;
            std::size_t currentLength = 0;
            while (cursorIndex + currentLength < environmentChars.size() &&
                environmentChars[cursorIndex + currentLength] != L'\0')
            {
                ++currentLength;
            }
            if (currentLength == 0)
            {
                break;
            }

            const int visibleLength = static_cast<int>(
                std::min<std::size_t>(currentLength, kMaxEnvironmentLineChars));
            QString lineText = QString::fromWCharArray(currentLine, visibleLength);
            if (currentLength > kMaxEnvironmentLineChars)
            {
                lineText += QStringLiteral(" ...<truncated>");
            }
            lines.push_back(lineText);
            cursorIndex += currentLength + 1;
        }

        if (diagnosticTextOut != nullptr)
        {
            if (!foundDoubleNull && offsetBytes >= kMaxEnvironmentPreviewBytes)
            {
                *diagnosticTextOut = QStringLiteral("环境变量块预览达到128KB上限，已截断。");
            }
            else if (readStoppedByError && lines.empty())
            {
                *diagnosticTextOut = QStringLiteral("环境变量块地址不可完整读取。");
            }
        }

        return lines;
    }

    // readPebProcessParameters64：
    // - 按 64 位布局解析 PEB->RTL_USER_PROCESS_PARAMETERS；
    // - 输出命令行、映像路径、当前目录、环境块地址；
    // - 返回结构体携带 readOk 与诊断文本，不直接抛出异常。
    RemotePebProcessParametersRead readPebProcessParameters64(
        HANDLE processHandle,
        const std::uint64_t pebAddress,
        const QString& labelText)
    {
        RemotePebProcessParametersRead result{};
        result.labelText = labelText;
        result.pebAddress = pebAddress;

        Peb64Lite pebSnapshot{};
        if (!readRemoteStructure(processHandle, pebAddress, pebSnapshot))
        {
            result.diagnosticText = QStringLiteral("读取64位PEB头失败。");
            return result;
        }

        result.processParametersAddress = reinterpret_cast<std::uint64_t>(
            pebSnapshot.processParameters);
        result.imageBaseAddress = reinterpret_cast<std::uint64_t>(
            pebSnapshot.imageBaseAddress);
        if (result.processParametersAddress == 0)
        {
            result.diagnosticText = QStringLiteral("64位PEB.ProcessParameters为空。");
            return result;
        }

        RtlUserProcessParameters64Lite processParameters{};
        if (!readRemoteStructure(
            processHandle,
            result.processParametersAddress,
            processParameters))
        {
            result.diagnosticText = QStringLiteral("读取64位RTL_USER_PROCESS_PARAMETERS失败。");
            return result;
        }

        result.readOk = true;
        result.environmentAddress = reinterpret_cast<std::uint64_t>(
            processParameters.environment);
        result.commandLineText = readRemoteUnicodeString64(
            processHandle,
            processParameters.commandLine);
        result.imagePathText = readRemoteUnicodeString64(
            processHandle,
            processParameters.imagePathName);
        result.currentDirectoryText = readRemoteUnicodeString64(
            processHandle,
            processParameters.currentDirectory.dosPath);
        return result;
    }

    // readPebProcessParameters32：
    // - 按 Wow64 32 位布局解析 PEB->RTL_USER_PROCESS_PARAMETERS；
    // - 解决 64 位程序读取 32 位目标时按 64 位结构偏移解析失败的问题；
    // - 返回结构体携带 readOk 与诊断文本，不直接抛出异常。
    RemotePebProcessParametersRead readPebProcessParameters32(
        HANDLE processHandle,
        const std::uint64_t pebAddress,
        const QString& labelText)
    {
        RemotePebProcessParametersRead result{};
        result.labelText = labelText;
        result.pebAddress = pebAddress;

        Peb32Lite pebSnapshot{};
        if (!readRemoteStructure(processHandle, pebAddress, pebSnapshot))
        {
            result.diagnosticText = QStringLiteral("读取32位PEB头失败。");
            return result;
        }

        result.processParametersAddress = static_cast<std::uint64_t>(
            pebSnapshot.processParameters);
        result.imageBaseAddress = static_cast<std::uint64_t>(
            pebSnapshot.imageBaseAddress);
        if (result.processParametersAddress == 0)
        {
            result.diagnosticText = QStringLiteral("32位PEB.ProcessParameters为空。");
            return result;
        }

        RtlUserProcessParameters32Lite processParameters{};
        if (!readRemoteStructure(
            processHandle,
            result.processParametersAddress,
            processParameters))
        {
            result.diagnosticText = QStringLiteral("读取32位RTL_USER_PROCESS_PARAMETERS失败。");
            return result;
        }

        result.readOk = true;
        result.environmentAddress = static_cast<std::uint64_t>(
            processParameters.environment);
        result.commandLineText = readRemoteUnicodeString32(
            processHandle,
            processParameters.commandLine);
        result.imagePathText = readRemoteUnicodeString32(
            processHandle,
            processParameters.imagePathName);
        result.currentDirectoryText = readRemoteUnicodeString32(
            processHandle,
            processParameters.currentDirectory.dosPath);
        return result;
    }

    // memoryStateToText：
    // - 内存区域 State 字段文本化。
    QString memoryStateToText(const DWORD stateValue)
    {
        switch (stateValue)
        {
        case MEM_COMMIT: return QStringLiteral("Commit");
        case MEM_RESERVE: return QStringLiteral("Reserve");
        case MEM_FREE: return QStringLiteral("Free");
        default: return QString("State(0x%1)").arg(stateValue, 0, 16).toUpper();
        }
    }

    // memoryTypeToText：
    // - 内存区域 Type 字段文本化。
    QString memoryTypeToText(const DWORD typeValue)
    {
        switch (typeValue)
        {
        case MEM_IMAGE: return QStringLiteral("Image");
        case MEM_MAPPED: return QStringLiteral("Mapped");
        case MEM_PRIVATE: return QStringLiteral("Private");
        default: return QString("Type(0x%1)").arg(typeValue, 0, 16).toUpper();
        }
    }

    // memoryProtectToText：
    // - 内存区域保护属性文本化，便于列表审计。
    QString memoryProtectToText(const DWORD protectValue)
    {
        if (protectValue == 0)
        {
            return QStringLiteral("-");
        }

        QStringList flags;
        const DWORD baseProtect = protectValue & 0xFF;
        switch (baseProtect)
        {
        case PAGE_NOACCESS: flags << QStringLiteral("NOACCESS"); break;
        case PAGE_READONLY: flags << QStringLiteral("R"); break;
        case PAGE_READWRITE: flags << QStringLiteral("RW"); break;
        case PAGE_WRITECOPY: flags << QStringLiteral("WC"); break;
        case PAGE_EXECUTE: flags << QStringLiteral("X"); break;
        case PAGE_EXECUTE_READ: flags << QStringLiteral("XR"); break;
        case PAGE_EXECUTE_READWRITE: flags << QStringLiteral("XRW"); break;
        case PAGE_EXECUTE_WRITECOPY: flags << QStringLiteral("XWC"); break;
        default: flags << QString("0x%1").arg(baseProtect, 0, 16).toUpper(); break;
        }

        if ((protectValue & PAGE_GUARD) != 0) flags << QStringLiteral("GUARD");
        if ((protectValue & PAGE_NOCACHE) != 0) flags << QStringLiteral("NOCACHE");
        if ((protectValue & PAGE_WRITECOMBINE) != 0) flags << QStringLiteral("WRITECOMBINE");
        return flags.join('|');
    }

    // protectionLevelToText：
    // - ProcessProtectionInformation 的单字节级别文本化。
    QString protectionLevelToText(const std::uint8_t protectionLevel)
    {
        if (protectionLevel == 0)
        {
            return QStringLiteral("None");
        }

        const std::uint8_t signer = protectionLevel >> 4;
        const std::uint8_t type = protectionLevel & 0x07;
        return QStringLiteral("Level=0x%1 (Signer=%2, Type=%3)")
            .arg(protectionLevel, 2, 16, QChar('0'))
            .arg(signer)
            .arg(type);
    }

    // countTopLevelWindowsByPid：
    // - 枚举所有顶层窗口并统计目标 PID 持有数量。
    std::uint32_t countTopLevelWindowsByPid(const std::uint32_t pid)
    {
        struct EnumContext final
        {
            std::uint32_t targetPid = 0;   // 目标进程 PID。
            std::uint32_t windowCount = 0; // 统计结果。
        };

        EnumContext context{};
        context.targetPid = pid;

        EnumWindows(
            [](HWND hwnd, LPARAM param) -> BOOL
            {
                auto* ctx = reinterpret_cast<EnumContext*>(param);
                if (ctx == nullptr)
                {
                    return FALSE;
                }

                DWORD ownerPid = 0;
                GetWindowThreadProcessId(hwnd, &ownerPid);
                if (ownerPid == ctx->targetPid)
                {
                    ++ctx->windowCount;
                }
                return TRUE;
            },
            reinterpret_cast<LPARAM>(&context));

        return context.windowCount;
    }

    // queryDesktopNameByProcessThreads：
    // - 通过目标进程线程 ID 获取其桌面对象名称（可用时）。
    QString queryDesktopNameByProcessThreads(const std::uint32_t pid)
    {
        HANDLE snapshotHandle = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
        if (snapshotHandle == INVALID_HANDLE_VALUE)
        {
            return QString();
        }

        QString desktopName;
        THREADENTRY32 threadEntry{};
        threadEntry.dwSize = sizeof(threadEntry);
        BOOL hasThread = Thread32First(snapshotHandle, &threadEntry);
        while (hasThread != FALSE)
        {
            if (threadEntry.th32OwnerProcessID == pid)
            {
                HDESK desktopHandle = GetThreadDesktop(threadEntry.th32ThreadID);
                if (desktopHandle != nullptr)
                {
                    wchar_t desktopBuffer[256] = {};
                    DWORD bytesNeeded = 0;
                    const BOOL queryOk = GetUserObjectInformationW(
                        desktopHandle,
                        UOI_NAME,
                        desktopBuffer,
                        static_cast<DWORD>(sizeof(desktopBuffer)),
                        &bytesNeeded);
                    if (queryOk != FALSE)
                    {
                        desktopName = QString::fromWCharArray(desktopBuffer);
                    }
                }
                break;
            }
            hasThread = Thread32Next(snapshotHandle, &threadEntry);
        }

        CloseHandle(snapshotHandle);
        return desktopName;
    }

    // ThreadBasicInformationNative：
    // - 对应线程基础信息结构；
    // - 只保留当前页面渲染需要的字段。
    struct ThreadBasicInformationNative
    {
        NTSTATUS exitStatus = 0;           // 线程退出状态。
        PVOID tebBaseAddress = nullptr;    // TEB 地址。
        CLIENT_ID clientId{};              // 客户端 ID。
        ULONG_PTR affinityMask = 0;        // 亲和性掩码。
        LONG priority = 0;                 // 当前优先级。
        LONG basePriority = 0;             // 基础优先级。
    };

    // queryTokenInfoBuffer：
    // - 统一读取令牌信息；
    // - 成功时把字节内容写入 bufferOut。
    bool queryTokenInfoBuffer(
        HANDLE tokenHandle,
        TOKEN_INFORMATION_CLASS infoClass,
        std::vector<std::uint8_t>& bufferOut)
    {
        bufferOut.clear();
        DWORD requiredLength = 0;
        GetTokenInformation(tokenHandle, infoClass, nullptr, 0, &requiredLength);
        if (requiredLength == 0)
        {
            return false;
        }

        bufferOut.resize(requiredLength);
        const BOOL queryOk = GetTokenInformation(
            tokenHandle,
            infoClass,
            bufferOut.data(),
            requiredLength,
            &requiredLength);
        return queryOk != FALSE;
    }

    // formatNtStatusHex：
    // - 把 NTSTATUS 转成 0xXXXXXXXX 十六进制文本；
    // - 供令牌开关应用失败时拼接可审计的错误细节。
    QString formatNtStatusHex(const NTSTATUS statusCode)
    {
        return QStringLiteral("0x%1")
            .arg(static_cast<unsigned long>(statusCode), 8, 16, QChar('0'))
            .toUpper();
    }

    // queryTokenBoolFlag：
    // - 读取“ULONG 布尔位”类型令牌字段；
    // - 成功时输出 bool 值，失败返回 false。
    bool queryTokenBoolFlag(
        HANDLE tokenHandle,
        const TOKEN_INFORMATION_CLASS infoClass,
        bool& valueOut)
    {
        ULONG rawValue = 0;
        DWORD returnLength = 0;
        const BOOL queryOk = GetTokenInformation(
            tokenHandle,
            infoClass,
            &rawValue,
            static_cast<DWORD>(sizeof(rawValue)),
            &returnLength);
        if (queryOk == FALSE || returnLength < sizeof(rawValue))
        {
            return false;
        }
        valueOut = (rawValue != 0);
        return true;
    }

    // queryTokenMandatoryPolicyBits：
    // - 读取 TokenMandatoryPolicy 并拆分两个复选框位；
    // - 输出 NoWriteUp / NewProcessMin 两个布尔值。
    bool queryTokenMandatoryPolicyBits(
        HANDLE tokenHandle,
        bool& noWriteUpOut,
        bool& newProcessMinOut)
    {
        TOKEN_MANDATORY_POLICY mandatoryPolicy{};
        DWORD returnLength = 0;
        const BOOL queryOk = GetTokenInformation(
            tokenHandle,
            TokenMandatoryPolicy,
            &mandatoryPolicy,
            static_cast<DWORD>(sizeof(mandatoryPolicy)),
            &returnLength);
        if (queryOk == FALSE || returnLength < sizeof(mandatoryPolicy))
        {
            return false;
        }
        noWriteUpOut = (mandatoryPolicy.Policy & kTokenMandatoryPolicyNoWriteUp) != 0;
        newProcessMinOut = (mandatoryPolicy.Policy & kTokenMandatoryPolicyNewProcessMin) != 0;
        return true;
    }

    // applyTokenBoolFlag：
    // - 使用 NtSetInformationToken 写入 ULONG 布尔位；
    // - 返回 NTSTATUS 以便调用者记录逐项失败原因。
    NTSTATUS applyTokenBoolFlag(
        const NtSetInformationTokenFn setInformationToken,
        HANDLE tokenHandle,
        const TOKEN_INFORMATION_CLASS infoClass,
        const bool enabled)
    {
        if (setInformationToken == nullptr || tokenHandle == nullptr)
        {
            return static_cast<NTSTATUS>(0xC000000DL);
        }
        ULONG rawValue = enabled ? 1UL : 0UL;
        return setInformationToken(
            tokenHandle,
            infoClass,
            &rawValue,
            static_cast<ULONG>(sizeof(rawValue)));
    }

    // applyTokenMandatoryPolicyBits：
    // - 把两个策略复选框合成为 TOKEN_MANDATORY_POLICY；
    // - 使用 NtSetInformationToken 一次性写入策略位。
    NTSTATUS applyTokenMandatoryPolicyBits(
        const NtSetInformationTokenFn setInformationToken,
        HANDLE tokenHandle,
        const bool noWriteUpEnabled,
        const bool newProcessMinEnabled)
    {
        if (setInformationToken == nullptr || tokenHandle == nullptr)
        {
            return static_cast<NTSTATUS>(0xC000000DL);
        }

        TOKEN_MANDATORY_POLICY mandatoryPolicy{};
        mandatoryPolicy.Policy = 0;
        if (noWriteUpEnabled)
        {
            mandatoryPolicy.Policy |= kTokenMandatoryPolicyNoWriteUp;
        }
        if (newProcessMinEnabled)
        {
            mandatoryPolicy.Policy |= kTokenMandatoryPolicyNewProcessMin;
        }
        return setInformationToken(
            tokenHandle,
            TokenMandatoryPolicy,
            &mandatoryPolicy,
            static_cast<ULONG>(sizeof(mandatoryPolicy)));
    }

    // describeIntegrityLevel：
    // - 完整性 RID 转文本。
    QString describeIntegrityLevel(const DWORD integrityRid)
    {
        switch (integrityRid)
        {
        case SECURITY_MANDATORY_UNTRUSTED_RID: return QStringLiteral("Untrusted");
        case SECURITY_MANDATORY_LOW_RID: return QStringLiteral("Low");
        case SECURITY_MANDATORY_MEDIUM_RID: return QStringLiteral("Medium");
        case SECURITY_MANDATORY_HIGH_RID: return QStringLiteral("High");
        case SECURITY_MANDATORY_SYSTEM_RID: return QStringLiteral("System");
        case SECURITY_MANDATORY_PROTECTED_PROCESS_RID: return QStringLiteral("ProtectedProcess");
        default: return QString("RID=%1").arg(integrityRid);
        }
    }

    // describePriorityClass：
    // - 优先级类常量转文本。
    QString describePriorityClass(const DWORD priorityClass)
    {
        switch (priorityClass)
        {
        case IDLE_PRIORITY_CLASS: return QStringLiteral("IDLE");
        case BELOW_NORMAL_PRIORITY_CLASS: return QStringLiteral("BELOW_NORMAL");
        case NORMAL_PRIORITY_CLASS: return QStringLiteral("NORMAL");
        case ABOVE_NORMAL_PRIORITY_CLASS: return QStringLiteral("ABOVE_NORMAL");
        case HIGH_PRIORITY_CLASS: return QStringLiteral("HIGH");
        case REALTIME_PRIORITY_CLASS: return QStringLiteral("REALTIME");
        default: return QString("UNKNOWN(%1)").arg(priorityClass);
        }
    }

    // tokenInfoClassNameById：
    // - 把 TokenInformationClass 数值映射到名称；
    // - 未知编号统一显示 TokenClassN，避免信息丢失。
    QString tokenInfoClassNameById(const ULONG classId)
    {
        switch (classId)
        {
        case 1: return QStringLiteral("TokenUser");
        case 2: return QStringLiteral("TokenGroups");
        case 3: return QStringLiteral("TokenPrivileges");
        case 4: return QStringLiteral("TokenOwner");
        case 5: return QStringLiteral("TokenPrimaryGroup");
        case 6: return QStringLiteral("TokenDefaultDacl");
        case 7: return QStringLiteral("TokenSource");
        case 8: return QStringLiteral("TokenType");
        case 9: return QStringLiteral("TokenImpersonationLevel");
        case 10: return QStringLiteral("TokenStatistics");
        case 11: return QStringLiteral("TokenRestrictedSids");
        case 12: return QStringLiteral("TokenSessionId");
        case 13: return QStringLiteral("TokenGroupsAndPrivileges");
        case 14: return QStringLiteral("TokenSessionReference");
        case 15: return QStringLiteral("TokenSandBoxInert");
        case 16: return QStringLiteral("TokenAuditPolicy");
        case 17: return QStringLiteral("TokenOrigin");
        case 18: return QStringLiteral("TokenElevationType");
        case 19: return QStringLiteral("TokenLinkedToken");
        case 20: return QStringLiteral("TokenElevation");
        case 21: return QStringLiteral("TokenHasRestrictions");
        case 22: return QStringLiteral("TokenAccessInformation");
        case 23: return QStringLiteral("TokenVirtualizationAllowed");
        case 24: return QStringLiteral("TokenVirtualizationEnabled");
        case 25: return QStringLiteral("TokenIntegrityLevel");
        case 26: return QStringLiteral("TokenUIAccess");
        case 27: return QStringLiteral("TokenMandatoryPolicy");
        case 28: return QStringLiteral("TokenLogonSid");
        case 29: return QStringLiteral("TokenIsAppContainer");
        case 30: return QStringLiteral("TokenCapabilities");
        case 31: return QStringLiteral("TokenAppContainerSid");
        case 32: return QStringLiteral("TokenAppContainerNumber");
        case 33: return QStringLiteral("TokenUserClaimAttributes");
        case 34: return QStringLiteral("TokenDeviceClaimAttributes");
        case 35: return QStringLiteral("TokenRestrictedUserClaimAttributes");
        case 36: return QStringLiteral("TokenRestrictedDeviceClaimAttributes");
        case 37: return QStringLiteral("TokenDeviceGroups");
        case 38: return QStringLiteral("TokenRestrictedDeviceGroups");
        case 39: return QStringLiteral("TokenSecurityAttributes");
        case 40: return QStringLiteral("TokenIsRestricted");
        case 41: return QStringLiteral("TokenProcessTrustLevel");
        case 42: return QStringLiteral("TokenPrivateNameSpace");
        case 43: return QStringLiteral("TokenSingletonAttributes");
        case 44: return QStringLiteral("TokenBnoIsolation");
        case 45: return QStringLiteral("TokenChildProcessFlags");
        case 46: return QStringLiteral("TokenIsLessPrivilegedAppContainer");
        case 47: return QStringLiteral("TokenIsSandboxed");
        case 48: return QStringLiteral("TokenOriginatingProcessTrustLevel");
        case 49: return QStringLiteral("TokenLoggingInformation");
        case 50: return QStringLiteral("TokenLearningMode");
        case 51: return QStringLiteral("TokenIsAppSilo");
        default: return QStringLiteral("TokenClass%1").arg(classId);
        }
    }

    // formatTokenRawPreview：
    // - 把原始字节缓冲区前 N 字节格式化为十六进制预览；
    // - 输出形如 "01 00 FF ..."，便于快速比对当前值。
    QString formatTokenRawPreview(const std::vector<std::uint8_t>& rawBuffer, const std::size_t maxBytes)
    {
        if (rawBuffer.empty() || maxBytes == 0)
        {
            return QStringLiteral("-");
        }

        QStringList byteTextList;
        const std::size_t previewCount = std::min<std::size_t>(rawBuffer.size(), maxBytes);
        byteTextList.reserve(static_cast<int>(previewCount) + 1);
        for (std::size_t index = 0; index < previewCount; ++index)
        {
            byteTextList << QStringLiteral("%1")
                .arg(static_cast<unsigned int>(rawBuffer[index]), 2, 16, QChar('0'))
                .toUpper();
        }
        if (rawBuffer.size() > previewCount)
        {
            byteTextList << QStringLiteral("...");
        }
        return byteTextList.join(QStringLiteral(" "));
    }
}

void ProcessDetailWindow::updateThreadInspectStatusLabel(const QString& statusText, const bool refreshing)
{
    // 状态标签刷新：
    // - refreshing=true 显示蓝色进行中；
    // - false 显示灰色完成态。
    if (m_threadInspectStatusLabel == nullptr)
    {
        return;
    }

    m_threadInspectStatusLabel->setText(statusText);
    m_threadInspectStatusLabel->setStyleSheet(
        refreshing
        ? buildStateLabelStyle(KswordTheme::PrimaryBlueColor, 700)
        : buildStateLabelStyle(statusSecondaryColor(), 600));
}

void ProcessDetailWindow::requestAsyncThreadInspectRefresh()
{
    // 防重入与非法 PID 保护。
    if (m_threadInspectRefreshing || m_baseRecord.pid == 0)
    {
        return;
    }

    m_threadInspectRefreshing = true;
    const std::uint64_t ticketValue = ++m_threadInspectRefreshTicket;
    updateThreadInspectStatusLabel(QStringLiteral("● 正在刷新线程细节..."), true);
    if (m_refreshThreadInspectButton != nullptr)
    {
        m_refreshThreadInspectButton->setEnabled(false);
    }

    if (m_threadInspectRefreshProgressPid == 0)
    {
        m_threadInspectRefreshProgressPid = kPro.add("进程详情", "刷新线程细节");
    }
    kPro.set(m_threadInspectRefreshProgressPid, "扫描线程信息", 0, 20.0f);

    const std::uint32_t pidValue = m_baseRecord.pid;
    QPointer<ProcessDetailWindow> guardThis(this);
    auto* refreshTask = QRunnable::create([guardThis, pidValue, ticketValue]()
        {
            ThreadInspectRefreshResult refreshResult{};
            const auto beginTime = std::chrono::steady_clock::now();

            HANDLE snapshotHandle = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
            if (snapshotHandle == INVALID_HANDLE_VALUE)
            {
                refreshResult.diagnosticText = QString("CreateToolhelp32Snapshot失败(%1)").arg(GetLastError());
            }
            else
            {
                HMODULE ntdllModule = GetModuleHandleW(L"ntdll.dll");
                NtQueryInformationThreadFn ntQueryThread = nullptr;
                if (ntdllModule != nullptr)
                {
                    ntQueryThread = reinterpret_cast<NtQueryInformationThreadFn>(
                        GetProcAddress(ntdllModule, "NtQueryInformationThread"));
                }

                THREADENTRY32 threadEntry{};
                threadEntry.dwSize = sizeof(threadEntry);
                BOOL hasThread = Thread32First(snapshotHandle, &threadEntry);
                while (hasThread != FALSE)
                {
                    if (threadEntry.th32OwnerProcessID == pidValue)
                    {
                        ThreadInspectItem rowItem{};
                        rowItem.threadId = threadEntry.th32ThreadID;
                        rowItem.stateText = QStringLiteral("Unknown");
                        rowItem.priorityValue = 0;
                        rowItem.switchCount = 0;
                        rowItem.startAddressText = QStringLiteral("-");
                        rowItem.tebAddressText = QStringLiteral("-");
                        rowItem.affinityText = QStringLiteral("-");
                        rowItem.registerSummaryText = QStringLiteral("-");

                        HANDLE threadHandle = OpenThread(
                            THREAD_QUERY_INFORMATION | THREAD_GET_CONTEXT | THREAD_SUSPEND_RESUME,
                            FALSE,
                            threadEntry.th32ThreadID);
                        if (threadHandle != nullptr)
                        {
                            rowItem.priorityValue = GetThreadPriority(threadHandle);
                            if (ntQueryThread != nullptr)
                            {
                                ThreadBasicInformationNative basicInfo{};
                                NTSTATUS basicStatus = ntQueryThread(
                                    threadHandle,
                                    0,
                                    &basicInfo,
                                    static_cast<ULONG>(sizeof(basicInfo)),
                                    nullptr);
                                if (NT_SUCCESS(basicStatus))
                                {
                                    rowItem.tebAddressText = uint64ToHex(
                                        reinterpret_cast<std::uint64_t>(basicInfo.tebBaseAddress));
                                    rowItem.affinityText = uint64ToHex(basicInfo.affinityMask);
                                    rowItem.stateText = (basicInfo.exitStatus == STATUS_PENDING)
                                        ? QStringLiteral("Running")
                                        : QStringLiteral("Exited");
                                }

                                PVOID startAddress = nullptr;
                                NTSTATUS startStatus = ntQueryThread(
                                    threadHandle,
                                    9,
                                    &startAddress,
                                    static_cast<ULONG>(sizeof(startAddress)),
                                    nullptr);
                                if (NT_SUCCESS(startStatus))
                                {
                                    rowItem.startAddressText = uint64ToHex(
                                        reinterpret_cast<std::uint64_t>(startAddress));
                                }
                            }

                            CONTEXT threadContext{};
                            threadContext.ContextFlags = CONTEXT_CONTROL | CONTEXT_INTEGER;
                            const DWORD suspendCount = SuspendThread(threadHandle);
                            if (suspendCount != static_cast<DWORD>(-1))
                            {
                                const BOOL contextOk = GetThreadContext(threadHandle, &threadContext);
                                ResumeThread(threadHandle);
                                if (contextOk != FALSE)
                                {
#if defined(_M_X64)
                                    rowItem.registerSummaryText = QString("RIP=%1 RSP=%2")
                                        .arg(uint64ToHex(threadContext.Rip))
                                        .arg(uint64ToHex(threadContext.Rsp));
#elif defined(_M_IX86)
                                    rowItem.registerSummaryText = QString("EIP=%1 ESP=%2")
                                        .arg(uint64ToHex(threadContext.Eip))
                                        .arg(uint64ToHex(threadContext.Esp));
#else
                                    rowItem.registerSummaryText = QStringLiteral("当前架构未实现");
#endif
                                }
                            }
                            CloseHandle(threadHandle);
                        }
                        else
                        {
                            rowItem.stateText = QStringLiteral("AccessDenied");
                        }

                        refreshResult.rows.push_back(std::move(rowItem));
                    }

                    hasThread = Thread32Next(snapshotHandle, &threadEntry);
                }
                CloseHandle(snapshotHandle);
            }

            std::sort(
                refreshResult.rows.begin(),
                refreshResult.rows.end(),
                [](const ThreadInspectItem& left, const ThreadInspectItem& right)
                {
                    return left.threadId < right.threadId;
                });

            refreshResult.elapsedMs = static_cast<std::uint64_t>(
                std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::steady_clock::now() - beginTime).count());

            QMetaObject::invokeMethod(
                guardThis,
                [guardThis, refreshResult, ticketValue]()
                {
                    if (guardThis == nullptr || guardThis->m_threadInspectRefreshTicket != ticketValue)
                    {
                        return;
                    }
                    guardThis->applyThreadInspectResult(refreshResult);
                },
                Qt::QueuedConnection);
        });
    QThreadPool::globalInstance()->start(refreshTask);
}

void ProcessDetailWindow::applyThreadInspectResult(const ThreadInspectRefreshResult& refreshResult)
{
    m_threadInspectRefreshing = false;
    if (m_refreshThreadInspectButton != nullptr)
    {
        m_refreshThreadInspectButton->setEnabled(true);
    }

    if (m_threadInspectTable != nullptr)
    {
        m_threadInspectTable->setSortingEnabled(false);
        m_threadInspectTable->setRowCount(0);
        for (const ThreadInspectItem& rowItem : refreshResult.rows)
        {
            const int row = m_threadInspectTable->rowCount();
            m_threadInspectTable->insertRow(row);
            m_threadInspectTable->setItem(row, toThreadColumnIndex(ThreadRowColumn::ThreadId), new QTableWidgetItem(QString::number(rowItem.threadId)));
            m_threadInspectTable->setItem(row, toThreadColumnIndex(ThreadRowColumn::State), new QTableWidgetItem(rowItem.stateText));
            m_threadInspectTable->setItem(row, toThreadColumnIndex(ThreadRowColumn::Priority), new QTableWidgetItem(QString::number(rowItem.priorityValue)));
            m_threadInspectTable->setItem(row, toThreadColumnIndex(ThreadRowColumn::SwitchCount), new QTableWidgetItem(QString::number(rowItem.switchCount)));
            m_threadInspectTable->setItem(row, toThreadColumnIndex(ThreadRowColumn::StartAddress), new QTableWidgetItem(rowItem.startAddressText));
            m_threadInspectTable->setItem(row, toThreadColumnIndex(ThreadRowColumn::TebAddress), new QTableWidgetItem(rowItem.tebAddressText));
            m_threadInspectTable->setItem(row, toThreadColumnIndex(ThreadRowColumn::Affinity), new QTableWidgetItem(rowItem.affinityText));
            m_threadInspectTable->setItem(row, toThreadColumnIndex(ThreadRowColumn::RegisterSummary), new QTableWidgetItem(rowItem.registerSummaryText));
        }
        m_threadInspectTable->setSortingEnabled(true);
    }

    QString statusText = QString("● 刷新完成 %1 ms | 线程数 %2")
        .arg(refreshResult.elapsedMs)
        .arg(refreshResult.rows.size());
    if (!refreshResult.diagnosticText.trimmed().isEmpty())
    {
        statusText += QString(" | %1").arg(refreshResult.diagnosticText);
    }
    updateThreadInspectStatusLabel(statusText, false);
    kPro.set(m_threadInspectRefreshProgressPid, "线程细节刷新完成", 0, 100.0f);
}

void ProcessDetailWindow::requestAsyncTokenRefresh()
{
    // 令牌页异步刷新：
    // - 解析用户 SID、完整性级别、组和特权；
    // - 全流程在后台线程执行，避免阻塞窗口交互。
    if (m_tokenRefreshing || m_baseRecord.pid == 0)
    {
        return;
    }

    m_tokenRefreshing = true;
    const std::uint64_t ticketValue = ++m_tokenRefreshTicket;
    if (m_refreshTokenButton != nullptr)
    {
        m_refreshTokenButton->setEnabled(false);
    }
    if (m_tokenStatusLabel != nullptr)
    {
        m_tokenStatusLabel->setText(QStringLiteral("● 正在刷新令牌..."));
        m_tokenStatusLabel->setStyleSheet(buildStateLabelStyle(KswordTheme::PrimaryBlueColor, 700));
    }

    if (m_tokenRefreshProgressPid == 0)
    {
        m_tokenRefreshProgressPid = kPro.add("进程详情", "刷新令牌信息");
    }
    kPro.set(m_tokenRefreshProgressPid, "读取令牌字段", 0, 20.0f);

    const std::uint32_t pidValue = m_baseRecord.pid;
    QPointer<ProcessDetailWindow> guardThis(this);
    auto* refreshTask = QRunnable::create([guardThis, pidValue, ticketValue]()
        {
            TextRefreshResult refreshResult{};
            const auto beginTime = std::chrono::steady_clock::now();
            std::wostringstream textBuilder;
            textBuilder << L"[Token / Security Information]\n";
            textBuilder << L"PID: " << pidValue << L"\n";

            // 打开进程句柄：
            // - 令牌信息需要 PROCESS_QUERY_LIMITED_INFORMATION；
            // - 安全扩展字段与虚拟内存摘要需要 PROCESS_QUERY_INFORMATION / VM_READ。
            HANDLE processHandle = OpenProcess(
                PROCESS_QUERY_INFORMATION | PROCESS_QUERY_LIMITED_INFORMATION | PROCESS_VM_READ,
                FALSE,
                pidValue);
            if (processHandle == nullptr)
            {
                processHandle = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pidValue);
            }
            if (processHandle == nullptr)
            {
                refreshResult.diagnosticText = QString("OpenProcess失败(%1)").arg(GetLastError());
            }
            else
            {
                // 动态加载 NtQueryInformationProcess：
                // - 用于读取 DebugPort/保护级别/DEP/关键进程等原生字段。
                HMODULE ntdllModule = GetModuleHandleW(L"ntdll.dll");
                NtQueryInformationProcessFn ntQueryProcess = reinterpret_cast<NtQueryInformationProcessFn>(
                    ntdllModule != nullptr ? GetProcAddress(ntdllModule, "NtQueryInformationProcess") : nullptr);

                HANDLE tokenHandle = nullptr;
                if (OpenProcessToken(processHandle, TOKEN_QUERY, &tokenHandle) == FALSE)
                {
                    refreshResult.diagnosticText = QString("OpenProcessToken失败(%1)").arg(GetLastError());
                }
                else
                {
                    std::vector<std::uint8_t> tokenBuffer;

                    // TokenUser：输出用户 SID 与账户名。
                    if (queryTokenInfoBuffer(tokenHandle, TokenUser, tokenBuffer))
                    {
                        const auto* tokenUserInfo = reinterpret_cast<const TOKEN_USER*>(tokenBuffer.data());
                        textBuilder << L"User: " << convertSidToText(tokenUserInfo->User.Sid).toStdWString() << L"\n";
                    }

                    // TokenIntegrityLevel：输出完整性级别。
                    if (queryTokenInfoBuffer(tokenHandle, TokenIntegrityLevel, tokenBuffer))
                    {
                        const auto* mandatoryLabel = reinterpret_cast<const TOKEN_MANDATORY_LABEL*>(tokenBuffer.data());
                        DWORD integrityRid = 0;
                        if (mandatoryLabel->Label.Sid != nullptr &&
                            *GetSidSubAuthorityCount(mandatoryLabel->Label.Sid) > 0)
                        {
                            integrityRid = *GetSidSubAuthority(
                                mandatoryLabel->Label.Sid,
                                *GetSidSubAuthorityCount(mandatoryLabel->Label.Sid) - 1);
                        }
                        textBuilder << L"Integrity: " << describeIntegrityLevel(integrityRid).toStdWString() << L"\n";
                    }

                    // TokenElevationType：输出提升类型（Default/Full/Limited）。
                    TOKEN_ELEVATION_TYPE elevationType = TokenElevationTypeDefault;
                    if (queryTokenInfoBuffer(tokenHandle, TokenElevationType, tokenBuffer) &&
                        tokenBuffer.size() >= sizeof(TOKEN_ELEVATION_TYPE))
                    {
                        elevationType = *reinterpret_cast<const TOKEN_ELEVATION_TYPE*>(tokenBuffer.data());
                    }
                    const wchar_t* elevationTypeText = L"Default";
                    if (elevationType == TokenElevationTypeFull)
                    {
                        elevationTypeText = L"Full";
                    }
                    else if (elevationType == TokenElevationTypeLimited)
                    {
                        elevationTypeText = L"Limited";
                    }
                    textBuilder << L"ElevationType: " << elevationTypeText << L"\n";

                    // TokenElevation：输出是否已提升。
                    if (queryTokenInfoBuffer(tokenHandle, TokenElevation, tokenBuffer) &&
                        tokenBuffer.size() >= sizeof(TOKEN_ELEVATION))
                    {
                        const auto* elevation = reinterpret_cast<const TOKEN_ELEVATION*>(tokenBuffer.data());
                        textBuilder << L"IsElevated: " << (elevation->TokenIsElevated != 0 ? L"true" : L"false") << L"\n";
                    }

                    // TokenLinkedToken：输出链接令牌是否存在（管理员拆分令牌场景）。
                    if (queryTokenInfoBuffer(tokenHandle, TokenLinkedToken, tokenBuffer) &&
                        tokenBuffer.size() >= sizeof(TOKEN_LINKED_TOKEN))
                    {
                        const auto* linkedTokenInfo = reinterpret_cast<const TOKEN_LINKED_TOKEN*>(tokenBuffer.data());
                        const bool hasLinkedToken =
                            linkedTokenInfo->LinkedToken != nullptr &&
                            linkedTokenInfo->LinkedToken != INVALID_HANDLE_VALUE;
                        textBuilder << L"LinkedToken: " << (hasLinkedToken ? L"Present" : L"None") << L"\n";
                        if (hasLinkedToken)
                        {
                            CloseHandle(linkedTokenInfo->LinkedToken);
                        }
                    }
                    else
                    {
                        textBuilder << L"LinkedToken: Unavailable\n";
                    }

                    // TokenRestrictedSids：输出限制 SID 数量。
                    if (queryTokenInfoBuffer(tokenHandle, TokenRestrictedSids, tokenBuffer))
                    {
                        const auto* restrictedSids = reinterpret_cast<const TOKEN_GROUPS*>(tokenBuffer.data());
                        textBuilder << L"RestrictedSidCount: " << restrictedSids->GroupCount << L"\n";
                    }
                    else
                    {
                        textBuilder << L"RestrictedSidCount: 0\n";
                    }

                    // TokenGroups：输出组数量与预览。
                    if (queryTokenInfoBuffer(tokenHandle, TokenGroups, tokenBuffer))
                    {
                        const auto* groupsInfo = reinterpret_cast<const TOKEN_GROUPS*>(tokenBuffer.data());
                        textBuilder << L"GroupCount: " << groupsInfo->GroupCount << L"\n";
                        const DWORD previewCount = std::min<DWORD>(groupsInfo->GroupCount, 16);
                        for (DWORD index = 0; index < previewCount; ++index)
                        {
                            textBuilder << L"  - "
                                << convertSidToText(groupsInfo->Groups[index].Sid).toStdWString()
                                << L"\n";
                        }
                    }

                    // TokenPrivileges：输出特权数量与启用状态。
                    if (queryTokenInfoBuffer(tokenHandle, TokenPrivileges, tokenBuffer))
                    {
                        const auto* privilegesInfo =
                            reinterpret_cast<const TOKEN_PRIVILEGES*>(tokenBuffer.data());
                        textBuilder << L"PrivilegeCount: " << privilegesInfo->PrivilegeCount << L"\n";
                        const DWORD previewCount = std::min<DWORD>(privilegesInfo->PrivilegeCount, 24);
                        for (DWORD index = 0; index < previewCount; ++index)
                        {
                            const LUID_AND_ATTRIBUTES& privilegeEntry = privilegesInfo->Privileges[index];
                            WCHAR privilegeName[256] = {};
                            DWORD nameLength = static_cast<DWORD>(std::size(privilegeName));
                            const BOOL nameOk = LookupPrivilegeNameW(
                                nullptr,
                                const_cast<LUID*>(&privilegeEntry.Luid),
                                privilegeName,
                                &nameLength);
                            textBuilder << L"  - "
                                << (nameOk != FALSE ? std::wstring(privilegeName) : L"<Unknown>")
                                << ((privilegeEntry.Attributes & SE_PRIVILEGE_ENABLED) != 0 ? L" [Enabled]" : L" [Disabled]")
                                << L"\n";
                        }
                    }

                    // 全信息类快照：
                    // - 枚举 TokenInformationClass 1..80；
                    // - 对每个类输出是否可读、字节长度与原始预览，避免遗漏关键字段。
                    textBuilder << L"\n[All TokenInformationClass Snapshot]\n";
                    for (ULONG classId = 1; classId <= 80; ++classId)
                    {
                        std::vector<std::uint8_t> classRawBuffer;
                        const TOKEN_INFORMATION_CLASS infoClass = static_cast<TOKEN_INFORMATION_CLASS>(classId);
                        const bool queryOk = queryTokenInfoBuffer(tokenHandle, infoClass, classRawBuffer);
                        if (queryOk)
                        {
                            textBuilder << L"  ["
                                << classId
                                << L"] "
                                << tokenInfoClassNameById(classId).toStdWString()
                                << L": size="
                                << classRawBuffer.size()
                                << L", raw="
                                << formatTokenRawPreview(classRawBuffer, 24).toStdWString()
                                << L"\n";
                        }
                        else
                        {
                            const DWORD queryError = GetLastError();
                            textBuilder << L"  ["
                                << classId
                                << L"] "
                                << tokenInfoClassNameById(classId).toStdWString()
                                << L": queryFailed("
                                << queryError
                                << L")\n";
                        }
                    }

                    CloseHandle(tokenHandle);
                }

                // 原生进程安全字段：
                // - DebugPort / DEP / Critical / Protection / Subsystem。
                if (ntQueryProcess != nullptr)
                {
                    ULONG_PTR debugPort = 0;
                    if (queryNtProcessInfoFixed(ntQueryProcess, processHandle, kProcessInfoClassDebugPort, debugPort))
                    {
                        textBuilder << L"DebugPort: " << uint64ToHex(debugPort).toStdWString() << L"\n";
                    }

                    ULONG executeFlags = 0;
                    if (queryNtProcessInfoFixed(ntQueryProcess, processHandle, kProcessInfoClassExecuteFlags, executeFlags))
                    {
                        textBuilder << L"DEPFlags: 0x"
                            << QString::number(executeFlags, 16).toUpper().toStdWString()
                            << L"\n";
                    }

                    ULONG breakOnTermination = 0;
                    if (queryNtProcessInfoFixed(ntQueryProcess, processHandle, kProcessInfoClassBreakOnTermination, breakOnTermination))
                    {
                        textBuilder << L"ProcessCriticalFlag(BreakOnTermination): "
                            << (breakOnTermination != 0 ? L"true" : L"false")
                            << L"\n";
                    }

                    std::uint8_t protectionLevel = 0;
                    if (queryNtProcessInfoFixed(ntQueryProcess, processHandle, kProcessInfoClassProtection, protectionLevel))
                    {
                        textBuilder << L"Protection: "
                            << protectionLevelToText(protectionLevel).toStdWString()
                            << L"\n";
                    }

                    ULONG subsystemType = 0;
                    if (queryNtProcessInfoFixed(ntQueryProcess, processHandle, kProcessInfoClassSubsystem, subsystemType))
                    {
                        textBuilder << L"SubsystemType: " << subsystemType << L"\n";
                    }
                }

                // GUI 资源统计：GDI / USER 对象计数。
                const DWORD gdiObjectCount = GetGuiResources(processHandle, GR_GDIOBJECTS);
                const DWORD userObjectCount = GetGuiResources(processHandle, GR_USEROBJECTS);
                textBuilder << L"GDIObjectCount: " << gdiObjectCount << L"\n";
                textBuilder << L"USERObjectCount: " << userObjectCount << L"\n";

                // IO 计数器：读写次数与字节数。
                IO_COUNTERS ioCounters{};
                if (GetProcessIoCounters(processHandle, &ioCounters) != FALSE)
                {
                    textBuilder << L"IoReadOps: " << ioCounters.ReadOperationCount << L"\n";
                    textBuilder << L"IoWriteOps: " << ioCounters.WriteOperationCount << L"\n";
                    textBuilder << L"IoReadBytes: " << ioCounters.ReadTransferCount << L"\n";
                    textBuilder << L"IoWriteBytes: " << ioCounters.WriteTransferCount << L"\n";
                }

                // Job 关联信息：是否在 Job 对象中。
                BOOL inJobObject = FALSE;
                if (IsProcessInJob(processHandle, nullptr, &inJobObject) != FALSE)
                {
                    textBuilder << L"InJobObject: " << (inJobObject != FALSE ? L"true" : L"false") << L"\n";
                }

                // 节能状态：动态查询 GetProcessInformation(ProcessPowerThrottling=4)。
                HMODULE kernel32Module = GetModuleHandleW(L"kernel32.dll");
                GetProcessInformationFn getProcessInformation = reinterpret_cast<GetProcessInformationFn>(
                    kernel32Module != nullptr ? GetProcAddress(kernel32Module, "GetProcessInformation") : nullptr);
                if (getProcessInformation != nullptr)
                {
                    ProcessPowerThrottlingStateNative powerState{};
                    powerState.version = 1;
                    const BOOL powerOk = getProcessInformation(
                        processHandle,
                        4,
                        &powerState,
                        static_cast<DWORD>(sizeof(powerState)));
                    if (powerOk != FALSE)
                    {
                        textBuilder << L"PowerThrottlingControlMask: 0x"
                            << QString::number(powerState.controlMask, 16).toUpper().toStdWString()
                            << L"\n";
                        textBuilder << L"PowerThrottlingStateMask: 0x"
                            << QString::number(powerState.stateMask, 16).toUpper().toStdWString()
                            << L"\n";
                    }
                }

                // 窗口相关信息：窗口数量 + 线程桌面名称 + 当前窗口站名称。
                const std::uint32_t windowCount = countTopLevelWindowsByPid(pidValue);
                textBuilder << L"TopLevelWindowCount: " << windowCount << L"\n";

                const QString desktopName = queryDesktopNameByProcessThreads(pidValue);
                if (!desktopName.trimmed().isEmpty())
                {
                    textBuilder << L"ThreadDesktop: " << desktopName.toStdWString() << L"\n";
                }

                HWINSTA processWindowStation = GetProcessWindowStation();
                if (processWindowStation != nullptr)
                {
                    wchar_t stationNameBuffer[256] = {};
                    DWORD stationNameBytes = 0;
                    const BOOL stationOk = GetUserObjectInformationW(
                        processWindowStation,
                        UOI_NAME,
                        stationNameBuffer,
                        static_cast<DWORD>(sizeof(stationNameBuffer)),
                        &stationNameBytes);
                    if (stationOk != FALSE)
                    {
                        textBuilder << L"ProcessWindowStation: " << stationNameBuffer << L"\n";
                    }
                }

                // 会话 ID 与句柄数补充输出。
                DWORD sessionId = 0;
                if (ProcessIdToSessionId(pidValue, &sessionId) != FALSE)
                {
                    textBuilder << L"SessionId: " << sessionId << L"\n";
                }
                DWORD handleCount = 0;
                if (GetProcessHandleCount(processHandle, &handleCount) != FALSE)
                {
                    textBuilder << L"HandleCount: " << handleCount << L"\n";
                }

                CloseHandle(processHandle);
            }

            refreshResult.detailText = QString::fromStdWString(textBuilder.str());
            refreshResult.elapsedMs = static_cast<std::uint64_t>(
                std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::steady_clock::now() - beginTime).count());

            QMetaObject::invokeMethod(
                guardThis,
                [guardThis, refreshResult, ticketValue]()
                {
                    if (guardThis == nullptr || guardThis->m_tokenRefreshTicket != ticketValue)
                    {
                        return;
                    }
                    guardThis->applyTokenRefreshResult(refreshResult);
                },
                Qt::QueuedConnection);
        });
    QThreadPool::globalInstance()->start(refreshTask);
}

void ProcessDetailWindow::refreshTokenSwitchStates()
{
    // 令牌开关回读日志：
    // - 本次回读链路复用同一个 kLogEvent；
    // - 便于把“打开句柄 -> 读取字段 -> 回填UI”串成一条可追踪链路。
    kLogEvent actionEvent;
    info << actionEvent
        << "[ProcessDetailWindow] refreshTokenSwitchStates: pid="
        << m_baseRecord.pid
        << eol;

    // setStatusLabel 作用：
    // - 统一写入状态文本与颜色；
    // - 避免多处分支重复 setText/setStyleSheet。
    const auto setStatusLabel =
        [this](const QString& statusText, const QColor& textColor, const int fontWeight)
    {
        if (m_tokenSwitchStatusLabel == nullptr)
        {
            return;
        }
        m_tokenSwitchStatusLabel->setText(statusText);
        m_tokenSwitchStatusLabel->setStyleSheet(buildStateLabelStyle(textColor, fontWeight));
    };

    if (m_baseRecord.pid == 0)
    {
        setStatusLabel(QStringLiteral("● 刷新失败：PID 无效"), statusWarningColor(), 700);
        warn << actionEvent
            << "[ProcessDetailWindow] refreshTokenSwitchStates: PID 无效。"
            << eol;
        return;
    }

    if (m_refreshTokenSwitchButton != nullptr)
    {
        m_refreshTokenSwitchButton->setEnabled(false);
    }
    setStatusLabel(QStringLiteral("● 正在读取令牌开关..."), KswordTheme::PrimaryBlueColor, 700);

    // 打开进程和令牌句柄：
    // - 读取开关只需要 TOKEN_QUERY；
    // - 进程句柄先用 QUERY_LIMITED，尽量降低权限要求。
    HANDLE processHandle = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, m_baseRecord.pid);
    if (processHandle == nullptr)
    {
        const DWORD openProcessError = GetLastError();
        setStatusLabel(
            QStringLiteral("● 刷新失败：OpenProcess(%1)").arg(openProcessError),
            statusWarningColor(),
            700);
        err << actionEvent
            << "[ProcessDetailWindow] refreshTokenSwitchStates: OpenProcess 失败, error="
            << openProcessError
            << eol;
        if (m_refreshTokenSwitchButton != nullptr)
        {
            m_refreshTokenSwitchButton->setEnabled(true);
        }
        return;
    }

    HANDLE tokenHandle = nullptr;
    if (OpenProcessToken(processHandle, TOKEN_QUERY, &tokenHandle) == FALSE || tokenHandle == nullptr)
    {
        const DWORD openTokenError = GetLastError();
        CloseHandle(processHandle);
        processHandle = nullptr;
        setStatusLabel(
            QStringLiteral("● 刷新失败：OpenProcessToken(%1)").arg(openTokenError),
            statusWarningColor(),
            700);
        err << actionEvent
            << "[ProcessDetailWindow] refreshTokenSwitchStates: OpenProcessToken 失败, error="
            << openTokenError
            << eol;
        if (m_refreshTokenSwitchButton != nullptr)
        {
            m_refreshTokenSwitchButton->setEnabled(true);
        }
        return;
    }

    // queryBoolToCheckBox 作用：
    // - 从令牌读取一个 ULONG 布尔位并写入对应复选框；
    // - 失败项写入 failItemList，便于最终状态栏汇总。
    int successCount = 0;
    QStringList failItemList;
    const auto queryBoolToCheckBox =
        [tokenHandle, &successCount, &failItemList](
            QCheckBox* checkBox,
            const TOKEN_INFORMATION_CLASS infoClass,
            const QString& itemName)
    {
        if (checkBox == nullptr)
        {
            failItemList << QStringLiteral("%1(控件为空)").arg(itemName);
            return;
        }

        bool flagValue = false;
        if (!queryTokenBoolFlag(tokenHandle, infoClass, flagValue))
        {
            const DWORD queryError = GetLastError();
            failItemList << QStringLiteral("%1(%2)").arg(itemName).arg(queryError);
            return;
        }
        checkBox->setChecked(flagValue);
        ++successCount;
    };

    queryBoolToCheckBox(m_tokenSandboxInertCheck, TokenSandBoxInert, QStringLiteral("SandboxInert"));
    queryBoolToCheckBox(m_tokenVirtualizationAllowedCheck, TokenVirtualizationAllowed, QStringLiteral("VirtualizationAllowed"));
    queryBoolToCheckBox(m_tokenVirtualizationEnabledCheck, TokenVirtualizationEnabled, QStringLiteral("VirtualizationEnabled"));
    queryBoolToCheckBox(m_tokenUiAccessCheck, TokenUIAccess, QStringLiteral("UIAccess"));
    queryBoolToCheckBox(
        m_tokenHasRestrictionsCheck,
        kTokenInfoClassHasRestrictions,
        QStringLiteral("HasRestrictions"));
    queryBoolToCheckBox(
        m_tokenIsAppContainerCheck,
        kTokenInfoClassIsAppContainer,
        QStringLiteral("IsAppContainer"));
    queryBoolToCheckBox(
        m_tokenIsRestrictedCheck,
        kTokenInfoClassIsRestricted,
        QStringLiteral("IsRestricted"));
    queryBoolToCheckBox(
        m_tokenIsLessPrivilegedAppContainerCheck,
        kTokenInfoClassIsLessPrivilegedAppContainer,
        QStringLiteral("IsLessPrivilegedAppContainer"));
    queryBoolToCheckBox(
        m_tokenIsSandboxedCheck,
        kTokenInfoClassIsSandboxed,
        QStringLiteral("IsSandboxed"));
    queryBoolToCheckBox(
        m_tokenIsAppSiloCheck,
        kTokenInfoClassIsAppSilo,
        QStringLiteral("IsAppSilo"));

    // MandatoryPolicy 读取：
    // - 一次读取两个位，分别同步到两个策略复选框；
    // - 失败同样记录到 failItemList 中。
    if (m_tokenMandatoryNoWriteUpCheck == nullptr || m_tokenMandatoryNewProcessMinCheck == nullptr)
    {
        failItemList << QStringLiteral("MandatoryPolicy(控件为空)");
    }
    else
    {
        bool noWriteUpEnabled = false;
        bool newProcessMinEnabled = false;
        if (queryTokenMandatoryPolicyBits(tokenHandle, noWriteUpEnabled, newProcessMinEnabled))
        {
            m_tokenMandatoryNoWriteUpCheck->setChecked(noWriteUpEnabled);
            m_tokenMandatoryNewProcessMinCheck->setChecked(newProcessMinEnabled);
            ++successCount;
        }
        else
        {
            const DWORD queryError = GetLastError();
            failItemList << QStringLiteral("MandatoryPolicy(%1)").arg(queryError);
        }
    }

    CloseHandle(tokenHandle);
    tokenHandle = nullptr;
    CloseHandle(processHandle);
    processHandle = nullptr;

    if (m_refreshTokenSwitchButton != nullptr)
    {
        m_refreshTokenSwitchButton->setEnabled(true);
    }

    // 最终状态栏：
    // - 全部成功显示绿色；
    // - 有失败项显示橙色并附失败清单。
    if (failItemList.isEmpty())
    {
        setStatusLabel(
            QStringLiteral("● 刷新完成：%1 项开关已同步").arg(successCount),
            statusIdleColor(),
            600);
        info << actionEvent
            << "[ProcessDetailWindow] refreshTokenSwitchStates: 完成, successCount="
            << successCount
            << eol;
    }
    else
    {
        setStatusLabel(
            QStringLiteral("● 刷新完成：成功%1，失败项=%2")
            .arg(successCount)
            .arg(failItemList.join(QStringLiteral(", "))),
            statusWarningColor(),
            700);
        warn << actionEvent
            << "[ProcessDetailWindow] refreshTokenSwitchStates: 部分失败, successCount="
            << successCount
            << ", failItems="
            << failItemList.join(QStringLiteral(" | ")).toStdString()
            << eol;
    }
}

void ProcessDetailWindow::applyTokenSwitchStates()
{
    // 令牌开关应用日志：
    // - 使用同一个 kLogEvent 贯穿整个应用流程；
    // - 每个字段单独记录结果，便于定位哪一项权限不足。
    kLogEvent actionEvent;
    info << actionEvent
        << "[ProcessDetailWindow] applyTokenSwitchStates: pid="
        << m_baseRecord.pid
        << eol;

    // setStatusLabel 作用：
    // - 统一写入状态文本与颜色；
    // - 与 refreshTokenSwitchStates 保持一致的反馈风格。
    const auto setStatusLabel =
        [this](const QString& statusText, const QColor& textColor, const int fontWeight)
    {
        if (m_tokenSwitchStatusLabel == nullptr)
        {
            return;
        }
        m_tokenSwitchStatusLabel->setText(statusText);
        m_tokenSwitchStatusLabel->setStyleSheet(buildStateLabelStyle(textColor, fontWeight));
    };

    if (m_baseRecord.pid == 0)
    {
        setStatusLabel(QStringLiteral("● 应用失败：PID 无效"), statusWarningColor(), 700);
        warn << actionEvent
            << "[ProcessDetailWindow] applyTokenSwitchStates: PID 无效。"
            << eol;
        return;
    }

    if (m_applyTokenSwitchButton != nullptr)
    {
        m_applyTokenSwitchButton->setEnabled(false);
    }
    setStatusLabel(QStringLiteral("● 正在应用令牌开关..."), KswordTheme::PrimaryBlueColor, 700);

    // 动态解析 NtSetInformationToken：
    // - 用户需求中提到 NtSetTokenInformation，这里对应 ntdll 导出的 NtSetInformationToken；
    // - 若导出不可用则直接终止并给出错误说明。
    HMODULE ntdllModule = GetModuleHandleW(L"ntdll.dll");
    if (ntdllModule == nullptr)
    {
        ntdllModule = LoadLibraryW(L"ntdll.dll");
    }
    const auto setInformationToken = reinterpret_cast<NtSetInformationTokenFn>(
        ntdllModule != nullptr ? GetProcAddress(ntdllModule, "NtSetInformationToken") : nullptr);
    if (setInformationToken == nullptr)
    {
        setStatusLabel(QStringLiteral("● 应用失败：NtSetInformationToken 不可用"), statusWarningColor(), 700);
        err << actionEvent
            << "[ProcessDetailWindow] applyTokenSwitchStates: NtSetInformationToken 不可用。"
            << eol;
        if (m_applyTokenSwitchButton != nullptr)
        {
            m_applyTokenSwitchButton->setEnabled(true);
        }
        return;
    }

    // 打开目标令牌：
    // - 写开关需要 TOKEN_ADJUST_DEFAULT；
    // - 同时保留 TOKEN_QUERY 用于后续回读和诊断。
    HANDLE processHandle = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, m_baseRecord.pid);
    if (processHandle == nullptr)
    {
        const DWORD openProcessError = GetLastError();
        setStatusLabel(
            QStringLiteral("● 应用失败：OpenProcess(%1)").arg(openProcessError),
            statusWarningColor(),
            700);
        err << actionEvent
            << "[ProcessDetailWindow] applyTokenSwitchStates: OpenProcess 失败, error="
            << openProcessError
            << eol;
        if (m_applyTokenSwitchButton != nullptr)
        {
            m_applyTokenSwitchButton->setEnabled(true);
        }
        return;
    }

    HANDLE tokenHandle = nullptr;
    if (OpenProcessToken(processHandle, TOKEN_ADJUST_DEFAULT | TOKEN_QUERY, &tokenHandle) == FALSE || tokenHandle == nullptr)
    {
        const DWORD openTokenError = GetLastError();
        CloseHandle(processHandle);
        processHandle = nullptr;
        setStatusLabel(
            QStringLiteral("● 应用失败：OpenProcessToken(%1)").arg(openTokenError),
            statusWarningColor(),
            700);
        err << actionEvent
            << "[ProcessDetailWindow] applyTokenSwitchStates: OpenProcessToken 失败, error="
            << openTokenError
            << eol;
        if (m_applyTokenSwitchButton != nullptr)
        {
            m_applyTokenSwitchButton->setEnabled(true);
        }
        return;
    }

    // applyBoolFromCheckBox 作用：
    // - 读取复选框状态并调用 NtSetInformationToken；
    // - 每项失败都会记录 NTSTATUS，最终统一汇总到状态栏。
    int successCount = 0;
    QStringList failItemList;
    const auto applyBoolFromCheckBox =
        [setInformationToken, tokenHandle, &successCount, &failItemList](
            QCheckBox* checkBox,
            const TOKEN_INFORMATION_CLASS infoClass,
            const QString& itemName)
    {
        if (checkBox == nullptr)
        {
            failItemList << QStringLiteral("%1(控件为空)").arg(itemName);
            return;
        }

        const NTSTATUS setStatus = applyTokenBoolFlag(
            setInformationToken,
            tokenHandle,
            infoClass,
            checkBox->isChecked());
        if (!NT_SUCCESS(setStatus))
        {
            failItemList << QStringLiteral("%1(%2)").arg(itemName).arg(formatNtStatusHex(setStatus));
            return;
        }
        ++successCount;
    };

    applyBoolFromCheckBox(m_tokenSandboxInertCheck, TokenSandBoxInert, QStringLiteral("SandboxInert"));
    applyBoolFromCheckBox(m_tokenVirtualizationAllowedCheck, TokenVirtualizationAllowed, QStringLiteral("VirtualizationAllowed"));
    applyBoolFromCheckBox(m_tokenVirtualizationEnabledCheck, TokenVirtualizationEnabled, QStringLiteral("VirtualizationEnabled"));
    applyBoolFromCheckBox(m_tokenUiAccessCheck, TokenUIAccess, QStringLiteral("UIAccess"));
    applyBoolFromCheckBox(
        m_tokenHasRestrictionsCheck,
        kTokenInfoClassHasRestrictions,
        QStringLiteral("HasRestrictions"));
    applyBoolFromCheckBox(
        m_tokenIsAppContainerCheck,
        kTokenInfoClassIsAppContainer,
        QStringLiteral("IsAppContainer"));
    applyBoolFromCheckBox(
        m_tokenIsRestrictedCheck,
        kTokenInfoClassIsRestricted,
        QStringLiteral("IsRestricted"));
    applyBoolFromCheckBox(
        m_tokenIsLessPrivilegedAppContainerCheck,
        kTokenInfoClassIsLessPrivilegedAppContainer,
        QStringLiteral("IsLessPrivilegedAppContainer"));
    applyBoolFromCheckBox(
        m_tokenIsSandboxedCheck,
        kTokenInfoClassIsSandboxed,
        QStringLiteral("IsSandboxed"));
    applyBoolFromCheckBox(
        m_tokenIsAppSiloCheck,
        kTokenInfoClassIsAppSilo,
        QStringLiteral("IsAppSilo"));

    // MandatoryPolicy 写入：
    // - 由两个策略复选框合成一个 POLICY 位掩码；
    // - 使用 NtSetInformationToken(TokenMandatoryPolicy) 一次提交。
    if (m_tokenMandatoryNoWriteUpCheck == nullptr || m_tokenMandatoryNewProcessMinCheck == nullptr)
    {
        failItemList << QStringLiteral("MandatoryPolicy(控件为空)");
    }
    else
    {
        const NTSTATUS policyStatus = applyTokenMandatoryPolicyBits(
            setInformationToken,
            tokenHandle,
            m_tokenMandatoryNoWriteUpCheck->isChecked(),
            m_tokenMandatoryNewProcessMinCheck->isChecked());
        if (NT_SUCCESS(policyStatus))
        {
            ++successCount;
        }
        else
        {
            failItemList << QStringLiteral("MandatoryPolicy(%1)").arg(formatNtStatusHex(policyStatus));
        }
    }

    CloseHandle(tokenHandle);
    tokenHandle = nullptr;
    CloseHandle(processHandle);
    processHandle = nullptr;

    if (m_applyTokenSwitchButton != nullptr)
    {
        m_applyTokenSwitchButton->setEnabled(true);
    }

    // 应用结果反馈：
    // - 全成功时刷新文本令牌页与开关页；
    // - 部分失败时也刷新开关页，确保 UI 与真实状态尽量一致。
    if (failItemList.isEmpty())
    {
        setStatusLabel(
            QStringLiteral("● 应用完成：成功%1项").arg(successCount),
            statusIdleColor(),
            600);
        info << actionEvent
            << "[ProcessDetailWindow] applyTokenSwitchStates: 全部成功, successCount="
            << successCount
            << eol;
    }
    else
    {
        setStatusLabel(
            QStringLiteral("● 应用完成：成功%1，失败项=%2")
            .arg(successCount)
            .arg(failItemList.join(QStringLiteral(", "))),
            statusWarningColor(),
            700);
        warn << actionEvent
            << "[ProcessDetailWindow] applyTokenSwitchStates: 部分失败, successCount="
            << successCount
            << ", failItems="
            << failItemList.join(QStringLiteral(" | ")).toStdString()
            << eol;
    }

    requestAsyncTokenRefresh();
    refreshTokenSwitchStates();
}

void ProcessDetailWindow::applyRawTokenInformation()
{
    // 原始设置应用日志：
    // - 同一条 kLogEvent 贯穿“解析输入 -> 打开令牌 -> NtSetInformationToken”；
    // - 保证失败点可在日志中准确定位。
    kLogEvent actionEvent;
    info << actionEvent
        << "[ProcessDetailWindow] applyRawTokenInformation: pid="
        << m_baseRecord.pid
        << eol;

    // setStatusLabel 作用：
    // - 统一设置令牌设置页状态文本和颜色；
    // - 减少多分支重复 setText/setStyleSheet 代码。
    const auto setStatusLabel =
        [this](const QString& statusText, const QColor& textColor, const int fontWeight)
    {
        if (m_tokenSwitchStatusLabel == nullptr)
        {
            return;
        }
        m_tokenSwitchStatusLabel->setText(statusText);
        m_tokenSwitchStatusLabel->setStyleSheet(buildStateLabelStyle(textColor, fontWeight));
    };

    if (m_baseRecord.pid == 0)
    {
        setStatusLabel(QStringLiteral("● 原始设置失败：PID 无效"), statusWarningColor(), 700);
        warn << actionEvent
            << "[ProcessDetailWindow] applyRawTokenInformation: PID 无效。"
            << eol;
        return;
    }
    if (m_tokenRawInfoClassCombo == nullptr || m_tokenRawInputModeCombo == nullptr || m_tokenRawPayloadEdit == nullptr)
    {
        setStatusLabel(QStringLiteral("● 原始设置失败：控件未初始化"), statusWarningColor(), 700);
        err << actionEvent
            << "[ProcessDetailWindow] applyRawTokenInformation: 原始设置控件为空。"
            << eol;
        return;
    }

    const int classId = m_tokenRawInfoClassCombo->currentData().toInt();
    const QString modeKey = m_tokenRawInputModeCombo->currentData().toString().trimmed().toLower();
    const QString payloadText = m_tokenRawPayloadEdit->text().trimmed();
    if (classId <= 0)
    {
        setStatusLabel(QStringLiteral("● 原始设置失败：信息类无效"), statusWarningColor(), 700);
        warn << actionEvent
            << "[ProcessDetailWindow] applyRawTokenInformation: classId 无效="
            << classId
            << eol;
        return;
    }
    if (payloadText.isEmpty())
    {
        setStatusLabel(QStringLiteral("● 原始设置失败：负载为空"), statusWarningColor(), 700);
        warn << actionEvent
            << "[ProcessDetailWindow] applyRawTokenInformation: payload 为空。"
            << eol;
        return;
    }

    // 输入解析：
    // - UInt32/UInt64 使用 Qt 自动进制（支持 0x 前缀）；
    // - HexBytes 按空格/逗号分隔字节序列。
    std::vector<std::uint8_t> payloadBuffer;
    QString parseErrorText;
    if (modeKey == QStringLiteral("u32"))
    {
        bool parseOk = false;
        const qulonglong parsedValue = payloadText.toULongLong(&parseOk, 0);
        if (!parseOk || parsedValue > 0xFFFFFFFFULL)
        {
            parseErrorText = QStringLiteral("UInt32 解析失败");
        }
        else
        {
            const std::uint32_t value32 = static_cast<std::uint32_t>(parsedValue);
            payloadBuffer.resize(sizeof(value32));
            std::memcpy(payloadBuffer.data(), &value32, sizeof(value32));
        }
    }
    else if (modeKey == QStringLiteral("u64"))
    {
        bool parseOk = false;
        const qulonglong parsedValue = payloadText.toULongLong(&parseOk, 0);
        if (!parseOk)
        {
            parseErrorText = QStringLiteral("UInt64 解析失败");
        }
        else
        {
            const std::uint64_t value64 = static_cast<std::uint64_t>(parsedValue);
            payloadBuffer.resize(sizeof(value64));
            std::memcpy(payloadBuffer.data(), &value64, sizeof(value64));
        }
    }
    else if (modeKey == QStringLiteral("hex"))
    {
        QString normalizedText = payloadText;
        normalizedText.replace(',', ' ');
        const QStringList tokenList = normalizedText.split(' ', Qt::SkipEmptyParts);
        if (tokenList.isEmpty())
        {
            parseErrorText = QStringLiteral("HexBytes 解析失败：没有字节");
        }
        else
        {
            payloadBuffer.reserve(static_cast<std::size_t>(tokenList.size()));
            for (const QString& tokenText : tokenList)
            {
                QString oneByteText = tokenText.trimmed();
                if (oneByteText.startsWith(QStringLiteral("0x"), Qt::CaseInsensitive))
                {
                    oneByteText = oneByteText.mid(2);
                }
                bool oneByteOk = false;
                const uint oneByteValue = oneByteText.toUInt(&oneByteOk, 16);
                if (!oneByteOk || oneByteValue > 0xFFU)
                {
                    parseErrorText = QStringLiteral("HexBytes 解析失败：非法字节 '%1'").arg(tokenText);
                    payloadBuffer.clear();
                    break;
                }
                payloadBuffer.push_back(static_cast<std::uint8_t>(oneByteValue));
            }
        }
    }
    else
    {
        parseErrorText = QStringLiteral("未知输入模式");
    }

    if (!parseErrorText.isEmpty() || payloadBuffer.empty())
    {
        setStatusLabel(
            QStringLiteral("● 原始设置失败：%1").arg(parseErrorText.isEmpty() ? QStringLiteral("负载为空") : parseErrorText),
            statusWarningColor(),
            700);
        warn << actionEvent
            << "[ProcessDetailWindow] applyRawTokenInformation: 负载解析失败, mode="
            << modeKey.toStdString()
            << ", payload="
            << payloadText.toStdString()
            << ", error="
            << parseErrorText.toStdString()
            << eol;
        return;
    }

    if (m_tokenRawApplyButton != nullptr)
    {
        m_tokenRawApplyButton->setEnabled(false);
    }
    setStatusLabel(QStringLiteral("● 正在应用原始令牌设置..."), KswordTheme::PrimaryBlueColor, 700);

    // 动态解析 NtSetInformationToken：
    // - 用户选择的信息类和值将直接透传到原生 API；
    // - 此路径用于覆盖快捷开关之外的全部可尝试设置项。
    HMODULE ntdllModule = GetModuleHandleW(L"ntdll.dll");
    if (ntdllModule == nullptr)
    {
        ntdllModule = LoadLibraryW(L"ntdll.dll");
    }
    const auto setInformationToken = reinterpret_cast<NtSetInformationTokenFn>(
        ntdllModule != nullptr ? GetProcAddress(ntdllModule, "NtSetInformationToken") : nullptr);
    if (setInformationToken == nullptr)
    {
        setStatusLabel(QStringLiteral("● 原始设置失败：NtSetInformationToken 不可用"), statusWarningColor(), 700);
        err << actionEvent
            << "[ProcessDetailWindow] applyRawTokenInformation: NtSetInformationToken 不可用。"
            << eol;
        if (m_tokenRawApplyButton != nullptr)
        {
            m_tokenRawApplyButton->setEnabled(true);
        }
        return;
    }

    // 打开目标令牌：
    // - TOKEN_ADJUST_DEFAULT 覆盖大部分设置项；
    // - TOKEN_ADJUST_SESSIONID 用于 TokenSessionId 等特例信息类。
    HANDLE processHandle = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, m_baseRecord.pid);
    if (processHandle == nullptr)
    {
        const DWORD openProcessError = GetLastError();
        setStatusLabel(
            QStringLiteral("● 原始设置失败：OpenProcess(%1)").arg(openProcessError),
            statusWarningColor(),
            700);
        err << actionEvent
            << "[ProcessDetailWindow] applyRawTokenInformation: OpenProcess 失败, error="
            << openProcessError
            << eol;
        if (m_tokenRawApplyButton != nullptr)
        {
            m_tokenRawApplyButton->setEnabled(true);
        }
        return;
    }

    HANDLE tokenHandle = nullptr;
    if (OpenProcessToken(
        processHandle,
        TOKEN_ADJUST_DEFAULT | kTokenAdjustSessionIdAccess | TOKEN_QUERY,
        &tokenHandle) == FALSE || tokenHandle == nullptr)
    {
        const DWORD openTokenError = GetLastError();
        CloseHandle(processHandle);
        processHandle = nullptr;
        setStatusLabel(
            QStringLiteral("● 原始设置失败：OpenProcessToken(%1)").arg(openTokenError),
            statusWarningColor(),
            700);
        err << actionEvent
            << "[ProcessDetailWindow] applyRawTokenInformation: OpenProcessToken 失败, error="
            << openTokenError
            << eol;
        if (m_tokenRawApplyButton != nullptr)
        {
            m_tokenRawApplyButton->setEnabled(true);
        }
        return;
    }

    const TOKEN_INFORMATION_CLASS infoClass = static_cast<TOKEN_INFORMATION_CLASS>(classId);
    const NTSTATUS setStatus = setInformationToken(
        tokenHandle,
        infoClass,
        payloadBuffer.data(),
        static_cast<ULONG>(payloadBuffer.size()));

    CloseHandle(tokenHandle);
    tokenHandle = nullptr;
    CloseHandle(processHandle);
    processHandle = nullptr;

    if (m_tokenRawApplyButton != nullptr)
    {
        m_tokenRawApplyButton->setEnabled(true);
    }

    if (NT_SUCCESS(setStatus))
    {
        setStatusLabel(
            QStringLiteral("● 原始设置成功：[%1] %2, size=%3")
            .arg(classId)
            .arg(tokenInfoClassNameById(static_cast<ULONG>(classId)))
            .arg(payloadBuffer.size()),
            statusIdleColor(),
            600);
        info << actionEvent
            << "[ProcessDetailWindow] applyRawTokenInformation: 成功, classId="
            << classId
            << ", className="
            << tokenInfoClassNameById(static_cast<ULONG>(classId)).toStdString()
            << ", payloadSize="
            << payloadBuffer.size()
            << ", payloadPreview="
            << formatTokenRawPreview(payloadBuffer, 24).toStdString()
            << eol;
    }
    else
    {
        setStatusLabel(
            QStringLiteral("● 原始设置失败：[%1] %2, status=%3")
            .arg(classId)
            .arg(tokenInfoClassNameById(static_cast<ULONG>(classId)))
            .arg(formatNtStatusHex(setStatus)),
            statusWarningColor(),
            700);
        err << actionEvent
            << "[ProcessDetailWindow] applyRawTokenInformation: 失败, classId="
            << classId
            << ", className="
            << tokenInfoClassNameById(static_cast<ULONG>(classId)).toStdString()
            << ", status="
            << formatNtStatusHex(setStatus).toStdString()
            << ", payloadSize="
            << payloadBuffer.size()
            << ", payloadPreview="
            << formatTokenRawPreview(payloadBuffer, 24).toStdString()
            << eol;
    }

    // 提交后统一刷新：
    // - 令牌详情页会重新抓取完整信息类快照；
    // - 快捷开关页会重新回读可见复选框状态。
    requestAsyncTokenRefresh();
    refreshTokenSwitchStates();
}

void ProcessDetailWindow::requestAsyncPebRefresh()
{
    // PEB页异步刷新：
    // - 展示 PEB 地址、优先级、亲和性、内存与 IO 计数；
    // - 额外输出虚拟地址空间统计摘要。
    if (m_pebRefreshing || m_baseRecord.pid == 0)
    {
        return;
    }

    {
        kLogEvent event;
        info << event
            << "[ProcessDetailWindow] requestAsyncPebRefresh: 开始异步刷新, pid="
            << m_baseRecord.pid
            << eol;
    }

    m_pebRefreshing = true;
    const std::uint64_t ticketValue = ++m_pebRefreshTicket;
    if (m_refreshPebButton != nullptr)
    {
        m_refreshPebButton->setEnabled(false);
    }
    if (m_pebStatusLabel != nullptr)
    {
        m_pebStatusLabel->setText(QStringLiteral("● 正在刷新PEB..."));
        m_pebStatusLabel->setStyleSheet(buildStateLabelStyle(KswordTheme::PrimaryBlueColor, 700));
    }

    if (m_pebRefreshProgressPid == 0)
    {
        m_pebRefreshProgressPid = kPro.add("进程详情", "刷新PEB信息");
    }
    kPro.set(m_pebRefreshProgressPid, "读取PEB与地址空间", 0, 20.0f);

    const std::uint32_t pidValue = m_baseRecord.pid;
    QPointer<ProcessDetailWindow> guardThis(this);
    auto* refreshTask = QRunnable::create([guardThis, pidValue, ticketValue]()
        {
            TextRefreshResult refreshResult{};
            const auto beginTime = std::chrono::steady_clock::now();
            const auto progressDispatcher = [guardThis, ticketValue](const QString& stepText, const float progressValue)
                {
                    QMetaObject::invokeMethod(
                        guardThis,
                        [guardThis, ticketValue, stepText, progressValue]()
                        {
                            if (guardThis == nullptr || guardThis->m_pebRefreshTicket != ticketValue)
                            {
                                return;
                            }
                            if (guardThis->m_pebRefreshProgressPid != 0)
                            {
                                kPro.set(
                                    guardThis->m_pebRefreshProgressPid,
                                    stepText.toStdString(),
                                    0,
                                    progressValue);
                            }
                        },
                        Qt::QueuedConnection);
                };

            progressDispatcher(QStringLiteral("打开目标进程"), 28.0f);
            std::wostringstream textBuilder;
            textBuilder << L"[PEB / Process Summary]\n";
            textBuilder << L"PID: " << pidValue << L"\n";

            HANDLE processHandle = OpenProcess(
                PROCESS_QUERY_INFORMATION | PROCESS_QUERY_LIMITED_INFORMATION | PROCESS_VM_READ,
                FALSE,
                pidValue);
            if (processHandle == nullptr)
            {
                processHandle = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pidValue);
            }
            if (processHandle == nullptr)
            {
                refreshResult.diagnosticText = QString("OpenProcess失败(%1)").arg(GetLastError());
                textBuilder << L"OpenProcess: <failed>\n";
                progressDispatcher(QStringLiteral("打开进程失败"), 100.0f);
            }
            else
            {
                progressDispatcher(QStringLiteral("读取PEB与基础信息"), 40.0f);
                PROCESS_BASIC_INFORMATION basicInfo{};
                HMODULE ntdllModule = GetModuleHandleW(L"ntdll.dll");
                NtQueryInformationProcessFn ntQueryProcess = reinterpret_cast<NtQueryInformationProcessFn>(
                    ntdllModule != nullptr ? GetProcAddress(ntdllModule, "NtQueryInformationProcess") : nullptr);
                if (ntQueryProcess != nullptr)
                {
                    NTSTATUS basicStatus = ntQueryProcess(
                        processHandle,
                        static_cast<ULONG>(ProcessBasicInformation),
                        &basicInfo,
                        static_cast<ULONG>(sizeof(basicInfo)),
                        nullptr);
                    if (NT_SUCCESS(basicStatus))
                    {
                        textBuilder << L"PEB Address: "
                            << uint64ToHex(reinterpret_cast<std::uint64_t>(basicInfo.PebBaseAddress)).toStdWString()
                            << L"\n";
                    }
                }

                // 命令行读取：
                // - 优先使用 ProcessCommandLineInformation；
                // - 失败时回退到已缓存的 m_baseRecord.commandLine。
                const QString commandLineText = queryCommandLineTextByNt(ntQueryProcess, processHandle);
                textBuilder << L"CommandLine: "
                    << (commandLineText.trimmed().isEmpty()
                        ? L"-"
                        : commandLineText.toStdWString())
                    << L"\n";

                // 当前目录：
                // - Nt 层当前目录在不同系统结构布局不稳定；
                // - 这里先给出“映像目录”作为稳定可得的近似值。
                QString imagePathText = QString::fromStdString(ks::process::QueryProcessPathByPid(pidValue));
                QString currentDirectoryText;
                if (!imagePathText.trimmed().isEmpty())
                {
                    currentDirectoryText = QFileInfo(imagePathText).absolutePath();
                }
                textBuilder << L"CurrentDirectory(approx): "
                    << (currentDirectoryText.trimmed().isEmpty()
                        ? L"-"
                        : currentDirectoryText.toStdWString())
                    << L"\n";

                ULONG_PTR processAffinityMask = 0;
                ULONG_PTR systemAffinityMask = 0;
                if (GetProcessAffinityMask(processHandle, &processAffinityMask, &systemAffinityMask) != FALSE)
                {
                    textBuilder << L"ProcessAffinity: " << uint64ToHex(processAffinityMask).toStdWString() << L"\n";
                    std::wostringstream coreTextBuilder;
                    bool firstCore = true;
                    for (int bitIndex = 0; bitIndex < static_cast<int>(sizeof(ULONG_PTR) * 8); ++bitIndex)
                    {
                        const ULONG_PTR mask = static_cast<ULONG_PTR>(1ULL) << bitIndex;
                        if ((processAffinityMask & mask) == 0)
                        {
                            continue;
                        }
                        if (!firstCore)
                        {
                            coreTextBuilder << L",";
                        }
                        coreTextBuilder << bitIndex;
                        firstCore = false;
                    }
                    textBuilder << L"CpuCoreAffinity: " << coreTextBuilder.str() << L"\n";
                }

                const DWORD priorityClass = GetPriorityClass(processHandle);
                textBuilder << L"PriorityClass: " << describePriorityClass(priorityClass).toStdWString() << L"\n";

                // Wow64 状态：
                // - 输出当前进程机器架构与 Wow64 来宾架构。
                USHORT processMachine = IMAGE_FILE_MACHINE_UNKNOWN;
                USHORT nativeMachine = IMAGE_FILE_MACHINE_UNKNOWN;
                if (IsWow64Process2(processHandle, &processMachine, &nativeMachine) != FALSE)
                {
                    textBuilder << L"Wow64ProcessMachine: 0x"
                        << QString::number(processMachine, 16).toUpper().toStdWString()
                        << L"\n";
                    textBuilder << L"Wow64NativeMachine: 0x"
                        << QString::number(nativeMachine, 16).toUpper().toStdWString()
                        << L"\n";
                }

                // 启动时间与 CPU 时间（内核 + 用户）。
                FILETIME creationTime{};
                FILETIME exitTime{};
                FILETIME kernelTime{};
                FILETIME userTime{};
                if (GetProcessTimes(
                    processHandle,
                    &creationTime,
                    &exitTime,
                    &kernelTime,
                    &userTime) != FALSE)
                {
                    ULARGE_INTEGER kernelValue{};
                    kernelValue.LowPart = kernelTime.dwLowDateTime;
                    kernelValue.HighPart = kernelTime.dwHighDateTime;
                    ULARGE_INTEGER userValue{};
                    userValue.LowPart = userTime.dwLowDateTime;
                    userValue.HighPart = userTime.dwHighDateTime;
                    const double kernelMs = static_cast<double>(kernelValue.QuadPart) / 10000.0;
                    const double userMs = static_cast<double>(userValue.QuadPart) / 10000.0;
                    textBuilder << L"KernelCpuMs: " << kernelMs << L"\n";
                    textBuilder << L"UserCpuMs: " << userMs << L"\n";
                }

                PROCESS_MEMORY_COUNTERS_EX memoryCounters{};
                if (GetProcessMemoryInfo(
                    processHandle,
                    reinterpret_cast<PPROCESS_MEMORY_COUNTERS>(&memoryCounters),
                    sizeof(memoryCounters)) != FALSE)
                {
                    textBuilder << L"WorkingSet: " << memoryCounters.WorkingSetSize << L" bytes\n";
                    textBuilder << L"PrivateUsage: " << memoryCounters.PrivateUsage << L" bytes\n";
                    textBuilder << L"PeakWorkingSet: " << memoryCounters.PeakWorkingSetSize << L" bytes\n";
                    textBuilder << L"QuotaPagedPool: " << memoryCounters.QuotaPagedPoolUsage << L" bytes\n";
                    textBuilder << L"QuotaNonPagedPool: " << memoryCounters.QuotaNonPagedPoolUsage << L" bytes\n";
                    textBuilder << L"PageFaultCount: " << memoryCounters.PageFaultCount << L"\n";
                }

                IO_COUNTERS ioCounters{};
                if (GetProcessIoCounters(processHandle, &ioCounters) != FALSE)
                {
                    textBuilder << L"ReadOps: " << ioCounters.ReadOperationCount << L"\n";
                    textBuilder << L"WriteOps: " << ioCounters.WriteOperationCount << L"\n";
                    textBuilder << L"ReadBytes: " << ioCounters.ReadTransferCount << L"\n";
                    textBuilder << L"WriteBytes: " << ioCounters.WriteTransferCount << L"\n";
                }

                progressDispatcher(QStringLiteral("解析PEB参数块"), 55.0f);

                // 子系统信息：ProcessSubsystemInformation（可用时）。
                if (ntQueryProcess != nullptr)
                {
                    ULONG subsystemInfo = 0;
                    if (queryNtProcessInfoFixed(
                        ntQueryProcess,
                        processHandle,
                        kProcessInfoClassSubsystem,
                        subsystemInfo))
                    {
                        textBuilder << L"SubsystemInformation: " << subsystemInfo << L"\n";
                    }
                }

                // PEB 参数块解析：
                // - Native PEB 按 64 位布局读取；
                // - Wow64 PEB 按 32 位布局读取，避免 32 位进程在 64 位工具里偏移错位。
                QString pebDiagnosticText;
                std::vector<RemotePebProcessParametersRead> pebReadList;
                if (basicInfo.PebBaseAddress != nullptr)
                {
                    pebReadList.push_back(readPebProcessParameters64(
                        processHandle,
                        reinterpret_cast<std::uint64_t>(basicInfo.PebBaseAddress),
                        QStringLiteral("NativePEB")));
                }
                else
                {
                    appendPebDiagnostic(pebDiagnosticText, QStringLiteral("ProcessBasicInformation 未返回 PEB 地址。"));
                }

                ULONG_PTR wow64PebAddress = 0;
                if (ntQueryProcess != nullptr &&
                    queryNtProcessInfoFixed(
                        ntQueryProcess,
                        processHandle,
                        kProcessInfoClassWow64Information,
                        wow64PebAddress) &&
                    wow64PebAddress != 0 &&
                    wow64PebAddress != reinterpret_cast<ULONG_PTR>(basicInfo.PebBaseAddress))
                {
                    pebReadList.push_back(readPebProcessParameters32(
                        processHandle,
                        static_cast<std::uint64_t>(wow64PebAddress),
                        QStringLiteral("Wow64PEB")));
                }

                std::uint64_t imageBaseAddress = 0;
                for (const RemotePebProcessParametersRead& pebRead : pebReadList)
                {
                    if (!pebRead.readOk)
                    {
                        appendPebDiagnostic(
                            pebDiagnosticText,
                            QStringLiteral("%1: %2")
                            .arg(pebRead.labelText)
                            .arg(pebRead.diagnosticText));
                        continue;
                    }

                    textBuilder << L"["
                        << pebRead.labelText.toStdWString()
                        << L"]\n";
                    textBuilder << L"  PebAddress: "
                        << uint64ToHex(pebRead.pebAddress).toStdWString()
                        << L"\n";
                    textBuilder << L"  ProcessParameters: "
                        << uint64ToHex(pebRead.processParametersAddress).toStdWString()
                        << L"\n";
                    textBuilder << L"  ImageBaseAddress: "
                        << uint64ToHex(pebRead.imageBaseAddress).toStdWString()
                        << L"\n";
                    textBuilder << L"  Environment: "
                        << uint64ToHex(pebRead.environmentAddress).toStdWString()
                        << L"\n";

                    if (!pebRead.commandLineText.trimmed().isEmpty())
                    {
                        textBuilder << L"CommandLine("
                            << pebRead.labelText.toStdWString()
                            << L"): "
                            << pebRead.commandLineText.toStdWString()
                            << L"\n";
                    }
                    if (!pebRead.imagePathText.trimmed().isEmpty())
                    {
                        imagePathText = pebRead.imagePathText;
                        textBuilder << L"ImagePath("
                            << pebRead.labelText.toStdWString()
                            << L"): "
                            << pebRead.imagePathText.toStdWString()
                            << L"\n";
                    }
                    if (!pebRead.currentDirectoryText.trimmed().isEmpty())
                    {
                        currentDirectoryText = pebRead.currentDirectoryText;
                        textBuilder << L"CurrentDirectory("
                            << pebRead.labelText.toStdWString()
                            << L"): "
                            << pebRead.currentDirectoryText.toStdWString()
                            << L"\n";
                    }
                    if (imageBaseAddress == 0 && pebRead.imageBaseAddress != 0)
                    {
                        imageBaseAddress = pebRead.imageBaseAddress;
                    }
                }

                if (!pebDiagnosticText.trimmed().isEmpty())
                {
                    appendPebDiagnostic(refreshResult.diagnosticText, pebDiagnosticText);
                }

                // 映像入口点：
                // - 优先使用 PEB.ImageBaseAddress，避免 ToolHelp 模块快照在部分进程上阻塞；
                // - 只读取 PE 头部，不枚举模块列表。
                progressDispatcher(QStringLiteral("解析映像入口点"), 60.0f);
                if (imageBaseAddress != 0)
                {
                    textBuilder << L"ImageBaseAddress: "
                        << uint64ToHex(imageBaseAddress).toStdWString()
                        << L"\n";
                    IMAGE_DOS_HEADER dosHeader{};
                    SIZE_T bytesRead = 0;
                    if (ReadProcessMemory(
                        processHandle,
                        reinterpret_cast<LPCVOID>(static_cast<std::uintptr_t>(imageBaseAddress)),
                        &dosHeader,
                        sizeof(dosHeader),
                        &bytesRead) != FALSE &&
                        bytesRead == sizeof(dosHeader) &&
                        dosHeader.e_magic == IMAGE_DOS_SIGNATURE &&
                        dosHeader.e_lfanew > 0 &&
                        dosHeader.e_lfanew < 0x100000)
                    {
                        IMAGE_NT_HEADERS64 ntHeader64{};
                        if (ReadProcessMemory(
                            processHandle,
                            reinterpret_cast<LPCVOID>(
                                static_cast<std::uintptr_t>(
                                    imageBaseAddress + static_cast<std::uint64_t>(dosHeader.e_lfanew))),
                            &ntHeader64,
                            sizeof(ntHeader64),
                            &bytesRead) != FALSE &&
                            bytesRead >= sizeof(DWORD) + sizeof(IMAGE_FILE_HEADER) + sizeof(WORD) &&
                            ntHeader64.Signature == IMAGE_NT_SIGNATURE)
                        {
                            const WORD optionalMagic = ntHeader64.OptionalHeader.Magic;
                            if (optionalMagic == IMAGE_NT_OPTIONAL_HDR32_MAGIC ||
                                optionalMagic == IMAGE_NT_OPTIONAL_HDR64_MAGIC)
                            {
                                const std::uint32_t entryRva = ntHeader64.OptionalHeader.AddressOfEntryPoint;
                                textBuilder << L"EntryPointRva: 0x"
                                    << QString::number(entryRva, 16).toUpper().toStdWString()
                                    << L"\n";
                                textBuilder << L"EntryPointAddress: "
                                    << uint64ToHex(imageBaseAddress + entryRva).toStdWString()
                                    << L"\n";
                            }
                        }
                    }
                }
                else
                {
                    appendPebDiagnostic(refreshResult.diagnosticText, QStringLiteral("PEB 未提供可用映像基址。"));
                }

                // 环境变量块预览：
                // - 从已成功解析的 PEB 参数块里选择第一个环境地址；
                // - 分块读取并设置 128KB 上限，避免环境块损坏时卡住后台任务。
                progressDispatcher(QStringLiteral("读取环境变量预览"), 64.0f);
                bool environmentPreviewOk = false;
                for (const RemotePebProcessParametersRead& pebRead : pebReadList)
                {
                    if (!pebRead.readOk || pebRead.environmentAddress == 0)
                    {
                        continue;
                    }

                    QString environmentDiagnosticText;
                    const QStringList environmentLines = readRemoteEnvironmentPreviewLines(
                        processHandle,
                        pebRead.environmentAddress,
                        &environmentDiagnosticText);
                    if (!environmentDiagnosticText.trimmed().isEmpty())
                    {
                        appendPebDiagnostic(refreshResult.diagnosticText, environmentDiagnosticText);
                    }
                    if (environmentLines.isEmpty())
                    {
                        continue;
                    }

                    environmentPreviewOk = true;
                    textBuilder << L"[EnvironmentPreview:"
                        << pebRead.labelText.toStdWString()
                        << L"]\n";
                    for (const QString& lineText : environmentLines)
                    {
                        textBuilder << L"  " << lineText.toStdWString() << L"\n";
                    }
                    break;
                }
                if (!environmentPreviewOk)
                {
                    textBuilder << L"[EnvironmentPreview]\n";
                    textBuilder << L"  <unavailable>\n";
                }

                SYSTEM_INFO systemInfo{};
                GetSystemInfo(&systemInfo);
                MEMORY_BASIC_INFORMATION memoryInfo{};
                std::uint64_t commitBytes = 0;
                std::uint64_t mappedBytes = 0;
                std::uint64_t imageBytes = 0;
                std::uint64_t privateBytes = 0;
                std::uint64_t regionCount = 0;
                std::uint64_t previewRegionCount = 0;
                constexpr std::uint64_t kMaxRegionScanCount = 60000;
                const std::uintptr_t minAddress =
                    reinterpret_cast<std::uintptr_t>(systemInfo.lpMinimumApplicationAddress);
                const std::uintptr_t maxAddress =
                    reinterpret_cast<std::uintptr_t>(systemInfo.lpMaximumApplicationAddress);
                std::uintptr_t cursorAddress = minAddress;
                bool regionScanTruncated = false;
                bool regionScanTimeout = false;
                const auto regionScanDeadline = beginTime + std::chrono::seconds(8);

                progressDispatcher(QStringLiteral("扫描虚拟地址空间"), 68.0f);
                textBuilder << L"[VirtualAddressRegionPreview]\n";
                while (cursorAddress < maxAddress)
                {
                    if (std::chrono::steady_clock::now() > regionScanDeadline)
                    {
                        regionScanTimeout = true;
                        break;
                    }

                    const SIZE_T querySize = VirtualQueryEx(
                        processHandle,
                        reinterpret_cast<LPCVOID>(cursorAddress),
                        &memoryInfo,
                        sizeof(memoryInfo));
                    if (querySize == 0)
                    {
                        break;
                    }
                    if (memoryInfo.RegionSize == 0)
                    {
                        appendPebDiagnostic(
                            refreshResult.diagnosticText,
                            QStringLiteral("虚拟内存枚举遇到零长度区域，已提前终止。"));
                        break;
                    }

                    ++regionCount;
                    if (regionCount >= kMaxRegionScanCount)
                    {
                        regionScanTruncated = true;
                        break;
                    }

                    if ((regionCount % 2000) == 0)
                    {
                        const float progressValue = std::min(
                            90.0f,
                            60.0f + static_cast<float>(regionCount) * 0.0005f);
                        progressDispatcher(QStringLiteral("扫描虚拟地址空间"), progressValue);
                    }

                    if (memoryInfo.State == MEM_COMMIT)
                    {
                        commitBytes += static_cast<std::uint64_t>(memoryInfo.RegionSize);
                    }
                    if (memoryInfo.Type == MEM_MAPPED)
                    {
                        mappedBytes += static_cast<std::uint64_t>(memoryInfo.RegionSize);
                    }
                    if (memoryInfo.Type == MEM_IMAGE)
                    {
                        imageBytes += static_cast<std::uint64_t>(memoryInfo.RegionSize);
                    }
                    if (memoryInfo.Type == MEM_PRIVATE)
                    {
                        privateBytes += static_cast<std::uint64_t>(memoryInfo.RegionSize);
                    }

                    // 预览输出：最多列出 40 个 committed 区域，包含状态/保护/类型/映射文件。
                    if (memoryInfo.State == MEM_COMMIT && previewRegionCount < 40)
                    {
                        QString mappedPathText;
                        wchar_t mappedPathBuffer[1024] = {};
                        if ((memoryInfo.Type == MEM_MAPPED || memoryInfo.Type == MEM_IMAGE) &&
                            GetMappedFileNameW(
                                processHandle,
                                memoryInfo.BaseAddress,
                                mappedPathBuffer,
                                static_cast<DWORD>(std::size(mappedPathBuffer))) > 0)
                        {
                            mappedPathText = QString::fromWCharArray(mappedPathBuffer);
                        }

                        const std::uint64_t baseAddress = reinterpret_cast<std::uint64_t>(memoryInfo.BaseAddress);
                        const std::uint64_t endAddress = baseAddress + static_cast<std::uint64_t>(memoryInfo.RegionSize);
                        textBuilder << L"  "
                            << uint64ToHex(baseAddress).toStdWString()
                            << L"-"
                            << uint64ToHex(endAddress).toStdWString()
                            << L" | "
                            << memoryStateToText(memoryInfo.State).toStdWString()
                            << L" | "
                            << memoryProtectToText(memoryInfo.Protect).toStdWString()
                            << L" | "
                            << memoryTypeToText(memoryInfo.Type).toStdWString();
                        if (!mappedPathText.trimmed().isEmpty())
                        {
                            textBuilder << L" | " << mappedPathText.toStdWString();
                        }
                        textBuilder << L"\n";
                        ++previewRegionCount;
                    }

                    const std::uintptr_t nextAddress =
                        cursorAddress + static_cast<std::uintptr_t>(memoryInfo.RegionSize);
                    if (nextAddress <= cursorAddress)
                    {
                        appendPebDiagnostic(
                            refreshResult.diagnosticText,
                            QStringLiteral("虚拟内存枚举地址发生回绕，已提前终止。"));
                        break;
                    }
                    cursorAddress = nextAddress;
                }
                if (regionScanTruncated)
                {
                    appendPebDiagnostic(
                        refreshResult.diagnosticText,
                        QString("虚拟内存枚举达到上限(%1)，结果为部分数据。")
                        .arg(kMaxRegionScanCount));
                }
                if (regionScanTimeout)
                {
                    appendPebDiagnostic(
                        refreshResult.diagnosticText,
                        QStringLiteral("虚拟内存枚举超过8秒，已返回部分结果。"));
                }
                textBuilder << L"RegionCount: " << regionCount << L"\n";
                textBuilder << L"CommitBytes: " << commitBytes << L"\n";
                textBuilder << L"MappedBytes: " << mappedBytes << L"\n";
                textBuilder << L"ImageBytes: " << imageBytes << L"\n";
                textBuilder << L"PrivateBytes: " << privateBytes << L"\n";

                // 堆信息：只统计 HeapList 数量。
                // - 旧实现继续调用 Heap32First/Heap32Next 遍历全部堆块；
                // - 这些 API 在大进程、受保护进程或堆损坏场景下可能在内部长时间阻塞；
                // - PEB 页首要目标是 ProcessParameters/环境块解析，因此这里主动跳过堆块全量枚举。
                progressDispatcher(QStringLiteral("跳过堆块枚举"), 90.0f);
                textBuilder << L"HeapCount: <skipped>\n";
                textBuilder << L"HeapBlockCount: <skipped>\n";
                textBuilder << L"HeapBlockEnumeration: <skipped to keep PEB refresh bounded>\n";

                progressDispatcher(QStringLiteral("汇总PEB结果"), 95.0f);

                CloseHandle(processHandle);
            }

            if (!refreshResult.diagnosticText.trimmed().isEmpty())
            {
                textBuilder << L"[Diagnostic]\n";
                textBuilder << L"  " << refreshResult.diagnosticText.toStdWString() << L"\n";
            }

            refreshResult.detailText = QString::fromStdWString(textBuilder.str());
            refreshResult.elapsedMs = static_cast<std::uint64_t>(
                std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::steady_clock::now() - beginTime).count());

            QMetaObject::invokeMethod(
                guardThis,
                [guardThis, refreshResult, ticketValue]()
                {
                    if (guardThis == nullptr || guardThis->m_pebRefreshTicket != ticketValue)
                    {
                        return;
                    }
                    guardThis->applyPebRefreshResult(refreshResult);
                },
                Qt::QueuedConnection);
        });
    QThreadPool::globalInstance()->start(refreshTask);
}

void ProcessDetailWindow::applyTokenRefreshResult(const TextRefreshResult& refreshResult)
{
    m_tokenRefreshing = false;
    if (m_refreshTokenButton != nullptr)
    {
        m_refreshTokenButton->setEnabled(true);
    }
    if (m_tokenDetailOutput != nullptr)
    {
        m_tokenDetailOutput->setText(refreshResult.detailText);
    }
    if (m_tokenStatusLabel != nullptr)
    {
        m_tokenStatusLabel->setText(QString("● 刷新完成 %1 ms").arg(refreshResult.elapsedMs));
        m_tokenStatusLabel->setStyleSheet(buildStateLabelStyle(statusIdleColor(), 600));
    }
    kPro.set(m_tokenRefreshProgressPid, "令牌信息刷新完成", 0, 100.0f);
}

void ProcessDetailWindow::applyPebRefreshResult(const TextRefreshResult& refreshResult)
{
    m_pebRefreshing = false;
    if (m_refreshPebButton != nullptr)
    {
        m_refreshPebButton->setEnabled(true);
    }
    if (m_pebDetailOutput != nullptr)
    {
        m_pebDetailOutput->setText(refreshResult.detailText);
    }
    if (m_pebStatusLabel != nullptr)
    {
        QString statusText = QString("● 刷新完成 %1 ms").arg(refreshResult.elapsedMs);
        QString statusStyle = buildStateLabelStyle(statusIdleColor(), 600);
        if (!refreshResult.diagnosticText.trimmed().isEmpty())
        {
            statusText += QString(" | %1").arg(refreshResult.diagnosticText);
            statusStyle = buildStateLabelStyle(statusWarningColor(), 700);
        }
        m_pebStatusLabel->setText(statusText);
        m_pebStatusLabel->setStyleSheet(statusStyle);
    }
    kPro.set(m_pebRefreshProgressPid, "PEB信息刷新完成", 0, 100.0f);

    kLogEvent event;
    info << event
        << "[ProcessDetailWindow] applyPebRefreshResult: 完成, pid="
        << m_baseRecord.pid
        << ", elapsedMs="
        << refreshResult.elapsedMs
        << ", diagnostic="
        << refreshResult.diagnosticText.toStdString()
        << eol;
}
