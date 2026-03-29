#include "FileDock.h"

// ============================================================
// FileDock.cpp
// 说明：
// - 该文件实现双栏资源管理器核心交互；
// - 支持导航、过滤、排序、基础文件操作与文件详情展示。
// ============================================================

#include "../theme.h"
#include "../UI/CodeEditorWidget.h"
#include "../UI/HexEditorWidget.h"

#include <QApplication>
#include <QClipboard>
#include <QCheckBox>
#include <QColor>
#include <QComboBox>
#include <QCryptographicHash>
#include <QDesktopServices>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDir>
#include <QDateTime>
#include <QFile>
#include <QFileDevice>
#include <QFileInfo>
#include <QFileDialog>
#include <QFileSystemModel>
#include <QFormLayout>
#include <QFrame>
#include <QGroupBox>
#include <QHeaderView>
#include <QHBoxLayout>
#include <QInputDialog>
#include <QItemSelectionModel>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QProcess>
#include <QPushButton>
#include <QRegularExpression>
#include <QScrollArea>
#include <QShortcut>
#include <QSortFilterProxyModel>
#include <QSplitter>
#include <QStandardItemModel>
#include <QStatusBar>
#include <QStorageInfo>
#include <QStackedWidget>
#include <QTabWidget>
#include <QTableWidget>
#include <QTableWidgetItem>
#include <QToolButton>
#include <QTreeView>
#include <QUrl>
#include <QVector>
#include <QVBoxLayout>

#include <array>
#include <algorithm>
#include <cctype>
#include <cmath>
#include <memory>
#include <string>

#include <Aclapi.h>
#include <Sddl.h>

namespace
{
    // 手动解析模型列定义：名称/大小/类型/修改时间/完整路径/是否目录。
    enum class ManualModelColumn : int
    {
        Name = 0,
        Size = 1,
        Type = 2,
        ModifiedTime = 3,
        FullPath = 4,
        IsDirectory = 5,
        Count = 6
    };

    // manualFsTypeToText 作用：手动解析结果类型转可读文本。
    QString manualFsTypeToText(const ks::file::ManualFsType fsType)
    {
        switch (fsType)
        {
        case ks::file::ManualFsType::Ntfs:
            return QStringLiteral("NTFS");
        case ks::file::ManualFsType::Fat32:
            return QStringLiteral("FAT32");
        default:
            return QStringLiteral("Unknown");
        }
    }

    // 统一按钮样式，保持与主界面蓝色主题一致。
    QString buildBlueButtonStyle()
    {
        return QStringLiteral(
            "QPushButton,QToolButton{"
            "  color:%1;"
            "  background:%5;"
            "  border:1px solid %2;"
            "  border-radius:3px;"
            "  padding:3px 8px;"
            "}"
            "QPushButton:hover,QToolButton:hover{background:%3;color:#FFFFFF;border:1px solid %3;}"
            "QPushButton:pressed,QToolButton:pressed{background:%4;color:#FFFFFF;}")
            .arg(KswordTheme::PrimaryBlueHex)
            .arg(KswordTheme::PrimaryBlueBorderHex)
            .arg(QStringLiteral("#2E8BFF"))
            .arg(KswordTheme::PrimaryBluePressedHex)
            .arg(KswordTheme::SurfaceHex());
    }

    // 统一输入控件样式。
    QString buildBlueInputStyle()
    {
        return QStringLiteral(
            "QLineEdit,QComboBox,QPlainTextEdit,QTextEdit{"
            "  border:1px solid %2;"
            "  border-radius:3px;"
            "  background:%3;"
            "  color:%4;"
            "  padding:2px 6px;"
            "}"
            "QLineEdit:focus,QComboBox:focus,QPlainTextEdit:focus,QTextEdit:focus{"
            "  border:1px solid %1;}")
            .arg(KswordTheme::PrimaryBlueHex)
            .arg(KswordTheme::BorderHex())
            .arg(KswordTheme::SurfaceHex())
            .arg(KswordTheme::TextPrimaryHex());
    }

    // 面包屑按钮样式：视觉上“嵌入输入框”，并保留轻量 hover 提示。
    QString buildBreadcrumbButtonStyle()
    {
        return QStringLiteral(
            "QToolButton{"
            "  color:%1;"
            "  background:transparent;"
            "  border:none;"
            "  padding:0 4px;"
            "}"
            "QToolButton:hover{"
            "  background:%2;"
            "  border-radius:3px;"
            "}"
            "QToolButton:pressed{"
            "  background:%3;"
            "}")
            .arg(KswordTheme::PrimaryBlueHex)
            .arg(KswordTheme::PrimaryBlueHoverHex)
            .arg(KswordTheme::PrimaryBluePressedHex);
    }

    // 递归复制目录：用于粘贴目录场景。
    bool copyDirectoryRecursively(const QString& sourcePath, const QString& targetPath, QString& errorTextOut)
    {
        QDir sourceDir(sourcePath);
        if (!sourceDir.exists())
        {
            errorTextOut = QStringLiteral("源目录不存在: %1").arg(sourcePath);
            return false;
        }

        QDir targetDir;
        if (!targetDir.mkpath(targetPath))
        {
            errorTextOut = QStringLiteral("创建目标目录失败: %1").arg(targetPath);
            return false;
        }

        const QFileInfoList entries = sourceDir.entryInfoList(
            QDir::NoDotAndDotDot | QDir::AllEntries | QDir::Hidden | QDir::System);
        for (const QFileInfo& info : entries)
        {
            const QString src = info.absoluteFilePath();
            const QString dst = QDir(targetPath).filePath(info.fileName());

            if (info.isDir())
            {
                if (!copyDirectoryRecursively(src, dst, errorTextOut))
                {
                    return false;
                }
            }
            else
            {
                if (QFile::exists(dst))
                {
                    QFile::remove(dst);
                }
                if (!QFile::copy(src, dst))
                {
                    errorTextOut = QStringLiteral("复制文件失败: %1 -> %2").arg(src, dst);
                    return false;
                }
            }
        }

        return true;
    }

    // runCommandCaptureText：
    // - 作用：同步执行 cmd 命令并返回标准输出/错误输出合并文本。
    // - 参数 commandText：传入 cmd /C 后执行的命令字符串。
    // - 参数 outputTextOut：返回执行输出文本，便于错误提示。
    // - 参数 exitCodeOut：返回进程退出码，调用方用于判断成功/失败。
    bool runCommandCaptureText(const QString& commandText, QString& outputTextOut, int& exitCodeOut)
    {
        QProcess process;
        process.setProgram(QStringLiteral("cmd.exe"));
        process.setArguments(QStringList{ QStringLiteral("/C"), commandText });
        process.start();
        process.waitForFinished(-1);

        const QByteArray stdOutBytes = process.readAllStandardOutput();
        const QByteArray stdErrBytes = process.readAllStandardError();
        outputTextOut = QString::fromLocal8Bit(stdOutBytes + stdErrBytes).trimmed();
        exitCodeOut = process.exitCode();
        return process.exitStatus() == QProcess::NormalExit && process.exitCode() == 0;
    }

    // takeOwnershipBySystemCommand：
    // - 作用：对目标路径执行 takeown 与 icacls，获取所有权并授权管理员组完全控制。
    // - 参数 targetPath：待处理文件/目录路径。
    // - 参数 detailTextOut：输出步骤详情（失败时用于提示）。
    // - 返回：全部步骤成功时返回 true。
    bool takeOwnershipBySystemCommand(const QString& targetPath, QString& detailTextOut)
    {
        const QFileInfo info(targetPath);
        const QString quotedPath = QStringLiteral("\"%1\"").arg(QDir::toNativeSeparators(targetPath));
        const QString takeOwnCommand = info.isDir()
            ? QStringLiteral("takeown /F %1 /A /R /D Y").arg(quotedPath)
            : QStringLiteral("takeown /F %1 /A /D Y").arg(quotedPath);
        const QString grantCommand = info.isDir()
            ? QStringLiteral("icacls %1 /grant *S-1-5-32-544:F /T /C").arg(quotedPath)
            : QStringLiteral("icacls %1 /grant *S-1-5-32-544:F /C").arg(quotedPath);

        QString firstOutput;
        int firstExitCode = -1;
        const bool takeOwnOk = runCommandCaptureText(takeOwnCommand, firstOutput, firstExitCode);

        QString secondOutput;
        int secondExitCode = -1;
        const bool grantOk = runCommandCaptureText(grantCommand, secondOutput, secondExitCode);

        detailTextOut = QStringLiteral(
            "目标: %1\n"
            "takeown命令: %2\n"
            "takeown退出码: %3\n"
            "takeown输出:\n%4\n\n"
            "icacls命令: %5\n"
            "icacls退出码: %6\n"
            "icacls输出:\n%7")
            .arg(QDir::toNativeSeparators(targetPath))
            .arg(takeOwnCommand)
            .arg(firstExitCode)
            .arg(firstOutput.isEmpty() ? QStringLiteral("<无输出>") : firstOutput)
            .arg(grantCommand)
            .arg(secondExitCode)
            .arg(secondOutput.isEmpty() ? QStringLiteral("<无输出>") : secondOutput);
        return takeOwnOk && grantOk;
    }

    // sidUseToText 作用：
    // - 把 SID_NAME_USE 枚举转换为可读文本；
    // - 用于 ACL 列表中显示主体类型（用户/组/域等）。
    QString sidUseToText(const SID_NAME_USE sidUse)
    {
        switch (sidUse)
        {
        case SidTypeUser: return QStringLiteral("User");
        case SidTypeGroup: return QStringLiteral("Group");
        case SidTypeDomain: return QStringLiteral("Domain");
        case SidTypeAlias: return QStringLiteral("Alias");
        case SidTypeWellKnownGroup: return QStringLiteral("WellKnownGroup");
        case SidTypeDeletedAccount: return QStringLiteral("DeletedAccount");
        case SidTypeInvalid: return QStringLiteral("Invalid");
        case SidTypeUnknown: return QStringLiteral("Unknown");
        case SidTypeComputer: return QStringLiteral("Computer");
        case SidTypeLabel: return QStringLiteral("Label");
        default: return QStringLiteral("Other");
        }
    }

    // sidToStringText 作用：
    // - 把 PSID 转换为标准字符串形式（S-1-5-...）；
    // - 失败时返回包含错误信息的占位文本。
    QString sidToStringText(PSID sidValue)
    {
        if (sidValue == nullptr)
        {
            return QStringLiteral("<空SID>");
        }
        LPWSTR sidStringBuffer = nullptr;
        if (::ConvertSidToStringSidW(sidValue, &sidStringBuffer) == FALSE || sidStringBuffer == nullptr)
        {
            return QStringLiteral("<SID转换失败 code=%1>").arg(::GetLastError());
        }
        QString sidText = QString::fromWCharArray(sidStringBuffer);
        ::LocalFree(sidStringBuffer);
        return sidText;
    }

    // sidToAccountText 作用：
    // - 通过 LookupAccountSidW 解析 SID 的域名与账户名；
    // - 解析失败时保留错误码，便于权限审计定位。
    QString sidToAccountText(PSID sidValue)
    {
        if (sidValue == nullptr)
        {
            return QStringLiteral("<空SID>");
        }

        wchar_t accountBuffer[256] = {};
        wchar_t domainBuffer[256] = {};
        DWORD accountSize = static_cast<DWORD>(std::size(accountBuffer));
        DWORD domainSize = static_cast<DWORD>(std::size(domainBuffer));
        SID_NAME_USE sidUse = SidTypeUnknown;
        if (::LookupAccountSidW(
            nullptr,
            sidValue,
            accountBuffer,
            &accountSize,
            domainBuffer,
            &domainSize,
            &sidUse) == FALSE)
        {
            return QStringLiteral("<账户解析失败 code=%1>").arg(::GetLastError());
        }

        const QString accountText = QString::fromWCharArray(accountBuffer);
        const QString domainText = QString::fromWCharArray(domainBuffer);
        if (domainText.isEmpty())
        {
            return QStringLiteral("%1 (%2)").arg(accountText, sidUseToText(sidUse));
        }
        return QStringLiteral("%1\\%2 (%3)").arg(domainText, accountText, sidUseToText(sidUse));
    }

    // aceTypeToText 作用：
    // - 把 ACE_HEADER::AceType 转换为可读文本；
    // - 未覆盖类型保留原始数值，避免信息丢失。
    QString aceTypeToText(const BYTE aceType)
    {
        switch (aceType)
        {
        case ACCESS_ALLOWED_ACE_TYPE: return QStringLiteral("ACCESS_ALLOWED");
        case ACCESS_DENIED_ACE_TYPE: return QStringLiteral("ACCESS_DENIED");
        case SYSTEM_AUDIT_ACE_TYPE: return QStringLiteral("SYSTEM_AUDIT");
        case SYSTEM_ALARM_ACE_TYPE: return QStringLiteral("SYSTEM_ALARM");
        case ACCESS_ALLOWED_OBJECT_ACE_TYPE: return QStringLiteral("ACCESS_ALLOWED_OBJECT");
        case ACCESS_DENIED_OBJECT_ACE_TYPE: return QStringLiteral("ACCESS_DENIED_OBJECT");
        case SYSTEM_AUDIT_OBJECT_ACE_TYPE: return QStringLiteral("SYSTEM_AUDIT_OBJECT");
        case SYSTEM_MANDATORY_LABEL_ACE_TYPE: return QStringLiteral("MANDATORY_LABEL");
        default:
            return QStringLiteral("ACE_%1").arg(aceType);
        }
    }

    // aceFlagsToText 作用：
    // - 解析 ACE 继承/审计标志位；
    // - 返回以“|”分隔的复合文本。
    QString aceFlagsToText(const BYTE aceFlags)
    {
        QStringList flagList;
        if ((aceFlags & OBJECT_INHERIT_ACE) != 0) flagList << QStringLiteral("OBJECT_INHERIT");
        if ((aceFlags & CONTAINER_INHERIT_ACE) != 0) flagList << QStringLiteral("CONTAINER_INHERIT");
        if ((aceFlags & NO_PROPAGATE_INHERIT_ACE) != 0) flagList << QStringLiteral("NO_PROPAGATE");
        if ((aceFlags & INHERIT_ONLY_ACE) != 0) flagList << QStringLiteral("INHERIT_ONLY");
        if ((aceFlags & INHERITED_ACE) != 0) flagList << QStringLiteral("INHERITED");
        if ((aceFlags & SUCCESSFUL_ACCESS_ACE_FLAG) != 0) flagList << QStringLiteral("AUDIT_SUCCESS");
        if ((aceFlags & FAILED_ACCESS_ACE_FLAG) != 0) flagList << QStringLiteral("AUDIT_FAIL");
        return flagList.isEmpty() ? QStringLiteral("None") : flagList.join('|');
    }

    // accessMaskToText 作用：
    // - 把文件系统访问掩码拆解为常见权限名；
    // - 既保留 GENERIC_*，也保留 FILE_* 细粒度权限。
    QString accessMaskToText(const DWORD accessMask)
    {
        QStringList rightList;
        if ((accessMask & GENERIC_ALL) != 0) rightList << QStringLiteral("GENERIC_ALL");
        if ((accessMask & GENERIC_READ) != 0) rightList << QStringLiteral("GENERIC_READ");
        if ((accessMask & GENERIC_WRITE) != 0) rightList << QStringLiteral("GENERIC_WRITE");
        if ((accessMask & GENERIC_EXECUTE) != 0) rightList << QStringLiteral("GENERIC_EXECUTE");
        if ((accessMask & FILE_ALL_ACCESS) == FILE_ALL_ACCESS) rightList << QStringLiteral("FILE_ALL_ACCESS");
        if ((accessMask & FILE_GENERIC_READ) == FILE_GENERIC_READ) rightList << QStringLiteral("FILE_GENERIC_READ");
        if ((accessMask & FILE_GENERIC_WRITE) == FILE_GENERIC_WRITE) rightList << QStringLiteral("FILE_GENERIC_WRITE");
        if ((accessMask & FILE_GENERIC_EXECUTE) == FILE_GENERIC_EXECUTE) rightList << QStringLiteral("FILE_GENERIC_EXECUTE");
        if ((accessMask & FILE_READ_DATA) != 0) rightList << QStringLiteral("READ_DATA");
        if ((accessMask & FILE_WRITE_DATA) != 0) rightList << QStringLiteral("WRITE_DATA");
        if ((accessMask & FILE_APPEND_DATA) != 0) rightList << QStringLiteral("APPEND_DATA");
        if ((accessMask & FILE_EXECUTE) != 0) rightList << QStringLiteral("EXECUTE");
        if ((accessMask & FILE_READ_ATTRIBUTES) != 0) rightList << QStringLiteral("READ_ATTRIBUTES");
        if ((accessMask & FILE_WRITE_ATTRIBUTES) != 0) rightList << QStringLiteral("WRITE_ATTRIBUTES");
        if ((accessMask & FILE_READ_EA) != 0) rightList << QStringLiteral("READ_EA");
        if ((accessMask & FILE_WRITE_EA) != 0) rightList << QStringLiteral("WRITE_EA");
        if ((accessMask & DELETE) != 0) rightList << QStringLiteral("DELETE");
        if ((accessMask & READ_CONTROL) != 0) rightList << QStringLiteral("READ_CONTROL");
        if ((accessMask & WRITE_DAC) != 0) rightList << QStringLiteral("WRITE_DAC");
        if ((accessMask & WRITE_OWNER) != 0) rightList << QStringLiteral("WRITE_OWNER");
        if ((accessMask & SYNCHRONIZE) != 0) rightList << QStringLiteral("SYNCHRONIZE");
        return rightList.isEmpty() ? QStringLiteral("None") : rightList.join('|');
    }

