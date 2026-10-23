#pragma once

#include <QPersistentModelIndex>
#include <QSet>
#include <functional>

class QTableView;

namespace ks::ui
{
    // Exclusions are first-column identities in the table's current model/root.
    // Sorting preserves them; reset/replacement invalidates them. The source model,
    // delegate and selection model stay unchanged. An empty set clears this layer.
    enum class TableRowFilterBusinessOwnership { LegacyCompatible, ExclusiveSharedIdentity };
    // Opt in only after every business row-hide writer for this table uses the
    // canonical API. Default compatibility preserves direct setRowHidden writers.
    void SetTableRowFilterBusinessOwnership(QTableView* table, TableRowFilterBusinessOwnership ownership);

    void ApplyTableRowFilter(QTableView* tableView,
        const QSet<QPersistentModelIndex>& excludedRows);
    // Optional large-filter projection: commit canonical identities immediately,
    // then converge row visibility in 4ms/256-row GUI slices. Replacements supersede
    // pending work; source model and row identities remain unchanged. The caller
    // keeps its existing filtering status while projectionPending() is true.
    void ApplyTableRowFilterIncrementally(QTableView* tableView,
        const QSet<QPersistentModelIndex>& excludedRows);
    // Immutable value filters can retain their own stable row keys instead of
    // allocating a persistent index for every exclusion. The fast GUI predicate
    // reads the current model row; it must follow that row's stable value key
    // across sorting/insertion/removal. Canonical acceptance is immediate and
    // the complete visibility replay is budgeted, including after model changes.
    // Requires ExclusiveSharedIdentity ownership. An empty predicate clears the
    // business layer. Other filter layers compose. Use a complete filter to
    // switch back to the persistent-index API before submitting index deltas.
    void ApplyTableRowFilterPredicateIncrementally(QTableView* tableView,
        std::function<bool(const QModelIndex&)> excluded);
    // Incremental append/trim producers compute a pure value delta on their
    // worker, validate the source generation, then create only changed indexes.
    // This edits the existing canonical business layer in O(delta) average work;
    // search/legacy/freeze layers still compose and projection remains budgeted.
    // New exclusions win if a row appears in both sets. Reset invalidates all ids.
    void ApplyTableRowFilterDelta(QTableView* tableView,
        const QSet<QPersistentModelIndex>& newlyExcludedRows,
        const QSet<QPersistentModelIndex>& newlyIncludedRows);
    bool IsTableRowFilterProjectionPending(const QTableView* tableView);
    bool IsTableRowExcludedByFilter(const QTableView* tableView, int row);
    bool HasTableRowsPassingFilters(const QTableView* tableView);
    quint64 TableRowFilterIdentityRevision(const QTableView* tableView);

    // Shared search/freeze adapters; a new search may search beyond its previous
    // result filter, but must never reveal a row excluded by the business filter.
    bool IsTableCellExcludedByBusinessFilter(const QTableView* tableView,
        const QModelIndex& index);
    bool IsTableRowHiddenByLegacyFilter(const QTableView* tableView, int row);
    void SetTableSearchRowExcluded(QTableView* tableView, int row, bool excluded);
    void ClearTableSearchRowFilter(QTableView* tableView);

    inline constexpr char TableRowFilterRevisionProperty[] =
        "ksword_table_row_filter_revision";
}
