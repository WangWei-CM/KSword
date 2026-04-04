#pragma once

// ============================================================
// HandleDock.h
// 作用：
// - 提供“句柄”Dock 页面，包含“句柄列表 / 对象类型”两个 Tab；
// - 句柄列表页实现 R3 句柄枚举、过滤、右键复制与关闭句柄；
// - 对象类型页复刻原内核对象类型视图，并为句柄页提供类型名映射。
// ============================================================

#include "../Framework.h"
#include "HandleObjectTypeWorker.h"

#include <QHash>
#include <QIcon>
#include <QWidget>

#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

class QCheckBox;
class QComboBox;
class QHBoxLayout;
class QLabel;
class QLineEdit;
class QPoint;
class QPushButton;
class QShowEvent;
class QSpinBox;
class QTabWidget;
class QTreeWidget;
class QTreeWidget;
class QTreeWidgetItem;
class QVBoxLayout;

class HandleDock final : public QWidget
{
    Q_OBJECT

public:
    // 构造函数作用：
    // - 创建句柄页面全部控件与多 Tab 结构；
    // - 初始化筛选条件、右键菜单、异步刷新入口。
    // 调用方法：MainWindow 创建 HandleDock(this)。
    // 传入 parent：Qt 父对象。
    // 传出：无（通过对象状态持有 UI 与缓存）。
    explicit HandleDock(QWidget* parent = nullptr);

    // focusProcessId 作用：
    // - 外部调用时把 PID 过滤框切换为目标 PID；
    // - 自动切到“句柄列表”Tab，并可选立即触发刷新。
    // 调用方法：MainWindow::focusHandleDockByPid 调用本函数。
    // 传入 processId：目标进程 PID；triggerRefresh：是否立即刷新。
    // 传出：无（内部更新 UI 状态与刷新任务）。
    void focusProcessId(std::uint32_t processId, bool triggerRefresh);

protected:
    // showEvent 作用：
    // - 页面首次可见时触发对象类型与句柄首轮刷新；
    // - 避免主窗口启动阶段被重查询拖慢。
    // 调用方法：Qt 自动回调。
    // 传入 event：显示事件对象。
    // 传出：无。
    void showEvent(QShowEvent* event) override;

private:
    // HandleTableColumn 作用：
    // - 统一定义句柄列表列索引，杜绝魔法数字。
    enum class HandleTableColumn : int
    {
        ProcessId = 0,   // ProcessId：所属进程 PID。
        ProcessName,     // ProcessName：进程名。
        HandleValue,     // HandleValue：句柄值（十六进制）。
        TypeIndex,       // TypeIndex：对象类型索引。
        ObjectName,      // ObjectName：对象名称（可为空）。
        ObjectAddress,   // ObjectAddress：内核对象地址（十六进制）。
        GrantedAccess,   // GrantedAccess：访问掩码（十六进制）。
        Attributes,      // Attributes：句柄属性文本。
        HandleCount,     // HandleCount：对象当前 HandleCount。
        PointerCount,    // PointerCount：对象当前 PointerCount。
        Count            // Count：列总数。
    };

    // ObjectTypeTableColumn 作用：
    // - 统一定义对象类型页列索引；
    // - 与旧 KernelDock 对象类型列保持同语义。
    enum class ObjectTypeTableColumn : int
    {
        TypeIndex = 0,      // TypeIndex：类型编号。
        TypeName,           // TypeName：类型名。
        ObjectCount,        // ObjectCount：对象总数。
        HandleCount,        // HandleCount：句柄总数。
        AccessMask,         // AccessMask：访问掩码。
        SecurityRequired,   // SecurityRequired：是否需要安全检查。
        MaintainCount,      // MaintainCount：是否维护句柄计数。
        Count               // Count：列总数。
    };

