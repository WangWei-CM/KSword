#pragma once

// ============================================================
// MemoryDock.h
// 作用：
// 1) 构建“内存”页面的完整多 Tab 交互界面；
// 2) 提供进程附加、模块查看、内存区域浏览、扫描、十六进制查看；
// 3) 提供断点与书签管理的基础能力。
// ============================================================

#include "../Framework.h"

#include <QWidget>

#include <atomic>      // std::atomic：扫描取消标志、并发状态标志。
#include <cstdint>     // std::uint32_t / std::uint64_t：PID、地址等固定宽度整数。
#include <string>      // std::string：日志与 Win32 调用时的字符串桥接。
#include <vector>      // std::vector：缓存进程/模块/区域/扫描结果。

// Qt 前置声明：尽量减少头文件编译依赖。
class QAction;
class QCheckBox;
class QComboBox;
class QHBoxLayout;
class QLabel;
class QLineEdit;
class QMenu;
class QPoint;
class QProgressBar;
class QPushButton;
class QSplitter;
class QStatusBar;
class QTableWidget;
class QTabWidget;
class QTextEdit;
class QTimer;
class QTreeWidget;
class QVBoxLayout;
class HexEditorWidget;

// Windows 句柄类型前置声明。
typedef void* HANDLE;

// ============================================================
// MemoryDock
// 说明：
// - 该类负责整个“内存页”的 UI 与业务逻辑；
// - 所有函数都在 cpp 中附带详细注释，便于后续维护。
// ============================================================
class MemoryDock final : public QWidget
{
    Q_OBJECT

public:
    // 构造函数：
    // - 作用：初始化全部 UI、连接信号槽、加载初始进程列表。
    // - 参数 parent：Qt 父控件指针，可为空。
    explicit MemoryDock(QWidget* parent = nullptr);

    // 析构函数：
    // - 作用：在控件销毁前释放进程句柄、停止定时器与后台扫描状态。
    ~MemoryDock() override;

private:
    // ========================================================
    // 内部数据结构定义（用于表格缓存与跨 Tab 共享状态）
    // ========================================================

    // ProcessEntry：
    // - 作用：保存单个进程行展示所需的数据。
    struct ProcessEntry
    {
        std::uint32_t pid = 0;          // 进程 PID。
        std::uint32_t sessionId = 0;    // 会话 ID。
        QString processName;            // 进程名。
        double cpuPercent = 0.0;        // CPU 占用（当前实现可选，默认为 0）。
        double workingSetMB = 0.0;      // 工作集内存（MB）。
    };

    // ModuleEntry：
    // - 作用：保存模块表格每一行的数据。
    struct ModuleEntry
    {
        QString moduleName;                     // 模块文件名（路径末尾）。
        QString fullPath;                       // 模块完整路径。
        std::uint64_t baseAddress = 0;          // 模块基址（用于跳转与复制）。
        std::uint64_t sizeBytes = 0;            // 模块大小（字节）。
        QString signatureState;                 // 数字签名状态文本（Signed/Unknown/...）。
        bool signatureTrusted = false;          // 签名是否可信（用于上色）。
        std::uint64_t entryPointOffset = 0;     // 入口点偏移（RVA）。
        QString runningState;                   // 运行状态文本（Running/Suspended/...）。
        QString threadIdText;                   // 代表线程 ID 文本（可能包含多个）。
        std::uint32_t representativeThreadId = 0; // 代表线程 ID（数值动作入口）。
    };

    // RegionEntry：
    // - 作用：保存 VirtualQueryEx 枚举得到的内存区域信息。
    struct RegionEntry
    {
        std::uint64_t baseAddress = 0;  // 区域起始地址。
        std::uint64_t regionSize = 0;   // 区域大小（字节）。
        std::uint32_t protect = 0;      // 保护属性位（PAGE_*）。
        std::uint32_t state = 0;        // 状态（MEM_COMMIT / MEM_RESERVE / MEM_FREE）。
        std::uint32_t type = 0;         // 类型（MEM_IMAGE / MEM_MAPPED / MEM_PRIVATE）。
        QString mappedFilePath;         // 映射文件路径（如果可获取）。
    };

