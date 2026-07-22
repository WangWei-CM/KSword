#include "RegistryOptimizationPage.h"
#include "../UI/VisibleTableWidget.h"

#include "../theme.h"
#include "../UI/TableColumnAutoFit.h"
#include "../ksword/profile/ProfileJsonLoader.h"

#include <QAbstractItemView>
#include <QAction>
#include <QCheckBox>
#include <QColorDialog>
#include <QComboBox>
#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QHeaderView>
#include <QHBoxLayout>
#include <QHash>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonParseError>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QMessageBox>
#include <QMetaObject>
#include <QOperatingSystemVersion>
#include <QPointer>
#include <QProcess>
#include <QPushButton>
#include <QRegularExpression>
#include <QSignalBlocker>
#include <QSize>
#include <QSizePolicy>
#include <QSplitter>
#include <QTableWidget>
#include <QTextEdit>
#include <QTemporaryDir>
#include <QTimer>
#include <QTreeWidget>
#include <QVBoxLayout>

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <thread>

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#include <Shellapi.h>
#include <ShlObj.h>

namespace
{
    constexpr const char* kOptimizationProfileFileName = "registry_optimization_items.json";
    constexpr int kRoleGroupName = Qt::UserRole + 1;
    constexpr int kItemNameColumn = 0;
    constexpr int kScopeColumn = 1;
    constexpr int kTypeColumn = 2;
    constexpr int kCurrentStateColumn = 3;
    constexpr int kTargetControlColumn = 4;
    constexpr int kActionButtonColumn = 5;
    constexpr int kConditionWarningColumn = 6;
    constexpr int kTargetColumnWidth = 132;
    constexpr int kActionColumnWidth = 82;
    constexpr int kDefaultRowHeight = 30;
    constexpr int kFilterDebounceMs = 200;

    struct VisibleStateRefreshResult
    {
        int tableRow = -1;
        QString stateText;
        QString targetLabel;
        bool targetEnabled = false;
    };

    // jsonString:
    // - Input object/name: JSON object and property name;
    // - Processing: converts scalar JSON values to trimmed QString text;
    // - Return: fallback when the property is missing or unsupported.
    QString jsonString(const QJsonObject& object, const QString& name, const QString& fallback = QString())
    {
        const QJsonValue value = object.value(name);
        if (value.isString()) return value.toString().trimmed();
        if (value.isDouble()) return QString::number(value.toDouble(), 'f', 0).trimmed();
        if (value.isBool()) return value.toBool() ? QStringLiteral("True") : QStringLiteral("False");
        return fallback;
    }

    // buildCenteredCellWidget:
    // - Input child/parent: the real editor button/checkbox/combobox and its table parent;
    // - Processing: wraps the child in a transparent QWidget with a centered HBox layout;
    // - Return: container widget for QTableWidget::setCellWidget, while child remains discoverable by findChild.
    QWidget* buildCenteredCellWidget(QWidget* child, QWidget* parent)
    {
        QWidget* container = new QWidget(parent);
        container->setAutoFillBackground(false);
        QHBoxLayout* layout = new QHBoxLayout(container);
        layout->setContentsMargins(4, 1, 4, 1);
        layout->setSpacing(0);
        layout->addStretch(1);
        layout->addWidget(child, 0, Qt::AlignCenter);
        layout->addStretch(1);
        child->setParent(container);
        return container;
    }

    // findTargetComboBox:
    // - 输入：目标选项单元格控件，可能是直接控件，也可能是居中容器；
    // - 处理：优先直接转换，失败后按对象名在子控件中查找；
    // - 返回：目标下拉框指针，找不到返回 nullptr。
    QComboBox* findTargetComboBox(QWidget* cellWidget)
    {
        if (cellWidget == nullptr)
        {
            return nullptr;
        }
        if (QComboBox* comboBox = qobject_cast<QComboBox*>(cellWidget))
        {
            return comboBox;
        }
        return cellWidget->findChild<QComboBox*>(QStringLiteral("optimizationTargetCombo"));
    }

    // findTargetCheckBox:
    // - 输入：目标选项单元格控件，可能是直接控件，也可能是居中容器；
    // - 处理：优先直接转换，失败后按对象名在子控件中查找；
    // - 返回：目标复选框指针，找不到返回 nullptr。
    QCheckBox* findTargetCheckBox(QWidget* cellWidget)
    {
        if (cellWidget == nullptr)
        {
            return nullptr;
        }
        if (QCheckBox* checkBox = qobject_cast<QCheckBox*>(cellWidget))
        {
            return checkBox;
        }
        return cellWidget->findChild<QCheckBox*>(QStringLiteral("optimizationTargetCheck"));
    }

    // jsonInt:
    // - Input object/name: JSON object and property name;
    // - Processing: reads numeric or string integer fields;
    // - Return: fallback on missing/invalid data.
    int jsonInt(const QJsonObject& object, const QString& name, const int fallback = 0)
    {
        const QJsonValue value = object.value(name);
        if (value.isDouble()) return value.toInt(fallback);
        if (value.isString())
        {
            bool ok = false;
            const int parsed = value.toString().trimmed().toInt(&ok);
            return ok ? parsed : fallback;
        }
        return fallback;
    }

    // scopeDisplayText:
    // - Input scopeText: Dism++ scope identifier;
    // - Processing: maps known scope identifiers to Chinese display text;
    // - Return: readable scope name while preserving unknown values.
    QString scopeDisplayText(const QString& scopeText)
    {
        if (scopeText.compare(QStringLiteral("Current"), Qt::CaseInsensitive) == 0) return QStringLiteral("当前用户");
        if (scopeText.compare(QStringLiteral("Default"), Qt::CaseInsensitive) == 0) return QStringLiteral("默认用户");
        if (scopeText.compare(QStringLiteral("System"), Qt::CaseInsensitive) == 0) return QStringLiteral("系统");
        return scopeText.isEmpty() ? QStringLiteral("未指定") : scopeText;
    }

    // trimDefaultValueName:
    // - Input valueName: UI/JSON value name;
    // - Processing: maps empty and "(默认)" to WinAPI default-value nullptr;
    // - Return: normalized registry value name.
    QString trimDefaultValueName(const QString& valueName)
    {
        const QString trimmed = valueName.trimmed();
        if (trimmed.isEmpty() || trimmed == QStringLiteral("(默认)")) return QString();
        return trimmed;
    }

    // winErrorText:
    // - Input errorCode: Win32 LSTATUS/GetLastError value;
    // - Processing: formats system error text through FormatMessageW;
    // - Return: localized diagnostic string.
    QString winErrorText(const LONG errorCode)
    {
        wchar_t* buffer = nullptr;
        const DWORD size = ::FormatMessageW(
            FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
            nullptr,
            static_cast<DWORD>(errorCode),
            0,
            reinterpret_cast<LPWSTR>(&buffer),
            0,
            nullptr);

        QString text = QStringLiteral("错误码 %1").arg(errorCode);
        if (size > 0 && buffer != nullptr)
        {
            text += QStringLiteral(": ") + QString::fromWCharArray(buffer, static_cast<int>(size)).trimmed();
        }
        if (buffer != nullptr) ::LocalFree(buffer);
        return text;
    }

    // parseRegistryPath:
    // - Input pathText: HKEY_* or HK* registry path;
    // - Processing: normalizes separators and resolves the root HKEY;
    // - Return: true with root/subpath on valid input, false otherwise.
    bool parseRegistryPath(const QString& pathText, HKEY* rootKeyOut, QString* subPathOut)
    {
        if (rootKeyOut == nullptr || subPathOut == nullptr) return false;

        QString text = pathText.trimmed();
        text.replace('/', '\\');
        while (text.contains(QStringLiteral("\\\\"))) text.replace(QStringLiteral("\\\\"), QStringLiteral("\\"));
        if (text.endsWith('\\')) text.chop(1);
        if (text.isEmpty()) return false;

        const int splitIndex = text.indexOf('\\');
        const QString rootText = splitIndex < 0 ? text : text.left(splitIndex);
        const QString subPath = splitIndex < 0 ? QString() : text.mid(splitIndex + 1);

        struct RootName
        {
            const wchar_t* fullName;
            const wchar_t* shortName;
            HKEY root;
        };
        static const std::array<RootName, 5> kRootNames{ {
            { L"HKEY_CLASSES_ROOT", L"HKCR", HKEY_CLASSES_ROOT },
            { L"HKEY_CURRENT_USER", L"HKCU", HKEY_CURRENT_USER },
            { L"HKEY_LOCAL_MACHINE", L"HKLM", HKEY_LOCAL_MACHINE },
            { L"HKEY_USERS", L"HKU", HKEY_USERS },
            { L"HKEY_CURRENT_CONFIG", L"HKCC", HKEY_CURRENT_CONFIG },
        } };

        for (const RootName& entry : kRootNames)
        {
            if (rootText.compare(QString::fromWCharArray(entry.fullName), Qt::CaseInsensitive) == 0 ||
                rootText.compare(QString::fromWCharArray(entry.shortName), Qt::CaseInsensitive) == 0)
            {
                *rootKeyOut = entry.root;
                *subPathOut = subPath;
                return true;
            }
        }
        return false;
    }

    // actionRegistryViewFlags:
    // - Input actionObject: one action JSON object;
    // - Processing: maps Wow64=True to KEY_WOW64_32KEY for 32-bit registry view operations;
    // - Return: extra REGSAM flags to OR into RegOpen/Create access masks.
    REGSAM actionRegistryViewFlags(const QJsonObject& actionObject)
    {
        const QString wow64Text = jsonString(actionObject, QStringLiteral("Wow64"));
        return wow64Text.compare(QStringLiteral("True"), Qt::CaseInsensitive) == 0 ? KEY_WOW64_32KEY : 0;
    }

    // parseRegistryType:
    // - Input typeText: REG_* string from JSON;
    // - Processing: maps supported registry types to WinAPI constants;
    // - Return: true when type is supported and stored in typeOut.
    bool parseRegistryType(const QString& typeText, DWORD* typeOut)
    {
        if (typeOut == nullptr) return false;
        const QString normalized = typeText.trimmed().toUpper();
        if (normalized == QStringLiteral("REG_SZ")) { *typeOut = REG_SZ; return true; }
        if (normalized == QStringLiteral("REG_EXPAND_SZ")) { *typeOut = REG_EXPAND_SZ; return true; }
        if (normalized == QStringLiteral("REG_BINARY")) { *typeOut = REG_BINARY; return true; }
        if (normalized == QStringLiteral("REG_DWORD")) { *typeOut = REG_DWORD; return true; }
        if (normalized == QStringLiteral("REG_QWORD")) { *typeOut = REG_QWORD; return true; }
        if (normalized == QStringLiteral("REG_MULTI_SZ")) { *typeOut = REG_MULTI_SZ; return true; }
        if (normalized == QStringLiteral("REG_NONE")) { *typeOut = REG_NONE; return true; }
        return false;
    }

    // parseHexInteger:
    // - Input text: Dism++ numeric text, usually hex without 0x for DWORD/QWORD;
    // - Processing: strips whitespace and parses base 16 unless 0x is present;
    // - Return: true and parsed value when valid.
    bool parseHexInteger(const QString& text, quint64* valueOut)
    {
        if (valueOut == nullptr) return false;
        QString normalized = text.trimmed();
        if (normalized.startsWith(QStringLiteral("0x"), Qt::CaseInsensitive))
        {
            normalized = normalized.mid(2);
        }
        bool ok = false;
        const quint64 value = normalized.toULongLong(&ok, 16);
        if (!ok) return false;
        *valueOut = value;
        return true;
    }

    // hexStringToBytes:
    // - Input text: contiguous or separated hex byte string;
    // - Processing: removes non-hex separators and converts pairs to bytes;
    // - Return: true with raw bytes when input has even hex length.
    bool hexStringToBytes(const QString& text, QByteArray* bytesOut)
    {
        if (bytesOut == nullptr) return false;
        QString hexText = text.trimmed();
        hexText.remove(QRegularExpression(QStringLiteral("[^0-9A-Fa-f]")));
        if (hexText.isEmpty())
        {
            bytesOut->clear();
            return true;
        }
        if ((hexText.size() % 2) != 0)
        {
            hexText.prepend(QLatin1Char('0'));
        }
        QByteArray output;
        output.reserve(hexText.size() / 2);
        for (int index = 0; index < hexText.size(); index += 2)
        {
            bool ok = false;
            const unsigned int byteValue = hexText.mid(index, 2).toUInt(&ok, 16);
            if (!ok) return false;
            output.append(static_cast<char>(byteValue & 0xFFU));
        }
        *bytesOut = output;
        return true;
    }

