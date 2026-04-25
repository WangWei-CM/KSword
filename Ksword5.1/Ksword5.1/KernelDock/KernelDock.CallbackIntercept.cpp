#include "KernelDock.h"

#include "KernelDock.CallbackIntercept.h"
#include "KernelDock.CallbackPromptManager.h"
#include "../theme.h"

#include "../../../shared/KswordArkLogProtocol.h"

#include <QApplication>
#include <QAbstractItemView>
#include <QCheckBox>
#include <QComboBox>
#include <QDateTime>
#include <QFile>
#include <QFileDialog>
#include <QGridLayout>
#include <QHeaderView>
#include <QHash>
#include <QIcon>
#include <QMenu>
#include <QInputDialog>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QClipboard>
#include <QPushButton>
#include <QSize>
#include <QSplitter>
#include <QStyledItemDelegate>
#include <QTabWidget>
#include <QTableWidget>
#include <QTableWidgetItem>
#include <QTimeZone>
#include <QUrl>
#include <QVBoxLayout>

#include <Windows.h>
#include <sddl.h>

#include <algorithm>
#include <limits>

namespace
{
    enum class GroupColumn : int
    {
        Id = 0,
        Name,
        Enabled,
        Priority,
        Comment,
        Count
    };

    enum class RuleColumn : int
    {
        Enabled,
        RuleId,
        GroupId,
        RuleName,
        OperationMask,
        MatchMode,
        Action,
        TimeoutMs,
        TimeoutDefaultDecision,
        Priority,
        Count
    };

    class OpaqueTableEditorDelegate final : public QStyledItemDelegate
    {
    public:
        explicit OpaqueTableEditorDelegate(QObject* parent = nullptr)
            : QStyledItemDelegate(parent)
        {
        }

        QWidget* createEditor(
            QWidget* parent,
            const QStyleOptionViewItem& option,
            const QModelIndex& index) const override
        {
            QWidget* editor = QStyledItemDelegate::createEditor(parent, option, index);
            auto* lineEdit = qobject_cast<QLineEdit*>(editor);
            if (lineEdit != nullptr)
            {
                lineEdit->setAutoFillBackground(true);
                lineEdit->setFrame(true);
                lineEdit->setStyleSheet(
                    QStringLiteral(
                        "QLineEdit{"
                        "  background:%1;"
                        "  color:%2;"
                        "  border:1px solid %3;"
                        "  border-radius:2px;"
                        "  padding:0px 4px;"
                        "}")
                    .arg(KswordTheme::SurfaceHex())
                    .arg(KswordTheme::TextPrimaryHex())
                    .arg(KswordTheme::BorderHex()));
            }
            return editor;
        }
    };

    quint32 defaultOperationMaskByType(const quint32 callbackType)
    {
        switch (callbackType)
        {
        case KSWORD_ARK_CALLBACK_TYPE_REGISTRY: return KSWORD_ARK_REG_OP_ALL;
        case KSWORD_ARK_CALLBACK_TYPE_PROCESS_CREATE: return KSWORD_ARK_PROCESS_OP_CREATE;
        case KSWORD_ARK_CALLBACK_TYPE_THREAD_CREATE: return KSWORD_ARK_THREAD_OP_CREATE | KSWORD_ARK_THREAD_OP_EXIT;
        case KSWORD_ARK_CALLBACK_TYPE_IMAGE_LOAD: return KSWORD_ARK_IMAGE_OP_LOAD;
        case KSWORD_ARK_CALLBACK_TYPE_OBJECT:
            return KSWORD_ARK_OBJECT_OP_HANDLE_CREATE |
                KSWORD_ARK_OBJECT_OP_HANDLE_DUPLICATE |
                KSWORD_ARK_OBJECT_OP_TYPE_PROCESS |
                KSWORD_ARK_OBJECT_OP_TYPE_THREAD;
        default:
            return 0U;
        }
    }

    QList<QPair<QString, quint32>> allowedActionListByType(const quint32 callbackType)
    {
        switch (callbackType)
        {
        case KSWORD_ARK_CALLBACK_TYPE_REGISTRY:
            return {
                { QStringLiteral("允许"), KSWORD_ARK_RULE_ACTION_ALLOW },
                { QStringLiteral("拒绝"), KSWORD_ARK_RULE_ACTION_DENY },
                { QStringLiteral("询问用户"), KSWORD_ARK_RULE_ACTION_ASK_USER },
                { QStringLiteral("记录日志"), KSWORD_ARK_RULE_ACTION_LOG_ONLY }
            };
        case KSWORD_ARK_CALLBACK_TYPE_PROCESS_CREATE:
            return {
                { QStringLiteral("允许"), KSWORD_ARK_RULE_ACTION_ALLOW },
                { QStringLiteral("拒绝"), KSWORD_ARK_RULE_ACTION_DENY },
                { QStringLiteral("记录日志"), KSWORD_ARK_RULE_ACTION_LOG_ONLY }
            };
        case KSWORD_ARK_CALLBACK_TYPE_THREAD_CREATE:
        case KSWORD_ARK_CALLBACK_TYPE_IMAGE_LOAD:
            return {
                { QStringLiteral("记录日志"), KSWORD_ARK_RULE_ACTION_LOG_ONLY }
            };
        case KSWORD_ARK_CALLBACK_TYPE_OBJECT:
            return {
                { QStringLiteral("允许"), KSWORD_ARK_RULE_ACTION_ALLOW },
                { QStringLiteral("降权拦截"), KSWORD_ARK_RULE_ACTION_STRIP_ACCESS },
                { QStringLiteral("记录日志"), KSWORD_ARK_RULE_ACTION_LOG_ONLY }
            };
        default:
            return {};
        }
    }

    QList<QPair<QString, quint32>> allowedMatchModeListByType(const quint32 callbackType)
    {
        if (callbackType == KSWORD_ARK_CALLBACK_TYPE_REGISTRY)
        {
            return {
                { QStringLiteral("精确匹配"), KSWORD_ARK_MATCH_MODE_EXACT },
                { QStringLiteral("前缀匹配"), KSWORD_ARK_MATCH_MODE_PREFIX },
                { QStringLiteral("通配符匹配"), KSWORD_ARK_MATCH_MODE_WILDCARD },
                { QStringLiteral("正则匹配"), KSWORD_ARK_MATCH_MODE_REGEX }
            };
        }

        return {
            { QStringLiteral("精确匹配"), KSWORD_ARK_MATCH_MODE_EXACT },
            { QStringLiteral("前缀匹配"), KSWORD_ARK_MATCH_MODE_PREFIX },
            { QStringLiteral("通配符匹配"), KSWORD_ARK_MATCH_MODE_WILDCARD }
        };
    }

    QList<QPair<QString, quint32>> decisionOptionList()
    {
        return {
            { QStringLiteral("允许"), KSWORD_ARK_DECISION_ALLOW },
            { QStringLiteral("拒绝"), KSWORD_ARK_DECISION_DENY }
        };
    }

    bool containsOptionValue(
        const QList<QPair<QString, quint32>>& optionList,
        const quint32 valueToFind)
    {
        for (const QPair<QString, quint32>& optionPair : optionList)
        {
            if (optionPair.second == valueToFind)
            {
                return true;
            }
        }
        return false;
    }

    bool parseUnsignedText(const QString& rawText, quint32* valueOut)
    {
        if (valueOut == nullptr)
        {
            return false;
        }

        QString textValue = rawText.trimmed();
        int base = 10;
        if (textValue.startsWith(QStringLiteral("0x"), Qt::CaseInsensitive))
        {
            textValue = textValue.mid(2);
            base = 16;
        }

        bool convertOk = false;
        const qulonglong parsedValue = textValue.toULongLong(&convertOk, base);
        if (!convertOk || parsedValue > std::numeric_limits<quint32>::max())
        {
            return false;
        }

        *valueOut = static_cast<quint32>(parsedValue);
        return true;
    }

    QString operationMaskToText(const quint32 operationMask)
    {
        return QStringLiteral("0x%1")
            .arg(static_cast<qulonglong>(operationMask), 8, 16, QChar('0'))
            .toUpper();
    }

    QList<QPair<QString, quint32>> operationPresetListByType(const quint32 callbackType)
    {
        switch (callbackType)
        {
        case KSWORD_ARK_CALLBACK_TYPE_REGISTRY:
            return {
                { QStringLiteral("全部"), KSWORD_ARK_REG_OP_ALL },
                { QStringLiteral("常见增"), KSWORD_ARK_REG_OP_CREATE_KEY | KSWORD_ARK_REG_OP_SET_VALUE },
                { QStringLiteral("常见删"), KSWORD_ARK_REG_OP_DELETE_KEY | KSWORD_ARK_REG_OP_DELETE_VALUE },
                { QStringLiteral("常见改"), KSWORD_ARK_REG_OP_SET_VALUE | KSWORD_ARK_REG_OP_RENAME_KEY | KSWORD_ARK_REG_OP_SET_INFO },
                { QStringLiteral("常见查"), KSWORD_ARK_REG_OP_OPEN_KEY | KSWORD_ARK_REG_OP_QUERY_VALUE },
                { QStringLiteral("创建键"), KSWORD_ARK_REG_OP_CREATE_KEY },
                { QStringLiteral("打开键"), KSWORD_ARK_REG_OP_OPEN_KEY },
                { QStringLiteral("删除键"), KSWORD_ARK_REG_OP_DELETE_KEY },
                { QStringLiteral("写入值"), KSWORD_ARK_REG_OP_SET_VALUE },
                { QStringLiteral("删除值"), KSWORD_ARK_REG_OP_DELETE_VALUE },
                { QStringLiteral("重命名键"), KSWORD_ARK_REG_OP_RENAME_KEY },
                { QStringLiteral("设置键信息"), KSWORD_ARK_REG_OP_SET_INFO },
                { QStringLiteral("查询值"), KSWORD_ARK_REG_OP_QUERY_VALUE }
            };

        case KSWORD_ARK_CALLBACK_TYPE_PROCESS_CREATE:
            return {
                { QStringLiteral("进程创建"), KSWORD_ARK_PROCESS_OP_CREATE }
            };

        case KSWORD_ARK_CALLBACK_TYPE_THREAD_CREATE:
            return {
                { QStringLiteral("线程创建"), KSWORD_ARK_THREAD_OP_CREATE },
                { QStringLiteral("线程退出"), KSWORD_ARK_THREAD_OP_EXIT },
                { QStringLiteral("创建+退出"), KSWORD_ARK_THREAD_OP_CREATE | KSWORD_ARK_THREAD_OP_EXIT }
            };

        case KSWORD_ARK_CALLBACK_TYPE_IMAGE_LOAD:
            return {
                { QStringLiteral("镜像加载"), KSWORD_ARK_IMAGE_OP_LOAD }
            };

        case KSWORD_ARK_CALLBACK_TYPE_OBJECT:
            return {
                { QStringLiteral("全部"), KSWORD_ARK_OBJECT_OP_HANDLE_CREATE | KSWORD_ARK_OBJECT_OP_HANDLE_DUPLICATE |
                    KSWORD_ARK_OBJECT_OP_TYPE_PROCESS | KSWORD_ARK_OBJECT_OP_TYPE_THREAD },
                { QStringLiteral("句柄创建"), KSWORD_ARK_OBJECT_OP_HANDLE_CREATE },
                { QStringLiteral("句柄复制"), KSWORD_ARK_OBJECT_OP_HANDLE_DUPLICATE },
                { QStringLiteral("进程对象"), KSWORD_ARK_OBJECT_OP_TYPE_PROCESS },
                { QStringLiteral("线程对象"), KSWORD_ARK_OBJECT_OP_TYPE_THREAD }
            };

        default:
            return {};
        }
    }

    QString initiatorPlaceholderByType(const quint32 callbackType)
    {
        switch (callbackType)
        {
        case KSWORD_ARK_CALLBACK_TYPE_REGISTRY:
            return QStringLiteral("例如：* 或 C:\\Windows\\System32\\reg.exe（支持自动转换）");
        case KSWORD_ARK_CALLBACK_TYPE_PROCESS_CREATE:
            return QStringLiteral("例如：* 或 C:\\Windows\\System32\\WindowsPowerShell\\v1.0\\powershell.exe（支持自动转换）");
        case KSWORD_ARK_CALLBACK_TYPE_THREAD_CREATE:
            return QStringLiteral("例如：* 或 C:\\Windows\\System32\\notepad.exe（支持自动转换）");
        case KSWORD_ARK_CALLBACK_TYPE_IMAGE_LOAD:
            return QStringLiteral("例如：* 或 C:\\Windows\\System32\\notepad.exe（支持自动转换）");
        case KSWORD_ARK_CALLBACK_TYPE_OBJECT:
            return QStringLiteral("例如：* 或 C:\\Windows\\System32\\WindowsPowerShell\\v1.0\\powershell.exe（支持自动转换）");
        default:
            return QStringLiteral("例如：*");
        }
    }

    QString targetPlaceholderByType(const quint32 callbackType)
    {
        switch (callbackType)
        {
        case KSWORD_ARK_CALLBACK_TYPE_REGISTRY:
            return QStringLiteral("例如：HKCU\\Software\\KswordDemo 或 \\REGISTRY\\USER\\*\\Software\\KswordDemo");
        case KSWORD_ARK_CALLBACK_TYPE_PROCESS_CREATE:
            return QStringLiteral("例如：C:\\Windows\\System32\\notepad.exe（支持自动转换）");
        case KSWORD_ARK_CALLBACK_TYPE_THREAD_CREATE:
            return QStringLiteral("例如：C:\\Windows\\System32\\notepad.exe（目标进程镜像，支持自动转换）");
        case KSWORD_ARK_CALLBACK_TYPE_IMAGE_LOAD:
            return QStringLiteral("例如：C:\\Windows\\System32\\kernel32.dll（支持自动转换）");
        case KSWORD_ARK_CALLBACK_TYPE_OBJECT:
            return QStringLiteral("例如：C:\\Windows\\System32\\notepad.exe（被打开句柄的目标进程，支持自动转换）");
        default:
            return QStringLiteral("例如：*");
        }
    }

    quint32 currentOperationMaskFromCombo(const QComboBox* comboBox, bool* okOut)
    {
        if (okOut != nullptr)
        {
            *okOut = false;
        }
        if (comboBox == nullptr)
        {
            return 0U;
        }

        quint32 operationMask = 0U;
        if (parseUnsignedText(comboBox->currentText(), &operationMask))
        {
            if (okOut != nullptr)
            {
                *okOut = true;
            }
            return operationMask;
        }

        const QString fullText = comboBox->currentText().trimmed();
        const int leftBracketIndex = fullText.lastIndexOf('(');
        const int rightBracketIndex = fullText.lastIndexOf(')');
        if (leftBracketIndex >= 0 && rightBracketIndex > leftBracketIndex)
        {
            const QString innerText = fullText.mid(leftBracketIndex + 1, rightBracketIndex - leftBracketIndex - 1).trimmed();
            if (parseUnsignedText(innerText, &operationMask))
            {
                if (okOut != nullptr)
                {
                    *okOut = true;
                }
                return operationMask;
            }
        }

        bool dataOk = false;
        const qulonglong dataValue = comboBox->currentData().toULongLong(&dataOk);
        if (dataOk && dataValue <= std::numeric_limits<quint32>::max())
        {
            if (okOut != nullptr)
            {
                *okOut = true;
            }
            return static_cast<quint32>(dataValue);
        }
        return 0U;
    }