    // HandleRow 作用：
    // - UI 层展示用的单行句柄记录；
    // - 字段全部为最终渲染态，避免主线程做二次转换。
    struct HandleRow
    {
        std::uint32_t processId = 0;       // processId：句柄所属进程 PID。
        QString processName;               // processName：句柄所属进程名。
        std::uint64_t handleValue = 0;     // handleValue：句柄数值。
        std::uint16_t typeIndex = 0;       // typeIndex：内核对象类型索引。
        QString typeName;                  // typeName：对象类型名。
        QString objectName;                // objectName：对象名称文本。
        std::uint64_t objectAddress = 0;   // objectAddress：对象地址。
        std::uint32_t grantedAccess = 0;   // grantedAccess：访问掩码。
        std::uint32_t attributes = 0;      // attributes：句柄属性位。
        std::uint32_t handleCount = 0;     // handleCount：对象句柄计数。
        std::uint32_t pointerCount = 0;    // pointerCount：对象指针计数。
    };

    // HandleRefreshOptions 作用：
    // - 把主线程当前过滤配置打包后传给后台线程；
    // - 避免后台线程直接访问 Qt 控件。
    struct HandleRefreshOptions
    {
        bool hasPidFilter = false;                 // hasPidFilter：是否启用 PID 过滤。
        std::uint32_t pidFilter = 0;               // pidFilter：目标 PID。
        QString keywordText;                       // keywordText：关键字（小写）。
        QString typeFilterText;                    // typeFilterText：类型过滤文本。
        bool onlyNamed = false;                    // onlyNamed：仅显示“有对象名”的句柄。
        bool resolveObjectName = true;             // resolveObjectName：是否尝试解析对象名。
        int nameResolveBudget = 300;               // nameResolveBudget：对象名解析预算数量。
        std::unordered_map<std::uint16_t, std::string> typeNameCacheByIndex; // typeNameCacheByIndex：上一轮类型缓存。
        std::unordered_map<std::uint16_t, std::string> typeNameMapFromObjectTab; // typeNameMapFromObjectTab：对象类型页产出的映射。
    };

    // HandleRefreshResult 作用：
    // - 后台线程返回给主线程的句柄刷新结果；
    // - 包含句柄行、类型列表与状态文案。
    struct HandleRefreshResult
    {
        std::vector<HandleRow> rows;               // rows：过滤后的句柄行结果。
        std::vector<QString> availableTypeList;    // availableTypeList：本轮可选类型列表。
        std::unordered_map<std::uint16_t, std::string> updatedTypeNameCacheByIndex; // updatedTypeNameCacheByIndex：更新后的类型缓存。
        std::size_t totalHandleCount = 0;          // totalHandleCount：系统总句柄数（枚举层）。
        std::size_t visibleHandleCount = 0;        // visibleHandleCount：过滤后可见句柄数。
        std::size_t resolvedNameCount = 0;         // resolvedNameCount：成功解析对象名数量。
        std::size_t objectTypeMappedCount = 0;     // objectTypeMappedCount：通过对象类型页映射命中的数量。
        std::uint64_t elapsedMs = 0;               // elapsedMs：后台耗时毫秒。
        QString diagnosticText;                    // diagnosticText：诊断信息（失败/降级/预算等）。
    };

    // ObjectTypeRefreshResult 作用：
    // - 后台线程返回给主线程的对象类型刷新结果；
    // - 包含对象类型行与 typeIndex 映射。
    struct ObjectTypeRefreshResult
    {
        std::vector<HandleObjectTypeEntry> rows;   // rows：对象类型行列表。
        std::unordered_map<std::uint16_t, std::string> typeNameMapByIndex; // typeNameMapByIndex：类型名映射。
        std::uint64_t elapsedMs = 0;               // elapsedMs：刷新耗时毫秒。
        QString diagnosticText;                    // diagnosticText：诊断文本。
    };

    // HandleDetailField 作用：
    // - 表示句柄详情面板中的一条键值记录；
    // - 供异步详情结果回填到详情树控件。
    struct HandleDetailField
    {
        QString keyText;     // keyText：字段名。
        QString valueText;   // valueText：字段值。
    };

    // HandleDetailRefreshResult 作用：
    // - 表示句柄详情后台查询结果；
    // - 包含通用字段、类型专用字段与诊断信息。
    struct HandleDetailRefreshResult
    {
        std::vector<HandleDetailField> fields; // fields：详情键值列表。
        QString diagnosticText;                // diagnosticText：诊断信息。
        std::uint64_t elapsedMs = 0;           // elapsedMs：详情查询耗时。
    };

private:
    // initializeUi 作用：
    // - 创建页面布局与多 Tab 容器；
    // - 构建句柄列表页与对象类型页。
    // 调用方法：构造函数内部调用一次。
    // 传入/传出：无。
    void initializeUi();