    // SearchValueType：
    // - 作用：定义扫描值的数据类型。
    enum class SearchValueType : int
    {
        Byte = 0,           // 1 字节整数。
        Int16,              // 2 字节整数。
        Int32,              // 4 字节整数。
        Int64,              // 8 字节整数。
        Float32,            // 单精度浮点。
        Float64,            // 双精度浮点。
        ByteArray,          // 字节数组（支持 ?? 通配）。
        StringAscii,        // ASCII 字符串。
        StringUnicode       // Unicode 字符串（UTF-16LE）。
    };

    // SearchCompareMode：
    // - 作用：定义再次扫描过滤条件。
    enum class SearchCompareMode : int
    {
        Equal = 0,          // 等于。
        Greater,            // 大于。
        Less,               // 小于。
        Between,            // 介于（当前值在 [A, B]）。
        Changed,            // 变化。
        Unchanged,          // 未变化。
        Increased,          // 增加。
        Decreased           // 减少。
    };

    // SearchResultEntry：
    // - 作用：保存扫描结果行。
    struct SearchResultEntry
    {
        std::uint64_t address = 0;      // 命中地址。
        QByteArray currentValueBytes;   // 当前值字节。
        QByteArray previousValueBytes;  // 上一轮值字节（再次扫描时有意义）。
        QString noteText;               // 备注文本（可由用户后续填充）。
    };

    // BreakpointEntry：
    // - 作用：保存软件断点信息（0xCC）。
    struct BreakpointEntry
    {
        std::uint64_t address = 0;      // 断点地址。
        std::uint8_t originalByte = 0;  // 原始字节（用于恢复）。
        bool enabled = false;           // 当前是否启用。
        std::uint64_t hitCount = 0;     // 命中次数（当前版本预留）。
        QString description;            // 断点描述文本。
    };

    // BookmarkEntry：
    // - 作用：保存用户书签信息。
    struct BookmarkEntry
    {
        std::uint64_t address = 0;      // 书签地址。
        QString noteText;               // 备注文本。
        QString addTimeText;            // 添加时间文本。
        QByteArray lastValueBytes;      // 上次刷新值（用于变化观察）。
    };

    // ParsedSearchPattern：
    // - 作用：保存扫描输入解析结果，避免线程内重复解析字符串。
    struct ParsedSearchPattern
    {
        SearchValueType valueType = SearchValueType::Byte; // 数据类型。
        QByteArray exactBytes;         // 精确匹配字节。
        QByteArray wildcardMask;       // 通配掩码（ByteArray 模式使用，1=有效，0=通配）。
        double lowerBound = 0.0;       // 数值下界（Between 或浮点误差比较使用）。
        double upperBound = 0.0;       // 数值上界。
        double epsilon = 0.00001;      // 浮点误差阈值。
    };

private:
    // ========================================================
    // UI 初始化与连接函数
    // ========================================================

    // initializeUi：
    // - 作用：初始化根布局、工具栏、Tab 区域与状态栏。
    // - 返回：无。
    void initializeUi();

    // initializeToolbar：
    // - 作用：初始化顶部全局工具栏。
    // - 返回：无。
    void initializeToolbar();

    // initializeTabs：
    // - 作用：初始化五个 Tab 页。
    // - 返回：无。
    void initializeTabs();

    // initializeProcessModuleTab：
    // - 作用：构建 Tab1（进程与模块）界面。
    // - 返回：无。
    void initializeProcessModuleTab();

    // initializeMemoryRegionTab：
    // - 作用：构建 Tab2（内存区域）界面。
    // - 返回：无。
    void initializeMemoryRegionTab();

    // initializeMemorySearchTab：
    // - 作用：构建 Tab3（内存搜索）界面。
    // - 返回：无。
    void initializeMemorySearchTab();

    // initializeMemoryViewerTab：
    // - 作用：构建 Tab4（内存查看器）界面。
    // - 返回：无。
    void initializeMemoryViewerTab();

