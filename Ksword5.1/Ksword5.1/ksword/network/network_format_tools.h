#pragma once

// ============================================================
// ksword/network/network_format_tools.h
// Namespace: ks::network
// Purpose:
// - Provide UI-independent formatting and parsing helpers for network data.
// - Keep PacketRecord payload rendering, endpoint text, IPv4 ranges, and byte
//   counters reusable outside NetworkDock.
// - Expose only STL types so this layer remains free of Qt widget/string types.
// ============================================================

#include "network.h"

#include <cstddef>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace ks::network
{
    // PayloadByteRange describes a safe readable subrange inside PacketRecord::packetBytes.
    // offset is the first payload byte and length is the number of bytes that can be read.
    struct PayloadByteRange
    {
        std::size_t offset = 0;
        std::size_t length = 0;
    };

    // FormatEndpointText combines an address and port into host:port text.
    // IPv6 addresses are wrapped in brackets so the port separator stays unambiguous.
    [[nodiscard]] std::string FormatEndpointText(
        const std::string& ipAddress,
        std::uint16_t portNumber);

    // FormatByteCount converts a byte counter into B/KB/MB/GB/TB text.
    // The return value is stable English text suitable for tables and logs.
    [[nodiscard]] std::string FormatByteCount(std::uint64_t bytesValue);

    // FormatBytesPerSecond formats a transfer rate by reusing FormatByteCount.
    // The input is bytes per second and the return value appends "/s".
    [[nodiscard]] std::string FormatBytesPerSecond(std::uint64_t bytesPerSecond);

    // FormatUnixTimestampMs converts Unix milliseconds to local timestamp text.
    // includeDate controls whether the date part is included in the returned string.
    [[nodiscard]] std::string FormatUnixTimestampMs(
        std::uint64_t timestampMs,
        bool includeDate);

    // FormatPercent converts a ratio/percent value into fixed decimal text.
    // decimals is clamped by implementation to a small safe range.
    [[nodiscard]] std::string FormatPercent(double percentValue, int decimals = 2);

    // FormatBytesToHexPreview renders a single-line hexadecimal preview.
    // At most maxBytesToRender bytes are included; truncated output includes total size.
    [[nodiscard]] std::string FormatBytesToHexPreview(
        const std::vector<std::uint8_t>& byteArray,
        std::size_t maxBytesToRender);

    // BuildPayloadByteRange calculates the safe payload range for a packet record.
    // It never returns a range extending past packetBytes, even when capture was truncated.
    [[nodiscard]] PayloadByteRange BuildPayloadByteRange(const PacketRecord& packetRecord);

    // BuildPayloadAsciiPreviewText returns a compact readable payload preview.
    // Text-like payloads keep printable segments; binary payloads fall back to dot mapping.
    [[nodiscard]] std::string BuildPayloadAsciiPreviewText(
        const PacketRecord& packetRecord,
        std::size_t previewByteLimit = 180);

    // BuildPayloadAsciiFullText returns the full retained payload as ASCII-ish text.
    // CR/LF/TAB are preserved and non-printable bytes are replaced by '.'.
    [[nodiscard]] std::string BuildPayloadAsciiFullText(const PacketRecord& packetRecord);

    // BuildPayloadHexFullText returns the full retained payload as space-separated HEX.
    // It is intended for replaying captured bytes through the manual request page.
    [[nodiscard]] std::string BuildPayloadHexFullText(const PacketRecord& packetRecord);

    // BuildPacketHexAsciiDumpText returns an offset + HEX + ASCII dump for retained bytes.
    // bytesPerRow defaults to the conventional 16-byte hex editor row width.
    [[nodiscard]] std::string BuildPacketHexAsciiDumpText(
        const PacketRecord& packetRecord,
        std::size_t bytesPerRow = 16);

    // BuildPacketCopyHeaderLine returns one metadata line for clipboard packet exports.
    // The line contains sequence, timestamp, PID, protocol, direction, endpoints, and sizes.
    [[nodiscard]] std::string BuildPacketCopyHeaderLine(const PacketRecord& packetRecord);

    // FormatIpv4HostOrder converts a host-order IPv4 integer into dotted decimal text.
    // The input layout is A.B.C.D as bits 31..0.
    [[nodiscard]] std::string FormatIpv4HostOrder(std::uint32_t ipv4HostOrder);

    // TryParseIpv4Text parses dotted decimal IPv4 text into host-order integer output.
    // Returns false on malformed input or null output pointer.
    [[nodiscard]] bool TryParseIpv4Text(
        const std::string& ipv4Text,
        std::uint32_t* ipv4HostOrderOut);

    // TryParseIpv4RangeText parses CIDR, begin-end, or single IPv4 expressions.
    // On success it writes an inclusive range and normalized text if pointers are provided.
    [[nodiscard]] bool TryParseIpv4RangeText(
        const std::string& rangeText,
        std::pair<std::uint32_t, std::uint32_t>* rangeOut,
        std::string* normalizedTextOut = nullptr);

    // TryParsePortRangeText parses either a single port or inclusive begin-end range.
    // On success the range is normalized so first <= second.
    [[nodiscard]] bool TryParsePortRangeText(
        const std::string& rangeText,
        std::pair<std::uint16_t, std::uint16_t>* rangeOut,
        std::string* normalizedTextOut = nullptr);
} // namespace ks::network
