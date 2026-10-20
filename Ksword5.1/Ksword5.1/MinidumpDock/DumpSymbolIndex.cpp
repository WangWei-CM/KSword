// ============================================================
// DumpSymbolIndex.cpp
// 作用：
// - 实现 DumpSymbolIndex.h 声明的模块区间索引与地址性质判定；
// - 区间表按基址升序排序后用 upper_bound 定位候选，再校验是否落在
//   [base, end) 内，重叠区间（已卸载模块地址被复用）优先返回已加载模块；
// - 哨兵值表覆盖 MSVC 调试运行时、Windows 堆调试、驱动验证器与
//   内核池的常见填充，命中即视为“未初始化 / 释放后使用”的强信号。
// ============================================================

#include "DumpSymbolIndex.h"

#include <algorithm>

namespace ks::minidump
{
    namespace
    {
        // kUserSpaceLimit64：x64 用户态地址上界（规范地址低半区）。
        constexpr std::uint64_t kUserSpaceLimit64 = 0x0000800000000000ull;
        // kKernelSpaceStart64：x64 内核地址下界（规范地址高半区）。
        constexpr std::uint64_t kKernelSpaceStart64 = 0xFFFF800000000000ull;
        // kKernelSpaceStart32：x86 内核地址下界（默认 2GB 分界）。
        constexpr std::uint64_t kKernelSpaceStart32 = 0x0000000080000000ull;
        // kNullPageLimit：CPU 保留的 NULL 页范围；命中即空指针解引用。
        constexpr std::uint64_t kNullPageLimit = 0x10000ull;

        // PoisonPair：一个哨兵值与它的中文含义。
        struct PoisonPair
        {
            std::uint32_t value; // value：32 位模式；64 位按高低位重复匹配。
            const char* meaning; // meaning：中文含义（UTF-8）。
        };

        // kPoisonValues：MSVC 调试运行时与 Windows 调试堆的官方填充值。
        // 命中说明该值不是有效指针，而是分配器/编译器写进去的标记，
        // 据此可直接把问题定性到“未初始化”或“释放后使用”。
        // 只收录有明确出处的填充值——凭印象往表里加值会让“疑似哨兵”
        // 出现在其实合法的地址上，属于有害误报。
        constexpr PoisonPair kPoisonValues[] = {
            { 0xCCCCCCCCu, "未初始化的栈内存（MSVC /RTC 调试填充）" },
            { 0xCDCDCDCDu, "未初始化的堆内存（MSVC 调试堆新分配填充）" },
            { 0xDDDDDDDDu, "已释放的堆内存（MSVC 调试堆释放填充），典型释放后使用" },
            { 0xFDFDFDFDu, "堆块守卫区（MSVC 调试堆 no-man's-land），典型越界读写" },
            { 0xABABABABu, "HeapAlloc 分配块尾部守卫区（Windows 调试堆）" },
            { 0xBAADF00Du, "LocalAlloc/HeapAlloc 未初始化内存（Windows 调试堆填充）" },
            { 0xFEEEFEEEu, "HeapFree 已释放内存（Windows 调试堆填充），典型释放后使用" },
            { 0xDEADBEEFu, "人为哨兵值（常见于驱动/内核代码的显式标记）" },
        };
    }

    QString BaseModuleName(const QString& path)
    {
        if (path.isEmpty())
        {
            return QString();
        }
        // separator：路径里最后一个分隔符位置；两种分隔符都要考虑。
        const qsizetype separator = std::max(
            path.lastIndexOf(QLatin1Char('\\')),
            path.lastIndexOf(QLatin1Char('/')));
        return separator >= 0 ? path.mid(separator + 1) : path;
    }

