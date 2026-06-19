#include "network_nids.h"

#include "network_format_tools.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cctype>
#include <iomanip>
#include <limits>
#include <sstream>

namespace ks::network
{
    namespace
    {
        constexpr std::uint64_t kPortScanWindowMs = 15000;
        constexpr std::uint64_t kPortScanAlertCooldownMs = 30000;
        constexpr std::uint64_t kFlowAlertCooldownMs = 60000;
        constexpr std::uint64_t kOutboundByteWindowMs = 30000;
        constexpr std::uint64_t kOutboundByteThreshold = 8ULL * 1024ULL * 1024ULL;

        // makeBaseAlert：用报文公共字段生成告警基础结构。
        NidsAlert makeBaseAlert(
            const PacketRecord& packetRecord,
            const NidsAlertSeverity severity,
            std::string category,
            std::string ruleId,
            std::string title,
            std::string detail)
        {
            NidsAlert alert;
            alert.timestampMs = packetRecord.captureTimestampMs;
            alert.sequenceId = packetRecord.sequenceId;
            alert.severity = severity;
            alert.category = std::move(category);
            alert.ruleId = std::move(ruleId);
            alert.title = std::move(title);
            alert.detail = std::move(detail);
            alert.processId = packetRecord.processId;
            alert.processName = packetRecord.processName;
            alert.protocol = packetRecord.protocol;
            alert.direction = packetRecord.direction;
            alert.localAddress = packetRecord.localAddress;
            alert.localPort = packetRecord.localPort;
            alert.remoteAddress = packetRecord.remoteAddress;
            alert.remotePort = packetRecord.remotePort;
            return alert;
        }

        // toLowerAscii：仅转换 ASCII，避免引入本地化依赖。
        std::string toLowerAscii(std::string text)
        {
            for (char& ch : text)
            {
                ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
            }
            return text;
        }

        // containsCaseInsensitive：大小写不敏感子串匹配。
        bool containsCaseInsensitive(const std::string& haystack, const std::string& needle)
        {
            if (needle.empty())
            {
                return true;
            }
            return toLowerAscii(haystack).find(toLowerAscii(needle)) != std::string::npos;
        }

        // startsWithCaseInsensitive：大小写不敏感前缀匹配。
        bool startsWithCaseInsensitive(const std::string& text, const std::string& prefix)
        {
            if (text.size() < prefix.size())
            {
                return false;
            }
            for (std::size_t index = 0; index < prefix.size(); ++index)
            {
                const char left = static_cast<char>(std::tolower(static_cast<unsigned char>(text[index])));
                const char right = static_cast<char>(std::tolower(static_cast<unsigned char>(prefix[index])));
                if (left != right)
                {
                    return false;
                }
            }
            return true;
        }

        // buildEndpointKey：构造 flow 去重键。
        std::string buildEndpointKey(const PacketRecord& packetRecord)
        {
            std::ostringstream stream;
            stream << static_cast<int>(packetRecord.protocol)
                << '|'
                << static_cast<int>(packetRecord.direction)
                << '|'
                << packetRecord.processId
                << '|'
                << packetRecord.localAddress
                << ':'
                << packetRecord.localPort
                << '|'
                << packetRecord.remoteAddress
                << ':'
                << packetRecord.remotePort;
            return stream.str();
        }

        // appendPortWindowEvent：维护固定时窗内端口事件。
        void appendPortWindowEvent(
            NidsEngine::PortScanWindow& window,
            const std::uint64_t nowMs,
            const std::uint16_t port)
        {
            window.eventList.push_back({ nowMs, port });
            while (!window.eventList.empty() &&
                nowMs > window.eventList.front().timestampMs &&
                nowMs - window.eventList.front().timestampMs > kPortScanWindowMs)
            {
                window.eventList.pop_front();
            }
        }

        // countDistinctPorts：统计当前扫描窗口内不同端口数。
        std::size_t countDistinctPorts(const std::deque<NidsEngine::PortEvent>& eventList)
        {
            std::unordered_set<std::uint16_t> portSet;
            portSet.reserve(eventList.size());
            for (const NidsEngine::PortEvent& eventItem : eventList)
            {
                portSet.insert(eventItem.port);
            }
            return portSet.size();
        }

