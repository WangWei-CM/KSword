#pragma once

// ============================================================
// ksword/network/network_download_tools.h
// Namespace: ks::network
// Purpose:
// - Provide UI-independent planning helpers for HTTP range downloads.
// - Keep segment splitting, range header construction, and progress math reusable.
// - Use only STL types so callers can adapt results to Qt or non-Qt UIs.
// ============================================================

#include <cstdint>
#include <string>
#include <vector>

namespace ks::network
{
    // DownloadSegmentPlan describes one inclusive byte range assigned to a worker.
    // byteCount is cached so UI and workers do not need to repeat end-begin math.
    struct DownloadSegmentPlan
    {
        int segmentIndex = 0;
        std::uint64_t beginByte = 0;
        std::uint64_t endByte = 0;
        std::uint64_t byteCount = 0;
    };

    // DownloadPlan describes the complete worker layout for one download task.
    // useRange is true only when multiple workers should send Range requests.
    struct DownloadPlan
    {
        int requestedThreadCount = 1;
        int actualThreadCount = 1;
        bool supportsRange = false;
        bool useRange = false;
        bool emptyFile = false;
        std::vector<DownloadSegmentPlan> segments;
    };

    // BuildDownloadSegmentPlan normalizes requested threads and splits bytes evenly.
    // totalBytes==0 returns one empty-file segment marked as finished by the caller.
    [[nodiscard]] DownloadPlan BuildDownloadSegmentPlan(
        std::uint64_t totalBytes,
        int requestedThreadCount,
        bool supportsRange,
        int maxThreadCount = 64);

    // CalculateSegmentByteCount returns the inclusive length of a byte range.
    // Invalid ranges return zero instead of underflowing.
    [[nodiscard]] std::uint64_t CalculateSegmentByteCount(
        std::uint64_t beginByte,
        std::uint64_t endByte);

    // BuildHttpRangeHeader returns an HTTP Range header value with CRLF-free text.
    // The returned string includes the "Range: " prefix expected by WinHTTP add-header.
    [[nodiscard]] std::string BuildHttpRangeHeader(
        std::uint64_t beginByte,
        std::uint64_t endByte);

    // CalculateProgressRatio returns downloaded/total clamped to [0, 1].
    // total==0 is treated as complete to match zero-byte file behavior.
    [[nodiscard]] double CalculateProgressRatio(
        std::uint64_t downloadedBytes,
        std::uint64_t totalBytes);

    // CalculateProgressPercent returns progress in [0, 100].
    // It is a convenience wrapper over CalculateProgressRatio.
    [[nodiscard]] double CalculateProgressPercent(
        std::uint64_t downloadedBytes,
        std::uint64_t totalBytes);

    // CalculateBytesPerSecond converts bytes transferred over elapsed milliseconds to B/s.
    // elapsedMs==0 returns zero to avoid division by zero.
    [[nodiscard]] std::uint64_t CalculateBytesPerSecond(
        std::uint64_t transferredBytes,
        std::uint64_t elapsedMs);
} // namespace ks::network
