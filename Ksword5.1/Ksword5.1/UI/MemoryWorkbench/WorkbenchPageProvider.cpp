// ============================================================
// WorkbenchPageProvider.cpp
// 作用：
// - 实现 WorkbenchPageProvider.h 声明的全部公开接口：R1 的 Gate 判定与会话
//   快照、R2 的 DDMA 脏扇区闩锁前置判定（真正的读线程调度在
//   WorkbenchPageProvider.Pool.cpp）、R3 的陈旧性核对与回填，以及 Wave 3
//   审核落实的 D2-D11（见 WorkbenchPageProvider.h 文件头"两条数轴"与
//   "D4/D5/D6"两节）。文件头的算法说明见 WorkbenchPageProvider.h，本文件
//   按 R1→R2→R3→D4/D5/D6 的顺序组织代码，不重复展开背景，只在每个判断点
//   标注对应哪一条。
// - 本文件只在 UI 线程被调用（头文件已经逐条声明），因此可以直接触碰
//   canvas_/target_/scratchLatched_ 等成员，不需要加锁；唯一跨线程的部分
//   全部封在 WorkbenchPageProvider.Pool.cpp 里的 ReadPool/InFlightJob。
// - D9（同步重入）纪律：target_->capture() 可能同步触发
//   WorkbenchTarget::sessionChanged，订阅者的槛理论上可以同步改变 canvas_
//   （换地址空间、甚至 setCanvas(nullptr)）。本文件任何用到 canvas_ 的地方都
//   直接读这个 QPointer 成员本身，绝不在 capture()/isSourceFresh() 这类可能
//   重入的调用**之前**把 canvas_ 缓存进局部变量再在调用之后继续用那个局部
//   变量——每次都是"现在、此刻"的 canvas_。
// ============================================================

#include "WorkbenchPageProvider.Pool.h"

#include <limits>
#include <utility>

namespace
{
    // NullMemoryIoPort：构造函数收到的 portFactory 违反"必须非空可调用"这条
    // 契约（为空，或调用后返回空指针）时的占位端口。纯防御——头文件注释把
    // "portFactory 必须非空可调用"写成调用方的义务，不是本类必须自己兜底的
    // 正常路径；有这个占位端口只是为了让 port_ 永远非空，后续代码不用在每个
    // 调用点都判空。Limits 报"无限制"（反正不会真的被用来读任何东西），
    // Read/Write 恒报失败，绝不假装读到或写入任何字节——与 IMemoryIoPort.h
    // 对 Failed 状态的定义一致："通道自身出了问题，没有得到关于目标任何字节
    // 是否可读的结论"。
    class NullMemoryIoPort final : public ksword::memwb::IMemoryIoPort
    {
    public:
        ksword::memwb::IoLimits Limits(const ksword::memwb::MemoryTargetSession& /*session*/) const override
        {
            return ksword::memwb::IoLimits{};
        }

        ksword::memwb::IoReadResult Read(
            const ksword::memwb::MemoryTargetSession& /*session*/,
            std::uint64_t /*address*/,
            std::uint64_t /*length*/) override
        {
            ksword::memwb::IoReadResult result;
            result.status = ksword::memwb::IoReadStatus::Failed;
            result.failure = "WorkbenchPageProvider: portFactory did not produce a port (contract violated)";
            return result;
        }

        ksword::memwb::IoWriteResult Write(
            const ksword::memwb::MemoryTargetSession& /*session*/,
            std::uint64_t /*address*/,
            const std::vector<std::uint8_t>& /*bytes*/,
            bool /*approved*/) override
        {
            ksword::memwb::IoWriteResult result;
            result.failure = "WorkbenchPageProvider: portFactory did not produce a port (contract violated)";
            return result;
        }
    };
}

namespace ks::ui
{
    WorkbenchPageProvider::WorkbenchPageProvider(
        IoPortFactory portFactory, WorkbenchTarget* target, QObject* parent)
        : QObject(parent)
        , target_(target)
        , readPool_(std::make_unique<ReadPool>())
    {
        // 契约要求 portFactory 非空可调用，且构造时恰好调用一次；这里按契约
        // 调用，但仍然对"调用后返回空指针"这种违约做兜底（见 NullMemoryIoPort
        // 的注释），不让 port_ 本身保持空指针状态。
        if (portFactory)
        {
            port_ = portFactory();
        }
        if (!port_)
        {
            port_ = std::make_unique<NullMemoryIoPort>();
        }
    }

    WorkbenchPageProvider::~WorkbenchPageProvider()
    {
        // 尽早让仍在跑的任务（至多一个，maxThreadCount=1）知道要停下来：这是
        // 析构函数体里第一件事，越早置位取消标志，读线程在下一次 Read 之前
        // 检查到的时间点就越早，readPool_ 析构时需要等待的时间就越短——不是
        // "消除等待"，是"尽量缩短"，完整论证见
        // WorkbenchPageProvider.Pool.h 文件头第 4/5 点。析构期间不需要再碰
        // pendingJobs_/canvas_——没有人会再看这些挂起的记账了，canvas_ 本身
        // 也即将随本对象一起失去意义。
        if (readPool_)
        {
            readPool_->CancelAll();
        }
        // 不显式 reset 任何成员：让 canvas_/target_/port_/readPool_ 按
        // WorkbenchPageProvider.h 里的声明顺序、以 C++ 的默认规则（声明的
        // 逆序）析构。readPool_ 声明在 port_ 之后，因此会先于 port_ 被销毁，
        // 这正是"port_ 被真正释放之前，不会再有工作线程在用它"的安全保证
        // 来源，不需要（也不应该）在这里手动改变这个顺序。
    }