        // tryReadTcpFlags：从保留的 IP 包字节中读取 TCP flags。
        bool tryReadTcpFlags(const PacketRecord& packetRecord, std::uint8_t& flagsOut)
        {
            if (packetRecord.protocol != PacketTransportProtocol::Tcp ||
                packetRecord.packetBytes.size() < 20)
            {
                return false;
            }

            const std::uint8_t version = static_cast<std::uint8_t>(packetRecord.packetBytes[0] >> 4);
            std::size_t tcpOffset = 0;
            if (version == 4)
            {
                const std::size_t ipv4HeaderLength = static_cast<std::size_t>(packetRecord.packetBytes[0] & 0x0F) * 4;
                if (ipv4HeaderLength < 20 || packetRecord.packetBytes.size() < ipv4HeaderLength + 14)
                {
                    return false;
                }
                tcpOffset = ipv4HeaderLength;
            }
            else if (version == 6)
            {
                if (packetRecord.packetBytes.size() < 54 || packetRecord.packetBytes[6] != 6)
                {
                    return false;
                }
                tcpOffset = 40;
            }
            else
            {
                return false;
            }

            flagsOut = packetRecord.packetBytes[tcpOffset + 13];
            return true;
        }

        // looksLikeTcpProbe：判断 TCP 报文是否像端口探测。
        bool looksLikeTcpProbe(const PacketRecord& packetRecord)
        {
            std::uint8_t flags = 0;
            if (tryReadTcpFlags(packetRecord, flags))
            {
                constexpr std::uint8_t synFlag = 0x02;
                constexpr std::uint8_t ackFlag = 0x10;
                return (flags & synFlag) != 0 && (flags & ackFlag) == 0;
            }

            return packetRecord.payloadSize == 0 && packetRecord.totalPacketSize <= 96;
        }

        // buildScanKey：构造端口扫描行为窗口键。
        std::string buildScanKey(const PacketRecord& packetRecord)
        {
            const bool inbound = packetRecord.direction == PacketDirection::Inbound;
            const std::string& actorAddress = inbound ? packetRecord.remoteAddress : packetRecord.localAddress;
            const std::string& targetAddress = inbound ? packetRecord.localAddress : packetRecord.remoteAddress;

            std::ostringstream stream;
            stream << static_cast<int>(packetRecord.protocol)
                << '|'
                << static_cast<int>(packetRecord.direction)
                << '|'
                << packetRecord.processId
                << '|'
                << actorAddress
                << "->"
                << targetAddress;
            return stream.str();
        }

        // readPayloadBytes：返回当前保留字节中的 payload 安全视图。
        std::vector<std::uint8_t> readPayloadBytes(const PacketRecord& packetRecord, const std::size_t limitBytes)
        {
            const PayloadByteRange payloadRange = BuildPayloadByteRange(packetRecord);
            if (payloadRange.length == 0 ||
                payloadRange.offset >= packetRecord.packetBytes.size())
            {
                return {};
            }

            const std::size_t readLength = std::min<std::size_t>(payloadRange.length, limitBytes);
            const auto beginIterator = packetRecord.packetBytes.begin() + static_cast<std::ptrdiff_t>(payloadRange.offset);
            const auto endIterator = beginIterator + static_cast<std::ptrdiff_t>(readLength);
            return std::vector<std::uint8_t>(beginIterator, endIterator);
        }

        // readPayloadAscii：读取 payload 的可打印 ASCII 视图。
        std::string readPayloadAscii(const PacketRecord& packetRecord, const std::size_t limitBytes)
        {
            const std::vector<std::uint8_t> payloadBytes = readPayloadBytes(packetRecord, limitBytes);
            std::string output;
            output.reserve(payloadBytes.size());
            for (const std::uint8_t byteValue : payloadBytes)
            {
                if (byteValue == '\r' || byteValue == '\n' || byteValue == '\t' ||
                    (byteValue >= 0x20 && byteValue <= 0x7E))
                {
                    output.push_back(static_cast<char>(byteValue));
                }
                else
                {
                    output.push_back('.');
                }
            }
            return output;
        }

