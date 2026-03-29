#include "HardwareDock.h"

// ============================================================
// HardwareDock.cpp
// 作用：
// 1) 提供硬件总览与利用率可视化；
// 2) 利用 PDH + Power API 周期采样 CPU/内存/每核频率；
// 3) 显卡与内存模块信息通过 PowerShell/WMI 文本化展示。
// ============================================================

#include "../theme.h"
#include "../UI/CodeEditorWidget.h"

#include <QAbstractItemView>
#include <QDateTime>
#include <QFrame>
#include <QGridLayout>
#include <QHeaderView>
#include <QLabel>
#include <QPainter>
#include <QProcess>
#include <QScrollArea>
#include <QTabWidget>
#include <QTableWidget>
#include <QTableWidgetItem>
#include <QTimer>
#include <QVBoxLayout>

#include <QtCharts/QChart>
#include <QtCharts/QChartView>
#include <QtCharts/QLineSeries>
#include <QtCharts/QValueAxis>

#include <algorithm>
#include <cmath>

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#include <Pdh.h>
#include <PowrProf.h>

#pragma comment(lib, "Pdh.lib")
#pragma comment(lib, "PowrProf.lib")

namespace
{
    // buildStatusColor 作用：
    // - 深浅色模式下返回统一可读的次级文本颜色。
    QColor buildStatusColor()
    {
        return KswordTheme::IsDarkModeEnabled()
            ? QColor(185, 205, 225)
            : QColor(55, 80, 105);
    }

    // bytesToGiBText 作用：
    // - 把字节数转换为 GiB 文本，保留 2 位小数。
    QString bytesToGiBText(const std::uint64_t bytesValue)
    {
        const double gibValue = static_cast<double>(bytesValue) / (1024.0 * 1024.0 * 1024.0);
        return QStringLiteral("%1 GiB").arg(gibValue, 0, 'f', 2);
    }

    // createNoFrameChartView 作用：
    // - 创建无边框 ChartView，统一 Dock 内视觉风格。
    QChartView* createNoFrameChartView(QChart* chart, QWidget* parentWidget)
    {
        QChartView* chartView = new QChartView(chart, parentWidget);
        chartView->setRenderHint(QPainter::Antialiasing, true);
        chartView->setFrameShape(QFrame::NoFrame);
        chartView->setMinimumHeight(120);
        return chartView;
    }
}

HardwareDock::HardwareDock(QWidget* parent)
    : QWidget(parent)
{
    // 构造流程日志：便于定位硬件页初始化失败点。
    kLogEvent event;
    info << event << "[HardwareDock] 构造开始。" << eol;

    initializeUi();
    initializeConnections();
    initializePerformanceCounters();
    refreshStaticHardwareTexts(true);
    refreshAllViews();

    m_refreshTimer = new QTimer(this);
    m_refreshTimer->setInterval(1000);
    connect(m_refreshTimer, &QTimer::timeout, this, [this]() {
        refreshAllViews();
    });
    m_refreshTimer->start();

    info << event << "[HardwareDock] 构造完成。" << eol;
}

HardwareDock::~HardwareDock()
{
    if (m_refreshTimer != nullptr)
    {
        m_refreshTimer->stop();
    }

    if (m_cpuPerfQueryHandle != nullptr)
    {
        ::PdhCloseQuery(reinterpret_cast<PDH_HQUERY>(m_cpuPerfQueryHandle));
        m_cpuPerfQueryHandle = nullptr;
        m_coreCounterHandles.clear();
    }
}

void HardwareDock::initializeUi()
{
    m_rootLayout = new QVBoxLayout(this);
    m_rootLayout->setContentsMargins(4, 4, 4, 4);
    m_rootLayout->setSpacing(6);

    m_sideTabWidget = new QTabWidget(this);
    m_sideTabWidget->setTabPosition(QTabWidget::West);
    m_rootLayout->addWidget(m_sideTabWidget, 1);

    initializeOverviewTab();
    initializeUtilizationTab();
    initializeCpuTab();
    initializeGpuTab();
    initializeMemoryTab();
}

