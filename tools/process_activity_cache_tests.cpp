#include "__HEADER__"
#include <algorithm>
#include <array>
#include <cassert>
#include <cstdint>
#include <deque>
#include <iostream>
#include <atomic>
#include <memory>
#include <functional>
#include <string>
#include <unordered_set>
#include <vector>
#define Q_ASSERT(value) assert(value)
struct QElapsedTimer { inline static int checksBeforeYield=100000000; int checks=0; void start() {} int elapsed() { return ++checks >= checksBeforeYield ? 4 : 0; } };
struct QRunnable { static std::function<void()> create(std::function<void()> task) { return task; } };
struct QThreadPool {
 inline static std::vector<std::function<void()>> tasks;
 static QThreadPool* globalInstance() { static QThreadPool instance;return &instance; }
 void start(std::function<void()> task) { tasks.push_back(std::move(task)); }
 static void drain() { auto queued=std::move(tasks);tasks.clear();for(auto& task:queued) task(); }
};
struct ProcessDock {
 enum class ProcessActivityMetric { Cpu,Memory,Disk,Network,Gpu };
 struct ProcessPoint { std::string identityKey; double cpuPercent=1,workingSetMB=2,diskMBps=3,netKBps=4,gpuPercent=5; };
 struct Processes {
  inline static unsigned reads=0;
  std::vector<ProcessPoint> rows{{"A"},{"B"}};
  std::size_t size() const { return rows.size(); }
  const ProcessPoint& operator[](std::size_t index) const { if(index==0) ++reads; return rows[index]; }
 };
 struct ProcessActivitySample {
  std::uint64_t sequence=0, elapsedMs=0;
  double totalCpuPercent=2,totalMemoryMB=4,totalDiskMBps=6,totalNetKBps=8,totalGpuPercent=10;
  Processes processes;
 };
 std::deque<ProcessActivitySample> m_activitySamples;
 std::uint64_t m_activityRecordingStartTick100ns=1;
 double m_activityTotalPhysicalMemoryMB=1024;
};
struct Fixture {
 struct SeriesPoint { double timeMs=0,percent=0,slope=0; };
 struct CachedSeries { ProcessDock::ProcessActivityMetric metric{};std::vector<SeriesPoint> points; };
 struct Rect { double width() const { return 800; } };
 ProcessDock* m_ownerDock;
 ks::ui::activity::SeriesCache m_rawSamples;
 struct RetiredAggregate { ks::ui::activity::SeriesCache cache;std::atomic<bool> complete{false}; };
 std::shared_ptr<RetiredAggregate> m_retiredAggregate;
 bool m_retainedEdgeActive=false;
 ks::ui::activity::SeriesCache::Point m_retainedEdgePoint{};
 unsigned releaseScheduled=0;
 void scheduleRetiredRelease() { ++releaseScheduled; }
 ks::ui::activity::SeriesCache::Point m_pendingPoint{};
 std::size_t m_pendingProcessIndex=0;
 bool m_pendingPointActive=false;
 bool m_aggregateWasPending=false;
 unsigned published=0;
 void scheduleAggregatePublication() { ++published; }
 unsigned scheduled=0;
 void scheduleAggregateContinuation() { ++scheduled; }
 std::uint64_t m_rawRecordingTick=0,m_projectionRevision=0;
 std::vector<std::string> m_rawSelectionKeys;
 double m_projectionStartMs=0,m_projectionEndMs=0;
 std::size_t m_projectionColumns=0;
 std::array<double,5> m_projectionDenominators{};
 std::vector<CachedSeries> m_cachedSeries;
 std::vector<ProcessDock::ProcessActivityMetric> m_cachedMetrics;
 bool m_seriesDirty=true;
 Rect chartRect() const { return {}; }
 double devicePixelRatioF() const { return 1; }
 double windowEndMs() const { return static_cast<double>(m_ownerDock->m_activitySamples.back().elapsedMs); }
 double windowDurationMs() const { return windowEndMs()+100; }
 __METHODS__
};
int main() {
 ProcessDock owner;
 for(std::uint64_t i=0;i<5000;++i) { ProcessDock::ProcessActivitySample point;point.sequence=i;point.elapsedMs=i*50; owner.m_activitySamples.push_back(std::move(point)); }
 Fixture f{&owner};
 std::vector<ProcessDock::ProcessActivityMetric> metrics{ProcessDock::ProcessActivityMetric::Cpu};
 f.updateSeriesCache(metrics,{"A"},{"A"});
 assert(ProcessDock::Processes::reads==5000);
 assert(f.m_rawSamples.size()==5000);
 assert(f.m_rawSamples.at(4000).values[4]==5);
 assert(f.m_cachedSeries.size()==1 && f.m_cachedSeries[0].points.size()<=1602);
 const auto revision=f.m_rawSamples.revision();
 f.updateSeriesCache(metrics,{"A"},{"A"});
 assert(ProcessDock::Processes::reads==5000 && f.m_rawSamples.revision()==revision);
 ProcessDock::ProcessActivitySample point;point.sequence=5000;point.elapsedMs=250000; owner.m_activitySamples.push_back(std::move(point));
 f.updateSeriesCache(metrics,{"A"},{"A"});
 assert(ProcessDock::Processes::reads==5001 && f.m_rawSamples.size()==5001);
 metrics.push_back(ProcessDock::ProcessActivityMetric::Gpu);
 f.updateSeriesCache(metrics,{"A"},{"A"});
 assert(ProcessDock::Processes::reads==5001 && f.m_cachedSeries.size()==2);
 assert(!f.updateSeriesCache(metrics,{"A","B"},{"A","B"}));
 QThreadPool::drain();
 f.updateSeriesCache(metrics,{"A","B"},{"A","B"});
 assert(ProcessDock::Processes::reads==10002);
 const auto bothRevision=f.m_rawSamples.revision();
 f.updateSeriesCache(metrics,{"B","A"},{"A","B"});
 assert(ProcessDock::Processes::reads==10002 && f.m_rawSamples.revision()==bothRevision);
 assert(f.m_rawSamples.at(4000).values[4]==10);
 for(int i=0;i<4990;++i) owner.m_activitySamples.pop_front();
 assert(!f.updateSeriesCache(metrics,{"B","A"},{"A","B"}));
 QThreadPool::drain();
 f.updateSeriesCache(metrics,{"B","A"},{"A","B"});
 assert(ProcessDock::Processes::reads==10013 && f.m_rawSamples.size()==12);
 assert(f.m_rawSamples.front().sequence==4989);
 owner.m_activitySamples.clear(); owner.m_activityRecordingStartTick100ns=2;
 ProcessDock::ProcessActivitySample next;next.sequence=0;next.elapsedMs=0;owner.m_activitySamples.push_back(next);
 assert(!f.updateSeriesCache(metrics,{},{}));
 QThreadPool::drain();
 f.updateSeriesCache(metrics,{},{});
 assert(f.m_rawSamples.size()==1 && f.m_rawSamples.front().sequence==0);
 assert(ProcessDock::Processes::reads==10013);
 assert(f.m_rawSamples.at(0).values[0]==2);
 // A single very large sample yields inside its process list, with no lost/duplicate aggregation.
 ProcessDock large;
 ProcessDock::ProcessActivitySample many; many.processes.rows.assign(10000, {"A"});
 large.m_activitySamples.push_back(std::move(many));
 Fixture split{&large};
 QElapsedTimer::checksBeforeYield=2;
 unsigned chunks=0;
 while(!split.prepareAggregateCache({"A"},{"A"})) { ++chunks; assert(chunks<1000); }
 assert(chunks>10 && split.scheduled==chunks && split.published==1);
 assert(split.m_rawSamples.at(0).values[0]==10000);
 assert(split.m_rawSamples.at(0).values[4]==50000);
 // Replace an incomplete selection before its continuation, then evict its source sample.
 split.m_rawSamples.clear(); split.m_pendingPointActive=false;
 assert(!split.prepareAggregateCache({"A"},{"A"}));
 assert(!split.prepareAggregateCache({"B"},{"B"}));
 while(!split.prepareAggregateCache({"B"},{"B"})) { QThreadPool::drain(); }
 assert(split.m_rawSamples.at(0).values[0]==0);
 split.m_rawSamples.clear();split.m_pendingPointActive=false;
 assert(!split.prepareAggregateCache({"A"},{"A"}));
 large.m_activitySamples.clear();
 ProcessDock::ProcessActivitySample replacement;replacement.sequence=10;replacement.elapsedMs=1000;
 large.m_activitySamples.push_back(replacement);
 while(!split.prepareAggregateCache({"A"},{"A"})) {}
 assert(split.m_rawSamples.size()==1 && split.m_rawSamples.front().sequence==10);
 assert(split.m_rawSamples.at(10).values[0]==1);
 // Actual retire/prepare methods never queue a second generation while disposal is pending.
 ProcessDock churn;
 for(std::uint64_t i=0;i<10000;++i) { ProcessDock::ProcessActivitySample v;v.sequence=i;v.elapsedMs=i;churn.m_activitySamples.push_back(std::move(v)); }
 Fixture recycle{&churn};QElapsedTimer::checksBeforeYield=100000000;
 assert(recycle.prepareAggregateCache({"A"},{"A"}));
 const auto readCount=ProcessDock::Processes::reads;
 assert(!recycle.prepareAggregateCache({"B"},{"B"}));
 assert(recycle.m_rawSamples.storageEmpty() && recycle.m_retiredAggregate->cache.size()==10000);
 const auto retiring=recycle.m_retiredAggregate;
 assert(!recycle.prepareAggregateCache({"A","B"},{"A","B"}));
 assert(recycle.m_retiredAggregate==retiring && QThreadPool::tasks.size()==1);
 assert(recycle.m_rawSamples.storageEmpty() && ProcessDock::Processes::reads==readCount);
 recycle.notifyHistoryReset(); // Hidden reset requires neither another paint nor a snapshot request.
 assert(recycle.releaseScheduled>0 && recycle.m_retiredAggregate==retiring);
 QThreadPool::drain();
 assert(retiring->complete.load() && retiring->cache.storageEmpty());
 assert(recycle.prepareAggregateCache({"B"},{"B"}));
 assert(recycle.m_rawSamples.size()==10000 && recycle.m_rawSamples.at(9999).values[0]==1);
 // A large source-head drop retires once, then rebuilds remaining canonical rows plus one exact edge.
 const auto edgeValue=recycle.m_rawSamples.at(9989).values;
 for(int i=0;i<9990;++i) churn.m_activitySamples.pop_front();
 assert(!recycle.prepareAggregateCache({"B"},{"B"}));
 assert(QThreadPool::tasks.size()==1 && recycle.m_retainedEdgeActive);
 assert(!recycle.prepareAggregateCache({"B"},{"B"}));
 assert(QThreadPool::tasks.size()==1);
 QThreadPool::drain();
 assert(recycle.prepareAggregateCache({"B"},{"B"}));
 assert(recycle.m_rawSamples.size()==11 && recycle.m_rawSamples.front().sequence==9989);
 assert(recycle.m_rawSamples.front().values==edgeValue);
 // Empty history retires even when prepare returns empty immediately; disposal holds no widget.
 churn.m_activitySamples.clear();
 assert(recycle.prepareAggregateCache({},{}));
 assert(recycle.m_rawSamples.storageEmpty() && recycle.m_retiredAggregate);
 const auto detached=recycle.m_retiredAggregate;
 recycle.m_retiredAggregate.reset();
 assert(QThreadPool::tasks.size()==1);
 QThreadPool::drain();
 assert(detached->complete.load() && detached->cache.storageEmpty());
 std::cout<<"PASS: single retirement slot, hidden clear, rapid replacement, bounded source trim and edge preservation\n";
 std::cout<<"PASS: production aggregate/update methods and budgeted per-sample cursor, source eviction and selection replacement\n";
}
