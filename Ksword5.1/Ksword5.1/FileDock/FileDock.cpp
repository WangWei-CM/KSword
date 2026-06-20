#include "FileDock.h"
#include "FilePropertyPeAnalyzer.h"
#include "FileHandleUsageScanner.h"

// ============================================================
// FileDock.cpp
// 说明：
// - 该文件实现双栏资源管理器核心交互；
// - 支持导航、过滤、排序、基础文件操作与文件详情展示。
// ============================================================

#include "../theme.h"
#include "../UI/CodeEditorWidget.h"
#include "../UI/HexEditorWidget.h"
#include "../ArkDriverClient/ArkDriverClient.h"
#include "../ksword/file/file.h"

#include <QApplication>
#include <QAbstractItemView>
#include <QAction>
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
#include <QGridLayout>
#include <QGroupBox>
#include <QHeaderView>
#include <QHBoxLayout>
#include <QInputDialog>
#include <QItemSelectionModel>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QMessageBox>
#include <QMetaObject>
#include <QModelIndex>
#include <QPlainTextEdit>
#include <QPointer>
#include <QProgressBar>
#include <QProcess>
#include <QPushButton>
#include <QRegularExpression>
#include <QRunnable>
#include <QScreen>
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
#include <QThreadPool>
#include <QToolButton>
#include <QTreeWidget>
#include <QTreeWidgetItem>
#include <QTreeView>
#include <QTimeZone>
#include <QUrl>
#include <QVector>
#include <QVBoxLayout>
#include <QWindow>

#include <array>
#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cctype>
#include <cmath>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <thread>

#include <Aclapi.h>
#include <Sddl.h>

#pragma comment(lib, "Advapi32.lib")

namespace
{
    struct DriverDeleteTarget
    {
        QString path;
        bool isDirectory = false;
    };

    enum class UnlockOperationMode
    {
        CloseHandleR3 = 0,
        TerminateProcessR3,
        TerminateProcessR0
    };

    struct UnlockProcessCandidate
    {
        std::uint32_t processId = 0U;
        QString processName;
        QString processImagePath;
        QStringList matchedTargetList;
        QStringList matchRuleList;
        std::size_t matchCount = 0U;
        bool isCurrentProcess = false;
        bool isCriticalProcess = false;
    };

    struct UnlockHandleCandidate
    {
        std::uint32_t processId = 0U;
        QString processName;
        QString processImagePath;
        std::uint64_t handleValue = 0U;
        std::uint32_t grantedAccess = 0U;
        QString matchedTargetPath;
        QString matchRuleText;
        QString objectName;
        QString enumerationSource;
        bool isCurrentProcess = false;
        bool isCriticalProcess = false;
    };

    struct UnlockSelectionResult
    {
        bool accepted = false;
        UnlockOperationMode operationMode = UnlockOperationMode::CloseHandleR3;
        std::vector<std::uint32_t> selectedProcessIdList;
        std::vector<UnlockHandleCandidate> selectedHandleList;
    };

    // UnlockSelectionSharedState：
    // - 作用：在线程与 UI 队列之间传递解锁器选择结果；
    // - 说明：使用 shared_ptr 托管，避免 FileDock 析构时队列中的 UI 回调访问已释放栈变量。
    struct UnlockSelectionSharedState
    {
        std::mutex mutex;                         // mutex：保护 completed 与 result 的互斥锁。
        std::condition_variable condition;        // condition：通知后台线程 UI 选择已完成。
        bool completed = false;                   // completed：标记 UI 选择流程是否已经写入结果。
        UnlockSelectionResult result;             // result：保存用户选择的操作方式及句柄/PID 目标。
    };

    // resolveVisibleDialogParent 作用：
    // - 为文件解锁器选择一个可见父窗口；
    // - Shell 右键会使用隐藏 FileDock 宿主，不能直接把弹窗挂在隐藏控件上。
    QWidget* resolveVisibleDialogParent(QWidget* const preferredParent)
    {
        QWidget* candidate = preferredParent;
        if (candidate != nullptr)
        {
            QWidget* const topLevel = candidate->window();
            if (topLevel != nullptr)
            {
                candidate = topLevel;
            }
        }
        if (candidate != nullptr && candidate->isVisible())
        {
            return candidate;
        }
        if (QWidget* const activeWindow = QApplication::activeWindow(); activeWindow != nullptr)
        {
            return activeWindow;
        }
        const QWidgetList topLevelWidgetList = QApplication::topLevelWidgets();
        for (QWidget* const widget : topLevelWidgetList)
        {
            if (widget != nullptr && widget->isVisible())
            {
                return widget;
            }
        }
        return preferredParent;
    }

    int calculateFileStandaloneWindowMaxWidth(
        QWidget* candidateParent,
        QWidget* fallbackWindow,
        const double ratio,
        const int fallbackWidth)
    {
        // 输入：
        // - candidateParent：优先参考的客户区控件；
        // - fallbackWindow：当前独立窗口，用于屏幕回退；
        // - ratio：客户区宽度比例；
        // - fallbackWidth：回退宽度。
        // 处理：
        // - 优先使用父控件 contentsRect 宽度；
        // - 父控件不可用时使用活动窗口客户区；
        // - 最后使用屏幕可用区域宽度。
        // 返回：按比例计算出的最大宽度；仅在完全无法判断时使用回退宽度。
        int clientWidth = 0;
        if (candidateParent != nullptr && candidateParent->contentsRect().width() > 0)
        {
            clientWidth = candidateParent->contentsRect().width();
        }

        if (clientWidth <= 0)
        {
            QWidget* activeWindow = QApplication::activeWindow();
            if (activeWindow != nullptr &&
                activeWindow != fallbackWindow &&
                activeWindow->contentsRect().width() > 0)
            {
                clientWidth = activeWindow->contentsRect().width();
            }
        }

        QScreen* targetScreen = nullptr;
        if (candidateParent != nullptr && candidateParent->windowHandle() != nullptr)
        {
            targetScreen = candidateParent->windowHandle()->screen();
        }
        if (targetScreen == nullptr && fallbackWindow != nullptr && fallbackWindow->windowHandle() != nullptr)
        {
            targetScreen = fallbackWindow->windowHandle()->screen();
        }
        if (targetScreen == nullptr)
        {
            targetScreen = QApplication::primaryScreen();
        }
        if (clientWidth <= 0 && targetScreen != nullptr)
        {
            clientWidth = targetScreen->availableGeometry().width();
        }

        const int boundedFallbackWidth = std::max(1, fallbackWidth);
        if (clientWidth <= 0 || ratio <= 0.0)
        {
            return boundedFallbackWidth;
        }
        return std::max(1, static_cast<int>(std::floor(static_cast<double>(clientWidth) * ratio)));
    }

    void applyFileStandaloneWindowWidthLimit(
        QWidget* window,
        QWidget* candidateParent,
        const QSize& preferredSize,
        const double ratio)
    {
        // 输入：
        // - window：待约束的文件属性窗口；
        // - candidateParent：客户区宽度来源；
        // - preferredSize：原始设计尺寸；
        // - ratio：最大宽度比例。
        // 处理：设置 maximumWidth 并裁剪初始 resize 宽度。
        // 返回：无。
        if (window == nullptr)
        {
            return;
        }

        const int maxWidth = calculateFileStandaloneWindowMaxWidth(
            candidateParent,
            window,
            ratio,
            preferredSize.width());
        window->setMaximumWidth(maxWidth);
        window->resize(std::min(preferredSize.width(), maxWidth), preferredSize.height());
    }

    // buildOpaqueStandaloneDialogStyle 作用：
    // - 生成独立弹窗不透明样式；
    // - 前置声明用于供解锁器选择对话框在 helper 正式定义前调用。
    QString buildOpaqueStandaloneDialogStyle(const QString& dialogObjectName);

    QString unlockOperationModeToText(const UnlockOperationMode mode)
    {
        if (mode == UnlockOperationMode::CloseHandleR3)
        {
            return QStringLiteral("R3 关闭句柄");
        }
        return mode == UnlockOperationMode::TerminateProcessR0
            ? QStringLiteral("R0 结束进程")
            : QStringLiteral("R3 结束进程");
    }

    QString formatHandleValueText(const std::uint64_t handleValue)
    {
        if (handleValue == 0U)
        {
            return QStringLiteral("-");
        }
        return QStringLiteral("0x%1")
            .arg(static_cast<qulonglong>(handleValue), 0, 16)
            .toUpper();
    }

    void appendUniqueText(QStringList& list, const QString& text)
    {
        const QString normalizedText = text.trimmed();
        if (!normalizedText.isEmpty() && !list.contains(normalizedText))
        {
            list.push_back(normalizedText);
        }
    }

    bool isCriticalProcessName(const QString& processName)
    {
        const QString normalizedName = processName.trimmed().toLower();
        if (normalizedName.isEmpty())
        {
            return false;
        }

        static const std::set<QString> criticalNameSet =
        {
            QStringLiteral("smss.exe"),
            QStringLiteral("csrss.exe"),
            QStringLiteral("wininit.exe"),
            QStringLiteral("services.exe"),
            QStringLiteral("lsass.exe"),
            QStringLiteral("winlogon.exe")
        };
        return criticalNameSet.find(normalizedName) != criticalNameSet.end();
    }

    // isPathReparsePoint：
    // - 作用：判断目标路径是否为重解析点（符号链接/Junction 等）；
    // - 用于目录递归删除时避免误跟进到链接目标。
    bool isPathReparsePoint(const QString& path)
    {
        const std::wstring nativePathText = QDir::toNativeSeparators(path).toStdWString();
        if (nativePathText.empty())
        {
            return false;
        }

        const DWORD fileAttributes = ::GetFileAttributesW(nativePathText.c_str());
        if (fileAttributes == INVALID_FILE_ATTRIBUTES)
        {
            return false;
        }
        return (fileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0U;
    }

    QString formatWin32ErrorText(const std::uint32_t errorCode)
    {
        if (errorCode == ERROR_SUCCESS)
        {
            return QStringLiteral("0");
        }

        wchar_t* messageBuffer = nullptr;
        const DWORD chars = ::FormatMessageW(
            FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
            nullptr,
            errorCode,
            MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT),
            reinterpret_cast<LPWSTR>(&messageBuffer),
            0,
            nullptr);
        QString messageText;
        if (chars > 0 && messageBuffer != nullptr)
        {
            messageText = QString::fromWCharArray(messageBuffer, static_cast<int>(chars)).trimmed();
        }
        if (messageBuffer != nullptr)
        {
            ::LocalFree(messageBuffer);
        }
        return messageText.isEmpty()
            ? QString::number(errorCode)
            : QStringLiteral("%1 (%2)").arg(errorCode).arg(messageText);
    }

    QString formatReparseTagText(const std::uint32_t tagValue)
    {
        return QStringLiteral("0x%1")
            .arg(tagValue, 8, 16, QChar('0'))
            .toUpper();
    }

    ks::file::ReparsePointQueryResult queryReparsePointForUi(const QString& path)
    {
        const QString nativePathText = QDir::toNativeSeparators(path).trimmed();
        if (nativePathText.isEmpty())
        {
            ks::file::ReparsePointQueryResult result{};
            result.errorText = L"路径为空。";
            result.win32Error = ERROR_INVALID_PARAMETER;
            return result;
        }

        const DWORD attributes = ::GetFileAttributesW(nativePathText.toStdWString().c_str());
        const bool directoryHint = attributes != INVALID_FILE_ATTRIBUTES
            && ((attributes & FILE_ATTRIBUTE_DIRECTORY) != 0U);
        return ks::file::QueryReparsePointInfo(nativePathText.toStdWString(), directoryHint);
    }

    QString reparseKindMarkerForPath(const QString& path)
    {
        const QString nativePathText = QDir::toNativeSeparators(path).trimmed();
        if (nativePathText.isEmpty())
        {
            return QString();
        }

        const DWORD attributes = ::GetFileAttributesW(nativePathText.toStdWString().c_str());
        if (attributes == INVALID_FILE_ATTRIBUTES || (attributes & FILE_ATTRIBUTE_REPARSE_POINT) == 0U)
        {
            return QString();
        }

        const ks::file::ReparsePointQueryResult result = queryReparsePointForUi(nativePathText);
        const QString kindText = QString::fromStdWString(result.kindName).trimmed();
        return kindText.isEmpty() ? QStringLiteral("UNKNOWN_REPARSE") : kindText;
    }

    QString reparseTargetFromResult(const ks::file::ReparsePointQueryResult& result)
    {
        QString targetText = QString::fromStdWString(result.resolvedTargetPath).trimmed();
        if (targetText.isEmpty())
        {
            targetText = QString::fromStdWString(result.printName).trimmed();
        }
        if (targetText.isEmpty())
        {
            targetText = QString::fromStdWString(result.substituteName).trimmed();
        }
        return QDir::toNativeSeparators(targetText);
    }

    QString formatReparsePointText(const QString& path)
    {
        const ks::file::ReparsePointQueryResult result = queryReparsePointForUi(path);
        QString content;
        content += QStringLiteral("目标路径: %1\n").arg(QDir::toNativeSeparators(path));

        if (!result.pathOpened)
        {
            content += QStringLiteral("状态: 无法打开目标（使用 FILE_FLAG_OPEN_REPARSE_POINT）\n");
            content += QStringLiteral("Win32错误: %1\n").arg(formatWin32ErrorText(result.win32Error));
            content += QStringLiteral("原因: %1\n").arg(QString::fromStdWString(result.errorText));
            return content;
        }

        if (!result.querySucceeded)
        {
            content += QStringLiteral("状态: FSCTL_GET_REPARSE_POINT 查询失败\n");
            content += QStringLiteral("是否重解析点: %1\n").arg(result.isReparsePoint ? QStringLiteral("是") : QStringLiteral("否"));
            content += QStringLiteral("Win32错误: %1\n").arg(formatWin32ErrorText(result.win32Error));
            content += QStringLiteral("原因: %1\n").arg(QString::fromStdWString(result.errorText));
            return content;
        }

        content += QStringLiteral("状态: OK\n");
        content += QStringLiteral("Reparse Tag: %1\n").arg(formatReparseTagText(result.tag));
        content += QStringLiteral("Tag名称: %1\n").arg(QString::fromStdWString(result.tagName));
        content += QStringLiteral("类型标记: %1\n").arg(QString::fromStdWString(result.kindName));
        content += QStringLiteral("Microsoft Tag: %1\n").arg(result.isMicrosoftTag ? QStringLiteral("是") : QStringLiteral("否"));
        content += QStringLiteral("Name Surrogate: %1\n").arg(result.isNameSurrogate ? QStringLiteral("是") : QStringLiteral("否"));
        content += QStringLiteral("是否相对链接: %1\n").arg(result.isRelative ? QStringLiteral("是") : QStringLiteral("否"));
        content += QStringLiteral("Substitute Name: %1\n").arg(QString::fromStdWString(result.substituteName));
        content += QStringLiteral("Print Name: %1\n").arg(QString::fromStdWString(result.printName));
        content += QStringLiteral("解析目标路径: %1\n").arg(reparseTargetFromResult(result));
        content += QStringLiteral("原始信息: %1\n").arg(QString::fromStdWString(result.rawPayloadText));
        content += QStringLiteral("Raw Hex Preview: %1\n").arg(QString::fromStdWString(result.rawHexPreview));
        if (!result.errorText.empty())
        {
            content += QStringLiteral("解析提示: %1\n").arg(QString::fromStdWString(result.errorText));
        }
        return content;
    }

    class ReparseAwareFileSystemModel final : public QFileSystemModel
    {
    public:
        explicit ReparseAwareFileSystemModel(QObject* parent = nullptr)
            : QFileSystemModel(parent)
        {
        }

        QVariant data(const QModelIndex& index, const int role = Qt::DisplayRole) const override
        {
            const QVariant baseValue = QFileSystemModel::data(index, role);
            if (!index.isValid())
            {
                return baseValue;
            }

            if (role != Qt::DisplayRole && role != Qt::ToolTipRole)
            {
                return baseValue;
            }

            const QString markerText = reparseKindMarkerForPath(filePath(index));
            if (markerText.isEmpty())
            {
                return baseValue;
            }

            if (role == Qt::ToolTipRole)
            {
                QString toolTipText = baseValue.toString();
                if (!toolTipText.isEmpty())
                {
                    toolTipText += QLatin1Char('\n');
                }
                toolTipText += QStringLiteral("重解析点: %1").arg(markerText);
                return toolTipText;
            }

            if (index.column() == 0)
            {
                return QStringLiteral("%1 [%2]").arg(baseValue.toString(), markerText);
            }
            if (index.column() == 2)
            {
                const QString typeText = baseValue.toString().trimmed();
                return typeText.isEmpty()
                    ? markerText
                    : QStringLiteral("%1 / %2").arg(markerText, typeText);
            }
            return baseValue;
        }
    };

    // buildDriverNtPath：
    // - 作用：把 Win32 路径转成驱动可直接使用的 NT 路径；
    // - 规则：普通盘符路径转成 \??\C:\...，UNC 路径转成 \??\UNC\...
    QString buildDriverNtPath(const QString& path)
    {
        const QString nativePathText = QDir::toNativeSeparators(path).trimmed();
        if (nativePathText.isEmpty())
        {
            return QString();
        }
        if (nativePathText.startsWith(QStringLiteral("\\??\\")))
        {
            return nativePathText;
        }
        if (nativePathText.startsWith(QStringLiteral("\\\\?\\")))
        {
            return QStringLiteral("\\??\\") + nativePathText.mid(4);
        }
        if (nativePathText.startsWith(QStringLiteral("\\Device\\")))
        {
            return nativePathText;
        }
        if (nativePathText.startsWith(QStringLiteral("\\\\")))
        {
            return QStringLiteral("\\??\\UNC\\") + nativePathText.mid(2);
        }
        return QStringLiteral("\\??\\") + nativePathText;
    }

    // buildLiteralNameFilterPattern：
    // - 作用：把关键字转成 QFileSystemModel::setNameFilters 可用的“包含匹配”通配符；
    // - 说明：对 *, ?, [, ] 做转义，避免用户输入被当作通配符语法。
    QString buildLiteralNameFilterPattern(const QString& keywordText)
    {
        QString escapedKeyword = keywordText;
        escapedKeyword.replace(QStringLiteral("["), QStringLiteral("[[]"));
        escapedKeyword.replace(QStringLiteral("]"), QStringLiteral("[]]"));
        escapedKeyword.replace(QStringLiteral("*"), QStringLiteral("[*]"));
        escapedKeyword.replace(QStringLiteral("?"), QStringLiteral("[?]"));
        return QStringLiteral("*%1*").arg(escapedKeyword);
    }

    // queryShortPathText：
    // - 作用：查询目标路径对应的 Win32 短路径（8.3）；
    // - 失败时返回空字符串，调用方可决定是否走原名兜底。
    QString queryShortPathText(const QString& path)
    {
        const std::wstring nativePathText = QDir::toNativeSeparators(path).toStdWString();
        if (nativePathText.empty())
        {
            return QString();
        }

        const DWORD requiredChars = ::GetShortPathNameW(nativePathText.c_str(), nullptr, 0);
        if (requiredChars == 0)
        {
            return QString();
        }

        QVector<wchar_t> shortPathBuffer(static_cast<int>(requiredChars) + 2, L'\0');
        const DWORD copiedChars = ::GetShortPathNameW(
            nativePathText.c_str(),
            shortPathBuffer.data(),
            static_cast<DWORD>(shortPathBuffer.size()));
        if (copiedChars == 0 || copiedChars >= static_cast<DWORD>(shortPathBuffer.size()))
        {
            return QString();
        }
        return QString::fromWCharArray(shortPathBuffer.data(), static_cast<int>(copiedChars));
    }

    // openKswordArkDriverHandle：
    // - 作用：通过 ArkDriverClient 连接 KswordARK 控制设备；
    // - 返回 move-only 句柄对象，避免 Dock 直接 CloseHandle。
    ksword::ark::DriverHandle openKswordArkDriverHandle(std::string* const detailTextOut)
    {
        if (detailTextOut != nullptr)
        {
            detailTextOut->clear();
        }

        const ksword::ark::DriverClient driverClient;
        ksword::ark::DriverHandle driverHandle = driverClient.open();
        if (driverHandle.isValid())
        {
            return driverHandle;
        }

        if (detailTextOut != nullptr)
        {
            const DWORD lastError = ::GetLastError();
            std::ostringstream oss;
            oss << "open KswordARK driver failed, error=" << lastError;
            *detailTextOut = oss.str();
        }
        return driverHandle;
    }

    // deletePathByR0Driver：
    // - 作用：向 ArkDriverClient 发送“删除单一路径”IOCTL；
    // - 参数 isDirectory 用于驱动端选择目录/文件打开语义。
    bool deletePathByR0Driver(
        ksword::ark::DriverHandle& driverHandle,
        const QString& path,
        const bool isDirectory,
        std::string* const detailTextOut)
    {
        if (detailTextOut != nullptr)
        {
            detailTextOut->clear();
        }

        if (!driverHandle.isValid())
        {
            if (detailTextOut != nullptr)
            {
                *detailTextOut = "invalid driver handle";
            }
            return false;
        }

        const QString driverNtPath = buildDriverNtPath(path);
        if (driverNtPath.isEmpty())
        {
            if (detailTextOut != nullptr)
            {
                *detailTextOut = "empty path";
            }
            return false;
        }

        const std::wstring ntPathText = driverNtPath.toStdWString();
        const ksword::ark::DriverClient driverClient;
        const ksword::ark::IoResult result = driverClient.deletePath(driverHandle, ntPathText, isDirectory);
        if (detailTextOut != nullptr)
        {
            std::ostringstream oss;
            oss << "path=" << QDir::toNativeSeparators(path).toStdString()
                << ", directory=" << (isDirectory ? 1 : 0)
                << ", bytesReturned=" << result.bytesReturned;
            if (result.ok)
            {
                oss << ", ioctl=ok";
            }
            else
            {
                oss << ", ioctl=fail, error=" << result.win32Error;
                if (!result.message.empty())
                {
                    oss << ", detail=" << result.message;
                }
            }
            *detailTextOut = oss.str();
        }
        return result.ok;
    }

    // terminateProcessByR0Driver：
    // - 作用：复用同一个 ArkDriverClient 句柄发送结束进程 IOCTL；
    // - 返回值：true=驱动返回成功，false=驱动返回失败或句柄无效。
    bool terminateProcessByR0Driver(
        ksword::ark::DriverHandle& driverHandle,
        const std::uint32_t processId,
        std::string* const detailTextOut)
    {
        if (detailTextOut != nullptr)
        {
            detailTextOut->clear();
        }

        if (!driverHandle.isValid())
        {
            if (detailTextOut != nullptr)
            {
                *detailTextOut = "invalid driver handle";
            }
            return false;
        }

        if (processId == 0U || processId <= 4U)
        {
            if (detailTextOut != nullptr)
            {
                *detailTextOut = "invalid target pid";
            }
            return false;
        }

        const ksword::ark::DriverClient driverClient;
        const ksword::ark::IoResult result = driverClient.terminateProcess(
            driverHandle,
            processId,
            static_cast<long>(0xC0000005u));
        if (detailTextOut != nullptr)
        {
            *detailTextOut = result.message;
        }
        return result.ok;
    }

    bool terminateProcessByR3(
        const std::uint32_t processId,
        std::string* const detailTextOut)
    {
        if (detailTextOut != nullptr)
        {
            detailTextOut->clear();
        }

        if (processId == 0U || processId <= 4U || processId == static_cast<std::uint32_t>(::GetCurrentProcessId()))
        {
            if (detailTextOut != nullptr)
            {
                *detailTextOut = "invalid target pid";
            }
            return false;
        }

        // 统一复用 ks::process 模块，确保各页面结束进程策略、返回语义与错误描述保持一致。
        std::string terminateErrorText;
        const bool terminateOk = ks::process::TerminateProcessByWin32(processId, &terminateErrorText);

        if (detailTextOut != nullptr)
        {
            std::ostringstream oss;
            oss << "pid=" << processId;
            if (terminateOk)
            {
                oss << ", TerminateProcess=ok";
            }
            else
            {
                oss << ", TerminateProcess=fail, detail=" << terminateErrorText;
            }
            *detailTextOut = oss.str();
        }
        return terminateOk;
    }

