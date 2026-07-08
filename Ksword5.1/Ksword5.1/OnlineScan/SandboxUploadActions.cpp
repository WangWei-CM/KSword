#include "SandboxUploadActions.h"

#include "OnlineScanSupport.h"
#include "VirusTotalOnlineScan.h"
#include "../ksword/process/process.h"

#include <QAction>
#include <QDir>
#include <QFileInfo>
#include <QIcon>
#include <QMenu>
#include <QMessageBox>
#include <QRegularExpression>
#include <QStringList>

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>

#include <iterator>
#include <string>
#include <utility>

namespace
{
    // sandboxUploadIcon 作用：
    // - 返回统一的上传/沙箱菜单图标；
    // - qrc 里没有专用云沙箱 alias，因此复用现有病毒图标。
    // 返回：可直接用于 QMenu/QAction 的 QIcon。
    QIcon sandboxUploadIcon()
    {
        return QIcon(QStringLiteral(":/Icon/process_critical.svg"));
    }

    // windowsDirectoryPath 作用：
    // - 读取当前 Windows 目录，优先 GetWindowsDirectoryW，失败时回退 SystemRoot 环境变量；
    // - 用于把 \SystemRoot\System32\drivers\xxx.sys 转换成 C:\Windows\System32\drivers\xxx.sys。
    // 返回：本机 Windows 目录；无法读取时返回空字符串。
    QString windowsDirectoryPath()
    {
        wchar_t windowsPathBuffer[MAX_PATH] = {};
        const UINT copiedChars = ::GetWindowsDirectoryW(windowsPathBuffer, MAX_PATH);
        if (copiedChars > 0 && copiedChars < MAX_PATH)
        {
            return QDir::toNativeSeparators(QString::fromWCharArray(windowsPathBuffer));
        }

        return QDir::toNativeSeparators(qEnvironmentVariable("SystemRoot"));
    }

    // stripOuterQuotes 作用：
    // - 去掉路径外层双引号；
    // - 只在首尾都为引号时剥离，避免破坏中间带引号的命令行。
    // 入参 text：原始路径文本。
    // 返回：剥离外层引号后的文本。
    QString stripOuterQuotes(const QString& text)
    {
        QString trimmedText = text.trimmed();
        while (trimmedText.size() >= 2 &&
            trimmedText.front() == QLatin1Char('"') &&
            trimmedText.back() == QLatin1Char('"'))
        {
            trimmedText = trimmedText.mid(1, trimmedText.size() - 2).trimmed();
        }
        return trimmedText;
    }

    // expandEnvironmentPath 作用：
    // - 展开 %SystemRoot%、%ProgramFiles% 等 Windows 环境变量；
    // - Qt 自身不会自动展开百分号形式变量，因此用 Win32 ExpandEnvironmentStringsW。
    // 入参 pathText：可能包含环境变量的路径。
    // 返回：展开后的路径；展开失败时返回原路径。
    QString expandEnvironmentPath(const QString& pathText)
    {
        const QString trimmedText = pathText.trimmed();
        if (!trimmedText.contains(QLatin1Char('%')))
        {
            return trimmedText;
        }

        const std::wstring wideInput = trimmedText.toStdWString();
        DWORD requiredChars = ::ExpandEnvironmentStringsW(wideInput.c_str(), nullptr, 0);
        if (requiredChars == 0)
        {
            return trimmedText;
        }

        std::wstring expandedText(requiredChars, L'\0');
        const DWORD copiedChars = ::ExpandEnvironmentStringsW(
            wideInput.c_str(),
            expandedText.data(),
            requiredChars);
        if (copiedChars == 0 || copiedChars > requiredChars)
        {
            return trimmedText;
        }
        if (!expandedText.empty() && expandedText.back() == L'\0')
        {
            expandedText.pop_back();
        }
        return QString::fromStdWString(expandedText);
    }