    // initializeBreakpointBookmarkTab：
    // - 作用：构建 Tab5（断点与书签）界面。
    // - 返回：无。
    void initializeBreakpointBookmarkTab();

    // initializeConnections：
    // - 作用：统一连接各控件交互逻辑。
    // - 返回：无。
    void initializeConnections();

    // initializeStatusBar：
    // - 作用：初始化状态栏默认文本。
    // - 返回：无。
    void initializeStatusBar();

    // initializeBookmarkRefreshTimer：
    // - 作用：初始化书签刷新定时器（默认 1 秒）。
    // - 返回：无。
    void initializeBookmarkRefreshTimer();

private:
    // ========================================================
    // 进程与模块（Tab1）相关函数
    // ========================================================

    // refreshProcessList：
    // - 作用：重新枚举系统进程并刷新进程表/下拉框。
    // - 参数 keepSelection：是否尽量保持当前选中 PID。
    // - 返回：无。
    void refreshProcessList(bool keepSelection);

    // updateProcessComboFromCache：
    // - 作用：根据 m_processCache 重建顶部“进程选择”下拉框。
    // - 返回：无。
    void updateProcessComboFromCache();

    // refreshModuleListForPid：
    // - 作用：按 PID 枚举模块并刷新模块列表。
    // - 参数 pid：目标进程 PID。
    // - 返回：true 表示枚举成功；false 表示失败。
    bool refreshModuleListForPid(std::uint32_t pid);

    // rebuildModuleTableFromCache：
    // - 作用：按当前过滤条件把 m_moduleCache 重绘到模块表。
    // - 说明：该函数只做“缓存 -> UI”投影，不做 Win32 枚举。
    // - 返回：无。
    void rebuildModuleTableFromCache();

    // attachToProcess：
    // - 作用：附加目标进程并缓存句柄。
    // - 参数 pid：目标 PID。
    // - 参数 processName：目标进程名（用于状态栏展示）。
    // - 参数 showMessage：是否弹出提示框反馈结果。
    // - 返回：true 附加成功；false 附加失败。
    bool attachToProcess(std::uint32_t pid, const QString& processName, bool showMessage);

    // detachProcess：
    // - 作用：分离当前进程并清理依赖该句柄的数据。
    // - 返回：无。
    void detachProcess();

    // openProcessHandleForRead：
    // - 作用：按 PID 打开进程句柄（读/查询权限）。
    // - 参数 pid：目标 PID。
    // - 参数 errorTextOut：失败时输出错误文本（可空）。
    // - 返回：成功返回有效 HANDLE；失败返回 nullptr。
    HANDLE openProcessHandleForRead(std::uint32_t pid, QString* errorTextOut = nullptr) const;

    // showProcessTableContextMenu：
    // - 作用：展示进程列表右键菜单（附加 / Dump 内存）。
    // - 参数 localPosition：鼠标在进程表 viewport 内的坐标。
    // - 返回：无。
    void showProcessTableContextMenu(const QPoint& localPosition);

    // requestDumpProcessMemoryByPid：
    // - 作用：弹出保存文件对话框并异步执行目标进程内存转储。
    // - 参数 pid：要转储的目标进程 PID。
    // - 参数 processName：目标进程名（用于默认文件名和提示）。
    // - 返回：无。
    void requestDumpProcessMemoryByPid(std::uint32_t pid, const QString& processName);

    // dumpProcessMemoryToFile：
    // - 作用：执行真正的内存区域遍历与文件写入。
    // - 参数 pid：目标 PID。
    // - 参数 dumpFilePath：输出文件路径。
    // - 参数 errorTextOut：失败时输出错误信息。
    // - 返回：true=成功；false=失败。
    bool dumpProcessMemoryToFile(
        std::uint32_t pid,
        const QString& dumpFilePath,
        QString& errorTextOut);

private:
    // ========================================================
    // 内存区域（Tab2）相关函数
    // ========================================================

    // refreshMemoryRegionList：
    // - 作用：刷新内存区域缓存并应用过滤展示。
    // - 参数 forceRequery：是否强制重新调用 VirtualQueryEx。
    // - 返回：无。
    void refreshMemoryRegionList(bool forceRequery);

