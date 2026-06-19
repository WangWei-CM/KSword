#include "FileHandleUsageScanner.h"

// ============================================================
// FileHandleUsageScanner.cpp
// 作用：
// - 保留 FileDock 使用的 Qt-facing 数据结构；
// - 将路径转换、句柄扫描、进程/模块占用分析全部委托给 ks::file；
// - UI 层继续负责 kProgress 转接和 QString 结果展示。
// ============================================================

#include "../ksword/file/file_handle_tools.h"

#include <QString>

#include <string>
#include <vector>

namespace filedock::handleusage
{
    namespace
    {
        // ToWidePathList 作用：把 UI 传入的 QString 路径集合转换成 std::wstring 集合。
        // 输入 absolutePaths：FileDock 选择或右键动作收集到的路径；返回后端可消费的宽字符串路径列表。
        std::vector<std::wstring> ToWidePathList(const std::vector<QString>& absolutePaths)
        {
            std::vector<std::wstring> pathList;
            pathList.reserve(absolutePaths.size());
            for (const QString& pathText : absolutePaths)
            {
                if (pathText.trimmed().isEmpty())
                {
                    continue;
                }
                pathList.push_back(pathText.toStdWString());
            }
            return pathList;
        }

        // ToQString 作用：把后端 std::wstring 文本转换为 UI 可直接显示的 QString。
        // 输入 textValue：后端宽字符串；返回 QString，空字符串保持为空。
        QString ToQString(const std::wstring& textValue)
        {
            return QString::fromStdWString(textValue);
        }

        // ConvertEntry 作用：把 ks::file 后端命中项转换为 FileDock 原有表格行模型。
        // 输入 backendEntry：后端扫描结果；返回 HandleUsageEntry，不含任何控件或颜色状态。
        HandleUsageEntry ConvertEntry(const ks::file::HandleUsageEntry& backendEntry)
        {
            HandleUsageEntry entry{};
            entry.processId = backendEntry.processId;
            entry.processName = ToQString(backendEntry.processName);
            entry.processImagePath = ToQString(backendEntry.processImagePath);
            entry.handleValue = backendEntry.handleValue;
            entry.typeIndex = backendEntry.typeIndex;
            entry.typeName = ToQString(backendEntry.typeName);
            entry.objectName = ToQString(backendEntry.objectName);
            entry.grantedAccess = backendEntry.grantedAccess;
            entry.attributes = backendEntry.attributes;
            entry.matchedTargetPath = ToQString(backendEntry.matchedTargetPath);
            entry.matchedByDirectoryRule = backendEntry.matchedByDirectoryRule;
            entry.matchRuleText = ToQString(backendEntry.matchRuleText);
            entry.enumerationSource = ToQString(backendEntry.enumerationSource);
            return entry;
        }

        // ConvertResult 作用：把 ks::file 扫描结果转换为 FileDock 现有窗口消费的结果类型。
        // 输入 backendResult：后端完整结果；返回含 QString 文本的 HandleUsageScanResult。
        HandleUsageScanResult ConvertResult(const ks::file::HandleUsageScanResult& backendResult)
        {
            HandleUsageScanResult result{};
            result.entries.reserve(backendResult.entries.size());
            for (const ks::file::HandleUsageEntry& backendEntry : backendResult.entries)
            {
                result.entries.push_back(ConvertEntry(backendEntry));
            }
            result.totalHandleCount = backendResult.totalHandleCount;
            result.fileLikeHandleCount = backendResult.fileLikeHandleCount;
            result.matchedHandleCount = backendResult.matchedHandleCount;
            result.processImageMatchCount = backendResult.processImageMatchCount;
            result.loadedModuleMatchCount = backendResult.loadedModuleMatchCount;
            result.kernelHandleMatchCount = backendResult.kernelHandleMatchCount;
            result.elapsedMs = backendResult.elapsedMs;
            result.diagnosticText = ToQString(backendResult.diagnosticText);
            return result;
        }
    }

    HandleUsageScanResult scanHandleUsageByPaths(
        const std::vector<QString>& absolutePaths,
        const int progressPid,
        const bool tryKernelHandleTable)
    {
        ks::file::HandleUsageScanOptions options{};
        options.tryKernelHandleTable = tryKernelHandleTable;
        if (progressPid > 0)
        {
            // ProgressCallback 只转接纯文本和百分比，具体进度条生命周期仍由 FileDock 窗口控制。
            options.progressCallback = [progressPid](const std::string& stepText, const float progressValue)
            {
                kPro.set(progressPid, stepText, 0, progressValue);
            };
        }

        const std::vector<std::wstring> pathList = ToWidePathList(absolutePaths);
        const ks::file::HandleUsageScanResult backendResult = ks::file::ScanHandleUsageByPaths(pathList, options);
        return ConvertResult(backendResult);
    }
}
