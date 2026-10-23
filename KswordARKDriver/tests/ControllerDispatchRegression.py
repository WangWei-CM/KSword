"""Replay the production registry and dispatcher with bounded WDF substitutes.

This does not load a driver or access storage. Other feature handlers are stubs;
the registry rows, controller forwarding wrappers, and role gate are production C.
"""
from pathlib import Path
import re
import shutil
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[2]
SOURCE = ROOT / "KswordARKDriver" / "src" / "dispatch"
OUTPUT = ROOT / ".codex-build-logs" / "controller-dispatch-regression"
OUTPUT.mkdir(parents=True, exist_ok=True)


def without_includes(text):
    return re.sub(r'^\s*#include[^\n]*\n', '\n', text, flags=re.M)


registry = (SOURCE / "ioctl_registry.c").read_text(encoding="utf-8-sig")
dispatch = (SOURCE / "ioctl_dispatch.c").read_text(encoding="utf-8-sig")
queue = (ROOT / "KswordARKDriver" / "src" / "framework" / "io_queue.c").read_text(encoding="utf-8-sig")
read_callback = re.search(r'^VOID\s+KswordARKDriverEvtIoRead\([\s\S]*?^\}', queue, re.M)
if not read_callback:
    raise RuntimeError("Missing production read callback")
header = (SOURCE / "ioctl_registry.h").read_text(encoding="utf-8-sig")
protocols = ['#include "ark/ark_ioctl.h"']
for name in re.findall(r'^#include "(driver/[^"]+)"', registry, re.M):
    protocols.append(f'#include "{name}"')
# Derive the expected population from production rows so additions cannot stale a literal count.
registry_rows = re.findall(r'^\s*\{\s*IOCTL_KSWORD_ARK_[A-Z0-9_]+\s*,', registry, re.M)
if not registry_rows:
    raise RuntimeError("Missing production registry rows")
protocols.append(f'#define KSWORD_TEST_EXPECTED_REGISTRY_COUNT {len(registry_rows)}U')
(OUTPUT / "dispatch_protocol_replay.h").write_text('\n'.join(protocols), encoding="utf-8")
(OUTPUT / "dispatch_registry_replay.h").write_text(without_includes(header), encoding="utf-8")

# Stub only the unrelated feature implementations, using actual registry declarations.
stubs = []
for name, arguments in re.findall(r'^NTSTATUS\s+(\w+)\(([^;]*?)\);', registry, re.M):
    stubs.append(f'NTSTATUS {name}(WDFDEVICE Device, WDFREQUEST Request, '
                 'size_t InputBufferLength, size_t OutputBufferLength, size_t* BytesReturned)' +
                 '{ return GenericHandler(Device, Request, InputBufferLength, '
                 'OutputBufferLength, BytesReturned); }')
(OUTPUT / "dispatch_feature_stubs.h").write_text('\n'.join(stubs), encoding="utf-8")
(OUTPUT / "dispatch_source_replay.h").write_text(
    '#line 1 "ioctl_registry.c"\n' + without_includes(registry) +
    '\n#line 1 "ioctl_dispatch.c"\n' + without_includes(dispatch) +
    '\n#line 1 "io_queue.c"\n' + read_callback[0], encoding="utf-8")

compiler = shutil.which("gcc")
if not compiler:
    sys.exit("Native x64 MinGW gcc is required for dispatcher role regression")
executable = OUTPUT / "controller_dispatch_regression.exe"
subprocess.run([compiler, "-std=c11", "-O2", "-Wall", "-Wextra", "-Werror",
                "-I", str(OUTPUT), "-I", str(ROOT / "shared"),
                "-I", str(ROOT / "KswordARKDriver" / "include"),
                str(ROOT / "KswordARKDriver" / "tests" / "controller_dispatch_regression.c"),
                "-o", str(executable)], check=True)
subprocess.run([str(executable)], check=True)
