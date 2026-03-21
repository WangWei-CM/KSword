#include "OtherDock.h"

// ============================================================
// OtherDock.cpp
// 作用说明：
// 1) 实现窗口列表标签页、过滤分组、右键操作和导出；
// 2) 实现窗口详细信息对话框，覆盖常规/样式/位置/状态等标签；
// 3) 所有枚举流程都走后台线程，避免阻塞 UI 线程。
// ============================================================

#include "../ProcessDock/ProcessDetailWindow.h"
#include "../theme.h"

#include <QApplication>
#include <QCheckBox>
#include <QClipboard>
#include <QComboBox>
#include <QDateTime>
#include <QDialog>
#include <QFile>
#include <QFileDialog>
#include <QFileIconProvider>
#include <QFileInfo>
#include <QFormLayout>
#include <QGuiApplication>
#include <QGroupBox>
#include <QHeaderView>
#include <QHBoxLayout>
#include <QIcon>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QMessageBox>
#include <QMetaObject>
#include <QPainter>
#include <QPixmap>
#include <QPlainTextEdit>
#include <QPointer>
#include <QProcess>
#include <QPushButton>
#include <QScreen>
#include <QSlider>
#include <QSpinBox>
#include <QSplitter>
#include <QStatusBar>
#include <QTabWidget>
#include <QTextStream>
#include <QTimer>
#include <QTimeZone>
#include <QToolButton>
#include <QTreeWidget>
#include <QTreeWidgetItemIterator>
#include <QVBoxLayout>
#include <QStringList>

#include <algorithm>
#include <array>
#include <cstdint>
#include <map>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <utility>

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#include <Psapi.h>
#include <TlHelp32.h>

namespace
{
    // 统一按钮样式：与全局蓝色主题保持一致，避免界面风格割裂。
    QString blueButtonStyle()
    {
        return QStringLiteral(
            "QPushButton,QToolButton{"
            "  color:%1;"
            "  background:#FFFFFF;"
            "  border:1px solid %2;"
            "  border-radius:3px;"
            "  padding:3px 8px;"
            "}"
            "QPushButton:hover,QToolButton:hover{background:%3;}"
            "QPushButton:pressed,QToolButton:pressed{background:%4;color:#FFFFFF;}")
            .arg(KswordTheme::PrimaryBlueHex)
            .arg(KswordTheme::PrimaryBlueBorderHex)
            .arg(KswordTheme::PrimaryBlueHoverHex)
            .arg(KswordTheme::PrimaryBluePressedHex);
    }

    // 统一输入框样式：过滤框、下拉框、数值输入用同一套视觉反馈。
    QString blueInputStyle()
    {
        return QStringLiteral(
            "QLineEdit,QComboBox,QSpinBox{"
            "  border:1px solid #C8DDF4;"
            "  border-radius:3px;"
            "  background:#FFFFFF;"
            "  padding:2px 6px;"
            "}"
            "QLineEdit:focus,QComboBox:focus,QSpinBox:focus{border:1px solid %1;}")
            .arg(KswordTheme::PrimaryBlueHex);
    }

    // 表头样式：突出列头，方便快速分辨字段。
    QString blueHeaderStyle()
    {
        return QStringLiteral("QHeaderView::section{color:%1;font-weight:600;}")
            .arg(KswordTheme::PrimaryBlueHex);
    }

    // 转换布尔文本：统一“是/否”显示，避免各处写法不一致。
    QString boolText(const bool value)
    {
        return value ? QStringLiteral("是") : QStringLiteral("否");
    }

    // 把 HWND 数值转十六进制文本，用于表格首列与详情标题。
    QString hwndToText(const quint64 hwndValue)
    {
        return QStringLiteral("0x%1")
            .arg(static_cast<qulonglong>(hwndValue), 0, 16)
            .toUpper();
    }

    // 列索引常量：
    // - 统一定义窗口表格列顺序，避免各函数散落魔法数字；
    // - 本次调整把“窗口标题”置于第一列，便于快速浏览。
    constexpr int kWindowColumnTitle = 0;
    constexpr int kWindowColumnHandle = 1;
    constexpr int kWindowColumnEnumApi = 2;
    constexpr int kWindowColumnClassName = 3;
    constexpr int kWindowColumnPid = 4;
    constexpr int kWindowColumnProcessName = 5;
    constexpr int kWindowColumnTid = 6;
    constexpr int kWindowColumnSize = 7;
    constexpr int kWindowColumnVisible = 8;
    constexpr int kWindowColumnEnabled = 9;
    constexpr int kWindowColumnTopMost = 10;
    constexpr int kWindowColumnState = 11;
    constexpr int kWindowColumnAlpha = 12;

    // queryProcessImagePathByPid：
    // - 通过 PID 查询可执行文件完整路径；
    // - 成功返回完整路径，失败返回空字符串。
    QString queryProcessImagePathByPid(const std::uint32_t pid)
    {
        if (pid == 0)
        {
            return QString();
        }

        HANDLE processHandle = ::OpenProcess(
            PROCESS_QUERY_LIMITED_INFORMATION | PROCESS_VM_READ,
            FALSE,
            pid);
        if (processHandle == nullptr)
        {
            processHandle = ::OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
        }
        if (processHandle == nullptr)
        {
            return QString();
        }

        wchar_t pathBuffer[MAX_PATH * 2] = {};
        DWORD pathLength = static_cast<DWORD>(std::size(pathBuffer));
        QString fullPath;
        if (::QueryFullProcessImageNameW(processHandle, 0, pathBuffer, &pathLength) != FALSE)
        {
            fullPath = QString::fromWCharArray(pathBuffer, static_cast<int>(pathLength));
        }

        ::CloseHandle(processHandle);
        return fullPath;
    }

    // 进程名查询：通过 PID 获取可执行文件名，并可选输出完整路径。
    QString queryProcessNameByPid(const std::uint32_t pid, QString* imagePathOut)
    {
        if (imagePathOut != nullptr)
        {
            imagePathOut->clear();
        }
        if (pid == 0)
        {
            return QStringLiteral("System");
        }

        const QString fullPath = queryProcessImagePathByPid(pid);
        if (fullPath.trimmed().isEmpty())
        {
            return QStringLiteral("PID_%1").arg(pid);
        }
        if (imagePathOut != nullptr)
        {
            *imagePathOut = fullPath;
        }

        const int slashPos = std::max(fullPath.lastIndexOf('/'), fullPath.lastIndexOf('\\'));
        return slashPos >= 0 ? fullPath.mid(slashPos + 1) : fullPath;
    }

    // queryProcessLogoIconByPath：
    // - 根据进程路径获取系统文件图标；
    // - 若路径无效或无法提取图标，则回退默认进程图标。
    QIcon queryProcessLogoIconByPath(const QString& imagePathText)
    {
        static QFileIconProvider iconProvider;
        if (!imagePathText.trimmed().isEmpty())
        {
            const QIcon fileIcon = iconProvider.icon(QFileInfo(imagePathText));
            if (!fileIcon.isNull())
            {
                return fileIcon;
            }
        }
        return QIcon(":/Icon/process_main.svg");
    }

    // 快照状态文本：把最小化/最大化/正常映射到统一字符串。
    QString windowStateText(
        const bool validState,
        const bool minimizedState,
        const bool maximizedState)
    {
        if (!validState)
        {
            return QStringLiteral("无效");
        }
        if (minimizedState)
        {
            return QStringLiteral("最小化");
        }
        if (maximizedState)
        {
            return QStringLiteral("最大化");
        }
        return QStringLiteral("正常");
    }

    // 过滤模式文本：用于日志中输出当前筛选语义，便于排查“看不到窗口”的问题。
    QString filterModeText(const int mode)
    {
        switch (mode)
        {
        case 0:
            return QStringLiteral("可见窗口");
        case 1:
            return QStringLiteral("隐藏窗口");
        case 2:
            return QStringLiteral("顶层窗口");
        case 3:
            return QStringLiteral("子窗口");
        case 4:
            return QStringLiteral("无效窗口");
        default:
            return QStringLiteral("未知模式");
        }
    }

    // 分组模式文本：统一转换为可读字符串，避免日志里仅出现数字索引。
    QString groupModeText(const int mode)
    {
        switch (mode)
        {
        case 0:
            return QStringLiteral("按进程分组");
        case 1:
            return QStringLiteral("按Z序");
        case 2:
            return QStringLiteral("按层级");
        case 3:
            return QStringLiteral("按显示器");
        default:
            return QStringLiteral("未知分组");
        }
    }

    // 视图模式文本：用于记录图标/列表/详情切换。
    QString viewModeText(const int mode)
    {
        switch (mode)
        {
        case 0:
            return QStringLiteral("图标视图");
        case 1:
            return QStringLiteral("列表视图");
        case 2:
            return QStringLiteral("详情视图");
        default:
            return QStringLiteral("未知视图");
        }
    }

    // 句柄字符串转 HWND：在详情对话框应用改动时需要把缓存句柄还原。
    HWND toHwnd(const quint64 hwndValue)
    {
        return reinterpret_cast<HWND>(static_cast<quintptr>(hwndValue));
    }

    // 枚举窗口属性上下文：用于 EnumPropsExW 回调收集属性名和值。
    struct WindowPropEnumContext
    {
        QStringList* lines = nullptr;   // 输出文本行集合。
        int maxCount = 0;               // 允许收集的最大行数。
    };

    // EnumPropsExW 回调：把每个属性键和值写入 lines，超限后提前终止。
    int CALLBACK enumWindowPropProc(HWND, LPWSTR stringPtr, HANDLE dataHandle, ULONG_PTR lParam)
    {
        WindowPropEnumContext* context = reinterpret_cast<WindowPropEnumContext*>(lParam);
        if (context == nullptr || context->lines == nullptr)
        {
            return FALSE;
        }
        if (context->lines->size() >= context->maxCount)
        {
            return FALSE;
        }

        QString keyText;
        if (stringPtr != nullptr)
        {
            const quintptr rawValue = reinterpret_cast<quintptr>(stringPtr);
            if ((rawValue >> 16) == 0)
            {
                keyText = QStringLiteral("ATOM_%1").arg(static_cast<unsigned int>(rawValue & 0xFFFFu));
            }
            else
            {
                keyText = QString::fromWCharArray(stringPtr);
            }
        }
        else
        {
            keyText = QStringLiteral("<null>");
        }

        context->lines->push_back(
            QStringLiteral("%1 = 0x%2")
            .arg(keyText)
            .arg(static_cast<qulonglong>(reinterpret_cast<quintptr>(dataHandle)), 0, 16));
        return TRUE;
    }

    // fileTimeToUInt64：把 FILETIME 转为 100ns 计数，便于做时间差计算。
    qulonglong fileTimeToUInt64(const FILETIME& fileTimeValue)
    {
        ULARGE_INTEGER fileTimeInteger{};
        fileTimeInteger.LowPart = fileTimeValue.dwLowDateTime;
        fileTimeInteger.HighPart = fileTimeValue.dwHighDateTime;
        return static_cast<qulonglong>(fileTimeInteger.QuadPart);
    }

    // fileTimeToLocalText：把 Windows FILETIME 格式化为本地时间字符串。
    QString fileTimeToLocalText(const FILETIME& fileTimeValue)
    {
        constexpr qulonglong kUnixEpochOffset100ns = 116444736000000000ULL;
        const qulonglong fileTimeTick = fileTimeToUInt64(fileTimeValue);
        if (fileTimeTick <= kUnixEpochOffset100ns)
        {
            return QStringLiteral("<N/A>");
        }

        const qint64 utcMs = static_cast<qint64>((fileTimeTick - kUnixEpochOffset100ns) / 10000ULL);
        return QDateTime::fromMSecsSinceEpoch(utcMs, QTimeZone::UTC).toLocalTime().toString(QStringLiteral("yyyy-MM-dd HH:mm:ss.zzz"));
    }

    // formatDurationFrom100ns：把 100ns 时长格式化为“小时:分钟:秒.毫秒”。
    QString formatDurationFrom100ns(const qulonglong duration100ns)
    {
        const qulonglong totalMs = duration100ns / 10000ULL;
        const qulonglong totalSeconds = totalMs / 1000ULL;
        const qulonglong remainMs = totalMs % 1000ULL;
        const qulonglong hours = totalSeconds / 3600ULL;
        const qulonglong minutes = (totalSeconds % 3600ULL) / 60ULL;
        const qulonglong seconds = totalSeconds % 60ULL;

        return QStringLiteral("%1h %2m %3s %4ms")
            .arg(hours)
            .arg(minutes)
            .arg(seconds)
            .arg(remainMs);
    }