    // enumerateMemoryRegionsByVirtualQuery：
    // - 作用：对附加进程执行 VirtualQueryEx 遍历。
    // - 参数 processHandle：目标进程句柄。
    // - 参数 regionsOut：输出区域数组（调用前会清空）。
    // - 参数 errorTextOut：失败信息输出（可空）。
    // - 返回：true 成功；false 失败。
    bool enumerateMemoryRegionsByVirtualQuery(
        HANDLE processHandle,
        std::vector<RegionEntry>& regionsOut,
        QString* errorTextOut = nullptr) const;

    // applyRegionFilterAndRebuildTable：
    // - 作用：按复选框条件过滤区域并重建 Tab2 表格。
    // - 返回：无。
    void applyRegionFilterAndRebuildTable();

private:
    // ========================================================
    // 内存搜索（Tab3）相关函数
    // ========================================================

    // parseSearchPatternFromUi：
    // - 作用：把 UI 输入解析成可执行扫描的结构体。
    // - 参数 patternOut：输出解析结果。
    // - 参数 errorTextOut：解析失败时输出错误原因。
    // - 返回：true 解析成功；false 解析失败。
    bool parseSearchPatternFromUi(
        ParsedSearchPattern& patternOut,
        QString& errorTextOut) const;

    // collectSearchRegionsFromUi：
    // - 作用：根据“范围选项/过滤选项”确定扫描区域集合。
    // - 参数 regionsOut：输出区域数组。
    // - 参数 errorTextOut：失败原因文本。
    // - 返回：true 成功；false 失败。
    bool collectSearchRegionsFromUi(
        std::vector<RegionEntry>& regionsOut,
        QString& errorTextOut);

    // startFirstScan：
    // - 作用：执行“首次扫描”。
    // - 返回：无。
    void startFirstScan();

    // startNextScan：
    // - 作用：执行“再次扫描”。
    // - 返回：无。
    void startNextScan();

    // resetScanState：
    // - 作用：重置扫描状态、清空结果表。
    // - 返回：无。
    void resetScanState();

    // cancelCurrentScan：
    // - 作用：设置扫描取消标志，后台线程会尽快停止。
    // - 返回：无。
    void cancelCurrentScan();

    // rebuildSearchResultTable：
    // - 作用：按 m_searchResultCache 重建结果表格。
    // - 返回：无。
    void rebuildSearchResultTable();

    // scanMemoryRegionsInBackground：
    // - 作用：后台执行首次扫描并在完成后回主线程提交结果。
    // - 参数 scanRegions：本轮扫描区域。
    // - 参数 pattern：本轮匹配模式。
    // - 返回：无。
    void scanMemoryRegionsInBackground(
        const std::vector<RegionEntry>& scanRegions,
        const ParsedSearchPattern& pattern);

private:
    // ========================================================
    // 内存查看器（Tab4）相关函数
    // ========================================================

    // jumpToAddressFromUi：
    // - 作用：读取地址输入框并跳转到目标地址。
    // - 返回：无。
    void jumpToAddressFromUi();

    // jumpToAddress：
    // - 作用：切换到指定地址并刷新一页十六进制视图。
    // - 参数 address：目标地址。
    // - 返回：无。
    void jumpToAddress(std::uint64_t address);

    // reloadMemoryViewerPage：
    // - 作用：从当前地址重新读取并重建十六进制表格。
    // - 返回：无。
    void reloadMemoryViewerPage();

    // writeSingleByteAtViewer：
    // - 作用：修改当前视图中的一个字节。
    // - 参数 absoluteAddress：目标地址。
    // - 参数 value：要写入的字节值。
    // - 参数 errorTextOut：失败时输出错误文本。
    // - 返回：true 写入成功；false 写入失败。
    bool writeSingleByteAtViewer(
        std::uint64_t absoluteAddress,
        std::uint8_t value,
        QString& errorTextOut);

private:
    // ========================================================
    // 断点与书签（Tab5）相关函数
    // ========================================================

