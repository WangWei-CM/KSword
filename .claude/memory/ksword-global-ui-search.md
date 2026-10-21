# KSword 标题栏全局搜索 / 双模式与四节点循环输入

主程序标题栏中间输入框是“搜索 / CMD”双模式（默认搜索），实现分三层：

- `Framework/CustomTitleBar`：输入组 `ksTitleInputGroup`（QToolButton 模式按钮 + QLineEdit 一体外观）。
  模式按钮 InstantPopup 菜单切换；搜索模式发 `searchTextEdited`，CMD 模式回车发 `commandSubmitted`
  （cmd /K 新控制台，MainWindow::executeCommandInNewConsole）。模式切换发 `inputModeChanged`。
  顶部输入框的普通 Tab/Shift+Tab 按 Global ↔ CurrentPage ↔ CurrentTable ↔ CMD 四节点循环；
  范围边界由 `requestCommandInputActivation -> activateCommandInput` 切 CMD，CMD 返回搜索仍走
  `requestSearchInputActivation -> activateSearchInput`。复用 `inputModeChanged` 同步搜索/CMD 配置弹层。
  CMD 的 Enter/Esc 透传给 `CommandExecutionPopup`；popup 内字段与 Ctrl/Alt/Meta+Tab 不被四节点逻辑接管。
  返回搜索要同步当前文本，包括空串，否则会恢复 CMD 模式前的旧查询。
- `UI/GlobalUiSearch`（ks::ui::GlobalUiSearchController，Q_OBJECT/QtMoc）：防抖 220ms 后**异步分片**扫描——
  控件树只能在 UI 线程碰，所以按“每个事件循环周期扫一个 Dock”切片（singleShot(0) 链），
  分片间让出事件循环保持 UI 响应；`m_searchGeneration` 代数自增实现取消（新输入/Esc/切模式/收起弹层
  都会作废在途分片）。扫描中弹层显示进度行（ksGlobalUiSearchProgressRow：QLabel“正在搜索：%1（%2/%3）”
  + 6px QProgressBar，chunk 用 PrimaryAccent），完成后 stable_sort 命中并切回结果列表。
  索引 QLabel/按钮/GroupBox 标题/QTabWidget 页签/下拉项/占位符/表头和表格可见列单元格。
  进度模板文本用 ks::i18n::sourceText 先翻模板再 .arg 拼接（整串运行时组合查不到词条）。
  结果弹层是 MainWindow 子控件（非顶层窗口，避免无边框窗口焦点问题），每条显示“匹配文本 + 页面路径
  （Dock › 内部页签 › 分组框，› 分隔）”。激活结果：ensureDockContentInitialized → withTemporaryNonTopMost
  raise → 逐层 QTabWidget/QStackedWidget setCurrent + QScrollArea ensureWidgetVisible → 140ms 后
  SearchHitFlashOverlay（目标控件子 overlay，主题强调色脉冲 3 次自毁）。
- MainWindow：`initGlobalUiSearchController`（initCustomTitleBar 末尾）注入 dock 列表
  （collectSearchableDockWidgets：18 主 Dock + 日志/监视/任务辅助 Dock）与激活回调
  （activateDockForSearchNavigation：isClosed 先 toggleView(true) 再 raise）。

通用表格搜索入口只保留图标按钮；空间足够时显示，点击后切换到“当前表格”范围并激活标题栏搜索框。
命中仍展示在搜索结果弹层中；默认不改变行可见状态。开启“仅显示搜索结果”后会附加通用行过滤，切离搜索、清空查询或撤销过滤时恢复此前的隐藏行快照，不能把原本隐藏的行一并显示。

可揭示性判定：向上遍历到 CDockWidget，途中显式隐藏且父不是 QStackedWidget 的控件视为不可揭示（不收录）。
非当前 Tab 页被 QStackedWidget 显式 hide 属于导航性隐藏，必须放行，否则搜不到其他页签内容。

## i18n 审计的隐藏约定（tools/i18n_language_pack.py audit）

