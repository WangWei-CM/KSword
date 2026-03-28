#include "ProcessDock.h"

#include "../theme.h"
#include "ProcessDetailWindow.h"

#include <QAbstractItemView>
#include <QApplication>
#include <QCheckBox>
#include <QClipboard>
#include <QComboBox>
#include <QCursor>
#include <QDateTime>
#include <QFileDialog>
#include <QFileIconProvider>
#include <QFileInfo>
#include <QFormLayout>
#include <QFrame>
#include <QGridLayout>
#include <QGroupBox>
#include <QHeaderView>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QMessageBox>
#include <QMetaObject>
#include <QPainter>
#include <QPlainTextEdit>
#include <QPointer>
#include <QPushButton>
#include <QPixmap>
#include <QRunnable>
#include <QResizeEvent>
#include <QScrollArea>
#include <QSignalBlocker>
#include <QSlider>
#include <QSvgRenderer>
#include <QTableWidget>
#include <QTableWidgetItem>
#include <QTabWidget>
#include <QTextEdit>
#include <QThreadPool>
#include <QTimer>
#include <QStringList>
#include <QTreeWidget>
#include <QTreeWidgetItem>
#include <QVBoxLayout>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <functional>
#include <limits>
#include <sstream>
#include <set>
#include <thread>
#include <unordered_set>

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>

namespace
{
    // 列标题文本常量，索引与 ProcessDock::TableColumn 一一对应。
    const QStringList ProcessTableHeaders{
        "进程名",
        "PID",
        "CPU",
        "RAM",
        "DISK",
        "GPU",
        "Net",
        "数字签名",
        "路径",
        "父进程",
        "启动参数",
        "用户",
        "启动时间",
        "管理员"
    };

    // 常用图标路径常量（全部来自 qrc 的 /Icon 前缀资源）。
    constexpr const char* IconProcessMain = ":/Icon/process_main.svg";
    constexpr const char* IconRefresh = ":/Icon/process_refresh.svg";
    constexpr const char* IconTree = ":/Icon/process_tree.svg";
    constexpr const char* IconList = ":/Icon/process_list.svg";
    constexpr const char* IconStart = ":/Icon/process_start.svg";
    constexpr const char* IconPause = ":/Icon/process_pause.svg";

    // 默认按钮图标尺寸。
    constexpr QSize DefaultIconSize(16, 16);
    constexpr QSize CompactIconButtonSize(28, 28);

    // 当前 steady_clock 时间转 100ns（与 ks::process 差值计算规则保持一致）。
    std::uint64_t steadyNow100ns()
    {
        const auto nowDuration = std::chrono::steady_clock::now().time_since_epoch();
        const auto nowNanoseconds = std::chrono::duration_cast<std::chrono::nanoseconds>(nowDuration).count();
        return static_cast<std::uint64_t>(nowNanoseconds / 100);
    }

    // 从策略下拉索引映射到 ks::process 策略枚举。
    ks::process::ProcessEnumStrategy toStrategy(const int strategyIndex)
    {
        switch (strategyIndex)
        {
        case 0:
            return ks::process::ProcessEnumStrategy::SnapshotProcess32;
        case 1:
            return ks::process::ProcessEnumStrategy::NtQuerySystemInfo;
        default:
            return ks::process::ProcessEnumStrategy::NtQuerySystemInfo;
        }
    }

    // 策略枚举转可读文本：用于刷新状态标签与详细日志输出。
    const char* strategyToText(const ks::process::ProcessEnumStrategy strategy)
    {
        switch (strategy)
        {
        case ks::process::ProcessEnumStrategy::SnapshotProcess32:
            return "Toolhelp Snapshot + Process32First/Next";
        case ks::process::ProcessEnumStrategy::NtQuerySystemInfo:
            return "NtQuerySystemInformation";
        case ks::process::ProcessEnumStrategy::Auto:
            return "Auto (NtQuery 优先, 失败回退 Toolhelp)";
        default:
            return "Unknown";
        }
    }

    // usageRatioToHighlightColor 作用：
    // - 按占用比例（0~1）返回主题蓝色透明高亮；
    // - 占用越高，alpha 越大，视觉上更“深”。
    QColor usageRatioToHighlightColor(double usageRatio)
    {
        usageRatio = std::clamp(usageRatio, 0.0, 1.0);
        const int alphaValue = static_cast<int>(24.0 + usageRatio * 146.0);
        QColor highlightColor = KswordTheme::PrimaryBlueColor;
        highlightColor.setAlpha(alphaValue);
        return highlightColor;
    }

    // hasDetailWindowSignificantChange 作用：
    // - 判断两轮进程记录是否存在“需要立刻同步到详情窗口”的显著变化；
    // - 通过阈值过滤掉轻微抖动，减少刷新期间 UI 卡顿。
    bool hasDetailWindowSignificantChange(
        const ks::process::ProcessRecord& oldRecord,
        const ks::process::ProcessRecord& newRecord)
    {
        if (oldRecord.pid != newRecord.pid ||
            oldRecord.creationTime100ns != newRecord.creationTime100ns)
        {
            return true;
        }

        if (std::fabs(oldRecord.cpuPercent - newRecord.cpuPercent) >= 4.0)
        {
            return true;
        }
        if (std::fabs(oldRecord.ramMB - newRecord.ramMB) >= 16.0)
        {
            return true;
        }
        if (std::fabs(oldRecord.diskMBps - newRecord.diskMBps) >= 1.0)
        {
            return true;
        }
        if (std::fabs(oldRecord.netKBps - newRecord.netKBps) >= 8.0)
        {
            return true;
        }
        if (std::fabs(oldRecord.gpuPercent - newRecord.gpuPercent) >= 5.0)
        {
            return true;
        }

        if (oldRecord.threadCount != newRecord.threadCount ||
            oldRecord.handleCount != newRecord.handleCount ||
            oldRecord.parentPid != newRecord.parentPid ||
            oldRecord.isAdmin != newRecord.isAdmin ||
            oldRecord.signatureTrusted != newRecord.signatureTrusted)
        {
            return true;
        }

        if (oldRecord.imagePath != newRecord.imagePath ||
            oldRecord.commandLine != newRecord.commandLine ||
            oldRecord.userName != newRecord.userName ||
            oldRecord.signatureState != newRecord.signatureState ||
            oldRecord.signaturePublisher != newRecord.signaturePublisher ||
            oldRecord.startTimeText != newRecord.startTimeText)
        {
            return true;
        }

        return false;
    }

    // 统一按钮蓝色样式，和现有主题风格保持一致。
    QString buildBlueButtonStyle(const bool iconOnlyButton)
    {
        // 图标按钮采用更紧凑 padding，避免出现多余空白。
        const QString paddingText = iconOnlyButton ? QStringLiteral("4px") : QStringLiteral("4px 10px");
        return QStringLiteral(
            "QPushButton {"
            "  color: %1;"
            "  background: %6;"
            "  border: 1px solid %2;"
            "  border-radius: 3px;"
            "  padding: %5;"
            "}"
            "QPushButton:hover {"
            "  background: %3;"
            "  color: #FFFFFF;"
            "  border: 1px solid %3;"
            "}"
            "QPushButton:pressed {"
            "  background: %4;"
            "  color: #FFFFFF;"
            "}")
            .arg(KswordTheme::PrimaryBlueHex)
            .arg(KswordTheme::PrimaryBlueBorderHex)
            .arg(QStringLiteral("#2E8BFF"))
            .arg(KswordTheme::PrimaryBluePressedHex)
            .arg(paddingText)
            .arg(KswordTheme::SurfaceHex());
    }

    // 下拉框主题描边样式，保持与按钮同色系。
    QString buildBlueComboBoxStyle()
    {
        return QStringLiteral(
            "QComboBox {"
            "  border: 1px solid %1;"
            "  border-radius: 4px;"
            "  padding: 2px 8px;"
            "  color: %4;"
            "  background: %5;"
            "}"
            "QComboBox:hover {"
            "  border-color: %2;"
            "  background: %3;"
            "}"
            "QComboBox::drop-down {"
            "  border: none;"
            "  width: 20px;"
            "}"
            "QComboBox QAbstractItemView {"
            "  border: 1px solid %1;"
            "  selection-background-color: %3;"
            "}")
            .arg(KswordTheme::PrimaryBlueBorderHex)
            .arg(KswordTheme::PrimaryBlueHex)
            .arg(KswordTheme::PrimaryBlueHoverHex)
            .arg(KswordTheme::TextPrimaryHex())
            .arg(KswordTheme::SurfaceHex());
    }

    // 统一“普通输入框”主题边框。
    QString buildBlueLineEditStyle()
    {
        return QStringLiteral(
            "QLineEdit, QPlainTextEdit, QTextEdit {"
            "  border: 1px solid %2;"
            "  border-radius: 3px;"
            "  background: %3;"
            "  color: %4;"
            "  padding: 3px 5px;"
            "}"
            "QLineEdit:focus, QPlainTextEdit:focus, QTextEdit:focus {"
            "  border: 1px solid %1;"
            "}")
            .arg(KswordTheme::PrimaryBlueHex)
            .arg(KswordTheme::BorderHex())
            .arg(KswordTheme::SurfaceHex())
            .arg(KswordTheme::TextPrimaryHex());
    }

    // 常见令牌特权列表：用于“可视化调权”表格。
    const QStringList CommonPrivilegeNames{
        "SeDebugPrivilege",
        "SeImpersonatePrivilege",
        "SeAssignPrimaryTokenPrivilege",
        "SeIncreaseQuotaPrivilege",
        "SeTcbPrivilege",
        "SeBackupPrivilege",
        "SeRestorePrivilege",
        "SeLoadDriverPrivilege",
        "SeSecurityPrivilege",
        "SeTakeOwnershipPrivilege"
    };

    // BitmaskFlagDefinition 作用：
    // - 统一描述“复选框可勾选的位标志定义”；
    // - nameText：显示名称；
    // - value：该位标志对应的掩码值；
    // - descriptionText：鼠标悬停说明，帮助用户理解语义。
    struct BitmaskFlagDefinition
    {
        const char* nameText = "";            // 标志名（例如 CREATE_SUSPENDED）。
        std::uint32_t value = 0;              // 标志位掩码（DWORD）。
        const char* descriptionText = "";     // 标志用途说明文本。
    };

    // CreateProcess.dwCreationFlags 常用且可组合的位标志全集。
    const std::vector<BitmaskFlagDefinition> CreateProcessFlagDefinitions{
        { "DEBUG_PROCESS", 0x00000001U, "调试子进程和其后代进程。" },
        { "DEBUG_ONLY_THIS_PROCESS", 0x00000002U, "仅调试当前创建的子进程。" },
        { "CREATE_SUSPENDED", 0x00000004U, "主线程创建后先挂起。" },
        { "DETACHED_PROCESS", 0x00000008U, "控制台进程脱离父控制台。" },
        { "CREATE_NEW_CONSOLE", 0x00000010U, "为新进程分配新控制台窗口。" },
        { "NORMAL_PRIORITY_CLASS", 0x00000020U, "普通优先级类。" },
        { "IDLE_PRIORITY_CLASS", 0x00000040U, "空闲优先级类。" },
        { "HIGH_PRIORITY_CLASS", 0x00000080U, "高优先级类。" },
        { "REALTIME_PRIORITY_CLASS", 0x00000100U, "实时优先级类（高风险）。" },
        { "CREATE_NEW_PROCESS_GROUP", 0x00000200U, "创建新的进程组。" },
        { "CREATE_UNICODE_ENVIRONMENT", 0x00000400U, "环境块按 Unicode 传递。" },
        { "CREATE_SEPARATE_WOW_VDM", 0x00000800U, "16 位应用使用独立 WOW VDM。" },
        { "CREATE_SHARED_WOW_VDM", 0x00001000U, "16 位应用共享 WOW VDM。" },
        { "CREATE_FORCEDOS", 0x00002000U, "强制 DOS 兼容模式（历史选项）。" },
        { "BELOW_NORMAL_PRIORITY_CLASS", 0x00004000U, "低于普通优先级类。" },
        { "ABOVE_NORMAL_PRIORITY_CLASS", 0x00008000U, "高于普通优先级类。" },
        { "INHERIT_PARENT_AFFINITY", 0x00010000U, "继承父进程 CPU 亲和性。" },
        { "CREATE_PROTECTED_PROCESS", 0x00040000U, "创建受保护进程（受系统限制）。" },
        { "EXTENDED_STARTUPINFO_PRESENT", 0x00080000U, "启用 STARTUPINFOEX 扩展结构。" },
        { "PROCESS_MODE_BACKGROUND_BEGIN", 0x00100000U, "进入后台模式（I/O/CPU 降优先级）。" },
        { "PROCESS_MODE_BACKGROUND_END", 0x00200000U, "退出后台模式。" },
        { "CREATE_SECURE_PROCESS", 0x00400000U, "创建安全进程（受系统策略限制）。" },
        { "CREATE_BREAKAWAY_FROM_JOB", 0x01000000U, "允许脱离 Job 对象。" },
        { "CREATE_PRESERVE_CODE_AUTHZ_LEVEL", 0x02000000U, "保持代码授权级别。" },
        { "CREATE_DEFAULT_ERROR_MODE", 0x04000000U, "使用默认错误模式。" },
        { "CREATE_NO_WINDOW", 0x08000000U, "控制台进程不创建窗口。" },
        { "PROFILE_USER", 0x10000000U, "启用用户模式性能统计。" },
        { "PROFILE_KERNEL", 0x20000000U, "启用内核模式性能统计。" },
        { "PROFILE_SERVER", 0x40000000U, "启用服务器性能统计。" },
        { "CREATE_IGNORE_SYSTEM_DEFAULT", 0x80000000U, "忽略系统默认设置（较少使用）。" }
    };

    // STARTUPINFO.dwFlags 位标志全集。
    const std::vector<BitmaskFlagDefinition> StartupInfoFlagDefinitions{
        { "STARTF_USESHOWWINDOW", 0x00000001U, "启用 wShowWindow 字段。" },
        { "STARTF_USESIZE", 0x00000002U, "启用 dwXSize/dwYSize 字段。" },
        { "STARTF_USEPOSITION", 0x00000004U, "启用 dwX/dwY 字段。" },
        { "STARTF_USECOUNTCHARS", 0x00000008U, "启用控制台字符网格大小字段。" },
        { "STARTF_USEFILLATTRIBUTE", 0x00000010U, "启用 dwFillAttribute 字段。" },
        { "STARTF_RUNFULLSCREEN", 0x00000020U, "全屏模式启动（主要针对旧控制台）。" },
        { "STARTF_FORCEONFEEDBACK", 0x00000040U, "强制显示忙碌光标反馈。" },
        { "STARTF_FORCEOFFFEEDBACK", 0x00000080U, "关闭启动忙碌光标反馈。" },
        { "STARTF_USESTDHANDLES", 0x00000100U, "启用标准输入/输出/错误句柄字段。" },
        { "STARTF_USEHOTKEY", 0x00000200U, "启用热键字段（hStdInput 解释为 hotkey）。" },
        { "STARTF_TITLEISLINKNAME", 0x00000800U, "标题解释为 Shell 链接名。" },
        { "STARTF_TITLEISAPPID", 0x00001000U, "标题解释为 AppUserModelID。" },
        { "STARTF_PREVENTPINNING", 0x00002000U, "阻止任务栏固定（需 AppID）。" },
        { "STARTF_UNTRUSTEDSOURCE", 0x00008000U, "标记命令来源不可信。" },
        { "STARTF_HOLOGRAPHIC", 0x00040000U, "全息场景启动标记（特定平台）。" }
    };

    // STARTUPINFO.dwFillAttribute 控制台颜色/样式标志全集。
    const std::vector<BitmaskFlagDefinition> ConsoleFillAttributeDefinitions{
        { "FOREGROUND_BLUE", 0x0001U, "前景色：蓝。" },
        { "FOREGROUND_GREEN", 0x0002U, "前景色：绿。" },
        { "FOREGROUND_RED", 0x0004U, "前景色：红。" },
        { "FOREGROUND_INTENSITY", 0x0008U, "前景色高亮。" },
        { "BACKGROUND_BLUE", 0x0010U, "背景色：蓝。" },
        { "BACKGROUND_GREEN", 0x0020U, "背景色：绿。" },
        { "BACKGROUND_RED", 0x0040U, "背景色：红。" },
        { "BACKGROUND_INTENSITY", 0x0080U, "背景色高亮。" },
        { "COMMON_LVB_LEADING_BYTE", 0x0100U, "双字节字符前导字节标记。" },
        { "COMMON_LVB_TRAILING_BYTE", 0x0200U, "双字节字符后继字节标记。" },
        { "COMMON_LVB_GRID_HORIZONTAL", 0x0400U, "水平网格线。" },
        { "COMMON_LVB_GRID_LVERTICAL", 0x0800U, "左垂直网格线。" },
        { "COMMON_LVB_GRID_RVERTICAL", 0x1000U, "右垂直网格线。" },
        { "COMMON_LVB_REVERSE_VIDEO", 0x4000U, "反色显示。" },
        { "COMMON_LVB_UNDERSCORE", 0x8000U, "下划线显示。" }
    };

    // Token DesiredAccess 常用位标志全集（OpenProcessToken / DuplicateTokenEx 路径）。
    const std::vector<BitmaskFlagDefinition> TokenDesiredAccessDefinitions{
        { "TOKEN_ASSIGN_PRIMARY", 0x00000001U, "可把令牌分配给新进程主令牌。" },
        { "TOKEN_DUPLICATE", 0x00000002U, "可复制令牌。" },
        { "TOKEN_IMPERSONATE", 0x00000004U, "可模拟令牌。" },
        { "TOKEN_QUERY", 0x00000008U, "可查询令牌信息。" },
        { "TOKEN_QUERY_SOURCE", 0x00000010U, "可查询令牌来源。" },
        { "TOKEN_ADJUST_PRIVILEGES", 0x00000020U, "可调整令牌特权。" },
        { "TOKEN_ADJUST_GROUPS", 0x00000040U, "可调整令牌组。" },
        { "TOKEN_ADJUST_DEFAULT", 0x00000080U, "可调整默认 DACL/Owner 等。" },
        { "TOKEN_ADJUST_SESSIONID", 0x00000100U, "可调整会话 ID。" },
        { "DELETE", 0x00010000U, "标准删除权限。" },
        { "READ_CONTROL", 0x00020000U, "标准读取安全描述符权限。" },
        { "WRITE_DAC", 0x00040000U, "标准写 DACL 权限。" },
        { "WRITE_OWNER", 0x00080000U, "标准写 Owner 权限。" },
        { "ACCESS_SYSTEM_SECURITY", 0x01000000U, "访问 SACL 权限（高权限）。" },
        { "MAXIMUM_ALLOWED", 0x02000000U, "请求对象允许的最大权限。" },
        { "GENERIC_ALL", 0x10000000U, "通用全部权限映射。" },
        { "GENERIC_EXECUTE", 0x20000000U, "通用执行权限映射。" },
        { "GENERIC_WRITE", 0x40000000U, "通用写权限映射。" },
        { "GENERIC_READ", 0x80000000U, "通用读权限映射。" }
    };
}

ProcessDock::ProcessDock(QWidget* parent)
    : QWidget(parent)
{
    // 初始化硬件并发参数：至少按 1 核处理，避免除零。
    m_logicalCpuCount = std::max<std::uint32_t>(1, std::thread::hardware_concurrency());

    // 构造阶段按“UI -> 连接 -> 定时器 -> 首次刷新”顺序执行。
    initializeUi();
    initializeConnections();
    initializeTimer();
    requestAsyncRefresh(true);
}

void ProcessDock::refreshThemeVisuals()
{
    // 仅重建当前表格可视层，不触发新的后台枚举任务。
    // 用途：深浅色切换后，立即刷新“新增/退出”行的主题高亮色。
    rebuildTable();
}

void ProcessDock::initializeUi()
{
    // 根布局只容纳一个侧边栏 tab 控件。
    m_rootLayout = new QVBoxLayout(this);
    m_rootLayout->setContentsMargins(0, 0, 0, 0);
    m_rootLayout->setSpacing(0);

    m_sideTabWidget = new QTabWidget(this);
    m_sideTabWidget->setTabPosition(QTabWidget::West);
    m_sideTabWidget->setDocumentMode(true);

    // “进程列表”页是本模块核心页面。
    m_processListPage = new QWidget(this);
    m_processPageLayout = new QVBoxLayout(m_processListPage);
    m_processPageLayout->setContentsMargins(6, 6, 6, 6);
    m_processPageLayout->setSpacing(6);

    // 初始化上方控制栏和下方表格。
    initializeTopControls();
    initializeProcessTable();
    initializeCreateProcessPage();

    m_rootLayout->addWidget(m_sideTabWidget);
}

