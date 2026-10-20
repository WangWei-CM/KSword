#include "ServiceDock.Internal.h"

#include <vector>

namespace
{
    // kServicesRegistryPath 作用：统一 SCM 服务配置的注册表根路径。
    constexpr wchar_t kServicesRegistryPath[] = L"SYSTEM\\CurrentControlSet\\Services";

    // clearRegistryErrorOutputs 作用：在每次注册表操作前清空可选错误输出。
    // 入参：errorTextOut/errorCodeOut 为调用方可选输出指针。
    // 返回：无。
    void clearRegistryErrorOutputs(QString* errorTextOut, DWORD* errorCodeOut)
    {
        if (errorTextOut != nullptr)
        {
            errorTextOut->clear();
        }
        if (errorCodeOut != nullptr)
        {
            *errorCodeOut = ERROR_SUCCESS;
        }
    }

    // setRegistryError 作用：把 Win32 注册表错误统一转换为 UI 可读文本。
    // 入参：operationText 为操作阶段；errorCode 为 Win32 错误码。
    // 返回：固定返回 false，便于调用方直接 return。
    bool setRegistryError(
        const QString& operationText,
        const DWORD errorCode,
        QString* errorTextOut,
        DWORD* errorCodeOut)
    {
        if (errorCodeOut != nullptr)
        {
            *errorCodeOut = errorCode;
        }
        if (errorTextOut != nullptr)
        {
            const QString win32Text = QString::fromUtf8(
                ks::service::FormatWin32ErrorText(errorCode).c_str());
            *errorTextOut = QStringLiteral("%1失败：%2")
                .arg(operationText, win32Text);
        }
        return false;
    }

    // isValidServiceName 作用：限制注册表删除目标只能是 Services 根键的直接子键。
    // 入参：serviceNameText 为 UI 选中的服务短名。
    // 返回：非空且不含路径跳转字符时返回 true。
    bool isValidServiceName(const QString& serviceNameText)
    {
        const QString normalizedNameText = serviceNameText.trimmed();
        return !normalizedNameText.isEmpty()
            && normalizedNameText != QStringLiteral(".")
            && normalizedNameText != QStringLiteral("..")
            && !normalizedNameText.contains(QLatin1Char('\\'))
            && !normalizedNameText.contains(QLatin1Char('/'));
    }

    // queryRegistryString 作用：读取 REG_SZ/REG_EXPAND_SZ，保留原始未展开文本。
    // 入参：openedKey 为已打开键；valueName 为值名；valueTextOut 接收字符串。
    // 返回：值存在且类型可接受时返回 true。
    bool queryRegistryString(
        const HKEY openedKey,
        const wchar_t* valueName,
        QString* valueTextOut)
    {
        if (openedKey == nullptr || valueName == nullptr || valueTextOut == nullptr)
        {
            return false;
        }
        valueTextOut->clear();

        DWORD valueType = 0;
        DWORD requiredBytes = 0;
        LONG queryResult = ::RegQueryValueExW(
            openedKey,
            valueName,
            nullptr,
            &valueType,
            nullptr,
            &requiredBytes);
        if (queryResult != ERROR_SUCCESS
            || requiredBytes == 0
            || (valueType != REG_SZ && valueType != REG_EXPAND_SZ))
        {
            return false;
        }

        std::vector<wchar_t> valueBuffer(
            static_cast<std::size_t>(requiredBytes / sizeof(wchar_t)) + 2,
            L'\0');
        queryResult = ::RegQueryValueExW(
            openedKey,
            valueName,
            nullptr,
            &valueType,
            reinterpret_cast<LPBYTE>(valueBuffer.data()),
            &requiredBytes);
        if (queryResult != ERROR_SUCCESS)
        {
            return false;
        }

        valueBuffer.back() = L'\0';
        *valueTextOut = QString::fromWCharArray(valueBuffer.data()).trimmed();
        return true;
    }

    // queryRegistryDword 作用：读取一个 REG_DWORD 配置值。
    // 入参：openedKey/valueName 指定目标；valueOut 接收 DWORD。
    // 返回：值存在、长度和类型均正确时返回 true。
    bool queryRegistryDword(
        const HKEY openedKey,
        const wchar_t* valueName,
        DWORD* valueOut)
    {
        if (openedKey == nullptr || valueName == nullptr || valueOut == nullptr)
        {
            return false;
        }

        DWORD valueType = 0;
        DWORD valueBytes = sizeof(DWORD);
        DWORD value = 0;
        const LONG queryResult = ::RegQueryValueExW(
            openedKey,
            valueName,
            nullptr,
            &valueType,
            reinterpret_cast<LPBYTE>(&value),
            &valueBytes);
        if (queryResult != ERROR_SUCCESS
            || valueType != REG_DWORD
            || valueBytes != sizeof(DWORD))
        {
            return false;
        }

        *valueOut = value;
        return true;
    }

