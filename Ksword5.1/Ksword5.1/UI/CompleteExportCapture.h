#pragma once

#include <QObject>
#include <QPointer>
#include <QString>

#include <cstdint>
#include <deque>
#include <functional>
#include <memory>

class QTableView;
class QAbstractItemView;

namespace ks::ui
{
    // All fragments are independently owned. Long cells are copied a bounded
    // fragment at a time on GUI; joining/escaping belongs to the value worker.
    struct CompleteExportCapturedCell
    {
        std::deque<QString> fragments;
        QString text() const;
    };

    struct CompleteExportCapturedTable
    {
        QString sourceIdentity;
        std::uint64_t sourceVersion = 0;
        QString languageId;
        std::deque<int> sourceColumns;
        std::deque<std::uint64_t> sourceRows;
        std::deque<CompleteExportCapturedCell> headers;
        std::deque<CompleteExportCapturedCell> cells; // Complete rows, row-major.
        const CompleteExportCapturedCell& cell(std::size_t row, std::size_t column) const;
    };

    // GUI-only producer port, destroyed on GUI before a completed value is
    // handed off. Each callable must be finite, reentrancy-safe and read only.
    // revision must change for every content/order/filter/column/selection
    // mutation that affects output; languageId must cover rendered wording.
    struct CompleteExportCapturePort
    {
        QPointer<QObject> source;
        QString sourceIdentity;
        std::uint64_t sourceVersion = 0;
        std::function<bool()> valid;
        std::function<std::uint64_t()> revision;
        std::function<QString()> languageId;
        std::function<int()> rowCount;
        std::function<int()> columnCount;
        // Optional hierarchy cursor. It advances once per ordinal, returns
        // false at the true end and removes the flat rowCount bound. GUI only.
        std::function<bool(std::uint64_t)> rowAvailable;
        bool scopeOnly = false; // Capture exact row/column scope, no DisplayRole cells.
        // Optional GUI value capture, once per included row, before handoff.
        // It must copy only that bounded business row, never format all cells.
        std::function<void(int)> rowCaptured;
        std::function<bool(int)> rowIncluded;
        std::function<bool(int)> columnIncluded;
        std::function<QString(int)> header;
        std::function<QString(int, int)> cell;
    };

    struct CompleteExportCaptureViewWatch
    {
        bool geometry = true; // Hide/show/column layout; disable only with a complete scope epoch.
        bool selection = false; // Enable for output whose scope actually depends on selection/current row.
    };

    // Signal-aware flat model/view adapter. Row/column business predicates are
    // explicit so freeze-hidden evidence and proxy/compare scope are preserved.
    // The optional scope revision must cover changes without model/header
    // signals (business masks, selection policy, etc). It is GUI only.
    CompleteExportCapturePort MakeCompleteExportTableViewCapturePort(QTableView* view,
        QString sourceIdentity, std::uint64_t sourceVersion,
        std::function<bool(int)> rowIncluded, std::function<bool(int)> columnIncluded,
        std::function<std::uint64_t()> scopeRevision = {}, std::function<QString()> languageId = {},
        CompleteExportCaptureViewWatch watch = {});

    CompleteExportCapturePort MakeCompleteExportItemViewCapturePort(QAbstractItemView* view,
        QString sourceIdentity, std::uint64_t sourceVersion,
        std::function<bool(int)> rowIncluded, std::function<bool(int)> columnIncluded,
        std::function<std::uint64_t()> scopeRevision = {}, std::function<QString()> languageId = {},
        CompleteExportCaptureViewWatch watch = {});

    enum class CompleteExportCapturePhase { Queued, Capturing, Completed, Canceled, Invalidated, Failed, Closed };
    enum class CompleteExportCaptureError { None, InvalidPort, SourceChanged, ReadFailed };
    struct CompleteExportCaptureStatus
    {
        std::uint64_t generation = 0;
        CompleteExportCapturePhase phase = CompleteExportCapturePhase::Queued;
        CompleteExportCaptureError error = CompleteExportCaptureError::None;
        int sourceRows = 0;
        std::uint64_t visitedRows = 0;
        std::uint64_t capturedRows = 0;
        std::uint64_t capturedCells = 0;
        QString nativeError;
        bool terminal() const;
    };
    struct CompleteExportCaptureBudget
    {
        int milliseconds = 3;
        int maximumUnits = 128;
        int utf16UnitsPerFragment = 32768;
    };

    class CompleteExportCapture final : public QObject
    {
    public:
        using Completion = std::function<void(const CompleteExportCaptureStatus&,
            std::shared_ptr<const CompleteExportCapturedTable>)>;
        explicit CompleteExportCapture(QObject* owner);
        ~CompleteExportCapture() override;
        // start performs no table walk. Work is genuinely queued and budgeted.
        // Busy/closed/invalid port returns0. Completion may delete/reenter owner.
        std::uint64_t start(CompleteExportCapturePort port, Completion completion,
            CompleteExportCaptureBudget budget = {});
        void cancel(); // Preparation only; never connected to accepted file jobs.
        void close(); // Call first in owner destructor.
        bool isBusy() const;
        CompleteExportCaptureStatus status() const;
        void setProgressCallback(std::function<void(const CompleteExportCaptureStatus&)> callback);

    private:
        struct State;
        std::unique_ptr<State> m_state;
        void queue();
        void step();
        bool unchanged();
        void finish(CompleteExportCapturePhase phase, CompleteExportCaptureError error = CompleteExportCaptureError::None,
            QString detail = {});
    };
}