void ProcessDock::initializeTopControls()
{
    // 控制区改为“两行布局”：第一行放操作按钮，第二行单独放监控状态。
    QVBoxLayout* controlContainerLayout = new QVBoxLayout();
    controlContainerLayout->setContentsMargins(0, 0, 0, 0);
    controlContainerLayout->setSpacing(4);

    m_controlLayout = new QHBoxLayout();
    m_controlLayout->setContentsMargins(0, 0, 0, 0);
    m_controlLayout->setSpacing(8);
    m_statusLayout = new QHBoxLayout();
    m_statusLayout->setContentsMargins(0, 0, 0, 0);
    m_statusLayout->setSpacing(8);

    // 遍历策略下拉框：
    // 1) Toolhelp（CreateToolhelp32Snapshot + Process32First/Next）
    // 2) NtQuerySystemInformation
    // 说明：不再默认 Auto，直接明确展示当前使用的方法。
    m_strategyCombo = new QComboBox(this);
    m_strategyCombo->addItem(QIcon(IconRefresh), "Toolhelp Snapshot / Process32First / Process32Next");
    m_strategyCombo->addItem(QIcon(IconRefresh), "NtQuerySystemInformation");
    m_strategyCombo->setCurrentIndex(1);
    m_strategyCombo->setToolTip("指定进程遍历方案");
    // 自适应宽度策略：避免长文本把 Dock 顶出横向滚动条。
    m_strategyCombo->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
    m_strategyCombo->setMinimumContentsLength(18);
    m_strategyCombo->setMaximumWidth(260);

    // 树/列表切换按钮：按需求仅显示图标。
    m_treeToggleButton = new QPushButton(QIcon(IconTree), "", this);
    m_treeToggleButton->setIconSize(DefaultIconSize);
    m_treeToggleButton->setFixedSize(CompactIconButtonSize);
    m_treeToggleButton->setCheckable(true);
    m_treeToggleButton->setChecked(false);
    m_treeToggleButton->setToolTip("切换为树状视图");

    // 视图模式下拉框：默认监视视图。
    m_viewModeCombo = new QComboBox(this);
    m_viewModeCombo->addItem(QIcon(IconList), "监视视图");
    m_viewModeCombo->addItem(QIcon(IconProcessMain), "详细信息视图");
    m_viewModeCombo->setCurrentIndex(static_cast<int>(ViewMode::Monitor));
    m_viewModeCombo->setToolTip("切换监视视图 / 详细信息视图");
    m_viewModeCombo->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
    m_viewModeCombo->setMinimumContentsLength(8);
    m_viewModeCombo->setMaximumWidth(180);

    const QString comboStyle = buildBlueComboBoxStyle();
    m_strategyCombo->setStyleSheet(comboStyle);
    m_viewModeCombo->setStyleSheet(comboStyle);

    // 开始/暂停按钮：按需求仅显示图标。
    m_startButton = new QPushButton(QIcon(IconStart), "", this);
    m_pauseButton = new QPushButton(QIcon(IconPause), "", this);
    m_startButton->setIconSize(DefaultIconSize);
    m_pauseButton->setIconSize(DefaultIconSize);
    m_startButton->setFixedSize(CompactIconButtonSize);
    m_pauseButton->setFixedSize(CompactIconButtonSize);
    m_startButton->setToolTip("开始周期性刷新进程列表");
    m_pauseButton->setToolTip("暂停周期性刷新进程列表");

    // 刷新周期滑块（秒）。
    m_refreshLabel = new QLabel("刷新间隔: 2 秒", this);
    m_refreshSlider = new QSlider(Qt::Horizontal, this);
    m_refreshSlider->setRange(1, 10);
    m_refreshSlider->setValue(2);
    m_refreshSlider->setFixedWidth(140);
    m_refreshSlider->setToolTip("设置几秒刷新一次");

    // 刷新状态标签：明确告诉用户当前是否在刷新，以及最后耗时。
    m_refreshStateLabel = new QLabel("● 空闲", this);
    m_refreshStateLabel->setStyleSheet(
        QStringLiteral("color:%1; font-weight:600;")
        .arg(KswordTheme::TextSecondaryHex()));
    m_refreshStateLabel->setToolTip("当前刷新状态");

    // 按钮统一蓝色风格（图标按钮版本）。
    const QString buttonStyle = buildBlueButtonStyle(true);
    m_treeToggleButton->setStyleSheet(buttonStyle);
    m_startButton->setStyleSheet(buttonStyle);
    m_pauseButton->setStyleSheet(buttonStyle);

    // 第一行放“操作按钮 + 刷新间隔”。
    m_controlLayout->addWidget(m_strategyCombo);
    m_controlLayout->addWidget(m_treeToggleButton);
    m_controlLayout->addWidget(m_viewModeCombo);
    m_controlLayout->addWidget(m_startButton);
    m_controlLayout->addWidget(m_pauseButton);
    m_controlLayout->addStretch(1);
    m_controlLayout->addWidget(m_refreshLabel);
    m_controlLayout->addWidget(m_refreshSlider);
    // 第二行只放“监控状态”，避免与操作按钮挤在同一行导致横向滚动。
    m_statusLayout->addWidget(m_refreshStateLabel, 1);
    m_statusLayout->addStretch(1);

    controlContainerLayout->addLayout(m_controlLayout);
    controlContainerLayout->addLayout(m_statusLayout);
    m_processPageLayout->addLayout(controlContainerLayout);
}

void ProcessDock::initializeProcessTable()
{
    m_processTable = new QTreeWidget(this);
    m_processTable->setColumnCount(static_cast<int>(TableColumn::Count));
    m_processTable->setHeaderLabels(ProcessTableHeaders);
    m_processTable->setRootIsDecorated(false);
    m_processTable->setItemsExpandable(false);
    m_processTable->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_processTable->setSelectionMode(QAbstractItemView::SingleSelection);
    m_processTable->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_processTable->setUniformRowHeights(true);
    m_processTable->setSortingEnabled(true);
    m_processTable->setContextMenuPolicy(Qt::CustomContextMenu);
    m_processTable->setAlternatingRowColors(true);
    // 列宽由自适应逻辑统一控制，强制关闭内部横向滚动条。
    m_processTable->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);

    // 表头支持拖动、右键显示/隐藏列。
    QHeaderView* headerView = m_processTable->header();
    headerView->setSectionsMovable(true);
    headerView->setStretchLastSection(false);
    headerView->setContextMenuPolicy(Qt::CustomContextMenu);
    headerView->setStyleSheet(QStringLiteral(
        "QHeaderView::section {"
        "  color: %1;"
        "  background: %2;"
        "  border: 1px solid %3;"
        "  padding: 4px;"
        "  font-weight: 600;"
        "}")
        .arg(KswordTheme::PrimaryBlueHex)
        .arg(KswordTheme::SurfaceHex())
        .arg(KswordTheme::BorderHex()));

    applyDefaultColumnWidths();
    applyViewMode(ViewMode::Monitor);
    applyAdaptiveColumnWidths();
    m_processPageLayout->addWidget(m_processTable, 1);

    // 满足需求 3.1：侧边栏 Tab 中包含“进程列表”页签。
    m_sideTabWidget->addTab(m_processListPage, QIcon(IconProcessMain), "进程列表");
    m_sideTabWidget->setCurrentIndex(0);
}

void ProcessDock::initializeCreateProcessPage()
{
    m_createProcessPage = new QWidget(this);
    m_createProcessPageLayout = new QVBoxLayout(m_createProcessPage);
    m_createProcessPageLayout->setContentsMargins(6, 6, 6, 6);
    m_createProcessPageLayout->setSpacing(6);

    QScrollArea* scrollArea = new QScrollArea(m_createProcessPage);
    scrollArea->setWidgetResizable(true);
    scrollArea->setFrameShape(QFrame::NoFrame);
    m_createProcessPageLayout->addWidget(scrollArea, 1);

    QWidget* contentWidget = new QWidget(scrollArea);
    QVBoxLayout* contentLayout = new QVBoxLayout(contentWidget);
    contentLayout->setContentsMargins(2, 2, 2, 2);
    contentLayout->setSpacing(8);
    scrollArea->setWidget(contentWidget);

    const QString inputStyle = buildBlueLineEditStyle();
    const QString comboStyle = buildBlueComboBoxStyle();
    const QString buttonStyle = buildBlueButtonStyle(false);

    // 每次重建页面前先清空位标志复选框缓存，避免重复初始化导致悬空指针。
    m_creationFlagChecks.clear();
    m_startupFlagChecks.clear();
    m_startupFillAttributeChecks.clear();
    m_tokenDesiredAccessChecks.clear();

    // buildBitmaskCheckGroup 作用：
    // - 根据“标志定义列表”自动生成复选框分组；
    // - 每个复选框通过 Qt Property 保存 flagValue/flagName，后续可统一做组合计算。
    const auto buildBitmaskCheckGroup =
        [](
            QWidget* parentWidget,
            const QString& groupTitle,
            const std::vector<BitmaskFlagDefinition>& definitionList,
            std::vector<QCheckBox*>* outputCheckBoxList) -> QGroupBox*
    {
        QGroupBox* groupBox = new QGroupBox(groupTitle, parentWidget);
        QGridLayout* groupLayout = new QGridLayout(groupBox);
        groupLayout->setContentsMargins(6, 6, 6, 6);
        groupLayout->setHorizontalSpacing(10);
        groupLayout->setVerticalSpacing(4);

        const int columnCount = 3;
        for (std::size_t index = 0; index < definitionList.size(); ++index)
        {
            const BitmaskFlagDefinition& definition = definitionList[index];
            QCheckBox* flagCheck = new QCheckBox(QString::fromUtf8(definition.nameText), groupBox);
            flagCheck->setProperty("flagValue", static_cast<qulonglong>(definition.value));
            flagCheck->setProperty("flagName", QString::fromUtf8(definition.nameText));
            flagCheck->setToolTip(
                QStringLiteral("%1\n值: 0x%2\n说明: %3")
                .arg(QString::fromUtf8(definition.nameText))
                .arg(QString::number(static_cast<qulonglong>(definition.value), 16).toUpper())
                .arg(QString::fromUtf8(definition.descriptionText)));

            const int row = static_cast<int>(index / static_cast<std::size_t>(columnCount));
            const int col = static_cast<int>(index % static_cast<std::size_t>(columnCount));
            groupLayout->addWidget(flagCheck, row, col);

            if (outputCheckBoxList != nullptr)
            {
                outputCheckBoxList->push_back(flagCheck);
            }
        }
        return groupBox;
    };

    // 1) 创建方式 + 令牌来源配置。
    QGroupBox* methodGroup = new QGroupBox("创建方式 / 令牌来源", contentWidget);
    QGridLayout* methodLayout = new QGridLayout(methodGroup);
    methodLayout->setHorizontalSpacing(8);
    methodLayout->setVerticalSpacing(6);
    m_createMethodCombo = new QComboBox(methodGroup);
    m_createMethodCombo->addItem("CreateProcessW");
    m_createMethodCombo->addItem("CreateProcessAsTokenW (内部使用 CreateProcessAsUserW + fallback)");
    m_createMethodCombo->setStyleSheet(comboStyle);
    m_createMethodCombo->setCurrentIndex(0);
    m_createMethodCombo->setToolTip("默认直接调用 CreateProcessW；切换到 Token 模式时会按 PID 打开并调整令牌。");

    m_tokenSourcePidEdit = new QLineEdit("0", methodGroup);
    m_tokenDesiredAccessEdit = new QLineEdit("0x00000FAB", methodGroup);
    m_tokenDuplicatePrimaryCheck = new QCheckBox("DuplicateTokenEx 成 PrimaryToken", methodGroup);
    m_tokenDuplicatePrimaryCheck->setChecked(true);
    m_tokenSourcePidEdit->setStyleSheet(inputStyle);
    m_tokenDesiredAccessEdit->setStyleSheet(inputStyle);

    // 方法选择区域补充中文语义，避免只看英文 API 名不易理解。
    methodLayout->addWidget(new QLabel("API（创建方式）:", methodGroup), 0, 0);
    methodLayout->addWidget(m_createMethodCombo, 0, 1, 1, 3);
    methodLayout->addWidget(new QLabel("源 PID（令牌来源进程）:", methodGroup), 1, 0);
    methodLayout->addWidget(m_tokenSourcePidEdit, 1, 1);
    methodLayout->addWidget(new QLabel("令牌访问掩码（DesiredAccess）:", methodGroup), 1, 2);
    methodLayout->addWidget(m_tokenDesiredAccessEdit, 1, 3);
    methodLayout->addWidget(m_tokenDuplicatePrimaryCheck, 2, 0, 1, 4);

    // Token DesiredAccess 位标志勾选区：
    // - 覆盖最常见 TOKEN_* / 标准权限 / GENERIC_*；
    // - 用户可通过勾选组合，自动拼出访问掩码。
    QGroupBox* tokenAccessGroup = buildBitmaskCheckGroup(
        methodGroup,
        "Token DesiredAccess 位标志组合",
        TokenDesiredAccessDefinitions,
        &m_tokenDesiredAccessChecks);
    methodLayout->addWidget(tokenAccessGroup, 3, 0, 1, 4);
    contentLayout->addWidget(methodGroup);

    // 2) CreateProcessW 基础参数。
    QGroupBox* basicGroup = new QGroupBox("CreateProcessW 参数（全部可选 Null）", contentWidget);
    QGridLayout* basicLayout = new QGridLayout(basicGroup);
    basicLayout->setHorizontalSpacing(8);
    basicLayout->setVerticalSpacing(6);

    m_useApplicationNameCheck = new QCheckBox("启用 lpApplicationName（应用程序路径）", basicGroup);
    m_useCommandLineCheck = new QCheckBox("启用 lpCommandLine（命令行参数）", basicGroup);
    m_useCurrentDirectoryCheck = new QCheckBox("启用 lpCurrentDirectory（当前工作目录）", basicGroup);
    m_useEnvironmentCheck = new QCheckBox("启用 lpEnvironment（环境变量块）", basicGroup);
    m_environmentUnicodeCheck = new QCheckBox("环境块按 Unicode 传递（CREATE_UNICODE_ENVIRONMENT）", basicGroup);
    m_inheritHandleCheck = new QCheckBox("bInheritHandles（是否继承句柄）=TRUE", basicGroup);

    m_applicationNameEdit = new QLineEdit(basicGroup);
    m_applicationBrowseButton = new QPushButton("浏览...", basicGroup);
    m_commandLineEdit = new QLineEdit(basicGroup);
    m_currentDirectoryEdit = new QLineEdit(basicGroup);
    m_currentDirectoryBrowseButton = new QPushButton("浏览...", basicGroup);
    m_environmentEditor = new QPlainTextEdit(basicGroup);
    m_creationFlagsEdit = new QLineEdit("0x00000000", basicGroup);
    m_environmentEditor->setPlaceholderText("每行一个 KEY=VALUE，留空则为 null。");
    m_environmentEditor->setFixedHeight(72);

    m_applicationNameEdit->setStyleSheet(inputStyle);
    m_commandLineEdit->setStyleSheet(inputStyle);
    m_currentDirectoryEdit->setStyleSheet(inputStyle);
    m_environmentEditor->setStyleSheet(inputStyle);
    m_creationFlagsEdit->setStyleSheet(inputStyle);
    m_applicationBrowseButton->setStyleSheet(buttonStyle);
    m_currentDirectoryBrowseButton->setStyleSheet(buttonStyle);

    m_useApplicationNameCheck->setChecked(false);
    m_useCommandLineCheck->setChecked(false);
    m_useCurrentDirectoryCheck->setChecked(false);
    m_useEnvironmentCheck->setChecked(false);
    m_environmentUnicodeCheck->setChecked(true);

    basicLayout->addWidget(m_useApplicationNameCheck, 0, 0);
    basicLayout->addWidget(m_applicationNameEdit, 0, 1, 1, 2);
    basicLayout->addWidget(m_applicationBrowseButton, 0, 3);
    basicLayout->addWidget(m_useCommandLineCheck, 1, 0);
    basicLayout->addWidget(m_commandLineEdit, 1, 1, 1, 3);
    basicLayout->addWidget(m_useCurrentDirectoryCheck, 2, 0);
    basicLayout->addWidget(m_currentDirectoryEdit, 2, 1, 1, 2);
    basicLayout->addWidget(m_currentDirectoryBrowseButton, 2, 3);
    basicLayout->addWidget(m_useEnvironmentCheck, 3, 0);
    basicLayout->addWidget(m_environmentEditor, 3, 1, 2, 3);
    basicLayout->addWidget(m_environmentUnicodeCheck, 5, 1, 1, 3);
    basicLayout->addWidget(new QLabel("dwCreationFlags（创建标志）:", basicGroup), 6, 0);
    basicLayout->addWidget(m_creationFlagsEdit, 6, 1);
    basicLayout->addWidget(m_inheritHandleCheck, 6, 2, 1, 2);

    // dwCreationFlags 位标志勾选区：
    // - 列出 CreateProcess 常见全部标志；
    // - 用户勾选后自动组合成掩码写回 dwCreationFlags 输入框。
    QGroupBox* creationFlagsGroup = buildBitmaskCheckGroup(
        basicGroup,
        "dwCreationFlags 位标志组合",
        CreateProcessFlagDefinitions,
        &m_creationFlagChecks);
    basicLayout->addWidget(creationFlagsGroup, 7, 0, 1, 4);
    contentLayout->addWidget(basicGroup);

    // 3) PROCESS / THREAD SECURITY_ATTRIBUTES。
    QGroupBox* securityGroup = new QGroupBox("SECURITY_ATTRIBUTES（Process / Thread）", contentWidget);
    QGridLayout* securityLayout = new QGridLayout(securityGroup);
    securityLayout->setHorizontalSpacing(8);
    securityLayout->setVerticalSpacing(6);

    m_useProcessSecurityCheck = new QCheckBox("启用 lpProcessAttributes（进程安全属性）", securityGroup);
    m_processSecurityLengthEdit = new QLineEdit("0", securityGroup);
    m_processSecurityDescriptorEdit = new QLineEdit("0", securityGroup);
    m_processSecurityInheritCheck = new QCheckBox("bInheritHandle（进程 SA）", securityGroup);

    m_useThreadSecurityCheck = new QCheckBox("启用 lpThreadAttributes（线程安全属性）", securityGroup);
    m_threadSecurityLengthEdit = new QLineEdit("0", securityGroup);
    m_threadSecurityDescriptorEdit = new QLineEdit("0", securityGroup);
    m_threadSecurityInheritCheck = new QCheckBox("bInheritHandle（线程 SA）", securityGroup);

    m_processSecurityLengthEdit->setStyleSheet(inputStyle);
    m_processSecurityDescriptorEdit->setStyleSheet(inputStyle);
    m_threadSecurityLengthEdit->setStyleSheet(inputStyle);
    m_threadSecurityDescriptorEdit->setStyleSheet(inputStyle);

    securityLayout->addWidget(m_useProcessSecurityCheck, 0, 0);
    securityLayout->addWidget(new QLabel("nLength（结构体长度）", securityGroup), 0, 1);
    securityLayout->addWidget(m_processSecurityLengthEdit, 0, 2);
    securityLayout->addWidget(new QLabel("lpSecurityDescriptor（安全描述符指针）", securityGroup), 0, 3);
    securityLayout->addWidget(m_processSecurityDescriptorEdit, 0, 4);
    securityLayout->addWidget(m_processSecurityInheritCheck, 0, 5);
    securityLayout->addWidget(m_useThreadSecurityCheck, 1, 0);
    securityLayout->addWidget(new QLabel("nLength（结构体长度）", securityGroup), 1, 1);
    securityLayout->addWidget(m_threadSecurityLengthEdit, 1, 2);
    securityLayout->addWidget(new QLabel("lpSecurityDescriptor（安全描述符指针）", securityGroup), 1, 3);
    securityLayout->addWidget(m_threadSecurityDescriptorEdit, 1, 4);
    securityLayout->addWidget(m_threadSecurityInheritCheck, 1, 5);
    contentLayout->addWidget(securityGroup);

    // 4) STARTUPINFOW 全字段。
    QGroupBox* startupGroup = new QGroupBox("STARTUPINFOW（全部字段）", contentWidget);
    QGridLayout* startupLayout = new QGridLayout(startupGroup);
    startupLayout->setHorizontalSpacing(8);
    startupLayout->setVerticalSpacing(6);
    m_useStartupInfoCheck = new QCheckBox("启用 lpStartupInfo（启动信息结构体，取消则传 NULL）", startupGroup);
    m_useStartupInfoCheck->setChecked(false);
    m_useStartupInfoCheck->setToolTip("注意：Win32 通常要求该参数非空，传 NULL 主要用于测试极限场景。");

    m_siCbEdit = new QLineEdit("0", startupGroup);
    m_siReservedEdit = new QLineEdit(startupGroup);
    m_siDesktopEdit = new QLineEdit(startupGroup);
    m_siTitleEdit = new QLineEdit(startupGroup);
    m_siXEdit = new QLineEdit("0", startupGroup);
    m_siYEdit = new QLineEdit("0", startupGroup);
    m_siXSizeEdit = new QLineEdit("0", startupGroup);
    m_siYSizeEdit = new QLineEdit("0", startupGroup);
    m_siXCountCharsEdit = new QLineEdit("0", startupGroup);
    m_siYCountCharsEdit = new QLineEdit("0", startupGroup);
    m_siFillAttributeEdit = new QLineEdit("0x00000000", startupGroup);
    m_siFlagsEdit = new QLineEdit("0x00000000", startupGroup);
    m_siShowWindowEdit = new QLineEdit("0", startupGroup);
    m_siCbReserved2Edit = new QLineEdit("0", startupGroup);
    m_siReserved2PtrEdit = new QLineEdit("0", startupGroup);
    m_siStdInputEdit = new QLineEdit("0", startupGroup);
    m_siStdOutputEdit = new QLineEdit("0", startupGroup);
    m_siStdErrorEdit = new QLineEdit("0", startupGroup);

    const QList<QLineEdit*> startupEdits{
        m_siCbEdit, m_siReservedEdit, m_siDesktopEdit, m_siTitleEdit,
        m_siXEdit, m_siYEdit, m_siXSizeEdit, m_siYSizeEdit,
        m_siXCountCharsEdit, m_siYCountCharsEdit, m_siFillAttributeEdit,
        m_siFlagsEdit, m_siShowWindowEdit, m_siCbReserved2Edit,
        m_siReserved2PtrEdit, m_siStdInputEdit, m_siStdOutputEdit, m_siStdErrorEdit
    };
    for (QLineEdit* startupEdit : startupEdits)
    {
        startupEdit->setStyleSheet(inputStyle);
    }

    int startupRow = 0;
    startupLayout->addWidget(m_useStartupInfoCheck, startupRow++, 0, 1, 6);
    const auto addStartupField = [&startupLayout, &startupRow](const QString& labelText, QWidget* editorWidget, const int colOffset)
        {
            startupLayout->addWidget(new QLabel(labelText), startupRow, colOffset);
            startupLayout->addWidget(editorWidget, startupRow, colOffset + 1);
        };

    addStartupField("cb（结构体大小）", m_siCbEdit, 0);
    addStartupField("lpReserved（保留字符串）", m_siReservedEdit, 2);
    addStartupField("lpDesktop（目标桌面）", m_siDesktopEdit, 4);
    ++startupRow;
    addStartupField("lpTitle（窗口标题）", m_siTitleEdit, 0);
    addStartupField("dwX（窗口X坐标）", m_siXEdit, 2);
    addStartupField("dwY（窗口Y坐标）", m_siYEdit, 4);
    ++startupRow;
    addStartupField("dwXSize（窗口宽）", m_siXSizeEdit, 0);
    addStartupField("dwYSize（窗口高）", m_siYSizeEdit, 2);
    addStartupField("dwXCountChars（控制台宽）", m_siXCountCharsEdit, 4);
    ++startupRow;
    addStartupField("dwYCountChars（控制台高）", m_siYCountCharsEdit, 0);
    addStartupField("dwFillAttribute（控制台属性）", m_siFillAttributeEdit, 2);
    addStartupField("dwFlags（启动标志）", m_siFlagsEdit, 4);
    ++startupRow;
    addStartupField("wShowWindow（显示方式）", m_siShowWindowEdit, 0);
    addStartupField("cbReserved2（保留2长度）", m_siCbReserved2Edit, 2);
    addStartupField("lpReserved2（保留2指针）", m_siReserved2PtrEdit, 4);
    ++startupRow;
    addStartupField("hStdInput（标准输入句柄）", m_siStdInputEdit, 0);
    addStartupField("hStdOutput（标准输出句柄）", m_siStdOutputEdit, 2);
    addStartupField("hStdError（标准错误句柄）", m_siStdErrorEdit, 4);
    ++startupRow;

    // STARTUPINFO.dwFillAttribute 位标志勾选区：
    // - 提供前景/背景颜色与样式位的可视化组合。
    QGroupBox* startupFillAttrGroup = buildBitmaskCheckGroup(
        startupGroup,
        "STARTUPINFO.dwFillAttribute 位标志组合",
        ConsoleFillAttributeDefinitions,
        &m_startupFillAttributeChecks);
    startupLayout->addWidget(startupFillAttrGroup, startupRow++, 0, 1, 6);

    // STARTUPINFO.dwFlags 位标志勾选区：
    // - 列出 STARTF_* 标志；
    // - 通过复选框直接组合，并自动回填到 dwFlags 输入框。
    QGroupBox* startupFlagsGroup = buildBitmaskCheckGroup(
        startupGroup,
        "STARTUPINFO.dwFlags 位标志组合",
        StartupInfoFlagDefinitions,
        &m_startupFlagChecks);
    startupLayout->addWidget(startupFlagsGroup, startupRow++, 0, 1, 6);

    contentLayout->addWidget(startupGroup);

    // 5) PROCESS_INFORMATION 全字段。
    QGroupBox* processInfoGroup = new QGroupBox("PROCESS_INFORMATION（输出结构体，支持自定义初值）", contentWidget);
    QGridLayout* processInfoLayout = new QGridLayout(processInfoGroup);
    processInfoLayout->setHorizontalSpacing(8);
    processInfoLayout->setVerticalSpacing(6);

    m_useProcessInfoCheck = new QCheckBox("启用 lpProcessInformation（进程信息输出结构，取消则传 NULL）", processInfoGroup);
    m_useProcessInfoCheck->setChecked(false);
    m_useProcessInfoCheck->setToolTip("注意：Win32 通常要求该参数非空，传 NULL 会导致调用失败。");
    m_piProcessHandleEdit = new QLineEdit("0", processInfoGroup);
    m_piThreadHandleEdit = new QLineEdit("0", processInfoGroup);
    m_piPidEdit = new QLineEdit("0", processInfoGroup);
    m_piTidEdit = new QLineEdit("0", processInfoGroup);
    m_piProcessHandleEdit->setStyleSheet(inputStyle);
    m_piThreadHandleEdit->setStyleSheet(inputStyle);
    m_piPidEdit->setStyleSheet(inputStyle);
    m_piTidEdit->setStyleSheet(inputStyle);

    processInfoLayout->addWidget(m_useProcessInfoCheck, 0, 0, 1, 4);
    processInfoLayout->addWidget(new QLabel("hProcess（输出进程句柄）", processInfoGroup), 1, 0);
    processInfoLayout->addWidget(m_piProcessHandleEdit, 1, 1);
    processInfoLayout->addWidget(new QLabel("hThread（输出线程句柄）", processInfoGroup), 1, 2);
    processInfoLayout->addWidget(m_piThreadHandleEdit, 1, 3);
    processInfoLayout->addWidget(new QLabel("dwProcessId（输出PID）", processInfoGroup), 2, 0);
    processInfoLayout->addWidget(m_piPidEdit, 2, 1);
    processInfoLayout->addWidget(new QLabel("dwThreadId（输出TID）", processInfoGroup), 2, 2);
    processInfoLayout->addWidget(m_piTidEdit, 2, 3);
    contentLayout->addWidget(processInfoGroup);

    // 6) Token 特权编辑器。
    QGroupBox* tokenPrivilegeGroup = new QGroupBox("Token 特权调整（AdjustTokenPrivileges）", contentWidget);
    QVBoxLayout* tokenPrivilegeLayout = new QVBoxLayout(tokenPrivilegeGroup);
    m_tokenPrivilegeTable = new QTableWidget(CommonPrivilegeNames.size(), 2, tokenPrivilegeGroup);
    m_tokenPrivilegeTable->setHorizontalHeaderLabels(QStringList{ "Privilege", "Action" });
    m_tokenPrivilegeTable->horizontalHeader()->setStretchLastSection(true);
    m_tokenPrivilegeTable->verticalHeader()->setVisible(false);
    m_tokenPrivilegeTable->setSelectionMode(QAbstractItemView::NoSelection);
    m_tokenPrivilegeTable->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_tokenPrivilegeTable->setAlternatingRowColors(true);

    for (int row = 0; row < CommonPrivilegeNames.size(); ++row)
    {
        QTableWidgetItem* nameItem = new QTableWidgetItem(CommonPrivilegeNames.at(row));
        nameItem->setFlags(nameItem->flags() & ~Qt::ItemIsEditable);
        m_tokenPrivilegeTable->setItem(row, 0, nameItem);

        QComboBox* actionCombo = new QComboBox(m_tokenPrivilegeTable);
        actionCombo->addItem("保持", static_cast<int>(ks::process::TokenPrivilegeAction::Keep));
        actionCombo->addItem("启用", static_cast<int>(ks::process::TokenPrivilegeAction::Enable));
        actionCombo->addItem("禁用", static_cast<int>(ks::process::TokenPrivilegeAction::Disable));
        actionCombo->addItem("移除", static_cast<int>(ks::process::TokenPrivilegeAction::Remove));
        actionCombo->setCurrentIndex(0);
        actionCombo->setStyleSheet(comboStyle);
        m_tokenPrivilegeTable->setCellWidget(row, 1, actionCombo);
    }

    QHBoxLayout* tokenActionLayout = new QHBoxLayout();
    m_applyTokenPrivilegeButton = new QPushButton("仅应用令牌调整（不创建）", tokenPrivilegeGroup);
    m_resetTokenPrivilegeButton = new QPushButton("重置全部特权动作为保持", tokenPrivilegeGroup);
    m_applyTokenPrivilegeButton->setStyleSheet(buttonStyle);
    m_resetTokenPrivilegeButton->setStyleSheet(buttonStyle);
    tokenActionLayout->addWidget(m_applyTokenPrivilegeButton);
    tokenActionLayout->addWidget(m_resetTokenPrivilegeButton);
    tokenActionLayout->addStretch(1);

    tokenPrivilegeLayout->addWidget(m_tokenPrivilegeTable, 1);
    tokenPrivilegeLayout->addLayout(tokenActionLayout);
    contentLayout->addWidget(tokenPrivilegeGroup);

    // 7) 操作按钮 + 输出日志。
    QGroupBox* actionGroup = new QGroupBox("执行与结果", contentWidget);
    QVBoxLayout* actionLayout = new QVBoxLayout(actionGroup);
    QHBoxLayout* actionButtonLayout = new QHBoxLayout();
    m_launchProcessButton = new QPushButton("执行创建进程", actionGroup);
    m_resetCreateFormButton = new QPushButton("恢复默认配置", actionGroup);
    m_launchProcessButton->setStyleSheet(buttonStyle);
    m_resetCreateFormButton->setStyleSheet(buttonStyle);
    actionButtonLayout->addWidget(m_launchProcessButton);
    actionButtonLayout->addWidget(m_resetCreateFormButton);
    actionButtonLayout->addStretch(1);

    m_createResultOutput = new QTextEdit(actionGroup);
    m_createResultOutput->setReadOnly(true);
    m_createResultOutput->setMinimumHeight(140);
    m_createResultOutput->setStyleSheet(inputStyle);
    m_createResultOutput->setPlaceholderText("这里显示请求参数摘要、API 返回结果和失败错误码。");

    actionLayout->addLayout(actionButtonLayout);
    actionLayout->addWidget(m_createResultOutput, 1);
    contentLayout->addWidget(actionGroup, 1);

    // 默认值补充：命令行默认跟随 applicationName。
    m_commandLineEdit->setPlaceholderText("例如: \"C:\\Windows\\System32\\notepad.exe\" C:\\test.txt");
    m_applicationNameEdit->setPlaceholderText("可执行文件路径（可为空并传 null）");
    m_currentDirectoryEdit->setPlaceholderText("工作目录（可为空并传 null）");

    // 为关键 CreateProcess 参数补充中文解释 Tooltip，便于用户理解每个字段的语义。
    m_applicationNameEdit->setToolTip("lpApplicationName：应用程序路径。可为 null，由命令行首段决定可执行文件。");
    m_commandLineEdit->setToolTip("lpCommandLine：完整命令行。可执行路径 + 参数，传入后可能被 API 就地修改。");
    m_currentDirectoryEdit->setToolTip("lpCurrentDirectory：子进程初始工作目录。");
    m_environmentEditor->setToolTip("lpEnvironment：环境变量块。每行 KEY=VALUE；禁用时传 null。");
    m_inheritHandleCheck->setToolTip("bInheritHandles：是否继承父进程可继承句柄。");
    m_creationFlagsEdit->setToolTip("dwCreationFlags：创建标志位掩码；可在下方复选框中逐位勾选组合。");
    m_useStartupInfoCheck->setToolTip("lpStartupInfo：启动信息结构体，控制窗口/标准句柄等行为。");
    m_useProcessInfoCheck->setToolTip("lpProcessInformation：接收新进程与主线程句柄/PID/TID 的输出结构。");
    m_useProcessSecurityCheck->setToolTip("lpProcessAttributes：进程对象安全属性。");
    m_useThreadSecurityCheck->setToolTip("lpThreadAttributes：主线程对象安全属性。");
    m_siFlagsEdit->setToolTip("STARTUPINFO.dwFlags：启动标志位掩码；可在下方 STARTF 复选框中组合。");
    m_siFillAttributeEdit->setToolTip("STARTUPINFO.dwFillAttribute：控制台颜色/样式位；可在下方复选框组合。");
    m_tokenDesiredAccessEdit->setToolTip("Token DesiredAccess：令牌访问掩码；可在下方复选框组合。");

    m_sideTabWidget->addTab(m_createProcessPage, QIcon(IconStart), "创建进程");
    initializeCreateProcessConnections();
}

