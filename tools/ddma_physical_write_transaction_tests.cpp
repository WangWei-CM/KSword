#include "DdmaPhysicalWriteTransaction.h"

#include <iostream>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace
{
    namespace backend = ksword::memory_backend;
    using Stage = backend::DdmaPhysicalWriteStage;
    using Result = backend::DdmaPhysicalWriteResult;
    using Outcome = backend::AccessOutcome;
    int checks = 0;
    int failures = 0;

    void expect(const bool condition, const std::string& message)
    {
        ++checks;
        if (!condition)
        {
            ++failures;
            std::cerr << "FAIL " << message << '\n';
        }
    }

    backend::DdmaPhysicalWriteRequest request()
    {
        backend::DdmaPhysicalWriteRequest value;
        value.generation = 7;
        value.snapshotAddress = 0x1000;
        value.targetAddress = 0x1002;
        value.original = QByteArray("01234567");
        value.edited = QByteArray("01AB4567");
        value.payload = QByteArray("AB");
        value.firstDiff = 2;
        return value;
    }

    Outcome readBytes(const QByteArray& bytes)
    {
        Outcome outcome;
        outcome.ok = true;
        outcome.data = bytes;
        outcome.bytesDone = static_cast<std::uint64_t>(bytes.size());
        return outcome;
    }

    Outcome writeBytes(const std::uint64_t count = 2)
    {
        Outcome outcome;
        outcome.ok = true;
        outcome.bytesDone = count;
        return outcome;
    }

    struct Step
    {
        Outcome outcome;
        int exception = 0; // 1 = std::exception; 2 = nonstandard exception.
        std::function<void()> entered;
    };

    struct FakeTransport
    {
        std::vector<Step> reads;
        std::vector<Step> writes;
        std::vector<std::string> operations;
        std::vector<bool> forceFlags;
        std::size_t nextRead = 0;
        std::size_t nextWrite = 0;
        int currentChecks = 0;
        bool current = true;

        Outcome execute(Step& step)
        {
            if (step.entered) { step.entered(); }
            if (step.exception == 1) { throw std::runtime_error("fake transport exception"); }
            if (step.exception == 2) { throw 17; }
            return step.outcome;
        }

        backend::DdmaPhysicalWritePort port()
        {
            return {
                [this] { ++currentChecks; return current; },
                [this] {
                    operations.emplace_back("read");
                    if (nextRead >= reads.size()) { throw std::logic_error("unexpected extra read"); }
                    return execute(reads[nextRead++]);
                },
                [this](const bool force) {
                    operations.emplace_back(force ? "write-force" : "write-normal");
                    forceFlags.push_back(force);
                    if (nextWrite >= writes.size()) { throw std::logic_error("unexpected extra write"); }
                    return execute(writes[nextWrite++]);
                }
            };
        }

        Result run(const backend::DdmaPhysicalWriteRequest& value, const bool force = false,
            const bool cancelled = false)
        {
            return backend::RunDdmaPhysicalWritePhase(value, port(), force, cancelled);
        }

        void expectOperations(std::vector<std::string> expected, const char* message)
        {
            expect(operations == expected, message);
        }
    };

    void orderedSuccess()
    {
        const auto value = request();
        FakeTransport fake;
        fake.reads = {{readBytes(value.original)}, {readBytes(value.edited)}};
        fake.writes = {{writeBytes()}};
        const auto result = fake.run(value);
        fake.expectOperations({"read", "write-normal", "read"}, "success order is exactly read/write/read");
        expect(result.stage == Stage::Finished, "success phase finishes");
        expect(result.before.data == value.original && result.actual.data == value.edited,
            "success retains complete original and complete actual snapshot");
        expect(result.writeAttempted && result.write.ok && result.write.bytesDone == 2,
            "success retains actual write receipt");
        expect(result.failureText.isEmpty() && !result.cancelled && !result.scratchDirty,
            "clean success has no hidden failure flags");
        expect(fake.currentChecks == 2 && fake.forceFlags == std::vector<bool>{false},
            "normal write checks freshness twice and never supplies force");
    }

    void preReadRejections()
    {
        const auto value = request();
        std::vector<Outcome> rejected;
        Outcome failed;
        failed.failureText = QStringLiteral("before read failed");
        rejected.push_back(failed);
        auto partial = readBytes(value.original);
        partial.partial = true;
        rejected.push_back(partial);
        rejected.push_back(readBytes(value.original.left(7)));
        rejected.push_back(readBytes(QByteArray("012X4567")));
        auto dirty = readBytes(value.original);
        dirty.scratchDirty = true;
        rejected.push_back(dirty);
        for (std::size_t index = 0; index < rejected.size(); ++index)
        {
            FakeTransport fake;
            fake.reads = {{rejected[index]}};
            const auto result = fake.run(value);
            fake.expectOperations({"read"}, "unusable/partial/short/changed/dirty before-read never writes");
            expect(result.stage == Stage::BeforeChanged && !result.writeAttempted,
                "pre-read rejection is explicit and never marks a write attempt");
            expect(result.actual.data == rejected[index].data && result.actual.ok == rejected[index].ok,
                "pre-read rejection preserves actual read outcome");
            expect(result.scratchDirty == rejected[index].scratchDirty,
                "pre-read rejection preserves scratch damage");
            expect(!result.write.ok && result.write.bytesDone == 0, "pre-read rejection cannot claim write success");
        }
    }

    void staleBoundaries()
    {
        const auto value = request();
        {
            FakeTransport fake;
            fake.current = false;
            const auto result = fake.run(value);
            fake.expectOperations({}, "stale-before-phase performs no transport operation");
            expect(result.stage == Stage::Stale && !result.writeAttempted && !result.write.ok,
                "stale-before-phase cannot claim a mutation");
        }
        {
            FakeTransport fake;
            fake.reads = {{readBytes(value.original), 0, [&] { fake.current = false; }}};
            const auto result = fake.run(value);
            fake.expectOperations({"read"}, "stale-between-before-read-and-write does not mutate");
            expect(result.stage == Stage::Stale && !result.writeAttempted && fake.currentChecks == 2,
                "second freshness check rejects the changed generation");
        }
        {
            FakeTransport fake;
            fake.reads = {{readBytes(value.original)}, {readBytes(value.edited)}};
            fake.writes = {{writeBytes(), 0, [&] { fake.current = false; }}};
            const auto result = fake.run(value);
            fake.expectOperations({"read", "write-normal", "read"},
                "generation change after write entry never suppresses final read");
            expect(result.stage == Stage::Finished && result.actual.data == value.edited
                && result.writeAttempted && fake.currentChecks == 2,
                "in-flight write finishes using its frozen request");
        }
    }

    Outcome forceRejected()
    {
        Outcome result;
        result.forceRequired = true;
        result.failureText = QStringLiteral("force required before mutation");
        return result;
    }

    void forceDecisionPhases()
    {
        const auto value = request();
        {
            FakeTransport fake;
            fake.reads = {{readBytes(value.original)}, {readBytes(value.original)}, {readBytes(value.edited)}};
            fake.writes = {{forceRejected()}, {writeBytes()}};
            const auto first = fake.run(value);
            fake.expectOperations({"read", "write-normal"}, "force-required phase stops without automatic force");
            expect(first.stage == Stage::NeedForce && first.writeAttempted && !first.write.ok
                && first.write.bytesDone == 0 && fake.forceFlags == std::vector<bool>{false},
                "safe force refusal is explicitly returned for the GUI decision");
            const auto second = fake.run(value, true);
            fake.expectOperations({"read", "write-normal", "read", "write-force", "read"},
                "approved force phase repeats full before-read then forced write then full readback");
            expect(second.stage == Stage::Finished && second.write.ok && second.actual.data == value.edited,
                "approved forced phase preserves verified actual data");
            expect(fake.forceFlags == std::vector<bool>({false, true}), "force comes only from explicit second-phase approval");
        }
        {
            FakeTransport fake;
            fake.reads = {{readBytes(QByteArray("012X4567"))}};
            const auto result = fake.run(value, true);
            fake.expectOperations({"read"}, "forced phase refuses target changed during GUI decision");
            expect(result.stage == Stage::BeforeChanged && !result.writeAttempted,
                "forced target mismatch remains a no-mutation rejection");
        }
        {
            FakeTransport fake;
            fake.reads = {{readBytes(value.original)}};
            const auto result = fake.run(value, false, true);
            fake.expectOperations({"read"}, "cancelled force phase performs only current full read");
            expect(result.stage == Stage::Finished && result.cancelled && !result.writeAttempted
                && result.actual.data == value.original && !result.write.ok,
                "cancellation preserves unchanged target and does not report a write");
            expect(value.edited == QByteArray("01AB4567"), "cancellation does not consume staged edited bytes");
        }
    }

    void unsafeForceRefusalsStillReadBack()
    {
        const auto value = request();
        std::vector<Outcome> unsafe;
        auto wrotePrefix = forceRejected();
        wrotePrefix.bytesDone = 1;
        unsafe.push_back(wrotePrefix);
        auto partial = forceRejected();
        partial.partial = true;
        unsafe.push_back(partial);
        auto partialWithPrefix = forceRejected();
        partialWithPrefix.partial = true;
        partialWithPrefix.bytesDone = 1;
        unsafe.push_back(partialWithPrefix);
        auto contradictorySuccess = forceRejected();
        contradictorySuccess.ok = true;
        unsafe.push_back(contradictorySuccess);
        auto dirty = forceRejected();
        dirty.scratchDirty = true;
        unsafe.push_back(dirty);
        for (const auto& receipt : unsafe)
        {
            FakeTransport fake;
            fake.reads = {{readBytes(value.original)}, {readBytes(QByteArray("01A34567"))}};
            fake.writes = {{receipt}};
            const auto result = fake.run(value);
            fake.expectOperations({"read", "write-normal", "read"},
                "partial/contradictory/dirty FORCE_REQUIRED still performs final read");
            expect(result.stage == Stage::Finished && result.writeAttempted,
                "possibly-mutating force refusal never enters GUI-force retry boundary");
            expect(result.write.bytesDone == receipt.bytesDone && result.write.partial == receipt.partial,
                "partial force receipt retains completed bytes and partial flag");
            expect(result.scratchDirty == receipt.scratchDirty && result.actual.data == QByteArray("01A34567"),
                "partial force receipt retains actual bytes and scratch damage");
        }
        FakeTransport approved;
        approved.reads = {{readBytes(value.original)}, {readBytes(value.original)}};
        approved.writes = {{forceRejected()}};
        const auto result = approved.run(value, true);
        approved.expectOperations({"read", "write-force", "read"}, "repeated FORCE_REQUIRED after approval is not auto-retried");
        expect(result.stage == Stage::Finished && !result.write.ok, "forced refusal remains a failed attempt");
    }

    void failedWritesAndReadbacks()
    {
        const auto value = request();
        std::vector<Outcome> writes;
        Outcome failed;
        failed.failureText = QStringLiteral("write failed");
        writes.push_back(failed);
        auto partial = writeBytes(1);
        partial.partial = true;
        writes.push_back(partial);
        auto dirty = writeBytes();
        dirty.scratchDirty = true;
        writes.push_back(dirty);
        for (const auto& receipt : writes)
        {
            FakeTransport fake;
            fake.reads = {{readBytes(value.original)}, {readBytes(value.edited)}};
            fake.writes = {{receipt}};
            const auto result = fake.run(value);
            fake.expectOperations({"read", "write-normal", "read"}, "failed/partial/dirty write always performs final read");
            expect(result.stage == Stage::Finished && result.writeAttempted && result.actual.data == value.edited,
                "failed write cannot abandon complete actual readback");
            expect(result.write.ok == receipt.ok && result.write.partial == receipt.partial
                && result.write.bytesDone == receipt.bytesDone && result.scratchDirty == receipt.scratchDirty,
                "final read cannot erase the original write failure/partial/damage evidence");
        }

        std::vector<Outcome> finalReads;
        Outcome failedRead;
        failedRead.failureText = QStringLiteral("final read failed");
        finalReads.push_back(failedRead);
        auto partialRead = readBytes(value.edited.left(4));
        partialRead.partial = true;
        finalReads.push_back(partialRead);
        auto dirtyRead = readBytes(value.edited);
        dirtyRead.scratchDirty = true;
        finalReads.push_back(dirtyRead);
        finalReads.push_back(readBytes(value.original));
        for (const auto& receipt : finalReads)
        {
            FakeTransport fake;
            fake.reads = {{readBytes(value.original)}, {receipt}};
            fake.writes = {{writeBytes()}};
            const auto result = fake.run(value);
            fake.expectOperations({"read", "write-normal", "read"}, "unsuccessful final verification does not retry write or read");
            expect(result.stage == Stage::Finished && result.actual.ok == receipt.ok
                && result.actual.partial == receipt.partial && result.actual.data == receipt.data,
                "final verification failure or mismatch remains observable");
            expect(result.scratchDirty == receipt.scratchDirty, "final read scratch damage is retained");
        }
    }

    void exceptionBoundaries()
    {
        const auto value = request();
        for (const int exception : {1, 2})
        {
            FakeTransport fake;
            fake.reads = {{readBytes(value.original)}, {readBytes(value.edited)}};
            fake.writes = {{Outcome{}, exception}};
            const auto result = fake.run(value);
            fake.expectOperations({"read", "write-normal", "read"}, "write exception still attempts exactly one final full read");
            expect(result.stage == Stage::Finished && result.writeAttempted && !result.failureText.isEmpty(),
                "write exception is an explicit failed/unknown mutation receipt");
            expect(!result.write.ok && result.actual.data == value.edited,
                "successful readback cannot turn a thrown write into reported write success");
        }
        for (const int exception : {1, 2})
        {
            FakeTransport fake;
            fake.reads = {{readBytes(value.original)}, {Outcome{}, exception}};
            fake.writes = {{writeBytes()}};
            const auto result = fake.run(value);
            fake.expectOperations({"read", "write-normal", "read"}, "final read exception is not an automatic read retry");
            expect(result.stage == Stage::Finished && result.writeAttempted && !result.failureText.isEmpty()
                && !result.actual.ok && result.actual.data.isEmpty(),
                "final read exception leaves unknown final state and explicit failure");
        }
        for (const int exception : {1, 2})
        {
            FakeTransport fake;
            fake.reads = {{Outcome{}, exception}};
            const auto result = fake.run(value);
            fake.expectOperations({"read"}, "pre-read exception never enters write or retry");
            expect(result.stage == Stage::Finished && !result.writeAttempted && !result.failureText.isEmpty()
                && !result.write.ok && !result.actual.ok,
                "pre-read exception cannot report successful mutation or verified final state");
        }
    }
}

int main()
{
    orderedSuccess();
    preReadRejections();
    staleBoundaries();
    forceDecisionPhases();
    unsafeForceRefusalsStillReadBack();
    failedWritesAndReadbacks();
    exceptionBoundaries();
    std::cout << (failures == 0 ? "PASS" : "FAIL") << " DdmaPhysicalWriteTransaction "
        << checks << " checks; " << failures << " failures\n";
    return failures == 0 ? 0 : 1;
}
