#pragma once

// LogDockWidget 是“日志输出”Dock 中的可视化日志面板。
// 该控件负责：
// 1) 展示日志表格（等级/时间/内容/文件/函数）；
// 2) 提供等级筛选、事件追踪、复制与导出能力；
// 3) 从全局日志管理器 KswordARKEventEntry 周期刷新数据。

#include "../Framework.h"

#include <QColor>
#include <QIcon>
#include <QPoint>
#include <QString>
#include <QWidget>

// 前置声明：减少头文件依赖体积，提升编译速度。
class QCheckBox;
class QHBoxLayout;
class QPushButton;
class QTableWidget;
class QTimer;
class QVBoxLayout;

class LogDockWidget final : public QWidget
{
public:
    // 构造函数作用：
    // - 初始化全部 UI 控件；
    // - 建立信号连接；
    // - 启动定时刷新。
    // 参数 parent：Qt 父对象指针。
    explicit LogDockWidget(QWidget* parent = nullptr);

private:
    // initializeUi 作用：
    // - 创建布局、筛选区、按钮区、日志表格；
    // - 设置不可编辑与列宽策略。
    void initializeUi();

    // initializeConnections 作用：
    // - 连接复选框、按钮、右键菜单、定时器等交互逻辑。
    void initializeConnections();

    // initializeRefreshTimer 作用：
    // - 创建并启动刷新定时器；
    // - 使用 Revision() 做轻量级变更检测。
    void initializeRefreshTimer();

    // refreshTableFromManager 作用：
    // - 从日志管理器读取快照；
    // - 应用等级筛选/追踪筛选；
    // - 重建表格并在需要时滚动到底部。
    // 参数 forceRefresh：
    // - true  强制刷新（用于用户主动操作）；
    // - false 仅在 revision 变化时刷新（用于定时器）。
    void refreshTableFromManager(bool forceRefresh);

    // rebuildTable 作用：
    // - 用 filteredEvents 重建全部表格行；
    // - 应用行样式（Error/Fatal 整行着色）。
    // 参数 filteredEvents：已筛选后的可见日志集合。
    void rebuildTable(const std::vector<kEvent>& filteredEvents);

    // applyDetailColumnVisibility 作用：
    // - 根据“详细信息”复选框状态切换文件列与函数列；
    // - 让日志表格在简洁模式和详细模式之间切换。
    void applyDetailColumnVisibility();

    // applyRowStyle 作用：
    // - 按日志等级为一行设置背景色与前景色。
    // 参数 row：目标行；logItem：该行对应日志对象。
    void applyRowStyle(int row, const kEvent& logItem);

    // makeLevelSquareIcon 作用：
    // - 生成一个纯色小方块图标，放在“等级”列。
    // 参数 color：方块颜色。
    // 返回值：生成后的 QIcon。
    QIcon makeLevelSquareIcon(const QColor& color) const;

    // getLevelColor 作用：返回等级对应颜色。
    QColor getLevelColor(kLogLevel level) const;

    // getLevelText 作用：返回等级字符串（DEBUG/INFO/...）。
    QString getLevelText(kLogLevel level) const;

    // isLevelEnabledByCheckbox 作用：
    // - 根据复选框状态判断某等级是否应展示。
    bool isLevelEnabledByCheckbox(kLogLevel level) const;

    // showTableContextMenu 作用：
    // - 在表格单元格右键时弹出菜单；
    // - 提供复制单元格/复制行/跟踪或取消追踪。
    // 参数 position：右键点在表格视口坐标系中的位置。
    void showTableContextMenu(const QPoint& position);

    // copySingleCell 作用：复制指定单元格文本到剪贴板。
    // 参数 row/column：目标行列索引。
    void copySingleCell(int row, int column);

    // copySingleRow 作用：复制指定整行（制表符分隔）到剪贴板。
    // 参数 row：目标行索引。
    void copySingleRow(int row);

    // copyVisibleRows 作用：
    // - 把当前表格可见内容全部复制到剪贴板；
    // - 适配事件追踪过滤后的可见子集。
    void copyVisibleRows();

    // buildVisibleRowText 作用：
    // - 按当前可见列拼接一行日志文本；
    // - 供“复制行/复制可见”复用，确保复制结果与界面一致。
    QString buildVisibleRowText(const kEvent& logItem) const;

    // startTrackingByRow 作用：
    // - 读取某行的 GUID；
    // - 进入“仅显示同 GUID 日志”模式。
    // 参数 row：触发追踪的行索引。
    void startTrackingByRow(int row);

    // cancelTracking 作用：退出事件追踪模式并恢复常规过滤。
    void cancelTracking();

    // exportAllLogs 作用：
    // - 弹出保存对话框；
    // - 调用 KswordARKEventEntry.Save 导出全部日志。
    void exportAllLogs();

    // chooseExportPath 作用：
    // - Windows 上优先调用原生 shell 保存对话框；
    // - shell 调用失败时回退为手动输入路径。
    // 返回值：
    // - 非空：用户确认后的目标路径；
    // - 空串：用户取消。
    QString chooseExportPath();

    // clearAllLogsWithDoubleConfirm 作用：
    // - 两次确认后清空日志管理器；
    // - 刷新表格。
    void clearAllLogsWithDoubleConfirm();

private:
    // ======== 主布局与控件 ========
    QVBoxLayout* m_rootLayout = nullptr;      // 根布局：纵向堆叠“按钮条/筛选条/表格”。
    QHBoxLayout* m_filterLayout = nullptr;    // 筛选布局：五个等级复选框。
    QHBoxLayout* m_actionLayout = nullptr;    // 动作布局：图标按钮 + 详细信息/自动滚动。

    QCheckBox* m_debugCheck = nullptr;        // Debug 级别显示开关。
    QCheckBox* m_infoCheck = nullptr;         // Info 级别显示开关。
    QCheckBox* m_warnCheck = nullptr;         // Warn 级别显示开关。
    QCheckBox* m_errorCheck = nullptr;        // Error 级别显示开关。
    QCheckBox* m_fatalCheck = nullptr;        // Fatal 级别显示开关。
    QCheckBox* m_detailCheck = nullptr;       // “详细信息”开关：控制文件/函数列显示。
    QCheckBox* m_autoScrollCheck = nullptr;   // “保持滚动到最底端”开关。

    QPushButton* m_exportButton = nullptr;       // 导出日志按钮。
    QPushButton* m_clearButton = nullptr;        // 清空日志按钮。
    QPushButton* m_copyVisibleButton = nullptr;  // 复制可见按钮。

    QTableWidget* m_logTable = nullptr;       // 核心日志表格控件。
    QTimer* m_refreshTimer = nullptr;         // 定时刷新器。

    // ======== 刷新与过滤状态 ========
    std::size_t m_lastRevision = 0;           // 上次渲染时的日志 revision。
    std::vector<kEvent> m_visibleEvents;      // 当前可见事件集合（行号与该数组一一对应）。

    bool m_isTracking = false;                // 当前是否处于“按 GUID 追踪”模式。
    GUID m_trackingGuid{};                    // 追踪模式下使用的目标 GUID。
};