    QString normalizeMatchAllPattern(const QString& rawPatternText)
    {
        const QString trimmedText = rawPatternText.trimmed();
        if (trimmedText == QStringLiteral("*") || trimmedText == QStringLiteral("**"))
        {
            return QString();
        }
        return rawPatternText;
    }

    QString normalizeUserModeFilePathPatternForKernel(const QString& rawPatternText)
    {
        QString pathPattern = rawPatternText.trimmed();
        if (pathPattern.isEmpty())
        {
            return pathPattern;
        }

        pathPattern.replace('/', '\\');

        auto startsWithInsensitive = [](const QString& textValue, const QString& prefixText) -> bool
            {
                return textValue.startsWith(prefixText, Qt::CaseInsensitive);
            };

        // 已是内核常见路径格式则直接透传。
        if (startsWithInsensitive(pathPattern, QStringLiteral("\\Device\\")) ||
            startsWithInsensitive(pathPattern, QStringLiteral("\\REGISTRY\\")) ||
            startsWithInsensitive(pathPattern, QStringLiteral("\\??\\")))
        {
            return pathPattern;
        }

        // 兼容 "\\??\\C:\..." 这种多一个反斜杠的写法。
        if (startsWithInsensitive(pathPattern, QStringLiteral("\\\\??\\")))
        {
            pathPattern.remove(0, 1);
            return pathPattern;
        }

        // 兼容 Win32 扩展前缀 "\\?\C:\..." / "\\?\UNC\server\share\..."
        if (startsWithInsensitive(pathPattern, QStringLiteral("\\\\?\\UNC\\")))
        {
            pathPattern = QStringLiteral("\\\\") + pathPattern.mid(8);
        }
        else if (startsWithInsensitive(pathPattern, QStringLiteral("\\\\?\\")))
        {
            pathPattern = pathPattern.mid(4);
        }

        // UNC 路径转换：\\server\share\foo -> \Device\Mup\server\share\foo
        if (pathPattern.startsWith(QStringLiteral("\\\\")))
        {
            QString uncRest = pathPattern.mid(2);
            while (uncRest.startsWith('\\'))
            {
                uncRest.remove(0, 1);
            }
            if (uncRest.isEmpty())
            {
                return QStringLiteral("\\Device\\Mup");
            }
            return QStringLiteral("\\Device\\Mup\\%1").arg(uncRest);
        }

        // 盘符路径转换：C:\foo -> \Device\HarddiskVolumeX\foo（优先），失败回退 \??\C:\foo。
        if (pathPattern.size() >= 2 &&
            pathPattern[0].isLetter() &&
            pathPattern[1] == QLatin1Char(':'))
        {
            const QString driveText = pathPattern.left(2).toUpper();
            wchar_t targetBuffer[1024] = {};
            const DWORD queryChars = ::QueryDosDeviceW(
                reinterpret_cast<LPCWSTR>(driveText.utf16()),
                targetBuffer,
                static_cast<DWORD>(sizeof(targetBuffer) / sizeof(targetBuffer[0])));

            QString restPath = pathPattern.mid(2);
            while (restPath.startsWith('\\'))
            {
                restPath.remove(0, 1);
            }

            if (queryChars > 0U && targetBuffer[0] != L'\0')
            {
                const QString ntDevicePrefix = QString::fromWCharArray(targetBuffer);
                if (!ntDevicePrefix.trimmed().isEmpty())
                {
                    return restPath.isEmpty()
                        ? ntDevicePrefix
                        : QStringLiteral("%1\\%2").arg(ntDevicePrefix, restPath);
                }
            }

            return QStringLiteral("\\??\\%1").arg(pathPattern);
        }

        return pathPattern;
    }

    QString normalizeRegistryTargetPatternForKernel(const QString& rawTargetPattern)
    {
        QString targetPattern = rawTargetPattern.trimmed();
        if (targetPattern.isEmpty())
        {
            return targetPattern;
        }

        targetPattern.replace('/', '\\');

        auto trimLeadingSlash = [](QString* textValue) {
            if (textValue == nullptr)
            {
                return;
            }
            while (textValue->startsWith('\\'))
            {
                textValue->remove(0, 1);
            }
        };

        auto restPathAfterRoot = [&](const QString& rootText) {
            QString restText = targetPattern.mid(rootText.size());
            trimLeadingSlash(&restText);
            return restText;
        };

        auto buildWithRoot = [](const QString& kernelRootText, const QString& restText) {
            if (restText.trimmed().isEmpty())
            {
                return kernelRootText;
            }
            return QStringLiteral("%1\\%2").arg(kernelRootText, restText);
        };

        auto currentUserSidText = []() -> QString {
            HANDLE tokenHandle = nullptr;
            if (::OpenProcessToken(::GetCurrentProcess(), TOKEN_QUERY, &tokenHandle) == FALSE)
            {
                return QString();
            }

            DWORD tokenBytes = 0;
            (void)::GetTokenInformation(tokenHandle, TokenUser, nullptr, 0, &tokenBytes);
            if (tokenBytes == 0U)
            {
                ::CloseHandle(tokenHandle);
                return QString();
            }

            QByteArray tokenBuffer(static_cast<int>(tokenBytes), 0);
            if (::GetTokenInformation(tokenHandle, TokenUser, tokenBuffer.data(), tokenBytes, &tokenBytes) == FALSE)
            {
                ::CloseHandle(tokenHandle);
                return QString();
            }
            ::CloseHandle(tokenHandle);

            const auto* tokenUser = reinterpret_cast<const TOKEN_USER*>(tokenBuffer.constData());
            if (tokenUser == nullptr || tokenUser->User.Sid == nullptr)
            {
                return QString();
            }

            LPWSTR sidWideText = nullptr;
            if (::ConvertSidToStringSidW(tokenUser->User.Sid, &sidWideText) == FALSE || sidWideText == nullptr)
            {
                return QString();
            }

            const QString sidText = QString::fromWCharArray(sidWideText);
            ::LocalFree(sidWideText);
            return sidText;
        };

        if (targetPattern.startsWith(QStringLiteral("\\REGISTRY\\"), Qt::CaseInsensitive) ||
            targetPattern.compare(QStringLiteral("\\REGISTRY"), Qt::CaseInsensitive) == 0)
        {
            return targetPattern;
        }

        if (targetPattern.startsWith(QStringLiteral("HKLM"), Qt::CaseInsensitive))
        {
            return buildWithRoot(QStringLiteral("\\REGISTRY\\MACHINE"), restPathAfterRoot(QStringLiteral("HKLM")));
        }
        if (targetPattern.startsWith(QStringLiteral("HKEY_LOCAL_MACHINE"), Qt::CaseInsensitive))
        {
            return buildWithRoot(QStringLiteral("\\REGISTRY\\MACHINE"), restPathAfterRoot(QStringLiteral("HKEY_LOCAL_MACHINE")));
        }
        if (targetPattern.startsWith(QStringLiteral("HKU"), Qt::CaseInsensitive))
        {
            return buildWithRoot(QStringLiteral("\\REGISTRY\\USER"), restPathAfterRoot(QStringLiteral("HKU")));
        }
        if (targetPattern.startsWith(QStringLiteral("HKEY_USERS"), Qt::CaseInsensitive))
        {
            return buildWithRoot(QStringLiteral("\\REGISTRY\\USER"), restPathAfterRoot(QStringLiteral("HKEY_USERS")));
        }
        if (targetPattern.startsWith(QStringLiteral("HKCR"), Qt::CaseInsensitive))
        {
            return buildWithRoot(QStringLiteral("\\REGISTRY\\MACHINE\\SOFTWARE\\Classes"), restPathAfterRoot(QStringLiteral("HKCR")));
        }
        if (targetPattern.startsWith(QStringLiteral("HKEY_CLASSES_ROOT"), Qt::CaseInsensitive))
        {
            return buildWithRoot(QStringLiteral("\\REGISTRY\\MACHINE\\SOFTWARE\\Classes"), restPathAfterRoot(QStringLiteral("HKEY_CLASSES_ROOT")));
        }
        if (targetPattern.startsWith(QStringLiteral("HKCC"), Qt::CaseInsensitive))
        {
            return buildWithRoot(
                QStringLiteral("\\REGISTRY\\MACHINE\\SYSTEM\\CurrentControlSet\\Hardware Profiles\\Current"),
                restPathAfterRoot(QStringLiteral("HKCC")));
        }
        if (targetPattern.startsWith(QStringLiteral("HKEY_CURRENT_CONFIG"), Qt::CaseInsensitive))
        {
            return buildWithRoot(
                QStringLiteral("\\REGISTRY\\MACHINE\\SYSTEM\\CurrentControlSet\\Hardware Profiles\\Current"),
                restPathAfterRoot(QStringLiteral("HKEY_CURRENT_CONFIG")));
        }
        if (targetPattern.startsWith(QStringLiteral("HKCU"), Qt::CaseInsensitive))
        {
            const QString sidText = currentUserSidText();
            const QString rootText = sidText.isEmpty()
                ? QStringLiteral("\\REGISTRY\\USER\\*")
                : QStringLiteral("\\REGISTRY\\USER\\%1").arg(sidText);
            return buildWithRoot(rootText, restPathAfterRoot(QStringLiteral("HKCU")));
        }
        if (targetPattern.startsWith(QStringLiteral("HKEY_CURRENT_USER"), Qt::CaseInsensitive))
        {
            const QString sidText = currentUserSidText();
            const QString rootText = sidText.isEmpty()
                ? QStringLiteral("\\REGISTRY\\USER\\*")
                : QStringLiteral("\\REGISTRY\\USER\\%1").arg(sidText);
            return buildWithRoot(rootText, restPathAfterRoot(QStringLiteral("HKEY_CURRENT_USER")));
        }

        return targetPattern;
    }

    QString utc100nsToDisplayText(const quint64 utc100ns)
    {
        if (utc100ns == 0ULL)
        {
            return QStringLiteral("-");
        }

        constexpr qint64 kFileTimeToUnixEpoch100ns = 116444736000000000LL;
        const qint64 value100ns = static_cast<qint64>(utc100ns);
        if (value100ns < kFileTimeToUnixEpoch100ns)
        {
            return QStringLiteral("-");
        }

        const qint64 unixMs = (value100ns - kFileTimeToUnixEpoch100ns) / 10000LL;
        const QDateTime utcDateTime = QDateTime::fromMSecsSinceEpoch(unixMs, QTimeZone::UTC);
        if (!utcDateTime.isValid())
        {
            return QStringLiteral("-");
        }
        return utcDateTime.toLocalTime().toString(QStringLiteral("yyyy-MM-dd HH:mm:ss.zzz"));
    }

    QTableWidgetItem* makeReadOnlyItem(const QString& textValue)
    {
        auto* item = new QTableWidgetItem(textValue);
        item->setFlags(item->flags() & ~Qt::ItemIsEditable);
        return item;
    }

    void applyRuleLineEditStyle(QLineEdit* lineEdit)
    {
        if (lineEdit == nullptr)
        {
            return;
        }
        lineEdit->setAutoFillBackground(true);
        lineEdit->setStyleSheet(
            QStringLiteral(
                "QLineEdit{"
                "  background:%1;"
                "  color:%2;"
                "  border:1px solid %3;"
                "  border-radius:2px;"
                "  padding:2px 6px;"
                "}"
                "QLineEdit:focus{"
                "  border:1px solid %4;"
                "}")
            .arg(KswordTheme::SurfaceHex())
            .arg(KswordTheme::TextPrimaryHex())
            .arg(KswordTheme::BorderHex())
            .arg(KswordTheme::PrimaryBlueHex));
    }

    QString encodeRuleClipboardText(const QString& rawText)
    {
        return QString::fromLatin1(QUrl::toPercentEncoding(rawText));
    }

    QString decodeRuleClipboardText(const QString& encodedText)
    {
        return QUrl::fromPercentEncoding(encodedText.toLatin1());
    }

    QString serializeRuleToClipboardText(const CallbackRuleModel& ruleModel)
    {
        QStringList lineList;
        lineList.push_back(QStringLiteral("KSWORD_CALLBACK_RULE_V1"));
        lineList.push_back(QStringLiteral("enabled=%1").arg(ruleModel.enabled ? 1 : 0));
        lineList.push_back(QStringLiteral("ruleId=%1").arg(ruleModel.ruleId));
        lineList.push_back(QStringLiteral("groupId=%1").arg(ruleModel.groupId));
        lineList.push_back(QStringLiteral("ruleName=%1").arg(encodeRuleClipboardText(ruleModel.ruleName)));
        lineList.push_back(QStringLiteral("callbackType=%1").arg(ruleModel.callbackType));
        lineList.push_back(QStringLiteral("operationMask=%1").arg(operationMaskToText(ruleModel.operationMask)));
        lineList.push_back(QStringLiteral("initiatorPattern=%1").arg(encodeRuleClipboardText(ruleModel.initiatorPattern)));
        lineList.push_back(QStringLiteral("targetPattern=%1").arg(encodeRuleClipboardText(ruleModel.targetPattern)));
        lineList.push_back(QStringLiteral("matchMode=%1").arg(ruleModel.matchMode));
        lineList.push_back(QStringLiteral("action=%1").arg(ruleModel.action));
        lineList.push_back(QStringLiteral("timeoutMs=%1").arg(ruleModel.timeoutMs));
        lineList.push_back(QStringLiteral("timeoutDefaultDecision=%1").arg(ruleModel.timeoutDefaultDecision));
        lineList.push_back(QStringLiteral("priority=%1").arg(ruleModel.priority));
        lineList.push_back(QStringLiteral("comment=%1").arg(encodeRuleClipboardText(ruleModel.comment)));
        return lineList.join(QLatin1Char('\n'));
    }

