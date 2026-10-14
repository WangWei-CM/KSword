# Issue 223：高密度结构报告

## 页面职责与共享入口

入口：进程列表 → 进程详细信息 → Process Detail Evidence，默认进入“结构报告”。页面按只读结构清单/字段详情组织，继续使用详情窗口框架；正文复用 `TablePresentation::Compact` 和原生三列树表，操作条明确为 None，文档级复制/导出沿用 `StructuredFieldView`。

对照了 `KernelObjectTypeMatrixTab` 的紧凑工具行与类型清单、`KernelBaseNamedObjectsTab` 的范围/类型筛选，以及共享 `TablePresentation`、`SegmentedControl`、`CodeEditorWidget`。参数页、线程操作或其它旧详情没有批量改造。

## 当前交互

- 默认选择“进程结构 / _EPROCESS”，立即展示字段列表。范围和类型可以直接选择，返回某个范围时记住上次类型；诊断集中在一个入口。
- 字段、偏移、类型/值占真实的三列，每行自然高度 22px（更大应用字体相应增长）。不再逐字段绘卡片、重复路径标题或复制按钮。
- Offset/Type 已在列中呈现，展开时不重复成两行；位域、DynData 别名等附加信息仍可通过单击字段名或原生键盘展开。
- Ctrl+C 复制当前单元格，Ctrl+Shift+C 或上下文菜单复制完整字段。更多菜单提供当前范围的复制与展开/收起；原文和全报告动作不受字段过滤截断。
- 搜索支持多个词，例如 `eprocess 0x0440`。在当前范围所有类型中查找，只展开到字段列表；空匹配不会误退回其它范围。清空恢复阅读状态，搜索中换范围不会带入旧范围滚动位置。
- “字段 / 原文”使用共享分段控制。原文首次使用时才创建只读 `CodeEditorWidget`，包含当前类型/范围的完整规范数据及元信息，可搜索、选取复制和导出；选择“全部报告”可查看整份报告。
- 刷新、状态与原搜索/文档操作合为一行，删除重复页标题。诊断页隐藏没有数据的偏移列。
- 三列表和两列诊断有明确的名称列宽，通过共享 `SetTableColumnAutoFitEnabled(false)` 退出按内容权重重新分配。类型/值列承接剩余空间，两种模式各自保留用户手动调宽结果，避免诊断长值把名称列挤成省略号。

## 数据与兼容性

`FieldNode::ReportRole` 由生产者明确区分目录、类型、记录和元信息；默认 Content 保持其它字段文档的原行为。显示计数来自原始资料，属性数不冒充字段数。三列表、搜索、原文和完整复制共用 `ComplexReportModel` 的规范快照；新增显示角色不改变 `FieldDocument::toPlainText()` 的公开格式。

范围约束在代理模型中执行，不能只依赖 `QTreeView::setRootIndex()`：当所选根被搜索过滤掉时，后者会成为无效根并意外显示其它分区。数值更新保留三列的持久索引，真正拓扑变化才重置。列的可访问文本只返回该列内容。

`CreateDetailFieldProjection` 增加可选 toolbarPrefix，原调用保持默认参数；只有本报告把既有刷新区并入文档工具行。中文/英文语言包定点更新，未重排整包。没有修改驱动协议、identity 准入、查询或采样权限。

## 验证

- Qt Core 生产模型/序列化回归：95 项通过。覆盖三列独立值、元信息不重复占行、完整原文、范围隔离、多词搜索、显示标题搜索、空匹配不越界、诊断过滤、持久单元格与可访问性。
- 最终主程序 Release/x64：`BUILD_RESULT=SUCCESS`、`EXIT_CODE=0`；语言包与主题门禁通过。产物 25096704 字节，SHA256 `2489B600ADE64A5256EE17149A623B14439BBF77F147A1AAC7C979587ACB5CAC`；日志 `.codex-build-logs/ksword-build-check-20261013-203103.raw.log`。
- 真实主程序读取现有生产 PDB 目录。默认 _EPROCESS 232 个字段，正常宽度首屏约 23 个字段；799px 窄窗仍保持相同垂直密度。_PEB 直接切换显示 115 个字段，不需经过折叠树路径。
- 已实际用 Ctrl+C 复制 UniqueProcessId 的偏移，并粘贴回搜索框，读回 `0x0440`；附加属性展开只增加 DynData 行，没有重复 Offset/Type。
- 已检查原文切换、范围/类型记忆、诊断入口、更多菜单、深浅主题字段表与窄窗。原“跟随系统”偏好已恢复。最后重新打开最终产物，核对了状态提示、独立单元格可访问文本、正常/799px窄窗字段表和诊断表；标题完整可读，诊断偏移空列已隐藏，窗口宽度恢复到原 1162px。
- 最终截图：`output/issue-223-dense-final-dark.jpg`、`issue-223-dense-final-narrow.jpg`、`issue-223-dense-final-diagnostics.jpg`、`issue-223-dense-final-diagnostic-narrow.jpg`。其它交互/主题证据来自本轮前一构建：`issue-223-dense-copy-offset.jpg`、`issue-223-dense-raw-dark.jpg`、`issue-223-dense-narrow-light.jpg`；之后仅收尾列宽、状态和辅助读取，没有继续改动原文/复制路径。

当前 R0 不可用，因此实窗覆盖静态目录与不可用诊断，不证明实际采样成功。真实 DPI/背景组合、文件导出选择器、其它详情窗口未逐项验收。本文的信息密度描述来自实窗，不把模型计时或构建结果当作 UI 操作效率的证据。
