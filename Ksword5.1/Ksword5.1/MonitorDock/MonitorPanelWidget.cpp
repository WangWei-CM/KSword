#include "MonitorPanelWidget.h"

// ============================================================
// MonitorPanelWidget.cpp
// 作用：
// 1) 实现“监视面板”四宫格性能图（CPU/内存/磁盘/网络）；
// 2) CPU 使用每个逻辑核心单独柱状条展示；
// 3) 采样逻辑与 MonitorDock 解耦，避免 WMI/ETW 页面臃肿。
// ============================================================

#include "../theme.h"

#include <QDateTime>
#include <QFrame>
#include <QGridLayout>
#include <QPainter>
#include <QTimer>
#include <QVBoxLayout>

#include <QtCharts/QBarCategoryAxis>
#include <QtCharts/QBarSeries>
#include <QtCharts/QBarSet>
#include <QtCharts/QAreaSeries>
#include <QtCharts/QChart>
#include <QtCharts/QChartView>
#include <QtCharts/QLineSeries>
#include <QtCharts/QValueAxis>

#include <algorithm>
#include <cstddef>

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#include <Pdh.h>
#include <iphlpapi.h>
#include <netioapi.h>

#pragma comment(lib, "Pdh.lib")
#pragma comment(lib, "Iphlpapi.lib")

namespace
{
    // bytesPerSecondToText 作用：
    // - 把字节每秒速率转换成人类可读文本；
    // - 用于折线图标题中展示“读/写、上/下行”的即时速率。
    QString bytesPerSecondToText(const double bytesPerSecondValue)
    {
        const double safeValue = std::max(0.0, bytesPerSecondValue);
        if (safeValue < 1024.0)
        {
            return QStringLiteral("%1 B/s").arg(safeValue, 0, 'f', 1);
        }
        if (safeValue < 1024.0 * 1024.0)
        {
            return QStringLiteral("%1 KB/s").arg(safeValue / 1024.0, 0, 'f', 1);
        }
        if (safeValue < 1024.0 * 1024.0 * 1024.0)
        {
            return QStringLiteral("%1 MB/s").arg(safeValue / (1024.0 * 1024.0), 0, 'f', 2);
        }
        return QStringLiteral("%1 GB/s").arg(safeValue / (1024.0 * 1024.0 * 1024.0), 0, 'f', 2);
    }

    // createNoFrameChartView 作用：
    // - 创建无边框、抗锯齿图表视图；
    // - 统一监视面板视觉风格，避免默认方框边缘过重。
    QChartView* createNoFrameChartView(QChart* chartPtr, QWidget* parentWidget)
    {
        QChartView* chartView = new QChartView(chartPtr, parentWidget);
        chartView->setRenderHint(QPainter::Antialiasing, true);
        chartView->setFrameShape(QFrame::NoFrame);
        chartView->setMinimumHeight(140);
        return chartView;
    }

    // cpuBarColor 作用：返回 CPU 每核柱状图主色。
    QColor cpuBarColor()
    {
        return KswordTheme::IsDarkModeEnabled()
            ? QColor(96, 172, 255)
            : QColor(67, 160, 255);
    }

    // memoryBarColor 作用：返回内存柱状图主色。
    QColor memoryBarColor()
    {
        return KswordTheme::IsDarkModeEnabled()
            ? QColor(130, 218, 170)
            : QColor(70, 185, 120);
    }
}

MonitorPanelWidget::MonitorPanelWidget(QWidget* parent)
    : QWidget(parent)
{
    // 构造日志使用同一个 kLogEvent，方便按 GUID 追踪整个初始化流程。
    kLogEvent event;
    info << event << "[MonitorPanelWidget] 构造开始。" << eol;

    initializeUi();
    initializeCounters();

    // 采样定时器：
    // - 每秒更新一次性能图；
    // - 使用 UI 线程定时器，避免与图表对象跨线程访问。
    m_refreshTimer = new QTimer(this);
    m_refreshTimer->setInterval(1000);
    connect(m_refreshTimer, &QTimer::timeout, this, [this]() {
        refreshMetrics();
    });
    m_refreshTimer->start();

    refreshMetrics();
    info << event << "[MonitorPanelWidget] 构造完成。" << eol;
}

