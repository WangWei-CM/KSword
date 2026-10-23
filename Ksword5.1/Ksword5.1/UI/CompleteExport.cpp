#include "CompleteExport.h"
#include "AsyncOperation.h"

#include <QByteArray>
#include <QPointer>
#include <QSaveFile>
#include <QThread>

#include <algorithm>
#include <exception>
#include <condition_variable>
#include <thread>
#include <limits>
#include <map>
#include <mutex>

namespace ks::ui
{
    namespace complete_export_detail
    {
        namespace
        {
            struct RetiredOwner
            {
                std::shared_ptr<const void> value;
                RetiredOwner* next = nullptr;
            };

            // Process-lifetime value-only service: destruction of a GUI job or
            // result never joins a worker or frees the retained full document.
            // Queue links are allocated when the export input is prepared, so
            // enqueue itself neither allocates nor falls back to GUI deletion.
            class RetirementLane final
            {
            public:
                RetirementLane() { std::thread([this]() { run(); }).detach(); }
                void enqueue(RetiredOwner* owner) noexcept
                {
                    {
                        std::lock_guard lock(m_mutex);
                        if (m_tail) m_tail->next = owner;
                        else m_head = owner;
                        m_tail = owner;
                    }
                    m_ready.notify_one();
                }
            private:
                void run() noexcept
                {
                    for (;;)
                    {
                        RetiredOwner* batch;
                        {
                            std::unique_lock lock(m_mutex);
                            m_ready.wait(lock, [this]() { return m_head != nullptr; });
                            batch = std::exchange(m_head, nullptr);
                            m_tail = nullptr;
                        }
                        while (batch)
                        {
                            auto* next = batch->next;
                            delete batch; // The complete snapshot's last owner is released here.
                            batch = next;
                        }
                    }
                }
                std::mutex m_mutex;
                std::condition_variable m_ready;
                RetiredOwner* m_head = nullptr;
                RetiredOwner* m_tail = nullptr;
            };
            RetirementLane& retirementLane()
            {
                // Intentional process-lifetime service, independent of Qt
                // application teardown and thread-pool admission/saturation.
                static auto* const lane = new RetirementLane;
                return *lane;
            }
        }

        std::shared_ptr<const void> RetainValue(const std::shared_ptr<const void>& value)
        {
            if (!value) return {};
            auto& lane = retirementLane(); // Initialize before transferring any retained ownership.
            auto* owner = new RetiredOwner{ value };
            const auto retained = std::shared_ptr<RetiredOwner>(owner, [&lane](RetiredOwner* obsolete) noexcept
            { lane.enqueue(obsolete); });
            return std::shared_ptr<const void>(retained, value.get());
        }
    }

    bool CompleteExportReceipt::terminal() const
    {
        return phase == CompleteExportPhase::FileCommitted || phase == CompleteExportPhase::ClipboardPublished
            || phase == CompleteExportPhase::Failed || phase == CompleteExportPhase::NotAccepted
            || phase == CompleteExportPhase::ClipboardNotPublished || phase == CompleteExportPhase::OutcomeUnavailable;
    }

    bool CompleteExportReceipt::succeeded() const
    {
        return phase == CompleteExportPhase::FileCommitted || phase == CompleteExportPhase::ClipboardPublished;
    }

    namespace
    {
        CompleteExportReceipt initialReceipt(const CompleteExportInput& input,
            const CompleteExportDestination destination, const QString& path)
        {
            CompleteExportReceipt receipt;
            receipt.destination = destination;
            receipt.sourceIdentity = input.sourceIdentity;
            receipt.sourceVersion = input.sourceVersion;
            receipt.languageId = input.translator.languageId();
            receipt.outputPath = path;
            receipt.expectedRows = input.expectedRows;
            return receipt;
        }

        QString exceptionDetail()
        {
            try { throw; }
            catch (const std::exception& error) { return QString::fromUtf8(error.what()); }
            catch (...) { return {}; }
        }

