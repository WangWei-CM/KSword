#pragma once

#include "KswordArkProcessIoctl.h"

/*
 * RXPF is an explicitly experimental, administrator-only research protocol.
 * The protocol never treats a partial build/ABI match as permission to call an
 * internal memory-manager routine or replace vector 14.
 */
#define KSWORD_ARK_RXPF_PROTOCOL_VERSION 1UL

#define KSWORD_ARK_IOCTL_FUNCTION_RXPF_QUERY_SUPPORT   0x900UL
#define KSWORD_ARK_IOCTL_FUNCTION_RXPF_REGISTER_PAGE   0x901UL
#define KSWORD_ARK_IOCTL_FUNCTION_RXPF_CHANGE_PAGE     0x902UL
#define KSWORD_ARK_IOCTL_FUNCTION_RXPF_QUERY_PAGE      0x903UL
#define KSWORD_ARK_IOCTL_FUNCTION_RXPF_WRITE_PAGE      0x904UL
#define KSWORD_ARK_IOCTL_FUNCTION_RXPF_SET_EMULATION   0x905UL
#define KSWORD_ARK_IOCTL_FUNCTION_RXPF_QUERY_STATS     0x906UL
#define KSWORD_ARK_IOCTL_FUNCTION_RXPF_DRAIN_EVENTS    0x907UL
#define KSWORD_ARK_IOCTL_FUNCTION_RXPF_UNREGISTER_PAGE 0x908UL
#define KSWORD_ARK_IOCTL_FUNCTION_RXPF_RUN_SELF_TEST   0x909UL
#define KSWORD_ARK_IOCTL_FUNCTION_RXPF_SET_HOOK_POINT 0x90AUL
/* 0x90B/FILE_WRITE_ACCESS belongs to FILE_IRP_SUBMIT; use a distinct full CTL_CODE. */
#define KSWORD_ARK_IOCTL_FUNCTION_RXPF_QUERY_BACKUP   0x931UL
/*
 * 注意：function code 0x90C 已被 ProcessProtect 的 SET_PROCESS_PROTECT_CONFIG 占用。
 * ATOMIC_COMMIT 挑一个未分配的位置 0x925，避开 Research / HVM / DDMA / PFN /
 * Clipboard / Memory / HVM_* 等已占段，防止 IOCTL 注册表 duplicate 导致请求
 * 命中错误 handler → BUFFER_TOO_SMALL（已实证）。
 */
#define KSWORD_ARK_IOCTL_FUNCTION_RXPF_ATOMIC_COMMIT 0x925UL
/*
 * EXEC_PROOF：一次 IOCTL 走完 Register(ALLOCATED_TEST)→Change(mode=2)→
 * Write(custom payload)→PageFaultInstall→ConcurrentExecute→Reset→Unregister，
 * 用于 **直接** 证明 payload 被 CPU 取指并通过 shadow IDT #PF emulator 执行。
 * 页面始终 NX，mode=1 restore 永不被尝试——走 ALLOCATED_TEST（driver 自有 PFN，
 * 不受 VTL1 SHA-512 hash enforcement），所以不存在 "字节与 digest 不符导致 restore
 * 被拒" 的问题。
 */
/*
 * 原 0x926：实证返回 win32=998 (ERROR_NOACCESS) + bytesReturned=0，handler sentinel 从未写入。
 * ATOMIC_COMMIT 0x925 走同样 dispatcher 工作正常。换成 0x930（远离 0x925），排除 function
 * code 相邻性或 0x926 被某个 undocumented filter 识别的可能。
 */
#define KSWORD_ARK_IOCTL_FUNCTION_RXPF_EXEC_PROOF 0x930UL

#define IOCTL_KSWORD_ARK_RXPF_QUERY_SUPPORT \
    CTL_CODE(KSWORD_ARK_IOCTL_DEVICE_TYPE, KSWORD_ARK_IOCTL_FUNCTION_RXPF_QUERY_SUPPORT, METHOD_BUFFERED, FILE_WRITE_ACCESS)