    // appendAclText 作用：
    // - 解析 ACL 中每一条 ACE，输出类型、标志、掩码、SID 与账户名；
    // - titleText 用于区分 DACL 与 SACL 段落。
    void appendAclText(const QString& titleText, PACL aclValue, QString& contentOut)
    {
        contentOut += QStringLiteral("\n[%1]\n").arg(titleText);
        if (aclValue == nullptr)
        {
            contentOut += QStringLiteral("ACL: <null>\n");
            return;
        }

        ACL_SIZE_INFORMATION aclSizeInfo{};
        if (::GetAclInformation(
            aclValue,
            &aclSizeInfo,
            static_cast<DWORD>(sizeof(aclSizeInfo)),
            AclSizeInformation) == FALSE)
        {
            contentOut += QStringLiteral("读取 ACL 信息失败, code=%1\n").arg(::GetLastError());
            return;
        }

        contentOut += QStringLiteral("ACE数量: %1\n").arg(aclSizeInfo.AceCount);
        for (DWORD aceIndex = 0; aceIndex < aclSizeInfo.AceCount; ++aceIndex)
        {
            LPVOID acePointer = nullptr;
            if (::GetAce(aclValue, aceIndex, &acePointer) == FALSE || acePointer == nullptr)
            {
                contentOut += QStringLiteral("  - ACE[%1] 读取失败, code=%2\n").arg(aceIndex).arg(::GetLastError());
                continue;
            }

            ACE_HEADER* aceHeader = reinterpret_cast<ACE_HEADER*>(acePointer);
            DWORD accessMask = 0;
            PSID aceSid = nullptr;

            switch (aceHeader->AceType)
            {
            case ACCESS_ALLOWED_ACE_TYPE:
            {
                ACCESS_ALLOWED_ACE* aceBody = reinterpret_cast<ACCESS_ALLOWED_ACE*>(acePointer);
                accessMask = aceBody->Mask;
                aceSid = reinterpret_cast<PSID>(&aceBody->SidStart);
                break;
            }
            case ACCESS_DENIED_ACE_TYPE:
            {
                ACCESS_DENIED_ACE* aceBody = reinterpret_cast<ACCESS_DENIED_ACE*>(acePointer);
                accessMask = aceBody->Mask;
                aceSid = reinterpret_cast<PSID>(&aceBody->SidStart);
                break;
            }
            case SYSTEM_AUDIT_ACE_TYPE:
            {
                SYSTEM_AUDIT_ACE* aceBody = reinterpret_cast<SYSTEM_AUDIT_ACE*>(acePointer);
                accessMask = aceBody->Mask;
                aceSid = reinterpret_cast<PSID>(&aceBody->SidStart);
                break;
            }
            case ACCESS_ALLOWED_OBJECT_ACE_TYPE:
            {
                ACCESS_ALLOWED_OBJECT_ACE* aceBody = reinterpret_cast<ACCESS_ALLOWED_OBJECT_ACE*>(acePointer);
                accessMask = aceBody->Mask;
                aceSid = reinterpret_cast<PSID>(&aceBody->SidStart);
                break;
            }
            case ACCESS_DENIED_OBJECT_ACE_TYPE:
            {
                ACCESS_DENIED_OBJECT_ACE* aceBody = reinterpret_cast<ACCESS_DENIED_OBJECT_ACE*>(acePointer);
                accessMask = aceBody->Mask;
                aceSid = reinterpret_cast<PSID>(&aceBody->SidStart);
                break;
            }
            case SYSTEM_AUDIT_OBJECT_ACE_TYPE:
            {
                SYSTEM_AUDIT_OBJECT_ACE* aceBody = reinterpret_cast<SYSTEM_AUDIT_OBJECT_ACE*>(acePointer);
                accessMask = aceBody->Mask;
                aceSid = reinterpret_cast<PSID>(&aceBody->SidStart);
                break;
            }
            default:
                break;
            }

            contentOut += QStringLiteral("  - ACE[%1]\n").arg(aceIndex);
            contentOut += QStringLiteral("    类型: %1\n").arg(aceTypeToText(aceHeader->AceType));
            contentOut += QStringLiteral("    标志: %1\n").arg(aceFlagsToText(aceHeader->AceFlags));
            contentOut += QStringLiteral("    Mask: 0x%1\n").arg(accessMask, 8, 16, QLatin1Char('0'));
            contentOut += QStringLiteral("    权限: %1\n").arg(accessMaskToText(accessMask));
            contentOut += QStringLiteral("    SID: %1\n").arg(sidToStringText(aceSid));
            contentOut += QStringLiteral("    账户: %1\n").arg(sidToAccountText(aceSid));
        }
    }

    // 简单文件详情对话框：按 Tab 展示通用/安全/哈希/签名/PE/字符串/十六进制。
    class FileDetailDialog final : public QDialog
    {
    public:
        explicit FileDetailDialog(const QString& filePath, QWidget* parent = nullptr)
            : QDialog(parent)
            , m_filePath(filePath)
        {
            setAttribute(Qt::WA_DeleteOnClose, true);
            setWindowTitle(QStringLiteral("文件属性 - %1").arg(QFileInfo(filePath).fileName()));
            resize(980, 680);

            QVBoxLayout* rootLayout = new QVBoxLayout(this);
            QTabWidget* tabWidget = new QTabWidget(this);
            rootLayout->addWidget(tabWidget, 1);

            tabWidget->addTab(buildGeneralTab(), QStringLiteral("常规信息"));
            tabWidget->addTab(buildSecurityTab(), QStringLiteral("安全与权限"));
            tabWidget->addTab(buildHashTab(), QStringLiteral("哈希与完整性"));
            tabWidget->addTab(buildSignatureTab(), QStringLiteral("数字签名"));
            tabWidget->addTab(buildPeTab(), QStringLiteral("PE信息"));
            tabWidget->addTab(buildStringsTab(), QStringLiteral("字符串"));
            tabWidget->addTab(buildHexTab(), QStringLiteral("十六进制"));
        }

    private:
        QWidget* buildGeneralTab()
        {
            QWidget* page = new QWidget(this);
            QVBoxLayout* layout = new QVBoxLayout(page);
            CodeEditorWidget* textEditorWidget = new CodeEditorWidget(page);
            textEditorWidget->setReadOnly(true);

            const QFileInfo info(m_filePath);
            QString content;
            content += QStringLiteral("完整路径: %1\n").arg(info.absoluteFilePath());
            content += QStringLiteral("文件名: %1\n").arg(info.fileName());
            content += QStringLiteral("类型: %1\n").arg(info.suffix());
            content += QStringLiteral("大小: %1 字节\n").arg(info.size());
            content += QStringLiteral("创建时间: %1\n").arg(info.birthTime().toString("yyyy-MM-dd HH:mm:ss"));
            content += QStringLiteral("修改时间: %1\n").arg(info.lastModified().toString("yyyy-MM-dd HH:mm:ss"));
            content += QStringLiteral("访问时间: %1\n").arg(info.lastRead().toString("yyyy-MM-dd HH:mm:ss"));
            content += QStringLiteral("是否可执行: %1\n").arg(info.isExecutable() ? QStringLiteral("是") : QStringLiteral("否"));
            content += QStringLiteral("是否隐藏: %1\n").arg(info.isHidden() ? QStringLiteral("是") : QStringLiteral("否"));
            content += QStringLiteral("是否可写: %1\n").arg(info.isWritable() ? QStringLiteral("是") : QStringLiteral("否"));
            textEditorWidget->setText(content);

            layout->addWidget(textEditorWidget, 1);
            return page;
        }

        QWidget* buildSecurityTab()
        {
            QWidget* page = new QWidget(this);
            QVBoxLayout* layout = new QVBoxLayout(page);
            CodeEditorWidget* textEditorWidget = new CodeEditorWidget(page);
            textEditorWidget->setReadOnly(true);

            QString content;
            const QString nativePath = QDir::toNativeSeparators(m_filePath);
            std::wstring nativePathBuffer = nativePath.toStdWString();
            content += QStringLiteral("目标路径: %1\n").arg(nativePath);

            // 先给出 Qt 维度的快速权限摘要，便于与 ACL 细节对照。
            QFileInfo info(m_filePath);
            content += QStringLiteral("快速权限摘要:\n");
            content += QStringLiteral("Read: %1\n").arg(info.isReadable() ? QStringLiteral("允许") : QStringLiteral("拒绝"));
            content += QStringLiteral("Write: %1\n").arg(info.isWritable() ? QStringLiteral("允许") : QStringLiteral("拒绝"));
            content += QStringLiteral("Execute: %1\n").arg(info.isExecutable() ? QStringLiteral("允许") : QStringLiteral("拒绝"));

            // 使用 GetNamedSecurityInfoW 拉取 Owner / Group / DACL，做深层 ACL/SID 解析。
            PSID ownerSid = nullptr;
            PSID groupSid = nullptr;
            PACL dacl = nullptr;
            PSECURITY_DESCRIPTOR securityDescriptor = nullptr;
            const DWORD queryMask = OWNER_SECURITY_INFORMATION
                | GROUP_SECURITY_INFORMATION
                | DACL_SECURITY_INFORMATION;
            const DWORD queryResult = ::GetNamedSecurityInfoW(
                nativePathBuffer.data(),
                SE_FILE_OBJECT,
                queryMask,
                &ownerSid,
                &groupSid,
                &dacl,
                nullptr,
                &securityDescriptor);
            if (queryResult != ERROR_SUCCESS)
            {
                content += QStringLiteral("\n深层安全描述符读取失败, code=%1\n").arg(queryResult);
            }
            else
            {
                content += QStringLiteral("\n[Owner]\n");
                content += QStringLiteral("SID: %1\n").arg(sidToStringText(ownerSid));
                content += QStringLiteral("账户: %1\n").arg(sidToAccountText(ownerSid));

                content += QStringLiteral("\n[Primary Group]\n");
                content += QStringLiteral("SID: %1\n").arg(sidToStringText(groupSid));
                content += QStringLiteral("账户: %1\n").arg(sidToAccountText(groupSid));

                appendAclText(QStringLiteral("DACL"), dacl, content);
                ::LocalFree(securityDescriptor);
            }

            // 尝试读取 SACL（审计 ACL），多数场景需要 SeSecurityPrivilege，失败也应输出原因。
            PSID saclOwnerSid = nullptr;
            PSID saclGroupSid = nullptr;
            PACL sacl = nullptr;
            PSECURITY_DESCRIPTOR saclDescriptor = nullptr;
            const DWORD saclResult = ::GetNamedSecurityInfoW(
                nativePathBuffer.data(),
                SE_FILE_OBJECT,
                SACL_SECURITY_INFORMATION,
                &saclOwnerSid,
                &saclGroupSid,
                nullptr,
                &sacl,
                &saclDescriptor);
            if (saclResult == ERROR_SUCCESS)
            {
                appendAclText(QStringLiteral("SACL"), sacl, content);
                ::LocalFree(saclDescriptor);
            }
            else
            {
                content += QStringLiteral("\n[SACL]\n");
                content += QStringLiteral("读取失败（通常需要 SeSecurityPrivilege）, code=%1\n").arg(saclResult);
            }

            content += QStringLiteral("\n说明：Mask 显示为十六进制，权限列为常见位标志拆解。");
            textEditorWidget->setText(content);

            layout->addWidget(textEditorWidget, 1);
            return page;
        }

        QWidget* buildHashTab()
        {
            QWidget* page = new QWidget(this);
            QVBoxLayout* layout = new QVBoxLayout(page);
            CodeEditorWidget* textEditorWidget = new CodeEditorWidget(page);
            textEditorWidget->setReadOnly(true);

            QFile file(m_filePath);
            QString content;
            if (!file.open(QIODevice::ReadOnly))
            {
                content = QStringLiteral("无法打开文件，哈希计算失败。");
                textEditorWidget->setText(content);
                layout->addWidget(textEditorWidget, 1);
                return page;
            }

            QCryptographicHash md5(QCryptographicHash::Md5);
            QCryptographicHash sha1(QCryptographicHash::Sha1);
            QCryptographicHash sha256(QCryptographicHash::Sha256);
            QCryptographicHash sha512(QCryptographicHash::Sha512);

            while (!file.atEnd())
            {
                const QByteArray chunk = file.read(1024 * 256);
                md5.addData(chunk);
                sha1.addData(chunk);
                sha256.addData(chunk);
                sha512.addData(chunk);
            }
            file.close();

            content += QStringLiteral("MD5: %1\n").arg(QString::fromLatin1(md5.result().toHex()));
            content += QStringLiteral("SHA1: %1\n").arg(QString::fromLatin1(sha1.result().toHex()));
            content += QStringLiteral("SHA256: %1\n").arg(QString::fromLatin1(sha256.result().toHex()));
            content += QStringLiteral("SHA512: %1\n").arg(QString::fromLatin1(sha512.result().toHex()));
            textEditorWidget->setText(content);

            layout->addWidget(textEditorWidget, 1);
            return page;
        }

        QWidget* buildSignatureTab()
        {
            QWidget* page = new QWidget(this);
            QVBoxLayout* layout = new QVBoxLayout(page);
            CodeEditorWidget* textEditorWidget = new CodeEditorWidget(page);
            textEditorWidget->setReadOnly(true);

            // 调用 PowerShell 获取 Authenticode 详细信息，提供可审计的签名结果。
            QProcess process;
            process.setProgram(QStringLiteral("powershell.exe"));
            const QString safeFilePath = m_filePath;
            QString escapedPath = safeFilePath;
            escapedPath.replace(QStringLiteral("'"), QStringLiteral("''"));
            const QString scriptText = QStringLiteral(
                "$sig=Get-AuthenticodeSignature -LiteralPath '%1';"
                "$cert=$sig.SignerCertificate;"
                "$subj=if($cert){$cert.Subject}else{'<无证书>'};"
                "$issuer=if($cert){$cert.Issuer}else{'<无证书>'};"
                "$thumb=if($cert){$cert.Thumbprint}else{'<无证书>'};"
                "$from=if($cert){$cert.NotBefore}else{'<无证书>'};"
                "$to=if($cert){$cert.NotAfter}else{'<无证书>'};"
                "$statusMsg=if($sig.StatusMessage){$sig.StatusMessage}else{'<无附加消息>'};"
                "Write-Output ('状态: ' + $sig.Status);"
                "Write-Output ('状态说明: ' + $statusMsg);"
                "Write-Output ('签名者主题: ' + $subj);"
                "Write-Output ('签名者颁发者: ' + $issuer);"
                "Write-Output ('证书指纹: ' + $thumb);"
                "Write-Output ('证书生效: ' + $from);"
                "Write-Output ('证书失效: ' + $to);"
                "Write-Output ('是否含时间戳: ' + ([bool]$sig.TimeStamperCertificate));")
                .arg(escapedPath);
            process.setArguments(QStringList{
                QStringLiteral("-NoProfile"),
                QStringLiteral("-ExecutionPolicy"),
                QStringLiteral("Bypass"),
                QStringLiteral("-Command"),
                scriptText
                });
            process.start();
            process.waitForFinished(15000);

            const QString stdOutText = QString::fromLocal8Bit(process.readAllStandardOutput()).trimmed();
            const QString stdErrText = QString::fromLocal8Bit(process.readAllStandardError()).trimmed();
            if (process.exitStatus() != QProcess::NormalExit || process.exitCode() != 0 || stdOutText.isEmpty())
            {
                QString failText = QStringLiteral("签名检查失败。\n");
                failText += QStringLiteral("退出码: %1\n").arg(process.exitCode());
                if (!stdErrText.isEmpty())
                {
                    failText += QStringLiteral("错误输出:\n%1").arg(stdErrText);
                }
                else
                {
                    failText += QStringLiteral("错误输出为空，可能系统未启用 PowerShell 或文件不可访问。");
                }
                textEditorWidget->setText(failText);
                layout->addWidget(textEditorWidget, 1);
                return page;
            }

            textEditorWidget->setText(stdOutText);
            layout->addWidget(textEditorWidget, 1);
            return page;
        }

