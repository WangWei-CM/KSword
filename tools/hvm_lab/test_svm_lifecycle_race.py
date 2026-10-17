"""Compile the production AMD stop/mutation bodies with deterministic callback interleavings.

This host-only regression never loads a driver or executes virtualization instructions.
The small adapter replaces the kernel phase and broadcast operations, while the functions
being checked are copied directly from the production source on every run.
"""

from __future__ import annotations

import argparse
import os
import re
import shutil
import subprocess
import uuid
from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]
RUNTIME = ROOT / "KswordARKDriver/src/features/hvm/hvm_runtime.c"
LIFECYCLE = ROOT / "KswordARKDriver/src/features/hvm/hvm_svm_lifecycle.c"


def function(source: str, name: str) -> str:
    """Return a complete production function, including its real declaration."""
    match = re.search(r"(?:static\s+)?NTSTATUS\s+" + re.escape(name) + r"\s*\(", source)
    if match is None:
        raise AssertionError(f"Production function missing: {name}")
    body = source.index("{", match.end())
    depth = 1
    end = body + 1
    while depth:
        depth += (source[end] == "{") - (source[end] == "}")
        end += 1
    return source[match.start():end]


ADAPTER = r'''
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#define _Inout_
#define FALSE 0
#define STATUS_SUCCESS ((NTSTATUS)0)
#define STATUS_DEVICE_BUSY ((NTSTATUS)-1)
#define STATUS_HV_OPERATION_FAILED ((NTSTATUS)-2)
#define NT_SUCCESS(value) ((value) >= 0)
#define KSWORD_ARK_HVM_STATE_UNLOAD_GUARD_ARMED 1U
#define KSWORD_ARK_HVM_STATE_RESIDENT_STOPPING 2U
#define KSWORD_ARK_HVM_STATE_RESIDENT_ACTIVE 4U
#define KSWORD_ARK_HVM_STATE_ROLLBACK_REQUIRED 8U
#define KSWORD_ARK_HVM_IMPLEMENTATION_CAPABILITY_ONLY 1U
#define KSWORD_ARK_HVM_IMPLEMENTATION_UNSUPPORTED 0U
typedef int32_t LONG;
typedef int32_t NTSTATUS;
typedef struct { void* Cpus; unsigned Identity; unsigned Count; } KSW_SVM_STATE;
typedef struct {
    KSW_SVM_STATE* BackendContext;
    LONG ResidentContextPreparing, ResidentProcessorCount, PowerTransitionPending;
    unsigned StateFlags, ResidentImplementation, NestedImplementation;
} KSW_HVM_RUNTIME;
static unsigned checks, broadcasts, disarms, acquisitions, releases;
static int phase, require_marker, replace_context, force_failure, broadcast_failure;
static KSW_HVM_RUNTIME* runtime_under_test;
static KSW_SVM_STATE expected_state;
#define CHECK(value) do { ++checks; if (!(value)) { \
    fprintf(stderr, "failed line %d: %s\n", __LINE__, #value); exit(1); } } while (0)
static LONG InterlockedExchange(LONG* value, LONG replacement)
{ LONG previous = *value; *value = replacement; return previous; }
static inline LONG InterlockedCompareExchange(LONG* value, LONG replacement, LONG comparand)
{ LONG previous = *value; if (previous == comparand) { *value = replacement; } return previous; }
static NTSTATUS KswordARKHvmAcquireResidentTransition(KSW_HVM_RUNTIME* runtime)
{
    ++acquisitions;
    CHECK(!phase);
    if (require_marker) { CHECK(runtime->ResidentContextPreparing == 1); }
    if (force_failure) { return STATUS_DEVICE_BUSY; }
    phase = 1;
    /* A queued reader resumes only after the previous allocation lifetime has retired. */
    if (replace_context) { runtime->BackendContext = &expected_state; }
    return STATUS_SUCCESS;
}
static void KswordARKHvmReleaseResidentTransition(KSW_HVM_RUNTIME* runtime)
{ (void)runtime; CHECK(phase); ++releases; phase = 0; }
static void KswordARKHvmStateSet(KSW_HVM_RUNTIME* runtime, unsigned flags)
{ runtime->StateFlags |= flags; }
static void KswordARKHvmStateClear(KSW_HVM_RUNTIME* runtime, unsigned flags)
{ runtime->StateFlags &= ~flags; }
static NTSTATUS KswordARKHvmDisarmUnloadGuard(KSW_HVM_RUNTIME* runtime)
{ ++disarms; runtime->StateFlags &= ~KSWORD_ARK_HVM_STATE_UNLOAD_GUARD_ARMED; return STATUS_SUCCESS; }
static NTSTATUS KswSvmBroadcast(KSW_SVM_STATE* state, int start)
{
    CHECK(phase && !start);
    /* No IPI may traverse a partial/freed ledger or the lifetime cached before phase acquisition. */
    CHECK(runtime_under_test->ResidentContextPreparing == 0);
    CHECK(state == &expected_state);
    ++broadcasts;
    if (broadcast_failure) { return STATUS_HV_OPERATION_FAILED; }
    runtime_under_test->ResidentProcessorCount = 0;
    return STATUS_SUCCESS;
}
static void KswordSvmWatchStopPokes(KSW_SVM_STATE* state) { (void)state; }
static void KswordSvmWatchStartPokes(KSW_SVM_STATE* state) { (void)state; }
static void Reset(KSW_HVM_RUNTIME* runtime)
{
    runtime->BackendContext = &expected_state;
    runtime->ResidentContextPreparing = runtime->ResidentProcessorCount = runtime->PowerTransitionPending = 0;
    runtime->StateFlags = KSWORD_ARK_HVM_STATE_UNLOAD_GUARD_ARMED;
    runtime->ResidentImplementation = runtime->NestedImplementation = 99;
    runtime_under_test = runtime;
    expected_state.Cpus = &expected_state;
    expected_state.Identity = 20;
    expected_state.Count = 1;
    broadcasts = disarms = acquisitions = releases = 0;
    phase = require_marker = replace_context = force_failure = broadcast_failure = 0;
}
'''