    AddressKind ClassifyAddress(const std::uint64_t address, const std::uint32_t pointerSize)
    {
        if (address < kNullPageLimit)
        {
            // 整个 NULL 页区间都不可映射：0 是裸空指针，小偏移是
            // “空指针 + 结构体成员偏移”，两者都指向同一个根因。
            return AddressKind::NullPage;
        }
        if (pointerSize == 4)
        {
            return address >= kKernelSpaceStart32
                ? AddressKind::KernelSpace
                : AddressKind::UserSpace;
        }
        if (address >= kKernelSpaceStart64)
        {
            return AddressKind::KernelSpace;
        }
        if (address < kUserSpaceLimit64)
        {
            return AddressKind::UserSpace;
        }
        // 落在 x64 的非规范地址空洞里：不是合法虚拟地址，多为数据被当成指针使用。
        return AddressKind::Unknown;
    }

    QString AddressKindText(const AddressKind kind)
    {
        switch (kind)
        {
        case AddressKind::NullPage:
            // 这段文字会用在寄存器、停止码参数等“不一定是地址”的场合，
            // 因此不能直接断言是空指针——寄存器里放个 1 或 0x100 是再正常不过的事。
            // 真正确定是地址的场合（如访问违例的出错地址）由调用方另行给出结论。
            return QStringLiteral("落在 NULL 页范围（作计数/标志更常见；若确为地址则是空指针加偏移）");
        case AddressKind::UserSpace:
            return QStringLiteral("用户态地址区间");
        case AddressKind::KernelSpace:
            return QStringLiteral("内核地址区间");
        case AddressKind::Poison:
            return QStringLiteral("分配器哨兵值");
        default:
            return QString();
        }
    }

    QString PoisonValueText(const std::uint64_t value, const std::uint32_t pointerSize)
    {
        // low/high：64 位下哨兵通常两半重复（如 0xCCCCCCCCCCCCCCCC）。
        const std::uint32_t low = static_cast<std::uint32_t>(value & 0xFFFFFFFFull);
        const std::uint32_t high = static_cast<std::uint32_t>(value >> 32);
        for (const PoisonPair& pair : kPoisonValues)
        {
            if (low != pair.value)
            {
                continue;
            }
            // 32 位目标只看低半；64 位要求高半也是同一哨兵或为 0，
            // 否则 0x00007FFCCCCCCCCC 这类正常地址会被误判。
            if (pointerSize == 4 || high == pair.value || high == 0)
            {
                return QString::fromUtf8(pair.meaning);
            }
        }
        // 全 F 是页表项/无效句柄常见的“已失效”标记，单列一条。
        if (value == 0xFFFFFFFFFFFFFFFFull || (pointerSize == 4 && low == 0xFFFFFFFFu))
        {
            return QStringLiteral("全 1 无效值（常见于未设置或已失效的指针/句柄）");
        }
        return QString();
    }

    void ModuleIndex::build(
        const std::vector<ModuleEntry>& modules,
        const std::vector<UnloadedModuleEntry>& unloaded,
        const std::uint32_t pointerSize)
    {
        m_pointerSize = pointerSize == 4 ? 4u : 8u;
        m_ranges.clear();
        m_ranges.reserve(modules.size() + unloaded.size());

        for (const ModuleEntry& module : modules)
        {
            CheckDumpCancellation();
            // 大小为 0 的模块无法形成区间，跳过以免产生零宽命中。
            if (module.base == 0 || module.size == 0)
            {
                continue;
            }
            Range range{};
            range.base = module.base;
            range.end = module.base + module.size;
            range.name = BaseModuleName(module.name);
            range.unloaded = false;
            m_ranges.push_back(std::move(range));
        }
        for (const UnloadedModuleEntry& module : unloaded)
        {
            CheckDumpCancellation();
            // 内核已卸载驱动表直接给出结束地址；用户态流只有大小。
            const std::uint64_t end = module.endAddress != 0
                ? module.endAddress
                : module.base + module.size;
            if (module.base == 0 || end <= module.base)
            {
                continue;
            }
            Range range{};
            range.base = module.base;
            range.end = end;
            range.name = BaseModuleName(module.name);
            range.unloaded = true;
            m_ranges.push_back(std::move(range));
        }

        // 排序键：基址升序；同基址时按名字定序，保证结果可复现。
        // 「已加载优先」不能靠排序实现——查找是从 upper_bound 往回走的，
        // 排序里谁在前，回溯时反而谁在后。优先级统一在 resolve() 里处理。
        std::sort(
            m_ranges.begin(),
            m_ranges.end(),
            [](const Range& left, const Range& right)
            {
                if (left.base != right.base)
                {
                    return left.base < right.base;
                }
                return left.name < right.name;
            });
    }