void HardwareDock::initializeOverviewTab()
{
    m_overviewPage = new QWidget(m_sideTabWidget);
    m_overviewLayout = new QVBoxLayout(m_overviewPage);
    m_overviewLayout->setContentsMargins(4, 4, 4, 4);
    m_overviewLayout->setSpacing(6);

    m_overviewSummaryLabel = new QLabel(QStringLiteral("采样中..."), m_overviewPage);
    m_overviewSummaryLabel->setStyleSheet(
        QStringLiteral("color:%1;font-weight:600;").arg(buildStatusColor().name()));
    m_overviewLayout->addWidget(m_overviewSummaryLabel, 0);

    m_overviewEditor = new CodeEditorWidget(m_overviewPage);
    m_overviewEditor->setReadOnly(true);
    m_overviewLayout->addWidget(m_overviewEditor, 1);

    m_sideTabWidget->addTab(m_overviewPage, QStringLiteral("概览"));
}

void HardwareDock::initializeUtilizationTab()
{
    m_utilizationPage = new QWidget(m_sideTabWidget);
    m_utilizationLayout = new QVBoxLayout(m_utilizationPage);
    m_utilizationLayout->setContentsMargins(4, 4, 4, 4);
    m_utilizationLayout->setSpacing(6);

    m_utilizationSummaryLabel = new QLabel(QStringLiteral("CPU/内存采样初始化中..."), m_utilizationPage);
    m_utilizationSummaryLabel->setStyleSheet(
        QStringLiteral("color:%1;font-weight:600;").arg(buildStatusColor().name()));
    m_utilizationLayout->addWidget(m_utilizationSummaryLabel, 0);

    m_coreChartScrollArea = new QScrollArea(m_utilizationPage);
    m_coreChartScrollArea->setWidgetResizable(true);
    m_coreChartHostWidget = new QWidget(m_coreChartScrollArea);
    m_coreChartGridLayout = new QGridLayout(m_coreChartHostWidget);
    m_coreChartGridLayout->setContentsMargins(0, 0, 0, 0);
    m_coreChartGridLayout->setHorizontalSpacing(6);
    m_coreChartGridLayout->setVerticalSpacing(6);
    m_coreChartScrollArea->setWidget(m_coreChartHostWidget);
    m_utilizationLayout->addWidget(m_coreChartScrollArea, 1);

    initializeCoreCharts();
    m_sideTabWidget->addTab(m_utilizationPage, QStringLiteral("利用率"));
}

void HardwareDock::initializeCpuTab()
{
    m_cpuPage = new QWidget(m_sideTabWidget);
    m_cpuLayout = new QVBoxLayout(m_cpuPage);
    m_cpuLayout->setContentsMargins(4, 4, 4, 4);
    m_cpuLayout->setSpacing(6);

    m_cpuDetailLabel = new QLabel(QStringLiteral("温度/电压读取中..."), m_cpuPage);
    m_cpuDetailLabel->setStyleSheet(
        QStringLiteral("color:%1;font-weight:600;").arg(buildStatusColor().name()));
    m_cpuLayout->addWidget(m_cpuDetailLabel, 0);

    m_cpuDetailTable = new QTableWidget(m_cpuPage);
    m_cpuDetailTable->setColumnCount(7);
    m_cpuDetailTable->setHorizontalHeaderLabels({
        QStringLiteral("逻辑处理器"),
        QStringLiteral("利用率(%)"),
        QStringLiteral("当前频率(MHz)"),
        QStringLiteral("最大频率(MHz)"),
        QStringLiteral("限频(MHz)"),
        QStringLiteral("温度"),
        QStringLiteral("电压")
        });
    m_cpuDetailTable->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_cpuDetailTable->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_cpuDetailTable->setSelectionMode(QAbstractItemView::SingleSelection);
    m_cpuDetailTable->horizontalHeader()->setSectionResizeMode(QHeaderView::ResizeToContents);
    m_cpuDetailTable->horizontalHeader()->setSectionResizeMode(0, QHeaderView::Stretch);
    m_cpuDetailTable->horizontalHeader()->setSectionResizeMode(1, QHeaderView::ResizeToContents);
    m_cpuDetailTable->horizontalHeader()->setSectionResizeMode(2, QHeaderView::ResizeToContents);
    m_cpuDetailTable->horizontalHeader()->setSectionResizeMode(3, QHeaderView::ResizeToContents);
    m_cpuDetailTable->horizontalHeader()->setSectionResizeMode(4, QHeaderView::ResizeToContents);
    m_cpuDetailTable->horizontalHeader()->setSectionResizeMode(5, QHeaderView::ResizeToContents);
    m_cpuDetailTable->horizontalHeader()->setSectionResizeMode(6, QHeaderView::ResizeToContents);
    m_cpuLayout->addWidget(m_cpuDetailTable, 1);

    m_sideTabWidget->addTab(m_cpuPage, QStringLiteral("CPU"));
}

