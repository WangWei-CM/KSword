#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <limits>
#include <optional>
#include <unordered_set>
#include <utility>
#include <vector>

namespace ksword::memwb
{
    // UI-owner-thread scheduling only: no I/O, widgets, cancellation, or source
    // identity lives here. The owner retains payloads and must cancel every
    // rejected/dropped ticket's exact in-flight registration. Ticket identities
    // must remain unique across source generations until old callbacks retire.
    class MemoryReadSchedule
    {
    public:
        using Ticket = std::uint64_t;

        enum class Purpose { Browse, ExplicitRetry, PostWrite, ExplicitRead };

        struct Request
        {
            Ticket ticket = 0;
            Purpose purpose = Purpose::Browse;
            std::size_t pageCount = 0;
            std::uint64_t interestRevision = 0;
        };

        enum class RejectReason
        {
            None, InvalidTicket, InvalidPurpose, InvalidPageCount,
            DuplicateTicket, StaleInterest, BrowseBudget, PageCountOverflow
        };

        struct EnqueueResult
        {
            bool accepted = false;
            std::vector<Ticket> droppedBrowseTickets;
            RejectReason reason = RejectReason::None;
        };

        struct Stats
        {
            std::size_t activeRequests = 0;
            std::size_t queuedBrowseRequests = 0;
            std::size_t queuedProtectedRequests = 0;
            std::size_t activePages = 0;
            std::size_t queuedBrowsePages = 0;
            std::size_t queuedProtectedPages = 0;
            std::size_t activeBrowsePages = 0;
            std::size_t pendingBrowsePages = 0;
            std::size_t outstandingBrowsePages = 0;
            std::size_t totalOutstandingPages = 0;
            std::size_t peakOutstandingBrowsePages = 0;
            std::size_t consecutiveProtected = 0;
            std::uint64_t accepted = 0;
            std::uint64_t rejected = 0;
            std::uint64_t droppedBrowse = 0;
            std::uint64_t finished = 0;
            std::uint64_t promoted = 0;
        };

        // 512 * 4096-byte pages = 2 MiB of outstanding browsing payload.
        // The provider limits each complete browse window to 256 pages, leaving
        // room for its newest successor while an old 256-page read is active.
        // Protected reads are FIFO and are never removed by latest-browse or
        // interest changes. Admission rejects numeric overflow before accepting.
        explicit MemoryReadSchedule(std::size_t maxBrowsePages = 512,
                                    std::size_t maxConsecutiveProtected = 2)
            : maxBrowsePages_(maxBrowsePages),
              maxConsecutiveProtected_(std::max<std::size_t>(1, maxConsecutiveProtected))
        {
        }

        // A Browse ticket contains the complete current pending browse payload,
        // not just one fragment of that payload: an accepted newer Browse ticket
        // replaces the one not yet started. The active ticket is never replaced.
        EnqueueResult enqueue(Ticket ticket, Purpose purpose,
                              std::size_t pageCount, std::uint64_t interestRevision = 0)
        {
            EnqueueResult result;
            if (ticket == 0)
                return reject(std::move(result), RejectReason::InvalidTicket);
            if (!validPurpose(purpose))
                return reject(std::move(result), RejectReason::InvalidPurpose);
            if (pageCount == 0)
                return reject(std::move(result), RejectReason::InvalidPageCount);
            if (tickets_.find(ticket) != tickets_.end())
                return reject(std::move(result), RejectReason::DuplicateTicket);

            const bool browse = purpose == Purpose::Browse;
            const std::size_t replacedPages = browse && pendingBrowse_ ? pendingBrowse_->pageCount : 0;
            if (browse)
            {
                if (interestRevision != interestRevision_)
                    return reject(std::move(result), RejectReason::StaleInterest);
                const auto activeBrowsePages = active_ && active_->purpose == Purpose::Browse
                    ? active_->pageCount : 0;
                if (pageCount > maxBrowsePages_ || activeBrowsePages > maxBrowsePages_ - pageCount)
                    return reject(std::move(result), RejectReason::BrowseBudget);
            }

            const auto retainedPages = totalPages_ - replacedPages;
            if (pageCount > std::numeric_limits<std::size_t>::max() - retainedPages)
                return reject(std::move(result), RejectReason::PageCountOverflow);

            Request request{ ticket, purpose, pageCount, interestRevision };
            if (browse)
            {
                // Allocate the result before mutating the old slot. Rejected
                // requests leave the previously accepted browse ticket intact.
                if (pendingBrowse_)
                    result.droppedBrowseTickets.push_back(pendingBrowse_->ticket);
                tickets_.insert(ticket);
                if (pendingBrowse_)
                {
                    tickets_.erase(pendingBrowse_->ticket);
                    increment(droppedBrowse_);
                }
                pendingBrowse_ = request;
            }
            else
            {
                // No accepted protected task is silently evicted to make room.
                protected_.push_back(request);
                try
                {
                    tickets_.insert(ticket);
                }
                catch (...)
                {
                    protected_.pop_back();
                    throw;
                }
                protectedPages_ += pageCount; // bounded by checked totalPages_
            }
            totalPages_ = retainedPages + pageCount;
            peakBrowsePages_ = std::max(peakBrowsePages_, outstandingBrowsePages());
            increment(accepted_);
            result.accepted = true;
            return result;
        }