    AddressNote ModuleIndex::resolve(const std::uint64_t address) const
    {
        AddressNote note{};
        note.kind = ClassifyAddress(address, m_pointerSize);

        // 哨兵值优先于范围分类：0xCCCCCCCCCCCCCCCC 会被算成非规范地址，
        // 但真正有价值的信息是“这是未初始化内存”。
        const QString poison = PoisonValueText(address, m_pointerSize);
        if (!poison.isEmpty())
        {
            note.kind = AddressKind::Poison;
            note.description = poison;
            return note;
        }

        // upper_bound 找到第一个 base > address 的区间，候选都在它前面。
        // 不能只看紧邻的那一项：模块区间可能重叠（已卸载驱动的地址被新驱动复用，
        // 或畸形转储给出的区间互相包含），此时覆盖目标地址的那个未必是基址最大的。
        // 因此向前回溯若干项，并且**已加载模块优先于已卸载模块**——
        // 地址既落在某个在用模块里、又落在某个旧模块的历史区间里时，
        // 正在执行的显然是前者，报后者会把结论引到完全无关的驱动上。
        const auto position = std::upper_bound(
            m_ranges.begin(),
            m_ranges.end(),
            address,
            [](const std::uint64_t value, const Range& range) { return value < range.base; });
        // kProbeDepth：回溯上限，避免畸形转储里大量重叠区间把查找退化成线性扫描。
        constexpr int kProbeDepth = 8;
        const Range* loadedHit = nullptr;   // loadedHit：命中的已加载模块。
        const Range* unloadedHit = nullptr; // unloadedHit：命中的已卸载模块（次选）。
        auto candidate = position;
        for (int step = 0; step < kProbeDepth && candidate != m_ranges.begin(); ++step)
        {
            --candidate;
            if (address < candidate->base || address >= candidate->end)
            {
                continue;
            }
            if (candidate->unloaded)
            {
                if (unloadedHit == nullptr)
                {
                    unloadedHit = &(*candidate);
                }
            }
            else
            {
                loadedHit = &(*candidate);
                break;
            }
        }
        const Range* const hit = loadedHit != nullptr ? loadedHit : unloadedHit;
        if (hit != nullptr)
        {
            note.moduleName = hit->name;
            note.moduleBase = hit->base;
            note.offset = address - hit->base;
            note.unloadedModule = hit->unloaded;
            note.symbolText = QStringLiteral("%1+0x%2")
                .arg(hit->name)
                .arg(QString::number(note.offset, 16).toUpper());
        }

        if (note.description.isEmpty())
        {
            note.description = AddressKindText(note.kind);
        }
        return note;
    }

    QString ModuleIndex::symbolText(const std::uint64_t address) const
    {
        return resolve(address).symbolText;
    }

    QString ModuleIndex::annotate(const std::uint64_t address) const
    {
        const AddressNote note = resolve(address);
        if (!note.symbolText.isEmpty())
        {
            return note.unloadedModule
                ? QStringLiteral("%1（已卸载模块，高度可疑）").arg(note.symbolText)
                : note.symbolText;
        }
        // 小于一页的数值绝大多数是计数、标志或枚举，不是地址。
        // 给它们挂“NULL 页”注解只会在寄存器表和参数表里刷出一片噪声，
        // 真正的空指针场景由调用方（如访问违例的出错地址）单独判定并说明。
        if (note.kind == AddressKind::NullPage && address < 0x1000ull)
        {
            return QString();
        }
        return note.description;
    }
}