    // writeUnsignedLittleEndian:
    // - Input value/byteCount: integer value and output width;
    // - Processing: serializes to Windows little-endian registry byte order;
    // - Return: QByteArray containing byteCount bytes.
    QByteArray writeUnsignedLittleEndian(const quint64 value, const int byteCount)
    {
        QByteArray output(byteCount, 0);
        for (int index = 0; index < byteCount; ++index)
        {
            output[index] = static_cast<char>((value >> (index * 8)) & 0xFFU);
        }
        return output;
    }

    // readUnsignedLittleEndian:
    // - Input bytes/byteCount: raw registry data and number of bytes to consume;
    // - Processing: decodes little-endian unsigned integer;
    // - Return: decoded value, or 0 when data is shorter than requested.
    quint64 readUnsignedLittleEndian(const QByteArray& bytes, const int byteCount)
    {
        if (bytes.size() < byteCount) return 0;
        quint64 value = 0;
        for (int index = 0; index < byteCount; ++index)
        {
            value |= (static_cast<quint64>(static_cast<unsigned char>(bytes.at(index))) << (index * 8));
        }
        return value;
    }

    // stringToUtf16RegistryData:
    // - Input text: REG_SZ/REG_EXPAND_SZ text;
    // - Processing: stores UTF-16LE including trailing NUL;
    // - Return: raw registry data.
    QByteArray stringToUtf16RegistryData(const QString& text)
    {
        QByteArray output;
        output.resize((text.size() + 1) * static_cast<int>(sizeof(wchar_t)));
        if (!text.isEmpty())
        {
            std::memcpy(output.data(), text.utf16(), text.size() * static_cast<int>(sizeof(wchar_t)));
        }
        output[output.size() - 2] = '\0';
        output[output.size() - 1] = '\0';
        return output;
    }

    // registryDataFromJsonText:
    // - Input type/dataText: registry type and Dism++ Data text;
    // - Processing: converts JSON text into raw WinAPI data bytes;
    // - Return: true on supported conversion, false with errorTextOut otherwise.
    bool registryDataFromJsonText(
        const DWORD type,
        const QString& dataText,
        QByteArray* rawDataOut,
        QString* errorTextOut)
    {
        if (rawDataOut == nullptr) return false;
        rawDataOut->clear();
        if (errorTextOut != nullptr) errorTextOut->clear();

        if (type == REG_DWORD)
        {
            quint64 value = 0;
            if (!parseHexInteger(dataText, &value) || value > 0xFFFFFFFFULL)
            {
                if (errorTextOut != nullptr) *errorTextOut = QStringLiteral("REG_DWORD 数据不是有效十六进制数：%1").arg(dataText);
                return false;
            }
            *rawDataOut = writeUnsignedLittleEndian(value, 4);
            return true;
        }
        if (type == REG_QWORD)
        {
            quint64 value = 0;
            if (!parseHexInteger(dataText, &value))
            {
                if (errorTextOut != nullptr) *errorTextOut = QStringLiteral("REG_QWORD 数据不是有效十六进制数：%1").arg(dataText);
                return false;
            }
            *rawDataOut = writeUnsignedLittleEndian(value, 8);
            return true;
        }
        if (type == REG_BINARY || type == REG_NONE)
        {
            if (!hexStringToBytes(dataText, rawDataOut))
            {
                if (errorTextOut != nullptr) *errorTextOut = QStringLiteral("REG_BINARY 数据不是有效十六进制字节串：%1").arg(dataText);
                return false;
            }
            return true;
        }
        if (type == REG_SZ || type == REG_EXPAND_SZ)
        {
            *rawDataOut = stringToUtf16RegistryData(dataText);
            return true;
        }
        if (type == REG_MULTI_SZ)
        {
            const QStringList lines = dataText.split(QLatin1Char('|'), Qt::SkipEmptyParts);
            QByteArray output;
            for (const QString& line : lines)
            {
                const QByteArray one = stringToUtf16RegistryData(line.trimmed());
                output.append(one.constData(), one.size() - static_cast<int>(sizeof(wchar_t)));
            }
            output.append('\0');
            output.append('\0');
            *rawDataOut = output;
            return true;
        }

        if (errorTextOut != nullptr) *errorTextOut = QStringLiteral("暂不支持写入注册表类型：%1").arg(type);
        return false;
    }

    // readRegistryValue:
    // - Input keyPath/valueName/accessFlags: registry key, value, and view flags;
    // - Processing: opens the key and queries the raw value;
    // - Return: true with type/data when the value exists and can be read.
    bool readRegistryValue(
        const QString& keyPath,
        const QString& valueName,
        const REGSAM accessFlags,
        DWORD* typeOut,
        QByteArray* dataOut,
        QString* errorTextOut)
    {
        if (typeOut == nullptr || dataOut == nullptr) return false;
        if (errorTextOut != nullptr) errorTextOut->clear();

        HKEY root = nullptr;
        QString subPath;
        if (!parseRegistryPath(keyPath, &root, &subPath))
        {
            if (errorTextOut != nullptr) *errorTextOut = QStringLiteral("注册表路径无效：%1").arg(keyPath);
            return false;
        }

        HKEY key = nullptr;
        const LONG openResult = ::RegOpenKeyExW(
            root,
            subPath.isEmpty() ? nullptr : reinterpret_cast<const wchar_t*>(subPath.utf16()),
            0,
            KEY_QUERY_VALUE | accessFlags,
            &key);
        if (openResult != ERROR_SUCCESS)
        {
            if (errorTextOut != nullptr) *errorTextOut = winErrorText(openResult);
            return false;
        }

        const QString realValueName = trimDefaultValueName(valueName);
        const wchar_t* valueNamePtr = realValueName.isEmpty() ? nullptr : reinterpret_cast<const wchar_t*>(realValueName.utf16());
        DWORD type = REG_NONE;
        DWORD dataBytes = 0;
        LONG queryResult = ::RegQueryValueExW(key, valueNamePtr, nullptr, &type, nullptr, &dataBytes);
        if (queryResult != ERROR_SUCCESS)
        {
            ::RegCloseKey(key);
            if (errorTextOut != nullptr) *errorTextOut = winErrorText(queryResult);
            return false;
        }

        QByteArray rawData(static_cast<int>(dataBytes), 0);
        queryResult = ::RegQueryValueExW(
            key,
            valueNamePtr,
            nullptr,
            &type,
            reinterpret_cast<LPBYTE>(rawData.data()),
            &dataBytes);
        ::RegCloseKey(key);
        if (queryResult != ERROR_SUCCESS)
        {
            if (errorTextOut != nullptr) *errorTextOut = winErrorText(queryResult);
            return false;
        }

        rawData.resize(static_cast<int>(dataBytes));
        *typeOut = type;
        *dataOut = rawData;
        return true;
    }

    // writeRegistryValue:
    // - Input key/value/type/data/accessFlags: target registry value and raw bytes;
    // - Processing: creates the key if needed, then writes value data;
    // - Return: true when RegSetValueExW succeeds.
    bool writeRegistryValue(
        const QString& keyPath,
        const QString& valueName,
        const DWORD type,
        const QByteArray& rawData,
        const REGSAM accessFlags,
        QString* errorTextOut)
    {
        if (errorTextOut != nullptr) errorTextOut->clear();
        HKEY root = nullptr;
        QString subPath;
        if (!parseRegistryPath(keyPath, &root, &subPath))
        {
            if (errorTextOut != nullptr) *errorTextOut = QStringLiteral("注册表路径无效：%1").arg(keyPath);
            return false;
        }

        HKEY key = nullptr;
        const LONG createResult = ::RegCreateKeyExW(
            root,
            subPath.isEmpty() ? nullptr : reinterpret_cast<const wchar_t*>(subPath.utf16()),
            0,
            nullptr,
            REG_OPTION_NON_VOLATILE,
            KEY_SET_VALUE | KEY_CREATE_SUB_KEY | accessFlags,
            nullptr,
            &key,
            nullptr);
        if (createResult != ERROR_SUCCESS)
        {
            if (errorTextOut != nullptr) *errorTextOut = winErrorText(createResult);
            return false;
        }

        const QString realValueName = trimDefaultValueName(valueName);
        const wchar_t* valueNamePtr = realValueName.isEmpty() ? nullptr : reinterpret_cast<const wchar_t*>(realValueName.utf16());
        const LONG setResult = ::RegSetValueExW(
            key,
            valueNamePtr,
            0,
            type,
            reinterpret_cast<const BYTE*>(rawData.constData()),
            static_cast<DWORD>(rawData.size()));
        ::RegCloseKey(key);
        if (setResult != ERROR_SUCCESS)
        {
            if (errorTextOut != nullptr) *errorTextOut = winErrorText(setResult);
            return false;
        }
        return true;
    }

    // deleteRegistryValueOrKey:
    // - Input keyPath/valueName/accessFlags: target key or value;
    // - Processing: deletes a value when valueName is present, otherwise deletes the key tree;
    // - Return: true when the delete operation succeeds.
    bool deleteRegistryValueOrKey(
        const QString& keyPath,
        const QString& valueName,
        const bool hasValueName,
        const REGSAM accessFlags,
        QString* errorTextOut)
    {
        if (errorTextOut != nullptr) errorTextOut->clear();
        HKEY root = nullptr;
        QString subPath;
        if (!parseRegistryPath(keyPath, &root, &subPath))
        {
            if (errorTextOut != nullptr) *errorTextOut = QStringLiteral("注册表路径无效：%1").arg(keyPath);
            return false;
        }

        if (hasValueName)
        {
            HKEY key = nullptr;
            const LONG openResult = ::RegOpenKeyExW(
                root,
                subPath.isEmpty() ? nullptr : reinterpret_cast<const wchar_t*>(subPath.utf16()),
                0,
                KEY_SET_VALUE | accessFlags,
                &key);
            if (openResult != ERROR_SUCCESS)
            {
                if (errorTextOut != nullptr) *errorTextOut = winErrorText(openResult);
                return false;
            }
            const QString realValueName = trimDefaultValueName(valueName);
            const wchar_t* valueNamePtr = realValueName.isEmpty() ? nullptr : reinterpret_cast<const wchar_t*>(realValueName.utf16());
            const LONG deleteResult = ::RegDeleteValueW(key, valueNamePtr);
            ::RegCloseKey(key);
            if (deleteResult != ERROR_SUCCESS)
            {
                if (errorTextOut != nullptr) *errorTextOut = winErrorText(deleteResult);
                return false;
            }
            return true;
        }

        if (subPath.isEmpty())
        {
            if (errorTextOut != nullptr) *errorTextOut = QStringLiteral("拒绝删除注册表根键。");
            return false;
        }

        const LONG deleteResult = ::RegDeleteTreeW(root, reinterpret_cast<const wchar_t*>(subPath.utf16()));
        if (deleteResult != ERROR_SUCCESS)
        {
            if (errorTextOut != nullptr) *errorTextOut = winErrorText(deleteResult);
            return false;
        }
        return true;
    }

    // shouldSkipActionError:
    // - Input actionObject: action JSON carrying optional SkipError field;
    // - Processing: Dism++ uses SkipError=2 for missing/expected failures;
    // - Return: true when this action error should be reported as skipped.
    bool shouldSkipActionError(const QJsonObject& actionObject)
    {
        return jsonString(actionObject, QStringLiteral("SkipError")).trimmed() == QStringLiteral("2");
    }

