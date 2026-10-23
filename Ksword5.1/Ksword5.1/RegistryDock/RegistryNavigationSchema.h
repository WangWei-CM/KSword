#pragma once
#include "../UI/PageNavigationTabCatalog.h"
namespace ks::registry_navigation
{
    enum Page { Editor, Values, Search, Pending, Optimization };
    inline constexpr ks::ui::PageNavigationTabDefinition Definitions[] = {
        {"registry.editor", "", "registry.tab.editor", "注册表编辑", true},
        {"registry.values", "registry.editor", "registry.tab.values", "值列表", true},
        {"registry.search", "registry.editor", "registry.tab.search_results", "搜索结果", false},
        {"registry.pending", "registry.editor", "", "待应用", false},
        {"registry.optimization", "", "registry.tab.optimization", "系统优化", false}
    };
}
