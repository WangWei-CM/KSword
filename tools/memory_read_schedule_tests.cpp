#include "../shared/evidence/memory_workbench/MemoryReadSchedule.h"

#include <cstdlib>
#include <iostream>
#include <limits>
#include <string>
#include <unordered_set>
#include <vector>

namespace
{
    using Schedule = ksword::memwb::MemoryReadSchedule;
    using Purpose = Schedule::Purpose;
    using Reason = Schedule::RejectReason;
    using Ticket = Schedule::Ticket;
    std::size_t assertions = 0;

    void check(bool condition, const char* expression, int line)
    {
        ++assertions;
        if (!condition)
        {
            std::cerr << "FAIL line " << line << ": " << expression << '\n';
            std::exit(1);
        }
    }

#define CHECK(expression) check(static_cast<bool>(expression), #expression, __LINE__)

    void emptyAndIdentity()
    {
        Schedule schedule;
        CHECK(schedule.maxBrowsePages() == 512);
        CHECK(schedule.interestRevision() == 0);
        CHECK(!schedule.popNext());
        CHECK(!schedule.finish(1));
        CHECK(schedule.clear().empty());
        CHECK(!schedule.contains(0));
        CHECK(schedule.enqueue(0, Purpose::Browse, 1).reason == Reason::InvalidTicket);
        CHECK(schedule.enqueue(1, static_cast<Purpose>(999), 1).reason == Reason::InvalidPurpose);
        CHECK(schedule.enqueue(1, Purpose::Browse, 0).reason == Reason::InvalidPageCount);
        CHECK(schedule.enqueue(1, Purpose::Browse, 1, 1).reason == Reason::StaleInterest);
        CHECK(schedule.enqueue(1, Purpose::Browse, 1).accepted);
        CHECK(schedule.enqueue(1, Purpose::ExplicitRetry, 1).reason == Reason::DuplicateTicket);
        CHECK(schedule.enqueue(1, Purpose::Browse, 2).reason == Reason::DuplicateTicket);
        auto request = schedule.popNext();
        CHECK(request && request->ticket == 1);
        CHECK(request->purpose == Purpose::Browse && request->pageCount == 1);
        CHECK(!schedule.popNext());
        CHECK(schedule.contains(1));
        CHECK(!schedule.finish(2));
        CHECK(schedule.stats().activeRequests == 1);
        CHECK(schedule.finish(1));
        CHECK(!schedule.finish(1));
        CHECK(!schedule.contains(1));
        CHECK(schedule.enqueue(1, Purpose::ExplicitRead, 3).accepted);
        CHECK(schedule.popNext()->ticket == 1);
        CHECK(schedule.finish(1));
        CHECK(schedule.stats().totalOutstandingPages == 0);
        CHECK(schedule.stats().accepted == 2);
        CHECK(schedule.stats().rejected == 6);
        CHECK(schedule.stats().finished == 2);
    }

    void slowBackendThousandScrolls()
    {
        Schedule schedule;
        CHECK(schedule.enqueue(1, Purpose::Browse, 256).accepted);
        CHECK(schedule.popNext()->ticket == 1);
        Ticket previousPending = 0;
        std::unordered_set<Ticket> cancelled;
        for (std::uint64_t revision = 1; revision <= 1000; ++revision)
        {
            const auto dropped = schedule.updateInterest(revision);
            CHECK(dropped.size() == (previousPending == 0 ? 0u : 1u));
            if (previousPending != 0)
            {
                CHECK(dropped.front() == previousPending);
                CHECK(cancelled.insert(dropped.front()).second);
                CHECK(!schedule.contains(previousPending));
            }
            const Ticket current = revision + 1;
            const auto result = schedule.enqueue(current, Purpose::Browse, 256, revision);
            CHECK(result.accepted && result.droppedBrowseTickets.empty());
            CHECK(schedule.stats().activeBrowsePages == 256);
            CHECK(schedule.stats().pendingBrowsePages == 256);
            CHECK(schedule.stats().outstandingBrowsePages == 512);
            CHECK(schedule.stats().totalOutstandingPages == 512);
            CHECK(schedule.stats().activeRequests == 1);
            CHECK(schedule.stats().queuedBrowseRequests == 1);
            CHECK(!schedule.popNext()); // blocked physical backend still owns it
            previousPending = current;
        }
        CHECK(cancelled.size() == 999);
        CHECK(schedule.stats().peakOutstandingBrowsePages == 512);
        CHECK(schedule.finish(1));
        const auto next = schedule.popNext();
        CHECK(next && next->ticket == 1001 && next->interestRevision == 1000);
        CHECK(schedule.finish(next->ticket));
        CHECK(!schedule.popNext());
        CHECK(schedule.stats().droppedBrowse == 999);
        CHECK(schedule.stats().finished == 2);
    }

