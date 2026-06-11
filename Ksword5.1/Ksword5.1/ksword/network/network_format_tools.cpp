#include "network_format_tools.h"

#include <algorithm>
#include <cctype>
#include <ctime>
#include <iomanip>
#include <sstream>

namespace ks::network
{
    namespace
    {
        // kHexBytesPerRowDefault is used when callers pass an invalid row width.
        constexpr std::size_t kHexBytesPerRowDefault = 16;

        // IsVisibleAsciiByte identifies bytes that can be displayed without escaping.
        // It intentionally limits output to the portable printable ASCII range.
        bool IsVisibleAsciiByte(const std::uint8_t byteValue)
        {
            return byteValue >= 32 && byteValue <= 126;
        }

        // NormalizeAsciiCharForPreview maps raw bytes to compact preview characters.
        // Whitespace used by text protocols is folded to spaces; binary bytes become dots.
        char NormalizeAsciiCharForPreview(const std::uint8_t byteValue)
        {
            if (IsVisibleAsciiByte(byteValue))
            {
                return static_cast<char>(byteValue);
            }
            if (byteValue == '\r' || byteValue == '\n' || byteValue == '\t')
            {
                return ' ';
            }
            return '.';
        }

        // TrimAscii removes leading and trailing ASCII whitespace from a string.
        // The function is deliberately small and locale-independent for parser stability.
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

        // SimplifyWhitespace collapses runs of ASCII whitespace to a single space.
        // The result is compact enough for packet preview reuse.
        std::string SimplifyWhitespace(const std::string& inputText)
        {
            std::string outputText;
            outputText.reserve(inputText.size());
            bool pendingSpace = false;
            for (const char currentChar : inputText)
            {
                if (std::isspace(static_cast<unsigned char>(currentChar)) != 0)
                {
                    pendingSpace = !outputText.empty();
                    continue;
                }
                if (pendingSpace)
                {
                    outputText.push_back(' ');
                    pendingSpace = false;
                }
                outputText.push_back(currentChar);
            }
            return TrimAscii(outputText);
        }

        // SplitOnce separates text around the first delimiter occurrence.
        // Returning false means the delimiter is absent or at an unusable position.
        bool SplitOnce(
            const std::string& inputText,
            const char delimiter,
            std::string* leftOut,
            std::string* rightOut)
        {
            const std::size_t delimiterIndex = inputText.find(delimiter);
            if (delimiterIndex == std::string::npos || delimiterIndex == 0)
            {
                return false;
            }
            if (leftOut != nullptr)
            {
                *leftOut = TrimAscii(inputText.substr(0, delimiterIndex));
            }
            if (rightOut != nullptr)
            {
                *rightOut = TrimAscii(inputText.substr(delimiterIndex + 1));
            }
            return true;
        }

        // TryParseUnsignedDecimal parses a non-negative decimal integer.
        // maxValue bounds the accepted value and avoids silent integer truncation.
        bool TryParseUnsignedDecimal(
            const std::string& inputText,
            const unsigned long long maxValue,
            unsigned long long* valueOut)
        {
            const std::string trimmedText = TrimAscii(inputText);
            if (trimmedText.empty() || valueOut == nullptr)
            {
                return false;
            }

            unsigned long long value = 0;
            for (const char currentChar : trimmedText)
            {
                if (!std::isdigit(static_cast<unsigned char>(currentChar)))
                {
                    return false;
                }
                value = (value * 10ULL) + static_cast<unsigned long long>(currentChar - '0');
                if (value > maxValue)
                {
                    return false;
                }
            }

            *valueOut = value;
            return true;
        }

        // AppendHexByte appends one uppercase two-digit hexadecimal byte to a stream.
        // Width/fill are set each time because iostream manipulators are sticky.
        void AppendHexByte(std::ostringstream& stream, const std::uint8_t byteValue)
        {
            stream << std::uppercase << std::hex << std::setw(2) << std::setfill('0')
                << static_cast<unsigned int>(byteValue)
                << std::dec << std::setfill(' ');
        }

        // AppendReadableSegmentIfUseful stores a simplified text segment if it is long enough.
        // Short fragments are ignored because they are often random printable binary bytes.
        void AppendReadableSegmentIfUseful(
            const std::string& segmentText,
            std::vector<std::string>& readableSegments)
        {
            const std::string simplifiedText = SimplifyWhitespace(segmentText);
            if (simplifiedText.size() >= 4)
            {
                readableSegments.push_back(simplifiedText);
            }
        }
    } // namespace