- setObjectName 的 `ksXxxYyy` 标识符、`#include "无路径分隔的头文件名.h"` 都会被提取，
  必须在两个语言包 source_translations 里加“恒等词条”（"ksTitleInputGroup": "ksTitleInputGroup"）。
- 组合文本按整串查词条：按钮文本 "搜索 ▾"/"CMD ▾" 是完整键，不能只登记 "搜索"。
- 审计器会同时提取 C++ 相邻字符串字面量的编译后完整值；拆开的每段都有词条但完整值缺失时，`audit` 必须失败。
- `text/contextText/translated/bind*` 等调用点引用的语义键必须真实存在于两份语言包；审计器会从源码反向校验，不能再依赖中文 fallback 蒙混通过。
- LanguageManager 的非中文兼容翻译按实际对象接入：普通 UpdateRequest/LayoutRequest 不再调度子树；Paint/help 与普通 Show 只处理接收对象及明确所属控件。Qt 6.9.3 的 `show_helper/showChildren` 向可见子控件先分别发送 Show，再发父 Show，不能在每个 Show 重扫其后代。QAction 没有 Show，须在暴露它的 QWidget 首显时补处理实际 actions，并保留其 managed/preserve 边界。
- 动态 setText/setPlaceholderText 仍经绘制前兼容翻译；模型保留原通知/root 代次。ChildAdded/Polish、显式 dirty 和受管 dirty 的首帧准备仍有效；局部 Paint 不得清掉已排队的子树 dirty。真正换语言前必须用旧语言包完整捕获隐藏控件规范源文，再广播 LanguageChange，不能只为减少遍历删掉此步骤。上述性能改动完成源码/QtCore 检查，实际主程序首帧、菜单与语言组合仍须用户 GUI 验收。
- source template 的 `%1` 等捕获值会再做一次源文本翻译，使外层英文模板不会夹带“安全模式”等稳定中文枚举；动态多行报告仍应逐行翻译后再 join，不能对最终整块只调用一次 sourceText。
- 语言包两文件键序逐行对齐（同键同行号），插入词条必须两包同锚点成对插入；
  用“整行内容锚点 + 行插入”脚本定点编辑，禁止 json.load/dump 重写。

## 四节点循环的实际回归入口（2026-10-04）

`tools/Invoke-SearchHistoryUiTests.ps1` 在既有构建日志目录编译真实 CustomTitleBar、GlobalUiSearch、TableSearchSupport、CommandExecutionPopup 和 LanguageManager，CMD执行请求只接记录槽。当前128条离屏断言覆盖双向四节点、菜单/修饰键/弹层字段Tab、Enter/Esc、真实防抖后过滤快照恢复、空查询和未提交分片的取消；不运行真实命令或驱动。

修饰键Tab按Qt正常焦点顺序移动时，当前表格上下文和名称可合法变化；范围不变应断言popup QTabBar索引，不能冻结包含动态表名的searchScopeDisplayText。切回搜索须同步空字符串，否则旧pending query可能重新启动；取消必须同时停止debounce、增加扫描代数并恢复原隐藏行快照。

## 当前页面搜索的惰性范围（2026-10-21）

- 当前页只搜索实际可见分支，不应为它构造所有隐藏的Memory/Misc子页。NestedDockPreparer传实际UiSearchScope，MainWindow将CurrentPage映射到prepareNavigationPages(true)，只保证当前根及其工作台/调试视图已准备。此路径不置全局prepared，之后全范围搜索仍完整准备全部原分支；Taskbar兼容路由及旧调用默认false。
- 现有DockSearchCursor仍按64节点/4ms目标和完整表格游标恢复，保留当前可见性、字段、索引和查询代次；本步未改匹配/排序/过滤/高亮导航。全范围冷正文仍缺完整值索引，不能据此将searchComplete置true或关闭GN-009/FP-003。
- 133项QtCore检查抽取真实Memory/Misc/MainWindow方法、原MainWindow搜索lambda与冻结旧路径；验证全范围调用等价、当前页0个无关工厂及构造中销毁/换页保护。实际产品GUI、语言/主题与可见正文仍由用户验收；未运行窗口、业务或驱动。
