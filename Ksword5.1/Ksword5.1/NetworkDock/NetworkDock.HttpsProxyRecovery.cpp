#include "NetworkDock.InternalCommon.h"

#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>

#include <cmath>
#include <limits>
#include <vector>

#pragma comment(lib, "Advapi32.lib")

namespace
{
    constexpr wchar_t kRecoveryParentPath[] =
        L"Software\\KSword\\NetworkDock";
    constexpr wchar_t kRecoverySubKey[] = L"HttpsProxyRecovery";
    constexpr wchar_t kRecoveryPath[] =
        L"Software\\KSword\\NetworkDock\\HttpsProxyRecovery";
    constexpr wchar_t kPendingValueName[] = L"Pending";
    constexpr wchar_t kSnapshotValueName[] = L"Snapshot";
    constexpr int kRecoveryFormatVersion = 1;
    constexpr DWORD kMaxSnapshotBytes = 128 * 1024;

    constexpr char kSchemaKey[] = "schema";
    constexpr char kSchemaValue[] = "ksword-https-proxy-recovery";
    constexpr char kVersionKey[] = "version";
    constexpr char kPresentKey[] = "present";
    constexpr char kValueKey[] = "value";
    constexpr char kProxyEnableKey[] = "proxy_enable";
    constexpr char kAutoDetectKey[] = "auto_detect";
    constexpr char kProxyServerKey[] = "proxy_server";
    constexpr char kProxyOverrideKey[] = "proxy_override";
    constexpr char kAutoConfigUrlKey[] = "auto_config_url";

    // setRecoveryError：组合持久化阶段与 Win32 状态，避免错误只剩数字。
    void setRecoveryError(
        QString* errorTextOut,
        const QString& operationText,
        const LONG resultCode)
    {
        if (errorTextOut != nullptr)
        {
            *errorTextOut = QStringLiteral("%1，Win32=%2")
                .arg(operationText)
                .arg(resultCode);
        }
    }

    // optionalDwordToJson：同时保存值是否存在，区分缺省与 DWORD 0。
    QJsonObject optionalDwordToJson(
        const std::optional<std::uint32_t>& value)
    {
        QJsonObject object;
        object.insert(
            QString::fromLatin1(kPresentKey),
            value.has_value());
        if (value.has_value())
        {
            object.insert(
                QString::fromLatin1(kValueKey),
                static_cast<qint64>(*value));
        }
        return object;
    }

    // optionalStringToJson：同时保存值是否存在，区分缺省与空字符串。
    QJsonObject optionalStringToJson(
        const std::optional<QString>& value)
    {
        QJsonObject object;
        object.insert(
            QString::fromLatin1(kPresentKey),
            value.has_value());
        if (value.has_value())
        {
            object.insert(QString::fromLatin1(kValueKey), *value);
        }
        return object;
    }

    // parseOptionalDword：严格校验存在标记、整数范围和 JSON 类型。
    bool parseOptionalDword(
        const QJsonObject& rootObject,
        const char* fieldName,
        std::optional<std::uint32_t>* valueOut,
        QString* errorTextOut)
    {
        if (valueOut == nullptr)
        {
            return false;
        }

        const QJsonValue fieldValue =
            rootObject.value(QString::fromLatin1(fieldName));
        if (!fieldValue.isObject())
        {
            if (errorTextOut != nullptr)
            {
                *errorTextOut = QStringLiteral(
                    "HTTPS 代理恢复快照缺少 DWORD 字段：%1")
                    .arg(QString::fromLatin1(fieldName));
            }
            return false;
        }

        const QJsonObject fieldObject = fieldValue.toObject();
        const QJsonValue presentValue =
            fieldObject.value(QString::fromLatin1(kPresentKey));
        if (!presentValue.isBool())
        {
            if (errorTextOut != nullptr)
            {
                *errorTextOut = QStringLiteral(
                    "HTTPS 代理恢复快照存在标记无效：%1")
                    .arg(QString::fromLatin1(fieldName));
            }
            return false;
        }
        if (!presentValue.toBool())
        {
            valueOut->reset();
            return true;
        }

        const QJsonValue dataValue =
            fieldObject.value(QString::fromLatin1(kValueKey));
        if (!dataValue.isDouble())
        {
            if (errorTextOut != nullptr)
            {
                *errorTextOut = QStringLiteral(
                    "HTTPS 代理恢复快照 DWORD 值无效：%1")
                    .arg(QString::fromLatin1(fieldName));
            }
            return false;
        }

        const double numericValue = dataValue.toDouble();
        if (!std::isfinite(numericValue)
            || numericValue < 0.0
            || numericValue
                > static_cast<double>(
                    std::numeric_limits<std::uint32_t>::max())
            || std::floor(numericValue) != numericValue)
        {
            if (errorTextOut != nullptr)
            {
                *errorTextOut = QStringLiteral(
                    "HTTPS 代理恢复快照 DWORD 超出范围：%1")
                    .arg(QString::fromLatin1(fieldName));
            }
            return false;
        }

        *valueOut = static_cast<std::uint32_t>(numericValue);
        return true;
    }

