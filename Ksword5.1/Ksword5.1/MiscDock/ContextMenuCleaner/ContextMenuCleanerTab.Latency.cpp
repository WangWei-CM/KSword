// ============================================================
// ContextMenuCleanerTab.Latency.cpp
// 作用：
// 1) 逐条测量右键菜单里每一项的"耗时贡献"——Shell 不提供单条耗时，只能用差值法：
//    先测基准（菜单完整弹出耗时），再对每一项临时禁用后复测，基准与复测之差即该项贡献；
// 2) 判定"在算 vs 在等"：采样窗口内 explorer 进程的 CPU 占挂钟比例，高=在算，低=在等外部对象；
// 3) 把结果写回耗时列（超过阈值标红）并把完整报告写进详情编辑器。
//
// 安全约束：
// - 临时禁用走 HKCU 覆盖（处理器改默认值置空、verb 加 LegacyDisable），**绝不删除原有键值**，
//   测完立刻按原值还原；开始测量前还会整份导出当前菜单为文件备份；
// - kProgress 没有取消接口，因此自带 std::atomic 停止标志；
// - 后台线程不触碰任何 Qt 控件，结果统一回到 UI 线程应用。
// ============================================================

#include "ContextMenuCleanerTab.h"
#include "../../UI/AsyncOperation.h"
#include "../../UI/RetainedCleanupJob.h"
#include <QTimer>
#include <exception>

#include "ContextMenuCleanerTab.Internal.h"

#include "../../ksword/window/context_menu_probe.h"
#include "../../theme.h"
#include "../../UI/StructuredFieldView.h"
#include "../../UI/VisibleTableWidget.h"

#include <QBrush>
#include <QColor>
#include <QDateTime>
#include <QFont>
#include <QHash>
#include <QLabel>
#include <QLineEdit>
#include <QMetaObject>
#include <QMessageBox>
#include <QPointer>
#include <QPushButton>
#include <QTableWidget>
#include <QTableWidgetItem>
#include <QTabWidget>
#include <QThreadPool>

#include <algorithm>
#include <array>
#include <vector>

namespace ks::misc
{

using namespace context_menu_cleaner_detail;

namespace
{
    // kMaxProbeItems：逐条阶段最多测多少条（还会被时间预算进一步截断）。
    constexpr int kMaxProbeItems = 24;

    // kBaselineSamples：开局基准采样次数。
    // 用多次采样算出"噪声底噪"（最长 − 最短），低于底噪的差异不能当成某条菜单项的贡献。
    constexpr int kBaselineSamples = 5;

    // kItemBracketSamples / kItemMutedSamples：单项采样次数。
    // 每项固定 3 次弹出：禁用前基准 1 次 → 禁用期间 1 次 → 禁用后基准 1 次。
    // 为什么必须紧邻着取基准（而不是只用一个"全局参考值"）：这台机器的菜单耗时会被
    // "阵发性卡顿"整体抬高约 2.6 秒，而它自己来自己走。若参考值是几分钟前测的，
    // 某个条目恰好在卡顿离开的窗口里被测到，就会凭空"认领"那段卡顿（实测出现过
    // 普通动词被报成 1212 ms，正好等于另一个慢扩展的延迟）。
    // 用紧邻的前后基准取最小值，漂移窗口里算出来的差值天然趋近 0。
    constexpr int kItemBracketSamples = 1;
    constexpr int kItemMutedSamples = 1;

    // kProbeGapMs：采样之间的静默间隔。
    // 取 0：探针内部已经轮询到菜单窗口消失才返回，再死等只是白花时间。
    constexpr int kProbeGapMs = 0;

    // kLatencyShortThresholdMs：门限下限。
    // 贡献低于门限的一律按"很短"显示（不给用户一串无意义的毫秒数），也不计入可归因合计。
    constexpr double kLatencyShortThresholdMs = 50.0;

    // kLatencyBudgetMs：逐条阶段的总时长预算。
    // 按基准测出的"单次菜单开销 × 每项 3 次弹出"反推本次能测几条，避免慢机器上把整轮拖成好几分钟；
    // 被预算砍掉的条数会写进报告，用户也可以随时点「停止测量」。
    constexpr int kLatencyBudgetMs = 90000;

    // resolveExplorerPid：取外壳进程 PID（菜单由它弹出，CPU 归因也看它）。
    // 入参：无；返回：PID，取不到返回 0。
    unsigned long resolveExplorerPid()
    {
        HWND tray = ::FindWindowW(L"Shell_TrayWnd", nullptr);
        if (tray == nullptr)
        {
            return 0;
        }
        DWORD processId = 0;
        ::GetWindowThreadProcessId(tray, &processId);
        return processId;
    }

