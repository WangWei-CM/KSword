// ============================================================
// DumpMemoryReader.cpp
// 作用：
// - 实现 DumpMemoryReader.h 声明的虚拟地址内存索引；
// - finalize() 排序后用 upper_bound 定位覆盖块，所有读取都要求
//   整段落在同一块内（转储里相邻虚拟地址未必在文件里相邻，
//   跨块拼接会读到无关字节）；
// - 段本身的文件范围在 finalize 阶段就用 DumpFileView 校验，
//   畸形转储的越界段直接丢弃，不会留到查询期。
// ============================================================

#include "DumpMemoryReader.h"

#include "DumpPageTable.h"

#include <algorithm>
#include <cstring>

namespace ks::minidump
{
    namespace
    {
        // kMaxRanges：内存块索引上限，防止畸形转储声明天量段拖垮解析。
        constexpr std::size_t kMaxRanges = 200000;
    }

    void DumpMemoryReader::addRange(
        const std::uint64_t virtualAddress,
        const std::uint64_t fileOffset,
        const std::uint64_t bytes,
        const QString& source)
    {
        if (bytes == 0 || virtualAddress == 0 || m_ranges.size() >= kMaxRanges)
        {
            return;
        }
        // 虚拟地址加法溢出的段一律丢弃：这类段无法参与区间比较。
        if (virtualAddress + bytes < virtualAddress)
        {
            return;
        }
        Range range{};
        range.virtualAddress = virtualAddress;
        range.fileOffset = fileOffset;
        range.bytes = bytes;
        range.source = source;
        m_ranges.push_back(range);
    }

    void DumpMemoryReader::finalize(const DumpFileView& view)
    {
        // 先剔除文件内越界的段，避免查询期还要重复校验。
        m_ranges.erase(
            std::remove_if(
                m_ranges.begin(),
                m_ranges.end(),
                [&view](const Range& range)
                {
                    return !view.contains(range.fileOffset, range.bytes);
                }),
            m_ranges.end());
        std::sort(
            m_ranges.begin(),
            m_ranges.end(),
            [](const Range& left, const Range& right)
            {
                if (left.virtualAddress != right.virtualAddress)
                {
                    return left.virtualAddress < right.virtualAddress;
                }
                // 同起点时长块排前面，find 命中后能覆盖更长的请求。
                return left.bytes > right.bytes;
            });
        m_finalized = true;
    }

    const DumpMemoryReader::Range* DumpMemoryReader::find(
        const std::uint64_t virtualAddress,
        const std::uint64_t bytes) const
    {
        if (!m_finalized || m_ranges.empty() || bytes == 0)
        {
            return nullptr;
        }
        if (virtualAddress + bytes < virtualAddress)
        {
            return nullptr;
        }
        // position：第一个起点大于目标地址的块；候选只可能是它前面那个。
        const auto position = std::upper_bound(
            m_ranges.begin(),
            m_ranges.end(),
            virtualAddress,
            [](const std::uint64_t value, const Range& range)
            {
                return value < range.virtualAddress;
            });
        // position 之前的块起点都 <= 目标地址；从最近的一项向前回溯，
        // 排序已让同起点的长块靠前，真实转储里一两步就能命中。
        // kProbeDepth 是对畸形转储中大量重叠块的兜底，避免退化成线性扫描。
        constexpr int kProbeDepth = 8;
        auto candidate = position;
        for (int step = 0; step < kProbeDepth && candidate != m_ranges.begin(); ++step)
        {
            --candidate;
            const std::uint64_t offsetInRange = virtualAddress - candidate->virtualAddress;
            if (offsetInRange < candidate->bytes &&
                candidate->bytes - offsetInRange >= bytes)
            {
                return &(*candidate);
            }
        }
        return nullptr;
    }

    bool DumpMemoryReader::contains(
        const std::uint64_t virtualAddress,
        const std::uint64_t bytes) const
    {
        if (find(virtualAddress, bytes) != nullptr)
        {
            return true;
        }
        // 块索引未命中时问页表：完整/内核转储的内存全靠这条路径。
        if (m_pageTable != nullptr && bytes != 0 && bytes - 1 <= (~0ull) - virtualAddress)
        {
            std::uint64_t physicalAddress = 0;
            // 只验证首尾两页可达即可判断"这段内存在不在转储里"；
            // 逐页验证会让栈扫描的每次候选校验都退化成一轮完整翻译。
            if (!m_pageTable->translate(virtualAddress, &physicalAddress))
            {
                return false;
            }
            return m_pageTable->translate(virtualAddress + bytes - 1, &physicalAddress);
        }
        return false;
    }

    bool DumpMemoryReader::read(
        const DumpFileView& view,
        const std::uint64_t virtualAddress,
        const std::uint64_t bytes,
        void* const buffer) const
    {
        if (buffer == nullptr)
        {
            return false;
        }
        const Range* const range = find(virtualAddress, bytes);
        if (range == nullptr)
        {
            // 页表后端自带真实文件视图，不使用这里传入的 view——完整转储路径下
            // 调用方可能传的是栈缓冲构成的临时视图，用它去查文件偏移会读到错字节。
            if (m_pageTable != nullptr)
            {
                return m_pageTable->readVirtual(virtualAddress, bytes, buffer);
            }
            return false;
        }
        const std::uint64_t fileOffset =
            range->fileOffset + (virtualAddress - range->virtualAddress);
        const unsigned char* const source = view.at(fileOffset, bytes);
        if (source == nullptr)
        {
            return false;
        }
        std::memcpy(buffer, source, static_cast<std::size_t>(bytes));
        return true;
    }

    void DumpMemoryReader::appendCapturedRanges(
        std::vector<DumpMemoryRange>* const rangesOut) const
    {
        if (!m_finalized || rangesOut == nullptr)
        {
            return;
        }
        rangesOut->reserve(rangesOut->size() + m_ranges.size());
        for (const Range& range : m_ranges)
        {
            CheckDumpCancellation();
            DumpMemoryRange entry{};
            entry.virtualAddress = range.virtualAddress;
            entry.fileOffset = range.fileOffset;
            entry.bytes = range.bytes;
            entry.source = range.source;
            rangesOut->push_back(std::move(entry));
        }
    }

}