    void WorkbenchPageProvider::setCanvas(HexCanvas* canvas)
    {
        // N6 修复：只有"从暂停恢复"（旧值空、新值非空）才需要补
        // cancelPages；换成另一块非空画布（目前没有这种用法）防御性地按
        // "不是恢复"处理，避免误撤销别的画布的 Pending 地址。
        const bool resuming = (canvas_ == nullptr) && (canvas != nullptr);
        // QPointer 赋值自动处理空指针与"画布随后被销毁"两种情况——后一种
        // 情况下 canvas_ 会自动变回空，不需要宿主显式再调一次 setCanvas(nullptr)。
        canvas_ = canvas;
        if (resuming && !pausedPendingRanges_.empty())
        {
            // 暂停期间落地、被丢弃的结果记在 pausedPendingRanges_ 里（见
            // onJobFinishedOnUiThread 的 "!canvas_" 分支）；恢复这一刻补一次
            // cancelPages，让地址从 Pending 变回 NotLoaded，下一次重绘的
            // PlanFetch 就能重新排进请求——这不是"自动重试已知失败的通道"
            // （那条红线只管 failedRanges_），只是让"暂停"这个动作对称。
            for (const PausedRangeRecord& record : pausedPendingRanges_)
            {
                if (canvas_ && canvas_->sourceRevision() == record.canvasRevision)
                {
                    canvas_->cancelPages(record.range);
                }
            }
            pausedPendingRanges_.clear();
        }
    }

    void WorkbenchPageProvider::setGateInputsProvider(GateInputsProvider provider)
    {
        gateInputsProvider_ = std::move(provider);
    }

    void WorkbenchPageProvider::UpdateViewportInterest(
        const std::vector<HexFetchRange>& ranges, std::uint64_t sourceRevision)
    {
        viewportInterest_ = ranges;
        viewportInterestCanvasRevision_ = sourceRevision;
    }

    void WorkbenchPageProvider::ProtectPages(
        const std::vector<HexFetchRange>& ranges, std::uint64_t sourceRevision)
    {
        for (auto& entry : pendingJobs_)
        {
            auto& record = entry.second;
            if (record.canvasRevision != sourceRevision || record.purpose != ReadPurpose::Browse) continue;
            bool overlap = false;
            for (const auto& pending : record.ranges)
                for (const auto& wanted : ranges)
                {
                    if (!pending.pageCount || !wanted.pageCount) continue;
                    const auto left = pending.firstPageStart / ksword::memwb::HexViewport::kPageBytes;
                    const auto right = wanted.firstPageStart / ksword::memwb::HexViewport::kPageBytes;
                    overlap = overlap || (left <= right ? right - left < pending.pageCount
                        : left - right < wanted.pageCount);
                }
            if (overlap) record.purpose = ReadPurpose::ExplicitRead;
        }
    }

    void WorkbenchPageProvider::RequestPages(
        const std::vector<HexFetchRange>& ranges, std::uint64_t sourceRevision)
    {
        requestPages(ranges, sourceRevision, ReadPurpose::Browse);
    }

    void WorkbenchPageProvider::RequestExplicitPages(
        const std::vector<HexFetchRange>& ranges, std::uint64_t sourceRevision)
    {
        ProtectPages(ranges, sourceRevision);
        requestPages(ranges, sourceRevision, ReadPurpose::ExplicitRead);
    }

