#include "pch.h"
#include "HookTargets.h"
#include "HookEngine.h"
#include "../MonitorAgent.h"
#include "../core/MonitorPipe.h"

#include <WinReg.h>
#include <shellapi.h>
#include <winternl.h>

#ifndef NT_SUCCESS
#define NT_SUCCESS(Status) (((NTSTATUS)(Status)) >= 0)
#endif

typedef enum _KS_FILE_INFORMATION_CLASS
{
    KsFileBasicInformation = 4,
    KsFileRenameInformation = 10,
    KsFileDispositionInformation = 13,
    KsFilePositionInformation = 14,
    KsFileEndOfFileInformation = 20,
    KsFileDispositionInformationEx = 64,
    KsFileRenameInformationEx = 65
} KS_FILE_INFORMATION_CLASS;

typedef enum _KS_KEY_INFORMATION_CLASS
{
    KsKeyBasicInformation = 0,
    KsKeyNodeInformation = 1,
    KsKeyFullInformation = 2,
    KsKeyNameInformation = 3,
    KsKeyCachedInformation = 4,
    KsKeyFlagsInformation = 5,
    KsKeyVirtualizationInformation = 6,
    KsKeyHandleTagsInformation = 7,
    KsKeyTrustInformation = 8,
    KsKeyLayerInformation = 9
} KS_KEY_INFORMATION_CLASS;

typedef enum _KS_KEY_VALUE_INFORMATION_CLASS
{
    KsKeyValueBasicInformation = 0,
    KsKeyValueFullInformation = 1,
    KsKeyValuePartialInformation = 2,
    KsKeyValueFullInformationAlign64 = 3,
    KsKeyValuePartialInformationAlign64 = 4,
    KsKeyValueLayerInformation = 5
} KS_KEY_VALUE_INFORMATION_CLASS;

typedef struct _KS_CLIENT_ID
{
    HANDLE UniqueProcess;
    HANDLE UniqueThread;
} KS_CLIENT_ID, *PKS_CLIENT_ID;

namespace apimon
{
    namespace
    {
        using CreateFileAFn = HANDLE(WINAPI*)(LPCSTR, DWORD, DWORD, LPSECURITY_ATTRIBUTES, DWORD, DWORD, HANDLE);
        using CreateFileWFn = HANDLE(WINAPI*)(LPCWSTR, DWORD, DWORD, LPSECURITY_ATTRIBUTES, DWORD, DWORD, HANDLE);
        using CreateFile2Fn = HANDLE(WINAPI*)(LPCWSTR, DWORD, DWORD, DWORD, LPCREATEFILE2_EXTENDED_PARAMETERS);
        using ReadFileFn = BOOL(WINAPI*)(HANDLE, LPVOID, DWORD, LPDWORD, LPOVERLAPPED);
        using WriteFileFn = BOOL(WINAPI*)(HANDLE, LPCVOID, DWORD, LPDWORD, LPOVERLAPPED);
        using DeviceIoControlFn = BOOL(WINAPI*)(HANDLE, DWORD, LPVOID, DWORD, LPVOID, DWORD, LPDWORD, LPOVERLAPPED);
        using DeleteFileWFn = BOOL(WINAPI*)(LPCWSTR);
        using DeleteFileAFn = BOOL(WINAPI*)(LPCSTR);
        using MoveFileExWFn = BOOL(WINAPI*)(LPCWSTR, LPCWSTR, DWORD);
        using MoveFileExAFn = BOOL(WINAPI*)(LPCSTR, LPCSTR, DWORD);
        using CopyFileWFn = BOOL(WINAPI*)(LPCWSTR, LPCWSTR, BOOL);
        using CopyFileAFn = BOOL(WINAPI*)(LPCSTR, LPCSTR, BOOL);
        using CopyFileExWFn = BOOL(WINAPI*)(LPCWSTR, LPCWSTR, LPPROGRESS_ROUTINE, LPVOID, LPBOOL, DWORD);
        using CopyFileExAFn = BOOL(WINAPI*)(LPCSTR, LPCSTR, LPPROGRESS_ROUTINE, LPVOID, LPBOOL, DWORD);
        using GetFileAttributesWFn = DWORD(WINAPI*)(LPCWSTR);
        using GetFileAttributesAFn = DWORD(WINAPI*)(LPCSTR);
        using GetFileAttributesExWFn = BOOL(WINAPI*)(LPCWSTR, GET_FILEEX_INFO_LEVELS, LPVOID);
        using GetFileAttributesExAFn = BOOL(WINAPI*)(LPCSTR, GET_FILEEX_INFO_LEVELS, LPVOID);
        using SetFileAttributesWFn = BOOL(WINAPI*)(LPCWSTR, DWORD);
        using SetFileAttributesAFn = BOOL(WINAPI*)(LPCSTR, DWORD);
        using FindFirstFileExWFn = HANDLE(WINAPI*)(LPCWSTR, FINDEX_INFO_LEVELS, LPVOID, FINDEX_SEARCH_OPS, LPVOID, DWORD);
        using FindFirstFileExAFn = HANDLE(WINAPI*)(LPCSTR, FINDEX_INFO_LEVELS, LPVOID, FINDEX_SEARCH_OPS, LPVOID, DWORD);
        using CreateDirectoryWFn = BOOL(WINAPI*)(LPCWSTR, LPSECURITY_ATTRIBUTES);
        using CreateDirectoryAFn = BOOL(WINAPI*)(LPCSTR, LPSECURITY_ATTRIBUTES);
        using RemoveDirectoryWFn = BOOL(WINAPI*)(LPCWSTR);
        using RemoveDirectoryAFn = BOOL(WINAPI*)(LPCSTR);
        using SetFileInformationByHandleFn = BOOL(WINAPI*)(HANDLE, FILE_INFO_BY_HANDLE_CLASS, LPVOID, DWORD);
        using CreateProcessAFn = BOOL(WINAPI*)(LPCSTR, LPSTR, LPSECURITY_ATTRIBUTES, LPSECURITY_ATTRIBUTES, BOOL, DWORD, LPVOID, LPCSTR, LPSTARTUPINFOA, LPPROCESS_INFORMATION);
        using CreateProcessWFn = BOOL(WINAPI*)(LPCWSTR, LPWSTR, LPSECURITY_ATTRIBUTES, LPSECURITY_ATTRIBUTES, BOOL, DWORD, LPVOID, LPCWSTR, LPSTARTUPINFOW, LPPROCESS_INFORMATION);
        using OpenProcessFn = HANDLE(WINAPI*)(DWORD, BOOL, DWORD);
        using OpenThreadFn = HANDLE(WINAPI*)(DWORD, BOOL, DWORD);
        using TerminateProcessFn = BOOL(WINAPI*)(HANDLE, UINT);
        using CreateThreadFn = HANDLE(WINAPI*)(LPSECURITY_ATTRIBUTES, SIZE_T, LPTHREAD_START_ROUTINE, LPVOID, DWORD, LPDWORD);
        using CreateRemoteThreadFn = HANDLE(WINAPI*)(HANDLE, LPSECURITY_ATTRIBUTES, SIZE_T, LPTHREAD_START_ROUTINE, LPVOID, DWORD, LPDWORD);
        using SuspendThreadFn = DWORD(WINAPI*)(HANDLE);
        using ResumeThreadFn = DWORD(WINAPI*)(HANDLE);
        using QueueUserAPCFn = DWORD(WINAPI*)(PAPCFUNC, HANDLE, ULONG_PTR);
        using GetThreadContextFn = BOOL(WINAPI*)(HANDLE, LPCONTEXT);
        using SetThreadContextFn = BOOL(WINAPI*)(HANDLE, const CONTEXT*);
        using VirtualAllocExFn = LPVOID(WINAPI*)(HANDLE, LPVOID, SIZE_T, DWORD, DWORD);
        using VirtualFreeExFn = BOOL(WINAPI*)(HANDLE, LPVOID, SIZE_T, DWORD);
        using VirtualProtectExFn = BOOL(WINAPI*)(HANDLE, LPVOID, SIZE_T, DWORD, PDWORD);
        using WriteProcessMemoryFn = BOOL(WINAPI*)(HANDLE, LPVOID, LPCVOID, SIZE_T, SIZE_T*);
        using ReadProcessMemoryFn = BOOL(WINAPI*)(HANDLE, LPCVOID, LPVOID, SIZE_T, SIZE_T*);
        using WinExecFn = UINT(WINAPI*)(LPCSTR, UINT);
        using ShellExecuteExWFn = BOOL(WINAPI*)(SHELLEXECUTEINFOW*);
        using ShellExecuteExAFn = BOOL(WINAPI*)(SHELLEXECUTEINFOA*);
        using LoadLibraryAFn = HMODULE(WINAPI*)(LPCSTR);
        using LoadLibraryWFn = HMODULE(WINAPI*)(LPCWSTR);
        using LoadLibraryExAFn = HMODULE(WINAPI*)(LPCSTR, HANDLE, DWORD);
        using LoadLibraryExWFn = HMODULE(WINAPI*)(LPCWSTR, HANDLE, DWORD);
        using LdrLoadDllFn = NTSTATUS(NTAPI*)(PWSTR, PULONG, PUNICODE_STRING, PHANDLE);
        using RegOpenKeyWFn = LSTATUS(WINAPI*)(HKEY, LPCWSTR, PHKEY);
        using RegOpenKeyAFn = LSTATUS(WINAPI*)(HKEY, LPCSTR, PHKEY);
        using RegOpenKeyExWFn = LSTATUS(WINAPI*)(HKEY, LPCWSTR, DWORD, REGSAM, PHKEY);
        using RegOpenKeyExAFn = LSTATUS(WINAPI*)(HKEY, LPCSTR, DWORD, REGSAM, PHKEY);
        using RegCreateKeyWFn = LSTATUS(WINAPI*)(HKEY, LPCWSTR, PHKEY);
        using RegCreateKeyAFn = LSTATUS(WINAPI*)(HKEY, LPCSTR, PHKEY);
        using RegCreateKeyExWFn = LSTATUS(WINAPI*)(HKEY, LPCWSTR, DWORD, LPWSTR, DWORD, REGSAM, const LPSECURITY_ATTRIBUTES, PHKEY, LPDWORD);
        using RegCreateKeyExAFn = LSTATUS(WINAPI*)(HKEY, LPCSTR, DWORD, LPSTR, DWORD, REGSAM, const LPSECURITY_ATTRIBUTES, PHKEY, LPDWORD);
        using RegQueryValueExWFn = LSTATUS(WINAPI*)(HKEY, LPCWSTR, LPDWORD, LPDWORD, LPBYTE, LPDWORD);
        using RegQueryValueExAFn = LSTATUS(WINAPI*)(HKEY, LPCSTR, LPDWORD, LPDWORD, LPBYTE, LPDWORD);
        using RegGetValueWFn = LSTATUS(WINAPI*)(HKEY, LPCWSTR, LPCWSTR, DWORD, LPDWORD, PVOID, LPDWORD);
        using RegGetValueAFn = LSTATUS(WINAPI*)(HKEY, LPCSTR, LPCSTR, DWORD, LPDWORD, PVOID, LPDWORD);
        using RegSetValueExWFn = LSTATUS(WINAPI*)(HKEY, LPCWSTR, DWORD, DWORD, const BYTE*, DWORD);
        using RegSetValueExAFn = LSTATUS(WINAPI*)(HKEY, LPCSTR, DWORD, DWORD, const BYTE*, DWORD);
        using RegSetKeyValueWFn = LSTATUS(WINAPI*)(HKEY, LPCWSTR, LPCWSTR, DWORD, const void*, DWORD);
        using RegSetKeyValueAFn = LSTATUS(WINAPI*)(HKEY, LPCSTR, LPCSTR, DWORD, const void*, DWORD);
        using RegDeleteValueWFn = LSTATUS(WINAPI*)(HKEY, LPCWSTR);
        using RegDeleteValueAFn = LSTATUS(WINAPI*)(HKEY, LPCSTR);
        using RegDeleteKeyWFn = LSTATUS(WINAPI*)(HKEY, LPCWSTR);
        using RegDeleteKeyAFn = LSTATUS(WINAPI*)(HKEY, LPCSTR);
        using RegDeleteKeyExWFn = LSTATUS(WINAPI*)(HKEY, LPCWSTR, REGSAM, DWORD);
        using RegDeleteKeyExAFn = LSTATUS(WINAPI*)(HKEY, LPCSTR, REGSAM, DWORD);
        using RegDeleteTreeWFn = LSTATUS(WINAPI*)(HKEY, LPCWSTR);
        using RegDeleteTreeAFn = LSTATUS(WINAPI*)(HKEY, LPCSTR);
        using RegCopyTreeWFn = LSTATUS(WINAPI*)(HKEY, LPCWSTR, HKEY);
        using RegCopyTreeAFn = LSTATUS(WINAPI*)(HKEY, LPCSTR, HKEY);
        using RegLoadKeyWFn = LSTATUS(WINAPI*)(HKEY, LPCWSTR, LPCWSTR);
        using RegLoadKeyAFn = LSTATUS(WINAPI*)(HKEY, LPCSTR, LPCSTR);
        using RegSaveKeyWFn = LSTATUS(WINAPI*)(HKEY, LPCWSTR, const LPSECURITY_ATTRIBUTES);
        using RegSaveKeyAFn = LSTATUS(WINAPI*)(HKEY, LPCSTR, const LPSECURITY_ATTRIBUTES);
        using RegRenameKeyFn = LSTATUS(WINAPI*)(HKEY, LPCWSTR, LPCWSTR);
        using RegEnumKeyExWFn = LSTATUS(WINAPI*)(HKEY, DWORD, LPWSTR, LPDWORD, LPDWORD, LPWSTR, LPDWORD, PFILETIME);
        using RegEnumKeyExAFn = LSTATUS(WINAPI*)(HKEY, DWORD, LPSTR, LPDWORD, LPDWORD, LPSTR, LPDWORD, PFILETIME);
        using RegEnumValueWFn = LSTATUS(WINAPI*)(HKEY, DWORD, LPWSTR, LPDWORD, LPDWORD, LPDWORD, LPBYTE, LPDWORD);
        using RegEnumValueAFn = LSTATUS(WINAPI*)(HKEY, DWORD, LPSTR, LPDWORD, LPDWORD, LPDWORD, LPBYTE, LPDWORD);
        using RegCloseKeyFn = LSTATUS(WINAPI*)(HKEY);
        using RegQueryInfoKeyWFn = LSTATUS(WINAPI*)(HKEY, LPWSTR, LPDWORD, LPDWORD, LPDWORD, LPDWORD, LPDWORD, LPDWORD, LPDWORD, LPDWORD, LPDWORD, PFILETIME);
        using RegQueryInfoKeyAFn = LSTATUS(WINAPI*)(HKEY, LPSTR, LPDWORD, LPDWORD, LPDWORD, LPDWORD, LPDWORD, LPDWORD, LPDWORD, LPDWORD, LPDWORD, PFILETIME);
        using RegFlushKeyFn = LSTATUS(WINAPI*)(HKEY);
        using RegDeleteKeyValueWFn = LSTATUS(WINAPI*)(HKEY, LPCWSTR, LPCWSTR);
        using RegDeleteKeyValueAFn = LSTATUS(WINAPI*)(HKEY, LPCSTR, LPCSTR);
        using RegConnectRegistryWFn = LSTATUS(WINAPI*)(LPCWSTR, HKEY, PHKEY);
        using RegConnectRegistryAFn = LSTATUS(WINAPI*)(LPCSTR, HKEY, PHKEY);
        using ConnectFn = int (WSAAPI*)(SOCKET, const sockaddr*, int);
        using WSAConnectFn = int (WSAAPI*)(SOCKET, const sockaddr*, int, LPWSABUF, LPWSABUF, LPQOS, LPQOS);
        using SendFn = int (WSAAPI*)(SOCKET, const char*, int, int);
        using WSASendFn = int (WSAAPI*)(SOCKET, LPWSABUF, DWORD, LPDWORD, DWORD, LPWSAOVERLAPPED, LPWSAOVERLAPPED_COMPLETION_ROUTINE);
        using SendToFn = int (WSAAPI*)(SOCKET, const char*, int, int, const sockaddr*, int);
        using RecvFn = int (WSAAPI*)(SOCKET, char*, int, int);
        using WSARecvFn = int (WSAAPI*)(SOCKET, LPWSABUF, DWORD, LPDWORD, LPDWORD, LPWSAOVERLAPPED, LPWSAOVERLAPPED_COMPLETION_ROUTINE);
        using RecvFromFn = int (WSAAPI*)(SOCKET, char*, int, int, sockaddr*, int*);
        using BindFn = int (WSAAPI*)(SOCKET, const sockaddr*, int);
        using ListenFn = int (WSAAPI*)(SOCKET, int);
        using AcceptFn = SOCKET(WSAAPI*)(SOCKET, sockaddr*, int*);
        using SocketFn = SOCKET(WSAAPI*)(int, int, int);
        using WSASocketWFn = SOCKET(WSAAPI*)(int, int, int, LPWSAPROTOCOL_INFOW, GROUP, DWORD);
        using WSASocketAFn = SOCKET(WSAAPI*)(int, int, int, LPWSAPROTOCOL_INFOA, GROUP, DWORD);
        using CloseSocketFn = int(WSAAPI*)(SOCKET);
        using ShutdownFn = int(WSAAPI*)(SOCKET, int);
        using KsIoApcRoutine = VOID(NTAPI*)(PVOID, PIO_STATUS_BLOCK, ULONG);
        using NtCreateFileFn = NTSTATUS(NTAPI*)(PHANDLE, ACCESS_MASK, POBJECT_ATTRIBUTES, PIO_STATUS_BLOCK, PLARGE_INTEGER, ULONG, ULONG, ULONG, ULONG, PVOID, ULONG);
        using NtOpenFileFn = NTSTATUS(NTAPI*)(PHANDLE, ACCESS_MASK, POBJECT_ATTRIBUTES, PIO_STATUS_BLOCK, ULONG, ULONG);
        using NtReadFileFn = NTSTATUS(NTAPI*)(HANDLE, HANDLE, KsIoApcRoutine, PVOID, PIO_STATUS_BLOCK, PVOID, ULONG, PLARGE_INTEGER, PULONG);
        using NtWriteFileFn = NTSTATUS(NTAPI*)(HANDLE, HANDLE, KsIoApcRoutine, PVOID, PIO_STATUS_BLOCK, PVOID, ULONG, PLARGE_INTEGER, PULONG);
        using NtSetInformationFileFn = NTSTATUS(NTAPI*)(HANDLE, PIO_STATUS_BLOCK, PVOID, ULONG, KS_FILE_INFORMATION_CLASS);
        using NtQueryInformationFileFn = NTSTATUS(NTAPI*)(HANDLE, PIO_STATUS_BLOCK, PVOID, ULONG, KS_FILE_INFORMATION_CLASS);
        using NtDeleteFileFn = NTSTATUS(NTAPI*)(POBJECT_ATTRIBUTES);
        using NtQueryAttributesFileFn = NTSTATUS(NTAPI*)(POBJECT_ATTRIBUTES, PVOID);
        using NtQueryFullAttributesFileFn = NTSTATUS(NTAPI*)(POBJECT_ATTRIBUTES, PVOID);
        using NtDeviceIoControlFileFn = NTSTATUS(NTAPI*)(HANDLE, HANDLE, KsIoApcRoutine, PVOID, PIO_STATUS_BLOCK, ULONG, PVOID, ULONG, PVOID, ULONG);
        using NtFsControlFileFn = NTSTATUS(NTAPI*)(HANDLE, HANDLE, KsIoApcRoutine, PVOID, PIO_STATUS_BLOCK, ULONG, PVOID, ULONG, PVOID, ULONG);
        using NtQueryDirectoryFileFn = NTSTATUS(NTAPI*)(HANDLE, HANDLE, KsIoApcRoutine, PVOID, PIO_STATUS_BLOCK, PVOID, ULONG, KS_FILE_INFORMATION_CLASS, BOOLEAN, PUNICODE_STRING, BOOLEAN);
        using NtQueryDirectoryFileExFn = NTSTATUS(NTAPI*)(HANDLE, HANDLE, KsIoApcRoutine, PVOID, PIO_STATUS_BLOCK, PVOID, ULONG, KS_FILE_INFORMATION_CLASS, ULONG, PUNICODE_STRING);
        using NtCreateKeyFn = NTSTATUS(NTAPI*)(PHANDLE, ACCESS_MASK, POBJECT_ATTRIBUTES, ULONG, PUNICODE_STRING, ULONG, PULONG);
        using NtOpenKeyFn = NTSTATUS(NTAPI*)(PHANDLE, ACCESS_MASK, POBJECT_ATTRIBUTES);
        using NtOpenKeyExFn = NTSTATUS(NTAPI*)(PHANDLE, ACCESS_MASK, POBJECT_ATTRIBUTES, ULONG);
        using NtSetValueKeyFn = NTSTATUS(NTAPI*)(HANDLE, PUNICODE_STRING, ULONG, ULONG, PVOID, ULONG);
        using NtQueryValueKeyFn = NTSTATUS(NTAPI*)(HANDLE, PUNICODE_STRING, KS_KEY_VALUE_INFORMATION_CLASS, PVOID, ULONG, PULONG);
        using NtEnumerateKeyFn = NTSTATUS(NTAPI*)(HANDLE, ULONG, KS_KEY_INFORMATION_CLASS, PVOID, ULONG, PULONG);
        using NtEnumerateValueKeyFn = NTSTATUS(NTAPI*)(HANDLE, ULONG, KS_KEY_VALUE_INFORMATION_CLASS, PVOID, ULONG, PULONG);
        using NtDeleteKeyFn = NTSTATUS(NTAPI*)(HANDLE);
        using NtDeleteValueKeyFn = NTSTATUS(NTAPI*)(HANDLE, PUNICODE_STRING);
        using NtFlushKeyFn = NTSTATUS(NTAPI*)(HANDLE);
        using NtRenameKeyFn = NTSTATUS(NTAPI*)(HANDLE, PUNICODE_STRING);
        using NtLoadKeyFn = NTSTATUS(NTAPI*)(POBJECT_ATTRIBUTES, POBJECT_ATTRIBUTES);
        using NtSaveKeyFn = NTSTATUS(NTAPI*)(HANDLE, HANDLE);
        using NtQueryKeyFn = NTSTATUS(NTAPI*)(HANDLE, KS_KEY_INFORMATION_CLASS, PVOID, ULONG, PULONG);
        using NtQueryMultipleValueKeyFn = NTSTATUS(NTAPI*)(HANDLE, PVOID, ULONG, PVOID, PULONG, PULONG);
        using NtNotifyChangeKeyFn = NTSTATUS(NTAPI*)(HANDLE, HANDLE, KsIoApcRoutine, PVOID, PIO_STATUS_BLOCK, ULONG, BOOLEAN, PVOID, ULONG, BOOLEAN);
        using NtLoadKey2Fn = NTSTATUS(NTAPI*)(POBJECT_ATTRIBUTES, POBJECT_ATTRIBUTES, ULONG);
        using NtSaveKeyExFn = NTSTATUS(NTAPI*)(HANDLE, HANDLE, ULONG);
        using NtLoadDriverFn = NTSTATUS(NTAPI*)(PUNICODE_STRING);
        using NtUnloadDriverFn = NTSTATUS(NTAPI*)(PUNICODE_STRING);
        using NtOpenProcessFn = NTSTATUS(NTAPI*)(PHANDLE, ACCESS_MASK, POBJECT_ATTRIBUTES, PKS_CLIENT_ID);
        using NtOpenThreadFn = NTSTATUS(NTAPI*)(PHANDLE, ACCESS_MASK, POBJECT_ATTRIBUTES, PKS_CLIENT_ID);
        using NtTerminateProcessFn = NTSTATUS(NTAPI*)(HANDLE, NTSTATUS);
        using NtCreateUserProcessFn = NTSTATUS(NTAPI*)(PHANDLE, PHANDLE, ACCESS_MASK, ACCESS_MASK, POBJECT_ATTRIBUTES, POBJECT_ATTRIBUTES, ULONG, ULONG, PVOID, PVOID, PVOID);
        using NtCreateProcessExFn = NTSTATUS(NTAPI*)(PHANDLE, ACCESS_MASK, POBJECT_ATTRIBUTES, HANDLE, ULONG, HANDLE, HANDLE, HANDLE, BOOLEAN);
        using NtCreateThreadExFn = NTSTATUS(NTAPI*)(PHANDLE, ACCESS_MASK, POBJECT_ATTRIBUTES, HANDLE, PVOID, PVOID, ULONG, SIZE_T, SIZE_T, SIZE_T, PVOID);
        using NtAllocateVirtualMemoryFn = NTSTATUS(NTAPI*)(HANDLE, PVOID*, ULONG_PTR, PSIZE_T, ULONG, ULONG);
        using NtFreeVirtualMemoryFn = NTSTATUS(NTAPI*)(HANDLE, PVOID*, PSIZE_T, ULONG);
        using NtProtectVirtualMemoryFn = NTSTATUS(NTAPI*)(HANDLE, PVOID*, PSIZE_T, ULONG, PULONG);
        using NtWriteVirtualMemoryFn = NTSTATUS(NTAPI*)(HANDLE, PVOID, PVOID, SIZE_T, PSIZE_T);
        using NtReadVirtualMemoryFn = NTSTATUS(NTAPI*)(HANDLE, PVOID, PVOID, SIZE_T, PSIZE_T);
        using NtMapViewOfSectionFn = NTSTATUS(NTAPI*)(HANDLE, HANDLE, PVOID*, ULONG_PTR, SIZE_T, PLARGE_INTEGER, PSIZE_T, DWORD, ULONG, ULONG);
        using NtUnmapViewOfSectionFn = NTSTATUS(NTAPI*)(HANDLE, PVOID);
        using NtDuplicateObjectFn = NTSTATUS(NTAPI*)(HANDLE, HANDLE, HANDLE, PHANDLE, ACCESS_MASK, ULONG, ULONG);
        using NtQueryInformationProcessFn = NTSTATUS(NTAPI*)(HANDLE, ULONG, PVOID, ULONG, PULONG);
        using NtSetInformationProcessFn = NTSTATUS(NTAPI*)(HANDLE, ULONG, PVOID, ULONG);
        using NtQueryVirtualMemoryFn = NTSTATUS(NTAPI*)(HANDLE, PVOID, ULONG, PVOID, SIZE_T, PSIZE_T);
        using NtCreateSectionFn = NTSTATUS(NTAPI*)(PHANDLE, ACCESS_MASK, POBJECT_ATTRIBUTES, PLARGE_INTEGER, ULONG, ULONG, HANDLE);
        using NtOpenSectionFn = NTSTATUS(NTAPI*)(PHANDLE, ACCESS_MASK, POBJECT_ATTRIBUTES);
        using NtQueueApcThreadFn = NTSTATUS(NTAPI*)(HANDLE, PVOID, PVOID, PVOID, PVOID);
        using NtQueueApcThreadExFn = NTSTATUS(NTAPI*)(HANDLE, HANDLE, PVOID, PVOID, PVOID, PVOID);
        using NtSuspendThreadFn = NTSTATUS(NTAPI*)(HANDLE, PULONG);
        using NtResumeThreadFn = NTSTATUS(NTAPI*)(HANDLE, PULONG);
        using NtGetContextThreadFn = NTSTATUS(NTAPI*)(HANDLE, PCONTEXT);
        using NtSetContextThreadFn = NTSTATUS(NTAPI*)(HANDLE, PCONTEXT);

        InlineHookRecord g_createFileAHook{};
        InlineHookRecord g_createFileWHook{};
        InlineHookRecord g_createFile2Hook{};
        InlineHookRecord g_readFileHook{};
        InlineHookRecord g_writeFileHook{};
        InlineHookRecord g_deviceIoControlHook{};
        InlineHookRecord g_deleteFileWHook{};
        InlineHookRecord g_deleteFileAHook{};
        InlineHookRecord g_moveFileExWHook{};
        InlineHookRecord g_moveFileExAHook{};
        InlineHookRecord g_copyFileWHook{};
        InlineHookRecord g_copyFileAHook{};
        InlineHookRecord g_copyFileExWHook{};
        InlineHookRecord g_copyFileExAHook{};
        InlineHookRecord g_getFileAttributesWHook{};
        InlineHookRecord g_getFileAttributesAHook{};
        InlineHookRecord g_getFileAttributesExWHook{};
        InlineHookRecord g_getFileAttributesExAHook{};
        InlineHookRecord g_setFileAttributesWHook{};
        InlineHookRecord g_setFileAttributesAHook{};
        InlineHookRecord g_findFirstFileExWHook{};
        InlineHookRecord g_findFirstFileExAHook{};
        InlineHookRecord g_createDirectoryWHook{};
        InlineHookRecord g_createDirectoryAHook{};
        InlineHookRecord g_removeDirectoryWHook{};
        InlineHookRecord g_removeDirectoryAHook{};
        InlineHookRecord g_setFileInformationByHandleHook{};
        InlineHookRecord g_createProcessAHook{};
        InlineHookRecord g_createProcessWHook{};
        InlineHookRecord g_openProcessHook{};
        InlineHookRecord g_openThreadHook{};
        InlineHookRecord g_terminateProcessHook{};
        InlineHookRecord g_createThreadHook{};
        InlineHookRecord g_createRemoteThreadHook{};
        InlineHookRecord g_suspendThreadHook{};
        InlineHookRecord g_resumeThreadHook{};
        InlineHookRecord g_queueUserAPCHook{};
        InlineHookRecord g_getThreadContextHook{};
        InlineHookRecord g_setThreadContextHook{};
        InlineHookRecord g_virtualAllocExHook{};
        InlineHookRecord g_virtualFreeExHook{};
        InlineHookRecord g_virtualProtectExHook{};
        InlineHookRecord g_writeProcessMemoryHook{};
        InlineHookRecord g_readProcessMemoryHook{};
        InlineHookRecord g_winExecHook{};
        InlineHookRecord g_shellExecuteExWHook{};
        InlineHookRecord g_shellExecuteExAHook{};
        InlineHookRecord g_loadLibraryAHook{};
        InlineHookRecord g_loadLibraryWHook{};
        InlineHookRecord g_loadLibraryExAHook{};
        InlineHookRecord g_loadLibraryExWHook{};
        InlineHookRecord g_ldrLoadDllHook{};
        InlineHookRecord g_regOpenKeyWHook{};
        InlineHookRecord g_regOpenKeyAHook{};
        InlineHookRecord g_regOpenKeyExWHook{};
        InlineHookRecord g_regOpenKeyExAHook{};
        InlineHookRecord g_regCreateKeyWHook{};
        InlineHookRecord g_regCreateKeyAHook{};
        InlineHookRecord g_regCreateKeyExWHook{};
        InlineHookRecord g_regCreateKeyExAHook{};
        InlineHookRecord g_regQueryValueExWHook{};
        InlineHookRecord g_regQueryValueExAHook{};
        InlineHookRecord g_regGetValueWHook{};
        InlineHookRecord g_regGetValueAHook{};
        InlineHookRecord g_regSetValueExWHook{};
        InlineHookRecord g_regSetValueExAHook{};
        InlineHookRecord g_regSetKeyValueWHook{};
        InlineHookRecord g_regSetKeyValueAHook{};
        InlineHookRecord g_regDeleteValueWHook{};
        InlineHookRecord g_regDeleteValueAHook{};
        InlineHookRecord g_regDeleteKeyWHook{};
        InlineHookRecord g_regDeleteKeyAHook{};
        InlineHookRecord g_regDeleteKeyExWHook{};
        InlineHookRecord g_regDeleteKeyExAHook{};
        InlineHookRecord g_regDeleteTreeWHook{};
        InlineHookRecord g_regDeleteTreeAHook{};
        InlineHookRecord g_regCopyTreeWHook{};
        InlineHookRecord g_regCopyTreeAHook{};
        InlineHookRecord g_regLoadKeyWHook{};
        InlineHookRecord g_regLoadKeyAHook{};
        InlineHookRecord g_regSaveKeyWHook{};
        InlineHookRecord g_regSaveKeyAHook{};
        InlineHookRecord g_regRenameKeyHook{};
        InlineHookRecord g_regEnumKeyExWHook{};
        InlineHookRecord g_regEnumKeyExAHook{};
        InlineHookRecord g_regEnumValueWHook{};
        InlineHookRecord g_regEnumValueAHook{};
        InlineHookRecord g_regCloseKeyHook{};
        InlineHookRecord g_regQueryInfoKeyWHook{};
        InlineHookRecord g_regQueryInfoKeyAHook{};
        InlineHookRecord g_regFlushKeyHook{};
        InlineHookRecord g_regDeleteKeyValueWHook{};
        InlineHookRecord g_regDeleteKeyValueAHook{};
        InlineHookRecord g_regConnectRegistryWHook{};
        InlineHookRecord g_regConnectRegistryAHook{};
        InlineHookRecord g_connectHook{};
        InlineHookRecord g_wsaConnectHook{};
        InlineHookRecord g_sendHook{};
        InlineHookRecord g_wsaSendHook{};
        InlineHookRecord g_sendToHook{};
        InlineHookRecord g_recvHook{};
        InlineHookRecord g_wsaRecvHook{};
        InlineHookRecord g_recvFromHook{};
        InlineHookRecord g_bindHook{};
        InlineHookRecord g_listenHook{};
        InlineHookRecord g_acceptHook{};
        InlineHookRecord g_socketHook{};
        InlineHookRecord g_wsaSocketWHook{};
        InlineHookRecord g_wsaSocketAHook{};
        InlineHookRecord g_closeSocketHook{};
        InlineHookRecord g_shutdownHook{};
        InlineHookRecord g_ntCreateFileHook{};
        InlineHookRecord g_ntOpenFileHook{};
        InlineHookRecord g_ntReadFileHook{};
        InlineHookRecord g_ntWriteFileHook{};
        InlineHookRecord g_ntSetInformationFileHook{};
        InlineHookRecord g_ntQueryInformationFileHook{};
        InlineHookRecord g_ntDeleteFileHook{};
        InlineHookRecord g_ntQueryAttributesFileHook{};
        InlineHookRecord g_ntQueryFullAttributesFileHook{};
        InlineHookRecord g_ntDeviceIoControlFileHook{};
        InlineHookRecord g_ntFsControlFileHook{};
        InlineHookRecord g_ntQueryDirectoryFileHook{};
        InlineHookRecord g_ntQueryDirectoryFileExHook{};
        InlineHookRecord g_ntCreateKeyHook{};
        InlineHookRecord g_ntOpenKeyHook{};
        InlineHookRecord g_ntOpenKeyExHook{};
        InlineHookRecord g_ntSetValueKeyHook{};
        InlineHookRecord g_ntQueryValueKeyHook{};
        InlineHookRecord g_ntEnumerateKeyHook{};
        InlineHookRecord g_ntEnumerateValueKeyHook{};
        InlineHookRecord g_ntDeleteKeyHook{};
        InlineHookRecord g_ntDeleteValueKeyHook{};
        InlineHookRecord g_ntFlushKeyHook{};
        InlineHookRecord g_ntRenameKeyHook{};
        InlineHookRecord g_ntLoadKeyHook{};
        InlineHookRecord g_ntSaveKeyHook{};
        InlineHookRecord g_ntQueryKeyHook{};
        InlineHookRecord g_ntQueryMultipleValueKeyHook{};
        InlineHookRecord g_ntNotifyChangeKeyHook{};
        InlineHookRecord g_ntLoadKey2Hook{};
        InlineHookRecord g_ntSaveKeyExHook{};
        InlineHookRecord g_ntLoadDriverHook{};
        InlineHookRecord g_ntUnloadDriverHook{};
        InlineHookRecord g_ntOpenProcessHook{};
        InlineHookRecord g_ntOpenThreadHook{};
        InlineHookRecord g_ntTerminateProcessHook{};
        InlineHookRecord g_ntCreateUserProcessHook{};
        InlineHookRecord g_ntCreateProcessExHook{};
        InlineHookRecord g_ntCreateThreadExHook{};
        InlineHookRecord g_ntAllocateVirtualMemoryHook{};
        InlineHookRecord g_ntFreeVirtualMemoryHook{};
        InlineHookRecord g_ntProtectVirtualMemoryHook{};
        InlineHookRecord g_ntWriteVirtualMemoryHook{};
        InlineHookRecord g_ntReadVirtualMemoryHook{};
        InlineHookRecord g_ntMapViewOfSectionHook{};
        InlineHookRecord g_ntUnmapViewOfSectionHook{};
        InlineHookRecord g_ntDuplicateObjectHook{};
        InlineHookRecord g_ntQueryInformationProcessHook{};
        InlineHookRecord g_ntSetInformationProcessHook{};
        InlineHookRecord g_ntQueryVirtualMemoryHook{};
        InlineHookRecord g_ntCreateSectionHook{};
        InlineHookRecord g_ntOpenSectionHook{};
        InlineHookRecord g_ntQueueApcThreadHook{};
        InlineHookRecord g_ntQueueApcThreadExHook{};
        InlineHookRecord g_ntSuspendThreadHook{};
        InlineHookRecord g_ntResumeThreadHook{};
        InlineHookRecord g_ntGetContextThreadHook{};
        InlineHookRecord g_ntSetContextThreadHook{};
        std::mutex g_hookOperationMutex;

        CreateFileAFn g_createFileAOriginal = nullptr;
        CreateFileWFn g_createFileWOriginal = nullptr;
        CreateFile2Fn g_createFile2Original = nullptr;
        ReadFileFn g_readFileOriginal = nullptr;
        WriteFileFn g_writeFileOriginal = nullptr;
        DeviceIoControlFn g_deviceIoControlOriginal = nullptr;
        DeleteFileWFn g_deleteFileWOriginal = nullptr;
        DeleteFileAFn g_deleteFileAOriginal = nullptr;
        MoveFileExWFn g_moveFileExWOriginal = nullptr;
        MoveFileExAFn g_moveFileExAOriginal = nullptr;
        CopyFileWFn g_copyFileWOriginal = nullptr;
        CopyFileAFn g_copyFileAOriginal = nullptr;
        CopyFileExWFn g_copyFileExWOriginal = nullptr;
        CopyFileExAFn g_copyFileExAOriginal = nullptr;
        GetFileAttributesWFn g_getFileAttributesWOriginal = nullptr;
        GetFileAttributesAFn g_getFileAttributesAOriginal = nullptr;
        GetFileAttributesExWFn g_getFileAttributesExWOriginal = nullptr;
        GetFileAttributesExAFn g_getFileAttributesExAOriginal = nullptr;
        SetFileAttributesWFn g_setFileAttributesWOriginal = nullptr;
        SetFileAttributesAFn g_setFileAttributesAOriginal = nullptr;
        FindFirstFileExWFn g_findFirstFileExWOriginal = nullptr;
        FindFirstFileExAFn g_findFirstFileExAOriginal = nullptr;
        CreateDirectoryWFn g_createDirectoryWOriginal = nullptr;
        CreateDirectoryAFn g_createDirectoryAOriginal = nullptr;
        RemoveDirectoryWFn g_removeDirectoryWOriginal = nullptr;
        RemoveDirectoryAFn g_removeDirectoryAOriginal = nullptr;
        SetFileInformationByHandleFn g_setFileInformationByHandleOriginal = nullptr;
        CreateProcessAFn g_createProcessAOriginal = nullptr;
        CreateProcessWFn g_createProcessWOriginal = nullptr;
        OpenProcessFn g_openProcessOriginal = nullptr;
        OpenThreadFn g_openThreadOriginal = nullptr;
        TerminateProcessFn g_terminateProcessOriginal = nullptr;
        CreateThreadFn g_createThreadOriginal = nullptr;
        CreateRemoteThreadFn g_createRemoteThreadOriginal = nullptr;
        SuspendThreadFn g_suspendThreadOriginal = nullptr;
        ResumeThreadFn g_resumeThreadOriginal = nullptr;
        QueueUserAPCFn g_queueUserAPCOriginal = nullptr;
        GetThreadContextFn g_getThreadContextOriginal = nullptr;
        SetThreadContextFn g_setThreadContextOriginal = nullptr;
        VirtualAllocExFn g_virtualAllocExOriginal = nullptr;
        VirtualFreeExFn g_virtualFreeExOriginal = nullptr;
        VirtualProtectExFn g_virtualProtectExOriginal = nullptr;
        WriteProcessMemoryFn g_writeProcessMemoryOriginal = nullptr;
        ReadProcessMemoryFn g_readProcessMemoryOriginal = nullptr;
        WinExecFn g_winExecOriginal = nullptr;
        ShellExecuteExWFn g_shellExecuteExWOriginal = nullptr;
        ShellExecuteExAFn g_shellExecuteExAOriginal = nullptr;
        LoadLibraryAFn g_loadLibraryAOriginal = nullptr;
        LoadLibraryWFn g_loadLibraryWOriginal = nullptr;
        LoadLibraryExAFn g_loadLibraryExAOriginal = nullptr;
        LoadLibraryExWFn g_loadLibraryExWOriginal = nullptr;
        LdrLoadDllFn g_ldrLoadDllOriginal = nullptr;
        RegOpenKeyWFn g_regOpenKeyWOriginal = nullptr;
        RegOpenKeyAFn g_regOpenKeyAOriginal = nullptr;
        RegOpenKeyExWFn g_regOpenKeyExWOriginal = nullptr;
        RegOpenKeyExAFn g_regOpenKeyExAOriginal = nullptr;
        RegCreateKeyWFn g_regCreateKeyWOriginal = nullptr;
        RegCreateKeyAFn g_regCreateKeyAOriginal = nullptr;
        RegCreateKeyExWFn g_regCreateKeyExWOriginal = nullptr;
        RegCreateKeyExAFn g_regCreateKeyExAOriginal = nullptr;
        RegQueryValueExWFn g_regQueryValueExWOriginal = nullptr;
        RegQueryValueExAFn g_regQueryValueExAOriginal = nullptr;
        RegGetValueWFn g_regGetValueWOriginal = nullptr;
        RegGetValueAFn g_regGetValueAOriginal = nullptr;
        RegSetValueExWFn g_regSetValueExWOriginal = nullptr;
        RegSetValueExAFn g_regSetValueExAOriginal = nullptr;
        RegSetKeyValueWFn g_regSetKeyValueWOriginal = nullptr;
        RegSetKeyValueAFn g_regSetKeyValueAOriginal = nullptr;
        RegDeleteValueWFn g_regDeleteValueWOriginal = nullptr;
        RegDeleteValueAFn g_regDeleteValueAOriginal = nullptr;
        RegDeleteKeyWFn g_regDeleteKeyWOriginal = nullptr;
        RegDeleteKeyAFn g_regDeleteKeyAOriginal = nullptr;
        RegDeleteKeyExWFn g_regDeleteKeyExWOriginal = nullptr;
        RegDeleteKeyExAFn g_regDeleteKeyExAOriginal = nullptr;
        RegDeleteTreeWFn g_regDeleteTreeWOriginal = nullptr;
        RegDeleteTreeAFn g_regDeleteTreeAOriginal = nullptr;
        RegCopyTreeWFn g_regCopyTreeWOriginal = nullptr;
        RegCopyTreeAFn g_regCopyTreeAOriginal = nullptr;
        RegLoadKeyWFn g_regLoadKeyWOriginal = nullptr;
        RegLoadKeyAFn g_regLoadKeyAOriginal = nullptr;
        RegSaveKeyWFn g_regSaveKeyWOriginal = nullptr;
        RegSaveKeyAFn g_regSaveKeyAOriginal = nullptr;
        RegRenameKeyFn g_regRenameKeyOriginal = nullptr;
        RegEnumKeyExWFn g_regEnumKeyExWOriginal = nullptr;
        RegEnumKeyExAFn g_regEnumKeyExAOriginal = nullptr;
        RegEnumValueWFn g_regEnumValueWOriginal = nullptr;
        RegEnumValueAFn g_regEnumValueAOriginal = nullptr;
        RegCloseKeyFn g_regCloseKeyOriginal = nullptr;
        RegQueryInfoKeyWFn g_regQueryInfoKeyWOriginal = nullptr;
        RegQueryInfoKeyAFn g_regQueryInfoKeyAOriginal = nullptr;
        RegFlushKeyFn g_regFlushKeyOriginal = nullptr;
        RegDeleteKeyValueWFn g_regDeleteKeyValueWOriginal = nullptr;
        RegDeleteKeyValueAFn g_regDeleteKeyValueAOriginal = nullptr;
        RegConnectRegistryWFn g_regConnectRegistryWOriginal = nullptr;
        RegConnectRegistryAFn g_regConnectRegistryAOriginal = nullptr;
        ConnectFn g_connectOriginal = nullptr;
        WSAConnectFn g_wsaConnectOriginal = nullptr;
        SendFn g_sendOriginal = nullptr;
        WSASendFn g_wsaSendOriginal = nullptr;
        SendToFn g_sendToOriginal = nullptr;
        RecvFn g_recvOriginal = nullptr;
        WSARecvFn g_wsaRecvOriginal = nullptr;
        RecvFromFn g_recvFromOriginal = nullptr;
        BindFn g_bindOriginal = nullptr;
        ListenFn g_listenOriginal = nullptr;
        AcceptFn g_acceptOriginal = nullptr;
        SocketFn g_socketOriginal = nullptr;
        WSASocketWFn g_wsaSocketWOriginal = nullptr;
        WSASocketAFn g_wsaSocketAOriginal = nullptr;
        CloseSocketFn g_closeSocketOriginal = nullptr;
        ShutdownFn g_shutdownOriginal = nullptr;
        NtCreateFileFn g_ntCreateFileOriginal = nullptr;
        NtOpenFileFn g_ntOpenFileOriginal = nullptr;
        NtReadFileFn g_ntReadFileOriginal = nullptr;
        NtWriteFileFn g_ntWriteFileOriginal = nullptr;
        NtSetInformationFileFn g_ntSetInformationFileOriginal = nullptr;
        NtQueryInformationFileFn g_ntQueryInformationFileOriginal = nullptr;
        NtDeleteFileFn g_ntDeleteFileOriginal = nullptr;
        NtQueryAttributesFileFn g_ntQueryAttributesFileOriginal = nullptr;
        NtQueryFullAttributesFileFn g_ntQueryFullAttributesFileOriginal = nullptr;
        NtDeviceIoControlFileFn g_ntDeviceIoControlFileOriginal = nullptr;
        NtFsControlFileFn g_ntFsControlFileOriginal = nullptr;
        NtQueryDirectoryFileFn g_ntQueryDirectoryFileOriginal = nullptr;
        NtQueryDirectoryFileExFn g_ntQueryDirectoryFileExOriginal = nullptr;
        NtCreateKeyFn g_ntCreateKeyOriginal = nullptr;
        NtOpenKeyFn g_ntOpenKeyOriginal = nullptr;
        NtOpenKeyExFn g_ntOpenKeyExOriginal = nullptr;
        NtSetValueKeyFn g_ntSetValueKeyOriginal = nullptr;
        NtQueryValueKeyFn g_ntQueryValueKeyOriginal = nullptr;
        NtEnumerateKeyFn g_ntEnumerateKeyOriginal = nullptr;
        NtEnumerateValueKeyFn g_ntEnumerateValueKeyOriginal = nullptr;
        NtDeleteKeyFn g_ntDeleteKeyOriginal = nullptr;
        NtDeleteValueKeyFn g_ntDeleteValueKeyOriginal = nullptr;
        NtFlushKeyFn g_ntFlushKeyOriginal = nullptr;
        NtRenameKeyFn g_ntRenameKeyOriginal = nullptr;
        NtLoadKeyFn g_ntLoadKeyOriginal = nullptr;
        NtSaveKeyFn g_ntSaveKeyOriginal = nullptr;
        NtQueryKeyFn g_ntQueryKeyOriginal = nullptr;
        NtQueryMultipleValueKeyFn g_ntQueryMultipleValueKeyOriginal = nullptr;
        NtNotifyChangeKeyFn g_ntNotifyChangeKeyOriginal = nullptr;
        NtLoadKey2Fn g_ntLoadKey2Original = nullptr;
        NtSaveKeyExFn g_ntSaveKeyExOriginal = nullptr;
        NtLoadDriverFn g_ntLoadDriverOriginal = nullptr;
        NtUnloadDriverFn g_ntUnloadDriverOriginal = nullptr;
        NtOpenProcessFn g_ntOpenProcessOriginal = nullptr;
        NtOpenThreadFn g_ntOpenThreadOriginal = nullptr;
        NtTerminateProcessFn g_ntTerminateProcessOriginal = nullptr;
        NtCreateUserProcessFn g_ntCreateUserProcessOriginal = nullptr;
        NtCreateProcessExFn g_ntCreateProcessExOriginal = nullptr;
        NtCreateThreadExFn g_ntCreateThreadExOriginal = nullptr;
        NtAllocateVirtualMemoryFn g_ntAllocateVirtualMemoryOriginal = nullptr;
        NtFreeVirtualMemoryFn g_ntFreeVirtualMemoryOriginal = nullptr;
        NtProtectVirtualMemoryFn g_ntProtectVirtualMemoryOriginal = nullptr;
        NtWriteVirtualMemoryFn g_ntWriteVirtualMemoryOriginal = nullptr;
        NtReadVirtualMemoryFn g_ntReadVirtualMemoryOriginal = nullptr;
        NtMapViewOfSectionFn g_ntMapViewOfSectionOriginal = nullptr;
        NtUnmapViewOfSectionFn g_ntUnmapViewOfSectionOriginal = nullptr;
        NtDuplicateObjectFn g_ntDuplicateObjectOriginal = nullptr;
        NtQueryInformationProcessFn g_ntQueryInformationProcessOriginal = nullptr;
        NtSetInformationProcessFn g_ntSetInformationProcessOriginal = nullptr;
        NtQueryVirtualMemoryFn g_ntQueryVirtualMemoryOriginal = nullptr;
        NtCreateSectionFn g_ntCreateSectionOriginal = nullptr;
        NtOpenSectionFn g_ntOpenSectionOriginal = nullptr;
        NtQueueApcThreadFn g_ntQueueApcThreadOriginal = nullptr;
        NtQueueApcThreadExFn g_ntQueueApcThreadExOriginal = nullptr;
        NtSuspendThreadFn g_ntSuspendThreadOriginal = nullptr;
        NtResumeThreadFn g_ntResumeThreadOriginal = nullptr;
        NtGetContextThreadFn g_ntGetContextThreadOriginal = nullptr;
        NtSetContextThreadFn g_ntSetContextThreadOriginal = nullptr;

        thread_local bool g_hookReentryGuard = false;

        class ScopedHookGuard
        {
        public:
            ScopedHookGuard()
            {
                m_bypass = g_hookReentryGuard;
                if (!m_bypass)
                {
                    g_hookReentryGuard = true;
                }
            }

            ~ScopedHookGuard()
            {
                if (!m_bypass)
                {
                    g_hookReentryGuard = false;
                }
            }

            bool bypass() const
            {
                return m_bypass;
            }

        private:
            bool m_bypass = false;
        };

        struct HookBinding
        {
            const wchar_t* moduleName;                              // moduleName：导出所在模块名。
            const char* procName;                                   // procName：导出函数名。
            ks::winapi_monitor::EventCategory categoryValue;        // categoryValue：对应监控分类。
            InlineHookRecord* hookRecord;                           // hookRecord：该 API 对应的 Hook 状态记录。
            void* detourAddress;                                    // detourAddress：Detour 函数地址。
            void** originalOut;                                     // originalOut：Trampoline 返回地址。
        };

        std::wstring ProcNameToWide(const char* procNamePointer)
        {
            if (procNamePointer == nullptr)
            {
                return std::wstring();
            }

            std::wstring wideText;
            while (*procNamePointer != '\0')
            {
                wideText.push_back(static_cast<wchar_t>(*procNamePointer));
                ++procNamePointer;
            }
            return wideText;
        }

        void AppendHookFailureText(
            std::wstring* detailTextOut,
            const HookBinding& bindingValue,
            const InlineHookInstallResult installResult,
            const std::wstring& errorText)
        {
            if (detailTextOut == nullptr)
            {
                return;
            }

            if (!detailTextOut->empty())
            {
                detailTextOut->append(L" | ");
            }

            detailTextOut->append(bindingValue.moduleName != nullptr ? bindingValue.moduleName : L"<module>");
            detailTextOut->append(L"!");
            detailTextOut->append(ProcNameToWide(bindingValue.procName));
            detailTextOut->append(
                installResult == InlineHookInstallResult::RetryableFailure
                ? L": retryable failure - "
                : L": disabled - ");
            detailTextOut->append(errorText.empty() ? L"unknown reason" : errorText);
        }

        std::wstring SafeWideText(const wchar_t* textPointer)
        {
            return textPointer != nullptr ? std::wstring(textPointer) : std::wstring();
        }

        // AnsiToWide 作用：
        // - 输入：textPointer 为可空 ANSI/系统代码页字符串；
        // - 处理：用 CP_ACP 转成宽字符串，失败时退化为逐字节扩展；
        // - 返回：用于 UI 展示的宽字符串，不抛出异常。
        std::wstring AnsiToWide(const char* const textPointer)
        {
            if (textPointer == nullptr)
            {
                return std::wstring();
            }

            const int requiredChars = ::MultiByteToWideChar(CP_ACP, 0, textPointer, -1, nullptr, 0);
            if (requiredChars > 0)
            {
                std::wstring wideText(static_cast<std::size_t>(requiredChars), L'\0');
                const int convertedChars = ::MultiByteToWideChar(
                    CP_ACP,
                    0,
                    textPointer,
                    -1,
                    wideText.data(),
                    requiredChars);
                if (convertedChars > 0 && !wideText.empty())
                {
                    wideText.resize(static_cast<std::size_t>(convertedChars - 1));
                    return wideText;
                }
            }

            std::wstring fallbackText;
            for (const unsigned char* scanPointer = reinterpret_cast<const unsigned char*>(textPointer);
                *scanPointer != '\0';
                ++scanPointer)
            {
                fallbackText.push_back(static_cast<wchar_t>(*scanPointer));
            }
            return fallbackText;
        }

        // UnicodeStringToWide 作用：
        // - 输入：unicodePointer 为可空 UNICODE_STRING；
        // - 处理：按 Length 字节数复制 Buffer，不要求源字符串 NUL 结尾；
        // - 返回：用于 Nt* 事件详情的宽字符串。
        std::wstring UnicodeStringToWide(const UNICODE_STRING* const unicodePointer)
        {
            if (unicodePointer == nullptr
                || unicodePointer->Buffer == nullptr
                || unicodePointer->Length == 0)
            {
                return std::wstring();
            }

            return std::wstring(
                unicodePointer->Buffer,
                unicodePointer->Buffer + (unicodePointer->Length / sizeof(wchar_t)));
        }

        std::wstring HexValue(const std::uint64_t value)
        {
            wchar_t textBuffer[32] = {};
            ::swprintf_s(textBuffer, L"0x%llX", static_cast<unsigned long long>(value));
            return std::wstring(textBuffer);
        }

        std::wstring HandleText(const HANDLE handleValue)
        {
            return HexValue(reinterpret_cast<std::uint64_t>(handleValue));
        }

        // AppendWideText 作用：
        // - 输入：targetBuffer 为栈上定长缓冲，textPointer 为可空宽字符串；
        // - 处理：从当前 NUL 结尾处追加最多 maxInputChars 个字符，溢出时安全截断；
        // - 返回：无返回值，调用者直接使用 targetBuffer。
        template <std::size_t kCount>
        void AppendWideText(
            wchar_t(&targetBuffer)[kCount],
            const wchar_t* textPointer,
            const std::size_t maxInputChars = static_cast<std::size_t>(-1))
        {
            if (kCount == 0 || textPointer == nullptr)
            {
                return;
            }

            std::size_t writeOffset = 0;
            while (writeOffset < kCount && targetBuffer[writeOffset] != L'\0')
            {
                ++writeOffset;
            }
            if (writeOffset >= kCount)
            {
                targetBuffer[kCount - 1] = L'\0';
                return;
            }

            std::size_t inputOffset = 0;
            while (writeOffset + 1 < kCount
                && inputOffset < maxInputChars
                && textPointer[inputOffset] != L'\0')
            {
                targetBuffer[writeOffset++] = textPointer[inputOffset++];
            }
            targetBuffer[writeOffset] = L'\0';
        }

        // AppendAnsiText 作用：
        // - 输入：targetBuffer 为宽字符详情缓冲，textPointer 为可空窄字符串；
        // - 处理：逐字节扩展为 wchar_t 并安全截断，避免 Hook 热路径调用堆分配转换；
        // - 返回：无返回值，目标缓冲保存尽力追加后的 NUL 结尾文本。
        template <std::size_t kCount>
        void AppendAnsiText(
            wchar_t(&targetBuffer)[kCount],
            const char* const textPointer,
            const std::size_t maxInputChars = static_cast<std::size_t>(-1))
        {
            if (kCount == 0 || textPointer == nullptr)
            {
                return;
            }

            std::size_t writeOffset = 0;
            while (writeOffset < kCount && targetBuffer[writeOffset] != L'\0')
            {
                ++writeOffset;
            }
            if (writeOffset >= kCount)
            {
                targetBuffer[kCount - 1] = L'\0';
                return;
            }

            std::size_t inputOffset = 0;
            while (writeOffset + 1 < kCount
                && inputOffset < maxInputChars
                && textPointer[inputOffset] != '\0')
            {
                targetBuffer[writeOffset++] = static_cast<unsigned char>(textPointer[inputOffset++]);
            }
            targetBuffer[writeOffset] = L'\0';
        }

        // AppendUnsignedText 前置声明：
        // - 输入：固定宽字符详情缓冲和无符号整数；
        // - 处理：真实实现位于下方，供前面的模板 helper 在实例化时可见；
        // - 返回：无返回值，声明本身不改变运行逻辑。
        template <std::size_t kCount>
        void AppendUnsignedText(wchar_t(&targetBuffer)[kCount], const unsigned long long value);

        // AppendHexText 前置声明：
        // - 输入：固定宽字符详情缓冲和十六进制值；
        // - 处理：解决模板两阶段查找中 AppendObjectNameText 先引用后定义的问题；
        // - 返回：无返回值，真实格式化逻辑仍由下方实现完成。
        template <std::size_t kCount>
        void AppendHexText(wchar_t(&targetBuffer)[kCount], const std::uint64_t value);

        // AppendUnicodeStringText 作用：
        // - 输入：unicodePointer 为可空 UNICODE_STRING；
        // - 处理：按 Length 限定追加，不依赖 Buffer 以 NUL 结尾；
        // - 返回：无返回值，适合 NtCreateFile/NtOpenKey 等 Nt* 详情拼接。
        template <std::size_t kCount>
        void AppendUnicodeStringText(
            wchar_t(&targetBuffer)[kCount],
            const UNICODE_STRING* const unicodePointer)
        {
            if (unicodePointer == nullptr
                || unicodePointer->Buffer == nullptr
                || unicodePointer->Length == 0)
            {
                return;
            }

            AppendWideText(
                targetBuffer,
                unicodePointer->Buffer,
                static_cast<std::size_t>(unicodePointer->Length / sizeof(wchar_t)));
        }

        // AppendObjectNameText 作用：
        // - 输入：objectAttributesPointer 为 Nt* OBJECT_ATTRIBUTES；
        // - 处理：追加 ObjectName 文本或句柄根提示，避免解引用其它复杂内核对象状态；
        // - 返回：无返回值，目标缓冲包含 path=<name> 或 rootHandle=<handle>。
        template <std::size_t kCount>
        void AppendObjectNameText(
            wchar_t(&targetBuffer)[kCount],
            const OBJECT_ATTRIBUTES* const objectAttributesPointer)
        {
            if (objectAttributesPointer == nullptr)
            {
                AppendWideText(targetBuffer, L"<null>");
                return;
            }

            if (objectAttributesPointer->RootDirectory != nullptr)
            {
                AppendWideText(targetBuffer, L"root=");
                AppendHexText(targetBuffer, reinterpret_cast<std::uint64_t>(objectAttributesPointer->RootDirectory));
                AppendWideText(targetBuffer, L"\\");
            }
            AppendUnicodeStringText(targetBuffer, objectAttributesPointer->ObjectName);
        }

        // AppendUnsignedText 作用：
        // - 输入：value 为要追加的无符号整数；
        // - 处理：先格式化到小栈缓冲，再追加到目标缓冲；
        // - 返回：无返回值，失败时保持已有文本并追加空串。
        template <std::size_t kCount>
        void AppendUnsignedText(wchar_t(&targetBuffer)[kCount], const unsigned long long value)
        {
            wchar_t numberBuffer[32] = {};
            (void)::swprintf_s(numberBuffer, L"%llu", value);
            AppendWideText(targetBuffer, numberBuffer);
        }

        // AppendHexText 作用：
        // - 输入：value 为要追加的指针/掩码值；
        // - 处理：格式化为 0x 前缀十六进制字符串；
        // - 返回：无返回值，目标缓冲空间不足时安全截断。
        template <std::size_t kCount>
        void AppendHexText(wchar_t(&targetBuffer)[kCount], const std::uint64_t value)
        {
            wchar_t numberBuffer[32] = {};
            (void)::swprintf_s(numberBuffer, L"0x%llX", static_cast<unsigned long long>(value));
            AppendWideText(targetBuffer, numberBuffer);
        }

        // AppendRegistryRootText 作用：
        // - 输入：rootKey 为注册表根键或普通 HKEY 句柄；
        // - 处理：常见根键输出 HKxx，普通句柄输出十六进制；
        // - 返回：无返回值，追加到调用者提供的详情缓冲。
        template <std::size_t kCount>
        void AppendRegistryRootText(wchar_t(&targetBuffer)[kCount], const HKEY rootKey)
        {
            if (rootKey == HKEY_CLASSES_ROOT) { AppendWideText(targetBuffer, L"HKCR"); return; }
            if (rootKey == HKEY_CURRENT_USER) { AppendWideText(targetBuffer, L"HKCU"); return; }
            if (rootKey == HKEY_LOCAL_MACHINE) { AppendWideText(targetBuffer, L"HKLM"); return; }
            if (rootKey == HKEY_USERS) { AppendWideText(targetBuffer, L"HKU"); return; }
            if (rootKey == HKEY_CURRENT_CONFIG) { AppendWideText(targetBuffer, L"HKCC"); return; }
            AppendHexText(targetBuffer, reinterpret_cast<std::uint64_t>(rootKey));
        }

        // BuildRegOpenDetail 作用：
        // - 输入：注册表打开 API 的关键参数；
        // - 处理：在固定栈缓冲中拼接 key/sam 详情，不触发堆分配；
        // - 返回：无返回值，detailBuffer 保存可直接发送的 NUL 结尾文本。
        template <std::size_t kCount>
        void BuildRegOpenDetail(
            wchar_t(&detailBuffer)[kCount],
            const HKEY rootKey,
            const wchar_t* const subKeyPointer,
            const REGSAM samDesired)
        {
            detailBuffer[0] = L'\0';
            AppendWideText(detailBuffer, L"key=");
            AppendRegistryRootText(detailBuffer, rootKey);
            AppendWideText(detailBuffer, L"\\");
            AppendWideText(detailBuffer, subKeyPointer);
            AppendWideText(detailBuffer, L" sam=");
            AppendHexText(detailBuffer, static_cast<std::uint64_t>(samDesired));
        }

        // BuildRegOpenDetailA 作用：
        // - 输入：ANSI 注册表打开 API 的根键、子键和访问掩码；
        // - 处理：根键用 HKxx/句柄表示，子键逐字节扩展为宽字符；
        // - 返回：无返回值，detailBuffer 保存可直接发送的详情文本。
        template <std::size_t kCount>
        void BuildRegOpenDetailA(
            wchar_t(&detailBuffer)[kCount],
            const HKEY rootKey,
            const char* const subKeyPointer,
            const REGSAM samDesired)
        {
            detailBuffer[0] = L'\0';
            AppendWideText(detailBuffer, L"key=");
            AppendRegistryRootText(detailBuffer, rootKey);
            AppendWideText(detailBuffer, L"\\");
            AppendAnsiText(detailBuffer, subKeyPointer);
            AppendWideText(detailBuffer, L" sam=");
            AppendHexText(detailBuffer, static_cast<std::uint64_t>(samDesired));
        }

        // BuildRegCreateDetail 作用：
        // - 输入：注册表创建 API 的关键参数与 disposition；
        // - 处理：只记录稳定小字段，避免在注册表锁上下文中做复杂解析；
        // - 返回：无返回值，detailBuffer 保存固定长度详情。
        template <std::size_t kCount>
        void BuildRegCreateDetail(
            wchar_t(&detailBuffer)[kCount],
            const HKEY rootKey,
            const wchar_t* const subKeyPointer,
            const DWORD optionsValue,
            const DWORD dispositionValue)
        {
            detailBuffer[0] = L'\0';
            AppendWideText(detailBuffer, L"key=");
            AppendRegistryRootText(detailBuffer, rootKey);
            AppendWideText(detailBuffer, L"\\");
            AppendWideText(detailBuffer, subKeyPointer);
            AppendWideText(detailBuffer, L" options=");
            AppendHexText(detailBuffer, static_cast<std::uint64_t>(optionsValue));
            AppendWideText(detailBuffer, L" disposition=");
            AppendUnsignedText(detailBuffer, static_cast<unsigned long long>(dispositionValue));
        }

        // BuildRegCreateDetailA 作用：
        // - 输入：ANSI 注册表创建 API 的关键参数；
        // - 处理：保持与 W 版相同字段，避免 UI 侧需要额外解析；
        // - 返回：无返回值，detailBuffer 保存固定长度摘要。
        template <std::size_t kCount>
        void BuildRegCreateDetailA(
            wchar_t(&detailBuffer)[kCount],
            const HKEY rootKey,
            const char* const subKeyPointer,
            const DWORD optionsValue,
            const DWORD dispositionValue)
        {
            detailBuffer[0] = L'\0';
            AppendWideText(detailBuffer, L"key=");
            AppendRegistryRootText(detailBuffer, rootKey);
            AppendWideText(detailBuffer, L"\\");
            AppendAnsiText(detailBuffer, subKeyPointer);
            AppendWideText(detailBuffer, L" options=");
            AppendHexText(detailBuffer, static_cast<std::uint64_t>(optionsValue));
            AppendWideText(detailBuffer, L" disposition=");
            AppendUnsignedText(detailBuffer, static_cast<unsigned long long>(dispositionValue));
        }

        // BuildRegSetValueDetail 作用：
        // - 输入：注册表写值 API 的句柄、值名、类型和数据长度；
        // - 处理：不读取 dataPointer 内容，避免触发页错误或复制敏感大数据；
        // - 返回：无返回值，detailBuffer 保存可发送的摘要文本。
        template <std::size_t kCount>
        void BuildRegSetValueDetail(
            wchar_t(&detailBuffer)[kCount],
            const HKEY keyHandle,
            const wchar_t* const valueNamePointer,
            const DWORD typeValue,
            const DWORD dataSize)
        {
            detailBuffer[0] = L'\0';
            AppendWideText(detailBuffer, L"hkey=");
            AppendHexText(detailBuffer, reinterpret_cast<std::uint64_t>(keyHandle));
            AppendWideText(detailBuffer, L" value=");
            AppendWideText(detailBuffer, valueNamePointer);
            AppendWideText(detailBuffer, L" type=");
            AppendUnsignedText(detailBuffer, static_cast<unsigned long long>(typeValue));
            AppendWideText(detailBuffer, L" size=");
            AppendUnsignedText(detailBuffer, static_cast<unsigned long long>(dataSize));
        }

        // BuildRegSetValueDetailA 作用：
        // - 输入：ANSI 注册表写值 API 的句柄、值名、类型和长度；
        // - 处理：只记录值名和元数据，不复制 dataPointer；
        // - 返回：无返回值，detailBuffer 保存可发送的摘要。
        template <std::size_t kCount>
        void BuildRegSetValueDetailA(
            wchar_t(&detailBuffer)[kCount],
            const HKEY keyHandle,
            const char* const valueNamePointer,
            const DWORD typeValue,
            const DWORD dataSize)
        {
            detailBuffer[0] = L'\0';
            AppendWideText(detailBuffer, L"hkey=");
            AppendHexText(detailBuffer, reinterpret_cast<std::uint64_t>(keyHandle));
            AppendWideText(detailBuffer, L" value=");
            AppendAnsiText(detailBuffer, valueNamePointer);
            AppendWideText(detailBuffer, L" type=");
            AppendUnsignedText(detailBuffer, static_cast<unsigned long long>(typeValue));
            AppendWideText(detailBuffer, L" size=");
            AppendUnsignedText(detailBuffer, static_cast<unsigned long long>(dataSize));
        }

        // BuildRegValueDetail 作用：
        // - 输入：注册表值操作常见参数；
        // - 处理：拼接 hkey/value/type/size 摘要，适用于查询、删除和枚举值；
        // - 返回：无返回值，detailBuffer 保存 NUL 结尾详情。
        template <std::size_t kCount>
        void BuildRegValueDetail(
            wchar_t(&detailBuffer)[kCount],
            const wchar_t* const prefixText,
            const HKEY keyHandle,
            const wchar_t* const valueNamePointer,
            const DWORD typeValue,
            const DWORD dataSize)
        {
            detailBuffer[0] = L'\0';
            AppendWideText(detailBuffer, prefixText != nullptr ? prefixText : L"hkey=");
            AppendHexText(detailBuffer, reinterpret_cast<std::uint64_t>(keyHandle));
            AppendWideText(detailBuffer, L" value=");
            AppendWideText(detailBuffer, valueNamePointer);
            AppendWideText(detailBuffer, L" type=");
            AppendUnsignedText(detailBuffer, static_cast<unsigned long long>(typeValue));
            AppendWideText(detailBuffer, L" size=");
            AppendUnsignedText(detailBuffer, static_cast<unsigned long long>(dataSize));
        }

        // BuildRegValueDetailA 作用：
        // - 输入：ANSI 注册表值操作参数；
        // - 处理：拼接 hkey/value/type/size 摘要，值名按窄字符扩展；
        // - 返回：无返回值，detailBuffer 保存 NUL 结尾文本。
        template <std::size_t kCount>
        void BuildRegValueDetailA(
            wchar_t(&detailBuffer)[kCount],
            const wchar_t* const prefixText,
            const HKEY keyHandle,
            const char* const valueNamePointer,
            const DWORD typeValue,
            const DWORD dataSize)
        {
            detailBuffer[0] = L'\0';
            AppendWideText(detailBuffer, prefixText != nullptr ? prefixText : L"hkey=");
            AppendHexText(detailBuffer, reinterpret_cast<std::uint64_t>(keyHandle));
            AppendWideText(detailBuffer, L" value=");
            AppendAnsiText(detailBuffer, valueNamePointer);
            AppendWideText(detailBuffer, L" type=");
            AppendUnsignedText(detailBuffer, static_cast<unsigned long long>(typeValue));
            AppendWideText(detailBuffer, L" size=");
            AppendUnsignedText(detailBuffer, static_cast<unsigned long long>(dataSize));
        }

        // BuildRegGetValueDetail 作用：
        // - 输入：RegGetValueW 的 hkey/subkey/value/flags/type/size；
        // - 处理：拼接查询来源和输出摘要，不读取返回数据内容；
        // - 返回：无返回值，detailBuffer 保存 NUL 结尾文本。
        template <std::size_t kCount>
        void BuildRegGetValueDetail(
            wchar_t(&detailBuffer)[kCount],
            const HKEY keyHandle,
            const wchar_t* const subKeyPointer,
            const wchar_t* const valueNamePointer,
            const DWORD flagsValue,
            const DWORD typeValue,
            const DWORD dataSize)
        {
            detailBuffer[0] = L'\0';
            AppendWideText(detailBuffer, L"hkey=");
            AppendHexText(detailBuffer, reinterpret_cast<std::uint64_t>(keyHandle));
            AppendWideText(detailBuffer, L" subkey=");
            AppendWideText(detailBuffer, subKeyPointer);
            AppendWideText(detailBuffer, L" value=");
            AppendWideText(detailBuffer, valueNamePointer);
            AppendWideText(detailBuffer, L" flags=");
            AppendHexText(detailBuffer, static_cast<std::uint64_t>(flagsValue));
            AppendWideText(detailBuffer, L" type=");
            AppendUnsignedText(detailBuffer, static_cast<unsigned long long>(typeValue));
            AppendWideText(detailBuffer, L" size=");
            AppendUnsignedText(detailBuffer, static_cast<unsigned long long>(dataSize));
        }

        // BuildRegGetValueDetailA 作用：
        // - 输入：RegGetValueA 的 hkey/subkey/value/flags/type/size；
        // - 处理：字段与 W 版保持一致，字符串按 ANSI 扩展；
        // - 返回：无返回值，detailBuffer 保存 NUL 结尾文本。
        template <std::size_t kCount>
        void BuildRegGetValueDetailA(
            wchar_t(&detailBuffer)[kCount],
            const HKEY keyHandle,
            const char* const subKeyPointer,
            const char* const valueNamePointer,
            const DWORD flagsValue,
            const DWORD typeValue,
            const DWORD dataSize)
        {
            detailBuffer[0] = L'\0';
            AppendWideText(detailBuffer, L"hkey=");
            AppendHexText(detailBuffer, reinterpret_cast<std::uint64_t>(keyHandle));
            AppendWideText(detailBuffer, L" subkey=");
            AppendAnsiText(detailBuffer, subKeyPointer);
            AppendWideText(detailBuffer, L" value=");
            AppendAnsiText(detailBuffer, valueNamePointer);
            AppendWideText(detailBuffer, L" flags=");
            AppendHexText(detailBuffer, static_cast<std::uint64_t>(flagsValue));
            AppendWideText(detailBuffer, L" type=");
            AppendUnsignedText(detailBuffer, static_cast<unsigned long long>(typeValue));
            AppendWideText(detailBuffer, L" size=");
            AppendUnsignedText(detailBuffer, static_cast<unsigned long long>(dataSize));
        }

        // BuildRegSubKeyDetail 作用：
        // - 输入：注册表子键操作的根键、子键名和访问掩码；
        // - 处理：输出 key=<root>\<subkey> view=<sam> 的短文本；
        // - 返回：无返回值，目标缓冲空间不足时自动截断。
        template <std::size_t kCount>
        void BuildRegSubKeyDetail(
            wchar_t(&detailBuffer)[kCount],
            const HKEY rootKey,
            const wchar_t* const subKeyPointer,
            const REGSAM viewValue)
        {
            detailBuffer[0] = L'\0';
            AppendWideText(detailBuffer, L"key=");
            AppendRegistryRootText(detailBuffer, rootKey);
            AppendWideText(detailBuffer, L"\\");
            AppendWideText(detailBuffer, subKeyPointer);
            AppendWideText(detailBuffer, L" view=");
            AppendHexText(detailBuffer, static_cast<std::uint64_t>(viewValue));
        }

        // BuildRegSubKeyDetailA 作用：
        // - 输入：ANSI 子键操作的根键、子键和视图掩码；
        // - 处理：输出 key=<root>\<subkey> view=<sam>；
        // - 返回：无返回值，目标缓冲空间不足时安全截断。
        template <std::size_t kCount>
        void BuildRegSubKeyDetailA(
            wchar_t(&detailBuffer)[kCount],
            const HKEY rootKey,
            const char* const subKeyPointer,
            const REGSAM viewValue)
        {
            detailBuffer[0] = L'\0';
            AppendWideText(detailBuffer, L"key=");
            AppendRegistryRootText(detailBuffer, rootKey);
            AppendWideText(detailBuffer, L"\\");
            AppendAnsiText(detailBuffer, subKeyPointer);
            AppendWideText(detailBuffer, L" view=");
            AppendHexText(detailBuffer, static_cast<std::uint64_t>(viewValue));
        }

        // BuildRegEnumKeyDetail 作用：
        // - 输入：枚举子键结果中的索引、名称和名称长度；
        // - 处理：输出 hkey/index/name/nameLen 摘要；
        // - 返回：无返回值，失败时名称可能为空但仍保留索引。
        template <std::size_t kCount>
        void BuildRegEnumKeyDetail(
            wchar_t(&detailBuffer)[kCount],
            const HKEY keyHandle,
            const DWORD indexValue,
            const wchar_t* const namePointer,
            const DWORD nameLength)
        {
            detailBuffer[0] = L'\0';
            AppendWideText(detailBuffer, L"hkey=");
            AppendHexText(detailBuffer, reinterpret_cast<std::uint64_t>(keyHandle));
            AppendWideText(detailBuffer, L" index=");
            AppendUnsignedText(detailBuffer, static_cast<unsigned long long>(indexValue));
            AppendWideText(detailBuffer, L" name=");
            AppendWideText(detailBuffer, namePointer, nameLength);
            AppendWideText(detailBuffer, L" nameLen=");
            AppendUnsignedText(detailBuffer, static_cast<unsigned long long>(nameLength));
        }

        // BuildRegEnumKeyDetailA 作用：
        // - 输入：RegEnumKeyExA 的句柄、索引、名称和名称长度；
        // - 处理：保留枚举索引和返回名称，名称按 ANSI 字节扩展；
        // - 返回：无返回值，detailBuffer 保存枚举摘要。
        template <std::size_t kCount>
        void BuildRegEnumKeyDetailA(
            wchar_t(&detailBuffer)[kCount],
            const HKEY keyHandle,
            const DWORD indexValue,
            const char* const namePointer,
            const DWORD nameLength)
        {
            detailBuffer[0] = L'\0';
            AppendWideText(detailBuffer, L"hkey=");
            AppendHexText(detailBuffer, reinterpret_cast<std::uint64_t>(keyHandle));
            AppendWideText(detailBuffer, L" index=");
            AppendUnsignedText(detailBuffer, static_cast<unsigned long long>(indexValue));
            AppendWideText(detailBuffer, L" name=");
            AppendAnsiText(detailBuffer, namePointer, nameLength);
            AppendWideText(detailBuffer, L" nameLen=");
            AppendUnsignedText(detailBuffer, static_cast<unsigned long long>(nameLength));
        }

        // BuildFilePathDetailW 作用：
        // - 输入：pathPointer 为可空宽路径，其它字段为文件 API 元数据；
        // - 处理：生成 path/access/share/disposition/flags 统一摘要；
        // - 返回：无返回值，detailBuffer 保存文件事件详情。
        template <std::size_t kCount>
        void BuildFilePathDetailW(
            wchar_t(&detailBuffer)[kCount],
            const wchar_t* const pathPointer,
            const DWORD desiredAccess,
            const DWORD shareMode,
            const DWORD creationDisposition,
            const DWORD flagsAndAttributes,
            const HANDLE resultHandle)
        {
            detailBuffer[0] = L'\0';
            AppendWideText(detailBuffer, L"path=");
            AppendWideText(detailBuffer, pathPointer);
            AppendWideText(detailBuffer, L" access=");
            AppendHexText(detailBuffer, desiredAccess);
            AppendWideText(detailBuffer, L" share=");
            AppendHexText(detailBuffer, shareMode);
            AppendWideText(detailBuffer, L" disposition=");
            AppendUnsignedText(detailBuffer, creationDisposition);
            AppendWideText(detailBuffer, L" flags=");
            AppendHexText(detailBuffer, flagsAndAttributes);
            AppendWideText(detailBuffer, L" handle=");
            AppendHexText(detailBuffer, reinterpret_cast<std::uint64_t>(resultHandle));
        }

        // BuildFilePathDetailA 作用：
        // - 输入：pathPointer 为可空 ANSI 路径，其它字段为文件 API 元数据；
        // - 处理：与 W 版字段保持一致，路径按 ANSI 字节扩展；
        // - 返回：无返回值，detailBuffer 保存文件事件详情。
        template <std::size_t kCount>
        void BuildFilePathDetailA(
            wchar_t(&detailBuffer)[kCount],
            const char* const pathPointer,
            const DWORD desiredAccess,
            const DWORD shareMode,
            const DWORD creationDisposition,
            const DWORD flagsAndAttributes,
            const HANDLE resultHandle)
        {
            detailBuffer[0] = L'\0';
            AppendWideText(detailBuffer, L"path=");
            AppendAnsiText(detailBuffer, pathPointer);
            AppendWideText(detailBuffer, L" access=");
            AppendHexText(detailBuffer, desiredAccess);
            AppendWideText(detailBuffer, L" share=");
            AppendHexText(detailBuffer, shareMode);
            AppendWideText(detailBuffer, L" disposition=");
            AppendUnsignedText(detailBuffer, creationDisposition);
            AppendWideText(detailBuffer, L" flags=");
            AppendHexText(detailBuffer, flagsAndAttributes);
            AppendWideText(detailBuffer, L" handle=");
            AppendHexText(detailBuffer, reinterpret_cast<std::uint64_t>(resultHandle));
        }

        // BuildTwoPathDetailW 作用：
        // - 输入：sourcePointer/targetPointer 为移动、复制等双路径 API 的路径；
        // - 处理：输出 src/dst/flags 的短摘要；
        // - 返回：无返回值，detailBuffer 保存可发送文本。
        template <std::size_t kCount>
        void BuildTwoPathDetailW(
            wchar_t(&detailBuffer)[kCount],
            const wchar_t* const sourcePointer,
            const wchar_t* const targetPointer,
            const DWORD flagsValue)
        {
            detailBuffer[0] = L'\0';
            AppendWideText(detailBuffer, L"src=");
            AppendWideText(detailBuffer, sourcePointer);
            AppendWideText(detailBuffer, L" dst=");
            AppendWideText(detailBuffer, targetPointer);
            AppendWideText(detailBuffer, L" flags=");
            AppendHexText(detailBuffer, flagsValue);
        }

        // BuildTwoPathDetailA 作用：
        // - 输入：sourcePointer/targetPointer 为 ANSI 双路径 API 的路径；
        // - 处理：输出 src/dst/flags，路径按窄字符扩展；
        // - 返回：无返回值，detailBuffer 保存可发送文本。
        template <std::size_t kCount>
        void BuildTwoPathDetailA(
            wchar_t(&detailBuffer)[kCount],
            const char* const sourcePointer,
            const char* const targetPointer,
            const DWORD flagsValue)
        {
            detailBuffer[0] = L'\0';
            AppendWideText(detailBuffer, L"src=");
            AppendAnsiText(detailBuffer, sourcePointer);
            AppendWideText(detailBuffer, L" dst=");
            AppendAnsiText(detailBuffer, targetPointer);
            AppendWideText(detailBuffer, L" flags=");
            AppendHexText(detailBuffer, flagsValue);
        }

        // BuildSinglePathDetailW 作用：
        // - 输入：pathPointer 为可空宽路径，flagsValue 为可选参数；
        // - 处理：输出 path/flags 摘要；
        // - 返回：无返回值，detailBuffer 保存 NUL 结尾文本。
        template <std::size_t kCount>
        void BuildSinglePathDetailW(
            wchar_t(&detailBuffer)[kCount],
            const wchar_t* const pathPointer,
            const DWORD flagsValue)
        {
            detailBuffer[0] = L'\0';
            AppendWideText(detailBuffer, L"path=");
            AppendWideText(detailBuffer, pathPointer);
            AppendWideText(detailBuffer, L" flags=");
            AppendHexText(detailBuffer, flagsValue);
        }

        // BuildSinglePathDetailA 作用：
        // - 输入：pathPointer 为可空 ANSI 路径，flagsValue 为可选参数；
        // - 处理：输出 path/flags 摘要，路径按窄字符扩展；
        // - 返回：无返回值，detailBuffer 保存 NUL 结尾文本。
        template <std::size_t kCount>
        void BuildSinglePathDetailA(
            wchar_t(&detailBuffer)[kCount],
            const char* const pathPointer,
            const DWORD flagsValue)
        {
            detailBuffer[0] = L'\0';
            AppendWideText(detailBuffer, L"path=");
            AppendAnsiText(detailBuffer, pathPointer);
            AppendWideText(detailBuffer, L" flags=");
            AppendHexText(detailBuffer, flagsValue);
        }

        // BuildHandleTransferDetail 作用：
        // - 输入：handleValue、请求字节数、实际字节数和可选偏移；
        // - 处理：为 NtReadFile/NtWriteFile 生成统一详情；
        // - 返回：无返回值，detailBuffer 保存 NUL 结尾文本。
        template <std::size_t kCount>
        void BuildHandleTransferDetail(
            wchar_t(&detailBuffer)[kCount],
            const HANDLE handleValue,
            const unsigned long requestLength,
            const unsigned long long actualLength,
            const LARGE_INTEGER* const byteOffsetPointer)
        {
            detailBuffer[0] = L'\0';
            AppendWideText(detailBuffer, L"handle=");
            AppendHexText(detailBuffer, reinterpret_cast<std::uint64_t>(handleValue));
            AppendWideText(detailBuffer, L" request=");
            AppendUnsignedText(detailBuffer, requestLength);
            AppendWideText(detailBuffer, L" transferred=");
            AppendUnsignedText(detailBuffer, actualLength);
            if (byteOffsetPointer != nullptr)
            {
                AppendWideText(detailBuffer, L" offset=");
                AppendUnsignedText(detailBuffer, static_cast<unsigned long long>(
                    byteOffsetPointer->QuadPart < 0 ? 0 : byteOffsetPointer->QuadPart));
            }
        }

        // BuildNtObjectPathDetail 作用：
        // - 输入：Nt* OBJECT_ATTRIBUTES 与若干掩码字段；
        // - 处理：输出 path/access/share/options/disposition；
        // - 返回：无返回值，detailBuffer 保存底层对象路径摘要。
        template <std::size_t kCount>
        void BuildNtObjectPathDetail(
            wchar_t(&detailBuffer)[kCount],
            const OBJECT_ATTRIBUTES* const objectAttributesPointer,
            const ACCESS_MASK desiredAccess,
            const ULONG shareAccess,
            const ULONG createDisposition,
            const ULONG createOptions)
        {
            detailBuffer[0] = L'\0';
            AppendWideText(detailBuffer, L"path=");
            AppendObjectNameText(detailBuffer, objectAttributesPointer);
            AppendWideText(detailBuffer, L" access=");
            AppendHexText(detailBuffer, desiredAccess);
            AppendWideText(detailBuffer, L" share=");
            AppendHexText(detailBuffer, shareAccess);
            AppendWideText(detailBuffer, L" disposition=");
            AppendUnsignedText(detailBuffer, createDisposition);
            AppendWideText(detailBuffer, L" options=");
            AppendHexText(detailBuffer, createOptions);
        }

        // BuildNtKeyValueDetail 作用：
        // - 输入：keyHandle、valueNamePointer、type/size；
        // - 处理：不读取 value 数据，仅记录名称与元数据；
        // - 返回：无返回值，detailBuffer 保存 Nt 注册表值操作摘要。
        template <std::size_t kCount>
        void BuildNtKeyValueDetail(
            wchar_t(&detailBuffer)[kCount],
            const HANDLE keyHandle,
            const UNICODE_STRING* const valueNamePointer,
            const ULONG typeValue,
            const ULONG dataSize)
        {
            detailBuffer[0] = L'\0';
            AppendWideText(detailBuffer, L"hkey=");
            AppendHexText(detailBuffer, reinterpret_cast<std::uint64_t>(keyHandle));
            AppendWideText(detailBuffer, L" value=");
            AppendUnicodeStringText(detailBuffer, valueNamePointer);
            AppendWideText(detailBuffer, L" type=");
            AppendUnsignedText(detailBuffer, typeValue);
            AppendWideText(detailBuffer, L" size=");
            AppendUnsignedText(detailBuffer, dataSize);
        }

        // BuildProcessHandleDetail 作用：
        // - 输入：进程句柄、访问掩码、PID 和返回句柄；
        // - 处理：统一输出 process/access/pid/handle 字段；
        // - 返回：无返回值，detailBuffer 保存进程事件摘要。
        template <std::size_t kCount>
        void BuildProcessHandleDetail(
            wchar_t(&detailBuffer)[kCount],
            const HANDLE processHandle,
            const ACCESS_MASK accessMask,
            const std::uint64_t processId,
            const HANDLE resultHandle)
        {
            detailBuffer[0] = L'\0';
            AppendWideText(detailBuffer, L"process=");
            AppendHexText(detailBuffer, reinterpret_cast<std::uint64_t>(processHandle));
            AppendWideText(detailBuffer, L" access=");
            AppendHexText(detailBuffer, accessMask);
            AppendWideText(detailBuffer, L" pid=");
            AppendUnsignedText(detailBuffer, processId);
            AppendWideText(detailBuffer, L" handle=");
            AppendHexText(detailBuffer, reinterpret_cast<std::uint64_t>(resultHandle));
        }

        // BuildRemoteMemoryDetail 作用：
        // - 输入：目标进程句柄、地址、长度、保护或分配标志；
        // - 处理：为跨进程内存 API 生成稳定字段；
        // - 返回：无返回值，detailBuffer 保存内存操作摘要。
        template <std::size_t kCount>
        void BuildRemoteMemoryDetail(
            wchar_t(&detailBuffer)[kCount],
            const HANDLE processHandle,
            const void* const baseAddress,
            const std::uint64_t sizeValue,
            const ULONG firstFlags,
            const ULONG secondFlags)
        {
            detailBuffer[0] = L'\0';
            AppendWideText(detailBuffer, L"process=");
            AppendHexText(detailBuffer, reinterpret_cast<std::uint64_t>(processHandle));
            AppendWideText(detailBuffer, L" base=");
            AppendHexText(detailBuffer, reinterpret_cast<std::uint64_t>(baseAddress));
            AppendWideText(detailBuffer, L" size=");
            AppendUnsignedText(detailBuffer, sizeValue);
            AppendWideText(detailBuffer, L" flags=");
            AppendHexText(detailBuffer, firstFlags);
            AppendWideText(detailBuffer, L" protect=");
            AppendHexText(detailBuffer, secondFlags);
        }

        // SumWsaBufferLength 作用：
        // - 输入：Winsock WSABUF 数组和元素数量；
        // - 处理：累加 len 字段并防止空指针访问；
        // - 返回：总请求字节数，超过 uint64 时自然截断到 uint64 范围。
        std::uint64_t SumWsaBufferLength(const WSABUF* const bufferPointer, const DWORD bufferCount)
        {
            std::uint64_t totalLength = 0;
            if (bufferPointer == nullptr)
            {
                return totalLength;
            }

            for (DWORD indexValue = 0; indexValue < bufferCount; ++indexValue)
            {
                totalLength += static_cast<std::uint64_t>(bufferPointer[indexValue].len);
            }
            return totalLength;
        }

        // AppendSocketAddress 作用：
        // - 输入：sockaddr 与长度；
        // - 处理：尽量用 GetNameInfoW 转成 host:port，失败时输出 <unknown>；
        // - 返回：无返回值，直接追加到 detailBuffer。
        template <std::size_t kCount>
        void AppendSocketAddress(wchar_t(&detailBuffer)[kCount], const sockaddr* const addressPointer, const int addressLength)
        {
            if (addressPointer == nullptr || addressLength <= 0)
            {
                AppendWideText(detailBuffer, L"<null>");
                return;
            }

            wchar_t hostBuffer[NI_MAXHOST] = {};
            wchar_t serviceBuffer[NI_MAXSERV] = {};
            const int resultValue = ::GetNameInfoW(
                addressPointer,
                addressLength,
                hostBuffer,
                NI_MAXHOST,
                serviceBuffer,
                NI_MAXSERV,
                NI_NUMERICHOST | NI_NUMERICSERV);
            if (resultValue != 0)
            {
                AppendWideText(detailBuffer, L"<unknown>");
                return;
            }

            AppendWideText(detailBuffer, hostBuffer);
            AppendWideText(detailBuffer, L":");
            AppendWideText(detailBuffer, serviceBuffer);
        }

        // BuildSocketDetail 作用：
        // - 输入：socket、请求长度、实际传输长度、flags 和可选地址；
        // - 处理：生成网络事件摘要，不读取网络缓冲内容；
        // - 返回：无返回值，detailBuffer 保存可发送文本。
        template <std::size_t kCount>
        void BuildSocketDetail(
            wchar_t(&detailBuffer)[kCount],
            const wchar_t* const verbText,
            const SOCKET socketValue,
            const std::uint64_t requestLength,
            const long long transferLength,
            const DWORD flagsValue,
            const sockaddr* const addressPointer = nullptr,
            const int addressLength = 0)
        {
            detailBuffer[0] = L'\0';
            AppendWideText(detailBuffer, L"socket=");
            AppendHexText(detailBuffer, static_cast<std::uint64_t>(socketValue));
            if (verbText != nullptr && verbText[0] != L'\0')
            {
                AppendWideText(detailBuffer, L" ");
                AppendWideText(detailBuffer, verbText);
            }
            AppendWideText(detailBuffer, L" request=");
            AppendUnsignedText(detailBuffer, static_cast<unsigned long long>(requestLength));
            AppendWideText(detailBuffer, L" transferred=");
            AppendUnsignedText(detailBuffer, static_cast<unsigned long long>(transferLength < 0 ? 0 : transferLength));
            AppendWideText(detailBuffer, L" flags=");
            AppendHexText(detailBuffer, static_cast<std::uint64_t>(flagsValue));
            if (addressPointer != nullptr)
            {
                AppendWideText(detailBuffer, L" remote=");
                AppendSocketAddress(detailBuffer, addressPointer, addressLength);
            }
        }

        bool CategoryEnabled(const ks::winapi_monitor::EventCategory categoryValue)
        {
            const MonitorConfig& configValue = ActiveConfig();
            switch (categoryValue)
            {
            case ks::winapi_monitor::EventCategory::File:
                return configValue.enableFile;
            case ks::winapi_monitor::EventCategory::Registry:
                return configValue.enableRegistry;
            case ks::winapi_monitor::EventCategory::Network:
                return configValue.enableNetwork;
            case ks::winapi_monitor::EventCategory::Process:
                return configValue.enableProcess;
            case ks::winapi_monitor::EventCategory::Loader:
                // Loader hook 还承担“后加载模块补装”职责：
                // - enableLoader 控制是否上报 LoadLibrary 事件；
                // - 注册表/网络/Shell32 进程启动模块可能晚于 Agent 注入加载，因此启用这些分类时也要安装加载器 hook。
                return configValue.enableLoader || configValue.enableRegistry || configValue.enableNetwork || configValue.enableProcess;
            default:
                break;
            }
            return true;
        }

        std::wstring TrimDetail(const std::wstring& detailText)
        {
            const std::size_t detailLimit = std::min<std::size_t>(
                ActiveConfig().detailLimitChars,
                ks::winapi_monitor::kMaxDetailChars - 1);
            return detailText.size() > detailLimit ? detailText.substr(0, detailLimit) : detailText;
        }

        std::wstring FormatRegistryRoot(const HKEY rootKey)
        {
            if (rootKey == HKEY_CLASSES_ROOT) { return L"HKCR"; }
            if (rootKey == HKEY_CURRENT_USER) { return L"HKCU"; }
            if (rootKey == HKEY_LOCAL_MACHINE) { return L"HKLM"; }
            if (rootKey == HKEY_USERS) { return L"HKU"; }
            if (rootKey == HKEY_CURRENT_CONFIG) { return L"HKCC"; }
            return HandleText(rootKey);
        }

        std::wstring FormatSocketAddress(const sockaddr* addressPointer, const int addressLength)
        {
            if (addressPointer == nullptr || addressLength <= 0)
            {
                return L"<null>";
            }

            wchar_t hostBuffer[NI_MAXHOST] = {};
            wchar_t serviceBuffer[NI_MAXSERV] = {};
            const int resultValue = ::GetNameInfoW(
                addressPointer,
                addressLength,
                hostBuffer,
                NI_MAXHOST,
                serviceBuffer,
                NI_MAXSERV,
                NI_NUMERICHOST | NI_NUMERICSERV);
            if (resultValue != 0)
            {
                return L"<unknown>";
            }
            return std::wstring(hostBuffer) + L":" + serviceBuffer;
        }

        // SendRawEventWithStatus 作用：
        // - 输入：category/module/api/status/detail 为已格式化事件；
        // - 处理：把 NTSTATUS/HRESULT/DWORD 统一截断为协议 resultCode；
        // - 返回：SendMonitorEventRaw 的返回值。
        bool SendRawEventWithStatus(
            const ks::winapi_monitor::EventCategory categoryValue,
            const wchar_t* const moduleName,
            const wchar_t* const apiName,
            const long statusValue,
            const wchar_t* const detailText)
        {
            return SendMonitorEventRaw(
                categoryValue,
                moduleName,
                apiName,
                static_cast<std::int32_t>(statusValue),
                detailText);
        }

        bool TryInstallBinding(HookBinding& bindingValue, std::wstring* detailTextOut)
        {
            if (!CategoryEnabled(bindingValue.categoryValue)
                || bindingValue.hookRecord->installed
                || bindingValue.hookRecord->permanentlyDisabled)
            {
                return bindingValue.hookRecord->installed;
            }

            std::wstring errorText;
            const InlineHookInstallResult installResult = InstallInlineHook(
                bindingValue.moduleName,
                bindingValue.procName,
                bindingValue.detourAddress,
                bindingValue.hookRecord,
                bindingValue.originalOut,
                &errorText);
            if (installResult == InlineHookInstallResult::Installed)
            {
                return true;
            }
            if (installResult == InlineHookInstallResult::PermanentFailure)
            {
                bindingValue.hookRecord->permanentlyDisabled = true;
            }
            AppendHookFailureText(detailTextOut, bindingValue, installResult, errorText);
            return false;
        }

        // SendLoaderEventIfEnabled 作用：
        // - 输入：LoadLibrary API 名、路径、结果和错误码；
        // - 处理：仅在 UI 勾选加载器分类时上报，避免“补装所需加载器 hook”强制制造事件噪声；
        // - 返回：无返回值，保留调用者负责恢复 LastError。
        void SendLoaderEventIfEnabled(
            const wchar_t* const apiName,
            const std::wstring& fileNameText,
            const HMODULE moduleHandle,
            const DWORD lastError,
            const std::wstring& extraText)
        {
            if (!ActiveConfig().enableLoader)
            {
                return;
            }

            SendMonitorEvent(
                ks::winapi_monitor::EventCategory::Loader,
                L"Kernel32",
                apiName,
                moduleHandle != nullptr ? 0 : static_cast<std::int32_t>(lastError),
                TrimDetail(L"path=" + fileNameText + extraText));
        }

        // WriteChildMonitorConfig 作用：
        // - 输入：childPidValue 为新子进程 PID，configValue 为父进程当前监控配置；
        // - 处理：生成子进程专属 INI，沿用当前分类开关、DLL 路径和自动注入策略；
        // - 返回：成功写入返回 true，失败返回 false 并填充 errorTextOut。
        bool WriteChildMonitorConfig(
            const DWORD childPidValue,
            const MonitorConfig& configValue,
            std::wstring* const errorTextOut)
        {
            if (errorTextOut != nullptr)
            {
                errorTextOut->clear();
            }
            if (childPidValue == 0 || configValue.agentDllPath.empty())
            {
                if (errorTextOut != nullptr)
                {
                    *errorTextOut = L"child pid or agent dll path is empty.";
                }
                return false;
            }

            const std::wstring sessionDirectory = ks::winapi_monitor::buildSessionDirectory();
            (void)::CreateDirectoryW(sessionDirectory.c_str(), nullptr);

            const std::wstring childConfigPath = ks::winapi_monitor::buildConfigPathForPid(childPidValue);
            const std::wstring childStopPath = ks::winapi_monitor::buildStopFlagPathForPid(childPidValue);
            (void)::DeleteFileW(childStopPath.c_str());

            HANDLE fileHandle = ::CreateFileW(
                childConfigPath.c_str(),
                GENERIC_WRITE,
                FILE_SHARE_READ,
                nullptr,
                CREATE_ALWAYS,
                FILE_ATTRIBUTE_NORMAL,
                nullptr);
            if (fileHandle == INVALID_HANDLE_VALUE)
            {
                if (errorTextOut != nullptr)
                {
                    *errorTextOut = L"CreateFileW child config failed. error=" + std::to_wstring(::GetLastError());
                }
                return false;
            }

            const std::wstring configText =
                L"[monitor]\r\n"
                L"pipe_name=" + ks::winapi_monitor::buildPipeNameForPid(childPidValue) + L"\r\n"
                L"stop_flag_path=" + childStopPath + L"\r\n"
                L"agent_dll_path=" + configValue.agentDllPath + L"\r\n"
                L"enable_file=" + std::to_wstring(configValue.enableFile ? 1 : 0) + L"\r\n"
                L"enable_registry=" + std::to_wstring(configValue.enableRegistry ? 1 : 0) + L"\r\n"
                L"enable_network=" + std::to_wstring(configValue.enableNetwork ? 1 : 0) + L"\r\n"
                L"enable_process=" + std::to_wstring(configValue.enableProcess ? 1 : 0) + L"\r\n"
                L"enable_loader=" + std::to_wstring(configValue.enableLoader ? 1 : 0) + L"\r\n"
                L"auto_inject_child=" + std::to_wstring(configValue.autoInjectChild ? 1 : 0) + L"\r\n"
                L"detail_limit=" + std::to_wstring(configValue.detailLimitChars) + L"\r\n";

            const wchar_t unicodeBom = static_cast<wchar_t>(0xFEFF);
            DWORD bomBytesWritten = 0;
            const BOOL bomWriteOk = ::WriteFile(
                fileHandle,
                &unicodeBom,
                sizeof(unicodeBom),
                &bomBytesWritten,
                nullptr);

            DWORD bytesWritten = 0;
            const BOOL writeOk = ::WriteFile(
                fileHandle,
                configText.data(),
                static_cast<DWORD>(configText.size() * sizeof(wchar_t)),
                &bytesWritten,
                nullptr);
            const DWORD writeError = ::GetLastError();
            ::CloseHandle(fileHandle);

            if (bomWriteOk == FALSE
                || bomBytesWritten != sizeof(unicodeBom)
                || writeOk == FALSE
                || bytesWritten != configText.size() * sizeof(wchar_t))
            {
                if (errorTextOut != nullptr)
                {
                    *errorTextOut = L"WriteFile child config failed. error=" + std::to_wstring(writeError);
                }
                return false;
            }
            return true;
        }

        // InjectAgentIntoChildProcess 作用：
        // - 输入：childPidValue 为子进程 PID，dllPath 为 APIMonitor_x64.dll 路径；
        // - 处理：使用 VirtualAllocEx/WriteProcessMemory/CreateRemoteThread(LoadLibraryW) 注入；
        // - 返回：注入成功返回 true，失败返回 false 并填充 errorTextOut。
        bool InjectAgentIntoChildProcess(
            const DWORD childPidValue,
            const std::wstring& dllPath,
            std::wstring* const errorTextOut)
        {
            if (errorTextOut != nullptr)
            {
                errorTextOut->clear();
            }
            if (childPidValue == 0 || dllPath.empty())
            {
                if (errorTextOut != nullptr)
                {
                    *errorTextOut = L"child pid or dll path is empty.";
                }
                return false;
            }

            HANDLE processHandle = ::OpenProcess(
                PROCESS_CREATE_THREAD | PROCESS_QUERY_INFORMATION | PROCESS_VM_OPERATION | PROCESS_VM_WRITE | PROCESS_VM_READ,
                FALSE,
                childPidValue);
            if (processHandle == nullptr)
            {
                if (errorTextOut != nullptr)
                {
                    *errorTextOut = L"OpenProcess child failed. error=" + std::to_wstring(::GetLastError());
                }
                return false;
            }

            const std::size_t byteCount = (dllPath.size() + 1) * sizeof(wchar_t);
            void* remotePathMemory = ::VirtualAllocEx(
                processHandle,
                nullptr,
                byteCount,
                MEM_COMMIT | MEM_RESERVE,
                PAGE_READWRITE);
            if (remotePathMemory == nullptr)
            {
                if (errorTextOut != nullptr)
                {
                    *errorTextOut = L"VirtualAllocEx child failed. error=" + std::to_wstring(::GetLastError());
                }
                ::CloseHandle(processHandle);
                return false;
            }

            SIZE_T bytesWritten = 0;
            const BOOL writeOk = ::WriteProcessMemory(
                processHandle,
                remotePathMemory,
                dllPath.c_str(),
                byteCount,
                &bytesWritten);
            if (writeOk == FALSE || bytesWritten != byteCount)
            {
                if (errorTextOut != nullptr)
                {
                    *errorTextOut = L"WriteProcessMemory child failed. error=" + std::to_wstring(::GetLastError());
                }
                ::VirtualFreeEx(processHandle, remotePathMemory, 0, MEM_RELEASE);
                ::CloseHandle(processHandle);
                return false;
            }

            HMODULE kernelModule = ::GetModuleHandleW(L"kernel32.dll");
            FARPROC loadLibraryPointer = kernelModule != nullptr ? ::GetProcAddress(kernelModule, "LoadLibraryW") : nullptr;
            if (loadLibraryPointer == nullptr)
            {
                if (errorTextOut != nullptr)
                {
                    *errorTextOut = L"GetProcAddress LoadLibraryW failed.";
                }
                ::VirtualFreeEx(processHandle, remotePathMemory, 0, MEM_RELEASE);
                ::CloseHandle(processHandle);
                return false;
            }

            HANDLE remoteThread = ::CreateRemoteThread(
                processHandle,
                nullptr,
                0,
                reinterpret_cast<LPTHREAD_START_ROUTINE>(loadLibraryPointer),
                remotePathMemory,
                0,
                nullptr);
            if (remoteThread == nullptr)
            {
                if (errorTextOut != nullptr)
                {
                    *errorTextOut = L"CreateRemoteThread child failed. error=" + std::to_wstring(::GetLastError());
                }
                ::VirtualFreeEx(processHandle, remotePathMemory, 0, MEM_RELEASE);
                ::CloseHandle(processHandle);
                return false;
            }

            (void)::WaitForSingleObject(remoteThread, 10000);
            DWORD exitCode = 0;
            (void)::GetExitCodeThread(remoteThread, &exitCode);
            ::CloseHandle(remoteThread);
            ::VirtualFreeEx(processHandle, remotePathMemory, 0, MEM_RELEASE);
            ::CloseHandle(processHandle);

            if (exitCode == 0)
            {
                if (errorTextOut != nullptr)
                {
                    *errorTextOut = L"Remote LoadLibraryW returned NULL.";
                }
                return false;
            }
            return true;
        }

        // AutoInjectChildIfRequested 作用：
        // - 输入：CreateProcessW 结果和 PROCESS_INFORMATION；
        // - 处理：配置启用时为子进程写配置并注入当前 Agent；
        // - 返回：无返回值，成功/失败均以内部事件上报。
        void AutoInjectChildIfRequested(
            const BOOL createResult,
            const PROCESS_INFORMATION* const processInfoPointer)
        {
            const MonitorConfig& configValue = ActiveConfig();
            if (createResult == FALSE
                || !configValue.autoInjectChild
                || processInfoPointer == nullptr
                || processInfoPointer->dwProcessId == 0
                || configValue.agentDllPath.empty())
            {
                return;
            }

            std::wstring errorText;
            bool successValue = WriteChildMonitorConfig(processInfoPointer->dwProcessId, configValue, &errorText);
            if (successValue)
            {
                successValue = InjectAgentIntoChildProcess(processInfoPointer->dwProcessId, configValue.agentDllPath, &errorText);
            }

            SendMonitorEvent(
                ks::winapi_monitor::EventCategory::Internal,
                L"Agent",
                successValue ? L"AutoInjectChild" : L"AutoInjectChildFailed",
                successValue ? 0 : 1,
                TrimDetail(
                    L"childPid=" + std::to_wstring(processInfoPointer->dwProcessId)
                    + (successValue ? L" injected" : (L" error=" + errorText))));
        }

        // AutoInjectChildFromCreateProcessAIfRequested 作用：
        // - 输入：CreateProcessA 的返回值和 PROCESS_INFORMATION；
        // - 处理：复用 W 版子进程配置写入与注入逻辑；
        // - 返回：无返回值，成功/失败均由内部事件反映。
        void AutoInjectChildFromCreateProcessAIfRequested(
            const BOOL createResult,
            const PROCESS_INFORMATION* const processInfoPointer)
        {
            AutoInjectChildIfRequested(createResult, processInfoPointer);
        }


        void RetryPendingHooksFromHook();

        // HookedLdrLoadDll 作用：
        // - 输入：ntdll LdrLoadDll 的原始参数；
        // - 处理：先调用原函数，再在成功加载模块后补装延迟 Hook；
        // - 返回：保持原始 NTSTATUS，不改写加载器语义。
        NTSTATUS NTAPI HookedLdrLoadDll(PWSTR searchPathPointer, PULONG dllCharacteristicsPointer, PUNICODE_STRING dllNamePointer, PHANDLE moduleHandlePointer)
        {
            ScopedHookGuard guardValue;
            if (guardValue.bypass())
            {
                return g_ldrLoadDllOriginal(searchPathPointer, dllCharacteristicsPointer, dllNamePointer, moduleHandlePointer);
            }

            const NTSTATUS statusValue = g_ldrLoadDllOriginal(searchPathPointer, dllCharacteristicsPointer, dllNamePointer, moduleHandlePointer);
            if (NT_SUCCESS(statusValue))
            {
                RetryPendingHooksFromHook();
            }
            if (ActiveConfig().enableLoader)
            {
                wchar_t detailBuffer[ks::winapi_monitor::kMaxDetailChars] = {};
                AppendWideText(detailBuffer, L"path=");
                AppendUnicodeStringText(detailBuffer, dllNamePointer);
                AppendWideText(detailBuffer, L" search=");
                AppendWideText(detailBuffer, searchPathPointer);
                AppendWideText(detailBuffer, L" flags=");
                AppendHexText(detailBuffer, dllCharacteristicsPointer != nullptr ? *dllCharacteristicsPointer : 0);
                AppendWideText(detailBuffer, L" handle=");
                AppendHexText(detailBuffer, moduleHandlePointer != nullptr ? reinterpret_cast<std::uint64_t>(*moduleHandlePointer) : 0);
                SendRawEventWithStatus(ks::winapi_monitor::EventCategory::Loader, L"ntdll", L"LdrLoadDll", statusValue, detailBuffer);
            }
            return statusValue;
        }

        HMODULE WINAPI HookedLoadLibraryA(const LPCSTR fileNamePointer)
        {
            ScopedHookGuard guardValue;
            if (guardValue.bypass())
            {
                return g_loadLibraryAOriginal(fileNamePointer);
            }

            const std::wstring fileNameText = AnsiToWide(fileNamePointer);
            HMODULE moduleHandle = g_loadLibraryAOriginal(fileNamePointer);
            const DWORD lastError = ::GetLastError();
            if (moduleHandle != nullptr)
            {
                RetryPendingHooksFromHook();
            }
            SendLoaderEventIfEnabled(L"LoadLibraryA", fileNameText, moduleHandle, lastError, std::wstring());
            ::SetLastError(lastError);
            return moduleHandle;
        }

        HMODULE WINAPI HookedLoadLibraryW(const LPCWSTR fileNamePointer)
        {
            ScopedHookGuard guardValue;
            if (guardValue.bypass())
            {
                return g_loadLibraryWOriginal(fileNamePointer);
            }

            const std::wstring fileNameText = SafeWideText(fileNamePointer);
            HMODULE moduleHandle = g_loadLibraryWOriginal(fileNamePointer);
            const DWORD lastError = ::GetLastError();
            if (moduleHandle != nullptr)
            {
                RetryPendingHooksFromHook();
            }
            SendLoaderEventIfEnabled(L"LoadLibraryW", fileNameText, moduleHandle, lastError, std::wstring());
            ::SetLastError(lastError);
            return moduleHandle;
        }

        HMODULE WINAPI HookedLoadLibraryExA(const LPCSTR fileNamePointer, HANDLE fileHandle, DWORD flagsValue)
        {
            ScopedHookGuard guardValue;
            if (guardValue.bypass())
            {
                return g_loadLibraryExAOriginal(fileNamePointer, fileHandle, flagsValue);
            }

            const std::wstring fileNameText = AnsiToWide(fileNamePointer);
            HMODULE moduleHandle = g_loadLibraryExAOriginal(fileNamePointer, fileHandle, flagsValue);
            const DWORD lastError = ::GetLastError();
            if (moduleHandle != nullptr)
            {
                RetryPendingHooksFromHook();
            }
            SendLoaderEventIfEnabled(L"LoadLibraryExA", fileNameText, moduleHandle, lastError, L" flags=" + HexValue(flagsValue));
            ::SetLastError(lastError);
            return moduleHandle;
        }

        HMODULE WINAPI HookedLoadLibraryExW(const LPCWSTR fileNamePointer, HANDLE fileHandle, DWORD flagsValue)
        {
            ScopedHookGuard guardValue;
            if (guardValue.bypass())
            {
                return g_loadLibraryExWOriginal(fileNamePointer, fileHandle, flagsValue);
            }

            const std::wstring fileNameText = SafeWideText(fileNamePointer);
            HMODULE moduleHandle = g_loadLibraryExWOriginal(fileNamePointer, fileHandle, flagsValue);
            const DWORD lastError = ::GetLastError();
            if (moduleHandle != nullptr)
            {
                RetryPendingHooksFromHook();
            }
            SendLoaderEventIfEnabled(L"LoadLibraryExW", fileNameText, moduleHandle, lastError, L" flags=" + HexValue(flagsValue));
            ::SetLastError(lastError);
            return moduleHandle;
        }

        HANDLE WINAPI HookedCreateFileA(LPCSTR fileNamePointer, DWORD desiredAccess, DWORD shareMode, LPSECURITY_ATTRIBUTES securityAttributes, DWORD creationDisposition, DWORD flagsAndAttributes, HANDLE templateFile)
        {
            ScopedHookGuard guardValue;
            if (guardValue.bypass())
            {
                return g_createFileAOriginal(fileNamePointer, desiredAccess, shareMode, securityAttributes, creationDisposition, flagsAndAttributes, templateFile);
            }

            HANDLE resultHandle = g_createFileAOriginal(fileNamePointer, desiredAccess, shareMode, securityAttributes, creationDisposition, flagsAndAttributes, templateFile);
            const DWORD lastError = ::GetLastError();
            wchar_t detailBuffer[ks::winapi_monitor::kMaxDetailChars] = {};
            BuildFilePathDetailA(detailBuffer, fileNamePointer, desiredAccess, shareMode, creationDisposition, flagsAndAttributes, resultHandle);
            SendRawEventWithStatus(ks::winapi_monitor::EventCategory::File, L"KernelBase", L"CreateFileA", resultHandle != INVALID_HANDLE_VALUE ? 0 : lastError, detailBuffer);
            ::SetLastError(lastError);
            return resultHandle;
        }

        HANDLE WINAPI HookedCreateFileW(LPCWSTR fileNamePointer, DWORD desiredAccess, DWORD shareMode, LPSECURITY_ATTRIBUTES securityAttributes, DWORD creationDisposition, DWORD flagsAndAttributes, HANDLE templateFile)
        {
            ScopedHookGuard guardValue;
            if (guardValue.bypass())
            {
                return g_createFileWOriginal(fileNamePointer, desiredAccess, shareMode, securityAttributes, creationDisposition, flagsAndAttributes, templateFile);
            }

            HANDLE resultHandle = g_createFileWOriginal(fileNamePointer, desiredAccess, shareMode, securityAttributes, creationDisposition, flagsAndAttributes, templateFile);
            const DWORD lastError = ::GetLastError();
            wchar_t detailBuffer[ks::winapi_monitor::kMaxDetailChars] = {};
            BuildFilePathDetailW(detailBuffer, fileNamePointer, desiredAccess, shareMode, creationDisposition, flagsAndAttributes, resultHandle);
            SendRawEventWithStatus(ks::winapi_monitor::EventCategory::File, L"KernelBase", L"CreateFileW", resultHandle != INVALID_HANDLE_VALUE ? 0 : lastError, detailBuffer);
            ::SetLastError(lastError);
            return resultHandle;
        }

        HANDLE WINAPI HookedCreateFile2(LPCWSTR fileNamePointer, DWORD desiredAccess, DWORD shareMode, DWORD creationDisposition, LPCREATEFILE2_EXTENDED_PARAMETERS createExParamsPointer)
        {
            ScopedHookGuard guardValue;
            if (guardValue.bypass())
            {
                return g_createFile2Original(fileNamePointer, desiredAccess, shareMode, creationDisposition, createExParamsPointer);
            }

            HANDLE resultHandle = g_createFile2Original(fileNamePointer, desiredAccess, shareMode, creationDisposition, createExParamsPointer);
            const DWORD lastError = ::GetLastError();
            const DWORD flagsValue = createExParamsPointer != nullptr ? createExParamsPointer->dwFileFlags : 0;
            const DWORD attributesValue = createExParamsPointer != nullptr ? createExParamsPointer->dwFileAttributes : 0;
            wchar_t detailBuffer[ks::winapi_monitor::kMaxDetailChars] = {};
            BuildFilePathDetailW(detailBuffer, fileNamePointer, desiredAccess, shareMode, creationDisposition, flagsValue | attributesValue, resultHandle);
            SendRawEventWithStatus(ks::winapi_monitor::EventCategory::File, L"KernelBase", L"CreateFile2", resultHandle != INVALID_HANDLE_VALUE ? 0 : lastError, detailBuffer);
            ::SetLastError(lastError);
            return resultHandle;
        }

        BOOL WINAPI HookedReadFile(HANDLE fileHandle, LPVOID bufferPointer, DWORD bytesToRead, LPDWORD bytesReadPointer, LPOVERLAPPED overlappedPointer)
        {
            if (IsMonitorPipeHandle(fileHandle))
            {
                return g_readFileOriginal(fileHandle, bufferPointer, bytesToRead, bytesReadPointer, overlappedPointer);
            }
            ScopedHookGuard guardValue;
            if (guardValue.bypass())
            {
                return g_readFileOriginal(fileHandle, bufferPointer, bytesToRead, bytesReadPointer, overlappedPointer);
            }

            const BOOL resultValue = g_readFileOriginal(fileHandle, bufferPointer, bytesToRead, bytesReadPointer, overlappedPointer);
            const DWORD lastError = ::GetLastError();
            wchar_t detailBuffer[ks::winapi_monitor::kMaxDetailChars] = {};
            BuildHandleTransferDetail(detailBuffer, fileHandle, bytesToRead, bytesReadPointer != nullptr ? *bytesReadPointer : 0, nullptr);
            SendRawEventWithStatus(ks::winapi_monitor::EventCategory::File, L"KernelBase", L"ReadFile", resultValue != FALSE ? 0 : lastError, detailBuffer);
            ::SetLastError(lastError);
            return resultValue;
        }

        BOOL WINAPI HookedWriteFile(HANDLE fileHandle, LPCVOID bufferPointer, DWORD bytesToWrite, LPDWORD bytesWrittenPointer, LPOVERLAPPED overlappedPointer)
        {
            if (IsMonitorPipeHandle(fileHandle))
            {
                return g_writeFileOriginal(fileHandle, bufferPointer, bytesToWrite, bytesWrittenPointer, overlappedPointer);
            }
            ScopedHookGuard guardValue;
            if (guardValue.bypass())
            {
                return g_writeFileOriginal(fileHandle, bufferPointer, bytesToWrite, bytesWrittenPointer, overlappedPointer);
            }

            const BOOL resultValue = g_writeFileOriginal(fileHandle, bufferPointer, bytesToWrite, bytesWrittenPointer, overlappedPointer);
            const DWORD lastError = ::GetLastError();
            wchar_t detailBuffer[ks::winapi_monitor::kMaxDetailChars] = {};
            BuildHandleTransferDetail(detailBuffer, fileHandle, bytesToWrite, bytesWrittenPointer != nullptr ? *bytesWrittenPointer : 0, nullptr);
            SendRawEventWithStatus(ks::winapi_monitor::EventCategory::File, L"KernelBase", L"WriteFile", resultValue != FALSE ? 0 : lastError, detailBuffer);
            ::SetLastError(lastError);
            return resultValue;
        }

        // HookedDeviceIoControl 作用：
        // - 输入：设备句柄、IOCTL 控制码、输入/输出缓冲与重叠结构；
        // - 处理：调用原始 DeviceIoControl 后记录控制码和传输规模，覆盖驱动通信/设备控制行为；
        // - 返回：保持原始 BOOL 结果，并恢复调用者可见的 LastError。
        BOOL WINAPI HookedDeviceIoControl(
            HANDLE deviceHandle,
            DWORD ioControlCode,
            LPVOID inBufferPointer,
            DWORD inBufferSize,
            LPVOID outBufferPointer,
            DWORD outBufferSize,
            LPDWORD bytesReturnedPointer,
            LPOVERLAPPED overlappedPointer)
        {
            ScopedHookGuard guardValue;
            if (guardValue.bypass())
            {
                return g_deviceIoControlOriginal(deviceHandle, ioControlCode, inBufferPointer, inBufferSize, outBufferPointer, outBufferSize, bytesReturnedPointer, overlappedPointer);
            }

            const BOOL resultValue = g_deviceIoControlOriginal(deviceHandle, ioControlCode, inBufferPointer, inBufferSize, outBufferPointer, outBufferSize, bytesReturnedPointer, overlappedPointer);
            const DWORD lastError = ::GetLastError();
            wchar_t detailBuffer[ks::winapi_monitor::kMaxDetailChars] = {};
            AppendWideText(detailBuffer, L"handle=");
            AppendHexText(detailBuffer, reinterpret_cast<std::uint64_t>(deviceHandle));
            AppendWideText(detailBuffer, L" code=");
            AppendHexText(detailBuffer, ioControlCode);
            AppendWideText(detailBuffer, L" in=");
            AppendUnsignedText(detailBuffer, inBufferSize);
            AppendWideText(detailBuffer, L" out=");
            AppendUnsignedText(detailBuffer, outBufferSize);
            AppendWideText(detailBuffer, L" returned=");
            AppendUnsignedText(detailBuffer, bytesReturnedPointer != nullptr ? *bytesReturnedPointer : 0);
            SendRawEventWithStatus(ks::winapi_monitor::EventCategory::File, L"KernelBase", L"DeviceIoControl", resultValue != FALSE ? 0 : lastError, detailBuffer);
            ::SetLastError(lastError);
            return resultValue;
        }

#define APIMON_BOOL_SINGLE_W(Detour, Original, ApiName) \
        BOOL WINAPI Detour(LPCWSTR pathPointer) \
        { \
            ScopedHookGuard guardValue; \
            if (guardValue.bypass()) { return Original(pathPointer); } \
            const BOOL resultValue = Original(pathPointer); \
            const DWORD lastError = ::GetLastError(); \
            wchar_t detailBuffer[ks::winapi_monitor::kMaxDetailChars] = {}; \
            BuildSinglePathDetailW(detailBuffer, pathPointer, 0); \
            SendRawEventWithStatus(ks::winapi_monitor::EventCategory::File, L"KernelBase", ApiName, resultValue != FALSE ? 0 : lastError, detailBuffer); \
            ::SetLastError(lastError); \
            return resultValue; \
        }

#define APIMON_BOOL_SINGLE_A(Detour, Original, ApiName) \
        BOOL WINAPI Detour(LPCSTR pathPointer) \
        { \
            ScopedHookGuard guardValue; \
            if (guardValue.bypass()) { return Original(pathPointer); } \
            const BOOL resultValue = Original(pathPointer); \
            const DWORD lastError = ::GetLastError(); \
            wchar_t detailBuffer[ks::winapi_monitor::kMaxDetailChars] = {}; \
            BuildSinglePathDetailA(detailBuffer, pathPointer, 0); \
            SendRawEventWithStatus(ks::winapi_monitor::EventCategory::File, L"KernelBase", ApiName, resultValue != FALSE ? 0 : lastError, detailBuffer); \
            ::SetLastError(lastError); \
            return resultValue; \
        }

        APIMON_BOOL_SINGLE_W(HookedDeleteFileW, g_deleteFileWOriginal, L"DeleteFileW")
        APIMON_BOOL_SINGLE_A(HookedDeleteFileA, g_deleteFileAOriginal, L"DeleteFileA")
        APIMON_BOOL_SINGLE_W(HookedRemoveDirectoryW, g_removeDirectoryWOriginal, L"RemoveDirectoryW")
        APIMON_BOOL_SINGLE_A(HookedRemoveDirectoryA, g_removeDirectoryAOriginal, L"RemoveDirectoryA")

        BOOL WINAPI HookedMoveFileExW(LPCWSTR existingFileNamePointer, LPCWSTR newFileNamePointer, DWORD flagsValue)
        {
            ScopedHookGuard guardValue;
            if (guardValue.bypass()) { return g_moveFileExWOriginal(existingFileNamePointer, newFileNamePointer, flagsValue); }
            const BOOL resultValue = g_moveFileExWOriginal(existingFileNamePointer, newFileNamePointer, flagsValue);
            const DWORD lastError = ::GetLastError();
            wchar_t detailBuffer[ks::winapi_monitor::kMaxDetailChars] = {};
            BuildTwoPathDetailW(detailBuffer, existingFileNamePointer, newFileNamePointer, flagsValue);
            SendRawEventWithStatus(ks::winapi_monitor::EventCategory::File, L"KernelBase", L"MoveFileExW", resultValue != FALSE ? 0 : lastError, detailBuffer);
            ::SetLastError(lastError);
            return resultValue;
        }

        BOOL WINAPI HookedMoveFileExA(LPCSTR existingFileNamePointer, LPCSTR newFileNamePointer, DWORD flagsValue)
        {
            ScopedHookGuard guardValue;
            if (guardValue.bypass()) { return g_moveFileExAOriginal(existingFileNamePointer, newFileNamePointer, flagsValue); }
            const BOOL resultValue = g_moveFileExAOriginal(existingFileNamePointer, newFileNamePointer, flagsValue);
            const DWORD lastError = ::GetLastError();
            wchar_t detailBuffer[ks::winapi_monitor::kMaxDetailChars] = {};
            BuildTwoPathDetailA(detailBuffer, existingFileNamePointer, newFileNamePointer, flagsValue);
            SendRawEventWithStatus(ks::winapi_monitor::EventCategory::File, L"KernelBase", L"MoveFileExA", resultValue != FALSE ? 0 : lastError, detailBuffer);
            ::SetLastError(lastError);
            return resultValue;
        }

        BOOL WINAPI HookedCopyFileW(LPCWSTR existingFileNamePointer, LPCWSTR newFileNamePointer, BOOL failIfExists)
        {
            ScopedHookGuard guardValue;
            if (guardValue.bypass()) { return g_copyFileWOriginal(existingFileNamePointer, newFileNamePointer, failIfExists); }
            const BOOL resultValue = g_copyFileWOriginal(existingFileNamePointer, newFileNamePointer, failIfExists);
            const DWORD lastError = ::GetLastError();
            wchar_t detailBuffer[ks::winapi_monitor::kMaxDetailChars] = {};
            BuildTwoPathDetailW(detailBuffer, existingFileNamePointer, newFileNamePointer, failIfExists ? 1 : 0);
            SendRawEventWithStatus(ks::winapi_monitor::EventCategory::File, L"KernelBase", L"CopyFileW", resultValue != FALSE ? 0 : lastError, detailBuffer);
            ::SetLastError(lastError);
            return resultValue;
        }

        BOOL WINAPI HookedCopyFileA(LPCSTR existingFileNamePointer, LPCSTR newFileNamePointer, BOOL failIfExists)
        {
            ScopedHookGuard guardValue;
            if (guardValue.bypass()) { return g_copyFileAOriginal(existingFileNamePointer, newFileNamePointer, failIfExists); }
            const BOOL resultValue = g_copyFileAOriginal(existingFileNamePointer, newFileNamePointer, failIfExists);
            const DWORD lastError = ::GetLastError();
            wchar_t detailBuffer[ks::winapi_monitor::kMaxDetailChars] = {};
            BuildTwoPathDetailA(detailBuffer, existingFileNamePointer, newFileNamePointer, failIfExists ? 1 : 0);
            SendRawEventWithStatus(ks::winapi_monitor::EventCategory::File, L"KernelBase", L"CopyFileA", resultValue != FALSE ? 0 : lastError, detailBuffer);
            ::SetLastError(lastError);
            return resultValue;
        }

        BOOL WINAPI HookedCopyFileExW(LPCWSTR existingFileNamePointer, LPCWSTR newFileNamePointer, LPPROGRESS_ROUTINE progressRoutinePointer, LPVOID dataPointer, LPBOOL cancelPointer, DWORD flagsValue)
        {
            ScopedHookGuard guardValue;
            if (guardValue.bypass()) { return g_copyFileExWOriginal(existingFileNamePointer, newFileNamePointer, progressRoutinePointer, dataPointer, cancelPointer, flagsValue); }
            const BOOL resultValue = g_copyFileExWOriginal(existingFileNamePointer, newFileNamePointer, progressRoutinePointer, dataPointer, cancelPointer, flagsValue);
            const DWORD lastError = ::GetLastError();
            wchar_t detailBuffer[ks::winapi_monitor::kMaxDetailChars] = {};
            BuildTwoPathDetailW(detailBuffer, existingFileNamePointer, newFileNamePointer, flagsValue);
            SendRawEventWithStatus(ks::winapi_monitor::EventCategory::File, L"KernelBase", L"CopyFileExW", resultValue != FALSE ? 0 : lastError, detailBuffer);
            ::SetLastError(lastError);
            return resultValue;
        }

        BOOL WINAPI HookedCopyFileExA(LPCSTR existingFileNamePointer, LPCSTR newFileNamePointer, LPPROGRESS_ROUTINE progressRoutinePointer, LPVOID dataPointer, LPBOOL cancelPointer, DWORD flagsValue)
        {
            ScopedHookGuard guardValue;
            if (guardValue.bypass()) { return g_copyFileExAOriginal(existingFileNamePointer, newFileNamePointer, progressRoutinePointer, dataPointer, cancelPointer, flagsValue); }
            const BOOL resultValue = g_copyFileExAOriginal(existingFileNamePointer, newFileNamePointer, progressRoutinePointer, dataPointer, cancelPointer, flagsValue);
            const DWORD lastError = ::GetLastError();
            wchar_t detailBuffer[ks::winapi_monitor::kMaxDetailChars] = {};
            BuildTwoPathDetailA(detailBuffer, existingFileNamePointer, newFileNamePointer, flagsValue);
            SendRawEventWithStatus(ks::winapi_monitor::EventCategory::File, L"KernelBase", L"CopyFileExA", resultValue != FALSE ? 0 : lastError, detailBuffer);
            ::SetLastError(lastError);
            return resultValue;
        }

        DWORD WINAPI HookedGetFileAttributesW(LPCWSTR fileNamePointer)
        {
            ScopedHookGuard guardValue;
            if (guardValue.bypass()) { return g_getFileAttributesWOriginal(fileNamePointer); }
            const DWORD resultValue = g_getFileAttributesWOriginal(fileNamePointer);
            const DWORD lastError = ::GetLastError();
            wchar_t detailBuffer[ks::winapi_monitor::kMaxDetailChars] = {};
            BuildSinglePathDetailW(detailBuffer, fileNamePointer, resultValue);
            SendRawEventWithStatus(ks::winapi_monitor::EventCategory::File, L"KernelBase", L"GetFileAttributesW", resultValue != INVALID_FILE_ATTRIBUTES ? 0 : lastError, detailBuffer);
            ::SetLastError(lastError);
            return resultValue;
        }

        DWORD WINAPI HookedGetFileAttributesA(LPCSTR fileNamePointer)
        {
            ScopedHookGuard guardValue;
            if (guardValue.bypass()) { return g_getFileAttributesAOriginal(fileNamePointer); }
            const DWORD resultValue = g_getFileAttributesAOriginal(fileNamePointer);
            const DWORD lastError = ::GetLastError();
            wchar_t detailBuffer[ks::winapi_monitor::kMaxDetailChars] = {};
            BuildSinglePathDetailA(detailBuffer, fileNamePointer, resultValue);
            SendRawEventWithStatus(ks::winapi_monitor::EventCategory::File, L"KernelBase", L"GetFileAttributesA", resultValue != INVALID_FILE_ATTRIBUTES ? 0 : lastError, detailBuffer);
            ::SetLastError(lastError);
            return resultValue;
        }

        BOOL WINAPI HookedGetFileAttributesExW(LPCWSTR fileNamePointer, GET_FILEEX_INFO_LEVELS infoLevelValue, LPVOID fileInformationPointer)
        {
            ScopedHookGuard guardValue;
            if (guardValue.bypass()) { return g_getFileAttributesExWOriginal(fileNamePointer, infoLevelValue, fileInformationPointer); }
            const BOOL resultValue = g_getFileAttributesExWOriginal(fileNamePointer, infoLevelValue, fileInformationPointer);
            const DWORD lastError = ::GetLastError();
            wchar_t detailBuffer[ks::winapi_monitor::kMaxDetailChars] = {};
            BuildSinglePathDetailW(detailBuffer, fileNamePointer, static_cast<DWORD>(infoLevelValue));
            SendRawEventWithStatus(ks::winapi_monitor::EventCategory::File, L"KernelBase", L"GetFileAttributesExW", resultValue != FALSE ? 0 : lastError, detailBuffer);
            ::SetLastError(lastError);
            return resultValue;
        }

        BOOL WINAPI HookedGetFileAttributesExA(LPCSTR fileNamePointer, GET_FILEEX_INFO_LEVELS infoLevelValue, LPVOID fileInformationPointer)
        {
            ScopedHookGuard guardValue;
            if (guardValue.bypass()) { return g_getFileAttributesExAOriginal(fileNamePointer, infoLevelValue, fileInformationPointer); }
            const BOOL resultValue = g_getFileAttributesExAOriginal(fileNamePointer, infoLevelValue, fileInformationPointer);
            const DWORD lastError = ::GetLastError();
            wchar_t detailBuffer[ks::winapi_monitor::kMaxDetailChars] = {};
            BuildSinglePathDetailA(detailBuffer, fileNamePointer, static_cast<DWORD>(infoLevelValue));
            SendRawEventWithStatus(ks::winapi_monitor::EventCategory::File, L"KernelBase", L"GetFileAttributesExA", resultValue != FALSE ? 0 : lastError, detailBuffer);
            ::SetLastError(lastError);
            return resultValue;
        }

        BOOL WINAPI HookedSetFileAttributesW(LPCWSTR fileNamePointer, DWORD fileAttributes)
        {
            ScopedHookGuard guardValue;
            if (guardValue.bypass()) { return g_setFileAttributesWOriginal(fileNamePointer, fileAttributes); }
            const BOOL resultValue = g_setFileAttributesWOriginal(fileNamePointer, fileAttributes);
            const DWORD lastError = ::GetLastError();
            wchar_t detailBuffer[ks::winapi_monitor::kMaxDetailChars] = {};
            BuildSinglePathDetailW(detailBuffer, fileNamePointer, fileAttributes);
            SendRawEventWithStatus(ks::winapi_monitor::EventCategory::File, L"KernelBase", L"SetFileAttributesW", resultValue != FALSE ? 0 : lastError, detailBuffer);
            ::SetLastError(lastError);
            return resultValue;
        }

        BOOL WINAPI HookedSetFileAttributesA(LPCSTR fileNamePointer, DWORD fileAttributes)
        {
            ScopedHookGuard guardValue;
            if (guardValue.bypass()) { return g_setFileAttributesAOriginal(fileNamePointer, fileAttributes); }
            const BOOL resultValue = g_setFileAttributesAOriginal(fileNamePointer, fileAttributes);
            const DWORD lastError = ::GetLastError();
            wchar_t detailBuffer[ks::winapi_monitor::kMaxDetailChars] = {};
            BuildSinglePathDetailA(detailBuffer, fileNamePointer, fileAttributes);
            SendRawEventWithStatus(ks::winapi_monitor::EventCategory::File, L"KernelBase", L"SetFileAttributesA", resultValue != FALSE ? 0 : lastError, detailBuffer);
            ::SetLastError(lastError);
            return resultValue;
        }

        HANDLE WINAPI HookedFindFirstFileExW(LPCWSTR fileNamePointer, FINDEX_INFO_LEVELS infoLevelValue, LPVOID findFileDataPointer, FINDEX_SEARCH_OPS searchOpValue, LPVOID searchFilterPointer, DWORD additionalFlags)
        {
            ScopedHookGuard guardValue;
            if (guardValue.bypass()) { return g_findFirstFileExWOriginal(fileNamePointer, infoLevelValue, findFileDataPointer, searchOpValue, searchFilterPointer, additionalFlags); }
            HANDLE resultHandle = g_findFirstFileExWOriginal(fileNamePointer, infoLevelValue, findFileDataPointer, searchOpValue, searchFilterPointer, additionalFlags);
            const DWORD lastError = ::GetLastError();
            wchar_t detailBuffer[ks::winapi_monitor::kMaxDetailChars] = {};
            BuildSinglePathDetailW(detailBuffer, fileNamePointer, additionalFlags);
            AppendWideText(detailBuffer, L" info=");
            AppendUnsignedText(detailBuffer, static_cast<unsigned long long>(infoLevelValue));
            AppendWideText(detailBuffer, L" search=");
            AppendUnsignedText(detailBuffer, static_cast<unsigned long long>(searchOpValue));
            SendRawEventWithStatus(ks::winapi_monitor::EventCategory::File, L"KernelBase", L"FindFirstFileExW", resultHandle != INVALID_HANDLE_VALUE ? 0 : lastError, detailBuffer);
            ::SetLastError(lastError);
            return resultHandle;
        }

        HANDLE WINAPI HookedFindFirstFileExA(LPCSTR fileNamePointer, FINDEX_INFO_LEVELS infoLevelValue, LPVOID findFileDataPointer, FINDEX_SEARCH_OPS searchOpValue, LPVOID searchFilterPointer, DWORD additionalFlags)
        {
            ScopedHookGuard guardValue;
            if (guardValue.bypass()) { return g_findFirstFileExAOriginal(fileNamePointer, infoLevelValue, findFileDataPointer, searchOpValue, searchFilterPointer, additionalFlags); }
            HANDLE resultHandle = g_findFirstFileExAOriginal(fileNamePointer, infoLevelValue, findFileDataPointer, searchOpValue, searchFilterPointer, additionalFlags);
            const DWORD lastError = ::GetLastError();
            wchar_t detailBuffer[ks::winapi_monitor::kMaxDetailChars] = {};
            BuildSinglePathDetailA(detailBuffer, fileNamePointer, additionalFlags);
            AppendWideText(detailBuffer, L" info=");
            AppendUnsignedText(detailBuffer, static_cast<unsigned long long>(infoLevelValue));
            AppendWideText(detailBuffer, L" search=");
            AppendUnsignedText(detailBuffer, static_cast<unsigned long long>(searchOpValue));
            SendRawEventWithStatus(ks::winapi_monitor::EventCategory::File, L"KernelBase", L"FindFirstFileExA", resultHandle != INVALID_HANDLE_VALUE ? 0 : lastError, detailBuffer);
            ::SetLastError(lastError);
            return resultHandle;
        }

        BOOL WINAPI HookedCreateDirectoryW(LPCWSTR pathNamePointer, LPSECURITY_ATTRIBUTES securityAttributesPointer)
        {
            ScopedHookGuard guardValue;
            if (guardValue.bypass()) { return g_createDirectoryWOriginal(pathNamePointer, securityAttributesPointer); }
            const BOOL resultValue = g_createDirectoryWOriginal(pathNamePointer, securityAttributesPointer);
            const DWORD lastError = ::GetLastError();
            wchar_t detailBuffer[ks::winapi_monitor::kMaxDetailChars] = {};
            BuildSinglePathDetailW(detailBuffer, pathNamePointer, 0);
            SendRawEventWithStatus(ks::winapi_monitor::EventCategory::File, L"KernelBase", L"CreateDirectoryW", resultValue != FALSE ? 0 : lastError, detailBuffer);
            ::SetLastError(lastError);
            return resultValue;
        }

        BOOL WINAPI HookedCreateDirectoryA(LPCSTR pathNamePointer, LPSECURITY_ATTRIBUTES securityAttributesPointer)
        {
            ScopedHookGuard guardValue;
            if (guardValue.bypass()) { return g_createDirectoryAOriginal(pathNamePointer, securityAttributesPointer); }
            const BOOL resultValue = g_createDirectoryAOriginal(pathNamePointer, securityAttributesPointer);
            const DWORD lastError = ::GetLastError();
            wchar_t detailBuffer[ks::winapi_monitor::kMaxDetailChars] = {};
            BuildSinglePathDetailA(detailBuffer, pathNamePointer, 0);
            SendRawEventWithStatus(ks::winapi_monitor::EventCategory::File, L"KernelBase", L"CreateDirectoryA", resultValue != FALSE ? 0 : lastError, detailBuffer);
            ::SetLastError(lastError);
            return resultValue;
        }

        BOOL WINAPI HookedSetFileInformationByHandle(HANDLE fileHandle, FILE_INFO_BY_HANDLE_CLASS fileInformationClass, LPVOID fileInformationPointer, DWORD bufferSize)
        {
            ScopedHookGuard guardValue;
            if (guardValue.bypass()) { return g_setFileInformationByHandleOriginal(fileHandle, fileInformationClass, fileInformationPointer, bufferSize); }
            const BOOL resultValue = g_setFileInformationByHandleOriginal(fileHandle, fileInformationClass, fileInformationPointer, bufferSize);
            const DWORD lastError = ::GetLastError();
            wchar_t detailBuffer[ks::winapi_monitor::kMaxDetailChars] = {};
            AppendWideText(detailBuffer, L"handle=");
            AppendHexText(detailBuffer, reinterpret_cast<std::uint64_t>(fileHandle));
            AppendWideText(detailBuffer, L" class=");
            AppendUnsignedText(detailBuffer, static_cast<unsigned long long>(fileInformationClass));
            AppendWideText(detailBuffer, L" size=");
            AppendUnsignedText(detailBuffer, bufferSize);
            SendRawEventWithStatus(ks::winapi_monitor::EventCategory::File, L"KernelBase", L"SetFileInformationByHandle", resultValue != FALSE ? 0 : lastError, detailBuffer);
            ::SetLastError(lastError);
            return resultValue;
        }

        BOOL WINAPI HookedCreateProcessA(LPCSTR applicationNamePointer, LPSTR commandLinePointer, LPSECURITY_ATTRIBUTES processAttributes, LPSECURITY_ATTRIBUTES threadAttributes, BOOL inheritHandles, DWORD creationFlags, LPVOID environmentPointer, LPCSTR currentDirectoryPointer, LPSTARTUPINFOA startupInfoPointer, LPPROCESS_INFORMATION processInfoPointer)
        {
            ScopedHookGuard guardValue;
            if (guardValue.bypass())
            {
                return g_createProcessAOriginal(applicationNamePointer, commandLinePointer, processAttributes, threadAttributes, inheritHandles, creationFlags, environmentPointer, currentDirectoryPointer, startupInfoPointer, processInfoPointer);
            }

            const std::wstring appNameText = AnsiToWide(applicationNamePointer);
            const std::wstring commandLineText = AnsiToWide(commandLinePointer);
            const std::wstring currentDirectoryText = AnsiToWide(currentDirectoryPointer);
            const BOOL resultValue = g_createProcessAOriginal(applicationNamePointer, commandLinePointer, processAttributes, threadAttributes, inheritHandles, creationFlags, environmentPointer, currentDirectoryPointer, startupInfoPointer, processInfoPointer);
            const DWORD lastError = ::GetLastError();
            const DWORD childPid = (resultValue != FALSE && processInfoPointer != nullptr) ? processInfoPointer->dwProcessId : 0;
            AutoInjectChildFromCreateProcessAIfRequested(resultValue, processInfoPointer);
            SendMonitorEvent(ks::winapi_monitor::EventCategory::Process, L"KernelBase", L"CreateProcessA", resultValue != FALSE ? 0 : static_cast<std::int32_t>(lastError), TrimDetail(L"app=" + appNameText + L" cmd=" + commandLineText + L" cwd=" + currentDirectoryText + L" flags=" + HexValue(creationFlags) + L" inherit=" + std::to_wstring(inheritHandles != FALSE) + L" childPid=" + std::to_wstring(childPid)));
            ::SetLastError(lastError);
            return resultValue;
        }

        BOOL WINAPI HookedCreateProcessW(LPCWSTR applicationNamePointer, LPWSTR commandLinePointer, LPSECURITY_ATTRIBUTES processAttributes, LPSECURITY_ATTRIBUTES threadAttributes, BOOL inheritHandles, DWORD creationFlags, LPVOID environmentPointer, LPCWSTR currentDirectoryPointer, LPSTARTUPINFOW startupInfoPointer, LPPROCESS_INFORMATION processInfoPointer)
        {
            ScopedHookGuard guardValue;
            if (guardValue.bypass())
            {
                return g_createProcessWOriginal(applicationNamePointer, commandLinePointer, processAttributes, threadAttributes, inheritHandles, creationFlags, environmentPointer, currentDirectoryPointer, startupInfoPointer, processInfoPointer);
            }

            const std::wstring appNameText = SafeWideText(applicationNamePointer);
            const std::wstring commandLineText = commandLinePointer != nullptr ? std::wstring(commandLinePointer) : std::wstring();
            const std::wstring currentDirectoryText = SafeWideText(currentDirectoryPointer);
            const BOOL resultValue = g_createProcessWOriginal(applicationNamePointer, commandLinePointer, processAttributes, threadAttributes, inheritHandles, creationFlags, environmentPointer, currentDirectoryPointer, startupInfoPointer, processInfoPointer);
            const DWORD lastError = ::GetLastError();
            const DWORD childPid = (resultValue != FALSE && processInfoPointer != nullptr) ? processInfoPointer->dwProcessId : 0;
            AutoInjectChildIfRequested(resultValue, processInfoPointer);
            SendMonitorEvent(ks::winapi_monitor::EventCategory::Process, L"KernelBase", L"CreateProcessW", resultValue != FALSE ? 0 : static_cast<std::int32_t>(lastError), TrimDetail(L"app=" + appNameText + L" cmd=" + commandLineText + L" cwd=" + currentDirectoryText + L" flags=" + HexValue(creationFlags) + L" inherit=" + std::to_wstring(inheritHandles != FALSE) + L" childPid=" + std::to_wstring(childPid)));
            ::SetLastError(lastError);
            return resultValue;
        }

        HANDLE WINAPI HookedOpenProcess(DWORD desiredAccess, BOOL inheritHandle, DWORD processId)
        {
            ScopedHookGuard guardValue;
            if (guardValue.bypass()) { return g_openProcessOriginal(desiredAccess, inheritHandle, processId); }
            HANDLE resultHandle = g_openProcessOriginal(desiredAccess, inheritHandle, processId);
            const DWORD lastError = ::GetLastError();
            wchar_t detailBuffer[ks::winapi_monitor::kMaxDetailChars] = {};
            BuildProcessHandleDetail(detailBuffer, nullptr, desiredAccess, processId, resultHandle);
            AppendWideText(detailBuffer, L" inherit=");
            AppendUnsignedText(detailBuffer, inheritHandle != FALSE ? 1 : 0);
            SendRawEventWithStatus(ks::winapi_monitor::EventCategory::Process, L"KernelBase", L"OpenProcess", resultHandle != nullptr ? 0 : lastError, detailBuffer);
            ::SetLastError(lastError);
            return resultHandle;
        }

        BOOL WINAPI HookedTerminateProcess(HANDLE processHandle, UINT exitCode)
        {
            ScopedHookGuard guardValue;
            if (guardValue.bypass()) { return g_terminateProcessOriginal(processHandle, exitCode); }
            const BOOL resultValue = g_terminateProcessOriginal(processHandle, exitCode);
            const DWORD lastError = ::GetLastError();
            wchar_t detailBuffer[ks::winapi_monitor::kMaxDetailChars] = {};
            AppendWideText(detailBuffer, L"process=");
            AppendHexText(detailBuffer, reinterpret_cast<std::uint64_t>(processHandle));
            AppendWideText(detailBuffer, L" exit=");
            AppendUnsignedText(detailBuffer, exitCode);
            SendRawEventWithStatus(ks::winapi_monitor::EventCategory::Process, L"KernelBase", L"TerminateProcess", resultValue != FALSE ? 0 : lastError, detailBuffer);
            ::SetLastError(lastError);
            return resultValue;
        }

        HANDLE WINAPI HookedCreateThread(LPSECURITY_ATTRIBUTES threadAttributesPointer, SIZE_T stackSize, LPTHREAD_START_ROUTINE startAddress, LPVOID parameterPointer, DWORD creationFlags, LPDWORD threadIdPointer)
        {
            ScopedHookGuard guardValue;
            if (guardValue.bypass()) { return g_createThreadOriginal(threadAttributesPointer, stackSize, startAddress, parameterPointer, creationFlags, threadIdPointer); }
            HANDLE resultHandle = g_createThreadOriginal(threadAttributesPointer, stackSize, startAddress, parameterPointer, creationFlags, threadIdPointer);
            const DWORD lastError = ::GetLastError();
            wchar_t detailBuffer[ks::winapi_monitor::kMaxDetailChars] = {};
            AppendWideText(detailBuffer, L"start=");
            AppendHexText(detailBuffer, reinterpret_cast<std::uint64_t>(startAddress));
            AppendWideText(detailBuffer, L" param=");
            AppendHexText(detailBuffer, reinterpret_cast<std::uint64_t>(parameterPointer));
            AppendWideText(detailBuffer, L" flags=");
            AppendHexText(detailBuffer, creationFlags);
            AppendWideText(detailBuffer, L" tid=");
            AppendUnsignedText(detailBuffer, threadIdPointer != nullptr ? *threadIdPointer : 0);
            AppendWideText(detailBuffer, L" handle=");
            AppendHexText(detailBuffer, reinterpret_cast<std::uint64_t>(resultHandle));
            SendRawEventWithStatus(ks::winapi_monitor::EventCategory::Process, L"KernelBase", L"CreateThread", resultHandle != nullptr ? 0 : lastError, detailBuffer);
            ::SetLastError(lastError);
            return resultHandle;
        }

        HANDLE WINAPI HookedCreateRemoteThread(HANDLE processHandle, LPSECURITY_ATTRIBUTES threadAttributesPointer, SIZE_T stackSize, LPTHREAD_START_ROUTINE startAddress, LPVOID parameterPointer, DWORD creationFlags, LPDWORD threadIdPointer)
        {
            ScopedHookGuard guardValue;
            if (guardValue.bypass()) { return g_createRemoteThreadOriginal(processHandle, threadAttributesPointer, stackSize, startAddress, parameterPointer, creationFlags, threadIdPointer); }
            HANDLE resultHandle = g_createRemoteThreadOriginal(processHandle, threadAttributesPointer, stackSize, startAddress, parameterPointer, creationFlags, threadIdPointer);
            const DWORD lastError = ::GetLastError();
            wchar_t detailBuffer[ks::winapi_monitor::kMaxDetailChars] = {};
            AppendWideText(detailBuffer, L"process=");
            AppendHexText(detailBuffer, reinterpret_cast<std::uint64_t>(processHandle));
            AppendWideText(detailBuffer, L" start=");
            AppendHexText(detailBuffer, reinterpret_cast<std::uint64_t>(startAddress));
            AppendWideText(detailBuffer, L" param=");
            AppendHexText(detailBuffer, reinterpret_cast<std::uint64_t>(parameterPointer));
            AppendWideText(detailBuffer, L" flags=");
            AppendHexText(detailBuffer, creationFlags);
            AppendWideText(detailBuffer, L" tid=");
            AppendUnsignedText(detailBuffer, threadIdPointer != nullptr ? *threadIdPointer : 0);
            AppendWideText(detailBuffer, L" handle=");
            AppendHexText(detailBuffer, reinterpret_cast<std::uint64_t>(resultHandle));
            SendRawEventWithStatus(ks::winapi_monitor::EventCategory::Process, L"KernelBase", L"CreateRemoteThread", resultHandle != nullptr ? 0 : lastError, detailBuffer);
            ::SetLastError(lastError);
            return resultHandle;
        }

        LPVOID WINAPI HookedVirtualAllocEx(HANDLE processHandle, LPVOID addressPointer, SIZE_T sizeValue, DWORD allocationType, DWORD protectValue)
        {
            ScopedHookGuard guardValue;
            if (guardValue.bypass()) { return g_virtualAllocExOriginal(processHandle, addressPointer, sizeValue, allocationType, protectValue); }
            LPVOID resultPointer = g_virtualAllocExOriginal(processHandle, addressPointer, sizeValue, allocationType, protectValue);
            const DWORD lastError = ::GetLastError();
            wchar_t detailBuffer[ks::winapi_monitor::kMaxDetailChars] = {};
            BuildRemoteMemoryDetail(detailBuffer, processHandle, resultPointer != nullptr ? resultPointer : addressPointer, static_cast<std::uint64_t>(sizeValue), allocationType, protectValue);
            SendRawEventWithStatus(ks::winapi_monitor::EventCategory::Process, L"KernelBase", L"VirtualAllocEx", resultPointer != nullptr ? 0 : lastError, detailBuffer);
            ::SetLastError(lastError);
            return resultPointer;
        }

        BOOL WINAPI HookedVirtualFreeEx(HANDLE processHandle, LPVOID addressPointer, SIZE_T sizeValue, DWORD freeType)
        {
            ScopedHookGuard guardValue;
            if (guardValue.bypass()) { return g_virtualFreeExOriginal(processHandle, addressPointer, sizeValue, freeType); }
            const BOOL resultValue = g_virtualFreeExOriginal(processHandle, addressPointer, sizeValue, freeType);
            const DWORD lastError = ::GetLastError();
            wchar_t detailBuffer[ks::winapi_monitor::kMaxDetailChars] = {};
            BuildRemoteMemoryDetail(detailBuffer, processHandle, addressPointer, static_cast<std::uint64_t>(sizeValue), freeType, 0);
            SendRawEventWithStatus(ks::winapi_monitor::EventCategory::Process, L"KernelBase", L"VirtualFreeEx", resultValue != FALSE ? 0 : lastError, detailBuffer);
            ::SetLastError(lastError);
            return resultValue;
        }

        BOOL WINAPI HookedVirtualProtectEx(HANDLE processHandle, LPVOID addressPointer, SIZE_T sizeValue, DWORD newProtect, PDWORD oldProtectPointer)
        {
            ScopedHookGuard guardValue;
            if (guardValue.bypass()) { return g_virtualProtectExOriginal(processHandle, addressPointer, sizeValue, newProtect, oldProtectPointer); }
            const BOOL resultValue = g_virtualProtectExOriginal(processHandle, addressPointer, sizeValue, newProtect, oldProtectPointer);
            const DWORD lastError = ::GetLastError();
            wchar_t detailBuffer[ks::winapi_monitor::kMaxDetailChars] = {};
            BuildRemoteMemoryDetail(detailBuffer, processHandle, addressPointer, static_cast<std::uint64_t>(sizeValue), oldProtectPointer != nullptr ? *oldProtectPointer : 0, newProtect);
            SendRawEventWithStatus(ks::winapi_monitor::EventCategory::Process, L"KernelBase", L"VirtualProtectEx", resultValue != FALSE ? 0 : lastError, detailBuffer);
            ::SetLastError(lastError);
            return resultValue;
        }

        BOOL WINAPI HookedWriteProcessMemory(HANDLE processHandle, LPVOID baseAddress, LPCVOID bufferPointer, SIZE_T sizeValue, SIZE_T* bytesWrittenPointer)
        {
            ScopedHookGuard guardValue;
            if (guardValue.bypass()) { return g_writeProcessMemoryOriginal(processHandle, baseAddress, bufferPointer, sizeValue, bytesWrittenPointer); }
            const BOOL resultValue = g_writeProcessMemoryOriginal(processHandle, baseAddress, bufferPointer, sizeValue, bytesWrittenPointer);
            const DWORD lastError = ::GetLastError();
            wchar_t detailBuffer[ks::winapi_monitor::kMaxDetailChars] = {};
            BuildRemoteMemoryDetail(detailBuffer, processHandle, baseAddress, static_cast<std::uint64_t>(sizeValue), 0, 0);
            AppendWideText(detailBuffer, L" written=");
            AppendUnsignedText(detailBuffer, static_cast<unsigned long long>(bytesWrittenPointer != nullptr ? *bytesWrittenPointer : 0));
            SendRawEventWithStatus(ks::winapi_monitor::EventCategory::Process, L"KernelBase", L"WriteProcessMemory", resultValue != FALSE ? 0 : lastError, detailBuffer);
            ::SetLastError(lastError);
            return resultValue;
        }

        BOOL WINAPI HookedReadProcessMemory(HANDLE processHandle, LPCVOID baseAddress, LPVOID bufferPointer, SIZE_T sizeValue, SIZE_T* bytesReadPointer)
        {
            ScopedHookGuard guardValue;
            if (guardValue.bypass()) { return g_readProcessMemoryOriginal(processHandle, baseAddress, bufferPointer, sizeValue, bytesReadPointer); }
            const BOOL resultValue = g_readProcessMemoryOriginal(processHandle, baseAddress, bufferPointer, sizeValue, bytesReadPointer);
            const DWORD lastError = ::GetLastError();
            wchar_t detailBuffer[ks::winapi_monitor::kMaxDetailChars] = {};
            BuildRemoteMemoryDetail(detailBuffer, processHandle, baseAddress, static_cast<std::uint64_t>(sizeValue), 0, 0);
            AppendWideText(detailBuffer, L" read=");
            AppendUnsignedText(detailBuffer, static_cast<unsigned long long>(bytesReadPointer != nullptr ? *bytesReadPointer : 0));
            SendRawEventWithStatus(ks::winapi_monitor::EventCategory::Process, L"KernelBase", L"ReadProcessMemory", resultValue != FALSE ? 0 : lastError, detailBuffer);
            ::SetLastError(lastError);
            return resultValue;
        }

        // HookedOpenThread 作用：
        // - 输入：线程访问掩码、继承标志和线程 ID；
        // - 处理：记录线程句柄打开，补齐后续 Suspend/Resume/Context/APC 链路上游；
        // - 返回：保持 OpenThread 原始 HANDLE，并恢复 LastError。
        HANDLE WINAPI HookedOpenThread(DWORD desiredAccess, BOOL inheritHandle, DWORD threadId)
        {
            ScopedHookGuard guardValue;
            if (guardValue.bypass()) { return g_openThreadOriginal(desiredAccess, inheritHandle, threadId); }
            HANDLE resultHandle = g_openThreadOriginal(desiredAccess, inheritHandle, threadId);
            const DWORD lastError = ::GetLastError();
            wchar_t detailBuffer[ks::winapi_monitor::kMaxDetailChars] = {};
            AppendWideText(detailBuffer, L"tid=");
            AppendUnsignedText(detailBuffer, threadId);
            AppendWideText(detailBuffer, L" access=");
            AppendHexText(detailBuffer, desiredAccess);
            AppendWideText(detailBuffer, L" inherit=");
            AppendUnsignedText(detailBuffer, inheritHandle != FALSE ? 1 : 0);
            AppendWideText(detailBuffer, L" handle=");
            AppendHexText(detailBuffer, reinterpret_cast<std::uint64_t>(resultHandle));
            SendRawEventWithStatus(ks::winapi_monitor::EventCategory::Process, L"KernelBase", L"OpenThread", resultHandle != nullptr ? 0 : lastError, detailBuffer);
            ::SetLastError(lastError);
            return resultHandle;
        }

        // HookedSuspendThread 作用：
        // - 输入：目标线程句柄；
        // - 处理：记录线程挂起操作和原始挂起计数，覆盖调试/注入常见控制面；
        // - 返回：保持 SuspendThread 返回值，并恢复 LastError。
        DWORD WINAPI HookedSuspendThread(HANDLE threadHandle)
        {
            ScopedHookGuard guardValue;
            if (guardValue.bypass()) { return g_suspendThreadOriginal(threadHandle); }
            const DWORD resultValue = g_suspendThreadOriginal(threadHandle);
            const DWORD lastError = ::GetLastError();
            wchar_t detailBuffer[ks::winapi_monitor::kMaxDetailChars] = {};
            AppendWideText(detailBuffer, L"thread=");
            AppendHexText(detailBuffer, reinterpret_cast<std::uint64_t>(threadHandle));
            AppendWideText(detailBuffer, L" previous=");
            AppendUnsignedText(detailBuffer, resultValue == static_cast<DWORD>(-1) ? 0 : resultValue);
            SendRawEventWithStatus(ks::winapi_monitor::EventCategory::Process, L"KernelBase", L"SuspendThread", resultValue != static_cast<DWORD>(-1) ? 0 : lastError, detailBuffer);
            ::SetLastError(lastError);
            return resultValue;
        }

        // HookedResumeThread 作用：
        // - 输入：目标线程句柄；
        // - 处理：记录线程恢复操作和原始挂起计数；
        // - 返回：保持 ResumeThread 返回值，并恢复 LastError。
        DWORD WINAPI HookedResumeThread(HANDLE threadHandle)
        {
            ScopedHookGuard guardValue;
            if (guardValue.bypass()) { return g_resumeThreadOriginal(threadHandle); }
            const DWORD resultValue = g_resumeThreadOriginal(threadHandle);
            const DWORD lastError = ::GetLastError();
            wchar_t detailBuffer[ks::winapi_monitor::kMaxDetailChars] = {};
            AppendWideText(detailBuffer, L"thread=");
            AppendHexText(detailBuffer, reinterpret_cast<std::uint64_t>(threadHandle));
            AppendWideText(detailBuffer, L" previous=");
            AppendUnsignedText(detailBuffer, resultValue == static_cast<DWORD>(-1) ? 0 : resultValue);
            SendRawEventWithStatus(ks::winapi_monitor::EventCategory::Process, L"KernelBase", L"ResumeThread", resultValue != static_cast<DWORD>(-1) ? 0 : lastError, detailBuffer);
            ::SetLastError(lastError);
            return resultValue;
        }

        // HookedQueueUserAPC 作用：
        // - 输入：APC 函数地址、线程句柄和参数；
        // - 处理：记录用户态 APC 排队，覆盖 QueueUserAPC 注入路径；
        // - 返回：保持 QueueUserAPC 的 DWORD 结果并恢复 LastError。
        DWORD WINAPI HookedQueueUserAPC(PAPCFUNC apcRoutinePointer, HANDLE threadHandle, ULONG_PTR dataValue)
        {
            ScopedHookGuard guardValue;
            if (guardValue.bypass()) { return g_queueUserAPCOriginal(apcRoutinePointer, threadHandle, dataValue); }
            const DWORD resultValue = g_queueUserAPCOriginal(apcRoutinePointer, threadHandle, dataValue);
            const DWORD lastError = ::GetLastError();
            wchar_t detailBuffer[ks::winapi_monitor::kMaxDetailChars] = {};
            AppendWideText(detailBuffer, L"thread=");
            AppendHexText(detailBuffer, reinterpret_cast<std::uint64_t>(threadHandle));
            AppendWideText(detailBuffer, L" apc=");
            AppendHexText(detailBuffer, reinterpret_cast<std::uint64_t>(apcRoutinePointer));
            AppendWideText(detailBuffer, L" data=");
            AppendHexText(detailBuffer, static_cast<std::uint64_t>(dataValue));
            SendRawEventWithStatus(ks::winapi_monitor::EventCategory::Process, L"KernelBase", L"QueueUserAPC", resultValue != 0 ? 0 : lastError, detailBuffer);
            ::SetLastError(lastError);
            return resultValue;
        }

        // HookedGetThreadContext 作用：
        // - 输入：线程句柄和 CONTEXT 输出缓冲；
        // - 处理：记录线程上下文读取，便于识别调试/劫持前置动作；
        // - 返回：保持原始 BOOL 结果，并恢复 LastError。
        BOOL WINAPI HookedGetThreadContext(HANDLE threadHandle, LPCONTEXT contextPointer)
        {
            ScopedHookGuard guardValue;
            if (guardValue.bypass()) { return g_getThreadContextOriginal(threadHandle, contextPointer); }
            const BOOL resultValue = g_getThreadContextOriginal(threadHandle, contextPointer);
            const DWORD lastError = ::GetLastError();
            wchar_t detailBuffer[ks::winapi_monitor::kMaxDetailChars] = {};
            AppendWideText(detailBuffer, L"thread=");
            AppendHexText(detailBuffer, reinterpret_cast<std::uint64_t>(threadHandle));
            AppendWideText(detailBuffer, L" flags=");
            AppendHexText(detailBuffer, contextPointer != nullptr ? contextPointer->ContextFlags : 0);
            SendRawEventWithStatus(ks::winapi_monitor::EventCategory::Process, L"KernelBase", L"GetThreadContext", resultValue != FALSE ? 0 : lastError, detailBuffer);
            ::SetLastError(lastError);
            return resultValue;
        }

        // HookedSetThreadContext 作用：
        // - 输入：线程句柄和 CONTEXT 输入缓冲；
        // - 处理：记录线程上下文写入，覆盖 SetThreadContext 注入/劫持路径；
        // - 返回：保持原始 BOOL 结果，并恢复 LastError。
        BOOL WINAPI HookedSetThreadContext(HANDLE threadHandle, const CONTEXT* contextPointer)
        {
            ScopedHookGuard guardValue;
            if (guardValue.bypass()) { return g_setThreadContextOriginal(threadHandle, contextPointer); }
            const BOOL resultValue = g_setThreadContextOriginal(threadHandle, contextPointer);
            const DWORD lastError = ::GetLastError();
            wchar_t detailBuffer[ks::winapi_monitor::kMaxDetailChars] = {};
            AppendWideText(detailBuffer, L"thread=");
            AppendHexText(detailBuffer, reinterpret_cast<std::uint64_t>(threadHandle));
            AppendWideText(detailBuffer, L" flags=");
            AppendHexText(detailBuffer, contextPointer != nullptr ? contextPointer->ContextFlags : 0);
#if defined(_M_X64)
            AppendWideText(detailBuffer, L" rip=");
            AppendHexText(detailBuffer, contextPointer != nullptr ? contextPointer->Rip : 0);
#endif
            SendRawEventWithStatus(ks::winapi_monitor::EventCategory::Process, L"KernelBase", L"SetThreadContext", resultValue != FALSE ? 0 : lastError, detailBuffer);
            ::SetLastError(lastError);
            return resultValue;
        }

        UINT WINAPI HookedWinExec(LPCSTR commandLinePointer, UINT showCommand)
        {
            ScopedHookGuard guardValue;
            if (guardValue.bypass()) { return g_winExecOriginal(commandLinePointer, showCommand); }
            const UINT resultValue = g_winExecOriginal(commandLinePointer, showCommand);
            const DWORD lastError = ::GetLastError();
            wchar_t detailBuffer[ks::winapi_monitor::kMaxDetailChars] = {};
            AppendWideText(detailBuffer, L"cmd=");
            AppendAnsiText(detailBuffer, commandLinePointer);
            AppendWideText(detailBuffer, L" show=");
            AppendUnsignedText(detailBuffer, showCommand);
            SendRawEventWithStatus(ks::winapi_monitor::EventCategory::Process, L"Kernel32", L"WinExec", resultValue > 31 ? 0 : resultValue, detailBuffer);
            ::SetLastError(lastError);
            return resultValue;
        }

        BOOL WINAPI HookedShellExecuteExW(SHELLEXECUTEINFOW* executeInfoPointer)
        {
            ScopedHookGuard guardValue;
            if (guardValue.bypass()) { return g_shellExecuteExWOriginal(executeInfoPointer); }
            const BOOL resultValue = g_shellExecuteExWOriginal(executeInfoPointer);
            const DWORD lastError = ::GetLastError();
            wchar_t detailBuffer[ks::winapi_monitor::kMaxDetailChars] = {};
            AppendWideText(detailBuffer, L"file=");
            AppendWideText(detailBuffer, executeInfoPointer != nullptr ? executeInfoPointer->lpFile : nullptr);
            AppendWideText(detailBuffer, L" verb=");
            AppendWideText(detailBuffer, executeInfoPointer != nullptr ? executeInfoPointer->lpVerb : nullptr);
            AppendWideText(detailBuffer, L" params=");
            AppendWideText(detailBuffer, executeInfoPointer != nullptr ? executeInfoPointer->lpParameters : nullptr);
            AppendWideText(detailBuffer, L" process=");
            AppendHexText(detailBuffer, executeInfoPointer != nullptr ? reinterpret_cast<std::uint64_t>(executeInfoPointer->hProcess) : 0);
            SendRawEventWithStatus(ks::winapi_monitor::EventCategory::Process, L"Shell32", L"ShellExecuteExW", resultValue != FALSE ? 0 : lastError, detailBuffer);
            ::SetLastError(lastError);
            return resultValue;
        }

        BOOL WINAPI HookedShellExecuteExA(SHELLEXECUTEINFOA* executeInfoPointer)
        {
            ScopedHookGuard guardValue;
            if (guardValue.bypass()) { return g_shellExecuteExAOriginal(executeInfoPointer); }
            const BOOL resultValue = g_shellExecuteExAOriginal(executeInfoPointer);
            const DWORD lastError = ::GetLastError();
            wchar_t detailBuffer[ks::winapi_monitor::kMaxDetailChars] = {};
            AppendWideText(detailBuffer, L"file=");
            AppendAnsiText(detailBuffer, executeInfoPointer != nullptr ? executeInfoPointer->lpFile : nullptr);
            AppendWideText(detailBuffer, L" verb=");
            AppendAnsiText(detailBuffer, executeInfoPointer != nullptr ? executeInfoPointer->lpVerb : nullptr);
            AppendWideText(detailBuffer, L" params=");
            AppendAnsiText(detailBuffer, executeInfoPointer != nullptr ? executeInfoPointer->lpParameters : nullptr);
            AppendWideText(detailBuffer, L" process=");
            AppendHexText(detailBuffer, executeInfoPointer != nullptr ? reinterpret_cast<std::uint64_t>(executeInfoPointer->hProcess) : 0);
            SendRawEventWithStatus(ks::winapi_monitor::EventCategory::Process, L"Shell32", L"ShellExecuteExA", resultValue != FALSE ? 0 : lastError, detailBuffer);
            ::SetLastError(lastError);
            return resultValue;
        }


#define APIMON_REG_SEND(ApiName, StatusValue, DetailBuffer) \
        SendMonitorEventRaw(ks::winapi_monitor::EventCategory::Registry, L"Advapi32", ApiName, static_cast<std::int32_t>(StatusValue), DetailBuffer)

        LSTATUS WINAPI HookedRegOpenKeyW(HKEY rootKey, LPCWSTR subKeyPointer, PHKEY resultKeyPointer)
        {
            ScopedHookGuard guardValue;
            if (guardValue.bypass()) { return g_regOpenKeyWOriginal(rootKey, subKeyPointer, resultKeyPointer); }
            const LSTATUS statusValue = g_regOpenKeyWOriginal(rootKey, subKeyPointer, resultKeyPointer);
            const DWORD lastError = ::GetLastError();
            wchar_t detailBuffer[ks::winapi_monitor::kMaxDetailChars] = {};
            BuildRegOpenDetail(detailBuffer, rootKey, subKeyPointer, 0);
            APIMON_REG_SEND(L"RegOpenKeyW", statusValue, detailBuffer);
            ::SetLastError(lastError);
            return statusValue;
        }

        LSTATUS WINAPI HookedRegOpenKeyA(HKEY rootKey, LPCSTR subKeyPointer, PHKEY resultKeyPointer)
        {
            ScopedHookGuard guardValue;
            if (guardValue.bypass()) { return g_regOpenKeyAOriginal(rootKey, subKeyPointer, resultKeyPointer); }
            const LSTATUS statusValue = g_regOpenKeyAOriginal(rootKey, subKeyPointer, resultKeyPointer);
            const DWORD lastError = ::GetLastError();
            wchar_t detailBuffer[ks::winapi_monitor::kMaxDetailChars] = {};
            BuildRegOpenDetailA(detailBuffer, rootKey, subKeyPointer, 0);
            APIMON_REG_SEND(L"RegOpenKeyA", statusValue, detailBuffer);
            ::SetLastError(lastError);
            return statusValue;
        }

        LSTATUS WINAPI HookedRegOpenKeyExW(HKEY rootKey, LPCWSTR subKeyPointer, DWORD optionsValue, REGSAM samDesired, PHKEY resultKeyPointer)
        {
            ScopedHookGuard guardValue;
            if (guardValue.bypass()) { return g_regOpenKeyExWOriginal(rootKey, subKeyPointer, optionsValue, samDesired, resultKeyPointer); }
            const LSTATUS statusValue = g_regOpenKeyExWOriginal(rootKey, subKeyPointer, optionsValue, samDesired, resultKeyPointer);
            const DWORD lastError = ::GetLastError();
            wchar_t detailBuffer[ks::winapi_monitor::kMaxDetailChars] = {};
            BuildRegOpenDetail(detailBuffer, rootKey, subKeyPointer, samDesired);
            APIMON_REG_SEND(L"RegOpenKeyExW", statusValue, detailBuffer);
            ::SetLastError(lastError);
            return statusValue;
        }

        LSTATUS WINAPI HookedRegOpenKeyExA(HKEY rootKey, LPCSTR subKeyPointer, DWORD optionsValue, REGSAM samDesired, PHKEY resultKeyPointer)
        {
            ScopedHookGuard guardValue;
            if (guardValue.bypass()) { return g_regOpenKeyExAOriginal(rootKey, subKeyPointer, optionsValue, samDesired, resultKeyPointer); }
            const LSTATUS statusValue = g_regOpenKeyExAOriginal(rootKey, subKeyPointer, optionsValue, samDesired, resultKeyPointer);
            const DWORD lastError = ::GetLastError();
            wchar_t detailBuffer[ks::winapi_monitor::kMaxDetailChars] = {};
            BuildRegOpenDetailA(detailBuffer, rootKey, subKeyPointer, samDesired);
            APIMON_REG_SEND(L"RegOpenKeyExA", statusValue, detailBuffer);
            ::SetLastError(lastError);
            return statusValue;
        }

        LSTATUS WINAPI HookedRegCreateKeyW(HKEY rootKey, LPCWSTR subKeyPointer, PHKEY resultKeyPointer)
        {
            ScopedHookGuard guardValue;
            if (guardValue.bypass()) { return g_regCreateKeyWOriginal(rootKey, subKeyPointer, resultKeyPointer); }
            const LSTATUS statusValue = g_regCreateKeyWOriginal(rootKey, subKeyPointer, resultKeyPointer);
            const DWORD lastError = ::GetLastError();
            wchar_t detailBuffer[ks::winapi_monitor::kMaxDetailChars] = {};
            BuildRegCreateDetail(detailBuffer, rootKey, subKeyPointer, 0, 0);
            APIMON_REG_SEND(L"RegCreateKeyW", statusValue, detailBuffer);
            ::SetLastError(lastError);
            return statusValue;
        }

        LSTATUS WINAPI HookedRegCreateKeyA(HKEY rootKey, LPCSTR subKeyPointer, PHKEY resultKeyPointer)
        {
            ScopedHookGuard guardValue;
            if (guardValue.bypass()) { return g_regCreateKeyAOriginal(rootKey, subKeyPointer, resultKeyPointer); }
            const LSTATUS statusValue = g_regCreateKeyAOriginal(rootKey, subKeyPointer, resultKeyPointer);
            const DWORD lastError = ::GetLastError();
            wchar_t detailBuffer[ks::winapi_monitor::kMaxDetailChars] = {};
            BuildRegCreateDetailA(detailBuffer, rootKey, subKeyPointer, 0, 0);
            APIMON_REG_SEND(L"RegCreateKeyA", statusValue, detailBuffer);
            ::SetLastError(lastError);
            return statusValue;
        }

        LSTATUS WINAPI HookedRegCreateKeyExW(HKEY rootKey, LPCWSTR subKeyPointer, DWORD reservedValue, LPWSTR classPointer, DWORD optionsValue, REGSAM samDesired, const LPSECURITY_ATTRIBUTES securityAttributes, PHKEY resultKeyPointer, LPDWORD dispositionPointer)
        {
            ScopedHookGuard guardValue;
            if (guardValue.bypass()) { return g_regCreateKeyExWOriginal(rootKey, subKeyPointer, reservedValue, classPointer, optionsValue, samDesired, securityAttributes, resultKeyPointer, dispositionPointer); }
            const LSTATUS statusValue = g_regCreateKeyExWOriginal(rootKey, subKeyPointer, reservedValue, classPointer, optionsValue, samDesired, securityAttributes, resultKeyPointer, dispositionPointer);
            const DWORD lastError = ::GetLastError();
            wchar_t detailBuffer[ks::winapi_monitor::kMaxDetailChars] = {};
            BuildRegCreateDetail(detailBuffer, rootKey, subKeyPointer, optionsValue, dispositionPointer != nullptr ? *dispositionPointer : 0);
            APIMON_REG_SEND(L"RegCreateKeyExW", statusValue, detailBuffer);
            ::SetLastError(lastError);
            return statusValue;
        }

        LSTATUS WINAPI HookedRegCreateKeyExA(HKEY rootKey, LPCSTR subKeyPointer, DWORD reservedValue, LPSTR classPointer, DWORD optionsValue, REGSAM samDesired, const LPSECURITY_ATTRIBUTES securityAttributes, PHKEY resultKeyPointer, LPDWORD dispositionPointer)
        {
            ScopedHookGuard guardValue;
            if (guardValue.bypass()) { return g_regCreateKeyExAOriginal(rootKey, subKeyPointer, reservedValue, classPointer, optionsValue, samDesired, securityAttributes, resultKeyPointer, dispositionPointer); }
            const LSTATUS statusValue = g_regCreateKeyExAOriginal(rootKey, subKeyPointer, reservedValue, classPointer, optionsValue, samDesired, securityAttributes, resultKeyPointer, dispositionPointer);
            const DWORD lastError = ::GetLastError();
            wchar_t detailBuffer[ks::winapi_monitor::kMaxDetailChars] = {};
            BuildRegCreateDetailA(detailBuffer, rootKey, subKeyPointer, optionsValue, dispositionPointer != nullptr ? *dispositionPointer : 0);
            APIMON_REG_SEND(L"RegCreateKeyExA", statusValue, detailBuffer);
            ::SetLastError(lastError);
            return statusValue;
        }

        LSTATUS WINAPI HookedRegQueryValueExW(HKEY keyHandle, LPCWSTR valueNamePointer, LPDWORD reservedPointer, LPDWORD typePointer, LPBYTE dataPointer, LPDWORD dataSizePointer)
        {
            ScopedHookGuard guardValue;
            if (guardValue.bypass()) { return g_regQueryValueExWOriginal(keyHandle, valueNamePointer, reservedPointer, typePointer, dataPointer, dataSizePointer); }
            const LSTATUS statusValue = g_regQueryValueExWOriginal(keyHandle, valueNamePointer, reservedPointer, typePointer, dataPointer, dataSizePointer);
            const DWORD lastError = ::GetLastError();
            wchar_t detailBuffer[ks::winapi_monitor::kMaxDetailChars] = {};
            BuildRegValueDetail(detailBuffer, L"hkey=", keyHandle, valueNamePointer, typePointer != nullptr ? *typePointer : 0, dataSizePointer != nullptr ? *dataSizePointer : 0);
            APIMON_REG_SEND(L"RegQueryValueExW", statusValue, detailBuffer);
            ::SetLastError(lastError);
            return statusValue;
        }

        LSTATUS WINAPI HookedRegQueryValueExA(HKEY keyHandle, LPCSTR valueNamePointer, LPDWORD reservedPointer, LPDWORD typePointer, LPBYTE dataPointer, LPDWORD dataSizePointer)
        {
            ScopedHookGuard guardValue;
            if (guardValue.bypass()) { return g_regQueryValueExAOriginal(keyHandle, valueNamePointer, reservedPointer, typePointer, dataPointer, dataSizePointer); }
            const LSTATUS statusValue = g_regQueryValueExAOriginal(keyHandle, valueNamePointer, reservedPointer, typePointer, dataPointer, dataSizePointer);
            const DWORD lastError = ::GetLastError();
            wchar_t detailBuffer[ks::winapi_monitor::kMaxDetailChars] = {};
            BuildRegValueDetailA(detailBuffer, L"hkey=", keyHandle, valueNamePointer, typePointer != nullptr ? *typePointer : 0, dataSizePointer != nullptr ? *dataSizePointer : 0);
            APIMON_REG_SEND(L"RegQueryValueExA", statusValue, detailBuffer);
            ::SetLastError(lastError);
            return statusValue;
        }

        LSTATUS WINAPI HookedRegGetValueW(HKEY keyHandle, LPCWSTR subKeyPointer, LPCWSTR valueNamePointer, DWORD flagsValue, LPDWORD typePointer, PVOID dataPointer, LPDWORD dataSizePointer)
        {
            ScopedHookGuard guardValue;
            if (guardValue.bypass()) { return g_regGetValueWOriginal(keyHandle, subKeyPointer, valueNamePointer, flagsValue, typePointer, dataPointer, dataSizePointer); }
            const LSTATUS statusValue = g_regGetValueWOriginal(keyHandle, subKeyPointer, valueNamePointer, flagsValue, typePointer, dataPointer, dataSizePointer);
            const DWORD lastError = ::GetLastError();
            wchar_t detailBuffer[ks::winapi_monitor::kMaxDetailChars] = {};
            BuildRegGetValueDetail(detailBuffer, keyHandle, subKeyPointer, valueNamePointer, flagsValue, typePointer != nullptr ? *typePointer : 0, dataSizePointer != nullptr ? *dataSizePointer : 0);
            APIMON_REG_SEND(L"RegGetValueW", statusValue, detailBuffer);
            ::SetLastError(lastError);
            return statusValue;
        }

        LSTATUS WINAPI HookedRegGetValueA(HKEY keyHandle, LPCSTR subKeyPointer, LPCSTR valueNamePointer, DWORD flagsValue, LPDWORD typePointer, PVOID dataPointer, LPDWORD dataSizePointer)
        {
            ScopedHookGuard guardValue;
            if (guardValue.bypass()) { return g_regGetValueAOriginal(keyHandle, subKeyPointer, valueNamePointer, flagsValue, typePointer, dataPointer, dataSizePointer); }
            const LSTATUS statusValue = g_regGetValueAOriginal(keyHandle, subKeyPointer, valueNamePointer, flagsValue, typePointer, dataPointer, dataSizePointer);
            const DWORD lastError = ::GetLastError();
            wchar_t detailBuffer[ks::winapi_monitor::kMaxDetailChars] = {};
            BuildRegGetValueDetailA(detailBuffer, keyHandle, subKeyPointer, valueNamePointer, flagsValue, typePointer != nullptr ? *typePointer : 0, dataSizePointer != nullptr ? *dataSizePointer : 0);
            APIMON_REG_SEND(L"RegGetValueA", statusValue, detailBuffer);
            ::SetLastError(lastError);
            return statusValue;
        }

        LSTATUS WINAPI HookedRegSetValueExW(HKEY keyHandle, LPCWSTR valueNamePointer, DWORD reservedValue, DWORD typeValue, const BYTE* dataPointer, DWORD dataSize)
        {
            ScopedHookGuard guardValue;
            if (guardValue.bypass()) { return g_regSetValueExWOriginal(keyHandle, valueNamePointer, reservedValue, typeValue, dataPointer, dataSize); }
            const LSTATUS statusValue = g_regSetValueExWOriginal(keyHandle, valueNamePointer, reservedValue, typeValue, dataPointer, dataSize);
            const DWORD lastError = ::GetLastError();
            wchar_t detailBuffer[ks::winapi_monitor::kMaxDetailChars] = {};
            BuildRegSetValueDetail(detailBuffer, keyHandle, valueNamePointer, typeValue, dataSize);
            APIMON_REG_SEND(L"RegSetValueExW", statusValue, detailBuffer);
            ::SetLastError(lastError);
            return statusValue;
        }

        LSTATUS WINAPI HookedRegSetValueExA(HKEY keyHandle, LPCSTR valueNamePointer, DWORD reservedValue, DWORD typeValue, const BYTE* dataPointer, DWORD dataSize)
        {
            ScopedHookGuard guardValue;
            if (guardValue.bypass()) { return g_regSetValueExAOriginal(keyHandle, valueNamePointer, reservedValue, typeValue, dataPointer, dataSize); }
            const LSTATUS statusValue = g_regSetValueExAOriginal(keyHandle, valueNamePointer, reservedValue, typeValue, dataPointer, dataSize);
            const DWORD lastError = ::GetLastError();
            wchar_t detailBuffer[ks::winapi_monitor::kMaxDetailChars] = {};
            BuildRegSetValueDetailA(detailBuffer, keyHandle, valueNamePointer, typeValue, dataSize);
            APIMON_REG_SEND(L"RegSetValueExA", statusValue, detailBuffer);
            ::SetLastError(lastError);
            return statusValue;
        }

        LSTATUS WINAPI HookedRegSetKeyValueW(HKEY keyHandle, LPCWSTR subKeyPointer, LPCWSTR valueNamePointer, DWORD typeValue, const void* dataPointer, DWORD dataSize)
        {
            ScopedHookGuard guardValue;
            if (guardValue.bypass()) { return g_regSetKeyValueWOriginal(keyHandle, subKeyPointer, valueNamePointer, typeValue, dataPointer, dataSize); }
            const LSTATUS statusValue = g_regSetKeyValueWOriginal(keyHandle, subKeyPointer, valueNamePointer, typeValue, dataPointer, dataSize);
            const DWORD lastError = ::GetLastError();
            wchar_t detailBuffer[ks::winapi_monitor::kMaxDetailChars] = {};
            BuildRegGetValueDetail(detailBuffer, keyHandle, subKeyPointer, valueNamePointer, 0, typeValue, dataSize);
            APIMON_REG_SEND(L"RegSetKeyValueW", statusValue, detailBuffer);
            ::SetLastError(lastError);
            return statusValue;
        }

        LSTATUS WINAPI HookedRegSetKeyValueA(HKEY keyHandle, LPCSTR subKeyPointer, LPCSTR valueNamePointer, DWORD typeValue, const void* dataPointer, DWORD dataSize)
        {
            ScopedHookGuard guardValue;
            if (guardValue.bypass()) { return g_regSetKeyValueAOriginal(keyHandle, subKeyPointer, valueNamePointer, typeValue, dataPointer, dataSize); }
            const LSTATUS statusValue = g_regSetKeyValueAOriginal(keyHandle, subKeyPointer, valueNamePointer, typeValue, dataPointer, dataSize);
            const DWORD lastError = ::GetLastError();
            wchar_t detailBuffer[ks::winapi_monitor::kMaxDetailChars] = {};
            BuildRegGetValueDetailA(detailBuffer, keyHandle, subKeyPointer, valueNamePointer, 0, typeValue, dataSize);
            APIMON_REG_SEND(L"RegSetKeyValueA", statusValue, detailBuffer);
            ::SetLastError(lastError);
            return statusValue;
        }

        LSTATUS WINAPI HookedRegDeleteValueW(HKEY keyHandle, LPCWSTR valueNamePointer)
        {
            ScopedHookGuard guardValue;
            if (guardValue.bypass()) { return g_regDeleteValueWOriginal(keyHandle, valueNamePointer); }
            const LSTATUS statusValue = g_regDeleteValueWOriginal(keyHandle, valueNamePointer);
            const DWORD lastError = ::GetLastError();
            wchar_t detailBuffer[ks::winapi_monitor::kMaxDetailChars] = {};
            BuildRegValueDetail(detailBuffer, L"hkey=", keyHandle, valueNamePointer, 0, 0);
            APIMON_REG_SEND(L"RegDeleteValueW", statusValue, detailBuffer);
            ::SetLastError(lastError);
            return statusValue;
        }

        LSTATUS WINAPI HookedRegDeleteValueA(HKEY keyHandle, LPCSTR valueNamePointer)
        {
            ScopedHookGuard guardValue;
            if (guardValue.bypass()) { return g_regDeleteValueAOriginal(keyHandle, valueNamePointer); }
            const LSTATUS statusValue = g_regDeleteValueAOriginal(keyHandle, valueNamePointer);
            const DWORD lastError = ::GetLastError();
            wchar_t detailBuffer[ks::winapi_monitor::kMaxDetailChars] = {};
            BuildRegValueDetailA(detailBuffer, L"hkey=", keyHandle, valueNamePointer, 0, 0);
            APIMON_REG_SEND(L"RegDeleteValueA", statusValue, detailBuffer);
            ::SetLastError(lastError);
            return statusValue;
        }

        LSTATUS WINAPI HookedRegDeleteKeyW(HKEY rootKey, LPCWSTR subKeyPointer)
        {
            ScopedHookGuard guardValue;
            if (guardValue.bypass()) { return g_regDeleteKeyWOriginal(rootKey, subKeyPointer); }
            const LSTATUS statusValue = g_regDeleteKeyWOriginal(rootKey, subKeyPointer);
            const DWORD lastError = ::GetLastError();
            wchar_t detailBuffer[ks::winapi_monitor::kMaxDetailChars] = {};
            BuildRegSubKeyDetail(detailBuffer, rootKey, subKeyPointer, 0);
            APIMON_REG_SEND(L"RegDeleteKeyW", statusValue, detailBuffer);
            ::SetLastError(lastError);
            return statusValue;
        }

        LSTATUS WINAPI HookedRegDeleteKeyA(HKEY rootKey, LPCSTR subKeyPointer)
        {
            ScopedHookGuard guardValue;
            if (guardValue.bypass()) { return g_regDeleteKeyAOriginal(rootKey, subKeyPointer); }
            const LSTATUS statusValue = g_regDeleteKeyAOriginal(rootKey, subKeyPointer);
            const DWORD lastError = ::GetLastError();
            wchar_t detailBuffer[ks::winapi_monitor::kMaxDetailChars] = {};
            BuildRegSubKeyDetailA(detailBuffer, rootKey, subKeyPointer, 0);
            APIMON_REG_SEND(L"RegDeleteKeyA", statusValue, detailBuffer);
            ::SetLastError(lastError);
            return statusValue;
        }

        LSTATUS WINAPI HookedRegDeleteKeyExW(HKEY rootKey, LPCWSTR subKeyPointer, REGSAM samDesired, DWORD reservedValue)
        {
            ScopedHookGuard guardValue;
            if (guardValue.bypass()) { return g_regDeleteKeyExWOriginal(rootKey, subKeyPointer, samDesired, reservedValue); }
            const LSTATUS statusValue = g_regDeleteKeyExWOriginal(rootKey, subKeyPointer, samDesired, reservedValue);
            const DWORD lastError = ::GetLastError();
            wchar_t detailBuffer[ks::winapi_monitor::kMaxDetailChars] = {};
            BuildRegSubKeyDetail(detailBuffer, rootKey, subKeyPointer, samDesired);
            APIMON_REG_SEND(L"RegDeleteKeyExW", statusValue, detailBuffer);
            ::SetLastError(lastError);
            return statusValue;
        }

        LSTATUS WINAPI HookedRegDeleteKeyExA(HKEY rootKey, LPCSTR subKeyPointer, REGSAM samDesired, DWORD reservedValue)
        {
            ScopedHookGuard guardValue;
            if (guardValue.bypass()) { return g_regDeleteKeyExAOriginal(rootKey, subKeyPointer, samDesired, reservedValue); }
            const LSTATUS statusValue = g_regDeleteKeyExAOriginal(rootKey, subKeyPointer, samDesired, reservedValue);
            const DWORD lastError = ::GetLastError();
            wchar_t detailBuffer[ks::winapi_monitor::kMaxDetailChars] = {};
            BuildRegSubKeyDetailA(detailBuffer, rootKey, subKeyPointer, samDesired);
            APIMON_REG_SEND(L"RegDeleteKeyExA", statusValue, detailBuffer);
            ::SetLastError(lastError);
            return statusValue;
        }

        LSTATUS WINAPI HookedRegDeleteTreeW(HKEY rootKey, LPCWSTR subKeyPointer)
        {
            ScopedHookGuard guardValue;
            if (guardValue.bypass()) { return g_regDeleteTreeWOriginal(rootKey, subKeyPointer); }
            const LSTATUS statusValue = g_regDeleteTreeWOriginal(rootKey, subKeyPointer);
            const DWORD lastError = ::GetLastError();
            wchar_t detailBuffer[ks::winapi_monitor::kMaxDetailChars] = {};
            BuildRegSubKeyDetail(detailBuffer, rootKey, subKeyPointer, 0);
            APIMON_REG_SEND(L"RegDeleteTreeW", statusValue, detailBuffer);
            ::SetLastError(lastError);
            return statusValue;
        }

        LSTATUS WINAPI HookedRegDeleteTreeA(HKEY rootKey, LPCSTR subKeyPointer)
        {
            ScopedHookGuard guardValue;
            if (guardValue.bypass()) { return g_regDeleteTreeAOriginal(rootKey, subKeyPointer); }
            const LSTATUS statusValue = g_regDeleteTreeAOriginal(rootKey, subKeyPointer);
            const DWORD lastError = ::GetLastError();
            wchar_t detailBuffer[ks::winapi_monitor::kMaxDetailChars] = {};
            BuildRegSubKeyDetailA(detailBuffer, rootKey, subKeyPointer, 0);
            APIMON_REG_SEND(L"RegDeleteTreeA", statusValue, detailBuffer);
            ::SetLastError(lastError);
            return statusValue;
        }

        LSTATUS WINAPI HookedRegCopyTreeW(HKEY rootKey, LPCWSTR subKeyPointer, HKEY destKey)
        {
            ScopedHookGuard guardValue;
            if (guardValue.bypass()) { return g_regCopyTreeWOriginal(rootKey, subKeyPointer, destKey); }
            const LSTATUS statusValue = g_regCopyTreeWOriginal(rootKey, subKeyPointer, destKey);
            const DWORD lastError = ::GetLastError();
            wchar_t detailBuffer[ks::winapi_monitor::kMaxDetailChars] = {};
            BuildRegSubKeyDetail(detailBuffer, rootKey, subKeyPointer, 0);
            AppendWideText(detailBuffer, L" dest=");
            AppendHexText(detailBuffer, reinterpret_cast<std::uint64_t>(destKey));
            APIMON_REG_SEND(L"RegCopyTreeW", statusValue, detailBuffer);
            ::SetLastError(lastError);
            return statusValue;
        }

        LSTATUS WINAPI HookedRegCopyTreeA(HKEY rootKey, LPCSTR subKeyPointer, HKEY destKey)
        {
            ScopedHookGuard guardValue;
            if (guardValue.bypass()) { return g_regCopyTreeAOriginal(rootKey, subKeyPointer, destKey); }
            const LSTATUS statusValue = g_regCopyTreeAOriginal(rootKey, subKeyPointer, destKey);
            const DWORD lastError = ::GetLastError();
            wchar_t detailBuffer[ks::winapi_monitor::kMaxDetailChars] = {};
            BuildRegSubKeyDetailA(detailBuffer, rootKey, subKeyPointer, 0);
            AppendWideText(detailBuffer, L" dest=");
            AppendHexText(detailBuffer, reinterpret_cast<std::uint64_t>(destKey));
            APIMON_REG_SEND(L"RegCopyTreeA", statusValue, detailBuffer);
            ::SetLastError(lastError);
            return statusValue;
        }

        LSTATUS WINAPI HookedRegLoadKeyW(HKEY rootKey, LPCWSTR subKeyPointer, LPCWSTR fileNamePointer)
        {
            ScopedHookGuard guardValue;
            if (guardValue.bypass()) { return g_regLoadKeyWOriginal(rootKey, subKeyPointer, fileNamePointer); }
            const LSTATUS statusValue = g_regLoadKeyWOriginal(rootKey, subKeyPointer, fileNamePointer);
            const DWORD lastError = ::GetLastError();
            wchar_t detailBuffer[ks::winapi_monitor::kMaxDetailChars] = {};
            BuildRegSubKeyDetail(detailBuffer, rootKey, subKeyPointer, 0);
            AppendWideText(detailBuffer, L" file=");
            AppendWideText(detailBuffer, fileNamePointer);
            APIMON_REG_SEND(L"RegLoadKeyW", statusValue, detailBuffer);
            ::SetLastError(lastError);
            return statusValue;
        }

        LSTATUS WINAPI HookedRegLoadKeyA(HKEY rootKey, LPCSTR subKeyPointer, LPCSTR fileNamePointer)
        {
            ScopedHookGuard guardValue;
            if (guardValue.bypass()) { return g_regLoadKeyAOriginal(rootKey, subKeyPointer, fileNamePointer); }
            const LSTATUS statusValue = g_regLoadKeyAOriginal(rootKey, subKeyPointer, fileNamePointer);
            const DWORD lastError = ::GetLastError();
            wchar_t detailBuffer[ks::winapi_monitor::kMaxDetailChars] = {};
            BuildRegSubKeyDetailA(detailBuffer, rootKey, subKeyPointer, 0);
            AppendWideText(detailBuffer, L" file=");
            AppendAnsiText(detailBuffer, fileNamePointer);
            APIMON_REG_SEND(L"RegLoadKeyA", statusValue, detailBuffer);
            ::SetLastError(lastError);
            return statusValue;
        }

        LSTATUS WINAPI HookedRegSaveKeyW(HKEY keyHandle, LPCWSTR fileNamePointer, const LPSECURITY_ATTRIBUTES securityAttributesPointer)
        {
            ScopedHookGuard guardValue;
            if (guardValue.bypass()) { return g_regSaveKeyWOriginal(keyHandle, fileNamePointer, securityAttributesPointer); }
            const LSTATUS statusValue = g_regSaveKeyWOriginal(keyHandle, fileNamePointer, securityAttributesPointer);
            const DWORD lastError = ::GetLastError();
            wchar_t detailBuffer[ks::winapi_monitor::kMaxDetailChars] = {};
            AppendWideText(detailBuffer, L"hkey=");
            AppendHexText(detailBuffer, reinterpret_cast<std::uint64_t>(keyHandle));
            AppendWideText(detailBuffer, L" file=");
            AppendWideText(detailBuffer, fileNamePointer);
            APIMON_REG_SEND(L"RegSaveKeyW", statusValue, detailBuffer);
            ::SetLastError(lastError);
            return statusValue;
        }

        LSTATUS WINAPI HookedRegSaveKeyA(HKEY keyHandle, LPCSTR fileNamePointer, const LPSECURITY_ATTRIBUTES securityAttributesPointer)
        {
            ScopedHookGuard guardValue;
            if (guardValue.bypass()) { return g_regSaveKeyAOriginal(keyHandle, fileNamePointer, securityAttributesPointer); }
            const LSTATUS statusValue = g_regSaveKeyAOriginal(keyHandle, fileNamePointer, securityAttributesPointer);
            const DWORD lastError = ::GetLastError();
            wchar_t detailBuffer[ks::winapi_monitor::kMaxDetailChars] = {};
            AppendWideText(detailBuffer, L"hkey=");
            AppendHexText(detailBuffer, reinterpret_cast<std::uint64_t>(keyHandle));
            AppendWideText(detailBuffer, L" file=");
            AppendAnsiText(detailBuffer, fileNamePointer);
            APIMON_REG_SEND(L"RegSaveKeyA", statusValue, detailBuffer);
            ::SetLastError(lastError);
            return statusValue;
        }

        LSTATUS WINAPI HookedRegRenameKey(HKEY keyHandle, LPCWSTR subKeyPointer, LPCWSTR newNamePointer)
        {
            ScopedHookGuard guardValue;
            if (guardValue.bypass()) { return g_regRenameKeyOriginal(keyHandle, subKeyPointer, newNamePointer); }
            const LSTATUS statusValue = g_regRenameKeyOriginal(keyHandle, subKeyPointer, newNamePointer);
            const DWORD lastError = ::GetLastError();
            wchar_t detailBuffer[ks::winapi_monitor::kMaxDetailChars] = {};
            AppendWideText(detailBuffer, L"hkey=");
            AppendHexText(detailBuffer, reinterpret_cast<std::uint64_t>(keyHandle));
            AppendWideText(detailBuffer, L" subkey=");
            AppendWideText(detailBuffer, subKeyPointer);
            AppendWideText(detailBuffer, L" new=");
            AppendWideText(detailBuffer, newNamePointer);
            APIMON_REG_SEND(L"RegRenameKey", statusValue, detailBuffer);
            ::SetLastError(lastError);
            return statusValue;
        }

        LSTATUS WINAPI HookedRegEnumKeyExW(HKEY keyHandle, DWORD indexValue, LPWSTR namePointer, LPDWORD nameLengthPointer, LPDWORD reservedPointer, LPWSTR classPointer, LPDWORD classLengthPointer, PFILETIME lastWriteTimePointer)
        {
            ScopedHookGuard guardValue;
            if (guardValue.bypass()) { return g_regEnumKeyExWOriginal(keyHandle, indexValue, namePointer, nameLengthPointer, reservedPointer, classPointer, classLengthPointer, lastWriteTimePointer); }
            const LSTATUS statusValue = g_regEnumKeyExWOriginal(keyHandle, indexValue, namePointer, nameLengthPointer, reservedPointer, classPointer, classLengthPointer, lastWriteTimePointer);
            const DWORD lastError = ::GetLastError();
            wchar_t detailBuffer[ks::winapi_monitor::kMaxDetailChars] = {};
            BuildRegEnumKeyDetail(detailBuffer, keyHandle, indexValue, statusValue == ERROR_SUCCESS ? namePointer : nullptr, statusValue == ERROR_SUCCESS && nameLengthPointer != nullptr ? *nameLengthPointer : 0);
            APIMON_REG_SEND(L"RegEnumKeyExW", statusValue, detailBuffer);
            ::SetLastError(lastError);
            return statusValue;
        }

        LSTATUS WINAPI HookedRegEnumKeyExA(HKEY keyHandle, DWORD indexValue, LPSTR namePointer, LPDWORD nameLengthPointer, LPDWORD reservedPointer, LPSTR classPointer, LPDWORD classLengthPointer, PFILETIME lastWriteTimePointer)
        {
            ScopedHookGuard guardValue;
            if (guardValue.bypass()) { return g_regEnumKeyExAOriginal(keyHandle, indexValue, namePointer, nameLengthPointer, reservedPointer, classPointer, classLengthPointer, lastWriteTimePointer); }
            const LSTATUS statusValue = g_regEnumKeyExAOriginal(keyHandle, indexValue, namePointer, nameLengthPointer, reservedPointer, classPointer, classLengthPointer, lastWriteTimePointer);
            const DWORD lastError = ::GetLastError();
            wchar_t detailBuffer[ks::winapi_monitor::kMaxDetailChars] = {};
            BuildRegEnumKeyDetailA(detailBuffer, keyHandle, indexValue, statusValue == ERROR_SUCCESS ? namePointer : nullptr, statusValue == ERROR_SUCCESS && nameLengthPointer != nullptr ? *nameLengthPointer : 0);
            APIMON_REG_SEND(L"RegEnumKeyExA", statusValue, detailBuffer);
            ::SetLastError(lastError);
            return statusValue;
        }

        LSTATUS WINAPI HookedRegEnumValueW(HKEY keyHandle, DWORD indexValue, LPWSTR valueNamePointer, LPDWORD valueNameLengthPointer, LPDWORD reservedPointer, LPDWORD typePointer, LPBYTE dataPointer, LPDWORD dataSizePointer)
        {
            ScopedHookGuard guardValue;
            if (guardValue.bypass()) { return g_regEnumValueWOriginal(keyHandle, indexValue, valueNamePointer, valueNameLengthPointer, reservedPointer, typePointer, dataPointer, dataSizePointer); }
            const LSTATUS statusValue = g_regEnumValueWOriginal(keyHandle, indexValue, valueNamePointer, valueNameLengthPointer, reservedPointer, typePointer, dataPointer, dataSizePointer);
            const DWORD lastError = ::GetLastError();
            wchar_t detailBuffer[ks::winapi_monitor::kMaxDetailChars] = {};
            BuildRegValueDetail(detailBuffer, L"hkey=", keyHandle, statusValue == ERROR_SUCCESS ? valueNamePointer : nullptr, typePointer != nullptr ? *typePointer : 0, dataSizePointer != nullptr ? *dataSizePointer : 0);
            AppendWideText(detailBuffer, L" index=");
            AppendUnsignedText(detailBuffer, indexValue);
            APIMON_REG_SEND(L"RegEnumValueW", statusValue, detailBuffer);
            ::SetLastError(lastError);
            return statusValue;
        }

        LSTATUS WINAPI HookedRegEnumValueA(HKEY keyHandle, DWORD indexValue, LPSTR valueNamePointer, LPDWORD valueNameLengthPointer, LPDWORD reservedPointer, LPDWORD typePointer, LPBYTE dataPointer, LPDWORD dataSizePointer)
        {
            ScopedHookGuard guardValue;
            if (guardValue.bypass()) { return g_regEnumValueAOriginal(keyHandle, indexValue, valueNamePointer, valueNameLengthPointer, reservedPointer, typePointer, dataPointer, dataSizePointer); }
            const LSTATUS statusValue = g_regEnumValueAOriginal(keyHandle, indexValue, valueNamePointer, valueNameLengthPointer, reservedPointer, typePointer, dataPointer, dataSizePointer);
            const DWORD lastError = ::GetLastError();
            wchar_t detailBuffer[ks::winapi_monitor::kMaxDetailChars] = {};
            BuildRegValueDetailA(detailBuffer, L"hkey=", keyHandle, statusValue == ERROR_SUCCESS ? valueNamePointer : nullptr, typePointer != nullptr ? *typePointer : 0, dataSizePointer != nullptr ? *dataSizePointer : 0);
            AppendWideText(detailBuffer, L" index=");
            AppendUnsignedText(detailBuffer, indexValue);
            APIMON_REG_SEND(L"RegEnumValueA", statusValue, detailBuffer);
            ::SetLastError(lastError);
            return statusValue;
        }

        LSTATUS WINAPI HookedRegCloseKey(HKEY keyHandle)
        {
            ScopedHookGuard guardValue;
            if (guardValue.bypass()) { return g_regCloseKeyOriginal(keyHandle); }
            const LSTATUS statusValue = g_regCloseKeyOriginal(keyHandle);
            const DWORD lastError = ::GetLastError();
            wchar_t detailBuffer[ks::winapi_monitor::kMaxDetailChars] = {};
            AppendWideText(detailBuffer, L"hkey=");
            AppendHexText(detailBuffer, reinterpret_cast<std::uint64_t>(keyHandle));
            APIMON_REG_SEND(L"RegCloseKey", statusValue, detailBuffer);
            ::SetLastError(lastError);
            return statusValue;
        }

        // HookedRegQueryInfoKeyW 作用：
        // - 输入：注册表键句柄和统计信息输出缓冲；
        // - 处理：记录子键/值数量查询，补齐枚举类注册表 API 覆盖；
        // - 返回：保持 RegQueryInfoKeyW 的 LSTATUS，并恢复 LastError。
        LSTATUS WINAPI HookedRegQueryInfoKeyW(
            HKEY keyHandle,
            LPWSTR classPointer,
            LPDWORD classLengthPointer,
            LPDWORD reservedPointer,
            LPDWORD subKeyCountPointer,
            LPDWORD maxSubKeyLengthPointer,
            LPDWORD maxClassLengthPointer,
            LPDWORD valueCountPointer,
            LPDWORD maxValueNameLengthPointer,
            LPDWORD maxValueLengthPointer,
            LPDWORD securityDescriptorLengthPointer,
            PFILETIME lastWriteTimePointer)
        {
            ScopedHookGuard guardValue;
            if (guardValue.bypass())
            {
                return g_regQueryInfoKeyWOriginal(keyHandle, classPointer, classLengthPointer, reservedPointer, subKeyCountPointer, maxSubKeyLengthPointer, maxClassLengthPointer, valueCountPointer, maxValueNameLengthPointer, maxValueLengthPointer, securityDescriptorLengthPointer, lastWriteTimePointer);
            }

            const LSTATUS statusValue = g_regQueryInfoKeyWOriginal(keyHandle, classPointer, classLengthPointer, reservedPointer, subKeyCountPointer, maxSubKeyLengthPointer, maxClassLengthPointer, valueCountPointer, maxValueNameLengthPointer, maxValueLengthPointer, securityDescriptorLengthPointer, lastWriteTimePointer);
            const DWORD lastError = ::GetLastError();
            wchar_t detailBuffer[ks::winapi_monitor::kMaxDetailChars] = {};
            AppendWideText(detailBuffer, L"hkey=");
            AppendHexText(detailBuffer, reinterpret_cast<std::uint64_t>(keyHandle));
            AppendWideText(detailBuffer, L" subKeys=");
            AppendUnsignedText(detailBuffer, subKeyCountPointer != nullptr ? *subKeyCountPointer : 0);
            AppendWideText(detailBuffer, L" values=");
            AppendUnsignedText(detailBuffer, valueCountPointer != nullptr ? *valueCountPointer : 0);
            APIMON_REG_SEND(L"RegQueryInfoKeyW", statusValue, detailBuffer);
            ::SetLastError(lastError);
            return statusValue;
        }

        // HookedRegQueryInfoKeyA 作用：
        // - 输入：ANSI 版本 RegQueryInfoKey 参数；
        // - 处理：记录键统计信息查询结果；
        // - 返回：保持原始 LSTATUS，并恢复 LastError。
        LSTATUS WINAPI HookedRegQueryInfoKeyA(
            HKEY keyHandle,
            LPSTR classPointer,
            LPDWORD classLengthPointer,
            LPDWORD reservedPointer,
            LPDWORD subKeyCountPointer,
            LPDWORD maxSubKeyLengthPointer,
            LPDWORD maxClassLengthPointer,
            LPDWORD valueCountPointer,
            LPDWORD maxValueNameLengthPointer,
            LPDWORD maxValueLengthPointer,
            LPDWORD securityDescriptorLengthPointer,
            PFILETIME lastWriteTimePointer)
        {
            ScopedHookGuard guardValue;
            if (guardValue.bypass())
            {
                return g_regQueryInfoKeyAOriginal(keyHandle, classPointer, classLengthPointer, reservedPointer, subKeyCountPointer, maxSubKeyLengthPointer, maxClassLengthPointer, valueCountPointer, maxValueNameLengthPointer, maxValueLengthPointer, securityDescriptorLengthPointer, lastWriteTimePointer);
            }

            const LSTATUS statusValue = g_regQueryInfoKeyAOriginal(keyHandle, classPointer, classLengthPointer, reservedPointer, subKeyCountPointer, maxSubKeyLengthPointer, maxClassLengthPointer, valueCountPointer, maxValueNameLengthPointer, maxValueLengthPointer, securityDescriptorLengthPointer, lastWriteTimePointer);
            const DWORD lastError = ::GetLastError();
            wchar_t detailBuffer[ks::winapi_monitor::kMaxDetailChars] = {};
            AppendWideText(detailBuffer, L"hkey=");
            AppendHexText(detailBuffer, reinterpret_cast<std::uint64_t>(keyHandle));
            AppendWideText(detailBuffer, L" subKeys=");
            AppendUnsignedText(detailBuffer, subKeyCountPointer != nullptr ? *subKeyCountPointer : 0);
            AppendWideText(detailBuffer, L" values=");
            AppendUnsignedText(detailBuffer, valueCountPointer != nullptr ? *valueCountPointer : 0);
            APIMON_REG_SEND(L"RegQueryInfoKeyA", statusValue, detailBuffer);
            ::SetLastError(lastError);
            return statusValue;
        }

        // HookedRegFlushKey 作用：
        // - 输入：注册表键句柄；
        // - 处理：记录强制落盘操作，该行为常用于持久化确认；
        // - 返回：保持 RegFlushKey 的 LSTATUS，并恢复 LastError。
        LSTATUS WINAPI HookedRegFlushKey(HKEY keyHandle)
        {
            ScopedHookGuard guardValue;
            if (guardValue.bypass()) { return g_regFlushKeyOriginal(keyHandle); }
            const LSTATUS statusValue = g_regFlushKeyOriginal(keyHandle);
            const DWORD lastError = ::GetLastError();
            wchar_t detailBuffer[ks::winapi_monitor::kMaxDetailChars] = {};
            AppendWideText(detailBuffer, L"hkey=");
            AppendHexText(detailBuffer, reinterpret_cast<std::uint64_t>(keyHandle));
            APIMON_REG_SEND(L"RegFlushKey", statusValue, detailBuffer);
            ::SetLastError(lastError);
            return statusValue;
        }

        // HookedRegDeleteKeyValueW 作用：
        // - 输入：键句柄、可选子键和值名；
        // - 处理：记录组合删除值 API，补齐 RegDeleteValue/RegDeleteTree 之间的空白；
        // - 返回：保持原始 LSTATUS，并恢复 LastError。
        LSTATUS WINAPI HookedRegDeleteKeyValueW(HKEY keyHandle, LPCWSTR subKeyPointer, LPCWSTR valueNamePointer)
        {
            ScopedHookGuard guardValue;
            if (guardValue.bypass()) { return g_regDeleteKeyValueWOriginal(keyHandle, subKeyPointer, valueNamePointer); }
            const LSTATUS statusValue = g_regDeleteKeyValueWOriginal(keyHandle, subKeyPointer, valueNamePointer);
            const DWORD lastError = ::GetLastError();
            wchar_t detailBuffer[ks::winapi_monitor::kMaxDetailChars] = {};
            BuildRegValueDetail(detailBuffer, L"hkey=", keyHandle, valueNamePointer, 0, 0);
            AppendWideText(detailBuffer, L" sub=");
            AppendWideText(detailBuffer, subKeyPointer);
            APIMON_REG_SEND(L"RegDeleteKeyValueW", statusValue, detailBuffer);
            ::SetLastError(lastError);
            return statusValue;
        }

        // HookedRegDeleteKeyValueA 作用：
        // - 输入：ANSI 版本键句柄、可选子键和值名；
        // - 处理：记录组合删除值 API；
        // - 返回：保持原始 LSTATUS，并恢复 LastError。
        LSTATUS WINAPI HookedRegDeleteKeyValueA(HKEY keyHandle, LPCSTR subKeyPointer, LPCSTR valueNamePointer)
        {
            ScopedHookGuard guardValue;
            if (guardValue.bypass()) { return g_regDeleteKeyValueAOriginal(keyHandle, subKeyPointer, valueNamePointer); }
            const LSTATUS statusValue = g_regDeleteKeyValueAOriginal(keyHandle, subKeyPointer, valueNamePointer);
            const DWORD lastError = ::GetLastError();
            wchar_t detailBuffer[ks::winapi_monitor::kMaxDetailChars] = {};
            BuildRegValueDetailA(detailBuffer, L"hkey=", keyHandle, valueNamePointer, 0, 0);
            AppendWideText(detailBuffer, L" sub=");
            AppendAnsiText(detailBuffer, subKeyPointer);
            APIMON_REG_SEND(L"RegDeleteKeyValueA", statusValue, detailBuffer);
            ::SetLastError(lastError);
            return statusValue;
        }

        // HookedRegConnectRegistryW 作用：
        // - 输入：远程机器名、根键和结果句柄指针；
        // - 处理：记录远程注册表连接行为；
        // - 返回：保持原始 LSTATUS，并恢复 LastError。
        LSTATUS WINAPI HookedRegConnectRegistryW(LPCWSTR machineNamePointer, HKEY rootKey, PHKEY resultKeyPointer)
        {
            ScopedHookGuard guardValue;
            if (guardValue.bypass()) { return g_regConnectRegistryWOriginal(machineNamePointer, rootKey, resultKeyPointer); }
            const LSTATUS statusValue = g_regConnectRegistryWOriginal(machineNamePointer, rootKey, resultKeyPointer);
            const DWORD lastError = ::GetLastError();
            wchar_t detailBuffer[ks::winapi_monitor::kMaxDetailChars] = {};
            AppendWideText(detailBuffer, L"machine=");
            AppendWideText(detailBuffer, machineNamePointer);
            AppendWideText(detailBuffer, L" root=");
            AppendRegistryRootText(detailBuffer, rootKey);
            AppendWideText(detailBuffer, L" result=");
            AppendHexText(detailBuffer, resultKeyPointer != nullptr ? reinterpret_cast<std::uint64_t>(*resultKeyPointer) : 0);
            APIMON_REG_SEND(L"RegConnectRegistryW", statusValue, detailBuffer);
            ::SetLastError(lastError);
            return statusValue;
        }

        // HookedRegConnectRegistryA 作用：
        // - 输入：ANSI 版本远程机器名、根键和结果句柄指针；
        // - 处理：记录远程注册表连接行为；
        // - 返回：保持原始 LSTATUS，并恢复 LastError。
        LSTATUS WINAPI HookedRegConnectRegistryA(LPCSTR machineNamePointer, HKEY rootKey, PHKEY resultKeyPointer)
        {
            ScopedHookGuard guardValue;
            if (guardValue.bypass()) { return g_regConnectRegistryAOriginal(machineNamePointer, rootKey, resultKeyPointer); }
            const LSTATUS statusValue = g_regConnectRegistryAOriginal(machineNamePointer, rootKey, resultKeyPointer);
            const DWORD lastError = ::GetLastError();
            wchar_t detailBuffer[ks::winapi_monitor::kMaxDetailChars] = {};
            AppendWideText(detailBuffer, L"machine=");
            AppendAnsiText(detailBuffer, machineNamePointer);
            AppendWideText(detailBuffer, L" root=");
            AppendRegistryRootText(detailBuffer, rootKey);
            AppendWideText(detailBuffer, L" result=");
            AppendHexText(detailBuffer, resultKeyPointer != nullptr ? reinterpret_cast<std::uint64_t>(*resultKeyPointer) : 0);
            APIMON_REG_SEND(L"RegConnectRegistryA", statusValue, detailBuffer);
            ::SetLastError(lastError);
            return statusValue;
        }

        NTSTATUS NTAPI HookedNtCreateFile(PHANDLE fileHandlePointer, ACCESS_MASK desiredAccess, POBJECT_ATTRIBUTES objectAttributesPointer, PIO_STATUS_BLOCK ioStatusBlockPointer, PLARGE_INTEGER allocationSizePointer, ULONG fileAttributes, ULONG shareAccess, ULONG createDisposition, ULONG createOptions, PVOID eaBufferPointer, ULONG eaLength)
        {
            ScopedHookGuard guardValue;
            if (guardValue.bypass()) { return g_ntCreateFileOriginal(fileHandlePointer, desiredAccess, objectAttributesPointer, ioStatusBlockPointer, allocationSizePointer, fileAttributes, shareAccess, createDisposition, createOptions, eaBufferPointer, eaLength); }
            const NTSTATUS statusValue = g_ntCreateFileOriginal(fileHandlePointer, desiredAccess, objectAttributesPointer, ioStatusBlockPointer, allocationSizePointer, fileAttributes, shareAccess, createDisposition, createOptions, eaBufferPointer, eaLength);
            wchar_t detailBuffer[ks::winapi_monitor::kMaxDetailChars] = {};
            BuildNtObjectPathDetail(detailBuffer, objectAttributesPointer, desiredAccess, shareAccess, createDisposition, createOptions);
            AppendWideText(detailBuffer, L" handle=");
            AppendHexText(detailBuffer, fileHandlePointer != nullptr ? reinterpret_cast<std::uint64_t>(*fileHandlePointer) : 0);
            SendRawEventWithStatus(ks::winapi_monitor::EventCategory::File, L"ntdll", L"NtCreateFile", statusValue, detailBuffer);
            return statusValue;
        }

        NTSTATUS NTAPI HookedNtOpenFile(PHANDLE fileHandlePointer, ACCESS_MASK desiredAccess, POBJECT_ATTRIBUTES objectAttributesPointer, PIO_STATUS_BLOCK ioStatusBlockPointer, ULONG shareAccess, ULONG openOptions)
        {
            ScopedHookGuard guardValue;
            if (guardValue.bypass()) { return g_ntOpenFileOriginal(fileHandlePointer, desiredAccess, objectAttributesPointer, ioStatusBlockPointer, shareAccess, openOptions); }
            const NTSTATUS statusValue = g_ntOpenFileOriginal(fileHandlePointer, desiredAccess, objectAttributesPointer, ioStatusBlockPointer, shareAccess, openOptions);
            wchar_t detailBuffer[ks::winapi_monitor::kMaxDetailChars] = {};
            BuildNtObjectPathDetail(detailBuffer, objectAttributesPointer, desiredAccess, shareAccess, 0, openOptions);
            AppendWideText(detailBuffer, L" handle=");
            AppendHexText(detailBuffer, fileHandlePointer != nullptr ? reinterpret_cast<std::uint64_t>(*fileHandlePointer) : 0);
            SendRawEventWithStatus(ks::winapi_monitor::EventCategory::File, L"ntdll", L"NtOpenFile", statusValue, detailBuffer);
            return statusValue;
        }

        NTSTATUS NTAPI HookedNtReadFile(HANDLE fileHandle, HANDLE eventHandle, KsIoApcRoutine apcRoutinePointer, PVOID apcContextPointer, PIO_STATUS_BLOCK ioStatusBlockPointer, PVOID bufferPointer, ULONG lengthValue, PLARGE_INTEGER byteOffsetPointer, PULONG keyPointer)
        {
            if (IsMonitorPipeHandle(fileHandle))
            {
                return g_ntReadFileOriginal(fileHandle, eventHandle, apcRoutinePointer, apcContextPointer, ioStatusBlockPointer, bufferPointer, lengthValue, byteOffsetPointer, keyPointer);
            }
            ScopedHookGuard guardValue;
            if (guardValue.bypass()) { return g_ntReadFileOriginal(fileHandle, eventHandle, apcRoutinePointer, apcContextPointer, ioStatusBlockPointer, bufferPointer, lengthValue, byteOffsetPointer, keyPointer); }
            const NTSTATUS statusValue = g_ntReadFileOriginal(fileHandle, eventHandle, apcRoutinePointer, apcContextPointer, ioStatusBlockPointer, bufferPointer, lengthValue, byteOffsetPointer, keyPointer);
            wchar_t detailBuffer[ks::winapi_monitor::kMaxDetailChars] = {};
            BuildHandleTransferDetail(detailBuffer, fileHandle, lengthValue, ioStatusBlockPointer != nullptr ? static_cast<unsigned long long>(ioStatusBlockPointer->Information) : 0, byteOffsetPointer);
            SendRawEventWithStatus(ks::winapi_monitor::EventCategory::File, L"ntdll", L"NtReadFile", statusValue, detailBuffer);
            return statusValue;
        }

        NTSTATUS NTAPI HookedNtWriteFile(HANDLE fileHandle, HANDLE eventHandle, KsIoApcRoutine apcRoutinePointer, PVOID apcContextPointer, PIO_STATUS_BLOCK ioStatusBlockPointer, PVOID bufferPointer, ULONG lengthValue, PLARGE_INTEGER byteOffsetPointer, PULONG keyPointer)
        {
            if (IsMonitorPipeHandle(fileHandle))
            {
                return g_ntWriteFileOriginal(fileHandle, eventHandle, apcRoutinePointer, apcContextPointer, ioStatusBlockPointer, bufferPointer, lengthValue, byteOffsetPointer, keyPointer);
            }
            ScopedHookGuard guardValue;
            if (guardValue.bypass()) { return g_ntWriteFileOriginal(fileHandle, eventHandle, apcRoutinePointer, apcContextPointer, ioStatusBlockPointer, bufferPointer, lengthValue, byteOffsetPointer, keyPointer); }
            const NTSTATUS statusValue = g_ntWriteFileOriginal(fileHandle, eventHandle, apcRoutinePointer, apcContextPointer, ioStatusBlockPointer, bufferPointer, lengthValue, byteOffsetPointer, keyPointer);
            wchar_t detailBuffer[ks::winapi_monitor::kMaxDetailChars] = {};
            BuildHandleTransferDetail(detailBuffer, fileHandle, lengthValue, ioStatusBlockPointer != nullptr ? static_cast<unsigned long long>(ioStatusBlockPointer->Information) : 0, byteOffsetPointer);
            SendRawEventWithStatus(ks::winapi_monitor::EventCategory::File, L"ntdll", L"NtWriteFile", statusValue, detailBuffer);
            return statusValue;
        }

        NTSTATUS NTAPI HookedNtSetInformationFile(HANDLE fileHandle, PIO_STATUS_BLOCK ioStatusBlockPointer, PVOID fileInformationPointer, ULONG lengthValue, KS_FILE_INFORMATION_CLASS fileInformationClass)
        {
            ScopedHookGuard guardValue;
            if (guardValue.bypass()) { return g_ntSetInformationFileOriginal(fileHandle, ioStatusBlockPointer, fileInformationPointer, lengthValue, fileInformationClass); }
            const NTSTATUS statusValue = g_ntSetInformationFileOriginal(fileHandle, ioStatusBlockPointer, fileInformationPointer, lengthValue, fileInformationClass);
            wchar_t detailBuffer[ks::winapi_monitor::kMaxDetailChars] = {};
            AppendWideText(detailBuffer, L"handle=");
            AppendHexText(detailBuffer, reinterpret_cast<std::uint64_t>(fileHandle));
            AppendWideText(detailBuffer, L" class=");
            AppendUnsignedText(detailBuffer, static_cast<unsigned long long>(fileInformationClass));
            AppendWideText(detailBuffer, L" size=");
            AppendUnsignedText(detailBuffer, lengthValue);
            SendRawEventWithStatus(ks::winapi_monitor::EventCategory::File, L"ntdll", L"NtSetInformationFile", statusValue, detailBuffer);
            return statusValue;
        }

        NTSTATUS NTAPI HookedNtQueryInformationFile(HANDLE fileHandle, PIO_STATUS_BLOCK ioStatusBlockPointer, PVOID fileInformationPointer, ULONG lengthValue, KS_FILE_INFORMATION_CLASS fileInformationClass)
        {
            ScopedHookGuard guardValue;
            if (guardValue.bypass()) { return g_ntQueryInformationFileOriginal(fileHandle, ioStatusBlockPointer, fileInformationPointer, lengthValue, fileInformationClass); }
            const NTSTATUS statusValue = g_ntQueryInformationFileOriginal(fileHandle, ioStatusBlockPointer, fileInformationPointer, lengthValue, fileInformationClass);
            wchar_t detailBuffer[ks::winapi_monitor::kMaxDetailChars] = {};
            AppendWideText(detailBuffer, L"handle=");
            AppendHexText(detailBuffer, reinterpret_cast<std::uint64_t>(fileHandle));
            AppendWideText(detailBuffer, L" class=");
            AppendUnsignedText(detailBuffer, static_cast<unsigned long long>(fileInformationClass));
            AppendWideText(detailBuffer, L" size=");
            AppendUnsignedText(detailBuffer, lengthValue);
            SendRawEventWithStatus(ks::winapi_monitor::EventCategory::File, L"ntdll", L"NtQueryInformationFile", statusValue, detailBuffer);
            return statusValue;
        }

        NTSTATUS NTAPI HookedNtDeleteFile(POBJECT_ATTRIBUTES objectAttributesPointer)
        {
            ScopedHookGuard guardValue;
            if (guardValue.bypass()) { return g_ntDeleteFileOriginal(objectAttributesPointer); }
            const NTSTATUS statusValue = g_ntDeleteFileOriginal(objectAttributesPointer);
            wchar_t detailBuffer[ks::winapi_monitor::kMaxDetailChars] = {};
            BuildNtObjectPathDetail(detailBuffer, objectAttributesPointer, 0, 0, 0, 0);
            SendRawEventWithStatus(ks::winapi_monitor::EventCategory::File, L"ntdll", L"NtDeleteFile", statusValue, detailBuffer);
            return statusValue;
        }

        NTSTATUS NTAPI HookedNtQueryAttributesFile(POBJECT_ATTRIBUTES objectAttributesPointer, PVOID fileInformationPointer)
        {
            ScopedHookGuard guardValue;
            if (guardValue.bypass()) { return g_ntQueryAttributesFileOriginal(objectAttributesPointer, fileInformationPointer); }
            const NTSTATUS statusValue = g_ntQueryAttributesFileOriginal(objectAttributesPointer, fileInformationPointer);
            wchar_t detailBuffer[ks::winapi_monitor::kMaxDetailChars] = {};
            BuildNtObjectPathDetail(detailBuffer, objectAttributesPointer, 0, 0, 0, 0);
            SendRawEventWithStatus(ks::winapi_monitor::EventCategory::File, L"ntdll", L"NtQueryAttributesFile", statusValue, detailBuffer);
            return statusValue;
        }

        NTSTATUS NTAPI HookedNtQueryFullAttributesFile(POBJECT_ATTRIBUTES objectAttributesPointer, PVOID fileInformationPointer)
        {
            ScopedHookGuard guardValue;
            if (guardValue.bypass()) { return g_ntQueryFullAttributesFileOriginal(objectAttributesPointer, fileInformationPointer); }
            const NTSTATUS statusValue = g_ntQueryFullAttributesFileOriginal(objectAttributesPointer, fileInformationPointer);
            wchar_t detailBuffer[ks::winapi_monitor::kMaxDetailChars] = {};
            BuildNtObjectPathDetail(detailBuffer, objectAttributesPointer, 0, 0, 0, 0);
            SendRawEventWithStatus(ks::winapi_monitor::EventCategory::File, L"ntdll", L"NtQueryFullAttributesFile", statusValue, detailBuffer);
            return statusValue;
        }

        // HookedNtDeviceIoControlFile 作用：
        // - 输入：文件/设备句柄、事件/APC、IO_STATUS_BLOCK、控制码和缓冲区长度；
        // - 处理：记录直接 ntdll 设备控制调用，覆盖绕过 KernelBase DeviceIoControl 的路径；
        // - 返回：保持原始 NTSTATUS。
        NTSTATUS NTAPI HookedNtDeviceIoControlFile(
            HANDLE fileHandle,
            HANDLE eventHandle,
            KsIoApcRoutine apcRoutinePointer,
            PVOID apcContextPointer,
            PIO_STATUS_BLOCK ioStatusBlockPointer,
            ULONG ioControlCode,
            PVOID inputBufferPointer,
            ULONG inputBufferLength,
            PVOID outputBufferPointer,
            ULONG outputBufferLength)
        {
            ScopedHookGuard guardValue;
            if (guardValue.bypass())
            {
                return g_ntDeviceIoControlFileOriginal(fileHandle, eventHandle, apcRoutinePointer, apcContextPointer, ioStatusBlockPointer, ioControlCode, inputBufferPointer, inputBufferLength, outputBufferPointer, outputBufferLength);
            }

            const NTSTATUS statusValue = g_ntDeviceIoControlFileOriginal(fileHandle, eventHandle, apcRoutinePointer, apcContextPointer, ioStatusBlockPointer, ioControlCode, inputBufferPointer, inputBufferLength, outputBufferPointer, outputBufferLength);
            wchar_t detailBuffer[ks::winapi_monitor::kMaxDetailChars] = {};
            AppendWideText(detailBuffer, L"handle=");
            AppendHexText(detailBuffer, reinterpret_cast<std::uint64_t>(fileHandle));
            AppendWideText(detailBuffer, L" code=");
            AppendHexText(detailBuffer, ioControlCode);
            AppendWideText(detailBuffer, L" in=");
            AppendUnsignedText(detailBuffer, inputBufferLength);
            AppendWideText(detailBuffer, L" out=");
            AppendUnsignedText(detailBuffer, outputBufferLength);
            AppendWideText(detailBuffer, L" info=");
            AppendUnsignedText(detailBuffer, ioStatusBlockPointer != nullptr ? static_cast<unsigned long long>(ioStatusBlockPointer->Information) : 0);
            SendRawEventWithStatus(ks::winapi_monitor::EventCategory::File, L"ntdll", L"NtDeviceIoControlFile", statusValue, detailBuffer);
            return statusValue;
        }

        // HookedNtFsControlFile 作用：
        // - 输入：文件句柄、控制码和缓冲区长度；
        // - 处理：记录文件系统控制调用，例如重解析点、卷控制和管道控制；
        // - 返回：保持原始 NTSTATUS。
        NTSTATUS NTAPI HookedNtFsControlFile(
            HANDLE fileHandle,
            HANDLE eventHandle,
            KsIoApcRoutine apcRoutinePointer,
            PVOID apcContextPointer,
            PIO_STATUS_BLOCK ioStatusBlockPointer,
            ULONG fsControlCode,
            PVOID inputBufferPointer,
            ULONG inputBufferLength,
            PVOID outputBufferPointer,
            ULONG outputBufferLength)
        {
            ScopedHookGuard guardValue;
            if (guardValue.bypass())
            {
                return g_ntFsControlFileOriginal(fileHandle, eventHandle, apcRoutinePointer, apcContextPointer, ioStatusBlockPointer, fsControlCode, inputBufferPointer, inputBufferLength, outputBufferPointer, outputBufferLength);
            }

            const NTSTATUS statusValue = g_ntFsControlFileOriginal(fileHandle, eventHandle, apcRoutinePointer, apcContextPointer, ioStatusBlockPointer, fsControlCode, inputBufferPointer, inputBufferLength, outputBufferPointer, outputBufferLength);
            wchar_t detailBuffer[ks::winapi_monitor::kMaxDetailChars] = {};
            AppendWideText(detailBuffer, L"handle=");
            AppendHexText(detailBuffer, reinterpret_cast<std::uint64_t>(fileHandle));
            AppendWideText(detailBuffer, L" code=");
            AppendHexText(detailBuffer, fsControlCode);
            AppendWideText(detailBuffer, L" in=");
            AppendUnsignedText(detailBuffer, inputBufferLength);
            AppendWideText(detailBuffer, L" out=");
            AppendUnsignedText(detailBuffer, outputBufferLength);
            SendRawEventWithStatus(ks::winapi_monitor::EventCategory::File, L"ntdll", L"NtFsControlFile", statusValue, detailBuffer);
            return statusValue;
        }

        // HookedNtQueryDirectoryFile 作用：
        // - 输入：目录句柄、查询缓冲、信息类、可选文件名和重启标志；
        // - 处理：记录直接目录枚举调用，补齐 FindFirstFileEx 以下的 Nt 层枚举；
        // - 返回：保持原始 NTSTATUS。
        NTSTATUS NTAPI HookedNtQueryDirectoryFile(
            HANDLE fileHandle,
            HANDLE eventHandle,
            KsIoApcRoutine apcRoutinePointer,
            PVOID apcContextPointer,
            PIO_STATUS_BLOCK ioStatusBlockPointer,
            PVOID fileInformationPointer,
            ULONG lengthValue,
            KS_FILE_INFORMATION_CLASS fileInformationClass,
            BOOLEAN returnSingleEntry,
            PUNICODE_STRING fileNamePointer,
            BOOLEAN restartScan)
        {
            ScopedHookGuard guardValue;
            if (guardValue.bypass())
            {
                return g_ntQueryDirectoryFileOriginal(fileHandle, eventHandle, apcRoutinePointer, apcContextPointer, ioStatusBlockPointer, fileInformationPointer, lengthValue, fileInformationClass, returnSingleEntry, fileNamePointer, restartScan);
            }

            const NTSTATUS statusValue = g_ntQueryDirectoryFileOriginal(fileHandle, eventHandle, apcRoutinePointer, apcContextPointer, ioStatusBlockPointer, fileInformationPointer, lengthValue, fileInformationClass, returnSingleEntry, fileNamePointer, restartScan);
            wchar_t detailBuffer[ks::winapi_monitor::kMaxDetailChars] = {};
            AppendWideText(detailBuffer, L"handle=");
            AppendHexText(detailBuffer, reinterpret_cast<std::uint64_t>(fileHandle));
            AppendWideText(detailBuffer, L" pattern=");
            AppendUnicodeStringText(detailBuffer, fileNamePointer);
            AppendWideText(detailBuffer, L" class=");
            AppendUnsignedText(detailBuffer, static_cast<unsigned long long>(fileInformationClass));
            AppendWideText(detailBuffer, L" size=");
            AppendUnsignedText(detailBuffer, lengthValue);
            AppendWideText(detailBuffer, L" single=");
            AppendUnsignedText(detailBuffer, returnSingleEntry ? 1 : 0);
            AppendWideText(detailBuffer, L" restart=");
            AppendUnsignedText(detailBuffer, restartScan ? 1 : 0);
            SendRawEventWithStatus(ks::winapi_monitor::EventCategory::File, L"ntdll", L"NtQueryDirectoryFile", statusValue, detailBuffer);
            return statusValue;
        }

        // HookedNtQueryDirectoryFileEx 作用：
        // - 输入：扩展目录查询参数，包括 QueryFlags 和可选文件名；
        // - 处理：记录现代 NtQueryDirectoryFileEx 枚举路径；
        // - 返回：保持原始 NTSTATUS。
        NTSTATUS NTAPI HookedNtQueryDirectoryFileEx(
            HANDLE fileHandle,
            HANDLE eventHandle,
            KsIoApcRoutine apcRoutinePointer,
            PVOID apcContextPointer,
            PIO_STATUS_BLOCK ioStatusBlockPointer,
            PVOID fileInformationPointer,
            ULONG lengthValue,
            KS_FILE_INFORMATION_CLASS fileInformationClass,
            ULONG queryFlags,
            PUNICODE_STRING fileNamePointer)
        {
            ScopedHookGuard guardValue;
            if (guardValue.bypass())
            {
                return g_ntQueryDirectoryFileExOriginal(fileHandle, eventHandle, apcRoutinePointer, apcContextPointer, ioStatusBlockPointer, fileInformationPointer, lengthValue, fileInformationClass, queryFlags, fileNamePointer);
            }

            const NTSTATUS statusValue = g_ntQueryDirectoryFileExOriginal(fileHandle, eventHandle, apcRoutinePointer, apcContextPointer, ioStatusBlockPointer, fileInformationPointer, lengthValue, fileInformationClass, queryFlags, fileNamePointer);
            wchar_t detailBuffer[ks::winapi_monitor::kMaxDetailChars] = {};
            AppendWideText(detailBuffer, L"handle=");
            AppendHexText(detailBuffer, reinterpret_cast<std::uint64_t>(fileHandle));
            AppendWideText(detailBuffer, L" pattern=");
            AppendUnicodeStringText(detailBuffer, fileNamePointer);
            AppendWideText(detailBuffer, L" class=");
            AppendUnsignedText(detailBuffer, static_cast<unsigned long long>(fileInformationClass));
            AppendWideText(detailBuffer, L" flags=");
            AppendHexText(detailBuffer, queryFlags);
            AppendWideText(detailBuffer, L" size=");
            AppendUnsignedText(detailBuffer, lengthValue);
            SendRawEventWithStatus(ks::winapi_monitor::EventCategory::File, L"ntdll", L"NtQueryDirectoryFileEx", statusValue, detailBuffer);
            return statusValue;
        }

        NTSTATUS NTAPI HookedNtCreateKey(PHANDLE keyHandlePointer, ACCESS_MASK desiredAccess, POBJECT_ATTRIBUTES objectAttributesPointer, ULONG titleIndex, PUNICODE_STRING classPointer, ULONG createOptions, PULONG dispositionPointer)
        {
            ScopedHookGuard guardValue;
            if (guardValue.bypass()) { return g_ntCreateKeyOriginal(keyHandlePointer, desiredAccess, objectAttributesPointer, titleIndex, classPointer, createOptions, dispositionPointer); }
            const NTSTATUS statusValue = g_ntCreateKeyOriginal(keyHandlePointer, desiredAccess, objectAttributesPointer, titleIndex, classPointer, createOptions, dispositionPointer);
            wchar_t detailBuffer[ks::winapi_monitor::kMaxDetailChars] = {};
            BuildNtObjectPathDetail(detailBuffer, objectAttributesPointer, desiredAccess, 0, dispositionPointer != nullptr ? *dispositionPointer : 0, createOptions);
            AppendWideText(detailBuffer, L" handle=");
            AppendHexText(detailBuffer, keyHandlePointer != nullptr ? reinterpret_cast<std::uint64_t>(*keyHandlePointer) : 0);
            SendRawEventWithStatus(ks::winapi_monitor::EventCategory::Registry, L"ntdll", L"NtCreateKey", statusValue, detailBuffer);
            return statusValue;
        }

        NTSTATUS NTAPI HookedNtOpenKey(PHANDLE keyHandlePointer, ACCESS_MASK desiredAccess, POBJECT_ATTRIBUTES objectAttributesPointer)
        {
            ScopedHookGuard guardValue;
            if (guardValue.bypass()) { return g_ntOpenKeyOriginal(keyHandlePointer, desiredAccess, objectAttributesPointer); }
            const NTSTATUS statusValue = g_ntOpenKeyOriginal(keyHandlePointer, desiredAccess, objectAttributesPointer);
            wchar_t detailBuffer[ks::winapi_monitor::kMaxDetailChars] = {};
            BuildNtObjectPathDetail(detailBuffer, objectAttributesPointer, desiredAccess, 0, 0, 0);
            AppendWideText(detailBuffer, L" handle=");
            AppendHexText(detailBuffer, keyHandlePointer != nullptr ? reinterpret_cast<std::uint64_t>(*keyHandlePointer) : 0);
            SendRawEventWithStatus(ks::winapi_monitor::EventCategory::Registry, L"ntdll", L"NtOpenKey", statusValue, detailBuffer);
            return statusValue;
        }

        NTSTATUS NTAPI HookedNtOpenKeyEx(PHANDLE keyHandlePointer, ACCESS_MASK desiredAccess, POBJECT_ATTRIBUTES objectAttributesPointer, ULONG openOptions)
        {
            ScopedHookGuard guardValue;
            if (guardValue.bypass()) { return g_ntOpenKeyExOriginal(keyHandlePointer, desiredAccess, objectAttributesPointer, openOptions); }
            const NTSTATUS statusValue = g_ntOpenKeyExOriginal(keyHandlePointer, desiredAccess, objectAttributesPointer, openOptions);
            wchar_t detailBuffer[ks::winapi_monitor::kMaxDetailChars] = {};
            BuildNtObjectPathDetail(detailBuffer, objectAttributesPointer, desiredAccess, 0, 0, openOptions);
            AppendWideText(detailBuffer, L" handle=");
            AppendHexText(detailBuffer, keyHandlePointer != nullptr ? reinterpret_cast<std::uint64_t>(*keyHandlePointer) : 0);
            SendRawEventWithStatus(ks::winapi_monitor::EventCategory::Registry, L"ntdll", L"NtOpenKeyEx", statusValue, detailBuffer);
            return statusValue;
        }

        NTSTATUS NTAPI HookedNtSetValueKey(HANDLE keyHandle, PUNICODE_STRING valueNamePointer, ULONG titleIndex, ULONG typeValue, PVOID dataPointer, ULONG dataSize)
        {
            ScopedHookGuard guardValue;
            if (guardValue.bypass()) { return g_ntSetValueKeyOriginal(keyHandle, valueNamePointer, titleIndex, typeValue, dataPointer, dataSize); }
            const NTSTATUS statusValue = g_ntSetValueKeyOriginal(keyHandle, valueNamePointer, titleIndex, typeValue, dataPointer, dataSize);
            wchar_t detailBuffer[ks::winapi_monitor::kMaxDetailChars] = {};
            BuildNtKeyValueDetail(detailBuffer, keyHandle, valueNamePointer, typeValue, dataSize);
            SendRawEventWithStatus(ks::winapi_monitor::EventCategory::Registry, L"ntdll", L"NtSetValueKey", statusValue, detailBuffer);
            return statusValue;
        }

        NTSTATUS NTAPI HookedNtQueryValueKey(HANDLE keyHandle, PUNICODE_STRING valueNamePointer, KS_KEY_VALUE_INFORMATION_CLASS keyValueInformationClass, PVOID keyValueInformationPointer, ULONG lengthValue, PULONG resultLengthPointer)
        {
            ScopedHookGuard guardValue;
            if (guardValue.bypass()) { return g_ntQueryValueKeyOriginal(keyHandle, valueNamePointer, keyValueInformationClass, keyValueInformationPointer, lengthValue, resultLengthPointer); }
            const NTSTATUS statusValue = g_ntQueryValueKeyOriginal(keyHandle, valueNamePointer, keyValueInformationClass, keyValueInformationPointer, lengthValue, resultLengthPointer);
            wchar_t detailBuffer[ks::winapi_monitor::kMaxDetailChars] = {};
            BuildNtKeyValueDetail(detailBuffer, keyHandle, valueNamePointer, static_cast<ULONG>(keyValueInformationClass), resultLengthPointer != nullptr ? *resultLengthPointer : lengthValue);
            SendRawEventWithStatus(ks::winapi_monitor::EventCategory::Registry, L"ntdll", L"NtQueryValueKey", statusValue, detailBuffer);
            return statusValue;
        }

        NTSTATUS NTAPI HookedNtEnumerateKey(HANDLE keyHandle, ULONG indexValue, KS_KEY_INFORMATION_CLASS keyInformationClass, PVOID keyInformationPointer, ULONG lengthValue, PULONG resultLengthPointer)
        {
            ScopedHookGuard guardValue;
            if (guardValue.bypass()) { return g_ntEnumerateKeyOriginal(keyHandle, indexValue, keyInformationClass, keyInformationPointer, lengthValue, resultLengthPointer); }
            const NTSTATUS statusValue = g_ntEnumerateKeyOriginal(keyHandle, indexValue, keyInformationClass, keyInformationPointer, lengthValue, resultLengthPointer);
            wchar_t detailBuffer[ks::winapi_monitor::kMaxDetailChars] = {};
            AppendWideText(detailBuffer, L"hkey=");
            AppendHexText(detailBuffer, reinterpret_cast<std::uint64_t>(keyHandle));
            AppendWideText(detailBuffer, L" index=");
            AppendUnsignedText(detailBuffer, indexValue);
            AppendWideText(detailBuffer, L" class=");
            AppendUnsignedText(detailBuffer, static_cast<unsigned long long>(keyInformationClass));
            AppendWideText(detailBuffer, L" size=");
            AppendUnsignedText(detailBuffer, resultLengthPointer != nullptr ? *resultLengthPointer : lengthValue);
            SendRawEventWithStatus(ks::winapi_monitor::EventCategory::Registry, L"ntdll", L"NtEnumerateKey", statusValue, detailBuffer);
            return statusValue;
        }

        NTSTATUS NTAPI HookedNtEnumerateValueKey(HANDLE keyHandle, ULONG indexValue, KS_KEY_VALUE_INFORMATION_CLASS keyValueInformationClass, PVOID keyValueInformationPointer, ULONG lengthValue, PULONG resultLengthPointer)
        {
            ScopedHookGuard guardValue;
            if (guardValue.bypass()) { return g_ntEnumerateValueKeyOriginal(keyHandle, indexValue, keyValueInformationClass, keyValueInformationPointer, lengthValue, resultLengthPointer); }
            const NTSTATUS statusValue = g_ntEnumerateValueKeyOriginal(keyHandle, indexValue, keyValueInformationClass, keyValueInformationPointer, lengthValue, resultLengthPointer);
            wchar_t detailBuffer[ks::winapi_monitor::kMaxDetailChars] = {};
            AppendWideText(detailBuffer, L"hkey=");
            AppendHexText(detailBuffer, reinterpret_cast<std::uint64_t>(keyHandle));
            AppendWideText(detailBuffer, L" index=");
            AppendUnsignedText(detailBuffer, indexValue);
            AppendWideText(detailBuffer, L" class=");
            AppendUnsignedText(detailBuffer, static_cast<unsigned long long>(keyValueInformationClass));
            AppendWideText(detailBuffer, L" size=");
            AppendUnsignedText(detailBuffer, resultLengthPointer != nullptr ? *resultLengthPointer : lengthValue);
            SendRawEventWithStatus(ks::winapi_monitor::EventCategory::Registry, L"ntdll", L"NtEnumerateValueKey", statusValue, detailBuffer);
            return statusValue;
        }

        NTSTATUS NTAPI HookedNtDeleteKey(HANDLE keyHandle)
        {
            ScopedHookGuard guardValue;
            if (guardValue.bypass()) { return g_ntDeleteKeyOriginal(keyHandle); }
            const NTSTATUS statusValue = g_ntDeleteKeyOriginal(keyHandle);
            wchar_t detailBuffer[ks::winapi_monitor::kMaxDetailChars] = {};
            AppendWideText(detailBuffer, L"hkey=");
            AppendHexText(detailBuffer, reinterpret_cast<std::uint64_t>(keyHandle));
            SendRawEventWithStatus(ks::winapi_monitor::EventCategory::Registry, L"ntdll", L"NtDeleteKey", statusValue, detailBuffer);
            return statusValue;
        }

        NTSTATUS NTAPI HookedNtDeleteValueKey(HANDLE keyHandle, PUNICODE_STRING valueNamePointer)
        {
            ScopedHookGuard guardValue;
            if (guardValue.bypass()) { return g_ntDeleteValueKeyOriginal(keyHandle, valueNamePointer); }
            const NTSTATUS statusValue = g_ntDeleteValueKeyOriginal(keyHandle, valueNamePointer);
            wchar_t detailBuffer[ks::winapi_monitor::kMaxDetailChars] = {};
            BuildNtKeyValueDetail(detailBuffer, keyHandle, valueNamePointer, 0, 0);
            SendRawEventWithStatus(ks::winapi_monitor::EventCategory::Registry, L"ntdll", L"NtDeleteValueKey", statusValue, detailBuffer);
            return statusValue;
        }

        NTSTATUS NTAPI HookedNtFlushKey(HANDLE keyHandle)
        {
            ScopedHookGuard guardValue;
            if (guardValue.bypass()) { return g_ntFlushKeyOriginal(keyHandle); }
            const NTSTATUS statusValue = g_ntFlushKeyOriginal(keyHandle);
            wchar_t detailBuffer[ks::winapi_monitor::kMaxDetailChars] = {};
            AppendWideText(detailBuffer, L"hkey=");
            AppendHexText(detailBuffer, reinterpret_cast<std::uint64_t>(keyHandle));
            SendRawEventWithStatus(ks::winapi_monitor::EventCategory::Registry, L"ntdll", L"NtFlushKey", statusValue, detailBuffer);
            return statusValue;
        }

        NTSTATUS NTAPI HookedNtRenameKey(HANDLE keyHandle, PUNICODE_STRING newNamePointer)
        {
            ScopedHookGuard guardValue;
            if (guardValue.bypass()) { return g_ntRenameKeyOriginal(keyHandle, newNamePointer); }
            const NTSTATUS statusValue = g_ntRenameKeyOriginal(keyHandle, newNamePointer);
            wchar_t detailBuffer[ks::winapi_monitor::kMaxDetailChars] = {};
            AppendWideText(detailBuffer, L"hkey=");
            AppendHexText(detailBuffer, reinterpret_cast<std::uint64_t>(keyHandle));
            AppendWideText(detailBuffer, L" new=");
            AppendUnicodeStringText(detailBuffer, newNamePointer);
            SendRawEventWithStatus(ks::winapi_monitor::EventCategory::Registry, L"ntdll", L"NtRenameKey", statusValue, detailBuffer);
            return statusValue;
        }

        NTSTATUS NTAPI HookedNtLoadKey(POBJECT_ATTRIBUTES targetKeyPointer, POBJECT_ATTRIBUTES sourceFilePointer)
        {
            ScopedHookGuard guardValue;
            if (guardValue.bypass()) { return g_ntLoadKeyOriginal(targetKeyPointer, sourceFilePointer); }
            const NTSTATUS statusValue = g_ntLoadKeyOriginal(targetKeyPointer, sourceFilePointer);
            wchar_t detailBuffer[ks::winapi_monitor::kMaxDetailChars] = {};
            AppendWideText(detailBuffer, L"target=");
            AppendObjectNameText(detailBuffer, targetKeyPointer);
            AppendWideText(detailBuffer, L" source=");
            AppendObjectNameText(detailBuffer, sourceFilePointer);
            SendRawEventWithStatus(ks::winapi_monitor::EventCategory::Registry, L"ntdll", L"NtLoadKey", statusValue, detailBuffer);
            return statusValue;
        }

        NTSTATUS NTAPI HookedNtSaveKey(HANDLE keyHandle, HANDLE fileHandle)
        {
            ScopedHookGuard guardValue;
            if (guardValue.bypass()) { return g_ntSaveKeyOriginal(keyHandle, fileHandle); }
            const NTSTATUS statusValue = g_ntSaveKeyOriginal(keyHandle, fileHandle);
            wchar_t detailBuffer[ks::winapi_monitor::kMaxDetailChars] = {};
            AppendWideText(detailBuffer, L"hkey=");
            AppendHexText(detailBuffer, reinterpret_cast<std::uint64_t>(keyHandle));
            AppendWideText(detailBuffer, L" file=");
            AppendHexText(detailBuffer, reinterpret_cast<std::uint64_t>(fileHandle));
            SendRawEventWithStatus(ks::winapi_monitor::EventCategory::Registry, L"ntdll", L"NtSaveKey", statusValue, detailBuffer);
            return statusValue;
        }

        // HookedNtQueryKey 作用：
        // - 输入：键句柄、信息类和输出缓冲长度；
        // - 处理：记录直接 Nt 层键元数据查询；
        // - 返回：保持原始 NTSTATUS。
        NTSTATUS NTAPI HookedNtQueryKey(HANDLE keyHandle, KS_KEY_INFORMATION_CLASS keyInformationClass, PVOID keyInformationPointer, ULONG lengthValue, PULONG resultLengthPointer)
        {
            ScopedHookGuard guardValue;
            if (guardValue.bypass()) { return g_ntQueryKeyOriginal(keyHandle, keyInformationClass, keyInformationPointer, lengthValue, resultLengthPointer); }
            const NTSTATUS statusValue = g_ntQueryKeyOriginal(keyHandle, keyInformationClass, keyInformationPointer, lengthValue, resultLengthPointer);
            wchar_t detailBuffer[ks::winapi_monitor::kMaxDetailChars] = {};
            AppendWideText(detailBuffer, L"hkey=");
            AppendHexText(detailBuffer, reinterpret_cast<std::uint64_t>(keyHandle));
            AppendWideText(detailBuffer, L" class=");
            AppendUnsignedText(detailBuffer, static_cast<unsigned long long>(keyInformationClass));
            AppendWideText(detailBuffer, L" size=");
            AppendUnsignedText(detailBuffer, resultLengthPointer != nullptr ? *resultLengthPointer : lengthValue);
            SendRawEventWithStatus(ks::winapi_monitor::EventCategory::Registry, L"ntdll", L"NtQueryKey", statusValue, detailBuffer);
            return statusValue;
        }

        // HookedNtQueryMultipleValueKey 作用：
        // - 输入：键句柄、值条目数组、条目数量和值缓冲长度；
        // - 处理：记录批量查询多个注册表值的 Nt 层路径；
        // - 返回：保持原始 NTSTATUS。
        NTSTATUS NTAPI HookedNtQueryMultipleValueKey(HANDLE keyHandle, PVOID valueEntriesPointer, ULONG entryCount, PVOID valueBufferPointer, PULONG bufferLengthPointer, PULONG requiredBufferLengthPointer)
        {
            ScopedHookGuard guardValue;
            if (guardValue.bypass()) { return g_ntQueryMultipleValueKeyOriginal(keyHandle, valueEntriesPointer, entryCount, valueBufferPointer, bufferLengthPointer, requiredBufferLengthPointer); }
            const NTSTATUS statusValue = g_ntQueryMultipleValueKeyOriginal(keyHandle, valueEntriesPointer, entryCount, valueBufferPointer, bufferLengthPointer, requiredBufferLengthPointer);
            wchar_t detailBuffer[ks::winapi_monitor::kMaxDetailChars] = {};
            AppendWideText(detailBuffer, L"hkey=");
            AppendHexText(detailBuffer, reinterpret_cast<std::uint64_t>(keyHandle));
            AppendWideText(detailBuffer, L" entries=");
            AppendUnsignedText(detailBuffer, entryCount);
            AppendWideText(detailBuffer, L" size=");
            AppendUnsignedText(detailBuffer, bufferLengthPointer != nullptr ? *bufferLengthPointer : 0);
            AppendWideText(detailBuffer, L" required=");
            AppendUnsignedText(detailBuffer, requiredBufferLengthPointer != nullptr ? *requiredBufferLengthPointer : 0);
            SendRawEventWithStatus(ks::winapi_monitor::EventCategory::Registry, L"ntdll", L"NtQueryMultipleValueKey", statusValue, detailBuffer);
            return statusValue;
        }

        // HookedNtNotifyChangeKey 作用：
        // - 输入：键句柄、事件/APC、过滤掩码、是否递归和缓冲信息；
        // - 处理：记录注册表变更通知订阅，覆盖监视型行为；
        // - 返回：保持原始 NTSTATUS。
        NTSTATUS NTAPI HookedNtNotifyChangeKey(HANDLE keyHandle, HANDLE eventHandle, KsIoApcRoutine apcRoutinePointer, PVOID apcContextPointer, PIO_STATUS_BLOCK ioStatusBlockPointer, ULONG completionFilter, BOOLEAN watchTree, PVOID bufferPointer, ULONG bufferSize, BOOLEAN asynchronous)
        {
            ScopedHookGuard guardValue;
            if (guardValue.bypass()) { return g_ntNotifyChangeKeyOriginal(keyHandle, eventHandle, apcRoutinePointer, apcContextPointer, ioStatusBlockPointer, completionFilter, watchTree, bufferPointer, bufferSize, asynchronous); }
            const NTSTATUS statusValue = g_ntNotifyChangeKeyOriginal(keyHandle, eventHandle, apcRoutinePointer, apcContextPointer, ioStatusBlockPointer, completionFilter, watchTree, bufferPointer, bufferSize, asynchronous);
            wchar_t detailBuffer[ks::winapi_monitor::kMaxDetailChars] = {};
            AppendWideText(detailBuffer, L"hkey=");
            AppendHexText(detailBuffer, reinterpret_cast<std::uint64_t>(keyHandle));
            AppendWideText(detailBuffer, L" filter=");
            AppendHexText(detailBuffer, completionFilter);
            AppendWideText(detailBuffer, L" tree=");
            AppendUnsignedText(detailBuffer, watchTree ? 1 : 0);
            AppendWideText(detailBuffer, L" async=");
            AppendUnsignedText(detailBuffer, asynchronous ? 1 : 0);
            SendRawEventWithStatus(ks::winapi_monitor::EventCategory::Registry, L"ntdll", L"NtNotifyChangeKey", statusValue, detailBuffer);
            return statusValue;
        }

        // HookedNtLoadKey2 作用：
        // - 输入：目标键对象、源 hive 文件对象和加载标志；
        // - 处理：记录带 flags 的 hive 加载路径；
        // - 返回：保持原始 NTSTATUS。
        NTSTATUS NTAPI HookedNtLoadKey2(POBJECT_ATTRIBUTES targetKeyPointer, POBJECT_ATTRIBUTES sourceFilePointer, ULONG flagsValue)
        {
            ScopedHookGuard guardValue;
            if (guardValue.bypass()) { return g_ntLoadKey2Original(targetKeyPointer, sourceFilePointer, flagsValue); }
            const NTSTATUS statusValue = g_ntLoadKey2Original(targetKeyPointer, sourceFilePointer, flagsValue);
            wchar_t detailBuffer[ks::winapi_monitor::kMaxDetailChars] = {};
            AppendWideText(detailBuffer, L"target=");
            AppendObjectNameText(detailBuffer, targetKeyPointer);
            AppendWideText(detailBuffer, L" source=");
            AppendObjectNameText(detailBuffer, sourceFilePointer);
            AppendWideText(detailBuffer, L" flags=");
            AppendHexText(detailBuffer, flagsValue);
            SendRawEventWithStatus(ks::winapi_monitor::EventCategory::Registry, L"ntdll", L"NtLoadKey2", statusValue, detailBuffer);
            return statusValue;
        }

        // HookedNtSaveKeyEx 作用：
        // - 输入：键句柄、文件句柄和保存格式标志；
        // - 处理：记录扩展 hive 保存路径；
        // - 返回：保持原始 NTSTATUS。
        NTSTATUS NTAPI HookedNtSaveKeyEx(HANDLE keyHandle, HANDLE fileHandle, ULONG formatValue)
        {
            ScopedHookGuard guardValue;
            if (guardValue.bypass()) { return g_ntSaveKeyExOriginal(keyHandle, fileHandle, formatValue); }
            const NTSTATUS statusValue = g_ntSaveKeyExOriginal(keyHandle, fileHandle, formatValue);
            wchar_t detailBuffer[ks::winapi_monitor::kMaxDetailChars] = {};
            AppendWideText(detailBuffer, L"hkey=");
            AppendHexText(detailBuffer, reinterpret_cast<std::uint64_t>(keyHandle));
            AppendWideText(detailBuffer, L" file=");
            AppendHexText(detailBuffer, reinterpret_cast<std::uint64_t>(fileHandle));
            AppendWideText(detailBuffer, L" format=");
            AppendHexText(detailBuffer, formatValue);
            SendRawEventWithStatus(ks::winapi_monitor::EventCategory::Registry, L"ntdll", L"NtSaveKeyEx", statusValue, detailBuffer);
            return statusValue;
        }

        // HookedNtLoadDriver 作用：
        // - 输入：注册表服务项路径 UNICODE_STRING；
        // - 处理：记录原生驱动加载请求，归入注册表/持久化相关监控；
        // - 返回：保持原始 NTSTATUS。
        NTSTATUS NTAPI HookedNtLoadDriver(PUNICODE_STRING serviceNamePointer)
        {
            ScopedHookGuard guardValue;
            if (guardValue.bypass()) { return g_ntLoadDriverOriginal(serviceNamePointer); }
            const NTSTATUS statusValue = g_ntLoadDriverOriginal(serviceNamePointer);
            wchar_t detailBuffer[ks::winapi_monitor::kMaxDetailChars] = {};
            AppendWideText(detailBuffer, L"service=");
            AppendUnicodeStringText(detailBuffer, serviceNamePointer);
            SendRawEventWithStatus(ks::winapi_monitor::EventCategory::Registry, L"ntdll", L"NtLoadDriver", statusValue, detailBuffer);
            return statusValue;
        }

        // HookedNtUnloadDriver 作用：
        // - 输入：注册表服务项路径 UNICODE_STRING；
        // - 处理：记录原生驱动卸载请求；
        // - 返回：保持原始 NTSTATUS。
        NTSTATUS NTAPI HookedNtUnloadDriver(PUNICODE_STRING serviceNamePointer)
        {
            ScopedHookGuard guardValue;
            if (guardValue.bypass()) { return g_ntUnloadDriverOriginal(serviceNamePointer); }
            const NTSTATUS statusValue = g_ntUnloadDriverOriginal(serviceNamePointer);
            wchar_t detailBuffer[ks::winapi_monitor::kMaxDetailChars] = {};
            AppendWideText(detailBuffer, L"service=");
            AppendUnicodeStringText(detailBuffer, serviceNamePointer);
            SendRawEventWithStatus(ks::winapi_monitor::EventCategory::Registry, L"ntdll", L"NtUnloadDriver", statusValue, detailBuffer);
            return statusValue;
        }

        NTSTATUS NTAPI HookedNtOpenProcess(PHANDLE processHandlePointer, ACCESS_MASK desiredAccess, POBJECT_ATTRIBUTES objectAttributesPointer, PKS_CLIENT_ID clientIdPointer)
        {
            ScopedHookGuard guardValue;
            if (guardValue.bypass()) { return g_ntOpenProcessOriginal(processHandlePointer, desiredAccess, objectAttributesPointer, clientIdPointer); }
            const NTSTATUS statusValue = g_ntOpenProcessOriginal(processHandlePointer, desiredAccess, objectAttributesPointer, clientIdPointer);
            wchar_t detailBuffer[ks::winapi_monitor::kMaxDetailChars] = {};
            BuildProcessHandleDetail(detailBuffer, nullptr, desiredAccess, clientIdPointer != nullptr ? reinterpret_cast<std::uint64_t>(clientIdPointer->UniqueProcess) : 0, processHandlePointer != nullptr ? *processHandlePointer : nullptr);
            SendRawEventWithStatus(ks::winapi_monitor::EventCategory::Process, L"ntdll", L"NtOpenProcess", statusValue, detailBuffer);
            return statusValue;
        }

        NTSTATUS NTAPI HookedNtOpenThread(PHANDLE threadHandlePointer, ACCESS_MASK desiredAccess, POBJECT_ATTRIBUTES objectAttributesPointer, PKS_CLIENT_ID clientIdPointer)
        {
            ScopedHookGuard guardValue;
            if (guardValue.bypass()) { return g_ntOpenThreadOriginal(threadHandlePointer, desiredAccess, objectAttributesPointer, clientIdPointer); }
            const NTSTATUS statusValue = g_ntOpenThreadOriginal(threadHandlePointer, desiredAccess, objectAttributesPointer, clientIdPointer);
            wchar_t detailBuffer[ks::winapi_monitor::kMaxDetailChars] = {};
            AppendWideText(detailBuffer, L"pid=");
            AppendHexText(detailBuffer, clientIdPointer != nullptr ? reinterpret_cast<std::uint64_t>(clientIdPointer->UniqueProcess) : 0);
            AppendWideText(detailBuffer, L" tid=");
            AppendHexText(detailBuffer, clientIdPointer != nullptr ? reinterpret_cast<std::uint64_t>(clientIdPointer->UniqueThread) : 0);
            AppendWideText(detailBuffer, L" access=");
            AppendHexText(detailBuffer, desiredAccess);
            AppendWideText(detailBuffer, L" handle=");
            AppendHexText(detailBuffer, threadHandlePointer != nullptr ? reinterpret_cast<std::uint64_t>(*threadHandlePointer) : 0);
            SendRawEventWithStatus(ks::winapi_monitor::EventCategory::Process, L"ntdll", L"NtOpenThread", statusValue, detailBuffer);
            return statusValue;
        }

        NTSTATUS NTAPI HookedNtTerminateProcess(HANDLE processHandle, NTSTATUS exitStatus)
        {
            ScopedHookGuard guardValue;
            if (guardValue.bypass()) { return g_ntTerminateProcessOriginal(processHandle, exitStatus); }
            const NTSTATUS statusValue = g_ntTerminateProcessOriginal(processHandle, exitStatus);
            wchar_t detailBuffer[ks::winapi_monitor::kMaxDetailChars] = {};
            AppendWideText(detailBuffer, L"process=");
            AppendHexText(detailBuffer, reinterpret_cast<std::uint64_t>(processHandle));
            AppendWideText(detailBuffer, L" exit=");
            AppendHexText(detailBuffer, static_cast<std::uint32_t>(exitStatus));
            SendRawEventWithStatus(ks::winapi_monitor::EventCategory::Process, L"ntdll", L"NtTerminateProcess", statusValue, detailBuffer);
            return statusValue;
        }

        NTSTATUS NTAPI HookedNtCreateUserProcess(PHANDLE processHandlePointer, PHANDLE threadHandlePointer, ACCESS_MASK processDesiredAccess, ACCESS_MASK threadDesiredAccess, POBJECT_ATTRIBUTES processObjectAttributesPointer, POBJECT_ATTRIBUTES threadObjectAttributesPointer, ULONG processFlags, ULONG threadFlags, PVOID processParametersPointer, PVOID createInfoPointer, PVOID attributeListPointer)
        {
            ScopedHookGuard guardValue;
            if (guardValue.bypass()) { return g_ntCreateUserProcessOriginal(processHandlePointer, threadHandlePointer, processDesiredAccess, threadDesiredAccess, processObjectAttributesPointer, threadObjectAttributesPointer, processFlags, threadFlags, processParametersPointer, createInfoPointer, attributeListPointer); }
            const NTSTATUS statusValue = g_ntCreateUserProcessOriginal(processHandlePointer, threadHandlePointer, processDesiredAccess, threadDesiredAccess, processObjectAttributesPointer, threadObjectAttributesPointer, processFlags, threadFlags, processParametersPointer, createInfoPointer, attributeListPointer);
            wchar_t detailBuffer[ks::winapi_monitor::kMaxDetailChars] = {};
            AppendWideText(detailBuffer, L"processAccess=");
            AppendHexText(detailBuffer, processDesiredAccess);
            AppendWideText(detailBuffer, L" threadAccess=");
            AppendHexText(detailBuffer, threadDesiredAccess);
            AppendWideText(detailBuffer, L" processFlags=");
            AppendHexText(detailBuffer, processFlags);
            AppendWideText(detailBuffer, L" threadFlags=");
            AppendHexText(detailBuffer, threadFlags);
            AppendWideText(detailBuffer, L" process=");
            AppendHexText(detailBuffer, processHandlePointer != nullptr ? reinterpret_cast<std::uint64_t>(*processHandlePointer) : 0);
            AppendWideText(detailBuffer, L" thread=");
            AppendHexText(detailBuffer, threadHandlePointer != nullptr ? reinterpret_cast<std::uint64_t>(*threadHandlePointer) : 0);
            SendRawEventWithStatus(ks::winapi_monitor::EventCategory::Process, L"ntdll", L"NtCreateUserProcess", statusValue, detailBuffer);
            return statusValue;
        }

        NTSTATUS NTAPI HookedNtCreateProcessEx(PHANDLE processHandlePointer, ACCESS_MASK desiredAccess, POBJECT_ATTRIBUTES objectAttributesPointer, HANDLE parentProcessHandle, ULONG flagsValue, HANDLE sectionHandle, HANDLE debugPortHandle, HANDLE exceptionPortHandle, BOOLEAN inJob)
        {
            ScopedHookGuard guardValue;
            if (guardValue.bypass()) { return g_ntCreateProcessExOriginal(processHandlePointer, desiredAccess, objectAttributesPointer, parentProcessHandle, flagsValue, sectionHandle, debugPortHandle, exceptionPortHandle, inJob); }
            const NTSTATUS statusValue = g_ntCreateProcessExOriginal(processHandlePointer, desiredAccess, objectAttributesPointer, parentProcessHandle, flagsValue, sectionHandle, debugPortHandle, exceptionPortHandle, inJob);
            wchar_t detailBuffer[ks::winapi_monitor::kMaxDetailChars] = {};
            AppendWideText(detailBuffer, L"parent=");
            AppendHexText(detailBuffer, reinterpret_cast<std::uint64_t>(parentProcessHandle));
            AppendWideText(detailBuffer, L" access=");
            AppendHexText(detailBuffer, desiredAccess);
            AppendWideText(detailBuffer, L" flags=");
            AppendHexText(detailBuffer, flagsValue);
            AppendWideText(detailBuffer, L" process=");
            AppendHexText(detailBuffer, processHandlePointer != nullptr ? reinterpret_cast<std::uint64_t>(*processHandlePointer) : 0);
            SendRawEventWithStatus(ks::winapi_monitor::EventCategory::Process, L"ntdll", L"NtCreateProcessEx", statusValue, detailBuffer);
            return statusValue;
        }

        NTSTATUS NTAPI HookedNtCreateThreadEx(PHANDLE threadHandlePointer, ACCESS_MASK desiredAccess, POBJECT_ATTRIBUTES objectAttributesPointer, HANDLE processHandle, PVOID startRoutinePointer, PVOID argumentPointer, ULONG createFlags, SIZE_T zeroBits, SIZE_T stackSize, SIZE_T maximumStackSize, PVOID attributeListPointer)
        {
            ScopedHookGuard guardValue;
            if (guardValue.bypass()) { return g_ntCreateThreadExOriginal(threadHandlePointer, desiredAccess, objectAttributesPointer, processHandle, startRoutinePointer, argumentPointer, createFlags, zeroBits, stackSize, maximumStackSize, attributeListPointer); }
            const NTSTATUS statusValue = g_ntCreateThreadExOriginal(threadHandlePointer, desiredAccess, objectAttributesPointer, processHandle, startRoutinePointer, argumentPointer, createFlags, zeroBits, stackSize, maximumStackSize, attributeListPointer);
            wchar_t detailBuffer[ks::winapi_monitor::kMaxDetailChars] = {};
            AppendWideText(detailBuffer, L"process=");
            AppendHexText(detailBuffer, reinterpret_cast<std::uint64_t>(processHandle));
            AppendWideText(detailBuffer, L" start=");
            AppendHexText(detailBuffer, reinterpret_cast<std::uint64_t>(startRoutinePointer));
            AppendWideText(detailBuffer, L" arg=");
            AppendHexText(detailBuffer, reinterpret_cast<std::uint64_t>(argumentPointer));
            AppendWideText(detailBuffer, L" flags=");
            AppendHexText(detailBuffer, createFlags);
            AppendWideText(detailBuffer, L" thread=");
            AppendHexText(detailBuffer, threadHandlePointer != nullptr ? reinterpret_cast<std::uint64_t>(*threadHandlePointer) : 0);
            SendRawEventWithStatus(ks::winapi_monitor::EventCategory::Process, L"ntdll", L"NtCreateThreadEx", statusValue, detailBuffer);
            return statusValue;
        }

        NTSTATUS NTAPI HookedNtAllocateVirtualMemory(HANDLE processHandle, PVOID* baseAddressPointer, ULONG_PTR zeroBits, PSIZE_T regionSizePointer, ULONG allocationType, ULONG protectValue)
        {
            ScopedHookGuard guardValue;
            if (guardValue.bypass()) { return g_ntAllocateVirtualMemoryOriginal(processHandle, baseAddressPointer, zeroBits, regionSizePointer, allocationType, protectValue); }
            const NTSTATUS statusValue = g_ntAllocateVirtualMemoryOriginal(processHandle, baseAddressPointer, zeroBits, regionSizePointer, allocationType, protectValue);
            wchar_t detailBuffer[ks::winapi_monitor::kMaxDetailChars] = {};
            BuildRemoteMemoryDetail(detailBuffer, processHandle, baseAddressPointer != nullptr ? *baseAddressPointer : nullptr, regionSizePointer != nullptr ? static_cast<std::uint64_t>(*regionSizePointer) : 0, allocationType, protectValue);
            SendRawEventWithStatus(ks::winapi_monitor::EventCategory::Process, L"ntdll", L"NtAllocateVirtualMemory", statusValue, detailBuffer);
            return statusValue;
        }

        NTSTATUS NTAPI HookedNtFreeVirtualMemory(HANDLE processHandle, PVOID* baseAddressPointer, PSIZE_T regionSizePointer, ULONG freeType)
        {
            ScopedHookGuard guardValue;
            if (guardValue.bypass()) { return g_ntFreeVirtualMemoryOriginal(processHandle, baseAddressPointer, regionSizePointer, freeType); }
            const NTSTATUS statusValue = g_ntFreeVirtualMemoryOriginal(processHandle, baseAddressPointer, regionSizePointer, freeType);
            wchar_t detailBuffer[ks::winapi_monitor::kMaxDetailChars] = {};
            BuildRemoteMemoryDetail(detailBuffer, processHandle, baseAddressPointer != nullptr ? *baseAddressPointer : nullptr, regionSizePointer != nullptr ? static_cast<std::uint64_t>(*regionSizePointer) : 0, freeType, 0);
            SendRawEventWithStatus(ks::winapi_monitor::EventCategory::Process, L"ntdll", L"NtFreeVirtualMemory", statusValue, detailBuffer);
            return statusValue;
        }

        NTSTATUS NTAPI HookedNtProtectVirtualMemory(HANDLE processHandle, PVOID* baseAddressPointer, PSIZE_T regionSizePointer, ULONG newProtect, PULONG oldProtectPointer)
        {
            ScopedHookGuard guardValue;
            if (guardValue.bypass()) { return g_ntProtectVirtualMemoryOriginal(processHandle, baseAddressPointer, regionSizePointer, newProtect, oldProtectPointer); }
            const NTSTATUS statusValue = g_ntProtectVirtualMemoryOriginal(processHandle, baseAddressPointer, regionSizePointer, newProtect, oldProtectPointer);
            wchar_t detailBuffer[ks::winapi_monitor::kMaxDetailChars] = {};
            BuildRemoteMemoryDetail(detailBuffer, processHandle, baseAddressPointer != nullptr ? *baseAddressPointer : nullptr, regionSizePointer != nullptr ? static_cast<std::uint64_t>(*regionSizePointer) : 0, oldProtectPointer != nullptr ? *oldProtectPointer : 0, newProtect);
            SendRawEventWithStatus(ks::winapi_monitor::EventCategory::Process, L"ntdll", L"NtProtectVirtualMemory", statusValue, detailBuffer);
            return statusValue;
        }

        NTSTATUS NTAPI HookedNtWriteVirtualMemory(HANDLE processHandle, PVOID baseAddress, PVOID bufferPointer, SIZE_T sizeValue, PSIZE_T bytesWrittenPointer)
        {
            ScopedHookGuard guardValue;
            if (guardValue.bypass()) { return g_ntWriteVirtualMemoryOriginal(processHandle, baseAddress, bufferPointer, sizeValue, bytesWrittenPointer); }
            const NTSTATUS statusValue = g_ntWriteVirtualMemoryOriginal(processHandle, baseAddress, bufferPointer, sizeValue, bytesWrittenPointer);
            wchar_t detailBuffer[ks::winapi_monitor::kMaxDetailChars] = {};
            BuildRemoteMemoryDetail(detailBuffer, processHandle, baseAddress, static_cast<std::uint64_t>(sizeValue), 0, 0);
            AppendWideText(detailBuffer, L" written=");
            AppendUnsignedText(detailBuffer, bytesWrittenPointer != nullptr ? static_cast<unsigned long long>(*bytesWrittenPointer) : 0);
            SendRawEventWithStatus(ks::winapi_monitor::EventCategory::Process, L"ntdll", L"NtWriteVirtualMemory", statusValue, detailBuffer);
            return statusValue;
        }

        NTSTATUS NTAPI HookedNtReadVirtualMemory(HANDLE processHandle, PVOID baseAddress, PVOID bufferPointer, SIZE_T sizeValue, PSIZE_T bytesReadPointer)
        {
            ScopedHookGuard guardValue;
            if (guardValue.bypass()) { return g_ntReadVirtualMemoryOriginal(processHandle, baseAddress, bufferPointer, sizeValue, bytesReadPointer); }
            const NTSTATUS statusValue = g_ntReadVirtualMemoryOriginal(processHandle, baseAddress, bufferPointer, sizeValue, bytesReadPointer);
            wchar_t detailBuffer[ks::winapi_monitor::kMaxDetailChars] = {};
            BuildRemoteMemoryDetail(detailBuffer, processHandle, baseAddress, static_cast<std::uint64_t>(sizeValue), 0, 0);
            AppendWideText(detailBuffer, L" read=");
            AppendUnsignedText(detailBuffer, bytesReadPointer != nullptr ? static_cast<unsigned long long>(*bytesReadPointer) : 0);
            SendRawEventWithStatus(ks::winapi_monitor::EventCategory::Process, L"ntdll", L"NtReadVirtualMemory", statusValue, detailBuffer);
            return statusValue;
        }

        NTSTATUS NTAPI HookedNtMapViewOfSection(HANDLE sectionHandle, HANDLE processHandle, PVOID* baseAddressPointer, ULONG_PTR zeroBits, SIZE_T commitSize, PLARGE_INTEGER sectionOffsetPointer, PSIZE_T viewSizePointer, DWORD inheritDisposition, ULONG allocationType, ULONG protectValue)
        {
            ScopedHookGuard guardValue;
            if (guardValue.bypass()) { return g_ntMapViewOfSectionOriginal(sectionHandle, processHandle, baseAddressPointer, zeroBits, commitSize, sectionOffsetPointer, viewSizePointer, inheritDisposition, allocationType, protectValue); }
            const NTSTATUS statusValue = g_ntMapViewOfSectionOriginal(sectionHandle, processHandle, baseAddressPointer, zeroBits, commitSize, sectionOffsetPointer, viewSizePointer, inheritDisposition, allocationType, protectValue);
            wchar_t detailBuffer[ks::winapi_monitor::kMaxDetailChars] = {};
            AppendWideText(detailBuffer, L"section=");
            AppendHexText(detailBuffer, reinterpret_cast<std::uint64_t>(sectionHandle));
            AppendWideText(detailBuffer, L" process=");
            AppendHexText(detailBuffer, reinterpret_cast<std::uint64_t>(processHandle));
            AppendWideText(detailBuffer, L" base=");
            AppendHexText(detailBuffer, baseAddressPointer != nullptr ? reinterpret_cast<std::uint64_t>(*baseAddressPointer) : 0);
            AppendWideText(detailBuffer, L" size=");
            AppendUnsignedText(detailBuffer, viewSizePointer != nullptr ? static_cast<unsigned long long>(*viewSizePointer) : 0);
            AppendWideText(detailBuffer, L" protect=");
            AppendHexText(detailBuffer, protectValue);
            SendRawEventWithStatus(ks::winapi_monitor::EventCategory::Process, L"ntdll", L"NtMapViewOfSection", statusValue, detailBuffer);
            return statusValue;
        }

        NTSTATUS NTAPI HookedNtUnmapViewOfSection(HANDLE processHandle, PVOID baseAddress)
        {
            ScopedHookGuard guardValue;
            if (guardValue.bypass()) { return g_ntUnmapViewOfSectionOriginal(processHandle, baseAddress); }
            const NTSTATUS statusValue = g_ntUnmapViewOfSectionOriginal(processHandle, baseAddress);
            wchar_t detailBuffer[ks::winapi_monitor::kMaxDetailChars] = {};
            BuildRemoteMemoryDetail(detailBuffer, processHandle, baseAddress, 0, 0, 0);
            SendRawEventWithStatus(ks::winapi_monitor::EventCategory::Process, L"ntdll", L"NtUnmapViewOfSection", statusValue, detailBuffer);
            return statusValue;
        }

        NTSTATUS NTAPI HookedNtDuplicateObject(HANDLE sourceProcessHandle, HANDLE sourceHandle, HANDLE targetProcessHandle, PHANDLE targetHandlePointer, ACCESS_MASK desiredAccess, ULONG handleAttributes, ULONG optionsValue)
        {
            ScopedHookGuard guardValue;
            if (guardValue.bypass()) { return g_ntDuplicateObjectOriginal(sourceProcessHandle, sourceHandle, targetProcessHandle, targetHandlePointer, desiredAccess, handleAttributes, optionsValue); }
            const NTSTATUS statusValue = g_ntDuplicateObjectOriginal(sourceProcessHandle, sourceHandle, targetProcessHandle, targetHandlePointer, desiredAccess, handleAttributes, optionsValue);
            wchar_t detailBuffer[ks::winapi_monitor::kMaxDetailChars] = {};
            AppendWideText(detailBuffer, L"sourceProcess=");
            AppendHexText(detailBuffer, reinterpret_cast<std::uint64_t>(sourceProcessHandle));
            AppendWideText(detailBuffer, L" sourceHandle=");
            AppendHexText(detailBuffer, reinterpret_cast<std::uint64_t>(sourceHandle));
            AppendWideText(detailBuffer, L" targetProcess=");
            AppendHexText(detailBuffer, reinterpret_cast<std::uint64_t>(targetProcessHandle));
            AppendWideText(detailBuffer, L" targetHandle=");
            AppendHexText(detailBuffer, targetHandlePointer != nullptr ? reinterpret_cast<std::uint64_t>(*targetHandlePointer) : 0);
            AppendWideText(detailBuffer, L" access=");
            AppendHexText(detailBuffer, desiredAccess);
            AppendWideText(detailBuffer, L" options=");
            AppendHexText(detailBuffer, optionsValue);
            SendRawEventWithStatus(ks::winapi_monitor::EventCategory::Process, L"ntdll", L"NtDuplicateObject", statusValue, detailBuffer);
            return statusValue;
        }

        // HookedNtQueryInformationProcess 作用：
        // - 输入：进程句柄、信息类和输出缓冲长度；
        // - 处理：记录直接进程信息查询，覆盖 PEB/调试/保护级别等原生查询路径；
        // - 返回：保持原始 NTSTATUS。
        NTSTATUS NTAPI HookedNtQueryInformationProcess(HANDLE processHandle, ULONG processInformationClass, PVOID processInformationPointer, ULONG processInformationLength, PULONG returnLengthPointer)
        {
            ScopedHookGuard guardValue;
            if (guardValue.bypass()) { return g_ntQueryInformationProcessOriginal(processHandle, processInformationClass, processInformationPointer, processInformationLength, returnLengthPointer); }
            const NTSTATUS statusValue = g_ntQueryInformationProcessOriginal(processHandle, processInformationClass, processInformationPointer, processInformationLength, returnLengthPointer);
            wchar_t detailBuffer[ks::winapi_monitor::kMaxDetailChars] = {};
            AppendWideText(detailBuffer, L"process=");
            AppendHexText(detailBuffer, reinterpret_cast<std::uint64_t>(processHandle));
            AppendWideText(detailBuffer, L" class=");
            AppendUnsignedText(detailBuffer, processInformationClass);
            AppendWideText(detailBuffer, L" size=");
            AppendUnsignedText(detailBuffer, returnLengthPointer != nullptr ? *returnLengthPointer : processInformationLength);
            SendRawEventWithStatus(ks::winapi_monitor::EventCategory::Process, L"ntdll", L"NtQueryInformationProcess", statusValue, detailBuffer);
            return statusValue;
        }

        // HookedNtSetInformationProcess 作用：
        // - 输入：进程句柄、信息类和输入缓冲长度；
        // - 处理：记录直接进程属性修改，例如保护/调试/优先级相关设置；
        // - 返回：保持原始 NTSTATUS。
        NTSTATUS NTAPI HookedNtSetInformationProcess(HANDLE processHandle, ULONG processInformationClass, PVOID processInformationPointer, ULONG processInformationLength)
        {
            ScopedHookGuard guardValue;
            if (guardValue.bypass()) { return g_ntSetInformationProcessOriginal(processHandle, processInformationClass, processInformationPointer, processInformationLength); }
            const NTSTATUS statusValue = g_ntSetInformationProcessOriginal(processHandle, processInformationClass, processInformationPointer, processInformationLength);
            wchar_t detailBuffer[ks::winapi_monitor::kMaxDetailChars] = {};
            AppendWideText(detailBuffer, L"process=");
            AppendHexText(detailBuffer, reinterpret_cast<std::uint64_t>(processHandle));
            AppendWideText(detailBuffer, L" class=");
            AppendUnsignedText(detailBuffer, processInformationClass);
            AppendWideText(detailBuffer, L" size=");
            AppendUnsignedText(detailBuffer, processInformationLength);
            SendRawEventWithStatus(ks::winapi_monitor::EventCategory::Process, L"ntdll", L"NtSetInformationProcess", statusValue, detailBuffer);
            return statusValue;
        }

        // HookedNtQueryVirtualMemory 作用：
        // - 输入：进程句柄、基址、信息类和输出缓冲长度；
        // - 处理：记录原生虚拟内存查询，补齐 VirtualQueryEx/Nt 层路径；
        // - 返回：保持原始 NTSTATUS。
        NTSTATUS NTAPI HookedNtQueryVirtualMemory(HANDLE processHandle, PVOID baseAddress, ULONG memoryInformationClass, PVOID memoryInformationPointer, SIZE_T memoryInformationLength, PSIZE_T returnLengthPointer)
        {
            ScopedHookGuard guardValue;
            if (guardValue.bypass()) { return g_ntQueryVirtualMemoryOriginal(processHandle, baseAddress, memoryInformationClass, memoryInformationPointer, memoryInformationLength, returnLengthPointer); }
            const NTSTATUS statusValue = g_ntQueryVirtualMemoryOriginal(processHandle, baseAddress, memoryInformationClass, memoryInformationPointer, memoryInformationLength, returnLengthPointer);
            wchar_t detailBuffer[ks::winapi_monitor::kMaxDetailChars] = {};
            BuildRemoteMemoryDetail(detailBuffer, processHandle, baseAddress, static_cast<std::uint64_t>(returnLengthPointer != nullptr ? *returnLengthPointer : memoryInformationLength), memoryInformationClass, 0);
            SendRawEventWithStatus(ks::winapi_monitor::EventCategory::Process, L"ntdll", L"NtQueryVirtualMemory", statusValue, detailBuffer);
            return statusValue;
        }

        // HookedNtCreateSection 作用：
        // - 输入：Section 结果句柄、访问掩码、对象属性、最大大小、保护、属性和文件句柄；
        // - 处理：记录 Section 创建，覆盖 section-map 注入链路的前置步骤；
        // - 返回：保持原始 NTSTATUS。
        NTSTATUS NTAPI HookedNtCreateSection(PHANDLE sectionHandlePointer, ACCESS_MASK desiredAccess, POBJECT_ATTRIBUTES objectAttributesPointer, PLARGE_INTEGER maximumSizePointer, ULONG sectionPageProtection, ULONG allocationAttributes, HANDLE fileHandle)
        {
            ScopedHookGuard guardValue;
            if (guardValue.bypass()) { return g_ntCreateSectionOriginal(sectionHandlePointer, desiredAccess, objectAttributesPointer, maximumSizePointer, sectionPageProtection, allocationAttributes, fileHandle); }
            const NTSTATUS statusValue = g_ntCreateSectionOriginal(sectionHandlePointer, desiredAccess, objectAttributesPointer, maximumSizePointer, sectionPageProtection, allocationAttributes, fileHandle);
            wchar_t detailBuffer[ks::winapi_monitor::kMaxDetailChars] = {};
            AppendWideText(detailBuffer, L"section=");
            AppendHexText(detailBuffer, sectionHandlePointer != nullptr ? reinterpret_cast<std::uint64_t>(*sectionHandlePointer) : 0);
            AppendWideText(detailBuffer, L" access=");
            AppendHexText(detailBuffer, desiredAccess);
            AppendWideText(detailBuffer, L" protect=");
            AppendHexText(detailBuffer, sectionPageProtection);
            AppendWideText(detailBuffer, L" attrs=");
            AppendHexText(detailBuffer, allocationAttributes);
            AppendWideText(detailBuffer, L" file=");
            AppendHexText(detailBuffer, reinterpret_cast<std::uint64_t>(fileHandle));
            SendRawEventWithStatus(ks::winapi_monitor::EventCategory::Process, L"ntdll", L"NtCreateSection", statusValue, detailBuffer);
            return statusValue;
        }

        // HookedNtOpenSection 作用：
        // - 输入：Section 结果句柄、访问掩码和对象属性；
        // - 处理：记录打开已存在 Section 对象；
        // - 返回：保持原始 NTSTATUS。
        NTSTATUS NTAPI HookedNtOpenSection(PHANDLE sectionHandlePointer, ACCESS_MASK desiredAccess, POBJECT_ATTRIBUTES objectAttributesPointer)
        {
            ScopedHookGuard guardValue;
            if (guardValue.bypass()) { return g_ntOpenSectionOriginal(sectionHandlePointer, desiredAccess, objectAttributesPointer); }
            const NTSTATUS statusValue = g_ntOpenSectionOriginal(sectionHandlePointer, desiredAccess, objectAttributesPointer);
            wchar_t detailBuffer[ks::winapi_monitor::kMaxDetailChars] = {};
            AppendWideText(detailBuffer, L"object=");
            AppendObjectNameText(detailBuffer, objectAttributesPointer);
            AppendWideText(detailBuffer, L" access=");
            AppendHexText(detailBuffer, desiredAccess);
            AppendWideText(detailBuffer, L" section=");
            AppendHexText(detailBuffer, sectionHandlePointer != nullptr ? reinterpret_cast<std::uint64_t>(*sectionHandlePointer) : 0);
            SendRawEventWithStatus(ks::winapi_monitor::EventCategory::Process, L"ntdll", L"NtOpenSection", statusValue, detailBuffer);
            return statusValue;
        }

        // HookedNtQueueApcThread 作用：
        // - 输入：线程句柄、APC 例程和三个参数；
        // - 处理：记录原生 APC 排队，覆盖绕过 QueueUserAPC 的注入路径；
        // - 返回：保持原始 NTSTATUS。
        NTSTATUS NTAPI HookedNtQueueApcThread(HANDLE threadHandle, PVOID apcRoutinePointer, PVOID argument1Pointer, PVOID argument2Pointer, PVOID argument3Pointer)
        {
            ScopedHookGuard guardValue;
            if (guardValue.bypass()) { return g_ntQueueApcThreadOriginal(threadHandle, apcRoutinePointer, argument1Pointer, argument2Pointer, argument3Pointer); }
            const NTSTATUS statusValue = g_ntQueueApcThreadOriginal(threadHandle, apcRoutinePointer, argument1Pointer, argument2Pointer, argument3Pointer);
            wchar_t detailBuffer[ks::winapi_monitor::kMaxDetailChars] = {};
            AppendWideText(detailBuffer, L"thread=");
            AppendHexText(detailBuffer, reinterpret_cast<std::uint64_t>(threadHandle));
            AppendWideText(detailBuffer, L" apc=");
            AppendHexText(detailBuffer, reinterpret_cast<std::uint64_t>(apcRoutinePointer));
            AppendWideText(detailBuffer, L" arg1=");
            AppendHexText(detailBuffer, reinterpret_cast<std::uint64_t>(argument1Pointer));
            SendRawEventWithStatus(ks::winapi_monitor::EventCategory::Process, L"ntdll", L"NtQueueApcThread", statusValue, detailBuffer);
            return statusValue;
        }

        // HookedNtQueueApcThreadEx 作用：
        // - 输入：线程句柄、Reserve 句柄、APC 例程和三个参数；
        // - 处理：记录扩展 APC 排队路径；
        // - 返回：保持原始 NTSTATUS。
        NTSTATUS NTAPI HookedNtQueueApcThreadEx(HANDLE threadHandle, HANDLE reserveHandle, PVOID apcRoutinePointer, PVOID argument1Pointer, PVOID argument2Pointer, PVOID argument3Pointer)
        {
            ScopedHookGuard guardValue;
            if (guardValue.bypass()) { return g_ntQueueApcThreadExOriginal(threadHandle, reserveHandle, apcRoutinePointer, argument1Pointer, argument2Pointer, argument3Pointer); }
            const NTSTATUS statusValue = g_ntQueueApcThreadExOriginal(threadHandle, reserveHandle, apcRoutinePointer, argument1Pointer, argument2Pointer, argument3Pointer);
            wchar_t detailBuffer[ks::winapi_monitor::kMaxDetailChars] = {};
            AppendWideText(detailBuffer, L"thread=");
            AppendHexText(detailBuffer, reinterpret_cast<std::uint64_t>(threadHandle));
            AppendWideText(detailBuffer, L" reserve=");
            AppendHexText(detailBuffer, reinterpret_cast<std::uint64_t>(reserveHandle));
            AppendWideText(detailBuffer, L" apc=");
            AppendHexText(detailBuffer, reinterpret_cast<std::uint64_t>(apcRoutinePointer));
            AppendWideText(detailBuffer, L" arg1=");
            AppendHexText(detailBuffer, reinterpret_cast<std::uint64_t>(argument1Pointer));
            SendRawEventWithStatus(ks::winapi_monitor::EventCategory::Process, L"ntdll", L"NtQueueApcThreadEx", statusValue, detailBuffer);
            return statusValue;
        }

        // HookedNtSuspendThread 作用：
        // - 输入：线程句柄和可选旧挂起计数输出；
        // - 处理：记录 Nt 层线程挂起；
        // - 返回：保持原始 NTSTATUS。
        NTSTATUS NTAPI HookedNtSuspendThread(HANDLE threadHandle, PULONG previousSuspendCountPointer)
        {
            ScopedHookGuard guardValue;
            if (guardValue.bypass()) { return g_ntSuspendThreadOriginal(threadHandle, previousSuspendCountPointer); }
            const NTSTATUS statusValue = g_ntSuspendThreadOriginal(threadHandle, previousSuspendCountPointer);
            wchar_t detailBuffer[ks::winapi_monitor::kMaxDetailChars] = {};
            AppendWideText(detailBuffer, L"thread=");
            AppendHexText(detailBuffer, reinterpret_cast<std::uint64_t>(threadHandle));
            AppendWideText(detailBuffer, L" previous=");
            AppendUnsignedText(detailBuffer, previousSuspendCountPointer != nullptr ? *previousSuspendCountPointer : 0);
            SendRawEventWithStatus(ks::winapi_monitor::EventCategory::Process, L"ntdll", L"NtSuspendThread", statusValue, detailBuffer);
            return statusValue;
        }

        // HookedNtResumeThread 作用：
        // - 输入：线程句柄和可选旧挂起计数输出；
        // - 处理：记录 Nt 层线程恢复；
        // - 返回：保持原始 NTSTATUS。
        NTSTATUS NTAPI HookedNtResumeThread(HANDLE threadHandle, PULONG previousSuspendCountPointer)
        {
            ScopedHookGuard guardValue;
            if (guardValue.bypass()) { return g_ntResumeThreadOriginal(threadHandle, previousSuspendCountPointer); }
            const NTSTATUS statusValue = g_ntResumeThreadOriginal(threadHandle, previousSuspendCountPointer);
            wchar_t detailBuffer[ks::winapi_monitor::kMaxDetailChars] = {};
            AppendWideText(detailBuffer, L"thread=");
            AppendHexText(detailBuffer, reinterpret_cast<std::uint64_t>(threadHandle));
            AppendWideText(detailBuffer, L" previous=");
            AppendUnsignedText(detailBuffer, previousSuspendCountPointer != nullptr ? *previousSuspendCountPointer : 0);
            SendRawEventWithStatus(ks::winapi_monitor::EventCategory::Process, L"ntdll", L"NtResumeThread", statusValue, detailBuffer);
            return statusValue;
        }

        // HookedNtGetContextThread 作用：
        // - 输入：线程句柄和 CONTEXT 输出缓冲；
        // - 处理：记录 Nt 层上下文读取；
        // - 返回：保持原始 NTSTATUS。
        NTSTATUS NTAPI HookedNtGetContextThread(HANDLE threadHandle, PCONTEXT contextPointer)
        {
            ScopedHookGuard guardValue;
            if (guardValue.bypass()) { return g_ntGetContextThreadOriginal(threadHandle, contextPointer); }
            const NTSTATUS statusValue = g_ntGetContextThreadOriginal(threadHandle, contextPointer);
            wchar_t detailBuffer[ks::winapi_monitor::kMaxDetailChars] = {};
            AppendWideText(detailBuffer, L"thread=");
            AppendHexText(detailBuffer, reinterpret_cast<std::uint64_t>(threadHandle));
            AppendWideText(detailBuffer, L" flags=");
            AppendHexText(detailBuffer, contextPointer != nullptr ? contextPointer->ContextFlags : 0);
            SendRawEventWithStatus(ks::winapi_monitor::EventCategory::Process, L"ntdll", L"NtGetContextThread", statusValue, detailBuffer);
            return statusValue;
        }

        // HookedNtSetContextThread 作用：
        // - 输入：线程句柄和 CONTEXT 输入缓冲；
        // - 处理：记录 Nt 层上下文写入；
        // - 返回：保持原始 NTSTATUS。
        NTSTATUS NTAPI HookedNtSetContextThread(HANDLE threadHandle, PCONTEXT contextPointer)
        {
            ScopedHookGuard guardValue;
            if (guardValue.bypass()) { return g_ntSetContextThreadOriginal(threadHandle, contextPointer); }
            const NTSTATUS statusValue = g_ntSetContextThreadOriginal(threadHandle, contextPointer);
            wchar_t detailBuffer[ks::winapi_monitor::kMaxDetailChars] = {};
            AppendWideText(detailBuffer, L"thread=");
            AppendHexText(detailBuffer, reinterpret_cast<std::uint64_t>(threadHandle));
            AppendWideText(detailBuffer, L" flags=");
            AppendHexText(detailBuffer, contextPointer != nullptr ? contextPointer->ContextFlags : 0);
#if defined(_M_X64)
            AppendWideText(detailBuffer, L" rip=");
            AppendHexText(detailBuffer, contextPointer != nullptr ? contextPointer->Rip : 0);
#endif
            SendRawEventWithStatus(ks::winapi_monitor::EventCategory::Process, L"ntdll", L"NtSetContextThread", statusValue, detailBuffer);
            return statusValue;
        }

        // HookedSocket 作用：
        // - 输入：协议族、socket 类型和协议号；
        // - 处理：记录基础 socket 创建，补齐 connect/send 前的网络对象创建行为；
        // - 返回：保持原始 SOCKET，并在失败时恢复 WSA 错误码。
        SOCKET WSAAPI HookedSocket(int addressFamily, int socketType, int protocolValue)
        {
            ScopedHookGuard guardValue;
            if (guardValue.bypass()) { return g_socketOriginal(addressFamily, socketType, protocolValue); }
            const SOCKET resultSocket = g_socketOriginal(addressFamily, socketType, protocolValue);
            const int errorValue = resultSocket != INVALID_SOCKET ? 0 : ::WSAGetLastError();
            wchar_t detailBuffer[ks::winapi_monitor::kMaxDetailChars] = {};
            AppendWideText(detailBuffer, L"af=");
            AppendUnsignedText(detailBuffer, static_cast<unsigned long long>(addressFamily));
            AppendWideText(detailBuffer, L" type=");
            AppendUnsignedText(detailBuffer, static_cast<unsigned long long>(socketType));
            AppendWideText(detailBuffer, L" proto=");
            AppendUnsignedText(detailBuffer, static_cast<unsigned long long>(protocolValue));
            AppendWideText(detailBuffer, L" socket=");
            AppendHexText(detailBuffer, static_cast<std::uint64_t>(resultSocket));
            SendMonitorEventRaw(ks::winapi_monitor::EventCategory::Network, L"Ws2_32", L"socket", errorValue, detailBuffer);
            if (resultSocket == INVALID_SOCKET) { ::WSASetLastError(errorValue); }
            return resultSocket;
        }

        // HookedWSASocketW 作用：
        // - 输入：协议族、类型、协议、协议信息、组和标志；
        // - 处理：记录扩展 socket 创建，包含 overlapped/flag 信息；
        // - 返回：保持原始 SOCKET，并在失败时恢复 WSA 错误码。
        SOCKET WSAAPI HookedWSASocketW(int addressFamily, int socketType, int protocolValue, LPWSAPROTOCOL_INFOW protocolInfoPointer, GROUP groupValue, DWORD flagsValue)
        {
            ScopedHookGuard guardValue;
            if (guardValue.bypass()) { return g_wsaSocketWOriginal(addressFamily, socketType, protocolValue, protocolInfoPointer, groupValue, flagsValue); }
            const SOCKET resultSocket = g_wsaSocketWOriginal(addressFamily, socketType, protocolValue, protocolInfoPointer, groupValue, flagsValue);
            const int errorValue = resultSocket != INVALID_SOCKET ? 0 : ::WSAGetLastError();
            wchar_t detailBuffer[ks::winapi_monitor::kMaxDetailChars] = {};
            AppendWideText(detailBuffer, L"af=");
            AppendUnsignedText(detailBuffer, static_cast<unsigned long long>(addressFamily));
            AppendWideText(detailBuffer, L" type=");
            AppendUnsignedText(detailBuffer, static_cast<unsigned long long>(socketType));
            AppendWideText(detailBuffer, L" proto=");
            AppendUnsignedText(detailBuffer, static_cast<unsigned long long>(protocolValue));
            AppendWideText(detailBuffer, L" flags=");
            AppendHexText(detailBuffer, flagsValue);
            AppendWideText(detailBuffer, L" socket=");
            AppendHexText(detailBuffer, static_cast<std::uint64_t>(resultSocket));
            SendMonitorEventRaw(ks::winapi_monitor::EventCategory::Network, L"Ws2_32", L"WSASocketW", errorValue, detailBuffer);
            if (resultSocket == INVALID_SOCKET) { ::WSASetLastError(errorValue); }
            return resultSocket;
        }

        // HookedWSASocketA 作用：
        // - 输入：ANSI 版本 WSASocket 参数；
        // - 处理：记录扩展 socket 创建；
        // - 返回：保持原始 SOCKET，并在失败时恢复 WSA 错误码。
        SOCKET WSAAPI HookedWSASocketA(int addressFamily, int socketType, int protocolValue, LPWSAPROTOCOL_INFOA protocolInfoPointer, GROUP groupValue, DWORD flagsValue)
        {
            ScopedHookGuard guardValue;
            if (guardValue.bypass()) { return g_wsaSocketAOriginal(addressFamily, socketType, protocolValue, protocolInfoPointer, groupValue, flagsValue); }
            const SOCKET resultSocket = g_wsaSocketAOriginal(addressFamily, socketType, protocolValue, protocolInfoPointer, groupValue, flagsValue);
            const int errorValue = resultSocket != INVALID_SOCKET ? 0 : ::WSAGetLastError();
            wchar_t detailBuffer[ks::winapi_monitor::kMaxDetailChars] = {};
            AppendWideText(detailBuffer, L"af=");
            AppendUnsignedText(detailBuffer, static_cast<unsigned long long>(addressFamily));
            AppendWideText(detailBuffer, L" type=");
            AppendUnsignedText(detailBuffer, static_cast<unsigned long long>(socketType));
            AppendWideText(detailBuffer, L" proto=");
            AppendUnsignedText(detailBuffer, static_cast<unsigned long long>(protocolValue));
            AppendWideText(detailBuffer, L" flags=");
            AppendHexText(detailBuffer, flagsValue);
            AppendWideText(detailBuffer, L" socket=");
            AppendHexText(detailBuffer, static_cast<std::uint64_t>(resultSocket));
            SendMonitorEventRaw(ks::winapi_monitor::EventCategory::Network, L"Ws2_32", L"WSASocketA", errorValue, detailBuffer);
            if (resultSocket == INVALID_SOCKET) { ::WSASetLastError(errorValue); }
            return resultSocket;
        }

        // HookedCloseSocket 作用：
        // - 输入：socket 句柄；
        // - 处理：记录网络句柄关闭；
        // - 返回：保持 closesocket 的 int 结果，并在失败时恢复 WSA 错误码。
        int WSAAPI HookedCloseSocket(SOCKET socketValue)
        {
            ScopedHookGuard guardValue;
            if (guardValue.bypass()) { return g_closeSocketOriginal(socketValue); }
            const int resultValue = g_closeSocketOriginal(socketValue);
            const int errorValue = resultValue == 0 ? 0 : ::WSAGetLastError();
            wchar_t detailBuffer[ks::winapi_monitor::kMaxDetailChars] = {};
            AppendWideText(detailBuffer, L"socket=");
            AppendHexText(detailBuffer, static_cast<std::uint64_t>(socketValue));
            SendMonitorEventRaw(ks::winapi_monitor::EventCategory::Network, L"Ws2_32", L"closesocket", errorValue, detailBuffer);
            if (resultValue != 0) { ::WSASetLastError(errorValue); }
            return resultValue;
        }

        // HookedShutdown 作用：
        // - 输入：socket 句柄和关闭方向；
        // - 处理：记录主动半关闭/全关闭操作；
        // - 返回：保持 shutdown 的 int 结果，并在失败时恢复 WSA 错误码。
        int WSAAPI HookedShutdown(SOCKET socketValue, int howValue)
        {
            ScopedHookGuard guardValue;
            if (guardValue.bypass()) { return g_shutdownOriginal(socketValue, howValue); }
            const int resultValue = g_shutdownOriginal(socketValue, howValue);
            const int errorValue = resultValue == 0 ? 0 : ::WSAGetLastError();
            wchar_t detailBuffer[ks::winapi_monitor::kMaxDetailChars] = {};
            AppendWideText(detailBuffer, L"socket=");
            AppendHexText(detailBuffer, static_cast<std::uint64_t>(socketValue));
            AppendWideText(detailBuffer, L" how=");
            AppendUnsignedText(detailBuffer, static_cast<unsigned long long>(howValue));
            SendMonitorEventRaw(ks::winapi_monitor::EventCategory::Network, L"Ws2_32", L"shutdown", errorValue, detailBuffer);
            if (resultValue != 0) { ::WSASetLastError(errorValue); }
            return resultValue;
        }

        int WSAAPI HookedConnect(SOCKET socketValue, const sockaddr* namePointer, int nameLength)
        {
            ScopedHookGuard guardValue;
            if (guardValue.bypass())
            {
                return g_connectOriginal(socketValue, namePointer, nameLength);
            }

            const int resultValue = g_connectOriginal(socketValue, namePointer, nameLength);
            const int errorValue = resultValue == 0 ? 0 : ::WSAGetLastError();
            wchar_t detailBuffer[ks::winapi_monitor::kMaxDetailChars] = {};
            BuildSocketDetail(detailBuffer, L"connect", socketValue, 0, 0, 0, namePointer, nameLength);
            SendMonitorEventRaw(
                ks::winapi_monitor::EventCategory::Network,
                L"Ws2_32",
                L"connect",
                errorValue,
                detailBuffer);
            if (resultValue != 0)
            {
                ::WSASetLastError(errorValue);
            }
            return resultValue;
        }

        int WSAAPI HookedWSAConnect(SOCKET socketValue, const sockaddr* namePointer, int nameLength, LPWSABUF callerDataPointer, LPWSABUF calleeDataPointer, LPQOS socketQosPointer, LPQOS groupQosPointer)
        {
            (void)callerDataPointer;
            (void)calleeDataPointer;
            (void)socketQosPointer;
            (void)groupQosPointer;
            ScopedHookGuard guardValue;
            if (guardValue.bypass())
            {
                return g_wsaConnectOriginal(socketValue, namePointer, nameLength, callerDataPointer, calleeDataPointer, socketQosPointer, groupQosPointer);
            }

            const int resultValue = g_wsaConnectOriginal(socketValue, namePointer, nameLength, callerDataPointer, calleeDataPointer, socketQosPointer, groupQosPointer);
            const int errorValue = resultValue == 0 ? 0 : ::WSAGetLastError();
            wchar_t detailBuffer[ks::winapi_monitor::kMaxDetailChars] = {};
            BuildSocketDetail(detailBuffer, L"connect", socketValue, 0, 0, 0, namePointer, nameLength);
            SendMonitorEventRaw(
                ks::winapi_monitor::EventCategory::Network,
                L"Ws2_32",
                L"WSAConnect",
                errorValue,
                detailBuffer);
            if (resultValue != 0)
            {
                ::WSASetLastError(errorValue);
            }
            return resultValue;
        }

        int WSAAPI HookedSend(SOCKET socketValue, const char* bufferPointer, int bufferLength, int flagsValue)
        {
            (void)bufferPointer;
            ScopedHookGuard guardValue;
            if (guardValue.bypass())
            {
                return g_sendOriginal(socketValue, bufferPointer, bufferLength, flagsValue);
            }

            const int resultValue = g_sendOriginal(socketValue, bufferPointer, bufferLength, flagsValue);
            const int errorValue = resultValue >= 0 ? 0 : ::WSAGetLastError();
            wchar_t detailBuffer[ks::winapi_monitor::kMaxDetailChars] = {};
            BuildSocketDetail(detailBuffer, L"send", socketValue, static_cast<std::uint64_t>(bufferLength < 0 ? 0 : bufferLength), resultValue, static_cast<DWORD>(flagsValue));
            SendMonitorEventRaw(
                ks::winapi_monitor::EventCategory::Network,
                L"Ws2_32",
                L"send",
                errorValue,
                detailBuffer);
            if (resultValue < 0)
            {
                ::WSASetLastError(errorValue);
            }
            return resultValue;
        }

        int WSAAPI HookedWSASend(SOCKET socketValue, LPWSABUF buffersPointer, DWORD bufferCount, LPDWORD bytesSentPointer, DWORD flagsValue, LPWSAOVERLAPPED overlappedPointer, LPWSAOVERLAPPED_COMPLETION_ROUTINE completionRoutinePointer)
        {
            ScopedHookGuard guardValue;
            if (guardValue.bypass())
            {
                return g_wsaSendOriginal(socketValue, buffersPointer, bufferCount, bytesSentPointer, flagsValue, overlappedPointer, completionRoutinePointer);
            }

            const std::uint64_t requestLength = SumWsaBufferLength(buffersPointer, bufferCount);
            const int resultValue = g_wsaSendOriginal(socketValue, buffersPointer, bufferCount, bytesSentPointer, flagsValue, overlappedPointer, completionRoutinePointer);
            const int errorValue = resultValue == 0 ? 0 : ::WSAGetLastError();
            const DWORD sentValue = bytesSentPointer != nullptr ? *bytesSentPointer : 0;
            wchar_t detailBuffer[ks::winapi_monitor::kMaxDetailChars] = {};
            BuildSocketDetail(detailBuffer, L"send", socketValue, requestLength, sentValue, flagsValue);
            SendMonitorEventRaw(
                ks::winapi_monitor::EventCategory::Network,
                L"Ws2_32",
                L"WSASend",
                errorValue,
                detailBuffer);
            if (resultValue != 0)
            {
                ::WSASetLastError(errorValue);
            }
            return resultValue;
        }

        int WSAAPI HookedSendTo(SOCKET socketValue, const char* bufferPointer, int bufferLength, int flagsValue, const sockaddr* toPointer, int toLength)
        {
            (void)bufferPointer;
            ScopedHookGuard guardValue;
            if (guardValue.bypass())
            {
                return g_sendToOriginal(socketValue, bufferPointer, bufferLength, flagsValue, toPointer, toLength);
            }

            const int resultValue = g_sendToOriginal(socketValue, bufferPointer, bufferLength, flagsValue, toPointer, toLength);
            const int errorValue = resultValue >= 0 ? 0 : ::WSAGetLastError();
            wchar_t detailBuffer[ks::winapi_monitor::kMaxDetailChars] = {};
            BuildSocketDetail(detailBuffer, L"sendto", socketValue, static_cast<std::uint64_t>(bufferLength < 0 ? 0 : bufferLength), resultValue, static_cast<DWORD>(flagsValue), toPointer, toLength);
            SendMonitorEventRaw(
                ks::winapi_monitor::EventCategory::Network,
                L"Ws2_32",
                L"sendto",
                errorValue,
                detailBuffer);
            if (resultValue < 0)
            {
                ::WSASetLastError(errorValue);
            }
            return resultValue;
        }

        int WSAAPI HookedRecv(SOCKET socketValue, char* bufferPointer, int bufferLength, int flagsValue)
        {
            (void)bufferPointer;
            ScopedHookGuard guardValue;
            if (guardValue.bypass())
            {
                return g_recvOriginal(socketValue, bufferPointer, bufferLength, flagsValue);
            }

            const int resultValue = g_recvOriginal(socketValue, bufferPointer, bufferLength, flagsValue);
            const int errorValue = resultValue >= 0 ? 0 : ::WSAGetLastError();
            wchar_t detailBuffer[ks::winapi_monitor::kMaxDetailChars] = {};
            BuildSocketDetail(detailBuffer, L"recv", socketValue, static_cast<std::uint64_t>(bufferLength < 0 ? 0 : bufferLength), resultValue, static_cast<DWORD>(flagsValue));
            SendMonitorEventRaw(
                ks::winapi_monitor::EventCategory::Network,
                L"Ws2_32",
                L"recv",
                errorValue,
                detailBuffer);
            if (resultValue < 0)
            {
                ::WSASetLastError(errorValue);
            }
            return resultValue;
        }

        int WSAAPI HookedWSARecv(SOCKET socketValue, LPWSABUF buffersPointer, DWORD bufferCount, LPDWORD bytesReceivedPointer, LPDWORD flagsPointer, LPWSAOVERLAPPED overlappedPointer, LPWSAOVERLAPPED_COMPLETION_ROUTINE completionRoutinePointer)
        {
            ScopedHookGuard guardValue;
            if (guardValue.bypass())
            {
                return g_wsaRecvOriginal(socketValue, buffersPointer, bufferCount, bytesReceivedPointer, flagsPointer, overlappedPointer, completionRoutinePointer);
            }

            const std::uint64_t requestLength = SumWsaBufferLength(buffersPointer, bufferCount);
            const int resultValue = g_wsaRecvOriginal(socketValue, buffersPointer, bufferCount, bytesReceivedPointer, flagsPointer, overlappedPointer, completionRoutinePointer);
            const int errorValue = resultValue == 0 ? 0 : ::WSAGetLastError();
            const DWORD receivedValue = bytesReceivedPointer != nullptr ? *bytesReceivedPointer : 0;
            const DWORD flagsValue = flagsPointer != nullptr ? *flagsPointer : 0;
            wchar_t detailBuffer[ks::winapi_monitor::kMaxDetailChars] = {};
            BuildSocketDetail(detailBuffer, L"recv", socketValue, requestLength, receivedValue, flagsValue);
            SendMonitorEventRaw(
                ks::winapi_monitor::EventCategory::Network,
                L"Ws2_32",
                L"WSARecv",
                errorValue,
                detailBuffer);
            if (resultValue != 0)
            {
                ::WSASetLastError(errorValue);
            }
            return resultValue;
        }

        int WSAAPI HookedRecvFrom(SOCKET socketValue, char* bufferPointer, int bufferLength, int flagsValue, sockaddr* fromPointer, int* fromLengthPointer)
        {
            (void)bufferPointer;
            ScopedHookGuard guardValue;
            if (guardValue.bypass())
            {
                return g_recvFromOriginal(socketValue, bufferPointer, bufferLength, flagsValue, fromPointer, fromLengthPointer);
            }

            const int resultValue = g_recvFromOriginal(socketValue, bufferPointer, bufferLength, flagsValue, fromPointer, fromLengthPointer);
            const int errorValue = resultValue >= 0 ? 0 : ::WSAGetLastError();
            const int fromLength = fromLengthPointer != nullptr ? *fromLengthPointer : 0;
            wchar_t detailBuffer[ks::winapi_monitor::kMaxDetailChars] = {};
            BuildSocketDetail(detailBuffer, L"recvfrom", socketValue, static_cast<std::uint64_t>(bufferLength < 0 ? 0 : bufferLength), resultValue, static_cast<DWORD>(flagsValue), fromPointer, fromLength);
            SendMonitorEventRaw(
                ks::winapi_monitor::EventCategory::Network,
                L"Ws2_32",
                L"recvfrom",
                errorValue,
                detailBuffer);
            if (resultValue < 0)
            {
                ::WSASetLastError(errorValue);
            }
            return resultValue;
        }

        int WSAAPI HookedBind(SOCKET socketValue, const sockaddr* namePointer, int nameLength)
        {
            ScopedHookGuard guardValue;
            if (guardValue.bypass())
            {
                return g_bindOriginal(socketValue, namePointer, nameLength);
            }

            const int resultValue = g_bindOriginal(socketValue, namePointer, nameLength);
            const int errorValue = resultValue == 0 ? 0 : ::WSAGetLastError();
            wchar_t detailBuffer[ks::winapi_monitor::kMaxDetailChars] = {};
            BuildSocketDetail(detailBuffer, L"bind", socketValue, 0, 0, 0, namePointer, nameLength);
            SendMonitorEventRaw(
                ks::winapi_monitor::EventCategory::Network,
                L"Ws2_32",
                L"bind",
                errorValue,
                detailBuffer);
            if (resultValue != 0)
            {
                ::WSASetLastError(errorValue);
            }
            return resultValue;
        }

        int WSAAPI HookedListen(SOCKET socketValue, int backlogValue)
        {
            ScopedHookGuard guardValue;
            if (guardValue.bypass())
            {
                return g_listenOriginal(socketValue, backlogValue);
            }

            const int resultValue = g_listenOriginal(socketValue, backlogValue);
            const int errorValue = resultValue == 0 ? 0 : ::WSAGetLastError();
            wchar_t detailBuffer[ks::winapi_monitor::kMaxDetailChars] = {};
            detailBuffer[0] = L'\0';
            AppendWideText(detailBuffer, L"socket=");
            AppendHexText(detailBuffer, static_cast<std::uint64_t>(socketValue));
            AppendWideText(detailBuffer, L" backlog=");
            AppendUnsignedText(detailBuffer, static_cast<unsigned long long>(backlogValue < 0 ? 0 : backlogValue));
            SendMonitorEventRaw(
                ks::winapi_monitor::EventCategory::Network,
                L"Ws2_32",
                L"listen",
                errorValue,
                detailBuffer);
            if (resultValue != 0)
            {
                ::WSASetLastError(errorValue);
            }
            return resultValue;
        }

        SOCKET WSAAPI HookedAccept(SOCKET socketValue, sockaddr* addressPointer, int* addressLengthPointer)
        {
            ScopedHookGuard guardValue;
            if (guardValue.bypass())
            {
                return g_acceptOriginal(socketValue, addressPointer, addressLengthPointer);
            }

            const SOCKET resultSocket = g_acceptOriginal(socketValue, addressPointer, addressLengthPointer);
            const int errorValue = resultSocket != INVALID_SOCKET ? 0 : ::WSAGetLastError();
            const int addressLength = addressLengthPointer != nullptr ? *addressLengthPointer : 0;
            wchar_t detailBuffer[ks::winapi_monitor::kMaxDetailChars] = {};
            BuildSocketDetail(detailBuffer, L"accept", socketValue, 0, static_cast<long long>(resultSocket == INVALID_SOCKET ? 0 : resultSocket), 0, addressPointer, addressLength);
            SendMonitorEventRaw(
                ks::winapi_monitor::EventCategory::Network,
                L"Ws2_32",
                L"accept",
                errorValue,
                detailBuffer);
            if (resultSocket == INVALID_SOCKET)
            {
                ::WSASetLastError(errorValue);
            }
            return resultSocket;
        }


        HookBinding g_bindings[] = {
            { L"KernelBase.dll", "CreateFileA", ks::winapi_monitor::EventCategory::File, &g_createFileAHook, reinterpret_cast<void*>(&HookedCreateFileA), reinterpret_cast<void**>(&g_createFileAOriginal) },
            { L"KernelBase.dll", "CreateFileW", ks::winapi_monitor::EventCategory::File, &g_createFileWHook, reinterpret_cast<void*>(&HookedCreateFileW), reinterpret_cast<void**>(&g_createFileWOriginal) },
            { L"KernelBase.dll", "CreateFile2", ks::winapi_monitor::EventCategory::File, &g_createFile2Hook, reinterpret_cast<void*>(&HookedCreateFile2), reinterpret_cast<void**>(&g_createFile2Original) },
            { L"KernelBase.dll", "ReadFile", ks::winapi_monitor::EventCategory::File, &g_readFileHook, reinterpret_cast<void*>(&HookedReadFile), reinterpret_cast<void**>(&g_readFileOriginal) },
            { L"KernelBase.dll", "WriteFile", ks::winapi_monitor::EventCategory::File, &g_writeFileHook, reinterpret_cast<void*>(&HookedWriteFile), reinterpret_cast<void**>(&g_writeFileOriginal) },
            { L"KernelBase.dll", "DeviceIoControl", ks::winapi_monitor::EventCategory::File, &g_deviceIoControlHook, reinterpret_cast<void*>(&HookedDeviceIoControl), reinterpret_cast<void**>(&g_deviceIoControlOriginal) },
            { L"KernelBase.dll", "DeleteFileW", ks::winapi_monitor::EventCategory::File, &g_deleteFileWHook, reinterpret_cast<void*>(&HookedDeleteFileW), reinterpret_cast<void**>(&g_deleteFileWOriginal) },
            { L"KernelBase.dll", "DeleteFileA", ks::winapi_monitor::EventCategory::File, &g_deleteFileAHook, reinterpret_cast<void*>(&HookedDeleteFileA), reinterpret_cast<void**>(&g_deleteFileAOriginal) },
            { L"KernelBase.dll", "MoveFileExW", ks::winapi_monitor::EventCategory::File, &g_moveFileExWHook, reinterpret_cast<void*>(&HookedMoveFileExW), reinterpret_cast<void**>(&g_moveFileExWOriginal) },
            { L"KernelBase.dll", "MoveFileExA", ks::winapi_monitor::EventCategory::File, &g_moveFileExAHook, reinterpret_cast<void*>(&HookedMoveFileExA), reinterpret_cast<void**>(&g_moveFileExAOriginal) },
            { L"KernelBase.dll", "CopyFileW", ks::winapi_monitor::EventCategory::File, &g_copyFileWHook, reinterpret_cast<void*>(&HookedCopyFileW), reinterpret_cast<void**>(&g_copyFileWOriginal) },
            { L"KernelBase.dll", "CopyFileA", ks::winapi_monitor::EventCategory::File, &g_copyFileAHook, reinterpret_cast<void*>(&HookedCopyFileA), reinterpret_cast<void**>(&g_copyFileAOriginal) },
            { L"KernelBase.dll", "CopyFileExW", ks::winapi_monitor::EventCategory::File, &g_copyFileExWHook, reinterpret_cast<void*>(&HookedCopyFileExW), reinterpret_cast<void**>(&g_copyFileExWOriginal) },
            { L"KernelBase.dll", "CopyFileExA", ks::winapi_monitor::EventCategory::File, &g_copyFileExAHook, reinterpret_cast<void*>(&HookedCopyFileExA), reinterpret_cast<void**>(&g_copyFileExAOriginal) },
            { L"KernelBase.dll", "GetFileAttributesW", ks::winapi_monitor::EventCategory::File, &g_getFileAttributesWHook, reinterpret_cast<void*>(&HookedGetFileAttributesW), reinterpret_cast<void**>(&g_getFileAttributesWOriginal) },
            { L"KernelBase.dll", "GetFileAttributesA", ks::winapi_monitor::EventCategory::File, &g_getFileAttributesAHook, reinterpret_cast<void*>(&HookedGetFileAttributesA), reinterpret_cast<void**>(&g_getFileAttributesAOriginal) },
            { L"KernelBase.dll", "GetFileAttributesExW", ks::winapi_monitor::EventCategory::File, &g_getFileAttributesExWHook, reinterpret_cast<void*>(&HookedGetFileAttributesExW), reinterpret_cast<void**>(&g_getFileAttributesExWOriginal) },
            { L"KernelBase.dll", "GetFileAttributesExA", ks::winapi_monitor::EventCategory::File, &g_getFileAttributesExAHook, reinterpret_cast<void*>(&HookedGetFileAttributesExA), reinterpret_cast<void**>(&g_getFileAttributesExAOriginal) },
            { L"KernelBase.dll", "SetFileAttributesW", ks::winapi_monitor::EventCategory::File, &g_setFileAttributesWHook, reinterpret_cast<void*>(&HookedSetFileAttributesW), reinterpret_cast<void**>(&g_setFileAttributesWOriginal) },
            { L"KernelBase.dll", "SetFileAttributesA", ks::winapi_monitor::EventCategory::File, &g_setFileAttributesAHook, reinterpret_cast<void*>(&HookedSetFileAttributesA), reinterpret_cast<void**>(&g_setFileAttributesAOriginal) },
            { L"KernelBase.dll", "FindFirstFileExW", ks::winapi_monitor::EventCategory::File, &g_findFirstFileExWHook, reinterpret_cast<void*>(&HookedFindFirstFileExW), reinterpret_cast<void**>(&g_findFirstFileExWOriginal) },
            { L"KernelBase.dll", "FindFirstFileExA", ks::winapi_monitor::EventCategory::File, &g_findFirstFileExAHook, reinterpret_cast<void*>(&HookedFindFirstFileExA), reinterpret_cast<void**>(&g_findFirstFileExAOriginal) },
            { L"KernelBase.dll", "CreateDirectoryW", ks::winapi_monitor::EventCategory::File, &g_createDirectoryWHook, reinterpret_cast<void*>(&HookedCreateDirectoryW), reinterpret_cast<void**>(&g_createDirectoryWOriginal) },
            { L"KernelBase.dll", "CreateDirectoryA", ks::winapi_monitor::EventCategory::File, &g_createDirectoryAHook, reinterpret_cast<void*>(&HookedCreateDirectoryA), reinterpret_cast<void**>(&g_createDirectoryAOriginal) },
            { L"KernelBase.dll", "RemoveDirectoryW", ks::winapi_monitor::EventCategory::File, &g_removeDirectoryWHook, reinterpret_cast<void*>(&HookedRemoveDirectoryW), reinterpret_cast<void**>(&g_removeDirectoryWOriginal) },
            { L"KernelBase.dll", "RemoveDirectoryA", ks::winapi_monitor::EventCategory::File, &g_removeDirectoryAHook, reinterpret_cast<void*>(&HookedRemoveDirectoryA), reinterpret_cast<void**>(&g_removeDirectoryAOriginal) },
            { L"KernelBase.dll", "SetFileInformationByHandle", ks::winapi_monitor::EventCategory::File, &g_setFileInformationByHandleHook, reinterpret_cast<void*>(&HookedSetFileInformationByHandle), reinterpret_cast<void**>(&g_setFileInformationByHandleOriginal) },
            { L"KernelBase.dll", "CreateProcessA", ks::winapi_monitor::EventCategory::Process, &g_createProcessAHook, reinterpret_cast<void*>(&HookedCreateProcessA), reinterpret_cast<void**>(&g_createProcessAOriginal) },
            { L"KernelBase.dll", "CreateProcessW", ks::winapi_monitor::EventCategory::Process, &g_createProcessWHook, reinterpret_cast<void*>(&HookedCreateProcessW), reinterpret_cast<void**>(&g_createProcessWOriginal) },
            { L"KernelBase.dll", "OpenProcess", ks::winapi_monitor::EventCategory::Process, &g_openProcessHook, reinterpret_cast<void*>(&HookedOpenProcess), reinterpret_cast<void**>(&g_openProcessOriginal) },
            { L"KernelBase.dll", "OpenThread", ks::winapi_monitor::EventCategory::Process, &g_openThreadHook, reinterpret_cast<void*>(&HookedOpenThread), reinterpret_cast<void**>(&g_openThreadOriginal) },
            { L"KernelBase.dll", "TerminateProcess", ks::winapi_monitor::EventCategory::Process, &g_terminateProcessHook, reinterpret_cast<void*>(&HookedTerminateProcess), reinterpret_cast<void**>(&g_terminateProcessOriginal) },
            { L"KernelBase.dll", "CreateThread", ks::winapi_monitor::EventCategory::Process, &g_createThreadHook, reinterpret_cast<void*>(&HookedCreateThread), reinterpret_cast<void**>(&g_createThreadOriginal) },
            { L"KernelBase.dll", "CreateRemoteThread", ks::winapi_monitor::EventCategory::Process, &g_createRemoteThreadHook, reinterpret_cast<void*>(&HookedCreateRemoteThread), reinterpret_cast<void**>(&g_createRemoteThreadOriginal) },
            { L"KernelBase.dll", "VirtualAllocEx", ks::winapi_monitor::EventCategory::Process, &g_virtualAllocExHook, reinterpret_cast<void*>(&HookedVirtualAllocEx), reinterpret_cast<void**>(&g_virtualAllocExOriginal) },
            { L"KernelBase.dll", "VirtualFreeEx", ks::winapi_monitor::EventCategory::Process, &g_virtualFreeExHook, reinterpret_cast<void*>(&HookedVirtualFreeEx), reinterpret_cast<void**>(&g_virtualFreeExOriginal) },
            { L"KernelBase.dll", "VirtualProtectEx", ks::winapi_monitor::EventCategory::Process, &g_virtualProtectExHook, reinterpret_cast<void*>(&HookedVirtualProtectEx), reinterpret_cast<void**>(&g_virtualProtectExOriginal) },
            { L"KernelBase.dll", "WriteProcessMemory", ks::winapi_monitor::EventCategory::Process, &g_writeProcessMemoryHook, reinterpret_cast<void*>(&HookedWriteProcessMemory), reinterpret_cast<void**>(&g_writeProcessMemoryOriginal) },
            { L"KernelBase.dll", "ReadProcessMemory", ks::winapi_monitor::EventCategory::Process, &g_readProcessMemoryHook, reinterpret_cast<void*>(&HookedReadProcessMemory), reinterpret_cast<void**>(&g_readProcessMemoryOriginal) },
            { L"KernelBase.dll", "SuspendThread", ks::winapi_monitor::EventCategory::Process, &g_suspendThreadHook, reinterpret_cast<void*>(&HookedSuspendThread), reinterpret_cast<void**>(&g_suspendThreadOriginal) },
            { L"KernelBase.dll", "ResumeThread", ks::winapi_monitor::EventCategory::Process, &g_resumeThreadHook, reinterpret_cast<void*>(&HookedResumeThread), reinterpret_cast<void**>(&g_resumeThreadOriginal) },
            { L"KernelBase.dll", "QueueUserAPC", ks::winapi_monitor::EventCategory::Process, &g_queueUserAPCHook, reinterpret_cast<void*>(&HookedQueueUserAPC), reinterpret_cast<void**>(&g_queueUserAPCOriginal) },
            { L"KernelBase.dll", "GetThreadContext", ks::winapi_monitor::EventCategory::Process, &g_getThreadContextHook, reinterpret_cast<void*>(&HookedGetThreadContext), reinterpret_cast<void**>(&g_getThreadContextOriginal) },
            { L"KernelBase.dll", "SetThreadContext", ks::winapi_monitor::EventCategory::Process, &g_setThreadContextHook, reinterpret_cast<void*>(&HookedSetThreadContext), reinterpret_cast<void**>(&g_setThreadContextOriginal) },
            { L"Kernel32.dll", "WinExec", ks::winapi_monitor::EventCategory::Process, &g_winExecHook, reinterpret_cast<void*>(&HookedWinExec), reinterpret_cast<void**>(&g_winExecOriginal) },
            { L"Shell32.dll", "ShellExecuteExW", ks::winapi_monitor::EventCategory::Process, &g_shellExecuteExWHook, reinterpret_cast<void*>(&HookedShellExecuteExW), reinterpret_cast<void**>(&g_shellExecuteExWOriginal) },
            { L"Shell32.dll", "ShellExecuteExA", ks::winapi_monitor::EventCategory::Process, &g_shellExecuteExAHook, reinterpret_cast<void*>(&HookedShellExecuteExA), reinterpret_cast<void**>(&g_shellExecuteExAOriginal) },
            { L"Kernel32.dll", "LoadLibraryA", ks::winapi_monitor::EventCategory::Loader, &g_loadLibraryAHook, reinterpret_cast<void*>(&HookedLoadLibraryA), reinterpret_cast<void**>(&g_loadLibraryAOriginal) },
            { L"Kernel32.dll", "LoadLibraryW", ks::winapi_monitor::EventCategory::Loader, &g_loadLibraryWHook, reinterpret_cast<void*>(&HookedLoadLibraryW), reinterpret_cast<void**>(&g_loadLibraryWOriginal) },
            { L"Kernel32.dll", "LoadLibraryExA", ks::winapi_monitor::EventCategory::Loader, &g_loadLibraryExAHook, reinterpret_cast<void*>(&HookedLoadLibraryExA), reinterpret_cast<void**>(&g_loadLibraryExAOriginal) },
            { L"Kernel32.dll", "LoadLibraryExW", ks::winapi_monitor::EventCategory::Loader, &g_loadLibraryExWHook, reinterpret_cast<void*>(&HookedLoadLibraryExW), reinterpret_cast<void**>(&g_loadLibraryExWOriginal) },
            { L"ntdll.dll", "LdrLoadDll", ks::winapi_monitor::EventCategory::Loader, &g_ldrLoadDllHook, reinterpret_cast<void*>(&HookedLdrLoadDll), reinterpret_cast<void**>(&g_ldrLoadDllOriginal) },
            { L"Advapi32.dll", "RegOpenKeyW", ks::winapi_monitor::EventCategory::Registry, &g_regOpenKeyWHook, reinterpret_cast<void*>(&HookedRegOpenKeyW), reinterpret_cast<void**>(&g_regOpenKeyWOriginal) },
            { L"Advapi32.dll", "RegOpenKeyA", ks::winapi_monitor::EventCategory::Registry, &g_regOpenKeyAHook, reinterpret_cast<void*>(&HookedRegOpenKeyA), reinterpret_cast<void**>(&g_regOpenKeyAOriginal) },
            { L"Advapi32.dll", "RegOpenKeyExW", ks::winapi_monitor::EventCategory::Registry, &g_regOpenKeyExWHook, reinterpret_cast<void*>(&HookedRegOpenKeyExW), reinterpret_cast<void**>(&g_regOpenKeyExWOriginal) },
            { L"Advapi32.dll", "RegOpenKeyExA", ks::winapi_monitor::EventCategory::Registry, &g_regOpenKeyExAHook, reinterpret_cast<void*>(&HookedRegOpenKeyExA), reinterpret_cast<void**>(&g_regOpenKeyExAOriginal) },
            { L"Advapi32.dll", "RegCreateKeyW", ks::winapi_monitor::EventCategory::Registry, &g_regCreateKeyWHook, reinterpret_cast<void*>(&HookedRegCreateKeyW), reinterpret_cast<void**>(&g_regCreateKeyWOriginal) },
            { L"Advapi32.dll", "RegCreateKeyA", ks::winapi_monitor::EventCategory::Registry, &g_regCreateKeyAHook, reinterpret_cast<void*>(&HookedRegCreateKeyA), reinterpret_cast<void**>(&g_regCreateKeyAOriginal) },
            { L"Advapi32.dll", "RegCreateKeyExW", ks::winapi_monitor::EventCategory::Registry, &g_regCreateKeyExWHook, reinterpret_cast<void*>(&HookedRegCreateKeyExW), reinterpret_cast<void**>(&g_regCreateKeyExWOriginal) },
            { L"Advapi32.dll", "RegCreateKeyExA", ks::winapi_monitor::EventCategory::Registry, &g_regCreateKeyExAHook, reinterpret_cast<void*>(&HookedRegCreateKeyExA), reinterpret_cast<void**>(&g_regCreateKeyExAOriginal) },
            { L"Advapi32.dll", "RegQueryValueExW", ks::winapi_monitor::EventCategory::Registry, &g_regQueryValueExWHook, reinterpret_cast<void*>(&HookedRegQueryValueExW), reinterpret_cast<void**>(&g_regQueryValueExWOriginal) },
            { L"Advapi32.dll", "RegQueryValueExA", ks::winapi_monitor::EventCategory::Registry, &g_regQueryValueExAHook, reinterpret_cast<void*>(&HookedRegQueryValueExA), reinterpret_cast<void**>(&g_regQueryValueExAOriginal) },
            { L"Advapi32.dll", "RegGetValueW", ks::winapi_monitor::EventCategory::Registry, &g_regGetValueWHook, reinterpret_cast<void*>(&HookedRegGetValueW), reinterpret_cast<void**>(&g_regGetValueWOriginal) },
            { L"Advapi32.dll", "RegGetValueA", ks::winapi_monitor::EventCategory::Registry, &g_regGetValueAHook, reinterpret_cast<void*>(&HookedRegGetValueA), reinterpret_cast<void**>(&g_regGetValueAOriginal) },
            { L"Advapi32.dll", "RegSetValueExW", ks::winapi_monitor::EventCategory::Registry, &g_regSetValueExWHook, reinterpret_cast<void*>(&HookedRegSetValueExW), reinterpret_cast<void**>(&g_regSetValueExWOriginal) },
            { L"Advapi32.dll", "RegSetValueExA", ks::winapi_monitor::EventCategory::Registry, &g_regSetValueExAHook, reinterpret_cast<void*>(&HookedRegSetValueExA), reinterpret_cast<void**>(&g_regSetValueExAOriginal) },
            { L"Advapi32.dll", "RegSetKeyValueW", ks::winapi_monitor::EventCategory::Registry, &g_regSetKeyValueWHook, reinterpret_cast<void*>(&HookedRegSetKeyValueW), reinterpret_cast<void**>(&g_regSetKeyValueWOriginal) },
            { L"Advapi32.dll", "RegSetKeyValueA", ks::winapi_monitor::EventCategory::Registry, &g_regSetKeyValueAHook, reinterpret_cast<void*>(&HookedRegSetKeyValueA), reinterpret_cast<void**>(&g_regSetKeyValueAOriginal) },
            { L"Advapi32.dll", "RegDeleteValueW", ks::winapi_monitor::EventCategory::Registry, &g_regDeleteValueWHook, reinterpret_cast<void*>(&HookedRegDeleteValueW), reinterpret_cast<void**>(&g_regDeleteValueWOriginal) },
            { L"Advapi32.dll", "RegDeleteValueA", ks::winapi_monitor::EventCategory::Registry, &g_regDeleteValueAHook, reinterpret_cast<void*>(&HookedRegDeleteValueA), reinterpret_cast<void**>(&g_regDeleteValueAOriginal) },
            { L"Advapi32.dll", "RegDeleteKeyW", ks::winapi_monitor::EventCategory::Registry, &g_regDeleteKeyWHook, reinterpret_cast<void*>(&HookedRegDeleteKeyW), reinterpret_cast<void**>(&g_regDeleteKeyWOriginal) },
            { L"Advapi32.dll", "RegDeleteKeyA", ks::winapi_monitor::EventCategory::Registry, &g_regDeleteKeyAHook, reinterpret_cast<void*>(&HookedRegDeleteKeyA), reinterpret_cast<void**>(&g_regDeleteKeyAOriginal) },
            { L"Advapi32.dll", "RegDeleteKeyExW", ks::winapi_monitor::EventCategory::Registry, &g_regDeleteKeyExWHook, reinterpret_cast<void*>(&HookedRegDeleteKeyExW), reinterpret_cast<void**>(&g_regDeleteKeyExWOriginal) },
            { L"Advapi32.dll", "RegDeleteKeyExA", ks::winapi_monitor::EventCategory::Registry, &g_regDeleteKeyExAHook, reinterpret_cast<void*>(&HookedRegDeleteKeyExA), reinterpret_cast<void**>(&g_regDeleteKeyExAOriginal) },
            { L"Advapi32.dll", "RegDeleteTreeW", ks::winapi_monitor::EventCategory::Registry, &g_regDeleteTreeWHook, reinterpret_cast<void*>(&HookedRegDeleteTreeW), reinterpret_cast<void**>(&g_regDeleteTreeWOriginal) },
            { L"Advapi32.dll", "RegDeleteTreeA", ks::winapi_monitor::EventCategory::Registry, &g_regDeleteTreeAHook, reinterpret_cast<void*>(&HookedRegDeleteTreeA), reinterpret_cast<void**>(&g_regDeleteTreeAOriginal) },
            { L"Advapi32.dll", "RegCopyTreeW", ks::winapi_monitor::EventCategory::Registry, &g_regCopyTreeWHook, reinterpret_cast<void*>(&HookedRegCopyTreeW), reinterpret_cast<void**>(&g_regCopyTreeWOriginal) },
            { L"Advapi32.dll", "RegCopyTreeA", ks::winapi_monitor::EventCategory::Registry, &g_regCopyTreeAHook, reinterpret_cast<void*>(&HookedRegCopyTreeA), reinterpret_cast<void**>(&g_regCopyTreeAOriginal) },
            { L"Advapi32.dll", "RegLoadKeyW", ks::winapi_monitor::EventCategory::Registry, &g_regLoadKeyWHook, reinterpret_cast<void*>(&HookedRegLoadKeyW), reinterpret_cast<void**>(&g_regLoadKeyWOriginal) },
            { L"Advapi32.dll", "RegLoadKeyA", ks::winapi_monitor::EventCategory::Registry, &g_regLoadKeyAHook, reinterpret_cast<void*>(&HookedRegLoadKeyA), reinterpret_cast<void**>(&g_regLoadKeyAOriginal) },
            { L"Advapi32.dll", "RegSaveKeyW", ks::winapi_monitor::EventCategory::Registry, &g_regSaveKeyWHook, reinterpret_cast<void*>(&HookedRegSaveKeyW), reinterpret_cast<void**>(&g_regSaveKeyWOriginal) },
            { L"Advapi32.dll", "RegSaveKeyA", ks::winapi_monitor::EventCategory::Registry, &g_regSaveKeyAHook, reinterpret_cast<void*>(&HookedRegSaveKeyA), reinterpret_cast<void**>(&g_regSaveKeyAOriginal) },
            { L"Advapi32.dll", "RegRenameKey", ks::winapi_monitor::EventCategory::Registry, &g_regRenameKeyHook, reinterpret_cast<void*>(&HookedRegRenameKey), reinterpret_cast<void**>(&g_regRenameKeyOriginal) },
            { L"Advapi32.dll", "RegEnumKeyExW", ks::winapi_monitor::EventCategory::Registry, &g_regEnumKeyExWHook, reinterpret_cast<void*>(&HookedRegEnumKeyExW), reinterpret_cast<void**>(&g_regEnumKeyExWOriginal) },
            { L"Advapi32.dll", "RegEnumKeyExA", ks::winapi_monitor::EventCategory::Registry, &g_regEnumKeyExAHook, reinterpret_cast<void*>(&HookedRegEnumKeyExA), reinterpret_cast<void**>(&g_regEnumKeyExAOriginal) },
            { L"Advapi32.dll", "RegEnumValueW", ks::winapi_monitor::EventCategory::Registry, &g_regEnumValueWHook, reinterpret_cast<void*>(&HookedRegEnumValueW), reinterpret_cast<void**>(&g_regEnumValueWOriginal) },
            { L"Advapi32.dll", "RegEnumValueA", ks::winapi_monitor::EventCategory::Registry, &g_regEnumValueAHook, reinterpret_cast<void*>(&HookedRegEnumValueA), reinterpret_cast<void**>(&g_regEnumValueAOriginal) },
            { L"Advapi32.dll", "RegCloseKey", ks::winapi_monitor::EventCategory::Registry, &g_regCloseKeyHook, reinterpret_cast<void*>(&HookedRegCloseKey), reinterpret_cast<void**>(&g_regCloseKeyOriginal) },
            { L"Advapi32.dll", "RegQueryInfoKeyW", ks::winapi_monitor::EventCategory::Registry, &g_regQueryInfoKeyWHook, reinterpret_cast<void*>(&HookedRegQueryInfoKeyW), reinterpret_cast<void**>(&g_regQueryInfoKeyWOriginal) },
            { L"Advapi32.dll", "RegQueryInfoKeyA", ks::winapi_monitor::EventCategory::Registry, &g_regQueryInfoKeyAHook, reinterpret_cast<void*>(&HookedRegQueryInfoKeyA), reinterpret_cast<void**>(&g_regQueryInfoKeyAOriginal) },
            { L"Advapi32.dll", "RegFlushKey", ks::winapi_monitor::EventCategory::Registry, &g_regFlushKeyHook, reinterpret_cast<void*>(&HookedRegFlushKey), reinterpret_cast<void**>(&g_regFlushKeyOriginal) },
            { L"Advapi32.dll", "RegDeleteKeyValueW", ks::winapi_monitor::EventCategory::Registry, &g_regDeleteKeyValueWHook, reinterpret_cast<void*>(&HookedRegDeleteKeyValueW), reinterpret_cast<void**>(&g_regDeleteKeyValueWOriginal) },
            { L"Advapi32.dll", "RegDeleteKeyValueA", ks::winapi_monitor::EventCategory::Registry, &g_regDeleteKeyValueAHook, reinterpret_cast<void*>(&HookedRegDeleteKeyValueA), reinterpret_cast<void**>(&g_regDeleteKeyValueAOriginal) },
            { L"Advapi32.dll", "RegConnectRegistryW", ks::winapi_monitor::EventCategory::Registry, &g_regConnectRegistryWHook, reinterpret_cast<void*>(&HookedRegConnectRegistryW), reinterpret_cast<void**>(&g_regConnectRegistryWOriginal) },
            { L"Advapi32.dll", "RegConnectRegistryA", ks::winapi_monitor::EventCategory::Registry, &g_regConnectRegistryAHook, reinterpret_cast<void*>(&HookedRegConnectRegistryA), reinterpret_cast<void**>(&g_regConnectRegistryAOriginal) },
            { L"ntdll.dll", "NtCreateKey", ks::winapi_monitor::EventCategory::Registry, &g_ntCreateKeyHook, reinterpret_cast<void*>(&HookedNtCreateKey), reinterpret_cast<void**>(&g_ntCreateKeyOriginal) },
            { L"ntdll.dll", "NtOpenKey", ks::winapi_monitor::EventCategory::Registry, &g_ntOpenKeyHook, reinterpret_cast<void*>(&HookedNtOpenKey), reinterpret_cast<void**>(&g_ntOpenKeyOriginal) },
            { L"ntdll.dll", "NtOpenKeyEx", ks::winapi_monitor::EventCategory::Registry, &g_ntOpenKeyExHook, reinterpret_cast<void*>(&HookedNtOpenKeyEx), reinterpret_cast<void**>(&g_ntOpenKeyExOriginal) },
            { L"ntdll.dll", "NtSetValueKey", ks::winapi_monitor::EventCategory::Registry, &g_ntSetValueKeyHook, reinterpret_cast<void*>(&HookedNtSetValueKey), reinterpret_cast<void**>(&g_ntSetValueKeyOriginal) },
            { L"ntdll.dll", "NtQueryValueKey", ks::winapi_monitor::EventCategory::Registry, &g_ntQueryValueKeyHook, reinterpret_cast<void*>(&HookedNtQueryValueKey), reinterpret_cast<void**>(&g_ntQueryValueKeyOriginal) },
            { L"ntdll.dll", "NtEnumerateKey", ks::winapi_monitor::EventCategory::Registry, &g_ntEnumerateKeyHook, reinterpret_cast<void*>(&HookedNtEnumerateKey), reinterpret_cast<void**>(&g_ntEnumerateKeyOriginal) },
            { L"ntdll.dll", "NtEnumerateValueKey", ks::winapi_monitor::EventCategory::Registry, &g_ntEnumerateValueKeyHook, reinterpret_cast<void*>(&HookedNtEnumerateValueKey), reinterpret_cast<void**>(&g_ntEnumerateValueKeyOriginal) },
            { L"ntdll.dll", "NtDeleteKey", ks::winapi_monitor::EventCategory::Registry, &g_ntDeleteKeyHook, reinterpret_cast<void*>(&HookedNtDeleteKey), reinterpret_cast<void**>(&g_ntDeleteKeyOriginal) },
            { L"ntdll.dll", "NtDeleteValueKey", ks::winapi_monitor::EventCategory::Registry, &g_ntDeleteValueKeyHook, reinterpret_cast<void*>(&HookedNtDeleteValueKey), reinterpret_cast<void**>(&g_ntDeleteValueKeyOriginal) },
            { L"ntdll.dll", "NtFlushKey", ks::winapi_monitor::EventCategory::Registry, &g_ntFlushKeyHook, reinterpret_cast<void*>(&HookedNtFlushKey), reinterpret_cast<void**>(&g_ntFlushKeyOriginal) },
            { L"ntdll.dll", "NtRenameKey", ks::winapi_monitor::EventCategory::Registry, &g_ntRenameKeyHook, reinterpret_cast<void*>(&HookedNtRenameKey), reinterpret_cast<void**>(&g_ntRenameKeyOriginal) },
            { L"ntdll.dll", "NtLoadKey", ks::winapi_monitor::EventCategory::Registry, &g_ntLoadKeyHook, reinterpret_cast<void*>(&HookedNtLoadKey), reinterpret_cast<void**>(&g_ntLoadKeyOriginal) },
            { L"ntdll.dll", "NtSaveKey", ks::winapi_monitor::EventCategory::Registry, &g_ntSaveKeyHook, reinterpret_cast<void*>(&HookedNtSaveKey), reinterpret_cast<void**>(&g_ntSaveKeyOriginal) },
            { L"ntdll.dll", "NtQueryKey", ks::winapi_monitor::EventCategory::Registry, &g_ntQueryKeyHook, reinterpret_cast<void*>(&HookedNtQueryKey), reinterpret_cast<void**>(&g_ntQueryKeyOriginal) },
            { L"ntdll.dll", "NtQueryMultipleValueKey", ks::winapi_monitor::EventCategory::Registry, &g_ntQueryMultipleValueKeyHook, reinterpret_cast<void*>(&HookedNtQueryMultipleValueKey), reinterpret_cast<void**>(&g_ntQueryMultipleValueKeyOriginal) },
            { L"ntdll.dll", "NtNotifyChangeKey", ks::winapi_monitor::EventCategory::Registry, &g_ntNotifyChangeKeyHook, reinterpret_cast<void*>(&HookedNtNotifyChangeKey), reinterpret_cast<void**>(&g_ntNotifyChangeKeyOriginal) },
            { L"ntdll.dll", "NtLoadKey2", ks::winapi_monitor::EventCategory::Registry, &g_ntLoadKey2Hook, reinterpret_cast<void*>(&HookedNtLoadKey2), reinterpret_cast<void**>(&g_ntLoadKey2Original) },
            { L"ntdll.dll", "NtSaveKeyEx", ks::winapi_monitor::EventCategory::Registry, &g_ntSaveKeyExHook, reinterpret_cast<void*>(&HookedNtSaveKeyEx), reinterpret_cast<void**>(&g_ntSaveKeyExOriginal) },
            { L"ntdll.dll", "NtLoadDriver", ks::winapi_monitor::EventCategory::Registry, &g_ntLoadDriverHook, reinterpret_cast<void*>(&HookedNtLoadDriver), reinterpret_cast<void**>(&g_ntLoadDriverOriginal) },
            { L"ntdll.dll", "NtUnloadDriver", ks::winapi_monitor::EventCategory::Registry, &g_ntUnloadDriverHook, reinterpret_cast<void*>(&HookedNtUnloadDriver), reinterpret_cast<void**>(&g_ntUnloadDriverOriginal) },
            { L"ntdll.dll", "NtCreateFile", ks::winapi_monitor::EventCategory::File, &g_ntCreateFileHook, reinterpret_cast<void*>(&HookedNtCreateFile), reinterpret_cast<void**>(&g_ntCreateFileOriginal) },
            { L"ntdll.dll", "NtOpenFile", ks::winapi_monitor::EventCategory::File, &g_ntOpenFileHook, reinterpret_cast<void*>(&HookedNtOpenFile), reinterpret_cast<void**>(&g_ntOpenFileOriginal) },
            { L"ntdll.dll", "NtReadFile", ks::winapi_monitor::EventCategory::File, &g_ntReadFileHook, reinterpret_cast<void*>(&HookedNtReadFile), reinterpret_cast<void**>(&g_ntReadFileOriginal) },
            { L"ntdll.dll", "NtWriteFile", ks::winapi_monitor::EventCategory::File, &g_ntWriteFileHook, reinterpret_cast<void*>(&HookedNtWriteFile), reinterpret_cast<void**>(&g_ntWriteFileOriginal) },
            { L"ntdll.dll", "NtSetInformationFile", ks::winapi_monitor::EventCategory::File, &g_ntSetInformationFileHook, reinterpret_cast<void*>(&HookedNtSetInformationFile), reinterpret_cast<void**>(&g_ntSetInformationFileOriginal) },
            { L"ntdll.dll", "NtQueryInformationFile", ks::winapi_monitor::EventCategory::File, &g_ntQueryInformationFileHook, reinterpret_cast<void*>(&HookedNtQueryInformationFile), reinterpret_cast<void**>(&g_ntQueryInformationFileOriginal) },
            { L"ntdll.dll", "NtDeleteFile", ks::winapi_monitor::EventCategory::File, &g_ntDeleteFileHook, reinterpret_cast<void*>(&HookedNtDeleteFile), reinterpret_cast<void**>(&g_ntDeleteFileOriginal) },
            { L"ntdll.dll", "NtQueryAttributesFile", ks::winapi_monitor::EventCategory::File, &g_ntQueryAttributesFileHook, reinterpret_cast<void*>(&HookedNtQueryAttributesFile), reinterpret_cast<void**>(&g_ntQueryAttributesFileOriginal) },
            { L"ntdll.dll", "NtQueryFullAttributesFile", ks::winapi_monitor::EventCategory::File, &g_ntQueryFullAttributesFileHook, reinterpret_cast<void*>(&HookedNtQueryFullAttributesFile), reinterpret_cast<void**>(&g_ntQueryFullAttributesFileOriginal) },
            { L"ntdll.dll", "NtDeviceIoControlFile", ks::winapi_monitor::EventCategory::File, &g_ntDeviceIoControlFileHook, reinterpret_cast<void*>(&HookedNtDeviceIoControlFile), reinterpret_cast<void**>(&g_ntDeviceIoControlFileOriginal) },
            { L"ntdll.dll", "NtFsControlFile", ks::winapi_monitor::EventCategory::File, &g_ntFsControlFileHook, reinterpret_cast<void*>(&HookedNtFsControlFile), reinterpret_cast<void**>(&g_ntFsControlFileOriginal) },
            { L"ntdll.dll", "NtQueryDirectoryFile", ks::winapi_monitor::EventCategory::File, &g_ntQueryDirectoryFileHook, reinterpret_cast<void*>(&HookedNtQueryDirectoryFile), reinterpret_cast<void**>(&g_ntQueryDirectoryFileOriginal) },
            { L"ntdll.dll", "NtQueryDirectoryFileEx", ks::winapi_monitor::EventCategory::File, &g_ntQueryDirectoryFileExHook, reinterpret_cast<void*>(&HookedNtQueryDirectoryFileEx), reinterpret_cast<void**>(&g_ntQueryDirectoryFileExOriginal) },
            { L"ntdll.dll", "NtOpenProcess", ks::winapi_monitor::EventCategory::Process, &g_ntOpenProcessHook, reinterpret_cast<void*>(&HookedNtOpenProcess), reinterpret_cast<void**>(&g_ntOpenProcessOriginal) },
            { L"ntdll.dll", "NtOpenThread", ks::winapi_monitor::EventCategory::Process, &g_ntOpenThreadHook, reinterpret_cast<void*>(&HookedNtOpenThread), reinterpret_cast<void**>(&g_ntOpenThreadOriginal) },
            { L"ntdll.dll", "NtTerminateProcess", ks::winapi_monitor::EventCategory::Process, &g_ntTerminateProcessHook, reinterpret_cast<void*>(&HookedNtTerminateProcess), reinterpret_cast<void**>(&g_ntTerminateProcessOriginal) },
            { L"ntdll.dll", "NtCreateUserProcess", ks::winapi_monitor::EventCategory::Process, &g_ntCreateUserProcessHook, reinterpret_cast<void*>(&HookedNtCreateUserProcess), reinterpret_cast<void**>(&g_ntCreateUserProcessOriginal) },
            { L"ntdll.dll", "NtCreateProcessEx", ks::winapi_monitor::EventCategory::Process, &g_ntCreateProcessExHook, reinterpret_cast<void*>(&HookedNtCreateProcessEx), reinterpret_cast<void**>(&g_ntCreateProcessExOriginal) },
            { L"ntdll.dll", "NtCreateThreadEx", ks::winapi_monitor::EventCategory::Process, &g_ntCreateThreadExHook, reinterpret_cast<void*>(&HookedNtCreateThreadEx), reinterpret_cast<void**>(&g_ntCreateThreadExOriginal) },
            { L"ntdll.dll", "NtAllocateVirtualMemory", ks::winapi_monitor::EventCategory::Process, &g_ntAllocateVirtualMemoryHook, reinterpret_cast<void*>(&HookedNtAllocateVirtualMemory), reinterpret_cast<void**>(&g_ntAllocateVirtualMemoryOriginal) },
            { L"ntdll.dll", "NtFreeVirtualMemory", ks::winapi_monitor::EventCategory::Process, &g_ntFreeVirtualMemoryHook, reinterpret_cast<void*>(&HookedNtFreeVirtualMemory), reinterpret_cast<void**>(&g_ntFreeVirtualMemoryOriginal) },
            { L"ntdll.dll", "NtProtectVirtualMemory", ks::winapi_monitor::EventCategory::Process, &g_ntProtectVirtualMemoryHook, reinterpret_cast<void*>(&HookedNtProtectVirtualMemory), reinterpret_cast<void**>(&g_ntProtectVirtualMemoryOriginal) },
            { L"ntdll.dll", "NtWriteVirtualMemory", ks::winapi_monitor::EventCategory::Process, &g_ntWriteVirtualMemoryHook, reinterpret_cast<void*>(&HookedNtWriteVirtualMemory), reinterpret_cast<void**>(&g_ntWriteVirtualMemoryOriginal) },
            { L"ntdll.dll", "NtReadVirtualMemory", ks::winapi_monitor::EventCategory::Process, &g_ntReadVirtualMemoryHook, reinterpret_cast<void*>(&HookedNtReadVirtualMemory), reinterpret_cast<void**>(&g_ntReadVirtualMemoryOriginal) },
            { L"ntdll.dll", "NtMapViewOfSection", ks::winapi_monitor::EventCategory::Process, &g_ntMapViewOfSectionHook, reinterpret_cast<void*>(&HookedNtMapViewOfSection), reinterpret_cast<void**>(&g_ntMapViewOfSectionOriginal) },
            { L"ntdll.dll", "NtUnmapViewOfSection", ks::winapi_monitor::EventCategory::Process, &g_ntUnmapViewOfSectionHook, reinterpret_cast<void*>(&HookedNtUnmapViewOfSection), reinterpret_cast<void**>(&g_ntUnmapViewOfSectionOriginal) },
            { L"ntdll.dll", "NtDuplicateObject", ks::winapi_monitor::EventCategory::Process, &g_ntDuplicateObjectHook, reinterpret_cast<void*>(&HookedNtDuplicateObject), reinterpret_cast<void**>(&g_ntDuplicateObjectOriginal) },
            { L"ntdll.dll", "NtQueryInformationProcess", ks::winapi_monitor::EventCategory::Process, &g_ntQueryInformationProcessHook, reinterpret_cast<void*>(&HookedNtQueryInformationProcess), reinterpret_cast<void**>(&g_ntQueryInformationProcessOriginal) },
            { L"ntdll.dll", "NtSetInformationProcess", ks::winapi_monitor::EventCategory::Process, &g_ntSetInformationProcessHook, reinterpret_cast<void*>(&HookedNtSetInformationProcess), reinterpret_cast<void**>(&g_ntSetInformationProcessOriginal) },
            { L"ntdll.dll", "NtQueryVirtualMemory", ks::winapi_monitor::EventCategory::Process, &g_ntQueryVirtualMemoryHook, reinterpret_cast<void*>(&HookedNtQueryVirtualMemory), reinterpret_cast<void**>(&g_ntQueryVirtualMemoryOriginal) },
            { L"ntdll.dll", "NtCreateSection", ks::winapi_monitor::EventCategory::Process, &g_ntCreateSectionHook, reinterpret_cast<void*>(&HookedNtCreateSection), reinterpret_cast<void**>(&g_ntCreateSectionOriginal) },
            { L"ntdll.dll", "NtOpenSection", ks::winapi_monitor::EventCategory::Process, &g_ntOpenSectionHook, reinterpret_cast<void*>(&HookedNtOpenSection), reinterpret_cast<void**>(&g_ntOpenSectionOriginal) },
            { L"ntdll.dll", "NtQueueApcThread", ks::winapi_monitor::EventCategory::Process, &g_ntQueueApcThreadHook, reinterpret_cast<void*>(&HookedNtQueueApcThread), reinterpret_cast<void**>(&g_ntQueueApcThreadOriginal) },
            { L"ntdll.dll", "NtQueueApcThreadEx", ks::winapi_monitor::EventCategory::Process, &g_ntQueueApcThreadExHook, reinterpret_cast<void*>(&HookedNtQueueApcThreadEx), reinterpret_cast<void**>(&g_ntQueueApcThreadExOriginal) },
            { L"ntdll.dll", "NtSuspendThread", ks::winapi_monitor::EventCategory::Process, &g_ntSuspendThreadHook, reinterpret_cast<void*>(&HookedNtSuspendThread), reinterpret_cast<void**>(&g_ntSuspendThreadOriginal) },
            { L"ntdll.dll", "NtResumeThread", ks::winapi_monitor::EventCategory::Process, &g_ntResumeThreadHook, reinterpret_cast<void*>(&HookedNtResumeThread), reinterpret_cast<void**>(&g_ntResumeThreadOriginal) },
            { L"ntdll.dll", "NtGetContextThread", ks::winapi_monitor::EventCategory::Process, &g_ntGetContextThreadHook, reinterpret_cast<void*>(&HookedNtGetContextThread), reinterpret_cast<void**>(&g_ntGetContextThreadOriginal) },
            { L"ntdll.dll", "NtSetContextThread", ks::winapi_monitor::EventCategory::Process, &g_ntSetContextThreadHook, reinterpret_cast<void*>(&HookedNtSetContextThread), reinterpret_cast<void**>(&g_ntSetContextThreadOriginal) },
            { L"Ws2_32.dll", "socket", ks::winapi_monitor::EventCategory::Network, &g_socketHook, reinterpret_cast<void*>(&HookedSocket), reinterpret_cast<void**>(&g_socketOriginal) },
            { L"Ws2_32.dll", "WSASocketW", ks::winapi_monitor::EventCategory::Network, &g_wsaSocketWHook, reinterpret_cast<void*>(&HookedWSASocketW), reinterpret_cast<void**>(&g_wsaSocketWOriginal) },
            { L"Ws2_32.dll", "WSASocketA", ks::winapi_monitor::EventCategory::Network, &g_wsaSocketAHook, reinterpret_cast<void*>(&HookedWSASocketA), reinterpret_cast<void**>(&g_wsaSocketAOriginal) },
            { L"Ws2_32.dll", "closesocket", ks::winapi_monitor::EventCategory::Network, &g_closeSocketHook, reinterpret_cast<void*>(&HookedCloseSocket), reinterpret_cast<void**>(&g_closeSocketOriginal) },
            { L"Ws2_32.dll", "shutdown", ks::winapi_monitor::EventCategory::Network, &g_shutdownHook, reinterpret_cast<void*>(&HookedShutdown), reinterpret_cast<void**>(&g_shutdownOriginal) },
            { L"Ws2_32.dll", "connect", ks::winapi_monitor::EventCategory::Network, &g_connectHook, reinterpret_cast<void*>(&HookedConnect), reinterpret_cast<void**>(&g_connectOriginal) },
            { L"Ws2_32.dll", "WSAConnect", ks::winapi_monitor::EventCategory::Network, &g_wsaConnectHook, reinterpret_cast<void*>(&HookedWSAConnect), reinterpret_cast<void**>(&g_wsaConnectOriginal) },
            { L"Ws2_32.dll", "send", ks::winapi_monitor::EventCategory::Network, &g_sendHook, reinterpret_cast<void*>(&HookedSend), reinterpret_cast<void**>(&g_sendOriginal) },
            { L"Ws2_32.dll", "WSASend", ks::winapi_monitor::EventCategory::Network, &g_wsaSendHook, reinterpret_cast<void*>(&HookedWSASend), reinterpret_cast<void**>(&g_wsaSendOriginal) },
            { L"Ws2_32.dll", "sendto", ks::winapi_monitor::EventCategory::Network, &g_sendToHook, reinterpret_cast<void*>(&HookedSendTo), reinterpret_cast<void**>(&g_sendToOriginal) },
            { L"Ws2_32.dll", "recv", ks::winapi_monitor::EventCategory::Network, &g_recvHook, reinterpret_cast<void*>(&HookedRecv), reinterpret_cast<void**>(&g_recvOriginal) },
            { L"Ws2_32.dll", "WSARecv", ks::winapi_monitor::EventCategory::Network, &g_wsaRecvHook, reinterpret_cast<void*>(&HookedWSARecv), reinterpret_cast<void**>(&g_wsaRecvOriginal) },
            { L"Ws2_32.dll", "recvfrom", ks::winapi_monitor::EventCategory::Network, &g_recvFromHook, reinterpret_cast<void*>(&HookedRecvFrom), reinterpret_cast<void**>(&g_recvFromOriginal) },
            { L"Ws2_32.dll", "bind", ks::winapi_monitor::EventCategory::Network, &g_bindHook, reinterpret_cast<void*>(&HookedBind), reinterpret_cast<void**>(&g_bindOriginal) },
            { L"Ws2_32.dll", "listen", ks::winapi_monitor::EventCategory::Network, &g_listenHook, reinterpret_cast<void*>(&HookedListen), reinterpret_cast<void**>(&g_listenOriginal) },
            { L"Ws2_32.dll", "accept", ks::winapi_monitor::EventCategory::Network, &g_acceptHook, reinterpret_cast<void*>(&HookedAccept), reinterpret_cast<void**>(&g_acceptOriginal) }
        };


        // RetryPendingHooksUnlocked 作用：
        // - 输入：无，使用全局绑定表；
        // - 处理：在调用者已经持有 g_hookOperationMutex 时补装尚未安装的可重试 Hook；
        // - 返回：无返回值，失败细节在后续 InstallConfiguredHooks 诊断中体现。
        void RetryPendingHooksUnlocked()
        {
            for (HookBinding& bindingValue : g_bindings)
            {
                (void)TryInstallBinding(bindingValue, nullptr);
            }
        }

        // RetryPendingHooksFromHook 作用：
        // - 输入：无；
        // - 处理：从 LoadLibrary/LdrLoadDll detour 返回后尝试获取 hook 锁；
        // - 返回：无返回值，锁正忙时跳过本轮补装。
        void RetryPendingHooksFromHook()
        {
            if (g_hookOperationMutex.try_lock())
            {
                RetryPendingHooksUnlocked();
                g_hookOperationMutex.unlock();
            }
        }

    }

    bool InstallConfiguredHooks(std::wstring* errorTextOut)
    {
        const std::lock_guard<std::mutex> lock(g_hookOperationMutex);
        if (errorTextOut != nullptr)
        {
            errorTextOut->clear();
        }

        bool hasEnabledCategory = false;
        bool installedAny = false;
        std::wstring failureText;
        for (HookBinding& bindingValue : g_bindings)
        {
            hasEnabledCategory = CategoryEnabled(bindingValue.categoryValue) || hasEnabledCategory;
            installedAny = TryInstallBinding(bindingValue, &failureText) || installedAny;
        }
        if (!hasEnabledCategory)
        {
            if (errorTextOut != nullptr)
            {
                *errorTextOut = L"No hook category is enabled in current config.";
            }
            return false;
        }
        if (!installedAny && errorTextOut != nullptr)
        {
            *errorTextOut = failureText.empty()
                ? std::wstring(L"No hook installed successfully.")
                : failureText;
            return false;
        }
        if (installedAny && errorTextOut != nullptr)
        {
            *errorTextOut = failureText;
        }
        return installedAny;
    }

    void UninstallConfiguredHooks()
    {
        const std::lock_guard<std::mutex> lock(g_hookOperationMutex);
        for (HookBinding& bindingValue : g_bindings)
        {
            UninstallInlineHook(bindingValue.hookRecord);
            if (bindingValue.originalOut != nullptr)
            {
                *bindingValue.originalOut = nullptr;
            }
        }
    }

    void RetryPendingHooks()
    {
        const std::lock_guard<std::mutex> lock(g_hookOperationMutex);
        RetryPendingHooksUnlocked();
    }
}

