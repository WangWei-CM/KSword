#pragma once

// ============================================================
// NetworkDock.InternalHelpers.h
// 作用：
// 1) 声明 NetworkDock 多个实现文件共享的辅助函数；
// 2) 避免把工具函数散落到各 .cpp，保证行为一致；
// 3) 供“普通 .cpp 拆分结构”复用，替代原先单编译单元匿名命名空间。
// ============================================================

#include "NetworkDock.h"

#include <QIcon>
#include <QString>

#include <cstddef> // std::size_t：字节长度、偏移计算。
#include <cstdint> // std::uint*_t：网络字段与序号。
#include <string>  // std::string：底层网络文本桥接。
#include <utility> // std::pair：范围返回值。
#include <vector>  // std::vector：报文字节容器。

class QTableWidget;
class QTableWidgetItem;

namespace network_dock_detail
{
    // toQString 作用：
    // - 把 UTF-8 std::string 统一转成 QString。
    QString toQString(const std::string& textValue);

    // formatEndpointText 作用：
    // - 把“地址 + 端口”格式化为可读端点字符串；
    // - IPv6 自动输出 [addr]:port 形式。
    QString formatEndpointText(const std::string& ipAddress, std::uint16_t portNumber);

    // formatBytesToHexText 作用：
    // - 把字节数组转换为单行十六进制预览；
    // - 超过上限时追加总长度提示。
    QString formatBytesToHexText(
        const std::vector<std::uint8_t>& byteArray,
        std::size_t maxBytesToRender);

    // buildPayloadByteRange 作用：
    // - 计算 payload 在 packetBytes 中的安全区间；
    // - 返回值 first=起始偏移，second=可读长度。
    std::pair<std::size_t, std::size_t> buildPayloadByteRange(
        const ks::network::PacketRecord& packetRecord);

    // buildPayloadAsciiPreviewText 作用：
    // - 生成流量列表“内容预览”字段；
    // - 优先提取可读 ASCII 片段，退化时输出简化文本。
    QString buildPayloadAsciiPreviewText(const ks::network::PacketRecord& packetRecord);

    // buildPayloadAsciiFullText 作用：
    // - 生成 payload 完整 ASCII 文本；
    // - 不可打印字节替换为 '.'。
    QString buildPayloadAsciiFullText(const ks::network::PacketRecord& packetRecord);

    // buildPayloadHexFullText 作用：
    // - 生成 payload 完整十六进制文本；
    // - 供“抓包回放到请求构造”回填 HEX 字段。
    QString buildPayloadHexFullText(const ks::network::PacketRecord& packetRecord);

    // buildPacketHexAsciiDumpText 作用：
    // - 生成“偏移 + HEX + ASCII”转储文本；
    // - 供复制和详情查看。
    QString buildPacketHexAsciiDumpText(const ks::network::PacketRecord& packetRecord);

    // buildPacketCopyHeaderLine 作用：
    // - 生成复制时每条报文块的元信息头。
    QString buildPacketCopyHeaderLine(const ks::network::PacketRecord& packetRecord);

    // formatIpv4HostOrder 作用：
    // - 主机序 IPv4 转点分十进制文本。
    QString formatIpv4HostOrder(std::uint32_t ipv4HostOrder);

    // tryParseIpv4Text 作用：
    // - 解析 IPv4 文本并输出主机序 32 位值。
    bool tryParseIpv4Text(const QString& ipv4Text, std::uint32_t& ipv4HostOrderOut);

    // tryParseIpv4RangeText 作用：
    // - 解析 CIDR/区间/单 IP 三类表达式；
    // - 成功时输出闭区间与标准化文本。
    bool tryParseIpv4RangeText(
        const QString& rangeText,
        std::pair<std::uint32_t, std::uint32_t>& rangeOut,
        QString& normalizeTextOut);

    // tryParsePortRangeText 作用：
    // - 解析单端口或端口区间表达式；
    // - 成功时输出闭区间与标准化文本。
    bool tryParsePortRangeText(
        const QString& rangeText,
        std::pair<std::uint16_t, std::uint16_t>& rangeOut,
        QString& normalizeTextOut);

    // createPacketCell 作用：
    // - 创建统一的只读表格单元格。
    QTableWidgetItem* createPacketCell(const QString& cellText);

    // populatePacketRow 作用：
    // - 把报文实体按统一列定义写入目标表格行。
    void populatePacketRow(
        QTableWidget* tableWidget,
        int rowIndex,
        const ks::network::PacketRecord& packetRecord,
        std::uint64_t sequenceId,
        const QIcon& processIcon);

    // showPacketDetailWindow 作用：
    // - 以非模态独立窗口展示报文详情并激活窗口。
    void showPacketDetailWindow(const ks::network::PacketRecord& packetRecord);
}

