#pragma once

// ============================================================
// ProcessDetailWindow.h
// 作用：
// - 提供“进程详细信息”独立窗口（非 Dock、非阻塞）；
// - 每个进程可打开独立窗口，包含 详细信息 / 线程 / 操作 / 模块 / 令牌 / 令牌开关 / PEB 七个 Tab；
// - 支持模块刷新、右键操作、DLL 注入、Shellcode 注入等能力。
// ============================================================

#include "../Framework.h"

#include <QHash>
#include <QIcon>
#include <QPointer>
#include <QStringList>
#include <QWidget>

#include <cstdint>
#include <string>
#include <vector>

#include "../../../shared/driver/KswordArkThreadIoctl.h"

// 前置声明：减少头文件依赖，提升编译速度。
class QCheckBox;
class QComboBox;
class QEvent;
class QFormLayout;
class QHBoxLayout;
class QLabel;
class QLineEdit;
class QPlainTextEdit;
class QPushButton;
class QTabWidget;
class QTableWidget;
class QTreeWidget;
class QTreeWidgetItem;
class QVBoxLayout;
class QPoint;
class CodeEditorWidget;

class ProcessDetailWindow final : public QWidget
{
    Q_OBJECT

public:
    // 构造函数作用：
    // - 接收进程基础快照作为窗口初始数据；
    // - 初始化所有 UI 与交互连接；
    // - 启动模块页首次异步刷新。
    explicit ProcessDetailWindow(const ks::process::ProcessRecord& baseRecord, QWidget* parent = nullptr);

    // updateBaseRecord 作用：
    // - 用外部最新进程快照更新窗口展示；
    // - 不会销毁窗口，只刷新显示文本与状态。
    void updateBaseRecord(const ks::process::ProcessRecord& baseRecord);

    // pid 作用：返回当前窗口绑定进程 PID。
    std::uint32_t pid() const;

    // showHotkeyTabAndRefresh 作用：
    // - 将详情窗口切换到“进程热键”页；
    // - 复用详情页已有热键扫描流程，触发一次异步刷新；
    // - 调用方通常来自进程列表右键菜单，用于把隐藏较深的热键功能变成一键入口。
    // 参数：无。
    // 返回：无。
    void showHotkeyTabAndRefresh();

signals:
    // requestOpenProcessByPid 作用：
    // - 在“转到父进程”按钮点击时发出；
    // - 由 ProcessDock 统一接收并打开对应进程详情窗口。
    void requestOpenProcessByPid(std::uint32_t pid);

    // requestOpenHandleDockByPid 作用：
    // - 在“跳转句柄”按钮点击时发出；
    // - 由 ProcessDock 转发给 MainWindow 打开句柄 Dock 并按 PID 过滤。
    void requestOpenHandleDockByPid(std::uint32_t pid);

private:
    // ModuleRefreshResult：模块页后台刷新结果数据结构。
    struct ModuleRefreshResult
    {
        ks::process::ProcessModuleSnapshot moduleSnapshot; // 模块 + 线程快照。
        std::uint64_t elapsedMs = 0;                       // 后台刷新耗时（毫秒）。
        bool includeSignatureCheck = false;                // 本轮是否执行签名校验。
    };

    // ThreadInspectItem：线程细节页单行数据。
    struct ThreadInspectItem
    {
        std::uint32_t threadId = 0;        // 线程 ID。
        std::uint32_t processId = 0;       // 所属进程 PID。
        QString stateText;                 // 状态文本（运行/结束/未知）。
        int priorityValue = 0;             // 线程优先级值。
        quint64 switchCount = 0;           // 上下文切换计数（当前实现可能为 0）。
        QString startAddressText;          // 线程起始地址（十六进制）。
        QString tebAddressText;            // 线程 TEB 地址（十六进制）。
        QString affinityText;              // 线程亲和性文本。
        QString registerSummaryText;       // 寄存器摘要文本。
        std::uint64_t startAddress = 0;    // 起始地址原始值。
        std::uint64_t win32StartAddress = 0; // Win32StartAddress 原始值。
        std::uint64_t tebAddress = 0;      // TEB 地址原始值。
        std::uint64_t userStackBase = 0;   // 用户栈基址。
        std::uint64_t userStackLimit = 0;  // 用户栈边界。
        std::uint64_t r0KernelStack = 0;   // KTHREAD.KernelStack。
        std::uint64_t r0StackBase = 0;     // KTHREAD.StackBase。
        std::uint64_t r0StackLimit = 0;    // KTHREAD.StackLimit。
        std::uint64_t r0InitialStack = 0;  // KTHREAD.InitialStack。
        std::uint32_t r0ThreadStatus = KSWORD_ARK_THREAD_R0_STATUS_UNAVAILABLE; // R0 线程状态。
        std::uint64_t r0CapabilityMask = 0; // R0 capability。
    };

    // ThreadInspectRefreshResult：线程细节异步刷新结果。
    struct ThreadInspectRefreshResult
    {
        std::vector<ThreadInspectItem> rows; // 线程行数据。
        QString diagnosticText;              // 诊断文本。
        std::uint64_t elapsedMs = 0;         // 刷新耗时（毫秒）。
    };

    // TextRefreshResult：令牌页/PEB 页文本刷新结果。
    struct TextRefreshResult
    {
        QString detailText;               // 展示文本内容。
        QString diagnosticText;           // 诊断文本。
        std::uint64_t elapsedMs = 0;      // 刷新耗时（毫秒）。
    };

