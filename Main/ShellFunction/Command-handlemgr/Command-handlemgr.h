#ifdef KSWORD_WITH_COMMAND
#pragma once
#include "../../KswordTotalHead.h"
//#define UNICODE
//#define _UNICODE
typedef LONG    KPRIORITY;
#define SystemProcessInformation    5 // ���ܺ�
typedef NTSTATUS(NTAPI* _NtQuerySystemInformation)(
    ULONG SystemInformationClass,
    PVOID SystemInformation,
    ULONG SystemInformationLength,
    PULONG ReturnLength
    );
// SYSTEM_HANDLE_TABLE_ENTRY_INFO���壨����NtQuerySystemInformation���ص����ݽṹ��
typedef struct _SYSTEM_HANDLE_TABLE_ENTRY_INFO {
    USHORT UniqueProcessId;     // ����ID��USHORT��
    USHORT CreatorBackTraceIndex;
    UCHAR ObjectTypeIndex;      // �������ͱ��
    UCHAR HandleAttributes;     // �������
    USHORT HandleValue;         // ���ֵ��USHORT��
    PVOID Object;               // �����ַ
    ULONG GrantedAccess;        // ����Ȩ��
} SYSTEM_HANDLE_TABLE_ENTRY_INFO;

// �Զ�����ݵ�SYSTEM_HANDLE�ṹ��
typedef struct _SYSTEM_HANDLE {
    ULONG ProcessId;            // ����ID��ULONG��
    UCHAR ObjectTypeNumber;     // �������ͱ��
    BYTE Flags;                // ��־λ
    USHORT Handle;             // ���ֵ��USHORT��
    PVOID Object;              // �����ַ
    ACCESS_MASK GrantedAccess; // ����Ȩ��
} SYSTEM_HANDLE, * PSYSTEM_HANDLE;

// �����Ϣ��ṹ��
typedef struct _SYSTEM_HANDLE_INFORMATION {
    ULONG HandleCount;          // ��ȷ�ֶ���
    SYSTEM_HANDLE_TABLE_ENTRY_INFO Handles[1];   // ��̬����
} SYSTEM_HANDLE_INFORMATION, * PSYSTEM_HANDLE_INFORMATION;
typedef DWORD(WINAPI* PNtQuerySystemInformation) (UINT systemInformation, PVOID SystemInformation, ULONG SystemInformationLength,
    PULONG ReturnLength);

BOOL NtQueryAllProcess();
int GetFileOccupyInformation(std::string startString);
#endif