#!/usr/bin/env python3
"""Run production redirect IOCTL/backend with shared-buffer and allocation mocks."""

from __future__ import annotations

import argparse
import os
from pathlib import Path
import re
import shutil
import subprocess
import uuid

from test_driver_candidate_reference import ROOT, function


HARNESS = r"""
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>
#define _KERNEL_MODE 1
#include "driver/KswordArkRedirectIoctl.h"
#define _In_
#define _In_opt_
#define _Out_
#define _Inout_
#define _Out_writes_bytes_(x)
#define _Out_writes_bytes_to_(x,y)
#define VOID void
#define UNREFERENCED_PARAMETER(x) ((void)(x))
#define NT_SUCCESS(x) ((x) >= 0)
using ULONG = unsigned long;
using SIZE_T = size_t;
using USHORT = unsigned short;
using WCHAR = wchar_t;
using BOOLEAN = bool;
using PVOID = void*;
using WDFDEVICE = void*;
using NTSTATUS = int;
constexpr BOOLEAN TRUE = true, FALSE = false;
constexpr NTSTATUS STATUS_SUCCESS = 0, STATUS_INVALID_PARAMETER = -1, STATUS_BUFFER_TOO_SMALL = -2;
constexpr NTSTATUS STATUS_REVISION_MISMATCH = -3, STATUS_NOT_SUPPORTED = -4;
constexpr NTSTATUS STATUS_INSUFFICIENT_RESOURCES = -5, STATUS_ACCESS_DENIED = -6;
constexpr ULONG KSWORD_ARK_REDIRECT_IOCTL_TAG_SNAPSHOT = 0x7352734BUL;
static_assert(sizeof(ULONG) == 4 && sizeof(WCHAR) == 2, "This regression uses the Windows ABI.");
static_assert(sizeof(KSWORD_ARK_REDIRECT_RULE) == 2112, "Shared rule layout changed.");
static_assert(sizeof(KSWORD_ARK_REDIRECT_SET_RULES_REQUEST) == 33824, "Shared request layout changed.");
struct KSWORD_ARK_REDIRECT_RUNTIME {
    int Lock = 0;
    ULONG RuntimeFlags = 0, FileRuleCount = 0, RegistryRuleCount = 0, Generation = 0;
    KSWORD_ARK_REDIRECT_RULE Rules[KSWORD_ARK_REDIRECT_MAX_RULES] = {};
};
static KSWORD_ARK_REDIRECT_RUNTIME runtime;
struct FakeRequest {
    std::vector<unsigned char> buffer;
    SIZE_T inputLength = sizeof(KSWORD_ARK_REDIRECT_SET_RULES_REQUEST);
    SIZE_T outputLength = sizeof(KSWORD_ARK_REDIRECT_SET_RULES_RESPONSE);
    bool writable = true;
    FakeRequest() : buffer(sizeof(KSWORD_ARK_REDIRECT_SET_RULES_REQUEST) + 32, 0xCD) {}
};
using WDFREQUEST = FakeRequest*;
static bool failAllocation;
static unsigned allocated, freed, held, lockAcquires, backendCalls, cases;
static KSWORD_ARK_REDIRECT_RUNTIME* KswordARKRedirectGetRuntime() { return &runtime; }
static void RtlZeroMemory(PVOID target, SIZE_T bytes) { std::memset(target, 0, bytes); }
static void RtlCopyMemory(PVOID target, const void* source, SIZE_T bytes) { std::memcpy(target, source, bytes); }
static void KswordARKAcquirePushLockExclusive(int* lock) { assert(*lock == 0); *lock = 1; ++lockAcquires; }
static void KswordARKReleasePushLockExclusive(int* lock) { assert(*lock == 1); *lock = 0; }
static void KswordARKRedirectIoctlLog(WDFDEVICE, const char*, const char*, ...) {}
static NTSTATUS KswordARKValidateDeviceIoControlWriteAccess(WDFREQUEST request) {
    return request->writable ? STATUS_SUCCESS : STATUS_ACCESS_DENIED;
}
static NTSTATUS KswordARKFileMonitorEnsureFilteringStarted() { return STATUS_SUCCESS; }
static NTSTATUS KswordARKRedirectNormalizeFilePath(WCHAR*) { return STATUS_SUCCESS; }
static NTSTATUS KswordARKRetrieveRequiredInputBuffer(WDFREQUEST request, SIZE_T bytes, PVOID* buffer, SIZE_T* actual) {
    *actual = request->inputLength; *buffer = request->buffer.data();
    return bytes <= *actual ? STATUS_SUCCESS : STATUS_BUFFER_TOO_SMALL;
}
static NTSTATUS KswordARKRetrieveRequiredOutputBuffer(WDFREQUEST request, SIZE_T bytes, PVOID* buffer, SIZE_T* actual) {
    *actual = request->outputLength; *buffer = request->buffer.data();
    return bytes <= *actual ? STATUS_SUCCESS : STATUS_BUFFER_TOO_SMALL;
}
[[maybe_unused]] static PVOID KswordARKAllocateNonPagedPool(SIZE_T bytes, ULONG tag) {
    assert(bytes == sizeof(KSWORD_ARK_REDIRECT_SET_RULES_REQUEST) && tag == KSWORD_ARK_REDIRECT_IOCTL_TAG_SNAPSHOT);
    if (failAllocation) return nullptr;
    ++allocated; ++held; return new unsigned char[bytes];
}
[[maybe_unused]] static void ExFreePoolWithTag(PVOID allocation, ULONG tag) {
    assert(allocation != nullptr && tag == KSWORD_ARK_REDIRECT_IOCTL_TAG_SNAPSHOT && held != 0);
    ++freed; --held; delete[] static_cast<unsigned char*>(allocation);
}
/*PRODUCTION*/
static NTSTATUS countedBackend(const KSWORD_ARK_REDIRECT_SET_RULES_REQUEST* request,
                               PVOID output, SIZE_T bytes, SIZE_T* written) {
    assert(request != output && held == 1);
    ++backendCalls;
    return KswordARKRedirectSetRules(request, output, bytes, written);
}
#define KswordARKRedirectSetRules countedBackend
/*HANDLER*/
#undef KswordARKRedirectSetRules
static KSWORD_ARK_REDIRECT_SET_RULES_REQUEST* prepare(FakeRequest& request, ULONG action, ULONG count = 0) {
    auto* input = reinterpret_cast<KSWORD_ARK_REDIRECT_SET_RULES_REQUEST*>(request.buffer.data());
    std::memset(input, 0, sizeof(*input));
    input->version = KSWORD_ARK_REDIRECT_PROTOCOL_VERSION; input->action = action; input->ruleCount = count;
    for (ULONG index = 0; index < count && index < KSWORD_ARK_REDIRECT_MAX_RULES; ++index) {
        auto& rule = input->rules[index];
        rule.ruleId = index + 1; rule.type = index % 2 == 0 ? KSWORD_ARK_REDIRECT_TYPE_FILE : KSWORD_ARK_REDIRECT_TYPE_REGISTRY;
        rule.action = KSWORD_ARK_REDIRECT_ACTION_REPLACE; rule.matchMode = KSWORD_ARK_REDIRECT_MATCH_PREFIX;
        rule.flags = KSWORD_ARK_REDIRECT_RULE_FLAG_ENABLED;
        rule.sourcePath[0] = rule.targetPath[0] = L'\\'; rule.sourcePath[1] = L's'; rule.targetPath[1] = L't';
    }
    return input;
}
static KSWORD_ARK_REDIRECT_SET_RULES_RESPONSE* response(FakeRequest& request) {
    return reinterpret_cast<KSWORD_ARK_REDIRECT_SET_RULES_RESPONSE*>(request.buffer.data());
}
static NTSTATUS invoke(FakeRequest& request, SIZE_T* written) {
    const unsigned beforeHeld = held;
    const NTSTATUS status = KswordARKRedirectIoctlSetRules(nullptr, &request, request.inputLength, request.outputLength, written);
    assert(beforeHeld == 0 && held == 0 && allocated == freed);
    for (SIZE_T index = sizeof(KSWORD_ARK_REDIRECT_SET_RULES_REQUEST); index < request.buffer.size(); ++index)
        assert(request.buffer[index] == 0xCD);
    ++cases; return status;
}
static std::vector<unsigned char> configuration() {
    const auto* bytes = reinterpret_cast<const unsigned char*>(&runtime);
    return std::vector<unsigned char>(bytes, bytes + sizeof(runtime));
}
static void unchanged(const std::vector<unsigned char>& before) {
    assert(std::memcmp(before.data(), &runtime, sizeof(runtime)) == 0);
}
int main() {
    SIZE_T written = 0;
    FakeRequest request;
    prepare(request, KSWORD_ARK_REDIRECT_ACTION_REPLACE, KSWORD_ARK_REDIRECT_MAX_RULES);
    assert(invoke(request, &written) == STATUS_SUCCESS);
    assert(written == sizeof(KSWORD_ARK_REDIRECT_SET_RULES_RESPONSE) && response(request)->status == KSWORD_ARK_REDIRECT_STATUS_APPLIED);
    assert(runtime.FileRuleCount == 8 && runtime.RegistryRuleCount == 8 && runtime.Generation == 1);
    assert(runtime.Rules[15].ruleId == 16 && runtime.Rules[15].sourcePath[1] == L's');
    auto old = configuration();
    prepare(request, KSWORD_ARK_REDIRECT_ACTION_REPLACE, 2)->rules[1].sourcePath[0] = L'x';
    assert(invoke(request, &written) == STATUS_SUCCESS && response(request)->status == KSWORD_ARK_REDIRECT_STATUS_INVALID_RULE);
    assert(response(request)->rejectedIndex == 1); unchanged(old);
    prepare(request, KSWORD_ARK_REDIRECT_ACTION_REPLACE, 1)->rules[0].type = 999;
    assert(invoke(request, &written) == STATUS_SUCCESS && response(request)->status == KSWORD_ARK_REDIRECT_STATUS_UNSUPPORTED_TYPE); unchanged(old);
    prepare(request, KSWORD_ARK_REDIRECT_ACTION_REPLACE, KSWORD_ARK_REDIRECT_MAX_RULES + 1);
    assert(invoke(request, &written) == STATUS_SUCCESS && response(request)->status == KSWORD_ARK_REDIRECT_STATUS_INVALID_RULE); unchanged(old);
    prepare(request, 999);
    assert(invoke(request, &written) == STATUS_SUCCESS && response(request)->status == KSWORD_ARK_REDIRECT_STATUS_INVALID_RULE); unchanged(old);
    prepare(request, KSWORD_ARK_REDIRECT_ACTION_REPLACE, 1)->version = 999;
    assert(invoke(request, &written) == STATUS_SUCCESS && response(request)->lastStatus == STATUS_REVISION_MISMATCH); unchanged(old);
    prepare(request, KSWORD_ARK_REDIRECT_ACTION_REPLACE, 1); failAllocation = true;
    const unsigned calls = backendCalls;
    assert(invoke(request, &written) == STATUS_INSUFFICIENT_RESOURCES && written == 0 && backendCalls == calls); unchanged(old); failAllocation = false;
    prepare(request, KSWORD_ARK_REDIRECT_ACTION_CLEAR); request.outputLength -= 1;
    assert(invoke(request, &written) == STATUS_BUFFER_TOO_SMALL && written == 0 && backendCalls == calls); unchanged(old); request.outputLength += 1;
    request.inputLength -= 1;
    assert(invoke(request, &written) == STATUS_BUFFER_TOO_SMALL && written == 0 && backendCalls == calls); unchanged(old); request.inputLength += 1;
    request.writable = false;
    assert(invoke(request, &written) == STATUS_ACCESS_DENIED && written == 0 && backendCalls == calls); unchanged(old); request.writable = true;
    prepare(request, KSWORD_ARK_REDIRECT_ACTION_REPLACE, 1);
    assert(invoke(request, &written) == STATUS_SUCCESS && runtime.FileRuleCount == 1 && runtime.RegistryRuleCount == 0);
    assert(runtime.Rules[1].flags == 0 && runtime.Rules[15].ruleId == 0);
    prepare(request, KSWORD_ARK_REDIRECT_ACTION_CLEAR);
    assert(invoke(request, &written) == STATUS_SUCCESS && response(request)->status == KSWORD_ARK_REDIRECT_STATUS_CLEARED && runtime.FileRuleCount == 0);
    prepare(request, KSWORD_ARK_REDIRECT_ACTION_REPLACE, 1)->rules[0].flags = 0;
    assert(invoke(request, &written) == STATUS_SUCCESS && response(request)->appliedCount == 0 && runtime.Rules[0].ruleId == 0);
    prepare(request, KSWORD_ARK_REDIRECT_ACTION_REPLACE, 1);
    assert(invoke(request, &written) == STATUS_SUCCESS && runtime.FileRuleCount == 1);
    prepare(request, KSWORD_ARK_REDIRECT_ACTION_DISABLE);
    assert(invoke(request, &written) == STATUS_SUCCESS && response(request)->status == KSWORD_ARK_REDIRECT_STATUS_DISABLED && runtime.FileRuleCount == 0);
    assert(allocated == freed && held == 0 && lockAcquires == 6);
    std::printf("redirect regression: %u cases; rule=%zu request=%zu old fixed-stack objects=%zu bytes\n", cases,
                sizeof(KSWORD_ARK_REDIRECT_RULE), sizeof(KSWORD_ARK_REDIRECT_SET_RULES_REQUEST),
                sizeof(KSWORD_ARK_REDIRECT_SET_RULES_REQUEST) + sizeof(runtime.Rules));
}
"""


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cxx", default="g++")
    args = parser.parse_args()
    compiler = shutil.which(args.cxx)
    if not compiler:
        raise SystemExit(f"C++ compiler unavailable: {args.cxx}")
    runtime = "KswordARKDriver/src/features/redirect/redirect_runtime.c"
    production = "\n\n".join(function(runtime, name) for name in [
        "KswordARKRedirectCountRulesByTypeLocked", "KswordARKRedirectIsRulePathValid", "KswordARKRedirectValidateRule",
        "KswordARKRedirectRefreshFlagsLocked", "KswordARKRedirectSetRules",
    ])
    handler = function("KswordARKDriver/src/features/redirect/redirect_ioctl.c", "KswordARKRedirectIoctlSetRules")
    environment = os.environ.copy()
    environment["PATH"] = str(Path(compiler).parent) + os.pathsep + environment.get("PATH", "")
    temporary_root = ROOT / ".codex-tmp" / "redirect-stack-tests"
    if not temporary_root.resolve().is_relative_to(ROOT.resolve()):
        raise SystemExit("Regression scratch path must remain inside the repository.")
    temporary = temporary_root / f"case-{uuid.uuid4().hex}"
    temporary.mkdir(parents=True)
    try:
        source = temporary / "regression.cpp"
        executable = temporary / ("regression.exe" if os.name == "nt" else "regression")
        source.write_text(HARNESS.replace("/*PRODUCTION*/", production).replace("/*HANDLER*/", handler), encoding="utf-8")
        subprocess.run([compiler, "-std=c++17", "-O1", "-fno-inline", "-fstack-usage", "-Wframe-larger-than=1024",
                        "-Wall", "-Wextra", "-Werror", "-I", str(ROOT / "shared"), str(source), "-o", str(executable)],
                       check=True, env=environment)
        subprocess.run([str(executable)], check=True, env=environment)
        stack_files = list(temporary.glob("*.su"))
        assert stack_files, "Compiler did not report stack usage."
        for name in ["KswordARKRedirectIoctlSetRules", "KswordARKRedirectSetRules"]:
            lines = [line for file in stack_files for line in file.read_text(encoding="utf-8").splitlines() if re.search(rf"\b{name}\(", line)]
            assert len(lines) == 1, f"Missing stack measurement for {name}"
            fields = lines[0].split("\t")
            assert int(fields[-2]) <= 1024, f"Production frame too large: {lines[0]}"
            print(f"{name}: compiler-reported frame {fields[-2]} bytes ({fields[-1]})")
    finally:
        if temporary.resolve().is_relative_to(temporary_root.resolve()):
            shutil.rmtree(temporary)


if __name__ == "__main__":
    main()