    bool deserializeRuleFromClipboardText(
        const QString& clipboardText,
        CallbackRuleModel* ruleOut,
        QString* errorTextOut)
    {
        if (ruleOut == nullptr)
        {
            if (errorTextOut != nullptr)
            {
                *errorTextOut = QStringLiteral("解析失败：ruleOut 为空。");
            }
            return false;
        }

        QString textValue = clipboardText;
        textValue.replace(QStringLiteral("\r\n"), QStringLiteral("\n"));
        textValue.replace(QStringLiteral("\r"), QStringLiteral("\n"));
        const QStringList rawLineList = textValue.split(QLatin1Char('\n'), Qt::SkipEmptyParts);
        if (rawLineList.isEmpty() || rawLineList.front().trimmed() != QStringLiteral("KSWORD_CALLBACK_RULE_V1"))
        {
            if (errorTextOut != nullptr)
            {
                *errorTextOut = QStringLiteral("解析失败：不是支持的规则文本格式。");
            }
            return false;
        }

        QHash<QString, QString> valueMap;
        for (int lineIndex = 1; lineIndex < rawLineList.size(); ++lineIndex)
        {
            const QString lineText = rawLineList[lineIndex].trimmed();
            const int separatorIndex = lineText.indexOf('=');
            if (separatorIndex <= 0)
            {
                continue;
            }
            const QString keyText = lineText.left(separatorIndex).trimmed();
            const QString valueTextPart = lineText.mid(separatorIndex + 1);
            valueMap.insert(keyText, valueTextPart);
        }

        CallbackRuleModel parsedRuleModel;
        parsedRuleModel.enabled = (valueMap.value(QStringLiteral("enabled")).trimmed() != QStringLiteral("0"));

        auto parseUIntField = [&](const QString& keyText, quint32* valueOut) -> bool {
            return parseUnsignedText(valueMap.value(keyText).trimmed(), valueOut);
        };

        if (!parseUIntField(QStringLiteral("ruleId"), &parsedRuleModel.ruleId) ||
            !parseUIntField(QStringLiteral("groupId"), &parsedRuleModel.groupId) ||
            !parseUIntField(QStringLiteral("callbackType"), &parsedRuleModel.callbackType) ||
            !parseUIntField(QStringLiteral("operationMask"), &parsedRuleModel.operationMask) ||
            !parseUIntField(QStringLiteral("matchMode"), &parsedRuleModel.matchMode) ||
            !parseUIntField(QStringLiteral("action"), &parsedRuleModel.action) ||
            !parseUIntField(QStringLiteral("timeoutMs"), &parsedRuleModel.timeoutMs) ||
            !parseUIntField(QStringLiteral("timeoutDefaultDecision"), &parsedRuleModel.timeoutDefaultDecision))
        {
            if (errorTextOut != nullptr)
            {
                *errorTextOut = QStringLiteral("解析失败：数值字段不合法。");
            }
            return false;
        }

        bool priorityOk = false;
        parsedRuleModel.priority = valueMap.value(QStringLiteral("priority")).trimmed().toInt(&priorityOk);
        if (!priorityOk)
        {
            if (errorTextOut != nullptr)
            {
                *errorTextOut = QStringLiteral("解析失败：priority 不合法。");
            }
            return false;
        }

        parsedRuleModel.ruleName = decodeRuleClipboardText(valueMap.value(QStringLiteral("ruleName")));
        parsedRuleModel.initiatorPattern = decodeRuleClipboardText(valueMap.value(QStringLiteral("initiatorPattern")));
        parsedRuleModel.targetPattern = decodeRuleClipboardText(valueMap.value(QStringLiteral("targetPattern")));
        parsedRuleModel.comment = decodeRuleClipboardText(valueMap.value(QStringLiteral("comment")));

        *ruleOut = parsedRuleModel;
        return true;
    }

    QString callbackRuleComboBackgroundHex()
    {
        return KswordTheme::IsDarkModeEnabled()
            ? QStringLiteral("#000000")
            : QStringLiteral("#FFFFFF");
    }

    QString callbackRuleComboTextHex()
    {
        return KswordTheme::IsDarkModeEnabled()
            ? QStringLiteral("#FFFFFF")
            : QStringLiteral("#1A1A1A");
    }

    QString callbackRuleComboStyle()
    {
        const QString backgroundHex = callbackRuleComboBackgroundHex();
        const QString textHex = callbackRuleComboTextHex();
        return QStringLiteral(
            "QComboBox{"
            "  background:%1;"
            "  color:%2;"
            "  border:1px solid %3;"
            "  border-radius:2px;"
            "  padding:2px 6px;"
            "}"
            "QComboBox::drop-down{"
            "  border:0px;"
            "}"
            "QComboBox QAbstractItemView{"
            "  background:%1;"
            "  color:%2;"
            "  border:1px solid %3;"
            "  selection-background-color:%4;"
            "  selection-color:#FFFFFF;"
            "}")
            .arg(backgroundHex)
            .arg(textHex)
            .arg(KswordTheme::BorderHex())
            .arg(KswordTheme::PrimaryBlueHex);
    }

    void applyRuleComboStyle(QComboBox* comboBox)
    {
        if (comboBox == nullptr)
        {
            return;
        }

        comboBox->setStyleSheet(callbackRuleComboStyle());
        if (comboBox->view() != nullptr)
        {
            comboBox->view()->setStyleSheet(
                QStringLiteral(
                    "QAbstractItemView{"
                    "  background:%1;"
                    "  color:%2;"
                    "  border:1px solid %3;"
                    "  selection-background-color:%4;"
                    "  selection-color:#FFFFFF;"
                    "}")
                .arg(callbackRuleComboBackgroundHex())
                .arg(callbackRuleComboTextHex())
                .arg(KswordTheme::BorderHex())
                .arg(KswordTheme::PrimaryBlueHex));
        }
    }
}

class CallbackInterceptController final : public QObject
{
public:
    explicit CallbackInterceptController(QWidget* hostPage, QObject* parent = nullptr)
        : QObject(parent)
        , m_hostPage(hostPage)
    {
        initializeUi();
        initializeConnections();

        addDefaultGroupIfNeeded();
        refreshRuleGroupComboOptions();
        reloadRuntimeState();

        m_promptManager = CallbackPromptManager::ensureGlobalManager(
            m_hostPage != nullptr ? m_hostPage->window() : nullptr);
        if (m_promptManager != nullptr)
        {
            connect(
                m_promptManager,
                &CallbackPromptManager::logLineGenerated,
                m_hostPage,
                [this](const QString& logText) {
                    appendEventLog(logText);
                });
            m_promptManager->start();
            appendAppLog(QStringLiteral("驱动回调询问管理器已启动。"));
        }
    }

    ~CallbackInterceptController() override = default;

private:
    void initializeUi()
    {
        if (m_hostPage == nullptr)
        {
            return;
        }

        auto* rootLayout = new QVBoxLayout(m_hostPage);
        rootLayout->setContentsMargins(4, 4, 4, 4);
        rootLayout->setSpacing(6);

        auto* topBarLayout = new QHBoxLayout();
        topBarLayout->setContentsMargins(0, 0, 0, 0);
        topBarLayout->setSpacing(6);

        m_globalEnabledCheck = new QCheckBox(QStringLiteral("全局启用"), m_hostPage);
        m_globalEnabledCheck->setChecked(true);
        m_applyButton = new QPushButton(QStringLiteral("应用"), m_hostPage);
        m_reloadStateButton = new QPushButton(QStringLiteral("重新加载驱动状态"), m_hostPage);
        m_importButton = new QPushButton(QStringLiteral("导入配置"), m_hostPage);
        m_exportButton = new QPushButton(QStringLiteral("导出配置"), m_hostPage);

        const auto setupIconButton = [this](QPushButton* button, const QIcon& iconValue, const QString& tipText) {
            if (button == nullptr || m_hostPage == nullptr)
            {
                return;
            }
            button->setText(QString());
            button->setIcon(iconValue);
            button->setToolTip(tipText);
            button->setFixedSize(30, 26);
            button->setIconSize(QSize(16, 16));
        };
        setupIconButton(m_applyButton, QIcon(QStringLiteral(":/Icon/process_start.svg")), QStringLiteral("应用规则"));
        setupIconButton(m_reloadStateButton, QIcon(QStringLiteral(":/Icon/process_refresh.svg")), QStringLiteral("重新加载驱动状态"));
        setupIconButton(m_importButton, QIcon(QStringLiteral(":/Icon/codeeditor_open.svg")), QStringLiteral("导入配置"));
        setupIconButton(m_exportButton, QIcon(QStringLiteral(":/Icon/log_export.svg")), QStringLiteral("导出配置"));

        topBarLayout->addWidget(m_globalEnabledCheck, 0);
        topBarLayout->addWidget(m_applyButton, 0);
        topBarLayout->addWidget(m_reloadStateButton, 0);
        topBarLayout->addWidget(m_importButton, 0);
        topBarLayout->addWidget(m_exportButton, 0);
        topBarLayout->addStretch(1);
        rootLayout->addLayout(topBarLayout, 0);

        m_statusLabel = new QLabel(QStringLiteral("状态：等待刷新"), m_hostPage);
        m_statusLabel->setStyleSheet(QStringLiteral("color:%1;font-weight:600;").arg(KswordTheme::TextSecondaryHex()));
        rootLayout->addWidget(m_statusLabel, 0);

        auto* mainSplitter = new QSplitter(Qt::Horizontal, m_hostPage);
        rootLayout->addWidget(mainSplitter, 1);

        auto* groupPane = new QWidget(mainSplitter);
        auto* groupLayout = new QVBoxLayout(groupPane);
        groupLayout->setContentsMargins(0, 0, 0, 0);
        groupLayout->setSpacing(6);

        auto* groupButtonLayout = new QHBoxLayout();
        groupButtonLayout->setContentsMargins(0, 0, 0, 0);
        groupButtonLayout->setSpacing(6);
        m_addGroupButton = new QPushButton(QStringLiteral("新增组"), groupPane);
        m_removeGroupButton = new QPushButton(QStringLiteral("删除组"), groupPane);
        m_renameGroupButton = new QPushButton(QStringLiteral("重命名"), groupPane);
        m_moveGroupUpButton = new QPushButton(QStringLiteral("上移"), groupPane);
        m_moveGroupDownButton = new QPushButton(QStringLiteral("下移"), groupPane);
        setupIconButton(m_addGroupButton, QIcon(QStringLiteral(":/Icon/process_start.svg")), QStringLiteral("新增规则组"));
        setupIconButton(m_removeGroupButton, QIcon(QStringLiteral(":/Icon/log_clear.svg")), QStringLiteral("删除当前规则组"));
        setupIconButton(m_renameGroupButton, QIcon(QStringLiteral(":/Icon/process_details.svg")), QStringLiteral("重命名当前规则组"));
        setupIconButton(m_moveGroupUpButton, QIcon(QStringLiteral(":/Icon/file_nav_up.svg")), QStringLiteral("规则组上移"));
        setupIconButton(m_moveGroupDownButton, QIcon(QStringLiteral(":/Icon/codeeditor_goto.svg")), QStringLiteral("规则组下移"));
        groupButtonLayout->addWidget(m_addGroupButton, 0);
        groupButtonLayout->addWidget(m_removeGroupButton, 0);
        groupButtonLayout->addWidget(m_renameGroupButton, 0);
        groupButtonLayout->addWidget(m_moveGroupUpButton, 0);
        groupButtonLayout->addWidget(m_moveGroupDownButton, 0);
        groupButtonLayout->addStretch(1);
        groupLayout->addLayout(groupButtonLayout, 0);

        m_groupTable = new QTableWidget(groupPane);
        m_groupTable->setColumnCount(static_cast<int>(GroupColumn::Count));
        m_groupTable->setHorizontalHeaderLabels(QStringList{
            QStringLiteral("groupId"),
            QStringLiteral("组名称"),
            QStringLiteral("启用"),
            QStringLiteral("优先级"),
            QStringLiteral("备注")
            });
        m_groupTable->setSelectionBehavior(QAbstractItemView::SelectRows);
        m_groupTable->setSelectionMode(QAbstractItemView::SingleSelection);
        m_groupTable->setEditTriggers(
            QAbstractItemView::DoubleClicked |
            QAbstractItemView::SelectedClicked |
            QAbstractItemView::EditKeyPressed);
        m_groupTable->setItemDelegate(new OpaqueTableEditorDelegate(m_groupTable));
        m_groupTable->verticalHeader()->setVisible(false);
        m_groupTable->horizontalHeader()->setSectionResizeMode(QHeaderView::ResizeToContents);
        m_groupTable->horizontalHeader()->setSectionResizeMode(static_cast<int>(GroupColumn::Comment), QHeaderView::Stretch);
        groupLayout->addWidget(m_groupTable, 1);

        auto* rightPane = new QWidget(mainSplitter);
        auto* rightLayout = new QVBoxLayout(rightPane);
        rightLayout->setContentsMargins(0, 0, 0, 0);
        rightLayout->setSpacing(6);

        auto* ruleToolbarLayout = new QHBoxLayout();
        ruleToolbarLayout->setContentsMargins(0, 0, 0, 0);
        ruleToolbarLayout->setSpacing(6);
        m_addRuleButton = new QPushButton(QStringLiteral("新增规则"), rightPane);
        m_removeRuleButton = new QPushButton(QStringLiteral("删除规则"), rightPane);
        m_moveRuleUpButton = new QPushButton(QStringLiteral("规则上移"), rightPane);
        m_moveRuleDownButton = new QPushButton(QStringLiteral("规则下移"), rightPane);
        setupIconButton(m_addRuleButton, QIcon(QStringLiteral(":/Icon/process_start.svg")), QStringLiteral("新增规则"));
        setupIconButton(m_removeRuleButton, QIcon(QStringLiteral(":/Icon/log_clear.svg")), QStringLiteral("删除当前规则"));
        setupIconButton(m_moveRuleUpButton, QIcon(QStringLiteral(":/Icon/file_nav_up.svg")), QStringLiteral("规则上移"));
        setupIconButton(m_moveRuleDownButton, QIcon(QStringLiteral(":/Icon/codeeditor_goto.svg")), QStringLiteral("规则下移"));
        ruleToolbarLayout->addWidget(m_addRuleButton, 0);
        ruleToolbarLayout->addWidget(m_removeRuleButton, 0);
        ruleToolbarLayout->addWidget(m_moveRuleUpButton, 0);
        ruleToolbarLayout->addWidget(m_moveRuleDownButton, 0);
        ruleToolbarLayout->addStretch(1);
        rightLayout->addLayout(ruleToolbarLayout, 0);

        m_ruleTabWidget = new QTabWidget(rightPane);
        rightLayout->addWidget(m_ruleTabWidget, 1);

        createRuleTableTab(KSWORD_ARK_CALLBACK_TYPE_REGISTRY, QStringLiteral("注册表"));
        createRuleTableTab(KSWORD_ARK_CALLBACK_TYPE_PROCESS_CREATE, QStringLiteral("进程创建"));
        createRuleTableTab(KSWORD_ARK_CALLBACK_TYPE_THREAD_CREATE, QStringLiteral("线程创建"));
        createRuleTableTab(KSWORD_ARK_CALLBACK_TYPE_IMAGE_LOAD, QStringLiteral("镜像加载"));
        createRuleTableTab(KSWORD_ARK_CALLBACK_TYPE_OBJECT, QStringLiteral("对象管理器"));

        auto* reservedPage = new QWidget(m_ruleTabWidget);
        auto* reservedLayout = new QVBoxLayout(reservedPage);
        reservedLayout->setContentsMargins(12, 12, 12, 12);
        auto* reservedLabel = new QLabel(
            QStringLiteral("文件系统微过滤器（预留）\n当前版本未实现。"),
            reservedPage);
        reservedLabel->setStyleSheet(QStringLiteral("color:%1;font-weight:600;").arg(KswordTheme::TextSecondaryHex()));
        reservedLabel->setAlignment(Qt::AlignCenter);
        reservedLayout->addStretch(1);
        reservedLayout->addWidget(reservedLabel, 0);
        reservedLayout->addStretch(1);
        const int reservedTabIndex = m_ruleTabWidget->addTab(reservedPage, QStringLiteral("文件系统微过滤器（预留）"));
        m_tabCallbackTypeMap.insert(reservedTabIndex, KSWORD_ARK_CALLBACK_TYPE_MINIFILTER_RESERVED);

        auto* logTabWidget = new QTabWidget(m_hostPage);
        m_appLogEditor = new QPlainTextEdit(logTabWidget);
        m_eventLogEditor = new QPlainTextEdit(logTabWidget);
        m_appLogEditor->setReadOnly(true);
        m_eventLogEditor->setReadOnly(true);
        logTabWidget->addTab(m_appLogEditor, QStringLiteral("应用日志"));
        logTabWidget->addTab(m_eventLogEditor, QStringLiteral("事件日志"));
        rootLayout->addWidget(logTabWidget, 0);

        mainSplitter->setStretchFactor(0, 3);
        mainSplitter->setStretchFactor(1, 7);
    }