    // latencyContextForArea：把"是否为桌面背景分类"映射到探针上下文。
    // 入参 isDesktopBackgroundArea：true 表示当前分类是桌面右键菜单（唯一可程序化触发的菜单）；
    //      outContext：输出上下文。
    // 返回：存在可程序化触发的菜单返回 true。
    // 说明：只有"空白处右键"能靠消息触发。文件/格式菜单需要先选中目标项（要写目标进程内存），
    //       IE 已废弃且需 IE 窗口，URL 绑定/打开方式/资源管理器主页根本不是菜单——
    //       这些一律返回 false，由调用方给出明确提示，绝不假装能测。
    // 注意：这里刻意用 bool 而不是 MenuArea —— MenuArea 是类的私有嵌套枚举，自由函数无权使用。
    bool latencyContextForArea(const bool isDesktopBackgroundArea,
        ks::window::MenuProbeContext* outContext)
    {
        if (isDesktopBackgroundArea)
        {
            if (outContext != nullptr)
            {
                *outContext = ks::window::MenuProbeContext::DesktopBackground;
            }
            return true;
        }
        return false;
    }

    // formatMilliseconds：统一的耗时文本。
    // 入参 value：毫秒；返回：保留一位小数的毫秒文本。
    QString formatMilliseconds(const double value)
    {
        return QStringLiteral("%1 ms").arg(value, 0, 'f', 1);
    }

