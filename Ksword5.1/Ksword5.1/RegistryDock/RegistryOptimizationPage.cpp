#include "../UI/StructuredFieldView.h"
#include "../UI/FlatButtonTheme.h"
#include "../UI/ThemeBinding.h"
#include "RegistryOptimizationPage.h"
#include "../UI/AsyncOperation.h"
#include <QShowEvent>
#include <QEvent>
#include <QScrollBar>
#include "../UI/PageControlStyle.h"
#include "../UI/PrimaryPageStyle.h"
#include "../UI/ToolbarMetrics.h"
#include "RegistryOptimizationTransactions.h"
#include "../Framework/PrivilegeElevationPrompt.h"
#include "../Internationalization/LanguageManager.h"
#include "../UI/CodeEditorWidget.h"
#include "../UI/TableInteractionSupport.h"
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
#include <QSaveFile>
#include <QPushButton>
#include "../UI/SegmentedControl.h"
#include <QRegularExpression>
#include <QSignalBlocker>
#include <QSize>
#include <QSizePolicy>
#include <QSplitter>
#include <QTableWidget>
#include <QTemporaryDir>
#include <QThreadPool>
#include <QTimer>
#include <QTreeWidget>
#include <QVBoxLayout>

#include <algorithm>
#include <utility>
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
#include <objbase.h>

namespace
{
    ks::ui::FieldNode nativeJsonNode(const QString& name, const QJsonValue& value)
    {
        ks::ui::FieldNode node;
        node.name = name;
        if (value.isObject()) {
            node.kind = ks::ui::FieldNode::Kind::Section;
            const auto object = value.toObject();
            for (auto it = object.constBegin(); it != object.constEnd(); ++it) node.children.append(nativeJsonNode(it.key(), it.value()));
        } else if (value.isArray()) {
            node.kind = ks::ui::FieldNode::Kind::Section;
            const auto array = value.toArray();
            for (qsizetype i = 0; i < array.size(); ++i) node.children.append(nativeJsonNode(QString::number(i), array[i]));
        } else if (value.isBool()) node.value = value.toBool() ? QStringLiteral("true") : QStringLiteral("false");
        else if (value.isDouble()) node.value = QString::number(value.toDouble(), 'g', 17);
        else if (value.isString()) node.value = value.toString();
        else node.value = QStringLiteral("null");
        return node;
    }

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

    // kExplorerBroadcastTimeoutMs：
    // - 作用：HWND_BROADCAST 广播时每个顶层窗口的应答超时上限（毫秒）；
    // - 说明：广播已改在后台线程执行，超时只影响该线程，不再冻结 UI。
    constexpr UINT kExplorerBroadcastTimeoutMs = 1500;

    // kStateApplyGenerationProperty：
    // - 作用：优化动作序列的代次，挂在页面对象的动态属性上；
    // - 说明：后台广播回投 UI 线程时比对代次，避免把过期结果推进到新一轮应用序列。
    constexpr const char* kStateApplyGenerationProperty = "kswordStateApplyGeneration";

    // ExplorerNotifyOutcome：
    // - 作用：承载 ExplorerNotify 广播执行结果的纯值类型对象，可安全跨线程回投；
    // - 入参：无；
    // - 返回：无。succeeded 表示动作是否成功，errorTextValue 为失败原因。
    struct ExplorerNotifyOutcome
    {
        bool succeeded = false;     // 广播动作是否成功执行。
        QString errorTextValue;     // 失败原因文本，成功时为空。
    };

    // actionUsesBlockingBroadcast：
    // - 作用：判断动作是否属于会阻塞调用线程的 ExplorerNotify 广播（AssocChanged / Custom）；
    // - 入参 actionObject：单条优化动作的 JSON 对象；
    // - 返回：true 表示必须放到后台线程执行，否则 UI 会被顶层窗口的应答超时拖住。
    [[maybe_unused]] bool actionUsesBlockingBroadcast(const QJsonObject& actionObject)
    {
        const QString familyText = jsonString(actionObject, QStringLiteral("action_family"),
            jsonString(actionObject, QStringLiteral("action_tag"))).trimmed();
        if (familyText.compare(QStringLiteral("ExplorerNotify"), Qt::CaseInsensitive) != 0)
        {
            return false;
        }

        const QString typeText = jsonString(actionObject, QStringLiteral("Type"));
        return typeText.compare(QStringLiteral("AssocChanged"), Qt::CaseInsensitive) == 0 ||
            typeText.compare(QStringLiteral("Custom"), Qt::CaseInsensitive) == 0;
    }

    // runExplorerNotifyBroadcast：
    // - 作用：真正执行 ExplorerNotify 的 shell 通知与顶层窗口广播，调用线程会被阻塞；
    // - 入参 actionObject：单条 ExplorerNotify 动作；
    //         initializeComApartment：本线程是否需要自行初始化 COM 套间（后台线程必须传 true）；
    // - 返回：执行结果值对象。全程只碰 Win32/Shell API，不触碰任何 QWidget。
    ExplorerNotifyOutcome runExplorerNotifyBroadcast(const QJsonObject& actionObject, const bool initializeComApartment)
    {
        ExplorerNotifyOutcome outcome;
        const QString typeText = jsonString(actionObject, QStringLiteral("Type"));

        if (typeText.compare(QStringLiteral("AssocChanged"), Qt::CaseInsensitive) == 0)
        {
            bool comApartmentInitialized = false;
            if (initializeComApartment)
            {
                comApartmentInitialized = SUCCEEDED(::CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED));
            }
            ::SHChangeNotify(SHCNE_ASSOCCHANGED, SHCNF_IDLIST, nullptr, nullptr);
            if (comApartmentInitialized)
            {
                ::CoUninitialize();
            }
            outcome.succeeded = true;
            return outcome;
        }

        if (typeText.compare(QStringLiteral("Custom"), Qt::CaseInsensitive) == 0)
        {
            quint64 messageValue = 0;
            quint64 wParamValue = 0;
            const bool messageOk = parseHexInteger(jsonString(actionObject, QStringLiteral("msg")), &messageValue);
            parseHexInteger(jsonString(actionObject, QStringLiteral("wParam"), QStringLiteral("0")), &wParamValue);
            if (!messageOk)
            {
                outcome.errorTextValue = QStringLiteral("ExplorerNotify Custom 缺少有效 msg。");
                return outcome;
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
                kExplorerBroadcastTimeoutMs,
                &sendResult);
            outcome.succeeded = true;
            return outcome;
        }

