#pragma once

// ============================================================
// DriverEnumerator.h
// 作用说明：
// 1) 提供驱动概览与对象信息的只读枚举入口；
// 2) 仅做 R3 查询和本地数据整理；
// 3) 不包含任何 Win32 控件或驱动写入逻辑。
// ============================================================

#include "DriverModel.h"

#include <string>
#include <vector>

namespace Ksword::Features::Driver {

// DriverEnumerationResult groups the latest snapshot and diagnostics. Inputs
// are none; processing occurs in DriverEnumerator.cpp; output is consumed by
// DriverActions and the two Win32 views.
struct DriverEnumerationResult {
    bool success = false;                          // success: true only when the snapshot was collected.
    DWORD win32Error = ERROR_SUCCESS;              // win32Error: Win32 error for diagnostics.
    std::wstring diagnosticText;                   // diagnosticText: human-readable result text.
    std::vector<DriverOverviewRow> overviewRows;    // overviewRows: driver overview table rows.
    std::vector<DriverObjectRow> objectRows;        // objectRows: driver object table rows.
};

// EnumerateDriverSnapshot refreshes both driver tables in one pass. There is no
// input; processing queries module data and Object Manager directories; output
// contains overview rows, object rows and a diagnostic message for the views.
DriverEnumerationResult EnumerateDriverSnapshot();

} // namespace Ksword::Features::Driver