    // expandEnvironmentPath:
    // - Input pathText: path containing Windows environment variables;
    // - Processing: calls ExpandEnvironmentStringsW and normalizes separators;
    // - Return: expanded path, or original text if expansion fails.
    QString expandEnvironmentPath(const QString& pathText)
    {
        const QString trimmedPath = pathText.trimmed();
        if (trimmedPath.isEmpty()) return QString();
        DWORD requiredChars = ::ExpandEnvironmentStringsW(
            reinterpret_cast<const wchar_t*>(trimmedPath.utf16()),
            nullptr,
            0);
        if (requiredChars == 0) return QDir::toNativeSeparators(trimmedPath);

        QString expanded;
        expanded.resize(static_cast<int>(requiredChars));
        const DWORD writtenChars = ::ExpandEnvironmentStringsW(
            reinterpret_cast<const wchar_t*>(trimmedPath.utf16()),
            reinterpret_cast<wchar_t*>(expanded.data()),
            requiredChars);
        if (writtenChars == 0) return QDir::toNativeSeparators(trimmedPath);
        while (expanded.endsWith(QChar::Null)) expanded.chop(1);
        return QDir::toNativeSeparators(expanded);
    }

    // splitZipFileReference:
    // - Input zipReference: JSON ZIPFile text such as Config\Data.zip/SwapMouse.exe;
    // - Processing: separates relative zip path from inner entry path;
    // - Return: true when both pieces are available.
    bool splitZipFileReference(const QString& zipReference, QString* zipRelativePathOut, QString* entryPathOut)
    {
        if (zipRelativePathOut == nullptr || entryPathOut == nullptr) return false;
        QString normalized = zipReference.trimmed();
        normalized.replace('\\', '/');
        const int zipSuffixIndex = normalized.indexOf(QStringLiteral(".zip"), 0, Qt::CaseInsensitive);
        if (zipSuffixIndex < 0) return false;
        const int zipEndIndex = zipSuffixIndex + 4;
        QString zipRelativePath = normalized.left(zipEndIndex);
        QString entryPath = normalized.mid(zipEndIndex);
        while (entryPath.startsWith('/')) entryPath.remove(0, 1);
        if (zipRelativePath.isEmpty() || entryPath.isEmpty()) return false;
        *zipRelativePathOut = QDir::fromNativeSeparators(zipRelativePath);
        *entryPathOut = QDir::fromNativeSeparators(entryPath);
        return true;
    }

    // registryOptimizationAssetCandidates:
    // - Input zipRelativePath: relative path inside registry_optimization_assets;
    // - Processing: searches runtime profiles first, then source-tree profiles for development;
    // - Return: candidate zip paths in priority order.
    QStringList registryOptimizationAssetCandidates(const QString& zipRelativePath)
    {
        QStringList candidates;
        const QString normalizedZipPath = QDir::fromNativeSeparators(zipRelativePath);
        const QString appDir = QCoreApplication::applicationDirPath();
        const QString currentDir = QDir::currentPath();
        candidates << QDir(appDir).filePath(QStringLiteral("profiles/registry_optimization_assets/%1").arg(normalizedZipPath));
        candidates << QDir(appDir).filePath(QStringLiteral("../profiles/registry_optimization_assets/%1").arg(normalizedZipPath));
        candidates << QDir(currentDir).filePath(QStringLiteral("profiles/registry_optimization_assets/%1").arg(normalizedZipPath));
        candidates << QDir(currentDir).filePath(QStringLiteral("Ksword5.1/Ksword5.1/profiles/registry_optimization_assets/%1").arg(normalizedZipPath));
        candidates.removeDuplicates();
        return candidates;
    }

    // splitRegistryParent:
    // - Input keyPath: normalized HKEY path;
    // - Processing: separates parent key and leaf key name;
    // - Return: true when keyPath has a parent segment.
    bool splitRegistryParent(const QString& keyPath, QString* parentPathOut, QString* leafNameOut)
    {
        if (parentPathOut == nullptr || leafNameOut == nullptr) return false;
        QString normalized = keyPath.trimmed();
        normalized.replace('/', '\\');
        if (normalized.endsWith('\\')) normalized.chop(1);
        const int splitIndex = normalized.lastIndexOf('\\');
        if (splitIndex <= 0) return false;
        *parentPathOut = normalized.left(splitIndex);
        *leafNameOut = normalized.mid(splitIndex + 1);
        return !parentPathOut->isEmpty() && !leafNameOut->isEmpty();
    }

    // renameRegistryKeySameParent:
    // - Input oldKeyPath/newKeyPath/accessFlags: source and target key path;
    // - Processing: calls RegRenameKey when both keys share the same parent;
    // - Return: true on successful key rename.
    bool renameRegistryKeySameParent(
        const QString& oldKeyPath,
        const QString& newKeyPath,
        const REGSAM accessFlags,
        QString* errorTextOut)
    {
        QString oldParent;
        QString oldLeaf;
        QString newParent;
        QString newLeaf;
        if (!splitRegistryParent(oldKeyPath, &oldParent, &oldLeaf) ||
            !splitRegistryParent(newKeyPath, &newParent, &newLeaf) ||
            oldParent.compare(newParent, Qt::CaseInsensitive) != 0)
        {
            if (errorTextOut != nullptr) *errorTextOut = QStringLiteral("仅支持同父注册表键重命名：%1 -> %2").arg(oldKeyPath, newKeyPath);
            return false;
        }

        HKEY parentRoot = nullptr;
        QString parentSubPath;
        if (!parseRegistryPath(oldParent, &parentRoot, &parentSubPath))
        {
            if (errorTextOut != nullptr) *errorTextOut = QStringLiteral("父注册表路径无效：%1").arg(oldParent);
            return false;
        }

        HKEY parentKey = nullptr;
        const LONG openResult = ::RegOpenKeyExW(
            parentRoot,
            parentSubPath.isEmpty() ? nullptr : reinterpret_cast<const wchar_t*>(parentSubPath.utf16()),
            0,
            KEY_WRITE | accessFlags,
            &parentKey);
        if (openResult != ERROR_SUCCESS)
        {
            if (errorTextOut != nullptr) *errorTextOut = winErrorText(openResult);
            return false;
        }

        using RegRenameKeyFunc = LSTATUS(WINAPI*)(HKEY, LPCWSTR, LPCWSTR);
        const auto renameKey = reinterpret_cast<RegRenameKeyFunc>(::GetProcAddress(::GetModuleHandleW(L"Advapi32.dll"), "RegRenameKey"));
        if (renameKey == nullptr)
        {
            ::RegCloseKey(parentKey);
            if (errorTextOut != nullptr) *errorTextOut = QStringLiteral("当前系统不支持 RegRenameKey。");
            return false;
        }

        const LONG renameResult = renameKey(
            parentKey,
            reinterpret_cast<const wchar_t*>(oldLeaf.utf16()),
            reinterpret_cast<const wchar_t*>(newLeaf.utf16()));
        ::RegCloseKey(parentKey);
        if (renameResult != ERROR_SUCCESS)
        {
            if (errorTextOut != nullptr) *errorTextOut = winErrorText(renameResult);
            return false;
        }
        return true;
    }

    // parseFunctionParameters:
    // - Input text: semicolon-delimited Dism++ function argument body;
    // - Processing: splits Key=Value pairs and keeps case-insensitive keys in lower case;
    // - Return: parameter map for RegExist/QueryServiceStart/OSVersion evaluation.
    QHash<QString, QString> parseFunctionParameters(const QString& text)
    {
        QHash<QString, QString> parameters;
        const QStringList parts = text.split(QLatin1Char(';'), Qt::SkipEmptyParts);
        for (const QString& part : parts)
        {
            const int equalIndex = part.indexOf('=');
            if (equalIndex < 0) continue;
            const QString key = part.left(equalIndex).trimmed().toLower();
            const QString value = part.mid(equalIndex + 1).trimmed();
            parameters.insert(key, value);
        }
        return parameters;
    }

    // compareRawRegistryData:
    // - Input actual/expected data and type;
    // - Processing: trims REG_SZ trailing NULs and compares binary data exactly otherwise;
    // - Return: true when current registry value equals expected JSON state.
    bool compareRawRegistryData(const DWORD type, const QByteArray& actualData, const QByteArray& expectedData)
    {
        if (type == REG_SZ || type == REG_EXPAND_SZ || type == REG_MULTI_SZ)
        {
            QString actual = QString::fromWCharArray(
                reinterpret_cast<const wchar_t*>(actualData.constData()),
                actualData.size() / static_cast<int>(sizeof(wchar_t)));
            QString expected = QString::fromWCharArray(
                reinterpret_cast<const wchar_t*>(expectedData.constData()),
                expectedData.size() / static_cast<int>(sizeof(wchar_t)));
            while (actual.endsWith(QChar::Null)) actual.chop(1);
            while (expected.endsWith(QChar::Null)) expected.chop(1);
            return actual == expected;
        }
        return actualData == expectedData;
    }

    // evaluateRegExistFunction:
    // - Input body: RegExist(...) argument text;
    // - Processing: checks key/value existence plus optional type/data comparison;
    // - Return: true when the registry condition is satisfied.
    bool evaluateRegExistFunction(const QString& body)
    {
        const QHash<QString, QString> parameters = parseFunctionParameters(body);
        const QString keyPath = parameters.value(QStringLiteral("key")).trimmed();
        const QString valueName = parameters.value(QStringLiteral("value"));
        if (keyPath.isEmpty()) return false;

        HKEY root = nullptr;
        QString subPath;
        if (!parseRegistryPath(keyPath, &root, &subPath)) return false;

        const REGSAM viewFlags =
            parameters.value(QStringLiteral("wow64")).compare(QStringLiteral("True"), Qt::CaseInsensitive) == 0
            ? KEY_WOW64_32KEY
            : 0;

        if (!parameters.contains(QStringLiteral("value")))
        {
            HKEY key = nullptr;
            const LONG openResult = ::RegOpenKeyExW(
                root,
                subPath.isEmpty() ? nullptr : reinterpret_cast<const wchar_t*>(subPath.utf16()),
                0,
                KEY_QUERY_VALUE | viewFlags,
                &key);
            if (openResult == ERROR_SUCCESS) ::RegCloseKey(key);
            return openResult == ERROR_SUCCESS;
        }

        DWORD actualType = REG_NONE;
        QByteArray actualData;
        if (!readRegistryValue(keyPath, valueName, viewFlags, &actualType, &actualData, nullptr))
        {
            return false;
        }

        if (parameters.contains(QStringLiteral("type")))
        {
            DWORD expectedType = REG_NONE;
            if (!parseRegistryType(parameters.value(QStringLiteral("type")), &expectedType) || actualType != expectedType)
            {
                return false;
            }
        }
        if (!parameters.contains(QStringLiteral("data")))
        {
            return true;
        }

        DWORD expectedType = actualType;
        if (parameters.contains(QStringLiteral("type")))
        {
            parseRegistryType(parameters.value(QStringLiteral("type")), &expectedType);
        }
        QByteArray expectedData;
        QString errorText;
        if (!registryDataFromJsonText(expectedType, parameters.value(QStringLiteral("data")), &expectedData, &errorText))
        {
            return false;
        }
        return compareRawRegistryData(actualType, actualData, expectedData);
    }

    // evaluateQueryServiceStartFunction:
    // - Input body: QueryServiceStart(...) argument text;
    // - Processing: queries service configuration and compares dwStartType;
    // - Return: true when service start type matches the JSON Type value.
    bool evaluateQueryServiceStartFunction(const QString& body)
    {
        const QHash<QString, QString> parameters = parseFunctionParameters(body);
        const QString serviceName = parameters.value(QStringLiteral("name")).trimmed();
        const QString typeText = parameters.value(QStringLiteral("type")).trimmed();
        if (serviceName.isEmpty() || typeText.isEmpty()) return false;

        bool ok = false;
        const DWORD expectedStartType = typeText.toULong(&ok, 10);
        if (!ok) return false;

        SC_HANDLE managerHandle = ::OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT);
        if (managerHandle == nullptr) return false;
        SC_HANDLE serviceHandle = ::OpenServiceW(
            managerHandle,
            reinterpret_cast<const wchar_t*>(serviceName.utf16()),
            SERVICE_QUERY_CONFIG);
        if (serviceHandle == nullptr)
        {
            ::CloseServiceHandle(managerHandle);
            return false;
        }

        DWORD bytesNeeded = 0;
        ::QueryServiceConfigW(serviceHandle, nullptr, 0, &bytesNeeded);
        QByteArray buffer(static_cast<int>(bytesNeeded), 0);
        const BOOL queryOk = ::QueryServiceConfigW(
            serviceHandle,
            reinterpret_cast<LPQUERY_SERVICE_CONFIGW>(buffer.data()),
            bytesNeeded,
            &bytesNeeded);
        ::CloseServiceHandle(serviceHandle);
        ::CloseServiceHandle(managerHandle);
        if (!queryOk) return false;