    // initializeHandleListTab 作用：
    // - 构建句柄列表 Tab 的工具栏、状态栏、表格；
    // - 绑定图标按钮与控件样式。
    // 调用方法：initializeUi 内部调用。
    // 传入/传出：无。
    void initializeHandleListTab();

    // initializeObjectTypeTab 作用：
    // - 构建对象类型 Tab（原 Kernel 对象类型视图）；
    // - 提供过滤、刷新、详情面板。
    // 调用方法：initializeUi 内部调用。
    // 传入/传出：无。
    void initializeObjectTypeTab();

    // initializeHandleTable 作用：
    // - 初始化句柄列表列、排序、选择行为；
    // - 统一列宽与右键策略。
    // 调用方法：initializeHandleListTab 内部调用。
    // 传入/传出：无。
    void initializeHandleTable();

    // initializeObjectTypeTable 作用：
    // - 初始化对象类型表列与交互行为；
    // - 与原 KernelDock 对象类型页保持一致列语义。
    // 调用方法：initializeObjectTypeTab 内部调用。
    // 传入/传出：无。
    void initializeObjectTypeTable();

    // initializeConnections 作用：
    // - 连接刷新按钮、筛选控件、右键菜单与详情联动；
    // - 把用户交互绑定到刷新管线。
    // 调用方法：构造函数内部调用一次。
    // 传入/传出：无。
    void initializeConnections();

    // requestAsyncRefresh 作用：
    // - 发起异步句柄刷新任务；
    // - 用 ticket 防止旧结果覆盖新结果。
    // 调用方法：按钮点击、筛选变化、首次显示时调用。
    // 传入 forceRefresh：true 允许强制拉起；false 遇到进行中任务则忽略。
    // 传出：无（结果通过 applyHandleRefreshResult 回填）。
    void requestAsyncRefresh(bool forceRefresh);

    // requestObjectTypeRefreshAsync 作用：
    // - 发起异步对象类型刷新任务；
    // - 成功后更新 typeIndex->typeName 映射供句柄枚举复用。
    // 调用方法：对象类型页刷新按钮、首次显示时调用。
    // 传入 forceRefresh：true 强制刷新；false 忽略并发。
    // 传出：无（结果通过 applyObjectTypeRefreshResult 回填）。
    void requestObjectTypeRefreshAsync(bool forceRefresh);

    // applyHandleRefreshResult 作用：
    // - 在主线程应用句柄刷新结果；
    // - 更新表格、类型下拉、状态文字与缓存。
    // 调用方法：后台任务完成后 invokeMethod 回调。
    // 传入 refreshTicket：任务序号；refreshResult：后台结果对象。
    // 传出：无。
    void applyHandleRefreshResult(std::uint64_t refreshTicket, const HandleRefreshResult& refreshResult);

    // applyObjectTypeRefreshResult 作用：
    // - 在主线程应用对象类型刷新结果；
    // - 更新对象类型表与类型映射缓存，并回刷句柄类型名。
    // 调用方法：对象类型后台任务完成后回调。
    // 传入 refreshTicket：任务序号；refreshResult：后台结果对象。
    // 传出：无。
    void applyObjectTypeRefreshResult(std::uint64_t refreshTicket, const ObjectTypeRefreshResult& refreshResult);

    // rebuildHandleTable 作用：
    // - 根据 m_rows 重建句柄列表表格；
    // - 同步每行 UserRole 元数据供右键动作使用。
    // 调用方法：applyHandleRefreshResult 内部调用。
    // 传入/传出：无。
    void rebuildHandleTable();

    // applyLocalHandleFilters 作用：
    // - 对当前完整句柄快照做本地过滤，不重新枚举系统句柄；
    // - 用于 PID/类型/关键字等轻量交互，避免反复重型刷新。
    // 调用方法：过滤条件变更、句柄刷新完成后调用。
    // 传入/传出：无。
    void applyLocalHandleFilters();