    // SectionRefreshResult：进程 SectionObject / ControlArea 异步查询结果。
    struct SectionRefreshResult
    {
        QString detailText;              // 展示文本内容。
        QString diagnosticText;          // 诊断文本。
        std::uint64_t elapsedMs = 0;     // 刷新耗时（毫秒）。
    };

    // HotkeyInspectItem：进程热键页单行数据。
    struct HotkeyInspectItem
    {
        QString objectText;              // HWND/HMENU/资源/快捷方式路径。
        std::uint32_t hotkeyId = 0;      // 菜单命令 ID / Accelerator 命令 ID / 0。
        std::uint32_t modifiers = 0;     // MOD_ALT/MOD_CONTROL/MOD_SHIFT/MOD_WIN 位图。
        std::uint32_t virtualKey = 0;    // 虚拟键码，未知时为 0。
        QString hotkeyText;              // 可读快捷键文本。
        std::uint32_t processId = 0;     // 所属进程 ID。
        std::uint32_t threadId = 0;      // 所属窗口线程 ID，非窗口来源为 0。
        QString processName;             // 所属进程名。
        QString sourceText;              // 来源：窗口热键/菜单/Accelerator/.lnk 等。
        QString detailText;              // 额外上下文。
    };

    // HotkeyInspectRefreshResult：进程热键异步刷新结果。
    struct HotkeyInspectRefreshResult
    {
        std::vector<HotkeyInspectItem> rows; // 热键行数据。
        QString diagnosticText;              // 诊断文本。
        std::uint64_t elapsedMs = 0;         // 刷新耗时（毫秒）。
    };

    // KeyboardHookInspectItem：键盘页钩子表单行数据。
    struct KeyboardHookInspectItem
    {
        QString objectText;       // tagHOOK 对象地址。
        QString typeText;         // WH_KEYBOARD / WH_KEYBOARD_LL。
        QString scopeText;        // 线程链 / 全局链。
        std::uint32_t processId = 0;
        std::uint32_t threadId = 0;
        QString procedureText;    // 回调地址或 R0 保存的回调偏移。
        QString moduleText;       // 模块基址或 module id。
        QString sourceText;       // 来源链。
        QString flagsText;        // tagHOOK flags。
        QString detailText;       // 链头、next、threadInfo 等诊断字段。
    };

    // KeyboardInspectRefreshResult：键盘页异步刷新结果。
    struct KeyboardInspectRefreshResult
    {
        std::vector<HotkeyInspectItem> hotkeyRows;     // 热键行数据。
        std::vector<KeyboardHookInspectItem> hookRows; // 钩子行数据。
        QString diagnosticText;                        // 诊断文本。
        std::uint64_t elapsedMs = 0;                   // 刷新耗时（毫秒）。
    };

    // StaticDetailRefreshResult：进程静态详情后台补齐结果。
    struct StaticDetailRefreshResult
    {
        ks::process::ProcessRecord processRecord; // 后台读取到的最新进程记录。
        QString diagnosticText;                   // 失败或降级原因。
        std::uint64_t elapsedMs = 0;              // 后台查询耗时（毫秒）。
        bool queryOk = false;                     // true 表示基础静态详情读取成功。
    };

private:
    // ======== UI 初始化 ========
    // changeEvent 作用：
    // - 监听调色板/样式变化；
    // - 在深浅色切换后重建内部样式，避免进程详情页残留白底。
    // 调用方式：Qt 自动触发。
    // 参数 event：变更事件对象。
    // 返回：无。
    void changeEvent(QEvent* event) override;

    void initializeUi();
    // applyThemeStyle 作用：
    // - 给详情窗口内部控件统一应用深浅色样式；
    // - 显式强制 Window/Base 颜色，规避 Win11 自动背景接管问题。
    // 调用方式：initializeUi 完成控件创建后调用；changeEvent 中再次调用。
    // 参数：无。
    // 返回：无。
    void applyThemeStyle();
    void initializeDetailTab();
    void initializeThreadTab();
    void initializeActionTab();
    void initializeModuleTab();
    void initializeTokenTab();
    // initializeKernelObjectTab 作用：
    // - 构建 Phase-2 “内核对象”页面；
    // - 展示 R0 扩展字段、DynData capability、字段来源和可用性；
    // - 仅显示 ObjectTable/SectionObject 可用状态，不在本页直接枚举句柄表或 Section。
    // 调用方式：initializeUi 中创建 m_kernelObjectTab 后调用。
    // 参数：无。
    // 返回：无。
    void initializeKernelObjectTab();
    // initializeHotkeyTab 作用：
    // - 构建“进程热键”页面；
    // - 覆盖窗口激活热键、菜单快捷键、PE Accelerator 资源和 .lnk 快捷方式热键。
    // 调用方式：initializeUi 中创建 m_hotkeyTab 后调用。
    // 参数：无。
    // 返回：无。
    void initializeHotkeyTab();
    // initializeKeyboardTab 作用：
    // - 构建“键盘”页面；
    // - 同时展示热键检测结果和 R0 键盘钩子链枚举结果。
    // 调用方式：initializeUi 中创建 m_keyboardTab 后调用。
    // 参数：无。
    // 返回：无。
    void initializeKeyboardTab();
    // initializeTokenSwitchTab 作用：
    // - 构建“令牌开关”页面；
    // - 提供复选框批量控制 Token 开关位，并提供刷新/应用按钮。
    // 调用方式：initializeUi 中创建 m_tokenSwitchTab 后调用。
    // 参数：无。
    // 返回：无。
    void initializeTokenSwitchTab();
    void initializePebTab();
    void initializeConnections();

