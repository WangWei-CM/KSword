/*++

Module Name:

    redirect_registry.c

Abstract:

    Registry create/open redirection callback for KswordARK.

Environment:

    Kernel-mode Configuration Manager callback

--*/

#include "redirect_internal.h"

static const WCHAR g_KswordArkRedirectRegistryAltitude[] = L"385201.6141";

typedef struct _KSWORD_ARK_REG_CREATE_OPEN_VIEW
{
    PUNICODE_STRING CompleteName;
    ACCESS_MASK DesiredAccess;
} KSWORD_ARK_REG_CREATE_OPEN_VIEW;

static BOOLEAN
KswordARKRedirectRegistryGetCreateOpenView(
    _In_ REG_NOTIFY_CLASS NotifyClass,
    _In_ PVOID OperationInfo,
    _Out_ KSWORD_ARK_REG_CREATE_OPEN_VIEW* ViewOut
    )
/*++

Routine Description:

    把 create/open 注册表回调参数规整成统一视图。中文说明：当前只处理公开的
    RegNtPreCreateKeyEx 与 RegNtPreOpenKeyEx，其它操作不参与路径替换。

Arguments:

    NotifyClass - Configuration Manager 通知类型。
    OperationInfo - 对应通知结构。
    ViewOut - 返回统一视图。

Return Value:

    TRUE 表示可尝试重定向；FALSE 表示忽略该通知。

--*/
{
    if (OperationInfo == NULL || ViewOut == NULL) {
        return FALSE;
    }
    RtlZeroMemory(ViewOut, sizeof(*ViewOut));

    if (NotifyClass == RegNtPreCreateKeyEx) {
        PREG_CREATE_KEY_INFORMATION info = (PREG_CREATE_KEY_INFORMATION)OperationInfo;
        ViewOut->CompleteName = info->CompleteName;
        ViewOut->DesiredAccess = info->DesiredAccess;
        return TRUE;
    }
    if (NotifyClass == RegNtPreOpenKeyEx) {
        PREG_OPEN_KEY_INFORMATION info = (PREG_OPEN_KEY_INFORMATION)OperationInfo;
        ViewOut->CompleteName = info->CompleteName;
        ViewOut->DesiredAccess = info->DesiredAccess;
        return TRUE;
    }

    return FALSE;
}

static NTSTATUS
KswordARKRedirectRegistryOpenTarget(
    _In_ const KSWORD_ARK_REDIRECT_RULE* Rule,
    _In_ ACCESS_MASK DesiredAccess,
    _Out_ HANDLE* KeyHandleOut
    )
/*++

Routine Description:

    打开重定向目标键。中文说明：Cm callback 要求回调返回成功并设置 ResultObject
    才能让本次打开落到替代键，因此这里使用 OBJ_KERNEL_HANDLE 受控打开目标路径。

Arguments:

    Rule - 命中的注册表重定向规则。
    DesiredAccess - 原请求希望获取的访问掩码。
    KeyHandleOut - 返回目标键 handle，调用方负责 ZwClose。

Return Value:

    ZwOpenKey 返回状态或参数错误。

--*/
{
    OBJECT_ATTRIBUTES objectAttributes;
    UNICODE_STRING targetPath;

    if (Rule == NULL || KeyHandleOut == NULL) {
        return STATUS_INVALID_PARAMETER;
    }
    *KeyHandleOut = NULL;

    RtlInitUnicodeString(&targetPath, Rule->targetPath);
    if (targetPath.Buffer == NULL || targetPath.Length == 0U) {
        return STATUS_INVALID_PARAMETER;
    }

    InitializeObjectAttributes(
        &objectAttributes,
        &targetPath,
        OBJ_KERNEL_HANDLE | OBJ_CASE_INSENSITIVE,
        NULL,
        NULL);
    if (DesiredAccess == 0UL) {
        DesiredAccess = KEY_READ;
    }
    return ZwOpenKey(
        KeyHandleOut,
        DesiredAccess,
        &objectAttributes);
}

NTSTATUS
KswordARKRedirectRegistryCallback(
    _In_opt_ PVOID CallbackContext,
    _In_opt_ PVOID Argument1,
    _In_opt_ PVOID Argument2
    )