        // parseDnsQuestion：解析第一个 DNS question 域名与 qtype。
        bool parseDnsQuestion(
            const PacketRecord& packetRecord,
            std::string& domainOut,
            std::uint16_t& qtypeOut)
        {
            const std::vector<std::uint8_t> payloadBytes = readPayloadBytes(packetRecord, 512);
            if (payloadBytes.size() < 12)
            {
                return false;
            }

            const std::uint16_t questionCount = static_cast<std::uint16_t>(
                (static_cast<std::uint16_t>(payloadBytes[4]) << 8) |
                static_cast<std::uint16_t>(payloadBytes[5]));
            if (questionCount == 0)
            {
                return false;
            }

            std::size_t offset = 12;
            std::vector<std::string> labelList;
            labelList.reserve(8);
            while (offset < payloadBytes.size())
            {
                const std::uint8_t labelLength = payloadBytes[offset++];
                if (labelLength == 0)
                {
                    break;
                }
                if ((labelLength & 0xC0) != 0 || labelLength > 63)
                {
                    return false;
                }
                if (offset + labelLength > payloadBytes.size())
                {
                    return false;
                }

                std::string label;
                label.reserve(labelLength);
                for (std::uint8_t labelIndex = 0; labelIndex < labelLength; ++labelIndex)
                {
                    const char ch = static_cast<char>(payloadBytes[offset + labelIndex]);
                    if (std::isalnum(static_cast<unsigned char>(ch)) || ch == '-' || ch == '_')
                    {
                        label.push_back(ch);
                    }
                    else
                    {
                        label.push_back('.');
                    }
                }
                labelList.push_back(std::move(label));
                offset += labelLength;
            }

            if (labelList.empty() || offset + 4 > payloadBytes.size())
            {
                return false;
            }

            qtypeOut = static_cast<std::uint16_t>(
                (static_cast<std::uint16_t>(payloadBytes[offset]) << 8) |
                static_cast<std::uint16_t>(payloadBytes[offset + 1]));

            std::ostringstream domainStream;
            for (std::size_t index = 0; index < labelList.size(); ++index)
            {
                if (index != 0)
                {
                    domainStream << '.';
                }
                domainStream << labelList[index];
            }
            domainOut = domainStream.str();
            return !domainOut.empty();
        }

        // splitDomainLabels：按点拆分域名。
        std::vector<std::string> splitDomainLabels(const std::string& domainText)
        {
            std::vector<std::string> labelList;
            std::string currentLabel;
            for (const char ch : domainText)
            {
                if (ch == '.')
                {
                    if (!currentLabel.empty())
                    {
                        labelList.push_back(currentLabel);
                        currentLabel.clear();
                    }
                }
                else
                {
                    currentLabel.push_back(ch);
                }
            }
            if (!currentLabel.empty())
            {
                labelList.push_back(currentLabel);
            }
            return labelList;
        }

        // shannonEntropy：计算 ASCII 文本粗略熵值。
        double shannonEntropy(const std::string& text)
        {
            if (text.empty())
            {
                return 0.0;
            }

            std::array<std::size_t, 256> frequency{};
            for (const unsigned char ch : text)
            {
                ++frequency[ch];
            }

            double entropy = 0.0;
            const double length = static_cast<double>(text.size());
            for (const std::size_t count : frequency)
            {
                if (count == 0)
                {
                    continue;
                }
                const double probability = static_cast<double>(count) / length;
                entropy -= probability * std::log2(probability);
            }
            return entropy;
        }