#define IOCTL_KSWORD_ARK_RXPF_REGISTER_PAGE \
    CTL_CODE(KSWORD_ARK_IOCTL_DEVICE_TYPE, KSWORD_ARK_IOCTL_FUNCTION_RXPF_REGISTER_PAGE, METHOD_BUFFERED, FILE_WRITE_ACCESS)
#define IOCTL_KSWORD_ARK_RXPF_CHANGE_PAGE \
    CTL_CODE(KSWORD_ARK_IOCTL_DEVICE_TYPE, KSWORD_ARK_IOCTL_FUNCTION_RXPF_CHANGE_PAGE, METHOD_BUFFERED, FILE_WRITE_ACCESS)
#define IOCTL_KSWORD_ARK_RXPF_QUERY_PAGE \
    CTL_CODE(KSWORD_ARK_IOCTL_DEVICE_TYPE, KSWORD_ARK_IOCTL_FUNCTION_RXPF_QUERY_PAGE, METHOD_BUFFERED, FILE_WRITE_ACCESS)
#define IOCTL_KSWORD_ARK_RXPF_WRITE_PAGE \
    CTL_CODE(KSWORD_ARK_IOCTL_DEVICE_TYPE, KSWORD_ARK_IOCTL_FUNCTION_RXPF_WRITE_PAGE, METHOD_BUFFERED, FILE_WRITE_ACCESS)
#define IOCTL_KSWORD_ARK_RXPF_SET_EMULATION \
    CTL_CODE(KSWORD_ARK_IOCTL_DEVICE_TYPE, KSWORD_ARK_IOCTL_FUNCTION_RXPF_SET_EMULATION, METHOD_BUFFERED, FILE_WRITE_ACCESS)
#define IOCTL_KSWORD_ARK_RXPF_QUERY_STATS \
    CTL_CODE(KSWORD_ARK_IOCTL_DEVICE_TYPE, KSWORD_ARK_IOCTL_FUNCTION_RXPF_QUERY_STATS, METHOD_BUFFERED, FILE_WRITE_ACCESS)
#define IOCTL_KSWORD_ARK_RXPF_DRAIN_EVENTS \
    CTL_CODE(KSWORD_ARK_IOCTL_DEVICE_TYPE, KSWORD_ARK_IOCTL_FUNCTION_RXPF_DRAIN_EVENTS, METHOD_BUFFERED, FILE_WRITE_ACCESS)
#define IOCTL_KSWORD_ARK_RXPF_UNREGISTER_PAGE \
    CTL_CODE(KSWORD_ARK_IOCTL_DEVICE_TYPE, KSWORD_ARK_IOCTL_FUNCTION_RXPF_UNREGISTER_PAGE, METHOD_BUFFERED, FILE_WRITE_ACCESS)
#define IOCTL_KSWORD_ARK_RXPF_RUN_SELF_TEST \
    CTL_CODE(KSWORD_ARK_IOCTL_DEVICE_TYPE, KSWORD_ARK_IOCTL_FUNCTION_RXPF_RUN_SELF_TEST, METHOD_BUFFERED, FILE_WRITE_ACCESS)
#define IOCTL_KSWORD_ARK_RXPF_SET_HOOK_POINT \
    CTL_CODE(KSWORD_ARK_IOCTL_DEVICE_TYPE, KSWORD_ARK_IOCTL_FUNCTION_RXPF_SET_HOOK_POINT, METHOD_BUFFERED, FILE_WRITE_ACCESS)
#define IOCTL_KSWORD_ARK_RXPF_QUERY_BACKUP \
    CTL_CODE(KSWORD_ARK_IOCTL_DEVICE_TYPE, KSWORD_ARK_IOCTL_FUNCTION_RXPF_QUERY_BACKUP, METHOD_BUFFERED, FILE_WRITE_ACCESS)