    // rebuildObjectTypeTable 作用：
    // - 根据 m_objectTypeRows 重建对象类型表；
    // - 支持按关键词过滤类型名与编号。
    // 调用方法：对象类型刷新完成、过滤框变更时调用。
    // 传入 filterKeyword：过滤关键字（可为空）。
    // 传出：无。
    void rebuildObjectTypeTable(const QString& filterKeyword);

    // collectHandleRefreshOptions 作用：
    // - 从句柄列表控件读取当前筛选配置并封装线程安全结构；
    // - 对 PID 文本做合法性校验。
    // 调用方法：requestAsyncRefresh 内部调用。
    // 传入/传出：无，返回 HandleRefreshOptions。
    HandleRefreshOptions collectHandleRefreshOptions() const;

    // updateTypeFilterItems 作用：
    // - 根据刷新结果重建“类型过滤”下拉项；
    // - 保留用户之前选择，避免刷新后重置。
    // 调用方法：applyHandleRefreshResult 内部调用。
    // 传入 availableTypeList：本轮类型列表。
    // 传出：无。
    void updateTypeFilterItems(const std::vector<QString>& availableTypeList);

    // refreshTypeFilterItemsFromAllRows 作用：
    // - 根据完整句柄快照重建类型过滤下拉项；
    // - 用于对象类型映射更新后同步过滤选项。
    // 调用方法：applyHandleRefreshResult / syncHandleTypeNamesFromObjectTypeMap 调用。
    // 传入/传出：无。
    void refreshTypeFilterItemsFromAllRows();

    // syncHandleTypeNamesFromObjectTypeMap 作用：
    // - 在对象类型页刷新完成后，把当前已缓存句柄行的类型名就地同步；
    // - 避免再次触发一次重型句柄枚举，从而减少 UI 卡顿。
    // 调用方法：applyObjectTypeRefreshResult 内部调用。
    // 传入/传出：无。
    void syncHandleTypeNamesFromObjectTypeMap();

    // requestHandleDetailRefresh 作用：
    // - 对当前选中句柄异步拉取详细信息；
    // - 详情包含通用字段与按类型分支的专用信息。
    // 调用方法：选中行切换、手动刷新详情时调用。
    // 传入 forceRefresh：true 强制刷新；false 遇到并发时忽略。
    // 传出：无。
    void requestHandleDetailRefresh(bool forceRefresh);

    // applyHandleDetailRefreshResult 作用：
    // - 在主线程应用句柄详情异步结果；
    // - 回填详情表和状态文本。
    // 调用方法：详情后台任务完成后回调。
    // 传入 refreshTicket：详情刷新序号；refreshResult：详情结果。
    // 传出：无。
    void applyHandleDetailRefreshResult(std::uint64_t refreshTicket, const HandleDetailRefreshResult& refreshResult);

    // updateHandleStatusLabel 作用：
    // - 统一更新句柄页状态标签文本与颜色；
    // - 在刷新中/完成态使用不同样式。
    // 调用方法：requestAsyncRefresh、applyHandleRefreshResult 调用。
    // 传入 statusText：显示文本；refreshing：是否处于刷新中。
    // 传出：无。
    void updateHandleStatusLabel(const QString& statusText, bool refreshing);

    // updateObjectTypeStatusLabel 作用：
    // - 更新对象类型页状态标签文本与颜色；
    // - 用于展示对象类型刷新状态。
    // 调用方法：requestObjectTypeRefreshAsync、applyObjectTypeRefreshResult 调用。
    // 传入 statusText：状态文本；refreshing：是否刷新中。
    // 传出：无。
    void updateObjectTypeStatusLabel(const QString& statusText, bool refreshing);

    // focusObjectTypeByIndex 作用：
    // - 切换到对象类型 Tab 并定位指定 typeIndex；
    // - 若未命中则仅设置过滤条件。
    // 调用方法：句柄列表右键“转到对象类型”调用。
    // 传入 typeIndex：目标类型索引。
    // 传出：无。
    void focusObjectTypeByIndex(std::uint16_t typeIndex);