        QWidget* buildPeTab()
        {
            QWidget* page = new QWidget(this);
            QVBoxLayout* layout = new QVBoxLayout(page);
            CodeEditorWidget* textEditorWidget = new CodeEditorWidget(page);
            textEditorWidget->setReadOnly(true);

            QFile file(m_filePath);
            QString content;
            if (!file.open(QIODevice::ReadOnly))
            {
                content = QStringLiteral("无法读取文件，无法解析PE信息。");
                textEditorWidget->setText(content);
                layout->addWidget(textEditorWidget, 1);
                return page;
            }

            const QByteArray headerBytes = file.read(4096);
            file.close();
            if (headerBytes.size() < 0x100)
            {
                textEditorWidget->setText(QStringLiteral("文件过小，无法识别PE。"));
                layout->addWidget(textEditorWidget, 1);
                return page;
            }

            const bool isMZ = headerBytes.size() >= 2
                && static_cast<unsigned char>(headerBytes[0]) == 0x4D
                && static_cast<unsigned char>(headerBytes[1]) == 0x5A;
            if (!isMZ)
            {
                textEditorWidget->setText(QStringLiteral("非PE文件（缺少 MZ 标记）。"));
                layout->addWidget(textEditorWidget, 1);
                return page;
            }

            const std::uint32_t peOffset = *reinterpret_cast<const std::uint32_t*>(headerBytes.constData() + 0x3C);
            content += QStringLiteral("文件格式：PE\n");
            content += QStringLiteral("e_lfanew: 0x%1\n").arg(QString::number(peOffset, 16).toUpper());
            if (headerBytes.size() >= static_cast<int>(peOffset + 0x18))
            {
                const std::uint16_t machine = *reinterpret_cast<const std::uint16_t*>(headerBytes.constData() + peOffset + 4);
                const std::uint16_t numberOfSections = *reinterpret_cast<const std::uint16_t*>(headerBytes.constData() + peOffset + 6);
                content += QStringLiteral("Machine: 0x%1\n").arg(QString::number(machine, 16).toUpper());
                content += QStringLiteral("Section数量: %1\n").arg(numberOfSections);
            }
            content += QStringLiteral("\n后续可扩展：导入表/导出表/资源树/区段熵值。");
            textEditorWidget->setText(content);

            layout->addWidget(textEditorWidget, 1);
            return page;
        }

        QWidget* buildStringsTab()
        {
            QWidget* page = new QWidget(this);
            QVBoxLayout* layout = new QVBoxLayout(page);
            CodeEditorWidget* textEditorWidget = new CodeEditorWidget(page);
            textEditorWidget->setReadOnly(true);
            layout->addWidget(textEditorWidget, 1);

            QFile file(m_filePath);
            QString rawStringText;
            if (!file.open(QIODevice::ReadOnly))
            {
                rawStringText = QStringLiteral("无法读取文件，无法提取字符串。");
                textEditorWidget->setText(rawStringText);
                return page;
            }

            const QByteArray bytes = file.readAll();
            file.close();

            QString current;
            QStringList result;
            for (char ch : bytes)
            {
                const unsigned char c = static_cast<unsigned char>(ch);
                if (std::isprint(c) != 0)
                {
                    current.append(QChar::fromLatin1(ch));
                }
                else
                {
                    if (current.length() >= 4)
                    {
                        result.append(current);
                    }
                    current.clear();
                }
                if (result.size() >= 2000)
                {
                    break;
                }
            }
            if (current.length() >= 4 && result.size() < 2000)
            {
                result.append(current);
            }

            rawStringText = result.join('\n');
            if (rawStringText.trimmed().isEmpty())
            {
                rawStringText = QStringLiteral("<未提取到可打印字符串，或文件内容全部为二进制不可见字符。>");
            }
            textEditorWidget->setText(rawStringText);

            return page;
        }

        QWidget* buildHexTab()
        {
            QWidget* page = new QWidget(this);
            QVBoxLayout* layout = new QVBoxLayout(page);

            // 统一复用 HexEditorWidget，避免各处重复实现十六进制转储逻辑。
            HexEditorWidget* hexEditorWidget = new HexEditorWidget(page);
            hexEditorWidget->setEditable(false);
            hexEditorWidget->setBytesPerRow(16);
            layout->addWidget(hexEditorWidget, 1);

            // hexHintLabel 用途：提示用户该页面默认仅预览文件前部字节。
            QLabel* hexHintLabel = new QLabel(page);
            hexHintLabel->setWordWrap(true);
            layout->addWidget(hexHintLabel, 0);

            // 文件详情页只读取前 2MB，防止超大文件导致属性窗口卡顿。
            constexpr qint64 kMaxPreviewBytes = 2 * 1024 * 1024;
            QFile file(m_filePath);
            if (!file.open(QIODevice::ReadOnly))
            {
                hexHintLabel->setText(QStringLiteral("无法读取文件，无法显示十六进制。"));
                hexEditorWidget->clearData();
                return page;
            }

            const qint64 totalBytes = file.size();
            const QByteArray bytes = file.read(kMaxPreviewBytes);
            file.close();

            if (bytes.isEmpty())
            {
                hexHintLabel->setText(QStringLiteral("文件为空。"));
                hexEditorWidget->clearData();
                return page;
            }

            // 直接把预览字节交给 HexEditorWidget，使用统一滚动、查找、跳转能力。
            hexEditorWidget->setByteArray(bytes, 0);

            if (totalBytes > bytes.size())
            {
                hexHintLabel->setText(
                    QStringLiteral("当前仅预览文件前 %1 字节，总大小 %2 字节。")
                    .arg(bytes.size())
                    .arg(totalBytes));
            }
            else
            {
                hexHintLabel->setText(
                    QStringLiteral("已加载完整文件，共 %1 字节。")
                    .arg(totalBytes));
            }

            return page;
        }

    private:
        QString m_filePath;   // 当前详情窗口对应的文件路径。
    };
}

FileDock::FileDock(QWidget* parent)
    : QWidget(parent)
{
    // 构造日志：记录文件模块启动。
    kLogEvent event;
    info << event << "[FileDock] 构造开始，初始化双栏资源管理器。" << eol;

    initializeUi();
}

void FileDock::initializeUi()
{
    m_rootLayout = new QVBoxLayout(this);
    m_rootLayout->setContentsMargins(4, 4, 4, 4);
    m_rootLayout->setSpacing(6);

    // 顶层改为竖排 Tab：文件管理 + 文件恢复。
    m_rootTabWidget = new QTabWidget(this);
    m_rootTabWidget->setTabPosition(QTabWidget::West);
    m_rootTabWidget->setDocumentMode(true);
    m_rootLayout->addWidget(m_rootTabWidget, 1);

    m_fileManagerPage = new QWidget(m_rootTabWidget);
    QVBoxLayout* managerLayout = new QVBoxLayout(m_fileManagerPage);
    managerLayout->setContentsMargins(0, 0, 0, 0);
    managerLayout->setSpacing(0);

    m_mainSplitter = new QSplitter(Qt::Horizontal, m_fileManagerPage);
    managerLayout->addWidget(m_mainSplitter, 1);

    initializePanel(m_leftPanel, QStringLiteral("左侧面板"));
    initializePanel(m_rightPanel, QStringLiteral("右侧面板"));
    m_mainSplitter->addWidget(m_leftPanel.rootWidget);
    m_mainSplitter->addWidget(m_rightPanel.rootWidget);
    m_mainSplitter->setStretchFactor(0, 1);
    m_mainSplitter->setStretchFactor(1, 1);

    m_rootTabWidget->addTab(m_fileManagerPage, QStringLiteral("文件管理"));
    initializeRecoveryPage();
    if (m_fileRecoveryPage != nullptr)
    {
        m_rootTabWidget->addTab(m_fileRecoveryPage, QStringLiteral("文件恢复"));
    }
}

void FileDock::initializePanel(FilePanelWidgets& panel, const QString& titleText)
{
    // 记录面板名称，后续日志统一附带“左侧/右侧”标签便于排障定位。
    panel.panelNameText = titleText;

    {
        kLogEvent event;
        info << event
            << "[FileDock] 开始初始化面板, panel="
            << titleText.toStdString()
            << eol;
    }

    panel.rootWidget = new QWidget(m_mainSplitter);
    panel.rootLayout = new QVBoxLayout(panel.rootWidget);
    panel.rootLayout->setContentsMargins(4, 4, 4, 4);
    panel.rootLayout->setSpacing(4);

    // 标题栏：区分左右面板。
    QLabel* titleLabel = new QLabel(titleText, panel.rootWidget);
    titleLabel->setStyleSheet(QStringLiteral("color:%1;font-weight:700;").arg(KswordTheme::PrimaryBlueHex));
    panel.rootLayout->addWidget(titleLabel, 0);

    panel.navWidget = new QWidget(panel.rootWidget);
    panel.navLayout = new QHBoxLayout(panel.navWidget);
    panel.navLayout->setContentsMargins(0, 0, 0, 0);
    panel.navLayout->setSpacing(4);

    panel.backButton = new QPushButton(QIcon(":/Icon/file_nav_back.svg"), QString(), panel.navWidget);
    panel.backButton->setToolTip(QStringLiteral("后退"));
    panel.backButton->setStyleSheet(buildBlueButtonStyle());
    panel.backButton->setFixedWidth(30);

    panel.forwardButton = new QPushButton(QIcon(":/Icon/file_nav_forward.svg"), QString(), panel.navWidget);
    panel.forwardButton->setToolTip(QStringLiteral("前进"));
    panel.forwardButton->setStyleSheet(buildBlueButtonStyle());
    panel.forwardButton->setFixedWidth(30);

    panel.upButton = new QPushButton(QIcon(":/Icon/file_nav_up.svg"), QString(), panel.navWidget);
    panel.upButton->setToolTip(QStringLiteral("上级目录"));
    panel.upButton->setStyleSheet(buildBlueButtonStyle());
    panel.upButton->setFixedWidth(30);

    panel.refreshButton = new QPushButton(QIcon(":/Icon/process_refresh.svg"), QString(), panel.navWidget);
    panel.refreshButton->setToolTip(QStringLiteral("刷新当前目录"));
    panel.refreshButton->setStyleSheet(buildBlueButtonStyle());
    panel.refreshButton->setFixedWidth(30);

    // 地址区域采用“堆叠控件”：
    // - 面包屑页：默认显示；
    // - 编辑页：点击空白热区后切换，按 Enter 跳转。
    panel.pathStack = new QStackedWidget(panel.navWidget);
    panel.pathStack->setMinimumWidth(260);

    panel.breadcrumbWidget = new QWidget(panel.pathStack);
    panel.breadcrumbWidget->setObjectName(QStringLiteral("EmbeddedBreadcrumbWidget"));
    panel.breadcrumbWidget->setStyleSheet(QStringLiteral(
        "QWidget#EmbeddedBreadcrumbWidget{"
        "  border:1px solid %1;"
        "  border-radius:3px;"
        "  background:%2;"
        "}").arg(KswordTheme::BorderHex(), KswordTheme::SurfaceHex()));
    panel.breadcrumbLayout = new QHBoxLayout(panel.breadcrumbWidget);
    panel.breadcrumbLayout->setContentsMargins(6, 2, 6, 2);
    panel.breadcrumbLayout->setSpacing(2);

    panel.pathEdit = new QLineEdit(panel.pathStack);
    panel.pathEdit->setPlaceholderText(QStringLiteral("输入路径后按回车跳转"));
    panel.pathEdit->setStyleSheet(buildBlueInputStyle());

    panel.pathStack->addWidget(panel.breadcrumbWidget);
    panel.pathStack->addWidget(panel.pathEdit);

    panel.navLayout->addWidget(panel.backButton);
    panel.navLayout->addWidget(panel.forwardButton);
    panel.navLayout->addWidget(panel.upButton);
    panel.navLayout->addWidget(panel.refreshButton);
    panel.navLayout->addWidget(panel.pathStack, 1);
    panel.rootLayout->addWidget(panel.navWidget, 0);

    panel.toolWidget = new QWidget(panel.rootWidget);
    panel.toolLayout = new QHBoxLayout(panel.toolWidget);
    panel.toolLayout->setContentsMargins(0, 0, 0, 0);
    panel.toolLayout->setSpacing(4);

    panel.viewModeCombo = new QComboBox(panel.toolWidget);
    panel.viewModeCombo->setStyleSheet(buildBlueInputStyle());
    panel.viewModeCombo->addItems(QStringList{ QStringLiteral("图标视图"), QStringLiteral("列表视图"), QStringLiteral("详情视图"), QStringLiteral("树形视图") });
    panel.viewModeCombo->setToolTip(QStringLiteral("切换文件显示模式，默认使用详情视图"));
    panel.viewModeCombo->setCurrentIndex(2);

    panel.showSystemCheck = new QCheckBox(QStringLiteral("系统"), panel.toolWidget);
    panel.showHiddenCheck = new QCheckBox(QStringLiteral("隐藏"), panel.toolWidget);
    panel.showSystemCheck->setChecked(true);
    panel.showHiddenCheck->setChecked(true);

    panel.sortModeCombo = new QComboBox(panel.toolWidget);
    panel.sortModeCombo->setStyleSheet(buildBlueInputStyle());
    panel.sortModeCombo->addItems(QStringList{ QStringLiteral("名称"), QStringLiteral("大小"), QStringLiteral("修改时间"), QStringLiteral("类型") });

    panel.readModeCombo = new QComboBox(panel.toolWidget);
    panel.readModeCombo->setStyleSheet(buildBlueInputStyle());
    panel.readModeCombo->addItems(QStringList{ QStringLiteral("Windows API"), QStringLiteral("手动解析文件系统") });
    panel.readModeCombo->setToolTip(QStringLiteral("切换目录读取方式：Windows API 或手动解析 NTFS/FAT32"));

    panel.filterEdit = new QLineEdit(panel.toolWidget);
    panel.filterEdit->setPlaceholderText(QStringLiteral("快速过滤"));
    panel.filterEdit->setStyleSheet(buildBlueInputStyle());

    panel.toolLayout->addWidget(panel.viewModeCombo, 0);
    panel.toolLayout->addWidget(panel.showSystemCheck, 0);
    panel.toolLayout->addWidget(panel.showHiddenCheck, 0);
    panel.toolLayout->addWidget(panel.sortModeCombo, 0);
    panel.toolLayout->addWidget(panel.readModeCombo, 0);
    panel.toolLayout->addWidget(panel.filterEdit, 1);
    panel.rootLayout->addWidget(panel.toolWidget, 0);

    panel.fsModel = new QFileSystemModel(panel.rootWidget);
    panel.fsModel->setReadOnly(false);
    panel.fsModel->setResolveSymlinks(true);
    panel.fsModel->setFilter(QDir::AllEntries | QDir::NoDotAndDotDot);

    panel.proxyModel = new QSortFilterProxyModel(panel.rootWidget);
    panel.proxyModel->setSourceModel(panel.fsModel);
    panel.proxyModel->setFilterCaseSensitivity(Qt::CaseInsensitive);
    panel.proxyModel->setFilterKeyColumn(0);

    panel.manualModel = new QStandardItemModel(panel.rootWidget);
    panel.manualModel->setColumnCount(static_cast<int>(ManualModelColumn::Count));
    panel.manualModel->setHeaderData(static_cast<int>(ManualModelColumn::Name), Qt::Horizontal, QStringLiteral("名称"));
    panel.manualModel->setHeaderData(static_cast<int>(ManualModelColumn::Size), Qt::Horizontal, QStringLiteral("大小"));
    panel.manualModel->setHeaderData(static_cast<int>(ManualModelColumn::Type), Qt::Horizontal, QStringLiteral("类型"));
    panel.manualModel->setHeaderData(static_cast<int>(ManualModelColumn::ModifiedTime), Qt::Horizontal, QStringLiteral("修改时间"));
    panel.manualModel->setHeaderData(static_cast<int>(ManualModelColumn::FullPath), Qt::Horizontal, QStringLiteral("完整路径"));
    panel.manualModel->setHeaderData(static_cast<int>(ManualModelColumn::IsDirectory), Qt::Horizontal, QStringLiteral("目录标记"));

    panel.manualProxyModel = new QSortFilterProxyModel(panel.rootWidget);
    panel.manualProxyModel->setSourceModel(panel.manualModel);
    panel.manualProxyModel->setFilterCaseSensitivity(Qt::CaseInsensitive);
    panel.manualProxyModel->setFilterKeyColumn(static_cast<int>(ManualModelColumn::Name));

    panel.fileView = new QTreeView(panel.rootWidget);
    panel.fileView->setModel(panel.proxyModel);
    panel.fileView->setSelectionMode(QAbstractItemView::ExtendedSelection);
    panel.fileView->setSelectionBehavior(QAbstractItemView::SelectRows);
    panel.fileView->setEditTriggers(QAbstractItemView::EditKeyPressed);
    panel.fileView->setContextMenuPolicy(Qt::CustomContextMenu);
    panel.fileView->setSortingEnabled(true);
    panel.fileView->setDragEnabled(true);
    panel.fileView->setAcceptDrops(true);
    panel.fileView->setDropIndicatorShown(true);
    panel.fileView->setDragDropMode(QAbstractItemView::DragDrop);
    panel.fileView->setDefaultDropAction(Qt::MoveAction);
    panel.fileView->setDragDropOverwriteMode(false);
    panel.fileView->header()->setStretchLastSection(false);
    panel.fileView->header()->setSectionResizeMode(0, QHeaderView::Stretch);
    panel.fileView->header()->setStyleSheet(QStringLiteral("QHeaderView::section{color:%1;}").arg(KswordTheme::PrimaryBlueHex));
    panel.rootLayout->addWidget(panel.fileView, 1);

    panel.statusBar = new QStatusBar(panel.rootWidget);
    panel.pathStatusLabel = new QLabel(QStringLiteral("路径: -"), panel.statusBar);
    panel.selectionStatusLabel = new QLabel(QStringLiteral("选中: 0"), panel.statusBar);
    panel.diskStatusLabel = new QLabel(QStringLiteral("磁盘: -"), panel.statusBar);
    panel.parserStatusLabel = new QLabel(QStringLiteral("解析器: Windows API"), panel.statusBar);
    panel.statusBar->addWidget(panel.pathStatusLabel, 1);
    panel.statusBar->addPermanentWidget(panel.parserStatusLabel, 0);
    panel.statusBar->addPermanentWidget(panel.selectionStatusLabel, 0);
    panel.statusBar->addPermanentWidget(panel.diskStatusLabel, 0);
    panel.rootLayout->addWidget(panel.statusBar, 0);

    // 初始化读取模式并同步模型。
    applyReadModeToPanel(panel);
    initializeConnections(panel);

    // 默认定位到系统根目录。
    const QString defaultPath = QDir::rootPath();
    navigateToPath(panel, defaultPath, true);

    {
        kLogEvent event;
        info << event
            << "[FileDock] 面板初始化完成, panel="
            << panel.panelNameText.toStdString()
            << ", defaultPath="
            << QDir::toNativeSeparators(defaultPath).toStdString()
            << eol;
    }
}

