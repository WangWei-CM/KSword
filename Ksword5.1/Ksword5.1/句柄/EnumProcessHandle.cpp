// ============================================================
// EnumProcessHandle.cpp
// 作用：
// - 保留旧句柄枚举控制台示例文件；
// - 不再维护独立原生句柄查询代码；
// - 通过 ks::file::BuildHandleSnapshot 复用主程序句柄 Dock 后端。
// 说明：本文件未纳入 Ksword5.1.vcxproj，仅作为手工诊断样例保留。
// ============================================================

#include "../ksword/file/file_handle_tools.h"

#include <Windows.h>

#include <cstdlib>
#include <cstdint>
#include <iostream>
#include <string>

int wmain(int argc, wchar_t* argv[])
{
    // 控制台输出使用 UTF-8 代码页，便于显示对象名中的中文路径。
    ::SetConsoleOutputCP(CP_UTF8);
    ::SetConsoleCP(CP_UTF8);

    // 可选参数 argv[1] 是 PID；未提供时枚举全局句柄快照。
    ks::file::HandleSnapshotOptions options{};
    options.enumMode = ks::file::HandleEnumMode::DuplicateHandle;
    options.resolveObjectName = true;
    options.nameResolveBudget = 128;
    if (argc > 1 && argv[1] != nullptr && argv[1][0] != L'\0')
    {
        options.hasPidFilter = true;
        options.pidFilter = static_cast<std::uint32_t>(std::wcstoul(argv[1], nullptr, 10));
    }

    // 后端负责 Nt API 动态加载、句柄复制、对象名解析和 R0/R3 差异字段填充。
    const ks::file::HandleSnapshotResult result = ks::file::BuildHandleSnapshot(options);
    std::wcout << L"total=" << result.totalHandleCount
        << L" visible=" << result.visibleHandleCount
        << L" elapsedMs=" << result.elapsedMs << L"\n";
    if (!result.diagnosticText.empty())
    {
        std::wcout << L"diagnostic=" << result.diagnosticText << L"\n";
    }

    // 示例只打印前 200 行，避免控制台被大系统句柄表刷屏。
    std::size_t printedCount = 0;
    for (const ks::file::HandleSnapshotRow& row : result.rows)
    {
        if (printedCount++ >= 200U)
        {
            std::wcout << L"<truncated>\n";
            break;
        }
        std::wcout << L"PID=" << row.processId
            << L" Process=" << row.processName
            << L" Handle=0x" << std::hex << row.handleValue << std::dec
            << L" Type=" << row.typeName
            << L" Name=" << row.objectName << L"\n";
    }
    return 0;
}
