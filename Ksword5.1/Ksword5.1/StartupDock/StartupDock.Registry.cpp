#include "StartupDock.Internal.h"

#include <cwchar>

using namespace startup_dock_detail;

namespace
{
    // RegistryValueRecord：统一承载一次注册表值枚举结果。
    struct RegistryValueRecord
    {
        QString valueNameText; // valueNameText：值名，空字符串表示默认值。
        QString valueDataText; // valueDataText：已转换成文本的数据。
        DWORD valueType = REG_NONE; // valueType：Win32 注册表类型。
    };
    // RunKeySpec：描述 Run/RunOnce/RunServices/CommandProcessor 等“枚举整键值列表”的登录项来源。
    struct RunKeySpec
    {
        HKEY rootKey = nullptr;              // rootKey：注册表根键。
        const wchar_t* subKeyText = L"";     // subKeyText：子键路径。
        const wchar_t* sourceTypeText = L""; // sourceTypeText：类型显示文本。
        const wchar_t* userText = L"";       // userText：上下文显示文本。
        const wchar_t* detailText = L"";     // detailText：该位置的说明文本。
    };
    // SingleValueSpec：描述“固定键路径 + 固定值名”的高级注册表来源。
    struct SingleValueSpec
    {
        HKEY rootKey = nullptr;              // rootKey：注册表根键。
        const wchar_t* subKeyText = L"";     // subKeyText：子键路径。
        const wchar_t* valueNameText = L"";  // valueNameText：固定值名。
        const wchar_t* sourceTypeText = L""; // sourceTypeText：类型显示文本。
        const wchar_t* userText = L"";       // userText：上下文显示文本。
        const wchar_t* detailText = L"";     // detailText：位置说明文本。
        bool resolveClsidFromValueData = false; // resolveClsidFromValueData：值数据是 CLSID 时解析宿主。
    };
    // ValueEnumSpec：描述“固定键路径 + 枚举全部值”的高级注册表来源。
    struct ValueEnumSpec
    {
        HKEY rootKey = nullptr;              // rootKey：注册表根键。
        const wchar_t* subKeyText = L"";     // subKeyText：子键路径。
        const wchar_t* sourceTypeText = L""; // sourceTypeText：类型显示文本。
        const wchar_t* userText = L"";       // userText：上下文显示文本。
        const wchar_t* detailText = L"";     // detailText：位置说明文本。
        bool resolveClsidFromValueData = false; // resolveClsidFromValueData：值数据是 CLSID 时解析宿主。
        bool resolveClsidFromValueName = false; // resolveClsidFromValueName：值名是 CLSID 时解析宿主。
    };
    // SubKeyValueSpec：描述“枚举子键，再从每个子键中读一个指定值”的来源。
    struct SubKeyValueSpec
    {
        HKEY rootKey = nullptr;              // rootKey：注册表根键。
        const wchar_t* subKeyText = L"";     // subKeyText：父键路径。
        const wchar_t* valueNameText = L"";  // valueNameText：每个子键中要读取的值名，空表示默认值。
        const wchar_t* sourceTypeText = L""; // sourceTypeText：类型显示文本。
        const wchar_t* userText = L"";       // userText：上下文显示文本。
        const wchar_t* detailText = L"";     // detailText：位置说明文本。
        bool resolveClsidFromValueData = false; // resolveClsidFromValueData：值数据是 CLSID 时解析宿主。
        bool resolveClsidFromSubKeyName = false; // resolveClsidFromSubKeyName：子键名是 CLSID 时解析宿主。
        bool deleteRegistryTree = false;     // deleteRegistryTree：删除时删整个子键。
    };
    // expandEnvironmentText：
    // - 作用：展开 REG_EXPAND_SZ 或路径文本中的环境变量；
    // - 失败时回退原始文本。
    QString expandEnvironmentText(const QString& inputText)
    {
        if (inputText.trimmed().isEmpty())
        {
            return QString();
        }

        wchar_t expandedBuffer[32768] = {};
        const DWORD expandedChars = ::ExpandEnvironmentStringsW(
            reinterpret_cast<LPCWSTR>(inputText.utf16()),
            expandedBuffer,
            static_cast<DWORD>(std::size(expandedBuffer)));
        if (expandedChars == 0 || expandedChars >= std::size(expandedBuffer))
        {
            return inputText.trimmed();
        }
        return QString::fromWCharArray(expandedBuffer).trimmed();
    }
    // rootKeyToText 作用：
    // - 把根键句柄转换成可读缩写；
    // - 供 locationText 和 groupLocationText 统一复用。
    QString rootKeyToText(HKEY rootKey)
    {
        if (rootKey == HKEY_CURRENT_USER)
        {
            return QStringLiteral("HKCU");
        }
        if (rootKey == HKEY_LOCAL_MACHINE)
        {
            return QStringLiteral("HKLM");
        }
        if (rootKey == HKEY_CLASSES_ROOT)
        {
            return QStringLiteral("HKCR");
        }
        return QStringLiteral("UNKNOWN");
    }
    // startupCategoryToText：本地复用分类中文名，避免匿名命名空间辅助函数访问类内私有静态成员。
    QString startupCategoryToText(const StartupDock::StartupCategory category)
    {
        switch (category)
        {
        case StartupDock::StartupCategory::All:
            return QStringLiteral("总览");
        case StartupDock::StartupCategory::Logon:
            return QStringLiteral("登录");
        case StartupDock::StartupCategory::Services:
            return QStringLiteral("服务");
        case StartupDock::StartupCategory::Drivers:
            return QStringLiteral("驱动");
        case StartupDock::StartupCategory::Tasks:
            return QStringLiteral("计划任务");
        case StartupDock::StartupCategory::Registry:
            return QStringLiteral("高级注册表");
        default:
            return QStringLiteral("未知");
        }
    }
    // buildRegistryLocationText：生成 "HKLM\\Software\\..." 形式的位置文本。
    QString buildRegistryLocationText(HKEY rootKey, const QString& subKeyText)
    {
        return QStringLiteral("%1\\%2")
            .arg(rootKeyToText(rootKey), subKeyText);
    }
    // isClsidText 作用：
    // - 粗略判断一段文本是否为 {GUID} 形式 CLSID。
    bool isClsidText(const QString& textValue)
    {
        const QString trimmedText = textValue.trimmed();
        return trimmedText.size() >= 38
            && trimmedText.startsWith(QLatin1Char('{'))
            && trimmedText.endsWith(QLatin1Char('}'));
    }
    // formatBinaryText 作用：
    // - 把二进制值压缩显示为十六进制前缀；
    // - 避免树节点里塞整段不可读原始字节。
    QString formatBinaryText(const std::vector<std::uint8_t>& rawBuffer)
    {
        if (rawBuffer.empty())
        {
            return QString();
        }

        QStringList byteTextList;
        const std::size_t displayCount = std::min<std::size_t>(rawBuffer.size(), 16);
        for (std::size_t index = 0; index < displayCount; ++index)
        {
            byteTextList << QStringLiteral("%1")
                .arg(rawBuffer[index], 2, 16, QChar('0'))
                .toUpper();
        }

        QString displayText = byteTextList.join(' ');
        if (rawBuffer.size() > displayCount)
        {
            displayText += QStringLiteral(" ... (%1 bytes)").arg(rawBuffer.size());
        }
        return displayText;
    }

