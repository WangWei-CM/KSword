#include "StartupDock.Internal.h"

using namespace startup_dock_detail;

namespace
{
    // WinsockKeySpec：
    // - 作用：描述 Winsock Catalog 相关注册表根位置；
    // - userText/detailText 用于 UI 展示上下文。
    struct WinsockKeySpec
    {
        HKEY rootKey = nullptr;              // rootKey：注册表根键。
        const wchar_t* subKeyText = L"";     // subKeyText：Catalog 根路径。
        const wchar_t* sourceTypeText = L""; // sourceTypeText：来源类型。
        const wchar_t* userText = L"";       // userText：上下文显示文本。
        const wchar_t* detailText = L"";     // detailText：该 Catalog 的说明文本。
    };

    // rootKeyToText：
    // - 作用：把根键句柄转换成缩写；
    // - 调用：构建 Winsock Catalog 的 locationText。
    QString rootKeyToText(HKEY rootKey)
    {
        if (rootKey == HKEY_LOCAL_MACHINE)
        {
            return QStringLiteral("HKLM");
        }
        return QStringLiteral("UNKNOWN");
    }

    // buildRegistryLocationText：
    // - 作用：生成 HKLM\... 形式的位置文本；
    // - 调用：Winsock Catalog 分组和叶子节点都复用。
    QString buildRegistryLocationText(HKEY rootKey, const QString& subKeyText)
    {
        return QStringLiteral("%1\\%2").arg(rootKeyToText(rootKey), subKeyText);
    }

    // enumerateRegistrySubKeys：
    // - 作用：枚举指定键下的全部一级子键名；
    // - 调用：遍历 Winsock Catalog_Entries* 下的各个条目。
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

    // enumerateRegistryValueTextList：
    // - 作用：把一个键下的全部值压缩成“值名=值内容”文本列表；
    // - 调用：Winsock Catalog 条目详情和命令列复用。
    QStringList enumerateRegistryValueTextList(HKEY rootKey, const QString& subKeyText)
    {
        QStringList valueTextList;
        HKEY openedKey = nullptr;
        const LONG openResult = ::RegOpenKeyExW(
            rootKey,
            reinterpret_cast<LPCWSTR>(subKeyText.utf16()),
            0,
            KEY_QUERY_VALUE,
            &openedKey);
        if (openResult != ERROR_SUCCESS || openedKey == nullptr)
        {
            return valueTextList;
        }

        DWORD valueIndex = 0;
        while (true)
        {
            wchar_t valueNameBuffer[1024] = {};
            DWORD valueNameChars = static_cast<DWORD>(std::size(valueNameBuffer));
            DWORD valueType = REG_NONE;
            DWORD dataBytes = 0;
            const LONG headerResult = ::RegEnumValueW(
                openedKey,
                valueIndex,
                valueNameBuffer,
                &valueNameChars,
                nullptr,
                &valueType,
                nullptr,
                &dataBytes);
            if (headerResult == ERROR_NO_MORE_ITEMS)
            {
                break;
            }
            if (headerResult != ERROR_SUCCESS)
            {
                ++valueIndex;
                continue;
            }

            std::vector<std::uint8_t> rawBuffer(static_cast<std::size_t>(dataBytes == 0 ? 2 : dataBytes));
            valueNameChars = static_cast<DWORD>(std::size(valueNameBuffer));
            const LONG dataResult = ::RegEnumValueW(
                openedKey,
                valueIndex,
                valueNameBuffer,
                &valueNameChars,
                nullptr,
                &valueType,
                rawBuffer.data(),
                &dataBytes);
            if (dataResult == ERROR_SUCCESS)
            {
                QString valueDataText;
                if (valueType == REG_SZ || valueType == REG_EXPAND_SZ)
                {
                    valueDataText = QString::fromWCharArray(reinterpret_cast<const wchar_t*>(rawBuffer.data())).trimmed();
                }
                else if (valueType == REG_DWORD && rawBuffer.size() >= sizeof(DWORD))
                {
                    valueDataText = QString::number(*reinterpret_cast<const DWORD*>(rawBuffer.data()));
                }
                else
                {
                    valueDataText = QStringLiteral("<%1 bytes>").arg(rawBuffer.size());
                }

                const QString valueNameText = QString::fromWCharArray(valueNameBuffer, static_cast<int>(valueNameChars));
                valueTextList << QStringLiteral("%1=%2")
                    .arg(valueNameText.trimmed().isEmpty() ? QStringLiteral("(默认值)") : valueNameText)
                    .arg(valueDataText);
            }
            ++valueIndex;
        }

        ::RegCloseKey(openedKey);
        return valueTextList;
    }