    // populateRegistrySnapshot 作用：从一个已打开的服务键读取基础配置字段。
    // 入参：openedKey 为 Services\\<name>；snapshotOut 已预置服务名。
    // 返回：无；各 has* 标志精确描述字段是否存在。
    void populateRegistrySnapshot(
        const HKEY openedKey,
        service_dock_detail::RegistryServiceSnapshot* snapshotOut)
    {
        if (openedKey == nullptr || snapshotOut == nullptr)
        {
            return;
        }

        snapshotOut->keyReadable = true;
        (void)queryRegistryString(openedKey, L"DisplayName", &snapshotOut->displayNameText);
        (void)queryRegistryString(openedKey, L"Description", &snapshotOut->descriptionText);
        (void)queryRegistryString(openedKey, L"ImagePath", &snapshotOut->binaryPathText);
        (void)queryRegistryString(openedKey, L"ObjectName", &snapshotOut->accountText);

        HKEY parametersKey = nullptr;
        const LONG openParametersResult = ::RegOpenKeyExW(
            openedKey,
            L"Parameters",
            0,
            KEY_READ | KEY_WOW64_64KEY,
            &parametersKey);
        if (openParametersResult == ERROR_SUCCESS && parametersKey != nullptr)
        {
            (void)queryRegistryString(
                parametersKey,
                L"ServiceDll",
                &snapshotOut->serviceDllPathText);
            ::RegCloseKey(parametersKey);
        }

        snapshotOut->hasServiceType = queryRegistryDword(
            openedKey,
            L"Type",
            &snapshotOut->serviceTypeValue);
        snapshotOut->hasStartType = queryRegistryDword(
            openedKey,
            L"Start",
            &snapshotOut->startTypeValue);
        snapshotOut->hasErrorControl = queryRegistryDword(
            openedKey,
            L"ErrorControl",
            &snapshotOut->errorControlValue);

        DWORD delayedAutoStartValue = 0;
        snapshotOut->delayedAutoStart = queryRegistryDword(
            openedKey,
            L"DelayedAutostart",
            &delayedAutoStartValue)
            && delayedAutoStartValue != 0;
    }
}

bool service_dock_detail::enumerateRegistryServiceSnapshots(
    std::vector<RegistryServiceSnapshot>* snapshotListOut,
    QString* errorTextOut,
    DWORD* errorCodeOut,
    const std::function<bool()>& shouldStop)
{
    clearRegistryErrorOutputs(errorTextOut, errorCodeOut);
    if (snapshotListOut == nullptr)
    {
        return setRegistryError(
            QStringLiteral("准备注册表服务扫描"),
            ERROR_INVALID_PARAMETER,
            errorTextOut,
            errorCodeOut);
    }
    snapshotListOut->clear();

    HKEY servicesRootKey = nullptr;
    const LONG openRootResult = ::RegOpenKeyExW(
        HKEY_LOCAL_MACHINE,
        kServicesRegistryPath,
        0,
        KEY_READ | KEY_WOW64_64KEY,
        &servicesRootKey);
    if (openRootResult != ERROR_SUCCESS || servicesRootKey == nullptr)
    {
        return setRegistryError(
            QStringLiteral("打开 Services 注册表根键"),
            static_cast<DWORD>(openRootResult),
            errorTextOut,
            errorCodeOut);
    }

    DWORD subKeyCount = 0;
    DWORD maximumSubKeyLength = 0;
    const LONG infoResult = ::RegQueryInfoKeyW(
        servicesRootKey,
        nullptr,
        nullptr,
        nullptr,
        &subKeyCount,
        &maximumSubKeyLength,
        nullptr,
        nullptr,
        nullptr,
        nullptr,
        nullptr,
        nullptr);
    if (infoResult != ERROR_SUCCESS)
    {
        ::RegCloseKey(servicesRootKey);
        return setRegistryError(
            QStringLiteral("读取 Services 注册表根键信息"),
            static_cast<DWORD>(infoResult),
            errorTextOut,
            errorCodeOut);
    }

    snapshotListOut->reserve(subKeyCount);
    std::vector<wchar_t> nameBuffer(
        static_cast<std::size_t>(maximumSubKeyLength) + 2,
        L'\0');
    for (DWORD subKeyIndex = 0;; ++subKeyIndex)
    {
        if (shouldStop && shouldStop())
        {
            ::RegCloseKey(servicesRootKey);
            return setRegistryError(QStringLiteral("枚举 Services 注册表子键"), ERROR_OPERATION_ABORTED, errorTextOut, errorCodeOut);
        }
        DWORD serviceNameLength = static_cast<DWORD>(nameBuffer.size() - 1);
        FILETIME lastWriteTime{};
        LONG enumerateResult = ::RegEnumKeyExW(
            servicesRootKey,
            subKeyIndex,
            nameBuffer.data(),
            &serviceNameLength,
            nullptr,
            nullptr,
            nullptr,
            &lastWriteTime);
        if (enumerateResult == ERROR_NO_MORE_ITEMS)
        {
            break;
        }
        if (enumerateResult == ERROR_MORE_DATA)
        {
            nameBuffer.resize(nameBuffer.size() * 2, L'\0');
            --subKeyIndex;
            continue;
        }
        if (enumerateResult != ERROR_SUCCESS)
        {
            ::RegCloseKey(servicesRootKey);
            return setRegistryError(
                QStringLiteral("枚举 Services 注册表子键"),
                static_cast<DWORD>(enumerateResult),
                errorTextOut,
                errorCodeOut);
        }

        RegistryServiceSnapshot snapshot;
        snapshot.serviceNameText = QString::fromWCharArray(
            nameBuffer.data(),
            static_cast<qsizetype>(serviceNameLength)).trimmed();

        HKEY serviceKey = nullptr;
        const LONG openServiceResult = ::RegOpenKeyExW(
            servicesRootKey,
            nameBuffer.data(),
            0,
            KEY_READ | KEY_WOW64_64KEY,
            &serviceKey);
        if (openServiceResult == ERROR_SUCCESS && serviceKey != nullptr)
        {
            populateRegistrySnapshot(serviceKey, &snapshot);
            ::RegCloseKey(serviceKey);
        }
        snapshotListOut->push_back(std::move(snapshot));
    }

    ::RegCloseKey(servicesRootKey);
    return true;
}