    // registryDataToText 作用：
    // - 把 Win32 注册表原始缓冲转换成可展示文本；
    // - 支持常见字符串、多字符串、DWORD/QWORD 和二进制兜底。
    QString registryDataToText(const DWORD valueType, const std::vector<std::uint8_t>& rawBuffer)
    {
        if (rawBuffer.empty())
        {
            return QString();
        }

        if (valueType == REG_SZ || valueType == REG_EXPAND_SZ)
        {
            const wchar_t* textPointer = reinterpret_cast<const wchar_t*>(rawBuffer.data());
            QString valueText = QString::fromWCharArray(textPointer).trimmed();
            if (valueType == REG_EXPAND_SZ)
            {
                valueText = expandEnvironmentText(valueText);
            }
            return valueText;
        }

        if (valueType == REG_MULTI_SZ)
        {
            QStringList itemTextList;
            const wchar_t* currentPointer = reinterpret_cast<const wchar_t*>(rawBuffer.data());
            const wchar_t* endPointer = reinterpret_cast<const wchar_t*>(rawBuffer.data() + rawBuffer.size());
            while (currentPointer < endPointer && *currentPointer != L'\0')
            {
                const QString itemText = QString::fromWCharArray(currentPointer).trimmed();
                if (!itemText.isEmpty())
                {
                    itemTextList << expandEnvironmentText(itemText);
                }
                currentPointer += wcslen(currentPointer) + 1;
            }
            return itemTextList.join(QStringLiteral(" | "));
        }

        if (valueType == REG_DWORD && rawBuffer.size() >= sizeof(DWORD))
        {
            const DWORD numericValue = *reinterpret_cast<const DWORD*>(rawBuffer.data());
            return QStringLiteral("%1 (0x%2)")
                .arg(numericValue)
                .arg(numericValue, 8, 16, QChar('0'))
                .toUpper();
        }

        if (valueType == REG_QWORD && rawBuffer.size() >= sizeof(unsigned long long))
        {
            const unsigned long long numericValue = *reinterpret_cast<const unsigned long long*>(rawBuffer.data());
            return QStringLiteral("%1 (0x%2)")
                .arg(numericValue)
                .arg(numericValue, 16, 16, QChar('0'))
                .toUpper();
        }

        return formatBinaryText(rawBuffer);
    }

    // queryRegistryValueRecord 作用：
    // - 读取一个值，返回文本和类型；
    // - 失败时返回空 optional。
    std::optional<RegistryValueRecord> queryRegistryValueRecord(
        HKEY rootKey,
        const QString& subKeyText,
        const QString& valueNameText)
    {
        HKEY openedKey = nullptr;
        const LONG openResult = ::RegOpenKeyExW(
            rootKey,
            reinterpret_cast<LPCWSTR>(subKeyText.utf16()),
            0,
            KEY_QUERY_VALUE,
            &openedKey);
        if (openResult != ERROR_SUCCESS || openedKey == nullptr)
        {
            return std::nullopt;
        }

        DWORD valueType = REG_NONE;
        DWORD bufferBytes = 0;
        const LONG querySizeResult = ::RegQueryValueExW(
            openedKey,
            valueNameText.isEmpty() ? nullptr : reinterpret_cast<LPCWSTR>(valueNameText.utf16()),
            nullptr,
            &valueType,
            nullptr,
            &bufferBytes);
        if (querySizeResult != ERROR_SUCCESS || bufferBytes == 0)
        {
            ::RegCloseKey(openedKey);
            return std::nullopt;
        }

        std::vector<std::uint8_t> rawBuffer(static_cast<std::size_t>(bufferBytes));
        const LONG queryDataResult = ::RegQueryValueExW(
            openedKey,
            valueNameText.isEmpty() ? nullptr : reinterpret_cast<LPCWSTR>(valueNameText.utf16()),
            nullptr,
            &valueType,
            rawBuffer.data(),
            &bufferBytes);
        ::RegCloseKey(openedKey);
        if (queryDataResult != ERROR_SUCCESS)
        {
            return std::nullopt;
        }

        RegistryValueRecord valueRecord;
        valueRecord.valueNameText = valueNameText;
        valueRecord.valueDataText = registryDataToText(valueType, rawBuffer).trimmed();
        valueRecord.valueType = valueType;
        return valueRecord;
    }

    // enumerateRegistryValues 作用：
    // - 枚举指定键的全部值；
    // - 返回后由上层决定是否过滤空值和类型。
    std::vector<RegistryValueRecord> enumerateRegistryValues(HKEY rootKey, const QString& subKeyText)
    {
        std::vector<RegistryValueRecord> resultList;
        HKEY openedKey = nullptr;
        const LONG openResult = ::RegOpenKeyExW(
            rootKey,
            reinterpret_cast<LPCWSTR>(subKeyText.utf16()),
            0,
            KEY_QUERY_VALUE,
            &openedKey);
        if (openResult != ERROR_SUCCESS || openedKey == nullptr)
        {
            return resultList;
        }

        DWORD valueIndex = 0;
        while (true)
        {
            wchar_t valueNameBuffer[1024] = {};
            DWORD valueNameChars = static_cast<DWORD>(std::size(valueNameBuffer));
            DWORD valueType = REG_NONE;
            DWORD dataBytes = 0;
            const LONG enumHeaderResult = ::RegEnumValueW(
                openedKey,
                valueIndex,
                valueNameBuffer,
                &valueNameChars,
                nullptr,
                &valueType,
                nullptr,
                &dataBytes);
            if (enumHeaderResult == ERROR_NO_MORE_ITEMS)
            {
                break;
            }
            if (enumHeaderResult != ERROR_SUCCESS)
            {
                ++valueIndex;
                continue;
            }

            std::vector<std::uint8_t> rawBuffer(static_cast<std::size_t>(dataBytes == 0 ? 2 : dataBytes));
            valueNameChars = static_cast<DWORD>(std::size(valueNameBuffer));
            const LONG enumDataResult = ::RegEnumValueW(
                openedKey,
                valueIndex,
                valueNameBuffer,
                &valueNameChars,
                nullptr,
                &valueType,
                rawBuffer.data(),
                &dataBytes);
            if (enumDataResult == ERROR_SUCCESS)
            {
                RegistryValueRecord valueRecord;
                valueRecord.valueNameText = QString::fromWCharArray(valueNameBuffer, static_cast<int>(valueNameChars));
                valueRecord.valueDataText = registryDataToText(valueType, rawBuffer).trimmed();
                valueRecord.valueType = valueType;
                resultList.push_back(valueRecord);
            }
            ++valueIndex;
        }

        ::RegCloseKey(openedKey);
        return resultList;
    }