    // addBreakpointByAddress：
    // - 作用：在指定地址写入 0xCC 并记录原字节。
    // - 参数 address：断点地址。
    // - 参数 description：断点描述文本。
    // - 参数 errorTextOut：失败信息输出。
    // - 返回：true 成功；false 失败。
    bool addBreakpointByAddress(
        std::uint64_t address,
        const QString& description,
        QString& errorTextOut);

    // removeBreakpointByRow：
    // - 作用：删除断点并恢复原字节。
    // - 参数 row：断点表中的行索引。
    // - 返回：true 成功；false 失败。
    bool removeBreakpointByRow(int row);

    // setBreakpointEnabledByRow：
    // - 作用：启用或禁用断点。
    // - 参数 row：断点表行索引。
    // - 参数 enabled：目标状态（true=启用，false=禁用）。
    // - 返回：true 成功；false 失败。
    bool setBreakpointEnabledByRow(int row, bool enabled);

    // rebuildBreakpointTable：
    // - 作用：重建断点表格显示。
    // - 返回：无。
    void rebuildBreakpointTable();

    // addBookmarkByAddress：
    // - 作用：添加书签记录。
    // - 参数 address：书签地址。
    // - 参数 noteText：备注文本。
    // - 返回：无。
    void addBookmarkByAddress(std::uint64_t address, const QString& noteText);

    // rebuildBookmarkTable：
    // - 作用：重建书签表格显示。
    // - 返回：无。
    void rebuildBookmarkTable();

    // refreshBookmarkValues：
    // - 作用：刷新书签当前值列，便于监控变量变化。
    // - 返回：无。
    void refreshBookmarkValues();

private:
    // ========================================================
    // 通用辅助函数
    // ========================================================

    // updateStatusBarText：
    // - 作用：更新状态栏（进程名、PID、读写状态）。
    // - 返回：无。
    void updateStatusBarText();

    // parseAddressText：
    // - 作用：解析十进制或十六进制地址字符串。
    // - 参数 text：输入文本。
    // - 参数 valueOut：输出地址数值。
    // - 返回：true 解析成功；false 解析失败。
    static bool parseAddressText(const QString& text, std::uint64_t& valueOut);

    // parseUnsignedNumber：
    // - 作用：解析通用无符号整数（支持 0x 与十进制）。
    // - 参数 text：输入文本。
    // - 参数 valueOut：输出值。
    // - 返回：true 成功；false 失败。
    static bool parseUnsignedNumber(const QString& text, std::uint64_t& valueOut);

    // formatAddress：
    // - 作用：格式化地址为 16 位十六进制文本。
    // - 参数 address：地址值。
    // - 返回：格式化后的 QString。
    static QString formatAddress(std::uint64_t address);

    // formatSize：
    // - 作用：格式化字节大小（B/KB/MB/GB）。
    // - 参数 sizeBytes：字节数。
    // - 返回：可读文本。
    static QString formatSize(std::uint64_t sizeBytes);

    // protectToText：
    // - 作用：把 PAGE_* 保护值转换为简写（R--、RW-、RX 等）。
    // - 参数 protect：Win32 protect 值。
    // - 返回：可读文本。
    static QString protectToText(std::uint32_t protect);

    // stateToText：
    // - 作用：把 MEM_* 状态值转换为可读文本。
    // - 参数 state：Win32 state 值。
    // - 返回：可读文本。
    static QString stateToText(std::uint32_t state);

    // typeToText：
    // - 作用：把 MEM_* 类型值转换为可读文本。
    // - 参数 type：Win32 type 值。
    // - 返回：可读文本。
    static QString typeToText(std::uint32_t type);

    // bytesToDisplayString：
    // - 作用：按数据类型把字节数组转换为可读值字符串。
    // - 参数 bytes：原始字节。
    // - 参数 valueType：目标数据类型。
    // - 返回：格式化后的可读文本。
    static QString bytesToDisplayString(const QByteArray& bytes, SearchValueType valueType);

private:
    // ========================================================
    // 顶层布局与全局控件
    // ========================================================