void HardwareDock::initializeGpuTab()
{
    m_gpuPage = new QWidget(m_sideTabWidget);
    m_gpuLayout = new QVBoxLayout(m_gpuPage);
    m_gpuLayout->setContentsMargins(4, 4, 4, 4);
    m_gpuLayout->setSpacing(6);

    m_gpuEditor = new CodeEditorWidget(m_gpuPage);
    m_gpuEditor->setReadOnly(true);
    m_gpuLayout->addWidget(m_gpuEditor, 1);

    m_sideTabWidget->addTab(m_gpuPage, QStringLiteral("显卡"));
}

void HardwareDock::initializeMemoryTab()
{
    m_memoryPage = new QWidget(m_sideTabWidget);
    m_memoryLayout = new QVBoxLayout(m_memoryPage);
    m_memoryLayout->setContentsMargins(4, 4, 4, 4);
    m_memoryLayout->setSpacing(6);

    m_memoryEditor = new CodeEditorWidget(m_memoryPage);
    m_memoryEditor->setReadOnly(true);
    m_memoryLayout->addWidget(m_memoryEditor, 1);

    m_sideTabWidget->addTab(m_memoryPage, QStringLiteral("内存"));
}

void HardwareDock::initializeCoreCharts()
{
    const DWORD logicalProcessorCount = std::max<DWORD>(1, ::GetActiveProcessorCount(ALL_PROCESSOR_GROUPS));
    const int coreCount = static_cast<int>(logicalProcessorCount);
    const int columnCount = std::max(1, static_cast<int>(std::ceil(std::sqrt(static_cast<double>(coreCount)))));

    m_coreChartEntries.clear();
    m_coreChartEntries.reserve(coreCount);

    for (int coreIndex = 0; coreIndex < coreCount; ++coreIndex)
    {
        CoreChartEntry chartEntry;
        chartEntry.containerWidget = new QWidget(m_coreChartHostWidget);
        QVBoxLayout* containerLayout = new QVBoxLayout(chartEntry.containerWidget);
        containerLayout->setContentsMargins(4, 4, 4, 4);
        containerLayout->setSpacing(2);

        chartEntry.titleLabel = new QLabel(
            QStringLiteral("CPU %1").arg(coreIndex),
            chartEntry.containerWidget);
        chartEntry.titleLabel->setStyleSheet(
            QStringLiteral("color:%1;font-weight:600;").arg(buildStatusColor().name()));
        containerLayout->addWidget(chartEntry.titleLabel, 0);

        chartEntry.lineSeries = new QLineSeries(chartEntry.containerWidget);
        chartEntry.lineSeries->setColor(QColor(KswordTheme::PrimaryBlueHex));
        for (int indexValue = 0; indexValue < m_historyLength; ++indexValue)
        {
            chartEntry.lineSeries->append(indexValue, 0.0);
        }

        QChart* chart = new QChart();
        chart->addSeries(chartEntry.lineSeries);
        chart->legend()->hide();
        chart->setBackgroundVisible(false);
        chart->setBackgroundRoundness(0);
        chart->setMargins(QMargins(0, 0, 0, 0));

        chartEntry.axisX = new QValueAxis(chart);
        chartEntry.axisX->setRange(0, m_historyLength - 1);
        chartEntry.axisX->setLabelsVisible(false);
        chartEntry.axisX->setGridLineVisible(false);
        chartEntry.axisX->setMinorGridLineVisible(false);

        chartEntry.axisY = new QValueAxis(chart);
        chartEntry.axisY->setRange(0.0, 100.0);
        chartEntry.axisY->setLabelsVisible(false);
        chartEntry.axisY->setGridLineVisible(false);
        chartEntry.axisY->setMinorGridLineVisible(false);

        chart->addAxis(chartEntry.axisX, Qt::AlignBottom);
        chart->addAxis(chartEntry.axisY, Qt::AlignLeft);
        chartEntry.lineSeries->attachAxis(chartEntry.axisX);
        chartEntry.lineSeries->attachAxis(chartEntry.axisY);

        chartEntry.chartView = createNoFrameChartView(chart, chartEntry.containerWidget);
        containerLayout->addWidget(chartEntry.chartView, 1);

        const int rowIndex = coreIndex / columnCount;
        const int columnIndex = coreIndex % columnCount;
        m_coreChartGridLayout->addWidget(chartEntry.containerWidget, rowIndex, columnIndex);
        m_coreChartEntries.push_back(chartEntry);
    }
}