    // showUnlockSelectionDialog：
    // - 作用：展示文件解锁器扫描结果，并让用户选择关闭句柄或结束进程；
    // - 参数 parent：父窗口，用于模态弹窗归属；
    // - 参数 processCandidateList：按 PID 聚合后的占用进程列表；
    // - 参数 handleCandidateList：按 PID+Handle 展开的可关闭句柄列表；
    // - 返回：用户确认的操作模式与选中目标；取消时 accepted=false。
    UnlockSelectionResult showUnlockSelectionDialog(
        QWidget* const parent,
        const std::vector<UnlockProcessCandidate>& processCandidateList,
        const std::vector<UnlockHandleCandidate>& handleCandidateList)
    {
        UnlockSelectionResult result;
        if (processCandidateList.empty() && handleCandidateList.empty())
        {
            return result;
        }

        QDialog dialog(parent);
        dialog.setObjectName(QStringLiteral("FileUnlockerSelectionDialog"));
        dialog.setStyleSheet(buildOpaqueStandaloneDialogStyle(dialog.objectName()));
        dialog.setWindowTitle(QStringLiteral("文件解锁器 - 选择操作目标"));
        dialog.resize(1080, 620);

        QVBoxLayout* const rootLayout = new QVBoxLayout(&dialog);
        QLabel* const tipLabel = new QLabel(
            QStringLiteral("已扫描到以下占用来源。建议先关闭选中句柄；若仍无法删除/重命名，再改用结束进程兜底。未勾选的目标不会处理。"),
            &dialog);
        tipLabel->setWordWrap(true);
        rootLayout->addWidget(tipLabel);

        QHBoxLayout* const modeLayout = new QHBoxLayout();
        QLabel* const modeLabel = new QLabel(QStringLiteral("操作方式："), &dialog);
        QComboBox* const modeComboBox = new QComboBox(&dialog);
        const bool hasClosableHandle = std::any_of(
            handleCandidateList.begin(),
            handleCandidateList.end(),
            [](const UnlockHandleCandidate& candidate) {
                return candidate.handleValue != 0U
                    && candidate.processId > 4U
                    && !candidate.isCurrentProcess
                    && !candidate.isCriticalProcess;
            });
        modeComboBox->addItem(QStringLiteral("关闭选中句柄(R3，推荐先尝试)"));
        modeComboBox->addItem(QStringLiteral("结束选中进程(R3)"));
        modeComboBox->addItem(QStringLiteral("结束选中进程(R0，更强力)"));
        if (!hasClosableHandle)
        {
            modeComboBox->setCurrentIndex(1);
        }
        modeLayout->addWidget(modeLabel);
        modeLayout->addWidget(modeComboBox, 1);
        rootLayout->addLayout(modeLayout);

        QStackedWidget* const tableStack = new QStackedWidget(&dialog);
        QTableWidget* const handleTable = new QTableWidget(static_cast<int>(handleCandidateList.size()), 7, &dialog);
        handleTable->setHorizontalHeaderLabels(QStringList{
            QStringLiteral("选择"),
            QStringLiteral("PID"),
            QStringLiteral("进程名"),
            QStringLiteral("Handle"),
            QStringLiteral("GrantedAccess"),
            QStringLiteral("命中路径"),
            QStringLiteral("说明") });
        handleTable->setSelectionBehavior(QAbstractItemView::SelectRows);
        handleTable->setSelectionMode(QAbstractItemView::ExtendedSelection);
        handleTable->setEditTriggers(QAbstractItemView::NoEditTriggers);
        handleTable->verticalHeader()->setVisible(false);
        handleTable->horizontalHeader()->setStretchLastSection(true);

        QTableWidget* const processTable = new QTableWidget(static_cast<int>(processCandidateList.size()), 6, &dialog);
        processTable->setHorizontalHeaderLabels(QStringList{
            QStringLiteral("选择"),
            QStringLiteral("PID"),
            QStringLiteral("进程名"),
            QStringLiteral("命中数"),
            QStringLiteral("命中路径"),
            QStringLiteral("说明") });
        processTable->setSelectionBehavior(QAbstractItemView::SelectRows);
        processTable->setSelectionMode(QAbstractItemView::ExtendedSelection);
        processTable->setEditTriggers(QAbstractItemView::NoEditTriggers);
        processTable->verticalHeader()->setVisible(false);
        processTable->horizontalHeader()->setStretchLastSection(true);

        auto makeTableItem = [](const QString& text, const bool enabled) {
            QTableWidgetItem* const item = new QTableWidgetItem(text);
            item->setFlags(enabled
                ? (Qt::ItemIsSelectable | Qt::ItemIsEnabled)
                : Qt::ItemIsSelectable);
            return item;
            };

        for (int row = 0; row < static_cast<int>(handleCandidateList.size()); ++row)
        {
            const UnlockHandleCandidate& candidate = handleCandidateList[static_cast<std::size_t>(row)];
            const bool canCloseHandle = candidate.handleValue != 0U
                && !candidate.isCurrentProcess
                && !candidate.isCriticalProcess
                && candidate.processId > 4U;

            QTableWidgetItem* const checkItem = new QTableWidgetItem();
            checkItem->setCheckState(Qt::Unchecked);
            checkItem->setData(Qt::UserRole, row);
            checkItem->setFlags(canCloseHandle
                ? (Qt::ItemIsUserCheckable | Qt::ItemIsSelectable | Qt::ItemIsEnabled)
                : (Qt::ItemIsUserCheckable | Qt::ItemIsSelectable));
            handleTable->setItem(row, 0, checkItem);

            QStringList noteList;
            appendUniqueText(noteList, candidate.objectName);
            appendUniqueText(noteList, candidate.processImagePath);
            appendUniqueText(noteList, candidate.matchRuleText);
            appendUniqueText(noteList, candidate.enumerationSource);
            if (candidate.handleValue == 0U)
            {
                noteList.push_back(QStringLiteral("无句柄值：该来源可能是进程映像/模块映射，不能用 R3 关闭句柄处理"));
            }
            if (candidate.isCurrentProcess)
            {
                noteList.push_back(QStringLiteral("已保护：当前 Ksword 进程，不可关闭句柄"));
            }
            if (candidate.isCriticalProcess)
            {
                noteList.push_back(QStringLiteral("已保护：关键系统进程，不可关闭句柄"));
            }

            handleTable->setItem(row, 1, makeTableItem(QString::number(candidate.processId), canCloseHandle));
            handleTable->setItem(row, 2, makeTableItem(candidate.processName.isEmpty() ? QStringLiteral("Unknown") : candidate.processName, canCloseHandle));
            handleTable->setItem(row, 3, makeTableItem(formatHandleValueText(candidate.handleValue), canCloseHandle));
            handleTable->setItem(row, 4, makeTableItem(formatHandleValueText(candidate.grantedAccess), canCloseHandle));
            handleTable->setItem(row, 5, makeTableItem(candidate.matchedTargetPath, canCloseHandle));
            handleTable->setItem(row, 6, makeTableItem(noteList.join(QStringLiteral("\n")), canCloseHandle));
        }

        for (int row = 0; row < static_cast<int>(processCandidateList.size()); ++row)
        {
            const UnlockProcessCandidate& candidate = processCandidateList[static_cast<std::size_t>(row)];
            const bool protectedProcess = candidate.isCurrentProcess || candidate.isCriticalProcess;

            QTableWidgetItem* const checkItem = new QTableWidgetItem();
            checkItem->setCheckState(Qt::Unchecked);
            checkItem->setData(Qt::UserRole, static_cast<qulonglong>(candidate.processId));
            checkItem->setFlags(protectedProcess
                ? (Qt::ItemIsUserCheckable | Qt::ItemIsSelectable)
                : (Qt::ItemIsUserCheckable | Qt::ItemIsSelectable | Qt::ItemIsEnabled));
            processTable->setItem(row, 0, checkItem);

            auto makeTextItem = [protectedProcess](const QString& text) {
                QTableWidgetItem* const item = new QTableWidgetItem(text);
                item->setFlags(protectedProcess
                    ? Qt::ItemIsSelectable
                    : (Qt::ItemIsSelectable | Qt::ItemIsEnabled));
                return item;
                };

            QStringList noteList;
            appendUniqueText(noteList, candidate.processImagePath);
            for (const QString& ruleText : candidate.matchRuleList)
            {
                appendUniqueText(noteList, ruleText);
            }
            if (candidate.isCurrentProcess)
            {
                noteList.push_back(QStringLiteral("已保护：当前 Ksword 进程，不可选择"));
            }
            if (candidate.isCriticalProcess)
            {
                noteList.push_back(QStringLiteral("已保护：关键系统进程，不可选择"));
            }

            processTable->setItem(row, 1, makeTextItem(QString::number(candidate.processId)));
            processTable->setItem(row, 2, makeTextItem(candidate.processName.isEmpty() ? QStringLiteral("Unknown") : candidate.processName));
            processTable->setItem(row, 3, makeTextItem(QString::number(candidate.matchCount)));
            processTable->setItem(row, 4, makeTextItem(candidate.matchedTargetList.join(QStringLiteral("\n"))));
            processTable->setItem(row, 5, makeTextItem(noteList.join(QStringLiteral("\n"))));
        }

        handleTable->resizeColumnsToContents();
        processTable->resizeColumnsToContents();
        tableStack->addWidget(handleTable);
        tableStack->addWidget(processTable);
        tableStack->setCurrentIndex(modeComboBox->currentIndex() == 0 ? 0 : 1);
        rootLayout->addWidget(tableStack, 1);

        QDialogButtonBox* const buttonBox = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dialog);
        QPushButton* const selectAllButton = buttonBox->addButton(QStringLiteral("全选当前可操作项"), QDialogButtonBox::ActionRole);
        QPushButton* const clearButton = buttonBox->addButton(QStringLiteral("清空选择"), QDialogButtonBox::ActionRole);
        buttonBox->button(QDialogButtonBox::Ok)->setText(modeComboBox->currentIndex() == 0
            ? QStringLiteral("关闭选中句柄")
            : QStringLiteral("执行选中操作"));
        buttonBox->button(QDialogButtonBox::Cancel)->setText(QStringLiteral("取消"));
        rootLayout->addWidget(buttonBox);

        auto collectSelectedHandles = [&handleTable, &handleCandidateList]() {
            std::vector<UnlockHandleCandidate> selectedHandleList;
            for (int row = 0; row < handleTable->rowCount(); ++row)
            {
                QTableWidgetItem* const item = handleTable->item(row, 0);
                if (item == nullptr
                    || !(item->flags() & Qt::ItemIsEnabled)
                    || item->checkState() != Qt::Checked)
                {
                    continue;
                }
                selectedHandleList.push_back(handleCandidateList[static_cast<std::size_t>(row)]);
            }
            return selectedHandleList;
            };

        auto collectSelectedIds = [&processTable, &processCandidateList]() {
            std::vector<std::uint32_t> selectedProcessIdList;
            for (int row = 0; row < processTable->rowCount(); ++row)
            {
                QTableWidgetItem* const item = processTable->item(row, 0);
                if (item == nullptr
                    || !(item->flags() & Qt::ItemIsEnabled)
                    || item->checkState() != Qt::Checked)
                {
                    continue;
                }
                selectedProcessIdList.push_back(processCandidateList[static_cast<std::size_t>(row)].processId);
            }
            return selectedProcessIdList;
            };

        auto setCheckedForTable = [](QTableWidget* const targetTable, const Qt::CheckState checkState) {
            for (int row = 0; row < targetTable->rowCount(); ++row)
            {
                QTableWidgetItem* const item = targetTable->item(row, 0);
                if (item != nullptr && (item->flags() & Qt::ItemIsEnabled))
                {
                    item->setCheckState(checkState);
                }
            }
            };
        QObject::connect(selectAllButton, &QPushButton::clicked, [&modeComboBox, &handleTable, &processTable, &setCheckedForTable]() {
            setCheckedForTable(modeComboBox->currentIndex() == 0 ? handleTable : processTable, Qt::Checked);
            });
        QObject::connect(clearButton, &QPushButton::clicked, [&modeComboBox, &handleTable, &processTable, &setCheckedForTable]() {
            setCheckedForTable(modeComboBox->currentIndex() == 0 ? handleTable : processTable, Qt::Unchecked);
            });
        QObject::connect(modeComboBox, QOverload<int>::of(&QComboBox::currentIndexChanged),
            [&tableStack, &buttonBox](const int modeIndex) {
                tableStack->setCurrentIndex(modeIndex == 0 ? 0 : 1);
                buttonBox->button(QDialogButtonBox::Ok)->setText(modeIndex == 0
                    ? QStringLiteral("关闭选中句柄")
                    : QStringLiteral("执行选中操作"));
            });
        QObject::connect(buttonBox, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
        QObject::connect(buttonBox, &QDialogButtonBox::accepted, [&dialog, &modeComboBox, &collectSelectedHandles, &collectSelectedIds]() {
            if (modeComboBox->currentIndex() == 0 && collectSelectedHandles().empty())
            {
                QMessageBox::information(
                    &dialog,
                    QStringLiteral("文件解锁器"),
                    QStringLiteral("请至少选择一个要关闭的句柄。"));
                return;
            }
            if (modeComboBox->currentIndex() != 0 && collectSelectedIds().empty())
            {
                QMessageBox::information(
                    &dialog,
                    QStringLiteral("文件解锁器"),
                    QStringLiteral("请至少选择一个要结束的进程。"));
                return;
            }
            dialog.accept();
            });

        if (dialog.exec() != QDialog::Accepted)
        {
            return result;
        }

        result.accepted = true;
        result.operationMode = modeComboBox->currentIndex() == 0
            ? UnlockOperationMode::CloseHandleR3
            : (modeComboBox->currentIndex() == 2
                ? UnlockOperationMode::TerminateProcessR0
                : UnlockOperationMode::TerminateProcessR3);
        if (result.operationMode == UnlockOperationMode::CloseHandleR3)
        {
            result.selectedHandleList = collectSelectedHandles();
        }
        else
        {
            result.selectedProcessIdList = collectSelectedIds();
        }
        return result;
    }

    // collectOccupyProcessIdsByPath：
    // - 作用：调用现有占用扫描器，提取“占用目标路径”的 PID 集合；
    // - 说明：这里运行在 R3，结果只用于展示/诊断，不能隐式结束进程。
    std::vector<std::uint32_t> collectOccupyProcessIdsByPath(
        const QString& path,
        QStringList* const detailTextListOut)
    {
        if (detailTextListOut != nullptr)
        {
            detailTextListOut->clear();
        }

        const std::vector<QString> scanTargets{ path };
        const filedock::handleusage::HandleUsageScanResult scanResult =
            filedock::handleusage::scanHandleUsageByPaths(
                scanTargets,
                0,
                false);

        std::set<std::uint32_t> processIdSet;
        QStringList processPreviewList;
        constexpr std::size_t MaxPreviewCount = 6U;
        for (const filedock::handleusage::HandleUsageEntry& entry : scanResult.entries)
        {
            if (entry.processId == 0U || entry.processId <= 4U)
            {
                continue;
            }

            const std::uint32_t processId = entry.processId;
            const auto insertResult = processIdSet.insert(processId);
            if (!insertResult.second)
            {
                continue;
            }

            if (processPreviewList.size() < static_cast<int>(MaxPreviewCount))
            {
                const QString processName =
                    entry.processName.trimmed().isEmpty()
                    ? QStringLiteral("Unknown")
                    : entry.processName.trimmed();
                processPreviewList.push_back(
                    QStringLiteral("%1(%2)").arg(processName).arg(processId));
            }
        }

        if (detailTextListOut != nullptr)
        {
            const QString diagnosticText = scanResult.diagnosticText.trimmed().isEmpty()
                ? QStringLiteral("-")
                : scanResult.diagnosticText.simplified();
            detailTextListOut->push_back(
                QStringLiteral("occupyScan matched=%1, diagnostic=%2")
                .arg(scanResult.matchedHandleCount)
                .arg(diagnosticText));

            if (!processPreviewList.isEmpty())
            {
                detailTextListOut->push_back(
                    QStringLiteral("occupyPidPreview=%1")
                    .arg(processPreviewList.join(QStringLiteral(", "))));
            }
        }

        return std::vector<std::uint32_t>(processIdSet.begin(), processIdSet.end());
    }