    void replacePendingAndBudgetRejection()
    {
        Schedule schedule(256);
        CHECK(schedule.enqueue(10, Purpose::Browse, 100).accepted);
        CHECK(schedule.popNext()->ticket == 10);
        CHECK(schedule.enqueue(11, Purpose::Browse, 100).accepted);
        const auto replaced = schedule.enqueue(12, Purpose::Browse, 156);
        CHECK(replaced.accepted);
        CHECK(replaced.droppedBrowseTickets == std::vector<Ticket>{11});
        CHECK(!schedule.contains(11) && schedule.contains(12));
        CHECK(schedule.stats().outstandingBrowsePages == 256);
        const auto rejected = schedule.enqueue(13, Purpose::Browse, 157);
        CHECK(!rejected.accepted && rejected.reason == Reason::BrowseBudget);
        CHECK(rejected.droppedBrowseTickets.empty());
        CHECK(schedule.contains(12) && !schedule.contains(13));
        CHECK(schedule.stats().pendingBrowsePages == 156);
        CHECK(schedule.enqueue(14, Purpose::ExplicitRead, 4096).accepted);
        CHECK(schedule.stats().outstandingBrowsePages == 256);
        CHECK(schedule.stats().totalOutstandingPages == 4352);
        CHECK(schedule.finish(10));
        CHECK(schedule.popNext()->ticket == 14);
        CHECK(schedule.finish(14));
        CHECK(schedule.popNext()->ticket == 12);
        CHECK(schedule.finish(12));
        Schedule noBrowse(0);
        CHECK(noBrowse.enqueue(1, Purpose::Browse, 1).reason == Reason::BrowseBudget);
        CHECK(noBrowse.enqueue(2, Purpose::PostWrite, 1).accepted);
        CHECK(noBrowse.popNext()->ticket == 2);
    }

    void protectedFifoAndFairness()
    {
        Schedule schedule;
        std::vector<Ticket> protectedOrder;
        for (Ticket i = 1; i <= 3000; ++i)
        {
            const Purpose purpose = i % 3 == 0 ? Purpose::ExplicitRead
                : i % 3 == 1 ? Purpose::PostWrite : Purpose::ExplicitRetry;
            CHECK(schedule.enqueue(i, purpose, 1).accepted);
            protectedOrder.push_back(i);
        }
        CHECK(schedule.stats().queuedProtectedRequests == 3000);
        CHECK(schedule.stats().queuedProtectedPages == 3000);
        CHECK(schedule.stats().totalOutstandingPages == 3000);
        Ticket nextBrowse = 10000;
        CHECK(schedule.enqueue(nextBrowse, Purpose::Browse, 256).accepted);
        std::size_t protectedSeen = 0;
        std::size_t browseSeen = 0;
        std::size_t protectedSinceBrowse = 0;
        std::unordered_set<Ticket> started;
        while (protectedSeen != protectedOrder.size())
        {
            const auto next = schedule.popNext();
            CHECK(next.has_value());
            CHECK(started.insert(next->ticket).second);
            CHECK(!schedule.popNext());
            if (next->purpose == Purpose::Browse)
            {
                ++browseSeen;
                CHECK(protectedSinceBrowse == 2);
                protectedSinceBrowse = 0;
                // Maintain demand in both lanes. Browsing cannot continuously
                // overtake the accepted protected FIFO either.
                CHECK(schedule.enqueue(++nextBrowse, Purpose::Browse, 256).accepted);
            }
            else
            {
                CHECK(next->ticket == protectedOrder[protectedSeen]);
                ++protectedSeen;
                ++protectedSinceBrowse;
                CHECK(protectedSinceBrowse <= 2);
            }
            CHECK(schedule.stats().outstandingBrowsePages <= 512);
            CHECK(schedule.finish(next->ticket));
        }
        CHECK(browseSeen == 1499);
        CHECK(schedule.stats().queuedProtectedRequests == 0);
        CHECK(schedule.stats().queuedProtectedPages == 0);
        CHECK(schedule.stats().droppedBrowse == 0);
        CHECK(schedule.popNext()->purpose == Purpose::Browse);
        CHECK(schedule.finish(nextBrowse));
        CHECK(schedule.stats().totalOutstandingPages == 0);

        // Zero policy input is safely clamped to one protected start; it cannot
        // create a continuously refreshed browse lane that starves protected I/O.
        Schedule clamped(512, 0);
        CHECK(clamped.enqueue(1, Purpose::ExplicitRead, 1).accepted);
        CHECK(clamped.enqueue(2, Purpose::Browse, 1).accepted);
        CHECK(clamped.popNext()->ticket == 1);
        CHECK(clamped.finish(1));
        CHECK(clamped.enqueue(3, Purpose::PostWrite, 1).accepted);
        CHECK(clamped.popNext()->ticket == 2);
    }