    // parseOptionalString：严格校验存在标记和字符串类型。
    bool parseOptionalString(
        const QJsonObject& rootObject,
        const char* fieldName,
        std::optional<QString>* valueOut,
        QString* errorTextOut)
    {
        if (valueOut == nullptr)
        {
            return false;
        }

        const QJsonValue fieldValue =
            rootObject.value(QString::fromLatin1(fieldName));
        if (!fieldValue.isObject())
        {
            if (errorTextOut != nullptr)
            {
                *errorTextOut = QStringLiteral(
                    "HTTPS 代理恢复快照缺少字符串字段：%1")
                    .arg(QString::fromLatin1(fieldName));
            }
            return false;
        }

        const QJsonObject fieldObject = fieldValue.toObject();
        const QJsonValue presentValue =
            fieldObject.value(QString::fromLatin1(kPresentKey));
        if (!presentValue.isBool())
        {
            if (errorTextOut != nullptr)
            {
                *errorTextOut = QStringLiteral(
                    "HTTPS 代理恢复快照存在标记无效：%1")
                    .arg(QString::fromLatin1(fieldName));
            }
            return false;
        }
        if (!presentValue.toBool())
        {
            valueOut->reset();
            return true;
        }

        const QJsonValue dataValue =
            fieldObject.value(QString::fromLatin1(kValueKey));
        if (!dataValue.isString())
        {
            if (errorTextOut != nullptr)
            {
                *errorTextOut = QStringLiteral(
                    "HTTPS 代理恢复快照字符串值无效：%1")
                    .arg(QString::fromLatin1(fieldName));
            }
            return false;
        }

        *valueOut = dataValue.toString();
        return true;
    }
}

