#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <limits>
#include <numeric>
#include <utility>
#include <vector>

struct ProcessTraceTimelineRatePoint
{
    std::uint64_t time100ns = 0;
    double uploadBytesPerSecond = 0.0;
    double downloadBytesPerSecond = 0.0;
};

namespace ks::ui
{
    // The complete samples stay authoritative. This index is only a drawing
    // accelerator: 64-sample leaves keep its extra storage small.
    class TimelineRateValues final
    {
        static constexpr std::size_t blockSize = 64;
        static constexpr auto none = std::numeric_limits<std::size_t>::max();
        struct Extremes
        {
            std::size_t lowUp = none, highUp = none, lowDown = none, highDown = none;
        };
        std::vector<Extremes> m_tree;
        std::size_t m_leaves = 0;
        bool m_orderedFinite = true;

        Extremes combine(Extremes a, const Extremes& b) const
        {
            if (a.lowUp == none) return b;
            if (b.lowUp == none) return a;
            const auto earlier = [](std::size_t x, std::size_t y) { return std::min(x, y); };
            const auto choose = [&earlier](std::size_t x, std::size_t y, double vx, double vy, bool low) {
                return vx == vy ? earlier(x, y) : ((low ? vx < vy : vx > vy) ? x : y);
            };
            a.lowUp = choose(a.lowUp, b.lowUp, points[a.lowUp].uploadBytesPerSecond, points[b.lowUp].uploadBytesPerSecond, true);
            a.highUp = choose(a.highUp, b.highUp, points[a.highUp].uploadBytesPerSecond, points[b.highUp].uploadBytesPerSecond, false);
            a.lowDown = choose(a.lowDown, b.lowDown, points[a.lowDown].downloadBytesPerSecond, points[b.lowDown].downloadBytesPerSecond, true);
            a.highDown = choose(a.highDown, b.highDown, points[a.highDown].downloadBytesPerSecond, points[b.highDown].downloadBytesPerSecond, false);
            return a;
        }
        Extremes query(std::size_t first, std::size_t end) const
        {
            Extremes result;
            const auto addPoint = [&](std::size_t index) { result = combine(result, {index, index, index, index}); };
            while (first < end && first % blockSize) addPoint(first++);
            while (first < end && end % blockSize) addPoint(--end);
            for (auto left = first / blockSize + m_leaves, right = end / blockSize + m_leaves;
                left < right; left /= 2, right /= 2)
            {
                if (left & 1) result = combine(result, m_tree[left++]);
                if (right & 1) result = combine(result, m_tree[--right]);
            }
            return result;
        }
    public:
        std::vector<ProcessTraceTimelineRatePoint> points;

        explicit TimelineRateValues(std::vector<ProcessTraceTimelineRatePoint> values) : points(std::move(values))
        {
            for (std::size_t i = 0; i < points.size(); ++i)
                if (!std::isfinite(points[i].uploadBytesPerSecond) || !std::isfinite(points[i].downloadBytesPerSecond)
                    || (i && points[i].time100ns < points[i - 1].time100ns)) { m_orderedFinite = false; return; }
            if (points.empty()) return;
            m_leaves = 1;
            const auto blocks = (points.size() + blockSize - 1) / blockSize;
            while (m_leaves < blocks) m_leaves *= 2;
            m_tree.resize(m_leaves * 2);
            for (std::size_t i = 0; i < points.size(); ++i)
                m_tree[m_leaves + i / blockSize] = combine(m_tree[m_leaves + i / blockSize], {i, i, i, i});
            for (auto i = m_leaves; i-- > 1;) m_tree[i] = combine(m_tree[i * 2], m_tree[i * 2 + 1]);
        }
        std::size_t indexBytes() const { return m_tree.size() * sizeof(Extremes); }

        struct Projection
        {
            double maximum = 0.0;
            std::vector<std::size_t> upload, download;
        };
        struct Parameters
        {
            std::uint64_t first = 0, last = 0;
            long double animatedFirst = 0, animatedLast = 0;
            double physicalWidth = 1.0;
            bool operator==(const Parameters& other) const
            {
                return first == other.first && last == other.last && animatedFirst == other.animatedFirst
                    && animatedLast == other.animatedLast && physicalWidth == other.physicalWidth;
            }
        };

        // Preserve each occupied physical column's first/last and both extrema
        // in source order, separately for each line. Never sample away peaks.
        // The last sample is deliberately separate: the widget animates it.
        Projection project(const Parameters& p) const
        {
            Projection result;
            if (points.empty() || p.last <= p.first) return result;
            if (!m_orderedFinite || !std::isfinite(p.physicalWidth) || p.physicalWidth <= 0)
            {
                for (std::size_t i = 0; i < points.size(); ++i)
                {
                    const auto& point = points[i];
                    if (point.time100ns < p.first || point.time100ns > p.last) continue;
                    result.maximum = std::max({result.maximum, point.uploadBytesPerSecond, point.downloadBytesPerSecond});
                    if (i + 1 < points.size()) { result.upload.push_back(i); result.download.push_back(i); }
                }
                return result; // Compatibility path retains even non-monotonic order.
            }
            const auto first = static_cast<std::size_t>(std::lower_bound(points.begin(), points.end(), p.first,
                [](const auto& point, auto time) { return point.time100ns < time; }) - points.begin());
            const auto end = static_cast<std::size_t>(std::upper_bound(points.begin(), points.end(), p.last,
                [](auto time, const auto& point) { return time < point.time100ns; }) - points.begin());
            const auto maximum = query(first, end);
            if (maximum.highUp != none)
                result.maximum = std::max({0.0, points[maximum.highUp].uploadBytesPerSecond, points[maximum.highDown].downloadBytesPerSecond});
            const auto stop = std::min(end, points.size() - 1);
            const auto pixel = [&p](std::uint64_t time) {
                const auto span = p.animatedLast - p.animatedFirst;
                const long double ratio = span <= 0 ? 0 : std::clamp((static_cast<long double>(time) - p.animatedFirst) / span, 0.0L, 1.0L);
                return std::min(std::floor(static_cast<double>(ratio) * p.physicalWidth), std::max(0.0, std::ceil(p.physicalWidth) - 1));
            };
            const auto append = [](std::vector<std::size_t>& out, std::size_t begin, std::size_t limit, std::size_t low, std::size_t high) {
                std::array<std::size_t, 4> ids{begin, low, high, limit - 1};
                std::sort(ids.begin(), ids.end());
                const auto unique = std::unique(ids.begin(), ids.end());
                out.insert(out.end(), ids.begin(), unique);
            };
            for (auto cursor = first; cursor < stop;)
            {
                const auto column = pixel(points[cursor].time100ns);
                const auto next = static_cast<std::size_t>(std::upper_bound(points.begin() + cursor, points.begin() + stop, column,
                    [&pixel](double value, const auto& point) { return value < pixel(point.time100ns); }) - points.begin());
                if (next - cursor <= 4)
                    for (auto i = cursor; i < next; ++i) { result.upload.push_back(i); result.download.push_back(i); }
                else
                {
                    const auto extremes = query(cursor, next);
                    append(result.upload, cursor, next, extremes.lowUp, extremes.highUp);
                    append(result.download, cursor, next, extremes.lowDown, extremes.highDown);
                }
                cursor = next;
            }
            return result;
        }
    };
}