void ProcessDock::initializeConnections()
{
    // 策略切换后立即强制刷新。
    connect(m_strategyCombo, &QComboBox::currentIndexChanged, this, [this]() {
        kLogEvent logEvent;
        info << logEvent
            << "[ProcessDock] 进程枚举策略切换为: "
            << strategyToText(toStrategy(m_strategyCombo->currentIndex()))
            << eol;
        requestAsyncRefresh(true);
    });

    // 树/列表切换：仅图标切换（不显示文字），并立即重建表格。
    connect(m_treeToggleButton, &QPushButton::toggled, this, [this](const bool checked) {
        if (checked)
        {
            m_treeToggleButton->setIcon(QIcon(IconList));
            m_treeToggleButton->setToolTip("切换为列表视图");
        }
        else
        {
            m_treeToggleButton->setIcon(QIcon(IconTree));
            m_treeToggleButton->setToolTip("切换为树状视图");
        }
        kLogEvent logEvent;
        info << logEvent
            << "[ProcessDock] 树状显示开关变更, treeMode=" << (checked ? "true" : "false")
            << eol;
        rebuildTable();
    });

    // 视图模式切换：重置默认可见列。
    connect(m_viewModeCombo, &QComboBox::currentIndexChanged, this, [this](const int modeIndex) {
        kLogEvent logEvent;
        info << logEvent
            << "[ProcessDock] 视图模式切换, modeIndex=" << modeIndex
            << ", detailMode=" << (modeIndex == static_cast<int>(ViewMode::Detail) ? "true" : "false")
            << eol;
        applyViewMode(static_cast<ViewMode>(modeIndex));
        rebuildTable();
        requestAsyncRefresh(true);
    });

    // 开始/暂停监视：仅切换标记和定时器，不阻塞 UI。
    connect(m_startButton, &QPushButton::clicked, this, [this]() {
        kLogEvent logEvent;
        info << logEvent << "[ProcessDock] 用户点击开始监视，恢复周期刷新。" << eol;
        m_monitoringEnabled = true;
        if (m_refreshTimer != nullptr)
        {
            m_refreshTimer->start();
        }
        requestAsyncRefresh(true);
    });
    connect(m_pauseButton, &QPushButton::clicked, this, [this]() {
        kLogEvent logEvent;
        warn << logEvent << "[ProcessDock] 用户点击暂停监视，暂停周期刷新。" << eol;
        m_monitoringEnabled = false;
        if (m_refreshTimer != nullptr)
        {
            m_refreshTimer->stop();
        }
        updateRefreshStateUi(false, "● 已暂停监视");
    });

    // 刷新间隔滑块：秒 -> 毫秒，动态应用到定时器。
    connect(m_refreshSlider, &QSlider::valueChanged, this, [this](const int seconds) {
        m_refreshLabel->setText(QString("刷新间隔: %1 秒").arg(seconds));
        if (m_refreshTimer != nullptr)
        {
            m_refreshTimer->setInterval(seconds * 1000);
        }

        kLogEvent logEvent;
        info << logEvent << "[ProcessDock] 刷新间隔变更为 " << seconds << " 秒。" << eol;
    });

    // 表格右键菜单。
    connect(m_processTable, &QTreeWidget::customContextMenuRequested, this, [this](const QPoint& localPosition) {
        showTableContextMenu(localPosition);
    });

    // 表头右键菜单（列显示/隐藏）。
    connect(m_processTable->header(), &QHeaderView::customContextMenuRequested, this, [this](const QPoint& localPosition) {
        showHeaderContextMenu(localPosition);
    });
}

void ProcessDock::initializeCreateProcessConnections()
{
    if (m_applicationBrowseButton != nullptr)
    {
        connect(m_applicationBrowseButton, &QPushButton::clicked, this, [this]() {
            browseCreateProcessApplicationPath();
            });
    }
    if (m_currentDirectoryBrowseButton != nullptr)
    {
        connect(m_currentDirectoryBrowseButton, &QPushButton::clicked, this, [this]() {
            browseCreateProcessCurrentDirectory();
            });
    }
    if (m_launchProcessButton != nullptr)
    {
        connect(m_launchProcessButton, &QPushButton::clicked, this, [this]() {
            executeCreateProcessRequest();
            });
    }
    if (m_resetCreateFormButton != nullptr)
    {
        connect(m_resetCreateFormButton, &QPushButton::clicked, this, [this]() {
            resetCreateProcessForm();
            });
    }
    if (m_applyTokenPrivilegeButton != nullptr)
    {
        connect(m_applyTokenPrivilegeButton, &QPushButton::clicked, this, [this]() {
            executeApplyTokenPrivilegeEditsOnly();
            });
    }
    if (m_resetTokenPrivilegeButton != nullptr && m_tokenPrivilegeTable != nullptr)
    {
        connect(m_resetTokenPrivilegeButton, &QPushButton::clicked, this, [this]() {
            for (int row = 0; row < m_tokenPrivilegeTable->rowCount(); ++row)
            {
                QComboBox* actionCombo = qobject_cast<QComboBox*>(m_tokenPrivilegeTable->cellWidget(row, 1));
                if (actionCombo != nullptr)
                {
                    actionCombo->setCurrentIndex(0);
                }
            }
            appendCreateResultLine("已重置全部特权动作到“保持”。");
            });
    }

    // 选择应用程序路径后，若 lpCommandLine 为空则自动填充便于快速执行。
    if (m_applicationNameEdit != nullptr && m_commandLineEdit != nullptr)
    {
        connect(m_applicationNameEdit, &QLineEdit::textChanged, this, [this](const QString& textValue) {
            if (textValue.trimmed().isEmpty())
            {
                return;
            }
            if (m_commandLineEdit->text().trimmed().isEmpty())
            {
                m_commandLineEdit->setText(QStringLiteral("\"%1\"").arg(textValue.trimmed()));
            }
            });
    }

    if (m_createMethodCombo != nullptr && m_tokenSourcePidEdit != nullptr)
    {
        connect(m_createMethodCombo, &QComboBox::currentIndexChanged, this, [this](const int indexValue) {
            const bool tokenMode = (indexValue == 1);
            m_tokenSourcePidEdit->setEnabled(tokenMode);
            m_tokenDesiredAccessEdit->setEnabled(tokenMode);
            m_tokenDuplicatePrimaryCheck->setEnabled(tokenMode);
            m_tokenPrivilegeTable->setEnabled(tokenMode);
            m_applyTokenPrivilegeButton->setEnabled(tokenMode);
            m_resetTokenPrivilegeButton->setEnabled(tokenMode);
            appendCreateResultLine(tokenMode
                ? "已切换到 Token 创建模式。"
                : "已切换到普通 CreateProcessW 模式。");
            });
        m_createMethodCombo->setCurrentIndex(0);
    }

    // 位标志编辑联动：
    // - 勾选复选框自动组合掩码写回输入框；
    // - 手工修改输入框会反向刷新复选框状态。
    bindBitmaskEditor(m_creationFlagsEdit, &m_creationFlagChecks, "dwCreationFlags");
    bindBitmaskEditor(m_siFlagsEdit, &m_startupFlagChecks, "STARTUPINFO.dwFlags");
    bindBitmaskEditor(m_siFillAttributeEdit, &m_startupFillAttributeChecks, "STARTUPINFO.dwFillAttribute");
    bindBitmaskEditor(m_tokenDesiredAccessEdit, &m_tokenDesiredAccessChecks, "Token DesiredAccess");

    const bool tokenMode = (m_createMethodCombo != nullptr && m_createMethodCombo->currentIndex() == 1);
    if (m_tokenSourcePidEdit != nullptr) m_tokenSourcePidEdit->setEnabled(tokenMode);
    if (m_tokenDesiredAccessEdit != nullptr) m_tokenDesiredAccessEdit->setEnabled(tokenMode);
    if (m_tokenDuplicatePrimaryCheck != nullptr) m_tokenDuplicatePrimaryCheck->setEnabled(tokenMode);
    if (m_tokenPrivilegeTable != nullptr) m_tokenPrivilegeTable->setEnabled(tokenMode);
    if (m_applyTokenPrivilegeButton != nullptr) m_applyTokenPrivilegeButton->setEnabled(tokenMode);
    if (m_resetTokenPrivilegeButton != nullptr) m_resetTokenPrivilegeButton->setEnabled(tokenMode);
}

void ProcessDock::initializeTimer()
{
    // 周期刷新定时器：默认 2 秒。
    m_refreshTimer = new QTimer(this);
    m_refreshTimer->setInterval(m_refreshSlider->value() * 1000);
    connect(m_refreshTimer, &QTimer::timeout, this, [this]() {
        requestAsyncRefresh(false);
    });
    m_refreshTimer->start();
}

