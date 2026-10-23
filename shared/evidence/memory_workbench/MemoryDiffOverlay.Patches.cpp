// MemoryDiffOverlay.Patches.cpp
// 作用：暂存补丁的改写 —— Stage / Discard / DiscardAll / AcceptWrite，
// 以及它们共用的"改写引擎"RewriteRange。
// 基线载入与只读查询在 MemoryDiffOverlay.cpp。

#include "MemoryDiffOverlay.h"

#include <cstddef>
#include <iterator>
#include <utility>

namespace ksword::memwb
{
    namespace
    {
        // 补丁块的终点（不含）。块一旦入库，终点必然不超过 uint64 上限。
        std::uint64_t BlockEnd(const DiffBlock& block)
        {
            return block.address + static_cast<std::uint64_t>(block.after.size());
        }

        // 把 address 开始的一个字节追加到 runs 末尾。
        // 如果它紧接在最后一个块之后就并进去（相邻合并），否则另起一个新块。
        // 入参：runs 正在构造的块序列（按地址升序）；address 字节地址；before/after 该字节的补丁值。
        void AppendByte(std::vector<DiffBlock>& runs, std::uint64_t address, std::uint8_t before, std::uint8_t after)
        {
            // 末块不存在或没有紧贴 address 时，另起新块。
            if (runs.empty() || BlockEnd(runs.back()) != address)
            {
                runs.emplace_back();
                runs.back().address = address;
            }

            runs.back().before.push_back(before);
            runs.back().after.push_back(after);
        }

        // 把 source 的 [offset, offset + count) 一段追加到 runs 末尾，合并规则同 AppendByte。
        // 入参：runs 正在构造的块序列；source 来源块；offset 块内起始偏移；count 字节数。
        void AppendSlice(std::vector<DiffBlock>& runs, const DiffBlock& source, std::uint64_t offset, std::uint64_t count)
        {
            if (count == 0)
            {
                return;
            }

            // 切片对应的绝对地址；紧贴末块就并入，否则另起新块。
            const std::uint64_t address = source.address + offset;
            if (runs.empty() || BlockEnd(runs.back()) != address)
            {
                runs.emplace_back();
                runs.back().address = address;
            }

            // 用迭代器区间整段拷贝，比逐字节追加快得多。
            DiffBlock& target = runs.back();
            const auto from = static_cast<std::ptrdiff_t>(offset);
            const auto to = static_cast<std::ptrdiff_t>(offset + count);
            target.before.insert(target.before.end(), source.before.begin() + from, source.before.begin() + to);
            target.after.insert(target.after.end(), source.after.begin() + from, source.after.begin() + to);
        }

        // 取 source 的 [offset, offset + count) 切成一个独立块。
        DiffBlock SliceBlock(const DiffBlock& source, std::uint64_t offset, std::uint64_t count)
        {
            std::vector<DiffBlock> single;
            AppendSlice(single, source, offset, count);
            return std::move(single.front());
        }
    }

