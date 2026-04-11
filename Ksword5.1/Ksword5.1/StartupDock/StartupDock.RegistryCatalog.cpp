#include "StartupDock.Internal.h"

#include <QFile>
#include <QRegularExpression>
#include <QSet>
#include <QTextStream>

namespace startup_dock_detail
{
    namespace
    {
        // ReplaceRule：逐条修正原始清单中的高频拼写/分隔错误。
        struct ReplaceRule
        {
            const char* fromText = ""; // fromText：待替换原文。
            const char* toText = "";   // toText：替换目标文本。
        };

        // applyCaseInsensitiveReplace 作用：
        // - 对单条规则执行不区分大小写替换；
        // - 传入空字符串时直接跳过，避免误改。
        void applyCaseInsensitiveReplace(
            QString* textInOut,
            const QString& fromText,
            const QString& toText)
        {
            if (textInOut == nullptr || fromText.trimmed().isEmpty())
            {
                return;
            }
            textInOut->replace(fromText, toText, Qt::CaseInsensitive);
        }

        // normalizeRegistryLocationLine 作用：
        // - 读取一行原始清单文本并修正常见错误；
        // - 仅返回合法 HKLM/HKCU/HKCR 路径，其他标题/说明行返回空串。
        QString normalizeRegistryLocationLine(const QString& rawLineText)
        {
            QString normalizedText = rawLineText.trimmed(); // normalizedText：当前行的可变标准化文本。
            if (normalizedText.isEmpty())
            {
                return QString();
            }

            const QString upperRawText = normalizedText.toUpper(); // upperRawText：用于快速判定是否是注册表路径。
            if (!upperRawText.startsWith(QStringLiteral("HKLM"))
                && !upperRawText.startsWith(QStringLiteral("HKCU"))
                && !upperRawText.startsWith(QStringLiteral("HKCR")))
            {
                return QString();
            }

            normalizedText.replace(QLatin1Char('/'), QLatin1Char('\\'));
            normalizedText.replace(
                QRegularExpression(QStringLiteral("\\s*\\\\\\s*")),
                QStringLiteral("\\"));
            normalizedText.replace(
                QRegularExpression(QStringLiteral("\\\\{2,}")),
                QStringLiteral("\\"));

            static const std::array<ReplaceRule, 28> replaceRuleList{ {
                { "HKLMSOFTWARE", "HKLM\\SOFTWARE" },
                { "HKLMSoftware", "HKLM\\Software" },
                { "HKLMSystem", "HKLM\\System" },
                { "HKLMSYSTEM", "HKLM\\SYSTEM" },
                { "HKCU\\SOFTWAREClasses", "HKCU\\SOFTWARE\\Classes" },
                { "HKCU\\SOFTWARE Classes", "HKCU\\SOFTWARE\\Classes" },
                { "HKLM\\SOFTWAREWow6432Node", "HKLM\\SOFTWARE\\Wow6432Node" },
                { "SOFTWAREClasses", "SOFTWARE\\Classes" },
                { "SOFTWARE Classes", "SOFTWARE\\Classes" },
                { "ShelllconOverlayldentifiers", "ShellIconOverlayIdentifiers" },
                { "Catalog Entries64", "Catalog_Entries64" },
                { "Folder ShellEx", "Folder\\ShellEx" },
                { "Explorer ShellExecuteHooks", "Explorer\\ShellExecuteHooks" },
                { "Explorer ShellServiceObjects", "Explorer\\ShellServiceObjects" },
                { "ShellExecute Hooks", "ShellExecuteHooks" },
                { "Internet ExplorerExtensions", "Internet Explorer\\Extensions" },
                { "Intemet", "Internet" },
                { "Interet", "Internet" },
                { "Userlnit", "Userinit" },
                { "Scmsave.exe", "Scrnsave.exe" },
                { "AutoStartDisconnect", "AutoStartOnDisconnect" },
                { "Appinit Dlls", "AppInit_DLLs" },
                { "Appinit_Dlls", "AppInit_DLLs" },
                { "Session Manager\\SOInitialCommand", "Session Manager\\S0InitialCommand" },
                { "HKCU\\Software\\Classes\\M\\ShellEx\\ContextMenuHandlers", "HKCU\\Software\\Classes\\*\\ShellEx\\ContextMenuHandlers" },
                { "HKCU\\Software\\Classes\\\\ShellEx\\PropertySheetHandlers", "HKCU\\Software\\Classes\\*\\ShellEx\\PropertySheetHandlers" },
                { "HKLM\\Software\\Classes\\\\ShellEx\\PropertySheetHandlers", "HKLM\\Software\\Classes\\*\\ShellEx\\PropertySheetHandlers" },
                { "HKLM\\Software\\Wow6432Node\\Classes\\\\ShellEx\\PropertySheetHandlers", "HKLM\\Software\\Wow6432Node\\Classes\\*\\ShellEx\\PropertySheetHandlers" }
            } };

            for (const ReplaceRule& replaceRule : replaceRuleList)
            {
                const QString fromText = QString::fromUtf8(replaceRule.fromText); // fromText：单条替换规则原文。
                const QString toText = QString::fromUtf8(replaceRule.toText);     // toText：单条替换规则目标文本。
                applyCaseInsensitiveReplace(&normalizedText, fromText, toText);
            }

            const QRegularExpression rootFixRegex(
                QStringLiteral("^(HKLM|HKCU|HKCR)\\s*(SOFTWARE|SYSTEM|Software|System|Classes|Environment|Control Panel)"),
                QRegularExpression::CaseInsensitiveOption);
            normalizedText.replace(rootFixRegex, QStringLiteral("\\1\\\\\\2"));
            normalizedText.replace(
                QRegularExpression(QStringLiteral("\\\\CLSID\\\\\\{?\\(?([0-9A-Fa-f\\-]{36})\\)?\\}?\\\\")),
                QStringLiteral("\\\\CLSID\\\\{\\1}\\\\"));
            normalizedText.replace(
                QRegularExpression(QStringLiteral("\\s*\\\\\\s*")),
                QStringLiteral("\\"));
            normalizedText.replace(
                QRegularExpression(QStringLiteral("\\\\{2,}")),
                QStringLiteral("\\"));

            if (normalizedText.startsWith(QStringLiteral("HKLM"), Qt::CaseInsensitive)
                || normalizedText.startsWith(QStringLiteral("HKCU"), Qt::CaseInsensitive)
                || normalizedText.startsWith(QStringLiteral("HKCR"), Qt::CaseInsensitive))
            {
                if (normalizedText.size() > 4 && normalizedText.at(4) != QLatin1Char('\\'))
                {
                    normalizedText.insert(4, QLatin1Char('\\'));
                }
            }

            normalizedText = normalizedText.trimmed();
            if (!normalizedText.startsWith(QStringLiteral("HKLM\\"), Qt::CaseInsensitive)
                && !normalizedText.startsWith(QStringLiteral("HKCU\\"), Qt::CaseInsensitive)
                && !normalizedText.startsWith(QStringLiteral("HKCR\\"), Qt::CaseInsensitive))
            {
                return QString();
            }

            normalizedText.replace(0, 4, normalizedText.left(4).toUpper());
            normalizedText.replace(QStringLiteral("}\\)\\InProcServer32"), QStringLiteral("}\\InProcServer32"), Qt::CaseInsensitive);
            normalizedText.replace(QStringLiteral("}\\)\\Instance"), QStringLiteral("}\\Instance"), Qt::CaseInsensitive);
            if (normalizedText.endsWith(QStringLiteral("\\(Default)"), Qt::CaseInsensitive))
            {
                normalizedText.chop(QStringLiteral("\\(Default)").size());
            }

            return normalizedText;
        }