    // ======== 详情页刷新 ========
    void refreshDetailTabTexts();
    // requestAsyncStaticDetailRefresh 作用：
    // - 在后台补齐路径、命令行、用户、签名等慢字段；
    // - 避免在窗口构造或周期同步时阻塞 UI 线程。
    // 调用方式：构造完成后、外部快照字段不完整时调用。
    // 参数 includeSignatureCheck：true 表示后台允许执行 WinVerifyTrust 签名校验。
    // 返回：无。
    void requestAsyncStaticDetailRefresh(bool includeSignatureCheck);
    // applyStaticDetailRefreshResult 作用：
    // - 在主线程合并后台静态详情结果；
    // - 只使用非空/有效字段覆盖当前缓存。
    // 调用方式：requestAsyncStaticDetailRefresh 的后台任务完成后投递。
    // 参数 refreshResult：后台查询结果。
    // 返回：无。
    void applyStaticDetailRefreshResult(const StaticDetailRefreshResult& refreshResult);
    // requestInitialRefreshForCurrentTab 作用：
    // - 按当前 Tab 懒启动首次重型刷新；
    // - 打开窗口时只展示“详细信息”页，不立即扫描模块/PEB/令牌。
    // 调用方式：currentChanged 信号和构造完成后调用。
    // 参数：无。
    // 返回：无。
    void requestInitialRefreshForCurrentTab();
    // refreshKernelObjectTabTexts 作用：
    // - 根据 m_baseRecord 刷新“内核对象”页所有标签；
    // - DynData 未命中时显示 Unavailable，避免误导用户可直接进入后续句柄/Section 枚举。
    // 调用方式：refreshDetailTabTexts 和 updateBaseRecord 间接调用。
    // 参数：无。
    // 返回：无。
    void refreshKernelObjectTabTexts();
    void refreshParentProcessSection();
    void updateWindowTitle();
    void requestAsyncThreadInspectRefresh();
    void applyThreadInspectResult(const ThreadInspectRefreshResult& refreshResult);
    void updateThreadInspectStatusLabel(const QString& statusText, bool refreshing);
    // openSelectedThreadStackWindow 作用：
    // - 根据线程页当前选中行打开 Phase-8 调用栈窗口；
    // - 使用最近一次线程刷新缓存中的 TID/PID/栈边界构造目标。
    // 调用方式：线程页按钮或表格双击。
    // 参数：无。
    // 返回：无。
    void openSelectedThreadStackWindow();

    // ======== 令牌页/PEB页刷新 ========
    void requestAsyncTokenRefresh();
    void requestAsyncPebRefresh();
    void applyTokenRefreshResult(const TextRefreshResult& refreshResult);
    void applyPebRefreshResult(const TextRefreshResult& refreshResult);
    // requestAsyncHotkeyRefresh 作用：
    // - 后台扫描当前进程相关热键来源；
    // - 不直接访问驱动，不阻塞详情窗口 UI 线程。
    // 调用方式：热键页首刷或点击刷新按钮。
    // 参数：无。
    // 返回：无。
    void requestAsyncHotkeyRefresh();
    // applyHotkeyRefreshResult 作用：
    // - 在主线程回填热键检测结果；
    // - 重建表格并更新状态标签。
    // 调用方式：requestAsyncHotkeyRefresh 后台任务完成后投递。
    // 参数 refreshResult：后台扫描结果。
    // 返回：无。
    void applyHotkeyRefreshResult(const HotkeyInspectRefreshResult& refreshResult);
    void rebuildHotkeyTable();
    void updateHotkeyStatusLabel(const QString& statusText, bool refreshing);
    void requestAsyncKeyboardRefresh();
    void applyKeyboardRefreshResult(const KeyboardInspectRefreshResult& refreshResult);
    void rebuildKeyboardHotkeyTable();
    void rebuildKeyboardHookTable();
    void updateKeyboardStatusLabel(const QString& statusText, bool refreshing);
    // requestAsyncSectionRefresh 作用：
    // - 通过 ArkDriverClient 异步查询 R0 SectionObject / ControlArea；
    // - 只传 PID，不把 UI 看到的内核地址传回驱动。
    // 调用方式：内核对象页刷新按钮或首次初始化后调用。
    // 参数：无。
    // 返回：无。
    void requestAsyncSectionRefresh();
    // applySectionRefreshResult 作用：
    // - 在主线程回填 Section/ControlArea 详情文本；
    // - 同步刷新状态标签。
    // 调用方式：后台任务完成后 invokeMethod 调用。
    // 参数 refreshResult：后台查询结果。
    // 返回：无。
    void applySectionRefreshResult(const SectionRefreshResult& refreshResult);
    // refreshTokenSwitchStates 作用：
    // - 读取当前目标进程令牌开关状态；
    // - 把读取结果同步到“令牌开关”页的复选框。
    // 调用方式：点击刷新按钮或窗口首次初始化后调用。
    // 参数：无。
    // 返回：无。
    void refreshTokenSwitchStates();
    // applyTokenSwitchStates 作用：
    // - 把“令牌开关”页复选框状态写回目标进程令牌；
    // - 底层通过 NtSetInformationToken（NtSetTokenInformation）逐项应用。
    // 调用方式：点击应用按钮后调用。
    // 参数：无。
    // 返回：无。
    void applyTokenSwitchStates();
    // applyRawTokenInformation 作用：
    // - 从“原始设置”区域读取 TokenInformationClass 与原始负载；
    // - 直接调用 NtSetInformationToken 提交，覆盖所有可尝试的信息类。
    // 调用方式：点击“应用原始设置”按钮后调用。
    // 参数：无。
    // 返回：无。
    void applyRawTokenInformation();