void FileDock::initializeConnections(FilePanelWidgets& panel)
{
    // 返回按钮：回退到上一个历史路径。
    connect(panel.backButton, &QPushButton::clicked, this, [this, &panel]() {
        if (panel.historyIndex <= 0 || panel.history.empty())
        {
            return;
        }

        panel.historyIndex -= 1;
        const QString targetPath = panel.history.at(static_cast<std::size_t>(panel.historyIndex));
        {
            kLogEvent event;
            info << event
                << "[FileDock] 历史后退, panel="
                << panel.panelNameText.toStdString()
                << ", targetPath="
                << QDir::toNativeSeparators(targetPath).toStdString()
                << eol;
        }
        navigateToPath(panel, targetPath, false);
    });

    // 前进按钮：进入历史中的下一个路径。
    connect(panel.forwardButton, &QPushButton::clicked, this, [this, &panel]() {
        if (panel.history.empty())
        {
            return;
        }
        const int nextIndex = panel.historyIndex + 1;
        if (nextIndex < 0 || nextIndex >= static_cast<int>(panel.history.size()))
        {
            return;
        }

        panel.historyIndex = nextIndex;
        const QString targetPath = panel.history.at(static_cast<std::size_t>(panel.historyIndex));
        {
            kLogEvent event;
            info << event
                << "[FileDock] 历史前进, panel="
                << panel.panelNameText.toStdString()
                << ", targetPath="
                << QDir::toNativeSeparators(targetPath).toStdString()
                << eol;
        }
        navigateToPath(panel, targetPath, false);
    });

    // 上级目录按钮：从当前目录切到 parent。
    connect(panel.upButton, &QPushButton::clicked, this, [this, &panel]() {
        if (panel.currentPath.isEmpty())
        {
            return;
        }

        QDir currentDir(panel.currentPath);
        if (!currentDir.cdUp())
        {
            return;
        }

        {
            kLogEvent event;
            info << event
                << "[FileDock] 上级目录跳转, panel="
                << panel.panelNameText.toStdString()
                << ", from="
                << QDir::toNativeSeparators(panel.currentPath).toStdString()
                << ", to="
                << QDir::toNativeSeparators(currentDir.absolutePath()).toStdString()
                << eol;
        }
        navigateToPath(panel, currentDir.absolutePath(), true);
    });

    // 刷新按钮：重新加载当前目录。
    connect(panel.refreshButton, &QPushButton::clicked, this, [this, &panel]() {
        kLogEvent event;
        info << event
            << "[FileDock] 手动刷新目录, panel="
            << panel.panelNameText.toStdString()
            << ", path="
            << QDir::toNativeSeparators(panel.currentPath).toStdString()
            << eol;
        refreshPanel(panel);
    });

    // 地址栏回车：按输入路径导航并自动回到面包屑显示模式。
    connect(panel.pathEdit, &QLineEdit::returnPressed, this, [this, &panel]() {
        const QString targetPath = panel.pathEdit->text().trimmed();
        {
            kLogEvent event;
            info << event
                << "[FileDock] 地址栏回车导航, panel="
                << panel.panelNameText.toStdString()
                << ", input="
                << QDir::toNativeSeparators(targetPath).toStdString()
                << eol;
        }
        navigateToPath(panel, targetPath, true);
        setPathEditMode(panel, false);
    });

    // 编辑完成但未回车时：回退到面包屑，避免长期停留在文本编辑态。
    connect(panel.pathEdit, &QLineEdit::editingFinished, this, [this, &panel]() {
        if (!panel.pathEditMode)
        {
            return;
        }
        if (panel.pathEdit->hasFocus())
        {
            return;
        }
        panel.pathEdit->setText(QDir::toNativeSeparators(panel.currentPath));
        setPathEditMode(panel, false);
    });

    // ESC：取消路径编辑并恢复当前路径文本。
    QShortcut* cancelPathEditShortcut = new QShortcut(QKeySequence(Qt::Key_Escape), panel.pathEdit);
    connect(cancelPathEditShortcut, &QShortcut::activated, this, [this, &panel]() {
        panel.pathEdit->setText(QDir::toNativeSeparators(panel.currentPath));
        setPathEditMode(panel, false);
        kLogEvent event;
        dbg << event
            << "[FileDock] 取消路径编辑, panel="
            << panel.panelNameText.toStdString()
            << eol;
    });

    // 视图切换：根据当前模式调整列显示与图标大小。
    connect(panel.viewModeCombo, &QComboBox::currentIndexChanged, this, [this, &panel](int) {
        applyPanelFilterAndSort(panel);
    });

    // 系统文件显隐切换。
    connect(panel.showSystemCheck, &QCheckBox::toggled, this, [this, &panel](bool) {
        applyPanelFilterAndSort(panel);
    });

    // 隐藏文件显隐切换。
    connect(panel.showHiddenCheck, &QCheckBox::toggled, this, [this, &panel](bool) {
        applyPanelFilterAndSort(panel);
    });

    // 排序模式切换。
    connect(panel.sortModeCombo, &QComboBox::currentIndexChanged, this, [this, &panel](int) {
        applyPanelFilterAndSort(panel);
    });

    // 读取模式切换：Windows API 与手动解析模型实时切换。
    connect(panel.readModeCombo, &QComboBox::currentIndexChanged, this, [this, &panel](int) {
        applyReadModeToPanel(panel);
        refreshPanel(panel);
    });

    // 快速过滤输入：实时更新代理模型。
    connect(panel.filterEdit, &QLineEdit::textChanged, this, [this, &panel](const QString&) {
        applyPanelFilterAndSort(panel);
    });

    // 双击打开：目录进入，文件交给系统默认程序。
    connect(panel.fileView, &QTreeView::doubleClicked, this, [this, &panel](const QModelIndex& proxyIndex) {
        if (!proxyIndex.isValid())
        {
            return;
        }

        const QString path = currentIndexPath(panel);
        if (path.isEmpty())
        {
            return;
        }

        QFileInfo info(path);
        if (info.isDir())
        {
            navigateToPath(panel, path, true);
            return;
        }

        QDesktopServices::openUrl(QUrl::fromLocalFile(path));
    });

    // 右键菜单入口。
    connect(panel.fileView, &QTreeView::customContextMenuRequested, this, [this, &panel](const QPoint& pos) {
        showPanelContextMenu(panel, pos);
    });

    // 选中变化时刷新状态栏。
    connect(panel.fileView->selectionModel(), &QItemSelectionModel::selectionChanged, this, [this, &panel](const QItemSelection&, const QItemSelection&) {
        updatePanelStatus(panel);
    });

    // 模型目录加载完成后更新状态栏，提示当前目录数据可见。
    connect(panel.fsModel, &QFileSystemModel::directoryLoaded, this, [this, &panel](const QString&) {
        updatePanelStatus(panel);
    });

    // Alt+D：快速切换到路径编辑模式，行为与常见文件管理器保持一致。
    QShortcut* editPathShortcut = new QShortcut(QKeySequence(Qt::ALT | Qt::Key_D), panel.rootWidget);
    connect(editPathShortcut, &QShortcut::activated, this, [this, &panel]() {
        setPathEditMode(panel, true);
    });

    // Enter 快捷键：打开选中项。
    QShortcut* openShortcut = new QShortcut(QKeySequence(Qt::Key_Return), panel.fileView);
    connect(openShortcut, &QShortcut::activated, this, [this, &panel]() {
        openSelectedItems(panel);
    });
    QShortcut* openShortcutEnter = new QShortcut(QKeySequence(Qt::Key_Enter), panel.fileView);
    connect(openShortcutEnter, &QShortcut::activated, this, [this, &panel]() {
        openSelectedItems(panel);
    });

    // F2 重命名快捷键。
    QShortcut* renameShortcut = new QShortcut(QKeySequence(Qt::Key_F2), panel.fileView);
    connect(renameShortcut, &QShortcut::activated, this, [this, &panel]() {
        renameSelectedItem(panel);
    });

    // Delete 删除快捷键。
    QShortcut* deleteShortcut = new QShortcut(QKeySequence(Qt::Key_Delete), panel.fileView);
    connect(deleteShortcut, &QShortcut::activated, this, [this, &panel]() {
        deleteSelectedItem(panel);
    });

    // Ctrl+C/Ctrl+X/Ctrl+V 常用文件操作快捷键。
    QShortcut* copyShortcut = new QShortcut(QKeySequence::Copy, panel.fileView);
    connect(copyShortcut, &QShortcut::activated, this, [this, &panel]() {
        copySelectedItems(panel);
    });
    QShortcut* cutShortcut = new QShortcut(QKeySequence::Cut, panel.fileView);
    connect(cutShortcut, &QShortcut::activated, this, [this, &panel]() {
        cutSelectedItems(panel);
    });
    QShortcut* pasteShortcut = new QShortcut(QKeySequence::Paste, panel.fileView);
    connect(pasteShortcut, &QShortcut::activated, this, [this, &panel]() {
        pasteClipboardItems(panel);
    });
}

void FileDock::navigateToPath(FilePanelWidgets& panel, const QString& pathText, bool recordHistory)
{
    {
        kLogEvent event;
        info << event
            << "[FileDock] 导航请求, panel="
            << panel.panelNameText.toStdString()
            << ", input="
            << QDir::toNativeSeparators(pathText).toStdString()
            << ", recordHistory="
            << (recordHistory ? "true" : "false")
            << eol;
    }

    // 去除空白并标准化路径格式，避免历史里混入重复写法。
    const QString trimmedPath = pathText.trimmed();
    if (trimmedPath.isEmpty())
    {
        kLogEvent event;
        warn << event
            << "[FileDock] 导航取消：输入路径为空, panel="
            << panel.panelNameText.toStdString()
            << eol;
        return;
    }

    const QString normalizedPath = QDir::cleanPath(QDir::fromNativeSeparators(trimmedPath));
    QDir targetDir(normalizedPath);
    if (!targetDir.exists())
    {
        kLogEvent event;
        warn << event << "[FileDock] 导航失败，目录不存在: " << normalizedPath.toStdString() << eol;
        QMessageBox::warning(this, QStringLiteral("路径无效"), QStringLiteral("目录不存在：%1").arg(normalizedPath));
        return;
    }

    // 根据当前读取模式更新模型根路径。
    if (currentModeIsManual(panel))
    {
        // 手动解析模式使用平铺模型，根索引固定为无效索引。
        panel.fileView->setRootIndex(QModelIndex());
    }
    else
    {
        const QModelIndex sourceRootIndex = panel.fsModel->setRootPath(normalizedPath);
        const QModelIndex proxyRootIndex = panel.proxyModel->mapFromSource(sourceRootIndex);
        panel.fileView->setRootIndex(proxyRootIndex);
    }
    panel.currentPath = normalizedPath;
    panel.pathEdit->setText(QDir::toNativeSeparators(normalizedPath));

    // 记录历史：当用户主动导航时清理“前进分支”再追加。
    if (recordHistory)
    {
        if (panel.historyIndex + 1 < static_cast<int>(panel.history.size()))
        {
            panel.history.erase(
                panel.history.begin() + panel.historyIndex + 1,
                panel.history.end());
        }

        if (panel.history.empty() || panel.history.back() != normalizedPath)
        {
            panel.history.push_back(normalizedPath);
            panel.historyIndex = static_cast<int>(panel.history.size()) - 1;
        }
        else
        {
            panel.historyIndex = static_cast<int>(panel.history.size()) - 1;
        }
    }

    // 同步按钮可用性状态。
    const bool canGoBack = panel.historyIndex > 0;
    const bool canGoForward = panel.historyIndex >= 0
        && (panel.historyIndex + 1) < static_cast<int>(panel.history.size());
    panel.backButton->setEnabled(canGoBack);
    panel.forwardButton->setEnabled(canGoForward);

    // 导航后更新面包屑、过滤排序和状态栏。
    rebuildBreadcrumb(panel);
    setPathEditMode(panel, false);
    applyPanelFilterAndSort(panel);
    updatePanelStatus(panel);

    {
        kLogEvent event;
        info << event
            << "[FileDock] 导航成功, panel="
            << panel.panelNameText.toStdString()
            << ", normalizedPath="
            << QDir::toNativeSeparators(normalizedPath).toStdString()
            << ", historySize="
            << panel.history.size()
            << ", historyIndex="
            << panel.historyIndex
            << eol;
    }
}

void FileDock::refreshPanel(FilePanelWidgets& panel)
{
    {
        kLogEvent event;
        dbg << event
            << "[FileDock] 刷新面板, panel="
            << panel.panelNameText.toStdString()
            << ", currentPath="
            << QDir::toNativeSeparators(panel.currentPath).toStdString()
            << eol;
    }

    // 没有当前目录时回到系统根路径，保证面板始终可用。
    if (panel.currentPath.isEmpty())
    {
        navigateToPath(panel, QDir::rootPath(), true);
        return;
    }

    // 复用导航逻辑触发模型重载，不写历史避免污染。
    if (currentModeIsManual(panel))
    {
        panel.manualLoadedPath.clear();
    }
    navigateToPath(panel, panel.currentPath, false);
}