MonitorPanelWidget::~MonitorPanelWidget()
{
    if (m_refreshTimer != nullptr)
    {
        m_refreshTimer->stop();
    }

    // 释放 CPU 查询句柄与每核计数器句柄。
    if (m_cpuPerfQueryHandle != nullptr)
    {
        ::PdhCloseQuery(reinterpret_cast<PDH_HQUERY>(m_cpuPerfQueryHandle));
        m_cpuPerfQueryHandle = nullptr;
        m_coreCounterHandles.clear();
    }

    // 释放磁盘查询句柄与读写计数器句柄。
    if (m_diskPerfQueryHandle != nullptr)
    {
        ::PdhCloseQuery(reinterpret_cast<PDH_HQUERY>(m_diskPerfQueryHandle));
        m_diskPerfQueryHandle = nullptr;
        m_diskReadCounterHandle = nullptr;
        m_diskWriteCounterHandle = nullptr;
    }
}

void MonitorPanelWidget::initializeUi()
{
    // 根布局：
    // - 四宫格图表占满整个监视面板；
    // - 间距统一为 6，和全项目 Dock 风格一致。
    m_rootLayout = new QVBoxLayout(this);
    m_rootLayout->setContentsMargins(4, 4, 4, 4);
    m_rootLayout->setSpacing(6);

    m_chartGridLayout = new QGridLayout();
    m_chartGridLayout->setContentsMargins(0, 0, 0, 0);
    m_chartGridLayout->setHorizontalSpacing(6);
    m_chartGridLayout->setVerticalSpacing(6);
    m_rootLayout->addLayout(m_chartGridLayout, 1);

    // ===================== CPU 每核柱状图 =====================
    const int logicalCoreCount = static_cast<int>(
        std::max<DWORD>(1, ::GetActiveProcessorCount(ALL_PROCESSOR_GROUPS)));
    QStringList cpuCategoryTextList;
    cpuCategoryTextList.reserve(logicalCoreCount);

    m_cpuCoreBarSet = new QBarSet(QStringLiteral("CPU Core Usage"));
    m_cpuCoreBarSet->setColor(cpuBarColor());
    m_cpuCoreBarSet->setBorderColor(cpuBarColor());

    for (int coreIndex = 0; coreIndex < logicalCoreCount; ++coreIndex)
    {
        m_cpuCoreBarSet->append(0.0);
        cpuCategoryTextList.push_back(QString::number(coreIndex));
    }

    QBarSeries* cpuBarSeries = new QBarSeries(this);
    cpuBarSeries->append(m_cpuCoreBarSet);

    QChart* cpuChart = new QChart();
    cpuChart->addSeries(cpuBarSeries);
    cpuChart->legend()->hide();
    cpuChart->setBackgroundVisible(false);
    cpuChart->setBackgroundRoundness(0);
    cpuChart->setMargins(QMargins(0, 0, 0, 0));
    cpuChart->setTitle(QStringLiteral("CPU 每核心利用率"));

    QBarCategoryAxis* cpuAxisX = new QBarCategoryAxis(cpuChart);
    cpuAxisX->append(cpuCategoryTextList);
    cpuAxisX->setGridLineVisible(false);
    cpuAxisX->setLabelsVisible(logicalCoreCount <= 24);

    QValueAxis* cpuAxisY = new QValueAxis(cpuChart);
    cpuAxisY->setRange(0.0, 100.0);
    cpuAxisY->setLabelsVisible(false);
    cpuAxisY->setGridLineVisible(false);
    cpuAxisY->setMinorGridLineVisible(false);

    cpuChart->addAxis(cpuAxisX, Qt::AlignBottom);
    cpuChart->addAxis(cpuAxisY, Qt::AlignLeft);
    cpuBarSeries->attachAxis(cpuAxisX);
    cpuBarSeries->attachAxis(cpuAxisY);
    m_cpuChartView = createNoFrameChartView(cpuChart, this);

    // ===================== 内存柱状图 =====================
    m_memoryBarSet = new QBarSet(QStringLiteral("Memory Usage"));
    m_memoryBarSet->append(0.0);
    m_memoryBarSet->setColor(memoryBarColor());
    m_memoryBarSet->setBorderColor(memoryBarColor());

    QBarSeries* memoryBarSeries = new QBarSeries(this);
    memoryBarSeries->append(m_memoryBarSet);

    QChart* memoryChart = new QChart();
    memoryChart->addSeries(memoryBarSeries);
    memoryChart->legend()->hide();
    memoryChart->setBackgroundVisible(false);
    memoryChart->setBackgroundRoundness(0);
    memoryChart->setMargins(QMargins(0, 0, 0, 0));
    memoryChart->setTitle(QStringLiteral("内存利用率"));

    QBarCategoryAxis* memoryAxisX = new QBarCategoryAxis(memoryChart);
    memoryAxisX->append(QStringList{ QStringLiteral("当前") });
    memoryAxisX->setLabelsVisible(false);
    memoryAxisX->setGridLineVisible(false);

    QValueAxis* memoryAxisY = new QValueAxis(memoryChart);
    memoryAxisY->setRange(0.0, 100.0);
    memoryAxisY->setLabelsVisible(false);
    memoryAxisY->setGridLineVisible(false);
    memoryAxisY->setMinorGridLineVisible(false);

    memoryChart->addAxis(memoryAxisX, Qt::AlignBottom);
    memoryChart->addAxis(memoryAxisY, Qt::AlignLeft);
    memoryBarSeries->attachAxis(memoryAxisX);
    memoryBarSeries->attachAxis(memoryAxisY);
    m_memoryChartView = createNoFrameChartView(memoryChart, this);

    // ===================== 折线图创建器（磁盘/网络复用） =====================
    auto createLineChartView =
        [this](
            const QString& titleText,
            const QColor& firstColor,
            const QColor& secondColor,
            const QString& firstSeriesName,
            const QString& secondSeriesName,
            QLineSeries** firstSeriesOut,
            QLineSeries** secondSeriesOut,
            QValueAxis** axisXOut,
            QValueAxis** axisYOut,
            QChartView** chartViewOut) {
        QLineSeries* firstSeries = new QLineSeries(this);
        firstSeries->setName(firstSeriesName);
        firstSeries->setColor(firstColor);
        QPen firstPen(firstColor);
        firstPen.setWidthF(2.6);
        firstSeries->setPen(firstPen);

        QLineSeries* secondSeries = new QLineSeries(this);
        secondSeries->setName(secondSeriesName);
        secondSeries->setColor(secondColor);
        QPen secondPen(secondColor);
        secondPen.setWidthF(2.6);
        secondSeries->setPen(secondPen);

        // 区域着色：
        // - 按用户要求给“磁盘读写/网络收发”曲线增加填充区域；
        // - 通过半透明主色增强深浅色主题下的可见性。
        QAreaSeries* firstAreaSeries = new QAreaSeries(firstSeries);
        firstAreaSeries->setName(firstSeriesName);
        firstAreaSeries->setPen(firstPen);
        QColor firstBrushColor = firstColor;
        firstBrushColor.setAlpha(52);
        firstAreaSeries->setBrush(QBrush(firstBrushColor));

        QAreaSeries* secondAreaSeries = new QAreaSeries(secondSeries);
        secondAreaSeries->setName(secondSeriesName);
        secondAreaSeries->setPen(secondPen);
        QColor secondBrushColor = secondColor;
        secondBrushColor.setAlpha(52);
        secondAreaSeries->setBrush(QBrush(secondBrushColor));

        QChart* chart = new QChart();
        chart->addSeries(firstAreaSeries);
        chart->addSeries(secondAreaSeries);
        chart->setBackgroundVisible(false);
        chart->setBackgroundRoundness(0);
        chart->setMargins(QMargins(0, 0, 0, 0));
        chart->setTitle(titleText);
        chart->legend()->setVisible(true);
        chart->legend()->setAlignment(Qt::AlignBottom);

        QValueAxis* axisX = new QValueAxis(chart);
        axisX->setRange(0, m_historyLength);
        axisX->setLabelsVisible(false);
        axisX->setGridLineVisible(false);
        axisX->setMinorGridLineVisible(false);

        QValueAxis* axisY = new QValueAxis(chart);
        axisY->setRange(0.0, 1.0);
        axisY->setLabelsVisible(false);
        axisY->setGridLineVisible(false);
        axisY->setMinorGridLineVisible(false);

        chart->addAxis(axisX, Qt::AlignBottom);
        chart->addAxis(axisY, Qt::AlignLeft);
        firstAreaSeries->attachAxis(axisX);
        firstAreaSeries->attachAxis(axisY);
        secondAreaSeries->attachAxis(axisX);
        secondAreaSeries->attachAxis(axisY);

        if (firstSeriesOut != nullptr)
        {
            *firstSeriesOut = firstSeries;
        }
        if (secondSeriesOut != nullptr)
        {
            *secondSeriesOut = secondSeries;
        }
        if (axisXOut != nullptr)
        {
            *axisXOut = axisX;
        }
        if (axisYOut != nullptr)
        {
            *axisYOut = axisY;
        }
        if (chartViewOut != nullptr)
        {
            *chartViewOut = createNoFrameChartView(chart, this);
        }
    };

    // 磁盘折线图：读/写双线。
    createLineChartView(
        QStringLiteral("磁盘读写速率"),
        QColor(65, 173, 255),
        QColor(86, 216, 150),
        QStringLiteral("读取"),
        QStringLiteral("写入"),
        &m_diskReadSeries,
        &m_diskWriteSeries,
        &m_diskAxisX,
        &m_diskAxisY,
        &m_diskChartView);

    // 网络折线图：下/上行双线。
    createLineChartView(
        QStringLiteral("网络收发速率"),
        QColor(105, 173, 255),
        QColor(255, 177, 92),
        QStringLiteral("下行"),
        QStringLiteral("上行"),
        &m_networkRxSeries,
        &m_networkTxSeries,
        &m_networkAxisX,
        &m_networkAxisY,
        &m_networkChartView);

    // 把四张图按 2x2 放入网格。
    m_chartGridLayout->addWidget(m_cpuChartView, 0, 0);
    m_chartGridLayout->addWidget(m_memoryChartView, 0, 1);
    m_chartGridLayout->addWidget(m_diskChartView, 1, 0);
    m_chartGridLayout->addWidget(m_networkChartView, 1, 1);
    m_chartGridLayout->setRowStretch(0, 1);
    m_chartGridLayout->setRowStretch(1, 1);
    m_chartGridLayout->setColumnStretch(0, 1);
    m_chartGridLayout->setColumnStretch(1, 1);
}

