#pragma once
#include "CompleteExport.h"
#include <functional>
#include <memory>

class QTableView;
class QTreeView;

namespace ks::ui
{
    enum class CompleteTableScope { All, Selected };
    enum class CompleteTableColumns { Visible, All };

    // GUI factory retains a complete immutable display version in O(1).
    // Values contain the complete CURRENT logical model row order/projection,
    // never a viewport prefix. All cells are the original DisplayRole wording.
    // cell is stateless and may only read values; no QObject/borrowed storage.
    // Canonical values and prepareRow value captures use typed
    // RetainCompleteExportSnapshot handles, including cancel/close retirement.
    // Shared row filters, hidden/frozen columns and selected/current scope are
    // captured centrally in queued slices while presentation commits defer.
    struct CompleteExportPublishedTable
    {
        std::shared_ptr<const void> values;
        QString sourceIdentity;
        std::uint64_t sourceVersion = 0;
        int rows = 0;
        int columns = 0;
        QString (*cell)(const void*, int row, int logicalColumn) = nullptr;
        // Optional GUI-only adapter for an existing mutable row model. The
        // common queued capture copies each included canonical business row
        // into values before acceptance. It is cleared before worker handoff.
        // Ready immutable producers leave this empty.
        std::function<void(int)> prepareRow;
    };
    struct CompleteExportCapturedTable;
    CompleteExportInput MakeCompleteTableExportInput(std::shared_ptr<const CompleteExportCapturedTable> scope,
        CompleteExportPublishedTable published, bool includeHeader, bool appendNewline,
        ks::i18n::SourceTextSnapshot translator);

    using CompleteExportTableProducer = std::function<CompleteExportPublishedTable()>;
    void RegisterCompleteExportTableProducer(QTableView* view, CompleteExportTableProducer producer);

    void CopyCompleteTableRows(QTableView* view, CompleteTableScope scope,
        CompleteTableColumns columns = CompleteTableColumns::Visible);
    void ExportCompleteTableRows(QTableView* view, CompleteTableScope scope,
        const QString& defaultFileNamePrefix, CompleteTableColumns columns = CompleteTableColumns::Visible);
    void CopyCompleteTreeRows(QTreeView* view);

    // Escape cancels preparation only. An accepted atomic output never cancels.
    inline constexpr char CompleteExportBusyProperty[] = "ksword_complete_export_busy";
    inline constexpr char CompleteExportPreparingProperty[] = "ksword_complete_export_preparing";
}
