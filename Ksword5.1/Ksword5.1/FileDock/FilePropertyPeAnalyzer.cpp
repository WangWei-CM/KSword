#include "FilePropertyPeAnalyzer.h"

// ============================================================
// FilePropertyPeAnalyzer.cpp
// 作用：
// - 保留 FileDock 属性窗口的 QString API；
// - 将 PE 读取、头部、节表、导入/导出/目录解析委托给 ks::file；
// - UI 层只负责把返回文本显示到 CodeEditorWidget。
// ============================================================

#include "../ksword/file/pe_analyzer.h"

#include <string>

namespace file_dock_detail
{
    QString buildPeAnalysisText(const QString& filePath)
    {
        // 输入 filePath 来自 Qt UI；转换为 std::wstring 后交给非 UI 后端。
        // 返回值仍为 QString，以保持 FileDock 属性窗口调用点不变。
        const std::wstring reportText = ks::file::BuildPeAnalysisText(filePath.toStdWString());
        return QString::fromStdWString(reportText);
    }
}
