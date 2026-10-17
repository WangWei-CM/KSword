#pragma once

#include <cstdint>
#include <limits>
#include <string>
#include <vector>

namespace ks::kernel::audit
{
    // Category：业务身份独立于界面译文，筛选和缓存始终使用此枚举。
    enum class Category { Inline, Iat, Eat, Ssdt, ShadowSsdt, ObjectType, Integrity };
    // Ownership：未知证据不等价于正常，也不直接推断为恶意修改。
    enum class Ownership { Unknown, Expected, Unexpected };

    // ModuleRange：系统模块快照中的真实加载区间；name 只保存规范化基名。
    struct ModuleRange
    {
        std::uint64_t base = 0; // 加载基址；零值不能提供可信归属。
        std::uint64_t size = 0; // 半开区间大小；溢出或零值无效。
        std::string name; // 系统返回的文件基名，不来自服务导出行。
        bool kernelImage = false; // 系统模块目录中的主内核身份。
    };
    // ModuleSnapshot：available 为假时不得使用残留记录作正常判断。
    struct ModuleSnapshot
    {
        bool available = false; // 本轮目录是否完整且可读取。
        long status = 0; // 目录查询原始 NTSTATUS，供调用者诊断。
        std::vector<ModuleRange> modules; // 本轮加载范围值快照。
    };
    // ServiceOwner：只向页面返回经过两次快照核验的归属与判定。
    struct ServiceOwner
    {
        Ownership ownership = Ownership::Unknown; // 默认未知，禁止缺证据时标正常。
        std::string moduleName; // 两份快照中的同一加载映像基名。
    };

    // Contains：用减法比较半开范围，先拒绝可能溢出的模块记录。
    inline bool Contains(const ModuleRange& module, const std::uint64_t address)
    {
        return module.base != 0 && module.size != 0
            && module.size <= (std::numeric_limits<std::uint64_t>::max)() - module.base
            && address >= module.base && address - module.base < module.size;
    }

    // UniqueOwner：重叠或未映射范围没有唯一归属，返回空而不是猜测最近模块。
    inline const ModuleRange* UniqueOwner(const ModuleSnapshot& snapshot, const std::uint64_t address)
    {
        const ModuleRange* owner = nullptr; // 只接纳唯一包含地址的合法记录。
        for (const auto& module : snapshot.modules)
        {
            if (Contains(module, address))
            {
                if (owner != nullptr || module.name.empty())
                {
                    return nullptr;
                }
                owner = &module;
            }
        }
        return owner;
    }

    // ResolveServiceOwner：输入真实服务地址和前后模块目录；输出可核验的地址归属。
    // 地址有效标志缺失、目录变化或无法唯一定位时均保留 Unknown。
    inline ServiceOwner ResolveServiceOwner(const std::uint64_t address, const bool addressValid,
        const bool shadow, const ModuleSnapshot& before, const ModuleSnapshot& after)
    {
        if (!addressValid || address == 0 || !before.available || !after.available)
        {
            return {};
        }
        const auto* first = UniqueOwner(before, address); // 服务枚举前的实际范围。
        const auto* last = UniqueOwner(after, address); // 服务枚举后的实际范围。
        if (first == nullptr || last == nullptr || first->base != last->base
            || first->size != last->size || first->name != last->name
            || first->kernelImage != last->kernelImage)
        {
            return {};
        }
        // Shadow 服务只允许精确宿主基名，不能用 win32k 前缀接纳任意第三方模块。
        const bool expected = shadow
            ? (last->name == "win32k.sys" || last->name == "win32kfull.sys"
                || last->name == "win32kbase.sys" || last->name == "cdd.dll")
            : last->kernelImage;
        return {expected ? Ownership::Expected : Ownership::Unexpected, last->name};
    }
}
