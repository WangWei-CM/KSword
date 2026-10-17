#pragma once

#include <cstdint>

namespace ksword::cooperative::runtime
{
    // 目标显式启用、主动调用检查点；Start 不暂停其它线程、不注入进程。
    struct Config
    {
        // leaseMs：租期毫秒数。
        std::uint32_t leaseMs = 10000;
    };

    // 返回 Win32 错误码。Stop 必须在调用方所有 Checkpoint 线程退出后、卸载前调用。
    std::uint32_t Start(const Config &config = {});
    // 由业务线程主动到达；没有暂停请求时立即返回。
    void Checkpoint();
    // 撤销接入并释放所有暂停，已发出的 TF 由安全 handler 收尾。
    void Stop();
}