    void initializeConnections()
    {
        if (m_globalEnabledCheck != nullptr)
        {
            connect(m_globalEnabledCheck, &QCheckBox::toggled, m_hostPage, [this](bool) {
                setDirtyState(true);
            });
        }

        connect(m_applyButton, &QPushButton::clicked, m_hostPage, [this]() {
            applyRulesToDriver();
        });
        connect(m_reloadStateButton, &QPushButton::clicked, m_hostPage, [this]() {
            reloadRuntimeState();
        });
        connect(m_importButton, &QPushButton::clicked, m_hostPage, [this]() {
            importConfigFromFile();
        });
        connect(m_exportButton, &QPushButton::clicked, m_hostPage, [this]() {
            exportConfigToFile();
        });

        connect(m_addGroupButton, &QPushButton::clicked, m_hostPage, [this]() {
            addGroupRow(0U);
        });
        connect(m_removeGroupButton, &QPushButton::clicked, m_hostPage, [this]() {
            removeCurrentGroup();
        });
        connect(m_renameGroupButton, &QPushButton::clicked, m_hostPage, [this]() {
            renameCurrentGroup();
        });
        connect(m_moveGroupUpButton, &QPushButton::clicked, m_hostPage, [this]() {
            moveCurrentGroup(-1);
        });
        connect(m_moveGroupDownButton, &QPushButton::clicked, m_hostPage, [this]() {
            moveCurrentGroup(1);
        });

        connect(m_addRuleButton, &QPushButton::clicked, m_hostPage, [this]() {
            addRuleToCurrentTab();
        });
        connect(m_removeRuleButton, &QPushButton::clicked, m_hostPage, [this]() {
            removeCurrentRule();
        });
        connect(m_moveRuleUpButton, &QPushButton::clicked, m_hostPage, [this]() {
            moveCurrentRule(-1);
        });
        connect(m_moveRuleDownButton, &QPushButton::clicked, m_hostPage, [this]() {
            moveCurrentRule(1);
        });

        connect(m_groupTable, &QTableWidget::itemChanged, m_hostPage, [this](QTableWidgetItem*) {
            if (m_ignoreUiSignal)
            {
                return;
            }
            refreshRuleGroupComboOptions();
            setDirtyState(true);
        });

        connect(m_ruleTabWidget, &QTabWidget::currentChanged, m_hostPage, [this](int) {
            const quint32 callbackType = currentRuleCallbackType();
            const bool reservedTab = (callbackType == KSWORD_ARK_CALLBACK_TYPE_MINIFILTER_RESERVED);
            m_addRuleButton->setEnabled(!reservedTab);
            m_removeRuleButton->setEnabled(!reservedTab);
            m_moveRuleUpButton->setEnabled(!reservedTab);
            m_moveRuleDownButton->setEnabled(!reservedTab);
        });
    }

    void createRuleTableTab(quint32 callbackType, const QString& titleText)
    {
        auto* tabPage = new QWidget(m_ruleTabWidget);
        auto* tabLayout = new QVBoxLayout(tabPage);
        tabLayout->setContentsMargins(0, 0, 0, 0);
        tabLayout->setSpacing(0);

        auto* ruleTable = new QTableWidget(tabPage);
        ruleTable->setColumnCount(static_cast<int>(RuleColumn::Count));
        ruleTable->setHorizontalHeaderLabels(QStringList{
            QStringLiteral("启用"),
            QStringLiteral("RuleID"),
            QStringLiteral("GroupID"),
            QStringLiteral("规则名称"),
            QStringLiteral("操作类型"),
            QStringLiteral("匹配模式"),
            QStringLiteral("动作"),
            QStringLiteral("超时毫秒"),
            QStringLiteral("超时决策"),
            QStringLiteral("优先级")
            });
        ruleTable->setSelectionBehavior(QAbstractItemView::SelectRows);
        ruleTable->setSelectionMode(QAbstractItemView::SingleSelection);
        ruleTable->setEditTriggers(
            QAbstractItemView::DoubleClicked |
            QAbstractItemView::SelectedClicked |
            QAbstractItemView::EditKeyPressed);
        ruleTable->setItemDelegate(new OpaqueTableEditorDelegate(ruleTable));
        ruleTable->setSortingEnabled(false);
        ruleTable->setWordWrap(false);
        ruleTable->setContextMenuPolicy(Qt::CustomContextMenu);
        ruleTable->verticalHeader()->setVisible(false);
        ruleTable->horizontalHeader()->setSectionResizeMode(QHeaderView::ResizeToContents);
        ruleTable->horizontalHeader()->setSectionResizeMode(static_cast<int>(RuleColumn::RuleName), QHeaderView::Stretch);
        tabLayout->addWidget(ruleTable, 1);

        connect(ruleTable, &QTableWidget::itemChanged, m_hostPage, [this](QTableWidgetItem*) {
            if (m_ignoreUiSignal)
            {
                return;
            }
            setDirtyState(true);
        });
        connect(ruleTable, &QWidget::customContextMenuRequested, m_hostPage, [this, ruleTable, callbackType](const QPoint& localPos) {
            showRuleTableContextMenu(ruleTable, callbackType, localPos);
        });

        const int tabIndex = m_ruleTabWidget->addTab(tabPage, titleText);
        m_tabCallbackTypeMap.insert(tabIndex, callbackType);
        m_ruleTableMap.insert(callbackType, ruleTable);
    }

    bool collectRuleByLogicalIndex(
        QTableWidget* ruleTable,
        const quint32 callbackType,
        const int logicalRuleIndex,
        CallbackRuleModel* ruleOut,
        QString* errorTextOut) const
    {
        if (ruleOut == nullptr)
        {
            if (errorTextOut != nullptr)
            {
                *errorTextOut = QStringLiteral("内部错误：ruleOut 为空。");
            }
            return false;
        }
        if (ruleTable == nullptr)
        {
            if (errorTextOut != nullptr)
            {
                *errorTextOut = QStringLiteral("内部错误：ruleTable 为空。");
            }
            return false;
        }

        QList<CallbackRuleModel> ruleList;
        if (!collectRuleListFromTable(ruleTable, callbackType, &ruleList, errorTextOut))
        {
            return false;
        }
        if (logicalRuleIndex < 0 || logicalRuleIndex >= ruleList.size())
        {
            if (errorTextOut != nullptr)
            {
                *errorTextOut = QStringLiteral("当前未选中有效规则。");
            }
            return false;
        }

        *ruleOut = ruleList.at(logicalRuleIndex);
        return true;
    }

    void copyCurrentRuleToClipboard(
        QTableWidget* ruleTable,
        const quint32 callbackType)
    {
        if (ruleTable == nullptr)
        {
            return;
        }

        const int logicalRuleIndex = currentRuleLogicalIndex(ruleTable);
        if (logicalRuleIndex < 0)
        {
            return;
        }

        CallbackRuleModel selectedRuleModel;
        QString errorText;
        if (!collectRuleByLogicalIndex(
            ruleTable,
            callbackType,
            logicalRuleIndex,
            &selectedRuleModel,
            &errorText))
        {
            QMessageBox::warning(
                m_hostPage,
                QStringLiteral("驱动回调"),
                QStringLiteral("复制规则失败：%1").arg(errorText));
            appendAppLog(QStringLiteral("复制规则失败：%1").arg(errorText));
            return;
        }

        QClipboard* clipboard = QApplication::clipboard();
        if (clipboard == nullptr)
        {
            appendAppLog(QStringLiteral("复制规则失败：系统剪贴板不可用。"));
            return;
        }

        clipboard->setText(serializeRuleToClipboardText(selectedRuleModel));
        appendAppLog(
            QStringLiteral("已复制规则到剪贴板：ruleId=%1，类型=%2")
            .arg(selectedRuleModel.ruleId)
            .arg(callbackTypeToDisplayText(callbackType)));
    }

    void pasteRuleFromClipboard(
        QTableWidget* ruleTable,
        const quint32 callbackType)
    {
        if (ruleTable == nullptr)
        {
            return;
        }

        QClipboard* clipboard = QApplication::clipboard();
        if (clipboard == nullptr)
        {
            QMessageBox::warning(
                m_hostPage,
                QStringLiteral("驱动回调"),
                QStringLiteral("粘贴失败：系统剪贴板不可用。"));
            appendAppLog(QStringLiteral("粘贴失败：系统剪贴板不可用。"));
            return;
        }

        const QString clipboardText = clipboard->text().trimmed();
        if (clipboardText.isEmpty())
        {
            QMessageBox::information(
                m_hostPage,
                QStringLiteral("驱动回调"),
                QStringLiteral("剪贴板为空，无法粘贴规则。"));
            return;
        }

        CallbackRuleModel pastedRuleModel;
        QString parseErrorText;
        if (!deserializeRuleFromClipboardText(clipboardText, &pastedRuleModel, &parseErrorText))
        {
            QMessageBox::warning(
                m_hostPage,
                QStringLiteral("驱动回调"),
                QStringLiteral("粘贴失败：%1").arg(parseErrorText));
            appendAppLog(QStringLiteral("粘贴失败：%1").arg(parseErrorText));
            return;
        }

        const quint32 sourceCallbackType = pastedRuleModel.callbackType;
        pastedRuleModel.ruleId = allocateNextRuleId();
        pastedRuleModel.callbackType = callbackType;
        pastedRuleModel.initiatorPattern = normalizeMatchAllPattern(pastedRuleModel.initiatorPattern);
        pastedRuleModel.targetPattern = normalizeMatchAllPattern(pastedRuleModel.targetPattern);
        if (pastedRuleModel.ruleName.trimmed().isEmpty())
        {
            pastedRuleModel.ruleName = QStringLiteral("规则%1").arg(pastedRuleModel.ruleId);
        }
        if (pastedRuleModel.comment.trimmed().isEmpty())
        {
            pastedRuleModel.comment = QStringLiteral("粘贴规则");
        }

        addDefaultGroupIfNeeded();
        if (!groupExists(pastedRuleModel.groupId))
        {
            pastedRuleModel.groupId = firstGroupId();
        }
        if (pastedRuleModel.groupId == 0U)
        {
            QMessageBox::warning(
                m_hostPage,
                QStringLiteral("驱动回调"),
                QStringLiteral("粘贴失败：当前没有可用规则组。"));
            appendAppLog(QStringLiteral("粘贴失败：当前没有可用规则组。"));
            return;
        }

        if (pastedRuleModel.operationMask == 0U)
        {
            pastedRuleModel.operationMask = defaultOperationMaskByType(callbackType);
        }

        const QList<QPair<QString, quint32>> matchModeOptionList = allowedMatchModeListByType(callbackType);
        if (!containsOptionValue(matchModeOptionList, pastedRuleModel.matchMode))
        {
            pastedRuleModel.matchMode = matchModeOptionList.isEmpty()
                ? KSWORD_ARK_MATCH_MODE_EXACT
                : matchModeOptionList.front().second;
        }

        const QList<QPair<QString, quint32>> actionOptionList = allowedActionListByType(callbackType);
        if (!containsOptionValue(actionOptionList, pastedRuleModel.action))
        {
            pastedRuleModel.action = actionOptionList.isEmpty()
                ? KSWORD_ARK_RULE_ACTION_LOG_ONLY
                : actionOptionList.front().second;
        }

        if (callbackType == KSWORD_ARK_CALLBACK_TYPE_REGISTRY &&
            pastedRuleModel.matchMode == KSWORD_ARK_MATCH_MODE_REGEX &&
            pastedRuleModel.action != KSWORD_ARK_RULE_ACTION_ASK_USER &&
            containsOptionValue(actionOptionList, KSWORD_ARK_RULE_ACTION_ASK_USER))
        {
            pastedRuleModel.action = KSWORD_ARK_RULE_ACTION_ASK_USER;
        }

        if (pastedRuleModel.action == KSWORD_ARK_RULE_ACTION_ASK_USER)
        {
            if (pastedRuleModel.timeoutMs == 0U)
            {
                pastedRuleModel.timeoutMs = 5000U;
            }
        }
        else
        {
            pastedRuleModel.timeoutMs = 0U;
        }
        if (pastedRuleModel.timeoutDefaultDecision != KSWORD_ARK_DECISION_ALLOW &&
            pastedRuleModel.timeoutDefaultDecision != KSWORD_ARK_DECISION_DENY)
        {
            pastedRuleModel.timeoutDefaultDecision = KSWORD_ARK_DECISION_ALLOW;
        }

        pastedRuleModel.priority = (ruleCountOfTable(ruleTable) + 1) * 10;

        m_ignoreUiSignal = true;
        appendRuleRow(ruleTable, callbackType, pastedRuleModel);
        m_ignoreUiSignal = false;

        const int newHeaderRow = ruleTable->rowCount() - 2;
        if (newHeaderRow >= 0)
        {
            ruleTable->setCurrentCell(newHeaderRow, static_cast<int>(RuleColumn::RuleName));
        }
        setDirtyState(true);

        if (sourceCallbackType != callbackType)
        {
            appendAppLog(
                QStringLiteral("剪贴板规则类型已转换：%1 -> %2")
                .arg(callbackTypeToDisplayText(sourceCallbackType))
                .arg(callbackTypeToDisplayText(callbackType)));
        }
        appendAppLog(QStringLiteral("已从剪贴板粘贴规则：newRuleId=%1").arg(pastedRuleModel.ruleId));
    }