    // ======== 模块页刷新 ========
    void requestAsyncModuleRefresh(bool forceRefresh);
    void applyModuleRefreshResult(const ModuleRefreshResult& refreshResult);
    void rebuildModuleTable();
    void updateModuleStatusLabel(const QString& statusText, bool refreshing);

    // ======== 模块表右键 ========
    void showModuleContextMenu(const QPoint& localPosition);
    void copyCurrentModuleCell();
    void copyCurrentModuleRow();
    void openCurrentModuleFolder();
    void unloadCurrentModule();
    void suspendCurrentModuleThread();
    void resumeCurrentModuleThread();
    void terminateCurrentModuleThread();

    // ======== 操作页动作 ========
    void executeTerminateProcessAction();
    // executeTerminateProcessComboAction 作用：
    // - 执行与进程列表右键“结束进程”一致的多方法组合结束动作；
    // - 输入为当前详情页绑定 PID，处理过程按固定方法链逐项尝试；
    // - 返回值：无，动作结果统一写日志。
    void executeTerminateProcessComboAction();
    void executeTerminateThreadsAction();
    void executeSelectedTerminateAction();
    void executeSuspendProcessAction();
    void executeResumeProcessAction();
    void executeSetCriticalAction(bool enableCritical);
    void executeSetPriorityAction();
    // executeSetPriorityActionById 作用：
    // - 根据菜单/按钮传入的优先级 ID 设置目标进程优先级；
    // - 输入 priorityActionId 对应 Idle/BelowNormal/Normal/AboveNormal/High/Realtime；
    // - 返回值：无，底层调用结果写日志。
    void executeSetPriorityActionById(int priorityActionId);
    // executeSetEfficiencyModeAction 作用：
    // - 开启或关闭目标进程 Windows 效率模式；
    // - 输入 enableEfficiencyMode 为 true 时开启，false 时关闭；
    // - 返回值：无，底层调用结果写日志。
    void executeSetEfficiencyModeAction(bool enableEfficiencyMode);
    // executeOpenProcessFolderAction 作用：
    // - 在资源管理器中定位当前进程映像文件所在位置；
    // - 输入来自 m_baseRecord.pid，不需要额外参数；
    // - 返回值：无，底层调用结果写日志。
    void executeOpenProcessFolderAction();
    // executeRefreshPplProtectionLevelAction 作用：
    // - 通过 R3 ProcessProtectionLevelInfo 刷新当前进程 PPL 枚举；
    // - 输入来自 m_baseRecord.pid，刷新后更新详情页缓存与文本；
    // - 返回值：无，查询结果写日志。
    void executeRefreshPplProtectionLevelAction();
    // executeR0TerminateProcessAction 作用：
    // - 通过 ArkDriverClient 请求 R0 结束当前进程；
    // - 输入来自 m_baseRecord.pid，不直接 DeviceIoControl；
    // - 返回值：无，驱动调用摘要写日志。
    void executeR0TerminateProcessAction();
    // executeR0SuspendProcessAction 作用：
    // - 通过 ArkDriverClient 请求 R0 挂起当前进程；
    // - 输入来自 m_baseRecord.pid，不直接 DeviceIoControl；
    // - 返回值：无，驱动调用摘要写日志。
    void executeR0SuspendProcessAction();
    // executeR0SetPplProtectionAction 作用：
    // - 通过 ArkDriverClient 设置目标进程 PS_PROTECTION 原始字节；
    // - 输入 protectionLevel 为 Signer<<4 | Type，levelDisplayText 用于日志展示；
    // - 返回值：无，驱动调用摘要写日志。
    void executeR0SetPplProtectionAction(std::uint8_t protectionLevel, const QString& levelDisplayText);
    // executeR0SetProcessHiddenAction 作用：
    // - 通过 ArkDriverClient 执行可恢复隐藏/取消隐藏；
    // - 输入 hidden=true 表示隐藏，visibilityFlags 指定改 PID/断链策略；
    // - 返回值：无，驱动调用摘要写日志。
    void executeR0SetProcessHiddenAction(bool hidden, unsigned long visibilityFlags = 0UL);
    // executeR0ClearProcessHiddenAction 作用：
    // - 通过 ArkDriverClient 清空驱动内全部可恢复隐藏标记；
    // - 输入无，影响驱动记录的全部目标；
    // - 返回值：无，驱动调用摘要写日志。
    void executeR0ClearProcessHiddenAction();
    // executeR0SetBreakOnTerminationAction 作用：
    // - 通过 ArkDriverClient 设置或清除 BreakOnTermination；
    // - 输入 enabled=true 表示启用，false 表示关闭；
    // - 返回值：无，驱动调用摘要写日志。
    void executeR0SetBreakOnTerminationAction(bool enabled);
    // executeR0DisableApcInsertionAction 作用：
    // - 通过 ArkDriverClient 清除当前进程现有线程 ApcQueueable 位；
    // - 输入来自 m_baseRecord.pid；
    // - 返回值：无，驱动调用摘要写日志。
    void executeR0DisableApcInsertionAction();
    // executeR0DkomRemoveFromCidTableAction 作用：
    // - 通过 ArkDriverClient 从 PspCidTable 删除当前进程 CID 表项；
    // - 输入来自 m_baseRecord.pid；
    // - 返回值：无，驱动调用摘要写日志。
    void executeR0DkomRemoveFromCidTableAction();
    void executeInjectDllAction();
    void executeInjectShellcodeAction();