    // mapNtDevicePathToDosPath 作用：
    // - 将 \Device\HarddiskVolumeX\... 映射到 C:\... 形式；
    // - 通过 QueryDosDeviceW 枚举本机盘符对应的 NT device 前缀。
    // 入参 ntPathText：NT device 路径。
    // 返回：映射成功时为 DOS 路径；失败时返回空字符串。
    QString mapNtDevicePathToDosPath(const QString& ntPathText)
    {
        const QString normalizedNtPath = QDir::toNativeSeparators(ntPathText.trimmed());
        if (!normalizedNtPath.startsWith(QStringLiteral("\\Device\\"), Qt::CaseInsensitive))
        {
            return QString();
        }

        for (wchar_t driveLetter = L'A'; driveLetter <= L'Z'; ++driveLetter)
        {
            const QString driveText = QStringLiteral("%1:").arg(QChar(driveLetter));
            wchar_t deviceNameBuffer[1024] = {};
            const DWORD copiedChars = ::QueryDosDeviceW(
                reinterpret_cast<LPCWSTR>(driveText.utf16()),
                deviceNameBuffer,
                static_cast<DWORD>(std::size(deviceNameBuffer)));
            if (copiedChars == 0)
            {
                continue;
            }

            const QString deviceName = QDir::toNativeSeparators(QString::fromWCharArray(deviceNameBuffer));
            if (deviceName.isEmpty() || !normalizedNtPath.startsWith(deviceName, Qt::CaseInsensitive))
            {
                continue;
            }

            const QString suffixText = normalizedNtPath.mid(deviceName.size());
            if (!suffixText.isEmpty() && !suffixText.startsWith(QLatin1Char('\\')))
            {
                continue;
            }
            return QDir::toNativeSeparators(driveText + suffixText);
        }

        return QString();
    }

    // extractQuotedCommandPath 作用：
    // - 从 "\"C:\Path\App.exe\" -arg" 形式命令行提取首个引号内路径；
    // - 仅当文件存在时返回，避免把普通参数误当路径。
    // 入参 commandText：命令行文本。
    // 返回：存在的首个引号内路径；失败时返回空字符串。
    QString extractQuotedCommandPath(const QString& commandText)
    {
        const QRegularExpression quotedPathExpression(QStringLiteral("\"([^\"]+)\""));
        QRegularExpressionMatchIterator matchIterator = quotedPathExpression.globalMatch(commandText);
        while (matchIterator.hasNext())
        {
            const QRegularExpressionMatch match = matchIterator.next();
            const QString candidatePath = ks::online_scan::normalizeKernelImagePathForUpload(match.captured(1));
            if (QFileInfo(candidatePath).isFile())
            {
                return QFileInfo(candidatePath).absoluteFilePath();
            }
        }
        return QString();
    }

    // extractUnquotedCommandPath 作用：
    // - 从未加引号命令行中尝试提取一个存在的 .exe/.dll/.sys/.ocx 文件路径；
    // - 支持路径包含空格，通过逐段递增拼接并检查文件存在性。
    // 入参 commandText：命令行文本。
    // 返回：存在的文件路径；失败时返回空字符串。
    QString extractUnquotedCommandPath(const QString& commandText)
    {
        const QString normalizedText = ks::online_scan::normalizeKernelImagePathForUpload(commandText);
        const QStringList tokenList = normalizedText.split(QRegularExpression(QStringLiteral("\\s+")), Qt::SkipEmptyParts);
        QString candidateText;
        for (const QString& tokenText : tokenList)
        {
            candidateText = candidateText.isEmpty()
                ? tokenText
                : (candidateText + QLatin1Char(' ') + tokenText);
            const QString strippedCandidate = stripOuterQuotes(candidateText);
            const QFileInfo candidateInfo(strippedCandidate);
            if (candidateInfo.isFile())
            {
                return candidateInfo.absoluteFilePath();
            }

            const QString lowerCandidate = strippedCandidate.toLower();
            if (lowerCandidate.endsWith(QStringLiteral(".exe")) ||
                lowerCandidate.endsWith(QStringLiteral(".dll")) ||
                lowerCandidate.endsWith(QStringLiteral(".sys")) ||
                lowerCandidate.endsWith(QStringLiteral(".ocx")))
            {
                break;
            }
        }
        return QString();
    }

