#ifdef KSWORD_WITH_COMMAND
#pragma once
#include "../../KswordTotalHead.h"
//#define UNICODE
//#define _UNICODE
typedef LONG    KPRIORITY;
#define SystemProcessInformation    5 // 功能号
typedef NTSTATUS(NTAPI* _NtQuerySystemInformation)(
    ULONG SystemInformationClass,
    PVOID SystemInformation,
    ULONG SystemInformationLength,
    PULONG ReturnLength
    );
// SYSTEM_HANDLE_TABLE_ENTRY_INFO定义（来自NtQuerySystemInformation返回的数据结构）
typedef struct _SYSTEM_HANDLE_TABLE_ENTRY_INFO {
    USHORT UniqueProcessId;     // 进程ID（USHORT）
    USHORT CreatorBackTraceIndex;
    UCHAR ObjectTypeIndex;      // 对象类型编号
    UCHAR HandleAttributes;     // 句柄属性
    USHORT HandleValue;         // 句柄值（USHORT）
    PVOID Object;               // 对象地址
    ULONG GrantedAccess;        // 访问权限
} SYSTEM_HANDLE_TABLE_ENTRY_INFO;

// 自定义兼容的SYSTEM_HANDLE结构体
typedef struct _SYSTEM_HANDLE {
    ULONG ProcessId;            // 进程ID（ULONG）
    UCHAR ObjectTypeNumber;     // 对象类型编号
    BYTE Flags;                // 标志位
    USHORT Handle;             // 句柄值（USHORT）
    PVOID Object;              // 对象地址
    ACCESS_MASK GrantedAccess; // 访问权限
} SYSTEM_HANDLE, * PSYSTEM_HANDLE;

// 句柄信息表结构体
typedef struct _SYSTEM_HANDLE_INFORMATION {
    ULONG HandleCount;          // 正确字段名
    SYSTEM_HANDLE_TABLE_ENTRY_INFO Handles[1];   // 动态数组
} SYSTEM_HANDLE_INFORMATION, * PSYSTEM_HANDLE_INFORMATION;
typedef DWORD(WINAPI* PNtQuerySystemInformation) (UINT systemInformation, PVOID SystemInformation, ULONG SystemInformationLength,
    PULONG ReturnLength);

BOOL NtQueryAllProcess();
int GetFileOccupyInformation(std::string startString);
#endif