#define IOCTL_KSWORD_ARK_RXPF_ATOMIC_COMMIT \
    CTL_CODE(KSWORD_ARK_IOCTL_DEVICE_TYPE, KSWORD_ARK_IOCTL_FUNCTION_RXPF_ATOMIC_COMMIT, METHOD_BUFFERED, FILE_WRITE_ACCESS)
#define IOCTL_KSWORD_ARK_RXPF_EXEC_PROOF \
    CTL_CODE(KSWORD_ARK_IOCTL_DEVICE_TYPE, KSWORD_ARK_IOCTL_FUNCTION_RXPF_EXEC_PROOF, METHOD_BUFFERED, FILE_WRITE_ACCESS)

#define KSWORD_ARK_RXPF_CONFIRMATION_TOKEN 0x46505852UL
#define KSWORD_ARK_RXPF_MAX_WRITE_BYTES 64UL
#define KSWORD_ARK_RXPF_MAX_EVENT_ROWS 64UL

#define KSWORD_ARK_RXPF_FLAG_UI_CONFIRMED 0x00000001UL
#define KSWORD_ARK_RXPF_FLAG_CAPTURE_BACKUP 0x00000002UL
/*
 * Unregister 专用：跳过 ReleaseRecord 的 Backup→page 自动恢复。
 * 让 R3 写入的字节保留到 PTE 回 RX 之后——必须由调用方自己负责
 * 在短窗口内把字节改回去（否则 PatchGuard/异常指令会蓝屏）。
 */
#define KSWORD_ARK_RXPF_FLAG_SKIP_BACKUP_RESTORE 0x00000004UL

#define KSWORD_ARK_RXPF_TARGET_ALLOCATED_TEST 1UL
#define KSWORD_ARK_RXPF_TARGET_SELF_IMAGE_TEST 2UL
#define KSWORD_ARK_RXPF_TARGET_EXTERNAL_IMAGE 3UL

#define KSWORD_ARK_RXPF_HOOK_ACTION_NONE     0UL
#define KSWORD_ARK_RXPF_HOOK_ACTION_REDIRECT 1UL
#define KSWORD_ARK_RXPF_HOOK_ACTION_LOG      2UL

#define KSWORD_ARK_RXPF_PAGE_STATE_EMPTY       0UL
#define KSWORD_ARK_RXPF_PAGE_STATE_RX          1UL
#define KSWORD_ARK_RXPF_PAGE_STATE_RW_NX       2UL
#define KSWORD_ARK_RXPF_PAGE_STATE_TERMINATING 3UL
#define KSWORD_ARK_RXPF_PAGE_STATE_ERROR       4UL

#define KSWORD_ARK_RXPF_SUPPORT_INITIALIZED             0x00000001UL
#define KSWORD_ARK_RXPF_SUPPORT_BUILD_MATCH             0x00000002UL
#define KSWORD_ARK_RXPF_SUPPORT_ABI_VERIFIED            0x00000004UL
#define KSWORD_ARK_RXPF_SUPPORT_ALLOCATED_TEST_PAGE     0x00000008UL
#define KSWORD_ARK_RXPF_SUPPORT_SELF_IMAGE_TEST_PAGE    0x00000010UL
#define KSWORD_ARK_RXPF_SUPPORT_IDT_SHADOW              0x00000020UL
#define KSWORD_ARK_RXPF_SUPPORT_EMULATOR                 0x00000040UL
#define KSWORD_ARK_RXPF_SUPPORT_EXTERNAL_IMAGE_COMPILED 0x00000080UL
#define KSWORD_ARK_RXPF_SUPPORT_IDT_INSTALLED            0x00000100UL
#define KSWORD_ARK_RXPF_SUPPORT_ALLOCATED_TEST_PASSED    0x00000200UL