    // enumerateRegistrySubKeys 作用：
    // - 枚举指定键下的全部一级子键名；
    // - 常用于 Active Setup / IFEO / BHO 这类“子键即条目”的位置。
    QStringList enumerateRegistrySubKeys(HKEY rootKey, const QString& subKeyText)
    {
        QStringList subKeyList;
        HKEY openedKey = nullptr;
        const LONG openResult = ::RegOpenKeyExW(
            rootKey,
            reinterpret_cast<LPCWSTR>(subKeyText.utf16()),
            0,
            KEY_ENUMERATE_SUB_KEYS,
            &openedKey);
        if (openResult != ERROR_SUCCESS || openedKey == nullptr)
        {
            return subKeyList;
        }

        DWORD subKeyIndex = 0;
        while (true)
        {
            wchar_t subKeyNameBuffer[1024] = {};
            DWORD subKeyChars = static_cast<DWORD>(std::size(subKeyNameBuffer));
            const LONG enumResult = ::RegEnumKeyExW(
                openedKey,
                subKeyIndex,
                subKeyNameBuffer,
                &subKeyChars,
                nullptr,
                nullptr,
                nullptr,
                nullptr);
            if (enumResult == ERROR_NO_MORE_ITEMS)
            {
                break;
            }
            if (enumResult == ERROR_SUCCESS)
            {
                subKeyList.push_back(QString::fromWCharArray(subKeyNameBuffer, static_cast<int>(subKeyChars)));
            }
            ++subKeyIndex;
        }

        ::RegCloseKey(openedKey);
        return subKeyList;
    }

    // queryClsidFriendlyName 作用：
    // - 读取 CLSID 在 HKCR\\CLSID 下的默认显示名；
    // - 主要给 BHO/Hook/COM 类条目补充可读名称。
    QString queryClsidFriendlyName(const QString& clsidText)
    {
        if (!isClsidText(clsidText))
        {
            return QString();
        }

        const QString subKeyText = QStringLiteral("CLSID\\%1").arg(clsidText.trimmed());
        const std::optional<RegistryValueRecord> valueRecord = queryRegistryValueRecord(
            HKEY_CLASSES_ROOT,
            subKeyText,
            QString());
        return valueRecord.has_value() ? valueRecord->valueDataText : QString();
    }

    // queryClsidServerPath 作用：
    // - 尝试从 CLSID 下解析 InprocServer32 / LocalServer32；
    // - 成功时返回 COM 组件宿主路径。
    QString queryClsidServerPath(const QString& clsidText)
    {
        if (!isClsidText(clsidText))
        {
            return QString();
        }

        const QString clsidSubKeyText = QStringLiteral("CLSID\\%1").arg(clsidText.trimmed());
        const std::array<QString, 2> candidateKeyList{ {
            clsidSubKeyText + QStringLiteral("\\InprocServer32"),
            clsidSubKeyText + QStringLiteral("\\LocalServer32")
        } };

        for (const QString& candidateKeyText : candidateKeyList)
        {
            const std::optional<RegistryValueRecord> valueRecord = queryRegistryValueRecord(
                HKEY_CLASSES_ROOT,
                candidateKeyText,
                QString());
            if (valueRecord.has_value() && !valueRecord->valueDataText.trimmed().isEmpty())
            {
                return normalizeFilePathText(valueRecord->valueDataText);
            }
        }
        return QString();
    }

    // appendDetailPart 作用：
    // - 追加详情片段时自动处理空串；
    // - 避免外层反复手写 if 判空。
    void appendDetailPart(QString* detailTextOut, const QString& textValue)
    {
        if (detailTextOut == nullptr || textValue.trimmed().isEmpty())
        {
            return;
        }

        if (!detailTextOut->trimmed().isEmpty())
        {
            *detailTextOut += QStringLiteral("；");
        }
        *detailTextOut += textValue.trimmed();
    }

