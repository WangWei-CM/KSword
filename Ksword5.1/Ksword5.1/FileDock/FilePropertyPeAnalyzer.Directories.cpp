#include "FilePropertyPeAnalyzer.Internal.h"

// ============================================================
// FilePropertyPeAnalyzer.Directories.cpp
// 作用：
// - 旧版 PE 数据目录解析分片的兼容编译单元；
// - 资源、重定位、调试、TLS、CLR、证书等目录摘要已经迁移到 ksword/file/pe_analyzer.cpp；
// - 保留该空编译单元只为稳定现有 .vcxproj 文件条目，不再承载后端逻辑。
// 输入：无运行时输入。
// 处理：本文件不再依赖 Qt 容器或 Win32 PE 结构解析实现。
// 返回：无返回值。
// ============================================================
