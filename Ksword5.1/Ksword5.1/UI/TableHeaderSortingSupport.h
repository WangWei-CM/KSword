#pragma once

class QTableWidget;
class QTableView;

namespace ks::ui
{
    // InstallTableHeaderClickSorting 作用：
    // - 为未开启 Qt 持续排序的 QTableWidget 安装表头点击排序；
    // - 每次点击只执行一次整表排序，不改变 sortingEnabled，避免后续逐格填表时行被提前搬动；
    // - 已开启原生排序的表格继续由 Qt 处理，重复调用不会重复连接信号。
    // 调用方法：通用表格配置完成、内部 model 已存在后传入表格指针。
    // 传入 tableWidget：需要接入表头排序的表格；传出：无。
    void InstallTableHeaderClickSorting(QTableWidget* tableWidget);

    // SetTableHeaderClickSortingEnabled 作用：
    // - 控制通用的一次性表头排序是否可用；
    // - 保留调用栈、采集顺序等具有固定行序语义的表格应传 false；
    // - 该开关不修改业务代码显式设置的 sortingEnabled 状态。
    // 传入 tableWidget：目标表格；enabled：true 允许点击排序，false 保留固定行序。
    // 传出：无。
    void SetTableHeaderClickSortingEnabled(
        QTableWidget* tableWidget,
        bool enabled);
    // Model views retain their existing model/proxy and selection model.
    // Configure the model comparator and disable dynamic sorting as appropriate.
    // Install after setModel; repeated install and later model/header replacement are safe.
    void InstallTableHeaderClickSorting(QTableView* tableView);
    void SetTableHeaderClickSortingEnabled(QTableView* tableView, bool enabled);
}