    // appendDriverDeleteTargetsPostOrder：
    // - 作用：把目录展开成“子项先删、目录后删”的后序列表；
    // - 说明：重解析点目录不递归进入，只删除链接本身。
    bool appendDriverDeleteTargetsPostOrder(
        const QString& rootPath,
        std::vector<DriverDeleteTarget>& targetsOut,
        QString& errorTextOut)
    {
        const QFileInfo rootInfo(rootPath);
        const bool rootExists = rootInfo.exists() || rootInfo.isSymLink();
        if (!rootExists)
        {
            errorTextOut = QStringLiteral("路径不存在：%1").arg(QDir::toNativeSeparators(rootPath));
            return false;
        }

        const bool isDirectory = rootInfo.isDir();
        const bool isReparsePoint = isPathReparsePoint(rootPath);
        if (isDirectory && !isReparsePoint)
        {
            const QFileInfoList childInfoList = QDir(rootPath).entryInfoList(
                QDir::NoDotAndDotDot | QDir::AllEntries | QDir::Hidden | QDir::System,
                QDir::DirsFirst | QDir::Name);
            for (const QFileInfo& childInfo : childInfoList)
            {
                if (!appendDriverDeleteTargetsPostOrder(childInfo.absoluteFilePath(), targetsOut, errorTextOut))
                {
                    return false;
                }
            }
        }

        targetsOut.push_back(DriverDeleteTarget{ rootPath, isDirectory });
        return true;
    }

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
        case ks::file::ManualFsType::ExFat:
            return QStringLiteral("exFAT");
        default:
            return QStringLiteral("Unknown");
        }
    }

    // 统一按钮样式，保持与主界面蓝色主题一致。
    QString buildBlueButtonStyle()
    {
        return KswordTheme::ThemedButtonStyle();
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

    // buildContextMenuStyle 作用：
    // - 为 FileDock 文件列表右键菜单生成独立主题样式；
    // - 修复浅色主题下菜单背景错误保持黑色，导致文字不可见的问题。
    QString buildContextMenuStyle()
    {
        const QString disabledTextColor = KswordTheme::IsDarkModeEnabled()
            ? QStringLiteral("#8C8C8C")
            : QStringLiteral("#7A8694");

        return QStringLiteral(
            "QMenu{"
            "  background:%1;"
            "  color:%2;"
            "  border:1px solid %3;"
            "}"
            "QMenu::item{"
            "  padding:3px 16px 3px 12px;"
            "  background:transparent;"
            "}"
            "QMenu::item:selected{"
            "  background:%4;"
            "  color:#FFFFFF;"
            "}"
            "QMenu::item:disabled{"
            "  color:%5;"
            "  background:transparent;"
            "}"
            "QMenu::separator{"
            "  height:1px;"
            "  background:%3;"
            "  margin:2px 6px;"
            "}")
            .arg(KswordTheme::SurfaceHex())
            .arg(KswordTheme::TextPrimaryHex())
            .arg(KswordTheme::BorderHex())
            .arg(KswordTheme::PrimaryBlueHex)
            .arg(disabledTextColor);
    }

    // buildOpaqueStandaloneDialogStyle 作用：
    // - 为“独立弹窗”覆盖父级 Dock 透明样式，防止浅色主题下出现黑底；
    // - 强制编辑区/表格区使用 palette(base) 作为不透明背景。
    QString buildOpaqueStandaloneDialogStyle(const QString& dialogObjectName)
    {
        return QStringLiteral(
            "QDialog#%1{"
            "  background-color:palette(window) !important;"
            "  color:palette(text) !important;"
            "}"
            "QDialog#%1 QTabWidget::pane{"
            "  background-color:palette(window) !important;"
            "  border:1px solid palette(mid) !important;"
            "}"
            "QDialog#%1 QPlainTextEdit,"
            "QDialog#%1 QTextEdit,"
            "QDialog#%1 QTreeWidget,"
            "QDialog#%1 QTableWidget,"
            "QDialog#%1 QAbstractScrollArea,"
            "QDialog#%1 QAbstractScrollArea::viewport{"
            "  background-color:palette(base) !important;"
            "  color:palette(text) !important;"
            "}"
            "QDialog#%1 QHeaderView::section{"
            "  background-color:palette(window) !important;"
            "  color:palette(text) !important;"
            "}")
            .arg(dialogObjectName);
    }

    // buildLogPreviewText 作用：
    // - 把多行结果压缩成单条日志预览文本，避免批量错误把日志面板刷满；
    // - 参数 sourceLines：原始明细文本集合；参数 maxLineCount：最多保留的行数；
    // - 返回：适合直接写入日志的 QString。
    QString buildLogPreviewText(const QStringList& sourceLines, const int maxLineCount = 8)
    {
        if (sourceLines.isEmpty())
        {
            return QStringLiteral("(空)");
        }

        QStringList previewLines;
        const int sourceLineCount = static_cast<int>(sourceLines.size());
        const int previewCount = sourceLineCount < maxLineCount
            ? sourceLineCount
            : maxLineCount;
        previewLines.reserve(previewCount + 1);
        for (int index = 0; index < previewCount; ++index)
        {
            previewLines.push_back(sourceLines[index]);
        }
        if (sourceLines.size() > previewCount)
        {
            previewLines.push_back(
                QStringLiteral("... 其余 %1 行省略").arg(sourceLines.size() - previewCount));
        }
        return previewLines.join(QStringLiteral("\n"));
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
            "  color:%1;"
            "  border-radius:3px;"
            "}"
            "QToolButton:pressed{"
            "  background:%3;"
            "  color:#FFFFFF;"
            "}")
            .arg(KswordTheme::TextPrimaryHex())
            .arg(KswordTheme::SurfaceAltHex())
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

    struct FileSecurityAceRow
    {
        QString scopeText;       // scopeText：ACE 来源范围，当前主要为 DACL/SACL。
        QString typeText;        // typeText：ACE 类型文本，例如 ACCESS_ALLOWED。
        QString flagsText;       // flagsText：继承/审计标志文本。
        DWORD mask = 0;          // mask：原始访问掩码，用于显示和后续编辑定位。
        QString rightsText;      // rightsText：mask 拆解后的常见文件权限名。
        QString sidText;         // sidText：字符串 SID，便于稳定定位。
        QString accountText;     // accountText：LookupAccountSidW 解析出的账户名。
        DWORD aceIndex = 0;      // aceIndex：ACL 内 ACE 序号。
        bool canEdit = false;    // canEdit：当前 UI 是否允许对该 ACE 执行删除/替换。
    };

    struct FileSecuritySnapshot
    {
        bool descriptorOk = false;        // descriptorOk：Owner/Group/DACL 是否读取成功。
        bool saclOk = false;              // saclOk：SACL 是否读取成功。
        DWORD descriptorError = ERROR_SUCCESS; // descriptorError：读取 Owner/Group/DACL 的 Win32 错误码。
        DWORD saclError = ERROR_SUCCESS;       // saclError：读取 SACL 的 Win32 错误码。
        QString ownerSidText;             // ownerSidText：Owner SID 字符串。
        QString ownerAccountText;         // ownerAccountText：Owner 账户文本。
        QString groupSidText;             // groupSidText：Primary Group SID 字符串。
        QString groupAccountText;         // groupAccountText：Primary Group 账户文本。
        QString detailText;               // detailText：兼容旧版本的完整文本明细，读取失败也会保留错误。
        std::vector<FileSecurityAceRow> aceRows; // aceRows：表格化 ACE 列表。
    };

    // appendAclRows：
    // - 输入 scopeText/aclValue：ACL 名称与 Windows ACL 指针。
    // - 处理：解析支持的 ACE 结构并转换为 UI 表格行；未知 ACE 仍进入文本明细。
    // - 返回：无，解析出的行追加到 rowsOut。
    void appendAclRows(const QString& scopeText, PACL aclValue, std::vector<FileSecurityAceRow>& rowsOut)
    {
        if (aclValue == nullptr)
        {
            return;
        }

        ACL_SIZE_INFORMATION aclSizeInfo{};
        if (::GetAclInformation(
            aclValue,
            &aclSizeInfo,
            static_cast<DWORD>(sizeof(aclSizeInfo)),
            AclSizeInformation) == FALSE)
        {
            return;
        }

        for (DWORD aceIndex = 0; aceIndex < aclSizeInfo.AceCount; ++aceIndex)
        {
            LPVOID acePointer = nullptr;
            if (::GetAce(aclValue, aceIndex, &acePointer) == FALSE || acePointer == nullptr)
            {
                continue;
            }

            ACE_HEADER* aceHeader = reinterpret_cast<ACE_HEADER*>(acePointer);
            DWORD accessMask = 0;
            PSID aceSid = nullptr;
            bool editableAce = false;

            switch (aceHeader->AceType)
            {
            case ACCESS_ALLOWED_ACE_TYPE:
            {
                ACCESS_ALLOWED_ACE* aceBody = reinterpret_cast<ACCESS_ALLOWED_ACE*>(acePointer);
                accessMask = aceBody->Mask;
                aceSid = reinterpret_cast<PSID>(&aceBody->SidStart);
                editableAce = scopeText == QStringLiteral("DACL") && (aceHeader->AceFlags & INHERITED_ACE) == 0;
                break;
            }
            case ACCESS_DENIED_ACE_TYPE:
            {
                ACCESS_DENIED_ACE* aceBody = reinterpret_cast<ACCESS_DENIED_ACE*>(acePointer);
                accessMask = aceBody->Mask;
                aceSid = reinterpret_cast<PSID>(&aceBody->SidStart);
                editableAce = scopeText == QStringLiteral("DACL") && (aceHeader->AceFlags & INHERITED_ACE) == 0;
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

            FileSecurityAceRow row;
            row.scopeText = scopeText;
            row.typeText = aceTypeToText(aceHeader->AceType);
            row.flagsText = aceFlagsToText(aceHeader->AceFlags);
            row.mask = accessMask;
            row.rightsText = accessMaskToText(accessMask);
            row.sidText = sidToStringText(aceSid);
            row.accountText = sidToAccountText(aceSid);
            row.aceIndex = aceIndex;
            row.canEdit = editableAce && aceSid != nullptr;
            rowsOut.push_back(row);
        }
    }

    // createReadonlyTableItem：
    // - 输入 cellText：待展示文本。
    // - 处理：创建不可编辑表格单元格，避免权限表误触编辑。
    // - 返回：新建 QTableWidgetItem，由表格接管生命周期。
    QTableWidgetItem* createReadonlyTableItem(const QString& cellText)
    {
        QTableWidgetItem* item = new QTableWidgetItem(cellText);
        item->setFlags(item->flags() & ~Qt::ItemIsEditable);
        return item;
    }

    // formatAccessMaskHex：
    // - 输入 accessMask：Win32 文件访问掩码。
    // - 处理：统一格式化为 8 位十六进制。
    // - 返回：0x 前缀大写文本。
    QString formatAccessMaskHex(const DWORD accessMask)
    {
        return QStringLiteral("0x%1")
            .arg(accessMask, 8, 16, QLatin1Char('0'))
            .toUpper();
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
            m_hashCancelRequested = std::make_shared<std::atomic_bool>(false);
            setAttribute(Qt::WA_DeleteOnClose, true);
            setObjectName(QStringLiteral("FileDetailDialogRoot"));
            setAttribute(Qt::WA_StyledBackground, true);
            setAutoFillBackground(true);
            setStyleSheet(buildOpaqueStandaloneDialogStyle(objectName()));
            setWindowTitle(QStringLiteral("文件属性 - %1").arg(QFileInfo(filePath).fileName()));
            // 文件属性窗内容可能包含超长路径、证书链和 PE 字段：
            // - 最大宽度按父窗口客户区 75% 限制；
            // - 初始宽度同步裁剪，防止窗口被长文本撑出屏幕。
            applyFileStandaloneWindowWidthLimit(
                this,
                resolveVisibleDialogParent(parent),
                QSize(980, 680),
                0.75);

            QVBoxLayout* rootLayout = new QVBoxLayout(this);
            QTabWidget* tabWidget = new QTabWidget(this);
            rootLayout->addWidget(tabWidget, 1);

            tabWidget->addTab(buildGeneralTab(), QStringLiteral("常规信息"));
            tabWidget->addTab(buildDeferredTab(QStringLiteral("reparse")), QStringLiteral("重解析点 / 符号链接"));
            tabWidget->addTab(buildDeferredTab(QStringLiteral("security")), QStringLiteral("安全与权限"));
            tabWidget->addTab(buildHashTab(), QStringLiteral("哈希与完整性"));
            tabWidget->addTab(buildDeferredTab(QStringLiteral("usage")), QStringLiteral("文件占用"));
            tabWidget->addTab(buildDeferredTab(QStringLiteral("signature")), QStringLiteral("数字签名"));
            tabWidget->addTab(buildDeferredTab(QStringLiteral("pe")), QStringLiteral("PE信息"));
            tabWidget->addTab(buildDeferredTab(QStringLiteral("dependencies")), QStringLiteral("依赖 DLL"));
            tabWidget->addTab(buildDeferredTab(QStringLiteral("strings")), QStringLiteral("字符串"));
            tabWidget->addTab(buildDeferredTab(QStringLiteral("hex")), QStringLiteral("十六进制"));

            connect(tabWidget, &QTabWidget::currentChanged, this, [this, tabWidget](const int tabIndex)
                {
                    activateDeferredTab(tabWidget, tabIndex);
                });
        }

    private:
        struct HashCalculationResult
        {
            bool openOk = false;       // openOk：文件是否成功打开。
            bool cancelled = false;    // cancelled：用户是否取消。
            qint64 totalBytes = 0;     // totalBytes：文件总大小。
            qint64 readBytes = 0;      // readBytes：实际读取字节数。
            qint64 elapsedMs = 0;      // elapsedMs：耗时毫秒。
            QString sha256Text;        // sha256Text：十六进制 SHA256。
            QString errorText;         // errorText：失败原因。
        };

        static QString formatHex64(const std::uint64_t value)
        {
            // 用途：统一格式化 R0 诊断地址。
            // 返回：0x 前缀大写十六进制字符串。
            return QStringLiteral("0x%1")
                .arg(static_cast<qulonglong>(value), 0, 16)
                .toUpper();
        }

        static QString formatNtStatus(const long status)
        {
            // 用途：NTSTATUS 同时显示十六进制和十进制，便于对照 WinDbg。
            // 返回：例如 0xC0000034 (-1073741772)。
            return QStringLiteral("0x%1 (%2)")
                .arg(static_cast<qulonglong>(static_cast<std::uint32_t>(status)), 8, 16, QChar('0'))
                .arg(status)
                .toUpper();
        }

        static QString fileTimeToText(const std::int64_t fileTimeValue)
        {
            // 用途：把 Windows FILETIME 语义的 100ns 时间戳转为本地时间文本。
            // 返回：可读时间；0 表示不可用。
            if (fileTimeValue <= 0)
            {
                return QStringLiteral("<Unavailable>");
            }

            constexpr std::int64_t windowsToUnix100Ns = 116444736000000000LL;
            const std::int64_t unixMilliseconds = (fileTimeValue - windowsToUnix100Ns) / 10000LL;
            if (unixMilliseconds <= 0)
            {
                return QStringLiteral("<Invalid:%1>").arg(fileTimeValue);
            }
            // Qt 6.9 已废弃 Qt::TimeSpec 重载；这里显式使用 UTC 时区，
            // 再转换为本地时间，保持 FILETIME 展示语义不变。
            return QDateTime::fromMSecsSinceEpoch(unixMilliseconds, QTimeZone::UTC)
                .toLocalTime()
                .toString(QStringLiteral("yyyy-MM-dd HH:mm:ss.zzz"));
        }

        static QString fileAttributesToText(const std::uint32_t attributes)
        {
            // 用途：拆解 FILE_ATTRIBUTE_*，让 R0 和 R3 属性差异可读。
            // 返回：属性名列表；无显式位时返回 NORMAL/0。
            QStringList parts;
            if ((attributes & FILE_ATTRIBUTE_READONLY) != 0U) parts << QStringLiteral("READONLY");
            if ((attributes & FILE_ATTRIBUTE_HIDDEN) != 0U) parts << QStringLiteral("HIDDEN");
            if ((attributes & FILE_ATTRIBUTE_SYSTEM) != 0U) parts << QStringLiteral("SYSTEM");
            if ((attributes & FILE_ATTRIBUTE_DIRECTORY) != 0U) parts << QStringLiteral("DIRECTORY");
            if ((attributes & FILE_ATTRIBUTE_ARCHIVE) != 0U) parts << QStringLiteral("ARCHIVE");
            if ((attributes & FILE_ATTRIBUTE_DEVICE) != 0U) parts << QStringLiteral("DEVICE");
            if ((attributes & FILE_ATTRIBUTE_NORMAL) != 0U) parts << QStringLiteral("NORMAL");
            if ((attributes & FILE_ATTRIBUTE_TEMPORARY) != 0U) parts << QStringLiteral("TEMPORARY");
            if ((attributes & FILE_ATTRIBUTE_SPARSE_FILE) != 0U) parts << QStringLiteral("SPARSE_FILE");
            if ((attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0U) parts << QStringLiteral("REPARSE_POINT");
            if ((attributes & FILE_ATTRIBUTE_COMPRESSED) != 0U) parts << QStringLiteral("COMPRESSED");
            if ((attributes & FILE_ATTRIBUTE_OFFLINE) != 0U) parts << QStringLiteral("OFFLINE");
            if ((attributes & FILE_ATTRIBUTE_NOT_CONTENT_INDEXED) != 0U) parts << QStringLiteral("NOT_CONTENT_INDEXED");
            if ((attributes & FILE_ATTRIBUTE_ENCRYPTED) != 0U) parts << QStringLiteral("ENCRYPTED");
            if ((attributes & FILE_ATTRIBUTE_INTEGRITY_STREAM) != 0U) parts << QStringLiteral("INTEGRITY_STREAM");
            if ((attributes & FILE_ATTRIBUTE_NO_SCRUB_DATA) != 0U) parts << QStringLiteral("NO_SCRUB_DATA");
            if (parts.isEmpty())
            {
                parts << QStringLiteral("0");
            }
            return QStringLiteral("0x%1 (%2)")
                .arg(attributes, 8, 16, QChar('0'))
                .arg(parts.join(QStringLiteral("|")))
                .toUpper();
        }

        static QString fileInfoStatusText(const std::uint32_t status)
        {
            // 用途：把共享协议状态码翻译为 UI 文本。
            // 返回：状态名称。
            switch (status)
            {
            case KSWORD_ARK_FILE_INFO_STATUS_OK:
                return QStringLiteral("OK");
            case KSWORD_ARK_FILE_INFO_STATUS_PARTIAL:
                return QStringLiteral("Partial");
            case KSWORD_ARK_FILE_INFO_STATUS_OPEN_FAILED:
                return QStringLiteral("Open Failed");
            case KSWORD_ARK_FILE_INFO_STATUS_BASIC_FAILED:
                return QStringLiteral("Basic Failed");
            case KSWORD_ARK_FILE_INFO_STATUS_STANDARD_FAILED:
                return QStringLiteral("Standard Failed");
            case KSWORD_ARK_FILE_INFO_STATUS_OBJECT_FAILED:
                return QStringLiteral("Object Failed");
            case KSWORD_ARK_FILE_INFO_STATUS_NAME_FAILED:
                return QStringLiteral("Name Failed");
            default:
                return QStringLiteral("Unavailable");
            }
        }

        static ksword::ark::FileInfoQueryResult queryR0FileInfo(const QFileInfo& info, const QString& ntPathText)
        {
            // 用途：通过 ArkDriverClient 调用 R0 文件基础信息查询。
            // 返回：驱动不可用时 ok=false，常规页自动回退 R3 展示。
            ksword::ark::FileInfoQueryResult result{};
            if (ntPathText.trimmed().isEmpty())
            {
                result.io.ok = false;
                result.io.win32Error = ERROR_INVALID_PARAMETER;
                result.io.message = "empty nt path";
                return result;
            }

            unsigned long flags = KSWORD_ARK_QUERY_FILE_INFO_FLAG_INCLUDE_ALL;
            if (info.isDir())
            {
                flags |= KSWORD_ARK_QUERY_FILE_INFO_FLAG_DIRECTORY;
            }

            const ksword::ark::DriverClient driverClient;
            return driverClient.queryFileInfo(ntPathText.toStdWString(), flags);
        }

        QString formatR0FileInfoText(const ksword::ark::FileInfoQueryResult& result) const
        {
            // 用途：生成 R0 文件信息页文本。
            // 返回：包含状态、大小、时间戳、对象诊断地址和失败原因的多行文本。
            QString content;
            if (!result.io.ok)
            {
                content += QStringLiteral("状态: Unavailable\n");
                content += QStringLiteral("原因: %1\n").arg(QString::fromStdString(result.io.message));
                content += QStringLiteral("Win32错误: %1\n").arg(result.io.win32Error);
                return content;
            }

            content += QStringLiteral("协议版本: %1\n").arg(result.version);
            content += QStringLiteral("查询状态: %1 (%2)\n").arg(fileInfoStatusText(result.queryStatus)).arg(result.queryStatus);
            content += QStringLiteral("字段标志: 0x%1\n").arg(result.fieldFlags, 8, 16, QChar('0')).toUpper();
            content += QStringLiteral("OpenStatus: %1\n").arg(formatNtStatus(result.openStatus));
            content += QStringLiteral("BasicStatus: %1\n").arg(formatNtStatus(result.basicStatus));
            content += QStringLiteral("StandardStatus: %1\n").arg(formatNtStatus(result.standardStatus));
            content += QStringLiteral("ObjectStatus: %1\n").arg(formatNtStatus(result.objectStatus));
            content += QStringLiteral("NameStatus: %1\n").arg(formatNtStatus(result.nameStatus));
            content += QStringLiteral("大小(EndOfFile): %1 字节\n").arg(static_cast<qlonglong>(result.endOfFile));
            content += QStringLiteral("分配大小: %1 字节\n").arg(static_cast<qlonglong>(result.allocationSize));
            content += QStringLiteral("属性: %1\n").arg(fileAttributesToText(result.fileAttributes));
            content += QStringLiteral("创建时间: %1\n").arg(fileTimeToText(result.creationTime));
            content += QStringLiteral("最后访问: %1\n").arg(fileTimeToText(result.lastAccessTime));
            content += QStringLiteral("最后写入: %1\n").arg(fileTimeToText(result.lastWriteTime));
            content += QStringLiteral("ChangeTime: %1\n").arg(fileTimeToText(result.changeTime));
            content += QStringLiteral("FileObject: %1\n").arg(formatHex64(result.fileObjectAddress));
            content += QStringLiteral("SectionObjectPointers: %1\n").arg(formatHex64(result.sectionObjectPointersAddress));
            content += QStringLiteral("DataSectionObject: %1\n").arg(formatHex64(result.dataSectionObjectAddress));
            content += QStringLiteral("ImageSectionObject: %1\n").arg(formatHex64(result.imageSectionObjectAddress));
            content += QStringLiteral("R0消息: %1\n").arg(QString::fromStdString(result.io.message));
            return content;
        }

        void startHashCalculation(
            CodeEditorWidget* textEditorWidget,
            QProgressBar* progressBar,
            QPushButton* startButton,
            QPushButton* cancelButton)
        {
            // 用途：启动 SHA256 后台流式计算。
            // 处理：每块读取后检查取消标记并异步更新进度。
            // 返回：无；结果通过 QueuedConnection 回填 UI。
            if (textEditorWidget == nullptr || progressBar == nullptr ||
                startButton == nullptr || cancelButton == nullptr)
            {
                return;
            }

            if (m_hashCancelRequested == nullptr)
            {
                m_hashCancelRequested = std::make_shared<std::atomic_bool>(false);
            }
            m_hashCancelRequested->store(false);

            startButton->setEnabled(false);
            cancelButton->setEnabled(true);
            cancelButton->setText(QStringLiteral("取消"));
            progressBar->setValue(0);
            textEditorWidget->setText(QStringLiteral("正在计算 SHA256，请等待...\n目标: %1")
                .arg(QDir::toNativeSeparators(m_filePath)));

            const QString filePathSnapshot = m_filePath;
            const auto cancelFlag = m_hashCancelRequested;
            QPointer<FileDetailDialog> guardThis(this);
            QPointer<CodeEditorWidget> editorGuard(textEditorWidget);
            QPointer<QProgressBar> progressGuard(progressBar);
            QPointer<QPushButton> startGuard(startButton);
            QPointer<QPushButton> cancelGuard(cancelButton);

            auto* task = QRunnable::create([guardThis, editorGuard, progressGuard, startGuard, cancelGuard, filePathSnapshot, cancelFlag]()
                {
                    HashCalculationResult result{};
                    const auto beginTime = std::chrono::steady_clock::now();
                    QFile file(filePathSnapshot);
                    result.totalBytes = QFileInfo(filePathSnapshot).size();

                    if (!file.open(QIODevice::ReadOnly))
                    {
                        result.errorText = file.errorString();
                    }
                    else
                    {
                        result.openOk = true;
                        QCryptographicHash sha256(QCryptographicHash::Sha256);
                        constexpr qint64 chunkBytes = 1024 * 1024;
                        auto lastProgressTime = std::chrono::steady_clock::now();

                        while (!file.atEnd())
                        {
                            if (cancelFlag != nullptr && cancelFlag->load())
                            {
                                result.cancelled = true;
                                break;
                            }

                            const QByteArray chunk = file.read(chunkBytes);
                            if (chunk.isEmpty())
                            {
                                if (file.error() != QFileDevice::NoError)
                                {
                                    result.errorText = file.errorString();
                                }
                                break;
                            }

                            sha256.addData(chunk);
                            result.readBytes += chunk.size();

                            const auto nowTime = std::chrono::steady_clock::now();
                            const bool shouldReport =
                                std::chrono::duration_cast<std::chrono::milliseconds>(nowTime - lastProgressTime).count() >= 100;
                            if (shouldReport && guardThis != nullptr && progressGuard != nullptr)
                            {
                                lastProgressTime = nowTime;
                                const int progressValue = result.totalBytes > 0
                                    ? static_cast<int>((result.readBytes * 1000LL) / result.totalBytes)
                                    : 1000;
                                FileDetailDialog* targetDialog = guardThis.data();
                                if (targetDialog == nullptr)
                                {
                                    continue;
                                }
                                QMetaObject::invokeMethod(
                                    targetDialog,
                                    [progressGuard, progressValue]()
                                    {
                                        if (progressGuard != nullptr)
                                        {
                                            progressGuard->setValue(std::min(progressValue, 1000));
                                        }
                                    },
                                    Qt::QueuedConnection);
                            }
                        }

                        if (!result.cancelled && result.errorText.isEmpty())
                        {
                            result.sha256Text = QString::fromLatin1(sha256.result().toHex());
                        }
                        file.close();
                    }

                    result.elapsedMs = static_cast<qint64>(
                        std::chrono::duration_cast<std::chrono::milliseconds>(
                            std::chrono::steady_clock::now() - beginTime).count());

                    FileDetailDialog* targetDialog = guardThis.data();
                    if (targetDialog == nullptr)
                    {
                        return;
                    }

                    QMetaObject::invokeMethod(
                        targetDialog,
                        [guardThis, editorGuard, progressGuard, startGuard, cancelGuard, result]()
                        {
                            if (guardThis == nullptr || editorGuard == nullptr ||
                                progressGuard == nullptr || startGuard == nullptr ||
                                cancelGuard == nullptr)
                            {
                                return;
                            }

                            startGuard->setEnabled(true);
                            cancelGuard->setEnabled(false);
                            cancelGuard->setText(QStringLiteral("取消"));
                            progressGuard->setValue(result.totalBytes > 0
                                ? static_cast<int>((result.readBytes * 1000LL) / result.totalBytes)
                                : 1000);

                            const double elapsedSeconds = std::max(0.001, static_cast<double>(result.elapsedMs) / 1000.0);
                            const double speedMiB = (static_cast<double>(result.readBytes) / (1024.0 * 1024.0)) / elapsedSeconds;

                            QString content;
                            content += QStringLiteral("算法: SHA256\n");
                            content += QStringLiteral("来源: 用户态流式读取(QCryptographicHash)\n");
                            content += QStringLiteral("文件: %1\n").arg(QDir::toNativeSeparators(guardThis->m_filePath));
                            content += QStringLiteral("总大小: %1 字节\n").arg(result.totalBytes);
                            content += QStringLiteral("已读取: %1 字节\n").arg(result.readBytes);
                            content += QStringLiteral("耗时: %1 ms\n").arg(result.elapsedMs);
                            content += QStringLiteral("速度: %1 MiB/s\n").arg(QString::number(speedMiB, 'f', 2));
                            content += QStringLiteral("是否取消: %1\n").arg(result.cancelled ? QStringLiteral("是") : QStringLiteral("否"));
                            if (!result.errorText.isEmpty())
                            {
                                content += QStringLiteral("错误: %1\n").arg(result.errorText);
                            }
                            if (!result.sha256Text.isEmpty())
                            {
                                content += QStringLiteral("SHA256: %1\n").arg(result.sha256Text);
                            }
                            editorGuard->setText(content);
                        },
                        Qt::QueuedConnection);
                });
            task->setAutoDelete(true);
            QThreadPool::globalInstance()->start(task);
        }

        void requestHashCancel(QPushButton* cancelButton)
        {
            // 用途：设置哈希取消标记。
            // 返回：无；后台线程在下一次块边界观察该标记。
            if (m_hashCancelRequested != nullptr)
            {
                m_hashCancelRequested->store(true);
            }
            if (cancelButton != nullptr)
            {
                cancelButton->setEnabled(false);
                cancelButton->setText(QStringLiteral("正在取消..."));
            }
        }

        void refreshUsageTable(QTreeWidget* table, QLabel* statusLabel, QPushButton* refreshButton)
        {
            // 用途：异步刷新属性页内的文件占用列表。
            // 处理：调用 FileHandleUsageScanner，结果显示 PID/Handle/GrantedAccess/来源。
            // 返回：无。
            if (table == nullptr || statusLabel == nullptr || refreshButton == nullptr)
            {
                return;
            }

            QFileInfo info(m_filePath);
            if (!info.exists())
            {
                statusLabel->setText(QStringLiteral("● 目标不存在，无法扫描占用。"));
                return;
            }

            refreshButton->setEnabled(false);
            table->clear();
            statusLabel->setText(QStringLiteral("● 正在扫描文件占用..."));

            const std::vector<QString> targetPaths{ info.absoluteFilePath() };
            QPointer<FileDetailDialog> guardThis(this);
            QPointer<QTreeWidget> tableGuard(table);
            QPointer<QLabel> statusGuard(statusLabel);
            QPointer<QPushButton> refreshGuard(refreshButton);

            auto* task = QRunnable::create([guardThis, tableGuard, statusGuard, refreshGuard, targetPaths]()
                {
                    const filedock::handleusage::HandleUsageScanResult scanResult =
                        filedock::handleusage::scanHandleUsageByPaths(targetPaths, 0);
                    FileDetailDialog* targetDialog = guardThis.data();
                    if (targetDialog == nullptr)
                    {
                        return;
                    }

                    QMetaObject::invokeMethod(
                        targetDialog,
                        [tableGuard, statusGuard, refreshGuard, scanResult]()
                        {
                            if (tableGuard == nullptr || statusGuard == nullptr || refreshGuard == nullptr)
                            {
                                return;
                            }

                            tableGuard->setSortingEnabled(false);
                            tableGuard->clear();
                            for (const filedock::handleusage::HandleUsageEntry& entry : scanResult.entries)
                            {
                                auto* item = new QTreeWidgetItem();
                                item->setText(0, QString::number(entry.processId));
                                item->setText(1, entry.processName);
                                item->setText(2, entry.handleValue == 0
                                    ? QStringLiteral("-")
                                    : formatHex64(entry.handleValue));
                                item->setText(3, entry.grantedAccess == 0
                                    ? QStringLiteral("-")
                                    : QStringLiteral("0x%1").arg(entry.grantedAccess, 8, 16, QChar('0')).toUpper());
                                item->setText(4, entry.objectName);
                                item->setText(5, entry.matchedTargetPath);
                                const QString sourceText = entry.enumerationSource.trimmed().isEmpty()
                                    ? QStringLiteral("R3 DuplicateHandle")
                                    : entry.enumerationSource;
                                const QString ruleText = entry.matchRuleText.trimmed().isEmpty()
                                    ? (entry.matchedByDirectoryRule ? QStringLiteral("目录前缀") : QStringLiteral("精确"))
                                    : entry.matchRuleText;
                                item->setText(6, QStringLiteral("%1 | %2").arg(sourceText, ruleText));
                                tableGuard->addTopLevelItem(item);
                            }
                            tableGuard->setSortingEnabled(true);
                            if (tableGuard->header() != nullptr)
                            {
                                tableGuard->resizeColumnToContents(0);
                                tableGuard->resizeColumnToContents(1);
                                tableGuard->resizeColumnToContents(2);
                                tableGuard->resizeColumnToContents(3);
                            }

                            QString statusText = QStringLiteral("● 扫描完成 %1 ms | 总句柄:%2 | 文件句柄:%3 | 命中:%4")
                                .arg(scanResult.elapsedMs)
                                .arg(scanResult.totalHandleCount)
                                .arg(scanResult.fileLikeHandleCount)
                                .arg(scanResult.matchedHandleCount);
                            if (!scanResult.diagnosticText.trimmed().isEmpty())
                            {
                                statusText += QStringLiteral(" | %1").arg(scanResult.diagnosticText);
                            }
                            statusGuard->setText(statusText);
                            refreshGuard->setEnabled(true);
                        },
                        Qt::QueuedConnection);
                });
            task->setAutoDelete(true);
            QThreadPool::globalInstance()->start(task);
        }

        void startR0FileInfoLoad(CodeEditorWidget* textEditorWidget, const QFileInfo& info, const QString& ntPathText, const QString& baseContent)
        {
            // 用途：后台读取 R0 文件基础信息，避免常规页构建时阻塞属性窗口打开。
            // 输入：textEditorWidget 为 UI 回填目标，info/ntPathText/baseContent 为首屏已生成的 R3 信息。
            // 处理：工作线程调用 ArkDriverClient；UI 线程只追加最终文本。
            // 返回：无；对话框关闭或控件释放后自动丢弃结果。
            if (textEditorWidget == nullptr)
            {
                return;
            }

            QPointer<FileDetailDialog> guardThis(this);
            QPointer<CodeEditorWidget> editorGuard(textEditorWidget);
            auto* task = QRunnable::create([guardThis, editorGuard, info, ntPathText, baseContent]()
                {
                    FileDetailDialog* targetDialog = guardThis.data();
                    if (targetDialog == nullptr)
                    {
                        return;
                    }

                    const ksword::ark::FileInfoQueryResult r0Info =
                        FileDetailDialog::queryR0FileInfo(info, ntPathText);
                    targetDialog = guardThis.data();
                    if (targetDialog == nullptr)
                    {
                        return;
                    }

                    QMetaObject::invokeMethod(
                        targetDialog,
                        [guardThis, editorGuard, baseContent, r0Info]()
                        {
                            if (guardThis == nullptr || editorGuard == nullptr)
                            {
                                return;
                            }

                            QString content = baseContent;
                            content.replace(
                                QStringLiteral("查询来源: R3 QFileInfo（R0 信息后台加载）"),
                                QStringLiteral("查询来源: %1").arg(r0Info.io.ok
                                    ? QStringLiteral("R0 KswordARK + R3 QFileInfo")
                                    : QStringLiteral("R3 QFileInfo (R0不可用)")));
                            content += QStringLiteral("\n[R0 文件基础信息]\n");
                            if (!r0Info.objectName.empty())
                            {
                                content += QStringLiteral("R0对象名: %1\n").arg(QString::fromStdWString(r0Info.objectName));
                            }
                            content += guardThis->formatR0FileInfoText(r0Info);
                            editorGuard->setText(content);
                        },
                        Qt::QueuedConnection);
                });
            task->setAutoDelete(true);
            QThreadPool::globalInstance()->start(task);
        }

        static FileSecuritySnapshot loadFileSecuritySnapshot(const QString& nativePath)
        {
            // 用途：读取 Owner/Group/DACL/SACL 并生成权限页快照。
            // 输入：nativePath 为 Windows 本机路径；调用者确保在后台线程执行。
            // 处理：同步调用 Windows 安全 API，同时保留旧文本明细和新表格行。
            // 返回：FileSecuritySnapshot；读取失败时 detailText 包含错误码，aceRows 可为空。
            FileSecuritySnapshot snapshot;
            QString content;
            std::wstring nativePathBuffer = nativePath.toStdWString();

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
            snapshot.descriptorError = queryResult;
            if (queryResult != ERROR_SUCCESS)
            {
                content += QStringLiteral("\n深层安全描述符读取失败, code=%1\n").arg(queryResult);
            }
            else
            {
                snapshot.descriptorOk = true;
                snapshot.ownerSidText = sidToStringText(ownerSid);
                snapshot.ownerAccountText = sidToAccountText(ownerSid);
                snapshot.groupSidText = sidToStringText(groupSid);
                snapshot.groupAccountText = sidToAccountText(groupSid);

                content += QStringLiteral("\n[Owner]\n");
                content += QStringLiteral("SID: %1\n").arg(snapshot.ownerSidText);
                content += QStringLiteral("账户: %1\n").arg(snapshot.ownerAccountText);

                content += QStringLiteral("\n[Primary Group]\n");
                content += QStringLiteral("SID: %1\n").arg(snapshot.groupSidText);
                content += QStringLiteral("账户: %1\n").arg(snapshot.groupAccountText);

                appendAclText(QStringLiteral("DACL"), dacl, content);
                appendAclRows(QStringLiteral("DACL"), dacl, snapshot.aceRows);
                ::LocalFree(securityDescriptor);
            }

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
            snapshot.saclError = saclResult;
            if (saclResult == ERROR_SUCCESS)
            {
                snapshot.saclOk = true;
                appendAclText(QStringLiteral("SACL"), sacl, content);
                appendAclRows(QStringLiteral("SACL"), sacl, snapshot.aceRows);
                ::LocalFree(saclDescriptor);
            }
            else
            {
                content += QStringLiteral("\n[SACL]\n");
                content += QStringLiteral("读取失败（通常需要 SeSecurityPrivilege）, code=%1\n").arg(saclResult);
            }

            content += QStringLiteral("\n说明：Mask 显示为十六进制，权限列为常见位标志拆解。");
            snapshot.detailText = content;
            return snapshot;
        }

        static QString buildSecurityDeepText(const QString& nativePath)
        {
            // 用途：兼容旧调用点，生成纯文本安全描述符明细。
            // 输入：nativePath 为 Windows 本机路径。
            // 处理：委托 loadFileSecuritySnapshot，避免维护两套 ACL 解析逻辑。
            // 返回：可展示文本；失败时包含错误码。
            return loadFileSecuritySnapshot(nativePath).detailText;
        }

        void populateSecurityWidgets(
            QTableWidget* aceTable,
            CodeEditorWidget* detailEditor,
            QLabel* statusLabel,
            const QString& baseContent,
            const FileSecuritySnapshot& snapshot)
        {
            // 用途：把后台读取的权限快照回填到权限页 UI。
            // 输入：aceTable/detailEditor/statusLabel 为目标控件，snapshot 为读取结果。
            // 处理：表格展示可编辑 DACL ACE，详情框保留完整文本和错误码。
            // 返回：无；控件为空时跳过对应更新。
            if (aceTable != nullptr)
            {
                aceTable->setSortingEnabled(false);
                aceTable->setRowCount(static_cast<int>(snapshot.aceRows.size()));
                for (int rowIndex = 0; rowIndex < static_cast<int>(snapshot.aceRows.size()); ++rowIndex)
                {
                    const FileSecurityAceRow& row = snapshot.aceRows[static_cast<std::size_t>(rowIndex)];
                    aceTable->setItem(rowIndex, 0, createReadonlyTableItem(row.scopeText));
                    aceTable->setItem(rowIndex, 1, createReadonlyTableItem(QString::number(row.aceIndex)));
                    aceTable->setItem(rowIndex, 2, createReadonlyTableItem(row.typeText));
                    aceTable->setItem(rowIndex, 3, createReadonlyTableItem(row.accountText));
                    aceTable->setItem(rowIndex, 4, createReadonlyTableItem(row.sidText));
                    aceTable->setItem(rowIndex, 5, createReadonlyTableItem(formatAccessMaskHex(row.mask)));
                    aceTable->setItem(rowIndex, 6, createReadonlyTableItem(row.rightsText));
                    aceTable->setItem(rowIndex, 7, createReadonlyTableItem(row.flagsText));
                    aceTable->setItem(rowIndex, 8, createReadonlyTableItem(row.canEdit ? QStringLiteral("可编辑") : QStringLiteral("只读展示")));
                    for (int columnIndex = 0; columnIndex < aceTable->columnCount(); ++columnIndex)
                    {
                        QTableWidgetItem* item = aceTable->item(rowIndex, columnIndex);
                        if (item == nullptr)
                        {
                            continue;
                        }
                        item->setData(Qt::UserRole + 1, row.scopeText);
                        item->setData(Qt::UserRole + 2, static_cast<qulonglong>(row.aceIndex));
                        item->setData(Qt::UserRole + 3, row.sidText);
                        item->setData(Qt::UserRole + 4, row.typeText);
                        item->setData(Qt::UserRole + 5, static_cast<qulonglong>(row.mask));
                        item->setData(Qt::UserRole + 6, row.canEdit);
                    }
                }
                aceTable->setSortingEnabled(true);
                aceTable->resizeColumnsToContents();
            }

            if (detailEditor != nullptr)
            {
                QString detailText = baseContent;
                if (snapshot.descriptorOk)
                {
                    detailText += QStringLiteral("\n[摘要]\nOwner: %1 | %2\nPrimary Group: %3 | %4\n")
                        .arg(snapshot.ownerAccountText)
                        .arg(snapshot.ownerSidText)
                        .arg(snapshot.groupAccountText)
                        .arg(snapshot.groupSidText);
                }
                detailText += snapshot.detailText;
                detailEditor->setText(detailText);
            }

            if (statusLabel != nullptr)
            {
                const QString daclState = snapshot.descriptorOk
                    ? QStringLiteral("DACL 已读取")
                    : QStringLiteral("DACL 读取失败:%1").arg(snapshot.descriptorError);
                const QString saclState = snapshot.saclOk
                    ? QStringLiteral("SACL 已读取")
                    : QStringLiteral("SACL 只读失败:%1").arg(snapshot.saclError);
                statusLabel->setText(QStringLiteral("● %1；%2；ACE=%3")
                    .arg(daclState)
                    .arg(saclState)
                    .arg(snapshot.aceRows.size()));
            }
        }

        void startSecurityDeepLoad(
            QTableWidget* aceTable,
            CodeEditorWidget* detailEditor,
            QLabel* statusLabel,
            const QString& baseContent,
            const QString& nativePath)
        {
            // 用途：后台执行深层 ACL/SACL 解析并刷新权限页 UI。
            // 输入：baseContent 为快速权限摘要，nativePath 为目标路径。
            // 处理：工作线程读取安全描述符，UI 线程更新表格、状态和详情文本。
            // 返回：无；控件失效时丢弃结果。
            if (aceTable == nullptr || detailEditor == nullptr || statusLabel == nullptr)
            {
                return;
            }

            QPointer<FileDetailDialog> guardThis(this);
            QPointer<QTableWidget> tableGuard(aceTable);
            QPointer<CodeEditorWidget> editorGuard(detailEditor);
            QPointer<QLabel> statusGuard(statusLabel);
            auto* task = QRunnable::create([guardThis, tableGuard, editorGuard, statusGuard, baseContent, nativePath]()
                {
                    FileDetailDialog* targetDialog = guardThis.data();
                    if (targetDialog == nullptr)
                    {
                        return;
                    }
                    const FileSecuritySnapshot snapshot = FileDetailDialog::loadFileSecuritySnapshot(nativePath);
                    targetDialog = guardThis.data();
                    if (targetDialog == nullptr)
                    {
                        return;
                    }

                    QMetaObject::invokeMethod(
                        targetDialog,
                        [guardThis, tableGuard, editorGuard, statusGuard, baseContent, snapshot]()
                        {
                            if (guardThis != nullptr)
                            {
                                guardThis->populateSecurityWidgets(
                                    tableGuard,
                                    editorGuard,
                                    statusGuard,
                                    baseContent,
                                    snapshot);
                            }
                        },
                        Qt::QueuedConnection);
                });
            task->setAutoDelete(true);
            QThreadPool::globalInstance()->start(task);
        }

        DWORD maskFromSecurityPreset(const int presetIndex)
        {
            // 用途：把权限预设下拉框映射为 Windows 文件访问掩码。
            // 输入：presetIndex 为 QComboBox 当前索引。
            // 处理：优先覆盖常见文件安全页语义，减少用户手工拼位需求。
            // 返回：FILE_* / 标准权限组合掩码。
            switch (presetIndex)
            {
            case 0:
                return FILE_GENERIC_READ;
            case 1:
                return FILE_GENERIC_WRITE;
            case 2:
                return FILE_GENERIC_READ | FILE_GENERIC_WRITE;
            case 3:
                return FILE_GENERIC_READ | FILE_GENERIC_EXECUTE;
            case 4:
                return FILE_GENERIC_READ | FILE_GENERIC_WRITE | FILE_GENERIC_EXECUTE | DELETE;
            case 5:
                return FILE_ALL_ACCESS;
            default:
                return FILE_GENERIC_READ;
            }
        }

        DWORD maskFromSecurityChecks(
            QCheckBox* readCheck,
            QCheckBox* writeCheck,
            QCheckBox* executeCheck,
            QCheckBox* deleteCheck,
            QCheckBox* writeDacCheck,
            QCheckBox* writeOwnerCheck)
        {
            // 用途：把高级复选框合成为访问掩码。
            // 输入：各权限复选框控件，可为空。
            // 处理：只采纳勾选的权限位；未勾选时返回 0。
            // 返回：访问掩码。
            DWORD mask = 0;
            if (readCheck != nullptr && readCheck->isChecked()) mask |= FILE_GENERIC_READ;
            if (writeCheck != nullptr && writeCheck->isChecked()) mask |= FILE_GENERIC_WRITE;
            if (executeCheck != nullptr && executeCheck->isChecked()) mask |= FILE_GENERIC_EXECUTE;
            if (deleteCheck != nullptr && deleteCheck->isChecked()) mask |= DELETE;
            if (writeDacCheck != nullptr && writeDacCheck->isChecked()) mask |= WRITE_DAC;
            if (writeOwnerCheck != nullptr && writeOwnerCheck->isChecked()) mask |= WRITE_OWNER;
            return mask;
        }

        DWORD inheritanceFlagsFromCombo(const int inheritIndex)
        {
            // 用途：把继承范围下拉框映射为 EXPLICIT_ACCESS 继承标志。
            // 输入：inheritIndex 为 QComboBox 当前索引。
            // 处理：文件默认不继承，目录可选择子对象继承。
            // 返回：NO_INHERITANCE / SUB_CONTAINERS_AND_OBJECTS_INHERIT 等标志。
            switch (inheritIndex)
            {
            case 1:
                return SUB_CONTAINERS_AND_OBJECTS_INHERIT;
            case 2:
                return SUB_OBJECTS_ONLY_INHERIT;
            case 3:
                return SUB_CONTAINERS_ONLY_INHERIT;
            default:
                return NO_INHERITANCE;
            }
        }

        bool extractAceSidAndType(LPVOID acePointer, BYTE* aceTypeOut, PSID* sidOut)
        {
            // 用途：从常见 ACE 结构取出 AceType 与 SID 指针。
            // 输入：acePointer 来自 GetAce。
            // 处理：只解析当前 UI 支持删除的 DACL ACE 类型。
            // 返回：成功解析返回 true；未知类型返回 false。
            if (acePointer == nullptr || aceTypeOut == nullptr || sidOut == nullptr)
            {
                return false;
            }

            ACE_HEADER* aceHeader = reinterpret_cast<ACE_HEADER*>(acePointer);
            *aceTypeOut = aceHeader->AceType;
            *sidOut = nullptr;
            switch (aceHeader->AceType)
            {
            case ACCESS_ALLOWED_ACE_TYPE:
                *sidOut = reinterpret_cast<PSID>(&reinterpret_cast<ACCESS_ALLOWED_ACE*>(acePointer)->SidStart);
                return true;
            case ACCESS_DENIED_ACE_TYPE:
                *sidOut = reinterpret_cast<PSID>(&reinterpret_cast<ACCESS_DENIED_ACE*>(acePointer)->SidStart);
                return true;
            default:
                return false;
            }
        }

        DWORD applySecurityAceChange(
            const QString& accountText,
            const DWORD accessMask,
            const ACCESS_MODE accessMode,
            const DWORD inheritanceFlags,
            QString& detailTextOut)
        {
            // 用途：添加或设置一个 DACL ACE。
            // 输入：accountText 为账户名或 SID 字符串，accessMask 为权限掩码，accessMode 为允许/拒绝模式。
            // 处理：读取现有 DACL，调用 SetEntriesInAclW 合成新 DACL，再写回文件对象。
            // 返回：Win32 错误码；ERROR_SUCCESS 表示写入成功。
            const QString normalizedAccount = accountText.trimmed();
            if (normalizedAccount.isEmpty())
            {
                detailTextOut = QStringLiteral("账户不能为空。可填写 DOMAIN\\User、BUILTIN\\Administrators 或 S-1-...。");
                return ERROR_INVALID_PARAMETER;
            }
            if (accessMask == 0)
            {
                detailTextOut = QStringLiteral("权限掩码为 0，未执行写入。");
                return ERROR_INVALID_PARAMETER;
            }

            std::wstring pathBuffer = QDir::toNativeSeparators(m_filePath).toStdWString();
            PACL oldDacl = nullptr;
            PSECURITY_DESCRIPTOR securityDescriptor = nullptr;
            DWORD result = ::GetNamedSecurityInfoW(
                pathBuffer.data(),
                SE_FILE_OBJECT,
                DACL_SECURITY_INFORMATION,
                nullptr,
                nullptr,
                &oldDacl,
                nullptr,
                &securityDescriptor);
            if (result != ERROR_SUCCESS)
            {
                detailTextOut = QStringLiteral("读取现有 DACL 失败，code=%1。").arg(result);
                return result;
            }

            std::wstring accountBuffer = normalizedAccount.toStdWString();
            PSID trusteeSid = nullptr;
            const bool accountLooksLikeSid = normalizedAccount.startsWith(QStringLiteral("S-"), Qt::CaseInsensitive);
            if (accountLooksLikeSid)
            {
                if (::ConvertStringSidToSidW(accountBuffer.c_str(), &trusteeSid) == FALSE || trusteeSid == nullptr)
                {
                    result = ::GetLastError();
                    if (securityDescriptor != nullptr)
                    {
                        ::LocalFree(securityDescriptor);
                    }
                    detailTextOut = QStringLiteral("SID 字符串解析失败，code=%1，SID=%2。")
                        .arg(result)
                        .arg(normalizedAccount);
                    return result;
                }
            }

            EXPLICIT_ACCESS_W explicitAccess{};
            explicitAccess.grfAccessPermissions = accessMask;
            explicitAccess.grfAccessMode = accessMode;
            explicitAccess.grfInheritance = inheritanceFlags;
            explicitAccess.Trustee.TrusteeForm = accountLooksLikeSid ? TRUSTEE_IS_SID : TRUSTEE_IS_NAME;
            explicitAccess.Trustee.TrusteeType = TRUSTEE_IS_UNKNOWN;
            explicitAccess.Trustee.ptstrName = accountLooksLikeSid
                ? reinterpret_cast<LPWSTR>(trusteeSid)
                : accountBuffer.data();

            PACL newDacl = nullptr;
            result = ::SetEntriesInAclW(1, &explicitAccess, oldDacl, &newDacl);
            if (result == ERROR_SUCCESS)
            {
                result = ::SetNamedSecurityInfoW(
                    pathBuffer.data(),
                    SE_FILE_OBJECT,
                    DACL_SECURITY_INFORMATION,
                    nullptr,
                    nullptr,
                    newDacl,
                    nullptr);
            }

            if (newDacl != nullptr)
            {
                ::LocalFree(newDacl);
            }
            if (trusteeSid != nullptr)
            {
                ::LocalFree(trusteeSid);
            }
            if (securityDescriptor != nullptr)
            {
                ::LocalFree(securityDescriptor);
            }

            detailTextOut = result == ERROR_SUCCESS
                ? QStringLiteral("已写入 DACL：账户=%1，模式=%2，Mask=%3，继承标志=0x%4。")
                    .arg(normalizedAccount)
                    .arg(accessMode == DENY_ACCESS ? QStringLiteral("拒绝") : QStringLiteral("允许/设置"))
                    .arg(formatAccessMaskHex(accessMask))
                    .arg(inheritanceFlags, 0, 16)
                : QStringLiteral("写入 DACL 失败，code=%1。账户=%2，Mask=%3。")
                    .arg(result)
                    .arg(normalizedAccount)
                    .arg(formatAccessMaskHex(accessMask));
            return result;
        }

        DWORD deleteSelectedDaclAce(QTableWidget* aceTable, QString& detailTextOut)
        {
            // 用途：删除权限表中当前选中的非继承 DACL ACE。
            // 输入：aceTable 为权限页 ACE 表格，当前行保存 SID/类型/序号元数据。
            // 处理：读取现有 DACL，复制除目标 ACE 外的原始 ACE 字节，最后 SetNamedSecurityInfoW 写回。
            // 返回：Win32 错误码；ERROR_SUCCESS 表示删除成功。
            if (aceTable == nullptr || aceTable->currentRow() < 0)
            {
                detailTextOut = QStringLiteral("请先在 DACL 表格中选择一条可编辑 ACE。");
                return ERROR_INVALID_PARAMETER;
            }

            const int rowIndex = aceTable->currentRow();
            QTableWidgetItem* firstItem = aceTable->item(rowIndex, 0);
            if (firstItem == nullptr)
            {
                detailTextOut = QStringLiteral("选中行无元数据，无法删除。");
                return ERROR_INVALID_PARAMETER;
            }

            const QString scopeText = firstItem->data(Qt::UserRole + 1).toString();
            const DWORD selectedAceIndex = firstItem->data(Qt::UserRole + 2).toUInt();
            const QString selectedSidText = firstItem->data(Qt::UserRole + 3).toString();
            const QString selectedTypeText = firstItem->data(Qt::UserRole + 4).toString();
            const bool canEdit = firstItem->data(Qt::UserRole + 6).toBool();
            if (scopeText != QStringLiteral("DACL") || !canEdit)
            {
                detailTextOut = QStringLiteral("当前 ACE 只能展示，不能由此按钮修改。继承 ACE、SACL 和对象 ACE 需要在来源对象或审计页处理。");
                return ERROR_ACCESS_DENIED;
            }

            std::wstring pathBuffer = QDir::toNativeSeparators(m_filePath).toStdWString();
            PACL oldDacl = nullptr;
            PSECURITY_DESCRIPTOR securityDescriptor = nullptr;
            DWORD result = ::GetNamedSecurityInfoW(
                pathBuffer.data(),
                SE_FILE_OBJECT,
                DACL_SECURITY_INFORMATION,
                nullptr,
                nullptr,
                &oldDacl,
                nullptr,
                &securityDescriptor);
            if (result != ERROR_SUCCESS)
            {
                detailTextOut = QStringLiteral("读取现有 DACL 失败，code=%1。").arg(result);
                return result;
            }
            if (oldDacl == nullptr)
            {
                if (securityDescriptor != nullptr)
                {
                    ::LocalFree(securityDescriptor);
                }
                detailTextOut = QStringLiteral("当前 DACL 为空，无法删除 ACE。");
                return ERROR_NOT_FOUND;
            }

            ACL_SIZE_INFORMATION aclSizeInfo{};
            if (::GetAclInformation(oldDacl, &aclSizeInfo, sizeof(aclSizeInfo), AclSizeInformation) == FALSE)
            {
                result = ::GetLastError();
                ::LocalFree(securityDescriptor);
                detailTextOut = QStringLiteral("读取 ACL 信息失败，code=%1。").arg(result);
                return result;
            }

            DWORD newAclBytes = sizeof(ACL);
            bool targetFound = false;
            for (DWORD aceIndex = 0; aceIndex < aclSizeInfo.AceCount; ++aceIndex)
            {
                LPVOID acePointer = nullptr;
                if (::GetAce(oldDacl, aceIndex, &acePointer) == FALSE || acePointer == nullptr)
                {
                    result = ::GetLastError();
                    ::LocalFree(securityDescriptor);
                    detailTextOut = QStringLiteral("读取 ACE[%1] 失败，code=%2。").arg(aceIndex).arg(result);
                    return result;
                }

                ACE_HEADER* aceHeader = reinterpret_cast<ACE_HEADER*>(acePointer);
                BYTE aceType = 0;
                PSID aceSid = nullptr;
                const bool sidOk = extractAceSidAndType(acePointer, &aceType, &aceSid);
                const bool isTarget = sidOk
                    && aceIndex == selectedAceIndex
                    && aceTypeToText(aceType) == selectedTypeText
                    && sidToStringText(aceSid) == selectedSidText
                    && (aceHeader->AceFlags & INHERITED_ACE) == 0;
                if (isTarget)
                {
                    targetFound = true;
                    continue;
                }
                newAclBytes += aceHeader->AceSize;
            }

            if (!targetFound)
            {
                ::LocalFree(securityDescriptor);
                detailTextOut = QStringLiteral("未在当前 DACL 中找到匹配 ACE，可能权限已被其它进程修改。请刷新后重试。");
                return ERROR_NOT_FOUND;
            }

            PACL newDacl = reinterpret_cast<PACL>(::LocalAlloc(LPTR, newAclBytes));
            if (newDacl == nullptr)
            {
                result = ::GetLastError();
                ::LocalFree(securityDescriptor);
                detailTextOut = QStringLiteral("分配新 DACL 失败，code=%1。").arg(result);
                return result;
            }

            const DWORD aclRevision = oldDacl->AclRevision;
            if (::InitializeAcl(newDacl, newAclBytes, aclRevision) == FALSE)
            {
                result = ::GetLastError();
                ::LocalFree(newDacl);
                ::LocalFree(securityDescriptor);
                detailTextOut = QStringLiteral("初始化新 DACL 失败，code=%1。").arg(result);
                return result;
            }

            for (DWORD aceIndex = 0; aceIndex < aclSizeInfo.AceCount; ++aceIndex)
            {
                LPVOID acePointer = nullptr;
                if (::GetAce(oldDacl, aceIndex, &acePointer) == FALSE || acePointer == nullptr)
                {
                    result = ::GetLastError();
                    ::LocalFree(newDacl);
                    ::LocalFree(securityDescriptor);
                    detailTextOut = QStringLiteral("复制 ACE[%1] 前读取失败，code=%2。").arg(aceIndex).arg(result);
                    return result;
                }

                ACE_HEADER* aceHeader = reinterpret_cast<ACE_HEADER*>(acePointer);
                BYTE aceType = 0;
                PSID aceSid = nullptr;
                const bool sidOk = extractAceSidAndType(acePointer, &aceType, &aceSid);
                const bool isTarget = sidOk
                    && aceIndex == selectedAceIndex
                    && aceTypeToText(aceType) == selectedTypeText
                    && sidToStringText(aceSid) == selectedSidText
                    && (aceHeader->AceFlags & INHERITED_ACE) == 0;
                if (isTarget)
                {
                    continue;
                }
                if (::AddAce(newDacl, aclRevision, MAXDWORD, acePointer, aceHeader->AceSize) == FALSE)
                {
                    result = ::GetLastError();
                    ::LocalFree(newDacl);
                    ::LocalFree(securityDescriptor);
                    detailTextOut = QStringLiteral("复制 ACE[%1] 到新 DACL 失败，code=%2。").arg(aceIndex).arg(result);
                    return result;
                }
            }

            result = ::SetNamedSecurityInfoW(
                pathBuffer.data(),
                SE_FILE_OBJECT,
                DACL_SECURITY_INFORMATION,
                nullptr,
                nullptr,
                newDacl,
                nullptr);
            ::LocalFree(newDacl);
            ::LocalFree(securityDescriptor);

            detailTextOut = result == ERROR_SUCCESS
                ? QStringLiteral("已删除 ACE[%1]：%2 | %3。").arg(selectedAceIndex).arg(selectedTypeText, selectedSidText)
                : QStringLiteral("删除 ACE 写回失败，code=%1。").arg(result);
            return result;
        }

        void startSignatureLoad(CodeEditorWidget* textEditorWidget)
        {
            // 用途：后台调用 PowerShell Get-AuthenticodeSignature。
            // 输入：textEditorWidget 为签名页显示目标。
            // 处理：外部进程最多等待 15 秒，超时会终止并回填错误文本。
            // 返回：无。
            if (textEditorWidget == nullptr)
            {
                return;
            }

            textEditorWidget->setText(QStringLiteral("正在后台读取 Authenticode 签名信息...\n目标: %1")
                .arg(QDir::toNativeSeparators(m_filePath)));

            const QString filePathSnapshot = m_filePath;
            QPointer<FileDetailDialog> guardThis(this);
            QPointer<CodeEditorWidget> editorGuard(textEditorWidget);
            auto* task = QRunnable::create([guardThis, editorGuard, filePathSnapshot]()
                {
                    QProcess process;
                    process.setProgram(QStringLiteral("powershell.exe"));
                    QString escapedPath = filePathSnapshot;
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
                    const bool finished = process.waitForFinished(15000);
                    if (!finished)
                    {
                        process.kill();
                        process.waitForFinished(1000);
                    }

                    const QString stdOutText = QString::fromLocal8Bit(process.readAllStandardOutput()).trimmed();
                    const QString stdErrText = QString::fromLocal8Bit(process.readAllStandardError()).trimmed();
                    QString finalText;
                    if (!finished)
                    {
                        finalText = QStringLiteral("签名检查超时，已终止 PowerShell。");
                    }
                    else if (process.exitStatus() != QProcess::NormalExit || process.exitCode() != 0 || stdOutText.isEmpty())
                    {
                        finalText = QStringLiteral("签名检查失败。\n退出码: %1\n").arg(process.exitCode());
                        finalText += stdErrText.isEmpty()
                            ? QStringLiteral("错误输出为空，可能系统未启用 PowerShell 或文件不可访问。")
                            : QStringLiteral("错误输出:\n%1").arg(stdErrText);
                    }
                    else
                    {
                        finalText = stdOutText;
                    }

                    FileDetailDialog* targetDialog = guardThis.data();
                    if (targetDialog == nullptr)
                    {
                        return;
                    }
                    QMetaObject::invokeMethod(
                        targetDialog,
                        [editorGuard, finalText]()
                        {
                            if (editorGuard != nullptr)
                            {
                                editorGuard->setText(finalText);
                            }
                        },
                        Qt::QueuedConnection);
                });
            task->setAutoDelete(true);
            QThreadPool::globalInstance()->start(task);
        }

        void startPeAnalysisLoad(CodeEditorWidget* textEditorWidget)
        {
            // 用途：后台执行 PE 深度解析文本生成。
            // 输入：textEditorWidget 为 PE 信息页显示目标。
            // 处理：调用 FilePropertyPeAnalyzer，避免 Import/Export/Directory 解析卡住 UI。
            // 返回：无。
            if (textEditorWidget == nullptr)
            {
                return;
            }

            textEditorWidget->setText(QStringLiteral("PE 信息加载中...\n目标: %1")
                .arg(QDir::toNativeSeparators(m_filePath)));
            const QString filePathSnapshot = m_filePath;
            QPointer<FileDetailDialog> guardThis(this);
            QPointer<CodeEditorWidget> editorGuard(textEditorWidget);
            auto* task = QRunnable::create([guardThis, editorGuard, filePathSnapshot]()
                {
                    const QString peText = file_dock_detail::buildPeAnalysisText(filePathSnapshot);
                    FileDetailDialog* targetDialog = guardThis.data();
                    if (targetDialog == nullptr)
                    {
                        return;
                    }
                    QMetaObject::invokeMethod(
                        targetDialog,
                        [editorGuard, peText]()
                        {
                            if (editorGuard != nullptr)
                            {
                                editorGuard->setText(peText);
                            }
                        },
                        Qt::QueuedConnection);
                });
            task->setAutoDelete(true);
            QThreadPool::globalInstance()->start(task);
        }

        static QString extractPrintableStringsPreview(const QString& filePath)
        {
            // 用途：以分块方式提取可打印 ASCII 字符串，替代 readAll。
            // 输入：filePath 为目标文件路径。
            // 处理：最多输出 2000 条字符串，最多扫描 128MiB，避免超大文件长时间占用线程。
            // 返回：可直接显示的字符串列表或错误/截断说明。
            QFile file(filePath);
            if (!file.open(QIODevice::ReadOnly))
            {
                return QStringLiteral("无法读取文件，无法提取字符串。错误: %1").arg(file.errorString());
            }

            constexpr qint64 kChunkBytes = 1024 * 1024;
            constexpr qint64 kMaxScanBytes = 128LL * 1024LL * 1024LL;
            QString current;
            QStringList result;
            qint64 scannedBytes = 0;
            while (!file.atEnd() && result.size() < 2000 && scannedBytes < kMaxScanBytes)
            {
                const QByteArray bytes = file.read(std::min(kChunkBytes, kMaxScanBytes - scannedBytes));
                if (bytes.isEmpty())
                {
                    break;
                }
                scannedBytes += bytes.size();
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
                            if (result.size() >= 2000)
                            {
                                break;
                            }
                        }
                        current.clear();
                    }
                }
            }
            if (current.length() >= 4 && result.size() < 2000)
            {
                result.append(current);
            }

            QString rawStringText = result.join('\n');
            if (rawStringText.trimmed().isEmpty())
            {
                rawStringText = QStringLiteral("<未提取到可打印字符串，或文件内容全部为二进制不可见字符。>");
            }
            if (!file.atEnd())
            {
                rawStringText += QStringLiteral("\n\n[提示] 已达到扫描/显示上限：扫描 %1 字节，显示 %2 条。")
                    .arg(scannedBytes)
                    .arg(result.size());
            }
            return rawStringText;
        }

        void startStringsLoad(CodeEditorWidget* textEditorWidget)
        {
            // 用途：后台提取字符串页内容。
            // 输入：textEditorWidget 为字符串页显示目标。
            // 处理：分块扫描文件，结果完成后回填 UI。
            // 返回：无。
            if (textEditorWidget == nullptr)
            {
                return;
            }

            textEditorWidget->setText(QStringLiteral("字符串扫描中...\n目标: %1")
                .arg(QDir::toNativeSeparators(m_filePath)));
            const QString filePathSnapshot = m_filePath;
            QPointer<FileDetailDialog> guardThis(this);
            QPointer<CodeEditorWidget> editorGuard(textEditorWidget);
            auto* task = QRunnable::create([guardThis, editorGuard, filePathSnapshot]()
                {
                    const QString stringsText = FileDetailDialog::extractPrintableStringsPreview(filePathSnapshot);
                    FileDetailDialog* targetDialog = guardThis.data();
                    if (targetDialog == nullptr)
                    {
                        return;
                    }
                    QMetaObject::invokeMethod(
                        targetDialog,
                        [editorGuard, stringsText]()
                        {
                            if (editorGuard != nullptr)
                            {
                                editorGuard->setText(stringsText);
                            }
                        },
                        Qt::QueuedConnection);
                });
            task->setAutoDelete(true);
            QThreadPool::globalInstance()->start(task);
        }

        static QString dependencyRowsToClipboardText(QTableWidget* table, const bool dllOnly)
        {
            // 用途：把依赖 DLL 表格选中行转换为剪贴板文本。
            // 输入：table 为依赖页表格，dllOnly 控制只复制 DLL 名称还是整行。
            // 返回：以换行分隔的文本；没有选中行时返回空字符串。
            if (table == nullptr)
            {
                return QString();
            }

            std::set<int> selectedRows;
            if (table->selectionModel() != nullptr)
            {
                const QModelIndexList rowIndexes = table->selectionModel()->selectedRows();
                for (const QModelIndex& index : rowIndexes)
                {
                    selectedRows.insert(index.row());
                }
            }
            if (selectedRows.empty() && table->currentRow() >= 0)
            {
                selectedRows.insert(table->currentRow());
            }

            QStringList lines;
            QStringList seenDllNames;
            for (const int rowIndex : selectedRows)
            {
                if (rowIndex < 0 || rowIndex >= table->rowCount())
                {
                    continue;
                }
                if (dllOnly)
                {
                    const QTableWidgetItem* dllItem = table->item(rowIndex, 0);
                    const QString dllName = dllItem != nullptr ? dllItem->text().trimmed() : QString();
                    if (!dllName.isEmpty() && !seenDllNames.contains(dllName, Qt::CaseInsensitive))
                    {
                        seenDllNames.push_back(dllName);
                        lines.push_back(dllName);
                    }
                    continue;
                }

                QStringList columns;
                for (int columnIndex = 0; columnIndex < table->columnCount(); ++columnIndex)
                {
                    const QTableWidgetItem* item = table->item(rowIndex, columnIndex);
                    columns.push_back(item != nullptr ? item->text() : QString());
                }
                lines.push_back(columns.join('\t'));
            }
            return lines.join('\n');
        }

        void populateDependencyTable(QTableWidget* table, QLabel* statusLabel, const file_dock_detail::PeDependencyResult& result, const qint64 elapsedMs)
        {
            // 用途：把后台解析出的依赖 DLL 结果填入表格。
            // 输入：table/statusLabel 为 UI 控件，result 为结构化导入表，elapsedMs 为解析耗时。
            // 处理：批量禁用排序和刷新，减少大量导入项时的 UI 抖动。
            // 返回：无。
            if (table == nullptr || statusLabel == nullptr)
            {
                return;
            }

            table->setSortingEnabled(false);
            table->setUpdatesEnabled(false);
            table->clearContents();
            constexpr int kMaxDisplayedDependencyRows = 20000;
            const int totalRowCount = static_cast<int>(result.rows.size());
            const int displayedRowCount = std::min(totalRowCount, kMaxDisplayedDependencyRows);
            table->setRowCount(displayedRowCount);
            for (int rowIndex = 0; rowIndex < displayedRowCount; ++rowIndex)
            {
                const file_dock_detail::PeDependencyRow& row = result.rows[rowIndex];
                const QString functionText = row.importMode == QStringLiteral("Ordinal")
                    ? QStringLiteral("#%1").arg(row.ordinalText)
                    : (row.functionName.trimmed().isEmpty() ? QStringLiteral("-") : row.functionName);

                table->setItem(rowIndex, 0, new QTableWidgetItem(row.dllName));
                table->setItem(rowIndex, 1, new QTableWidgetItem(functionText));
                table->setItem(rowIndex, 2, new QTableWidgetItem(row.hintText));
                table->setItem(rowIndex, 3, new QTableWidgetItem(row.importMode));
                table->setItem(rowIndex, 4, new QTableWidgetItem(row.thunkRvaText));
                table->setItem(rowIndex, 5, new QTableWidgetItem(row.diagnosticText));
            }
            table->setUpdatesEnabled(true);
            table->setSortingEnabled(true);
            if (table->horizontalHeader() != nullptr)
            {
                table->resizeColumnToContents(0);
                table->resizeColumnToContents(1);
                table->resizeColumnToContents(2);
                table->resizeColumnToContents(3);
            }

            if (!result.success)
            {
                statusLabel->setText(result.isPe
                    ? QStringLiteral("● PE 解析失败，未能读取依赖 DLL。耗时 %1 ms").arg(elapsedMs)
                    : QStringLiteral("● 不适用：目标不是 PE 文件。耗时 %1 ms").arg(elapsedMs));
                return;
            }
            if (!result.errorText.trimmed().isEmpty() && result.rows.isEmpty())
            {
                statusLabel->setText(QStringLiteral("● %1 耗时 %2 ms").arg(result.errorText.trimmed()).arg(elapsedMs));
                return;
            }
            QString statusText = QStringLiteral("● 加载完成 %1 ms | DLL:%2 | 导入项:%3")
                .arg(elapsedMs)
                .arg(result.dllNames.size())
                .arg(result.rows.size());
            if (displayedRowCount < totalRowCount)
            {
                statusText += QStringLiteral(" | 表格仅显示前 %1 行").arg(displayedRowCount);
            }
            statusLabel->setText(statusText);
        }

        void startDependencyLoad(QTableWidget* table, QLabel* statusLabel, CodeEditorWidget* detailEditor)
        {
            // 用途：后台读取 EXE/DLL Import Directory 并展示依赖 DLL。
            // 输入：table/statusLabel/detailEditor 为依赖页 UI 控件。
            // 处理：工作线程解析 PE；UI 线程填表和显示错误详情，失败不阻塞不崩溃。
            // 返回：无。
            if (table == nullptr || statusLabel == nullptr || detailEditor == nullptr)
            {
                return;
            }

            statusLabel->setText(QStringLiteral("● 正在后台读取 Import Directory..."));
            detailEditor->setText(QStringLiteral("依赖 DLL 加载中...\n目标: %1")
                .arg(QDir::toNativeSeparators(m_filePath)));

            const QString filePathSnapshot = m_filePath;
            QPointer<FileDetailDialog> guardThis(this);
            QPointer<QTableWidget> tableGuard(table);
            QPointer<QLabel> statusGuard(statusLabel);
            QPointer<CodeEditorWidget> detailGuard(detailEditor);
            auto* task = QRunnable::create([guardThis, tableGuard, statusGuard, detailGuard, filePathSnapshot]()
                {
                    const auto beginTime = std::chrono::steady_clock::now();
                    const file_dock_detail::PeDependencyResult result =
                        file_dock_detail::analyzePeDependencies(filePathSnapshot);
                    const qint64 elapsedMs = static_cast<qint64>(
                        std::chrono::duration_cast<std::chrono::milliseconds>(
                            std::chrono::steady_clock::now() - beginTime).count());

                    FileDetailDialog* targetDialog = guardThis.data();
                    if (targetDialog == nullptr)
                    {
                        return;
                    }
                    QMetaObject::invokeMethod(
                        targetDialog,
                        [guardThis, tableGuard, statusGuard, detailGuard, result, elapsedMs]()
                        {
                            if (guardThis == nullptr || tableGuard == nullptr ||
                                statusGuard == nullptr || detailGuard == nullptr)
                            {
                                return;
                            }

                            guardThis->populateDependencyTable(tableGuard, statusGuard, result, elapsedMs);
                            QString detailText;
                            detailText += QStringLiteral("目标: %1\n").arg(QDir::toNativeSeparators(guardThis->m_filePath));
                            if (!result.success || !result.errorText.trimmed().isEmpty())
                            {
                                detailText += QStringLiteral("%1\n").arg(result.errorText.trimmed());
                            }
                            else
                            {
                                detailText += QStringLiteral("依赖 DLL 名称:\n");
                                for (const QString& dllName : result.dllNames)
                                {
                                    detailText += QStringLiteral("  - %1\n").arg(dllName);
                                }
                            }
                            detailGuard->setText(detailText);
                        },
                        Qt::QueuedConnection);
                });
            task->setAutoDelete(true);
            QThreadPool::globalInstance()->start(task);
        }

        QWidget* buildDeferredTab(const QString& lazyKey)
        {
            // 用途：创建文件属性窗口的轻量占位页。
            // 处理：只保存 lazyKey 和提示文本，不读取文件、不启动外部进程；
            // 返回：首次切换到该页时由 activateDeferredTab 替换为真实页面。
            QWidget* page = new QWidget(this);
            page->setProperty("ks_file_detail_lazy_key", lazyKey);
            QVBoxLayout* layout = new QVBoxLayout(page);
            QLabel* loadingLabel = new QLabel(page);
            loadingLabel->setWordWrap(true);
            loadingLabel->setText(QStringLiteral(
                "该页面将在首次打开时加载。\n"
                "这样文件属性窗口可以先快速弹出，PE/签名/字符串等重型分析不会阻塞首屏。"));
            layout->addWidget(loadingLabel, 0);
            layout->addStretch(1);
            return page;
        }

        void activateDeferredTab(QTabWidget* tabWidget, const int tabIndex)
        {
            // 用途：首次选中懒加载页时，用真实页面替换占位页。
            // 输入 tabWidget/tabIndex：当前属性窗口 Tab 容器和被激活的索引。
            // 处理：根据 lazyKey 创建对应页面；重型页面内部继续走后台线程。
            // 返回：无；已加载页面不会重复替换。
            if (tabWidget == nullptr || tabIndex < 0)
            {
                return;
            }

            QWidget* placeholderPage = tabWidget->widget(tabIndex);
            if (placeholderPage == nullptr)
            {
                return;
            }
            const QString lazyKey = placeholderPage
                ->property("ks_file_detail_lazy_key")
                .toString()
                .trimmed()
                .toLower();
            if (lazyKey.isEmpty())
            {
                return;
            }

            QWidget* realPage = nullptr;
            if (lazyKey == QStringLiteral("security"))
            {
                realPage = buildSecurityTab();
            }
            else if (lazyKey == QStringLiteral("reparse"))
            {
                realPage = buildReparseTab();
            }
            else if (lazyKey == QStringLiteral("usage"))
            {
                realPage = buildUsageTab();
            }
            else if (lazyKey == QStringLiteral("signature"))
            {
                realPage = buildSignatureTab();
            }
            else if (lazyKey == QStringLiteral("pe"))
            {
                realPage = buildPeTab();
            }
            else if (lazyKey == QStringLiteral("dependencies"))
            {
                realPage = buildDependencyTab();
            }
            else if (lazyKey == QStringLiteral("strings"))
            {
                realPage = buildStringsTab();
            }
            else if (lazyKey == QStringLiteral("hex"))
            {
                realPage = buildHexTab();
            }

            if (realPage == nullptr)
            {
                return;
            }

            const QString titleText = tabWidget->tabText(tabIndex);
            tabWidget->removeTab(tabIndex);
            tabWidget->insertTab(tabIndex, realPage, titleText);
            tabWidget->setCurrentIndex(tabIndex);
            placeholderPage->deleteLater();
        }

        QWidget* buildGeneralTab()
        {
            QWidget* page = new QWidget(this);
            QVBoxLayout* layout = new QVBoxLayout(page);
            CodeEditorWidget* textEditorWidget = new CodeEditorWidget(page);
            textEditorWidget->setReadOnly(true);

            const QFileInfo info(m_filePath);
            const QString ntPathText = buildDriverNtPath(info.absoluteFilePath());
            QString content;
            content += QStringLiteral("[路径]\n");
            content += QStringLiteral("Win32路径: %1\n").arg(QDir::toNativeSeparators(info.absoluteFilePath()));
            content += QStringLiteral("NT路径: %1\n").arg(ntPathText.isEmpty() ? QStringLiteral("<转换失败>") : ntPathText);
            content += QStringLiteral("查询来源: R3 QFileInfo（R0 信息后台加载）\n\n");

            content += QStringLiteral("[R3 QFileInfo]\n");
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
            if (isPathReparsePoint(info.absoluteFilePath()))
            {
                content += QStringLiteral("重解析点: 是（首屏仅做属性位判断，不同步追踪链接目标）\n");
            }
            else
            {
                content += QStringLiteral("重解析点: 否\n");
            }

            const QString baseContent = content;
            content += QStringLiteral("\n[R0 文件基础信息]\n");
            content += QStringLiteral("正在后台加载，属性窗口首屏不会等待驱动查询完成...\n");
            textEditorWidget->setText(content);

            layout->addWidget(textEditorWidget, 1);
            startR0FileInfoLoad(textEditorWidget, info, ntPathText, baseContent);
            return page;
        }

        QWidget* buildReparseTab()
        {
            QWidget* page = new QWidget(this);
            QVBoxLayout* layout = new QVBoxLayout(page);
            CodeEditorWidget* textEditorWidget = new CodeEditorWidget(page);
            textEditorWidget->setReadOnly(true);

            QString content;
            if (!isPathReparsePoint(m_filePath))
            {
                content += QStringLiteral("目标路径: %1\n").arg(QDir::toNativeSeparators(m_filePath));
                content += QStringLiteral("状态: 当前目标不是 FILE_ATTRIBUTE_REPARSE_POINT。\n");
            }
            else
            {
                content += formatReparsePointText(m_filePath);
            }

            textEditorWidget->setText(content);
            layout->addWidget(textEditorWidget, 1);
            return page;
        }

        QWidget* buildSecurityTab()
        {
            QWidget* page = new QWidget(this);
            QVBoxLayout* layout = new QVBoxLayout(page);

            const QString nativePath = QDir::toNativeSeparators(m_filePath);
            QString baseContent;
            baseContent += QStringLiteral("目标路径: %1\n").arg(nativePath);

            // 先给出 Qt 维度的快速权限摘要，便于与 ACL 细节对照。
            QFileInfo info(m_filePath);
            baseContent += QStringLiteral("快速权限摘要:\n");
            baseContent += QStringLiteral("Read: %1\n").arg(info.isReadable() ? QStringLiteral("允许") : QStringLiteral("拒绝"));
            baseContent += QStringLiteral("Write: %1\n").arg(info.isWritable() ? QStringLiteral("允许") : QStringLiteral("拒绝"));
            baseContent += QStringLiteral("Execute: %1\n").arg(info.isExecutable() ? QStringLiteral("允许") : QStringLiteral("拒绝"));

            QGroupBox* operationGroup = new QGroupBox(QStringLiteral("权限编辑"), page);
            QGridLayout* operationLayout = new QGridLayout(operationGroup);

            QLineEdit* accountEdit = new QLineEdit(operationGroup);
            accountEdit->setPlaceholderText(QStringLiteral("账户或 SID，例如 BUILTIN\\Administrators / Everyone / S-1-5-32-544"));
            accountEdit->setStyleSheet(buildBlueInputStyle());

            QComboBox* accessModeCombo = new QComboBox(operationGroup);
            accessModeCombo->setStyleSheet(buildBlueInputStyle());
            accessModeCombo->addItem(QStringLiteral("允许：添加/合并"), static_cast<int>(GRANT_ACCESS));
            accessModeCombo->addItem(QStringLiteral("允许：替换该主体权限"), static_cast<int>(SET_ACCESS));
            accessModeCombo->addItem(QStringLiteral("拒绝：添加/合并"), static_cast<int>(DENY_ACCESS));

            QComboBox* presetCombo = new QComboBox(operationGroup);
            presetCombo->setStyleSheet(buildBlueInputStyle());
            presetCombo->addItems(QStringList{
                QStringLiteral("读取"),
                QStringLiteral("写入"),
                QStringLiteral("读取 + 写入"),
                QStringLiteral("读取 + 执行"),
                QStringLiteral("修改"),
                QStringLiteral("完全控制") });
            presetCombo->setCurrentIndex(4);

            QLineEdit* customMaskEdit = new QLineEdit(operationGroup);
            customMaskEdit->setPlaceholderText(QStringLiteral("可选自定义 Mask，如 0x001F01FF；留空使用预设/复选框"));
            customMaskEdit->setStyleSheet(buildBlueInputStyle());

            QComboBox* inheritanceCombo = new QComboBox(operationGroup);
            inheritanceCombo->setStyleSheet(buildBlueInputStyle());
            inheritanceCombo->addItems(QStringList{
                QStringLiteral("仅当前对象"),
                QStringLiteral("目录和文件继承"),
                QStringLiteral("仅文件继承"),
                QStringLiteral("仅目录继承") });

            QCheckBox* readCheck = new QCheckBox(QStringLiteral("读"), operationGroup);
            QCheckBox* writeCheck = new QCheckBox(QStringLiteral("写"), operationGroup);
            QCheckBox* executeCheck = new QCheckBox(QStringLiteral("执行"), operationGroup);
            QCheckBox* deleteCheck = new QCheckBox(QStringLiteral("删除"), operationGroup);
            QCheckBox* writeDacCheck = new QCheckBox(QStringLiteral("改 DACL"), operationGroup);
            QCheckBox* writeOwnerCheck = new QCheckBox(QStringLiteral("改所有者"), operationGroup);

            QPushButton* applyAceButton = new QPushButton(QStringLiteral("应用 ACE"), operationGroup);
            QPushButton* deleteAceButton = new QPushButton(QStringLiteral("删除选中 ACE"), operationGroup);
            QPushButton* refreshButton = new QPushButton(QStringLiteral("刷新权限"), operationGroup);
            applyAceButton->setStyleSheet(buildBlueButtonStyle());
            deleteAceButton->setStyleSheet(buildBlueButtonStyle());
            refreshButton->setStyleSheet(buildBlueButtonStyle());

            operationLayout->addWidget(new QLabel(QStringLiteral("主体"), operationGroup), 0, 0);
            operationLayout->addWidget(accountEdit, 0, 1, 1, 5);
            operationLayout->addWidget(new QLabel(QStringLiteral("动作"), operationGroup), 1, 0);
            operationLayout->addWidget(accessModeCombo, 1, 1);
            operationLayout->addWidget(new QLabel(QStringLiteral("预设"), operationGroup), 1, 2);
            operationLayout->addWidget(presetCombo, 1, 3);
            operationLayout->addWidget(new QLabel(QStringLiteral("继承"), operationGroup), 1, 4);
            operationLayout->addWidget(inheritanceCombo, 1, 5);
            operationLayout->addWidget(new QLabel(QStringLiteral("权限位"), operationGroup), 2, 0);
            operationLayout->addWidget(readCheck, 2, 1);
            operationLayout->addWidget(writeCheck, 2, 2);
            operationLayout->addWidget(executeCheck, 2, 3);
            operationLayout->addWidget(deleteCheck, 2, 4);
            operationLayout->addWidget(writeDacCheck, 2, 5);
            operationLayout->addWidget(writeOwnerCheck, 3, 1);
            operationLayout->addWidget(new QLabel(QStringLiteral("Mask"), operationGroup), 4, 0);
            operationLayout->addWidget(customMaskEdit, 4, 1, 1, 3);
            operationLayout->addWidget(applyAceButton, 4, 4);
            operationLayout->addWidget(deleteAceButton, 4, 5);
            operationLayout->addWidget(refreshButton, 5, 5);
            layout->addWidget(operationGroup, 0);

            QLabel* statusLabel = new QLabel(QStringLiteral("● 正在读取安全描述符..."), page);
            statusLabel->setTextInteractionFlags(Qt::TextSelectableByMouse);
            layout->addWidget(statusLabel, 0);

            QSplitter* splitter = new QSplitter(Qt::Vertical, page);
            QTableWidget* aceTable = new QTableWidget(splitter);
            aceTable->setColumnCount(9);
            aceTable->setHorizontalHeaderLabels(QStringList{
                QStringLiteral("范围"),
                QStringLiteral("序号"),
                QStringLiteral("类型"),
                QStringLiteral("账户"),
                QStringLiteral("SID"),
                QStringLiteral("Mask"),
                QStringLiteral("权限"),
                QStringLiteral("标志"),
                QStringLiteral("编辑状态")
                });
            aceTable->setAlternatingRowColors(true);
            aceTable->setSelectionBehavior(QAbstractItemView::SelectRows);
            aceTable->setSelectionMode(QAbstractItemView::SingleSelection);
            aceTable->setEditTriggers(QAbstractItemView::NoEditTriggers);
            aceTable->setSortingEnabled(true);
            if (aceTable->horizontalHeader() != nullptr)
            {
                aceTable->horizontalHeader()->setStretchLastSection(true);
            }

            CodeEditorWidget* detailEditor = new CodeEditorWidget(splitter);
            detailEditor->setReadOnly(true);
            detailEditor->setText(baseContent + QStringLiteral(
                "\n深层 Owner/Group/DACL/SACL 正在后台加载...\n"
                "\n操作说明：\n"
                "- 上方表格用于结构化展示 ACE；继承 ACE、SACL、对象 ACE 默认只读展示。\n"
                "- 应用 ACE 使用 Windows 安全 API 写 DACL，失败会保留错误码。\n"
                "- 删除选中 ACE 只删除非继承 DACL 中精确匹配的当前 ACE。\n"));

            splitter->addWidget(aceTable);
            splitter->addWidget(detailEditor);
            splitter->setStretchFactor(0, 3);
            splitter->setStretchFactor(1, 2);
            layout->addWidget(splitter, 1);

            const auto syncChecksFromPreset = [this, presetCombo, readCheck, writeCheck, executeCheck, deleteCheck, writeDacCheck, writeOwnerCheck]()
                {
                    const DWORD presetMask = maskFromSecurityPreset(presetCombo->currentIndex());
                    readCheck->setChecked((presetMask & FILE_GENERIC_READ) == FILE_GENERIC_READ);
                    writeCheck->setChecked((presetMask & FILE_GENERIC_WRITE) == FILE_GENERIC_WRITE);
                    executeCheck->setChecked((presetMask & FILE_GENERIC_EXECUTE) == FILE_GENERIC_EXECUTE);
                    deleteCheck->setChecked((presetMask & DELETE) != 0);
                    writeDacCheck->setChecked((presetMask & WRITE_DAC) != 0);
                    writeOwnerCheck->setChecked((presetMask & WRITE_OWNER) != 0);
                };
            syncChecksFromPreset();
            connect(presetCombo, &QComboBox::currentIndexChanged, this, [syncChecksFromPreset](int)
                {
                    syncChecksFromPreset();
                });

            const auto refreshSecurityUi = [this, aceTable, detailEditor, statusLabel, baseContent, nativePath]()
                {
                    if (aceTable != nullptr)
                    {
                        aceTable->setRowCount(0);
                    }
                    if (statusLabel != nullptr)
                    {
                        statusLabel->setText(QStringLiteral("● 正在读取安全描述符..."));
                    }
                    if (detailEditor != nullptr)
                    {
                        detailEditor->setText(baseContent + QStringLiteral("\n深层 Owner/Group/DACL/SACL 正在后台加载...\n"));
                    }
                    startSecurityDeepLoad(aceTable, detailEditor, statusLabel, baseContent, nativePath);
                };

            connect(refreshButton, &QPushButton::clicked, this, refreshSecurityUi);
            connect(applyAceButton, &QPushButton::clicked, this,
                [this,
                accountEdit,
                accessModeCombo,
                inheritanceCombo,
                customMaskEdit,
                readCheck,
                writeCheck,
                executeCheck,
                deleteCheck,
                writeDacCheck,
                writeOwnerCheck,
                statusLabel,
                refreshSecurityUi]()
                {
                    DWORD accessMask = maskFromSecurityChecks(
                        readCheck,
                        writeCheck,
                        executeCheck,
                        deleteCheck,
                        writeDacCheck,
                        writeOwnerCheck);
                    const QString customMaskText = customMaskEdit->text().trimmed();
                    if (!customMaskText.isEmpty())
                    {
                        bool parseOk = false;
                        const qulonglong parsedMask = customMaskText.toULongLong(&parseOk, 0);
                        if (!parseOk || parsedMask > 0xFFFFFFFFULL)
                        {
                            QMessageBox::warning(this, QStringLiteral("权限编辑"), QStringLiteral("自定义 Mask 格式无效：%1").arg(customMaskText));
                            return;
                        }
                        accessMask = static_cast<DWORD>(parsedMask);
                    }

                    QString detailText;
                    const ACCESS_MODE accessMode = static_cast<ACCESS_MODE>(accessModeCombo->currentData().toInt());
                    const DWORD result = applySecurityAceChange(
                        accountEdit->text(),
                        accessMask,
                        accessMode,
                        inheritanceFlagsFromCombo(inheritanceCombo->currentIndex()),
                        detailText);
                    if (statusLabel != nullptr)
                    {
                        statusLabel->setText(result == ERROR_SUCCESS
                            ? QStringLiteral("● %1").arg(detailText)
                            : QStringLiteral("● %1").arg(detailText));
                    }
                    if (result != ERROR_SUCCESS)
                    {
                        QMessageBox::warning(this, QStringLiteral("权限编辑"), detailText);
                        refreshSecurityUi();
                        return;
                    }
                    refreshSecurityUi();
                });

            connect(deleteAceButton, &QPushButton::clicked, this, [this, aceTable, statusLabel, refreshSecurityUi]()
                {
                    const QMessageBox::StandardButton userChoice = QMessageBox::question(
                        this,
                        QStringLiteral("删除 ACE"),
                        QStringLiteral("将删除当前选中的非继承 DACL ACE。\n继承 ACE 需要到来源目录修改。是否继续？"),
                        QMessageBox::Yes | QMessageBox::No,
                        QMessageBox::No);
                    if (userChoice != QMessageBox::Yes)
                    {
                        return;
                    }

                    QString detailText;
                    const DWORD result = deleteSelectedDaclAce(aceTable, detailText);
                    if (statusLabel != nullptr)
                    {
                        statusLabel->setText(QStringLiteral("● %1").arg(detailText));
                    }
                    if (result != ERROR_SUCCESS)
                    {
                        QMessageBox::warning(this, QStringLiteral("删除 ACE"), detailText);
                        refreshSecurityUi();
                        return;
                    }
                    refreshSecurityUi();
                });

            QMetaObject::invokeMethod(page, refreshSecurityUi, Qt::QueuedConnection);
            return page;
        }

        QWidget* buildHashTab()
        {
            QWidget* page = new QWidget(this);
            QVBoxLayout* layout = new QVBoxLayout(page);

            QHBoxLayout* toolbarLayout = new QHBoxLayout();
            QPushButton* startButton = new QPushButton(QStringLiteral("计算 SHA256"), page);
            QPushButton* cancelButton = new QPushButton(QStringLiteral("取消"), page);
            cancelButton->setEnabled(false);
            toolbarLayout->addWidget(startButton, 0);
            toolbarLayout->addWidget(cancelButton, 0);
            toolbarLayout->addStretch(1);
            layout->addLayout(toolbarLayout);

            QProgressBar* progressBar = new QProgressBar(page);
            progressBar->setRange(0, 1000);
            progressBar->setValue(0);
            layout->addWidget(progressBar, 0);

            CodeEditorWidget* textEditorWidget = new CodeEditorWidget(page);
            textEditorWidget->setReadOnly(true);
            layout->addWidget(textEditorWidget, 1);

            textEditorWidget->setText(QStringLiteral(
                "Phase 10 哈希页：\n"
                "- SHA256 使用用户态流式读取，避免一次性读入大文件。\n"
                "- 点击“取消”会在下一个块读取边界停止。\n"
                "- 可用 PowerShell Get-FileHash -Algorithm SHA256 进行对比。\n"));

            connect(startButton, &QPushButton::clicked, this, [this, textEditorWidget, progressBar, startButton, cancelButton]()
                {
                    startHashCalculation(textEditorWidget, progressBar, startButton, cancelButton);
                });
            connect(cancelButton, &QPushButton::clicked, this, [this, cancelButton]()
                {
                    requestHashCancel(cancelButton);
                });
            return page;
        }

        QWidget* buildUsageTab()
        {
            // 用途：Phase-10 把现有文件占用扫描结果直接嵌入属性窗口。
            // 处理：后台调用 FileHandleUsageScanner；Scanner 当前会优先使用用户态句柄快照，
            // 并在诊断中标记 DuplicateHandle/NtObjectName 等来源。后续 R0 HandleTable
            // 增强可以在 Scanner 内部扩展，属性页无需直接 DeviceIoControl。
            QWidget* page = new QWidget(this);
            QVBoxLayout* layout = new QVBoxLayout(page);

            QHBoxLayout* toolbarLayout = new QHBoxLayout();
            QPushButton* refreshButton = new QPushButton(QStringLiteral("刷新占用"), page);
            QLabel* statusLabel = new QLabel(QStringLiteral("● 等待扫描"), page);
            statusLabel->setTextInteractionFlags(Qt::TextSelectableByMouse);
            toolbarLayout->addWidget(refreshButton, 0);
            toolbarLayout->addWidget(statusLabel, 1);
            layout->addLayout(toolbarLayout);

            QTreeWidget* table = new QTreeWidget(page);
            table->setColumnCount(7);
            table->setHeaderLabels(QStringList{
                QStringLiteral("PID"),
                QStringLiteral("进程名"),
                QStringLiteral("Handle"),
                QStringLiteral("GrantedAccess"),
                QStringLiteral("对象/路径"),
                QStringLiteral("命中目标"),
                QStringLiteral("枚举来源")
                });
            table->setRootIsDecorated(false);
            table->setAlternatingRowColors(true);
            table->setSelectionBehavior(QAbstractItemView::SelectRows);
            table->setEditTriggers(QAbstractItemView::NoEditTriggers);
            table->setSortingEnabled(true);
            if (table->header() != nullptr)
            {
                table->header()->setStretchLastSection(true);
            }
            layout->addWidget(table, 1);

            connect(refreshButton, &QPushButton::clicked, this, [this, table, statusLabel, refreshButton]()
                {
                    refreshUsageTable(table, statusLabel, refreshButton);
                });

            QMetaObject::invokeMethod(
                page,
                [this, table, statusLabel, refreshButton]()
                {
                    refreshUsageTable(table, statusLabel, refreshButton);
                },
                Qt::QueuedConnection);
            return page;
        }

        QWidget* buildSignatureTab()
        {
            QWidget* page = new QWidget(this);
            QVBoxLayout* layout = new QVBoxLayout(page);
            CodeEditorWidget* textEditorWidget = new CodeEditorWidget(page);
            textEditorWidget->setReadOnly(true);
            layout->addWidget(textEditorWidget, 1);
            startSignatureLoad(textEditorWidget);
            return page;
        }

        QWidget* buildPeTab()
        {
            QWidget* page = new QWidget(this);
            QVBoxLayout* layout = new QVBoxLayout(page);
            CodeEditorWidget* textEditorWidget = new CodeEditorWidget(page);
            textEditorWidget->setReadOnly(true);

            layout->addWidget(textEditorWidget, 1);
            startPeAnalysisLoad(textEditorWidget);
            return page;
        }

        QWidget* buildDependencyTab()
        {
            // 用途：创建“依赖 DLL”页 UI。
            // 处理：表格展示 DLL/函数/Ordinal/Hint/IAT RVA，底部文本显示摘要或错误；
            //      真实 PE Import Directory 解析由后台线程完成。
            // 返回：可嵌入属性窗口的 QWidget。
            QWidget* page = new QWidget(this);
            QVBoxLayout* layout = new QVBoxLayout(page);

            QLabel* statusLabel = new QLabel(QStringLiteral("● 等待加载依赖 DLL"), page);
            statusLabel->setTextInteractionFlags(Qt::TextSelectableByMouse);
            layout->addWidget(statusLabel, 0);

            QTableWidget* table = new QTableWidget(page);
            table->setColumnCount(6);
            table->setHorizontalHeaderLabels(QStringList{
                QStringLiteral("DLL 名称"),
                QStringLiteral("函数名 / Ordinal"),
                QStringLiteral("Hint"),
                QStringLiteral("导入方式"),
                QStringLiteral("Thunk/IAT RVA"),
                QStringLiteral("诊断")
                });
            table->setAlternatingRowColors(true);
            table->setSelectionBehavior(QAbstractItemView::SelectRows);
            table->setSelectionMode(QAbstractItemView::ExtendedSelection);
            table->setEditTriggers(QAbstractItemView::NoEditTriggers);
            table->setSortingEnabled(true);
            table->setContextMenuPolicy(Qt::CustomContextMenu);
            if (table->horizontalHeader() != nullptr)
            {
                table->horizontalHeader()->setStretchLastSection(true);
            }
            layout->addWidget(table, 3);

            CodeEditorWidget* detailEditor = new CodeEditorWidget(page);
            detailEditor->setReadOnly(true);
            layout->addWidget(detailEditor, 1);

            connect(table, &QTableWidget::customContextMenuRequested, this, [table](const QPoint& position)
                {
                    if (table == nullptr)
                    {
                        return;
                    }

                    QMenu menu(table);
                    QAction* copyRowsAction = menu.addAction(QStringLiteral("复制选中行"));
                    QAction* copyDllAction = menu.addAction(QStringLiteral("复制 DLL 名称"));
                    QAction* selectedAction = menu.exec(table->viewport()->mapToGlobal(position));
                    if (selectedAction == nullptr)
                    {
                        return;
                    }
                    if (selectedAction != copyRowsAction && selectedAction != copyDllAction)
                    {
                        return;
                    }

                    const bool dllOnly = selectedAction == copyDllAction;
                    const QString clipboardText = dependencyRowsToClipboardText(table, dllOnly);
                    if (!clipboardText.isEmpty() && QApplication::clipboard() != nullptr)
                    {
                        QApplication::clipboard()->setText(clipboardText);
                    }
                });

            startDependencyLoad(table, statusLabel, detailEditor);
            return page;
        }

        QWidget* buildStringsTab()
        {
            QWidget* page = new QWidget(this);
            QVBoxLayout* layout = new QVBoxLayout(page);
            CodeEditorWidget* textEditorWidget = new CodeEditorWidget(page);
            textEditorWidget->setReadOnly(true);
            layout->addWidget(textEditorWidget, 1);
            startStringsLoad(textEditorWidget);

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
        std::shared_ptr<std::atomic_bool> m_hashCancelRequested; // 哈希计算取消标记，后台线程共享。
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

FileDock::~FileDock()
{
    // 析构阶段只发出停止信号，避免后台线程继续等待新的 UI 选择结果。
    m_unlockerWorkerStopRequested.store(true);
    std::thread workerThread;
    {
        std::lock_guard<std::mutex> lock(m_unlockerWorkerMutex);
        if (m_unlockerWorkerThread.joinable())
        {
            workerThread = std::move(m_unlockerWorkerThread);
        }
    }
    if (workerThread.joinable())
    {
        workerThread.join();
    }
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

    // 驱动器下拉框：
    // - 固定放在地址栏右侧，直接跳转任意盘符根目录；
    // - 解决默认路径体验更偏向当前系统盘的问题。
    panel.driveCombo = new QComboBox(panel.navWidget);
    panel.driveCombo->setStyleSheet(buildBlueInputStyle());
    panel.driveCombo->setMinimumWidth(92);
    panel.driveCombo->setMaximumWidth(140);
    panel.driveCombo->setToolTip(QStringLiteral("快速跳转到任意驱动器根目录"));

    panel.pathStack->addWidget(panel.breadcrumbWidget);
    panel.pathStack->addWidget(panel.pathEdit);

    panel.navLayout->addWidget(panel.backButton);
    panel.navLayout->addWidget(panel.forwardButton);
    panel.navLayout->addWidget(panel.upButton);
    panel.navLayout->addWidget(panel.refreshButton);
    panel.navLayout->addWidget(panel.pathStack, 1);
    panel.navLayout->addWidget(panel.driveCombo, 0);
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
    panel.readModeCombo->addItems(QStringList{
        QStringLiteral("Windows API"),
        QStringLiteral("手动解析文件系统"),
        QStringLiteral("作为NTFS解析"),
        QStringLiteral("作为FAT32解析"),
        QStringLiteral("作为exFAT解析") });
    panel.readModeCombo->setToolTip(QStringLiteral("切换目录读取方式：Windows API、自动手动解析，或强制按 NTFS/FAT32/exFAT 解析。"));

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

    panel.fsModel = new ReparseAwareFileSystemModel(panel.rootWidget);
    panel.fsModel->setReadOnly(false);
    panel.fsModel->setResolveSymlinks(true);
    panel.fsModel->setFilter(QDir::AllEntries | QDir::NoDotAndDotDot);
    // 关闭“仅灰显不隐藏”行为，确保名称过滤严格只显示匹配项。
    panel.fsModel->setNameFilterDisables(false);

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
    refreshDriveCombo(panel);

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

    // 驱动器下拉框切换：直接跳转到对应盘符根目录。
    connect(panel.driveCombo, &QComboBox::currentIndexChanged, this, [this, &panel](const int indexValue) {
        if (indexValue < 0)
        {
            return;
        }

        const QString targetRootPath = panel.driveCombo->itemData(indexValue).toString();
        if (targetRootPath.trimmed().isEmpty())
        {
            return;
        }

        if (panel.currentPath.compare(targetRootPath, Qt::CaseInsensitive) == 0)
        {
            return;
        }

        kLogEvent event;
        info << event
            << "[FileDock] 驱动器下拉框跳转, panel="
            << panel.panelNameText.toStdString()
            << ", targetRoot="
            << QDir::toNativeSeparators(targetRootPath).toStdString()
            << eol;
        navigateToPath(panel, targetRootPath, true);
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
        panel.manualLoadedPath.clear();
        if (panel.manualModel != nullptr)
        {
            panel.manualModel->setRowCount(0);
        }
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
    openShortcut->setContext(Qt::WidgetShortcut);
    connect(openShortcut, &QShortcut::activated, this, [this, &panel]() {
        openSelectedItems(panel);
    });
    QShortcut* openShortcutEnter = new QShortcut(QKeySequence(Qt::Key_Enter), panel.fileView);
    openShortcutEnter->setContext(Qt::WidgetShortcut);
    connect(openShortcutEnter, &QShortcut::activated, this, [this, &panel]() {
        openSelectedItems(panel);
    });

    // F2 重命名快捷键。
    QShortcut* renameShortcut = new QShortcut(QKeySequence(Qt::Key_F2), panel.fileView);
    renameShortcut->setContext(Qt::WidgetShortcut);
    connect(renameShortcut, &QShortcut::activated, this, [this, &panel]() {
        renameSelectedItem(panel);
    });

    // Delete 删除快捷键。
    QShortcut* deleteShortcut = new QShortcut(QKeySequence(Qt::Key_Delete), panel.fileView);
    deleteShortcut->setContext(Qt::WidgetShortcut);
    connect(deleteShortcut, &QShortcut::activated, this, [this, &panel]() {
        deleteSelectedItem(panel);
    });

    // Ctrl+C/Ctrl+X/Ctrl+V 常用文件操作快捷键。
    QShortcut* copyShortcut = new QShortcut(QKeySequence::Copy, panel.fileView);
    copyShortcut->setContext(Qt::WidgetShortcut);
    connect(copyShortcut, &QShortcut::activated, this, [this, &panel]() {
        copySelectedItems(panel);
    });
    QShortcut* cutShortcut = new QShortcut(QKeySequence::Cut, panel.fileView);
    cutShortcut->setContext(Qt::WidgetShortcut);
    connect(cutShortcut, &QShortcut::activated, this, [this, &panel]() {
        cutSelectedItems(panel);
    });
    QShortcut* pasteShortcut = new QShortcut(QKeySequence::Paste, panel.fileView);
    pasteShortcut->setContext(Qt::WidgetShortcut);
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

    QString normalizedPath = QDir::cleanPath(QDir::fromNativeSeparators(trimmedPath));

    // 允许用户直接输入裸盘符（如 D:）后跳转到盘根目录。
    if (normalizedPath.size() == 2
        && normalizedPath.at(1) == QChar(':')
        && normalizedPath.at(0).isLetter())
    {
        normalizedPath += QDir::separator();
    }

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
    refreshDriveCombo(panel);

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
        "QPushButton{border:none;background:transparent;color:%1;}"
        "QPushButton:hover{background:%2;color:%1;}")
        .arg(KswordTheme::TextPrimaryColorHex())
        .arg(KswordTheme::IsDarkModeEnabled() ? KswordTheme::SurfaceMutedColorHex() : KswordTheme::PrimaryBlueSubtleHex()));
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
        // 手动模式下仅在“当前路径未加载且未在解析”时才拉起新任务，
        // 避免过滤/排序改动触发同一路径重复全盘扫描。
        const bool loadedMatchesCurrentPath =
            (panel.manualLoadedPath.compare(panel.currentPath, Qt::CaseInsensitive) == 0);
        const bool samePathParsing =
            panel.manualParseInProgress
            && (panel.manualParsingPath.compare(panel.currentPath, Qt::CaseInsensitive) == 0);
        if (!loadedMatchesCurrentPath && !samePathParsing)
        {
            requestAsyncManualReload(panel, false);
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
            panel.fsModel->setNameFilters(QStringList());
        }
        else
        {
            panel.fsModel->setNameFilters(QStringList{
                buildLiteralNameFilterPattern(filterText)
                });
        }
        // Windows API 模式改由 QFileSystemModel 执行名称过滤，代理层只保留排序职责。
        panel.proxyModel->setFilterRegularExpression(QRegularExpression());

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

void FileDock::refreshDriveCombo(FilePanelWidgets& panel)
{
    if (panel.driveCombo == nullptr)
    {
        return;
    }

    const QSignalBlocker blocker(panel.driveCombo);
    panel.driveCombo->clear();

    const QFileInfoList driveList = QDir::drives();
    int selectedIndex = -1;
    for (const QFileInfo& driveInfo : driveList)
    {
        const QString rootPath = QDir::toNativeSeparators(driveInfo.absoluteFilePath());
        QString displayText = rootPath;
        if (displayText.endsWith(QDir::separator()))
        {
            displayText.chop(1);
        }
        panel.driveCombo->addItem(displayText, rootPath);

        if (!panel.currentPath.isEmpty()
            && panel.currentPath.startsWith(driveInfo.absoluteFilePath(), Qt::CaseInsensitive))
        {
            selectedIndex = panel.driveCombo->count() - 1;
        }
    }

    if (selectedIndex >= 0)
    {
        panel.driveCombo->setCurrentIndex(selectedIndex);
    }
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
            const ks::file::ManualFsType requestedFsType = requestedManualFsTypeForPanel(panel);
            panel.parserStatusLabel->setText(
                requestedFsType == ks::file::ManualFsType::Unknown
                ? QStringLiteral("解析器: 手动解析")
                : QStringLiteral("解析器: %1 (待解析)").arg(manualFsTypeToText(requestedFsType)));
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
    // usedWinApiFallback：记录手动 NTFS 解析是否已经降级到 Windows API。
    bool usedWinApiFallback = false;
    const ks::file::ManualFsType requestedFsType = requestedManualFsTypeForPanel(panel);
    const bool parseOk = ks::file::ManualFileSystemParser::enumerateDirectory(
        panel.currentPath,
        entries,
        fsType,
        errorText,
        &usedWinApiFallback,
        requestedFsType);

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

        QString typeText = itemValue.typeText;
        const QString reparseMarkerText = reparseKindMarkerForPath(itemValue.absolutePath);
        if (!reparseMarkerText.isEmpty())
        {
            typeText = QStringLiteral("%1 / %2").arg(reparseMarkerText, typeText);
        }
        rowItems.push_back(new QStandardItem(typeText));
        rowItems.push_back(new QStandardItem(itemValue.modifiedTime.isValid()
            ? itemValue.modifiedTime.toString(QStringLiteral("yyyy-MM-dd HH:mm:ss"))
            : QStringLiteral("-")));
        rowItems.push_back(new QStandardItem(QDir::toNativeSeparators(itemValue.absolutePath)));
        rowItems.push_back(new QStandardItem(itemValue.isDirectory ? QStringLiteral("1") : QStringLiteral("0")));
        panel.manualModel->appendRow(rowItems);
    }

    if (panel.parserStatusLabel != nullptr)
    {
        // 手动链路失败后若已回退到 Windows API，则必须明确展示真实来源，避免 UI 误导。
        if (usedWinApiFallback)
        {
            panel.parserStatusLabel->setText(
                QStringLiteral("解析器: Windows API 回退 (%1)")
                .arg(manualFsTypeToText(fsType)));
        }
        else
        {
            panel.parserStatusLabel->setText(
                QStringLiteral("解析器: %1 (手动)")
                .arg(manualFsTypeToText(fsType)));
        }
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

void FileDock::requestAsyncManualReload(FilePanelWidgets& panel, const bool showWarningMessage)
{
    if (panel.manualModel == nullptr || panel.currentPath.isEmpty())
    {
        return;
    }

    // requestedPath：记录本次调用目标路径，避免在异步流程里读取到后续变更值。
    const QString requestedPath = panel.currentPath;
    const ks::file::ManualFsType requestedFsType = requestedManualFsTypeForPanel(panel);

    // 路径已经加载且当前没有任务运行时，直接复用结果，避免无意义重复解析。
    if (!panel.manualParseInProgress
        && panel.manualLoadedPath.compare(requestedPath, Qt::CaseInsensitive) == 0
        && panel.manualRequestedFsType == requestedFsType)
    {
        return;
    }

    panel.manualRequestedFsType = requestedFsType;

    // 若已有后台任务在跑，仅在“目标路径发生变化”时登记 pending。
    // 同一路径重复触发（例如用户调排序/过滤）不再触发第二次全盘解析。
    if (panel.manualParseInProgress)
    {
        const bool samePathRunning =
            (panel.manualParsingPath.compare(requestedPath, Qt::CaseInsensitive) == 0);
        if (samePathRunning)
        {
            panel.manualParsePendingShowWarning =
                panel.manualParsePendingShowWarning || showWarningMessage;
            return;
        }

        panel.manualParsePending = true;
        panel.manualParsePendingShowWarning = panel.manualParsePendingShowWarning || showWarningMessage;

        {
            kLogEvent event;
            dbg << event
                << "[FileDock] 手动解析任务排队, panel="
                << panel.panelNameText.toStdString()
                << ", runningPath="
                << QDir::toNativeSeparators(panel.manualParsingPath).toStdString()
                << ", pendingPath="
                << QDir::toNativeSeparators(requestedPath).toStdString()
                << eol;
        }
        return;
    }

    panel.manualParseInProgress = true;
    panel.manualParsePending = false;
    panel.manualParsePendingShowWarning = false;
    panel.manualParseRequestSerial += 1;
    panel.manualParsingPath = requestedPath;

    // 记录请求上下文：用于后台线程回传时校验“结果是否过期”。
    const int requestSerial = panel.manualParseRequestSerial;
    const QString requestPath = requestedPath;
    const QString panelNameText = panel.panelNameText;
    const bool leftPanelRequest = (&panel == &m_leftPanel);

    // 手动解析进度条：满足“手动解析要可视化进度”的需求。
    const int progressPid = kPro.add("文件", (panelNameText + QStringLiteral(" 手动解析")).toStdString());
    kPro.set(progressPid, "准备解析目录", 0, 5.0f);

    if (panel.parserStatusLabel != nullptr)
    {
        panel.parserStatusLabel->setText(QStringLiteral("解析器: 手动解析中..."));
    }
    if (panel.readModeCombo != nullptr)
    {
        panel.readModeCombo->setEnabled(false);
    }

    {
        kLogEvent event;
        info << event
            << "[FileDock] 启动异步手动解析, panel="
            << panelNameText.toStdString()
            << ", path="
            << QDir::toNativeSeparators(requestPath).toStdString()
            << ", requestSerial="
            << requestSerial
            << eol;
    }

    QPointer<FileDock> safeThis(this);
    std::thread([safeThis, leftPanelRequest, requestPath, requestedFsType, panelNameText, showWarningMessage, requestSerial, progressPid]() {
        kPro.set(progressPid, "解析文件系统结构中", 0, 35.0f);

        std::vector<ks::file::ManualDirectoryEntry> parsedEntries;
        ks::file::ManualFsType parsedFsType = ks::file::ManualFsType::Unknown;
        QString parseErrorText;
        // usedWinApiFallback：记录后台解析是否已退回到 Windows API，供 UI 正确显示状态。
        bool usedWinApiFallback = false;
        const bool parseOk = ks::file::ManualFileSystemParser::enumerateDirectory(
            requestPath,
            parsedEntries,
            parsedFsType,
            parseErrorText,
            &usedWinApiFallback,
            requestedFsType);

        kPro.set(progressPid, parseOk ? "生成目录列表中" : "解析失败，整理错误信息", 0, 78.0f);

        if (safeThis.isNull())
        {
            kPro.set(progressPid, "界面已关闭", 0, 100.0f);
            return;
        }

        const bool invokeOk = QMetaObject::invokeMethod(
            safeThis.data(),
            [safeThis, leftPanelRequest, requestPath, requestedFsType, panelNameText, showWarningMessage, requestSerial, progressPid, parseOk, parsedEntries, parsedFsType, parseErrorText, usedWinApiFallback]() {
                if (safeThis.isNull())
                {
                    kPro.set(progressPid, "界面已关闭", 0, 100.0f);
                    return;
                }

                FilePanelWidgets& targetPanel = leftPanelRequest ? safeThis->m_leftPanel : safeThis->m_rightPanel;
                if (targetPanel.manualParseRequestSerial != requestSerial)
                {
                    // 过期结果直接丢弃，避免“慢任务覆盖新路径数据”。
                    if (targetPanel.manualParsingPath.compare(requestPath, Qt::CaseInsensitive) == 0)
                    {
                        targetPanel.manualParseInProgress = false;
                        targetPanel.manualParsingPath.clear();
                        if (targetPanel.readModeCombo != nullptr)
                        {
                            targetPanel.readModeCombo->setEnabled(true);
                        }
                    }

                    {
                        kLogEvent event;
                        warn << event
                            << "[FileDock] 丢弃过期手动解析结果, panel="
                            << panelNameText.toStdString()
                            << ", path="
                            << QDir::toNativeSeparators(requestPath).toStdString()
                            << ", requestSerial="
                            << requestSerial
                            << ", currentSerial="
                            << targetPanel.manualParseRequestSerial
                            << eol;
                    }

                    kPro.set(progressPid, "结果过期已忽略", 0, 100.0f);

                    if (!targetPanel.manualParseInProgress && targetPanel.manualParsePending)
                    {
                        const bool pendingShowWarning = targetPanel.manualParsePendingShowWarning;
                        targetPanel.manualParsePending = false;
                        targetPanel.manualParsePendingShowWarning = false;
                        safeThis->requestAsyncManualReload(targetPanel, pendingShowWarning);
                    }
                    return;
                }

                targetPanel.manualParseInProgress = false;
                targetPanel.manualParsingPath.clear();
                targetPanel.manualRequestedFsType = requestedFsType;
                if (targetPanel.readModeCombo != nullptr)
                {
                    targetPanel.readModeCombo->setEnabled(true);
                }

                targetPanel.manualModel->setRowCount(0);
                targetPanel.lastManualFsType = parsedFsType;

                if (!parseOk)
                {
                    // 失败时也记住路径，避免过滤/排序触发连续重试。
                    targetPanel.manualLoadedPath = requestPath;
                    if (targetPanel.parserStatusLabel != nullptr)
                    {
                        targetPanel.parserStatusLabel->setText(QStringLiteral("解析器: 手动解析失败"));
                    }
                    if (showWarningMessage)
                    {
                        QMessageBox::warning(
                            safeThis.data(),
                            QStringLiteral("手动解析失败"),
                            QStringLiteral("路径: %1\n错误: %2")
                            .arg(QDir::toNativeSeparators(requestPath))
                            .arg(parseErrorText));
                    }

                    kLogEvent event;
                    warn << event
                        << "[FileDock] 异步手动解析失败, panel="
                        << panelNameText.toStdString()
                        << ", path="
                        << QDir::toNativeSeparators(requestPath).toStdString()
                        << ", error="
                        << parseErrorText.toStdString()
                        << eol;
                }
                else
                {
                    // 批量回填模型：
                    // - 不再阻断 manualModel 信号，避免 proxy 无法感知新增行导致“日志显示有 rows 但视图空白”。
                    // - 通过临时关闭视图重绘降低批量插入期间的 UI 开销。
                    if (targetPanel.fileView != nullptr)
                    {
                        targetPanel.fileView->setUpdatesEnabled(false);
                    }
                    for (const ks::file::ManualDirectoryEntry& itemValue : parsedEntries)
                    {
                        QList<QStandardItem*> rowItems;
                        rowItems.reserve(static_cast<int>(ManualModelColumn::Count));

                        QStandardItem* nameItem = new QStandardItem(itemValue.name);
                        nameItem->setData(itemValue.absolutePath, Qt::UserRole);
                        nameItem->setData(itemValue.isDirectory, Qt::UserRole + 1);
                        rowItems.push_back(nameItem);

                        QStandardItem* sizeItem = new QStandardItem(
                            itemValue.isDirectory ? QStringLiteral("-") : formatSizeText(itemValue.sizeBytes));
                        sizeItem->setData(static_cast<qulonglong>(itemValue.sizeBytes), Qt::UserRole);
                        rowItems.push_back(sizeItem);

                        QString typeText = itemValue.typeText;
                        const QString reparseMarkerText = reparseKindMarkerForPath(itemValue.absolutePath);
                        if (!reparseMarkerText.isEmpty())
                        {
                            typeText = QStringLiteral("%1 / %2").arg(reparseMarkerText, typeText);
                        }
                        rowItems.push_back(new QStandardItem(typeText));
                        rowItems.push_back(new QStandardItem(itemValue.modifiedTime.isValid()
                            ? itemValue.modifiedTime.toString(QStringLiteral("yyyy-MM-dd HH:mm:ss"))
                            : QStringLiteral("-")));
                        rowItems.push_back(new QStandardItem(QDir::toNativeSeparators(itemValue.absolutePath)));
                        rowItems.push_back(new QStandardItem(itemValue.isDirectory ? QStringLiteral("1") : QStringLiteral("0")));
                        targetPanel.manualModel->appendRow(rowItems);
                    }
                    if (targetPanel.manualProxyModel != nullptr)
                    {
                        targetPanel.manualProxyModel->invalidate();
                    }
                    if (targetPanel.fileView != nullptr)
                    {
                        targetPanel.fileView->setRootIndex(QModelIndex());
                        targetPanel.fileView->setUpdatesEnabled(true);
                    }

                    if (targetPanel.parserStatusLabel != nullptr)
                    {
                        // 异步路径与同步路径保持同一展示规则，避免回退后仍伪装成手动解析。
                        if (usedWinApiFallback)
                        {
                            targetPanel.parserStatusLabel->setText(
                                QStringLiteral("解析器: Windows API 回退 (%1)")
                                .arg(manualFsTypeToText(parsedFsType)));
                        }
                        else
                        {
                            targetPanel.parserStatusLabel->setText(
                                QStringLiteral("解析器: %1 (手动)").arg(manualFsTypeToText(parsedFsType)));
                        }
                    }
                    targetPanel.manualLoadedPath = requestPath;

                    kLogEvent event;
                    info << event
                        << "[FileDock] 异步手动解析完成, panel="
                        << panelNameText.toStdString()
                        << ", fsType="
                        << manualFsTypeToText(parsedFsType).toStdString()
                        << ", rows="
                        << parsedEntries.size()
                        << ", path="
                        << QDir::toNativeSeparators(requestPath).toStdString()
                        << eol;
                }

                // 模型回填后重新应用过滤/排序，让视图立即更新到当前条件。
                safeThis->applyPanelFilterAndSort(targetPanel);
                kPro.set(progressPid, parseOk ? "手动解析完成" : "手动解析失败", 0, 100.0f);

                // 若解析过程中用户又切了目录，完成后立即执行挂起请求。
                if (!targetPanel.manualParseInProgress && targetPanel.manualParsePending)
                {
                    const bool pendingShowWarning = targetPanel.manualParsePendingShowWarning;
                    targetPanel.manualParsePending = false;
                    targetPanel.manualParsePendingShowWarning = false;
                    safeThis->requestAsyncManualReload(targetPanel, pendingShowWarning);
                }
            },
            Qt::QueuedConnection);

        if (!invokeOk)
        {
            kPro.set(progressPid, "回调失败", 0, 100.0f);
        }
    }).detach();
}

bool FileDock::currentModeIsManual(const FilePanelWidgets& panel) const
{
    return panel.readModeCombo != nullptr && panel.readModeCombo->currentIndex() >= 1;
}

ks::file::ManualFsType FileDock::requestedManualFsTypeForPanel(const FilePanelWidgets& panel) const
{
    if (panel.readModeCombo == nullptr)
    {
        return ks::file::ManualFsType::Unknown;
    }

    switch (panel.readModeCombo->currentIndex())
    {
    case 2:
        return ks::file::ManualFsType::Ntfs;
    case 3:
        return ks::file::ManualFsType::Fat32;
    case 4:
        return ks::file::ManualFsType::ExFat;
    default:
        return ks::file::ManualFsType::Unknown;
    }
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
    m_recoveryTable->setColumnCount(7);
    m_recoveryTable->setHorizontalHeaderLabels(QStringList{
        QStringLiteral("文件名"),
        QStringLiteral("路径提示"),
        QStringLiteral("大小"),
        QStringLiteral("修改时间"),
        QStringLiteral("记录号"),
        QStringLiteral("完整度"),
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
    // 对外保留同步入口名，内部改为异步实现，避免阻塞 UI。
    scanDeletedFilesForRecoveryAsync();
}

void FileDock::scanDeletedFilesForRecoveryAsync()
{
    if (m_recoveryVolumeCombo == nullptr || m_recoveryTable == nullptr || m_recoveryStatusLabel == nullptr)
    {
        return;
    }
    if (m_recoveryScanInProgress)
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
    m_recoveryScanInProgress = true;
    if (m_recoveryScanButton != nullptr)
    {
        m_recoveryScanButton->setEnabled(false);
    }
    if (m_recoveryExportButton != nullptr)
    {
        m_recoveryExportButton->setEnabled(false);
    }

    {
        kLogEvent event;
        info << event
            << "[FileDock] 开始扫描误删文件, volume="
            << QDir::toNativeSeparators(rootPath).toStdString()
            << eol;
    }

    const int progressPid = kPro.add("文件恢复", "扫描误删");
    kPro.set(progressPid, "准备扫描卷", 0, 5.0f);

    QPointer<FileDock> safeThis(this);
    std::thread([safeThis, rootPath, progressPid]() {
        QString errorText;
        std::vector<ks::file::NtfsDeletedFileEntry> deletedItems;

        kPro.set(progressPid, "准备读取 NTFS 元数据", 0, 3.0f);
        const bool scanOk = ks::file::ManualFileSystemParser::enumerateNtfsDeletedFiles(
            rootPath,
            deletedItems,
            errorText,
            [progressPid](const int percentValue, const QString& stageText) {
                const int boundedPercent = std::clamp(percentValue, 0, 100);
                kPro.set(progressPid, stageText.toStdString(), 0, static_cast<float>(boundedPercent));
            });
        if (!scanOk)
        {
            kPro.set(progressPid, "扫描失败，整理错误信息", 0, 82.0f);
        }

        if (safeThis.isNull())
        {
            kPro.set(progressPid, "界面已关闭", 0, 100.0f);
            return;
        }

        QMetaObject::invokeMethod(
            safeThis.data(),
            [safeThis, rootPath, progressPid, scanOk, deletedItems, errorText]() {
                if (safeThis.isNull())
                {
                    kPro.set(progressPid, "界面已关闭", 0, 100.0f);
                    return;
                }

                safeThis->m_recoveryScanInProgress = false;
                if (safeThis->m_recoveryScanButton != nullptr)
                {
                    safeThis->m_recoveryScanButton->setEnabled(true);
                }
                if (safeThis->m_recoveryExportButton != nullptr)
                {
                    safeThis->m_recoveryExportButton->setEnabled(true);
                }

                if (!scanOk)
                {
                    safeThis->m_recoveryStatusLabel->setText(QStringLiteral("扫描失败：%1").arg(errorText));
                    kLogEvent event;
                    err << event
                        << "[FileDock] 扫描误删失败, volume="
                        << QDir::toNativeSeparators(rootPath).toStdString()
                        << ", error="
                        << errorText.toStdString()
                        << eol;
                    QMessageBox::warning(safeThis.data(), QStringLiteral("扫描失败"), errorText);
                    kPro.set(progressPid, "扫描失败", 0, 100.0f);
                    return;
                }

                safeThis->m_deletedRecoveryItems = deletedItems;
                safeThis->m_recoveryTable->setUpdatesEnabled(false);
                safeThis->m_recoveryTable->setSortingEnabled(false);
                safeThis->m_recoveryTable->clearContents();
                safeThis->m_recoveryTable->setRowCount(static_cast<int>(safeThis->m_deletedRecoveryItems.size()));
                for (int row = 0; row < static_cast<int>(safeThis->m_deletedRecoveryItems.size()); ++row)
                {
                    const ks::file::NtfsDeletedFileEntry& itemValue = safeThis->m_deletedRecoveryItems[static_cast<std::size_t>(row)];
                    // 完整度文本：优先显示估计百分比，无法评估时明确标记为未知。
                    const QString integrityText =
                        (itemValue.estimatedIntegrityPercent >= 0)
                        ? QStringLiteral("%1%").arg(itemValue.estimatedIntegrityPercent)
                        : QStringLiteral("未知");

                    // 恢复能力文本：同时体现 resident、原始文件名是否保留，便于快速筛选。
                    QString recoverabilityText =
                        itemValue.residentDataReady
                        ? QStringLiteral("Resident可恢复")
                        : QStringLiteral("仅元数据");
                    if (!itemValue.hasOriginalName)
                    {
                        recoverabilityText += QStringLiteral(" / 缺名");
                    }

                    QTableWidgetItem* nameItem = new QTableWidgetItem(itemValue.fileName);
                    if (!itemValue.hasOriginalName)
                    {
                        nameItem->setToolTip(QStringLiteral("该条目原始文件名已丢失，当前名称为系统生成的占位名。"));
                    }
                    safeThis->m_recoveryTable->setItem(row, 0, nameItem);
                    safeThis->m_recoveryTable->setItem(row, 1, new QTableWidgetItem(itemValue.pathHint));
                    safeThis->m_recoveryTable->setItem(row, 2, new QTableWidgetItem(formatSizeText(itemValue.sizeBytes)));
                    safeThis->m_recoveryTable->setItem(row, 3, new QTableWidgetItem(
                        itemValue.modifiedTime.isValid()
                        ? itemValue.modifiedTime.toString(QStringLiteral("yyyy-MM-dd HH:mm:ss"))
                        : QStringLiteral("-")));
                    safeThis->m_recoveryTable->setItem(row, 4, new QTableWidgetItem(QString::number(itemValue.fileReference)));
                    safeThis->m_recoveryTable->setItem(row, 5, new QTableWidgetItem(integrityText));
                    safeThis->m_recoveryTable->setItem(row, 6, new QTableWidgetItem(recoverabilityText));
                }
                safeThis->m_recoveryTable->setUpdatesEnabled(true);

                const int residentReadyCount = static_cast<int>(std::count_if(
                    safeThis->m_deletedRecoveryItems.begin(),
                    safeThis->m_deletedRecoveryItems.end(),
                    [](const ks::file::NtfsDeletedFileEntry& item) { return item.residentDataReady; }));
                const int highIntegrityCount = static_cast<int>(std::count_if(
                    safeThis->m_deletedRecoveryItems.begin(),
                    safeThis->m_deletedRecoveryItems.end(),
                    [](const ks::file::NtfsDeletedFileEntry& item) { return item.estimatedIntegrityPercent >= 80; }));

                safeThis->m_recoveryStatusLabel->setText(
                    QStringLiteral("扫描完成：%1 项（Resident %2 项，完整度≥80%% %3 项）")
                    .arg(safeThis->m_deletedRecoveryItems.size())
                    .arg(residentReadyCount)
                    .arg(highIntegrityCount));

                kLogEvent event;
                info << event
                    << "[FileDock] 扫描误删完成, volume="
                    << QDir::toNativeSeparators(rootPath).toStdString()
                    << ", total="
                    << safeThis->m_deletedRecoveryItems.size()
                    << eol;
                kPro.set(progressPid, "扫描完成", 0, 100.0f);
            },
            Qt::QueuedConnection);
    }).detach();
}

void FileDock::recoverSelectedDeletedFiles()
{
    // 对外保留同步入口名，内部改为异步实现，避免阻塞 UI。
    recoverSelectedDeletedFilesAsync();
}

void FileDock::recoverSelectedDeletedFilesAsync()
{
    if (m_recoveryTable == nullptr || m_recoveryVolumeCombo == nullptr)
    {
        return;
    }
    if (m_recoveryRecoverInProgress)
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
    std::vector<ks::file::NtfsDeletedFileEntry> selectedItems;
    selectedItems.reserve(static_cast<std::size_t>(selectedRows.size()));
    for (const QModelIndex& rowIndex : selectedRows)
    {
        const int rowValue = rowIndex.row();
        if (rowValue < 0 || rowValue >= static_cast<int>(m_deletedRecoveryItems.size()))
        {
            continue;
        }
        selectedItems.push_back(m_deletedRecoveryItems[static_cast<std::size_t>(rowValue)]);
    }
    if (selectedItems.empty())
    {
        QMessageBox::information(this, QStringLiteral("文件恢复"), QStringLiteral("未读取到有效恢复条目。"));
        return;
    }

    m_recoveryRecoverInProgress = true;
    if (m_recoveryScanButton != nullptr)
    {
        m_recoveryScanButton->setEnabled(false);
    }
    if (m_recoveryExportButton != nullptr)
    {
        m_recoveryExportButton->setEnabled(false);
    }

    {
        kLogEvent event;
        info << event
            << "[FileDock] 开始恢复选中误删项, volume="
            << QDir::toNativeSeparators(volumeRoot).toStdString()
            << ", selectedRows="
            << selectedItems.size()
            << eol;
    }

    const int progressPid = kPro.add("文件恢复", "恢复选中");
    kPro.set(progressPid, "准备恢复", 0, 5.0f);

    QPointer<FileDock> safeThis(this);
    std::thread([safeThis, progressPid, volumeRoot, exportDir, selectedItems]() {
        int successCount = 0;
        QStringList failTextList;

        for (std::size_t index = 0; index < selectedItems.size(); ++index)
        {
            const ks::file::NtfsDeletedFileEntry& deletedItem = selectedItems[index];
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

            const float progress = 5.0f
                + (static_cast<float>(index + 1) / static_cast<float>(selectedItems.size())) * 90.0f;
            kPro.set(progressPid, "恢复处理中", 0, progress);
        }

        if (safeThis.isNull())
        {
            kPro.set(progressPid, "界面已关闭", 0, 100.0f);
            return;
        }

        QMetaObject::invokeMethod(
            safeThis.data(),
            [safeThis, progressPid, successCount, failTextList]() {
                if (safeThis.isNull())
                {
                    kPro.set(progressPid, "界面已关闭", 0, 100.0f);
                    return;
                }

                safeThis->m_recoveryRecoverInProgress = false;
                if (safeThis->m_recoveryScanButton != nullptr)
                {
                    safeThis->m_recoveryScanButton->setEnabled(true);
                }
                if (safeThis->m_recoveryExportButton != nullptr)
                {
                    safeThis->m_recoveryExportButton->setEnabled(true);
                }

                const QString summaryText = QStringLiteral("恢复完成：成功 %1，失败 %2。")
                    .arg(successCount)
                    .arg(failTextList.size());
                safeThis->m_recoveryStatusLabel->setText(summaryText);

                if (failTextList.empty())
                {
                    kLogEvent event;
                    info << event
                        << "[FileDock] 恢复完成, success="
                        << successCount
                        << ", failed=0"
                        << eol;
                    QMessageBox::information(safeThis.data(), QStringLiteral("文件恢复"), summaryText);
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
                        safeThis.data(),
                        QStringLiteral("文件恢复"),
                        summaryText + QStringLiteral("\n\n失败明细：\n") + failTextList.join('\n'));
                }

                kPro.set(progressPid, "恢复完成", 0, 100.0f);
            },
            Qt::QueuedConnection);
    }).detach();
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
    QStringList linkTargetList;
    for (const QString& path : menuPaths)
    {
        QFileInfo info(path);
        hasAnyFile = hasAnyFile || info.isFile();
        if (isPathReparsePoint(path))
        {
            const ks::file::ReparsePointQueryResult reparseResult = queryReparsePointForUi(path);
            const QString targetText = reparseTargetFromResult(reparseResult).trimmed();
            if (!targetText.isEmpty() && !linkTargetList.contains(targetText, Qt::CaseInsensitive))
            {
                linkTargetList.push_back(targetText);
            }
        }
    }
    const QString firstLinkTarget = (!linkTargetList.isEmpty() && isSingleSelection) ? linkTargetList.front() : QString();

    QMenu menu(this);
    menu.setStyleSheet(buildContextMenuStyle());
    QAction* openAction = menu.addAction(QIcon(":/Icon/process_start.svg"), QStringLiteral("打开/运行"));
    QAction* editAction = menu.addAction(QIcon(":/Icon/process_details.svg"), QStringLiteral("编辑（文本）"));
    QAction* copyPathAction = menu.addAction(QIcon(":/Icon/process_copy_cell.svg"), QStringLiteral("复制路径"));
    QAction* copyKernelPathAction = menu.addAction(QIcon(":/Icon/process_copy_cell.svg"), QStringLiteral("复制内核模式地址"));
    QAction* copyShortNameAction = menu.addAction(QIcon(":/Icon/process_copy_cell.svg"), QStringLiteral("复制短文件名"));
    QAction* copyLinkTargetAction = menu.addAction(QIcon(":/Icon/process_copy_cell.svg"), QStringLiteral("复制链接目标"));
    QAction* openLinkTargetAction = menu.addAction(QIcon(":/Icon/process_start.svg"), QStringLiteral("打开链接目标"));
    QAction* locateLinkTargetAction = menu.addAction(QIcon(":/Icon/process_open_folder.svg"), QStringLiteral("定位链接目标"));
    menu.addSeparator();
    QAction* copyAction = menu.addAction(QIcon(":/Icon/log_copy.svg"), QStringLiteral("复制"));
    QAction* cutAction = menu.addAction(QIcon(":/Icon/process_suspend.svg"), QStringLiteral("剪切"));
    QAction* pasteAction = menu.addAction(QIcon(":/Icon/process_resume.svg"), QStringLiteral("粘贴"));
    QAction* renameAction = menu.addAction(QIcon(":/Icon/process_priority.svg"), QStringLiteral("重命名(F2)"));
    QAction* deleteAction = menu.addAction(QIcon(":/Icon/process_terminate.svg"), QStringLiteral("删除(Delete)"));
    QAction* driverDeleteAction = menu.addAction(QIcon(":/Icon/process_terminate.svg"), QStringLiteral("驱动删除(R0)"));
    QAction* unlockByDriverAction = menu.addAction(QIcon(":/Icon/handle_refresh.svg"), QStringLiteral("文件解锁器(R3/R0)"));
    QAction* takeOwnerAction = menu.addAction(QIcon(":/Icon/file_owner.svg"), QStringLiteral("取得所有权"));
    menu.addSeparator();
    QAction* newFileAction = menu.addAction(QIcon(":/Icon/process_details.svg"), QStringLiteral("新建文件"));
    QAction* newFolderAction = menu.addAction(QIcon(":/Icon/process_open_folder.svg"), QStringLiteral("新建文件夹"));
    QAction* openTerminalAction = menu.addAction(QIcon(":/Icon/process_tree.svg"), QStringLiteral("在终端中打开"));
    menu.addSeparator();
    QAction* columnAction = menu.addAction(QIcon(":/Icon/process_list.svg"), QStringLiteral("选择列..."));
    QAction* detailAction = menu.addAction(QIcon(":/Icon/process_details.svg"), QStringLiteral("属性..."));
    menu.addSeparator();

    // 分析动作改为顶层菜单，减少层级并提升右键操作效率。
    QAction* hashAction = menu.addAction(QIcon(":/Icon/log_track.svg"), QStringLiteral("计算哈希值"));
    QAction* signAction = menu.addAction(QIcon(":/Icon/process_critical.svg"), QStringLiteral("检查数字签名"));
    QAction* entropyAction = menu.addAction(QIcon(":/Icon/process_uncritical.svg"), QStringLiteral("计算熵值"));
    QAction* hexAction = menu.addAction(QIcon(":/Icon/process_details.svg"), QStringLiteral("十六进制查看"));
    QAction* peAction = menu.addAction(QIcon(":/Icon/process_list.svg"), QStringLiteral("在PE查看器中打开"));
    QAction* mappedProcessScanAction = menu.addAction(QIcon(":/Icon/process_tree.svg"), QStringLiteral("扫描映射进程(R0)"));

    // 结合选中集合动态启用菜单项，保证“多选”和“右键动作”行为一致。
    const bool singleFileOnly = isSingleSelection && QFileInfo(firstPath).isFile();
    openAction->setEnabled(hasSelection);
    editAction->setEnabled(hasAnyFile);
    copyPathAction->setEnabled(hasSelection);
    copyKernelPathAction->setEnabled(hasSelection);
    copyShortNameAction->setEnabled(hasSelection);
    copyLinkTargetAction->setEnabled(!linkTargetList.isEmpty());
    openLinkTargetAction->setEnabled(!firstLinkTarget.isEmpty());
    locateLinkTargetAction->setEnabled(!firstLinkTarget.isEmpty());
    copyAction->setEnabled(hasSelection);
    cutAction->setEnabled(hasSelection);
    pasteAction->setEnabled(!m_clipboardPaths.empty());
    renameAction->setEnabled(isSingleSelection);
    deleteAction->setEnabled(hasSelection);
    driverDeleteAction->setEnabled(hasSelection);
    unlockByDriverAction->setEnabled(hasSelection);
    takeOwnerAction->setEnabled(hasSelection);
    detailAction->setEnabled(hasSelection);
    hashAction->setEnabled(hasAnyFile);
    signAction->setEnabled(hasAnyFile);
    entropyAction->setEnabled(hasAnyFile);
    hexAction->setEnabled(singleFileOnly);
    peAction->setEnabled(singleFileOnly);
    mappedProcessScanAction->setEnabled(hasAnyFile);

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
        kLogEvent editEvent;
        int successCount = 0;
        QStringList failedPaths;
        for (const QString& path : menuPaths)
        {
            QFileInfo info(path);
            if (!info.isFile())
            {
                continue;
            }
            const bool openOk = QDesktopServices::openUrl(QUrl::fromLocalFile(path));
            if (openOk)
            {
                successCount += 1;
                continue;
            }

            failedPaths.push_back(QDir::toNativeSeparators(path));
        }

        if (!failedPaths.isEmpty())
        {
            warn << editEvent
                << "[FileDock] 编辑（文本）部分失败, panel="
                << panel.panelNameText.toStdString()
                << ", successCount="
                << successCount
                << ", failCount="
                << failedPaths.size()
                << ", failedPreview=\n"
                << buildLogPreviewText(failedPaths).toStdString()
                << eol;
        }
        else
        {
            info << editEvent
                << "[FileDock] 编辑（文本）完成, panel="
                << panel.panelNameText.toStdString()
                << ", successCount="
                << successCount
                << eol;
        }
        return;
    }
    if (selectedAction == copyPathAction)
    {
        copySelectedItemPath(panel);
        return;
    }
    if (selectedAction == copyKernelPathAction)
    {
        copySelectedItemKernelPath(panel);
        return;
    }
    if (selectedAction == copyShortNameAction)
    {
        copySelectedItemShortName(panel);
        return;
    }
    if (selectedAction == copyLinkTargetAction)
    {
        QApplication::clipboard()->setText(linkTargetList.join(QStringLiteral("\n")));
        kLogEvent event;
        info << event
            << "[FileDock] 复制链接目标到剪贴板, panel="
            << panel.panelNameText.toStdString()
            << ", count="
            << linkTargetList.size()
            << eol;
        return;
    }
    if (selectedAction == openLinkTargetAction)
    {
        const bool openOk = QDesktopServices::openUrl(QUrl::fromLocalFile(firstLinkTarget));
        if (!openOk)
        {
            QMessageBox::warning(
                this,
                QStringLiteral("打开链接目标"),
                QStringLiteral("无法打开链接目标：%1").arg(QDir::toNativeSeparators(firstLinkTarget)));
        }
        return;
    }
    if (selectedAction == locateLinkTargetAction)
    {
        const QFileInfo targetInfo(firstLinkTarget);
        const QString locatePath = targetInfo.isDir()
            ? targetInfo.absoluteFilePath()
            : targetInfo.absolutePath();
        if (locatePath.trimmed().isEmpty() || !QDir(locatePath).exists())
        {
            QMessageBox::warning(
                this,
                QStringLiteral("定位链接目标"),
                QStringLiteral("目标所在目录不存在或不可访问：%1").arg(QDir::toNativeSeparators(firstLinkTarget)));
            return;
        }
        navigateToPath(panel, locatePath, true);
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
    if (selectedAction == driverDeleteAction)
    {
        deleteSelectedItemByDriver(panel);
        return;
    }
    if (selectedAction == unlockByDriverAction)
    {
        unlockSelectedItemsByDriver(panel);
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
        const bool startOk = QProcess::startDetached(
            QStringLiteral("cmd.exe"),
            QStringList{ QStringLiteral("/K"), QStringLiteral("cd /d \"%1\"").arg(workPath) });
        kLogEvent terminalEvent;
        if (!startOk)
        {
            warn << terminalEvent
                << "[FileDock] 在终端中打开失败, panel="
                << panel.panelNameText.toStdString()
                << ", workPath="
                << QDir::toNativeSeparators(workPath).toStdString()
                << eol;
        }
        else
        {
            info << terminalEvent
                << "[FileDock] 在终端中打开完成, panel="
                << panel.panelNameText.toStdString()
                << ", workPath="
                << QDir::toNativeSeparators(workPath).toStdString()
                << eol;
        }
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
                kLogEvent detailCancelEvent;
                info << detailCancelEvent
                    << "[FileDock] 批量属性打开取消, panel="
                    << panel.panelNameText.toStdString()
                    << ", selectedCount="
                    << menuPaths.size()
                    << eol;
                return;
            }
            openCount = kMaxAutoOpenDetailCount;
        }

        for (std::size_t i = 0; i < openCount; ++i)
        {
            showFileDetailDialog(menuPaths[i]);
        }

        kLogEvent detailEvent;
        info << detailEvent
            << "[FileDock] 批量属性打开完成, panel="
            << panel.panelNameText.toStdString()
            << ", openedCount="
            << openCount
            << ", selectedCount="
            << menuPaths.size()
            << eol;
        return;
    }
    if (selectedAction == hashAction)
    {
        // 哈希计算支持多选：仅对文件条目计算，目录自动跳过。
        if (!hasAnyFile)
        {
            kLogEvent hashEmptyEvent;
            warn << hashEmptyEvent
                << "[FileDock] 哈希计算取消：未选中文件, panel="
                << panel.panelNameText.toStdString()
                << eol;
            return;
        }

        kLogEvent hashEvent;
        int successCount = 0;
        QStringList failedLines;
        for (const QString& path : menuPaths)
        {
            QFileInfo fileInfo(path);
            if (!fileInfo.isFile())
            {
                continue;
            }

            QFile file(path);
            if (!file.open(QIODevice::ReadOnly))
            {
                failedLines << QStringLiteral("%1 | 无法打开文件。")
                    .arg(QDir::toNativeSeparators(path));
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

            successCount += 1;
            info << hashEvent
                << "[FileDock] 哈希计算结果, filePath="
                << QDir::toNativeSeparators(path).toStdString()
                << ", md5="
                << QString::fromLatin1(md5.result().toHex()).toStdString()
                << ", sha1="
                << QString::fromLatin1(sha1.result().toHex()).toStdString()
                << ", sha256="
                << QString::fromLatin1(sha256.result().toHex()).toStdString()
                << eol;
        }

        if (!failedLines.isEmpty())
        {
            warn << hashEvent
                << "[FileDock] 哈希计算部分失败, panel="
                << panel.panelNameText.toStdString()
                << ", successCount="
                << successCount
                << ", failCount="
                << failedLines.size()
                << ", failedPreview=\n"
                << buildLogPreviewText(failedLines).toStdString()
                << eol;
        }

        info << hashEvent
            << "[FileDock] 哈希计算完成, panel="
            << panel.panelNameText.toStdString()
            << ", successCount="
            << successCount
            << ", failCount="
            << failedLines.size()
            << eol;
        return;
    }
    if (selectedAction == signAction)
    {
        // 数字签名入口与属性页联动，支持多选逐个打开详情。
        if (!hasAnyFile)
        {
            kLogEvent signEmptyEvent;
            warn << signEmptyEvent
                << "[FileDock] 签名检查取消：未选中文件, panel="
                << panel.panelNameText.toStdString()
                << eol;
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

        const std::size_t fileSelectionCount = static_cast<std::size_t>(std::count_if(
            menuPaths.begin(),
            menuPaths.end(),
            [](const QString& path)
            {
                return QFileInfo(path).isFile();
            }));
        kLogEvent signEvent;
        if (openedCount == kMaxAutoOpenSignCount && fileSelectionCount > kMaxAutoOpenSignCount)
        {
            warn << signEvent
                << "[FileDock] 签名检查已截断打开数量, panel="
                << panel.panelNameText.toStdString()
                << ", openedCount="
                << openedCount
                << ", fileSelectionCount="
                << fileSelectionCount
                << eol;
        }

        info << signEvent
            << "[FileDock] 签名检查完成, panel="
            << panel.panelNameText.toStdString()
            << ", openedCount="
            << openedCount
            << ", fileSelectionCount="
            << fileSelectionCount
            << eol;
        return;
    }
    if (selectedAction == entropyAction)
    {
        // 熵值计算支持多选：仅统计文件条目。
        if (!hasAnyFile)
        {
            kLogEvent entropyEmptyEvent;
            warn << entropyEmptyEvent
                << "[FileDock] 熵值计算取消：未选中文件, panel="
                << panel.panelNameText.toStdString()
                << eol;
            return;
        }

        kLogEvent entropyEvent;
        int successCount = 0;
        QStringList failedLines;
        for (const QString& path : menuPaths)
        {
            QFileInfo fileInfo(path);
            if (!fileInfo.isFile())
            {
                continue;
            }

            QFile file(path);
            if (!file.open(QIODevice::ReadOnly))
            {
                failedLines << QStringLiteral("%1 | 无法打开文件。")
                    .arg(QDir::toNativeSeparators(path));
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

            successCount += 1;
            info << entropyEvent
                << "[FileDock] 熵值计算结果, filePath="
                << QDir::toNativeSeparators(path).toStdString()
                << ", entropy="
                << QString::number(entropy, 'f', 4).toStdString()
                << eol;
        }

        if (!failedLines.isEmpty())
        {
            warn << entropyEvent
                << "[FileDock] 熵值计算部分失败, panel="
                << panel.panelNameText.toStdString()
                << ", successCount="
                << successCount
                << ", failCount="
                << failedLines.size()
                << ", failedPreview=\n"
                << buildLogPreviewText(failedLines).toStdString()
                << eol;
        }

        info << entropyEvent
            << "[FileDock] 熵值计算完成, panel="
            << panel.panelNameText.toStdString()
            << ", successCount="
            << successCount
            << ", failCount="
            << failedLines.size()
            << eol;
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
    if (selectedAction == mappedProcessScanAction)
    {
        openMappedProcessScanWindow(menuPaths);
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

    int successCount = 0;
    int failCount = 0;
    QStringList failedPaths;
    for (const QString& path : paths)
    {
        QFileInfo info(path);
        if (info.isDir())
        {
            if (paths.size() == 1)
            {
                navigateToPath(panel, path, true);
                successCount += 1;
            }
            else
            {
                const bool openOk = QDesktopServices::openUrl(QUrl::fromLocalFile(path));
                if (openOk)
                {
                    successCount += 1;
                }
                else
                {
                    failCount += 1;
                    failedPaths.push_back(QDir::toNativeSeparators(path));
                }
            }
            continue;
        }

        const bool openOk = QDesktopServices::openUrl(QUrl::fromLocalFile(path));
        if (openOk)
        {
            successCount += 1;
        }
        else
        {
            failCount += 1;
            failedPaths.push_back(QDir::toNativeSeparators(path));
        }
    }

    kLogEvent resultEvent;
    if (failCount > 0)
    {
        warn << resultEvent
            << "[FileDock] 打开选中项部分失败, panel="
            << panel.panelNameText.toStdString()
            << ", successCount="
            << successCount
            << ", failCount="
            << failCount
            << ", failedPreview=\n"
            << buildLogPreviewText(failedPaths).toStdString()
            << eol;
        return;
    }

    info << resultEvent
        << "[FileDock] 打开选中项完成, panel="
        << panel.panelNameText.toStdString()
        << ", successCount="
        << successCount
        << eol;
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

void FileDock::copySelectedItemKernelPath(FilePanelWidgets& panel)
{
    const std::vector<QString> paths = selectedPaths(panel);
    if (paths.empty())
    {
        return;
    }

    QStringList lines;
    lines.reserve(static_cast<int>(paths.size()));
    for (const QString& path : paths)
    {
        const QString kernelPath = buildDriverNtPath(path);
        lines << (kernelPath.isEmpty() ? QDir::toNativeSeparators(path) : kernelPath);
    }
    QApplication::clipboard()->setText(lines.join('\n'));

    kLogEvent event;
    info << event
        << "[FileDock] 复制内核模式地址到剪贴板, panel="
        << panel.panelNameText.toStdString()
        << ", count="
        << paths.size()
        << eol;
}

void FileDock::copySelectedItemShortName(FilePanelWidgets& panel)
{
    const std::vector<QString> paths = selectedPaths(panel);
    if (paths.empty())
    {
        return;
    }

    QStringList shortNameLines;
    shortNameLines.reserve(static_cast<int>(paths.size()));

    int shortNameHitCount = 0;
    int fallbackCount = 0;
    for (const QString& path : paths)
    {
        const QString shortPathText = queryShortPathText(path);
        QString shortNameText;
        if (!shortPathText.isEmpty())
        {
            shortNameText = QFileInfo(shortPathText).fileName().trimmed();
            if (shortNameText.isEmpty())
            {
                shortNameText = QDir::toNativeSeparators(shortPathText);
            }
            shortNameHitCount += 1;
        }
        else
        {
            shortNameText = QFileInfo(path).fileName().trimmed();
            if (shortNameText.isEmpty())
            {
                shortNameText = QDir::toNativeSeparators(path);
            }
            fallbackCount += 1;
        }

        shortNameLines << shortNameText;
    }

    QApplication::clipboard()->setText(shortNameLines.join('\n'));

    kLogEvent event;
    info << event
        << "[FileDock] 复制短文件名到剪贴板, panel="
        << panel.panelNameText.toStdString()
        << ", count="
        << paths.size()
        << ", shortNameHitCount="
        << shortNameHitCount
        << ", fallbackCount="
        << fallbackCount
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
            << ", errorPreview=\n"
            << buildLogPreviewText(errorLines).toStdString()
            << eol;
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
                if (isPathReparsePoint(path))
                {
                    removeOk = (::RemoveDirectoryW(QDir::toNativeSeparators(path).toStdWString().c_str()) != FALSE);
                }
                else
                {
                    removeOk = QDir(path).removeRecursively();
                }
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
            << ", errorPreview=\n"
            << buildLogPreviewText(errors).toStdString()
            << eol;
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

void FileDock::deleteSelectedItemByDriver(FilePanelWidgets& panel)
{
    const std::vector<QString> paths = selectedPaths(panel);
    if (paths.empty())
    {
        return;
    }

    {
        kLogEvent event;
        info << event
            << "[FileDock] 驱动删除请求, panel="
            << panel.panelNameText.toStdString()
            << ", count="
            << paths.size()
            << eol;
    }

    const QMessageBox::StandardButton userChoice = QMessageBox::question(
        this,
        QStringLiteral("驱动删除确认"),
        QStringLiteral("将通过 KswordARK 驱动直接硬删除选中的 %1 项。\n此操作不会进入回收站，目录会递归删除，是否继续？")
            .arg(paths.size()),
        QMessageBox::Yes | QMessageBox::No,
        QMessageBox::No);
    if (userChoice != QMessageBox::Yes)
    {
        return;
    }

    std::vector<DriverDeleteTarget> deleteTargets;
    QStringList prepareErrors;
    for (const QString& path : paths)
    {
        QString errorText;
        if (!appendDriverDeleteTargetsPostOrder(path, deleteTargets, errorText))
        {
            prepareErrors.push_back(errorText);
        }
    }

    if (deleteTargets.empty())
    {
        kLogEvent event;
        warn << event
            << "[FileDock] 驱动删除取消：未生成可删除目标, panel="
            << panel.panelNameText.toStdString()
            << ", prepareErrorPreview=\n"
            << buildLogPreviewText(prepareErrors).toStdString()
            << eol;
        return;
    }

    std::string openDriverDetailText;
    ksword::ark::DriverHandle driverHandle = openKswordArkDriverHandle(&openDriverDetailText);
    if (!driverHandle.isValid())
    {
        kLogEvent event;
        warn << event
            << "[FileDock] 驱动删除失败：无法连接驱动, panel="
            << panel.panelNameText.toStdString()
            << ", detail="
            << openDriverDetailText
            << eol;
        QMessageBox::warning(
            this,
            QStringLiteral("驱动删除"),
            QStringLiteral("无法连接 KswordARK 驱动设备，请先启用 R0 驱动。"));
        return;
    }

    const int progressPid = kPro.add("文件", "驱动删除");
    kPro.set(progressPid, "驱动删除开始", 0, 5.0f);

    QStringList deleteErrors = prepareErrors;
    const std::size_t totalTargetCount = deleteTargets.size();
    for (std::size_t index = 0; index < totalTargetCount; ++index)
    {
        const DriverDeleteTarget& target = deleteTargets[index];
        std::string detailText;
        bool deleteOk = deletePathByR0Driver(
            driverHandle,
            target.path,
            target.isDirectory,
            &detailText);
        if (!deleteOk && !target.isDirectory)
        {
            // 驱动删除失败后仅扫描占用进程用于提示，禁止绕过文件解锁器的显式选择流程。
            QStringList fallbackDetails;
            const std::vector<std::uint32_t> occupyPids =
                collectOccupyProcessIdsByPath(target.path, &fallbackDetails);

            QStringList errorLines;
            errorLines.push_back(QString::fromStdString(detailText));
            errorLines.push_back(
                QStringLiteral("occupyPidCount=%1, autoTerminate=disabled")
                .arg(occupyPids.size()));
            errorLines.push_back(QStringLiteral("请先使用“文件解锁器”选择并确认要结束的占用进程，再重新执行驱动删除。"));
            errorLines.append(fallbackDetails);

            deleteErrors.push_back(errorLines.join(QStringLiteral(" | ")));
        }
        else if (!deleteOk)
        {
            deleteErrors.push_back(QString::fromStdString(detailText));
        }

        const float progress =
            5.0f + (static_cast<float>(index + 1) / static_cast<float>(totalTargetCount)) * 90.0f;
        kPro.set(progressPid, "驱动删除处理中", 0, progress);
    }

    driverHandle.reset();

    refreshPanel(panel);
    kPro.set(progressPid, "驱动删除完成", 0, 100.0f);

    if (!deleteErrors.isEmpty())
    {
        kLogEvent event;
        warn << event
            << "[FileDock] 驱动删除部分失败, panel="
            << panel.panelNameText.toStdString()
            << ", errorCount="
            << deleteErrors.size()
            << ", errorPreview=\n"
            << buildLogPreviewText(deleteErrors).toStdString()
            << eol;
        return;
    }

    kLogEvent event;
    info << event
        << "[FileDock] 驱动删除完成, panel="
        << panel.panelNameText.toStdString()
        << ", originalCount="
        << paths.size()
        << ", targetCount="
        << deleteTargets.size()
        << eol;
}

void FileDock::unlockSelectedItemsByDriver(FilePanelWidgets& panel)
{
    const std::vector<QString> paths = selectedPaths(panel);
    if (paths.empty())
    {
        return;
    }

    unlockPathsByDriver(paths, QStringLiteral("panel_context_menu"), &panel);
}

void FileDock::unlockPathsByDriver(
    const std::vector<QString>& targetPaths,
    const QString& triggerTag,
    FilePanelWidgets* panelForRefresh)
{
    std::vector<QString> paths;
    paths.reserve(targetPaths.size());
    for (const QString& path : targetPaths)
    {
        const QString normalizedPath = QDir::toNativeSeparators(path.trimmed());
        if (!normalizedPath.isEmpty())
        {
            paths.push_back(normalizedPath);
        }
    }
    if (paths.empty())
    {
        return;
    }

    enum class RefreshTarget
    {
        Both = 0,
        Left,
        Right
    };
    const RefreshTarget refreshTarget =
        (panelForRefresh == &m_leftPanel) ? RefreshTarget::Left :
        (panelForRefresh == &m_rightPanel) ? RefreshTarget::Right :
        RefreshTarget::Both;

    QWidget* const dialogParent = resolveVisibleDialogParent(this);
    const QMessageBox::StandardButton scanChoice = QMessageBox::question(
        dialogParent,
        QStringLiteral("文件解锁器扫描确认"),
        QStringLiteral("将扫描选中路径的占用来源。扫描完成后可选择关闭句柄，或改用 R3/R0 结束进程兜底。\n是否开始扫描？"),
        QMessageBox::Yes | QMessageBox::No,
        QMessageBox::No);
    if (scanChoice != QMessageBox::Yes)
    {
        return;
    }

    struct UnlockJobResult
    {
        bool scanCompleted = false;
        QStringList scanDetailList;
        std::vector<UnlockProcessCandidate> processCandidateList;
        std::vector<UnlockHandleCandidate> handleCandidateList;
        UnlockOperationMode operationMode = UnlockOperationMode::CloseHandleR3;
        std::vector<std::uint32_t> selectedProcessIdList;
        std::vector<UnlockHandleCandidate> selectedHandleList;
        std::size_t closeHandleSuccessCount = 0U;
        std::size_t terminateSuccessCount = 0U;
        QStringList operationFailList;
        QStringList skippedTargetList;
        QString driverErrorText;
    };

    {
        std::lock_guard<std::mutex> lock(m_unlockerWorkerMutex);
        if (m_unlockerWorkerRunning.load())
        {
            QMessageBox::information(dialogParent, QStringLiteral("文件解锁器"), QStringLiteral("已有解锁任务正在执行，请稍候。"));
            return;
        }
        if (m_unlockerWorkerThread.joinable())
        {
            m_unlockerWorkerThread.join();
        }
        m_unlockerWorkerStopRequested.store(false);
        m_unlockerWorkerRunning.store(true);
    }

    const int progressPid = kPro.add("文件", "文件解锁器");
    kPro.set(progressPid, "准备扫描占用来源", 0, 5.0f);

    QPointer<FileDock> safeThis(this);
    {
        std::lock_guard<std::mutex> lock(m_unlockerWorkerMutex);
        m_unlockerWorkerThread = std::thread([safeThis, paths, triggerTag, refreshTarget, progressPid, this]() {
        UnlockJobResult jobResult;
        const auto markWorkerStopped = [this]() {
            this->m_unlockerWorkerRunning.store(false);
            };

        kPro.set(progressPid, "扫描占用来源", 0, 35.0f);
        const filedock::handleusage::HandleUsageScanResult scanResult =
            filedock::handleusage::scanHandleUsageByPaths(paths, progressPid);
        jobResult.scanCompleted = true;
        jobResult.scanDetailList.push_back(
            QStringLiteral("matched=%1, elapsedMs=%2, diagnostic=%3")
            .arg(scanResult.matchedHandleCount)
            .arg(scanResult.elapsedMs)
            .arg(scanResult.diagnosticText.trimmed().isEmpty()
                ? QStringLiteral("-")
                : scanResult.diagnosticText.simplified()));

        std::map<std::uint32_t, UnlockProcessCandidate> candidateByPid;
        const std::uint32_t currentProcessId = static_cast<std::uint32_t>(::GetCurrentProcessId());
        for (const filedock::handleusage::HandleUsageEntry& entry : scanResult.entries)
        {
            if (entry.processId == 0U)
            {
                continue;
            }

            UnlockProcessCandidate& processCandidate = candidateByPid[entry.processId];
            processCandidate.processId = entry.processId;
            if (processCandidate.processName.isEmpty() && !entry.processName.trimmed().isEmpty())
            {
                processCandidate.processName = entry.processName.trimmed();
            }
            if (processCandidate.processImagePath.isEmpty() && !entry.processImagePath.trimmed().isEmpty())
            {
                processCandidate.processImagePath = entry.processImagePath.trimmed();
            }
            appendUniqueText(processCandidate.matchedTargetList, entry.matchedTargetPath);
            appendUniqueText(processCandidate.matchRuleList, entry.matchRuleText);
            processCandidate.matchCount += 1U;
            processCandidate.isCurrentProcess = entry.processId == currentProcessId;
            processCandidate.isCriticalProcess = entry.processId <= 4U || isCriticalProcessName(processCandidate.processName);

            UnlockHandleCandidate handleCandidate{};
            handleCandidate.processId = entry.processId;
            handleCandidate.processName = entry.processName.trimmed();
            handleCandidate.processImagePath = entry.processImagePath.trimmed();
            handleCandidate.handleValue = entry.handleValue;
            handleCandidate.grantedAccess = entry.grantedAccess;
            handleCandidate.matchedTargetPath = entry.matchedTargetPath;
            handleCandidate.matchRuleText = entry.matchRuleText;
            handleCandidate.objectName = entry.objectName;
            handleCandidate.enumerationSource = entry.enumerationSource;
            handleCandidate.isCurrentProcess = processCandidate.isCurrentProcess;
            handleCandidate.isCriticalProcess = processCandidate.isCriticalProcess;
            jobResult.handleCandidateList.push_back(handleCandidate);
        }

        jobResult.processCandidateList.reserve(candidateByPid.size());
        for (const auto& entry : candidateByPid)
        {
            jobResult.processCandidateList.push_back(entry.second);
        }

        if (safeThis.isNull())
        {
            kPro.set(progressPid, "界面已关闭", 0, 100.0f);
            markWorkerStopped();
            return;
        }

        UnlockSelectionResult selectionResult;
        const std::shared_ptr<UnlockSelectionSharedState> selectionState =
            std::make_shared<UnlockSelectionSharedState>();
        const bool selectionInvokeOk = QMetaObject::invokeMethod(
            safeThis.data(),
            [selectionState, safeThis, progressPid, jobResult]() {
                QWidget* const unlockerDialogParent = resolveVisibleDialogParent(safeThis.data());
                UnlockSelectionResult uiSelectionResult;
                if (safeThis.isNull())
                {
                    kPro.set(progressPid, "界面已关闭", 0, 100.0f);
                }
                else if (jobResult.processCandidateList.empty() && jobResult.handleCandidateList.empty())
                {
                    QMessageBox::information(
                        unlockerDialogParent,
                        QStringLiteral("文件解锁器"),
                        QStringLiteral("未发现占用来源，无需解锁。"));
                    kPro.set(progressPid, "未发现占用来源", 0, 100.0f);
                }
                else
                {
                    uiSelectionResult = showUnlockSelectionDialog(
                        unlockerDialogParent,
                        jobResult.processCandidateList,
                        jobResult.handleCandidateList);
                }

                {
                    std::lock_guard<std::mutex> lock(selectionState->mutex);
                    selectionState->result = uiSelectionResult;
                    selectionState->completed = true;
                }

                selectionState->condition.notify_all();
            },
            Qt::QueuedConnection);
        if (!selectionInvokeOk)
        {
            kPro.set(progressPid, "回调失败", 0, 100.0f);
            markWorkerStopped();
            return;
        }

        {
            std::unique_lock<std::mutex> lock(selectionState->mutex);
            while (!selectionState->completed)
            {
                if (safeThis.isNull() || this->m_unlockerWorkerStopRequested.load())
                {
                    kPro.set(progressPid, "界面已关闭", 0, 100.0f);
                    markWorkerStopped();
                    return;
                }

                selectionState->condition.wait_for(lock, std::chrono::milliseconds(100));
            }

            selectionResult = selectionState->result;
        }

        const bool noSelectedHandle = selectionResult.selectedHandleList.empty();
        const bool noSelectedProcess = selectionResult.selectedProcessIdList.empty();
        if (!selectionResult.accepted
            || (selectionResult.operationMode == UnlockOperationMode::CloseHandleR3 && noSelectedHandle)
            || (selectionResult.operationMode != UnlockOperationMode::CloseHandleR3 && noSelectedProcess))
        {
            kPro.set(progressPid, "用户取消", 0, 100.0f);
            markWorkerStopped();
            return;
        }

        jobResult.operationMode = selectionResult.operationMode;
        jobResult.selectedHandleList = selectionResult.selectedHandleList;
        jobResult.selectedProcessIdList = selectionResult.selectedProcessIdList;
        kPro.set(progressPid, unlockOperationModeToText(jobResult.operationMode).toStdString(), 0, 55.0f);

        if (jobResult.operationMode == UnlockOperationMode::CloseHandleR3)
        {
            const std::size_t totalHandleCount = jobResult.selectedHandleList.size();
            for (std::size_t index = 0; index < totalHandleCount; ++index)
            {
                if (safeThis.isNull() || this->m_unlockerWorkerStopRequested.load())
                {
                    break;
                }

                const UnlockHandleCandidate& handleCandidate = jobResult.selectedHandleList[index];
                if (handleCandidate.processId <= 4U
                    || handleCandidate.handleValue == 0U
                    || handleCandidate.isCurrentProcess
                    || handleCandidate.isCriticalProcess)
                {
                    jobResult.skippedTargetList.push_back(
                        QStringLiteral("pid=%1 | handle=%2 | %3 | 已保护或无效，未关闭")
                        .arg(handleCandidate.processId)
                        .arg(formatHandleValueText(handleCandidate.handleValue))
                        .arg(handleCandidate.processName.isEmpty() ? QStringLiteral("Unknown") : handleCandidate.processName));
                    continue;
                }

                std::string detailText;
                const bool closeOk = ks::file::CloseRemoteHandle(
                    handleCandidate.processId,
                    handleCandidate.handleValue,
                    detailText);
                if (closeOk)
                {
                    jobResult.closeHandleSuccessCount += 1U;
                }
                else
                {
                    jobResult.operationFailList.push_back(
                        QStringLiteral("pid=%1 | handle=%2 | %3 | %4")
                        .arg(handleCandidate.processId)
                        .arg(formatHandleValueText(handleCandidate.handleValue))
                        .arg(handleCandidate.processName.isEmpty() ? QStringLiteral("Unknown") : handleCandidate.processName)
                        .arg(QString::fromStdString(detailText)));
                }

                const float progress =
                    55.0f + (static_cast<float>(index + 1) / static_cast<float>(totalHandleCount)) * 40.0f;
                kPro.set(progressPid, "关闭选中句柄", 0, progress);
            }
        }
        else
        {
            ksword::ark::DriverHandle driverHandle;
            if (jobResult.operationMode == UnlockOperationMode::TerminateProcessR0)
            {
                std::string openDriverDetailText;
                driverHandle = openKswordArkDriverHandle(&openDriverDetailText);
                if (!driverHandle.isValid())
                {
                    jobResult.driverErrorText = QString::fromStdString(openDriverDetailText);
                }
            }

            std::map<std::uint32_t, UnlockProcessCandidate> candidateBySelectedPid;
            for (const UnlockProcessCandidate& candidate : jobResult.processCandidateList)
            {
                candidateBySelectedPid[candidate.processId] = candidate;
            }

            if (jobResult.operationMode == UnlockOperationMode::TerminateProcessR0
                && !driverHandle.isValid())
            {
                jobResult.operationFailList.push_back(
                    QStringLiteral("R0 驱动连接失败：%1")
                    .arg(jobResult.driverErrorText));
            }
            else
            {
                const std::size_t totalProcessCount = jobResult.selectedProcessIdList.size();
                for (std::size_t index = 0; index < totalProcessCount; ++index)
                {
                    if (safeThis.isNull() || this->m_unlockerWorkerStopRequested.load())
                    {
                        break;
                    }

                    const std::uint32_t processId = jobResult.selectedProcessIdList[index];
                    const auto candidateIter = candidateBySelectedPid.find(processId);
                    const QString processName = (candidateIter != candidateBySelectedPid.end())
                        ? candidateIter->second.processName
                        : QString();
                    const bool protectedProcess = candidateIter != candidateBySelectedPid.end()
                        && (candidateIter->second.isCurrentProcess || candidateIter->second.isCriticalProcess);
                    if (processId <= 4U || processId == static_cast<std::uint32_t>(::GetCurrentProcessId()) || protectedProcess)
                    {
                        jobResult.skippedTargetList.push_back(
                            QStringLiteral("pid=%1 | %2 | 已保护，未结束")
                            .arg(processId)
                            .arg(processName.isEmpty() ? QStringLiteral("Unknown") : processName));
                        continue;
                    }

                    std::string detailText;
                    const bool terminateOk = (jobResult.operationMode == UnlockOperationMode::TerminateProcessR0)
                        ? terminateProcessByR0Driver(driverHandle, processId, &detailText)
                        : terminateProcessByR3(processId, &detailText);
                    if (terminateOk)
                    {
                        jobResult.terminateSuccessCount += 1U;
                    }
                    else
                    {
                        jobResult.operationFailList.push_back(
                            QStringLiteral("pid=%1 | %2 | %3")
                            .arg(processId)
                            .arg(processName.isEmpty() ? QStringLiteral("Unknown") : processName)
                            .arg(QString::fromStdString(detailText)));
                    }

                    const float progress =
                        55.0f + (static_cast<float>(index + 1) / static_cast<float>(totalProcessCount)) * 40.0f;
                    kPro.set(progressPid, "结束选中进程", 0, progress);
                }
            }

            driverHandle.reset();
        }

        if (safeThis.isNull())
        {
            kPro.set(progressPid, "界面已关闭", 0, 100.0f);
            markWorkerStopped();
            return;
        }

        const bool finishInvokeOk = QMetaObject::invokeMethod(
            safeThis.data(),
            [safeThis, triggerTag, refreshTarget, progressPid, jobResult, paths, markWorkerStopped]() {
                if (safeThis.isNull())
                {
                    kPro.set(progressPid, "界面已关闭", 0, 100.0f);
                    return;
                }
                QWidget* const unlockerDialogParent = resolveVisibleDialogParent(safeThis.data());

                if (refreshTarget == RefreshTarget::Left)
                {
                    safeThis->refreshPanel(safeThis->m_leftPanel);
                }
                else if (refreshTarget == RefreshTarget::Right)
                {
                    safeThis->refreshPanel(safeThis->m_rightPanel);
                }
                else
                {
                    safeThis->refreshPanel(safeThis->m_leftPanel);
                    safeThis->refreshPanel(safeThis->m_rightPanel);
                }

                const QString modeText = unlockOperationModeToText(jobResult.operationMode);
                const std::size_t selectedCount = (jobResult.operationMode == UnlockOperationMode::CloseHandleR3)
                    ? jobResult.selectedHandleList.size()
                    : jobResult.selectedProcessIdList.size();
                const std::size_t successCount = (jobResult.operationMode == UnlockOperationMode::CloseHandleR3)
                    ? jobResult.closeHandleSuccessCount
                    : jobResult.terminateSuccessCount;
                const QString summaryText = QStringLiteral("操作方式：%1\n扫描到占用进程：%2\n扫描到句柄记录：%3\n选中目标：%4\n成功处理：%5\n失败/跳过：%6")
                    .arg(modeText)
                    .arg(jobResult.processCandidateList.size())
                    .arg(jobResult.handleCandidateList.size())
                    .arg(selectedCount)
                    .arg(successCount)
                    .arg(jobResult.operationFailList.size() + jobResult.skippedTargetList.size());
                if (jobResult.operationFailList.isEmpty() && jobResult.skippedTargetList.isEmpty())
                {
                    QMessageBox::information(
                        unlockerDialogParent,
                        QStringLiteral("文件解锁器"),
                        summaryText);
                }
                else
                {
                    QMessageBox::warning(
                        unlockerDialogParent,
                        QStringLiteral("文件解锁器"),
                        summaryText + QStringLiteral("\n\n明细（节选）：\n%1")
                        .arg(buildLogPreviewText(jobResult.operationFailList + jobResult.skippedTargetList, 8)));
                }

                kLogEvent event;
                if (!jobResult.operationFailList.isEmpty() || !jobResult.skippedTargetList.isEmpty())
                {
                    warn << event
                        << "[FileDock] 文件解锁器部分失败, panel="
                        << triggerTag.toStdString()
                        << ", mode="
                        << modeText.toStdString()
                        << ", targetCount="
                        << paths.size()
                        << ", occupyProcessCount="
                        << jobResult.processCandidateList.size()
                        << ", handleRecordCount="
                        << jobResult.handleCandidateList.size()
                        << ", selectedCount="
                        << selectedCount
                        << ", successCount="
                        << successCount
                        << ", failCount="
                        << (jobResult.operationFailList.size() + jobResult.skippedTargetList.size())
                        << ", scanPreview=\n"
                        << buildLogPreviewText(jobResult.scanDetailList).toStdString()
                        << ", failPreview=\n"
                        << buildLogPreviewText(jobResult.operationFailList + jobResult.skippedTargetList).toStdString()
                        << eol;
                }
                else
                {
                    info << event
                        << "[FileDock] 文件解锁器完成, panel="
                        << triggerTag.toStdString()
                        << ", mode="
                        << modeText.toStdString()
                        << ", targetCount="
                        << paths.size()
                        << ", occupyProcessCount="
                        << jobResult.processCandidateList.size()
                        << ", handleRecordCount="
                        << jobResult.handleCandidateList.size()
                        << ", selectedCount="
                        << selectedCount
                        << ", successCount="
                        << successCount
                        << eol;
                }

                kPro.set(progressPid, "文件解锁器完成", 0, 100.0f);
                markWorkerStopped();
            },
            Qt::QueuedConnection);
        if (!finishInvokeOk)
        {
            kPro.set(progressPid, "回调失败", 0, 100.0f);
            markWorkerStopped();
        }
        });
    }
}

void FileDock::unlockFileByPath(const QString& targetPath)
{
    const QString normalizedPath = QDir::toNativeSeparators(targetPath.trimmed());
    if (normalizedPath.isEmpty())
    {
        return;
    }

    unlockPathsByDriver(std::vector<QString>{ normalizedPath }, QStringLiteral("system_context_menu"), nullptr);
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
            << ", detailPreview=\n"
            << buildLogPreviewText(errorDetails).toStdString()
            << eol;
        return;
    }

    kLogEvent finishEvent;
    info << finishEvent
        << "[FileDock] 取得所有权完成, panel="
        << panel.panelNameText.toStdString()
        << ", successCount="
        << paths.size()
        << eol;
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
        kLogEvent event;
        warn << event
            << "[FileDock] 打开文件详情失败：目标不存在, filePath="
            << QDir::toNativeSeparators(filePath).toStdString()
            << eol;
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

void FileDock::openFileDetailByPath(const QString& filePath)
{
    showFileDetailDialog(filePath);
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
