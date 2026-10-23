/* 设备文件身份租约只持有进程/线程对象引用，不授予目标访问权限。 */
#pragma once

/* 使用公共驱动类型及唯一共享协议定义。 */
#include "debugger.h"

/* 在现有同步 WDF 请求存续期间，将目标引用绑定到该请求的独立文件上下文。 */
NTSTATUS KswordARKDebuggerRetainIdentity(WDFREQUEST Request,
    const KSWORD_ARK_DEBUGGER_REQUEST* Input, KSWORD_ARK_DEBUGGER_RESPONSE* Output);
