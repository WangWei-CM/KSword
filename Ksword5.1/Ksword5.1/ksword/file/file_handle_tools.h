#pragma once

// ============================================================
// ksword/file/file_handle_tools.h
// 命名空间：ks::file
// 作用：
// - 提供 FileDock 与句柄 Dock 共享的路径规范化、系统句柄枚举、
//   NtQueryObject/NtQuerySystemInformation 包装和文件占用扫描能力；
// - 本层只暴露 std 文本类型与 Win32 基础类型，不依赖 Qt 控件或界面字符串类型；
// - UI 层负责把结果转换为表格、颜色、按钮状态和进度条显示。
// ============================================================

#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <unordered_map>
#include <vector>

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#include <winternl.h>

namespace ks::file
{
    // ProgressCallback 作用：
    // - 后端扫描过程中上报纯文本阶段与百分比；
    // - UI 可转接到 kPro/状态栏，命令行调用者也可忽略。
    using ProgressCallback = std::function<void(const std::string& stepText, float progressValue)>;

    // HandleEnumMode 作用：
    // - 描述句柄快照构建使用的枚举路径；
    // - 与句柄 Dock 的 UI 下拉框保持语义一致，但不依赖 UI 枚举。
    enum class HandleEnumMode : int
    {
        UserSnapshot = 0,      // UserSnapshot：只采集 R3 系统句柄快照。
        DuplicateHandle = 1,   // DuplicateHandle：R3 快照后复制句柄并解析计数/对象名。
        KernelHandleTable = 2  // KernelHandleTable：在 R3 快照基础上叠加 R0 HandleTable 差异。
    };

    // HandleDiffStatus 作用：
    // - 描述 R3/R0 双源枚举中的差异状态；
    // - UI 层仅负责将其渲染为本地化文本。
    enum class HandleDiffStatus : int
    {
        NotCompared = 0,
        UserOnly,
        KernelOnly,
        Both
    };

    // NtApiSet 作用：
    // - 保存动态解析到的 ntdll 入口；
    // - 调用方可复用同一份函数指针，避免重复 GetProcAddress。
    struct NtApiSet
    {
        using NtQuerySystemInformationFn = NTSTATUS(NTAPI*)(ULONG, PVOID, ULONG, PULONG);
        using NtQueryObjectFn = NTSTATUS(NTAPI*)(HANDLE, ULONG, PVOID, ULONG, PULONG);

        HMODULE ntdllModule = nullptr;                               // ntdllModule：ntdll 模块句柄，进程生命周期内有效。
        NtQuerySystemInformationFn querySystemInformation = nullptr; // querySystemInformation：NtQuerySystemInformation 地址。
        NtQueryObjectFn queryObject = nullptr;                       // queryObject：NtQueryObject 地址。

        // ready 作用：判断本次 Nt API 动态加载是否满足句柄功能最低要求。
        bool ready() const;
    };

    // RawSystemHandle 作用：
    // - 表示 NtQuerySystemInformation(SystemExtendedHandleInformation) 的安全解码结果；
    // - 只保留 UI 与扫描后端共用字段。
    struct RawSystemHandle
    {
        std::uint32_t processId = 0;
        std::uint64_t handleValue = 0;
        std::uint16_t typeIndex = 0;
        std::uint64_t objectAddress = 0;
        std::uint32_t grantedAccess = 0;
        std::uint32_t attributes = 0;
    };

    // ObjectBasicInfo 作用：
    // - 封装 ObjectBasicInformation 中句柄/指针计数；
    // - 屏蔽 SDK 头文件里未公开字段布局差异。
    struct ObjectBasicInfo
    {
        std::uint32_t handleCount = 0;
        std::uint32_t pointerCount = 0;
    };

    // ObjectNameQueryResult 作用：
    // - 表示一次对象名解析的完整状态；
    // - 区分“查到了空名称”和“查询失败”。
    struct ObjectNameQueryResult
    {
        bool available = false;
        bool failed = false;
        bool usedFallback = false;
        std::wstring objectName;
    };

    // TargetPathPattern 作用：
    // - 表示文件占用扫描中的一个比较规则；
    // - displayPath 保留用户视角路径，normalizedPath 用于大小写无关匹配。
    struct TargetPathPattern
    {
        std::wstring displayPath;
        std::wstring normalizedPath;
        bool directoryMode = false;
    };