void ProcessDock::updateRefreshStateUi(const bool refreshing, const QString& stateText)
{
    // 统一刷新状态标签更新逻辑，确保“刷新中/空闲”展示一致。
    if (m_refreshStateLabel == nullptr)
    {
        return;
    }

    if (refreshing)
    {
        m_refreshStateLabel->setStyleSheet(
            QStringLiteral("color:%1; font-weight:700;").arg(KswordTheme::PrimaryBlueHex));
    }
    else
    {
        const QString idleColor = KswordTheme::IsDarkModeEnabled()
            ? QStringLiteral("#6ECF7A")
            : QStringLiteral("#2F7D32");
        m_refreshStateLabel->setStyleSheet(
            QStringLiteral("color:%1; font-weight:600;").arg(idleColor));
    }
    m_refreshStateLabel->setText(stateText);
}

void ProcessDock::applyDefaultColumnWidths()
{
    m_processTable->setColumnWidth(toColumnIndex(TableColumn::Name), 280);
    m_processTable->setColumnWidth(toColumnIndex(TableColumn::Pid), 80);
    m_processTable->setColumnWidth(toColumnIndex(TableColumn::Cpu), 80);
    m_processTable->setColumnWidth(toColumnIndex(TableColumn::Ram), 90);
    m_processTable->setColumnWidth(toColumnIndex(TableColumn::Disk), 95);
    m_processTable->setColumnWidth(toColumnIndex(TableColumn::Gpu), 80);
    m_processTable->setColumnWidth(toColumnIndex(TableColumn::Net), 95);
    m_processTable->setColumnWidth(toColumnIndex(TableColumn::Signature), 260);
    m_processTable->setColumnWidth(toColumnIndex(TableColumn::Path), 280);
    m_processTable->setColumnWidth(toColumnIndex(TableColumn::ParentPid), 90);
    m_processTable->setColumnWidth(toColumnIndex(TableColumn::CommandLine), 320);
    m_processTable->setColumnWidth(toColumnIndex(TableColumn::User), 180);
    m_processTable->setColumnWidth(toColumnIndex(TableColumn::StartTime), 160);
    m_processTable->setColumnWidth(toColumnIndex(TableColumn::IsAdmin), 90);
}

void ProcessDock::applyViewMode(const ViewMode viewMode)
{
    // 先全部隐藏，再按视图打开目标列，保证状态可预测。
    for (int column = 0; column < static_cast<int>(TableColumn::Count); ++column)
    {
        m_processTable->setColumnHidden(column, true);
    }

    // 监视视图：进程名 + PID + 性能计数器。
    if (viewMode == ViewMode::Monitor)
    {
        m_processTable->setColumnHidden(toColumnIndex(TableColumn::Name), false);
        m_processTable->setColumnHidden(toColumnIndex(TableColumn::Pid), false);
        m_processTable->setColumnHidden(toColumnIndex(TableColumn::Cpu), false);
        m_processTable->setColumnHidden(toColumnIndex(TableColumn::Ram), false);
        m_processTable->setColumnHidden(toColumnIndex(TableColumn::Disk), false);
        m_processTable->setColumnHidden(toColumnIndex(TableColumn::Gpu), false);
        m_processTable->setColumnHidden(toColumnIndex(TableColumn::Net), false);
        applyAdaptiveColumnWidths();
        return;
    }

    // 详细信息视图：按用户要求“不要性能计数器列”。
    // 仅展示静态/管理相关信息列。
    m_processTable->setColumnHidden(toColumnIndex(TableColumn::Name), false);
    m_processTable->setColumnHidden(toColumnIndex(TableColumn::Pid), false);
    m_processTable->setColumnHidden(toColumnIndex(TableColumn::Signature), false);
    m_processTable->setColumnHidden(toColumnIndex(TableColumn::Path), false);
    m_processTable->setColumnHidden(toColumnIndex(TableColumn::ParentPid), false);
    m_processTable->setColumnHidden(toColumnIndex(TableColumn::CommandLine), false);
    m_processTable->setColumnHidden(toColumnIndex(TableColumn::User), false);
    m_processTable->setColumnHidden(toColumnIndex(TableColumn::StartTime), false);
    m_processTable->setColumnHidden(toColumnIndex(TableColumn::IsAdmin), false);
    applyAdaptiveColumnWidths();
}

void ProcessDock::applyAdaptiveColumnWidths()
{
    // 该函数作用：按当前可见列数量，均分可用宽度，彻底避免横向滚动条出现。
    if (m_processTable == nullptr)
    {
        return;
    }

    QHeaderView* headerView = m_processTable->header();
    if (headerView == nullptr)
    {
        return;
    }

    // 先统计可见列，隐藏列保留 ResizeToContents，防止切换视图时状态错乱。
    int visibleColumnCount = 0;
    for (int column = 0; column < static_cast<int>(TableColumn::Count); ++column)
    {
        if (!m_processTable->isColumnHidden(column))
        {
            ++visibleColumnCount;
            headerView->setSectionResizeMode(column, QHeaderView::Stretch);
        }
        else
        {
            headerView->setSectionResizeMode(column, QHeaderView::ResizeToContents);
        }
    }

    if (visibleColumnCount <= 0)
    {
        return;
    }

    // 兜底：宽度分配有时会因延迟布局未立刻生效，主动触发一次 viewport 更新。
    m_processTable->viewport()->update();
}

void ProcessDock::resizeEvent(QResizeEvent* event)
{
    QWidget::resizeEvent(event);

    // Dock 尺寸变化后立即重新分配列宽，保证任何宽度下都不出现内部横向滚动条。
    applyAdaptiveColumnWidths();
}

void ProcessDock::requestAsyncRefresh(const bool forceRefresh)
{
    // 需求：每次刷新前都检测 Ctrl，按下则跳过本轮（无论是否强制刷新）。
    if ((::GetAsyncKeyState(VK_CONTROL) & 0x8000) != 0)
    {
        kLogEvent logEvent;
        dbg << logEvent << "[ProcessDock] 检测到 Ctrl 按下，本轮刷新跳过。" << eol;
        updateRefreshStateUi(false, "● Ctrl 按下，跳过本轮刷新");
        return;
    }

    // 非强制刷新时，暂停监视或正在刷新则直接跳过。
    if (!forceRefresh)
    {
        // 右键菜单弹出期间冻结周期刷新，防止菜单绑定项被重建后失效。
        if (m_contextMenuVisible)
        {
            kLogEvent logEvent;
            dbg << logEvent << "[ProcessDock] 右键菜单处于打开状态，本轮刷新跳过。" << eol;
            updateRefreshStateUi(false, "● 菜单打开中，跳过本轮刷新");
            return;
        }

        if (!m_monitoringEnabled || m_refreshInProgress)
        {
            kLogEvent logEvent;
            dbg << logEvent
                << "[ProcessDock] 跳过非强制刷新, monitoringEnabled=" << (m_monitoringEnabled ? "true" : "false")
                << ", refreshInProgress=" << (m_refreshInProgress ? "true" : "false")
                << eol;
            return;
        }
    }

    // 强制刷新也要避免并发任务叠加。
    if (m_refreshInProgress)
    {
        kLogEvent logEvent;
        dbg << logEvent << "[ProcessDock] 跳过刷新：当前已有后台刷新任务在执行。" << eol;
        return;
    }
    m_refreshInProgress = true;

    // 创建并复用“进程刷新”进度任务，避免每轮刷新都新增新卡片。
    if (m_refreshProgressTaskPid <= 0)
    {
        m_refreshProgressTaskPid = kPro.add("进程列表刷新", "初始化刷新任务");
    }
    kPro.set(m_refreshProgressTaskPid, "准备刷新参数...", 0, 0.02f);

    // 复制当前缓存快照给后台线程，避免跨线程读写冲突。
    const int strategyIndex = m_strategyCombo->currentIndex();
    const bool detailModeEnabled = (currentViewMode() == ViewMode::Detail);
    const bool isFirstRefresh = m_cacheByIdentity.empty();
    const int staticDetailFillBudget =
        detailModeEnabled
        ? (isFirstRefresh ? 96 : 48)   // 详细视图也做预算控制，避免首轮全量静态查询导致 UI 抖动。
        : (isFirstRefresh ? 8 : 4);    // 监视视图优先速度，预算更小。
    const std::uint32_t cpuCount = m_logicalCpuCount;
    const auto previousCache = m_cacheByIdentity;
    const auto previousCounters = m_counterSampleByIdentity;

    // ticket 用于丢弃过期结果（防止乱序覆盖）。
    const std::uint64_t localTicket = ++m_refreshTicket;
    m_lastRefreshStartTime = std::chrono::steady_clock::now();
    QPointer<ProcessDock> guard(this);

    // 刷新前先更新 UI 状态与日志，给出明显“刷新中”提示。
    updateRefreshStateUi(
        true,
        QString("● 正在刷新... 策略=%1")
        .arg(QString::fromUtf8(strategyToText(toStrategy(strategyIndex)))));

    {
        kLogEvent logEvent;
        info << logEvent
            << "[ProcessDock] 刷新开始, ticket=" << localTicket
            << ", force=" << (forceRefresh ? "true" : "false")
            << ", strategy=" << strategyToText(toStrategy(strategyIndex))
            << ", detailMode=" << (detailModeEnabled ? "true" : "false")
            << ", staticBudget=" << staticDetailFillBudget
            << ", cacheSize=" << previousCache.size()
            << eol;
    }

    // QRunnable + 线程池：满足“异步刷新，不阻塞 GUI”。
    QRunnable* backgroundTask = QRunnable::create([
        guard,
        localTicket,
        strategyIndex,
        detailModeEnabled,
        staticDetailFillBudget,
        cpuCount,
        progressPid = m_refreshProgressTaskPid,
        previousCache,
        previousCounters]() mutable {
        const ProcessDock::RefreshResult refreshResult = ProcessDock::buildRefreshResult(
            strategyIndex,
            detailModeEnabled,
            staticDetailFillBudget,
            progressPid,
            previousCache,
            previousCounters,
            cpuCount);

        if (guard == nullptr)
        {
            return;
        }

        // 结果通过队列连接回主线程更新 UI。
        QMetaObject::invokeMethod(guard, [guard, localTicket, refreshResult]() {
            if (guard == nullptr)
            {
                return;
            }

            // 只接受最新 ticket 的结果，旧结果直接丢弃。
            if (localTicket < guard->m_refreshTicket)
            {
                guard->m_refreshInProgress = false;
                return;
            }

            guard->applyRefreshResult(refreshResult);
            guard->m_refreshInProgress = false;
        }, Qt::QueuedConnection);
    });

    backgroundTask->setAutoDelete(true);
    QThreadPool::globalInstance()->start(backgroundTask);
}

void ProcessDock::applyRefreshResult(const RefreshResult& refreshResult)
{
    // 计算主线程观测耗时，用于“刷新状态标签”和日志输出。
    const auto nowTime = std::chrono::steady_clock::now();
    const auto elapsedMs = static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(nowTime - m_lastRefreshStartTime).count());

    // 把最新进程数据同步到已打开的详情窗口（若对应进程仍存在）。
    // 性能策略：
    // 1) 仅同步“可见且未最小化”的详情窗口；
    // 2) 轻微变化由节流器吸收，避免每轮刷新都触发重型解析链路。
    const std::chrono::milliseconds detailWindowSyncInterval(1500);
    for (auto windowIt = m_detailWindowByIdentity.begin(); windowIt != m_detailWindowByIdentity.end();)
    {
        if (windowIt->second == nullptr)
        {
            m_detailWindowLastSyncTimeByIdentity.erase(windowIt->first);
            windowIt = m_detailWindowByIdentity.erase(windowIt);
            continue;
        }

        const QPointer<ProcessDetailWindow>& detailWindow = windowIt->second;
        if (!detailWindow->isVisible() || detailWindow->isMinimized())
        {
            ++windowIt;
            continue;
        }

        const auto nextCacheIt = refreshResult.nextCache.find(windowIt->first);
        if (nextCacheIt == refreshResult.nextCache.end())
        {
            ++windowIt;
            continue;
        }

        const auto previousCacheIt = m_cacheByIdentity.find(windowIt->first);
        const bool hasSignificantChange =
            (previousCacheIt == m_cacheByIdentity.end()) ||
            hasDetailWindowSignificantChange(previousCacheIt->second.record, nextCacheIt->second.record);

        const auto lastSyncIt = m_detailWindowLastSyncTimeByIdentity.find(windowIt->first);
        const bool reachPeriodicSyncTime =
            (lastSyncIt == m_detailWindowLastSyncTimeByIdentity.end()) ||
            (std::chrono::duration_cast<std::chrono::milliseconds>(nowTime - lastSyncIt->second) >= detailWindowSyncInterval);

        if (hasSignificantChange || reachPeriodicSyncTime)
        {
            detailWindow->updateBaseRecord(nextCacheIt->second.record);
            m_detailWindowLastSyncTimeByIdentity[windowIt->first] = nowTime;
        }
        ++windowIt;
    }

    // 用新结果替换缓存，并重建表格。
    m_cacheByIdentity = refreshResult.nextCache;
    m_counterSampleByIdentity = refreshResult.nextCounters;

    rebuildTable();

    // 更新进度任务：本轮刷新完成后自动隐藏卡片。
    if (m_refreshProgressTaskPid > 0)
    {
        kPro.set(m_refreshProgressTaskPid, "刷新完成", 100, 1.0f);
    }

    // 刷新状态标签展示详细统计，明确告诉用户“刷新已完成”。
    updateRefreshStateUi(
        false,
        QString("● 刷新完成 %1 ms | 方法:%2 | 枚举:%3 新增:%4 退出:%5")
        .arg(elapsedMs)
        .arg(QString::fromUtf8(strategyToText(refreshResult.actualStrategy)))
        .arg(refreshResult.enumeratedCount)
        .arg(refreshResult.newProcessCount)
        .arg(refreshResult.exitedProcessCount));

    // 输出详细刷新日志，便于后续性能与正确性排查。
    {
        kLogEvent logEvent;
        info << logEvent
            << "[ProcessDock] 刷新完成, elapsedMs(main)=" << elapsedMs
            << ", elapsedMs(worker)=" << refreshResult.workerElapsedMs
            << ", strategySelected=" << strategyToText(refreshResult.selectedStrategy)
            << ", strategyActual=" << strategyToText(refreshResult.actualStrategy)
            << ", enumerated=" << refreshResult.enumeratedCount
            << ", reused=" << refreshResult.reusedProcessCount
            << ", new=" << refreshResult.newProcessCount
            << ", exitedHold=" << refreshResult.exitedProcessCount
            << ", staticFilled=" << refreshResult.staticFilledCount
            << ", staticDeferred=" << refreshResult.staticDeferredCount
            << ", cacheNow=" << m_cacheByIdentity.size()
            << eol;
    }
}

ProcessDock::RefreshResult ProcessDock::buildRefreshResult(
    const int strategyIndex,
    const bool detailModeEnabled,
    const int staticDetailFillBudget,
    const int progressTaskPid,
    const std::unordered_map<std::string, CacheEntry>& previousCache,
    const std::unordered_map<std::string, ks::process::CounterSample>& previousCounters,
    const std::uint32_t logicalCpuCount)
{
    const auto workerStartTime = std::chrono::steady_clock::now();

    RefreshResult refreshResult;
    refreshResult.nextCache.clear();
    refreshResult.nextCounters.clear();
    refreshResult.selectedStrategyIndex = strategyIndex;
    refreshResult.selectedStrategy = toStrategy(strategyIndex);
    refreshResult.actualStrategy = refreshResult.selectedStrategy;
    refreshResult.detailModeEnabled = detailModeEnabled;

    // 进度条阶段 1：开始枚举。
    if (progressTaskPid > 0)
    {
        kPro.set(progressTaskPid, "正在枚举进程列表...", 10, 0.10f);
    }

    const ks::process::ProcessEnumStrategy strategy = toStrategy(strategyIndex);
    std::vector<ks::process::ProcessRecord> latestProcessList = ks::process::EnumerateProcesses(
        strategy,
        &refreshResult.actualStrategy);
    const std::uint64_t sampleTick = steadyNow100ns();
    refreshResult.enumeratedCount = latestProcessList.size();

    if (progressTaskPid > 0)
    {
        kPro.set(progressTaskPid, "正在复用缓存并计算性能计数...", 25, 0.25f);
    }

    // 静态详情预算控制：
    // - 预算用于限制“路径/命令行/用户/签名”等慢操作，避免首轮刷新过慢；
    // - 监视模式预算较低，详细模式预算较高。
    int remainingStaticBudget = std::max(0, staticDetailFillBudget);

    // 第一阶段：预处理 identity、复用旧字段，并筛选“需要补静态详情”的 PID 列表。
    std::vector<std::string> identityKeys(latestProcessList.size());
    std::vector<bool> isNewProcess(latestProcessList.size(), false);
    std::vector<bool> shouldFillStatic(latestProcessList.size(), false);
    std::vector<bool> includeSignatureList(latestProcessList.size(), false);

    for (std::size_t recordIndex = 0; recordIndex < latestProcessList.size(); ++recordIndex)
    {
        ks::process::ProcessRecord& processRecord = latestProcessList[recordIndex];

        // 若 creationTime 未取到，仍可用 0 参与 key（稳定但区分度降低）。
        const std::string identityKey = ks::process::BuildProcessIdentityKey(
            processRecord.pid,
            processRecord.creationTime100ns);
        identityKeys[recordIndex] = identityKey;

        const auto oldCacheIt = previousCache.find(identityKey);
        bool needsStaticFill = false;
        bool includeSignatureCheck = false;
        if (oldCacheIt != previousCache.end())
        {
            // 复用规则：PID + 创建时间相同则复用旧静态字段（性能计数器除外）。
            const ks::process::ProcessRecord& oldRecord = oldCacheIt->second.record;
            if (processRecord.imagePath.empty()) processRecord.imagePath = oldRecord.imagePath;
            if (processRecord.commandLine.empty()) processRecord.commandLine = oldRecord.commandLine;
            if (processRecord.userName.empty()) processRecord.userName = oldRecord.userName;
            if (processRecord.signatureState.empty()) processRecord.signatureState = oldRecord.signatureState;
            if (processRecord.signaturePublisher.empty()) processRecord.signaturePublisher = oldRecord.signaturePublisher;
            processRecord.signatureTrusted = oldRecord.signatureTrusted;
            if (processRecord.startTimeText.empty()) processRecord.startTimeText = oldRecord.startTimeText;
            processRecord.isAdmin = oldRecord.isAdmin;
            processRecord.staticDetailsReady = oldRecord.staticDetailsReady;
            ++refreshResult.reusedProcessCount;

            // 旧进程若静态字段还不完整，或签名仍 Pending，在预算允许时继续补齐。
            const bool signaturePending = (processRecord.signatureState.empty() || processRecord.signatureState == "Pending");
            needsStaticFill = !processRecord.staticDetailsReady || (detailModeEnabled && signaturePending);
            includeSignatureCheck = detailModeEnabled;
        }
        else
        {
            // 新出现进程：计数 + 依据预算决定是否补齐静态详情。
            ++refreshResult.newProcessCount;
            isNewProcess[recordIndex] = true;
            needsStaticFill = true;
            includeSignatureCheck = detailModeEnabled;
        }

        if (needsStaticFill)
        {
            // 详细信息视图与监视视图都遵循预算：
            // - 详细视图预算更高，但不再全量硬查，避免刷新瞬间卡顿；
            // - 预算外的项标记为 Pending，后续轮次继续补齐。
            const bool allowFill = (remainingStaticBudget > 0);
            if (allowFill)
            {
                shouldFillStatic[recordIndex] = true;
                includeSignatureList[recordIndex] = includeSignatureCheck;
                --remainingStaticBudget;
            }
            else
            {
                // 超预算时延后慢操作：保持基础数据可见，并标记后续继续处理。
                if (processRecord.signatureState.empty())
                {
                    processRecord.signatureState = "Pending";
                }
                processRecord.signaturePublisher.clear();
                processRecord.signatureTrusted = false;
                ++refreshResult.staticDeferredCount;
            }
        }
    }

    // 第二阶段：把“路径/签名/参数”等慢静态操作并行化，减少详细视图卡顿。
    std::vector<std::size_t> staticFillIndices;
    staticFillIndices.reserve(latestProcessList.size());
    for (std::size_t recordIndex = 0; recordIndex < shouldFillStatic.size(); ++recordIndex)
    {
        if (shouldFillStatic[recordIndex])
        {
            staticFillIndices.push_back(recordIndex);
        }
    }

    if (progressTaskPid > 0)
    {
        kPro.set(progressTaskPid, "正在并行补齐路径/签名/参数...", 40, 0.40f);
    }

    if (!staticFillIndices.empty())
    {
        // 线程数量策略：
        // - 详细视图：使用更多并行度，加速签名校验；
        // - 监视视图：仅小并发，避免过度占用 CPU。
        const unsigned int hardwareThreads = std::max(1u, std::thread::hardware_concurrency());
        const unsigned int wantedThreads = detailModeEnabled
            ? std::max(2u, std::min(6u, hardwareThreads))
            : 2u;
        const unsigned int workerCount = std::max(
            1u,
            std::min<unsigned int>(wantedThreads, static_cast<unsigned int>(staticFillIndices.size())));

        std::atomic<std::size_t> nextTaskIndex{ 0 };
        std::vector<std::thread> workerThreads;
        workerThreads.reserve(workerCount);

        // 每个线程循环领取 PID 任务并调用 FillProcessStaticDetails。
        for (unsigned int workerId = 0; workerId < workerCount; ++workerId)
        {
            workerThreads.emplace_back([&]() {
                for (;;)
                {
                    const std::size_t taskOrder = nextTaskIndex.fetch_add(1);
                    if (taskOrder >= staticFillIndices.size())
                    {
                        break;
                    }

                    const std::size_t recordIndex = staticFillIndices[taskOrder];
                    ks::process::FillProcessStaticDetails(
                        latestProcessList[recordIndex],
                        includeSignatureList[recordIndex]);
                }
                });
        }

        for (std::thread& workerThread : workerThreads)
        {
            if (workerThread.joinable())
            {
                workerThread.join();
            }
        }
        refreshResult.staticFilledCount += staticFillIndices.size();
    }

    // 第三阶段：计算性能差值并写回缓存（该阶段仍串行，保证逻辑简单稳定）。
    std::size_t processIndex = 0;
    for (std::size_t recordIndex = 0; recordIndex < latestProcessList.size(); ++recordIndex)
    {
        ++processIndex;
        ks::process::ProcessRecord& processRecord = latestProcessList[recordIndex];
        const std::string& identityKey = identityKeys[recordIndex];

        // 若当前策略未填动态计数器，则显式刷新一次。
        if (!processRecord.dynamicCountersReady)
        {
            ks::process::RefreshProcessDynamicCounters(processRecord);
        }

        // 计算 CPU/DISK 衍生计数，并写入下一轮样本。
        ks::process::CounterSample nextSample{};
        const auto oldCounterIt = previousCounters.find(identityKey);
        const ks::process::CounterSample* oldSample =
            (oldCounterIt == previousCounters.end()) ? nullptr : &oldCounterIt->second;
        ks::process::UpdateDerivedCounters(
            processRecord,
            oldSample,
            nextSample,
            logicalCpuCount,
            sampleTick);
        refreshResult.nextCounters[identityKey] = nextSample;

        CacheEntry cacheEntry{};
        cacheEntry.record = std::move(processRecord);
        cacheEntry.missingRounds = 0;
        cacheEntry.isNewInLatestRound = isNewProcess[recordIndex];
        cacheEntry.isExitedInLatestRound = false;
        refreshResult.nextCache.emplace(identityKey, std::move(cacheEntry));

        // 进度条阶段 3：按处理进度更新（频率做了抽样，避免过度抖动）。
        if (progressTaskPid > 0 && (processIndex % 48 == 0 || processIndex == latestProcessList.size()))
        {
            const double ratio = latestProcessList.empty()
                ? 1.0
                : (static_cast<double>(processIndex) / static_cast<double>(latestProcessList.size()));
            const float progressValue = static_cast<float>(0.50 + ratio * 0.35); // 50% -> 85%
            kPro.set(progressTaskPid, "正在处理缓存与性能差值...", 55, progressValue);
        }
    }

    // 再处理退出进程：上一轮存在、本轮不存在，则保留显示 1 轮灰底。
    for (const auto& oldPair : previousCache)
    {
        if (refreshResult.nextCache.find(oldPair.first) != refreshResult.nextCache.end())
        {
            continue;
        }

        const CacheEntry& oldEntry = oldPair.second;
        if (oldEntry.missingRounds >= 1)
        {
            // 已经保留过一轮，本次彻底移除。
            continue;
        }

        CacheEntry exitedEntry = oldEntry;
        exitedEntry.missingRounds = oldEntry.missingRounds + 1;
        exitedEntry.isNewInLatestRound = false;
        exitedEntry.isExitedInLatestRound = true;
        refreshResult.nextCache.emplace(oldPair.first, std::move(exitedEntry));
        ++refreshResult.exitedProcessCount;

        const auto oldCounterIt = previousCounters.find(oldPair.first);
        if (oldCounterIt != previousCounters.end())
        {
            refreshResult.nextCounters.emplace(oldPair.first, oldCounterIt->second);
        }
    }

    if (progressTaskPid > 0)
    {
        kPro.set(progressTaskPid, "后台刷新结果构建完成，等待主线程应用...", 90, 0.90f);
    }

    refreshResult.workerElapsedMs = static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - workerStartTime).count());

    return refreshResult;
}