    // buildModuleSummaryText：按 PID 枚举模块并生成摘要文本，避免在 UI 中展示过长列表。
    QString buildModuleSummaryText(const std::uint32_t processId, const int maxRenderCount, int* moduleCountOut)
    {
        if (moduleCountOut != nullptr)
        {
            *moduleCountOut = 0;
        }
        if (processId == 0)
        {
            return QStringLiteral("<System PID>");
        }

        HANDLE snapshotHandle = ::CreateToolhelp32Snapshot(TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32, processId);
        if (snapshotHandle == INVALID_HANDLE_VALUE)
        {
            return QStringLiteral("<模块枚举失败或无权限>");
        }

        MODULEENTRY32W moduleEntry{};
        moduleEntry.dwSize = sizeof(moduleEntry);
        QStringList lineList;
        int moduleCount = 0;

        if (::Module32FirstW(snapshotHandle, &moduleEntry) != FALSE)
        {
            do
            {
                ++moduleCount;
                if (moduleCount <= maxRenderCount)
                {
                    lineList << QStringLiteral("[%1] %2 | Base=0x%3 | Size=%4")
                        .arg(moduleCount)
                        .arg(QString::fromWCharArray(moduleEntry.szModule))
                        .arg(static_cast<qulonglong>(reinterpret_cast<quintptr>(moduleEntry.modBaseAddr)), 0, 16)
                        .arg(moduleEntry.modBaseSize);
                }

                moduleEntry.dwSize = sizeof(moduleEntry);
            } while (::Module32NextW(snapshotHandle, &moduleEntry) != FALSE);
        }

        ::CloseHandle(snapshotHandle);

        if (moduleCountOut != nullptr)
        {
            *moduleCountOut = moduleCount;
        }

        if (lineList.isEmpty())
        {
            return QStringLiteral("<未读取到模块>");
        }
        return lineList.join(QChar::LineFeed);
    }

    // 窗口缩略图抓取：优先按句柄抓图，失败时返回占位图。
    QPixmap grabWindowPreviewPixmap(const quint64 hwndValue, const QSize& targetSize)
    {
        QPixmap sourcePixmap;
        QScreen* primaryScreen = QGuiApplication::primaryScreen();
        if (primaryScreen != nullptr)
        {
            sourcePixmap = primaryScreen->grabWindow(static_cast<WId>(hwndValue));
        }

        if (!sourcePixmap.isNull())
        {
            return sourcePixmap.scaled(
                targetSize,
                Qt::KeepAspectRatio,
                Qt::SmoothTransformation);
        }

        QPixmap fallbackPixmap(targetSize);
        fallbackPixmap.fill(QColor(246, 248, 252));
        QPainter painter(&fallbackPixmap);
        painter.setPen(QColor(120, 120, 120));
        painter.drawRect(fallbackPixmap.rect().adjusted(0, 0, -1, -1));
        painter.drawText(
            fallbackPixmap.rect(),
            Qt::AlignCenter,
            QStringLiteral("无可用缩略图"));
        painter.end();
        return fallbackPixmap;
    }

    // 通过 PID 打开进程详情窗口：复用现有 ProcessDetailWindow，便于跳转联动。
    void openProcessDetailWindow(QWidget* parent, const std::uint32_t pid)
    {
        if (pid == 0)
        {
            return;
        }

        ks::process::ProcessRecord record;
        bool found = ks::process::QueryProcessStaticDetailByPid(pid, record);
        if (!found)
        {
            std::vector<ks::process::ProcessRecord> list =
                ks::process::EnumerateProcesses(ks::process::ProcessEnumStrategy::Auto);
            const auto it = std::find_if(
                list.begin(),
                list.end(),
                [pid](const ks::process::ProcessRecord& item) {
                    return item.pid == pid;
                });
            if (it != list.end())
            {
                record = *it;
                found = true;
            }
        }

        if (!found)
        {
            QMessageBox::warning(
                parent,
                QStringLiteral("进程详情"),
                QStringLiteral("未找到 PID=%1 对应进程。").arg(pid));
            return;
        }

        ProcessDetailWindow* detailWindow = new ProcessDetailWindow(record, nullptr);
        detailWindow->setAttribute(Qt::WA_DeleteOnClose, true);
        detailWindow->show();
        detailWindow->raise();
        detailWindow->activateWindow();
    }

    // 枚举上下文：用于 EnumWindows / EnumChildWindows 回调过程携带输出容器。
    struct EnumContext
    {
        std::vector<OtherDock::WindowInfo>* outputList = nullptr; // 输出窗口列表。
        int zOrderCounter = 0;                                    // 当前枚举序号。
    };

    // 填充单个 WindowInfo：从 HWND 读取常用属性并写入结构体。
    void fillWindowInfo(
        HWND windowHandle,
        const int zOrderValue,
        const bool childFlag,
        const QString& enumApiName,
        OtherDock::WindowInfo& infoOut)
    {
        infoOut.hwndValue = static_cast<quint64>(reinterpret_cast<quintptr>(windowHandle));
        infoOut.parentHwndValue = static_cast<quint64>(
            reinterpret_cast<quintptr>(::GetParent(windowHandle)));
        infoOut.ownerHwndValue = static_cast<quint64>(
            reinterpret_cast<quintptr>(::GetWindow(windowHandle, GW_OWNER)));
        infoOut.enumApiName = enumApiName;
        infoOut.isChildWindow = childFlag;
        infoOut.valid = ::IsWindow(windowHandle) != FALSE;
        infoOut.zOrder = zOrderValue;

        wchar_t titleBuffer[1024] = {};
        ::GetWindowTextW(windowHandle, titleBuffer, static_cast<int>(std::size(titleBuffer)));
        infoOut.titleText = QString::fromWCharArray(titleBuffer);

        wchar_t classBuffer[512] = {};
        ::GetClassNameW(windowHandle, classBuffer, static_cast<int>(std::size(classBuffer)));
        infoOut.classNameText = QString::fromWCharArray(classBuffer);

        RECT rect{};
        if (::GetWindowRect(windowHandle, &rect) != FALSE)
        {
            infoOut.windowRect = QRect(rect.left, rect.top, rect.right - rect.left, rect.bottom - rect.top);
        }
        else
        {
            infoOut.windowRect = QRect();
        }

        infoOut.styleValue = static_cast<quint64>(::GetWindowLongPtrW(windowHandle, GWL_STYLE));
        infoOut.exStyleValue = static_cast<quint64>(::GetWindowLongPtrW(windowHandle, GWL_EXSTYLE));

        infoOut.visible = ::IsWindowVisible(windowHandle) != FALSE;
        infoOut.enabled = ::IsWindowEnabled(windowHandle) != FALSE;
        infoOut.topMost = (infoOut.exStyleValue & WS_EX_TOPMOST) != 0;
        infoOut.minimized = ::IsIconic(windowHandle) != FALSE;
        infoOut.maximized = ::IsZoomed(windowHandle) != FALSE;

        std::uint32_t processId = 0;
        const DWORD threadId = ::GetWindowThreadProcessId(windowHandle, reinterpret_cast<DWORD*>(&processId));
        infoOut.processId = processId;
        infoOut.threadId = static_cast<std::uint32_t>(threadId);
        infoOut.processNameText = queryProcessNameByPid(processId, &infoOut.processImagePathText);

        // 透明度：仅当窗口具备分层属性时尝试读取 Alpha。
        infoOut.alphaValue = 255;
        if ((infoOut.exStyleValue & WS_EX_LAYERED) != 0)
        {
            COLORREF colorKey = 0;
            BYTE alpha = 255;
            DWORD flags = 0;
            if (::GetLayeredWindowAttributes(windowHandle, &colorKey, &alpha, &flags) != FALSE)
            {
                Q_UNUSED(colorKey);
                Q_UNUSED(flags);
                infoOut.alphaValue = static_cast<int>(alpha);
            }
        }
    }

    // 子窗口枚举回调：把每个子窗口都纳入快照，满足“层级视图”需求。
    BOOL CALLBACK enumChildWindowProc(HWND windowHandle, LPARAM param)
    {
        EnumContext* context = reinterpret_cast<EnumContext*>(param);
        if (context == nullptr || context->outputList == nullptr)
        {
            return TRUE;
        }

        OtherDock::WindowInfo info;
        fillWindowInfo(
            windowHandle,
            context->zOrderCounter++,
            true,
            QStringLiteral("EnumChildWindows"),
            info);
        context->outputList->push_back(info);
        return TRUE;
    }

    // 顶层窗口枚举回调：先记录顶层窗口，再递归枚举其子窗口。
    BOOL CALLBACK enumTopWindowProc(HWND windowHandle, LPARAM param)
    {
        EnumContext* context = reinterpret_cast<EnumContext*>(param);
        if (context == nullptr || context->outputList == nullptr)
        {
            return TRUE;
        }

        OtherDock::WindowInfo info;
        fillWindowInfo(
            windowHandle,
            context->zOrderCounter++,
            false,
            QStringLiteral("EnumWindows"),
            info);
        context->outputList->push_back(info);

        ::EnumChildWindows(windowHandle, enumChildWindowProc, param);
        return TRUE;
    }
}

// ============================================================
// WindowDetailDialog
// 作用说明：
// - 独立非模态窗口属性对话框；
// - 提供标签页化属性查看与部分实时修改能力。
// ============================================================
class WindowDetailDialog final : public QDialog
{
public:
    // 构造函数：
    // - 作用：缓存初始窗口信息，构建 UI 并同步一次运行时数据。
    // - 参数 info：来自窗口列表的快照数据。
    // - 参数 parent：Qt 父控件。
    explicit WindowDetailDialog(const OtherDock::WindowInfo& info, QWidget* parent = nullptr)
        : QDialog(parent)
        , m_info(info)
    {
        setAttribute(Qt::WA_DeleteOnClose, true);
        setWindowTitle(QStringLiteral("窗口属性 - [%1] (%2)")
            .arg(info.titleText.isEmpty() ? QStringLiteral("<无标题>") : info.titleText,
                hwndToText(info.hwndValue)));
        resize(900, 680);

        initializeUi();
        refreshRuntimeInfo();
    }

private:
    // 构建 UI：创建 8 个标签页并绑定底部操作按钮。
    void initializeUi()
    {
        QVBoxLayout* rootLayout = new QVBoxLayout(this);

        m_tabWidget = new QTabWidget(this);
        rootLayout->addWidget(m_tabWidget, 1);

        // ==================== 1. 常规信息 Tab ====================
        QWidget* generalPage = new QWidget(m_tabWidget);
        QFormLayout* generalLayout = new QFormLayout(generalPage);
        m_handleLabel = new QLabel(generalPage);
        m_parentHandleLabel = new QLabel(generalPage);
        m_ownerHandleLabel = new QLabel(generalPage);
        m_titleEdit = new QLineEdit(generalPage);
        m_classNameLabel = new QLabel(generalPage);
        m_instanceLabel = new QLabel(generalPage);
        m_stateLabel = new QLabel(generalPage);
        m_relationLabel = new QLabel(generalPage);
        generalLayout->addRow(QStringLiteral("句柄"), m_handleLabel);
        generalLayout->addRow(QStringLiteral("父句柄"), m_parentHandleLabel);
        generalLayout->addRow(QStringLiteral("所有者句柄"), m_ownerHandleLabel);
        generalLayout->addRow(QStringLiteral("标题（可编辑）"), m_titleEdit);
        generalLayout->addRow(QStringLiteral("类名"), m_classNameLabel);
        generalLayout->addRow(QStringLiteral("实例句柄"), m_instanceLabel);
        generalLayout->addRow(QStringLiteral("状态摘要"), m_stateLabel);
        generalLayout->addRow(QStringLiteral("关系摘要"), m_relationLabel);
        m_tabWidget->addTab(generalPage, QStringLiteral("常规信息"));

        // ==================== 2. 样式 Tab ====================
        QWidget* stylePage = new QWidget(m_tabWidget);
        QVBoxLayout* styleLayout = new QVBoxLayout(stylePage);
        m_styleText = new QPlainTextEdit(stylePage);
        m_styleText->setReadOnly(true);
        styleLayout->addWidget(m_styleText, 1);
        m_tabWidget->addTab(stylePage, QStringLiteral("样式与外观"));

        // ==================== 3. 位置尺寸 Tab ====================
        QWidget* positionPage = new QWidget(m_tabWidget);
        QFormLayout* positionLayout = new QFormLayout(positionPage);
        m_xSpin = new QSpinBox(positionPage);
        m_ySpin = new QSpinBox(positionPage);
        m_widthSpin = new QSpinBox(positionPage);
        m_heightSpin = new QSpinBox(positionPage);
        for (QSpinBox* spinBox : { m_xSpin, m_ySpin, m_widthSpin, m_heightSpin })
        {
            spinBox->setRange(-32768, 32768);
        }
        m_widthSpin->setRange(1, 32768);
        m_heightSpin->setRange(1, 32768);
        QPushButton* centerScreenButton = new QPushButton(QIcon(":/Icon/process_tree.svg"), QStringLiteral("居中到屏幕"), positionPage);
        centerScreenButton->setToolTip(QStringLiteral("把窗口移动到主屏幕中央"));
        centerScreenButton->setStyleSheet(blueButtonStyle());
        positionLayout->addRow(QStringLiteral("X"), m_xSpin);
        positionLayout->addRow(QStringLiteral("Y"), m_ySpin);
        positionLayout->addRow(QStringLiteral("宽度"), m_widthSpin);
        positionLayout->addRow(QStringLiteral("高度"), m_heightSpin);
        positionLayout->addRow(QStringLiteral("快捷操作"), centerScreenButton);
        connect(centerScreenButton, &QPushButton::clicked, this, [this]() {
            QScreen* screen = QGuiApplication::primaryScreen();
            if (screen == nullptr)
            {
                return;
            }
            const QRect screenRect = screen->availableGeometry();
            const int targetX = screenRect.x() + (screenRect.width() - m_widthSpin->value()) / 2;
            const int targetY = screenRect.y() + (screenRect.height() - m_heightSpin->value()) / 2;
            m_xSpin->setValue(targetX);
            m_ySpin->setValue(targetY);
        });
        m_tabWidget->addTab(positionPage, QStringLiteral("位置与尺寸"));

        // ==================== 4. 窗口状态 Tab ====================
        QWidget* statePage = new QWidget(m_tabWidget);
        QFormLayout* stateLayout = new QFormLayout(statePage);
        m_topMostCheck = new QCheckBox(QStringLiteral("置顶窗口"), statePage);
        m_alphaSlider = new QSlider(Qt::Horizontal, statePage);
        m_alphaSlider->setRange(20, 255);
        m_alphaLabel = new QLabel(statePage);
        stateLayout->addRow(QStringLiteral("置顶"), m_topMostCheck);
        stateLayout->addRow(QStringLiteral("透明度"), m_alphaSlider);
        stateLayout->addRow(QStringLiteral("当前 Alpha"), m_alphaLabel);
        connect(m_alphaSlider, &QSlider::valueChanged, this, [this](int value) {
            m_alphaLabel->setText(QStringLiteral("%1").arg(value));
        });
        m_tabWidget->addTab(statePage, QStringLiteral("窗口状态"));

        // ==================== 5. 进程线程 Tab ====================
        QWidget* processPage = new QWidget(m_tabWidget);
        QVBoxLayout* processLayout = new QVBoxLayout(processPage);
        m_processThreadText = new QPlainTextEdit(processPage);
        m_processThreadText->setReadOnly(true);
        processLayout->addWidget(m_processThreadText, 1);
        m_tabWidget->addTab(processPage, QStringLiteral("进程与线程"));

        // ==================== 6. 类信息 Tab ====================
        QWidget* classPage = new QWidget(m_tabWidget);
        QVBoxLayout* classLayout = new QVBoxLayout(classPage);
        m_classText = new QPlainTextEdit(classPage);
        m_classText->setReadOnly(true);
        classLayout->addWidget(m_classText, 1);
        m_tabWidget->addTab(classPage, QStringLiteral("类信息"));

        // ==================== 7. 消息钩子 Tab ====================
        QWidget* hookPage = new QWidget(m_tabWidget);
        QVBoxLayout* hookLayout = new QVBoxLayout(hookPage);
        m_hookText = new QPlainTextEdit(hookPage);
        m_hookText->setReadOnly(true);
        hookLayout->addWidget(m_hookText, 1);
        m_tabWidget->addTab(hookPage, QStringLiteral("消息钩子"));

        // ==================== 8. 高级属性 Tab ====================
        QWidget* advancedPage = new QWidget(m_tabWidget);
        QVBoxLayout* advancedLayout = new QVBoxLayout(advancedPage);
        m_advancedText = new QPlainTextEdit(advancedPage);
        m_advancedText->setReadOnly(true);
        advancedLayout->addWidget(m_advancedText, 1);
        m_tabWidget->addTab(advancedPage, QStringLiteral("高级属性"));

        // ==================== 底部按钮 ====================
        QHBoxLayout* buttonLayout = new QHBoxLayout();
        rootLayout->addLayout(buttonLayout);
        buttonLayout->addStretch(1);
        m_refreshButton = new QPushButton(QIcon(":/Icon/process_refresh.svg"), QString(), this);
        m_applyButton = new QPushButton(QIcon(":/Icon/process_start.svg"), QString(), this);
        m_exportButton = new QPushButton(QIcon(":/Icon/log_export.svg"), QString(), this);
        m_closeButton = new QPushButton(QIcon(":/Icon/process_terminate.svg"), QString(), this);

        m_refreshButton->setToolTip(QStringLiteral("刷新窗口属性"));
        m_applyButton->setToolTip(QStringLiteral("应用修改"));
        m_exportButton->setToolTip(QStringLiteral("导出信息"));
        m_closeButton->setToolTip(QStringLiteral("关闭详情窗口"));

        for (QPushButton* button : { m_refreshButton, m_applyButton, m_exportButton, m_closeButton })
        {
            button->setStyleSheet(blueButtonStyle());
            button->setFixedWidth(34);
            buttonLayout->addWidget(button, 0);
        }

        connect(m_refreshButton, &QPushButton::clicked, this, [this]() {
            refreshRuntimeInfo();
        });
        connect(m_applyButton, &QPushButton::clicked, this, [this]() {
            applyChanges();
        });
        connect(m_exportButton, &QPushButton::clicked, this, [this]() {
            exportInfo();
        });
        connect(m_closeButton, &QPushButton::clicked, this, &QDialog::close);
    }