    // showHandleTableContextMenu 作用：
    // - 在句柄表上弹出右键菜单；
    // - 提供复制、关闭句柄、转到对象类型等动作。
    // 调用方法：QTreeWidget::customContextMenuRequested 回调。
    // 传入 localPosition：表格局部坐标。
    // 传出：无。
    void showHandleTableContextMenu(const QPoint& localPosition);

    // showHandleHeaderContextMenu 作用：
    // - 在句柄表头弹出列管理菜单；
    // - 支持显示/隐藏列，补足基础列系统能力。
    // 调用方法：QHeaderView::customContextMenuRequested 回调。
    // 传入 localPosition：表头局部坐标。
    // 传出：无。
    void showHandleHeaderContextMenu(const QPoint& localPosition);

    // showObjectTypeDetailByCurrentRow 作用：
    // - 根据对象类型表当前行刷新详情文本；
    // - 复刻原 KernelDock 对象类型详情。
    // 调用方法：currentCellChanged 回调。
    // 传入/传出：无。
    void showObjectTypeDetailByCurrentRow();

    // showHandleDetailPlaceholder 作用：
    // - 在无选中句柄或详情未就绪时显示占位内容；
    // - 避免详情区域残留旧数据。
    // 调用方法：初始化、过滤后无选中项时调用。
    // 传入 messageText：占位说明文本。
    // 传出：无。
    void showHandleDetailPlaceholder(const QString& messageText);

    // selectedHandleRow 作用：
    // - 读取当前选中句柄行对应的缓存记录；
    // - 返回可修改指针供动作函数使用。
    // 调用方法：右键动作执行前调用。
    // 传入/传出：无，返回 HandleRow*（无选中时返回 nullptr）。
    HandleRow* selectedHandleRow();

    // copyCurrentHandleCell 作用：
    // - 复制当前句柄单元格文本到剪贴板；
    // - 用于快速审计字段值。
    // 调用方法：右键菜单“复制单元格”。
    // 传入/传出：无。
    void copyCurrentHandleCell();

    // copyCurrentHandleRow 作用：
    // - 复制当前句柄整行（TAB 分隔）到剪贴板；
    // - 便于粘贴到表格工具或日志。
    // 调用方法：右键菜单“复制整行”。
    // 传入/传出：无。
    void copyCurrentHandleRow();

    // closeCurrentHandle 作用：
    // - 对选中句柄执行 DuplicateHandle(DUPLICATE_CLOSE_SOURCE)；
    // - 成功后自动触发刷新。
    // 调用方法：右键菜单“关闭句柄”。
    // 传入/传出：无。
    void closeCurrentHandle();

    // closeSameTypeHandlesInCurrentProcess 作用：
    // - 关闭“同 PID + 同 TypeIndex”的一组句柄；
    // - 用于批量处置异常句柄，提升句柄管理能力。
    // 调用方法：右键菜单“批量关闭同类型句柄”。
    // 传入/传出：无。
    void closeSameTypeHandlesInCurrentProcess();

    // buildHandleRefreshResult 作用：
    // - 后台线程核心：枚举系统句柄并按条件筛选；
    // - 优先使用对象类型页映射生成类型名，再补充兜底查询。
    // 调用方法：requestAsyncRefresh 在后台线程中调用。
    // 传入 options：刷新配置。
    // 传出：HandleRefreshResult（按值返回）。
    static HandleRefreshResult buildHandleRefreshResult(const HandleRefreshOptions& options);

    // buildObjectTypeRefreshResult 作用：
    // - 后台线程核心：刷新对象类型快照并构建 typeIndex 映射；
    // - 供对象类型 Tab 和句柄列表共享。
    // 调用方法：requestObjectTypeRefreshAsync 在后台线程调用。
    // 传入：无。
    // 传出：ObjectTypeRefreshResult（按值返回）。
    static ObjectTypeRefreshResult buildObjectTypeRefreshResult();

    // closeRemoteHandle 作用：
    // - 封装关闭远程进程句柄的 Win32 操作；
    // - 返回布尔结果与详细错误文本。
    // 调用方法：closeCurrentHandle / closeSameTypeHandlesInCurrentProcess 调用。
    // 传入 processId：目标 PID；handleValue：目标句柄值。
    // 传出 detailTextOut：动作详情；返回 true/false。
    static bool closeRemoteHandle(
        std::uint32_t processId,
        std::uint64_t handleValue,
        std::string& detailTextOut);