    void WorkbenchPageProvider::requestPages(
        const std::vector<HexFetchRange>& ranges, std::uint64_t sourceRevision, ReadPurpose purpose)
    {
        // 空请求没有任何事可做：不取会话、不判 Gate、不发起任何端口调用。
        if (ranges.empty())
        {
            return;
        }
        // target_ 按构造契约永远非空（调用方负责生命周期），这里仍然防御一下，
        // 把"没有会话来源"按"不可用"处理，而不是解引用空指针。
        if (target_ == nullptr)
        {
            for (const HexFetchRange& range : ranges)
            {
                if (canvas_)
                {
                    canvas_->cancelPages(range);
                }
            }
            return;
        }

        // D7：provider 入口把每段 range 的 pageCount 夹到缓存容量——canvas 自己
        // 发起的请求本来就已经遵守这个上限（HexViewport::ValidateRange 的既有
        // 校验），这里只是给 retryFailedRanges/rereadByteRange 之外、万一直接
        // 调用 RequestPages 的路径再加一道防线，不依赖调用方自律。
        std::vector<HexFetchRange> clampedRanges = ranges;
        for (HexFetchRange& range : clampedRanges)
        {
            if (range.pageCount > static_cast<std::uint64_t>(ksword::memwb::HexViewport::kMaxCachedPages))
            {
                range.pageCount = static_cast<std::uint64_t>(ksword::memwb::HexViewport::kMaxCachedPages);
            }
        }

        // ---- R1：只取一次 capture()（决策 2 落实要点 1）----
        // cap.session 供 Gate 判定与端口读取使用；cap.rev.source 是"目标轴"
        // （targetRevision），同时作为陈旧性比较值与 DDMA 闩锁键——与
        // sourceRevision 参数（画布轴，canvasRevision）是两个独立的数轴，
        // 不得混用，见头文件"两条数轴"一节。capture() 内部会先把 DDMA 代次
        // 拉取一遍（仅通道为 Ddma 时），可能同步发出 sessionChanged（D9）；
        // 调用之后不缓存 canvas_ 的任何引用，后面每次用到都重新读这个
        // QPointer 成员。
        const TargetCapture cap = target_->capture();
        const std::uint64_t targetRevision = cap.rev.source;
        const std::uint64_t canvasRevision = sourceRevision;

        // cancelRangesIfCanvasStillMatches：下面两个"同步拒绝"分支共用的小
        // helper——只有此刻 canvas_ 仍然存在、且它的来源代次仍然等于这次调用
        // 收到的 canvasRevision 时才真的 cancelPages（D6 的防线：哪怕理论上
        // 这几行之间不太可能发生重入，也不假设"同一个函数调用栈内一定不会
        // 变"）。
        const auto cancelRangesIfCanvasStillMatches = [this, canvasRevision](const std::vector<HexFetchRange>& list)
        {
            if (canvas_ && canvas_->sourceRevision() == canvasRevision)
            {
                for (const HexFetchRange& range : list)
                {
                    canvas_->cancelPages(range);
                }
            }
        };

        const ksword::memwb::GateInputs gateInputs =
            gateInputsProvider_ ? gateInputsProvider_() : ksword::memwb::GateInputs{};
        const ksword::memwb::GateVerdict verdict =
            ksword::memwb::EvaluateChannel(cap.session.scope, cap.session.channel, gateInputs);
        if (!verdict.available)
        {
            // 不可用：对每个 range 调用 cancelPages（不是 deliverUnreadable——
            // "通道不可用"与"目标本身读不到"是两件事），一次端口调用都不
            // 发起。
            cancelRangesIfCanvasStillMatches(clampedRanges);
            // D2 续：去抖——同一个 GateReason 连续命中只在第一次发信号，避免
            // 画布每次重绘触发的重复 RequestPages 把状态条刷成一直闪烁。
            if (!lastGateUnavailableReason_.has_value() || *lastGateUnavailableReason_ != verdict.reason)
            {
                lastGateUnavailableReason_ = verdict.reason;
                emit channelUnavailable(verdict);
            }
            return;
        }
        // Gate 恢复可用：清空去抖记忆，下一次再变不可用时（哪怕原因相同）
        // 也会重新发一次信号——那是一次"新的"不可用事件。
        lastGateUnavailableReason_.reset();

        // ---- R2：DDMA 脏扇区闩锁前置判定 ----
        // 只有通道确实是 Ddma、闩锁确实置位、且闩锁记的**目标轴**代次与本次
        // 请求取到的 targetRevision 相符时才拦截（决策 2：闩锁键已经从画布轴
        // 改成目标轴，requestReload() 推进目标代次会自动释放闩锁）。
        const bool isDdmaChannel = (cap.session.channel == ksword::memwb::Channel::Ddma);
        if (isDdmaChannel && scratchLatched_ && scratchLatchedForRevision_ == targetRevision)
        {
            // 按"整体已知不可用"处理：cancelPages 全部 range，不问端口，并
            // 发 retryBlockedByLatch（D10：撞上闩锁不再是"什么都不提示"的
            // 静默拦截）。闩锁置位时已经发过一次 scratchAreaDirtyLatched，
            // 这里不重复发那个信号。
            cancelRangesIfCanvasStillMatches(clampedRanges);
            // N5 修复：同一组（闩锁目标轴代次，触发请求的画布轴代次）只通知
            // 一次——不去抖的话画布每次重绘都会在这里重发信号，与 Gate 路径
            // （lastGateUnavailableReason_）的去抖写法对称。
            const std::pair<std::uint64_t, std::uint64_t> latchNotifyKey{scratchLatchedForRevision_, canvasRevision};
            if (!lastLatchRetryNotifiedFor_.has_value() || *lastLatchRetryNotifiedFor_ != latchNotifyKey)
            {
                lastLatchRetryNotifiedFor_ = latchNotifyKey;
                emit retryBlockedByLatch(canvasRevision);
            }
            return;
        }

        submitJob(cap, canvasRevision, targetRevision, isDdmaChannel, clampedRanges, purpose);
    }