    // HandleSnapshotOptions 作用：
    // - 为句柄 Dock 后台刷新打包输入条件；
    // - 字段均为 std 类型，避免后台层读取 Qt 控件。
    struct HandleSnapshotOptions
    {
        bool hasPidFilter = false;
        std::uint32_t pidFilter = 0;
        std::wstring typeFilterText;
        bool resolveObjectName = true;
        int nameResolveBudget = 300;
        HandleEnumMode enumMode = HandleEnumMode::DuplicateHandle;
        std::unordered_map<std::uint16_t, std::string> typeNameCacheByIndex;
        std::unordered_map<std::uint16_t, std::string> typeNameMapFromObjectTab;
    };

    // HandleSnapshotRow 作用：
    // - 句柄 Dock 可直接渲染的一行后端数据；
    // - 不含颜色、表格项、图标等 UI 状态。
    struct HandleSnapshotRow
    {
        std::uint32_t processId = 0;
        std::wstring processName;
        std::uint64_t handleValue = 0;
        std::uint16_t typeIndex = 0;
        std::wstring typeName;
        std::wstring objectName;
        std::uint64_t objectAddress = 0;
        std::uint32_t grantedAccess = 0;
        std::uint32_t attributes = 0;
        std::uint32_t handleCount = 0;
        std::uint32_t pointerCount = 0;
        bool basicInfoAvailable = false;
        bool objectNameAvailable = false;
        bool objectNameFailed = false;
        bool objectNameFromFallback = false;
        HandleEnumMode sourceMode = HandleEnumMode::UserSnapshot;
        HandleDiffStatus diffStatus = HandleDiffStatus::NotCompared;
        std::uint32_t decodeStatus = 0;
        std::uint32_t r0FieldFlags = 0;
        std::uint64_t r0DynDataCapabilityMask = 0;
        std::uint32_t epObjectTableOffset = 0xFFFFFFFFUL;
        std::uint32_t htHandleContentionEventOffset = 0xFFFFFFFFUL;
        std::uint32_t obDecodeShift = 0xFFFFFFFFUL;
        std::uint32_t obAttributesShift = 0xFFFFFFFFUL;
        std::uint32_t otNameOffset = 0xFFFFFFFFUL;
        std::uint32_t otIndexOffset = 0xFFFFFFFFUL;
    };

    // HandleSnapshotResult 作用：
    // - 聚合句柄 Dock 一轮后台刷新结果；
    // - 诊断文本保持为宽字符串，UI 可直接转换为界面字符串。
    struct HandleSnapshotResult
    {
        std::vector<HandleSnapshotRow> rows;
        std::vector<std::wstring> availableTypeList;
        std::unordered_map<std::uint16_t, std::string> updatedTypeNameCacheByIndex;
        std::size_t totalHandleCount = 0;
        std::size_t visibleHandleCount = 0;
        std::size_t basicInfoResolvedCount = 0;
        std::size_t resolvedNameCount = 0;
        std::size_t fallbackNameCount = 0;
        std::size_t objectTypeMappedCount = 0;
        std::size_t kernelHandleCount = 0;
        std::size_t userOnlyCount = 0;
        std::size_t kernelOnlyCount = 0;
        std::size_t bothCount = 0;
        std::uint64_t elapsedMs = 0;
        std::wstring diagnosticText;
    };

    // HandleUsageEntry 作用：
    // - 表示文件/目录占用扫描命中的一个来源；
    // - 可来自真实文件句柄、进程映像或模块快照。
    struct HandleUsageEntry
    {
        std::uint32_t processId = 0;
        std::wstring processName;
        std::wstring processImagePath;
        std::uint64_t handleValue = 0;
        std::uint16_t typeIndex = 0;
        std::wstring typeName;
        std::wstring objectName;
        std::uint32_t grantedAccess = 0;
        std::uint32_t attributes = 0;
        std::wstring matchedTargetPath;
        bool matchedByDirectoryRule = false;
        std::wstring matchRuleText;
        std::wstring enumerationSource;
    };

    // HandleUsageScanOptions 作用：
    // - 控制文件占用扫描策略；
    // - 默认先尝试 R0 HandleTable，失败或无命中时回退 R3 DuplicateHandle。
    struct HandleUsageScanOptions
    {
        bool tryKernelHandleTable = true;
        ProgressCallback progressCallback;
    };