    // ======== 工具函数 ========
    QIcon resolveProcessIcon(const std::string& processPath, int iconPixelSize);
    QString formatModuleSizeText(std::uint32_t moduleSizeBytes) const;
    QString formatHexText(std::uint64_t value) const;
    // readBinaryFile 作用：读取二进制文件到缓冲区，并沿用调用方传入的同一 kLogEvent 输出过程日志。
    // 调用方式：由注入动作函数传入 actionEvent，以保证“动作+文件读取”日志链路一致。
    // 参数 filePath：文件路径；bufferOut：输出缓冲区；errorTextOut：错误信息；actionEvent：同链路日志事件对象。
    // 返回值：读取成功返回 true，失败返回 false。
    bool readBinaryFile(
        const QString& filePath,
        std::vector<std::uint8_t>& bufferOut,
        std::string& errorTextOut,
        const kLogEvent& actionEvent) const;
    // showActionResultMessage 作用：统一记录动作结果（不弹框），并复用外层传入的同一 kLogEvent 维持调用链。
    // 调用方式：动作函数先创建 kLogEvent，再将该事件对象传入本函数。
    // 参数 title：动作标题；actionOk：动作是否成功；detailText：动作详情；actionEvent：同链路日志事件对象。
    // 返回值：无。
    void showActionResultMessage(const QString& title, bool actionOk, const std::string& detailText, const kLogEvent& actionEvent);
    ks::process::ProcessModuleRecord* selectedModuleRecord();

private:
    // ======== 当前绑定进程基础数据 ========
    ks::process::ProcessRecord m_baseRecord;   // 当前窗口绑定进程快照。
    std::string m_identityKey;                 // PID+CreateTime 组成的 identity 字符串。

    // ======== 根布局与 Tabs ========
    QVBoxLayout* m_rootLayout = nullptr;       // 窗口根布局。
    QTabWidget* m_tabWidget = nullptr;         // 详情窗口全部 Tab 的容器。
    QWidget* m_detailTab = nullptr;            // “详细信息”页。
    QWidget* m_threadTab = nullptr;            // “线程”页。
    QWidget* m_actionTab = nullptr;            // “操作”页。
    QWidget* m_moduleTab = nullptr;            // “模块”页。
    QWidget* m_tokenTab = nullptr;             // “令牌”页。
    QWidget* m_tokenSwitchTab = nullptr;       // “令牌开关”页。
    QWidget* m_kernelObjectTab = nullptr;      // “内核对象”页。
    QWidget* m_hotkeyTab = nullptr;            // “进程热键”页。
    QWidget* m_keyboardTab = nullptr;          // “键盘”页。
    QWidget* m_pebTab = nullptr;               // “PEB”页。

    // ======== 详细信息页控件 ========
    QVBoxLayout* m_detailLayout = nullptr;     // 详细页总布局。
    QLabel* m_processIconLabel = nullptr;      // 顶部进程图标（40px）。
    QLabel* m_processTitleLabel = nullptr;     // 顶部标题（进程名 + PID）。
    QLineEdit* m_pathLineEdit = nullptr;       // 程序路径（只读）。
    QPushButton* m_copyPathButton = nullptr;   // 复制路径按钮。
    QPushButton* m_openPathFolderButton = nullptr; // 打开路径按钮。
    QLineEdit* m_commandLineEdit = nullptr;    // 启动命令行（只读）。
    QPushButton* m_copyCommandButton = nullptr; // 复制命令行按钮。
    QLabel* m_parentIconLabel = nullptr;       // 父进程图标（20px）。
    QLabel* m_parentInfoLabel = nullptr;       // 父进程名 + PID。
    QPushButton* m_openHandleDockButton = nullptr; // 跳转到句柄 Dock 按钮（按当前 PID 过滤）。
    QPushButton* m_gotoParentButton = nullptr; // 转到父进程按钮。

    QLabel* m_detailStartTimeValue = nullptr;  // 启动时间值。
    QLabel* m_detailUserValue = nullptr;       // 用户值。
    QLabel* m_detailAdminValue = nullptr;      // 是否管理员值。
    QLabel* m_detailArchitectureValue = nullptr; // 架构值。
    QLabel* m_detailPriorityValue = nullptr;   // 优先级值。
    QLabel* m_detailSessionValue = nullptr;    // 会话 ID 值。
    QLabel* m_detailThreadCountValue = nullptr; // 线程数值。
    QLabel* m_detailHandleCountValue = nullptr; // 句柄数值。
    QLabel* m_detailCpuValue = nullptr;        // CPU 当前占用值。
    QLabel* m_detailRamValue = nullptr;        // RAM 当前占用值。
    QLabel* m_detailDiskValue = nullptr;       // DISK 当前占用值。
    QLabel* m_detailSignatureValue = nullptr;  // 数字签名状态值。