    void interestAndSourceInvalidation()
    {
        Schedule schedule;
        CHECK(schedule.updateInterest(50).empty());
        CHECK(schedule.enqueue(50, Purpose::Browse, 4, 50).accepted);
        CHECK(schedule.enqueue(51, Purpose::PostWrite, 8, 49).accepted);
        CHECK(schedule.popNext()->ticket == 51);
        CHECK(schedule.updateInterest(50).empty());
        const auto dropped = schedule.updateInterest(51);
        CHECK(dropped == std::vector<Ticket>{50});
        CHECK(schedule.contains(51));
        CHECK(schedule.enqueue(52, Purpose::ExplicitRetry, 3).accepted);
        CHECK(schedule.enqueue(53, Purpose::Browse, 5, 51).accepted);
        CHECK(schedule.enqueue(54, Purpose::Browse, 5, 50).reason == Reason::StaleInterest);
        const auto cancelled = schedule.clear();
        CHECK((cancelled == std::vector<Ticket>{51, 52, 53}));
        CHECK(!schedule.contains(50) && !schedule.contains(51));
        CHECK(schedule.stats().totalOutstandingPages == 0);
        CHECK(schedule.stats().activeRequests == 0);
        CHECK(schedule.stats().queuedProtectedRequests == 0);
        CHECK(schedule.stats().queuedBrowseRequests == 0);
        CHECK(schedule.clear().empty());
        CHECK(schedule.interestRevision() == 51);

        // A late callback from the retired source cannot finish a newly started
        // ticket. Source generation checks and physical-worker retirement remain
        // owner responsibilities; unique tickets prevent accidental aliasing.
        CHECK(schedule.updateInterest(0).empty());
        CHECK(schedule.enqueue(1000, Purpose::Browse, 2).accepted);
        CHECK(schedule.popNext()->ticket == 1000);
        CHECK(!schedule.finish(51));
        CHECK(schedule.stats().activeRequests == 1);
        CHECK(schedule.finish(1000));
        const auto maximum = std::numeric_limits<std::uint64_t>::max();
        CHECK(schedule.updateInterest(maximum).empty());
        CHECK(schedule.enqueue(1001, Purpose::Browse, 1, maximum).accepted);
        CHECK(schedule.updateInterest(0) == std::vector<Ticket>{1001});
        CHECK(schedule.enqueue(1002, Purpose::Browse, 1).accepted);
    }

    void numericalLimits()
    {
        const auto maximum = std::numeric_limits<std::size_t>::max();
        Schedule schedule(maximum);
        CHECK(schedule.enqueue(1, Purpose::ExplicitRead, maximum).accepted);
        CHECK(schedule.stats().queuedProtectedPages == maximum);
        CHECK(schedule.enqueue(2, Purpose::PostWrite, 1).reason == Reason::PageCountOverflow);
        CHECK(schedule.enqueue(3, Purpose::Browse, 1).reason == Reason::PageCountOverflow);
        CHECK(schedule.popNext()->ticket == 1);
        CHECK(schedule.enqueue(4, Purpose::ExplicitRetry, 1).reason == Reason::PageCountOverflow);
        CHECK(schedule.stats().activePages == maximum);
        CHECK(schedule.finish(1));
        CHECK(schedule.enqueue(5, Purpose::Browse, maximum).accepted);
        const auto replacement = schedule.enqueue(6, Purpose::Browse, 1);
        CHECK(replacement.accepted);
        CHECK(replacement.droppedBrowseTickets == std::vector<Ticket>{5});
        CHECK(schedule.stats().totalOutstandingPages == 1);
        CHECK(schedule.popNext()->ticket == 6);
        CHECK(schedule.enqueue(7, Purpose::Browse, maximum).reason == Reason::BrowseBudget);
        CHECK(schedule.stats().outstandingBrowsePages == 1);
        CHECK(schedule.finish(6));
        CHECK(schedule.enqueue(8, Purpose::Browse, maximum).accepted);
        CHECK(schedule.popNext()->ticket == 8);
        CHECK(schedule.enqueue(9, Purpose::Browse, 1).reason == Reason::BrowseBudget);
        CHECK(schedule.finish(8));
        CHECK(schedule.stats().totalOutstandingPages == 0);
    }

