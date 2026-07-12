#include "pch.h"
#include "HookTargets.h"
#include "HookEngine.h"
#include "../MonitorAgent.h"
#include "../core/MonitorPipe.h"

#include <WinReg.h>
#include <bcrypt.h>
#include <evntrace.h>
#include <evntprov.h>
#include <ncrypt.h>
#include <objbase.h>
#include <psapi.h>
#include <rpc.h>
#include <shellapi.h>
#include <tlhelp32.h>
#include <urlmon.h>
#include <wincrypt.h>
#include <windns.h>
#include <winhttp.h>
#include <winioctl.h>
#include <wininet.h>
#include <winsvc.h>
#include <winternl.h>
#include <wintrust.h>

#ifndef NT_SUCCESS
#define NT_SUCCESS(Status) (((NTSTATUS)(Status)) >= 0)
#endif

#ifndef FSCTL_REQUEST_OPLOCK
#define FSCTL_REQUEST_OPLOCK CTL_CODE(FILE_DEVICE_FILE_SYSTEM, 144, METHOD_BUFFERED, FILE_ANY_ACCESS)
#endif
#ifndef FSCTL_REQUEST_FILTER_OPLOCK
#define FSCTL_REQUEST_FILTER_OPLOCK CTL_CODE(FILE_DEVICE_FILE_SYSTEM, 23, METHOD_BUFFERED, FILE_ANY_ACCESS)
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
        using CloseHandleFn = BOOL(WINAPI*)(HANDLE);
        using DuplicateHandleFn = BOOL(WINAPI*)(HANDLE, HANDLE, HANDLE, LPHANDLE, DWORD, BOOL, DWORD);
        using CreateFileMappingWFn = HANDLE(WINAPI*)(HANDLE, LPSECURITY_ATTRIBUTES, DWORD, DWORD, DWORD, LPCWSTR);
        using CreateFileMappingAFn = HANDLE(WINAPI*)(HANDLE, LPSECURITY_ATTRIBUTES, DWORD, DWORD, DWORD, LPCSTR);
        using OpenFileMappingWFn = HANDLE(WINAPI*)(DWORD, BOOL, LPCWSTR);
        using OpenFileMappingAFn = HANDLE(WINAPI*)(DWORD, BOOL, LPCSTR);
        using MapViewOfFileFn = LPVOID(WINAPI*)(HANDLE, DWORD, DWORD, DWORD, SIZE_T);
        using MapViewOfFileExFn = LPVOID(WINAPI*)(HANDLE, DWORD, DWORD, DWORD, SIZE_T, LPVOID);
        using UnmapViewOfFileFn = BOOL(WINAPI*)(LPCVOID);
        using FlushViewOfFileFn = BOOL(WINAPI*)(LPCVOID, SIZE_T);
        using FreeLibraryFn = BOOL(WINAPI*)(HMODULE);
        using GetProcAddressFn = FARPROC(WINAPI*)(HMODULE, LPCSTR);
        using LdrGetProcedureAddressFn = NTSTATUS(NTAPI*)(HMODULE, PANSI_STRING, WORD, PVOID*);
        using RegCreateKeyTransactedWFn = LSTATUS(WINAPI*)(HKEY, LPCWSTR, DWORD, LPWSTR, DWORD, REGSAM, const LPSECURITY_ATTRIBUTES, PHKEY, LPDWORD, HANDLE, PVOID);
        using RegCreateKeyTransactedAFn = LSTATUS(WINAPI*)(HKEY, LPCSTR, DWORD, LPSTR, DWORD, REGSAM, const LPSECURITY_ATTRIBUTES, PHKEY, LPDWORD, HANDLE, PVOID);
        using RegOpenKeyTransactedWFn = LSTATUS(WINAPI*)(HKEY, LPCWSTR, DWORD, REGSAM, PHKEY, HANDLE, PVOID);
        using RegOpenKeyTransactedAFn = LSTATUS(WINAPI*)(HKEY, LPCSTR, DWORD, REGSAM, PHKEY, HANDLE, PVOID);
        using RegDeleteKeyTransactedWFn = LSTATUS(WINAPI*)(HKEY, LPCWSTR, REGSAM, DWORD, HANDLE, PVOID);
        using RegDeleteKeyTransactedAFn = LSTATUS(WINAPI*)(HKEY, LPCSTR, REGSAM, DWORD, HANDLE, PVOID);
        using RegReplaceKeyWFn = LSTATUS(WINAPI*)(HKEY, LPCWSTR, LPCWSTR, LPCWSTR);
        using RegReplaceKeyAFn = LSTATUS(WINAPI*)(HKEY, LPCSTR, LPCSTR, LPCSTR);
        using RegRestoreKeyWFn = LSTATUS(WINAPI*)(HKEY, LPCWSTR, DWORD);
        using RegRestoreKeyAFn = LSTATUS(WINAPI*)(HKEY, LPCSTR, DWORD);
        using RegUnLoadKeyWFn = LSTATUS(WINAPI*)(HKEY, LPCWSTR);
        using RegUnLoadKeyAFn = LSTATUS(WINAPI*)(HKEY, LPCSTR);
        using RegLoadAppKeyWFn = LSTATUS(WINAPI*)(LPCWSTR, PHKEY, REGSAM, DWORD, DWORD);
        using RegLoadAppKeyAFn = LSTATUS(WINAPI*)(LPCSTR, PHKEY, REGSAM, DWORD, DWORD);
        using RegNotifyChangeKeyValueFn = LSTATUS(WINAPI*)(HKEY, BOOL, DWORD, HANDLE, BOOL);
        using NtCloseFn = NTSTATUS(NTAPI*)(HANDLE);
        using NtCreateKeyTransactedFn = NTSTATUS(NTAPI*)(PHANDLE, ACCESS_MASK, POBJECT_ATTRIBUTES, ULONG, PUNICODE_STRING, ULONG, HANDLE, PULONG);
        using NtOpenKeyTransactedFn = NTSTATUS(NTAPI*)(PHANDLE, ACCESS_MASK, POBJECT_ATTRIBUTES, HANDLE);
        using NtOpenKeyTransactedExFn = NTSTATUS(NTAPI*)(PHANDLE, ACCESS_MASK, POBJECT_ATTRIBUTES, ULONG, HANDLE);
        using NtReplaceKeyFn = NTSTATUS(NTAPI*)(POBJECT_ATTRIBUTES, HANDLE, POBJECT_ATTRIBUTES);
        using NtRestoreKeyFn = NTSTATUS(NTAPI*)(HANDLE, HANDLE, ULONG);
        using NtUnloadKeyFn = NTSTATUS(NTAPI*)(POBJECT_ATTRIBUTES);
        using NtUnloadKey2Fn = NTSTATUS(NTAPI*)(POBJECT_ATTRIBUTES, ULONG);
        using NtUnloadKeyExFn = NTSTATUS(NTAPI*)(POBJECT_ATTRIBUTES, HANDLE);
        using OpenProcessTokenFn = BOOL(WINAPI*)(HANDLE, DWORD, PHANDLE);
        using OpenThreadTokenFn = BOOL(WINAPI*)(HANDLE, DWORD, BOOL, PHANDLE);
        using AdjustTokenPrivilegesFn = BOOL(WINAPI*)(HANDLE, BOOL, PTOKEN_PRIVILEGES, DWORD, PTOKEN_PRIVILEGES, PDWORD);
        using DuplicateTokenFn = BOOL(WINAPI*)(HANDLE, SECURITY_IMPERSONATION_LEVEL, PHANDLE);
        using DuplicateTokenExFn = BOOL(WINAPI*)(HANDLE, DWORD, LPSECURITY_ATTRIBUTES, SECURITY_IMPERSONATION_LEVEL, TOKEN_TYPE, PHANDLE);
        using CreateProcessAsUserWFn = BOOL(WINAPI*)(HANDLE, LPCWSTR, LPWSTR, LPSECURITY_ATTRIBUTES, LPSECURITY_ATTRIBUTES, BOOL, DWORD, LPVOID, LPCWSTR, LPSTARTUPINFOW, LPPROCESS_INFORMATION);
        using CreateProcessAsUserAFn = BOOL(WINAPI*)(HANDLE, LPCSTR, LPSTR, LPSECURITY_ATTRIBUTES, LPSECURITY_ATTRIBUTES, BOOL, DWORD, LPVOID, LPCSTR, LPSTARTUPINFOA, LPPROCESS_INFORMATION);
        using CreateProcessWithTokenWFn = BOOL(WINAPI*)(HANDLE, DWORD, LPCWSTR, LPWSTR, DWORD, LPVOID, LPCWSTR, LPSTARTUPINFOW, LPPROCESS_INFORMATION);
        using LookupPrivilegeValueWFn = BOOL(WINAPI*)(LPCWSTR, LPCWSTR, PLUID);
        using LookupPrivilegeValueAFn = BOOL(WINAPI*)(LPCSTR, LPCSTR, PLUID);
        using OpenSCManagerWFn = SC_HANDLE(WINAPI*)(LPCWSTR, LPCWSTR, DWORD);
        using OpenSCManagerAFn = SC_HANDLE(WINAPI*)(LPCSTR, LPCSTR, DWORD);
        using OpenServiceWFn = SC_HANDLE(WINAPI*)(SC_HANDLE, LPCWSTR, DWORD);
        using OpenServiceAFn = SC_HANDLE(WINAPI*)(SC_HANDLE, LPCSTR, DWORD);
        using CreateServiceWFn = SC_HANDLE(WINAPI*)(SC_HANDLE, LPCWSTR, LPCWSTR, DWORD, DWORD, DWORD, DWORD, LPCWSTR, LPCWSTR, LPDWORD, LPCWSTR, LPCWSTR, LPCWSTR);
        using CreateServiceAFn = SC_HANDLE(WINAPI*)(SC_HANDLE, LPCSTR, LPCSTR, DWORD, DWORD, DWORD, DWORD, LPCSTR, LPCSTR, LPDWORD, LPCSTR, LPCSTR, LPCSTR);
        using ChangeServiceConfigWFn = BOOL(WINAPI*)(SC_HANDLE, DWORD, DWORD, DWORD, LPCWSTR, LPCWSTR, LPDWORD, LPCWSTR, LPCWSTR, LPCWSTR, LPCWSTR);
        using ChangeServiceConfigAFn = BOOL(WINAPI*)(SC_HANDLE, DWORD, DWORD, DWORD, LPCSTR, LPCSTR, LPDWORD, LPCSTR, LPCSTR, LPCSTR, LPCSTR);
        using StartServiceWFn = BOOL(WINAPI*)(SC_HANDLE, DWORD, LPCWSTR*);
        using StartServiceAFn = BOOL(WINAPI*)(SC_HANDLE, DWORD, LPCSTR*);
        using ControlServiceFn = BOOL(WINAPI*)(SC_HANDLE, DWORD, LPSERVICE_STATUS);
        using DeleteServiceFn = BOOL(WINAPI*)(SC_HANDLE);
        using CloseServiceHandleFn = BOOL(WINAPI*)(SC_HANDLE);
        using WSAIoctlFn = int(WSAAPI*)(SOCKET, DWORD, LPVOID, DWORD, LPVOID, DWORD, LPDWORD, LPWSAOVERLAPPED, LPWSAOVERLAPPED_COMPLETION_ROUTINE);
        using WSASendToFn = int(WSAAPI*)(SOCKET, LPWSABUF, DWORD, LPDWORD, DWORD, const sockaddr*, int, LPWSAOVERLAPPED, LPWSAOVERLAPPED_COMPLETION_ROUTINE);
        using WSARecvFromFn = int(WSAAPI*)(SOCKET, LPWSABUF, DWORD, LPDWORD, LPDWORD, sockaddr*, LPINT, LPWSAOVERLAPPED, LPWSAOVERLAPPED_COMPLETION_ROUTINE);
        using GetAddrInfoWFn = INT(WSAAPI*)(PCWSTR, PCWSTR, const ADDRINFOW*, PADDRINFOW*);
        using GetAddrInfoAFn = INT(WSAAPI*)(PCSTR, PCSTR, const ADDRINFOA*, PADDRINFOA*);
        using DnsQueryWFn = DNS_STATUS(WINAPI*)(PCWSTR, WORD, DWORD, PVOID, PDNS_RECORDW*, PVOID*);
        using DnsQueryAFn = DNS_STATUS(WINAPI*)(PCSTR, WORD, DWORD, PVOID, PDNS_RECORDA*, PVOID*);
        using WinHttpOpenFn = HINTERNET(WINAPI*)(LPCWSTR, DWORD, LPCWSTR, LPCWSTR, DWORD);
        using WinHttpConnectFn = HINTERNET(WINAPI*)(HINTERNET, LPCWSTR, INTERNET_PORT, DWORD);
        using WinHttpOpenRequestFn = HINTERNET(WINAPI*)(HINTERNET, LPCWSTR, LPCWSTR, LPCWSTR, LPCWSTR, LPCWSTR*, DWORD);
        using WinHttpSendRequestFn = BOOL(WINAPI*)(HINTERNET, LPCWSTR, DWORD, LPVOID, DWORD, DWORD, DWORD_PTR);
        using WinHttpReceiveResponseFn = BOOL(WINAPI*)(HINTERNET, LPVOID);
        using WinHttpReadDataFn = BOOL(WINAPI*)(HINTERNET, LPVOID, DWORD, LPDWORD);
        using WinHttpWriteDataFn = BOOL(WINAPI*)(HINTERNET, LPCVOID, DWORD, LPDWORD);
        using WinHttpCloseHandleFn = BOOL(WINAPI*)(HINTERNET);
        using InternetOpenWFn = HINTERNET(WINAPI*)(LPCWSTR, DWORD, LPCWSTR, LPCWSTR, DWORD);
        using InternetOpenAFn = HINTERNET(WINAPI*)(LPCSTR, DWORD, LPCSTR, LPCSTR, DWORD);
        using InternetConnectWFn = HINTERNET(WINAPI*)(HINTERNET, LPCWSTR, INTERNET_PORT, LPCWSTR, LPCWSTR, DWORD, DWORD, DWORD_PTR);
        using InternetConnectAFn = HINTERNET(WINAPI*)(HINTERNET, LPCSTR, INTERNET_PORT, LPCSTR, LPCSTR, DWORD, DWORD, DWORD_PTR);
        using HttpOpenRequestWFn = HINTERNET(WINAPI*)(HINTERNET, LPCWSTR, LPCWSTR, LPCWSTR, LPCWSTR, LPCWSTR*, DWORD, DWORD_PTR);
        using HttpOpenRequestAFn = HINTERNET(WINAPI*)(HINTERNET, LPCSTR, LPCSTR, LPCSTR, LPCSTR, LPCSTR*, DWORD, DWORD_PTR);
        using HttpSendRequestWFn = BOOL(WINAPI*)(HINTERNET, LPCWSTR, DWORD, LPVOID, DWORD);
        using HttpSendRequestAFn = BOOL(WINAPI*)(HINTERNET, LPCSTR, DWORD, LPVOID, DWORD);
        using InternetReadFileFn = BOOL(WINAPI*)(HINTERNET, LPVOID, DWORD, LPDWORD);
        using InternetWriteFileFn = BOOL(WINAPI*)(HINTERNET, LPCVOID, DWORD, LPDWORD);
        using InternetCloseHandleFn = BOOL(WINAPI*)(HINTERNET);
        using CryptAcquireContextWFn = BOOL(WINAPI*)(HCRYPTPROV*, LPCWSTR, LPCWSTR, DWORD, DWORD);
        using CryptAcquireContextAFn = BOOL(WINAPI*)(HCRYPTPROV*, LPCSTR, LPCSTR, DWORD, DWORD);
        using CryptCreateHashFn = BOOL(WINAPI*)(HCRYPTPROV, ALG_ID, HCRYPTKEY, DWORD, HCRYPTHASH*);
        using CryptHashDataFn = BOOL(WINAPI*)(HCRYPTHASH, const BYTE*, DWORD, DWORD);
        using CryptDeriveKeyFn = BOOL(WINAPI*)(HCRYPTPROV, ALG_ID, HCRYPTHASH, DWORD, HCRYPTKEY*);
        using CryptEncryptFn = BOOL(WINAPI*)(HCRYPTKEY, HCRYPTHASH, BOOL, DWORD, BYTE*, DWORD*, DWORD);
        using CryptDecryptFn = BOOL(WINAPI*)(HCRYPTKEY, HCRYPTHASH, BOOL, DWORD, BYTE*, DWORD*);
        using CryptGenRandomFn = BOOL(WINAPI*)(HCRYPTPROV, DWORD, BYTE*);
        using CryptReleaseContextFn = BOOL(WINAPI*)(HCRYPTPROV, DWORD);
        using BCryptOpenAlgorithmProviderFn = NTSTATUS(WINAPI*)(BCRYPT_ALG_HANDLE*, LPCWSTR, LPCWSTR, ULONG);
        using BCryptCreateHashFn = NTSTATUS(WINAPI*)(BCRYPT_ALG_HANDLE, BCRYPT_HASH_HANDLE*, PUCHAR, ULONG, PUCHAR, ULONG, ULONG);
        using BCryptHashDataFn = NTSTATUS(WINAPI*)(BCRYPT_HASH_HANDLE, PUCHAR, ULONG, ULONG);
        using BCryptFinishHashFn = NTSTATUS(WINAPI*)(BCRYPT_HASH_HANDLE, PUCHAR, ULONG, ULONG);
        using BCryptEncryptFn = NTSTATUS(WINAPI*)(BCRYPT_KEY_HANDLE, PUCHAR, ULONG, VOID*, PUCHAR, ULONG, PUCHAR, ULONG, ULONG*, ULONG);
        using BCryptDecryptFn = NTSTATUS(WINAPI*)(BCRYPT_KEY_HANDLE, PUCHAR, ULONG, VOID*, PUCHAR, ULONG, PUCHAR, ULONG, ULONG*, ULONG);
        using BCryptGenRandomFn = NTSTATUS(WINAPI*)(BCRYPT_ALG_HANDLE, PUCHAR, ULONG, ULONG);
        using BCryptCloseAlgorithmProviderFn = NTSTATUS(WINAPI*)(BCRYPT_ALG_HANDLE, ULONG);
        using BCryptDestroyHashFn = NTSTATUS(WINAPI*)(BCRYPT_HASH_HANDLE);
        using CoCreateInstanceFn = HRESULT(WINAPI*)(REFCLSID, LPUNKNOWN, DWORD, REFIID, LPVOID*);
        using CoCreateInstanceExFn = HRESULT(WINAPI*)(REFCLSID, IUnknown*, DWORD, COSERVERINFO*, DWORD, MULTI_QI*);
        using CoGetClassObjectFn = HRESULT(WINAPI*)(REFCLSID, DWORD, COSERVERINFO*, REFIID, LPVOID*);
        using VirtualAllocFn = LPVOID(WINAPI*)(LPVOID, SIZE_T, DWORD, DWORD);
        using VirtualFreeFn = BOOL(WINAPI*)(LPVOID, SIZE_T, DWORD);
        using VirtualProtectFn = BOOL(WINAPI*)(LPVOID, SIZE_T, DWORD, PDWORD);
        using CreateToolhelp32SnapshotFn = HANDLE(WINAPI*)(DWORD, DWORD);
        using Module32FirstWFn = BOOL(WINAPI*)(HANDLE, LPMODULEENTRY32W);
        using Module32NextWFn = BOOL(WINAPI*)(HANDLE, LPMODULEENTRY32W);
        using Module32FirstAFn = BOOL(WINAPI*)(HANDLE, tagMODULEENTRY32*);
        using Module32NextAFn = BOOL(WINAPI*)(HANDLE, tagMODULEENTRY32*);
        using GetModuleHandleWFn = HMODULE(WINAPI*)(LPCWSTR);
        using GetModuleHandleAFn = HMODULE(WINAPI*)(LPCSTR);
        using GetModuleHandleExWFn = BOOL(WINAPI*)(DWORD, LPCWSTR, HMODULE*);
        using GetModuleHandleExAFn = BOOL(WINAPI*)(DWORD, LPCSTR, HMODULE*);
        using GetModuleFileNameWFn = DWORD(WINAPI*)(HMODULE, LPWSTR, DWORD);
        using GetModuleFileNameAFn = DWORD(WINAPI*)(HMODULE, LPSTR, DWORD);
        using CreateHardLinkWFn = BOOL(WINAPI*)(LPCWSTR, LPCWSTR, LPSECURITY_ATTRIBUTES);
        using CreateHardLinkAFn = BOOL(WINAPI*)(LPCSTR, LPCSTR, LPSECURITY_ATTRIBUTES);
        using ReplaceFileWFn = BOOL(WINAPI*)(LPCWSTR, LPCWSTR, LPCWSTR, DWORD, LPVOID, LPVOID);
        using ReplaceFileAFn = BOOL(WINAPI*)(LPCSTR, LPCSTR, LPCSTR, DWORD, LPVOID, LPVOID);
        using SetEndOfFileFn = BOOL(WINAPI*)(HANDLE);
        using LockFileExFn = BOOL(WINAPI*)(HANDLE, DWORD, DWORD, DWORD, DWORD, LPOVERLAPPED);
        using UnlockFileExFn = BOOL(WINAPI*)(HANDLE, DWORD, DWORD, DWORD, LPOVERLAPPED);
        using ShellExecuteWFn = HINSTANCE(WINAPI*)(HWND, LPCWSTR, LPCWSTR, LPCWSTR, LPCWSTR, INT);
        using ShellExecuteAFn = HINSTANCE(WINAPI*)(HWND, LPCSTR, LPCSTR, LPCSTR, LPCSTR, INT);
        using CreateProcessWithLogonWFn = BOOL(WINAPI*)(LPCWSTR, LPCWSTR, LPCWSTR, DWORD, LPCWSTR, LPWSTR, DWORD, LPVOID, LPCWSTR, LPSTARTUPINFOW, LPPROCESS_INFORMATION);
        using ChangeServiceConfig2WFn = BOOL(WINAPI*)(SC_HANDLE, DWORD, LPVOID);
        using ChangeServiceConfig2AFn = BOOL(WINAPI*)(SC_HANDLE, DWORD, LPVOID);
        using QueryServiceStatusExFn = BOOL(WINAPI*)(SC_HANDLE, SC_STATUS_TYPE, LPBYTE, DWORD, LPDWORD);
        using QueryServiceConfigWFn = BOOL(WINAPI*)(SC_HANDLE, LPQUERY_SERVICE_CONFIGW, DWORD, LPDWORD);
        using QueryServiceConfigAFn = BOOL(WINAPI*)(SC_HANDLE, LPQUERY_SERVICE_CONFIGA, DWORD, LPDWORD);
        using EnumServicesStatusExWFn = BOOL(WINAPI*)(SC_HANDLE, SC_ENUM_TYPE, DWORD, DWORD, LPBYTE, DWORD, LPDWORD, LPDWORD, LPDWORD, LPCWSTR);
        using EnumServicesStatusExAFn = BOOL(WINAPI*)(SC_HANDLE, SC_ENUM_TYPE, DWORD, DWORD, LPBYTE, DWORD, LPDWORD, LPDWORD, LPDWORD, LPCSTR);
        using WinHttpQueryHeadersFn = BOOL(WINAPI*)(HINTERNET, DWORD, LPCWSTR, LPVOID, LPDWORD, LPDWORD);
        using WinHttpQueryDataAvailableFn = BOOL(WINAPI*)(HINTERNET, LPDWORD);
        using WinHttpSetOptionFn = BOOL(WINAPI*)(HINTERNET, DWORD, LPVOID, DWORD);
        using InternetOpenUrlWFn = HINTERNET(WINAPI*)(HINTERNET, LPCWSTR, LPCWSTR, DWORD, DWORD, DWORD_PTR);
        using InternetOpenUrlAFn = HINTERNET(WINAPI*)(HINTERNET, LPCSTR, LPCSTR, DWORD, DWORD, DWORD_PTR);
        using InternetQueryDataAvailableFn = BOOL(WINAPI*)(HINTERNET, LPDWORD, DWORD, DWORD_PTR);
        using InternetSetOptionWFn = BOOL(WINAPI*)(HINTERNET, DWORD, LPVOID, DWORD);
        using InternetSetOptionAFn = BOOL(WINAPI*)(HINTERNET, DWORD, LPVOID, DWORD);
        using InternetCrackUrlWFn = BOOL(WINAPI*)(LPCWSTR, DWORD, DWORD, LPURL_COMPONENTSW);
        using InternetCrackUrlAFn = BOOL(WINAPI*)(LPCSTR, DWORD, DWORD, LPURL_COMPONENTSA);
        using URLDownloadToFileWFn = HRESULT(WINAPI*)(LPUNKNOWN, LPCWSTR, LPCWSTR, DWORD, LPBINDSTATUSCALLBACK);
        using URLDownloadToFileAFn = HRESULT(WINAPI*)(LPUNKNOWN, LPCSTR, LPCSTR, DWORD, LPBINDSTATUSCALLBACK);
        using CryptImportKeyFn = BOOL(WINAPI*)(HCRYPTPROV, const BYTE*, DWORD, HCRYPTKEY, DWORD, HCRYPTKEY*);
        using CryptExportKeyFn = BOOL(WINAPI*)(HCRYPTKEY, HCRYPTKEY, DWORD, DWORD, BYTE*, DWORD*);
        using CryptDestroyKeyFn = BOOL(WINAPI*)(HCRYPTKEY);
        using CryptDestroyHashFn = BOOL(WINAPI*)(HCRYPTHASH);
        using BCryptGenerateSymmetricKeyFn = NTSTATUS(WINAPI*)(BCRYPT_ALG_HANDLE, BCRYPT_KEY_HANDLE*, PUCHAR, ULONG, PUCHAR, ULONG, ULONG);
        using BCryptImportKeyFn = NTSTATUS(WINAPI*)(BCRYPT_ALG_HANDLE, BCRYPT_KEY_HANDLE, LPCWSTR, BCRYPT_KEY_HANDLE*, PUCHAR, ULONG, PUCHAR, ULONG, ULONG);
        using BCryptImportKeyPairFn = NTSTATUS(WINAPI*)(BCRYPT_ALG_HANDLE, BCRYPT_KEY_HANDLE, LPCWSTR, BCRYPT_KEY_HANDLE*, PUCHAR, ULONG, ULONG);
        using BCryptDestroyKeyFn = NTSTATUS(WINAPI*)(BCRYPT_KEY_HANDLE);
        using CoInitializeExFn = HRESULT(WINAPI*)(LPVOID, DWORD);
        using CoInitializeSecurityFn = HRESULT(WINAPI*)(PSECURITY_DESCRIPTOR, LONG, SOLE_AUTHENTICATION_SERVICE*, void*, DWORD, DWORD, void*, DWORD, void*);
        using CoUninitializeFn = void(WINAPI*)();
        using CreateRemoteThreadExFn = HANDLE(WINAPI*)(HANDLE, LPSECURITY_ATTRIBUTES, SIZE_T, LPTHREAD_START_ROUTINE, LPVOID, DWORD, LPPROC_THREAD_ATTRIBUTE_LIST, LPDWORD);
        using CreateSymbolicLinkWFn = BOOLEAN(WINAPI*)(LPCWSTR, LPCWSTR, DWORD);
        using CreateSymbolicLinkAFn = BOOLEAN(WINAPI*)(LPCSTR, LPCSTR, DWORD);
        using GetFinalPathNameByHandleWFn = DWORD(WINAPI*)(HANDLE, LPWSTR, DWORD, DWORD);
        using GetFinalPathNameByHandleAFn = DWORD(WINAPI*)(HANDLE, LPSTR, DWORD, DWORD);
        using GetFileSizeExFn = BOOL(WINAPI*)(HANDLE, PLARGE_INTEGER);
        using SetFilePointerExFn = BOOL(WINAPI*)(HANDLE, LARGE_INTEGER, PLARGE_INTEGER, DWORD);
        using CreateNamedPipeWFn = HANDLE(WINAPI*)(LPCWSTR, DWORD, DWORD, DWORD, DWORD, DWORD, DWORD, LPSECURITY_ATTRIBUTES);
        using CreateNamedPipeAFn = HANDLE(WINAPI*)(LPCSTR, DWORD, DWORD, DWORD, DWORD, DWORD, DWORD, LPSECURITY_ATTRIBUTES);
        using ConnectNamedPipeFn = BOOL(WINAPI*)(HANDLE, LPOVERLAPPED);
        using DisconnectNamedPipeFn = BOOL(WINAPI*)(HANDLE);
        using WaitNamedPipeWFn = BOOL(WINAPI*)(LPCWSTR, DWORD);
        using WaitNamedPipeAFn = BOOL(WINAPI*)(LPCSTR, DWORD);
        using TransactNamedPipeFn = BOOL(WINAPI*)(HANDLE, LPVOID, DWORD, LPVOID, DWORD, LPDWORD, LPOVERLAPPED);
        using CreateMutexWFn = HANDLE(WINAPI*)(LPSECURITY_ATTRIBUTES, BOOL, LPCWSTR);
        using CreateMutexAFn = HANDLE(WINAPI*)(LPSECURITY_ATTRIBUTES, BOOL, LPCSTR);
        using OpenMutexWFn = HANDLE(WINAPI*)(DWORD, BOOL, LPCWSTR);
        using OpenMutexAFn = HANDLE(WINAPI*)(DWORD, BOOL, LPCSTR);
        using CreateEventWFn = HANDLE(WINAPI*)(LPSECURITY_ATTRIBUTES, BOOL, BOOL, LPCWSTR);
        using CreateEventAFn = HANDLE(WINAPI*)(LPSECURITY_ATTRIBUTES, BOOL, BOOL, LPCSTR);
        using OpenEventWFn = HANDLE(WINAPI*)(DWORD, BOOL, LPCWSTR);
        using OpenEventAFn = HANDLE(WINAPI*)(DWORD, BOOL, LPCSTR);
        using CreateSemaphoreWFn = HANDLE(WINAPI*)(LPSECURITY_ATTRIBUTES, LONG, LONG, LPCWSTR);
        using CreateSemaphoreAFn = HANDLE(WINAPI*)(LPSECURITY_ATTRIBUTES, LONG, LONG, LPCSTR);
        using OpenSemaphoreWFn = HANDLE(WINAPI*)(DWORD, BOOL, LPCWSTR);
        using OpenSemaphoreAFn = HANDLE(WINAPI*)(DWORD, BOOL, LPCSTR);
        using WaitForSingleObjectFn = DWORD(WINAPI*)(HANDLE, DWORD);
        using WaitForMultipleObjectsFn = DWORD(WINAPI*)(DWORD, const HANDLE*, BOOL, DWORD);
        using SetEventFn = BOOL(WINAPI*)(HANDLE);
        using ResetEventFn = BOOL(WINAPI*)(HANDLE);
        using ReleaseMutexFn = BOOL(WINAPI*)(HANDLE);
        using ReleaseSemaphoreFn = BOOL(WINAPI*)(HANDLE, LONG, LPLONG);
        using GetEnvironmentVariableWFn = DWORD(WINAPI*)(LPCWSTR, LPWSTR, DWORD);
        using GetEnvironmentVariableAFn = DWORD(WINAPI*)(LPCSTR, LPSTR, DWORD);
        using SetEnvironmentVariableWFn = BOOL(WINAPI*)(LPCWSTR, LPCWSTR);
        using SetEnvironmentVariableAFn = BOOL(WINAPI*)(LPCSTR, LPCSTR);
        using ExpandEnvironmentStringsWFn = DWORD(WINAPI*)(LPCWSTR, LPWSTR, DWORD);
        using ExpandEnvironmentStringsAFn = DWORD(WINAPI*)(LPCSTR, LPSTR, DWORD);
        using SetDllDirectoryWFn = BOOL(WINAPI*)(LPCWSTR);
        using SetDllDirectoryAFn = BOOL(WINAPI*)(LPCSTR);
        using SetDefaultDllDirectoriesFn = BOOL(WINAPI*)(DWORD);
        using AddDllDirectoryFn = DLL_DIRECTORY_COOKIE(WINAPI*)(PCWSTR);
        using RemoveDllDirectoryFn = BOOL(WINAPI*)(DLL_DIRECTORY_COOKIE);
        using ImpersonateLoggedOnUserFn = BOOL(WINAPI*)(HANDLE);
        using RevertToSelfFn = BOOL(WINAPI*)();
        using SetThreadTokenFn = BOOL(WINAPI*)(PHANDLE, HANDLE);
        using SetWindowsHookExWFn = HHOOK(WINAPI*)(int, HOOKPROC, HINSTANCE, DWORD);
        using SetWindowsHookExAFn = HHOOK(WINAPI*)(int, HOOKPROC, HINSTANCE, DWORD);
        using UnhookWindowsHookExFn = BOOL(WINAPI*)(HHOOK);
        using EnumProcessesFn = BOOL(WINAPI*)(DWORD*, DWORD, DWORD*);
        using EnumProcessModulesFn = BOOL(WINAPI*)(HANDLE, HMODULE*, DWORD, LPDWORD);
        using EnumProcessModulesExFn = BOOL(WINAPI*)(HANDLE, HMODULE*, DWORD, LPDWORD, DWORD);
        using GetMappedFileNameWFn = DWORD(WINAPI*)(HANDLE, LPVOID, LPWSTR, DWORD);
        using GetMappedFileNameAFn = DWORD(WINAPI*)(HANDLE, LPVOID, LPSTR, DWORD);
        using EnumWindowsFn = BOOL(WINAPI*)(WNDENUMPROC, LPARAM);
        using EnumChildWindowsFn = BOOL(WINAPI*)(HWND, WNDENUMPROC, LPARAM);
        using FindWindowWFn = HWND(WINAPI*)(LPCWSTR, LPCWSTR);
        using FindWindowAFn = HWND(WINAPI*)(LPCSTR, LPCSTR);
        using FindWindowExWFn = HWND(WINAPI*)(HWND, HWND, LPCWSTR, LPCWSTR);
        using FindWindowExAFn = HWND(WINAPI*)(HWND, HWND, LPCSTR, LPCSTR);
        using GetWindowThreadProcessIdFn = DWORD(WINAPI*)(HWND, LPDWORD);
        using GetForegroundWindowFn = HWND(WINAPI*)();
        using GetDCFn = HDC(WINAPI*)(HWND);
        using ReleaseDCFn = int(WINAPI*)(HWND, HDC);
        using CreateCompatibleDCFn = HDC(WINAPI*)(HDC);
        using DeleteDCFn = BOOL(WINAPI*)(HDC);
        using CreateCompatibleBitmapFn = HBITMAP(WINAPI*)(HDC, int, int);
        using BitBltFn = BOOL(WINAPI*)(HDC, int, int, int, int, HDC, int, int, DWORD);
        using StretchBltFn = BOOL(WINAPI*)(HDC, int, int, int, int, HDC, int, int, int, int, DWORD);
        using DeleteObjectFn = BOOL(WINAPI*)(HGDIOBJ);
        using OpenClipboardFn = BOOL(WINAPI*)(HWND);
        using CloseClipboardFn = BOOL(WINAPI*)();
        using GetClipboardDataFn = HANDLE(WINAPI*)(UINT);
        using SetClipboardDataFn = HANDLE(WINAPI*)(UINT, HANDLE);
        using EmptyClipboardFn = BOOL(WINAPI*)();
        using StartTraceWFn = ULONG(WINAPI*)(PTRACEHANDLE, LPCWSTR, PEVENT_TRACE_PROPERTIES);
        using StartTraceAFn = ULONG(WINAPI*)(PTRACEHANDLE, LPCSTR, PEVENT_TRACE_PROPERTIES);
        using ControlTraceWFn = ULONG(WINAPI*)(TRACEHANDLE, LPCWSTR, PEVENT_TRACE_PROPERTIES, ULONG);
        using ControlTraceAFn = ULONG(WINAPI*)(TRACEHANDLE, LPCSTR, PEVENT_TRACE_PROPERTIES, ULONG);
        using EnableTraceEx2Fn = ULONG(WINAPI*)(TRACEHANDLE, LPCGUID, ULONG, UCHAR, ULONGLONG, ULONGLONG, ULONG, void*);
        using OpenTraceWFn = TRACEHANDLE(WINAPI*)(PEVENT_TRACE_LOGFILEW);
        using OpenTraceAFn = TRACEHANDLE(WINAPI*)(PEVENT_TRACE_LOGFILEA);
        using ProcessTraceFn = ULONG(WINAPI*)(PTRACEHANDLE, ULONG, LPFILETIME, LPFILETIME);
        using CloseTraceFn = ULONG(WINAPI*)(TRACEHANDLE);
        using EventRegisterFn = ULONG(WINAPI*)(LPCGUID, PENABLECALLBACK, PVOID, PREGHANDLE);
        using EventUnregisterFn = ULONG(WINAPI*)(REGHANDLE);
        using EventWriteFn = ULONG(WINAPI*)(REGHANDLE, PCEVENT_DESCRIPTOR, ULONG, PEVENT_DATA_DESCRIPTOR);
        using EventWriteExFn = ULONG(WINAPI*)(REGHANDLE, PCEVENT_DESCRIPTOR, ULONG64, ULONG, LPCGUID, LPCGUID, ULONG, PEVENT_DATA_DESCRIPTOR);
        using WinVerifyTrustFn = LONG(WINAPI*)(HWND, GUID*, LPVOID);
        using CryptQueryObjectFn = BOOL(WINAPI*)(DWORD, const void*, DWORD, DWORD, DWORD, DWORD*, DWORD*, DWORD*, HCERTSTORE*, HCRYPTMSG*, const void**);
        using CertOpenStoreFn = HCERTSTORE(WINAPI*)(LPCSTR, DWORD, HCRYPTPROV_LEGACY, DWORD, const void*);
        using CertCloseStoreFn = BOOL(WINAPI*)(HCERTSTORE, DWORD);
        using CertFindCertificateInStoreFn = PCCERT_CONTEXT(WINAPI*)(HCERTSTORE, DWORD, DWORD, DWORD, const void*, PCCERT_CONTEXT);
        using CertGetCertificateChainFn = BOOL(WINAPI*)(HCERTCHAINENGINE, PCCERT_CONTEXT, LPFILETIME, HCERTSTORE, PCERT_CHAIN_PARA, DWORD, LPVOID, PCCERT_CHAIN_CONTEXT*);
        using CertVerifyCertificateChainPolicyFn = BOOL(WINAPI*)(LPCSTR, PCCERT_CHAIN_CONTEXT, PCERT_CHAIN_POLICY_PARA, PCERT_CHAIN_POLICY_STATUS);
        using CryptProtectDataFn = BOOL(WINAPI*)(DATA_BLOB*, LPCWSTR, DATA_BLOB*, PVOID, CRYPTPROTECT_PROMPTSTRUCT*, DWORD, DATA_BLOB*);
        using CryptUnprotectDataFn = BOOL(WINAPI*)(DATA_BLOB*, LPWSTR*, DATA_BLOB*, PVOID, CRYPTPROTECT_PROMPTSTRUCT*, DWORD, DATA_BLOB*);
        using NCryptOpenStorageProviderFn = SECURITY_STATUS(WINAPI*)(NCRYPT_PROV_HANDLE*, LPCWSTR, DWORD);
        using NCryptOpenKeyFn = SECURITY_STATUS(WINAPI*)(NCRYPT_PROV_HANDLE, NCRYPT_KEY_HANDLE*, LPCWSTR, DWORD, DWORD);
        using NCryptCreatePersistedKeyFn = SECURITY_STATUS(WINAPI*)(NCRYPT_PROV_HANDLE, NCRYPT_KEY_HANDLE*, LPCWSTR, LPCWSTR, DWORD, DWORD);
        using NCryptFinalizeKeyFn = SECURITY_STATUS(WINAPI*)(NCRYPT_KEY_HANDLE, DWORD);
        using NCryptEncryptFn = SECURITY_STATUS(WINAPI*)(NCRYPT_KEY_HANDLE, PBYTE, DWORD, VOID*, PBYTE, DWORD, DWORD*, DWORD);
        using NCryptDecryptFn = SECURITY_STATUS(WINAPI*)(NCRYPT_KEY_HANDLE, PBYTE, DWORD, VOID*, PBYTE, DWORD, DWORD*, DWORD);
        using NCryptSignHashFn = SECURITY_STATUS(WINAPI*)(NCRYPT_KEY_HANDLE, VOID*, PBYTE, DWORD, PBYTE, DWORD, DWORD*, DWORD);
        using NCryptVerifySignatureFn = SECURITY_STATUS(WINAPI*)(NCRYPT_KEY_HANDLE, VOID*, PBYTE, DWORD, PBYTE, DWORD, DWORD);
        using NCryptExportKeyFn = SECURITY_STATUS(WINAPI*)(NCRYPT_KEY_HANDLE, NCRYPT_KEY_HANDLE, LPCWSTR, VOID*, PBYTE, DWORD, DWORD*, DWORD);
        using NCryptImportKeyFn = SECURITY_STATUS(WINAPI*)(NCRYPT_PROV_HANDLE, NCRYPT_KEY_HANDLE, LPCWSTR, VOID*, NCRYPT_KEY_HANDLE*, PBYTE, DWORD, DWORD);
        using NCryptDeleteKeyFn = SECURITY_STATUS(WINAPI*)(NCRYPT_KEY_HANDLE, DWORD);
        using NCryptFreeObjectFn = SECURITY_STATUS(WINAPI*)(NCRYPT_HANDLE);
        using RpcStringBindingComposeWFn = RPC_STATUS(RPC_ENTRY*)(RPC_WSTR, RPC_WSTR, RPC_WSTR, RPC_WSTR, RPC_WSTR, RPC_WSTR*);
        using RpcStringBindingComposeAFn = RPC_STATUS(RPC_ENTRY*)(RPC_CSTR, RPC_CSTR, RPC_CSTR, RPC_CSTR, RPC_CSTR, RPC_CSTR*);
        using RpcBindingFromStringBindingWFn = RPC_STATUS(RPC_ENTRY*)(RPC_WSTR, RPC_BINDING_HANDLE*);
        using RpcBindingFromStringBindingAFn = RPC_STATUS(RPC_ENTRY*)(RPC_CSTR, RPC_BINDING_HANDLE*);
        using RpcBindingFreeFn = RPC_STATUS(RPC_ENTRY*)(RPC_BINDING_HANDLE*);
        using RpcMgmtEpEltInqBeginFn = RPC_STATUS(RPC_ENTRY*)(RPC_BINDING_HANDLE, unsigned long, RPC_IF_ID*, unsigned long, UUID*, RPC_EP_INQ_HANDLE*);
        using RpcMgmtEpEltInqNextWFn = RPC_STATUS(RPC_ENTRY*)(RPC_EP_INQ_HANDLE, RPC_IF_ID*, RPC_BINDING_HANDLE*, UUID*, RPC_WSTR*);
        using RpcMgmtEpEltInqNextAFn = RPC_STATUS(RPC_ENTRY*)(RPC_EP_INQ_HANDLE, RPC_IF_ID*, RPC_BINDING_HANDLE*, UUID*, RPC_CSTR*);
        using RpcMgmtEpEltInqDoneFn = RPC_STATUS(RPC_ENTRY*)(RPC_EP_INQ_HANDLE*);
        using NtQueryInformationTokenFn = NTSTATUS(NTAPI*)(HANDLE, ULONG, PVOID, ULONG, PULONG);
        using NtSetInformationTokenFn = NTSTATUS(NTAPI*)(HANDLE, ULONG, PVOID, ULONG);
        using NtAdjustPrivilegesTokenFn = NTSTATUS(NTAPI*)(HANDLE, BOOLEAN, PTOKEN_PRIVILEGES, ULONG, PTOKEN_PRIVILEGES, PULONG);
        using NtCreateMutantFn = NTSTATUS(NTAPI*)(PHANDLE, ACCESS_MASK, POBJECT_ATTRIBUTES, BOOLEAN);
        using NtOpenMutantFn = NTSTATUS(NTAPI*)(PHANDLE, ACCESS_MASK, POBJECT_ATTRIBUTES);
        using NtReleaseMutantFn = NTSTATUS(NTAPI*)(HANDLE, PLONG);
        using NtCreateEventFn = NTSTATUS(NTAPI*)(PHANDLE, ACCESS_MASK, POBJECT_ATTRIBUTES, ULONG, BOOLEAN);
        using NtOpenEventFn = NTSTATUS(NTAPI*)(PHANDLE, ACCESS_MASK, POBJECT_ATTRIBUTES);
        using NtSetEventFn = NTSTATUS(NTAPI*)(HANDLE, PLONG);
        using NtResetEventFn = NTSTATUS(NTAPI*)(HANDLE, PLONG);
        using NtWaitForSingleObjectFn = NTSTATUS(NTAPI*)(HANDLE, BOOLEAN, PLARGE_INTEGER);
        using NtWaitForMultipleObjectsFn = NTSTATUS(NTAPI*)(ULONG, HANDLE*, ULONG, BOOLEAN, PLARGE_INTEGER);
        using NtQuerySystemInformationFn = NTSTATUS(NTAPI*)(ULONG, PVOID, ULONG, PULONG);
        using NtQueryObjectFn = NTSTATUS(NTAPI*)(HANDLE, ULONG, PVOID, ULONG, PULONG);
        using LogonUserWFn = BOOL(WINAPI*)(LPCWSTR, LPCWSTR, LPCWSTR, DWORD, DWORD, PHANDLE);
        using LogonUserAFn = BOOL(WINAPI*)(LPCSTR, LPCSTR, LPCSTR, DWORD, DWORD, PHANDLE);
        using GetTokenInformationFn = BOOL(WINAPI*)(HANDLE, TOKEN_INFORMATION_CLASS, LPVOID, DWORD, PDWORD);
        using SetTokenInformationFn = BOOL(WINAPI*)(HANDLE, TOKEN_INFORMATION_CLASS, LPVOID, DWORD);
        using CheckTokenMembershipFn = BOOL(WINAPI*)(HANDLE, PSID, PBOOL);
        using CreateRestrictedTokenFn = BOOL(WINAPI*)(HANDLE, DWORD, DWORD, PSID_AND_ATTRIBUTES, DWORD, PLUID_AND_ATTRIBUTES, DWORD, PSID_AND_ATTRIBUTES, PHANDLE);
        using ImpersonateSelfFn = BOOL(WINAPI*)(SECURITY_IMPERSONATION_LEVEL);
        using ImpersonateNamedPipeClientFn = BOOL(WINAPI*)(HANDLE);
        using CredReadWFn = BOOL(WINAPI*)(LPCWSTR, DWORD, DWORD, PVOID*);
        using CredReadAFn = BOOL(WINAPI*)(LPCSTR, DWORD, DWORD, PVOID*);
        using CredEnumerateWFn = BOOL(WINAPI*)(LPCWSTR, DWORD, DWORD*, PVOID*);
        using CredEnumerateAFn = BOOL(WINAPI*)(LPCSTR, DWORD, DWORD*, PVOID*);
        using CredWriteWFn = BOOL(WINAPI*)(PVOID, DWORD);
        using CredWriteAFn = BOOL(WINAPI*)(PVOID, DWORD);
        using CredDeleteWFn = BOOL(WINAPI*)(LPCWSTR, DWORD, DWORD);
        using CredDeleteAFn = BOOL(WINAPI*)(LPCSTR, DWORD, DWORD);
        using CredFreeFn = VOID(WINAPI*)(PVOID);
        using LsaOpenPolicyFn = NTSTATUS(WINAPI*)(PUNICODE_STRING, PVOID, ACCESS_MASK, PVOID*);
        using LsaCloseFn = NTSTATUS(WINAPI*)(PVOID);
        using LsaEnumerateLogonSessionsFn = NTSTATUS(WINAPI*)(PULONG, PVOID*);
        using LsaGetLogonSessionDataFn = NTSTATUS(WINAPI*)(PVOID, PVOID*);
        using LsaFreeReturnBufferFn = NTSTATUS(WINAPI*)(PVOID);
        using LsaLookupNames2Fn = NTSTATUS(WINAPI*)(PVOID, ULONG, ULONG, PUNICODE_STRING, PVOID*, PVOID*);
        using LsaLookupSids2Fn = NTSTATUS(WINAPI*)(PVOID, ULONG, ULONG, PVOID*, PVOID*, PVOID*);
        using OpenEventLogWFn = HANDLE(WINAPI*)(LPCWSTR, LPCWSTR);
        using OpenEventLogAFn = HANDLE(WINAPI*)(LPCSTR, LPCSTR);
        using RegisterEventSourceWFn = HANDLE(WINAPI*)(LPCWSTR, LPCWSTR);
        using RegisterEventSourceAFn = HANDLE(WINAPI*)(LPCSTR, LPCSTR);
        using ReadEventLogWFn = BOOL(WINAPI*)(HANDLE, DWORD, DWORD, LPVOID, DWORD, DWORD*, DWORD*);
        using ReadEventLogAFn = BOOL(WINAPI*)(HANDLE, DWORD, DWORD, LPVOID, DWORD, DWORD*, DWORD*);
        using ClearEventLogWFn = BOOL(WINAPI*)(HANDLE, LPCWSTR);
        using ClearEventLogAFn = BOOL(WINAPI*)(HANDLE, LPCSTR);
        using ReportEventWFn = BOOL(WINAPI*)(HANDLE, WORD, WORD, DWORD, PSID, WORD, DWORD, LPCWSTR*, LPVOID);
        using ReportEventAFn = BOOL(WINAPI*)(HANDLE, WORD, WORD, DWORD, PSID, WORD, DWORD, LPCSTR*, LPVOID);
        using CloseEventLogFn = BOOL(WINAPI*)(HANDLE);
        using NetUserEnumFn = DWORD(WINAPI*)(LPCWSTR, DWORD, DWORD, LPBYTE*, DWORD, LPDWORD, LPDWORD, LPDWORD);
        using NetLocalGroupEnumFn = DWORD(WINAPI*)(LPCWSTR, DWORD, LPBYTE*, DWORD, LPDWORD, LPDWORD, PDWORD_PTR);
        using NetGroupEnumFn = DWORD(WINAPI*)(LPCWSTR, DWORD, LPBYTE*, DWORD, LPDWORD, LPDWORD, PDWORD_PTR);
        using NetShareEnumFn = DWORD(WINAPI*)(LPWSTR, DWORD, LPBYTE*, DWORD, LPDWORD, LPDWORD, LPDWORD);
        using NetSessionEnumFn = DWORD(WINAPI*)(LPWSTR, LPWSTR, LPWSTR, DWORD, LPBYTE*, DWORD, LPDWORD, LPDWORD, LPDWORD);
        using NetServerEnumFn = DWORD(WINAPI*)(LPCWSTR, DWORD, LPBYTE*, DWORD, LPDWORD, LPDWORD, DWORD, LPCWSTR, LPDWORD);
        using NetWkstaGetInfoFn = DWORD(WINAPI*)(LPWSTR, DWORD, LPBYTE*);
        using NetApiBufferFreeFn = DWORD(WINAPI*)(LPVOID);
        using GetExtendedTcpTableFn = ULONG(WINAPI*)(PVOID, PDWORD, BOOL, ULONG, ULONG, ULONG);
        using GetExtendedUdpTableFn = ULONG(WINAPI*)(PVOID, PDWORD, BOOL, ULONG, ULONG, ULONG);
        using GetTcpTable2Fn = ULONG(WINAPI*)(PVOID, PULONG, BOOL);
        using GetUdpTableFn = ULONG(WINAPI*)(PVOID, PDWORD, BOOL);
        using GetAdaptersAddressesFn = ULONG(WINAPI*)(ULONG, ULONG, PVOID, PVOID, PULONG);
        using GetNetworkParamsFn = DWORD(WINAPI*)(PVOID, PULONG);
        using GetIpNetTable2Fn = ULONG(WINAPI*)(USHORT, PVOID*);
        using GetIfTable2Fn = ULONG(WINAPI*)(PVOID*);
        using FreeMibTableFn = VOID(WINAPI*)(PVOID);
        using WTSOpenServerWFn = HANDLE(WINAPI*)(LPWSTR);
        using WTSOpenServerAFn = HANDLE(WINAPI*)(LPSTR);
        using WTSCloseServerFn = VOID(WINAPI*)(HANDLE);
        using WTSEnumerateSessionsWFn = BOOL(WINAPI*)(HANDLE, DWORD, DWORD, PVOID*, DWORD*);
        using WTSEnumerateSessionsAFn = BOOL(WINAPI*)(HANDLE, DWORD, DWORD, PVOID*, DWORD*);
        using WTSEnumerateProcessesWFn = BOOL(WINAPI*)(HANDLE, DWORD, DWORD, PVOID*, DWORD*);
        using WTSEnumerateProcessesAFn = BOOL(WINAPI*)(HANDLE, DWORD, DWORD, PVOID*, DWORD*);
        using WTSQuerySessionInformationWFn = BOOL(WINAPI*)(HANDLE, DWORD, int, LPWSTR*, DWORD*);
        using WTSQuerySessionInformationAFn = BOOL(WINAPI*)(HANDLE, DWORD, int, LPSTR*, DWORD*);
        using WTSFreeMemoryFn = VOID(WINAPI*)(PVOID);
        using CreateJobObjectWFn = HANDLE(WINAPI*)(LPSECURITY_ATTRIBUTES, LPCWSTR);
        using CreateJobObjectAFn = HANDLE(WINAPI*)(LPSECURITY_ATTRIBUTES, LPCSTR);
        using OpenJobObjectWFn = HANDLE(WINAPI*)(DWORD, BOOL, LPCWSTR);
        using OpenJobObjectAFn = HANDLE(WINAPI*)(DWORD, BOOL, LPCSTR);
        using AssignProcessToJobObjectFn = BOOL(WINAPI*)(HANDLE, HANDLE);
        using TerminateJobObjectFn = BOOL(WINAPI*)(HANDLE, UINT);
        using SetInformationJobObjectFn = BOOL(WINAPI*)(HANDLE, JOBOBJECTINFOCLASS, LPVOID, DWORD);
        using QueryInformationJobObjectFn = BOOL(WINAPI*)(HANDLE, JOBOBJECTINFOCLASS, LPVOID, DWORD, LPDWORD);
        using AcquireCredentialsHandleWFn = SECURITY_STATUS(WINAPI*)(LPWSTR, LPWSTR, ULONG, PVOID, PVOID, PVOID, PVOID, PVOID, PVOID);
        using AcquireCredentialsHandleAFn = SECURITY_STATUS(WINAPI*)(LPSTR, LPSTR, ULONG, PVOID, PVOID, PVOID, PVOID, PVOID, PVOID);
        using InitializeSecurityContextWFn = SECURITY_STATUS(WINAPI*)(PVOID, PVOID, LPWSTR, ULONG, ULONG, ULONG, PVOID, ULONG, PVOID, PVOID, PULONG, PVOID);
        using InitializeSecurityContextAFn = SECURITY_STATUS(WINAPI*)(PVOID, PVOID, LPSTR, ULONG, ULONG, ULONG, PVOID, ULONG, PVOID, PVOID, PULONG, PVOID);
        using AcceptSecurityContextFn = SECURITY_STATUS(WINAPI*)(PVOID, PVOID, PVOID, ULONG, ULONG, PVOID, PVOID, PULONG, PVOID);
        using EncryptMessageFn = SECURITY_STATUS(WINAPI*)(PVOID, ULONG, PVOID, ULONG);
        using DecryptMessageFn = SECURITY_STATUS(WINAPI*)(PVOID, PVOID, ULONG, PULONG);
        using DeleteSecurityContextFn = SECURITY_STATUS(WINAPI*)(PVOID);
        using FreeCredentialsHandleFn = SECURITY_STATUS(WINAPI*)(PVOID);
        using Process32FirstWFn = BOOL(WINAPI*)(HANDLE, PROCESSENTRY32W*);
        using Process32FirstAFn = BOOL(WINAPI*)(HANDLE, tagPROCESSENTRY32*);
        using Process32NextWFn = BOOL(WINAPI*)(HANDLE, PROCESSENTRY32W*);
        using Process32NextAFn = BOOL(WINAPI*)(HANDLE, tagPROCESSENTRY32*);
        using Thread32FirstFn = BOOL(WINAPI*)(HANDLE, LPTHREADENTRY32);
        using Thread32NextFn = BOOL(WINAPI*)(HANDLE, LPTHREADENTRY32);
        using Heap32ListFirstFn = BOOL(WINAPI*)(HANDLE, LPHEAPLIST32);
        using Heap32ListNextFn = BOOL(WINAPI*)(HANDLE, LPHEAPLIST32);
        using Heap32FirstFn = BOOL(WINAPI*)(LPHEAPENTRY32, DWORD, ULONG_PTR);
        using Heap32NextFn = BOOL(WINAPI*)(LPHEAPENTRY32);
        using QueryFullProcessImageNameWFn = BOOL(WINAPI*)(HANDLE, DWORD, LPWSTR, PDWORD);
        using QueryFullProcessImageNameAFn = BOOL(WINAPI*)(HANDLE, DWORD, LPSTR, PDWORD);
        using GetProcessImageFileNameWFn = DWORD(WINAPI*)(HANDLE, LPWSTR, DWORD);
        using GetProcessImageFileNameAFn = DWORD(WINAPI*)(HANDLE, LPSTR, DWORD);
        using GetProcessIdFn = DWORD(WINAPI*)(HANDLE);
        using GetThreadIdFn = DWORD(WINAPI*)(HANDLE);
        using IsWow64ProcessFn = BOOL(WINAPI*)(HANDLE, PBOOL);
        using IsWow64Process2Fn = BOOL(WINAPI*)(HANDLE, USHORT*, USHORT*);
        using Wow64DisableWow64FsRedirectionFn = BOOL(WINAPI*)(PVOID*);
        using Wow64RevertWow64FsRedirectionFn = BOOL(WINAPI*)(PVOID);
        using GetTempPathWFn = DWORD(WINAPI*)(DWORD, LPWSTR);
        using GetTempPathAFn = DWORD(WINAPI*)(DWORD, LPSTR);
        using GetTempFileNameWFn = UINT(WINAPI*)(LPCWSTR, LPCWSTR, UINT, LPWSTR);
        using GetTempFileNameAFn = UINT(WINAPI*)(LPCSTR, LPCSTR, UINT, LPSTR);
        using GetFullPathNameWFn = DWORD(WINAPI*)(LPCWSTR, DWORD, LPWSTR, LPWSTR*);
        using GetFullPathNameAFn = DWORD(WINAPI*)(LPCSTR, DWORD, LPSTR, LPSTR*);
        using SearchPathWFn = DWORD(WINAPI*)(LPCWSTR, LPCWSTR, LPCWSTR, DWORD, LPWSTR, LPWSTR*);
        using SearchPathAFn = DWORD(WINAPI*)(LPCSTR, LPCSTR, LPCSTR, DWORD, LPSTR, LPSTR*);
        using GetShortPathNameWFn = DWORD(WINAPI*)(LPCWSTR, LPWSTR, DWORD);
        using GetShortPathNameAFn = DWORD(WINAPI*)(LPCSTR, LPSTR, DWORD);
        using GetLongPathNameWFn = DWORD(WINAPI*)(LPCWSTR, LPWSTR, DWORD);
        using GetLongPathNameAFn = DWORD(WINAPI*)(LPCSTR, LPSTR, DWORD);
        using CreatePipeFn = BOOL(WINAPI*)(PHANDLE, PHANDLE, LPSECURITY_ATTRIBUTES, DWORD);
        using CreateMailslotWFn = HANDLE(WINAPI*)(LPCWSTR, DWORD, DWORD, LPSECURITY_ATTRIBUTES);
        using CreateMailslotAFn = HANDLE(WINAPI*)(LPCSTR, DWORD, DWORD, LPSECURITY_ATTRIBUTES);
        using CreateDirectoryExWFn = BOOL(WINAPI*)(LPCWSTR, LPCWSTR, LPSECURITY_ATTRIBUTES);
        using CreateDirectoryExAFn = BOOL(WINAPI*)(LPCSTR, LPCSTR, LPSECURITY_ATTRIBUTES);
        using WSAStartupFn = int(WSAAPI*)(WORD, LPWSADATA);
        using WSACleanupFn = int(WSAAPI*)();
        using SelectFn = int(WSAAPI*)(int, fd_set*, fd_set*, fd_set*, const timeval*);
        using IoctlSocketFn = int(WSAAPI*)(SOCKET, long, u_long*);
        using SetSockOptFn = int(WSAAPI*)(SOCKET, int, int, const char*, int);
        using GetSockOptFn = int(WSAAPI*)(SOCKET, int, int, char*, int*);
        using GetSockNameFn = int(WSAAPI*)(SOCKET, sockaddr*, int*);
        using GetPeerNameFn = int(WSAAPI*)(SOCKET, sockaddr*, int*);
        using WSAEventSelectFn = int(WSAAPI*)(SOCKET, WSAEVENT, long);
        using WSAAsyncSelectFn = int(WSAAPI*)(SOCKET, HWND, unsigned int, long);
        using HostEntPtr = hostent*;
        using GetHostByNameFn = HostEntPtr(WSAAPI*)(const char*);
        using GetHostByAddrFn = HostEntPtr(WSAAPI*)(const char*, int, int);
        using GetNameInfoWFn = INT(WSAAPI*)(const sockaddr*, int, PWCHAR, DWORD, PWCHAR, DWORD, INT);
        using GetNameInfoAFn = INT(WSAAPI*)(const sockaddr*, int, PCHAR, DWORD, PCHAR, DWORD, INT);
        using WinHttpAddRequestHeadersFn = BOOL(WINAPI*)(HINTERNET, LPCWSTR, DWORD, DWORD);
        using WinHttpSetCredentialsFn = BOOL(WINAPI*)(HINTERNET, DWORD, DWORD, LPCWSTR, LPCWSTR, LPVOID);
        using WinHttpCrackUrlFn = BOOL(WINAPI*)(LPCWSTR, DWORD, DWORD, LPURL_COMPONENTS);
        using WinHttpCreateUrlFn = BOOL(WINAPI*)(LPURL_COMPONENTS, DWORD, LPWSTR, LPDWORD);
        using WinHttpSetTimeoutsFn = BOOL(WINAPI*)(HINTERNET, int, int, int, int);
        using HttpQueryInfoWFn = BOOL(WINAPI*)(HINTERNET, DWORD, LPVOID, LPDWORD, LPDWORD);
        using HttpQueryInfoAFn = BOOL(WINAPI*)(HINTERNET, DWORD, LPVOID, LPDWORD, LPDWORD);
        using InternetQueryOptionWFn = BOOL(WINAPI*)(HINTERNET, DWORD, LPVOID, LPDWORD);
        using InternetQueryOptionAFn = BOOL(WINAPI*)(HINTERNET, DWORD, LPVOID, LPDWORD);
        using NtOpenDirectoryObjectFn = NTSTATUS(NTAPI*)(PHANDLE, ACCESS_MASK, POBJECT_ATTRIBUTES);
        using NtQueryDirectoryObjectFn = NTSTATUS(NTAPI*)(HANDLE, PVOID, ULONG, BOOLEAN, BOOLEAN, PULONG, PULONG);
        using NtCreateSymbolicLinkObjectFn = NTSTATUS(NTAPI*)(PHANDLE, ACCESS_MASK, POBJECT_ATTRIBUTES, PUNICODE_STRING);
        using NtOpenSymbolicLinkObjectFn = NTSTATUS(NTAPI*)(PHANDLE, ACCESS_MASK, POBJECT_ATTRIBUTES);
        using NtQuerySymbolicLinkObjectFn = NTSTATUS(NTAPI*)(HANDLE, PUNICODE_STRING, PULONG);
        using NtCreateSemaphoreFn = NTSTATUS(NTAPI*)(PHANDLE, ACCESS_MASK, POBJECT_ATTRIBUTES, LONG, LONG);
        using NtOpenSemaphoreFn = NTSTATUS(NTAPI*)(PHANDLE, ACCESS_MASK, POBJECT_ATTRIBUTES);
        using NtReleaseSemaphoreFn = NTSTATUS(NTAPI*)(HANDLE, LONG, PLONG);

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
        InlineHookRecord g_closeHandleHook{};
        InlineHookRecord g_duplicateHandleHook{};
        InlineHookRecord g_createFileMappingWHook{};
        InlineHookRecord g_createFileMappingAHook{};
        InlineHookRecord g_openFileMappingWHook{};
        InlineHookRecord g_openFileMappingAHook{};
        InlineHookRecord g_mapViewOfFileHook{};
        InlineHookRecord g_mapViewOfFileExHook{};
        InlineHookRecord g_unmapViewOfFileHook{};
        InlineHookRecord g_flushViewOfFileHook{};
        InlineHookRecord g_freeLibraryHook{};
        InlineHookRecord g_getProcAddressHook{};
        InlineHookRecord g_ldrGetProcedureAddressHook{};
        InlineHookRecord g_regCreateKeyTransactedWHook{};
        InlineHookRecord g_regCreateKeyTransactedAHook{};
        InlineHookRecord g_regOpenKeyTransactedWHook{};
        InlineHookRecord g_regOpenKeyTransactedAHook{};
        InlineHookRecord g_regDeleteKeyTransactedWHook{};
        InlineHookRecord g_regDeleteKeyTransactedAHook{};
        InlineHookRecord g_regReplaceKeyWHook{};
        InlineHookRecord g_regReplaceKeyAHook{};
        InlineHookRecord g_regRestoreKeyWHook{};
        InlineHookRecord g_regRestoreKeyAHook{};
        InlineHookRecord g_regUnLoadKeyWHook{};
        InlineHookRecord g_regUnLoadKeyAHook{};
        InlineHookRecord g_regLoadAppKeyWHook{};
        InlineHookRecord g_regLoadAppKeyAHook{};
        InlineHookRecord g_regNotifyChangeKeyValueHook{};
        InlineHookRecord g_ntCloseHook{};
        InlineHookRecord g_ntCreateKeyTransactedHook{};
        InlineHookRecord g_ntOpenKeyTransactedHook{};
        InlineHookRecord g_ntOpenKeyTransactedExHook{};
        InlineHookRecord g_ntReplaceKeyHook{};
        InlineHookRecord g_ntRestoreKeyHook{};
        InlineHookRecord g_ntUnloadKeyHook{};
        InlineHookRecord g_ntUnloadKey2Hook{};
        InlineHookRecord g_ntUnloadKeyExHook{};
        InlineHookRecord g_openProcessTokenHook{};
        InlineHookRecord g_openThreadTokenHook{};
        InlineHookRecord g_adjustTokenPrivilegesHook{};
        InlineHookRecord g_duplicateTokenHook{};
        InlineHookRecord g_duplicateTokenExHook{};
        InlineHookRecord g_createProcessAsUserWHook{};
        InlineHookRecord g_createProcessAsUserAHook{};
        InlineHookRecord g_createProcessWithTokenWHook{};
        InlineHookRecord g_lookupPrivilegeValueWHook{};
        InlineHookRecord g_lookupPrivilegeValueAHook{};
        InlineHookRecord g_openSCManagerWHook{};
        InlineHookRecord g_openSCManagerAHook{};
        InlineHookRecord g_openServiceWHook{};
        InlineHookRecord g_openServiceAHook{};
        InlineHookRecord g_createServiceWHook{};
        InlineHookRecord g_createServiceAHook{};
        InlineHookRecord g_changeServiceConfigWHook{};
        InlineHookRecord g_changeServiceConfigAHook{};
        InlineHookRecord g_startServiceWHook{};
        InlineHookRecord g_startServiceAHook{};
        InlineHookRecord g_controlServiceHook{};
        InlineHookRecord g_deleteServiceHook{};
        InlineHookRecord g_closeServiceHandleHook{};
        InlineHookRecord g_wsaIoctlHook{};
        InlineHookRecord g_wsaSendToHook{};
        InlineHookRecord g_wsaRecvFromHook{};
        InlineHookRecord g_getAddrInfoWHook{};
        InlineHookRecord g_getAddrInfoAHook{};
        InlineHookRecord g_dnsQueryWHook{};
        InlineHookRecord g_dnsQueryAHook{};
        InlineHookRecord g_winHttpOpenHook{};
        InlineHookRecord g_winHttpConnectHook{};
        InlineHookRecord g_winHttpOpenRequestHook{};
        InlineHookRecord g_winHttpSendRequestHook{};
        InlineHookRecord g_winHttpReceiveResponseHook{};
        InlineHookRecord g_winHttpReadDataHook{};
        InlineHookRecord g_winHttpWriteDataHook{};
        InlineHookRecord g_winHttpCloseHandleHook{};
        InlineHookRecord g_internetOpenWHook{};
        InlineHookRecord g_internetOpenAHook{};
        InlineHookRecord g_internetConnectWHook{};
        InlineHookRecord g_internetConnectAHook{};
        InlineHookRecord g_httpOpenRequestWHook{};
        InlineHookRecord g_httpOpenRequestAHook{};
        InlineHookRecord g_httpSendRequestWHook{};
        InlineHookRecord g_httpSendRequestAHook{};
        InlineHookRecord g_internetReadFileHook{};
        InlineHookRecord g_internetWriteFileHook{};
        InlineHookRecord g_internetCloseHandleHook{};
        InlineHookRecord g_cryptAcquireContextWHook{};
        InlineHookRecord g_cryptAcquireContextAHook{};
        InlineHookRecord g_cryptCreateHashHook{};
        InlineHookRecord g_cryptHashDataHook{};
        InlineHookRecord g_cryptDeriveKeyHook{};
        InlineHookRecord g_cryptEncryptHook{};
        InlineHookRecord g_cryptDecryptHook{};
        InlineHookRecord g_cryptGenRandomHook{};
        InlineHookRecord g_cryptReleaseContextHook{};
        InlineHookRecord g_bCryptOpenAlgorithmProviderHook{};
        InlineHookRecord g_bCryptCreateHashHook{};
        InlineHookRecord g_bCryptHashDataHook{};
        InlineHookRecord g_bCryptFinishHashHook{};
        InlineHookRecord g_bCryptEncryptHook{};
        InlineHookRecord g_bCryptDecryptHook{};
        InlineHookRecord g_bCryptGenRandomHook{};
        InlineHookRecord g_bCryptCloseAlgorithmProviderHook{};
        InlineHookRecord g_bCryptDestroyHashHook{};
        InlineHookRecord g_coCreateInstanceHook{};
        InlineHookRecord g_coCreateInstanceExHook{};
        InlineHookRecord g_coGetClassObjectHook{};
        InlineHookRecord g_virtualAllocHook{};
        InlineHookRecord g_virtualFreeHook{};
        InlineHookRecord g_virtualProtectHook{};
        InlineHookRecord g_createToolhelp32SnapshotHook{};
        InlineHookRecord g_module32FirstWHook{};
        InlineHookRecord g_module32NextWHook{};
        InlineHookRecord g_module32FirstAHook{};
        InlineHookRecord g_module32NextAHook{};
        InlineHookRecord g_getModuleHandleWHook{};
        InlineHookRecord g_getModuleHandleAHook{};
        InlineHookRecord g_getModuleHandleExWHook{};
        InlineHookRecord g_getModuleHandleExAHook{};
        InlineHookRecord g_getModuleFileNameWHook{};
        InlineHookRecord g_getModuleFileNameAHook{};
        InlineHookRecord g_createHardLinkWHook{};
        InlineHookRecord g_createHardLinkAHook{};
        InlineHookRecord g_replaceFileWHook{};
        InlineHookRecord g_replaceFileAHook{};
        InlineHookRecord g_setEndOfFileHook{};
        InlineHookRecord g_lockFileExHook{};
        InlineHookRecord g_unlockFileExHook{};
        InlineHookRecord g_shellExecuteWHook{};
        InlineHookRecord g_shellExecuteAHook{};
        InlineHookRecord g_createProcessWithLogonWHook{};
        InlineHookRecord g_changeServiceConfig2WHook{};
        InlineHookRecord g_changeServiceConfig2AHook{};
        InlineHookRecord g_queryServiceStatusExHook{};
        InlineHookRecord g_queryServiceConfigWHook{};
        InlineHookRecord g_queryServiceConfigAHook{};
        InlineHookRecord g_enumServicesStatusExWHook{};
        InlineHookRecord g_enumServicesStatusExAHook{};
        InlineHookRecord g_winHttpQueryHeadersHook{};
        InlineHookRecord g_winHttpQueryDataAvailableHook{};
        InlineHookRecord g_winHttpSetOptionHook{};
        InlineHookRecord g_internetOpenUrlWHook{};
        InlineHookRecord g_internetOpenUrlAHook{};
        InlineHookRecord g_internetQueryDataAvailableHook{};
        InlineHookRecord g_internetSetOptionWHook{};
        InlineHookRecord g_internetSetOptionAHook{};
        InlineHookRecord g_internetCrackUrlWHook{};
        InlineHookRecord g_internetCrackUrlAHook{};
        InlineHookRecord g_urlDownloadToFileWHook{};
        InlineHookRecord g_urlDownloadToFileAHook{};
        InlineHookRecord g_cryptImportKeyHook{};
        InlineHookRecord g_cryptExportKeyHook{};
        InlineHookRecord g_cryptDestroyKeyHook{};
        InlineHookRecord g_cryptDestroyHashHook{};
        InlineHookRecord g_bCryptGenerateSymmetricKeyHook{};
        InlineHookRecord g_bCryptImportKeyHook{};
        InlineHookRecord g_bCryptImportKeyPairHook{};
        InlineHookRecord g_bCryptDestroyKeyHook{};
        InlineHookRecord g_coInitializeExHook{};
        InlineHookRecord g_coInitializeSecurityHook{};
        InlineHookRecord g_coUninitializeHook{};
        InlineHookRecord g_createRemoteThreadExHook{};
        InlineHookRecord g_createSymbolicLinkWHook{};
        InlineHookRecord g_createSymbolicLinkAHook{};
        InlineHookRecord g_getFinalPathNameByHandleWHook{};
        InlineHookRecord g_getFinalPathNameByHandleAHook{};
        InlineHookRecord g_getFileSizeExHook{};
        InlineHookRecord g_setFilePointerExHook{};
        InlineHookRecord g_createNamedPipeWHook{};
        InlineHookRecord g_createNamedPipeAHook{};
        InlineHookRecord g_connectNamedPipeHook{};
        InlineHookRecord g_disconnectNamedPipeHook{};
        InlineHookRecord g_waitNamedPipeWHook{};
        InlineHookRecord g_waitNamedPipeAHook{};
        InlineHookRecord g_transactNamedPipeHook{};
        InlineHookRecord g_createMutexWHook{};
        InlineHookRecord g_createMutexAHook{};
        InlineHookRecord g_openMutexWHook{};
        InlineHookRecord g_openMutexAHook{};
        InlineHookRecord g_createEventWHook{};
        InlineHookRecord g_createEventAHook{};
        InlineHookRecord g_openEventWHook{};
        InlineHookRecord g_openEventAHook{};
        InlineHookRecord g_createSemaphoreWHook{};
        InlineHookRecord g_createSemaphoreAHook{};
        InlineHookRecord g_openSemaphoreWHook{};
        InlineHookRecord g_openSemaphoreAHook{};
        InlineHookRecord g_waitForSingleObjectHook{};
        InlineHookRecord g_waitForMultipleObjectsHook{};
        InlineHookRecord g_setEventHook{};
        InlineHookRecord g_resetEventHook{};
        InlineHookRecord g_releaseMutexHook{};
        InlineHookRecord g_releaseSemaphoreHook{};
        InlineHookRecord g_getEnvironmentVariableWHook{};
        InlineHookRecord g_getEnvironmentVariableAHook{};
        InlineHookRecord g_setEnvironmentVariableWHook{};
        InlineHookRecord g_setEnvironmentVariableAHook{};
        InlineHookRecord g_expandEnvironmentStringsWHook{};
        InlineHookRecord g_expandEnvironmentStringsAHook{};
        InlineHookRecord g_setDllDirectoryWHook{};
        InlineHookRecord g_setDllDirectoryAHook{};
        InlineHookRecord g_setDefaultDllDirectoriesHook{};
        InlineHookRecord g_addDllDirectoryHook{};
        InlineHookRecord g_removeDllDirectoryHook{};
        InlineHookRecord g_impersonateLoggedOnUserHook{};
        InlineHookRecord g_revertToSelfHook{};
        InlineHookRecord g_setThreadTokenHook{};
        InlineHookRecord g_setWindowsHookExWHook{};
        InlineHookRecord g_setWindowsHookExAHook{};
        InlineHookRecord g_unhookWindowsHookExHook{};
        InlineHookRecord g_enumProcessesHook{};
        InlineHookRecord g_enumProcessModulesHook{};
        InlineHookRecord g_enumProcessModulesExHook{};
        InlineHookRecord g_getMappedFileNameWHook{};
        InlineHookRecord g_getMappedFileNameAHook{};
        InlineHookRecord g_enumWindowsHook{};
        InlineHookRecord g_enumChildWindowsHook{};
        InlineHookRecord g_findWindowWHook{};
        InlineHookRecord g_findWindowAHook{};
        InlineHookRecord g_findWindowExWHook{};
        InlineHookRecord g_findWindowExAHook{};
        InlineHookRecord g_getWindowThreadProcessIdHook{};
        InlineHookRecord g_getForegroundWindowHook{};
        InlineHookRecord g_getDCHook{};
        InlineHookRecord g_releaseDCHook{};
        InlineHookRecord g_createCompatibleDCHook{};
        InlineHookRecord g_deleteDCHook{};
        InlineHookRecord g_createCompatibleBitmapHook{};
        InlineHookRecord g_bitBltHook{};
        InlineHookRecord g_stretchBltHook{};
        InlineHookRecord g_deleteObjectHook{};
        InlineHookRecord g_openClipboardHook{};
        InlineHookRecord g_closeClipboardHook{};
        InlineHookRecord g_getClipboardDataHook{};
        InlineHookRecord g_setClipboardDataHook{};
        InlineHookRecord g_emptyClipboardHook{};
        InlineHookRecord g_startTraceWHook{};
        InlineHookRecord g_startTraceAHook{};
        InlineHookRecord g_controlTraceWHook{};
        InlineHookRecord g_controlTraceAHook{};
        InlineHookRecord g_enableTraceEx2Hook{};
        InlineHookRecord g_openTraceWHook{};
        InlineHookRecord g_openTraceAHook{};
        InlineHookRecord g_processTraceHook{};
        InlineHookRecord g_closeTraceHook{};
        InlineHookRecord g_eventRegisterHook{};
        InlineHookRecord g_eventUnregisterHook{};
        InlineHookRecord g_eventWriteHook{};
        InlineHookRecord g_eventWriteExHook{};
        InlineHookRecord g_winVerifyTrustHook{};
        InlineHookRecord g_cryptQueryObjectHook{};
        InlineHookRecord g_certOpenStoreHook{};
        InlineHookRecord g_certCloseStoreHook{};
        InlineHookRecord g_certFindCertificateInStoreHook{};
        InlineHookRecord g_certGetCertificateChainHook{};
        InlineHookRecord g_certVerifyCertificateChainPolicyHook{};
        InlineHookRecord g_cryptProtectDataHook{};
        InlineHookRecord g_cryptUnprotectDataHook{};
        InlineHookRecord g_nCryptOpenStorageProviderHook{};
        InlineHookRecord g_nCryptOpenKeyHook{};
        InlineHookRecord g_nCryptCreatePersistedKeyHook{};
        InlineHookRecord g_nCryptFinalizeKeyHook{};
        InlineHookRecord g_nCryptEncryptHook{};
        InlineHookRecord g_nCryptDecryptHook{};
        InlineHookRecord g_nCryptSignHashHook{};
        InlineHookRecord g_nCryptVerifySignatureHook{};
        InlineHookRecord g_nCryptExportKeyHook{};
        InlineHookRecord g_nCryptImportKeyHook{};
        InlineHookRecord g_nCryptDeleteKeyHook{};
        InlineHookRecord g_nCryptFreeObjectHook{};
        InlineHookRecord g_rpcStringBindingComposeWHook{};
        InlineHookRecord g_rpcStringBindingComposeAHook{};
        InlineHookRecord g_rpcBindingFromStringBindingWHook{};
        InlineHookRecord g_rpcBindingFromStringBindingAHook{};
        InlineHookRecord g_rpcBindingFreeHook{};
        InlineHookRecord g_rpcMgmtEpEltInqBeginHook{};
        InlineHookRecord g_rpcMgmtEpEltInqNextWHook{};
        InlineHookRecord g_rpcMgmtEpEltInqNextAHook{};
        InlineHookRecord g_rpcMgmtEpEltInqDoneHook{};
        InlineHookRecord g_ntQueryInformationTokenHook{};
        InlineHookRecord g_ntSetInformationTokenHook{};
        InlineHookRecord g_ntAdjustPrivilegesTokenHook{};
        InlineHookRecord g_ntCreateMutantHook{};
        InlineHookRecord g_ntOpenMutantHook{};
        InlineHookRecord g_ntReleaseMutantHook{};
        InlineHookRecord g_ntCreateEventHook{};
        InlineHookRecord g_ntOpenEventHook{};
        InlineHookRecord g_ntSetEventHook{};
        InlineHookRecord g_ntResetEventHook{};
        InlineHookRecord g_ntWaitForSingleObjectHook{};
        InlineHookRecord g_ntWaitForMultipleObjectsHook{};
        InlineHookRecord g_ntQuerySystemInformationHook{};
        InlineHookRecord g_ntQueryObjectHook{};
        InlineHookRecord g_logonUserWHook{};
        InlineHookRecord g_logonUserAHook{};
        InlineHookRecord g_getTokenInformationHook{};
        InlineHookRecord g_setTokenInformationHook{};
        InlineHookRecord g_checkTokenMembershipHook{};
        InlineHookRecord g_createRestrictedTokenHook{};
        InlineHookRecord g_impersonateSelfHook{};
        InlineHookRecord g_impersonateNamedPipeClientHook{};
        InlineHookRecord g_credReadWHook{};
        InlineHookRecord g_credReadAHook{};
        InlineHookRecord g_credEnumerateWHook{};
        InlineHookRecord g_credEnumerateAHook{};
        InlineHookRecord g_credWriteWHook{};
        InlineHookRecord g_credWriteAHook{};
        InlineHookRecord g_credDeleteWHook{};
        InlineHookRecord g_credDeleteAHook{};
        InlineHookRecord g_credFreeHook{};
        InlineHookRecord g_lsaOpenPolicyHook{};
        InlineHookRecord g_lsaCloseHook{};
        InlineHookRecord g_lsaEnumerateLogonSessionsHook{};
        InlineHookRecord g_lsaGetLogonSessionDataHook{};
        InlineHookRecord g_lsaFreeReturnBufferHook{};
        InlineHookRecord g_lsaLookupNames2Hook{};
        InlineHookRecord g_lsaLookupSids2Hook{};
        InlineHookRecord g_openEventLogWHook{};
        InlineHookRecord g_openEventLogAHook{};
        InlineHookRecord g_registerEventSourceWHook{};
        InlineHookRecord g_registerEventSourceAHook{};
        InlineHookRecord g_readEventLogWHook{};
        InlineHookRecord g_readEventLogAHook{};
        InlineHookRecord g_clearEventLogWHook{};
        InlineHookRecord g_clearEventLogAHook{};
        InlineHookRecord g_reportEventWHook{};
        InlineHookRecord g_reportEventAHook{};
        InlineHookRecord g_closeEventLogHook{};
        InlineHookRecord g_netUserEnumHook{};
        InlineHookRecord g_netLocalGroupEnumHook{};
        InlineHookRecord g_netGroupEnumHook{};
        InlineHookRecord g_netShareEnumHook{};
        InlineHookRecord g_netSessionEnumHook{};
        InlineHookRecord g_netServerEnumHook{};
        InlineHookRecord g_netWkstaGetInfoHook{};
        InlineHookRecord g_netApiBufferFreeHook{};
        InlineHookRecord g_getExtendedTcpTableHook{};
        InlineHookRecord g_getExtendedUdpTableHook{};
        InlineHookRecord g_getTcpTable2Hook{};
        InlineHookRecord g_getUdpTableHook{};
        InlineHookRecord g_getAdaptersAddressesHook{};
        InlineHookRecord g_getNetworkParamsHook{};
        InlineHookRecord g_getIpNetTable2Hook{};
        InlineHookRecord g_getIfTable2Hook{};
        InlineHookRecord g_freeMibTableHook{};
        InlineHookRecord g_wtsOpenServerWHook{};
        InlineHookRecord g_wtsOpenServerAHook{};
        InlineHookRecord g_wtsCloseServerHook{};
        InlineHookRecord g_wtsEnumerateSessionsWHook{};
        InlineHookRecord g_wtsEnumerateSessionsAHook{};
        InlineHookRecord g_wtsEnumerateProcessesWHook{};
        InlineHookRecord g_wtsEnumerateProcessesAHook{};
        InlineHookRecord g_wtsQuerySessionInformationWHook{};
        InlineHookRecord g_wtsQuerySessionInformationAHook{};
        InlineHookRecord g_wtsFreeMemoryHook{};
        InlineHookRecord g_createJobObjectWHook{};
        InlineHookRecord g_createJobObjectAHook{};
        InlineHookRecord g_openJobObjectWHook{};
        InlineHookRecord g_openJobObjectAHook{};
        InlineHookRecord g_assignProcessToJobObjectHook{};
        InlineHookRecord g_terminateJobObjectHook{};
        InlineHookRecord g_setInformationJobObjectHook{};
        InlineHookRecord g_queryInformationJobObjectHook{};
        InlineHookRecord g_acquireCredentialsHandleWHook{};
        InlineHookRecord g_acquireCredentialsHandleAHook{};
        InlineHookRecord g_initializeSecurityContextWHook{};
        InlineHookRecord g_initializeSecurityContextAHook{};
        InlineHookRecord g_acceptSecurityContextHook{};
        InlineHookRecord g_encryptMessageHook{};
        InlineHookRecord g_decryptMessageHook{};
        InlineHookRecord g_deleteSecurityContextHook{};
        InlineHookRecord g_freeCredentialsHandleHook{};
        InlineHookRecord g_process32FirstWHook{};
        InlineHookRecord g_process32FirstAHook{};
        InlineHookRecord g_process32NextWHook{};
        InlineHookRecord g_process32NextAHook{};
        InlineHookRecord g_thread32FirstHook{};
        InlineHookRecord g_thread32NextHook{};
        InlineHookRecord g_heap32ListFirstHook{};
        InlineHookRecord g_heap32ListNextHook{};
        InlineHookRecord g_heap32FirstHook{};
        InlineHookRecord g_heap32NextHook{};
        InlineHookRecord g_queryFullProcessImageNameWHook{};
        InlineHookRecord g_queryFullProcessImageNameAHook{};
        InlineHookRecord g_getProcessImageFileNameWHook{};
        InlineHookRecord g_getProcessImageFileNameAHook{};
        InlineHookRecord g_getProcessIdHook{};
        InlineHookRecord g_getThreadIdHook{};
        InlineHookRecord g_isWow64ProcessHook{};
        InlineHookRecord g_isWow64Process2Hook{};
        InlineHookRecord g_wow64DisableWow64FsRedirectionHook{};
        InlineHookRecord g_wow64RevertWow64FsRedirectionHook{};
        InlineHookRecord g_getTempPathWHook{};
        InlineHookRecord g_getTempPathAHook{};
        InlineHookRecord g_getTempFileNameWHook{};
        InlineHookRecord g_getTempFileNameAHook{};
        InlineHookRecord g_getFullPathNameWHook{};
        InlineHookRecord g_getFullPathNameAHook{};
        InlineHookRecord g_searchPathWHook{};
        InlineHookRecord g_searchPathAHook{};
        InlineHookRecord g_getShortPathNameWHook{};
        InlineHookRecord g_getShortPathNameAHook{};
        InlineHookRecord g_getLongPathNameWHook{};
        InlineHookRecord g_getLongPathNameAHook{};
        InlineHookRecord g_createPipeHook{};
        InlineHookRecord g_createMailslotWHook{};
        InlineHookRecord g_createMailslotAHook{};
        InlineHookRecord g_createDirectoryExWHook{};
        InlineHookRecord g_createDirectoryExAHook{};
        InlineHookRecord g_wsaStartupHook{};
        InlineHookRecord g_wsaCleanupHook{};
        InlineHookRecord g_selectHook{};
        InlineHookRecord g_ioctlSocketHook{};
        InlineHookRecord g_setSockOptHook{};
        InlineHookRecord g_getSockOptHook{};
        InlineHookRecord g_getSockNameHook{};
        InlineHookRecord g_getPeerNameHook{};
        InlineHookRecord g_wsaEventSelectHook{};
        InlineHookRecord g_wsaAsyncSelectHook{};
        InlineHookRecord g_getHostByNameHook{};
        InlineHookRecord g_getHostByAddrHook{};
        InlineHookRecord g_getNameInfoWHook{};
        InlineHookRecord g_getNameInfoAHook{};
        InlineHookRecord g_winHttpAddRequestHeadersHook{};
        InlineHookRecord g_winHttpSetCredentialsHook{};
        InlineHookRecord g_winHttpCrackUrlHook{};
        InlineHookRecord g_winHttpCreateUrlHook{};
        InlineHookRecord g_winHttpSetTimeoutsHook{};
        InlineHookRecord g_httpQueryInfoWHook{};
        InlineHookRecord g_httpQueryInfoAHook{};
        InlineHookRecord g_internetQueryOptionWHook{};
        InlineHookRecord g_internetQueryOptionAHook{};
        InlineHookRecord g_ntOpenDirectoryObjectHook{};
        InlineHookRecord g_ntQueryDirectoryObjectHook{};
        InlineHookRecord g_ntCreateSymbolicLinkObjectHook{};
        InlineHookRecord g_ntOpenSymbolicLinkObjectHook{};
        InlineHookRecord g_ntQuerySymbolicLinkObjectHook{};
        InlineHookRecord g_ntCreateSemaphoreHook{};
        InlineHookRecord g_ntOpenSemaphoreHook{};
        InlineHookRecord g_ntReleaseSemaphoreHook{};
        std::mutex g_hookOperationMutex;

        const wchar_t* FsctlCodeToText(const ULONG fsControlCode)
        {
            switch (fsControlCode)
            {
            case FSCTL_REQUEST_OPLOCK:
                return L"FSCTL_REQUEST_OPLOCK";
            case FSCTL_REQUEST_BATCH_OPLOCK:
                return L"FSCTL_REQUEST_BATCH_OPLOCK";
            case FSCTL_REQUEST_FILTER_OPLOCK:
                return L"FSCTL_REQUEST_FILTER_OPLOCK";
            case FSCTL_OPLOCK_BREAK_ACKNOWLEDGE:
                return L"FSCTL_OPLOCK_BREAK_ACKNOWLEDGE";
            case FSCTL_OPBATCH_ACK_CLOSE_PENDING:
                return L"FSCTL_OPBATCH_ACK_CLOSE_PENDING";
            case FSCTL_OPLOCK_BREAK_NOTIFY:
                return L"FSCTL_OPLOCK_BREAK_NOTIFY";
            case FSCTL_REQUEST_OPLOCK_LEVEL_1:
                return L"FSCTL_REQUEST_OPLOCK_LEVEL_1";
            case FSCTL_REQUEST_OPLOCK_LEVEL_2:
                return L"FSCTL_REQUEST_OPLOCK_LEVEL_2";
            default:
                return L"UNKNOWN_FSCTL";
            }
        }

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
        CloseHandleFn g_closeHandleOriginal = nullptr;
        DuplicateHandleFn g_duplicateHandleOriginal = nullptr;
        CreateFileMappingWFn g_createFileMappingWOriginal = nullptr;
        CreateFileMappingAFn g_createFileMappingAOriginal = nullptr;
        OpenFileMappingWFn g_openFileMappingWOriginal = nullptr;
        OpenFileMappingAFn g_openFileMappingAOriginal = nullptr;
        MapViewOfFileFn g_mapViewOfFileOriginal = nullptr;
        MapViewOfFileExFn g_mapViewOfFileExOriginal = nullptr;
        UnmapViewOfFileFn g_unmapViewOfFileOriginal = nullptr;
        FlushViewOfFileFn g_flushViewOfFileOriginal = nullptr;
        FreeLibraryFn g_freeLibraryOriginal = nullptr;
        GetProcAddressFn g_getProcAddressOriginal = nullptr;
        LdrGetProcedureAddressFn g_ldrGetProcedureAddressOriginal = nullptr;
        RegCreateKeyTransactedWFn g_regCreateKeyTransactedWOriginal = nullptr;
        RegCreateKeyTransactedAFn g_regCreateKeyTransactedAOriginal = nullptr;
        RegOpenKeyTransactedWFn g_regOpenKeyTransactedWOriginal = nullptr;
        RegOpenKeyTransactedAFn g_regOpenKeyTransactedAOriginal = nullptr;
        RegDeleteKeyTransactedWFn g_regDeleteKeyTransactedWOriginal = nullptr;
        RegDeleteKeyTransactedAFn g_regDeleteKeyTransactedAOriginal = nullptr;
        RegReplaceKeyWFn g_regReplaceKeyWOriginal = nullptr;
        RegReplaceKeyAFn g_regReplaceKeyAOriginal = nullptr;
        RegRestoreKeyWFn g_regRestoreKeyWOriginal = nullptr;
        RegRestoreKeyAFn g_regRestoreKeyAOriginal = nullptr;
        RegUnLoadKeyWFn g_regUnLoadKeyWOriginal = nullptr;
        RegUnLoadKeyAFn g_regUnLoadKeyAOriginal = nullptr;
        RegLoadAppKeyWFn g_regLoadAppKeyWOriginal = nullptr;
        RegLoadAppKeyAFn g_regLoadAppKeyAOriginal = nullptr;
        RegNotifyChangeKeyValueFn g_regNotifyChangeKeyValueOriginal = nullptr;
        NtCloseFn g_ntCloseOriginal = nullptr;
        NtCreateKeyTransactedFn g_ntCreateKeyTransactedOriginal = nullptr;
        NtOpenKeyTransactedFn g_ntOpenKeyTransactedOriginal = nullptr;
        NtOpenKeyTransactedExFn g_ntOpenKeyTransactedExOriginal = nullptr;
        NtReplaceKeyFn g_ntReplaceKeyOriginal = nullptr;
        NtRestoreKeyFn g_ntRestoreKeyOriginal = nullptr;
        NtUnloadKeyFn g_ntUnloadKeyOriginal = nullptr;
        NtUnloadKey2Fn g_ntUnloadKey2Original = nullptr;
        NtUnloadKeyExFn g_ntUnloadKeyExOriginal = nullptr;
        OpenProcessTokenFn g_openProcessTokenOriginal = nullptr;
        OpenThreadTokenFn g_openThreadTokenOriginal = nullptr;
        AdjustTokenPrivilegesFn g_adjustTokenPrivilegesOriginal = nullptr;
        DuplicateTokenFn g_duplicateTokenOriginal = nullptr;
        DuplicateTokenExFn g_duplicateTokenExOriginal = nullptr;
        CreateProcessAsUserWFn g_createProcessAsUserWOriginal = nullptr;
        CreateProcessAsUserAFn g_createProcessAsUserAOriginal = nullptr;
        CreateProcessWithTokenWFn g_createProcessWithTokenWOriginal = nullptr;
        LookupPrivilegeValueWFn g_lookupPrivilegeValueWOriginal = nullptr;
        LookupPrivilegeValueAFn g_lookupPrivilegeValueAOriginal = nullptr;
        OpenSCManagerWFn g_openSCManagerWOriginal = nullptr;
        OpenSCManagerAFn g_openSCManagerAOriginal = nullptr;
        OpenServiceWFn g_openServiceWOriginal = nullptr;
        OpenServiceAFn g_openServiceAOriginal = nullptr;
        CreateServiceWFn g_createServiceWOriginal = nullptr;
        CreateServiceAFn g_createServiceAOriginal = nullptr;
        ChangeServiceConfigWFn g_changeServiceConfigWOriginal = nullptr;
        ChangeServiceConfigAFn g_changeServiceConfigAOriginal = nullptr;
        StartServiceWFn g_startServiceWOriginal = nullptr;
        StartServiceAFn g_startServiceAOriginal = nullptr;
        ControlServiceFn g_controlServiceOriginal = nullptr;
        DeleteServiceFn g_deleteServiceOriginal = nullptr;
        CloseServiceHandleFn g_closeServiceHandleOriginal = nullptr;
        WSAIoctlFn g_wsaIoctlOriginal = nullptr;
        WSASendToFn g_wsaSendToOriginal = nullptr;
        WSARecvFromFn g_wsaRecvFromOriginal = nullptr;
        GetAddrInfoWFn g_getAddrInfoWOriginal = nullptr;
        GetAddrInfoAFn g_getAddrInfoAOriginal = nullptr;
        DnsQueryWFn g_dnsQueryWOriginal = nullptr;
        DnsQueryAFn g_dnsQueryAOriginal = nullptr;
        WinHttpOpenFn g_winHttpOpenOriginal = nullptr;
        WinHttpConnectFn g_winHttpConnectOriginal = nullptr;
        WinHttpOpenRequestFn g_winHttpOpenRequestOriginal = nullptr;
        WinHttpSendRequestFn g_winHttpSendRequestOriginal = nullptr;
        WinHttpReceiveResponseFn g_winHttpReceiveResponseOriginal = nullptr;
        WinHttpReadDataFn g_winHttpReadDataOriginal = nullptr;
        WinHttpWriteDataFn g_winHttpWriteDataOriginal = nullptr;
        WinHttpCloseHandleFn g_winHttpCloseHandleOriginal = nullptr;
        InternetOpenWFn g_internetOpenWOriginal = nullptr;
        InternetOpenAFn g_internetOpenAOriginal = nullptr;
        InternetConnectWFn g_internetConnectWOriginal = nullptr;
        InternetConnectAFn g_internetConnectAOriginal = nullptr;
        HttpOpenRequestWFn g_httpOpenRequestWOriginal = nullptr;
        HttpOpenRequestAFn g_httpOpenRequestAOriginal = nullptr;
        HttpSendRequestWFn g_httpSendRequestWOriginal = nullptr;
        HttpSendRequestAFn g_httpSendRequestAOriginal = nullptr;
        InternetReadFileFn g_internetReadFileOriginal = nullptr;
        InternetWriteFileFn g_internetWriteFileOriginal = nullptr;
        InternetCloseHandleFn g_internetCloseHandleOriginal = nullptr;
        CryptAcquireContextWFn g_cryptAcquireContextWOriginal = nullptr;
        CryptAcquireContextAFn g_cryptAcquireContextAOriginal = nullptr;
        CryptCreateHashFn g_cryptCreateHashOriginal = nullptr;
        CryptHashDataFn g_cryptHashDataOriginal = nullptr;
        CryptDeriveKeyFn g_cryptDeriveKeyOriginal = nullptr;
        CryptEncryptFn g_cryptEncryptOriginal = nullptr;
        CryptDecryptFn g_cryptDecryptOriginal = nullptr;
        CryptGenRandomFn g_cryptGenRandomOriginal = nullptr;
        CryptReleaseContextFn g_cryptReleaseContextOriginal = nullptr;
        BCryptOpenAlgorithmProviderFn g_bCryptOpenAlgorithmProviderOriginal = nullptr;
        BCryptCreateHashFn g_bCryptCreateHashOriginal = nullptr;
        BCryptHashDataFn g_bCryptHashDataOriginal = nullptr;
        BCryptFinishHashFn g_bCryptFinishHashOriginal = nullptr;
        BCryptEncryptFn g_bCryptEncryptOriginal = nullptr;
        BCryptDecryptFn g_bCryptDecryptOriginal = nullptr;
        BCryptGenRandomFn g_bCryptGenRandomOriginal = nullptr;
        BCryptCloseAlgorithmProviderFn g_bCryptCloseAlgorithmProviderOriginal = nullptr;
        BCryptDestroyHashFn g_bCryptDestroyHashOriginal = nullptr;
        CoCreateInstanceFn g_coCreateInstanceOriginal = nullptr;
        CoCreateInstanceExFn g_coCreateInstanceExOriginal = nullptr;
        CoGetClassObjectFn g_coGetClassObjectOriginal = nullptr;
        VirtualAllocFn g_virtualAllocOriginal = nullptr;
        VirtualFreeFn g_virtualFreeOriginal = nullptr;
        VirtualProtectFn g_virtualProtectOriginal = nullptr;
        CreateToolhelp32SnapshotFn g_createToolhelp32SnapshotOriginal = nullptr;
        Module32FirstWFn g_module32FirstWOriginal = nullptr;
        Module32NextWFn g_module32NextWOriginal = nullptr;
        Module32FirstAFn g_module32FirstAOriginal = nullptr;
        Module32NextAFn g_module32NextAOriginal = nullptr;
        GetModuleHandleWFn g_getModuleHandleWOriginal = nullptr;
        GetModuleHandleAFn g_getModuleHandleAOriginal = nullptr;
        GetModuleHandleExWFn g_getModuleHandleExWOriginal = nullptr;
        GetModuleHandleExAFn g_getModuleHandleExAOriginal = nullptr;
        GetModuleFileNameWFn g_getModuleFileNameWOriginal = nullptr;
        GetModuleFileNameAFn g_getModuleFileNameAOriginal = nullptr;
        CreateHardLinkWFn g_createHardLinkWOriginal = nullptr;
        CreateHardLinkAFn g_createHardLinkAOriginal = nullptr;
        ReplaceFileWFn g_replaceFileWOriginal = nullptr;
        ReplaceFileAFn g_replaceFileAOriginal = nullptr;
        SetEndOfFileFn g_setEndOfFileOriginal = nullptr;
        LockFileExFn g_lockFileExOriginal = nullptr;
        UnlockFileExFn g_unlockFileExOriginal = nullptr;
        ShellExecuteWFn g_shellExecuteWOriginal = nullptr;
        ShellExecuteAFn g_shellExecuteAOriginal = nullptr;
        CreateProcessWithLogonWFn g_createProcessWithLogonWOriginal = nullptr;
        ChangeServiceConfig2WFn g_changeServiceConfig2WOriginal = nullptr;
        ChangeServiceConfig2AFn g_changeServiceConfig2AOriginal = nullptr;
        QueryServiceStatusExFn g_queryServiceStatusExOriginal = nullptr;
        QueryServiceConfigWFn g_queryServiceConfigWOriginal = nullptr;
        QueryServiceConfigAFn g_queryServiceConfigAOriginal = nullptr;
        EnumServicesStatusExWFn g_enumServicesStatusExWOriginal = nullptr;
        EnumServicesStatusExAFn g_enumServicesStatusExAOriginal = nullptr;
        WinHttpQueryHeadersFn g_winHttpQueryHeadersOriginal = nullptr;
        WinHttpQueryDataAvailableFn g_winHttpQueryDataAvailableOriginal = nullptr;
        WinHttpSetOptionFn g_winHttpSetOptionOriginal = nullptr;
        InternetOpenUrlWFn g_internetOpenUrlWOriginal = nullptr;
        InternetOpenUrlAFn g_internetOpenUrlAOriginal = nullptr;
        InternetQueryDataAvailableFn g_internetQueryDataAvailableOriginal = nullptr;
        InternetSetOptionWFn g_internetSetOptionWOriginal = nullptr;
        InternetSetOptionAFn g_internetSetOptionAOriginal = nullptr;
        InternetCrackUrlWFn g_internetCrackUrlWOriginal = nullptr;
        InternetCrackUrlAFn g_internetCrackUrlAOriginal = nullptr;
        URLDownloadToFileWFn g_urlDownloadToFileWOriginal = nullptr;
        URLDownloadToFileAFn g_urlDownloadToFileAOriginal = nullptr;
        CryptImportKeyFn g_cryptImportKeyOriginal = nullptr;
        CryptExportKeyFn g_cryptExportKeyOriginal = nullptr;
        CryptDestroyKeyFn g_cryptDestroyKeyOriginal = nullptr;
        CryptDestroyHashFn g_cryptDestroyHashOriginal = nullptr;
        BCryptGenerateSymmetricKeyFn g_bCryptGenerateSymmetricKeyOriginal = nullptr;
        BCryptImportKeyFn g_bCryptImportKeyOriginal = nullptr;
        BCryptImportKeyPairFn g_bCryptImportKeyPairOriginal = nullptr;
        BCryptDestroyKeyFn g_bCryptDestroyKeyOriginal = nullptr;
        CoInitializeExFn g_coInitializeExOriginal = nullptr;
        CoInitializeSecurityFn g_coInitializeSecurityOriginal = nullptr;
        CoUninitializeFn g_coUninitializeOriginal = nullptr;
        CreateRemoteThreadExFn g_createRemoteThreadExOriginal = nullptr;
        CreateSymbolicLinkWFn g_createSymbolicLinkWOriginal = nullptr;
        CreateSymbolicLinkAFn g_createSymbolicLinkAOriginal = nullptr;
        GetFinalPathNameByHandleWFn g_getFinalPathNameByHandleWOriginal = nullptr;
        GetFinalPathNameByHandleAFn g_getFinalPathNameByHandleAOriginal = nullptr;
        GetFileSizeExFn g_getFileSizeExOriginal = nullptr;
        SetFilePointerExFn g_setFilePointerExOriginal = nullptr;
        CreateNamedPipeWFn g_createNamedPipeWOriginal = nullptr;
        CreateNamedPipeAFn g_createNamedPipeAOriginal = nullptr;
        ConnectNamedPipeFn g_connectNamedPipeOriginal = nullptr;
        DisconnectNamedPipeFn g_disconnectNamedPipeOriginal = nullptr;
        WaitNamedPipeWFn g_waitNamedPipeWOriginal = nullptr;
        WaitNamedPipeAFn g_waitNamedPipeAOriginal = nullptr;
        TransactNamedPipeFn g_transactNamedPipeOriginal = nullptr;
        CreateMutexWFn g_createMutexWOriginal = nullptr;
        CreateMutexAFn g_createMutexAOriginal = nullptr;
        OpenMutexWFn g_openMutexWOriginal = nullptr;
        OpenMutexAFn g_openMutexAOriginal = nullptr;
        CreateEventWFn g_createEventWOriginal = nullptr;
        CreateEventAFn g_createEventAOriginal = nullptr;
        OpenEventWFn g_openEventWOriginal = nullptr;
        OpenEventAFn g_openEventAOriginal = nullptr;
        CreateSemaphoreWFn g_createSemaphoreWOriginal = nullptr;
        CreateSemaphoreAFn g_createSemaphoreAOriginal = nullptr;
        OpenSemaphoreWFn g_openSemaphoreWOriginal = nullptr;
        OpenSemaphoreAFn g_openSemaphoreAOriginal = nullptr;
        WaitForSingleObjectFn g_waitForSingleObjectOriginal = nullptr;
        WaitForMultipleObjectsFn g_waitForMultipleObjectsOriginal = nullptr;
        SetEventFn g_setEventOriginal = nullptr;
        ResetEventFn g_resetEventOriginal = nullptr;
        ReleaseMutexFn g_releaseMutexOriginal = nullptr;
        ReleaseSemaphoreFn g_releaseSemaphoreOriginal = nullptr;
        GetEnvironmentVariableWFn g_getEnvironmentVariableWOriginal = nullptr;
        GetEnvironmentVariableAFn g_getEnvironmentVariableAOriginal = nullptr;
        SetEnvironmentVariableWFn g_setEnvironmentVariableWOriginal = nullptr;
        SetEnvironmentVariableAFn g_setEnvironmentVariableAOriginal = nullptr;
        ExpandEnvironmentStringsWFn g_expandEnvironmentStringsWOriginal = nullptr;
        ExpandEnvironmentStringsAFn g_expandEnvironmentStringsAOriginal = nullptr;
        SetDllDirectoryWFn g_setDllDirectoryWOriginal = nullptr;
        SetDllDirectoryAFn g_setDllDirectoryAOriginal = nullptr;
        SetDefaultDllDirectoriesFn g_setDefaultDllDirectoriesOriginal = nullptr;
        AddDllDirectoryFn g_addDllDirectoryOriginal = nullptr;
        RemoveDllDirectoryFn g_removeDllDirectoryOriginal = nullptr;
        ImpersonateLoggedOnUserFn g_impersonateLoggedOnUserOriginal = nullptr;
        RevertToSelfFn g_revertToSelfOriginal = nullptr;
        SetThreadTokenFn g_setThreadTokenOriginal = nullptr;
        SetWindowsHookExWFn g_setWindowsHookExWOriginal = nullptr;
        SetWindowsHookExAFn g_setWindowsHookExAOriginal = nullptr;
        UnhookWindowsHookExFn g_unhookWindowsHookExOriginal = nullptr;
        EnumProcessesFn g_enumProcessesOriginal = nullptr;
        EnumProcessModulesFn g_enumProcessModulesOriginal = nullptr;
        EnumProcessModulesExFn g_enumProcessModulesExOriginal = nullptr;
        GetMappedFileNameWFn g_getMappedFileNameWOriginal = nullptr;
        GetMappedFileNameAFn g_getMappedFileNameAOriginal = nullptr;
        EnumWindowsFn g_enumWindowsOriginal = nullptr;
        EnumChildWindowsFn g_enumChildWindowsOriginal = nullptr;
        FindWindowWFn g_findWindowWOriginal = nullptr;
        FindWindowAFn g_findWindowAOriginal = nullptr;
        FindWindowExWFn g_findWindowExWOriginal = nullptr;
        FindWindowExAFn g_findWindowExAOriginal = nullptr;
        GetWindowThreadProcessIdFn g_getWindowThreadProcessIdOriginal = nullptr;
        GetForegroundWindowFn g_getForegroundWindowOriginal = nullptr;
        GetDCFn g_getDCOriginal = nullptr;
        ReleaseDCFn g_releaseDCOriginal = nullptr;
        CreateCompatibleDCFn g_createCompatibleDCOriginal = nullptr;
        DeleteDCFn g_deleteDCOriginal = nullptr;
        CreateCompatibleBitmapFn g_createCompatibleBitmapOriginal = nullptr;
        BitBltFn g_bitBltOriginal = nullptr;
        StretchBltFn g_stretchBltOriginal = nullptr;
        DeleteObjectFn g_deleteObjectOriginal = nullptr;
        OpenClipboardFn g_openClipboardOriginal = nullptr;
        CloseClipboardFn g_closeClipboardOriginal = nullptr;
        GetClipboardDataFn g_getClipboardDataOriginal = nullptr;
        SetClipboardDataFn g_setClipboardDataOriginal = nullptr;
        EmptyClipboardFn g_emptyClipboardOriginal = nullptr;
        StartTraceWFn g_startTraceWOriginal = nullptr;
        StartTraceAFn g_startTraceAOriginal = nullptr;
        ControlTraceWFn g_controlTraceWOriginal = nullptr;
        ControlTraceAFn g_controlTraceAOriginal = nullptr;
        EnableTraceEx2Fn g_enableTraceEx2Original = nullptr;
        OpenTraceWFn g_openTraceWOriginal = nullptr;
        OpenTraceAFn g_openTraceAOriginal = nullptr;
        ProcessTraceFn g_processTraceOriginal = nullptr;
        CloseTraceFn g_closeTraceOriginal = nullptr;
        EventRegisterFn g_eventRegisterOriginal = nullptr;
        EventUnregisterFn g_eventUnregisterOriginal = nullptr;
        EventWriteFn g_eventWriteOriginal = nullptr;
        EventWriteExFn g_eventWriteExOriginal = nullptr;
        WinVerifyTrustFn g_winVerifyTrustOriginal = nullptr;
        CryptQueryObjectFn g_cryptQueryObjectOriginal = nullptr;
        CertOpenStoreFn g_certOpenStoreOriginal = nullptr;
        CertCloseStoreFn g_certCloseStoreOriginal = nullptr;
        CertFindCertificateInStoreFn g_certFindCertificateInStoreOriginal = nullptr;
        CertGetCertificateChainFn g_certGetCertificateChainOriginal = nullptr;
        CertVerifyCertificateChainPolicyFn g_certVerifyCertificateChainPolicyOriginal = nullptr;
        CryptProtectDataFn g_cryptProtectDataOriginal = nullptr;
        CryptUnprotectDataFn g_cryptUnprotectDataOriginal = nullptr;
        NCryptOpenStorageProviderFn g_nCryptOpenStorageProviderOriginal = nullptr;
        NCryptOpenKeyFn g_nCryptOpenKeyOriginal = nullptr;
        NCryptCreatePersistedKeyFn g_nCryptCreatePersistedKeyOriginal = nullptr;
        NCryptFinalizeKeyFn g_nCryptFinalizeKeyOriginal = nullptr;
        NCryptEncryptFn g_nCryptEncryptOriginal = nullptr;
        NCryptDecryptFn g_nCryptDecryptOriginal = nullptr;
        NCryptSignHashFn g_nCryptSignHashOriginal = nullptr;
        NCryptVerifySignatureFn g_nCryptVerifySignatureOriginal = nullptr;
        NCryptExportKeyFn g_nCryptExportKeyOriginal = nullptr;
        NCryptImportKeyFn g_nCryptImportKeyOriginal = nullptr;
        NCryptDeleteKeyFn g_nCryptDeleteKeyOriginal = nullptr;
        NCryptFreeObjectFn g_nCryptFreeObjectOriginal = nullptr;
        RpcStringBindingComposeWFn g_rpcStringBindingComposeWOriginal = nullptr;
        RpcStringBindingComposeAFn g_rpcStringBindingComposeAOriginal = nullptr;
        RpcBindingFromStringBindingWFn g_rpcBindingFromStringBindingWOriginal = nullptr;
        RpcBindingFromStringBindingAFn g_rpcBindingFromStringBindingAOriginal = nullptr;
        RpcBindingFreeFn g_rpcBindingFreeOriginal = nullptr;
        RpcMgmtEpEltInqBeginFn g_rpcMgmtEpEltInqBeginOriginal = nullptr;
        RpcMgmtEpEltInqNextWFn g_rpcMgmtEpEltInqNextWOriginal = nullptr;
        RpcMgmtEpEltInqNextAFn g_rpcMgmtEpEltInqNextAOriginal = nullptr;
        RpcMgmtEpEltInqDoneFn g_rpcMgmtEpEltInqDoneOriginal = nullptr;
        NtQueryInformationTokenFn g_ntQueryInformationTokenOriginal = nullptr;
        NtSetInformationTokenFn g_ntSetInformationTokenOriginal = nullptr;
        NtAdjustPrivilegesTokenFn g_ntAdjustPrivilegesTokenOriginal = nullptr;
        NtCreateMutantFn g_ntCreateMutantOriginal = nullptr;
        NtOpenMutantFn g_ntOpenMutantOriginal = nullptr;
        NtReleaseMutantFn g_ntReleaseMutantOriginal = nullptr;
        NtCreateEventFn g_ntCreateEventOriginal = nullptr;
        NtOpenEventFn g_ntOpenEventOriginal = nullptr;
        NtSetEventFn g_ntSetEventOriginal = nullptr;
        NtResetEventFn g_ntResetEventOriginal = nullptr;
        NtWaitForSingleObjectFn g_ntWaitForSingleObjectOriginal = nullptr;
        NtWaitForMultipleObjectsFn g_ntWaitForMultipleObjectsOriginal = nullptr;
        NtQuerySystemInformationFn g_ntQuerySystemInformationOriginal = nullptr;
        NtQueryObjectFn g_ntQueryObjectOriginal = nullptr;
        LogonUserWFn g_logonUserWOriginal = nullptr;
        LogonUserAFn g_logonUserAOriginal = nullptr;
        GetTokenInformationFn g_getTokenInformationOriginal = nullptr;
        SetTokenInformationFn g_setTokenInformationOriginal = nullptr;
        CheckTokenMembershipFn g_checkTokenMembershipOriginal = nullptr;
        CreateRestrictedTokenFn g_createRestrictedTokenOriginal = nullptr;
        ImpersonateSelfFn g_impersonateSelfOriginal = nullptr;
        ImpersonateNamedPipeClientFn g_impersonateNamedPipeClientOriginal = nullptr;
        CredReadWFn g_credReadWOriginal = nullptr;
        CredReadAFn g_credReadAOriginal = nullptr;
        CredEnumerateWFn g_credEnumerateWOriginal = nullptr;
        CredEnumerateAFn g_credEnumerateAOriginal = nullptr;
        CredWriteWFn g_credWriteWOriginal = nullptr;
        CredWriteAFn g_credWriteAOriginal = nullptr;
        CredDeleteWFn g_credDeleteWOriginal = nullptr;
        CredDeleteAFn g_credDeleteAOriginal = nullptr;
        CredFreeFn g_credFreeOriginal = nullptr;
        LsaOpenPolicyFn g_lsaOpenPolicyOriginal = nullptr;
        LsaCloseFn g_lsaCloseOriginal = nullptr;
        LsaEnumerateLogonSessionsFn g_lsaEnumerateLogonSessionsOriginal = nullptr;
        LsaGetLogonSessionDataFn g_lsaGetLogonSessionDataOriginal = nullptr;
        LsaFreeReturnBufferFn g_lsaFreeReturnBufferOriginal = nullptr;
        LsaLookupNames2Fn g_lsaLookupNames2Original = nullptr;
        LsaLookupSids2Fn g_lsaLookupSids2Original = nullptr;
        OpenEventLogWFn g_openEventLogWOriginal = nullptr;
        OpenEventLogAFn g_openEventLogAOriginal = nullptr;
        RegisterEventSourceWFn g_registerEventSourceWOriginal = nullptr;
        RegisterEventSourceAFn g_registerEventSourceAOriginal = nullptr;
        ReadEventLogWFn g_readEventLogWOriginal = nullptr;
        ReadEventLogAFn g_readEventLogAOriginal = nullptr;
        ClearEventLogWFn g_clearEventLogWOriginal = nullptr;
        ClearEventLogAFn g_clearEventLogAOriginal = nullptr;
        ReportEventWFn g_reportEventWOriginal = nullptr;
        ReportEventAFn g_reportEventAOriginal = nullptr;
        CloseEventLogFn g_closeEventLogOriginal = nullptr;
        NetUserEnumFn g_netUserEnumOriginal = nullptr;
        NetLocalGroupEnumFn g_netLocalGroupEnumOriginal = nullptr;
        NetGroupEnumFn g_netGroupEnumOriginal = nullptr;
        NetShareEnumFn g_netShareEnumOriginal = nullptr;
        NetSessionEnumFn g_netSessionEnumOriginal = nullptr;
        NetServerEnumFn g_netServerEnumOriginal = nullptr;
        NetWkstaGetInfoFn g_netWkstaGetInfoOriginal = nullptr;
        NetApiBufferFreeFn g_netApiBufferFreeOriginal = nullptr;
        GetExtendedTcpTableFn g_getExtendedTcpTableOriginal = nullptr;
        GetExtendedUdpTableFn g_getExtendedUdpTableOriginal = nullptr;
        GetTcpTable2Fn g_getTcpTable2Original = nullptr;
        GetUdpTableFn g_getUdpTableOriginal = nullptr;
        GetAdaptersAddressesFn g_getAdaptersAddressesOriginal = nullptr;
        GetNetworkParamsFn g_getNetworkParamsOriginal = nullptr;
        GetIpNetTable2Fn g_getIpNetTable2Original = nullptr;
        GetIfTable2Fn g_getIfTable2Original = nullptr;
        FreeMibTableFn g_freeMibTableOriginal = nullptr;
        WTSOpenServerWFn g_wtsOpenServerWOriginal = nullptr;
        WTSOpenServerAFn g_wtsOpenServerAOriginal = nullptr;
        WTSCloseServerFn g_wtsCloseServerOriginal = nullptr;
        WTSEnumerateSessionsWFn g_wtsEnumerateSessionsWOriginal = nullptr;
        WTSEnumerateSessionsAFn g_wtsEnumerateSessionsAOriginal = nullptr;
        WTSEnumerateProcessesWFn g_wtsEnumerateProcessesWOriginal = nullptr;
        WTSEnumerateProcessesAFn g_wtsEnumerateProcessesAOriginal = nullptr;
        WTSQuerySessionInformationWFn g_wtsQuerySessionInformationWOriginal = nullptr;
        WTSQuerySessionInformationAFn g_wtsQuerySessionInformationAOriginal = nullptr;
        WTSFreeMemoryFn g_wtsFreeMemoryOriginal = nullptr;
        CreateJobObjectWFn g_createJobObjectWOriginal = nullptr;
        CreateJobObjectAFn g_createJobObjectAOriginal = nullptr;
        OpenJobObjectWFn g_openJobObjectWOriginal = nullptr;
        OpenJobObjectAFn g_openJobObjectAOriginal = nullptr;
        AssignProcessToJobObjectFn g_assignProcessToJobObjectOriginal = nullptr;
        TerminateJobObjectFn g_terminateJobObjectOriginal = nullptr;
        SetInformationJobObjectFn g_setInformationJobObjectOriginal = nullptr;
        QueryInformationJobObjectFn g_queryInformationJobObjectOriginal = nullptr;
        AcquireCredentialsHandleWFn g_acquireCredentialsHandleWOriginal = nullptr;
        AcquireCredentialsHandleAFn g_acquireCredentialsHandleAOriginal = nullptr;
        InitializeSecurityContextWFn g_initializeSecurityContextWOriginal = nullptr;
        InitializeSecurityContextAFn g_initializeSecurityContextAOriginal = nullptr;
        AcceptSecurityContextFn g_acceptSecurityContextOriginal = nullptr;
        EncryptMessageFn g_encryptMessageOriginal = nullptr;
        DecryptMessageFn g_decryptMessageOriginal = nullptr;
        DeleteSecurityContextFn g_deleteSecurityContextOriginal = nullptr;
        FreeCredentialsHandleFn g_freeCredentialsHandleOriginal = nullptr;
        Process32FirstWFn g_process32FirstWOriginal = nullptr;
        Process32FirstAFn g_process32FirstAOriginal = nullptr;
        Process32NextWFn g_process32NextWOriginal = nullptr;
        Process32NextAFn g_process32NextAOriginal = nullptr;
        Thread32FirstFn g_thread32FirstOriginal = nullptr;
        Thread32NextFn g_thread32NextOriginal = nullptr;
        Heap32ListFirstFn g_heap32ListFirstOriginal = nullptr;
        Heap32ListNextFn g_heap32ListNextOriginal = nullptr;
        Heap32FirstFn g_heap32FirstOriginal = nullptr;
        Heap32NextFn g_heap32NextOriginal = nullptr;
        QueryFullProcessImageNameWFn g_queryFullProcessImageNameWOriginal = nullptr;
        QueryFullProcessImageNameAFn g_queryFullProcessImageNameAOriginal = nullptr;
        GetProcessImageFileNameWFn g_getProcessImageFileNameWOriginal = nullptr;
        GetProcessImageFileNameAFn g_getProcessImageFileNameAOriginal = nullptr;
        GetProcessIdFn g_getProcessIdOriginal = nullptr;
        GetThreadIdFn g_getThreadIdOriginal = nullptr;
        IsWow64ProcessFn g_isWow64ProcessOriginal = nullptr;
        IsWow64Process2Fn g_isWow64Process2Original = nullptr;
        Wow64DisableWow64FsRedirectionFn g_wow64DisableWow64FsRedirectionOriginal = nullptr;
        Wow64RevertWow64FsRedirectionFn g_wow64RevertWow64FsRedirectionOriginal = nullptr;
        GetTempPathWFn g_getTempPathWOriginal = nullptr;
        GetTempPathAFn g_getTempPathAOriginal = nullptr;
        GetTempFileNameWFn g_getTempFileNameWOriginal = nullptr;
        GetTempFileNameAFn g_getTempFileNameAOriginal = nullptr;
        GetFullPathNameWFn g_getFullPathNameWOriginal = nullptr;
        GetFullPathNameAFn g_getFullPathNameAOriginal = nullptr;
        SearchPathWFn g_searchPathWOriginal = nullptr;
        SearchPathAFn g_searchPathAOriginal = nullptr;
        GetShortPathNameWFn g_getShortPathNameWOriginal = nullptr;
        GetShortPathNameAFn g_getShortPathNameAOriginal = nullptr;
        GetLongPathNameWFn g_getLongPathNameWOriginal = nullptr;
        GetLongPathNameAFn g_getLongPathNameAOriginal = nullptr;
        CreatePipeFn g_createPipeOriginal = nullptr;
        CreateMailslotWFn g_createMailslotWOriginal = nullptr;
        CreateMailslotAFn g_createMailslotAOriginal = nullptr;
        CreateDirectoryExWFn g_createDirectoryExWOriginal = nullptr;
        CreateDirectoryExAFn g_createDirectoryExAOriginal = nullptr;
        WSAStartupFn g_wsaStartupOriginal = nullptr;
        WSACleanupFn g_wsaCleanupOriginal = nullptr;
        SelectFn g_selectOriginal = nullptr;
        IoctlSocketFn g_ioctlSocketOriginal = nullptr;
        SetSockOptFn g_setSockOptOriginal = nullptr;
        GetSockOptFn g_getSockOptOriginal = nullptr;
        GetSockNameFn g_getSockNameOriginal = nullptr;
        GetPeerNameFn g_getPeerNameOriginal = nullptr;
        WSAEventSelectFn g_wsaEventSelectOriginal = nullptr;
        WSAAsyncSelectFn g_wsaAsyncSelectOriginal = nullptr;
        GetHostByNameFn g_getHostByNameOriginal = nullptr;
        GetHostByAddrFn g_getHostByAddrOriginal = nullptr;
        GetNameInfoWFn g_getNameInfoWOriginal = nullptr;
        GetNameInfoAFn g_getNameInfoAOriginal = nullptr;
        WinHttpAddRequestHeadersFn g_winHttpAddRequestHeadersOriginal = nullptr;
        WinHttpSetCredentialsFn g_winHttpSetCredentialsOriginal = nullptr;
        WinHttpCrackUrlFn g_winHttpCrackUrlOriginal = nullptr;
        WinHttpCreateUrlFn g_winHttpCreateUrlOriginal = nullptr;
        WinHttpSetTimeoutsFn g_winHttpSetTimeoutsOriginal = nullptr;
        HttpQueryInfoWFn g_httpQueryInfoWOriginal = nullptr;
        HttpQueryInfoAFn g_httpQueryInfoAOriginal = nullptr;
        InternetQueryOptionWFn g_internetQueryOptionWOriginal = nullptr;
        InternetQueryOptionAFn g_internetQueryOptionAOriginal = nullptr;
        NtOpenDirectoryObjectFn g_ntOpenDirectoryObjectOriginal = nullptr;
        NtQueryDirectoryObjectFn g_ntQueryDirectoryObjectOriginal = nullptr;
        NtCreateSymbolicLinkObjectFn g_ntCreateSymbolicLinkObjectOriginal = nullptr;
        NtOpenSymbolicLinkObjectFn g_ntOpenSymbolicLinkObjectOriginal = nullptr;
        NtQuerySymbolicLinkObjectFn g_ntQuerySymbolicLinkObjectOriginal = nullptr;
        NtCreateSemaphoreFn g_ntCreateSemaphoreOriginal = nullptr;
        NtOpenSemaphoreFn g_ntOpenSemaphoreOriginal = nullptr;
        NtReleaseSemaphoreFn g_ntReleaseSemaphoreOriginal = nullptr;

        thread_local bool g_hookReentryGuard = false;

        class ScopedHookGuard
        {
        public:
            ScopedHookGuard()
            {
                m_bypass = g_hookReentryGuard || IsInlineHookInternalBypassActive();
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
            void* hookAddress;                                      // hookAddress：Hooked wrapper 函数地址。
            void** originalOut;                                     // originalOut：Trampoline 返回地址。
        };

        struct RawHookBinding
        {
            std::wstring moduleName;                                // moduleName：Raw Hook 目标模块名。
            std::string procName;                                   // procName：Raw Hook 目标导出名。
            std::wstring procNameWide;                              // procNameWide：事件上报使用的宽字符导出名。
            ks::winapi_monitor::EventCategory categoryValue = ks::winapi_monitor::EventCategory::Process; // categoryValue：按模块粗略归类。
            InlineHookRecord hookRecord{};                          // hookRecord：Raw Hook inline patch 状态。
            void* originalAddress = nullptr;                        // originalAddress：InstallInlineHook 生成的 trampoline。
            void* entryStubAddress = nullptr;                       // entryStubAddress：动态生成的通用入口 stub。
        };

        struct FakeSuccessRuntimeRule
        {
            std::wstring moduleName;                                // moduleName：事件上报使用的模块名。
            std::wstring installModuleName;                         // installModuleName：传给 GetModuleHandleW 的模块名，默认补齐 .dll。
            std::wstring apiName;                                   // apiName：事件上报使用的 API 名。
            std::string apiNameAnsi;                                // apiNameAnsi：传给 GetProcAddress 的 ANSI 导出名。
            std::wstring matchKey;                                  // matchKey：规范化 module!api 精确匹配键。
            ks::winapi_monitor::EventCategory categoryValue = ks::winapi_monitor::EventCategory::Process; // categoryValue：事件分类。
            FakeSuccessReturnType returnType = FakeSuccessReturnType::Scalar; // returnType：返回值模板。
            FakeSuccessLastErrorKind lastErrorKind = FakeSuccessLastErrorKind::None; // lastErrorKind：错误码写入方式。
            std::uint64_t returnValue = 0;                          // returnValue：写入 RAX 的返回值。
            std::uint32_t lastErrorValue = 0;                       // lastErrorValue：Win32/WSA 错误码。
            InlineHookRecord hookRecord{};                          // hookRecord：Fake 专用 inline patch 状态。
            void* originalAddress = nullptr;                        // originalAddress：安装时仍生成 trampoline，但 fake 路径不会跳入。
            void* entryStubAddress = nullptr;                       // entryStubAddress：动态生成的 fake return stub。
        };

        // RawBindings：
        // - 输入：无；
        // - 处理：返回进程内唯一 Raw Hook 动态绑定集合，集合本身故意泄漏到进程结束；
        // - 返回：可手动 clear 的 vector 引用。
        // - 原因：目标进程退出阶段仍可能有被 hook API 被 CRT/loader 调用，自动析构会让入口 stub 指向已释放上下文。
        std::vector<std::unique_ptr<RawHookBinding>>& RawBindings()
        {
            static auto* const bindingList = new std::vector<std::unique_ptr<RawHookBinding>>();
            return *bindingList;
        }

        // RawHookKeys：
        // - 输入：无；
        // - 处理：返回 Raw module!export 去重集合，集合本身故意泄漏到进程结束；
        // - 返回：可手动 clear 的 unordered_set 引用。
        std::unordered_set<std::wstring>& RawHookKeys()
        {
            static auto* const keySet = new std::unordered_set<std::wstring>();
            return *keySet;
        }

        // FakeSuccessRules：
        // - 输入：无；
        // - 处理：返回 Fake Success 规则集合，集合本身不参与 C++ 静态析构；
        // - 返回：可手动 clear 的 vector 引用。
        // - 原因：fake entry stub 持有 FakeSuccessRuntimeRule*，进程退出未显式 Stop 时不能让自动析构提前释放该上下文。
        std::vector<std::unique_ptr<FakeSuccessRuntimeRule>>& FakeSuccessRules()
        {
            static auto* const ruleList = new std::vector<std::unique_ptr<FakeSuccessRuntimeRule>>();
            return *ruleList;
        }

        // FakeSuccessRuleMap：
        // - 输入：无；
        // - 处理：返回 module!api 到 Fake 规则的快速索引，映射本身不参与 C++ 静态析构；
        // - 返回：可手动 clear 的 unordered_map 引用。
        std::unordered_map<std::wstring, FakeSuccessRuntimeRule*>& FakeSuccessRuleMap()
        {
            static auto* const ruleMap = new std::unordered_map<std::wstring, FakeSuccessRuntimeRule*>();
            return *ruleMap;
        }

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

        std::wstring ToLowerWide(std::wstring textValue)
        {
            std::transform(
                textValue.begin(),
                textValue.end(),
                textValue.begin(),
                [](const wchar_t ch) { return static_cast<wchar_t>(::towlower(ch)); });
            return textValue;
        }

        std::string ToLowerAnsi(std::string textValue)
        {
            std::transform(
                textValue.begin(),
                textValue.end(),
                textValue.begin(),
                [](const unsigned char ch) { return static_cast<char>(::tolower(ch)); });
            return textValue;
        }

        // MakeRawHookKey 作用：
        // - 输入：模块名和导出名；
        // - 处理：统一大小写并拼接成 module!api 键；
        // - 返回：Raw 绑定去重和强类型覆盖判断使用的稳定 key。
        std::wstring MakeRawHookKey(const std::wstring& moduleName, const std::string& procName)
        {
            return ToLowerWide(moduleName) + L"!" + ToLowerWide(AnsiToWide(procName.c_str()));
        }

        std::wstring NormalizeModuleNameForMatch(std::wstring moduleName)
        {
            moduleName = ToLowerWide(moduleName);
            if (moduleName.size() > 4 && moduleName.substr(moduleName.size() - 4) == L".dll")
            {
                moduleName.resize(moduleName.size() - 4);
            }
            return moduleName;
        }

        std::wstring MakeFakeSuccessKey(const std::wstring& moduleName, const std::wstring& apiName)
        {
            return NormalizeModuleNameForMatch(moduleName) + L"!" + ToLowerWide(apiName);
        }

        std::wstring MakeFakeSuccessKey(const std::wstring& moduleName, const std::string& apiName)
        {
            return MakeFakeSuccessKey(moduleName, AnsiToWide(apiName.c_str()));
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

        ks::winapi_monitor::EventCategory InferRawHookCategory(const std::wstring& moduleName, const std::string& procName);
        void EmitRawStubByte(unsigned char* codePointer, std::size_t& offsetValue, unsigned char byteValue);
        void EmitRawStubU64(unsigned char* codePointer, std::size_t& offsetValue, std::uint64_t value);

        std::string WideToAnsiExportName(const std::wstring& textValue)
        {
            // WideToAnsiExportName 作用：
            // - 输入：UI/INI 中保存的 API 导出名宽字符串；
            // - 处理：按系统代码页转成 GetProcAddress 需要的窄字符串，失败时仅保留 ASCII；
            // - 返回：可传给 GetProcAddress 的导出名，无法转换时返回空串。
            if (textValue.empty())
            {
                return std::string();
            }

            const int requiredBytes = ::WideCharToMultiByte(
                CP_ACP,
                0,
                textValue.c_str(),
                -1,
                nullptr,
                0,
                nullptr,
                nullptr);
            if (requiredBytes > 0)
            {
                std::string ansiText(static_cast<std::size_t>(requiredBytes), '\0');
                const int convertedBytes = ::WideCharToMultiByte(
                    CP_ACP,
                    0,
                    textValue.c_str(),
                    -1,
                    ansiText.data(),
                    requiredBytes,
                    nullptr,
                    nullptr);
                if (convertedBytes > 0 && !ansiText.empty())
                {
                    ansiText.resize(static_cast<std::size_t>(convertedBytes - 1));
                    return ansiText;
                }
            }

            std::string fallbackText;
            for (const wchar_t ch : textValue)
            {
                if (ch == L'\0')
                {
                    break;
                }
                if (ch < 0x20 || ch > 0x7E)
                {
                    return std::string();
                }
                fallbackText.push_back(static_cast<char>(ch));
            }
            return fallbackText;
        }

        std::wstring BuildInstallModuleName(const std::wstring& moduleName)
        {
            // BuildInstallModuleName 作用：
            // - 输入：用户配置的模块名，可以含 .dll，也可以只写 KernelBase 这样的短名；
            // - 处理：安装 Hook 时保留已有扩展名，无扩展名时补齐 .dll；
            // - 返回：GetModuleHandleW 可直接尝试匹配的模块名。
            std::wstring installName = moduleName;
            if (installName.find(L'.') == std::wstring::npos)
            {
                installName.append(L".dll");
            }
            return installName;
        }

        FakeSuccessRuntimeRule* FindFakeSuccessRule(const std::wstring& moduleName, const std::string& procName)
        {
            const MonitorConfig& configValue = ActiveConfig();
            auto& ruleMap = FakeSuccessRuleMap();
            if (!configValue.fakeSuccessEnabled || ruleMap.empty())
            {
                return nullptr;
            }

            const auto iterator = ruleMap.find(MakeFakeSuccessKey(moduleName, procName));
            return iterator != ruleMap.end() ? iterator->second : nullptr;
        }

        template <std::size_t kCount>
        void AppendFakeSuccessDetail(wchar_t(&detailBuffer)[kCount], const FakeSuccessRuntimeRule& ruleValue)
        {
            AppendWideText(detailBuffer, L"FakeSuccess=1 original=skipped returnType=");
            switch (ruleValue.returnType)
            {
            case FakeSuccessReturnType::Bool: AppendWideText(detailBuffer, L"BOOL"); break;
            case FakeSuccessReturnType::Handle: AppendWideText(detailBuffer, L"HANDLE/PVOID"); break;
            case FakeSuccessReturnType::Dword: AppendWideText(detailBuffer, L"DWORD/UINT/int"); break;
            case FakeSuccessReturnType::NtStatus: AppendWideText(detailBuffer, L"NTSTATUS"); break;
            case FakeSuccessReturnType::HResult: AppendWideText(detailBuffer, L"HRESULT"); break;
            case FakeSuccessReturnType::LStatus: AppendWideText(detailBuffer, L"LSTATUS"); break;
            case FakeSuccessReturnType::SocketInt: AppendWideText(detailBuffer, L"SOCKET/int(WSA)"); break;
            default: AppendWideText(detailBuffer, L"Scalar"); break;
            }
            AppendWideText(detailBuffer, L" return=");
            AppendHexText(detailBuffer, ruleValue.returnValue);
            if (ruleValue.lastErrorKind != FakeSuccessLastErrorKind::None)
            {
                AppendWideText(detailBuffer, ruleValue.lastErrorKind == FakeSuccessLastErrorKind::Wsa ? L" WSAError=" : L" LastError=");
                AppendUnsignedText(detailBuffer, ruleValue.lastErrorValue);
            }
        }

        std::int32_t FakeSuccessResultCode(const FakeSuccessRuntimeRule& ruleValue)
        {
            if (ruleValue.lastErrorKind != FakeSuccessLastErrorKind::None && ruleValue.lastErrorValue != 0)
            {
                return static_cast<std::int32_t>(ruleValue.lastErrorValue);
            }
            if (ruleValue.returnType == FakeSuccessReturnType::Bool)
            {
                return ruleValue.returnValue != 0 ? 0 : static_cast<std::int32_t>(ruleValue.returnValue & 0xFFFFFFFFULL);
            }
            if (ruleValue.returnType == FakeSuccessReturnType::Handle)
            {
                return (ruleValue.returnValue != 0 && ruleValue.returnValue != 0xFFFFFFFFFFFFFFFFULL)
                    ? 0
                    : static_cast<std::int32_t>(ruleValue.returnValue & 0xFFFFFFFFULL);
            }
            if (ruleValue.returnType == FakeSuccessReturnType::SocketInt)
            {
                return static_cast<std::uint32_t>(ruleValue.returnValue & 0xFFFFFFFFULL) != 0xFFFFFFFFU
                    ? 0
                    : static_cast<std::int32_t>(ruleValue.returnValue & 0xFFFFFFFFULL);
            }
            return static_cast<std::int32_t>(ruleValue.returnValue & 0xFFFFFFFFULL);
        }

        std::uint64_t FakeSuccessEnter(FakeSuccessRuntimeRule* const ruleValue)
        {
            if (ruleValue == nullptr)
            {
                return 0;
            }

            const DWORD previousLastError = ::GetLastError();
            ScopedHookGuard guardValue;
            if (!guardValue.bypass())
            {
                wchar_t detailBuffer[ks::winapi_monitor::kMaxDetailChars] = {};
                AppendFakeSuccessDetail(detailBuffer, *ruleValue);
                SendRawEventWithStatus(
                    ruleValue->categoryValue,
                    ruleValue->moduleName.c_str(),
                    ruleValue->apiName.c_str(),
                    FakeSuccessResultCode(*ruleValue),
                    detailBuffer);
            }

            if (ruleValue->lastErrorKind == FakeSuccessLastErrorKind::Win32)
            {
                ::SetLastError(ruleValue->lastErrorValue);
            }
            else if (ruleValue->lastErrorKind == FakeSuccessLastErrorKind::Wsa)
            {
                ::SetLastError(previousLastError);
                ::WSASetLastError(static_cast<int>(ruleValue->lastErrorValue));
            }
            else
            {
                ::SetLastError(previousLastError);
            }

            return ruleValue->returnValue;
        }

        // BuildFakeSuccessRuleIndex 作用：
        // - 输入：ActiveConfig 中由 UI 写入的 Fake Success 规则；
        // - 处理：规范化 module!api 匹配键，补齐安装模块名，转换 ANSI 导出名，并按“首条规则优先”建立索引；
        // - 返回：无返回值，FakeSuccessRules/FakeSuccessRuleMap 保存本会话可安装的运行时规则。
        void BuildFakeSuccessRuleIndex()
        {
            auto& ruleList = FakeSuccessRules();
            auto& ruleMap = FakeSuccessRuleMap();
            ruleList.clear();
            ruleMap.clear();

            const MonitorConfig& configValue = ActiveConfig();
            if (!configValue.fakeSuccessEnabled)
            {
                return;
            }

            for (const FakeSuccessRule& sourceRule : configValue.fakeSuccessRules)
            {
                const std::wstring matchKey = MakeFakeSuccessKey(sourceRule.moduleName, sourceRule.apiName);
                if (matchKey.empty() || ruleMap.find(matchKey) != ruleMap.end())
                {
                    continue;
                }

                std::string ansiApiName = WideToAnsiExportName(sourceRule.apiName);
                if (ansiApiName.empty())
                {
                    continue;
                }

                auto runtimeRule = std::make_unique<FakeSuccessRuntimeRule>();
                runtimeRule->moduleName = sourceRule.moduleName;
                runtimeRule->installModuleName = BuildInstallModuleName(sourceRule.moduleName);
                runtimeRule->apiName = sourceRule.apiName;
                runtimeRule->apiNameAnsi = std::move(ansiApiName);
                runtimeRule->matchKey = matchKey;
                runtimeRule->categoryValue = InferRawHookCategory(runtimeRule->installModuleName, runtimeRule->apiNameAnsi);
                runtimeRule->returnType = sourceRule.returnType;
                runtimeRule->returnValue = sourceRule.returnValue;
                runtimeRule->lastErrorKind = sourceRule.lastErrorKind;
                runtimeRule->lastErrorValue = sourceRule.lastErrorValue;

                FakeSuccessRuntimeRule* const rulePointer = runtimeRule.get();
                ruleMap.emplace(rulePointer->matchKey, rulePointer);
                ruleList.push_back(std::move(runtimeRule));
            }
        }

        // BuildFakeSuccessEntryStub 作用：
        // - 输入：ruleValue 指向一条 Fake Success 运行时规则；
        // - 处理：生成 x64 小型 detour stub，调用 FakeSuccessEnter(rule) 上报事件并取得 RAX 返回值；
        // - 返回：可执行内存地址，作为 InstallInlineHook 的 detourAddress，失败返回 nullptr。
        void* BuildFakeSuccessEntryStub(FakeSuccessRuntimeRule* const ruleValue)
        {
            if (ruleValue == nullptr)
            {
                return nullptr;
            }

            constexpr std::size_t kFakeStubBytes = 64;
            unsigned char* const codePointer = static_cast<unsigned char*>(::VirtualAlloc(
                nullptr,
                kFakeStubBytes,
                MEM_COMMIT | MEM_RESERVE,
                PAGE_EXECUTE_READWRITE));
            if (codePointer == nullptr)
            {
                return nullptr;
            }

            std::size_t offsetValue = 0;
            const auto emit = [codePointer, &offsetValue](const unsigned char byteValue) {
                EmitRawStubByte(codePointer, offsetValue, byteValue);
            };

            emit(0x48); emit(0x83); emit(0xEC); emit(0x28); // sub rsp, 0x28：对齐栈并提供 32 字节 shadow space。
            emit(0x48); emit(0xB9);                         // mov rcx, ruleValue：Windows x64 第一个参数。
            EmitRawStubU64(codePointer, offsetValue, reinterpret_cast<std::uint64_t>(ruleValue));
            emit(0x48); emit(0xB8);                         // mov rax, FakeSuccessEnter。
            EmitRawStubU64(codePointer, offsetValue, reinterpret_cast<std::uint64_t>(&FakeSuccessEnter));
            emit(0xFF); emit(0xD0);                         // call rax：返回值留在 RAX。
            emit(0x48); emit(0x83); emit(0xC4); emit(0x28); // add rsp, 0x28：恢复调用者栈。
            emit(0xC3);                                     // ret：直接返回到原 API 调用者，原函数完全不执行。

            ::FlushInstructionCache(::GetCurrentProcess(), codePointer, offsetValue);
            return codePointer;
        }

        void FreeFakeSuccessEntryStub(void* const stubAddress)
        {
            // FreeFakeSuccessEntryStub 作用：
            // - 输入：BuildFakeSuccessEntryStub 返回的可执行内存；
            // - 处理：释放动态 stub，调用方必须先卸载 inline hook；
            // - 返回：无返回值。
            if (stubAddress != nullptr)
            {
                ::VirtualFree(stubAddress, 0, MEM_RELEASE);
            }
        }

        void AppendFakeSuccessFailureText(
            std::wstring* const detailTextOut,
            const FakeSuccessRuntimeRule& ruleValue,
            const InlineHookInstallResult installResult,
            const std::wstring& errorText)
        {
            // AppendFakeSuccessFailureText 作用：
            // - 输入：安装失败的规则、失败类型和 HookEngine 诊断文本；
            // - 处理：追加到 InstallConfiguredHooks 的聚合错误里，便于 UI 内部事件显示；
            // - 返回：无返回值，detailTextOut 为空时静默跳过。
            if (detailTextOut == nullptr)
            {
                return;
            }
            if (!detailTextOut->empty())
            {
                detailTextOut->append(L" | ");
            }

            detailTextOut->append(ruleValue.moduleName);
            detailTextOut->append(L"!");
            detailTextOut->append(ruleValue.apiName);
            detailTextOut->append(
                installResult == InlineHookInstallResult::RetryableFailure
                ? L": fake retryable failure - "
                : L": fake disabled - ");
            detailTextOut->append(errorText.empty() ? L"unknown reason" : errorText);
        }

        bool TryInstallFakeSuccessRule(
            FakeSuccessRuntimeRule& ruleValue,
            const std::optional<ks::winapi_monitor::EventCategory> categoryOverride,
            std::wstring* const detailTextOut)
        {
            // TryInstallFakeSuccessRule 作用：
            // - 输入：运行时规则，可选分类覆盖用于强类型绑定命中时保持分类更准确；
            // - 处理：为规则生成 fake-return stub，并把目标导出 inline patch 到该 stub；
            // - 返回：当前规则已安装或本次安装成功返回 true，失败返回 false。
            if (categoryOverride.has_value())
            {
                ruleValue.categoryValue = categoryOverride.value();
            }
            if (ruleValue.hookRecord.installed || ruleValue.hookRecord.permanentlyDisabled)
            {
                return ruleValue.hookRecord.installed;
            }
            if (ruleValue.entryStubAddress == nullptr)
            {
                ruleValue.entryStubAddress = BuildFakeSuccessEntryStub(&ruleValue);
                if (ruleValue.entryStubAddress == nullptr)
                {
                    ruleValue.hookRecord.permanentlyDisabled = true;
                    AppendFakeSuccessFailureText(
                        detailTextOut,
                        ruleValue,
                        InlineHookInstallResult::PermanentFailure,
                        L"VirtualAlloc for fake-return stub failed.");
                    return false;
                }
            }

            std::wstring errorText;
            const InlineHookInstallResult installResult = InstallInlineHook(
                ruleValue.installModuleName.c_str(),
                ruleValue.apiNameAnsi.c_str(),
                ruleValue.entryStubAddress,
                &ruleValue.hookRecord,
                &ruleValue.originalAddress,
                &errorText);
            if (installResult == InlineHookInstallResult::Installed)
            {
                return true;
            }
            if (installResult == InlineHookInstallResult::PermanentFailure)
            {
                ruleValue.hookRecord.permanentlyDisabled = true;
            }
            AppendFakeSuccessFailureText(detailTextOut, ruleValue, installResult, errorText);
            return false;
        }

        // RawHookEnter 作用：
        // - 输入：bindingValue 为动态 stub 传入的 Raw Hook 元数据；
        // - 处理：在不解析参数语义的前提下上报 module/api/target/trampoline，并用全局 reentry guard 避免日志链路递归；
        // - 返回：无返回值，动态 stub 随后恢复寄存器并跳入 trampoline 继续执行原函数。
        void RawHookEnter(RawHookBinding* const bindingValue)
        {
            if (bindingValue == nullptr)
            {
                return;
            }

            ScopedHookGuard guardValue;
            if (guardValue.bypass())
            {
                return;
            }

            wchar_t detailBuffer[ks::winapi_monitor::kMaxDetailChars] = {};
            AppendWideText(detailBuffer, L"Raw ABI fallback target=");
            AppendHexText(detailBuffer, reinterpret_cast<std::uint64_t>(bindingValue->hookRecord.targetAddress));
            AppendWideText(detailBuffer, L" trampoline=");
            AppendHexText(detailBuffer, reinterpret_cast<std::uint64_t>(bindingValue->originalAddress));
            AppendWideText(detailBuffer, L" strongTyped=0 params=unparsed");
            SendRawEventWithStatus(
                bindingValue->categoryValue,
                bindingValue->moduleName.c_str(),
                bindingValue->procNameWide.c_str(),
                0,
                detailBuffer);
        }

        // EmitRawStubByte/EmitRawStubU32/EmitRawStubU64 作用：
        // - 输入：动态代码缓冲和待写入数值；
        // - 处理：顺序写入 x64 Raw Hook stub 指令字节；
        // - 返回：无返回值，offsetValue 推进到下一条指令位置。
        void EmitRawStubByte(unsigned char* const codePointer, std::size_t& offsetValue, const unsigned char byteValue)
        {
            codePointer[offsetValue++] = byteValue;
        }

        void EmitRawStubU32(unsigned char* const codePointer, std::size_t& offsetValue, const std::uint32_t value)
        {
            std::memcpy(codePointer + offsetValue, &value, sizeof(value));
            offsetValue += sizeof(value);
        }

        void EmitRawStubU64(unsigned char* const codePointer, std::size_t& offsetValue, const std::uint64_t value)
        {
            std::memcpy(codePointer + offsetValue, &value, sizeof(value));
            offsetValue += sizeof(value);
        }

        void EmitRawStubMovdquStore(unsigned char* const codePointer, std::size_t& offsetValue, const unsigned char xmmIndex, const unsigned char stackOffset)
        {
            EmitRawStubByte(codePointer, offsetValue, 0xF3);
            EmitRawStubByte(codePointer, offsetValue, 0x0F);
            EmitRawStubByte(codePointer, offsetValue, 0x7F);
            EmitRawStubByte(codePointer, offsetValue, static_cast<unsigned char>(0x44U + (xmmIndex * 0x08U)));
            EmitRawStubByte(codePointer, offsetValue, 0x24);
            EmitRawStubByte(codePointer, offsetValue, stackOffset);
        }

        void EmitRawStubMovdquLoad(unsigned char* const codePointer, std::size_t& offsetValue, const unsigned char xmmIndex, const unsigned char stackOffset)
        {
            EmitRawStubByte(codePointer, offsetValue, 0xF3);
            EmitRawStubByte(codePointer, offsetValue, 0x0F);
            EmitRawStubByte(codePointer, offsetValue, 0x6F);
            EmitRawStubByte(codePointer, offsetValue, static_cast<unsigned char>(0x44U + (xmmIndex * 0x08U)));
            EmitRawStubByte(codePointer, offsetValue, 0x24);
            EmitRawStubByte(codePointer, offsetValue, stackOffset);
        }

        // BuildRawEntryStub 作用：
        // - 输入：bindingValue 指向 Raw Hook 元数据，stub 通过该指针读取 trampoline；
        // - 处理：生成 x64 通用入口桩，保存整数参数寄存器和 XMM0-XMM5，调用 RawHookEnter 后跳转到 trampoline；
        // - 返回：可作为 InstallInlineHook hookAddress 的可执行内存地址，失败返回 nullptr。
        void* BuildRawEntryStub(RawHookBinding* const bindingValue)
        {
            if (bindingValue == nullptr)
            {
                return nullptr;
            }

            constexpr std::size_t kRawStubBytes = 256;
            unsigned char* const codePointer = static_cast<unsigned char*>(::VirtualAlloc(
                nullptr,
                kRawStubBytes,
                MEM_COMMIT | MEM_RESERVE,
                PAGE_EXECUTE_READWRITE));
            if (codePointer == nullptr)
            {
                return nullptr;
            }

            std::size_t offsetValue = 0;
            const auto emit = [codePointer, &offsetValue](const unsigned char byteValue) {
                EmitRawStubByte(codePointer, offsetValue, byteValue);
            };

            emit(0x50);                         // push rax
            emit(0x51);                         // push rcx
            emit(0x52);                         // push rdx
            emit(0x41); emit(0x50);             // push r8
            emit(0x41); emit(0x51);             // push r9
            emit(0x41); emit(0x52);             // push r10
            emit(0x41); emit(0x53);             // push r11
            emit(0x48); emit(0x81); emit(0xEC); // sub rsp, 0xA0
            EmitRawStubU32(codePointer, offsetValue, 0x000000A0U);

            EmitRawStubMovdquStore(codePointer, offsetValue, 0, 0x20);
            EmitRawStubMovdquStore(codePointer, offsetValue, 1, 0x30);
            EmitRawStubMovdquStore(codePointer, offsetValue, 2, 0x40);
            EmitRawStubMovdquStore(codePointer, offsetValue, 3, 0x50);
            EmitRawStubMovdquStore(codePointer, offsetValue, 4, 0x60);
            EmitRawStubMovdquStore(codePointer, offsetValue, 5, 0x70);

            emit(0x48); emit(0xB9);             // mov rcx, bindingValue
            EmitRawStubU64(codePointer, offsetValue, reinterpret_cast<std::uint64_t>(bindingValue));
            emit(0x48); emit(0xB8);             // mov rax, RawHookEnter
            EmitRawStubU64(codePointer, offsetValue, reinterpret_cast<std::uint64_t>(&RawHookEnter));
            emit(0xFF); emit(0xD0);             // call rax

            EmitRawStubMovdquLoad(codePointer, offsetValue, 0, 0x20);
            EmitRawStubMovdquLoad(codePointer, offsetValue, 1, 0x30);
            EmitRawStubMovdquLoad(codePointer, offsetValue, 2, 0x40);
            EmitRawStubMovdquLoad(codePointer, offsetValue, 3, 0x50);
            EmitRawStubMovdquLoad(codePointer, offsetValue, 4, 0x60);
            EmitRawStubMovdquLoad(codePointer, offsetValue, 5, 0x70);

            emit(0x48); emit(0x81); emit(0xC4); // add rsp, 0xA0
            EmitRawStubU32(codePointer, offsetValue, 0x000000A0U);
            emit(0x41); emit(0x5B);             // pop r11
            emit(0x41); emit(0x5A);             // pop r10
            emit(0x41); emit(0x59);             // pop r9
            emit(0x41); emit(0x58);             // pop r8
            emit(0x5A);                         // pop rdx
            emit(0x59);                         // pop rcx
            emit(0x58);                         // pop rax
            emit(0x49); emit(0xBB);             // mov r11, &bindingValue->originalAddress
            EmitRawStubU64(codePointer, offsetValue, reinterpret_cast<std::uint64_t>(&bindingValue->originalAddress));
            emit(0x4D); emit(0x8B); emit(0x1B); // mov r11, [r11]
            emit(0x41); emit(0xFF); emit(0xE3); // jmp r11

            ::FlushInstructionCache(::GetCurrentProcess(), codePointer, offsetValue);
            return codePointer;
        }

        void FreeRawEntryStub(void* const stubAddress)
        {
            if (stubAddress != nullptr)
            {
                ::VirtualFree(stubAddress, 0, MEM_RELEASE);
            }
        }

        bool TryInstallBinding(HookBinding& bindingValue, std::wstring* detailTextOut)
        {
            FakeSuccessRuntimeRule* const fakeRule = FindFakeSuccessRule(bindingValue.moduleName, bindingValue.procName);
            if (fakeRule != nullptr)
            {
                return TryInstallFakeSuccessRule(*fakeRule, bindingValue.categoryValue, detailTextOut);
            }

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
                bindingValue.hookAddress,
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

        // JoinIniList 作用：
        // - 输入：MonitorConfig 中已拆分的 Raw 模块/黑名单列表；
        // - 处理：按 INI 单行格式重新用分号拼接，供自动注入子进程继承父进程 Raw 配置；
        // - 返回：可直接写入 config_<pid>.ini 的列表文本。
        std::wstring JoinIniList(const std::vector<std::wstring>& itemList)
        {
            std::wstring joinedText;
            for (const std::wstring& itemText : itemList)
            {
                if (itemText.empty())
                {
                    continue;
                }
                if (!joinedText.empty())
                {
                    joinedText.append(L";");
                }
                joinedText.append(itemText);
            }
            return joinedText;
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
                L"enable_raw_fallback=" + std::to_wstring(configValue.enableRawFallback ? 1 : 0) + L"\r\n"
                L"raw_use_default_denylist=" + std::to_wstring(configValue.rawUseDefaultDenyList ? 1 : 0) + L"\r\n"
                L"raw_modules=" + JoinIniList(configValue.rawModuleList) + L"\r\n"
                L"raw_denylist=" + JoinIniList(configValue.rawDenyList) + L"\r\n"
                L"fake_success_enabled=" + std::to_wstring(configValue.fakeSuccessEnabled ? 1 : 0) + L"\r\n"
                L"fake_success_raw_fallback=" + std::to_wstring(configValue.fakeSuccessRawFallback ? 1 : 0) + L"\r\n"
                L"fake_success_rules=" + configValue.fakeSuccessRulesText + L"\r\n"
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

#define APIMON_BOOL_SINGLE_W(HookName, Original, ApiName) \
        BOOL WINAPI HookName(LPCWSTR pathPointer) \
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

#define APIMON_BOOL_SINGLE_A(HookName, Original, ApiName) \
        BOOL WINAPI HookName(LPCSTR pathPointer) \
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
            AppendWideText(detailBuffer, L"fsctlName=");
            AppendWideText(detailBuffer, FsctlCodeToText(fsControlCode));
            AppendWideText(detailBuffer, L" handle=");
            AppendHexText(detailBuffer, reinterpret_cast<std::uint64_t>(fileHandle));
            AppendWideText(detailBuffer, L" code=");
            AppendHexText(detailBuffer, fsControlCode);
            AppendWideText(detailBuffer, L" status=");
            AppendHexText(detailBuffer, static_cast<std::uint32_t>(statusValue));
            AppendWideText(detailBuffer, L" in=");
            AppendUnsignedText(detailBuffer, inputBufferLength);
            AppendWideText(detailBuffer, L" out=");
            AppendUnsignedText(detailBuffer, outputBufferLength);
            AppendWideText(detailBuffer, L" info=");
            AppendUnsignedText(detailBuffer, ioStatusBlockPointer != nullptr ? static_cast<unsigned long long>(ioStatusBlockPointer->Information) : 0);
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


        // AppendProcNameText 作用：
        // - 输入：namePointer 为 GetProcAddress 形式的函数名，可能是 MAKEINTRESOURCEA 风格序号；
        // - 处理：优先输出 name=文本，序号输入输出 ordinal=<id>，避免把低地址序号当字符串解引用；
        // - 返回：无返回值，目标缓冲追加安全可显示的过程名摘要。
        template <std::size_t kCount>
        void AppendProcNameText(wchar_t(&detailBuffer)[kCount], const char* const namePointer)
        {
            if (namePointer == nullptr)
            {
                AppendWideText(detailBuffer, L"name=<null>");
                return;
            }
            if (HIWORD(reinterpret_cast<ULONG_PTR>(namePointer)) == 0)
            {
                AppendWideText(detailBuffer, L"ordinal=");
                AppendUnsignedText(detailBuffer, static_cast<unsigned long long>(LOWORD(reinterpret_cast<ULONG_PTR>(namePointer))));
                return;
            }
            AppendWideText(detailBuffer, L"name=");
            AppendAnsiText(detailBuffer, namePointer);
        }

        // AppendAnsiStringText 作用：
        // - 输入：ansiPointer 为 ntdll ANSI_STRING，可为空；
        // - 处理：按 Length 复制窄字符，避免依赖 Buffer 以 NUL 结束；
        // - 返回：无返回值，目标缓冲追加可截断的过程名文本。
        template <std::size_t kCount>
        void AppendAnsiStringText(wchar_t(&detailBuffer)[kCount], const ANSI_STRING* const ansiPointer)
        {
            if (ansiPointer == nullptr || ansiPointer->Buffer == nullptr || ansiPointer->Length == 0)
            {
                return;
            }
            AppendAnsiText(detailBuffer, ansiPointer->Buffer, ansiPointer->Length);
        }

        // BuildSimpleHandleDetail 作用：
        // - 输入：fieldName 为字段名，handleValue 为待记录句柄；
        // - 处理：生成 field=<hex> 统一详情，复用在 CloseHandle/NtClose/ServiceHandle 等生命周期 API；
        // - 返回：无返回值，detailBuffer 保存固定长度摘要。
        template <std::size_t kCount, typename HandleType>
        void BuildSimpleHandleDetail(wchar_t(&detailBuffer)[kCount], const wchar_t* const fieldName, const HandleType handleValue)
        {
            detailBuffer[0] = L'\0';
            AppendWideText(detailBuffer, fieldName != nullptr ? fieldName : L"handle");
            AppendWideText(detailBuffer, L"=");
            AppendHexText(detailBuffer, reinterpret_cast<std::uint64_t>(handleValue));
        }

        // EmitWin32BoolEvent 作用：
        // - 输入：分类、模块、API、BOOL 结果、LastError 和详情；
        // - 处理：把 BOOL 成功归一为 resultCode=0，失败用 LastError，并恢复调用者线程 LastError；
        // - 返回：无返回值，原函数返回值由 Hooked wrapper 自行返回。
        void EmitWin32BoolEvent(
            const ks::winapi_monitor::EventCategory categoryValue,
            const wchar_t* const moduleName,
            const wchar_t* const apiName,
            const BOOL resultValue,
            const DWORD lastError,
            const wchar_t* const detailText)
        {
            SendRawEventWithStatus(categoryValue, moduleName, apiName, resultValue != FALSE ? 0 : lastError, detailText);
            ::SetLastError(lastError);
        }

#define APIMON_SIMPLE_BOOL_HOOK(HookName, OriginalName, CategoryValue, ModuleText, ApiText, ParamList, ArgList, DetailBlock) \
        BOOL WINAPI HookName ParamList \
        { \
            ScopedHookGuard guardValue; \
            if (guardValue.bypass()) { return OriginalName ArgList; } \
            const BOOL resultValue = OriginalName ArgList; \
            const DWORD lastError = ::GetLastError(); \
            wchar_t detailBuffer[ks::winapi_monitor::kMaxDetailChars] = {}; \
            DetailBlock; \
            EmitWin32BoolEvent(CategoryValue, ModuleText, ApiText, resultValue, lastError, detailBuffer); \
            return resultValue; \
        }

#define APIMON_SIMPLE_HANDLE_HOOK(ReturnType, HookName, OriginalName, CategoryValue, ModuleText, ApiText, ParamList, ArgList, DetailBlock) \
        ReturnType WINAPI HookName ParamList \
        { \
            ScopedHookGuard guardValue; \
            if (guardValue.bypass()) { return OriginalName ArgList; } \
            const ReturnType resultHandle = OriginalName ArgList; \
            const DWORD lastError = ::GetLastError(); \
            wchar_t detailBuffer[ks::winapi_monitor::kMaxDetailChars] = {}; \
            DetailBlock; \
            SendRawEventWithStatus(CategoryValue, ModuleText, ApiText, resultHandle != nullptr ? 0 : lastError, detailBuffer); \
            ::SetLastError(lastError); \
            return resultHandle; \
        }

#define APIMON_SIMPLE_DWORD_NONZERO_HOOK(HookName, OriginalName, CategoryValue, ModuleText, ApiText, ParamList, ArgList, DetailBlock) \
        DWORD WINAPI HookName ParamList \
        { \
            ScopedHookGuard guardValue; \
            if (guardValue.bypass()) { return OriginalName ArgList; } \
            const DWORD resultValue = OriginalName ArgList; \
            const DWORD lastError = ::GetLastError(); \
            wchar_t detailBuffer[ks::winapi_monitor::kMaxDetailChars] = {}; \
            DetailBlock; \
            SendRawEventWithStatus(CategoryValue, ModuleText, ApiText, resultValue != 0 ? 0 : lastError, detailBuffer); \
            ::SetLastError(lastError); \
            return resultValue; \
        }

#define APIMON_SIMPLE_UINT_NONZERO_HOOK(HookName, OriginalName, CategoryValue, ModuleText, ApiText, ParamList, ArgList, DetailBlock) \
        UINT WINAPI HookName ParamList \
        { \
            ScopedHookGuard guardValue; \
            if (guardValue.bypass()) { return OriginalName ArgList; } \
            const UINT resultValue = OriginalName ArgList; \
            const DWORD lastError = ::GetLastError(); \
            wchar_t detailBuffer[ks::winapi_monitor::kMaxDetailChars] = {}; \
            DetailBlock; \
            SendRawEventWithStatus(CategoryValue, ModuleText, ApiText, resultValue != 0 ? 0 : lastError, detailBuffer); \
            ::SetLastError(lastError); \
            return resultValue; \
        }

#define APIMON_SIMPLE_INT_POSITIVE_HOOK(HookName, OriginalName, CategoryValue, ModuleText, ApiText, ParamList, ArgList, DetailBlock) \
        int WINAPI HookName ParamList \
        { \
            ScopedHookGuard guardValue; \
            if (guardValue.bypass()) { return OriginalName ArgList; } \
            const int resultValue = OriginalName ArgList; \
            const DWORD lastError = ::GetLastError(); \
            wchar_t detailBuffer[ks::winapi_monitor::kMaxDetailChars] = {}; \
            DetailBlock; \
            SendRawEventWithStatus(CategoryValue, ModuleText, ApiText, resultValue > 0 ? 0 : lastError, detailBuffer); \
            ::SetLastError(lastError); \
            return resultValue; \
        }

#define APIMON_SIMPLE_ULONG_STATUS_HOOK(HookName, OriginalName, CategoryValue, ModuleText, ApiText, ParamList, ArgList, DetailBlock) \
        ULONG WINAPI HookName ParamList \
        { \
            ScopedHookGuard guardValue; \
            if (guardValue.bypass()) { return OriginalName ArgList; } \
            const ULONG statusValue = OriginalName ArgList; \
            wchar_t detailBuffer[ks::winapi_monitor::kMaxDetailChars] = {}; \
            DetailBlock; \
            SendRawEventWithStatus(CategoryValue, ModuleText, ApiText, static_cast<std::int32_t>(statusValue), detailBuffer); \
            return statusValue; \
        }

#define APIMON_SIMPLE_LONG_STATUS_HOOK(HookName, OriginalName, CategoryValue, ModuleText, ApiText, ParamList, ArgList, DetailBlock) \
        LONG WINAPI HookName ParamList \
        { \
            ScopedHookGuard guardValue; \
            if (guardValue.bypass()) { return OriginalName ArgList; } \
            const LONG statusValue = OriginalName ArgList; \
            wchar_t detailBuffer[ks::winapi_monitor::kMaxDetailChars] = {}; \
            DetailBlock; \
            SendRawEventWithStatus(CategoryValue, ModuleText, ApiText, static_cast<std::int32_t>(statusValue), detailBuffer); \
            return statusValue; \
        }

#define APIMON_SIMPLE_SECURITY_STATUS_HOOK(HookName, OriginalName, CategoryValue, ModuleText, ApiText, ParamList, ArgList, DetailBlock) \
        SECURITY_STATUS WINAPI HookName ParamList \
        { \
            ScopedHookGuard guardValue; \
            if (guardValue.bypass()) { return OriginalName ArgList; } \
            const SECURITY_STATUS statusValue = OriginalName ArgList; \
            wchar_t detailBuffer[ks::winapi_monitor::kMaxDetailChars] = {}; \
            DetailBlock; \
            SendRawEventWithStatus(CategoryValue, ModuleText, ApiText, static_cast<std::int32_t>(statusValue), detailBuffer); \
            return statusValue; \
        }

#define APIMON_SIMPLE_RPC_STATUS_HOOK(HookName, OriginalName, CategoryValue, ModuleText, ApiText, ParamList, ArgList, DetailBlock) \
        RPC_STATUS RPC_ENTRY HookName ParamList \
        { \
            ScopedHookGuard guardValue; \
            if (guardValue.bypass()) { return OriginalName ArgList; } \
            const RPC_STATUS statusValue = OriginalName ArgList; \
            wchar_t detailBuffer[ks::winapi_monitor::kMaxDetailChars] = {}; \
            DetailBlock; \
            SendRawEventWithStatus(CategoryValue, ModuleText, ApiText, static_cast<std::int32_t>(statusValue), detailBuffer); \
            return statusValue; \
        }

#define APIMON_SIMPLE_VOID_HOOK(HookName, OriginalName, CategoryValue, ModuleText, ApiText, ParamList, ArgList, DetailBlock) \
        void WINAPI HookName ParamList \
        { \
            ScopedHookGuard guardValue; \
            if (guardValue.bypass()) { OriginalName ArgList; return; } \
            OriginalName ArgList; \
            wchar_t detailBuffer[ks::winapi_monitor::kMaxDetailChars] = {}; \
            DetailBlock; \
            SendRawEventWithStatus(CategoryValue, ModuleText, ApiText, 0, detailBuffer); \
        }

        // HookedCloseHandle 作用：
        // - 输入：任意 Win32 HANDLE；
        // - 处理：记录句柄关闭，补齐对象生命周期尾部，便于和 Open/Create/Duplicate 类事件串联；
        // - 返回：保持 CloseHandle 原始 BOOL 结果，并恢复 LastError。
        APIMON_SIMPLE_BOOL_HOOK(HookedCloseHandle, g_closeHandleOriginal, ks::winapi_monitor::EventCategory::Process, L"KernelBase", L"CloseHandle",
            (HANDLE handleValue), (handleValue),
            { BuildSimpleHandleDetail(detailBuffer, L"handle", handleValue); })

        // HookedDuplicateHandle 作用：
        // - 输入：源/目标进程、源句柄、目标输出句柄、访问掩码和选项；
        // - 处理：记录跨进程句柄复制，覆盖提权、注入和句柄窃取常见前置动作；
        // - 返回：保持 DuplicateHandle 原始 BOOL 结果，并恢复 LastError。
        APIMON_SIMPLE_BOOL_HOOK(HookedDuplicateHandle, g_duplicateHandleOriginal, ks::winapi_monitor::EventCategory::Process, L"KernelBase", L"DuplicateHandle",
            (HANDLE sourceProcessHandle, HANDLE sourceHandle, HANDLE targetProcessHandle, LPHANDLE targetHandlePointer, DWORD desiredAccess, BOOL inheritHandle, DWORD optionsValue),
            (sourceProcessHandle, sourceHandle, targetProcessHandle, targetHandlePointer, desiredAccess, inheritHandle, optionsValue),
            { AppendWideText(detailBuffer, L"srcProc="); AppendHexText(detailBuffer, reinterpret_cast<std::uint64_t>(sourceProcessHandle)); AppendWideText(detailBuffer, L" src="); AppendHexText(detailBuffer, reinterpret_cast<std::uint64_t>(sourceHandle)); AppendWideText(detailBuffer, L" dstProc="); AppendHexText(detailBuffer, reinterpret_cast<std::uint64_t>(targetProcessHandle)); AppendWideText(detailBuffer, L" dst="); AppendHexText(detailBuffer, targetHandlePointer != nullptr ? reinterpret_cast<std::uint64_t>(*targetHandlePointer) : 0); AppendWideText(detailBuffer, L" access="); AppendHexText(detailBuffer, desiredAccess); AppendWideText(detailBuffer, L" inherit="); AppendUnsignedText(detailBuffer, inheritHandle != FALSE ? 1ULL : 0ULL); AppendWideText(detailBuffer, L" options="); AppendHexText(detailBuffer, optionsValue); })

        // HookedCreateFileMappingW 作用：
        // - 输入：文件句柄、保护属性、大小和映射名；
        // - 处理：记录 section/file mapping 创建，补齐 MapViewOfFile 与 NtCreateSection 之间的 Win32 层；
        // - 返回：保持 CreateFileMappingW 原始 HANDLE，并恢复 LastError。
        APIMON_SIMPLE_HANDLE_HOOK(HANDLE, HookedCreateFileMappingW, g_createFileMappingWOriginal, ks::winapi_monitor::EventCategory::File, L"KernelBase", L"CreateFileMappingW",
            (HANDLE fileHandle, LPSECURITY_ATTRIBUTES securityAttributes, DWORD protectValue, DWORD maximumSizeHigh, DWORD maximumSizeLow, LPCWSTR namePointer),
            (fileHandle, securityAttributes, protectValue, maximumSizeHigh, maximumSizeLow, namePointer),
            { AppendWideText(detailBuffer, L"file="); AppendHexText(detailBuffer, reinterpret_cast<std::uint64_t>(fileHandle)); AppendWideText(detailBuffer, L" protect="); AppendHexText(detailBuffer, protectValue); AppendWideText(detailBuffer, L" size="); AppendHexText(detailBuffer, (static_cast<std::uint64_t>(maximumSizeHigh) << 32) | maximumSizeLow); AppendWideText(detailBuffer, L" name="); AppendWideText(detailBuffer, namePointer); })

        // HookedCreateFileMappingA 作用：
        // - 输入：ANSI 映射名版本 CreateFileMapping；
        // - 处理：记录文件映射创建参数，映射名按窄字符扩展；
        // - 返回：保持原始 HANDLE，并恢复 LastError。
        APIMON_SIMPLE_HANDLE_HOOK(HANDLE, HookedCreateFileMappingA, g_createFileMappingAOriginal, ks::winapi_monitor::EventCategory::File, L"KernelBase", L"CreateFileMappingA",
            (HANDLE fileHandle, LPSECURITY_ATTRIBUTES securityAttributes, DWORD protectValue, DWORD maximumSizeHigh, DWORD maximumSizeLow, LPCSTR namePointer),
            (fileHandle, securityAttributes, protectValue, maximumSizeHigh, maximumSizeLow, namePointer),
            { AppendWideText(detailBuffer, L"file="); AppendHexText(detailBuffer, reinterpret_cast<std::uint64_t>(fileHandle)); AppendWideText(detailBuffer, L" protect="); AppendHexText(detailBuffer, protectValue); AppendWideText(detailBuffer, L" size="); AppendHexText(detailBuffer, (static_cast<std::uint64_t>(maximumSizeHigh) << 32) | maximumSizeLow); AppendWideText(detailBuffer, L" name="); AppendAnsiText(detailBuffer, namePointer); })

        // HookedOpenFileMappingW/A 作用：记录命名 mapping 打开；返回原始 HANDLE。
        APIMON_SIMPLE_HANDLE_HOOK(HANDLE, HookedOpenFileMappingW, g_openFileMappingWOriginal, ks::winapi_monitor::EventCategory::File, L"KernelBase", L"OpenFileMappingW",
            (DWORD desiredAccess, BOOL inheritHandle, LPCWSTR namePointer), (desiredAccess, inheritHandle, namePointer),
            { AppendWideText(detailBuffer, L"access="); AppendHexText(detailBuffer, desiredAccess); AppendWideText(detailBuffer, L" inherit="); AppendUnsignedText(detailBuffer, inheritHandle != FALSE ? 1ULL : 0ULL); AppendWideText(detailBuffer, L" name="); AppendWideText(detailBuffer, namePointer); })
        APIMON_SIMPLE_HANDLE_HOOK(HANDLE, HookedOpenFileMappingA, g_openFileMappingAOriginal, ks::winapi_monitor::EventCategory::File, L"KernelBase", L"OpenFileMappingA",
            (DWORD desiredAccess, BOOL inheritHandle, LPCSTR namePointer), (desiredAccess, inheritHandle, namePointer),
            { AppendWideText(detailBuffer, L"access="); AppendHexText(detailBuffer, desiredAccess); AppendWideText(detailBuffer, L" inherit="); AppendUnsignedText(detailBuffer, inheritHandle != FALSE ? 1ULL : 0ULL); AppendWideText(detailBuffer, L" name="); AppendAnsiText(detailBuffer, namePointer); })

        // HookedMapViewOfFile/Ex 作用：记录映射视图落点和大小；返回原始基址指针。
        APIMON_SIMPLE_HANDLE_HOOK(LPVOID, HookedMapViewOfFile, g_mapViewOfFileOriginal, ks::winapi_monitor::EventCategory::Process, L"KernelBase", L"MapViewOfFile",
            (HANDLE mappingHandle, DWORD desiredAccess, DWORD offsetHigh, DWORD offsetLow, SIZE_T bytesToMap), (mappingHandle, desiredAccess, offsetHigh, offsetLow, bytesToMap),
            { AppendWideText(detailBuffer, L"mapping="); AppendHexText(detailBuffer, reinterpret_cast<std::uint64_t>(mappingHandle)); AppendWideText(detailBuffer, L" access="); AppendHexText(detailBuffer, desiredAccess); AppendWideText(detailBuffer, L" offset="); AppendHexText(detailBuffer, (static_cast<std::uint64_t>(offsetHigh) << 32) | offsetLow); AppendWideText(detailBuffer, L" bytes="); AppendUnsignedText(detailBuffer, static_cast<unsigned long long>(bytesToMap)); AppendWideText(detailBuffer, L" base="); AppendHexText(detailBuffer, reinterpret_cast<std::uint64_t>(resultHandle)); })
        APIMON_SIMPLE_HANDLE_HOOK(LPVOID, HookedMapViewOfFileEx, g_mapViewOfFileExOriginal, ks::winapi_monitor::EventCategory::Process, L"KernelBase", L"MapViewOfFileEx",
            (HANDLE mappingHandle, DWORD desiredAccess, DWORD offsetHigh, DWORD offsetLow, SIZE_T bytesToMap, LPVOID baseAddress), (mappingHandle, desiredAccess, offsetHigh, offsetLow, bytesToMap, baseAddress),
            { AppendWideText(detailBuffer, L"mapping="); AppendHexText(detailBuffer, reinterpret_cast<std::uint64_t>(mappingHandle)); AppendWideText(detailBuffer, L" access="); AppendHexText(detailBuffer, desiredAccess); AppendWideText(detailBuffer, L" hint="); AppendHexText(detailBuffer, reinterpret_cast<std::uint64_t>(baseAddress)); AppendWideText(detailBuffer, L" bytes="); AppendUnsignedText(detailBuffer, static_cast<unsigned long long>(bytesToMap)); AppendWideText(detailBuffer, L" base="); AppendHexText(detailBuffer, reinterpret_cast<std::uint64_t>(resultHandle)); })

        // HookedUnmapViewOfFile/FlushViewOfFile 作用：记录映射视图生命周期和回写动作；返回原始 BOOL。
        APIMON_SIMPLE_BOOL_HOOK(HookedUnmapViewOfFile, g_unmapViewOfFileOriginal, ks::winapi_monitor::EventCategory::Process, L"KernelBase", L"UnmapViewOfFile",
            (LPCVOID baseAddress), (baseAddress),
            { AppendWideText(detailBuffer, L"base="); AppendHexText(detailBuffer, reinterpret_cast<std::uint64_t>(baseAddress)); })
        APIMON_SIMPLE_BOOL_HOOK(HookedFlushViewOfFile, g_flushViewOfFileOriginal, ks::winapi_monitor::EventCategory::File, L"KernelBase", L"FlushViewOfFile",
            (LPCVOID baseAddress, SIZE_T bytesToFlush), (baseAddress, bytesToFlush),
            { AppendWideText(detailBuffer, L"base="); AppendHexText(detailBuffer, reinterpret_cast<std::uint64_t>(baseAddress)); AppendWideText(detailBuffer, L" bytes="); AppendUnsignedText(detailBuffer, static_cast<unsigned long long>(bytesToFlush)); })

#define APIMON_SIMPLE_LSTATUS_HOOK(HookName, OriginalName, ModuleText, ApiText, ParamList, ArgList, DetailBlock) \
        LSTATUS WINAPI HookName ParamList \
        { \
            ScopedHookGuard guardValue; \
            if (guardValue.bypass()) { return OriginalName ArgList; } \
            const LSTATUS statusValue = OriginalName ArgList; \
            wchar_t detailBuffer[ks::winapi_monitor::kMaxDetailChars] = {}; \
            DetailBlock; \
            SendRawEventWithStatus(ks::winapi_monitor::EventCategory::Registry, ModuleText, ApiText, statusValue, detailBuffer); \
            return statusValue; \
        }

#define APIMON_SIMPLE_NTSTATUS_HOOK(HookName, OriginalName, CategoryValue, ModuleText, ApiText, ParamList, ArgList, DetailBlock) \
        NTSTATUS NTAPI HookName ParamList \
        { \
            ScopedHookGuard guardValue; \
            if (guardValue.bypass()) { return OriginalName ArgList; } \
            const NTSTATUS statusValue = OriginalName ArgList; \
            wchar_t detailBuffer[ks::winapi_monitor::kMaxDetailChars] = {}; \
            DetailBlock; \
            SendRawEventWithStatus(CategoryValue, ModuleText, ApiText, statusValue, detailBuffer); \
            return statusValue; \
        }

        // HookedFreeLibrary/GetProcAddress/LdrGetProcedureAddress 作用：
        // - 输入：加载器卸载、导出解析和 ntdll 层过程解析参数；
        // - 处理：记录动态解析链路，弥补只看目标 API 调用但看不到“解析意图”的缺口；
        // - 返回：保持原始返回值和 LastError/NTSTATUS。
        APIMON_SIMPLE_BOOL_HOOK(HookedFreeLibrary, g_freeLibraryOriginal, ks::winapi_monitor::EventCategory::Loader, L"Kernel32", L"FreeLibrary",
            (HMODULE moduleHandle), (moduleHandle),
            { AppendWideText(detailBuffer, L"module="); AppendHexText(detailBuffer, reinterpret_cast<std::uint64_t>(moduleHandle)); })

        FARPROC WINAPI HookedGetProcAddress(HMODULE moduleHandle, LPCSTR procNamePointer)
        {
            ScopedHookGuard guardValue;
            if (guardValue.bypass()) { return g_getProcAddressOriginal(moduleHandle, procNamePointer); }
            const FARPROC resultPointer = g_getProcAddressOriginal(moduleHandle, procNamePointer);
            const DWORD lastError = ::GetLastError();
            wchar_t detailBuffer[ks::winapi_monitor::kMaxDetailChars] = {};
            AppendWideText(detailBuffer, L"module=");
            AppendHexText(detailBuffer, reinterpret_cast<std::uint64_t>(moduleHandle));
            AppendWideText(detailBuffer, L" ");
            AppendProcNameText(detailBuffer, procNamePointer);
            AppendWideText(detailBuffer, L" address=");
            AppendHexText(detailBuffer, reinterpret_cast<std::uint64_t>(resultPointer));
            SendRawEventWithStatus(ks::winapi_monitor::EventCategory::Loader, L"Kernel32", L"GetProcAddress", resultPointer != nullptr ? 0 : lastError, detailBuffer);
            ::SetLastError(lastError);
            return resultPointer;
        }

        NTSTATUS NTAPI HookedLdrGetProcedureAddress(HMODULE moduleHandle, PANSI_STRING procNamePointer, WORD ordinalValue, PVOID* functionPointer)
        {
            ScopedHookGuard guardValue;
            if (guardValue.bypass()) { return g_ldrGetProcedureAddressOriginal(moduleHandle, procNamePointer, ordinalValue, functionPointer); }
            const NTSTATUS statusValue = g_ldrGetProcedureAddressOriginal(moduleHandle, procNamePointer, ordinalValue, functionPointer);
            wchar_t detailBuffer[ks::winapi_monitor::kMaxDetailChars] = {};
            AppendWideText(detailBuffer, L"module=");
            AppendHexText(detailBuffer, reinterpret_cast<std::uint64_t>(moduleHandle));
            AppendWideText(detailBuffer, L" name=");
            AppendAnsiStringText(detailBuffer, procNamePointer);
            AppendWideText(detailBuffer, L" ordinal=");
            AppendUnsignedText(detailBuffer, ordinalValue);
            AppendWideText(detailBuffer, L" address=");
            AppendHexText(detailBuffer, functionPointer != nullptr ? reinterpret_cast<std::uint64_t>(*functionPointer) : 0);
            SendRawEventWithStatus(ks::winapi_monitor::EventCategory::Loader, L"ntdll", L"LdrGetProcedureAddress", statusValue, detailBuffer);
            return statusValue;
        }

        // HookedReg*Transacted/Restore/Unload/Notify 作用：补齐 advapi32 注册表变体；返回原始 LSTATUS。
        APIMON_SIMPLE_LSTATUS_HOOK(HookedRegCreateKeyTransactedW, g_regCreateKeyTransactedWOriginal, L"Advapi32", L"RegCreateKeyTransactedW",
            (HKEY rootKey, LPCWSTR subKeyPointer, DWORD reservedValue, LPWSTR classPointer, DWORD optionsValue, REGSAM samDesired, const LPSECURITY_ATTRIBUTES securityAttributes, PHKEY resultKeyPointer, LPDWORD dispositionPointer, HANDLE transactionHandle, PVOID extendedParameter),
            (rootKey, subKeyPointer, reservedValue, classPointer, optionsValue, samDesired, securityAttributes, resultKeyPointer, dispositionPointer, transactionHandle, extendedParameter),
            { BuildRegCreateDetail(detailBuffer, rootKey, subKeyPointer, optionsValue, dispositionPointer != nullptr ? *dispositionPointer : 0); AppendWideText(detailBuffer, L" transaction="); AppendHexText(detailBuffer, reinterpret_cast<std::uint64_t>(transactionHandle)); AppendWideText(detailBuffer, L" hkeyOut="); AppendHexText(detailBuffer, resultKeyPointer != nullptr ? reinterpret_cast<std::uint64_t>(*resultKeyPointer) : 0); })
        APIMON_SIMPLE_LSTATUS_HOOK(HookedRegCreateKeyTransactedA, g_regCreateKeyTransactedAOriginal, L"Advapi32", L"RegCreateKeyTransactedA",
            (HKEY rootKey, LPCSTR subKeyPointer, DWORD reservedValue, LPSTR classPointer, DWORD optionsValue, REGSAM samDesired, const LPSECURITY_ATTRIBUTES securityAttributes, PHKEY resultKeyPointer, LPDWORD dispositionPointer, HANDLE transactionHandle, PVOID extendedParameter),
            (rootKey, subKeyPointer, reservedValue, classPointer, optionsValue, samDesired, securityAttributes, resultKeyPointer, dispositionPointer, transactionHandle, extendedParameter),
            { BuildRegCreateDetailA(detailBuffer, rootKey, subKeyPointer, optionsValue, dispositionPointer != nullptr ? *dispositionPointer : 0); AppendWideText(detailBuffer, L" transaction="); AppendHexText(detailBuffer, reinterpret_cast<std::uint64_t>(transactionHandle)); AppendWideText(detailBuffer, L" hkeyOut="); AppendHexText(detailBuffer, resultKeyPointer != nullptr ? reinterpret_cast<std::uint64_t>(*resultKeyPointer) : 0); })
        APIMON_SIMPLE_LSTATUS_HOOK(HookedRegOpenKeyTransactedW, g_regOpenKeyTransactedWOriginal, L"Advapi32", L"RegOpenKeyTransactedW",
            (HKEY rootKey, LPCWSTR subKeyPointer, DWORD optionsValue, REGSAM samDesired, PHKEY resultKeyPointer, HANDLE transactionHandle, PVOID extendedParameter),
            (rootKey, subKeyPointer, optionsValue, samDesired, resultKeyPointer, transactionHandle, extendedParameter),
            { BuildRegOpenDetail(detailBuffer, rootKey, subKeyPointer, samDesired); AppendWideText(detailBuffer, L" transaction="); AppendHexText(detailBuffer, reinterpret_cast<std::uint64_t>(transactionHandle)); AppendWideText(detailBuffer, L" hkeyOut="); AppendHexText(detailBuffer, resultKeyPointer != nullptr ? reinterpret_cast<std::uint64_t>(*resultKeyPointer) : 0); })
        APIMON_SIMPLE_LSTATUS_HOOK(HookedRegOpenKeyTransactedA, g_regOpenKeyTransactedAOriginal, L"Advapi32", L"RegOpenKeyTransactedA",
            (HKEY rootKey, LPCSTR subKeyPointer, DWORD optionsValue, REGSAM samDesired, PHKEY resultKeyPointer, HANDLE transactionHandle, PVOID extendedParameter),
            (rootKey, subKeyPointer, optionsValue, samDesired, resultKeyPointer, transactionHandle, extendedParameter),
            { BuildRegOpenDetailA(detailBuffer, rootKey, subKeyPointer, samDesired); AppendWideText(detailBuffer, L" transaction="); AppendHexText(detailBuffer, reinterpret_cast<std::uint64_t>(transactionHandle)); AppendWideText(detailBuffer, L" hkeyOut="); AppendHexText(detailBuffer, resultKeyPointer != nullptr ? reinterpret_cast<std::uint64_t>(*resultKeyPointer) : 0); })
        APIMON_SIMPLE_LSTATUS_HOOK(HookedRegDeleteKeyTransactedW, g_regDeleteKeyTransactedWOriginal, L"Advapi32", L"RegDeleteKeyTransactedW",
            (HKEY rootKey, LPCWSTR subKeyPointer, REGSAM viewValue, DWORD reservedValue, HANDLE transactionHandle, PVOID extendedParameter),
            (rootKey, subKeyPointer, viewValue, reservedValue, transactionHandle, extendedParameter),
            { BuildRegSubKeyDetail(detailBuffer, rootKey, subKeyPointer, viewValue); AppendWideText(detailBuffer, L" transaction="); AppendHexText(detailBuffer, reinterpret_cast<std::uint64_t>(transactionHandle)); })
        APIMON_SIMPLE_LSTATUS_HOOK(HookedRegDeleteKeyTransactedA, g_regDeleteKeyTransactedAOriginal, L"Advapi32", L"RegDeleteKeyTransactedA",
            (HKEY rootKey, LPCSTR subKeyPointer, REGSAM viewValue, DWORD reservedValue, HANDLE transactionHandle, PVOID extendedParameter),
            (rootKey, subKeyPointer, viewValue, reservedValue, transactionHandle, extendedParameter),
            { BuildRegSubKeyDetailA(detailBuffer, rootKey, subKeyPointer, viewValue); AppendWideText(detailBuffer, L" transaction="); AppendHexText(detailBuffer, reinterpret_cast<std::uint64_t>(transactionHandle)); })

        APIMON_SIMPLE_LSTATUS_HOOK(HookedRegReplaceKeyW, g_regReplaceKeyWOriginal, L"Advapi32", L"RegReplaceKeyW",
            (HKEY rootKey, LPCWSTR subKeyPointer, LPCWSTR newFilePointer, LPCWSTR oldFilePointer), (rootKey, subKeyPointer, newFilePointer, oldFilePointer),
            { BuildRegSubKeyDetail(detailBuffer, rootKey, subKeyPointer, 0); AppendWideText(detailBuffer, L" newFile="); AppendWideText(detailBuffer, newFilePointer); AppendWideText(detailBuffer, L" oldFile="); AppendWideText(detailBuffer, oldFilePointer); })
        APIMON_SIMPLE_LSTATUS_HOOK(HookedRegReplaceKeyA, g_regReplaceKeyAOriginal, L"Advapi32", L"RegReplaceKeyA",
            (HKEY rootKey, LPCSTR subKeyPointer, LPCSTR newFilePointer, LPCSTR oldFilePointer), (rootKey, subKeyPointer, newFilePointer, oldFilePointer),
            { BuildRegSubKeyDetailA(detailBuffer, rootKey, subKeyPointer, 0); AppendWideText(detailBuffer, L" newFile="); AppendAnsiText(detailBuffer, newFilePointer); AppendWideText(detailBuffer, L" oldFile="); AppendAnsiText(detailBuffer, oldFilePointer); })
        APIMON_SIMPLE_LSTATUS_HOOK(HookedRegRestoreKeyW, g_regRestoreKeyWOriginal, L"Advapi32", L"RegRestoreKeyW",
            (HKEY keyHandle, LPCWSTR filePointer, DWORD flagsValue), (keyHandle, filePointer, flagsValue),
            { AppendWideText(detailBuffer, L"hkey="); AppendHexText(detailBuffer, reinterpret_cast<std::uint64_t>(keyHandle)); AppendWideText(detailBuffer, L" file="); AppendWideText(detailBuffer, filePointer); AppendWideText(detailBuffer, L" flags="); AppendHexText(detailBuffer, flagsValue); })
        APIMON_SIMPLE_LSTATUS_HOOK(HookedRegRestoreKeyA, g_regRestoreKeyAOriginal, L"Advapi32", L"RegRestoreKeyA",
            (HKEY keyHandle, LPCSTR filePointer, DWORD flagsValue), (keyHandle, filePointer, flagsValue),
            { AppendWideText(detailBuffer, L"hkey="); AppendHexText(detailBuffer, reinterpret_cast<std::uint64_t>(keyHandle)); AppendWideText(detailBuffer, L" file="); AppendAnsiText(detailBuffer, filePointer); AppendWideText(detailBuffer, L" flags="); AppendHexText(detailBuffer, flagsValue); })
        APIMON_SIMPLE_LSTATUS_HOOK(HookedRegUnLoadKeyW, g_regUnLoadKeyWOriginal, L"Advapi32", L"RegUnLoadKeyW",
            (HKEY rootKey, LPCWSTR subKeyPointer), (rootKey, subKeyPointer),
            { BuildRegSubKeyDetail(detailBuffer, rootKey, subKeyPointer, 0); })
        APIMON_SIMPLE_LSTATUS_HOOK(HookedRegUnLoadKeyA, g_regUnLoadKeyAOriginal, L"Advapi32", L"RegUnLoadKeyA",
            (HKEY rootKey, LPCSTR subKeyPointer), (rootKey, subKeyPointer),
            { BuildRegSubKeyDetailA(detailBuffer, rootKey, subKeyPointer, 0); })
        APIMON_SIMPLE_LSTATUS_HOOK(HookedRegLoadAppKeyW, g_regLoadAppKeyWOriginal, L"Advapi32", L"RegLoadAppKeyW",
            (LPCWSTR filePointer, PHKEY resultKeyPointer, REGSAM samDesired, DWORD optionsValue, DWORD reservedValue), (filePointer, resultKeyPointer, samDesired, optionsValue, reservedValue),
            { AppendWideText(detailBuffer, L"file="); AppendWideText(detailBuffer, filePointer); AppendWideText(detailBuffer, L" sam="); AppendHexText(detailBuffer, samDesired); AppendWideText(detailBuffer, L" options="); AppendHexText(detailBuffer, optionsValue); AppendWideText(detailBuffer, L" hkeyOut="); AppendHexText(detailBuffer, resultKeyPointer != nullptr ? reinterpret_cast<std::uint64_t>(*resultKeyPointer) : 0); })
        APIMON_SIMPLE_LSTATUS_HOOK(HookedRegLoadAppKeyA, g_regLoadAppKeyAOriginal, L"Advapi32", L"RegLoadAppKeyA",
            (LPCSTR filePointer, PHKEY resultKeyPointer, REGSAM samDesired, DWORD optionsValue, DWORD reservedValue), (filePointer, resultKeyPointer, samDesired, optionsValue, reservedValue),
            { AppendWideText(detailBuffer, L"file="); AppendAnsiText(detailBuffer, filePointer); AppendWideText(detailBuffer, L" sam="); AppendHexText(detailBuffer, samDesired); AppendWideText(detailBuffer, L" options="); AppendHexText(detailBuffer, optionsValue); AppendWideText(detailBuffer, L" hkeyOut="); AppendHexText(detailBuffer, resultKeyPointer != nullptr ? reinterpret_cast<std::uint64_t>(*resultKeyPointer) : 0); })
        APIMON_SIMPLE_LSTATUS_HOOK(HookedRegNotifyChangeKeyValue, g_regNotifyChangeKeyValueOriginal, L"Advapi32", L"RegNotifyChangeKeyValue",
            (HKEY keyHandle, BOOL watchSubtree, DWORD notifyFilter, HANDLE eventHandle, BOOL asynchronous), (keyHandle, watchSubtree, notifyFilter, eventHandle, asynchronous),
            { AppendWideText(detailBuffer, L"hkey="); AppendHexText(detailBuffer, reinterpret_cast<std::uint64_t>(keyHandle)); AppendWideText(detailBuffer, L" subtree="); AppendUnsignedText(detailBuffer, watchSubtree != FALSE ? 1ULL : 0ULL); AppendWideText(detailBuffer, L" filter="); AppendHexText(detailBuffer, notifyFilter); AppendWideText(detailBuffer, L" event="); AppendHexText(detailBuffer, reinterpret_cast<std::uint64_t>(eventHandle)); AppendWideText(detailBuffer, L" async="); AppendUnsignedText(detailBuffer, asynchronous != FALSE ? 1ULL : 0ULL); })

        // HookedNtClose/native registry variants 作用：补齐 ntdll 句柄关闭和高级注册表 native API；返回原始 NTSTATUS。
        APIMON_SIMPLE_NTSTATUS_HOOK(HookedNtClose, g_ntCloseOriginal, ks::winapi_monitor::EventCategory::Process, L"ntdll", L"NtClose",
            (HANDLE handleValue), (handleValue),
            { BuildSimpleHandleDetail(detailBuffer, L"handle", handleValue); })
        APIMON_SIMPLE_NTSTATUS_HOOK(HookedNtCreateKeyTransacted, g_ntCreateKeyTransactedOriginal, ks::winapi_monitor::EventCategory::Registry, L"ntdll", L"NtCreateKeyTransacted",
            (PHANDLE keyHandlePointer, ACCESS_MASK desiredAccess, POBJECT_ATTRIBUTES objectAttributes, ULONG titleIndex, PUNICODE_STRING classPointer, ULONG createOptions, HANDLE transactionHandle, PULONG dispositionPointer),
            (keyHandlePointer, desiredAccess, objectAttributes, titleIndex, classPointer, createOptions, transactionHandle, dispositionPointer),
            { AppendWideText(detailBuffer, L"key="); AppendObjectNameText(detailBuffer, objectAttributes); AppendWideText(detailBuffer, L" access="); AppendHexText(detailBuffer, desiredAccess); AppendWideText(detailBuffer, L" options="); AppendHexText(detailBuffer, createOptions); AppendWideText(detailBuffer, L" transaction="); AppendHexText(detailBuffer, reinterpret_cast<std::uint64_t>(transactionHandle)); AppendWideText(detailBuffer, L" hkeyOut="); AppendHexText(detailBuffer, keyHandlePointer != nullptr ? reinterpret_cast<std::uint64_t>(*keyHandlePointer) : 0); })
        APIMON_SIMPLE_NTSTATUS_HOOK(HookedNtOpenKeyTransacted, g_ntOpenKeyTransactedOriginal, ks::winapi_monitor::EventCategory::Registry, L"ntdll", L"NtOpenKeyTransacted",
            (PHANDLE keyHandlePointer, ACCESS_MASK desiredAccess, POBJECT_ATTRIBUTES objectAttributes, HANDLE transactionHandle),
            (keyHandlePointer, desiredAccess, objectAttributes, transactionHandle),
            { AppendWideText(detailBuffer, L"key="); AppendObjectNameText(detailBuffer, objectAttributes); AppendWideText(detailBuffer, L" access="); AppendHexText(detailBuffer, desiredAccess); AppendWideText(detailBuffer, L" transaction="); AppendHexText(detailBuffer, reinterpret_cast<std::uint64_t>(transactionHandle)); AppendWideText(detailBuffer, L" hkeyOut="); AppendHexText(detailBuffer, keyHandlePointer != nullptr ? reinterpret_cast<std::uint64_t>(*keyHandlePointer) : 0); })
        APIMON_SIMPLE_NTSTATUS_HOOK(HookedNtOpenKeyTransactedEx, g_ntOpenKeyTransactedExOriginal, ks::winapi_monitor::EventCategory::Registry, L"ntdll", L"NtOpenKeyTransactedEx",
            (PHANDLE keyHandlePointer, ACCESS_MASK desiredAccess, POBJECT_ATTRIBUTES objectAttributes, ULONG openOptions, HANDLE transactionHandle),
            (keyHandlePointer, desiredAccess, objectAttributes, openOptions, transactionHandle),
            { AppendWideText(detailBuffer, L"key="); AppendObjectNameText(detailBuffer, objectAttributes); AppendWideText(detailBuffer, L" access="); AppendHexText(detailBuffer, desiredAccess); AppendWideText(detailBuffer, L" options="); AppendHexText(detailBuffer, openOptions); AppendWideText(detailBuffer, L" transaction="); AppendHexText(detailBuffer, reinterpret_cast<std::uint64_t>(transactionHandle)); })
        APIMON_SIMPLE_NTSTATUS_HOOK(HookedNtReplaceKey, g_ntReplaceKeyOriginal, ks::winapi_monitor::EventCategory::Registry, L"ntdll", L"NtReplaceKey",
            (POBJECT_ATTRIBUTES newFileObjectAttributes, HANDLE targetHandle, POBJECT_ATTRIBUTES oldFileObjectAttributes),
            (newFileObjectAttributes, targetHandle, oldFileObjectAttributes),
            { AppendWideText(detailBuffer, L"target="); AppendHexText(detailBuffer, reinterpret_cast<std::uint64_t>(targetHandle)); AppendWideText(detailBuffer, L" newFile="); AppendObjectNameText(detailBuffer, newFileObjectAttributes); AppendWideText(detailBuffer, L" oldFile="); AppendObjectNameText(detailBuffer, oldFileObjectAttributes); })
        APIMON_SIMPLE_NTSTATUS_HOOK(HookedNtRestoreKey, g_ntRestoreKeyOriginal, ks::winapi_monitor::EventCategory::Registry, L"ntdll", L"NtRestoreKey",
            (HANDLE keyHandle, HANDLE fileHandle, ULONG flagsValue), (keyHandle, fileHandle, flagsValue),
            { AppendWideText(detailBuffer, L"hkey="); AppendHexText(detailBuffer, reinterpret_cast<std::uint64_t>(keyHandle)); AppendWideText(detailBuffer, L" file="); AppendHexText(detailBuffer, reinterpret_cast<std::uint64_t>(fileHandle)); AppendWideText(detailBuffer, L" flags="); AppendHexText(detailBuffer, flagsValue); })
        APIMON_SIMPLE_NTSTATUS_HOOK(HookedNtUnloadKey, g_ntUnloadKeyOriginal, ks::winapi_monitor::EventCategory::Registry, L"ntdll", L"NtUnloadKey",
            (POBJECT_ATTRIBUTES targetKey), (targetKey),
            { AppendWideText(detailBuffer, L"key="); AppendObjectNameText(detailBuffer, targetKey); })
        APIMON_SIMPLE_NTSTATUS_HOOK(HookedNtUnloadKey2, g_ntUnloadKey2Original, ks::winapi_monitor::EventCategory::Registry, L"ntdll", L"NtUnloadKey2",
            (POBJECT_ATTRIBUTES targetKey, ULONG flagsValue), (targetKey, flagsValue),
            { AppendWideText(detailBuffer, L"key="); AppendObjectNameText(detailBuffer, targetKey); AppendWideText(detailBuffer, L" flags="); AppendHexText(detailBuffer, flagsValue); })
        APIMON_SIMPLE_NTSTATUS_HOOK(HookedNtUnloadKeyEx, g_ntUnloadKeyExOriginal, ks::winapi_monitor::EventCategory::Registry, L"ntdll", L"NtUnloadKeyEx",
            (POBJECT_ATTRIBUTES targetKey, HANDLE eventHandle), (targetKey, eventHandle),
            { AppendWideText(detailBuffer, L"key="); AppendObjectNameText(detailBuffer, targetKey); AppendWideText(detailBuffer, L" event="); AppendHexText(detailBuffer, reinterpret_cast<std::uint64_t>(eventHandle)); })

        // HookedToken* 作用：
        // - 输入：Token 打开、复制、提权和带 Token 创建进程的关键参数；
        // - 处理：补齐权限提升、令牌窃取和用户上下文切换行为的 Win32 层观测；
        // - 返回：保持原始 BOOL 结果，并恢复 LastError。
        APIMON_SIMPLE_BOOL_HOOK(HookedOpenProcessToken, g_openProcessTokenOriginal, ks::winapi_monitor::EventCategory::Process, L"Advapi32", L"OpenProcessToken",
            (HANDLE processHandle, DWORD desiredAccess, PHANDLE tokenHandlePointer), (processHandle, desiredAccess, tokenHandlePointer),
            { AppendWideText(detailBuffer, L"process="); AppendHexText(detailBuffer, reinterpret_cast<std::uint64_t>(processHandle)); AppendWideText(detailBuffer, L" access="); AppendHexText(detailBuffer, desiredAccess); AppendWideText(detailBuffer, L" token="); AppendHexText(detailBuffer, tokenHandlePointer != nullptr ? reinterpret_cast<std::uint64_t>(*tokenHandlePointer) : 0); })
        APIMON_SIMPLE_BOOL_HOOK(HookedOpenThreadToken, g_openThreadTokenOriginal, ks::winapi_monitor::EventCategory::Process, L"Advapi32", L"OpenThreadToken",
            (HANDLE threadHandle, DWORD desiredAccess, BOOL openAsSelf, PHANDLE tokenHandlePointer), (threadHandle, desiredAccess, openAsSelf, tokenHandlePointer),
            { AppendWideText(detailBuffer, L"thread="); AppendHexText(detailBuffer, reinterpret_cast<std::uint64_t>(threadHandle)); AppendWideText(detailBuffer, L" access="); AppendHexText(detailBuffer, desiredAccess); AppendWideText(detailBuffer, L" openAsSelf="); AppendUnsignedText(detailBuffer, openAsSelf != FALSE ? 1ULL : 0ULL); AppendWideText(detailBuffer, L" token="); AppendHexText(detailBuffer, tokenHandlePointer != nullptr ? reinterpret_cast<std::uint64_t>(*tokenHandlePointer) : 0); })
        APIMON_SIMPLE_BOOL_HOOK(HookedAdjustTokenPrivileges, g_adjustTokenPrivilegesOriginal, ks::winapi_monitor::EventCategory::Process, L"Advapi32", L"AdjustTokenPrivileges",
            (HANDLE tokenHandle, BOOL disableAllPrivileges, PTOKEN_PRIVILEGES newStatePointer, DWORD bufferLength, PTOKEN_PRIVILEGES previousStatePointer, PDWORD returnLengthPointer),
            (tokenHandle, disableAllPrivileges, newStatePointer, bufferLength, previousStatePointer, returnLengthPointer),
            { AppendWideText(detailBuffer, L"token="); AppendHexText(detailBuffer, reinterpret_cast<std::uint64_t>(tokenHandle)); AppendWideText(detailBuffer, L" disableAll="); AppendUnsignedText(detailBuffer, disableAllPrivileges != FALSE ? 1ULL : 0ULL); AppendWideText(detailBuffer, L" privilegeCount="); AppendUnsignedText(detailBuffer, newStatePointer != nullptr ? newStatePointer->PrivilegeCount : 0); AppendWideText(detailBuffer, L" returnLen="); AppendUnsignedText(detailBuffer, returnLengthPointer != nullptr ? *returnLengthPointer : 0); })
        APIMON_SIMPLE_BOOL_HOOK(HookedDuplicateToken, g_duplicateTokenOriginal, ks::winapi_monitor::EventCategory::Process, L"Advapi32", L"DuplicateToken",
            (HANDLE existingTokenHandle, SECURITY_IMPERSONATION_LEVEL impersonationLevel, PHANDLE newTokenHandlePointer), (existingTokenHandle, impersonationLevel, newTokenHandlePointer),
            { AppendWideText(detailBuffer, L"token="); AppendHexText(detailBuffer, reinterpret_cast<std::uint64_t>(existingTokenHandle)); AppendWideText(detailBuffer, L" level="); AppendUnsignedText(detailBuffer, static_cast<unsigned long long>(impersonationLevel)); AppendWideText(detailBuffer, L" newToken="); AppendHexText(detailBuffer, newTokenHandlePointer != nullptr ? reinterpret_cast<std::uint64_t>(*newTokenHandlePointer) : 0); })
        APIMON_SIMPLE_BOOL_HOOK(HookedDuplicateTokenEx, g_duplicateTokenExOriginal, ks::winapi_monitor::EventCategory::Process, L"Advapi32", L"DuplicateTokenEx",
            (HANDLE existingTokenHandle, DWORD desiredAccess, LPSECURITY_ATTRIBUTES tokenAttributes, SECURITY_IMPERSONATION_LEVEL impersonationLevel, TOKEN_TYPE tokenType, PHANDLE newTokenHandlePointer),
            (existingTokenHandle, desiredAccess, tokenAttributes, impersonationLevel, tokenType, newTokenHandlePointer),
            { AppendWideText(detailBuffer, L"token="); AppendHexText(detailBuffer, reinterpret_cast<std::uint64_t>(existingTokenHandle)); AppendWideText(detailBuffer, L" access="); AppendHexText(detailBuffer, desiredAccess); AppendWideText(detailBuffer, L" level="); AppendUnsignedText(detailBuffer, static_cast<unsigned long long>(impersonationLevel)); AppendWideText(detailBuffer, L" type="); AppendUnsignedText(detailBuffer, static_cast<unsigned long long>(tokenType)); AppendWideText(detailBuffer, L" newToken="); AppendHexText(detailBuffer, newTokenHandlePointer != nullptr ? reinterpret_cast<std::uint64_t>(*newTokenHandlePointer) : 0); })
        APIMON_SIMPLE_BOOL_HOOK(HookedCreateProcessAsUserW, g_createProcessAsUserWOriginal, ks::winapi_monitor::EventCategory::Process, L"Advapi32", L"CreateProcessAsUserW",
            (HANDLE tokenHandle, LPCWSTR applicationNamePointer, LPWSTR commandLinePointer, LPSECURITY_ATTRIBUTES processAttributes, LPSECURITY_ATTRIBUTES threadAttributes, BOOL inheritHandles, DWORD creationFlags, LPVOID environmentPointer, LPCWSTR currentDirectoryPointer, LPSTARTUPINFOW startupInfoPointer, LPPROCESS_INFORMATION processInformationPointer),
            (tokenHandle, applicationNamePointer, commandLinePointer, processAttributes, threadAttributes, inheritHandles, creationFlags, environmentPointer, currentDirectoryPointer, startupInfoPointer, processInformationPointer),
            { AppendWideText(detailBuffer, L"token="); AppendHexText(detailBuffer, reinterpret_cast<std::uint64_t>(tokenHandle)); AppendWideText(detailBuffer, L" app="); AppendWideText(detailBuffer, applicationNamePointer); AppendWideText(detailBuffer, L" cmd="); AppendWideText(detailBuffer, commandLinePointer); AppendWideText(detailBuffer, L" flags="); AppendHexText(detailBuffer, creationFlags); AppendWideText(detailBuffer, L" childPid="); AppendUnsignedText(detailBuffer, processInformationPointer != nullptr ? processInformationPointer->dwProcessId : 0); })
        APIMON_SIMPLE_BOOL_HOOK(HookedCreateProcessAsUserA, g_createProcessAsUserAOriginal, ks::winapi_monitor::EventCategory::Process, L"Advapi32", L"CreateProcessAsUserA",
            (HANDLE tokenHandle, LPCSTR applicationNamePointer, LPSTR commandLinePointer, LPSECURITY_ATTRIBUTES processAttributes, LPSECURITY_ATTRIBUTES threadAttributes, BOOL inheritHandles, DWORD creationFlags, LPVOID environmentPointer, LPCSTR currentDirectoryPointer, LPSTARTUPINFOA startupInfoPointer, LPPROCESS_INFORMATION processInformationPointer),
            (tokenHandle, applicationNamePointer, commandLinePointer, processAttributes, threadAttributes, inheritHandles, creationFlags, environmentPointer, currentDirectoryPointer, startupInfoPointer, processInformationPointer),
            { AppendWideText(detailBuffer, L"token="); AppendHexText(detailBuffer, reinterpret_cast<std::uint64_t>(tokenHandle)); AppendWideText(detailBuffer, L" app="); AppendAnsiText(detailBuffer, applicationNamePointer); AppendWideText(detailBuffer, L" cmd="); AppendAnsiText(detailBuffer, commandLinePointer); AppendWideText(detailBuffer, L" flags="); AppendHexText(detailBuffer, creationFlags); AppendWideText(detailBuffer, L" childPid="); AppendUnsignedText(detailBuffer, processInformationPointer != nullptr ? processInformationPointer->dwProcessId : 0); })
        APIMON_SIMPLE_BOOL_HOOK(HookedCreateProcessWithTokenW, g_createProcessWithTokenWOriginal, ks::winapi_monitor::EventCategory::Process, L"Advapi32", L"CreateProcessWithTokenW",
            (HANDLE tokenHandle, DWORD logonFlags, LPCWSTR applicationNamePointer, LPWSTR commandLinePointer, DWORD creationFlags, LPVOID environmentPointer, LPCWSTR currentDirectoryPointer, LPSTARTUPINFOW startupInfoPointer, LPPROCESS_INFORMATION processInformationPointer),
            (tokenHandle, logonFlags, applicationNamePointer, commandLinePointer, creationFlags, environmentPointer, currentDirectoryPointer, startupInfoPointer, processInformationPointer),
            { AppendWideText(detailBuffer, L"token="); AppendHexText(detailBuffer, reinterpret_cast<std::uint64_t>(tokenHandle)); AppendWideText(detailBuffer, L" logonFlags="); AppendHexText(detailBuffer, logonFlags); AppendWideText(detailBuffer, L" app="); AppendWideText(detailBuffer, applicationNamePointer); AppendWideText(detailBuffer, L" cmd="); AppendWideText(detailBuffer, commandLinePointer); AppendWideText(detailBuffer, L" childPid="); AppendUnsignedText(detailBuffer, processInformationPointer != nullptr ? processInformationPointer->dwProcessId : 0); })
        APIMON_SIMPLE_BOOL_HOOK(HookedLookupPrivilegeValueW, g_lookupPrivilegeValueWOriginal, ks::winapi_monitor::EventCategory::Process, L"Advapi32", L"LookupPrivilegeValueW",
            (LPCWSTR systemNamePointer, LPCWSTR namePointer, PLUID luidPointer), (systemNamePointer, namePointer, luidPointer),
            { AppendWideText(detailBuffer, L"system="); AppendWideText(detailBuffer, systemNamePointer); AppendWideText(detailBuffer, L" name="); AppendWideText(detailBuffer, namePointer); AppendWideText(detailBuffer, L" luidLow="); AppendHexText(detailBuffer, luidPointer != nullptr ? luidPointer->LowPart : 0); })
        APIMON_SIMPLE_BOOL_HOOK(HookedLookupPrivilegeValueA, g_lookupPrivilegeValueAOriginal, ks::winapi_monitor::EventCategory::Process, L"Advapi32", L"LookupPrivilegeValueA",
            (LPCSTR systemNamePointer, LPCSTR namePointer, PLUID luidPointer), (systemNamePointer, namePointer, luidPointer),
            { AppendWideText(detailBuffer, L"system="); AppendAnsiText(detailBuffer, systemNamePointer); AppendWideText(detailBuffer, L" name="); AppendAnsiText(detailBuffer, namePointer); AppendWideText(detailBuffer, L" luidLow="); AppendHexText(detailBuffer, luidPointer != nullptr ? luidPointer->LowPart : 0); })

        // HookedService* 作用：
        // - 输入：SCM/Service 打开、创建、配置、启动、控制、删除和关闭参数；
        // - 处理：补齐服务安装与驱动服务控制路径的 Advapi32 层观测；
        // - 返回：保持原始 SC_HANDLE/BOOL 结果，并恢复 LastError。
        APIMON_SIMPLE_HANDLE_HOOK(SC_HANDLE, HookedOpenSCManagerW, g_openSCManagerWOriginal, ks::winapi_monitor::EventCategory::Process, L"Advapi32", L"OpenSCManagerW",
            (LPCWSTR machineNamePointer, LPCWSTR databaseNamePointer, DWORD desiredAccess), (machineNamePointer, databaseNamePointer, desiredAccess),
            { AppendWideText(detailBuffer, L"machine="); AppendWideText(detailBuffer, machineNamePointer); AppendWideText(detailBuffer, L" database="); AppendWideText(detailBuffer, databaseNamePointer); AppendWideText(detailBuffer, L" access="); AppendHexText(detailBuffer, desiredAccess); AppendWideText(detailBuffer, L" handle="); AppendHexText(detailBuffer, reinterpret_cast<std::uint64_t>(resultHandle)); })
        APIMON_SIMPLE_HANDLE_HOOK(SC_HANDLE, HookedOpenSCManagerA, g_openSCManagerAOriginal, ks::winapi_monitor::EventCategory::Process, L"Advapi32", L"OpenSCManagerA",
            (LPCSTR machineNamePointer, LPCSTR databaseNamePointer, DWORD desiredAccess), (machineNamePointer, databaseNamePointer, desiredAccess),
            { AppendWideText(detailBuffer, L"machine="); AppendAnsiText(detailBuffer, machineNamePointer); AppendWideText(detailBuffer, L" database="); AppendAnsiText(detailBuffer, databaseNamePointer); AppendWideText(detailBuffer, L" access="); AppendHexText(detailBuffer, desiredAccess); AppendWideText(detailBuffer, L" handle="); AppendHexText(detailBuffer, reinterpret_cast<std::uint64_t>(resultHandle)); })
        APIMON_SIMPLE_HANDLE_HOOK(SC_HANDLE, HookedOpenServiceW, g_openServiceWOriginal, ks::winapi_monitor::EventCategory::Process, L"Advapi32", L"OpenServiceW",
            (SC_HANDLE managerHandle, LPCWSTR serviceNamePointer, DWORD desiredAccess), (managerHandle, serviceNamePointer, desiredAccess),
            { AppendWideText(detailBuffer, L"scm="); AppendHexText(detailBuffer, reinterpret_cast<std::uint64_t>(managerHandle)); AppendWideText(detailBuffer, L" service="); AppendWideText(detailBuffer, serviceNamePointer); AppendWideText(detailBuffer, L" access="); AppendHexText(detailBuffer, desiredAccess); AppendWideText(detailBuffer, L" handle="); AppendHexText(detailBuffer, reinterpret_cast<std::uint64_t>(resultHandle)); })
        APIMON_SIMPLE_HANDLE_HOOK(SC_HANDLE, HookedOpenServiceA, g_openServiceAOriginal, ks::winapi_monitor::EventCategory::Process, L"Advapi32", L"OpenServiceA",
            (SC_HANDLE managerHandle, LPCSTR serviceNamePointer, DWORD desiredAccess), (managerHandle, serviceNamePointer, desiredAccess),
            { AppendWideText(detailBuffer, L"scm="); AppendHexText(detailBuffer, reinterpret_cast<std::uint64_t>(managerHandle)); AppendWideText(detailBuffer, L" service="); AppendAnsiText(detailBuffer, serviceNamePointer); AppendWideText(detailBuffer, L" access="); AppendHexText(detailBuffer, desiredAccess); AppendWideText(detailBuffer, L" handle="); AppendHexText(detailBuffer, reinterpret_cast<std::uint64_t>(resultHandle)); })
        APIMON_SIMPLE_HANDLE_HOOK(SC_HANDLE, HookedCreateServiceW, g_createServiceWOriginal, ks::winapi_monitor::EventCategory::Process, L"Advapi32", L"CreateServiceW",
            (SC_HANDLE managerHandle, LPCWSTR serviceNamePointer, LPCWSTR displayNamePointer, DWORD desiredAccess, DWORD serviceType, DWORD startType, DWORD errorControl, LPCWSTR binaryPathPointer, LPCWSTR loadOrderGroupPointer, LPDWORD tagIdPointer, LPCWSTR dependenciesPointer, LPCWSTR serviceStartNamePointer, LPCWSTR passwordPointer),
            (managerHandle, serviceNamePointer, displayNamePointer, desiredAccess, serviceType, startType, errorControl, binaryPathPointer, loadOrderGroupPointer, tagIdPointer, dependenciesPointer, serviceStartNamePointer, passwordPointer),
            { AppendWideText(detailBuffer, L"service="); AppendWideText(detailBuffer, serviceNamePointer); AppendWideText(detailBuffer, L" type="); AppendHexText(detailBuffer, serviceType); AppendWideText(detailBuffer, L" start="); AppendHexText(detailBuffer, startType); AppendWideText(detailBuffer, L" path="); AppendWideText(detailBuffer, binaryPathPointer); AppendWideText(detailBuffer, L" handle="); AppendHexText(detailBuffer, reinterpret_cast<std::uint64_t>(resultHandle)); })
        APIMON_SIMPLE_HANDLE_HOOK(SC_HANDLE, HookedCreateServiceA, g_createServiceAOriginal, ks::winapi_monitor::EventCategory::Process, L"Advapi32", L"CreateServiceA",
            (SC_HANDLE managerHandle, LPCSTR serviceNamePointer, LPCSTR displayNamePointer, DWORD desiredAccess, DWORD serviceType, DWORD startType, DWORD errorControl, LPCSTR binaryPathPointer, LPCSTR loadOrderGroupPointer, LPDWORD tagIdPointer, LPCSTR dependenciesPointer, LPCSTR serviceStartNamePointer, LPCSTR passwordPointer),
            (managerHandle, serviceNamePointer, displayNamePointer, desiredAccess, serviceType, startType, errorControl, binaryPathPointer, loadOrderGroupPointer, tagIdPointer, dependenciesPointer, serviceStartNamePointer, passwordPointer),
            { AppendWideText(detailBuffer, L"service="); AppendAnsiText(detailBuffer, serviceNamePointer); AppendWideText(detailBuffer, L" type="); AppendHexText(detailBuffer, serviceType); AppendWideText(detailBuffer, L" start="); AppendHexText(detailBuffer, startType); AppendWideText(detailBuffer, L" path="); AppendAnsiText(detailBuffer, binaryPathPointer); AppendWideText(detailBuffer, L" handle="); AppendHexText(detailBuffer, reinterpret_cast<std::uint64_t>(resultHandle)); })
        APIMON_SIMPLE_BOOL_HOOK(HookedChangeServiceConfigW, g_changeServiceConfigWOriginal, ks::winapi_monitor::EventCategory::Process, L"Advapi32", L"ChangeServiceConfigW",
            (SC_HANDLE serviceHandle, DWORD serviceType, DWORD startType, DWORD errorControl, LPCWSTR binaryPathPointer, LPCWSTR loadOrderGroupPointer, LPDWORD tagIdPointer, LPCWSTR dependenciesPointer, LPCWSTR serviceStartNamePointer, LPCWSTR passwordPointer, LPCWSTR displayNamePointer),
            (serviceHandle, serviceType, startType, errorControl, binaryPathPointer, loadOrderGroupPointer, tagIdPointer, dependenciesPointer, serviceStartNamePointer, passwordPointer, displayNamePointer),
            { AppendWideText(detailBuffer, L"service="); AppendHexText(detailBuffer, reinterpret_cast<std::uint64_t>(serviceHandle)); AppendWideText(detailBuffer, L" type="); AppendHexText(detailBuffer, serviceType); AppendWideText(detailBuffer, L" start="); AppendHexText(detailBuffer, startType); AppendWideText(detailBuffer, L" path="); AppendWideText(detailBuffer, binaryPathPointer); })
        APIMON_SIMPLE_BOOL_HOOK(HookedChangeServiceConfigA, g_changeServiceConfigAOriginal, ks::winapi_monitor::EventCategory::Process, L"Advapi32", L"ChangeServiceConfigA",
            (SC_HANDLE serviceHandle, DWORD serviceType, DWORD startType, DWORD errorControl, LPCSTR binaryPathPointer, LPCSTR loadOrderGroupPointer, LPDWORD tagIdPointer, LPCSTR dependenciesPointer, LPCSTR serviceStartNamePointer, LPCSTR passwordPointer, LPCSTR displayNamePointer),
            (serviceHandle, serviceType, startType, errorControl, binaryPathPointer, loadOrderGroupPointer, tagIdPointer, dependenciesPointer, serviceStartNamePointer, passwordPointer, displayNamePointer),
            { AppendWideText(detailBuffer, L"service="); AppendHexText(detailBuffer, reinterpret_cast<std::uint64_t>(serviceHandle)); AppendWideText(detailBuffer, L" type="); AppendHexText(detailBuffer, serviceType); AppendWideText(detailBuffer, L" start="); AppendHexText(detailBuffer, startType); AppendWideText(detailBuffer, L" path="); AppendAnsiText(detailBuffer, binaryPathPointer); })
        APIMON_SIMPLE_BOOL_HOOK(HookedStartServiceW, g_startServiceWOriginal, ks::winapi_monitor::EventCategory::Process, L"Advapi32", L"StartServiceW",
            (SC_HANDLE serviceHandle, DWORD argumentCount, LPCWSTR* argumentVector), (serviceHandle, argumentCount, argumentVector),
            { AppendWideText(detailBuffer, L"service="); AppendHexText(detailBuffer, reinterpret_cast<std::uint64_t>(serviceHandle)); AppendWideText(detailBuffer, L" argc="); AppendUnsignedText(detailBuffer, argumentCount); })
        APIMON_SIMPLE_BOOL_HOOK(HookedStartServiceA, g_startServiceAOriginal, ks::winapi_monitor::EventCategory::Process, L"Advapi32", L"StartServiceA",
            (SC_HANDLE serviceHandle, DWORD argumentCount, LPCSTR* argumentVector), (serviceHandle, argumentCount, argumentVector),
            { AppendWideText(detailBuffer, L"service="); AppendHexText(detailBuffer, reinterpret_cast<std::uint64_t>(serviceHandle)); AppendWideText(detailBuffer, L" argc="); AppendUnsignedText(detailBuffer, argumentCount); })
        APIMON_SIMPLE_BOOL_HOOK(HookedControlService, g_controlServiceOriginal, ks::winapi_monitor::EventCategory::Process, L"Advapi32", L"ControlService",
            (SC_HANDLE serviceHandle, DWORD controlCode, LPSERVICE_STATUS serviceStatusPointer), (serviceHandle, controlCode, serviceStatusPointer),
            { AppendWideText(detailBuffer, L"service="); AppendHexText(detailBuffer, reinterpret_cast<std::uint64_t>(serviceHandle)); AppendWideText(detailBuffer, L" control="); AppendHexText(detailBuffer, controlCode); })
        APIMON_SIMPLE_BOOL_HOOK(HookedDeleteService, g_deleteServiceOriginal, ks::winapi_monitor::EventCategory::Process, L"Advapi32", L"DeleteService",
            (SC_HANDLE serviceHandle), (serviceHandle),
            { BuildSimpleHandleDetail(detailBuffer, L"service", serviceHandle); })
        APIMON_SIMPLE_BOOL_HOOK(HookedCloseServiceHandle, g_closeServiceHandleOriginal, ks::winapi_monitor::EventCategory::Process, L"Advapi32", L"CloseServiceHandle",
            (SC_HANDLE serviceHandle), (serviceHandle),
            { BuildSimpleHandleDetail(detailBuffer, L"service", serviceHandle); })

#define APIMON_SIMPLE_WSA_INT_HOOK(HookName, OriginalName, ApiText, SuccessExpression, ParamList, ArgList, DetailBlock) \
        int WSAAPI HookName ParamList \
        { \
            ScopedHookGuard guardValue; \
            if (guardValue.bypass()) { return OriginalName ArgList; } \
            const int resultValue = OriginalName ArgList; \
            const int errorValue = (SuccessExpression) ? 0 : ::WSAGetLastError(); \
            wchar_t detailBuffer[ks::winapi_monitor::kMaxDetailChars] = {}; \
            DetailBlock; \
            SendMonitorEventRaw(ks::winapi_monitor::EventCategory::Network, L"Ws2_32", ApiText, errorValue, detailBuffer); \
            if (!(SuccessExpression)) { ::WSASetLastError(errorValue); } \
            return resultValue; \
        }

        // HookedWs2Extended 作用：
        // - 输入：扩展 Winsock 控制、面向地址的 WSASendTo/WSARecvFrom、名称解析参数；
        // - 处理：补齐基础 send/recv/connect 之外的高频网络路径；
        // - 返回：保持原始 Winsock 返回值，并恢复 WSA 错误码。
        APIMON_SIMPLE_WSA_INT_HOOK(HookedWSAIoctl, g_wsaIoctlOriginal, L"WSAIoctl", resultValue == 0,
            (SOCKET socketValue, DWORD ioControlCode, LPVOID inBufferPointer, DWORD inBufferSize, LPVOID outBufferPointer, DWORD outBufferSize, LPDWORD bytesReturnedPointer, LPWSAOVERLAPPED overlappedPointer, LPWSAOVERLAPPED_COMPLETION_ROUTINE completionRoutinePointer),
            (socketValue, ioControlCode, inBufferPointer, inBufferSize, outBufferPointer, outBufferSize, bytesReturnedPointer, overlappedPointer, completionRoutinePointer),
            { AppendWideText(detailBuffer, L"socket="); AppendHexText(detailBuffer, static_cast<std::uint64_t>(socketValue)); AppendWideText(detailBuffer, L" code="); AppendHexText(detailBuffer, ioControlCode); AppendWideText(detailBuffer, L" in="); AppendUnsignedText(detailBuffer, inBufferSize); AppendWideText(detailBuffer, L" out="); AppendUnsignedText(detailBuffer, outBufferSize); AppendWideText(detailBuffer, L" returned="); AppendUnsignedText(detailBuffer, bytesReturnedPointer != nullptr ? *bytesReturnedPointer : 0); })
        APIMON_SIMPLE_WSA_INT_HOOK(HookedWSASendTo, g_wsaSendToOriginal, L"WSASendTo", resultValue == 0,
            (SOCKET socketValue, LPWSABUF buffersPointer, DWORD bufferCount, LPDWORD bytesSentPointer, DWORD flagsValue, const sockaddr* toPointer, int toLength, LPWSAOVERLAPPED overlappedPointer, LPWSAOVERLAPPED_COMPLETION_ROUTINE completionRoutinePointer),
            (socketValue, buffersPointer, bufferCount, bytesSentPointer, flagsValue, toPointer, toLength, overlappedPointer, completionRoutinePointer),
            { BuildSocketDetail(detailBuffer, L"sendto", socketValue, SumWsaBufferLength(buffersPointer, bufferCount), bytesSentPointer != nullptr ? static_cast<int>(*bytesSentPointer) : 0, flagsValue, toPointer, toLength); })
        APIMON_SIMPLE_WSA_INT_HOOK(HookedWSARecvFrom, g_wsaRecvFromOriginal, L"WSARecvFrom", resultValue == 0,
            (SOCKET socketValue, LPWSABUF buffersPointer, DWORD bufferCount, LPDWORD bytesReceivedPointer, LPDWORD flagsPointer, sockaddr* fromPointer, LPINT fromLengthPointer, LPWSAOVERLAPPED overlappedPointer, LPWSAOVERLAPPED_COMPLETION_ROUTINE completionRoutinePointer),
            (socketValue, buffersPointer, bufferCount, bytesReceivedPointer, flagsPointer, fromPointer, fromLengthPointer, overlappedPointer, completionRoutinePointer),
            { BuildSocketDetail(detailBuffer, L"recvfrom", socketValue, SumWsaBufferLength(buffersPointer, bufferCount), bytesReceivedPointer != nullptr ? static_cast<int>(*bytesReceivedPointer) : 0, flagsPointer != nullptr ? *flagsPointer : 0, fromPointer, fromLengthPointer != nullptr ? *fromLengthPointer : 0); })
        APIMON_SIMPLE_WSA_INT_HOOK(HookedGetAddrInfoW, g_getAddrInfoWOriginal, L"GetAddrInfoW", resultValue == 0,
            (PCWSTR nodeNamePointer, PCWSTR serviceNamePointer, const ADDRINFOW* hintsPointer, PADDRINFOW* resultPointer), (nodeNamePointer, serviceNamePointer, hintsPointer, resultPointer),
            { AppendWideText(detailBuffer, L"node="); AppendWideText(detailBuffer, nodeNamePointer); AppendWideText(detailBuffer, L" service="); AppendWideText(detailBuffer, serviceNamePointer); AppendWideText(detailBuffer, L" result="); AppendHexText(detailBuffer, resultPointer != nullptr ? reinterpret_cast<std::uint64_t>(*resultPointer) : 0); })
        APIMON_SIMPLE_WSA_INT_HOOK(HookedGetAddrInfoA, g_getAddrInfoAOriginal, L"getaddrinfo", resultValue == 0,
            (PCSTR nodeNamePointer, PCSTR serviceNamePointer, const ADDRINFOA* hintsPointer, PADDRINFOA* resultPointer), (nodeNamePointer, serviceNamePointer, hintsPointer, resultPointer),
            { AppendWideText(detailBuffer, L"node="); AppendAnsiText(detailBuffer, nodeNamePointer); AppendWideText(detailBuffer, L" service="); AppendAnsiText(detailBuffer, serviceNamePointer); AppendWideText(detailBuffer, L" result="); AppendHexText(detailBuffer, resultPointer != nullptr ? reinterpret_cast<std::uint64_t>(*resultPointer) : 0); })

        // HookedDnsQueryW/A 作用：记录 DNSAPI 显式解析请求；返回原始 DNS_STATUS。
        DNS_STATUS WINAPI HookedDnsQueryW(PCWSTR namePointer, WORD typeValue, DWORD optionsValue, PVOID extraPointer, PDNS_RECORDW* queryResultsPointer, PVOID* reservedPointer)
        {
            ScopedHookGuard guardValue;
            if (guardValue.bypass()) { return g_dnsQueryWOriginal(namePointer, typeValue, optionsValue, extraPointer, queryResultsPointer, reservedPointer); }
            const DNS_STATUS statusValue = g_dnsQueryWOriginal(namePointer, typeValue, optionsValue, extraPointer, queryResultsPointer, reservedPointer);
            wchar_t detailBuffer[ks::winapi_monitor::kMaxDetailChars] = {};
            AppendWideText(detailBuffer, L"name=");
            AppendWideText(detailBuffer, namePointer);
            AppendWideText(detailBuffer, L" type=");
            AppendUnsignedText(detailBuffer, typeValue);
            AppendWideText(detailBuffer, L" options=");
            AppendHexText(detailBuffer, optionsValue);
            SendRawEventWithStatus(ks::winapi_monitor::EventCategory::Network, L"Dnsapi", L"DnsQuery_W", statusValue, detailBuffer);
            return statusValue;
        }

        DNS_STATUS WINAPI HookedDnsQueryA(PCSTR namePointer, WORD typeValue, DWORD optionsValue, PVOID extraPointer, PDNS_RECORDA* queryResultsPointer, PVOID* reservedPointer)
        {
            ScopedHookGuard guardValue;
            if (guardValue.bypass()) { return g_dnsQueryAOriginal(namePointer, typeValue, optionsValue, extraPointer, queryResultsPointer, reservedPointer); }
            const DNS_STATUS statusValue = g_dnsQueryAOriginal(namePointer, typeValue, optionsValue, extraPointer, queryResultsPointer, reservedPointer);
            wchar_t detailBuffer[ks::winapi_monitor::kMaxDetailChars] = {};
            AppendWideText(detailBuffer, L"name=");
            AppendAnsiText(detailBuffer, namePointer);
            AppendWideText(detailBuffer, L" type=");
            AppendUnsignedText(detailBuffer, typeValue);
            AppendWideText(detailBuffer, L" options=");
            AppendHexText(detailBuffer, optionsValue);
            SendRawEventWithStatus(ks::winapi_monitor::EventCategory::Network, L"Dnsapi", L"DnsQuery_A", statusValue, detailBuffer);
            return statusValue;
        }

        // HookedWinHttp/WinInet 作用：
        // - 输入：HTTP session/connect/request/read/write/close 参数；
        // - 处理：补齐只 hook Winsock 时看不到 URL/host/verb 的高层网络语义；
        // - 返回：保持原始 HINTERNET/BOOL，并恢复 LastError。
        APIMON_SIMPLE_HANDLE_HOOK(HINTERNET, HookedWinHttpOpen, g_winHttpOpenOriginal, ks::winapi_monitor::EventCategory::Network, L"Winhttp", L"WinHttpOpen",
            (LPCWSTR userAgentPointer, DWORD accessType, LPCWSTR proxyNamePointer, LPCWSTR proxyBypassPointer, DWORD flagsValue), (userAgentPointer, accessType, proxyNamePointer, proxyBypassPointer, flagsValue),
            { AppendWideText(detailBuffer, L"agent="); AppendWideText(detailBuffer, userAgentPointer); AppendWideText(detailBuffer, L" accessType="); AppendUnsignedText(detailBuffer, accessType); AppendWideText(detailBuffer, L" flags="); AppendHexText(detailBuffer, flagsValue); AppendWideText(detailBuffer, L" handle="); AppendHexText(detailBuffer, reinterpret_cast<std::uint64_t>(resultHandle)); })
        APIMON_SIMPLE_HANDLE_HOOK(HINTERNET, HookedWinHttpConnect, g_winHttpConnectOriginal, ks::winapi_monitor::EventCategory::Network, L"Winhttp", L"WinHttpConnect",
            (HINTERNET sessionHandle, LPCWSTR serverNamePointer, INTERNET_PORT serverPort, DWORD reservedValue), (sessionHandle, serverNamePointer, serverPort, reservedValue),
            { AppendWideText(detailBuffer, L"session="); AppendHexText(detailBuffer, reinterpret_cast<std::uint64_t>(sessionHandle)); AppendWideText(detailBuffer, L" host="); AppendWideText(detailBuffer, serverNamePointer); AppendWideText(detailBuffer, L" port="); AppendUnsignedText(detailBuffer, serverPort); AppendWideText(detailBuffer, L" handle="); AppendHexText(detailBuffer, reinterpret_cast<std::uint64_t>(resultHandle)); })
        APIMON_SIMPLE_HANDLE_HOOK(HINTERNET, HookedWinHttpOpenRequest, g_winHttpOpenRequestOriginal, ks::winapi_monitor::EventCategory::Network, L"Winhttp", L"WinHttpOpenRequest",
            (HINTERNET connectHandle, LPCWSTR verbPointer, LPCWSTR objectNamePointer, LPCWSTR versionPointer, LPCWSTR referrerPointer, LPCWSTR* acceptTypesPointer, DWORD flagsValue),
            (connectHandle, verbPointer, objectNamePointer, versionPointer, referrerPointer, acceptTypesPointer, flagsValue),
            { AppendWideText(detailBuffer, L"connect="); AppendHexText(detailBuffer, reinterpret_cast<std::uint64_t>(connectHandle)); AppendWideText(detailBuffer, L" verb="); AppendWideText(detailBuffer, verbPointer); AppendWideText(detailBuffer, L" object="); AppendWideText(detailBuffer, objectNamePointer); AppendWideText(detailBuffer, L" flags="); AppendHexText(detailBuffer, flagsValue); AppendWideText(detailBuffer, L" handle="); AppendHexText(detailBuffer, reinterpret_cast<std::uint64_t>(resultHandle)); })
        APIMON_SIMPLE_BOOL_HOOK(HookedWinHttpSendRequest, g_winHttpSendRequestOriginal, ks::winapi_monitor::EventCategory::Network, L"Winhttp", L"WinHttpSendRequest",
            (HINTERNET requestHandle, LPCWSTR headersPointer, DWORD headersLength, LPVOID optionalPointer, DWORD optionalLength, DWORD totalLength, DWORD_PTR contextValue),
            (requestHandle, headersPointer, headersLength, optionalPointer, optionalLength, totalLength, contextValue),
            { AppendWideText(detailBuffer, L"request="); AppendHexText(detailBuffer, reinterpret_cast<std::uint64_t>(requestHandle)); AppendWideText(detailBuffer, L" headersLen="); AppendUnsignedText(detailBuffer, headersLength); AppendWideText(detailBuffer, L" optionalLen="); AppendUnsignedText(detailBuffer, optionalLength); AppendWideText(detailBuffer, L" total="); AppendUnsignedText(detailBuffer, totalLength); })
        APIMON_SIMPLE_BOOL_HOOK(HookedWinHttpReceiveResponse, g_winHttpReceiveResponseOriginal, ks::winapi_monitor::EventCategory::Network, L"Winhttp", L"WinHttpReceiveResponse",
            (HINTERNET requestHandle, LPVOID reservedPointer), (requestHandle, reservedPointer),
            { BuildSimpleHandleDetail(detailBuffer, L"request", requestHandle); })
        APIMON_SIMPLE_BOOL_HOOK(HookedWinHttpReadData, g_winHttpReadDataOriginal, ks::winapi_monitor::EventCategory::Network, L"Winhttp", L"WinHttpReadData",
            (HINTERNET requestHandle, LPVOID bufferPointer, DWORD bytesToRead, LPDWORD bytesReadPointer), (requestHandle, bufferPointer, bytesToRead, bytesReadPointer),
            { AppendWideText(detailBuffer, L"request="); AppendHexText(detailBuffer, reinterpret_cast<std::uint64_t>(requestHandle)); AppendWideText(detailBuffer, L" requestBytes="); AppendUnsignedText(detailBuffer, bytesToRead); AppendWideText(detailBuffer, L" read="); AppendUnsignedText(detailBuffer, bytesReadPointer != nullptr ? *bytesReadPointer : 0); })
        APIMON_SIMPLE_BOOL_HOOK(HookedWinHttpWriteData, g_winHttpWriteDataOriginal, ks::winapi_monitor::EventCategory::Network, L"Winhttp", L"WinHttpWriteData",
            (HINTERNET requestHandle, LPCVOID bufferPointer, DWORD bytesToWrite, LPDWORD bytesWrittenPointer), (requestHandle, bufferPointer, bytesToWrite, bytesWrittenPointer),
            { AppendWideText(detailBuffer, L"request="); AppendHexText(detailBuffer, reinterpret_cast<std::uint64_t>(requestHandle)); AppendWideText(detailBuffer, L" requestBytes="); AppendUnsignedText(detailBuffer, bytesToWrite); AppendWideText(detailBuffer, L" written="); AppendUnsignedText(detailBuffer, bytesWrittenPointer != nullptr ? *bytesWrittenPointer : 0); })
        APIMON_SIMPLE_BOOL_HOOK(HookedWinHttpCloseHandle, g_winHttpCloseHandleOriginal, ks::winapi_monitor::EventCategory::Network, L"Winhttp", L"WinHttpCloseHandle",
            (HINTERNET internetHandle), (internetHandle),
            { BuildSimpleHandleDetail(detailBuffer, L"handle", internetHandle); })

        APIMON_SIMPLE_HANDLE_HOOK(HINTERNET, HookedInternetOpenW, g_internetOpenWOriginal, ks::winapi_monitor::EventCategory::Network, L"Wininet", L"InternetOpenW",
            (LPCWSTR agentPointer, DWORD accessType, LPCWSTR proxyNamePointer, LPCWSTR proxyBypassPointer, DWORD flagsValue), (agentPointer, accessType, proxyNamePointer, proxyBypassPointer, flagsValue),
            { AppendWideText(detailBuffer, L"agent="); AppendWideText(detailBuffer, agentPointer); AppendWideText(detailBuffer, L" accessType="); AppendUnsignedText(detailBuffer, accessType); AppendWideText(detailBuffer, L" flags="); AppendHexText(detailBuffer, flagsValue); AppendWideText(detailBuffer, L" handle="); AppendHexText(detailBuffer, reinterpret_cast<std::uint64_t>(resultHandle)); })
        APIMON_SIMPLE_HANDLE_HOOK(HINTERNET, HookedInternetOpenA, g_internetOpenAOriginal, ks::winapi_monitor::EventCategory::Network, L"Wininet", L"InternetOpenA",
            (LPCSTR agentPointer, DWORD accessType, LPCSTR proxyNamePointer, LPCSTR proxyBypassPointer, DWORD flagsValue), (agentPointer, accessType, proxyNamePointer, proxyBypassPointer, flagsValue),
            { AppendWideText(detailBuffer, L"agent="); AppendAnsiText(detailBuffer, agentPointer); AppendWideText(detailBuffer, L" accessType="); AppendUnsignedText(detailBuffer, accessType); AppendWideText(detailBuffer, L" flags="); AppendHexText(detailBuffer, flagsValue); AppendWideText(detailBuffer, L" handle="); AppendHexText(detailBuffer, reinterpret_cast<std::uint64_t>(resultHandle)); })
        APIMON_SIMPLE_HANDLE_HOOK(HINTERNET, HookedInternetConnectW, g_internetConnectWOriginal, ks::winapi_monitor::EventCategory::Network, L"Wininet", L"InternetConnectW",
            (HINTERNET internetHandle, LPCWSTR serverNamePointer, INTERNET_PORT serverPort, LPCWSTR userNamePointer, LPCWSTR passwordPointer, DWORD serviceValue, DWORD flagsValue, DWORD_PTR contextValue),
            (internetHandle, serverNamePointer, serverPort, userNamePointer, passwordPointer, serviceValue, flagsValue, contextValue),
            { AppendWideText(detailBuffer, L"root="); AppendHexText(detailBuffer, reinterpret_cast<std::uint64_t>(internetHandle)); AppendWideText(detailBuffer, L" host="); AppendWideText(detailBuffer, serverNamePointer); AppendWideText(detailBuffer, L" port="); AppendUnsignedText(detailBuffer, serverPort); AppendWideText(detailBuffer, L" service="); AppendUnsignedText(detailBuffer, serviceValue); AppendWideText(detailBuffer, L" handle="); AppendHexText(detailBuffer, reinterpret_cast<std::uint64_t>(resultHandle)); })
        APIMON_SIMPLE_HANDLE_HOOK(HINTERNET, HookedInternetConnectA, g_internetConnectAOriginal, ks::winapi_monitor::EventCategory::Network, L"Wininet", L"InternetConnectA",
            (HINTERNET internetHandle, LPCSTR serverNamePointer, INTERNET_PORT serverPort, LPCSTR userNamePointer, LPCSTR passwordPointer, DWORD serviceValue, DWORD flagsValue, DWORD_PTR contextValue),
            (internetHandle, serverNamePointer, serverPort, userNamePointer, passwordPointer, serviceValue, flagsValue, contextValue),
            { AppendWideText(detailBuffer, L"root="); AppendHexText(detailBuffer, reinterpret_cast<std::uint64_t>(internetHandle)); AppendWideText(detailBuffer, L" host="); AppendAnsiText(detailBuffer, serverNamePointer); AppendWideText(detailBuffer, L" port="); AppendUnsignedText(detailBuffer, serverPort); AppendWideText(detailBuffer, L" service="); AppendUnsignedText(detailBuffer, serviceValue); AppendWideText(detailBuffer, L" handle="); AppendHexText(detailBuffer, reinterpret_cast<std::uint64_t>(resultHandle)); })
        APIMON_SIMPLE_HANDLE_HOOK(HINTERNET, HookedHttpOpenRequestW, g_httpOpenRequestWOriginal, ks::winapi_monitor::EventCategory::Network, L"Wininet", L"HttpOpenRequestW",
            (HINTERNET connectHandle, LPCWSTR verbPointer, LPCWSTR objectNamePointer, LPCWSTR versionPointer, LPCWSTR referrerPointer, LPCWSTR* acceptTypesPointer, DWORD flagsValue, DWORD_PTR contextValue),
            (connectHandle, verbPointer, objectNamePointer, versionPointer, referrerPointer, acceptTypesPointer, flagsValue, contextValue),
            { AppendWideText(detailBuffer, L"connect="); AppendHexText(detailBuffer, reinterpret_cast<std::uint64_t>(connectHandle)); AppendWideText(detailBuffer, L" verb="); AppendWideText(detailBuffer, verbPointer); AppendWideText(detailBuffer, L" object="); AppendWideText(detailBuffer, objectNamePointer); AppendWideText(detailBuffer, L" flags="); AppendHexText(detailBuffer, flagsValue); AppendWideText(detailBuffer, L" handle="); AppendHexText(detailBuffer, reinterpret_cast<std::uint64_t>(resultHandle)); })
        APIMON_SIMPLE_HANDLE_HOOK(HINTERNET, HookedHttpOpenRequestA, g_httpOpenRequestAOriginal, ks::winapi_monitor::EventCategory::Network, L"Wininet", L"HttpOpenRequestA",
            (HINTERNET connectHandle, LPCSTR verbPointer, LPCSTR objectNamePointer, LPCSTR versionPointer, LPCSTR referrerPointer, LPCSTR* acceptTypesPointer, DWORD flagsValue, DWORD_PTR contextValue),
            (connectHandle, verbPointer, objectNamePointer, versionPointer, referrerPointer, acceptTypesPointer, flagsValue, contextValue),
            { AppendWideText(detailBuffer, L"connect="); AppendHexText(detailBuffer, reinterpret_cast<std::uint64_t>(connectHandle)); AppendWideText(detailBuffer, L" verb="); AppendAnsiText(detailBuffer, verbPointer); AppendWideText(detailBuffer, L" object="); AppendAnsiText(detailBuffer, objectNamePointer); AppendWideText(detailBuffer, L" flags="); AppendHexText(detailBuffer, flagsValue); AppendWideText(detailBuffer, L" handle="); AppendHexText(detailBuffer, reinterpret_cast<std::uint64_t>(resultHandle)); })
        APIMON_SIMPLE_BOOL_HOOK(HookedHttpSendRequestW, g_httpSendRequestWOriginal, ks::winapi_monitor::EventCategory::Network, L"Wininet", L"HttpSendRequestW",
            (HINTERNET requestHandle, LPCWSTR headersPointer, DWORD headersLength, LPVOID optionalPointer, DWORD optionalLength), (requestHandle, headersPointer, headersLength, optionalPointer, optionalLength),
            { AppendWideText(detailBuffer, L"request="); AppendHexText(detailBuffer, reinterpret_cast<std::uint64_t>(requestHandle)); AppendWideText(detailBuffer, L" headersLen="); AppendUnsignedText(detailBuffer, headersLength); AppendWideText(detailBuffer, L" optionalLen="); AppendUnsignedText(detailBuffer, optionalLength); })
        APIMON_SIMPLE_BOOL_HOOK(HookedHttpSendRequestA, g_httpSendRequestAOriginal, ks::winapi_monitor::EventCategory::Network, L"Wininet", L"HttpSendRequestA",
            (HINTERNET requestHandle, LPCSTR headersPointer, DWORD headersLength, LPVOID optionalPointer, DWORD optionalLength), (requestHandle, headersPointer, headersLength, optionalPointer, optionalLength),
            { AppendWideText(detailBuffer, L"request="); AppendHexText(detailBuffer, reinterpret_cast<std::uint64_t>(requestHandle)); AppendWideText(detailBuffer, L" headersLen="); AppendUnsignedText(detailBuffer, headersLength); AppendWideText(detailBuffer, L" optionalLen="); AppendUnsignedText(detailBuffer, optionalLength); })
        APIMON_SIMPLE_BOOL_HOOK(HookedInternetReadFile, g_internetReadFileOriginal, ks::winapi_monitor::EventCategory::Network, L"Wininet", L"InternetReadFile",
            (HINTERNET fileHandle, LPVOID bufferPointer, DWORD bytesToRead, LPDWORD bytesReadPointer), (fileHandle, bufferPointer, bytesToRead, bytesReadPointer),
            { AppendWideText(detailBuffer, L"handle="); AppendHexText(detailBuffer, reinterpret_cast<std::uint64_t>(fileHandle)); AppendWideText(detailBuffer, L" requestBytes="); AppendUnsignedText(detailBuffer, bytesToRead); AppendWideText(detailBuffer, L" read="); AppendUnsignedText(detailBuffer, bytesReadPointer != nullptr ? *bytesReadPointer : 0); })
        APIMON_SIMPLE_BOOL_HOOK(HookedInternetWriteFile, g_internetWriteFileOriginal, ks::winapi_monitor::EventCategory::Network, L"Wininet", L"InternetWriteFile",
            (HINTERNET fileHandle, LPCVOID bufferPointer, DWORD bytesToWrite, LPDWORD bytesWrittenPointer), (fileHandle, bufferPointer, bytesToWrite, bytesWrittenPointer),
            { AppendWideText(detailBuffer, L"handle="); AppendHexText(detailBuffer, reinterpret_cast<std::uint64_t>(fileHandle)); AppendWideText(detailBuffer, L" requestBytes="); AppendUnsignedText(detailBuffer, bytesToWrite); AppendWideText(detailBuffer, L" written="); AppendUnsignedText(detailBuffer, bytesWrittenPointer != nullptr ? *bytesWrittenPointer : 0); })
        APIMON_SIMPLE_BOOL_HOOK(HookedInternetCloseHandle, g_internetCloseHandleOriginal, ks::winapi_monitor::EventCategory::Network, L"Wininet", L"InternetCloseHandle",
            (HINTERNET internetHandle), (internetHandle),
            { BuildSimpleHandleDetail(detailBuffer, L"handle", internetHandle); })

#define APIMON_SIMPLE_HRESULT_HOOK(HookName, OriginalName, ModuleText, ApiText, ParamList, ArgList, DetailBlock) \
        HRESULT WINAPI HookName ParamList \
        { \
            ScopedHookGuard guardValue; \
            if (guardValue.bypass()) { return OriginalName ArgList; } \
            const HRESULT resultValue = OriginalName ArgList; \
            wchar_t detailBuffer[ks::winapi_monitor::kMaxDetailChars] = {}; \
            DetailBlock; \
            SendRawEventWithStatus(ks::winapi_monitor::EventCategory::Process, ModuleText, ApiText, resultValue, detailBuffer); \
            return resultValue; \
        }

        // HookedCrypto/COM 作用：
        // - 输入：CryptoAPI/CNG/COM 关键对象创建、加解密、随机数和类工厂参数；
        // - 处理：补齐安全敏感库调用面，详情只记录算法/长度/句柄，不复制明文或密钥内容；
        // - 返回：保持原始 BOOL/NTSTATUS/HRESULT 结果。
        APIMON_SIMPLE_BOOL_HOOK(HookedCryptAcquireContextW, g_cryptAcquireContextWOriginal, ks::winapi_monitor::EventCategory::Process, L"Advapi32", L"CryptAcquireContextW",
            (HCRYPTPROV* providerPointer, LPCWSTR containerPointer, LPCWSTR providerNamePointer, DWORD providerType, DWORD flagsValue), (providerPointer, containerPointer, providerNamePointer, providerType, flagsValue),
            { AppendWideText(detailBuffer, L"container="); AppendWideText(detailBuffer, containerPointer); AppendWideText(detailBuffer, L" provider="); AppendWideText(detailBuffer, providerNamePointer); AppendWideText(detailBuffer, L" type="); AppendUnsignedText(detailBuffer, providerType); AppendWideText(detailBuffer, L" flags="); AppendHexText(detailBuffer, flagsValue); AppendWideText(detailBuffer, L" handle="); AppendHexText(detailBuffer, providerPointer != nullptr ? static_cast<std::uint64_t>(*providerPointer) : 0); })
        APIMON_SIMPLE_BOOL_HOOK(HookedCryptAcquireContextA, g_cryptAcquireContextAOriginal, ks::winapi_monitor::EventCategory::Process, L"Advapi32", L"CryptAcquireContextA",
            (HCRYPTPROV* providerPointer, LPCSTR containerPointer, LPCSTR providerNamePointer, DWORD providerType, DWORD flagsValue), (providerPointer, containerPointer, providerNamePointer, providerType, flagsValue),
            { AppendWideText(detailBuffer, L"container="); AppendAnsiText(detailBuffer, containerPointer); AppendWideText(detailBuffer, L" provider="); AppendAnsiText(detailBuffer, providerNamePointer); AppendWideText(detailBuffer, L" type="); AppendUnsignedText(detailBuffer, providerType); AppendWideText(detailBuffer, L" flags="); AppendHexText(detailBuffer, flagsValue); AppendWideText(detailBuffer, L" handle="); AppendHexText(detailBuffer, providerPointer != nullptr ? static_cast<std::uint64_t>(*providerPointer) : 0); })
        APIMON_SIMPLE_BOOL_HOOK(HookedCryptCreateHash, g_cryptCreateHashOriginal, ks::winapi_monitor::EventCategory::Process, L"Advapi32", L"CryptCreateHash",
            (HCRYPTPROV providerHandle, ALG_ID algorithmId, HCRYPTKEY keyHandle, DWORD flagsValue, HCRYPTHASH* hashHandlePointer), (providerHandle, algorithmId, keyHandle, flagsValue, hashHandlePointer),
            { AppendWideText(detailBuffer, L"provider="); AppendHexText(detailBuffer, static_cast<std::uint64_t>(providerHandle)); AppendWideText(detailBuffer, L" alg="); AppendHexText(detailBuffer, algorithmId); AppendWideText(detailBuffer, L" flags="); AppendHexText(detailBuffer, flagsValue); AppendWideText(detailBuffer, L" hash="); AppendHexText(detailBuffer, hashHandlePointer != nullptr ? static_cast<std::uint64_t>(*hashHandlePointer) : 0); })
        APIMON_SIMPLE_BOOL_HOOK(HookedCryptHashData, g_cryptHashDataOriginal, ks::winapi_monitor::EventCategory::Process, L"Advapi32", L"CryptHashData",
            (HCRYPTHASH hashHandle, const BYTE* dataPointer, DWORD dataLength, DWORD flagsValue), (hashHandle, dataPointer, dataLength, flagsValue),
            { AppendWideText(detailBuffer, L"hash="); AppendHexText(detailBuffer, static_cast<std::uint64_t>(hashHandle)); AppendWideText(detailBuffer, L" bytes="); AppendUnsignedText(detailBuffer, dataLength); AppendWideText(detailBuffer, L" flags="); AppendHexText(detailBuffer, flagsValue); })
        APIMON_SIMPLE_BOOL_HOOK(HookedCryptDeriveKey, g_cryptDeriveKeyOriginal, ks::winapi_monitor::EventCategory::Process, L"Advapi32", L"CryptDeriveKey",
            (HCRYPTPROV providerHandle, ALG_ID algorithmId, HCRYPTHASH hashHandle, DWORD flagsValue, HCRYPTKEY* keyHandlePointer), (providerHandle, algorithmId, hashHandle, flagsValue, keyHandlePointer),
            { AppendWideText(detailBuffer, L"provider="); AppendHexText(detailBuffer, static_cast<std::uint64_t>(providerHandle)); AppendWideText(detailBuffer, L" alg="); AppendHexText(detailBuffer, algorithmId); AppendWideText(detailBuffer, L" hash="); AppendHexText(detailBuffer, static_cast<std::uint64_t>(hashHandle)); AppendWideText(detailBuffer, L" key="); AppendHexText(detailBuffer, keyHandlePointer != nullptr ? static_cast<std::uint64_t>(*keyHandlePointer) : 0); })
        APIMON_SIMPLE_BOOL_HOOK(HookedCryptEncrypt, g_cryptEncryptOriginal, ks::winapi_monitor::EventCategory::Process, L"Advapi32", L"CryptEncrypt",
            (HCRYPTKEY keyHandle, HCRYPTHASH hashHandle, BOOL finalValue, DWORD flagsValue, BYTE* dataPointer, DWORD* dataLengthPointer, DWORD bufferLength), (keyHandle, hashHandle, finalValue, flagsValue, dataPointer, dataLengthPointer, bufferLength),
            { AppendWideText(detailBuffer, L"key="); AppendHexText(detailBuffer, static_cast<std::uint64_t>(keyHandle)); AppendWideText(detailBuffer, L" final="); AppendUnsignedText(detailBuffer, finalValue != FALSE ? 1ULL : 0ULL); AppendWideText(detailBuffer, L" dataLen="); AppendUnsignedText(detailBuffer, dataLengthPointer != nullptr ? *dataLengthPointer : 0); AppendWideText(detailBuffer, L" bufferLen="); AppendUnsignedText(detailBuffer, bufferLength); })
        APIMON_SIMPLE_BOOL_HOOK(HookedCryptDecrypt, g_cryptDecryptOriginal, ks::winapi_monitor::EventCategory::Process, L"Advapi32", L"CryptDecrypt",
            (HCRYPTKEY keyHandle, HCRYPTHASH hashHandle, BOOL finalValue, DWORD flagsValue, BYTE* dataPointer, DWORD* dataLengthPointer), (keyHandle, hashHandle, finalValue, flagsValue, dataPointer, dataLengthPointer),
            { AppendWideText(detailBuffer, L"key="); AppendHexText(detailBuffer, static_cast<std::uint64_t>(keyHandle)); AppendWideText(detailBuffer, L" final="); AppendUnsignedText(detailBuffer, finalValue != FALSE ? 1ULL : 0ULL); AppendWideText(detailBuffer, L" dataLen="); AppendUnsignedText(detailBuffer, dataLengthPointer != nullptr ? *dataLengthPointer : 0); })
        APIMON_SIMPLE_BOOL_HOOK(HookedCryptGenRandom, g_cryptGenRandomOriginal, ks::winapi_monitor::EventCategory::Process, L"Advapi32", L"CryptGenRandom",
            (HCRYPTPROV providerHandle, DWORD bufferLength, BYTE* bufferPointer), (providerHandle, bufferLength, bufferPointer),
            { AppendWideText(detailBuffer, L"provider="); AppendHexText(detailBuffer, static_cast<std::uint64_t>(providerHandle)); AppendWideText(detailBuffer, L" bytes="); AppendUnsignedText(detailBuffer, bufferLength); })
        APIMON_SIMPLE_BOOL_HOOK(HookedCryptReleaseContext, g_cryptReleaseContextOriginal, ks::winapi_monitor::EventCategory::Process, L"Advapi32", L"CryptReleaseContext",
            (HCRYPTPROV providerHandle, DWORD flagsValue), (providerHandle, flagsValue),
            { AppendWideText(detailBuffer, L"provider="); AppendHexText(detailBuffer, static_cast<std::uint64_t>(providerHandle)); AppendWideText(detailBuffer, L" flags="); AppendHexText(detailBuffer, flagsValue); })

        APIMON_SIMPLE_NTSTATUS_HOOK(HookedBCryptOpenAlgorithmProvider, g_bCryptOpenAlgorithmProviderOriginal, ks::winapi_monitor::EventCategory::Process, L"Bcrypt", L"BCryptOpenAlgorithmProvider",
            (BCRYPT_ALG_HANDLE* algorithmHandlePointer, LPCWSTR algorithmIdPointer, LPCWSTR implementationPointer, ULONG flagsValue),
            (algorithmHandlePointer, algorithmIdPointer, implementationPointer, flagsValue),
            { AppendWideText(detailBuffer, L"alg="); AppendWideText(detailBuffer, algorithmIdPointer); AppendWideText(detailBuffer, L" impl="); AppendWideText(detailBuffer, implementationPointer); AppendWideText(detailBuffer, L" flags="); AppendHexText(detailBuffer, flagsValue); AppendWideText(detailBuffer, L" handle="); AppendHexText(detailBuffer, algorithmHandlePointer != nullptr ? reinterpret_cast<std::uint64_t>(*algorithmHandlePointer) : 0); })
        APIMON_SIMPLE_NTSTATUS_HOOK(HookedBCryptCreateHash, g_bCryptCreateHashOriginal, ks::winapi_monitor::EventCategory::Process, L"Bcrypt", L"BCryptCreateHash",
            (BCRYPT_ALG_HANDLE algorithmHandle, BCRYPT_HASH_HANDLE* hashHandlePointer, PUCHAR hashObjectPointer, ULONG hashObjectLength, PUCHAR secretPointer, ULONG secretLength, ULONG flagsValue),
            (algorithmHandle, hashHandlePointer, hashObjectPointer, hashObjectLength, secretPointer, secretLength, flagsValue),
            { AppendWideText(detailBuffer, L"algHandle="); AppendHexText(detailBuffer, reinterpret_cast<std::uint64_t>(algorithmHandle)); AppendWideText(detailBuffer, L" objectLen="); AppendUnsignedText(detailBuffer, hashObjectLength); AppendWideText(detailBuffer, L" secretLen="); AppendUnsignedText(detailBuffer, secretLength); AppendWideText(detailBuffer, L" flags="); AppendHexText(detailBuffer, flagsValue); AppendWideText(detailBuffer, L" hash="); AppendHexText(detailBuffer, hashHandlePointer != nullptr ? reinterpret_cast<std::uint64_t>(*hashHandlePointer) : 0); })
        APIMON_SIMPLE_NTSTATUS_HOOK(HookedBCryptHashData, g_bCryptHashDataOriginal, ks::winapi_monitor::EventCategory::Process, L"Bcrypt", L"BCryptHashData",
            (BCRYPT_HASH_HANDLE hashHandle, PUCHAR inputPointer, ULONG inputLength, ULONG flagsValue), (hashHandle, inputPointer, inputLength, flagsValue),
            { AppendWideText(detailBuffer, L"hash="); AppendHexText(detailBuffer, reinterpret_cast<std::uint64_t>(hashHandle)); AppendWideText(detailBuffer, L" bytes="); AppendUnsignedText(detailBuffer, inputLength); AppendWideText(detailBuffer, L" flags="); AppendHexText(detailBuffer, flagsValue); })
        APIMON_SIMPLE_NTSTATUS_HOOK(HookedBCryptFinishHash, g_bCryptFinishHashOriginal, ks::winapi_monitor::EventCategory::Process, L"Bcrypt", L"BCryptFinishHash",
            (BCRYPT_HASH_HANDLE hashHandle, PUCHAR outputPointer, ULONG outputLength, ULONG flagsValue), (hashHandle, outputPointer, outputLength, flagsValue),
            { AppendWideText(detailBuffer, L"hash="); AppendHexText(detailBuffer, reinterpret_cast<std::uint64_t>(hashHandle)); AppendWideText(detailBuffer, L" outputLen="); AppendUnsignedText(detailBuffer, outputLength); AppendWideText(detailBuffer, L" flags="); AppendHexText(detailBuffer, flagsValue); })
        APIMON_SIMPLE_NTSTATUS_HOOK(HookedBCryptEncrypt, g_bCryptEncryptOriginal, ks::winapi_monitor::EventCategory::Process, L"Bcrypt", L"BCryptEncrypt",
            (BCRYPT_KEY_HANDLE keyHandle, PUCHAR inputPointer, ULONG inputLength, VOID* paddingInfoPointer, PUCHAR ivPointer, ULONG ivLength, PUCHAR outputPointer, ULONG outputLength, ULONG* resultLengthPointer, ULONG flagsValue),
            (keyHandle, inputPointer, inputLength, paddingInfoPointer, ivPointer, ivLength, outputPointer, outputLength, resultLengthPointer, flagsValue),
            { AppendWideText(detailBuffer, L"key="); AppendHexText(detailBuffer, reinterpret_cast<std::uint64_t>(keyHandle)); AppendWideText(detailBuffer, L" inputLen="); AppendUnsignedText(detailBuffer, inputLength); AppendWideText(detailBuffer, L" outputLen="); AppendUnsignedText(detailBuffer, outputLength); AppendWideText(detailBuffer, L" resultLen="); AppendUnsignedText(detailBuffer, resultLengthPointer != nullptr ? *resultLengthPointer : 0); AppendWideText(detailBuffer, L" flags="); AppendHexText(detailBuffer, flagsValue); })
        APIMON_SIMPLE_NTSTATUS_HOOK(HookedBCryptDecrypt, g_bCryptDecryptOriginal, ks::winapi_monitor::EventCategory::Process, L"Bcrypt", L"BCryptDecrypt",
            (BCRYPT_KEY_HANDLE keyHandle, PUCHAR inputPointer, ULONG inputLength, VOID* paddingInfoPointer, PUCHAR ivPointer, ULONG ivLength, PUCHAR outputPointer, ULONG outputLength, ULONG* resultLengthPointer, ULONG flagsValue),
            (keyHandle, inputPointer, inputLength, paddingInfoPointer, ivPointer, ivLength, outputPointer, outputLength, resultLengthPointer, flagsValue),
            { AppendWideText(detailBuffer, L"key="); AppendHexText(detailBuffer, reinterpret_cast<std::uint64_t>(keyHandle)); AppendWideText(detailBuffer, L" inputLen="); AppendUnsignedText(detailBuffer, inputLength); AppendWideText(detailBuffer, L" outputLen="); AppendUnsignedText(detailBuffer, outputLength); AppendWideText(detailBuffer, L" resultLen="); AppendUnsignedText(detailBuffer, resultLengthPointer != nullptr ? *resultLengthPointer : 0); AppendWideText(detailBuffer, L" flags="); AppendHexText(detailBuffer, flagsValue); })
        APIMON_SIMPLE_NTSTATUS_HOOK(HookedBCryptGenRandom, g_bCryptGenRandomOriginal, ks::winapi_monitor::EventCategory::Process, L"Bcrypt", L"BCryptGenRandom",
            (BCRYPT_ALG_HANDLE algorithmHandle, PUCHAR bufferPointer, ULONG bufferLength, ULONG flagsValue), (algorithmHandle, bufferPointer, bufferLength, flagsValue),
            { AppendWideText(detailBuffer, L"algHandle="); AppendHexText(detailBuffer, reinterpret_cast<std::uint64_t>(algorithmHandle)); AppendWideText(detailBuffer, L" bytes="); AppendUnsignedText(detailBuffer, bufferLength); AppendWideText(detailBuffer, L" flags="); AppendHexText(detailBuffer, flagsValue); })
        APIMON_SIMPLE_NTSTATUS_HOOK(HookedBCryptCloseAlgorithmProvider, g_bCryptCloseAlgorithmProviderOriginal, ks::winapi_monitor::EventCategory::Process, L"Bcrypt", L"BCryptCloseAlgorithmProvider",
            (BCRYPT_ALG_HANDLE algorithmHandle, ULONG flagsValue), (algorithmHandle, flagsValue),
            { AppendWideText(detailBuffer, L"algHandle="); AppendHexText(detailBuffer, reinterpret_cast<std::uint64_t>(algorithmHandle)); AppendWideText(detailBuffer, L" flags="); AppendHexText(detailBuffer, flagsValue); })
        APIMON_SIMPLE_NTSTATUS_HOOK(HookedBCryptDestroyHash, g_bCryptDestroyHashOriginal, ks::winapi_monitor::EventCategory::Process, L"Bcrypt", L"BCryptDestroyHash",
            (BCRYPT_HASH_HANDLE hashHandle), (hashHandle),
            { BuildSimpleHandleDetail(detailBuffer, L"hash", hashHandle); })
        APIMON_SIMPLE_HRESULT_HOOK(HookedCoCreateInstance, g_coCreateInstanceOriginal, L"Ole32", L"CoCreateInstance",
            (REFCLSID classId, LPUNKNOWN outerUnknownPointer, DWORD classContext, REFIID interfaceId, LPVOID* objectPointer),
            (classId, outerUnknownPointer, classContext, interfaceId, objectPointer),
            { AppendWideText(detailBuffer, L"clsctx="); AppendHexText(detailBuffer, classContext); AppendWideText(detailBuffer, L" object="); AppendHexText(detailBuffer, objectPointer != nullptr ? reinterpret_cast<std::uint64_t>(*objectPointer) : 0); })
        APIMON_SIMPLE_HRESULT_HOOK(HookedCoCreateInstanceEx, g_coCreateInstanceExOriginal, L"Ole32", L"CoCreateInstanceEx",
            (REFCLSID classId, IUnknown* outerUnknownPointer, DWORD classContext, COSERVERINFO* serverInfoPointer, DWORD multiQiCount, MULTI_QI* resultsPointer),
            (classId, outerUnknownPointer, classContext, serverInfoPointer, multiQiCount, resultsPointer),
            { AppendWideText(detailBuffer, L"clsctx="); AppendHexText(detailBuffer, classContext); AppendWideText(detailBuffer, L" qiCount="); AppendUnsignedText(detailBuffer, multiQiCount); AppendWideText(detailBuffer, L" server="); AppendWideText(detailBuffer, serverInfoPointer != nullptr ? serverInfoPointer->pwszName : nullptr); })
        APIMON_SIMPLE_HRESULT_HOOK(HookedCoGetClassObject, g_coGetClassObjectOriginal, L"Ole32", L"CoGetClassObject",
            (REFCLSID classId, DWORD classContext, COSERVERINFO* serverInfoPointer, REFIID interfaceId, LPVOID* classObjectPointer),
            (classId, classContext, serverInfoPointer, interfaceId, classObjectPointer),
            { AppendWideText(detailBuffer, L"clsctx="); AppendHexText(detailBuffer, classContext); AppendWideText(detailBuffer, L" factory="); AppendHexText(detailBuffer, classObjectPointer != nullptr ? reinterpret_cast<std::uint64_t>(*classObjectPointer) : 0); AppendWideText(detailBuffer, L" server="); AppendWideText(detailBuffer, serverInfoPointer != nullptr ? serverInfoPointer->pwszName : nullptr); })

        // HookedLocalMemory 作用：
        // - 输入：当前进程 VirtualAlloc/Free/Protect 参数；
        // - 处理：补齐非 Ex 本地内存分配、释放和权限修改，覆盖自解密/动态代码生成常见路径；
        // - 返回：保持原始返回值并恢复 LastError。
        APIMON_SIMPLE_HANDLE_HOOK(LPVOID, HookedVirtualAlloc, g_virtualAllocOriginal, ks::winapi_monitor::EventCategory::Process, L"KernelBase", L"VirtualAlloc",
            (LPVOID baseAddress, SIZE_T sizeValue, DWORD allocationType, DWORD protectValue), (baseAddress, sizeValue, allocationType, protectValue),
            { AppendWideText(detailBuffer, L"baseHint="); AppendHexText(detailBuffer, reinterpret_cast<std::uint64_t>(baseAddress)); AppendWideText(detailBuffer, L" size="); AppendUnsignedText(detailBuffer, static_cast<unsigned long long>(sizeValue)); AppendWideText(detailBuffer, L" type="); AppendHexText(detailBuffer, allocationType); AppendWideText(detailBuffer, L" protect="); AppendHexText(detailBuffer, protectValue); AppendWideText(detailBuffer, L" result="); AppendHexText(detailBuffer, reinterpret_cast<std::uint64_t>(resultHandle)); })
        APIMON_SIMPLE_BOOL_HOOK(HookedVirtualFree, g_virtualFreeOriginal, ks::winapi_monitor::EventCategory::Process, L"KernelBase", L"VirtualFree",
            (LPVOID baseAddress, SIZE_T sizeValue, DWORD freeType), (baseAddress, sizeValue, freeType),
            { AppendWideText(detailBuffer, L"base="); AppendHexText(detailBuffer, reinterpret_cast<std::uint64_t>(baseAddress)); AppendWideText(detailBuffer, L" size="); AppendUnsignedText(detailBuffer, static_cast<unsigned long long>(sizeValue)); AppendWideText(detailBuffer, L" type="); AppendHexText(detailBuffer, freeType); })
        APIMON_SIMPLE_BOOL_HOOK(HookedVirtualProtect, g_virtualProtectOriginal, ks::winapi_monitor::EventCategory::Process, L"KernelBase", L"VirtualProtect",
            (LPVOID baseAddress, SIZE_T sizeValue, DWORD newProtect, PDWORD oldProtectPointer), (baseAddress, sizeValue, newProtect, oldProtectPointer),
            { AppendWideText(detailBuffer, L"base="); AppendHexText(detailBuffer, reinterpret_cast<std::uint64_t>(baseAddress)); AppendWideText(detailBuffer, L" size="); AppendUnsignedText(detailBuffer, static_cast<unsigned long long>(sizeValue)); AppendWideText(detailBuffer, L" newProtect="); AppendHexText(detailBuffer, newProtect); AppendWideText(detailBuffer, L" oldProtect="); AppendHexText(detailBuffer, oldProtectPointer != nullptr ? *oldProtectPointer : 0); })

        // HookedToolhelpAndModule 作用：
        // - 输入：Toolhelp 快照和模块枚举/模块路径查询参数；
        // - 处理：补齐模块枚举和加载器探测链路；
        // - 返回：保持原始 HANDLE/BOOL/DWORD 语义。
        HANDLE WINAPI HookedCreateToolhelp32Snapshot(DWORD flagsValue, DWORD processId)
        {
            ScopedHookGuard guardValue;
            if (guardValue.bypass()) { return g_createToolhelp32SnapshotOriginal(flagsValue, processId); }
            const HANDLE resultHandle = g_createToolhelp32SnapshotOriginal(flagsValue, processId);
            const DWORD lastError = ::GetLastError();
            wchar_t detailBuffer[ks::winapi_monitor::kMaxDetailChars] = {};
            AppendWideText(detailBuffer, L"flags=");
            AppendHexText(detailBuffer, flagsValue);
            AppendWideText(detailBuffer, L" pid=");
            AppendUnsignedText(detailBuffer, processId);
            AppendWideText(detailBuffer, L" snapshot=");
            AppendHexText(detailBuffer, reinterpret_cast<std::uint64_t>(resultHandle));
            SendRawEventWithStatus(ks::winapi_monitor::EventCategory::Process, L"Kernel32", L"CreateToolhelp32Snapshot", resultHandle != INVALID_HANDLE_VALUE ? 0 : lastError, detailBuffer);
            ::SetLastError(lastError);
            return resultHandle;
        }

        APIMON_SIMPLE_BOOL_HOOK(HookedModule32FirstW, g_module32FirstWOriginal, ks::winapi_monitor::EventCategory::Loader, L"Kernel32", L"Module32FirstW",
            (HANDLE snapshotHandle, LPMODULEENTRY32W entryPointer), (snapshotHandle, entryPointer),
            { AppendWideText(detailBuffer, L"snapshot="); AppendHexText(detailBuffer, reinterpret_cast<std::uint64_t>(snapshotHandle)); AppendWideText(detailBuffer, L" module="); AppendWideText(detailBuffer, resultValue != FALSE && entryPointer != nullptr ? entryPointer->szModule : nullptr); AppendWideText(detailBuffer, L" base="); AppendHexText(detailBuffer, resultValue != FALSE && entryPointer != nullptr ? reinterpret_cast<std::uint64_t>(entryPointer->modBaseAddr) : 0); })
        APIMON_SIMPLE_BOOL_HOOK(HookedModule32NextW, g_module32NextWOriginal, ks::winapi_monitor::EventCategory::Loader, L"Kernel32", L"Module32NextW",
            (HANDLE snapshotHandle, LPMODULEENTRY32W entryPointer), (snapshotHandle, entryPointer),
            { AppendWideText(detailBuffer, L"snapshot="); AppendHexText(detailBuffer, reinterpret_cast<std::uint64_t>(snapshotHandle)); AppendWideText(detailBuffer, L" module="); AppendWideText(detailBuffer, resultValue != FALSE && entryPointer != nullptr ? entryPointer->szModule : nullptr); AppendWideText(detailBuffer, L" base="); AppendHexText(detailBuffer, resultValue != FALSE && entryPointer != nullptr ? reinterpret_cast<std::uint64_t>(entryPointer->modBaseAddr) : 0); })
        APIMON_SIMPLE_BOOL_HOOK(HookedModule32FirstA, g_module32FirstAOriginal, ks::winapi_monitor::EventCategory::Loader, L"Kernel32", L"Module32First",
            (HANDLE snapshotHandle, tagMODULEENTRY32* entryPointer), (snapshotHandle, entryPointer),
            { AppendWideText(detailBuffer, L"snapshot="); AppendHexText(detailBuffer, reinterpret_cast<std::uint64_t>(snapshotHandle)); AppendWideText(detailBuffer, L" module="); if (resultValue != FALSE && entryPointer != nullptr) { AppendAnsiText(detailBuffer, entryPointer->szModule); } AppendWideText(detailBuffer, L" base="); AppendHexText(detailBuffer, resultValue != FALSE && entryPointer != nullptr ? reinterpret_cast<std::uint64_t>(entryPointer->modBaseAddr) : 0); })
        APIMON_SIMPLE_BOOL_HOOK(HookedModule32NextA, g_module32NextAOriginal, ks::winapi_monitor::EventCategory::Loader, L"Kernel32", L"Module32Next",
            (HANDLE snapshotHandle, tagMODULEENTRY32* entryPointer), (snapshotHandle, entryPointer),
            { AppendWideText(detailBuffer, L"snapshot="); AppendHexText(detailBuffer, reinterpret_cast<std::uint64_t>(snapshotHandle)); AppendWideText(detailBuffer, L" module="); if (resultValue != FALSE && entryPointer != nullptr) { AppendAnsiText(detailBuffer, entryPointer->szModule); } AppendWideText(detailBuffer, L" base="); AppendHexText(detailBuffer, resultValue != FALSE && entryPointer != nullptr ? reinterpret_cast<std::uint64_t>(entryPointer->modBaseAddr) : 0); })

        APIMON_SIMPLE_HANDLE_HOOK(HMODULE, HookedGetModuleHandleW, g_getModuleHandleWOriginal, ks::winapi_monitor::EventCategory::Loader, L"Kernel32", L"GetModuleHandleW",
            (LPCWSTR moduleNamePointer), (moduleNamePointer),
            { AppendWideText(detailBuffer, L"name="); AppendWideText(detailBuffer, moduleNamePointer); AppendWideText(detailBuffer, L" module="); AppendHexText(detailBuffer, reinterpret_cast<std::uint64_t>(resultHandle)); })
        APIMON_SIMPLE_HANDLE_HOOK(HMODULE, HookedGetModuleHandleA, g_getModuleHandleAOriginal, ks::winapi_monitor::EventCategory::Loader, L"Kernel32", L"GetModuleHandleA",
            (LPCSTR moduleNamePointer), (moduleNamePointer),
            { AppendWideText(detailBuffer, L"name="); AppendAnsiText(detailBuffer, moduleNamePointer); AppendWideText(detailBuffer, L" module="); AppendHexText(detailBuffer, reinterpret_cast<std::uint64_t>(resultHandle)); })
        APIMON_SIMPLE_BOOL_HOOK(HookedGetModuleHandleExW, g_getModuleHandleExWOriginal, ks::winapi_monitor::EventCategory::Loader, L"Kernel32", L"GetModuleHandleExW",
            (DWORD flagsValue, LPCWSTR moduleNamePointer, HMODULE* moduleHandlePointer), (flagsValue, moduleNamePointer, moduleHandlePointer),
            { AppendWideText(detailBuffer, L"flags="); AppendHexText(detailBuffer, flagsValue); AppendWideText(detailBuffer, L" name="); AppendWideText(detailBuffer, moduleNamePointer); AppendWideText(detailBuffer, L" module="); AppendHexText(detailBuffer, moduleHandlePointer != nullptr ? reinterpret_cast<std::uint64_t>(*moduleHandlePointer) : 0); })
        APIMON_SIMPLE_BOOL_HOOK(HookedGetModuleHandleExA, g_getModuleHandleExAOriginal, ks::winapi_monitor::EventCategory::Loader, L"Kernel32", L"GetModuleHandleExA",
            (DWORD flagsValue, LPCSTR moduleNamePointer, HMODULE* moduleHandlePointer), (flagsValue, moduleNamePointer, moduleHandlePointer),
            { AppendWideText(detailBuffer, L"flags="); AppendHexText(detailBuffer, flagsValue); AppendWideText(detailBuffer, L" name="); AppendAnsiText(detailBuffer, moduleNamePointer); AppendWideText(detailBuffer, L" module="); AppendHexText(detailBuffer, moduleHandlePointer != nullptr ? reinterpret_cast<std::uint64_t>(*moduleHandlePointer) : 0); })

        DWORD WINAPI HookedGetModuleFileNameW(HMODULE moduleHandle, LPWSTR fileNamePointer, DWORD sizeValue)
        {
            ScopedHookGuard guardValue;
            if (guardValue.bypass()) { return g_getModuleFileNameWOriginal(moduleHandle, fileNamePointer, sizeValue); }
            const DWORD resultValue = g_getModuleFileNameWOriginal(moduleHandle, fileNamePointer, sizeValue);
            const DWORD lastError = ::GetLastError();
            wchar_t detailBuffer[ks::winapi_monitor::kMaxDetailChars] = {};
            AppendWideText(detailBuffer, L"module=");
            AppendHexText(detailBuffer, reinterpret_cast<std::uint64_t>(moduleHandle));
            AppendWideText(detailBuffer, L" size=");
            AppendUnsignedText(detailBuffer, sizeValue);
            AppendWideText(detailBuffer, L" path=");
            AppendWideText(detailBuffer, resultValue != 0 ? fileNamePointer : nullptr, resultValue);
            SendRawEventWithStatus(ks::winapi_monitor::EventCategory::Loader, L"Kernel32", L"GetModuleFileNameW", resultValue != 0 ? 0 : lastError, detailBuffer);
            ::SetLastError(lastError);
            return resultValue;
        }

        DWORD WINAPI HookedGetModuleFileNameA(HMODULE moduleHandle, LPSTR fileNamePointer, DWORD sizeValue)
        {
            ScopedHookGuard guardValue;
            if (guardValue.bypass()) { return g_getModuleFileNameAOriginal(moduleHandle, fileNamePointer, sizeValue); }
            const DWORD resultValue = g_getModuleFileNameAOriginal(moduleHandle, fileNamePointer, sizeValue);
            const DWORD lastError = ::GetLastError();
            wchar_t detailBuffer[ks::winapi_monitor::kMaxDetailChars] = {};
            AppendWideText(detailBuffer, L"module=");
            AppendHexText(detailBuffer, reinterpret_cast<std::uint64_t>(moduleHandle));
            AppendWideText(detailBuffer, L" size=");
            AppendUnsignedText(detailBuffer, sizeValue);
            AppendWideText(detailBuffer, L" path=");
            if (resultValue != 0) { AppendAnsiText(detailBuffer, fileNamePointer, resultValue); }
            SendRawEventWithStatus(ks::winapi_monitor::EventCategory::Loader, L"Kernel32", L"GetModuleFileNameA", resultValue != 0 ? 0 : lastError, detailBuffer);
            ::SetLastError(lastError);
            return resultValue;
        }

        // HookedFileExtras 作用：记录硬链接、文件替换、EOF 设置和文件锁操作；返回原始 BOOL。
        APIMON_SIMPLE_BOOL_HOOK(HookedCreateHardLinkW, g_createHardLinkWOriginal, ks::winapi_monitor::EventCategory::File, L"KernelBase", L"CreateHardLinkW",
            (LPCWSTR fileNamePointer, LPCWSTR existingFileNamePointer, LPSECURITY_ATTRIBUTES securityAttributes), (fileNamePointer, existingFileNamePointer, securityAttributes),
            { BuildTwoPathDetailW(detailBuffer, existingFileNamePointer, fileNamePointer, 0); })
        APIMON_SIMPLE_BOOL_HOOK(HookedCreateHardLinkA, g_createHardLinkAOriginal, ks::winapi_monitor::EventCategory::File, L"KernelBase", L"CreateHardLinkA",
            (LPCSTR fileNamePointer, LPCSTR existingFileNamePointer, LPSECURITY_ATTRIBUTES securityAttributes), (fileNamePointer, existingFileNamePointer, securityAttributes),
            { BuildTwoPathDetailA(detailBuffer, existingFileNamePointer, fileNamePointer, 0); })
        APIMON_SIMPLE_BOOL_HOOK(HookedReplaceFileW, g_replaceFileWOriginal, ks::winapi_monitor::EventCategory::File, L"KernelBase", L"ReplaceFileW",
            (LPCWSTR replacedFilePointer, LPCWSTR replacementFilePointer, LPCWSTR backupFilePointer, DWORD flagsValue, LPVOID excludePointer, LPVOID reservedPointer),
            (replacedFilePointer, replacementFilePointer, backupFilePointer, flagsValue, excludePointer, reservedPointer),
            { BuildTwoPathDetailW(detailBuffer, replacedFilePointer, replacementFilePointer, flagsValue); AppendWideText(detailBuffer, L" backup="); AppendWideText(detailBuffer, backupFilePointer); })
        APIMON_SIMPLE_BOOL_HOOK(HookedReplaceFileA, g_replaceFileAOriginal, ks::winapi_monitor::EventCategory::File, L"KernelBase", L"ReplaceFileA",
            (LPCSTR replacedFilePointer, LPCSTR replacementFilePointer, LPCSTR backupFilePointer, DWORD flagsValue, LPVOID excludePointer, LPVOID reservedPointer),
            (replacedFilePointer, replacementFilePointer, backupFilePointer, flagsValue, excludePointer, reservedPointer),
            { BuildTwoPathDetailA(detailBuffer, replacedFilePointer, replacementFilePointer, flagsValue); AppendWideText(detailBuffer, L" backup="); AppendAnsiText(detailBuffer, backupFilePointer); })
        APIMON_SIMPLE_BOOL_HOOK(HookedSetEndOfFile, g_setEndOfFileOriginal, ks::winapi_monitor::EventCategory::File, L"KernelBase", L"SetEndOfFile",
            (HANDLE fileHandle), (fileHandle),
            { BuildSimpleHandleDetail(detailBuffer, L"file", fileHandle); })
        APIMON_SIMPLE_BOOL_HOOK(HookedLockFileEx, g_lockFileExOriginal, ks::winapi_monitor::EventCategory::File, L"KernelBase", L"LockFileEx",
            (HANDLE fileHandle, DWORD flagsValue, DWORD reservedValue, DWORD bytesLow, DWORD bytesHigh, LPOVERLAPPED overlappedPointer),
            (fileHandle, flagsValue, reservedValue, bytesLow, bytesHigh, overlappedPointer),
            { AppendWideText(detailBuffer, L"file="); AppendHexText(detailBuffer, reinterpret_cast<std::uint64_t>(fileHandle)); AppendWideText(detailBuffer, L" flags="); AppendHexText(detailBuffer, flagsValue); AppendWideText(detailBuffer, L" bytes="); AppendHexText(detailBuffer, (static_cast<std::uint64_t>(bytesHigh) << 32) | bytesLow); })
        APIMON_SIMPLE_BOOL_HOOK(HookedUnlockFileEx, g_unlockFileExOriginal, ks::winapi_monitor::EventCategory::File, L"KernelBase", L"UnlockFileEx",
            (HANDLE fileHandle, DWORD reservedValue, DWORD bytesLow, DWORD bytesHigh, LPOVERLAPPED overlappedPointer),
            (fileHandle, reservedValue, bytesLow, bytesHigh, overlappedPointer),
            { AppendWideText(detailBuffer, L"file="); AppendHexText(detailBuffer, reinterpret_cast<std::uint64_t>(fileHandle)); AppendWideText(detailBuffer, L" bytes="); AppendHexText(detailBuffer, (static_cast<std::uint64_t>(bytesHigh) << 32) | bytesLow); })

        // HookedShellExecuteW/A 作用：
        // - 输入：ShellExecute 目标、参数、目录和显示方式；
        // - 处理：补齐 ShellExecuteEx 之外的老式 Shell 启动入口；
        // - 返回：保持原始 HINSTANCE 语义，<=32 视作失败并恢复 LastError。
        HINSTANCE WINAPI HookedShellExecuteW(HWND windowHandle, LPCWSTR operationPointer, LPCWSTR filePointer, LPCWSTR parametersPointer, LPCWSTR directoryPointer, INT showCommand)
        {
            ScopedHookGuard guardValue;
            if (guardValue.bypass()) { return g_shellExecuteWOriginal(windowHandle, operationPointer, filePointer, parametersPointer, directoryPointer, showCommand); }
            const HINSTANCE resultValue = g_shellExecuteWOriginal(windowHandle, operationPointer, filePointer, parametersPointer, directoryPointer, showCommand);
            const DWORD lastError = ::GetLastError();
            wchar_t detailBuffer[ks::winapi_monitor::kMaxDetailChars] = {};
            AppendWideText(detailBuffer, L"op=");
            AppendWideText(detailBuffer, operationPointer);
            AppendWideText(detailBuffer, L" file=");
            AppendWideText(detailBuffer, filePointer);
            AppendWideText(detailBuffer, L" params=");
            AppendWideText(detailBuffer, parametersPointer);
            AppendWideText(detailBuffer, L" cwd=");
            AppendWideText(detailBuffer, directoryPointer);
            SendRawEventWithStatus(ks::winapi_monitor::EventCategory::Process, L"Shell32", L"ShellExecuteW", reinterpret_cast<INT_PTR>(resultValue) > 32 ? 0 : lastError, detailBuffer);
            ::SetLastError(lastError);
            return resultValue;
        }

        HINSTANCE WINAPI HookedShellExecuteA(HWND windowHandle, LPCSTR operationPointer, LPCSTR filePointer, LPCSTR parametersPointer, LPCSTR directoryPointer, INT showCommand)
        {
            ScopedHookGuard guardValue;
            if (guardValue.bypass()) { return g_shellExecuteAOriginal(windowHandle, operationPointer, filePointer, parametersPointer, directoryPointer, showCommand); }
            const HINSTANCE resultValue = g_shellExecuteAOriginal(windowHandle, operationPointer, filePointer, parametersPointer, directoryPointer, showCommand);
            const DWORD lastError = ::GetLastError();
            wchar_t detailBuffer[ks::winapi_monitor::kMaxDetailChars] = {};
            AppendWideText(detailBuffer, L"op=");
            AppendAnsiText(detailBuffer, operationPointer);
            AppendWideText(detailBuffer, L" file=");
            AppendAnsiText(detailBuffer, filePointer);
            AppendWideText(detailBuffer, L" params=");
            AppendAnsiText(detailBuffer, parametersPointer);
            AppendWideText(detailBuffer, L" cwd=");
            AppendAnsiText(detailBuffer, directoryPointer);
            SendRawEventWithStatus(ks::winapi_monitor::EventCategory::Process, L"Shell32", L"ShellExecuteA", reinterpret_cast<INT_PTR>(resultValue) > 32 ? 0 : lastError, detailBuffer);
            ::SetLastError(lastError);
            return resultValue;
        }

        APIMON_SIMPLE_BOOL_HOOK(HookedCreateProcessWithLogonW, g_createProcessWithLogonWOriginal, ks::winapi_monitor::EventCategory::Process, L"Advapi32", L"CreateProcessWithLogonW",
            (LPCWSTR userNamePointer, LPCWSTR domainPointer, LPCWSTR passwordPointer, DWORD logonFlags, LPCWSTR applicationNamePointer, LPWSTR commandLinePointer, DWORD creationFlags, LPVOID environmentPointer, LPCWSTR currentDirectoryPointer, LPSTARTUPINFOW startupInfoPointer, LPPROCESS_INFORMATION processInformationPointer),
            (userNamePointer, domainPointer, passwordPointer, logonFlags, applicationNamePointer, commandLinePointer, creationFlags, environmentPointer, currentDirectoryPointer, startupInfoPointer, processInformationPointer),
            { AppendWideText(detailBuffer, L"user="); AppendWideText(detailBuffer, domainPointer); AppendWideText(detailBuffer, L"\\"); AppendWideText(detailBuffer, userNamePointer); AppendWideText(detailBuffer, L" logonFlags="); AppendHexText(detailBuffer, logonFlags); AppendWideText(detailBuffer, L" app="); AppendWideText(detailBuffer, applicationNamePointer); AppendWideText(detailBuffer, L" cmd="); AppendWideText(detailBuffer, commandLinePointer); AppendWideText(detailBuffer, L" childPid="); AppendUnsignedText(detailBuffer, processInformationPointer != nullptr ? processInformationPointer->dwProcessId : 0); })

        // HookedServiceQuery 作用：记录服务二级配置、状态和枚举，补齐服务创建/启动之外的 SCM 侦察面；返回原始 BOOL。
        APIMON_SIMPLE_BOOL_HOOK(HookedChangeServiceConfig2W, g_changeServiceConfig2WOriginal, ks::winapi_monitor::EventCategory::Process, L"Advapi32", L"ChangeServiceConfig2W",
            (SC_HANDLE serviceHandle, DWORD infoLevel, LPVOID infoPointer), (serviceHandle, infoLevel, infoPointer),
            { AppendWideText(detailBuffer, L"service="); AppendHexText(detailBuffer, reinterpret_cast<std::uint64_t>(serviceHandle)); AppendWideText(detailBuffer, L" level="); AppendUnsignedText(detailBuffer, infoLevel); })
        APIMON_SIMPLE_BOOL_HOOK(HookedChangeServiceConfig2A, g_changeServiceConfig2AOriginal, ks::winapi_monitor::EventCategory::Process, L"Advapi32", L"ChangeServiceConfig2A",
            (SC_HANDLE serviceHandle, DWORD infoLevel, LPVOID infoPointer), (serviceHandle, infoLevel, infoPointer),
            { AppendWideText(detailBuffer, L"service="); AppendHexText(detailBuffer, reinterpret_cast<std::uint64_t>(serviceHandle)); AppendWideText(detailBuffer, L" level="); AppendUnsignedText(detailBuffer, infoLevel); })
        APIMON_SIMPLE_BOOL_HOOK(HookedQueryServiceStatusEx, g_queryServiceStatusExOriginal, ks::winapi_monitor::EventCategory::Process, L"Advapi32", L"QueryServiceStatusEx",
            (SC_HANDLE serviceHandle, SC_STATUS_TYPE infoLevel, LPBYTE bufferPointer, DWORD bufferSize, LPDWORD bytesNeededPointer),
            (serviceHandle, infoLevel, bufferPointer, bufferSize, bytesNeededPointer),
            { AppendWideText(detailBuffer, L"service="); AppendHexText(detailBuffer, reinterpret_cast<std::uint64_t>(serviceHandle)); AppendWideText(detailBuffer, L" level="); AppendUnsignedText(detailBuffer, infoLevel); AppendWideText(detailBuffer, L" buffer="); AppendUnsignedText(detailBuffer, bufferSize); AppendWideText(detailBuffer, L" needed="); AppendUnsignedText(detailBuffer, bytesNeededPointer != nullptr ? *bytesNeededPointer : 0); })
        APIMON_SIMPLE_BOOL_HOOK(HookedQueryServiceConfigW, g_queryServiceConfigWOriginal, ks::winapi_monitor::EventCategory::Process, L"Advapi32", L"QueryServiceConfigW",
            (SC_HANDLE serviceHandle, LPQUERY_SERVICE_CONFIGW configPointer, DWORD bufferSize, LPDWORD bytesNeededPointer), (serviceHandle, configPointer, bufferSize, bytesNeededPointer),
            { AppendWideText(detailBuffer, L"service="); AppendHexText(detailBuffer, reinterpret_cast<std::uint64_t>(serviceHandle)); AppendWideText(detailBuffer, L" buffer="); AppendUnsignedText(detailBuffer, bufferSize); AppendWideText(detailBuffer, L" needed="); AppendUnsignedText(detailBuffer, bytesNeededPointer != nullptr ? *bytesNeededPointer : 0); if (resultValue != FALSE && configPointer != nullptr) { AppendWideText(detailBuffer, L" path="); AppendWideText(detailBuffer, configPointer->lpBinaryPathName); } })
        APIMON_SIMPLE_BOOL_HOOK(HookedQueryServiceConfigA, g_queryServiceConfigAOriginal, ks::winapi_monitor::EventCategory::Process, L"Advapi32", L"QueryServiceConfigA",
            (SC_HANDLE serviceHandle, LPQUERY_SERVICE_CONFIGA configPointer, DWORD bufferSize, LPDWORD bytesNeededPointer), (serviceHandle, configPointer, bufferSize, bytesNeededPointer),
            { AppendWideText(detailBuffer, L"service="); AppendHexText(detailBuffer, reinterpret_cast<std::uint64_t>(serviceHandle)); AppendWideText(detailBuffer, L" buffer="); AppendUnsignedText(detailBuffer, bufferSize); AppendWideText(detailBuffer, L" needed="); AppendUnsignedText(detailBuffer, bytesNeededPointer != nullptr ? *bytesNeededPointer : 0); if (resultValue != FALSE && configPointer != nullptr) { AppendWideText(detailBuffer, L" path="); AppendAnsiText(detailBuffer, configPointer->lpBinaryPathName); } })
        APIMON_SIMPLE_BOOL_HOOK(HookedEnumServicesStatusExW, g_enumServicesStatusExWOriginal, ks::winapi_monitor::EventCategory::Process, L"Advapi32", L"EnumServicesStatusExW",
            (SC_HANDLE managerHandle, SC_ENUM_TYPE infoLevel, DWORD serviceType, DWORD serviceState, LPBYTE servicesPointer, DWORD bufferSize, LPDWORD bytesNeededPointer, LPDWORD servicesReturnedPointer, LPDWORD resumeHandlePointer, LPCWSTR groupNamePointer),
            (managerHandle, infoLevel, serviceType, serviceState, servicesPointer, bufferSize, bytesNeededPointer, servicesReturnedPointer, resumeHandlePointer, groupNamePointer),
            { AppendWideText(detailBuffer, L"scm="); AppendHexText(detailBuffer, reinterpret_cast<std::uint64_t>(managerHandle)); AppendWideText(detailBuffer, L" type="); AppendHexText(detailBuffer, serviceType); AppendWideText(detailBuffer, L" state="); AppendHexText(detailBuffer, serviceState); AppendWideText(detailBuffer, L" returned="); AppendUnsignedText(detailBuffer, servicesReturnedPointer != nullptr ? *servicesReturnedPointer : 0); AppendWideText(detailBuffer, L" group="); AppendWideText(detailBuffer, groupNamePointer); })
        APIMON_SIMPLE_BOOL_HOOK(HookedEnumServicesStatusExA, g_enumServicesStatusExAOriginal, ks::winapi_monitor::EventCategory::Process, L"Advapi32", L"EnumServicesStatusExA",
            (SC_HANDLE managerHandle, SC_ENUM_TYPE infoLevel, DWORD serviceType, DWORD serviceState, LPBYTE servicesPointer, DWORD bufferSize, LPDWORD bytesNeededPointer, LPDWORD servicesReturnedPointer, LPDWORD resumeHandlePointer, LPCSTR groupNamePointer),
            (managerHandle, infoLevel, serviceType, serviceState, servicesPointer, bufferSize, bytesNeededPointer, servicesReturnedPointer, resumeHandlePointer, groupNamePointer),
            { AppendWideText(detailBuffer, L"scm="); AppendHexText(detailBuffer, reinterpret_cast<std::uint64_t>(managerHandle)); AppendWideText(detailBuffer, L" type="); AppendHexText(detailBuffer, serviceType); AppendWideText(detailBuffer, L" state="); AppendHexText(detailBuffer, serviceState); AppendWideText(detailBuffer, L" returned="); AppendUnsignedText(detailBuffer, servicesReturnedPointer != nullptr ? *servicesReturnedPointer : 0); AppendWideText(detailBuffer, L" group="); AppendAnsiText(detailBuffer, groupNamePointer); })

        // HookedHttpQueryExtras 作用：记录 WinHTTP/WinINet 查询、选项设置和 URL 直连入口；返回原始结果。
        APIMON_SIMPLE_BOOL_HOOK(HookedWinHttpQueryHeaders, g_winHttpQueryHeadersOriginal, ks::winapi_monitor::EventCategory::Network, L"Winhttp", L"WinHttpQueryHeaders",
            (HINTERNET requestHandle, DWORD infoLevel, LPCWSTR namePointer, LPVOID bufferPointer, LPDWORD bufferLengthPointer, LPDWORD indexPointer),
            (requestHandle, infoLevel, namePointer, bufferPointer, bufferLengthPointer, indexPointer),
            { AppendWideText(detailBuffer, L"request="); AppendHexText(detailBuffer, reinterpret_cast<std::uint64_t>(requestHandle)); AppendWideText(detailBuffer, L" level="); AppendHexText(detailBuffer, infoLevel); AppendWideText(detailBuffer, L" name="); AppendWideText(detailBuffer, namePointer); AppendWideText(detailBuffer, L" length="); AppendUnsignedText(detailBuffer, bufferLengthPointer != nullptr ? *bufferLengthPointer : 0); })
        APIMON_SIMPLE_BOOL_HOOK(HookedWinHttpQueryDataAvailable, g_winHttpQueryDataAvailableOriginal, ks::winapi_monitor::EventCategory::Network, L"Winhttp", L"WinHttpQueryDataAvailable",
            (HINTERNET requestHandle, LPDWORD bytesAvailablePointer), (requestHandle, bytesAvailablePointer),
            { AppendWideText(detailBuffer, L"request="); AppendHexText(detailBuffer, reinterpret_cast<std::uint64_t>(requestHandle)); AppendWideText(detailBuffer, L" available="); AppendUnsignedText(detailBuffer, bytesAvailablePointer != nullptr ? *bytesAvailablePointer : 0); })
        APIMON_SIMPLE_BOOL_HOOK(HookedWinHttpSetOption, g_winHttpSetOptionOriginal, ks::winapi_monitor::EventCategory::Network, L"Winhttp", L"WinHttpSetOption",
            (HINTERNET internetHandle, DWORD optionValue, LPVOID bufferPointer, DWORD bufferLength), (internetHandle, optionValue, bufferPointer, bufferLength),
            { AppendWideText(detailBuffer, L"handle="); AppendHexText(detailBuffer, reinterpret_cast<std::uint64_t>(internetHandle)); AppendWideText(detailBuffer, L" option="); AppendHexText(detailBuffer, optionValue); AppendWideText(detailBuffer, L" length="); AppendUnsignedText(detailBuffer, bufferLength); })
        APIMON_SIMPLE_HANDLE_HOOK(HINTERNET, HookedInternetOpenUrlW, g_internetOpenUrlWOriginal, ks::winapi_monitor::EventCategory::Network, L"Wininet", L"InternetOpenUrlW",
            (HINTERNET internetHandle, LPCWSTR urlPointer, LPCWSTR headersPointer, DWORD headersLength, DWORD flagsValue, DWORD_PTR contextValue),
            (internetHandle, urlPointer, headersPointer, headersLength, flagsValue, contextValue),
            { AppendWideText(detailBuffer, L"root="); AppendHexText(detailBuffer, reinterpret_cast<std::uint64_t>(internetHandle)); AppendWideText(detailBuffer, L" url="); AppendWideText(detailBuffer, urlPointer); AppendWideText(detailBuffer, L" flags="); AppendHexText(detailBuffer, flagsValue); AppendWideText(detailBuffer, L" handle="); AppendHexText(detailBuffer, reinterpret_cast<std::uint64_t>(resultHandle)); })
        APIMON_SIMPLE_HANDLE_HOOK(HINTERNET, HookedInternetOpenUrlA, g_internetOpenUrlAOriginal, ks::winapi_monitor::EventCategory::Network, L"Wininet", L"InternetOpenUrlA",
            (HINTERNET internetHandle, LPCSTR urlPointer, LPCSTR headersPointer, DWORD headersLength, DWORD flagsValue, DWORD_PTR contextValue),
            (internetHandle, urlPointer, headersPointer, headersLength, flagsValue, contextValue),
            { AppendWideText(detailBuffer, L"root="); AppendHexText(detailBuffer, reinterpret_cast<std::uint64_t>(internetHandle)); AppendWideText(detailBuffer, L" url="); AppendAnsiText(detailBuffer, urlPointer); AppendWideText(detailBuffer, L" flags="); AppendHexText(detailBuffer, flagsValue); AppendWideText(detailBuffer, L" handle="); AppendHexText(detailBuffer, reinterpret_cast<std::uint64_t>(resultHandle)); })
        APIMON_SIMPLE_BOOL_HOOK(HookedInternetQueryDataAvailable, g_internetQueryDataAvailableOriginal, ks::winapi_monitor::EventCategory::Network, L"Wininet", L"InternetQueryDataAvailable",
            (HINTERNET fileHandle, LPDWORD bytesAvailablePointer, DWORD flagsValue, DWORD_PTR contextValue), (fileHandle, bytesAvailablePointer, flagsValue, contextValue),
            { AppendWideText(detailBuffer, L"handle="); AppendHexText(detailBuffer, reinterpret_cast<std::uint64_t>(fileHandle)); AppendWideText(detailBuffer, L" available="); AppendUnsignedText(detailBuffer, bytesAvailablePointer != nullptr ? *bytesAvailablePointer : 0); AppendWideText(detailBuffer, L" flags="); AppendHexText(detailBuffer, flagsValue); })
        APIMON_SIMPLE_BOOL_HOOK(HookedInternetSetOptionW, g_internetSetOptionWOriginal, ks::winapi_monitor::EventCategory::Network, L"Wininet", L"InternetSetOptionW",
            (HINTERNET internetHandle, DWORD optionValue, LPVOID bufferPointer, DWORD bufferLength), (internetHandle, optionValue, bufferPointer, bufferLength),
            { AppendWideText(detailBuffer, L"handle="); AppendHexText(detailBuffer, reinterpret_cast<std::uint64_t>(internetHandle)); AppendWideText(detailBuffer, L" option="); AppendHexText(detailBuffer, optionValue); AppendWideText(detailBuffer, L" length="); AppendUnsignedText(detailBuffer, bufferLength); })
        APIMON_SIMPLE_BOOL_HOOK(HookedInternetSetOptionA, g_internetSetOptionAOriginal, ks::winapi_monitor::EventCategory::Network, L"Wininet", L"InternetSetOptionA",
            (HINTERNET internetHandle, DWORD optionValue, LPVOID bufferPointer, DWORD bufferLength), (internetHandle, optionValue, bufferPointer, bufferLength),
            { AppendWideText(detailBuffer, L"handle="); AppendHexText(detailBuffer, reinterpret_cast<std::uint64_t>(internetHandle)); AppendWideText(detailBuffer, L" option="); AppendHexText(detailBuffer, optionValue); AppendWideText(detailBuffer, L" length="); AppendUnsignedText(detailBuffer, bufferLength); })
        APIMON_SIMPLE_BOOL_HOOK(HookedInternetCrackUrlW, g_internetCrackUrlWOriginal, ks::winapi_monitor::EventCategory::Network, L"Wininet", L"InternetCrackUrlW",
            (LPCWSTR urlPointer, DWORD urlLength, DWORD flagsValue, LPURL_COMPONENTSW componentsPointer), (urlPointer, urlLength, flagsValue, componentsPointer),
            { AppendWideText(detailBuffer, L"url="); AppendWideText(detailBuffer, urlPointer, urlLength != 0 ? urlLength : static_cast<std::size_t>(-1)); AppendWideText(detailBuffer, L" flags="); AppendHexText(detailBuffer, flagsValue); })
        APIMON_SIMPLE_BOOL_HOOK(HookedInternetCrackUrlA, g_internetCrackUrlAOriginal, ks::winapi_monitor::EventCategory::Network, L"Wininet", L"InternetCrackUrlA",
            (LPCSTR urlPointer, DWORD urlLength, DWORD flagsValue, LPURL_COMPONENTSA componentsPointer), (urlPointer, urlLength, flagsValue, componentsPointer),
            { AppendWideText(detailBuffer, L"url="); AppendAnsiText(detailBuffer, urlPointer, urlLength != 0 ? urlLength : static_cast<std::size_t>(-1)); AppendWideText(detailBuffer, L" flags="); AppendHexText(detailBuffer, flagsValue); })

        HRESULT WINAPI HookedURLDownloadToFileW(LPUNKNOWN callerPointer, LPCWSTR urlPointer, LPCWSTR fileNamePointer, DWORD reservedValue, LPBINDSTATUSCALLBACK callbackPointer)
        {
            ScopedHookGuard guardValue;
            if (guardValue.bypass()) { return g_urlDownloadToFileWOriginal(callerPointer, urlPointer, fileNamePointer, reservedValue, callbackPointer); }
            const HRESULT resultValue = g_urlDownloadToFileWOriginal(callerPointer, urlPointer, fileNamePointer, reservedValue, callbackPointer);
            wchar_t detailBuffer[ks::winapi_monitor::kMaxDetailChars] = {};
            AppendWideText(detailBuffer, L"url=");
            AppendWideText(detailBuffer, urlPointer);
            AppendWideText(detailBuffer, L" file=");
            AppendWideText(detailBuffer, fileNamePointer);
            SendRawEventWithStatus(ks::winapi_monitor::EventCategory::Network, L"Urlmon", L"URLDownloadToFileW", resultValue, detailBuffer);
            return resultValue;
        }

        HRESULT WINAPI HookedURLDownloadToFileA(LPUNKNOWN callerPointer, LPCSTR urlPointer, LPCSTR fileNamePointer, DWORD reservedValue, LPBINDSTATUSCALLBACK callbackPointer)
        {
            ScopedHookGuard guardValue;
            if (guardValue.bypass()) { return g_urlDownloadToFileAOriginal(callerPointer, urlPointer, fileNamePointer, reservedValue, callbackPointer); }
            const HRESULT resultValue = g_urlDownloadToFileAOriginal(callerPointer, urlPointer, fileNamePointer, reservedValue, callbackPointer);
            wchar_t detailBuffer[ks::winapi_monitor::kMaxDetailChars] = {};
            AppendWideText(detailBuffer, L"url=");
            AppendAnsiText(detailBuffer, urlPointer);
            AppendWideText(detailBuffer, L" file=");
            AppendAnsiText(detailBuffer, fileNamePointer);
            SendRawEventWithStatus(ks::winapi_monitor::EventCategory::Network, L"Urlmon", L"URLDownloadToFileA", resultValue, detailBuffer);
            return resultValue;
        }

        // HookedCryptoLifecycle 作用：记录密钥导入/导出/销毁以及 CNG 密钥创建导入销毁；不记录密钥内容。
        APIMON_SIMPLE_BOOL_HOOK(HookedCryptImportKey, g_cryptImportKeyOriginal, ks::winapi_monitor::EventCategory::Process, L"Advapi32", L"CryptImportKey",
            (HCRYPTPROV providerHandle, const BYTE* dataPointer, DWORD dataLength, HCRYPTKEY publicKeyHandle, DWORD flagsValue, HCRYPTKEY* keyHandlePointer),
            (providerHandle, dataPointer, dataLength, publicKeyHandle, flagsValue, keyHandlePointer),
            { AppendWideText(detailBuffer, L"provider="); AppendHexText(detailBuffer, static_cast<std::uint64_t>(providerHandle)); AppendWideText(detailBuffer, L" bytes="); AppendUnsignedText(detailBuffer, dataLength); AppendWideText(detailBuffer, L" flags="); AppendHexText(detailBuffer, flagsValue); AppendWideText(detailBuffer, L" key="); AppendHexText(detailBuffer, keyHandlePointer != nullptr ? static_cast<std::uint64_t>(*keyHandlePointer) : 0); })
        APIMON_SIMPLE_BOOL_HOOK(HookedCryptExportKey, g_cryptExportKeyOriginal, ks::winapi_monitor::EventCategory::Process, L"Advapi32", L"CryptExportKey",
            (HCRYPTKEY keyHandle, HCRYPTKEY exportKeyHandle, DWORD blobType, DWORD flagsValue, BYTE* dataPointer, DWORD* dataLengthPointer),
            (keyHandle, exportKeyHandle, blobType, flagsValue, dataPointer, dataLengthPointer),
            { AppendWideText(detailBuffer, L"key="); AppendHexText(detailBuffer, static_cast<std::uint64_t>(keyHandle)); AppendWideText(detailBuffer, L" exportKey="); AppendHexText(detailBuffer, static_cast<std::uint64_t>(exportKeyHandle)); AppendWideText(detailBuffer, L" blobType="); AppendHexText(detailBuffer, blobType); AppendWideText(detailBuffer, L" flags="); AppendHexText(detailBuffer, flagsValue); AppendWideText(detailBuffer, L" bytes="); AppendUnsignedText(detailBuffer, dataLengthPointer != nullptr ? *dataLengthPointer : 0); })
        APIMON_SIMPLE_BOOL_HOOK(HookedCryptDestroyKey, g_cryptDestroyKeyOriginal, ks::winapi_monitor::EventCategory::Process, L"Advapi32", L"CryptDestroyKey",
            (HCRYPTKEY keyHandle), (keyHandle),
            { AppendWideText(detailBuffer, L"key="); AppendHexText(detailBuffer, static_cast<std::uint64_t>(keyHandle)); })
        APIMON_SIMPLE_BOOL_HOOK(HookedCryptDestroyHash, g_cryptDestroyHashOriginal, ks::winapi_monitor::EventCategory::Process, L"Advapi32", L"CryptDestroyHash",
            (HCRYPTHASH hashHandle), (hashHandle),
            { AppendWideText(detailBuffer, L"hash="); AppendHexText(detailBuffer, static_cast<std::uint64_t>(hashHandle)); })
        APIMON_SIMPLE_NTSTATUS_HOOK(HookedBCryptGenerateSymmetricKey, g_bCryptGenerateSymmetricKeyOriginal, ks::winapi_monitor::EventCategory::Process, L"Bcrypt", L"BCryptGenerateSymmetricKey",
            (BCRYPT_ALG_HANDLE algorithmHandle, BCRYPT_KEY_HANDLE* keyHandlePointer, PUCHAR keyObjectPointer, ULONG keyObjectLength, PUCHAR secretPointer, ULONG secretLength, ULONG flagsValue),
            (algorithmHandle, keyHandlePointer, keyObjectPointer, keyObjectLength, secretPointer, secretLength, flagsValue),
            { AppendWideText(detailBuffer, L"algHandle="); AppendHexText(detailBuffer, reinterpret_cast<std::uint64_t>(algorithmHandle)); AppendWideText(detailBuffer, L" objectLen="); AppendUnsignedText(detailBuffer, keyObjectLength); AppendWideText(detailBuffer, L" secretLen="); AppendUnsignedText(detailBuffer, secretLength); AppendWideText(detailBuffer, L" flags="); AppendHexText(detailBuffer, flagsValue); AppendWideText(detailBuffer, L" key="); AppendHexText(detailBuffer, keyHandlePointer != nullptr ? reinterpret_cast<std::uint64_t>(*keyHandlePointer) : 0); })
        APIMON_SIMPLE_NTSTATUS_HOOK(HookedBCryptImportKey, g_bCryptImportKeyOriginal, ks::winapi_monitor::EventCategory::Process, L"Bcrypt", L"BCryptImportKey",
            (BCRYPT_ALG_HANDLE algorithmHandle, BCRYPT_KEY_HANDLE importKeyHandle, LPCWSTR blobTypePointer, BCRYPT_KEY_HANDLE* keyHandlePointer, PUCHAR keyObjectPointer, ULONG keyObjectLength, PUCHAR inputPointer, ULONG inputLength, ULONG flagsValue),
            (algorithmHandle, importKeyHandle, blobTypePointer, keyHandlePointer, keyObjectPointer, keyObjectLength, inputPointer, inputLength, flagsValue),
            { AppendWideText(detailBuffer, L"algHandle="); AppendHexText(detailBuffer, reinterpret_cast<std::uint64_t>(algorithmHandle)); AppendWideText(detailBuffer, L" blobType="); AppendWideText(detailBuffer, blobTypePointer); AppendWideText(detailBuffer, L" inputLen="); AppendUnsignedText(detailBuffer, inputLength); AppendWideText(detailBuffer, L" flags="); AppendHexText(detailBuffer, flagsValue); AppendWideText(detailBuffer, L" key="); AppendHexText(detailBuffer, keyHandlePointer != nullptr ? reinterpret_cast<std::uint64_t>(*keyHandlePointer) : 0); })
        APIMON_SIMPLE_NTSTATUS_HOOK(HookedBCryptImportKeyPair, g_bCryptImportKeyPairOriginal, ks::winapi_monitor::EventCategory::Process, L"Bcrypt", L"BCryptImportKeyPair",
            (BCRYPT_ALG_HANDLE algorithmHandle, BCRYPT_KEY_HANDLE importKeyHandle, LPCWSTR blobTypePointer, BCRYPT_KEY_HANDLE* keyHandlePointer, PUCHAR inputPointer, ULONG inputLength, ULONG flagsValue),
            (algorithmHandle, importKeyHandle, blobTypePointer, keyHandlePointer, inputPointer, inputLength, flagsValue),
            { AppendWideText(detailBuffer, L"algHandle="); AppendHexText(detailBuffer, reinterpret_cast<std::uint64_t>(algorithmHandle)); AppendWideText(detailBuffer, L" blobType="); AppendWideText(detailBuffer, blobTypePointer); AppendWideText(detailBuffer, L" inputLen="); AppendUnsignedText(detailBuffer, inputLength); AppendWideText(detailBuffer, L" flags="); AppendHexText(detailBuffer, flagsValue); AppendWideText(detailBuffer, L" key="); AppendHexText(detailBuffer, keyHandlePointer != nullptr ? reinterpret_cast<std::uint64_t>(*keyHandlePointer) : 0); })
        APIMON_SIMPLE_NTSTATUS_HOOK(HookedBCryptDestroyKey, g_bCryptDestroyKeyOriginal, ks::winapi_monitor::EventCategory::Process, L"Bcrypt", L"BCryptDestroyKey",
            (BCRYPT_KEY_HANDLE keyHandle), (keyHandle),
            { BuildSimpleHandleDetail(detailBuffer, L"key", keyHandle); })

        APIMON_SIMPLE_HRESULT_HOOK(HookedCoInitializeEx, g_coInitializeExOriginal, L"Ole32", L"CoInitializeEx",
            (LPVOID reservedPointer, DWORD coInitValue), (reservedPointer, coInitValue),
            { AppendWideText(detailBuffer, L"coinit="); AppendHexText(detailBuffer, coInitValue); })
        APIMON_SIMPLE_HRESULT_HOOK(HookedCoInitializeSecurity, g_coInitializeSecurityOriginal, L"Ole32", L"CoInitializeSecurity",
            (PSECURITY_DESCRIPTOR securityDescriptorPointer, LONG authServiceCount, SOLE_AUTHENTICATION_SERVICE* authServicesPointer, void* reserved1Pointer, DWORD authnLevel, DWORD impLevel, void* authListPointer, DWORD capabilitiesValue, void* reserved3Pointer),
            (securityDescriptorPointer, authServiceCount, authServicesPointer, reserved1Pointer, authnLevel, impLevel, authListPointer, capabilitiesValue, reserved3Pointer),
            { AppendWideText(detailBuffer, L"authCount="); AppendUnsignedText(detailBuffer, static_cast<unsigned long long>(authServiceCount < 0 ? 0 : authServiceCount)); AppendWideText(detailBuffer, L" authnLevel="); AppendHexText(detailBuffer, authnLevel); AppendWideText(detailBuffer, L" impLevel="); AppendHexText(detailBuffer, impLevel); AppendWideText(detailBuffer, L" caps="); AppendHexText(detailBuffer, capabilitiesValue); })
        void WINAPI HookedCoUninitialize()
        {
            ScopedHookGuard guardValue;
            if (guardValue.bypass()) { g_coUninitializeOriginal(); return; }
            g_coUninitializeOriginal();
            SendRawEventWithStatus(ks::winapi_monitor::EventCategory::Process, L"Ole32", L"CoUninitialize", 0, L"");
        }

        // HookedThirdBatchProcess 作用：
        // - 输入：注入扩展、同步对象、环境变量、DLL 搜索路径和 User32 hook 参数；
        // - 处理：继续沿用“先调用原函数，再记录结果并恢复错误码”的 APIMonitor 旧模式；
        // - 返回：保持各 WinAPI 原始返回语义。
        APIMON_SIMPLE_HANDLE_HOOK(HANDLE, HookedCreateRemoteThreadEx, g_createRemoteThreadExOriginal, ks::winapi_monitor::EventCategory::Process, L"KernelBase", L"CreateRemoteThreadEx",
            (HANDLE processHandle, LPSECURITY_ATTRIBUTES threadAttributes, SIZE_T stackSize, LPTHREAD_START_ROUTINE startAddress, LPVOID parameterPointer, DWORD creationFlags, LPPROC_THREAD_ATTRIBUTE_LIST attributeListPointer, LPDWORD threadIdPointer),
            (processHandle, threadAttributes, stackSize, startAddress, parameterPointer, creationFlags, attributeListPointer, threadIdPointer),
            { AppendWideText(detailBuffer, L"process="); AppendHexText(detailBuffer, reinterpret_cast<std::uint64_t>(processHandle)); AppendWideText(detailBuffer, L" start="); AppendHexText(detailBuffer, reinterpret_cast<std::uint64_t>(startAddress)); AppendWideText(detailBuffer, L" param="); AppendHexText(detailBuffer, reinterpret_cast<std::uint64_t>(parameterPointer)); AppendWideText(detailBuffer, L" flags="); AppendHexText(detailBuffer, creationFlags); AppendWideText(detailBuffer, L" tid="); AppendUnsignedText(detailBuffer, threadIdPointer != nullptr ? *threadIdPointer : 0); AppendWideText(detailBuffer, L" thread="); AppendHexText(detailBuffer, reinterpret_cast<std::uint64_t>(resultHandle)); })

        BOOLEAN WINAPI HookedCreateSymbolicLinkW(LPCWSTR linkPointer, LPCWSTR targetPointer, DWORD flagsValue)
        {
            ScopedHookGuard guardValue;
            if (guardValue.bypass()) { return g_createSymbolicLinkWOriginal(linkPointer, targetPointer, flagsValue); }
            const BOOLEAN resultValue = g_createSymbolicLinkWOriginal(linkPointer, targetPointer, flagsValue);
            const DWORD lastError = ::GetLastError();
            wchar_t detailBuffer[ks::winapi_monitor::kMaxDetailChars] = {};
            BuildTwoPathDetailW(detailBuffer, linkPointer, targetPointer, flagsValue);
            SendRawEventWithStatus(ks::winapi_monitor::EventCategory::File, L"KernelBase", L"CreateSymbolicLinkW", resultValue != FALSE ? 0 : lastError, detailBuffer);
            ::SetLastError(lastError);
            return resultValue;
        }

        BOOLEAN WINAPI HookedCreateSymbolicLinkA(LPCSTR linkPointer, LPCSTR targetPointer, DWORD flagsValue)
        {
            ScopedHookGuard guardValue;
            if (guardValue.bypass()) { return g_createSymbolicLinkAOriginal(linkPointer, targetPointer, flagsValue); }
            const BOOLEAN resultValue = g_createSymbolicLinkAOriginal(linkPointer, targetPointer, flagsValue);
            const DWORD lastError = ::GetLastError();
            wchar_t detailBuffer[ks::winapi_monitor::kMaxDetailChars] = {};
            BuildTwoPathDetailA(detailBuffer, linkPointer, targetPointer, flagsValue);
            SendRawEventWithStatus(ks::winapi_monitor::EventCategory::File, L"KernelBase", L"CreateSymbolicLinkA", resultValue != FALSE ? 0 : lastError, detailBuffer);
            ::SetLastError(lastError);
            return resultValue;
        }

        DWORD WINAPI HookedGetFinalPathNameByHandleW(HANDLE fileHandle, LPWSTR filePathPointer, DWORD filePathSize, DWORD flagsValue)
        {
            ScopedHookGuard guardValue;
            if (guardValue.bypass()) { return g_getFinalPathNameByHandleWOriginal(fileHandle, filePathPointer, filePathSize, flagsValue); }
            const DWORD resultValue = g_getFinalPathNameByHandleWOriginal(fileHandle, filePathPointer, filePathSize, flagsValue);
            const DWORD lastError = ::GetLastError();
            wchar_t detailBuffer[ks::winapi_monitor::kMaxDetailChars] = {};
            AppendWideText(detailBuffer, L"file=");
            AppendHexText(detailBuffer, reinterpret_cast<std::uint64_t>(fileHandle));
            AppendWideText(detailBuffer, L" flags=");
            AppendHexText(detailBuffer, flagsValue);
            AppendWideText(detailBuffer, L" path=");
            if (resultValue != 0 && resultValue < filePathSize) { AppendWideText(detailBuffer, filePathPointer, resultValue); }
            SendRawEventWithStatus(ks::winapi_monitor::EventCategory::File, L"KernelBase", L"GetFinalPathNameByHandleW", resultValue != 0 ? 0 : lastError, detailBuffer);
            ::SetLastError(lastError);
            return resultValue;
        }

        DWORD WINAPI HookedGetFinalPathNameByHandleA(HANDLE fileHandle, LPSTR filePathPointer, DWORD filePathSize, DWORD flagsValue)
        {
            ScopedHookGuard guardValue;
            if (guardValue.bypass()) { return g_getFinalPathNameByHandleAOriginal(fileHandle, filePathPointer, filePathSize, flagsValue); }
            const DWORD resultValue = g_getFinalPathNameByHandleAOriginal(fileHandle, filePathPointer, filePathSize, flagsValue);
            const DWORD lastError = ::GetLastError();
            wchar_t detailBuffer[ks::winapi_monitor::kMaxDetailChars] = {};
            AppendWideText(detailBuffer, L"file=");
            AppendHexText(detailBuffer, reinterpret_cast<std::uint64_t>(fileHandle));
            AppendWideText(detailBuffer, L" flags=");
            AppendHexText(detailBuffer, flagsValue);
            AppendWideText(detailBuffer, L" path=");
            if (resultValue != 0 && resultValue < filePathSize) { AppendAnsiText(detailBuffer, filePathPointer, resultValue); }
            SendRawEventWithStatus(ks::winapi_monitor::EventCategory::File, L"KernelBase", L"GetFinalPathNameByHandleA", resultValue != 0 ? 0 : lastError, detailBuffer);
            ::SetLastError(lastError);
            return resultValue;
        }

        APIMON_SIMPLE_BOOL_HOOK(HookedGetFileSizeEx, g_getFileSizeExOriginal, ks::winapi_monitor::EventCategory::File, L"KernelBase", L"GetFileSizeEx",
            (HANDLE fileHandle, PLARGE_INTEGER fileSizePointer), (fileHandle, fileSizePointer),
            { AppendWideText(detailBuffer, L"file="); AppendHexText(detailBuffer, reinterpret_cast<std::uint64_t>(fileHandle)); AppendWideText(detailBuffer, L" size="); AppendHexText(detailBuffer, fileSizePointer != nullptr ? static_cast<std::uint64_t>(fileSizePointer->QuadPart) : 0); })
        APIMON_SIMPLE_BOOL_HOOK(HookedSetFilePointerEx, g_setFilePointerExOriginal, ks::winapi_monitor::EventCategory::File, L"KernelBase", L"SetFilePointerEx",
            (HANDLE fileHandle, LARGE_INTEGER distanceValue, PLARGE_INTEGER newPointer, DWORD moveMethod), (fileHandle, distanceValue, newPointer, moveMethod),
            { AppendWideText(detailBuffer, L"file="); AppendHexText(detailBuffer, reinterpret_cast<std::uint64_t>(fileHandle)); AppendWideText(detailBuffer, L" distance="); AppendHexText(detailBuffer, static_cast<std::uint64_t>(distanceValue.QuadPart)); AppendWideText(detailBuffer, L" method="); AppendUnsignedText(detailBuffer, moveMethod); AppendWideText(detailBuffer, L" new="); AppendHexText(detailBuffer, newPointer != nullptr ? static_cast<std::uint64_t>(newPointer->QuadPart) : 0); })

        APIMON_SIMPLE_HANDLE_HOOK(HANDLE, HookedCreateNamedPipeW, g_createNamedPipeWOriginal, ks::winapi_monitor::EventCategory::File, L"KernelBase", L"CreateNamedPipeW",
            (LPCWSTR namePointer, DWORD openMode, DWORD pipeMode, DWORD maxInstances, DWORD outBufferSize, DWORD inBufferSize, DWORD defaultTimeout, LPSECURITY_ATTRIBUTES securityAttributes),
            (namePointer, openMode, pipeMode, maxInstances, outBufferSize, inBufferSize, defaultTimeout, securityAttributes),
            { AppendWideText(detailBuffer, L"name="); AppendWideText(detailBuffer, namePointer); AppendWideText(detailBuffer, L" openMode="); AppendHexText(detailBuffer, openMode); AppendWideText(detailBuffer, L" pipeMode="); AppendHexText(detailBuffer, pipeMode); AppendWideText(detailBuffer, L" instances="); AppendUnsignedText(detailBuffer, maxInstances); AppendWideText(detailBuffer, L" handle="); AppendHexText(detailBuffer, reinterpret_cast<std::uint64_t>(resultHandle)); })
        APIMON_SIMPLE_HANDLE_HOOK(HANDLE, HookedCreateNamedPipeA, g_createNamedPipeAOriginal, ks::winapi_monitor::EventCategory::File, L"KernelBase", L"CreateNamedPipeA",
            (LPCSTR namePointer, DWORD openMode, DWORD pipeMode, DWORD maxInstances, DWORD outBufferSize, DWORD inBufferSize, DWORD defaultTimeout, LPSECURITY_ATTRIBUTES securityAttributes),
            (namePointer, openMode, pipeMode, maxInstances, outBufferSize, inBufferSize, defaultTimeout, securityAttributes),
            { AppendWideText(detailBuffer, L"name="); AppendAnsiText(detailBuffer, namePointer); AppendWideText(detailBuffer, L" openMode="); AppendHexText(detailBuffer, openMode); AppendWideText(detailBuffer, L" pipeMode="); AppendHexText(detailBuffer, pipeMode); AppendWideText(detailBuffer, L" instances="); AppendUnsignedText(detailBuffer, maxInstances); AppendWideText(detailBuffer, L" handle="); AppendHexText(detailBuffer, reinterpret_cast<std::uint64_t>(resultHandle)); })
        APIMON_SIMPLE_BOOL_HOOK(HookedConnectNamedPipe, g_connectNamedPipeOriginal, ks::winapi_monitor::EventCategory::File, L"KernelBase", L"ConnectNamedPipe",
            (HANDLE pipeHandle, LPOVERLAPPED overlappedPointer), (pipeHandle, overlappedPointer),
            { BuildSimpleHandleDetail(detailBuffer, L"pipe", pipeHandle); })
        APIMON_SIMPLE_BOOL_HOOK(HookedDisconnectNamedPipe, g_disconnectNamedPipeOriginal, ks::winapi_monitor::EventCategory::File, L"KernelBase", L"DisconnectNamedPipe",
            (HANDLE pipeHandle), (pipeHandle),
            { BuildSimpleHandleDetail(detailBuffer, L"pipe", pipeHandle); })
        APIMON_SIMPLE_BOOL_HOOK(HookedWaitNamedPipeW, g_waitNamedPipeWOriginal, ks::winapi_monitor::EventCategory::File, L"KernelBase", L"WaitNamedPipeW",
            (LPCWSTR namePointer, DWORD timeoutValue), (namePointer, timeoutValue),
            { AppendWideText(detailBuffer, L"name="); AppendWideText(detailBuffer, namePointer); AppendWideText(detailBuffer, L" timeout="); AppendUnsignedText(detailBuffer, timeoutValue); })
        APIMON_SIMPLE_BOOL_HOOK(HookedWaitNamedPipeA, g_waitNamedPipeAOriginal, ks::winapi_monitor::EventCategory::File, L"KernelBase", L"WaitNamedPipeA",
            (LPCSTR namePointer, DWORD timeoutValue), (namePointer, timeoutValue),
            { AppendWideText(detailBuffer, L"name="); AppendAnsiText(detailBuffer, namePointer); AppendWideText(detailBuffer, L" timeout="); AppendUnsignedText(detailBuffer, timeoutValue); })
        APIMON_SIMPLE_BOOL_HOOK(HookedTransactNamedPipe, g_transactNamedPipeOriginal, ks::winapi_monitor::EventCategory::File, L"KernelBase", L"TransactNamedPipe",
            (HANDLE pipeHandle, LPVOID inBufferPointer, DWORD inBufferSize, LPVOID outBufferPointer, DWORD outBufferSize, LPDWORD bytesReadPointer, LPOVERLAPPED overlappedPointer),
            (pipeHandle, inBufferPointer, inBufferSize, outBufferPointer, outBufferSize, bytesReadPointer, overlappedPointer),
            { AppendWideText(detailBuffer, L"pipe="); AppendHexText(detailBuffer, reinterpret_cast<std::uint64_t>(pipeHandle)); AppendWideText(detailBuffer, L" in="); AppendUnsignedText(detailBuffer, inBufferSize); AppendWideText(detailBuffer, L" out="); AppendUnsignedText(detailBuffer, outBufferSize); AppendWideText(detailBuffer, L" read="); AppendUnsignedText(detailBuffer, bytesReadPointer != nullptr ? *bytesReadPointer : 0); })

        APIMON_SIMPLE_HANDLE_HOOK(HANDLE, HookedCreateMutexW, g_createMutexWOriginal, ks::winapi_monitor::EventCategory::Process, L"KernelBase", L"CreateMutexW",
            (LPSECURITY_ATTRIBUTES securityAttributes, BOOL initialOwner, LPCWSTR namePointer), (securityAttributes, initialOwner, namePointer),
            { AppendWideText(detailBuffer, L"name="); AppendWideText(detailBuffer, namePointer); AppendWideText(detailBuffer, L" initialOwner="); AppendUnsignedText(detailBuffer, initialOwner != FALSE ? 1ULL : 0ULL); AppendWideText(detailBuffer, L" handle="); AppendHexText(detailBuffer, reinterpret_cast<std::uint64_t>(resultHandle)); })
        APIMON_SIMPLE_HANDLE_HOOK(HANDLE, HookedCreateMutexA, g_createMutexAOriginal, ks::winapi_monitor::EventCategory::Process, L"KernelBase", L"CreateMutexA",
            (LPSECURITY_ATTRIBUTES securityAttributes, BOOL initialOwner, LPCSTR namePointer), (securityAttributes, initialOwner, namePointer),
            { AppendWideText(detailBuffer, L"name="); AppendAnsiText(detailBuffer, namePointer); AppendWideText(detailBuffer, L" initialOwner="); AppendUnsignedText(detailBuffer, initialOwner != FALSE ? 1ULL : 0ULL); AppendWideText(detailBuffer, L" handle="); AppendHexText(detailBuffer, reinterpret_cast<std::uint64_t>(resultHandle)); })
        APIMON_SIMPLE_HANDLE_HOOK(HANDLE, HookedOpenMutexW, g_openMutexWOriginal, ks::winapi_monitor::EventCategory::Process, L"KernelBase", L"OpenMutexW",
            (DWORD desiredAccess, BOOL inheritHandle, LPCWSTR namePointer), (desiredAccess, inheritHandle, namePointer),
            { AppendWideText(detailBuffer, L"name="); AppendWideText(detailBuffer, namePointer); AppendWideText(detailBuffer, L" access="); AppendHexText(detailBuffer, desiredAccess); AppendWideText(detailBuffer, L" handle="); AppendHexText(detailBuffer, reinterpret_cast<std::uint64_t>(resultHandle)); })
        APIMON_SIMPLE_HANDLE_HOOK(HANDLE, HookedOpenMutexA, g_openMutexAOriginal, ks::winapi_monitor::EventCategory::Process, L"KernelBase", L"OpenMutexA",
            (DWORD desiredAccess, BOOL inheritHandle, LPCSTR namePointer), (desiredAccess, inheritHandle, namePointer),
            { AppendWideText(detailBuffer, L"name="); AppendAnsiText(detailBuffer, namePointer); AppendWideText(detailBuffer, L" access="); AppendHexText(detailBuffer, desiredAccess); AppendWideText(detailBuffer, L" handle="); AppendHexText(detailBuffer, reinterpret_cast<std::uint64_t>(resultHandle)); })

        APIMON_SIMPLE_HANDLE_HOOK(HANDLE, HookedCreateEventW, g_createEventWOriginal, ks::winapi_monitor::EventCategory::Process, L"KernelBase", L"CreateEventW",
            (LPSECURITY_ATTRIBUTES securityAttributes, BOOL manualReset, BOOL initialState, LPCWSTR namePointer), (securityAttributes, manualReset, initialState, namePointer),
            { AppendWideText(detailBuffer, L"name="); AppendWideText(detailBuffer, namePointer); AppendWideText(detailBuffer, L" manual="); AppendUnsignedText(detailBuffer, manualReset != FALSE ? 1ULL : 0ULL); AppendWideText(detailBuffer, L" initial="); AppendUnsignedText(detailBuffer, initialState != FALSE ? 1ULL : 0ULL); AppendWideText(detailBuffer, L" handle="); AppendHexText(detailBuffer, reinterpret_cast<std::uint64_t>(resultHandle)); })
        APIMON_SIMPLE_HANDLE_HOOK(HANDLE, HookedCreateEventA, g_createEventAOriginal, ks::winapi_monitor::EventCategory::Process, L"KernelBase", L"CreateEventA",
            (LPSECURITY_ATTRIBUTES securityAttributes, BOOL manualReset, BOOL initialState, LPCSTR namePointer), (securityAttributes, manualReset, initialState, namePointer),
            { AppendWideText(detailBuffer, L"name="); AppendAnsiText(detailBuffer, namePointer); AppendWideText(detailBuffer, L" manual="); AppendUnsignedText(detailBuffer, manualReset != FALSE ? 1ULL : 0ULL); AppendWideText(detailBuffer, L" initial="); AppendUnsignedText(detailBuffer, initialState != FALSE ? 1ULL : 0ULL); AppendWideText(detailBuffer, L" handle="); AppendHexText(detailBuffer, reinterpret_cast<std::uint64_t>(resultHandle)); })
        APIMON_SIMPLE_HANDLE_HOOK(HANDLE, HookedOpenEventW, g_openEventWOriginal, ks::winapi_monitor::EventCategory::Process, L"KernelBase", L"OpenEventW",
            (DWORD desiredAccess, BOOL inheritHandle, LPCWSTR namePointer), (desiredAccess, inheritHandle, namePointer),
            { AppendWideText(detailBuffer, L"name="); AppendWideText(detailBuffer, namePointer); AppendWideText(detailBuffer, L" access="); AppendHexText(detailBuffer, desiredAccess); AppendWideText(detailBuffer, L" handle="); AppendHexText(detailBuffer, reinterpret_cast<std::uint64_t>(resultHandle)); })
        APIMON_SIMPLE_HANDLE_HOOK(HANDLE, HookedOpenEventA, g_openEventAOriginal, ks::winapi_monitor::EventCategory::Process, L"KernelBase", L"OpenEventA",
            (DWORD desiredAccess, BOOL inheritHandle, LPCSTR namePointer), (desiredAccess, inheritHandle, namePointer),
            { AppendWideText(detailBuffer, L"name="); AppendAnsiText(detailBuffer, namePointer); AppendWideText(detailBuffer, L" access="); AppendHexText(detailBuffer, desiredAccess); AppendWideText(detailBuffer, L" handle="); AppendHexText(detailBuffer, reinterpret_cast<std::uint64_t>(resultHandle)); })
        APIMON_SIMPLE_HANDLE_HOOK(HANDLE, HookedCreateSemaphoreW, g_createSemaphoreWOriginal, ks::winapi_monitor::EventCategory::Process, L"KernelBase", L"CreateSemaphoreW",
            (LPSECURITY_ATTRIBUTES securityAttributes, LONG initialCount, LONG maximumCount, LPCWSTR namePointer), (securityAttributes, initialCount, maximumCount, namePointer),
            { AppendWideText(detailBuffer, L"name="); AppendWideText(detailBuffer, namePointer); AppendWideText(detailBuffer, L" initial="); AppendUnsignedText(detailBuffer, static_cast<unsigned long long>(initialCount < 0 ? 0 : initialCount)); AppendWideText(detailBuffer, L" max="); AppendUnsignedText(detailBuffer, static_cast<unsigned long long>(maximumCount < 0 ? 0 : maximumCount)); AppendWideText(detailBuffer, L" handle="); AppendHexText(detailBuffer, reinterpret_cast<std::uint64_t>(resultHandle)); })
        APIMON_SIMPLE_HANDLE_HOOK(HANDLE, HookedCreateSemaphoreA, g_createSemaphoreAOriginal, ks::winapi_monitor::EventCategory::Process, L"KernelBase", L"CreateSemaphoreA",
            (LPSECURITY_ATTRIBUTES securityAttributes, LONG initialCount, LONG maximumCount, LPCSTR namePointer), (securityAttributes, initialCount, maximumCount, namePointer),
            { AppendWideText(detailBuffer, L"name="); AppendAnsiText(detailBuffer, namePointer); AppendWideText(detailBuffer, L" initial="); AppendUnsignedText(detailBuffer, static_cast<unsigned long long>(initialCount < 0 ? 0 : initialCount)); AppendWideText(detailBuffer, L" max="); AppendUnsignedText(detailBuffer, static_cast<unsigned long long>(maximumCount < 0 ? 0 : maximumCount)); AppendWideText(detailBuffer, L" handle="); AppendHexText(detailBuffer, reinterpret_cast<std::uint64_t>(resultHandle)); })
        APIMON_SIMPLE_HANDLE_HOOK(HANDLE, HookedOpenSemaphoreW, g_openSemaphoreWOriginal, ks::winapi_monitor::EventCategory::Process, L"KernelBase", L"OpenSemaphoreW",
            (DWORD desiredAccess, BOOL inheritHandle, LPCWSTR namePointer), (desiredAccess, inheritHandle, namePointer),
            { AppendWideText(detailBuffer, L"name="); AppendWideText(detailBuffer, namePointer); AppendWideText(detailBuffer, L" access="); AppendHexText(detailBuffer, desiredAccess); AppendWideText(detailBuffer, L" handle="); AppendHexText(detailBuffer, reinterpret_cast<std::uint64_t>(resultHandle)); })
        APIMON_SIMPLE_HANDLE_HOOK(HANDLE, HookedOpenSemaphoreA, g_openSemaphoreAOriginal, ks::winapi_monitor::EventCategory::Process, L"KernelBase", L"OpenSemaphoreA",
            (DWORD desiredAccess, BOOL inheritHandle, LPCSTR namePointer), (desiredAccess, inheritHandle, namePointer),
            { AppendWideText(detailBuffer, L"name="); AppendAnsiText(detailBuffer, namePointer); AppendWideText(detailBuffer, L" access="); AppendHexText(detailBuffer, desiredAccess); AppendWideText(detailBuffer, L" handle="); AppendHexText(detailBuffer, reinterpret_cast<std::uint64_t>(resultHandle)); })

        DWORD WINAPI HookedWaitForSingleObject(HANDLE handleValue, DWORD millisecondsValue)
        {
            ScopedHookGuard guardValue;
            if (guardValue.bypass()) { return g_waitForSingleObjectOriginal(handleValue, millisecondsValue); }
            const DWORD resultValue = g_waitForSingleObjectOriginal(handleValue, millisecondsValue);
            const DWORD lastError = ::GetLastError();
            wchar_t detailBuffer[ks::winapi_monitor::kMaxDetailChars] = {};
            AppendWideText(detailBuffer, L"handle=");
            AppendHexText(detailBuffer, reinterpret_cast<std::uint64_t>(handleValue));
            AppendWideText(detailBuffer, L" timeout=");
            AppendUnsignedText(detailBuffer, millisecondsValue);
            AppendWideText(detailBuffer, L" result=");
            AppendHexText(detailBuffer, resultValue);
            SendRawEventWithStatus(ks::winapi_monitor::EventCategory::Process, L"KernelBase", L"WaitForSingleObject", resultValue != WAIT_FAILED ? 0 : lastError, detailBuffer);
            ::SetLastError(lastError);
            return resultValue;
        }

        DWORD WINAPI HookedWaitForMultipleObjects(DWORD countValue, const HANDLE* handlesPointer, BOOL waitAll, DWORD millisecondsValue)
        {
            ScopedHookGuard guardValue;
            if (guardValue.bypass()) { return g_waitForMultipleObjectsOriginal(countValue, handlesPointer, waitAll, millisecondsValue); }
            const DWORD resultValue = g_waitForMultipleObjectsOriginal(countValue, handlesPointer, waitAll, millisecondsValue);
            const DWORD lastError = ::GetLastError();
            wchar_t detailBuffer[ks::winapi_monitor::kMaxDetailChars] = {};
            AppendWideText(detailBuffer, L"count=");
            AppendUnsignedText(detailBuffer, countValue);
            AppendWideText(detailBuffer, L" waitAll=");
            AppendUnsignedText(detailBuffer, waitAll != FALSE ? 1ULL : 0ULL);
            AppendWideText(detailBuffer, L" timeout=");
            AppendUnsignedText(detailBuffer, millisecondsValue);
            AppendWideText(detailBuffer, L" result=");
            AppendHexText(detailBuffer, resultValue);
            SendRawEventWithStatus(ks::winapi_monitor::EventCategory::Process, L"KernelBase", L"WaitForMultipleObjects", resultValue != WAIT_FAILED ? 0 : lastError, detailBuffer);
            ::SetLastError(lastError);
            return resultValue;
        }

        APIMON_SIMPLE_BOOL_HOOK(HookedSetEvent, g_setEventOriginal, ks::winapi_monitor::EventCategory::Process, L"KernelBase", L"SetEvent",
            (HANDLE eventHandle), (eventHandle),
            { BuildSimpleHandleDetail(detailBuffer, L"event", eventHandle); })
        APIMON_SIMPLE_BOOL_HOOK(HookedResetEvent, g_resetEventOriginal, ks::winapi_monitor::EventCategory::Process, L"KernelBase", L"ResetEvent",
            (HANDLE eventHandle), (eventHandle),
            { BuildSimpleHandleDetail(detailBuffer, L"event", eventHandle); })
        APIMON_SIMPLE_BOOL_HOOK(HookedReleaseMutex, g_releaseMutexOriginal, ks::winapi_monitor::EventCategory::Process, L"KernelBase", L"ReleaseMutex",
            (HANDLE mutexHandle), (mutexHandle),
            { BuildSimpleHandleDetail(detailBuffer, L"mutex", mutexHandle); })
        APIMON_SIMPLE_BOOL_HOOK(HookedReleaseSemaphore, g_releaseSemaphoreOriginal, ks::winapi_monitor::EventCategory::Process, L"KernelBase", L"ReleaseSemaphore",
            (HANDLE semaphoreHandle, LONG releaseCount, LPLONG previousCountPointer), (semaphoreHandle, releaseCount, previousCountPointer),
            { AppendWideText(detailBuffer, L"semaphore="); AppendHexText(detailBuffer, reinterpret_cast<std::uint64_t>(semaphoreHandle)); AppendWideText(detailBuffer, L" release="); AppendUnsignedText(detailBuffer, static_cast<unsigned long long>(releaseCount < 0 ? 0 : releaseCount)); AppendWideText(detailBuffer, L" previous="); AppendUnsignedText(detailBuffer, previousCountPointer != nullptr ? static_cast<unsigned long long>(*previousCountPointer) : 0ULL); })

        DWORD WINAPI HookedGetEnvironmentVariableW(LPCWSTR namePointer, LPWSTR bufferPointer, DWORD sizeValue)
        {
            ScopedHookGuard guardValue;
            if (guardValue.bypass()) { return g_getEnvironmentVariableWOriginal(namePointer, bufferPointer, sizeValue); }
            const DWORD resultValue = g_getEnvironmentVariableWOriginal(namePointer, bufferPointer, sizeValue);
            const DWORD lastError = ::GetLastError();
            wchar_t detailBuffer[ks::winapi_monitor::kMaxDetailChars] = {};
            AppendWideText(detailBuffer, L"name=");
            AppendWideText(detailBuffer, namePointer);
            AppendWideText(detailBuffer, L" size=");
            AppendUnsignedText(detailBuffer, sizeValue);
            AppendWideText(detailBuffer, L" resultLen=");
            AppendUnsignedText(detailBuffer, resultValue);
            SendRawEventWithStatus(ks::winapi_monitor::EventCategory::Process, L"KernelBase", L"GetEnvironmentVariableW", resultValue != 0 ? 0 : lastError, detailBuffer);
            ::SetLastError(lastError);
            return resultValue;
        }

        DWORD WINAPI HookedGetEnvironmentVariableA(LPCSTR namePointer, LPSTR bufferPointer, DWORD sizeValue)
        {
            ScopedHookGuard guardValue;
            if (guardValue.bypass()) { return g_getEnvironmentVariableAOriginal(namePointer, bufferPointer, sizeValue); }
            const DWORD resultValue = g_getEnvironmentVariableAOriginal(namePointer, bufferPointer, sizeValue);
            const DWORD lastError = ::GetLastError();
            wchar_t detailBuffer[ks::winapi_monitor::kMaxDetailChars] = {};
            AppendWideText(detailBuffer, L"name=");
            AppendAnsiText(detailBuffer, namePointer);
            AppendWideText(detailBuffer, L" size=");
            AppendUnsignedText(detailBuffer, sizeValue);
            AppendWideText(detailBuffer, L" resultLen=");
            AppendUnsignedText(detailBuffer, resultValue);
            SendRawEventWithStatus(ks::winapi_monitor::EventCategory::Process, L"KernelBase", L"GetEnvironmentVariableA", resultValue != 0 ? 0 : lastError, detailBuffer);
            ::SetLastError(lastError);
            return resultValue;
        }

        APIMON_SIMPLE_BOOL_HOOK(HookedSetEnvironmentVariableW, g_setEnvironmentVariableWOriginal, ks::winapi_monitor::EventCategory::Process, L"KernelBase", L"SetEnvironmentVariableW",
            (LPCWSTR namePointer, LPCWSTR valuePointer), (namePointer, valuePointer),
            { AppendWideText(detailBuffer, L"name="); AppendWideText(detailBuffer, namePointer); AppendWideText(detailBuffer, L" valueLen="); AppendUnsignedText(detailBuffer, valuePointer != nullptr ? static_cast<unsigned long long>(::wcslen(valuePointer)) : 0ULL); })
        APIMON_SIMPLE_BOOL_HOOK(HookedSetEnvironmentVariableA, g_setEnvironmentVariableAOriginal, ks::winapi_monitor::EventCategory::Process, L"KernelBase", L"SetEnvironmentVariableA",
            (LPCSTR namePointer, LPCSTR valuePointer), (namePointer, valuePointer),
            { AppendWideText(detailBuffer, L"name="); AppendAnsiText(detailBuffer, namePointer); AppendWideText(detailBuffer, L" valueLen="); AppendUnsignedText(detailBuffer, valuePointer != nullptr ? static_cast<unsigned long long>(::strlen(valuePointer)) : 0ULL); })

        DWORD WINAPI HookedExpandEnvironmentStringsW(LPCWSTR sourcePointer, LPWSTR destinationPointer, DWORD sizeValue)
        {
            ScopedHookGuard guardValue;
            if (guardValue.bypass()) { return g_expandEnvironmentStringsWOriginal(sourcePointer, destinationPointer, sizeValue); }
            const DWORD resultValue = g_expandEnvironmentStringsWOriginal(sourcePointer, destinationPointer, sizeValue);
            const DWORD lastError = ::GetLastError();
            wchar_t detailBuffer[ks::winapi_monitor::kMaxDetailChars] = {};
            AppendWideText(detailBuffer, L"src=");
            AppendWideText(detailBuffer, sourcePointer);
            AppendWideText(detailBuffer, L" size=");
            AppendUnsignedText(detailBuffer, sizeValue);
            AppendWideText(detailBuffer, L" resultLen=");
            AppendUnsignedText(detailBuffer, resultValue);
            SendRawEventWithStatus(ks::winapi_monitor::EventCategory::Process, L"KernelBase", L"ExpandEnvironmentStringsW", resultValue != 0 ? 0 : lastError, detailBuffer);
            ::SetLastError(lastError);
            return resultValue;
        }

        DWORD WINAPI HookedExpandEnvironmentStringsA(LPCSTR sourcePointer, LPSTR destinationPointer, DWORD sizeValue)
        {
            ScopedHookGuard guardValue;
            if (guardValue.bypass()) { return g_expandEnvironmentStringsAOriginal(sourcePointer, destinationPointer, sizeValue); }
            const DWORD resultValue = g_expandEnvironmentStringsAOriginal(sourcePointer, destinationPointer, sizeValue);
            const DWORD lastError = ::GetLastError();
            wchar_t detailBuffer[ks::winapi_monitor::kMaxDetailChars] = {};
            AppendWideText(detailBuffer, L"src=");
            AppendAnsiText(detailBuffer, sourcePointer);
            AppendWideText(detailBuffer, L" size=");
            AppendUnsignedText(detailBuffer, sizeValue);
            AppendWideText(detailBuffer, L" resultLen=");
            AppendUnsignedText(detailBuffer, resultValue);
            SendRawEventWithStatus(ks::winapi_monitor::EventCategory::Process, L"KernelBase", L"ExpandEnvironmentStringsA", resultValue != 0 ? 0 : lastError, detailBuffer);
            ::SetLastError(lastError);
            return resultValue;
        }

        APIMON_SIMPLE_BOOL_HOOK(HookedSetDllDirectoryW, g_setDllDirectoryWOriginal, ks::winapi_monitor::EventCategory::Loader, L"Kernel32", L"SetDllDirectoryW",
            (LPCWSTR pathPointer), (pathPointer),
            { AppendWideText(detailBuffer, L"path="); AppendWideText(detailBuffer, pathPointer); })
        APIMON_SIMPLE_BOOL_HOOK(HookedSetDllDirectoryA, g_setDllDirectoryAOriginal, ks::winapi_monitor::EventCategory::Loader, L"Kernel32", L"SetDllDirectoryA",
            (LPCSTR pathPointer), (pathPointer),
            { AppendWideText(detailBuffer, L"path="); AppendAnsiText(detailBuffer, pathPointer); })
        APIMON_SIMPLE_BOOL_HOOK(HookedSetDefaultDllDirectories, g_setDefaultDllDirectoriesOriginal, ks::winapi_monitor::EventCategory::Loader, L"Kernel32", L"SetDefaultDllDirectories",
            (DWORD directoryFlags), (directoryFlags),
            { AppendWideText(detailBuffer, L"flags="); AppendHexText(detailBuffer, directoryFlags); })
        APIMON_SIMPLE_HANDLE_HOOK(DLL_DIRECTORY_COOKIE, HookedAddDllDirectory, g_addDllDirectoryOriginal, ks::winapi_monitor::EventCategory::Loader, L"Kernel32", L"AddDllDirectory",
            (PCWSTR pathPointer), (pathPointer),
            { AppendWideText(detailBuffer, L"path="); AppendWideText(detailBuffer, pathPointer); AppendWideText(detailBuffer, L" cookie="); AppendHexText(detailBuffer, reinterpret_cast<std::uint64_t>(resultHandle)); })
        APIMON_SIMPLE_BOOL_HOOK(HookedRemoveDllDirectory, g_removeDllDirectoryOriginal, ks::winapi_monitor::EventCategory::Loader, L"Kernel32", L"RemoveDllDirectory",
            (DLL_DIRECTORY_COOKIE cookieValue), (cookieValue),
            { AppendWideText(detailBuffer, L"cookie="); AppendHexText(detailBuffer, reinterpret_cast<std::uint64_t>(cookieValue)); })

        APIMON_SIMPLE_BOOL_HOOK(HookedImpersonateLoggedOnUser, g_impersonateLoggedOnUserOriginal, ks::winapi_monitor::EventCategory::Process, L"Advapi32", L"ImpersonateLoggedOnUser",
            (HANDLE tokenHandle), (tokenHandle),
            { BuildSimpleHandleDetail(detailBuffer, L"token", tokenHandle); })
        APIMON_SIMPLE_BOOL_HOOK(HookedRevertToSelf, g_revertToSelfOriginal, ks::winapi_monitor::EventCategory::Process, L"Advapi32", L"RevertToSelf",
            (), (),
            { detailBuffer[0] = L'\0'; })
        APIMON_SIMPLE_BOOL_HOOK(HookedSetThreadToken, g_setThreadTokenOriginal, ks::winapi_monitor::EventCategory::Process, L"Advapi32", L"SetThreadToken",
            (PHANDLE threadHandlePointer, HANDLE tokenHandle), (threadHandlePointer, tokenHandle),
            { AppendWideText(detailBuffer, L"thread="); AppendHexText(detailBuffer, threadHandlePointer != nullptr ? reinterpret_cast<std::uint64_t>(*threadHandlePointer) : 0); AppendWideText(detailBuffer, L" token="); AppendHexText(detailBuffer, reinterpret_cast<std::uint64_t>(tokenHandle)); })

        APIMON_SIMPLE_HANDLE_HOOK(HHOOK, HookedSetWindowsHookExW, g_setWindowsHookExWOriginal, ks::winapi_monitor::EventCategory::Process, L"User32", L"SetWindowsHookExW",
            (int hookId, HOOKPROC hookProcPointer, HINSTANCE moduleHandle, DWORD threadId), (hookId, hookProcPointer, moduleHandle, threadId),
            { AppendWideText(detailBuffer, L"id="); AppendUnsignedText(detailBuffer, static_cast<unsigned long long>(hookId < 0 ? 0 : hookId)); AppendWideText(detailBuffer, L" proc="); AppendHexText(detailBuffer, reinterpret_cast<std::uint64_t>(hookProcPointer)); AppendWideText(detailBuffer, L" module="); AppendHexText(detailBuffer, reinterpret_cast<std::uint64_t>(moduleHandle)); AppendWideText(detailBuffer, L" tid="); AppendUnsignedText(detailBuffer, threadId); AppendWideText(detailBuffer, L" hook="); AppendHexText(detailBuffer, reinterpret_cast<std::uint64_t>(resultHandle)); })
        APIMON_SIMPLE_HANDLE_HOOK(HHOOK, HookedSetWindowsHookExA, g_setWindowsHookExAOriginal, ks::winapi_monitor::EventCategory::Process, L"User32", L"SetWindowsHookExA",
            (int hookId, HOOKPROC hookProcPointer, HINSTANCE moduleHandle, DWORD threadId), (hookId, hookProcPointer, moduleHandle, threadId),
            { AppendWideText(detailBuffer, L"id="); AppendUnsignedText(detailBuffer, static_cast<unsigned long long>(hookId < 0 ? 0 : hookId)); AppendWideText(detailBuffer, L" proc="); AppendHexText(detailBuffer, reinterpret_cast<std::uint64_t>(hookProcPointer)); AppendWideText(detailBuffer, L" module="); AppendHexText(detailBuffer, reinterpret_cast<std::uint64_t>(moduleHandle)); AppendWideText(detailBuffer, L" tid="); AppendUnsignedText(detailBuffer, threadId); AppendWideText(detailBuffer, L" hook="); AppendHexText(detailBuffer, reinterpret_cast<std::uint64_t>(resultHandle)); })
        APIMON_SIMPLE_BOOL_HOOK(HookedUnhookWindowsHookEx, g_unhookWindowsHookExOriginal, ks::winapi_monitor::EventCategory::Process, L"User32", L"UnhookWindowsHookEx",
            (HHOOK hookHandle), (hookHandle),
            { BuildSimpleHandleDetail(detailBuffer, L"hook", hookHandle); })

        // HookedFourthBatchDiscovery 作用：
        // - 输入：PSAPI 进程/模块枚举、User32 窗口发现、GDI 屏幕采集和剪贴板参数；
        // - 处理：记录侦察、桌面交互和截屏/剪贴板访问行为，不复制敏感缓冲区内容；
        // - 返回：保持原始 WinAPI 返回值，并恢复 LastError。
        APIMON_SIMPLE_BOOL_HOOK(HookedEnumProcesses, g_enumProcessesOriginal, ks::winapi_monitor::EventCategory::Process, L"Psapi", L"EnumProcesses",
            (DWORD* processIdsPointer, DWORD bytesValue, DWORD* bytesNeededPointer), (processIdsPointer, bytesValue, bytesNeededPointer),
            { AppendWideText(detailBuffer, L"bytes="); AppendUnsignedText(detailBuffer, bytesValue); AppendWideText(detailBuffer, L" needed="); AppendUnsignedText(detailBuffer, bytesNeededPointer != nullptr ? *bytesNeededPointer : 0); })
        APIMON_SIMPLE_BOOL_HOOK(HookedEnumProcessModules, g_enumProcessModulesOriginal, ks::winapi_monitor::EventCategory::Loader, L"Psapi", L"EnumProcessModules",
            (HANDLE processHandle, HMODULE* modulePointer, DWORD bytesValue, LPDWORD bytesNeededPointer), (processHandle, modulePointer, bytesValue, bytesNeededPointer),
            { AppendWideText(detailBuffer, L"process="); AppendHexText(detailBuffer, reinterpret_cast<std::uint64_t>(processHandle)); AppendWideText(detailBuffer, L" bytes="); AppendUnsignedText(detailBuffer, bytesValue); AppendWideText(detailBuffer, L" needed="); AppendUnsignedText(detailBuffer, bytesNeededPointer != nullptr ? *bytesNeededPointer : 0); })
        APIMON_SIMPLE_BOOL_HOOK(HookedEnumProcessModulesEx, g_enumProcessModulesExOriginal, ks::winapi_monitor::EventCategory::Loader, L"Psapi", L"EnumProcessModulesEx",
            (HANDLE processHandle, HMODULE* modulePointer, DWORD bytesValue, LPDWORD bytesNeededPointer, DWORD filterFlag), (processHandle, modulePointer, bytesValue, bytesNeededPointer, filterFlag),
            { AppendWideText(detailBuffer, L"process="); AppendHexText(detailBuffer, reinterpret_cast<std::uint64_t>(processHandle)); AppendWideText(detailBuffer, L" bytes="); AppendUnsignedText(detailBuffer, bytesValue); AppendWideText(detailBuffer, L" needed="); AppendUnsignedText(detailBuffer, bytesNeededPointer != nullptr ? *bytesNeededPointer : 0); AppendWideText(detailBuffer, L" filter="); AppendHexText(detailBuffer, filterFlag); })
        APIMON_SIMPLE_DWORD_NONZERO_HOOK(HookedGetMappedFileNameW, g_getMappedFileNameWOriginal, ks::winapi_monitor::EventCategory::Loader, L"Psapi", L"GetMappedFileNameW",
            (HANDLE processHandle, LPVOID baseAddress, LPWSTR fileNamePointer, DWORD sizeValue), (processHandle, baseAddress, fileNamePointer, sizeValue),
            { AppendWideText(detailBuffer, L"process="); AppendHexText(detailBuffer, reinterpret_cast<std::uint64_t>(processHandle)); AppendWideText(detailBuffer, L" base="); AppendHexText(detailBuffer, reinterpret_cast<std::uint64_t>(baseAddress)); AppendWideText(detailBuffer, L" size="); AppendUnsignedText(detailBuffer, sizeValue); AppendWideText(detailBuffer, L" path="); if (resultValue != 0) { AppendWideText(detailBuffer, fileNamePointer, resultValue); } })
        APIMON_SIMPLE_DWORD_NONZERO_HOOK(HookedGetMappedFileNameA, g_getMappedFileNameAOriginal, ks::winapi_monitor::EventCategory::Loader, L"Psapi", L"GetMappedFileNameA",
            (HANDLE processHandle, LPVOID baseAddress, LPSTR fileNamePointer, DWORD sizeValue), (processHandle, baseAddress, fileNamePointer, sizeValue),
            { AppendWideText(detailBuffer, L"process="); AppendHexText(detailBuffer, reinterpret_cast<std::uint64_t>(processHandle)); AppendWideText(detailBuffer, L" base="); AppendHexText(detailBuffer, reinterpret_cast<std::uint64_t>(baseAddress)); AppendWideText(detailBuffer, L" size="); AppendUnsignedText(detailBuffer, sizeValue); AppendWideText(detailBuffer, L" path="); if (resultValue != 0) { AppendAnsiText(detailBuffer, fileNamePointer, resultValue); } })

        APIMON_SIMPLE_BOOL_HOOK(HookedEnumWindows, g_enumWindowsOriginal, ks::winapi_monitor::EventCategory::Process, L"User32", L"EnumWindows",
            (WNDENUMPROC callbackPointer, LPARAM parameterValue), (callbackPointer, parameterValue),
            { AppendWideText(detailBuffer, L"callback="); AppendHexText(detailBuffer, reinterpret_cast<std::uint64_t>(callbackPointer)); AppendWideText(detailBuffer, L" param="); AppendHexText(detailBuffer, static_cast<std::uint64_t>(parameterValue)); })
        APIMON_SIMPLE_BOOL_HOOK(HookedEnumChildWindows, g_enumChildWindowsOriginal, ks::winapi_monitor::EventCategory::Process, L"User32", L"EnumChildWindows",
            (HWND parentWindow, WNDENUMPROC callbackPointer, LPARAM parameterValue), (parentWindow, callbackPointer, parameterValue),
            { AppendWideText(detailBuffer, L"parent="); AppendHexText(detailBuffer, reinterpret_cast<std::uint64_t>(parentWindow)); AppendWideText(detailBuffer, L" callback="); AppendHexText(detailBuffer, reinterpret_cast<std::uint64_t>(callbackPointer)); AppendWideText(detailBuffer, L" param="); AppendHexText(detailBuffer, static_cast<std::uint64_t>(parameterValue)); })
        APIMON_SIMPLE_HANDLE_HOOK(HWND, HookedFindWindowW, g_findWindowWOriginal, ks::winapi_monitor::EventCategory::Process, L"User32", L"FindWindowW",
            (LPCWSTR classNamePointer, LPCWSTR windowNamePointer), (classNamePointer, windowNamePointer),
            { AppendWideText(detailBuffer, L"class="); AppendWideText(detailBuffer, classNamePointer); AppendWideText(detailBuffer, L" title="); AppendWideText(detailBuffer, windowNamePointer); AppendWideText(detailBuffer, L" hwnd="); AppendHexText(detailBuffer, reinterpret_cast<std::uint64_t>(resultHandle)); })
        APIMON_SIMPLE_HANDLE_HOOK(HWND, HookedFindWindowA, g_findWindowAOriginal, ks::winapi_monitor::EventCategory::Process, L"User32", L"FindWindowA",
            (LPCSTR classNamePointer, LPCSTR windowNamePointer), (classNamePointer, windowNamePointer),
            { AppendWideText(detailBuffer, L"class="); AppendAnsiText(detailBuffer, classNamePointer); AppendWideText(detailBuffer, L" title="); AppendAnsiText(detailBuffer, windowNamePointer); AppendWideText(detailBuffer, L" hwnd="); AppendHexText(detailBuffer, reinterpret_cast<std::uint64_t>(resultHandle)); })
        APIMON_SIMPLE_HANDLE_HOOK(HWND, HookedFindWindowExW, g_findWindowExWOriginal, ks::winapi_monitor::EventCategory::Process, L"User32", L"FindWindowExW",
            (HWND parentWindow, HWND childAfterWindow, LPCWSTR classNamePointer, LPCWSTR windowNamePointer), (parentWindow, childAfterWindow, classNamePointer, windowNamePointer),
            { AppendWideText(detailBuffer, L"parent="); AppendHexText(detailBuffer, reinterpret_cast<std::uint64_t>(parentWindow)); AppendWideText(detailBuffer, L" after="); AppendHexText(detailBuffer, reinterpret_cast<std::uint64_t>(childAfterWindow)); AppendWideText(detailBuffer, L" class="); AppendWideText(detailBuffer, classNamePointer); AppendWideText(detailBuffer, L" title="); AppendWideText(detailBuffer, windowNamePointer); AppendWideText(detailBuffer, L" hwnd="); AppendHexText(detailBuffer, reinterpret_cast<std::uint64_t>(resultHandle)); })
        APIMON_SIMPLE_HANDLE_HOOK(HWND, HookedFindWindowExA, g_findWindowExAOriginal, ks::winapi_monitor::EventCategory::Process, L"User32", L"FindWindowExA",
            (HWND parentWindow, HWND childAfterWindow, LPCSTR classNamePointer, LPCSTR windowNamePointer), (parentWindow, childAfterWindow, classNamePointer, windowNamePointer),
            { AppendWideText(detailBuffer, L"parent="); AppendHexText(detailBuffer, reinterpret_cast<std::uint64_t>(parentWindow)); AppendWideText(detailBuffer, L" after="); AppendHexText(detailBuffer, reinterpret_cast<std::uint64_t>(childAfterWindow)); AppendWideText(detailBuffer, L" class="); AppendAnsiText(detailBuffer, classNamePointer); AppendWideText(detailBuffer, L" title="); AppendAnsiText(detailBuffer, windowNamePointer); AppendWideText(detailBuffer, L" hwnd="); AppendHexText(detailBuffer, reinterpret_cast<std::uint64_t>(resultHandle)); })
        APIMON_SIMPLE_DWORD_NONZERO_HOOK(HookedGetWindowThreadProcessId, g_getWindowThreadProcessIdOriginal, ks::winapi_monitor::EventCategory::Process, L"User32", L"GetWindowThreadProcessId",
            (HWND windowHandle, LPDWORD processIdPointer), (windowHandle, processIdPointer),
            { AppendWideText(detailBuffer, L"hwnd="); AppendHexText(detailBuffer, reinterpret_cast<std::uint64_t>(windowHandle)); AppendWideText(detailBuffer, L" pid="); AppendUnsignedText(detailBuffer, processIdPointer != nullptr ? *processIdPointer : 0); AppendWideText(detailBuffer, L" tid="); AppendUnsignedText(detailBuffer, resultValue); })
        APIMON_SIMPLE_HANDLE_HOOK(HWND, HookedGetForegroundWindow, g_getForegroundWindowOriginal, ks::winapi_monitor::EventCategory::Process, L"User32", L"GetForegroundWindow",
            (), (),
            { AppendWideText(detailBuffer, L"hwnd="); AppendHexText(detailBuffer, reinterpret_cast<std::uint64_t>(resultHandle)); })
        APIMON_SIMPLE_HANDLE_HOOK(HDC, HookedGetDC, g_getDCOriginal, ks::winapi_monitor::EventCategory::Process, L"User32", L"GetDC",
            (HWND windowHandle), (windowHandle),
            { AppendWideText(detailBuffer, L"hwnd="); AppendHexText(detailBuffer, reinterpret_cast<std::uint64_t>(windowHandle)); AppendWideText(detailBuffer, L" dc="); AppendHexText(detailBuffer, reinterpret_cast<std::uint64_t>(resultHandle)); })
        APIMON_SIMPLE_INT_POSITIVE_HOOK(HookedReleaseDC, g_releaseDCOriginal, ks::winapi_monitor::EventCategory::Process, L"User32", L"ReleaseDC",
            (HWND windowHandle, HDC deviceContext), (windowHandle, deviceContext),
            { AppendWideText(detailBuffer, L"hwnd="); AppendHexText(detailBuffer, reinterpret_cast<std::uint64_t>(windowHandle)); AppendWideText(detailBuffer, L" dc="); AppendHexText(detailBuffer, reinterpret_cast<std::uint64_t>(deviceContext)); AppendWideText(detailBuffer, L" result="); AppendUnsignedText(detailBuffer, static_cast<unsigned long long>(resultValue < 0 ? 0 : resultValue)); })

        APIMON_SIMPLE_HANDLE_HOOK(HDC, HookedCreateCompatibleDC, g_createCompatibleDCOriginal, ks::winapi_monitor::EventCategory::Process, L"Gdi32", L"CreateCompatibleDC",
            (HDC deviceContext), (deviceContext),
            { AppendWideText(detailBuffer, L"sourceDc="); AppendHexText(detailBuffer, reinterpret_cast<std::uint64_t>(deviceContext)); AppendWideText(detailBuffer, L" dc="); AppendHexText(detailBuffer, reinterpret_cast<std::uint64_t>(resultHandle)); })
        APIMON_SIMPLE_BOOL_HOOK(HookedDeleteDC, g_deleteDCOriginal, ks::winapi_monitor::EventCategory::Process, L"Gdi32", L"DeleteDC",
            (HDC deviceContext), (deviceContext),
            { AppendWideText(detailBuffer, L"dc="); AppendHexText(detailBuffer, reinterpret_cast<std::uint64_t>(deviceContext)); })
        APIMON_SIMPLE_HANDLE_HOOK(HBITMAP, HookedCreateCompatibleBitmap, g_createCompatibleBitmapOriginal, ks::winapi_monitor::EventCategory::Process, L"Gdi32", L"CreateCompatibleBitmap",
            (HDC deviceContext, int widthValue, int heightValue), (deviceContext, widthValue, heightValue),
            { AppendWideText(detailBuffer, L"dc="); AppendHexText(detailBuffer, reinterpret_cast<std::uint64_t>(deviceContext)); AppendWideText(detailBuffer, L" width="); AppendUnsignedText(detailBuffer, static_cast<unsigned long long>(widthValue < 0 ? 0 : widthValue)); AppendWideText(detailBuffer, L" height="); AppendUnsignedText(detailBuffer, static_cast<unsigned long long>(heightValue < 0 ? 0 : heightValue)); AppendWideText(detailBuffer, L" bitmap="); AppendHexText(detailBuffer, reinterpret_cast<std::uint64_t>(resultHandle)); })
        APIMON_SIMPLE_BOOL_HOOK(HookedBitBlt, g_bitBltOriginal, ks::winapi_monitor::EventCategory::Process, L"Gdi32", L"BitBlt",
            (HDC destinationDc, int xDest, int yDest, int widthValue, int heightValue, HDC sourceDc, int xSource, int ySource, DWORD ropValue),
            (destinationDc, xDest, yDest, widthValue, heightValue, sourceDc, xSource, ySource, ropValue),
            { AppendWideText(detailBuffer, L"dst="); AppendHexText(detailBuffer, reinterpret_cast<std::uint64_t>(destinationDc)); AppendWideText(detailBuffer, L" src="); AppendHexText(detailBuffer, reinterpret_cast<std::uint64_t>(sourceDc)); AppendWideText(detailBuffer, L" size="); AppendUnsignedText(detailBuffer, static_cast<unsigned long long>(widthValue < 0 ? 0 : widthValue)); AppendWideText(detailBuffer, L"x"); AppendUnsignedText(detailBuffer, static_cast<unsigned long long>(heightValue < 0 ? 0 : heightValue)); AppendWideText(detailBuffer, L" rop="); AppendHexText(detailBuffer, ropValue); })
        APIMON_SIMPLE_BOOL_HOOK(HookedStretchBlt, g_stretchBltOriginal, ks::winapi_monitor::EventCategory::Process, L"Gdi32", L"StretchBlt",
            (HDC destinationDc, int xDest, int yDest, int widthDest, int heightDest, HDC sourceDc, int xSource, int ySource, int widthSource, int heightSource, DWORD ropValue),
            (destinationDc, xDest, yDest, widthDest, heightDest, sourceDc, xSource, ySource, widthSource, heightSource, ropValue),
            { AppendWideText(detailBuffer, L"dst="); AppendHexText(detailBuffer, reinterpret_cast<std::uint64_t>(destinationDc)); AppendWideText(detailBuffer, L" src="); AppendHexText(detailBuffer, reinterpret_cast<std::uint64_t>(sourceDc)); AppendWideText(detailBuffer, L" dstSize="); AppendUnsignedText(detailBuffer, static_cast<unsigned long long>(widthDest < 0 ? 0 : widthDest)); AppendWideText(detailBuffer, L"x"); AppendUnsignedText(detailBuffer, static_cast<unsigned long long>(heightDest < 0 ? 0 : heightDest)); AppendWideText(detailBuffer, L" srcSize="); AppendUnsignedText(detailBuffer, static_cast<unsigned long long>(widthSource < 0 ? 0 : widthSource)); AppendWideText(detailBuffer, L"x"); AppendUnsignedText(detailBuffer, static_cast<unsigned long long>(heightSource < 0 ? 0 : heightSource)); AppendWideText(detailBuffer, L" rop="); AppendHexText(detailBuffer, ropValue); })
        APIMON_SIMPLE_BOOL_HOOK(HookedDeleteObject, g_deleteObjectOriginal, ks::winapi_monitor::EventCategory::Process, L"Gdi32", L"DeleteObject",
            (HGDIOBJ objectHandle), (objectHandle),
            { AppendWideText(detailBuffer, L"object="); AppendHexText(detailBuffer, reinterpret_cast<std::uint64_t>(objectHandle)); })

        APIMON_SIMPLE_BOOL_HOOK(HookedOpenClipboard, g_openClipboardOriginal, ks::winapi_monitor::EventCategory::Process, L"User32", L"OpenClipboard",
            (HWND ownerWindow), (ownerWindow),
            { AppendWideText(detailBuffer, L"owner="); AppendHexText(detailBuffer, reinterpret_cast<std::uint64_t>(ownerWindow)); })
        APIMON_SIMPLE_BOOL_HOOK(HookedCloseClipboard, g_closeClipboardOriginal, ks::winapi_monitor::EventCategory::Process, L"User32", L"CloseClipboard",
            (), (),
            { detailBuffer[0] = L'\0'; })
        APIMON_SIMPLE_HANDLE_HOOK(HANDLE, HookedGetClipboardData, g_getClipboardDataOriginal, ks::winapi_monitor::EventCategory::Process, L"User32", L"GetClipboardData",
            (UINT formatValue), (formatValue),
            { AppendWideText(detailBuffer, L"format="); AppendUnsignedText(detailBuffer, formatValue); AppendWideText(detailBuffer, L" handle="); AppendHexText(detailBuffer, reinterpret_cast<std::uint64_t>(resultHandle)); })
        APIMON_SIMPLE_HANDLE_HOOK(HANDLE, HookedSetClipboardData, g_setClipboardDataOriginal, ks::winapi_monitor::EventCategory::Process, L"User32", L"SetClipboardData",
            (UINT formatValue, HANDLE dataHandle), (formatValue, dataHandle),
            { AppendWideText(detailBuffer, L"format="); AppendUnsignedText(detailBuffer, formatValue); AppendWideText(detailBuffer, L" input="); AppendHexText(detailBuffer, reinterpret_cast<std::uint64_t>(dataHandle)); AppendWideText(detailBuffer, L" handle="); AppendHexText(detailBuffer, reinterpret_cast<std::uint64_t>(resultHandle)); })
        APIMON_SIMPLE_BOOL_HOOK(HookedEmptyClipboard, g_emptyClipboardOriginal, ks::winapi_monitor::EventCategory::Process, L"User32", L"EmptyClipboard",
            (), (),
            { detailBuffer[0] = L'\0'; })

        // HookedFourthBatchTelemetry 作用：
        // - 输入：ETW session/provider/trace 句柄与控制参数；
        // - 处理：记录用户态事件跟踪启停、provider 注册和事件写入入口；
        // - 返回：保持 ETW 原始 ULONG 状态码，ERROR_SUCCESS 为成功。
        APIMON_SIMPLE_ULONG_STATUS_HOOK(HookedStartTraceW, g_startTraceWOriginal, ks::winapi_monitor::EventCategory::Process, L"Advapi32", L"StartTraceW",
            (PTRACEHANDLE traceHandlePointer, LPCWSTR instanceNamePointer, PEVENT_TRACE_PROPERTIES propertiesPointer), (traceHandlePointer, instanceNamePointer, propertiesPointer),
            { AppendWideText(detailBuffer, L"name="); AppendWideText(detailBuffer, instanceNamePointer); AppendWideText(detailBuffer, L" trace="); AppendHexText(detailBuffer, traceHandlePointer != nullptr ? *traceHandlePointer : 0); AppendWideText(detailBuffer, L" props="); AppendHexText(detailBuffer, reinterpret_cast<std::uint64_t>(propertiesPointer)); })
        APIMON_SIMPLE_ULONG_STATUS_HOOK(HookedStartTraceA, g_startTraceAOriginal, ks::winapi_monitor::EventCategory::Process, L"Advapi32", L"StartTraceA",
            (PTRACEHANDLE traceHandlePointer, LPCSTR instanceNamePointer, PEVENT_TRACE_PROPERTIES propertiesPointer), (traceHandlePointer, instanceNamePointer, propertiesPointer),
            { AppendWideText(detailBuffer, L"name="); AppendAnsiText(detailBuffer, instanceNamePointer); AppendWideText(detailBuffer, L" trace="); AppendHexText(detailBuffer, traceHandlePointer != nullptr ? *traceHandlePointer : 0); AppendWideText(detailBuffer, L" props="); AppendHexText(detailBuffer, reinterpret_cast<std::uint64_t>(propertiesPointer)); })
        APIMON_SIMPLE_ULONG_STATUS_HOOK(HookedControlTraceW, g_controlTraceWOriginal, ks::winapi_monitor::EventCategory::Process, L"Advapi32", L"ControlTraceW",
            (TRACEHANDLE traceHandle, LPCWSTR instanceNamePointer, PEVENT_TRACE_PROPERTIES propertiesPointer, ULONG controlCode), (traceHandle, instanceNamePointer, propertiesPointer, controlCode),
            { AppendWideText(detailBuffer, L"trace="); AppendHexText(detailBuffer, traceHandle); AppendWideText(detailBuffer, L" name="); AppendWideText(detailBuffer, instanceNamePointer); AppendWideText(detailBuffer, L" control="); AppendHexText(detailBuffer, controlCode); })
        APIMON_SIMPLE_ULONG_STATUS_HOOK(HookedControlTraceA, g_controlTraceAOriginal, ks::winapi_monitor::EventCategory::Process, L"Advapi32", L"ControlTraceA",
            (TRACEHANDLE traceHandle, LPCSTR instanceNamePointer, PEVENT_TRACE_PROPERTIES propertiesPointer, ULONG controlCode), (traceHandle, instanceNamePointer, propertiesPointer, controlCode),
            { AppendWideText(detailBuffer, L"trace="); AppendHexText(detailBuffer, traceHandle); AppendWideText(detailBuffer, L" name="); AppendAnsiText(detailBuffer, instanceNamePointer); AppendWideText(detailBuffer, L" control="); AppendHexText(detailBuffer, controlCode); })
        APIMON_SIMPLE_ULONG_STATUS_HOOK(HookedEnableTraceEx2, g_enableTraceEx2Original, ks::winapi_monitor::EventCategory::Process, L"Advapi32", L"EnableTraceEx2",
            (TRACEHANDLE traceHandle, LPCGUID providerIdPointer, ULONG controlCode, UCHAR levelValue, ULONGLONG matchAnyKeyword, ULONGLONG matchAllKeyword, ULONG timeoutValue, void* enableParametersPointer),
            (traceHandle, providerIdPointer, controlCode, levelValue, matchAnyKeyword, matchAllKeyword, timeoutValue, enableParametersPointer),
            { AppendWideText(detailBuffer, L"trace="); AppendHexText(detailBuffer, traceHandle); AppendWideText(detailBuffer, L" provider="); AppendHexText(detailBuffer, reinterpret_cast<std::uint64_t>(providerIdPointer)); AppendWideText(detailBuffer, L" control="); AppendHexText(detailBuffer, controlCode); AppendWideText(detailBuffer, L" level="); AppendUnsignedText(detailBuffer, levelValue); AppendWideText(detailBuffer, L" any="); AppendHexText(detailBuffer, matchAnyKeyword); AppendWideText(detailBuffer, L" all="); AppendHexText(detailBuffer, matchAllKeyword); })

        TRACEHANDLE WINAPI HookedOpenTraceW(PEVENT_TRACE_LOGFILEW logFilePointer)
        {
            ScopedHookGuard guardValue;
            if (guardValue.bypass()) { return g_openTraceWOriginal(logFilePointer); }
            const TRACEHANDLE traceHandle = g_openTraceWOriginal(logFilePointer);
            const DWORD lastError = ::GetLastError();
            wchar_t detailBuffer[ks::winapi_monitor::kMaxDetailChars] = {};
            AppendWideText(detailBuffer, L"logfile=");
            AppendWideText(detailBuffer, logFilePointer != nullptr ? logFilePointer->LogFileName : nullptr);
            AppendWideText(detailBuffer, L" logger=");
            AppendWideText(detailBuffer, logFilePointer != nullptr ? logFilePointer->LoggerName : nullptr);
            AppendWideText(detailBuffer, L" trace=");
            AppendHexText(detailBuffer, traceHandle);
            SendRawEventWithStatus(ks::winapi_monitor::EventCategory::Process, L"Advapi32", L"OpenTraceW", traceHandle != INVALID_PROCESSTRACE_HANDLE ? 0 : lastError, detailBuffer);
            ::SetLastError(lastError);
            return traceHandle;
        }

        TRACEHANDLE WINAPI HookedOpenTraceA(PEVENT_TRACE_LOGFILEA logFilePointer)
        {
            ScopedHookGuard guardValue;
            if (guardValue.bypass()) { return g_openTraceAOriginal(logFilePointer); }
            const TRACEHANDLE traceHandle = g_openTraceAOriginal(logFilePointer);
            const DWORD lastError = ::GetLastError();
            wchar_t detailBuffer[ks::winapi_monitor::kMaxDetailChars] = {};
            AppendWideText(detailBuffer, L"logfile=");
            AppendAnsiText(detailBuffer, logFilePointer != nullptr ? logFilePointer->LogFileName : nullptr);
            AppendWideText(detailBuffer, L" logger=");
            AppendAnsiText(detailBuffer, logFilePointer != nullptr ? logFilePointer->LoggerName : nullptr);
            AppendWideText(detailBuffer, L" trace=");
            AppendHexText(detailBuffer, traceHandle);
            SendRawEventWithStatus(ks::winapi_monitor::EventCategory::Process, L"Advapi32", L"OpenTraceA", traceHandle != INVALID_PROCESSTRACE_HANDLE ? 0 : lastError, detailBuffer);
            ::SetLastError(lastError);
            return traceHandle;
        }

        APIMON_SIMPLE_ULONG_STATUS_HOOK(HookedProcessTrace, g_processTraceOriginal, ks::winapi_monitor::EventCategory::Process, L"Advapi32", L"ProcessTrace",
            (PTRACEHANDLE traceHandleArray, ULONG handleCount, LPFILETIME startTimePointer, LPFILETIME endTimePointer), (traceHandleArray, handleCount, startTimePointer, endTimePointer),
            { AppendWideText(detailBuffer, L"count="); AppendUnsignedText(detailBuffer, handleCount); AppendWideText(detailBuffer, L" first="); AppendHexText(detailBuffer, traceHandleArray != nullptr && handleCount != 0 ? traceHandleArray[0] : 0); })
        APIMON_SIMPLE_ULONG_STATUS_HOOK(HookedCloseTrace, g_closeTraceOriginal, ks::winapi_monitor::EventCategory::Process, L"Advapi32", L"CloseTrace",
            (TRACEHANDLE traceHandle), (traceHandle),
            { AppendWideText(detailBuffer, L"trace="); AppendHexText(detailBuffer, traceHandle); })
        APIMON_SIMPLE_ULONG_STATUS_HOOK(HookedEventRegister, g_eventRegisterOriginal, ks::winapi_monitor::EventCategory::Process, L"Advapi32", L"EventRegister",
            (LPCGUID providerIdPointer, PENABLECALLBACK callbackPointer, PVOID callbackContextPointer, PREGHANDLE registrationHandlePointer), (providerIdPointer, callbackPointer, callbackContextPointer, registrationHandlePointer),
            { AppendWideText(detailBuffer, L"provider="); AppendHexText(detailBuffer, reinterpret_cast<std::uint64_t>(providerIdPointer)); AppendWideText(detailBuffer, L" callback="); AppendHexText(detailBuffer, reinterpret_cast<std::uint64_t>(callbackPointer)); AppendWideText(detailBuffer, L" reg="); AppendHexText(detailBuffer, registrationHandlePointer != nullptr ? *registrationHandlePointer : 0); })
        APIMON_SIMPLE_ULONG_STATUS_HOOK(HookedEventUnregister, g_eventUnregisterOriginal, ks::winapi_monitor::EventCategory::Process, L"Advapi32", L"EventUnregister",
            (REGHANDLE registrationHandle), (registrationHandle),
            { AppendWideText(detailBuffer, L"reg="); AppendHexText(detailBuffer, registrationHandle); })
        APIMON_SIMPLE_ULONG_STATUS_HOOK(HookedEventWrite, g_eventWriteOriginal, ks::winapi_monitor::EventCategory::Process, L"Advapi32", L"EventWrite",
            (REGHANDLE registrationHandle, PCEVENT_DESCRIPTOR eventDescriptorPointer, ULONG userDataCount, PEVENT_DATA_DESCRIPTOR userDataPointer), (registrationHandle, eventDescriptorPointer, userDataCount, userDataPointer),
            { AppendWideText(detailBuffer, L"reg="); AppendHexText(detailBuffer, registrationHandle); AppendWideText(detailBuffer, L" event="); AppendUnsignedText(detailBuffer, eventDescriptorPointer != nullptr ? eventDescriptorPointer->Id : 0); AppendWideText(detailBuffer, L" fields="); AppendUnsignedText(detailBuffer, userDataCount); })
        APIMON_SIMPLE_ULONG_STATUS_HOOK(HookedEventWriteEx, g_eventWriteExOriginal, ks::winapi_monitor::EventCategory::Process, L"Advapi32", L"EventWriteEx",
            (REGHANDLE registrationHandle, PCEVENT_DESCRIPTOR eventDescriptorPointer, ULONG64 filterValue, ULONG flagsValue, LPCGUID activityIdPointer, LPCGUID relatedActivityIdPointer, ULONG userDataCount, PEVENT_DATA_DESCRIPTOR userDataPointer),
            (registrationHandle, eventDescriptorPointer, filterValue, flagsValue, activityIdPointer, relatedActivityIdPointer, userDataCount, userDataPointer),
            { AppendWideText(detailBuffer, L"reg="); AppendHexText(detailBuffer, registrationHandle); AppendWideText(detailBuffer, L" event="); AppendUnsignedText(detailBuffer, eventDescriptorPointer != nullptr ? eventDescriptorPointer->Id : 0); AppendWideText(detailBuffer, L" flags="); AppendHexText(detailBuffer, flagsValue); AppendWideText(detailBuffer, L" fields="); AppendUnsignedText(detailBuffer, userDataCount); })

        // HookedFourthBatchTrustCrypto 作用：记录签名/证书/DPAPI 和 CNG Key Storage 入口；不记录密钥或明文内容。
        APIMON_SIMPLE_LONG_STATUS_HOOK(HookedWinVerifyTrust, g_winVerifyTrustOriginal, ks::winapi_monitor::EventCategory::Process, L"Wintrust", L"WinVerifyTrust",
            (HWND windowHandle, GUID* actionIdPointer, LPVOID dataPointer), (windowHandle, actionIdPointer, dataPointer),
            { AppendWideText(detailBuffer, L"hwnd="); AppendHexText(detailBuffer, reinterpret_cast<std::uint64_t>(windowHandle)); AppendWideText(detailBuffer, L" action="); AppendHexText(detailBuffer, reinterpret_cast<std::uint64_t>(actionIdPointer)); AppendWideText(detailBuffer, L" data="); AppendHexText(detailBuffer, reinterpret_cast<std::uint64_t>(dataPointer)); })
        APIMON_SIMPLE_BOOL_HOOK(HookedCryptQueryObject, g_cryptQueryObjectOriginal, ks::winapi_monitor::EventCategory::Process, L"Crypt32", L"CryptQueryObject",
            (DWORD objectType, const void* objectPointer, DWORD expectedContentTypeFlags, DWORD expectedFormatTypeFlags, DWORD flagsValue, DWORD* encodingTypePointer, DWORD* contentTypePointer, DWORD* formatTypePointer, HCERTSTORE* certStorePointer, HCRYPTMSG* cryptMsgPointer, const void** contextPointer),
            (objectType, objectPointer, expectedContentTypeFlags, expectedFormatTypeFlags, flagsValue, encodingTypePointer, contentTypePointer, formatTypePointer, certStorePointer, cryptMsgPointer, contextPointer),
            { AppendWideText(detailBuffer, L"type="); AppendHexText(detailBuffer, objectType); AppendWideText(detailBuffer, L" object="); AppendHexText(detailBuffer, reinterpret_cast<std::uint64_t>(objectPointer)); AppendWideText(detailBuffer, L" content="); AppendHexText(detailBuffer, contentTypePointer != nullptr ? *contentTypePointer : 0); AppendWideText(detailBuffer, L" format="); AppendHexText(detailBuffer, formatTypePointer != nullptr ? *formatTypePointer : 0); })
        APIMON_SIMPLE_HANDLE_HOOK(HCERTSTORE, HookedCertOpenStore, g_certOpenStoreOriginal, ks::winapi_monitor::EventCategory::Process, L"Crypt32", L"CertOpenStore",
            (LPCSTR storeProviderPointer, DWORD encodingType, HCRYPTPROV_LEGACY cryptProvider, DWORD flagsValue, const void* parameterPointer),
            (storeProviderPointer, encodingType, cryptProvider, flagsValue, parameterPointer),
            { AppendWideText(detailBuffer, L"provider="); AppendAnsiText(detailBuffer, storeProviderPointer); AppendWideText(detailBuffer, L" encoding="); AppendHexText(detailBuffer, encodingType); AppendWideText(detailBuffer, L" flags="); AppendHexText(detailBuffer, flagsValue); AppendWideText(detailBuffer, L" store="); AppendHexText(detailBuffer, reinterpret_cast<std::uint64_t>(resultHandle)); })
        APIMON_SIMPLE_BOOL_HOOK(HookedCertCloseStore, g_certCloseStoreOriginal, ks::winapi_monitor::EventCategory::Process, L"Crypt32", L"CertCloseStore",
            (HCERTSTORE certStoreHandle, DWORD flagsValue), (certStoreHandle, flagsValue),
            { AppendWideText(detailBuffer, L"store="); AppendHexText(detailBuffer, reinterpret_cast<std::uint64_t>(certStoreHandle)); AppendWideText(detailBuffer, L" flags="); AppendHexText(detailBuffer, flagsValue); })
        APIMON_SIMPLE_HANDLE_HOOK(PCCERT_CONTEXT, HookedCertFindCertificateInStore, g_certFindCertificateInStoreOriginal, ks::winapi_monitor::EventCategory::Process, L"Crypt32", L"CertFindCertificateInStore",
            (HCERTSTORE certStoreHandle, DWORD encodingType, DWORD findFlags, DWORD findType, const void* findParaPointer, PCCERT_CONTEXT previousContextPointer),
            (certStoreHandle, encodingType, findFlags, findType, findParaPointer, previousContextPointer),
            { AppendWideText(detailBuffer, L"store="); AppendHexText(detailBuffer, reinterpret_cast<std::uint64_t>(certStoreHandle)); AppendWideText(detailBuffer, L" findType="); AppendHexText(detailBuffer, findType); AppendWideText(detailBuffer, L" flags="); AppendHexText(detailBuffer, findFlags); AppendWideText(detailBuffer, L" cert="); AppendHexText(detailBuffer, reinterpret_cast<std::uint64_t>(resultHandle)); })
        APIMON_SIMPLE_BOOL_HOOK(HookedCertGetCertificateChain, g_certGetCertificateChainOriginal, ks::winapi_monitor::EventCategory::Process, L"Crypt32", L"CertGetCertificateChain",
            (HCERTCHAINENGINE chainEngineHandle, PCCERT_CONTEXT certContextPointer, LPFILETIME timePointer, HCERTSTORE additionalStoreHandle, PCERT_CHAIN_PARA chainParaPointer, DWORD flagsValue, LPVOID reservedPointer, PCCERT_CHAIN_CONTEXT* chainContextPointer),
            (chainEngineHandle, certContextPointer, timePointer, additionalStoreHandle, chainParaPointer, flagsValue, reservedPointer, chainContextPointer),
            { AppendWideText(detailBuffer, L"engine="); AppendHexText(detailBuffer, reinterpret_cast<std::uint64_t>(chainEngineHandle)); AppendWideText(detailBuffer, L" cert="); AppendHexText(detailBuffer, reinterpret_cast<std::uint64_t>(certContextPointer)); AppendWideText(detailBuffer, L" flags="); AppendHexText(detailBuffer, flagsValue); AppendWideText(detailBuffer, L" chain="); AppendHexText(detailBuffer, chainContextPointer != nullptr ? reinterpret_cast<std::uint64_t>(*chainContextPointer) : 0); })
        APIMON_SIMPLE_BOOL_HOOK(HookedCertVerifyCertificateChainPolicy, g_certVerifyCertificateChainPolicyOriginal, ks::winapi_monitor::EventCategory::Process, L"Crypt32", L"CertVerifyCertificateChainPolicy",
            (LPCSTR policyPointer, PCCERT_CHAIN_CONTEXT chainContextPointer, PCERT_CHAIN_POLICY_PARA policyParaPointer, PCERT_CHAIN_POLICY_STATUS policyStatusPointer),
            (policyPointer, chainContextPointer, policyParaPointer, policyStatusPointer),
            { AppendWideText(detailBuffer, L"policy="); AppendAnsiText(detailBuffer, policyPointer); AppendWideText(detailBuffer, L" chain="); AppendHexText(detailBuffer, reinterpret_cast<std::uint64_t>(chainContextPointer)); AppendWideText(detailBuffer, L" error="); AppendHexText(detailBuffer, policyStatusPointer != nullptr ? policyStatusPointer->dwError : 0); })
        APIMON_SIMPLE_BOOL_HOOK(HookedCryptProtectData, g_cryptProtectDataOriginal, ks::winapi_monitor::EventCategory::Process, L"Crypt32", L"CryptProtectData",
            (DATA_BLOB* dataInPointer, LPCWSTR descriptionPointer, DATA_BLOB* optionalEntropyPointer, PVOID reservedPointer, CRYPTPROTECT_PROMPTSTRUCT* promptPointer, DWORD flagsValue, DATA_BLOB* dataOutPointer),
            (dataInPointer, descriptionPointer, optionalEntropyPointer, reservedPointer, promptPointer, flagsValue, dataOutPointer),
            { AppendWideText(detailBuffer, L"inBytes="); AppendUnsignedText(detailBuffer, dataInPointer != nullptr ? dataInPointer->cbData : 0); AppendWideText(detailBuffer, L" entropyBytes="); AppendUnsignedText(detailBuffer, optionalEntropyPointer != nullptr ? optionalEntropyPointer->cbData : 0); AppendWideText(detailBuffer, L" outBytes="); AppendUnsignedText(detailBuffer, dataOutPointer != nullptr ? dataOutPointer->cbData : 0); AppendWideText(detailBuffer, L" flags="); AppendHexText(detailBuffer, flagsValue); AppendWideText(detailBuffer, L" desc="); AppendWideText(detailBuffer, descriptionPointer); })
        APIMON_SIMPLE_BOOL_HOOK(HookedCryptUnprotectData, g_cryptUnprotectDataOriginal, ks::winapi_monitor::EventCategory::Process, L"Crypt32", L"CryptUnprotectData",
            (DATA_BLOB* dataInPointer, LPWSTR* descriptionPointer, DATA_BLOB* optionalEntropyPointer, PVOID reservedPointer, CRYPTPROTECT_PROMPTSTRUCT* promptPointer, DWORD flagsValue, DATA_BLOB* dataOutPointer),
            (dataInPointer, descriptionPointer, optionalEntropyPointer, reservedPointer, promptPointer, flagsValue, dataOutPointer),
            { AppendWideText(detailBuffer, L"inBytes="); AppendUnsignedText(detailBuffer, dataInPointer != nullptr ? dataInPointer->cbData : 0); AppendWideText(detailBuffer, L" entropyBytes="); AppendUnsignedText(detailBuffer, optionalEntropyPointer != nullptr ? optionalEntropyPointer->cbData : 0); AppendWideText(detailBuffer, L" outBytes="); AppendUnsignedText(detailBuffer, dataOutPointer != nullptr ? dataOutPointer->cbData : 0); AppendWideText(detailBuffer, L" flags="); AppendHexText(detailBuffer, flagsValue); AppendWideText(detailBuffer, L" desc="); AppendWideText(detailBuffer, descriptionPointer != nullptr ? *descriptionPointer : nullptr); })

        APIMON_SIMPLE_SECURITY_STATUS_HOOK(HookedNCryptOpenStorageProvider, g_nCryptOpenStorageProviderOriginal, ks::winapi_monitor::EventCategory::Process, L"Ncrypt", L"NCryptOpenStorageProvider",
            (NCRYPT_PROV_HANDLE* providerHandlePointer, LPCWSTR providerNamePointer, DWORD flagsValue), (providerHandlePointer, providerNamePointer, flagsValue),
            { AppendWideText(detailBuffer, L"name="); AppendWideText(detailBuffer, providerNamePointer); AppendWideText(detailBuffer, L" flags="); AppendHexText(detailBuffer, flagsValue); AppendWideText(detailBuffer, L" provider="); AppendHexText(detailBuffer, providerHandlePointer != nullptr ? static_cast<std::uint64_t>(*providerHandlePointer) : 0); })
        APIMON_SIMPLE_SECURITY_STATUS_HOOK(HookedNCryptOpenKey, g_nCryptOpenKeyOriginal, ks::winapi_monitor::EventCategory::Process, L"Ncrypt", L"NCryptOpenKey",
            (NCRYPT_PROV_HANDLE providerHandle, NCRYPT_KEY_HANDLE* keyHandlePointer, LPCWSTR keyNamePointer, DWORD legacyKeySpec, DWORD flagsValue), (providerHandle, keyHandlePointer, keyNamePointer, legacyKeySpec, flagsValue),
            { AppendWideText(detailBuffer, L"provider="); AppendHexText(detailBuffer, static_cast<std::uint64_t>(providerHandle)); AppendWideText(detailBuffer, L" name="); AppendWideText(detailBuffer, keyNamePointer); AppendWideText(detailBuffer, L" spec="); AppendHexText(detailBuffer, legacyKeySpec); AppendWideText(detailBuffer, L" flags="); AppendHexText(detailBuffer, flagsValue); AppendWideText(detailBuffer, L" key="); AppendHexText(detailBuffer, keyHandlePointer != nullptr ? static_cast<std::uint64_t>(*keyHandlePointer) : 0); })
        APIMON_SIMPLE_SECURITY_STATUS_HOOK(HookedNCryptCreatePersistedKey, g_nCryptCreatePersistedKeyOriginal, ks::winapi_monitor::EventCategory::Process, L"Ncrypt", L"NCryptCreatePersistedKey",
            (NCRYPT_PROV_HANDLE providerHandle, NCRYPT_KEY_HANDLE* keyHandlePointer, LPCWSTR algorithmIdPointer, LPCWSTR keyNamePointer, DWORD legacyKeySpec, DWORD flagsValue), (providerHandle, keyHandlePointer, algorithmIdPointer, keyNamePointer, legacyKeySpec, flagsValue),
            { AppendWideText(detailBuffer, L"provider="); AppendHexText(detailBuffer, static_cast<std::uint64_t>(providerHandle)); AppendWideText(detailBuffer, L" alg="); AppendWideText(detailBuffer, algorithmIdPointer); AppendWideText(detailBuffer, L" name="); AppendWideText(detailBuffer, keyNamePointer); AppendWideText(detailBuffer, L" spec="); AppendHexText(detailBuffer, legacyKeySpec); AppendWideText(detailBuffer, L" flags="); AppendHexText(detailBuffer, flagsValue); AppendWideText(detailBuffer, L" key="); AppendHexText(detailBuffer, keyHandlePointer != nullptr ? static_cast<std::uint64_t>(*keyHandlePointer) : 0); })
        APIMON_SIMPLE_SECURITY_STATUS_HOOK(HookedNCryptFinalizeKey, g_nCryptFinalizeKeyOriginal, ks::winapi_monitor::EventCategory::Process, L"Ncrypt", L"NCryptFinalizeKey",
            (NCRYPT_KEY_HANDLE keyHandle, DWORD flagsValue), (keyHandle, flagsValue),
            { AppendWideText(detailBuffer, L"key="); AppendHexText(detailBuffer, static_cast<std::uint64_t>(keyHandle)); AppendWideText(detailBuffer, L" flags="); AppendHexText(detailBuffer, flagsValue); })
        APIMON_SIMPLE_SECURITY_STATUS_HOOK(HookedNCryptEncrypt, g_nCryptEncryptOriginal, ks::winapi_monitor::EventCategory::Process, L"Ncrypt", L"NCryptEncrypt",
            (NCRYPT_KEY_HANDLE keyHandle, PBYTE inputPointer, DWORD inputLength, VOID* paddingInfoPointer, PBYTE outputPointer, DWORD outputLength, DWORD* resultLengthPointer, DWORD flagsValue),
            (keyHandle, inputPointer, inputLength, paddingInfoPointer, outputPointer, outputLength, resultLengthPointer, flagsValue),
            { AppendWideText(detailBuffer, L"key="); AppendHexText(detailBuffer, static_cast<std::uint64_t>(keyHandle)); AppendWideText(detailBuffer, L" in="); AppendUnsignedText(detailBuffer, inputLength); AppendWideText(detailBuffer, L" out="); AppendUnsignedText(detailBuffer, outputLength); AppendWideText(detailBuffer, L" result="); AppendUnsignedText(detailBuffer, resultLengthPointer != nullptr ? *resultLengthPointer : 0); AppendWideText(detailBuffer, L" flags="); AppendHexText(detailBuffer, flagsValue); })
        APIMON_SIMPLE_SECURITY_STATUS_HOOK(HookedNCryptDecrypt, g_nCryptDecryptOriginal, ks::winapi_monitor::EventCategory::Process, L"Ncrypt", L"NCryptDecrypt",
            (NCRYPT_KEY_HANDLE keyHandle, PBYTE inputPointer, DWORD inputLength, VOID* paddingInfoPointer, PBYTE outputPointer, DWORD outputLength, DWORD* resultLengthPointer, DWORD flagsValue),
            (keyHandle, inputPointer, inputLength, paddingInfoPointer, outputPointer, outputLength, resultLengthPointer, flagsValue),
            { AppendWideText(detailBuffer, L"key="); AppendHexText(detailBuffer, static_cast<std::uint64_t>(keyHandle)); AppendWideText(detailBuffer, L" in="); AppendUnsignedText(detailBuffer, inputLength); AppendWideText(detailBuffer, L" out="); AppendUnsignedText(detailBuffer, outputLength); AppendWideText(detailBuffer, L" result="); AppendUnsignedText(detailBuffer, resultLengthPointer != nullptr ? *resultLengthPointer : 0); AppendWideText(detailBuffer, L" flags="); AppendHexText(detailBuffer, flagsValue); })
        APIMON_SIMPLE_SECURITY_STATUS_HOOK(HookedNCryptSignHash, g_nCryptSignHashOriginal, ks::winapi_monitor::EventCategory::Process, L"Ncrypt", L"NCryptSignHash",
            (NCRYPT_KEY_HANDLE keyHandle, VOID* paddingInfoPointer, PBYTE hashValuePointer, DWORD hashValueLength, PBYTE signaturePointer, DWORD signatureLength, DWORD* resultLengthPointer, DWORD flagsValue),
            (keyHandle, paddingInfoPointer, hashValuePointer, hashValueLength, signaturePointer, signatureLength, resultLengthPointer, flagsValue),
            { AppendWideText(detailBuffer, L"key="); AppendHexText(detailBuffer, static_cast<std::uint64_t>(keyHandle)); AppendWideText(detailBuffer, L" hash="); AppendUnsignedText(detailBuffer, hashValueLength); AppendWideText(detailBuffer, L" sig="); AppendUnsignedText(detailBuffer, signatureLength); AppendWideText(detailBuffer, L" result="); AppendUnsignedText(detailBuffer, resultLengthPointer != nullptr ? *resultLengthPointer : 0); AppendWideText(detailBuffer, L" flags="); AppendHexText(detailBuffer, flagsValue); })
        APIMON_SIMPLE_SECURITY_STATUS_HOOK(HookedNCryptVerifySignature, g_nCryptVerifySignatureOriginal, ks::winapi_monitor::EventCategory::Process, L"Ncrypt", L"NCryptVerifySignature",
            (NCRYPT_KEY_HANDLE keyHandle, VOID* paddingInfoPointer, PBYTE hashValuePointer, DWORD hashValueLength, PBYTE signaturePointer, DWORD signatureLength, DWORD flagsValue),
            (keyHandle, paddingInfoPointer, hashValuePointer, hashValueLength, signaturePointer, signatureLength, flagsValue),
            { AppendWideText(detailBuffer, L"key="); AppendHexText(detailBuffer, static_cast<std::uint64_t>(keyHandle)); AppendWideText(detailBuffer, L" hash="); AppendUnsignedText(detailBuffer, hashValueLength); AppendWideText(detailBuffer, L" sig="); AppendUnsignedText(detailBuffer, signatureLength); AppendWideText(detailBuffer, L" flags="); AppendHexText(detailBuffer, flagsValue); })
        APIMON_SIMPLE_SECURITY_STATUS_HOOK(HookedNCryptExportKey, g_nCryptExportKeyOriginal, ks::winapi_monitor::EventCategory::Process, L"Ncrypt", L"NCryptExportKey",
            (NCRYPT_KEY_HANDLE keyHandle, NCRYPT_KEY_HANDLE exportKeyHandle, LPCWSTR blobTypePointer, VOID* parameterListPointer, PBYTE outputPointer, DWORD outputLength, DWORD* resultLengthPointer, DWORD flagsValue),
            (keyHandle, exportKeyHandle, blobTypePointer, parameterListPointer, outputPointer, outputLength, resultLengthPointer, flagsValue),
            { AppendWideText(detailBuffer, L"key="); AppendHexText(detailBuffer, static_cast<std::uint64_t>(keyHandle)); AppendWideText(detailBuffer, L" blob="); AppendWideText(detailBuffer, blobTypePointer); AppendWideText(detailBuffer, L" out="); AppendUnsignedText(detailBuffer, outputLength); AppendWideText(detailBuffer, L" result="); AppendUnsignedText(detailBuffer, resultLengthPointer != nullptr ? *resultLengthPointer : 0); AppendWideText(detailBuffer, L" flags="); AppendHexText(detailBuffer, flagsValue); })
        APIMON_SIMPLE_SECURITY_STATUS_HOOK(HookedNCryptImportKey, g_nCryptImportKeyOriginal, ks::winapi_monitor::EventCategory::Process, L"Ncrypt", L"NCryptImportKey",
            (NCRYPT_PROV_HANDLE providerHandle, NCRYPT_KEY_HANDLE importKeyHandle, LPCWSTR blobTypePointer, VOID* parameterListPointer, NCRYPT_KEY_HANDLE* keyHandlePointer, PBYTE inputPointer, DWORD inputLength, DWORD flagsValue),
            (providerHandle, importKeyHandle, blobTypePointer, parameterListPointer, keyHandlePointer, inputPointer, inputLength, flagsValue),
            { AppendWideText(detailBuffer, L"provider="); AppendHexText(detailBuffer, static_cast<std::uint64_t>(providerHandle)); AppendWideText(detailBuffer, L" blob="); AppendWideText(detailBuffer, blobTypePointer); AppendWideText(detailBuffer, L" in="); AppendUnsignedText(detailBuffer, inputLength); AppendWideText(detailBuffer, L" key="); AppendHexText(detailBuffer, keyHandlePointer != nullptr ? static_cast<std::uint64_t>(*keyHandlePointer) : 0); AppendWideText(detailBuffer, L" flags="); AppendHexText(detailBuffer, flagsValue); })
        APIMON_SIMPLE_SECURITY_STATUS_HOOK(HookedNCryptDeleteKey, g_nCryptDeleteKeyOriginal, ks::winapi_monitor::EventCategory::Process, L"Ncrypt", L"NCryptDeleteKey",
            (NCRYPT_KEY_HANDLE keyHandle, DWORD flagsValue), (keyHandle, flagsValue),
            { AppendWideText(detailBuffer, L"key="); AppendHexText(detailBuffer, static_cast<std::uint64_t>(keyHandle)); AppendWideText(detailBuffer, L" flags="); AppendHexText(detailBuffer, flagsValue); })
        APIMON_SIMPLE_SECURITY_STATUS_HOOK(HookedNCryptFreeObject, g_nCryptFreeObjectOriginal, ks::winapi_monitor::EventCategory::Process, L"Ncrypt", L"NCryptFreeObject",
            (NCRYPT_HANDLE objectHandle), (objectHandle),
            { AppendWideText(detailBuffer, L"object="); AppendHexText(detailBuffer, static_cast<std::uint64_t>(objectHandle)); })

        // HookedFourthBatchRpcNative 作用：
        // - 输入：RPC endpoint/binding 枚举参数和 ntdll token/object/sync/query 参数；
        // - 处理：补齐 RPC 发现面与 native 层对象/令牌/同步调用面；
        // - 返回：保持 RPC_STATUS 或 NTSTATUS 原始语义。
        APIMON_SIMPLE_RPC_STATUS_HOOK(HookedRpcStringBindingComposeW, g_rpcStringBindingComposeWOriginal, ks::winapi_monitor::EventCategory::Network, L"Rpcrt4", L"RpcStringBindingComposeW",
            (RPC_WSTR objUuidPointer, RPC_WSTR protocolSequencePointer, RPC_WSTR networkAddressPointer, RPC_WSTR endpointPointer, RPC_WSTR optionsPointer, RPC_WSTR* stringBindingPointer),
            (objUuidPointer, protocolSequencePointer, networkAddressPointer, endpointPointer, optionsPointer, stringBindingPointer),
            { AppendWideText(detailBuffer, L"protseq="); AppendWideText(detailBuffer, reinterpret_cast<LPCWSTR>(protocolSequencePointer)); AppendWideText(detailBuffer, L" addr="); AppendWideText(detailBuffer, reinterpret_cast<LPCWSTR>(networkAddressPointer)); AppendWideText(detailBuffer, L" endpoint="); AppendWideText(detailBuffer, reinterpret_cast<LPCWSTR>(endpointPointer)); AppendWideText(detailBuffer, L" binding="); AppendHexText(detailBuffer, stringBindingPointer != nullptr ? reinterpret_cast<std::uint64_t>(*stringBindingPointer) : 0); })
        APIMON_SIMPLE_RPC_STATUS_HOOK(HookedRpcStringBindingComposeA, g_rpcStringBindingComposeAOriginal, ks::winapi_monitor::EventCategory::Network, L"Rpcrt4", L"RpcStringBindingComposeA",
            (RPC_CSTR objUuidPointer, RPC_CSTR protocolSequencePointer, RPC_CSTR networkAddressPointer, RPC_CSTR endpointPointer, RPC_CSTR optionsPointer, RPC_CSTR* stringBindingPointer),
            (objUuidPointer, protocolSequencePointer, networkAddressPointer, endpointPointer, optionsPointer, stringBindingPointer),
            { AppendWideText(detailBuffer, L"protseq="); AppendAnsiText(detailBuffer, reinterpret_cast<LPCSTR>(protocolSequencePointer)); AppendWideText(detailBuffer, L" addr="); AppendAnsiText(detailBuffer, reinterpret_cast<LPCSTR>(networkAddressPointer)); AppendWideText(detailBuffer, L" endpoint="); AppendAnsiText(detailBuffer, reinterpret_cast<LPCSTR>(endpointPointer)); AppendWideText(detailBuffer, L" binding="); AppendHexText(detailBuffer, stringBindingPointer != nullptr ? reinterpret_cast<std::uint64_t>(*stringBindingPointer) : 0); })
        APIMON_SIMPLE_RPC_STATUS_HOOK(HookedRpcBindingFromStringBindingW, g_rpcBindingFromStringBindingWOriginal, ks::winapi_monitor::EventCategory::Network, L"Rpcrt4", L"RpcBindingFromStringBindingW",
            (RPC_WSTR stringBindingPointer, RPC_BINDING_HANDLE* bindingHandlePointer), (stringBindingPointer, bindingHandlePointer),
            { AppendWideText(detailBuffer, L"bindingText="); AppendWideText(detailBuffer, reinterpret_cast<LPCWSTR>(stringBindingPointer)); AppendWideText(detailBuffer, L" binding="); AppendHexText(detailBuffer, bindingHandlePointer != nullptr ? reinterpret_cast<std::uint64_t>(*bindingHandlePointer) : 0); })
        APIMON_SIMPLE_RPC_STATUS_HOOK(HookedRpcBindingFromStringBindingA, g_rpcBindingFromStringBindingAOriginal, ks::winapi_monitor::EventCategory::Network, L"Rpcrt4", L"RpcBindingFromStringBindingA",
            (RPC_CSTR stringBindingPointer, RPC_BINDING_HANDLE* bindingHandlePointer), (stringBindingPointer, bindingHandlePointer),
            { AppendWideText(detailBuffer, L"bindingText="); AppendAnsiText(detailBuffer, reinterpret_cast<LPCSTR>(stringBindingPointer)); AppendWideText(detailBuffer, L" binding="); AppendHexText(detailBuffer, bindingHandlePointer != nullptr ? reinterpret_cast<std::uint64_t>(*bindingHandlePointer) : 0); })
        APIMON_SIMPLE_RPC_STATUS_HOOK(HookedRpcBindingFree, g_rpcBindingFreeOriginal, ks::winapi_monitor::EventCategory::Network, L"Rpcrt4", L"RpcBindingFree",
            (RPC_BINDING_HANDLE* bindingHandlePointer), (bindingHandlePointer),
            { AppendWideText(detailBuffer, L"binding="); AppendHexText(detailBuffer, bindingHandlePointer != nullptr ? reinterpret_cast<std::uint64_t>(*bindingHandlePointer) : 0); })
        APIMON_SIMPLE_RPC_STATUS_HOOK(HookedRpcMgmtEpEltInqBegin, g_rpcMgmtEpEltInqBeginOriginal, ks::winapi_monitor::EventCategory::Network, L"Rpcrt4", L"RpcMgmtEpEltInqBegin",
            (RPC_BINDING_HANDLE endpointBindingHandle, unsigned long inquiryType, RPC_IF_ID* ifIdPointer, unsigned long versionOption, UUID* objectUuidPointer, RPC_EP_INQ_HANDLE* inquiryContextPointer),
            (endpointBindingHandle, inquiryType, ifIdPointer, versionOption, objectUuidPointer, inquiryContextPointer),
            { AppendWideText(detailBuffer, L"binding="); AppendHexText(detailBuffer, reinterpret_cast<std::uint64_t>(endpointBindingHandle)); AppendWideText(detailBuffer, L" type="); AppendHexText(detailBuffer, inquiryType); AppendWideText(detailBuffer, L" versionOpt="); AppendHexText(detailBuffer, versionOption); AppendWideText(detailBuffer, L" inquiry="); AppendHexText(detailBuffer, inquiryContextPointer != nullptr ? reinterpret_cast<std::uint64_t>(*inquiryContextPointer) : 0); })
        APIMON_SIMPLE_RPC_STATUS_HOOK(HookedRpcMgmtEpEltInqNextW, g_rpcMgmtEpEltInqNextWOriginal, ks::winapi_monitor::EventCategory::Network, L"Rpcrt4", L"RpcMgmtEpEltInqNextW",
            (RPC_EP_INQ_HANDLE inquiryContext, RPC_IF_ID* ifIdPointer, RPC_BINDING_HANDLE* bindingHandlePointer, UUID* objectUuidPointer, RPC_WSTR* annotationPointer),
            (inquiryContext, ifIdPointer, bindingHandlePointer, objectUuidPointer, annotationPointer),
            { AppendWideText(detailBuffer, L"inquiry="); AppendHexText(detailBuffer, reinterpret_cast<std::uint64_t>(inquiryContext)); AppendWideText(detailBuffer, L" binding="); AppendHexText(detailBuffer, bindingHandlePointer != nullptr ? reinterpret_cast<std::uint64_t>(*bindingHandlePointer) : 0); AppendWideText(detailBuffer, L" annotation="); AppendWideText(detailBuffer, annotationPointer != nullptr ? reinterpret_cast<LPCWSTR>(*annotationPointer) : nullptr); })
        APIMON_SIMPLE_RPC_STATUS_HOOK(HookedRpcMgmtEpEltInqNextA, g_rpcMgmtEpEltInqNextAOriginal, ks::winapi_monitor::EventCategory::Network, L"Rpcrt4", L"RpcMgmtEpEltInqNextA",
            (RPC_EP_INQ_HANDLE inquiryContext, RPC_IF_ID* ifIdPointer, RPC_BINDING_HANDLE* bindingHandlePointer, UUID* objectUuidPointer, RPC_CSTR* annotationPointer),
            (inquiryContext, ifIdPointer, bindingHandlePointer, objectUuidPointer, annotationPointer),
            { AppendWideText(detailBuffer, L"inquiry="); AppendHexText(detailBuffer, reinterpret_cast<std::uint64_t>(inquiryContext)); AppendWideText(detailBuffer, L" binding="); AppendHexText(detailBuffer, bindingHandlePointer != nullptr ? reinterpret_cast<std::uint64_t>(*bindingHandlePointer) : 0); AppendWideText(detailBuffer, L" annotation="); AppendAnsiText(detailBuffer, annotationPointer != nullptr ? reinterpret_cast<LPCSTR>(*annotationPointer) : nullptr); })
        APIMON_SIMPLE_RPC_STATUS_HOOK(HookedRpcMgmtEpEltInqDone, g_rpcMgmtEpEltInqDoneOriginal, ks::winapi_monitor::EventCategory::Network, L"Rpcrt4", L"RpcMgmtEpEltInqDone",
            (RPC_EP_INQ_HANDLE* inquiryContextPointer), (inquiryContextPointer),
            { AppendWideText(detailBuffer, L"inquiry="); AppendHexText(detailBuffer, inquiryContextPointer != nullptr ? reinterpret_cast<std::uint64_t>(*inquiryContextPointer) : 0); })

        APIMON_SIMPLE_NTSTATUS_HOOK(HookedNtQueryInformationToken, g_ntQueryInformationTokenOriginal, ks::winapi_monitor::EventCategory::Process, L"ntdll", L"NtQueryInformationToken",
            (HANDLE tokenHandle, ULONG infoClass, PVOID tokenInformationPointer, ULONG tokenInformationLength, PULONG returnLengthPointer),
            (tokenHandle, infoClass, tokenInformationPointer, tokenInformationLength, returnLengthPointer),
            { AppendWideText(detailBuffer, L"token="); AppendHexText(detailBuffer, reinterpret_cast<std::uint64_t>(tokenHandle)); AppendWideText(detailBuffer, L" class="); AppendUnsignedText(detailBuffer, infoClass); AppendWideText(detailBuffer, L" length="); AppendUnsignedText(detailBuffer, tokenInformationLength); AppendWideText(detailBuffer, L" return="); AppendUnsignedText(detailBuffer, returnLengthPointer != nullptr ? *returnLengthPointer : 0); })
        APIMON_SIMPLE_NTSTATUS_HOOK(HookedNtSetInformationToken, g_ntSetInformationTokenOriginal, ks::winapi_monitor::EventCategory::Process, L"ntdll", L"NtSetInformationToken",
            (HANDLE tokenHandle, ULONG infoClass, PVOID tokenInformationPointer, ULONG tokenInformationLength),
            (tokenHandle, infoClass, tokenInformationPointer, tokenInformationLength),
            { AppendWideText(detailBuffer, L"token="); AppendHexText(detailBuffer, reinterpret_cast<std::uint64_t>(tokenHandle)); AppendWideText(detailBuffer, L" class="); AppendUnsignedText(detailBuffer, infoClass); AppendWideText(detailBuffer, L" length="); AppendUnsignedText(detailBuffer, tokenInformationLength); })
        APIMON_SIMPLE_NTSTATUS_HOOK(HookedNtAdjustPrivilegesToken, g_ntAdjustPrivilegesTokenOriginal, ks::winapi_monitor::EventCategory::Process, L"ntdll", L"NtAdjustPrivilegesToken",
            (HANDLE tokenHandle, BOOLEAN disableAllPrivileges, PTOKEN_PRIVILEGES newStatePointer, ULONG bufferLength, PTOKEN_PRIVILEGES previousStatePointer, PULONG returnLengthPointer),
            (tokenHandle, disableAllPrivileges, newStatePointer, bufferLength, previousStatePointer, returnLengthPointer),
            { AppendWideText(detailBuffer, L"token="); AppendHexText(detailBuffer, reinterpret_cast<std::uint64_t>(tokenHandle)); AppendWideText(detailBuffer, L" disableAll="); AppendUnsignedText(detailBuffer, disableAllPrivileges != FALSE ? 1ULL : 0ULL); AppendWideText(detailBuffer, L" privileges="); AppendUnsignedText(detailBuffer, newStatePointer != nullptr ? newStatePointer->PrivilegeCount : 0); AppendWideText(detailBuffer, L" length="); AppendUnsignedText(detailBuffer, bufferLength); })
        APIMON_SIMPLE_NTSTATUS_HOOK(HookedNtCreateMutant, g_ntCreateMutantOriginal, ks::winapi_monitor::EventCategory::Process, L"ntdll", L"NtCreateMutant",
            (PHANDLE mutantHandlePointer, ACCESS_MASK desiredAccess, POBJECT_ATTRIBUTES objectAttributesPointer, BOOLEAN initialOwner),
            (mutantHandlePointer, desiredAccess, objectAttributesPointer, initialOwner),
            { AppendWideText(detailBuffer, L"access="); AppendHexText(detailBuffer, desiredAccess); AppendWideText(detailBuffer, L" initialOwner="); AppendUnsignedText(detailBuffer, initialOwner != FALSE ? 1ULL : 0ULL); AppendWideText(detailBuffer, L" object="); AppendObjectNameText(detailBuffer, objectAttributesPointer); AppendWideText(detailBuffer, L" handle="); AppendHexText(detailBuffer, mutantHandlePointer != nullptr ? reinterpret_cast<std::uint64_t>(*mutantHandlePointer) : 0); })
        APIMON_SIMPLE_NTSTATUS_HOOK(HookedNtOpenMutant, g_ntOpenMutantOriginal, ks::winapi_monitor::EventCategory::Process, L"ntdll", L"NtOpenMutant",
            (PHANDLE mutantHandlePointer, ACCESS_MASK desiredAccess, POBJECT_ATTRIBUTES objectAttributesPointer),
            (mutantHandlePointer, desiredAccess, objectAttributesPointer),
            { AppendWideText(detailBuffer, L"access="); AppendHexText(detailBuffer, desiredAccess); AppendWideText(detailBuffer, L" object="); AppendObjectNameText(detailBuffer, objectAttributesPointer); AppendWideText(detailBuffer, L" handle="); AppendHexText(detailBuffer, mutantHandlePointer != nullptr ? reinterpret_cast<std::uint64_t>(*mutantHandlePointer) : 0); })
        APIMON_SIMPLE_NTSTATUS_HOOK(HookedNtReleaseMutant, g_ntReleaseMutantOriginal, ks::winapi_monitor::EventCategory::Process, L"ntdll", L"NtReleaseMutant",
            (HANDLE mutantHandle, PLONG previousCountPointer), (mutantHandle, previousCountPointer),
            { AppendWideText(detailBuffer, L"mutant="); AppendHexText(detailBuffer, reinterpret_cast<std::uint64_t>(mutantHandle)); AppendWideText(detailBuffer, L" previous="); AppendUnsignedText(detailBuffer, previousCountPointer != nullptr ? static_cast<unsigned long long>(*previousCountPointer < 0 ? 0 : *previousCountPointer) : 0ULL); })
        APIMON_SIMPLE_NTSTATUS_HOOK(HookedNtCreateEvent, g_ntCreateEventOriginal, ks::winapi_monitor::EventCategory::Process, L"ntdll", L"NtCreateEvent",
            (PHANDLE eventHandlePointer, ACCESS_MASK desiredAccess, POBJECT_ATTRIBUTES objectAttributesPointer, ULONG eventType, BOOLEAN initialState),
            (eventHandlePointer, desiredAccess, objectAttributesPointer, eventType, initialState),
            { AppendWideText(detailBuffer, L"access="); AppendHexText(detailBuffer, desiredAccess); AppendWideText(detailBuffer, L" type="); AppendUnsignedText(detailBuffer, eventType); AppendWideText(detailBuffer, L" initial="); AppendUnsignedText(detailBuffer, initialState != FALSE ? 1ULL : 0ULL); AppendWideText(detailBuffer, L" object="); AppendObjectNameText(detailBuffer, objectAttributesPointer); AppendWideText(detailBuffer, L" handle="); AppendHexText(detailBuffer, eventHandlePointer != nullptr ? reinterpret_cast<std::uint64_t>(*eventHandlePointer) : 0); })
        APIMON_SIMPLE_NTSTATUS_HOOK(HookedNtOpenEvent, g_ntOpenEventOriginal, ks::winapi_monitor::EventCategory::Process, L"ntdll", L"NtOpenEvent",
            (PHANDLE eventHandlePointer, ACCESS_MASK desiredAccess, POBJECT_ATTRIBUTES objectAttributesPointer),
            (eventHandlePointer, desiredAccess, objectAttributesPointer),
            { AppendWideText(detailBuffer, L"access="); AppendHexText(detailBuffer, desiredAccess); AppendWideText(detailBuffer, L" object="); AppendObjectNameText(detailBuffer, objectAttributesPointer); AppendWideText(detailBuffer, L" handle="); AppendHexText(detailBuffer, eventHandlePointer != nullptr ? reinterpret_cast<std::uint64_t>(*eventHandlePointer) : 0); })
        APIMON_SIMPLE_NTSTATUS_HOOK(HookedNtSetEvent, g_ntSetEventOriginal, ks::winapi_monitor::EventCategory::Process, L"ntdll", L"NtSetEvent",
            (HANDLE eventHandle, PLONG previousStatePointer), (eventHandle, previousStatePointer),
            { AppendWideText(detailBuffer, L"event="); AppendHexText(detailBuffer, reinterpret_cast<std::uint64_t>(eventHandle)); AppendWideText(detailBuffer, L" previous="); AppendUnsignedText(detailBuffer, previousStatePointer != nullptr ? static_cast<unsigned long long>(*previousStatePointer < 0 ? 0 : *previousStatePointer) : 0ULL); })
        APIMON_SIMPLE_NTSTATUS_HOOK(HookedNtResetEvent, g_ntResetEventOriginal, ks::winapi_monitor::EventCategory::Process, L"ntdll", L"NtResetEvent",
            (HANDLE eventHandle, PLONG previousStatePointer), (eventHandle, previousStatePointer),
            { AppendWideText(detailBuffer, L"event="); AppendHexText(detailBuffer, reinterpret_cast<std::uint64_t>(eventHandle)); AppendWideText(detailBuffer, L" previous="); AppendUnsignedText(detailBuffer, previousStatePointer != nullptr ? static_cast<unsigned long long>(*previousStatePointer < 0 ? 0 : *previousStatePointer) : 0ULL); })
        APIMON_SIMPLE_NTSTATUS_HOOK(HookedNtWaitForSingleObject, g_ntWaitForSingleObjectOriginal, ks::winapi_monitor::EventCategory::Process, L"ntdll", L"NtWaitForSingleObject",
            (HANDLE objectHandle, BOOLEAN alertableValue, PLARGE_INTEGER timeoutPointer), (objectHandle, alertableValue, timeoutPointer),
            { AppendWideText(detailBuffer, L"handle="); AppendHexText(detailBuffer, reinterpret_cast<std::uint64_t>(objectHandle)); AppendWideText(detailBuffer, L" alertable="); AppendUnsignedText(detailBuffer, alertableValue != FALSE ? 1ULL : 0ULL); AppendWideText(detailBuffer, L" timeout="); AppendHexText(detailBuffer, timeoutPointer != nullptr ? static_cast<std::uint64_t>(timeoutPointer->QuadPart) : 0); })
        APIMON_SIMPLE_NTSTATUS_HOOK(HookedNtWaitForMultipleObjects, g_ntWaitForMultipleObjectsOriginal, ks::winapi_monitor::EventCategory::Process, L"ntdll", L"NtWaitForMultipleObjects",
            (ULONG countValue, HANDLE* handlesPointer, ULONG waitType, BOOLEAN alertableValue, PLARGE_INTEGER timeoutPointer),
            (countValue, handlesPointer, waitType, alertableValue, timeoutPointer),
            { AppendWideText(detailBuffer, L"count="); AppendUnsignedText(detailBuffer, countValue); AppendWideText(detailBuffer, L" first="); AppendHexText(detailBuffer, handlesPointer != nullptr && countValue != 0 ? reinterpret_cast<std::uint64_t>(handlesPointer[0]) : 0); AppendWideText(detailBuffer, L" waitType="); AppendUnsignedText(detailBuffer, waitType); AppendWideText(detailBuffer, L" alertable="); AppendUnsignedText(detailBuffer, alertableValue != FALSE ? 1ULL : 0ULL); })
        APIMON_SIMPLE_NTSTATUS_HOOK(HookedNtQuerySystemInformation, g_ntQuerySystemInformationOriginal, ks::winapi_monitor::EventCategory::Process, L"ntdll", L"NtQuerySystemInformation",
            (ULONG infoClass, PVOID systemInformationPointer, ULONG systemInformationLength, PULONG returnLengthPointer),
            (infoClass, systemInformationPointer, systemInformationLength, returnLengthPointer),
            { AppendWideText(detailBuffer, L"class="); AppendUnsignedText(detailBuffer, infoClass); AppendWideText(detailBuffer, L" length="); AppendUnsignedText(detailBuffer, systemInformationLength); AppendWideText(detailBuffer, L" return="); AppendUnsignedText(detailBuffer, returnLengthPointer != nullptr ? *returnLengthPointer : 0); })
        APIMON_SIMPLE_NTSTATUS_HOOK(HookedNtQueryObject, g_ntQueryObjectOriginal, ks::winapi_monitor::EventCategory::Process, L"ntdll", L"NtQueryObject",
            (HANDLE objectHandle, ULONG infoClass, PVOID objectInformationPointer, ULONG objectInformationLength, PULONG returnLengthPointer),
            (objectHandle, infoClass, objectInformationPointer, objectInformationLength, returnLengthPointer),
            { AppendWideText(detailBuffer, L"object="); AppendHexText(detailBuffer, reinterpret_cast<std::uint64_t>(objectHandle)); AppendWideText(detailBuffer, L" class="); AppendUnsignedText(detailBuffer, infoClass); AppendWideText(detailBuffer, L" length="); AppendUnsignedText(detailBuffer, objectInformationLength); AppendWideText(detailBuffer, L" return="); AppendUnsignedText(detailBuffer, returnLengthPointer != nullptr ? *returnLengthPointer : 0); })

        // HookedFifthBatchSecurity 作用：
        // - 输入：登录、令牌、凭据、LSA 和 EventLog 参数；
        // - 处理：补齐账号认证、凭据访问、本机安全策略枚举和日志读写行为面，不记录密码/凭据内容；
        // - 返回：保持 Win32 BOOL/HANDLE 或 NTSTATUS 原始语义。
        APIMON_SIMPLE_BOOL_HOOK(HookedLogonUserW, g_logonUserWOriginal, ks::winapi_monitor::EventCategory::Process, L"Advapi32", L"LogonUserW",
            (LPCWSTR userNamePointer, LPCWSTR domainPointer, LPCWSTR passwordPointer, DWORD logonType, DWORD logonProvider, PHANDLE tokenHandlePointer),
            (userNamePointer, domainPointer, passwordPointer, logonType, logonProvider, tokenHandlePointer),
            { AppendWideText(detailBuffer, L"user="); AppendWideText(detailBuffer, domainPointer); AppendWideText(detailBuffer, L"\\"); AppendWideText(detailBuffer, userNamePointer); AppendWideText(detailBuffer, L" type="); AppendUnsignedText(detailBuffer, logonType); AppendWideText(detailBuffer, L" provider="); AppendUnsignedText(detailBuffer, logonProvider); AppendWideText(detailBuffer, L" token="); AppendHexText(detailBuffer, tokenHandlePointer != nullptr ? reinterpret_cast<std::uint64_t>(*tokenHandlePointer) : 0); })
        APIMON_SIMPLE_BOOL_HOOK(HookedLogonUserA, g_logonUserAOriginal, ks::winapi_monitor::EventCategory::Process, L"Advapi32", L"LogonUserA",
            (LPCSTR userNamePointer, LPCSTR domainPointer, LPCSTR passwordPointer, DWORD logonType, DWORD logonProvider, PHANDLE tokenHandlePointer),
            (userNamePointer, domainPointer, passwordPointer, logonType, logonProvider, tokenHandlePointer),
            { AppendWideText(detailBuffer, L"user="); AppendAnsiText(detailBuffer, domainPointer); AppendWideText(detailBuffer, L"\\"); AppendAnsiText(detailBuffer, userNamePointer); AppendWideText(detailBuffer, L" type="); AppendUnsignedText(detailBuffer, logonType); AppendWideText(detailBuffer, L" provider="); AppendUnsignedText(detailBuffer, logonProvider); AppendWideText(detailBuffer, L" token="); AppendHexText(detailBuffer, tokenHandlePointer != nullptr ? reinterpret_cast<std::uint64_t>(*tokenHandlePointer) : 0); })
        APIMON_SIMPLE_BOOL_HOOK(HookedGetTokenInformation, g_getTokenInformationOriginal, ks::winapi_monitor::EventCategory::Process, L"Advapi32", L"GetTokenInformation",
            (HANDLE tokenHandle, TOKEN_INFORMATION_CLASS infoClass, LPVOID tokenInformationPointer, DWORD tokenInformationLength, PDWORD returnLengthPointer),
            (tokenHandle, infoClass, tokenInformationPointer, tokenInformationLength, returnLengthPointer),
            { AppendWideText(detailBuffer, L"token="); AppendHexText(detailBuffer, reinterpret_cast<std::uint64_t>(tokenHandle)); AppendWideText(detailBuffer, L" class="); AppendUnsignedText(detailBuffer, static_cast<unsigned long long>(infoClass)); AppendWideText(detailBuffer, L" length="); AppendUnsignedText(detailBuffer, tokenInformationLength); AppendWideText(detailBuffer, L" return="); AppendUnsignedText(detailBuffer, returnLengthPointer != nullptr ? *returnLengthPointer : 0); })
        APIMON_SIMPLE_BOOL_HOOK(HookedSetTokenInformation, g_setTokenInformationOriginal, ks::winapi_monitor::EventCategory::Process, L"Advapi32", L"SetTokenInformation",
            (HANDLE tokenHandle, TOKEN_INFORMATION_CLASS infoClass, LPVOID tokenInformationPointer, DWORD tokenInformationLength),
            (tokenHandle, infoClass, tokenInformationPointer, tokenInformationLength),
            { AppendWideText(detailBuffer, L"token="); AppendHexText(detailBuffer, reinterpret_cast<std::uint64_t>(tokenHandle)); AppendWideText(detailBuffer, L" class="); AppendUnsignedText(detailBuffer, static_cast<unsigned long long>(infoClass)); AppendWideText(detailBuffer, L" length="); AppendUnsignedText(detailBuffer, tokenInformationLength); })
        APIMON_SIMPLE_BOOL_HOOK(HookedCheckTokenMembership, g_checkTokenMembershipOriginal, ks::winapi_monitor::EventCategory::Process, L"Advapi32", L"CheckTokenMembership",
            (HANDLE tokenHandle, PSID sidPointer, PBOOL isMemberPointer), (tokenHandle, sidPointer, isMemberPointer),
            { AppendWideText(detailBuffer, L"token="); AppendHexText(detailBuffer, reinterpret_cast<std::uint64_t>(tokenHandle)); AppendWideText(detailBuffer, L" sid="); AppendHexText(detailBuffer, reinterpret_cast<std::uint64_t>(sidPointer)); AppendWideText(detailBuffer, L" member="); AppendUnsignedText(detailBuffer, isMemberPointer != nullptr && *isMemberPointer != FALSE ? 1ULL : 0ULL); })
        APIMON_SIMPLE_BOOL_HOOK(HookedCreateRestrictedToken, g_createRestrictedTokenOriginal, ks::winapi_monitor::EventCategory::Process, L"Advapi32", L"CreateRestrictedToken",
            (HANDLE existingTokenHandle, DWORD flagsValue, DWORD disableSidCount, PSID_AND_ATTRIBUTES sidsToDisablePointer, DWORD deletePrivilegeCount, PLUID_AND_ATTRIBUTES privilegesToDeletePointer, DWORD restrictSidCount, PSID_AND_ATTRIBUTES sidsToRestrictPointer, PHANDLE newTokenHandlePointer),
            (existingTokenHandle, flagsValue, disableSidCount, sidsToDisablePointer, deletePrivilegeCount, privilegesToDeletePointer, restrictSidCount, sidsToRestrictPointer, newTokenHandlePointer),
            { AppendWideText(detailBuffer, L"token="); AppendHexText(detailBuffer, reinterpret_cast<std::uint64_t>(existingTokenHandle)); AppendWideText(detailBuffer, L" flags="); AppendHexText(detailBuffer, flagsValue); AppendWideText(detailBuffer, L" disableSids="); AppendUnsignedText(detailBuffer, disableSidCount); AppendWideText(detailBuffer, L" deletePrivs="); AppendUnsignedText(detailBuffer, deletePrivilegeCount); AppendWideText(detailBuffer, L" restrictSids="); AppendUnsignedText(detailBuffer, restrictSidCount); AppendWideText(detailBuffer, L" newToken="); AppendHexText(detailBuffer, newTokenHandlePointer != nullptr ? reinterpret_cast<std::uint64_t>(*newTokenHandlePointer) : 0); })
        APIMON_SIMPLE_BOOL_HOOK(HookedImpersonateSelf, g_impersonateSelfOriginal, ks::winapi_monitor::EventCategory::Process, L"Advapi32", L"ImpersonateSelf",
            (SECURITY_IMPERSONATION_LEVEL impersonationLevel), (impersonationLevel),
            { AppendWideText(detailBuffer, L"level="); AppendUnsignedText(detailBuffer, static_cast<unsigned long long>(impersonationLevel)); })
        APIMON_SIMPLE_BOOL_HOOK(HookedImpersonateNamedPipeClient, g_impersonateNamedPipeClientOriginal, ks::winapi_monitor::EventCategory::Process, L"Advapi32", L"ImpersonateNamedPipeClient",
            (HANDLE pipeHandle), (pipeHandle),
            { BuildSimpleHandleDetail(detailBuffer, L"pipe", pipeHandle); })
        APIMON_SIMPLE_BOOL_HOOK(HookedCredReadW, g_credReadWOriginal, ks::winapi_monitor::EventCategory::Process, L"Advapi32", L"CredReadW",
            (LPCWSTR targetNamePointer, DWORD typeValue, DWORD flagsValue, PVOID* credentialPointer), (targetNamePointer, typeValue, flagsValue, credentialPointer),
            { AppendWideText(detailBuffer, L"target="); AppendWideText(detailBuffer, targetNamePointer); AppendWideText(detailBuffer, L" type="); AppendUnsignedText(detailBuffer, typeValue); AppendWideText(detailBuffer, L" flags="); AppendHexText(detailBuffer, flagsValue); AppendWideText(detailBuffer, L" credential="); AppendHexText(detailBuffer, credentialPointer != nullptr ? reinterpret_cast<std::uint64_t>(*credentialPointer) : 0); })
        APIMON_SIMPLE_BOOL_HOOK(HookedCredReadA, g_credReadAOriginal, ks::winapi_monitor::EventCategory::Process, L"Advapi32", L"CredReadA",
            (LPCSTR targetNamePointer, DWORD typeValue, DWORD flagsValue, PVOID* credentialPointer), (targetNamePointer, typeValue, flagsValue, credentialPointer),
            { AppendWideText(detailBuffer, L"target="); AppendAnsiText(detailBuffer, targetNamePointer); AppendWideText(detailBuffer, L" type="); AppendUnsignedText(detailBuffer, typeValue); AppendWideText(detailBuffer, L" flags="); AppendHexText(detailBuffer, flagsValue); AppendWideText(detailBuffer, L" credential="); AppendHexText(detailBuffer, credentialPointer != nullptr ? reinterpret_cast<std::uint64_t>(*credentialPointer) : 0); })
        APIMON_SIMPLE_BOOL_HOOK(HookedCredEnumerateW, g_credEnumerateWOriginal, ks::winapi_monitor::EventCategory::Process, L"Advapi32", L"CredEnumerateW",
            (LPCWSTR filterPointer, DWORD flagsValue, DWORD* countPointer, PVOID* credentialArrayPointer), (filterPointer, flagsValue, countPointer, credentialArrayPointer),
            { AppendWideText(detailBuffer, L"filter="); AppendWideText(detailBuffer, filterPointer); AppendWideText(detailBuffer, L" flags="); AppendHexText(detailBuffer, flagsValue); AppendWideText(detailBuffer, L" count="); AppendUnsignedText(detailBuffer, countPointer != nullptr ? *countPointer : 0); })
        APIMON_SIMPLE_BOOL_HOOK(HookedCredEnumerateA, g_credEnumerateAOriginal, ks::winapi_monitor::EventCategory::Process, L"Advapi32", L"CredEnumerateA",
            (LPCSTR filterPointer, DWORD flagsValue, DWORD* countPointer, PVOID* credentialArrayPointer), (filterPointer, flagsValue, countPointer, credentialArrayPointer),
            { AppendWideText(detailBuffer, L"filter="); AppendAnsiText(detailBuffer, filterPointer); AppendWideText(detailBuffer, L" flags="); AppendHexText(detailBuffer, flagsValue); AppendWideText(detailBuffer, L" count="); AppendUnsignedText(detailBuffer, countPointer != nullptr ? *countPointer : 0); })
        APIMON_SIMPLE_BOOL_HOOK(HookedCredWriteW, g_credWriteWOriginal, ks::winapi_monitor::EventCategory::Process, L"Advapi32", L"CredWriteW",
            (PVOID credentialPointer, DWORD flagsValue), (credentialPointer, flagsValue),
            { AppendWideText(detailBuffer, L"credential="); AppendHexText(detailBuffer, reinterpret_cast<std::uint64_t>(credentialPointer)); AppendWideText(detailBuffer, L" flags="); AppendHexText(detailBuffer, flagsValue); })
        APIMON_SIMPLE_BOOL_HOOK(HookedCredWriteA, g_credWriteAOriginal, ks::winapi_monitor::EventCategory::Process, L"Advapi32", L"CredWriteA",
            (PVOID credentialPointer, DWORD flagsValue), (credentialPointer, flagsValue),
            { AppendWideText(detailBuffer, L"credential="); AppendHexText(detailBuffer, reinterpret_cast<std::uint64_t>(credentialPointer)); AppendWideText(detailBuffer, L" flags="); AppendHexText(detailBuffer, flagsValue); })
        APIMON_SIMPLE_BOOL_HOOK(HookedCredDeleteW, g_credDeleteWOriginal, ks::winapi_monitor::EventCategory::Process, L"Advapi32", L"CredDeleteW",
            (LPCWSTR targetNamePointer, DWORD typeValue, DWORD flagsValue), (targetNamePointer, typeValue, flagsValue),
            { AppendWideText(detailBuffer, L"target="); AppendWideText(detailBuffer, targetNamePointer); AppendWideText(detailBuffer, L" type="); AppendUnsignedText(detailBuffer, typeValue); AppendWideText(detailBuffer, L" flags="); AppendHexText(detailBuffer, flagsValue); })
        APIMON_SIMPLE_BOOL_HOOK(HookedCredDeleteA, g_credDeleteAOriginal, ks::winapi_monitor::EventCategory::Process, L"Advapi32", L"CredDeleteA",
            (LPCSTR targetNamePointer, DWORD typeValue, DWORD flagsValue), (targetNamePointer, typeValue, flagsValue),
            { AppendWideText(detailBuffer, L"target="); AppendAnsiText(detailBuffer, targetNamePointer); AppendWideText(detailBuffer, L" type="); AppendUnsignedText(detailBuffer, typeValue); AppendWideText(detailBuffer, L" flags="); AppendHexText(detailBuffer, flagsValue); })
        APIMON_SIMPLE_VOID_HOOK(HookedCredFree, g_credFreeOriginal, ks::winapi_monitor::EventCategory::Process, L"Advapi32", L"CredFree",
            (PVOID bufferPointer), (bufferPointer),
            { AppendWideText(detailBuffer, L"buffer="); AppendHexText(detailBuffer, reinterpret_cast<std::uint64_t>(bufferPointer)); })

        APIMON_SIMPLE_NTSTATUS_HOOK(HookedLsaOpenPolicy, g_lsaOpenPolicyOriginal, ks::winapi_monitor::EventCategory::Process, L"Advapi32", L"LsaOpenPolicy",
            (PUNICODE_STRING systemNamePointer, PVOID objectAttributesPointer, ACCESS_MASK desiredAccess, PVOID* policyHandlePointer),
            (systemNamePointer, objectAttributesPointer, desiredAccess, policyHandlePointer),
            { AppendWideText(detailBuffer, L"system="); AppendUnicodeStringText(detailBuffer, systemNamePointer); AppendWideText(detailBuffer, L" access="); AppendHexText(detailBuffer, desiredAccess); AppendWideText(detailBuffer, L" policy="); AppendHexText(detailBuffer, policyHandlePointer != nullptr ? reinterpret_cast<std::uint64_t>(*policyHandlePointer) : 0); })
        APIMON_SIMPLE_NTSTATUS_HOOK(HookedLsaClose, g_lsaCloseOriginal, ks::winapi_monitor::EventCategory::Process, L"Advapi32", L"LsaClose",
            (PVOID objectHandle), (objectHandle),
            { AppendWideText(detailBuffer, L"handle="); AppendHexText(detailBuffer, reinterpret_cast<std::uint64_t>(objectHandle)); })
        APIMON_SIMPLE_NTSTATUS_HOOK(HookedLsaEnumerateLogonSessions, g_lsaEnumerateLogonSessionsOriginal, ks::winapi_monitor::EventCategory::Process, L"Advapi32", L"LsaEnumerateLogonSessions",
            (PULONG logonSessionCountPointer, PVOID* logonSessionListPointer), (logonSessionCountPointer, logonSessionListPointer),
            { AppendWideText(detailBuffer, L"count="); AppendUnsignedText(detailBuffer, logonSessionCountPointer != nullptr ? *logonSessionCountPointer : 0); AppendWideText(detailBuffer, L" list="); AppendHexText(detailBuffer, logonSessionListPointer != nullptr ? reinterpret_cast<std::uint64_t>(*logonSessionListPointer) : 0); })
        APIMON_SIMPLE_NTSTATUS_HOOK(HookedLsaGetLogonSessionData, g_lsaGetLogonSessionDataOriginal, ks::winapi_monitor::EventCategory::Process, L"Advapi32", L"LsaGetLogonSessionData",
            (PVOID logonIdPointer, PVOID* sessionDataPointer), (logonIdPointer, sessionDataPointer),
            { AppendWideText(detailBuffer, L"logonId="); AppendHexText(detailBuffer, reinterpret_cast<std::uint64_t>(logonIdPointer)); AppendWideText(detailBuffer, L" data="); AppendHexText(detailBuffer, sessionDataPointer != nullptr ? reinterpret_cast<std::uint64_t>(*sessionDataPointer) : 0); })
        APIMON_SIMPLE_NTSTATUS_HOOK(HookedLsaFreeReturnBuffer, g_lsaFreeReturnBufferOriginal, ks::winapi_monitor::EventCategory::Process, L"Advapi32", L"LsaFreeReturnBuffer",
            (PVOID bufferPointer), (bufferPointer),
            { AppendWideText(detailBuffer, L"buffer="); AppendHexText(detailBuffer, reinterpret_cast<std::uint64_t>(bufferPointer)); })
        APIMON_SIMPLE_NTSTATUS_HOOK(HookedLsaLookupNames2, g_lsaLookupNames2Original, ks::winapi_monitor::EventCategory::Process, L"Advapi32", L"LsaLookupNames2",
            (PVOID policyHandle, ULONG flagsValue, ULONG countValue, PUNICODE_STRING namesPointer, PVOID* referencedDomainsPointer, PVOID* sidsPointer),
            (policyHandle, flagsValue, countValue, namesPointer, referencedDomainsPointer, sidsPointer),
            { AppendWideText(detailBuffer, L"policy="); AppendHexText(detailBuffer, reinterpret_cast<std::uint64_t>(policyHandle)); AppendWideText(detailBuffer, L" flags="); AppendHexText(detailBuffer, flagsValue); AppendWideText(detailBuffer, L" count="); AppendUnsignedText(detailBuffer, countValue); AppendWideText(detailBuffer, L" first="); if (namesPointer != nullptr && countValue != 0) { AppendUnicodeStringText(detailBuffer, &namesPointer[0]); } })
        APIMON_SIMPLE_NTSTATUS_HOOK(HookedLsaLookupSids2, g_lsaLookupSids2Original, ks::winapi_monitor::EventCategory::Process, L"Advapi32", L"LsaLookupSids2",
            (PVOID policyHandle, ULONG flagsValue, ULONG countValue, PVOID* sidsPointer, PVOID* referencedDomainsPointer, PVOID* namesPointer),
            (policyHandle, flagsValue, countValue, sidsPointer, referencedDomainsPointer, namesPointer),
            { AppendWideText(detailBuffer, L"policy="); AppendHexText(detailBuffer, reinterpret_cast<std::uint64_t>(policyHandle)); AppendWideText(detailBuffer, L" flags="); AppendHexText(detailBuffer, flagsValue); AppendWideText(detailBuffer, L" count="); AppendUnsignedText(detailBuffer, countValue); AppendWideText(detailBuffer, L" firstSid="); AppendHexText(detailBuffer, sidsPointer != nullptr && countValue != 0 ? reinterpret_cast<std::uint64_t>(sidsPointer[0]) : 0); })

        APIMON_SIMPLE_HANDLE_HOOK(HANDLE, HookedOpenEventLogW, g_openEventLogWOriginal, ks::winapi_monitor::EventCategory::Process, L"Advapi32", L"OpenEventLogW",
            (LPCWSTR serverNamePointer, LPCWSTR sourceNamePointer), (serverNamePointer, sourceNamePointer),
            { AppendWideText(detailBuffer, L"server="); AppendWideText(detailBuffer, serverNamePointer); AppendWideText(detailBuffer, L" source="); AppendWideText(detailBuffer, sourceNamePointer); AppendWideText(detailBuffer, L" handle="); AppendHexText(detailBuffer, reinterpret_cast<std::uint64_t>(resultHandle)); })
        APIMON_SIMPLE_HANDLE_HOOK(HANDLE, HookedOpenEventLogA, g_openEventLogAOriginal, ks::winapi_monitor::EventCategory::Process, L"Advapi32", L"OpenEventLogA",
            (LPCSTR serverNamePointer, LPCSTR sourceNamePointer), (serverNamePointer, sourceNamePointer),
            { AppendWideText(detailBuffer, L"server="); AppendAnsiText(detailBuffer, serverNamePointer); AppendWideText(detailBuffer, L" source="); AppendAnsiText(detailBuffer, sourceNamePointer); AppendWideText(detailBuffer, L" handle="); AppendHexText(detailBuffer, reinterpret_cast<std::uint64_t>(resultHandle)); })
        APIMON_SIMPLE_HANDLE_HOOK(HANDLE, HookedRegisterEventSourceW, g_registerEventSourceWOriginal, ks::winapi_monitor::EventCategory::Process, L"Advapi32", L"RegisterEventSourceW",
            (LPCWSTR serverNamePointer, LPCWSTR sourceNamePointer), (serverNamePointer, sourceNamePointer),
            { AppendWideText(detailBuffer, L"server="); AppendWideText(detailBuffer, serverNamePointer); AppendWideText(detailBuffer, L" source="); AppendWideText(detailBuffer, sourceNamePointer); AppendWideText(detailBuffer, L" handle="); AppendHexText(detailBuffer, reinterpret_cast<std::uint64_t>(resultHandle)); })
        APIMON_SIMPLE_HANDLE_HOOK(HANDLE, HookedRegisterEventSourceA, g_registerEventSourceAOriginal, ks::winapi_monitor::EventCategory::Process, L"Advapi32", L"RegisterEventSourceA",
            (LPCSTR serverNamePointer, LPCSTR sourceNamePointer), (serverNamePointer, sourceNamePointer),
            { AppendWideText(detailBuffer, L"server="); AppendAnsiText(detailBuffer, serverNamePointer); AppendWideText(detailBuffer, L" source="); AppendAnsiText(detailBuffer, sourceNamePointer); AppendWideText(detailBuffer, L" handle="); AppendHexText(detailBuffer, reinterpret_cast<std::uint64_t>(resultHandle)); })
        APIMON_SIMPLE_BOOL_HOOK(HookedReadEventLogW, g_readEventLogWOriginal, ks::winapi_monitor::EventCategory::Process, L"Advapi32", L"ReadEventLogW",
            (HANDLE eventLogHandle, DWORD readFlags, DWORD recordOffset, LPVOID bufferPointer, DWORD bytesToRead, DWORD* bytesReadPointer, DWORD* minBytesNeededPointer),
            (eventLogHandle, readFlags, recordOffset, bufferPointer, bytesToRead, bytesReadPointer, minBytesNeededPointer),
            { AppendWideText(detailBuffer, L"log="); AppendHexText(detailBuffer, reinterpret_cast<std::uint64_t>(eventLogHandle)); AppendWideText(detailBuffer, L" flags="); AppendHexText(detailBuffer, readFlags); AppendWideText(detailBuffer, L" offset="); AppendUnsignedText(detailBuffer, recordOffset); AppendWideText(detailBuffer, L" bytes="); AppendUnsignedText(detailBuffer, bytesToRead); AppendWideText(detailBuffer, L" read="); AppendUnsignedText(detailBuffer, bytesReadPointer != nullptr ? *bytesReadPointer : 0); })
        APIMON_SIMPLE_BOOL_HOOK(HookedReadEventLogA, g_readEventLogAOriginal, ks::winapi_monitor::EventCategory::Process, L"Advapi32", L"ReadEventLogA",
            (HANDLE eventLogHandle, DWORD readFlags, DWORD recordOffset, LPVOID bufferPointer, DWORD bytesToRead, DWORD* bytesReadPointer, DWORD* minBytesNeededPointer),
            (eventLogHandle, readFlags, recordOffset, bufferPointer, bytesToRead, bytesReadPointer, minBytesNeededPointer),
            { AppendWideText(detailBuffer, L"log="); AppendHexText(detailBuffer, reinterpret_cast<std::uint64_t>(eventLogHandle)); AppendWideText(detailBuffer, L" flags="); AppendHexText(detailBuffer, readFlags); AppendWideText(detailBuffer, L" offset="); AppendUnsignedText(detailBuffer, recordOffset); AppendWideText(detailBuffer, L" bytes="); AppendUnsignedText(detailBuffer, bytesToRead); AppendWideText(detailBuffer, L" read="); AppendUnsignedText(detailBuffer, bytesReadPointer != nullptr ? *bytesReadPointer : 0); })
        APIMON_SIMPLE_BOOL_HOOK(HookedClearEventLogW, g_clearEventLogWOriginal, ks::winapi_monitor::EventCategory::Process, L"Advapi32", L"ClearEventLogW",
            (HANDLE eventLogHandle, LPCWSTR backupFileNamePointer), (eventLogHandle, backupFileNamePointer),
            { AppendWideText(detailBuffer, L"log="); AppendHexText(detailBuffer, reinterpret_cast<std::uint64_t>(eventLogHandle)); AppendWideText(detailBuffer, L" backup="); AppendWideText(detailBuffer, backupFileNamePointer); })
        APIMON_SIMPLE_BOOL_HOOK(HookedClearEventLogA, g_clearEventLogAOriginal, ks::winapi_monitor::EventCategory::Process, L"Advapi32", L"ClearEventLogA",
            (HANDLE eventLogHandle, LPCSTR backupFileNamePointer), (eventLogHandle, backupFileNamePointer),
            { AppendWideText(detailBuffer, L"log="); AppendHexText(detailBuffer, reinterpret_cast<std::uint64_t>(eventLogHandle)); AppendWideText(detailBuffer, L" backup="); AppendAnsiText(detailBuffer, backupFileNamePointer); })
        APIMON_SIMPLE_BOOL_HOOK(HookedReportEventW, g_reportEventWOriginal, ks::winapi_monitor::EventCategory::Process, L"Advapi32", L"ReportEventW",
            (HANDLE eventLogHandle, WORD typeValue, WORD categoryValue, DWORD eventIdValue, PSID userSidPointer, WORD stringCount, DWORD dataSize, LPCWSTR* stringsPointer, LPVOID rawDataPointer),
            (eventLogHandle, typeValue, categoryValue, eventIdValue, userSidPointer, stringCount, dataSize, stringsPointer, rawDataPointer),
            { AppendWideText(detailBuffer, L"log="); AppendHexText(detailBuffer, reinterpret_cast<std::uint64_t>(eventLogHandle)); AppendWideText(detailBuffer, L" type="); AppendUnsignedText(detailBuffer, typeValue); AppendWideText(detailBuffer, L" category="); AppendUnsignedText(detailBuffer, categoryValue); AppendWideText(detailBuffer, L" event="); AppendHexText(detailBuffer, eventIdValue); AppendWideText(detailBuffer, L" strings="); AppendUnsignedText(detailBuffer, stringCount); AppendWideText(detailBuffer, L" data="); AppendUnsignedText(detailBuffer, dataSize); })
        APIMON_SIMPLE_BOOL_HOOK(HookedReportEventA, g_reportEventAOriginal, ks::winapi_monitor::EventCategory::Process, L"Advapi32", L"ReportEventA",
            (HANDLE eventLogHandle, WORD typeValue, WORD categoryValue, DWORD eventIdValue, PSID userSidPointer, WORD stringCount, DWORD dataSize, LPCSTR* stringsPointer, LPVOID rawDataPointer),
            (eventLogHandle, typeValue, categoryValue, eventIdValue, userSidPointer, stringCount, dataSize, stringsPointer, rawDataPointer),
            { AppendWideText(detailBuffer, L"log="); AppendHexText(detailBuffer, reinterpret_cast<std::uint64_t>(eventLogHandle)); AppendWideText(detailBuffer, L" type="); AppendUnsignedText(detailBuffer, typeValue); AppendWideText(detailBuffer, L" category="); AppendUnsignedText(detailBuffer, categoryValue); AppendWideText(detailBuffer, L" event="); AppendHexText(detailBuffer, eventIdValue); AppendWideText(detailBuffer, L" strings="); AppendUnsignedText(detailBuffer, stringCount); AppendWideText(detailBuffer, L" data="); AppendUnsignedText(detailBuffer, dataSize); })
        APIMON_SIMPLE_BOOL_HOOK(HookedCloseEventLog, g_closeEventLogOriginal, ks::winapi_monitor::EventCategory::Process, L"Advapi32", L"CloseEventLog",
            (HANDLE eventLogHandle), (eventLogHandle),
            { BuildSimpleHandleDetail(detailBuffer, L"log", eventLogHandle); })

        // HookedFifthBatchInventory 作用：
        // - 输入：NetAPI、IP Helper、WTS、Job Object 和 SSPI/Secur32 参数；
        // - 处理：补齐域/共享/会话枚举、网络连接表枚举、终端会话枚举、进程 Job 控制和认证上下文行为；
        // - 返回：保持各 API 原始状态码、BOOL、HANDLE 或 void 语义。
        APIMON_SIMPLE_ULONG_STATUS_HOOK(HookedNetUserEnum, g_netUserEnumOriginal, ks::winapi_monitor::EventCategory::Network, L"Netapi32", L"NetUserEnum",
            (LPCWSTR serverNamePointer, DWORD levelValue, DWORD filterValue, LPBYTE* bufferPointer, DWORD preferredMaxLength, LPDWORD entriesReadPointer, LPDWORD totalEntriesPointer, LPDWORD resumeHandlePointer),
            (serverNamePointer, levelValue, filterValue, bufferPointer, preferredMaxLength, entriesReadPointer, totalEntriesPointer, resumeHandlePointer),
            { AppendWideText(detailBuffer, L"server="); AppendWideText(detailBuffer, serverNamePointer); AppendWideText(detailBuffer, L" level="); AppendUnsignedText(detailBuffer, levelValue); AppendWideText(detailBuffer, L" filter="); AppendHexText(detailBuffer, filterValue); AppendWideText(detailBuffer, L" read="); AppendUnsignedText(detailBuffer, entriesReadPointer != nullptr ? *entriesReadPointer : 0); AppendWideText(detailBuffer, L" total="); AppendUnsignedText(detailBuffer, totalEntriesPointer != nullptr ? *totalEntriesPointer : 0); })
        APIMON_SIMPLE_ULONG_STATUS_HOOK(HookedNetLocalGroupEnum, g_netLocalGroupEnumOriginal, ks::winapi_monitor::EventCategory::Network, L"Netapi32", L"NetLocalGroupEnum",
            (LPCWSTR serverNamePointer, DWORD levelValue, LPBYTE* bufferPointer, DWORD preferredMaxLength, LPDWORD entriesReadPointer, LPDWORD totalEntriesPointer, PDWORD_PTR resumeHandlePointer),
            (serverNamePointer, levelValue, bufferPointer, preferredMaxLength, entriesReadPointer, totalEntriesPointer, resumeHandlePointer),
            { AppendWideText(detailBuffer, L"server="); AppendWideText(detailBuffer, serverNamePointer); AppendWideText(detailBuffer, L" level="); AppendUnsignedText(detailBuffer, levelValue); AppendWideText(detailBuffer, L" read="); AppendUnsignedText(detailBuffer, entriesReadPointer != nullptr ? *entriesReadPointer : 0); AppendWideText(detailBuffer, L" total="); AppendUnsignedText(detailBuffer, totalEntriesPointer != nullptr ? *totalEntriesPointer : 0); })
        APIMON_SIMPLE_ULONG_STATUS_HOOK(HookedNetGroupEnum, g_netGroupEnumOriginal, ks::winapi_monitor::EventCategory::Network, L"Netapi32", L"NetGroupEnum",
            (LPCWSTR serverNamePointer, DWORD levelValue, LPBYTE* bufferPointer, DWORD preferredMaxLength, LPDWORD entriesReadPointer, LPDWORD totalEntriesPointer, PDWORD_PTR resumeHandlePointer),
            (serverNamePointer, levelValue, bufferPointer, preferredMaxLength, entriesReadPointer, totalEntriesPointer, resumeHandlePointer),
            { AppendWideText(detailBuffer, L"server="); AppendWideText(detailBuffer, serverNamePointer); AppendWideText(detailBuffer, L" level="); AppendUnsignedText(detailBuffer, levelValue); AppendWideText(detailBuffer, L" read="); AppendUnsignedText(detailBuffer, entriesReadPointer != nullptr ? *entriesReadPointer : 0); AppendWideText(detailBuffer, L" total="); AppendUnsignedText(detailBuffer, totalEntriesPointer != nullptr ? *totalEntriesPointer : 0); })
        APIMON_SIMPLE_ULONG_STATUS_HOOK(HookedNetShareEnum, g_netShareEnumOriginal, ks::winapi_monitor::EventCategory::Network, L"Netapi32", L"NetShareEnum",
            (LPWSTR serverNamePointer, DWORD levelValue, LPBYTE* bufferPointer, DWORD preferredMaxLength, LPDWORD entriesReadPointer, LPDWORD totalEntriesPointer, LPDWORD resumeHandlePointer),
            (serverNamePointer, levelValue, bufferPointer, preferredMaxLength, entriesReadPointer, totalEntriesPointer, resumeHandlePointer),
            { AppendWideText(detailBuffer, L"server="); AppendWideText(detailBuffer, serverNamePointer); AppendWideText(detailBuffer, L" level="); AppendUnsignedText(detailBuffer, levelValue); AppendWideText(detailBuffer, L" read="); AppendUnsignedText(detailBuffer, entriesReadPointer != nullptr ? *entriesReadPointer : 0); AppendWideText(detailBuffer, L" total="); AppendUnsignedText(detailBuffer, totalEntriesPointer != nullptr ? *totalEntriesPointer : 0); })
        APIMON_SIMPLE_ULONG_STATUS_HOOK(HookedNetSessionEnum, g_netSessionEnumOriginal, ks::winapi_monitor::EventCategory::Network, L"Netapi32", L"NetSessionEnum",
            (LPWSTR serverNamePointer, LPWSTR uncClientNamePointer, LPWSTR userNamePointer, DWORD levelValue, LPBYTE* bufferPointer, DWORD preferredMaxLength, LPDWORD entriesReadPointer, LPDWORD totalEntriesPointer, LPDWORD resumeHandlePointer),
            (serverNamePointer, uncClientNamePointer, userNamePointer, levelValue, bufferPointer, preferredMaxLength, entriesReadPointer, totalEntriesPointer, resumeHandlePointer),
            { AppendWideText(detailBuffer, L"server="); AppendWideText(detailBuffer, serverNamePointer); AppendWideText(detailBuffer, L" client="); AppendWideText(detailBuffer, uncClientNamePointer); AppendWideText(detailBuffer, L" user="); AppendWideText(detailBuffer, userNamePointer); AppendWideText(detailBuffer, L" level="); AppendUnsignedText(detailBuffer, levelValue); AppendWideText(detailBuffer, L" read="); AppendUnsignedText(detailBuffer, entriesReadPointer != nullptr ? *entriesReadPointer : 0); AppendWideText(detailBuffer, L" total="); AppendUnsignedText(detailBuffer, totalEntriesPointer != nullptr ? *totalEntriesPointer : 0); })
        APIMON_SIMPLE_ULONG_STATUS_HOOK(HookedNetServerEnum, g_netServerEnumOriginal, ks::winapi_monitor::EventCategory::Network, L"Netapi32", L"NetServerEnum",
            (LPCWSTR serverNamePointer, DWORD levelValue, LPBYTE* bufferPointer, DWORD preferredMaxLength, LPDWORD entriesReadPointer, LPDWORD totalEntriesPointer, DWORD serverType, LPCWSTR domainPointer, LPDWORD resumeHandlePointer),
            (serverNamePointer, levelValue, bufferPointer, preferredMaxLength, entriesReadPointer, totalEntriesPointer, serverType, domainPointer, resumeHandlePointer),
            { AppendWideText(detailBuffer, L"server="); AppendWideText(detailBuffer, serverNamePointer); AppendWideText(detailBuffer, L" domain="); AppendWideText(detailBuffer, domainPointer); AppendWideText(detailBuffer, L" type="); AppendHexText(detailBuffer, serverType); AppendWideText(detailBuffer, L" read="); AppendUnsignedText(detailBuffer, entriesReadPointer != nullptr ? *entriesReadPointer : 0); AppendWideText(detailBuffer, L" total="); AppendUnsignedText(detailBuffer, totalEntriesPointer != nullptr ? *totalEntriesPointer : 0); })
        APIMON_SIMPLE_ULONG_STATUS_HOOK(HookedNetWkstaGetInfo, g_netWkstaGetInfoOriginal, ks::winapi_monitor::EventCategory::Network, L"Netapi32", L"NetWkstaGetInfo",
            (LPWSTR serverNamePointer, DWORD levelValue, LPBYTE* bufferPointer), (serverNamePointer, levelValue, bufferPointer),
            { AppendWideText(detailBuffer, L"server="); AppendWideText(detailBuffer, serverNamePointer); AppendWideText(detailBuffer, L" level="); AppendUnsignedText(detailBuffer, levelValue); AppendWideText(detailBuffer, L" buffer="); AppendHexText(detailBuffer, bufferPointer != nullptr ? reinterpret_cast<std::uint64_t>(*bufferPointer) : 0); })
        APIMON_SIMPLE_ULONG_STATUS_HOOK(HookedNetApiBufferFree, g_netApiBufferFreeOriginal, ks::winapi_monitor::EventCategory::Network, L"Netapi32", L"NetApiBufferFree",
            (LPVOID bufferPointer), (bufferPointer),
            { AppendWideText(detailBuffer, L"buffer="); AppendHexText(detailBuffer, reinterpret_cast<std::uint64_t>(bufferPointer)); })

        APIMON_SIMPLE_ULONG_STATUS_HOOK(HookedGetExtendedTcpTable, g_getExtendedTcpTableOriginal, ks::winapi_monitor::EventCategory::Network, L"Iphlpapi", L"GetExtendedTcpTable",
            (PVOID tcpTablePointer, PDWORD sizePointer, BOOL orderValue, ULONG familyValue, ULONG tableClassValue, ULONG reservedValue),
            (tcpTablePointer, sizePointer, orderValue, familyValue, tableClassValue, reservedValue),
            { AppendWideText(detailBuffer, L"family="); AppendUnsignedText(detailBuffer, familyValue); AppendWideText(detailBuffer, L" class="); AppendUnsignedText(detailBuffer, tableClassValue); AppendWideText(detailBuffer, L" ordered="); AppendUnsignedText(detailBuffer, orderValue != FALSE ? 1ULL : 0ULL); AppendWideText(detailBuffer, L" size="); AppendUnsignedText(detailBuffer, sizePointer != nullptr ? *sizePointer : 0); })
        APIMON_SIMPLE_ULONG_STATUS_HOOK(HookedGetExtendedUdpTable, g_getExtendedUdpTableOriginal, ks::winapi_monitor::EventCategory::Network, L"Iphlpapi", L"GetExtendedUdpTable",
            (PVOID udpTablePointer, PDWORD sizePointer, BOOL orderValue, ULONG familyValue, ULONG tableClassValue, ULONG reservedValue),
            (udpTablePointer, sizePointer, orderValue, familyValue, tableClassValue, reservedValue),
            { AppendWideText(detailBuffer, L"family="); AppendUnsignedText(detailBuffer, familyValue); AppendWideText(detailBuffer, L" class="); AppendUnsignedText(detailBuffer, tableClassValue); AppendWideText(detailBuffer, L" ordered="); AppendUnsignedText(detailBuffer, orderValue != FALSE ? 1ULL : 0ULL); AppendWideText(detailBuffer, L" size="); AppendUnsignedText(detailBuffer, sizePointer != nullptr ? *sizePointer : 0); })
        APIMON_SIMPLE_ULONG_STATUS_HOOK(HookedGetTcpTable2, g_getTcpTable2Original, ks::winapi_monitor::EventCategory::Network, L"Iphlpapi", L"GetTcpTable2",
            (PVOID tcpTablePointer, PULONG sizePointer, BOOL orderValue), (tcpTablePointer, sizePointer, orderValue),
            { AppendWideText(detailBuffer, L"ordered="); AppendUnsignedText(detailBuffer, orderValue != FALSE ? 1ULL : 0ULL); AppendWideText(detailBuffer, L" size="); AppendUnsignedText(detailBuffer, sizePointer != nullptr ? *sizePointer : 0); })
        APIMON_SIMPLE_ULONG_STATUS_HOOK(HookedGetUdpTable, g_getUdpTableOriginal, ks::winapi_monitor::EventCategory::Network, L"Iphlpapi", L"GetUdpTable",
            (PVOID udpTablePointer, PDWORD sizePointer, BOOL orderValue), (udpTablePointer, sizePointer, orderValue),
            { AppendWideText(detailBuffer, L"ordered="); AppendUnsignedText(detailBuffer, orderValue != FALSE ? 1ULL : 0ULL); AppendWideText(detailBuffer, L" size="); AppendUnsignedText(detailBuffer, sizePointer != nullptr ? *sizePointer : 0); })
        APIMON_SIMPLE_ULONG_STATUS_HOOK(HookedGetAdaptersAddresses, g_getAdaptersAddressesOriginal, ks::winapi_monitor::EventCategory::Network, L"Iphlpapi", L"GetAdaptersAddresses",
            (ULONG familyValue, ULONG flagsValue, PVOID reservedPointer, PVOID addressesPointer, PULONG sizePointer),
            (familyValue, flagsValue, reservedPointer, addressesPointer, sizePointer),
            { AppendWideText(detailBuffer, L"family="); AppendUnsignedText(detailBuffer, familyValue); AppendWideText(detailBuffer, L" flags="); AppendHexText(detailBuffer, flagsValue); AppendWideText(detailBuffer, L" size="); AppendUnsignedText(detailBuffer, sizePointer != nullptr ? *sizePointer : 0); })
        APIMON_SIMPLE_ULONG_STATUS_HOOK(HookedGetNetworkParams, g_getNetworkParamsOriginal, ks::winapi_monitor::EventCategory::Network, L"Iphlpapi", L"GetNetworkParams",
            (PVOID fixedInfoPointer, PULONG sizePointer), (fixedInfoPointer, sizePointer),
            { AppendWideText(detailBuffer, L"size="); AppendUnsignedText(detailBuffer, sizePointer != nullptr ? *sizePointer : 0); })
        APIMON_SIMPLE_ULONG_STATUS_HOOK(HookedGetIpNetTable2, g_getIpNetTable2Original, ks::winapi_monitor::EventCategory::Network, L"Iphlpapi", L"GetIpNetTable2",
            (USHORT familyValue, PVOID* tablePointer), (familyValue, tablePointer),
            { AppendWideText(detailBuffer, L"family="); AppendUnsignedText(detailBuffer, familyValue); AppendWideText(detailBuffer, L" table="); AppendHexText(detailBuffer, tablePointer != nullptr ? reinterpret_cast<std::uint64_t>(*tablePointer) : 0); })
        APIMON_SIMPLE_ULONG_STATUS_HOOK(HookedGetIfTable2, g_getIfTable2Original, ks::winapi_monitor::EventCategory::Network, L"Iphlpapi", L"GetIfTable2",
            (PVOID* tablePointer), (tablePointer),
            { AppendWideText(detailBuffer, L"table="); AppendHexText(detailBuffer, tablePointer != nullptr ? reinterpret_cast<std::uint64_t>(*tablePointer) : 0); })
        APIMON_SIMPLE_VOID_HOOK(HookedFreeMibTable, g_freeMibTableOriginal, ks::winapi_monitor::EventCategory::Network, L"Iphlpapi", L"FreeMibTable",
            (PVOID memoryPointer), (memoryPointer),
            { AppendWideText(detailBuffer, L"memory="); AppendHexText(detailBuffer, reinterpret_cast<std::uint64_t>(memoryPointer)); })

        APIMON_SIMPLE_HANDLE_HOOK(HANDLE, HookedWTSOpenServerW, g_wtsOpenServerWOriginal, ks::winapi_monitor::EventCategory::Process, L"Wtsapi32", L"WTSOpenServerW",
            (LPWSTR serverNamePointer), (serverNamePointer),
            { AppendWideText(detailBuffer, L"server="); AppendWideText(detailBuffer, serverNamePointer); AppendWideText(detailBuffer, L" handle="); AppendHexText(detailBuffer, reinterpret_cast<std::uint64_t>(resultHandle)); })
        APIMON_SIMPLE_HANDLE_HOOK(HANDLE, HookedWTSOpenServerA, g_wtsOpenServerAOriginal, ks::winapi_monitor::EventCategory::Process, L"Wtsapi32", L"WTSOpenServerA",
            (LPSTR serverNamePointer), (serverNamePointer),
            { AppendWideText(detailBuffer, L"server="); AppendAnsiText(detailBuffer, serverNamePointer); AppendWideText(detailBuffer, L" handle="); AppendHexText(detailBuffer, reinterpret_cast<std::uint64_t>(resultHandle)); })
        APIMON_SIMPLE_VOID_HOOK(HookedWTSCloseServer, g_wtsCloseServerOriginal, ks::winapi_monitor::EventCategory::Process, L"Wtsapi32", L"WTSCloseServer",
            (HANDLE serverHandle), (serverHandle),
            { BuildSimpleHandleDetail(detailBuffer, L"server", serverHandle); })
        APIMON_SIMPLE_BOOL_HOOK(HookedWTSEnumerateSessionsW, g_wtsEnumerateSessionsWOriginal, ks::winapi_monitor::EventCategory::Process, L"Wtsapi32", L"WTSEnumerateSessionsW",
            (HANDLE serverHandle, DWORD reservedValue, DWORD versionValue, PVOID* sessionInfoPointer, DWORD* countPointer),
            (serverHandle, reservedValue, versionValue, sessionInfoPointer, countPointer),
            { AppendWideText(detailBuffer, L"server="); AppendHexText(detailBuffer, reinterpret_cast<std::uint64_t>(serverHandle)); AppendWideText(detailBuffer, L" version="); AppendUnsignedText(detailBuffer, versionValue); AppendWideText(detailBuffer, L" count="); AppendUnsignedText(detailBuffer, countPointer != nullptr ? *countPointer : 0); })
        APIMON_SIMPLE_BOOL_HOOK(HookedWTSEnumerateSessionsA, g_wtsEnumerateSessionsAOriginal, ks::winapi_monitor::EventCategory::Process, L"Wtsapi32", L"WTSEnumerateSessionsA",
            (HANDLE serverHandle, DWORD reservedValue, DWORD versionValue, PVOID* sessionInfoPointer, DWORD* countPointer),
            (serverHandle, reservedValue, versionValue, sessionInfoPointer, countPointer),
            { AppendWideText(detailBuffer, L"server="); AppendHexText(detailBuffer, reinterpret_cast<std::uint64_t>(serverHandle)); AppendWideText(detailBuffer, L" version="); AppendUnsignedText(detailBuffer, versionValue); AppendWideText(detailBuffer, L" count="); AppendUnsignedText(detailBuffer, countPointer != nullptr ? *countPointer : 0); })
        APIMON_SIMPLE_BOOL_HOOK(HookedWTSEnumerateProcessesW, g_wtsEnumerateProcessesWOriginal, ks::winapi_monitor::EventCategory::Process, L"Wtsapi32", L"WTSEnumerateProcessesW",
            (HANDLE serverHandle, DWORD reservedValue, DWORD versionValue, PVOID* processInfoPointer, DWORD* countPointer),
            (serverHandle, reservedValue, versionValue, processInfoPointer, countPointer),
            { AppendWideText(detailBuffer, L"server="); AppendHexText(detailBuffer, reinterpret_cast<std::uint64_t>(serverHandle)); AppendWideText(detailBuffer, L" version="); AppendUnsignedText(detailBuffer, versionValue); AppendWideText(detailBuffer, L" count="); AppendUnsignedText(detailBuffer, countPointer != nullptr ? *countPointer : 0); })
        APIMON_SIMPLE_BOOL_HOOK(HookedWTSEnumerateProcessesA, g_wtsEnumerateProcessesAOriginal, ks::winapi_monitor::EventCategory::Process, L"Wtsapi32", L"WTSEnumerateProcessesA",
            (HANDLE serverHandle, DWORD reservedValue, DWORD versionValue, PVOID* processInfoPointer, DWORD* countPointer),
            (serverHandle, reservedValue, versionValue, processInfoPointer, countPointer),
            { AppendWideText(detailBuffer, L"server="); AppendHexText(detailBuffer, reinterpret_cast<std::uint64_t>(serverHandle)); AppendWideText(detailBuffer, L" version="); AppendUnsignedText(detailBuffer, versionValue); AppendWideText(detailBuffer, L" count="); AppendUnsignedText(detailBuffer, countPointer != nullptr ? *countPointer : 0); })
        APIMON_SIMPLE_BOOL_HOOK(HookedWTSQuerySessionInformationW, g_wtsQuerySessionInformationWOriginal, ks::winapi_monitor::EventCategory::Process, L"Wtsapi32", L"WTSQuerySessionInformationW",
            (HANDLE serverHandle, DWORD sessionId, int infoClass, LPWSTR* bufferPointer, DWORD* bytesReturnedPointer),
            (serverHandle, sessionId, infoClass, bufferPointer, bytesReturnedPointer),
            { AppendWideText(detailBuffer, L"server="); AppendHexText(detailBuffer, reinterpret_cast<std::uint64_t>(serverHandle)); AppendWideText(detailBuffer, L" session="); AppendUnsignedText(detailBuffer, sessionId); AppendWideText(detailBuffer, L" class="); AppendUnsignedText(detailBuffer, static_cast<unsigned long long>(infoClass < 0 ? 0 : infoClass)); AppendWideText(detailBuffer, L" bytes="); AppendUnsignedText(detailBuffer, bytesReturnedPointer != nullptr ? *bytesReturnedPointer : 0); })
        APIMON_SIMPLE_BOOL_HOOK(HookedWTSQuerySessionInformationA, g_wtsQuerySessionInformationAOriginal, ks::winapi_monitor::EventCategory::Process, L"Wtsapi32", L"WTSQuerySessionInformationA",
            (HANDLE serverHandle, DWORD sessionId, int infoClass, LPSTR* bufferPointer, DWORD* bytesReturnedPointer),
            (serverHandle, sessionId, infoClass, bufferPointer, bytesReturnedPointer),
            { AppendWideText(detailBuffer, L"server="); AppendHexText(detailBuffer, reinterpret_cast<std::uint64_t>(serverHandle)); AppendWideText(detailBuffer, L" session="); AppendUnsignedText(detailBuffer, sessionId); AppendWideText(detailBuffer, L" class="); AppendUnsignedText(detailBuffer, static_cast<unsigned long long>(infoClass < 0 ? 0 : infoClass)); AppendWideText(detailBuffer, L" bytes="); AppendUnsignedText(detailBuffer, bytesReturnedPointer != nullptr ? *bytesReturnedPointer : 0); })
        APIMON_SIMPLE_VOID_HOOK(HookedWTSFreeMemory, g_wtsFreeMemoryOriginal, ks::winapi_monitor::EventCategory::Process, L"Wtsapi32", L"WTSFreeMemory",
            (PVOID memoryPointer), (memoryPointer),
            { AppendWideText(detailBuffer, L"memory="); AppendHexText(detailBuffer, reinterpret_cast<std::uint64_t>(memoryPointer)); })

        APIMON_SIMPLE_HANDLE_HOOK(HANDLE, HookedCreateJobObjectW, g_createJobObjectWOriginal, ks::winapi_monitor::EventCategory::Process, L"KernelBase", L"CreateJobObjectW",
            (LPSECURITY_ATTRIBUTES securityAttributesPointer, LPCWSTR namePointer), (securityAttributesPointer, namePointer),
            { AppendWideText(detailBuffer, L"name="); AppendWideText(detailBuffer, namePointer); AppendWideText(detailBuffer, L" job="); AppendHexText(detailBuffer, reinterpret_cast<std::uint64_t>(resultHandle)); })
        APIMON_SIMPLE_HANDLE_HOOK(HANDLE, HookedCreateJobObjectA, g_createJobObjectAOriginal, ks::winapi_monitor::EventCategory::Process, L"KernelBase", L"CreateJobObjectA",
            (LPSECURITY_ATTRIBUTES securityAttributesPointer, LPCSTR namePointer), (securityAttributesPointer, namePointer),
            { AppendWideText(detailBuffer, L"name="); AppendAnsiText(detailBuffer, namePointer); AppendWideText(detailBuffer, L" job="); AppendHexText(detailBuffer, reinterpret_cast<std::uint64_t>(resultHandle)); })
        APIMON_SIMPLE_HANDLE_HOOK(HANDLE, HookedOpenJobObjectW, g_openJobObjectWOriginal, ks::winapi_monitor::EventCategory::Process, L"KernelBase", L"OpenJobObjectW",
            (DWORD desiredAccess, BOOL inheritHandle, LPCWSTR namePointer), (desiredAccess, inheritHandle, namePointer),
            { AppendWideText(detailBuffer, L"name="); AppendWideText(detailBuffer, namePointer); AppendWideText(detailBuffer, L" access="); AppendHexText(detailBuffer, desiredAccess); AppendWideText(detailBuffer, L" inherit="); AppendUnsignedText(detailBuffer, inheritHandle != FALSE ? 1ULL : 0ULL); AppendWideText(detailBuffer, L" job="); AppendHexText(detailBuffer, reinterpret_cast<std::uint64_t>(resultHandle)); })
        APIMON_SIMPLE_HANDLE_HOOK(HANDLE, HookedOpenJobObjectA, g_openJobObjectAOriginal, ks::winapi_monitor::EventCategory::Process, L"KernelBase", L"OpenJobObjectA",
            (DWORD desiredAccess, BOOL inheritHandle, LPCSTR namePointer), (desiredAccess, inheritHandle, namePointer),
            { AppendWideText(detailBuffer, L"name="); AppendAnsiText(detailBuffer, namePointer); AppendWideText(detailBuffer, L" access="); AppendHexText(detailBuffer, desiredAccess); AppendWideText(detailBuffer, L" inherit="); AppendUnsignedText(detailBuffer, inheritHandle != FALSE ? 1ULL : 0ULL); AppendWideText(detailBuffer, L" job="); AppendHexText(detailBuffer, reinterpret_cast<std::uint64_t>(resultHandle)); })
        APIMON_SIMPLE_BOOL_HOOK(HookedAssignProcessToJobObject, g_assignProcessToJobObjectOriginal, ks::winapi_monitor::EventCategory::Process, L"KernelBase", L"AssignProcessToJobObject",
            (HANDLE jobHandle, HANDLE processHandle), (jobHandle, processHandle),
            { AppendWideText(detailBuffer, L"job="); AppendHexText(detailBuffer, reinterpret_cast<std::uint64_t>(jobHandle)); AppendWideText(detailBuffer, L" process="); AppendHexText(detailBuffer, reinterpret_cast<std::uint64_t>(processHandle)); })
        APIMON_SIMPLE_BOOL_HOOK(HookedTerminateJobObject, g_terminateJobObjectOriginal, ks::winapi_monitor::EventCategory::Process, L"KernelBase", L"TerminateJobObject",
            (HANDLE jobHandle, UINT exitCode), (jobHandle, exitCode),
            { AppendWideText(detailBuffer, L"job="); AppendHexText(detailBuffer, reinterpret_cast<std::uint64_t>(jobHandle)); AppendWideText(detailBuffer, L" exit="); AppendUnsignedText(detailBuffer, exitCode); })
        APIMON_SIMPLE_BOOL_HOOK(HookedSetInformationJobObject, g_setInformationJobObjectOriginal, ks::winapi_monitor::EventCategory::Process, L"KernelBase", L"SetInformationJobObject",
            (HANDLE jobHandle, JOBOBJECTINFOCLASS infoClass, LPVOID jobObjectInformationPointer, DWORD jobObjectInformationLength),
            (jobHandle, infoClass, jobObjectInformationPointer, jobObjectInformationLength),
            { AppendWideText(detailBuffer, L"job="); AppendHexText(detailBuffer, reinterpret_cast<std::uint64_t>(jobHandle)); AppendWideText(detailBuffer, L" class="); AppendUnsignedText(detailBuffer, static_cast<unsigned long long>(infoClass)); AppendWideText(detailBuffer, L" length="); AppendUnsignedText(detailBuffer, jobObjectInformationLength); })
        APIMON_SIMPLE_BOOL_HOOK(HookedQueryInformationJobObject, g_queryInformationJobObjectOriginal, ks::winapi_monitor::EventCategory::Process, L"KernelBase", L"QueryInformationJobObject",
            (HANDLE jobHandle, JOBOBJECTINFOCLASS infoClass, LPVOID jobObjectInformationPointer, DWORD jobObjectInformationLength, LPDWORD returnLengthPointer),
            (jobHandle, infoClass, jobObjectInformationPointer, jobObjectInformationLength, returnLengthPointer),
            { AppendWideText(detailBuffer, L"job="); AppendHexText(detailBuffer, reinterpret_cast<std::uint64_t>(jobHandle)); AppendWideText(detailBuffer, L" class="); AppendUnsignedText(detailBuffer, static_cast<unsigned long long>(infoClass)); AppendWideText(detailBuffer, L" length="); AppendUnsignedText(detailBuffer, jobObjectInformationLength); AppendWideText(detailBuffer, L" return="); AppendUnsignedText(detailBuffer, returnLengthPointer != nullptr ? *returnLengthPointer : 0); })

        APIMON_SIMPLE_SECURITY_STATUS_HOOK(HookedAcquireCredentialsHandleW, g_acquireCredentialsHandleWOriginal, ks::winapi_monitor::EventCategory::Process, L"Secur32", L"AcquireCredentialsHandleW",
            (LPWSTR principalPointer, LPWSTR packagePointer, ULONG credentialUse, PVOID logonIdPointer, PVOID authDataPointer, PVOID getKeyFnPointer, PVOID getKeyArgumentPointer, PVOID credentialHandlePointer, PVOID expiryPointer),
            (principalPointer, packagePointer, credentialUse, logonIdPointer, authDataPointer, getKeyFnPointer, getKeyArgumentPointer, credentialHandlePointer, expiryPointer),
            { AppendWideText(detailBuffer, L"principal="); AppendWideText(detailBuffer, principalPointer); AppendWideText(detailBuffer, L" package="); AppendWideText(detailBuffer, packagePointer); AppendWideText(detailBuffer, L" use="); AppendHexText(detailBuffer, credentialUse); AppendWideText(detailBuffer, L" cred="); AppendHexText(detailBuffer, reinterpret_cast<std::uint64_t>(credentialHandlePointer)); })
        APIMON_SIMPLE_SECURITY_STATUS_HOOK(HookedAcquireCredentialsHandleA, g_acquireCredentialsHandleAOriginal, ks::winapi_monitor::EventCategory::Process, L"Secur32", L"AcquireCredentialsHandleA",
            (LPSTR principalPointer, LPSTR packagePointer, ULONG credentialUse, PVOID logonIdPointer, PVOID authDataPointer, PVOID getKeyFnPointer, PVOID getKeyArgumentPointer, PVOID credentialHandlePointer, PVOID expiryPointer),
            (principalPointer, packagePointer, credentialUse, logonIdPointer, authDataPointer, getKeyFnPointer, getKeyArgumentPointer, credentialHandlePointer, expiryPointer),
            { AppendWideText(detailBuffer, L"principal="); AppendAnsiText(detailBuffer, principalPointer); AppendWideText(detailBuffer, L" package="); AppendAnsiText(detailBuffer, packagePointer); AppendWideText(detailBuffer, L" use="); AppendHexText(detailBuffer, credentialUse); AppendWideText(detailBuffer, L" cred="); AppendHexText(detailBuffer, reinterpret_cast<std::uint64_t>(credentialHandlePointer)); })
        APIMON_SIMPLE_SECURITY_STATUS_HOOK(HookedInitializeSecurityContextW, g_initializeSecurityContextWOriginal, ks::winapi_monitor::EventCategory::Process, L"Secur32", L"InitializeSecurityContextW",
            (PVOID credentialHandlePointer, PVOID oldContextPointer, LPWSTR targetNamePointer, ULONG requestFlags, ULONG reserved1, ULONG targetDataRep, PVOID inputPointer, ULONG reserved2, PVOID newContextPointer, PVOID outputPointer, PULONG contextAttributesPointer, PVOID expiryPointer),
            (credentialHandlePointer, oldContextPointer, targetNamePointer, requestFlags, reserved1, targetDataRep, inputPointer, reserved2, newContextPointer, outputPointer, contextAttributesPointer, expiryPointer),
            { AppendWideText(detailBuffer, L"target="); AppendWideText(detailBuffer, targetNamePointer); AppendWideText(detailBuffer, L" flags="); AppendHexText(detailBuffer, requestFlags); AppendWideText(detailBuffer, L" attrs="); AppendHexText(detailBuffer, contextAttributesPointer != nullptr ? *contextAttributesPointer : 0); })
        APIMON_SIMPLE_SECURITY_STATUS_HOOK(HookedInitializeSecurityContextA, g_initializeSecurityContextAOriginal, ks::winapi_monitor::EventCategory::Process, L"Secur32", L"InitializeSecurityContextA",
            (PVOID credentialHandlePointer, PVOID oldContextPointer, LPSTR targetNamePointer, ULONG requestFlags, ULONG reserved1, ULONG targetDataRep, PVOID inputPointer, ULONG reserved2, PVOID newContextPointer, PVOID outputPointer, PULONG contextAttributesPointer, PVOID expiryPointer),
            (credentialHandlePointer, oldContextPointer, targetNamePointer, requestFlags, reserved1, targetDataRep, inputPointer, reserved2, newContextPointer, outputPointer, contextAttributesPointer, expiryPointer),
            { AppendWideText(detailBuffer, L"target="); AppendAnsiText(detailBuffer, targetNamePointer); AppendWideText(detailBuffer, L" flags="); AppendHexText(detailBuffer, requestFlags); AppendWideText(detailBuffer, L" attrs="); AppendHexText(detailBuffer, contextAttributesPointer != nullptr ? *contextAttributesPointer : 0); })
        APIMON_SIMPLE_SECURITY_STATUS_HOOK(HookedAcceptSecurityContext, g_acceptSecurityContextOriginal, ks::winapi_monitor::EventCategory::Process, L"Secur32", L"AcceptSecurityContext",
            (PVOID credentialHandlePointer, PVOID contextHandlePointer, PVOID inputPointer, ULONG requestFlags, ULONG targetDataRep, PVOID newContextPointer, PVOID outputPointer, PULONG contextAttributesPointer, PVOID expiryPointer),
            (credentialHandlePointer, contextHandlePointer, inputPointer, requestFlags, targetDataRep, newContextPointer, outputPointer, contextAttributesPointer, expiryPointer),
            { AppendWideText(detailBuffer, L"cred="); AppendHexText(detailBuffer, reinterpret_cast<std::uint64_t>(credentialHandlePointer)); AppendWideText(detailBuffer, L" ctx="); AppendHexText(detailBuffer, reinterpret_cast<std::uint64_t>(contextHandlePointer)); AppendWideText(detailBuffer, L" flags="); AppendHexText(detailBuffer, requestFlags); AppendWideText(detailBuffer, L" attrs="); AppendHexText(detailBuffer, contextAttributesPointer != nullptr ? *contextAttributesPointer : 0); })
        APIMON_SIMPLE_SECURITY_STATUS_HOOK(HookedEncryptMessage, g_encryptMessageOriginal, ks::winapi_monitor::EventCategory::Process, L"Secur32", L"EncryptMessage",
            (PVOID contextHandlePointer, ULONG qualityOfProtection, PVOID messagePointer, ULONG sequenceNumber),
            (contextHandlePointer, qualityOfProtection, messagePointer, sequenceNumber),
            { AppendWideText(detailBuffer, L"ctx="); AppendHexText(detailBuffer, reinterpret_cast<std::uint64_t>(contextHandlePointer)); AppendWideText(detailBuffer, L" qop="); AppendHexText(detailBuffer, qualityOfProtection); AppendWideText(detailBuffer, L" seq="); AppendUnsignedText(detailBuffer, sequenceNumber); })
        APIMON_SIMPLE_SECURITY_STATUS_HOOK(HookedDecryptMessage, g_decryptMessageOriginal, ks::winapi_monitor::EventCategory::Process, L"Secur32", L"DecryptMessage",
            (PVOID contextHandlePointer, PVOID messagePointer, ULONG sequenceNumber, PULONG qualityOfProtectionPointer),
            (contextHandlePointer, messagePointer, sequenceNumber, qualityOfProtectionPointer),
            { AppendWideText(detailBuffer, L"ctx="); AppendHexText(detailBuffer, reinterpret_cast<std::uint64_t>(contextHandlePointer)); AppendWideText(detailBuffer, L" seq="); AppendUnsignedText(detailBuffer, sequenceNumber); AppendWideText(detailBuffer, L" qop="); AppendHexText(detailBuffer, qualityOfProtectionPointer != nullptr ? *qualityOfProtectionPointer : 0); })
        APIMON_SIMPLE_SECURITY_STATUS_HOOK(HookedDeleteSecurityContext, g_deleteSecurityContextOriginal, ks::winapi_monitor::EventCategory::Process, L"Secur32", L"DeleteSecurityContext",
            (PVOID contextHandlePointer), (contextHandlePointer),
            { AppendWideText(detailBuffer, L"ctx="); AppendHexText(detailBuffer, reinterpret_cast<std::uint64_t>(contextHandlePointer)); })
        APIMON_SIMPLE_SECURITY_STATUS_HOOK(HookedFreeCredentialsHandle, g_freeCredentialsHandleOriginal, ks::winapi_monitor::EventCategory::Process, L"Secur32", L"FreeCredentialsHandle",
            (PVOID credentialHandlePointer), (credentialHandlePointer),
            { AppendWideText(detailBuffer, L"cred="); AppendHexText(detailBuffer, reinterpret_cast<std::uint64_t>(credentialHandlePointer)); })

        // HookedSixthBatchEnumerationPathsNetwork 作用：
        // - 输入：Toolhelp 枚举、进程镜像查询、路径解析/临时文件、管道/邮槽、Winsock 扩展和 HTTP 查询参数；
        // - 处理：补齐进程/线程/堆侦察、路径落点、IPC 创建、socket 配置和 HTTP 元数据访问面；
        // - 返回：保持各 API 原始返回值，必要时恢复 LastError/WSAError。
        APIMON_SIMPLE_BOOL_HOOK(HookedProcess32FirstW, g_process32FirstWOriginal, ks::winapi_monitor::EventCategory::Process, L"Kernel32", L"Process32FirstW",
            (HANDLE snapshotHandle, PROCESSENTRY32W* entryPointer), (snapshotHandle, entryPointer),
            { AppendWideText(detailBuffer, L"snapshot="); AppendHexText(detailBuffer, reinterpret_cast<std::uint64_t>(snapshotHandle)); AppendWideText(detailBuffer, L" pid="); AppendUnsignedText(detailBuffer, entryPointer != nullptr ? entryPointer->th32ProcessID : 0); AppendWideText(detailBuffer, L" exe="); AppendWideText(detailBuffer, entryPointer != nullptr ? entryPointer->szExeFile : nullptr); })
        APIMON_SIMPLE_BOOL_HOOK(HookedProcess32FirstA, g_process32FirstAOriginal, ks::winapi_monitor::EventCategory::Process, L"Kernel32", L"Process32First",
            (HANDLE snapshotHandle, tagPROCESSENTRY32* entryPointer), (snapshotHandle, entryPointer),
            { AppendWideText(detailBuffer, L"snapshot="); AppendHexText(detailBuffer, reinterpret_cast<std::uint64_t>(snapshotHandle)); AppendWideText(detailBuffer, L" pid="); AppendUnsignedText(detailBuffer, entryPointer != nullptr ? entryPointer->th32ProcessID : 0); AppendWideText(detailBuffer, L" exe="); AppendAnsiText(detailBuffer, entryPointer != nullptr ? entryPointer->szExeFile : nullptr); })
        APIMON_SIMPLE_BOOL_HOOK(HookedProcess32NextW, g_process32NextWOriginal, ks::winapi_monitor::EventCategory::Process, L"Kernel32", L"Process32NextW",
            (HANDLE snapshotHandle, PROCESSENTRY32W* entryPointer), (snapshotHandle, entryPointer),
            { AppendWideText(detailBuffer, L"snapshot="); AppendHexText(detailBuffer, reinterpret_cast<std::uint64_t>(snapshotHandle)); AppendWideText(detailBuffer, L" pid="); AppendUnsignedText(detailBuffer, entryPointer != nullptr ? entryPointer->th32ProcessID : 0); AppendWideText(detailBuffer, L" exe="); AppendWideText(detailBuffer, entryPointer != nullptr ? entryPointer->szExeFile : nullptr); })
        APIMON_SIMPLE_BOOL_HOOK(HookedProcess32NextA, g_process32NextAOriginal, ks::winapi_monitor::EventCategory::Process, L"Kernel32", L"Process32Next",
            (HANDLE snapshotHandle, tagPROCESSENTRY32* entryPointer), (snapshotHandle, entryPointer),
            { AppendWideText(detailBuffer, L"snapshot="); AppendHexText(detailBuffer, reinterpret_cast<std::uint64_t>(snapshotHandle)); AppendWideText(detailBuffer, L" pid="); AppendUnsignedText(detailBuffer, entryPointer != nullptr ? entryPointer->th32ProcessID : 0); AppendWideText(detailBuffer, L" exe="); AppendAnsiText(detailBuffer, entryPointer != nullptr ? entryPointer->szExeFile : nullptr); })
        APIMON_SIMPLE_BOOL_HOOK(HookedThread32First, g_thread32FirstOriginal, ks::winapi_monitor::EventCategory::Process, L"Kernel32", L"Thread32First",
            (HANDLE snapshotHandle, LPTHREADENTRY32 entryPointer), (snapshotHandle, entryPointer),
            { AppendWideText(detailBuffer, L"snapshot="); AppendHexText(detailBuffer, reinterpret_cast<std::uint64_t>(snapshotHandle)); AppendWideText(detailBuffer, L" tid="); AppendUnsignedText(detailBuffer, entryPointer != nullptr ? entryPointer->th32ThreadID : 0); AppendWideText(detailBuffer, L" ownerPid="); AppendUnsignedText(detailBuffer, entryPointer != nullptr ? entryPointer->th32OwnerProcessID : 0); })
        APIMON_SIMPLE_BOOL_HOOK(HookedThread32Next, g_thread32NextOriginal, ks::winapi_monitor::EventCategory::Process, L"Kernel32", L"Thread32Next",
            (HANDLE snapshotHandle, LPTHREADENTRY32 entryPointer), (snapshotHandle, entryPointer),
            { AppendWideText(detailBuffer, L"snapshot="); AppendHexText(detailBuffer, reinterpret_cast<std::uint64_t>(snapshotHandle)); AppendWideText(detailBuffer, L" tid="); AppendUnsignedText(detailBuffer, entryPointer != nullptr ? entryPointer->th32ThreadID : 0); AppendWideText(detailBuffer, L" ownerPid="); AppendUnsignedText(detailBuffer, entryPointer != nullptr ? entryPointer->th32OwnerProcessID : 0); })
        APIMON_SIMPLE_BOOL_HOOK(HookedHeap32ListFirst, g_heap32ListFirstOriginal, ks::winapi_monitor::EventCategory::Process, L"Kernel32", L"Heap32ListFirst",
            (HANDLE snapshotHandle, LPHEAPLIST32 heapListPointer), (snapshotHandle, heapListPointer),
            { AppendWideText(detailBuffer, L"snapshot="); AppendHexText(detailBuffer, reinterpret_cast<std::uint64_t>(snapshotHandle)); AppendWideText(detailBuffer, L" pid="); AppendUnsignedText(detailBuffer, heapListPointer != nullptr ? heapListPointer->th32ProcessID : 0); AppendWideText(detailBuffer, L" heap="); AppendHexText(detailBuffer, heapListPointer != nullptr ? static_cast<std::uint64_t>(heapListPointer->th32HeapID) : 0); })
        APIMON_SIMPLE_BOOL_HOOK(HookedHeap32ListNext, g_heap32ListNextOriginal, ks::winapi_monitor::EventCategory::Process, L"Kernel32", L"Heap32ListNext",
            (HANDLE snapshotHandle, LPHEAPLIST32 heapListPointer), (snapshotHandle, heapListPointer),
            { AppendWideText(detailBuffer, L"snapshot="); AppendHexText(detailBuffer, reinterpret_cast<std::uint64_t>(snapshotHandle)); AppendWideText(detailBuffer, L" pid="); AppendUnsignedText(detailBuffer, heapListPointer != nullptr ? heapListPointer->th32ProcessID : 0); AppendWideText(detailBuffer, L" heap="); AppendHexText(detailBuffer, heapListPointer != nullptr ? static_cast<std::uint64_t>(heapListPointer->th32HeapID) : 0); })
        APIMON_SIMPLE_BOOL_HOOK(HookedHeap32First, g_heap32FirstOriginal, ks::winapi_monitor::EventCategory::Process, L"Kernel32", L"Heap32First",
            (LPHEAPENTRY32 heapEntryPointer, DWORD processId, ULONG_PTR heapId), (heapEntryPointer, processId, heapId),
            { AppendWideText(detailBuffer, L"pid="); AppendUnsignedText(detailBuffer, processId); AppendWideText(detailBuffer, L" heap="); AppendHexText(detailBuffer, heapId); AppendWideText(detailBuffer, L" block="); AppendHexText(detailBuffer, heapEntryPointer != nullptr ? heapEntryPointer->dwAddress : 0); AppendWideText(detailBuffer, L" size="); AppendUnsignedText(detailBuffer, heapEntryPointer != nullptr ? heapEntryPointer->dwBlockSize : 0); })
        APIMON_SIMPLE_BOOL_HOOK(HookedHeap32Next, g_heap32NextOriginal, ks::winapi_monitor::EventCategory::Process, L"Kernel32", L"Heap32Next",
            (LPHEAPENTRY32 heapEntryPointer), (heapEntryPointer),
            { AppendWideText(detailBuffer, L"pid="); AppendUnsignedText(detailBuffer, heapEntryPointer != nullptr ? heapEntryPointer->th32ProcessID : 0); AppendWideText(detailBuffer, L" heap="); AppendHexText(detailBuffer, heapEntryPointer != nullptr ? static_cast<std::uint64_t>(heapEntryPointer->th32HeapID) : 0); AppendWideText(detailBuffer, L" block="); AppendHexText(detailBuffer, heapEntryPointer != nullptr ? heapEntryPointer->dwAddress : 0); })
        APIMON_SIMPLE_BOOL_HOOK(HookedQueryFullProcessImageNameW, g_queryFullProcessImageNameWOriginal, ks::winapi_monitor::EventCategory::Process, L"KernelBase", L"QueryFullProcessImageNameW",
            (HANDLE processHandle, DWORD flagsValue, LPWSTR imageNamePointer, PDWORD sizePointer), (processHandle, flagsValue, imageNamePointer, sizePointer),
            { AppendWideText(detailBuffer, L"process="); AppendHexText(detailBuffer, reinterpret_cast<std::uint64_t>(processHandle)); AppendWideText(detailBuffer, L" flags="); AppendHexText(detailBuffer, flagsValue); AppendWideText(detailBuffer, L" size="); AppendUnsignedText(detailBuffer, sizePointer != nullptr ? *sizePointer : 0); AppendWideText(detailBuffer, L" image="); if (resultValue != FALSE) { AppendWideText(detailBuffer, imageNamePointer, sizePointer != nullptr ? *sizePointer : static_cast<std::size_t>(-1)); } })
        APIMON_SIMPLE_BOOL_HOOK(HookedQueryFullProcessImageNameA, g_queryFullProcessImageNameAOriginal, ks::winapi_monitor::EventCategory::Process, L"KernelBase", L"QueryFullProcessImageNameA",
            (HANDLE processHandle, DWORD flagsValue, LPSTR imageNamePointer, PDWORD sizePointer), (processHandle, flagsValue, imageNamePointer, sizePointer),
            { AppendWideText(detailBuffer, L"process="); AppendHexText(detailBuffer, reinterpret_cast<std::uint64_t>(processHandle)); AppendWideText(detailBuffer, L" flags="); AppendHexText(detailBuffer, flagsValue); AppendWideText(detailBuffer, L" size="); AppendUnsignedText(detailBuffer, sizePointer != nullptr ? *sizePointer : 0); AppendWideText(detailBuffer, L" image="); if (resultValue != FALSE) { AppendAnsiText(detailBuffer, imageNamePointer, sizePointer != nullptr ? *sizePointer : static_cast<std::size_t>(-1)); } })
        APIMON_SIMPLE_DWORD_NONZERO_HOOK(HookedGetProcessImageFileNameW, g_getProcessImageFileNameWOriginal, ks::winapi_monitor::EventCategory::Process, L"Psapi", L"GetProcessImageFileNameW",
            (HANDLE processHandle, LPWSTR imageNamePointer, DWORD sizeValue), (processHandle, imageNamePointer, sizeValue),
            { AppendWideText(detailBuffer, L"process="); AppendHexText(detailBuffer, reinterpret_cast<std::uint64_t>(processHandle)); AppendWideText(detailBuffer, L" size="); AppendUnsignedText(detailBuffer, sizeValue); AppendWideText(detailBuffer, L" image="); if (resultValue != 0) { AppendWideText(detailBuffer, imageNamePointer, resultValue); } })
        APIMON_SIMPLE_DWORD_NONZERO_HOOK(HookedGetProcessImageFileNameA, g_getProcessImageFileNameAOriginal, ks::winapi_monitor::EventCategory::Process, L"Psapi", L"GetProcessImageFileNameA",
            (HANDLE processHandle, LPSTR imageNamePointer, DWORD sizeValue), (processHandle, imageNamePointer, sizeValue),
            { AppendWideText(detailBuffer, L"process="); AppendHexText(detailBuffer, reinterpret_cast<std::uint64_t>(processHandle)); AppendWideText(detailBuffer, L" size="); AppendUnsignedText(detailBuffer, sizeValue); AppendWideText(detailBuffer, L" image="); if (resultValue != 0) { AppendAnsiText(detailBuffer, imageNamePointer, resultValue); } })
        APIMON_SIMPLE_DWORD_NONZERO_HOOK(HookedGetProcessId, g_getProcessIdOriginal, ks::winapi_monitor::EventCategory::Process, L"KernelBase", L"GetProcessId",
            (HANDLE processHandle), (processHandle),
            { AppendWideText(detailBuffer, L"process="); AppendHexText(detailBuffer, reinterpret_cast<std::uint64_t>(processHandle)); AppendWideText(detailBuffer, L" pid="); AppendUnsignedText(detailBuffer, resultValue); })
        APIMON_SIMPLE_DWORD_NONZERO_HOOK(HookedGetThreadId, g_getThreadIdOriginal, ks::winapi_monitor::EventCategory::Process, L"KernelBase", L"GetThreadId",
            (HANDLE threadHandle), (threadHandle),
            { AppendWideText(detailBuffer, L"thread="); AppendHexText(detailBuffer, reinterpret_cast<std::uint64_t>(threadHandle)); AppendWideText(detailBuffer, L" tid="); AppendUnsignedText(detailBuffer, resultValue); })
        APIMON_SIMPLE_BOOL_HOOK(HookedIsWow64Process, g_isWow64ProcessOriginal, ks::winapi_monitor::EventCategory::Process, L"KernelBase", L"IsWow64Process",
            (HANDLE processHandle, PBOOL wow64ProcessPointer), (processHandle, wow64ProcessPointer),
            { AppendWideText(detailBuffer, L"process="); AppendHexText(detailBuffer, reinterpret_cast<std::uint64_t>(processHandle)); AppendWideText(detailBuffer, L" wow64="); AppendUnsignedText(detailBuffer, wow64ProcessPointer != nullptr && *wow64ProcessPointer != FALSE ? 1ULL : 0ULL); })
        APIMON_SIMPLE_BOOL_HOOK(HookedIsWow64Process2, g_isWow64Process2Original, ks::winapi_monitor::EventCategory::Process, L"KernelBase", L"IsWow64Process2",
            (HANDLE processHandle, USHORT* processMachinePointer, USHORT* nativeMachinePointer), (processHandle, processMachinePointer, nativeMachinePointer),
            { AppendWideText(detailBuffer, L"process="); AppendHexText(detailBuffer, reinterpret_cast<std::uint64_t>(processHandle)); AppendWideText(detailBuffer, L" procMachine="); AppendHexText(detailBuffer, processMachinePointer != nullptr ? *processMachinePointer : 0); AppendWideText(detailBuffer, L" nativeMachine="); AppendHexText(detailBuffer, nativeMachinePointer != nullptr ? *nativeMachinePointer : 0); })
        APIMON_SIMPLE_BOOL_HOOK(HookedWow64DisableWow64FsRedirection, g_wow64DisableWow64FsRedirectionOriginal, ks::winapi_monitor::EventCategory::File, L"KernelBase", L"Wow64DisableWow64FsRedirection",
            (PVOID* oldValuePointer), (oldValuePointer),
            { AppendWideText(detailBuffer, L"old="); AppendHexText(detailBuffer, oldValuePointer != nullptr ? reinterpret_cast<std::uint64_t>(*oldValuePointer) : 0); })
        APIMON_SIMPLE_BOOL_HOOK(HookedWow64RevertWow64FsRedirection, g_wow64RevertWow64FsRedirectionOriginal, ks::winapi_monitor::EventCategory::File, L"KernelBase", L"Wow64RevertWow64FsRedirection",
            (PVOID oldValuePointer), (oldValuePointer),
            { AppendWideText(detailBuffer, L"old="); AppendHexText(detailBuffer, reinterpret_cast<std::uint64_t>(oldValuePointer)); })

        APIMON_SIMPLE_DWORD_NONZERO_HOOK(HookedGetTempPathW, g_getTempPathWOriginal, ks::winapi_monitor::EventCategory::File, L"KernelBase", L"GetTempPathW",
            (DWORD bufferLength, LPWSTR bufferPointer), (bufferLength, bufferPointer),
            { AppendWideText(detailBuffer, L"buffer="); AppendUnsignedText(detailBuffer, bufferLength); AppendWideText(detailBuffer, L" path="); if (resultValue != 0) { AppendWideText(detailBuffer, bufferPointer, resultValue); } })
        APIMON_SIMPLE_DWORD_NONZERO_HOOK(HookedGetTempPathA, g_getTempPathAOriginal, ks::winapi_monitor::EventCategory::File, L"KernelBase", L"GetTempPathA",
            (DWORD bufferLength, LPSTR bufferPointer), (bufferLength, bufferPointer),
            { AppendWideText(detailBuffer, L"buffer="); AppendUnsignedText(detailBuffer, bufferLength); AppendWideText(detailBuffer, L" path="); if (resultValue != 0) { AppendAnsiText(detailBuffer, bufferPointer, resultValue); } })
        APIMON_SIMPLE_UINT_NONZERO_HOOK(HookedGetTempFileNameW, g_getTempFileNameWOriginal, ks::winapi_monitor::EventCategory::File, L"KernelBase", L"GetTempFileNameW",
            (LPCWSTR pathNamePointer, LPCWSTR prefixStringPointer, UINT uniqueValue, LPWSTR tempFileNamePointer), (pathNamePointer, prefixStringPointer, uniqueValue, tempFileNamePointer),
            { AppendWideText(detailBuffer, L"path="); AppendWideText(detailBuffer, pathNamePointer); AppendWideText(detailBuffer, L" prefix="); AppendWideText(detailBuffer, prefixStringPointer); AppendWideText(detailBuffer, L" unique="); AppendUnsignedText(detailBuffer, uniqueValue); AppendWideText(detailBuffer, L" file="); AppendWideText(detailBuffer, tempFileNamePointer); })
        APIMON_SIMPLE_UINT_NONZERO_HOOK(HookedGetTempFileNameA, g_getTempFileNameAOriginal, ks::winapi_monitor::EventCategory::File, L"KernelBase", L"GetTempFileNameA",
            (LPCSTR pathNamePointer, LPCSTR prefixStringPointer, UINT uniqueValue, LPSTR tempFileNamePointer), (pathNamePointer, prefixStringPointer, uniqueValue, tempFileNamePointer),
            { AppendWideText(detailBuffer, L"path="); AppendAnsiText(detailBuffer, pathNamePointer); AppendWideText(detailBuffer, L" prefix="); AppendAnsiText(detailBuffer, prefixStringPointer); AppendWideText(detailBuffer, L" unique="); AppendUnsignedText(detailBuffer, uniqueValue); AppendWideText(detailBuffer, L" file="); AppendAnsiText(detailBuffer, tempFileNamePointer); })
        APIMON_SIMPLE_DWORD_NONZERO_HOOK(HookedGetFullPathNameW, g_getFullPathNameWOriginal, ks::winapi_monitor::EventCategory::File, L"KernelBase", L"GetFullPathNameW",
            (LPCWSTR fileNamePointer, DWORD bufferLength, LPWSTR bufferPointer, LPWSTR* filePartPointer), (fileNamePointer, bufferLength, bufferPointer, filePartPointer),
            { AppendWideText(detailBuffer, L"input="); AppendWideText(detailBuffer, fileNamePointer); AppendWideText(detailBuffer, L" buffer="); AppendUnsignedText(detailBuffer, bufferLength); AppendWideText(detailBuffer, L" full="); if (resultValue != 0 && resultValue < bufferLength) { AppendWideText(detailBuffer, bufferPointer, resultValue); } })
        APIMON_SIMPLE_DWORD_NONZERO_HOOK(HookedGetFullPathNameA, g_getFullPathNameAOriginal, ks::winapi_monitor::EventCategory::File, L"KernelBase", L"GetFullPathNameA",
            (LPCSTR fileNamePointer, DWORD bufferLength, LPSTR bufferPointer, LPSTR* filePartPointer), (fileNamePointer, bufferLength, bufferPointer, filePartPointer),
            { AppendWideText(detailBuffer, L"input="); AppendAnsiText(detailBuffer, fileNamePointer); AppendWideText(detailBuffer, L" buffer="); AppendUnsignedText(detailBuffer, bufferLength); AppendWideText(detailBuffer, L" full="); if (resultValue != 0 && resultValue < bufferLength) { AppendAnsiText(detailBuffer, bufferPointer, resultValue); } })
        APIMON_SIMPLE_DWORD_NONZERO_HOOK(HookedSearchPathW, g_searchPathWOriginal, ks::winapi_monitor::EventCategory::File, L"KernelBase", L"SearchPathW",
            (LPCWSTR pathPointer, LPCWSTR fileNamePointer, LPCWSTR extensionPointer, DWORD bufferLength, LPWSTR bufferPointer, LPWSTR* filePartPointer),
            (pathPointer, fileNamePointer, extensionPointer, bufferLength, bufferPointer, filePartPointer),
            { AppendWideText(detailBuffer, L"path="); AppendWideText(detailBuffer, pathPointer); AppendWideText(detailBuffer, L" file="); AppendWideText(detailBuffer, fileNamePointer); AppendWideText(detailBuffer, L" ext="); AppendWideText(detailBuffer, extensionPointer); AppendWideText(detailBuffer, L" result="); if (resultValue != 0 && resultValue < bufferLength) { AppendWideText(detailBuffer, bufferPointer, resultValue); } })
        APIMON_SIMPLE_DWORD_NONZERO_HOOK(HookedSearchPathA, g_searchPathAOriginal, ks::winapi_monitor::EventCategory::File, L"KernelBase", L"SearchPathA",
            (LPCSTR pathPointer, LPCSTR fileNamePointer, LPCSTR extensionPointer, DWORD bufferLength, LPSTR bufferPointer, LPSTR* filePartPointer),
            (pathPointer, fileNamePointer, extensionPointer, bufferLength, bufferPointer, filePartPointer),
            { AppendWideText(detailBuffer, L"path="); AppendAnsiText(detailBuffer, pathPointer); AppendWideText(detailBuffer, L" file="); AppendAnsiText(detailBuffer, fileNamePointer); AppendWideText(detailBuffer, L" ext="); AppendAnsiText(detailBuffer, extensionPointer); AppendWideText(detailBuffer, L" result="); if (resultValue != 0 && resultValue < bufferLength) { AppendAnsiText(detailBuffer, bufferPointer, resultValue); } })
        APIMON_SIMPLE_DWORD_NONZERO_HOOK(HookedGetShortPathNameW, g_getShortPathNameWOriginal, ks::winapi_monitor::EventCategory::File, L"KernelBase", L"GetShortPathNameW",
            (LPCWSTR longPathPointer, LPWSTR shortPathPointer, DWORD bufferLength), (longPathPointer, shortPathPointer, bufferLength),
            { AppendWideText(detailBuffer, L"input="); AppendWideText(detailBuffer, longPathPointer); AppendWideText(detailBuffer, L" output="); if (resultValue != 0 && resultValue < bufferLength) { AppendWideText(detailBuffer, shortPathPointer, resultValue); } })
        APIMON_SIMPLE_DWORD_NONZERO_HOOK(HookedGetShortPathNameA, g_getShortPathNameAOriginal, ks::winapi_monitor::EventCategory::File, L"KernelBase", L"GetShortPathNameA",
            (LPCSTR longPathPointer, LPSTR shortPathPointer, DWORD bufferLength), (longPathPointer, shortPathPointer, bufferLength),
            { AppendWideText(detailBuffer, L"input="); AppendAnsiText(detailBuffer, longPathPointer); AppendWideText(detailBuffer, L" output="); if (resultValue != 0 && resultValue < bufferLength) { AppendAnsiText(detailBuffer, shortPathPointer, resultValue); } })
        APIMON_SIMPLE_DWORD_NONZERO_HOOK(HookedGetLongPathNameW, g_getLongPathNameWOriginal, ks::winapi_monitor::EventCategory::File, L"KernelBase", L"GetLongPathNameW",
            (LPCWSTR shortPathPointer, LPWSTR longPathPointer, DWORD bufferLength), (shortPathPointer, longPathPointer, bufferLength),
            { AppendWideText(detailBuffer, L"input="); AppendWideText(detailBuffer, shortPathPointer); AppendWideText(detailBuffer, L" output="); if (resultValue != 0 && resultValue < bufferLength) { AppendWideText(detailBuffer, longPathPointer, resultValue); } })
        APIMON_SIMPLE_DWORD_NONZERO_HOOK(HookedGetLongPathNameA, g_getLongPathNameAOriginal, ks::winapi_monitor::EventCategory::File, L"KernelBase", L"GetLongPathNameA",
            (LPCSTR shortPathPointer, LPSTR longPathPointer, DWORD bufferLength), (shortPathPointer, longPathPointer, bufferLength),
            { AppendWideText(detailBuffer, L"input="); AppendAnsiText(detailBuffer, shortPathPointer); AppendWideText(detailBuffer, L" output="); if (resultValue != 0 && resultValue < bufferLength) { AppendAnsiText(detailBuffer, longPathPointer, resultValue); } })
        APIMON_SIMPLE_BOOL_HOOK(HookedCreatePipe, g_createPipeOriginal, ks::winapi_monitor::EventCategory::File, L"KernelBase", L"CreatePipe",
            (PHANDLE readPipePointer, PHANDLE writePipePointer, LPSECURITY_ATTRIBUTES securityAttributesPointer, DWORD sizeValue), (readPipePointer, writePipePointer, securityAttributesPointer, sizeValue),
            { AppendWideText(detailBuffer, L"size="); AppendUnsignedText(detailBuffer, sizeValue); AppendWideText(detailBuffer, L" read="); AppendHexText(detailBuffer, readPipePointer != nullptr ? reinterpret_cast<std::uint64_t>(*readPipePointer) : 0); AppendWideText(detailBuffer, L" write="); AppendHexText(detailBuffer, writePipePointer != nullptr ? reinterpret_cast<std::uint64_t>(*writePipePointer) : 0); })
        APIMON_SIMPLE_HANDLE_HOOK(HANDLE, HookedCreateMailslotW, g_createMailslotWOriginal, ks::winapi_monitor::EventCategory::File, L"KernelBase", L"CreateMailslotW",
            (LPCWSTR namePointer, DWORD maxMessageSize, DWORD readTimeout, LPSECURITY_ATTRIBUTES securityAttributesPointer), (namePointer, maxMessageSize, readTimeout, securityAttributesPointer),
            { AppendWideText(detailBuffer, L"name="); AppendWideText(detailBuffer, namePointer); AppendWideText(detailBuffer, L" maxMsg="); AppendUnsignedText(detailBuffer, maxMessageSize); AppendWideText(detailBuffer, L" timeout="); AppendUnsignedText(detailBuffer, readTimeout); AppendWideText(detailBuffer, L" handle="); AppendHexText(detailBuffer, reinterpret_cast<std::uint64_t>(resultHandle)); })
        APIMON_SIMPLE_HANDLE_HOOK(HANDLE, HookedCreateMailslotA, g_createMailslotAOriginal, ks::winapi_monitor::EventCategory::File, L"KernelBase", L"CreateMailslotA",
            (LPCSTR namePointer, DWORD maxMessageSize, DWORD readTimeout, LPSECURITY_ATTRIBUTES securityAttributesPointer), (namePointer, maxMessageSize, readTimeout, securityAttributesPointer),
            { AppendWideText(detailBuffer, L"name="); AppendAnsiText(detailBuffer, namePointer); AppendWideText(detailBuffer, L" maxMsg="); AppendUnsignedText(detailBuffer, maxMessageSize); AppendWideText(detailBuffer, L" timeout="); AppendUnsignedText(detailBuffer, readTimeout); AppendWideText(detailBuffer, L" handle="); AppendHexText(detailBuffer, reinterpret_cast<std::uint64_t>(resultHandle)); })
        APIMON_SIMPLE_BOOL_HOOK(HookedCreateDirectoryExW, g_createDirectoryExWOriginal, ks::winapi_monitor::EventCategory::File, L"KernelBase", L"CreateDirectoryExW",
            (LPCWSTR templateDirectoryPointer, LPCWSTR newDirectoryPointer, LPSECURITY_ATTRIBUTES securityAttributesPointer), (templateDirectoryPointer, newDirectoryPointer, securityAttributesPointer),
            { BuildTwoPathDetailW(detailBuffer, templateDirectoryPointer, newDirectoryPointer, 0); })
        APIMON_SIMPLE_BOOL_HOOK(HookedCreateDirectoryExA, g_createDirectoryExAOriginal, ks::winapi_monitor::EventCategory::File, L"KernelBase", L"CreateDirectoryExA",
            (LPCSTR templateDirectoryPointer, LPCSTR newDirectoryPointer, LPSECURITY_ATTRIBUTES securityAttributesPointer), (templateDirectoryPointer, newDirectoryPointer, securityAttributesPointer),
            { BuildTwoPathDetailA(detailBuffer, templateDirectoryPointer, newDirectoryPointer, 0); })

        APIMON_SIMPLE_WSA_INT_HOOK(HookedWSAStartup, g_wsaStartupOriginal, L"WSAStartup", resultValue == 0,
            (WORD versionRequested, LPWSADATA dataPointer), (versionRequested, dataPointer),
            { AppendWideText(detailBuffer, L"version="); AppendHexText(detailBuffer, versionRequested); AppendWideText(detailBuffer, L" highVersion="); AppendHexText(detailBuffer, dataPointer != nullptr ? dataPointer->wHighVersion : 0); })
        APIMON_SIMPLE_WSA_INT_HOOK(HookedWSACleanup, g_wsaCleanupOriginal, L"WSACleanup", resultValue == 0,
            (), (),
            { detailBuffer[0] = L'\0'; })
        APIMON_SIMPLE_WSA_INT_HOOK(HookedSelect, g_selectOriginal, L"select", resultValue != SOCKET_ERROR,
            (int nfdsValue, fd_set* readSetPointer, fd_set* writeSetPointer, fd_set* exceptSetPointer, const timeval* timeoutPointer),
            (nfdsValue, readSetPointer, writeSetPointer, exceptSetPointer, timeoutPointer),
            { AppendWideText(detailBuffer, L"nfds="); AppendUnsignedText(detailBuffer, static_cast<unsigned long long>(nfdsValue < 0 ? 0 : nfdsValue)); AppendWideText(detailBuffer, L" ready="); AppendUnsignedText(detailBuffer, static_cast<unsigned long long>(resultValue < 0 ? 0 : resultValue)); AppendWideText(detailBuffer, L" timeout="); AppendHexText(detailBuffer, reinterpret_cast<std::uint64_t>(timeoutPointer)); })
        APIMON_SIMPLE_WSA_INT_HOOK(HookedIoctlSocket, g_ioctlSocketOriginal, L"ioctlsocket", resultValue == 0,
            (SOCKET socketValue, long commandValue, u_long* argumentPointer), (socketValue, commandValue, argumentPointer),
            { AppendWideText(detailBuffer, L"socket="); AppendHexText(detailBuffer, static_cast<std::uint64_t>(socketValue)); AppendWideText(detailBuffer, L" cmd="); AppendHexText(detailBuffer, static_cast<std::uint64_t>(commandValue)); AppendWideText(detailBuffer, L" arg="); AppendHexText(detailBuffer, argumentPointer != nullptr ? *argumentPointer : 0); })
        APIMON_SIMPLE_WSA_INT_HOOK(HookedSetSockOpt, g_setSockOptOriginal, L"setsockopt", resultValue == 0,
            (SOCKET socketValue, int levelValue, int optionName, const char* optionValuePointer, int optionLength), (socketValue, levelValue, optionName, optionValuePointer, optionLength),
            { AppendWideText(detailBuffer, L"socket="); AppendHexText(detailBuffer, static_cast<std::uint64_t>(socketValue)); AppendWideText(detailBuffer, L" level="); AppendUnsignedText(detailBuffer, static_cast<unsigned long long>(levelValue < 0 ? 0 : levelValue)); AppendWideText(detailBuffer, L" opt="); AppendUnsignedText(detailBuffer, static_cast<unsigned long long>(optionName < 0 ? 0 : optionName)); AppendWideText(detailBuffer, L" len="); AppendUnsignedText(detailBuffer, static_cast<unsigned long long>(optionLength < 0 ? 0 : optionLength)); })
        APIMON_SIMPLE_WSA_INT_HOOK(HookedGetSockOpt, g_getSockOptOriginal, L"getsockopt", resultValue == 0,
            (SOCKET socketValue, int levelValue, int optionName, char* optionValuePointer, int* optionLengthPointer), (socketValue, levelValue, optionName, optionValuePointer, optionLengthPointer),
            { AppendWideText(detailBuffer, L"socket="); AppendHexText(detailBuffer, static_cast<std::uint64_t>(socketValue)); AppendWideText(detailBuffer, L" level="); AppendUnsignedText(detailBuffer, static_cast<unsigned long long>(levelValue < 0 ? 0 : levelValue)); AppendWideText(detailBuffer, L" opt="); AppendUnsignedText(detailBuffer, static_cast<unsigned long long>(optionName < 0 ? 0 : optionName)); AppendWideText(detailBuffer, L" len="); AppendUnsignedText(detailBuffer, optionLengthPointer != nullptr ? static_cast<unsigned long long>(*optionLengthPointer < 0 ? 0 : *optionLengthPointer) : 0ULL); })
        APIMON_SIMPLE_WSA_INT_HOOK(HookedGetSockName, g_getSockNameOriginal, L"getsockname", resultValue == 0,
            (SOCKET socketValue, sockaddr* namePointer, int* nameLengthPointer), (socketValue, namePointer, nameLengthPointer),
            { AppendWideText(detailBuffer, L"socket="); AppendHexText(detailBuffer, static_cast<std::uint64_t>(socketValue)); AppendWideText(detailBuffer, L" addr="); AppendSocketAddress(detailBuffer, namePointer, nameLengthPointer != nullptr ? *nameLengthPointer : 0); })
        APIMON_SIMPLE_WSA_INT_HOOK(HookedGetPeerName, g_getPeerNameOriginal, L"getpeername", resultValue == 0,
            (SOCKET socketValue, sockaddr* namePointer, int* nameLengthPointer), (socketValue, namePointer, nameLengthPointer),
            { AppendWideText(detailBuffer, L"socket="); AppendHexText(detailBuffer, static_cast<std::uint64_t>(socketValue)); AppendWideText(detailBuffer, L" addr="); AppendSocketAddress(detailBuffer, namePointer, nameLengthPointer != nullptr ? *nameLengthPointer : 0); })
        APIMON_SIMPLE_WSA_INT_HOOK(HookedWSAEventSelect, g_wsaEventSelectOriginal, L"WSAEventSelect", resultValue == 0,
            (SOCKET socketValue, WSAEVENT eventHandle, long networkEvents), (socketValue, eventHandle, networkEvents),
            { AppendWideText(detailBuffer, L"socket="); AppendHexText(detailBuffer, static_cast<std::uint64_t>(socketValue)); AppendWideText(detailBuffer, L" event="); AppendHexText(detailBuffer, reinterpret_cast<std::uint64_t>(eventHandle)); AppendWideText(detailBuffer, L" netEvents="); AppendHexText(detailBuffer, static_cast<std::uint64_t>(networkEvents)); })
        APIMON_SIMPLE_WSA_INT_HOOK(HookedWSAAsyncSelect, g_wsaAsyncSelectOriginal, L"WSAAsyncSelect", resultValue == 0,
            (SOCKET socketValue, HWND windowHandle, unsigned int messageValue, long networkEvents), (socketValue, windowHandle, messageValue, networkEvents),
            { AppendWideText(detailBuffer, L"socket="); AppendHexText(detailBuffer, static_cast<std::uint64_t>(socketValue)); AppendWideText(detailBuffer, L" hwnd="); AppendHexText(detailBuffer, reinterpret_cast<std::uint64_t>(windowHandle)); AppendWideText(detailBuffer, L" msg="); AppendHexText(detailBuffer, messageValue); AppendWideText(detailBuffer, L" netEvents="); AppendHexText(detailBuffer, static_cast<std::uint64_t>(networkEvents)); })
        APIMON_SIMPLE_HANDLE_HOOK(HostEntPtr, HookedGetHostByName, g_getHostByNameOriginal, ks::winapi_monitor::EventCategory::Network, L"Ws2_32", L"gethostbyname",
            (const char* namePointer), (namePointer),
            { AppendWideText(detailBuffer, L"name="); AppendAnsiText(detailBuffer, namePointer); AppendWideText(detailBuffer, L" result="); AppendHexText(detailBuffer, reinterpret_cast<std::uint64_t>(resultHandle)); })
        APIMON_SIMPLE_HANDLE_HOOK(HostEntPtr, HookedGetHostByAddr, g_getHostByAddrOriginal, ks::winapi_monitor::EventCategory::Network, L"Ws2_32", L"gethostbyaddr",
            (const char* addressPointer, int lengthValue, int typeValue), (addressPointer, lengthValue, typeValue),
            { AppendWideText(detailBuffer, L"len="); AppendUnsignedText(detailBuffer, static_cast<unsigned long long>(lengthValue < 0 ? 0 : lengthValue)); AppendWideText(detailBuffer, L" type="); AppendUnsignedText(detailBuffer, static_cast<unsigned long long>(typeValue < 0 ? 0 : typeValue)); AppendWideText(detailBuffer, L" result="); AppendHexText(detailBuffer, reinterpret_cast<std::uint64_t>(resultHandle)); })
        APIMON_SIMPLE_WSA_INT_HOOK(HookedGetNameInfoW, g_getNameInfoWOriginal, L"GetNameInfoW", resultValue == 0,
            (const sockaddr* sockaddrPointer, int sockaddrLength, PWCHAR hostPointer, DWORD hostLength, PWCHAR servicePointer, DWORD serviceLength, INT flagsValue),
            (sockaddrPointer, sockaddrLength, hostPointer, hostLength, servicePointer, serviceLength, flagsValue),
            { AppendSocketAddress(detailBuffer, sockaddrPointer, sockaddrLength); AppendWideText(detailBuffer, L" flags="); AppendHexText(detailBuffer, static_cast<std::uint64_t>(flagsValue)); AppendWideText(detailBuffer, L" host="); if (resultValue == 0) { AppendWideText(detailBuffer, hostPointer); } })
        APIMON_SIMPLE_WSA_INT_HOOK(HookedGetNameInfoA, g_getNameInfoAOriginal, L"getnameinfo", resultValue == 0,
            (const sockaddr* sockaddrPointer, int sockaddrLength, PCHAR hostPointer, DWORD hostLength, PCHAR servicePointer, DWORD serviceLength, INT flagsValue),
            (sockaddrPointer, sockaddrLength, hostPointer, hostLength, servicePointer, serviceLength, flagsValue),
            { AppendSocketAddress(detailBuffer, sockaddrPointer, sockaddrLength); AppendWideText(detailBuffer, L" flags="); AppendHexText(detailBuffer, static_cast<std::uint64_t>(flagsValue)); AppendWideText(detailBuffer, L" host="); if (resultValue == 0) { AppendAnsiText(detailBuffer, hostPointer); } })

        APIMON_SIMPLE_BOOL_HOOK(HookedWinHttpAddRequestHeaders, g_winHttpAddRequestHeadersOriginal, ks::winapi_monitor::EventCategory::Network, L"Winhttp", L"WinHttpAddRequestHeaders",
            (HINTERNET requestHandle, LPCWSTR headersPointer, DWORD headersLength, DWORD modifiersValue), (requestHandle, headersPointer, headersLength, modifiersValue),
            { AppendWideText(detailBuffer, L"request="); AppendHexText(detailBuffer, reinterpret_cast<std::uint64_t>(requestHandle)); AppendWideText(detailBuffer, L" length="); AppendUnsignedText(detailBuffer, headersLength); AppendWideText(detailBuffer, L" modifiers="); AppendHexText(detailBuffer, modifiersValue); })
        APIMON_SIMPLE_BOOL_HOOK(HookedWinHttpSetCredentials, g_winHttpSetCredentialsOriginal, ks::winapi_monitor::EventCategory::Network, L"Winhttp", L"WinHttpSetCredentials",
            (HINTERNET requestHandle, DWORD authTargets, DWORD authScheme, LPCWSTR userNamePointer, LPCWSTR passwordPointer, LPVOID authParamsPointer),
            (requestHandle, authTargets, authScheme, userNamePointer, passwordPointer, authParamsPointer),
            { AppendWideText(detailBuffer, L"request="); AppendHexText(detailBuffer, reinterpret_cast<std::uint64_t>(requestHandle)); AppendWideText(detailBuffer, L" targets="); AppendHexText(detailBuffer, authTargets); AppendWideText(detailBuffer, L" scheme="); AppendHexText(detailBuffer, authScheme); AppendWideText(detailBuffer, L" user="); AppendWideText(detailBuffer, userNamePointer); })
        APIMON_SIMPLE_BOOL_HOOK(HookedWinHttpCrackUrl, g_winHttpCrackUrlOriginal, ks::winapi_monitor::EventCategory::Network, L"Winhttp", L"WinHttpCrackUrl",
            (LPCWSTR urlPointer, DWORD urlLength, DWORD flagsValue, LPURL_COMPONENTS componentsPointer), (urlPointer, urlLength, flagsValue, componentsPointer),
            { AppendWideText(detailBuffer, L"url="); AppendWideText(detailBuffer, urlPointer, urlLength != 0 ? urlLength : static_cast<std::size_t>(-1)); AppendWideText(detailBuffer, L" flags="); AppendHexText(detailBuffer, flagsValue); })
        APIMON_SIMPLE_BOOL_HOOK(HookedWinHttpCreateUrl, g_winHttpCreateUrlOriginal, ks::winapi_monitor::EventCategory::Network, L"Winhttp", L"WinHttpCreateUrl",
            (LPURL_COMPONENTS componentsPointer, DWORD flagsValue, LPWSTR urlPointer, LPDWORD urlLengthPointer), (componentsPointer, flagsValue, urlPointer, urlLengthPointer),
            { AppendWideText(detailBuffer, L"flags="); AppendHexText(detailBuffer, flagsValue); AppendWideText(detailBuffer, L" length="); AppendUnsignedText(detailBuffer, urlLengthPointer != nullptr ? *urlLengthPointer : 0); AppendWideText(detailBuffer, L" url="); if (resultValue != FALSE) { AppendWideText(detailBuffer, urlPointer); } })
        APIMON_SIMPLE_BOOL_HOOK(HookedWinHttpSetTimeouts, g_winHttpSetTimeoutsOriginal, ks::winapi_monitor::EventCategory::Network, L"Winhttp", L"WinHttpSetTimeouts",
            (HINTERNET internetHandle, int resolveTimeout, int connectTimeout, int sendTimeout, int receiveTimeout),
            (internetHandle, resolveTimeout, connectTimeout, sendTimeout, receiveTimeout),
            { AppendWideText(detailBuffer, L"handle="); AppendHexText(detailBuffer, reinterpret_cast<std::uint64_t>(internetHandle)); AppendWideText(detailBuffer, L" resolve="); AppendUnsignedText(detailBuffer, static_cast<unsigned long long>(resolveTimeout < 0 ? 0 : resolveTimeout)); AppendWideText(detailBuffer, L" connect="); AppendUnsignedText(detailBuffer, static_cast<unsigned long long>(connectTimeout < 0 ? 0 : connectTimeout)); AppendWideText(detailBuffer, L" send="); AppendUnsignedText(detailBuffer, static_cast<unsigned long long>(sendTimeout < 0 ? 0 : sendTimeout)); AppendWideText(detailBuffer, L" recv="); AppendUnsignedText(detailBuffer, static_cast<unsigned long long>(receiveTimeout < 0 ? 0 : receiveTimeout)); })
        APIMON_SIMPLE_BOOL_HOOK(HookedHttpQueryInfoW, g_httpQueryInfoWOriginal, ks::winapi_monitor::EventCategory::Network, L"Wininet", L"HttpQueryInfoW",
            (HINTERNET requestHandle, DWORD infoLevel, LPVOID bufferPointer, LPDWORD bufferLengthPointer, LPDWORD indexPointer), (requestHandle, infoLevel, bufferPointer, bufferLengthPointer, indexPointer),
            { AppendWideText(detailBuffer, L"request="); AppendHexText(detailBuffer, reinterpret_cast<std::uint64_t>(requestHandle)); AppendWideText(detailBuffer, L" level="); AppendHexText(detailBuffer, infoLevel); AppendWideText(detailBuffer, L" length="); AppendUnsignedText(detailBuffer, bufferLengthPointer != nullptr ? *bufferLengthPointer : 0); })
        APIMON_SIMPLE_BOOL_HOOK(HookedHttpQueryInfoA, g_httpQueryInfoAOriginal, ks::winapi_monitor::EventCategory::Network, L"Wininet", L"HttpQueryInfoA",
            (HINTERNET requestHandle, DWORD infoLevel, LPVOID bufferPointer, LPDWORD bufferLengthPointer, LPDWORD indexPointer), (requestHandle, infoLevel, bufferPointer, bufferLengthPointer, indexPointer),
            { AppendWideText(detailBuffer, L"request="); AppendHexText(detailBuffer, reinterpret_cast<std::uint64_t>(requestHandle)); AppendWideText(detailBuffer, L" level="); AppendHexText(detailBuffer, infoLevel); AppendWideText(detailBuffer, L" length="); AppendUnsignedText(detailBuffer, bufferLengthPointer != nullptr ? *bufferLengthPointer : 0); })
        APIMON_SIMPLE_BOOL_HOOK(HookedInternetQueryOptionW, g_internetQueryOptionWOriginal, ks::winapi_monitor::EventCategory::Network, L"Wininet", L"InternetQueryOptionW",
            (HINTERNET internetHandle, DWORD optionValue, LPVOID bufferPointer, LPDWORD bufferLengthPointer), (internetHandle, optionValue, bufferPointer, bufferLengthPointer),
            { AppendWideText(detailBuffer, L"handle="); AppendHexText(detailBuffer, reinterpret_cast<std::uint64_t>(internetHandle)); AppendWideText(detailBuffer, L" option="); AppendHexText(detailBuffer, optionValue); AppendWideText(detailBuffer, L" length="); AppendUnsignedText(detailBuffer, bufferLengthPointer != nullptr ? *bufferLengthPointer : 0); })
        APIMON_SIMPLE_BOOL_HOOK(HookedInternetQueryOptionA, g_internetQueryOptionAOriginal, ks::winapi_monitor::EventCategory::Network, L"Wininet", L"InternetQueryOptionA",
            (HINTERNET internetHandle, DWORD optionValue, LPVOID bufferPointer, LPDWORD bufferLengthPointer), (internetHandle, optionValue, bufferPointer, bufferLengthPointer),
            { AppendWideText(detailBuffer, L"handle="); AppendHexText(detailBuffer, reinterpret_cast<std::uint64_t>(internetHandle)); AppendWideText(detailBuffer, L" option="); AppendHexText(detailBuffer, optionValue); AppendWideText(detailBuffer, L" length="); AppendUnsignedText(detailBuffer, bufferLengthPointer != nullptr ? *bufferLengthPointer : 0); })

        // HookedSixthBatchNativeObjectSync 作用：
        // - 输入：ntdll 对象目录、符号链接和 semaphore 原生对象参数；
        // - 处理：补齐 Win32 同步/路径枚举背后的 Object Manager 层访问与命名对象解析行为；
        // - 返回：保持各 API 原始 NTSTATUS，并将对象名、访问掩码、计数和输出句柄写入事件详情。
        APIMON_SIMPLE_NTSTATUS_HOOK(HookedNtOpenDirectoryObject, g_ntOpenDirectoryObjectOriginal, ks::winapi_monitor::EventCategory::Process, L"ntdll", L"NtOpenDirectoryObject",
            (PHANDLE directoryHandlePointer, ACCESS_MASK desiredAccess, POBJECT_ATTRIBUTES objectAttributesPointer),
            (directoryHandlePointer, desiredAccess, objectAttributesPointer),
            { AppendWideText(detailBuffer, L"object="); AppendObjectNameText(detailBuffer, objectAttributesPointer); AppendWideText(detailBuffer, L" access="); AppendHexText(detailBuffer, desiredAccess); AppendWideText(detailBuffer, L" handle="); AppendHexText(detailBuffer, directoryHandlePointer != nullptr ? reinterpret_cast<std::uint64_t>(*directoryHandlePointer) : 0); })
        APIMON_SIMPLE_NTSTATUS_HOOK(HookedNtQueryDirectoryObject, g_ntQueryDirectoryObjectOriginal, ks::winapi_monitor::EventCategory::Process, L"ntdll", L"NtQueryDirectoryObject",
            (HANDLE directoryHandle, PVOID bufferPointer, ULONG lengthValue, BOOLEAN returnSingleEntry, BOOLEAN restartScan, PULONG contextPointer, PULONG returnLengthPointer),
            (directoryHandle, bufferPointer, lengthValue, returnSingleEntry, restartScan, contextPointer, returnLengthPointer),
            { AppendWideText(detailBuffer, L"directory="); AppendHexText(detailBuffer, reinterpret_cast<std::uint64_t>(directoryHandle)); AppendWideText(detailBuffer, L" length="); AppendUnsignedText(detailBuffer, lengthValue); AppendWideText(detailBuffer, L" single="); AppendUnsignedText(detailBuffer, returnSingleEntry != FALSE ? 1ULL : 0ULL); AppendWideText(detailBuffer, L" restart="); AppendUnsignedText(detailBuffer, restartScan != FALSE ? 1ULL : 0ULL); AppendWideText(detailBuffer, L" context="); AppendUnsignedText(detailBuffer, contextPointer != nullptr ? *contextPointer : 0); AppendWideText(detailBuffer, L" returned="); AppendUnsignedText(detailBuffer, returnLengthPointer != nullptr ? *returnLengthPointer : 0); })
        APIMON_SIMPLE_NTSTATUS_HOOK(HookedNtCreateSymbolicLinkObject, g_ntCreateSymbolicLinkObjectOriginal, ks::winapi_monitor::EventCategory::Process, L"ntdll", L"NtCreateSymbolicLinkObject",
            (PHANDLE linkHandlePointer, ACCESS_MASK desiredAccess, POBJECT_ATTRIBUTES objectAttributesPointer, PUNICODE_STRING targetNamePointer),
            (linkHandlePointer, desiredAccess, objectAttributesPointer, targetNamePointer),
            { AppendWideText(detailBuffer, L"object="); AppendObjectNameText(detailBuffer, objectAttributesPointer); AppendWideText(detailBuffer, L" target="); AppendUnicodeStringText(detailBuffer, targetNamePointer); AppendWideText(detailBuffer, L" access="); AppendHexText(detailBuffer, desiredAccess); AppendWideText(detailBuffer, L" handle="); AppendHexText(detailBuffer, linkHandlePointer != nullptr ? reinterpret_cast<std::uint64_t>(*linkHandlePointer) : 0); })
        APIMON_SIMPLE_NTSTATUS_HOOK(HookedNtOpenSymbolicLinkObject, g_ntOpenSymbolicLinkObjectOriginal, ks::winapi_monitor::EventCategory::Process, L"ntdll", L"NtOpenSymbolicLinkObject",
            (PHANDLE linkHandlePointer, ACCESS_MASK desiredAccess, POBJECT_ATTRIBUTES objectAttributesPointer),
            (linkHandlePointer, desiredAccess, objectAttributesPointer),
            { AppendWideText(detailBuffer, L"object="); AppendObjectNameText(detailBuffer, objectAttributesPointer); AppendWideText(detailBuffer, L" access="); AppendHexText(detailBuffer, desiredAccess); AppendWideText(detailBuffer, L" handle="); AppendHexText(detailBuffer, linkHandlePointer != nullptr ? reinterpret_cast<std::uint64_t>(*linkHandlePointer) : 0); })
        APIMON_SIMPLE_NTSTATUS_HOOK(HookedNtQuerySymbolicLinkObject, g_ntQuerySymbolicLinkObjectOriginal, ks::winapi_monitor::EventCategory::Process, L"ntdll", L"NtQuerySymbolicLinkObject",
            (HANDLE linkHandle, PUNICODE_STRING targetNamePointer, PULONG returnLengthPointer),
            (linkHandle, targetNamePointer, returnLengthPointer),
            { AppendWideText(detailBuffer, L"link="); AppendHexText(detailBuffer, reinterpret_cast<std::uint64_t>(linkHandle)); AppendWideText(detailBuffer, L" target="); AppendUnicodeStringText(detailBuffer, targetNamePointer); AppendWideText(detailBuffer, L" returned="); AppendUnsignedText(detailBuffer, returnLengthPointer != nullptr ? *returnLengthPointer : 0); })
        APIMON_SIMPLE_NTSTATUS_HOOK(HookedNtCreateSemaphore, g_ntCreateSemaphoreOriginal, ks::winapi_monitor::EventCategory::Process, L"ntdll", L"NtCreateSemaphore",
            (PHANDLE semaphoreHandlePointer, ACCESS_MASK desiredAccess, POBJECT_ATTRIBUTES objectAttributesPointer, LONG initialCount, LONG maximumCount),
            (semaphoreHandlePointer, desiredAccess, objectAttributesPointer, initialCount, maximumCount),
            { AppendWideText(detailBuffer, L"object="); AppendObjectNameText(detailBuffer, objectAttributesPointer); AppendWideText(detailBuffer, L" access="); AppendHexText(detailBuffer, desiredAccess); AppendWideText(detailBuffer, L" initial="); AppendUnsignedText(detailBuffer, static_cast<unsigned long long>(initialCount < 0 ? 0 : initialCount)); AppendWideText(detailBuffer, L" max="); AppendUnsignedText(detailBuffer, static_cast<unsigned long long>(maximumCount < 0 ? 0 : maximumCount)); AppendWideText(detailBuffer, L" handle="); AppendHexText(detailBuffer, semaphoreHandlePointer != nullptr ? reinterpret_cast<std::uint64_t>(*semaphoreHandlePointer) : 0); })
        APIMON_SIMPLE_NTSTATUS_HOOK(HookedNtOpenSemaphore, g_ntOpenSemaphoreOriginal, ks::winapi_monitor::EventCategory::Process, L"ntdll", L"NtOpenSemaphore",
            (PHANDLE semaphoreHandlePointer, ACCESS_MASK desiredAccess, POBJECT_ATTRIBUTES objectAttributesPointer),
            (semaphoreHandlePointer, desiredAccess, objectAttributesPointer),
            { AppendWideText(detailBuffer, L"object="); AppendObjectNameText(detailBuffer, objectAttributesPointer); AppendWideText(detailBuffer, L" access="); AppendHexText(detailBuffer, desiredAccess); AppendWideText(detailBuffer, L" handle="); AppendHexText(detailBuffer, semaphoreHandlePointer != nullptr ? reinterpret_cast<std::uint64_t>(*semaphoreHandlePointer) : 0); })
        APIMON_SIMPLE_NTSTATUS_HOOK(HookedNtReleaseSemaphore, g_ntReleaseSemaphoreOriginal, ks::winapi_monitor::EventCategory::Process, L"ntdll", L"NtReleaseSemaphore",
            (HANDLE semaphoreHandle, LONG releaseCount, PLONG previousCountPointer),
            (semaphoreHandle, releaseCount, previousCountPointer),
            { AppendWideText(detailBuffer, L"semaphore="); AppendHexText(detailBuffer, reinterpret_cast<std::uint64_t>(semaphoreHandle)); AppendWideText(detailBuffer, L" release="); AppendUnsignedText(detailBuffer, static_cast<unsigned long long>(releaseCount < 0 ? 0 : releaseCount)); AppendWideText(detailBuffer, L" previous="); AppendUnsignedText(detailBuffer, previousCountPointer != nullptr ? static_cast<unsigned long long>(*previousCountPointer < 0 ? 0 : *previousCountPointer) : 0ULL); })

        // HookBinding g_bindings：
        // - 输入：静态白名单中的模块/导出名/分类和 Hooked wrapper 元数据；
        // - 处理：InstallConfiguredHooks 遍历该表安装 inline hook，LoadLibrary/LdrLoadDll 后会重试延迟模块；
        // - 返回：本表本身无返回值，是所有已支持 API 覆盖面的权威来源。
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
            { L"KernelBase.dll", "CreateFileMappingW", ks::winapi_monitor::EventCategory::File, &g_createFileMappingWHook, reinterpret_cast<void*>(&HookedCreateFileMappingW), reinterpret_cast<void**>(&g_createFileMappingWOriginal) },
            { L"KernelBase.dll", "CreateFileMappingA", ks::winapi_monitor::EventCategory::File, &g_createFileMappingAHook, reinterpret_cast<void*>(&HookedCreateFileMappingA), reinterpret_cast<void**>(&g_createFileMappingAOriginal) },
            { L"KernelBase.dll", "OpenFileMappingW", ks::winapi_monitor::EventCategory::File, &g_openFileMappingWHook, reinterpret_cast<void*>(&HookedOpenFileMappingW), reinterpret_cast<void**>(&g_openFileMappingWOriginal) },
            { L"KernelBase.dll", "OpenFileMappingA", ks::winapi_monitor::EventCategory::File, &g_openFileMappingAHook, reinterpret_cast<void*>(&HookedOpenFileMappingA), reinterpret_cast<void**>(&g_openFileMappingAOriginal) },
            { L"KernelBase.dll", "FlushViewOfFile", ks::winapi_monitor::EventCategory::File, &g_flushViewOfFileHook, reinterpret_cast<void*>(&HookedFlushViewOfFile), reinterpret_cast<void**>(&g_flushViewOfFileOriginal) },
            { L"KernelBase.dll", "CreateHardLinkW", ks::winapi_monitor::EventCategory::File, &g_createHardLinkWHook, reinterpret_cast<void*>(&HookedCreateHardLinkW), reinterpret_cast<void**>(&g_createHardLinkWOriginal) },
            { L"KernelBase.dll", "CreateHardLinkA", ks::winapi_monitor::EventCategory::File, &g_createHardLinkAHook, reinterpret_cast<void*>(&HookedCreateHardLinkA), reinterpret_cast<void**>(&g_createHardLinkAOriginal) },
            { L"KernelBase.dll", "ReplaceFileW", ks::winapi_monitor::EventCategory::File, &g_replaceFileWHook, reinterpret_cast<void*>(&HookedReplaceFileW), reinterpret_cast<void**>(&g_replaceFileWOriginal) },
            { L"KernelBase.dll", "ReplaceFileA", ks::winapi_monitor::EventCategory::File, &g_replaceFileAHook, reinterpret_cast<void*>(&HookedReplaceFileA), reinterpret_cast<void**>(&g_replaceFileAOriginal) },
            { L"KernelBase.dll", "SetEndOfFile", ks::winapi_monitor::EventCategory::File, &g_setEndOfFileHook, reinterpret_cast<void*>(&HookedSetEndOfFile), reinterpret_cast<void**>(&g_setEndOfFileOriginal) },
            { L"KernelBase.dll", "LockFileEx", ks::winapi_monitor::EventCategory::File, &g_lockFileExHook, reinterpret_cast<void*>(&HookedLockFileEx), reinterpret_cast<void**>(&g_lockFileExOriginal) },
            { L"KernelBase.dll", "UnlockFileEx", ks::winapi_monitor::EventCategory::File, &g_unlockFileExHook, reinterpret_cast<void*>(&HookedUnlockFileEx), reinterpret_cast<void**>(&g_unlockFileExOriginal) },
            { L"KernelBase.dll", "CreateSymbolicLinkW", ks::winapi_monitor::EventCategory::File, &g_createSymbolicLinkWHook, reinterpret_cast<void*>(&HookedCreateSymbolicLinkW), reinterpret_cast<void**>(&g_createSymbolicLinkWOriginal) },
            { L"KernelBase.dll", "CreateSymbolicLinkA", ks::winapi_monitor::EventCategory::File, &g_createSymbolicLinkAHook, reinterpret_cast<void*>(&HookedCreateSymbolicLinkA), reinterpret_cast<void**>(&g_createSymbolicLinkAOriginal) },
            { L"KernelBase.dll", "GetFinalPathNameByHandleW", ks::winapi_monitor::EventCategory::File, &g_getFinalPathNameByHandleWHook, reinterpret_cast<void*>(&HookedGetFinalPathNameByHandleW), reinterpret_cast<void**>(&g_getFinalPathNameByHandleWOriginal) },
            { L"KernelBase.dll", "GetFinalPathNameByHandleA", ks::winapi_monitor::EventCategory::File, &g_getFinalPathNameByHandleAHook, reinterpret_cast<void*>(&HookedGetFinalPathNameByHandleA), reinterpret_cast<void**>(&g_getFinalPathNameByHandleAOriginal) },
            { L"KernelBase.dll", "GetFileSizeEx", ks::winapi_monitor::EventCategory::File, &g_getFileSizeExHook, reinterpret_cast<void*>(&HookedGetFileSizeEx), reinterpret_cast<void**>(&g_getFileSizeExOriginal) },
            { L"KernelBase.dll", "SetFilePointerEx", ks::winapi_monitor::EventCategory::File, &g_setFilePointerExHook, reinterpret_cast<void*>(&HookedSetFilePointerEx), reinterpret_cast<void**>(&g_setFilePointerExOriginal) },
            { L"KernelBase.dll", "CreateNamedPipeW", ks::winapi_monitor::EventCategory::File, &g_createNamedPipeWHook, reinterpret_cast<void*>(&HookedCreateNamedPipeW), reinterpret_cast<void**>(&g_createNamedPipeWOriginal) },
            { L"KernelBase.dll", "CreateNamedPipeA", ks::winapi_monitor::EventCategory::File, &g_createNamedPipeAHook, reinterpret_cast<void*>(&HookedCreateNamedPipeA), reinterpret_cast<void**>(&g_createNamedPipeAOriginal) },
            { L"KernelBase.dll", "ConnectNamedPipe", ks::winapi_monitor::EventCategory::File, &g_connectNamedPipeHook, reinterpret_cast<void*>(&HookedConnectNamedPipe), reinterpret_cast<void**>(&g_connectNamedPipeOriginal) },
            { L"KernelBase.dll", "DisconnectNamedPipe", ks::winapi_monitor::EventCategory::File, &g_disconnectNamedPipeHook, reinterpret_cast<void*>(&HookedDisconnectNamedPipe), reinterpret_cast<void**>(&g_disconnectNamedPipeOriginal) },
            { L"KernelBase.dll", "WaitNamedPipeW", ks::winapi_monitor::EventCategory::File, &g_waitNamedPipeWHook, reinterpret_cast<void*>(&HookedWaitNamedPipeW), reinterpret_cast<void**>(&g_waitNamedPipeWOriginal) },
            { L"KernelBase.dll", "WaitNamedPipeA", ks::winapi_monitor::EventCategory::File, &g_waitNamedPipeAHook, reinterpret_cast<void*>(&HookedWaitNamedPipeA), reinterpret_cast<void**>(&g_waitNamedPipeAOriginal) },
            { L"KernelBase.dll", "TransactNamedPipe", ks::winapi_monitor::EventCategory::File, &g_transactNamedPipeHook, reinterpret_cast<void*>(&HookedTransactNamedPipe), reinterpret_cast<void**>(&g_transactNamedPipeOriginal) },
            { L"KernelBase.dll", "CreateProcessA", ks::winapi_monitor::EventCategory::Process, &g_createProcessAHook, reinterpret_cast<void*>(&HookedCreateProcessA), reinterpret_cast<void**>(&g_createProcessAOriginal) },
            { L"KernelBase.dll", "CreateProcessW", ks::winapi_monitor::EventCategory::Process, &g_createProcessWHook, reinterpret_cast<void*>(&HookedCreateProcessW), reinterpret_cast<void**>(&g_createProcessWOriginal) },
            { L"KernelBase.dll", "OpenProcess", ks::winapi_monitor::EventCategory::Process, &g_openProcessHook, reinterpret_cast<void*>(&HookedOpenProcess), reinterpret_cast<void**>(&g_openProcessOriginal) },
            { L"KernelBase.dll", "OpenThread", ks::winapi_monitor::EventCategory::Process, &g_openThreadHook, reinterpret_cast<void*>(&HookedOpenThread), reinterpret_cast<void**>(&g_openThreadOriginal) },
            { L"KernelBase.dll", "TerminateProcess", ks::winapi_monitor::EventCategory::Process, &g_terminateProcessHook, reinterpret_cast<void*>(&HookedTerminateProcess), reinterpret_cast<void**>(&g_terminateProcessOriginal) },
            { L"KernelBase.dll", "CreateThread", ks::winapi_monitor::EventCategory::Process, &g_createThreadHook, reinterpret_cast<void*>(&HookedCreateThread), reinterpret_cast<void**>(&g_createThreadOriginal) },
            { L"KernelBase.dll", "CreateRemoteThread", ks::winapi_monitor::EventCategory::Process, &g_createRemoteThreadHook, reinterpret_cast<void*>(&HookedCreateRemoteThread), reinterpret_cast<void**>(&g_createRemoteThreadOriginal) },
            { L"KernelBase.dll", "CreateRemoteThreadEx", ks::winapi_monitor::EventCategory::Process, &g_createRemoteThreadExHook, reinterpret_cast<void*>(&HookedCreateRemoteThreadEx), reinterpret_cast<void**>(&g_createRemoteThreadExOriginal) },
            { L"KernelBase.dll", "VirtualAllocEx", ks::winapi_monitor::EventCategory::Process, &g_virtualAllocExHook, reinterpret_cast<void*>(&HookedVirtualAllocEx), reinterpret_cast<void**>(&g_virtualAllocExOriginal) },
            { L"KernelBase.dll", "VirtualFreeEx", ks::winapi_monitor::EventCategory::Process, &g_virtualFreeExHook, reinterpret_cast<void*>(&HookedVirtualFreeEx), reinterpret_cast<void**>(&g_virtualFreeExOriginal) },
            { L"KernelBase.dll", "VirtualProtectEx", ks::winapi_monitor::EventCategory::Process, &g_virtualProtectExHook, reinterpret_cast<void*>(&HookedVirtualProtectEx), reinterpret_cast<void**>(&g_virtualProtectExOriginal) },
            { L"KernelBase.dll", "VirtualAlloc", ks::winapi_monitor::EventCategory::Process, &g_virtualAllocHook, reinterpret_cast<void*>(&HookedVirtualAlloc), reinterpret_cast<void**>(&g_virtualAllocOriginal) },
            { L"KernelBase.dll", "VirtualFree", ks::winapi_monitor::EventCategory::Process, &g_virtualFreeHook, reinterpret_cast<void*>(&HookedVirtualFree), reinterpret_cast<void**>(&g_virtualFreeOriginal) },
            { L"KernelBase.dll", "VirtualProtect", ks::winapi_monitor::EventCategory::Process, &g_virtualProtectHook, reinterpret_cast<void*>(&HookedVirtualProtect), reinterpret_cast<void**>(&g_virtualProtectOriginal) },
            { L"KernelBase.dll", "WriteProcessMemory", ks::winapi_monitor::EventCategory::Process, &g_writeProcessMemoryHook, reinterpret_cast<void*>(&HookedWriteProcessMemory), reinterpret_cast<void**>(&g_writeProcessMemoryOriginal) },
            { L"KernelBase.dll", "ReadProcessMemory", ks::winapi_monitor::EventCategory::Process, &g_readProcessMemoryHook, reinterpret_cast<void*>(&HookedReadProcessMemory), reinterpret_cast<void**>(&g_readProcessMemoryOriginal) },
            { L"KernelBase.dll", "SuspendThread", ks::winapi_monitor::EventCategory::Process, &g_suspendThreadHook, reinterpret_cast<void*>(&HookedSuspendThread), reinterpret_cast<void**>(&g_suspendThreadOriginal) },
            { L"KernelBase.dll", "ResumeThread", ks::winapi_monitor::EventCategory::Process, &g_resumeThreadHook, reinterpret_cast<void*>(&HookedResumeThread), reinterpret_cast<void**>(&g_resumeThreadOriginal) },
            { L"KernelBase.dll", "QueueUserAPC", ks::winapi_monitor::EventCategory::Process, &g_queueUserAPCHook, reinterpret_cast<void*>(&HookedQueueUserAPC), reinterpret_cast<void**>(&g_queueUserAPCOriginal) },
            { L"KernelBase.dll", "GetThreadContext", ks::winapi_monitor::EventCategory::Process, &g_getThreadContextHook, reinterpret_cast<void*>(&HookedGetThreadContext), reinterpret_cast<void**>(&g_getThreadContextOriginal) },
            { L"KernelBase.dll", "SetThreadContext", ks::winapi_monitor::EventCategory::Process, &g_setThreadContextHook, reinterpret_cast<void*>(&HookedSetThreadContext), reinterpret_cast<void**>(&g_setThreadContextOriginal) },
            { L"KernelBase.dll", "CloseHandle", ks::winapi_monitor::EventCategory::Process, &g_closeHandleHook, reinterpret_cast<void*>(&HookedCloseHandle), reinterpret_cast<void**>(&g_closeHandleOriginal) },
            { L"KernelBase.dll", "DuplicateHandle", ks::winapi_monitor::EventCategory::Process, &g_duplicateHandleHook, reinterpret_cast<void*>(&HookedDuplicateHandle), reinterpret_cast<void**>(&g_duplicateHandleOriginal) },
            { L"KernelBase.dll", "MapViewOfFile", ks::winapi_monitor::EventCategory::Process, &g_mapViewOfFileHook, reinterpret_cast<void*>(&HookedMapViewOfFile), reinterpret_cast<void**>(&g_mapViewOfFileOriginal) },
            { L"KernelBase.dll", "MapViewOfFileEx", ks::winapi_monitor::EventCategory::Process, &g_mapViewOfFileExHook, reinterpret_cast<void*>(&HookedMapViewOfFileEx), reinterpret_cast<void**>(&g_mapViewOfFileExOriginal) },
            { L"KernelBase.dll", "UnmapViewOfFile", ks::winapi_monitor::EventCategory::Process, &g_unmapViewOfFileHook, reinterpret_cast<void*>(&HookedUnmapViewOfFile), reinterpret_cast<void**>(&g_unmapViewOfFileOriginal) },
            { L"KernelBase.dll", "CreateMutexW", ks::winapi_monitor::EventCategory::Process, &g_createMutexWHook, reinterpret_cast<void*>(&HookedCreateMutexW), reinterpret_cast<void**>(&g_createMutexWOriginal) },
            { L"KernelBase.dll", "CreateMutexA", ks::winapi_monitor::EventCategory::Process, &g_createMutexAHook, reinterpret_cast<void*>(&HookedCreateMutexA), reinterpret_cast<void**>(&g_createMutexAOriginal) },
            { L"KernelBase.dll", "OpenMutexW", ks::winapi_monitor::EventCategory::Process, &g_openMutexWHook, reinterpret_cast<void*>(&HookedOpenMutexW), reinterpret_cast<void**>(&g_openMutexWOriginal) },
            { L"KernelBase.dll", "OpenMutexA", ks::winapi_monitor::EventCategory::Process, &g_openMutexAHook, reinterpret_cast<void*>(&HookedOpenMutexA), reinterpret_cast<void**>(&g_openMutexAOriginal) },
            { L"KernelBase.dll", "CreateEventW", ks::winapi_monitor::EventCategory::Process, &g_createEventWHook, reinterpret_cast<void*>(&HookedCreateEventW), reinterpret_cast<void**>(&g_createEventWOriginal) },
            { L"KernelBase.dll", "CreateEventA", ks::winapi_monitor::EventCategory::Process, &g_createEventAHook, reinterpret_cast<void*>(&HookedCreateEventA), reinterpret_cast<void**>(&g_createEventAOriginal) },
            { L"KernelBase.dll", "OpenEventW", ks::winapi_monitor::EventCategory::Process, &g_openEventWHook, reinterpret_cast<void*>(&HookedOpenEventW), reinterpret_cast<void**>(&g_openEventWOriginal) },
            { L"KernelBase.dll", "OpenEventA", ks::winapi_monitor::EventCategory::Process, &g_openEventAHook, reinterpret_cast<void*>(&HookedOpenEventA), reinterpret_cast<void**>(&g_openEventAOriginal) },
            { L"KernelBase.dll", "CreateSemaphoreW", ks::winapi_monitor::EventCategory::Process, &g_createSemaphoreWHook, reinterpret_cast<void*>(&HookedCreateSemaphoreW), reinterpret_cast<void**>(&g_createSemaphoreWOriginal) },
            { L"KernelBase.dll", "CreateSemaphoreA", ks::winapi_monitor::EventCategory::Process, &g_createSemaphoreAHook, reinterpret_cast<void*>(&HookedCreateSemaphoreA), reinterpret_cast<void**>(&g_createSemaphoreAOriginal) },
            { L"KernelBase.dll", "OpenSemaphoreW", ks::winapi_monitor::EventCategory::Process, &g_openSemaphoreWHook, reinterpret_cast<void*>(&HookedOpenSemaphoreW), reinterpret_cast<void**>(&g_openSemaphoreWOriginal) },
            { L"KernelBase.dll", "OpenSemaphoreA", ks::winapi_monitor::EventCategory::Process, &g_openSemaphoreAHook, reinterpret_cast<void*>(&HookedOpenSemaphoreA), reinterpret_cast<void**>(&g_openSemaphoreAOriginal) },
            { L"KernelBase.dll", "WaitForSingleObject", ks::winapi_monitor::EventCategory::Process, &g_waitForSingleObjectHook, reinterpret_cast<void*>(&HookedWaitForSingleObject), reinterpret_cast<void**>(&g_waitForSingleObjectOriginal) },
            { L"KernelBase.dll", "WaitForMultipleObjects", ks::winapi_monitor::EventCategory::Process, &g_waitForMultipleObjectsHook, reinterpret_cast<void*>(&HookedWaitForMultipleObjects), reinterpret_cast<void**>(&g_waitForMultipleObjectsOriginal) },
            { L"KernelBase.dll", "SetEvent", ks::winapi_monitor::EventCategory::Process, &g_setEventHook, reinterpret_cast<void*>(&HookedSetEvent), reinterpret_cast<void**>(&g_setEventOriginal) },
            { L"KernelBase.dll", "ResetEvent", ks::winapi_monitor::EventCategory::Process, &g_resetEventHook, reinterpret_cast<void*>(&HookedResetEvent), reinterpret_cast<void**>(&g_resetEventOriginal) },
            { L"KernelBase.dll", "ReleaseMutex", ks::winapi_monitor::EventCategory::Process, &g_releaseMutexHook, reinterpret_cast<void*>(&HookedReleaseMutex), reinterpret_cast<void**>(&g_releaseMutexOriginal) },
            { L"KernelBase.dll", "ReleaseSemaphore", ks::winapi_monitor::EventCategory::Process, &g_releaseSemaphoreHook, reinterpret_cast<void*>(&HookedReleaseSemaphore), reinterpret_cast<void**>(&g_releaseSemaphoreOriginal) },
            { L"KernelBase.dll", "GetEnvironmentVariableW", ks::winapi_monitor::EventCategory::Process, &g_getEnvironmentVariableWHook, reinterpret_cast<void*>(&HookedGetEnvironmentVariableW), reinterpret_cast<void**>(&g_getEnvironmentVariableWOriginal) },
            { L"KernelBase.dll", "GetEnvironmentVariableA", ks::winapi_monitor::EventCategory::Process, &g_getEnvironmentVariableAHook, reinterpret_cast<void*>(&HookedGetEnvironmentVariableA), reinterpret_cast<void**>(&g_getEnvironmentVariableAOriginal) },
            { L"KernelBase.dll", "SetEnvironmentVariableW", ks::winapi_monitor::EventCategory::Process, &g_setEnvironmentVariableWHook, reinterpret_cast<void*>(&HookedSetEnvironmentVariableW), reinterpret_cast<void**>(&g_setEnvironmentVariableWOriginal) },
            { L"KernelBase.dll", "SetEnvironmentVariableA", ks::winapi_monitor::EventCategory::Process, &g_setEnvironmentVariableAHook, reinterpret_cast<void*>(&HookedSetEnvironmentVariableA), reinterpret_cast<void**>(&g_setEnvironmentVariableAOriginal) },
            { L"KernelBase.dll", "ExpandEnvironmentStringsW", ks::winapi_monitor::EventCategory::Process, &g_expandEnvironmentStringsWHook, reinterpret_cast<void*>(&HookedExpandEnvironmentStringsW), reinterpret_cast<void**>(&g_expandEnvironmentStringsWOriginal) },
            { L"KernelBase.dll", "ExpandEnvironmentStringsA", ks::winapi_monitor::EventCategory::Process, &g_expandEnvironmentStringsAHook, reinterpret_cast<void*>(&HookedExpandEnvironmentStringsA), reinterpret_cast<void**>(&g_expandEnvironmentStringsAOriginal) },
            { L"Kernel32.dll", "WinExec", ks::winapi_monitor::EventCategory::Process, &g_winExecHook, reinterpret_cast<void*>(&HookedWinExec), reinterpret_cast<void**>(&g_winExecOriginal) },
            { L"Kernel32.dll", "CreateToolhelp32Snapshot", ks::winapi_monitor::EventCategory::Process, &g_createToolhelp32SnapshotHook, reinterpret_cast<void*>(&HookedCreateToolhelp32Snapshot), reinterpret_cast<void**>(&g_createToolhelp32SnapshotOriginal) },
            { L"Shell32.dll", "ShellExecuteExW", ks::winapi_monitor::EventCategory::Process, &g_shellExecuteExWHook, reinterpret_cast<void*>(&HookedShellExecuteExW), reinterpret_cast<void**>(&g_shellExecuteExWOriginal) },
            { L"Shell32.dll", "ShellExecuteExA", ks::winapi_monitor::EventCategory::Process, &g_shellExecuteExAHook, reinterpret_cast<void*>(&HookedShellExecuteExA), reinterpret_cast<void**>(&g_shellExecuteExAOriginal) },
            { L"Shell32.dll", "ShellExecuteW", ks::winapi_monitor::EventCategory::Process, &g_shellExecuteWHook, reinterpret_cast<void*>(&HookedShellExecuteW), reinterpret_cast<void**>(&g_shellExecuteWOriginal) },
            { L"Shell32.dll", "ShellExecuteA", ks::winapi_monitor::EventCategory::Process, &g_shellExecuteAHook, reinterpret_cast<void*>(&HookedShellExecuteA), reinterpret_cast<void**>(&g_shellExecuteAOriginal) },
            { L"Advapi32.dll", "OpenProcessToken", ks::winapi_monitor::EventCategory::Process, &g_openProcessTokenHook, reinterpret_cast<void*>(&HookedOpenProcessToken), reinterpret_cast<void**>(&g_openProcessTokenOriginal) },
            { L"Advapi32.dll", "OpenThreadToken", ks::winapi_monitor::EventCategory::Process, &g_openThreadTokenHook, reinterpret_cast<void*>(&HookedOpenThreadToken), reinterpret_cast<void**>(&g_openThreadTokenOriginal) },
            { L"Advapi32.dll", "AdjustTokenPrivileges", ks::winapi_monitor::EventCategory::Process, &g_adjustTokenPrivilegesHook, reinterpret_cast<void*>(&HookedAdjustTokenPrivileges), reinterpret_cast<void**>(&g_adjustTokenPrivilegesOriginal) },
            { L"Advapi32.dll", "DuplicateToken", ks::winapi_monitor::EventCategory::Process, &g_duplicateTokenHook, reinterpret_cast<void*>(&HookedDuplicateToken), reinterpret_cast<void**>(&g_duplicateTokenOriginal) },
            { L"Advapi32.dll", "DuplicateTokenEx", ks::winapi_monitor::EventCategory::Process, &g_duplicateTokenExHook, reinterpret_cast<void*>(&HookedDuplicateTokenEx), reinterpret_cast<void**>(&g_duplicateTokenExOriginal) },
            { L"Advapi32.dll", "CreateProcessAsUserW", ks::winapi_monitor::EventCategory::Process, &g_createProcessAsUserWHook, reinterpret_cast<void*>(&HookedCreateProcessAsUserW), reinterpret_cast<void**>(&g_createProcessAsUserWOriginal) },
            { L"Advapi32.dll", "CreateProcessAsUserA", ks::winapi_monitor::EventCategory::Process, &g_createProcessAsUserAHook, reinterpret_cast<void*>(&HookedCreateProcessAsUserA), reinterpret_cast<void**>(&g_createProcessAsUserAOriginal) },
            { L"Advapi32.dll", "CreateProcessWithTokenW", ks::winapi_monitor::EventCategory::Process, &g_createProcessWithTokenWHook, reinterpret_cast<void*>(&HookedCreateProcessWithTokenW), reinterpret_cast<void**>(&g_createProcessWithTokenWOriginal) },
            { L"Advapi32.dll", "CreateProcessWithLogonW", ks::winapi_monitor::EventCategory::Process, &g_createProcessWithLogonWHook, reinterpret_cast<void*>(&HookedCreateProcessWithLogonW), reinterpret_cast<void**>(&g_createProcessWithLogonWOriginal) },
            { L"Advapi32.dll", "ImpersonateLoggedOnUser", ks::winapi_monitor::EventCategory::Process, &g_impersonateLoggedOnUserHook, reinterpret_cast<void*>(&HookedImpersonateLoggedOnUser), reinterpret_cast<void**>(&g_impersonateLoggedOnUserOriginal) },
            { L"Advapi32.dll", "RevertToSelf", ks::winapi_monitor::EventCategory::Process, &g_revertToSelfHook, reinterpret_cast<void*>(&HookedRevertToSelf), reinterpret_cast<void**>(&g_revertToSelfOriginal) },
            { L"Advapi32.dll", "SetThreadToken", ks::winapi_monitor::EventCategory::Process, &g_setThreadTokenHook, reinterpret_cast<void*>(&HookedSetThreadToken), reinterpret_cast<void**>(&g_setThreadTokenOriginal) },
            { L"Advapi32.dll", "LookupPrivilegeValueW", ks::winapi_monitor::EventCategory::Process, &g_lookupPrivilegeValueWHook, reinterpret_cast<void*>(&HookedLookupPrivilegeValueW), reinterpret_cast<void**>(&g_lookupPrivilegeValueWOriginal) },
            { L"Advapi32.dll", "LookupPrivilegeValueA", ks::winapi_monitor::EventCategory::Process, &g_lookupPrivilegeValueAHook, reinterpret_cast<void*>(&HookedLookupPrivilegeValueA), reinterpret_cast<void**>(&g_lookupPrivilegeValueAOriginal) },
            { L"Advapi32.dll", "OpenSCManagerW", ks::winapi_monitor::EventCategory::Process, &g_openSCManagerWHook, reinterpret_cast<void*>(&HookedOpenSCManagerW), reinterpret_cast<void**>(&g_openSCManagerWOriginal) },
            { L"Advapi32.dll", "OpenSCManagerA", ks::winapi_monitor::EventCategory::Process, &g_openSCManagerAHook, reinterpret_cast<void*>(&HookedOpenSCManagerA), reinterpret_cast<void**>(&g_openSCManagerAOriginal) },
            { L"Advapi32.dll", "OpenServiceW", ks::winapi_monitor::EventCategory::Process, &g_openServiceWHook, reinterpret_cast<void*>(&HookedOpenServiceW), reinterpret_cast<void**>(&g_openServiceWOriginal) },
            { L"Advapi32.dll", "OpenServiceA", ks::winapi_monitor::EventCategory::Process, &g_openServiceAHook, reinterpret_cast<void*>(&HookedOpenServiceA), reinterpret_cast<void**>(&g_openServiceAOriginal) },
            { L"Advapi32.dll", "CreateServiceW", ks::winapi_monitor::EventCategory::Process, &g_createServiceWHook, reinterpret_cast<void*>(&HookedCreateServiceW), reinterpret_cast<void**>(&g_createServiceWOriginal) },
            { L"Advapi32.dll", "CreateServiceA", ks::winapi_monitor::EventCategory::Process, &g_createServiceAHook, reinterpret_cast<void*>(&HookedCreateServiceA), reinterpret_cast<void**>(&g_createServiceAOriginal) },
            { L"Advapi32.dll", "ChangeServiceConfigW", ks::winapi_monitor::EventCategory::Process, &g_changeServiceConfigWHook, reinterpret_cast<void*>(&HookedChangeServiceConfigW), reinterpret_cast<void**>(&g_changeServiceConfigWOriginal) },
            { L"Advapi32.dll", "ChangeServiceConfigA", ks::winapi_monitor::EventCategory::Process, &g_changeServiceConfigAHook, reinterpret_cast<void*>(&HookedChangeServiceConfigA), reinterpret_cast<void**>(&g_changeServiceConfigAOriginal) },
            { L"Advapi32.dll", "ChangeServiceConfig2W", ks::winapi_monitor::EventCategory::Process, &g_changeServiceConfig2WHook, reinterpret_cast<void*>(&HookedChangeServiceConfig2W), reinterpret_cast<void**>(&g_changeServiceConfig2WOriginal) },
            { L"Advapi32.dll", "ChangeServiceConfig2A", ks::winapi_monitor::EventCategory::Process, &g_changeServiceConfig2AHook, reinterpret_cast<void*>(&HookedChangeServiceConfig2A), reinterpret_cast<void**>(&g_changeServiceConfig2AOriginal) },
            { L"Advapi32.dll", "StartServiceW", ks::winapi_monitor::EventCategory::Process, &g_startServiceWHook, reinterpret_cast<void*>(&HookedStartServiceW), reinterpret_cast<void**>(&g_startServiceWOriginal) },
            { L"Advapi32.dll", "StartServiceA", ks::winapi_monitor::EventCategory::Process, &g_startServiceAHook, reinterpret_cast<void*>(&HookedStartServiceA), reinterpret_cast<void**>(&g_startServiceAOriginal) },
            { L"Advapi32.dll", "ControlService", ks::winapi_monitor::EventCategory::Process, &g_controlServiceHook, reinterpret_cast<void*>(&HookedControlService), reinterpret_cast<void**>(&g_controlServiceOriginal) },
            { L"Advapi32.dll", "DeleteService", ks::winapi_monitor::EventCategory::Process, &g_deleteServiceHook, reinterpret_cast<void*>(&HookedDeleteService), reinterpret_cast<void**>(&g_deleteServiceOriginal) },
            { L"Advapi32.dll", "CloseServiceHandle", ks::winapi_monitor::EventCategory::Process, &g_closeServiceHandleHook, reinterpret_cast<void*>(&HookedCloseServiceHandle), reinterpret_cast<void**>(&g_closeServiceHandleOriginal) },
            { L"Advapi32.dll", "QueryServiceStatusEx", ks::winapi_monitor::EventCategory::Process, &g_queryServiceStatusExHook, reinterpret_cast<void*>(&HookedQueryServiceStatusEx), reinterpret_cast<void**>(&g_queryServiceStatusExOriginal) },
            { L"Advapi32.dll", "QueryServiceConfigW", ks::winapi_monitor::EventCategory::Process, &g_queryServiceConfigWHook, reinterpret_cast<void*>(&HookedQueryServiceConfigW), reinterpret_cast<void**>(&g_queryServiceConfigWOriginal) },
            { L"Advapi32.dll", "QueryServiceConfigA", ks::winapi_monitor::EventCategory::Process, &g_queryServiceConfigAHook, reinterpret_cast<void*>(&HookedQueryServiceConfigA), reinterpret_cast<void**>(&g_queryServiceConfigAOriginal) },
            { L"Advapi32.dll", "EnumServicesStatusExW", ks::winapi_monitor::EventCategory::Process, &g_enumServicesStatusExWHook, reinterpret_cast<void*>(&HookedEnumServicesStatusExW), reinterpret_cast<void**>(&g_enumServicesStatusExWOriginal) },
            { L"Advapi32.dll", "EnumServicesStatusExA", ks::winapi_monitor::EventCategory::Process, &g_enumServicesStatusExAHook, reinterpret_cast<void*>(&HookedEnumServicesStatusExA), reinterpret_cast<void**>(&g_enumServicesStatusExAOriginal) },
            { L"Advapi32.dll", "CryptAcquireContextW", ks::winapi_monitor::EventCategory::Process, &g_cryptAcquireContextWHook, reinterpret_cast<void*>(&HookedCryptAcquireContextW), reinterpret_cast<void**>(&g_cryptAcquireContextWOriginal) },
            { L"Advapi32.dll", "CryptAcquireContextA", ks::winapi_monitor::EventCategory::Process, &g_cryptAcquireContextAHook, reinterpret_cast<void*>(&HookedCryptAcquireContextA), reinterpret_cast<void**>(&g_cryptAcquireContextAOriginal) },
            { L"Advapi32.dll", "CryptCreateHash", ks::winapi_monitor::EventCategory::Process, &g_cryptCreateHashHook, reinterpret_cast<void*>(&HookedCryptCreateHash), reinterpret_cast<void**>(&g_cryptCreateHashOriginal) },
            { L"Advapi32.dll", "CryptHashData", ks::winapi_monitor::EventCategory::Process, &g_cryptHashDataHook, reinterpret_cast<void*>(&HookedCryptHashData), reinterpret_cast<void**>(&g_cryptHashDataOriginal) },
            { L"Advapi32.dll", "CryptDeriveKey", ks::winapi_monitor::EventCategory::Process, &g_cryptDeriveKeyHook, reinterpret_cast<void*>(&HookedCryptDeriveKey), reinterpret_cast<void**>(&g_cryptDeriveKeyOriginal) },
            { L"Advapi32.dll", "CryptEncrypt", ks::winapi_monitor::EventCategory::Process, &g_cryptEncryptHook, reinterpret_cast<void*>(&HookedCryptEncrypt), reinterpret_cast<void**>(&g_cryptEncryptOriginal) },
            { L"Advapi32.dll", "CryptDecrypt", ks::winapi_monitor::EventCategory::Process, &g_cryptDecryptHook, reinterpret_cast<void*>(&HookedCryptDecrypt), reinterpret_cast<void**>(&g_cryptDecryptOriginal) },
            { L"Advapi32.dll", "CryptGenRandom", ks::winapi_monitor::EventCategory::Process, &g_cryptGenRandomHook, reinterpret_cast<void*>(&HookedCryptGenRandom), reinterpret_cast<void**>(&g_cryptGenRandomOriginal) },
            { L"Advapi32.dll", "CryptReleaseContext", ks::winapi_monitor::EventCategory::Process, &g_cryptReleaseContextHook, reinterpret_cast<void*>(&HookedCryptReleaseContext), reinterpret_cast<void**>(&g_cryptReleaseContextOriginal) },
            { L"Advapi32.dll", "CryptImportKey", ks::winapi_monitor::EventCategory::Process, &g_cryptImportKeyHook, reinterpret_cast<void*>(&HookedCryptImportKey), reinterpret_cast<void**>(&g_cryptImportKeyOriginal) },
            { L"Advapi32.dll", "CryptExportKey", ks::winapi_monitor::EventCategory::Process, &g_cryptExportKeyHook, reinterpret_cast<void*>(&HookedCryptExportKey), reinterpret_cast<void**>(&g_cryptExportKeyOriginal) },
            { L"Advapi32.dll", "CryptDestroyKey", ks::winapi_monitor::EventCategory::Process, &g_cryptDestroyKeyHook, reinterpret_cast<void*>(&HookedCryptDestroyKey), reinterpret_cast<void**>(&g_cryptDestroyKeyOriginal) },
            { L"Advapi32.dll", "CryptDestroyHash", ks::winapi_monitor::EventCategory::Process, &g_cryptDestroyHashHook, reinterpret_cast<void*>(&HookedCryptDestroyHash), reinterpret_cast<void**>(&g_cryptDestroyHashOriginal) },
            { L"Kernel32.dll", "LoadLibraryA", ks::winapi_monitor::EventCategory::Loader, &g_loadLibraryAHook, reinterpret_cast<void*>(&HookedLoadLibraryA), reinterpret_cast<void**>(&g_loadLibraryAOriginal) },
            { L"Kernel32.dll", "LoadLibraryW", ks::winapi_monitor::EventCategory::Loader, &g_loadLibraryWHook, reinterpret_cast<void*>(&HookedLoadLibraryW), reinterpret_cast<void**>(&g_loadLibraryWOriginal) },
            { L"Kernel32.dll", "LoadLibraryExA", ks::winapi_monitor::EventCategory::Loader, &g_loadLibraryExAHook, reinterpret_cast<void*>(&HookedLoadLibraryExA), reinterpret_cast<void**>(&g_loadLibraryExAOriginal) },
            { L"Kernel32.dll", "LoadLibraryExW", ks::winapi_monitor::EventCategory::Loader, &g_loadLibraryExWHook, reinterpret_cast<void*>(&HookedLoadLibraryExW), reinterpret_cast<void**>(&g_loadLibraryExWOriginal) },
            { L"Kernel32.dll", "FreeLibrary", ks::winapi_monitor::EventCategory::Loader, &g_freeLibraryHook, reinterpret_cast<void*>(&HookedFreeLibrary), reinterpret_cast<void**>(&g_freeLibraryOriginal) },
            { L"Kernel32.dll", "GetProcAddress", ks::winapi_monitor::EventCategory::Loader, &g_getProcAddressHook, reinterpret_cast<void*>(&HookedGetProcAddress), reinterpret_cast<void**>(&g_getProcAddressOriginal) },
            { L"Kernel32.dll", "SetDllDirectoryW", ks::winapi_monitor::EventCategory::Loader, &g_setDllDirectoryWHook, reinterpret_cast<void*>(&HookedSetDllDirectoryW), reinterpret_cast<void**>(&g_setDllDirectoryWOriginal) },
            { L"Kernel32.dll", "SetDllDirectoryA", ks::winapi_monitor::EventCategory::Loader, &g_setDllDirectoryAHook, reinterpret_cast<void*>(&HookedSetDllDirectoryA), reinterpret_cast<void**>(&g_setDllDirectoryAOriginal) },
            { L"Kernel32.dll", "SetDefaultDllDirectories", ks::winapi_monitor::EventCategory::Loader, &g_setDefaultDllDirectoriesHook, reinterpret_cast<void*>(&HookedSetDefaultDllDirectories), reinterpret_cast<void**>(&g_setDefaultDllDirectoriesOriginal) },
            { L"Kernel32.dll", "AddDllDirectory", ks::winapi_monitor::EventCategory::Loader, &g_addDllDirectoryHook, reinterpret_cast<void*>(&HookedAddDllDirectory), reinterpret_cast<void**>(&g_addDllDirectoryOriginal) },
            { L"Kernel32.dll", "RemoveDllDirectory", ks::winapi_monitor::EventCategory::Loader, &g_removeDllDirectoryHook, reinterpret_cast<void*>(&HookedRemoveDllDirectory), reinterpret_cast<void**>(&g_removeDllDirectoryOriginal) },
            { L"Kernel32.dll", "Module32FirstW", ks::winapi_monitor::EventCategory::Loader, &g_module32FirstWHook, reinterpret_cast<void*>(&HookedModule32FirstW), reinterpret_cast<void**>(&g_module32FirstWOriginal) },
            { L"Kernel32.dll", "Module32NextW", ks::winapi_monitor::EventCategory::Loader, &g_module32NextWHook, reinterpret_cast<void*>(&HookedModule32NextW), reinterpret_cast<void**>(&g_module32NextWOriginal) },
            { L"Kernel32.dll", "Module32First", ks::winapi_monitor::EventCategory::Loader, &g_module32FirstAHook, reinterpret_cast<void*>(&HookedModule32FirstA), reinterpret_cast<void**>(&g_module32FirstAOriginal) },
            { L"Kernel32.dll", "Module32Next", ks::winapi_monitor::EventCategory::Loader, &g_module32NextAHook, reinterpret_cast<void*>(&HookedModule32NextA), reinterpret_cast<void**>(&g_module32NextAOriginal) },
            { L"Kernel32.dll", "GetModuleHandleW", ks::winapi_monitor::EventCategory::Loader, &g_getModuleHandleWHook, reinterpret_cast<void*>(&HookedGetModuleHandleW), reinterpret_cast<void**>(&g_getModuleHandleWOriginal) },
            { L"Kernel32.dll", "GetModuleHandleA", ks::winapi_monitor::EventCategory::Loader, &g_getModuleHandleAHook, reinterpret_cast<void*>(&HookedGetModuleHandleA), reinterpret_cast<void**>(&g_getModuleHandleAOriginal) },
            { L"Kernel32.dll", "GetModuleHandleExW", ks::winapi_monitor::EventCategory::Loader, &g_getModuleHandleExWHook, reinterpret_cast<void*>(&HookedGetModuleHandleExW), reinterpret_cast<void**>(&g_getModuleHandleExWOriginal) },
            { L"Kernel32.dll", "GetModuleHandleExA", ks::winapi_monitor::EventCategory::Loader, &g_getModuleHandleExAHook, reinterpret_cast<void*>(&HookedGetModuleHandleExA), reinterpret_cast<void**>(&g_getModuleHandleExAOriginal) },
            { L"Kernel32.dll", "GetModuleFileNameW", ks::winapi_monitor::EventCategory::Loader, &g_getModuleFileNameWHook, reinterpret_cast<void*>(&HookedGetModuleFileNameW), reinterpret_cast<void**>(&g_getModuleFileNameWOriginal) },
            { L"Kernel32.dll", "GetModuleFileNameA", ks::winapi_monitor::EventCategory::Loader, &g_getModuleFileNameAHook, reinterpret_cast<void*>(&HookedGetModuleFileNameA), reinterpret_cast<void**>(&g_getModuleFileNameAOriginal) },
            { L"ntdll.dll", "LdrLoadDll", ks::winapi_monitor::EventCategory::Loader, &g_ldrLoadDllHook, reinterpret_cast<void*>(&HookedLdrLoadDll), reinterpret_cast<void**>(&g_ldrLoadDllOriginal) },
            { L"ntdll.dll", "LdrGetProcedureAddress", ks::winapi_monitor::EventCategory::Loader, &g_ldrGetProcedureAddressHook, reinterpret_cast<void*>(&HookedLdrGetProcedureAddress), reinterpret_cast<void**>(&g_ldrGetProcedureAddressOriginal) },
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
            { L"Advapi32.dll", "RegCreateKeyTransactedW", ks::winapi_monitor::EventCategory::Registry, &g_regCreateKeyTransactedWHook, reinterpret_cast<void*>(&HookedRegCreateKeyTransactedW), reinterpret_cast<void**>(&g_regCreateKeyTransactedWOriginal) },
            { L"Advapi32.dll", "RegCreateKeyTransactedA", ks::winapi_monitor::EventCategory::Registry, &g_regCreateKeyTransactedAHook, reinterpret_cast<void*>(&HookedRegCreateKeyTransactedA), reinterpret_cast<void**>(&g_regCreateKeyTransactedAOriginal) },
            { L"Advapi32.dll", "RegOpenKeyTransactedW", ks::winapi_monitor::EventCategory::Registry, &g_regOpenKeyTransactedWHook, reinterpret_cast<void*>(&HookedRegOpenKeyTransactedW), reinterpret_cast<void**>(&g_regOpenKeyTransactedWOriginal) },
            { L"Advapi32.dll", "RegOpenKeyTransactedA", ks::winapi_monitor::EventCategory::Registry, &g_regOpenKeyTransactedAHook, reinterpret_cast<void*>(&HookedRegOpenKeyTransactedA), reinterpret_cast<void**>(&g_regOpenKeyTransactedAOriginal) },
            { L"Advapi32.dll", "RegDeleteKeyTransactedW", ks::winapi_monitor::EventCategory::Registry, &g_regDeleteKeyTransactedWHook, reinterpret_cast<void*>(&HookedRegDeleteKeyTransactedW), reinterpret_cast<void**>(&g_regDeleteKeyTransactedWOriginal) },
            { L"Advapi32.dll", "RegDeleteKeyTransactedA", ks::winapi_monitor::EventCategory::Registry, &g_regDeleteKeyTransactedAHook, reinterpret_cast<void*>(&HookedRegDeleteKeyTransactedA), reinterpret_cast<void**>(&g_regDeleteKeyTransactedAOriginal) },
            { L"Advapi32.dll", "RegReplaceKeyW", ks::winapi_monitor::EventCategory::Registry, &g_regReplaceKeyWHook, reinterpret_cast<void*>(&HookedRegReplaceKeyW), reinterpret_cast<void**>(&g_regReplaceKeyWOriginal) },
            { L"Advapi32.dll", "RegReplaceKeyA", ks::winapi_monitor::EventCategory::Registry, &g_regReplaceKeyAHook, reinterpret_cast<void*>(&HookedRegReplaceKeyA), reinterpret_cast<void**>(&g_regReplaceKeyAOriginal) },
            { L"Advapi32.dll", "RegRestoreKeyW", ks::winapi_monitor::EventCategory::Registry, &g_regRestoreKeyWHook, reinterpret_cast<void*>(&HookedRegRestoreKeyW), reinterpret_cast<void**>(&g_regRestoreKeyWOriginal) },
            { L"Advapi32.dll", "RegRestoreKeyA", ks::winapi_monitor::EventCategory::Registry, &g_regRestoreKeyAHook, reinterpret_cast<void*>(&HookedRegRestoreKeyA), reinterpret_cast<void**>(&g_regRestoreKeyAOriginal) },
            { L"Advapi32.dll", "RegUnLoadKeyW", ks::winapi_monitor::EventCategory::Registry, &g_regUnLoadKeyWHook, reinterpret_cast<void*>(&HookedRegUnLoadKeyW), reinterpret_cast<void**>(&g_regUnLoadKeyWOriginal) },
            { L"Advapi32.dll", "RegUnLoadKeyA", ks::winapi_monitor::EventCategory::Registry, &g_regUnLoadKeyAHook, reinterpret_cast<void*>(&HookedRegUnLoadKeyA), reinterpret_cast<void**>(&g_regUnLoadKeyAOriginal) },
            { L"Advapi32.dll", "RegLoadAppKeyW", ks::winapi_monitor::EventCategory::Registry, &g_regLoadAppKeyWHook, reinterpret_cast<void*>(&HookedRegLoadAppKeyW), reinterpret_cast<void**>(&g_regLoadAppKeyWOriginal) },
            { L"Advapi32.dll", "RegLoadAppKeyA", ks::winapi_monitor::EventCategory::Registry, &g_regLoadAppKeyAHook, reinterpret_cast<void*>(&HookedRegLoadAppKeyA), reinterpret_cast<void**>(&g_regLoadAppKeyAOriginal) },
            { L"Advapi32.dll", "RegNotifyChangeKeyValue", ks::winapi_monitor::EventCategory::Registry, &g_regNotifyChangeKeyValueHook, reinterpret_cast<void*>(&HookedRegNotifyChangeKeyValue), reinterpret_cast<void**>(&g_regNotifyChangeKeyValueOriginal) },
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
            { L"ntdll.dll", "NtCreateKeyTransacted", ks::winapi_monitor::EventCategory::Registry, &g_ntCreateKeyTransactedHook, reinterpret_cast<void*>(&HookedNtCreateKeyTransacted), reinterpret_cast<void**>(&g_ntCreateKeyTransactedOriginal) },
            { L"ntdll.dll", "NtOpenKeyTransacted", ks::winapi_monitor::EventCategory::Registry, &g_ntOpenKeyTransactedHook, reinterpret_cast<void*>(&HookedNtOpenKeyTransacted), reinterpret_cast<void**>(&g_ntOpenKeyTransactedOriginal) },
            { L"ntdll.dll", "NtOpenKeyTransactedEx", ks::winapi_monitor::EventCategory::Registry, &g_ntOpenKeyTransactedExHook, reinterpret_cast<void*>(&HookedNtOpenKeyTransactedEx), reinterpret_cast<void**>(&g_ntOpenKeyTransactedExOriginal) },
            { L"ntdll.dll", "NtReplaceKey", ks::winapi_monitor::EventCategory::Registry, &g_ntReplaceKeyHook, reinterpret_cast<void*>(&HookedNtReplaceKey), reinterpret_cast<void**>(&g_ntReplaceKeyOriginal) },
            { L"ntdll.dll", "NtRestoreKey", ks::winapi_monitor::EventCategory::Registry, &g_ntRestoreKeyHook, reinterpret_cast<void*>(&HookedNtRestoreKey), reinterpret_cast<void**>(&g_ntRestoreKeyOriginal) },
            { L"ntdll.dll", "NtUnloadKey", ks::winapi_monitor::EventCategory::Registry, &g_ntUnloadKeyHook, reinterpret_cast<void*>(&HookedNtUnloadKey), reinterpret_cast<void**>(&g_ntUnloadKeyOriginal) },
            { L"ntdll.dll", "NtUnloadKey2", ks::winapi_monitor::EventCategory::Registry, &g_ntUnloadKey2Hook, reinterpret_cast<void*>(&HookedNtUnloadKey2), reinterpret_cast<void**>(&g_ntUnloadKey2Original) },
            { L"ntdll.dll", "NtUnloadKeyEx", ks::winapi_monitor::EventCategory::Registry, &g_ntUnloadKeyExHook, reinterpret_cast<void*>(&HookedNtUnloadKeyEx), reinterpret_cast<void**>(&g_ntUnloadKeyExOriginal) },
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
            { L"ntdll.dll", "NtClose", ks::winapi_monitor::EventCategory::Process, &g_ntCloseHook, reinterpret_cast<void*>(&HookedNtClose), reinterpret_cast<void**>(&g_ntCloseOriginal) },
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
            { L"Ws2_32.dll", "accept", ks::winapi_monitor::EventCategory::Network, &g_acceptHook, reinterpret_cast<void*>(&HookedAccept), reinterpret_cast<void**>(&g_acceptOriginal) },
            { L"Ws2_32.dll", "WSAIoctl", ks::winapi_monitor::EventCategory::Network, &g_wsaIoctlHook, reinterpret_cast<void*>(&HookedWSAIoctl), reinterpret_cast<void**>(&g_wsaIoctlOriginal) },
            { L"Ws2_32.dll", "WSASendTo", ks::winapi_monitor::EventCategory::Network, &g_wsaSendToHook, reinterpret_cast<void*>(&HookedWSASendTo), reinterpret_cast<void**>(&g_wsaSendToOriginal) },
            { L"Ws2_32.dll", "WSARecvFrom", ks::winapi_monitor::EventCategory::Network, &g_wsaRecvFromHook, reinterpret_cast<void*>(&HookedWSARecvFrom), reinterpret_cast<void**>(&g_wsaRecvFromOriginal) },
            { L"Ws2_32.dll", "GetAddrInfoW", ks::winapi_monitor::EventCategory::Network, &g_getAddrInfoWHook, reinterpret_cast<void*>(&HookedGetAddrInfoW), reinterpret_cast<void**>(&g_getAddrInfoWOriginal) },
            { L"Ws2_32.dll", "getaddrinfo", ks::winapi_monitor::EventCategory::Network, &g_getAddrInfoAHook, reinterpret_cast<void*>(&HookedGetAddrInfoA), reinterpret_cast<void**>(&g_getAddrInfoAOriginal) },
            { L"Dnsapi.dll", "DnsQuery_W", ks::winapi_monitor::EventCategory::Network, &g_dnsQueryWHook, reinterpret_cast<void*>(&HookedDnsQueryW), reinterpret_cast<void**>(&g_dnsQueryWOriginal) },
            { L"Dnsapi.dll", "DnsQuery_A", ks::winapi_monitor::EventCategory::Network, &g_dnsQueryAHook, reinterpret_cast<void*>(&HookedDnsQueryA), reinterpret_cast<void**>(&g_dnsQueryAOriginal) },
            { L"Winhttp.dll", "WinHttpOpen", ks::winapi_monitor::EventCategory::Network, &g_winHttpOpenHook, reinterpret_cast<void*>(&HookedWinHttpOpen), reinterpret_cast<void**>(&g_winHttpOpenOriginal) },
            { L"Winhttp.dll", "WinHttpConnect", ks::winapi_monitor::EventCategory::Network, &g_winHttpConnectHook, reinterpret_cast<void*>(&HookedWinHttpConnect), reinterpret_cast<void**>(&g_winHttpConnectOriginal) },
            { L"Winhttp.dll", "WinHttpOpenRequest", ks::winapi_monitor::EventCategory::Network, &g_winHttpOpenRequestHook, reinterpret_cast<void*>(&HookedWinHttpOpenRequest), reinterpret_cast<void**>(&g_winHttpOpenRequestOriginal) },
            { L"Winhttp.dll", "WinHttpSendRequest", ks::winapi_monitor::EventCategory::Network, &g_winHttpSendRequestHook, reinterpret_cast<void*>(&HookedWinHttpSendRequest), reinterpret_cast<void**>(&g_winHttpSendRequestOriginal) },
            { L"Winhttp.dll", "WinHttpReceiveResponse", ks::winapi_monitor::EventCategory::Network, &g_winHttpReceiveResponseHook, reinterpret_cast<void*>(&HookedWinHttpReceiveResponse), reinterpret_cast<void**>(&g_winHttpReceiveResponseOriginal) },
            { L"Winhttp.dll", "WinHttpReadData", ks::winapi_monitor::EventCategory::Network, &g_winHttpReadDataHook, reinterpret_cast<void*>(&HookedWinHttpReadData), reinterpret_cast<void**>(&g_winHttpReadDataOriginal) },
            { L"Winhttp.dll", "WinHttpWriteData", ks::winapi_monitor::EventCategory::Network, &g_winHttpWriteDataHook, reinterpret_cast<void*>(&HookedWinHttpWriteData), reinterpret_cast<void**>(&g_winHttpWriteDataOriginal) },
            { L"Winhttp.dll", "WinHttpQueryHeaders", ks::winapi_monitor::EventCategory::Network, &g_winHttpQueryHeadersHook, reinterpret_cast<void*>(&HookedWinHttpQueryHeaders), reinterpret_cast<void**>(&g_winHttpQueryHeadersOriginal) },
            { L"Winhttp.dll", "WinHttpQueryDataAvailable", ks::winapi_monitor::EventCategory::Network, &g_winHttpQueryDataAvailableHook, reinterpret_cast<void*>(&HookedWinHttpQueryDataAvailable), reinterpret_cast<void**>(&g_winHttpQueryDataAvailableOriginal) },
            { L"Winhttp.dll", "WinHttpSetOption", ks::winapi_monitor::EventCategory::Network, &g_winHttpSetOptionHook, reinterpret_cast<void*>(&HookedWinHttpSetOption), reinterpret_cast<void**>(&g_winHttpSetOptionOriginal) },
            { L"Winhttp.dll", "WinHttpCloseHandle", ks::winapi_monitor::EventCategory::Network, &g_winHttpCloseHandleHook, reinterpret_cast<void*>(&HookedWinHttpCloseHandle), reinterpret_cast<void**>(&g_winHttpCloseHandleOriginal) },
            { L"Wininet.dll", "InternetOpenW", ks::winapi_monitor::EventCategory::Network, &g_internetOpenWHook, reinterpret_cast<void*>(&HookedInternetOpenW), reinterpret_cast<void**>(&g_internetOpenWOriginal) },
            { L"Wininet.dll", "InternetOpenA", ks::winapi_monitor::EventCategory::Network, &g_internetOpenAHook, reinterpret_cast<void*>(&HookedInternetOpenA), reinterpret_cast<void**>(&g_internetOpenAOriginal) },
            { L"Wininet.dll", "InternetConnectW", ks::winapi_monitor::EventCategory::Network, &g_internetConnectWHook, reinterpret_cast<void*>(&HookedInternetConnectW), reinterpret_cast<void**>(&g_internetConnectWOriginal) },
            { L"Wininet.dll", "InternetConnectA", ks::winapi_monitor::EventCategory::Network, &g_internetConnectAHook, reinterpret_cast<void*>(&HookedInternetConnectA), reinterpret_cast<void**>(&g_internetConnectAOriginal) },
            { L"Wininet.dll", "HttpOpenRequestW", ks::winapi_monitor::EventCategory::Network, &g_httpOpenRequestWHook, reinterpret_cast<void*>(&HookedHttpOpenRequestW), reinterpret_cast<void**>(&g_httpOpenRequestWOriginal) },
            { L"Wininet.dll", "HttpOpenRequestA", ks::winapi_monitor::EventCategory::Network, &g_httpOpenRequestAHook, reinterpret_cast<void*>(&HookedHttpOpenRequestA), reinterpret_cast<void**>(&g_httpOpenRequestAOriginal) },
            { L"Wininet.dll", "HttpSendRequestW", ks::winapi_monitor::EventCategory::Network, &g_httpSendRequestWHook, reinterpret_cast<void*>(&HookedHttpSendRequestW), reinterpret_cast<void**>(&g_httpSendRequestWOriginal) },
            { L"Wininet.dll", "HttpSendRequestA", ks::winapi_monitor::EventCategory::Network, &g_httpSendRequestAHook, reinterpret_cast<void*>(&HookedHttpSendRequestA), reinterpret_cast<void**>(&g_httpSendRequestAOriginal) },
            { L"Wininet.dll", "InternetReadFile", ks::winapi_monitor::EventCategory::Network, &g_internetReadFileHook, reinterpret_cast<void*>(&HookedInternetReadFile), reinterpret_cast<void**>(&g_internetReadFileOriginal) },
            { L"Wininet.dll", "InternetWriteFile", ks::winapi_monitor::EventCategory::Network, &g_internetWriteFileHook, reinterpret_cast<void*>(&HookedInternetWriteFile), reinterpret_cast<void**>(&g_internetWriteFileOriginal) },
            { L"Wininet.dll", "InternetOpenUrlW", ks::winapi_monitor::EventCategory::Network, &g_internetOpenUrlWHook, reinterpret_cast<void*>(&HookedInternetOpenUrlW), reinterpret_cast<void**>(&g_internetOpenUrlWOriginal) },
            { L"Wininet.dll", "InternetOpenUrlA", ks::winapi_monitor::EventCategory::Network, &g_internetOpenUrlAHook, reinterpret_cast<void*>(&HookedInternetOpenUrlA), reinterpret_cast<void**>(&g_internetOpenUrlAOriginal) },
            { L"Wininet.dll", "InternetQueryDataAvailable", ks::winapi_monitor::EventCategory::Network, &g_internetQueryDataAvailableHook, reinterpret_cast<void*>(&HookedInternetQueryDataAvailable), reinterpret_cast<void**>(&g_internetQueryDataAvailableOriginal) },
            { L"Wininet.dll", "InternetSetOptionW", ks::winapi_monitor::EventCategory::Network, &g_internetSetOptionWHook, reinterpret_cast<void*>(&HookedInternetSetOptionW), reinterpret_cast<void**>(&g_internetSetOptionWOriginal) },
            { L"Wininet.dll", "InternetSetOptionA", ks::winapi_monitor::EventCategory::Network, &g_internetSetOptionAHook, reinterpret_cast<void*>(&HookedInternetSetOptionA), reinterpret_cast<void**>(&g_internetSetOptionAOriginal) },
            { L"Wininet.dll", "InternetCrackUrlW", ks::winapi_monitor::EventCategory::Network, &g_internetCrackUrlWHook, reinterpret_cast<void*>(&HookedInternetCrackUrlW), reinterpret_cast<void**>(&g_internetCrackUrlWOriginal) },
            { L"Wininet.dll", "InternetCrackUrlA", ks::winapi_monitor::EventCategory::Network, &g_internetCrackUrlAHook, reinterpret_cast<void*>(&HookedInternetCrackUrlA), reinterpret_cast<void**>(&g_internetCrackUrlAOriginal) },
            { L"Wininet.dll", "InternetCloseHandle", ks::winapi_monitor::EventCategory::Network, &g_internetCloseHandleHook, reinterpret_cast<void*>(&HookedInternetCloseHandle), reinterpret_cast<void**>(&g_internetCloseHandleOriginal) },
            { L"Urlmon.dll", "URLDownloadToFileW", ks::winapi_monitor::EventCategory::Network, &g_urlDownloadToFileWHook, reinterpret_cast<void*>(&HookedURLDownloadToFileW), reinterpret_cast<void**>(&g_urlDownloadToFileWOriginal) },
            { L"Urlmon.dll", "URLDownloadToFileA", ks::winapi_monitor::EventCategory::Network, &g_urlDownloadToFileAHook, reinterpret_cast<void*>(&HookedURLDownloadToFileA), reinterpret_cast<void**>(&g_urlDownloadToFileAOriginal) },
            { L"Bcrypt.dll", "BCryptOpenAlgorithmProvider", ks::winapi_monitor::EventCategory::Process, &g_bCryptOpenAlgorithmProviderHook, reinterpret_cast<void*>(&HookedBCryptOpenAlgorithmProvider), reinterpret_cast<void**>(&g_bCryptOpenAlgorithmProviderOriginal) },
            { L"Bcrypt.dll", "BCryptCreateHash", ks::winapi_monitor::EventCategory::Process, &g_bCryptCreateHashHook, reinterpret_cast<void*>(&HookedBCryptCreateHash), reinterpret_cast<void**>(&g_bCryptCreateHashOriginal) },
            { L"Bcrypt.dll", "BCryptHashData", ks::winapi_monitor::EventCategory::Process, &g_bCryptHashDataHook, reinterpret_cast<void*>(&HookedBCryptHashData), reinterpret_cast<void**>(&g_bCryptHashDataOriginal) },
            { L"Bcrypt.dll", "BCryptFinishHash", ks::winapi_monitor::EventCategory::Process, &g_bCryptFinishHashHook, reinterpret_cast<void*>(&HookedBCryptFinishHash), reinterpret_cast<void**>(&g_bCryptFinishHashOriginal) },
            { L"Bcrypt.dll", "BCryptEncrypt", ks::winapi_monitor::EventCategory::Process, &g_bCryptEncryptHook, reinterpret_cast<void*>(&HookedBCryptEncrypt), reinterpret_cast<void**>(&g_bCryptEncryptOriginal) },
            { L"Bcrypt.dll", "BCryptDecrypt", ks::winapi_monitor::EventCategory::Process, &g_bCryptDecryptHook, reinterpret_cast<void*>(&HookedBCryptDecrypt), reinterpret_cast<void**>(&g_bCryptDecryptOriginal) },
            { L"Bcrypt.dll", "BCryptGenRandom", ks::winapi_monitor::EventCategory::Process, &g_bCryptGenRandomHook, reinterpret_cast<void*>(&HookedBCryptGenRandom), reinterpret_cast<void**>(&g_bCryptGenRandomOriginal) },
            { L"Bcrypt.dll", "BCryptCloseAlgorithmProvider", ks::winapi_monitor::EventCategory::Process, &g_bCryptCloseAlgorithmProviderHook, reinterpret_cast<void*>(&HookedBCryptCloseAlgorithmProvider), reinterpret_cast<void**>(&g_bCryptCloseAlgorithmProviderOriginal) },
            { L"Bcrypt.dll", "BCryptDestroyHash", ks::winapi_monitor::EventCategory::Process, &g_bCryptDestroyHashHook, reinterpret_cast<void*>(&HookedBCryptDestroyHash), reinterpret_cast<void**>(&g_bCryptDestroyHashOriginal) },
            { L"Bcrypt.dll", "BCryptGenerateSymmetricKey", ks::winapi_monitor::EventCategory::Process, &g_bCryptGenerateSymmetricKeyHook, reinterpret_cast<void*>(&HookedBCryptGenerateSymmetricKey), reinterpret_cast<void**>(&g_bCryptGenerateSymmetricKeyOriginal) },
            { L"Bcrypt.dll", "BCryptImportKey", ks::winapi_monitor::EventCategory::Process, &g_bCryptImportKeyHook, reinterpret_cast<void*>(&HookedBCryptImportKey), reinterpret_cast<void**>(&g_bCryptImportKeyOriginal) },
            { L"Bcrypt.dll", "BCryptImportKeyPair", ks::winapi_monitor::EventCategory::Process, &g_bCryptImportKeyPairHook, reinterpret_cast<void*>(&HookedBCryptImportKeyPair), reinterpret_cast<void**>(&g_bCryptImportKeyPairOriginal) },
            { L"Bcrypt.dll", "BCryptDestroyKey", ks::winapi_monitor::EventCategory::Process, &g_bCryptDestroyKeyHook, reinterpret_cast<void*>(&HookedBCryptDestroyKey), reinterpret_cast<void**>(&g_bCryptDestroyKeyOriginal) },
            { L"Ole32.dll", "CoInitializeEx", ks::winapi_monitor::EventCategory::Process, &g_coInitializeExHook, reinterpret_cast<void*>(&HookedCoInitializeEx), reinterpret_cast<void**>(&g_coInitializeExOriginal) },
            { L"Ole32.dll", "CoInitializeSecurity", ks::winapi_monitor::EventCategory::Process, &g_coInitializeSecurityHook, reinterpret_cast<void*>(&HookedCoInitializeSecurity), reinterpret_cast<void**>(&g_coInitializeSecurityOriginal) },
            { L"Ole32.dll", "CoUninitialize", ks::winapi_monitor::EventCategory::Process, &g_coUninitializeHook, reinterpret_cast<void*>(&HookedCoUninitialize), reinterpret_cast<void**>(&g_coUninitializeOriginal) },
            { L"Ole32.dll", "CoCreateInstance", ks::winapi_monitor::EventCategory::Process, &g_coCreateInstanceHook, reinterpret_cast<void*>(&HookedCoCreateInstance), reinterpret_cast<void**>(&g_coCreateInstanceOriginal) },
            { L"Ole32.dll", "CoCreateInstanceEx", ks::winapi_monitor::EventCategory::Process, &g_coCreateInstanceExHook, reinterpret_cast<void*>(&HookedCoCreateInstanceEx), reinterpret_cast<void**>(&g_coCreateInstanceExOriginal) },
            { L"Ole32.dll", "CoGetClassObject", ks::winapi_monitor::EventCategory::Process, &g_coGetClassObjectHook, reinterpret_cast<void*>(&HookedCoGetClassObject), reinterpret_cast<void**>(&g_coGetClassObjectOriginal) },
            { L"User32.dll", "SetWindowsHookExW", ks::winapi_monitor::EventCategory::Process, &g_setWindowsHookExWHook, reinterpret_cast<void*>(&HookedSetWindowsHookExW), reinterpret_cast<void**>(&g_setWindowsHookExWOriginal) },
            { L"User32.dll", "SetWindowsHookExA", ks::winapi_monitor::EventCategory::Process, &g_setWindowsHookExAHook, reinterpret_cast<void*>(&HookedSetWindowsHookExA), reinterpret_cast<void**>(&g_setWindowsHookExAOriginal) },
            { L"User32.dll", "UnhookWindowsHookEx", ks::winapi_monitor::EventCategory::Process, &g_unhookWindowsHookExHook, reinterpret_cast<void*>(&HookedUnhookWindowsHookEx), reinterpret_cast<void**>(&g_unhookWindowsHookExOriginal) },
            { L"Psapi.dll", "EnumProcesses", ks::winapi_monitor::EventCategory::Process, &g_enumProcessesHook, reinterpret_cast<void*>(&HookedEnumProcesses), reinterpret_cast<void**>(&g_enumProcessesOriginal) },
            { L"Psapi.dll", "EnumProcessModules", ks::winapi_monitor::EventCategory::Loader, &g_enumProcessModulesHook, reinterpret_cast<void*>(&HookedEnumProcessModules), reinterpret_cast<void**>(&g_enumProcessModulesOriginal) },
            { L"Psapi.dll", "EnumProcessModulesEx", ks::winapi_monitor::EventCategory::Loader, &g_enumProcessModulesExHook, reinterpret_cast<void*>(&HookedEnumProcessModulesEx), reinterpret_cast<void**>(&g_enumProcessModulesExOriginal) },
            { L"Psapi.dll", "GetMappedFileNameW", ks::winapi_monitor::EventCategory::Loader, &g_getMappedFileNameWHook, reinterpret_cast<void*>(&HookedGetMappedFileNameW), reinterpret_cast<void**>(&g_getMappedFileNameWOriginal) },
            { L"Psapi.dll", "GetMappedFileNameA", ks::winapi_monitor::EventCategory::Loader, &g_getMappedFileNameAHook, reinterpret_cast<void*>(&HookedGetMappedFileNameA), reinterpret_cast<void**>(&g_getMappedFileNameAOriginal) },
            { L"User32.dll", "EnumWindows", ks::winapi_monitor::EventCategory::Process, &g_enumWindowsHook, reinterpret_cast<void*>(&HookedEnumWindows), reinterpret_cast<void**>(&g_enumWindowsOriginal) },
            { L"User32.dll", "EnumChildWindows", ks::winapi_monitor::EventCategory::Process, &g_enumChildWindowsHook, reinterpret_cast<void*>(&HookedEnumChildWindows), reinterpret_cast<void**>(&g_enumChildWindowsOriginal) },
            { L"User32.dll", "FindWindowW", ks::winapi_monitor::EventCategory::Process, &g_findWindowWHook, reinterpret_cast<void*>(&HookedFindWindowW), reinterpret_cast<void**>(&g_findWindowWOriginal) },
            { L"User32.dll", "FindWindowA", ks::winapi_monitor::EventCategory::Process, &g_findWindowAHook, reinterpret_cast<void*>(&HookedFindWindowA), reinterpret_cast<void**>(&g_findWindowAOriginal) },
            { L"User32.dll", "FindWindowExW", ks::winapi_monitor::EventCategory::Process, &g_findWindowExWHook, reinterpret_cast<void*>(&HookedFindWindowExW), reinterpret_cast<void**>(&g_findWindowExWOriginal) },
            { L"User32.dll", "FindWindowExA", ks::winapi_monitor::EventCategory::Process, &g_findWindowExAHook, reinterpret_cast<void*>(&HookedFindWindowExA), reinterpret_cast<void**>(&g_findWindowExAOriginal) },
            { L"User32.dll", "GetWindowThreadProcessId", ks::winapi_monitor::EventCategory::Process, &g_getWindowThreadProcessIdHook, reinterpret_cast<void*>(&HookedGetWindowThreadProcessId), reinterpret_cast<void**>(&g_getWindowThreadProcessIdOriginal) },
            { L"User32.dll", "GetForegroundWindow", ks::winapi_monitor::EventCategory::Process, &g_getForegroundWindowHook, reinterpret_cast<void*>(&HookedGetForegroundWindow), reinterpret_cast<void**>(&g_getForegroundWindowOriginal) },
            { L"User32.dll", "GetDC", ks::winapi_monitor::EventCategory::Process, &g_getDCHook, reinterpret_cast<void*>(&HookedGetDC), reinterpret_cast<void**>(&g_getDCOriginal) },
            { L"User32.dll", "ReleaseDC", ks::winapi_monitor::EventCategory::Process, &g_releaseDCHook, reinterpret_cast<void*>(&HookedReleaseDC), reinterpret_cast<void**>(&g_releaseDCOriginal) },
            { L"User32.dll", "OpenClipboard", ks::winapi_monitor::EventCategory::Process, &g_openClipboardHook, reinterpret_cast<void*>(&HookedOpenClipboard), reinterpret_cast<void**>(&g_openClipboardOriginal) },
            { L"User32.dll", "CloseClipboard", ks::winapi_monitor::EventCategory::Process, &g_closeClipboardHook, reinterpret_cast<void*>(&HookedCloseClipboard), reinterpret_cast<void**>(&g_closeClipboardOriginal) },
            { L"User32.dll", "GetClipboardData", ks::winapi_monitor::EventCategory::Process, &g_getClipboardDataHook, reinterpret_cast<void*>(&HookedGetClipboardData), reinterpret_cast<void**>(&g_getClipboardDataOriginal) },
            { L"User32.dll", "SetClipboardData", ks::winapi_monitor::EventCategory::Process, &g_setClipboardDataHook, reinterpret_cast<void*>(&HookedSetClipboardData), reinterpret_cast<void**>(&g_setClipboardDataOriginal) },
            { L"User32.dll", "EmptyClipboard", ks::winapi_monitor::EventCategory::Process, &g_emptyClipboardHook, reinterpret_cast<void*>(&HookedEmptyClipboard), reinterpret_cast<void**>(&g_emptyClipboardOriginal) },
            { L"Gdi32.dll", "CreateCompatibleDC", ks::winapi_monitor::EventCategory::Process, &g_createCompatibleDCHook, reinterpret_cast<void*>(&HookedCreateCompatibleDC), reinterpret_cast<void**>(&g_createCompatibleDCOriginal) },
            { L"Gdi32.dll", "DeleteDC", ks::winapi_monitor::EventCategory::Process, &g_deleteDCHook, reinterpret_cast<void*>(&HookedDeleteDC), reinterpret_cast<void**>(&g_deleteDCOriginal) },
            { L"Gdi32.dll", "CreateCompatibleBitmap", ks::winapi_monitor::EventCategory::Process, &g_createCompatibleBitmapHook, reinterpret_cast<void*>(&HookedCreateCompatibleBitmap), reinterpret_cast<void**>(&g_createCompatibleBitmapOriginal) },
            { L"Gdi32.dll", "BitBlt", ks::winapi_monitor::EventCategory::Process, &g_bitBltHook, reinterpret_cast<void*>(&HookedBitBlt), reinterpret_cast<void**>(&g_bitBltOriginal) },
            { L"Gdi32.dll", "StretchBlt", ks::winapi_monitor::EventCategory::Process, &g_stretchBltHook, reinterpret_cast<void*>(&HookedStretchBlt), reinterpret_cast<void**>(&g_stretchBltOriginal) },
            { L"Gdi32.dll", "DeleteObject", ks::winapi_monitor::EventCategory::Process, &g_deleteObjectHook, reinterpret_cast<void*>(&HookedDeleteObject), reinterpret_cast<void**>(&g_deleteObjectOriginal) },
            { L"Advapi32.dll", "StartTraceW", ks::winapi_monitor::EventCategory::Process, &g_startTraceWHook, reinterpret_cast<void*>(&HookedStartTraceW), reinterpret_cast<void**>(&g_startTraceWOriginal) },
            { L"Advapi32.dll", "StartTraceA", ks::winapi_monitor::EventCategory::Process, &g_startTraceAHook, reinterpret_cast<void*>(&HookedStartTraceA), reinterpret_cast<void**>(&g_startTraceAOriginal) },
            { L"Advapi32.dll", "ControlTraceW", ks::winapi_monitor::EventCategory::Process, &g_controlTraceWHook, reinterpret_cast<void*>(&HookedControlTraceW), reinterpret_cast<void**>(&g_controlTraceWOriginal) },
            { L"Advapi32.dll", "ControlTraceA", ks::winapi_monitor::EventCategory::Process, &g_controlTraceAHook, reinterpret_cast<void*>(&HookedControlTraceA), reinterpret_cast<void**>(&g_controlTraceAOriginal) },
            { L"Advapi32.dll", "EnableTraceEx2", ks::winapi_monitor::EventCategory::Process, &g_enableTraceEx2Hook, reinterpret_cast<void*>(&HookedEnableTraceEx2), reinterpret_cast<void**>(&g_enableTraceEx2Original) },
            { L"Advapi32.dll", "OpenTraceW", ks::winapi_monitor::EventCategory::Process, &g_openTraceWHook, reinterpret_cast<void*>(&HookedOpenTraceW), reinterpret_cast<void**>(&g_openTraceWOriginal) },
            { L"Advapi32.dll", "OpenTraceA", ks::winapi_monitor::EventCategory::Process, &g_openTraceAHook, reinterpret_cast<void*>(&HookedOpenTraceA), reinterpret_cast<void**>(&g_openTraceAOriginal) },
            { L"Advapi32.dll", "ProcessTrace", ks::winapi_monitor::EventCategory::Process, &g_processTraceHook, reinterpret_cast<void*>(&HookedProcessTrace), reinterpret_cast<void**>(&g_processTraceOriginal) },
            { L"Advapi32.dll", "CloseTrace", ks::winapi_monitor::EventCategory::Process, &g_closeTraceHook, reinterpret_cast<void*>(&HookedCloseTrace), reinterpret_cast<void**>(&g_closeTraceOriginal) },
            { L"Advapi32.dll", "EventRegister", ks::winapi_monitor::EventCategory::Process, &g_eventRegisterHook, reinterpret_cast<void*>(&HookedEventRegister), reinterpret_cast<void**>(&g_eventRegisterOriginal) },
            { L"Advapi32.dll", "EventUnregister", ks::winapi_monitor::EventCategory::Process, &g_eventUnregisterHook, reinterpret_cast<void*>(&HookedEventUnregister), reinterpret_cast<void**>(&g_eventUnregisterOriginal) },
            { L"Advapi32.dll", "EventWrite", ks::winapi_monitor::EventCategory::Process, &g_eventWriteHook, reinterpret_cast<void*>(&HookedEventWrite), reinterpret_cast<void**>(&g_eventWriteOriginal) },
            { L"Advapi32.dll", "EventWriteEx", ks::winapi_monitor::EventCategory::Process, &g_eventWriteExHook, reinterpret_cast<void*>(&HookedEventWriteEx), reinterpret_cast<void**>(&g_eventWriteExOriginal) },
            { L"Wintrust.dll", "WinVerifyTrust", ks::winapi_monitor::EventCategory::Process, &g_winVerifyTrustHook, reinterpret_cast<void*>(&HookedWinVerifyTrust), reinterpret_cast<void**>(&g_winVerifyTrustOriginal) },
            { L"Crypt32.dll", "CryptQueryObject", ks::winapi_monitor::EventCategory::Process, &g_cryptQueryObjectHook, reinterpret_cast<void*>(&HookedCryptQueryObject), reinterpret_cast<void**>(&g_cryptQueryObjectOriginal) },
            { L"Crypt32.dll", "CertOpenStore", ks::winapi_monitor::EventCategory::Process, &g_certOpenStoreHook, reinterpret_cast<void*>(&HookedCertOpenStore), reinterpret_cast<void**>(&g_certOpenStoreOriginal) },
            { L"Crypt32.dll", "CertCloseStore", ks::winapi_monitor::EventCategory::Process, &g_certCloseStoreHook, reinterpret_cast<void*>(&HookedCertCloseStore), reinterpret_cast<void**>(&g_certCloseStoreOriginal) },
            { L"Crypt32.dll", "CertFindCertificateInStore", ks::winapi_monitor::EventCategory::Process, &g_certFindCertificateInStoreHook, reinterpret_cast<void*>(&HookedCertFindCertificateInStore), reinterpret_cast<void**>(&g_certFindCertificateInStoreOriginal) },
            { L"Crypt32.dll", "CertGetCertificateChain", ks::winapi_monitor::EventCategory::Process, &g_certGetCertificateChainHook, reinterpret_cast<void*>(&HookedCertGetCertificateChain), reinterpret_cast<void**>(&g_certGetCertificateChainOriginal) },
            { L"Crypt32.dll", "CertVerifyCertificateChainPolicy", ks::winapi_monitor::EventCategory::Process, &g_certVerifyCertificateChainPolicyHook, reinterpret_cast<void*>(&HookedCertVerifyCertificateChainPolicy), reinterpret_cast<void**>(&g_certVerifyCertificateChainPolicyOriginal) },
            { L"Crypt32.dll", "CryptProtectData", ks::winapi_monitor::EventCategory::Process, &g_cryptProtectDataHook, reinterpret_cast<void*>(&HookedCryptProtectData), reinterpret_cast<void**>(&g_cryptProtectDataOriginal) },
            { L"Crypt32.dll", "CryptUnprotectData", ks::winapi_monitor::EventCategory::Process, &g_cryptUnprotectDataHook, reinterpret_cast<void*>(&HookedCryptUnprotectData), reinterpret_cast<void**>(&g_cryptUnprotectDataOriginal) },
            { L"Ncrypt.dll", "NCryptOpenStorageProvider", ks::winapi_monitor::EventCategory::Process, &g_nCryptOpenStorageProviderHook, reinterpret_cast<void*>(&HookedNCryptOpenStorageProvider), reinterpret_cast<void**>(&g_nCryptOpenStorageProviderOriginal) },
            { L"Ncrypt.dll", "NCryptOpenKey", ks::winapi_monitor::EventCategory::Process, &g_nCryptOpenKeyHook, reinterpret_cast<void*>(&HookedNCryptOpenKey), reinterpret_cast<void**>(&g_nCryptOpenKeyOriginal) },
            { L"Ncrypt.dll", "NCryptCreatePersistedKey", ks::winapi_monitor::EventCategory::Process, &g_nCryptCreatePersistedKeyHook, reinterpret_cast<void*>(&HookedNCryptCreatePersistedKey), reinterpret_cast<void**>(&g_nCryptCreatePersistedKeyOriginal) },
            { L"Ncrypt.dll", "NCryptFinalizeKey", ks::winapi_monitor::EventCategory::Process, &g_nCryptFinalizeKeyHook, reinterpret_cast<void*>(&HookedNCryptFinalizeKey), reinterpret_cast<void**>(&g_nCryptFinalizeKeyOriginal) },
            { L"Ncrypt.dll", "NCryptEncrypt", ks::winapi_monitor::EventCategory::Process, &g_nCryptEncryptHook, reinterpret_cast<void*>(&HookedNCryptEncrypt), reinterpret_cast<void**>(&g_nCryptEncryptOriginal) },
            { L"Ncrypt.dll", "NCryptDecrypt", ks::winapi_monitor::EventCategory::Process, &g_nCryptDecryptHook, reinterpret_cast<void*>(&HookedNCryptDecrypt), reinterpret_cast<void**>(&g_nCryptDecryptOriginal) },
            { L"Ncrypt.dll", "NCryptSignHash", ks::winapi_monitor::EventCategory::Process, &g_nCryptSignHashHook, reinterpret_cast<void*>(&HookedNCryptSignHash), reinterpret_cast<void**>(&g_nCryptSignHashOriginal) },
            { L"Ncrypt.dll", "NCryptVerifySignature", ks::winapi_monitor::EventCategory::Process, &g_nCryptVerifySignatureHook, reinterpret_cast<void*>(&HookedNCryptVerifySignature), reinterpret_cast<void**>(&g_nCryptVerifySignatureOriginal) },
            { L"Ncrypt.dll", "NCryptExportKey", ks::winapi_monitor::EventCategory::Process, &g_nCryptExportKeyHook, reinterpret_cast<void*>(&HookedNCryptExportKey), reinterpret_cast<void**>(&g_nCryptExportKeyOriginal) },
            { L"Ncrypt.dll", "NCryptImportKey", ks::winapi_monitor::EventCategory::Process, &g_nCryptImportKeyHook, reinterpret_cast<void*>(&HookedNCryptImportKey), reinterpret_cast<void**>(&g_nCryptImportKeyOriginal) },
            { L"Ncrypt.dll", "NCryptDeleteKey", ks::winapi_monitor::EventCategory::Process, &g_nCryptDeleteKeyHook, reinterpret_cast<void*>(&HookedNCryptDeleteKey), reinterpret_cast<void**>(&g_nCryptDeleteKeyOriginal) },
            { L"Ncrypt.dll", "NCryptFreeObject", ks::winapi_monitor::EventCategory::Process, &g_nCryptFreeObjectHook, reinterpret_cast<void*>(&HookedNCryptFreeObject), reinterpret_cast<void**>(&g_nCryptFreeObjectOriginal) },
            { L"Rpcrt4.dll", "RpcStringBindingComposeW", ks::winapi_monitor::EventCategory::Network, &g_rpcStringBindingComposeWHook, reinterpret_cast<void*>(&HookedRpcStringBindingComposeW), reinterpret_cast<void**>(&g_rpcStringBindingComposeWOriginal) },
            { L"Rpcrt4.dll", "RpcStringBindingComposeA", ks::winapi_monitor::EventCategory::Network, &g_rpcStringBindingComposeAHook, reinterpret_cast<void*>(&HookedRpcStringBindingComposeA), reinterpret_cast<void**>(&g_rpcStringBindingComposeAOriginal) },
            { L"Rpcrt4.dll", "RpcBindingFromStringBindingW", ks::winapi_monitor::EventCategory::Network, &g_rpcBindingFromStringBindingWHook, reinterpret_cast<void*>(&HookedRpcBindingFromStringBindingW), reinterpret_cast<void**>(&g_rpcBindingFromStringBindingWOriginal) },
            { L"Rpcrt4.dll", "RpcBindingFromStringBindingA", ks::winapi_monitor::EventCategory::Network, &g_rpcBindingFromStringBindingAHook, reinterpret_cast<void*>(&HookedRpcBindingFromStringBindingA), reinterpret_cast<void**>(&g_rpcBindingFromStringBindingAOriginal) },
            { L"Rpcrt4.dll", "RpcBindingFree", ks::winapi_monitor::EventCategory::Network, &g_rpcBindingFreeHook, reinterpret_cast<void*>(&HookedRpcBindingFree), reinterpret_cast<void**>(&g_rpcBindingFreeOriginal) },
            { L"Rpcrt4.dll", "RpcMgmtEpEltInqBegin", ks::winapi_monitor::EventCategory::Network, &g_rpcMgmtEpEltInqBeginHook, reinterpret_cast<void*>(&HookedRpcMgmtEpEltInqBegin), reinterpret_cast<void**>(&g_rpcMgmtEpEltInqBeginOriginal) },
            { L"Rpcrt4.dll", "RpcMgmtEpEltInqNextW", ks::winapi_monitor::EventCategory::Network, &g_rpcMgmtEpEltInqNextWHook, reinterpret_cast<void*>(&HookedRpcMgmtEpEltInqNextW), reinterpret_cast<void**>(&g_rpcMgmtEpEltInqNextWOriginal) },
            { L"Rpcrt4.dll", "RpcMgmtEpEltInqNextA", ks::winapi_monitor::EventCategory::Network, &g_rpcMgmtEpEltInqNextAHook, reinterpret_cast<void*>(&HookedRpcMgmtEpEltInqNextA), reinterpret_cast<void**>(&g_rpcMgmtEpEltInqNextAOriginal) },
            { L"Rpcrt4.dll", "RpcMgmtEpEltInqDone", ks::winapi_monitor::EventCategory::Network, &g_rpcMgmtEpEltInqDoneHook, reinterpret_cast<void*>(&HookedRpcMgmtEpEltInqDone), reinterpret_cast<void**>(&g_rpcMgmtEpEltInqDoneOriginal) },
            { L"ntdll.dll", "NtQueryInformationToken", ks::winapi_monitor::EventCategory::Process, &g_ntQueryInformationTokenHook, reinterpret_cast<void*>(&HookedNtQueryInformationToken), reinterpret_cast<void**>(&g_ntQueryInformationTokenOriginal) },
            { L"ntdll.dll", "NtSetInformationToken", ks::winapi_monitor::EventCategory::Process, &g_ntSetInformationTokenHook, reinterpret_cast<void*>(&HookedNtSetInformationToken), reinterpret_cast<void**>(&g_ntSetInformationTokenOriginal) },
            { L"ntdll.dll", "NtAdjustPrivilegesToken", ks::winapi_monitor::EventCategory::Process, &g_ntAdjustPrivilegesTokenHook, reinterpret_cast<void*>(&HookedNtAdjustPrivilegesToken), reinterpret_cast<void**>(&g_ntAdjustPrivilegesTokenOriginal) },
            { L"ntdll.dll", "NtCreateMutant", ks::winapi_monitor::EventCategory::Process, &g_ntCreateMutantHook, reinterpret_cast<void*>(&HookedNtCreateMutant), reinterpret_cast<void**>(&g_ntCreateMutantOriginal) },
            { L"ntdll.dll", "NtOpenMutant", ks::winapi_monitor::EventCategory::Process, &g_ntOpenMutantHook, reinterpret_cast<void*>(&HookedNtOpenMutant), reinterpret_cast<void**>(&g_ntOpenMutantOriginal) },
            { L"ntdll.dll", "NtReleaseMutant", ks::winapi_monitor::EventCategory::Process, &g_ntReleaseMutantHook, reinterpret_cast<void*>(&HookedNtReleaseMutant), reinterpret_cast<void**>(&g_ntReleaseMutantOriginal) },
            { L"ntdll.dll", "NtCreateEvent", ks::winapi_monitor::EventCategory::Process, &g_ntCreateEventHook, reinterpret_cast<void*>(&HookedNtCreateEvent), reinterpret_cast<void**>(&g_ntCreateEventOriginal) },
            { L"ntdll.dll", "NtOpenEvent", ks::winapi_monitor::EventCategory::Process, &g_ntOpenEventHook, reinterpret_cast<void*>(&HookedNtOpenEvent), reinterpret_cast<void**>(&g_ntOpenEventOriginal) },
            { L"ntdll.dll", "NtSetEvent", ks::winapi_monitor::EventCategory::Process, &g_ntSetEventHook, reinterpret_cast<void*>(&HookedNtSetEvent), reinterpret_cast<void**>(&g_ntSetEventOriginal) },
            { L"ntdll.dll", "NtResetEvent", ks::winapi_monitor::EventCategory::Process, &g_ntResetEventHook, reinterpret_cast<void*>(&HookedNtResetEvent), reinterpret_cast<void**>(&g_ntResetEventOriginal) },
            { L"ntdll.dll", "NtWaitForSingleObject", ks::winapi_monitor::EventCategory::Process, &g_ntWaitForSingleObjectHook, reinterpret_cast<void*>(&HookedNtWaitForSingleObject), reinterpret_cast<void**>(&g_ntWaitForSingleObjectOriginal) },
            { L"ntdll.dll", "NtWaitForMultipleObjects", ks::winapi_monitor::EventCategory::Process, &g_ntWaitForMultipleObjectsHook, reinterpret_cast<void*>(&HookedNtWaitForMultipleObjects), reinterpret_cast<void**>(&g_ntWaitForMultipleObjectsOriginal) },
            { L"ntdll.dll", "NtQuerySystemInformation", ks::winapi_monitor::EventCategory::Process, &g_ntQuerySystemInformationHook, reinterpret_cast<void*>(&HookedNtQuerySystemInformation), reinterpret_cast<void**>(&g_ntQuerySystemInformationOriginal) },
            { L"ntdll.dll", "NtQueryObject", ks::winapi_monitor::EventCategory::Process, &g_ntQueryObjectHook, reinterpret_cast<void*>(&HookedNtQueryObject), reinterpret_cast<void**>(&g_ntQueryObjectOriginal) },
            { L"Advapi32.dll", "LogonUserW", ks::winapi_monitor::EventCategory::Process, &g_logonUserWHook, reinterpret_cast<void*>(&HookedLogonUserW), reinterpret_cast<void**>(&g_logonUserWOriginal) },
            { L"Advapi32.dll", "LogonUserA", ks::winapi_monitor::EventCategory::Process, &g_logonUserAHook, reinterpret_cast<void*>(&HookedLogonUserA), reinterpret_cast<void**>(&g_logonUserAOriginal) },
            { L"Advapi32.dll", "GetTokenInformation", ks::winapi_monitor::EventCategory::Process, &g_getTokenInformationHook, reinterpret_cast<void*>(&HookedGetTokenInformation), reinterpret_cast<void**>(&g_getTokenInformationOriginal) },
            { L"Advapi32.dll", "SetTokenInformation", ks::winapi_monitor::EventCategory::Process, &g_setTokenInformationHook, reinterpret_cast<void*>(&HookedSetTokenInformation), reinterpret_cast<void**>(&g_setTokenInformationOriginal) },
            { L"Advapi32.dll", "CheckTokenMembership", ks::winapi_monitor::EventCategory::Process, &g_checkTokenMembershipHook, reinterpret_cast<void*>(&HookedCheckTokenMembership), reinterpret_cast<void**>(&g_checkTokenMembershipOriginal) },
            { L"Advapi32.dll", "CreateRestrictedToken", ks::winapi_monitor::EventCategory::Process, &g_createRestrictedTokenHook, reinterpret_cast<void*>(&HookedCreateRestrictedToken), reinterpret_cast<void**>(&g_createRestrictedTokenOriginal) },
            { L"Advapi32.dll", "ImpersonateSelf", ks::winapi_monitor::EventCategory::Process, &g_impersonateSelfHook, reinterpret_cast<void*>(&HookedImpersonateSelf), reinterpret_cast<void**>(&g_impersonateSelfOriginal) },
            { L"Advapi32.dll", "ImpersonateNamedPipeClient", ks::winapi_monitor::EventCategory::Process, &g_impersonateNamedPipeClientHook, reinterpret_cast<void*>(&HookedImpersonateNamedPipeClient), reinterpret_cast<void**>(&g_impersonateNamedPipeClientOriginal) },
            { L"Advapi32.dll", "CredReadW", ks::winapi_monitor::EventCategory::Process, &g_credReadWHook, reinterpret_cast<void*>(&HookedCredReadW), reinterpret_cast<void**>(&g_credReadWOriginal) },
            { L"Advapi32.dll", "CredReadA", ks::winapi_monitor::EventCategory::Process, &g_credReadAHook, reinterpret_cast<void*>(&HookedCredReadA), reinterpret_cast<void**>(&g_credReadAOriginal) },
            { L"Advapi32.dll", "CredEnumerateW", ks::winapi_monitor::EventCategory::Process, &g_credEnumerateWHook, reinterpret_cast<void*>(&HookedCredEnumerateW), reinterpret_cast<void**>(&g_credEnumerateWOriginal) },
            { L"Advapi32.dll", "CredEnumerateA", ks::winapi_monitor::EventCategory::Process, &g_credEnumerateAHook, reinterpret_cast<void*>(&HookedCredEnumerateA), reinterpret_cast<void**>(&g_credEnumerateAOriginal) },
            { L"Advapi32.dll", "CredWriteW", ks::winapi_monitor::EventCategory::Process, &g_credWriteWHook, reinterpret_cast<void*>(&HookedCredWriteW), reinterpret_cast<void**>(&g_credWriteWOriginal) },
            { L"Advapi32.dll", "CredWriteA", ks::winapi_monitor::EventCategory::Process, &g_credWriteAHook, reinterpret_cast<void*>(&HookedCredWriteA), reinterpret_cast<void**>(&g_credWriteAOriginal) },
            { L"Advapi32.dll", "CredDeleteW", ks::winapi_monitor::EventCategory::Process, &g_credDeleteWHook, reinterpret_cast<void*>(&HookedCredDeleteW), reinterpret_cast<void**>(&g_credDeleteWOriginal) },
            { L"Advapi32.dll", "CredDeleteA", ks::winapi_monitor::EventCategory::Process, &g_credDeleteAHook, reinterpret_cast<void*>(&HookedCredDeleteA), reinterpret_cast<void**>(&g_credDeleteAOriginal) },
            { L"Advapi32.dll", "CredFree", ks::winapi_monitor::EventCategory::Process, &g_credFreeHook, reinterpret_cast<void*>(&HookedCredFree), reinterpret_cast<void**>(&g_credFreeOriginal) },
            { L"Advapi32.dll", "LsaOpenPolicy", ks::winapi_monitor::EventCategory::Process, &g_lsaOpenPolicyHook, reinterpret_cast<void*>(&HookedLsaOpenPolicy), reinterpret_cast<void**>(&g_lsaOpenPolicyOriginal) },
            { L"Advapi32.dll", "LsaClose", ks::winapi_monitor::EventCategory::Process, &g_lsaCloseHook, reinterpret_cast<void*>(&HookedLsaClose), reinterpret_cast<void**>(&g_lsaCloseOriginal) },
            { L"Advapi32.dll", "LsaEnumerateLogonSessions", ks::winapi_monitor::EventCategory::Process, &g_lsaEnumerateLogonSessionsHook, reinterpret_cast<void*>(&HookedLsaEnumerateLogonSessions), reinterpret_cast<void**>(&g_lsaEnumerateLogonSessionsOriginal) },
            { L"Advapi32.dll", "LsaGetLogonSessionData", ks::winapi_monitor::EventCategory::Process, &g_lsaGetLogonSessionDataHook, reinterpret_cast<void*>(&HookedLsaGetLogonSessionData), reinterpret_cast<void**>(&g_lsaGetLogonSessionDataOriginal) },
            { L"Advapi32.dll", "LsaFreeReturnBuffer", ks::winapi_monitor::EventCategory::Process, &g_lsaFreeReturnBufferHook, reinterpret_cast<void*>(&HookedLsaFreeReturnBuffer), reinterpret_cast<void**>(&g_lsaFreeReturnBufferOriginal) },
            { L"Advapi32.dll", "LsaLookupNames2", ks::winapi_monitor::EventCategory::Process, &g_lsaLookupNames2Hook, reinterpret_cast<void*>(&HookedLsaLookupNames2), reinterpret_cast<void**>(&g_lsaLookupNames2Original) },
            { L"Advapi32.dll", "LsaLookupSids2", ks::winapi_monitor::EventCategory::Process, &g_lsaLookupSids2Hook, reinterpret_cast<void*>(&HookedLsaLookupSids2), reinterpret_cast<void**>(&g_lsaLookupSids2Original) },
            { L"Advapi32.dll", "OpenEventLogW", ks::winapi_monitor::EventCategory::Process, &g_openEventLogWHook, reinterpret_cast<void*>(&HookedOpenEventLogW), reinterpret_cast<void**>(&g_openEventLogWOriginal) },
            { L"Advapi32.dll", "OpenEventLogA", ks::winapi_monitor::EventCategory::Process, &g_openEventLogAHook, reinterpret_cast<void*>(&HookedOpenEventLogA), reinterpret_cast<void**>(&g_openEventLogAOriginal) },
            { L"Advapi32.dll", "RegisterEventSourceW", ks::winapi_monitor::EventCategory::Process, &g_registerEventSourceWHook, reinterpret_cast<void*>(&HookedRegisterEventSourceW), reinterpret_cast<void**>(&g_registerEventSourceWOriginal) },
            { L"Advapi32.dll", "RegisterEventSourceA", ks::winapi_monitor::EventCategory::Process, &g_registerEventSourceAHook, reinterpret_cast<void*>(&HookedRegisterEventSourceA), reinterpret_cast<void**>(&g_registerEventSourceAOriginal) },
            { L"Advapi32.dll", "ReadEventLogW", ks::winapi_monitor::EventCategory::Process, &g_readEventLogWHook, reinterpret_cast<void*>(&HookedReadEventLogW), reinterpret_cast<void**>(&g_readEventLogWOriginal) },
            { L"Advapi32.dll", "ReadEventLogA", ks::winapi_monitor::EventCategory::Process, &g_readEventLogAHook, reinterpret_cast<void*>(&HookedReadEventLogA), reinterpret_cast<void**>(&g_readEventLogAOriginal) },
            { L"Advapi32.dll", "ClearEventLogW", ks::winapi_monitor::EventCategory::Process, &g_clearEventLogWHook, reinterpret_cast<void*>(&HookedClearEventLogW), reinterpret_cast<void**>(&g_clearEventLogWOriginal) },
            { L"Advapi32.dll", "ClearEventLogA", ks::winapi_monitor::EventCategory::Process, &g_clearEventLogAHook, reinterpret_cast<void*>(&HookedClearEventLogA), reinterpret_cast<void**>(&g_clearEventLogAOriginal) },
            { L"Advapi32.dll", "ReportEventW", ks::winapi_monitor::EventCategory::Process, &g_reportEventWHook, reinterpret_cast<void*>(&HookedReportEventW), reinterpret_cast<void**>(&g_reportEventWOriginal) },
            { L"Advapi32.dll", "ReportEventA", ks::winapi_monitor::EventCategory::Process, &g_reportEventAHook, reinterpret_cast<void*>(&HookedReportEventA), reinterpret_cast<void**>(&g_reportEventAOriginal) },
            { L"Advapi32.dll", "CloseEventLog", ks::winapi_monitor::EventCategory::Process, &g_closeEventLogHook, reinterpret_cast<void*>(&HookedCloseEventLog), reinterpret_cast<void**>(&g_closeEventLogOriginal) },
            { L"Netapi32.dll", "NetUserEnum", ks::winapi_monitor::EventCategory::Network, &g_netUserEnumHook, reinterpret_cast<void*>(&HookedNetUserEnum), reinterpret_cast<void**>(&g_netUserEnumOriginal) },
            { L"Netapi32.dll", "NetLocalGroupEnum", ks::winapi_monitor::EventCategory::Network, &g_netLocalGroupEnumHook, reinterpret_cast<void*>(&HookedNetLocalGroupEnum), reinterpret_cast<void**>(&g_netLocalGroupEnumOriginal) },
            { L"Netapi32.dll", "NetGroupEnum", ks::winapi_monitor::EventCategory::Network, &g_netGroupEnumHook, reinterpret_cast<void*>(&HookedNetGroupEnum), reinterpret_cast<void**>(&g_netGroupEnumOriginal) },
            { L"Netapi32.dll", "NetShareEnum", ks::winapi_monitor::EventCategory::Network, &g_netShareEnumHook, reinterpret_cast<void*>(&HookedNetShareEnum), reinterpret_cast<void**>(&g_netShareEnumOriginal) },
            { L"Netapi32.dll", "NetSessionEnum", ks::winapi_monitor::EventCategory::Network, &g_netSessionEnumHook, reinterpret_cast<void*>(&HookedNetSessionEnum), reinterpret_cast<void**>(&g_netSessionEnumOriginal) },
            { L"Netapi32.dll", "NetServerEnum", ks::winapi_monitor::EventCategory::Network, &g_netServerEnumHook, reinterpret_cast<void*>(&HookedNetServerEnum), reinterpret_cast<void**>(&g_netServerEnumOriginal) },
            { L"Netapi32.dll", "NetWkstaGetInfo", ks::winapi_monitor::EventCategory::Network, &g_netWkstaGetInfoHook, reinterpret_cast<void*>(&HookedNetWkstaGetInfo), reinterpret_cast<void**>(&g_netWkstaGetInfoOriginal) },
            { L"Netapi32.dll", "NetApiBufferFree", ks::winapi_monitor::EventCategory::Network, &g_netApiBufferFreeHook, reinterpret_cast<void*>(&HookedNetApiBufferFree), reinterpret_cast<void**>(&g_netApiBufferFreeOriginal) },
            { L"Iphlpapi.dll", "GetExtendedTcpTable", ks::winapi_monitor::EventCategory::Network, &g_getExtendedTcpTableHook, reinterpret_cast<void*>(&HookedGetExtendedTcpTable), reinterpret_cast<void**>(&g_getExtendedTcpTableOriginal) },
            { L"Iphlpapi.dll", "GetExtendedUdpTable", ks::winapi_monitor::EventCategory::Network, &g_getExtendedUdpTableHook, reinterpret_cast<void*>(&HookedGetExtendedUdpTable), reinterpret_cast<void**>(&g_getExtendedUdpTableOriginal) },
            { L"Iphlpapi.dll", "GetTcpTable2", ks::winapi_monitor::EventCategory::Network, &g_getTcpTable2Hook, reinterpret_cast<void*>(&HookedGetTcpTable2), reinterpret_cast<void**>(&g_getTcpTable2Original) },
            { L"Iphlpapi.dll", "GetUdpTable", ks::winapi_monitor::EventCategory::Network, &g_getUdpTableHook, reinterpret_cast<void*>(&HookedGetUdpTable), reinterpret_cast<void**>(&g_getUdpTableOriginal) },
            { L"Iphlpapi.dll", "GetAdaptersAddresses", ks::winapi_monitor::EventCategory::Network, &g_getAdaptersAddressesHook, reinterpret_cast<void*>(&HookedGetAdaptersAddresses), reinterpret_cast<void**>(&g_getAdaptersAddressesOriginal) },
            { L"Iphlpapi.dll", "GetNetworkParams", ks::winapi_monitor::EventCategory::Network, &g_getNetworkParamsHook, reinterpret_cast<void*>(&HookedGetNetworkParams), reinterpret_cast<void**>(&g_getNetworkParamsOriginal) },
            { L"Iphlpapi.dll", "GetIpNetTable2", ks::winapi_monitor::EventCategory::Network, &g_getIpNetTable2Hook, reinterpret_cast<void*>(&HookedGetIpNetTable2), reinterpret_cast<void**>(&g_getIpNetTable2Original) },
            { L"Iphlpapi.dll", "GetIfTable2", ks::winapi_monitor::EventCategory::Network, &g_getIfTable2Hook, reinterpret_cast<void*>(&HookedGetIfTable2), reinterpret_cast<void**>(&g_getIfTable2Original) },
            { L"Iphlpapi.dll", "FreeMibTable", ks::winapi_monitor::EventCategory::Network, &g_freeMibTableHook, reinterpret_cast<void*>(&HookedFreeMibTable), reinterpret_cast<void**>(&g_freeMibTableOriginal) },
            { L"Wtsapi32.dll", "WTSOpenServerW", ks::winapi_monitor::EventCategory::Process, &g_wtsOpenServerWHook, reinterpret_cast<void*>(&HookedWTSOpenServerW), reinterpret_cast<void**>(&g_wtsOpenServerWOriginal) },
            { L"Wtsapi32.dll", "WTSOpenServerA", ks::winapi_monitor::EventCategory::Process, &g_wtsOpenServerAHook, reinterpret_cast<void*>(&HookedWTSOpenServerA), reinterpret_cast<void**>(&g_wtsOpenServerAOriginal) },
            { L"Wtsapi32.dll", "WTSCloseServer", ks::winapi_monitor::EventCategory::Process, &g_wtsCloseServerHook, reinterpret_cast<void*>(&HookedWTSCloseServer), reinterpret_cast<void**>(&g_wtsCloseServerOriginal) },
            { L"Wtsapi32.dll", "WTSEnumerateSessionsW", ks::winapi_monitor::EventCategory::Process, &g_wtsEnumerateSessionsWHook, reinterpret_cast<void*>(&HookedWTSEnumerateSessionsW), reinterpret_cast<void**>(&g_wtsEnumerateSessionsWOriginal) },
            { L"Wtsapi32.dll", "WTSEnumerateSessionsA", ks::winapi_monitor::EventCategory::Process, &g_wtsEnumerateSessionsAHook, reinterpret_cast<void*>(&HookedWTSEnumerateSessionsA), reinterpret_cast<void**>(&g_wtsEnumerateSessionsAOriginal) },
            { L"Wtsapi32.dll", "WTSEnumerateProcessesW", ks::winapi_monitor::EventCategory::Process, &g_wtsEnumerateProcessesWHook, reinterpret_cast<void*>(&HookedWTSEnumerateProcessesW), reinterpret_cast<void**>(&g_wtsEnumerateProcessesWOriginal) },
            { L"Wtsapi32.dll", "WTSEnumerateProcessesA", ks::winapi_monitor::EventCategory::Process, &g_wtsEnumerateProcessesAHook, reinterpret_cast<void*>(&HookedWTSEnumerateProcessesA), reinterpret_cast<void**>(&g_wtsEnumerateProcessesAOriginal) },
            { L"Wtsapi32.dll", "WTSQuerySessionInformationW", ks::winapi_monitor::EventCategory::Process, &g_wtsQuerySessionInformationWHook, reinterpret_cast<void*>(&HookedWTSQuerySessionInformationW), reinterpret_cast<void**>(&g_wtsQuerySessionInformationWOriginal) },
            { L"Wtsapi32.dll", "WTSQuerySessionInformationA", ks::winapi_monitor::EventCategory::Process, &g_wtsQuerySessionInformationAHook, reinterpret_cast<void*>(&HookedWTSQuerySessionInformationA), reinterpret_cast<void**>(&g_wtsQuerySessionInformationAOriginal) },
            { L"Wtsapi32.dll", "WTSFreeMemory", ks::winapi_monitor::EventCategory::Process, &g_wtsFreeMemoryHook, reinterpret_cast<void*>(&HookedWTSFreeMemory), reinterpret_cast<void**>(&g_wtsFreeMemoryOriginal) },
            { L"KernelBase.dll", "CreateJobObjectW", ks::winapi_monitor::EventCategory::Process, &g_createJobObjectWHook, reinterpret_cast<void*>(&HookedCreateJobObjectW), reinterpret_cast<void**>(&g_createJobObjectWOriginal) },
            { L"KernelBase.dll", "CreateJobObjectA", ks::winapi_monitor::EventCategory::Process, &g_createJobObjectAHook, reinterpret_cast<void*>(&HookedCreateJobObjectA), reinterpret_cast<void**>(&g_createJobObjectAOriginal) },
            { L"KernelBase.dll", "OpenJobObjectW", ks::winapi_monitor::EventCategory::Process, &g_openJobObjectWHook, reinterpret_cast<void*>(&HookedOpenJobObjectW), reinterpret_cast<void**>(&g_openJobObjectWOriginal) },
            { L"KernelBase.dll", "OpenJobObjectA", ks::winapi_monitor::EventCategory::Process, &g_openJobObjectAHook, reinterpret_cast<void*>(&HookedOpenJobObjectA), reinterpret_cast<void**>(&g_openJobObjectAOriginal) },
            { L"KernelBase.dll", "AssignProcessToJobObject", ks::winapi_monitor::EventCategory::Process, &g_assignProcessToJobObjectHook, reinterpret_cast<void*>(&HookedAssignProcessToJobObject), reinterpret_cast<void**>(&g_assignProcessToJobObjectOriginal) },
            { L"KernelBase.dll", "TerminateJobObject", ks::winapi_monitor::EventCategory::Process, &g_terminateJobObjectHook, reinterpret_cast<void*>(&HookedTerminateJobObject), reinterpret_cast<void**>(&g_terminateJobObjectOriginal) },
            { L"KernelBase.dll", "SetInformationJobObject", ks::winapi_monitor::EventCategory::Process, &g_setInformationJobObjectHook, reinterpret_cast<void*>(&HookedSetInformationJobObject), reinterpret_cast<void**>(&g_setInformationJobObjectOriginal) },
            { L"KernelBase.dll", "QueryInformationJobObject", ks::winapi_monitor::EventCategory::Process, &g_queryInformationJobObjectHook, reinterpret_cast<void*>(&HookedQueryInformationJobObject), reinterpret_cast<void**>(&g_queryInformationJobObjectOriginal) },
            { L"Secur32.dll", "AcquireCredentialsHandleW", ks::winapi_monitor::EventCategory::Process, &g_acquireCredentialsHandleWHook, reinterpret_cast<void*>(&HookedAcquireCredentialsHandleW), reinterpret_cast<void**>(&g_acquireCredentialsHandleWOriginal) },
            { L"Secur32.dll", "AcquireCredentialsHandleA", ks::winapi_monitor::EventCategory::Process, &g_acquireCredentialsHandleAHook, reinterpret_cast<void*>(&HookedAcquireCredentialsHandleA), reinterpret_cast<void**>(&g_acquireCredentialsHandleAOriginal) },
            { L"Secur32.dll", "InitializeSecurityContextW", ks::winapi_monitor::EventCategory::Process, &g_initializeSecurityContextWHook, reinterpret_cast<void*>(&HookedInitializeSecurityContextW), reinterpret_cast<void**>(&g_initializeSecurityContextWOriginal) },
            { L"Secur32.dll", "InitializeSecurityContextA", ks::winapi_monitor::EventCategory::Process, &g_initializeSecurityContextAHook, reinterpret_cast<void*>(&HookedInitializeSecurityContextA), reinterpret_cast<void**>(&g_initializeSecurityContextAOriginal) },
            { L"Secur32.dll", "AcceptSecurityContext", ks::winapi_monitor::EventCategory::Process, &g_acceptSecurityContextHook, reinterpret_cast<void*>(&HookedAcceptSecurityContext), reinterpret_cast<void**>(&g_acceptSecurityContextOriginal) },
            { L"Secur32.dll", "EncryptMessage", ks::winapi_monitor::EventCategory::Process, &g_encryptMessageHook, reinterpret_cast<void*>(&HookedEncryptMessage), reinterpret_cast<void**>(&g_encryptMessageOriginal) },
            { L"Secur32.dll", "DecryptMessage", ks::winapi_monitor::EventCategory::Process, &g_decryptMessageHook, reinterpret_cast<void*>(&HookedDecryptMessage), reinterpret_cast<void**>(&g_decryptMessageOriginal) },
            { L"Secur32.dll", "DeleteSecurityContext", ks::winapi_monitor::EventCategory::Process, &g_deleteSecurityContextHook, reinterpret_cast<void*>(&HookedDeleteSecurityContext), reinterpret_cast<void**>(&g_deleteSecurityContextOriginal) },
            { L"Secur32.dll", "FreeCredentialsHandle", ks::winapi_monitor::EventCategory::Process, &g_freeCredentialsHandleHook, reinterpret_cast<void*>(&HookedFreeCredentialsHandle), reinterpret_cast<void**>(&g_freeCredentialsHandleOriginal) },
            // Sixth batch bindings 作用：
            // - 输入：新增枚举、路径、IPC、Winsock、HTTP 和 ntdll 对象层 wrapper；
            // - 处理：继续复用既有 inline hook 安装流程，不引入新的外部 hook 框架；
            // - 返回：本静态表无返回值，新增项随 InstallConfiguredHooks 统一安装/卸载。
            { L"Kernel32.dll", "Process32FirstW", ks::winapi_monitor::EventCategory::Process, &g_process32FirstWHook, reinterpret_cast<void*>(&HookedProcess32FirstW), reinterpret_cast<void**>(&g_process32FirstWOriginal) },
            { L"Kernel32.dll", "Process32First", ks::winapi_monitor::EventCategory::Process, &g_process32FirstAHook, reinterpret_cast<void*>(&HookedProcess32FirstA), reinterpret_cast<void**>(&g_process32FirstAOriginal) },
            { L"Kernel32.dll", "Process32NextW", ks::winapi_monitor::EventCategory::Process, &g_process32NextWHook, reinterpret_cast<void*>(&HookedProcess32NextW), reinterpret_cast<void**>(&g_process32NextWOriginal) },
            { L"Kernel32.dll", "Process32Next", ks::winapi_monitor::EventCategory::Process, &g_process32NextAHook, reinterpret_cast<void*>(&HookedProcess32NextA), reinterpret_cast<void**>(&g_process32NextAOriginal) },
            { L"Kernel32.dll", "Thread32First", ks::winapi_monitor::EventCategory::Process, &g_thread32FirstHook, reinterpret_cast<void*>(&HookedThread32First), reinterpret_cast<void**>(&g_thread32FirstOriginal) },
            { L"Kernel32.dll", "Thread32Next", ks::winapi_monitor::EventCategory::Process, &g_thread32NextHook, reinterpret_cast<void*>(&HookedThread32Next), reinterpret_cast<void**>(&g_thread32NextOriginal) },
            { L"Kernel32.dll", "Heap32ListFirst", ks::winapi_monitor::EventCategory::Process, &g_heap32ListFirstHook, reinterpret_cast<void*>(&HookedHeap32ListFirst), reinterpret_cast<void**>(&g_heap32ListFirstOriginal) },
            { L"Kernel32.dll", "Heap32ListNext", ks::winapi_monitor::EventCategory::Process, &g_heap32ListNextHook, reinterpret_cast<void*>(&HookedHeap32ListNext), reinterpret_cast<void**>(&g_heap32ListNextOriginal) },
            { L"Kernel32.dll", "Heap32First", ks::winapi_monitor::EventCategory::Process, &g_heap32FirstHook, reinterpret_cast<void*>(&HookedHeap32First), reinterpret_cast<void**>(&g_heap32FirstOriginal) },
            { L"Kernel32.dll", "Heap32Next", ks::winapi_monitor::EventCategory::Process, &g_heap32NextHook, reinterpret_cast<void*>(&HookedHeap32Next), reinterpret_cast<void**>(&g_heap32NextOriginal) },
            { L"KernelBase.dll", "QueryFullProcessImageNameW", ks::winapi_monitor::EventCategory::Process, &g_queryFullProcessImageNameWHook, reinterpret_cast<void*>(&HookedQueryFullProcessImageNameW), reinterpret_cast<void**>(&g_queryFullProcessImageNameWOriginal) },
            { L"KernelBase.dll", "QueryFullProcessImageNameA", ks::winapi_monitor::EventCategory::Process, &g_queryFullProcessImageNameAHook, reinterpret_cast<void*>(&HookedQueryFullProcessImageNameA), reinterpret_cast<void**>(&g_queryFullProcessImageNameAOriginal) },
            { L"Psapi.dll", "GetProcessImageFileNameW", ks::winapi_monitor::EventCategory::Process, &g_getProcessImageFileNameWHook, reinterpret_cast<void*>(&HookedGetProcessImageFileNameW), reinterpret_cast<void**>(&g_getProcessImageFileNameWOriginal) },
            { L"Psapi.dll", "GetProcessImageFileNameA", ks::winapi_monitor::EventCategory::Process, &g_getProcessImageFileNameAHook, reinterpret_cast<void*>(&HookedGetProcessImageFileNameA), reinterpret_cast<void**>(&g_getProcessImageFileNameAOriginal) },
            { L"KernelBase.dll", "GetProcessId", ks::winapi_monitor::EventCategory::Process, &g_getProcessIdHook, reinterpret_cast<void*>(&HookedGetProcessId), reinterpret_cast<void**>(&g_getProcessIdOriginal) },
            { L"KernelBase.dll", "GetThreadId", ks::winapi_monitor::EventCategory::Process, &g_getThreadIdHook, reinterpret_cast<void*>(&HookedGetThreadId), reinterpret_cast<void**>(&g_getThreadIdOriginal) },
            { L"KernelBase.dll", "IsWow64Process", ks::winapi_monitor::EventCategory::Process, &g_isWow64ProcessHook, reinterpret_cast<void*>(&HookedIsWow64Process), reinterpret_cast<void**>(&g_isWow64ProcessOriginal) },
            { L"KernelBase.dll", "IsWow64Process2", ks::winapi_monitor::EventCategory::Process, &g_isWow64Process2Hook, reinterpret_cast<void*>(&HookedIsWow64Process2), reinterpret_cast<void**>(&g_isWow64Process2Original) },
            { L"KernelBase.dll", "Wow64DisableWow64FsRedirection", ks::winapi_monitor::EventCategory::File, &g_wow64DisableWow64FsRedirectionHook, reinterpret_cast<void*>(&HookedWow64DisableWow64FsRedirection), reinterpret_cast<void**>(&g_wow64DisableWow64FsRedirectionOriginal) },
            { L"KernelBase.dll", "Wow64RevertWow64FsRedirection", ks::winapi_monitor::EventCategory::File, &g_wow64RevertWow64FsRedirectionHook, reinterpret_cast<void*>(&HookedWow64RevertWow64FsRedirection), reinterpret_cast<void**>(&g_wow64RevertWow64FsRedirectionOriginal) },
            { L"KernelBase.dll", "GetTempPathW", ks::winapi_monitor::EventCategory::File, &g_getTempPathWHook, reinterpret_cast<void*>(&HookedGetTempPathW), reinterpret_cast<void**>(&g_getTempPathWOriginal) },
            { L"KernelBase.dll", "GetTempPathA", ks::winapi_monitor::EventCategory::File, &g_getTempPathAHook, reinterpret_cast<void*>(&HookedGetTempPathA), reinterpret_cast<void**>(&g_getTempPathAOriginal) },
            { L"KernelBase.dll", "GetTempFileNameW", ks::winapi_monitor::EventCategory::File, &g_getTempFileNameWHook, reinterpret_cast<void*>(&HookedGetTempFileNameW), reinterpret_cast<void**>(&g_getTempFileNameWOriginal) },
            { L"KernelBase.dll", "GetTempFileNameA", ks::winapi_monitor::EventCategory::File, &g_getTempFileNameAHook, reinterpret_cast<void*>(&HookedGetTempFileNameA), reinterpret_cast<void**>(&g_getTempFileNameAOriginal) },
            { L"KernelBase.dll", "GetFullPathNameW", ks::winapi_monitor::EventCategory::File, &g_getFullPathNameWHook, reinterpret_cast<void*>(&HookedGetFullPathNameW), reinterpret_cast<void**>(&g_getFullPathNameWOriginal) },
            { L"KernelBase.dll", "GetFullPathNameA", ks::winapi_monitor::EventCategory::File, &g_getFullPathNameAHook, reinterpret_cast<void*>(&HookedGetFullPathNameA), reinterpret_cast<void**>(&g_getFullPathNameAOriginal) },
            { L"KernelBase.dll", "SearchPathW", ks::winapi_monitor::EventCategory::File, &g_searchPathWHook, reinterpret_cast<void*>(&HookedSearchPathW), reinterpret_cast<void**>(&g_searchPathWOriginal) },
            { L"KernelBase.dll", "SearchPathA", ks::winapi_monitor::EventCategory::File, &g_searchPathAHook, reinterpret_cast<void*>(&HookedSearchPathA), reinterpret_cast<void**>(&g_searchPathAOriginal) },
            { L"KernelBase.dll", "GetShortPathNameW", ks::winapi_monitor::EventCategory::File, &g_getShortPathNameWHook, reinterpret_cast<void*>(&HookedGetShortPathNameW), reinterpret_cast<void**>(&g_getShortPathNameWOriginal) },
            { L"KernelBase.dll", "GetShortPathNameA", ks::winapi_monitor::EventCategory::File, &g_getShortPathNameAHook, reinterpret_cast<void*>(&HookedGetShortPathNameA), reinterpret_cast<void**>(&g_getShortPathNameAOriginal) },
            { L"KernelBase.dll", "GetLongPathNameW", ks::winapi_monitor::EventCategory::File, &g_getLongPathNameWHook, reinterpret_cast<void*>(&HookedGetLongPathNameW), reinterpret_cast<void**>(&g_getLongPathNameWOriginal) },
            { L"KernelBase.dll", "GetLongPathNameA", ks::winapi_monitor::EventCategory::File, &g_getLongPathNameAHook, reinterpret_cast<void*>(&HookedGetLongPathNameA), reinterpret_cast<void**>(&g_getLongPathNameAOriginal) },
            { L"KernelBase.dll", "CreatePipe", ks::winapi_monitor::EventCategory::File, &g_createPipeHook, reinterpret_cast<void*>(&HookedCreatePipe), reinterpret_cast<void**>(&g_createPipeOriginal) },
            { L"KernelBase.dll", "CreateMailslotW", ks::winapi_monitor::EventCategory::File, &g_createMailslotWHook, reinterpret_cast<void*>(&HookedCreateMailslotW), reinterpret_cast<void**>(&g_createMailslotWOriginal) },
            { L"KernelBase.dll", "CreateMailslotA", ks::winapi_monitor::EventCategory::File, &g_createMailslotAHook, reinterpret_cast<void*>(&HookedCreateMailslotA), reinterpret_cast<void**>(&g_createMailslotAOriginal) },
            { L"KernelBase.dll", "CreateDirectoryExW", ks::winapi_monitor::EventCategory::File, &g_createDirectoryExWHook, reinterpret_cast<void*>(&HookedCreateDirectoryExW), reinterpret_cast<void**>(&g_createDirectoryExWOriginal) },
            { L"KernelBase.dll", "CreateDirectoryExA", ks::winapi_monitor::EventCategory::File, &g_createDirectoryExAHook, reinterpret_cast<void*>(&HookedCreateDirectoryExA), reinterpret_cast<void**>(&g_createDirectoryExAOriginal) },
            { L"Ws2_32.dll", "WSAStartup", ks::winapi_monitor::EventCategory::Network, &g_wsaStartupHook, reinterpret_cast<void*>(&HookedWSAStartup), reinterpret_cast<void**>(&g_wsaStartupOriginal) },
            { L"Ws2_32.dll", "WSACleanup", ks::winapi_monitor::EventCategory::Network, &g_wsaCleanupHook, reinterpret_cast<void*>(&HookedWSACleanup), reinterpret_cast<void**>(&g_wsaCleanupOriginal) },
            { L"Ws2_32.dll", "select", ks::winapi_monitor::EventCategory::Network, &g_selectHook, reinterpret_cast<void*>(&HookedSelect), reinterpret_cast<void**>(&g_selectOriginal) },
            { L"Ws2_32.dll", "ioctlsocket", ks::winapi_monitor::EventCategory::Network, &g_ioctlSocketHook, reinterpret_cast<void*>(&HookedIoctlSocket), reinterpret_cast<void**>(&g_ioctlSocketOriginal) },
            { L"Ws2_32.dll", "setsockopt", ks::winapi_monitor::EventCategory::Network, &g_setSockOptHook, reinterpret_cast<void*>(&HookedSetSockOpt), reinterpret_cast<void**>(&g_setSockOptOriginal) },
            { L"Ws2_32.dll", "getsockopt", ks::winapi_monitor::EventCategory::Network, &g_getSockOptHook, reinterpret_cast<void*>(&HookedGetSockOpt), reinterpret_cast<void**>(&g_getSockOptOriginal) },
            { L"Ws2_32.dll", "getsockname", ks::winapi_monitor::EventCategory::Network, &g_getSockNameHook, reinterpret_cast<void*>(&HookedGetSockName), reinterpret_cast<void**>(&g_getSockNameOriginal) },
            { L"Ws2_32.dll", "getpeername", ks::winapi_monitor::EventCategory::Network, &g_getPeerNameHook, reinterpret_cast<void*>(&HookedGetPeerName), reinterpret_cast<void**>(&g_getPeerNameOriginal) },
            { L"Ws2_32.dll", "WSAEventSelect", ks::winapi_monitor::EventCategory::Network, &g_wsaEventSelectHook, reinterpret_cast<void*>(&HookedWSAEventSelect), reinterpret_cast<void**>(&g_wsaEventSelectOriginal) },
            { L"Ws2_32.dll", "WSAAsyncSelect", ks::winapi_monitor::EventCategory::Network, &g_wsaAsyncSelectHook, reinterpret_cast<void*>(&HookedWSAAsyncSelect), reinterpret_cast<void**>(&g_wsaAsyncSelectOriginal) },
            { L"Ws2_32.dll", "gethostbyname", ks::winapi_monitor::EventCategory::Network, &g_getHostByNameHook, reinterpret_cast<void*>(&HookedGetHostByName), reinterpret_cast<void**>(&g_getHostByNameOriginal) },
            { L"Ws2_32.dll", "gethostbyaddr", ks::winapi_monitor::EventCategory::Network, &g_getHostByAddrHook, reinterpret_cast<void*>(&HookedGetHostByAddr), reinterpret_cast<void**>(&g_getHostByAddrOriginal) },
            { L"Ws2_32.dll", "GetNameInfoW", ks::winapi_monitor::EventCategory::Network, &g_getNameInfoWHook, reinterpret_cast<void*>(&HookedGetNameInfoW), reinterpret_cast<void**>(&g_getNameInfoWOriginal) },
            { L"Ws2_32.dll", "getnameinfo", ks::winapi_monitor::EventCategory::Network, &g_getNameInfoAHook, reinterpret_cast<void*>(&HookedGetNameInfoA), reinterpret_cast<void**>(&g_getNameInfoAOriginal) },
            { L"Winhttp.dll", "WinHttpAddRequestHeaders", ks::winapi_monitor::EventCategory::Network, &g_winHttpAddRequestHeadersHook, reinterpret_cast<void*>(&HookedWinHttpAddRequestHeaders), reinterpret_cast<void**>(&g_winHttpAddRequestHeadersOriginal) },
            { L"Winhttp.dll", "WinHttpSetCredentials", ks::winapi_monitor::EventCategory::Network, &g_winHttpSetCredentialsHook, reinterpret_cast<void*>(&HookedWinHttpSetCredentials), reinterpret_cast<void**>(&g_winHttpSetCredentialsOriginal) },
            { L"Winhttp.dll", "WinHttpCrackUrl", ks::winapi_monitor::EventCategory::Network, &g_winHttpCrackUrlHook, reinterpret_cast<void*>(&HookedWinHttpCrackUrl), reinterpret_cast<void**>(&g_winHttpCrackUrlOriginal) },
            { L"Winhttp.dll", "WinHttpCreateUrl", ks::winapi_monitor::EventCategory::Network, &g_winHttpCreateUrlHook, reinterpret_cast<void*>(&HookedWinHttpCreateUrl), reinterpret_cast<void**>(&g_winHttpCreateUrlOriginal) },
            { L"Winhttp.dll", "WinHttpSetTimeouts", ks::winapi_monitor::EventCategory::Network, &g_winHttpSetTimeoutsHook, reinterpret_cast<void*>(&HookedWinHttpSetTimeouts), reinterpret_cast<void**>(&g_winHttpSetTimeoutsOriginal) },
            { L"Wininet.dll", "HttpQueryInfoW", ks::winapi_monitor::EventCategory::Network, &g_httpQueryInfoWHook, reinterpret_cast<void*>(&HookedHttpQueryInfoW), reinterpret_cast<void**>(&g_httpQueryInfoWOriginal) },
            { L"Wininet.dll", "HttpQueryInfoA", ks::winapi_monitor::EventCategory::Network, &g_httpQueryInfoAHook, reinterpret_cast<void*>(&HookedHttpQueryInfoA), reinterpret_cast<void**>(&g_httpQueryInfoAOriginal) },
            { L"Wininet.dll", "InternetQueryOptionW", ks::winapi_monitor::EventCategory::Network, &g_internetQueryOptionWHook, reinterpret_cast<void*>(&HookedInternetQueryOptionW), reinterpret_cast<void**>(&g_internetQueryOptionWOriginal) },
            { L"Wininet.dll", "InternetQueryOptionA", ks::winapi_monitor::EventCategory::Network, &g_internetQueryOptionAHook, reinterpret_cast<void*>(&HookedInternetQueryOptionA), reinterpret_cast<void**>(&g_internetQueryOptionAOriginal) },
            { L"ntdll.dll", "NtOpenDirectoryObject", ks::winapi_monitor::EventCategory::Process, &g_ntOpenDirectoryObjectHook, reinterpret_cast<void*>(&HookedNtOpenDirectoryObject), reinterpret_cast<void**>(&g_ntOpenDirectoryObjectOriginal) },
            { L"ntdll.dll", "NtQueryDirectoryObject", ks::winapi_monitor::EventCategory::Process, &g_ntQueryDirectoryObjectHook, reinterpret_cast<void*>(&HookedNtQueryDirectoryObject), reinterpret_cast<void**>(&g_ntQueryDirectoryObjectOriginal) },
            { L"ntdll.dll", "NtCreateSymbolicLinkObject", ks::winapi_monitor::EventCategory::Process, &g_ntCreateSymbolicLinkObjectHook, reinterpret_cast<void*>(&HookedNtCreateSymbolicLinkObject), reinterpret_cast<void**>(&g_ntCreateSymbolicLinkObjectOriginal) },
            { L"ntdll.dll", "NtOpenSymbolicLinkObject", ks::winapi_monitor::EventCategory::Process, &g_ntOpenSymbolicLinkObjectHook, reinterpret_cast<void*>(&HookedNtOpenSymbolicLinkObject), reinterpret_cast<void**>(&g_ntOpenSymbolicLinkObjectOriginal) },
            { L"ntdll.dll", "NtQuerySymbolicLinkObject", ks::winapi_monitor::EventCategory::Process, &g_ntQuerySymbolicLinkObjectHook, reinterpret_cast<void*>(&HookedNtQuerySymbolicLinkObject), reinterpret_cast<void**>(&g_ntQuerySymbolicLinkObjectOriginal) },
            { L"ntdll.dll", "NtCreateSemaphore", ks::winapi_monitor::EventCategory::Process, &g_ntCreateSemaphoreHook, reinterpret_cast<void*>(&HookedNtCreateSemaphore), reinterpret_cast<void**>(&g_ntCreateSemaphoreOriginal) },
            { L"ntdll.dll", "NtOpenSemaphore", ks::winapi_monitor::EventCategory::Process, &g_ntOpenSemaphoreHook, reinterpret_cast<void*>(&HookedNtOpenSemaphore), reinterpret_cast<void**>(&g_ntOpenSemaphoreOriginal) },
            { L"ntdll.dll", "NtReleaseSemaphore", ks::winapi_monitor::EventCategory::Process, &g_ntReleaseSemaphoreHook, reinterpret_cast<void*>(&HookedNtReleaseSemaphore), reinterpret_cast<void**>(&g_ntReleaseSemaphoreOriginal) }
        };

        bool StartsWithWide(const std::wstring& textValue, const std::wstring& prefixValue)
        {
            return textValue.size() >= prefixValue.size()
                && std::equal(prefixValue.begin(), prefixValue.end(), textValue.begin());
        }

        bool IsStrongTypedExport(const std::wstring& moduleName, const std::string& procName)
        {
            const std::wstring targetKey = MakeRawHookKey(moduleName, procName);
            for (const HookBinding& bindingValue : g_bindings)
            {
                if (bindingValue.moduleName == nullptr || bindingValue.procName == nullptr)
                {
                    continue;
                }
                if (MakeRawHookKey(bindingValue.moduleName, bindingValue.procName) == targetKey)
                {
                    return true;
                }
            }
            return false;
        }

        bool IsUnsafeRawFallbackModule(const std::wstring& moduleName)
        {
            // IsUnsafeRawFallbackModule 作用：
            // - 输入：Raw Fallback 配置中的模块名；
            // - 处理：识别不适合通用 ABI 兜底 hook 的底层模块；
            // - 返回：true 表示 Raw 枚举应跳过该模块，强类型 hook 和精确 Fake Success 不受影响。
            // - 原因：ntdll 导出包含 syscall、loader、运行时和内部调度面；即使排除 Nt/Rtl/Ldr 前缀，
            //   剩余导出仍可能在 CRT/loader/异常处理路径被高频调用，通用 Raw trampoline 风险过高。
            return NormalizeModuleNameForMatch(moduleName) == L"ntdll";
        }

        bool MatchesRawDenyPattern(const std::string& procName, const std::wstring& patternText)
        {
            if (procName.empty() || patternText.empty())
            {
                return false;
            }

            const std::wstring lowerProcName = ToLowerWide(AnsiToWide(procName.c_str()));
            std::wstring lowerPattern = ToLowerWide(patternText);
            if (!lowerPattern.empty() && lowerPattern.back() == L'*')
            {
                lowerPattern.pop_back();
                return !lowerPattern.empty() && StartsWithWide(lowerProcName, lowerPattern);
            }
            return lowerProcName == lowerPattern;
        }

        std::vector<std::wstring> SplitRawDenyPatternText(const wchar_t* const patternText)
        {
            // SplitRawDenyPatternText 作用：
            // - 输入：共享协议中的内置 Raw 黑名单文本；
            // - 处理：按分号、逗号或换行拆分，并去掉每项首尾空白；
            // - 返回：可供 MatchesRawDenyPattern 逐项匹配的规则列表。
            std::vector<std::wstring> patternList;
            if (patternText == nullptr || patternText[0] == L'\0')
            {
                return patternList;
            }

            std::wstring currentPattern;
            const auto flushPattern = [&patternList, &currentPattern]() {
                const auto firstIt = std::find_if_not(
                    currentPattern.begin(),
                    currentPattern.end(),
                    [](const wchar_t ch) { return ch == L' ' || ch == L'\t' || ch == L'\r' || ch == L'\n'; });
                const auto lastIt = std::find_if_not(
                    currentPattern.rbegin(),
                    currentPattern.rend(),
                    [](const wchar_t ch) { return ch == L' ' || ch == L'\t' || ch == L'\r' || ch == L'\n'; }).base();
                if (firstIt < lastIt)
                {
                    patternList.emplace_back(firstIt, lastIt);
                }
                currentPattern.clear();
            };

            for (const wchar_t ch : std::wstring(patternText))
            {
                if (ch == L';' || ch == L',' || ch == L'\r' || ch == L'\n')
                {
                    flushPattern();
                    continue;
                }
                currentPattern.push_back(ch);
            }
            flushPattern();
            return patternList;
        }

        const std::vector<std::wstring>& DefaultRawDenyPatterns()
        {
            // DefaultRawDenyPatterns 作用：
            // - 输入：无；
            // - 处理：懒加载共享默认黑名单，避免每次枚举导出都重复拆分字符串；
            // - 返回：进程内只读规则列表，调用方不得修改。
            static const std::vector<std::wstring> defaultPatternList =
                SplitRawDenyPatternText(ks::winapi_monitor::kDefaultRawHookDenyList);
            return defaultPatternList;
        }

        bool IsRawDeniedByConfig(const std::string& procName)
        {
            const MonitorConfig& configValue = ActiveConfig();
            if (configValue.rawUseDefaultDenyList)
            {
                for (const std::wstring& patternText : DefaultRawDenyPatterns())
                {
                    if (MatchesRawDenyPattern(procName, patternText))
                    {
                        return true;
                    }
                }
            }

            // 用户额外黑名单始终生效：
            // - 默认黑名单可以被用户关闭；
            // - 下方自定义规则仍然用于兜底 Raw Hook，便于临时压制某个目标进程的噪声 API。
            for (const std::wstring& patternText : configValue.rawDenyList)
            {
                if (MatchesRawDenyPattern(procName, patternText))
                {
                    return true;
                }
            }
            return false;
        }

        bool ExportNameLooksHookable(const std::string& procName)
        {
            if (procName.empty())
            {
                return false;
            }
            if (procName[0] == '?' || procName[0] == '_')
            {
                return false;
            }
            for (const unsigned char ch : procName)
            {
                if (ch < 0x20 || ch >= 0x7F)
                {
                    return false;
                }
            }
            return true;
        }

        ks::winapi_monitor::EventCategory InferRawHookCategory(const std::wstring& moduleName, const std::string& procName)
        {
            const std::wstring moduleLower = ToLowerWide(moduleName);
            const std::string procLower = ToLowerAnsi(procName);
            if (moduleLower == L"ws2_32.dll" || moduleLower == L"wininet.dll" || moduleLower == L"winhttp.dll"
                || moduleLower == L"iphlpapi.dll" || moduleLower == L"dnsapi.dll" || moduleLower == L"netapi32.dll"
                || moduleLower == L"urlmon.dll" || moduleLower == L"wldap32.dll")
            {
                return ks::winapi_monitor::EventCategory::Network;
            }
            if (procLower.rfind("reg", 0) == 0 || procLower.find("key") != std::string::npos)
            {
                return ks::winapi_monitor::EventCategory::Registry;
            }
            if (procLower.find("file") != std::string::npos || procLower.find("directory") != std::string::npos
                || procLower.find("path") != std::string::npos || procLower.find("pipe") != std::string::npos)
            {
                return ks::winapi_monitor::EventCategory::File;
            }
            if (procLower.find("library") != std::string::npos || procLower.find("module") != std::string::npos
                || procLower.rfind("ldr", 0) == 0)
            {
                return ks::winapi_monitor::EventCategory::Loader;
            }
            return ks::winapi_monitor::EventCategory::Process;
        }

        bool EnumerateNamedExports(HMODULE moduleHandle, std::vector<std::string>* exportNamesOut)
        {
            if (moduleHandle == nullptr || exportNamesOut == nullptr)
            {
                return false;
            }
            exportNamesOut->clear();

            const auto* const basePointer = reinterpret_cast<const unsigned char*>(moduleHandle);
            const auto* const dosHeader = reinterpret_cast<const IMAGE_DOS_HEADER*>(basePointer);
            if (dosHeader->e_magic != IMAGE_DOS_SIGNATURE || dosHeader->e_lfanew <= 0)
            {
                return false;
            }

            const auto* const ntHeader = reinterpret_cast<const IMAGE_NT_HEADERS64*>(basePointer + dosHeader->e_lfanew);
            if (ntHeader->Signature != IMAGE_NT_SIGNATURE)
            {
                return false;
            }

            const IMAGE_DATA_DIRECTORY& exportDirectory =
                ntHeader->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXPORT];
            if (exportDirectory.VirtualAddress == 0 || exportDirectory.Size < sizeof(IMAGE_EXPORT_DIRECTORY))
            {
                return false;
            }

            const DWORD imageSize = ntHeader->OptionalHeader.SizeOfImage;
            const auto rvaToPointer = [basePointer, imageSize](const DWORD rvaValue) -> const void* {
                if (rvaValue == 0 || rvaValue >= imageSize)
                {
                    return nullptr;
                }
                return basePointer + rvaValue;
            };

            const auto* const exportTable = static_cast<const IMAGE_EXPORT_DIRECTORY*>(
                rvaToPointer(exportDirectory.VirtualAddress));
            if (exportTable == nullptr || exportTable->NumberOfNames == 0 || exportTable->AddressOfNames == 0)
            {
                return false;
            }

            const auto* const nameRvaList = static_cast<const DWORD*>(rvaToPointer(exportTable->AddressOfNames));
            if (nameRvaList == nullptr)
            {
                return false;
            }

            exportNamesOut->reserve(exportTable->NumberOfNames);
            for (DWORD indexValue = 0; indexValue < exportTable->NumberOfNames; ++indexValue)
            {
                const char* const namePointer = static_cast<const char*>(rvaToPointer(nameRvaList[indexValue]));
                if (namePointer == nullptr || namePointer[0] == '\0')
                {
                    continue;
                }
                exportNamesOut->push_back(namePointer);
            }

            std::sort(exportNamesOut->begin(), exportNamesOut->end());
            exportNamesOut->erase(
                std::unique(exportNamesOut->begin(), exportNamesOut->end()),
                exportNamesOut->end());
            return !exportNamesOut->empty();
        }

        bool TryInstallRawHookBinding(RawHookBinding& bindingValue)
        {
            if (bindingValue.hookRecord.installed || bindingValue.hookRecord.permanentlyDisabled)
            {
                return bindingValue.hookRecord.installed;
            }
            if (bindingValue.entryStubAddress == nullptr)
            {
                bindingValue.entryStubAddress = BuildRawEntryStub(&bindingValue);
                if (bindingValue.entryStubAddress == nullptr)
                {
                    bindingValue.hookRecord.permanentlyDisabled = true;
                    return false;
                }
            }

            std::wstring ignoredErrorText;
            const InlineHookInstallResult installResult = InstallInlineHook(
                bindingValue.moduleName.c_str(),
                bindingValue.procName.c_str(),
                bindingValue.entryStubAddress,
                &bindingValue.hookRecord,
                &bindingValue.originalAddress,
                &ignoredErrorText);
            if (installResult == InlineHookInstallResult::Installed)
            {
                return true;
            }
            if (installResult == InlineHookInstallResult::PermanentFailure)
            {
                bindingValue.hookRecord.permanentlyDisabled = true;
            }
            return false;
        }

        bool InstallFakeSuccessHooks(std::wstring* const detailTextOut)
        {
            // InstallFakeSuccessHooks 作用：
            // - 输入：已由 BuildFakeSuccessRuleIndex 准备好的 Fake Success 规则集合；
            // - 处理：仅在 fake_success_raw_fallback=1 时补装未被强类型表覆盖的精确 module!api 规则；
            // - 返回：至少一条 Fake Success 规则已安装/安装成功时返回 true。
            const MonitorConfig& configValue = ActiveConfig();
            auto& ruleList = FakeSuccessRules();
            if (!configValue.fakeSuccessEnabled || !configValue.fakeSuccessRawFallback || ruleList.empty())
            {
                return false;
            }

            bool installedAny = false;
            for (std::unique_ptr<FakeSuccessRuntimeRule>& rulePointer : ruleList)
            {
                if (rulePointer == nullptr)
                {
                    continue;
                }
                installedAny = TryInstallFakeSuccessRule(*rulePointer, std::nullopt, detailTextOut) || installedAny;
            }
            return installedAny;
        }

        void UninstallFakeSuccessHooks()
        {
            // UninstallFakeSuccessHooks 作用：
            // - 输入：无，使用当前 Fake Success 运行时规则表；
            // - 处理：撤销 inline patch，释放 trampoline 和动态 fake-return stub，并清空索引；
            // - 返回：无返回值。
            auto& ruleList = FakeSuccessRules();
            for (std::unique_ptr<FakeSuccessRuntimeRule>& rulePointer : ruleList)
            {
                if (rulePointer == nullptr)
                {
                    continue;
                }
                UninstallInlineHook(&rulePointer->hookRecord);
                rulePointer->originalAddress = nullptr;
                FreeFakeSuccessEntryStub(rulePointer->entryStubAddress);
                rulePointer->entryStubAddress = nullptr;
            }
            ruleList.clear();
            FakeSuccessRuleMap().clear();
        }

        void DiscoverRawHookBindingsForLoadedModules()
        {
            const MonitorConfig& configValue = ActiveConfig();
            if (!configValue.enableRawFallback)
            {
                return;
            }

            constexpr std::size_t kMaxRawExportsPerModule = 768;
            std::vector<std::string> exportNameList;
            for (const std::wstring& moduleName : configValue.rawModuleList)
            {
                if (moduleName.empty())
                {
                    continue;
                }
                if (IsUnsafeRawFallbackModule(moduleName))
                {
                    continue;
                }

                HMODULE moduleHandle = ::GetModuleHandleW(moduleName.c_str());
                if (moduleHandle == nullptr)
                {
                    continue;
                }
                if (!EnumerateNamedExports(moduleHandle, &exportNameList))
                {
                    continue;
                }

                std::size_t acceptedCount = 0;
                for (const std::string& exportName : exportNameList)
                {
                    if (acceptedCount >= kMaxRawExportsPerModule)
                    {
                        break;
                    }
                    if (!ExportNameLooksHookable(exportName)
                        || IsStrongTypedExport(moduleName, exportName)
                        || (configValue.fakeSuccessRawFallback && FindFakeSuccessRule(moduleName, exportName) != nullptr)
                        || IsRawDeniedByConfig(exportName))
                    {
                        continue;
                    }

                    const std::wstring rawKey = MakeRawHookKey(moduleName, exportName);
                    auto& rawKeySet = RawHookKeys();
                    auto& rawBindingList = RawBindings();
                    if (rawKeySet.find(rawKey) != rawKeySet.end())
                    {
                        continue;
                    }

                    auto bindingPointer = std::make_unique<RawHookBinding>();
                    bindingPointer->moduleName = moduleName;
                    bindingPointer->procName = exportName;
                    bindingPointer->procNameWide = AnsiToWide(exportName.c_str());
                    bindingPointer->categoryValue = InferRawHookCategory(moduleName, exportName);
                    rawKeySet.insert(rawKey);
                    rawBindingList.push_back(std::move(bindingPointer));
                    ++acceptedCount;
                }
            }
        }

        bool InstallRawFallbackHooks()
        {
            const MonitorConfig& configValue = ActiveConfig();
            if (!configValue.enableRawFallback)
            {
                return false;
            }

            DiscoverRawHookBindingsForLoadedModules();

            bool installedAny = false;
            for (std::unique_ptr<RawHookBinding>& bindingPointer : RawBindings())
            {
                if (bindingPointer == nullptr)
                {
                    continue;
                }
                installedAny = TryInstallRawHookBinding(*bindingPointer) || installedAny;
            }
            return installedAny;
        }

        void UninstallRawFallbackHooks()
        {
            auto& rawBindingList = RawBindings();
            for (std::unique_ptr<RawHookBinding>& bindingPointer : rawBindingList)
            {
                if (bindingPointer == nullptr)
                {
                    continue;
                }
                UninstallInlineHook(&bindingPointer->hookRecord);
                bindingPointer->originalAddress = nullptr;
                FreeRawEntryStub(bindingPointer->entryStubAddress);
                bindingPointer->entryStubAddress = nullptr;
            }
            rawBindingList.clear();
            RawHookKeys().clear();
        }

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
            (void)InstallFakeSuccessHooks(nullptr);
            (void)InstallRawFallbackHooks();
        }

        // RetryPendingHooksFromHook 作用：
        // - 输入：无；
        // - Processing: after the LoadLibrary/LdrLoadDll hooked wrapper returns, try to acquire the hook lock.
        // - 返回：无返回值，锁正忙时跳过本轮补装。
        void RetryPendingHooksFromHook()
        {
            if (g_hookOperationMutex.try_lock())
            {
                ScopedInlineHookInternalBypass hookOperationBypassScope;
                RetryPendingHooksUnlocked();
                g_hookOperationMutex.unlock();
            }
        }

    }

    bool InstallConfiguredHooks(std::wstring* errorTextOut)
    {
        const std::lock_guard<std::mutex> lock(g_hookOperationMutex);
        ScopedInlineHookInternalBypass hookOperationBypassScope;
        if (errorTextOut != nullptr)
        {
            errorTextOut->clear();
        }

        bool hasEnabledCategory = false;
        bool installedAny = false;
        std::wstring failureText;
        BuildFakeSuccessRuleIndex();
        for (HookBinding& bindingValue : g_bindings)
        {
            hasEnabledCategory = CategoryEnabled(bindingValue.categoryValue) || hasEnabledCategory;
            installedAny = TryInstallBinding(bindingValue, &failureText) || installedAny;
        }
        hasEnabledCategory = ActiveConfig().enableRawFallback
            || (ActiveConfig().fakeSuccessEnabled && !FakeSuccessRules().empty())
            || hasEnabledCategory;
        installedAny = InstallFakeSuccessHooks(&failureText) || installedAny;
        installedAny = InstallRawFallbackHooks() || installedAny;
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
        ScopedInlineHookInternalBypass hookOperationBypassScope;
        for (HookBinding& bindingValue : g_bindings)
        {
            UninstallInlineHook(bindingValue.hookRecord);
            if (bindingValue.originalOut != nullptr)
            {
                *bindingValue.originalOut = nullptr;
            }
        }
        UninstallRawFallbackHooks();
        UninstallFakeSuccessHooks();
    }
    void RetryPendingHooks()
    {
        const std::lock_guard<std::mutex> lock(g_hookOperationMutex);
        ScopedInlineHookInternalBypass hookOperationBypassScope;
        RetryPendingHooksUnlocked();
    }
}