    // 改写 [start, endExclusive) 的补丁。
    // 做法：找出与该范围重叠或相邻的旧块 -> 旧块伸出范围之外的左右两段原样保留 ->
    // 范围内逐字节套规则 -> 全部拼成新块（相邻自动合并，被规则抹掉的字节自然拆开）->
    // 校验总量 -> 换进补丁表。校验失败时补丁表一个字节都没动过。
    // 入参：start/endExclusive 范围（调用方保证终点不溢出且长度有界）；
    //       rule 逐字节改写规则；maxTotal 改写后允许的补丁总量。
    // 返回：成功 true；总量超限 false，状态不变。
    bool MemoryDiffOverlay::RewriteRange(
        std::uint64_t start,
        std::uint64_t endExclusive,
        const ByteRule& rule,
        std::uint64_t maxTotal)
    {
        using PatchMap = std::map<std::uint64_t, DiffBlock>;

        // 第一步：收集受影响的旧块。起点 <= start 的块里只有紧邻前一个可能碰到范围；
        // 起点在 (start, endExclusive] 内的块都算（起点恰为终点即相邻）。
        std::vector<PatchMap::const_iterator> affected;
        PatchMap::const_iterator scan = state_->patches_.upper_bound(start);
        if (scan != state_->patches_.begin())
        {
            PatchMap::const_iterator previous = std::prev(scan);
            if (BlockEnd(previous->second) >= start)
            {
                affected.push_back(previous);
            }
        }

        while (scan != state_->patches_.end() && scan->first <= endExclusive)
        {
            affected.push_back(scan);
            ++scan;
        }

        // 旧块总字节数，换表时要从总量里扣掉。
        std::uint64_t oldBytes = 0;
        for (const PatchMap::const_iterator& entry : affected)
        {
            oldBytes += static_cast<std::uint64_t>(entry->second.after.size());
        }

        // 第二步：拼新块。先放第一个旧块伸到范围左侧的那一段。
        std::vector<DiffBlock> runs;
        if (!affected.empty())
        {
            const DiffBlock& first = affected.front()->second;
            if (first.address < start)
            {
                AppendSlice(runs, first, 0, start - first.address);
            }
        }

        // 第三步：范围内逐字节取旧状态、套规则、把仍有补丁的字节接上去。
        // cursor 指向第一个"终点还在当前地址之后"的旧块，随地址单调推进。
        std::size_t cursor = 0;
        for (std::uint64_t address = start; address < endExclusive; ++address)
        {
            while (cursor < affected.size() && BlockEnd(affected[cursor]->second) <= address)
            {
                ++cursor;
            }

            // 当前地址落在某个旧块内就取出旧的 before/after，否则旧状态为"无补丁"。
            ByteState old;
            if (cursor < affected.size() && affected[cursor]->second.address <= address)
            {
                const DiffBlock& block = affected[cursor]->second;
                const std::size_t offset = static_cast<std::size_t>(address - block.address);
                old.patched = true;
                old.before = block.before[offset];
                old.after = block.after[offset];
            }

            const ByteState next = rule(address, old);
            if (next.patched)
            {
                AppendByte(runs, address, next.before, next.after);
            }
        }

        // 第四步：最后一个旧块伸到范围右侧的那一段原样接上。
        // 终点恰与范围终点相接的旧块（相邻块）整块落在这里。
        if (!affected.empty())
        {
            const DiffBlock& last = affected.back()->second;
            const std::uint64_t lastEnd = BlockEnd(last);
            if (lastEnd > endExclusive)
            {
                // 旧块起点必不超过范围终点（收集时的条件），所以右段恰从 endExclusive 开始。
                AppendSlice(runs, last, endExclusive - last.address, lastEnd - endExclusive);
            }
        }

        // 第五步：算新总量并校验。超限就直接返回，旧块一个都没动。
        std::uint64_t newBytes = 0;
        for (const DiffBlock& run : runs)
        {
            newBytes += static_cast<std::uint64_t>(run.after.size());
        }

        const std::uint64_t newTotal = state_->pendingBytes_ - oldBytes + newBytes;
        if (newTotal > maxTotal)
        {
            return false;
        }

        Detach();
        ++mutationSerial_;

        // 第六步：换表。先删旧块，再放新块，最后更新总量。
        for (const PatchMap::const_iterator& entry : affected)
        {
            state_->patches_.erase(entry->first);
        }

        for (DiffBlock& run : runs)
        {
            const std::uint64_t key = run.address;
            state_->patches_.emplace(key, std::move(run));
        }

        state_->pendingBytes_ = newTotal;
        return true;
    }

