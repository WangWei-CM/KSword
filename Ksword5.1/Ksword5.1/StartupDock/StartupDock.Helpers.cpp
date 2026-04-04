#include "StartupDock.Internal.h"

#include "../theme.h"

#include <QPainter>
#include <QPixmap>
#include <QSvgRenderer>

#include <wintrust.h>
#include <Softpub.h>
#include <winver.h>

#pragma comment(lib, "Wintrust.lib")
#pragma comment(lib, "Version.lib")

namespace startup_dock_detail
{
    namespace
    {
        // queryCompanyNameByVersion 作用：
        // - 从文件版本信息读取 CompanyName；
        // - 作为发布者显示的快速兜底来源。
        QString queryCompanyNameByVersion(const QString& filePathText)
        {
            if (filePathText.trimmed().isEmpty())
            {
                return QString();
            }

            const std::wstring utf16Path = filePathText.toStdWString();
            DWORD handleValue = 0;
            const DWORD versionInfoBytes = ::GetFileVersionInfoSizeW(utf16Path.c_str(), &handleValue);
            if (versionInfoBytes == 0)
            {
                return QString();
            }

            std::vector<std::uint8_t> versionBuffer(versionInfoBytes);
            if (::GetFileVersionInfoW(
                utf16Path.c_str(),
                0,
                versionInfoBytes,
                versionBuffer.data()) == FALSE)
            {
                return QString();
            }

            struct LangAndCodePage
            {
                WORD language = 0;
                WORD codePage = 0;
            };

            LangAndCodePage* translationPointer = nullptr;
            UINT translationBytes = 0;
            if (::VerQueryValueW(
                versionBuffer.data(),
                L"\\VarFileInfo\\Translation",
                reinterpret_cast<LPVOID*>(&translationPointer),
                &translationBytes) == FALSE
                || translationPointer == nullptr
                || translationBytes < sizeof(LangAndCodePage))
            {
                return QString();
            }

            wchar_t queryPathBuffer[64] = {};
            _snwprintf_s(
                queryPathBuffer,
                _countof(queryPathBuffer),
                _TRUNCATE,
                L"\\StringFileInfo\\%04x%04x\\CompanyName",
                translationPointer[0].language,
                translationPointer[0].codePage);

            wchar_t* companyNamePointer = nullptr;
            UINT companyNameChars = 0;
            if (::VerQueryValueW(
                versionBuffer.data(),
                queryPathBuffer,
                reinterpret_cast<LPVOID*>(&companyNamePointer),
                &companyNameChars) == FALSE
                || companyNamePointer == nullptr
                || companyNameChars <= 1)
            {
                return QString();
            }

            return QString::fromWCharArray(companyNamePointer).trimmed();
        }

        // isFileTrustedByWindows 作用：
        // - 使用 WinVerifyTrust 判断文件是否被 Windows 信任。
        bool isFileTrustedByWindows(const QString& filePathText)
        {
            if (filePathText.trimmed().isEmpty())
            {
                return false;
            }

            const std::wstring utf16Path = filePathText.toStdWString();
            WINTRUST_FILE_INFO fileInfo{};
            fileInfo.cbStruct = sizeof(fileInfo);
            fileInfo.pcwszFilePath = utf16Path.c_str();

            WINTRUST_DATA trustData{};
            trustData.cbStruct = sizeof(trustData);
            trustData.dwUIChoice = WTD_UI_NONE;
            trustData.fdwRevocationChecks = WTD_REVOKE_NONE;
            trustData.dwUnionChoice = WTD_CHOICE_FILE;
            trustData.dwStateAction = WTD_STATEACTION_VERIFY;
            trustData.dwProvFlags = WTD_CACHE_ONLY_URL_RETRIEVAL;
            trustData.pFile = &fileInfo;

            GUID policyGuid = WINTRUST_ACTION_GENERIC_VERIFY_V2;
            const LONG verifyResult = ::WinVerifyTrust(nullptr, &policyGuid, &trustData);

            trustData.dwStateAction = WTD_STATEACTION_CLOSE;
            ::WinVerifyTrust(nullptr, &policyGuid, &trustData);

            return verifyResult == ERROR_SUCCESS;
        }
    }

    QIcon createBlueIcon(const char* resourcePath, const QSize& iconSize)
    {
        const QString iconPath = QString::fromUtf8(resourcePath);
        QSvgRenderer renderer(iconPath);
        if (!renderer.isValid())
        {
            return QIcon(iconPath);
        }

        QPixmap pixmap(iconSize);
        pixmap.fill(Qt::transparent);

        QPainter painter(&pixmap);
        painter.setRenderHint(QPainter::Antialiasing, true);
        renderer.render(&painter, QRectF(0, 0, iconSize.width(), iconSize.height()));
        painter.setCompositionMode(QPainter::CompositionMode_SourceIn);
        painter.fillRect(pixmap.rect(), KswordTheme::PrimaryBlueColor);
        painter.end();

        return QIcon(pixmap);
    }