    QVBoxLayout* m_rootLayout = nullptr;      // 根布局（垂直：工具栏 + Tab + 状态栏）。
    QHBoxLayout* m_toolbarLayout = nullptr;   // 顶部工具栏布局。
    QTabWidget* m_tabWidget = nullptr;        // 五个功能 Tab 的容器。
    QStatusBar* m_statusBar = nullptr;        // 底部状态栏。

    // 工具栏控件。
    QComboBox* m_processCombo = nullptr;      // 进程选择下拉框。
    QPushButton* m_attachButton = nullptr;    // 附加按钮。
    QPushButton* m_detachButton = nullptr;    // 分离按钮。
    QPushButton* m_refreshButton = nullptr;   // 刷新按钮。
    QPushButton* m_settingsButton = nullptr;  // 设置按钮（线程数、缓存大小）。

    // 状态栏标签。
    QLabel* m_statusProcessLabel = nullptr;   // 显示当前进程名。
    QLabel* m_statusPidLabel = nullptr;       // 显示当前 PID。
    QLabel* m_statusMemoryIoLabel = nullptr;  // 显示当前读写状态。

    // ========================================================
    // Tab1：进程与模块
    // ========================================================

    QWidget* m_tabProcessModule = nullptr;    // Tab1 页面容器。
    QTableWidget* m_processTable = nullptr;   // 进程列表表格。
    QLineEdit* m_moduleFilterEdit = nullptr;  // 模块名称过滤输入框。
    QPushButton* m_moduleRefreshButton = nullptr; // 模块刷新按钮。
    QCheckBox* m_moduleSignatureCheck = nullptr;  // 模块刷新时是否校验签名。
    QLabel* m_moduleStatusLabel = nullptr;        // 模块刷新状态标签。
    QTreeWidget* m_moduleTable = nullptr;         // 模块列表表格（树形表头风格）。

    // ========================================================
    // Tab2：内存区域
    // ========================================================

    QWidget* m_tabRegions = nullptr;          // Tab2 页面容器。
    QCheckBox* m_regionCommittedOnlyCheck = nullptr; // 仅已提交区域过滤。
    QCheckBox* m_regionImageOnlyCheck = nullptr;     // 仅 IMAGE 类型过滤。
    QCheckBox* m_regionReadableOnlyCheck = nullptr;  // 仅可读区域过滤。
    QTableWidget* m_regionTable = nullptr;    // 内存区域表格。

    // ========================================================
    // Tab3：内存搜索
    // ========================================================

    QWidget* m_tabSearch = nullptr;           // Tab3 页面容器。
    QComboBox* m_searchTypeCombo = nullptr;   // 数据类型下拉框。
    QLineEdit* m_searchValueEdit = nullptr;   // 搜索值输入框。
    QComboBox* m_searchRangeCombo = nullptr;  // 范围模式下拉框（全内存/自定义）。
    QLineEdit* m_searchRangeStartEdit = nullptr; // 自定义范围起始地址。
    QLineEdit* m_searchRangeEndEdit = nullptr;   // 自定义范围结束地址。
    QCheckBox* m_searchImageOnlyCheck = nullptr; // 仅 IMAGE 区域。
    QCheckBox* m_searchHeapOnlyCheck = nullptr;  // 仅堆区域（当前按 PRIVATE 近似）。
    QCheckBox* m_searchStackOnlyCheck = nullptr; // 仅栈区域（当前预留标记）。
    QPushButton* m_firstScanButton = nullptr; // 首次扫描按钮。
    QPushButton* m_nextScanButton = nullptr;  // 再次扫描按钮。
    QPushButton* m_resetScanButton = nullptr; // 重置扫描按钮。
    QPushButton* m_cancelScanButton = nullptr;// 取消扫描按钮。
    QComboBox* m_nextScanCompareCombo = nullptr; // 再次扫描条件下拉框。
    QLineEdit* m_nextScanValueEdit = nullptr;    // 再次扫描值输入框。
    QLineEdit* m_nextScanValueBEdit = nullptr;   // Between 上界输入框。
    QTableWidget* m_searchResultTable = nullptr; // 扫描结果表格。
    QProgressBar* m_scanProgressBar = nullptr;   // 扫描进度条。
    QLabel* m_scanStatusLabel = nullptr;         // 扫描状态文本。