    // 暂存一次编辑，检查顺序见头文件。
    StageStatus MemoryDiffOverlay::Stage(std::uint64_t address, const std::vector<std::uint8_t>& bytes)
    {
        // 1. 没有字节：不可能是一次编辑。
        if (bytes.empty())
        {
            return StageStatus::Empty;
        }

        // 2. 终点溢出。放在窗口检查之前，这样溢出请求永远得到"溢出"而不是"窗口外"。
        const std::uint64_t length = static_cast<std::uint64_t>(bytes.size());
        if (!RangeRepresentable(address, length))
        {
            return StageStatus::AddressOverflow;
        }

        // 3. 必须完整落在窗口内。先比较偏移再减长度，全程不会回绕。
        const std::uint64_t windowSize = static_cast<std::uint64_t>(state_->baseline_.size());
        const bool inWindow = state_->hasBaseline_
            && address >= state_->baseAddress_
            && address - state_->baseAddress_ <= windowSize
            && length <= windowSize - (address - state_->baseAddress_);
        if (!inWindow)
        {
            return StageStatus::OutOfWindow;
        }

        // 4. 范围内每个字节都必须真实读到过：不能编辑从没读到的字节。
        const std::size_t startIndex = static_cast<std::size_t>(address - state_->baseAddress_);
        for (std::size_t offset = 0; offset < bytes.size(); ++offset)
        {
            if (state_->validMask_[startIndex + offset] == 0)
            {
                return StageStatus::UnreadBytes;
            }
        }

        // 5. 单次就比上限大，没必要往下拼块（也避免临时分配一大块内存）。
        if (length > state_->maxPendingBytes_)
        {
            return StageStatus::TooLarge;
        }

        // 规则：新字节等于当前基线 -> 该处补丁消失；否则写入，before 保留最早一次。
        const std::uint64_t start = address;
        const ByteRule rule = [this, &bytes, start](std::uint64_t at, const ByteState& old) -> ByteState
        {
            const std::uint8_t baselineByte = state_->baseline_[static_cast<std::size_t>(at - state_->baseAddress_)];
            const std::uint8_t incoming = bytes[static_cast<std::size_t>(at - start)];

            // 空操作编辑不留痕。
            ByteState next;
            if (incoming == baselineByte)
            {
                return next;
            }

            // 已有补丁时 before 继续沿用最早那次，不被中间值覆盖。
            next.patched = true;
            next.after = incoming;
            next.before = old.patched ? old.before : baselineByte;
            return next;
        };

        // 6. 套引擎；合并后总量超限则整次拒绝。
        if (!RewriteRange(address, address + length, rule, state_->maxPendingBytes_))
        {
            return StageStatus::TooLarge;
        }

        return StageStatus::Ok;
    }

    // 丢弃 [address, address + length) 内的补丁。
    // 不走 RewriteRange：丢弃的范围不受数据长度约束（可能是整个地址空间），
    // 逐字节扫描不可行，所以只处理与范围真正重叠的那几个块。
    std::uint64_t MemoryDiffOverlay::Discard(std::uint64_t address, std::uint64_t length)
    {
        if (length == 0 || !RangeRepresentable(address, length))
        {
            return 0;
        }

        Detach();
        ++mutationSerial_;
        using PatchMap = std::map<std::uint64_t, DiffBlock>;
        const std::uint64_t endExclusive = address + length;

        // 收集与范围真正重叠的块：起点 <= address 的前一个块要看它的终点是否越过 address，
        // 起点在 (address, endExclusive) 内的块必然重叠。
        std::vector<PatchMap::iterator> hit;
        PatchMap::iterator scan = state_->patches_.upper_bound(address);
        if (scan != state_->patches_.begin())
        {
            PatchMap::iterator previous = std::prev(scan);
            if (BlockEnd(previous->second) > address)
            {
                hit.push_back(previous);
            }
        }

        while (scan != state_->patches_.end() && scan->first < endExclusive)
        {
            hit.push_back(scan);
            ++scan;
        }

        // 逐块处理：整块取出后删除，把伸出范围左右两侧的部分切成新块放回去。
        std::uint64_t removed = 0;
        for (const PatchMap::iterator& entry : hit)
        {
            DiffBlock block = std::move(entry->second);
            state_->patches_.erase(entry);

            // 被丢弃的交集 = [max(块起点, address), min(块终点, endExclusive))。
            const std::uint64_t blockEnd = BlockEnd(block);
            const std::uint64_t cutStart = block.address > address ? block.address : address;
            const std::uint64_t cutEnd = blockEnd < endExclusive ? blockEnd : endExclusive;
            removed += cutEnd - cutStart;

            // 左侧残留：块起点在 address 之前。
            if (block.address < address)
            {
                DiffBlock head = SliceBlock(block, 0, address - block.address);
                const std::uint64_t key = head.address;
                state_->patches_.emplace(key, std::move(head));
            }

            // 右侧残留：块终点在 endExclusive 之后。
            if (blockEnd > endExclusive)
            {
                DiffBlock tail = SliceBlock(block, endExclusive - block.address, blockEnd - endExclusive);
                const std::uint64_t key = tail.address;
                state_->patches_.emplace(key, std::move(tail));
            }
        }

        state_->pendingBytes_ -= removed;
        return removed;
    }