    void explicitDisplayPromotion()
    {
        Schedule schedule;
        CHECK(!schedule.promote(0));
        CHECK(!schedule.promote(1));
        CHECK(schedule.enqueue(1, Purpose::Browse, 256).accepted);
        CHECK(schedule.popNext()->ticket == 1);
        CHECK(schedule.enqueue(2, Purpose::Browse, 256).accepted);
        CHECK(schedule.stats().outstandingBrowsePages == 512);
        CHECK(!schedule.promote(1, Purpose::PostWrite));
        CHECK(!schedule.promote(1, Purpose::ExplicitRetry));
        CHECK(!schedule.promote(1, Purpose::Browse));
        CHECK(!schedule.promote(1, static_cast<Purpose>(999)));
        CHECK(schedule.stats().outstandingBrowsePages == 512);
        CHECK(schedule.promote(1));
        CHECK(!schedule.promote(1));
        CHECK(schedule.stats().activeBrowsePages == 0);
        CHECK(schedule.stats().pendingBrowsePages == 256);
        CHECK(schedule.stats().outstandingBrowsePages == 256);
        CHECK(schedule.stats().activePages == 256);
        CHECK(schedule.stats().totalOutstandingPages == 512);
        CHECK(!schedule.popNext()); // promotion never starts a second operation
        CHECK(schedule.updateInterest(1) == std::vector<Ticket>{2});
        CHECK(schedule.contains(1));
        CHECK(schedule.finish(1));

        CHECK(schedule.enqueue(10, Purpose::PostWrite, 2).accepted);
        CHECK(schedule.enqueue(11, Purpose::ExplicitRetry, 3).accepted);
        CHECK(schedule.enqueue(12, Purpose::Browse, 4, 1).accepted);
        CHECK(!schedule.promote(10));
        CHECK(schedule.promote(12, Purpose::ExplicitRead));
        CHECK(!schedule.promote(12));
        CHECK(schedule.stats().queuedBrowseRequests == 0);
        CHECK(schedule.stats().pendingBrowsePages == 0);
        CHECK(schedule.stats().queuedProtectedRequests == 3);
        CHECK(schedule.stats().queuedProtectedPages == 9);
        CHECK(schedule.stats().totalOutstandingPages == 9);
        CHECK(schedule.updateInterest(2).empty());
        CHECK(schedule.enqueue(13, Purpose::Browse, 5, 2).accepted);
        CHECK(schedule.popNext()->ticket == 10);
        CHECK(schedule.finish(10));
        CHECK(schedule.popNext()->ticket == 11);
        CHECK(schedule.finish(11));
        CHECK(schedule.popNext()->ticket == 13); // fairness survives promotion
        CHECK(schedule.finish(13));
        const auto promoted = schedule.popNext();
        CHECK(promoted && promoted->ticket == 12);
        CHECK(promoted->purpose == Purpose::ExplicitRead);
        CHECK(promoted->pageCount == 4 && promoted->interestRevision == 1);
        CHECK(schedule.finish(12));
        CHECK(schedule.stats().promoted == 2);
        CHECK(schedule.stats().accepted == 6);
        CHECK(schedule.stats().droppedBrowse == 1);
        CHECK(schedule.stats().totalOutstandingPages == 0);
    }
}

int main()
{
    emptyAndIdentity();
    slowBackendThousandScrolls();
    replacePendingAndBudgetRejection();
    protectedFifoAndFairness();
    interestAndSourceInvalidation();
    numericalLimits();
    explicitDisplayPromotion();
    std::cout << "MemoryReadSchedule: " << assertions << " assertions passed\n";
    return 0;
}
