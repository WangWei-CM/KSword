#pragma once

#include "KswordArkProcessIoctl.h"

// ============================================================
// KswordArkNetworkIoctl.h
// 作用：
// - 定义 R3/R0 网络过滤与端口隐藏控制协议；
// - R0 通过 WFP callout 执行端口级阻断/放行策略；
// - 端口隐藏采用规则快照与查询接口表达，R3 可据此过滤展示。
// ============================================================

#define KSWORD_ARK_NETWORK_PROTOCOL_VERSION 1UL

#define KSWORD_ARK_IOCTL_FUNCTION_NETWORK_SET_RULES   0x829UL
#define KSWORD_ARK_IOCTL_FUNCTION_NETWORK_QUERY_STATUS 0x82AUL

#define IOCTL_KSWORD_ARK_NETWORK_SET_RULES \
    CTL_CODE( \
        KSWORD_ARK_IOCTL_DEVICE_TYPE, \
        KSWORD_ARK_IOCTL_FUNCTION_NETWORK_SET_RULES, \
        METHOD_BUFFERED, \
        FILE_WRITE_ACCESS)

#define IOCTL_KSWORD_ARK_NETWORK_QUERY_STATUS \
    CTL_CODE( \
        KSWORD_ARK_IOCTL_DEVICE_TYPE, \
        KSWORD_ARK_IOCTL_FUNCTION_NETWORK_QUERY_STATUS, \
        METHOD_BUFFERED, \
        FILE_ANY_ACCESS)

#define KSWORD_ARK_NETWORK_ACTION_DISABLE 0UL
#define KSWORD_ARK_NETWORK_ACTION_REPLACE 1UL
#define KSWORD_ARK_NETWORK_ACTION_CLEAR   2UL

#define KSWORD_ARK_NETWORK_RULE_ACTION_ALLOW     1UL
#define KSWORD_ARK_NETWORK_RULE_ACTION_BLOCK     2UL
#define KSWORD_ARK_NETWORK_RULE_ACTION_HIDE_PORT 3UL

#define KSWORD_ARK_NETWORK_DIRECTION_INBOUND  0x00000001UL
#define KSWORD_ARK_NETWORK_DIRECTION_OUTBOUND 0x00000002UL
#define KSWORD_ARK_NETWORK_DIRECTION_BOTH \
    (KSWORD_ARK_NETWORK_DIRECTION_INBOUND | KSWORD_ARK_NETWORK_DIRECTION_OUTBOUND)

#define KSWORD_ARK_NETWORK_PROTOCOL_ANY 0UL
#define KSWORD_ARK_NETWORK_PROTOCOL_TCP 6UL
#define KSWORD_ARK_NETWORK_PROTOCOL_UDP 17UL

#define KSWORD_ARK_NETWORK_RULE_FLAG_ENABLED 0x00000001UL

#define KSWORD_ARK_NETWORK_RUNTIME_WFP_REGISTERED 0x00000001UL
#define KSWORD_ARK_NETWORK_RUNTIME_WFP_STARTED    0x00000002UL
#define KSWORD_ARK_NETWORK_RUNTIME_RULES_ACTIVE   0x00000004UL
#define KSWORD_ARK_NETWORK_RUNTIME_PORT_HIDE      0x00000008UL

#define KSWORD_ARK_NETWORK_STATUS_UNKNOWN          0UL
#define KSWORD_ARK_NETWORK_STATUS_APPLIED          1UL
#define KSWORD_ARK_NETWORK_STATUS_CLEARED          2UL
#define KSWORD_ARK_NETWORK_STATUS_DISABLED         3UL
#define KSWORD_ARK_NETWORK_STATUS_INVALID_RULE     4UL
#define KSWORD_ARK_NETWORK_STATUS_WFP_UNAVAILABLE  5UL
#define KSWORD_ARK_NETWORK_STATUS_OPERATION_FAILED 6UL

#define KSWORD_ARK_NETWORK_MAX_RULES 32UL

// 单条网络规则。port 为 0 表示匹配任意端口，processId 为 0 表示匹配任意进程。
typedef struct _KSWORD_ARK_NETWORK_RULE
{
    unsigned long ruleId;
    unsigned long action;
    unsigned long directionMask;
    unsigned long protocol;
    unsigned long processId;
    unsigned long flags;
    unsigned short localPort;
    unsigned short remotePort;
    unsigned long reserved0;
    unsigned long reserved1;
} KSWORD_ARK_NETWORK_RULE;

// 设置网络规则请求。REPLACE 覆盖整个规则快照，CLEAR/DISABLE 清空规则。
typedef struct _KSWORD_ARK_NETWORK_SET_RULES_REQUEST
{
    unsigned long version;
    unsigned long action;
    unsigned long ruleCount;
    unsigned long flags;
    KSWORD_ARK_NETWORK_RULE rules[KSWORD_ARK_NETWORK_MAX_RULES];
} KSWORD_ARK_NETWORK_SET_RULES_REQUEST;

// 设置网络规则响应。blockedCount/hiddenPortCount 便于 R3 快速展示能力状态。
typedef struct _KSWORD_ARK_NETWORK_SET_RULES_RESPONSE
{
    unsigned long version;
    unsigned long status;
    unsigned long runtimeFlags;
    unsigned long appliedCount;
    unsigned long blockedRuleCount;
    unsigned long hiddenPortRuleCount;
    unsigned long rejectedIndex;
    unsigned long generation;
    long lastStatus;
    unsigned long reserved;
} KSWORD_ARK_NETWORK_SET_RULES_RESPONSE;

// 查询网络运行时响应。rules 是当前 R0 快照，R3 可据此做端口隐藏展示过滤。
typedef struct _KSWORD_ARK_NETWORK_STATUS_RESPONSE
{
    unsigned long version;
    unsigned long status;
    unsigned long runtimeFlags;
    unsigned long ruleCount;
    unsigned long blockedRuleCount;
    unsigned long hiddenPortRuleCount;
    unsigned long generation;
    unsigned long reserved;
    unsigned long long classifyCount;
    unsigned long long blockedCount;
    long registerStatus;
    long engineStatus;
    KSWORD_ARK_NETWORK_RULE rules[KSWORD_ARK_NETWORK_MAX_RULES];
} KSWORD_ARK_NETWORK_STATUS_RESPONSE;