    void showRuleTableContextMenu(
        QTableWidget* ruleTable,
        const quint32 callbackType,
        const QPoint& localPos)
    {
        if (ruleTable == nullptr || callbackType == KSWORD_ARK_CALLBACK_TYPE_MINIFILTER_RESERVED)
        {
            return;
        }

        const int clickedRow = ruleTable->rowAt(localPos.y());
        if (clickedRow >= 0)
        {
            const int headerRow = normalizeRuleHeaderRow(clickedRow);
            if (headerRow >= 0)
            {
                ruleTable->setCurrentCell(headerRow, static_cast<int>(RuleColumn::RuleName));
            }
        }

        const int logicalRuleIndex = currentRuleLogicalIndex(ruleTable);
        const int ruleCount = ruleCountOfTable(ruleTable);
        const bool hasCurrentRule = (logicalRuleIndex >= 0 && logicalRuleIndex < ruleCount);

        QMenu contextMenu(ruleTable);
        QAction* addRuleAction = contextMenu.addAction(
            QIcon(QStringLiteral(":/Icon/process_start.svg")),
            QStringLiteral("新增规则"));
        QAction* removeRuleAction = contextMenu.addAction(
            QIcon(QStringLiteral(":/Icon/log_clear.svg")),
            QStringLiteral("删除当前规则"));
        QAction* moveUpRuleAction = contextMenu.addAction(
            QIcon(QStringLiteral(":/Icon/file_nav_up.svg")),
            QStringLiteral("上移当前规则"));
        QAction* moveDownRuleAction = contextMenu.addAction(
            QIcon(QStringLiteral(":/Icon/codeeditor_goto.svg")),
            QStringLiteral("下移当前规则"));
        contextMenu.addSeparator();
        QAction* copyRuleAction = contextMenu.addAction(
            QIcon(QStringLiteral(":/Icon/process_copy_row.svg")),
            QStringLiteral("复制规则文本"));
        QAction* pasteRuleAction = contextMenu.addAction(
            QIcon(QStringLiteral(":/Icon/codeeditor_paste.svg")),
            QStringLiteral("粘贴为新规则"));

        removeRuleAction->setEnabled(hasCurrentRule);
        moveUpRuleAction->setEnabled(hasCurrentRule && logicalRuleIndex > 0);
        moveDownRuleAction->setEnabled(hasCurrentRule && logicalRuleIndex < (ruleCount - 1));
        copyRuleAction->setEnabled(hasCurrentRule);
        pasteRuleAction->setEnabled(QApplication::clipboard() != nullptr &&
            !QApplication::clipboard()->text().trimmed().isEmpty());

        QAction* selectedAction = contextMenu.exec(ruleTable->viewport()->mapToGlobal(localPos));
        if (selectedAction == nullptr)
        {
            return;
        }

        if (m_ruleTabWidget != nullptr)
        {
            const int tabIndex = m_tabCallbackTypeMap.key(callbackType, -1);
            if (tabIndex >= 0 && tabIndex != m_ruleTabWidget->currentIndex())
            {
                m_ruleTabWidget->setCurrentIndex(tabIndex);
            }
        }

        if (selectedAction == addRuleAction)
        {
            addRuleToCurrentTab();
            return;
        }
        if (selectedAction == removeRuleAction)
        {
            removeCurrentRule();
            return;
        }
        if (selectedAction == moveUpRuleAction)
        {
            moveCurrentRule(-1);
            return;
        }
        if (selectedAction == moveDownRuleAction)
        {
            moveCurrentRule(1);
            return;
        }
        if (selectedAction == copyRuleAction)
        {
            copyCurrentRuleToClipboard(ruleTable, callbackType);
            return;
        }
        if (selectedAction == pasteRuleAction)
        {
            pasteRuleFromClipboard(ruleTable, callbackType);
            return;
        }
    }

    void addDefaultGroupIfNeeded()
    {
        if (m_groupTable->rowCount() > 0)
        {
            return;
        }

        CallbackRuleGroupModel defaultGroup;
        defaultGroup.groupId = 1U;
        defaultGroup.groupName = QStringLiteral("默认组");
        defaultGroup.enabled = true;
        defaultGroup.priority = 10;
        defaultGroup.comment = QStringLiteral("默认规则组");
        appendGroupRow(defaultGroup);
        m_groupTable->setCurrentCell(0, static_cast<int>(GroupColumn::Name));
    }

    void appendGroupRow(const CallbackRuleGroupModel& groupModel)
    {
        const int rowIndex = m_groupTable->rowCount();
        m_groupTable->insertRow(rowIndex);

        m_groupTable->setItem(rowIndex, static_cast<int>(GroupColumn::Id), makeReadOnlyItem(QString::number(groupModel.groupId)));
        m_groupTable->setItem(rowIndex, static_cast<int>(GroupColumn::Name), new QTableWidgetItem(groupModel.groupName));

        auto* enabledItem = new QTableWidgetItem();
        enabledItem->setFlags(enabledItem->flags() | Qt::ItemIsUserCheckable);
        enabledItem->setCheckState(groupModel.enabled ? Qt::Checked : Qt::Unchecked);
        m_groupTable->setItem(rowIndex, static_cast<int>(GroupColumn::Enabled), enabledItem);

        m_groupTable->setItem(rowIndex, static_cast<int>(GroupColumn::Priority), new QTableWidgetItem(QString::number(groupModel.priority)));
        m_groupTable->setItem(rowIndex, static_cast<int>(GroupColumn::Comment), new QTableWidgetItem(groupModel.comment));
    }

    void setDirtyState(const bool dirtyState)
    {
        m_dirty = dirtyState;
        updateStatusLabel();
    }

    void appendAppLog(const QString& logText)
    {
        if (m_appLogEditor == nullptr)
        {
            return;
        }
        const QString lineText = QStringLiteral("[%1] %2")
            .arg(QDateTime::currentDateTime().toString(QStringLiteral("HH:mm:ss")))
            .arg(logText);
        m_appLogEditor->appendPlainText(lineText);
    }

    void appendEventLog(const QString& logText)
    {
        if (m_eventLogEditor == nullptr)
        {
            return;
        }
        const QString lineText = QStringLiteral("[%1] %2")
            .arg(QDateTime::currentDateTime().toString(QStringLiteral("HH:mm:ss")))
            .arg(logText);
        m_eventLogEditor->appendPlainText(lineText);
    }

    quint32 allocateNextGroupId() const
    {
        quint32 maxGroupId = 0U;
        for (int rowIndex = 0; rowIndex < m_groupTable->rowCount(); ++rowIndex)
        {
            QTableWidgetItem* groupIdItem = m_groupTable->item(rowIndex, static_cast<int>(GroupColumn::Id));
            if (groupIdItem == nullptr)
            {
                continue;
            }
            quint32 groupId = 0U;
            if (parseUnsignedText(groupIdItem->text(), &groupId))
            {
                maxGroupId = std::max(maxGroupId, groupId);
            }
        }
        return maxGroupId + 1U;
    }

    quint32 allocateNextRuleId() const
    {
        quint32 maxRuleId = 0U;
        for (auto iterator = m_ruleTableMap.begin(); iterator != m_ruleTableMap.end(); ++iterator)
        {
            const QTableWidget* ruleTable = iterator.value();
            if (ruleTable == nullptr)
            {
                continue;
            }
            for (int rowIndex = 0; rowIndex < ruleTable->rowCount(); ++rowIndex)
            {
                const QTableWidgetItem* ruleIdItem = ruleTable->item(rowIndex, static_cast<int>(RuleColumn::RuleId));
                if (ruleIdItem == nullptr)
                {
                    continue;
                }
                quint32 ruleId = 0U;
                if (parseUnsignedText(ruleIdItem->text(), &ruleId))
                {
                    maxRuleId = std::max(maxRuleId, ruleId);
                }
            }
        }
        return maxRuleId + 1U;
    }

    quint32 firstGroupId() const
    {
        for (int rowIndex = 0; rowIndex < m_groupTable->rowCount(); ++rowIndex)
        {
            QTableWidgetItem* groupIdItem = m_groupTable->item(rowIndex, static_cast<int>(GroupColumn::Id));
            if (groupIdItem == nullptr)
            {
                continue;
            }
            quint32 groupId = 0U;
            if (parseUnsignedText(groupIdItem->text(), &groupId))
            {
                return groupId;
            }
        }
        return 0U;
    }

    bool groupExists(const quint32 groupId) const
    {
        if (groupId == 0U)
        {
            return false;
        }

        for (int rowIndex = 0; rowIndex < m_groupTable->rowCount(); ++rowIndex)
        {
            QTableWidgetItem* groupIdItem = m_groupTable->item(rowIndex, static_cast<int>(GroupColumn::Id));
            if (groupIdItem == nullptr)
            {
                continue;
            }
            quint32 currentGroupId = 0U;
            if (parseUnsignedText(groupIdItem->text(), &currentGroupId) && currentGroupId == groupId)
            {
                return true;
            }
        }
        return false;
    }

    int ruleCountOfTable(const QTableWidget* ruleTable) const
    {
        if (ruleTable == nullptr || ruleTable->rowCount() <= 0)
        {
            return 0;
        }
        return ruleTable->rowCount() / 2;
    }

    int normalizeRuleHeaderRow(const int anyRowIndex) const
    {
        if (anyRowIndex < 0)
        {
            return -1;
        }
        return (anyRowIndex % 2 == 0) ? anyRowIndex : (anyRowIndex - 1);
    }

    int currentRuleLogicalIndex(const QTableWidget* ruleTable) const
    {
        if (ruleTable == nullptr)
        {
            return -1;
        }
        const int headerRow = normalizeRuleHeaderRow(ruleTable->currentRow());
        if (headerRow < 0)
        {
            return -1;
        }
        return headerRow / 2;
    }

    void addGroupRow(const quint32 preferredId)
    {
        CallbackRuleGroupModel newGroup;
        newGroup.groupId = (preferredId == 0U) ? allocateNextGroupId() : preferredId;
        newGroup.groupName = QStringLiteral("规则组%1").arg(newGroup.groupId);
        newGroup.enabled = true;
        newGroup.priority = (m_groupTable->rowCount() + 1) * 10;
        newGroup.comment = QStringLiteral("新建规则组");

        m_ignoreUiSignal = true;
        appendGroupRow(newGroup);
        m_ignoreUiSignal = false;

        refreshRuleGroupComboOptions();
        setDirtyState(true);
        appendAppLog(QStringLiteral("新增规则组成功：groupId=%1").arg(newGroup.groupId));
    }

    void removeCurrentGroup()
    {
        const int rowIndex = m_groupTable->currentRow();
        if (rowIndex < 0)
        {
            return;
        }

        QTableWidgetItem* groupIdItem = m_groupTable->item(rowIndex, static_cast<int>(GroupColumn::Id));
        if (groupIdItem == nullptr)
        {
            return;
        }

        quint32 groupId = 0U;
        if (!parseUnsignedText(groupIdItem->text(), &groupId))
        {
            return;
        }

        QList<CallbackRuleModel> allRuleList;
        QString ruleErrorText;
        if (!collectAllRulesFromUi(&allRuleList, &ruleErrorText))
        {
            QMessageBox::warning(m_hostPage, QStringLiteral("驱动回调"), ruleErrorText);
            return;
        }
        allRuleList.erase(
            std::remove_if(
                allRuleList.begin(),
                allRuleList.end(),
                [groupId](const CallbackRuleModel& ruleModel) {
                    return ruleModel.groupId == groupId;
                }),
            allRuleList.end());

        m_ignoreUiSignal = true;
        m_groupTable->removeRow(rowIndex);
        for (auto iterator = m_ruleTableMap.begin(); iterator != m_ruleTableMap.end(); ++iterator)
        {
            QTableWidget* ruleTable = iterator.value();
            if (ruleTable != nullptr)
            {
                ruleTable->setRowCount(0);
            }
        }
        for (const CallbackRuleModel& ruleModel : allRuleList)
        {
            QTableWidget* targetRuleTable = m_ruleTableMap.value(ruleModel.callbackType, nullptr);
            if (targetRuleTable != nullptr)
            {
                appendRuleRow(targetRuleTable, ruleModel.callbackType, ruleModel);
            }
        }
        m_ignoreUiSignal = false;

        refreshRuleGroupComboOptions();
        addDefaultGroupIfNeeded();
        setDirtyState(true);
        appendAppLog(QStringLiteral("删除规则组成功：groupId=%1").arg(groupId));
    }

    void renameCurrentGroup()
    {
        const int rowIndex = m_groupTable->currentRow();
        if (rowIndex < 0)
        {
            return;
        }

        QTableWidgetItem* groupNameItem = m_groupTable->item(rowIndex, static_cast<int>(GroupColumn::Name));
        if (groupNameItem == nullptr)
        {
            return;
        }

        bool okPressed = false;
        const QString newNameText = QInputDialog::getText(
            m_hostPage,
            QStringLiteral("重命名规则组"),
            QStringLiteral("请输入组名称："),
            QLineEdit::Normal,
            groupNameItem->text(),
            &okPressed).trimmed();
        if (!okPressed || newNameText.isEmpty())
        {
            return;
        }

        groupNameItem->setText(newNameText);
        refreshRuleGroupComboOptions();
        setDirtyState(true);
        appendAppLog(QStringLiteral("规则组重命名成功：%1").arg(newNameText));
    }

    void moveCurrentGroup(const int direction)
    {
        const int currentRow = m_groupTable->currentRow();
        if (currentRow < 0)
        {
            return;
        }

        const int targetRow = currentRow + direction;
        if (targetRow < 0 || targetRow >= m_groupTable->rowCount())
        {
            return;
        }

        QList<CallbackRuleGroupModel> groupList;
        QString errorText;
        if (!collectGroupsFromUi(&groupList, &errorText))
        {
            QMessageBox::warning(m_hostPage, QStringLiteral("驱动回调"), errorText);
            return;
        }

        std::swap(groupList[currentRow], groupList[targetRow]);
        for (int index = 0; index < groupList.size(); ++index)
        {
            groupList[index].priority = (index + 1) * 10;
        }

        m_ignoreUiSignal = true;
        m_groupTable->setRowCount(0);
        for (const CallbackRuleGroupModel& groupModel : groupList)
        {
            appendGroupRow(groupModel);
        }
        m_groupTable->setCurrentCell(targetRow, static_cast<int>(GroupColumn::Name));
        m_ignoreUiSignal = false;

        refreshRuleGroupComboOptions();
        setDirtyState(true);
    }

    void addRuleToCurrentTab()
    {
        const quint32 callbackType = currentRuleCallbackType();
        QTableWidget* ruleTable = currentRuleTable();
        if (ruleTable == nullptr || callbackType == KSWORD_ARK_CALLBACK_TYPE_MINIFILTER_RESERVED)
        {
            return;
        }

        CallbackRuleModel ruleModel;
        ruleModel.ruleId = allocateNextRuleId();
        ruleModel.groupId = firstGroupId();
        ruleModel.ruleName = QStringLiteral("规则%1").arg(ruleModel.ruleId);
        ruleModel.enabled = true;
        ruleModel.callbackType = callbackType;
        ruleModel.operationMask = defaultOperationMaskByType(callbackType);
        ruleModel.initiatorPattern.clear();
        ruleModel.targetPattern.clear();
        ruleModel.matchMode = allowedMatchModeListByType(callbackType).isEmpty()
            ? KSWORD_ARK_MATCH_MODE_EXACT
            : allowedMatchModeListByType(callbackType).front().second;
        ruleModel.action = allowedActionListByType(callbackType).isEmpty()
            ? KSWORD_ARK_RULE_ACTION_LOG_ONLY
            : allowedActionListByType(callbackType).front().second;
        ruleModel.timeoutMs = (ruleModel.action == KSWORD_ARK_RULE_ACTION_ASK_USER) ? 5000U : 0U;
        ruleModel.timeoutDefaultDecision = KSWORD_ARK_DECISION_ALLOW;
        ruleModel.priority = (ruleCountOfTable(ruleTable) + 1) * 10;
        ruleModel.comment = QStringLiteral("新建规则");

        m_ignoreUiSignal = true;
        appendRuleRow(ruleTable, callbackType, ruleModel);
        m_ignoreUiSignal = false;

        ruleTable->setCurrentCell(ruleTable->rowCount() - 2, static_cast<int>(RuleColumn::RuleName));
        setDirtyState(true);
        appendAppLog(
            QStringLiteral("新增规则成功：ruleId=%1，类型=%2")
            .arg(ruleModel.ruleId)
            .arg(callbackTypeToDisplayText(callbackType)));
    }