        class SaveFileDevice final : public CompleteExportAtomicDevice
        {
        public:
            bool open(const QString& path, const CompleteExportFileMode mode) override
            {
                m_file.setFileName(path);
                m_file.setDirectWriteFallback(false);
                QIODevice::OpenMode flags = QIODevice::WriteOnly;
                if (mode == CompleteExportFileMode::NativeText) flags |= QIODevice::Text;
                return m_file.open(flags);
            }
            qint64 write(const char* bytes, const qint64 length) override { return m_file.write(bytes, length); }
            bool flush() override { return m_file.flush(); }
            bool commit() override { return m_file.commit(); }
            void cancel() override { m_file.cancelWriting(); }
            QString errorString() const override { return m_file.errorString(); }
        private:
            QSaveFile m_file;
        };

        class CopyDevice final : public CompleteExportAtomicDevice
        {
        public:
            bool open(const QString&, CompleteExportFileMode) override { return true; }
            qint64 write(const char* bytes, const qint64 length) override
            {
                m_bytes.append(bytes, static_cast<qsizetype>(length));
                return length;
            }
            bool flush() override { return true; }
            bool commit() override { return true; }
            void cancel() override { m_bytes.clear(); }
            QString errorString() const override { return {}; }
            QByteArray m_bytes;
        };
    }

    struct CompleteExportSink::State
    {
        CompleteExportAtomicDevice& device;
        CompleteExportReceipt& receipt;
    };

    CompleteExportSink::CompleteExportSink(State& state) : m_state(&state) {}

    bool CompleteExportSink::good() const { return m_state->receipt.error == CompleteExportError::None; }
    std::uint64_t CompleteExportSink::rows() const { return m_state->receipt.rows; }
    std::uint64_t CompleteExportSink::utf8Bytes() const { return m_state->receipt.utf8Bytes; }

    void CompleteExportSink::fail(const CompleteExportError error, QString detail)
    {
        if (good())
        {
            m_state->receipt.error = error == CompleteExportError::None ? CompleteExportError::FormatFailed : error;
            m_state->receipt.nativeError = std::move(detail);
        }
    }

    bool CompleteExportSink::writeUtf8(const QByteArrayView bytes)
    {
        if (!good()) return false;
        qsizetype offset = 0;
        while (offset < bytes.size())
        {
            const auto remaining = bytes.size() - offset;
            const auto written = m_state->device.write(bytes.data() + offset, static_cast<qint64>(remaining));
            if (written <= 0 || written > remaining)
            {
                fail(CompleteExportError::WriteFailed, m_state->device.errorString());
                return false;
            }
            if (static_cast<std::uint64_t>(written) > std::numeric_limits<std::uint64_t>::max() - utf8Bytes())
            {
                fail(CompleteExportError::WriteFailed);
                return false;
            }
            m_state->receipt.utf8Bytes += static_cast<std::uint64_t>(written);
            offset += static_cast<qsizetype>(written);
        }
        return true;
    }

    bool CompleteExportSink::writeText(const QStringView text)
    {
        constexpr qsizetype chunkSize = 32768;
        qsizetype offset = 0;
        while (offset < text.size() && good())
        {
            auto length = std::min(chunkSize, text.size() - offset);
            if (offset + length < text.size() && text[offset + length - 1].isHighSurrogate()
                && text[offset + length].isLowSurrogate()) --length;
            const auto bytes = text.sliced(offset, length).toUtf8();
            if (!writeUtf8(bytes)) return false;
            offset += length;
        }
        return good();
    }

    bool CompleteExportSink::rowsCompleted(const std::uint64_t count)
    {
        if (!good()) return false;
        if (count > std::numeric_limits<std::uint64_t>::max() - rows()
            || (m_state->receipt.expectedRows && (rows() > *m_state->receipt.expectedRows
                || count > *m_state->receipt.expectedRows - rows())))
        {
            fail(CompleteExportError::IncompleteRows);
            return false;
        }
        m_state->receipt.rows += count;
        return true;
    }