    // verdictText：把探针给出的英文判定转成中文结论。
    // 入参 verdict：computing / waiting / unknown；返回：面向用户的一句结论。
    QString verdictText(const std::string& verdict)
    {
        if (verdict == "computing")
        {
            return QStringLiteral("像是有组件真在计算（CPU 占比高），方向是查菜单扩展");
        }
        if (verdict == "waiting")
        {
            return QStringLiteral("不像扩展在算 —— 更像在等外部对象（IPC / 网络 / 服务超时），逐条禁用扩展意义不大");
        }
        return QStringLiteral("无法判定");
    }
}  // namespace

// 注意：返回类型写在限定名之前，此时还没进入类作用域，必须写成 ContextMenuCleanerTab::MenuArea。
ContextMenuCleanerTab::MenuArea ContextMenuCleanerTab::currentArea() const
{
    // 子页签顺序与 initializeUi 里的建页顺序一致。
    const std::array<MenuArea, 7> ordered{
        MenuArea::InternetExplorer, MenuArea::Desktop, MenuArea::File, MenuArea::UrlBinding,
        MenuArea::OpenWith, MenuArea::FormatMenu, MenuArea::ExplorerHome };

    const int index = m_areaTabWidget != nullptr ? m_areaTabWidget->currentIndex() : 0;
    if (index < 0 || index >= static_cast<int>(ordered.size()))
    {
        return MenuArea::File;
    }
    return ordered[static_cast<std::size_t>(index)];
}

bool ContextMenuCleanerTab::areaSupportsLatencyProbe(const MenuArea area)
{
    ks::window::MenuProbeContext context = ks::window::MenuProbeContext::DesktopBackground;
    return latencyContextForArea(area == MenuArea::Desktop, &context);
}

QVector<ContextMenuCleanerTab::LatencyProbeItem> ContextMenuCleanerTab::collectLatencyProbeItems(
    const MenuArea area, int* truncatedCountOut) const
{
    if (truncatedCountOut != nullptr)
    {
        *truncatedCountOut = 0;
    }

    const AreaWidgets* areaWidgets = widgetsForArea(area);
    QVector<LatencyProbeItem> items;
    if (areaWidgets == nullptr)
    {
        return items;
    }

    // 先按 CLSID 归组：同一个扩展常被注册到多个位置（例如同时挂在 Directory\Background
    // 与 DesktopBackground）。只禁用其中一处时，另一处仍会加载它并照样慢，
    // 差值就会被算成 0 —— 这是"重复注册导致假阴性"的根因，必须整组一起禁用。
    QHash<QString, QStringList> pathsByClsid;
    for (const ContextMenuEntry& entry : areaWidgets->entries)
    {
        if (!entry.clsidText.isEmpty() && !entry.subKeyPath.isEmpty())
        {
            QStringList& paths = pathsByClsid[entry.clsidText.toUpper()];
            if (!paths.contains(entry.subKeyPath))
            {
                paths.append(entry.subKeyPath);
            }
        }
    }

    // 两轮筛选：先收处理器（shellex，最可能在菜单构建时做重活），再收静态动词。
    const auto collectPass = [&](const bool handlerPass)
    {
        for (int index = 0; index < areaWidgets->entries.size(); ++index)
        {
            const ContextMenuEntry& entry = areaWidgets->entries.at(index);
            // 只有 ContextMenuHandlers 里的处理器才用"把默认值（CLSID）置空"来禁用；
            // 其余（静态动词、带 DelegateExecute 的动词、IE MenuExt）都用 LegacyDisable。
            // 注意：不能只看 clsidText 是否为空 —— DelegateExecute 动词也带 CLSID，
            // 若按处理器处理就会"改了默认值但其实没禁用"，那条永远只会测出「很短」（假阴性）。
            const bool isHandler = entry.entryKind.contains(QStringLiteral("shellex"), Qt::CaseInsensitive);
            if (isHandler != handlerPass)
            {
                continue;
            }

            LatencyProbeItem item;
            item.entryIndex = index;
            item.itemName = entry.itemName;
            item.displayName = entry.displayName.isEmpty() ? entry.itemName : entry.displayName;
            item.subKeyPath = entry.subKeyPath;
            item.kind = entry.entryKind;
            item.isHandler = isHandler;
            // 处理器：把同 CLSID 的所有注册路径都带上；动词没有 CLSID，只测它自己。
            if (isHandler && !entry.clsidText.isEmpty())
            {
                item.aliasPaths = pathsByClsid.value(entry.clsidText.toUpper());
            }
            if (item.aliasPaths.isEmpty() && !item.subKeyPath.isEmpty())
            {
                item.aliasPaths.append(item.subKeyPath);
            }
            if (!item.subKeyPath.isEmpty())
            {
                items.push_back(item);
            }
        }
    };
    collectPass(true);
    collectPass(false);

    if (items.size() > kMaxProbeItems)
    {
        if (truncatedCountOut != nullptr)
        {
            *truncatedCountOut = items.size() - kMaxProbeItems;
        }
        items.resize(kMaxProbeItems);
    }
    return items;
}

void ContextMenuCleanerTab::startLatencyMeasurement()
{
    const QPointer<ContextMenuCleanerTab> alive(this);
    settleShellTransaction();if(!alive||shellBusy())return;
    const MenuArea area=currentArea();auto* widgets=widgetsForArea(area);
    if(widgets->tableBuilding)return;
    ks::window::MenuProbeContext ignored=ks::window::MenuProbeContext::DesktopBackground;
    if(!latencyContextForArea(area==MenuArea::Desktop,&ignored))return;
    const unsigned long explorerPid=resolveExplorerPid();
    if(!explorerPid){QMessageBox::warning(this,QStringLiteral("测量菜单耗时"),QStringLiteral("找不到资源管理器进程，无法测量。"));return;}
    int truncatedCount=0;const auto items=collectLatencyProbeItems(area,&truncatedCount);
    if(items.isEmpty()){QMessageBox::information(this,QStringLiteral("测量菜单耗时"),QStringLiteral("当前分类还没有可测条目，请先点「刷新」枚举一次。"));return;}
    const int entryCount=widgets->entries.size();
    if(!beginShellAction())return;
    const auto answer=QMessageBox::question(this,QStringLiteral("测量菜单耗时"),
        QStringLiteral("测量过程会：\n1) 自动导出整份右键菜单注册表备份（防意外）；\n2) 反复短暂弹出桌面右键菜单并自动关闭（不注入鼠标键盘）；\n3) 逐条临时禁用菜单项后复测，随后立即还原。\n\n耗时取决于菜单本身快慢：整轮有总时长预算（约 3 分钟），预算内测不完的条目会列入未测，不再拖长时间。开始吗？"),
        QMessageBox::Yes|QMessageBox::No,QMessageBox::Yes);
    if(!alive)return;
    if(answer!=QMessageBox::Yes){endShellAction();return;}
    auto session=std::make_shared<shell_transaction::LatencySession>();
    const auto serial=m_registrySerial;
    auto cleanup=ks::ui::RetainedCleanupJob::create([session,serial]{
        session->stop.store(true);
        std::lock_guard<std::mutex> lock(*serial);
        shell_transaction::NativePort port;
        return shell_transaction::cleanupLatencySession(port,*session);
    },0,[session]{session->stop.store(true);});
    if(!cleanup){endShellAction();return;}
    m_latencySession=session;m_latencyCleanup=cleanup;m_latencyArea=area;
    m_latencySnapshotGeneration=widgets->refreshGeneration;
    m_latencyRunning.store(true);m_latencyCancelRequested.store(false);
    m_latencyProgressPid=kPro.add(this,"Shell 菜单耗时测量","准备基准测量");
    if(!m_latencyPulseTimer)
    {m_latencyPulseTimer=new QTimer(this);m_latencyPulseTimer->setInterval(80);connect(m_latencyPulseTimer,&QTimer::timeout,this,[this]{pollLatencyProgress();});}
    m_latencyPulseTimer->start();
    submitShellTransaction([area,items,truncatedCount,entryCount,explorerPid,session,cleanup]{
        return runLatencyMeasurement(area,items,truncatedCount,entryCount,explorerPid,session,cleanup);
    },[this,area](const ShellTransactionResult& result){
        // Rejection never ran the worker guard; release that unused reservation once.
        if(m_latencyCleanup&&!m_latencyCleanup->isCleanupRequested())m_latencyCleanup->requestCleanup();
        if(result.latency&&widgetsForArea(area)->refreshGeneration==m_latencySnapshotGeneration)
            applyLatencyResults(area,result.latency->baselineMs,result.latency->contributions,
                result.latency->report,result.latency->measuredEntries);
        else
        {
            m_latencyRunning.store(false);setLatencyUiRunning(false);
            if(m_latencyStatusLabel)m_latencyStatusLabel->setText(result.ok
                ? QStringLiteral("测量已停止，已还原临时注册表。") : result.failureText());
        }
        if(result.latency&&result.latency->cancelled&&!m_latencySession->recoveryPending.load()&&m_latencyStatusLabel)
            m_latencyStatusLabel->setText(QStringLiteral("测量已停止，已还原临时注册表。"));
        if(!result.failures.isEmpty()&&m_latencyStatusLabel)
            m_latencyStatusLabel->setText(QStringLiteral("临时注册表恢复未完成，请重试恢复：%1").arg(result.failures.join('\n')));
        pollLatencyProgress();
    });
}

void ContextMenuCleanerTab::pollLatencyProgress()
{
    const QPointer<ContextMenuCleanerTab> alive(this);
    settleShellTransaction();if(!alive||!m_latencySession)return;
    QVector<QPair<int,double>> singles;QString text;float progress=0;
    {std::lock_guard<std::mutex> lock(m_latencySession->progressMutex);singles.swap(m_latencySession->singles);text=m_latencySession->progressText;progress=m_latencySession->progress;}
    if(m_latencyRunning.load()&&widgetsForArea(m_latencyArea)->refreshGeneration==m_latencySnapshotGeneration)
        for(const auto& value:singles)applySingleLatencyResult(m_latencyArea,value.first,value.second);
    if(m_latencyProgressPid>0&&!text.isEmpty())kPro.set(m_latencyProgressPid,text.toStdString(),0,progress);
    if(m_latencyCleanup&&m_latencyCleanup->hasFailed()&&m_latencyStatusLabel&&!text.isEmpty())m_latencyStatusLabel->setText(text);
    setLatencyUiRunning(m_latencyRunning.load());
    if(!m_latencyRunning.load()&&m_latencyCleanup&&m_latencyCleanup->isComplete())
    {
        if(m_latencyProgressPid>0){kPro.set(m_latencyProgressPid,"测量完成",0,1.0f);m_latencyProgressPid=0;}
        if(m_latencySession->failedInlineRestore.load()&&m_latencyStatusLabel)
            m_latencyStatusLabel->setText(QStringLiteral("临时注册表已恢复；测量结果保留在详情中。"));
        m_latencyPulseTimer->stop();m_latencyCleanup.reset();m_latencySession.reset();
        setLatencyUiRunning(false);
    }
}

ContextMenuCleanerTab::ShellTransactionResult ContextMenuCleanerTab::runLatencyMeasurement(
    const MenuArea area,const QVector<LatencyProbeItem>& items,int truncatedCount,int entryCount,
    unsigned long explorerPid,std::shared_ptr<shell_transaction::LatencySession> session,
    std::shared_ptr<ks::ui::RetainedCleanupJob> cleanup)
{
    struct CleanupOnExit
    {
        std::shared_ptr<ks::ui::RetainedCleanupJob> job;
        ~CleanupOnExit(){job->requestCleanup();}
    } cleanupOnExit{cleanup};
    ShellTransactionResult result;result.stage=ShellTransactionStage::Completed;
    if(session->stop.load()) { result.ok=true;return result; }
    QString backupPath;
    if(!exportContextMenuBackupToFile(&backupPath,&result.error))
    {result.stage=ShellTransactionStage::BackupFailed;return result;}
    result.path=backupPath;
    if(session->stop.load()){result.ok=true;return result;}
    const auto probeContext=ks::window::MenuProbeContext::DesktopBackground;
    shell_transaction::NativePort port;
            // ---- 后台线程：只做测量与数据拼装，不碰任何 Qt 控件 ----
            // 报告刻意保持简短：逐条明细在表格的「耗时」列里，这里只给结论和关键数字。
            ks::ui::FieldDocument report;
            report.note(QStringLiteral("右键菜单耗时测量报告（%1，%2）")
                .arg(QDateTime::currentDateTime().toString(QStringLiteral("yyyy-MM-dd HH:mm:ss")),
                     area == ContextMenuCleanerTab::MenuArea::Desktop
                         ? QStringLiteral("桌面右键菜单") : QStringLiteral("其它分类")));
            if (!backupPath.isEmpty())
            {
                report.field(QStringLiteral("已先备份整份菜单"), QStringLiteral("%1").arg(backupPath));
            }

            // 基准：菜单完整弹出耗时（含显示阶段）+ 进程 CPU 归因。
            const ks::window::MenuProbeMeasurement baseline = ks::window::MeasureMenuOpenLatency(
                probeContext, explorerPid,
                ks::window::MenuProbeOptions{ kBaselineSamples, 8000, kProbeGapMs });

            // 用最小值当基准：外部干扰只会让耗时变长，最小值最接近"这菜单本来要多久"。
            const double baselineMs = baseline.minMs;
            // 门限 = max(50 ms, 本次实测的噪声底噪)。低于门限的差异分不出真假，
            // 一律按「很短」处理 —— 不显示数值，也不计入可归因合计。
            // 这一步直接解决"一片 100 ms+"的问题：那些数字本来就是抖动，不是贡献。
            const double noiseSpreadMs = std::max(0.0, baseline.maxMs - baseline.minMs);
            const double thresholdMs = std::max(kLatencyShortThresholdMs, noiseSpreadMs);

            report.field(QStringLiteral("基准"), QStringLiteral("最短 %1 / 中位 %2 / 最长 %3（弹出 %4 次）")
                .arg(formatMilliseconds(baseline.minMs))
                .arg(formatMilliseconds(baseline.medianMs))
                .arg(formatMilliseconds(baseline.maxMs))
                .arg(baseline.samples.size()));
            if (baseline.processCpuMeasured)
            {
                report.field(QStringLiteral("判定"), QStringLiteral("%1（explorer CPU 占挂钟 %2%）")
                    .arg(verdictText(baseline.verdict))
                    .arg(baseline.cpuRatio * 100.0, 0, 'f', 1));
            }
            else
            {
                report.field(QStringLiteral("判定"), QStringLiteral("%1").arg(verdictText(baseline.verdict)));
            }
            report.field(QStringLiteral("门限"), QStringLiteral("%1（低于它的条目在表格里显示为「很短」）")
                .arg(formatMilliseconds(thresholdMs)));
            if (!baseline.diagnostic.empty())
            {
                report.field(QStringLiteral("注意"), QStringLiteral("%1")
                    .arg(QString::fromStdString(baseline.diagnostic)));
            }

            // ---- 逐条差值：每条固定 3 次弹出（禁用前基准 → 禁用期间 → 禁用后基准）----
            // 为什么必须紧邻取基准：这台机器的菜单会被"阵发性卡顿"整体抬高约 2.6 秒，而它自己来自己走。
            // 曾经为了提速只用一个全局参考值，结果某个普通动词恰好在卡顿离开的窗口里被测到，
            // 凭空认领了 1212 ms（正好等于另一个慢扩展的延迟）。紧邻的前后基准能把这类漂移抵消掉，
            // 而 3 次弹出仍比最初的 4 次（禁用期间测 2 次）更快。
            QVector<double> contributions;
            contributions.resize(entryCount);
            for (int i = 0; i < contributions.size(); ++i)
            {
                contributions[i] = -1.0;
            }

            const int total = items.size();

            // 时间预算：常规条目只弹出 1 次，按基准单次开销反推本次能测几条。
            const double perItemEstimateMs = std::max(1.0, (baselineMs + static_cast<double>(kProbeGapMs))
                * static_cast<double>(kItemBracketSamples * 2 + kItemMutedSamples));
            int plannedItems = static_cast<int>(
                static_cast<double>(kLatencyBudgetMs) / perItemEstimateMs);
            plannedItems = std::clamp(plannedItems, 1, total);

            int significantCount = 0;   // significantCount：确认后有显著贡献的条目数。
            double attributableMs = 0.0; // attributableMs：可归因到具体条目的耗时合计。
            int aliasGroupCount = 0;    // aliasGroupCount：与其它位置共用同一扩展的条目数。
            int unstableCount = 0;      // unstableCount：因前后基准对不上而被判为"不稳定"的条目数。
            QStringList unreverted;
            QStringList probeFailures;

            const auto measureWithItemDisabled = [&](const LatencyProbeItem& item,
                int mutedSamples,double* mutedMsOut,int* disabledPathCountOut,bool* aliasGroupUsedOut) {
                bool disabled=true;int paths=0;
                for(const auto& path:item.aliasPaths)
                {
                    if(session->stop.load()){disabled=false;break;}
                    shell_transaction::TemporaryOverride record;
                    if(!shell_transaction::captureOverride(port,{QStringLiteral("HKCU"),0,path},item.isHandler,record))
                    {probeFailures.push_back(record.error);disabled=false;break;}
                    session->overrides.push_back(std::move(record));
                    session->recoveryPending.store(true);
                    if(!shell_transaction::disableOverride(port,session->overrides.last())){probeFailures.push_back(session->overrides.last().error);disabled=false;break;}
                    ++paths;
                }
                ks::window::MenuProbeMeasurement muted;
                std::exception_ptr failure;
                try
                {
                    if(disabled&&!session->stop.load())muted=ks::window::MeasureMenuOpenLatency(
                        probeContext,explorerPid,ks::window::MenuProbeOptions{mutedSamples,8000,kProbeGapMs});
                }
                catch(...){failure=std::current_exception();}
                if(!shell_transaction::restoreOverrides(port,*session))
                {
                    session->failedInlineRestore.store(true);session->stop.store(true);
                    for(const auto& record:session->overrides)if(record.pending)unreverted.push_back(record.error);
                    disabled=false;
                }
                if(failure)std::rethrow_exception(failure);
                if(session->stop.load()||!disabled||!muted.attempted||muted.samples.empty())return false;
                if(disabledPathCountOut)*disabledPathCountOut=paths;
                if(aliasGroupUsedOut)*aliasGroupUsedOut=paths>1;
                if(mutedMsOut)*mutedMsOut=muted.minMs;
                return true;
            };

            QVector<int> measuredEntries;
            const auto publishResult = [&](int entryIndex,double valueMs) {
                measuredEntries.push_back(entryIndex);
                std::lock_guard<std::mutex> lock(session->progressMutex);
                session->singles.push_back(qMakePair(entryIndex,valueMs));
            };

            for (int index = 0; index < plannedItems; ++index)
            {
                if (session->stop.load()) break;
                const LatencyProbeItem& item = items.at(index);
                {
                    std::lock_guard<std::mutex> lock(session->progressMutex);
                    session->progressText=QStringLiteral("正在测：%1 (%2/%3)")
                        .arg(item.displayName).arg(index+1).arg(plannedItems);
                    session->progress=static_cast<float>(index)/static_cast<float>(plannedItems);
                }

                // A：禁用前的基准（紧邻参照之一）。
                const ks::window::MenuProbeMeasurement before = ks::window::MeasureMenuOpenLatency(
                    probeContext, explorerPid,
                    ks::window::MenuProbeOptions{ kItemBracketSamples, 8000, kProbeGapMs });

                double mutedMs = 0.0;
                int disabledPathCount = 0;
                bool aliasGroupUsed = false;
                if (!measureWithItemDisabled(item, kItemMutedSamples, &mutedMs,
                        &disabledPathCount, &aliasGroupUsed))
                {
                    continue;
                }
                if (aliasGroupUsed)
                {
                    ++aliasGroupCount;
                }

                if(session->stop.load())break; // Do not start another menu probe after accepted stop/close.

                // B：禁用后的基准（紧邻参照之二）。
                const ks::window::MenuProbeMeasurement after = ks::window::MeasureMenuOpenLatency(
                    probeContext, explorerPid,
                    ks::window::MenuProbeOptions{ kItemBracketSamples, 8000, kProbeGapMs });

                if (!before.attempted || before.samples.empty() ||
                    !after.attempted || after.samples.empty())
                {
                    continue;
                }

                // 参照取紧邻的前后两次基准里更快的一次。
                const double itemReferenceMs = std::min(before.minMs, after.minMs);

                // 稳定性判据：前后两次基准自己就对不上，说明这几秒内外部状态变了
                // （这台机器有"阵发性卡顿"，会自己来自己走）。此时这条的差值不可信 ——
                // 宁可标成「不稳定」也不能写一个数字，否则会把"卡顿离开"记到这一条头上
                // （实测出现过普通动词被报成 1212 ms，正好等于另一个慢扩展的延迟）。
                if (std::abs(before.minMs - after.minMs) > thresholdMs)
                {
                    contributions[item.entryIndex] = -1.0;
                    publishResult(item.entryIndex, -1.0);
                    ++unstableCount;
                    continue;
                }

                const double rawContribution = itemReferenceMs - mutedMs;
                contributions[item.entryIndex] = rawContribution > thresholdMs ? rawContribution : 0.0;
                // 测完一条立刻填一条，不等整轮结束。
                publishResult(item.entryIndex, contributions[item.entryIndex]);
            }

            // 汇总：只统计超过门限的条目。
            for (const double value : contributions)
            {
                if (value > 0.0)
                {
                    ++significantCount;
                    attributableMs += value;
                }
            }
            const double residualMs = std::max(0.0, baselineMs - attributableMs);
            const int notPlannedCount = total - measuredEntries.size();

            // 报告只留结论与关键数字；逐条明细在表格「耗时」列里（很短 = 无可测贡献）。
            report.field(QStringLiteral("逐条"), QStringLiteral("共测 %1 项，其中 %2 项有显著贡献，%3 项因状态不稳定跳过，合计 %4")
                .arg(measuredEntries.size()).arg(significantCount).arg(unstableCount)
                .arg(formatMilliseconds(attributableMs)));
            report.field(QStringLiteral("残差"), QStringLiteral("%1（基准里无法归因到单条扩展的部分）")
                .arg(formatMilliseconds(residualMs)));

            report.field(QStringLiteral("结论"), QStringLiteral("%1").arg(
                residualMs > attributableMs
                    ? QStringLiteral("主要不是某一条菜单扩展的问题，优先按上面的判定方向排查；表格里显示「很短」的条目可以直接排除。")
                    : QStringLiteral("耗时主要落在标红的条目上，可优先禁用它们后再测一次。")));
            if (notPlannedCount > 0)
            {
                report.field(QStringLiteral("未测"), QStringLiteral("%1 项（停止、失败或超出本次时间预算）").arg(notPlannedCount));
            }
            if (aliasGroupCount > 0)
            {
                report.field(QStringLiteral("已按 CLSID 归组"), QStringLiteral("%1 条与其它位置共用同一扩展，禁用时一并处理。")
                    .arg(aliasGroupCount));
            }
            if (truncatedCount > 0)
            {
                report.field(QStringLiteral("未测"), QStringLiteral("另有 %1 项（超出单次测量上限）").arg(truncatedCount));
            }
            if (!unreverted.isEmpty())
            {
                report.field(QStringLiteral("警告"), QStringLiteral("%1 项未能还原，请用「恢复备份」兜底")
                    .arg(unreverted.size()));
            }

            if(!probeFailures.isEmpty())report.field(QStringLiteral("跳过项"),probeFailures.join('\n'));
            report.note(QStringLiteral("明细见表格「耗时」列。"));


    result.latency=std::make_shared<LatencyMeasurementResult>();
    result.latency->cancelled=session->stop.load();
    result.latency->baselineMs=baselineMs;
    result.latency->contributions=std::move(contributions);
    result.latency->measuredEntries=std::move(measuredEntries);
    result.latency->report=std::move(report);
    result.ok=unreverted.isEmpty();
    result.failures=unreverted;
    return result;
}

void ContextMenuCleanerTab::cancelLatencyMeasurement()
{
    if(m_latencySession)m_latencySession->stop.store(true);
    const bool retry=m_latencyCleanup&&m_latencyCleanup->hasFailed();
    if(retry)m_latencyCleanup->requestCleanup();
    if(m_latencyStatusLabel)m_latencyStatusLabel->setText(retry?QStringLiteral("正在恢复临时注册表…"):QStringLiteral("正在停止测量（等待当前一项测完）…"));
    setLatencyUiRunning(m_latencyRunning.load());
}

void ContextMenuCleanerTab::applyLatencyResults(
    const MenuArea area,
    const double baselineMs,
    const QVector<double>& perEntryMs,
    const ks::ui::FieldDocument& report, const QVector<int>& measuredEntries)
{
    AreaWidgets* areaWidgets = widgetsForArea(area);
    for(const int index:measuredEntries)
        if(index>=0&&index<perEntryMs.size())applySingleLatencyResult(area,index,perEntryMs.at(index));

    // 报告写进详情编辑器：复用上一轮接好的详情布局系统（四种布局都生效）。
    if (areaWidgets != nullptr && areaWidgets->detailEditor != nullptr)
    {
        areaWidgets->detailEditor->setDocument(report);
    }
    if (m_latencyStatusLabel != nullptr)
    {
        m_latencyStatusLabel->setText(QStringLiteral("测量完成：基准 %1，明细见下方详情区")
            .arg(formatMilliseconds(baselineMs)));
    }

    m_latencyRunning.store(false);
    m_latencyCancelRequested.store(false);
    setLatencyUiRunning(false);
}

void ContextMenuCleanerTab::writeLatencyCell(
    AreaWidgets* widgets, const int row, const double valueMs) const
{
    if (widgets == nullptr || widgets->table == nullptr || row < 0 ||
        row >= widgets->table->rowCount())
    {
        return;
    }

    QTableWidgetItem* latencyItem = widgets->table->item(row, kColumnLatency);
    if (latencyItem == nullptr)
    {
        latencyItem = new QTableWidgetItem();
        widgets->table->setItem(row, kColumnLatency, latencyItem);
    }

    // 低于门限的贡献一律显示「很短」：不给用户一串分不出真假的毫秒数。
    // 负值表示"未测"（时间预算没轮到，或前后基准对不上判定为不稳定）。
    const bool measured = valueMs >= 0.0;
    const bool significant = valueMs > 0.0;
    if (!measured)
    {
        latencyItem->setText(QStringLiteral("不稳定"));
        latencyItem->setToolTip(QStringLiteral(
            "这一条测量期间前后基准对不上（外部状态在变），差值不可信，故不给出数值"));
    }
    else
    {
        latencyItem->setText(significant ? formatMilliseconds(valueMs) : QStringLiteral("很短"));
        latencyItem->setToolTip(significant
            ? QStringLiteral("禁用这一条后整个菜单变快了这么多（超过本次噪声门限，可作参考）")
            : QStringLiteral("贡献低于本次测量的门限（噪声底噪或 50 ms），视为没有可测影响"));
    }

    QFont font = latencyItem->font();
    if (significant)
    {
        // 值得看的条目标红加粗：用主题的语义错误色，浅色/深色主题下都可读。
        latencyItem->setForeground(QBrush(QColor(KswordTheme::ErrorHex())));
        font.setBold(true);
    }
    else
    {
        // 未超门限的用次要文字色。必须用 *ColorHex() 这种"具体 #RRGGBB"访问器：
        // TextSecondaryHex() 返回的是 palette(...) 动态令牌，只适合塞进样式表，
        // 用 QColor 解析会得到无效颜色（工程的主题令牌审计会直接拦下）。
        latencyItem->setForeground(QBrush(QColor(KswordTheme::TextSecondaryColorHex())));
        font.setBold(false);
    }
    latencyItem->setFont(font);
}

void ContextMenuCleanerTab::applySingleLatencyResult(
    const MenuArea area, const int entryIndex, const double valueMs)
{
    // "测完一条填一条"：只按 entries 下标定位那一行，不动其它行、不重建表格。
    AreaWidgets* areaWidgets = widgetsForArea(area);
    if (areaWidgets == nullptr || areaWidgets->table == nullptr || entryIndex < 0)
    {
        return;
    }
    if(entryIndex>=areaWidgets->entryAnchors.size())return;
    auto* anchor=areaWidgets->entryAnchors.at(entryIndex);
    if(anchor)writeLatencyCell(areaWidgets,anchor->row(),valueMs);
}

void ContextMenuCleanerTab::clearLatencyColumn(const MenuArea area)
{
    AreaWidgets* areaWidgets = widgetsForArea(area);
    if (areaWidgets == nullptr || areaWidgets->table == nullptr)
    {
        return;
    }
    const int rowCount = areaWidgets->table->rowCount();
    for (int row = 0; row < rowCount; ++row)
    {
        if (QTableWidgetItem* latencyItem = areaWidgets->table->item(row, kColumnLatency);
            latencyItem != nullptr)
        {
            latencyItem->setText(QString());
            latencyItem->setToolTip(QString());
        }
    }
}

void ContextMenuCleanerTab::setLatencyUiRunning(const bool running)
{
    const bool busy = shellBusy();
    if (m_backupButton) m_backupButton->setEnabled(!busy);
    if (m_restoreBackupButton) m_restoreBackupButton->setEnabled(!busy);
    if (m_latencyButton != nullptr)
    {
        m_latencyButton->setEnabled(!busy && !widgetsForArea(currentArea())->tableBuilding);
    }
    if (m_latencyStopButton != nullptr)
    {
        const bool retry=m_latencyCleanup&&m_latencyCleanup->hasFailed();
        m_latencyStopButton->setText(retry?QStringLiteral("重试恢复"):QStringLiteral("停止测量"));
        m_latencyStopButton->setEnabled(running||retry);
    }

    // 测量期间禁用各分类的刷新/删除/筛选/复制：避免表格被重建，把逐条结果冲掉。
    const std::array<MenuArea, 7> allAreas{
        MenuArea::InternetExplorer, MenuArea::Desktop, MenuArea::File, MenuArea::UrlBinding,
        MenuArea::OpenWith, MenuArea::FormatMenu, MenuArea::ExplorerHome };
    for (const MenuArea area : allAreas)
    {
        AreaWidgets* widgets = widgetsForArea(area);
        if (widgets == nullptr)
        {
            continue;
        }
        if (widgets->refreshButton != nullptr)
        {
            widgets->refreshButton->setEnabled(!busy);
        }
        if (widgets->deleteButton != nullptr)
        {
            widgets->deleteButton->setEnabled(!busy && !widgets->tableBuilding);
        }
        if (widgets->copyButton != nullptr)
        {
            widgets->copyButton->setEnabled(!busy && !widgets->tableBuilding);
        }
        if (widgets->filterEdit != nullptr)
        {
            widgets->filterEdit->setEnabled(!busy);
        }
        if (widgets->restoreButton != nullptr)
        {
            widgets->restoreButton->setEnabled(!busy);
        }
    }
    if (!busy) flushDeferredAreaRefreshes();
}

}  // namespace ks::misc