#define KSWORD_ARK_RXPF_BUILD_STATUS_UNINITIALIZED       0UL
#define KSWORD_ARK_RXPF_BUILD_STATUS_SUPPORTED           1UL
#define KSWORD_ARK_RXPF_BUILD_STATUS_OS_BUILD_MISMATCH   2UL
#define KSWORD_ARK_RXPF_BUILD_STATUS_IMAGE_MISMATCH      3UL
#define KSWORD_ARK_RXPF_BUILD_STATUS_RSDS_MISMATCH       4UL
#define KSWORD_ARK_RXPF_BUILD_STATUS_SIGNATURE_MISMATCH  5UL
#define KSWORD_ARK_RXPF_BUILD_STATUS_SECTION_INVALID     6UL
#define KSWORD_ARK_RXPF_BUILD_STATUS_SYMBOL_UNAVAILABLE  7UL

#define KSWORD_ARK_RXPF_DECODE_NONE               0UL
#define KSWORD_ARK_RXPF_DECODE_NOP                1UL
#define KSWORD_ARK_RXPF_DECODE_MOV                2UL
#define KSWORD_ARK_RXPF_DECODE_LEA                3UL
#define KSWORD_ARK_RXPF_DECODE_PUSH               4UL
#define KSWORD_ARK_RXPF_DECODE_POP                5UL
#define KSWORD_ARK_RXPF_DECODE_ADD                6UL
#define KSWORD_ARK_RXPF_DECODE_SUB                7UL
#define KSWORD_ARK_RXPF_DECODE_XOR                8UL
#define KSWORD_ARK_RXPF_DECODE_AND                9UL
#define KSWORD_ARK_RXPF_DECODE_OR                 10UL
#define KSWORD_ARK_RXPF_DECODE_CMP                11UL
#define KSWORD_ARK_RXPF_DECODE_TEST               12UL
#define KSWORD_ARK_RXPF_DECODE_JMP                13UL
#define KSWORD_ARK_RXPF_DECODE_JCC                14UL
#define KSWORD_ARK_RXPF_DECODE_CALL               15UL
#define KSWORD_ARK_RXPF_DECODE_RET                16UL
#define KSWORD_ARK_RXPF_DECODE_HOOK_REDIRECT      17UL

#define KSWORD_ARK_RXPF_EMULATION_NONE                    0UL
#define KSWORD_ARK_RXPF_EMULATION_SUCCESS                 1UL
#define KSWORD_ARK_RXPF_EMULATION_NOT_MANAGED             2UL
#define KSWORD_ARK_RXPF_EMULATION_NOT_EXECUTE_FAULT       3UL
#define KSWORD_ARK_RXPF_EMULATION_USER_FAULT              4UL
#define KSWORD_ARK_RXPF_EMULATION_RECURSIVE               5UL
#define KSWORD_ARK_RXPF_EMULATION_UNSUPPORTED_INSTRUCTION 6UL
#define KSWORD_ARK_RXPF_EMULATION_CROSS_PAGE              7UL
#define KSWORD_ARK_RXPF_EMULATION_INVALID_ADDRESS         8UL
#define KSWORD_ARK_RXPF_EMULATION_STACK_RANGE             9UL
#define KSWORD_ARK_RXPF_EMULATION_TOPOLOGY_CHANGED        10UL
#define KSWORD_ARK_RXPF_EMULATION_INTERNAL_ERROR          11UL
#define KSWORD_ARK_RXPF_EMULATION_REDIRECTED             12UL

typedef struct _KSWORD_ARK_RXPF_REQUEST_HEADER
{
    unsigned long version;
    unsigned long size;
    unsigned long flags;
    unsigned long confirmationToken;
} KSWORD_ARK_RXPF_REQUEST_HEADER;

