#pragma once

#include "../UI/AsyncOperation.h"
#include "../UI/CompleteExport.h"

#include <QEvent>
#include <QPointer>
#include <QTimer>
#include <QStringList>
#include <memory>
#include <type_traits>
#include <vector>

namespace ks::monitor
{
    template<class Value> std::shared_ptr<const Value> MakeCanonicalExportSnapshot(Value value)
    {
        return ks::ui::RetainCompleteExportSnapshot<Value>(
            std::make_shared<const Value>(std::move(value)));
    }
    enum class CanonicalExportOutcomeKind { Empty, Canceled, PreparationFailed, NotAccepted, FileFinished };
    struct CanonicalExportOutcome
    {
        CanonicalExportOutcomeKind kind = CanonicalExportOutcomeKind::PreparationFailed;
        ks::ui::CompleteExportReceipt receipt;
    };
    enum class CanonicalExportFormat { Tsv, FlatTsv, QuotedCsv };
    template<class Snapshot> struct PreparedCanonicalExport
    {
        using Ref = typename std::decay_t<decltype(std::declval<Snapshot>().rows)>::value_type;
        std::shared_ptr<const Snapshot> source;
        std::vector<Ref> rows;
        QString comment;
        CanonicalExportFormat format = CanonicalExportFormat::Tsv;
    };
    template<class Snapshot, class Sink>
    bool FormatCanonicalMonitorExport(const PreparedCanonicalExport<Snapshot>& value, Sink& sink)
    {
        if (!value.source) return false;
        if (!value.comment.isEmpty() && !sink.writeText(value.comment + QChar('\n'))) return false;
        const bool csv = value.format == CanonicalExportFormat::QuotedCsv;
        auto encode = [csv, &value](QString text, bool header) {
            if (csv) { text.replace(QChar('"'), QStringLiteral("\"\"")); return QChar('"') + text + QChar('"'); }
            if (!header) {
                text.replace(QChar('\t'), QChar(' '));
                if (value.format == CanonicalExportFormat::FlatTsv) text.replace(QChar('\n'), QChar(' '));
            }
            return text;
        };
        QStringList header;
        for (const auto& cell : value.source->headers) header.push_back(encode(cell, true));
        const QChar separator(csv ? ',' : '\t');
        if (!sink.writeText(header.join(separator) + QChar('\n'))) return false;
        for (const auto& record : value.rows) {
            auto cells = value.source->displayedCells(record);
            for (auto& cell : cells) cell = encode(std::move(cell), false);
            if (!sink.writeText(cells.join(separator) + QChar('\n')) || !sink.rowsCompleted()) return false;
        }
        return true;
    }

    // Pure canonical snapshots make preparation independent of model mutation.
    // Match/sort stay in a cancelable value job; only the complete selected set
    // is accepted as a noncancelable complete-file side-effect transaction.
    class MonitorCanonicalExport final : public QObject
    {
    public:
        using Completion = std::function<void(const CanonicalExportOutcome&)>;
        explicit MonitorCanonicalExport(QObject* owner)
            : QObject(owner), m_prepare(new ks::ui::AsyncOperation(this)), m_writer(new ks::ui::CompleteExportOperation(this)), m_observed(owner)
        {
            const QPointer<MonitorCanonicalExport> guard(this);
            m_prepare->setStateChangedCallback([guard]() { if (guard) guard->notify(); });
            m_writer->setStateChangedCallback([guard]() { if (guard) guard->notify(); });
            if (m_observed) m_observed->installEventFilter(this);
        }
        ~MonitorCanonicalExport() override { close(); }
        void close()
        {
            if (m_closed) return;
            m_closed = true; m_changed = {}; m_completion = {}; m_prepare->close(); m_writer->close();
            if (m_observed) m_observed->removeEventFilter(this);
        }
        bool isBusy() const { return m_prepare->isBusy() || m_writer->isBusy(); }
        bool isPreparing() const { return m_prepare->isBusy() && !m_writer->isBusy(); }
        void cancelPreparation()
        {
            if (!isPreparing()) return;
            const QPointer<MonitorCanonicalExport> guard(this);
            const auto completion = std::move(m_completion);
            m_prepare->cancel();
            if (guard) notify();
            if (guard && !m_closed && completion) completion({CanonicalExportOutcomeKind::Canceled, {}});
        }
        void reconcile() { m_writer->reconcile(); }
        void setStateChangedCallback(std::function<void()> callback) { m_changed = std::move(callback); }

