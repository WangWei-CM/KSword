#pragma once

#include "KswordArkProcessIoctl.h"

// ============================================================
// KswordArkRegistryIoctl.h
// 作用：
// - 定义 R3/R0 注册表只读查询协议；
// - 协议只承载路径、值名、类型和原始数据，不承载 UI 格式化细节。
// ============================================================

#define KSWORD_ARK_REGISTRY_PROTOCOL_VERSION 1UL

#define KSWORD_ARK_IOCTL_FUNCTION_READ_REGISTRY_VALUE 0x823UL

#define IOCTL_KSWORD_ARK_READ_REGISTRY_VALUE \
    CTL_CODE( \
        KSWORD_ARK_IOCTL_DEVICE_TYPE, \
        KSWORD_ARK_IOCTL_FUNCTION_READ_REGISTRY_VALUE, \
        METHOD_BUFFERED, \
        FILE_ANY_ACCESS)

#define KSWORD_ARK_REGISTRY_PATH_CHARS 512U
#define KSWORD_ARK_REGISTRY_VALUE_NAME_CHARS 256U
#define KSWORD_ARK_REGISTRY_DATA_MAX_BYTES 4096U

#define KSWORD_ARK_REGISTRY_READ_FLAG_VALUE_NAME_PRESENT 0x00000001UL

#define KSWORD_ARK_REGISTRY_READ_STATUS_UNKNOWN 0UL
#define KSWORD_ARK_REGISTRY_READ_STATUS_SUCCESS 1UL
#define KSWORD_ARK_REGISTRY_READ_STATUS_NOT_FOUND 2UL
#define KSWORD_ARK_REGISTRY_READ_STATUS_BUFFER_TOO_SMALL 3UL
#define KSWORD_ARK_REGISTRY_READ_STATUS_FAILED 4UL

typedef struct _KSWORD_ARK_READ_REGISTRY_VALUE_REQUEST
{
    unsigned long version;
    unsigned long flags;
    unsigned long maxDataBytes;
    unsigned long reserved;
    wchar_t keyPath[KSWORD_ARK_REGISTRY_PATH_CHARS];
    wchar_t valueName[KSWORD_ARK_REGISTRY_VALUE_NAME_CHARS];
} KSWORD_ARK_READ_REGISTRY_VALUE_REQUEST;

typedef struct _KSWORD_ARK_READ_REGISTRY_VALUE_RESPONSE
{
    unsigned long version;
    unsigned long status;
    unsigned long valueType;
    unsigned long dataBytes;
    unsigned long requiredBytes;
    long lastStatus;
    unsigned long reserved;
    unsigned char data[KSWORD_ARK_REGISTRY_DATA_MAX_BYTES];
} KSWORD_ARK_READ_REGISTRY_VALUE_RESPONSE;
