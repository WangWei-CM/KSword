#include "../Ksword5.1/Ksword5.1/UI/MemoryEditHistory.Core.h"

#include <algorithm>
#include <cstdlib>
#include <iostream>
#include <random>

using History = ks::ui::detail::MemoryEditHistory;
namespace
{
    unsigned checks = 0;
    void require(bool condition, const char* description)
    {
        ++checks;
        if (condition) return;
        std::cerr << "FAIL: " << description << '\n';
        std::exit(1);
    }
}

int main()
{
    History history;
    const History::Bytes original{1,2,3,4,5,6,7,8};
    History::Bytes current = original;
    current[0] = 9; current[1] = 10; current[6] = 11;
    require(history.record(original, current) == History::RecordResult::Recorded, "batch edit records");
    require(history.undoCount() == 1 && history.redoCount() == 0, "one refresh is one transaction");
    const auto firstUndo = history.undo(current);
    require(firstUndo && *firstUndo == original, "undo sparse and contiguous ranges atomically");
    require(history.canRedo() && !history.canUndo(), "cursor after undo");
    require(history.record(original, original) == History::RecordResult::Unchanged && history.canRedo(),
        "no-op preserves redo");
    require(history.redo(*firstUndo) == current, "redo exact bytes");
    auto mismatch = current;
    mismatch[6] = 99;
    require(!history.undo(mismatch) && history.canUndo(), "mismatch rejects atomically without moving cursor");
    require(!history.undo(History::Bytes{1,2}), "length mismatch rejects");
    require(history.undo(current) == original, "failed playback preserves valid transaction");
    auto replacement = original;
    replacement[4] = 42;
    require(history.record(original.data(), replacement.data(), original.size()) == History::RecordResult::Recorded,
        "pointer API avoids full snapshot copy");
    require(!history.canRedo() && history.undoCount() == 1, "new edit branches and removes redo");
    require(history.undo(replacement.data(), replacement.size()) == original, "pointer undo");
    require(history.redo(original.data(), original.size()) == replacement, "pointer redo");
    const auto retainedCost = history.memoryUsage();
    require(retainedCost > 0 && retainedCost <= 32U * 1024U * 1024U, "accounted memory bounded");
    history.reset();
    require(!history.canUndo() && !history.canRedo() && !history.memoryUsage(), "reset clears all history");

    History countBounded(4096, 2);
    History::Bytes countCurrent(16, 0);
    for (unsigned i = 1; i <= 3; ++i)
    {
        auto next = countCurrent;
        next[0] = static_cast<std::uint8_t>(i);
        require(countBounded.record(countCurrent, next) == History::RecordResult::Recorded, "record bounded count");
        countCurrent = std::move(next);
    }
    require(countBounded.undoCount() == 2, "oldest command evicted at count limit");
    countCurrent = *countBounded.undo(countCurrent);
    countCurrent = *countBounded.undo(countCurrent);
    require(countCurrent[0] == 1 && !countBounded.canUndo(), "eviction retains nearest valid history");

    History byteBounded(retainedCost, 256);
    History::Bytes byteCurrent(16, 0);
    for (unsigned i = 1; i <= 4; ++i)
    {
        auto next = byteCurrent;
        next[0] = static_cast<std::uint8_t>(i);
        require(byteBounded.record(byteCurrent, next) == History::RecordResult::Recorded, "record byte bounded");
        require(byteBounded.memoryUsage() <= retainedCost && byteBounded.undoCount() == 1,
            "byte bound evicts earlier changes");
        byteCurrent = std::move(next);
    }
    const History::Bytes largeBefore(1024, 0), largeAfter(1024, 1);
    require(byteBounded.record(largeBefore, largeAfter) == History::RecordResult::CapacityExceeded,
        "oversized transaction rejected before retaining payload");
    require(!byteBounded.canUndo() && !byteBounded.canRedo() && !byteBounded.memoryUsage(),
        "oversized edit invalidates stale timeline");
    require(largeAfter.front() == 1 && largeAfter.back() == 1, "edited buffer never discarded");
    History disabled(4096, 0);
    require(disabled.record(original, replacement) == History::RecordResult::CapacityExceeded, "zero command bound");
    require(history.record(original, replacement) == History::RecordResult::Recorded, "record before size reset");
    require(history.record(original, History::Bytes{1}) == History::RecordResult::SizeMismatch
        && !history.canUndo(), "size change resets timeline");
    require(history.record(nullptr, original.data(), original.size()) == History::RecordResult::InvalidInput,
        "invalid pointer rejects");
    require(history.record(nullptr, nullptr, 0) == History::RecordResult::Unchanged, "empty input is no-op");

    // Random batches test the actual timeline against complete reference snapshots.
    History randomHistory(2U * 1024U * 1024U, 256);
    std::mt19937 random(42);
    std::vector<History::Bytes> snapshots{History::Bytes(4096, 0)};
    for (unsigned transaction = 0; transaction < 128; ++transaction)
    {
        auto next = snapshots.back();
        for (unsigned byte = 0; byte < 8; ++byte)
        {
            const auto offset = random() % next.size();
            next[offset] ^= static_cast<std::uint8_t>((random() % 255) + 1);
        }
        require(randomHistory.record(snapshots.back(), next) == History::RecordResult::Recorded,
            "random transaction records");
        snapshots.push_back(std::move(next));
    }
    auto randomCurrent = snapshots.back();
    for (std::size_t i = snapshots.size() - 1; i; --i)
    {
        const auto restored = randomHistory.undo(randomCurrent);
        require(restored && *restored == snapshots[i - 1], "random undo equals reference snapshot");
        randomCurrent = *restored;
    }
    for (std::size_t i = 1; i < snapshots.size(); ++i)
    {
        const auto restored = randomHistory.redo(randomCurrent);
        require(restored && *restored == snapshots[i], "random redo equals reference snapshot");
        randomCurrent = *restored;
    }
    History spanHistory;
    auto spanBefore = History::Bytes(4096, 0);
    auto spanAfter = spanBefore;
    spanAfter[4093] = 1; spanAfter[4095] = 2;
    require(spanHistory.recordRange(spanBefore.data(), spanAfter.data(), spanAfter.size(), 4093, 3)
        == History::RecordResult::Recorded, "range edit records absolute tail offsets");
    require(spanHistory.undo(spanAfter) == spanBefore, "range undo checks full snapshot identity and tail bytes");
    require(spanHistory.redo(spanBefore) == spanAfter, "range redo preserves unchanged bytes outside span");
    require(spanHistory.recordRange(spanAfter.data(), spanAfter.data(), spanAfter.size(), 4096, 1)
        == History::RecordResult::InvalidInput, "invalid range cannot wrap");
    require(spanHistory.recordRange(spanAfter.data(), spanAfter.data(), spanAfter.size(), 4096, 0)
        == History::RecordResult::Unchanged, "empty end span preserves history");
    require(spanHistory.recordRange(nullptr, spanAfter.data(), spanAfter.size(), 0, 1)
        == History::RecordResult::InvalidInput, "range null source rejects without changing history");
    History randomRanges(2U * 1024U * 1024U, 256);
    std::vector<History::Bytes> rangeSnapshots{History::Bytes(4096, 0)};
    for (unsigned transaction = 0; transaction < 128; ++transaction)
    {
        auto next = rangeSnapshots.back();
        const auto offset = random() % next.size();
        const auto count = (std::min)(static_cast<std::size_t>(1 + random() % 64), next.size() - offset);
        for (auto at = offset; at < offset + count; ++at)
            if (at == offset || (random() & 1U)) next[at] ^= static_cast<std::uint8_t>(1 + random() % 255);
        require(randomRanges.recordRange(rangeSnapshots.back().data(), next.data(), next.size(), offset, count)
            == History::RecordResult::Recorded, "random sparse span records one transaction");
        rangeSnapshots.push_back(std::move(next));
    }
    auto rangeCurrent = rangeSnapshots.back();
    for (std::size_t at = rangeSnapshots.size() - 1; at; --at)
    {
        const auto restored = randomRanges.undo(rangeCurrent);
        require(restored && *restored == rangeSnapshots[at - 1], "random range undo equals full reference snapshot");
        rangeCurrent = *restored;
    }
    for (std::size_t at = 1; at < rangeSnapshots.size(); ++at)
    {
        const auto restored = randomRanges.redo(rangeCurrent);
        require(restored && *restored == rangeSnapshots[at], "random range redo equals full reference snapshot");
        rangeCurrent = *restored;
    }
    History::DifferenceSummary summary;
    auto baseline = History::Bytes(257, 0);
    auto observed = baseline;
    for (unsigned transaction = 0; transaction < 4000; ++transaction)
    {
        auto next = observed;
        const auto offset = random() % next.size();
        const auto count = (std::min)(static_cast<std::size_t>(1 + random() % 32), next.size() - offset);
        for (auto at = offset; at < offset + count; ++at)
            next[at] = static_cast<std::uint8_t>(random() % 3);
        const auto incremental = History::updateDifferenceSummary(summary, baseline.data(), observed.data(),
            next.data(), next.size(), offset, count);
        History::DifferenceSummary reference;
        for (std::size_t at = 0; at < next.size(); ++at)
        {
            if (next[at] == baseline[at]) continue;
            ++reference.bytes;
            if (!at || next[at - 1] == baseline[at - 1]) ++reference.runs;
        }
        require(incremental && incremental->bytes == reference.bytes && incremental->runs == reference.runs,
            "incremental run/byte summary equals independent full scan after merge/split/tail edits");
        summary = *incremental;
        observed = std::move(next);
    }
    require(!History::updateDifferenceSummary(summary, baseline.data(), observed.data(), observed.data(),
        observed.size(), observed.size(), 1), "summary rejects overflowing span");
    std::cout << "PASS: " << checks << " memory history checks\n";
}