    struct CompleteExportEngine
    {
        static CompleteExportResult run(const CompleteExportInput& input, const QString& path,
            CompleteExportAtomicDevice& device, const CompleteExportDestination destination)
        {
            CompleteExportResult result;
            result.receipt = initialReceipt(input, destination, path);
            auto& receipt = result.receipt;
            receipt.phase = CompleteExportPhase::Running;
            CompleteExportSink::State state{ device, receipt };
            CompleteExportSink sink(state);
            try
            {
                if (!input.serialize || (destination == CompleteExportDestination::File && path.isEmpty()))
                    sink.fail(CompleteExportError::InvalidInput);
                else if (!device.open(path, destination == CompleteExportDestination::File
                    ? input.fileMode : CompleteExportFileMode::Binary))
                    sink.fail(CompleteExportError::OpenFailed, device.errorString());
                else
                {
                    if (destination == CompleteExportDestination::File && input.utf8Bom)
                        sink.writeUtf8(QByteArrayView("\xef\xbb\xbf", 3));
                    if (sink.good() && !input.serialize(sink, input.translator)) sink.fail(CompleteExportError::FormatFailed);
                    if (sink.good() && receipt.expectedRows && receipt.rows != *receipt.expectedRows)
                        sink.fail(CompleteExportError::IncompleteRows);
                    if (sink.good() && !device.flush()) sink.fail(CompleteExportError::FlushFailed, device.errorString());
                    if (sink.good() && !device.commit()) sink.fail(CompleteExportError::CommitFailed, device.errorString());
                }
            }
            catch (...)
            {
                sink.fail(CompleteExportError::UnexpectedFailure, exceptionDetail());
            }
            if (!sink.good())
            {
                device.cancel();
                receipt.phase = CompleteExportPhase::Failed;
            }
            else receipt.phase = destination == CompleteExportDestination::File
                ? CompleteExportPhase::FileCommitted : CompleteExportPhase::ClipboardReady;
            return result;
        }
    };

    CompleteExportResult RunCompleteExportFile(const CompleteExportInput& input,
        const QString& path, CompleteExportAtomicDevice& device)
    {
        return CompleteExportEngine::run(input, path, device, CompleteExportDestination::File);
    }

    CompleteExportResult RunCompleteExportCopy(const CompleteExportInput& input)
    {
        CopyDevice device;
        auto result = CompleteExportEngine::run(input, {}, device, CompleteExportDestination::Clipboard);
        if (result.receipt.phase == CompleteExportPhase::ClipboardReady)
        {
            try { result.clipboardText = std::make_shared<const QString>(QString::fromUtf8(device.m_bytes)); }
            catch (...)
            {
                result.receipt.phase = CompleteExportPhase::Failed;
                result.receipt.error = CompleteExportError::UnexpectedFailure;
                result.receipt.nativeError = exceptionDetail();
            }
        }
        return result;
    }

    struct CompleteExportRecord
    {
        mutable std::mutex mutex;
        CompleteExportReceipt value;
        CompleteExportReceipt get() const { std::lock_guard lock(mutex); return value; }
        void running() { std::lock_guard lock(mutex); value.phase = CompleteExportPhase::Running; }
        void completed(CompleteExportReceipt receipt)
        {
            std::lock_guard lock(mutex);
            receipt.id = value.id;
            value = std::move(receipt);
        }
        void settleDelivery(const AsyncOperationOutcome outcome)
        {
            std::lock_guard lock(mutex);
            if (value.terminal() || value.phase == CompleteExportPhase::ClipboardPublishing) return;
            if (value.phase == CompleteExportPhase::ClipboardReady)
                value.phase = CompleteExportPhase::ClipboardNotPublished;
            else if (value.phase == CompleteExportPhase::AwaitingAdmission)
            {
                if (outcome == AsyncOperationOutcome::BusyRejected || outcome == AsyncOperationOutcome::OwnerClosed)
                    value.phase = CompleteExportPhase::NotAccepted;
                else
                {
                    // An accepted launch can fail before the worker body starts.
                    value.phase = CompleteExportPhase::Failed;
                    value.error = CompleteExportError::UnexpectedFailure;
                }
            }
            else
            {
                // A delivery/allocation exception is not evidence of file write failure.
                value.phase = CompleteExportPhase::OutcomeUnavailable;
                value.error = CompleteExportError::ResultUnavailable;
            }
        }
        bool beginPublish()
        {
            std::lock_guard lock(mutex);
            if (value.phase != CompleteExportPhase::ClipboardReady) return false;
            value.phase = CompleteExportPhase::ClipboardPublishing;
            return true;
        }
        void published(const bool success)
        {
            std::lock_guard lock(mutex);
            value.phase = success ? CompleteExportPhase::ClipboardPublished : CompleteExportPhase::Failed;
            if (!success) value.error = CompleteExportError::ClipboardUnavailable;
        }
    };

