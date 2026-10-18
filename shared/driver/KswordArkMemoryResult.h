#pragma once

#include "KswordArkMemoryIoctl.h"

// 只解释既有响应，不发送请求、不修改内核状态，供 CLI 与界面共用。
namespace ksword::memory_result
{
    // completed：本协议的同步阶段只以 STATUS_SUCCESS 表示已经完成。
    // STATUS_PENDING 是未执行的初值，不能按 NT_SUCCESS 当成完成。
    inline bool completed(const long status) noexcept
    {
        return status == 0;
    }

    // mappedSucceeded：检查必需阶段和用户实际请求的写测试；不把跳过项当失败。
    inline bool mappedSucceeded(
        const KSWORD_ARK_CHANGE_MAPPED_PAGE_PROTECTION_RESPONSE& response,
        const unsigned long flags) noexcept
    {
        return response.version == 1UL && response.size == sizeof(response)
            && completed(response.mdlAllocStatus) && completed(response.lockStatus)
            && completed(response.mapStatus) && completed(response.protectStatus)
            && ((flags & KSWORD_ARK_CHANGE_PROTECTION_FLAG_WRITE_TEST) == 0UL
                || completed(response.writeTestStatus));
    }

    // imageSucceeded：别名映射属于写测试分支，恢复是独立的用户请求阶段。
    inline bool imageSucceeded(
        const KSWORD_ARK_CHANGE_IMAGE_PAGE_PROTECTION_RESPONSE& response,
        const unsigned long flags) noexcept
    {
        return response.version == 1UL && response.size == sizeof(response)
            && completed(response.resolverStatus) && completed(response.mdlAllocStatus)
            && completed(response.lockStatus) && completed(response.imageProtectStatus)
            && ((flags & KSWORD_ARK_IMAGE_PROT_FLAG_WRITE_TEST) == 0UL
                || (completed(response.aliasMapStatus) && completed(response.writeTestStatus)))
            && ((flags & KSWORD_ARK_IMAGE_PROT_FLAG_RESTORE) == 0UL
                || completed(response.restoreStatus));
    }

    // ByteEvidence：每个槽位是否真的经过读取；值为零本身不能证明读取发生。
    struct ByteEvidence
    {
        bool before = false;       // 原始 VA 的基线读取是否有效。
        bool afterWrite = false;   // 写测试分支后从原始 VA 读取是否有效。
        bool afterRestore = false; // 恢复分支后从原始 VA 读取是否有效。
    };

    // attempted：排除协议中明确表示未执行的两种初值，失败阶段也可有独立读回。
    inline bool attempted(const long status) noexcept
    {
        return status != 0x103L && status != static_cast<long>(0xC00000BBUL);
    }

    // imageByteEvidence：依据实际分支与完整槽位判断；0xDEAD 读取失败哨兵不能截为AD。
    inline ByteEvidence imageByteEvidence(
        const KSWORD_ARK_CHANGE_IMAGE_PAGE_PROTECTION_RESPONSE& response,
        const unsigned long flags) noexcept
    {
        ByteEvidence result; // 未证明的读取全部保持不可用。
        const bool locked = response.version == 1UL && response.size == sizeof(response)
            && completed(response.resolverStatus) && completed(response.mdlAllocStatus)
            && completed(response.lockStatus); // 三项前置必须实际完成。
        result.before = locked && response.originalPte <= 0xFFULL;
        result.afterWrite = locked && (flags & KSWORD_ARK_IMAGE_PROT_FLAG_WRITE_TEST) != 0UL
            && completed(response.aliasMapStatus) && response.aliasVa != 0ULL
            && attempted(response.writeTestStatus) && response.afterImageProtectPte <= 0xFFULL;
        result.afterRestore = locked && (flags & KSWORD_ARK_IMAGE_PROT_FLAG_RESTORE) != 0UL
            && attempted(response.restoreStatus) && response.afterRestorePte <= 0xFFULL;
        return result;
    }
}