void HardwareDock::initializeConnections()
{
    // 暂无额外交互按钮，预留函数用于后续扩展。
}

void HardwareDock::initializePerformanceCounters()
{
    if (m_cpuPerfQueryHandle != nullptr)
    {
        return;
    }

    PDH_HQUERY queryHandle = nullptr;
    const PDH_STATUS queryStatus = ::PdhOpenQueryW(nullptr, 0, &queryHandle);
    if (queryStatus != ERROR_SUCCESS || queryHandle == nullptr)
    {
        kLogEvent event;
        warn << event
            << "[HardwareDock] 初始化PDH失败：PdhOpenQueryW, status="
            << queryStatus
            << eol;
        return;
    }

    m_cpuPerfQueryHandle = queryHandle;
    m_coreCounterHandles.clear();
    m_coreCounterHandles.reserve(m_coreChartEntries.size());

    for (int coreIndex = 0; coreIndex < static_cast<int>(m_coreChartEntries.size()); ++coreIndex)
    {
        const QString counterPath = QStringLiteral("\\Processor(%1)\\% Processor Time").arg(coreIndex);
        PDH_HCOUNTER counterHandle = nullptr;
        const PDH_STATUS addStatus = ::PdhAddEnglishCounterW(
            queryHandle,
            reinterpret_cast<LPCWSTR>(counterPath.utf16()),
            0,
            &counterHandle);
        if (addStatus != ERROR_SUCCESS || counterHandle == nullptr)
        {
            // 某个核心计数器失败时占位 nullptr，后续采样按 0 处理。
            m_coreCounterHandles.push_back(nullptr);
            continue;
        }
        m_coreCounterHandles.push_back(counterHandle);
    }

    ::PdhCollectQueryData(queryHandle);
}

void HardwareDock::refreshAllViews()
{
    std::vector<double> coreUsageList;
    coreUsageList.reserve(m_coreChartEntries.size());
    double totalCpuUsage = 0.0;
    if (!samplePerCoreUsage(&coreUsageList, &totalCpuUsage))
    {
        coreUsageList.assign(m_coreChartEntries.size(), 0.0);
        totalCpuUsage = 0.0;
    }

    double memoryUsagePercent = 0.0;
    sampleMemoryUsage(&memoryUsagePercent);

    std::vector<CpuPowerSnapshot> powerInfoList;
    sampleCpuPowerInfo(&powerInfoList);

    ++m_sampleCounter;
    updateOverviewText(totalCpuUsage, memoryUsagePercent);
    updateUtilizationView(coreUsageList, memoryUsagePercent);
    updateCpuDetailTable(coreUsageList, powerInfoList);

    ++m_refreshRound;
    Q_UNUSED(m_refreshRound);
    refreshStaticHardwareTexts(false);
}