void FileDock::rebuildBreadcrumb(FilePanelWidgets& panel)
{
    if (panel.breadcrumbLayout == nullptr)
    {
        return;
    }

    {
        kLogEvent event;
        dbg << event
            << "[FileDock] 重建面包屑, panel="
            << panel.panelNameText.toStdString()
            << ", path="
            << QDir::toNativeSeparators(panel.currentPath).toStdString()
            << eol;
    }

    // 清理旧的面包屑按钮和分隔符，防止布局叠加。
    while (QLayoutItem* item = panel.breadcrumbLayout->takeAt(0))
    {
        if (QWidget* widget = item->widget())
        {
            widget->deleteLater();
        }
        delete item;
    }

    const QString nativePath = QDir::toNativeSeparators(panel.currentPath);
    if (nativePath.isEmpty())
    {
        return;
    }

    int crumbButtonCount = 0;
    QStringList pathParts = nativePath.split(QDir::separator(), Qt::SkipEmptyParts);
    QString runningPath;

    // Windows 驱动器路径（如 C:\）单独处理，保证首段可点击。
    if (nativePath.contains(':'))
    {
        const int colonIndex = nativePath.indexOf(':');
        if (colonIndex >= 0)
        {
            runningPath = nativePath.left(colonIndex + 1) + QDir::separator();
            QString driveText = runningPath;
            driveText.chop(1);

            QToolButton* driveButton = new QToolButton(panel.breadcrumbWidget);
            driveButton->setText(driveText);
            driveButton->setStyleSheet(buildBreadcrumbButtonStyle());
            driveButton->setToolTip(QStringLiteral("跳转到 %1").arg(driveText));
            panel.breadcrumbLayout->addWidget(driveButton, 0);
            crumbButtonCount += 1;
            connect(driveButton, &QToolButton::clicked, this, [this, &panel, runningPath]() {
                kLogEvent event;
                info << event
                    << "[FileDock] 面包屑跳转(盘符), panel="
                    << panel.panelNameText.toStdString()
                    << ", targetPath="
                    << QDir::toNativeSeparators(runningPath).toStdString()
                    << eol;
                navigateToPath(panel, runningPath, true);
            });

            if (!pathParts.isEmpty() && pathParts.front().contains(':'))
            {
                pathParts.removeFirst();
            }
        }
    }
    else if (nativePath.startsWith(QDir::separator()))
    {
        runningPath = QString(QDir::separator());
        QToolButton* rootButton = new QToolButton(panel.breadcrumbWidget);
        rootButton->setText(QStringLiteral("/"));
        rootButton->setStyleSheet(buildBreadcrumbButtonStyle());
        rootButton->setToolTip(QStringLiteral("跳转到根目录"));
        panel.breadcrumbLayout->addWidget(rootButton, 0);
        crumbButtonCount += 1;
        connect(rootButton, &QToolButton::clicked, this, [this, &panel]() {
            kLogEvent event;
            info << event
                << "[FileDock] 面包屑跳转(根目录), panel="
                << panel.panelNameText.toStdString()
                << eol;
            navigateToPath(panel, QString(QDir::separator()), true);
        });
    }

    // 逐段创建路径按钮，支持点击任意层级跳转。
    for (int i = 0; i < pathParts.size(); ++i)
    {
        const QString& part = pathParts.at(i);
        if (part.isEmpty())
        {
            continue;
        }

        if (!runningPath.isEmpty() && !runningPath.endsWith(QDir::separator()))
        {
            runningPath += QDir::separator();
        }
        runningPath += part;

        QLabel* sepLabel = new QLabel(QStringLiteral(">"), panel.breadcrumbWidget);
        sepLabel->setStyleSheet(QStringLiteral("color:%1;").arg(KswordTheme::PrimaryBlueHex));
        panel.breadcrumbLayout->addWidget(sepLabel, 0);

        const QString capturePath = runningPath;
        QToolButton* partButton = new QToolButton(panel.breadcrumbWidget);
        partButton->setText(part);
        partButton->setStyleSheet(buildBreadcrumbButtonStyle());
        partButton->setToolTip(QStringLiteral("跳转到 %1").arg(capturePath));
        panel.breadcrumbLayout->addWidget(partButton, 0);
        crumbButtonCount += 1;
        connect(partButton, &QToolButton::clicked, this, [this, &panel, capturePath]() {
            kLogEvent event;
            info << event
                << "[FileDock] 面包屑跳转(路径段), panel="
                << panel.panelNameText.toStdString()
                << ", targetPath="
                << QDir::toNativeSeparators(capturePath).toStdString()
                << eol;
            navigateToPath(panel, capturePath, true);
        });
    }

    // 面包屑末尾添加“透明热区”：
    // - 点击路径按钮=按段回退；
    // - 点击空白区域=切换到文本编辑模式。
    panel.breadcrumbEditTriggerButton = new QPushButton(panel.breadcrumbWidget);
    panel.breadcrumbEditTriggerButton->setFlat(true);
    panel.breadcrumbEditTriggerButton->setCursor(Qt::IBeamCursor);
    panel.breadcrumbEditTriggerButton->setToolTip(QStringLiteral("点击空白区域编辑路径"));
    panel.breadcrumbEditTriggerButton->setStyleSheet(QStringLiteral(
        "QPushButton{border:none;background:transparent;}"
        "QPushButton:hover{background:rgba(46,139,255,0.22);}"));
    panel.breadcrumbLayout->addWidget(panel.breadcrumbEditTriggerButton, 1);
    connect(panel.breadcrumbEditTriggerButton, &QPushButton::clicked, this, [this, &panel]() {
        kLogEvent event;
        info << event
            << "[FileDock] 点击面包屑空白区进入路径编辑, panel="
            << panel.panelNameText.toStdString()
            << eol;
        setPathEditMode(panel, true);
    });

    {
        kLogEvent event;
        dbg << event
            << "[FileDock] 面包屑重建完成, panel="
            << panel.panelNameText.toStdString()
            << ", breadcrumbButtonCount="
            << crumbButtonCount
            << eol;
    }
}

void FileDock::setPathEditMode(FilePanelWidgets& panel, bool editMode)
{
    if (panel.pathStack == nullptr || panel.pathEdit == nullptr || panel.breadcrumbWidget == nullptr)
    {
        return;
    }

    if (panel.pathEditMode == editMode)
    {
        return;
    }

    panel.pathEditMode = editMode;
    if (editMode)
    {
        panel.pathStack->setCurrentWidget(panel.pathEdit);
        panel.pathEdit->setText(QDir::toNativeSeparators(panel.currentPath));
        panel.pathEdit->setFocus();
        panel.pathEdit->selectAll();
    }
    else
    {
        panel.pathStack->setCurrentWidget(panel.breadcrumbWidget);
        panel.pathEdit->clearFocus();
    }

    kLogEvent event;
    dbg << event
        << "[FileDock] 地址栏显示模式切换, panel="
        << panel.panelNameText.toStdString()
        << ", mode="
        << (editMode ? "edit" : "breadcrumb")
        << eol;
}

void FileDock::updatePanelStatus(FilePanelWidgets& panel)
{
    // 路径状态：直接显示当前目录。
    panel.pathStatusLabel->setText(QStringLiteral("路径: %1").arg(QDir::toNativeSeparators(panel.currentPath)));

    // 统计选中项数量与总大小（文件夹大小不做递归统计，避免卡顿）。
    const std::vector<QString> selectedItemPaths = selectedPaths(panel);
    std::uint64_t totalSize = 0;
    for (const QString& path : selectedItemPaths)
    {
        QFileInfo info(path);
        if (info.isFile())
        {
            totalSize += static_cast<std::uint64_t>(std::max<qint64>(0, info.size()));
        }
    }

    QString attributeHint;
    if (selectedItemPaths.size() == 1)
    {
        QFileInfo info(selectedItemPaths.front());
        QStringList attrs;
        if (!info.isWritable())
        {
            attrs.push_back(QStringLiteral("只读"));
        }
        if (info.isHidden())
        {
            attrs.push_back(QStringLiteral("隐藏"));
        }
        if (info.isSymLink())
        {
            attrs.push_back(QStringLiteral("链接"));
        }
        if (!attrs.isEmpty())
        {
            attributeHint = QStringLiteral(" [%1]").arg(attrs.join(','));
        }
    }

    panel.selectionStatusLabel->setText(
        QStringLiteral("选中: %1  大小: %2%3")
        .arg(selectedItemPaths.size())
        .arg(formatSizeText(totalSize))
        .arg(attributeHint));

    // 磁盘状态：显示当前分区剩余空间。
    const QStorageInfo storageInfo(panel.currentPath);
    if (storageInfo.isValid() && storageInfo.isReady())
    {
        panel.diskStatusLabel->setText(
            QStringLiteral("剩余: %1 / 总计: %2")
            .arg(formatSizeText(static_cast<std::uint64_t>(storageInfo.bytesAvailable())))
            .arg(formatSizeText(static_cast<std::uint64_t>(storageInfo.bytesTotal()))));
    }
    else
    {
        panel.diskStatusLabel->setText(QStringLiteral("磁盘: -"));
    }

    // 状态日志去重：只有内容变化时输出，避免选区抖动造成日志风暴。
    const QString statusSignature = QStringLiteral("%1|%2|%3|%4")
        .arg(panel.currentPath)
        .arg(selectedItemPaths.size())
        .arg(static_cast<qulonglong>(totalSize))
        .arg(panel.diskStatusLabel->text());
    if (statusSignature != panel.lastStatusLogSignature)
    {
        panel.lastStatusLogSignature = statusSignature;
        kLogEvent event;
        dbg << event
            << "[FileDock] 状态栏更新, panel="
            << panel.panelNameText.toStdString()
            << ", selectedCount="
            << selectedItemPaths.size()
            << ", selectedBytes="
            << static_cast<qulonglong>(totalSize)
            << ", path="
            << QDir::toNativeSeparators(panel.currentPath).toStdString()
            << eol;
    }
}

void FileDock::applyPanelFilterAndSort(FilePanelWidgets& panel)
{
    const bool manualMode = currentModeIsManual(panel);
    const int modeIndex = panel.viewModeCombo->currentIndex();
    const QString filterText = panel.filterEdit->text().trimmed();

    if (manualMode)
    {
        if (panel.manualLoadedPath.compare(panel.currentPath, Qt::CaseInsensitive) != 0)
        {
            reloadManualModel(panel, false);
        }

        if (filterText.isEmpty())
        {
            panel.manualProxyModel->setFilterRegularExpression(QRegularExpression());
        }
        else
        {
            const QString pattern = QStringLiteral(".*%1.*").arg(QRegularExpression::escape(filterText));
            panel.manualProxyModel->setFilterRegularExpression(
                QRegularExpression(pattern, QRegularExpression::CaseInsensitiveOption));
        }

        int sortColumn = static_cast<int>(ManualModelColumn::Name);
        switch (panel.sortModeCombo->currentIndex())
        {
        case 1:
            sortColumn = static_cast<int>(ManualModelColumn::Size);
            break;
        case 2:
            sortColumn = static_cast<int>(ManualModelColumn::ModifiedTime);
            break;
        case 3:
            sortColumn = static_cast<int>(ManualModelColumn::Type);
            break;
        default:
            sortColumn = static_cast<int>(ManualModelColumn::Name);
            break;
        }
        panel.fileView->sortByColumn(sortColumn, Qt::AscendingOrder);

        const bool showDetailColumns = (modeIndex == 2 || modeIndex == 3);
        panel.fileView->setIconSize(modeIndex == 0 ? QSize(32, 32) : QSize(18, 18));
        panel.fileView->setRootIsDecorated(false);
        panel.fileView->setItemsExpandable(false);
        panel.fileView->setIndentation(10);

        panel.fileView->setColumnHidden(static_cast<int>(ManualModelColumn::Size), !showDetailColumns);
        panel.fileView->setColumnHidden(static_cast<int>(ManualModelColumn::Type), !showDetailColumns);
        panel.fileView->setColumnHidden(static_cast<int>(ManualModelColumn::ModifiedTime), !showDetailColumns);
        panel.fileView->setColumnHidden(static_cast<int>(ManualModelColumn::FullPath), true);
        panel.fileView->setColumnHidden(static_cast<int>(ManualModelColumn::IsDirectory), true);
    }
    else
    {
        // 组合模型过滤标志：按用户勾选决定是否显示隐藏/系统文件。
        QDir::Filters filters = QDir::AllEntries | QDir::NoDotAndDotDot;
        if (panel.showHiddenCheck->isChecked())
        {
            filters |= QDir::Hidden;
        }
        if (panel.showSystemCheck->isChecked())
        {
            filters |= QDir::System;
        }
        panel.fsModel->setFilter(filters);

        if (filterText.isEmpty())
        {
            panel.proxyModel->setFilterRegularExpression(QRegularExpression());
        }
        else
        {
            const QString pattern = QStringLiteral(".*%1.*").arg(QRegularExpression::escape(filterText));
            panel.proxyModel->setFilterRegularExpression(
                QRegularExpression(pattern, QRegularExpression::CaseInsensitiveOption));
        }

        int sortColumn = 0;
        switch (panel.sortModeCombo->currentIndex())
        {
        case 1:
            sortColumn = 1;
            break;
        case 2:
            sortColumn = 3;
            break;
        case 3:
            sortColumn = 2;
            break;
        default:
            sortColumn = 0;
            break;
        }
        panel.fileView->sortByColumn(sortColumn, Qt::AscendingOrder);

        const bool showDetailColumns = (modeIndex == 2 || modeIndex == 3);
        panel.fileView->setIconSize(modeIndex == 0 ? QSize(32, 32) : QSize(18, 18));
        panel.fileView->setRootIsDecorated(modeIndex == 3);
        panel.fileView->setItemsExpandable(modeIndex == 3);
        panel.fileView->setIndentation(modeIndex == 3 ? 18 : 10);
        for (int column = 1; column < panel.fsModel->columnCount(); ++column)
        {
            panel.fileView->setColumnHidden(column, !showDetailColumns);
        }
        if (modeIndex == 1)
        {
            panel.fileView->setRootIsDecorated(false);
            panel.fileView->setItemsExpandable(false);
        }
        if (panel.parserStatusLabel != nullptr)
        {
            panel.parserStatusLabel->setText(QStringLiteral("解析器: Windows API"));
        }
    }

    // 过滤参数日志去重：仅在用户真实调整条件时输出详细参数。
    const QString filterSignature = QStringLiteral("%1|%2|%3|%4|%5|%6")
        .arg(panel.showHiddenCheck->isChecked() ? 1 : 0)
        .arg(panel.showSystemCheck->isChecked() ? 1 : 0)
        .arg(panel.sortModeCombo->currentIndex())
        .arg(panel.viewModeCombo->currentIndex())
        .arg(panel.readModeCombo->currentIndex())
        .arg(panel.filterEdit->text());
    if (filterSignature != panel.lastFilterLogSignature)
    {
        panel.lastFilterLogSignature = filterSignature;
        kLogEvent event;
        info << event
            << "[FileDock] 过滤/排序参数变更, panel="
            << panel.panelNameText.toStdString()
            << ", showHidden="
            << (panel.showHiddenCheck->isChecked() ? "true" : "false")
            << ", showSystem="
            << (panel.showSystemCheck->isChecked() ? "true" : "false")
            << ", sortModeIndex="
            << panel.sortModeCombo->currentIndex()
            << ", viewModeIndex="
            << panel.viewModeCombo->currentIndex()
            << ", readModeIndex="
            << panel.readModeCombo->currentIndex()
            << ", keyword="
            << panel.filterEdit->text().toStdString()
            << eol;
    }

    updatePanelStatus(panel);
}

void FileDock::applyReadModeToPanel(FilePanelWidgets& panel)
{
    if (panel.fileView == nullptr)
    {
        return;
    }

    if (currentModeIsManual(panel))
    {
        panel.fileView->setModel(panel.manualProxyModel);
        panel.fileView->setRootIndex(QModelIndex());
        panel.showHiddenCheck->setEnabled(false);
        panel.showSystemCheck->setEnabled(false);
        if (panel.parserStatusLabel != nullptr)
        {
            panel.parserStatusLabel->setText(QStringLiteral("解析器: 手动解析"));
        }
    }
    else
    {
        panel.fileView->setModel(panel.proxyModel);
        panel.showHiddenCheck->setEnabled(true);
        panel.showSystemCheck->setEnabled(true);
        if (!panel.currentPath.isEmpty())
        {
            const QModelIndex sourceRootIndex = panel.fsModel->setRootPath(panel.currentPath);
            const QModelIndex proxyRootIndex = panel.proxyModel->mapFromSource(sourceRootIndex);
            panel.fileView->setRootIndex(proxyRootIndex);
        }
        if (panel.parserStatusLabel != nullptr)
        {
            panel.parserStatusLabel->setText(QStringLiteral("解析器: Windows API"));
        }
    }

    panel.fileView->header()->setStretchLastSection(false);
    panel.fileView->header()->setSectionResizeMode(0, QHeaderView::Stretch);
    QItemSelectionModel* selectionModel = panel.fileView->selectionModel();
    if (selectionModel != nullptr)
    {
        QObject::disconnect(selectionModel, nullptr, this, nullptr);
        connect(selectionModel, &QItemSelectionModel::selectionChanged, this, [this, &panel](const QItemSelection&, const QItemSelection&) {
            updatePanelStatus(panel);
        });
    }
}