    void WorkbenchPageProvider::submitJob(
        const TargetCapture& cap,
        std::uint64_t canvasRevision,
        std::uint64_t targetRevision,
        bool isDdmaChannel,
        const std::vector<HexFetchRange>& ranges,
        ReadPurpose purpose)
    {
        // ---- 投给唯一的单线程读池 ----
        const std::uint64_t ticket = ++jobCounter_;
        const std::uint64_t submissionGeneration = generation_;

        // N2 修复：每个任务各自一份"是否已经开始跑"的标志，初值 false，与
        // PendingJobRecord::started 共享引用计数（见文件头 N2 一节）。
        auto started = std::make_shared<std::atomic<bool>>(false);

        // D4/D5/D6：登记这次提交的记账——canvasAllInFlight 丢弃"排队中、从未
        // 真正跑过"的任务时，只能靠这张表知道该对画布做什么。正常落地（无论
        // 新鲜/陈旧/失败/代次不符）都会在 onJobFinishedOnUiThread 开头把自己
        // 的条目摘掉。
        pendingJobs_[ticket] = PendingJobRecord{canvasRevision, ranges, started, purpose};

        // D3：全新的一次提交，先把共享的"通道中毒"标志清回 false——保证这次
        // 请求总有机会真正问一次端口。如果它恰好排在一个"即将中毒"的任务
        // 后面，等它真正开始跑时，前一个任务可能早已把标志重新置位，这正是
        // D3 要抓的"已排队任务不得再碰已知坏掉的通道"，不是本行的职责。
        readPool_->ResetPoison();

        auto cancelFlag = std::make_shared<std::atomic<bool>>(false);
        auto* job = new InFlightJob(
            QPointer<WorkbenchPageProvider>(this),
            port_.get(),
            cap.session,
            ticket,
            canvasRevision,
            targetRevision,
            submissionGeneration,
            isDdmaChannel,
            ranges,
            cancelFlag,
            readPool_->PoisonFlag(),
            started,
            readPool_.get());
        readPool_->Submit(job, ticket, std::move(cancelFlag));
    }

    void WorkbenchPageProvider::cancelAllInFlight()
    {
        // D4：代次推进——任何此刻还没落地的任务，之后落地时都会发现自己的
        // generation 落后于 generation_，被判定为已作废。
        ++generation_;

        // D5/D6：立即（同步，在本函数自己的调用栈里）对"已提交、尚未落地"的
        // 任务逐条核对画布代次，补一次 cancelPages——这是"排队中、从未真正
        // 跑过 run()"的任务唯一能被通知到的机会（它们接下来会被
        // readPool_->CancelAll() 的 QThreadPool::clear() 直接删除，永远不会
        // 触发 onJobFinishedOnUiThread）。仍在跑的、或已经跑完但完成事件还
        // 卡在队列里的任务，稍后落地时会走 D4 的代次核对分支，不需要在这里
        // 对它们做第二次处理（这里统一处理一次就够）。
        for (const auto& entry : pendingJobs_)
        {
            const PendingJobRecord& record = entry.second;
            if (canvas_ && canvas_->sourceRevision() == record.canvasRevision)
            {
                for (const HexFetchRange& range : record.ranges)
                {
                    canvas_->cancelPages(range);
                }
            }
        }
        pendingJobs_.clear();

        // 身份变化之后，旧的"失败待重试"/"暂停待恢复"记账都不再对应任何
        // 有意义的地址——新地址空间会自己 InvalidateAll，继续留着只会让
        // retryAllFailed()/下次 setCanvas() 恢复误碰旧地址。
        failedRanges_.clear();
        pausedPendingRanges_.clear();

        if (readPool_)
        {
            readPool_->CancelAll();
        }
    }

    bool WorkbenchPageProvider::hasInFlightRequests() const noexcept
    {
        // Wave 3 wpJ5 新增（增量①）：pendingJobs_ 恰好就是"已提交、尚未落地"的
        // 任务记账表（见头文件 PendingJobRecord 的声明处注释），不管是仍在读线程
        // 里跑着的那一个，还是已经排队但还没真正开始跑的后续任务，都在表里；
        // 落地（无论哪条分支）都会在 onJobFinishedOnUiThread 开头摘掉自己的条目。
        // 因此"表非空"与"确实有未完成的页读取请求"是同一件事，不需要另开计数。
        return !pendingJobs_.empty();
    }

    void WorkbenchPageProvider::resetScratchLatch()
    {
        scratchLatched_ = false;
        scratchLatchedForRevision_ = 0;
        // N5 修复：闩锁被显式清除，去抖记忆一起清空——下一次命中（哪怕键值
        // 恰好相同）要被当成"新的一次"重新通知。
        lastLatchRetryNotifiedFor_.reset();
    }