CASES = r'''
int main(void)
{
    KSW_HVM_RUNTIME runtime;
    KSW_SVM_STATE retired_state = { &retired_state, 10, 1 };
    NTSTATUS status;

    /* Preparing/retiring state is deliberately not a usable broadcast ledger. */
    Reset(&runtime);
    runtime.ResidentContextPreparing = 1;
    runtime.BackendContext = &retired_state;
    CHECK(KswordSvmStop(&runtime) == STATUS_SUCCESS);
    CHECK(broadcasts == 0 && disarms == 0 && acquisitions == 1 && releases == 1);
    CHECK(runtime.StateFlags == KSWORD_ARK_HVM_STATE_UNLOAD_GUARD_ARMED);

    /* Allocation ownership must be fetched after a queued phase acquisition returns. */
    Reset(&runtime);
    runtime.BackendContext = &retired_state;
    replace_context = 1;
    CHECK(KswordSvmStop(&runtime) == STATUS_SUCCESS);
    CHECK(broadcasts == 1 && disarms == 1 && releases == 1);

    /* Contradictory hardware ownership must remain fail closed during mutation. */
    Reset(&runtime);
    runtime.ResidentContextPreparing = runtime.ResidentProcessorCount = 1;
    CHECK(KswordSvmStop(&runtime) == STATUS_DEVICE_BUSY);
    CHECK(broadcasts == 0 && disarms == 0 && runtime.ResidentProcessorCount == 1);

    /* An ordinary resident still has to acknowledge a real all-CPU stop. */
    Reset(&runtime);
    runtime.ResidentProcessorCount = 1;
    CHECK(KswordSvmStop(&runtime) == STATUS_SUCCESS);
    CHECK(broadcasts == 1 && disarms == 1 && runtime.ResidentProcessorCount == 0);

    /* Failed broadcast retains hardware and unload ownership. */
    Reset(&runtime);
    runtime.ResidentProcessorCount = broadcast_failure = 1;
    CHECK(KswordSvmStop(&runtime) == STATUS_HV_OPERATION_FAILED);
    CHECK(disarms == 0 && runtime.ResidentProcessorCount == 1);
    CHECK((runtime.StateFlags & KSWORD_ARK_HVM_STATE_ROLLBACK_REQUIRED) != 0);

    /* A power stop keeps the existing unload interlock until S0 resumes. */
    Reset(&runtime);
    runtime.PowerTransitionPending = 1;
    CHECK(KswordSvmStop(&runtime) == STATUS_SUCCESS);
    CHECK(broadcasts == 1 && disarms == 0);

    /* Failed phase acquisition does not use the ledger or invent stop evidence. */
    Reset(&runtime);
    force_failure = 1;
    CHECK(KswordSvmStop(&runtime) == STATUS_DEVICE_BUSY);
    CHECK(broadcasts == 0 && disarms == 0 && releases == 0);

    /* The mutation marker closes new readers before the barrier drains old readers. */
    Reset(&runtime);
    require_marker = 1;
    status = KswordARKHvmBeginBackendContextMutation(&runtime);
    CHECK(status == STATUS_SUCCESS && runtime.ResidentContextPreparing == 1);
    CHECK(acquisitions == 1 && releases == 1 && !phase);
    CHECK(KswordSvmStop(&runtime) == STATUS_SUCCESS);
    CHECK(broadcasts == 0);
    InterlockedExchange(&runtime.ResidentContextPreparing, 0);
    require_marker = 0;
    CHECK(KswordSvmStop(&runtime) == STATUS_SUCCESS && broadcasts == 1);

    /* A rejected mutation never leaves the runtime permanently marked preparing. */
    Reset(&runtime);
    force_failure = require_marker = 1;
    CHECK(KswordARKHvmBeginBackendContextMutation(&runtime) == STATUS_DEVICE_BUSY);
    CHECK(runtime.ResidentContextPreparing == 0 && releases == 0);

    printf("AMD lifecycle callback regression: %u checks passed\n", checks);
    return 0;
}
'''


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--compiler", default=shutil.which("gcc") or shutil.which("clang") or shutil.which("cc"))
    parser.add_argument("--prove-regression", action="store_true", help="Also verify the unpatched HEAD stop fails these same cases")
    args = parser.parse_args()
    if not args.compiler:
        parser.error("A host GCC/Clang compiler is required; no driver build or load is used")
    runtime = RUNTIME.read_text(encoding="utf-8-sig")
    lifecycle = LIFECYCLE.read_text(encoding="utf-8-sig")
    bodies = function(runtime, "KswordARKHvmBeginBackendContextMutation") + "\n" + function(lifecycle, "KswSvmDisarm")
    stop = function(lifecycle, "KswordSvmStop")
    work_root = (ROOT / "tools/hvm_lab/artifacts").resolve()
    work_root.mkdir(parents=True, exist_ok=True)
    # Ordinary inherited Windows ACLs keep the compiler subprocess able to use this directory.
    temporary = work_root / ("ksword-svm-lifecycle-" + uuid.uuid4().hex)
    temporary.mkdir()
    temporary.resolve().relative_to(work_root)
    source = temporary / "regression.c"
    executable = temporary / "regression.exe"
    compiler_environment = dict(os.environ, TMP=str(temporary), TEMP=str(temporary))
    try:
        source.write_text(ADAPTER + bodies + "\n" + stop + CASES, encoding="utf-8")
        command = [args.compiler, "-std=c11", "-Wall", "-Wextra", "-Werror", "-O2", str(source), "-o", str(executable)]
        subprocess.run(command, check=True, env=compiler_environment)
        subprocess.run([str(executable)], check=True)
        if args.prove_regression:
            original = subprocess.check_output(["git", "show", "HEAD:KswordARKDriver/src/features/hvm/hvm_svm_lifecycle.c"], cwd=ROOT).decode("utf-8-sig")
            source.write_text(ADAPTER + bodies + "\n" + function(original, "KswordSvmStop") + CASES, encoding="utf-8")
            subprocess.run(command, check=True, env=compiler_environment)
            result = subprocess.run([str(executable)], capture_output=True, text=True)
            if result.returncode == 0:
                raise AssertionError("Unpatched HEAD unexpectedly passed the regression")
            print("Unpatched HEAD rejected as expected: " + result.stderr.strip())
    finally:
        # Remove only the exact two generated artifacts in this verified child directory.
        source.unlink(missing_ok=True)
        executable.unlink(missing_ok=True)
        temporary.rmdir()


if __name__ == "__main__":
    main()