bool service_dock_detail::queryRegistryServiceSnapshot(
    const QString& serviceNameText,
    RegistryServiceSnapshot* snapshotOut,
    QString* errorTextOut,
    DWORD* errorCodeOut)
{
    clearRegistryErrorOutputs(errorTextOut, errorCodeOut);
    if (snapshotOut == nullptr || !isValidServiceName(serviceNameText))
    {
        return setRegistryError(
            QStringLiteral("校验服务注册表目标"),
            ERROR_INVALID_PARAMETER,
            errorTextOut,
            errorCodeOut);
    }

    const QString normalizedNameText = serviceNameText.trimmed();
    const QString relativePathText = QStringLiteral("%1\\%2")
        .arg(QString::fromWCharArray(kServicesRegistryPath), normalizedNameText);
    HKEY serviceKey = nullptr;
    const LONG openResult = ::RegOpenKeyExW(
        HKEY_LOCAL_MACHINE,
        reinterpret_cast<LPCWSTR>(relativePathText.utf16()),
        0,
        KEY_READ | KEY_WOW64_64KEY,
        &serviceKey);
    if (openResult != ERROR_SUCCESS || serviceKey == nullptr)
    {
        return setRegistryError(
            QStringLiteral("打开服务注册表键"),
            static_cast<DWORD>(openResult),
            errorTextOut,
            errorCodeOut);
    }

    RegistryServiceSnapshot snapshot;
    snapshot.serviceNameText = normalizedNameText;
    populateRegistrySnapshot(serviceKey, &snapshot);
    ::RegCloseKey(serviceKey);

    *snapshotOut = std::move(snapshot);
    return true;
}

bool service_dock_detail::deleteRegistryServiceKey(
    const QString& serviceNameText,
    QString* errorTextOut,
    DWORD* errorCodeOut)
{
    clearRegistryErrorOutputs(errorTextOut, errorCodeOut);
    if (!isValidServiceName(serviceNameText))
    {
        return setRegistryError(
            QStringLiteral("校验待删除服务注册表目标"),
            ERROR_INVALID_PARAMETER,
            errorTextOut,
            errorCodeOut);
    }

    HKEY servicesRootKey = nullptr;
    const LONG openRootResult = ::RegOpenKeyExW(
        HKEY_LOCAL_MACHINE,
        kServicesRegistryPath,
        0,
        KEY_WRITE | KEY_ENUMERATE_SUB_KEYS | KEY_WOW64_64KEY,
        &servicesRootKey);
    if (openRootResult != ERROR_SUCCESS || servicesRootKey == nullptr)
    {
        return setRegistryError(
            QStringLiteral("打开可写 Services 注册表根键"),
            static_cast<DWORD>(openRootResult),
            errorTextOut,
            errorCodeOut);
    }

    const std::wstring serviceNameWide = serviceNameText.trimmed().toStdWString();
    const LONG deleteResult = ::RegDeleteTreeW(
        servicesRootKey,
        serviceNameWide.c_str());
    ::RegCloseKey(servicesRootKey);
    if (deleteResult != ERROR_SUCCESS && deleteResult != ERROR_FILE_NOT_FOUND)
    {
        return setRegistryError(
            QStringLiteral("删除服务注册表键树"),
            static_cast<DWORD>(deleteResult),
            errorTextOut,
            errorCodeOut);
    }
    return true;
}