bool NetworkDock::persistHttpsSystemProxyRecoveryTransaction(
    const std::optional<std::uint32_t>& proxyEnable,
    const std::optional<std::uint32_t>& autoDetect,
    const std::optional<QString>& proxyServer,
    const std::optional<QString>& proxyOverride,
    const std::optional<QString>& autoConfigUrl,
    QString* errorTextOut) const
{
    // 已发布事务必须先由启动恢复链路消费，不能被新快照覆盖。
    HKEY existingKey = nullptr;
    LONG resultCode = ::RegOpenKeyExW(
        HKEY_CURRENT_USER,
        kRecoveryPath,
        0,
        KEY_READ,
        &existingKey);
    if (resultCode == ERROR_SUCCESS && existingKey != nullptr)
    {
        DWORD pendingValue = 0;
        DWORD valueType = REG_NONE;
        DWORD byteCount = sizeof(pendingValue);
        const LONG pendingResult = ::RegQueryValueExW(
            existingKey,
            kPendingValueName,
            nullptr,
            &valueType,
            reinterpret_cast<BYTE*>(&pendingValue),
            &byteCount);
        ::RegCloseKey(existingKey);
        const bool validPendingValue = pendingResult == ERROR_SUCCESS
            && valueType == REG_DWORD
            && byteCount == sizeof(pendingValue)
            && (pendingValue == 0 || pendingValue == 1);
        if (validPendingValue && pendingValue == 1)
        {
            if (errorTextOut != nullptr)
            {
                *errorTextOut = QStringLiteral(
                    "检测到尚未完成的 HTTPS 代理恢复事务，拒绝覆盖原始快照。");
            }
            return false;
        }
        if (pendingResult != ERROR_FILE_NOT_FOUND && !validPendingValue)
        {
            setRecoveryError(
                errorTextOut,
                QStringLiteral("现有 HTTPS 代理恢复事务标记损坏"),
                pendingResult == ERROR_SUCCESS
                    ? ERROR_INVALID_DATA
                    : pendingResult);
            return false;
        }
    }
    else if (resultCode != ERROR_FILE_NOT_FOUND)
    {
        setRecoveryError(
            errorTextOut,
            QStringLiteral("检查 HTTPS 代理恢复事务失败"),
            resultCode);
        return false;
    }

    // 未发布的半成品可以安全清理，因为系统代理尚未进入改写阶段。
    if (!clearHttpsSystemProxyRecoveryTransaction(errorTextOut))
    {
        return false;
    }

    QJsonObject snapshotObject;
    snapshotObject.insert(
        QString::fromLatin1(kSchemaKey),
        QString::fromLatin1(kSchemaValue));
    snapshotObject.insert(
        QString::fromLatin1(kVersionKey),
        kRecoveryFormatVersion);
    snapshotObject.insert(
        QString::fromLatin1(kProxyEnableKey),
        optionalDwordToJson(proxyEnable));
    snapshotObject.insert(
        QString::fromLatin1(kAutoDetectKey),
        optionalDwordToJson(autoDetect));
    snapshotObject.insert(
        QString::fromLatin1(kProxyServerKey),
        optionalStringToJson(proxyServer));
    snapshotObject.insert(
        QString::fromLatin1(kProxyOverrideKey),
        optionalStringToJson(proxyOverride));
    snapshotObject.insert(
        QString::fromLatin1(kAutoConfigUrlKey),
        optionalStringToJson(autoConfigUrl));
    const QByteArray snapshotBytes =
        QJsonDocument(snapshotObject).toJson(QJsonDocument::Compact);
    if (snapshotBytes.isEmpty()
        || snapshotBytes.size() > static_cast<qsizetype>(kMaxSnapshotBytes))
    {
        if (errorTextOut != nullptr)
        {
            *errorTextOut = QStringLiteral(
                "HTTPS 代理恢复快照大小无效：%1 字节")
                .arg(snapshotBytes.size());
        }
        return false;
    }

    HKEY recoveryKey = nullptr;
    resultCode = ::RegCreateKeyExW(
        HKEY_CURRENT_USER,
        kRecoveryPath,
        0,
        nullptr,
        REG_OPTION_NON_VOLATILE,
        KEY_READ | KEY_WRITE,
        nullptr,
        &recoveryKey,
        nullptr);
    if (resultCode != ERROR_SUCCESS || recoveryKey == nullptr)
    {
        setRecoveryError(
            errorTextOut,
            QStringLiteral("创建 HTTPS 代理恢复事务失败"),
            resultCode);
        return false;
    }

    bool pendingPublished = false;
    resultCode = ::RegSetValueExW(
        recoveryKey,
        kSnapshotValueName,
        0,
        REG_BINARY,
        reinterpret_cast<const BYTE*>(snapshotBytes.constData()),
        static_cast<DWORD>(snapshotBytes.size()));
    if (resultCode == ERROR_SUCCESS)
    {
        const DWORD pendingValue = 1;
        resultCode = ::RegSetValueExW(
            recoveryKey,
            kPendingValueName,
            0,
            REG_DWORD,
            reinterpret_cast<const BYTE*>(&pendingValue),
            sizeof(pendingValue));
        pendingPublished = resultCode == ERROR_SUCCESS;
    }
    if (resultCode == ERROR_SUCCESS)
    {
        // Pending 必须先真实落盘，调用方随后才允许写 Internet Settings。
        resultCode = ::RegFlushKey(recoveryKey);
    }
    ::RegCloseKey(recoveryKey);

    if (resultCode != ERROR_SUCCESS)
    {
        setRecoveryError(
            errorTextOut,
            pendingPublished
                ? QStringLiteral("持久化 HTTPS 代理恢复事务失败")
                : QStringLiteral("写入 HTTPS 代理恢复事务失败"),
            resultCode);
        if (!pendingPublished)
        {
            QString cleanupError;
            (void)clearHttpsSystemProxyRecoveryTransaction(&cleanupError);
        }
        return false;
    }
    return true;
}

