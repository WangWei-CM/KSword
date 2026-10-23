#pragma once
#include <QStringList>
#include <vector>
#include "../Internationalization/LanguageManager.h"

namespace ks::ui {
// Page producers and the cold directory consume the same ordered tab records.
enum class MemoryNavigationGroup { None, Audit, Pfn, Consumer, HyperV, Workbench };
struct MemoryNavigationTabSpec {
    const char *id, *source, *languageKey;
    bool historicalChinese;
    MemoryNavigationGroup children;
};
inline const std::vector<MemoryNavigationTabSpec>& MemoryNavigationTabs(const MemoryNavigationGroup group)
{
    switch (group) {
    case MemoryNavigationGroup::Audit: {
        static const std::vector<MemoryNavigationTabSpec> tabs{
            {"distribution", "Physical distribution", "memory.audit.tab.distribution", false, MemoryNavigationGroup::None},
            {"user_residency", "User-mode residency", "memory.audit.tab.user_residency", false, MemoryNavigationGroup::None},
            {"processes", "Kernel process snapshot", "memory.audit.tab.processes", false, MemoryNavigationGroup::None},
            {"pool_tags", "Pool tags", "memory.audit.tab.pool_tags", false, MemoryNavigationGroup::None},
            {"big_pool", "Big Pool allocations", "memory.audit.tab.big_pool", false, MemoryNavigationGroup::None},
            {"pfn", "Physical page attribution", nullptr, false, MemoryNavigationGroup::Pfn},
            {"hyperv", "Hyper-V / host memory", nullptr, false, MemoryNavigationGroup::HyperV},
        };
        return tabs;
    }
    case MemoryNavigationGroup::Pfn: {
        static const std::vector<MemoryNavigationTabSpec> tabs{
            {"usage", "Usage by page state", nullptr, false, MemoryNavigationGroup::None},
            {"identities", "Largest backing identities", nullptr, false, MemoryNavigationGroup::None},
            {"mappings", "PFN and mappings", nullptr, false, MemoryNavigationGroup::None},
            {"evidence", "Coverage and evidence", nullptr, false, MemoryNavigationGroup::None},
            {"coverage", "Consumer coverage", nullptr, false, MemoryNavigationGroup::None},
            {"objects", "Objects and observed consumers", nullptr, false, MemoryNavigationGroup::None},
            {"page_consumers", "Page-table, stack and pool consumers", nullptr, false, MemoryNavigationGroup::None},
            {"consumers", "GPU and allocation history", nullptr, false, MemoryNavigationGroup::Consumer},
        };
        return tabs;
    }
    case MemoryNavigationGroup::Consumer: {
        static const std::vector<MemoryNavigationTabSpec> tabs{
            {"gpu", "GPU consumers", nullptr, false, MemoryNavigationGroup::None},
            {"pool_analysis", "Pool allocation analysis", nullptr, false, MemoryNavigationGroup::None},
            {"recorder", "Recorder output", nullptr, false, MemoryNavigationGroup::None},
        };
        return tabs;
    }
    case MemoryNavigationGroup::HyperV: {
        static const std::vector<MemoryNavigationTabSpec> tabs{
            {"partitions", "Partitions and utility VMs", nullptr, false, MemoryNavigationGroup::None},
            {"host", "Host memory evidence", nullptr, false, MemoryNavigationGroup::None},
            {"processes", "Visible host processes", nullptr, false, MemoryNavigationGroup::None},
            {"providers", "Provider coverage", nullptr, false, MemoryNavigationGroup::None},
            {"interpretation", "Interpretation and snapshot context", nullptr, false, MemoryNavigationGroup::None},
        };
        return tabs;
    }
    case MemoryNavigationGroup::Workbench: {
        static const std::vector<MemoryNavigationTabSpec> tabs{
            {"hex", "十六进制", nullptr, true, MemoryNavigationGroup::None},
            {"disasm", "反汇编", nullptr, true, MemoryNavigationGroup::None},
            {"text", "文本", nullptr, true, MemoryNavigationGroup::None},
            {"compare", "对比", nullptr, true, MemoryNavigationGroup::None},
            {"pseudocode", "C 伪代码", nullptr, true, MemoryNavigationGroup::None},
        };
        return tabs;
    }
    default: { static const std::vector<MemoryNavigationTabSpec> empty; return empty; }
    }
}
inline QString MemoryNavigationTabTitle(const MemoryNavigationGroup group, const int index)
{
    const auto& tabs = MemoryNavigationTabs(group);
    if (index < 0 || static_cast<std::size_t>(index) >= tabs.size()) { return {}; }
    const auto& tab = tabs[static_cast<std::size_t>(index)];
    const auto source = QString::fromUtf8(tab.source);
    if (tab.languageKey) { return ks::i18n::text(QString::fromLatin1(tab.languageKey), source); }
    return tab.historicalChinese ? ks::i18n::sourceText(source) : ks::i18n::packedSourceText(source);
}
inline QStringList MemoryNavigationTabTitles(const MemoryNavigationGroup group)
{
    QStringList titles;
    for (int index = 0; index < static_cast<int>(MemoryNavigationTabs(group).size()); ++index) {
        titles.append(MemoryNavigationTabTitle(group, index));
    }
    return titles;
}
}