    std::string FormatEndpointText(const std::string& ipAddress, const std::uint16_t portNumber)
    {
        // IPv6 literals contain ':' and need brackets before appending the port separator.
        if (ipAddress.find(':') != std::string::npos)
        {
            return "[" + ipAddress + "]:" + std::to_string(portNumber);
        }
        return ipAddress + ":" + std::to_string(portNumber);
    }

    std::string FormatByteCount(const std::uint64_t bytesValue)
    {
        // Unit selection follows powers of 1024 to match binary transfer counters.
        static const char* kUnitList[] = { "B", "KB", "MB", "GB", "TB" };
        double normalizedValue = static_cast<double>(bytesValue);
        int unitIndex = 0;
        while (normalizedValue >= 1024.0 && unitIndex < 4)
        {
            normalizedValue /= 1024.0;
            ++unitIndex;
        }

        std::ostringstream stream;
        if (unitIndex == 0)
        {
            stream << bytesValue << " B";
        }
        else
        {
            stream << std::fixed << std::setprecision(2) << normalizedValue << ' ' << kUnitList[unitIndex];
        }
        return stream.str();
    }

    std::string FormatBytesPerSecond(const std::uint64_t bytesPerSecond)
    {
        return FormatByteCount(bytesPerSecond) + "/s";
    }

    std::string FormatUnixTimestampMs(const std::uint64_t timestampMs, const bool includeDate)
    {
        const std::time_t seconds = static_cast<std::time_t>(timestampMs / 1000ULL);
        const unsigned int milliseconds = static_cast<unsigned int>(timestampMs % 1000ULL);

        std::tm localTime{};
#if defined(_WIN32)
        localtime_s(&localTime, &seconds);
#else
        localtime_r(&seconds, &localTime);
#endif

        std::ostringstream stream;
        stream << std::setfill('0');
        if (includeDate)
        {
            stream << std::setw(4) << (localTime.tm_year + 1900) << '-'
                << std::setw(2) << (localTime.tm_mon + 1) << '-'
                << std::setw(2) << localTime.tm_mday << ' ';
        }
        stream << std::setw(2) << localTime.tm_hour << ':'
            << std::setw(2) << localTime.tm_min << ':'
            << std::setw(2) << localTime.tm_sec << '.'
            << std::setw(3) << milliseconds;
        return stream.str();
    }

    std::string FormatPercent(const double percentValue, const int decimals)
    {
        const int safeDecimals = std::clamp(decimals, 0, 6);
        const double safePercent = std::clamp(percentValue, 0.0, 100.0);
        std::ostringstream stream;
        stream << std::fixed << std::setprecision(safeDecimals) << safePercent;
        return stream.str();
    }

    std::string FormatBytesToHexPreview(
        const std::vector<std::uint8_t>& byteArray,
        const std::size_t maxBytesToRender)
    {
        if (byteArray.empty())
        {
            return "<empty>";
        }

        const std::size_t renderLength = std::min<std::size_t>(byteArray.size(), maxBytesToRender);
        std::ostringstream stream;
        for (std::size_t index = 0; index < renderLength; ++index)
        {
            if (index > 0)
            {
                stream << ' ';
            }
            AppendHexByte(stream, byteArray[index]);
        }
        if (renderLength < byteArray.size())
        {
            stream << " ... (total=" << byteArray.size() << " bytes)";
        }
        return stream.str();
    }

    PayloadByteRange BuildPayloadByteRange(const PacketRecord& packetRecord)
    {
        if (packetRecord.packetBytes.empty() || packetRecord.payloadOffset >= packetRecord.packetBytes.size())
        {
            return {};
        }

        const std::size_t maxReadableLength = packetRecord.packetBytes.size() - packetRecord.payloadOffset;
        const std::size_t expectedPayloadLength = packetRecord.payloadSize == 0
            ? maxReadableLength
            : std::min<std::size_t>(static_cast<std::size_t>(packetRecord.payloadSize), maxReadableLength);
        return PayloadByteRange{ packetRecord.payloadOffset, expectedPayloadLength };
    }

