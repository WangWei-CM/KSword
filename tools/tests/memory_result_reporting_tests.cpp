#define WIN32_LEAN_AND_MEAN
#include <Windows.h>
#include "../../shared/driver/KswordArkMemoryResult.h"
#include <cstdio>

#ifdef REVIEW_HVCI_TEXT_TESTS
#include "../../Ksword5.1/Ksword5.1/MiscDock/HvciBypass/HvciBypassBackend.h"
#endif

namespace
{
    int checks = 0;   // 本次实际执行的断言数。
    int failures = 0; // 失败数，决定测试退出码。
    constexpr long failed = static_cast<long>(0xC0000001UL); // 同步阶段失败。
    constexpr long skipped = static_cast<long>(0xC00000BBUL); // 未执行的可选分支。

    // check：记录真实生产判定的结果，不发送任何设备请求。
    void check(const bool condition, const char* name)
    {
        ++checks;
        if (!condition)
        {
            ++failures;
        }
        std::printf("%s %s\n", condition ? "PASS" : "FAIL", name);
    }

    // image：构造合法、必需阶段完成但未执行可选分支的协议响应。
    KSWORD_ARK_CHANGE_IMAGE_PAGE_PROTECTION_RESPONSE image()
    {
        KSWORD_ARK_CHANGE_IMAGE_PAGE_PROTECTION_RESPONSE response{};
        response.version = 1UL;
        response.size = sizeof(response);
        response.aliasMapStatus = skipped;
        response.writeTestStatus = skipped;
        response.restoreStatus = skipped;
        return response;
    }
}

