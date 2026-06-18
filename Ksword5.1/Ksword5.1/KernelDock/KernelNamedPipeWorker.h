#pragma once

// ============================================================
// KernelNamedPipeWorker.h
// 作用说明：
// 1) 提供 Named Pipe 的 R3 目录枚举 worker；
// 2) 通过 NPFS 文件系统目录读取命名管道条目；
// 3) 不枚举进程句柄，不依赖 R0 IOCTL，不修改 shared/driver 协议。
// ============================================================

#include <QString>

#include <cstdint>
#include <vector>

// KernelNamedPipeEntry：
// - 输入来源：NtQueryDirectoryFile 返回的 NPFS 目录项；
// - 处理逻辑：worker 将文件名、属性、时间戳和来源目录整理为 UI 可直接展示的字段；
// - 返回行为：由 runKernelNamedPipeSnapshotTask 写入 rowsOut，可为空。
struct KernelNamedPipeEntry
{
    QString pipeName;             // pipeName：管道名称，不含 \Device\NamedPipe 前缀。
    QString ntPath;               // ntPath：完整 NT 路径，例如 \Device\NamedPipe\InitShutdown。
    QString sourceDirectory;      // sourceDirectory：本行来自哪个候选目录。
    QString statusText;           // statusText：单行状态，通常为 STATUS_SUCCESS。
    bool querySucceeded = false;  // querySucceeded：该条目是否来自成功的目录查询。
    std::uint32_t attributes = 0; // attributes：FILE_DIRECTORY_INFORMATION.FileAttributes。
    QString attributesText;       // attributesText：属性位拆解文本。
    QString lastWriteTimeText;    // lastWriteTimeText：最后写入时间，可不可用时为 <Unavailable>。
    std::int64_t lastWriteTime = 0; // lastWriteTime：原始 FILETIME 100ns 值。
};

// KernelNamedPipeDirectoryStatus：
// - 输入来源：每个候选目录的一次 NtOpenFile/NtQueryDirectoryFile 尝试；
// - 处理逻辑：记录打开、查询、返回行数和最后 NTSTATUS；
// - 返回行为：用于详情面板展示路径候选和失败原因。
struct KernelNamedPipeDirectoryStatus
{
    QString candidatePath;          // candidatePath：尝试打开的 NT 路径。
    QString statusText;             // statusText：格式化后的 NTSTATUS 或诊断文本。
    bool openSucceeded = false;     // openSucceeded：NtOpenFile 是否成功。
    bool querySucceeded = false;    // querySucceeded：NtQueryDirectoryFile 是否完整走到 STATUS_NO_MORE_FILES。
    std::uint32_t lastStatus = 0;   // lastStatus：最后一次 NTSTATUS 的无符号视图。
    std::size_t returnedRows = 0;   // returnedRows：该候选目录返回的条目数。
};

// KernelNamedPipeSnapshot：
// - 输入来源：runKernelNamedPipeSnapshotTask 汇总所有候选路径；
// - 处理逻辑：聚合去重后的管道行和每个候选路径状态；
// - 返回行为：UI 根据 taskSucceeded/anyQuerySucceeded 判断状态颜色和提示。
struct KernelNamedPipeSnapshot
{
    std::vector<KernelNamedPipeEntry> rows;                  // rows：去重后的命名管道列表。
    std::vector<KernelNamedPipeDirectoryStatus> directories; // directories：候选路径状态列表。
    QString summaryText;                                     // summaryText：适合状态栏展示的摘要。
    QString errorText;                                       // errorText：致命错误，例如 ntdll 入口缺失。
    bool taskSucceeded = false;                              // taskSucceeded：worker 是否完成任务。
    bool anyQuerySucceeded = false;                          // anyQuerySucceeded：是否至少一个候选目录枚举成功。
    std::uint64_t elapsedMs = 0;                              // elapsedMs：后台耗时。
};

// runKernelNamedPipeSnapshotTask：
// - 输入：无显式输入，worker 内部使用固定候选路径 \Device\NamedPipe 及等价路径；
// - 处理逻辑：动态解析 NtOpenFile/NtQueryDirectoryFile，按 NPFS 文件目录枚举命名管道；
// - 返回：KernelNamedPipeSnapshot，包含 rows、候选状态、耗时和错误文本。
KernelNamedPipeSnapshot runKernelNamedPipeSnapshotTask();
