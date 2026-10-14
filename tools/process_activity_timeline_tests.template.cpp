// 只替代采样宿主；窗口、快照冻结、绘制坐标和鼠标命中全部来自当前生产方法。
#include <QRectF>
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <deque>
#include <iostream>
#include <iterator>

struct HistoryLimit
{
    int count = 50; // 最近模式的保留数。
    int value() const { return count; }
};

struct ProcessDock
{
    enum class ActivityHistoryMode { None, All, Recent };
    struct ProcessActivitySample
    {
        std::uint64_t elapsedMs = 0; // 实际采样时间。
    };
    ActivityHistoryMode m_activityHistoryMode = ActivityHistoryMode::All;
    HistoryLimit limit; // 模拟最近数量输入，不涉及控件绘制。
    HistoryLimit* m_activityHistoryLimitSpin = &limit;
    std::deque<ProcessActivitySample> m_activitySamples;
    bool m_activityTimelinePinnedToLatest = true;
    bool snapshot = false; // 是否正在查看历史快照。
    int interval = 1000; // 采样周期毫秒。
    int refreshIntervalMillisecondsFromInput() const { return interval; }
    bool isProcessActivityTableSnapshotActive() const { return snapshot; }
};

struct Timeline
{
    ProcessDock* m_ownerDock = nullptr; // 生产方法读取的采样宿主。
    double m_liveEndMs = 0.0; // 连续绘制时钟。
    double m_frozenWindowEndMs = 0.0; // 快照冻结的右端点。
    double m_frozenWindowDurationMs = 0.0; // 快照冻结的跨度。
    QRectF chartRect() const { return QRectF(42.0, 8.0, 1000.0, 100.0); }
__PRODUCTION_METHODS__
};

int failures = 0; // 汇总所有断言后返回失败状态。
int checks = 0; // 输出实际检查数量。

// 检查生产方法结果；失败输出具体用户行为，不以运行成功替代行为断言。
void check(bool passed, const char* message)
{
    ++checks;
    if (!passed)
    {
        ++failures;
        std::cerr << "FAIL: " << message << '\n';
    }
}

// 以固定时间输入覆盖长时间留存、模式切换、快照和边界采样。
int main()
{
    ProcessDock dock;
    Timeline chart;
    chart.m_ownerDock = &dock;
    const QRectF plot = chart.chartRect(); // 绘制和命中共同使用的实际 Qt 矩形。
    for (std::uint64_t time = 0; time <= 120000; time += 1000)
    {
        dock.m_activitySamples.push_back({ time });
    }
    chart.m_liveEndMs = 119500.0;
    check(chart.sampleIndexToX(0, plot) >= plot.left(), "Keep all must display the oldest retained sample");
    check(chart.sampleIndexToX(120, plot) <= plot.right(), "Keep all must display the newest retained sample");
    check(chart.sampleIndexAtX(42) == 0, "Keep all left-edge hit testing must reach the oldest sample");
    check(chart.sampleIndexAtX(1042) == 120, "Keep all right-edge hit testing must reach the newest sample");
    const double oldDuration = chart.windowDurationMs(); // 新采样前跨度用于验证点距压缩。
    const double oldSpacing = chart.sampleIndexToX(1, plot) - chart.sampleIndexToX(0, plot);
    dock.limit.count = 2;
    check(chart.windowDurationMs() == oldDuration, "Keep all must ignore the hidden recent-count input");
    dock.m_activitySamples.push_back({ 121000 });
    chart.m_liveEndMs = 120500.0;
    check(chart.windowDurationMs() > oldDuration, "Keep all must expand the time span as samples arrive");
    check(chart.sampleIndexToX(1, plot) - chart.sampleIndexToX(0, plot) < oldSpacing,
        "Keep all must reduce X spacing rather than scroll away old samples");

    // 冻结原窗口后，后台采样与模式变化都不能移动已选快照。
    chart.freezeVisibleTimelineWindow();
    const double frozenX = chart.sampleIndexToX(30, plot);
    dock.snapshot = true;
    dock.m_activityTimelinePinnedToLatest = false;
    dock.m_activitySamples.push_back({ 180000 });
    chart.m_liveEndMs = 180000.0;
    dock.m_activityHistoryMode = ProcessDock::ActivityHistoryMode::Recent;
    check(chart.sampleIndexToX(30, plot) == frozenX, "Selecting history must preserve the original X position");
    dock.snapshot = false;
    dock.m_activityTimelinePinnedToLatest = true;
    dock.limit.count = 50;
    check(chart.windowDurationMs() == 49000.0, "Keep latest must retain its fixed scrolling span");
    check(chart.sampleIndexToX(0, plot) < plot.left(), "Keep latest must scroll old samples out of view");
    dock.m_activityHistoryMode = ProcessDock::ActivityHistoryMode::All;
    check(chart.sampleIndexAtX(42) == 0, "Returning to Keep all must restore the oldest retained sample");

    // 窄窗与不同采样周期仍覆盖实际首尾，不能把全量跨度退回到条数乘周期。
    for (const int interval : { 100, 1000, 5000 })
    {
        dock.interval = interval;
        for (const double width : { 80.0, 1000.0, 2400.0 })
        {
            const QRectF resizedPlot(42.0, 8.0, width, 100.0);
            check(std::abs(chart.sampleIndexToX(0, resizedPlot) - resizedPlot.left()) < 0.000001,
                "Keep all must anchor the oldest sample at the left across widths and intervals");
            check(chart.sampleIndexToX(122, resizedPlot) <= resizedPlot.right(),
                "Keep all must include the latest sample across widths and intervals");
        }
    }

    // 最近模式已裁剪的样本不能复活，全量改为覆盖仍实际留存的首尾时间。
    dock.m_activitySamples = { {60000}, {61000}, {62000} };
    chart.m_liveEndMs = 62000.0;
    check(chart.windowEndMs() - chart.windowDurationMs() <= 60000.0,
        "Keep all after trimming must include the first remaining sample");
    dock.m_activitySamples = { {62000} };
    check(chart.windowDurationMs() > 0.0, "Single-sample history must keep a nonzero span");
    check(std::isfinite(chart.sampleIndexToX(0, plot)), "Single-sample X must be finite");
    dock.m_activitySamples.clear();
    check(chart.windowDurationMs() > 0.0 && chart.sampleIndexAtX(42) == -1,
        "Empty history must keep a valid axis and have no sample hit");
    std::cout << "PROCESS_ACTIVITY_TIMELINE_CHECKS=" << checks << " FAILURES=" << failures << '\n';
    return failures == 0 ? 0 : 1;
}