bool FileDock::reloadManualModel(FilePanelWidgets& panel, const bool showWarningMessage)
{
    if (panel.manualModel == nullptr || panel.currentPath.isEmpty())
    {
        return false;
    }

    std::vector<ks::file::ManualDirectoryEntry> entries;
    ks::file::ManualFsType fsType = ks::file::ManualFsType::Unknown;
    QString errorText;
    const bool parseOk = ks::file::ManualFileSystemParser::enumerateDirectory(
        panel.currentPath,
        entries,
        fsType,
        errorText);

    panel.manualModel->removeRows(0, panel.manualModel->rowCount());
    panel.lastManualFsType = fsType;
    if (!parseOk)
    {
        panel.manualLoadedPath.clear();
        if (panel.parserStatusLabel != nullptr)
        {
            panel.parserStatusLabel->setText(QStringLiteral("解析器: 手动解析失败"));
        }
        if (showWarningMessage)
        {
            QMessageBox::warning(
                this,
                QStringLiteral("手动解析失败"),
                QStringLiteral("路径: %1\n错误: %2")
                .arg(QDir::toNativeSeparators(panel.currentPath))
                .arg(errorText));
        }

        kLogEvent event;
        warn << event
            << "[FileDock] 手动解析失败, panel="
            << panel.panelNameText.toStdString()
            << ", path="
            << QDir::toNativeSeparators(panel.currentPath).toStdString()
            << ", error="
            << errorText.toStdString()
            << eol;
        return false;
    }

    for (const ks::file::ManualDirectoryEntry& itemValue : entries)
    {
        QList<QStandardItem*> rowItems;
        rowItems.reserve(static_cast<int>(ManualModelColumn::Count));

        QStandardItem* nameItem = new QStandardItem(itemValue.name);
        nameItem->setData(itemValue.absolutePath, Qt::UserRole);
        nameItem->setData(itemValue.isDirectory, Qt::UserRole + 1);
        rowItems.push_back(nameItem);

        QStandardItem* sizeItem = new QStandardItem(itemValue.isDirectory ? QStringLiteral("-") : formatSizeText(itemValue.sizeBytes));
        sizeItem->setData(static_cast<qulonglong>(itemValue.sizeBytes), Qt::UserRole);
        rowItems.push_back(sizeItem);

        rowItems.push_back(new QStandardItem(itemValue.typeText));
        rowItems.push_back(new QStandardItem(itemValue.modifiedTime.isValid()
            ? itemValue.modifiedTime.toString(QStringLiteral("yyyy-MM-dd HH:mm:ss"))
            : QStringLiteral("-")));
        rowItems.push_back(new QStandardItem(QDir::toNativeSeparators(itemValue.absolutePath)));
        rowItems.push_back(new QStandardItem(itemValue.isDirectory ? QStringLiteral("1") : QStringLiteral("0")));
        panel.manualModel->appendRow(rowItems);
    }

    if (panel.parserStatusLabel != nullptr)
    {
        panel.parserStatusLabel->setText(
            QStringLiteral("解析器: %1 (手动)")
            .arg(manualFsTypeToText(fsType)));
    }
    panel.manualLoadedPath = panel.currentPath;

    kLogEvent event;
    info << event
        << "[FileDock] 手动解析完成, panel="
        << panel.panelNameText.toStdString()
        << ", fsType="
        << manualFsTypeToText(fsType).toStdString()
        << ", rows="
        << entries.size()
        << ", path="
        << QDir::toNativeSeparators(panel.currentPath).toStdString()
        << eol;
    return true;
}

bool FileDock::currentModeIsManual(const FilePanelWidgets& panel) const
{
    return panel.readModeCombo != nullptr && panel.readModeCombo->currentIndex() == 1;
}

void FileDock::initializeRecoveryPage()
{
    m_fileRecoveryPage = new QWidget(m_rootTabWidget);
    QVBoxLayout* recoveryLayout = new QVBoxLayout(m_fileRecoveryPage);
    recoveryLayout->setContentsMargins(6, 6, 6, 6);
    recoveryLayout->setSpacing(6);

    QWidget* toolWidget = new QWidget(m_fileRecoveryPage);
    QHBoxLayout* toolLayout = new QHBoxLayout(toolWidget);
    toolLayout->setContentsMargins(0, 0, 0, 0);
    toolLayout->setSpacing(6);

    m_recoveryVolumeCombo = new QComboBox(toolWidget);
    m_recoveryVolumeCombo->setStyleSheet(buildBlueInputStyle());
    m_recoveryVolumeCombo->setToolTip(QStringLiteral("选择要扫描误删文件的 NTFS 卷。"));

    m_recoveryRefreshButton = new QPushButton(QIcon(":/Icon/process_refresh.svg"), QString(), toolWidget);
    m_recoveryRefreshButton->setToolTip(QStringLiteral("刷新可扫描卷列表"));
    m_recoveryRefreshButton->setStyleSheet(buildBlueButtonStyle());
    m_recoveryRefreshButton->setFixedWidth(30);

    m_recoveryScanButton = new QPushButton(QIcon(":/Icon/log_track.svg"), QStringLiteral("扫描误删"), toolWidget);
    m_recoveryScanButton->setToolTip(QStringLiteral("解析 NTFS MFT，扫描删除项"));
    m_recoveryScanButton->setStyleSheet(buildBlueButtonStyle());

    m_recoveryExportButton = new QPushButton(QIcon(":/Icon/log_export.svg"), QStringLiteral("恢复选中"), toolWidget);
    m_recoveryExportButton->setToolTip(QStringLiteral("导出选中删除项（当前仅支持 resident 数据）"));
    m_recoveryExportButton->setStyleSheet(buildBlueButtonStyle());

    toolLayout->addWidget(new QLabel(QStringLiteral("卷: "), toolWidget), 0);
    toolLayout->addWidget(m_recoveryVolumeCombo, 1);
    toolLayout->addWidget(m_recoveryRefreshButton, 0);
    toolLayout->addWidget(m_recoveryScanButton, 0);
    toolLayout->addWidget(m_recoveryExportButton, 0);
    recoveryLayout->addWidget(toolWidget, 0);

    m_recoveryTable = new QTableWidget(m_fileRecoveryPage);
    m_recoveryTable->setColumnCount(6);
    m_recoveryTable->setHorizontalHeaderLabels(QStringList{
        QStringLiteral("文件名"),
        QStringLiteral("路径提示"),
        QStringLiteral("大小"),
        QStringLiteral("修改时间"),
        QStringLiteral("记录号"),
        QStringLiteral("恢复能力")
        });
    m_recoveryTable->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_recoveryTable->setSelectionMode(QAbstractItemView::ExtendedSelection);
    m_recoveryTable->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_recoveryTable->verticalHeader()->setVisible(false);
    m_recoveryTable->horizontalHeader()->setStretchLastSection(true);
    m_recoveryTable->setAlternatingRowColors(true);
    recoveryLayout->addWidget(m_recoveryTable, 1);

    m_recoveryStatusLabel = new QLabel(QStringLiteral("请选择NTFS卷并开始扫描。"), m_fileRecoveryPage);
    recoveryLayout->addWidget(m_recoveryStatusLabel, 0);

    connect(m_recoveryRefreshButton, &QPushButton::clicked, this, [this]() {
        refreshRecoveryVolumeList();
    });
    connect(m_recoveryScanButton, &QPushButton::clicked, this, [this]() {
        scanDeletedFilesForRecovery();
    });
    connect(m_recoveryExportButton, &QPushButton::clicked, this, [this]() {
        recoverSelectedDeletedFiles();
    });

    refreshRecoveryVolumeList();
}

void FileDock::refreshRecoveryVolumeList()
{
    if (m_recoveryVolumeCombo == nullptr)
    {
        return;
    }

    m_recoveryVolumeCombo->clear();
    const QFileInfoList driveList = QDir::drives();
    for (const QFileInfo& driveInfo : driveList)
    {
        const QString rootPath = QDir::toNativeSeparators(driveInfo.absoluteFilePath());
        const ks::file::ManualFsType fsType = ks::file::ManualFileSystemParser::detectFileSystemType(rootPath);
        if (fsType != ks::file::ManualFsType::Ntfs)
        {
            continue;
        }

        const QString displayText = QStringLiteral("%1 (NTFS)").arg(rootPath);
        m_recoveryVolumeCombo->addItem(displayText, rootPath);
    }

    if (m_recoveryVolumeCombo->count() == 0)
    {
        m_recoveryStatusLabel->setText(QStringLiteral("未检测到可扫描的 NTFS 卷。"));
    }
    else
    {
        m_recoveryStatusLabel->setText(QStringLiteral("已刷新卷列表，可执行误删扫描。"));
    }

    kLogEvent event;
    info << event
        << "[FileDock] 刷新文件恢复卷列表, count="
        << m_recoveryVolumeCombo->count()
        << eol;
}

void FileDock::scanDeletedFilesForRecovery()
{
    if (m_recoveryVolumeCombo == nullptr || m_recoveryTable == nullptr || m_recoveryStatusLabel == nullptr)
    {
        return;
    }
    if (m_recoveryVolumeCombo->currentIndex() < 0)
    {
        QMessageBox::warning(this, QStringLiteral("文件恢复"), QStringLiteral("请先选择 NTFS 卷。"));
        return;
    }

    const QString rootPath = m_recoveryVolumeCombo->currentData().toString();
    m_recoveryStatusLabel->setText(QStringLiteral("正在扫描：%1").arg(rootPath));
    QApplication::processEvents();

    {
        kLogEvent event;
        info << event
            << "[FileDock] 开始扫描误删文件, volume="
            << QDir::toNativeSeparators(rootPath).toStdString()
            << eol;
    }

    QString errorText;
    std::vector<ks::file::NtfsDeletedFileEntry> deletedItems;
    const bool scanOk = ks::file::ManualFileSystemParser::enumerateNtfsDeletedFiles(
        rootPath,
        deletedItems,
        errorText);
    if (!scanOk)
    {
        m_recoveryStatusLabel->setText(QStringLiteral("扫描失败：%1").arg(errorText));
        kLogEvent event;
        err << event
            << "[FileDock] 扫描误删失败, volume="
            << QDir::toNativeSeparators(rootPath).toStdString()
            << ", error="
            << errorText.toStdString()
            << eol;
        QMessageBox::warning(this, QStringLiteral("扫描失败"), errorText);
        return;
    }

    m_deletedRecoveryItems = std::move(deletedItems);
    m_recoveryTable->setRowCount(static_cast<int>(m_deletedRecoveryItems.size()));
    for (int row = 0; row < static_cast<int>(m_deletedRecoveryItems.size()); ++row)
    {
        const ks::file::NtfsDeletedFileEntry& itemValue = m_deletedRecoveryItems[static_cast<std::size_t>(row)];
        m_recoveryTable->setItem(row, 0, new QTableWidgetItem(itemValue.fileName));
        m_recoveryTable->setItem(row, 1, new QTableWidgetItem(itemValue.pathHint));
        m_recoveryTable->setItem(row, 2, new QTableWidgetItem(formatSizeText(itemValue.sizeBytes)));
        m_recoveryTable->setItem(row, 3, new QTableWidgetItem(
            itemValue.modifiedTime.isValid()
            ? itemValue.modifiedTime.toString(QStringLiteral("yyyy-MM-dd HH:mm:ss"))
            : QStringLiteral("-")));
        m_recoveryTable->setItem(row, 4, new QTableWidgetItem(QString::number(itemValue.fileReference)));
        m_recoveryTable->setItem(row, 5, new QTableWidgetItem(
            itemValue.residentDataReady ? QStringLiteral("Resident可恢复") : QStringLiteral("仅元数据")));
    }

    m_recoveryStatusLabel->setText(
        QStringLiteral("扫描完成：%1 项（Resident 可恢复 %2 项）")
        .arg(m_deletedRecoveryItems.size())
        .arg(std::count_if(
            m_deletedRecoveryItems.begin(),
            m_deletedRecoveryItems.end(),
            [](const ks::file::NtfsDeletedFileEntry& item) { return item.residentDataReady; })));

    kLogEvent event;
    info << event
        << "[FileDock] 扫描误删完成, volume="
        << QDir::toNativeSeparators(rootPath).toStdString()
        << ", total="
        << m_deletedRecoveryItems.size()
        << eol;
}

void FileDock::recoverSelectedDeletedFiles()
{
    if (m_recoveryTable == nullptr || m_recoveryVolumeCombo == nullptr)
    {
        return;
    }
    const QModelIndexList selectedRows = m_recoveryTable->selectionModel()->selectedRows();
    if (selectedRows.empty())
    {
        QMessageBox::information(this, QStringLiteral("文件恢复"), QStringLiteral("请先在列表中选择要恢复的条目。"));
        return;
    }

    const QString exportDir = QFileDialog::getExistingDirectory(
        this,
        QStringLiteral("选择恢复输出目录"),
        QDir::homePath());
    if (exportDir.isEmpty())
    {
        return;
    }

    const QString volumeRoot = m_recoveryVolumeCombo->currentData().toString();
    {
        kLogEvent event;
        info << event
            << "[FileDock] 开始恢复选中误删项, volume="
            << QDir::toNativeSeparators(volumeRoot).toStdString()
            << ", selectedRows="
            << selectedRows.size()
            << eol;
    }
    int successCount = 0;
    QStringList failTextList;
    for (const QModelIndex& rowIndex : selectedRows)
    {
        const int rowValue = rowIndex.row();
        if (rowValue < 0 || rowValue >= static_cast<int>(m_deletedRecoveryItems.size()))
        {
            continue;
        }

        const ks::file::NtfsDeletedFileEntry& deletedItem =
            m_deletedRecoveryItems[static_cast<std::size_t>(rowValue)];
        QString exportName = deletedItem.fileName.trimmed();
        if (exportName.isEmpty())
        {
            exportName = QStringLiteral("deleted_%1.bin").arg(deletedItem.fileReference);
        }
        const QString targetPath = QDir(exportDir).filePath(exportName);
        QString errorText;
        const bool ok = ks::file::ManualFileSystemParser::recoverNtfsResidentFile(
            volumeRoot,
            deletedItem,
            targetPath,
            errorText);
        if (ok)
        {
            ++successCount;
        }
        else
        {
            failTextList.push_back(QStringLiteral("%1: %2").arg(exportName, errorText));
        }
    }

    const QString summaryText = QStringLiteral("恢复完成：成功 %1，失败 %2。")
        .arg(successCount)
        .arg(failTextList.size());
    m_recoveryStatusLabel->setText(summaryText);
    if (failTextList.empty())
    {
        kLogEvent event;
        info << event
            << "[FileDock] 恢复完成, success="
            << successCount
            << ", failed=0"
            << eol;
        QMessageBox::information(this, QStringLiteral("文件恢复"), summaryText);
    }
    else
    {
        kLogEvent event;
        warn << event
            << "[FileDock] 恢复部分失败, success="
            << successCount
            << ", failed="
            << failTextList.size()
            << eol;
        QMessageBox::warning(
            this,
            QStringLiteral("文件恢复"),
            summaryText + QStringLiteral("\n\n失败明细：\n") + failTextList.join('\n'));
    }
}

