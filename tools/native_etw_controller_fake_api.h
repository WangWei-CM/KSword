#pragma once
#include "../shared/usermode/backend/monitor/EtwSessionController.h"

ULONG WINAPI NativeControllerStartTraceW(PTRACEHANDLE, LPCWSTR, PEVENT_TRACE_PROPERTIES);
ULONG WINAPI NativeControllerControlTraceW(TRACEHANDLE, LPCWSTR, PEVENT_TRACE_PROPERTIES, ULONG);
ULONG WINAPI NativeControllerEnableTraceEx2(TRACEHANDLE, LPCGUID, ULONG, UCHAR, ULONGLONG, ULONGLONG, ULONG, PENABLE_TRACE_PARAMETERS);
TRACEHANDLE WINAPI NativeControllerOpenTraceW(PEVENT_TRACE_LOGFILEW);
ULONG WINAPI NativeControllerCloseTrace(TRACEHANDLE);
ULONG WINAPI NativeControllerProcessTrace(TRACEHANDLE*, ULONG, LPFILETIME, LPFILETIME);

#define StartTraceW NativeControllerStartTraceW
#define ControlTraceW NativeControllerControlTraceW
#define EnableTraceEx2 NativeControllerEnableTraceEx2
#define OpenTraceW NativeControllerOpenTraceW
#define CloseTrace NativeControllerCloseTrace
#define ProcessTrace NativeControllerProcessTrace
