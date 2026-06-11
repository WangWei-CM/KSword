#pragma once

// ============================================================
// ksword/network/network_diagnostics_tools.h
// Namespace: ks::network
// Purpose:
// - Provide non-UI helpers used by ARP/DNS/ICMP diagnostic views.
// - Keep row formatting, MAC parsing, and host-scan arithmetic out of NetworkDock.
// - Avoid Qt dependencies; all text uses std::string.
// ============================================================

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace ks::network
{
    // Ipv4ScanRange stores a normalized inclusive IPv4 scan range.
    // withinLimit is false when hostCount exceeds the caller-provided maximum.
    struct Ipv4ScanRange
    {
        std::uint32_t beginHostOrder = 0;
        std::uint32_t endHostOrder = 0;
        std::uint64_t hostCount = 0;
        bool withinLimit = true;
    };

    // FormatHardwareAddress renders raw hardware bytes using an ASCII separator.
    // Empty inputs return an empty string because some ARP rows may omit a MAC.
    [[nodiscard]] std::string FormatHardwareAddress(
        const std::uint8_t* addressBytes,
        std::size_t addressLength,
        char separator = '-');

    // TryParseMacAddressText parses AA-BB-CC-DD-EE-FF or AA:BB:CC:DD:EE:FF text.
    // On success macBytesOut contains exactly six bytes.
    [[nodiscard]] bool TryParseMacAddressText(
        const std::string& macText,
        std::vector<std::uint8_t>* macBytesOut,
        std::string* errorTextOut = nullptr);

    // ArpEntryTypeToString maps MIB_IPNET_TYPE_* numeric values to stable text.
    // Unknown values are reported as "Unknown".
    [[nodiscard]] std::string ArpEntryTypeToString(std::uint32_t typeCode);

    // FormatDnsFlags renders DNS cache flags in conventional uppercase hex.
    // The output includes a 0x prefix for direct UI/log display.
    [[nodiscard]] std::string FormatDnsFlags(std::uint32_t flagsValue);

    // NormalizeIpv4ScanRange sorts endpoints and computes the inclusive host count.
    // maxHostCount is used only to fill withinLimit; zero disables the limit check.
    [[nodiscard]] Ipv4ScanRange NormalizeIpv4ScanRange(
        std::uint32_t firstHostOrder,
        std::uint32_t secondHostOrder,
        std::uint64_t maxHostCount);

    // CalculateIntegerProgressPercent returns floor(done*100/total) in [0,100].
    // total==0 returns 0 because there is no active diagnostic scan to complete.
    [[nodiscard]] int CalculateIntegerProgressPercent(
        std::uint64_t doneCount,
        std::uint64_t totalCount);

    // FormatIcmpEchoDetail builds a compact detail string from ICMP echo output.
    // Alive replies report TTL; other replies report the raw ICMP status code.
    [[nodiscard]] std::string FormatIcmpEchoDetail(
        bool alive,
        std::uint32_t statusCode,
        std::uint8_t ttlValue);
} // namespace ks::network