bool HardwareDock::samplePerCoreUsage(
    std::vector<double>* coreUsageOut,
    double* totalUsageOut)
{
    if (coreUsageOut == nullptr || totalUsageOut == nullptr)
    {
        return false;
    }
    if (m_cpuPerfQueryHandle == nullptr)
    {
        initializePerformanceCounters();
    }
    if (m_cpuPerfQueryHandle == nullptr)
    {
        return false;
    }

    const PDH_HQUERY queryHandle = reinterpret_cast<PDH_HQUERY>(m_cpuPerfQueryHandle);
    const PDH_STATUS collectStatus = ::PdhCollectQueryData(queryHandle);
    if (collectStatus != ERROR_SUCCESS)
    {
        return false;
    }

    coreUsageOut->clear();
    coreUsageOut->reserve(m_coreCounterHandles.size());
    double usageSum = 0.0;
    int validCount = 0;

    for (void* counterHandleVoid : m_coreCounterHandles)
    {
        if (counterHandleVoid == nullptr)
        {
            coreUsageOut->push_back(0.0);
            continue;
        }

        PDH_FMT_COUNTERVALUE formattedValue{};
        const PDH_STATUS readStatus = ::PdhGetFormattedCounterValue(
            reinterpret_cast<PDH_HCOUNTER>(counterHandleVoid),
            PDH_FMT_DOUBLE,
            nullptr,
            &formattedValue);
        if (readStatus != ERROR_SUCCESS)
        {
            coreUsageOut->push_back(0.0);
            continue;
        }

        const double usageValue = std::clamp(formattedValue.doubleValue, 0.0, 100.0);
        coreUsageOut->push_back(usageValue);
        usageSum += usageValue;
        ++validCount;
    }

    *totalUsageOut = validCount > 0 ? (usageSum / static_cast<double>(validCount)) : 0.0;
    return true;
}

bool HardwareDock::sampleCpuPowerInfo(std::vector<CpuPowerSnapshot>* powerInfoOut)
{
    if (powerInfoOut == nullptr)
    {
        return false;
    }

    const ULONG logicalProcessorCount = std::max<ULONG>(
        1,
        static_cast<ULONG>(m_coreChartEntries.size()));
    // KsProcessorPowerInformation 用途：
    // - 与 CallNtPowerInformation(ProcessorInformation) 输出结构保持二进制兼容；
    // - 避免不同 SDK 版本缺少 PROCESSOR_POWER_INFORMATION 定义导致编译失败。
    struct KsProcessorPowerInformation
    {
        ULONG Number;            // Number：逻辑处理器编号。
        ULONG MaxMhz;            // MaxMhz：最大频率。
        ULONG CurrentMhz;        // CurrentMhz：当前频率。
        ULONG MhzLimit;          // MhzLimit：限频上限。
        ULONG MaxIdleState;      // MaxIdleState：最大空闲状态。
        ULONG CurrentIdleState;  // CurrentIdleState：当前空闲状态。
    };
    std::vector<KsProcessorPowerInformation> nativeInfoList(logicalProcessorCount);

    const NTSTATUS ntStatus = ::CallNtPowerInformation(
        ProcessorInformation,
        nullptr,
        0,
        nativeInfoList.data(),
        static_cast<ULONG>(nativeInfoList.size() * sizeof(KsProcessorPowerInformation)));
    if (ntStatus != 0)
    {
        return false;
    }

    powerInfoOut->clear();
    powerInfoOut->reserve(nativeInfoList.size());
    for (const KsProcessorPowerInformation& nativeInfo : nativeInfoList)
    {
        CpuPowerSnapshot snapshot;
        snapshot.coreIndex = nativeInfo.Number;
        snapshot.currentMhz = nativeInfo.CurrentMhz;
        snapshot.maxMhz = nativeInfo.MaxMhz;
        snapshot.limitMhz = nativeInfo.MhzLimit;
        powerInfoOut->push_back(snapshot);
    }
    return true;
}

bool HardwareDock::sampleMemoryUsage(double* memoryUsagePercentOut)
{
    if (memoryUsagePercentOut == nullptr)
    {
        return false;
    }

    MEMORYSTATUSEX memoryStatus{};
    memoryStatus.dwLength = sizeof(memoryStatus);
    if (::GlobalMemoryStatusEx(&memoryStatus) == FALSE)
    {
        *memoryUsagePercentOut = 0.0;
        return false;
    }

    *memoryUsagePercentOut = static_cast<double>(memoryStatus.dwMemoryLoad);
    return true;
}