typedef struct _KSWORD_ARK_RXPF_QUERY_SUPPORT_RESPONSE
{
    unsigned long version;
    unsigned long size;
    unsigned long supportFlags;
    unsigned long buildStatus;
    unsigned long ntBuildNumber;
    unsigned long imageTimeDateStamp;
    unsigned long imageSize;
    unsigned long imageCheckSum;
    unsigned long functionRva;
    unsigned long functionOperationRx;
    unsigned long functionOperationReadOnlyNx;
    unsigned long functionParameterCount;
    unsigned long pdbAge;
    unsigned long processorCount;
    long lastStatus;
    unsigned long reserved;
    unsigned char pdbGuid[16];
    unsigned char signature[64];
    unsigned char signatureMask[64];
} KSWORD_ARK_RXPF_QUERY_SUPPORT_RESPONSE;

typedef struct _KSWORD_ARK_RXPF_REGISTER_PAGE_REQUEST
{
    KSWORD_ARK_RXPF_REQUEST_HEADER header;
    unsigned long targetKind;
    unsigned long reserved;
    unsigned long long targetAddress;
} KSWORD_ARK_RXPF_REGISTER_PAGE_REQUEST;

typedef struct _KSWORD_ARK_RXPF_PAGE_RESPONSE
{
    unsigned long version;
    unsigned long size;
    unsigned long state;
    unsigned long targetKind;
    unsigned long flags;
    unsigned long generation;
    unsigned long referenceCount;
    unsigned long emulationEnabled;
    unsigned long long recordId;
    unsigned long long pageBase;
    unsigned long long writableAlias;
    unsigned long long pfn;
    unsigned long long ownerImageBase;
    unsigned long long faultCount;
    unsigned long long emulatedCount;
    unsigned long long unsupportedCount;
    long lastStatus;
    unsigned long lastFailureReason;
    unsigned long originalProtection;
    unsigned long currentProtection;
    unsigned long writableAliasProtection;
    unsigned long lastWriteOffset;
    unsigned long lastWriteLength;
    unsigned char lastWriteBytes[KSWORD_ARK_RXPF_MAX_WRITE_BYTES];
} KSWORD_ARK_RXPF_PAGE_RESPONSE;

typedef struct _KSWORD_ARK_RXPF_RECORD_REQUEST
{
    KSWORD_ARK_RXPF_REQUEST_HEADER header;
    unsigned long long recordId;
} KSWORD_ARK_RXPF_RECORD_REQUEST;

typedef struct _KSWORD_ARK_RXPF_WRITE_PAGE_REQUEST
{
    KSWORD_ARK_RXPF_REQUEST_HEADER header;
    unsigned long long recordId;
    unsigned long offset;
    unsigned long length;
    unsigned char bytes[KSWORD_ARK_RXPF_MAX_WRITE_BYTES];
} KSWORD_ARK_RXPF_WRITE_PAGE_REQUEST;

typedef struct _KSWORD_ARK_RXPF_SET_EMULATION_REQUEST
{
    KSWORD_ARK_RXPF_REQUEST_HEADER header;
    unsigned long long recordId;
    unsigned long enable;
    unsigned long reserved;
} KSWORD_ARK_RXPF_SET_EMULATION_REQUEST;

typedef struct _KSWORD_ARK_RXPF_STATS_RESPONSE
{
    unsigned long version;
    unsigned long size;
    unsigned long generation;
    unsigned long registeredPages;
    unsigned long enabledPages;
    unsigned long idtInstalled;
    unsigned long processorCount;
    unsigned long activeHandlers;
    unsigned long long totalFaults;
    unsigned long long managedFaults;
    unsigned long long emulatedInstructions;
    unsigned long long chainedFaults;
    unsigned long long recursiveFaults;
    unsigned long long unsupportedInstructions;
    unsigned long long droppedEvents;
    long lastStatus;
    unsigned long reserved;
} KSWORD_ARK_RXPF_STATS_RESPONSE;

typedef struct _KSWORD_ARK_RXPF_EVENT_ROW
{
    unsigned long long sequence;
    unsigned long long timestamp;
    unsigned long long cr2;
    unsigned long long rip;
    unsigned long long errorCode;
    unsigned long long recordId;
    unsigned long long newRip;
    unsigned long processorIndex;
    unsigned long decodedInstruction;
    unsigned long emulationResult;
    long status;
} KSWORD_ARK_RXPF_EVENT_ROW;