    // defaultSourceText 作用：
    // - 生成统一的兜底来源文本；
    // - 避免结果窗口出现空来源。
    // 入参 sourceText：调用方来源文本。
    // 返回：非空来源文本。
    QString defaultSourceText(const QString& sourceText)
    {
        const QString trimmedText = sourceText.trimmed();
        return trimmedText.isEmpty() ? QStringLiteral("右键上传到沙箱") : trimmedText;
    }
}

QAction* ks::online_scan::addVirusTotalSandboxMenu(
    QMenu* menu,
    QWidget* parentWidget,
    QFilePathResolver resolver)
{
    if (menu == nullptr)
    {
        return nullptr;
    }

    // 菜单结构必须固定为“上传到沙箱 -> VT”，ThreatBook 本轮不显示。
    QMenu* sandboxMenu = menu->addMenu(sandboxUploadIcon(), QStringLiteral("上传到沙箱"));
    QAction* virusTotalAction = sandboxMenu->addAction(sandboxUploadIcon(), QStringLiteral("VT"));
    QObject::connect(virusTotalAction, &QAction::triggered, menu, [resolver = std::move(resolver), parentWidget]()
        {
            // 输入：右键菜单触发时的当前 UI 状态。
            // 处理：延迟解析路径并进入统一 VT 上传；解析异常用弹窗提示。
            // 返回：无。
            if (!resolver)
            {
                showErrorDialog(
                    parentWidget,
                    QStringLiteral("上传到沙箱"),
                    QStringLiteral("当前入口没有提供文件路径解析器。"));
                return;
            }

            const SandboxUploadTarget target = resolver();
            if (target.filePath.trimmed().isEmpty())
            {
                showErrorDialog(
                    parentWidget,
                    QStringLiteral("上传到沙箱 - VT"),
                    target.errorText.trimmed().isEmpty()
                        ? QStringLiteral("未解析到可上传文件路径。若来源是 PID，进程可能已退出，或当前权限不足。")
                        : target.errorText.trimmed());
                return;
            }
            uploadFileToVirusTotal(target.filePath, target.sourceText, parentWidget);
        });
    return virusTotalAction;
}

void ks::online_scan::uploadFileToVirusTotal(
    const QString& filePath,
    const QString& sourceText,
    QWidget* parentWidget)
{
    const QString normalizedPath = extractExistingFilePathForUpload(filePath);
    QString fileErrorText;
    if (!validateReadableFile(normalizedPath, kVirusTotalLargeUploadMaxBytes, &fileErrorText))
    {
        showErrorDialog(
            parentWidget,
            QStringLiteral("上传到沙箱 - VT"),
            fileErrorText);
        return;
    }

    VirusTotalOnlineScan::scanFileAndAutoDelete(
        QFileInfo(normalizedPath).absoluteFilePath(),
        defaultSourceText(sourceText),
        parentWidget);
}

void ks::online_scan::uploadProcessImageByPid(
    const std::uint32_t pid,
    const QString& sourceText,
    QWidget* parentWidget)
{
    if (pid == 0)
    {
        showErrorDialog(
            parentWidget,
            QStringLiteral("上传到沙箱 - VT"),
            QStringLiteral("PID 无效，无法解析进程镜像路径。"));
        return;
    }

    const QString processPath = QString::fromStdString(ks::process::QueryProcessPathByPid(pid)).trimmed();
    if (processPath.isEmpty())
    {
        showErrorDialog(
            parentWidget,
            QStringLiteral("上传到沙箱 - VT"),
            QStringLiteral("无法解析 PID=%1 的进程镜像路径。进程可能已退出，或当前权限不足。").arg(pid));
        return;
    }

    // resolvedSourceText 作用：
    // - 调用方传入来源说明时原样使用；
    // - 来源为空时明确写入 PID，避免结果窗口只显示通用“右键上传到沙箱”。
    const QString resolvedSourceText = sourceText.trimmed().isEmpty()
        ? QStringLiteral("PID=%1").arg(pid)
        : sourceText.trimmed();
    uploadFileToVirusTotal(processPath, resolvedSourceText, parentWidget);
}