    // ======== 线程页控件 ========
    QVBoxLayout* m_threadLayout = nullptr;     // 线程页总布局。
    QPushButton* m_refreshThreadInspectButton = nullptr; // 刷新线程细节按钮。
    QLabel* m_threadInspectStatusLabel = nullptr; // 线程细节刷新状态。
    QTableWidget* m_threadInspectTable = nullptr; // 线程细节表格。

    // ======== 操作页控件 ========
    QVBoxLayout* m_actionLayout = nullptr;     // 操作页总布局。
    QComboBox* m_terminateActionCombo = nullptr; // 结束方案下拉框。
    QPushButton* m_executeTerminateActionButton = nullptr; // 执行当前结束方案按钮。
    QPushButton* m_suspendProcessButton = nullptr; // 挂起进程。
    QPushButton* m_resumeProcessButton = nullptr; // 恢复进程。
    QPushButton* m_setCriticalButton = nullptr; // 设为关键进程。
    QPushButton* m_clearCriticalButton = nullptr; // 取消关键进程。

    QComboBox* m_priorityCombo = nullptr;      // 优先级选择框。
    QPushButton* m_applyPriorityButton = nullptr; // 应用优先级按钮。
    QPushButton* m_openProcessFolderButton = nullptr; // 打开进程所在目录按钮。
    QPushButton* m_refreshPplProtectionButton = nullptr; // 手动刷新 PPL 保护级别按钮。
    QPushButton* m_enableEfficiencyModeButton = nullptr; // 开启效率模式按钮。
    QPushButton* m_disableEfficiencyModeButton = nullptr; // 关闭效率模式按钮。
    QPushButton* m_r0TerminateProcessButton = nullptr; // R0 结束进程按钮。
    QPushButton* m_r0SuspendProcessButton = nullptr; // R0 挂起进程按钮。
    QPushButton* m_r0SetPplButton = nullptr; // R0 设置 PPL 层级按钮。
    QPushButton* m_r0VisibilityButton = nullptr; // R0 可恢复隐藏菜单按钮。
    QPushButton* m_r0DangerFlagsButton = nullptr; // R0 危险标志/DKOM 菜单按钮。

    QLineEdit* m_dllPathLineEdit = nullptr;    // DLL 路径输入框。
    QPushButton* m_browseDllButton = nullptr;  // 浏览 DLL 按钮。
    QPushButton* m_injectDllButton = nullptr;  // 执行 DLL 注入按钮。

    QLineEdit* m_shellcodePathLineEdit = nullptr; // shellcode 文件路径输入框。
    QPushButton* m_browseShellcodeButton = nullptr; // 浏览 shellcode 文件按钮。
    QPushButton* m_injectShellcodeButton = nullptr; // 执行 shellcode 注入按钮。

    // ======== 模块页控件 ========
    QVBoxLayout* m_moduleLayout = nullptr;     // 模块页总布局。
    QHBoxLayout* m_moduleTopBarLayout = nullptr; // 模块页顶部工具栏布局。
    QPushButton* m_refreshModuleButton = nullptr; // 模块刷新按钮。
    QCheckBox* m_signatureCheckBox = nullptr;  // 是否刷新时做签名校验。
    QLabel* m_moduleStatusLabel = nullptr;     // 模块刷新状态标签。
    QTreeWidget* m_moduleTable = nullptr;      // 模块表格。

    std::vector<ks::process::ProcessModuleRecord> m_moduleRecords; // 当前模块数据缓存。

    bool m_moduleRefreshing = false;           // 模块刷新进行中标记。
    bool m_moduleInitialRefreshStarted = false; // 模块页首次刷新是否已经按需启动。
    bool m_firstModuleRefreshDone = false;     // 首轮模块刷新是否已完成。
    std::uint64_t m_moduleRefreshTicket = 0;   // 模块刷新序号（防乱序）。
    int m_moduleRefreshProgressPid = 0;        // 首轮模块刷新对应的 kPro 任务 PID。

    // ======== 线程细节刷新状态 ========
    bool m_threadInspectRefreshing = false;        // 线程细节是否正在刷新。
    bool m_threadInspectInitialRefreshStarted = false; // 线程页首次刷新是否已经按需启动。
    std::uint64_t m_threadInspectRefreshTicket = 0;// 线程细节刷新序号。
    int m_threadInspectRefreshProgressPid = 0;     // 线程细节刷新对应进度 PID。
    std::vector<ThreadInspectItem> m_threadInspectRows; // 线程详情页最近一次刷新缓存。