typedef struct _KSWORD_ARK_RXPF_DRAIN_EVENTS_REQUEST
{
    KSWORD_ARK_RXPF_REQUEST_HEADER header;
    unsigned long long afterSequence;
    unsigned long maxRows;
    unsigned long reserved;
} KSWORD_ARK_RXPF_DRAIN_EVENTS_REQUEST;

typedef struct _KSWORD_ARK_RXPF_DRAIN_EVENTS_RESPONSE
{
    unsigned long version;
    unsigned long size;
    unsigned long returnedRows;
    unsigned long availableRows;
    unsigned long long newestSequence;
    unsigned long long droppedRows;
    KSWORD_ARK_RXPF_EVENT_ROW rows[KSWORD_ARK_RXPF_MAX_EVENT_ROWS];
} KSWORD_ARK_RXPF_DRAIN_EVENTS_RESPONSE;

typedef struct _KSWORD_ARK_RXPF_SELF_TEST_RESPONSE
{
    unsigned long version;
    unsigned long size;
    unsigned long result;
    unsigned long instructionCount;
    unsigned long long recordId;
    unsigned long long returnedValue;
    unsigned long long expectedValue;
    unsigned long long faultsObserved;
    long lastStatus;
    unsigned long reserved;
} KSWORD_ARK_RXPF_SELF_TEST_RESPONSE;

typedef struct _KSWORD_ARK_RXPF_SET_HOOK_POINT_REQUEST
{
    KSWORD_ARK_RXPF_REQUEST_HEADER header;
    unsigned long long recordId;
    unsigned long long hookAddress;
    unsigned long hookAction;
    unsigned long reserved;
    unsigned long long redirectTarget;
} KSWORD_ARK_RXPF_SET_HOOK_POINT_REQUEST;

typedef struct _KSWORD_ARK_RXPF_HOOK_POINT_RESPONSE
{
    unsigned long version;
    unsigned long size;
    unsigned long long recordId;
    unsigned long long hookAddress;
    unsigned long hookAction;
    unsigned long state;
    unsigned long long redirectTarget;
    unsigned long long redirectCount;
    long lastStatus;
    unsigned long reserved;
} KSWORD_ARK_RXPF_HOOK_POINT_RESPONSE;

/*
 * QueryBackup：读 Record->Backup（Register(CAPTURE_BACKUP) 时拷贝的原页快照）
 * 的任意子段，供 Phase G wizard 在 Unregister(SKIP_RESTORE) 之前把原字节缓存到
 * R3 作恢复用。offset+length 必须落在 [0, PAGE_SIZE)；length 至多
 * KSWORD_ARK_RXPF_MAX_WRITE_BYTES (64)。若 Record 没带 CAPTURE_BACKUP，
 * Backup 为 NULL，返回 STATUS_NOT_FOUND。
 */
typedef struct _KSWORD_ARK_RXPF_QUERY_BACKUP_REQUEST
{
    KSWORD_ARK_RXPF_REQUEST_HEADER header;
    unsigned long long recordId;
    unsigned long offset;
    unsigned long length;
} KSWORD_ARK_RXPF_QUERY_BACKUP_REQUEST;

typedef struct _KSWORD_ARK_RXPF_QUERY_BACKUP_RESPONSE
{
    unsigned long version;
    unsigned long size;
    unsigned long long recordId;
    unsigned long offset;
    unsigned long length;
    unsigned long hasBackup;
    unsigned long reserved;
    long lastStatus;
    unsigned long reserved2;
    unsigned char bytes[KSWORD_ARK_RXPF_MAX_WRITE_BYTES];
} KSWORD_ARK_RXPF_QUERY_BACKUP_RESPONSE;