    // 刷新运行时信息：对句柄重新采样，避免显示陈旧数据。
    void refreshRuntimeInfo()
    {
        HWND windowHandle = toHwnd(m_info.hwndValue);
        if (::IsWindow(windowHandle) == FALSE)
        {
            m_stateLabel->setText(QStringLiteral("窗口已失效"));
            return;
        }

        // 句柄与基础状态字段。
        m_handleLabel->setText(hwndToText(m_info.hwndValue));
        m_parentHandleLabel->setText(hwndToText(static_cast<quint64>(reinterpret_cast<quintptr>(::GetParent(windowHandle)))));
        m_ownerHandleLabel->setText(hwndToText(static_cast<quint64>(reinterpret_cast<quintptr>(::GetWindow(windowHandle, GW_OWNER)))));
        m_classNameLabel->setText(m_info.classNameText);
        m_titleEdit->setText(m_info.titleText);
        m_instanceLabel->setText(hwndToText(static_cast<quint64>(::GetWindowLongPtrW(windowHandle, GWLP_HINSTANCE))));

        const bool visible = ::IsWindowVisible(windowHandle) != FALSE;
        const bool enabled = ::IsWindowEnabled(windowHandle) != FALSE;
        const bool minimized = ::IsIconic(windowHandle) != FALSE;
        const bool maximized = ::IsZoomed(windowHandle) != FALSE;

        m_stateLabel->setText(QStringLiteral("可见:%1, 可用:%2, 状态:%3")
            .arg(boolText(visible), boolText(enabled), windowStateText(true, minimized, maximized)));
        m_relationLabel->setText(QStringLiteral("PID=%1, TID=%2")
            .arg(m_info.processId)
            .arg(m_info.threadId));

        // 样式信息：输出十六进制样式值与常见标记判断。
        const quint64 styleValue = static_cast<quint64>(::GetWindowLongPtrW(windowHandle, GWL_STYLE));
        const quint64 exStyleValue = static_cast<quint64>(::GetWindowLongPtrW(windowHandle, GWL_EXSTYLE));
        QString styleText;
        styleText += QStringLiteral("Style: 0x%1\n").arg(styleValue, 0, 16);
        styleText += QStringLiteral("ExStyle: 0x%1\n").arg(exStyleValue, 0, 16);
        styleText += QStringLiteral("WS_POPUP: %1\n").arg(boolText((styleValue & WS_POPUP) != 0));
        styleText += QStringLiteral("WS_CHILD: %1\n").arg(boolText((styleValue & WS_CHILD) != 0));
        styleText += QStringLiteral("WS_VISIBLE: %1\n").arg(boolText((styleValue & WS_VISIBLE) != 0));
        styleText += QStringLiteral("WS_DISABLED: %1\n").arg(boolText((styleValue & WS_DISABLED) != 0));
        styleText += QStringLiteral("WS_EX_TOPMOST: %1\n").arg(boolText((exStyleValue & WS_EX_TOPMOST) != 0));
        styleText += QStringLiteral("WS_EX_LAYERED: %1\n").arg(boolText((exStyleValue & WS_EX_LAYERED) != 0));
        m_styleText->setPlainText(styleText);

        // 窗口位置与尺寸写回编辑控件。
        RECT rect{};
        const bool windowRectReady = (::GetWindowRect(windowHandle, &rect) != FALSE);
        if (windowRectReady)
        {
            m_xSpin->setValue(rect.left);
            m_ySpin->setValue(rect.top);
            m_widthSpin->setValue(std::max<int>(1, static_cast<int>(rect.right - rect.left)));
            m_heightSpin->setValue(std::max<int>(1, static_cast<int>(rect.bottom - rect.top)));
        }
        else
        {
            m_xSpin->setValue(0);
            m_ySpin->setValue(0);
            m_widthSpin->setValue(1);
            m_heightSpin->setValue(1);
        }

        // 状态页：置顶与透明度。
        m_topMostCheck->setChecked((exStyleValue & WS_EX_TOPMOST) != 0);
        int alphaValue = 255;
        if ((exStyleValue & WS_EX_LAYERED) != 0)
        {
            COLORREF colorKey = 0;
            BYTE alpha = 255;
            DWORD flags = 0;
            if (::GetLayeredWindowAttributes(windowHandle, &colorKey, &alpha, &flags) != FALSE)
            {
                Q_UNUSED(colorKey);
                Q_UNUSED(flags);
                alphaValue = static_cast<int>(alpha);
            }
        }
        m_alphaSlider->setValue(alphaValue);
        m_alphaLabel->setText(QString::number(alphaValue));

        // 进程线程信息：补齐路径、优先级、句柄计数、启动时间和模块摘要，便于排障和行为审计。
        QString processText;
        processText += QStringLiteral("PID: %1\n").arg(m_info.processId);
        processText += QStringLiteral("ProcessName: %1\n").arg(m_info.processNameText);
        processText += QStringLiteral("TID: %1\n").arg(m_info.threadId);
        if (windowRectReady)
        {
            processText += QStringLiteral("WindowRect: [%1,%2,%3,%4]\n")
                .arg(rect.left).arg(rect.top).arg(rect.right).arg(rect.bottom);
        }
        else
        {
            processText += QStringLiteral("WindowRect: <N/A>\n");
        }

        HANDLE processHandle = ::OpenProcess(
            PROCESS_QUERY_LIMITED_INFORMATION | PROCESS_VM_READ,
            FALSE,
            m_info.processId);
        if (processHandle != nullptr)
        {
            wchar_t processPathBuffer[MAX_PATH * 2] = {};
            DWORD processPathLength = static_cast<DWORD>(std::size(processPathBuffer));
            if (::QueryFullProcessImageNameW(processHandle, 0, processPathBuffer, &processPathLength) != FALSE)
            {
                processText += QStringLiteral("ProcessPath: %1\n")
                    .arg(QString::fromWCharArray(processPathBuffer, static_cast<int>(processPathLength)));
            }

            const DWORD processPriority = ::GetPriorityClass(processHandle);
            processText += QStringLiteral("ProcessPriorityClass: 0x%1\n")
                .arg(static_cast<qulonglong>(processPriority), 0, 16);

            DWORD handleCount = 0;
            if (::GetProcessHandleCount(processHandle, &handleCount) != FALSE)
            {
                processText += QStringLiteral("ProcessHandleCount: %1\n").arg(handleCount);
            }

            FILETIME createTime{};
            FILETIME exitTime{};
            FILETIME kernelTime{};
            FILETIME userTime{};
            if (::GetProcessTimes(processHandle, &createTime, &exitTime, &kernelTime, &userTime) != FALSE)
            {
                processText += QStringLiteral("StartTime: %1\n").arg(fileTimeToLocalText(createTime));
                const qulonglong cpuTotal = fileTimeToUInt64(kernelTime) + fileTimeToUInt64(userTime);
                processText += QStringLiteral("CPUTime(Kernel+User): %1\n").arg(formatDurationFrom100ns(cpuTotal));

                FILETIME nowFileTime{};
                ::GetSystemTimeAsFileTime(&nowFileTime);
                const qulonglong nowTick = fileTimeToUInt64(nowFileTime);
                const qulonglong startTick = fileTimeToUInt64(createTime);
                if (nowTick > startTick)
                {
                    processText += QStringLiteral("RunTime: %1\n").arg(formatDurationFrom100ns(nowTick - startTick));
                }
            }

            ::CloseHandle(processHandle);
        }
        else
        {
            processText += QStringLiteral("ProcessOpen: 失败（权限不足或进程已退出）\n");
        }

        int moduleCount = 0;
        const QString moduleSummaryText = buildModuleSummaryText(m_info.processId, 8, &moduleCount);
        processText += QStringLiteral("ModuleCount: %1\n").arg(moduleCount);
        processText += QStringLiteral("ModulePreview:\n%1\n").arg(moduleSummaryText);

        HANDLE threadHandle = ::OpenThread(THREAD_QUERY_LIMITED_INFORMATION, FALSE, m_info.threadId);
        if (threadHandle != nullptr)
        {
            const int threadPriority = ::GetThreadPriority(threadHandle);
            processText += QStringLiteral("ThreadPriority: %1\n").arg(threadPriority);

            BOOL priorityBoostDisabled = FALSE;
            if (::GetThreadPriorityBoost(threadHandle, &priorityBoostDisabled) != FALSE)
            {
                processText += QStringLiteral("ThreadPriorityBoostDisabled: %1\n")
                    .arg(boolText(priorityBoostDisabled != FALSE));
            }
            ::CloseHandle(threadHandle);
        }
        else
        {
            processText += QStringLiteral("ThreadOpen: 失败（线程可能已退出）\n");
        }
        processText += QStringLiteral("提示：可从右键菜单跳转到进程详细信息页。");
        m_processThreadText->setPlainText(processText);

        // 类信息：展示类名、类样式、窗口过程地址以及类资源句柄。
        QString classText;
        classText += QStringLiteral("ClassName: %1\n").arg(m_info.classNameText);
        classText += QStringLiteral("ClassAtom: 0x%1\n").arg(static_cast<qulonglong>(::GetClassLongPtrW(windowHandle, GCW_ATOM)), 0, 16);
        classText += QStringLiteral("ClassStyle: 0x%1\n").arg(static_cast<qulonglong>(::GetClassLongPtrW(windowHandle, GCL_STYLE)), 0, 16);
        classText += QStringLiteral("ClassExtraBytes: %1\n")
            .arg(static_cast<qlonglong>(::GetClassLongPtrW(windowHandle, GCL_CBCLSEXTRA)));
        classText += QStringLiteral("WindowExtraBytes: %1\n")
            .arg(static_cast<qlonglong>(::GetClassLongPtrW(windowHandle, GCL_CBWNDEXTRA)));
        classText += QStringLiteral("WndProc: 0x%1\n").arg(static_cast<qulonglong>(::GetWindowLongPtrW(windowHandle, GWLP_WNDPROC)), 0, 16);
        classText += QStringLiteral("MenuHandle: 0x%1\n").arg(static_cast<qulonglong>(reinterpret_cast<quintptr>(::GetMenu(windowHandle))), 0, 16);
        classText += QStringLiteral("ClassHCursor: 0x%1\n")
            .arg(static_cast<qulonglong>(::GetClassLongPtrW(windowHandle, GCLP_HCURSOR)), 0, 16);
        classText += QStringLiteral("ClassHIcon: 0x%1\n")
            .arg(static_cast<qulonglong>(::GetClassLongPtrW(windowHandle, GCLP_HICON)), 0, 16);
        classText += QStringLiteral("ClassHIconSm: 0x%1\n")
            .arg(static_cast<qulonglong>(::GetClassLongPtrW(windowHandle, GCLP_HICONSM)), 0, 16);
        classText += QStringLiteral("ClassHbrBackground: 0x%1\n")
            .arg(static_cast<qulonglong>(::GetClassLongPtrW(windowHandle, GCLP_HBRBACKGROUND)), 0, 16);

        const HMODULE classModuleHandle = reinterpret_cast<HMODULE>(::GetClassLongPtrW(windowHandle, GCLP_HMODULE));
        classText += QStringLiteral("ClassModule: 0x%1\n")
            .arg(static_cast<qulonglong>(reinterpret_cast<quintptr>(classModuleHandle)), 0, 16);
        if (classModuleHandle != nullptr)
        {
            wchar_t modulePathBuffer[MAX_PATH * 2] = {};
            const DWORD pathLength = ::GetModuleFileNameW(
                classModuleHandle,
                modulePathBuffer,
                static_cast<DWORD>(std::size(modulePathBuffer)));
            if (pathLength > 0)
            {
                classText += QStringLiteral("ClassModulePath: %1\n")
                    .arg(QString::fromWCharArray(modulePathBuffer, static_cast<int>(pathLength)));
            }
        }
        m_classText->setPlainText(classText);

        // 钩子/消息页：补齐消息队列状态与 GUI 线程焦点句柄，替代原先占位文本。
        int childWindowCount = 0;
        ::EnumChildWindows(
            windowHandle,
            [](HWND, LPARAM lParam) -> BOOL {
                int* countPtr = reinterpret_cast<int*>(lParam);
                if (countPtr != nullptr)
                {
                    *countPtr += 1;
                }
                return TRUE;
            },
            reinterpret_cast<LPARAM>(&childWindowCount));

        int siblingCount = 0;
        const HWND parentWindowHandle = ::GetParent(windowHandle);
        if (parentWindowHandle != nullptr)
        {
            std::pair<HWND, int*> siblingContext{ windowHandle, &siblingCount };
            ::EnumChildWindows(
                parentWindowHandle,
                [](HWND childHandle, LPARAM lParam) -> BOOL {
                    auto* pairPtr = reinterpret_cast<std::pair<HWND, int*>*>(lParam);
                    if (pairPtr == nullptr || pairPtr->second == nullptr)
                    {
                        return TRUE;
                    }
                    if (childHandle != pairPtr->first)
                    {
                        *(pairPtr->second) += 1;
                    }
                    return TRUE;
                },
                reinterpret_cast<LPARAM>(&siblingContext));
        }

        GUITHREADINFO guiThreadInfo{};
        guiThreadInfo.cbSize = sizeof(guiThreadInfo);
        const bool guiInfoReady = (::GetGUIThreadInfo(m_info.threadId, &guiThreadInfo) != FALSE);

        const DWORD queueStatus = ::GetQueueStatus(QS_ALLINPUT);
        QString hookText;
        hookText += QStringLiteral("IsHungAppWindow: %1\n")
            .arg(boolText(::IsHungAppWindow(windowHandle) != FALSE));
        hookText += QStringLiteral("ChildWindowCount: %1\n").arg(childWindowCount);
        hookText += QStringLiteral("SiblingWindowCount: %1\n").arg(siblingCount);
        hookText += QStringLiteral("Visible: %1\n").arg(boolText(::IsWindowVisible(windowHandle) != FALSE));
        hookText += QStringLiteral("Enabled: %1\n").arg(boolText(::IsWindowEnabled(windowHandle) != FALSE));
        hookText += QStringLiteral("QueueStatusRaw: 0x%1\n")
            .arg(static_cast<qulonglong>(queueStatus), 0, 16);
        hookText += QStringLiteral("QueueCurrentFlags(LOWORD): 0x%1\n")
            .arg(static_cast<unsigned int>(LOWORD(queueStatus)), 0, 16);
        hookText += QStringLiteral("QueueNewFlags(HIWORD): 0x%1\n")
            .arg(static_cast<unsigned int>(HIWORD(queueStatus)), 0, 16);
        hookText += QStringLiteral("GUIThreadInfoReady: %1\n").arg(boolText(guiInfoReady));
        if (guiInfoReady)
        {
            hookText += QStringLiteral("Focus: %1\n")
                .arg(hwndToText(static_cast<quint64>(reinterpret_cast<quintptr>(guiThreadInfo.hwndFocus))));
            hookText += QStringLiteral("Active: %1\n")
                .arg(hwndToText(static_cast<quint64>(reinterpret_cast<quintptr>(guiThreadInfo.hwndActive))));
            hookText += QStringLiteral("Capture: %1\n")
                .arg(hwndToText(static_cast<quint64>(reinterpret_cast<quintptr>(guiThreadInfo.hwndCapture))));
            hookText += QStringLiteral("Caret: %1\n")
                .arg(hwndToText(static_cast<quint64>(reinterpret_cast<quintptr>(guiThreadInfo.hwndCaret))));
            hookText += QStringLiteral("MenuOwner: %1\n")
                .arg(hwndToText(static_cast<quint64>(reinterpret_cast<quintptr>(guiThreadInfo.hwndMenuOwner))));
        }
        hookText += QStringLiteral("ForegroundWindow: %1\n")
            .arg(hwndToText(static_cast<quint64>(reinterpret_cast<quintptr>(::GetForegroundWindow()))));
        hookText += QStringLiteral("说明：系统级 Hook 链表不提供官方通用枚举接口，当前页聚焦可稳定获取的消息上下文。");
        m_hookText->setPlainText(hookText);

        // 高级页：补齐 DPI、属性表（GetProp/EnumPropsExW）与窗口额外字节等关键内容。
        QString advancedText;
        advancedText += QStringLiteral("UserData (GWLP_USERDATA): 0x%1\n")
            .arg(static_cast<qulonglong>(::GetWindowLongPtrW(windowHandle, GWLP_USERDATA)), 0, 16);
        advancedText += QStringLiteral("ID (GWLP_ID): 0x%1\n")
            .arg(static_cast<qulonglong>(::GetWindowLongPtrW(windowHandle, GWLP_ID)), 0, 16);
        advancedText += QStringLiteral("WndProc (GWLP_WNDPROC): 0x%1\n")
            .arg(static_cast<qulonglong>(::GetWindowLongPtrW(windowHandle, GWLP_WNDPROC)), 0, 16);
        advancedText += QStringLiteral("HINSTANCE (GWLP_HINSTANCE): 0x%1\n")
            .arg(static_cast<qulonglong>(::GetWindowLongPtrW(windowHandle, GWLP_HINSTANCE)), 0, 16);
        advancedText += QStringLiteral("Parent: %1\n").arg(hwndToText(static_cast<quint64>(reinterpret_cast<quintptr>(::GetParent(windowHandle)))));
        advancedText += QStringLiteral("Owner: %1\n").arg(hwndToText(static_cast<quint64>(reinterpret_cast<quintptr>(::GetWindow(windowHandle, GW_OWNER)))));

        DWORD displayAffinity = 0;
        if (::GetWindowDisplayAffinity(windowHandle, &displayAffinity) != FALSE)
        {
            advancedText += QStringLiteral("DisplayAffinity: 0x%1\n")
                .arg(static_cast<qulonglong>(displayAffinity), 0, 16);
        }

        RECT clientRect{};
        if (::GetClientRect(windowHandle, &clientRect) != FALSE)
        {
            advancedText += QStringLiteral("ClientRect: [%1,%2,%3,%4]\n")
                .arg(clientRect.left)
                .arg(clientRect.top)
                .arg(clientRect.right)
                .arg(clientRect.bottom);
        }

        // 兼容旧系统：GetDpiForWindow 仅在新系统提供，运行时动态判断。
        using GetDpiForWindowFunc = UINT(WINAPI*)(HWND);
        const HMODULE user32Handle = ::GetModuleHandleW(L"user32.dll");
        GetDpiForWindowFunc getDpiForWindow = nullptr;
        if (user32Handle != nullptr)
        {
            getDpiForWindow = reinterpret_cast<GetDpiForWindowFunc>(::GetProcAddress(user32Handle, "GetDpiForWindow"));
        }
        if (getDpiForWindow != nullptr)
        {
            advancedText += QStringLiteral("DPI: %1\n").arg(getDpiForWindow(windowHandle));
        }

        // 枚举窗口属性键值：用于发现子类化框架、输入法、UI库挂载的额外状态。
        QStringList propLines;
        WindowPropEnumContext propContext;
        propContext.lines = &propLines;
        propContext.maxCount = 200;
        ::EnumPropsExW(windowHandle, enumWindowPropProc, reinterpret_cast<LPARAM>(&propContext));
        advancedText += QStringLiteral("PropCount: %1\n").arg(propLines.size());
        if (!propLines.isEmpty())
        {
            advancedText += QStringLiteral("Properties:\n");
            for (const QString& line : propLines)
            {
                advancedText += QStringLiteral("  - %1\n").arg(line);
            }
        }
        else
        {
            advancedText += QStringLiteral("Properties: <none>\n");
        }
        m_advancedText->setPlainText(advancedText);
    }