    HexFetchRange WorkbenchPageProvider::normalizeRangeForRetry(const HexFetchRange& range)
    {
        // pageCount==0 的输入原样返回——调用方据此判断要不要整体跳过这一条，
        // 不在这里强行拼一个"至少一页"的范围（那样会把"调用方传了个空范围"
        // 这个事实悄悄抹掉）。
        if (range.pageCount == 0)
        {
            return range;
        }

        // D7/D11：先把起点向下对齐到页边界（调用方可能传来字节级别未对齐的
        // "范围"，典型来源是 WriteController 按字节换算时的粗略估计）。
        const std::uint64_t alignedStart = ksword::memwb::HexViewport::PageStartOf(range.firstPageStart);

        // span：原始范围覆盖的字节数；originalLastByte：覆盖到的最后一个字节
        // 地址，溢出时夹到 2^64-1（不当作"非法"直接拒绝——夹到地址空间上限
        // 本身就是一种安全的规整）。pageCount 本身可能大到乘以 kPageBytes 就
        // 先溢出（例如调用方直接传来一个荒谬的页数），这里先判一次乘法会不会
        // 溢出，溢出直接按"覆盖到地址空间尽头"处理，不先算出一个错误的 span。
        constexpr std::uint64_t kPageBytes = ksword::memwb::HexViewport::kPageBytes;
        std::uint64_t originalLastByte = (std::numeric_limits<std::uint64_t>::max)();
        if (range.pageCount <= (std::numeric_limits<std::uint64_t>::max)() / kPageBytes)
        {
            const std::uint64_t span = range.pageCount * kPageBytes;
            if (span > 0 && range.firstPageStart <= (std::numeric_limits<std::uint64_t>::max)() - (span - 1ULL))
            {
                originalLastByte = range.firstPageStart + (span - 1ULL);
            }
        }

        const std::uint64_t alignedLastPageStart = ksword::memwb::HexViewport::PageStartOf(originalLastByte);
        std::uint64_t pageCount = (alignedLastPageStart - alignedStart) / kPageBytes + 1ULL;
        const std::uint64_t maxCachedPages = static_cast<std::uint64_t>(ksword::memwb::HexViewport::kMaxCachedPages);
        if (pageCount > maxCachedPages)
        {
            // 超过缓存容量：夹取到容量上限——多读的页缓存也装不下，没有意义
            // （D11：超大范围会把可见页挤出缓存，不如干脆只读能装下的那一截）。
            pageCount = maxCachedPages;
        }

        HexFetchRange normalized;
        normalized.firstPageStart = alignedStart;
        normalized.pageCount = pageCount;
        return normalized;
    }

    void WorkbenchPageProvider::retryFailedRanges(const std::vector<HexFetchRange>& ranges)
    {
        if (ranges.empty() || target_ == nullptr)
        {
            return;
        }
        // 决策 2 落实要点 4：画布为空就没有地方回填，直接返回；画布轴用
        // canvas_->sourceRevision()（不是 target 轴——这是审核报告里
        // "retryaxis"那条缺陷的根因：旧实现传的是 target 的代次，画布按自己
        // 的轴比对，整页拒收）。
        if (!canvas_)
        {
            return;
        }

        // D7/D11：逐段页对齐 + 夹取容量，调用方可能传来字节级别未对齐、或
        // 超出缓存容量的范围。
        std::vector<HexFetchRange> normalized;
        normalized.reserve(ranges.size());
        for (const HexFetchRange& range : ranges)
        {
            if (range.pageCount == 0)
            {
                continue;
            }
            normalized.push_back(normalizeRangeForRetry(range));
        }
        if (normalized.empty())
        {
            return;
        }

        requestPages(normalized, canvas_->sourceRevision(), ReadPurpose::ExplicitRetry);
    }

    void WorkbenchPageProvider::rereadByteRange(std::uint64_t address, std::uint64_t length)
    {
        if (length == 0 || target_ == nullptr || !canvas_)
        {
            return;
        }

        // 字节范围 -> 页范围：先算出覆盖到的最后一个字节地址（溢出按"夹到
        // 2^64-1"处理，与 normalizeRangeForRetry 同一套规则），再交给同一个
        // 规整函数做页对齐与容量夹取（D7/D11，与 retryFailedRanges 共用逻辑，
        // 这里先拼一个"伪页范围"：firstPageStart=address，pageCount 按字节数
        // 估算出一个上界，规整函数会重新按真实页数算一遍，不依赖这个估算的
        // 精确性，只要不小于真实需要的页数即可）。
        const std::uint64_t lastByte = (length - 1ULL <= (std::numeric_limits<std::uint64_t>::max)() - address)
            ? (address + length - 1ULL)
            : (std::numeric_limits<std::uint64_t>::max)();
        const std::uint64_t pageStart = ksword::memwb::HexViewport::PageStartOf(address);
        const std::uint64_t pageEnd = ksword::memwb::HexViewport::PageStartOf(lastByte);
        const std::uint64_t pageCount =
            (pageEnd - pageStart) / ksword::memwb::HexViewport::kPageBytes + 1ULL;

        HexFetchRange estimate;
        estimate.firstPageStart = pageStart;
        estimate.pageCount = pageCount;
        const HexFetchRange range = normalizeRangeForRetry(estimate);
        if (range.pageCount == 0)
        {
            return;
        }

        // 去重：这次要读的每一页如果都已经在某个"已提交、尚未落地、且还没
        // 真正开始跑"的任务的记账范围内，直接跳过，避免连续撤销/重做堆积
        // 重复任务；只要有一页不在就整段照常提交（不拆成"只读缺的那几页"）。
        //
        // N2 修复（真实缺口）：record.started 为真的记账**不提供去重覆盖**。
        // 这个任务已经真正开始跑，可能已经读过这一页，继续靠它去重会把写
        // 后的重读请求整段吞掉，画面停在写之前的值。只有"还在队列里排队、
        // 从未被 run() 碰过"的任务才适合去重——它迟早会读到最新内容。
        bool everyPageAlreadyPending = !pendingJobs_.empty();
        for (std::uint64_t index = 0; everyPageAlreadyPending && index < range.pageCount; ++index)
        {
            const std::uint64_t probe = range.firstPageStart + index * ksword::memwb::HexViewport::kPageBytes;
            bool found = false;
            for (const auto& entry : pendingJobs_)
            {
                const PendingJobRecord& record = entry.second;
                // A replaceable browsing read must never satisfy protected post-write readback.
                if (record.purpose != ReadPurpose::PostWrite || (record.started && record.started->load()))
                {
                    // 已经开始跑：不提供去重覆盖，看下一条记账。
                    continue;
                }
                for (const HexFetchRange& pending : record.ranges)
                {
                    if (probe >= pending.firstPageStart
                        && probe < pending.firstPageStart + pending.pageCount * ksword::memwb::HexViewport::kPageBytes)
                    {
                        found = true;
                        break;
                    }
                }
                if (found)
                {
                    break;
                }
            }
            if (!found)
            {
                everyPageAlreadyPending = false;
            }
        }
        if (everyPageAlreadyPending)
        {
            return;
        }

        requestPages({range}, canvas_->sourceRevision(), ReadPurpose::PostWrite);
    }

