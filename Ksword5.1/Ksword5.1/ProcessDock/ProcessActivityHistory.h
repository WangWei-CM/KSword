#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <iterator>
#include <memory>
#include <mutex>
#include <utility>
#include <vector>

namespace ks::process
{
    template<class Sample> class ProcessActivityHistoryRetirementLane;

    // GUI-owned canonical sequence. Retention changes its logical front in O(1).
    // Physically obsolete samples remain unreachable until a bounded pointer batch
    // can be handed to the single retirement lane; no sample payload is copied.
    template<class Sample>
    class ProcessActivityHistory
    {
    public:
        using Storage = std::deque<std::unique_ptr<Sample>>;
        struct Stamp
        {
            std::uint64_t session = 0;
            std::uint64_t policy = 0;
        };

        class const_iterator
        {
            using BaseIterator = typename Storage::const_iterator;
            BaseIterator m_iterator;
            explicit const_iterator(BaseIterator iterator) : m_iterator(iterator) {}
            friend class ProcessActivityHistory;
        public:
            using iterator_category = std::random_access_iterator_tag;
            using value_type = Sample;
            using difference_type = std::ptrdiff_t;
            using reference = const Sample&;
            using pointer = const Sample*;
            const_iterator() = default;
            reference operator*() const { return **m_iterator; }
            pointer operator->() const { return m_iterator->get(); }
            reference operator[](difference_type offset) const { return *m_iterator[offset]; }
            const_iterator& operator++() { ++m_iterator; return *this; }
            const_iterator operator++(int) { auto old = *this; ++*this; return old; }
            const_iterator& operator--() { --m_iterator; return *this; }
            const_iterator operator--(int) { auto old = *this; --*this; return old; }
            const_iterator& operator+=(difference_type offset) { m_iterator += offset; return *this; }
            const_iterator& operator-=(difference_type offset) { m_iterator -= offset; return *this; }
            friend const_iterator operator+(const_iterator value, difference_type offset) { return value += offset; }
            friend const_iterator operator+(difference_type offset, const_iterator value) { return value += offset; }
            friend const_iterator operator-(const_iterator value, difference_type offset) { return value -= offset; }
            friend difference_type operator-(const_iterator left, const_iterator right) { return left.m_iterator - right.m_iterator; }
            friend bool operator==(const_iterator left, const_iterator right) { return left.m_iterator == right.m_iterator; }
            friend bool operator!=(const_iterator left, const_iterator right) { return !(left == right); }
            friend bool operator<(const_iterator left, const_iterator right) { return left.m_iterator < right.m_iterator; }
            friend bool operator>(const_iterator left, const_iterator right) { return right < left; }
            friend bool operator<=(const_iterator left, const_iterator right) { return !(right < left); }
            friend bool operator>=(const_iterator left, const_iterator right) { return !(left < right); }
        };

        ProcessActivityHistory() = default;
        ProcessActivityHistory(const ProcessActivityHistory&) = delete;
        ProcessActivityHistory& operator=(const ProcessActivityHistory&) = delete;

        std::size_t size() const noexcept { return m_storage.size() - m_retiredPrefix; }
        bool empty() const noexcept { return size() == 0; }
        const Sample& operator[](std::size_t index) const { return *m_storage[m_retiredPrefix + index]; }
        const Sample& front() const { return (*this)[0]; }
        const Sample& back() const { return *m_storage.back(); }
        const_iterator cbegin() const { return const_iterator(m_storage.cbegin() + static_cast<std::ptrdiff_t>(m_retiredPrefix)); }
        const_iterator cend() const { return const_iterator(m_storage.cend()); }
        const_iterator begin() const { return cbegin(); }
        const_iterator end() const { return cend(); }

        void push_back(Sample&& sample) { m_storage.push_back(std::make_unique<Sample>(std::move(sample))); }
        void NotePolicyChange() noexcept { ++m_stamp.policy; }
        Stamp stamp() const noexcept { return m_stamp; }
        std::size_t retiredSize() const noexcept { return m_retiredPrefix; }