        // An explicit display request can overlap pages already marked pending
        // by browsing. Preserve that complete ticket rather than losing its
        // missing pages when PlanFetch excludes in-flight pages. A queued browse
        // moves to the protected FIFO tail at this new explicit intent; an
        // active browse simply becomes protected without dispatching a new read.
        // PostWrite and ExplicitRetry deliberately require fresh operations:
        // an already-started old read cannot satisfy those temporal contracts.
        bool promote(Ticket ticket, Purpose purpose = Purpose::ExplicitRead)
        {
            if (purpose != Purpose::ExplicitRead)
                return false;
            if (active_ && active_->ticket == ticket && active_->purpose == Purpose::Browse)
            {
                active_->purpose = purpose;
                increment(promoted_);
                return true;
            }
            if (!pendingBrowse_ || pendingBrowse_->ticket != ticket)
                return false;
            auto request = *pendingBrowse_;
            request.purpose = purpose;
            // Allocate first: an allocation failure leaves the browse slot and
            // its original accounting unchanged.
            protected_.push_back(request);
            protectedPages_ += request.pageCount; // part of checked totalPages_
            pendingBrowse_.reset();
            increment(promoted_);
            return true;
        }

        // The owner sends its full viewport interest before submitting missing
        // pages. Equality is intentional: revisions may wrap or reset when the
        // owner changes source. An old active operation still counts toward the
        // budget until finish(); the owner validates its callback separately.
        std::vector<Ticket> updateInterest(std::uint64_t revision)
        {
            std::vector<Ticket> dropped;
            if (pendingBrowse_ && pendingBrowse_->interestRevision != revision)
            {
                dropped.push_back(pendingBrowse_->ticket);
                totalPages_ -= pendingBrowse_->pageCount;
                tickets_.erase(pendingBrowse_->ticket);
                pendingBrowse_.reset();
                increment(droppedBrowse_);
            }
            interestRevision_ = revision;
            return dropped;
        }

        // At most one runnable may be submitted. Every started ticket must call
        // finish(ticket), including a failed or cooperatively cancelled read.
        // Two protected starts at most while browsing waits; after a browse start
        // at least one protected task runs if available, preventing either lane
        // from starving. Protected order itself is always FIFO.
        std::optional<Request> popNext()
        {
            if (active_)
                return std::nullopt;
            if (pendingBrowse_ && (protected_.empty() || consecutiveProtected_ >= maxConsecutiveProtected_))
            {
                active_ = pendingBrowse_;
                pendingBrowse_.reset();
                consecutiveProtected_ = 0;
            }
            else if (!protected_.empty())
            {
                active_ = protected_.front();
                protected_.pop_front();
                protectedPages_ -= active_->pageCount;
                if (consecutiveProtected_ < maxConsecutiveProtected_)
                    ++consecutiveProtected_;
            }
            return active_;
        }

