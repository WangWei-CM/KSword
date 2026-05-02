/*++

Module Name:

    redirect_file.c

Abstract:

    File-system create redirection helper for the shared minifilter runtime.

Environment:

    Kernel-mode minifilter

--*/

#include "redirect_internal.h"

NTSTATUS
KswordARKRedirectTryRewriteFileCreate(
    _Inout_ PFLT_CALLBACK_DATA Data,
    _In_ PCFLT_RELATED_OBJECTS FltObjects,
    _Out_ BOOLEAN* RedirectedOut
    )
/*++

Routine Description:

    在 IRP_MJ_CREATE pre-operation 中尝试替换 FileObject->FileName。中文说明：
    该实现只使用 FltMgr/IO manager 公开字段，命中规则后用目标 NT 路径替换本次
    create 名称，并调用 FltSetCallbackDataDirty 通知 FltMgr 重新处理参数。

Arguments:

    Data - FltMgr callback data。
    FltObjects - FltMgr related objects。
    RedirectedOut - 返回 TRUE 表示已经改写本次 create 路径。

Return Value:

    STATUS_SUCCESS 表示已检查完成；失败状态表示规则命中但改写失败。

--*/
{
    KSWORD_ARK_REDIRECT_RUNTIME* runtime = KswordARKRedirectGetRuntime();
    KSWORD_ARK_REDIRECT_RULE matchedRule;
    UNICODE_STRING sourceName;
    UNICODE_STRING targetName;
    ULONG processId = 0UL;
    NTSTATUS status = STATUS_SUCCESS;

    if (RedirectedOut == NULL) {
        return STATUS_INVALID_PARAMETER;
    }
    *RedirectedOut = FALSE;

    if (Data == NULL || FltObjects == NULL || FltObjects->FileObject == NULL) {
        return STATUS_INVALID_PARAMETER;
    }
    if (Data->Iopb == NULL || Data->Iopb->MajorFunction != IRP_MJ_CREATE) {
        return STATUS_SUCCESS;
    }
    if ((runtime->RuntimeFlags & KSWORD_ARK_REDIRECT_RUNTIME_FILE_ACTIVE) == 0UL) {
        return STATUS_SUCCESS;
    }
    if (FltObjects->FileObject->FileName.Buffer == NULL ||
        FltObjects->FileObject->FileName.Length == 0U) {
        return STATUS_SUCCESS;
    }

    sourceName = FltObjects->FileObject->FileName;
    processId = (ULONG)(ULONG_PTR)FltGetRequestorProcessId(Data);

    ExAcquirePushLockShared(&runtime->Lock);
    status = KswordARKRedirectFindMatchLocked(
        runtime,
        KSWORD_ARK_REDIRECT_TYPE_FILE,
        processId,
        &sourceName,
        &matchedRule);
    ExReleasePushLockShared(&runtime->Lock);
    if (!NT_SUCCESS(status)) {
        return STATUS_SUCCESS;
    }

    RtlInitUnicodeString(&targetName, matchedRule.targetPath);
    if (targetName.Buffer == NULL || targetName.Length == 0U) {
        return STATUS_INVALID_PARAMETER;
    }

    status = IoReplaceFileObjectName(
        FltObjects->FileObject,
        targetName.Buffer,
        targetName.Length);
    if (!NT_SUCCESS(status)) {
        KswordARKRedirectLogFormat(
            "Warn",
            "File redirect failed, pid=%lu, ruleId=%lu, status=0x%08X.",
            (unsigned long)processId,
            (unsigned long)matchedRule.ruleId,
            (unsigned int)status);
        return status;
    }

    FltSetCallbackDataDirty(Data);
    InterlockedIncrement64(&runtime->FileRedirectHits);
    *RedirectedOut = TRUE;

    KswordARKRedirectLogFormat(
        "Info",
        "File redirect applied, pid=%lu, ruleId=%lu.",
        (unsigned long)processId,
        (unsigned long)matchedRule.ruleId);
    return STATUS_SUCCESS;
}
