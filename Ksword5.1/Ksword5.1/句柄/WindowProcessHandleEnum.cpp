// ============================================================
// WindowProcessHandleEnum.cpp
// 作用：
// - 保留旧“按路径枚举占用句柄”控制台示例；
// - 不再维护独立原生句柄查询代码；
// - 通过 ks::file::ScanHandleUsageByPaths 复用 FileDock 后端。
// 说明：本文件未纳入 Ksword5.1.vcxproj，仅作为手工诊断样例保留。
// ============================================================

#include "../ksword/file/file_handle_tools.h"

#include <Windows.h>

#include <iostream>
#include <string>
#include <vector>

int wmain(int argc, wchar_t* argv[])
{
    // 输入参数是一个或多个文件/目录路径；目录会按前缀规则匹配。
    ::SetConsoleOutputCP(CP_UTF8);
    ::SetConsoleCP(CP_UTF8);
    if (argc <= 1)
    {
        std::wcout << L"Usage: WindowProcessHandleEnum.exe <file-or-directory> [more paths...]\n";
        return 1;
    }

    // 路径原样传给 ks::file，后端会生成 DOS/NT 双视角匹配规则。
    std::vector<std::wstring> pathList;
    for (int index = 1; index < argc; ++index)
    {
        if (argv[index] != nullptr && argv[index][0] != L'\0')
        {
            pathList.emplace_back(argv[index]);
        }
    }

    // 进度回调只输出阶段文本；真实 UI 进度条由 FileDock 适配层负责。
    ks::file::HandleUsageScanOptions options{};
    options.tryKernelHandleTable = true;
    options.progressCallback = [](const std::string& stepText, const float progressValue)
    {
        std::cout << "[" << progressValue << "%] " << stepText << std::endl;
    };

    const ks::file::HandleUsageScanResult result = ks::file::ScanHandleUsageByPaths(pathList, options);
    std::wcout << L"matched=" << result.entries.size()
        << L" totalHandles=" << result.totalHandleCount
        << L" elapsedMs=" << result.elapsedMs << L"\n";
    if (!result.diagnosticText.empty())
    {
        std::wcout << L"diagnostic=" << result.diagnosticText << L"\n";
    }

    // 输出命中来源，包含文件句柄、进程映像和模块加载三类后端结果。
    for (const ks::file::HandleUsageEntry& entry : result.entries)
    {
        std::wcout << L"PID=" << entry.processId
            << L" Process=" << entry.processName
            << L" Handle=0x" << std::hex << entry.handleValue << std::dec
            << L" Source=" << entry.enumerationSource
            << L" Rule=" << entry.matchRuleText
            << L" Object=" << entry.objectName << L"\n";
    }
    return 0;
}