    void removeCurrentRule()
    {
        QTableWidget* ruleTable = currentRuleTable();
        const quint32 callbackType = currentRuleCallbackType();
        if (ruleTable == nullptr)
        {
            return;
        }

        QList<CallbackRuleModel> ruleList;
        QString errorText;
        if (!collectRuleListFromTable(ruleTable, callbackType, &ruleList, &errorText))
        {
            QMessageBox::warning(m_hostPage, QStringLiteral("驱动回调"), errorText);
            return;
        }

        const int currentRuleIndex = currentRuleLogicalIndex(ruleTable);
        if (currentRuleIndex < 0 || currentRuleIndex >= ruleList.size())
        {
            return;
        }
        ruleList.removeAt(currentRuleIndex);

        m_ignoreUiSignal = true;
        ruleTable->setRowCount(0);
        for (const CallbackRuleModel& ruleModel : ruleList)
        {
            appendRuleRow(ruleTable, callbackType, ruleModel);
        }
        m_ignoreUiSignal = false;

        if (ruleCountOfTable(ruleTable) > 0)
        {
            const int targetRuleIndex = std::min(currentRuleIndex, ruleCountOfTable(ruleTable) - 1);
            ruleTable->setCurrentCell(targetRuleIndex * 2, static_cast<int>(RuleColumn::RuleName));
        }
        setDirtyState(true);
        appendAppLog(QStringLiteral("删除规则成功。"));
    }

    void moveCurrentRule(const int direction)
    {
        QTableWidget* ruleTable = currentRuleTable();
        const quint32 callbackType = currentRuleCallbackType();
        if (ruleTable == nullptr)
        {
            return;
        }

        const int currentRuleIndex = currentRuleLogicalIndex(ruleTable);
        if (currentRuleIndex < 0)
        {
            return;
        }

        QList<CallbackRuleModel> ruleList;
        QString errorText;
        if (!collectRuleListFromTable(ruleTable, callbackType, &ruleList, &errorText))
        {
            QMessageBox::warning(m_hostPage, QStringLiteral("驱动回调"), errorText);
            return;
        }

        const int targetRuleIndex = currentRuleIndex + direction;
        if (targetRuleIndex < 0 || targetRuleIndex >= ruleList.size())
        {
            return;
        }

        std::swap(ruleList[currentRuleIndex], ruleList[targetRuleIndex]);
        for (int index = 0; index < ruleList.size(); ++index)
        {
            ruleList[index].priority = (index + 1) * 10;
        }

        m_ignoreUiSignal = true;
        ruleTable->setRowCount(0);
        for (const CallbackRuleModel& ruleModel : ruleList)
        {
            appendRuleRow(ruleTable, callbackType, ruleModel);
        }
        m_ignoreUiSignal = false;

        ruleTable->setCurrentCell(targetRuleIndex * 2, static_cast<int>(RuleColumn::RuleName));
        setDirtyState(true);
    }

    void appendRuleRow(
        QTableWidget* ruleTable,
        const quint32 callbackType,
        const CallbackRuleModel& ruleModel)
    {
        if (ruleTable == nullptr)
        {
            return;
        }

        const int headerRow = ruleTable->rowCount();
        const int detailRow = headerRow + 1;
        ruleTable->insertRow(headerRow);
        ruleTable->insertRow(detailRow);

        ruleTable->setSpan(headerRow, static_cast<int>(RuleColumn::Enabled), 2, 1);
        ruleTable->setSpan(headerRow, static_cast<int>(RuleColumn::RuleId), 2, 1);

        auto* enabledItem = new QTableWidgetItem();
        enabledItem->setFlags(enabledItem->flags() | Qt::ItemIsUserCheckable);
        enabledItem->setCheckState(ruleModel.enabled ? Qt::Checked : Qt::Unchecked);
        ruleTable->setItem(headerRow, static_cast<int>(RuleColumn::Enabled), enabledItem);
        ruleTable->setItem(headerRow, static_cast<int>(RuleColumn::RuleId), makeReadOnlyItem(QString::number(ruleModel.ruleId)));

        auto* groupCombo = new QComboBox(ruleTable);
        applyRuleComboStyle(groupCombo);
        ruleTable->setCellWidget(headerRow, static_cast<int>(RuleColumn::GroupId), groupCombo);
        ruleTable->setItem(headerRow, static_cast<int>(RuleColumn::RuleName), new QTableWidgetItem(ruleModel.ruleName));

        auto* operationCombo = new QComboBox(ruleTable);
        applyRuleComboStyle(operationCombo);
        operationCombo->setEditable(true);
        operationCombo->setInsertPolicy(QComboBox::NoInsert);
        if (operationCombo->lineEdit() != nullptr)
        {
            operationCombo->lineEdit()->setPlaceholderText(QStringLiteral("可选预设或自定义，例如 0xFFFFFFFF"));
        }
        const QList<QPair<QString, quint32>> operationPresetList = operationPresetListByType(callbackType);
        for (const QPair<QString, quint32>& presetPair : operationPresetList)
        {
            operationCombo->addItem(
                QStringLiteral("%1 (%2)").arg(presetPair.first, operationMaskToText(presetPair.second)),
                presetPair.second);
        }
        const int operationPresetIndex = operationCombo->findData(ruleModel.operationMask);
        if (operationPresetIndex >= 0)
        {
            operationCombo->setCurrentIndex(operationPresetIndex);
        }
        else
        {
            operationCombo->setEditText(operationMaskToText(ruleModel.operationMask));
        }
        connect(operationCombo, &QComboBox::currentIndexChanged, m_hostPage, [this](int) {
            if (!m_ignoreUiSignal)
            {
                setDirtyState(true);
            }
        });
        if (operationCombo->lineEdit() != nullptr)
        {
            connect(operationCombo->lineEdit(), &QLineEdit::textEdited, m_hostPage, [this](const QString&) {
                if (!m_ignoreUiSignal)
                {
                    setDirtyState(true);
                }
            });
        }
        ruleTable->setCellWidget(headerRow, static_cast<int>(RuleColumn::OperationMask), operationCombo);

        auto* matchModeCombo = new QComboBox(ruleTable);
        applyRuleComboStyle(matchModeCombo);
        for (const QPair<QString, quint32>& optionPair : allowedMatchModeListByType(callbackType))
        {
            matchModeCombo->addItem(optionPair.first, optionPair.second);
        }
        const int matchModeIndex = matchModeCombo->findData(ruleModel.matchMode);
        matchModeCombo->setCurrentIndex(matchModeIndex >= 0 ? matchModeIndex : 0);
        connect(matchModeCombo, &QComboBox::currentIndexChanged, m_hostPage, [this, ruleTable, headerRow, callbackType](int) {
            if (m_ignoreUiSignal)
            {
                return;
            }

            auto* currentMatchCombo = qobject_cast<QComboBox*>(
                ruleTable->cellWidget(headerRow, static_cast<int>(RuleColumn::MatchMode)));
            auto* currentActionCombo = qobject_cast<QComboBox*>(
                ruleTable->cellWidget(headerRow, static_cast<int>(RuleColumn::Action)));
            const quint32 matchMode =
                (currentMatchCombo != nullptr)
                ? static_cast<quint32>(currentMatchCombo->currentData().toUInt())
                : KSWORD_ARK_MATCH_MODE_EXACT;
            const quint32 actionType =
                (currentActionCombo != nullptr)
                ? static_cast<quint32>(currentActionCombo->currentData().toUInt())
                : KSWORD_ARK_RULE_ACTION_ALLOW;

            if (callbackType == KSWORD_ARK_CALLBACK_TYPE_REGISTRY &&
                matchMode == KSWORD_ARK_MATCH_MODE_REGEX &&
                actionType != KSWORD_ARK_RULE_ACTION_ASK_USER &&
                currentActionCombo != nullptr)
            {
                const int askUserIndex = currentActionCombo->findData(
                    QVariant::fromValue(static_cast<uint>(KSWORD_ARK_RULE_ACTION_ASK_USER)));
                if (askUserIndex >= 0)
                {
                    m_ignoreUiSignal = true;
                    currentActionCombo->setCurrentIndex(askUserIndex);
                    m_ignoreUiSignal = false;
                }
            }
            setDirtyState(true);
        });
        ruleTable->setCellWidget(headerRow, static_cast<int>(RuleColumn::MatchMode), matchModeCombo);

        auto* actionCombo = new QComboBox(ruleTable);
        applyRuleComboStyle(actionCombo);
        for (const QPair<QString, quint32>& optionPair : allowedActionListByType(callbackType))
        {
            actionCombo->addItem(optionPair.first, optionPair.second);
        }
        const int actionIndex = actionCombo->findData(ruleModel.action);
        actionCombo->setCurrentIndex(actionIndex >= 0 ? actionIndex : 0);
        connect(actionCombo, &QComboBox::currentIndexChanged, m_hostPage, [this, ruleTable, headerRow](int) {
            if (m_ignoreUiSignal)
            {
                return;
            }
            auto* currentActionCombo = qobject_cast<QComboBox*>(
                ruleTable->cellWidget(headerRow, static_cast<int>(RuleColumn::Action)));
            auto* currentMatchCombo = qobject_cast<QComboBox*>(
                ruleTable->cellWidget(headerRow, static_cast<int>(RuleColumn::MatchMode)));
            const quint32 actionType = (currentActionCombo != nullptr)
                ? static_cast<quint32>(currentActionCombo->currentData().toUInt())
                : KSWORD_ARK_RULE_ACTION_ALLOW;
            const quint32 matchMode = (currentMatchCombo != nullptr)
                ? static_cast<quint32>(currentMatchCombo->currentData().toUInt())
                : KSWORD_ARK_MATCH_MODE_EXACT;
            QTableWidgetItem* timeoutItem = ruleTable->item(headerRow, static_cast<int>(RuleColumn::TimeoutMs));
            if (timeoutItem != nullptr && actionType != KSWORD_ARK_RULE_ACTION_ASK_USER)
            {
                timeoutItem->setText(QStringLiteral("0"));
            }
            if (timeoutItem != nullptr && actionType == KSWORD_ARK_RULE_ACTION_ASK_USER)
            {
                quint32 timeoutValue = 0U;
                if (!parseUnsignedText(timeoutItem->text(), &timeoutValue) || timeoutValue == 0U)
                {
                    timeoutItem->setText(QStringLiteral("5000"));
                }
            }

            if (actionType != KSWORD_ARK_RULE_ACTION_ASK_USER &&
                matchMode == KSWORD_ARK_MATCH_MODE_REGEX &&
                currentMatchCombo != nullptr)
            {
                const int exactIndex = currentMatchCombo->findData(
                    QVariant::fromValue(static_cast<uint>(KSWORD_ARK_MATCH_MODE_EXACT)));
                if (exactIndex >= 0)
                {
                    m_ignoreUiSignal = true;
                    currentMatchCombo->setCurrentIndex(exactIndex);
                    m_ignoreUiSignal = false;
                }
            }
            setDirtyState(true);
        });
        ruleTable->setCellWidget(headerRow, static_cast<int>(RuleColumn::Action), actionCombo);

        ruleTable->setItem(
            headerRow,
            static_cast<int>(RuleColumn::TimeoutMs),
            new QTableWidgetItem(QString::number(ruleModel.timeoutMs)));

        auto* timeoutDecisionCombo = new QComboBox(ruleTable);
        applyRuleComboStyle(timeoutDecisionCombo);
        for (const QPair<QString, quint32>& optionPair : decisionOptionList())
        {
            timeoutDecisionCombo->addItem(optionPair.first, optionPair.second);
        }
        const int timeoutDecisionIndex = timeoutDecisionCombo->findData(ruleModel.timeoutDefaultDecision);
        timeoutDecisionCombo->setCurrentIndex(timeoutDecisionIndex >= 0 ? timeoutDecisionIndex : 0);
        connect(timeoutDecisionCombo, &QComboBox::currentIndexChanged, m_hostPage, [this](int) {
            if (!m_ignoreUiSignal)
            {
                setDirtyState(true);
            }
        });
        ruleTable->setCellWidget(
            headerRow,
            static_cast<int>(RuleColumn::TimeoutDefaultDecision),
            timeoutDecisionCombo);

        ruleTable->setItem(
            headerRow,
            static_cast<int>(RuleColumn::Priority),
            new QTableWidgetItem(QString::number(ruleModel.priority)));

        ruleTable->setItem(detailRow, static_cast<int>(RuleColumn::GroupId), makeReadOnlyItem(QStringLiteral("发起程序匹配")));
        auto* initiatorEdit = new QLineEdit(ruleTable);
        initiatorEdit->setText(ruleModel.initiatorPattern);
        initiatorEdit->setPlaceholderText(initiatorPlaceholderByType(callbackType));
        applyRuleLineEditStyle(initiatorEdit);
        connect(initiatorEdit, &QLineEdit::textEdited, m_hostPage, [this](const QString&) {
            if (!m_ignoreUiSignal)
            {
                setDirtyState(true);
            }
        });
        ruleTable->setCellWidget(detailRow, static_cast<int>(RuleColumn::RuleName), initiatorEdit);

        ruleTable->setItem(detailRow, static_cast<int>(RuleColumn::OperationMask), makeReadOnlyItem(QStringLiteral("目标匹配")));
        auto* targetEdit = new QLineEdit(ruleTable);
        targetEdit->setText(ruleModel.targetPattern);
        targetEdit->setPlaceholderText(targetPlaceholderByType(callbackType));
        applyRuleLineEditStyle(targetEdit);
        connect(targetEdit, &QLineEdit::textEdited, m_hostPage, [this](const QString&) {
            if (!m_ignoreUiSignal)
            {
                setDirtyState(true);
            }
        });
        ruleTable->setCellWidget(detailRow, static_cast<int>(RuleColumn::MatchMode), targetEdit);

        ruleTable->setItem(detailRow, static_cast<int>(RuleColumn::Action), makeReadOnlyItem(QStringLiteral("备注")));
        ruleTable->setSpan(detailRow, static_cast<int>(RuleColumn::TimeoutMs), 1, 3);
        auto* commentEdit = new QLineEdit(ruleTable);
        commentEdit->setText(ruleModel.comment);
        commentEdit->setPlaceholderText(QStringLiteral("备注"));
        applyRuleLineEditStyle(commentEdit);
        connect(commentEdit, &QLineEdit::textEdited, m_hostPage, [this](const QString&) {
            if (!m_ignoreUiSignal)
            {
                setDirtyState(true);
            }
        });
        ruleTable->setCellWidget(detailRow, static_cast<int>(RuleColumn::TimeoutMs), commentEdit);

        refreshRuleGroupComboForCell(groupCombo, ruleModel.groupId);
    }

