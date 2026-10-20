#include "../../Ksword5.1/Ksword5.1/ksword/log/log.h"

#include <algorithm>
#include <atomic>
#include <iostream>
#include <stdexcept>
#include <thread>
#include <vector>

namespace
{
    void Check(const bool value, const char* message)
    {
        if (!value) throw std::runtime_error(message);
    }

    ks::log::Event MakeEvent(const int id, const ks::log::Level level = ks::log::Level::Info)
    {
        ks::log::Event event;
        event.guid.Data1 = static_cast<unsigned long>(id);
        event.level = level;
        event.content = std::to_string(id);
        return event;
    }

    void DeltaAndThreshold()
    {
        ks::log::LogEntry entry;
        for (int index = 0; index < 100000; ++index) entry.add(MakeEvent(index));
        const auto baseline = entry.TailCursor();
        Check(baseline.nextIndex == 100000, "tail baseline must not copy/replay history");
        entry.add(MakeEvent(100000, ks::log::Level::Debug));
        entry.add(MakeEvent(100001, ks::log::Level::Warn));
        entry.add(MakeEvent(100002, ks::log::Level::Error));
        const auto first = entry.SnapshotAfter(baseline, 2, static_cast<int>(ks::log::Level::Warn));
        Check(first.events.size() == 1 && first.events[0].content == "100001", "threshold excludes low levels");
        Check(first.cursor.nextIndex == 100002 && first.hasMore, "cursor advances across excluded rows with scan budget");
        const auto second = entry.SnapshotAfter(first.cursor, 2, 0);
        Check(second.events.size() == 1 && second.events[0].content == "100002", "lowering threshold never replays consumed rows");
        Check(!second.hasMore && second.cursor.nextIndex == 100003, "batch drains exactly the new suffix");
        const auto idle = entry.SnapshotAfter(second.cursor, 128, 0);
        Check(idle.events.empty() && !idle.hasMore, "idle polls are empty");
        const auto all = entry.Snapshot();
        Check(all.size() == 100003, "notification delta must not truncate full archive");
        const auto tracked = entry.Track(all[100001].guid);
        Check(tracked.size() == 1 && tracked[0].content == "100001", "legacy tracking remains full and exact");
    }

    void EpochAndEnableBoundary()
    {
        ks::log::LogEntry entry;
        entry.add(MakeEvent(1));
        const auto old = entry.TailCursor();
        const auto oldSequence = entry.Snapshot().back().recordSequence;
        entry.clear();
        entry.add(MakeEvent(2));
        entry.add(MakeEvent(3));
        const auto cleared = entry.SnapshotAfter(old, 128, 0);
        Check(cleared.events.empty() && cleared.cursor.epoch != old.epoch && cleared.cursor.nextIndex == 2,
            "clear skips replaced history even when its count exceeds previous history");
        entry.add(MakeEvent(4));
        const auto fresh = entry.SnapshotAfter(cleared.cursor, 128, 0);
        Check(fresh.events.size() == 1 && fresh.events[0].content == "4", "new epoch delivers subsequent records");
        Check(fresh.events[0].recordSequence > oldSequence, "stable model keys never repeat after clear");
        entry.add(MakeEvent(5));
        const auto reenabled = entry.TailCursor();
        entry.add(MakeEvent(6));
        const auto enabled = entry.SnapshotAfter(reenabled, 128, 0);
        Check(enabled.events.size() == 1 && enabled.events[0].content == "6", "reenabling uses atomic tail baseline");
        const auto zero = entry.SnapshotAfter(reenabled, 0, 0);
        Check(zero.events.empty() && zero.cursor.nextIndex == reenabled.nextIndex && zero.hasMore,
            "zero budget does not consume records");
    }

    std::vector<ks::log::Event> ReferenceRecent(
        const std::vector<ks::log::Event>& archive, const std::size_t limit,
        const std::uint32_t mask, const GUID* guid)
    {
        std::vector<ks::log::Event> expected;
        if (limit == 0) return expected;
        for (auto row = archive.rbegin(); row != archive.rend(); ++row)
        {
            const auto level = static_cast<unsigned int>(row->level);
            if (level >= 32 || (mask & (std::uint32_t{1} << level)) == 0) continue;
            if (guid != nullptr && !ks::log::IsSameGuid(*guid, row->guid)) continue;
            expected.push_back(*row);
            if (expected.size() == limit) break;
        }
        std::reverse(expected.begin(), expected.end());
        return expected;
    }

