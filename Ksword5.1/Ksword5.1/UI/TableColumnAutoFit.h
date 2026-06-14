#pragma once

class QApplication;
class QAbstractItemView;

namespace ks::ui
{
    // InstallGlobalTableColumnAutoFit 作用：
    // - 给 QApplication 安装一次全局表格列宽自适应过滤器；
    // - 过滤器只处理 QTableView/QTableWidget/QTreeView/QTreeWidget 这类带横向表头的视图；
    // - 初始显示、布局变化、视口尺寸变化时先按表头/抽样内容估算首选宽度，再把总宽度压入当前视口；
    // - 短文本列保持紧凑，长文本列获得更多剩余空间，尽量避免默认出现横向滚动条；
    // - 不修改横向/纵向滚动条策略，用户手动拖宽列后仍可自然出现横向滚动条。
    // 参数 appInstance：当前 QApplication 实例；为空时忽略。
    // 返回值：无。重复调用会被 QApplication 属性去重。
    void InstallGlobalTableColumnAutoFit(QApplication* appInstance);

    // RequestTableColumnAutoFit 作用：
    // - 对单个 QTableView/QTableWidget/QTreeView/QTreeWidget 请求一次列宽自适应；
    // - 内部复用全局内容感知自适应逻辑，默认把可见列压入当前 viewport；
    // - 若该表格已经被用户手动拖动过列宽，则保留用户宽度并跳过本次请求。
    // 参数 view：目标表格/树表视图；为空或不支持时忽略。
    // 返回值：无。不会修改横向或纵向滚动条策略。
    void RequestTableColumnAutoFit(QAbstractItemView* view);
}