void MonitorPanelWidget::initializeCounters()
{
    // CPU 每核计数器初始化：
    // - 句柄未创建时打开 PDH Query；
    // - 为每一个核心添加 \Processor(n)\% Processor Time 计数器。
    if (m_cpuPerfQueryHandle == nullptr)
    {
        PDH_HQUERY cpuQueryHandle = nullptr;
        const PDH_STATUS openCpuQueryStatus = ::PdhOpenQueryW(nullptr, 0, &cpuQueryHandle);
        if (openCpuQueryStatus == ERROR_SUCCESS && cpuQueryHandle != nullptr)
        {
            m_cpuPerfQueryHandle = cpuQueryHandle;
            m_coreCounterHandles.clear();
            const int coreCount = m_cpuCoreBarSet != nullptr ? m_cpuCoreBarSet->count() : 0;
            m_coreCounterHandles.reserve(static_cast<std::size_t>(std::max(0, coreCount)));

            for (int coreIndex = 0; coreIndex < coreCount; ++coreIndex)
            {
                const QString counterPath = QStringLiteral("\\Processor(%1)\\% Processor Time").arg(coreIndex);
                PDH_HCOUNTER cpuCounterHandle = nullptr;
                const PDH_STATUS addCounterStatus = ::PdhAddEnglishCounterW(
                    cpuQueryHandle,
                    reinterpret_cast<LPCWSTR>(counterPath.utf16()),
                    0,
                    &cpuCounterHandle);
                if (addCounterStatus != ERROR_SUCCESS || cpuCounterHandle == nullptr)
                {
                    // 某核心计数器创建失败时保留 nullptr，占位后续填 0。
                    m_coreCounterHandles.push_back(nullptr);
                    continue;
                }
                m_coreCounterHandles.push_back(cpuCounterHandle);
            }

            // 首次 collect 先建立基线，下一次采样得到稳定数据。
            ::PdhCollectQueryData(cpuQueryHandle);
        }
    }

    // 磁盘计数器初始化：
    // - 读取系统总磁盘读写字节每秒速率；
    // - 失败时保持句柄为空，刷新阶段回退为 0。
    if (m_diskPerfQueryHandle == nullptr)
    {
        PDH_HQUERY diskQueryHandle = nullptr;
        const PDH_STATUS openDiskQueryStatus = ::PdhOpenQueryW(nullptr, 0, &diskQueryHandle);
        if (openDiskQueryStatus == ERROR_SUCCESS && diskQueryHandle != nullptr)
        {
            PDH_HCOUNTER readCounterHandle = nullptr;
            PDH_HCOUNTER writeCounterHandle = nullptr;

            const PDH_STATUS addReadStatus = ::PdhAddEnglishCounterW(
                diskQueryHandle,
                L"\\PhysicalDisk(_Total)\\Disk Read Bytes/sec",
                0,
                &readCounterHandle);
            const PDH_STATUS addWriteStatus = ::PdhAddEnglishCounterW(
                diskQueryHandle,
                L"\\PhysicalDisk(_Total)\\Disk Write Bytes/sec",
                0,
                &writeCounterHandle);

            if (addReadStatus == ERROR_SUCCESS
                && addWriteStatus == ERROR_SUCCESS
                && readCounterHandle != nullptr
                && writeCounterHandle != nullptr)
            {
                m_diskPerfQueryHandle = diskQueryHandle;
                m_diskReadCounterHandle = readCounterHandle;
                m_diskWriteCounterHandle = writeCounterHandle;
                ::PdhCollectQueryData(diskQueryHandle);
            }
            else
            {
                ::PdhCloseQuery(diskQueryHandle);
            }
        }
    }
}