void FileDock::showPanelContextMenu(FilePanelWidgets& panel, const QPoint& localPos)
{
    kLogEvent menuOpenEvent;
    dbg << menuOpenEvent
        << "[FileDock] 打开右键菜单, panel="
        << panel.panelNameText.toStdString()
        << ", localPos=("
        << localPos.x()
        << ","
        << localPos.y()
        << ")"
        << eol;

    // 右键命中行时，优先保证“命中行”与“选中集合”一致。
    // 说明：若命中的是已选中行，则保留原多选；若命中未选中行，则切成该单行。
    const QModelIndex hitIndex = panel.fileView->indexAt(localPos);
    QItemSelectionModel* selectionModel = panel.fileView->selectionModel();
    if (hitIndex.isValid() && selectionModel != nullptr)
    {
        const bool hitAlreadySelected = selectionModel->isSelected(hitIndex);
        if (!hitAlreadySelected)
        {
            selectionModel->select(hitIndex, QItemSelectionModel::ClearAndSelect | QItemSelectionModel::Rows);
        }
        panel.fileView->setCurrentIndex(hitIndex);
    }

    // 右键菜单所使用的数据统一来自“当前选中集合”。
    const std::vector<QString> menuPaths = selectedPaths(panel);
    const bool hasSelection = !menuPaths.empty();
    const bool isSingleSelection = menuPaths.size() == 1;
    const QString firstPath = isSingleSelection ? menuPaths.front() : QString();

    // 统计选中内容类型：用于控制菜单可用状态，避免多选时误触单文件功能。
    bool hasAnyFile = false;
    for (const QString& path : menuPaths)
    {
        QFileInfo info(path);
        hasAnyFile = hasAnyFile || info.isFile();
    }

    QMenu menu(this);
    QAction* openAction = menu.addAction(QIcon(":/Icon/process_start.svg"), QStringLiteral("打开/运行"));
    QAction* editAction = menu.addAction(QIcon(":/Icon/process_details.svg"), QStringLiteral("编辑（文本）"));
    QAction* copyPathAction = menu.addAction(QIcon(":/Icon/process_copy_cell.svg"), QStringLiteral("复制路径"));
    menu.addSeparator();
    QAction* copyAction = menu.addAction(QIcon(":/Icon/log_copy.svg"), QStringLiteral("复制"));
    QAction* cutAction = menu.addAction(QIcon(":/Icon/process_suspend.svg"), QStringLiteral("剪切"));
    QAction* pasteAction = menu.addAction(QIcon(":/Icon/process_resume.svg"), QStringLiteral("粘贴"));
    QAction* renameAction = menu.addAction(QIcon(":/Icon/process_priority.svg"), QStringLiteral("重命名(F2)"));
    QAction* deleteAction = menu.addAction(QIcon(":/Icon/process_terminate.svg"), QStringLiteral("删除(Delete)"));
    QAction* takeOwnerAction = menu.addAction(QIcon(":/Icon/file_owner.svg"), QStringLiteral("取得所有权"));
    menu.addSeparator();
    QAction* newFileAction = menu.addAction(QIcon(":/Icon/process_details.svg"), QStringLiteral("新建文件"));
    QAction* newFolderAction = menu.addAction(QIcon(":/Icon/process_open_folder.svg"), QStringLiteral("新建文件夹"));
    QAction* openTerminalAction = menu.addAction(QIcon(":/Icon/process_tree.svg"), QStringLiteral("在终端中打开"));
    menu.addSeparator();
    QAction* columnAction = menu.addAction(QIcon(":/Icon/process_list.svg"), QStringLiteral("选择列..."));
    QAction* detailAction = menu.addAction(QIcon(":/Icon/process_details.svg"), QStringLiteral("属性..."));
    menu.addSeparator();

    // “分析”子菜单：承载哈希/签名/熵值/十六进制入口。
    QMenu* analysisMenu = menu.addMenu(QStringLiteral("分析"));
    analysisMenu->setIcon(QIcon(":/Icon/log_track.svg"));
    QAction* hashAction = analysisMenu->addAction(QIcon(":/Icon/log_track.svg"), QStringLiteral("计算哈希值"));
    QAction* signAction = analysisMenu->addAction(QIcon(":/Icon/process_critical.svg"), QStringLiteral("检查数字签名"));
    QAction* entropyAction = analysisMenu->addAction(QIcon(":/Icon/process_uncritical.svg"), QStringLiteral("计算熵值"));
    QAction* hexAction = analysisMenu->addAction(QIcon(":/Icon/process_details.svg"), QStringLiteral("十六进制查看"));
    analysisMenu->addSeparator();
    QAction* peAction = analysisMenu->addAction(QIcon(":/Icon/process_list.svg"), QStringLiteral("在PE查看器中打开"));

    // 结合选中集合动态启用菜单项，保证“多选”和“右键动作”行为一致。
    const bool singleFileOnly = isSingleSelection && QFileInfo(firstPath).isFile();
    openAction->setEnabled(hasSelection);
    editAction->setEnabled(hasAnyFile);
    copyPathAction->setEnabled(hasSelection);
    copyAction->setEnabled(hasSelection);
    cutAction->setEnabled(hasSelection);
    pasteAction->setEnabled(!m_clipboardPaths.empty());
    renameAction->setEnabled(isSingleSelection);
    deleteAction->setEnabled(hasSelection);
    takeOwnerAction->setEnabled(hasSelection);
    detailAction->setEnabled(hasSelection);
    hashAction->setEnabled(hasAnyFile);
    signAction->setEnabled(hasAnyFile);
    entropyAction->setEnabled(hasAnyFile);
    hexAction->setEnabled(singleFileOnly);
    peAction->setEnabled(singleFileOnly);

    QAction* selectedAction = menu.exec(panel.fileView->viewport()->mapToGlobal(localPos));
    if (selectedAction == nullptr)
    {
        kLogEvent menuCancelEvent;
        dbg << menuCancelEvent
            << "[FileDock] 右键菜单取消, panel="
            << panel.panelNameText.toStdString()
            << eol;
        return;
    }

    {
        kLogEvent menuActionEvent;
        info << menuActionEvent
            << "[FileDock] 右键菜单执行动作, panel="
            << panel.panelNameText.toStdString()
            << ", action="
            << selectedAction->text().toStdString()
            << ", selectedCount="
            << menuPaths.size()
            << eol;
    }

    if (selectedAction == openAction)
    {
        openSelectedItems(panel);
        return;
    }
    if (selectedAction == editAction)
    {
        // 编辑操作支持多选：逐个交给系统默认编辑器。
        for (const QString& path : menuPaths)
        {
            QFileInfo info(path);
            if (!info.isFile())
            {
                continue;
            }
            QDesktopServices::openUrl(QUrl::fromLocalFile(path));
        }
        return;
    }
    if (selectedAction == copyPathAction)
    {
        copySelectedItemPath(panel);
        return;
    }
    if (selectedAction == copyAction)
    {
        copySelectedItems(panel);
        return;
    }
    if (selectedAction == cutAction)
    {
        cutSelectedItems(panel);
        return;
    }
    if (selectedAction == pasteAction)
    {
        pasteClipboardItems(panel);
        return;
    }
    if (selectedAction == renameAction)
    {
        renameSelectedItem(panel);
        return;
    }
    if (selectedAction == deleteAction)
    {
        deleteSelectedItem(panel);
        return;
    }
    if (selectedAction == takeOwnerAction)
    {
        takeOwnershipSelectedItems(panel);
        return;
    }
    if (selectedAction == newFileAction)
    {
        createNewFileOrFolder(panel, false);
        return;
    }
    if (selectedAction == newFolderAction)
    {
        createNewFileOrFolder(panel, true);
        return;
    }
    if (selectedAction == openTerminalAction)
    {
        const QString workPath = panel.currentPath.isEmpty() ? QDir::homePath() : panel.currentPath;
        QProcess::startDetached(QStringLiteral("cmd.exe"), QStringList{ QStringLiteral("/K"), QStringLiteral("cd /d \"%1\"").arg(workPath) });
        return;
    }
    if (selectedAction == columnAction)
    {
        showColumnManagerDialog(panel);
        return;
    }
    if (selectedAction == detailAction)
    {
        // 属性窗口支持多选批量打开；数量过大时做一次确认，避免窗口风暴。
        constexpr std::size_t kMaxAutoOpenDetailCount = 8;
        std::size_t openCount = menuPaths.size();
        if (openCount > kMaxAutoOpenDetailCount)
        {
            const QMessageBox::StandardButton userChoice = QMessageBox::question(
                this,
                QStringLiteral("批量属性"),
                QStringLiteral("已选择 %1 项，最多建议打开 %2 个属性窗口。\n是否仅打开前 %2 项？")
                    .arg(menuPaths.size())
                    .arg(kMaxAutoOpenDetailCount),
                QMessageBox::Yes | QMessageBox::No,
                QMessageBox::Yes);
            if (userChoice != QMessageBox::Yes)
            {
                return;
            }
            openCount = kMaxAutoOpenDetailCount;
        }

        for (std::size_t i = 0; i < openCount; ++i)
        {
            showFileDetailDialog(menuPaths[i]);
        }
        return;
    }
    if (selectedAction == hashAction)
    {
        // 哈希计算支持多选：仅对文件条目计算，目录自动跳过。
        if (!hasAnyFile)
        {
            QMessageBox::information(this, QStringLiteral("哈希计算"), QStringLiteral("请选择至少一个文件。"));
            return;
        }

        QStringList hashLines;
        for (const QString& path : menuPaths)
        {
            QFileInfo info(path);
            if (!info.isFile())
            {
                continue;
            }

            QFile file(path);
            if (!file.open(QIODevice::ReadOnly))
            {
                hashLines << QStringLiteral("[%1]\n无法打开文件。").arg(path);
                continue;
            }

            QCryptographicHash md5(QCryptographicHash::Md5);
            QCryptographicHash sha1(QCryptographicHash::Sha1);
            QCryptographicHash sha256(QCryptographicHash::Sha256);
            while (!file.atEnd())
            {
                const QByteArray chunk = file.read(1024 * 256);
                md5.addData(chunk);
                sha1.addData(chunk);
                sha256.addData(chunk);
            }
            file.close();

            hashLines << QStringLiteral(
                "[%1]\nMD5: %2\nSHA1: %3\nSHA256: %4")
                .arg(path)
                .arg(QString::fromLatin1(md5.result().toHex()))
                .arg(QString::fromLatin1(sha1.result().toHex()))
                .arg(QString::fromLatin1(sha256.result().toHex()));
        }

        QMessageBox::information(this, QStringLiteral("哈希计算"), hashLines.join(QStringLiteral("\n\n")));
        return;
    }
    if (selectedAction == signAction)
    {
        // 数字签名入口与属性页联动，支持多选逐个打开详情。
        if (!hasAnyFile)
        {
            QMessageBox::information(this, QStringLiteral("签名检查"), QStringLiteral("请选择至少一个文件。"));
            return;
        }

        constexpr std::size_t kMaxAutoOpenSignCount = 8;
        std::size_t openedCount = 0;
        for (const QString& path : menuPaths)
        {
            if (!QFileInfo(path).isFile())
            {
                continue;
            }
            if (openedCount >= kMaxAutoOpenSignCount)
            {
                break;
            }
            showFileDetailDialog(path);
            openedCount += 1;
        }

        if (openedCount == kMaxAutoOpenSignCount && menuPaths.size() > kMaxAutoOpenSignCount)
        {
            QMessageBox::information(
                this,
                QStringLiteral("签名检查"),
                QStringLiteral("为避免打开过多窗口，已仅展示前 %1 个文件。")
                    .arg(kMaxAutoOpenSignCount));
        }
        return;
    }
    if (selectedAction == entropyAction)
    {
        // 熵值计算支持多选：仅统计文件条目。
        if (!hasAnyFile)
        {
            QMessageBox::information(this, QStringLiteral("熵值计算"), QStringLiteral("请选择至少一个文件。"));
            return;
        }

        QStringList entropyLines;
        for (const QString& path : menuPaths)
        {
            QFileInfo info(path);
            if (!info.isFile())
            {
                continue;
            }

            QFile file(path);
            if (!file.open(QIODevice::ReadOnly))
            {
                entropyLines << QStringLiteral("[%1] 无法打开文件。").arg(path);
                continue;
            }

            std::array<std::uint64_t, 256> bucket{};
            std::uint64_t totalCount = 0;
            while (!file.atEnd())
            {
                const QByteArray chunk = file.read(1024 * 256);
                for (unsigned char byteValue : chunk)
                {
                    bucket[byteValue] += 1;
                    totalCount += 1;
                }
            }
            file.close();

            double entropy = 0.0;
            if (totalCount > 0)
            {
                for (std::uint64_t count : bucket)
                {
                    if (count == 0)
                    {
                        continue;
                    }
                    const double p = static_cast<double>(count) / static_cast<double>(totalCount);
                    entropy -= p * std::log2(p);
                }
            }

            entropyLines << QStringLiteral("[%1] 熵值: %2 (0~8)")
                .arg(path)
                .arg(QString::number(entropy, 'f', 4));
        }

        QMessageBox::information(this, QStringLiteral("熵值计算"), entropyLines.join('\n'));
        return;
    }
    if (selectedAction == hexAction || selectedAction == peAction)
    {
        if (!firstPath.isEmpty() && QFileInfo(firstPath).isFile())
        {
            showFileDetailDialog(firstPath);
        }
        return;
    }
}

void FileDock::openSelectedItems(FilePanelWidgets& panel)
{
    // 打开逻辑支持多选：目录与文件分开处理，避免多目录时误切换当前面板路径。
    const std::vector<QString> paths = selectedPaths(panel);
    if (paths.empty())
    {
        return;
    }

    {
        kLogEvent event;
        info << event
            << "[FileDock] 打开选中项, panel="
            << panel.panelNameText.toStdString()
            << ", count="
            << paths.size()
            << eol;
    }

    for (const QString& path : paths)
    {
        QFileInfo info(path);
        if (info.isDir())
        {
            if (paths.size() == 1)
            {
                navigateToPath(panel, path, true);
            }
            else
            {
                QDesktopServices::openUrl(QUrl::fromLocalFile(path));
            }
            continue;
        }

        QDesktopServices::openUrl(QUrl::fromLocalFile(path));
    }
}

void FileDock::copySelectedItemPath(FilePanelWidgets& panel)
{
    const std::vector<QString> paths = selectedPaths(panel);
    if (paths.empty())
    {
        return;
    }

    QStringList lines;
    for (const QString& path : paths)
    {
        lines << QDir::toNativeSeparators(path);
    }
    QApplication::clipboard()->setText(lines.join('\n'));

    kLogEvent event;
    info << event
        << "[FileDock] 复制路径到剪贴板, panel="
        << panel.panelNameText.toStdString()
        << ", count="
        << paths.size()
        << eol;
}

void FileDock::copySelectedItems(FilePanelWidgets& panel)
{
    m_clipboardPaths = selectedPaths(panel);
    m_clipboardCutMode = false;

    if (!m_clipboardPaths.empty())
    {
        kLogEvent event;
        info << event
            << "[FileDock] 复制项, panel="
            << panel.panelNameText.toStdString()
            << ", count="
            << m_clipboardPaths.size()
            << eol;
    }
}

void FileDock::cutSelectedItems(FilePanelWidgets& panel)
{
    m_clipboardPaths = selectedPaths(panel);
    m_clipboardCutMode = true;

    if (!m_clipboardPaths.empty())
    {
        kLogEvent event;
        info << event
            << "[FileDock] 剪切项, panel="
            << panel.panelNameText.toStdString()
            << ", count="
            << m_clipboardPaths.size()
            << eol;
    }
}

void FileDock::pasteClipboardItems(FilePanelWidgets& panel)
{
    if (m_clipboardPaths.empty())
    {
        return;
    }

    {
        kLogEvent event;
        info << event
            << "[FileDock] 粘贴请求, panel="
            << panel.panelNameText.toStdString()
            << ", targetPath="
            << QDir::toNativeSeparators(panel.currentPath).toStdString()
            << ", clipboardCount="
            << m_clipboardPaths.size()
            << ", cutMode="
            << (m_clipboardCutMode ? "true" : "false")
            << eol;
    }

    // 用进度卡片反馈粘贴过程，避免用户无感等待。
    const int progressPid = kPro.add("文件", "粘贴");
    kPro.set(progressPid, "准备粘贴", 0, 5.0f);

    QStringList errorLines;
    const std::size_t totalCount = m_clipboardPaths.size();
    for (std::size_t i = 0; i < totalCount; ++i)
    {
        const QString sourcePath = m_clipboardPaths[i];
        QFileInfo sourceInfo(sourcePath);
        if (!sourceInfo.exists())
        {
            errorLines << QStringLiteral("源不存在：%1").arg(sourcePath);
            continue;
        }

        const QString targetPath = QDir(panel.currentPath).filePath(sourceInfo.fileName());
        if (QDir::cleanPath(sourcePath).compare(QDir::cleanPath(targetPath), Qt::CaseInsensitive) == 0)
        {
            continue;
        }

        bool itemOk = false;
        if (m_clipboardCutMode)
        {
            // 剪切优先尝试重命名移动，失败再走复制+删除兜底。
            itemOk = QFile::rename(sourcePath, targetPath);
            if (!itemOk)
            {
                QString copyErrorText;
                if (sourceInfo.isDir())
                {
                    itemOk = copyDirectoryRecursively(sourcePath, targetPath, copyErrorText)
                        && QDir(sourcePath).removeRecursively();
                }
                else
                {
                    if (QFile::exists(targetPath))
                    {
                        QFile::remove(targetPath);
                    }
                    itemOk = QFile::copy(sourcePath, targetPath) && QFile::remove(sourcePath);
                    if (!itemOk)
                    {
                        copyErrorText = QStringLiteral("移动失败: %1 -> %2").arg(sourcePath, targetPath);
                    }
                }
                if (!itemOk)
                {
                    errorLines << copyErrorText;
                }
            }
        }
        else
        {
            // 复制模式：目录递归复制，文件覆盖复制。
            if (sourceInfo.isDir())
            {
                QString copyErrorText;
                itemOk = copyDirectoryRecursively(sourcePath, targetPath, copyErrorText);
                if (!itemOk)
                {
                    errorLines << copyErrorText;
                }
            }
            else
            {
                if (QFile::exists(targetPath))
                {
                    QFile::remove(targetPath);
                }
                itemOk = QFile::copy(sourcePath, targetPath);
                if (!itemOk)
                {
                    errorLines << QStringLiteral("复制失败: %1 -> %2").arg(sourcePath, targetPath);
                }
            }
        }

        const float progress = 5.0f + (static_cast<float>(i + 1) / static_cast<float>(totalCount)) * 90.0f;
        kPro.set(progressPid, "粘贴处理中", 0, progress);
    }

    if (m_clipboardCutMode && errorLines.isEmpty())
    {
        // 剪切全部成功时清空内部剪贴板。
        m_clipboardPaths.clear();
        m_clipboardCutMode = false;
    }

    refreshPanel(panel);
    kPro.set(progressPid, "粘贴完成", 0, 100.0f);

    if (!errorLines.isEmpty())
    {
        kLogEvent event;
        warn << event
            << "[FileDock] 粘贴部分失败, panel="
            << panel.panelNameText.toStdString()
            << ", errorCount="
            << errorLines.size()
            << eol;
        QMessageBox::warning(
            this,
            QStringLiteral("粘贴结果"),
            QStringLiteral("部分操作失败：\n%1").arg(errorLines.join('\n')));
        return;
    }

    kLogEvent event;
    info << event
        << "[FileDock] 粘贴完成, panel="
        << panel.panelNameText.toStdString()
        << ", totalCount="
        << totalCount
        << eol;
}

