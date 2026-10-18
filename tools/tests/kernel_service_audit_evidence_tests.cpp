#include "../../Ksword5.1/Ksword5.1/KernelDock/KernelServiceAuditEvidence.h"
#include <cstdlib>
#include <iostream>

namespace
{
    int failures = 0; // 本进程独立的断言失败数。
    int assertions = 0; // 输出真实执行的检查数量。
    void Check(const bool condition, const char* name)
    {
        ++assertions;
        if (!condition)
        {
            ++failures;
            std::cerr << name << '\n';
        }
    }
}

// 使用生产头中的判定：覆盖重定向、未知和模块生命周期变化，不访问真实内核。
int main()
{
    namespace audit = ks::kernel::audit;
    const audit::ModuleSnapshot modules{true, 0,
        {{0x1000, 0x1000, "ntoskrnl.exe", true},
         {0x4000, 0x1000, "win32kfull.sys", false},
         {0x7000, 0x1000, "thirdparty.sys", false}}};
    const auto classify = [&modules](const std::uint64_t address, const bool shadow = false)
    {
        return audit::ResolveServiceOwner(address, true, shadow, modules, modules).ownership;
    };
    Check(classify(0x1100) == audit::Ownership::Expected, "SSDT kernel range");
    Check(classify(0x7100) == audit::Ownership::Unexpected, "SSDT redirect outside kernel");
    Check(classify(0x4100, true) == audit::Ownership::Expected, "Shadow exact host range");
    Check(classify(0x7100, true) == audit::Ownership::Unexpected, "Shadow redirect");
    Check(classify(0x2100) == audit::Ownership::Unknown, "unmapped gap is unknown");
    Check(classify(0) == audit::Ownership::Unknown, "zero address is unknown");
    Check(classify(0x2000) == audit::Ownership::Unknown, "range end is exclusive");
    Check(audit::ResolveServiceOwner(0x1100, false, false, modules, modules).ownership
        == audit::Ownership::Unknown, "invalid service-address evidence flag");
    // 目录失败和地址掩码后零基址不提供任何正常结论。
    auto changed = modules;
    changed.available = false;
    Check(audit::ResolveServiceOwner(0x1100, true, false, modules, changed).ownership
        == audit::Ownership::Unknown, "module query failed");
    changed = modules;
    changed.modules[0].base = 0;
    Check(audit::ResolveServiceOwner(0x1100, true, false, modules, changed).ownership
        == audit::Ownership::Unknown, "redacted module range");
    changed = modules;
    changed.modules[0].size = 0x1100;
    Check(audit::ResolveServiceOwner(0x1100, true, false, modules, changed).ownership
        == audit::Ownership::Unknown, "module identity changed during query");
    // 同地址有重叠归属、整数溢出和伪造前缀均不能绕过真实范围判断。
    changed = modules;
    changed.modules.push_back({0x1000, 0x1000, "overlap.sys", false});
    Check(audit::ResolveServiceOwner(0x1100, true, false, changed, changed).ownership
        == audit::Ownership::Unknown, "ambiguous overlapping ranges");
    changed = modules;
    changed.modules[0] = {(std::numeric_limits<std::uint64_t>::max)() - 2, 10, "ntoskrnl.exe", true};
    Check(audit::ResolveServiceOwner((std::numeric_limits<std::uint64_t>::max)() - 1,
        true, false, changed, changed).ownership == audit::Ownership::Unknown, "range overflow");
    changed = modules;
    changed.modules[1].name = "win32k-malicious.sys";
    Check(audit::ResolveServiceOwner(0x4100, true, true, changed, changed).ownership
        == audit::Ownership::Unexpected, "Shadow host prefix spoof");
    // 显示文本从不进入这个接口，稳定类别值不随任何语言投影变化。
    Check(static_cast<int>(audit::Category::Integrity) != static_cast<int>(audit::Category::Ssdt),
        "distinct stable category identifiers");
    std::cout << "KERNEL_SERVICE_AUDIT_CHECKS=" << assertions
        << " FAILURES=" << failures << '\n';
    return failures == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
