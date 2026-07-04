#pragma once

class QApplication;
class QAbstractItemView;

namespace ks::ui
{
    // InstallGlobalTableColumnAutoFit 作用：
    // - 给 QApplication 安装一次全局表格列宽自适应过滤器；
    // - 过滤器只处理 QTableView/QTableWidget/QTreeView/QTreeWidget 这类带横向表头的视图；
    // - 初始显示、布局变化、视口尺寸变化时先按表头/抽样内容估算首选宽度，再把总宽度压入当前视口；
    // - 同时监听表格 viewport 的真实 Resize 事件，避免 Dock/Tab/Splitter 首次布局后表格仍按临时宽度显示；
    // - setCellWidget/indexWidget 控件会参与最小列宽计算，复选框、下拉框、按钮不会被默认压缩错位；
    // - 短文本列保持紧凑，长文本列获得更多剩余空间，尽量避免默认出现横向滚动条；
    // - 不修改横向/纵向滚动条策略，用户手动拖宽列后仍可自然出现横向滚动条。
    // 参数 appInstance：当前 QApplication 实例；为空时忽略。
    // 返回值：无。重复调用会被 QApplication 属性去重。
    void InstallGlobalTableColumnAutoFit(QApplication* appInstance);

    // RequestTableColumnAutoFit 作用：
    // - 对单个 QTableView/QTableWidget/QTreeView/QTreeWidget 请求一次列宽自适应；
    // - 内部复用全局内容感知自适应逻辑，默认把可见列压入当前 viewport；
    // - 请求只会合并到队列尾部执行一次，不使用多轮定时修正；
    // - 若该表格已经被用户手动拖动过列宽，则保留用户宽度并跳过本次请求。
    // 参数 view：目标表格/树表视图；为空或不支持时忽略。
    // 返回值：无。不会修改横向或纵向滚动条策略。
    void RequestTableColumnAutoFit(QAbstractItemView* view);

    // SetTableColumnAutoFitEnabled 作用：
    // - 对单个 QTableView/QTreeView 控件启用或关闭全局列宽自适应；
    // - 输入 view：目标表格/树表视图；enabled 为 true 时允许全局 fit，false 时完全跳过；
    // - 处理：关闭时会清理待执行请求，避免文件管理器这类自管列宽的视图被全局逻辑反向撑动布局；
    // - 返回值：无。无效或不支持的 view 会被忽略。
    void SetTableColumnAutoFitEnabled(QAbstractItemView* view, bool enabled);
}