    // buildHandleDetailRefreshResult 作用：
    // - 后台线程核心：按句柄类型生成专用详情；
    // - 支持 File/Key/Process/Thread/Token/Section/Event/Mutant 等类型的增强展示。
    // 调用方法：requestHandleDetailRefresh 在线程池中调用。
    // 传入 row：当前选中句柄行快照。
    // 传出：HandleDetailRefreshResult（按值返回）。
    static HandleDetailRefreshResult buildHandleDetailRefreshResult(const HandleRow& row);

    // formatHex 作用：统一把数值转为 0x 前缀十六进制文本。
    // 调用方法：表格渲染时调用。
    // 传入 value：64 位值；width：最小宽度（补零）。
    // 传出：QString 文本。
    static QString formatHex(std::uint64_t value, int width = 0);

    // formatTypeIndexDisplayText 作用：
    // - 把 TypeIndex 列格式化为“类型名 + 编号”可读文本；
    // - 已解析类型时显示如“Directory (50)”；未解析时仅显示编号。
    // 调用方法：句柄表渲染与类型名同步时调用。
    // 传入 typeIndex：类型索引；typeName：类型名称。
    // 传出：QString 文本。
    static QString formatTypeIndexDisplayText(std::uint16_t typeIndex, const QString& typeName);

    // formatHandleAttributes 作用：
    // - 把句柄属性位转换为可读文本（INHERIT/PROTECT/AUDIT）；
    // - 未命中时回退“None”。
    // 调用方法：表格渲染时调用。
    // 传入 attributes：原始属性位。
    // 传出：QString 文本。
    static QString formatHandleAttributes(std::uint32_t attributes);

    // resolveProcessIconByPid 作用：
    // - 根据 PID 解析进程图标并做缓存；
    // - 同一个 PID 只在首次渲染时解析一次，后续直接复用缓存图标。
    // 调用方法：重建句柄表时调用。
    // 传入 processId：目标进程 PID。
    // 传出：QIcon 进程图标；失败时回退默认图标。
    QIcon resolveProcessIconByPid(std::uint32_t processId);

    // queryProcessImagePathCached 作用：
    // - 查询并缓存 PID 对应的进程路径；
    // - 避免同一 PID 反复走路径查询。
    // 调用方法：resolveProcessIconByPid 内部调用。
    // 传入 processId：目标进程 PID。
    // 传出：QString 进程路径；失败时返回空字符串。
    QString queryProcessImagePathCached(std::uint32_t processId);

    // decodeGrantedAccessText 作用：
    // - 按对象类型把 GrantedAccess 位掩码翻译成语义权限文本；
    // - 用于列表 tooltip 与详情面板增强可读性。
    // 调用方法：句柄表渲染与详情构建时调用。
    // 传入 typeName：对象类型名；grantedAccess：访问掩码。
    // 传出：QString 语义权限文本。
    static QString decodeGrantedAccessText(const QString& typeName, std::uint32_t grantedAccess);

private:
    QVBoxLayout* m_rootLayout = nullptr;         // m_rootLayout：页面根布局。
    QTabWidget* m_tabWidget = nullptr;           // m_tabWidget：句柄模块 Tab 容器。