        template<class Snapshot>
        bool start(std::shared_ptr<const Snapshot> source, QString identity, QString path, QString comment, Completion completion,
            CanonicalExportFormat format = CanonicalExportFormat::Tsv, bool requireRows = true)
        {
            if (m_closed || isBusy() || !source) return false;
            using Prepared = PreparedCanonicalExport<Snapshot>;
            struct Request { std::shared_ptr<const Snapshot> source; QString comment; CanonicalExportFormat format; };
            m_completion = std::move(completion);
            const QPointer<MonitorCanonicalExport> guard(this);
            const auto ticket = m_prepare->submit<Request, std::shared_ptr<const Prepared>>({std::move(source), std::move(comment), format},
                [](const Request& request, const ks::ui::AsyncOperationToken& token) {
                    Prepared result;
                    result.source = request.source; result.comment = request.comment; result.format = request.format;
                    const auto ordered = request.source->orderedRows(); result.rows.reserve(ordered.size());
                    for (const auto& record : ordered) {
                        if (token.isCanceled()) return std::shared_ptr<const Prepared>{};
                        if (request.source->matches(record)) result.rows.push_back(record);
                    }
                    return MakeCanonicalExportSnapshot(std::move(result));
                },
                [guard, identity = std::move(identity), path = std::move(path), requireRows](const std::shared_ptr<const Prepared>& prepared, std::uint64_t) {
                    if (!guard || guard->m_closed || !prepared) return true;
                    if (requireRows && prepared->rows.empty()) { guard->finish({CanonicalExportOutcomeKind::Empty, {}}); return true; }
                    const auto& snapshot = *prepared->source;
                    auto input = ks::ui::MakeCompleteExportInput(prepared, identity, snapshot.archiveRevision,
                        std::optional<std::uint64_t>(prepared->rows.size()), snapshot.language,
                        [](const Prepared& value, ks::ui::CompleteExportSink& sink, const ks::i18n::SourceTextSnapshot&) {
                            return FormatCanonicalMonitorExport(value, sink);
                        });
                    input.fileMode = ks::ui::CompleteExportFileMode::NativeText;
                    input.utf8Bom = prepared->format == CanonicalExportFormat::QuotedCsv;
                    const auto journal = guard->m_writer->journal();
                    if (!guard->m_writer->startFile(path, std::move(input), [guard, journal](const auto& receipt) {
                        if (guard && !guard->m_closed) guard->finish({CanonicalExportOutcomeKind::FileFinished, receipt});
                        if (guard && !guard->m_closed && receipt.phase != ks::ui::CompleteExportPhase::OutcomeUnavailable)
                            journal->acknowledge(receipt.id);
                    })) guard->finish({CanonicalExportOutcomeKind::NotAccepted, {}});
                    return true;
                },
                [guard](std::exception_ptr) { if (guard && !guard->m_closed) guard->finish({CanonicalExportOutcomeKind::PreparationFailed, {}}); });
            return ticket != 0;
        }

    protected:
        bool eventFilter(QObject* watched, QEvent* event) override
        {
            if (!m_closed && watched == m_observed && !m_reconcileQueued
                && (event->type() == QEvent::Show || event->type() == QEvent::WindowActivate))
            {
                m_reconcileQueued = true;
                const QPointer<MonitorCanonicalExport> guard(this);
                QTimer::singleShot(0, this, [guard]() {
                    if (!guard) return;
                    guard->m_reconcileQueued = false;
                    if (!guard->m_closed) guard->reconcile();
                });
            }
            return QObject::eventFilter(watched, event);
        }
    private:
        void notify() { if (!m_closed && m_changed) m_changed(); }
        void finish(CanonicalExportOutcome value)
        {
            const QPointer<MonitorCanonicalExport> guard(this);
            const auto completion = std::move(m_completion);
            notify();
            if (guard && !m_closed && completion) completion(value);
        }
        ks::ui::AsyncOperation* m_prepare;
        ks::ui::CompleteExportOperation* m_writer;
        QPointer<QObject> m_observed;
        std::function<void()> m_changed;
        Completion m_completion;
        bool m_closed = false, m_reconcileQueued = false;
    };
}