// 入口：覆盖请求阶段完成、正值PENDING、真正零字节与读取失败哨兵。
int main()
{
    using namespace ksword::memory_result;
    KSWORD_ARK_CHANGE_MAPPED_PAGE_PROTECTION_RESPONSE mapped{}; // 必需阶段成功响应。
    mapped.version = 1UL;
    mapped.size = sizeof(mapped);
    mapped.writeTestStatus = skipped;
    check(mappedSucceeded(mapped, 0UL), "mapped ignores an unrequested write stage");
    check(!mappedSucceeded(mapped, KSWORD_ARK_CHANGE_PROTECTION_FLAG_WRITE_TEST),
        "mapped rejects a requested but skipped write stage");
    mapped.writeTestStatus = failed;
    check(!mappedSucceeded(mapped, 1UL), "mapped propagates requested write failure");
    mapped.writeTestStatus = 0;
    check(mappedSucceeded(mapped, 1UL), "mapped accepts all requested completed stages");
    mapped.protectStatus = 0x103L;
    check(!mappedSucceeded(mapped, 0UL), "positive PENDING is not completion");
    mapped.protectStatus = 0;
    mapped.version = 2UL;
    check(!mappedSucceeded(mapped, 0UL), "mapped rejects an incompatible version");

    auto response = image(); // 没有请求写测试时，别名缺席不应变成失败。
    check(imageSucceeded(response, 0UL), "image accepts protection without optional stages");
    check(!imageSucceeded(response, 1UL), "image requires requested alias and write completion");
    response.aliasMapStatus = 0;
    response.writeTestStatus = 0;
    response.aliasVa = 0xFFFF900000001000ULL;
    check(imageSucceeded(response, 1UL), "image accepts completed requested write");
    check(!imageSucceeded(response, 3UL), "image requires requested restore completion");
    response.restoreStatus = failed;
    check(!imageSucceeded(response, 3UL), "image propagates restore failure after a successful write");
    response.restoreStatus = 0;
    check(imageSucceeded(response, 3UL), "image accepts completed write and restore");
    response.restoreStatus = 0x103L;
    check(!imageSucceeded(response, 3UL), "image rejects a pending restore");
    response = image();
    response.restoreStatus = 0;
    check(imageSucceeded(response, 2UL), "restore-only does not require an alias stage");

    response = image(); // 合法零字节与没有证据的默认零必须可区分。
    auto evidence = imageByteEvidence(response, 3UL);
    check(evidence.before && !evidence.afterWrite && !evidence.afterRestore,
        "proven zero baseline does not invent skipped later reads");
    response.resolverStatus = failed;
    evidence = imageByteEvidence(response, 3UL);
    check(!evidence.before && !evidence.afterWrite && !evidence.afterRestore,
        "failed prerequisites suppress default zero evidence");
    response = image();
    response.lockStatus = 0x103L;
    check(!imageByteEvidence(response, 3UL).before, "pending lock does not prove a baseline read");
    response = image();
    response.originalPte = 0xDEADULL;
    check(!imageByteEvidence(response, 0UL).before, "read failure sentinel is not byte AD");
    response.originalPte = 0;
    response.aliasMapStatus = 0;
    response.aliasVa = 0xFFFF900000001000ULL;
    response.writeTestStatus = failed;
    response.restoreStatus = failed;
    evidence = imageByteEvidence(response, 3UL);
    check(evidence.afterWrite && evidence.afterRestore,
        "independent real readbacks remain observations when write or restore failed");
    response.afterImageProtectPte = 0xDEADULL;
    response.afterRestorePte = 0xDEADULL;
    evidence = imageByteEvidence(response, 3UL);
    check(!evidence.afterWrite && !evidence.afterRestore, "later read failure sentinels are unavailable");
    response.size = 0;
    check(!imageByteEvidence(response, 3UL).before, "malformed metadata does not prove byte evidence");

#ifdef REVIEW_HVCI_TEXT_TESTS
    // 直接执行从当前生产源码提取的纯判定函数，不调用任何实验后端。
    using ks::misc::hvci_bypass::ExperimentResult;
    using ks::misc::hvci_bypass::verdictText;
    ExperimentResult result;
    result.ioSuccess = true;
    result.flags = 3UL;
    result.resolverStatus = failed;
    QString text = verdictText(result); // 未读取时不能出现实际数值读回。
    check(!text.contains(QStringLiteral("[读回] 修改前字节 = 0x00"))
        && !text.contains(QStringLiteral("[读回] 写入后字节 = 0x00"))
        && !text.contains(QStringLiteral("[读回] 恢复后字节 = 0x00")),
        "verdict does not print unobserved zero bytes");
    check(text.contains(QStringLiteral("不可用")), "verdict explains missing readback evidence");
    result.resolverStatus = result.mdlAllocStatus = result.lockStatus = result.imageProtectStatus = 0;
    result.aliasMapStatus = result.writeTestStatus = 0;
    result.hasPreModifyByte = result.hasPostWriteByte = result.hasPostRestoreByte = true;
    result.preModifyByte = 0;
    result.postWriteByte = result.postRestoreByte = 0xFF;
    result.restoreStatus = failed;
    text = verdictText(result);
    check(text.contains(QStringLiteral("恢复尚未证实完成"))
        && !text.contains(QStringLiteral("绕过已确认")),
        "restore failure cannot be reported as confirmed safe completion");
    result.restoreStatus = 0;
    result.hasPostRestoreByte = false;
    text = verdictText(result);
    check(text.contains(QStringLiteral("恢复后字节不可用"))
        && text.contains(QStringLiteral("恢复尚未证实完成")),
        "successful restore status does not fabricate a failed readback");
    result.flags = 1UL;
    result.hasPostWriteByte = false;
    text = verdictText(result);
    check(text.contains(QStringLiteral("字节证据不完整")), "missing readback prevents effect claims");
    result.hasPostWriteByte = true;
    result.imageProtectStatus = failed;
    text = verdictText(result);
    check(text.contains(QStringLiteral("写入测试  成功"))
        && !text.contains(QStringLiteral("跳过（前置失败）")),
        "independent completed write stage is not hidden by protection failure");
    check(!text.contains(QStringLiteral("HVCI 保护生效"))
        && !text.contains(QStringLiteral("绕过已确认")),
        "local stage evidence does not establish overall protection state");
#endif

    std::printf("MEMORY_RESULT_CHECKS=%d FAILURES=%d\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