        // looksRandomDnsLabel：判断 DNS label 是否呈随机/隧道化特征。
        bool looksRandomDnsLabel(const std::string& label)
        {
            if (label.size() < 24)
            {
                return false;
            }

            std::size_t digitCount = 0;
            std::size_t alphaCount = 0;
            std::unordered_set<char> uniqueCharSet;
            for (const char ch : label)
            {
                if (std::isdigit(static_cast<unsigned char>(ch)))
                {
                    ++digitCount;
                }
                if (std::isalpha(static_cast<unsigned char>(ch)))
                {
                    ++alphaCount;
                }
                uniqueCharSet.insert(static_cast<char>(std::tolower(static_cast<unsigned char>(ch))));
            }

            return alphaCount >= 12 &&
                digitCount >= 3 &&
                uniqueCharSet.size() >= 14 &&
                shannonEntropy(label) >= 3.6;
        }

        // hasSuffix：大小写不敏感后缀匹配。
        bool hasSuffix(const std::string& text, const std::string& suffix)
        {
            if (text.size() < suffix.size())
            {
                return false;
            }
            return startsWithCaseInsensitive(text.substr(text.size() - suffix.size()), suffix);
        }

        // buildRiskyPortTitle：返回风险端口说明。
        bool buildRiskyPortTitle(const std::uint16_t port, std::string& titleOut)
        {
            switch (port)
            {
            case 23:
            case 2323:
                titleOut = "Telnet 端口通信";
                return true;
            case 4444:
            case 1337:
            case 31337:
                titleOut = "常见后门端口通信";
                return true;
            case 6667:
                titleOut = "IRC/C2 常见端口通信";
                return true;
            case 5900:
                titleOut = "VNC 远程控制端口通信";
                return true;
            default:
                return false;
            }
        }

        // buildHttpFirstLine：截取 HTTP 请求首行。
        std::string buildHttpFirstLine(const std::string& payloadText)
        {
            const std::size_t endPosition = payloadText.find('\n');
            std::string firstLine = endPosition == std::string::npos
                ? payloadText
                : payloadText.substr(0, endPosition);
            while (!firstLine.empty() && (firstLine.back() == '\r' || firstLine.back() == '\n'))
            {
                firstLine.pop_back();
            }
            if (firstLine.size() > 220)
            {
                firstLine.resize(220);
                firstLine += "...";
            }
            return firstLine;
        }

        // looksLikeHttpRequest：判断 payload 是否为明文 HTTP 请求。
        bool looksLikeHttpRequest(const std::string& payloadText)
        {
            static const std::array<const char*, 9> kHttpMethods = {
                "GET ", "POST ", "PUT ", "DELETE ", "PATCH ", "HEAD ", "OPTIONS ", "TRACE ", "CONNECT "
            };
            for (const char* methodText : kHttpMethods)
            {
                if (startsWithCaseInsensitive(payloadText, methodText))
                {
                    return true;
                }
            }
            return false;
        }

        // buildPacketSummary：构造报文端点摘要。
        std::string buildPacketSummary(const PacketRecord& packetRecord)
        {
            std::ostringstream stream;
            stream << PacketProtocolToString(packetRecord.protocol)
                << ' '
                << PacketDirectionToString(packetRecord.direction)
                << ' '
                << FormatEndpointText(packetRecord.localAddress, packetRecord.localPort)
                << " -> "
                << FormatEndpointText(packetRecord.remoteAddress, packetRecord.remotePort);
            return stream.str();
        }

        // appendByteWindowEvent：维护出站字节固定时窗。
        void appendByteWindowEvent(
            NidsEngine::ByteWindow& window,
            const std::uint64_t nowMs,
            const std::uint64_t byteCount)
        {
            window.eventList.push_back({ nowMs, byteCount });
            window.totalBytes += byteCount;
            while (!window.eventList.empty() &&
                nowMs > window.eventList.front().timestampMs &&
                nowMs - window.eventList.front().timestampMs > kOutboundByteWindowMs)
            {
                window.totalBytes = window.totalBytes > window.eventList.front().byteCount
                    ? window.totalBytes - window.eventList.front().byteCount
                    : 0;
                window.eventList.pop_front();
            }
        }
    }

    std::string NidsAlertSeverityToString(const NidsAlertSeverity severity)
    {
        switch (severity)
        {
        case NidsAlertSeverity::Low:
            return "Low";
        case NidsAlertSeverity::Medium:
            return "Medium";
        case NidsAlertSeverity::High:
            return "High";
        case NidsAlertSeverity::Critical:
            return "Critical";
        default:
            return "Unknown";
        }
    }