    struct CompleteExportJournal::State
    {
        mutable std::mutex mutex;
        std::uint64_t nextId = 0;
        std::map<std::uint64_t, std::shared_ptr<CompleteExportRecord>> records;
    };

    CompleteExportJournal::CompleteExportJournal() : m_state(std::make_unique<State>()) {}
    CompleteExportJournal::~CompleteExportJournal() = default;

    std::shared_ptr<CompleteExportRecord> CompleteExportJournal::prepare(const CompleteExportInput& input,
        const CompleteExportDestination destination, const QString& path)
    {
        auto record = std::make_shared<CompleteExportRecord>();
        record->value = initialReceipt(input, destination, path);
        std::lock_guard lock(m_state->mutex);
        if (m_state->nextId == std::numeric_limits<std::uint64_t>::max()) return {};
        record->value.id = ++m_state->nextId;
        m_state->records.emplace(record->value.id, record);
        return record;
    }

    std::optional<CompleteExportReceipt> CompleteExportJournal::receipt(const std::uint64_t id) const
    {
        std::shared_ptr<CompleteExportRecord> record;
        {
            std::lock_guard lock(m_state->mutex);
            const auto found = m_state->records.find(id);
            if (found == m_state->records.end()) return {};
            record = found->second;
        }
        return record->get();
    }

    std::vector<CompleteExportReceipt> CompleteExportJournal::receipts() const
    {
        std::vector<std::shared_ptr<CompleteExportRecord>> records;
        {
            std::lock_guard lock(m_state->mutex);
            for (const auto& item : m_state->records) records.push_back(item.second);
        }
        std::vector<CompleteExportReceipt> result;
        result.reserve(records.size());
        for (const auto& record : records) result.push_back(record->get());
        return result;
    }

    bool CompleteExportJournal::acknowledge(const std::uint64_t id)
    {
        std::lock_guard lock(m_state->mutex);
        const auto found = m_state->records.find(id);
        if (found == m_state->records.end() || !found->second->get().terminal()) return false;
        m_state->records.erase(found);
        return true;
    }

    std::shared_ptr<CompleteExportJournal> SharedCompleteExportJournal()
    {
        static const auto journal = std::make_shared<CompleteExportJournal>();
        return journal;
    }

    struct CompleteExportOperation::State
    {
        std::shared_ptr<CompleteExportJournal> journal;
        std::unique_ptr<AsyncOperation> operation;
        std::shared_ptr<CompleteExportRecord> active;
        Completion completion;
        ClipboardPublisher publisher;
        std::function<void()> stateChanged;
        bool closed = false;
    };

    CompleteExportOperation::CompleteExportOperation(QObject* owner, std::shared_ptr<CompleteExportJournal> journal)
        : QObject(owner), m_state(std::make_unique<State>())
    {
        m_state->journal = journal ? std::move(journal) : SharedCompleteExportJournal();
        m_state->operation = std::make_unique<AsyncOperation>(this);
        m_state->operation->setStateChangedCallback([this]()
        {
            const QPointer<CompleteExportOperation> guard(this);
            reconcile();
            if (guard) notifyUi();
        });
        if (owner) QObject::connect(owner, &QObject::destroyed, this, [this]() { close(); });
    }

    CompleteExportOperation::~CompleteExportOperation() { close(); }

    std::uint64_t CompleteExportOperation::startFile(QString path, CompleteExportInput input, Completion completion)
    {
        return start(CompleteExportDestination::File, std::move(path), std::move(input), {}, std::move(completion));
    }

    std::uint64_t CompleteExportOperation::startCopy(CompleteExportInput input, ClipboardPublisher publish, Completion completion)
    {
        return start(CompleteExportDestination::Clipboard, {}, std::move(input), std::move(publish), std::move(completion));
    }