void HardwareDock::updateOverviewText(const double cpuUsagePercent, const double memoryUsagePercent)
{
    if (m_overviewSummaryLabel == nullptr)
    {
        return;
    }

    MEMORYSTATUSEX memoryStatus{};
    memoryStatus.dwLength = sizeof(memoryStatus);
    ::GlobalMemoryStatusEx(&memoryStatus);

    const QString summaryText = QStringLiteral(
        "CPU总体利用率: %1%    内存利用率: %2%    可用内存: %3 / 总内存: %4")
        .arg(cpuUsagePercent, 0, 'f', 1)
        .arg(memoryUsagePercent, 0, 'f', 1)
        .arg(bytesToGiBText(memoryStatus.ullAvailPhys))
        .arg(bytesToGiBText(memoryStatus.ullTotalPhys));
    m_overviewSummaryLabel->setText(summaryText);
}

void HardwareDock::updateUtilizationView(
    const std::vector<double>& coreUsageList,
    const double memoryUsagePercent)
{
    if (m_utilizationSummaryLabel != nullptr)
    {
        double averageCpuUsage = 0.0;
        if (!coreUsageList.empty())
        {
            for (double usageValue : coreUsageList)
            {
                averageCpuUsage += usageValue;
            }
            averageCpuUsage /= static_cast<double>(coreUsageList.size());
        }

        m_utilizationSummaryLabel->setText(
            QStringLiteral("CPU总体: %1%    内存: %2%    逻辑处理器: %3")
            .arg(averageCpuUsage, 0, 'f', 1)
            .arg(memoryUsagePercent, 0, 'f', 1)
            .arg(coreUsageList.size()));
    }

    const int chartCount = std::min(
        static_cast<int>(m_coreChartEntries.size()),
        static_cast<int>(coreUsageList.size()));
    for (int indexValue = 0; indexValue < chartCount; ++indexValue)
    {
        CoreChartEntry& chartEntry = m_coreChartEntries[static_cast<std::size_t>(indexValue)];
        const double usageValue = coreUsageList[static_cast<std::size_t>(indexValue)];
        chartEntry.titleLabel->setText(
            QStringLiteral("CPU %1  %2%")
            .arg(indexValue)
            .arg(usageValue, 0, 'f', 1));
        appendCoreSeriesPoint(chartEntry, usageValue);
    }
}

void HardwareDock::updateCpuDetailTable(
    const std::vector<double>& coreUsageList,
    const std::vector<CpuPowerSnapshot>& powerInfoList)
{
    if (m_cpuDetailTable == nullptr)
    {
        return;
    }

    const int rowCount = std::max(
        static_cast<int>(coreUsageList.size()),
        static_cast<int>(powerInfoList.size()));
    m_cpuDetailTable->setRowCount(rowCount);

    const QString sensorText = buildCpuSensorText(false);
    QString temperatureText = QStringLiteral("N/A");
    QString voltageText = QStringLiteral("N/A");
    const QStringList sensorParts = sensorText.split('|');
    if (sensorParts.size() >= 2)
    {
        temperatureText = sensorParts.at(0);
        voltageText = sensorParts.at(1);
    }

    for (int rowIndex = 0; rowIndex < rowCount; ++rowIndex)
    {
        const QString coreName = QStringLiteral("CPU %1").arg(rowIndex);
        const QString usageText = rowIndex < static_cast<int>(coreUsageList.size())
            ? QString::number(coreUsageList[static_cast<std::size_t>(rowIndex)], 'f', 1)
            : QStringLiteral("0.0");

        QString currentMhzText = QStringLiteral("N/A");
        QString maxMhzText = QStringLiteral("N/A");
        QString limitMhzText = QStringLiteral("N/A");
        if (rowIndex < static_cast<int>(powerInfoList.size()))
        {
            const CpuPowerSnapshot& snapshot = powerInfoList[static_cast<std::size_t>(rowIndex)];
            currentMhzText = QString::number(snapshot.currentMhz);
            maxMhzText = QString::number(snapshot.maxMhz);
            limitMhzText = QString::number(snapshot.limitMhz);
        }

        m_cpuDetailTable->setItem(rowIndex, 0, new QTableWidgetItem(coreName));
        m_cpuDetailTable->setItem(rowIndex, 1, new QTableWidgetItem(usageText));
        m_cpuDetailTable->setItem(rowIndex, 2, new QTableWidgetItem(currentMhzText));
        m_cpuDetailTable->setItem(rowIndex, 3, new QTableWidgetItem(maxMhzText));
        m_cpuDetailTable->setItem(rowIndex, 4, new QTableWidgetItem(limitMhzText));
        m_cpuDetailTable->setItem(rowIndex, 5, new QTableWidgetItem(temperatureText));
        m_cpuDetailTable->setItem(rowIndex, 6, new QTableWidgetItem(voltageText));
    }

    if (m_cpuDetailLabel != nullptr)
    {
        m_cpuDetailLabel->setText(
            QStringLiteral("CPU传感器：温度=%1，电压=%2（不可读时显示N/A）")
            .arg(temperatureText)
            .arg(voltageText));
    }
}