    std::vector<NidsAlert> NidsEngine::AnalyzePacket(const PacketRecord& packetRecord)
    {
        std::vector<NidsAlert> alertList;
        analyzePortScan(packetRecord, alertList);
        analyzeDnsPayload(packetRecord, alertList);
        analyzeHttpPayload(packetRecord, alertList);
        analyzeSuspiciousPort(packetRecord, alertList);
        analyzeOutboundByteBurst(packetRecord, alertList);
        return alertList;
    }

    void NidsEngine::Reset()
    {
        m_portScanWindowByKey.clear();
        m_outboundByteWindowByKey.clear();
        m_flowAlertLastTimestampByKey.clear();
    }

    void NidsEngine::analyzePortScan(const PacketRecord& packetRecord, std::vector<NidsAlert>& alertList)
    {
        if (packetRecord.captureTimestampMs == 0 ||
            packetRecord.direction == PacketDirection::Unknown)
        {
            return;
        }

        std::uint16_t probePort = 0;
        std::size_t threshold = 0;
        std::string ruleId;
        if (packetRecord.protocol == PacketTransportProtocol::Tcp)
        {
            if (!looksLikeTcpProbe(packetRecord))
            {
                return;
            }
            probePort = packetRecord.direction == PacketDirection::Inbound
                ? packetRecord.localPort
                : packetRecord.remotePort;
            threshold = 18;
            ruleId = "NIDS-SCAN-TCP-PORT-BURST";
        }
        else if (packetRecord.protocol == PacketTransportProtocol::Udp)
        {
            probePort = packetRecord.direction == PacketDirection::Inbound
                ? packetRecord.localPort
                : packetRecord.remotePort;
            threshold = 24;
            ruleId = "NIDS-SCAN-UDP-PORT-BURST";
        }
        else
        {
            return;
        }

        if (probePort == 0)
        {
            return;
        }

        const std::string scanKey = buildScanKey(packetRecord);
        PortScanWindow& window = m_portScanWindowByKey[scanKey];
        appendPortWindowEvent(window, packetRecord.captureTimestampMs, probePort);

        const std::size_t distinctPortCount = countDistinctPorts(window.eventList);
        if (distinctPortCount < threshold)
        {
            return;
        }
        if (window.lastAlertTimestampMs != 0 &&
            packetRecord.captureTimestampMs > window.lastAlertTimestampMs &&
            packetRecord.captureTimestampMs - window.lastAlertTimestampMs < kPortScanAlertCooldownMs)
        {
            return;
        }
        window.lastAlertTimestampMs = packetRecord.captureTimestampMs;

        std::ostringstream detailStream;
        detailStream << buildPacketSummary(packetRecord)
            << ", "
            << kPortScanWindowMs / 1000
            << "s 内探测 "
            << distinctPortCount
            << " 个不同端口。";

        alertList.push_back(makeBaseAlert(
            packetRecord,
            NidsAlertSeverity::High,
            "Scan",
            ruleId,
            "短时端口扫描",
            detailStream.str()));
    }

