#include "__HEADER__"
#include <algorithm>
#include <array>
#include <cassert>
#include <cstdint>
#include <deque>
#include <iostream>
#include <string>
#include <unordered_set>
#include <vector>
#define Q_ASSERT(value) assert(value)
struct ProcessDock {
 enum class ProcessActivityMetric { Cpu,Memory,Disk,Network,Gpu };
 struct ProcessPoint { std::string identityKey; double cpuPercent=1,workingSetMB=2,diskMBps=3,netKBps=4,gpuPercent=5; };
 struct Processes {
  inline static unsigned reads=0;
  std::vector<ProcessPoint> rows{{"A"},{"B"}};
  auto begin() const { ++reads; return rows.begin(); }
  auto end() const { return rows.end(); }
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
 f.updateSeriesCache(metrics,{"A","B"},{"A","B"});
 assert(ProcessDock::Processes::reads==10002);
 const auto bothRevision=f.m_rawSamples.revision();
 f.updateSeriesCache(metrics,{"B","A"},{"A","B"});
 assert(ProcessDock::Processes::reads==10002 && f.m_rawSamples.revision()==bothRevision);
 assert(f.m_rawSamples.at(4000).values[4]==10);
 for(int i=0;i<4990;++i) owner.m_activitySamples.pop_front();
 f.updateSeriesCache(metrics,{"B","A"},{"A","B"});
 assert(ProcessDock::Processes::reads==10002 && f.m_rawSamples.size()==12);
 assert(f.m_rawSamples.front().sequence==4989);
 owner.m_activitySamples.clear(); owner.m_activityRecordingStartTick100ns=2;
 ProcessDock::ProcessActivitySample next;next.sequence=0;next.elapsedMs=0;owner.m_activitySamples.push_back(next);
 f.updateSeriesCache(metrics,{},{});
 assert(f.m_rawSamples.size()==1 && f.m_rawSamples.front().sequence==0);
 assert(ProcessDock::Processes::reads==10002);
 assert(f.m_rawSamples.at(0).values[0]==2);
 std::cout<<"PASS: production aggregate/update methods: one-new-sample scan, metric reuse, unordered selection, trim and session reset\n";
}