        const auto* config = reinterpret_cast<const QUERY_SERVICE_CONFIGW*>(buffer.constData());
        return config != nullptr && config->dwStartType == expectedStartType;
    }

    // evaluateOsVersionFunction:
    // - Input body: OSVersion(...) argument text;
    // - Processing: compares current Windows major.minor version to the Text value;
    // - Return: true when the comparison is satisfied.
    bool evaluateOsVersionFunction(const QString& body)
    {
        const QHash<QString, QString> parameters = parseFunctionParameters(body);
        const QString targetText = parameters.value(QStringLiteral("text")).trimmed();
        if (targetText.isEmpty()) return false;

        const QStringList parts = targetText.split(QLatin1Char('.'));
        if (parts.size() < 2) return false;
        bool majorOk = false;
        bool minorOk = false;
        const int targetMajor = parts.at(0).toInt(&majorOk);
        const int targetMinor = parts.at(1).toInt(&minorOk);
        if (!majorOk || !minorOk) return false;

        const QOperatingSystemVersion current = QOperatingSystemVersion::current();
        const int currentScore = current.majorVersion() * 1000 + current.minorVersion();
        const int targetScore = targetMajor * 1000 + targetMinor;
        const QString compareText = parameters.value(QStringLiteral("compare"), QStringLiteral("=")).trimmed();
        if (compareText == QStringLiteral(">=")) return currentScore >= targetScore;
        if (compareText == QStringLiteral("<=")) return currentScore <= targetScore;
        if (compareText == QStringLiteral(">")) return currentScore > targetScore;
        if (compareText == QStringLiteral("<")) return currentScore < targetScore;
        if (compareText == QStringLiteral("!=")) return currentScore != targetScore;
        return currentScore == targetScore;
    }

    // ConditionParser:
    // - Input expression: Dism++ boolean condition text;
    // - Processing: recursive-descent parses NOT/AND/OR, parentheses, and supported functions;
    // - Return: boolean result; unsupported functions evaluate false to avoid false positives.
    class ConditionParser
    {
    public:
        explicit ConditionParser(const QString& expression)
            : m_expression(expression)
        {
        }

        bool evaluate()
        {
            m_position = 0;
            const bool result = parseOrExpression();
            skipWhitespace();
            return result && m_position <= m_expression.size();
        }

    private:
        void skipWhitespace()
        {
            while (m_position < m_expression.size() && m_expression.at(m_position).isSpace())
            {
                ++m_position;
            }
        }

        bool matchKeyword(const QString& keyword)
        {
            skipWhitespace();
            if (m_expression.mid(m_position, keyword.size()).compare(keyword, Qt::CaseInsensitive) != 0)
            {
                return false;
            }
            const int end = m_position + keyword.size();
            if (end < m_expression.size() && (m_expression.at(end).isLetterOrNumber() || m_expression.at(end) == QLatin1Char('_')))
            {
                return false;
            }
            m_position = end;
            return true;
        }

        bool parseOrExpression()
        {
            bool value = parseAndExpression();
            while (matchKeyword(QStringLiteral("OR")))
            {
                const bool rhs = parseAndExpression();
                value = value || rhs;
            }
            return value;
        }

        bool parseAndExpression()
        {
            bool value = parseUnaryExpression();
            while (matchKeyword(QStringLiteral("AND")))
            {
                const bool rhs = parseUnaryExpression();
                value = value && rhs;
            }
            return value;
        }

        bool parseUnaryExpression()
        {
            if (matchKeyword(QStringLiteral("NOT")))
            {
                return !parseUnaryExpression();
            }
            return parsePrimaryExpression();
        }

        bool parsePrimaryExpression()
        {
            skipWhitespace();
            if (m_position >= m_expression.size()) return false;
            if (m_expression.at(m_position) == QLatin1Char('('))
            {
                ++m_position;
                const bool value = parseOrExpression();
                skipWhitespace();
                if (m_position < m_expression.size() && m_expression.at(m_position) == QLatin1Char(')'))
                {
                    ++m_position;
                }
                return value;
            }
            return parseFunctionCall();
        }

        bool parseFunctionCall()
        {
            skipWhitespace();
            const int nameStart = m_position;
            while (m_position < m_expression.size() &&
                (m_expression.at(m_position).isLetterOrNumber() || m_expression.at(m_position) == QLatin1Char('_')))
            {
                ++m_position;
            }
            const QString functionName = m_expression.mid(nameStart, m_position - nameStart).trimmed();
            skipWhitespace();
            if (functionName.isEmpty() || m_position >= m_expression.size() || m_expression.at(m_position) != QLatin1Char('('))
            {
                return false;
            }

            ++m_position;
            const int bodyStart = m_position;
            int depth = 1;
            while (m_position < m_expression.size() && depth > 0)
            {
                const QChar ch = m_expression.at(m_position);
                if (ch == QLatin1Char('(')) ++depth;
                else if (ch == QLatin1Char(')')) --depth;
                if (depth > 0) ++m_position;
            }
            const QString body = m_expression.mid(bodyStart, m_position - bodyStart);
            if (m_position < m_expression.size() && m_expression.at(m_position) == QLatin1Char(')'))
            {
                ++m_position;
            }

            if (functionName.compare(QStringLiteral("RegExist"), Qt::CaseInsensitive) == 0)
            {
                return evaluateRegExistFunction(body);
            }
            if (functionName.compare(QStringLiteral("QueryServiceStart"), Qt::CaseInsensitive) == 0)
            {
                return evaluateQueryServiceStartFunction(body);
            }
            if (functionName.compare(QStringLiteral("OSVersion"), Qt::CaseInsensitive) == 0)
            {
                return evaluateOsVersionFunction(body);
            }
            return false;
        }

        QString m_expression;
        int m_position = 0;
    };
}

RegistryOptimizationPage::RegistryOptimizationPage(QWidget* parent)
    : QWidget(parent)
{
    initializeUi();
    initializeConnections();
    loadOptimizationProfile();
}

void RegistryOptimizationPage::initializeUi()
{
    QVBoxLayout* rootLayout = new QVBoxLayout(this);
    rootLayout->setContentsMargins(4, 4, 4, 4);
    rootLayout->setSpacing(6);

    QHBoxLayout* toolLayout = new QHBoxLayout();
    toolLayout->setContentsMargins(0, 0, 0, 0);
    toolLayout->setSpacing(6);

    QWidget* presetButtonWidget = new QWidget(this);
    QHBoxLayout* presetButtonLayout = new QHBoxLayout(presetButtonWidget);
    presetButtonLayout->setContentsMargins(0, 0, 0, 0);
    presetButtonLayout->setSpacing(0);
    m_columnPresetAButton = new QPushButton(QStringLiteral("A"), presetButtonWidget);
    m_columnPresetBButton = new QPushButton(QStringLiteral("B"), presetButtonWidget);
    m_columnPresetAButton->setFixedWidth(30);
    m_columnPresetBButton->setFixedWidth(30);
    m_columnPresetAButton->setToolTip(QStringLiteral("A 组：操作视图，只显示项目、作用域、状态、目标和应用按钮。"));
    m_columnPresetBButton->setToolTip(QStringLiteral("B 组：诊断视图，只显示项目、作用域、类型和条件/警告。"));
    presetButtonLayout->addWidget(m_columnPresetAButton);
    presetButtonLayout->addWidget(m_columnPresetBButton);

    m_filterEdit = new QLineEdit(this);
    m_filterEdit->setPlaceholderText(QStringLiteral("过滤组名、项目名、作用域或条件"));
    m_filterEdit->setStyleSheet(QStringLiteral(
        "QLineEdit{border:1px solid %1;border-radius:3px;background:%2;color:%3;padding:3px 6px;}"
        "QLineEdit:focus{border:1px solid %4;}").arg(
            KswordTheme::BorderHex(),
            KswordTheme::SurfaceHex(),
            KswordTheme::TextPrimaryHex(),
            KswordTheme::PrimaryBlueHex));
    m_filterDebounceTimer = new QTimer(this);
    m_filterDebounceTimer->setSingleShot(true);
    m_filterDebounceTimer->setInterval(kFilterDebounceMs);

    m_reloadButton = new QPushButton(QStringLiteral("重新加载 JSON"), this);
    m_refreshStateButton = new QPushButton(QStringLiteral("刷新可见状态"), this);
    m_reloadButton->setStyleSheet(KswordTheme::ThemedButtonStyle());
    m_refreshStateButton->setStyleSheet(KswordTheme::ThemedButtonStyle());

    toolLayout->addWidget(presetButtonWidget, 0);
    toolLayout->addWidget(m_filterEdit, 1);
    toolLayout->addWidget(m_refreshStateButton, 0);
    toolLayout->addWidget(m_reloadButton, 0);
    rootLayout->addLayout(toolLayout, 0);

    m_splitter = new QSplitter(Qt::Horizontal, this);
    rootLayout->addWidget(m_splitter, 1);

    m_groupTree = new QTreeWidget(m_splitter);
    m_groupTree->setColumnCount(1);
    m_groupTree->setHeaderLabel(QStringLiteral("优化分组"));
    m_groupTree->setMinimumWidth(260);
    m_groupTree->header()->setStyleSheet(QStringLiteral("QHeaderView::section{color:%1;font-weight:600;}").arg(KswordTheme::PrimaryBlueHex));

    QWidget* rightWidget = new QWidget(m_splitter);
    QVBoxLayout* rightLayout = new QVBoxLayout(rightWidget);
    rightLayout->setContentsMargins(0, 0, 0, 0);
    rightLayout->setSpacing(6);

    m_itemTable = new ks::ui::VisibleTableWidget(rightWidget);
    m_itemTable->setColumnCount(7);
    m_itemTable->setHorizontalHeaderLabels(QStringList{
        QStringLiteral("项目"),
        QStringLiteral("作用域"),
        QStringLiteral("类型"),
        QStringLiteral("当前状态"),
        QStringLiteral("目标选项"),
        QStringLiteral("操作"),
        QStringLiteral("条件/警告")
    });
    m_itemTable->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_itemTable->setSelectionMode(QAbstractItemView::SingleSelection);
    m_itemTable->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_itemTable->setAlternatingRowColors(true);
    m_itemTable->setCornerButtonEnabled(false);
    m_itemTable->setWordWrap(false);
    m_itemTable->setTextElideMode(Qt::ElideRight);
    m_itemTable->verticalHeader()->setDefaultSectionSize(kDefaultRowHeight);
    m_itemTable->verticalHeader()->setMinimumSectionSize(kDefaultRowHeight);
    m_itemTable->horizontalHeader()->setStyleSheet(QStringLiteral("QHeaderView::section{color:%1;font-weight:600;}").arg(KswordTheme::PrimaryBlueHex));
    m_itemTable->horizontalHeader()->setSectionResizeMode(kItemNameColumn, QHeaderView::Stretch);
    m_itemTable->horizontalHeader()->setSectionResizeMode(kScopeColumn, QHeaderView::ResizeToContents);
    m_itemTable->horizontalHeader()->setSectionResizeMode(kTypeColumn, QHeaderView::ResizeToContents);
    m_itemTable->horizontalHeader()->setSectionResizeMode(kCurrentStateColumn, QHeaderView::ResizeToContents);
    m_itemTable->horizontalHeader()->setSectionResizeMode(kTargetControlColumn, QHeaderView::Fixed);
    m_itemTable->horizontalHeader()->setSectionResizeMode(kActionButtonColumn, QHeaderView::Fixed);
    m_itemTable->horizontalHeader()->setSectionResizeMode(kConditionWarningColumn, QHeaderView::Stretch);
    m_itemTable->horizontalHeader()->setContextMenuPolicy(Qt::CustomContextMenu);
    m_itemTable->setColumnWidth(kTargetControlColumn, kTargetColumnWidth);
    m_itemTable->setColumnWidth(kActionButtonColumn, kActionColumnWidth);
    rightLayout->addWidget(m_itemTable, 2);

    m_detailText = new QTextEdit(rightWidget);
    m_detailText->setReadOnly(true);
    m_detailText->setMinimumHeight(130);
    rightLayout->addWidget(m_detailText, 1);

    m_statusLabel = new QLabel(QStringLiteral("系统优化：等待加载 profiles JSON。"), this);
    m_statusLabel->setTextInteractionFlags(Qt::TextSelectableByMouse);
    rootLayout->addWidget(m_statusLabel, 0);

    m_splitter->setStretchFactor(0, 0);
    m_splitter->setStretchFactor(1, 1);
    refreshColumnPresetButtonStyles();
}