    void refreshRuleGroupComboForCell(QComboBox* groupCombo, const quint32 selectedGroupId)
    {
        if (groupCombo == nullptr)
        {
            return;
        }

        applyRuleComboStyle(groupCombo);
        groupCombo->clear();
        for (int rowIndex = 0; rowIndex < m_groupTable->rowCount(); ++rowIndex)
        {
            QTableWidgetItem* groupIdItem = m_groupTable->item(rowIndex, static_cast<int>(GroupColumn::Id));
            QTableWidgetItem* groupNameItem = m_groupTable->item(rowIndex, static_cast<int>(GroupColumn::Name));
            if (groupIdItem == nullptr || groupNameItem == nullptr)
            {
                continue;
            }

            quint32 groupId = 0U;
            if (!parseUnsignedText(groupIdItem->text(), &groupId))
            {
                continue;
            }
            groupCombo->addItem(
                QStringLiteral("[%1] %2").arg(groupId).arg(groupNameItem->text().trimmed()),
                groupId);
        }

        int targetIndex = groupCombo->findData(selectedGroupId);
        if (targetIndex < 0)
        {
            targetIndex = 0;
        }
        groupCombo->setCurrentIndex(targetIndex);
        connect(groupCombo, &QComboBox::currentIndexChanged, m_hostPage, [this](int) {
            if (!m_ignoreUiSignal)
            {
                setDirtyState(true);
            }
        });
    }

    void refreshRuleGroupComboOptions()
    {
        for (auto iterator = m_ruleTableMap.begin(); iterator != m_ruleTableMap.end(); ++iterator)
        {
            QTableWidget* ruleTable = iterator.value();
            if (ruleTable == nullptr)
            {
                continue;
            }

            for (int rowIndex = 0; rowIndex < ruleTable->rowCount(); rowIndex += 2)
            {
                auto* groupCombo = qobject_cast<QComboBox*>(
                    ruleTable->cellWidget(rowIndex, static_cast<int>(RuleColumn::GroupId)));
                quint32 selectedGroupId = firstGroupId();
                if (groupCombo != nullptr)
                {
                    selectedGroupId = static_cast<quint32>(groupCombo->currentData().toUInt());
                }
                refreshRuleGroupComboForCell(groupCombo, selectedGroupId);
            }
        }
    }

    quint32 currentRuleCallbackType() const
    {
        return m_tabCallbackTypeMap.value(
            m_ruleTabWidget != nullptr ? m_ruleTabWidget->currentIndex() : -1,
            KSWORD_ARK_CALLBACK_TYPE_NONE);
    }

    QTableWidget* currentRuleTable() const
    {
        const quint32 callbackType = currentRuleCallbackType();
        return m_ruleTableMap.value(callbackType, nullptr);
    }

    bool collectGroupsFromUi(
        QList<CallbackRuleGroupModel>* groupListOut,
        QString* errorTextOut) const
    {
        if (groupListOut == nullptr)
        {
            if (errorTextOut != nullptr)
            {
                *errorTextOut = QStringLiteral("内部错误：groupListOut 为空。");
            }
            return false;
        }

        groupListOut->clear();
        for (int rowIndex = 0; rowIndex < m_groupTable->rowCount(); ++rowIndex)
        {
            QTableWidgetItem* idItem = m_groupTable->item(rowIndex, static_cast<int>(GroupColumn::Id));
            QTableWidgetItem* nameItem = m_groupTable->item(rowIndex, static_cast<int>(GroupColumn::Name));
            QTableWidgetItem* enabledItem = m_groupTable->item(rowIndex, static_cast<int>(GroupColumn::Enabled));
            QTableWidgetItem* priorityItem = m_groupTable->item(rowIndex, static_cast<int>(GroupColumn::Priority));
            QTableWidgetItem* commentItem = m_groupTable->item(rowIndex, static_cast<int>(GroupColumn::Comment));
            if (idItem == nullptr || nameItem == nullptr || enabledItem == nullptr || priorityItem == nullptr || commentItem == nullptr)
            {
                continue;
            }

            CallbackRuleGroupModel groupModel;
            if (!parseUnsignedText(idItem->text(), &groupModel.groupId))
            {
                if (errorTextOut != nullptr)
                {
                    *errorTextOut = QStringLiteral("规则组行 %1 的 groupId 非法。").arg(rowIndex + 1);
                }
                return false;
            }

            bool priorityOk = false;
            groupModel.priority = priorityItem->text().trimmed().toInt(&priorityOk);
            if (!priorityOk)
            {
                if (errorTextOut != nullptr)
                {
                    *errorTextOut = QStringLiteral("规则组行 %1 的优先级非法。").arg(rowIndex + 1);
                }
                return false;
            }

            groupModel.groupName = nameItem->text().trimmed();
            groupModel.enabled = (enabledItem->checkState() == Qt::Checked);
            groupModel.comment = commentItem->text().trimmed();
            groupListOut->push_back(groupModel);
        }

        return true;
    }

    bool collectRuleListFromTable(
        QTableWidget* ruleTable,
        const quint32 callbackType,
        QList<CallbackRuleModel>* ruleListOut,
        QString* errorTextOut) const
    {
        if (ruleTable == nullptr || ruleListOut == nullptr)
        {
            if (errorTextOut != nullptr)
            {
                *errorTextOut = QStringLiteral("内部错误：ruleTable 或 ruleListOut 为空。");
            }
            return false;
        }

        ruleListOut->clear();
        const int totalRuleCount = ruleCountOfTable(ruleTable);
        for (int logicalRuleIndex = 0; logicalRuleIndex < totalRuleCount; ++logicalRuleIndex)
        {
            const int headerRow = logicalRuleIndex * 2;
            const int detailRow = headerRow + 1;

            QTableWidgetItem* ruleIdItem = ruleTable->item(headerRow, static_cast<int>(RuleColumn::RuleId));
            QTableWidgetItem* ruleNameItem = ruleTable->item(headerRow, static_cast<int>(RuleColumn::RuleName));
            QTableWidgetItem* enabledItem = ruleTable->item(headerRow, static_cast<int>(RuleColumn::Enabled));
            QTableWidgetItem* timeoutItem = ruleTable->item(headerRow, static_cast<int>(RuleColumn::TimeoutMs));
            QTableWidgetItem* priorityItem = ruleTable->item(headerRow, static_cast<int>(RuleColumn::Priority));
            auto* groupCombo = qobject_cast<QComboBox*>(ruleTable->cellWidget(headerRow, static_cast<int>(RuleColumn::GroupId)));
            auto* operationCombo = qobject_cast<QComboBox*>(ruleTable->cellWidget(headerRow, static_cast<int>(RuleColumn::OperationMask)));
            auto* matchModeCombo = qobject_cast<QComboBox*>(ruleTable->cellWidget(headerRow, static_cast<int>(RuleColumn::MatchMode)));
            auto* actionCombo = qobject_cast<QComboBox*>(ruleTable->cellWidget(headerRow, static_cast<int>(RuleColumn::Action)));
            auto* timeoutDecisionCombo = qobject_cast<QComboBox*>(ruleTable->cellWidget(headerRow, static_cast<int>(RuleColumn::TimeoutDefaultDecision)));
            auto* initiatorEdit = qobject_cast<QLineEdit*>(ruleTable->cellWidget(detailRow, static_cast<int>(RuleColumn::RuleName)));
            auto* targetEdit = qobject_cast<QLineEdit*>(ruleTable->cellWidget(detailRow, static_cast<int>(RuleColumn::MatchMode)));
            auto* commentEdit = qobject_cast<QLineEdit*>(ruleTable->cellWidget(detailRow, static_cast<int>(RuleColumn::TimeoutMs)));

            if (ruleIdItem == nullptr || ruleNameItem == nullptr || enabledItem == nullptr ||
                timeoutItem == nullptr || priorityItem == nullptr ||
                groupCombo == nullptr || operationCombo == nullptr ||
                matchModeCombo == nullptr || actionCombo == nullptr || timeoutDecisionCombo == nullptr ||
                initiatorEdit == nullptr || targetEdit == nullptr || commentEdit == nullptr)
            {
                continue;
            }

            CallbackRuleModel ruleModel;
            if (!parseUnsignedText(ruleIdItem->text(), &ruleModel.ruleId))
            {
                if (errorTextOut != nullptr)
                {
                    *errorTextOut = QStringLiteral("规则 %1 的 ruleId 非法。").arg(logicalRuleIndex + 1);
                }
                return false;
            }

            ruleModel.groupId = static_cast<quint32>(groupCombo->currentData().toUInt());
            if (ruleModel.groupId == 0U)
            {
                if (errorTextOut != nullptr)
                {
                    *errorTextOut = QStringLiteral("规则 %1 未选择有效规则组。").arg(logicalRuleIndex + 1);
                }
                return false;
            }

            bool operationParseOk = false;
            ruleModel.operationMask = currentOperationMaskFromCombo(operationCombo, &operationParseOk);
            if (!operationParseOk)
            {
                if (errorTextOut != nullptr)
                {
                    *errorTextOut = QStringLiteral("规则 %1 的 operationMask 非法。").arg(logicalRuleIndex + 1);
                }
                return false;
            }

            bool timeoutOk = false;
            const quint32 timeoutMs = static_cast<quint32>(timeoutItem->text().trimmed().toUInt(&timeoutOk));
            if (!timeoutOk)
            {
                if (errorTextOut != nullptr)
                {
                    *errorTextOut = QStringLiteral("规则 %1 的 timeoutMs 非法。").arg(logicalRuleIndex + 1);
                }
                return false;
            }

            bool priorityOk = false;
            const qint32 rulePriority = priorityItem->text().trimmed().toInt(&priorityOk);
            if (!priorityOk)
            {
                if (errorTextOut != nullptr)
                {
                    *errorTextOut = QStringLiteral("规则 %1 的优先级非法。").arg(logicalRuleIndex + 1);
                }
                return false;
            }

            ruleModel.ruleName = ruleNameItem->text().trimmed();
            ruleModel.enabled = (enabledItem->checkState() == Qt::Checked);
            ruleModel.callbackType = callbackType;
            ruleModel.initiatorPattern = normalizeMatchAllPattern(initiatorEdit->text());
            ruleModel.targetPattern = normalizeMatchAllPattern(targetEdit->text());
            ruleModel.matchMode = static_cast<quint32>(matchModeCombo->currentData().toUInt());
            ruleModel.action = static_cast<quint32>(actionCombo->currentData().toUInt());
            ruleModel.timeoutMs = timeoutMs;
            ruleModel.timeoutDefaultDecision = static_cast<quint32>(timeoutDecisionCombo->currentData().toUInt());
            ruleModel.priority = rulePriority;
            ruleModel.comment = commentEdit->text().trimmed();

            if (!ruleModel.initiatorPattern.trimmed().isEmpty())
            {
                // 所有回调类型的 initiator 最终都与内核采集到的进程镜像路径比较，
                // 这里统一把常见用户态路径（如 C:\...）转换到内核可匹配形式。
                ruleModel.initiatorPattern =
                    normalizeUserModeFilePathPatternForKernel(ruleModel.initiatorPattern);
            }

            if (ruleModel.callbackType == KSWORD_ARK_CALLBACK_TYPE_REGISTRY)
            {
                ruleModel.targetPattern = normalizeRegistryTargetPatternForKernel(ruleModel.targetPattern);
            }
            else if (!ruleModel.targetPattern.trimmed().isEmpty())
            {
                ruleModel.targetPattern =
                    normalizeUserModeFilePathPatternForKernel(ruleModel.targetPattern);
            }

            if (ruleModel.action != KSWORD_ARK_RULE_ACTION_ASK_USER)
            {
                ruleModel.timeoutMs = 0U;
            }

            ruleListOut->push_back(ruleModel);
        }

        return true;
    }

    bool collectAllRulesFromUi(
        QList<CallbackRuleModel>* ruleListOut,
        QString* errorTextOut) const
    {
        if (ruleListOut == nullptr)
        {
            if (errorTextOut != nullptr)
            {
                *errorTextOut = QStringLiteral("内部错误：ruleListOut 为空。");
            }
            return false;
        }

        ruleListOut->clear();
        for (auto iterator = m_ruleTableMap.begin(); iterator != m_ruleTableMap.end(); ++iterator)
        {
            const quint32 callbackType = iterator.key();
            QTableWidget* ruleTable = iterator.value();
            QList<CallbackRuleModel> typeRuleList;
            if (!collectRuleListFromTable(ruleTable, callbackType, &typeRuleList, errorTextOut))
            {
                return false;
            }
            ruleListOut->append(typeRuleList);
        }

        return true;
    }

    bool collectConfigFromUi(
        CallbackConfigDocument* configOut,
        QString* errorTextOut)
    {
        if (configOut == nullptr)
        {
            if (errorTextOut != nullptr)
            {
                *errorTextOut = QStringLiteral("内部错误：configOut 为空。");
            }
            return false;
        }

        CallbackConfigDocument configDocument;
        configDocument.schemaVersion = KSWORD_ARK_CALLBACK_RULE_SCHEMA_VERSION;
        configDocument.exportedAtUtc = QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs);
        configDocument.appVersion = QStringLiteral("Ksword5.1");
        configDocument.globalEnabled = (m_globalEnabledCheck != nullptr) ? m_globalEnabledCheck->isChecked() : true;
        configDocument.ruleVersion = m_nextRuleVersion;

        if (!collectGroupsFromUi(&configDocument.groups, errorTextOut))
        {
            return false;
        }
        if (!collectAllRulesFromUi(&configDocument.rules, errorTextOut))
        {
            return false;
        }

        const CallbackValidationResult validationResult = validateCallbackConfig(configDocument);
        if (!validationResult.success)
        {
            if (errorTextOut != nullptr)
            {
                *errorTextOut = validationResult.errorList.join(QStringLiteral("；"));
            }
            return false;
        }

        for (const QString& warningText : validationResult.warningList)
        {
            appendAppLog(QStringLiteral("配置警告：%1").arg(warningText));
        }

