#pragma once

// ============================================================
// FilePropertyPeAnalyzer.Internal.h
// 作用：
// 1) 声明 PE 解析器多实现文件共享的扩展函数；
// 2) 让主解析器与扩展表项解析实现解耦；
// 3) 避免继续把所有 PE 解析逻辑堆到单一 .cpp。
// ============================================================

#include <QByteArray>
#include <QTextStream>

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>

#include <array>
#include <cstdint>
#include <vector>

namespace file_dock_detail::pe_tables_detail
{
    // calculateSectionEntropy：
    // - 计算区段原始数据熵值。
    double calculateSectionEntropy(
        const QByteArray& fileBytes,
        std::uint32_t rawOffsetValue,
        std::uint32_t rawSizeValue);

    // dumpDataDirectories：
    // - 输出全部数据目录总览。
    void dumpDataDirectories(
        QTextStream& outputStream,
        const std::array<IMAGE_DATA_DIRECTORY, IMAGE_NUMBEROF_DIRECTORY_ENTRIES>& directoryList);

    // dumpDelayImportTable：
    // - 输出延迟导入表。
    void dumpDelayImportTable(
        QTextStream& outputStream,
        const QByteArray& fileBytes,
        bool isPe64,
        std::uint32_t sizeOfHeadersValue,
        const std::vector<IMAGE_SECTION_HEADER>& sectionList,
        const IMAGE_DATA_DIRECTORY& delayImportDirectory);

    // dumpTlsDirectory：
    // - 输出 TLS 表与回调摘要。
    void dumpTlsDirectory(
        QTextStream& outputStream,
        const QByteArray& fileBytes,
        bool isPe64,
        std::uint64_t imageBaseValue,
        std::uint32_t sizeOfHeadersValue,
        const std::vector<IMAGE_SECTION_HEADER>& sectionList,
        const IMAGE_DATA_DIRECTORY& tlsDirectory);

    // dumpResourceDirectory：
    // - 输出资源目录一级概览。
    void dumpResourceDirectory(
        QTextStream& outputStream,
        const QByteArray& fileBytes,
        std::uint32_t sizeOfHeadersValue,
        const std::vector<IMAGE_SECTION_HEADER>& sectionList,
        const IMAGE_DATA_DIRECTORY& resourceDirectory);

    // dumpBaseRelocDirectory：
    // - 输出重定位表块摘要。
    void dumpBaseRelocDirectory(
        QTextStream& outputStream,
        const QByteArray& fileBytes,
        std::uint32_t sizeOfHeadersValue,
        const std::vector<IMAGE_SECTION_HEADER>& sectionList,
        const IMAGE_DATA_DIRECTORY& relocDirectory);

    // dumpDebugDirectory：
    // - 输出调试目录摘要。
    void dumpDebugDirectory(
        QTextStream& outputStream,
        const QByteArray& fileBytes,
        std::uint32_t sizeOfHeadersValue,
        const std::vector<IMAGE_SECTION_HEADER>& sectionList,
        const IMAGE_DATA_DIRECTORY& debugDirectory);

    // dumpBoundImportDirectory：
    // - 输出绑定导入表摘要。
    void dumpBoundImportDirectory(
        QTextStream& outputStream,
        const QByteArray& fileBytes,
        std::uint32_t sizeOfHeadersValue,
        const std::vector<IMAGE_SECTION_HEADER>& sectionList,
        const IMAGE_DATA_DIRECTORY& boundImportDirectory);

    // dumpLoadConfigDirectory：
    // - 输出 Load Config 摘要。
    void dumpLoadConfigDirectory(
        QTextStream& outputStream,
        const QByteArray& fileBytes,
        bool isPe64,
        std::uint32_t sizeOfHeadersValue,
        const std::vector<IMAGE_SECTION_HEADER>& sectionList,
        const IMAGE_DATA_DIRECTORY& loadConfigDirectory);

    // dumpClrDirectory：
    // - 输出 CLR/.NET 头摘要。
    void dumpClrDirectory(
        QTextStream& outputStream,
        const QByteArray& fileBytes,
        std::uint32_t sizeOfHeadersValue,
        const std::vector<IMAGE_SECTION_HEADER>& sectionList,
        const IMAGE_DATA_DIRECTORY& clrDirectory);

    // dumpSecurityDirectory：
    // - 输出安全目录/证书摘要。
    void dumpSecurityDirectory(
        QTextStream& outputStream,
        const IMAGE_DATA_DIRECTORY& securityDirectory);
}