        // loadRegistryLocationCatalog 作用：
        // - 从资源文件加载并标准化启动项注册表位置清单；
        // - 统一去重后按原始顺序返回，供注册表树创建“已知路径节点”。
        QStringList loadRegistryLocationCatalog()
        {
            QStringList locationList;                 // locationList：最终用于注册表树的路径列表。
            QSet<QString> locationKeySet;            // locationKeySet：去重集合（使用小写键）。
            QFile catalogFile(QStringLiteral(":/Data/startup_registry_locations.txt")); // catalogFile：内置资源清单文件对象。
            if (!catalogFile.open(QIODevice::ReadOnly | QIODevice::Text))
            {
                return locationList;
            }

            QTextStream catalogStream(&catalogFile); // catalogStream：按行读取清单文本。
            catalogStream.setEncoding(QStringConverter::Utf8);
            while (!catalogStream.atEnd())
            {
                const QString rawLineText = catalogStream.readLine();              // rawLineText：资源中的原始单行文本。
                const QString normalizedText = normalizeRegistryLocationLine(rawLineText); // normalizedText：修正并校验后的路径。
                if (normalizedText.isEmpty())
                {
                    continue;
                }

                const QString dedupeKeyText = normalizedText.toLower(); // dedupeKeyText：不区分大小写的去重键。
                if (locationKeySet.contains(dedupeKeyText))
                {
                    continue;
                }

                locationKeySet.insert(dedupeKeyText);
                locationList.push_back(normalizedText);
            }

            return locationList;
        }
    }

    QStringList buildKnownStartupRegistryLocationList()
    {
        static const QStringList cachedLocationList = loadRegistryLocationCatalog(); // cachedLocationList：静态缓存，避免重复解析资源文件。
        return cachedLocationList;
    }
}