void RegistryOptimizationPage::initializeConnections()
{
    connect(m_columnPresetAButton, &QPushButton::clicked, this, [this]() { applyColumnPreset(ColumnPreset::A); });
    connect(m_columnPresetBButton, &QPushButton::clicked, this, [this]() { applyColumnPreset(ColumnPreset::B); });
    connect(m_reloadButton, &QPushButton::clicked, this, [this]() { loadOptimizationProfile(); });
    connect(m_refreshStateButton, &QPushButton::clicked, this, [this]() { refreshVisibleStates(); });
    connect(m_filterEdit, &QLineEdit::textChanged, this, [this]() { m_filterDebounceTimer->start(); });
    connect(m_filterDebounceTimer, &QTimer::timeout, this, [this]() { rebuildItemTable(); });
    connect(m_groupTree, &QTreeWidget::currentItemChanged, this, [this](QTreeWidgetItem*, QTreeWidgetItem*) {
        m_filterDebounceTimer->stop();
        rebuildItemTable();
    });
    connect(m_itemTable, &QTableWidget::currentCellChanged, this, [this](int currentRow, int, int, int) {
        updateDetailPanel(currentRow);
    });
    connect(m_itemTable->horizontalHeader(), &QHeaderView::customContextMenuRequested, this, [this](const QPoint& localPos) {
        showHeaderColumnMenu(localPos);
    });
}

QStringList RegistryOptimizationPage::profileCandidatePaths() const
{
    QStringList paths;
    const QString fileName = QString::fromLatin1(kOptimizationProfileFileName);
    const QString applicationDirectory = QCoreApplication::applicationDirPath();
    const QString currentDirectory = QDir::currentPath();
    paths << QDir(applicationDirectory).filePath(QStringLiteral("profiles/%1").arg(fileName));
    paths << QDir(applicationDirectory).filePath(QStringLiteral("../profiles/%1").arg(fileName));
    paths << QDir(currentDirectory).filePath(QStringLiteral("profiles/%1").arg(fileName));
    paths << QDir(currentDirectory).filePath(QStringLiteral("Ksword5.1/Ksword5.1/profiles/%1").arg(fileName));
    paths << QDir(currentDirectory).filePath(QStringLiteral("../profiles/%1").arg(fileName));
    paths.removeDuplicates();
    return paths;
}

void RegistryOptimizationPage::loadOptimizationProfile()
{
    cancelStateRefresh();
    if (m_filterDebounceTimer != nullptr) m_filterDebounceTimer->stop();
    m_itemList.clear();
    m_visibleRows.clear();
    m_loadedProfilePath.clear();
    m_itemTable->setRowCount(0);
    m_groupTree->clear();
    m_detailText->clear();

    QString selectedPath;
    for (const QString& candidatePath : profileCandidatePaths())
    {
        const QString resolvedPath = ks::profile::resolveProfileJsonPath(candidatePath);
        if (!resolvedPath.isEmpty())
        {
            selectedPath = QDir::cleanPath(resolvedPath);
            break;
        }
    }

    if (selectedPath.isEmpty())
    {
        updateStatusText(QStringLiteral("未找到 profiles/%1；系统优化页已加载，但没有可显示项目。").arg(QString::fromLatin1(kOptimizationProfileFileName)));
        return;
    }

    QJsonParseError parseError{};
    QString readErrorText;
    const QJsonDocument document = ks::profile::readProfileJsonDocument(selectedPath, &parseError, &readErrorText);
    if (parseError.error != QJsonParseError::NoError || !document.isArray())
    {
        const QString reasonText = readErrorText.isEmpty() ? parseError.errorString() : readErrorText;
        updateStatusText(QStringLiteral("系统优化 JSON 解析失败：%1 (%2)").arg(reasonText, selectedPath));
        return;
    }

    const QJsonArray itemArray = document.array();
    for (const QJsonValue& itemValue : itemArray)
    {
        if (!itemValue.isObject()) continue;
        const QJsonObject itemObject = itemValue.toObject();

        OptimizationItem item;
        item.groupIndex = jsonInt(itemObject, QStringLiteral("group_index"));
        item.itemIndex = jsonInt(itemObject, QStringLiteral("item_index"));
        item.groupNameText = jsonString(itemObject, QStringLiteral("group_name"));
        item.itemNameText = jsonString(itemObject, QStringLiteral("item_name"));
        item.itemTypeText = jsonString(itemObject, QStringLiteral("item_type"));
        item.groupConditionText = jsonString(itemObject, QStringLiteral("group_condition"));
        item.itemConditionText = jsonString(itemObject, QStringLiteral("item_condition"));
        item.warningText = jsonString(itemObject, QStringLiteral("item_warning"));

        const QJsonArray scopeArray = itemObject.value(QStringLiteral("scopes")).toArray();
        for (const QJsonValue& scopeValue : scopeArray)
        {
            if (!scopeValue.isObject()) continue;
            const QJsonObject scopeObject = scopeValue.toObject();
            OptimizationScope scope;
            scope.scopeText = jsonString(scopeObject, QStringLiteral("scope"));
            scope.conditionText = jsonString(scopeObject, QStringLiteral("scope_condition"));

            const QJsonArray stateArray = scopeObject.value(QStringLiteral("states")).toArray();
            for (const QJsonValue& stateValue : stateArray)
            {
                if (!stateValue.isObject()) continue;
                const QJsonObject stateObject = stateValue.toObject();
                OptimizationState state;
                state.tagText = jsonString(stateObject, QStringLiteral("state_tag"));
                state.labelText = jsonString(stateObject, QStringLiteral("state_label"));
                state.conditionText = jsonString(stateObject, QStringLiteral("state_condition"));
                state.warningText = jsonString(stateObject, QStringLiteral("state_warning"));

                const QJsonArray actionArray = stateObject.value(QStringLiteral("actions")).toArray();
                for (const QJsonValue& actionValue : actionArray)
                {
                    if (actionValue.isObject()) state.actionList.push_back(actionValue.toObject());
                }
                scope.stateList.push_back(state);
            }
            item.scopeList.push_back(scope);
        }
        m_itemList.push_back(item);
    }

    m_loadedProfilePath = selectedPath;
    rebuildGroupTree();
    rebuildItemTable();
    applyColumnPreset(m_columnPreset);
    updateStatusText(QStringLiteral("已加载系统优化 JSON：%1；项目 %2 个。").arg(selectedPath).arg(m_itemList.size()));
}

void RegistryOptimizationPage::rebuildGroupTree()
{
    QSignalBlocker blocker(m_groupTree);
    m_groupTree->clear();

    QHash<QString, int> groupCounts;
    QVector<QString> groupOrder;
    int rowCount = 0;
    for (const OptimizationItem& item : m_itemList)
    {
        const int itemScopeCount = std::max(1, static_cast<int>(item.scopeList.size()));
        if (!groupCounts.contains(item.groupNameText))
        {
            groupOrder.push_back(item.groupNameText);
        }
        groupCounts[item.groupNameText] += itemScopeCount;
        rowCount += itemScopeCount;
    }

    QTreeWidgetItem* allItem = new QTreeWidgetItem(m_groupTree);
    allItem->setText(0, QStringLiteral("全部 (%1)").arg(rowCount));
    allItem->setData(0, kRoleGroupName, QString());
    allItem->setSelected(true);
    m_groupTree->setCurrentItem(allItem);

    for (const QString& groupName : groupOrder)
    {
        QTreeWidgetItem* groupItem = new QTreeWidgetItem(m_groupTree);
        groupItem->setText(0, QStringLiteral("%1 (%2)").arg(groupName).arg(groupCounts.value(groupName)));
        groupItem->setData(0, kRoleGroupName, groupName);
    }
    m_groupTree->expandAll();
}

void RegistryOptimizationPage::rebuildItemTable()
{
    cancelStateRefresh();
    m_rebuildingTable = true;
    QSignalBlocker blocker(m_itemTable);
    const bool updatesEnabled = m_itemTable->updatesEnabled();
    m_itemTable->setUpdatesEnabled(false);
    m_visibleRows.clear();
    m_itemTable->setRowCount(0);

    const QTreeWidgetItem* selectedGroupItem = m_groupTree->currentItem();
    const QString selectedGroup = selectedGroupItem == nullptr ? QString() : selectedGroupItem->data(0, kRoleGroupName).toString();
    const QString filterText = m_filterEdit->text().trimmed();

    for (int itemIndex = 0; itemIndex < m_itemList.size(); ++itemIndex)
    {
        const OptimizationItem& item = m_itemList.at(itemIndex);
        if (!selectedGroup.isEmpty() && item.groupNameText.compare(selectedGroup, Qt::CaseInsensitive) != 0)
        {
            continue;
        }

        for (int scopeIndex = 0; scopeIndex < item.scopeList.size(); ++scopeIndex)
        {
            const OptimizationScope& scope = item.scopeList.at(scopeIndex);
            const QString haystack = QStringLiteral("%1\n%2\n%3\n%4\n%5\n%6")
                .arg(item.groupNameText, item.itemNameText, item.itemTypeText, scope.scopeText, item.itemConditionText, item.warningText);
            if (!filterText.isEmpty() && !haystack.contains(filterText, Qt::CaseInsensitive))
            {
                continue;
            }

            const int tableRow = m_itemTable->rowCount();
            m_itemTable->insertRow(tableRow);
            m_itemTable->setRowHeight(tableRow, kDefaultRowHeight);
            m_visibleRows.push_back(VisibleRow{ itemIndex, scopeIndex });

            m_itemTable->setItem(tableRow, 0, new QTableWidgetItem(item.itemNameText));
            m_itemTable->setItem(tableRow, 1, new QTableWidgetItem(scopeDisplayText(scope.scopeText)));
            m_itemTable->setItem(tableRow, 2, new QTableWidgetItem(item.itemTypeText));
            m_itemTable->setItem(tableRow, 3, new QTableWidgetItem(QStringLiteral("未检测")));
            QTableWidgetItem* targetPlaceholderItem = new QTableWidgetItem();
            targetPlaceholderItem->setSizeHint(QSize(kTargetColumnWidth, kDefaultRowHeight));
            targetPlaceholderItem->setFlags(Qt::ItemIsEnabled | Qt::ItemIsSelectable);
            m_itemTable->setItem(tableRow, kTargetControlColumn, targetPlaceholderItem);
            QTableWidgetItem* actionPlaceholderItem = new QTableWidgetItem();
            actionPlaceholderItem->setSizeHint(QSize(kActionColumnWidth, kDefaultRowHeight));
            actionPlaceholderItem->setFlags(Qt::ItemIsEnabled | Qt::ItemIsSelectable);
            m_itemTable->setItem(tableRow, kActionButtonColumn, actionPlaceholderItem);

            if (item.itemTypeText.compare(QStringLiteral("Combo"), Qt::CaseInsensitive) == 0)
            {
                QComboBox* comboBox = new QComboBox(m_itemTable);
                comboBox->setObjectName(QStringLiteral("optimizationTargetCombo"));
                comboBox->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
                comboBox->setMinimumWidth(kTargetColumnWidth - 16);
                for (const OptimizationState& state : scope.stateList)
                {
                    if (state.tagText.compare(QStringLiteral("Dropdown"), Qt::CaseInsensitive) == 0)
                    {
                        comboBox->addItem(state.labelText, state.labelText);
                    }
                }
                comboBox->setProperty("row", tableRow);
                m_itemTable->setCellWidget(tableRow, kTargetControlColumn, buildCenteredCellWidget(comboBox, m_itemTable));
            }
            else
            {
                QCheckBox* checkBox = new QCheckBox(QStringLiteral("启用"), m_itemTable);
                checkBox->setObjectName(QStringLiteral("optimizationTargetCheck"));
                checkBox->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);
                checkBox->setProperty("row", tableRow);
                m_itemTable->setCellWidget(tableRow, kTargetControlColumn, buildCenteredCellWidget(checkBox, m_itemTable));
            }

            QPushButton* applyButton = new QPushButton(QStringLiteral("应用"), m_itemTable);
            applyButton->setStyleSheet(KswordTheme::ThemedButtonStyle());
            applyButton->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);
            applyButton->setFixedWidth(kActionColumnWidth - 14);
            applyButton->setProperty("row", tableRow);
            connect(applyButton, &QPushButton::clicked, this, [this, applyButton]() {
                const int row = applyButton->property("row").toInt();
                applyVisibleRow(row);
            });
            m_itemTable->setCellWidget(tableRow, kActionButtonColumn, buildCenteredCellWidget(applyButton, m_itemTable));

            QStringList conditionLines;
            if (!item.groupConditionText.isEmpty()) conditionLines << QStringLiteral("组条件: %1").arg(item.groupConditionText);
            if (!item.itemConditionText.isEmpty()) conditionLines << QStringLiteral("项目条件: %1").arg(item.itemConditionText);
            if (!scope.conditionText.isEmpty()) conditionLines << QStringLiteral("作用域条件: %1").arg(scope.conditionText);
            if (!item.warningText.isEmpty()) conditionLines << QStringLiteral("警告: %1").arg(item.warningText);
            m_itemTable->setItem(tableRow, 6, new QTableWidgetItem(conditionLines.join(QStringLiteral(" | "))));
        }
    }

    m_rebuildingTable = false;
    if (m_itemTable->rowCount() > 0)
    {
        m_itemTable->setCurrentCell(0, 0);
        updateDetailPanel(0);
    }
    else
    {
        m_detailText->clear();
    }
    updateStatusText(QStringLiteral("系统优化：当前显示 %1 行；来源 %2。").arg(m_itemTable->rowCount()).arg(m_loadedProfilePath));
    applyColumnPreset(m_columnPreset);
    m_itemTable->setUpdatesEnabled(updatesEnabled);
    if (updatesEnabled) m_itemTable->viewport()->update();
}