        std::size_t TrimTo(const std::size_t limit) noexcept
        {
            const auto removed = size() > limit ? size() - limit : 0;
            m_retiredPrefix += removed;
            return removed;
        }
        void Clear() noexcept
        {
            m_retiredPrefix = m_storage.size();
            ++m_stamp.session;
        }

    private:
        friend class ProcessActivityHistoryRetirementLane<Sample>;
        Storage m_storage;
        std::size_t m_retiredPrefix = 0;
        Stamp m_stamp;

        void DetachRetiredBatch(std::vector<std::unique_ptr<Sample>>& target, const std::size_t maximumSamples)
        {
            const auto count = std::min(m_retiredPrefix, maximumSamples);
            for (std::size_t index = 0; index < count; ++index)
            {
                target.push_back(std::move(m_storage.front()));
                m_storage.pop_front(); // only an empty unique_ptr is destroyed on the GUI
                --m_retiredPrefix;
            }
        }
        void DetachAll(Storage& target) noexcept
        {
            m_storage.swap(target);
            m_retiredPrefix = 0;
        }
    };

    // One active worker and one bounded ordinary batch, never a queue of jobs.
    // Close may add exactly one O(1)-swapped final backing. The worker cannot
    // access canonical state, so obsolete policy/session stamps cannot publish
    // old data or remove a sample appended after the accepted trim/clear.
    template<class Sample>
    class ProcessActivityHistoryRetirementLane
    {
        using History = ProcessActivityHistory<Sample>;
        using Storage = typename History::Storage;
        using Batch = std::vector<std::unique_ptr<Sample>>;
    public:
        bool TryBegin(History& history, const std::size_t maximumSamples)
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            if (m_active || m_closed || history.retiredSize() == 0 || maximumSamples == 0) return false;
            // Reserve before detaching anything; pointer moves below cannot allocate.
            m_batch.reserve(maximumSamples);
            m_batchStamp = history.stamp();
            history.DetachRetiredBatch(m_batch, maximumSamples);
            m_active = true;
            return true;
        }

        // Called once by the GUI owner's destructor. Return true to start Drain;
        // an already active Drain consumes the final backing before returning.
        bool Finalize(History& history)
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            if (m_closed) return false;
            m_closed = true;
            m_finalStamp = history.stamp();
            history.DetachAll(m_finalBacking);
            if (m_active || m_finalBacking.empty()) return false;
            m_batchStamp = m_finalStamp;
            m_active = true;
            return true;
        }

        void Drain()
        {
            for (;;)
            {
                {
                    Batch obsoleteBatch;
                    Storage obsoleteFinal;
                    {
                        std::lock_guard<std::mutex> lock(m_mutex);
                        obsoleteBatch.swap(m_batch);
                        obsoleteFinal.swap(m_finalBacking);
                    }
                } // H x P destruction, including vector/deque blocks/map, is entirely on worker
                {
                    std::lock_guard<std::mutex> lock(m_mutex);
                    if (m_finalBacking.empty())
                    {
                        m_active = false;
                        return;
                    }
                    // Finalize raced the previous destruction. Drain its one backing
                    // on the next pass, without scheduling a second worker.
                    m_batchStamp = m_finalStamp;
                }
            }
        }

        bool active() const { std::lock_guard<std::mutex> lock(m_mutex); return m_active; }
        typename History::Stamp batchStamp() const { std::lock_guard<std::mutex> lock(m_mutex); return m_batchStamp; }

    private:
        mutable std::mutex m_mutex;
        Batch m_batch;
        Storage m_finalBacking;
        typename History::Stamp m_batchStamp;
        typename History::Stamp m_finalStamp;
        bool m_active = false;
        bool m_closed = false;
    };
}