QString ks::online_scan::normalizeKernelImagePathForUpload(const QString& rawPathText)
{
    QString pathText = stripOuterQuotes(rawPathText);
    if (pathText.isEmpty())
    {
        return QString();
    }

    pathText.replace(QLatin1Char('/'), QLatin1Char('\\'));

    if (pathText.startsWith(QStringLiteral("\\??\\")))
    {
        pathText = pathText.mid(4);
    }
    else if (pathText.startsWith(QStringLiteral("\\\\?\\")))
    {
        pathText = pathText.mid(4);
    }

    const QString windowsPath = windowsDirectoryPath();
    if (pathText.startsWith(QStringLiteral("\\SystemRoot\\"), Qt::CaseInsensitive) && !windowsPath.isEmpty())
    {
        pathText = windowsPath + pathText.mid(QStringLiteral("\\SystemRoot").size());
    }
    else if (pathText.compare(QStringLiteral("\\SystemRoot"), Qt::CaseInsensitive) == 0 && !windowsPath.isEmpty())
    {
        pathText = windowsPath;
    }
    else if (pathText.startsWith(QStringLiteral("SystemRoot\\"), Qt::CaseInsensitive) && !windowsPath.isEmpty())
    {
        pathText = windowsPath + QStringLiteral("\\") + pathText.mid(QStringLiteral("SystemRoot\\").size());
    }
    else if (pathText.startsWith(QStringLiteral("%SystemRoot%"), Qt::CaseInsensitive))
    {
        pathText = expandEnvironmentPath(pathText);
    }
    else if (pathText.startsWith(QStringLiteral("\\Device\\"), Qt::CaseInsensitive))
    {
        const QString mappedPath = mapNtDevicePathToDosPath(pathText);
        if (!mappedPath.isEmpty())
        {
            pathText = mappedPath;
        }
    }
    else
    {
        pathText = expandEnvironmentPath(pathText);
    }

    return QDir::toNativeSeparators(pathText);
}

QString ks::online_scan::extractExistingFilePathForUpload(const QString& rawPathText)
{
    QString pathText = normalizeKernelImagePathForUpload(rawPathText);
    if (pathText.isEmpty())
    {
        return QString();
    }

    const QFileInfo directInfo(pathText);
    if (directInfo.isFile())
    {
        return directInfo.absoluteFilePath();
    }

    const QString quotedPath = extractQuotedCommandPath(pathText);
    if (!quotedPath.isEmpty())
    {
        return quotedPath;
    }

    const QString unquotedPath = extractUnquotedCommandPath(pathText);
    if (!unquotedPath.isEmpty())
    {
        return unquotedPath;
    }

    return pathText;
}

bool ks::online_scan::tryParsePidFromText(const QString& pidText, std::uint32_t* pidOut)
{
    if (pidOut != nullptr)
    {
        *pidOut = 0;
    }

    const QString text = pidText.trimmed();
    if (text.isEmpty())
    {
        return false;
    }

    // 先处理纯数字，避免正则从十六进制地址等字段中误提取片段。
    bool parseOk = false;
    const quint64 directValue = text.toULongLong(&parseOk, 10);
    if (parseOk && directValue > 0 && directValue <= 0xFFFFFFFFULL)
    {
        if (pidOut != nullptr)
        {
            *pidOut = static_cast<std::uint32_t>(directValue);
        }
        return true;
    }

    const QRegularExpression pidExpression(QStringLiteral("(?:PID|Pid|pid)?\\s*[:=]?\\s*(\\d{1,10})"));
    const QRegularExpressionMatch match = pidExpression.match(text);
    if (!match.hasMatch())
    {
        return false;
    }

    const quint64 value = match.captured(1).toULongLong(&parseOk, 10);
    if (!parseOk || value == 0 || value > 0xFFFFFFFFULL)
    {
        return false;
    }

    if (pidOut != nullptr)
    {
        *pidOut = static_cast<std::uint32_t>(value);
    }
    return true;
}