void HardwareDock::appendCoreSeriesPoint(CoreChartEntry& chartEntry, const double usagePercent)
{
    if (chartEntry.lineSeries == nullptr || chartEntry.axisX == nullptr || chartEntry.axisY == nullptr)
    {
        return;
    }

    chartEntry.lineSeries->append(m_sampleCounter, usagePercent);
    while (chartEntry.lineSeries->count() > m_historyLength)
    {
        chartEntry.lineSeries->remove(0);
    }

    const QList<QPointF> pointList = chartEntry.lineSeries->points();
    if (!pointList.isEmpty())
    {
        chartEntry.axisX->setRange(pointList.first().x(), pointList.last().x());
    }
    chartEntry.axisY->setRange(0.0, 100.0);
}

void HardwareDock::refreshStaticHardwareTexts(const bool forceRefresh)
{
    if (forceRefresh)
    {
        m_cachedOverviewStaticText = buildOverviewStaticText();
        m_cachedGpuStaticText = buildGpuStaticText();
        m_cachedMemoryStaticText = buildMemoryStaticText();
    }

    if (m_overviewEditor != nullptr && !m_cachedOverviewStaticText.isEmpty())
    {
        m_overviewEditor->setText(m_cachedOverviewStaticText);
    }
    if (m_gpuEditor != nullptr && !m_cachedGpuStaticText.isEmpty())
    {
        m_gpuEditor->setText(m_cachedGpuStaticText);
    }
    if (m_memoryEditor != nullptr && !m_cachedMemoryStaticText.isEmpty())
    {
        m_memoryEditor->setText(m_cachedMemoryStaticText);
    }
}

QString HardwareDock::queryPowerShellText(const QString& scriptText, const int timeoutMs) const
{
    QProcess process;
    process.setProgram(QStringLiteral("powershell.exe"));
    process.setArguments({
        QStringLiteral("-NoProfile"),
        QStringLiteral("-ExecutionPolicy"),
        QStringLiteral("Bypass"),
        QStringLiteral("-Command"),
        scriptText
        });
    process.start();
    process.waitForFinished(timeoutMs);

    const QString standardOutput = QString::fromLocal8Bit(process.readAllStandardOutput()).trimmed();
    const QString standardError = QString::fromLocal8Bit(process.readAllStandardError()).trimmed();
    if (process.exitStatus() != QProcess::NormalExit || process.exitCode() != 0)
    {
        return QStringLiteral("PowerShell执行失败。\nExitCode=%1\nError=%2")
            .arg(process.exitCode())
            .arg(standardError.isEmpty() ? QStringLiteral("<空>") : standardError);
    }
    if (standardOutput.isEmpty())
    {
        return QStringLiteral("<无输出>");
    }
    return standardOutput;
}