void FileDock::createNewFileOrFolder(FilePanelWidgets& panel, bool createFolder)
{
    {
        kLogEvent event;
        info << event
            << "[FileDock] 新建请求, panel="
            << panel.panelNameText.toStdString()
            << ", type="
            << (createFolder ? "folder" : "file")
            << ", currentPath="
            << QDir::toNativeSeparators(panel.currentPath).toStdString()
            << eol;
    }

    bool ok = false;
    const QString inputName = QInputDialog::getText(
        this,
        createFolder ? QStringLiteral("新建文件夹") : QStringLiteral("新建文件"),
        QStringLiteral("请输入名称："),
        QLineEdit::Normal,
        createFolder ? QStringLiteral("新建文件夹") : QStringLiteral("新建文件.txt"),
        &ok);
    if (!ok)
    {
        return;
    }

    const QString trimmedName = inputName.trimmed();
    if (trimmedName.isEmpty())
    {
        return;
    }

    const QString targetPath = QDir(panel.currentPath).filePath(trimmedName);
    bool createOk = false;
    if (createFolder)
    {
        QDir dir;
        createOk = dir.mkpath(targetPath);
    }
    else
    {
        QFile file(targetPath);
        createOk = file.open(QIODevice::WriteOnly | QIODevice::Truncate);
        file.close();
    }

    if (!createOk)
    {
        kLogEvent event;
        err << event
            << "[FileDock] 新建失败, panel="
            << panel.panelNameText.toStdString()
            << ", targetPath="
            << QDir::toNativeSeparators(targetPath).toStdString()
            << eol;
        QMessageBox::warning(this, QStringLiteral("创建失败"), QStringLiteral("无法创建：%1").arg(targetPath));
        return;
    }

    refreshPanel(panel);

    kLogEvent event;
    info << event
        << "[FileDock] 新建成功, panel="
        << panel.panelNameText.toStdString()
        << ", targetPath="
        << QDir::toNativeSeparators(targetPath).toStdString()
        << eol;
}

void FileDock::renameSelectedItem(FilePanelWidgets& panel)
{
    const QString path = currentIndexPath(panel);
    if (path.isEmpty())
    {
        return;
    }

    {
        kLogEvent event;
        info << event
            << "[FileDock] 重命名请求, panel="
            << panel.panelNameText.toStdString()
            << ", oldPath="
            << QDir::toNativeSeparators(path).toStdString()
            << eol;
    }

    QFileInfo oldInfo(path);
    bool ok = false;
    const QString newName = QInputDialog::getText(
        this,
        QStringLiteral("重命名"),
        QStringLiteral("新名称："),
        QLineEdit::Normal,
        oldInfo.fileName(),
        &ok);
    if (!ok)
    {
        return;
    }

    const QString trimmedName = newName.trimmed();
    if (trimmedName.isEmpty() || trimmedName == oldInfo.fileName())
    {
        return;
    }

    const QString newPath = oldInfo.dir().filePath(trimmedName);
    bool renameOk = false;
    if (oldInfo.isDir())
    {
        QDir parentDir = oldInfo.dir();
        renameOk = parentDir.rename(oldInfo.fileName(), trimmedName);
    }
    else
    {
        renameOk = QFile::rename(path, newPath);
    }

    if (!renameOk)
    {
        kLogEvent event;
        err << event
            << "[FileDock] 重命名失败, panel="
            << panel.panelNameText.toStdString()
            << ", oldPath="
            << QDir::toNativeSeparators(path).toStdString()
            << ", newPath="
            << QDir::toNativeSeparators(newPath).toStdString()
            << eol;
        QMessageBox::warning(
            this,
            QStringLiteral("重命名失败"),
            QStringLiteral("无法重命名：\n%1\n->\n%2").arg(path, newPath));
        return;
    }

    refreshPanel(panel);

    kLogEvent event;
    info << event
        << "[FileDock] 重命名成功, panel="
        << panel.panelNameText.toStdString()
        << ", oldPath="
        << QDir::toNativeSeparators(path).toStdString()
        << ", newPath="
        << QDir::toNativeSeparators(newPath).toStdString()
        << eol;
}

void FileDock::deleteSelectedItem(FilePanelWidgets& panel)
{
    const std::vector<QString> paths = selectedPaths(panel);
    if (paths.empty())
    {
        return;
    }

    {
        kLogEvent event;
        info << event
            << "[FileDock] 删除请求, panel="
            << panel.panelNameText.toStdString()
            << ", count="
            << paths.size()
            << eol;
    }

    const QMessageBox::StandardButton userChoice = QMessageBox::question(
        this,
        QStringLiteral("删除确认"),
        QStringLiteral("确定删除选中的 %1 项吗？").arg(paths.size()),
        QMessageBox::Yes | QMessageBox::No,
        QMessageBox::No);
    if (userChoice != QMessageBox::Yes)
    {
        return;
    }

    const int progressPid = kPro.add("文件", "删除");
    kPro.set(progressPid, "删除开始", 0, 5.0f);

    QStringList errors;
    for (std::size_t i = 0; i < paths.size(); ++i)
    {
        const QString path = paths[i];
        bool removeOk = false;

        // 优先回收站删除，失败再使用硬删除兜底。
        removeOk = QFile::moveToTrash(path);
        if (!removeOk)
        {
            QFileInfo info(path);
            if (info.isDir())
            {
                removeOk = QDir(path).removeRecursively();
            }
            else
            {
                removeOk = QFile::remove(path);
            }
        }

        if (!removeOk)
        {
            errors << QStringLiteral("删除失败：%1").arg(path);
        }

        const float progress = 5.0f + (static_cast<float>(i + 1) / static_cast<float>(paths.size())) * 90.0f;
        kPro.set(progressPid, "删除处理中", 0, progress);
    }

    refreshPanel(panel);
    kPro.set(progressPid, "删除完成", 0, 100.0f);

    if (!errors.isEmpty())
    {
        kLogEvent event;
        warn << event
            << "[FileDock] 删除部分失败, panel="
            << panel.panelNameText.toStdString()
            << ", errorCount="
            << errors.size()
            << eol;
        QMessageBox::warning(this, QStringLiteral("删除结果"), errors.join('\n'));
        return;
    }

    kLogEvent event;
    info << event
        << "[FileDock] 删除完成, panel="
        << panel.panelNameText.toStdString()
        << ", count="
        << paths.size()
        << eol;
}

void FileDock::takeOwnershipSelectedItems(FilePanelWidgets& panel)
{
    const std::vector<QString> paths = selectedPaths(panel);
    if (paths.empty())
    {
        return;
    }

    kLogEvent startEvent;
    info << startEvent
        << "[FileDock] 取得所有权请求, panel="
        << panel.panelNameText.toStdString()
        << ", count="
        << paths.size()
        << eol;

    const QMessageBox::StandardButton userChoice = QMessageBox::question(
        this,
        QStringLiteral("取得所有权"),
        QStringLiteral("将对选中的 %1 项执行“取得所有权 + 完全控制授权”。\n此操作可能需要管理员权限，是否继续？")
        .arg(paths.size()),
        QMessageBox::Yes | QMessageBox::No,
        QMessageBox::No);
    if (userChoice != QMessageBox::Yes)
    {
        return;
    }

    const int progressPid = kPro.add("文件", "取得所有权");
    kPro.set(progressPid, "准备执行", 0, 5.0f);

    QStringList errorDetails;
    for (std::size_t index = 0; index < paths.size(); ++index)
    {
        const QString& targetPath = paths[index];
        QString detailText;
        const bool itemOk = takeOwnershipBySystemCommand(targetPath, detailText);
        if (!itemOk)
        {
            errorDetails.push_back(detailText);
        }

        const float progress = 5.0f + (static_cast<float>(index + 1) / static_cast<float>(paths.size())) * 90.0f;
        kPro.set(progressPid, "处理中", 0, progress);
    }
    kPro.set(progressPid, "完成", 0, 100.0f);

    refreshPanel(panel);

    if (!errorDetails.isEmpty())
    {
        kLogEvent failEvent;
        warn << failEvent
            << "[FileDock] 取得所有权部分失败, panel="
            << panel.panelNameText.toStdString()
            << ", failCount="
            << errorDetails.size()
            << eol;
        QMessageBox::warning(
            this,
            QStringLiteral("取得所有权结果"),
            QStringLiteral("部分项执行失败，详细信息如下：\n\n%1")
            .arg(errorDetails.join(QStringLiteral("\n\n--------------------\n\n"))));
        return;
    }

    kLogEvent finishEvent;
    info << finishEvent
        << "[FileDock] 取得所有权完成, panel="
        << panel.panelNameText.toStdString()
        << ", successCount="
        << paths.size()
        << eol;
    QMessageBox::information(this, QStringLiteral("取得所有权"), QStringLiteral("操作已完成。"));
}

void FileDock::showColumnManagerDialog(FilePanelWidgets& panel)
{
    {
        kLogEvent event;
        info << event
            << "[FileDock] 打开列管理器, panel="
            << panel.panelNameText.toStdString()
            << eol;
    }

    QDialog dialog(this);
    dialog.setWindowTitle(QStringLiteral("列管理器"));
    dialog.resize(340, 260);

    QVBoxLayout* rootLayout = new QVBoxLayout(&dialog);
    QLabel* tipLabel = new QLabel(QStringLiteral("勾选表示显示该列，可拖拽表头调整顺序。"), &dialog);
    tipLabel->setWordWrap(true);
    rootLayout->addWidget(tipLabel, 0);

    const bool manualMode = currentModeIsManual(panel);
    const int columnCount = manualMode
        ? (panel.manualModel == nullptr ? 0 : panel.manualModel->columnCount())
        : (panel.fsModel == nullptr ? 0 : panel.fsModel->columnCount());
    std::vector<QCheckBox*> columnChecks;
    columnChecks.reserve(static_cast<std::size_t>(columnCount));
    for (int column = 0; column < columnCount; ++column)
    {
        const QString columnName = manualMode
            ? panel.manualModel->headerData(column, Qt::Horizontal).toString()
            : panel.fsModel->headerData(column, Qt::Horizontal).toString();
        QCheckBox* checkBox = new QCheckBox(columnName, &dialog);
        checkBox->setChecked(!panel.fileView->isColumnHidden(column));
        checkBox->setToolTip(QStringLiteral("切换列“%1”显示状态").arg(columnName));
        rootLayout->addWidget(checkBox, 0);
        columnChecks.push_back(checkBox);
    }
    rootLayout->addStretch(1);

    QDialogButtonBox* buttonBox = new QDialogButtonBox(
        QDialogButtonBox::Ok | QDialogButtonBox::Cancel,
        &dialog);
    rootLayout->addWidget(buttonBox, 0);
    connect(buttonBox, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
    connect(buttonBox, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);

    if (dialog.exec() != QDialog::Accepted)
    {
        return;
    }

    for (int column = 0; column < static_cast<int>(columnChecks.size()); ++column)
    {
        const bool visible = columnChecks[static_cast<std::size_t>(column)]->isChecked();
        panel.fileView->setColumnHidden(column, !visible);
    }

    kLogEvent event;
    info << event
        << "[FileDock] 列管理器应用完成, panel="
        << panel.panelNameText.toStdString()
        << eol;
}

void FileDock::showFileDetailDialog(const QString& filePath)
{
    QFileInfo fileInfo(filePath);
    if (!fileInfo.exists())
    {
        QMessageBox::warning(this, QStringLiteral("文件详情"), QStringLiteral("目标不存在：%1").arg(filePath));
        return;
    }

    // 非模态详情窗：允许同时打开多个属性页做对比。
    FileDetailDialog* dialog = new FileDetailDialog(filePath, this);
    dialog->setWindowFlag(Qt::WindowStaysOnTopHint, false);
    dialog->show();
    dialog->raise();
    dialog->activateWindow();

    kLogEvent event;
    info << event
        << "[FileDock] 打开文件详情窗口, filePath="
        << QDir::toNativeSeparators(filePath).toStdString()
        << eol;
}

QString FileDock::currentIndexPath(const FilePanelWidgets& panel) const
{
    if (panel.fileView == nullptr)
    {
        return QString();
    }

    const QModelIndex proxyIndex = panel.fileView->currentIndex();
    if (!proxyIndex.isValid())
    {
        return QString();
    }

    if (currentModeIsManual(panel))
    {
        if (panel.manualProxyModel == nullptr || panel.manualModel == nullptr)
        {
            return QString();
        }

        const QModelIndex sourceIndex = panel.manualProxyModel->mapToSource(proxyIndex);
        if (!sourceIndex.isValid())
        {
            return QString();
        }

        const QStandardItem* fullPathItem = panel.manualModel->item(
            sourceIndex.row(),
            static_cast<int>(ManualModelColumn::FullPath));
        if (fullPathItem == nullptr)
        {
            return QString();
        }
        return fullPathItem->text();
    }

    if (panel.proxyModel == nullptr || panel.fsModel == nullptr)
    {
        return QString();
    }
    const QModelIndex sourceIndex = panel.proxyModel->mapToSource(proxyIndex);
    return sourceIndex.isValid() ? panel.fsModel->filePath(sourceIndex) : QString();
}

std::vector<QString> FileDock::selectedPaths(const FilePanelWidgets& panel) const
{
    std::vector<QString> result;
    if (panel.fileView == nullptr || panel.fileView->selectionModel() == nullptr)
    {
        return result;
    }

    const QModelIndexList selectedRows = panel.fileView->selectionModel()->selectedRows(0);
    result.reserve(static_cast<std::size_t>(selectedRows.size()));

    if (currentModeIsManual(panel))
    {
        if (panel.manualProxyModel == nullptr || panel.manualModel == nullptr)
        {
            return result;
        }
        for (const QModelIndex& proxyIndex : selectedRows)
        {
            const QModelIndex sourceIndex = panel.manualProxyModel->mapToSource(proxyIndex);
            if (!sourceIndex.isValid())
            {
                continue;
            }
            const QStandardItem* fullPathItem = panel.manualModel->item(
                sourceIndex.row(),
                static_cast<int>(ManualModelColumn::FullPath));
            if (fullPathItem == nullptr)
            {
                continue;
            }
            const QString pathText = fullPathItem->text();
            if (pathText.isEmpty())
            {
                continue;
            }
            if (std::find(result.begin(), result.end(), pathText) == result.end())
            {
                result.push_back(pathText);
            }
        }
    }
    else
    {
        if (panel.proxyModel == nullptr || panel.fsModel == nullptr)
        {
            return result;
        }
        for (const QModelIndex& proxyIndex : selectedRows)
        {
            const QModelIndex sourceIndex = panel.proxyModel->mapToSource(proxyIndex);
            if (!sourceIndex.isValid())
            {
                continue;
            }
            const QString path = panel.fsModel->filePath(sourceIndex);
            if (path.isEmpty())
            {
                continue;
            }
            if (std::find(result.begin(), result.end(), path) == result.end())
            {
                result.push_back(path);
            }
        }
    }

    // 如果多选为空但存在当前行，回退为当前行路径，便于右键单项操作。
    if (result.empty())
    {
        const QString currentPath = currentIndexPath(panel);
        if (!currentPath.isEmpty())
        {
            result.push_back(currentPath);
        }
    }
    return result;
}

QString FileDock::formatSizeText(std::uint64_t sizeBytes)
{
    static const std::array<const char*, 5> units{ "B", "KB", "MB", "GB", "TB" };
    double value = static_cast<double>(sizeBytes);
    std::size_t unitIndex = 0;
    while (value >= 1024.0 && (unitIndex + 1) < units.size())
    {
        value /= 1024.0;
        ++unitIndex;
    }

    if (unitIndex == 0)
    {
        return QStringLiteral("%1 %2").arg(static_cast<qulonglong>(sizeBytes)).arg(units[unitIndex]);
    }
    return QStringLiteral("%1 %2").arg(QString::number(value, 'f', 2)).arg(units[unitIndex]);
}