    void CompareRows(const std::vector<ks::log::Event>& actual, const std::vector<ks::log::Event>& expected)
    {
        Check(actual.size() == expected.size(), "indexed recent count differs from full archive reference");
        for (std::size_t index = 0; index < actual.size(); ++index)
        {
            Check(actual[index].recordSequence == expected[index].recordSequence
                && actual[index].content == expected[index].content
                && ks::log::IsSameGuid(actual[index].guid, expected[index].guid),
                "indexed recent ordering or identity differs from full archive reference");
        }
    }

    void IndexedQueriesAndTokens()
    {
        ks::log::LogEntry entry;
        for (int index = 0; index < 8000; ++index)
        {
            auto event = MakeEvent(index, static_cast<ks::log::Level>((index * 7) % 5));
            event.guid.Data1 = static_cast<unsigned long>(index % 37);
            event.guid.Data4[7] = static_cast<unsigned char>((index / 37) % 3);
            entry.add(std::move(event));
        }
        auto highLevel = MakeEvent(9000, static_cast<ks::log::Level>(31));
        entry.add(highLevel);
        entry.add(MakeEvent(9001, static_cast<ks::log::Level>(32)));
        entry.add(MakeEvent(9002, static_cast<ks::log::Level>(-1)));
        const auto archive = entry.Snapshot();
        const GUID known = archive[9].guid;
        GUID absent{};
        absent.Data1 = 0xffffffffUL;
        for (const auto mask : {0U, 1U, 2U, 3U, 7U, 8U, 16U, 24U, 31U, 0x80000000U, 0xffffffffU})
        {
            for (const std::size_t limit : {std::size_t{0}, std::size_t{1}, std::size_t{17}, std::size_t{10000}})
            {
                for (const GUID* guid : {static_cast<const GUID*>(nullptr), &known, static_cast<const GUID*>(&absent)})
                {
                    CompareRows(entry.SnapshotRecent(limit, mask, guid), ReferenceRecent(archive, limit, mask, guid));
                }
            }
        }

        auto current = entry.SnapshotRecentIfChanged(200, 31, &known, nullptr);
        Check(current.changed && !current.events.empty(), "first projection must return matching rows");
        for (int index = 0; index < 10000; ++index) entry.add(MakeEvent(20000 + index));
        const auto unrelated = entry.SnapshotRecentIfChanged(200, 31, &known, &current.token);
        Check(!unrelated.changed && unrelated.events.empty(), "unrelated GUID appends must not copy or rebuild old projection");
        auto matching = MakeEvent(40000, ks::log::Level::Error);
        matching.guid = known;
        entry.add(matching);
        current = entry.SnapshotRecentIfChanged(200, 31, &known, &current.token);
        Check(current.changed && current.events.back().content == "40000", "matching append must update old GUID projection");
        const auto limitChanged = entry.SnapshotRecentIfChanged(201, 31, &known, &current.token);
        Check(limitChanged.changed, "display limit belongs to query token even when newest match is unchanged");
        const auto scopeChanged = entry.SnapshotRecentIfChanged(200, 31, nullptr, &current.token);
        Check(scopeChanged.changed, "tracking scope belongs to query token");
        const auto levelChanged = entry.SnapshotRecentIfChanged(200, 16, &known, &current.token);
        Check(levelChanged.changed, "level mask belongs to query token");

        auto rare = entry.SnapshotRecentIfChanged(200, 16, nullptr, nullptr);
        for (int index = 0; index < 1000; ++index) entry.add(MakeEvent(50000 + index));
        const auto excluded = entry.SnapshotRecentIfChanged(200, 16, nullptr, &rare.token);
        Check(!excluded.changed && excluded.events.empty(), "excluded severity appends must not rebuild sparse projection");
        const auto noLevels = entry.SnapshotRecentIfChanged(200, 0, nullptr, nullptr);
        entry.add(MakeEvent(60000, ks::log::Level::Fatal));
        Check(!entry.SnapshotRecentIfChanged(200, 0, nullptr, &noLevels.token).changed,
            "all-levels-disabled projection remains unchanged on writes");
        entry.clear();
        entry.add(matching);
        const auto cleared = entry.SnapshotRecentIfChanged(200, 31, &known, &current.token);
        Check(cleared.changed && cleared.token.epoch != current.token.epoch && cleared.events.size() == 1,
            "clear must retire every old index and invalidate projection tokens");
        Check(entry.SnapshotRecent(100, 0xffffffffU).size() == 1, "no stale index remains after clear");
    }