    // 应用改动：把标题、位置、置顶、透明度写回目标窗口。
    void applyChanges()
    {
        HWND windowHandle = toHwnd(m_info.hwndValue);
        if (::IsWindow(windowHandle) == FALSE)
        {
            QMessageBox::warning(this, QStringLiteral("应用失败"), QStringLiteral("目标窗口句柄已失效。"));
            return;
        }

        // 标题修改：支持直接编辑后写回。
        const QString titleText = m_titleEdit->text();
        ::SetWindowTextW(windowHandle, reinterpret_cast<const wchar_t*>(titleText.utf16()));

        // 位置尺寸修改：按输入框坐标和尺寸移动窗口。
        ::MoveWindow(
            windowHandle,
            m_xSpin->value(),
            m_ySpin->value(),
            std::max(1, m_widthSpin->value()),
            std::max(1, m_heightSpin->value()),
            TRUE);

        // 置顶切换：由复选框决定 HWND_TOPMOST / HWND_NOTOPMOST。
        ::SetWindowPos(
            windowHandle,
            m_topMostCheck->isChecked() ? HWND_TOPMOST : HWND_NOTOPMOST,
            0,
            0,
            0,
            0,
            SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);

        // 透明度修改：强制设置 WS_EX_LAYERED 后写入 Alpha。
        LONG_PTR exStyle = ::GetWindowLongPtrW(windowHandle, GWL_EXSTYLE);
        if ((exStyle & WS_EX_LAYERED) == 0)
        {
            ::SetWindowLongPtrW(windowHandle, GWL_EXSTYLE, exStyle | WS_EX_LAYERED);
        }
        ::SetLayeredWindowAttributes(windowHandle, 0, static_cast<BYTE>(m_alphaSlider->value()), LWA_ALPHA);

        refreshRuntimeInfo();
        QMessageBox::information(this, QStringLiteral("窗口属性"), QStringLiteral("已应用修改。"));
    }

    // 导出信息：把当前所有标签页关键内容写入文本文件。
    void exportInfo()
    {
        const QString defaultName = QStringLiteral("window_detail_%1_%2.txt")
            .arg(QString::number(m_info.processId))
            .arg(QDateTime::currentDateTime().toString(QStringLiteral("yyyyMMdd_HHmmss")));

        const QString outputPath = QFileDialog::getSaveFileName(
            this,
            QStringLiteral("导出窗口详情"),
            defaultName,
            QStringLiteral("文本文件 (*.txt)"));
        if (outputPath.trimmed().isEmpty())
        {
            return;
        }

        QFile file(outputPath);
        if (!file.open(QIODevice::WriteOnly | QIODevice::Text | QIODevice::Truncate))
        {
            QMessageBox::warning(this, QStringLiteral("导出失败"), QStringLiteral("无法写入文件：%1").arg(outputPath));
            return;
        }

        QTextStream out(&file);
        out.setEncoding(QStringConverter::Utf8);
        out << buildReportText();
        file.close();

        QMessageBox::information(this, QStringLiteral("导出成功"), QStringLiteral("已保存到：%1").arg(outputPath));
    }

