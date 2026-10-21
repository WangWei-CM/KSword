#pragma once
#include "../Ksword5.1/Ksword5.1/ksword/network/network_process_etw_monitor.h"

ULONG WINAPI NativeFakeStartTraceW(PTRACEHANDLE, LPCWSTR, PEVENT_TRACE_PROPERTIES);
ULONG WINAPI NativeFakeControlTraceW(TRACEHANDLE, LPCWSTR, PEVENT_TRACE_PROPERTIES, ULONG);
ULONG WINAPI NativeFakeEnableTraceEx2(TRACEHANDLE, LPCGUID, ULONG, UCHAR, ULONGLONG, ULONGLONG, ULONG, PENABLE_TRACE_PARAMETERS);
PROCESSTRACE_HANDLE WINAPI NativeFakeOpenTraceW(PEVENT_TRACE_LOGFILEW);
ULONG WINAPI NativeFakeCloseTrace(PROCESSTRACE_HANDLE);
ULONG WINAPI NativeFakeProcessTrace(PROCESSTRACE_HANDLE*, ULONG, LPFILETIME, LPFILETIME);

#define StartTraceW NativeFakeStartTraceW
#define ControlTraceW NativeFakeControlTraceW
#define EnableTraceEx2 NativeFakeEnableTraceEx2
#define OpenTraceW NativeFakeOpenTraceW
#define CloseTrace NativeFakeCloseTrace
#define ProcessTrace NativeFakeProcessTrace
