#pragma once

#include "KswordArkProcessIoctl.h"

// ============================================================
// KswordArkRedirectIoctl.h
// 作用：
// - 定义 R3/R0 文件与注册表重定向控制协议；
// - R0 只维护受控规则表并在公开回调路径中做名字替换；
// - 默认不启用任何规则，必须由 R3 显式设置规则后才生效。
// ============================================================

#define KSWORD_ARK_REDIRECT_PROTOCOL_VERSION 1UL

#define KSWORD_ARK_IOCTL_FUNCTION_REDIRECT_SET_RULES 0x827UL
#define KSWORD_ARK_IOCTL_FUNCTION_REDIRECT_QUERY_STATUS 0x828UL

#define IOCTL_KSWORD_ARK_REDIRECT_SET_RULES \
    CTL_CODE( \
        KSWORD_ARK_IOCTL_DEVICE_TYPE, \
        KSWORD_ARK_IOCTL_FUNCTION_REDIRECT_SET_RULES, \
        METHOD_BUFFERED, \
        FILE_WRITE_ACCESS)

#define IOCTL_KSWORD_ARK_REDIRECT_QUERY_STATUS \
    CTL_CODE( \
        KSWORD_ARK_IOCTL_DEVICE_TYPE, \
        KSWORD_ARK_IOCTL_FUNCTION_REDIRECT_QUERY_STATUS, \
        METHOD_BUFFERED, \
        FILE_ANY_ACCESS)

#define KSWORD_ARK_REDIRECT_ACTION_DISABLE 0UL
#define KSWORD_ARK_REDIRECT_ACTION_REPLACE 1UL
#define KSWORD_ARK_REDIRECT_ACTION_CLEAR   2UL

#define KSWORD_ARK_REDIRECT_TYPE_FILE     1UL
#define KSWORD_ARK_REDIRECT_TYPE_REGISTRY 2UL

#define KSWORD_ARK_REDIRECT_MATCH_EXACT  1UL
#define KSWORD_ARK_REDIRECT_MATCH_PREFIX 2UL

#define KSWORD_ARK_REDIRECT_RULE_FLAG_ENABLED 0x00000001UL

#define KSWORD_ARK_REDIRECT_RUNTIME_FILE_ACTIVE     0x00000001UL
#define KSWORD_ARK_REDIRECT_RUNTIME_REGISTRY_ACTIVE 0x00000002UL
#define KSWORD_ARK_REDIRECT_RUNTIME_REGISTRY_HOOKED 0x00000004UL

#define KSWORD_ARK_REDIRECT_STATUS_UNKNOWN            0UL
#define KSWORD_ARK_REDIRECT_STATUS_APPLIED            1UL
#define KSWORD_ARK_REDIRECT_STATUS_CLEARED            2UL
#define KSWORD_ARK_REDIRECT_STATUS_DISABLED           3UL
#define KSWORD_ARK_REDIRECT_STATUS_INVALID_RULE       4UL
#define KSWORD_ARK_REDIRECT_STATUS_UNSUPPORTED_TYPE   5UL
#define KSWORD_ARK_REDIRECT_STATUS_OPERATION_FAILED   6UL

#define KSWORD_ARK_REDIRECT_MAX_RULES 16UL
#define KSWORD_ARK_REDIRECT_PATH_CHARS 520U
#define KSWORD_ARK_REDIRECT_ALTITUDE_CHARS 32U

// 单条重定向规则。sourcePath 与 targetPath 都使用 NT namespace 路径。
typedef struct _KSWORD_ARK_REDIRECT_RULE
{
    unsigned long ruleId;
    unsigned long type;
    unsigned long action;
    unsigned long matchMode;
    unsigned long flags;
    unsigned long processId;
    unsigned long reserved0;
    unsigned long reserved1;
    wchar_t sourcePath[KSWORD_ARK_REDIRECT_PATH_CHARS];
    wchar_t targetPath[KSWORD_ARK_REDIRECT_PATH_CHARS];
} KSWORD_ARK_REDIRECT_RULE;

// 设置规则请求。ruleCount 为 0 或 action=CLEAR 时清空指定类型规则。
typedef struct _KSWORD_ARK_REDIRECT_SET_RULES_REQUEST
{
    unsigned long version;
    unsigned long action;
    unsigned long typeMask;
    unsigned long ruleCount;
    unsigned long flags;
    unsigned long reserved0;
    unsigned long reserved1;
    unsigned long reserved2;
    KSWORD_ARK_REDIRECT_RULE rules[KSWORD_ARK_REDIRECT_MAX_RULES];
} KSWORD_ARK_REDIRECT_SET_RULES_REQUEST;

// 设置规则响应。lastStatus 记录 R0 真实 NTSTATUS，appliedCount 记录启用规则数。
typedef struct _KSWORD_ARK_REDIRECT_SET_RULES_RESPONSE
{
    unsigned long version;
    unsigned long status;
    unsigned long runtimeFlags;
    unsigned long appliedCount;
    unsigned long rejectedIndex;
    unsigned long fileRuleCount;
    unsigned long registryRuleCount;
    unsigned long generation;
    long lastStatus;
    unsigned long reserved;
} KSWORD_ARK_REDIRECT_SET_RULES_RESPONSE;

// 查询运行时响应。R3 后续 UI 可直接展示规则数量与最新命中计数。
typedef struct _KSWORD_ARK_REDIRECT_STATUS_RESPONSE
{
    unsigned long version;
    unsigned long status;
    unsigned long runtimeFlags;
    unsigned long fileRuleCount;
    unsigned long registryRuleCount;
    unsigned long generation;
    unsigned long long fileRedirectHits;
    unsigned long long registryRedirectHits;
    long registryRegisterStatus;
    unsigned long reserved;
    KSWORD_ARK_REDIRECT_RULE rules[KSWORD_ARK_REDIRECT_MAX_RULES];
} KSWORD_ARK_REDIRECT_STATUS_RESPONSE;