        bool finish(Ticket ticket)
        {
            if (!active_ || active_->ticket != ticket)
                return false;
            totalPages_ -= active_->pageCount;
            tickets_.erase(ticket);
            active_.reset();
            increment(finished_);
            return true;
        }

        // Explicit owner invalidation/cancellation only. Returns the active
        // ticket first, followed by protected FIFO and the pending browse ticket.
        // It neither cancels I/O nor authorizes a second physical operation while
        // a cleared active worker is still running; the owner must retire that
        // worker before using this schedule for another source generation.
        std::vector<Ticket> clear()
        {
            std::vector<Ticket> cancelled;
            cancelled.reserve(tickets_.size());
            if (active_)
                cancelled.push_back(active_->ticket);
            for (const auto& request : protected_)
                cancelled.push_back(request.ticket);
            if (pendingBrowse_)
                cancelled.push_back(pendingBrowse_->ticket);
            active_.reset();
            protected_.clear();
            pendingBrowse_.reset();
            tickets_.clear();
            totalPages_ = 0;
            protectedPages_ = 0;
            consecutiveProtected_ = 0;
            return cancelled;
        }

        Stats stats() const
        {
            Stats result;
            result.activeRequests = active_ ? 1 : 0;
            result.queuedBrowseRequests = pendingBrowse_ ? 1 : 0;
            result.queuedProtectedRequests = protected_.size();
            result.activePages = active_ ? active_->pageCount : 0;
            result.queuedBrowsePages = pendingBrowse_ ? pendingBrowse_->pageCount : 0;
            result.queuedProtectedPages = protectedPages_;
            result.activeBrowsePages = active_ && active_->purpose == Purpose::Browse ? active_->pageCount : 0;
            result.pendingBrowsePages = pendingBrowse_ ? pendingBrowse_->pageCount : 0;
            result.outstandingBrowsePages = outstandingBrowsePages();
            result.totalOutstandingPages = totalPages_;
            result.peakOutstandingBrowsePages = peakBrowsePages_;
            result.consecutiveProtected = consecutiveProtected_;
            result.accepted = accepted_;
            result.rejected = rejected_;
            result.droppedBrowse = droppedBrowse_;
            result.finished = finished_;
            result.promoted = promoted_;
            return result;
        }

        bool contains(Ticket ticket) const { return tickets_.find(ticket) != tickets_.end(); }
        std::uint64_t interestRevision() const { return interestRevision_; }
        std::size_t maxBrowsePages() const { return maxBrowsePages_; }

    private:
        static bool validPurpose(Purpose purpose)
        {
            switch (purpose)
            {
            case Purpose::Browse:
            case Purpose::ExplicitRetry:
            case Purpose::PostWrite:
            case Purpose::ExplicitRead:
                return true;
            }
            return false;
        }

        static void increment(std::uint64_t& counter)
        {
            if (counter != std::numeric_limits<std::uint64_t>::max())
                ++counter;
        }

        EnqueueResult reject(EnqueueResult result, RejectReason reason)
        {
            result.reason = reason;
            increment(rejected_);
            return result;
        }

        std::size_t outstandingBrowsePages() const
        {
            return (active_ && active_->purpose == Purpose::Browse ? active_->pageCount : 0)
                + (pendingBrowse_ ? pendingBrowse_->pageCount : 0);
        }

        std::size_t maxBrowsePages_;
        std::size_t maxConsecutiveProtected_;
        std::uint64_t interestRevision_ = 0;
        std::optional<Request> active_;
        std::optional<Request> pendingBrowse_;
        std::deque<Request> protected_;
        std::unordered_set<Ticket> tickets_;
        std::size_t totalPages_ = 0;
        std::size_t protectedPages_ = 0;
        std::size_t peakBrowsePages_ = 0;
        std::size_t consecutiveProtected_ = 0;
        std::uint64_t accepted_ = 0;
        std::uint64_t rejected_ = 0;
        std::uint64_t droppedBrowse_ = 0;
        std::uint64_t finished_ = 0;
        std::uint64_t promoted_ = 0;
    };
}