void MonitorPanelWidget::refreshMetrics()
{
    // 采样 CPU 每核占用与总占用平均值。
    std::vector<double> perCoreUsageList;
    double totalCpuUsage = 0.0;
    if (!samplePerCoreCpuUsage(&perCoreUsageList, &totalCpuUsage))
    {
        perCoreUsageList.assign(static_cast<std::size_t>(
            m_cpuCoreBarSet != nullptr ? std::max(0, m_cpuCoreBarSet->count()) : 0), 0.0);
        totalCpuUsage = 0.0;
    }

    // 更新 CPU 每核柱条数据。
    if (m_cpuCoreBarSet != nullptr)
    {
        const int coreBarCount = m_cpuCoreBarSet->count();
        for (int indexValue = 0; indexValue < coreBarCount; ++indexValue)
        {
            const double usageValue =
                indexValue < static_cast<int>(perCoreUsageList.size())
                ? perCoreUsageList[static_cast<std::size_t>(indexValue)]
                : 0.0;
            m_cpuCoreBarSet->replace(indexValue, usageValue);
        }
    }
    if (m_cpuChartView != nullptr && m_cpuChartView->chart() != nullptr)
    {
        m_cpuChartView->chart()->setTitle(
            QStringLiteral("CPU 每核心利用率（总体 %1%）").arg(totalCpuUsage, 0, 'f', 1));
    }

    // 采样并更新内存柱图。
    double memoryUsagePercent = 0.0;
    if (!sampleMemoryUsage(&memoryUsagePercent))
    {
        memoryUsagePercent = 0.0;
    }
    if (m_memoryBarSet != nullptr)
    {
        m_memoryBarSet->replace(0, memoryUsagePercent);
    }
    if (m_memoryChartView != nullptr && m_memoryChartView->chart() != nullptr)
    {
        m_memoryChartView->chart()->setTitle(
            QStringLiteral("内存利用率 %1%").arg(memoryUsagePercent, 0, 'f', 1));
    }

    // 采样并更新磁盘/网络折线图。
    double diskReadBytesPerSec = 0.0;
    double diskWriteBytesPerSec = 0.0;
    if (!sampleDiskRate(&diskReadBytesPerSec, &diskWriteBytesPerSec))
    {
        diskReadBytesPerSec = 0.0;
        diskWriteBytesPerSec = 0.0;
    }

    double networkRxBytesPerSec = 0.0;
    double networkTxBytesPerSec = 0.0;
    if (!sampleNetworkRate(&networkRxBytesPerSec, &networkTxBytesPerSec))
    {
        networkRxBytesPerSec = 0.0;
        networkTxBytesPerSec = 0.0;
    }

    ++m_sampleCounter;
    appendLineSample(m_diskReadSeries, m_diskAxisX, m_diskAxisY, diskReadBytesPerSec);
    appendLineSample(m_diskWriteSeries, m_diskAxisX, m_diskAxisY, diskWriteBytesPerSec);
    appendLineSample(m_networkRxSeries, m_networkAxisX, m_networkAxisY, networkRxBytesPerSec);
    appendLineSample(m_networkTxSeries, m_networkAxisX, m_networkAxisY, networkTxBytesPerSec);

    // 双序列共享 Y 轴，需要按两条线共同最大值修正 Y 范围。
    auto updateAxisRangeByPair = [](
        QLineSeries* firstSeries,
        QLineSeries* secondSeries,
        QValueAxis* axisY) {
        if (firstSeries == nullptr || secondSeries == nullptr || axisY == nullptr)
        {
            return;
        }

        double maxYValue = 1.0;
        const QList<QPointF> firstPointList = firstSeries->points();
        const QList<QPointF> secondPointList = secondSeries->points();
        for (const QPointF& pointValue : firstPointList)
        {
            maxYValue = std::max(maxYValue, pointValue.y());
        }
        for (const QPointF& pointValue : secondPointList)
        {
            maxYValue = std::max(maxYValue, pointValue.y());
        }
        axisY->setRange(0.0, maxYValue * 1.2);
    };
    updateAxisRangeByPair(m_diskReadSeries, m_diskWriteSeries, m_diskAxisY);
    updateAxisRangeByPair(m_networkRxSeries, m_networkTxSeries, m_networkAxisY);

    if (m_diskChartView != nullptr && m_diskChartView->chart() != nullptr)
    {
        m_diskChartView->chart()->setTitle(
            QStringLiteral("磁盘读写速率  读:%1  写:%2")
            .arg(bytesPerSecondToText(diskReadBytesPerSec))
            .arg(bytesPerSecondToText(diskWriteBytesPerSec)));
    }
    if (m_networkChartView != nullptr && m_networkChartView->chart() != nullptr)
    {
        m_networkChartView->chart()->setTitle(
            QStringLiteral("网络收发速率  下:%1  上:%2")
            .arg(bytesPerSecondToText(networkRxBytesPerSec))
            .arg(bytesPerSecondToText(networkTxBytesPerSec)));
    }
}