QString HardwareDock::buildOverviewStaticText() const
{
    SYSTEM_INFO systemInfo{};
    ::GetSystemInfo(&systemInfo);

    MEMORYSTATUSEX memoryStatus{};
    memoryStatus.dwLength = sizeof(memoryStatus);
    ::GlobalMemoryStatusEx(&memoryStatus);

    QString text;
    text += QStringLiteral("系统名称: %1\n").arg(QSysInfo::prettyProductName());
    text += QStringLiteral("CPU架构: %1\n").arg(QSysInfo::currentCpuArchitecture());
    text += QStringLiteral("内核类型: %1\n").arg(QSysInfo::kernelType());
    text += QStringLiteral("内核版本: %1\n").arg(QSysInfo::kernelVersion());
    text += QStringLiteral("逻辑处理器数量: %1\n").arg(::GetActiveProcessorCount(ALL_PROCESSOR_GROUPS));
    text += QStringLiteral("处理器组掩码(十六进制): 0x%1\n")
        .arg(QString::number(static_cast<qulonglong>(systemInfo.dwActiveProcessorMask), 16).toUpper());
    text += QStringLiteral("页面大小: %1 字节\n").arg(systemInfo.dwPageSize);
    text += QStringLiteral("物理内存总量: %1\n").arg(bytesToGiBText(memoryStatus.ullTotalPhys));
    text += QStringLiteral("当前可用内存: %1\n").arg(bytesToGiBText(memoryStatus.ullAvailPhys));
    text += QStringLiteral("虚拟内存总量: %1\n").arg(bytesToGiBText(memoryStatus.ullTotalVirtual));
    text += QStringLiteral("当前可用虚拟内存: %1\n").arg(bytesToGiBText(memoryStatus.ullAvailVirtual));
    text += QStringLiteral("系统启动时间: %1\n")
        .arg(QDateTime::fromMSecsSinceEpoch(
            QDateTime::currentMSecsSinceEpoch() - static_cast<qint64>(::GetTickCount64()))
            .toString(QStringLiteral("yyyy-MM-dd HH:mm:ss")));
    return text;
}

QString HardwareDock::buildGpuStaticText() const
{
    const QString scriptText = QStringLiteral(
        "$list=Get-CimInstance Win32_VideoController | "
        "Select-Object Name,AdapterRAM,DriverVersion,VideoProcessor,CurrentHorizontalResolution,CurrentVerticalResolution,CurrentRefreshRate; "
        "if($null -eq $list){'未读取到显卡信息'} else {$list | Format-Table -AutoSize | Out-String}");
    return queryPowerShellText(scriptText, 5000);
}

QString HardwareDock::buildMemoryStaticText() const
{
    const QString scriptText = QStringLiteral(
        "$phy=Get-CimInstance Win32_PhysicalMemory | "
        "Select-Object BankLabel,Manufacturer,PartNumber,ConfiguredClockSpeed,Capacity,SMBIOSMemoryType; "
        "$os=Get-CimInstance Win32_OperatingSystem | Select-Object TotalVisibleMemorySize,FreePhysicalMemory; "
        "'[物理内存条]';"
        "$phy | Format-Table -AutoSize | Out-String; "
        "'[操作系统内存]';"
        "$os | Format-List | Out-String");
    return queryPowerShellText(scriptText, 6000);
}

QString HardwareDock::buildCpuSensorText(const bool forceRefresh)
{
    // 传感器读取代价较高，默认只在首次读取或显式强制刷新时执行。
    if (!forceRefresh && !m_cachedSensorText.isEmpty())
    {
        return m_cachedSensorText;
    }

    const QString temperatureScript = QStringLiteral(
        "$v=Get-CimInstance -Namespace root/wmi -ClassName MSAcpi_ThermalZoneTemperature -ErrorAction SilentlyContinue | "
        "Select-Object -First 1 -ExpandProperty CurrentTemperature; "
        "if($null -eq $v){'N/A'}else{([math]::Round(($v/10)-273.15,1)).ToString() + '°C'}");
    const QString voltageScript = QStringLiteral(
        "$v=Get-CimInstance Win32_Processor | Select-Object -First 1 -ExpandProperty CurrentVoltage; "
        "if($null -eq $v -or $v -eq 0){'N/A'}else{([math]::Round($v*0.1,2)).ToString() + 'V'}");

    const QString temperatureText = queryPowerShellText(temperatureScript, 3500).split('\n').value(0).trimmed();
    const QString voltageText = queryPowerShellText(voltageScript, 3500).split('\n').value(0).trimmed();
    m_cachedSensorText = QStringLiteral("%1|%2")
        .arg(temperatureText.isEmpty() ? QStringLiteral("N/A") : temperatureText)
        .arg(voltageText.isEmpty() ? QStringLiteral("N/A") : voltageText);
    return m_cachedSensorText;
}