    std::string BuildPayloadAsciiPreviewText(
        const PacketRecord& packetRecord,
        const std::size_t previewByteLimit)
    {
        const PayloadByteRange payloadRange = BuildPayloadByteRange(packetRecord);
        if (payloadRange.length == 0)
        {
            return "<empty>";
        }

        const std::size_t previewLength = std::min<std::size_t>(payloadRange.length, previewByteLimit);
        std::vector<std::string> readableSegments;
        std::string currentSegment;
        currentSegment.reserve(previewLength);

        for (std::size_t index = 0; index < previewLength; ++index)
        {
            const std::uint8_t byteValue = packetRecord.packetBytes[payloadRange.offset + index];
            if (IsVisibleAsciiByte(byteValue) || byteValue == ' ' || byteValue == '\t')
            {
                currentSegment.push_back(IsVisibleAsciiByte(byteValue) ? static_cast<char>(byteValue) : ' ');
                continue;
            }

            AppendReadableSegmentIfUseful(currentSegment, readableSegments);
            currentSegment.clear();
        }
        AppendReadableSegmentIfUseful(currentSegment, readableSegments);

        std::string previewText;
        if (!readableSegments.empty())
        {
            for (std::size_t index = 0; index < readableSegments.size(); ++index)
            {
                if (index > 0)
                {
                    previewText += " | ";
                }
                previewText += readableSegments[index];
            }
        }
        else
        {
            previewText.reserve(previewLength);
            for (std::size_t index = 0; index < previewLength; ++index)
            {
                previewText.push_back(NormalizeAsciiCharForPreview(packetRecord.packetBytes[payloadRange.offset + index]));
            }
            previewText = SimplifyWhitespace(previewText);
        }

        if (previewText.empty())
        {
            previewText = "<binary payload>";
        }
        if (previewLength < payloadRange.length)
        {
            previewText += " ...";
        }
        if (packetRecord.packetBytesTruncated)
        {
            previewText += " [truncated]";
        }
        return previewText;
    }

    std::string BuildPayloadAsciiFullText(const PacketRecord& packetRecord)
    {
        const PayloadByteRange payloadRange = BuildPayloadByteRange(packetRecord);
        if (payloadRange.length == 0)
        {
            return "<empty>";
        }

        std::string asciiText;
        asciiText.reserve(payloadRange.length);
        for (std::size_t index = 0; index < payloadRange.length; ++index)
        {
            const std::uint8_t byteValue = packetRecord.packetBytes[payloadRange.offset + index];
            if (byteValue == '\r')
            {
                if (index + 1 < payloadRange.length && packetRecord.packetBytes[payloadRange.offset + index + 1] == '\n')
                {
                    ++index;
                }
                asciiText.push_back('\n');
                continue;
            }
            if (byteValue == '\n' || byteValue == '\t')
            {
                asciiText.push_back(static_cast<char>(byteValue));
                continue;
            }
            asciiText.push_back(IsVisibleAsciiByte(byteValue) ? static_cast<char>(byteValue) : '.');
        }

        if (packetRecord.packetBytesTruncated)
        {
            asciiText += "\n[truncated capture]";
        }
        return asciiText;
    }

    std::string BuildPayloadHexFullText(const PacketRecord& packetRecord)
    {
        const PayloadByteRange payloadRange = BuildPayloadByteRange(packetRecord);
        if (payloadRange.length == 0)
        {
            return std::string();
        }

        std::ostringstream stream;
        for (std::size_t index = 0; index < payloadRange.length; ++index)
        {
            if (index > 0)
            {
                stream << ' ';
            }
            AppendHexByte(stream, packetRecord.packetBytes[payloadRange.offset + index]);
        }
        return stream.str();
    }