void ProcessDock::rebuildTable()
{
    if (m_processTable == nullptr)
    {
        return;
    }

    // 记录用户当前排序列与顺序，解决“刷新后被重置为 PID 排序”的问题。
    const int previousSortColumn = m_processTable->header()->sortIndicatorSection();
    const Qt::SortOrder previousSortOrder = m_processTable->header()->sortIndicatorOrder();

    // 刷新期间临时冻结视图更新，减少大量 addTopLevelItem 时的重绘抖动。
    m_processTable->setUpdatesEnabled(false);
    m_processTable->clear();

    // 树状模式下保持父子顺序，禁用自动排序。
    const bool enableSorting = !isTreeModeEnabled();
    m_processTable->setSortingEnabled(false);

    const std::vector<DisplayRow> displayRows = buildDisplayOrder();

    // 先预计算 RAM/DISK/NET 的本轮最大值，用于把绝对值映射成“占用比例高亮”。
    double maxRamMB = 0.0;
    double maxDiskMBps = 0.0;
    double maxNetKBps = 0.0;
    for (const DisplayRow& displayRow : displayRows)
    {
        if (displayRow.record == nullptr)
        {
            continue;
        }
        maxRamMB = std::max(maxRamMB, displayRow.record->ramMB);
        maxDiskMBps = std::max(maxDiskMBps, displayRow.record->diskMBps);
        maxNetKBps = std::max(maxNetKBps, displayRow.record->netKBps);
    }

    // applyUsageHighlight 作用：
    // - 对目标单元格按占用比例绘制主题蓝背景；
    // - 占用越高，背景越明显。
    const auto applyUsageHighlight = [this](QTreeWidgetItem* item, const int columnIndex, double usageRatio)
        {
            if (item == nullptr)
            {
                return;
            }
            usageRatio = std::clamp(usageRatio, 0.0, 1.0);
            const QColor usageColor = usageRatioToHighlightColor(usageRatio);
            item->setBackground(columnIndex, usageColor);

            // 高占用时将文本置白，保证可读性。
            if (usageRatio >= 0.70)
            {
                item->setForeground(columnIndex, QColor(255, 255, 255));
            }
        };

    for (const DisplayRow& displayRow : displayRows)
    {
        if (displayRow.record == nullptr)
        {
            continue;
        }

        QTreeWidgetItem* rowItem = new QTreeWidgetItem();
        const ks::process::ProcessRecord& processRecord = *displayRow.record;
        const std::string identityKey = ks::process::BuildProcessIdentityKey(
            processRecord.pid,
            processRecord.creationTime100ns);
        rowItem->setData(0, Qt::UserRole, QString::fromStdString(identityKey));

        // 每一列文本填充，Name 列附加图标。
        for (int columnIndex = 0; columnIndex < static_cast<int>(TableColumn::Count); ++columnIndex)
        {
            const TableColumn column = static_cast<TableColumn>(columnIndex);
            rowItem->setText(columnIndex, formatColumnText(processRecord, column, displayRow.depth));
        }

        // 进程名列固定显示目标 EXE 图标（命中缓存后开销可控）。
        rowItem->setIcon(toColumnIndex(TableColumn::Name), resolveProcessIcon(processRecord));

        // 管理员列：按要求使用“绿色/红色方块”直观显示状态。
        rowItem->setTextAlignment(toColumnIndex(TableColumn::IsAdmin), Qt::AlignCenter);
        const QColor adminYesColor = KswordTheme::IsDarkModeEnabled()
            ? QColor(130, 210, 140)
            : QColor(34, 139, 34);
        const QColor adminNoColor = KswordTheme::IsDarkModeEnabled()
            ? QColor(255, 140, 140)
            : QColor(220, 50, 47);
        rowItem->setForeground(
            toColumnIndex(TableColumn::IsAdmin),
            processRecord.isAdmin ? adminYesColor : adminNoColor);

        // 数字签名列：非受信任时标红，方便快速识别风险进程。
        if (!processRecord.signatureTrusted && processRecord.signatureState != "Pending")
        {
            rowItem->setForeground(toColumnIndex(TableColumn::Signature), adminNoColor);
        }
        else if (processRecord.signatureTrusted)
        {
            rowItem->setForeground(toColumnIndex(TableColumn::Signature), adminYesColor);
        }

        // 新增进程绿色高亮；退出保留进程灰色高亮。
        if (displayRow.isNew)
        {
            for (int columnIndex = 0; columnIndex < static_cast<int>(TableColumn::Count); ++columnIndex)
            {
                rowItem->setBackground(columnIndex, KswordTheme::NewRowBackgroundColor());
            }
        }
        else if (displayRow.isExited)
        {
            for (int columnIndex = 0; columnIndex < static_cast<int>(TableColumn::Count); ++columnIndex)
            {
                rowItem->setBackground(columnIndex, KswordTheme::ExitedRowBackgroundColor());
                rowItem->setForeground(columnIndex, KswordTheme::ExitedRowForegroundColor());
            }
        }
        else
        {
            // 常规行按占用比例做主题色高亮（CPU/RAM/DISK/GPU/NET）。
            const double cpuUsageRatio = std::clamp(processRecord.cpuPercent / 100.0, 0.0, 1.0);
            const double ramUsageRatio = (maxRamMB > 0.0)
                ? std::clamp(processRecord.ramMB / maxRamMB, 0.0, 1.0)
                : 0.0;
            const double diskUsageRatio = (maxDiskMBps > 0.0)
                ? std::clamp(processRecord.diskMBps / maxDiskMBps, 0.0, 1.0)
                : 0.0;
            const double gpuUsageRatio = std::clamp(processRecord.gpuPercent / 100.0, 0.0, 1.0);
            const double netUsageRatio = (maxNetKBps > 0.0)
                ? std::clamp(processRecord.netKBps / maxNetKBps, 0.0, 1.0)
                : 0.0;

            applyUsageHighlight(rowItem, toColumnIndex(TableColumn::Cpu), cpuUsageRatio);
            applyUsageHighlight(rowItem, toColumnIndex(TableColumn::Ram), ramUsageRatio);
            applyUsageHighlight(rowItem, toColumnIndex(TableColumn::Disk), diskUsageRatio);
            applyUsageHighlight(rowItem, toColumnIndex(TableColumn::Gpu), gpuUsageRatio);
            applyUsageHighlight(rowItem, toColumnIndex(TableColumn::Net), netUsageRatio);
        }

        m_processTable->addTopLevelItem(rowItem);
    }

    m_processTable->setSortingEnabled(enableSorting);
    if (enableSorting)
    {
        // 恢复用户上一次排序选择，而不是强制回到 PID 升序。
        const int safeSortColumn =
            (previousSortColumn >= 0 && previousSortColumn < static_cast<int>(TableColumn::Count))
            ? previousSortColumn
            : toColumnIndex(TableColumn::Pid);
        m_processTable->header()->setSortIndicator(safeSortColumn, previousSortOrder);
        m_processTable->sortItems(safeSortColumn, previousSortOrder);
    }

    // 根据本轮数据刷新标题栏“占用总和”。
    updateUsageSummaryInHeader(displayRows);

    // 表格重建完成后恢复刷新绘制。
    m_processTable->setUpdatesEnabled(true);
    m_processTable->viewport()->update();

    // 重建完成后输出一条细粒度日志，便于分析 UI 刷新开销与排序状态。
    kLogEvent logEvent;
    dbg << logEvent
        << "[ProcessDock] rebuildTable 完成, rows=" << displayRows.size()
        << ", treeMode=" << (isTreeModeEnabled() ? "true" : "false")
        << ", sortingEnabled=" << (enableSorting ? "true" : "false")
        << ", sortColumn=" << (enableSorting ? m_processTable->header()->sortIndicatorSection() : -1)
        << eol;
}

void ProcessDock::updateUsageSummaryInHeader(const std::vector<DisplayRow>& displayRows)
{
    // 标题栏展示占用总和：对当前可见数据（排除退出保留行）做聚合。
    if (m_processTable == nullptr || m_processTable->headerItem() == nullptr)
    {
        return;
    }

    double totalCpuPercent = 0.0;
    double totalRamMB = 0.0;
    double totalDiskMBps = 0.0;
    double totalGpuPercent = 0.0;
    double totalNetKBps = 0.0;
    for (const DisplayRow& displayRow : displayRows)
    {
        if (displayRow.record == nullptr || displayRow.isExited)
        {
            continue;
        }

        // CPU 汇总按用户要求排除“System Idle Process”(PID=0) 的空闲占比。
        const bool isSystemIdleProcess =
            (displayRow.record->pid == 0) ||
            (QString::fromStdString(displayRow.record->processName).compare("System Idle Process", Qt::CaseInsensitive) == 0);
        if (!isSystemIdleProcess)
        {
            totalCpuPercent += displayRow.record->cpuPercent;
        }

        totalRamMB += displayRow.record->ramMB;
        totalDiskMBps += displayRow.record->diskMBps;
        totalGpuPercent += displayRow.record->gpuPercent;
        totalNetKBps += displayRow.record->netKBps;
    }

    // 非占用列保持原始列名；占用列追加“总和”文本（不使用 Σ 符号）。
    QTreeWidgetItem* headerItem = m_processTable->headerItem();
    headerItem->setText(toColumnIndex(TableColumn::Name), ProcessTableHeaders.at(toColumnIndex(TableColumn::Name)));
    headerItem->setText(toColumnIndex(TableColumn::Pid), ProcessTableHeaders.at(toColumnIndex(TableColumn::Pid)));
    headerItem->setText(
        toColumnIndex(TableColumn::Cpu),
        QString("CPU %1%").arg(totalCpuPercent, 0, 'f', 2));
    headerItem->setText(
        toColumnIndex(TableColumn::Ram),
        QString("RAM %1 MB").arg(totalRamMB, 0, 'f', 1));
    headerItem->setText(
        toColumnIndex(TableColumn::Disk),
        QString("DISK %1 MB/s").arg(totalDiskMBps, 0, 'f', 2));
    headerItem->setText(
        toColumnIndex(TableColumn::Gpu),
        QString("GPU %1%").arg(totalGpuPercent, 0, 'f', 1));
    headerItem->setText(
        toColumnIndex(TableColumn::Net),
        QString("Net %1 KB/s").arg(totalNetKBps, 0, 'f', 2));
    headerItem->setText(toColumnIndex(TableColumn::Signature), ProcessTableHeaders.at(toColumnIndex(TableColumn::Signature)));
    headerItem->setText(toColumnIndex(TableColumn::Path), ProcessTableHeaders.at(toColumnIndex(TableColumn::Path)));
    headerItem->setText(toColumnIndex(TableColumn::ParentPid), ProcessTableHeaders.at(toColumnIndex(TableColumn::ParentPid)));
    headerItem->setText(toColumnIndex(TableColumn::CommandLine), ProcessTableHeaders.at(toColumnIndex(TableColumn::CommandLine)));
    headerItem->setText(toColumnIndex(TableColumn::User), ProcessTableHeaders.at(toColumnIndex(TableColumn::User)));
    headerItem->setText(toColumnIndex(TableColumn::StartTime), ProcessTableHeaders.at(toColumnIndex(TableColumn::StartTime)));
    headerItem->setText(toColumnIndex(TableColumn::IsAdmin), ProcessTableHeaders.at(toColumnIndex(TableColumn::IsAdmin)));
}

std::vector<ProcessDock::DisplayRow> ProcessDock::buildDisplayOrder() const
{
    return isTreeModeEnabled() ? buildTreeDisplayOrder() : buildListDisplayOrder();
}

std::vector<ProcessDock::DisplayRow> ProcessDock::buildListDisplayOrder() const
{
    std::vector<DisplayRow> displayRows;
    displayRows.reserve(m_cacheByIdentity.size());

    for (const auto& cachePair : m_cacheByIdentity)
    {
        DisplayRow displayRow{};
        displayRow.record = const_cast<ks::process::ProcessRecord*>(&cachePair.second.record);
        displayRow.depth = 0;
        displayRow.isNew = cachePair.second.isNewInLatestRound;
        displayRow.isExited = cachePair.second.isExitedInLatestRound;
        displayRows.push_back(displayRow);
    }

    std::sort(displayRows.begin(), displayRows.end(), [](const DisplayRow& leftRow, const DisplayRow& rightRow) {
        if (leftRow.record == nullptr || rightRow.record == nullptr)
        {
            return false;
        }
        return leftRow.record->pid < rightRow.record->pid;
    });
    return displayRows;
}

std::vector<ProcessDock::DisplayRow> ProcessDock::buildTreeDisplayOrder() const
{
    // Step1：把缓存转换成便于处理的指针数组。
    struct Node
    {
        const std::string* identityKey = nullptr;
        const CacheEntry* cacheEntry = nullptr;
    };
    std::vector<Node> nodes;
    nodes.reserve(m_cacheByIdentity.size());
    for (const auto& cachePair : m_cacheByIdentity)
    {
        nodes.push_back(Node{ &cachePair.first, &cachePair.second });
    }

    // Step2：构建 parentPid -> child 节点列表。
    std::unordered_map<std::uint32_t, std::vector<Node>> childrenByParentPid;
    std::unordered_set<std::uint32_t> existingPidSet;
    for (const Node& node : nodes)
    {
        if (node.cacheEntry == nullptr)
        {
            continue;
        }
        existingPidSet.insert(node.cacheEntry->record.pid);
        childrenByParentPid[node.cacheEntry->record.parentPid].push_back(node);
    }

    // 子列表按 PID 排序，保证同层稳定顺序。
    for (auto& pair : childrenByParentPid)
    {
        auto& childNodes = pair.second;
        std::sort(childNodes.begin(), childNodes.end(), [](const Node& leftNode, const Node& rightNode) {
            return leftNode.cacheEntry->record.pid < rightNode.cacheEntry->record.pid;
        });
    }

    // Step3：找到根节点（父 PID 不存在或为 0）。
    std::vector<Node> rootNodes;
    for (const Node& node : nodes)
    {
        const std::uint32_t parentPid = node.cacheEntry->record.parentPid;
        if (parentPid == 0 || existingPidSet.find(parentPid) == existingPidSet.end())
        {
            rootNodes.push_back(node);
        }
    }
    std::sort(rootNodes.begin(), rootNodes.end(), [](const Node& leftNode, const Node& rightNode) {
        return leftNode.cacheEntry->record.pid < rightNode.cacheEntry->record.pid;
    });

    // Step4：DFS 生成“树状列表顺序 + 缩进深度”。
    std::vector<DisplayRow> displayRows;
    std::unordered_set<std::string> visitedIdentitySet;

    std::function<void(const Node&, int)> appendNode;
    appendNode = [&](const Node& node, const int depth)
        {
            if (node.identityKey == nullptr || node.cacheEntry == nullptr)
            {
                return;
            }
            if (visitedIdentitySet.find(*node.identityKey) != visitedIdentitySet.end())
            {
                return;
            }
            visitedIdentitySet.insert(*node.identityKey);

            DisplayRow displayRow{};
            displayRow.record = const_cast<ks::process::ProcessRecord*>(&node.cacheEntry->record);
            displayRow.depth = depth;
            displayRow.isNew = node.cacheEntry->isNewInLatestRound;
            displayRow.isExited = node.cacheEntry->isExitedInLatestRound;
            displayRows.push_back(displayRow);

            const auto childIt = childrenByParentPid.find(node.cacheEntry->record.pid);
            if (childIt == childrenByParentPid.end())
            {
                return;
            }
            for (const Node& childNode : childIt->second)
            {
                appendNode(childNode, depth + 1);
            }
        };

    for (const Node& rootNode : rootNodes)
    {
        appendNode(rootNode, 0);
    }

    // 兜底：若仍有未访问节点（极端 parent 环），直接平铺补入。
    for (const Node& node : nodes)
    {
        if (node.identityKey == nullptr || node.cacheEntry == nullptr)
        {
            continue;
        }
        if (visitedIdentitySet.find(*node.identityKey) != visitedIdentitySet.end())
        {
            continue;
        }
        DisplayRow fallbackRow{};
        fallbackRow.record = const_cast<ks::process::ProcessRecord*>(&node.cacheEntry->record);
        fallbackRow.depth = 0;
        fallbackRow.isNew = node.cacheEntry->isNewInLatestRound;
        fallbackRow.isExited = node.cacheEntry->isExitedInLatestRound;
        displayRows.push_back(fallbackRow);
    }

    return displayRows;
}

