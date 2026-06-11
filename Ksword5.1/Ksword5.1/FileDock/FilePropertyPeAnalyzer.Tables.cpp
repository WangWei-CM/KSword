#include "FilePropertyPeAnalyzer.Internal.h"

// ============================================================
// FilePropertyPeAnalyzer.Tables.cpp
// 作用：
// - 旧版 PE 表项解析分片的兼容编译单元；
// - 可复用的 PE 基础分析后端已迁移到 ks::file::AnalyzePeFile；
// - UI 层现在通过 FilePropertyPeAnalyzer.cpp 调用 ks::file，不再在本文件维护解析逻辑。
// 输入：无运行时输入。
// 处理：本文件不再注册或执行任何 PE 解析函数。
// 返回：无返回值。
// ============================================================