    // finalizeRegistryEntry 作用：
    // - 统一填充注册表条目的命令、路径、发布者和删除元数据；
    // - 支持值数据或子键名为 CLSID 时解析 COM 宿主路径。
    void finalizeRegistryEntry(
        StartupDock::StartupEntry* entryOut,
        const QString& rawCommandText,
        const QString& fallbackClsidText,
        const QString& registryValueNameText,
        const bool deleteRegistryTree,
        const bool resolveClsidFromValueData)
    {
        if (entryOut == nullptr)
        {
            return;
        }

        entryOut->commandText = rawCommandText.trimmed();
        entryOut->registryValueNameText = registryValueNameText;
        entryOut->deleteRegistryTree = deleteRegistryTree;
        entryOut->canOpenRegistryLocation = !entryOut->locationText.trimmed().isEmpty();
        entryOut->canDelete = entryOut->canOpenRegistryLocation;

        QString resolvedImagePathText;
        if (resolveClsidFromValueData && isClsidText(entryOut->commandText))
        {
            resolvedImagePathText = queryClsidServerPath(entryOut->commandText);
            appendDetailPart(&entryOut->detailText, QStringLiteral("CLSID=%1").arg(entryOut->commandText));
        }
        if (resolvedImagePathText.trimmed().isEmpty() && isClsidText(fallbackClsidText))
        {
            resolvedImagePathText = queryClsidServerPath(fallbackClsidText);
            appendDetailPart(&entryOut->detailText, QStringLiteral("CLSID=%1").arg(fallbackClsidText));
        }

        const QString clsidFriendlyNameText =
            isClsidText(fallbackClsidText)
            ? queryClsidFriendlyName(fallbackClsidText)
            : (isClsidText(entryOut->commandText) ? queryClsidFriendlyName(entryOut->commandText) : QString());
        appendDetailPart(
            &entryOut->detailText,
            clsidFriendlyNameText.isEmpty() ? QString() : QStringLiteral("组件=%1").arg(clsidFriendlyNameText));

        entryOut->imagePathText = resolvedImagePathText.trimmed().isEmpty()
            ? normalizeFilePathText(entryOut->commandText)
            : resolvedImagePathText;
        entryOut->publisherText = queryPublisherTextByPath(entryOut->imagePathText);
        entryOut->canOpenFileLocation = !entryOut->imagePathText.trimmed().isEmpty();
        entryOut->enabled = true;
    }
    const std::array<RunKeySpec, 21>& buildRunKeySpecList()
    {
        static const std::array<RunKeySpec, 21> runKeySpecList{ {
            { HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Run", L"Run", L"当前用户", L"用户登录后自动运行" },
            { HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\RunOnce", L"RunOnce", L"当前用户", L"当前用户一次性登录项" },
            { HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\RunServices", L"RunServices", L"当前用户", L"兼容性 RunServices 登录项" },
            { HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\RunServicesOnce", L"RunServicesOnce", L"当前用户", L"兼容性 RunServicesOnce 登录项" },
            { HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Policies\\Explorer\\Run", L"PoliciesRun", L"当前用户", L"策略控制的登录项" },
            { HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows NT\\CurrentVersion\\Windows", L"WindowsRun", L"当前用户", L"Windows 兼容 Run/Load 位置" },
            { HKEY_CURRENT_USER, L"Software\\Microsoft\\Command Processor", L"CommandProcessorAutorun", L"当前用户", L"命令行解释器 Autorun" },
            { HKEY_LOCAL_MACHINE, L"Software\\Microsoft\\Windows\\CurrentVersion\\Run", L"Run", L"本机", L"系统级登录后自动运行" },
            { HKEY_LOCAL_MACHINE, L"Software\\Microsoft\\Windows\\CurrentVersion\\RunOnce", L"RunOnce", L"本机", L"系统级一次性登录项" },
            { HKEY_LOCAL_MACHINE, L"Software\\Microsoft\\Windows\\CurrentVersion\\RunServices", L"RunServices", L"本机", L"兼容性 RunServices 登录项" },
            { HKEY_LOCAL_MACHINE, L"Software\\Microsoft\\Windows\\CurrentVersion\\RunServicesOnce", L"RunServicesOnce", L"本机", L"兼容性 RunServicesOnce 登录项" },
            { HKEY_LOCAL_MACHINE, L"Software\\Microsoft\\Windows\\CurrentVersion\\Policies\\Explorer\\Run", L"PoliciesRun", L"本机", L"策略控制的登录项" },
            { HKEY_LOCAL_MACHINE, L"Software\\Microsoft\\Windows NT\\CurrentVersion\\Windows", L"WindowsRun", L"本机", L"Windows 兼容 Run/Load 位置" },
            { HKEY_LOCAL_MACHINE, L"Software\\Microsoft\\Command Processor", L"CommandProcessorAutorun", L"本机", L"命令行解释器 Autorun" },
            { HKEY_LOCAL_MACHINE, L"Software\\Microsoft\\Windows NT\\CurrentVersion\\Terminal Server\\Install\\Software\\Microsoft\\Windows\\CurrentVersion\\Run", L"TerminalServerRun", L"本机", L"终端服务安装模式 Run" },
            { HKEY_LOCAL_MACHINE, L"Software\\Microsoft\\Windows NT\\CurrentVersion\\Terminal Server\\Install\\Software\\Microsoft\\Windows\\CurrentVersion\\RunOnce", L"TerminalServerRunOnce", L"本机", L"终端服务安装模式 RunOnce" },
            { HKEY_LOCAL_MACHINE, L"Software\\WOW6432Node\\Microsoft\\Windows\\CurrentVersion\\Run", L"Run32", L"本机(32位)", L"32 位视图 Run" },
            { HKEY_LOCAL_MACHINE, L"Software\\WOW6432Node\\Microsoft\\Windows\\CurrentVersion\\RunOnce", L"RunOnce32", L"本机(32位)", L"32 位视图 RunOnce" },
            { HKEY_LOCAL_MACHINE, L"Software\\WOW6432Node\\Microsoft\\Windows\\CurrentVersion\\RunServices", L"RunServices32", L"本机(32位)", L"32 位视图 RunServices" },
            { HKEY_LOCAL_MACHINE, L"Software\\WOW6432Node\\Microsoft\\Windows\\CurrentVersion\\RunServicesOnce", L"RunServicesOnce32", L"本机(32位)", L"32 位视图 RunServicesOnce" },
            { HKEY_LOCAL_MACHINE, L"Software\\WOW6432Node\\Microsoft\\Windows\\CurrentVersion\\Policies\\Explorer\\Run", L"PoliciesRun32", L"本机(32位)", L"32 位视图策略 Run" }
        } };
        return runKeySpecList;
    }
    const std::array<SingleValueSpec, 20>& buildSingleValueSpecList()
    {
        static const std::array<SingleValueSpec, 20> singleValueSpecList{ {
            { HKEY_LOCAL_MACHINE, L"Software\\Microsoft\\Windows NT\\CurrentVersion\\Winlogon", L"Shell", L"WinlogonShell", L"本机", L"Winlogon Shell", false },
            { HKEY_LOCAL_MACHINE, L"Software\\Microsoft\\Windows NT\\CurrentVersion\\Winlogon", L"Userinit", L"WinlogonUserinit", L"本机", L"Winlogon Userinit", false },
            { HKEY_LOCAL_MACHINE, L"Software\\Microsoft\\Windows NT\\CurrentVersion\\Winlogon", L"Taskman", L"WinlogonTaskman", L"本机", L"Winlogon Taskman", false },
            { HKEY_LOCAL_MACHINE, L"Software\\Microsoft\\Windows NT\\CurrentVersion\\Winlogon", L"VmApplet", L"WinlogonVmApplet", L"本机", L"Winlogon VM Applet", false },
            { HKEY_LOCAL_MACHINE, L"Software\\Microsoft\\Windows NT\\CurrentVersion\\Winlogon", L"GinaDLL", L"WinlogonGinaDll", L"本机", L"旧式 GINA 登录 DLL", false },
            { HKEY_LOCAL_MACHINE, L"Software\\Microsoft\\Windows NT\\CurrentVersion\\Winlogon", L"AppSetup", L"WinlogonAppSetup", L"本机", L"Winlogon AppSetup", false },
            { HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows NT\\CurrentVersion\\Winlogon", L"Shell", L"UserWinlogonShell", L"当前用户", L"用户级 Winlogon Shell", false },
            { HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows NT\\CurrentVersion\\Winlogon", L"Userinit", L"UserWinlogonUserinit", L"当前用户", L"用户级 Winlogon Userinit", false },
            { HKEY_LOCAL_MACHINE, L"Software\\Microsoft\\Windows NT\\CurrentVersion\\Windows", L"AppInit_DLLs", L"AppInitDlls", L"本机", L"AppInit DLL 列表", false },
            { HKEY_LOCAL_MACHINE, L"Software\\WOW6432Node\\Microsoft\\Windows NT\\CurrentVersion\\Windows", L"AppInit_DLLs", L"AppInitDlls32", L"本机(32位)", L"32 位 AppInit DLL 列表", false },
            { HKEY_LOCAL_MACHINE, L"System\\CurrentControlSet\\Control\\Lsa", L"Authentication Packages", L"LsaAuthPackages", L"本机", L"LSA 认证包", false },
            { HKEY_LOCAL_MACHINE, L"System\\CurrentControlSet\\Control\\Lsa", L"Security Packages", L"LsaSecurityPackages", L"本机", L"LSA 安全包", false },
            { HKEY_LOCAL_MACHINE, L"System\\CurrentControlSet\\Control\\Lsa", L"Notification Packages", L"LsaNotificationPackages", L"本机", L"LSA 通知包", false },
            { HKEY_LOCAL_MACHINE, L"System\\CurrentControlSet\\Control\\Lsa\\OSConfig", L"Security Packages", L"LsaOsConfigSecurityPackages", L"本机", L"LSA OSConfig 安全包", false },
            { HKEY_LOCAL_MACHINE, L"System\\CurrentControlSet\\Control\\Session Manager", L"BootExecute", L"BootExecute", L"本机", L"会话管理器启动前执行命令", false },
            { HKEY_LOCAL_MACHINE, L"System\\CurrentControlSet\\Control\\Session Manager", L"SetupExecute", L"SetupExecute", L"本机", L"会话管理器 SetupExecute", false },
            { HKEY_LOCAL_MACHINE, L"System\\CurrentControlSet\\Control\\Session Manager", L"Execute", L"SessionManagerExecute", L"本机", L"会话管理器 Execute", false },
            { HKEY_LOCAL_MACHINE, L"Software\\Microsoft\\Windows\\CurrentVersion\\Policies\\System", L"Shell", L"PoliciesSystemShell", L"本机", L"策略指定系统 Shell", false },
            { HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows NT\\CurrentVersion\\Windows", L"Load", L"WindowsLoad", L"当前用户", L"Windows 兼容 Load", false },
            { HKEY_LOCAL_MACHINE, L"Software\\Microsoft\\Windows NT\\CurrentVersion\\Windows", L"Load", L"MachineWindowsLoad", L"本机", L"系统级 Windows Load", false }
        } };
        return singleValueSpecList;
    }
    const std::array<ValueEnumSpec, 19>& buildValueEnumSpecList()
    {
        static const std::array<ValueEnumSpec, 19> valueEnumSpecList{ {
            { HKEY_LOCAL_MACHINE, L"Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\ShellExecuteHooks", L"ShellExecuteHooks", L"本机", L"Explorer Shell Execute Hooks", true, true },
            { HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\ShellExecuteHooks", L"ShellExecuteHooksUser", L"当前用户", L"用户级 Explorer Shell Execute Hooks", true, true },
            { HKEY_LOCAL_MACHINE, L"Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\SharedTaskScheduler", L"SharedTaskScheduler", L"本机", L"Explorer Shared Task Scheduler", true, false },
            { HKEY_LOCAL_MACHINE, L"Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\ShellServiceObjectDelayLoad", L"ShellDelayLoad", L"本机", L"Explorer 延迟加载 COM", true, false },
            { HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\ShellServiceObjectDelayLoad", L"ShellDelayLoadUser", L"当前用户", L"用户级 Explorer 延迟加载 COM", true, false },
            { HKEY_LOCAL_MACHINE, L"Software\\Microsoft\\Windows\\CurrentVersion\\Shell Extensions\\Approved", L"ShellExtensionsApproved", L"本机", L"Shell 扩展白名单", false, true },
            { HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Shell Extensions\\Approved", L"ShellExtensionsApprovedUser", L"当前用户", L"用户级 Shell 扩展白名单", false, true },
            { HKEY_LOCAL_MACHINE, L"Software\\Microsoft\\Internet Explorer\\URLSearchHooks", L"IEUrlSearchHooks", L"本机", L"Internet Explorer URL Search Hooks", true, true },
            { HKEY_CURRENT_USER, L"Software\\Microsoft\\Internet Explorer\\URLSearchHooks", L"IEUrlSearchHooksUser", L"当前用户", L"用户级 URL Search Hooks", true, true },
            { HKEY_LOCAL_MACHINE, L"Software\\Microsoft\\Internet Explorer\\Toolbar", L"IEToolbar", L"本机", L"Internet Explorer Toolbar", true, true },
            { HKEY_CURRENT_USER, L"Software\\Microsoft\\Internet Explorer\\Toolbar", L"IEToolbarUser", L"当前用户", L"用户级 Internet Explorer Toolbar", true, true },
            { HKEY_LOCAL_MACHINE, L"System\\CurrentControlSet\\Control\\Session Manager\\AppCertDlls", L"AppCertDlls", L"本机", L"AppCert DLL 注入点", false, false },
            { HKEY_LOCAL_MACHINE, L"System\\CurrentControlSet\\Control\\Session Manager\\KnownDLLs", L"KnownDlls", L"本机", L"Known DLL 列表", false, false },
            { HKEY_LOCAL_MACHINE, L"System\\CurrentControlSet\\Control\\Session Manager\\KnownDLLs32", L"KnownDlls32", L"本机", L"32 位 Known DLL 列表", false, false },
            { HKEY_LOCAL_MACHINE, L"Software\\Microsoft\\Windows NT\\CurrentVersion\\Drivers32", L"Drivers32", L"本机", L"系统解码器/媒体驱动", false, false },
            { HKEY_LOCAL_MACHINE, L"Software\\WOW6432Node\\Microsoft\\Windows NT\\CurrentVersion\\Drivers32", L"Drivers32Wow64", L"本机(32位)", L"32 位解码器/媒体驱动", false, false },
            { HKEY_LOCAL_MACHINE, L"Software\\Microsoft\\Windows\\CurrentVersion\\RunOnceEx", L"RunOnceExRoot", L"本机", L"RunOnceEx 根键直接值", false, false },
            { HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\RunOnceEx", L"RunOnceExRootUser", L"当前用户", L"用户级 RunOnceEx 根键直接值", false, false },
            { HKEY_LOCAL_MACHINE, L"Software\\WOW6432Node\\Microsoft\\Windows\\CurrentVersion\\RunOnceEx", L"RunOnceExRoot32", L"本机(32位)", L"32 位 RunOnceEx 根键直接值", false, false }
        } };
        return valueEnumSpecList;
    }
    const std::array<SubKeyValueSpec, 15>& buildSubKeyValueSpecList()
    {
        static const std::array<SubKeyValueSpec, 15> subKeyValueSpecList{ {
            { HKEY_LOCAL_MACHINE, L"Software\\Microsoft\\Active Setup\\Installed Components", L"StubPath", L"ActiveSetup", L"本机", L"Active Setup StubPath", true, false, true },
            { HKEY_LOCAL_MACHINE, L"Software\\Microsoft\\Windows NT\\CurrentVersion\\Image File Execution Options", L"Debugger", L"IFEO-Debugger", L"本机", L"映像执行调试器劫持", false, false, false },
            { HKEY_LOCAL_MACHINE, L"Software\\Microsoft\\Windows NT\\CurrentVersion\\Image File Execution Options", L"VerifierDlls", L"IFEO-VerifierDlls", L"本机", L"映像验证器 DLL", false, false, false },
            { HKEY_LOCAL_MACHINE, L"Software\\Microsoft\\Windows NT\\CurrentVersion\\SilentProcessExit", L"MonitorProcess", L"SilentProcessExit", L"本机", L"SilentProcessExit 监视进程", false, false, false },
            { HKEY_LOCAL_MACHINE, L"Software\\Microsoft\\Windows NT\\CurrentVersion\\Winlogon\\Notify", L"DLLName", L"WinlogonNotify", L"本机", L"Winlogon Notify 包", false, false, true },
            { HKEY_LOCAL_MACHINE, L"Software\\Microsoft\\Windows\\CurrentVersion\\Authentication\\Credential Providers", L"", L"CredentialProvider", L"本机", L"Credential Provider", false, true, true },
            { HKEY_LOCAL_MACHINE, L"Software\\Microsoft\\Windows\\CurrentVersion\\Authentication\\Credential Provider Filters", L"", L"CredentialProviderFilter", L"本机", L"Credential Provider Filter", false, true, true },
            { HKEY_LOCAL_MACHINE, L"Software\\Microsoft\\Windows\\CurrentVersion\\Authentication\\PLAP Providers", L"", L"PlapProvider", L"本机", L"PLAP Provider", false, true, true },
            { HKEY_LOCAL_MACHINE, L"Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\Browser Helper Objects", L"", L"BHO", L"本机", L"Browser Helper Object", false, true, true },
            { HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\Browser Helper Objects", L"", L"BHO-User", L"当前用户", L"用户级 Browser Helper Object", false, true, true },
            { HKEY_LOCAL_MACHINE, L"Software\\Microsoft\\Internet Explorer\\Explorer Bars", L"", L"IEExplorerBar", L"本机", L"Internet Explorer Explorer Bar", false, true, true },
            { HKEY_CURRENT_USER, L"Software\\Microsoft\\Internet Explorer\\Explorer Bars", L"", L"IEExplorerBar-User", L"当前用户", L"用户级 Explorer Bar", false, true, true },
            { HKEY_LOCAL_MACHINE, L"System\\CurrentControlSet\\Control\\Print\\Monitors", L"Driver", L"PrintMonitor", L"本机", L"打印监视器驱动", false, false, true },
            { HKEY_LOCAL_MACHINE, L"Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\ShellIconOverlayIdentifiers", L"", L"ShellIconOverlay", L"本机", L"Shell 图标覆盖标识符", true, false, true },
            { HKEY_LOCAL_MACHINE, L"Software\\Microsoft\\Windows\\CurrentVersion\\Sidebar\\Gadgets", L"Path", L"SidebarGadget", L"本机", L"Sidebar Gadget 注册", false, false, true }
        } };
        return subKeyValueSpecList;
    }
    void appendValueBasedLogonEntry(
        std::vector<StartupDock::StartupEntry>* entryListOut,
        const RunKeySpec& keySpec,
        const RegistryValueRecord& valueRecord)
    {
        if (entryListOut == nullptr || valueRecord.valueDataText.trimmed().isEmpty())
        {
            return;
        }

        const QString subKeyText = QString::fromWCharArray(keySpec.subKeyText);
        const QString locationText = buildRegistryLocationText(keySpec.rootKey, subKeyText);

        StartupDock::StartupEntry entry;
        entry.category = StartupDock::StartupCategory::Logon;
        entry.categoryText = startupCategoryToText(entry.category);
        entry.itemNameText = valueRecord.valueNameText.trimmed().isEmpty()
            ? QStringLiteral("(默认值)")
            : valueRecord.valueNameText;
        entry.locationText = locationText;
        entry.locationGroupText = locationText;
        entry.userText = QString::fromWCharArray(keySpec.userText);
        entry.sourceTypeText = QString::fromWCharArray(keySpec.sourceTypeText);
        entry.detailText = QString::fromWCharArray(keySpec.detailText);
        entry.uniqueIdText = QStringLiteral("REGLOGON|%1|%2")
            .arg(locationText, entry.itemNameText);
        finalizeRegistryEntry(
            &entry,
            valueRecord.valueDataText,
            QString(),
            valueRecord.valueNameText,
            false,
            false);
        entryListOut->push_back(entry);
    }
    void appendRunOnceExEntries(std::vector<StartupDock::StartupEntry>* entryListOut)
    {
        if (entryListOut == nullptr)
        {
            return;
        }

        const std::array<RunKeySpec, 3> runOnceExSpecList{ {
            { HKEY_LOCAL_MACHINE, L"Software\\Microsoft\\Windows\\CurrentVersion\\RunOnceEx", L"RunOnceEx", L"本机", L"RunOnceEx 子键值" },
            { HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\RunOnceEx", L"RunOnceExUser", L"当前用户", L"用户级 RunOnceEx 子键值" },
            { HKEY_LOCAL_MACHINE, L"Software\\WOW6432Node\\Microsoft\\Windows\\CurrentVersion\\RunOnceEx", L"RunOnceEx32", L"本机(32位)", L"32 位 RunOnceEx 子键值" }
        } };

        for (const RunKeySpec& keySpec : runOnceExSpecList)
        {
            const QString rootSubKeyText = QString::fromWCharArray(keySpec.subKeyText);
            const QString groupLocationText = buildRegistryLocationText(keySpec.rootKey, rootSubKeyText);
            const QStringList subKeyNameList = enumerateRegistrySubKeys(keySpec.rootKey, rootSubKeyText);
            for (const QString& subKeyNameText : subKeyNameList)
            {
                const QString itemSubKeyText = rootSubKeyText + QLatin1Char('\\') + subKeyNameText;
                const auto valueRecordList = enumerateRegistryValues(keySpec.rootKey, itemSubKeyText);
                for (const RegistryValueRecord& valueRecord : valueRecordList)
                {
                    const QString valueNameText = valueRecord.valueNameText.trimmed();
                    if (valueRecord.valueDataText.trimmed().isEmpty()
                        || valueNameText.compare(QStringLiteral("Flags"), Qt::CaseInsensitive) == 0
                        || valueNameText.compare(QStringLiteral("Title"), Qt::CaseInsensitive) == 0)
                    {
                        continue;
                    }

                    StartupDock::StartupEntry entry;
                    entry.category = StartupDock::StartupCategory::Logon;
                    entry.categoryText = startupCategoryToText(entry.category);
                    entry.itemNameText = valueNameText.isEmpty()
                        ? QStringLiteral("%1\\(默认值)").arg(subKeyNameText)
                        : QStringLiteral("%1\\%2").arg(subKeyNameText, valueNameText);
                    entry.locationText = buildRegistryLocationText(keySpec.rootKey, itemSubKeyText);
                    entry.locationGroupText = groupLocationText;
                    entry.userText = QString::fromWCharArray(keySpec.userText);
                    entry.sourceTypeText = QString::fromWCharArray(keySpec.sourceTypeText);
                    entry.detailText = QStringLiteral("%1；子键=%2")
                        .arg(QString::fromWCharArray(keySpec.detailText), subKeyNameText);
                    entry.uniqueIdText = QStringLiteral("RUNONCEEX|%1|%2")
                        .arg(entry.locationText, entry.itemNameText);
                    finalizeRegistryEntry(
                        &entry,
                        valueRecord.valueDataText,
                        QString(),
                        valueRecord.valueNameText,
                        false,
                        false);
                    entryListOut->push_back(entry);
                }
            }
        }
    }
    void appendSingleValueEntries(std::vector<StartupDock::StartupEntry>* entryListOut)
    {
        if (entryListOut == nullptr)
        {
            return;
        }

        for (const SingleValueSpec& valueSpec : buildSingleValueSpecList())
        {
            const QString subKeyText = QString::fromWCharArray(valueSpec.subKeyText);
            const QString valueNameText = QString::fromWCharArray(valueSpec.valueNameText);
            const std::optional<RegistryValueRecord> valueRecord = queryRegistryValueRecord(
                valueSpec.rootKey,
                subKeyText,
                valueNameText);
            if (!valueRecord.has_value() || valueRecord->valueDataText.trimmed().isEmpty())
            {
                continue;
            }

            StartupDock::StartupEntry entry;
            entry.category = StartupDock::StartupCategory::Registry;
            entry.categoryText = startupCategoryToText(entry.category);
            entry.itemNameText = valueNameText.trimmed().isEmpty() ? QStringLiteral("(默认值)") : valueNameText;
            entry.locationText = buildRegistryLocationText(valueSpec.rootKey, subKeyText);
            entry.locationGroupText = entry.locationText;
            entry.userText = QString::fromWCharArray(valueSpec.userText);
            entry.sourceTypeText = QString::fromWCharArray(valueSpec.sourceTypeText);
            entry.detailText = QString::fromWCharArray(valueSpec.detailText);
            entry.uniqueIdText = QStringLiteral("SINGLE|%1|%2").arg(entry.locationText, entry.itemNameText);
            finalizeRegistryEntry(
                &entry,
                valueRecord->valueDataText,
                QString(),
                valueNameText,
                false,
                valueSpec.resolveClsidFromValueData);
            entryListOut->push_back(entry);
        }
    }
    void appendValueEnumEntries(std::vector<StartupDock::StartupEntry>* entryListOut)
    {
        if (entryListOut == nullptr)
        {
            return;
        }

        for (const ValueEnumSpec& valueSpec : buildValueEnumSpecList())
        {
            const QString subKeyText = QString::fromWCharArray(valueSpec.subKeyText);
            const QString locationText = buildRegistryLocationText(valueSpec.rootKey, subKeyText);
            const auto valueRecordList = enumerateRegistryValues(valueSpec.rootKey, subKeyText);
            for (const RegistryValueRecord& valueRecord : valueRecordList)
            {
                if (valueRecord.valueDataText.trimmed().isEmpty())
                {
                    continue;
                }

                StartupDock::StartupEntry entry;
                entry.category = StartupDock::StartupCategory::Registry;
                entry.categoryText = startupCategoryToText(entry.category);
                entry.itemNameText = valueRecord.valueNameText.trimmed().isEmpty()
                    ? QStringLiteral("(默认值)")
                    : valueRecord.valueNameText;
                entry.locationText = locationText;
                entry.locationGroupText = locationText;
                entry.userText = QString::fromWCharArray(valueSpec.userText);
                entry.sourceTypeText = QString::fromWCharArray(valueSpec.sourceTypeText);
                entry.detailText = QString::fromWCharArray(valueSpec.detailText);
                entry.uniqueIdText = QStringLiteral("VALUEENUM|%1|%2")
                    .arg(locationText, entry.itemNameText);
                finalizeRegistryEntry(
                    &entry,
                    valueRecord.valueDataText,
                    valueSpec.resolveClsidFromValueName ? valueRecord.valueNameText : QString(),
                    valueRecord.valueNameText,
                    false,
                    valueSpec.resolveClsidFromValueData);
                entryListOut->push_back(entry);
            }
        }
    }
    void appendSubKeyValueEntries(std::vector<StartupDock::StartupEntry>* entryListOut)
    {
        if (entryListOut == nullptr)
        {
            return;
        }

        for (const SubKeyValueSpec& valueSpec : buildSubKeyValueSpecList())
        {
            const QString rootSubKeyText = QString::fromWCharArray(valueSpec.subKeyText);
            const QString groupLocationText = buildRegistryLocationText(valueSpec.rootKey, rootSubKeyText);
            const QString valueNameText = QString::fromWCharArray(valueSpec.valueNameText);
            const QStringList subKeyNameList = enumerateRegistrySubKeys(valueSpec.rootKey, rootSubKeyText);
            for (const QString& subKeyNameText : subKeyNameList)
            {
                const QString itemSubKeyText = rootSubKeyText + QLatin1Char('\\') + subKeyNameText;
                const std::optional<RegistryValueRecord> valueRecord = queryRegistryValueRecord(
                    valueSpec.rootKey,
                    itemSubKeyText,
                    valueNameText);
                if (!valueRecord.has_value() && !valueSpec.resolveClsidFromSubKeyName)
                {
                    continue;
                }

                RegistryValueRecord effectiveValueRecord;
                effectiveValueRecord.valueNameText = valueNameText;
                effectiveValueRecord.valueDataText = valueRecord.has_value()
                    ? valueRecord->valueDataText
                    : QString();
                effectiveValueRecord.valueType = valueRecord.has_value()
                    ? valueRecord->valueType
                    : REG_NONE;

                QString itemNameText = subKeyNameText;
                const QString clsidFallbackText = valueSpec.resolveClsidFromSubKeyName ? subKeyNameText : QString();
                const QString clsidFriendlyNameText = queryClsidFriendlyName(clsidFallbackText);
                if (!clsidFriendlyNameText.trimmed().isEmpty())
                {
                    itemNameText = clsidFriendlyNameText;
                }

                QString commandText = effectiveValueRecord.valueDataText;
                if (commandText.trimmed().isEmpty() && valueSpec.resolveClsidFromSubKeyName)
                {
                    commandText = subKeyNameText;
                }
                if (commandText.trimmed().isEmpty())
                {
                    continue;
                }

                StartupDock::StartupEntry entry;
                entry.category = StartupDock::StartupCategory::Registry;
                entry.categoryText = startupCategoryToText(entry.category);
                entry.itemNameText = itemNameText;
                entry.locationText = buildRegistryLocationText(valueSpec.rootKey, itemSubKeyText);
                entry.locationGroupText = groupLocationText;
                entry.userText = QString::fromWCharArray(valueSpec.userText);
                entry.sourceTypeText = QString::fromWCharArray(valueSpec.sourceTypeText);
                entry.detailText = QStringLiteral("%1；子键=%2")
                    .arg(QString::fromWCharArray(valueSpec.detailText), subKeyNameText);
                entry.uniqueIdText = QStringLiteral("SUBKEY|%1|%2")
                    .arg(entry.locationText, valueNameText);
                finalizeRegistryEntry(
                    &entry,
                    commandText,
                    clsidFallbackText,
                    effectiveValueRecord.valueNameText,
                    valueSpec.deleteRegistryTree,
                    valueSpec.resolveClsidFromValueData);
                entryListOut->push_back(entry);
            }
        }
    }
}

void StartupDock::appendLogonEntries(std::vector<StartupEntry>* entryListOut)
{
    if (entryListOut == nullptr)
    {
        return;
    }

    for (const RunKeySpec& keySpec : buildRunKeySpecList())
    {
        const QString subKeyText = QString::fromWCharArray(keySpec.subKeyText);
        const auto valueRecordList = enumerateRegistryValues(keySpec.rootKey, subKeyText);
        for (const RegistryValueRecord& valueRecord : valueRecordList)
        {
            if (valueRecord.valueDataText.trimmed().isEmpty())
            {
                continue;
            }

            if (subKeyText.endsWith(QStringLiteral("\\Windows"), Qt::CaseInsensitive))
            {
                const QString lowerValueNameText = valueRecord.valueNameText.trimmed().toLower();
                if (lowerValueNameText != QStringLiteral("run") && lowerValueNameText != QStringLiteral("load"))
                {
                    continue;
                }
            }

            if (subKeyText.endsWith(QStringLiteral("Command Processor"), Qt::CaseInsensitive)
                && valueRecord.valueNameText.compare(QStringLiteral("Autorun"), Qt::CaseInsensitive) != 0)
            {
                continue;
            }

            appendValueBasedLogonEntry(entryListOut, keySpec, valueRecord);
        }
    }

    appendRunOnceExEntries(entryListOut);

    const QString currentUserStartupPath = QDir::toNativeSeparators(
        qEnvironmentVariable("APPDATA")
        + QStringLiteral("\\Microsoft\\Windows\\Start Menu\\Programs\\Startup"));
    const QString commonStartupPath = QDir::toNativeSeparators(
        qEnvironmentVariable("ProgramData")
        + QStringLiteral("\\Microsoft\\Windows\\Start Menu\\Programs\\Startup"));
    const std::array<std::pair<QString, QString>, 2> startupFolderList{ {
        { currentUserStartupPath, QStringLiteral("当前用户") },
        { commonStartupPath, QStringLiteral("本机") }
    } };

    for (const auto& folderPair : startupFolderList)
    {
        const QDir startupDir(folderPair.first);
        if (!startupDir.exists())
        {
            continue;
        }

        const QFileInfoList fileInfoList = startupDir.entryInfoList(
            QDir::Files | QDir::NoDotAndDotDot,
            QDir::Name | QDir::IgnoreCase);
        for (const QFileInfo& fileInfo : fileInfoList)
        {
            StartupEntry entry;
            entry.category = StartupCategory::Logon;
            entry.categoryText = categoryToText(entry.category);
            entry.itemNameText = fileInfo.fileName();
            entry.commandText = fileInfo.absoluteFilePath();
            entry.imagePathText = fileInfo.absoluteFilePath();
            entry.publisherText = queryPublisherTextByPath(entry.imagePathText);
            entry.locationText = folderPair.first;
            entry.locationGroupText.clear();
            entry.userText = folderPair.second;
            entry.sourceTypeText = QStringLiteral("StartupFolder");
            entry.detailText = QStringLiteral("开始菜单启动文件夹");
            entry.enabled = true;
            entry.canOpenFileLocation = true;
            entry.canDelete = true;
            entry.deleteRegistryTree = false;
            entry.uniqueIdText = QStringLiteral("STARTUPFOLDER|%1").arg(fileInfo.absoluteFilePath());
            entryListOut->push_back(entry);
        }
    }
}

void StartupDock::appendAdvancedRegistryEntries(std::vector<StartupEntry>* entryListOut)
{
    if (entryListOut == nullptr)
    {
        return;
    }

    appendSingleValueEntries(entryListOut);
    appendValueEnumEntries(entryListOut);
    appendSubKeyValueEntries(entryListOut);
}
