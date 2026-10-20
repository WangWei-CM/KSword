#pragma once

#include "MemoryAccessBackend.h"
#include <exception>
#include <functional>

namespace ksword::memory_backend
{
    // Value-only transaction stages. No QWidget, dialog, model or GUI wait.
    enum class DdmaPhysicalWriteStage { Stale, BeforeChanged, NeedForce, Finished };

    struct DdmaPhysicalWriteRequest
    {
        DdmaSession session;
        std::uint64_t generation = 0;
        std::uint64_t snapshotAddress = 0;
        std::uint64_t targetAddress = 0;
        QByteArray original;
        QByteArray edited;
        QByteArray payload;
        qsizetype firstDiff = 0;
    };

    struct DdmaPhysicalWriteResult
    {
        DdmaPhysicalWriteStage stage = DdmaPhysicalWriteStage::Stale;
        AccessOutcome before;
        AccessOutcome write;
        AccessOutcome actual;
        bool cancelled = false;
        bool scratchDirty = false;
        bool writeAttempted = false;
        QString failureText;
    };

    struct DdmaPhysicalWritePort
    {
        std::function<bool()> isCurrent;
        std::function<AccessOutcome()> readActual;
        std::function<AccessOutcome(bool)> write;
    };

    // FORCE is a GUI decision between two complete phases. The second phase
    // always rereads the complete original snapshot before any forced write.
    // Once write() was entered, cancellation/generation changes never suppress
    // the final physical readback. Scratch restore remains owned by the port.
    inline DdmaPhysicalWriteResult RunDdmaPhysicalWritePhase(
        const DdmaPhysicalWriteRequest& request, const DdmaPhysicalWritePort& port,
        const bool forceApproved, const bool cancelled)
    {
        DdmaPhysicalWriteResult result;
        result.cancelled = cancelled;
        bool finalReadAttempted = false;
        const auto readActual = [&]() {
            auto actual = port.readActual();
            result.scratchDirty = result.scratchDirty || actual.scratchDirty;
            return actual;
        };
        try
        {
            if (!port.isCurrent()) { return result; }
            result.before = readActual();
            if (!result.before.ok || result.before.partial || result.before.scratchDirty
                || result.before.data.size() != request.original.size()
                || result.before.data != request.original)
            {
                result.actual = result.before;
                result.stage = DdmaPhysicalWriteStage::BeforeChanged;
                return result;
            }
            if (cancelled)
            {
                result.actual = result.before;
                result.stage = DdmaPhysicalWriteStage::Finished;
                return result;
            }
            if (!port.isCurrent()) { return result; }
            result.writeAttempted = true;
            result.write = port.write(forceApproved);
            result.scratchDirty = result.scratchDirty || result.write.scratchDirty;
            if (!result.write.ok && result.write.forceRequired && !forceApproved
                && result.write.bytesDone == 0 && !result.write.partial && !result.write.scratchDirty)
            {
                result.stage = DdmaPhysicalWriteStage::NeedForce;
                return result; // Driver explicitly rejected before mutation.
            }
            finalReadAttempted = true;
            result.actual = readActual(); // Also after failed/partial writes.
            result.stage = DdmaPhysicalWriteStage::Finished;
            return result;
        }
        catch (const std::exception& error)
        {
            result.failureText = QString::fromUtf8(error.what());
        }
        catch (...)
        {
            result.failureText = QStringLiteral("DDMA transaction failed unexpectedly.");
        }
        // A transport exception may mean an unknown mutation outcome. Preserve
        // the frozen backup and attempt readback; never call it rollback.
        if (result.writeAttempted && !finalReadAttempted)
        {
            try { result.actual = readActual(); }
            catch (...) { /* The receipt retains unknown final state. */ }
        }
        result.stage = DdmaPhysicalWriteStage::Finished;
        return result;
    }
}