    std::uint64_t CompleteExportOperation::start(const CompleteExportDestination destination, QString path,
        CompleteExportInput input, ClipboardPublisher publish, Completion completion)
    {
        Q_ASSERT(QThread::currentThread() == thread());
        const QPointer<CompleteExportOperation> guard(this);
        reconcile(); // A recovered completion may open a modal, close, or begin a newer export.
        if (!guard || m_state->closed || isBusy() || !input.serialize
            || (destination == CompleteExportDestination::File && path.isEmpty())
            || (destination == CompleteExportDestination::Clipboard && !publish)) return 0;
        const auto record = m_state->journal->prepare(input, destination, path);
        if (!record) return 0;
        const auto id = record->get().id;
        struct Request
        {
            std::shared_ptr<const CompleteExportInput> input;
            QString path;
            CompleteExportDestination destination;
            std::shared_ptr<CompleteExportRecord> record;
        };
        m_state->active = record;
        m_state->completion = std::move(completion);
        m_state->publisher = std::move(publish);
        try
        {
            Request request{ std::make_shared<const CompleteExportInput>(std::move(input)), std::move(path), destination, record };
            const auto ticket = m_state->operation->submitTransaction<Request, CompleteExportResult>(std::move(request),
                [](const Request& value)
                {
                    value.record->running();
                    CompleteExportResult result;
                    if (value.destination == CompleteExportDestination::File)
                    {
                        SaveFileDevice device;
                        result = RunCompleteExportFile(*value.input, value.path, device);
                    }
                    else result = RunCompleteExportCopy(*value.input);
                    // Publish the truthful side-effect receipt before any Qt delivery/allocation.
                    result.receipt.id = value.record->get().id;
                    value.record->completed(result.receipt);
                    return result;
                },
                [guard, record](const CompleteExportResult& result, std::uint64_t)
                {
                    if (!guard || guard->m_state->active != record) return true;
                    if (result.clipboardText && record->beginPublish())
                    {
                        const auto publisher = guard->m_state->publisher;
                        bool published = false;
                        try { published = publisher && publisher(*result.clipboardText); } catch (...) {}
                        // Publisher may synchronously delete its page; this is value state only.
                        record->published(published);
                    }
                    return true;
                },
                [record](std::exception_ptr) { record->settleDelivery(AsyncOperationOutcome::Failed); },
                [record](const AsyncOperationOutcome outcome) { record->settleDelivery(outcome); });
            if (!ticket)
            {
                record->settleDelivery(AsyncOperationOutcome::BusyRejected);
                if (guard && m_state->active == record) finishUi();
                return 0;
            }
        }
        catch (...)
        {
            record->settleDelivery(AsyncOperationOutcome::BusyRejected);
            if (guard && m_state->active == record) finishUi();
            return 0;
        }
        return id; // No access to this after submit: state notification may have deleted it.
    }

    void CompleteExportOperation::close()
    {
        if (m_state->closed) return;
        m_state->closed = true;
        m_state->stateChanged = {};
        m_state->completion = {};
        m_state->publisher = {};
        m_state->operation->close();
        m_state->active.reset();
    }

    bool CompleteExportOperation::isBusy() const { return m_state->active || m_state->operation->isInFlight(); }
    std::optional<CompleteExportReceipt> CompleteExportOperation::currentReceipt() const
    {
        return m_state->active ? std::optional<CompleteExportReceipt>(m_state->active->get()) : std::nullopt;
    }
    std::shared_ptr<CompleteExportJournal> CompleteExportOperation::journal() const { return m_state->journal; }
    void CompleteExportOperation::setStateChangedCallback(std::function<void()> callback) { m_state->stateChanged = std::move(callback); }

    void CompleteExportOperation::reconcile()
    {
        if (!m_state->closed && m_state->active && !m_state->operation->isInFlight()
            && m_state->active->get().terminal()) finishUi();
    }

    void CompleteExportOperation::finishUi()
    {
        if (!m_state->active) return;
        const auto receipt = m_state->active->get();
        const auto completion = std::move(m_state->completion);
        m_state->active.reset();
        m_state->publisher = {};
        // Clear old identity before modal/reentrant/deleting user callbacks.
        if (completion && receipt.phase != CompleteExportPhase::NotAccepted)
        { try { completion(receipt); } catch (...) {} }
    }

    void CompleteExportOperation::notifyUi()
    {
        const auto callback = m_state->stateChanged;
        if (!m_state->closed && callback) { try { callback(); } catch (...) {} }
    }
}