    // 组装导出文本：集中输出所有页核心内容，便于审计留档。
    QString buildReportText() const
    {
        QString text;
        text += QStringLiteral("[常规]\n");
        text += QStringLiteral("句柄: %1\n").arg(m_handleLabel->text());
        text += QStringLiteral("父句柄: %1\n").arg(m_parentHandleLabel->text());
        text += QStringLiteral("所有者句柄: %1\n").arg(m_ownerHandleLabel->text());
        text += QStringLiteral("标题: %1\n").arg(m_titleEdit->text());
        text += QStringLiteral("类名: %1\n").arg(m_classNameLabel->text());
        text += QStringLiteral("实例句柄: %1\n").arg(m_instanceLabel->text());
        text += QStringLiteral("状态摘要: %1\n").arg(m_stateLabel->text());
        text += QStringLiteral("关系摘要: %1\n\n").arg(m_relationLabel->text());

        text += QStringLiteral("[样式]\n%1\n\n").arg(m_styleText->toPlainText());
        text += QStringLiteral("[位置]\nX=%1, Y=%2, W=%3, H=%4\n\n")
            .arg(m_xSpin->value())
            .arg(m_ySpin->value())
            .arg(m_widthSpin->value())
            .arg(m_heightSpin->value());
        text += QStringLiteral("[状态]\nTopMost=%1, Alpha=%2\n\n")
            .arg(boolText(m_topMostCheck->isChecked()))
            .arg(m_alphaSlider->value());
        text += QStringLiteral("[进程线程]\n%1\n\n").arg(m_processThreadText->toPlainText());
        text += QStringLiteral("[类信息]\n%1\n\n").arg(m_classText->toPlainText());
        text += QStringLiteral("[消息钩子]\n%1\n\n").arg(m_hookText->toPlainText());
        text += QStringLiteral("[高级属性]\n%1\n").arg(m_advancedText->toPlainText());
        return text;
    }

private:
    OtherDock::WindowInfo m_info;          // 目标窗口快照。

    QTabWidget* m_tabWidget = nullptr;     // 标签页容器。

    QLabel* m_handleLabel = nullptr;       // 句柄显示。
    QLabel* m_parentHandleLabel = nullptr; // 父句柄显示。
    QLabel* m_ownerHandleLabel = nullptr;  // 所有者句柄显示。
    QLineEdit* m_titleEdit = nullptr;      // 可编辑标题输入。
    QLabel* m_classNameLabel = nullptr;    // 类名显示。
    QLabel* m_instanceLabel = nullptr;     // 实例句柄显示。
    QLabel* m_stateLabel = nullptr;        // 状态摘要显示。
    QLabel* m_relationLabel = nullptr;     // 关系摘要显示。

    QPlainTextEdit* m_styleText = nullptr;      // 样式文本区域。
    QSpinBox* m_xSpin = nullptr;                // X 坐标输入。
    QSpinBox* m_ySpin = nullptr;                // Y 坐标输入。
    QSpinBox* m_widthSpin = nullptr;            // 宽度输入。
    QSpinBox* m_heightSpin = nullptr;           // 高度输入。
    QCheckBox* m_topMostCheck = nullptr;        // 置顶复选框。
    QSlider* m_alphaSlider = nullptr;           // 透明度滑块。
    QLabel* m_alphaLabel = nullptr;             // 透明度文本。

    QPlainTextEdit* m_processThreadText = nullptr; // 进程线程页文本。
    QPlainTextEdit* m_classText = nullptr;         // 类信息页文本。
    QPlainTextEdit* m_hookText = nullptr;          // 钩子页文本。
    QPlainTextEdit* m_advancedText = nullptr;      // 高级页文本。

    QPushButton* m_refreshButton = nullptr;    // 刷新按钮。
    QPushButton* m_applyButton = nullptr;      // 应用按钮。
    QPushButton* m_exportButton = nullptr;     // 导出按钮。
    QPushButton* m_closeButton = nullptr;      // 关闭按钮。
};

OtherDock::OtherDock(QWidget* parent)
    : QWidget(parent)
{
    kLogEvent event;
    info << event << "[OtherDock] 构造开始，初始化窗口管理页。" << eol;

    initializeUi();
    initializeConnections();
    applyViewMode();
    refreshWindowListAsync();
}

OtherDock::~OtherDock()
{
    if (m_autoRefreshTimer != nullptr)
    {
        m_autoRefreshTimer->stop();
    }
}

void OtherDock::initializeUi()
{
    // 根布局负责承载“工具栏 + 主分割区 + 状态栏”。
    m_rootLayout = new QVBoxLayout(this);
    m_rootLayout->setContentsMargins(4, 4, 4, 4);
    m_rootLayout->setSpacing(6);

    // 顶部工具栏：覆盖刷新、自动刷新、筛选、分组和导出等控制项。
    m_toolBarWidget = new QWidget(this);
    m_toolBarLayout = new QHBoxLayout(m_toolBarWidget);
    m_toolBarLayout->setContentsMargins(0, 0, 0, 0);
    m_toolBarLayout->setSpacing(4);
    m_rootLayout->addWidget(m_toolBarWidget, 0);

    m_refreshButton = new QPushButton(QIcon(":/Icon/process_refresh.svg"), QString(), m_toolBarWidget);
    m_refreshButton->setToolTip(QStringLiteral("刷新窗口列表"));
    m_refreshButton->setStyleSheet(blueButtonStyle());
    m_refreshButton->setFixedWidth(32);

    m_autoRefreshCheck = new QCheckBox(QStringLiteral("自动"), m_toolBarWidget);
    m_autoRefreshCheck->setToolTip(QStringLiteral("启用自动刷新窗口列表"));

    m_autoRefreshIntervalSpin = new QSpinBox(m_toolBarWidget);
    m_autoRefreshIntervalSpin->setRange(300, 5000);
    m_autoRefreshIntervalSpin->setSingleStep(100);
    m_autoRefreshIntervalSpin->setValue(1000);
    m_autoRefreshIntervalSpin->setSuffix(QStringLiteral(" ms"));
    m_autoRefreshIntervalSpin->setToolTip(QStringLiteral("自动刷新间隔（毫秒）"));
    m_autoRefreshIntervalSpin->setStyleSheet(blueInputStyle());

    m_filterEdit = new QLineEdit(m_toolBarWidget);
    m_filterEdit->setPlaceholderText(QStringLiteral("筛选：标题 / 进程名 / 类名 / HWND"));
    m_filterEdit->setToolTip(QStringLiteral("输入关键字实时过滤窗口"));
    m_filterEdit->setStyleSheet(blueInputStyle());

    m_filterModeCombo = new QComboBox(m_toolBarWidget);
    m_filterModeCombo->addItems({
        QStringLiteral("可见窗口"),
        QStringLiteral("隐藏窗口"),
        QStringLiteral("顶层窗口"),
        QStringLiteral("子窗口"),
        QStringLiteral("无效窗口")
        });
    m_filterModeCombo->setToolTip(QStringLiteral("选择窗口过滤条件"));
    m_filterModeCombo->setStyleSheet(blueInputStyle());

    m_groupModeCombo = new QComboBox(m_toolBarWidget);
    m_groupModeCombo->addItems({
        QStringLiteral("按进程分组"),
        QStringLiteral("按Z序排列"),
        QStringLiteral("按窗口层级"),
        QStringLiteral("按显示器分组")
        });
    m_groupModeCombo->setToolTip(QStringLiteral("选择窗口分组方式"));
    m_groupModeCombo->setStyleSheet(blueInputStyle());

    m_viewModeCombo = new QComboBox(m_toolBarWidget);
    m_viewModeCombo->addItems({
        QStringLiteral("图标视图"),
        QStringLiteral("列表视图"),
        QStringLiteral("详细信息视图")
        });
    m_viewModeCombo->setToolTip(QStringLiteral("切换列表显示样式"));
    m_viewModeCombo->setStyleSheet(blueInputStyle());

    m_exportButton = new QPushButton(QIcon(":/Icon/log_export.svg"), QString(), m_toolBarWidget);
    m_exportButton->setToolTip(QStringLiteral("导出当前列表为 TSV"));
    m_exportButton->setStyleSheet(blueButtonStyle());
    m_exportButton->setFixedWidth(32);

    m_toolBarLayout->addWidget(m_refreshButton, 0);
    m_toolBarLayout->addWidget(m_autoRefreshCheck, 0);
    m_toolBarLayout->addWidget(m_autoRefreshIntervalSpin, 0);
    m_toolBarLayout->addWidget(m_filterEdit, 1);
    m_toolBarLayout->addWidget(m_filterModeCombo, 0);
    m_toolBarLayout->addWidget(m_groupModeCombo, 0);
    m_toolBarLayout->addWidget(m_viewModeCombo, 0);
    m_toolBarLayout->addWidget(m_exportButton, 0);

    // 中部主内容：左侧窗口树，右侧预览与摘要信息。
    m_mainSplitter = new QSplitter(Qt::Horizontal, this);
    m_rootLayout->addWidget(m_mainSplitter, 1);

    m_windowTree = new QTreeWidget(m_mainSplitter);
    m_windowTree->setContextMenuPolicy(Qt::CustomContextMenu);
    m_windowTree->setRootIsDecorated(true);
    m_windowTree->setAlternatingRowColors(true);
    m_windowTree->setSelectionMode(QAbstractItemView::SingleSelection);
    m_windowTree->setUniformRowHeights(true);
    m_windowTree->setSortingEnabled(false);
    m_windowTree->header()->setStyleSheet(blueHeaderStyle());
    m_windowTree->setColumnCount(13);
    m_windowTree->setHeaderLabels({
        QStringLiteral("窗口标题"),
        QStringLiteral("句柄"),
        QStringLiteral("枚举API"),
        QStringLiteral("类名"),
        QStringLiteral("PID"),
        QStringLiteral("进程名"),
        QStringLiteral("TID"),
        QStringLiteral("大小"),
        QStringLiteral("可见"),
        QStringLiteral("可用"),
        QStringLiteral("顶层"),
        QStringLiteral("状态"),
        QStringLiteral("透明度")
        });
    m_windowTree->header()->setStretchLastSection(false);
    m_windowTree->header()->setSectionResizeMode(kWindowColumnTitle, QHeaderView::Interactive);
    m_windowTree->header()->setSectionResizeMode(kWindowColumnHandle, QHeaderView::ResizeToContents);
    m_windowTree->header()->setSectionResizeMode(kWindowColumnEnumApi, QHeaderView::ResizeToContents);
    m_windowTree->header()->setSectionResizeMode(kWindowColumnClassName, QHeaderView::ResizeToContents);
    m_windowTree->header()->setSectionResizeMode(kWindowColumnPid, QHeaderView::ResizeToContents);
    m_windowTree->header()->setSectionResizeMode(kWindowColumnProcessName, QHeaderView::ResizeToContents);
    m_windowTree->header()->setSectionResizeMode(kWindowColumnTid, QHeaderView::ResizeToContents);
    m_windowTree->header()->setSectionResizeMode(kWindowColumnSize, QHeaderView::ResizeToContents);
    m_windowTree->header()->setSectionResizeMode(kWindowColumnVisible, QHeaderView::ResizeToContents);
    m_windowTree->header()->setSectionResizeMode(kWindowColumnEnabled, QHeaderView::ResizeToContents);
    m_windowTree->header()->setSectionResizeMode(kWindowColumnTopMost, QHeaderView::ResizeToContents);
    m_windowTree->header()->setSectionResizeMode(kWindowColumnState, QHeaderView::ResizeToContents);
    m_windowTree->header()->setSectionResizeMode(kWindowColumnAlpha, QHeaderView::ResizeToContents);
    m_windowTree->setColumnWidth(kWindowColumnTitle, 420);

    m_previewWidget = new QWidget(m_mainSplitter);
    m_previewLayout = new QVBoxLayout(m_previewWidget);
    m_previewLayout->setContentsMargins(6, 6, 6, 6);
    m_previewLayout->setSpacing(6);

    m_thumbnailLabel = new QLabel(m_previewWidget);
    m_thumbnailLabel->setMinimumSize(320, 220);
    m_thumbnailLabel->setAlignment(Qt::AlignCenter);
    m_thumbnailLabel->setStyleSheet(QStringLiteral("border:1px solid #D8D8D8;background:#F8FAFD;"));

    m_captureButton = new QPushButton(QIcon(":/Icon/process_details.svg"), QString(), m_previewWidget);
    m_captureButton->setToolTip(QStringLiteral("保存当前窗口截图"));
    m_captureButton->setStyleSheet(blueButtonStyle());
    m_captureButton->setFixedWidth(34);

    m_quickInfoText = new QTextEdit(m_previewWidget);
    m_quickInfoText->setReadOnly(true);
    m_quickInfoText->setStyleSheet(blueInputStyle());

    QHBoxLayout* captureLayout = new QHBoxLayout();
    captureLayout->addStretch(1);
    captureLayout->addWidget(m_captureButton, 0);

    m_previewLayout->addWidget(m_thumbnailLabel, 0);
    m_previewLayout->addLayout(captureLayout);
    m_previewLayout->addWidget(m_quickInfoText, 1);

    // 默认把左侧窗口列表区域放宽，避免窗口标题被截断。
    m_mainSplitter->setStretchFactor(0, 4);
    m_mainSplitter->setStretchFactor(1, 1);

    // 底部状态栏：展示总数、可见数、系统窗口数和当前选中信息。
    m_statusBar = new QStatusBar(this);
    m_totalLabel = new QLabel(QStringLiteral("总数: 0"), m_statusBar);
    m_visibleLabel = new QLabel(QStringLiteral("可见: 0"), m_statusBar);
    m_systemLabel = new QLabel(QStringLiteral("系统: 0"), m_statusBar);
    m_selectedLabel = new QLabel(QStringLiteral("选中: -"), m_statusBar);
    m_statusBar->addWidget(m_totalLabel, 0);
    m_statusBar->addWidget(m_visibleLabel, 0);
    m_statusBar->addWidget(m_systemLabel, 0);
    m_statusBar->addPermanentWidget(m_selectedLabel, 1);
    m_rootLayout->addWidget(m_statusBar, 0);

    // 自动刷新定时器：由勾选框控制启动和停止。
    m_autoRefreshTimer = new QTimer(this);
    m_autoRefreshTimer->setInterval(m_autoRefreshIntervalSpin->value());
}