    // buildWinsockKeySpecList：
    // - 作用：集中定义要显示的 Winsock Catalog 注册表位置；
    // - 调用：appendWinsockEntries 循环使用。
    const std::array<WinsockKeySpec, 4>& buildWinsockKeySpecList()
    {
        static const std::array<WinsockKeySpec, 4> winsockKeySpecList{ {
            { HKEY_LOCAL_MACHINE, L"System\\CurrentControlSet\\Services\\WinSock2\\Parameters\\Protocol_Catalog9\\Catalog_Entries", L"Winsock-ProtocolCatalog", L"本机", L"Winsock Protocol Catalog" },
            { HKEY_LOCAL_MACHINE, L"System\\CurrentControlSet\\Services\\WinSock2\\Parameters\\Protocol_Catalog9\\Catalog_Entries64", L"Winsock-ProtocolCatalog64", L"本机", L"Winsock 64 位 Protocol Catalog" },
            { HKEY_LOCAL_MACHINE, L"System\\CurrentControlSet\\Services\\WinSock2\\Parameters\\NameSpace_Catalog5\\Catalog_Entries", L"Winsock-NameSpaceCatalog", L"本机", L"Winsock NameSpace Catalog" },
            { HKEY_LOCAL_MACHINE, L"System\\CurrentControlSet\\Services\\WinSock2\\Parameters\\NameSpace_Catalog5\\Catalog_Entries64", L"Winsock-NameSpaceCatalog64", L"本机", L"Winsock 64 位 NameSpace Catalog" }
        } };
        return winsockKeySpecList;
    }
}

void StartupDock::appendWinsockEntries(std::vector<StartupEntry>* entryListOut)
{
    if (entryListOut == nullptr)
    {
        return;
    }

    for (const WinsockKeySpec& keySpec : buildWinsockKeySpecList())
    {
        const QString rootSubKeyText = QString::fromWCharArray(keySpec.subKeyText);
        const QString groupLocationText = buildRegistryLocationText(keySpec.rootKey, rootSubKeyText);
        const QStringList subKeyNameList = enumerateRegistrySubKeys(keySpec.rootKey, rootSubKeyText);
        for (const QString& subKeyNameText : subKeyNameList)
        {
            const QString itemSubKeyText = rootSubKeyText + QLatin1Char('\\') + subKeyNameText;
            const QStringList valueTextList = enumerateRegistryValueTextList(keySpec.rootKey, itemSubKeyText);

            StartupEntry entry;
            entry.category = StartupCategory::Registry;
            entry.categoryText = categoryToText(entry.category);
            entry.itemNameText = QStringLiteral("Winsock 项 %1").arg(subKeyNameText);
            entry.publisherText.clear();
            entry.imagePathText.clear();
            entry.commandText = valueTextList.join(QStringLiteral("；"));
            entry.locationText = buildRegistryLocationText(keySpec.rootKey, itemSubKeyText);
            entry.locationGroupText = groupLocationText;
            entry.registryValueNameText.clear();
            entry.userText = QString::fromWCharArray(keySpec.userText);
            entry.detailText = QStringLiteral("%1；键值数量=%2")
                .arg(QString::fromWCharArray(keySpec.detailText))
                .arg(valueTextList.size());
            entry.sourceTypeText = QString::fromWCharArray(keySpec.sourceTypeText);
            entry.enabled = true;
            entry.canOpenFileLocation = false;
            entry.canOpenRegistryLocation = true;
            entry.canDelete = false;
            entry.deleteRegistryTree = false;
            entry.uniqueIdText = QStringLiteral("WINSOCK|%1").arg(entry.locationText);
            entryListOut->push_back(entry);
        }
    }
}