    // ======== 内核对象页控件 ========
    QVBoxLayout* m_kernelObjectLayout = nullptr; // 内核对象页总布局。
    QLabel* m_kernelObjectR0StatusValue = nullptr; // R0 扩展读取状态。
    QLabel* m_kernelObjectCapabilityValue = nullptr; // DynData capability 位图。
    QLabel* m_kernelObjectProtectionValue = nullptr; // EPROCESS.Protection 原始值。
    QLabel* m_kernelObjectSignatureValue = nullptr; // SignatureLevel 原始值。
    QLabel* m_kernelObjectSectionSignatureValue = nullptr; // SectionSignatureLevel 原始值。
    QLabel* m_kernelObjectHandleTableValue = nullptr; // ObjectTable 可用性和当前指针。
    QLabel* m_kernelObjectSectionObjectValue = nullptr; // SectionObject 可用性和当前指针。
    QLabel* m_kernelObjectImagePathValue = nullptr; // R0 镜像路径。
    QLabel* m_kernelObjectSessionSourceValue = nullptr; // Session 来源。
    QLabel* m_kernelObjectImagePathSourceValue = nullptr; // 镜像路径来源。
    QLabel* m_kernelObjectProtectionSourceValue = nullptr; // Protection 来源。
    QLabel* m_kernelObjectSignatureSourceValue = nullptr; // SignatureLevel 来源。
    QLabel* m_kernelObjectSectionSignatureSourceValue = nullptr; // SectionSignatureLevel 来源。
    QLabel* m_kernelObjectObjectTableSourceValue = nullptr; // ObjectTable 来源。
    QLabel* m_kernelObjectSectionObjectSourceValue = nullptr; // SectionObject 来源。
    QLabel* m_kernelObjectProtectionOffsetValue = nullptr; // Protection 偏移。
    QLabel* m_kernelObjectSignatureOffsetValue = nullptr; // SignatureLevel 偏移。
    QLabel* m_kernelObjectSectionSignatureOffsetValue = nullptr; // SectionSignatureLevel 偏移。
    QLabel* m_kernelObjectObjectTableOffsetValue = nullptr; // ObjectTable 偏移。
    QLabel* m_kernelObjectSectionObjectOffsetValue = nullptr; // SectionObject 偏移。
    QPushButton* m_refreshSectionInfoButton = nullptr; // 刷新 Section/ControlArea 按钮。
    QLabel* m_sectionInfoStatusLabel = nullptr; // Section/ControlArea 查询状态。
    CodeEditorWidget* m_sectionInfoOutput = nullptr; // Section/ControlArea 详情文本输出。
    bool m_sectionInfoRefreshing = false; // Section 查询是否进行中。
    bool m_sectionInfoInitialRefreshStarted = false; // Section 页首次查询是否已经按需启动。
    std::uint64_t m_sectionInfoRefreshTicket = 0; // Section 查询序号。
    int m_sectionInfoRefreshProgressPid = 0; // Section 查询 kPro 任务 PID。

    // ======== 进程热键页控件与状态 ========
    QVBoxLayout* m_hotkeyLayout = nullptr;       // 进程热键页总布局。
    QPushButton* m_refreshHotkeyButton = nullptr; // 刷新热键按钮。
    QLabel* m_hotkeyStatusLabel = nullptr;       // 热键扫描状态。
    QTableWidget* m_hotkeyTable = nullptr;       // 热键结果表。
    bool m_hotkeyRefreshing = false;             // 热键扫描是否进行中。
    bool m_hotkeyInitialRefreshStarted = false;  // 热键页首次扫描是否已经按需启动。
    std::uint64_t m_hotkeyRefreshTicket = 0;     // 热键扫描序号。
    int m_hotkeyRefreshProgressPid = 0;          // 热键扫描 kPro 任务 PID。
    std::vector<HotkeyInspectItem> m_hotkeyRows; // 热键结果缓存。

    // ======== 键盘页控件与状态 ========
    QVBoxLayout* m_keyboardLayout = nullptr;       // 键盘页总布局。
    QPushButton* m_refreshKeyboardButton = nullptr; // 刷新键盘按钮。
    QLabel* m_keyboardStatusLabel = nullptr;       // 键盘页扫描状态。
    QTabWidget* m_keyboardInnerTabWidget = nullptr; // 键盘页内部热键/钩子分栏。
    QTableWidget* m_keyboardHotkeyTable = nullptr; // 键盘页热键表。
    QTableWidget* m_keyboardHookTable = nullptr;   // 键盘钩子结果表。
    bool m_keyboardRefreshing = false;             // 键盘页扫描是否进行中。
    bool m_keyboardInitialRefreshStarted = false;  // 键盘页首次扫描是否已启动。
    std::uint64_t m_keyboardRefreshTicket = 0;     // 键盘页扫描序号。
    int m_keyboardRefreshProgressPid = 0;          // 键盘页扫描 kPro 任务 PID。
    std::vector<HotkeyInspectItem> m_keyboardHotkeyRows; // 键盘页热键缓存。
    std::vector<KeyboardHookInspectItem> m_keyboardHookRows; // 键盘页钩子缓存。

    // ======== 令牌页控件与状态 ========
    QVBoxLayout* m_tokenLayout = nullptr;          // 令牌页布局。
    QPushButton* m_refreshTokenButton = nullptr;   // 刷新令牌信息按钮。
    QLabel* m_tokenStatusLabel = nullptr;          // 令牌页状态文本。
    CodeEditorWidget* m_tokenDetailOutput = nullptr; // 令牌信息输出框（统一文本编辑器组件，只读）。
    bool m_tokenRefreshing = false;                // 令牌页刷新状态。
    bool m_tokenInitialRefreshStarted = false;     // 令牌页首次刷新是否已经按需启动。
    std::uint64_t m_tokenRefreshTicket = 0;        // 令牌页刷新序号。
    int m_tokenRefreshProgressPid = 0;             // 令牌页刷新进度 PID。