bool MonitorPanelWidget::samplePerCoreCpuUsage(
    std::vector<double>* coreUsagesOut,
    double* totalUsageOut)
{
    if (coreUsagesOut == nullptr || totalUsageOut == nullptr)
    {
        return false;
    }

    if (m_cpuPerfQueryHandle == nullptr)
    {
        initializeCounters();
    }
    if (m_cpuPerfQueryHandle == nullptr)
    {
        return false;
    }

    const PDH_STATUS collectStatus = ::PdhCollectQueryData(
        reinterpret_cast<PDH_HQUERY>(m_cpuPerfQueryHandle));
    if (collectStatus != ERROR_SUCCESS)
    {
        return false;
    }

    coreUsagesOut->clear();
    coreUsagesOut->reserve(m_coreCounterHandles.size());

    double usageSum = 0.0;
    int validCoreCount = 0;
    for (void* counterHandleVoid : m_coreCounterHandles)
    {
        if (counterHandleVoid == nullptr)
        {
            coreUsagesOut->push_back(0.0);
            continue;
        }

        PDH_FMT_COUNTERVALUE counterValue{};
        const PDH_STATUS readStatus = ::PdhGetFormattedCounterValue(
            reinterpret_cast<PDH_HCOUNTER>(counterHandleVoid),
            PDH_FMT_DOUBLE,
            nullptr,
            &counterValue);
        if (readStatus != ERROR_SUCCESS)
        {
            coreUsagesOut->push_back(0.0);
            continue;
        }

        const double usageValue = std::clamp(counterValue.doubleValue, 0.0, 100.0);
        coreUsagesOut->push_back(usageValue);
        usageSum += usageValue;
        ++validCoreCount;
    }

    *totalUsageOut = validCoreCount > 0
        ? usageSum / static_cast<double>(validCoreCount)
        : 0.0;
    return true;
}

