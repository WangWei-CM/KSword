#pragma once
#include <Windows.h>
#include <winternl.h>

#include <map>
#include <string>
#include <vector>
#include <winternl.h>

void KswordNTMain();

// 复用原代码中的结构体和枚举定义
typedef struct _OBJECT_TYPE_INFORMATION
{
    UNICODE_STRING TypeName;
    ULONG TotalNumberOfObjects;
    ULONG TotalNumberOfHandles;
    ULONG TotalPagedPoolUsage;
    ULONG TotalNonPagedPoolUsage;
    ULONG TotalNamePoolUsage;
    ULONG TotalHandleTableUsage;
    ULONG HighWaterNumberOfObjects;
    ULONG HighWaterNumberOfHandles;
    ULONG HighWaterPagedPoolUsage;
    ULONG HighWaterNonPagedPoolUsage;
    ULONG HighWaterNamePoolUsage;
    ULONG HighWaterHandleTableUsage;
    ULONG InvalidAttributes;
    GENERIC_MAPPING GenericMapping;
    ULONG ValidAccessMask;
    BOOLEAN SecurityRequired;
    BOOLEAN MaintainHandleCount;
    UCHAR TypeIndex;
    CHAR ReservedByte;
    ULONG PoolType;
    ULONG DefaultPagedPoolCharge;
    ULONG DefaultNonPagedPoolCharge;
} OBJECT_TYPE_INFORMATION, * POBJECT_TYPE_INFORMATION;

typedef struct _OBJECT_TYPES_INFORMATION {
    ULONG NumberOfTypes;
} OBJECT_TYPES_INFORMATION, * POBJECT_TYPES_INFORMATION;

typedef enum _MY_OBJECT_INFORMATION_CLASS {
    OIC_ObjectBasicInformation = 0,
    OIC_ObjectNameInformation = 1,
    OIC_ObjectTypeInformation = 2,
    OIC_ObjectTypesInformation = 3,
    OIC_ObjectHandleFlagInformation = 4,
    OIC_ObjectSessionInformation = 5,
} MY_OBJECT_INFORMATION_CLASS;

// 声明外部函数和全局变量
extern "C" NTSTATUS NTAPI NtQueryObject(
    HANDLE ObjectHandle,
    OBJECT_INFORMATION_CLASS ObjectInformationClass,
    PVOID ObjectInformation,
    ULONG ObjectInformationLength,
    PULONG ReturnLength
);

extern std::map<UCHAR, std::wstring> g_typeMap;
extern bool g_isQueryCompleted;
extern bool g_isQuerySuccess;

// 函数声明
std::map<UCHAR, std::wstring> QueryObjectTypeIndexMap();
void RenderTypeIndexWindow();