void RegistryOptimizationPage::refreshVisibleStates()
{
    if (m_stateRefreshInProgress) return;

    const QVector<OptimizationItem> itemSnapshot = m_itemList;
    const QVector<VisibleRow> visibleRowsSnapshot = m_visibleRows;
    const std::uint64_t generation = ++m_stateRefreshGeneration;
    m_stateRefreshInProgress = true;
    m_refreshStateButton->setEnabled(false);

    QPointer<RegistryOptimizationPage> guardThis(this);
    std::thread([guardThis, generation, itemSnapshot, visibleRowsSnapshot]() {
        QVector<VisibleStateRefreshResult> results;
        results.reserve(visibleRowsSnapshot.size());
        for (int tableRow = 0; tableRow < visibleRowsSnapshot.size(); ++tableRow)
        {
            const VisibleRow& rowRef = visibleRowsSnapshot.at(tableRow);
            VisibleStateRefreshResult result;
            result.tableRow = tableRow;
            result.stateText = QStringLiteral("未匹配/未知");
            if (rowRef.itemIndex >= 0 && rowRef.itemIndex < itemSnapshot.size())
            {
                const OptimizationItem& item = itemSnapshot.at(rowRef.itemIndex);
                if (rowRef.scopeIndex >= 0 && rowRef.scopeIndex < item.scopeList.size())
                {
                    const OptimizationScope& scope = item.scopeList.at(rowRef.scopeIndex);
                    for (const OptimizationState& state : scope.stateList)
                    {
                        if (!state.conditionText.trimmed().isEmpty() &&
                            RegistryOptimizationPage::evaluateConditionText(state.conditionText))
                        {
                            result.stateText = state.labelText;
                            result.targetLabel = state.labelText;
                            result.targetEnabled = state.tagText.compare(QStringLiteral("State"), Qt::CaseInsensitive) == 0;
                            break;
                        }
                    }
                }
            }
            results.push_back(std::move(result));
        }

        QMetaObject::invokeMethod(qApp, [guardThis, generation, results = std::move(results)]() {
            if (guardThis == nullptr || guardThis->m_stateRefreshGeneration != generation) return;

            guardThis->m_stateRefreshInProgress = false;
            guardThis->m_refreshStateButton->setEnabled(true);
            const bool updatesEnabled = guardThis->m_itemTable->updatesEnabled();
            guardThis->m_itemTable->setUpdatesEnabled(false);
            for (const VisibleStateRefreshResult& result : results)
            {
                if (result.tableRow < 0 || result.tableRow >= guardThis->m_itemTable->rowCount()) continue;
                if (QTableWidgetItem* statusItem = guardThis->m_itemTable->item(result.tableRow, kCurrentStateColumn))
                {
                    statusItem->setText(result.stateText);
                }

                QWidget* targetWidget = guardThis->m_itemTable->cellWidget(result.tableRow, kTargetControlColumn);
                if (QComboBox* comboBox = findTargetComboBox(targetWidget))
                {
                    const int comboIndex = comboBox->findData(result.targetLabel);
                    if (comboIndex >= 0) comboBox->setCurrentIndex(comboIndex);
                }
                else if (QCheckBox* checkBox = findTargetCheckBox(targetWidget))
                {
                    checkBox->setChecked(result.targetEnabled);
                }
            }
            guardThis->m_itemTable->setUpdatesEnabled(updatesEnabled);
            if (updatesEnabled) guardThis->m_itemTable->viewport()->update();
            guardThis->updateStatusText(QStringLiteral("系统优化：已刷新 %1 个可见项状态。").arg(results.size()));
        }, Qt::QueuedConnection);
    }).detach();
}

void RegistryOptimizationPage::cancelStateRefresh()
{
    if (!m_stateRefreshInProgress) return;

    ++m_stateRefreshGeneration;
    m_stateRefreshInProgress = false;
    if (m_refreshStateButton != nullptr) m_refreshStateButton->setEnabled(true);
}

void RegistryOptimizationPage::refreshVisibleRowState(const int tableRow)
{
    if (tableRow < 0 || tableRow >= m_visibleRows.size()) return;
    const VisibleRow rowRef = m_visibleRows.at(tableRow);
    if (rowRef.itemIndex < 0 || rowRef.itemIndex >= m_itemList.size()) return;
    const OptimizationItem& item = m_itemList.at(rowRef.itemIndex);
    if (rowRef.scopeIndex < 0 || rowRef.scopeIndex >= item.scopeList.size()) return;
    const OptimizationScope& scope = item.scopeList.at(rowRef.scopeIndex);

    const OptimizationState* detectedState = detectedStateForScope(scope);
    const QString stateText = detectedState == nullptr
        ? QStringLiteral("未匹配/未知")
        : detectedState->labelText;
    if (QTableWidgetItem* statusItem = m_itemTable->item(tableRow, 3))
    {
        statusItem->setText(stateText);
    }

    QWidget* targetWidget = m_itemTable->cellWidget(tableRow, kTargetControlColumn);
    if (QComboBox* comboBox = findTargetComboBox(targetWidget))
    {
        if (detectedState != nullptr)
        {
            const int comboIndex = comboBox->findData(detectedState->labelText);
            if (comboIndex >= 0) comboBox->setCurrentIndex(comboIndex);
        }
    }
    else if (QCheckBox* checkBox = findTargetCheckBox(targetWidget))
    {
        checkBox->setChecked(detectedState != nullptr &&
            detectedState->tagText.compare(QStringLiteral("State"), Qt::CaseInsensitive) == 0);
    }
}

void RegistryOptimizationPage::updateDetailPanel(const int tableRow)
{
    if (m_rebuildingTable || tableRow < 0 || tableRow >= m_visibleRows.size())
    {
        m_detailText->clear();
        return;
    }

    const VisibleRow rowRef = m_visibleRows.at(tableRow);
    const OptimizationItem& item = m_itemList.at(rowRef.itemIndex);
    const OptimizationScope& scope = item.scopeList.at(rowRef.scopeIndex);

    QStringList lines;
    lines << QStringLiteral("项目: %1").arg(item.itemNameText);
    lines << QStringLiteral("分组: %1").arg(item.groupNameText);
    lines << QStringLiteral("作用域: %1 (%2)").arg(scopeDisplayText(scope.scopeText), scope.scopeText);
    lines << QStringLiteral("类型: %1").arg(item.itemTypeText);
    if (!item.warningText.isEmpty()) lines << QStringLiteral("项目警告: %1").arg(item.warningText);
    if (!item.groupConditionText.isEmpty()) lines << QStringLiteral("组条件: %1").arg(item.groupConditionText);
    if (!item.itemConditionText.isEmpty()) lines << QStringLiteral("项目条件: %1").arg(item.itemConditionText);
    if (!scope.conditionText.isEmpty()) lines << QStringLiteral("作用域条件: %1").arg(scope.conditionText);
    lines << QStringLiteral("");
    lines << QStringLiteral("状态/动作:");
    for (const OptimizationState& state : scope.stateList)
    {
        lines << QStringLiteral("- %1 [%2] 条件=%3").arg(state.labelText, state.tagText, state.conditionText);
        if (!state.warningText.isEmpty()) lines << QStringLiteral("  警告: %1").arg(state.warningText);
        for (const QJsonObject& action : state.actionList)
        {
            lines << QStringLiteral("  * %1").arg(jsonString(action, QStringLiteral("summary"), QString::fromUtf8(QJsonDocument(action).toJson(QJsonDocument::Compact))));
        }
    }
    m_detailText->setPlainText(lines.join(QLatin1Char('\n')));
}

void RegistryOptimizationPage::updateStatusText(const QString& text)
{
    if (m_statusLabel != nullptr) m_statusLabel->setText(text);
}

void RegistryOptimizationPage::applyColumnPreset(const ColumnPreset preset)
{
    m_columnPreset = preset;
    for (int columnIndex = 0; columnIndex < m_itemTable->columnCount(); ++columnIndex)
    {
        m_itemTable->setColumnHidden(columnIndex, !isColumnVisibleInPreset(columnIndex, preset));
    }
    m_itemTable->setColumnWidth(kTargetControlColumn, kTargetColumnWidth);
    m_itemTable->setColumnWidth(kActionButtonColumn, kActionColumnWidth);
    refreshColumnPresetButtonStyles();
    ks::ui::RequestTableColumnAutoFit(m_itemTable);
}

void RegistryOptimizationPage::refreshColumnPresetButtonStyles()
{
    const QString inactiveStyle = KswordTheme::ThemedButtonStyle();
    const QString activeStyle = QStringLiteral(
        "QPushButton{background:%1;color:white;border:1px solid %2;border-radius:3px;padding:3px 8px;font-weight:700;}"
        "QPushButton:hover{background:%3;}"
        "QPushButton:pressed{background:%4;}").arg(
            KswordTheme::PrimaryBlueHex,
            KswordTheme::PrimaryBlueBorderHex,
            KswordTheme::PrimaryBlueActiveHex,
            KswordTheme::PrimaryBluePressedHex);

    if (m_columnPresetAButton != nullptr)
    {
        m_columnPresetAButton->setStyleSheet(m_columnPreset == ColumnPreset::A ? activeStyle : inactiveStyle);
    }
    if (m_columnPresetBButton != nullptr)
    {
        m_columnPresetBButton->setStyleSheet(m_columnPreset == ColumnPreset::B ? activeStyle : inactiveStyle);
    }
}

void RegistryOptimizationPage::showHeaderColumnMenu(const QPoint& localPos)
{
    QMenu columnMenu(this);
    columnMenu.setStyleSheet(KswordTheme::ContextMenuStyle());
    for (int columnIndex = 0; columnIndex < m_itemTable->columnCount(); ++columnIndex)
    {
        const QString headerText = m_itemTable->horizontalHeaderItem(columnIndex) == nullptr
            ? QStringLiteral("列 %1").arg(columnIndex)
            : m_itemTable->horizontalHeaderItem(columnIndex)->text();
        QAction* columnAction = columnMenu.addAction(headerText);
        columnAction->setCheckable(true);
        columnAction->setChecked(!m_itemTable->isColumnHidden(columnIndex));
        columnAction->setData(columnIndex);
    }

    QAction* selectedAction = columnMenu.exec(m_itemTable->horizontalHeader()->mapToGlobal(localPos));
    if (selectedAction == nullptr)
    {
        return;
    }

    const int columnIndex = selectedAction->data().toInt();
    if (columnIndex < 0 || columnIndex >= m_itemTable->columnCount())
    {
        return;
    }
    if (!selectedAction->isChecked())
    {
        int visibleColumnCount = 0;
        for (int currentColumn = 0; currentColumn < m_itemTable->columnCount(); ++currentColumn)
        {
            if (!m_itemTable->isColumnHidden(currentColumn))
            {
                ++visibleColumnCount;
            }
        }
        if (visibleColumnCount <= 1)
        {
            return;
        }
    }

    m_itemTable->setColumnHidden(columnIndex, !selectedAction->isChecked());
    m_columnPreset = ColumnPreset::Custom;
    refreshColumnPresetButtonStyles();
    ks::ui::RequestTableColumnAutoFit(m_itemTable);
}