bool NetworkDock::loadHttpsSystemProxyRecoveryTransaction(
    bool* pendingOut,
    std::optional<std::uint32_t>* proxyEnableOut,
    std::optional<std::uint32_t>* autoDetectOut,
    std::optional<QString>* proxyServerOut,
    std::optional<QString>* proxyOverrideOut,
    std::optional<QString>* autoConfigUrlOut,
    QString* errorTextOut) const
{
    if (pendingOut == nullptr
        || proxyEnableOut == nullptr
        || autoDetectOut == nullptr
        || proxyServerOut == nullptr
        || proxyOverrideOut == nullptr
        || autoConfigUrlOut == nullptr)
    {
        if (errorTextOut != nullptr)
        {
            *errorTextOut = QStringLiteral("HTTPS 代理恢复事务输出对象为空。");
        }
        return false;
    }

    *pendingOut = false;
    proxyEnableOut->reset();
    autoDetectOut->reset();
    proxyServerOut->reset();
    proxyOverrideOut->reset();
    autoConfigUrlOut->reset();

    HKEY recoveryKey = nullptr;
    LONG resultCode = ::RegOpenKeyExW(
        HKEY_CURRENT_USER,
        kRecoveryPath,
        0,
        KEY_READ,
        &recoveryKey);
    if (resultCode == ERROR_FILE_NOT_FOUND)
    {
        return true;
    }
    if (resultCode != ERROR_SUCCESS || recoveryKey == nullptr)
    {
        setRecoveryError(
            errorTextOut,
            QStringLiteral("打开 HTTPS 代理恢复事务失败"),
            resultCode);
        return false;
    }

    DWORD pendingValue = 0;
    DWORD valueType = REG_NONE;
    DWORD byteCount = sizeof(pendingValue);
    resultCode = ::RegQueryValueExW(
        recoveryKey,
        kPendingValueName,
        nullptr,
        &valueType,
        reinterpret_cast<BYTE*>(&pendingValue),
        &byteCount);
    if (resultCode == ERROR_FILE_NOT_FOUND)
    {
        ::RegCloseKey(recoveryKey);
        return true;
    }
    if (resultCode != ERROR_SUCCESS
        || valueType != REG_DWORD
        || byteCount != sizeof(pendingValue)
        || pendingValue != 1)
    {
        ::RegCloseKey(recoveryKey);
        setRecoveryError(
            errorTextOut,
            QStringLiteral("HTTPS 代理恢复事务 Pending 标记无效"),
            resultCode == ERROR_SUCCESS ? ERROR_INVALID_DATA : resultCode);
        return false;
    }
    *pendingOut = true;

    valueType = REG_NONE;
    byteCount = 0;
    resultCode = ::RegQueryValueExW(
        recoveryKey,
        kSnapshotValueName,
        nullptr,
        &valueType,
        nullptr,
        &byteCount);
    if (resultCode != ERROR_SUCCESS
        || valueType != REG_BINARY
        || byteCount == 0
        || byteCount > kMaxSnapshotBytes)
    {
        ::RegCloseKey(recoveryKey);
        setRecoveryError(
            errorTextOut,
            QStringLiteral("读取 HTTPS 代理恢复快照失败"),
            resultCode == ERROR_SUCCESS ? ERROR_INVALID_DATA : resultCode);
        return false;
    }

    std::vector<char> snapshotBuffer(
        static_cast<std::size_t>(byteCount));
    resultCode = ::RegQueryValueExW(
        recoveryKey,
        kSnapshotValueName,
        nullptr,
        &valueType,
        reinterpret_cast<BYTE*>(snapshotBuffer.data()),
        &byteCount);
    ::RegCloseKey(recoveryKey);
    if (resultCode != ERROR_SUCCESS)
    {
        setRecoveryError(
            errorTextOut,
            QStringLiteral("读取 HTTPS 代理恢复快照失败"),
            resultCode);
        return false;
    }

    const QByteArray snapshotBytes(
        snapshotBuffer.data(),
        static_cast<qsizetype>(byteCount));
    QJsonParseError parseError;
    const QJsonDocument snapshotDocument =
        QJsonDocument::fromJson(snapshotBytes, &parseError);
    if (parseError.error != QJsonParseError::NoError
        || !snapshotDocument.isObject())
    {
        if (errorTextOut != nullptr)
        {
            *errorTextOut = QStringLiteral(
                "解析 HTTPS 代理恢复快照失败：%1")
                .arg(parseError.errorString());
        }
        return false;
    }

    const QJsonObject snapshotObject = snapshotDocument.object();
    const QJsonValue schemaValue =
        snapshotObject.value(QString::fromLatin1(kSchemaKey));
    const QJsonValue versionValue =
        snapshotObject.value(QString::fromLatin1(kVersionKey));
    if (!schemaValue.isString()
        || schemaValue.toString() != QString::fromLatin1(kSchemaValue)
        || !versionValue.isDouble()
        || versionValue.toInt(-1) != kRecoveryFormatVersion)
    {
        if (errorTextOut != nullptr)
        {
            *errorTextOut = QStringLiteral(
                "HTTPS 代理恢复快照格式或版本不受支持。");
        }
        return false;
    }

    return parseOptionalDword(
            snapshotObject,
            kProxyEnableKey,
            proxyEnableOut,
            errorTextOut)
        && parseOptionalDword(
            snapshotObject,
            kAutoDetectKey,
            autoDetectOut,
            errorTextOut)
        && parseOptionalString(
            snapshotObject,
            kProxyServerKey,
            proxyServerOut,
            errorTextOut)
        && parseOptionalString(
            snapshotObject,
            kProxyOverrideKey,
            proxyOverrideOut,
            errorTextOut)
        && parseOptionalString(
            snapshotObject,
            kAutoConfigUrlKey,
            autoConfigUrlOut,
            errorTextOut);
}