    // 丢弃全部补丁。
    void MemoryDiffOverlay::DiscardAll()
    {
        Detach();
        ++mutationSerial_;
        state_->patches_.clear();
        state_->pendingBytes_ = 0;
    }

    // 写事务确认写入并回读后调用，语义见头文件。
    AcceptWriteStatus MemoryDiffOverlay::AcceptWrite(const DiffBlock& block, const std::vector<std::uint8_t>& readBackBytes)
    {
        // 先做全部校验，通过之前不改任何状态。长度不符优先报告：这是最常见的调用错误。
        if (readBackBytes.size() != block.after.size())
        {
            return AcceptWriteStatus::ReadBackLengthMismatch;
        }

        if (block.after.empty())
        {
            return AcceptWriteStatus::Empty;
        }

        const std::uint64_t length = static_cast<std::uint64_t>(block.after.size());
        if (!RangeRepresentable(block.address, length))
        {
            return AcceptWriteStatus::AddressOverflow;
        }

        const std::uint64_t start = block.address;
        const std::uint64_t endExclusive = start + length;

        // 补丁侧：被这次写入覆盖的暂存字节移除；若用户在写入期间把同一处改成了别的值，
        // 新暂存保留，但目标上现在就是回读值，所以 before 改成回读值，
        // 并且新暂存恰好等于回读值时同样视为空操作而消失。
        const ByteRule rule = [&block, &readBackBytes, start](std::uint64_t at, const ByteState& old) -> ByteState
        {
            ByteState next = old;
            if (!old.patched)
            {
                return next;
            }

            const std::size_t index = static_cast<std::size_t>(at - start);
            if (old.after == block.after[index])
            {
                next.patched = false;
                return next;
            }

            next.before = readBackBytes[index];
            if (next.after == readBackBytes[index])
            {
                next.patched = false;
            }

            return next;
        };

        // 范围长度等于调用方手里已有的字节数，有界；总量只减不增，上限传最大值。
        RewriteRange(start, endExclusive, rule, kMemoryDiffOverlayAddressMax);

        // 基线侧：窗口内的部分用回读字节顶替，标记有效与"自己写入"；窗口外的忽略。
        for (std::uint64_t at = start; at < endExclusive; ++at)
        {
            if (!InWindow(at))
            {
                continue;
            }

            const std::size_t index = static_cast<std::size_t>(at - state_->baseAddress_);
            state_->baseline_[index] = readBackBytes[static_cast<std::size_t>(at - start)];
            state_->validMask_[index] = 1;
            state_->selfWritten_[index] = 1;
        }

        return AcceptWriteStatus::Ok;
    }
}
