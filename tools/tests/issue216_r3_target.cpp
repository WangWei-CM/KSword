#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>

#include <cstdint>
#include <cstdio>
#include <cwchar>

// main：仅分配本进程的一段测试内存；无输入参数，以退出码和标准输出报告状态。
// 后续验收可打开输出的停止事件并 SetEvent；否则最多等待三十分钟便释放所有资源退出。
int main()
{
    constexpr SIZE_T kAllocationBytes = 64U * 1024U; // 唯一受控测试区域，固定为 64 KiB。
    constexpr DWORD kWaitMilliseconds = 30U * 60U * 1000U; // 等待上限，不依赖外部清理。
    const DWORD targetPid = ::GetCurrentProcessId(); // 输出自身 PID，绝不访问其它进程。
    auto* const bytes = static_cast<std::uint8_t*>(::VirtualAlloc(
        nullptr, kAllocationBytes, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE)); // 自有独立分配。
    if (bytes == nullptr)
    {
        std::fprintf(stderr, "ISSUE216_TARGET_ERROR=VirtualAlloc:%lu\n", ::GetLastError());
        return 1;
    }

    // 每一字节都由区域内偏移确定：期望值为 (offset * 37 + 11) & 0xFF。
    // 模式覆盖全部区域，没有用户数据、外部文件、敏感数据或运行中内容变化。
    for (SIZE_T offset = 0; offset < kAllocationBytes; ++offset) // 当前区域内字节偏移。
    {
        bytes[offset] = static_cast<std::uint8_t>((offset * 37U + 11U) & 0xFFU);
    }
    MEMORY_BASIC_INFORMATION region{}; // 自身地址查询的真实区域信息。
    const SIZE_T queryBytes = ::VirtualQueryEx(::GetCurrentProcess(), bytes, &region, sizeof(region));
    if (queryBytes != sizeof(region) || region.BaseAddress != bytes
        || region.RegionSize != kAllocationBytes || region.State != MEM_COMMIT
        || region.Protect != PAGE_READWRITE)
    {
        std::fprintf(stderr, "ISSUE216_TARGET_ERROR=UnexpectedRegion:%lu\n", ::GetLastError());
        ::VirtualFree(bytes, 0, MEM_RELEASE);
        return 2;
    }

    // 停止事件是当前会话的本地临时对象；程序关闭最后一个句柄后自动销毁。
    wchar_t stopName[96]{}; // 独立停止事件名称，仅含固定前缀和自身 PID。
    swprintf_s(stopName, sizeof(stopName) / sizeof(stopName[0]),
        L"Local\\KSwordIssue216R3TargetStop-%lu", targetPid);
    const HANDLE stopEvent = ::CreateEventW(nullptr, TRUE, FALSE, stopName); // 手动复位停止事件。
    if (stopEvent == nullptr)
    {
        std::fprintf(stderr, "ISSUE216_TARGET_ERROR=CreateEvent:%lu\n", ::GetLastError());
        ::VirtualFree(bytes, 0, MEM_RELEASE);
        return 3;
    }

    // READY 只在内容填充、区域核验和停止事件创建全部成功后输出，并立即刷新。
    std::printf("ISSUE216_TARGET_PID=%lu\n", targetPid);
    std::printf("ISSUE216_TARGET_BASE=0x%016llX\n",
        static_cast<unsigned long long>(reinterpret_cast<std::uintptr_t>(bytes)));
    std::printf("ISSUE216_TARGET_REGION_BYTES=%llu\n", static_cast<unsigned long long>(region.RegionSize));
    std::printf("ISSUE216_TARGET_PATTERN=(offset*37+11)&0xFF\n");
    std::printf("ISSUE216_TARGET_STOP_EVENT=Local\\KSwordIssue216R3TargetStop-%lu\n", targetPid);
    std::printf("ISSUE216_TARGET_TIMEOUT_MS=%lu\n", kWaitMilliseconds);
    std::printf("ISSUE216_TARGET_READY=1\n");
    std::fflush(stdout);

    // 只等待事件或有限时间；不访问 GUI、网络、驱动及其它进程内存。
    const DWORD waitResult = ::WaitForSingleObject(stopEvent, kWaitMilliseconds); // 停止或超时结果。
    const DWORD waitError = waitResult == WAIT_FAILED ? ::GetLastError() : ERROR_SUCCESS; // 保存错误码。
    ::CloseHandle(stopEvent);
    const BOOL released = ::VirtualFree(bytes, 0, MEM_RELEASE); // 所有退出分支均释放自有内存。
    const DWORD releaseError = released ? ERROR_SUCCESS : ::GetLastError(); // 保存释放结果诊断。
    std::printf("ISSUE216_TARGET_EXIT=%s\n",
        waitResult == WAIT_OBJECT_0 ? "STOPPED" : (waitResult == WAIT_TIMEOUT ? "TIMEOUT" : "FAILED"));
    std::fflush(stdout);
    if (waitResult == WAIT_FAILED || !released)
    {
        std::fprintf(stderr, "ISSUE216_TARGET_ERROR=CleanupOrWait:%lu\n",
            waitResult == WAIT_FAILED ? waitError : releaseError);
        return 4;
    }
    return 0;
}
