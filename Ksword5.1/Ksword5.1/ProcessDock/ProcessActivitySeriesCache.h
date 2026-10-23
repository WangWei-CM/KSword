#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <limits>
#include <vector>

namespace ks::ui::activity
{
    // Display-only aggregation. Canonical process snapshots remain owned by ProcessDock.
    class SeriesCache
    {
    public:
        static constexpr std::size_t MetricCount = 5;
        struct Point
        {
            std::uint64_t sequence = 0;
            double timeMs = 0.0;
            std::array<double, MetricCount> values{};
        };
        struct Extrema
        {
            std::uint64_t minimum = 0;
            std::uint64_t maximum = 0;
            bool valid = false;
        };

        void clear()
        {
            m_points.clear();
            m_levels.clear();
            ++m_revision;
        }

        // Constant-time handoff; MSVC deque pop_front retains spare allocations until destruction.
        void swap(SeriesCache& other) noexcept
        {
            m_points.swap(other.m_points);
            m_levels.swap(other.m_levels);
            std::swap(m_revision, other.m_revision);
        }
        bool storageEmpty() const { return m_points.empty() && m_levels.empty(); }

        bool empty() const { return m_points.empty(); }
        std::size_t size() const { return m_points.size(); }
        std::uint64_t revision() const { return m_revision; }
        const Point& front() const { return m_points.front(); }
        const Point& back() const { return m_points.back(); }
        const Point& at(const std::uint64_t sequence) const
        {
            return m_points.at(static_cast<std::size_t>(sequence - front().sequence));
        }

        // Sequence continuity lets every projection keep the canonical sample identity.
        bool append(Point point)
        {
            if (!empty() && (back().sequence == std::numeric_limits<std::uint64_t>::max()
                || point.sequence != back().sequence + 1U || point.timeMs < back().timeMs))
            {
                return false;
            }
            // Match the chart's existing invalid-value presentation contract without changing source data.
            for (double& value : point.values)
            {
                if (!std::isfinite(value)) value = 0.0;
            }
            m_points.push_back(point);
            Block leaf{};
            leaf.id = point.sequence;
            leaf.first = point.sequence;
            leaf.last = point.sequence;
            leaf.minimum.fill(point.sequence);
            leaf.maximum.fill(point.sequence);
            appendBlock(0, leaf);
            for (std::size_t level = 1; level < 63; ++level)
            {
                const std::uint64_t span = std::uint64_t{1} << level;
                if ((point.sequence & (span - 1U)) != span - 1U)
                {
                    break;
                }
                const std::uint64_t id = point.sequence >> level;
                const Block* left = blockAt(level - 1U, id * 2U);
                const Block* right = blockAt(level - 1U, id * 2U + 1U);
                if (left == nullptr || right == nullptr)
                {
                    break; // A cache may begin in the middle of an aligned block.
                }
                Block merged{};
                merged.id = id;
                merged.first = left->first;
                merged.last = right->last;
                for (std::size_t metric = 0; metric < MetricCount; ++metric)
                {
                    merged.minimum[metric] = choose(left->minimum[metric], right->minimum[metric], metric, false);
                    merged.maximum[metric] = choose(left->maximum[metric], right->maximum[metric], metric, true);
                }
                appendBlock(level, merged);
            }
            ++m_revision;
            return true;
        }

        // Drop only display cache rows that the source no longer retains; callers may keep one edge.
        void trimBefore(const std::uint64_t firstSequence)
        {
            bool changed = false;
            while (!empty() && front().sequence < firstSequence)
            {
                m_points.pop_front();
                changed = true;
            }
            for (auto& level : m_levels)
            {
                while (!level.empty() && level.front().first < firstSequence)
                {
                    level.pop_front();
                }
            }
            if (changed)
            {
                ++m_revision;
            }
        }

        Extrema extrema(std::uint64_t first, std::uint64_t last, const std::size_t metric) const
        {
            Extrema result{};
            if (empty() || metric >= MetricCount || first > last || last < front().sequence || first > back().sequence)
            {
                return result;
            }
            first = std::max(first, front().sequence);
            last = std::min(last, back().sequence);
            for (std::uint64_t cursor = first; cursor <= last;)
            {
                const Block* block = nullptr;
                for (std::size_t level = m_levels.size(); level-- > 0;)
                {
                    const std::uint64_t span = std::uint64_t{1} << level;
                    if ((cursor & (span - 1U)) == 0 && span - 1U <= last - cursor)
                    {
                        block = blockAt(level, cursor >> level);
                        if (block != nullptr)
                        {
                            break;
                        }
                    }
                }
                // Every retained row has a leaf, even when its enclosing block was partial.
                if (block == nullptr)
                {
                    return {};
                }
                if (!result.valid)
                {
                    result = {block->minimum[metric], block->maximum[metric], true};
                }
                else
                {
                    result.minimum = choose(result.minimum, block->minimum[metric], metric, false);
                    result.maximum = choose(result.maximum, block->maximum[metric], metric, true);
                }
                if (block->last == last)
                {
                    break;
                }
                cursor = block->last + 1U;
            }
            return result;
        }