bool NetworkDock::clearHttpsSystemProxyRecoveryTransaction(
    QString* errorTextOut)
{
    HKEY parentKey = nullptr;
    LONG resultCode = ::RegOpenKeyExW(
        HKEY_CURRENT_USER,
        kRecoveryParentPath,
        0,
        DELETE | KEY_ENUMERATE_SUB_KEYS | KEY_QUERY_VALUE | KEY_SET_VALUE,
        &parentKey);
    if (resultCode == ERROR_FILE_NOT_FOUND)
    {
        return true;
    }
    if (resultCode != ERROR_SUCCESS || parentKey == nullptr)
    {
        setRecoveryError(
            errorTextOut,
            QStringLiteral("打开 HTTPS 代理恢复事务父键失败"),
            resultCode);
        return false;
    }

    resultCode = ::RegDeleteTreeW(parentKey, kRecoverySubKey);
    if (resultCode == ERROR_SUCCESS || resultCode == ERROR_FILE_NOT_FOUND)
    {
        resultCode = ::RegFlushKey(parentKey);
    }
    ::RegCloseKey(parentKey);
    if (resultCode == ERROR_SUCCESS)
    {
        return true;
    }
    setRecoveryError(
        errorTextOut,
        QStringLiteral("清除 HTTPS 代理恢复事务失败"),
        resultCode);
    return false;
}

bool NetworkDock::recoverPendingHttpsSystemProxyTransaction(
    bool* recoveredOut,
    QString* errorTextOut)
{
    if (recoveredOut == nullptr)
    {
        if (errorTextOut != nullptr)
        {
            *errorTextOut = QStringLiteral("HTTPS 代理恢复结果输出对象为空。");
        }
        m_httpsProxyRecoveryRequired = true;
        return false;
    }
    *recoveredOut = false;

    bool pending = false;
    std::optional<std::uint32_t> previousProxyEnable;
    std::optional<std::uint32_t> previousAutoDetect;
    std::optional<QString> previousProxyServer;
    std::optional<QString> previousProxyOverride;
    std::optional<QString> previousAutoConfigUrl;
    QString errorText;
    if (!loadHttpsSystemProxyRecoveryTransaction(
            &pending,
            &previousProxyEnable,
            &previousAutoDetect,
            &previousProxyServer,
            &previousProxyOverride,
            &previousAutoConfigUrl,
            &errorText))
    {
        m_httpsProxyRecoveryRequired = true;
        if (errorTextOut != nullptr)
        {
            *errorTextOut = errorText;
        }
        return false;
    }
    if (!pending)
    {
        return true;
    }

    m_httpsPreviousProxyEnable = previousProxyEnable;
    m_httpsPreviousAutoDetect = previousAutoDetect;
    m_httpsPreviousProxyServer = previousProxyServer;
    m_httpsPreviousProxyOverride = previousProxyOverride;
    m_httpsPreviousAutoConfigUrl = previousAutoConfigUrl;
    m_httpsSystemProxySnapshotCaptured = true;
    m_httpsProxyRecoveryRequired = true;
    if (!restoreHttpsSystemProxySnapshot(&errorText))
    {
        if (errorTextOut != nullptr)
        {
            *errorTextOut = errorText;
        }
        return false;
    }

    *recoveredOut = true;
    return true;
}
