#include "../Ksword5.1/Ksword5.1/ProcessDock/ProcessActivitySeriesCache.h"
#include <cassert>
#include <iostream>
#include <random>

using ks::ui::activity::SeriesCache;
int main()
{
    SeriesCache cache;
    assert(cache.project(0, 0, 100, 100).empty());
    std::mt19937 random(311);
    for (std::uint64_t i = 37; i < 500037; ++i)
    {
        SeriesCache::Point point{};
        point.sequence = i;
        point.timeMs = static_cast<double>(i * 50U);
        for (std::size_t metric = 0; metric < SeriesCache::MetricCount; ++metric)
            point.values[metric] = static_cast<double>(random() % 1000);
        if (i == 250013) point.values[0] = 100000;
        assert(cache.append(point));
    }
    assert(!cache.append(cache.back()));
    assert(cache.size() == 500000);
    for (std::size_t metric = 0; metric < SeriesCache::MetricCount; ++metric)
    {
        const auto extrema = cache.extrema(cache.front().sequence, cache.back().sequence, metric);
        std::uint64_t minimum = cache.front().sequence, maximum = minimum;
        for (std::uint64_t i = minimum + 1; i <= cache.back().sequence; ++i)
        {
            if (cache.at(i).values[metric] < cache.at(minimum).values[metric]) minimum = i;
            if (cache.at(i).values[metric] > cache.at(maximum).values[metric]) maximum = i;
        }
        assert(extrema.valid && extrema.minimum == minimum && extrema.maximum == maximum);
    }
    const auto projected = cache.project(0, cache.front().timeMs, cache.back().timeMs, 800);
    assert(projected.size() <= 1602);
    assert(std::is_sorted(projected.begin(), projected.end()));
    assert(std::find(projected.begin(), projected.end(), 250013) != projected.end());
    assert(projected.front() == cache.front().sequence && projected.back() == cache.back().sequence);
    // Trim through unaligned hierarchy blocks, then continue appending without rebuilding history.
    cache.trimBefore(499901);
    assert(cache.front().sequence == 499901 && cache.size() == 136);
    for (std::uint64_t i = 500037; i < 501137; ++i)
        assert(cache.append({i, static_cast<double>(i * 50), {1,2,3,4,5}}));
    for (std::uint64_t first = 499901; first < 500130; first += 7)
    {
        const std::uint64_t last = std::min<std::uint64_t>(first + 23, cache.back().sequence);
        const auto range = cache.extrema(first,last,0);
        std::uint64_t lo=first, hi=first;
        for(std::uint64_t i=first+1; i<=last; ++i)
        {
            if(cache.at(i).values[0] < cache.at(lo).values[0]) lo=i;
            if(cache.at(i).values[0] > cache.at(hi).values[0]) hi=i;
        }
        assert(range.valid && range.minimum==lo && range.maximum==hi);
    }
    SeriesCache ramp;
    assert(ramp.append({0,0,{0,0,0,0,0}}));
    assert(ramp.append({1,10,{10,0,0,0,0}}));
    assert(ramp.append({2,20,{20,0,0,0,0}}));
    assert(ramp.slope(0,0,100)==1 && ramp.slope(1,0,100)==1 && ramp.slope(2,0,100)==1);
    assert(ramp.normalizedValue(2,0,10)==100);
    const auto offscreen = ramp.project(0, 1000, 2000, 10);
    assert(offscreen.size()==1 && offscreen.front()==2);
    SeriesCache invalid;
    const double infinity = std::numeric_limits<double>::infinity();
    const double nan = std::numeric_limits<double>::quiet_NaN();
    assert(invalid.append({0,0,{nan,infinity,-infinity,50,50}}));
    assert(invalid.append({1,1,{50,50,50,50,50}}));
    assert(invalid.normalizedValue(0,0,100)==0);
    assert(invalid.normalizedValue(0,1,100)==0);
    assert(invalid.normalizedValue(0,2,100)==0);
    assert(invalid.extrema(0,1,0).maximum==1);
    assert(invalid.normalizedValue(1,0,infinity)==0);
    cache.clear(); assert(cache.empty());
    assert(cache.append({999,1,{1,2,3,4,5}}));
    assert(cache.extrema(999,999,4).maximum==999);
    std::cout << "PASS: 500k complete history, exact extrema, bounded pixel projection, narrow peak, trim/append and slopes\n";
}