bool RegistryOptimizationPage::isColumnVisibleInPreset(const int columnIndex, const ColumnPreset preset) const
{
    if (preset == ColumnPreset::A)
    {
        return columnIndex == kItemNameColumn ||
            columnIndex == kScopeColumn ||
            columnIndex == kCurrentStateColumn ||
            columnIndex == kTargetControlColumn ||
            columnIndex == kActionButtonColumn;
    }
    if (preset == ColumnPreset::B)
    {
        return columnIndex == kItemNameColumn ||
            columnIndex == kScopeColumn ||
            columnIndex == kTypeColumn ||
            columnIndex == kConditionWarningColumn;
    }
    return !m_itemTable->isColumnHidden(columnIndex);
}

const RegistryOptimizationPage::OptimizationState* RegistryOptimizationPage::selectedTargetStateForRow(const int tableRow) const
{
    if (tableRow < 0 || tableRow >= m_visibleRows.size()) return nullptr;
    const VisibleRow rowRef = m_visibleRows.at(tableRow);
    if (rowRef.itemIndex < 0 || rowRef.itemIndex >= m_itemList.size()) return nullptr;
    const OptimizationItem& item = m_itemList.at(rowRef.itemIndex);
    if (rowRef.scopeIndex < 0 || rowRef.scopeIndex >= item.scopeList.size()) return nullptr;
    const OptimizationScope& scope = item.scopeList.at(rowRef.scopeIndex);

    QWidget* targetWidget = m_itemTable->cellWidget(tableRow, kTargetControlColumn);
    if (const QComboBox* comboBox = findTargetComboBox(targetWidget))
    {
        const QString targetLabel = comboBox->currentData().toString();
        for (const OptimizationState& state : scope.stateList)
        {
            if (state.tagText.compare(QStringLiteral("Dropdown"), Qt::CaseInsensitive) == 0 &&
                state.labelText == targetLabel)
            {
                return &state;
            }
        }
        return nullptr;
    }

    const QCheckBox* targetCheckBox = findTargetCheckBox(targetWidget);
    const bool targetEnabled = targetCheckBox == nullptr
        ? false
        : targetCheckBox->isChecked();
    const QString desiredTag = targetEnabled ? QStringLiteral("True") : QStringLiteral("False");
    for (const OptimizationState& state : scope.stateList)
    {
        if (state.tagText.compare(desiredTag, Qt::CaseInsensitive) == 0)
        {
            return &state;
        }
    }
    return nullptr;
}

const RegistryOptimizationPage::OptimizationState* RegistryOptimizationPage::detectedStateForScope(const OptimizationScope& scope) const
{
    for (const OptimizationState& state : scope.stateList)
    {
        if (!state.conditionText.trimmed().isEmpty() && evaluateConditionText(state.conditionText))
        {
            return &state;
        }
    }
    return nullptr;
}

bool RegistryOptimizationPage::applyVisibleRow(const int tableRow)
{
    cancelStateRefresh();
    if (tableRow < 0 || tableRow >= m_visibleRows.size()) return false;
    const VisibleRow rowRef = m_visibleRows.at(tableRow);
    const OptimizationItem& item = m_itemList.at(rowRef.itemIndex);
    const OptimizationScope& scope = item.scopeList.at(rowRef.scopeIndex);
    const OptimizationState* state = selectedTargetStateForRow(tableRow);
    if (state == nullptr)
    {
        QMessageBox::warning(this, QStringLiteral("系统优化"), QStringLiteral("未找到当前行对应的目标状态。"));
        return false;
    }

    QString warningText;
    if (!item.warningText.isEmpty()) warningText += item.warningText + QLatin1Char('\n');
    if (!state->warningText.isEmpty()) warningText += state->warningText + QLatin1Char('\n');
    const QString messageText = QStringLiteral("即将应用：\n%1\n作用域：%2\n目标：%3\n\n%4是否继续？")
        .arg(item.itemNameText, scopeDisplayText(scope.scopeText), state->labelText, warningText);
    if (QMessageBox::question(this, QStringLiteral("确认系统优化"), messageText) != QMessageBox::Yes)
    {
        return false;
    }

    QStringList detailLines;
    const bool applyOk = applyStateActions(item, scope, *state, &detailLines);
    m_detailText->setPlainText(detailLines.join(QLatin1Char('\n')));
    refreshVisibleRowState(tableRow);
    updateStatusText(applyOk
        ? QStringLiteral("系统优化：已应用 %1 -> %2。").arg(item.itemNameText, state->labelText)
        : QStringLiteral("系统优化：应用失败 %1 -> %2；请查看详情。").arg(item.itemNameText, state->labelText));
    return applyOk;
}

bool RegistryOptimizationPage::applyStateActions(
    const OptimizationItem& item,
    const OptimizationScope& scope,
    const OptimizationState& state,
    QStringList* detailLinesOut)
{
    if (detailLinesOut != nullptr)
    {
        detailLinesOut->clear();
        *detailLinesOut << QStringLiteral("应用项目: %1").arg(item.itemNameText);
        *detailLinesOut << QStringLiteral("作用域: %1").arg(scopeDisplayText(scope.scopeText));
        *detailLinesOut << QStringLiteral("目标状态: %1").arg(state.labelText);
    }

    if (state.actionList.isEmpty())
    {
        if (detailLinesOut != nullptr) *detailLinesOut << QStringLiteral("该状态没有动作。");
        return true;
    }

    bool allOk = true;
    bool restartExplorer = false;
    for (const QJsonObject& actionObject : state.actionList)
    {
        bool actionRestartExplorer = false;
        const bool actionOk = executeAction(actionObject, detailLinesOut, &actionRestartExplorer);
        restartExplorer = restartExplorer || actionRestartExplorer;
        allOk = allOk && actionOk;
    }
    if (restartExplorer && detailLinesOut != nullptr)
    {
        *detailLinesOut << QStringLiteral("提示: 部分动作标记需要重启 Explorer 或重新登录后完全生效。");
    }
    return allOk;
}

bool RegistryOptimizationPage::executeAction(
    const QJsonObject& actionObject,
    QStringList* detailLinesOut,
    bool* restartExplorerOut)
{
    if (restartExplorerOut != nullptr) *restartExplorerOut = false;
    const QString familyText = jsonString(actionObject, QStringLiteral("action_family"),
        jsonString(actionObject, QStringLiteral("action_tag"))).trimmed();
    const QString summaryText = jsonString(actionObject, QStringLiteral("summary"),
        QString::fromUtf8(QJsonDocument(actionObject).toJson(QJsonDocument::Compact)));
    QString errorText;
    bool ok = false;

    if (familyText.compare(QStringLiteral("RegWrite"), Qt::CaseInsensitive) == 0 ||
        familyText.compare(QStringLiteral("RegExist"), Qt::CaseInsensitive) == 0)
    {
        ok = executeRegistryWriteAction(actionObject, &errorText);
    }
    else if (familyText.compare(QStringLiteral("RegDelete"), Qt::CaseInsensitive) == 0 ||
        familyText.compare(QStringLiteral("RegDetete"), Qt::CaseInsensitive) == 0)
    {
        ok = executeRegistryDeleteAction(actionObject, &errorText);
    }
    else if (familyText.compare(QStringLiteral("RegMove"), Qt::CaseInsensitive) == 0)
    {
        ok = executeRegistryMoveAction(actionObject, &errorText);
    }
    else if (familyText.compare(QStringLiteral("SetServiceStart"), Qt::CaseInsensitive) == 0)
    {
        ok = executeServiceStartAction(actionObject, &errorText);
    }
    else if (familyText.compare(QStringLiteral("ExplorerNotify"), Qt::CaseInsensitive) == 0)
    {
        ok = executeExplorerNotifyAction(actionObject, &errorText);
    }
    else if (familyText.compare(QStringLiteral("FileCreateByZIP"), Qt::CaseInsensitive) == 0)
    {
        ok = executeFileCreateByZipAction(actionObject, &errorText);
    }
    else
    {
        errorText = QStringLiteral("暂不支持动作族：%1").arg(familyText);
        ok = false;
    }

    const QString restartText = jsonString(actionObject, QStringLiteral("activate_restart"));
    if (restartExplorerOut != nullptr && restartText.contains(QStringLiteral("Explorer"), Qt::CaseInsensitive))
    {
        *restartExplorerOut = true;
    }

    if (!ok && shouldSkipActionError(actionObject))
    {
        if (detailLinesOut != nullptr) *detailLinesOut << QStringLiteral("[跳过] %1；原因：%2").arg(summaryText, errorText);
        return true;
    }
    if (detailLinesOut != nullptr)
    {
        *detailLinesOut << (ok
            ? QStringLiteral("[成功] %1").arg(summaryText)
            : QStringLiteral("[失败] %1；原因：%2").arg(summaryText, errorText));
    }
    return ok;
}

bool RegistryOptimizationPage::executeRegistryWriteAction(const QJsonObject& actionObject, QString* errorTextOut)
{
    if (errorTextOut != nullptr) errorTextOut->clear();
    const QString keyPath = jsonString(actionObject, QStringLiteral("Key"));
    const QString valueName = jsonString(actionObject, QStringLiteral("Value"));
    const QString typeText = jsonString(actionObject, QStringLiteral("Type"), QStringLiteral("REG_DWORD"));
    QString dataText = jsonString(actionObject, QStringLiteral("Data"));
    const QString operatorText = jsonString(actionObject, QStringLiteral("Operator"));
    const REGSAM viewFlags = actionRegistryViewFlags(actionObject);

    if (keyPath.isEmpty())
    {
        if (errorTextOut != nullptr) *errorTextOut = QStringLiteral("RegWrite 缺少 Key。");
        return false;
    }

    DWORD valueType = REG_NONE;
    if (!parseRegistryType(typeText, &valueType))
    {
        if (errorTextOut != nullptr) *errorTextOut = QStringLiteral("不支持注册表类型：%1").arg(typeText);
        return false;
    }

    if (dataText == QStringLiteral("?ColorDialog()"))
    {
        const QColor color = QColorDialog::getColor(Qt::white, this, QStringLiteral("选择注册表颜色值"));
        if (!color.isValid())
        {
            if (errorTextOut != nullptr) *errorTextOut = QStringLiteral("用户取消颜色选择。");
            return false;
        }
        const quint32 colorValue = 0xFF000000U |
            (static_cast<quint32>(color.blue()) << 16) |
            (static_cast<quint32>(color.green()) << 8) |
            static_cast<quint32>(color.red());
        dataText = QStringLiteral("%1").arg(colorValue, 8, 16, QLatin1Char('0')).toUpper();
    }

    QByteArray newData;
    QString conversionError;
    if (!registryDataFromJsonText(valueType, dataText, &newData, &conversionError))
    {
        if (errorTextOut != nullptr) *errorTextOut = conversionError;
        return false;
    }

    if (!operatorText.isEmpty())
    {
        DWORD currentType = valueType;
        QByteArray currentData;
        readRegistryValue(keyPath, valueName, viewFlags, &currentType, &currentData, nullptr);
        if (valueType == REG_DWORD)
        {
            const quint64 currentValue = currentData.size() >= 4 ? readUnsignedLittleEndian(currentData, 4) : 0;
            const quint64 maskValue = readUnsignedLittleEndian(newData, 4);
            const quint64 updatedValue = operatorText == QStringLiteral("|")
                ? (currentValue | maskValue)
                : (currentValue & maskValue);
            newData = writeUnsignedLittleEndian(updatedValue, 4);
        }
        else if (valueType == REG_QWORD)
        {
            const quint64 currentValue = currentData.size() >= 8 ? readUnsignedLittleEndian(currentData, 8) : 0;
            const quint64 maskValue = readUnsignedLittleEndian(newData, 8);
            const quint64 updatedValue = operatorText == QStringLiteral("|")
                ? (currentValue | maskValue)
                : (currentValue & maskValue);
            newData = writeUnsignedLittleEndian(updatedValue, 8);
        }
        else if (valueType == REG_BINARY)
        {
            if (currentData.size() < newData.size())
            {
                const int oldSize = currentData.size();
                currentData.resize(newData.size());
                std::fill(currentData.begin() + oldSize, currentData.end(), '\0');
            }
            for (int index = 0; index < newData.size(); ++index)
            {
                const unsigned char currentByte = index < currentData.size() ? static_cast<unsigned char>(currentData.at(index)) : 0U;
                const unsigned char maskByte = static_cast<unsigned char>(newData.at(index));
                currentData[index] = static_cast<char>(operatorText == QStringLiteral("|")
                    ? (currentByte | maskByte)
                    : (currentByte & maskByte));
            }
            newData = currentData.left(newData.size());
        }
        else
        {
            if (errorTextOut != nullptr) *errorTextOut = QStringLiteral("Operator 仅支持 REG_DWORD/REG_QWORD/REG_BINARY。");
            return false;
        }
    }

    return writeRegistryValue(keyPath, valueName, valueType, newData, viewFlags, errorTextOut);
}