void OtherDock::initializeConnections()
{
    // 手动刷新：触发后台枚举。
    connect(m_refreshButton, &QPushButton::clicked, this, [this]() {
        kLogEvent event;
        info << event
            << "[OtherDock] 用户点击刷新窗口列表。"
            << eol;
        refreshWindowListAsync();
    });

    // 自动刷新开关：勾选后按间隔周期刷新。
    connect(m_autoRefreshCheck, &QCheckBox::toggled, this, [this](bool checked) {
        kLogEvent event;
        info << event
            << "[OtherDock] 自动刷新状态切换, enabled="
            << (checked ? "true" : "false")
            << ", intervalMs="
            << m_autoRefreshIntervalSpin->value()
            << eol;
        if (checked)
        {
            m_autoRefreshTimer->start(m_autoRefreshIntervalSpin->value());
        }
        else
        {
            m_autoRefreshTimer->stop();
        }
    });

    // 自动刷新间隔变化时，立即更新正在运行的定时器。
    connect(m_autoRefreshIntervalSpin, &QSpinBox::valueChanged, this, [this](int value) {
        m_autoRefreshTimer->setInterval(value);
        kLogEvent event;
        info << event
            << "[OtherDock] 自动刷新间隔更新, intervalMs="
            << value
            << ", timerActive="
            << (m_autoRefreshTimer->isActive() ? "true" : "false")
            << eol;
        if (m_autoRefreshCheck->isChecked())
        {
            m_autoRefreshTimer->start(value);
        }
    });

    // 过滤、分组、视图切换均可直接重绘当前快照，无需重新枚举。
    connect(m_filterEdit, &QLineEdit::textChanged, this, [this](const QString& text) {
        kLogEvent event;
        dbg << event
            << "[OtherDock] 关键字过滤变更, keyword="
            << text.toStdString()
            << eol;
        rebuildWindowTreeFromSnapshot();
    });
    connect(m_filterModeCombo, &QComboBox::currentIndexChanged, this, [this](int index) {
        kLogEvent event;
        info << event
            << "[OtherDock] 过滤模式切换, mode="
            << filterModeText(index).toStdString()
            << eol;
        rebuildWindowTreeFromSnapshot();
    });
    connect(m_groupModeCombo, &QComboBox::currentIndexChanged, this, [this](int index) {
        kLogEvent event;
        info << event
            << "[OtherDock] 分组模式切换, mode="
            << groupModeText(index).toStdString()
            << eol;
        rebuildWindowTreeFromSnapshot();
    });
    connect(m_viewModeCombo, &QComboBox::currentIndexChanged, this, [this](int index) {
        kLogEvent event;
        info << event
            << "[OtherDock] 视图模式切换, mode="
            << viewModeText(index).toStdString()
            << eol;
        applyViewMode();
    });

    // 导出当前可见行。
    connect(m_exportButton, &QPushButton::clicked, this, [this]() {
        kLogEvent event;
        info << event
            << "[OtherDock] 用户点击导出窗口列表。"
            << eol;
        exportVisibleRowsToTsv();
    });

    // 自动刷新定时器回调。
    connect(m_autoRefreshTimer, &QTimer::timeout, this, [this]() {
        kLogEvent event;
        dbg << event
            << "[OtherDock] 自动刷新定时器触发。"
            << eol;
        refreshWindowListAsync();
    });

    // 右键菜单：窗口操作入口。
    connect(m_windowTree, &QTreeWidget::customContextMenuRequested, this, [this](const QPoint& pos) {
        showWindowContextMenu(pos);
    });

    // 双击行：直接打开窗口详细信息。
    connect(m_windowTree, &QTreeWidget::itemDoubleClicked, this, [this](QTreeWidgetItem* item, int) {
        if (item == nullptr || item->data(0, Qt::UserRole + 1).toBool())
        {
            return;
        }
        const quint64 hwndValue = item->data(0, Qt::UserRole).toULongLong();
        const WindowInfo* windowInfo = findInfoByHwnd(hwndValue);
        if (windowInfo != nullptr)
        {
            kLogEvent event;
            info << event
                << "[OtherDock] 双击窗口项，打开详情, hwnd="
                << hwndToText(windowInfo->hwndValue).toStdString()
                << ", pid="
                << windowInfo->processId
                << eol;
            openWindowDetailDialog(*windowInfo);
        }
    });

    // 选中项变化：更新右侧预览与状态栏“选中”文本。
    connect(m_windowTree, &QTreeWidget::currentItemChanged, this, [this](QTreeWidgetItem* current, QTreeWidgetItem*) {
        const bool isLeaf = current != nullptr && !current->data(0, Qt::UserRole + 1).toBool();
        if (!isLeaf)
        {
            updatePreviewPanel(nullptr);
            m_selectedLabel->setText(QStringLiteral("选中: -"));
            return;
        }

        const quint64 hwndValue = current->data(0, Qt::UserRole).toULongLong();
        const WindowInfo* windowInfo = findInfoByHwnd(hwndValue);
        updatePreviewPanel(windowInfo);
        if (windowInfo != nullptr)
        {
            m_selectedLabel->setText(QStringLiteral("选中: %1  %2")
                .arg(hwndToText(windowInfo->hwndValue), windowInfo->titleText));
            kLogEvent event;
            dbg << event
                << "[OtherDock] 选中窗口变更, hwnd="
                << hwndToText(windowInfo->hwndValue).toStdString()
                << ", title="
                << windowInfo->titleText.toStdString()
                << eol;
        }
    });

    // 截图按钮：把当前选中窗口抓图并保存到文件。
    connect(m_captureButton, &QPushButton::clicked, this, [this]() {
        QTreeWidgetItem* item = m_windowTree->currentItem();
        if (item == nullptr || item->data(0, Qt::UserRole + 1).toBool())
        {
            kLogEvent event;
            warn << event
                << "[OtherDock] 截图失败：未选中有效窗口。"
                << eol;
            QMessageBox::information(this, QStringLiteral("窗口截图"), QStringLiteral("请先选中一个窗口。"));
            return;
        }

        const quint64 hwndValue = item->data(0, Qt::UserRole).toULongLong();
        const QPixmap pixmap = grabWindowPreviewPixmap(hwndValue, QSize(900, 600));
        if (pixmap.isNull())
        {
            kLogEvent event;
            err << event
                << "[OtherDock] 截图抓取失败, hwnd="
                << hwndToText(hwndValue).toStdString()
                << eol;
            QMessageBox::warning(this, QStringLiteral("窗口截图"), QStringLiteral("抓图失败。"));
            return;
        }

        const QString defaultName = QStringLiteral("window_capture_%1_%2.png")
            .arg(QString::number(hwndValue, 16).toUpper())
            .arg(QDateTime::currentDateTime().toString(QStringLiteral("yyyyMMdd_HHmmss")));
        const QString savePath = QFileDialog::getSaveFileName(
            this,
            QStringLiteral("保存窗口截图"),
            defaultName,
            QStringLiteral("PNG 图片 (*.png)"));
        if (savePath.trimmed().isEmpty())
        {
            kLogEvent event;
            dbg << event
                << "[OtherDock] 截图保存取消, hwnd="
                << hwndToText(hwndValue).toStdString()
                << eol;
            return;
        }

        if (!pixmap.save(savePath))
        {
            kLogEvent event;
            err << event
                << "[OtherDock] 截图保存失败, hwnd="
                << hwndToText(hwndValue).toStdString()
                << ", path="
                << savePath.toStdString()
                << eol;
            QMessageBox::warning(this, QStringLiteral("窗口截图"), QStringLiteral("保存失败：%1").arg(savePath));
            return;
        }
        kLogEvent event;
        info << event
            << "[OtherDock] 截图保存成功, hwnd="
            << hwndToText(hwndValue).toStdString()
            << ", path="
            << savePath.toStdString()
            << eol;
        QMessageBox::information(this, QStringLiteral("窗口截图"), QStringLiteral("截图已保存：%1").arg(savePath));
    });
}

void OtherDock::applyViewMode()
{
    // 图标/列表/详情模式通过列显隐与装饰样式切换实现。
    const int mode = m_viewModeCombo->currentIndex();
    if (mode == 0)
    {
        // 图标视图：保留关键列，降低信息密度。
        for (int col = 0; col < m_windowTree->columnCount(); ++col)
        {
            const bool keepVisible =
                (col == kWindowColumnTitle
                    || col == kWindowColumnHandle
                    || col == kWindowColumnProcessName);
            m_windowTree->setColumnHidden(col, !keepVisible);
        }
        m_windowTree->setIconSize(QSize(20, 20));
    }
    else if (mode == 1)
    {
        // 列表视图：比图标模式多保留 PID、状态、可见性。
        for (int col = 0; col < m_windowTree->columnCount(); ++col)
        {
            const bool keepVisible =
                (col == kWindowColumnTitle
                    || col == kWindowColumnHandle
                    || col == kWindowColumnPid
                    || col == kWindowColumnProcessName
                    || col == kWindowColumnState);
            m_windowTree->setColumnHidden(col, !keepVisible);
        }
        m_windowTree->setIconSize(QSize(16, 16));
    }
    else
    {
        // 详细信息视图：显示所有列，满足审计场景。
        for (int col = 0; col < m_windowTree->columnCount(); ++col)
        {
            m_windowTree->setColumnHidden(col, false);
        }
        m_windowTree->setIconSize(QSize(16, 16));
    }

    // 记录当前视图策略，便于用户反馈“列突然消失”时快速定位。
    kLogEvent event;
    info << event
        << "[OtherDock] 应用视图模式, mode="
        << viewModeText(mode).toStdString()
        << eol;
}

void OtherDock::refreshWindowListAsync()
{
    // 防并发刷新：上一轮未完成时直接忽略当前请求。
    if (m_refreshRunning.exchange(true))
    {
        kLogEvent event;
        dbg << event
            << "[OtherDock] 跳过刷新：已有枚举任务在执行。"
            << eol;
        return;
    }

    kLogEvent startEvent;
    info << startEvent
        << "[OtherDock] 启动异步窗口枚举, filterMode="
        << filterModeText(m_filterModeCombo->currentIndex()).toStdString()
        << ", groupMode="
        << groupModeText(m_groupModeCombo->currentIndex()).toStdString()
        << ", keyword="
        << m_filterEdit->text().toStdString()
        << eol;

    if (m_refreshProgressPid == 0)
    {
        m_refreshProgressPid = kPro.add("窗口", "窗口枚举");
    }
    kPro.set(m_refreshProgressPid, "开始枚举窗口", 0, 5.0f);

    QPointer<OtherDock> guardThis(this);
    std::thread([guardThis]() {
        std::vector<WindowInfo> snapshot;
        snapshot.reserve(512);

        EnumContext context;
        context.outputList = &snapshot;
        context.zOrderCounter = 0;
        ::EnumWindows(enumTopWindowProc, reinterpret_cast<LPARAM>(&context));

        if (guardThis == nullptr)
        {
            return;
        }
        QMetaObject::invokeMethod(qApp, [guardThis, snapshot = std::move(snapshot)]() mutable {
            if (guardThis == nullptr)
            {
                return;
            }

            kPro.set(guardThis->m_refreshProgressPid, "合并窗口快照", 0, 70.0f);

            // 记录上一轮有效窗口，作为“新增/退出”对比基线。
            std::vector<WindowInfo> previousValidList;
            previousValidList.reserve(guardThis->m_windowSnapshot.size());
            for (const WindowInfo& info : guardThis->m_windowSnapshot)
            {
                if (info.valid)
                {
                    previousValidList.push_back(info);
                }
            }

            std::unordered_set<quint64> currentHandleSet;
            currentHandleSet.reserve(snapshot.size());
            for (const WindowInfo& info : snapshot)
            {
                currentHandleSet.insert(info.hwndValue);
            }

            std::unordered_set<quint64> previousHandleSet;
            previousHandleSet.reserve(previousValidList.size());
            for (const WindowInfo& info : previousValidList)
            {
                previousHandleSet.insert(info.hwndValue);
            }

            // 计算新增窗口，用于绿色高亮。
            guardThis->m_newWindowHandles.clear();
            for (const WindowInfo& info : snapshot)
            {
                if (previousHandleSet.find(info.hwndValue) == previousHandleSet.end())
                {
                    guardThis->m_newWindowHandles.push_back(info.hwndValue);
                }
            }

            // 计算退出窗口：保留一轮并置灰显示。
            guardThis->m_exitedOneRound.clear();
            for (const WindowInfo& oldInfo : previousValidList)
            {
                if (currentHandleSet.find(oldInfo.hwndValue) != currentHandleSet.end())
                {
                    continue;
                }

                WindowInfo exitedInfo = oldInfo;
                exitedInfo.valid = false;
                exitedInfo.visible = false;
                exitedInfo.enabled = false;
                exitedInfo.topMost = false;
                exitedInfo.minimized = false;
                exitedInfo.maximized = false;
                exitedInfo.titleText = QStringLiteral("[已退出] %1").arg(oldInfo.titleText);
                guardThis->m_exitedOneRound.push_back(exitedInfo);
            }

            // 当前显示列表 = 当前快照 + 退出保留一轮。
            guardThis->m_previousSnapshot = snapshot;
            guardThis->m_windowSnapshot = snapshot;
            guardThis->m_windowSnapshot.insert(
                guardThis->m_windowSnapshot.end(),
                guardThis->m_exitedOneRound.begin(),
                guardThis->m_exitedOneRound.end());

            guardThis->rebuildWindowTreeFromSnapshot();
            guardThis->updateStatusBar();
            guardThis->m_refreshRunning.store(false);
            kPro.set(guardThis->m_refreshProgressPid, "窗口枚举完成", 0, 100.0f);

            kLogEvent event;
            info << event
                << "[OtherDock] 枚举完成，当前窗口="
                << snapshot.size()
                << ", 退出保留="
                << guardThis->m_exitedOneRound.size()
                << eol;
        }, Qt::QueuedConnection);
    }).detach();
}