    void NidsEngine::analyzeDnsPayload(const PacketRecord& packetRecord, std::vector<NidsAlert>& alertList)
    {
        if (packetRecord.protocol != PacketTransportProtocol::Udp ||
            packetRecord.payloadSize == 0 ||
            (packetRecord.localPort != 53 && packetRecord.remotePort != 53))
        {
            return;
        }

        std::string domainText;
        std::uint16_t qtype = 0;
        if (!parseDnsQuestion(packetRecord, domainText, qtype))
        {
            return;
        }

        const std::string domainLower = toLowerAscii(domainText);
        const std::string flowKey = buildEndpointKey(packetRecord) + "|dns|" + domainLower;
        std::vector<std::string> labelList = splitDomainLabels(domainText);

        std::size_t longestLabel = 0;
        bool randomLabelFound = false;
        for (const std::string& label : labelList)
        {
            longestLabel = std::max(longestLabel, label.size());
            randomLabelFound = randomLabelFound || looksRandomDnsLabel(label);
        }

        if (hasSuffix(domainLower, ".onion"))
        {
            if (!shouldEmitFlowAlert(flowKey + "|onion", packetRecord.captureTimestampMs, kFlowAlertCooldownMs))
            {
                return;
            }
            alertList.push_back(makeBaseAlert(
                packetRecord,
                NidsAlertSeverity::High,
                "DNS",
                "NIDS-DNS-ONION-QUERY",
                "DNS 查询 .onion 域名",
                "查询域名: " + domainText));
            return;
        }

        if (domainText.size() >= 90 || longestLabel >= 48 || randomLabelFound)
        {
            if (!shouldEmitFlowAlert(flowKey + "|anomaly", packetRecord.captureTimestampMs, kFlowAlertCooldownMs))
            {
                return;
            }

            std::ostringstream detailStream;
            detailStream << "查询域名: " << domainText
                << ", 长度=" << domainText.size()
                << ", 最长标签=" << longestLabel
                << ", qtype=" << qtype;

            alertList.push_back(makeBaseAlert(
                packetRecord,
                NidsAlertSeverity::Medium,
                "DNS",
                "NIDS-DNS-LONG-RANDOM-DOMAIN",
                "异常 DNS 域名形态",
                detailStream.str()));
            return;
        }

        static const std::array<const char*, 5> kDynamicDnsSuffixes = {
            ".duckdns.org", ".no-ip.org", ".ddns.net", ".hopto.org", ".dynu.net"
        };
        for (const char* suffixText : kDynamicDnsSuffixes)
        {
            if (!hasSuffix(domainLower, suffixText))
            {
                continue;
            }
            if (!shouldEmitFlowAlert(flowKey + "|ddns", packetRecord.captureTimestampMs, kFlowAlertCooldownMs))
            {
                return;
            }
            alertList.push_back(makeBaseAlert(
                packetRecord,
                NidsAlertSeverity::Low,
                "DNS",
                "NIDS-DNS-DYNAMIC-DNS",
                "动态 DNS 域名查询",
                "查询域名: " + domainText));
            return;
        }
    }

    void NidsEngine::analyzeHttpPayload(const PacketRecord& packetRecord, std::vector<NidsAlert>& alertList)
    {
        if (packetRecord.protocol != PacketTransportProtocol::Tcp ||
            packetRecord.payloadSize == 0)
        {
            return;
        }

        const std::string payloadText = readPayloadAscii(packetRecord, 1024);
        if (!looksLikeHttpRequest(payloadText))
        {
            return;
        }

        const std::string payloadLower = toLowerAscii(payloadText);
        NidsAlertSeverity severity = NidsAlertSeverity::Medium;
        std::string matchedReason;

        static const std::array<const char*, 8> kHighRiskNeedles = {
            "powershell", "cmd.exe", "/shell", "webshell", " nc ", "bash -c", "wget ", "curl "
        };
        for (const char* needleText : kHighRiskNeedles)
        {
            if (payloadLower.find(needleText) != std::string::npos)
            {
                severity = NidsAlertSeverity::High;
                matchedReason = needleText;
                break;
            }
        }

        if (matchedReason.empty())
        {
            static const std::array<const char*, 8> kMediumRiskNeedles = {
                "../", "%2e%2e", "/etc/passwd", "union select", "<script", "/cgi-bin/", "/wp-admin", "base64,"
            };
            for (const char* needleText : kMediumRiskNeedles)
            {
                if (payloadLower.find(needleText) != std::string::npos)
                {
                    matchedReason = needleText;
                    break;
                }
            }
        }

        if (matchedReason.empty())
        {
            return;
        }

        const std::string flowKey = buildEndpointKey(packetRecord) + "|http|" + matchedReason;
        if (!shouldEmitFlowAlert(flowKey, packetRecord.captureTimestampMs, kFlowAlertCooldownMs))
        {
            return;
        }

        const std::string firstLine = buildHttpFirstLine(payloadText);
        alertList.push_back(makeBaseAlert(
            packetRecord,
            severity,
            "HTTP",
            "NIDS-HTTP-SUSPICIOUS-PAYLOAD",
            "明文 HTTP 可疑载荷",
            "命中特征: " + matchedReason + ", 首行: " + firstLine));
    }