    bool WorkbenchPageProvider::isSourceFresh(std::uint64_t capturedTargetRevision)
    {
        if (target_ == nullptr)
        {
            return false;
        }
        // 契约：比较之前必须先拉取一次最新状态。capture() 内部会先调用
        // session()，通道为 Ddma 时据此拉一遍最新暂存扇区代次，必要时 bump
        // 目标轴来源代次——确保"换了暂存扇区"这类没有可靠回调的变化不会被漏
        // 判为"未陈旧"（头文件 isSourceFresh 的契约注释，对应 Wave 2 审核
        // wpI D2 的同一形状）。调用之后不缓存任何引用，调用方（本文件内）
        // 后续用到 canvas_ 都重新读这个 QPointer 成员（D9）。
        const TargetCapture latest = target_->capture();
        return latest.rev.source == capturedTargetRevision;
    }

    void WorkbenchPageProvider::onJobFinishedOnUiThread(std::uint64_t jobTicket)
    {
        if (!readPool_)
        {
            return;
        }
        std::optional<ReadPool::CompletedJob> maybeJob = readPool_->TakeCompleted(jobTicket);
        if (!maybeJob)
        {
            // 票据不存在：可能从未存在，也可能这次结果已经被别的路径处理过。
            // 没有东西可核对，直接返回。
            return;
        }
        const ReadPool::CompletedJob& job = *maybeJob;

        // 这份记账到这里就算"落地"了，不管接下来走哪条分支，provider 自己的
        // 挂起表都不再需要它（D4/D5/D6）。
        pendingJobs_.erase(jobTicket);

        // Wave 3 wpJ5 新增（增量①）：从这一行开始，函数无论接下来走哪条分支
        // return，都必须先发一次 jobLanded()——本函数下面的每一个 return 语句
        // 前都手动补了一次 emit（不用"统一出口"的写法，是为了不触碰这段代码
        // 既有的、已经独立审核过的控制流结构，降低引入新缺陷的风险；代价是
        // 以后再加新的 return 分支时要记得同步补一次，已在每处加注释提醒）。
        // 契约：hasInFlightRequests() 在 pendingJobs_.erase 之后、jobLanded()
        // 发出之前的这一小段窗口内可能已经变回"空"（如果这是最后一个在途任务），
        // 订阅者据此驱动的 noteDirty 要的正是"现在没有在途了"这个事实，顺序上
        // 没有问题。

        // D4：代次核对——cancelAllInFlight 在这份结果还没落地之前被调用过，
        // 对应的 cancelPages 已经在 cancelAllInFlight 自己的调用栈里同步处理
        // 过了（见该函数），这里只负责不把它当成新鲜结果展示，不重复触碰
        // 画布。
        if (job.jobGeneration != generation_)
        {
            emit jobLanded(); // wpJ5 增量①：这条分支也取到了真实的完成记账，照发。
            return;
        }

        // 先扫一遍全部 range 的结果，汇总两个判断要用到的聚合信息，外加
        // "第一条失败文本"：
        // - anyCancelled：任一 range 带着 cancelled，说明这批任务在在途时被
        //   cancelAllInFlight 作废过。正常情况下 D4 的代次核对已经先拦住了
        //   这种结果（cancelFlag_ 只由 cancelAllInFlight 置位，而它恒会先
        //   推进 generation_），这里保留只是防御——万一出现 cancelled=true
        //   而代次恰好没变的边界情况，同样整批丢弃，不展示任何假数据。
        // - anyScratchDirty：任一 range 报告过 DDMA 暂存区被弄脏；这个检查
        //   与 channelFailed 互相独立，脏扇区信号要如实落地，不能因为走了
        //   失败分支就漏报。
        // N3 修复：不再汇总"anyChannelFailed"这个全局布尔——channelFailed
        // 现在按 range 逐个处理（见下面的主循环），否则会退回 N3 的原始
        // 缺陷（同批已读成功的 range 被另一个失败的 range 连带丢弃）。
        bool anyCancelled = false;
        bool anyScratchDirty = false;
        QString firstFailureText;
        for (const JobRangeResult& item : job.rangeResults)
        {
            if (item.result.cancelled)
            {
                anyCancelled = true;
            }
            if (item.result.channelFailed && firstFailureText.isEmpty() && !item.result.failure.empty())
            {
                firstFailureText = QString::fromStdString(item.result.failure);
            }
            if (item.result.scratchAreaDirty)
            {
                anyScratchDirty = true;
            }
        }

        if (anyCancelled)
        {
            // 见上面的防御性说明：D4 的代次核对正常已经处理过，这里再补一次
            // 画布代次守卫的 cancelPages 不会有副作用。"取消"是用户显式动作，
            // 与 channelFailed 不是同一类事：整批撤销、不记进 failedRanges_。
            if (canvas_ && canvas_->sourceRevision() == job.canvasRevision)
            {
                for (const JobRangeResult& item : job.rangeResults)
                {
                    canvas_->cancelPages(item.range);
                }
            }
            emit jobLanded(); // wpJ5 增量①：被取消的任务也已经真正处理完毕。
            return;
        }

        if (!canvas_)
        {
            // N6 修复：画布已经不在了（暂停或真销毁，分不清）。结果没地方
            // 交付，但不能什么都不记——统一记进 pausedPendingRanges_，
            // setCanvas() 侦测到"从空恢复为非空"时会消费它（见该函数）。
            for (const JobRangeResult& item : job.rangeResults)
            {
                pausedPendingRanges_.push_back(PausedRangeRecord{job.canvasRevision, item.range});
            }
            emit jobLanded(); // wpJ5 增量①：丢弃也是处理完毕，记账已经补上。
            return;
        }

        // N1 修复（D9 没修好的根因）：capture() 仍可能同步触发
        // sessionChanged、同步调用 setCanvas(nullptr)；上一轮只在调用之前
        // 判过一次 canvas_，这正是 0xC0000005 的根因——调用后立刻重新判空。
        const bool sourceFresh = isSourceFresh(job.targetRevision);
        if (!canvas_)
        {
            emit jobLanded(); // wpJ5 增量①：重入把画布摘空，同样算落地完毕。
            return;
        }

        if (!sourceFresh)
        {
            // 核对不通过（**目标轴**已经前进）：整批 cancelPages（画布代次
            // 仍等于提交时的 canvasRevision 才做，D6）。用原始 range 粒度，
            // 整批撤销才能让它们恢复到"可以再次请求"的状态；这类与
            // channelFailed 不同（目标变了不是通道坏了），不记进 failedRanges_。
            if (canvas_->sourceRevision() == job.canvasRevision)
            {
                for (const JobRangeResult& item : job.rangeResults)
                {
                    canvas_->cancelPages(item.range);
                }
            }
            emit jobLanded(); // wpJ5 增量①：目标轴已前进导致的整批撤销也是一次落地。
            return;
        }

        // 核对通过，先落地 DDMA 脏扇区闩锁——与是否 channelFailed 无关，见
        // 上面的聚合说明。闩锁键是**目标轴**（决策 2），只在"确实是 Ddma
        // 通道的这次请求"且"闩锁尚未命中这个目标代次"时才置位并发信号，
        // 保证同一目标代次内只发一次。
        if (job.isDdma && anyScratchDirty)
        {
            if (!scratchLatched_ || scratchLatchedForRevision_ != job.targetRevision)
            {
                scratchLatched_ = true;
                scratchLatchedForRevision_ = job.targetRevision;
                emit scratchAreaDirtyLatched();
            }
        }

        // D6 续（本包自己测出的同族缺口，不是审核报告原文，记在这里防止
        // 回归）：isSourceFresh 只核对**目标轴**，target 没变但 canvas 换了
        // 地址空间（例如宿主在同一个目标上重建了可见窗口）时也会走到这里。
        // deliverPage/deliverUnreadable 内部会用 job.canvasRevision 去比
        // HexViewport 自己的 sourceRevision_，代次不符会被自动拒收，天然
        // 安全；但 cancelPages 没有任何代次校验（HexViewport::CancelInFlight
        // 只校验对齐/边界），如果这里不额外守卫，NotAttempted 分支的
        // cancelPages 就会按"地址"直接抹掉新地址空间刚刚登记的同一地址的
        // 在途页——即使这批结果本身因为代次不符根本不会被交付。canvasStale
        // 为真时整段跳过，不逐页处理（既不交付也不撤销，让新地址空间自己的
        // 登记原样保留）。
        const bool canvasStale = (canvas_->sourceRevision() != job.canvasRevision);
        if (canvasStale)
        {
            emit jobLanded(); // wpJ5 增量①：画布代次已经前进，这批结果整段跳过也算落地。
            return;
        }

        // N3/N4 修复：按 range 逐个处理，不再用一个全局布尔决定整批要不要
        // 丢弃。deliverPageRecord 只负责"这一页怎么交付"，两个分支共用，
        // 避免同一段转换代码抄两遍；NotAttempted 不在这里处理——两个分支
        // 对它的处置不同，留给调用者自己决定（见下）。
        const auto deliverPageRecord = [this, &job](const ksword::memwb::PageRecord& page)
        {
            switch (page.state)
            {
            case ksword::memwb::PageState::Valid:
            case ksword::memwb::PageState::PartiallyValid:
            {
                QByteArray bytes(
                    reinterpret_cast<const char*>(page.bytes.data()), static_cast<int>(page.bytes.size()));
                QByteArray mask(
                    reinterpret_cast<const char*>(page.valid.data()), static_cast<int>(page.valid.size()));
                // 回填画布必须原样带回**画布轴**（canvasRevision），不是
                // 目标轴——这是决策 2 修复前"axismismatch"那类缺陷的
                // 根因，HexViewport::InsertPage 比对的是它自己的
                // sourceRevision_（即画布轴）。
                canvas_->deliverPage(page.pageStart, bytes, mask, job.canvasRevision);
                break;
            }
            case ksword::memwb::PageState::Unreadable:
                canvas_->deliverUnreadable(HexFetchRange{page.pageStart, 1, 0}, job.canvasRevision);
                break;
            case ksword::memwb::PageState::NotAttempted:
            default:
                break; // 调用者自己处理 NotAttempted（见下面两个分支）。
            }
        };

        // failedRangeCountThisJob：本次落地里新记进 failedRanges_ 的 range
        // 条数，供 readFailed 信号携带（N3/N4 修复新增）。
        int failedRangeCountThisJob = 0;
        for (const JobRangeResult& item : job.rangeResults)
        {
            if (item.skippedDueToPoison)
            {
                // D3：整段从未问过端口，result.pages 恒为空。N4 修复：必须
                // 记进 failedRanges_，否则这段地址永远卡在 Pending——没有
                // 任何代码路径会再碰它。
                failedRanges_.push_back(item.range);
                ++failedRangeCountThisJob;
                continue;
            }
            if (item.result.channelFailed)
            {
                // N3 修复：通道失败的这一段**不**跟着整批丢弃同批其它已经
                // 读成功的 range；它自己真正读到的页仍按 deliverPageRecord
                // 正常交付（同一个 range 可能一半有效一半失败）。残留的
                // NotAttempted 页**不** cancelPages（否则又会自动重试），
                // 保持 Pending 并记进 failedRanges_，只能显式重试才再问端口。
                for (const ksword::memwb::PageRecord& page : item.result.pages)
                {
                    deliverPageRecord(page);
                }
                failedRanges_.push_back(item.range);
                ++failedRangeCountThisJob;
                continue;
            }

            // 正常（channelFailed 与 skippedDueToPoison 都为假）：按原逻辑
            // 逐页回填。NotAttempted 的唯一成因是"同一 range 内更早的块报
            // scratchAreaDirty 导致整体停止"——cancelPages 允许之后重试，
            // 下一次请求会先撞上闩锁前置判定，不会真的再碰端口。
            for (const ksword::memwb::PageRecord& page : item.result.pages)
            {
                deliverPageRecord(page);
                if (page.state == ksword::memwb::PageState::NotAttempted)
                {
                    canvas_->cancelPages(HexFetchRange{page.pageStart, 1, 0});
                }
            }
        }

        if (failedRangeCountThisJob > 0)
        {
            // D2 决策(a) 续：不自动重试，必须显式 retryFailedRanges/
            // retryAllFailed 才会再问端口；count 是本次新增条数，供状态条
            // 展示"N 处失败/待重试"。
            emit readFailed(job.canvasRevision, firstFailureText, failedRangeCountThisJob);
        }

        // wpJ5 增量①：无论这次落地有没有失败的 range，都要发一次
        // jobLanded()——"记账被处理完毕"与"结果是否全部成功"是两件事。
        emit jobLanded();
    }

    std::vector<HexFetchRange> WorkbenchPageProvider::failedRanges() const
    {
        // N3/N4 修复新增：只读快照，调用方（状态条/诊断抽屉）据此展示"N 处
        // 可重试"，不触发任何状态变化。
        return failedRanges_;
    }

    void WorkbenchPageProvider::retryAllFailed()
    {
        // N3/N4 修复新增：状态条"重试"按钮的落点。先取出当前全部记账并
        // 立刻清空——若这次重试仍然失败/被跳过，onJobFinishedOnUiThread
        // 会把对应 range 原样重新记回来，不会丢记账。复用
        // retryFailedRanges 做页对齐/夹取/Gate/闩锁判定，不重新实现一遍。
        if (failedRanges_.empty())
        {
            return;
        }
        std::vector<HexFetchRange> ranges = std::move(failedRanges_);
        failedRanges_.clear();
        retryFailedRanges(ranges);
    }
}