bool OtherDock::passFilter(const WindowInfo& info) const
{
    // 条件过滤：先按下拉条件筛掉不符合的项。
    const int mode = m_filterModeCombo->currentIndex();
    switch (mode)
    {
    case 0:
        if (!info.visible || !info.valid)
        {
            return false;
        }
        break;
    case 1:
        if (info.visible || !info.valid)
        {
            return false;
        }
        break;
    case 2:
        if (info.isChildWindow || !info.valid)
        {
            return false;
        }
        break;
    case 3:
        if (!info.isChildWindow || !info.valid)
        {
            return false;
        }
        break;
    case 4:
        if (info.valid)
        {
            return false;
        }
        break;
    default:
        break;
    }

    // 关键字过滤：支持标题、类名、进程名和句柄文本。
    const QString keyword = m_filterEdit->text().trimmed();
    if (keyword.isEmpty())
    {
        return true;
    }

    const QString keywordLower = keyword.toLower();
    const QString joinedText = QStringLiteral("%1 %2 %3 %4 %5")
        .arg(info.titleText)
        .arg(info.classNameText)
        .arg(info.processNameText)
        .arg(info.processId)
        .arg(hwndToText(info.hwndValue))
        .toLower();
    return joinedText.contains(keywordLower);
}

void OtherDock::rebuildWindowTreeFromSnapshot()
{
    kLogEvent startEvent;
    dbg << startEvent
        << "[OtherDock] 重建窗口树开始, snapshotCount="
        << m_windowSnapshot.size()
        << ", filterMode="
        << filterModeText(m_filterModeCombo->currentIndex()).toStdString()
        << ", groupMode="
        << groupModeText(m_groupModeCombo->currentIndex()).toStdString()
        << eol;

    m_windowTree->clear();

    // 进程图标缓存：
    // - 以 PID 作为 key，避免同一进程重复提取图标；
    // - 缓存命中可显著降低刷新时的图标查询开销。
    std::unordered_map<std::uint32_t, QIcon> processLogoCache;
    auto resolveProcessLogo = [&processLogoCache](const WindowInfo& info) -> QIcon {
        const auto found = processLogoCache.find(info.processId);
        if (found != processLogoCache.end())
        {
            return found->second;
        }
        const QIcon processLogo = queryProcessLogoIconByPath(info.processImagePathText);
        processLogoCache.emplace(info.processId, processLogo);
        return processLogo;
    };

    // 公共叶子节点生成函数：统一填充列与高亮规则。
    auto appendLeafItem = [this, &resolveProcessLogo](QTreeWidgetItem* parent, const WindowInfo& info) {
        QTreeWidgetItem* item = nullptr;
        if (parent != nullptr)
        {
            item = new QTreeWidgetItem(parent);
        }
        else
        {
            item = new QTreeWidgetItem(m_windowTree);
        }

        const QString sizeText = QStringLiteral("%1 x %2")
            .arg(info.windowRect.width())
            .arg(info.windowRect.height());

        item->setText(kWindowColumnTitle, info.titleText.isEmpty() ? QStringLiteral("<无标题>") : info.titleText);
        item->setText(kWindowColumnHandle, hwndToText(info.hwndValue));
        item->setText(kWindowColumnEnumApi, info.enumApiName);
        item->setText(kWindowColumnClassName, info.classNameText);
        item->setText(kWindowColumnPid, QString::number(info.processId));
        item->setText(kWindowColumnProcessName, info.processNameText);
        item->setText(kWindowColumnTid, QString::number(info.threadId));
        item->setText(kWindowColumnSize, sizeText);
        item->setText(kWindowColumnVisible, boolText(info.visible));
        item->setText(kWindowColumnEnabled, boolText(info.enabled));
        item->setText(kWindowColumnTopMost, boolText(info.topMost));
        item->setText(kWindowColumnState, windowStateText(info.valid, info.minimized, info.maximized));
        item->setText(kWindowColumnAlpha, QString::number(info.alphaValue));

        item->setData(kWindowColumnTitle, Qt::UserRole, QVariant::fromValue(static_cast<qulonglong>(info.hwndValue)));
        item->setData(kWindowColumnTitle, Qt::UserRole + 1, false);

        // 叶子图标改为显示“进程 logo”，并放到进程列，避免标题列图标干扰阅读。
        item->setIcon(kWindowColumnProcessName, resolveProcessLogo(info));

        // 新增窗口高亮绿色，退出窗口置灰。
        const bool isNew = std::find(
            m_newWindowHandles.begin(),
            m_newWindowHandles.end(),
            info.hwndValue) != m_newWindowHandles.end();

        if (!info.valid)
        {
            for (int col = 0; col < m_windowTree->columnCount(); ++col)
            {
                item->setForeground(col, QBrush(QColor(128, 128, 128)));
                item->setBackground(col, QBrush(QColor(238, 238, 238)));
            }
        }
        else if (isNew)
        {
            for (int col = 0; col < m_windowTree->columnCount(); ++col)
            {
                item->setBackground(col, QBrush(QColor(224, 245, 224)));
            }
        }
    };

    // 根据分组选项重建树结构。
    const int groupMode = m_groupModeCombo->currentIndex();

    if (groupMode == 0)
    {
        // 按进程分组：进程节点下挂对应窗口子节点。
        std::map<QString, QTreeWidgetItem*> processMap;
        for (const WindowInfo& info : m_windowSnapshot)
        {
            if (!passFilter(info))
            {
                continue;
            }

            const QString processKey = QStringLiteral("%1 (PID:%2)")
                .arg(info.processNameText, QString::number(info.processId));
            QTreeWidgetItem* groupItem = nullptr;
            auto found = processMap.find(processKey);
            if (found == processMap.end())
            {
                groupItem = new QTreeWidgetItem(m_windowTree);
                groupItem->setText(0, processKey);
                // 进程分组节点同样展示进程 logo，满足“所有进程都显示图标”的需求。
                groupItem->setIcon(0, resolveProcessLogo(info));
                groupItem->setData(0, Qt::UserRole + 1, true);
                processMap.emplace(processKey, groupItem);
            }
            else
            {
                groupItem = found->second;
            }
            appendLeafItem(groupItem, info);
        }
    }
    else if (groupMode == 1)
    {
        // 按 Z 序：平铺显示，按枚举顺序排序。
        std::vector<WindowInfo> orderedList = m_windowSnapshot;
        std::sort(
            orderedList.begin(),
            orderedList.end(),
            [](const WindowInfo& left, const WindowInfo& right) {
                return left.zOrder < right.zOrder;
            });
        for (const WindowInfo& info : orderedList)
        {
            if (!passFilter(info))
            {
                continue;
            }
            appendLeafItem(nullptr, info);
        }
    }
    else if (groupMode == 2)
    {
        // 按层级：桌面/顶层/子窗口/弹出窗口/工具窗口分类。
        std::map<QString, QTreeWidgetItem*> levelGroupMap;
        auto getLevelGroup = [this, &levelGroupMap](const QString& key) -> QTreeWidgetItem* {
            auto found = levelGroupMap.find(key);
            if (found != levelGroupMap.end())
            {
                return found->second;
            }
            QTreeWidgetItem* item = new QTreeWidgetItem(m_windowTree);
            item->setText(0, key);
            item->setIcon(0, QIcon(":/Icon/process_tree.svg"));
            item->setData(0, Qt::UserRole + 1, true);
            levelGroupMap.emplace(key, item);
            return item;
        };

        for (const WindowInfo& info : m_windowSnapshot)
        {
            if (!passFilter(info))
            {
                continue;
            }

            QString groupKey = QStringLiteral("顶级窗口");
            if (info.hwndValue == static_cast<quint64>(reinterpret_cast<quintptr>(::GetDesktopWindow())))
            {
                groupKey = QStringLiteral("桌面窗口");
            }
            else if (info.isChildWindow)
            {
                groupKey = QStringLiteral("子窗口");
            }
            else if ((info.styleValue & WS_POPUP) != 0)
            {
                groupKey = QStringLiteral("弹出窗口");
            }
            else if ((info.exStyleValue & WS_EX_TOOLWINDOW) != 0)
            {
                groupKey = QStringLiteral("工具窗口");
            }

            appendLeafItem(getLevelGroup(groupKey), info);
        }
    }
    else
    {
        // 按显示器：依据窗口中心点所在屏幕分组。
        std::map<QString, QTreeWidgetItem*> monitorGroupMap;
        const QList<QScreen*> screenList = QGuiApplication::screens();

        auto getMonitorGroup = [this, &monitorGroupMap](const QString& key) -> QTreeWidgetItem* {
            auto found = monitorGroupMap.find(key);
            if (found != monitorGroupMap.end())
            {
                return found->second;
            }
            QTreeWidgetItem* item = new QTreeWidgetItem(m_windowTree);
            item->setText(0, key);
            item->setIcon(0, QIcon(":/Icon/process_list.svg"));
            item->setData(0, Qt::UserRole + 1, true);
            monitorGroupMap.emplace(key, item);
            return item;
        };

        for (const WindowInfo& info : m_windowSnapshot)
        {
            if (!passFilter(info))
            {
                continue;
            }

            QString groupKey = QStringLiteral("未知显示器");
            const QPoint centerPoint = info.windowRect.center();
            for (int i = 0; i < screenList.size(); ++i)
            {
                QScreen* screen = screenList.at(i);
                if (screen != nullptr && screen->geometry().contains(centerPoint))
                {
                    groupKey = QStringLiteral("显示器%1: %2")
                        .arg(i + 1)
                        .arg(screen->name());
                    break;
                }
            }
            appendLeafItem(getMonitorGroup(groupKey), info);
        }
    }

    // 统一展开第一层节点，便于用户直接看到数据。
    for (int i = 0; i < m_windowTree->topLevelItemCount(); ++i)
    {
        QTreeWidgetItem* topItem = m_windowTree->topLevelItem(i);
        if (topItem != nullptr && topItem->data(0, Qt::UserRole + 1).toBool())
        {
            topItem->setExpanded(true);
        }
    }

    kLogEvent finishEvent;
    dbg << finishEvent
        << "[OtherDock] 重建窗口树完成, topLevelCount="
        << m_windowTree->topLevelItemCount()
        << eol;
}

void OtherDock::updateStatusBar()
{
    int totalCount = 0;
    int visibleCount = 0;
    int systemCount = 0;
    for (const WindowInfo& info : m_windowSnapshot)
    {
        if (!info.valid)
        {
            continue;
        }
        ++totalCount;
        if (info.visible)
        {
            ++visibleCount;
        }
        if (info.processId <= 4
            || info.processNameText.compare(QStringLiteral("System"), Qt::CaseInsensitive) == 0
            || info.processNameText.compare(QStringLiteral("csrss.exe"), Qt::CaseInsensitive) == 0)
        {
            ++systemCount;
        }
    }

    m_totalLabel->setText(QStringLiteral("总数: %1").arg(totalCount));
    m_visibleLabel->setText(QStringLiteral("可见: %1").arg(visibleCount));
    m_systemLabel->setText(QStringLiteral("系统: %1").arg(systemCount));

    kLogEvent event;
    dbg << event
        << "[OtherDock] 状态栏更新, total="
        << totalCount
        << ", visible="
        << visibleCount
        << ", system="
        << systemCount
        << eol;
}