bool MonitorPanelWidget::sampleMemoryUsage(double* memoryUsageOut) const
{
    if (memoryUsageOut == nullptr)
    {
        return false;
    }

    MEMORYSTATUSEX memoryStatus{};
    memoryStatus.dwLength = sizeof(memoryStatus);
    if (::GlobalMemoryStatusEx(&memoryStatus) == FALSE)
    {
        *memoryUsageOut = 0.0;
        return false;
    }

    *memoryUsageOut = static_cast<double>(memoryStatus.dwMemoryLoad);
    return true;
}

bool MonitorPanelWidget::sampleDiskRate(
    double* readBytesPerSecOut,
    double* writeBytesPerSecOut)
{
    if (readBytesPerSecOut == nullptr || writeBytesPerSecOut == nullptr)
    {
        return false;
    }

    if (m_diskPerfQueryHandle == nullptr)
    {
        initializeCounters();
    }
    if (m_diskPerfQueryHandle == nullptr
        || m_diskReadCounterHandle == nullptr
        || m_diskWriteCounterHandle == nullptr)
    {
        return false;
    }

    const PDH_HQUERY diskQueryHandle = reinterpret_cast<PDH_HQUERY>(m_diskPerfQueryHandle);
    const PDH_STATUS collectStatus = ::PdhCollectQueryData(diskQueryHandle);
    if (collectStatus != ERROR_SUCCESS)
    {
        return false;
    }

    PDH_FMT_COUNTERVALUE readCounterValue{};
    PDH_FMT_COUNTERVALUE writeCounterValue{};
    const PDH_STATUS readStatus = ::PdhGetFormattedCounterValue(
        reinterpret_cast<PDH_HCOUNTER>(m_diskReadCounterHandle),
        PDH_FMT_DOUBLE,
        nullptr,
        &readCounterValue);
    const PDH_STATUS writeStatus = ::PdhGetFormattedCounterValue(
        reinterpret_cast<PDH_HCOUNTER>(m_diskWriteCounterHandle),
        PDH_FMT_DOUBLE,
        nullptr,
        &writeCounterValue);
    if (readStatus != ERROR_SUCCESS || writeStatus != ERROR_SUCCESS)
    {
        return false;
    }

    *readBytesPerSecOut = std::max(0.0, readCounterValue.doubleValue);
    *writeBytesPerSecOut = std::max(0.0, writeCounterValue.doubleValue);
    return true;
}

