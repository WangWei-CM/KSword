#pragma once

#include "KernelDynamicProjection.h"
#include <array>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace Ksword::Features::Kernel {

struct KernelObjectNamespaceTreeNodeState;
struct KernelProjectionSource {
    KernelFeatureId featureId = KernelFeatureId::ObjectNamespaceOverview;
    std::uint64_t serial = 0;
    std::vector<std::wstring> columns;
    std::shared_ptr<const KernelDynamicProjection::Rows> rows;
    Ksword::Ui::ImmutableTextRows fallbackDisplay;
};
struct KernelProjectionInput {
    std::shared_ptr<const KernelProjectionSource> source;
    KernelFeatureDescriptor descriptor;
    std::wstring filter, moduleFilter, windowsDirectory,treeSelectedKey,treeTopKey;
    bool riskOnly = false;
    std::array<std::wstring,4> selectedChoices;
    std::array<bool,4> choicePresent{};
};
struct KernelProjectedChoice { std::vector<std::wstring> items; int selected = 0; bool changed = false; };
struct KernelProjectedTreeNode {
    std::size_t parent = 0;
    std::wstring text;
    std::shared_ptr<KernelObjectNamespaceTreeNodeState> state;
    bool expand = false;
};
inline std::size_t KernelProjectedRowHash(const std::vector<std::wstring>& row) {
    std::size_t hash=row.size();
    for(const auto& field:row){
        hash^=std::hash<std::wstring>{}(field)+0x9e3779b9U+(hash<<6U)+(hash>>2U);
        hash^=field.size()+0x9e3779b9U+(hash<<6U)+(hash>>2U);
    }
    return hash;
}
struct KernelValueProjection {
    std::shared_ptr<const KernelProjectionInput> input;
    std::uint64_t requestId = 0;
    std::vector<std::wstring> columns;
    std::vector<int> widths, indents;
    Ksword::Ui::ImmutableTextRows rows;
    std::vector<std::vector<std::wstring>> summary;
    std::wstring status, detail;
    bool hasStatus = false, hasDetail = false, selectionDetail = false, selectFirst = false;
    std::array<KernelProjectedChoice,4> choices;
    bool tree = false;
    std::vector<KernelProjectedTreeNode> treeNodes;
    std::vector<std::uintptr_t> nativeTreeHandles;
    std::size_t treeSelected=0,treeTop=0;
    std::unordered_map<std::size_t,std::vector<std::size_t>> rowHashes;
};
inline int KernelFindProjectedRow(const std::vector<std::wstring>& value,const KernelValueProjection& projection) {
    const auto found=projection.rowHashes.find(KernelProjectedRowHash(value));
    if(found==projection.rowHashes.end())return -1;
    for(const auto index:found->second)if(index<projection.rows.size()&&projection.rows[index]==value)return static_cast<int>(index);
    return -1;
}

inline bool KernelProjectionMatches(const KernelFeatureId feature, const std::uint64_t request,
    const std::shared_ptr<const KernelProjectionSource>& expected, const KernelValueProjection& value) {
    return value.input && value.input->source && value.requestId==request &&
        value.input->source->featureId==feature && value.input->source==expected;
}

} // namespace Ksword::Features::Kernel
