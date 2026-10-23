#pragma once

/*
 * KSword 共享：调试器随机命名启动器 payload。
 * x64dbg 与 Cheat Engine 两个 ExecutablePlugin 共用。
 *
 * 目的：绕开按进程名/路径匹配的黑名单。做法是将 payload 整树以硬链接
 * 克隆到一个每次会话唯一的目录，并把主 exe 以随机名硬链接出去，再
 * 从该路径启动。硬链接不占用额外盘空间（NTFS 同卷），跨卷降级到普
 * 通复制。清理走 SHFileOperationW 递归删除。
 */

#include <string>
#include <windows.h>

namespace ksword::randomized_payload {

enum class NamingMode : unsigned long {
    // 保留原主 exe 名字作主干，追加 8 位 hex 后缀。
    // 例：x64dbg-A3F70B21.exe、cheatengine-x86_64-D4E13C09.exe。
    // 对 AV/EDR 的身份保留更稳；默认模式。
    SuffixedPreserveStem = 0UL,
    // 完全随机 GUID 文件名（去花括号，纯 32 位 hex）。
    // 隐蔽性最强，但失去签名/属性可识别性。
    FullyRandomized = 1UL,
};

struct PrepareResult {
    bool success = false;
    // 克隆后的 payload 根目录（新路径），退出清理用。
    std::wstring clonedDirectory;
    // 克隆目录下主 exe 的完整路径（已随机命名）。
    std::wstring clonedExecutable;
    // 失败时的 Win32 错误码（success==true 时为 ERROR_SUCCESS）。
    DWORD lastError = ERROR_SUCCESS;
    // 失败时的最后一次操作描述（可读 ASCII），success==true 时为空。
    std::string failureDescription;
    // Held exclusively from directory creation until CreateProcess has either
    // succeeded or failed. Call finishRandomizedPayloadLaunch on every path.
    HANDLE launchLease = INVALID_HANDLE_VALUE;
    HANDLE launchMutex = nullptr;
};

// 克隆 sourcePayloadRoot 整个目录树到 %LOCALAPPDATA%\KSword\sessions\<guid>\，
// 其中 primaryExeName（例如 L"x64dbg.exe"）会被重命名。sourcePayloadRoot
// 允许尾部有或没有反斜杠；primaryExeName 是相对 sourcePayloadRoot 的文件名
// （不含子目录分隔，否则返回失败）。
PrepareResult prepareRandomizedPayload(
    const std::wstring& sourcePayloadRoot,
    const std::wstring& primaryExeName,
    NamingMode mode);

// Records the actual child PID and creation time before releasing the launch
// lease. Pass nullptr when CreateProcess was not called or failed. This is
// best-effort bookkeeping and must not turn a successful launch into a failure.
void finishRandomizedPayloadLaunch(PrepareResult& prepared, HANDLE launchedProcess);

// 删除克隆目录及其下所有内容。clonedDirectory 必须是
// prepareRandomizedPayload 返回的目录，否则直接拒绝（防误删）。
void cleanupRandomizedPayload(const std::wstring& clonedDirectory);

// 清理过期的旧会话目录。prepareRandomizedPayload 内部会调。启动租约、
// 精确进程身份及实际映像路径共同保护尚在使用的会话；无 waiter 的 launch
// 命令场景（CE 进程脱离 launcher 独立运行）由持久身份记录兜底。
void sweepStaleSessions();

} // namespace ksword::randomized_payload
