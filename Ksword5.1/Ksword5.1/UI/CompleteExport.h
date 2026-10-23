#pragma once

#include "../Internationalization/LanguageManager.h"

#include <QByteArrayView>
#include <QObject>
#include <QString>
#include <QStringView>

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <type_traits>
#include <utility>
#include <vector>

namespace ks::ui
{
    enum class CompleteExportDestination { File, Clipboard };
    enum class CompleteExportFileMode { Binary, NativeText };
    enum class CompleteExportError
    {
        None, InvalidInput, OpenFailed, WriteFailed, FlushFailed, CommitFailed,
        FormatFailed, IncompleteRows, UnexpectedFailure, ClipboardUnavailable, ResultUnavailable
    };
    enum class CompleteExportPhase
    {
        AwaitingAdmission, Running, ClipboardReady, ClipboardPublishing,
        FileCommitted, ClipboardPublished, Failed, NotAccepted, ClipboardNotPublished, OutcomeUnavailable
    };

    struct CompleteExportReceipt
    {
        std::uint64_t id = 0;
        CompleteExportDestination destination = CompleteExportDestination::File;
        CompleteExportPhase phase = CompleteExportPhase::AwaitingAdmission;
        CompleteExportError error = CompleteExportError::None;
        QString sourceIdentity;
        std::uint64_t sourceVersion = 0;
        QString languageId;
        QString outputPath;
        QString nativeError;
        std::optional<std::uint64_t> expectedRows;
        std::uint64_t rows = 0;
        std::uint64_t utf8Bytes = 0; // Logical bytes supplied, before native Text newline conversion.
        bool terminal() const;
        bool succeeded() const;
    };

    // Testable atomic device contract. Production creates a QSaveFile on the
    // worker thread; fake devices exercise short writes/flush/commit without IO.
    class CompleteExportAtomicDevice
    {
    public:
        virtual ~CompleteExportAtomicDevice() = default;
        virtual bool open(const QString& path, CompleteExportFileMode mode) = 0;
        virtual qint64 write(const char* bytes, qint64 length) = 0;
        virtual bool flush() = 0;
        virtual bool commit() = 0;
        virtual void cancel() = 0;
        virtual QString errorString() const = 0;
    };

    class CompleteExportSink final
    {
    public:
        // Long QString values encode in bounded chunks; no full intermediate
        // line/UTF8 document is needed for file output. A short positive write
        // is completed; zero/negative or over-reported writes fail atomically.
        bool writeUtf8(QByteArrayView bytes);
        bool writeText(QStringView text);
        bool rowsCompleted(std::uint64_t count = 1);
        void fail(CompleteExportError error, QString detail = {});
        bool good() const;
        std::uint64_t rows() const;
        std::uint64_t utf8Bytes() const;

    private:
        struct State;
        explicit CompleteExportSink(State& state);
        State* m_state;
        friend struct CompleteExportEngine;
    };

    struct CompleteExportInput
    {
        QString sourceIdentity;
        std::uint64_t sourceVersion = 0;
        std::optional<std::uint64_t> expectedRows;
        ks::i18n::SourceTextSnapshot translator;
        CompleteExportFileMode fileMode = CompleteExportFileMode::NativeText;
        bool utf8Bom = false; // File only. Clipboard always receives plain Unicode text.
        std::function<bool(CompleteExportSink&, const ks::i18n::SourceTextSnapshot&)> serialize;
    };

    namespace complete_export_detail
    {
        // Returns an alias whose final release only enqueues a preallocated
        // owner node. The retained complete value is released by the independent
        // value retirement thread, including rejection, close and dropped delivery.
        std::shared_ptr<const void> RetainValue(const std::shared_ptr<const void>& value);
    }

    // Use the returned handle as the producer's canonical owner as well when
    // its own model/controller can release a complete root on GUI. Values only.
    template<class Value>
    std::shared_ptr<const Value> RetainCompleteExportSnapshot(const std::shared_ptr<const Value>& value)
    {
        static_assert(!std::is_base_of_v<QObject, Value>, "Retirement accepts values, never QObject");
        if (!value) return {};
        const auto retained = complete_export_detail::RetainValue(value);
        return std::shared_ptr<const Value>(retained, value.get());
    }