        *configOut = configDocument;
        return true;
    }

    void updateStatusLabel()
    {
        if (m_statusLabel == nullptr)
        {
            return;
        }

        const QString statusText = QStringLiteral(
            "状态：%1 | 驱动%2 | 规则版本=%3 | 规则数=%4 | 等待接收者=%5 | 待决策=%6 | 生效时间=%7 | 未应用修改=%8")
            .arg(m_rulesApplied ? QStringLiteral("已应用") : QStringLiteral("未应用"))
            .arg(m_runtimeState.driverOnline != 0U ? QStringLiteral("在线") : QStringLiteral("离线"))
            .arg(m_runtimeState.appliedRuleVersion)
            .arg(m_runtimeState.ruleCount)
            .arg(m_runtimeState.waitingReceiverCount)
            .arg(m_runtimeState.pendingDecisionCount)
            .arg(utc100nsToDisplayText(m_runtimeState.appliedAtUtc100ns))
            .arg(m_dirty ? QStringLiteral("是") : QStringLiteral("否"));
        m_statusLabel->setText(statusText);
        m_statusLabel->setStyleSheet(
            QStringLiteral("color:%1;font-weight:600;")
            .arg(m_runtimeState.driverOnline != 0U
                ? QStringLiteral("#3A8F3A")
                : KswordTheme::WarningAccentColor().name()));
    }

    bool queryRuntimeState(KSWORD_ARK_CALLBACK_RUNTIME_STATE* runtimeStateOut, QString* errorTextOut) const
    {
        if (runtimeStateOut == nullptr)
        {
            if (errorTextOut != nullptr)
            {
                *errorTextOut = QStringLiteral("内部错误：runtimeStateOut 为空。");
            }
            return false;
        }

        HANDLE driverHandle = ::CreateFileW(
            KSWORD_ARK_LOG_WIN32_PATH,
            GENERIC_READ | GENERIC_WRITE,
            FILE_SHARE_READ | FILE_SHARE_WRITE,
            nullptr,
            OPEN_EXISTING,
            FILE_ATTRIBUTE_NORMAL,
            nullptr);
        if (driverHandle == INVALID_HANDLE_VALUE)
        {
            if (errorTextOut != nullptr)
            {
                *errorTextOut = QStringLiteral("连接驱动失败，error=%1").arg(::GetLastError());
            }
            return false;
        }

        KSWORD_ARK_CALLBACK_RUNTIME_STATE runtimeState{};
        DWORD bytesReturned = 0;
        const BOOL ioctlOk = ::DeviceIoControl(
            driverHandle,
            IOCTL_KSWORD_ARK_GET_CALLBACK_RUNTIME_STATE,
            nullptr,
            0,
            &runtimeState,
            static_cast<DWORD>(sizeof(runtimeState)),
            &bytesReturned,
            nullptr);
        ::CloseHandle(driverHandle);

        if (ioctlOk == FALSE || bytesReturned < sizeof(runtimeState))
        {
            if (errorTextOut != nullptr)
            {
                *errorTextOut = QStringLiteral("获取驱动状态失败，error=%1").arg(::GetLastError());
            }
            return false;
        }

        *runtimeStateOut = runtimeState;
        return true;
    }

    void reloadRuntimeState()
    {
        KSWORD_ARK_CALLBACK_RUNTIME_STATE runtimeState{};
        QString errorText;
        if (!queryRuntimeState(&runtimeState, &errorText))
        {
            RtlZeroMemory(&m_runtimeState, sizeof(m_runtimeState));
            appendAppLog(QStringLiteral("重新加载驱动状态失败：%1").arg(errorText));
            updateStatusLabel();
            return;
        }

        m_runtimeState = runtimeState;
        m_rulesApplied = (m_runtimeState.rulesApplied != 0U);
        if (m_runtimeState.appliedRuleVersion >= m_nextRuleVersion)
        {
            m_nextRuleVersion = m_runtimeState.appliedRuleVersion + 1ULL;
        }

        appendAppLog(
            QStringLiteral("驱动状态已刷新：online=%1, groups=%2, rules=%3, pending=%4, waiting=%5")
            .arg(m_runtimeState.driverOnline)
            .arg(m_runtimeState.groupCount)
            .arg(m_runtimeState.ruleCount)
            .arg(m_runtimeState.pendingDecisionCount)
            .arg(m_runtimeState.waitingReceiverCount));
        updateStatusLabel();
    }

    void applyRulesToDriver()
    {
        CallbackConfigDocument configDocument;
        QString errorText;
        if (!collectConfigFromUi(&configDocument, &errorText))
        {
            QMessageBox::warning(m_hostPage, QStringLiteral("驱动回调"), QStringLiteral("应用失败：%1").arg(errorText));
            appendAppLog(QStringLiteral("应用失败：%1").arg(errorText));
            return;
        }

        QByteArray blobBytes;
        if (!buildCallbackRuleBlobFromConfig(configDocument, &blobBytes, &errorText))
        {
            QMessageBox::warning(m_hostPage, QStringLiteral("驱动回调"), QStringLiteral("规则编译失败：%1").arg(errorText));
            appendAppLog(QStringLiteral("规则编译失败：%1").arg(errorText));
            return;
        }

        HANDLE driverHandle = ::CreateFileW(
            KSWORD_ARK_LOG_WIN32_PATH,
            GENERIC_READ | GENERIC_WRITE,
            FILE_SHARE_READ | FILE_SHARE_WRITE,
            nullptr,
            OPEN_EXISTING,
            FILE_ATTRIBUTE_NORMAL,
            nullptr);
        if (driverHandle == INVALID_HANDLE_VALUE)
        {
            const DWORD lastError = ::GetLastError();
            QMessageBox::warning(
                m_hostPage,
                QStringLiteral("驱动回调"),
                QStringLiteral("连接驱动失败，error=%1。").arg(lastError));
            appendAppLog(QStringLiteral("连接驱动失败，error=%1").arg(lastError));
            return;
        }

        DWORD bytesReturned = 0;
        const BOOL ioctlOk = ::DeviceIoControl(
            driverHandle,
            IOCTL_KSWORD_ARK_SET_CALLBACK_RULES,
            blobBytes.data(),
            static_cast<DWORD>(blobBytes.size()),
            nullptr,
            0,
            &bytesReturned,
            nullptr);
        const DWORD ioctlError = ioctlOk ? ERROR_SUCCESS : ::GetLastError();
        ::CloseHandle(driverHandle);

        if (ioctlOk == FALSE)
        {
            QMessageBox::warning(
                m_hostPage,
                QStringLiteral("驱动回调"),
                QStringLiteral("应用到驱动失败，error=%1。").arg(ioctlError));
            appendAppLog(QStringLiteral("应用到驱动失败，error=%1").arg(ioctlError));
            return;
        }

        m_rulesApplied = true;
        m_dirty = false;
        m_nextRuleVersion = configDocument.ruleVersion + 1ULL;

        appendAppLog(
            QStringLiteral("应用成功：ruleVersion=%1, groupCount=%2, ruleCount=%3, blobBytes=%4")
            .arg(configDocument.ruleVersion)
            .arg(configDocument.groups.size())
            .arg(configDocument.rules.size())
            .arg(blobBytes.size()));

        reloadRuntimeState();
        const bool hasAskUserRule = std::any_of(
            configDocument.rules.cbegin(),
            configDocument.rules.cend(),
            [](const CallbackRuleModel& ruleModel) {
                return ruleModel.enabled &&
                    ruleModel.callbackType == KSWORD_ARK_CALLBACK_TYPE_REGISTRY &&
                    ruleModel.action == KSWORD_ARK_RULE_ACTION_ASK_USER;
            });
        if (hasAskUserRule && m_runtimeState.waitingReceiverCount == 0U)
        {
            appendAppLog(
                QStringLiteral("警告：检测到“询问用户”规则，但当前等待接收者为 0。")
                + QStringLiteral("请确认弹窗管理器已启动，否则驱动将按默认决策回退。"));
        }
        updateStatusLabel();
    }

    void importConfigFromFile()
    {
        const QString filePath = QFileDialog::getOpenFileName(
            m_hostPage,
            QStringLiteral("导入回调规则"),
            QString(),
            QStringLiteral("Ksword Rule File (*.kswrules);;JSON (*.json);;All Files (*)"));
        if (filePath.trimmed().isEmpty())
        {
            return;
        }

        QFile inputFile(filePath);
        if (!inputFile.open(QIODevice::ReadOnly))
        {
            const QString errorText = QStringLiteral("打开文件失败：%1").arg(inputFile.errorString());
            QMessageBox::warning(m_hostPage, QStringLiteral("驱动回调"), errorText);
            appendAppLog(QStringLiteral("导入失败：%1").arg(errorText));
            return;
        }

        const QByteArray jsonBytes = inputFile.readAll();
        inputFile.close();

        CallbackConfigDocument importedDocument;
        QStringList warningList;
        QString errorText;
        if (!importCallbackConfigFromJson(jsonBytes, &importedDocument, &warningList, &errorText))
        {
            QMessageBox::warning(m_hostPage, QStringLiteral("驱动回调"), errorText);
            appendAppLog(QStringLiteral("导入失败：%1").arg(errorText));
            return;
        }

        const CallbackValidationResult validationResult = validateCallbackConfig(importedDocument);
        if (!validationResult.success)
        {
            const QString validateError = validationResult.errorList.join(QStringLiteral("；"));
            QMessageBox::warning(m_hostPage, QStringLiteral("驱动回调"), QStringLiteral("导入配置不合法：%1").arg(validateError));
            appendAppLog(QStringLiteral("导入配置不合法：%1").arg(validateError));
            return;
        }

        for (const QString& warningText : warningList)
        {
            appendAppLog(QStringLiteral("导入警告：%1").arg(warningText));
        }
        for (const QString& warningText : validationResult.warningList)
        {
            appendAppLog(QStringLiteral("配置警告：%1").arg(warningText));
        }

        populateUiFromConfig(importedDocument);
        m_nextRuleVersion = std::max(m_nextRuleVersion, importedDocument.ruleVersion + 1ULL);
        setDirtyState(true);
        appendAppLog(QStringLiteral("导入成功：%1").arg(filePath));
    }

    void exportConfigToFile()
    {
        CallbackConfigDocument configDocument;
        QString errorText;
        if (!collectConfigFromUi(&configDocument, &errorText))
        {
            QMessageBox::warning(m_hostPage, QStringLiteral("驱动回调"), QStringLiteral("导出失败：%1").arg(errorText));
            appendAppLog(QStringLiteral("导出失败：%1").arg(errorText));
            return;
        }

        const QString filePath = QFileDialog::getSaveFileName(
            m_hostPage,
            QStringLiteral("导出回调规则"),
            QStringLiteral("callback_rules.kswrules"),
            QStringLiteral("Ksword Rule File (*.kswrules);;JSON (*.json);;All Files (*)"));
        if (filePath.trimmed().isEmpty())
        {
            return;
        }

        QByteArray jsonBytes;
        if (!exportCallbackConfigToJson(configDocument, &jsonBytes, &errorText))
        {
            QMessageBox::warning(m_hostPage, QStringLiteral("驱动回调"), QStringLiteral("导出失败：%1").arg(errorText));
            appendAppLog(QStringLiteral("导出失败：%1").arg(errorText));
            return;
        }

        QFile outputFile(filePath);
        if (!outputFile.open(QIODevice::WriteOnly | QIODevice::Truncate))
        {
            const QString ioError = QStringLiteral("写入失败：%1").arg(outputFile.errorString());
            QMessageBox::warning(m_hostPage, QStringLiteral("驱动回调"), ioError);
            appendAppLog(QStringLiteral("导出失败：%1").arg(ioError));
            return;
        }
        outputFile.write(jsonBytes);
        outputFile.close();

        appendAppLog(QStringLiteral("导出成功：%1").arg(filePath));
    }

    void populateUiFromConfig(const CallbackConfigDocument& configDocument)
    {
        m_ignoreUiSignal = true;

        if (m_globalEnabledCheck != nullptr)
        {
            m_globalEnabledCheck->setChecked(configDocument.globalEnabled);
        }

        m_groupTable->setRowCount(0);
        QList<CallbackRuleGroupModel> sortedGroups = configDocument.groups;
        std::sort(sortedGroups.begin(), sortedGroups.end(), [](const CallbackRuleGroupModel& left, const CallbackRuleGroupModel& right) {
            if (left.priority != right.priority)
            {
                return left.priority < right.priority;
            }
            return left.groupId < right.groupId;
        });
        for (const CallbackRuleGroupModel& groupModel : sortedGroups)
        {
            appendGroupRow(groupModel);
        }
        addDefaultGroupIfNeeded();

        for (auto iterator = m_ruleTableMap.begin(); iterator != m_ruleTableMap.end(); ++iterator)
        {
            QTableWidget* ruleTable = iterator.value();
            if (ruleTable != nullptr)
            {
                ruleTable->setRowCount(0);
            }
        }

        QList<CallbackRuleModel> sortedRules = configDocument.rules;
        std::sort(sortedRules.begin(), sortedRules.end(), [](const CallbackRuleModel& left, const CallbackRuleModel& right) {
            if (left.callbackType != right.callbackType)
            {
                return left.callbackType < right.callbackType;
            }
            if (left.priority != right.priority)
            {
                return left.priority < right.priority;
            }
            return left.ruleId < right.ruleId;
        });
        for (const CallbackRuleModel& ruleModel : sortedRules)
        {
            QTableWidget* ruleTable = m_ruleTableMap.value(ruleModel.callbackType, nullptr);
            if (ruleTable == nullptr)
            {
                continue;
            }
            appendRuleRow(ruleTable, ruleModel.callbackType, ruleModel);
        }

        m_ignoreUiSignal = false;
        refreshRuleGroupComboOptions();
    }

private:
    QWidget* m_hostPage = nullptr;
    QPointer<CallbackPromptManager> m_promptManager;

    QCheckBox* m_globalEnabledCheck = nullptr;
    QPushButton* m_applyButton = nullptr;
    QPushButton* m_reloadStateButton = nullptr;
    QPushButton* m_importButton = nullptr;
    QPushButton* m_exportButton = nullptr;
    QLabel* m_statusLabel = nullptr;

    QPushButton* m_addGroupButton = nullptr;
    QPushButton* m_removeGroupButton = nullptr;
    QPushButton* m_renameGroupButton = nullptr;
    QPushButton* m_moveGroupUpButton = nullptr;
    QPushButton* m_moveGroupDownButton = nullptr;
    QTableWidget* m_groupTable = nullptr;

    QPushButton* m_addRuleButton = nullptr;
    QPushButton* m_removeRuleButton = nullptr;
    QPushButton* m_moveRuleUpButton = nullptr;
    QPushButton* m_moveRuleDownButton = nullptr;
    QTabWidget* m_ruleTabWidget = nullptr;
    QHash<quint32, QTableWidget*> m_ruleTableMap;
    QHash<int, quint32> m_tabCallbackTypeMap;

    QPlainTextEdit* m_appLogEditor = nullptr;
    QPlainTextEdit* m_eventLogEditor = nullptr;

    KSWORD_ARK_CALLBACK_RUNTIME_STATE m_runtimeState{};
    quint64 m_nextRuleVersion = 1ULL;
    bool m_rulesApplied = false;
    bool m_dirty = false;
    bool m_ignoreUiSignal = false;
};

void KernelDock::initializeCallbackInterceptTab()
{
    if (m_callbackInterceptPage == nullptr || m_callbackInterceptController != nullptr)
    {
        return;
    }

    m_callbackInterceptController = new CallbackInterceptController(m_callbackInterceptPage, this);
}