    std::string BuildPacketHexAsciiDumpText(
        const PacketRecord& packetRecord,
        const std::size_t bytesPerRow)
    {
        const std::size_t safeBytesPerRow = bytesPerRow == 0 ? kHexBytesPerRowDefault : bytesPerRow;
        const std::vector<std::uint8_t>& packetBytes = packetRecord.packetBytes;
        if (packetBytes.empty())
        {
            return "00000000  -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- --  |<empty>|";
        }

        std::ostringstream stream;
        stream << "Offset(h)  Hex bytes";
        if (safeBytesPerRow < 16)
        {
            stream << "\n";
        }
        else
        {
            stream << "                                              ASCII\n";
        }
        const std::size_t rowCount = (packetBytes.size() + safeBytesPerRow - 1) / safeBytesPerRow;
        for (std::size_t rowIndex = 0; rowIndex < rowCount; ++rowIndex)
        {
            const std::size_t rowOffset = rowIndex * safeBytesPerRow;
            stream << std::uppercase << std::hex << std::setw(8) << std::setfill('0') << rowOffset
                << std::dec << std::setfill(' ') << "  ";

            std::string asciiColumn;
            asciiColumn.reserve(safeBytesPerRow);
            for (std::size_t byteColumn = 0; byteColumn < safeBytesPerRow; ++byteColumn)
            {
                const std::size_t byteIndex = rowOffset + byteColumn;
                if (byteIndex < packetBytes.size())
                {
                    AppendHexByte(stream, packetBytes[byteIndex]);
                    asciiColumn.push_back(IsVisibleAsciiByte(packetBytes[byteIndex]) ? static_cast<char>(packetBytes[byteIndex]) : '.');
                }
                else
                {
                    stream << "  ";
                    asciiColumn.push_back(' ');
                }
                if (byteColumn + 1 < safeBytesPerRow)
                {
                    stream << ' ';
                }
            }
            stream << "  |" << asciiColumn << '|';
            if (rowIndex + 1 < rowCount)
            {
                stream << '\n';
            }
        }

        if (packetRecord.packetBytesTruncated)
        {
            stream << "\n[truncated capture: original bytes exceed retain limit]";
        }
        return stream.str();
    }

    std::string BuildPacketCopyHeaderLine(const PacketRecord& packetRecord)
    {
        std::ostringstream stream;
        stream << '#' << packetRecord.sequenceId
            << " | time=" << FormatUnixTimestampMs(packetRecord.captureTimestampMs, true)
            << " | pid=" << packetRecord.processId
            << " | protocol=" << PacketProtocolToString(packetRecord.protocol)
            << " | direction=" << PacketDirectionToString(packetRecord.direction)
            << " | local=" << FormatEndpointText(packetRecord.localAddress, packetRecord.localPort)
            << " | remote=" << FormatEndpointText(packetRecord.remoteAddress, packetRecord.remotePort)
            << " | length=" << packetRecord.payloadSize << '/' << packetRecord.totalPacketSize;
        return stream.str();
    }

    std::string FormatIpv4HostOrder(const std::uint32_t ipv4HostOrder)
    {
        std::ostringstream stream;
        stream << ((ipv4HostOrder >> 24) & 0xFFU) << '.'
            << ((ipv4HostOrder >> 16) & 0xFFU) << '.'
            << ((ipv4HostOrder >> 8) & 0xFFU) << '.'
            << (ipv4HostOrder & 0xFFU);
        return stream.str();
    }

    bool TryParseIpv4Text(const std::string& ipv4Text, std::uint32_t* const ipv4HostOrderOut)
    {
        if (ipv4HostOrderOut == nullptr)
        {
            return false;
        }

        const std::string trimmedText = TrimAscii(ipv4Text);
        if (trimmedText.empty())
        {
            return false;
        }

        std::uint32_t ipv4Value = 0;
        std::size_t segmentBegin = 0;
        int segmentCount = 0;
        while (segmentBegin <= trimmedText.size())
        {
            const std::size_t dotIndex = trimmedText.find('.', segmentBegin);
            const std::size_t segmentEnd = dotIndex == std::string::npos ? trimmedText.size() : dotIndex;
            if (segmentEnd == segmentBegin)
            {
                return false;
            }

            unsigned long long segmentValue = 0;
            if (!TryParseUnsignedDecimal(trimmedText.substr(segmentBegin, segmentEnd - segmentBegin), 255ULL, &segmentValue))
            {
                return false;
            }

            ipv4Value = (ipv4Value << 8) | static_cast<std::uint32_t>(segmentValue);
            ++segmentCount;
            if (dotIndex == std::string::npos)
            {
                break;
            }
            segmentBegin = dotIndex + 1;
        }

        if (segmentCount != 4)
        {
            return false;
        }
        *ipv4HostOrderOut = ipv4Value;
        return true;
    }