        // Each physical column preserves its true minimum and maximum, in chronological order.
        // The original time axis still performs exact sample hit testing outside this helper.
        std::vector<std::uint64_t> project(const std::size_t metric, const double start,
            const double end, const std::size_t columns) const
        {
            std::vector<std::uint64_t> result;
            if (empty() || metric >= MetricCount || !(end > start) || columns == 0)
            {
                return result;
            }
            const auto less = [](const Point& point, const double time) { return point.timeMs < time; };
            auto first = std::lower_bound(m_points.begin(), m_points.end(), start, less);
            if (first != m_points.begin())
            {
                --first;
            }
            auto last = std::upper_bound(first, m_points.end(), end,
                [](const double time, const Point& point) { return time < point.timeMs; });
            if (last != m_points.end())
            {
                ++last;
            }
            if (first == last)
            {
                return result;
            }
            result.reserve(columns * 2U + 2U);
            result.push_back(first->sequence);
            auto bucketFirst = std::lower_bound(first, last, start, less);
            for (std::size_t column = 0; column < columns && bucketFirst != last; ++column)
            {
                const double boundary = start + (end - start) * static_cast<double>(column + 1U)
                    / static_cast<double>(columns);
                const auto bucketEnd = column + 1U == columns
                    ? std::upper_bound(bucketFirst, last, end,
                        [](const double time, const Point& point) { return time < point.timeMs; })
                    : std::lower_bound(bucketFirst, last, boundary, less);
                if (bucketFirst != bucketEnd)
                {
                    const auto range = extrema(bucketFirst->sequence, std::prev(bucketEnd)->sequence, metric);
                    if (range.valid)
                    {
                        result.push_back(std::min(range.minimum, range.maximum));
                        if (range.minimum != range.maximum)
                        {
                            result.push_back(std::max(range.minimum, range.maximum));
                        }
                    }
                }
                bucketFirst = bucketEnd;
            }
            result.push_back(std::prev(last)->sequence);
            result.erase(std::unique(result.begin(), result.end()), result.end());
            return result;
        }

        double normalizedValue(const std::uint64_t sequence, const std::size_t metric, const double denominator) const
        {
            const double value = at(sequence).values[metric] / std::max(1.0, denominator) * 100.0;
            return std::isfinite(value) ? std::clamp(value, 0.0, 100.0) : 0.0;
        }

        // Same monotone Hermite tangent as the original chart, read from exact raw neighbors.
        double slope(const std::uint64_t sequence, const std::size_t metric, const double denominator) const
        {
            if (size() < 2U)
            {
                return 0.0;
            }
            const auto secant = [&](const std::uint64_t left, const std::uint64_t right) {
                return (normalizedValue(right, metric, denominator) - normalizedValue(left, metric, denominator))
                    / std::max(1.0, at(right).timeMs - at(left).timeMs);
            };
            if (sequence == front().sequence) return secant(sequence, sequence + 1U);
            if (sequence == back().sequence) return secant(sequence - 1U, sequence);
            const double before = secant(sequence - 1U, sequence);
            const double after = secant(sequence, sequence + 1U);
            if (before * after <= 0.0) return 0.0;
            const double previousWidth = std::max(1.0, at(sequence).timeMs - at(sequence - 1U).timeMs);
            const double nextWidth = std::max(1.0, at(sequence + 1U).timeMs - at(sequence).timeMs);
            const double previousWeight = 2.0 * nextWidth + previousWidth;
            const double nextWeight = nextWidth + 2.0 * previousWidth;
            return (previousWeight + nextWeight) / (previousWeight / before + nextWeight / after);
        }

    private:
        struct Block
        {
            std::uint64_t id = 0;
            std::uint64_t first = 0;
            std::uint64_t last = 0;
            std::array<std::uint64_t, MetricCount> minimum{};
            std::array<std::uint64_t, MetricCount> maximum{};
        };
        std::uint64_t choose(const std::uint64_t a, const std::uint64_t b,
            const std::size_t metric, const bool maximum) const
        {
            const double av = at(a).values[metric];
            const double bv = at(b).values[metric];
            return av == bv ? std::min(a, b) : ((maximum ? av > bv : av < bv) ? a : b);
        }
        const Block* blockAt(const std::size_t level, const std::uint64_t id) const
        {
            if (level >= m_levels.size() || m_levels[level].empty()) return nullptr;
            const auto& blocks = m_levels[level];
            if (id < blocks.front().id || id > blocks.back().id) return nullptr;
            return &blocks[static_cast<std::size_t>(id - blocks.front().id)];
        }
        void appendBlock(const std::size_t level, const Block& block)
        {
            if (m_levels.size() <= level) m_levels.resize(level + 1U);
            m_levels[level].push_back(block);
        }
        std::deque<Point> m_points;
        std::vector<std::deque<Block>> m_levels;
        std::uint64_t m_revision = 0;
    };
}