    // HandleUsageScanResult 作用：
    // - 聚合 FileDock 文件占用扫描的结果与统计；
    // - UI 层只负责展示 entries 和 diagnosticText。
    struct HandleUsageScanResult
    {
        std::vector<HandleUsageEntry> entries;
        std::size_t totalHandleCount = 0;
        std::size_t fileLikeHandleCount = 0;
        std::size_t matchedHandleCount = 0;
        std::size_t processImageMatchCount = 0;
        std::size_t loadedModuleMatchCount = 0;
        std::size_t kernelHandleMatchCount = 0;
        std::uint64_t elapsedMs = 0;
        std::wstring diagnosticText;
    };

    // NormalizeNativePath 作用：统一 Windows 路径分隔符和长路径前缀。
    std::wstring NormalizeNativePath(const std::wstring& pathText);

    // NormalizePathForCompare 作用：生成大小写无关、无尾部分隔符的比较键。
    std::wstring NormalizePathForCompare(const std::wstring& pathText);

    // BuildNtPathEquivalent 作用：把 DOS/UNC 路径转换为 NT 设备路径。
    bool BuildNtPathEquivalent(const std::wstring& absolutePath, std::wstring& ntPathOut);

    // BuildTargetPathPatterns 作用：为一组用户路径生成 DOS/NT 双视角匹配规则。
    std::vector<TargetPathPattern> BuildTargetPathPatterns(const std::vector<std::wstring>& absolutePaths);

    // MatchTargetPath 作用：按精确文件或目录前缀规则匹配候选路径。
    bool MatchTargetPath(
        const std::wstring& normalizedCandidatePath,
        const std::vector<TargetPathPattern>& patternList,
        std::wstring& matchedTargetPathOut,
        bool& matchedByDirectoryRuleOut);

    // QueryNtApis 作用：动态加载 NtQuerySystemInformation 和 NtQueryObject。
    NtApiSet QueryNtApis();

    // QuerySystemHandles 作用：安全读取扩展系统句柄快照。
    bool QuerySystemHandles(
        const NtApiSet& apiSet,
        std::vector<RawSystemHandle>& recordsOut,
        std::wstring& diagnosticTextOut);

    // QueryNtObjectText 作用：读取 NtQueryObject 返回的 UNICODE_STRING 文本。
    bool QueryNtObjectText(
        const NtApiSet& apiSet,
        HANDLE objectHandle,
        ULONG informationClass,
        std::wstring& textOut);

    // QueryObjectBasicInfo 作用：读取对象 HandleCount/PointerCount。
    bool QueryObjectBasicInfo(
        const NtApiSet& apiSet,
        HANDLE objectHandle,
        ObjectBasicInfo& basicInfoOut);

    // ResolveObjectNameText 作用：先走 NtQueryObject，再按对象类型走回退路径。
    ObjectNameQueryResult ResolveObjectNameText(
        const NtApiSet& apiSet,
        HANDLE objectHandle,
        const std::wstring& typeNameText);

    // DuplicateRemoteHandleToLocal 作用：把目标进程句柄复制到当前进程；调用方负责 CloseHandle。
    bool DuplicateRemoteHandleToLocal(
        HANDLE sourceProcessHandle,
        std::uint64_t handleValue,
        HANDLE& localHandleOut);

    // QueryFinalDosPathByHandle 作用：用 GetFinalPathNameByHandleW 读取标准 DOS 路径。
    bool QueryFinalDosPathByHandle(HANDLE objectHandle, std::wstring& pathOut);

    // CloseRemoteHandle 作用：通过 DuplicateHandle(DUPLICATE_CLOSE_SOURCE) 关闭远程句柄。
    bool CloseRemoteHandle(std::uint32_t processId, std::uint64_t handleValue, std::string& detailTextOut);

    // BuildHandleSnapshot 作用：构建句柄 Dock 使用的完整后端快照。
    HandleSnapshotResult BuildHandleSnapshot(const HandleSnapshotOptions& options);

    // ScanHandleUsageByPaths 作用：按文件/目录路径扫描占用来源。
    HandleUsageScanResult ScanHandleUsageByPaths(
        const std::vector<std::wstring>& absolutePaths,
        const HandleUsageScanOptions& options = HandleUsageScanOptions{});
}
