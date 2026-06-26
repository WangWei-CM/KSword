# Win32k GUI R0 只读审计前期准备

## 0. 目标、边界和当前依据

本文面向下一阶段 `win32k.sys` / `win32kbase.sys` / `win32kfull.sys` 基于 PDB 的 R0 GUI 审计能力准备。目标是先把可落地、可验收、只读的审计面拆清楚，不在本阶段设计默认拦截、修复、写内存或 Hook win32k 路径。

当前本地依据：

- PDB 缓存根：`E:\KswordPDB\PDB\pdb-cache\amd64`，已存在 `win32k.pdb`、`win32kbase.pdb`、`win32kfull.pdb`，且三个目录下均已有多个 GUID 子目录。可作为离线 profile 生成资产。
- 现有轻量窗口页：`KswordARKLight\Features\Window\` 当前基于 R3 Win32 API 枚举窗口，核心 API 包括 `EnumWindows`、`GetWindowInfo`、`GetWindowThreadProcessId`、`GetClassNameW`、`GetWindowTextW`、`IsWindowVisible`、`IsWindowEnabled`、`IsIconic`、`IsZoomed`、`IsWindowUnicode`。
- Qt 主项目 `Ksword5.1\Ksword5.1\WindowDock\WindowDock.cpp` 当前只是保留编译入口，实际窗口管理逻辑仍在旧的 `OtherDock` 路径。
- 现有 R0 键盘/Hook 协议：`shared\driver\KswordArkKeyboardIoctl.h` 已定义 `IOCTL_KSWORD_ARK_ENUM_KEYBOARD_HOTKEYS`、`IOCTL_KSWORD_ARK_ENUM_KEYBOARD_HOOKS`，并已有 hotkey/hook 返回字段。
- 现有 R0 实现：`KswordARKDriver\src\features\keyboard\keyboard_query.c` 已读取 `win32kfull.sys` / `win32kbase.sys`，解析 `UserGetSiloGlobals`，通过 `EditionIsHotKey` 附近模式定位热键布局，并使用一批硬编码 hook 偏移遍历 `WH_KEYBOARD` / `WH_KEYBOARD_LL` 链。
- `docs\OpenArk功能对照与TODO.md` 明确把 `System Hotkey` 作为未覆盖项，把 GUI/窗口方向视为可补齐 ARK 核心能力。

设计原则：

1. **PDB profile first**：R3/offline 先按 PDB GUID+Age 生成 profile，R0 只消费已匹配的模块身份和字段偏移，不在 R0 解析 PDB。
2. **只读、限量、失败关闭**：所有 R0 枚举都要有最大条数、最大链深、状态码、部分成功语义；偏移缺失时返回 unsupported/partial，不猜偏移。
3. **跨视图优先**：R0 读取 win32k 内部对象，R3 同时用公开 Win32 API 枚举，对比差异后给出风险提示，避免单源误判。
4. **Session 感知**：GUI 对象有 session/desktop/thread 上下文，R0 读取时必须明确 session 来源，必要时通过目标进程/线程进入对应 session 视图；不做跨 session 盲扫。
5. **先审计，后扩展**：第一阶段不做默认消息拦截、不改 hook 链、不改 hotkey、不改窗口状态。

---

## 1. PDB profile 资产与适配建议

### 1.1 需要的 PDB 模块

| 模块 | 用途 | 优先级 |
|---|---|---|
| `win32kbase.pdb` | session/silo globals、WindowStation/Desktop 基础结构、剪贴板/输入队列相关基础字段候选。 | P0 |
| `win32kfull.pdb` | `tagWND`、`tagTHREADINFO`、`tagQ`、`tagHOOK`、hotkey、窗口消息/用户对象实现细节候选。 | P0 |
| `win32k.pdb` | 兼容旧路径、导出/转发关系、部分全局或壳层入口核对。 | P1 |
| `ntkrnlmp.pdb` / `ntkrla57.pdb` | `ETHREAD` 到 Win32Thread、进程/线程/session 关联、模块身份校验。 | P0 |
| `kbdclass.pdb` / `kbdhid.pdb` / `mouclass.pdb` / `mouhid.pdb` | 后续输入设备链审计，不是 win32k GUI MVP 必需。 | P2 |

### 1.2 推荐实现方式

- 离线工具或 R3 准备步骤读取 `E:\KswordPDB\PDB\pdb-cache\amd64\*.pdb\<guid>\*.pdb`，生成 JSON/二进制 profile。
- Profile 以 `moduleName + pdbGuid + pdbAge + imageSize + timeDateStamp` 为 key，字段为结构大小、成员偏移、关键全局 RVA、关键函数 RVA。
- R0 启动或 IOCTL 前只做模块身份匹配：已加载 `win32kbase.sys` / `win32kfull.sys` 的 PE 信息必须命中 profile；不命中则返回 unsupported。
- 结构/全局名必须以 PDB 实测为准。下文使用的 `tagWINDOWSTATION`、`tagDESKTOP`、`tagWND`、`tagTHREADINFO`、`tagQ`、`tagHOOK` 等为候选名称，不允许硬编码假定所有 build 一致。

### 1.3 现有实现需要替换的硬编码点

当前 hotkey/hook 代码已有只读框架，但还不是完整 PDB 适配：

- 热键枚举：已通过 `UserGetSiloGlobals` + `EditionIsHotKey` 附近模式推导 hotkey table/layout。下一步应让 PDB profile 直接给出 hotkey 表/对象字段偏移，模式扫描只作为降级诊断，不作为主路径。
- Hook 枚举：当前 `threadHookArrayOffset=0x3C0`、`desktopInfoOffset=0x1F8`、`desktopHookArrayOffset=0x28`、`tagHOOK` 多个字段偏移为硬编码。下一步必须由 `tagTHREADINFO` / desktop info / `tagHOOK` PDB 成员偏移替换。

---

## 2. 可落地只读审计功能清单

### 2.1 功能矩阵

| 功能 | 优先级 | 需要 PDB 模块 | 可能结构/全局/链表 | R0 输出字段 | R3/UI 展示字段 | Cross-view 对比 API | 数据来源 | 风险和降级 | 验收方式 |
|---|---|---|---|---|---|---|---|---|---|
| Session 枚举与当前 GUI session 识别 | P0 | `ntkrnlmp.pdb`/`ntkrla57.pdb`、`win32kbase.pdb` | `EPROCESS.Session`、`ETHREAD`、win32k session/silo globals、`UserGetSiloGlobals` 返回对象 | sessionId、代表 PID/TID、win32kbase/win32kfull base、sessionGlobals、profileId、状态码 | Session、进程数、线程数、GUI 对象是否可读、profile 命中状态 | `ProcessIdToSessionId`、`WTSEnumerateSessionsW` | session space 读取失败时只返回 sessionId 和 unsupported；不跨 session 盲扫 | 在 Win10/Win11 至少 2 个 build 上，R0 sessionId 与 R3 API 一致；profile 不匹配时返回 unsupported 而非崩溃 |
| WindowStation 树 | P1 | `win32kbase.pdb`、`win32kfull.pdb` | 候选 `tagWINDOWSTATION`，session globals 中窗口站链表/句柄表 | stationObject、nameHash/namePtr、sessionId、desktopCount、flags、next | WindowStation 名称、Session、Desktop 数、异常标记 | `EnumWindowStationsW` | 名称字段可能不是稳定 UNICODE_STRING；读不到名称时显示对象地址和 partial | R3 `EnumWindowStationsW` 能看到的窗口站在 R0 树中存在；读不到名称不影响继续枚举 desktop |
| Desktop 树 | P1 | `win32kbase.pdb`、`win32kfull.pdb` | 候选 `tagDESKTOP`、desktop list、desktop info、窗口链头、hook 链头 | desktopObject、windowStationObject、sessionId、desktopName、heap/base、threadCount、windowCount、hookHead、flags | `WinSta\Desktop` 树、窗口数、线程数、Hook 数、是否当前输入桌面 | `EnumDesktopsW`、`OpenInputDesktop`、`GetThreadDesktop` | 安全桌面/登录桌面可能因权限或 session 不可达；返回 inaccessible/partial | 普通用户桌面下 R3 `EnumDesktopsW` 的桌面名与 R0 树一致；切换输入桌面后当前标记变化可解释 |
| HWND / `tagWND` 窗口表 | P0 | `win32kfull.pdb`、`win32kbase.pdb` | 候选 `tagWND`、desktop window list、owner/parent/child/next 链、class/title/style 字段 | hwnd/handleValue、tagWndObject、pid、tid、threadInfo、desktopObject、parent/owner/firstChild/next、style/exStyle、state flags、rect/clientRect、class atom/name、title ptr/摘要、processImage 可选 | HWND、PID/TID、进程名、标题、类名、可见/禁用/最小化、父子/Owner、Desktop、R0-only/R3-only 标记 | `EnumWindows`、`EnumChildWindows`、`GetWindowInfo`、`GetWindowThreadProcessId`、`GetClassNameW`、`GetWindowTextW` | 标题/类名读取跨进程和 session 容易失败；R0 默认只读短摘要并限长；链损坏时停止该链 | R3 `EnumWindows` 顶层窗口在 R0 表中可匹配；隐藏/message-only/R0-only 差异能显示但不导致误删 |
| `tagTHREADINFO` GUI 线程表 | P0 | `win32kfull.pdb`、`ntkrnlmp.pdb`/`ntkrla57.pdb` | 候选 `tagTHREADINFO`，`PsGetThreadWin32Thread`，`ETHREAD`，thread hook array，desktop/queue 指针 | tid、pid、ethread、threadInfo、desktopObject、queueObject、flags、hookArray、activeWnd/focus/capture 候选字段、lastError/status | TID/PID、进程名、Desktop、Queue、是否 GUI 线程、Hook 数、焦点/捕获摘要 | `GetWindowThreadProcessId`、`GetGUIThreadInfo`、Toolhelp/系统线程枚举 | 非 GUI 线程 Win32Thread 为空；返回 `not_gui_thread`；不反向猜结构 | 对每个 R3 窗口 TID，R0 能找到对应 `tagTHREADINFO` 或给出明确状态；`GetGUIThreadInfo` 可校验焦点/活动窗口 |
| 输入队列 / `tagQ` | P1 | `win32kfull.pdb`、`win32kbase.pdb` | 候选 `tagQ`、threadInfo->pq、队列 flags、active/focus/capture/caret/menu owner 字段、队列链 | queueObject、ownerTid/Pid、threadCount、activeWnd、focusWnd、captureWnd、caretWnd、menuOwner、flags、messageCount 摘要 | Queue 地址、关联线程、Active/Focus/Capture/Caret/Menu、队列状态、异常标记 | `GetGUIThreadInfo`、`GetForegroundWindow`、`GetFocus`/`GetCapture` 在本线程或辅助线程上下文 | 消息队列内部链易变；P1 只做状态字段，不默认遍历所有消息 | R0 active/focus/capture 与 `GetGUIThreadInfo`/`GetForegroundWindow` 在稳定桌面下大体一致；不一致记录时间戳和 session |
| Window Hooks | P0 | `win32kfull.pdb`、`win32kbase.pdb` | 候选 `tagHOOK`、thread hook array、desktop/global hook array、module id 映射 | hookObject、scope(thread/global)、hookType、pid/tid、targetThreadInfo、desktopInfo、procedureAddress、moduleBase/moduleId、flags、next、chainHead、status | Hook 类型、范围、目标 PID/TID、回调地址、模块归属、全局/线程链、异常模块 | 无完整公开枚举 API；可与当前已有 `WH_KEYBOARD/WH_KEYBOARD_LL` 结果、进程模块表、签名信息交叉 | Hook 类型多，字段随 build 变；P0 先覆盖现有键盘 hook 并扩展通用表头，未知类型只显示编号 | 现有 `IOCTL_KSWORD_ARK_ENUM_KEYBOARD_HOOKS` 字段由 PDB profile 填充偏移；同一机器上结果不劣于旧硬编码路径 |
| Hotkeys | P0 | `win32kfull.pdb`、`win32kbase.pdb` | session globals hotkey table，hotkey object/list，threadInfo/window 指针，VK/modifier/id 字段 | hotkeyObject、bucket、depth、modifiers、vk、id、pid、tid、threadInfo、windowObject、sessionGlobals、next、status | 快捷键、VK、修饰键、PID/TID、HWND、窗口标题、注册来源、链深 | 无公开“枚举全部 RegisterHotKey”API；可用测试程序注册热键后 R0 结果校验；窗口字段用 `IsWindow`/`GetWindowThreadProcessId` 对比 | 表布局变化风险高；PDB profile 主路径，模式扫描只作 fallback；链深限制 512 或更低 | 测试进程注册多个 hotkey 后能稳定枚举；取消注册后消失；profile 不支持时返回 unsupported |
| Clipboard owner/listener | P1 | `win32kbase.pdb`、`win32kfull.pdb` | desktop/session clipboard state、owner hwnd、open hwnd、viewer/listener list、sequence number 候选字段 | ownerHwnd、openClipboardHwnd、viewer/listener hwnd、pid/tid、desktopObject、sequence、formatCount 摘要、status | 剪贴板 Owner/Open 窗口、监听者列表、PID/TID、窗口标题、是否隐藏/message-only | `GetClipboardOwner`、`GetOpenClipboardWindow`、`GetClipboardSequenceNumber`、`EnumClipboardFormats`、`GetClipboardViewer` | listener 链和现代 AddClipboardFormatListener 存储细节可能变化；只读 hwnd 列表，不读取剪贴板内容默认值 | R0 owner/open hwnd 与 R3 API 一致；注册 clipboard listener 的测试窗口能出现在 listener 或降级说明中 |
| Message-only windows | P1 | `win32kfull.pdb` | `tagWND` parent/owner/desktop/message-only sentinel，窗口链 | hwnd、tagWndObject、pid/tid、class/title、style/exStyle、owner、threadInfo、desktopObject、isMessageOnly | 消息窗口表、进程、类名、标题、是否无可见父窗口、风险标签 | `FindWindowEx(HWND_MESSAGE, ...)`、`EnumChildWindows(HWND_MESSAGE, ...)` 可辅助 | message-only 根/标志需 PDB 验证；读不到标志时用父链/desktop 关系降级 | 创建 message-only 测试窗口后 R0/R3 均能识别；普通顶层窗口不误标为 message-only |
| Foreground / focus / capture | P0 | `win32kbase.pdb`、`win32kfull.pdb` | desktop/session foreground queue/window、`tagQ` active/focus/capture、threadInfo 状态字段 | foregroundHwnd、activeHwnd、focusHwnd、captureHwnd、caretHwnd、queueObject、tid/pid、desktopObject、timestamp/status | 前台/活动/焦点/捕获窗口卡片，关联线程和进程，R0/R3 差异 | `GetForegroundWindow`、`GetGUIThreadInfo`、`GetFocus`、`GetCapture` | 焦点/捕获有调用线程语义；R3 对比必须说明采样上下文；R0 只做快照 | 手工切换窗口、鼠标捕获测试时，R0 快照与 R3 API 在同一桌面下可解释一致 |

### 2.2 P0/P1/P2 分层

- **P0 必做**：PDB profile 生成与匹配、`tagWND` 顶层窗口 cross-view、`tagTHREADINFO` 基础字段、foreground/focus/capture 快照、hotkey PDB 化、现有 keyboard hook PDB 化。
- **P1 跟进**：WindowStation/Desktop 树、`tagQ` 输入队列详情、message-only windows、clipboard owner/listener、更多 hook 类型统一展示。
- **P2 延后**：输入设备链、深度消息队列采样、剪贴板格式/内容审计、GDI 句柄关联、win32k inline/path 完整性扩展。

---

## 3. WM_COPYDATA 与窗口消息事件捕获

### 3.1 可以只读审计的内容

这些可以作为 R0 GUI 审计默认能力，因为它们不需要拦截消息路径，也不读取任意用户态 payload：

- 枚举可能参与 `WM_COPYDATA` 通信的窗口：普通顶层窗口、隐藏窗口、message-only windows、特定类名窗口。
- 展示接收方上下文：HWND、PID/TID、进程路径、类名、标题、Desktop、WindowStation、线程队列、可见性、完整性/签名信息由现有进程模块补充。
- 识别风险模式：高权限进程中的隐藏/message-only 窗口、无标题但长期存在的窗口、短时间内大量窗口创建销毁的进程、R0 可见但 R3 `EnumWindows` 不可见的窗口。
- 做队列/焦点状态快照：active/focus/capture/foreground 与窗口表关联，用于判断消息可能进入的上下文。

验收方式：准备一个测试发送端和接收端，接收端创建普通窗口与 message-only 窗口；R0 只读窗口表能标出接收端窗口及其线程队列，不要求看到 payload。

### 3.2 需要 R3 hook、ETW 或采样的内容

这些属于“事件流”而不是静态对象枚举，不建议作为 R0 只读默认项：

- 每一条 `SendMessageW` / `SendMessageTimeoutW` / `PostMessageW` 的发送方、接收方、消息号和时间线。
- `WM_COPYDATA` 的 `COPYDATASTRUCT.dwData/cbData/lpData` 内容、payload hash、字符串摘要。
- 跨进程 sender/receiver 的调用栈、窗口过程返回值、超时状态。

建议路径：

- **R3 API hook/监控**：复用现有 WinAPI 监控方向，在目标进程或测试进程侧记录 `SendMessage*` / `PostMessage*`，只在用户授权目标进程时采集 payload 摘要。
- **ETW**：优先验证是否有可用 provider/事件能覆盖窗口消息或 GUI 活动；若事件不足，只作为时间线辅助，不承诺完整消息捕获。
- **采样**：对高风险窗口做定时静态快照，记录窗口状态、队列摘要和前台/焦点变化，避免持续拦截。

验收方式：同一测试程序发送 `WM_COPYDATA`，R3 hook 能记录消息号和 payload 摘要；R0 表只负责给出接收 HWND 的真实归属和 cross-view 状态。

### 3.3 不适合 R0 默认拦截的内容

以下不进入默认实现：

- Hook 或 patch `win32kfull!xxxSendMessage`、窗口过程分发、队列投递路径。
- 在 R0 默认复制任意 `COPYDATASTRUCT.lpData` 指向的用户缓冲区。
- 默认阻断、修改、重放窗口消息。
- 为了捕获消息而插入全局 WH_CALLWNDPROC/WH_GETMESSAGE hook。

原因：这些路径对 PatchGuard/稳定性/会话隔离/GUI 死锁风险都过高，且会把“审计”变成“拦截”。如果后续要做，只能放到显式授权的 R3 目标监控或实验模式。

---

## 4. UI 前置建议

### 4.1 桌面树

- 左侧树：`Session -> WindowStation -> Desktop`。
- 节点字段：名称、对象地址、窗口数、线程数、Hook 数、是否当前输入桌面、profile 状态。
- 交互：选择 Desktop 后过滤右侧窗口表/线程队列表/Hook 表。
- 验收：R3 `EnumWindowStationsW`/`EnumDesktopsW` 可见节点与树节点一致；不可访问节点显示 partial，不阻塞其它节点。

### 4.2 窗口表

- 字段：HWND、R0 tagWND、PID、TID、进程名、标题、类名、Style、ExStyle、Rect、Parent、Owner、Desktop、可见/启用/最小化、R0/R3 cross-view 状态。
- 支持筛选：PID/TID、Desktop、隐藏窗口、message-only、R0-only、标题/类名关键字。
- 验收：现有 `KswordARKLight\Features\Window` R3 枚举结果可作为 baseline；新增 R0 表至少能覆盖 baseline 顶层窗口。

### 4.3 线程队列表

- 字段：PID、TID、`tagTHREADINFO`、`tagQ`、Desktop、Active/Focus/Capture/Caret、Hook 数、Queue flags、状态。
- 交互：点击线程显示该线程拥有窗口、队列状态、线程 hook。
- 验收：窗口表中每个 TID 均能跳转到线程队列表；非 GUI 线程不出现在该表或显示 `not_gui_thread`。

### 4.4 Hook 表

- 字段：Hook 类型、Scope、PID/TID、Desktop、Hook 对象、回调地址、模块基址/名称、flags、链深、异常原因。
- 初始覆盖：复用现有 keyboard hook 协议字段，先把偏移来源从硬编码改为 PDB profile。
- 验收：旧 keyboard hook 枚举测试通过；PDB profile 命中时显示 profile id 和偏移来源。

### 4.5 Hotkey 表

- 字段：修饰键、VK、ID、PID/TID、HWND、标题、类名、hotkey object、bucket/depth、状态。
- 交互：只读查看；删除/修改不属于本阶段。
- 验收：测试程序注册/注销 hotkey 后表项同步变化；不注册时不产生误报。

### 4.6 异常告警表

- 初始规则：R0-only 窗口、隐藏 message-only 高权限窗口、Hook 回调地址不在已加载模块、Hook 链循环/超深、hotkey 链异常、clipboard owner/listener 指向已退出进程、foreground/focus 指向无效 HWND。
- 告警字段：级别、对象类型、对象地址、PID/TID、原因、建议动作、数据源、采样时间。
- 验收：构造链深限制/无效 HWND 不应导致崩溃；仅显示告警和 partial 状态。

---

## 5. 最小可验收 MVP

### 5.1 第一批字段

P0 MVP 只做以下字段，避免一次铺开：

1. Profile 层：`win32kbase.sys`、`win32kfull.sys` 模块身份；`tagWND`、`tagTHREADINFO`、`tagQ`、`tagHOOK` 的结构大小和必要成员偏移；hotkey 表/对象必要偏移。
2. Window 表：HWND、tagWND、PID、TID、Desktop、Style/ExStyle、Rect、Parent、Owner、Visible/Enabled、Class/Title 短摘要、R0/R3 match 状态。
3. Thread/Queue 表：TID、PID、threadInfo、queue、desktop、active/focus/capture hwnd、状态。
4. Hook 表：先覆盖 `WH_KEYBOARD`、`WH_KEYBOARD_LL`，字段与当前 `KSWORD_ARK_KEYBOARD_HOOK_ENTRY` 对齐，但偏移来源改为 profile。
5. Hotkey 表：字段与当前 `KSWORD_ARK_KEYBOARD_HOTKEY_ENTRY` 对齐，但 table/object layout 来源改为 profile。
6. Foreground/focus/capture 快照：返回当前 desktop/queue 视角下的 hwnd 与 PID/TID。

暂不做：clipboard listener 完整链、所有窗口消息事件、WM_COPYDATA payload、GDI 句柄、删除/修复动作。

### 5.2 Build 覆盖

最小覆盖建议：

- Windows 10 22H2 x64：至少 1 个常用 build。
- Windows 11 23H2 x64：至少 1 个常用 build。
- Windows 11 24H2/25H2 x64：至少 1 个当前主力 build。

每个 build 必须有对应 `win32kbase.pdb` 和 `win32kfull.pdb` profile。缺任一 profile 时，该 build 的 GUI R0 审计入口显示 unsupported，不走硬编码。

### 5.3 如何确认无崩溃

- R0 所有读取使用安全读封装，单字段失败只影响当前对象；链表遍历必须有最大条数、最大深度、已访问集合或循环检测。
- 每个 IOCTL 输出包含：version、status、totalCount、returnedCount、entrySize、profileId、lastStatus、truncated 标记。
- 压力测试只做只读刷新：连续刷新窗口表/Hook 表/Hotkey 表 100 次；同时创建/销毁窗口、注册/注销 hotkey、切换前台窗口。
- 负面测试：profile 缺失、profile GUID 不匹配、输出 buffer 太小、目标 session 无 GUI 线程、窗口销毁竞态。预期均返回 unsupported/partial/buffer_truncated/read_failed，不蓝屏、不挂 UI。
- Cross-view 测试：R3 `EnumWindows` baseline 与 R0 `tagWND` 快照做匹配率统计；不要求 100% 完全一致，但每个差异都必须有分类：R0-only、R3-only、session 不同、权限/桌面不可达、对象销毁竞态。

---

## 6. 当前最高优先级和技术债务

### P0 最高优先级

1. 建立 `win32kbase/win32kfull` PDB profile schema，并把模块身份、结构成员偏移、全局 RVA、字段有效位写清楚。
2. 把现有 keyboard hotkey/hook 枚举从“模式扫描 + 硬编码偏移”迁移为“PDB profile 主路径 + 模式扫描降级诊断”。
3. 做 `tagWND` 顶层窗口 cross-view MVP，把 R0 只读窗口快照与现有 `KswordARKLight\Features\Window` R3 baseline 对齐。

### 最大风险点

1. **win32k 私有结构跨 build 变化**：不能继续扩大硬编码偏移，必须 profile 匹配失败即关闭能力。
2. **session/desktop 上下文错误**：跨 session 读 GUI 对象容易读到无效地址或误判；所有枚举都要绑定 session 和 desktop。
3. **消息捕获边界失控**：WM_COPYDATA payload 和窗口消息事件不应混入 R0 默认审计，否则会把只读审计升级成高风险拦截。

---

## 7. 结论

下一步可以做庞大的 GUI 审计功能，但落点应先是 PDB profile 和只读 cross-view，而不是直接改内核逻辑去拦截消息。以现有资产看，`win32kbase.pdb`、`win32kfull.pdb`、现有 keyboard IOCTL、现有 R3 Window API baseline 已足够支撑 P0 MVP：窗口表、GUI 线程/队列快照、foreground/focus/capture、hotkey、keyboard hook。Clipboard、message-only windows、Desktop 树属于 P1，可在 P0 稳定后扩展。WM_COPYDATA 真实事件和 payload 捕获应放在 R3 hook/ETW/采样路径，不作为 R0 默认功能。
