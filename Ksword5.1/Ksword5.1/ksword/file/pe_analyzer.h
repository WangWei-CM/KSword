#pragma once

// ============================================================
// ksword/file/pe_analyzer.h
// 命名空间：ks::file
// 作用：
// - 提供不依赖 Qt 的 PE 基础解析能力；
// - 解析 PE 头、节表、导入/导出、数据目录和常用目录摘要；
// - UI 层只负责把 std::wstring 报告转换为编辑器文本。
// ============================================================

#include <cstdint>
#include <string>
#include <vector>

namespace ks::file
{
    // PeSectionSummary 作用：
    // - 保存区段表中 UI/日志常用字段；
    // - analyzePeFile 可用于后续非文本化展示。
    struct PeSectionSummary
    {
        std::string name;
        std::uint32_t virtualAddress = 0;
        std::uint32_t virtualSize = 0;
        std::uint32_t rawOffset = 0;
        std::uint32_t rawSize = 0;
        std::uint32_t characteristics = 0;
        double entropy = 0.0;
    };

    // PeAnalysisResult 作用：
    // - 聚合 PE 解析结果文本与结构化节表摘要；
    // - success=false 时 reportText 保存可读失败原因。
    struct PeAnalysisResult
    {
        bool success = false;
        bool isPe64 = false;
        std::uint16_t machine = 0;
        std::uint16_t subsystem = 0;
        std::uint32_t entryPointRva = 0;
        std::uint64_t imageBase = 0;
        std::vector<PeSectionSummary> sections;
        std::wstring reportText;
    };

    // AnalyzePeFile 作用：读取文件并解析 PE 结构，返回结构化结果与报告文本。
    PeAnalysisResult AnalyzePeFile(const std::wstring& filePath);

    // BuildPeAnalysisText 作用：兼容属性窗口现有“直接拿文本显示”的调用方式。
    std::wstring BuildPeAnalysisText(const std::wstring& filePath);

    // BuildPeAnalysisTextUtf8 作用：便于非 Qt 调用者传入 UTF-8 文件路径并获得 UTF-8 报告。
    std::string BuildPeAnalysisTextUtf8(const std::string& filePathUtf8);
}