/*
 * AtomicCommit：在一个 IOCTL 内原子完成
 *   Register(EXTERNAL_IMAGE) → Change(mode=2) → Write(payload) →
 *   Unregister(SKIP_BACKUP_RESTORE)。
 * NX 窗口被压缩到驱动函数内（微秒级），避免 GUI 分步点击在
 * 高频 fetch 页上必蓝的时序竞态。
 * targetKind 必须是 KSWORD_ARK_RXPF_TARGET_EXTERNAL_IMAGE。
 * targetAddress 必须页对齐。offset+length 必须 <= PAGE_SIZE。
 */
typedef struct _KSWORD_ARK_RXPF_ATOMIC_COMMIT_REQUEST
{
    KSWORD_ARK_RXPF_REQUEST_HEADER header;
    unsigned long targetKind;
    unsigned long reserved;
    unsigned long long targetAddress;
    unsigned long offset;
    unsigned long length;
    unsigned char bytes[KSWORD_ARK_RXPF_MAX_WRITE_BYTES];
} KSWORD_ARK_RXPF_ATOMIC_COMMIT_REQUEST;

/*
 * EXEC_PROOF 请求：payload + 期望 RAX。
 * targetKind 仅接受 KSWORD_ARK_RXPF_TARGET_ALLOCATED_TEST
 * （SELF_IMAGE_TEST 的页在 driver .text 自身，被 PatchGuard 盯着，不适合反复改）。
 * 调用方可以写任意字节序列，但如果想通过 verdict，必须保证 payload 走完后
 * RAX == expectedReturnValue。典型 payload: 48 B8 <imm64> C3（12 字节
 * mov rax, imm64; ret），expectedReturnValue == imm64。
 */
typedef struct _KSWORD_ARK_RXPF_EXEC_PROOF_REQUEST
{
    KSWORD_ARK_RXPF_REQUEST_HEADER header;
    unsigned long targetKind;
    unsigned long length;
    unsigned long long expectedReturnValue;
    unsigned char bytes[KSWORD_ARK_RXPF_MAX_WRITE_BYTES];
} KSWORD_ARK_RXPF_EXEC_PROOF_REQUEST;

/*
 * EXEC_PROOF 响应：R0 走完 Register→Change→Write→Install→Execute 后汇报
 * 所有 worker 的 RAX（取 worker 0 作为代表）、faults 计数、匹配 verdict。
 * verdict:
 *   0 = SUCCESS           — returnedValue == expectedReturnValue，且 fault 计数非零
 *   1 = RETURN_MISMATCH   — concurrent test 跑完但 RAX 不符
 *   2 = NO_FAULTS         — RAX 匹配但 fault 计数为 0（emulator 没工作，payload 可能被别的路径执行）
 *   3 = EXEC_FAILED       — concurrent test 抛异常或 worker=0
 *   4 = SETUP_FAILED      — register/change/write/install 任一步失败
 */
typedef struct _KSWORD_ARK_RXPF_EXEC_PROOF_RESPONSE
{
    unsigned long version;
    unsigned long size;
    unsigned long targetKind;
    unsigned long length;
    unsigned long workerCount;
    unsigned long verdict;
    unsigned long long recordId;
    unsigned long long pageBase;
    unsigned long long returnedValue;
    unsigned long long expectedReturnValue;
    unsigned long long faultsObserved;
    long lastStatus;
    long setupStatus;
} KSWORD_ARK_RXPF_EXEC_PROOF_RESPONSE;

#define KSWORD_ARK_RXPF_EXEC_PROOF_VERDICT_SUCCESS         0UL
#define KSWORD_ARK_RXPF_EXEC_PROOF_VERDICT_RETURN_MISMATCH 1UL
#define KSWORD_ARK_RXPF_EXEC_PROOF_VERDICT_NO_FAULTS       2UL
#define KSWORD_ARK_RXPF_EXEC_PROOF_VERDICT_EXEC_FAILED     3UL
#define KSWORD_ARK_RXPF_EXEC_PROOF_VERDICT_SETUP_FAILED    4UL