    // Explicit producer port: retain a published immutable value root in O(1).
    // A formatter must be stateless; all finite context belongs in Snapshot.
    // Snapshot must contain values only (no model/widget/document/QObject or
    // borrowed storage). Publishing/capturing it must not deepcopy full source
    // data on GUI; GUI-only models use the separate budgeted capture adapter.
    template<class Snapshot, class Formatter>
    CompleteExportInput MakeCompleteExportInput(const std::shared_ptr<const Snapshot>& snapshot,
        QString sourceIdentity, std::uint64_t sourceVersion,
        std::optional<std::uint64_t> expectedRows, ks::i18n::SourceTextSnapshot translator,
        Formatter formatter)
    {
        static_assert(!std::is_base_of_v<QObject, Snapshot>, "Export snapshot must be a value, never QObject");
        static_assert(std::is_empty_v<Formatter> || std::is_pointer_v<Formatter>,
            "Export formatter must be stateless; freeze context inside the value snapshot");
        CompleteExportInput input;
        input.sourceIdentity = std::move(sourceIdentity);
        input.sourceVersion = sourceVersion;
        input.expectedRows = expectedRows;
        input.translator = std::move(translator);
        if (!snapshot) return input;
        const auto retained = complete_export_detail::RetainValue(snapshot);
        input.serialize = [retained, formatter](CompleteExportSink& sink,
            const ks::i18n::SourceTextSnapshot& frozen) {
            return retained && formatter(*static_cast<const Snapshot*>(retained.get()), sink, frozen);
        };
        return input;
    }

    struct CompleteExportResult
    {
        CompleteExportReceipt receipt;
        std::shared_ptr<const QString> clipboardText; // Never retained by the receipt journal.
    };

    // Pure value engine entry for fake-device tests and non-UI consumers.
    // There is no cancellation token after admission and no borrowed GUI access.
    CompleteExportResult RunCompleteExportFile(const CompleteExportInput& input,
        const QString& path, CompleteExportAtomicDevice& device);
    CompleteExportResult RunCompleteExportCopy(const CompleteExportInput& input);

    struct CompleteExportRecord;
    class CompleteExportJournal final
    {
    public:
        CompleteExportJournal();
        ~CompleteExportJournal();
        std::optional<CompleteExportReceipt> receipt(std::uint64_t id) const;
        std::vector<CompleteExportReceipt> receipts() const;
        // Only terminal metadata may be acknowledged. Accepted work is never
        // evicted; the journal contains no source roots or full copied text.
        bool acknowledge(std::uint64_t id);

    private:
        struct State;
        std::unique_ptr<State> m_state;
        std::shared_ptr<CompleteExportRecord> prepare(const CompleteExportInput& input,
            CompleteExportDestination destination, const QString& path);
        friend class CompleteExportOperation;
    };

    // Obtain on GUI and retain in the controller. Workers receive a record
    // value handle and never call this accessor or any LanguageManager singleton.
    std::shared_ptr<CompleteExportJournal> SharedCompleteExportJournal();

    class CompleteExportOperation final : public QObject
    {
    public:
        using Completion = std::function<void(const CompleteExportReceipt&)>;
        using ClipboardPublisher = std::function<bool(const QString&)>; // GUI only.
        explicit CompleteExportOperation(QObject* owner,
            std::shared_ptr<CompleteExportJournal> journal = {});
        ~CompleteExportOperation() override;

        // Select a path and freeze a complete stable value version before this
        // call. Recheck page/capture busy after a native dialog. Nonzero accepts
        // one side-effect transaction; busy/closed/invalid/admission refusal is 0.
        // Accepted work survives close/drop and is never latest-only/canceled.
        std::uint64_t startFile(QString path, CompleteExportInput input, Completion completion);
        std::uint64_t startCopy(CompleteExportInput input, ClipboardPublisher publish,
            Completion completion);
        void close(); // Call first in the owning page's destructor.
        bool isBusy() const;
        std::optional<CompleteExportReceipt> currentReceipt() const;
        std::shared_ptr<CompleteExportJournal> journal() const;
        void setStateChangedCallback(std::function<void()> callback);
        void reconcile(); // Recovers actual file result when a Qt completion was dropped.

    private:
        struct State;
        std::unique_ptr<State> m_state;
        std::uint64_t start(CompleteExportDestination destination, QString path,
            CompleteExportInput input, ClipboardPublisher publish, Completion completion);
        void finishUi();
        void notifyUi();
    };
}
