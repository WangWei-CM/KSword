#pragma once

#include "KernelPageLayout.h"
#include "../../Ui/NumericSortKey.h"
#include "../../Ui/ImmutableTextRows.h"

#include <algorithm>
#include <cwchar>
#include <map>
#include <memory>
#include <numeric>
#include <string>
#include <unordered_set>
#include <utility>
#include <vector>

namespace Ksword::Features::Kernel {

// Worker-owned flattening of a complete facade result. Every dynamic/hidden
// column remains present; the UI chooses native column widths separately.
struct KernelDynamicProjection {
    using Rows = std::vector<std::vector<std::wstring>>;
    std::vector<std::wstring> columns;
    Ksword::Ui::ImmutableTextRows rows;
    std::shared_ptr<const Rows> rawRows;
};

inline KernelDynamicProjection BuildKernelDynamicProjection(const KernelFeatureId featureId,
    const KernelOperationResult& result) {
    KernelDynamicProjection output;
    std::unordered_set<std::wstring> seen;
    const auto add = [&](const std::wstring& column) {
        if (seen.insert(column).second) output.columns.push_back(column);
    };
    for (const auto& column : CanonicalColumnNames(featureId)) add(column);
    for (const auto& row : result.rows) for (const auto& column : row.columns) add(column.first);
    add(L"Detail");

    std::vector<std::vector<std::wstring>> aliases;
    aliases.reserve(output.columns.size());
    for (const auto& column : output.columns) {
        auto accepted = ColumnAliases(featureId, column);
        if (accepted.empty()) accepted.push_back(column);
        aliases.push_back(std::move(accepted));
    }
    struct InsensitiveLess {
        bool operator()(const std::wstring& left, const std::wstring& right) const {
            return ::_wcsicmp(left.c_str(), right.c_str()) < 0;
        }
    };
    output.rows.reserve(result.rows.size());
    for (const auto& row : result.rows) {
        // Preserve the original first case-insensitive occurrence even when
        // its value is empty. Alias priority decides any later fallback.
        std::map<std::wstring, const std::wstring*, InsensitiveLess> fields;
        for (const auto& field : row.columns) fields.try_emplace(field.first, &field.second);
        std::vector<std::wstring> cells(output.columns.size());
        for (std::size_t index = 0; index < output.columns.size(); ++index) {
            const auto& column = output.columns[index];
            if (column == L"#") {
                cells[index] = std::to_wstring(output.rows.size());
            } else if (column == L"Detail") {
                cells[index] = row.detailText;
            } else {
                for (const auto& alias : aliases[index]) {
                    const auto found = fields.find(alias);
                    if (found != fields.end() && !found->second->empty()) {
                        cells[index] = *found->second;
                        break;
                    }
                }
            }
        }
        output.rows.push_back(std::move(cells));
    }
    output.rawRows = output.rows.contiguousSnapshot();
    return output;
}

inline std::vector<std::size_t> BuildKernelSortOrder(const std::vector<std::wstring>& cells,
    const bool ascending) {
    std::vector<Ksword::Ui::NumericSortKey> keys;
    keys.reserve(cells.size());
    for (const auto& cell : cells) keys.push_back(Ksword::Ui::ParseNumericSortKey(cell));
    std::vector<std::size_t> order(cells.size());
    std::iota(order.begin(), order.end(), std::size_t{0});
    std::stable_sort(order.begin(), order.end(), [&](const auto left, const auto right) {
        int compared = Ksword::Ui::CompareNumericSortKeys(keys[left], keys[right]);
        if (!compared) compared = ::_wcsicmp(cells[left].c_str(), cells[right].c_str());
        return ascending ? compared < 0 : compared > 0;
    });
    return order;
}

struct KernelSortSnapshot {
    KernelFeatureId featureId = KernelFeatureId::ObjectNamespaceOverview;
    std::uint64_t revision = 0;
    std::uint64_t requestId = 0;
    int column = -1;
    bool ascending = true;
    std::vector<std::size_t> order;
    std::vector<std::size_t> inverse;
    Ksword::Ui::ImmutableTextRows rows;
};

struct KernelQuerySnapshot {
    KernelOperationResult result;
    KernelDynamicProjection projection;
};

} // namespace Ksword::Features::Kernel