        outcome.errorTextValue = QStringLiteral("暂不支持 ExplorerNotify 类型：%1").arg(typeText);
        return outcome;
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
    bool readRegistryValue(const QString& keyPath, const QString& valueName,
        const REGSAM accessFlags, DWORD* typeOut, QByteArray* dataOut, QString* errorTextOut)
    {
        if (!typeOut || !dataOut) return false;
        const RegistryAccessContext context{(accessFlags & KEY_WOW64_32KEY) ? 32
            : (accessFlags & KEY_WOW64_64KEY) ? 64 : 0, false}; // 配置明确 Win32 视图。
        RegistryValueState value; // 直接读取共享访问层，不裁剪值名或原始字节。
        if (!RegistryWorkbenchAccess::read(keyPath, valueName, context, &value, errorTextOut)
            || !value.exists || !value.complete) return false;
        *typeOut = value.type;
        *dataOut = value.data;
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
    QStringList registryOptimizationAssetCandidates(const QString& zipRelativePath, const QString& appDir, const QString& currentDir)
    {
        QStringList candidates;
        const QString normalizedZipPath = QDir::fromNativeSeparators(zipRelativePath);
        candidates << QDir(appDir).filePath(QStringLiteral("profiles/registry_optimization_assets/%1").arg(normalizedZipPath));
        candidates << QDir(appDir).filePath(QStringLiteral("../profiles/registry_optimization_assets/%1").arg(normalizedZipPath));
        candidates << QDir(currentDir).filePath(QStringLiteral("profiles/registry_optimization_assets/%1").arg(normalizedZipPath));
        candidates << QDir(currentDir).filePath(QStringLiteral("Ksword5.1/Ksword5.1/profiles/registry_optimization_assets/%1").arg(normalizedZipPath));
        candidates.removeDuplicates();
        return candidates;
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

        const REGSAM viewFlags =
            parameters.value(QStringLiteral("wow64")).compare(QStringLiteral("True"), Qt::CaseInsensitive) == 0
            ? KEY_WOW64_32KEY
            : 0;

        if (!parameters.contains(QStringLiteral("value")))
        {
            const RegistryAccessContext context{viewFlags == KEY_WOW64_32KEY ? 32 : 0, false};
            bool exists = false; // 条件检查不将访问失败当作存在。
            return RegistryWorkbenchAccess::keyExists(keyPath, context, &exists, nullptr) && exists;
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
    m_profileOperation = std::make_unique<ks::ui::AsyncOperation>(this);
    m_ordinaryOperation = std::make_unique<ks::ui::AsyncOperation>(this);
    m_ordinaryOperation->setStateChangedCallback([this] {
        if (m_stateApplyInProgress && !m_ordinaryOperation->isBusy() && !m_ordinaryReceipt)
            cancelStateApply(QStringLiteral("注册表操作未取得完整回执；请刷新后核对。"));
    });
    initializeUi();
    initializeConnections();
}

RegistryOptimizationPage::~RegistryOptimizationPage()
{
    if (m_externalSession) { m_externalSession->stop(); m_externalSession.reset(); }
    m_ordinaryOperation->close();
    if (m_scopeOperation) m_scopeOperation->close();
    m_profileOperation->close();
    cancelStateApply();
}

void RegistryOptimizationPage::showEvent(QShowEvent* event)
{
    if (!m_profileRequested) loadOptimizationProfile();
    else if (m_profilePresentationDirty) presentOptimizationProfile();
    if (m_scopeProjection && m_scopeProjectionTimer) m_scopeProjectionTimer->start(0);
    scheduleVisibleRowWidgets();
    QWidget::showEvent(event);
}

void RegistryOptimizationPage::presentOptimizationProfile()
{
    if (!isVisible() || !m_profilePresentationDirty) return;
    m_profilePresentationDirty = false;
    rebuildGroupTree();
    rebuildItemTable();
    applyColumnPreset(m_columnPreset);
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
    ks::ui::InstallSegmentedButtons(presetButtonLayout, {m_columnPresetAButton, m_columnPresetBButton});

    m_filterEdit = new QLineEdit(this);
    // 优化项/分组结果过滤共用此字段，不改变原有去抖和列预设。
    ks::ui::StyleSearchField(m_filterEdit);
    m_filterEdit->setPlaceholderText(QStringLiteral("过滤组名、项目名、作用域或条件"));
    m_filterDebounceTimer = new QTimer(this);
    m_filterDebounceTimer->setSingleShot(true);
    m_filterDebounceTimer->setInterval(kFilterDebounceMs);

    m_reloadButton = new QPushButton(QStringLiteral("重新加载 JSON"), this);
    m_refreshStateButton = new QPushButton(QStringLiteral("刷新可见状态"), this);
    m_cancelApplyButton = new QPushButton(QStringLiteral("取消应用"), this);
    m_reloadButton->setToolTip(QStringLiteral("从配置文件重新读取优化项清单"));
    m_refreshStateButton->setToolTip(QStringLiteral("重新读取注册表，刷新列表中各优化项的当前状态"));
    m_cancelApplyButton->setToolTip(QStringLiteral("中止正在进行的批量应用操作"));
    m_reloadButton->setStyleSheet(KswordTheme::ThemedButtonStyle());
    m_refreshStateButton->setStyleSheet(KswordTheme::ThemedButtonStyle());
    m_cancelApplyButton->setStyleSheet(KswordTheme::ThemedButtonStyle());
    m_cancelApplyButton->setEnabled(false);

    toolLayout->addWidget(presetButtonWidget, 0);
    toolLayout->addWidget(m_filterEdit, 1);
    toolLayout->addWidget(m_cancelApplyButton, 0);
    toolLayout->addWidget(m_refreshStateButton, 0);
    toolLayout->addWidget(m_reloadButton, 0);
    // A/B 属于紧贴的一个分段控件；其余控制项按统一间距排布。
    ks::ui::NormalizeToolbarRow(presetButtonLayout, 0);
    ks::ui::NormalizeToolbarRow(toolLayout);
    rootLayout->addLayout(toolLayout, 0);

    m_splitter = new QSplitter(Qt::Horizontal, this);
    rootLayout->addWidget(m_splitter, 1);

    m_groupTree = new QTreeWidget(m_splitter);
    m_groupTree->setColumnCount(1);
    m_groupTree->setHeaderLabel(QStringLiteral("优化分组"));
    m_groupTree->setMinimumWidth(260);
    // 分组导航与结果列统一采用表头角色，强调色只表示实际选择。
    m_groupTree->header()->setStyleSheet(QString());

    QWidget* rightWidget = new QWidget(m_splitter);
    QVBoxLayout* rightLayout = new QVBoxLayout(rightWidget);
    rightLayout->setContentsMargins(0, 0, 0, 0);
    rightLayout->setSpacing(6);

    m_itemTable = new ks::ui::VisibleTableWidget(rightWidget);
    // 优化项自带目标和应用动作列，隐藏不适用的通用快照操作栏。
    ks::ui::SetTableActionBarMode(m_itemTable, ks::ui::TableActionBarMode::None);
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
    m_itemTable->horizontalHeader()->setStyleSheet(QString());
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

    // 优化详情由程序按当前行生成，统一编辑器会保留规范源文本并在切换语言时重绘。
    m_detailText = new ks::ui::StructuredFieldView(rightWidget);


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
    connect(m_cancelApplyButton, &QPushButton::clicked, this, [this]() { cancelStateApply(QStringLiteral("用户已取消应用。")); });
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
    m_profileRequested = true;
    cancelStateRefresh();
    cancelStateApply(QStringLiteral("已取消正在进行的应用，因为优化配置已重新加载。"));
    if (m_stateApplyInProgress) { m_deferredProfileReload = true; return; }
    ++m_scopeProjectionRevision;
    if (m_scopeOperation) m_scopeOperation->cancel();
    if (m_scopeProjectionTimer) m_scopeProjectionTimer->stop();
    if (m_rowWidgetTimer) m_rowWidgetTimer->stop();
    m_rebuildingTable = true;
    m_itemTable->setEnabled(false);
    m_refreshStateButton->setEnabled(false);
    auto oldProjection = std::move(m_scopeProjection);
    auto oldItems = std::move(m_itemList);
    auto oldRows = std::move(m_visibleRows);
    QThreadPool::globalInstance()->start([projection = std::move(oldProjection), items = std::move(oldItems), rows = std::move(oldRows)]() {});
    if (m_filterDebounceTimer != nullptr) m_filterDebounceTimer->stop();
    m_loadedProfilePath.clear();
    m_groupTree->clear();
    m_detailText->setDocument({});
    m_profilePresentationDirty = false;
    m_profileOperation->submit<QStringList, ProfileLoadResult>(profileCandidatePaths(),
        [](const QStringList& candidates, const ks::ui::AsyncOperationToken& token) {
            ProfileLoadResult result;
            for (const QString& candidate : candidates)
            {
                if (token.isCanceled()) return result;
                const QString path = ks::profile::resolveProfileJsonPath(candidate);
                if (!path.isEmpty()) { result.path = QDir::cleanPath(path); result.found = true; break; }
            }
            if (!result.found || token.isCanceled()) return result;
            QJsonParseError parseError{};
            QString readError;
            const QJsonDocument document = ks::profile::readProfileJsonDocument(result.path, &parseError, &readError);
            if (parseError.error != QJsonParseError::NoError || !document.isArray())
            {
                result.error = readError.isEmpty() ? parseError.errorString() : readError;
                return result;
            }
            result.parsed = true;
            const QJsonArray itemArray = document.array();
            for (const QJsonValue& itemValue : itemArray)
            {
                if (token.isCanceled()) return result;
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
                result.items.push_back(std::move(item));
            }

            return result;
        },
        [this](const ProfileLoadResult& result, std::uint64_t) {
            if (!result.found)
                updateStatusText(QStringLiteral("未找到 profiles/%1；系统优化页已加载，但没有可显示项目。")
                    .arg(QString::fromLatin1(kOptimizationProfileFileName)));
            else if (!result.parsed)
                updateStatusText(QStringLiteral("系统优化 JSON 解析失败：%1 (%2)").arg(result.error, result.path));
            else
            {
                m_loadedProfilePath = result.path;
                m_itemList = result.items;
                m_profilePresentationDirty = true;
                presentOptimizationProfile();
                updateStatusText(QStringLiteral("已加载系统优化 JSON：%1；项目 %2 个。")
                    .arg(result.path).arg(m_itemList.size()));
            }
            return true;
        },
        [this](std::exception_ptr failure) {
            QString reason;
            try { if (failure) std::rethrow_exception(failure); }
            catch (const std::exception& error) { reason = QString::fromUtf8(error.what()); }
            catch (...) {}
            updateStatusText(QStringLiteral("系统优化 JSON 解析失败：%1 (%2)").arg(reason, QString()));
        });
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
    // 过滤防抖定时器可能在菜单嵌套事件循环中到期。
    // 清空可见行和单元格控件前统一等待菜单关闭。
    const QPointer<RegistryOptimizationPage> safeThis(this);
    if (ks::ui::DeferTableUiCommitIfContextMenuOpen(
        this,
        QStringLiteral("registry-optimization-table-rebuild"),
        {m_itemTable},
        [safeThis]()
        {
            if (!safeThis.isNull())
            {
                safeThis->rebuildItemTable();
            }
        }))
    {
        return;
    }

    cancelStateRefresh();
    cancelStateApply(QStringLiteral("已取消正在进行的应用，因为筛选结果已改变。"));
    if (m_stateApplyInProgress) { m_deferredItemRebuild = true; return; }
    const quint64 revision = ++m_scopeProjectionRevision;
    m_rebuildingTable = true;
    m_itemTable->setEnabled(false);
    m_refreshStateButton->setEnabled(false);
    if (m_scopeProjectionTimer) m_scopeProjectionTimer->stop();
    auto retiredProjection = std::move(m_scopeProjection);
    QThreadPool::globalInstance()->start([retiredProjection = std::move(retiredProjection)]() {});
    const QTreeWidgetItem* selectedGroupItem = m_groupTree->currentItem();
    const QString selectedGroup = selectedGroupItem ? selectedGroupItem->data(0, kRoleGroupName).toString() : QString();
    const QString filterText = m_filterEdit->text().trimmed();
    if (!m_scopeOperation) {
        m_scopeOperation = std::make_unique<ks::ui::AsyncOperation>(this);
        m_scopeOperation->setStateChangedCallback([this]() {
            if (m_rebuildingTable && !m_scopeProjection && !m_scopeOperation->isInFlight())
                updateStatusText(ks::i18n::sourceText(QStringLiteral("系统优化：列表准备失败，请重新筛选或加载。")));
        });
    }
    struct Request { QVector<OptimizationItem> items; QString group; QString filter; QString undetected; quint64 revision; };
    m_scopeOperation->submit<Request, ScopeProjection>(
        {m_itemList, selectedGroup, filterText, ks::i18n::sourceText(QStringLiteral("未检测")), revision},
        [](const Request& request, const ks::ui::AsyncOperationToken& token) {
            ScopeProjection result;
            result.revision = request.revision;
            for (qsizetype itemIndex = 0; itemIndex < request.items.size(); ++itemIndex) {
                if (token.isCanceled()) return result;
                const auto& item = request.items.at(itemIndex);
                if (!request.group.isEmpty() && item.groupNameText.compare(request.group, Qt::CaseInsensitive) != 0) continue;
                for (qsizetype scopeIndex = 0; scopeIndex < item.scopeList.size(); ++scopeIndex) {
                    const auto& scope = item.scopeList.at(scopeIndex);
                    const QString haystack = QStringLiteral("%1\n%2\n%3\n%4\n%5\n%6")
                        .arg(item.groupNameText, item.itemNameText, item.itemTypeText, scope.scopeText, item.itemConditionText, item.warningText);
                    if (!request.filter.isEmpty() && !haystack.contains(request.filter, Qt::CaseInsensitive)) continue;
                    PreparedScopeRow row;
                    row.source.itemIndex = static_cast<int>(itemIndex);
                    row.source.scopeIndex = static_cast<int>(scopeIndex);
                    if (item.itemTypeText.compare(QStringLiteral("Combo"), Qt::CaseInsensitive) == 0)
                        for (const auto& state : scope.stateList)
                            if (state.tagText.compare(QStringLiteral("Dropdown"), Qt::CaseInsensitive) == 0) { row.source.targetLabel = state.labelText; break; }
                    QStringList conditions;
                    if (!item.groupConditionText.isEmpty()) conditions << QStringLiteral("组条件: %1").arg(item.groupConditionText);
                    if (!item.itemConditionText.isEmpty()) conditions << QStringLiteral("项目条件: %1").arg(item.itemConditionText);
                    if (!scope.conditionText.isEmpty()) conditions << QStringLiteral("作用域条件: %1").arg(scope.conditionText);
                    if (!item.warningText.isEmpty()) conditions << QStringLiteral("警告: %1").arg(item.warningText);
                    row.columns = {item.itemNameText, scopeDisplayText(scope.scopeText), item.itemTypeText, request.undetected,
                        QString(), QString(), conditions.join(QStringLiteral(" | "))};
                    result.rows.push_back(std::move(row));
                }
            }
            return result;
        }, [this](const ScopeProjection& result, std::uint64_t) -> bool {
            if (result.revision != m_scopeProjectionRevision) return true;
            m_scopeProjection = std::make_shared<ScopeProjection>(result);
            m_nextScopeProjectionRow = 0;
            m_scopeProjectionClearing = true;
            if (!m_scopeProjectionTimer) {
                m_scopeProjectionTimer = new QTimer(this);
                m_scopeProjectionTimer->setSingleShot(true);
                connect(m_scopeProjectionTimer, &QTimer::timeout, this, [this]() { continueItemProjection(); });
            }
            m_scopeProjectionTimer->start(0);
            return true;
        }, [this, revision](std::exception_ptr) {
            if (revision != m_scopeProjectionRevision) return;
            updateStatusText(ks::i18n::sourceText(QStringLiteral("系统优化：列表准备失败，请重新筛选或加载。")));
        });
}

void RegistryOptimizationPage::continueItemProjection()
{
    if (!isVisible() || !m_scopeProjection || m_scopeProjection->revision != m_scopeProjectionRevision) return;
    const QPointer<RegistryOptimizationPage> guard(this);
    if (ks::ui::DeferTableUiCommitIfContextMenuOpen(this, QStringLiteral("registry-optimization-scope-projection"), {m_itemTable},
        [guard]() { if (guard) guard->continueItemProjection(); })) return;
    QSignalBlocker blocker(m_itemTable);
    for (int unit = 0; unit < 24; ++unit) {
        if (m_scopeProjectionClearing) {
            if (m_itemTable->rowCount() > 0) { m_itemTable->removeRow(m_itemTable->rowCount() - 1); continue; }
            auto retiredRows = std::move(m_visibleRows);
            QThreadPool::globalInstance()->start([retiredRows = std::move(retiredRows)]() {});
            m_rowsWithWidgets.clear();
            m_scopeProjectionClearing = false;
        }
        if (m_nextScopeProjectionRow >= m_scopeProjection->rows.size()) {
            m_rebuildingTable = false;
            m_itemTable->setEnabled(true);
            m_refreshStateButton->setEnabled(true);
            if (m_itemTable->rowCount() > 0) { m_itemTable->setCurrentCell(0, 0); updateDetailPanel(0); }
            else m_detailText->setDocument({});
            updateStatusText(QStringLiteral("系统优化：当前显示 %1 行；来源 %2。").arg(m_itemTable->rowCount()).arg(m_loadedProfilePath));
            applyColumnPreset(m_columnPreset);
            auto retiredProjection = std::move(m_scopeProjection);
            QThreadPool::globalInstance()->start([retiredProjection = std::move(retiredProjection)]() {});
            scheduleVisibleRowWidgets();
            return;
        }
        const auto& prepared = m_scopeProjection->rows.at(m_nextScopeProjectionRow++);
        const int row = m_itemTable->rowCount();
        m_itemTable->insertRow(row);
        m_itemTable->setRowHeight(row, kDefaultRowHeight);
        m_visibleRows.push_back(prepared.source);
        for (int column = 0; column < prepared.columns.size(); ++column) {
            auto* item = new QTableWidgetItem(prepared.columns.at(column));
            item->setFlags(Qt::ItemIsEnabled | Qt::ItemIsSelectable);
            if (column == kTargetControlColumn) item->setSizeHint(QSize(kTargetColumnWidth, kDefaultRowHeight));
            if (column == kActionButtonColumn) item->setSizeHint(QSize(kActionColumnWidth, kDefaultRowHeight));
            m_itemTable->setItem(row, column, item);
        }
    }
    m_scopeProjectionTimer->start(0);
}

bool RegistryOptimizationPage::eventFilter(QObject* watched, QEvent* event)
{
    if (m_itemTable && watched == m_itemTable->viewport()
        && (event->type() == QEvent::Resize || event->type() == QEvent::Show)) scheduleVisibleRowWidgets();
    return QWidget::eventFilter(watched, event);
}

void RegistryOptimizationPage::scheduleVisibleRowWidgets()
{
    if (!m_rowWidgetTimer) {
        m_rowWidgetTimer = new QTimer(this);
        m_rowWidgetTimer->setSingleShot(true);
        connect(m_rowWidgetTimer, &QTimer::timeout, this, [this]() { materializeVisibleRowWidgets(); });
        m_itemTable->viewport()->installEventFilter(this);
        connect(m_itemTable->verticalScrollBar(), &QScrollBar::valueChanged, this, [this]() { scheduleVisibleRowWidgets(); });
    }
    m_rowWidgetTimer->start(0);
}

void RegistryOptimizationPage::materializeVisibleRowWidgets()
{
    if (m_rebuildingTable || !m_itemTable->isVisible() || m_itemTable->rowCount() == 0) return;
    const QPointer<RegistryOptimizationPage> guard(this);
    if (ks::ui::DeferTableUiCommitIfContextMenuOpen(this, QStringLiteral("registry-optimization-visible-editors"), {m_itemTable},
        [guard]() { if (guard) guard->materializeVisibleRowWidgets(); })) return;
    const quint64 revision = m_scopeProjectionRevision;
    const int first = std::max(0, m_itemTable->rowAt(0));
    const int bottom = m_itemTable->rowAt(m_itemTable->viewport()->height() - 1);
    const int last = bottom >= 0 ? bottom : m_itemTable->rowCount() - 1;
    for (const int row : QSet<int>(m_rowsWithWidgets)) {
        if (row >= first && row <= last) continue;
        m_itemTable->removeCellWidget(row, kTargetControlColumn);
        m_itemTable->removeCellWidget(row, kActionButtonColumn);
        m_rowsWithWidgets.remove(row);
    }
    for (int row = first; row <= last; ++row) {
        if (m_rowsWithWidgets.contains(row) || row >= m_visibleRows.size()) continue;
        const auto& source = m_visibleRows.at(row);
        const auto& item = m_itemList.at(source.itemIndex);
        const auto& scope = item.scopeList.at(source.scopeIndex);
        if (item.itemTypeText.compare(QStringLiteral("Combo"), Qt::CaseInsensitive) == 0) {
            auto* combo = new QComboBox(m_itemTable);
            ks::ui::StylePrimaryCombo(combo);
            combo->setObjectName(QStringLiteral("optimizationTargetCombo"));
            combo->setMinimumWidth(kTargetColumnWidth - 16);
            for (const auto& state : scope.stateList)
                if (state.tagText.compare(QStringLiteral("Dropdown"), Qt::CaseInsensitive) == 0) combo->addItem(state.labelText, state.labelText);
            const int index = combo->findData(source.targetLabel);
            if (index >= 0) combo->setCurrentIndex(index);
            connect(combo, &QComboBox::currentIndexChanged, this, [this, combo, row, revision]() {
                if (revision == m_scopeProjectionRevision && !m_rebuildingTable && row < m_visibleRows.size()) m_visibleRows[row].targetLabel = combo->currentData().toString();
            });
            m_itemTable->setCellWidget(row, kTargetControlColumn, buildCenteredCellWidget(combo, m_itemTable));
        } else {
            auto* check = new QCheckBox(QStringLiteral("启用"), m_itemTable);
            check->setObjectName(QStringLiteral("optimizationTargetCheck"));
            check->setChecked(source.targetEnabled);
            connect(check, &QCheckBox::toggled, this, [this, row, revision](bool checked) {
                if (revision == m_scopeProjectionRevision && !m_rebuildingTable && row < m_visibleRows.size()) m_visibleRows[row].targetEnabled = checked;
            });
            m_itemTable->setCellWidget(row, kTargetControlColumn, buildCenteredCellWidget(check, m_itemTable));
        }
        auto* apply = new QPushButton(QStringLiteral("应用"), m_itemTable);
        apply->setObjectName(QStringLiteral("registryOptimizationApplyButton"));
        apply->setToolTip(QStringLiteral("对这一项执行注册表优化设置"));
        apply->setStyleSheet(KswordTheme::ThemedButtonStyle());
        apply->setFixedWidth(kActionColumnWidth - 14);
        apply->setEnabled(!m_stateApplyInProgress);
        connect(apply, &QPushButton::clicked, this, [this, row, revision]() { if (revision == m_scopeProjectionRevision && !m_rebuildingTable) applyVisibleRow(row); });
        m_itemTable->setCellWidget(row, kActionButtonColumn, buildCenteredCellWidget(apply, m_itemTable));
        m_rowsWithWidgets.insert(row);
    }
}

void RegistryOptimizationPage::setTargetForRow(const int row, const QString& label, const bool enabled)
{
    if (row < 0 || row >= m_visibleRows.size()) return;
    m_visibleRows[row].targetLabel = label;
    m_visibleRows[row].targetEnabled = enabled;
    QWidget* target = m_itemTable->cellWidget(row, kTargetControlColumn);
    if (auto* combo = findTargetComboBox(target)) {
        const int index = combo->findData(label);
        if (index >= 0) combo->setCurrentIndex(index);
    } else if (auto* check = findTargetCheckBox(target)) check->setChecked(enabled);
}

void RegistryOptimizationPage::refreshVisibleStates()
{
    if (m_rebuildingTable || m_stateRefreshInProgress) return;

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

                guardThis->setTargetForRow(result.tableRow, result.targetLabel, result.targetEnabled);
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

    setTargetForRow(tableRow, detectedState ? detectedState->labelText : QString(),
        detectedState && detectedState->tagText.compare(QStringLiteral("State"), Qt::CaseInsensitive) == 0);
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
        m_detailText->setDocument({});
        return;
    }

    const VisibleRow rowRef = m_visibleRows.at(tableRow);
    const OptimizationItem& item = m_itemList.at(rowRef.itemIndex);
    const OptimizationScope& scope = item.scopeList.at(rowRef.scopeIndex);

    ks::ui::FieldDocument lines;
    lines.field(QStringLiteral("项目"), QStringLiteral("%1").arg(item.itemNameText));
    lines.field(QStringLiteral("分组"), QStringLiteral("%1").arg(item.groupNameText));
    lines.field(QStringLiteral("作用域"), QStringLiteral("%1 (%2)").arg(QStringLiteral("%1").arg(scopeDisplayText(scope.scopeText))).arg(QStringLiteral("%1").arg(scope.scopeText)));
    lines.field(QStringLiteral("类型"), QStringLiteral("%1").arg(item.itemTypeText));
    if (!item.warningText.isEmpty()) lines.field(QStringLiteral("项目警告"), QStringLiteral("%1").arg(item.warningText));
    if (!item.groupConditionText.isEmpty()) lines.field(QStringLiteral("组条件"), QStringLiteral("%1").arg(item.groupConditionText));
    if (!item.itemConditionText.isEmpty()) lines.field(QStringLiteral("项目条件"), QStringLiteral("%1").arg(item.itemConditionText));
    if (!scope.conditionText.isEmpty()) lines.field(QStringLiteral("作用域条件"), QStringLiteral("%1").arg(scope.conditionText));

    lines.section(QStringLiteral("状态/动作"));
    for (const OptimizationState& state : scope.stateList) {
        ks::ui::FieldNode stateNode;
        stateNode.kind = ks::ui::FieldNode::Kind::Section;
        stateNode.name = state.labelText;
        ks::ui::FieldDocument stateFields;
        stateFields.field(QStringLiteral("标签"), state.tagText);
        stateFields.field(QStringLiteral("条件"), state.conditionText);
        if (!state.warningText.isEmpty()) stateFields.field(QStringLiteral("警告"), state.warningText, true);
        stateNode.children = stateFields.nodes;
        for (qsizetype index = 0; index < state.actionList.size(); ++index)
            stateNode.children.append(nativeJsonNode(QStringLiteral("Action %1").arg(index + 1), state.actionList[index]));
        lines.nodes.last().children.append(std::move(stateNode));
    }
    m_detailText->setDocument(lines);
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
    // 业务只更新列组选中态，底面、圆角和过渡由已安装的分段控件统一维护。
    if (m_columnPresetAButton != nullptr)
    {
        ks::ui::SetSegmentedButtonSelected(m_columnPresetAButton, m_columnPreset == ColumnPreset::A);
    }
    if (m_columnPresetBButton != nullptr)
    {
        ks::ui::SetSegmentedButtonSelected(m_columnPresetBButton, m_columnPreset == ColumnPreset::B);
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

    if (item.itemTypeText.compare(QStringLiteral("Combo"), Qt::CaseInsensitive) == 0)
    {
        for (const auto& state : scope.stateList)
            if (state.tagText.compare(QStringLiteral("Dropdown"), Qt::CaseInsensitive) == 0 && state.labelText == rowRef.targetLabel) return &state;
        return nullptr;
    }
    const bool targetEnabled = rowRef.targetEnabled;
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
    if (m_stateApplyInProgress)
    {
        updateStatusText(QStringLiteral("系统优化：已有动作正在应用，请等待完成或点击“取消应用”。"));
        return false;
    }

    cancelStateRefresh();
    if (m_rebuildingTable || tableRow < 0 || tableRow >= m_visibleRows.size()) return false;
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
    if (QMessageBox::question(
            this,
            QStringLiteral("确认系统优化"),
            messageText,
            QMessageBox::Yes | QMessageBox::No,
            QMessageBox::No) != QMessageBox::Yes)
    {
        return false;
    }

    beginStateApply(tableRow, item, scope, *state);
    return true;
}

void RegistryOptimizationPage::beginStateApply(
    const int tableRow,
    const OptimizationItem& item,
    const OptimizationScope& scope,
    const OptimizationState& state)
{
    m_stateApplyInProgress = true;
    m_stateApplyCancelRequested = false;
    m_stateApplyCancelReason.clear();
    m_stateApplyTableRow = tableRow;
    m_stateApplyItemName = item.itemNameText;
    m_stateApplyTargetLabel = state.labelText;
    m_stateApplyActions = state.actionList;
    m_stateApplyNextActionIndex = 0;
    m_stateApplyActiveAction = QJsonObject();
    m_stateApplyDetailLines = {};
    m_stateApplyDetailLines.field(QStringLiteral("应用项目"), item.itemNameText);
    m_stateApplyDetailLines.field(QStringLiteral("作用域"), scopeDisplayText(scope.scopeText));
    m_stateApplyDetailLines.field(QStringLiteral("目标状态"), state.labelText);
    m_stateApplyAllOk = true;
    m_stateApplyRestartExplorer = false;
    // 序列代次自增：上一轮仍在后台执行的广播动作回投时会被识别为过期并丢弃。
    setProperty(kStateApplyGenerationProperty, property(kStateApplyGenerationProperty).toULongLong() + 1);
    setApplyControlsEnabled(false);
    m_detailText->setDocument(m_stateApplyDetailLines);

    if (m_stateApplyActions.isEmpty())
    {
        m_stateApplyDetailLines.note(QStringLiteral("该状态没有动作。"));
        finishStateApply();
        return;
    }

    updateStatusText(QStringLiteral("系统优化：正在应用 %1 -> %2（共 %3 个动作）。")
        .arg(m_stateApplyItemName, m_stateApplyTargetLabel)
        .arg(m_stateApplyActions.size()));
    QTimer::singleShot(0, this, [this]() { continueStateApply(); });
}

void RegistryOptimizationPage::continueStateApply()
{
    if (!m_stateApplyInProgress || m_externalSession
        || m_ordinaryOperation->isBusy() || !m_stateApplyActiveAction.isEmpty()) return;

    if (m_stateApplyNextActionIndex >= m_stateApplyActions.size())
    {
        finishStateApply();
        return;
    }

    m_stateApplyActiveAction = m_stateApplyActions.at(m_stateApplyNextActionIndex);
    const int actionNumber = m_stateApplyNextActionIndex + 1;
    updateStatusText(QStringLiteral("系统优化：正在应用 %1 -> %2（动作 %3/%4）。")
        .arg(m_stateApplyItemName, m_stateApplyTargetLabel)
        .arg(actionNumber)
        .arg(m_stateApplyActions.size()));

    if (actionRequiresExternalProcess(m_stateApplyActiveAction))
    {
        QString startErrorText;
        if (!startExternalAction(m_stateApplyActiveAction, &startErrorText))
        {
            completePendingAction(false, startErrorText);
        }
        return;
    }

    const QString family = jsonString(m_stateApplyActiveAction, QStringLiteral("action_family"),
        jsonString(m_stateApplyActiveAction, QStringLiteral("action_tag"))).trimmed();
    if ((family.compare(QStringLiteral("RegWrite"), Qt::CaseInsensitive) == 0
            || family.compare(QStringLiteral("RegExist"), Qt::CaseInsensitive) == 0)
        && jsonString(m_stateApplyActiveAction, QStringLiteral("Data")) == QStringLiteral("?ColorDialog()"))
        prepareColorAction(m_stateApplyActiveAction);
    else
        submitOrdinaryAction(m_stateApplyActiveAction);
}

void RegistryOptimizationPage::submitOrdinaryAction(const QJsonObject& action, const RegistryValueState* original)
{
    struct Request { QJsonObject action; RegistryValueState original; bool hasOriginal = false; };
    const Request request{action, original ? *original : RegistryValueState{}, original != nullptr};
    const quint64 generation = property(kStateApplyGenerationProperty).toULongLong();
    m_ordinaryReceipt = false;
    const auto ticket = m_ordinaryOperation->submitTransaction<Request, OrdinaryActionResult>(request,
        [](const Request& value) {
            OrdinaryActionResult result;
            result.ok = executeAction(value.action, &result.details, &result.restartExplorer, value.hasOriginal ? &value.original : nullptr);
            return result;
        }, [this, generation](const OrdinaryActionResult& result, std::uint64_t) {
            acceptOrdinaryAction(result, generation);
            return true;
        }, [this](std::exception_ptr) {
            m_ordinaryReceipt = true;
            QTimer::singleShot(0, this, [this] { cancelStateApply(QStringLiteral("注册表操作未取得完整回执；请刷新后核对。")); });
        });
    if (!ticket) cancelStateApply(QStringLiteral("注册表操作未取得完整回执；请刷新后核对。"));
}

void RegistryOptimizationPage::prepareColorAction(const QJsonObject& action)
{
    struct Result { RegistryValueState original; QString error; bool ok = false; };
    const quint64 generation = property(kStateApplyGenerationProperty).toULongLong();
    m_ordinaryReceipt = false;
    m_ordinaryOperation->submit<QJsonObject, Result>(action, [](const QJsonObject& value, const ks::ui::AsyncOperationToken&) {
        Result result;
        const QString key = jsonString(value, QStringLiteral("Key"));
        const RegistryAccessContext context{actionRegistryViewFlags(value) == KEY_WOW64_32KEY ? 32 : 0, false};
        if (!RegistryWorkbenchAccess::read(key, jsonString(value, QStringLiteral("Value")), context, &result.original, &result.error)
            || !result.original.complete) return result;
        if (key.isEmpty()) { result.error = QStringLiteral("RegWrite 缺少 Key。"); return result; }
        DWORD type = REG_NONE;
        const QString typeText = jsonString(value, QStringLiteral("Type"), QStringLiteral("REG_DWORD"));
        if (!parseRegistryType(typeText, &type)) { result.error = QStringLiteral("不支持注册表类型：%1").arg(typeText); return result; }
        result.ok = true;
        return result;
    }, [this, action, generation](const Result& result, std::uint64_t) {
        m_ordinaryReceipt = true;
        QTimer::singleShot(0, this, [this, action, generation, result] {
            if (!m_stateApplyInProgress || property(kStateApplyGenerationProperty).toULongLong() != generation) return;
            if (m_stateApplyCancelRequested) { cancelStateApply(m_stateApplyCancelReason); return; }
            if (!result.ok)
            {
                OrdinaryActionResult outcome;
                outcome.ok = describeActionResult(action, false, result.error, &outcome.details, &outcome.restartExplorer);
                acceptOrdinaryAction(outcome, generation);
                return;
            }
            const QPointer<RegistryOptimizationPage> alive(this);
            const QColor color = QColorDialog::getColor(Qt::white, this, QStringLiteral("选择注册表颜色值"));
            if (!alive || !m_stateApplyInProgress || property(kStateApplyGenerationProperty).toULongLong() != generation) return;
            if (!color.isValid())
            {
                OrdinaryActionResult outcome;
                outcome.ok = describeActionResult(action, false, QStringLiteral("用户取消颜色选择。"), &outcome.details, &outcome.restartExplorer);
                acceptOrdinaryAction(outcome, generation);
                return;
            }
            const quint32 colorValue = 0xFF000000U | (static_cast<quint32>(color.blue()) << 16)
                | (static_cast<quint32>(color.green()) << 8) | static_cast<quint32>(color.red());
            auto resolved = action;
            if (!action.value(QStringLiteral("summary")).isString())
                resolved.insert(QStringLiteral("summary"), QString::fromUtf8(QJsonDocument(action).toJson(QJsonDocument::Compact)));
            resolved.insert(QStringLiteral("Data"), QStringLiteral("%1").arg(colorValue, 8, 16, QLatin1Char('0')).toUpper());
            submitOrdinaryAction(resolved, &result.original);
        });
        return true;
    }, [this](std::exception_ptr) {
        m_ordinaryReceipt = true;
        QTimer::singleShot(0, this, [this] { cancelStateApply(QStringLiteral("注册表操作未取得完整回执；请刷新后核对。")); });
    });
}

void RegistryOptimizationPage::acceptOrdinaryAction(const OrdinaryActionResult& result, const quint64 generation)
{
    m_ordinaryReceipt = true;
    if (!m_stateApplyInProgress || property(kStateApplyGenerationProperty).toULongLong() != generation) return;
    m_stateApplyRestartExplorer = m_stateApplyRestartExplorer || result.restartExplorer;
    m_stateApplyAllOk = m_stateApplyAllOk && result.ok;
    m_stateApplyDetailLines.nodes += result.details.nodes;
    m_stateApplyActiveAction = {};
    ++m_stateApplyNextActionIndex;
    m_detailText->setDocument(m_stateApplyDetailLines);
    QTimer::singleShot(0, this, [this, generation] {
        if (!m_stateApplyInProgress || property(kStateApplyGenerationProperty).toULongLong() != generation) return;
        if (m_stateApplyCancelRequested) cancelStateApply(m_stateApplyCancelReason);
        else continueStateApply();
    });
}

void RegistryOptimizationPage::completePendingAction(const bool actionOk, const QString& errorText)
{
    if (!m_stateApplyInProgress || m_stateApplyActiveAction.isEmpty()) return;

    if (!actionOk)
    {
        const QPointer<RegistryOptimizationPage> alive(this);
        (void)ks::ui::promptForPrivilegeFailure(
            this,
            QStringLiteral("执行系统优化"),
            errorText);
        if (!alive) return;
    }

    bool actionRestartExplorer = false;
    ks::ui::FieldDocument actionDetailLines;
    const QString summaryText = jsonString(m_stateApplyActiveAction, QStringLiteral("summary"),
        QString::fromUtf8(QJsonDocument(m_stateApplyActiveAction).toJson(QJsonDocument::Compact)));
    const QString restartText = jsonString(m_stateApplyActiveAction, QStringLiteral("activate_restart"));
    actionRestartExplorer = restartText.contains(QStringLiteral("Explorer"), Qt::CaseInsensitive);
    const bool skippedError = !actionOk && shouldSkipActionError(m_stateApplyActiveAction);
    if (skippedError)
    {
        actionDetailLines.field(QStringLiteral("跳过"), summaryText).field(QStringLiteral("原因"), errorText, true);
    }
    else
    {
        actionDetailLines.field(actionOk ? QStringLiteral("成功") : QStringLiteral("失败"), summaryText);
        if (!actionOk) actionDetailLines.field(QStringLiteral("原因"), errorText, true);
    }

    m_stateApplyRestartExplorer = m_stateApplyRestartExplorer || actionRestartExplorer;
    m_stateApplyAllOk = m_stateApplyAllOk && (actionOk || skippedError);
    m_stateApplyDetailLines.nodes += actionDetailLines.nodes;
    m_stateApplyActiveAction = QJsonObject();
    ++m_stateApplyNextActionIndex;
    m_detailText->setDocument(m_stateApplyDetailLines);
    QTimer::singleShot(0, this, [this]() {
        if (m_stateApplyCancelRequested) cancelStateApply(m_stateApplyCancelReason);
        else continueStateApply();
    });
}

void RegistryOptimizationPage::finishStateApply()
{
    if (!m_stateApplyInProgress) return;

    if (m_stateApplyRestartExplorer)
    {
        m_stateApplyDetailLines.note(QStringLiteral("提示: 部分动作标记需要重启 Explorer 或重新登录后完全生效。"));
    }
    m_detailText->setDocument(m_stateApplyDetailLines);
    refreshVisibleRowState(m_stateApplyTableRow);
    const bool allOk = m_stateApplyAllOk;
    const QString itemName = m_stateApplyItemName;
    const QString targetLabel = m_stateApplyTargetLabel;
    m_stateApplyInProgress = false;
    m_stateApplyTableRow = -1;
    m_stateApplyActions.clear();
    m_stateApplyActiveAction = QJsonObject();
    m_stateApplyNextActionIndex = 0;
    setApplyControlsEnabled(true);
    updateStatusText(allOk
        ? QStringLiteral("系统优化：已应用 %1 -> %2。").arg(itemName, targetLabel)
        : QStringLiteral("系统优化：应用失败 %1 -> %2；请查看详情。").arg(itemName, targetLabel));
}

void RegistryOptimizationPage::cancelStateApply(const QString& reasonText)
{
    if (!m_stateApplyInProgress) return;

    if (m_externalSession) { m_externalSession->stop(); m_externalSession.reset(); }
    if (m_ordinaryOperation->isBusy())
    {
        m_stateApplyCancelRequested = true;
        m_stateApplyCancelReason = reasonText;
        updateStatusText(QStringLiteral("系统优化：已请求取消，将在当前动作完成并取得回执后停止。"));
        return;
    }

    // 序列代次自增：仍在后台阻塞的广播动作完成后不再推进这一轮（或下一轮）序列。
    setProperty(kStateApplyGenerationProperty, property(kStateApplyGenerationProperty).toULongLong() + 1);

    if (!reasonText.isEmpty())
    {
        m_stateApplyDetailLines.field(QStringLiteral("已取消"), reasonText, true);
        m_detailText->setDocument(m_stateApplyDetailLines);
        updateStatusText(QStringLiteral("系统优化：%1").arg(reasonText));
    }
    m_stateApplyInProgress = false;
    m_stateApplyTableRow = -1;
    m_stateApplyActions.clear();
    m_stateApplyActiveAction = QJsonObject();
    m_stateApplyNextActionIndex = 0;
    setApplyControlsEnabled(true);
    m_stateApplyCancelRequested = false;
    m_stateApplyCancelReason.clear();
    const bool reload = std::exchange(m_deferredProfileReload, false);
    const bool rebuild = std::exchange(m_deferredItemRebuild, false);
    if (reload) QTimer::singleShot(0, this, [this] { loadOptimizationProfile(); });
    else if (rebuild) QTimer::singleShot(0, this, [this] { rebuildItemTable(); });
}

void RegistryOptimizationPage::setApplyControlsEnabled(const bool enabled)
{
    if (m_itemTable != nullptr)
    {
        const QList<QPushButton*> applyButtons = m_itemTable->findChildren<QPushButton*>(QStringLiteral("registryOptimizationApplyButton"));
        for (QPushButton* button : applyButtons)
        {
            button->setEnabled(enabled);
        }
    }
    if (m_cancelApplyButton != nullptr) m_cancelApplyButton->setEnabled(!enabled);
    if (m_refreshStateButton != nullptr) m_refreshStateButton->setEnabled(enabled && !m_stateRefreshInProgress);
}

bool RegistryOptimizationPage::actionRequiresExternalProcess(const QJsonObject& actionObject)
{
    const QString familyText = jsonString(actionObject, QStringLiteral("action_family"),
        jsonString(actionObject, QStringLiteral("action_tag"))).trimmed();
    if (familyText.compare(QStringLiteral("FileCreateByZIP"), Qt::CaseInsensitive) == 0)
    {
        return true;
    }
    if (familyText.compare(QStringLiteral("ExplorerNotify"), Qt::CaseInsensitive) != 0)
    {
        return false;
    }
    return jsonString(actionObject, QStringLiteral("Type")).compare(QStringLiteral("Cmd"), Qt::CaseInsensitive) == 0;
}

bool RegistryOptimizationPage::startExternalAction(const QJsonObject& actionObject, QString* errorTextOut)
{
    if (errorTextOut) errorTextOut->clear();
    auto session = ks::registry::external::Session::create();
    if (!session) { if (errorTextOut) *errorTextOut = QString::number(ERROR_OPERATION_ABORTED); return false; }
    auto work = session->reserve();
    if (!work) { if (errorTextOut) *errorTextOut = QString::number(ERROR_OPERATION_ABORTED); return false; }
    struct Request { QJsonObject action; QString appDir; QString currentDir; std::shared_ptr<ks::registry::external::Session::Work> work; };
    const Request request{actionObject,QCoreApplication::applicationDirPath(),QDir::currentPath(),std::move(work)};
    const quint64 generation = property(kStateApplyGenerationProperty).toULongLong();
    m_externalSession = std::move(session);
    m_ordinaryReceipt = false;
    const auto ticket = m_ordinaryOperation->submitTransaction<Request,ExternalActionResult>(request,
        [](const Request& value) { return executeExternalAction(value.action,value.appDir,value.currentDir,value.work); },
        [this,generation](const ExternalActionResult& result,std::uint64_t) {
            m_ordinaryReceipt = true;
            QTimer::singleShot(0,this,[this,generation,result] {
                if (m_externalSession) { m_externalSession->stop(); m_externalSession.reset(); }
                if (!m_stateApplyInProgress || property(kStateApplyGenerationProperty).toULongLong()!=generation) return;
                if (result.canceled) { m_stateApplyCancelRequested=true;m_stateApplyCancelReason=result.error; }
                completePendingAction(result.ok,result.error);
            });
            return true;
        }, [this](std::exception_ptr) {
            m_ordinaryReceipt = true;
            QTimer::singleShot(0,this,[this] { cancelStateApply(QStringLiteral("注册表操作未取得完整回执；请刷新后核对。")); });
        });
    if (!ticket)
    {
        m_ordinaryReceipt = true;
        if (m_externalSession) {m_externalSession->stop();m_externalSession.reset();}
        if (errorTextOut) *errorTextOut=QString::number(ERROR_OPERATION_ABORTED);
    }
    return ticket != 0;
}

RegistryOptimizationPage::ExternalActionResult RegistryOptimizationPage::executeExternalAction(
    const QJsonObject& actionObject,const QString& appDir,const QString& currentDir,
    const std::shared_ptr<ks::registry::external::Session::Work>& work)
{
    // Declared first: every local process/file object is destroyed before publishing physical completion.
    struct Completion { std::shared_ptr<ks::registry::external::Session::Work> work; ~Completion(){work->finish();} } completion{work};
    auto& runtime=*work->runtime();
    ExternalActionResult result;
    const auto canceled=[&]() {
        result.canceled=true;
        result.error=QStringLiteral("外部动作已取消；进程已结束，已提交的更改不会撤回。");
        return result;
    };
    if(runtime.stopped.load())return canceled();
    const QString familyText=jsonString(actionObject,QStringLiteral("action_family"),jsonString(actionObject,QStringLiteral("action_tag"))).trimmed();
    QString programPath,entryPath,destinationPath;
    QStringList arguments;
    if(familyText.compare(QStringLiteral("ExplorerNotify"),Qt::CaseInsensitive)==0)
    {
        const auto commandText=jsonString(actionObject,QStringLiteral("Cmd"));
        if(commandText.isEmpty()){result.error=QStringLiteral("ExplorerNotify Cmd 缺少命令。");return result;}
        programPath=QStringLiteral("cmd.exe");arguments={QStringLiteral("/c"),commandText};
    }
    else if(familyText.compare(QStringLiteral("FileCreateByZIP"),Qt::CaseInsensitive)==0)
    {
        destinationPath=expandEnvironmentPath(jsonString(actionObject,QStringLiteral("Path")));
        const auto zipReference=jsonString(actionObject,QStringLiteral("ZIPFile"));
        if(destinationPath.isEmpty() || zipReference.isEmpty()){result.error=QStringLiteral("FileCreateByZIP 缺少 Path 或 ZIPFile。");return result;}
        QString zipRelativePath;
        if(!splitZipFileReference(zipReference,&zipRelativePath,&entryPath)){result.error=QStringLiteral("ZIPFile 格式无效：%1").arg(zipReference);return result;}
        QString zipPath;
        for(const auto& candidate:registryOptimizationAssetCandidates(zipRelativePath,appDir,currentDir))
            if(QFileInfo::exists(candidate)){zipPath=QDir::cleanPath(candidate);break;}
        if(zipPath.isEmpty()){result.error=QStringLiteral("未找到 ZIP 资产：profiles/registry_optimization_assets/%1").arg(zipRelativePath);return result;}
        QTemporaryDir temporary;
        if(!temporary.isValid()){result.error=QStringLiteral("无法创建临时目录用于解压 ZIP。");return result;}
        runtime.temporaryRoot=temporary.path();
        temporary.setAutoRemove(false); // Retained resource owner removes it only after actual process termination.
        programPath=QStringLiteral("tar.exe");
        arguments={QStringLiteral("-xf"),QDir::toNativeSeparators(zipPath),QStringLiteral("-C"),QDir::toNativeSeparators(runtime.temporaryRoot),QDir::fromNativeSeparators(entryPath)};
    }
    else {result.error=QStringLiteral("不支持的外部动作族：%1").arg(familyText);return result;}
    const auto process=ks::registry::external::runProcess(runtime,programPath,arguments);
    if(process.canceled)return canceled();
    if(!process.ok){result.error=process.error.isEmpty()?QStringLiteral("命令退出码：%1").arg(process.exitCode):process.error;return result;}
    if(!destinationPath.isEmpty())
    {
        if(runtime.stopped.load())return canceled();
        const auto extractedPath=QDir(runtime.temporaryRoot).filePath(QDir::fromNativeSeparators(entryPath));
        if(!QFileInfo::exists(extractedPath)){result.error=QStringLiteral("ZIP 中未找到条目：%1").arg(QDir::fromNativeSeparators(entryPath));return result;}
        const QFileInfo destinationInfo(destinationPath);auto directory=destinationInfo.dir();
        if(!directory.exists()&&!directory.mkpath(QStringLiteral("."))){result.error=QStringLiteral("无法创建目标目录：%1").arg(directory.absolutePath());return result;}
        QFile input(extractedPath);QSaveFile output(destinationPath);output.setDirectWriteFallback(false);
        const auto copyFailed=[&]() {output.cancelWriting();result.error=QStringLiteral("无法复制 ZIP 条目到目标：%1").arg(destinationPath);return result;};
        if(!input.open(QIODevice::ReadOnly)||!output.open(QIODevice::WriteOnly))return copyFailed();
        while(!input.atEnd())
        {
            if(runtime.stopped.load()){output.cancelWriting();return canceled();}
            const auto block=input.read(1024*1024);
            if(block.isEmpty()&&!input.atEnd())return copyFailed();
            qint64 position=0;
            while(position<block.size())
            {
                const auto written=output.write(block.constData()+position,block.size()-position);
                if(written<=0 || written>block.size()-position)return copyFailed();
                position+=written;
            }
        }
        if(input.error()!=QFileDevice::NoError)return copyFailed();
        if(runtime.stopped.load()){output.cancelWriting();return canceled();}
        if(!output.commit())return copyFailed();
        result.published=true;
    }
    result.ok=true;
    result.canceled=runtime.stopped.load(); // Stop later actions without relabeling an already-published file as rolled back.
    if(result.canceled)result.error=QStringLiteral("外部动作已取消；进程已结束，已提交的更改不会撤回。");
    return result;
}

bool RegistryOptimizationPage::executeAction(
    const QJsonObject& actionObject,
    ks::ui::FieldDocument* detailLinesOut,
    bool* restartExplorerOut, const RegistryValueState* original)
{
    if (restartExplorerOut != nullptr) *restartExplorerOut = false;
    const QString familyText = jsonString(actionObject, QStringLiteral("action_family"),
        jsonString(actionObject, QStringLiteral("action_tag"))).trimmed();
    QString errorText;
    bool ok = false;

    if (familyText.compare(QStringLiteral("RegWrite"), Qt::CaseInsensitive) == 0 ||
        familyText.compare(QStringLiteral("RegExist"), Qt::CaseInsensitive) == 0)
    {
        ok = executeRegistryWriteAction(actionObject, &errorText, original);
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
        errorText = QStringLiteral("FileCreateByZIP 必须通过异步外部动作执行。");
        ok = false;
    }
    else
    {
        errorText = QStringLiteral("暂不支持动作族：%1").arg(familyText);
        ok = false;
    }

    return describeActionResult(actionObject, ok, errorText, detailLinesOut, restartExplorerOut);
}

bool RegistryOptimizationPage::describeActionResult(const QJsonObject& actionObject, const bool ok, const QString& errorText,
    ks::ui::FieldDocument* detailLinesOut, bool* restartExplorerOut)
{
    const QString summaryText = jsonString(actionObject, QStringLiteral("summary"),
        QString::fromUtf8(QJsonDocument(actionObject).toJson(QJsonDocument::Compact)));
    const QString restartText = jsonString(actionObject, QStringLiteral("activate_restart"));
    if (restartExplorerOut != nullptr && restartText.contains(QStringLiteral("Explorer"), Qt::CaseInsensitive))
    {
        *restartExplorerOut = true;
    }

    if (!ok && shouldSkipActionError(actionObject))
    {
        if (detailLinesOut != nullptr) detailLinesOut->field(QStringLiteral("跳过"), summaryText).field(QStringLiteral("原因"), errorText, true);
        return true;
    }
    if (detailLinesOut != nullptr)
    {
        detailLinesOut->field(ok ? QStringLiteral("成功") : QStringLiteral("失败"), summaryText);
        if (!ok) detailLinesOut->field(QStringLiteral("原因"), errorText, true);
    }
    return ok;
}

bool RegistryOptimizationPage::executeRegistryWriteAction(const QJsonObject& actionObject, QString* errorTextOut, const RegistryValueState* capturedOriginal)
{
    if (errorTextOut != nullptr) errorTextOut->clear();
    const QString keyPath = jsonString(actionObject, QStringLiteral("Key"));
    const QString valueName = jsonString(actionObject, QStringLiteral("Value"));
    const QString typeText = jsonString(actionObject, QStringLiteral("Type"), QStringLiteral("REG_DWORD"));
    QString dataText = jsonString(actionObject, QStringLiteral("Data"));
    const QString operatorText = jsonString(actionObject, QStringLiteral("Operator"));
    const REGSAM viewFlags = actionRegistryViewFlags(actionObject);
    const RegistryAccessContext context{viewFlags == KEY_WOW64_32KEY ? 32 : 0, false}; // 固定配置来源。
    RegistryValueState original; // 模态/运算前的完整原始值，用于提交前冲突检查。
    if (capturedOriginal) original = *capturedOriginal;
    else if (!RegistryWorkbenchAccess::read(keyPath, valueName, context, &original, errorTextOut)) return false;
    if (!original.complete) return false;

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

    QByteArray newData;
    QString conversionError;
    if (!registryDataFromJsonText(valueType, dataText, &newData, &conversionError))
    {
        if (errorTextOut != nullptr) *errorTextOut = conversionError;
        return false;
    }

    if (!operatorText.isEmpty())
    {
        if (original.exists && original.type != valueType)
        {
            if (errorTextOut) *errorTextOut = QStringLiteral("Registry optimization operator requires the original value type to match.");
            return false;
        }
        QByteArray currentData = original.exists ? original.data : QByteArray(); // 缺失值才采用配置的零初值。
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

    return ks::registry::optimization::write(keyPath, valueName, valueType, newData, original, context, errorTextOut);
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
    const RegistryAccessContext context{actionRegistryViewFlags(actionObject) == KEY_WOW64_32KEY ? 32 : 0, false};
    return ks::registry::optimization::remove(keyPath, valueName, hasValueName, context, errorTextOut);
}

bool RegistryOptimizationPage::executeRegistryMoveAction(const QJsonObject& actionObject, QString* errorTextOut)
{
    const QString keyPath = jsonString(actionObject, QStringLiteral("Key"));
    const QString newKeyPath = jsonString(actionObject, QStringLiteral("NewKey"));
    const QString valueName = jsonString(actionObject, QStringLiteral("Value"));
    const QString newValueName = jsonString(actionObject, QStringLiteral("NewValue"));
    const REGSAM viewFlags = actionRegistryViewFlags(actionObject);

    const RegistryAccessContext context{viewFlags == KEY_WOW64_32KEY ? 32 : 0, false}; // 配置的准确来源。
    const bool isValue = actionObject.contains(QStringLiteral("Value")) || actionObject.contains(QStringLiteral("NewValue"));
    const QString target = newKeyPath.isEmpty() && isValue ? keyPath : newKeyPath;
    return ks::registry::optimization::move(keyPath, valueName, target, newValueName, isValue, context, errorTextOut);

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
        if (errorTextOut != nullptr) *errorTextOut = QStringLiteral("ExplorerNotify Cmd 必须通过异步外部动作执行。");
        return false;
    }

    // AssocChanged / Custom 的正常路径已由 continueStateApply 改走后台线程；
    // 这里只保留同步兜底（不重复初始化 COM，UI 线程套间由 Qt 负责）。
    const ExplorerNotifyOutcome outcome = runExplorerNotifyBroadcast(actionObject, true);
    if (!outcome.succeeded && errorTextOut != nullptr)
    {
        *errorTextOut = outcome.errorTextValue;
    }
    return outcome.succeeded;
}

bool RegistryOptimizationPage::evaluateConditionText(const QString& conditionText)
{
    const QString trimmedCondition = conditionText.trimmed();
    if (trimmedCondition.isEmpty()) return false;
    ConditionParser parser(trimmedCondition);
    return parser.evaluate();
}