bool RegistryOptimizationPage::executeRegistryDeleteAction(const QJsonObject& actionObject, QString* errorTextOut)
{
    const QString keyPath = jsonString(actionObject, QStringLiteral("Key"));
    const QString valueName = jsonString(actionObject, QStringLiteral("Value"));
    if (keyPath.isEmpty())
    {
        if (errorTextOut != nullptr) *errorTextOut = QStringLiteral("RegDelete 缺少 Key。");
        return false;
    }
    const bool hasValueName = actionObject.contains(QStringLiteral("Value"));
    return deleteRegistryValueOrKey(keyPath, valueName, hasValueName, actionRegistryViewFlags(actionObject), errorTextOut);
}

bool RegistryOptimizationPage::executeRegistryMoveAction(const QJsonObject& actionObject, QString* errorTextOut)
{
    const QString keyPath = jsonString(actionObject, QStringLiteral("Key"));
    const QString newKeyPath = jsonString(actionObject, QStringLiteral("NewKey"));
    const QString valueName = jsonString(actionObject, QStringLiteral("Value"));
    const QString newValueName = jsonString(actionObject, QStringLiteral("NewValue"));
    const REGSAM viewFlags = actionRegistryViewFlags(actionObject);

    if (!valueName.isEmpty() || !newValueName.isEmpty())
    {
        DWORD type = REG_NONE;
        QByteArray data;
        if (!readRegistryValue(keyPath, valueName, viewFlags, &type, &data, errorTextOut)) return false;
        const QString targetKey = newKeyPath.isEmpty() ? keyPath : newKeyPath;
        if (!writeRegistryValue(targetKey, newValueName, type, data, viewFlags, errorTextOut)) return false;
        return deleteRegistryValueOrKey(keyPath, valueName, true, viewFlags, errorTextOut);
    }

    if (keyPath.isEmpty() || newKeyPath.isEmpty())
    {
        if (errorTextOut != nullptr) *errorTextOut = QStringLiteral("RegMove 缺少 Key 或 NewKey。");
        return false;
    }
    return renameRegistryKeySameParent(keyPath, newKeyPath, viewFlags, errorTextOut);
}

bool RegistryOptimizationPage::executeServiceStartAction(const QJsonObject& actionObject, QString* errorTextOut)
{
    const QString serviceName = jsonString(actionObject, QStringLiteral("Name"));
    const QString typeText = jsonString(actionObject, QStringLiteral("Type"));
    if (serviceName.isEmpty() || typeText.isEmpty())
    {
        if (errorTextOut != nullptr) *errorTextOut = QStringLiteral("SetServiceStart 缺少 Name 或 Type。");
        return false;
    }

    bool ok = false;
    const DWORD startType = typeText.toULong(&ok, 10);
    if (!ok)
    {
        if (errorTextOut != nullptr) *errorTextOut = QStringLiteral("服务启动类型无效：%1").arg(typeText);
        return false;
    }

    SC_HANDLE managerHandle = ::OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT);
    if (managerHandle == nullptr)
    {
        if (errorTextOut != nullptr) *errorTextOut = winErrorText(static_cast<LONG>(::GetLastError()));
        return false;
    }
    SC_HANDLE serviceHandle = ::OpenServiceW(
        managerHandle,
        reinterpret_cast<const wchar_t*>(serviceName.utf16()),
        SERVICE_CHANGE_CONFIG);
    if (serviceHandle == nullptr)
    {
        const DWORD errorCode = ::GetLastError();
        ::CloseServiceHandle(managerHandle);
        if (errorTextOut != nullptr) *errorTextOut = winErrorText(static_cast<LONG>(errorCode));
        return false;
    }

    const BOOL changeOk = ::ChangeServiceConfigW(
        serviceHandle,
        SERVICE_NO_CHANGE,
        startType,
        SERVICE_NO_CHANGE,
        nullptr,
        nullptr,
        nullptr,
        nullptr,
        nullptr,
        nullptr,
        nullptr);
    const DWORD errorCode = changeOk ? ERROR_SUCCESS : ::GetLastError();
    ::CloseServiceHandle(serviceHandle);
    ::CloseServiceHandle(managerHandle);
    if (!changeOk)
    {
        if (errorTextOut != nullptr) *errorTextOut = winErrorText(static_cast<LONG>(errorCode));
        return false;
    }
    return true;
}

bool RegistryOptimizationPage::executeExplorerNotifyAction(const QJsonObject& actionObject, QString* errorTextOut)
{
    const QString typeText = jsonString(actionObject, QStringLiteral("Type"));
    if (typeText.compare(QStringLiteral("Cmd"), Qt::CaseInsensitive) == 0)
    {
        const QString commandText = jsonString(actionObject, QStringLiteral("Cmd"));
        if (commandText.isEmpty())
        {
            if (errorTextOut != nullptr) *errorTextOut = QStringLiteral("ExplorerNotify Cmd 缺少命令。");
            return false;
        }
        const int exitCode = QProcess::execute(QStringLiteral("cmd.exe"), QStringList{ QStringLiteral("/c"), commandText });
        if (exitCode != 0)
        {
            if (errorTextOut != nullptr) *errorTextOut = QStringLiteral("命令退出码：%1").arg(exitCode);
            return false;
        }
        return true;
    }

    if (typeText.compare(QStringLiteral("AssocChanged"), Qt::CaseInsensitive) == 0)
    {
        ::SHChangeNotify(SHCNE_ASSOCCHANGED, SHCNF_IDLIST, nullptr, nullptr);
        return true;
    }

    if (typeText.compare(QStringLiteral("Custom"), Qt::CaseInsensitive) == 0)
    {
        quint64 messageValue = 0;
        quint64 wParamValue = 0;
        const bool messageOk = parseHexInteger(jsonString(actionObject, QStringLiteral("msg")), &messageValue);
        parseHexInteger(jsonString(actionObject, QStringLiteral("wParam"), QStringLiteral("0")), &wParamValue);
        if (!messageOk)
        {
            if (errorTextOut != nullptr) *errorTextOut = QStringLiteral("ExplorerNotify Custom 缺少有效 msg。");
            return false;
        }

        const QString lParamText = jsonString(actionObject, QStringLiteral("lParam"));
        const QByteArray lParamUtf16 = QByteArray(
            reinterpret_cast<const char*>(lParamText.utf16()),
            (lParamText.size() + 1) * static_cast<int>(sizeof(wchar_t)));
        DWORD_PTR sendResult = 0;
        ::SendMessageTimeoutW(
            HWND_BROADCAST,
            static_cast<UINT>(messageValue),
            static_cast<WPARAM>(wParamValue),
            lParamText.isEmpty() ? 0 : reinterpret_cast<LPARAM>(lParamUtf16.constData()),
            SMTO_ABORTIFHUNG,
            1500,
            &sendResult);
        return true;
    }

    if (errorTextOut != nullptr) *errorTextOut = QStringLiteral("暂不支持 ExplorerNotify 类型：%1").arg(typeText);
    return false;
}

bool RegistryOptimizationPage::executeFileCreateByZipAction(const QJsonObject& actionObject, QString* errorTextOut)
{
    const QString destinationPath = expandEnvironmentPath(jsonString(actionObject, QStringLiteral("Path")));
    const QString zipReference = jsonString(actionObject, QStringLiteral("ZIPFile"));
    if (destinationPath.isEmpty() || zipReference.isEmpty())
    {
        if (errorTextOut != nullptr) *errorTextOut = QStringLiteral("FileCreateByZIP 缺少 Path 或 ZIPFile。");
        return false;
    }

    QString zipRelativePath;
    QString entryPath;
    if (!splitZipFileReference(zipReference, &zipRelativePath, &entryPath))
    {
        if (errorTextOut != nullptr) *errorTextOut = QStringLiteral("ZIPFile 格式无效：%1").arg(zipReference);
        return false;
    }

    QString zipPath;
    for (const QString& candidate : registryOptimizationAssetCandidates(zipRelativePath))
    {
        if (QFileInfo::exists(candidate))
        {
            zipPath = QDir::cleanPath(candidate);
            break;
        }
    }
    if (zipPath.isEmpty())
    {
        if (errorTextOut != nullptr)
        {
            *errorTextOut = QStringLiteral("未找到 ZIP 资产：profiles/registry_optimization_assets/%1").arg(zipRelativePath);
        }
        return false;
    }

    QTemporaryDir temporaryDir;
    if (!temporaryDir.isValid())
    {
        if (errorTextOut != nullptr) *errorTextOut = QStringLiteral("无法创建临时目录用于解压 ZIP。");
        return false;
    }

    const QString normalizedEntryPath = QDir::fromNativeSeparators(entryPath);
    QProcess extractProcess;
    extractProcess.setProgram(QStringLiteral("tar.exe"));
    extractProcess.setArguments(QStringList{
        QStringLiteral("-xf"),
        QDir::toNativeSeparators(zipPath),
        QStringLiteral("-C"),
        QDir::toNativeSeparators(temporaryDir.path()),
        normalizedEntryPath
    });
    extractProcess.start();
    if (!extractProcess.waitForFinished(30000) || extractProcess.exitCode() != 0)
    {
        if (errorTextOut != nullptr)
        {
            *errorTextOut = QStringLiteral("tar 解压失败：%1").arg(QString::fromLocal8Bit(extractProcess.readAllStandardError()).trimmed());
        }
        return false;
    }

    const QString extractedPath = QDir(temporaryDir.path()).filePath(normalizedEntryPath);
    if (!QFileInfo::exists(extractedPath))
    {
        if (errorTextOut != nullptr) *errorTextOut = QStringLiteral("ZIP 中未找到条目：%1").arg(normalizedEntryPath);
        return false;
    }

    const QFileInfo destinationInfo(destinationPath);
    QDir destinationDirectory = destinationInfo.dir();
    if (!destinationDirectory.exists() && !destinationDirectory.mkpath(QStringLiteral(".")))
    {
        if (errorTextOut != nullptr) *errorTextOut = QStringLiteral("无法创建目标目录：%1").arg(destinationDirectory.absolutePath());
        return false;
    }
    if (QFileInfo::exists(destinationPath) && !QFile::remove(destinationPath))
    {
        if (errorTextOut != nullptr) *errorTextOut = QStringLiteral("无法覆盖目标文件：%1").arg(destinationPath);
        return false;
    }
    if (!QFile::copy(extractedPath, destinationPath))
    {
        if (errorTextOut != nullptr) *errorTextOut = QStringLiteral("无法复制 ZIP 条目到目标：%1").arg(destinationPath);
        return false;
    }
    return true;
}

bool RegistryOptimizationPage::evaluateConditionText(const QString& conditionText)
{
    const QString trimmedCondition = conditionText.trimmed();
    if (trimmedCondition.isEmpty()) return false;
    ConditionParser parser(trimmedCondition);
    return parser.evaluate();
}
