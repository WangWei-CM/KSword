# Taskbar 页面平铺导航

- 入口在独立 `Taskbar` 右侧网格按钮，不在 KSword 主窗口标题栏。`PageOverviewWindow` 是置顶无边框透明工具窗，一级 Dock 各占一列、二三级页缩进，支持名称/完整路径搜索、Enter 跳首项、Esc 收起、拖动和缩放。RGB 只用于列顶和交互面；可见时更新并遵守系统动画开关。底板半透明，文字保持完整不透明度。
- 真实目录由主程序 `UI/PageNavigationBridge` 读取，不在 Taskbar 硬编码菜单。目录在一级 Dock 框架准备好后发布，逐 Dock 初始化并分片发送；不遍历表格单元格、不切换 Tab 以采集。QTabWidget 的显式隐藏项和禁用态保留，内存工作台的五个自绘分段内容页通过 `ksword_navigation_page_titles` 登记。
- R3 协议在 `shared/usermode/KswordPageNavigation.h`，使用有长度上限的 UTF-8 JSON / WM_COPYDATA。双方校验窗口所属 exe 位于同一发行目录，请求令牌和目录代次隔离旧响应；Taskbar 只请求目录或服务端已登记的页面 ID，不传控件指针/函数地址/命令。实际路由保存 QPointer，激活时按外到内恢复 Tab 链并复核可见和启用状态，过期页面要求刷新目录。
- Taskbar 未找到新版 KSword 时显示显式启动入口；运行时点开页面会收起总览并把焦点交给 KSword。主程序和 Taskbar 都要重新构建至统一 `Ksword5.1/x64/Release`，使用者需运行匹配版本。GUI 与跳转效果由用户验收，不能把编译成功当成运行验收。
