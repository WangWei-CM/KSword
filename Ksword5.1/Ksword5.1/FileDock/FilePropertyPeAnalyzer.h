#pragma once

// ============================================================
// FilePropertyPeAnalyzer.h
// 作用：
// 1) 为文件属性窗口提供 PE 头解析文本；
// 2) 输出导入表、导出表与区段概览；
// 3) 与 FileDock UI 解耦，便于后续继续扩展资源表/重定位表。
// ============================================================

#include <QString>

namespace file_dock_detail
{
    // buildPeAnalysisText 作用：
    // - 读取并解析指定文件的 PE 结构；
    // - 返回可直接显示到 CodeEditorWidget 的文本。
    // 参数 filePath：目标文件完整路径。
    // 返回：解析结果文本；若失败则返回可读错误说明。
    QString buildPeAnalysisText(const QString& filePath);
}

