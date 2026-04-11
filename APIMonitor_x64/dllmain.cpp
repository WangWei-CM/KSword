#include "pch.h"
#include "MonitorAgent.h"

// ============================================================
// dllmain.cpp
// 作用：
// 1) 作为 APIMonitor_x64 的 DLL 入口；
// 2) 仅负责最小化转发到 MonitorAgent，避免在 DllMain 中堆积业务逻辑；
// 3) 保持 DllMain 简短，降低 Loader Lock 风险。
// ============================================================

BOOL APIENTRY DllMain(HMODULE moduleHandle, DWORD reasonCode, LPVOID reservedPointer)
{
    (void)reservedPointer;

    switch (reasonCode)
    {
    case DLL_PROCESS_ATTACH:
        apimon::OnProcessAttach(moduleHandle);
        break;
    case DLL_PROCESS_DETACH:
        apimon::OnProcessDetach();
        break;
    default:
        break;
    }
    return TRUE;
}