    // ========================================================
    // Tab4：内存查看器
    // ========================================================

    QWidget* m_tabViewer = nullptr;           // Tab4 页面容器。
    QLineEdit* m_viewAddressEdit = nullptr;   // 地址导航输入框。
    QPushButton* m_viewJumpButton = nullptr;  // 跳转按钮。
    QLabel* m_viewProtectLabel = nullptr;     // 当前地址保护属性标签。
    HexEditorWidget* m_hexEditorWidget = nullptr; // 统一十六进制编辑器组件。
    QLabel* m_viewerStatusLabel = nullptr;    // 查看器状态文本。

    // ========================================================
    // Tab5：断点与书签
    // ========================================================

    QWidget* m_tabBpBookmark = nullptr;       // Tab5 页面容器。
    QTableWidget* m_breakpointTable = nullptr;// 断点表格。
    QPushButton* m_addBreakpointButton = nullptr;    // 添加断点按钮。
    QPushButton* m_removeBreakpointButton = nullptr; // 删除断点按钮。
    QPushButton* m_toggleBreakpointButton = nullptr; // 启用/禁用断点按钮。
    QTableWidget* m_bookmarkTable = nullptr;  // 书签表格。
    QPushButton* m_addBookmarkButton = nullptr;      // 添加书签按钮。
    QPushButton* m_removeBookmarkButton = nullptr;   // 删除书签按钮。
    QPushButton* m_refreshBookmarkButton = nullptr;  // 刷新书签值按钮。
    QPushButton* m_jumpBookmarkButton = nullptr;     // 跳转书签按钮。

private:
    // ========================================================
    // 运行时状态与缓存
    // ========================================================

    HANDLE m_attachedProcessHandle = nullptr; // 当前附加的目标进程句柄。
    std::uint32_t m_attachedPid = 0;          // 当前附加 PID。
    QString m_attachedProcessName;            // 当前附加进程名。
    bool m_canReadWriteMemory = false;        // 当前句柄是否可读写内存。

    std::vector<ProcessEntry> m_processCache; // 进程缓存（Tab1/工具栏复用）。
    std::vector<ModuleEntry> m_moduleCache;   // 模块缓存（Tab1 使用）。
    std::atomic<bool> m_moduleRefreshInProgress{ false }; // 模块刷新是否进行中（异步任务状态）。
    std::atomic<std::uint64_t> m_moduleRefreshTicket{ 0 }; // 模块刷新票据（丢弃过期结果）。
    int m_dumpMemoryProgressPid = 0;        // Dump 内存任务的进度条 PID。
    std::vector<RegionEntry> m_regionCache;   // 区域缓存（Tab2/Tab3 复用）。

    std::vector<SearchResultEntry> m_searchResultCache; // 扫描结果缓存（Tab3）。
    std::size_t m_searchResultVisibleCount = 0;         // 当前结果表实际显示条数（可能小于缓存总数）。
    SearchValueType m_lastSearchValueType = SearchValueType::Byte; // 最近一次扫描类型。
    std::atomic<bool> m_scanInProgress{ false };       // 当前是否正在扫描。
    std::atomic<bool> m_scanCancelRequested{ false };  // 扫描取消标志。
    std::uint32_t m_scanThreadCount = 4;               // 扫描线程数（设置可调）。
    std::uint32_t m_scanChunkSizeKB = 1024;            // 单次读取块大小（KB，设置可调）。

    std::uint64_t m_currentViewerAddress = 0;          // Tab4 当前起始地址。
    QByteArray m_currentViewerPageBytes;               // Tab4 当前页原始字节缓存。

    std::vector<BreakpointEntry> m_breakpointCache;    // 断点缓存（Tab5）。
    std::vector<BookmarkEntry> m_bookmarkCache;        // 书签缓存（Tab5）。
    QTimer* m_bookmarkRefreshTimer = nullptr;          // 书签刷新定时器。
};