    QTableWidgetItem* createReadOnlyItem(const QString& textValue)
    {
        QTableWidgetItem* itemPointer = new QTableWidgetItem(textValue);
        itemPointer->setFlags(itemPointer->flags() & ~Qt::ItemIsEditable);
        itemPointer->setToolTip(textValue);
        return itemPointer;
    }

    QString winErrorText(const DWORD errorCode)
    {
        LPWSTR bufferPointer = nullptr;
        const DWORD charCount = ::FormatMessageW(
            FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
            nullptr,
            errorCode,
            MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT),
            reinterpret_cast<LPWSTR>(&bufferPointer),
            0,
            nullptr);
        if (charCount == 0 || bufferPointer == nullptr)
        {
            return QStringLiteral("Win32Error=%1").arg(errorCode);
        }

        const QString messageText = QString::fromWCharArray(bufferPointer).trimmed();
        ::LocalFree(bufferPointer);
        return QStringLiteral("%1 (code=%2)").arg(messageText).arg(errorCode);
    }

    QString normalizeFilePathText(const QString& commandText)
    {
        const QString trimmedText = commandText.trimmed();
        if (trimmedText.isEmpty())
        {
            return QString();
        }

        if (trimmedText.startsWith('"'))
        {
            const int endQuoteIndex = trimmedText.indexOf('"', 1);
            if (endQuoteIndex > 1)
            {
                return QDir::toNativeSeparators(trimmedText.mid(1, endQuoteIndex - 1));
            }
        }

        const int exeIndex = trimmedText.indexOf(QStringLiteral(".exe"), 0, Qt::CaseInsensitive);
        if (exeIndex > 0)
        {
            return QDir::toNativeSeparators(trimmedText.left(exeIndex + 4));
        }

        const int dllIndex = trimmedText.indexOf(QStringLiteral(".dll"), 0, Qt::CaseInsensitive);
        if (dllIndex > 0)
        {
            return QDir::toNativeSeparators(trimmedText.left(dllIndex + 4));
        }

        const int sysIndex = trimmedText.indexOf(QStringLiteral(".sys"), 0, Qt::CaseInsensitive);
        if (sysIndex > 0)
        {
            return QDir::toNativeSeparators(trimmedText.left(sysIndex + 4));
        }

        const int spaceIndex = trimmedText.indexOf(' ');
        if (spaceIndex > 0)
        {
            return QDir::toNativeSeparators(trimmedText.left(spaceIndex));
        }
        return QDir::toNativeSeparators(trimmedText);
    }

    QString queryPublisherTextByPath(const QString& filePathText)
    {
        const QFileInfo fileInfo(filePathText);
        if (!fileInfo.exists() || !fileInfo.isFile())
        {
            return QString();
        }

        const QString companyNameText = queryCompanyNameByVersion(fileInfo.absoluteFilePath());
        if (!companyNameText.isEmpty())
        {
            return companyNameText + (isFileTrustedByWindows(fileInfo.absoluteFilePath())
                ? QStringLiteral(" (Trusted)")
                : QStringLiteral(" (Untrusted)"));
        }

        if (isFileTrustedByWindows(fileInfo.absoluteFilePath()))
        {
            return QStringLiteral("Signed (Trusted)");
        }
        return QString();
    }

    QString buildStatusText(const bool enabled)
    {
        return enabled ? QStringLiteral("启用") : QStringLiteral("禁用");
    }

    QStringList parseCsvLine(const QString& csvLineText)
    {
        QStringList fieldList;
        QString currentFieldText;
        bool inQuotes = false;

        for (int index = 0; index < csvLineText.size(); ++index)
        {
            const QChar currentChar = csvLineText.at(index);
            if (currentChar == QChar('"'))
            {
                if (inQuotes && index + 1 < csvLineText.size() && csvLineText.at(index + 1) == QChar('"'))
                {
                    currentFieldText.push_back(QChar('"'));
                    ++index;
                }
                else
                {
                    inQuotes = !inQuotes;
                }
                continue;
            }

            if (!inQuotes && currentChar == QChar(','))
            {
                fieldList.push_back(currentFieldText);
                currentFieldText.clear();
                continue;
            }

            currentFieldText.push_back(currentChar);
        }

        fieldList.push_back(currentFieldText);
        return fieldList;
    }
}