    void NidsEngine::analyzeSuspiciousPort(const PacketRecord& packetRecord, std::vector<NidsAlert>& alertList)
    {
        if (packetRecord.direction == PacketDirection::Unknown)
        {
            return;
        }

        const std::uint16_t servicePort = packetRecord.direction == PacketDirection::Inbound
            ? packetRecord.localPort
            : packetRecord.remotePort;
        std::string titleText;
        if (!buildRiskyPortTitle(servicePort, titleText))
        {
            return;
        }

        if (packetRecord.protocol == PacketTransportProtocol::Tcp &&
            packetRecord.payloadSize == 0 &&
            !looksLikeTcpProbe(packetRecord))
        {
            return;
        }

        const std::string flowKey = buildEndpointKey(packetRecord) + "|risky-port|" + std::to_string(servicePort);
        if (!shouldEmitFlowAlert(flowKey, packetRecord.captureTimestampMs, kFlowAlertCooldownMs))
        {
            return;
        }

        std::ostringstream detailStream;
        detailStream << buildPacketSummary(packetRecord)
            << ", 端口="
            << servicePort
            << ", payload="
            << packetRecord.payloadSize
            << " bytes";

        alertList.push_back(makeBaseAlert(
            packetRecord,
            servicePort == 4444 || servicePort == 1337 || servicePort == 31337
                ? NidsAlertSeverity::High
                : NidsAlertSeverity::Medium,
            "Port",
            "NIDS-PORT-RISKY-SERVICE",
            titleText,
            detailStream.str()));
    }

    void NidsEngine::analyzeOutboundByteBurst(const PacketRecord& packetRecord, std::vector<NidsAlert>& alertList)
    {
        if (packetRecord.direction != PacketDirection::Outbound ||
            packetRecord.processId == 0 ||
            packetRecord.payloadSize == 0 ||
            packetRecord.captureTimestampMs == 0)
        {
            return;
        }

        const std::string flowKey = buildEndpointKey(packetRecord) + "|out-bytes";
        ByteWindow& window = m_outboundByteWindowByKey[flowKey];
        appendByteWindowEvent(window, packetRecord.captureTimestampMs, packetRecord.payloadSize);
        if (window.totalBytes < kOutboundByteThreshold)
        {
            return;
        }

        if (window.lastAlertTimestampMs != 0 &&
            packetRecord.captureTimestampMs > window.lastAlertTimestampMs &&
            packetRecord.captureTimestampMs - window.lastAlertTimestampMs < kFlowAlertCooldownMs)
        {
            return;
        }
        window.lastAlertTimestampMs = packetRecord.captureTimestampMs;

        std::ostringstream detailStream;
        detailStream << buildPacketSummary(packetRecord)
            << ", "
            << kOutboundByteWindowMs / 1000
            << "s 出站累计 "
            << FormatByteCount(window.totalBytes);

        alertList.push_back(makeBaseAlert(
            packetRecord,
            NidsAlertSeverity::Medium,
            "Flow",
            "NIDS-FLOW-OUTBOUND-BYTE-BURST",
            "短时大流量出站",
            detailStream.str()));
    }

    bool NidsEngine::shouldEmitFlowAlert(
        const std::string& dedupeKey,
        const std::uint64_t nowMs,
        const std::uint64_t cooldownMs)
    {
        if (nowMs == 0)
        {
            return true;
        }

        const auto iterator = m_flowAlertLastTimestampByKey.find(dedupeKey);
        if (iterator != m_flowAlertLastTimestampByKey.end() &&
            nowMs > iterator->second &&
            nowMs - iterator->second < cooldownMs)
        {
            return false;
        }
        m_flowAlertLastTimestampByKey[dedupeKey] = nowMs;
        return true;
    }
} // namespace ks::network