    void ConcurrentIndexedQueries()
    {
        ks::log::LogEntry entry;
        GUID tracked{};
        tracked.Data1 = 17;
        std::atomic<bool> done{false};
        std::thread writer([&entry, &done, tracked]()
        {
            for (int index = 0; index < 10000; ++index)
            {
                auto event = MakeEvent(index, static_cast<ks::log::Level>(index % 5));
                if (index % 3 == 0) event.guid = tracked;
                entry.add(std::move(event));
            }
            done = true;
        });
        ks::log::RecentSnapshotToken token;
        do
        {
            const auto snapshot = entry.SnapshotRecentIfChanged(23, 24, &tracked, &token);
            token = snapshot.token;
            Check(snapshot.events.size() <= 23, "concurrent indexed query respects recent N");
            std::uint64_t previous = 0;
            for (const auto& event : snapshot.events)
            {
                Check(ks::log::IsSameGuid(event.guid, tracked), "concurrent query cannot mix GUID indexes");
                Check(event.level == ks::log::Level::Error || event.level == ks::log::Level::Fatal,
                    "concurrent query cannot mix severity indexes");
                Check(event.recordSequence > previous, "concurrent indexed results remain chronological");
                previous = event.recordSequence;
            }
        } while (!done.load());
        writer.join();
        CompareRows(entry.SnapshotRecent(100, 24, &tracked), ReferenceRecent(entry.Snapshot(), 100, 24, &tracked));
    }

    void ConcurrentWritersAndClear()
    {
        ks::log::LogEntry entry;
        auto cursor = entry.TailCursor();
        std::atomic<int> writersRemaining{4};
        std::vector<std::thread> writers;
        for (int writer = 0; writer < 4; ++writer)
        {
            writers.emplace_back([&entry, &writersRemaining, writer]()
            {
                for (int index = 0; index < 2500; ++index) entry.add(MakeEvent(writer * 2500 + index));
                --writersRemaining;
            });
        }
        std::uint64_t previousSequence = 0;
        std::size_t consumed = 0;
        for (;;)
        {
            const auto batch = entry.SnapshotAfter(cursor, 37, 0);
            cursor = batch.cursor;
            Check(batch.events.size() <= 37, "delta scan/copy respects budget");
            for (const auto& event : batch.events)
            {
                Check(event.recordSequence > previousSequence, "concurrent records must be delivered once in archive order");
                previousSequence = event.recordSequence;
                ++consumed;
            }
            if (writersRemaining.load() == 0 && batch.events.empty() && !batch.hasMore
                && cursor.nextIndex == entry.TailCursor().nextIndex) break;
            std::this_thread::yield();
        }
        for (auto& writer : writers) writer.join();
        Check(consumed == 10000 && entry.Snapshot().size() == 10000, "all concurrent records must remain archived and delivered");

        std::atomic<bool> done{false};
        std::thread clearer([&entry, &done]()
        {
            for (int epoch = 0; epoch < 100; ++epoch)
            {
                entry.clear();
                for (int index = 0; index < 30; ++index) entry.add(MakeEvent(index));
            }
            done = true;
        });
        auto seenEpoch = cursor.epoch;
        previousSequence = 0;
        do
        {
            const auto batch = entry.SnapshotAfter(cursor, 13, 0);
            if (batch.cursor.epoch != seenEpoch)
            {
                Check(batch.events.empty(), "stale epoch must not replay replacement history");
                previousSequence = 0;
                seenEpoch = batch.cursor.epoch;
            }
            for (const auto& event : batch.events)
            {
                Check(event.recordSequence > previousSequence, "per-epoch cursor remains ordered during clear race");
                previousSequence = event.recordSequence;
            }
            cursor = batch.cursor;
        } while (!done.load());
        clearer.join();
        Check(entry.Snapshot().size() == 30, "clear retains complete final epoch");
    }
}

int main()
{
    try
    {
        DeltaAndThreshold();
        EpochAndEnableBoundary();
        ConcurrentWritersAndClear();
        IndexedQueriesAndTokens();
        ConcurrentIndexedQueries();
        std::cout << "PASS log archive: bounded delta, threshold, clear epochs, tail baseline, concurrent writers/clear, indexed/reference equivalence, sparse GUID/level tokens, concurrent indexed queries\n";
        return 0;
    }
    catch (const std::exception& error)
    {
        std::cerr << "FAIL: " << error.what() << '\n';
        return 1;
    }
}
