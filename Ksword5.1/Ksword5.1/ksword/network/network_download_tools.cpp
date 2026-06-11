#include "network_download_tools.h"

#include <algorithm>
#include <cstddef>
#include <limits>

namespace ks::network
{
    DownloadPlan BuildDownloadSegmentPlan(
        const std::uint64_t totalBytes,
        const int requestedThreadCount,
        const bool supportsRange,
        const int maxThreadCount)
    {
        DownloadPlan plan;
        plan.requestedThreadCount = std::max(1, requestedThreadCount);
        plan.supportsRange = supportsRange;

        // Empty files still get one logical segment so UI code has a stable row.
        if (totalBytes == 0)
        {
            plan.actualThreadCount = 1;
            plan.useRange = false;
            plan.emptyFile = true;
            plan.segments.push_back(DownloadSegmentPlan{ 0, 0, 0, 0 });
            return plan;
        }

        // Range support decides whether the task can be split across workers.
        int actualThreadCount = supportsRange ? plan.requestedThreadCount : 1;
        const int safeMaxThreadCount = std::max(1, maxThreadCount);
        const int byteBoundedThreadCount = static_cast<int>(std::min<std::uint64_t>(
            static_cast<std::uint64_t>(safeMaxThreadCount),
            totalBytes));
        actualThreadCount = std::max(1, std::min(actualThreadCount, byteBoundedThreadCount));

        plan.actualThreadCount = actualThreadCount;
        plan.useRange = supportsRange && actualThreadCount > 1;
        plan.emptyFile = false;
        plan.segments.reserve(static_cast<std::size_t>(actualThreadCount));

        // Remainder bytes are distributed to the earliest segments for even balancing.
        const std::uint64_t averageSegmentBytes = totalBytes / static_cast<std::uint64_t>(actualThreadCount);
        const std::uint64_t remainSegmentBytes = totalBytes % static_cast<std::uint64_t>(actualThreadCount);
        std::uint64_t beginByte = 0;
        for (int index = 0; index < actualThreadCount; ++index)
        {
            const std::uint64_t segmentBytes = averageSegmentBytes +
                (index < static_cast<int>(remainSegmentBytes) ? 1ULL : 0ULL);
            const std::uint64_t endByte = beginByte + segmentBytes - 1ULL;
            plan.segments.push_back(DownloadSegmentPlan{ index, beginByte, endByte, segmentBytes });
            beginByte = endByte + 1ULL;
        }
        return plan;
    }

    std::uint64_t CalculateSegmentByteCount(
        const std::uint64_t beginByte,
        const std::uint64_t endByte)
    {
        if (endByte < beginByte)
        {
            return 0;
        }
        return endByte - beginByte + 1ULL;
    }

    std::string BuildHttpRangeHeader(
        const std::uint64_t beginByte,
        const std::uint64_t endByte)
    {
        return "Range: bytes=" + std::to_string(beginByte) + "-" + std::to_string(endByte);
    }

    double CalculateProgressRatio(
        const std::uint64_t downloadedBytes,
        const std::uint64_t totalBytes)
    {
        if (totalBytes == 0)
        {
            return 1.0;
        }
        const std::uint64_t clampedDownloaded = std::min(downloadedBytes, totalBytes);
        return std::clamp(
            static_cast<double>(clampedDownloaded) / static_cast<double>(totalBytes),
            0.0,
            1.0);
    }

    double CalculateProgressPercent(
        const std::uint64_t downloadedBytes,
        const std::uint64_t totalBytes)
    {
        return CalculateProgressRatio(downloadedBytes, totalBytes) * 100.0;
    }

    std::uint64_t CalculateBytesPerSecond(
        const std::uint64_t transferredBytes,
        const std::uint64_t elapsedMs)
    {
        if (elapsedMs == 0)
        {
            return 0;
        }

        // 使用浮点中间值避免 transferredBytes*1000 在长任务或大文件下溢出。
        const long double rateValue =
            (static_cast<long double>(transferredBytes) * 1000.0L) /
            static_cast<long double>(elapsedMs);
        if (rateValue >= static_cast<long double>(std::numeric_limits<std::uint64_t>::max()))
        {
            return std::numeric_limits<std::uint64_t>::max();
        }
        return static_cast<std::uint64_t>(rateValue);
    }
} // namespace ks::network