/*++

Routine Description:

    处理注册表 create/open 重定向。中文说明：只在规则命中时打开目标键并填入
    ResultObject，然后返回 STATUS_CALLBACK_BYPASS 让 Configuration Manager 使用
    替代对象；未命中或失败时不改变原请求。

Arguments:

    CallbackContext - 重定向运行时。
    Argument1 - REG_NOTIFY_CLASS。
    Argument2 - 具体操作结构。

Return Value:

    STATUS_CALLBACK_BYPASS 表示已替换目标；其它情况返回 STATUS_SUCCESS。

--*/
{
    KSWORD_ARK_REDIRECT_RUNTIME* runtime = (KSWORD_ARK_REDIRECT_RUNTIME*)CallbackContext;
    REG_NOTIFY_CLASS notifyClass = (REG_NOTIFY_CLASS)(ULONG_PTR)Argument1;
    KSWORD_ARK_REG_CREATE_OPEN_VIEW view;
    KSWORD_ARK_REDIRECT_RULE matchedRule;
    HANDLE targetHandle = NULL;
    PVOID targetObject = NULL;
    ULONG processId = HandleToULong(PsGetCurrentProcessId());
    NTSTATUS status = STATUS_SUCCESS;

    if (runtime == NULL || Argument2 == NULL) {
        return STATUS_SUCCESS;
    }
    if ((runtime->RuntimeFlags & KSWORD_ARK_REDIRECT_RUNTIME_REGISTRY_ACTIVE) == 0UL) {
        return STATUS_SUCCESS;
    }
    if (!KswordARKRedirectRegistryGetCreateOpenView(notifyClass, Argument2, &view)) {
        return STATUS_SUCCESS;
    }
    if (view.CompleteName == NULL || view.CompleteName->Buffer == NULL || view.CompleteName->Length == 0U) {
        return STATUS_SUCCESS;
    }

    ExAcquirePushLockShared(&runtime->Lock);
    status = KswordARKRedirectFindMatchLocked(
        runtime,
        KSWORD_ARK_REDIRECT_TYPE_REGISTRY,
        processId,
        view.CompleteName,
        &matchedRule);
    ExReleasePushLockShared(&runtime->Lock);
    if (!NT_SUCCESS(status)) {
        return STATUS_SUCCESS;
    }

    status = KswordARKRedirectRegistryOpenTarget(
        &matchedRule,
        view.DesiredAccess,
        &targetHandle);
    if (!NT_SUCCESS(status)) {
        KswordARKRedirectLogFormat(
            "Warn",
            "Registry redirect open-target failed, pid=%lu, ruleId=%lu, status=0x%08X.",
            (unsigned long)processId,
            (unsigned long)matchedRule.ruleId,
            (unsigned int)status);
        return STATUS_SUCCESS;
    }

    status = ObReferenceObjectByHandle(
        targetHandle,
        0,
        NULL,
        KernelMode,
        &targetObject,
        NULL);
    ZwClose(targetHandle);
    if (!NT_SUCCESS(status) || targetObject == NULL) {
        KswordARKRedirectLogFormat(
            "Warn",
            "Registry redirect reference-target failed, pid=%lu, ruleId=%lu, status=0x%08X.",
            (unsigned long)processId,
            (unsigned long)matchedRule.ruleId,
            (unsigned int)status);
        return STATUS_SUCCESS;
    }

    if (notifyClass == RegNtPreCreateKeyEx) {
        PREG_CREATE_KEY_INFORMATION info = (PREG_CREATE_KEY_INFORMATION)Argument2;
        if (info->ResultObject != NULL) {
            *info->ResultObject = targetObject;
        }
        info->GrantedAccess = view.DesiredAccess;
        if (info->Disposition != NULL) {
            *info->Disposition = REG_OPENED_EXISTING_KEY;
        }
    }
    else {
        PREG_OPEN_KEY_INFORMATION info = (PREG_OPEN_KEY_INFORMATION)Argument2;
        if (info->ResultObject != NULL) {
            *info->ResultObject = targetObject;
        }
        info->GrantedAccess = view.DesiredAccess;
    }

    InterlockedIncrement64(&runtime->RegistryRedirectHits);
    KswordARKRedirectLogFormat(
        "Info",
        "Registry redirect applied, pid=%lu, ruleId=%lu.",
        (unsigned long)processId,
        (unsigned long)matchedRule.ruleId);
    return STATUS_CALLBACK_BYPASS;
}

NTSTATUS
KswordARKRedirectRegistryRegister(
    _In_ KSWORD_ARK_REDIRECT_RUNTIME* Runtime,
    _In_ PDRIVER_OBJECT DriverObject
    )
/*++

Routine Description:

    注册独立注册表重定向 callback。中文说明：该 callback 与自定义回调规则模块
    使用不同 altitude，避免两个会话修改同一 callback 文件产生冲突。

Arguments:

    Runtime - 重定向运行时。
    DriverObject - 驱动对象。

Return Value:

    CmRegisterCallbackEx 返回状态。

--*/
{
    UNICODE_STRING altitudeText;
    NTSTATUS status = STATUS_SUCCESS;

    if (Runtime == NULL || DriverObject == NULL) {
        return STATUS_INVALID_PARAMETER;
    }

    RtlInitUnicodeString(&altitudeText, g_KswordArkRedirectRegistryAltitude);
    Runtime->RegistryCookie.QuadPart = 0LL;
    status = CmRegisterCallbackEx(
        KswordARKRedirectRegistryCallback,
        &altitudeText,
        DriverObject,
        Runtime,
        &Runtime->RegistryCookie,
        NULL);
    return status;
}

VOID
KswordARKRedirectRegistryUnregister(
    _In_ KSWORD_ARK_REDIRECT_RUNTIME* Runtime
    )
/*++

Routine Description:

    注销注册表重定向 callback。中文说明：Cookie 非零时才调用 CmUnRegisterCallback，
    并清除 REGISTRY_HOOKED 标志，保证卸载幂等。

Arguments:

    Runtime - 重定向运行时。

Return Value:

    None. 本函数没有返回值。

--*/
{
    if (Runtime == NULL) {
        return;
    }

    if (Runtime->RegistryCookie.QuadPart != 0LL) {
        (VOID)CmUnRegisterCallback(Runtime->RegistryCookie);
        Runtime->RegistryCookie.QuadPart = 0LL;
    }
    Runtime->RuntimeFlags &= ~KSWORD_ARK_REDIRECT_RUNTIME_REGISTRY_HOOKED;
}
