#pragma once

// ============================================================
// KsecDdExecutor.h
// 作用：
// 1) 在 ntoskrnl 中定位可用于 KSecDD 回调的写 gadget（mov [rcx], edx; ret）；
// 2) 通过 LSASS 注入路径复用其已有的 KSecDD 连接执行内核任意地址写；
// 3) 通过 Server Silo 路径建立独立 KSecDD 连接执行内核任意地址写。
//
// KSecDD（ksecdd.sys）是 Windows 内核安全支持提供程序接口驱动。
// 其 IOCTL_KSEC_IPC_SET_FUNCTION_RETURN (0x0039006F) 允许连接进程
// 以内核模式执行指定地址的函数，寄存器布局为：
//   RAX = Function（要执行的内核地址）
//   RCX = Argument（第一参数）
//   EDX = Value（第二参数的低 32 位）
// 配合 ntoskrnl 中的 `mov dword ptr [rcx], edx; ret` gadget，
// 可将任意 32 位值写入任意内核地址——用于把 g_CiOptions 改成 0。
//
// 安全边界与风险：
// - g_CiOptions 属于 PatchGuard 巡检范围，长时间保持修改会蓝屏。
// - Server Silo 路径每次启动周期最多安全调用 4 次，第 5 次触发 Bug Check。
// - LSASS 路径需要注入远程线程，可能被安全软件拦截。
// ============================================================

#include <QString>
#include <QStringList>

#include <cstdint>

namespace ks::misc::disable_dse
{
    // GadgetLocation：
    // - 作用：在 ntoskrnl 中定位写 gadget 的结果。
    //   写 gadget 是 `mov dword ptr [rcx], edx; ret`（字节 89 11 C3），
    //   KSecDD 会以 RAX=gadget、RCX=目标地址、EDX=目标值 调用它。
    struct GadgetLocation
    {
        bool ok = false;                   // ok：是否成功定位到 gadget。
        std::uint64_t kernelAddress = 0;   // kernelAddress：gadget 在内核中的虚拟地址。
        std::uint64_t ntoskrnlBase = 0;    // ntoskrnlBase：ntoskrnl 的内核加载基址。
        std::uint32_t rva = 0;             // rva：gadget 相对 ntoskrnl 基址的偏移。
        QStringList traceLines;            // traceLines：定位每一步的诊断信息。
        QString failureText;               // failureText：ok 为 false 时的失败原因。
    };

    // ExecuteResult：
    // - 作用：一次 KSecDD 执行操作的结果，包括通道建立与 IOCTL 发送。
    struct ExecuteResult
    {
        bool ok = false;                   // ok：IOCTL 发送成功且无明显错误。
        long ioctlStatus = 0;              // ioctlStatus：NtDeviceIoControlFile 返回的 NTSTATUS。
        QStringList traceLines;            // traceLines：执行过程诊断信息。
        QString failureText;               // failureText：ok 为 false 时的失败原因。
    };

    // locateWriteGadget：
    // - 作用：在本机 ntoskrnl 的可执行节中搜索 `mov [rcx], edx; ret` 字节序列，
    //   并用内核模块基址换算成内核虚拟地址；
    // - 返回：GadgetLocation；全过程只读，不访问驱动，也不打开 KSecDD。
    GadgetLocation locateWriteGadget();

    // executeViaLsass：
    // - 输入 gadgetAddress：已定位的写 gadget 内核地址；
    //        targetAddress：要写入的内核地址（g_CiOptions）；
    //        value：要写入的 32 位值；
    // - 作用：找到 LSASS 进程、枚举其 KSecDD 句柄、注入 shellcode 完成一次 IOCTL 调用；
    // - 前提：LSASS 不是 PPL、进程已提升权限、x64 架构；
    // - 返回：ExecuteResult；ok 为 true 只表示 IOCTL 已发送，调用方仍需回读验证。
    ExecuteResult executeViaLsass(
        std::uint64_t gadgetAddress,
        std::uint64_t targetAddress,
        std::uint32_t value);

    // executeViaSilo：
    // - 输入 gadgetAddress：已定位的写 gadget 内核地址；
    //        targetAddress：要写入的内核地址（g_CiOptions）；
    //        value：要写入的 32 位值；
    // - 作用：取得 SeTcbPrivilege → 创建 Server Silo → 在 Silo 子进程中建立独立
    //   KSecDD 连接并发送 IOCTL；
    // - 前提：Schedule 服务在线、Server Silo API 齐全、进程已提升权限、x64 架构；
    // - 硬性限制：每次启动周期最多安全调用 4 次，第 5 次会触发内核 Bug Check；
    // - 返回：ExecuteResult；ok 为 true 只表示 IOCTL 已发送，调用方仍需回读验证。
    ExecuteResult executeViaSilo(
        std::uint64_t gadgetAddress,
        std::uint64_t targetAddress,
        std::uint32_t value);
}
