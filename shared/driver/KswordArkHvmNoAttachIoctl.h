#pragma once

#include "KswordArkHvmIoctl.h"

/* 第一版只控制一个选定的 x64 用户线程，不创建调试对象。 */
#define KSWORD_ARK_HVM_NOATTACH_VERSION 1UL
/* 新操作独立于既有附加调试器协议。 */
#define KSWORD_ARK_IOCTL_FUNCTION_HVM_NOATTACH 0x924UL
/* 所有操作均要求特权；QUERY 绝不进入 VMX。 */
#define IOCTL_KSWORD_ARK_HVM_NOATTACH                                                                        \
    CTL_CODE(KSWORD_ARK_IOCTL_DEVICE_TYPE, KSWORD_ARK_IOCTL_FUNCTION_HVM_NOATTACH, METHOD_BUFFERED,          \
             FILE_WRITE_ACCESS)
/* 读取当前能力或本方拥有的会话。 */
#define KSWORD_ARK_HVM_NOATTACH_QUERY 0UL
/* 显式准备并启动本模块独占的 HVM 会话。 */
#define KSWORD_ARK_HVM_NOATTACH_OPEN 1UL
/* 请求在选定线程的下一次用户指令取指处停止。 */
#define KSWORD_ARK_HVM_NOATTACH_PAUSE 2UL
/* 消费一次停止身份，并请求一个真实的 MTF 执行边界。 */
#define KSWORD_ARK_HVM_NOATTACH_STEP 3UL
/* 消费一次停止身份，并允许正常执行。 */
#define KSWORD_ARK_HVM_NOATTACH_CONTINUE 4UL
/* 撤销执行控制，仅释放本方拥有的 HVM 生命周期。 */
#define KSWORD_ARK_HVM_NOATTACH_CLOSE 5UL
/* OPEN 要求调用方明确确认执行实验操作。 */
#define KSWORD_ARK_HVM_NOATTACH_CONFIRMED 1UL
/* 不能因裸机能力缺失而推断允许嵌套运行。 */
#define KSWORD_ARK_HVM_NOATTACH_ALLOW_NESTED 2UL
/* 当前不存在本方拥有的会话。 */
#define KSWORD_ARK_HVM_NOATTACH_CLOSED 0UL
/* 选定线程可正常运行。 */
#define KSWORD_ARK_HVM_NOATTACH_RUNNING 1UL
/* 已请求暂停，但尚未捕获真实的用户取指。 */
#define KSWORD_ARK_HVM_NOATTACH_PAUSE_PENDING 2UL
/* 选定线程停留在真实用户取指或匹配的 MTF 边界。 */
#define KSWORD_ARK_HVM_NOATTACH_STOPPED 3UL
/* 停止身份已被消费，正在等待该线程的下一次取指。 */
#define KSWORD_ARK_HVM_NOATTACH_STEP_PENDING 4UL
/* 一个处理器拥有正在执行的 MTF 请求。 */
#define KSWORD_ARK_HVM_NOATTACH_STEPPING 5UL
/* 中途发生其它退出，不能认定单条指令已完成。 */
#define KSWORD_ARK_HVM_NOATTACH_INTERRUPTED 6UL
/* 所有者或目标生命周期已结束，控制权已撤销。 */
#define KSWORD_ARK_HVM_NOATTACH_EXITED 7UL
/* 处理器回滚失败，保留资源供显式恢复。 */
#define KSWORD_ARK_HVM_NOATTACH_RECOVERY_REQUIRED 8UL
/* 当前平台无法实现本协议的执行语义。 */
#define KSWORD_ARK_HVM_NOATTACH_UNSUPPORTED 9UL
/* 继续请求已消费旧停止，须在下一次真实取指重新核对现场。 */
#define KSWORD_ARK_HVM_NOATTACH_CONTINUE_PENDING 10UL
/* 停止快照来自选定线程的用户取指。 */
#define KSWORD_ARK_HVM_NOATTACH_STOP_FETCH 1UL
/* 停止快照来自匹配的真实 MTF 完成边界。 */
#define KSWORD_ARK_HVM_NOATTACH_STOP_STEP 2UL
/* 此能力不表示支持整个进程或内核态停止。 */
#define KSWORD_ARK_HVM_NOATTACH_CAP_SINGLE_USER_THREAD 1UL
/* 仅表示 GPR、RIP、RSP 和 RFLAGS；明确不包含 XSTATE。 */
#define KSWORD_ARK_HVM_NOATTACH_CAP_GPR 2UL
/* 必须同时具备原生 Intel MBEC、MTF 和单上下文 INVEPT。 */
#define KSWORD_ARK_HVM_NOATTACH_CAP_EXECUTION 4UL

/* 使用已引用的内核对象重新核对定宽请求身份。 */
typedef struct _KSWORD_ARK_HVM_NOATTACH_REQUEST
{
    unsigned long version;
    unsigned long size;
    unsigned long operation;
    unsigned long flags;
    unsigned long processId;
    unsigned long threadId;
    unsigned long long processCreationTime;
    unsigned long long threadCreationTime;
    unsigned long long ownerCreationTime;
    unsigned long long sessionId;
    unsigned long long stopId;
} KSWORD_ARK_HVM_NOATTACH_REQUEST;

/* 这是停止现场的唯一权威文档；绝不将历史遥测复制到此处。 */
typedef struct _KSWORD_ARK_HVM_NOATTACH_RESPONSE
{
    unsigned long version;
    unsigned long size;
    long status;
    unsigned long state;
    unsigned long capabilities;
    unsigned long stopReason;
    unsigned long processId;
    unsigned long threadId;
    unsigned long long sessionId;
    unsigned long long stopId;
    unsigned long long processCreationTime;
    unsigned long long threadCreationTime;
    unsigned long long rip;
    unsigned long long rsp;
    unsigned long long rflags;
    unsigned long long cr3;
    unsigned long long rax;
    unsigned long long rcx;
    unsigned long long rdx;
    unsigned long long rbx;
    unsigned long long rbp;
    unsigned long long rsi;
    unsigned long long rdi;
    unsigned long long r8;
    unsigned long long r9;
    unsigned long long r10;
    unsigned long long r11;
    unsigned long long r12;
    unsigned long long r13;
    unsigned long long r14;
    unsigned long long r15;
} KSWORD_ARK_HVM_NOATTACH_RESPONSE;
