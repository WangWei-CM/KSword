#pragma once

// Shared protocol for KswordARK log channel.

// Kernel-mode device object name.
#define KSWORD_ARK_LOG_DEVICE_NT_NAME L"\\Device\\KswordARKLog"

// Kernel-mode DOS symbolic link name.
#define KSWORD_ARK_LOG_DOS_NAME L"\\DosDevices\\KswordARKLog"

// User-mode CreateFileW path.
#define KSWORD_ARK_LOG_WIN32_PATH L"\\\\.\\KswordARKLog"

// Log frame terminator marker.
#define KSWORD_ARK_LOG_END_MARKER "END_OF_LOG"