    QWidget* m_handleListPage = nullptr;         // m_handleListPage：句柄列表页容器。
    QVBoxLayout* m_handleListLayout = nullptr;   // m_handleListLayout：句柄列表页布局。
    QHBoxLayout* m_toolbarLayout = nullptr;      // m_toolbarLayout：句柄列表顶部工具栏布局。
    QPushButton* m_refreshButton = nullptr;      // m_refreshButton：句柄刷新按钮（图标化）。
    QLineEdit* m_pidFilterEdit = nullptr;        // m_pidFilterEdit：PID 过滤输入框。
    QLineEdit* m_keywordFilterEdit = nullptr;    // m_keywordFilterEdit：关键字过滤输入框。
    QComboBox* m_typeFilterCombo = nullptr;      // m_typeFilterCombo：对象类型过滤下拉。
    QCheckBox* m_onlyNamedCheckBox = nullptr;    // m_onlyNamedCheckBox：仅显示有对象名句柄。
    QCheckBox* m_resolveNameCheckBox = nullptr;  // m_resolveNameCheckBox：是否开启对象名解析。
    QSpinBox* m_nameBudgetSpinBox = nullptr;     // m_nameBudgetSpinBox：对象名解析预算。
    QLabel* m_statusLabel = nullptr;             // m_statusLabel：句柄刷新状态文本。
    QTreeWidget* m_tableWidget = nullptr;        // m_tableWidget：句柄列表表格。
    QLabel* m_handleDetailStatusLabel = nullptr; // m_handleDetailStatusLabel：句柄详情状态文本。
    QTreeWidget* m_handleDetailTable = nullptr;  // m_handleDetailTable：句柄详情键值表。

    QWidget* m_objectTypePage = nullptr;         // m_objectTypePage：对象类型页容器。
    QVBoxLayout* m_objectTypeLayout = nullptr;   // m_objectTypeLayout：对象类型页布局。
    QHBoxLayout* m_objectTypeToolLayout = nullptr; // m_objectTypeToolLayout：对象类型工具栏布局。
    QPushButton* m_refreshObjectTypeButton = nullptr; // m_refreshObjectTypeButton：对象类型刷新按钮。
    QLineEdit* m_objectTypeFilterEdit = nullptr; // m_objectTypeFilterEdit：对象类型过滤输入框。
    QLabel* m_objectTypeStatusLabel = nullptr;   // m_objectTypeStatusLabel：对象类型状态文本。
    QTreeWidget* m_objectTypeTable = nullptr;    // m_objectTypeTable：对象类型列表表格。
    QTreeWidget* m_objectTypeDetailTable = nullptr; // m_objectTypeDetailTable：对象类型详情（键值对表）。

    bool m_refreshInProgress = false;            // m_refreshInProgress：句柄刷新互斥标记。
    bool m_objectTypeRefreshInProgress = false;  // m_objectTypeRefreshInProgress：对象类型刷新互斥标记。
    bool m_handleDetailRefreshInProgress = false; // m_handleDetailRefreshInProgress：句柄详情刷新互斥标记。
    bool m_initialRefreshDone = false;           // m_initialRefreshDone：首轮刷新是否已完成。
    std::uint64_t m_refreshTicket = 0;           // m_refreshTicket：句柄刷新序号，防止乱序覆盖。
    std::uint64_t m_objectTypeRefreshTicket = 0; // m_objectTypeRefreshTicket：对象类型刷新序号。
    std::uint64_t m_handleDetailRefreshTicket = 0; // m_handleDetailRefreshTicket：句柄详情刷新序号。
    int m_refreshProgressPid = 0;                // m_refreshProgressPid：句柄刷新 kPro 任务 PID。
    int m_objectTypeRefreshProgressPid = 0;      // m_objectTypeRefreshProgressPid：对象类型刷新 kPro 任务 PID。
    int m_handleDetailRefreshProgressPid = 0;    // m_handleDetailRefreshProgressPid：句柄详情刷新 kPro 任务 PID。

    std::vector<HandleRow> m_allRows;            // m_allRows：完整句柄快照缓存。
    std::vector<HandleRow> m_rows;               // m_rows：当前过滤后的句柄列表缓存。
    std::vector<HandleObjectTypeEntry> m_objectTypeRows; // m_objectTypeRows：对象类型行缓存。
    std::unordered_map<std::uint16_t, std::string> m_typeNameCacheByIndex; // m_typeNameCacheByIndex：句柄刷新阶段生成的类型缓存。
    std::unordered_map<std::uint16_t, std::string> m_typeNameMapByIndexFromObjectTab; // m_typeNameMapByIndexFromObjectTab：对象类型页映射缓存。
    QHash<quint32, QIcon> m_processIconCacheByPid; // m_processIconCacheByPid：PID -> 图标缓存。
    QHash<quint32, QString> m_processImagePathCacheByPid; // m_processImagePathCacheByPid：PID -> 路径缓存。
};