    bool TryParseIpv4RangeText(
        const std::string& rangeText,
        std::pair<std::uint32_t, std::uint32_t>* const rangeOut,
        std::string* const normalizedTextOut)
    {
        if (rangeOut == nullptr)
        {
            return false;
        }

        const std::string trimmedText = TrimAscii(rangeText);
        if (trimmedText.empty())
        {
            return false;
        }

        std::string leftText;
        std::string rightText;
        if (SplitOnce(trimmedText, '/', &leftText, &rightText))
        {
            std::uint32_t baseIpHostOrder = 0;
            unsigned long long prefixValue = 0;
            if (!TryParseIpv4Text(leftText, &baseIpHostOrder) ||
                !TryParseUnsignedDecimal(rightText, 32ULL, &prefixValue))
            {
                return false;
            }

            const unsigned int prefixLength = static_cast<unsigned int>(prefixValue);
            const std::uint32_t netmask = prefixLength == 0
                ? 0U
                : (0xFFFFFFFFU << static_cast<unsigned int>(32U - prefixLength));
            const std::uint32_t rangeBegin = baseIpHostOrder & netmask;
            const std::uint32_t rangeEnd = rangeBegin | (~netmask);
            *rangeOut = { rangeBegin, rangeEnd };
            if (normalizedTextOut != nullptr)
            {
                *normalizedTextOut = FormatIpv4HostOrder(rangeBegin) + "/" + std::to_string(prefixLength);
            }
            return true;
        }

        if (SplitOnce(trimmedText, '-', &leftText, &rightText))
        {
            std::uint32_t beginIpHostOrder = 0;
            std::uint32_t endIpHostOrder = 0;
            if (!TryParseIpv4Text(leftText, &beginIpHostOrder) ||
                !TryParseIpv4Text(rightText, &endIpHostOrder))
            {
                return false;
            }

            const std::uint32_t normalizedBegin = std::min(beginIpHostOrder, endIpHostOrder);
            const std::uint32_t normalizedEnd = std::max(beginIpHostOrder, endIpHostOrder);
            *rangeOut = { normalizedBegin, normalizedEnd };
            if (normalizedTextOut != nullptr)
            {
                *normalizedTextOut = FormatIpv4HostOrder(normalizedBegin) + "-" + FormatIpv4HostOrder(normalizedEnd);
            }
            return true;
        }

        std::uint32_t singleIpHostOrder = 0;
        if (!TryParseIpv4Text(trimmedText, &singleIpHostOrder))
        {
            return false;
        }
        *rangeOut = { singleIpHostOrder, singleIpHostOrder };
        if (normalizedTextOut != nullptr)
        {
            *normalizedTextOut = FormatIpv4HostOrder(singleIpHostOrder);
        }
        return true;
    }

    bool TryParsePortRangeText(
        const std::string& rangeText,
        std::pair<std::uint16_t, std::uint16_t>* const rangeOut,
        std::string* const normalizedTextOut)
    {
        if (rangeOut == nullptr)
        {
            return false;
        }

        const std::string trimmedText = TrimAscii(rangeText);
        if (trimmedText.empty())
        {
            return false;
        }

        std::string leftText;
        std::string rightText;
        if (SplitOnce(trimmedText, '-', &leftText, &rightText))
        {
            unsigned long long beginPortValue = 0;
            unsigned long long endPortValue = 0;
            if (!TryParseUnsignedDecimal(leftText, 65535ULL, &beginPortValue) ||
                !TryParseUnsignedDecimal(rightText, 65535ULL, &endPortValue))
            {
                return false;
            }

            const std::uint16_t normalizedBegin = static_cast<std::uint16_t>(std::min(beginPortValue, endPortValue));
            const std::uint16_t normalizedEnd = static_cast<std::uint16_t>(std::max(beginPortValue, endPortValue));
            *rangeOut = { normalizedBegin, normalizedEnd };
            if (normalizedTextOut != nullptr)
            {
                *normalizedTextOut = std::to_string(normalizedBegin) + "-" + std::to_string(normalizedEnd);
            }
            return true;
        }

        unsigned long long singlePortValue = 0;
        if (!TryParseUnsignedDecimal(trimmedText, 65535ULL, &singlePortValue))
        {
            return false;
        }
        const std::uint16_t singlePort = static_cast<std::uint16_t>(singlePortValue);
        *rangeOut = { singlePort, singlePort };
        if (normalizedTextOut != nullptr)
        {
            *normalizedTextOut = std::to_string(singlePort);
        }
        return true;
    }
} // namespace ks::network