bool MonitorPanelWidget::sampleNetworkRate(
    double* rxBytesPerSecOut,
    double* txBytesPerSecOut)
{
    if (rxBytesPerSecOut == nullptr || txBytesPerSecOut == nullptr)
    {
        return false;
    }

    MIB_IF_TABLE2* ifTablePointer = nullptr;
    const DWORD tableStatus = ::GetIfTable2(&ifTablePointer);
    if (tableStatus != NO_ERROR || ifTablePointer == nullptr)
    {
        return false;
    }

    std::uint64_t totalRxBytes = 0;
    std::uint64_t totalTxBytes = 0;
    for (ULONG rowIndex = 0; rowIndex < ifTablePointer->NumEntries; ++rowIndex)
    {
        const MIB_IF_ROW2& rowValue = ifTablePointer->Table[rowIndex];
        if (rowValue.OperStatus != IfOperStatusUp)
        {
            continue;
        }
        if (rowValue.Type == IF_TYPE_SOFTWARE_LOOPBACK)
        {
            continue;
        }

        totalRxBytes += static_cast<std::uint64_t>(rowValue.InOctets);
        totalTxBytes += static_cast<std::uint64_t>(rowValue.OutOctets);
    }
    ::FreeMibTable(ifTablePointer);

    const qint64 currentSampleMs = QDateTime::currentMSecsSinceEpoch();
    if (m_lastNetworkSampleMs <= 0)
    {
        m_lastNetworkSampleMs = currentSampleMs;
        m_lastNetworkRxBytes = totalRxBytes;
        m_lastNetworkTxBytes = totalTxBytes;
        *rxBytesPerSecOut = 0.0;
        *txBytesPerSecOut = 0.0;
        return true;
    }

    const qint64 elapsedMs = currentSampleMs - m_lastNetworkSampleMs;
    if (elapsedMs <= 0)
    {
        return false;
    }

    const std::uint64_t deltaRxBytes = totalRxBytes >= m_lastNetworkRxBytes
        ? (totalRxBytes - m_lastNetworkRxBytes)
        : 0;
    const std::uint64_t deltaTxBytes = totalTxBytes >= m_lastNetworkTxBytes
        ? (totalTxBytes - m_lastNetworkTxBytes)
        : 0;

    m_lastNetworkSampleMs = currentSampleMs;
    m_lastNetworkRxBytes = totalRxBytes;
    m_lastNetworkTxBytes = totalTxBytes;

    *rxBytesPerSecOut = static_cast<double>(deltaRxBytes) * 1000.0 / static_cast<double>(elapsedMs);
    *txBytesPerSecOut = static_cast<double>(deltaTxBytes) * 1000.0 / static_cast<double>(elapsedMs);
    return true;
}

void MonitorPanelWidget::appendLineSample(
    QLineSeries* series,
    QValueAxis* axisX,
    QValueAxis* axisY,
    const double value)
{
    if (series == nullptr || axisX == nullptr || axisY == nullptr)
    {
        return;
    }

    series->append(m_sampleCounter, value);
    while (series->count() > m_historyLength)
    {
        series->remove(0);
    }

    const QList<QPointF> pointList = series->points();
    if (pointList.isEmpty())
    {
        return;
    }

    const double minX = pointList.first().x();
    const double maxX = pointList.last().x();
    axisX->setRange(minX, std::max(maxX, minX + 1.0));

    double maxYValue = 1.0;
    for (const QPointF& pointValue : pointList)
    {
        maxYValue = std::max(maxYValue, pointValue.y());
    }
    axisY->setRange(0.0, maxYValue * 1.2);
}