void ProcessDock::showTableContextMenu(const QPoint& localPosition)
{
    QTreeWidgetItem* clickedItem = m_processTable->itemAt(localPosition);
    if (clickedItem == nullptr)
    {
        clearContextActionBinding();
        return;
    }
    m_processTable->setCurrentItem(clickedItem);
    const int clickedColumn = m_processTable->columnAt(localPosition.x());
    if (clickedColumn >= 0)
    {
        m_processTable->setCurrentItem(clickedItem, clickedColumn);
    }
    bindContextActionToItem(clickedItem);

    QMenu contextMenu(this);
    QAction* copyCellAction = contextMenu.addAction(blueTintedIcon(":/Icon/process_copy_cell.svg"), "复制单元格");
    QAction* copyRowAction = contextMenu.addAction(blueTintedIcon(":/Icon/process_copy_row.svg"), "复制行");
    contextMenu.addSeparator();

    // 结束进程二级菜单。
    QMenu* killSubMenu = contextMenu.addMenu(blueTintedIcon(":/Icon/process_terminate.svg"), "结束进程");
    QAction* taskkillAction = killSubMenu->addAction(blueTintedIcon(":/Icon/process_terminate.svg"), "Taskkill");
    QAction* taskkillForceAction = killSubMenu->addAction(blueTintedIcon(":/Icon/process_terminate.svg"), "Taskkill /f");
    QAction* terminateProcessAction = killSubMenu->addAction(blueTintedIcon(":/Icon/process_terminate.svg"), "TerminateProcess");
    QAction* terminateThreadsAction = killSubMenu->addAction(blueTintedIcon(":/Icon/process_terminate.svg"), "TerminateThread(全部线程)");
    QAction* injectInvalidShellcodeAction = killSubMenu->addAction(blueTintedIcon(":/Icon/process_terminate.svg"), "注入无效shellcode");

    QAction* suspendAction = contextMenu.addAction(blueTintedIcon(":/Icon/process_suspend.svg"), "挂起进程");
    QAction* resumeAction = contextMenu.addAction(blueTintedIcon(":/Icon/process_resume.svg"), "恢复进程");
    QAction* setCriticalAction = contextMenu.addAction(blueTintedIcon(":/Icon/process_critical.svg"), "设为关键进程");
    QAction* clearCriticalAction = contextMenu.addAction(blueTintedIcon(":/Icon/process_uncritical.svg"), "取消关键进程");
    QAction* openFolderAction = contextMenu.addAction(blueTintedIcon(":/Icon/process_open_folder.svg"), "打开所在目录");

    // 优先级二级菜单。
    QMenu* prioritySubMenu = contextMenu.addMenu(blueTintedIcon(":/Icon/process_priority.svg"), "设置进程优先级");
    QAction* idlePriority = prioritySubMenu->addAction(blueTintedIcon(":/Icon/process_priority.svg"), "Idle");
    QAction* belowNormalPriority = prioritySubMenu->addAction(blueTintedIcon(":/Icon/process_priority.svg"), "Below Normal");
    QAction* normalPriority = prioritySubMenu->addAction(blueTintedIcon(":/Icon/process_priority.svg"), "Normal");
    QAction* aboveNormalPriority = prioritySubMenu->addAction(blueTintedIcon(":/Icon/process_priority.svg"), "Above Normal");
    QAction* highPriority = prioritySubMenu->addAction(blueTintedIcon(":/Icon/process_priority.svg"), "High");
    QAction* realtimePriority = prioritySubMenu->addAction(blueTintedIcon(":/Icon/process_priority.svg"), "Realtime");
    idlePriority->setData(0);
    belowNormalPriority->setData(1);
    normalPriority->setData(2);
    aboveNormalPriority->setData(3);
    highPriority->setData(4);
    realtimePriority->setData(5);

    QAction* detailsAction = contextMenu.addAction(blueTintedIcon(":/Icon/process_details.svg"), "进程详细信息");

    m_contextMenuVisible = true;
    QAction* selectedAction = contextMenu.exec(m_processTable->viewport()->mapToGlobal(localPosition));
    m_contextMenuVisible = false;
    if (selectedAction == nullptr)
    {
        clearContextActionBinding();
        return;
    }

    {
        kLogEvent logEvent;
        info << logEvent
            << "[ProcessDock] 右键菜单执行动作: " << selectedAction->text().toStdString()
            << eol;
    }

    if (selectedAction == copyCellAction) { copyCurrentCell(); }
    else if (selectedAction == copyRowAction) { copyCurrentRow(); }
    else if (selectedAction == taskkillAction) { executeTaskKillAction(false); }
    else if (selectedAction == taskkillForceAction) { executeTaskKillAction(true); }
    else if (selectedAction == terminateProcessAction) { executeTerminateProcessAction(); }
    else if (selectedAction == terminateThreadsAction) { executeTerminateThreadsAction(); }
    else if (selectedAction == injectInvalidShellcodeAction) { executeInjectInvalidShellcodeAction(); }
    else if (selectedAction == suspendAction) { executeSuspendAction(); }
    else if (selectedAction == resumeAction) { executeResumeAction(); }
    else if (selectedAction == setCriticalAction) { executeSetCriticalAction(true); }
    else if (selectedAction == clearCriticalAction) { executeSetCriticalAction(false); }
    else if (selectedAction == openFolderAction) { executeOpenFolderAction(); }
    else if (selectedAction == detailsAction) { openProcessDetailsPlaceholder(); }
    else if (selectedAction->parent() == prioritySubMenu)
    {
        executeSetPriorityAction(selectedAction->data().toInt());
    }

    clearContextActionBinding();
}

void ProcessDock::showHeaderContextMenu(const QPoint& localPosition)
{
    Q_UNUSED(localPosition);

    // 每列一个勾选动作，允许用户动态显示/隐藏。
    QMenu columnMenu(this);
    for (int columnIndex = 0; columnIndex < static_cast<int>(TableColumn::Count); ++columnIndex)
    {
        QAction* toggleAction = columnMenu.addAction(ProcessTableHeaders.at(columnIndex));
        toggleAction->setCheckable(true);
        toggleAction->setChecked(!m_processTable->isColumnHidden(columnIndex));
        toggleAction->setData(columnIndex);
    }

    QAction* selectedAction = columnMenu.exec(QCursor::pos());
    if (selectedAction == nullptr)
    {
        return;
    }

    const int columnIndex = selectedAction->data().toInt();
    const bool shouldShow = selectedAction->isChecked();
    m_processTable->setColumnHidden(columnIndex, !shouldShow);
    applyAdaptiveColumnWidths();

    kLogEvent logEvent;
    info << logEvent
        << "[ProcessDock] 列显示状态变更, column=" << columnIndex
        << ", header=" << ProcessTableHeaders.value(columnIndex).toStdString()
        << ", visible=" << (shouldShow ? "true" : "false")
        << eol;
}

void ProcessDock::copyCurrentCell()
{
    QTreeWidgetItem* currentItem = m_processTable->currentItem();
    if (currentItem == nullptr)
    {
        return;
    }

    const int currentColumn = m_processTable->currentColumn();
    if (currentColumn < 0)
    {
        return;
    }
    QApplication::clipboard()->setText(currentItem->text(currentColumn));

    kLogEvent logEvent;
    dbg << logEvent
        << "[ProcessDock] 复制单元格, column=" << currentColumn
        << ", text=" << currentItem->text(currentColumn).toStdString()
        << eol;
}

void ProcessDock::copyCurrentRow()
{
    QTreeWidgetItem* currentItem = m_processTable->currentItem();
    if (currentItem == nullptr)
    {
        return;
    }

    QStringList rowFields;
    rowFields.reserve(static_cast<int>(TableColumn::Count));
    for (int columnIndex = 0; columnIndex < static_cast<int>(TableColumn::Count); ++columnIndex)
    {
        rowFields.push_back(currentItem->text(columnIndex));
    }
    QApplication::clipboard()->setText(rowFields.join("\t"));

    kLogEvent logEvent;
    dbg << logEvent
        << "[ProcessDock] 复制整行, text=" << rowFields.join("\t").toStdString()
        << eol;
}

void ProcessDock::bindContextActionToItem(QTreeWidgetItem* clickedItem)
{
    clearContextActionBinding();
    if (clickedItem == nullptr)
    {
        return;
    }

    m_contextActionIdentityKey = clickedItem->data(0, Qt::UserRole).toString().toStdString();
    if (m_contextActionIdentityKey.empty())
    {
        return;
    }

    const auto cacheIt = m_cacheByIdentity.find(m_contextActionIdentityKey);
    if (cacheIt != m_cacheByIdentity.end())
    {
        m_contextActionRecord = cacheIt->second.record;
        m_hasContextActionRecord = true;
        return;
    }

    // 若刷新刚好重建导致 identity 缓存缺失，尝试用当前行 PID 兜底。
    bool pidParseOk = false;
    const std::uint32_t pidValue = clickedItem->text(toColumnIndex(TableColumn::Pid)).toUInt(&pidParseOk);
    if (pidParseOk)
    {
        m_contextActionRecord = {};
        m_contextActionRecord.pid = pidValue;
        m_contextActionRecord.processName = clickedItem->text(toColumnIndex(TableColumn::Name)).toStdString();
        m_hasContextActionRecord = true;
    }
}

void ProcessDock::clearContextActionBinding()
{
    m_contextActionIdentityKey.clear();
    m_hasContextActionRecord = false;
    m_contextMenuVisible = false;
}

std::string ProcessDock::selectedIdentityKey() const
{
    if (!m_contextActionIdentityKey.empty())
    {
        return m_contextActionIdentityKey;
    }

    QTreeWidgetItem* currentItem = m_processTable->currentItem();
    if (currentItem == nullptr)
    {
        return std::string();
    }
    return currentItem->data(0, Qt::UserRole).toString().toStdString();
}

ks::process::ProcessRecord* ProcessDock::selectedRecord()
{
    const std::string identityKey = selectedIdentityKey();
    if (identityKey.empty())
    {
        return nullptr;
    }

    auto cacheIt = m_cacheByIdentity.find(identityKey);
    if (cacheIt == m_cacheByIdentity.end())
    {
        if (m_hasContextActionRecord)
        {
            return &m_contextActionRecord;
        }
        return nullptr;
    }
    return &cacheIt->second.record;
}

QString ProcessDock::formatColumnText(const ks::process::ProcessRecord& processRecord, const TableColumn column, const int depth) const
{
    switch (column)
    {
    case TableColumn::Name:
        return QString::fromStdString(buildRulerPrefix(depth) + processRecord.processName);
    case TableColumn::Pid:
        return QString::number(processRecord.pid);
    case TableColumn::Cpu:
        // CPU 改为两位小数，避免低占用进程全部显示 0.0 的视觉误差。
        return QString::number(processRecord.cpuPercent, 'f', 2) + "%";
    case TableColumn::Ram:
        return QString::number(processRecord.ramMB, 'f', 1) + " MB";
    case TableColumn::Disk:
        return QString::number(processRecord.diskMBps, 'f', 2) + " MB/s";
    case TableColumn::Gpu:
        return QString::number(processRecord.gpuPercent, 'f', 1) + "%";
    case TableColumn::Net:
        return QString::number(processRecord.netKBps, 'f', 2) + " KB/s";
    case TableColumn::Signature:
        // 显示“厂家 + 可信状态”文本，未填充时显示 Unknown。
        return QString::fromStdString(processRecord.signatureState.empty() ? "Unknown" : processRecord.signatureState);
    case TableColumn::Path:
        return QString::fromStdString(processRecord.imagePath.empty() ? "-" : processRecord.imagePath);
    case TableColumn::ParentPid:
        return QString::number(processRecord.parentPid);
    case TableColumn::CommandLine:
        return QString::fromStdString(processRecord.commandLine.empty() ? "-" : processRecord.commandLine);
    case TableColumn::User:
        return QString::fromStdString(processRecord.userName.empty() ? "-" : processRecord.userName);
    case TableColumn::StartTime:
        return QString::fromStdString(processRecord.startTimeText);
    case TableColumn::IsAdmin:
        // 用方块 + 文本表示管理员状态（颜色在重建表格时设置）。
        return processRecord.isAdmin ? "■ 是" : "■ 否";
    default:
        return QString();
    }
}

QIcon ProcessDock::resolveProcessIcon(const ks::process::ProcessRecord& processRecord)
{
    // 仅使用后台缓存中的 imagePath：
    // - 禁止在 UI 线程里按 PID 再查路径，避免刷新阶段出现卡顿。
    QString pathText = QString::fromStdString(processRecord.imagePath);
    if (pathText.trimmed().isEmpty())
    {
        return QIcon(":/Icon/process_main.svg");
    }

    auto iconIt = m_iconCacheByPath.find(pathText);
    if (iconIt != m_iconCacheByPath.end())
    {
        return iconIt.value();
    }

    // 先尝试直接从 EXE 路径构造图标（期望拿到真实程序图标）。
    QIcon processIcon(pathText);
    if (processIcon.isNull())
    {
        QFileIconProvider iconProvider;
        processIcon = iconProvider.icon(QFileInfo(pathText));
    }
    if (processIcon.isNull())
    {
        processIcon = QIcon(":/Icon/process_main.svg");
    }
    m_iconCacheByPath.insert(pathText, processIcon);
    return processIcon;
}

QIcon ProcessDock::blueTintedIcon(const char* iconPath, const QSize& iconSize) const
{
    QSvgRenderer renderer(QString::fromUtf8(iconPath));
    if (!renderer.isValid())
    {
        return QIcon(QString::fromUtf8(iconPath));
    }

    QPixmap iconPixmap(iconSize);
    iconPixmap.fill(Qt::transparent);

    QPainter painter(&iconPixmap);
    painter.setRenderHint(QPainter::Antialiasing, true);
    renderer.render(&painter, QRectF(0, 0, iconSize.width(), iconSize.height()));
    painter.setCompositionMode(QPainter::CompositionMode_SourceIn);
    painter.fillRect(iconPixmap.rect(), KswordTheme::PrimaryBlueColor);
    painter.end();
    return QIcon(iconPixmap);
}

void ProcessDock::showActionResultMessage(
    const QString& title,
    const bool actionOk,
    const std::string& detailText,
    const kLogEvent& actionEvent)
{
    // 统一动作结果日志：按照规范不再弹窗，避免频繁打断用户流程。
    const std::string normalizedDetailText = detailText.empty() ? "无附加信息" : detailText;
    (actionOk ? info : err) << actionEvent
        << "[ProcessDock] 动作结果, title=" << title.toStdString()
        << ", actionOk=" << (actionOk ? "true" : "false")
        << ", detail=" << normalizedDetailText
        << eol;
}

std::string ProcessDock::buildRulerPrefix(const int depth)
{
    if (depth <= 0)
    {
        return std::string();
    }

    std::string prefixText;
    for (int index = 0; index < depth; ++index)
    {
        prefixText += (index + 1 == depth) ? "└─ " : "│  ";
    }
    return prefixText;
}

int ProcessDock::toColumnIndex(const TableColumn column)
{
    return static_cast<int>(column);
}

void ProcessDock::syncEditValueFromBitmaskChecks(
    QLineEdit* const valueEdit,
    const std::vector<QCheckBox*>* const checkBoxList)
{
    // 参数合法性检查：任一为空直接返回，避免空指针访问。
    if (valueEdit == nullptr || checkBoxList == nullptr)
    {
        return;
    }

    // 先计算“所有已知位掩码 + 已勾选位掩码”，
    // 以便在回写时保留用户手工输入但列表中未覆盖的未知位。
    std::uint32_t knownMask = 0;
    std::uint32_t checkedMask = 0;
    for (QCheckBox* checkBox : *checkBoxList)
    {
        if (checkBox == nullptr)
        {
            continue;
        }
        bool convertOk = false;
        const std::uint32_t flagValue = static_cast<std::uint32_t>(
            checkBox->property("flagValue").toULongLong(&convertOk));
        if (!convertOk)
        {
            continue;
        }

        knownMask |= flagValue;
        if (checkBox->isChecked())
        {
            checkedMask |= flagValue;
        }
    }

    // 保留未知位：避免勾选一个已知位时误清空手工填入的其他位。
    bool parseOk = false;
    const std::uint32_t originalValue = parseUInt32WithDefault(valueEdit->text(), 0, &parseOk);
    const std::uint32_t unknownMask = parseOk ? (originalValue & ~knownMask) : 0;
    const std::uint32_t mergedValue = (checkedMask | unknownMask);

    const QString mergedText = QStringLiteral("0x%1")
        .arg(static_cast<qulonglong>(mergedValue), 8, 16, QChar('0'))
        .toUpper();
    if (valueEdit->text().compare(mergedText, Qt::CaseInsensitive) == 0)
    {
        return;
    }

    // 阻断 textChanged 信号，防止“回写文本 -> 再次反向同步”递归触发。
    const QSignalBlocker blocker(valueEdit);
    valueEdit->setText(mergedText);
}

void ProcessDock::syncBitmaskChecksFromEditValue(
    QLineEdit* const valueEdit,
    const std::vector<QCheckBox*>* const checkBoxList,
    const QString& fieldDisplayName)
{
    // 参数合法性检查：任一为空直接返回。
    if (valueEdit == nullptr || checkBoxList == nullptr)
    {
        return;
    }

    // 解析失败时仅跳过勾选同步，不主动覆写用户输入内容。
    bool parseOk = false;
    const std::uint32_t editValue = parseUInt32WithDefault(valueEdit->text(), 0, &parseOk);
    if (!parseOk)
    {
        Q_UNUSED(fieldDisplayName);
        return;
    }

    // 按输入值逐项更新勾选状态；使用 QSignalBlocker 防止触发 toggled 回调。
    for (QCheckBox* checkBox : *checkBoxList)
    {
        if (checkBox == nullptr)
        {
            continue;
        }

        bool convertOk = false;
        const std::uint32_t flagValue = static_cast<std::uint32_t>(
            checkBox->property("flagValue").toULongLong(&convertOk));
        if (!convertOk || flagValue == 0)
        {
            continue;
        }

        const bool shouldChecked = ((editValue & flagValue) == flagValue);
        if (checkBox->isChecked() == shouldChecked)
        {
            continue;
        }

        const QSignalBlocker blocker(checkBox);
        checkBox->setChecked(shouldChecked);
    }
}

void ProcessDock::bindBitmaskEditor(
    QLineEdit* const valueEdit,
    std::vector<QCheckBox*>* const checkBoxList,
    const QString& fieldDisplayName)
{
    // 参数校验：没有输入框或没有复选框列表则不绑定。
    if (valueEdit == nullptr || checkBoxList == nullptr)
    {
        return;
    }

    // 复选框 -> 文本框：每次勾选变化都回算位掩码。
    for (QCheckBox* checkBox : *checkBoxList)
    {
        if (checkBox == nullptr)
        {
            continue;
        }

        connect(checkBox, &QCheckBox::toggled, this, [this, valueEdit, checkBoxList, fieldDisplayName](bool) {
            syncEditValueFromBitmaskChecks(valueEdit, checkBoxList);

            kLogEvent logEvent;
            dbg << logEvent
                << "[ProcessDock] 位标志勾选变更, field="
                << fieldDisplayName.toStdString()
                << ", value="
                << valueEdit->text().toStdString()
                << eol;
            });
    }

    // 文本框 -> 复选框：支持用户手工输入十进制或 0x 十六进制。
    connect(valueEdit, &QLineEdit::textChanged, this, [this, valueEdit, checkBoxList, fieldDisplayName](const QString&) {
        syncBitmaskChecksFromEditValue(valueEdit, checkBoxList, fieldDisplayName);
        });

    // 初始同步：页面打开时让默认值和复选框状态一致。
    syncBitmaskChecksFromEditValue(valueEdit, checkBoxList, fieldDisplayName);
}

bool ProcessDock::parseUnsignedText(const QString& text, std::uint64_t& valueOut)
{
    QString normalizedText = text.trimmed();
    if (normalizedText.isEmpty())
    {
        valueOut = 0;
        return true;
    }

    int numberBase = 10;
    if (normalizedText.startsWith("0x", Qt::CaseInsensitive))
    {
        normalizedText = normalizedText.mid(2);
        numberBase = 16;
    }
    else if (normalizedText.endsWith(QStringLiteral("h"), Qt::CaseInsensitive))
    {
        normalizedText.chop(1);
        numberBase = 16;
    }

    bool parseOk = false;
    const std::uint64_t parsedValue = normalizedText.toULongLong(&parseOk, numberBase);
    if (!parseOk)
    {
        valueOut = 0;
        return false;
    }
    valueOut = parsedValue;
    return true;
}

std::uint32_t ProcessDock::parseUInt32WithDefault(
    const QString& text,
    const std::uint32_t defaultValue,
    bool* const parseOkOut)
{
    std::uint64_t parsedValue = 0;
    if (!parseUnsignedText(text, parsedValue))
    {
        if (parseOkOut != nullptr)
        {
            *parseOkOut = false;
        }
        return defaultValue;
    }
    if (parsedValue > static_cast<std::uint64_t>(std::numeric_limits<std::uint32_t>::max()))
    {
        if (parseOkOut != nullptr)
        {
            *parseOkOut = false;
        }
        return defaultValue;
    }
    if (parseOkOut != nullptr)
    {
        *parseOkOut = true;
    }
    return static_cast<std::uint32_t>(parsedValue);
}

std::uint64_t ProcessDock::parseUInt64WithDefault(
    const QString& text,
    const std::uint64_t defaultValue,
    bool* const parseOkOut)
{
    std::uint64_t parsedValue = 0;
    if (!parseUnsignedText(text, parsedValue))
    {
        if (parseOkOut != nullptr)
        {
            *parseOkOut = false;
        }
        return defaultValue;
    }
    if (parseOkOut != nullptr)
    {
        *parseOkOut = true;
    }
    return parsedValue;
}

void ProcessDock::appendCreateResultLine(const QString& lineText)
{
    if (m_createResultOutput == nullptr)
    {
        return;
    }
    const QString timeText = QDateTime::currentDateTime().toString("HH:mm:ss");
    m_createResultOutput->append(QString("[%1] %2").arg(timeText, lineText));
}

void ProcessDock::browseCreateProcessApplicationPath()
{
    const QString filePath = QFileDialog::getOpenFileName(
        this,
        "选择可执行文件",
        m_applicationNameEdit != nullptr ? m_applicationNameEdit->text().trimmed() : QString(),
        "Executable (*.exe);;All Files (*.*)");
    if (filePath.isEmpty())
    {
        return;
    }

    if (m_applicationNameEdit != nullptr)
    {
        m_applicationNameEdit->setText(filePath);
    }
    if (m_useApplicationNameCheck != nullptr)
    {
        m_useApplicationNameCheck->setChecked(true);
    }
}