    // ======== 令牌开关页控件与状态 ========
    QVBoxLayout* m_tokenSwitchLayout = nullptr;    // 令牌开关页总布局。
    QPushButton* m_refreshTokenSwitchButton = nullptr; // 刷新令牌开关按钮。
    QPushButton* m_applyTokenSwitchButton = nullptr;   // 应用令牌开关按钮。
    QPushButton* m_refreshTokenAllInfoButton = nullptr; // 刷新全部令牌信息按钮（触发全信息类枚举）。
    QLabel* m_tokenSwitchStatusLabel = nullptr;    // 令牌开关应用状态文本。
    bool m_tokenSwitchInitialRefreshStarted = false; // 令牌开关页首次回读是否已经按需启动。
    QCheckBox* m_tokenSandboxInertCheck = nullptr; // SandboxInert 开关。
    QCheckBox* m_tokenVirtualizationAllowedCheck = nullptr; // VirtualizationAllowed 开关。
    QCheckBox* m_tokenVirtualizationEnabledCheck = nullptr; // VirtualizationEnabled 开关。
    QCheckBox* m_tokenUiAccessCheck = nullptr;     // UIAccess 开关。
    QCheckBox* m_tokenMandatoryNoWriteUpCheck = nullptr; // MandatoryPolicy: NoWriteUp 位。
    QCheckBox* m_tokenMandatoryNewProcessMinCheck = nullptr; // MandatoryPolicy: NewProcessMin 位。
    QCheckBox* m_tokenHasRestrictionsCheck = nullptr; // TokenHasRestrictions（class=21）开关。
    QCheckBox* m_tokenIsAppContainerCheck = nullptr;  // TokenIsAppContainer（class=29）开关。
    QCheckBox* m_tokenIsRestrictedCheck = nullptr; // TokenIsRestricted（class=40）开关。
    QCheckBox* m_tokenIsLessPrivilegedAppContainerCheck = nullptr; // TokenIsLessPrivilegedAppContainer（class=46）开关。
    QCheckBox* m_tokenIsSandboxedCheck = nullptr; // TokenIsSandboxed（class=47）开关。
    QCheckBox* m_tokenIsAppSiloCheck = nullptr; // TokenIsAppSilo（class=51）开关。
    QComboBox* m_tokenRawInfoClassCombo = nullptr; // 原始设置：TokenInformationClass 选择框。
    QComboBox* m_tokenRawInputModeCombo = nullptr; // 原始设置：负载输入模式（UInt32/UInt64/HexBytes）。
    QLineEdit* m_tokenRawPayloadEdit = nullptr;    // 原始设置：负载输入文本。
    QPushButton* m_tokenRawApplyButton = nullptr;  // 原始设置：执行 NtSetInformationToken 的按钮。

    // ======== PEB页控件与状态 ========
    QVBoxLayout* m_pebLayout = nullptr;            // PEB 页布局。
    QPushButton* m_refreshPebButton = nullptr;     // 刷新 PEB 信息按钮。
    QPushButton* m_applyPebEditButton = nullptr;   // 应用 PEB 可编辑字段按钮。
    QLabel* m_pebStatusLabel = nullptr;            // PEB 页状态文本。
    QComboBox* m_pebTargetCombo = nullptr;          // PEB 写入目标：NativePEB / Wow64PEB。
    QLineEdit* m_pebCommandLineEdit = nullptr;      // 可编辑 CommandLine。
    QLineEdit* m_pebImagePathEdit = nullptr;        // 可编辑 ImagePathName。
    QLineEdit* m_pebCurrentDirectoryEdit = nullptr; // 可编辑 CurrentDirectory.DosPath。
    QLineEdit* m_pebImageBaseEdit = nullptr;        // 高级：PEB.ImageBaseAddress。
    QLineEdit* m_pebAffinityMaskEdit = nullptr;     // 可编辑进程亲和性掩码。
    QComboBox* m_pebPriorityClassCombo = nullptr;   // 可编辑优先级。
    QLineEdit* m_pebEnvironmentNameEdit = nullptr;  // 环境变量名。
    QLineEdit* m_pebEnvironmentValueEdit = nullptr; // 环境变量值。
    QPlainTextEdit* m_pebReadonlyReasonOutput = nullptr; // 不可直接修改字段说明。
    CodeEditorWidget* m_pebDetailOutput = nullptr;   // PEB 信息输出框（统一文本编辑器组件，只读）。
    bool m_pebRefreshing = false;                  // PEB 页刷新状态。
    bool m_pebInitialRefreshStarted = false;       // PEB 页首次刷新是否已经按需启动。
    std::uint64_t m_pebRefreshTicket = 0;          // PEB 页刷新序号。
    int m_pebRefreshProgressPid = 0;               // PEB 页刷新进度 PID。

    // applyPebEditableFields：
    // - 读取 PEB 页编辑区输入；
    // - 尽量写回远程 PEB/ProcessParameters 与进程基础运行属性；
    // - 成功/失败通过状态栏和消息框反馈。
    void applyPebEditableFields();
    // populatePebEditableFieldsFromText：
    // - 从 PEB 刷新文本中提取当前目标 PEB 的可写字段；
    // - 自动填充编辑框，避免用户手工复制长命令行或路径；
    // - 仅更新 UI 控件，无返回值。
    void populatePebEditableFieldsFromText(const QString& detailText);
    bool m_staticDetailRefreshing = false;         // 静态详情后台补齐是否进行中。
    bool m_staticDetailRefreshAttempted = false;   // 静态详情是否已经尝试后台补齐，避免周期刷新重复排队。
    std::uint64_t m_staticDetailRefreshTicket = 0; // 静态详情刷新序号。
    bool m_themeStyleApplying = false;             // 主题样式重建防重入标记，避免 changeEvent 循环触发。

    // 图标缓存：路径 -> 图标，避免重复读取系统图标。
    QHash<QString, QIcon> m_iconCacheByPath;
};