void OtherDock::updatePreviewPanel(const WindowInfo* info)
{
    // 无选中项时清空预览，防止显示过期信息。
    if (info == nullptr)
    {
        m_thumbnailLabel->setPixmap(QPixmap());
        m_quickInfoText->setPlainText(QStringLiteral("请选择左侧窗口项查看预览。"));
        kLogEvent event;
        dbg << event
            << "[OtherDock] 预览面板清空（无有效选中项）。"
            << eol;
        return;
    }

    // 抓取缩略图并更新右侧关键属性摘要。
    const QPixmap previewPixmap = grabWindowPreviewPixmap(
        info->hwndValue,
        m_thumbnailLabel->size());
    m_thumbnailLabel->setPixmap(previewPixmap);

    QString quickText;
    quickText += QStringLiteral("句柄: %1\n").arg(hwndToText(info->hwndValue));
    quickText += QStringLiteral("标题: %1\n").arg(info->titleText);
    quickText += QStringLiteral("类名: %1\n").arg(info->classNameText);
    quickText += QStringLiteral("PID/TID: %1 / %2\n").arg(info->processId).arg(info->threadId);
    quickText += QStringLiteral("进程名: %1\n").arg(info->processNameText);
    quickText += QStringLiteral("矩形: [%1,%2,%3,%4]\n")
        .arg(info->windowRect.left())
        .arg(info->windowRect.top())
        .arg(info->windowRect.right())
        .arg(info->windowRect.bottom());
    quickText += QStringLiteral("可见:%1  可用:%2  顶层:%3\n")
        .arg(boolText(info->visible))
        .arg(boolText(info->enabled))
        .arg(boolText(info->topMost));
    quickText += QStringLiteral("状态:%1  透明度:%2\n")
        .arg(windowStateText(info->valid, info->minimized, info->maximized))
        .arg(info->alphaValue);
    m_quickInfoText->setPlainText(quickText);

    kLogEvent event;
    dbg << event
        << "[OtherDock] 预览面板刷新, hwnd="
        << hwndToText(info->hwndValue).toStdString()
        << ", pid="
        << info->processId
        << eol;
}

void OtherDock::showWindowContextMenu(const QPoint& localPos)
{
    QTreeWidgetItem* item = m_windowTree->itemAt(localPos);
    if (item == nullptr || item->data(0, Qt::UserRole + 1).toBool())
    {
        return;
    }

    const quint64 hwndValue = item->data(0, Qt::UserRole).toULongLong();
    const WindowInfo* windowInfo = findInfoByHwnd(hwndValue);
    if (windowInfo == nullptr)
    {
        return;
    }

    {
        kLogEvent event;
        dbg << event
            << "[OtherDock] 打开右键菜单, hwnd="
            << hwndToText(hwndValue).toStdString()
            << ", pid="
            << windowInfo->processId
            << eol;
    }

    QMenu menu(this);
    QAction* activateAction = menu.addAction(QIcon(":/Icon/process_start.svg"), QStringLiteral("激活窗口"));
    QAction* topMostAction = menu.addAction(QIcon(":/Icon/process_priority.svg"), QStringLiteral("置顶/取消置顶"));
    QAction* showHideAction = menu.addAction(QIcon(":/Icon/process_pause.svg"), QStringLiteral("显示/隐藏"));
    QAction* enableDisableAction = menu.addAction(QIcon(":/Icon/process_uncritical.svg"), QStringLiteral("启用/禁用"));
    QAction* flashAction = menu.addAction(QIcon(":/Icon/process_refresh.svg"), QStringLiteral("闪烁窗口"));
    menu.addSeparator();
    QAction* processDetailAction = menu.addAction(QIcon(":/Icon/process_details.svg"), QStringLiteral("转到进程详细信息"));
    QAction* windowDetailAction = menu.addAction(QIcon(":/Icon/process_details.svg"), QStringLiteral("在详细信息中打开"));
    QAction* sendMessageAction = menu.addAction(QIcon(":/Icon/log_track.svg"), QStringLiteral("发送测试消息"));
    menu.addSeparator();
    QAction* terminateAction = menu.addAction(QIcon(":/Icon/process_terminate.svg"), QStringLiteral("结束进程"));

    QAction* selectedAction = menu.exec(m_windowTree->viewport()->mapToGlobal(localPos));
    if (selectedAction == nullptr)
    {
        kLogEvent event;
        dbg << event
            << "[OtherDock] 右键菜单取消, hwnd="
            << hwndToText(hwndValue).toStdString()
            << eol;
        return;
    }

    HWND windowHandle = toHwnd(windowInfo->hwndValue);

    if (selectedAction == activateAction)
    {
        kLogEvent event;
        info << event
            << "[OtherDock] 执行操作：激活窗口, hwnd="
            << hwndToText(hwndValue).toStdString()
            << eol;
        ::ShowWindow(windowHandle, SW_SHOW);
        ::SetForegroundWindow(windowHandle);
    }
    else if (selectedAction == topMostAction)
    {
        kLogEvent event;
        info << event
            << "[OtherDock] 执行操作：切换置顶, hwnd="
            << hwndToText(hwndValue).toStdString()
            << eol;
        const bool currentTopMost = (::GetWindowLongPtrW(windowHandle, GWL_EXSTYLE) & WS_EX_TOPMOST) != 0;
        ::SetWindowPos(
            windowHandle,
            currentTopMost ? HWND_NOTOPMOST : HWND_TOPMOST,
            0,
            0,
            0,
            0,
            SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
    }
    else if (selectedAction == showHideAction)
    {
        kLogEvent event;
        info << event
            << "[OtherDock] 执行操作：显示/隐藏, hwnd="
            << hwndToText(hwndValue).toStdString()
            << eol;
        const bool currentlyVisible = ::IsWindowVisible(windowHandle) != FALSE;
        ::ShowWindow(windowHandle, currentlyVisible ? SW_HIDE : SW_SHOW);
    }
    else if (selectedAction == enableDisableAction)
    {
        kLogEvent event;
        info << event
            << "[OtherDock] 执行操作：启用/禁用, hwnd="
            << hwndToText(hwndValue).toStdString()
            << eol;
        const bool currentlyEnabled = ::IsWindowEnabled(windowHandle) != FALSE;
        ::EnableWindow(windowHandle, currentlyEnabled ? FALSE : TRUE);
    }
    else if (selectedAction == flashAction)
    {
        kLogEvent event;
        info << event
            << "[OtherDock] 执行操作：闪烁窗口, hwnd="
            << hwndToText(hwndValue).toStdString()
            << eol;
        FLASHWINFO flashInfo{};
        flashInfo.cbSize = sizeof(flashInfo);
        flashInfo.hwnd = windowHandle;
        flashInfo.dwFlags = FLASHW_ALL | FLASHW_TIMERNOFG;
        flashInfo.uCount = 3;
        flashInfo.dwTimeout = 0;
        ::FlashWindowEx(&flashInfo);
    }
    else if (selectedAction == processDetailAction)
    {
        kLogEvent event;
        info << event
            << "[OtherDock] 执行操作：打开进程详情, pid="
            << windowInfo->processId
            << eol;
        openProcessDetailWindow(this, windowInfo->processId);
    }
    else if (selectedAction == windowDetailAction)
    {
        kLogEvent event;
        info << event
            << "[OtherDock] 执行操作：打开窗口详情, hwnd="
            << hwndToText(hwndValue).toStdString()
            << eol;
        openWindowDetailDialog(*windowInfo);
    }
    else if (selectedAction == sendMessageAction)
    {
        kLogEvent event;
        dbg << event
            << "[OtherDock] 执行操作：发送测试消息, hwnd="
            << hwndToText(hwndValue).toStdString()
            << eol;
        DWORD_PTR resultValue = 0;
        ::SendMessageTimeoutW(
            windowHandle,
            WM_NULL,
            0,
            0,
            SMTO_ABORTIFHUNG,
            1200,
            &resultValue);
        Q_UNUSED(resultValue);
    }
    else if (selectedAction == terminateAction)
    {
        kLogEvent event;
        warn << event
            << "[OtherDock] 执行操作：结束进程确认, pid="
            << windowInfo->processId
            << eol;
        const QMessageBox::StandardButton userChoice = QMessageBox::question(
            this,
            QStringLiteral("结束进程"),
            QStringLiteral("确定结束 PID=%1 (%2) ?")
                .arg(windowInfo->processId)
                .arg(windowInfo->processNameText),
            QMessageBox::Yes | QMessageBox::No,
            QMessageBox::No);
        if (userChoice == QMessageBox::Yes)
        {
            HANDLE processHandle = ::OpenProcess(PROCESS_TERMINATE, FALSE, windowInfo->processId);
            if (processHandle == nullptr)
            {
                kLogEvent openEvent;
                err << openEvent
                    << "[OtherDock] 结束进程失败：OpenProcess失败, pid="
                    << windowInfo->processId
                    << eol;
                QMessageBox::warning(this, QStringLiteral("结束进程"), QStringLiteral("打开进程失败，可能权限不足。"));
            }
            else
            {
                const BOOL terminateOk = ::TerminateProcess(processHandle, 0);
                ::CloseHandle(processHandle);
                if (terminateOk == FALSE)
                {
                    kLogEvent terminateEvent;
                    err << terminateEvent
                        << "[OtherDock] 结束进程失败：TerminateProcess失败, pid="
                        << windowInfo->processId
                        << eol;
                    QMessageBox::warning(this, QStringLiteral("结束进程"), QStringLiteral("TerminateProcess 调用失败。"));
                }
                else
                {
                    kLogEvent terminateEvent;
                    warn << terminateEvent
                        << "[OtherDock] 结束进程成功, pid="
                        << windowInfo->processId
                        << eol;
                }
            }
        }
        else
        {
            kLogEvent event;
            dbg << event
                << "[OtherDock] 结束进程操作已取消, pid="
                << windowInfo->processId
                << eol;
        }
    }

    // 任意状态类动作后都刷新一次，确保列表与真实状态一致。
    refreshWindowListAsync();
}

void OtherDock::exportVisibleRowsToTsv()
{
    // 遍历当前树，导出叶子节点；分组节点不参与导出。
    QStringList lines;

    QStringList header;
    for (int col = 0; col < m_windowTree->columnCount(); ++col)
    {
        if (m_windowTree->isColumnHidden(col))
        {
            continue;
        }
        header << m_windowTree->headerItem()->text(col);
    }
    lines << header.join('\t');

    QTreeWidgetItemIterator iterator(m_windowTree);
    while (*iterator != nullptr)
    {
        QTreeWidgetItem* item = *iterator;
        ++iterator;

        if (item->data(0, Qt::UserRole + 1).toBool())
        {
            continue;
        }

        QStringList rowValues;
        for (int col = 0; col < m_windowTree->columnCount(); ++col)
        {
            if (m_windowTree->isColumnHidden(col))
            {
                continue;
            }
            rowValues << item->text(col).replace('\t', ' ');
        }
        lines << rowValues.join('\t');
    }

    if (lines.size() <= 1)
    {
        kLogEvent event;
        dbg << event
            << "[OtherDock] 导出取消：当前无可见数据行。"
            << eol;
        QMessageBox::information(this, QStringLiteral("导出窗口列表"), QStringLiteral("没有可导出的行。"));
        return;
    }

    const QString defaultName = QStringLiteral("window_list_%1.tsv")
        .arg(QDateTime::currentDateTime().toString(QStringLiteral("yyyyMMdd_HHmmss")));
    const QString outputPath = QFileDialog::getSaveFileName(
        this,
        QStringLiteral("导出窗口列表"),
        defaultName,
        QStringLiteral("TSV文件 (*.tsv);;文本文件 (*.txt)"));
    if (outputPath.trimmed().isEmpty())
    {
        kLogEvent event;
        dbg << event
            << "[OtherDock] 导出取消：用户未选择输出路径。"
            << eol;
        return;
    }

    QFile file(outputPath);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Text | QIODevice::Truncate))
    {
        kLogEvent event;
        err << event
            << "[OtherDock] 导出失败：无法写入文件, path="
            << outputPath.toStdString()
            << eol;
        QMessageBox::warning(this, QStringLiteral("导出窗口列表"), QStringLiteral("无法写入文件：%1").arg(outputPath));
        return;
    }

    QTextStream out(&file);
    out.setEncoding(QStringConverter::Utf8);
    for (const QString& line : lines)
    {
        out << line << '\n';
    }
    file.close();

    kLogEvent event;
    info << event << "[OtherDock] 导出窗口列表成功:" << outputPath.toStdString() << eol;
    QMessageBox::information(this, QStringLiteral("导出窗口列表"), QStringLiteral("导出完成：%1").arg(outputPath));
}

void OtherDock::openWindowDetailDialog(const WindowInfo& windowInfo)
{
    kLogEvent event;
    info << event
        << "[OtherDock] 打开窗口详情对话框, hwnd="
        << hwndToText(windowInfo.hwndValue).toStdString()
        << ", pid="
        << windowInfo.processId
        << eol;

    WindowDetailDialog* dialog = new WindowDetailDialog(windowInfo, this);
    dialog->show();
    dialog->raise();
    dialog->activateWindow();
}

const OtherDock::WindowInfo* OtherDock::findInfoByHwnd(const quint64 hwndValue) const
{
    const auto finder = [hwndValue](const WindowInfo& info) {
        return info.hwndValue == hwndValue;
    };

    const auto currentIt = std::find_if(
        m_windowSnapshot.begin(),
        m_windowSnapshot.end(),
        finder);
    if (currentIt != m_windowSnapshot.end())
    {
        return &(*currentIt);
    }

    const auto exitIt = std::find_if(
        m_exitedOneRound.begin(),
        m_exitedOneRound.end(),
        finder);
    if (exitIt != m_exitedOneRound.end())
    {
        return &(*exitIt);
    }

    return nullptr;
}
