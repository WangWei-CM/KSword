#include "network_diagnostics_tools.h"

#include <algorithm>
#include <cctype>
#include <iomanip>
#include <sstream>

namespace ks::network
{
    namespace
    {
        // These values mirror Windows MIB_IPNET_TYPE_* constants without requiring Win32 headers.
        constexpr std::uint32_t kArpTypeInvalid = 2;
        constexpr std::uint32_t kArpTypeDynamic = 3;
        constexpr std::uint32_t kArpTypeStatic = 4;

        // TrimAscii keeps parser behavior independent from locale-specific whitespace rules.
        std::string TrimAscii(const std::string& inputText)
        {
            std::size_t beginIndex = 0;
            while (beginIndex < inputText.size() &&
                std::isspace(static_cast<unsigned char>(inputText[beginIndex])) != 0)
            {
                ++beginIndex;
            }

            std::size_t endIndex = inputText.size();
            while (endIndex > beginIndex &&
                std::isspace(static_cast<unsigned char>(inputText[endIndex - 1])) != 0)
            {
                --endIndex;
            }
            return inputText.substr(beginIndex, endIndex - beginIndex);
        }

        // HexNibbleValue converts one hex character to a numeric nibble.
        // It returns -1 for invalid input so callers can produce precise errors.
        int HexNibbleValue(const char currentChar)
        {
            if (currentChar >= '0' && currentChar <= '9')
            {
                return currentChar - '0';
            }
            if (currentChar >= 'a' && currentChar <= 'f')
            {
                return currentChar - 'a' + 10;
            }
            if (currentChar >= 'A' && currentChar <= 'F')
            {
                return currentChar - 'A' + 10;
            }
            return -1;
        }
    } // namespace

    std::string FormatHardwareAddress(
        const std::uint8_t* const addressBytes,
        const std::size_t addressLength,
        const char separator)
    {
        if (addressBytes == nullptr || addressLength == 0)
        {
            return std::string();
        }

        std::ostringstream stream;
        for (std::size_t index = 0; index < addressLength; ++index)
        {
            if (index > 0)
            {
                stream << separator;
            }
            stream << std::uppercase << std::hex << std::setw(2) << std::setfill('0')
                << static_cast<unsigned int>(addressBytes[index])
                << std::dec << std::setfill(' ');
        }
        return stream.str();
    }

    bool TryParseMacAddressText(
        const std::string& macText,
        std::vector<std::uint8_t>* const macBytesOut,
        std::string* const errorTextOut)
    {
        if (macBytesOut == nullptr)
        {
            return false;
        }
        macBytesOut->clear();
        if (errorTextOut != nullptr)
        {
            errorTextOut->clear();
        }

        std::string normalizedText = TrimAscii(macText);
        std::replace(normalizedText.begin(), normalizedText.end(), ':', '-');
        if (normalizedText.empty())
        {
            if (errorTextOut != nullptr)
            {
                *errorTextOut = "MAC address is empty.";
            }
            return false;
        }

        std::size_t segmentBegin = 0;
        while (segmentBegin <= normalizedText.size())
        {
            const std::size_t separatorIndex = normalizedText.find('-', segmentBegin);
            const std::size_t segmentEnd = separatorIndex == std::string::npos ? normalizedText.size() : separatorIndex;
            if (segmentEnd - segmentBegin != 2)
            {
                if (errorTextOut != nullptr)
                {
                    *errorTextOut = "MAC address must contain six two-digit hex segments.";
                }
                macBytesOut->clear();
                return false;
            }

            const int highNibble = HexNibbleValue(normalizedText[segmentBegin]);
            const int lowNibble = HexNibbleValue(normalizedText[segmentBegin + 1]);
            if (highNibble < 0 || lowNibble < 0)
            {
                if (errorTextOut != nullptr)
                {
                    *errorTextOut = "MAC address contains non-hex characters.";
                }
                macBytesOut->clear();
                return false;
            }
            macBytesOut->push_back(static_cast<std::uint8_t>((highNibble << 4) | lowNibble));

            if (separatorIndex == std::string::npos)
            {
                break;
            }
            segmentBegin = separatorIndex + 1;
        }

        if (macBytesOut->size() != 6)
        {
            if (errorTextOut != nullptr)
            {
                *errorTextOut = "MAC address must contain exactly six segments.";
            }
            macBytesOut->clear();
            return false;
        }
        return true;
    }

    std::string ArpEntryTypeToString(const std::uint32_t typeCode)
    {
        switch (typeCode)
        {
        case kArpTypeDynamic:
            return "Dynamic";
        case kArpTypeStatic:
            return "Static";
        case kArpTypeInvalid:
            return "Invalid";
        default:
            return "Unknown";
        }
    }

    std::string FormatDnsFlags(const std::uint32_t flagsValue)
    {
        std::ostringstream stream;
        stream << "0x" << std::uppercase << std::hex << flagsValue;
        return stream.str();
    }

    Ipv4ScanRange NormalizeIpv4ScanRange(
        const std::uint32_t firstHostOrder,
        const std::uint32_t secondHostOrder,
        const std::uint64_t maxHostCount)
    {
        Ipv4ScanRange range;
        range.beginHostOrder = std::min(firstHostOrder, secondHostOrder);
        range.endHostOrder = std::max(firstHostOrder, secondHostOrder);
        range.hostCount = static_cast<std::uint64_t>(range.endHostOrder) - range.beginHostOrder + 1ULL;
        range.withinLimit = maxHostCount == 0 || range.hostCount <= maxHostCount;
        return range;
    }

    int CalculateIntegerProgressPercent(
        const std::uint64_t doneCount,
        const std::uint64_t totalCount)
    {
        if (totalCount == 0)
        {
            return 0;
        }
        const std::uint64_t clampedDone = std::min(doneCount, totalCount);
        // 使用 long double 避免 done*100 在极大计数下溢出，同时保持向下取整语义。
        const long double progressValue =
            (static_cast<long double>(clampedDone) * 100.0L) /
            static_cast<long double>(totalCount);
        return static_cast<int>(std::clamp(progressValue, 0.0L, 100.0L));
    }

    std::string FormatIcmpEchoDetail(
        const bool alive,
        const std::uint32_t statusCode,
        const std::uint8_t ttlValue)
    {
        if (alive)
        {
            return "TTL=" + std::to_string(static_cast<unsigned int>(ttlValue));
        }
        return "Status=" + std::to_string(statusCode);
    }
} // namespace ks::network