void ProcessDock::browseCreateProcessCurrentDirectory()
{
    const QString startPath = m_currentDirectoryEdit != nullptr
        ? m_currentDirectoryEdit->text().trimmed()
        : QString();
    const QString directoryPath = QFileDialog::getExistingDirectory(
        this,
        "选择工作目录",
        startPath);
    if (directoryPath.isEmpty())
    {
        return;
    }

    if (m_currentDirectoryEdit != nullptr)
    {
        m_currentDirectoryEdit->setText(directoryPath);
    }
    if (m_useCurrentDirectoryCheck != nullptr)
    {
        m_useCurrentDirectoryCheck->setChecked(true);
    }
}

void ProcessDock::resetCreateProcessForm()
{
    if (m_createMethodCombo != nullptr) m_createMethodCombo->setCurrentIndex(0);
    if (m_useApplicationNameCheck != nullptr) m_useApplicationNameCheck->setChecked(false);
    if (m_applicationNameEdit != nullptr) m_applicationNameEdit->clear();
    if (m_useCommandLineCheck != nullptr) m_useCommandLineCheck->setChecked(false);
    if (m_commandLineEdit != nullptr) m_commandLineEdit->clear();
    if (m_useCurrentDirectoryCheck != nullptr) m_useCurrentDirectoryCheck->setChecked(false);
    if (m_currentDirectoryEdit != nullptr) m_currentDirectoryEdit->clear();
    if (m_useEnvironmentCheck != nullptr) m_useEnvironmentCheck->setChecked(false);
    if (m_environmentUnicodeCheck != nullptr) m_environmentUnicodeCheck->setChecked(true);
    if (m_environmentEditor != nullptr) m_environmentEditor->clear();
    if (m_inheritHandleCheck != nullptr) m_inheritHandleCheck->setChecked(false);
    if (m_creationFlagsEdit != nullptr) m_creationFlagsEdit->setText("0x00000000");

    if (m_useProcessSecurityCheck != nullptr) m_useProcessSecurityCheck->setChecked(false);
    if (m_processSecurityLengthEdit != nullptr) m_processSecurityLengthEdit->setText("0");
    if (m_processSecurityDescriptorEdit != nullptr) m_processSecurityDescriptorEdit->setText("0");
    if (m_processSecurityInheritCheck != nullptr) m_processSecurityInheritCheck->setChecked(false);
    if (m_useThreadSecurityCheck != nullptr) m_useThreadSecurityCheck->setChecked(false);
    if (m_threadSecurityLengthEdit != nullptr) m_threadSecurityLengthEdit->setText("0");
    if (m_threadSecurityDescriptorEdit != nullptr) m_threadSecurityDescriptorEdit->setText("0");
    if (m_threadSecurityInheritCheck != nullptr) m_threadSecurityInheritCheck->setChecked(false);

    if (m_useStartupInfoCheck != nullptr) m_useStartupInfoCheck->setChecked(false);
    if (m_siCbEdit != nullptr) m_siCbEdit->setText("0");
    if (m_siReservedEdit != nullptr) m_siReservedEdit->clear();
    if (m_siDesktopEdit != nullptr) m_siDesktopEdit->clear();
    if (m_siTitleEdit != nullptr) m_siTitleEdit->clear();
    if (m_siXEdit != nullptr) m_siXEdit->setText("0");
    if (m_siYEdit != nullptr) m_siYEdit->setText("0");
    if (m_siXSizeEdit != nullptr) m_siXSizeEdit->setText("0");
    if (m_siYSizeEdit != nullptr) m_siYSizeEdit->setText("0");
    if (m_siXCountCharsEdit != nullptr) m_siXCountCharsEdit->setText("0");
    if (m_siYCountCharsEdit != nullptr) m_siYCountCharsEdit->setText("0");
    if (m_siFillAttributeEdit != nullptr) m_siFillAttributeEdit->setText("0x00000000");
    if (m_siFlagsEdit != nullptr) m_siFlagsEdit->setText("0x00000000");
    if (m_siShowWindowEdit != nullptr) m_siShowWindowEdit->setText("0");
    if (m_siCbReserved2Edit != nullptr) m_siCbReserved2Edit->setText("0");
    if (m_siReserved2PtrEdit != nullptr) m_siReserved2PtrEdit->setText("0");
    if (m_siStdInputEdit != nullptr) m_siStdInputEdit->setText("0");
    if (m_siStdOutputEdit != nullptr) m_siStdOutputEdit->setText("0");
    if (m_siStdErrorEdit != nullptr) m_siStdErrorEdit->setText("0");

    if (m_useProcessInfoCheck != nullptr) m_useProcessInfoCheck->setChecked(false);
    if (m_piProcessHandleEdit != nullptr) m_piProcessHandleEdit->setText("0");
    if (m_piThreadHandleEdit != nullptr) m_piThreadHandleEdit->setText("0");
    if (m_piPidEdit != nullptr) m_piPidEdit->setText("0");
    if (m_piTidEdit != nullptr) m_piTidEdit->setText("0");

    if (m_tokenSourcePidEdit != nullptr) m_tokenSourcePidEdit->setText("0");
    if (m_tokenDesiredAccessEdit != nullptr) m_tokenDesiredAccessEdit->setText("0x00000FAB");
    if (m_tokenDuplicatePrimaryCheck != nullptr) m_tokenDuplicatePrimaryCheck->setChecked(true);

    if (m_tokenPrivilegeTable != nullptr)
    {
        for (int row = 0; row < m_tokenPrivilegeTable->rowCount(); ++row)
        {
            QComboBox* actionCombo = qobject_cast<QComboBox*>(m_tokenPrivilegeTable->cellWidget(row, 1));
            if (actionCombo != nullptr)
            {
                actionCombo->setCurrentIndex(0);
            }
        }
    }

    if (m_createResultOutput != nullptr)
    {
        m_createResultOutput->clear();
    }
    appendCreateResultLine("已恢复创建进程表单默认值。");
}

ks::process::CreateProcessRequest ProcessDock::buildCreateProcessRequestFromUi(
    bool* const buildOk,
    QString* const errorTextOut) const
{
    ks::process::CreateProcessRequest request;
    if (buildOk != nullptr)
    {
        *buildOk = false;
    }

    const auto failBuild = [errorTextOut](const QString& textValue) {
        if (errorTextOut != nullptr)
        {
            *errorTextOut = textValue;
        }
        };

    request.useApplicationName = (m_useApplicationNameCheck != nullptr && m_useApplicationNameCheck->isChecked());
    request.applicationName = (m_applicationNameEdit != nullptr) ? m_applicationNameEdit->text().trimmed().toStdString() : std::string();

    request.useCommandLine = (m_useCommandLineCheck != nullptr && m_useCommandLineCheck->isChecked());
    request.commandLine = (m_commandLineEdit != nullptr) ? m_commandLineEdit->text().trimmed().toStdString() : std::string();

    request.useCurrentDirectory = (m_useCurrentDirectoryCheck != nullptr && m_useCurrentDirectoryCheck->isChecked());
    request.currentDirectory = (m_currentDirectoryEdit != nullptr) ? m_currentDirectoryEdit->text().trimmed().toStdString() : std::string();

    request.useEnvironment = (m_useEnvironmentCheck != nullptr && m_useEnvironmentCheck->isChecked());
    request.environmentUnicode = (m_environmentUnicodeCheck != nullptr && m_environmentUnicodeCheck->isChecked());
    if (request.useEnvironment && m_environmentEditor != nullptr)
    {
        const QStringList envLines = m_environmentEditor->toPlainText().split('\n');
        for (const QString& lineText : envLines)
        {
            const QString trimmedText = lineText.trimmed();
            if (!trimmedText.isEmpty())
            {
                request.environmentEntries.push_back(trimmedText.toStdString());
            }
        }
    }

    request.inheritHandles = (m_inheritHandleCheck != nullptr && m_inheritHandleCheck->isChecked());

    bool parseOk = false;
    request.creationFlags = parseUInt32WithDefault(
        m_creationFlagsEdit != nullptr ? m_creationFlagsEdit->text() : QString(),
        0,
        &parseOk);
    if (!parseOk)
    {
        failBuild("dwCreationFlags 解析失败，请输入十进制或 0x 十六进制。");
        return request;
    }

    request.processAttributes.useValue = (m_useProcessSecurityCheck != nullptr && m_useProcessSecurityCheck->isChecked());
    if (request.processAttributes.useValue)
    {
        request.processAttributes.nLength = parseUInt32WithDefault(
            m_processSecurityLengthEdit != nullptr ? m_processSecurityLengthEdit->text() : QString(),
            0,
            &parseOk);
        if (!parseOk)
        {
            failBuild("Process SECURITY_ATTRIBUTES.nLength 解析失败。");
            return request;
        }
        request.processAttributes.securityDescriptor = parseUInt64WithDefault(
            m_processSecurityDescriptorEdit != nullptr ? m_processSecurityDescriptorEdit->text() : QString(),
            0,
            &parseOk);
        if (!parseOk)
        {
            failBuild("Process SECURITY_ATTRIBUTES.lpSecurityDescriptor 解析失败。");
            return request;
        }
        request.processAttributes.inheritHandle = (m_processSecurityInheritCheck != nullptr && m_processSecurityInheritCheck->isChecked());
    }

    request.threadAttributes.useValue = (m_useThreadSecurityCheck != nullptr && m_useThreadSecurityCheck->isChecked());
    if (request.threadAttributes.useValue)
    {
        request.threadAttributes.nLength = parseUInt32WithDefault(
            m_threadSecurityLengthEdit != nullptr ? m_threadSecurityLengthEdit->text() : QString(),
            0,
            &parseOk);
        if (!parseOk)
        {
            failBuild("Thread SECURITY_ATTRIBUTES.nLength 解析失败。");
            return request;
        }
        request.threadAttributes.securityDescriptor = parseUInt64WithDefault(
            m_threadSecurityDescriptorEdit != nullptr ? m_threadSecurityDescriptorEdit->text() : QString(),
            0,
            &parseOk);
        if (!parseOk)
        {
            failBuild("Thread SECURITY_ATTRIBUTES.lpSecurityDescriptor 解析失败。");
            return request;
        }
        request.threadAttributes.inheritHandle = (m_threadSecurityInheritCheck != nullptr && m_threadSecurityInheritCheck->isChecked());
    }

    request.startupInfo.useValue = (m_useStartupInfoCheck != nullptr && m_useStartupInfoCheck->isChecked());
    if (request.startupInfo.useValue)
    {
        request.startupInfo.cb = parseUInt32WithDefault(m_siCbEdit != nullptr ? m_siCbEdit->text() : QString(), 0, &parseOk);
        if (!parseOk) { failBuild("STARTUPINFO.cb 解析失败。"); return request; }
        request.startupInfo.lpReserved = (m_siReservedEdit != nullptr ? m_siReservedEdit->text() : QString()).toStdString();
        request.startupInfo.lpDesktop = (m_siDesktopEdit != nullptr ? m_siDesktopEdit->text() : QString()).toStdString();
        request.startupInfo.lpTitle = (m_siTitleEdit != nullptr ? m_siTitleEdit->text() : QString()).toStdString();
        request.startupInfo.dwX = parseUInt32WithDefault(m_siXEdit != nullptr ? m_siXEdit->text() : QString(), 0, &parseOk);
        if (!parseOk) { failBuild("STARTUPINFO.dwX 解析失败。"); return request; }
        request.startupInfo.dwY = parseUInt32WithDefault(m_siYEdit != nullptr ? m_siYEdit->text() : QString(), 0, &parseOk);
        if (!parseOk) { failBuild("STARTUPINFO.dwY 解析失败。"); return request; }
        request.startupInfo.dwXSize = parseUInt32WithDefault(m_siXSizeEdit != nullptr ? m_siXSizeEdit->text() : QString(), 0, &parseOk);
        if (!parseOk) { failBuild("STARTUPINFO.dwXSize 解析失败。"); return request; }
        request.startupInfo.dwYSize = parseUInt32WithDefault(m_siYSizeEdit != nullptr ? m_siYSizeEdit->text() : QString(), 0, &parseOk);
        if (!parseOk) { failBuild("STARTUPINFO.dwYSize 解析失败。"); return request; }
        request.startupInfo.dwXCountChars = parseUInt32WithDefault(m_siXCountCharsEdit != nullptr ? m_siXCountCharsEdit->text() : QString(), 0, &parseOk);
        if (!parseOk) { failBuild("STARTUPINFO.dwXCountChars 解析失败。"); return request; }
        request.startupInfo.dwYCountChars = parseUInt32WithDefault(m_siYCountCharsEdit != nullptr ? m_siYCountCharsEdit->text() : QString(), 0, &parseOk);
        if (!parseOk) { failBuild("STARTUPINFO.dwYCountChars 解析失败。"); return request; }
        request.startupInfo.dwFillAttribute = parseUInt32WithDefault(m_siFillAttributeEdit != nullptr ? m_siFillAttributeEdit->text() : QString(), 0, &parseOk);
        if (!parseOk) { failBuild("STARTUPINFO.dwFillAttribute 解析失败。"); return request; }
        request.startupInfo.dwFlags = parseUInt32WithDefault(m_siFlagsEdit != nullptr ? m_siFlagsEdit->text() : QString(), 0, &parseOk);
        if (!parseOk) { failBuild("STARTUPINFO.dwFlags 解析失败。"); return request; }
        request.startupInfo.wShowWindow = static_cast<std::uint16_t>(
            parseUInt32WithDefault(m_siShowWindowEdit != nullptr ? m_siShowWindowEdit->text() : QString(), 0, &parseOk));
        if (!parseOk) { failBuild("STARTUPINFO.wShowWindow 解析失败。"); return request; }
        request.startupInfo.cbReserved2 = static_cast<std::uint16_t>(
            parseUInt32WithDefault(m_siCbReserved2Edit != nullptr ? m_siCbReserved2Edit->text() : QString(), 0, &parseOk));
        if (!parseOk) { failBuild("STARTUPINFO.cbReserved2 解析失败。"); return request; }
        request.startupInfo.lpReserved2 = parseUInt64WithDefault(m_siReserved2PtrEdit != nullptr ? m_siReserved2PtrEdit->text() : QString(), 0, &parseOk);
        if (!parseOk) { failBuild("STARTUPINFO.lpReserved2 解析失败。"); return request; }
        request.startupInfo.hStdInput = parseUInt64WithDefault(m_siStdInputEdit != nullptr ? m_siStdInputEdit->text() : QString(), 0, &parseOk);
        if (!parseOk) { failBuild("STARTUPINFO.hStdInput 解析失败。"); return request; }
        request.startupInfo.hStdOutput = parseUInt64WithDefault(m_siStdOutputEdit != nullptr ? m_siStdOutputEdit->text() : QString(), 0, &parseOk);
        if (!parseOk) { failBuild("STARTUPINFO.hStdOutput 解析失败。"); return request; }
        request.startupInfo.hStdError = parseUInt64WithDefault(m_siStdErrorEdit != nullptr ? m_siStdErrorEdit->text() : QString(), 0, &parseOk);
        if (!parseOk) { failBuild("STARTUPINFO.hStdError 解析失败。"); return request; }
    }

    request.processInfo.useValue = (m_useProcessInfoCheck != nullptr && m_useProcessInfoCheck->isChecked());
    if (request.processInfo.useValue)
    {
        request.processInfo.hProcess = parseUInt64WithDefault(m_piProcessHandleEdit != nullptr ? m_piProcessHandleEdit->text() : QString(), 0, &parseOk);
        if (!parseOk) { failBuild("PROCESS_INFORMATION.hProcess 解析失败。"); return request; }
        request.processInfo.hThread = parseUInt64WithDefault(m_piThreadHandleEdit != nullptr ? m_piThreadHandleEdit->text() : QString(), 0, &parseOk);
        if (!parseOk) { failBuild("PROCESS_INFORMATION.hThread 解析失败。"); return request; }
        request.processInfo.dwProcessId = parseUInt32WithDefault(m_piPidEdit != nullptr ? m_piPidEdit->text() : QString(), 0, &parseOk);
        if (!parseOk) { failBuild("PROCESS_INFORMATION.dwProcessId 解析失败。"); return request; }
        request.processInfo.dwThreadId = parseUInt32WithDefault(m_piTidEdit != nullptr ? m_piTidEdit->text() : QString(), 0, &parseOk);
        if (!parseOk) { failBuild("PROCESS_INFORMATION.dwThreadId 解析失败。"); return request; }
    }

    request.tokenModeEnabled = (m_createMethodCombo != nullptr && m_createMethodCombo->currentIndex() == 1);
    if (request.tokenModeEnabled)
    {
        request.tokenSourcePid = parseUInt32WithDefault(
            m_tokenSourcePidEdit != nullptr ? m_tokenSourcePidEdit->text() : QString(),
            0,
            &parseOk);
        if (!parseOk)
        {
            failBuild("Token 模式 source PID 解析失败。");
            return request;
        }
        request.tokenDesiredAccess = parseUInt32WithDefault(
            m_tokenDesiredAccessEdit != nullptr ? m_tokenDesiredAccessEdit->text() : QString(),
            0,
            &parseOk);
        if (!parseOk)
        {
            failBuild("Token 模式 desired access 解析失败。");
            return request;
        }
        request.duplicatePrimaryToken = (m_tokenDuplicatePrimaryCheck != nullptr && m_tokenDuplicatePrimaryCheck->isChecked());

        if (m_tokenPrivilegeTable != nullptr)
        {
            for (int row = 0; row < m_tokenPrivilegeTable->rowCount(); ++row)
            {
                QTableWidgetItem* privilegeItem = m_tokenPrivilegeTable->item(row, 0);
                QComboBox* actionCombo = qobject_cast<QComboBox*>(m_tokenPrivilegeTable->cellWidget(row, 1));
                if (privilegeItem == nullptr || actionCombo == nullptr)
                {
                    continue;
                }

                const auto actionValue = static_cast<ks::process::TokenPrivilegeAction>(
                    actionCombo->currentData().toInt());
                if (actionValue == ks::process::TokenPrivilegeAction::Keep)
                {
                    continue;
                }

                ks::process::TokenPrivilegeEdit editItem{};
                editItem.privilegeName = privilegeItem->text().trimmed().toStdString();
                editItem.action = actionValue;
                request.tokenPrivilegeEdits.push_back(std::move(editItem));
            }
        }
    }

    if (buildOk != nullptr)
    {
        *buildOk = true;
    }
    if (errorTextOut != nullptr)
    {
        errorTextOut->clear();
    }
    return request;
}

void ProcessDock::executeApplyTokenPrivilegeEditsOnly()
{
    // 令牌调整动作日志：整段流程复用同一个 kLogEvent，避免离散调用链。
    kLogEvent actionEvent;
    bool buildOk = false;
    QString errorText;
    const ks::process::CreateProcessRequest request = buildCreateProcessRequestFromUi(&buildOk, &errorText);
    if (!buildOk)
    {
        appendCreateResultLine("参数解析失败: " + errorText);
        err << actionEvent
            << "[ProcessDock] 令牌调整参数解析失败, error="
            << errorText.toStdString()
            << eol;
        return;
    }
    if (!request.tokenModeEnabled)
    {
        appendCreateResultLine("当前不是 Token 模式，无法仅应用令牌调整。");
        warn << actionEvent
            << "[ProcessDock] 令牌调整被拒绝：当前非 Token 模式。"
            << eol;
        return;
    }

    std::string detailText;
    const bool adjustOk = ks::process::ApplyTokenPrivilegeEditsByPid(
        request.tokenSourcePid,
        request.tokenDesiredAccess,
        request.duplicatePrimaryToken,
        request.tokenPrivilegeEdits,
        &detailText);
    std::ostringstream desiredAccessStream;
    desiredAccessStream << "0x" << std::uppercase << std::hex << request.tokenDesiredAccess;

    appendCreateResultLine(QString("令牌调整结果: %1").arg(adjustOk ? "成功" : "失败"));
    appendCreateResultLine(QString::fromStdString(detailText.empty() ? "无附加信息" : detailText));
    (adjustOk ? info : err) << actionEvent
        << "[ProcessDock] 令牌调整完成, ok=" << (adjustOk ? "true" : "false")
        << ", sourcePid=" << request.tokenSourcePid
        << ", desiredAccess=" << desiredAccessStream.str()
        << ", duplicatePrimary=" << (request.duplicatePrimaryToken ? "true" : "false")
        << ", editCount=" << request.tokenPrivilegeEdits.size()
        << ", detail=" << (detailText.empty() ? "无附加信息" : detailText)
        << eol;
}

