#pragma once

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include "../../shared/debug/CooperativeDebugProtocol.h"
#include <atomic>
#include <string>
#include <windows.h>

namespace ksword::cooperative::runtime::detail
{
    enum class Command
    {
        None,
        Continue,
        Step
    };

    // 所有控制状态由短 SRW 锁保护；VEH 等待时绝不持锁。
    struct Runtime
    {
        SRWLOCK lock = SRWLOCK_INIT;
        // enabled：是否仍接受检查点。
        std::atomic<bool> enabled{false};
        // activeHandlers：正在执行或等待的VEH数量。
        std::atomic<LONG> activeHandlers{0};
        // activeCheckpoints：尚未从检查点返回的调用数量。
        std::atomic<LONG> activeCheckpoints{0};
        // stop：工作线程取消事件；wake：被停业务线程的命令事件。
        HANDLE stop = nullptr;
        // wake：暂停业务线程命令事件。
        HANDLE wake = nullptr;
        // worker：独立命令线程，不允许成为调试目标。
        HANDLE worker = nullptr;
        // pipe：独占本地端点；owner：真实控制进程对象。
        HANDLE pipe = INVALID_HANDLE_VALUE;
        // owner：认证控制者进程句柄。
        HANDLE owner = nullptr;
        // armedThread：在已发出 TF 后追踪线程是否已退出。
        HANDLE armedThread = nullptr;
        // handler：VEH注册句柄。
        PVOID handler = nullptr;
        // trapAddress：组件INT3实际指令地址。
        PVOID trapAddress = nullptr;
        // PID/TID：目标、命令线程、自有 TF 线程和当前暂停线程。
        // pid：目标进程标识。
        DWORD pid = 0;
        // workerTid：不可调试的命令线程标识。
        DWORD workerTid = 0;
        // armedTid：自有TF所属线程标识。
        DWORD armedTid = 0;
        // pausedTid：当前停驻线程标识。
        DWORD pausedTid = 0;
        // leaseMs：控制租期；lastError：异步检查点拒绝原因。
        DWORD leaseMs = DefaultLeaseMs;
        // lastError：异步检查点错误。
        DWORD lastError = 0;
        // creation 与令牌分别保护进程、控制会话身份。
        std::uint64_t creation = 0;
        // tokenLow：随机会话令牌低位。
        std::uint64_t tokenLow = 0;
        // tokenHigh：随机会话令牌高位。
        std::uint64_t tokenHigh = 0;
        // TF 必须属于发出它的同一会话，不能转交新控制者。
        // armedTokenLow：已武装TF的原会话低位。
        std::uint64_t armedTokenLow = 0;
        // armedTokenHigh：已武装TF的原会话高位。
        std::uint64_t armedTokenHigh = 0;
        // 到期时间、停点序号、当前停点、请求序号均独立维护。
        // deadline：当前控制租期截止时间。
        std::uint64_t deadline = 0;
        // stopCounter：单调停点序号。
        std::uint64_t stopCounter = 0;
        // stopId：当前可消费停点。
        std::uint64_t stopId = 0;
        // lastRequestId：上次已认证请求序号。
        std::uint64_t lastRequestId = 0;
        // connected 表示已认证；pauseRequested 只在未来检查点生效。
        bool connected = false;
        // pauseRequested：等待下一检查点的暂停请求。
        bool pauseRequested = false;
        State state = State::Running;
        Command command = Command::None;
        // snapshot：当前暂停的整数现场。
        RegisterSnapshot snapshot{};
    };

    extern Runtime instance;
    std::uint64_t CreationTime(HANDLE process);
    void ReleaseController();
    DWORD WINAPI Worker(void *);
    LONG CALLBACK Handler(EXCEPTION_POINTERS *exception);
    DWORD CreateEndpoint();
}