void ProcessDock::executeCreateProcessRequest()
{
    // 创建进程动作日志：整段流程复用同一个 kLogEvent，避免离散调用链。
    kLogEvent createProcessEvent;
    bool buildOk = false;
    QString errorText;
    const ks::process::CreateProcessRequest request = buildCreateProcessRequestFromUi(&buildOk, &errorText);
    if (!buildOk)
    {
        appendCreateResultLine("参数解析失败: " + errorText);
        err << createProcessEvent
            << "[ProcessDock] CreateProcess 参数解析失败, error="
            << errorText.toStdString()
            << eol;
        return;
    }

    ks::process::CreateProcessResult createResult{};
    const bool launchOk = ks::process::LaunchProcess(request, &createResult);
    appendCreateResultLine(QString("调用结果: %1").arg(launchOk ? "成功" : "失败"));
    appendCreateResultLine(QString("路径模式: %1").arg(createResult.usedTokenPath ? "Token" : "CreateProcessW"));
    appendCreateResultLine(QString("错误码: %1").arg(createResult.win32Error));
    appendCreateResultLine(QString::fromStdString(createResult.detailText));
    if (createResult.processInfoAvailable)
    {
        appendCreateResultLine(
            QString("输出 PI: pid=%1 tid=%2 hProcess=0x%3 hThread=0x%4")
            .arg(createResult.dwProcessId)
            .arg(createResult.dwThreadId)
            .arg(QString::number(createResult.hProcess, 16))
            .arg(QString::number(createResult.hThread, 16)));
    }

    (launchOk ? info : err) << createProcessEvent
        << "[ProcessDock] CreateProcess 请求完成, ok=" << (launchOk ? "true" : "false")
        << ", tokenMode=" << (request.tokenModeEnabled ? "true" : "false")
        << ", error=" << createResult.win32Error
        << ", detail=" << createResult.detailText
        << eol;

    if (launchOk)
    {
        requestAsyncRefresh(true);
    }
}

bool ProcessDock::isTreeModeEnabled() const
{
    return m_treeToggleButton != nullptr && m_treeToggleButton->isChecked();
}

ProcessDock::ViewMode ProcessDock::currentViewMode() const
{
    return static_cast<ViewMode>(m_viewModeCombo->currentIndex());
}

void ProcessDock::executeTaskKillAction(const bool forceKill)
{
    ks::process::ProcessRecord* processRecord = selectedRecord();
    if (processRecord == nullptr)
    {
        kLogEvent logEvent;
        warn << logEvent << "[ProcessDock] executeTaskKillAction 被忽略：当前没有选中进程。" << eol;
        return;
    }

    std::string detailText;
    const bool actionOk = ks::process::ExecuteTaskKill(processRecord->pid, forceKill, &detailText);
    // 单次动作统一复用 actionEvent，保证同一调用链日志 GUID 一致。
    kLogEvent actionEvent;
    (actionOk ? info : err) << actionEvent
        << "[ProcessDock] TaskKill action, pid=" << processRecord->pid
        << ", force=" << (forceKill ? "true" : "false")
        << ", ok=" << (actionOk ? "true" : "false")
        << ", detail=" << detailText
        << eol;
    showActionResultMessage(forceKill ? "Taskkill /f" : "Taskkill", actionOk, detailText, actionEvent);
    if (actionOk) requestAsyncRefresh(true);
}

void ProcessDock::executeTerminateProcessAction()
{
    ks::process::ProcessRecord* processRecord = selectedRecord();
    if (processRecord == nullptr)
    {
        kLogEvent logEvent;
        warn << logEvent << "[ProcessDock] executeTerminateProcessAction 被忽略：当前没有选中进程。" << eol;
        return;
    }

    std::string detailText;
    const bool actionOk = ks::process::TerminateProcessByWin32(processRecord->pid, &detailText);
    // 单次动作统一复用 actionEvent，保证同一调用链日志 GUID 一致。
    kLogEvent actionEvent;
    (actionOk ? info : err) << actionEvent
        << "[ProcessDock] TerminateProcess action, pid=" << processRecord->pid
        << ", ok=" << (actionOk ? "true" : "false")
        << ", detail=" << detailText
        << eol;
    showActionResultMessage("TerminateProcess", actionOk, detailText, actionEvent);
    if (actionOk) requestAsyncRefresh(true);
}

void ProcessDock::executeTerminateThreadsAction()
{
    ks::process::ProcessRecord* processRecord = selectedRecord();
    if (processRecord == nullptr)
    {
        kLogEvent logEvent;
        warn << logEvent << "[ProcessDock] executeTerminateThreadsAction 被忽略：当前没有选中进程。" << eol;
        return;
    }

    std::string detailText;
    const bool actionOk = ks::process::TerminateAllThreadsByPid(processRecord->pid, &detailText);
    // 单次动作统一复用 actionEvent，保证同一调用链日志 GUID 一致。
    kLogEvent actionEvent;
    (actionOk ? info : err) << actionEvent
        << "[ProcessDock] TerminateThreads action, pid=" << processRecord->pid
        << ", ok=" << (actionOk ? "true" : "false")
        << ", detail=" << detailText
        << eol;
    showActionResultMessage("TerminateThread(全部线程)", actionOk, detailText, actionEvent);
    if (actionOk) requestAsyncRefresh(true);
}

void ProcessDock::executeInjectInvalidShellcodeAction()
{
    ks::process::ProcessRecord* processRecord = selectedRecord();
    if (processRecord == nullptr)
    {
        kLogEvent logEvent;
        warn << logEvent << "[ProcessDock] executeInjectInvalidShellcodeAction 被忽略：当前没有选中进程。" << eol;
        return;
    }

    std::string detailText;
    const bool actionOk = ks::process::InjectInvalidShellcode(processRecord->pid, &detailText);
    // 单次动作统一复用 actionEvent，保证同一调用链日志 GUID 一致。
    kLogEvent actionEvent;
    (actionOk ? warn : err) << actionEvent
        << "[ProcessDock] InjectInvalidShellcode action, pid=" << processRecord->pid
        << ", ok=" << (actionOk ? "true" : "false")
        << ", detail=" << detailText
        << eol;
    showActionResultMessage("注入无效shellcode", actionOk, detailText, actionEvent);
    if (actionOk) requestAsyncRefresh(true);
}

void ProcessDock::executeSuspendAction()
{
    ks::process::ProcessRecord* processRecord = selectedRecord();
    if (processRecord == nullptr)
    {
        kLogEvent logEvent;
        warn << logEvent << "[ProcessDock] executeSuspendAction 被忽略：当前没有选中进程。" << eol;
        return;
    }

    std::string detailText;
    const bool actionOk = ks::process::SuspendProcess(processRecord->pid, &detailText);
    // 单次动作统一复用 actionEvent，保证同一调用链日志 GUID 一致。
    kLogEvent actionEvent;
    (actionOk ? info : err) << actionEvent
        << "[ProcessDock] SuspendProcess action, pid=" << processRecord->pid
        << ", ok=" << (actionOk ? "true" : "false")
        << ", detail=" << detailText
        << eol;
    showActionResultMessage("挂起进程", actionOk, detailText, actionEvent);
}

void ProcessDock::executeResumeAction()
{
    ks::process::ProcessRecord* processRecord = selectedRecord();
    if (processRecord == nullptr)
    {
        kLogEvent logEvent;
        warn << logEvent << "[ProcessDock] executeResumeAction 被忽略：当前没有选中进程。" << eol;
        return;
    }

    std::string detailText;
    const bool actionOk = ks::process::ResumeProcess(processRecord->pid, &detailText);
    // 单次动作统一复用 actionEvent，保证同一调用链日志 GUID 一致。
    kLogEvent actionEvent;
    (actionOk ? info : err) << actionEvent
        << "[ProcessDock] ResumeProcess action, pid=" << processRecord->pid
        << ", ok=" << (actionOk ? "true" : "false")
        << ", detail=" << detailText
        << eol;
    showActionResultMessage("恢复进程", actionOk, detailText, actionEvent);
}

void ProcessDock::executeSetCriticalAction(const bool enableCritical)
{
    ks::process::ProcessRecord* processRecord = selectedRecord();
    if (processRecord == nullptr)
    {
        kLogEvent logEvent;
        warn << logEvent << "[ProcessDock] executeSetCriticalAction 被忽略：当前没有选中进程。" << eol;
        return;
    }

    std::string detailText;
    const bool actionOk = ks::process::SetProcessCriticalFlag(processRecord->pid, enableCritical, &detailText);
    // 单次动作统一复用 actionEvent，保证同一调用链日志 GUID 一致。
    kLogEvent actionEvent;
    (actionOk ? info : err) << actionEvent
        << "[ProcessDock] SetCritical action, pid=" << processRecord->pid
        << ", enable=" << (enableCritical ? "true" : "false")
        << ", ok=" << (actionOk ? "true" : "false")
        << ", detail=" << detailText
        << eol;
    showActionResultMessage(enableCritical ? "设为关键进程" : "取消关键进程", actionOk, detailText, actionEvent);
}

void ProcessDock::executeSetPriorityAction(const int priorityActionId)
{
    ks::process::ProcessRecord* processRecord = selectedRecord();
    if (processRecord == nullptr)
    {
        kLogEvent logEvent;
        warn << logEvent << "[ProcessDock] executeSetPriorityAction 被忽略：当前没有选中进程。" << eol;
        return;
    }

    ks::process::ProcessPriorityLevel priorityLevel = ks::process::ProcessPriorityLevel::Normal;
    switch (priorityActionId)
    {
    case 0: priorityLevel = ks::process::ProcessPriorityLevel::Idle; break;
    case 1: priorityLevel = ks::process::ProcessPriorityLevel::BelowNormal; break;
    case 2: priorityLevel = ks::process::ProcessPriorityLevel::Normal; break;
    case 3: priorityLevel = ks::process::ProcessPriorityLevel::AboveNormal; break;
    case 4: priorityLevel = ks::process::ProcessPriorityLevel::High; break;
    case 5: priorityLevel = ks::process::ProcessPriorityLevel::Realtime; break;
    default: break;
    }

    std::string detailText;
    const bool actionOk = ks::process::SetProcessPriority(processRecord->pid, priorityLevel, &detailText);
    // 单次动作统一复用 actionEvent，保证同一调用链日志 GUID 一致。
    kLogEvent actionEvent;
    (actionOk ? info : err) << actionEvent
        << "[ProcessDock] SetPriority action, pid=" << processRecord->pid
        << ", actionId=" << priorityActionId
        << ", ok=" << (actionOk ? "true" : "false")
        << ", detail=" << detailText
        << eol;
    showActionResultMessage("设置进程优先级", actionOk, detailText, actionEvent);
}

void ProcessDock::executeOpenFolderAction()
{
    ks::process::ProcessRecord* processRecord = selectedRecord();
    if (processRecord == nullptr)
    {
        kLogEvent logEvent;
        warn << logEvent << "[ProcessDock] executeOpenFolderAction 被忽略：当前没有选中进程。" << eol;
        return;
    }

    std::string detailText;
    const bool actionOk = ks::process::OpenProcessFolder(processRecord->pid, &detailText);
    // 单次动作统一复用 actionEvent，保证同一调用链日志 GUID 一致。
    kLogEvent actionEvent;
    (actionOk ? info : err) << actionEvent
        << "[ProcessDock] OpenFolder action, pid=" << processRecord->pid
        << ", ok=" << (actionOk ? "true" : "false")
        << ", detail=" << detailText
        << eol;
    showActionResultMessage("打开所在目录", actionOk, detailText, actionEvent);
}

void ProcessDock::openProcessDetailsPlaceholder()
{
    ks::process::ProcessRecord* processRecord = selectedRecord();
    if (processRecord == nullptr)
    {
        kLogEvent logEvent;
        warn << logEvent << "[ProcessDock] 打开进程详细信息失败：当前没有选中进程。" << eol;
        QMessageBox::warning(this, "进程详细信息", "请先在表格中选中一个进程。");
        return;
    }

    // 详情窗口展示前，主动补齐静态字段，避免“路径/用户/命令行为空”。
    ks::process::ProcessRecord detailRecord = *processRecord;
    const bool needStaticQuery =
        detailRecord.imagePath.empty() ||
        detailRecord.commandLine.empty() ||
        detailRecord.userName.empty() ||
        detailRecord.signatureState.empty() ||
        detailRecord.signatureState == "Pending";
    if (needStaticQuery)
    {
        ks::process::ProcessRecord queriedRecord{};
        if (ks::process::QueryProcessStaticDetailByPid(detailRecord.pid, queriedRecord))
        {
            // 优先使用查询结果中的“非空字段”，其余保留原缓存。
            if (!queriedRecord.processName.empty()) detailRecord.processName = queriedRecord.processName;
            if (!queriedRecord.imagePath.empty()) detailRecord.imagePath = queriedRecord.imagePath;
            if (!queriedRecord.commandLine.empty()) detailRecord.commandLine = queriedRecord.commandLine;
            if (!queriedRecord.userName.empty()) detailRecord.userName = queriedRecord.userName;
            if (!queriedRecord.startTimeText.empty()) detailRecord.startTimeText = queriedRecord.startTimeText;
            if (!queriedRecord.architectureText.empty()) detailRecord.architectureText = queriedRecord.architectureText;
            if (!queriedRecord.priorityText.empty()) detailRecord.priorityText = queriedRecord.priorityText;
            if (!queriedRecord.signatureState.empty()) detailRecord.signatureState = queriedRecord.signatureState;
            if (!queriedRecord.signaturePublisher.empty()) detailRecord.signaturePublisher = queriedRecord.signaturePublisher;
            detailRecord.signatureTrusted = queriedRecord.signatureTrusted;
            detailRecord.isAdmin = queriedRecord.isAdmin;
            if (queriedRecord.creationTime100ns != 0) detailRecord.creationTime100ns = queriedRecord.creationTime100ns;
            if (queriedRecord.parentPid != 0) detailRecord.parentPid = queriedRecord.parentPid;
            if (queriedRecord.sessionId != 0) detailRecord.sessionId = queriedRecord.sessionId;
            if (queriedRecord.threadCount != 0) detailRecord.threadCount = queriedRecord.threadCount;
            if (queriedRecord.handleCount != 0) detailRecord.handleCount = queriedRecord.handleCount;
            detailRecord.staticDetailsReady = queriedRecord.staticDetailsReady;
        }
    }

    // identityKey 用于“一进程一窗口”复用逻辑。
    const std::string identityKey = ks::process::BuildProcessIdentityKey(
        detailRecord.pid,
        detailRecord.creationTime100ns);

    auto existingWindowIt = m_detailWindowByIdentity.find(identityKey);
    if (existingWindowIt != m_detailWindowByIdentity.end() && existingWindowIt->second != nullptr)
    {
        existingWindowIt->second->updateBaseRecord(detailRecord);
        m_detailWindowLastSyncTimeByIdentity[identityKey] = std::chrono::steady_clock::now();
        existingWindowIt->second->show();
        existingWindowIt->second->raise();
        existingWindowIt->second->activateWindow();

        kLogEvent logEvent;
        info << logEvent
            << "[ProcessDock] 复用已存在进程详情窗口, pid=" << detailRecord.pid
            << ", identity=" << identityKey
            << eol;
        return;
    }

    // 创建新的独立窗口（不属于 Docking System，可并行打开多个）。
    ProcessDetailWindow* detailWindow = new ProcessDetailWindow(detailRecord, nullptr);
    detailWindow->setAttribute(Qt::WA_DeleteOnClose, true);
    m_detailWindowByIdentity[identityKey] = detailWindow;
    m_detailWindowLastSyncTimeByIdentity[identityKey] = std::chrono::steady_clock::now();

    // 详情窗口销毁后，从缓存移除，防止悬空指针。
    connect(detailWindow, &QObject::destroyed, this, [this, identityKey]() {
        m_detailWindowByIdentity.erase(identityKey);
        m_detailWindowLastSyncTimeByIdentity.erase(identityKey);
    });

    // “转到父进程”由详情窗口发信号到这里统一处理。
    connect(detailWindow, &ProcessDetailWindow::requestOpenProcessByPid, this, [this](const std::uint32_t parentPid) {
        openProcessDetailWindowByPid(parentPid);
    });

    detailWindow->show();
    detailWindow->raise();
    detailWindow->activateWindow();

    kLogEvent logEvent;
    info << logEvent
        << "[ProcessDock] 创建新的进程详情窗口, pid=" << detailRecord.pid
        << ", identity=" << identityKey
        << eol;
}

void ProcessDock::openProcessDetailWindowByPid(const std::uint32_t pid)
{
    // 优先从当前缓存中查找对应 PID（可避免额外系统调用）。
    for (const auto& cachePair : m_cacheByIdentity)
    {
        const ks::process::ProcessRecord& cacheRecord = cachePair.second.record;
        if (cacheRecord.pid != pid)
        {
            continue;
        }

        auto existingWindowIt = m_detailWindowByIdentity.find(cachePair.first);
        if (existingWindowIt != m_detailWindowByIdentity.end() && existingWindowIt->second != nullptr)
        {
            existingWindowIt->second->updateBaseRecord(cacheRecord);
            m_detailWindowLastSyncTimeByIdentity[cachePair.first] = std::chrono::steady_clock::now();
            existingWindowIt->second->show();
            existingWindowIt->second->raise();
            existingWindowIt->second->activateWindow();
            return;
        }

        // 与当前选中逻辑一致：若缓存静态字段不完整，则开窗前补齐一次。
        ks::process::ProcessRecord detailRecord = cacheRecord;
        if (detailRecord.imagePath.empty() ||
            detailRecord.commandLine.empty() ||
            detailRecord.userName.empty() ||
            detailRecord.signatureState.empty() ||
            detailRecord.signatureState == "Pending")
        {
            ks::process::ProcessRecord queriedRecord{};
            if (ks::process::QueryProcessStaticDetailByPid(detailRecord.pid, queriedRecord))
            {
                if (!queriedRecord.processName.empty()) detailRecord.processName = queriedRecord.processName;
                if (!queriedRecord.imagePath.empty()) detailRecord.imagePath = queriedRecord.imagePath;
                if (!queriedRecord.commandLine.empty()) detailRecord.commandLine = queriedRecord.commandLine;
                if (!queriedRecord.userName.empty()) detailRecord.userName = queriedRecord.userName;
                if (!queriedRecord.startTimeText.empty()) detailRecord.startTimeText = queriedRecord.startTimeText;
                if (!queriedRecord.architectureText.empty()) detailRecord.architectureText = queriedRecord.architectureText;
                if (!queriedRecord.priorityText.empty()) detailRecord.priorityText = queriedRecord.priorityText;
                if (!queriedRecord.signatureState.empty()) detailRecord.signatureState = queriedRecord.signatureState;
                if (!queriedRecord.signaturePublisher.empty()) detailRecord.signaturePublisher = queriedRecord.signaturePublisher;
                detailRecord.signatureTrusted = queriedRecord.signatureTrusted;
                detailRecord.isAdmin = queriedRecord.isAdmin;
                if (queriedRecord.creationTime100ns != 0) detailRecord.creationTime100ns = queriedRecord.creationTime100ns;
                if (queriedRecord.parentPid != 0) detailRecord.parentPid = queriedRecord.parentPid;
                if (queriedRecord.sessionId != 0) detailRecord.sessionId = queriedRecord.sessionId;
                if (queriedRecord.threadCount != 0) detailRecord.threadCount = queriedRecord.threadCount;
                if (queriedRecord.handleCount != 0) detailRecord.handleCount = queriedRecord.handleCount;
                detailRecord.staticDetailsReady = queriedRecord.staticDetailsReady;
            }
        }

        ProcessDetailWindow* detailWindow = new ProcessDetailWindow(detailRecord, nullptr);
        detailWindow->setAttribute(Qt::WA_DeleteOnClose, true);
        m_detailWindowByIdentity[cachePair.first] = detailWindow;
        m_detailWindowLastSyncTimeByIdentity[cachePair.first] = std::chrono::steady_clock::now();
        connect(detailWindow, &QObject::destroyed, this, [this, identityKey = cachePair.first]() {
            m_detailWindowByIdentity.erase(identityKey);
            m_detailWindowLastSyncTimeByIdentity.erase(identityKey);
        });
        connect(detailWindow, &ProcessDetailWindow::requestOpenProcessByPid, this, [this](const std::uint32_t parentPid) {
            openProcessDetailWindowByPid(parentPid);
        });
        detailWindow->show();
        detailWindow->raise();
        detailWindow->activateWindow();
        return;
    }

    // 缓存不存在时，尝试实时查询该 PID 的最小详情并打开窗口。
    ks::process::ProcessRecord queriedRecord{};
    if (!ks::process::QueryProcessStaticDetailByPid(pid, queriedRecord))
    {
        queriedRecord.pid = pid;
        queriedRecord.processName = ks::process::GetProcessNameByPID(pid);
    }
    if (queriedRecord.processName.empty())
    {
        queriedRecord.processName = "PID_" + std::to_string(pid);
    }

    const std::string identityKey = ks::process::BuildProcessIdentityKey(
        queriedRecord.pid,
        queriedRecord.creationTime100ns);

    ProcessDetailWindow* detailWindow = new ProcessDetailWindow(queriedRecord, nullptr);
    detailWindow->setAttribute(Qt::WA_DeleteOnClose, true);
    m_detailWindowByIdentity[identityKey] = detailWindow;
    m_detailWindowLastSyncTimeByIdentity[identityKey] = std::chrono::steady_clock::now();
    connect(detailWindow, &QObject::destroyed, this, [this, identityKey]() {
        m_detailWindowByIdentity.erase(identityKey);
        m_detailWindowLastSyncTimeByIdentity.erase(identityKey);
    });
    connect(detailWindow, &ProcessDetailWindow::requestOpenProcessByPid, this, [this](const std::uint32_t parentPid) {
        openProcessDetailWindowByPid(parentPid);
    });
    detailWindow->show();
    detailWindow->raise();
    detailWindow->activateWindow();
}
