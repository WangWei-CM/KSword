#include "HardwareDock.h"
#include "DiskMonitorPage.h"
#include "MemoryCompositionHistoryWidget.h"
#include "HardwareOtherDevicesPage.h"

// ============================================================
// HardwareDock.cpp
// 作用：
// 1) 提供利用率优先的硬件监控视图与硬件总览；
// 2) 利用 PDH + Power API 周期采样 CPU/内存/每核频率；
// 3) 显卡与内存模块信息通过 PowerShell/WMI 文本化展示。
// ============================================================

#include "../theme.h"
#include "../UI/CodeEditorWidget.h"
#include "../UI/PerformanceNavCard.h"

#include <QAbstractScrollArea>
#include <QAbstractItemView>
#include <QBrush>
#include <QDateTime>
#include <QFrame>
#include <QHBoxLayout>
#include <QGridLayout>
#include <QHeaderView>
#include <QLabel>
#include <QListWidget>
#include <QListWidgetItem>
#include <QMetaObject>
#include <QPainter>
#include <QPen>
#include <QPointer>
#include <QProcess>
#include <QRegularExpression>
#include <QResizeEvent>
#include <QScrollArea>
#include <QShowEvent>
#include <QSizePolicy>
#include <QStackedWidget>
#include <QTabWidget>
#include <QTableWidget>
#include <QTableWidgetItem>
#include <QTimer>
#include <QVBoxLayout>

#include <QtCharts/QChart>
#include <QtCharts/QChartView>
#include <QtCharts/QAreaSeries>
#include <QtCharts/QLineSeries>
#include <QtCharts/QValueAxis>

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstring>
#include <limits>
#include <thread>

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#include <intrin.h>
#include <Pdh.h>
#include <pdhmsg.h>
#include <PowrProf.h>
#include <Psapi.h>
#include <dxgi1_6.h>
#include <iphlpapi.h>
#include <netioapi.h>

#pragma comment(lib, "Pdh.lib")
#pragma comment(lib, "PowrProf.lib")
#pragma comment(lib, "Psapi.lib")
#pragma comment(lib, "Dxgi.lib")
#pragma comment(lib, "Iphlpapi.lib")

namespace
{
    // queryPowerShellTextSync 前置声明：
    // - 供下方硬件摘要函数调用；
    // - 实际定义位于同命名空间后半段。
    QString queryPowerShellTextSync(const QString& scriptText, int timeoutMs);

    // buildStatusColor 作用：
    // - 深浅色模式下返回统一可读的次级文本颜色。
    QColor buildStatusColor()
    {
        return KswordTheme::IsDarkModeEnabled()
            ? QColor(185, 205, 225)
            : QColor(55, 80, 105);
    }

    // appendTransparentBackgroundStyle 作用：
    // - 给“硬件 -> 计数器/利用率”页控件补充透明背景样式；
    // - 若控件属于滚动区域，还会同步把 viewport 设为透明，避免残留底色。
    void appendTransparentBackgroundStyle(QWidget* widgetPointer)
    {
        if (widgetPointer == nullptr)
        {
            return;
        }

        widgetPointer->setAttribute(Qt::WA_StyledBackground, true);
        widgetPointer->setAutoFillBackground(false);

        // transparentDeclarationText 用途：透明背景声明片段；transparentRuleText 用途：当前控件专用规则块。
        const QString transparentDeclarationText =
            QStringLiteral("background:transparent;background-color:transparent;border:none;");
        const QString transparentRuleText = QStringLiteral("%1{%2}")
            .arg(QString::fromLatin1(widgetPointer->metaObject()->className()))
            .arg(transparentDeclarationText);
        if (!widgetPointer->styleSheet().contains(QStringLiteral("background:transparent")))
        {
            widgetPointer->setStyleSheet(widgetPointer->styleSheet() + transparentRuleText);
        }

        QAbstractScrollArea* abstractScrollAreaPointer =
            qobject_cast<QAbstractScrollArea*>(widgetPointer);
        if (abstractScrollAreaPointer == nullptr || abstractScrollAreaPointer->viewport() == nullptr)
        {
            return;
        }

        abstractScrollAreaPointer->viewport()->setAttribute(Qt::WA_StyledBackground, true);
        abstractScrollAreaPointer->viewport()->setAutoFillBackground(false);
        if (!abstractScrollAreaPointer->viewport()->styleSheet().contains(QStringLiteral("background:transparent")))
        {
            const QString viewportTransparentRuleText = QStringLiteral("%1{%2}")
                .arg(QString::fromLatin1(abstractScrollAreaPointer->viewport()->metaObject()->className()))
                .arg(transparentDeclarationText);
            abstractScrollAreaPointer->viewport()->setStyleSheet(
                abstractScrollAreaPointer->viewport()->styleSheet() + viewportTransparentRuleText);
        }
    }

    // configureTransparentChart 作用：
    // - 统一关闭图表背景与绘图区背景；
    // - 避免 QChart 在透明容器上仍然绘制白底/深底块。
    void configureTransparentChart(QChart* chartPointer)
    {
        if (chartPointer == nullptr)
        {
            return;
        }

        chartPointer->setBackgroundVisible(false);
        chartPointer->setPlotAreaBackgroundVisible(false);
        chartPointer->setBackgroundRoundness(0);
        chartPointer->setMargins(QMargins(0, 0, 0, 0));
    }

    // configureTransparentChartViewOnly 作用：
    // - 只清理 QChart 外层背景，不覆盖调用者已设置的 plotArea 背景和网格边框；
    // - CPU 单核利用率图需要保留绘图区方框/填充，因此不能调用 configureTransparentChart；
    // - 返回行为：无返回值，空指针直接忽略。
    void configureTransparentChartViewOnly(QChart* chartPointer)
    {
        if (chartPointer == nullptr)
        {
            return;
        }

        chartPointer->setBackgroundVisible(false);
        chartPointer->setBackgroundRoundness(0);
        chartPointer->setMargins(QMargins(0, 0, 0, 0));
    }

    // configureCompressibleWidget 作用：
    // - 清掉控件默认最小尺寸，允许 Dock 窄宽/低高时继续压缩而不是请求外层滚动条；
    // - horizontalPolicy/verticalPolicy 用于按页面角色指定横纵向分配策略；
    // - 返回行为：无返回值，只修改 QWidget 的布局属性。
    void configureCompressibleWidget(
        QWidget* widgetPointer,
        const QSizePolicy::Policy horizontalPolicy = QSizePolicy::Preferred,
        const QSizePolicy::Policy verticalPolicy = QSizePolicy::Preferred)
    {
        if (widgetPointer == nullptr)
        {
            return;
        }

        widgetPointer->setMinimumSize(0, 0);
        widgetPointer->setSizePolicy(horizontalPolicy, verticalPolicy);
    }

    // configureCompressibleLabel 作用：
    // - 让长设备名、CPU/GPU 型号和详情文本在空间不足时被布局压缩/裁剪；
    // - 这样页面宽高变小时优先缩小内容，而不是把外层 QScrollArea 撑出滚动条；
    // - 返回行为：无返回值，只修改 QLabel 的布局属性。
    void configureCompressibleLabel(
        QLabel* labelPointer,
        const QSizePolicy::Policy horizontalPolicy = QSizePolicy::Ignored,
        const QSizePolicy::Policy verticalPolicy = QSizePolicy::Preferred)
    {
        configureCompressibleWidget(labelPointer, horizontalPolicy, verticalPolicy);
    }

    // configurePersistentHeaderLabel 作用：
    // - 用于“利用率”详情页顶部标题和右侧设备型号备注；
    // - 这些标签必须始终保留一行可见高度，不能被图表区域压缩到 0；
    // - 返回行为：无返回值，只调整 QLabel 的单行布局策略。
    void configurePersistentHeaderLabel(
        QLabel* labelPointer,
        const QSizePolicy::Policy horizontalPolicy = QSizePolicy::Preferred)
    {
        if (labelPointer == nullptr)
        {
            return;
        }

        labelPointer->setMinimumSize(0, 0);
        labelPointer->setWordWrap(false);
        labelPointer->setSizePolicy(horizontalPolicy, QSizePolicy::Fixed);
        labelPointer->setMinimumHeight(std::max(1, labelPointer->sizeHint().height()));
    }

    // lockLabelHeightToFont 作用：
    // - 在设置大字号样式后重新按字体度量锁定 QLabel 行高；
    // - 解决 QLabel 先计算普通字号 sizeHint、后套 46px 样式时顶部/底部被布局裁剪的问题；
    // - 参数 extraVerticalPadding：给字体 ascent/descent 外额外预留的上下像素总量；
    // - 返回行为：无返回值，仅更新标签最小/最大高度。
    void lockLabelHeightToFont(QLabel* labelPointer, const int extraVerticalPadding)
    {
        if (labelPointer == nullptr)
        {
            return;
        }

        // ensurePolished 用途：让 stylesheet 的 font-size/font-weight 先生效，再读取字体度量。
        labelPointer->ensurePolished();
        // fontHeight 用途：读取应用样式后真实字体高度，避免依赖过期 sizeHint。
        const int fontHeight = labelPointer->fontMetrics().height();
        // targetHeight 用途：大标题保留上下余量，避免高 DPI 与字体 fallback 时被裁剪。
        const int targetHeight = std::max(
            labelPointer->sizeHint().height(),
            fontHeight + std::max(0, extraVerticalPadding));
        labelPointer->setMinimumHeight(targetHeight);
        labelPointer->setMaximumHeight(targetHeight);
    }

    // bytesToGiBText 作用：
    // - 把字节数转换为 GiB 文本，保留 2 位小数。
    QString bytesToGiBText(const std::uint64_t bytesValue)
    {
        const double gibValue = static_cast<double>(bytesValue) / (1024.0 * 1024.0 * 1024.0);
        return QStringLiteral("%1 GiB").arg(gibValue, 0, 'f', 2);
    }

    // bytesPerSecondToText 作用：
    // - 把字节每秒速率转换为可读文本（B/s、KB/s、MB/s、GB/s）；
    // - 用于利用率子页摘要中展示磁盘/网络速率。
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

    // bytesToReadableText 作用：
    // - 把字节数转换为 B/KB/MB/GB 的可读文本；
    // - 用于任务管理器参数区展示容量信息。
    QString bytesToReadableText(const double bytesValue)
    {
        const double safeValue = std::max(0.0, bytesValue);
        if (safeValue < 1024.0)
        {
            return QStringLiteral("%1 B").arg(safeValue, 0, 'f', 0);
        }
        if (safeValue < 1024.0 * 1024.0)
        {
            return QStringLiteral("%1 KB").arg(safeValue / 1024.0, 0, 'f', 1);
        }
        if (safeValue < 1024.0 * 1024.0 * 1024.0)
        {
            return QStringLiteral("%1 MB").arg(safeValue / (1024.0 * 1024.0), 0, 'f', 2);
        }
        return QStringLiteral("%1 GB").arg(safeValue / (1024.0 * 1024.0 * 1024.0), 0, 'f', 2);
    }

    // resolveGpuEngineKeyFromCounter 作用：
    // - 把 PDH GPU 引擎计数器名称映射为固定键名；
    // - 仅关心任务管理器常见四类：3D/Copy/Video Encode/Video Decode。
    QString resolveGpuEngineKeyFromCounter(const QString& counterNameText)
    {
        const QString lowerText = counterNameText.toLower();
        if (lowerText.contains(QStringLiteral("engtype_3d")))
        {
            return QStringLiteral("3d");
        }
        if (lowerText.contains(QStringLiteral("engtype_copy")))
        {
            return QStringLiteral("copy");
        }
        if (lowerText.contains(QStringLiteral("engtype_videoencode"))
            || lowerText.contains(QStringLiteral("engtype_videncode")))
        {
            return QStringLiteral("video_encode");
        }
        if (lowerText.contains(QStringLiteral("engtype_videodecode"))
            || lowerText.contains(QStringLiteral("engtype_viddecode")))
        {
            return QStringLiteral("video_decode");
        }
        return QString();
    }

    // packLuidKey 作用：
    // - 把 Windows LUID 的 HighPart/LowPart 合并为稳定 64 位键；
    // - 用于 DXGI 显卡适配器与 PDH GPU Engine 实例之间做关联。
    std::uint64_t packLuidKey(const LUID& luidValue)
    {
        const std::uint64_t highPartValue =
            static_cast<std::uint64_t>(static_cast<std::uint32_t>(luidValue.HighPart));
        const std::uint64_t lowPartValue =
            static_cast<std::uint64_t>(luidValue.LowPart);
        return (highPartValue << 32U) | lowPartValue;
    }

    // interfaceLuidToKey 作用：
    // - 把 MIB_IF_ROW2 的 InterfaceLuid.Value 转为无符号键；
    // - 调用方用该键跨采样周期匹配同一块网卡。
    std::uint64_t interfaceLuidToKey(const std::uint64_t luidValue)
    {
        return luidValue;
    }

    // simplifyDiskInstanceName 作用：
    // - 把 PDH PhysicalDisk 实例名转换为任务管理器风格标题；
    // - 示例："0 C:" 显示为“磁盘 0 (C:)”。
    QString simplifyDiskInstanceName(const QString& instanceNameText)
    {
        const QString trimmedText = instanceNameText.trimmed();
        if (trimmedText.isEmpty())
        {
            return QStringLiteral("磁盘");
        }
        if (trimmedText == QStringLiteral("_Total"))
        {
            return QStringLiteral("磁盘总计");
        }

        const int spaceIndex = trimmedText.indexOf(QLatin1Char(' '));
        if (spaceIndex > 0)
        {
            const QString diskIndexText = trimmedText.left(spaceIndex).trimmed();
            const QString volumeText = trimmedText.mid(spaceIndex + 1).trimmed();
            if (!volumeText.isEmpty())
            {
                return QStringLiteral("磁盘 %1 (%2)").arg(diskIndexText, volumeText);
            }
            return QStringLiteral("磁盘 %1").arg(diskIndexText);
        }

        return QStringLiteral("磁盘 %1").arg(trimmedText);
    }

    // parseGpuAdapterKeyFromEngineName 作用：
    // - 从 PDH GPU Engine 实例名中解析 LUID；
    // - Windows 常见格式包含“luid_0xHIGH_0xLOW”，失败时返回 false。
    bool parseGpuAdapterKeyFromEngineName(
        const QString& engineNameText,
        std::uint64_t* adapterKeyOut)
    {
        if (adapterKeyOut == nullptr)
        {
            return false;
        }

        static const QRegularExpression luidRegex(
            QStringLiteral("luid_0x([0-9a-fA-F]+)_0x([0-9a-fA-F]+)"));
        const QRegularExpressionMatch matchValue = luidRegex.match(engineNameText);
        if (!matchValue.hasMatch())
        {
            return false;
        }

        bool highOk = false;
        bool lowOk = false;
        const std::uint64_t highValue = matchValue.captured(1).toULongLong(&highOk, 16);
        const std::uint64_t lowValue = matchValue.captured(2).toULongLong(&lowOk, 16);
        if (!highOk || !lowOk)
        {
            return false;
        }

        *adapterKeyOut = ((highValue & 0xFFFFFFFFULL) << 32U) | (lowValue & 0xFFFFFFFFULL);
        return true;
    }

    // formatDurationText 作用：
    // - 把秒数格式化为“天:时:分:秒”； 
    // - 用于 CPU 页“正常运行时间”展示。
    QString formatDurationText(const std::uint64_t totalSeconds)
    {
        const std::uint64_t dayCount = totalSeconds / 86400ULL;
        const std::uint64_t hourCount = (totalSeconds % 86400ULL) / 3600ULL;
        const std::uint64_t minuteCount = (totalSeconds % 3600ULL) / 60ULL;
        const std::uint64_t secondCount = totalSeconds % 60ULL;
        return QStringLiteral("%1:%2:%3:%4")
            .arg(dayCount)
            .arg(hourCount, 2, 10, QLatin1Char('0'))
            .arg(minuteCount, 2, 10, QLatin1Char('0'))
            .arg(secondCount, 2, 10, QLatin1Char('0'));
    }

    // queryCpuBrandTextByCpuid 作用：
    // - 通过 CPUID 指令读取 CPU 品牌字符串；
    // - 避免 CPU 型号依赖 PowerShell 查询。
    QString queryCpuBrandTextByCpuid()
    {
        int cpuInfo[4] = {};
        __cpuid(cpuInfo, 0x80000000);
        const unsigned int maxExtendedLeaf = static_cast<unsigned int>(cpuInfo[0]);
        if (maxExtendedLeaf < 0x80000004)
        {
            return QStringLiteral("N/A");
        }

        char brandBuffer[49] = {};
        int* brandIntBuffer = reinterpret_cast<int*>(brandBuffer);
        __cpuid(brandIntBuffer, 0x80000002);
        __cpuid(brandIntBuffer + 4, 0x80000003);
        __cpuid(brandIntBuffer + 8, 0x80000004);

        const QString brandText = QString::fromLatin1(brandBuffer).trimmed();
        return brandText.isEmpty() ? QStringLiteral("N/A") : brandText;
    }

    // countBits 作用：
    // - 计算处理器亲和掩码中的置位数量；
    // - 用于统计逻辑处理器个数。
    int countBits(const KAFFINITY affinityMask)
    {
        return std::popcount(static_cast<unsigned long long>(affinityMask));
    }

    // MemoryHardwareSummarySnapshot 作用：
    // - 保存内存硬件摘要（频率、插槽、外形规格）；
    // - 由后台 PowerShell 查询填充，用于利用率详情页展示。
    struct MemoryHardwareSummarySnapshot
    {
        int speedMhz = 0;               // speedMhz：内存主频（MHz）。
        int usedSlots = 0;              // usedSlots：已使用插槽数量。
        int totalSlots = 0;             // totalSlots：主板总插槽数量。
        QString formFactorText = QStringLiteral("N/A"); // formFactorText：内存外形规格文本。
    };

    // GpuHardwareSummarySnapshot 作用：
    // - 保存显卡摘要（名称、驱动、显存）； 
    // - 由后台 PowerShell 查询填充，用于 GPU 利用率详情页展示。
    struct GpuHardwareSummarySnapshot
    {
        QString adapterNameText = QStringLiteral("N/A");    // adapterNameText：显卡名称。
        QString driverVersionText = QStringLiteral("N/A");  // driverVersionText：驱动版本。
        QString driverDateText = QStringLiteral("N/A");     // driverDateText：驱动日期。
        QString pnpDeviceIdText = QStringLiteral("N/A");    // pnpDeviceIdText：PNP 设备ID。
        double dedicatedMemoryGiB = 0.0;                    // dedicatedMemoryGiB：专用显存 GiB。
    };

    // queryMemoryHardwareSummarySnapshot 作用：
    // - 查询内存硬件参数（速度、插槽、外形规格）； 
    // - 仅在后台线程调用，避免阻塞 UI。
    MemoryHardwareSummarySnapshot queryMemoryHardwareSummarySnapshot()
    {
        const QString scriptText = QStringLiteral(
            "$mods=Get-CimInstance Win32_PhysicalMemory; "
            "$arr=Get-CimInstance Win32_PhysicalMemoryArray | Select-Object -First 1 -ExpandProperty MemoryDevices; "
            "$speed=($mods | Select-Object -First 1 -ExpandProperty ConfiguredClockSpeed); "
            "$formCode=($mods | Select-Object -First 1 -ExpandProperty FormFactor); "
            "$formText=if([int]$formCode -eq 8){'DIMM'}elseif([int]$formCode -eq 12){'SODIMM'}elseif([int]$formCode -gt 0){'代码'+[string]$formCode}else{'N/A'}; "
            "\"$speed|$($mods.Count)|$arr|$formText\"");
        const QString outputText = queryPowerShellTextSync(scriptText, 3200);
        const QStringList fieldList = outputText.split('|');

        MemoryHardwareSummarySnapshot snapshot;
        if (fieldList.size() >= 4)
        {
            snapshot.speedMhz = fieldList.at(0).trimmed().toInt();
            snapshot.usedSlots = fieldList.at(1).trimmed().toInt();
            snapshot.totalSlots = fieldList.at(2).trimmed().toInt();
            snapshot.formFactorText = fieldList.at(3).trimmed();
            if (snapshot.formFactorText.isEmpty())
            {
                snapshot.formFactorText = QStringLiteral("N/A");
            }
        }
        return snapshot;
    }

    // queryGpuHardwareSummarySnapshot 作用：
    // - 查询显卡摘要（名称、驱动版本、专用显存）； 
    // - 仅在后台线程调用，避免阻塞 UI。
    GpuHardwareSummarySnapshot queryGpuHardwareSummarySnapshot()
    {
        const QString scriptText = QStringLiteral(
            "$gpu=Get-CimInstance Win32_VideoController | Select-Object -First 1 Name,DriverVersion,DriverDate,AdapterRAM,PNPDeviceID; "
            "if($null -eq $gpu){'N/A|N/A|N/A|N/A|0'}else{\"$($gpu.Name)|$($gpu.DriverVersion)|$($gpu.DriverDate)|$($gpu.PNPDeviceID)|$($gpu.AdapterRAM)\"}");
        const QString outputText = queryPowerShellTextSync(scriptText, 2800);
        const QStringList fieldList = outputText.split('|');

        GpuHardwareSummarySnapshot snapshot;
        if (fieldList.size() >= 5)
        {
            snapshot.adapterNameText = fieldList.at(0).trimmed();
            snapshot.driverVersionText = fieldList.at(1).trimmed();
            snapshot.driverDateText = fieldList.at(2).trimmed();
            snapshot.pnpDeviceIdText = fieldList.at(3).trimmed();
            const double memoryBytes = fieldList.at(4).trimmed().toDouble();
            snapshot.dedicatedMemoryGiB = memoryBytes / (1024.0 * 1024.0 * 1024.0);
        }
        return snapshot;
    }

    // createNoFrameChartView 作用：
    // - 创建无边框 ChartView，统一 Dock 内视觉风格。
    QChartView* createNoFrameChartView(QChart* chart, QWidget* parentWidget)
    {
        configureTransparentChart(chart);
        QChartView* chartView = new QChartView(chart, parentWidget);
        chartView->setRenderHint(QPainter::Antialiasing, true);
        chartView->setFrameShape(QFrame::NoFrame);
        // ChartView 本身也可能产生 QGraphicsView 滚动条，这里统一关闭并允许高度压到 0。
        chartView->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
        chartView->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
        chartView->setSizeAdjustPolicy(QAbstractScrollArea::AdjustIgnored);
        configureCompressibleWidget(chartView, QSizePolicy::Expanding, QSizePolicy::Expanding);
        appendTransparentBackgroundStyle(chartView);
        return chartView;
    }

    // createPlotBackgroundChartView 作用：
    // - 创建保留 plotArea 样式的 ChartView；
    // - 用于所有利用率折线图，使绘图区方框、网格和面积填充不会被通用透明逻辑关闭；
    // - 返回值：已设置无边框和透明 viewport 的 QChartView。
    QChartView* createPlotBackgroundChartView(QChart* chart, QWidget* parentWidget)
    {
        configureTransparentChartViewOnly(chart);
        QChartView* chartView = new QChartView(chart, parentWidget);
        chartView->setRenderHint(QPainter::Antialiasing, true);
        chartView->setFrameShape(QFrame::NoFrame);
        // ChartView 本身仍然不显示滚动条，尺寸由外层利用率布局统一控制。
        chartView->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
        chartView->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
        chartView->setSizeAdjustPolicy(QAbstractScrollArea::AdjustIgnored);
        configureCompressibleWidget(chartView, QSizePolicy::Expanding, QSizePolicy::Expanding);
        appendTransparentBackgroundStyle(chartView);
        return chartView;
    }

    // colorWithAlpha 作用：
    // - 基于折线主色生成不同透明度的辅助色；
    // - 用于绘图区背景、网格、边框和面积填充保持同一色相。
    QColor colorWithAlpha(const QColor& sourceColor, const int alphaValue)
    {
        return QColor(
            sourceColor.red(),
            sourceColor.green(),
            sourceColor.blue(),
            std::clamp(alphaValue, 0, 255));
    }

    // initializeLineSeriesHistory 作用：
    // - 按固定历史长度预填充折线点；
    // - 让界面首次显示时图表有完整 X 轴窗口，后续采样只做滑动窗口追加。
    void initializeLineSeriesHistory(
        QLineSeries* lineSeries,
        const int historyLength,
        const double sampleValue = 0.0)
    {
        if (lineSeries == nullptr)
        {
            return;
        }

        for (int indexValue = 0; indexValue < historyLength; ++indexValue)
        {
            lineSeries->append(indexValue, sampleValue);
        }
    }

    // createBaselineSeries 作用：
    // - 创建与折线点数量一致的 0 轴基准线；
    // - QAreaSeries 依赖上下两条线闭合区域，因此每条利用率线都单独持有基准线。
    QLineSeries* createBaselineSeries(
        QWidget* parentWidget,
        const int historyLength,
        const double baselineValue = 0.0)
    {
        QLineSeries* baselineSeries = new QLineSeries(parentWidget);
        initializeLineSeriesHistory(baselineSeries, historyLength, baselineValue);
        return baselineSeries;
    }

    // addFilledAreaSeries 作用：
    // - 把一条折线和一条基准线组合为面积图并加入 QChart；
    // - 返回值：新建的 QAreaSeries，失败时返回 nullptr。
    QAreaSeries* addFilledAreaSeries(
        QChart* chartPointer,
        QLineSeries* lineSeries,
        QLineSeries* baselineSeries,
        const QColor& lineColor,
        const int fillAlpha = 46)
    {
        if (chartPointer == nullptr || lineSeries == nullptr || baselineSeries == nullptr)
        {
            return nullptr;
        }

        QAreaSeries* areaSeries = new QAreaSeries(lineSeries, baselineSeries);
        areaSeries->setName(lineSeries->name());
        areaSeries->setColor(colorWithAlpha(lineColor, fillAlpha));
        areaSeries->setBorderColor(lineColor);
        areaSeries->setPen(QPen(lineColor, 1.6));
        chartPointer->addSeries(areaSeries);
        return areaSeries;
    }

    // configureUtilizationPlotChart 作用：
    // - 统一设置利用率图表的外观；
    // - 保留透明外背景，同时给 plotArea 添加浅色背景和明确方框。
    void configureUtilizationPlotChart(
        QChart* chartPointer,
        const QColor& accentColor,
        const QString& titleText = QString(),
        const bool legendVisible = false)
    {
        if (chartPointer == nullptr)
        {
            return;
        }

        chartPointer->legend()->setVisible(legendVisible);
        if (legendVisible)
        {
            chartPointer->legend()->setAlignment(Qt::AlignBottom);
        }
        chartPointer->setBackgroundVisible(false);
        chartPointer->setBackgroundRoundness(0);
        chartPointer->setMargins(QMargins(0, 0, 0, 0));
        chartPointer->setTitle(titleText);
        chartPointer->setPlotAreaBackgroundVisible(true);
        chartPointer->setPlotAreaBackgroundBrush(QBrush(colorWithAlpha(accentColor, 18)));
        chartPointer->setPlotAreaBackgroundPen(QPen(colorWithAlpha(accentColor, 150), 1.0));
    }

    // configureUtilizationValueAxis 作用：
    // - 统一隐藏轴标签但保留轴线与网格；
    // - 方框和横向网格共同强化利用率趋势图边界。
    void configureUtilizationValueAxis(
        QValueAxis* axisPointer,
        const QColor& accentColor,
        const double lowerValue,
        const double upperValue)
    {
        if (axisPointer == nullptr)
        {
            return;
        }

        axisPointer->setRange(lowerValue, upperValue);
        axisPointer->setLabelsVisible(false);
        axisPointer->setGridLineVisible(true);
        axisPointer->setMinorGridLineVisible(false);
        axisPointer->setLineVisible(true);
        axisPointer->setLinePen(QPen(colorWithAlpha(accentColor, 140), 1.0));
        axisPointer->setGridLinePen(QPen(colorWithAlpha(accentColor, 46), 1.0));
    }

    // queryPowerShellTextSync 作用：
    // - 在当前线程同步执行一条 PowerShell 脚本并返回文本结果；
    // - 仅在后台工作线程调用，避免阻塞 UI 线程。
    // 参数 scriptText：要执行的 PowerShell 命令文本。
    // 参数 timeoutMs：超时时间（毫秒）。
    // 返回值：标准输出文本；失败时返回错误描述。
    QString queryPowerShellTextSync(const QString& scriptText, const int timeoutMs)
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

        // waitStartedOk 用途：判断 PowerShell 进程是否成功拉起。
        const bool waitStartedOk = process.waitForStarted(1200);
        if (!waitStartedOk)
        {
            return QStringLiteral("PowerShell启动失败。");
        }

        // waitFinishedOk 用途：判断命令是否在超时前结束。
        const bool waitFinishedOk = process.waitForFinished(timeoutMs);
        if (!waitFinishedOk)
        {
            process.kill();
            process.waitForFinished(800);
            return QStringLiteral("PowerShell执行超时（%1 ms）。").arg(timeoutMs);
        }

        // standardOutputText 用途：保存命令标准输出。
        // standardErrorText  用途：保存命令标准错误输出。
        const QString standardOutputText = QString::fromLocal8Bit(process.readAllStandardOutput()).trimmed();
        const QString standardErrorText = QString::fromLocal8Bit(process.readAllStandardError()).trimmed();
        if (process.exitStatus() != QProcess::NormalExit || process.exitCode() != 0)
        {
            return QStringLiteral("PowerShell执行失败。\nExitCode=%1\nError=%2")
                .arg(process.exitCode())
                .arg(standardErrorText.isEmpty() ? QStringLiteral("<空>") : standardErrorText);
        }

        if (standardOutputText.isEmpty())
        {
            return QStringLiteral("<无输出>");
        }
        return standardOutputText;
    }

    // buildOverviewStaticTextSnapshot 作用：
    // - 构建“概览”页静态文本快照；
    // - 仅做轻量 Win32/Qt 系统信息读取，不依赖 UI 对象。
    QString buildOverviewStaticTextSnapshot()
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

    // buildOverviewPeripheralTextSnapshot 作用：
    // - 采集“概览”页的外设与硬件设备总览文本；
    // - 覆盖用户要求的声卡/网卡/摄像头，并补充主板/BIOS/磁盘等信息。
    // 说明：
    // - 该函数会执行 PowerShell + CIM 查询；
    // - 必须在后台线程调用，避免阻塞 UI 线程。
    QString buildOverviewPeripheralTextSnapshot()
    {
        const QString scriptText = QStringLiteral(
            "$ErrorActionPreference='SilentlyContinue'; "
            "function Write-Section([string]$title,[object]$rows){ "
            "  if($null -eq $rows){return \"[$title]`n<未检测到>`n`n\"}; "
            "  $count = ($rows | Measure-Object).Count; "
            "  if($count -eq 0){return \"[$title]`n<未检测到>`n`n\"}; "
            "  $table = ($rows | Format-Table -AutoSize | Out-String); "
            "  return \"[$title]`n$table`n\"; "
            "}; "
            "$text = ''; "
            "$baseBoardRows = Get-CimInstance Win32_BaseBoard | Select-Object Manufacturer,Product,Version,SerialNumber; "
            "$biosRows = Get-CimInstance Win32_BIOS | Select-Object Manufacturer,SMBIOSBIOSVersion,ReleaseDate,SerialNumber; "
            "$cpuRows = Get-CimInstance Win32_Processor | Select-Object Name,Manufacturer,NumberOfCores,NumberOfLogicalProcessors,MaxClockSpeed; "
            "$diskRows = Get-CimInstance Win32_DiskDrive | Select-Object Model,InterfaceType,MediaType,Size,SerialNumber; "
            "$gpuRows = Get-CimInstance Win32_VideoController | Select-Object Name,AdapterRAM,DriverVersion,VideoProcessor; "
            "$soundRows = Get-CimInstance Win32_SoundDevice | Select-Object Name,Manufacturer,Status; "
            "$networkRows = Get-CimInstance Win32_NetworkAdapter | Where-Object { $_.PhysicalAdapter -eq $true } | "
            "  Select-Object Name,AdapterType,Speed,MACAddress,NetConnectionStatus,Manufacturer; "
            "$cameraRows = Get-CimInstance Win32_PnPEntity | "
            "  Where-Object { $_.PNPClass -eq 'Image' -or $_.Service -like '*usbvideo*' } | "
            "  Select-Object Name,Manufacturer,Status,Service,PNPDeviceID; "
            "$monitorRows = Get-CimInstance Win32_DesktopMonitor | Select-Object Name,MonitorType,ScreenWidth,ScreenHeight,Status; "
            "$printerRows = Get-CimInstance Win32_Printer | Select-Object Name,DriverName,PortName,WorkOffline,Default; "
            "$usbRows = Get-CimInstance Win32_USBControllerDevice | Select-Object Dependent -First 30; "
            "$text += Write-Section '主板' $baseBoardRows; "
            "$text += Write-Section 'BIOS' $biosRows; "
            "$text += Write-Section '处理器' $cpuRows; "
            "$text += Write-Section '磁盘设备' $diskRows; "
            "$text += Write-Section '显卡设备' $gpuRows; "
            "$text += Write-Section '声卡设备' $soundRows; "
            "$text += Write-Section '网卡设备(物理)' $networkRows; "
            "$text += Write-Section '摄像头设备' $cameraRows; "
            "$text += Write-Section '显示器设备' $monitorRows; "
            "$text += Write-Section '打印机设备' $printerRows; "
            "$text += Write-Section 'USB控制器映射(前30条)' $usbRows; "
            "$text");
        return queryPowerShellTextSync(scriptText, 9000);
    }

    // buildGpuStaticTextSnapshot 作用：
    // - 通过 WMI 采集显卡信息文本；
    // - 该函数会调用 PowerShell，必须放在后台线程执行。
    QString buildGpuStaticTextSnapshot()
    {
        const QString scriptText = QStringLiteral(
            "$list=Get-CimInstance Win32_VideoController | "
            "Select-Object Name,AdapterRAM,DriverVersion,VideoProcessor,CurrentHorizontalResolution,CurrentVerticalResolution,CurrentRefreshRate; "
            "if($null -eq $list){'未读取到显卡信息'} else {$list | Format-Table -AutoSize | Out-String}");
        return queryPowerShellTextSync(scriptText, 5000);
    }

    // buildMemoryStaticTextSnapshot 作用：
    // - 通过 WMI 采集内存条与系统内存信息文本；
    // - 该函数会调用 PowerShell，必须放在后台线程执行。
    QString buildMemoryStaticTextSnapshot()
    {
        const QString scriptText = QStringLiteral(
            "$phy=Get-CimInstance Win32_PhysicalMemory | "
            "Select-Object BankLabel,Manufacturer,PartNumber,ConfiguredClockSpeed,Capacity,SMBIOSMemoryType; "
            "$os=Get-CimInstance Win32_OperatingSystem | Select-Object TotalVisibleMemorySize,FreePhysicalMemory; "
            "'[物理内存条]';"
            "$phy | Format-Table -AutoSize | Out-String; "
            "'[操作系统内存]';"
            "$os | Format-List | Out-String");
        return queryPowerShellTextSync(scriptText, 6000);
    }

    // SensorProbeResult 作用：
    // - 保存一次传感器探测的值、来源和失败原因；
    // - 供异步刷新逻辑决定界面展示与日志输出。
    struct SensorProbeResult
    {
        QString valueText = QStringLiteral("N/A"); // valueText：探测到的传感器值文本。
        QString sourceText; // sourceText：成功读取时的来源标识。
        QString reasonText; // reasonText：失败时的原因汇总。
        QString rawOutputText; // rawOutputText：脚本原始返回文本，便于诊断。
        bool success = false; // success：本次探测是否读到有效值。
        bool expectedUnavailable = false; // expectedUnavailable：传感器源按系统能力缺失，属于可预期不可用。
    };

#pragma pack(push, 4)
    // CoreTempSharedDataPrefix 作用：
    // - 映射 Core Temp 共享内存结构中长期兼容的原始前缀；
    // - 新版 CoreTempMappingObjectEx 会在该前缀后追加字段，温度读取只需要前缀；
    // - 结构体成员顺序来自 Core Temp 开发者文档，4 字节对齐必须保持一致。
    struct CoreTempSharedDataPrefix
    {
        unsigned int uiLoad[256];      // uiLoad：每线程/核心负载，当前温度读取不使用。
        unsigned int uiTjMax[128];     // uiTjMax：每核心 TjMax，用于 DeltaToTjMax 转换。
        unsigned int uiCoreCnt;        // uiCoreCnt：单 CPU 核心数量。
        unsigned int uiCPUCnt;         // uiCPUCnt：CPU 封装数量。
        float fTemp[256];              // fTemp：每核心温度或到 TjMax 距离。
        float fVID;                    // fVID：Core Temp 上报的 VID 电压。
        float fCPUSpeed;               // fCPUSpeed：CPU 当前频率。
        float fFSBSpeed;               // fFSBSpeed：总线频率。
        float fMultiplier;             // fMultiplier：倍频。
        char sCPUName[100];            // sCPUName：Core Temp 识别到的 CPU 名称。
        unsigned char ucFahrenheit;    // ucFahrenheit：温度是否为华氏度。
        unsigned char ucDeltaToTjMax;  // ucDeltaToTjMax：温度字段是否为到 TjMax 的距离。
    };
#pragma pack(pop)

    // isReadableSensorValue 作用：
    // - 判断传感器文本是否是可展示的有效值；
    // - 统一处理空串和 N/A。
    bool isReadableSensorValue(const QString& sensorValueText)
    {
        const QString trimmedValueText = sensorValueText.trimmed();
        return !trimmedValueText.isEmpty() && trimmedValueText != QStringLiteral("N/A");
    }

    // formatCelsiusSensorValue 作用：
    // - 统一校验并格式化摄氏温度；
    // - 输入 valueCelsius 为摄氏度浮点值；
    // - 返回空串表示越界或 NaN，否则返回带 °C 后缀的展示文本。
    QString formatCelsiusSensorValue(const double valueCelsius)
    {
        if (!std::isfinite(valueCelsius) || valueCelsius < -30.0 || valueCelsius > 130.0)
        {
            return QString();
        }
        return QStringLiteral("%1°C").arg(valueCelsius, 0, 'f', 1);
    }

    // parseSensorProbeOutput 作用：
    // - 解析 PowerShell 返回的 OK|... / ERR|... 协议文本；
    // - 回退兼容旧版“只返回一个值”的简单文本。
    SensorProbeResult parseSensorProbeOutput(const QString& rawOutputText)
    {
        SensorProbeResult probeResult;
        probeResult.rawOutputText = rawOutputText.trimmed();

        const QString firstLineText = probeResult.rawOutputText
            .split('\n', Qt::SkipEmptyParts)
            .value(0)
            .trimmed();
        if (firstLineText.startsWith(QStringLiteral("OK|")))
        {
            const QStringList resultPartList = firstLineText.split('|');
            probeResult.valueText =
                resultPartList.size() >= 2 ? resultPartList.at(1).trimmed() : QStringLiteral("N/A");
            probeResult.sourceText =
                resultPartList.size() >= 3 ? resultPartList.mid(2).join(QStringLiteral("|")).trimmed() : QString();
            probeResult.success = isReadableSensorValue(probeResult.valueText);
            if (!probeResult.success)
            {
                probeResult.reasonText = QStringLiteral("脚本返回成功标记，但值为空。");
            }
            return probeResult;
        }

        if (firstLineText.startsWith(QStringLiteral("ERR|")))
        {
            probeResult.reasonText = firstLineText.mid(4).trimmed();
            if (probeResult.reasonText.isEmpty())
            {
                probeResult.reasonText = QStringLiteral("脚本返回失败标记，但未提供原因。");
            }
            return probeResult;
        }

        if (firstLineText.isEmpty())
        {
            probeResult.reasonText = QStringLiteral("脚本无输出。");
            return probeResult;
        }

        if (firstLineText == QStringLiteral("<无输出>")
            || firstLineText.contains(QStringLiteral("PowerShell"))
            || firstLineText.contains(QStringLiteral("失败"))
            || firstLineText.contains(QStringLiteral("超时")))
        {
            probeResult.reasonText = firstLineText;
            return probeResult;
        }

        probeResult.valueText = firstLineText;
        probeResult.success = isReadableSensorValue(probeResult.valueText);
        if (!probeResult.success)
        {
            probeResult.reasonText = QStringLiteral("脚本仅返回了空值或 N/A。");
        }
        return probeResult;
    }

    // buildSensorProbeSignatureText 作用：
    // - 生成用于日志去重的稳定签名；
    // - 成功时包含来源和值，失败时包含原因。
    QString buildSensorProbeSignatureText(
        const QString& probeNameText,
        const SensorProbeResult& probeResult)
    {
        if (probeResult.success)
        {
            return QStringLiteral("%1:OK:%2:%3")
                .arg(probeNameText)
                .arg(probeResult.sourceText)
                .arg(probeResult.valueText);
        }

        return QStringLiteral("%1:ERR:%2")
            .arg(probeNameText)
            .arg(probeResult.reasonText);
    }

    // buildSensorProbeLogFragment 作用：
    // - 生成单个传感器项的日志片段；
    // - 失败时优先带出原因，成功时带出来源和值。
    QString buildSensorProbeLogFragment(
        const QString& probeNameText,
        const SensorProbeResult& probeResult)
    {
        if (probeResult.success)
        {
            return QStringLiteral("%1=%2，来源=%3")
                .arg(probeNameText)
                .arg(probeResult.valueText)
                .arg(probeResult.sourceText.isEmpty() ? QStringLiteral("未标注") : probeResult.sourceText);
        }

        return QStringLiteral("%1失败，原因=%2")
            .arg(probeNameText)
            .arg(probeResult.reasonText.isEmpty() ? QStringLiteral("未提供原因。") : probeResult.reasonText);
    }

    // sensorReasonContainsAny 作用：
    // - 在探测诊断文本中查找任一特征片段；
    // - 参数 reasonText 为 PowerShell/CIM/Counter 汇总原因；
    // - 参数 markerTextList 为需要匹配的可预期或硬失败关键字；
    // - 返回 true 表示至少命中一个关键字，否则返回 false。
    bool sensorReasonContainsAny(
        const QString& reasonText,
        const QStringList& markerTextList)
    {
        for (const QString& markerText : markerTextList)
        {
            if (!markerText.isEmpty() && reasonText.contains(markerText, Qt::CaseInsensitive))
            {
                return true;
            }
        }
        return false;
    }

    // sensorProbeHasExecutionFailure 作用：
    // - 区分“脚本/权限/进程执行失败”和“硬件传感器源本来不存在”；
    // - 真正执行异常仍需要 WARN，避免把 PowerShell 超时或拒绝访问静默吞掉；
    // - 返回 true 表示应按异常失败处理，false 表示还需继续做可预期不可用判定。
    bool sensorProbeHasExecutionFailure(const SensorProbeResult& probeResult)
    {
        const QString diagnosticText = probeResult.reasonText
            + QStringLiteral("\n")
            + probeResult.rawOutputText;
        static const QStringList hardFailureMarkerList = {
            QStringLiteral("PowerShell启动失败"),
            QStringLiteral("PowerShell执行失败"),
            QStringLiteral("PowerShell执行超时"),
            QStringLiteral("脚本无输出"),
            QStringLiteral("脚本返回成功标记"),
            QStringLiteral("脚本仅返回"),
            QStringLiteral("拒绝访问"),
            QStringLiteral("Access denied"),
            QStringLiteral("RPC")
        };
        return sensorReasonContainsAny(diagnosticText, hardFailureMarkerList);
    }

    // isExpectedCpuTemperatureUnavailable 作用：
    // - 识别 Windows 常见的 CPU 温度不可暴露场景；
    // - Libre/OpenHardwareMonitor 命名空间缺失、ACPI 热区不支持、热区计数器无实例都很常见；
    // - 返回 true 时 UI 继续展示 N/A，但日志不应升级为 WARN。
    bool isExpectedCpuTemperatureUnavailable(const SensorProbeResult& probeResult)
    {
        if (probeResult.success || probeResult.reasonText.isEmpty())
        {
            return false;
        }
        if (sensorProbeHasExecutionFailure(probeResult))
        {
            return false;
        }

        static const QStringList expectedTemperatureMarkerList = {
            QStringLiteral("Core Temp共享内存未打开"),
            QStringLiteral("无效命名空间"),
            QStringLiteral("Invalid namespace"),
            QStringLiteral("不支持"),
            QStringLiteral("Not supported"),
            QStringLiteral("指定的实例不存在"),
            QStringLiteral("does not exist"),
            QStringLiteral("未找到CPU温度传感器"),
            QStringLiteral("无热区数据"),
            QStringLiteral("读取值无效"),
            QStringLiteral("样本值无效"),
            QStringLiteral("无数据"),
            QStringLiteral("传感器存在但值无效"),
            QStringLiteral("热区值超出有效范围"),
            QStringLiteral("未找到可用温度来源")
        };
        return sensorReasonContainsAny(probeResult.reasonText, expectedTemperatureMarkerList);
    }

    // isExpectedCpuVoltageUnavailable 作用：
    // - 识别 Win32_Processor CurrentVoltage 不提供或不可解析的常见情况；
    // - 这些值来自 SMBIOS，很多主板/虚拟化环境不会提供真实核心电压；
    // - 返回 true 时只保留 N/A 展示，不输出误导性的 WARN。
    bool isExpectedCpuVoltageUnavailable(const SensorProbeResult& probeResult)
    {
        if (probeResult.success || probeResult.reasonText.isEmpty())
        {
            return false;
        }
        if (sensorProbeHasExecutionFailure(probeResult))
        {
            return false;
        }

        static const QStringList expectedVoltageMarkerList = {
            QStringLiteral("CurrentVoltage"),
            QStringLiteral("无法解析"),
            QStringLiteral("未返回处理器对象"),
            QStringLiteral("WMIC path Win32_Processor: 无输出")
        };
        return sensorReasonContainsAny(probeResult.reasonText, expectedVoltageMarkerList);
    }

    // queryCoreTempSharedMemoryProbeResult 作用：
    // - 读取 Core Temp 暴露的全局共享内存 CoreTempMappingObject；
    // - CPU-Z/硬件监控类工具通常依赖驱动/MSR，Windows WMI 读不到时可借助此类后端；
    // - 成功时返回当前核心温度最大值，失败时返回结构化原因。
    SensorProbeResult queryCoreTempSharedMemoryProbeResult()
    {
        SensorProbeResult probeResult;
        HANDLE mappingHandle = ::OpenFileMappingW(
            FILE_MAP_READ,
            FALSE,
            L"Global\\CoreTempMappingObjectEx");
        if (mappingHandle == nullptr)
        {
            mappingHandle = ::OpenFileMappingW(
                FILE_MAP_READ,
                FALSE,
                L"CoreTempMappingObjectEx");
        }
        if (mappingHandle == nullptr)
        {
            mappingHandle = ::OpenFileMappingW(
                FILE_MAP_READ,
                FALSE,
                L"Global\\CoreTempMappingObject");
        }
        if (mappingHandle == nullptr)
        {
            mappingHandle = ::OpenFileMappingW(
                FILE_MAP_READ,
                FALSE,
                L"CoreTempMappingObject");
        }
        if (mappingHandle == nullptr)
        {
            probeResult.reasonText = QStringLiteral("Core Temp共享内存未打开。");
            return probeResult;
        }

        const void* mappedViewPointer = ::MapViewOfFile(
            mappingHandle,
            FILE_MAP_READ,
            0,
            0,
            sizeof(CoreTempSharedDataPrefix));
        if (mappedViewPointer == nullptr)
        {
            const DWORD errorCode = ::GetLastError();
            ::CloseHandle(mappingHandle);
            probeResult.reasonText = QStringLiteral("Core Temp共享内存映射失败，Win32错误=%1。")
                .arg(errorCode);
            return probeResult;
        }

        const CoreTempSharedDataPrefix* sharedDataPointer =
            static_cast<const CoreTempSharedDataPrefix*>(mappedViewPointer);
        const unsigned int packageCount = std::clamp(sharedDataPointer->uiCPUCnt, 1U, 128U);
        const unsigned int coreCount = std::clamp(sharedDataPointer->uiCoreCnt, 1U, 256U);
        const unsigned int sampleCount = std::min(256U, std::max(coreCount, packageCount * coreCount));
        double maxTemperatureCelsius = -1000.0;
        for (unsigned int sampleIndex = 0; sampleIndex < sampleCount; ++sampleIndex)
        {
            double valueCelsius = static_cast<double>(sharedDataPointer->fTemp[sampleIndex]);
            if (sharedDataPointer->ucFahrenheit != 0U)
            {
                valueCelsius = (valueCelsius - 32.0) * 5.0 / 9.0;
            }
            if (sharedDataPointer->ucDeltaToTjMax != 0U)
            {
                const unsigned int tjMaxIndex = std::min(sampleIndex, 127U);
                const double tjMaxValue = static_cast<double>(sharedDataPointer->uiTjMax[tjMaxIndex]);
                if (tjMaxValue > 0.0)
                {
                    valueCelsius = tjMaxValue - valueCelsius;
                }
            }
            if (!std::isfinite(valueCelsius) || valueCelsius < -30.0 || valueCelsius > 130.0)
            {
                continue;
            }
            maxTemperatureCelsius = std::max(maxTemperatureCelsius, valueCelsius);
        }

        const QString valueText = formatCelsiusSensorValue(maxTemperatureCelsius);
        if (isReadableSensorValue(valueText))
        {
            probeResult.valueText = valueText;
            probeResult.sourceText = QStringLiteral("Core Temp共享内存 / 核心最高温");
            probeResult.success = true;
        }
        else
        {
            probeResult.reasonText = QStringLiteral("Core Temp共享内存存在，但未得到有效核心温度样本。");
        }

        ::UnmapViewOfFile(mappedViewPointer);
        ::CloseHandle(mappingHandle);
        return probeResult;
    }

    // queryCpuTemperatureProbeResult 作用：
    // - 查询 CPU 温度第一可用值（单位 °C）；
    // - 按“Libre/OpenHardwareMonitor -> CIM/WMI 热区 -> Thermal Counter -> TemperatureProbe”顺序回退；
    // - 失败时返回结构化原因文本。
    SensorProbeResult queryCpuTemperatureProbeResult()
    {
        SensorProbeResult coreTempProbeResult = queryCoreTempSharedMemoryProbeResult();
        if (coreTempProbeResult.success)
        {
            return coreTempProbeResult;
        }

        const QString temperatureScript = QStringLiteral(
            "$ErrorActionPreference='Stop'; "
            "function Add-Reason($list,[string]$reason){ if(-not [string]::IsNullOrWhiteSpace($reason)){ [void]$list.Add($reason) } }; "
            "function Format-Temp([double]$value){ "
            "  if([double]::IsNaN($value) -or [double]::IsInfinity($value)){ return $null }; "
            "  if($value -lt -30 -or $value -gt 130){ return $null }; "
            "  return ([math]::Round($value,1)).ToString() + '°C'; "
            "}; "
            "function Emit-Success([string]$value,[string]$source){ Write-Output ('OK|' + $value + '|' + $source); exit 0 }; "
            "function Test-CpuSensor($sensor){ "
            "  $name=[string]$sensor.Name; $identifier=[string]$sensor.Identifier; $hardwareName=[string]$sensor.HardwareName; "
            "  $text=($name + ' ' + $identifier + ' ' + $hardwareName); "
            "  if($text -match '(?i)cpu|processor|package|core|xeon|intel'){ return $true }; "
            "  if($identifier -match '(?i)/intelcpu|/cpu|/amdcpu'){ return $true }; "
            "  return $false; "
            "}; "
            "$reasons = New-Object 'System.Collections.Generic.List[string]'; "
            "foreach($serviceName in @('LibreHardwareMonitor','OpenHardwareMonitor','CoreTemp','HWiNFO64','HWiNFO32')){ "
            "  try { "
            "    $svc=Get-Service -Name $serviceName -ErrorAction SilentlyContinue; "
            "    if($null -ne $svc){ Add-Reason $reasons ('服务 ' + $serviceName + ': ' + [string]$svc.Status) } "
            "  } catch { } "
            "}; "
            "foreach($ns in @('root/LibreHardwareMonitor','root/OpenHardwareMonitor')){ "
            "  try { "
            "    $sensorRows=@(Get-CimInstance -Namespace $ns -ClassName Sensor -ErrorAction Stop); "
            "    $cpuTemps=@($sensorRows | Where-Object { "
            "      $_.SensorType -eq 'Temperature' -and "
            "      (Test-CpuSensor $_) "
            "    } | Sort-Object @{Expression={if($_.Name -match 'Package|CPU Package'){0}elseif($_.Name -match 'Core'){1}else{2}}}, Name); "
            "    if($cpuTemps.Count -le 0){ Add-Reason $reasons ('CIM ' + $ns + ': 未找到CPU温度传感器'); continue }; "
            "    foreach($sensor in $cpuTemps){ "
            "      $temp=Format-Temp ([double]$sensor.Value); "
            "      if($null -ne $temp){ Emit-Success $temp ('CIM ' + $ns + ' / ' + $sensor.Name) } "
            "    } "
            "    Add-Reason $reasons ('CIM ' + $ns + ': 传感器存在但值无效'); "
            "  } catch { Add-Reason $reasons ('CIM ' + $ns + ': ' + $_.Exception.Message) } "
            "}; "
            "foreach($ns in @('root/CIMV2','root/WMI')){ "
            "  foreach($className in @('Sensor','HardwareMonitor')){ "
            "    try { "
            "      $genericRows=@(Get-CimInstance -Namespace $ns -ClassName $className -ErrorAction Stop); "
            "      $genericTemps=@($genericRows | Where-Object { "
            "        (($_.SensorType -eq 'Temperature') -or ($_.Type -eq 'Temperature') -or ($_.Name -match '(?i)temperature|temp')) -and "
            "        (Test-CpuSensor $_) "
            "      }); "
            "      foreach($sensor in $genericTemps){ "
            "        $rawValue=$null; "
            "        if($null -ne $sensor.Value){ $rawValue=$sensor.Value } elseif($null -ne $sensor.CurrentValue){ $rawValue=$sensor.CurrentValue } elseif($null -ne $sensor.CurrentReading){ $rawValue=$sensor.CurrentReading }; "
            "        if($null -ne $rawValue){ "
            "          $temp=Format-Temp ([double]$rawValue); "
            "          if($null -ne $temp){ Emit-Success $temp ('CIM ' + $ns + ' / ' + $className + ' / ' + [string]$sensor.Name) } "
            "        } "
            "      } "
            "      if($genericRows.Count -gt 0){ Add-Reason $reasons ('CIM ' + $ns + '/' + $className + ': 未找到可用CPU温度值') } "
            "    } catch { } "
            "  } "
            "}; "
            "try { "
            "  $zoneRows=@(Get-CimInstance -Namespace root/wmi -ClassName MSAcpi_ThermalZoneTemperature -ErrorAction Stop); "
            "  if($zoneRows.Count -le 0){ Add-Reason $reasons 'CIM root/wmi: 无热区数据' }; "
            "  foreach($row in $zoneRows){ "
            "    $temp=Format-Temp ((([double]$row.CurrentTemperature)/10.0)-273.15); "
            "    if($null -ne $temp){ Emit-Success $temp 'CIM root/wmi / MSAcpi_ThermalZoneTemperature' } "
            "  } "
            "  Add-Reason $reasons 'CIM root/wmi: 热区值超出有效范围'; "
            "} catch { Add-Reason $reasons ('CIM root/wmi: ' + $_.Exception.Message) } "
            "try { "
            "  $zoneRows=@(Get-WmiObject -Namespace root\\wmi -Class MSAcpi_ThermalZoneTemperature -ErrorAction Stop); "
            "  if($zoneRows.Count -le 0){ Add-Reason $reasons 'WMI root\\\\wmi: 无热区数据' }; "
            "  foreach($row in $zoneRows){ "
            "    $temp=Format-Temp ((([double]$row.CurrentTemperature)/10.0)-273.15); "
            "    if($null -ne $temp){ Emit-Success $temp 'WMI root\\\\wmi / MSAcpi_ThermalZoneTemperature' } "
            "  } "
            "  Add-Reason $reasons 'WMI root\\\\wmi: 热区值超出有效范围'; "
            "} catch { Add-Reason $reasons ('WMI root\\\\wmi: ' + $_.Exception.Message) } "
            "foreach($counterPath in @('\\Thermal Zone Information(*)\\High Precision Temperature','\\Thermal Zone Information(*)\\Temperature')){ "
            "  try { "
            "    $samples=@((Get-Counter $counterPath -ErrorAction Stop).CounterSamples); "
            "    if($samples.Count -le 0){ Add-Reason $reasons ('Counter ' + $counterPath + ': 无实例'); continue }; "
            "    foreach($sample in $samples){ "
            "      $raw=[double]$sample.CookedValue; "
            "      if($raw -gt 200){ $raw=($raw/10.0)-273.15 }; "
            "      $temp=Format-Temp $raw; "
            "      if($null -ne $temp){ Emit-Success $temp ('Counter ' + $sample.Path) } "
            "    } "
            "    Add-Reason $reasons ('Counter ' + $counterPath + ': 样本值无效'); "
            "  } catch { Add-Reason $reasons ('Counter ' + $counterPath + ': ' + $_.Exception.Message) } "
            "}; "
            "try { "
            "  $probeRows=@(Get-CimInstance Win32_TemperatureProbe -ErrorAction Stop); "
            "  if($probeRows.Count -le 0){ Add-Reason $reasons 'CIM Win32_TemperatureProbe: 无数据' }; "
            "  foreach($probe in $probeRows){ "
            "    if($null -eq $probe.CurrentReading){ continue }; "
            "    $temp=Format-Temp ([double]$probe.CurrentReading); "
            "    if($null -ne $temp){ Emit-Success $temp 'CIM Win32_TemperatureProbe / CurrentReading' } "
            "  } "
            "  Add-Reason $reasons 'CIM Win32_TemperatureProbe: 读取值无效'; "
            "} catch { Add-Reason $reasons ('CIM Win32_TemperatureProbe: ' + $_.Exception.Message) } "
            "if($reasons.Count -le 0){ Add-Reason $reasons '未找到可用温度来源' }; "
            "Write-Output ('ERR|' + ($reasons -join ' || '));");
        SensorProbeResult probeResult = parseSensorProbeOutput(queryPowerShellTextSync(temperatureScript, 5200));
        if (!coreTempProbeResult.reasonText.isEmpty())
        {
            probeResult.reasonText = coreTempProbeResult.reasonText
                + QStringLiteral(" || ")
                + probeResult.reasonText;
        }
        if (isExpectedCpuTemperatureUnavailable(probeResult))
        {
            probeResult.expectedUnavailable = true;
            probeResult.reasonText = QStringLiteral(
                "当前系统未暴露CPU温度传感器；已保持N/A。"
                "CPU本身可能有DTS，但Windows WMI通常不直接暴露；"
                "请开启Core Temp共享内存、LibreHardwareMonitor或OpenHardwareMonitor的WMI后端。");
        }
        return probeResult;
    }

    // queryCpuVoltageProbeResult 作用：
    // - 查询 CPU 电压第一可用值（单位 V）；
    // - 同时兼容 SMBIOS 位标志与十倍电压值编码；
    // - 失败时返回结构化原因文本。
    SensorProbeResult queryCpuVoltageProbeResult()
    {
        const QString voltageScript = QStringLiteral(
            "$ErrorActionPreference='Stop'; "
            "function Add-Reason($list,[string]$reason){ if(-not [string]::IsNullOrWhiteSpace($reason)){ [void]$list.Add($reason) } }; "
            "function Format-Voltage([uint16]$raw){ "
            "  if($raw -eq 0){ return $null }; "
            "  if(($raw -band 0x80) -ne 0){ "
            "    $decoded=(($raw -band 0x7F) / 10.0); "
            "    if($decoded -gt 0){ return ([math]::Round($decoded,2)).ToString() + 'V' } "
            "  }; "
            "  if(($raw -band 0x1) -ne 0){ return '5.0V' }; "
            "  if(($raw -band 0x2) -ne 0){ return '3.3V' }; "
            "  if(($raw -band 0x4) -ne 0){ return '2.9V' }; "
            "  return $null; "
            "}; "
            "function Emit-Success([string]$value,[string]$source){ Write-Output ('OK|' + $value + '|' + $source); exit 0 }; "
            "$reasons = New-Object 'System.Collections.Generic.List[string]'; "
            "try { "
            "  $cpu=Get-CimInstance Win32_Processor -ErrorAction Stop | Select-Object -First 1; "
            "  if($null -eq $cpu){ Add-Reason $reasons 'CIM Win32_Processor: 未返回处理器对象' } "
            "  else { "
            "    $voltage=Format-Voltage ([uint16]$cpu.CurrentVoltage); "
            "    if($null -ne $voltage){ Emit-Success $voltage 'CIM Win32_Processor / CurrentVoltage' } "
            "    Add-Reason $reasons ('CIM Win32_Processor: CurrentVoltage=' + [string]$cpu.CurrentVoltage + ' 无法解析'); "
            "  } "
            "} catch { Add-Reason $reasons ('CIM Win32_Processor: ' + $_.Exception.Message) } "
            "try { "
            "  $cpu=Get-WmiObject Win32_Processor -ErrorAction Stop | Select-Object -First 1; "
            "  if($null -eq $cpu){ Add-Reason $reasons 'WMI Win32_Processor: 未返回处理器对象' } "
            "  else { "
            "    $voltage=Format-Voltage ([uint16]$cpu.CurrentVoltage); "
            "    if($null -ne $voltage){ Emit-Success $voltage 'WMI Win32_Processor / CurrentVoltage' } "
            "    Add-Reason $reasons ('WMI Win32_Processor: CurrentVoltage=' + [string]$cpu.CurrentVoltage + ' 无法解析'); "
            "  } "
            "} catch { Add-Reason $reasons ('WMI Win32_Processor: ' + $_.Exception.Message) } "
            "try { "
            "  $wmicText=& wmic.exe path Win32_Processor get CurrentVoltage /value 2>&1 | Out-String; "
            "  if(-not [string]::IsNullOrWhiteSpace($wmicText)){ "
            "    $match=[regex]::Match($wmicText,'CurrentVoltage=(\\d+)'); "
            "    if($match.Success){ "
            "      $voltage=Format-Voltage ([uint16]$match.Groups[1].Value); "
            "      if($null -ne $voltage){ Emit-Success $voltage 'WMIC path Win32_Processor / CurrentVoltage' } "
            "      Add-Reason $reasons ('WMIC path Win32_Processor: CurrentVoltage=' + $match.Groups[1].Value + ' 无法解析'); "
            "    } else { Add-Reason $reasons ('WMIC path Win32_Processor: 返回=' + $wmicText.Trim()) } "
            "  } else { Add-Reason $reasons 'WMIC path Win32_Processor: 无输出' } "
            "} catch { Add-Reason $reasons ('WMIC path Win32_Processor: ' + $_.Exception.Message) } "
            "if($reasons.Count -le 0){ Add-Reason $reasons '未找到可用电压来源' }; "
            "Write-Output ('ERR|' + ($reasons -join ' || '));");
        SensorProbeResult probeResult = parseSensorProbeOutput(queryPowerShellTextSync(voltageScript, 4200));
        if (isExpectedCpuVoltageUnavailable(probeResult))
        {
            probeResult.expectedUnavailable = true;
            probeResult.reasonText = QStringLiteral(
                "当前系统未暴露CPU电压传感器；已保持N/A。"
                "Win32_Processor CurrentVoltage 常由SMBIOS决定，可能不是可读传感器。");
        }
        return probeResult;
    }
}

HardwareDock::HardwareDock(QWidget* parent)
    : QWidget(parent)
{
    // 构造流程日志：便于定位硬件页初始化失败点。
    kLogEvent event;
    info << event << "[HardwareDock] 构造开始。" << eol;
    // 硬件页整体不向 ADS 外层申请最小尺寸，窄面板下由内部图表主动压缩。
    configureCompressibleWidget(this, QSizePolicy::Expanding, QSizePolicy::Expanding);

    initializeUi();
    initializeConnections();

    // 启动阶段先填充占位文本，避免首帧等待 PowerShell 导致窗口卡住。
    m_cachedOverviewStaticText = QStringLiteral("硬件概览加载中，请稍候...");
    m_cachedGpuStaticText = QStringLiteral("显卡信息加载中，请稍候...");
    m_cachedMemoryStaticText = QStringLiteral("内存信息加载中，请稍候...");
    m_cachedSensorText = QStringLiteral("N/A|N/A");
    if (m_cpuModelLabel != nullptr && !m_cpuModelText.isEmpty())
    {
        m_cpuModelLabel->setText(m_cpuModelText);
    }

    m_refreshTimer = new QTimer(this);
    m_refreshTimer->setInterval(1000);
    connect(m_refreshTimer, &QTimer::timeout, this, [this]() {
        refreshAllViews();
    });

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

    if (m_diskPerfQueryHandle != nullptr)
    {
        ::PdhCloseQuery(reinterpret_cast<PDH_HQUERY>(m_diskPerfQueryHandle));
        m_diskPerfQueryHandle = nullptr;
        m_diskReadCounterHandle = nullptr;
        m_diskWriteCounterHandle = nullptr;
    }

    if (m_gpuPerfQueryHandle != nullptr)
    {
        ::PdhCloseQuery(reinterpret_cast<PDH_HQUERY>(m_gpuPerfQueryHandle));
        m_gpuPerfQueryHandle = nullptr;
        m_gpuCounterHandle = nullptr;
    }
}

void HardwareDock::resizeEvent(QResizeEvent* resizeEventPointer)
{
    QWidget::resizeEvent(resizeEventPointer);
    adjustUtilizationChartHeights();
}

void HardwareDock::showEvent(QShowEvent* showEventPointer)
{
    QWidget::showEvent(showEventPointer);

    if (!m_initialSamplingStarted)
    {
        m_initialSamplingStarted = true;
        initializePerformanceCounters();
        refreshCpuTopologyStaticInfo();
        refreshSystemVolumeInfo();
        refreshStaticHardwareTexts(false);
        refreshAllViews();
        requestAsyncStaticInfoRefresh();
        requestAsyncSensorRefresh();
        if (m_refreshTimer != nullptr)
        {
            m_refreshTimer->start();
        }
    }

    // 首次显示阶段分阶段重排，确保滚动区 viewport 高度已经稳定。
    scheduleUtilizationLayoutRefresh();
}

void HardwareDock::initializeUi()
{
    m_rootLayout = new QVBoxLayout(this);
    m_rootLayout->setContentsMargins(4, 4, 4, 4);
    m_rootLayout->setSpacing(6);

    m_sideTabWidget = new QTabWidget(this);
    configureCompressibleWidget(m_sideTabWidget, QSizePolicy::Expanding, QSizePolicy::Expanding);
    m_sideTabWidget->setTabPosition(QTabWidget::West);
    m_rootLayout->addWidget(m_sideTabWidget, 1);

    // 页签优先级：先注册“利用率”，让硬件 Dock 初次打开时默认进入性能监控页。
    initializeUtilizationTab();
    initializeOverviewTab();
    initializeCpuTab();
    initializeGpuTab();
    initializeMemoryTab();
    initializeDiskMonitorTab();
    initializeOtherDevicesTab();

    if (m_sideTabWidget != nullptr)
    {
        connect(
            m_sideTabWidget,
            &QTabWidget::currentChanged,
            this,
            [this](const int tabIndexValue)
            {
                Q_UNUSED(tabIndexValue);
                if (m_sideTabWidget == nullptr || m_utilizationPage == nullptr)
                {
                    return;
                }
                if (m_sideTabWidget->currentWidget() != m_utilizationPage)
                {
                    return;
                }
                // 进入“利用率”总页时刷新高度，修复首次进入 CPU 子页时尚未正确撑开的问题。
                scheduleUtilizationLayoutRefresh();
            });
    }
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
    configureCompressibleWidget(m_utilizationPage, QSizePolicy::Expanding, QSizePolicy::Expanding);
    appendTransparentBackgroundStyle(m_utilizationPage);
    m_utilizationLayout = new QVBoxLayout(m_utilizationPage);
    m_utilizationLayout->setContentsMargins(4, 4, 4, 4);
    m_utilizationLayout->setSpacing(6);

    // 任务管理器风格布局：
    // - 左侧为性能导航卡片列表；
    // - 右侧为详情页堆栈，随左侧选中项切换。
    m_utilizationBodyLayout = new QHBoxLayout();
    m_utilizationBodyLayout->setContentsMargins(0, 0, 0, 0);
    m_utilizationBodyLayout->setSpacing(8);
    m_utilizationLayout->addLayout(m_utilizationBodyLayout, 1);

    m_utilizationSidebarList = new QListWidget(m_utilizationPage);
    m_utilizationSidebarList->setFrameShape(QFrame::NoFrame);
    m_utilizationSidebarList->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    // 设备数量较多时不能继续把缩略卡片压到不可读高度，改为保留卡片高度并允许左侧独立滚动。
    m_utilizationSidebarList->setVerticalScrollBarPolicy(Qt::ScrollBarAsNeeded);
    m_utilizationSidebarList->setVerticalScrollMode(QAbstractItemView::ScrollPerPixel);
    m_utilizationSidebarList->setSelectionMode(QAbstractItemView::SingleSelection);
    m_utilizationSidebarList->setSpacing(2);
    configureCompressibleWidget(m_utilizationSidebarList, QSizePolicy::Preferred, QSizePolicy::Expanding);
    m_utilizationSidebarList->setMinimumWidth(96);
    m_utilizationSidebarList->setMaximumWidth(228);
    m_utilizationSidebarList->setStyleSheet(
        QStringLiteral(
            "QListWidget{border:none;background:transparent;}"
            "QListWidget::item{border:none;padding:0px;margin:0px;}"
            "QListWidget::item:selected{background:transparent;}"));
    appendTransparentBackgroundStyle(m_utilizationSidebarList);
    m_utilizationBodyLayout->addWidget(m_utilizationSidebarList, 0);

    m_utilizationDetailStack = new QStackedWidget(m_utilizationPage);
    configureCompressibleWidget(m_utilizationDetailStack, QSizePolicy::Expanding, QSizePolicy::Expanding);
    appendTransparentBackgroundStyle(m_utilizationDetailStack);
    m_utilizationBodyLayout->addWidget(m_utilizationDetailStack, 1);

    initializeUtilizationCpuSubTab();
    initializeUtilizationMemorySubTab();
    initializeUtilizationDiskSubTab();
    initializeUtilizationNetworkSubTab();
    initializeUtilizationGpuSubTab();
    initializeUtilizationSidebarCards();

    connect(
        m_utilizationSidebarList,
        &QListWidget::currentRowChanged,
        this,
        [this](const int rowIndex)
        {
            syncUtilizationSidebarSelection(rowIndex);
        });

    m_utilizationSidebarList->setCurrentRow(0);
    syncUtilizationSidebarSelection(0);

    m_sideTabWidget->addTab(m_utilizationPage, QStringLiteral("利用率"));
}

void HardwareDock::initializeUtilizationSidebarCards()
{
    if (m_utilizationSidebarList == nullptr)
    {
        return;
    }

    m_cpuNavCard = addUtilizationSidebarCard(
        m_utilizationCpuSubPage,
        QStringLiteral("CPU"),
        QColor(90, 178, 255),
        UtilizationDeviceKind::Cpu,
        -1);
    m_memoryNavCard = addUtilizationSidebarCard(
        m_utilizationMemorySubPage,
        QStringLiteral("内存"),
        QColor(184, 99, 255),
        UtilizationDeviceKind::Memory,
        -1);
    // 磁盘、网卡、GPU 不再注册固定聚合卡片：
    // - 设备发现后由 ensure*UtilizationDevice 动态追加；
    // - 这样多硬盘/多显卡/多网卡会像任务管理器一样各占一个入口。
    m_diskNavCard = nullptr;
    m_networkNavCard = nullptr;
    m_gpuNavCard = nullptr;

    if (m_memoryNavCard != nullptr)
    {
        m_memoryNavCard->setSeriesColors(QColor(184, 99, 255), QColor(79, 195, 247));
    }
}

PerformanceNavCard* HardwareDock::addUtilizationSidebarCard(
    QWidget* detailPage,
    const QString& titleText,
    const QColor& accentColor,
    const UtilizationDeviceKind kind,
    const int deviceIndex)
{
    if (m_utilizationSidebarList == nullptr)
    {
        return nullptr;
    }

    // itemPointer 用途：承载 PerformanceNavCard 的 QListWidget 行。
    QListWidgetItem* itemPointer = new QListWidgetItem();
    // cardPointer 用途：实际绘制任务管理器风格缩略卡片。
    PerformanceNavCard* cardPointer = new PerformanceNavCard(m_utilizationSidebarList);
    cardPointer->setTitleText(titleText);
    cardPointer->setSubtitleText(QStringLiteral("采样中..."));
    cardPointer->setAccentColor(accentColor);
    itemPointer->setSizeHint(cardPointer->sizeHint());
    m_utilizationSidebarList->addItem(itemPointer);
    m_utilizationSidebarList->setItemWidget(itemPointer, cardPointer);

    // navEntry 用途：记录 QListWidget 行号与 QStackedWidget 页面之间的稳定映射。
    UtilizationNavEntry navEntry;
    navEntry.navCard = cardPointer;
    navEntry.detailPage = detailPage;
    navEntry.kind = kind;
    navEntry.deviceIndex = deviceIndex;
    m_utilizationNavEntries.push_back(navEntry);
    return cardPointer;
}

void HardwareDock::syncUtilizationSidebarSelection(const int selectedRowIndex)
{
    if (m_utilizationDetailStack == nullptr)
    {
        return;
    }

    const int pageCount = m_utilizationDetailStack->count();
    if (pageCount <= 0)
    {
        return;
    }

    const int entryCount = static_cast<int>(m_utilizationNavEntries.size());
    const int boundedRowIndex = entryCount > 0
        ? std::clamp(selectedRowIndex, 0, entryCount - 1)
        : std::clamp(selectedRowIndex, 0, pageCount - 1);

    // targetPageIndex 用途：把左侧行号映射到右侧堆栈真实页面索引。
    int targetPageIndex = std::clamp(boundedRowIndex, 0, pageCount - 1);
    if (boundedRowIndex >= 0 && boundedRowIndex < entryCount)
    {
        QWidget* targetPageWidget = m_utilizationNavEntries[static_cast<std::size_t>(boundedRowIndex)].detailPage;
        if (targetPageWidget != nullptr)
        {
            const int resolvedIndex = m_utilizationDetailStack->indexOf(targetPageWidget);
            if (resolvedIndex >= 0)
            {
                targetPageIndex = resolvedIndex;
            }
        }
    }
    m_utilizationDetailStack->setCurrentIndex(targetPageIndex);

    for (int entryIndex = 0; entryIndex < entryCount; ++entryIndex)
    {
        UtilizationNavEntry& entry = m_utilizationNavEntries[static_cast<std::size_t>(entryIndex)];
        if (entry.navCard != nullptr)
        {
            entry.navCard->setSelectedState(entryIndex == boundedRowIndex);
        }
    }

    // 选项切换后立即重算大图高度，避免首帧出现滚动条。
    scheduleUtilizationLayoutRefresh();
}

void HardwareDock::adjustUtilizationChartHeights()
{
    // applyFixedHeightIfChanged 作用：
    // - 仅在目标高度变化时写入最小/最大高度，避免无意义重排触发递归 resize；
    // - widgetPointer：待设置控件；heightValue：目标固定高度（像素）；
    // - 返回行为：无返回值，非法高度直接忽略。
    auto applyFixedHeightIfChanged =
        [](QWidget* widgetPointer, const int heightValue)
        {
            if (widgetPointer == nullptr || heightValue <= 0)
            {
                return;
            }
            if (widgetPointer->minimumHeight() == heightValue
                && widgetPointer->maximumHeight() == heightValue)
            {
                return;
            }
            widgetPointer->setMinimumHeight(heightValue);
            widgetPointer->setMaximumHeight(heightValue);
        };

    // applyMaxHeightIfChanged 作用：
    // - 仅调整最大高度，最小高度保持 0，防止文字区反向撑大父布局；
    // - widgetPointer：目标控件；maxHeightValue：目标最大高度（像素）；
    // - 返回行为：无返回值，非法高度直接忽略。
    auto applyMaxHeightIfChanged =
        [](QWidget* widgetPointer, const int maxHeightValue)
        {
            if (widgetPointer == nullptr || maxHeightValue <= 0)
            {
                return;
            }
            if (widgetPointer->minimumHeight() == 0
                && widgetPointer->maximumHeight() == maxHeightValue)
            {
                return;
            }
            widgetPointer->setMinimumHeight(0);
            widgetPointer->setMaximumHeight(maxHeightValue);
        };

    // ===================== 左侧设备列表：按宽度收缩，按高度滚动 =====================
    if (m_utilizationPage != nullptr && m_utilizationSidebarList != nullptr)
    {
        // pageWidth 用途：根据利用率页实际宽度估算左侧栏宽，窄面板下主动让出图表区域。
        const int pageWidth = std::max(0, m_utilizationPage->contentsRect().width());
        const int sidebarWidth = std::clamp(pageWidth / 4, 96, 228);
        if (m_utilizationSidebarList->minimumWidth() != sidebarWidth
            || m_utilizationSidebarList->maximumWidth() != sidebarWidth)
        {
            m_utilizationSidebarList->setMinimumWidth(sidebarWidth);
            m_utilizationSidebarList->setMaximumWidth(sidebarWidth);
        }

        // cardHeight 用途：保持缩略图最小可读高度；多磁盘/多网卡/GPU 时由列表滚动承接溢出。
        const int cardHeight = 52;
        if (m_utilizationSidebarList->spacing() != 2)
        {
            m_utilizationSidebarList->setSpacing(2);
        }
        for (int rowIndex = 0; rowIndex < m_utilizationSidebarList->count(); ++rowIndex)
        {
            QListWidgetItem* itemPointer = m_utilizationSidebarList->item(rowIndex);
            if (itemPointer == nullptr)
            {
                continue;
            }
            const QSize nextSizeHint(sidebarWidth, cardHeight);
            if (itemPointer->sizeHint() != nextSizeHint)
            {
                itemPointer->setSizeHint(nextSizeHint);
            }
        }
    }

    // ===================== CPU 页：按核心网格动态压缩高度 =====================
    if (m_utilizationCpuSubPage != nullptr
        && m_coreChartHostWidget != nullptr
        && m_coreChartGridLayout != nullptr
        && !m_coreChartEntries.empty())
    {
        // cpuReferenceHeight 用途：稳定页面高度，避免用子控件 sizeHint 反向撑高外层 Dock。
        int cpuReferenceHeight = 0;
        if (m_utilizationDetailStack != nullptr)
        {
            cpuReferenceHeight = m_utilizationDetailStack->contentsRect().height();
        }
        if (cpuReferenceHeight <= 0)
        {
            cpuReferenceHeight = m_utilizationCpuSubPage->contentsRect().height();
        }
        if (cpuReferenceHeight <= 0)
        {
            cpuReferenceHeight = 240;
        }

        const int titleHeight = 72;
        const int headerHeight = std::max(
            m_cpuModelLabel != nullptr ? m_cpuModelLabel->height() : 0,
            titleHeight);
        const int summaryHeight = m_utilizationSummaryLabel != nullptr
            ? m_utilizationSummaryLabel->sizeHint().height()
            : 16;
        const int detailHeight = std::max(
            m_cpuUtilPrimaryDetailLabel != nullptr ? m_cpuUtilPrimaryDetailLabel->sizeHint().height() : 0,
            m_cpuUtilSecondaryDetailLabel != nullptr ? m_cpuUtilSecondaryDetailLabel->sizeHint().height() : 0);
        // availableChartAreaHeight 用途：核心图可用高度；允许极小高度，保证页面整体不冒滚动条。
        const int availableChartAreaHeight = std::max(
            1,
            cpuReferenceHeight - headerHeight - summaryHeight - detailHeight - 42);
        const int gridRows = std::max(1, m_cpuCoreGridRowCount);
        const int gridSpacing = std::max(0, m_coreChartGridLayout->verticalSpacing());
        // cellHeight 用途：每个逻辑处理器小卡片高度；低高度下继续压缩而不是让滚动条接管。
        const int cellHeight = std::max(
            1,
            (availableChartAreaHeight - gridSpacing * (gridRows - 1)) / gridRows);

        for (CoreChartEntry& chartEntry : m_coreChartEntries)
        {
            if (chartEntry.containerWidget != nullptr)
            {
                applyFixedHeightIfChanged(chartEntry.containerWidget, cellHeight);
            }
            if (chartEntry.chartView != nullptr)
            {
                const int titleReserveHeight = chartEntry.titleLabel != nullptr
                    ? std::min(18, std::max(0, chartEntry.titleLabel->sizeHint().height()))
                    : 0;
                const int chartHeight = std::max(1, cellHeight - titleReserveHeight - 10);
                applyFixedHeightIfChanged(chartEntry.chartView, chartHeight);
            }
        }

        const int hostHeight = gridRows * cellHeight + gridSpacing * (gridRows - 1);
        applyFixedHeightIfChanged(m_coreChartHostWidget, hostHeight);
        if (m_coreChartScrollArea != nullptr)
        {
            // CPU 核心图区域固定到可用高度，核心多时压缩单元格，不显示滚动条。
            applyFixedHeightIfChanged(m_coreChartScrollArea, availableChartAreaHeight);
        }

        applyMaxHeightIfChanged(m_cpuUtilPrimaryDetailLabel, std::max(1, m_cpuUtilPrimaryDetailLabel != nullptr ? m_cpuUtilPrimaryDetailLabel->sizeHint().height() : 1));
        applyMaxHeightIfChanged(m_cpuUtilSecondaryDetailLabel, std::max(1, m_cpuUtilSecondaryDetailLabel != nullptr ? m_cpuUtilSecondaryDetailLabel->sizeHint().height() : 1));
    }

    // ===================== 其他页：按页面高度比例压缩主图 =====================
    auto adjustMainChartHeight =
        [](
            QWidget* pageWidget,
            QWidget* chartView,
            const double ratioValue,
            const int minHeightValue,
            const int reserveHeightValue)
        {
            if (pageWidget == nullptr || chartView == nullptr)
            {
                return;
            }
            const int pageHeight = pageWidget->contentsRect().height();
            if (pageHeight <= 0)
            {
                return;
            }
            const int safeMinHeight = std::max(1, minHeightValue);
            const int maxAllowedHeight = std::max(1, pageHeight - reserveHeightValue);
            const int expectedHeight = static_cast<int>(std::round(static_cast<double>(pageHeight) * ratioValue));
            const int finalHeight = std::clamp(expectedHeight, 1, maxAllowedHeight);
            const int boundedHeight = std::min(finalHeight, std::max(safeMinHeight, maxAllowedHeight));
            if (chartView->minimumHeight() != boundedHeight
                || chartView->maximumHeight() != boundedHeight)
            {
                chartView->setMinimumHeight(boundedHeight);
                chartView->setMaximumHeight(boundedHeight);
            }
        };

    adjustMainChartHeight(m_utilizationMemorySubPage, m_memoryCompositionHistoryWidget, 0.36, 24, 116);
    adjustMainChartHeight(m_utilizationDiskSubPage, m_diskUtilChartView, 0.40, 24, 120);
    adjustMainChartHeight(m_utilizationNetworkSubPage, m_networkUtilChartView, 0.40, 24, 120);

    applyMaxHeightIfChanged(m_memoryUtilPrimaryDetailLabel, std::max(1, m_memoryUtilPrimaryDetailLabel != nullptr ? m_memoryUtilPrimaryDetailLabel->sizeHint().height() : 1));
    applyMaxHeightIfChanged(m_memoryUtilSecondaryDetailLabel, std::max(1, m_memoryUtilSecondaryDetailLabel != nullptr ? m_memoryUtilSecondaryDetailLabel->sizeHint().height() : 1));
    applyMaxHeightIfChanged(m_diskUtilDetailLabel, std::max(1, m_diskUtilDetailLabel != nullptr ? m_diskUtilDetailLabel->sizeHint().height() : 1));
    applyMaxHeightIfChanged(m_networkUtilDetailLabel, std::max(1, m_networkUtilDetailLabel != nullptr ? m_networkUtilDetailLabel->sizeHint().height() : 1));
    for (DiskUtilizationDevice& device : m_diskUtilDevices)
    {
        adjustMainChartHeight(device.pageWidget, device.chartView, 0.40, 24, 120);
        if (device.detailLabel != nullptr)
        {
            applyMaxHeightIfChanged(device.detailLabel, std::max(1, device.detailLabel->sizeHint().height()));
        }
    }
    for (NetworkUtilizationDevice& device : m_networkUtilDevices)
    {
        adjustMainChartHeight(device.pageWidget, device.chartView, 0.40, 24, 120);
        if (device.detailLabel != nullptr)
        {
            applyMaxHeightIfChanged(device.detailLabel, std::max(1, device.detailLabel->sizeHint().height()));
        }
    }

    // GPU 页：四个引擎图 + 两条显存曲线全部动态压缩。
    if (m_utilizationGpuSubPage != nullptr)
    {
        // gpuReferenceHeight 用途：GPU 子页布局参考高度，优先使用堆栈可见区域，避免自反馈增高。
        int gpuReferenceHeight = 0;
        if (m_utilizationDetailStack != nullptr)
        {
            gpuReferenceHeight = m_utilizationDetailStack->contentsRect().height();
        }
        if (gpuReferenceHeight <= 0)
        {
            gpuReferenceHeight = m_utilizationGpuSubPage->contentsRect().height();
        }
        if (gpuReferenceHeight <= 0)
        {
            gpuReferenceHeight = 320;
        }

        const int titleHeight = std::max(
            m_gpuAdapterTitleLabel != nullptr ? m_gpuAdapterTitleLabel->sizeHint().height() : 0,
            58);
        const int summaryHeight = m_gpuUtilSummaryLabel != nullptr
            ? m_gpuUtilSummaryLabel->sizeHint().height()
            : 20;
        const int detailHeight = m_gpuUtilDetailLabel != nullptr
            ? m_gpuUtilDetailLabel->sizeHint().height()
            : 22;
        // reservedHeight 用途：GPU 页非图表区预留高度（含布局间距与上下边距）。
        const int reservedHeight = titleHeight + summaryHeight + detailHeight + 38;
        const int availableHeight = std::max(1, gpuReferenceHeight - reservedHeight);

        // engineAreaHeight 用途：分配给 2x2 引擎图区域的高度。
        const int engineAreaHeight = std::max(
            1,
            static_cast<int>(std::round(static_cast<double>(availableHeight) * 0.52)));
        const int memoryAreaEachHeight = std::max(1, (availableHeight - engineAreaHeight - 8) / 2);
        if (m_gpuEngineHostWidget != nullptr && m_gpuEngineGridLayout != nullptr)
        {
            const int rowSpacing = std::max(0, m_gpuEngineGridLayout->verticalSpacing());
            const int cellHeight = std::max(1, (engineAreaHeight - rowSpacing) / 2);
            for (GpuEngineChartEntry& chartEntry : m_gpuEngineCharts)
            {
                if (chartEntry.chartView != nullptr)
                {
                    applyMaxHeightIfChanged(chartEntry.chartView, std::max(1, cellHeight - 10));
                }
                if (chartEntry.titleLabel != nullptr)
                {
                    chartEntry.titleLabel->setMinimumHeight(0);
                    chartEntry.titleLabel->setMaximumHeight(18);
                }
            }
            applyMaxHeightIfChanged(m_gpuEngineHostWidget, engineAreaHeight);
        }

        if (m_gpuDedicatedMemoryChartView != nullptr)
        {
            applyMaxHeightIfChanged(m_gpuDedicatedMemoryChartView, memoryAreaEachHeight);
        }
        if (m_gpuSharedMemoryChartView != nullptr)
        {
            applyMaxHeightIfChanged(m_gpuSharedMemoryChartView, memoryAreaEachHeight);
        }
        if (m_gpuUtilDetailLabel != nullptr)
        {
            applyMaxHeightIfChanged(m_gpuUtilDetailLabel, std::max(1, m_gpuUtilDetailLabel->sizeHint().height()));
        }
    }
    for (GpuUtilizationDevice& device : m_gpuUtilDevices)
    {
        if (device.pageWidget == nullptr)
        {
            continue;
        }

        // gpuReferenceHeight 用途：多 GPU 子页的当前稳定高度。
        int gpuReferenceHeight = 0;
        if (m_utilizationDetailStack != nullptr)
        {
            gpuReferenceHeight = m_utilizationDetailStack->contentsRect().height();
        }
        if (gpuReferenceHeight <= 0)
        {
            gpuReferenceHeight = device.pageWidget->contentsRect().height();
        }
        if (gpuReferenceHeight <= 0)
        {
            continue;
        }

        const int layoutSpacing = 6;
        const int headerHeight = 58;
        const int summaryHeight = device.summaryLabel != nullptr
            ? device.summaryLabel->sizeHint().height()
            : 0;
        const int detailHeight = device.detailLabel != nullptr
            ? device.detailLabel->sizeHint().height()
            : 0;
        const int reservedHeight = headerHeight + summaryHeight + detailHeight + layoutSpacing * 7 + 12;
        const int graphAreaHeight = std::max(1, gpuReferenceHeight - reservedHeight);
        const int engineAreaHeight = std::max(1, graphAreaHeight / 2);
        const int memoryAreaEachHeight = std::max(1, graphAreaHeight / 4);

        if (device.engineHostWidget != nullptr && device.engineGridLayout != nullptr)
        {
            const int rowSpacing = std::max(0, device.engineGridLayout->verticalSpacing());
            const int cellHeight = std::max(1, (engineAreaHeight - rowSpacing) / 2);
            for (GpuEngineChartEntry& chartEntry : device.engineCharts)
            {
                if (chartEntry.chartView != nullptr)
                {
                    applyMaxHeightIfChanged(chartEntry.chartView, std::max(1, cellHeight - 14));
                }
                if (chartEntry.titleLabel != nullptr)
                {
                    chartEntry.titleLabel->setMinimumHeight(0);
                    chartEntry.titleLabel->setMaximumHeight(18);
                }
            }
            applyMaxHeightIfChanged(device.engineHostWidget, engineAreaHeight);
        }
        applyMaxHeightIfChanged(device.dedicatedMemoryChartView, memoryAreaEachHeight);
        applyMaxHeightIfChanged(device.sharedMemoryChartView, memoryAreaEachHeight);
        if (device.detailLabel != nullptr)
        {
            applyMaxHeightIfChanged(device.detailLabel, std::max(1, device.detailLabel->sizeHint().height()));
        }
    }
}

void HardwareDock::initializeUtilizationCpuSubTab()
{
    m_utilizationCpuSubPage = new QWidget(m_utilizationDetailStack);
    configureCompressibleWidget(m_utilizationCpuSubPage, QSizePolicy::Expanding, QSizePolicy::Expanding);
    appendTransparentBackgroundStyle(m_utilizationCpuSubPage);
    QVBoxLayout* cpuSubLayout = new QVBoxLayout(m_utilizationCpuSubPage);
    cpuSubLayout->setContentsMargins(4, 4, 4, 4);
    cpuSubLayout->setSpacing(6);

    QHBoxLayout* headerLayout = new QHBoxLayout();
    QLabel* titleLabel = new QLabel(QStringLiteral("CPU"), m_utilizationCpuSubPage);
    configurePersistentHeaderLabel(titleLabel);
    titleLabel->setStyleSheet(
        QStringLiteral("font-size:46px;font-weight:700;color:%1;")
        .arg(KswordTheme::TextPrimaryHex()));
    lockLabelHeightToFont(titleLabel, 14);
    m_cpuModelLabel = new QLabel(QStringLiteral("检测中..."), m_utilizationCpuSubPage);
    configurePersistentHeaderLabel(m_cpuModelLabel, QSizePolicy::Ignored);
    m_cpuModelLabel->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
    m_cpuModelLabel->setStyleSheet(
        QStringLiteral("font-size:15px;font-weight:500;color:%1;")
        .arg(KswordTheme::TextPrimaryHex()));
    lockLabelHeightToFont(m_cpuModelLabel, 6);
    headerLayout->addWidget(titleLabel, 0);
    headerLayout->addStretch(1);
    headerLayout->addWidget(m_cpuModelLabel, 0);
    cpuSubLayout->addLayout(headerLayout, 0);

    m_utilizationSummaryLabel = new QLabel(QStringLiteral("30 秒内的利用率 %"), m_utilizationCpuSubPage);
    configureCompressibleLabel(m_utilizationSummaryLabel);
    m_utilizationSummaryLabel->setStyleSheet(
        QStringLiteral("color:%1;font-size:14px;font-weight:600;").arg(buildStatusColor().name()));
    cpuSubLayout->addWidget(m_utilizationSummaryLabel, 0);

    m_coreChartScrollArea = new QScrollArea(m_utilizationCpuSubPage);
    m_coreChartScrollArea->setWidgetResizable(true);
    m_coreChartScrollArea->setFrameShape(QFrame::NoFrame);
    // 核心数量很多时压缩网格，不显示内部滚动条。
    m_coreChartScrollArea->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    m_coreChartScrollArea->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    m_coreChartScrollArea->setSizeAdjustPolicy(QAbstractScrollArea::AdjustIgnored);
    configureCompressibleWidget(m_coreChartScrollArea, QSizePolicy::Expanding, QSizePolicy::Expanding);
    appendTransparentBackgroundStyle(m_coreChartScrollArea);
    m_coreChartHostWidget = new QWidget(m_coreChartScrollArea);
    configureCompressibleWidget(m_coreChartHostWidget, QSizePolicy::Expanding, QSizePolicy::Expanding);
    appendTransparentBackgroundStyle(m_coreChartHostWidget);
    m_coreChartGridLayout = new QGridLayout(m_coreChartHostWidget);
    m_coreChartGridLayout->setContentsMargins(0, 0, 0, 0);
    m_coreChartGridLayout->setHorizontalSpacing(6);
    m_coreChartGridLayout->setVerticalSpacing(6);
    m_coreChartScrollArea->setWidget(m_coreChartHostWidget);
    cpuSubLayout->addWidget(m_coreChartScrollArea, 1);

    QHBoxLayout* detailLayout = new QHBoxLayout();
    detailLayout->setSpacing(16);
    m_cpuUtilPrimaryDetailLabel = new QLabel(QStringLiteral("CPU 详情采样中..."), m_utilizationCpuSubPage);
    m_cpuUtilSecondaryDetailLabel = new QLabel(QStringLiteral("硬件参数读取中..."), m_utilizationCpuSubPage);
    configureCompressibleLabel(m_cpuUtilPrimaryDetailLabel);
    configureCompressibleLabel(m_cpuUtilSecondaryDetailLabel);
    m_cpuUtilPrimaryDetailLabel->setWordWrap(false);
    m_cpuUtilSecondaryDetailLabel->setWordWrap(false);
    m_cpuUtilPrimaryDetailLabel->setStyleSheet(
        QStringLiteral("font-size:14px;color:%1;").arg(KswordTheme::TextPrimaryHex()));
    m_cpuUtilSecondaryDetailLabel->setStyleSheet(
        QStringLiteral("font-size:14px;color:%1;").arg(KswordTheme::TextPrimaryHex()));
    detailLayout->addWidget(m_cpuUtilPrimaryDetailLabel, 1);
    detailLayout->addWidget(m_cpuUtilSecondaryDetailLabel, 1);
    cpuSubLayout->addLayout(detailLayout, 0);

    initializeCoreCharts();
    m_utilizationDetailStack->addWidget(m_utilizationCpuSubPage);
}

void HardwareDock::initializeUtilizationMemorySubTab()
{
    m_utilizationMemorySubPage = new QWidget(m_utilizationDetailStack);
    configureCompressibleWidget(m_utilizationMemorySubPage, QSizePolicy::Expanding, QSizePolicy::Expanding);
    appendTransparentBackgroundStyle(m_utilizationMemorySubPage);
    QVBoxLayout* memorySubLayout = new QVBoxLayout(m_utilizationMemorySubPage);
    memorySubLayout->setContentsMargins(4, 4, 4, 4);
    memorySubLayout->setSpacing(6);

    QHBoxLayout* headerLayout = new QHBoxLayout();
    QLabel* titleLabel = new QLabel(QStringLiteral("内存"), m_utilizationMemorySubPage);
    configurePersistentHeaderLabel(titleLabel);
    titleLabel->setStyleSheet(
        QStringLiteral("font-size:46px;font-weight:700;color:%1;")
        .arg(KswordTheme::TextPrimaryHex()));
    lockLabelHeightToFont(titleLabel, 14);
    m_memoryCapacityLabel = new QLabel(QStringLiteral("读取中..."), m_utilizationMemorySubPage);
    configurePersistentHeaderLabel(m_memoryCapacityLabel, QSizePolicy::Ignored);
    m_memoryCapacityLabel->setStyleSheet(
        QStringLiteral("font-size:31px;font-weight:500;color:%1;")
        .arg(KswordTheme::TextPrimaryHex()));
    lockLabelHeightToFont(m_memoryCapacityLabel, 8);
    headerLayout->addWidget(titleLabel, 0);
    headerLayout->addStretch(1);
    headerLayout->addWidget(m_memoryCapacityLabel, 0);
    memorySubLayout->addLayout(headerLayout, 0);

    m_memoryUtilSummaryLabel = new QLabel(QStringLiteral("内存使用量"), m_utilizationMemorySubPage);
    configureCompressibleLabel(m_memoryUtilSummaryLabel);
    m_memoryUtilSummaryLabel->setStyleSheet(
        QStringLiteral("color:%1;font-size:14px;font-weight:600;").arg(buildStatusColor().name()));
    memorySubLayout->addWidget(m_memoryUtilSummaryLabel, 0);

    m_memoryCompositionHistoryWidget = new MemoryCompositionHistoryWidget(m_utilizationMemorySubPage);
    configureCompressibleWidget(m_memoryCompositionHistoryWidget, QSizePolicy::Expanding, QSizePolicy::Expanding);
    memorySubLayout->addWidget(m_memoryCompositionHistoryWidget, 1);

    QHBoxLayout* detailLayout = new QHBoxLayout();
    detailLayout->setSpacing(16);
    m_memoryUtilPrimaryDetailLabel = new QLabel(QStringLiteral("内存参数采样中..."), m_utilizationMemorySubPage);
    m_memoryUtilSecondaryDetailLabel = new QLabel(QStringLiteral("硬件参数读取中..."), m_utilizationMemorySubPage);
    configureCompressibleLabel(m_memoryUtilPrimaryDetailLabel);
    configureCompressibleLabel(m_memoryUtilSecondaryDetailLabel);
    m_memoryUtilPrimaryDetailLabel->setWordWrap(false);
    m_memoryUtilSecondaryDetailLabel->setWordWrap(false);
    m_memoryUtilPrimaryDetailLabel->setStyleSheet(
        QStringLiteral("font-size:14px;color:%1;").arg(KswordTheme::TextPrimaryHex()));
    m_memoryUtilSecondaryDetailLabel->setStyleSheet(
        QStringLiteral("font-size:14px;color:%1;").arg(KswordTheme::TextPrimaryHex()));
    detailLayout->addWidget(m_memoryUtilPrimaryDetailLabel, 1);
    detailLayout->addWidget(m_memoryUtilSecondaryDetailLabel, 1);
    memorySubLayout->addLayout(detailLayout, 0);

    m_utilizationDetailStack->addWidget(m_utilizationMemorySubPage);
}

void HardwareDock::initializeUtilizationDiskSubTab()
{
    m_utilizationDiskSubPage = new QWidget(m_utilizationDetailStack);
    configureCompressibleWidget(m_utilizationDiskSubPage, QSizePolicy::Expanding, QSizePolicy::Expanding);
    appendTransparentBackgroundStyle(m_utilizationDiskSubPage);
    QVBoxLayout* diskSubLayout = new QVBoxLayout(m_utilizationDiskSubPage);
    diskSubLayout->setContentsMargins(4, 4, 4, 4);
    diskSubLayout->setSpacing(6);

    QLabel* titleLabel = new QLabel(QStringLiteral("磁盘"), m_utilizationDiskSubPage);
    configurePersistentHeaderLabel(titleLabel);
    titleLabel->setStyleSheet(
        QStringLiteral("font-size:46px;font-weight:700;color:%1;")
        .arg(KswordTheme::TextPrimaryHex()));
    lockLabelHeightToFont(titleLabel, 14);
    diskSubLayout->addWidget(titleLabel, 0);

    m_diskUtilSummaryLabel = new QLabel(QStringLiteral("磁盘采样初始化中..."), m_utilizationDiskSubPage);
    configureCompressibleLabel(m_diskUtilSummaryLabel);
    m_diskUtilSummaryLabel->setStyleSheet(
        QStringLiteral("color:%1;font-size:14px;font-weight:600;").arg(buildStatusColor().name()));
    diskSubLayout->addWidget(m_diskUtilSummaryLabel, 0);

    m_diskReadLineSeries = new QLineSeries(m_utilizationDiskSubPage);
    m_diskReadLineSeries->setName(QStringLiteral("读取"));
    const QColor diskReadColor(80, 170, 255);
    const QColor diskWriteColor(255, 190, 105);
    m_diskReadLineSeries->setColor(diskReadColor);
    m_diskReadBaselineSeries = createBaselineSeries(m_utilizationDiskSubPage, m_historyLength);
    m_diskWriteLineSeries = new QLineSeries(m_utilizationDiskSubPage);
    m_diskWriteLineSeries->setName(QStringLiteral("写入"));
    m_diskWriteLineSeries->setColor(diskWriteColor);
    m_diskWriteBaselineSeries = createBaselineSeries(m_utilizationDiskSubPage, m_historyLength);
    initializeLineSeriesHistory(m_diskReadLineSeries, m_historyLength);
    initializeLineSeriesHistory(m_diskWriteLineSeries, m_historyLength);

    QChart* diskChart = new QChart();
    m_diskReadAreaSeries = addFilledAreaSeries(
        diskChart,
        m_diskReadLineSeries,
        m_diskReadBaselineSeries,
        diskReadColor,
        42);
    m_diskWriteAreaSeries = addFilledAreaSeries(
        diskChart,
        m_diskWriteLineSeries,
        m_diskWriteBaselineSeries,
        diskWriteColor,
        34);
    configureUtilizationPlotChart(
        diskChart,
        diskReadColor,
        QStringLiteral("磁盘读写速率趋势"),
        true);

    m_diskUtilAxisX = new QValueAxis(diskChart);
    configureUtilizationValueAxis(m_diskUtilAxisX, diskReadColor, 0.0, static_cast<double>(m_historyLength));

    m_diskUtilAxisY = new QValueAxis(diskChart);
    configureUtilizationValueAxis(m_diskUtilAxisY, diskReadColor, 0.0, 1.0);

    diskChart->addAxis(m_diskUtilAxisX, Qt::AlignBottom);
    diskChart->addAxis(m_diskUtilAxisY, Qt::AlignLeft);
    if (m_diskReadAreaSeries != nullptr)
    {
        m_diskReadAreaSeries->attachAxis(m_diskUtilAxisX);
        m_diskReadAreaSeries->attachAxis(m_diskUtilAxisY);
    }
    if (m_diskWriteAreaSeries != nullptr)
    {
        m_diskWriteAreaSeries->attachAxis(m_diskUtilAxisX);
        m_diskWriteAreaSeries->attachAxis(m_diskUtilAxisY);
    }

    m_diskUtilChartView = createPlotBackgroundChartView(diskChart, m_utilizationDiskSubPage);
    diskSubLayout->addWidget(m_diskUtilChartView, 1);

    m_diskUtilDetailLabel = new QLabel(QStringLiteral("磁盘参数采样中..."), m_utilizationDiskSubPage);
    configureCompressibleLabel(m_diskUtilDetailLabel);
    m_diskUtilDetailLabel->setWordWrap(false);
    m_diskUtilDetailLabel->setStyleSheet(
        QStringLiteral("font-size:14px;color:%1;").arg(KswordTheme::TextPrimaryHex()));
    diskSubLayout->addWidget(m_diskUtilDetailLabel, 0);

    m_utilizationDetailStack->addWidget(m_utilizationDiskSubPage);
}

void HardwareDock::initializeUtilizationNetworkSubTab()
{
    m_utilizationNetworkSubPage = new QWidget(m_utilizationDetailStack);
    configureCompressibleWidget(m_utilizationNetworkSubPage, QSizePolicy::Expanding, QSizePolicy::Expanding);
    appendTransparentBackgroundStyle(m_utilizationNetworkSubPage);
    QVBoxLayout* networkSubLayout = new QVBoxLayout(m_utilizationNetworkSubPage);
    networkSubLayout->setContentsMargins(4, 4, 4, 4);
    networkSubLayout->setSpacing(6);

    QLabel* titleLabel = new QLabel(QStringLiteral("以太网"), m_utilizationNetworkSubPage);
    configurePersistentHeaderLabel(titleLabel);
    titleLabel->setStyleSheet(
        QStringLiteral("font-size:46px;font-weight:700;color:%1;")
        .arg(KswordTheme::TextPrimaryHex()));
    lockLabelHeightToFont(titleLabel, 14);
    networkSubLayout->addWidget(titleLabel, 0);

    m_networkUtilSummaryLabel = new QLabel(QStringLiteral("网络采样初始化中..."), m_utilizationNetworkSubPage);
    configureCompressibleLabel(m_networkUtilSummaryLabel);
    m_networkUtilSummaryLabel->setStyleSheet(
        QStringLiteral("color:%1;font-size:14px;font-weight:600;").arg(buildStatusColor().name()));
    networkSubLayout->addWidget(m_networkUtilSummaryLabel, 0);

    m_networkRxLineSeries = new QLineSeries(m_utilizationNetworkSubPage);
    m_networkRxLineSeries->setName(QStringLiteral("下行"));
    const QColor networkRxColor(92, 190, 255);
    const QColor networkTxColor(153, 129, 255);
    m_networkRxLineSeries->setColor(networkRxColor);
    m_networkRxBaselineSeries = createBaselineSeries(m_utilizationNetworkSubPage, m_historyLength);
    m_networkTxLineSeries = new QLineSeries(m_utilizationNetworkSubPage);
    m_networkTxLineSeries->setName(QStringLiteral("上行"));
    m_networkTxLineSeries->setColor(networkTxColor);
    m_networkTxBaselineSeries = createBaselineSeries(m_utilizationNetworkSubPage, m_historyLength);
    initializeLineSeriesHistory(m_networkRxLineSeries, m_historyLength);
    initializeLineSeriesHistory(m_networkTxLineSeries, m_historyLength);

    QChart* networkChart = new QChart();
    m_networkRxAreaSeries = addFilledAreaSeries(
        networkChart,
        m_networkRxLineSeries,
        m_networkRxBaselineSeries,
        networkRxColor,
        42);
    m_networkTxAreaSeries = addFilledAreaSeries(
        networkChart,
        m_networkTxLineSeries,
        m_networkTxBaselineSeries,
        networkTxColor,
        34);
    configureUtilizationPlotChart(
        networkChart,
        networkRxColor,
        QStringLiteral("网络收发速率趋势"),
        true);

    m_networkUtilAxisX = new QValueAxis(networkChart);
    configureUtilizationValueAxis(m_networkUtilAxisX, networkRxColor, 0.0, static_cast<double>(m_historyLength));

    m_networkUtilAxisY = new QValueAxis(networkChart);
    configureUtilizationValueAxis(m_networkUtilAxisY, networkRxColor, 0.0, 1.0);

    networkChart->addAxis(m_networkUtilAxisX, Qt::AlignBottom);
    networkChart->addAxis(m_networkUtilAxisY, Qt::AlignLeft);
    if (m_networkRxAreaSeries != nullptr)
    {
        m_networkRxAreaSeries->attachAxis(m_networkUtilAxisX);
        m_networkRxAreaSeries->attachAxis(m_networkUtilAxisY);
    }
    if (m_networkTxAreaSeries != nullptr)
    {
        m_networkTxAreaSeries->attachAxis(m_networkUtilAxisX);
        m_networkTxAreaSeries->attachAxis(m_networkUtilAxisY);
    }

    m_networkUtilChartView = createPlotBackgroundChartView(networkChart, m_utilizationNetworkSubPage);
    networkSubLayout->addWidget(m_networkUtilChartView, 1);

    m_networkUtilDetailLabel = new QLabel(QStringLiteral("网络参数采样中..."), m_utilizationNetworkSubPage);
    configureCompressibleLabel(m_networkUtilDetailLabel);
    m_networkUtilDetailLabel->setWordWrap(false);
    m_networkUtilDetailLabel->setStyleSheet(
        QStringLiteral("font-size:14px;color:%1;").arg(KswordTheme::TextPrimaryHex()));
    networkSubLayout->addWidget(m_networkUtilDetailLabel, 0);

    m_utilizationDetailStack->addWidget(m_utilizationNetworkSubPage);
}

void HardwareDock::initializeUtilizationGpuSubTab()
{
    m_utilizationGpuSubPage = new QWidget(m_utilizationDetailStack);
    configureCompressibleWidget(m_utilizationGpuSubPage, QSizePolicy::Expanding, QSizePolicy::Expanding);
    appendTransparentBackgroundStyle(m_utilizationGpuSubPage);
    QVBoxLayout* gpuSubLayout = new QVBoxLayout(m_utilizationGpuSubPage);
    gpuSubLayout->setContentsMargins(4, 4, 4, 4);
    gpuSubLayout->setSpacing(6);

    QHBoxLayout* headerLayout = new QHBoxLayout();
    QLabel* titleLabel = new QLabel(QStringLiteral("GPU"), m_utilizationGpuSubPage);
    configurePersistentHeaderLabel(titleLabel);
    titleLabel->setStyleSheet(
        QStringLiteral("font-size:46px;font-weight:700;color:%1;")
        .arg(KswordTheme::TextPrimaryHex()));
    lockLabelHeightToFont(titleLabel, 14);
    m_gpuAdapterTitleLabel = new QLabel(QStringLiteral("适配器读取中..."), m_utilizationGpuSubPage);
    configurePersistentHeaderLabel(m_gpuAdapterTitleLabel, QSizePolicy::Ignored);
    m_gpuAdapterTitleLabel->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
    m_gpuAdapterTitleLabel->setStyleSheet(
        QStringLiteral("font-size:18px;font-weight:500;color:%1;")
        .arg(KswordTheme::TextPrimaryHex()));
    lockLabelHeightToFont(m_gpuAdapterTitleLabel, 6);
    headerLayout->addWidget(titleLabel, 0);
    headerLayout->addStretch(1);
    headerLayout->addWidget(m_gpuAdapterTitleLabel, 0);
    gpuSubLayout->addLayout(headerLayout, 0);

    m_gpuUtilSummaryLabel = new QLabel(QStringLiteral("GPU采样初始化中..."), m_utilizationGpuSubPage);
    configureCompressibleLabel(m_gpuUtilSummaryLabel);
    m_gpuUtilSummaryLabel->setStyleSheet(
        QStringLiteral("color:%1;font-size:14px;font-weight:600;").arg(buildStatusColor().name()));
    gpuSubLayout->addWidget(m_gpuUtilSummaryLabel, 0);

    // GPU 引擎四宫格：
    // - 对齐任务管理器的 3D / Copy / Video Encode / Video Decode；
    // - 每个引擎独立曲线和标题，便于定位瓶颈引擎。
    m_gpuEngineHostWidget = new QWidget(m_utilizationGpuSubPage);
    configureCompressibleWidget(m_gpuEngineHostWidget, QSizePolicy::Expanding, QSizePolicy::Expanding);
    appendTransparentBackgroundStyle(m_gpuEngineHostWidget);
    m_gpuEngineGridLayout = new QGridLayout(m_gpuEngineHostWidget);
    m_gpuEngineGridLayout->setContentsMargins(0, 0, 0, 0);
    m_gpuEngineGridLayout->setHorizontalSpacing(6);
    m_gpuEngineGridLayout->setVerticalSpacing(6);
    m_gpuEngineCharts.clear();

    auto addGpuEngineChart =
        [this](const QString& engineKeyText, const QString& displayNameText, const QColor& lineColor, const int rowIndex, const int columnIndex)
        {
            QWidget* cellWidget = new QWidget(m_gpuEngineHostWidget);
            configureCompressibleWidget(cellWidget, QSizePolicy::Expanding, QSizePolicy::Expanding);
            appendTransparentBackgroundStyle(cellWidget);
            QVBoxLayout* cellLayout = new QVBoxLayout(cellWidget);
            cellLayout->setContentsMargins(0, 0, 0, 0);
            cellLayout->setSpacing(2);

            QLabel* cellTitle = new QLabel(displayNameText, cellWidget);
            configureCompressibleLabel(cellTitle);
            cellTitle->setStyleSheet(
                QStringLiteral("font-size:14px;font-weight:600;color:%1;")
                .arg(KswordTheme::TextPrimaryHex()));
            cellLayout->addWidget(cellTitle, 0);

            QLineSeries* lineSeries = new QLineSeries(cellWidget);
            lineSeries->setColor(lineColor);
            QLineSeries* baselineSeries = createBaselineSeries(cellWidget, m_historyLength);
            initializeLineSeriesHistory(lineSeries, m_historyLength);

            QChart* chartPointer = new QChart();
            QAreaSeries* areaSeries = addFilledAreaSeries(
                chartPointer,
                lineSeries,
                baselineSeries,
                lineColor,
                44);
            configureUtilizationPlotChart(chartPointer, lineColor);

            QValueAxis* axisX = new QValueAxis(chartPointer);
            configureUtilizationValueAxis(axisX, lineColor, 0.0, static_cast<double>(m_historyLength));

            QValueAxis* axisY = new QValueAxis(chartPointer);
            configureUtilizationValueAxis(axisY, lineColor, 0.0, 100.0);

            chartPointer->addAxis(axisX, Qt::AlignBottom);
            chartPointer->addAxis(axisY, Qt::AlignLeft);
            if (areaSeries != nullptr)
            {
                areaSeries->attachAxis(axisX);
                areaSeries->attachAxis(axisY);
            }

            QChartView* chartView = createPlotBackgroundChartView(chartPointer, cellWidget);
            cellLayout->addWidget(chartView, 1);
            m_gpuEngineGridLayout->addWidget(cellWidget, rowIndex, columnIndex);

            GpuEngineChartEntry chartEntry;
            chartEntry.engineKeyText = engineKeyText;
            chartEntry.displayNameText = displayNameText;
            chartEntry.titleLabel = cellTitle;
            chartEntry.chartView = chartView;
            chartEntry.lineSeries = lineSeries;
            chartEntry.baselineSeries = baselineSeries;
            chartEntry.areaSeries = areaSeries;
            chartEntry.axisX = axisX;
            chartEntry.axisY = axisY;
            m_gpuEngineCharts.push_back(chartEntry);
        };

    addGpuEngineChart(QStringLiteral("3d"), QStringLiteral("3D"), QColor(105, 173, 255), 0, 0);
    addGpuEngineChart(QStringLiteral("copy"), QStringLiteral("Copy"), QColor(110, 196, 247), 0, 1);
    addGpuEngineChart(QStringLiteral("video_encode"), QStringLiteral("Video Encode"), QColor(125, 184, 255), 1, 0);
    addGpuEngineChart(QStringLiteral("video_decode"), QStringLiteral("Video Decode"), QColor(137, 178, 255), 1, 1);
    gpuSubLayout->addWidget(m_gpuEngineHostWidget, 1);

    // 显存曲线：专用显存 + 共享显存。
    m_gpuDedicatedMemoryLineSeries = new QLineSeries(m_utilizationGpuSubPage);
    const QColor gpuDedicatedMemoryColor(92, 167, 255);
    const QColor gpuSharedMemoryColor(113, 185, 255);
    m_gpuDedicatedMemoryLineSeries->setColor(gpuDedicatedMemoryColor);
    m_gpuDedicatedMemoryBaselineSeries = createBaselineSeries(m_utilizationGpuSubPage, m_historyLength);
    m_gpuSharedMemoryLineSeries = new QLineSeries(m_utilizationGpuSubPage);
    m_gpuSharedMemoryLineSeries->setColor(gpuSharedMemoryColor);
    m_gpuSharedMemoryBaselineSeries = createBaselineSeries(m_utilizationGpuSubPage, m_historyLength);
    initializeLineSeriesHistory(m_gpuDedicatedMemoryLineSeries, m_historyLength);
    initializeLineSeriesHistory(m_gpuSharedMemoryLineSeries, m_historyLength);

    auto createGpuMemoryChart =
        [this](
            const QString& titleText,
            QLineSeries* lineSeries,
            QLineSeries* baselineSeries,
            QAreaSeries** areaSeriesOut,
            const QColor& lineColor,
            QValueAxis** axisXOut,
            QValueAxis** axisYOut,
            QChartView** chartViewOut)
        {
            QChart* chartPointer = new QChart();
            QAreaSeries* areaSeries = addFilledAreaSeries(
                chartPointer,
                lineSeries,
                baselineSeries,
                lineColor,
                42);
            configureUtilizationPlotChart(chartPointer, lineColor, titleText);

            QValueAxis* axisX = new QValueAxis(chartPointer);
            configureUtilizationValueAxis(axisX, lineColor, 0.0, static_cast<double>(m_historyLength));

            QValueAxis* axisY = new QValueAxis(chartPointer);
            configureUtilizationValueAxis(axisY, lineColor, 0.0, 1.0);

            chartPointer->addAxis(axisX, Qt::AlignBottom);
            chartPointer->addAxis(axisY, Qt::AlignLeft);
            if (areaSeries != nullptr)
            {
                areaSeries->attachAxis(axisX);
                areaSeries->attachAxis(axisY);
            }

            if (areaSeriesOut != nullptr)
            {
                *areaSeriesOut = areaSeries;
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
                *chartViewOut = createPlotBackgroundChartView(chartPointer, m_utilizationGpuSubPage);
            }
        };

    createGpuMemoryChart(
        QStringLiteral("专用 GPU 内存利用率"),
        m_gpuDedicatedMemoryLineSeries,
        m_gpuDedicatedMemoryBaselineSeries,
        &m_gpuDedicatedMemoryAreaSeries,
        gpuDedicatedMemoryColor,
        &m_gpuDedicatedMemoryAxisX,
        &m_gpuDedicatedMemoryAxisY,
        &m_gpuDedicatedMemoryChartView);
    createGpuMemoryChart(
        QStringLiteral("共享 GPU 内存利用率"),
        m_gpuSharedMemoryLineSeries,
        m_gpuSharedMemoryBaselineSeries,
        &m_gpuSharedMemoryAreaSeries,
        gpuSharedMemoryColor,
        &m_gpuSharedMemoryAxisX,
        &m_gpuSharedMemoryAxisY,
        &m_gpuSharedMemoryChartView);

    gpuSubLayout->addWidget(m_gpuDedicatedMemoryChartView, 0);
    gpuSubLayout->addWidget(m_gpuSharedMemoryChartView, 0);

    m_gpuUtilDetailLabel = new QLabel(QStringLiteral("GPU参数采样中..."), m_utilizationGpuSubPage);
    configureCompressibleLabel(m_gpuUtilDetailLabel);
    m_gpuUtilDetailLabel->setWordWrap(false);
    m_gpuUtilDetailLabel->setStyleSheet(
        QStringLiteral("font-size:14px;color:%1;").arg(KswordTheme::TextPrimaryHex()));
    gpuSubLayout->addWidget(m_gpuUtilDetailLabel, 0);

    m_utilizationDetailStack->addWidget(m_utilizationGpuSubPage);
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

void HardwareDock::initializeDiskMonitorTab()
{
    // 硬盘监控页独立承载资源监视器式进程 IO 表，避免塞进“利用率”详情页造成导航混乱。
    m_diskMonitorPage = new DiskMonitorPage(m_sideTabWidget);
    m_sideTabWidget->addTab(m_diskMonitorPage, QStringLiteral("硬盘监控"));
}

void HardwareDock::initializeOtherDevicesTab()
{
    // 其他设备页必须挂在 HardwareDock 内部侧边 Tab，不创建新的主窗口 ADS Dock。
    m_otherDevicesPage = new HardwareOtherDevicesPage(m_sideTabWidget);
    m_sideTabWidget->addTab(m_otherDevicesPage, QStringLiteral("其他设备"));
}

void HardwareDock::initializeCoreCharts()
{
    const DWORD logicalProcessorCount = std::max<DWORD>(1, ::GetActiveProcessorCount(ALL_PROCESSOR_GROUPS));
    const int coreCount = static_cast<int>(logicalProcessorCount);
    const int columnCount = std::max(1, static_cast<int>(std::ceil(std::sqrt(static_cast<double>(coreCount)))));
    const int rowCount = std::max(1, static_cast<int>(std::ceil(
        static_cast<double>(coreCount) / static_cast<double>(columnCount))));
    m_cpuCoreGridColumnCount = columnCount;
    m_cpuCoreGridRowCount = rowCount;

    m_coreChartEntries.clear();
    m_coreChartEntries.reserve(coreCount);

    for (int coreIndex = 0; coreIndex < coreCount; ++coreIndex)
    {
        CoreChartEntry chartEntry;
        chartEntry.containerWidget = new QWidget(m_coreChartHostWidget);
        configureCompressibleWidget(chartEntry.containerWidget, QSizePolicy::Expanding, QSizePolicy::Expanding);
        appendTransparentBackgroundStyle(chartEntry.containerWidget);
        QVBoxLayout* containerLayout = new QVBoxLayout(chartEntry.containerWidget);
        containerLayout->setContentsMargins(4, 4, 4, 4);
        containerLayout->setSpacing(2);

        chartEntry.titleLabel = new QLabel(
            QStringLiteral("CPU %1").arg(coreIndex),
            chartEntry.containerWidget);
        configureCompressibleLabel(chartEntry.titleLabel);
        chartEntry.titleLabel->setStyleSheet(
            QStringLiteral("color:%1;font-weight:600;").arg(buildStatusColor().name()));
        chartEntry.titleLabel->setAlignment(Qt::AlignLeft | Qt::AlignVCenter);
        containerLayout->addWidget(chartEntry.titleLabel, 0);

        chartEntry.lineSeries = new QLineSeries(chartEntry.containerWidget);
        chartEntry.lineSeries->setColor(QColor(KswordTheme::PrimaryBlueHex));
        chartEntry.baselineSeries = new QLineSeries(chartEntry.containerWidget);
        for (int indexValue = 0; indexValue < m_historyLength; ++indexValue)
        {
            chartEntry.lineSeries->append(indexValue, 0.0);
            chartEntry.baselineSeries->append(indexValue, 0.0);
        }

        QChart* chart = new QChart();
        chartEntry.areaSeries = new QAreaSeries(chartEntry.lineSeries, chartEntry.baselineSeries);
        chartEntry.areaSeries->setColor(QColor(45, 125, 255, 46));
        chartEntry.areaSeries->setBorderColor(QColor(KswordTheme::PrimaryBlueHex));
        chartEntry.areaSeries->setPen(QPen(QColor(KswordTheme::PrimaryBlueHex), 1.6));
        chart->addSeries(chartEntry.areaSeries);
        chart->legend()->hide();
        chart->setBackgroundVisible(false);
        chart->setBackgroundRoundness(0);
        chart->setMargins(QMargins(0, 0, 0, 0));
        chart->setPlotAreaBackgroundVisible(true);
        chart->setPlotAreaBackgroundBrush(QBrush(QColor(45, 125, 255, 18)));
        chart->setPlotAreaBackgroundPen(QPen(QColor(45, 125, 255, 150), 1.0));

        chartEntry.axisX = new QValueAxis(chart);
        chartEntry.axisX->setRange(0, m_historyLength - 1);
        chartEntry.axisX->setLabelsVisible(false);
        chartEntry.axisX->setGridLineVisible(true);
        chartEntry.axisX->setMinorGridLineVisible(false);
        chartEntry.axisX->setLineVisible(true);
        chartEntry.axisX->setLinePen(QPen(QColor(45, 125, 255, 140), 1.0));
        chartEntry.axisX->setGridLinePen(QPen(QColor(45, 125, 255, 46), 1.0));

        chartEntry.axisY = new QValueAxis(chart);
        chartEntry.axisY->setRange(0.0, 100.0);
        chartEntry.axisY->setLabelsVisible(false);
        chartEntry.axisY->setGridLineVisible(true);
        chartEntry.axisY->setMinorGridLineVisible(false);
        chartEntry.axisY->setLineVisible(true);
        chartEntry.axisY->setLinePen(QPen(QColor(45, 125, 255, 140), 1.0));
        chartEntry.axisY->setGridLinePen(QPen(QColor(45, 125, 255, 46), 1.0));

        chart->addAxis(chartEntry.axisX, Qt::AlignBottom);
        chart->addAxis(chartEntry.axisY, Qt::AlignLeft);
        chartEntry.areaSeries->attachAxis(chartEntry.axisX);
        chartEntry.areaSeries->attachAxis(chartEntry.axisY);

        chartEntry.chartView = createPlotBackgroundChartView(chart, chartEntry.containerWidget);
        containerLayout->addWidget(chartEntry.chartView, 1);

        const int rowIndex = coreIndex / columnCount;
        const int columnIndex = coreIndex % columnCount;
        m_coreChartGridLayout->addWidget(chartEntry.containerWidget, rowIndex, columnIndex);
        m_coreChartEntries.push_back(chartEntry);
    }

    adjustUtilizationChartHeights();
}

void HardwareDock::initializeConnections()
{
    // 暂无额外交互按钮，预留函数用于后续扩展。
}

void HardwareDock::scheduleUtilizationLayoutRefresh()
{
    // 当前事件循环先重排一次，确保新追加设备卡片能立即拿到稳定尺寸。
    adjustUtilizationChartHeights();
    // 0ms 延迟用于等待 QListWidget 插入行后完成 viewport 尺寸更新。
    QTimer::singleShot(0, this, [this]()
    {
        adjustUtilizationChartHeights();
    });
    // 80ms 延迟用于 ADS Dock 动画或首次显示链路完成后再校准一次。
    QTimer::singleShot(80, this, [this]()
    {
        adjustUtilizationChartHeights();
    });
}

int HardwareDock::findDiskUtilizationDeviceIndexByInstance(const QString& instanceNameText) const
{
    for (int indexValue = 0; indexValue < static_cast<int>(m_diskUtilDevices.size()); ++indexValue)
    {
        const DiskUtilizationDevice& device = m_diskUtilDevices[static_cast<std::size_t>(indexValue)];
        if (QString::compare(device.instanceNameText, instanceNameText, Qt::CaseInsensitive) == 0)
        {
            return indexValue;
        }
    }
    return -1;
}

int HardwareDock::ensureDiskUtilizationDevice(
    const DiskRateSample& sample,
    const int ordinalIndex)
{
    const int existingIndex = findDiskUtilizationDeviceIndexByInstance(sample.instanceNameText);
    if (existingIndex >= 0)
    {
        return existingIndex;
    }

    // device 用途：为新发现的物理磁盘实例保留 UI 控件和历史采样。
    DiskUtilizationDevice device;
    device.instanceNameText = sample.instanceNameText;
    device.displayNameText = sample.displayNameText.isEmpty()
        ? QStringLiteral("磁盘 %1").arg(ordinalIndex)
        : sample.displayNameText;
    createDiskUtilizationDevicePage(&device);
    m_diskUtilDevices.push_back(device);

    const int deviceIndex = static_cast<int>(m_diskUtilDevices.size()) - 1;
    m_diskUtilDevices[static_cast<std::size_t>(deviceIndex)].navCard = addUtilizationSidebarCard(
        m_diskUtilDevices[static_cast<std::size_t>(deviceIndex)].pageWidget,
        m_diskUtilDevices[static_cast<std::size_t>(deviceIndex)].displayNameText,
        QColor(104, 204, 116),
        UtilizationDeviceKind::Disk,
        deviceIndex);
    if (m_diskUtilDevices[static_cast<std::size_t>(deviceIndex)].navCard != nullptr)
    {
        m_diskUtilDevices[static_cast<std::size_t>(deviceIndex)].navCard->setSeriesColors(
            QColor(80, 170, 255),
            QColor(255, 190, 105));
    }
    scheduleUtilizationLayoutRefresh();
    return deviceIndex;
}

int HardwareDock::findNetworkUtilizationDeviceIndexByKey(const std::uint64_t interfaceKey) const
{
    for (int indexValue = 0; indexValue < static_cast<int>(m_networkUtilDevices.size()); ++indexValue)
    {
        const NetworkUtilizationDevice& device = m_networkUtilDevices[static_cast<std::size_t>(indexValue)];
        if (device.interfaceKey == interfaceKey)
        {
            return indexValue;
        }
    }
    return -1;
}

int HardwareDock::ensureNetworkUtilizationDevice(
    const NetworkRateSample& sample,
    const int ordinalIndex)
{
    const int existingIndex = findNetworkUtilizationDeviceIndexByKey(sample.interfaceKey);
    if (existingIndex >= 0)
    {
        return existingIndex;
    }

    // device 用途：为新发现的网卡接口保留 UI 控件和增量采样基线。
    NetworkUtilizationDevice device;
    device.interfaceKey = sample.interfaceKey;
    device.displayNameText = sample.displayNameText.isEmpty()
        ? QStringLiteral("以太网 %1").arg(ordinalIndex)
        : sample.displayNameText;
    device.linkBitsPerSecond = sample.linkBitsPerSecond;
    device.lastRxBytes = sample.totalRxBytes;
    device.lastTxBytes = sample.totalTxBytes;
    device.lastSampleMs = QDateTime::currentMSecsSinceEpoch();
    device.hasPreviousSample = true;
    createNetworkUtilizationDevicePage(&device);
    m_networkUtilDevices.push_back(device);

    const int deviceIndex = static_cast<int>(m_networkUtilDevices.size()) - 1;
    m_networkUtilDevices[static_cast<std::size_t>(deviceIndex)].navCard = addUtilizationSidebarCard(
        m_networkUtilDevices[static_cast<std::size_t>(deviceIndex)].pageWidget,
        m_networkUtilDevices[static_cast<std::size_t>(deviceIndex)].displayNameText,
        QColor(230, 149, 76),
        UtilizationDeviceKind::Network,
        deviceIndex);
    if (m_networkUtilDevices[static_cast<std::size_t>(deviceIndex)].navCard != nullptr)
    {
        m_networkUtilDevices[static_cast<std::size_t>(deviceIndex)].navCard->setSeriesColors(
            QColor(80, 170, 255),
            QColor(255, 190, 105));
    }
    scheduleUtilizationLayoutRefresh();
    return deviceIndex;
}

int HardwareDock::findGpuUtilizationDeviceIndexByKey(const std::uint64_t adapterKey) const
{
    for (int indexValue = 0; indexValue < static_cast<int>(m_gpuUtilDevices.size()); ++indexValue)
    {
        const GpuUtilizationDevice& device = m_gpuUtilDevices[static_cast<std::size_t>(indexValue)];
        if (device.adapterKeyAssigned && device.adapterKey == adapterKey)
        {
            return indexValue;
        }
    }
    return -1;
}

int HardwareDock::ensureGpuUtilizationDevice(
    const GpuUsageSample& sample,
    const int ordinalIndex)
{
    const int existingIndex = findGpuUtilizationDeviceIndexByKey(sample.adapterKey);
    if (existingIndex >= 0)
    {
        return existingIndex;
    }

    // device 用途：为新发现的 DXGI 适配器保留任务管理器风格 GPU 详情页。
    GpuUtilizationDevice device;
    device.adapterKey = sample.adapterKey;
    device.adapterKeyAssigned = true;
    device.adapterIndex = sample.adapterIndex;
    device.displayNameText = QStringLiteral("GPU %1").arg(ordinalIndex);
    createGpuUtilizationDevicePage(&device);
    m_gpuUtilDevices.push_back(device);

    const int deviceIndex = static_cast<int>(m_gpuUtilDevices.size()) - 1;
    m_gpuUtilDevices[static_cast<std::size_t>(deviceIndex)].navCard = addUtilizationSidebarCard(
        m_gpuUtilDevices[static_cast<std::size_t>(deviceIndex)].pageWidget,
        m_gpuUtilDevices[static_cast<std::size_t>(deviceIndex)].displayNameText,
        QColor(105, 173, 255),
        UtilizationDeviceKind::Gpu,
        deviceIndex);
    scheduleUtilizationLayoutRefresh();
    return deviceIndex;
}

void HardwareDock::createDiskUtilizationDevicePage(DiskUtilizationDevice* devicePointer)
{
    if (devicePointer == nullptr || m_utilizationDetailStack == nullptr)
    {
        return;
    }

    devicePointer->pageWidget = new QWidget(m_utilizationDetailStack);
    configureCompressibleWidget(devicePointer->pageWidget, QSizePolicy::Expanding, QSizePolicy::Expanding);
    appendTransparentBackgroundStyle(devicePointer->pageWidget);
    QVBoxLayout* pageLayout = new QVBoxLayout(devicePointer->pageWidget);
    pageLayout->setContentsMargins(4, 4, 4, 4);
    pageLayout->setSpacing(6);

    QLabel* titleLabel = new QLabel(devicePointer->displayNameText, devicePointer->pageWidget);
    configurePersistentHeaderLabel(titleLabel);
    titleLabel->setStyleSheet(
        QStringLiteral("font-size:46px;font-weight:700;color:%1;")
        .arg(KswordTheme::TextPrimaryHex()));
    lockLabelHeightToFont(titleLabel, 14);
    pageLayout->addWidget(titleLabel, 0);

    devicePointer->summaryLabel = new QLabel(QStringLiteral("磁盘采样初始化中..."), devicePointer->pageWidget);
    configureCompressibleLabel(devicePointer->summaryLabel);
    devicePointer->summaryLabel->setStyleSheet(
        QStringLiteral("color:%1;font-size:14px;font-weight:600;").arg(buildStatusColor().name()));
    pageLayout->addWidget(devicePointer->summaryLabel, 0);

    devicePointer->readLineSeries = new QLineSeries(devicePointer->pageWidget);
    devicePointer->readLineSeries->setName(QStringLiteral("读取"));
    const QColor readColor(80, 170, 255);
    const QColor writeColor(255, 190, 105);
    devicePointer->readLineSeries->setColor(readColor);
    devicePointer->readBaselineSeries = createBaselineSeries(devicePointer->pageWidget, m_historyLength);
    devicePointer->writeLineSeries = new QLineSeries(devicePointer->pageWidget);
    devicePointer->writeLineSeries->setName(QStringLiteral("写入"));
    devicePointer->writeLineSeries->setColor(writeColor);
    devicePointer->writeBaselineSeries = createBaselineSeries(devicePointer->pageWidget, m_historyLength);
    initializeLineSeriesHistory(devicePointer->readLineSeries, m_historyLength);
    initializeLineSeriesHistory(devicePointer->writeLineSeries, m_historyLength);

    QChart* chart = new QChart();
    devicePointer->readAreaSeries = addFilledAreaSeries(
        chart,
        devicePointer->readLineSeries,
        devicePointer->readBaselineSeries,
        readColor,
        42);
    devicePointer->writeAreaSeries = addFilledAreaSeries(
        chart,
        devicePointer->writeLineSeries,
        devicePointer->writeBaselineSeries,
        writeColor,
        34);
    configureUtilizationPlotChart(
        chart,
        readColor,
        QStringLiteral("%1 读写速率趋势").arg(devicePointer->displayNameText),
        true);
    devicePointer->axisX = new QValueAxis(chart);
    configureUtilizationValueAxis(devicePointer->axisX, readColor, 0.0, static_cast<double>(m_historyLength));
    devicePointer->axisY = new QValueAxis(chart);
    configureUtilizationValueAxis(devicePointer->axisY, readColor, 0.0, 1.0);
    chart->addAxis(devicePointer->axisX, Qt::AlignBottom);
    chart->addAxis(devicePointer->axisY, Qt::AlignLeft);
    if (devicePointer->readAreaSeries != nullptr)
    {
        devicePointer->readAreaSeries->attachAxis(devicePointer->axisX);
        devicePointer->readAreaSeries->attachAxis(devicePointer->axisY);
    }
    if (devicePointer->writeAreaSeries != nullptr)
    {
        devicePointer->writeAreaSeries->attachAxis(devicePointer->axisX);
        devicePointer->writeAreaSeries->attachAxis(devicePointer->axisY);
    }
    devicePointer->chartView = createPlotBackgroundChartView(chart, devicePointer->pageWidget);
    pageLayout->addWidget(devicePointer->chartView, 1);

    devicePointer->detailLabel = new QLabel(QStringLiteral("磁盘参数采样中..."), devicePointer->pageWidget);
    configureCompressibleLabel(devicePointer->detailLabel);
    devicePointer->detailLabel->setWordWrap(false);
    devicePointer->detailLabel->setStyleSheet(
        QStringLiteral("font-size:14px;color:%1;").arg(KswordTheme::TextPrimaryHex()));
    pageLayout->addWidget(devicePointer->detailLabel, 0);

    m_utilizationDetailStack->addWidget(devicePointer->pageWidget);
}

void HardwareDock::createNetworkUtilizationDevicePage(NetworkUtilizationDevice* devicePointer)
{
    if (devicePointer == nullptr || m_utilizationDetailStack == nullptr)
    {
        return;
    }

    devicePointer->pageWidget = new QWidget(m_utilizationDetailStack);
    configureCompressibleWidget(devicePointer->pageWidget, QSizePolicy::Expanding, QSizePolicy::Expanding);
    appendTransparentBackgroundStyle(devicePointer->pageWidget);
    QVBoxLayout* pageLayout = new QVBoxLayout(devicePointer->pageWidget);
    pageLayout->setContentsMargins(4, 4, 4, 4);
    pageLayout->setSpacing(6);

    QLabel* titleLabel = new QLabel(devicePointer->displayNameText, devicePointer->pageWidget);
    configurePersistentHeaderLabel(titleLabel);
    titleLabel->setStyleSheet(
        QStringLiteral("font-size:46px;font-weight:700;color:%1;")
        .arg(KswordTheme::TextPrimaryHex()));
    lockLabelHeightToFont(titleLabel, 14);
    pageLayout->addWidget(titleLabel, 0);

    devicePointer->summaryLabel = new QLabel(QStringLiteral("网络采样初始化中..."), devicePointer->pageWidget);
    configureCompressibleLabel(devicePointer->summaryLabel);
    devicePointer->summaryLabel->setStyleSheet(
        QStringLiteral("color:%1;font-size:14px;font-weight:600;").arg(buildStatusColor().name()));
    pageLayout->addWidget(devicePointer->summaryLabel, 0);

    devicePointer->rxLineSeries = new QLineSeries(devicePointer->pageWidget);
    devicePointer->rxLineSeries->setName(QStringLiteral("下行"));
    const QColor rxColor(92, 190, 255);
    const QColor txColor(255, 190, 105);
    devicePointer->rxLineSeries->setColor(rxColor);
    devicePointer->rxBaselineSeries = createBaselineSeries(devicePointer->pageWidget, m_historyLength);
    devicePointer->txLineSeries = new QLineSeries(devicePointer->pageWidget);
    devicePointer->txLineSeries->setName(QStringLiteral("上行"));
    devicePointer->txLineSeries->setColor(txColor);
    devicePointer->txBaselineSeries = createBaselineSeries(devicePointer->pageWidget, m_historyLength);
    initializeLineSeriesHistory(devicePointer->rxLineSeries, m_historyLength);
    initializeLineSeriesHistory(devicePointer->txLineSeries, m_historyLength);

    QChart* chart = new QChart();
    devicePointer->rxAreaSeries = addFilledAreaSeries(
        chart,
        devicePointer->rxLineSeries,
        devicePointer->rxBaselineSeries,
        rxColor,
        42);
    devicePointer->txAreaSeries = addFilledAreaSeries(
        chart,
        devicePointer->txLineSeries,
        devicePointer->txBaselineSeries,
        txColor,
        34);
    configureUtilizationPlotChart(
        chart,
        rxColor,
        QStringLiteral("%1 收发速率趋势").arg(devicePointer->displayNameText),
        true);
    devicePointer->axisX = new QValueAxis(chart);
    configureUtilizationValueAxis(devicePointer->axisX, rxColor, 0.0, static_cast<double>(m_historyLength));
    devicePointer->axisY = new QValueAxis(chart);
    configureUtilizationValueAxis(devicePointer->axisY, rxColor, 0.0, 1.0);
    chart->addAxis(devicePointer->axisX, Qt::AlignBottom);
    chart->addAxis(devicePointer->axisY, Qt::AlignLeft);
    if (devicePointer->rxAreaSeries != nullptr)
    {
        devicePointer->rxAreaSeries->attachAxis(devicePointer->axisX);
        devicePointer->rxAreaSeries->attachAxis(devicePointer->axisY);
    }
    if (devicePointer->txAreaSeries != nullptr)
    {
        devicePointer->txAreaSeries->attachAxis(devicePointer->axisX);
        devicePointer->txAreaSeries->attachAxis(devicePointer->axisY);
    }
    devicePointer->chartView = createPlotBackgroundChartView(chart, devicePointer->pageWidget);
    pageLayout->addWidget(devicePointer->chartView, 1);

    devicePointer->detailLabel = new QLabel(QStringLiteral("网络参数采样中..."), devicePointer->pageWidget);
    configureCompressibleLabel(devicePointer->detailLabel);
    devicePointer->detailLabel->setWordWrap(false);
    devicePointer->detailLabel->setStyleSheet(
        QStringLiteral("font-size:14px;color:%1;").arg(KswordTheme::TextPrimaryHex()));
    pageLayout->addWidget(devicePointer->detailLabel, 0);

    m_utilizationDetailStack->addWidget(devicePointer->pageWidget);
}

void HardwareDock::createGpuUtilizationDevicePage(GpuUtilizationDevice* devicePointer)
{
    if (devicePointer == nullptr || m_utilizationDetailStack == nullptr)
    {
        return;
    }

    devicePointer->pageWidget = new QWidget(m_utilizationDetailStack);
    configureCompressibleWidget(devicePointer->pageWidget, QSizePolicy::Expanding, QSizePolicy::Expanding);
    appendTransparentBackgroundStyle(devicePointer->pageWidget);
    QVBoxLayout* pageLayout = new QVBoxLayout(devicePointer->pageWidget);
    pageLayout->setContentsMargins(4, 4, 4, 4);
    pageLayout->setSpacing(6);

    QHBoxLayout* headerLayout = new QHBoxLayout();
    QLabel* titleLabel = new QLabel(devicePointer->displayNameText, devicePointer->pageWidget);
    configurePersistentHeaderLabel(titleLabel);
    titleLabel->setStyleSheet(
        QStringLiteral("font-size:46px;font-weight:700;color:%1;")
        .arg(KswordTheme::TextPrimaryHex()));
    lockLabelHeightToFont(titleLabel, 14);
    devicePointer->adapterTitleLabel = new QLabel(QStringLiteral("适配器读取中..."), devicePointer->pageWidget);
    configurePersistentHeaderLabel(devicePointer->adapterTitleLabel, QSizePolicy::Ignored);
    devicePointer->adapterTitleLabel->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
    devicePointer->adapterTitleLabel->setStyleSheet(
        QStringLiteral("font-size:15px;font-weight:500;color:%1;")
        .arg(KswordTheme::TextPrimaryHex()));
    lockLabelHeightToFont(devicePointer->adapterTitleLabel, 6);
    headerLayout->addWidget(titleLabel, 0);
    headerLayout->addStretch(1);
    headerLayout->addWidget(devicePointer->adapterTitleLabel, 0);
    pageLayout->addLayout(headerLayout, 0);

    devicePointer->summaryLabel = new QLabel(QStringLiteral("GPU采样初始化中..."), devicePointer->pageWidget);
    configureCompressibleLabel(devicePointer->summaryLabel);
    devicePointer->summaryLabel->setStyleSheet(
        QStringLiteral("color:%1;font-size:14px;font-weight:600;").arg(buildStatusColor().name()));
    pageLayout->addWidget(devicePointer->summaryLabel, 0);

    devicePointer->engineHostWidget = new QWidget(devicePointer->pageWidget);
    configureCompressibleWidget(devicePointer->engineHostWidget, QSizePolicy::Expanding, QSizePolicy::Expanding);
    appendTransparentBackgroundStyle(devicePointer->engineHostWidget);
    devicePointer->engineGridLayout = new QGridLayout(devicePointer->engineHostWidget);
    devicePointer->engineGridLayout->setContentsMargins(0, 0, 0, 0);
    devicePointer->engineGridLayout->setHorizontalSpacing(6);
    devicePointer->engineGridLayout->setVerticalSpacing(6);
    devicePointer->engineCharts.clear();

    auto addEngineChart =
        [this, devicePointer](
            const QString& keyText,
            const QString& displayText,
            const QColor& lineColor,
            const int rowIndex,
            const int columnIndex)
        {
            QWidget* cellWidget = new QWidget(devicePointer->engineHostWidget);
            configureCompressibleWidget(cellWidget, QSizePolicy::Expanding, QSizePolicy::Expanding);
            appendTransparentBackgroundStyle(cellWidget);
            QVBoxLayout* cellLayout = new QVBoxLayout(cellWidget);
            cellLayout->setContentsMargins(3, 3, 3, 3);
            cellLayout->setSpacing(2);

            GpuEngineChartEntry chartEntry;
            chartEntry.engineKeyText = keyText;
            chartEntry.displayNameText = displayText;
            chartEntry.titleLabel = new QLabel(displayText, cellWidget);
            configureCompressibleLabel(chartEntry.titleLabel);
            chartEntry.titleLabel->setStyleSheet(
                QStringLiteral("font-size:12px;color:%1;").arg(KswordTheme::TextPrimaryHex()));
            cellLayout->addWidget(chartEntry.titleLabel, 0);

            chartEntry.lineSeries = new QLineSeries(cellWidget);
            chartEntry.lineSeries->setColor(lineColor);
            chartEntry.baselineSeries = createBaselineSeries(cellWidget, m_historyLength);
            initializeLineSeriesHistory(chartEntry.lineSeries, m_historyLength);

            QChart* chart = new QChart();
            chartEntry.areaSeries = addFilledAreaSeries(
                chart,
                chartEntry.lineSeries,
                chartEntry.baselineSeries,
                lineColor,
                44);
            configureUtilizationPlotChart(chart, lineColor);
            chartEntry.axisX = new QValueAxis(chart);
            configureUtilizationValueAxis(chartEntry.axisX, lineColor, 0.0, static_cast<double>(m_historyLength));
            chartEntry.axisY = new QValueAxis(chart);
            configureUtilizationValueAxis(chartEntry.axisY, lineColor, 0.0, 100.0);
            chart->addAxis(chartEntry.axisX, Qt::AlignBottom);
            chart->addAxis(chartEntry.axisY, Qt::AlignLeft);
            if (chartEntry.areaSeries != nullptr)
            {
                chartEntry.areaSeries->attachAxis(chartEntry.axisX);
                chartEntry.areaSeries->attachAxis(chartEntry.axisY);
            }
            chartEntry.chartView = createPlotBackgroundChartView(chart, cellWidget);
            cellLayout->addWidget(chartEntry.chartView, 1);

            devicePointer->engineGridLayout->addWidget(cellWidget, rowIndex, columnIndex);
            devicePointer->engineCharts.push_back(chartEntry);
        };

    addEngineChart(QStringLiteral("3d"), QStringLiteral("3D"), QColor(105, 173, 255), 0, 0);
    addEngineChart(QStringLiteral("copy"), QStringLiteral("Copy"), QColor(110, 196, 247), 0, 1);
    addEngineChart(QStringLiteral("video_encode"), QStringLiteral("Video Encode"), QColor(125, 184, 255), 1, 0);
    addEngineChart(QStringLiteral("video_decode"), QStringLiteral("Video Decode"), QColor(137, 178, 255), 1, 1);
    pageLayout->addWidget(devicePointer->engineHostWidget, 1);

    auto createMemoryChart =
        [this, devicePointer](
            const QString& titleText,
            QLineSeries** seriesOut,
            QLineSeries** baselineSeriesOut,
            QAreaSeries** areaSeriesOut,
            const QColor& lineColor,
            QValueAxis** axisXOut,
            QValueAxis** axisYOut,
            QChartView** chartViewOut)
        {
            *seriesOut = new QLineSeries(devicePointer->pageWidget);
            (*seriesOut)->setColor(lineColor);
            *baselineSeriesOut = createBaselineSeries(devicePointer->pageWidget, m_historyLength);
            initializeLineSeriesHistory(*seriesOut, m_historyLength);

            QChart* chart = new QChart();
            *areaSeriesOut = addFilledAreaSeries(
                chart,
                *seriesOut,
                *baselineSeriesOut,
                lineColor,
                42);
            configureUtilizationPlotChart(chart, lineColor, titleText);
            *axisXOut = new QValueAxis(chart);
            configureUtilizationValueAxis(*axisXOut, lineColor, 0.0, static_cast<double>(m_historyLength));
            *axisYOut = new QValueAxis(chart);
            configureUtilizationValueAxis(*axisYOut, lineColor, 0.0, 1.0);
            chart->addAxis(*axisXOut, Qt::AlignBottom);
            chart->addAxis(*axisYOut, Qt::AlignLeft);
            if (*areaSeriesOut != nullptr)
            {
                (*areaSeriesOut)->attachAxis(*axisXOut);
                (*areaSeriesOut)->attachAxis(*axisYOut);
            }
            *chartViewOut = createPlotBackgroundChartView(chart, devicePointer->pageWidget);
        };

    createMemoryChart(
        QStringLiteral("专用 GPU 内存利用率"),
        &devicePointer->dedicatedMemoryLineSeries,
        &devicePointer->dedicatedMemoryBaselineSeries,
        &devicePointer->dedicatedMemoryAreaSeries,
        QColor(92, 167, 255),
        &devicePointer->dedicatedMemoryAxisX,
        &devicePointer->dedicatedMemoryAxisY,
        &devicePointer->dedicatedMemoryChartView);
    createMemoryChart(
        QStringLiteral("共享 GPU 内存利用率"),
        &devicePointer->sharedMemoryLineSeries,
        &devicePointer->sharedMemoryBaselineSeries,
        &devicePointer->sharedMemoryAreaSeries,
        QColor(113, 185, 255),
        &devicePointer->sharedMemoryAxisX,
        &devicePointer->sharedMemoryAxisY,
        &devicePointer->sharedMemoryChartView);
    pageLayout->addWidget(devicePointer->dedicatedMemoryChartView, 0);
    pageLayout->addWidget(devicePointer->sharedMemoryChartView, 0);

    devicePointer->detailLabel = new QLabel(QStringLiteral("GPU参数采样中..."), devicePointer->pageWidget);
    configureCompressibleLabel(devicePointer->detailLabel);
    devicePointer->detailLabel->setWordWrap(false);
    devicePointer->detailLabel->setStyleSheet(
        QStringLiteral("font-size:14px;color:%1;").arg(KswordTheme::TextPrimaryHex()));
    pageLayout->addWidget(devicePointer->detailLabel, 0);

    m_utilizationDetailStack->addWidget(devicePointer->pageWidget);
}

void HardwareDock::refreshCpuTopologyStaticInfo()
{
    m_cpuModelText = queryCpuBrandTextByCpuid();
    if (m_cpuModelLabel != nullptr && !m_cpuModelText.isEmpty())
    {
        m_cpuModelLabel->setText(m_cpuModelText);
    }

    DWORD requiredBytes = 0;
    ::GetLogicalProcessorInformationEx(RelationAll, nullptr, &requiredBytes);
    if (requiredBytes == 0)
    {
        m_cpuLogicalCoreCount = static_cast<int>(::GetActiveProcessorCount(ALL_PROCESSOR_GROUPS));
        return;
    }

    std::vector<unsigned char> buffer(requiredBytes);
    SYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX* infoPointer =
        reinterpret_cast<SYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX*>(buffer.data());
    if (::GetLogicalProcessorInformationEx(RelationAll, infoPointer, &requiredBytes) == FALSE)
    {
        m_cpuLogicalCoreCount = static_cast<int>(::GetActiveProcessorCount(ALL_PROCESSOR_GROUPS));
        return;
    }

    int packageCount = 0;
    int physicalCoreCount = 0;
    int logicalCoreCount = 0;
    std::uint64_t l1Bytes = 0;
    std::uint64_t l2Bytes = 0;
    std::uint64_t l3Bytes = 0;

    DWORD offsetBytes = 0;
    while (offsetBytes < requiredBytes)
    {
        SYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX* entryPointer =
            reinterpret_cast<SYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX*>(buffer.data() + offsetBytes);
        if (entryPointer->Relationship == RelationProcessorPackage)
        {
            ++packageCount;
        }
        else if (entryPointer->Relationship == RelationProcessorCore)
        {
            ++physicalCoreCount;
            for (WORD groupIndex = 0; groupIndex < entryPointer->Processor.GroupCount; ++groupIndex)
            {
                logicalCoreCount += countBits(entryPointer->Processor.GroupMask[groupIndex].Mask);
            }
        }
        else if (entryPointer->Relationship == RelationCache)
        {
            if (entryPointer->Cache.Level == 1)
            {
                l1Bytes += static_cast<std::uint64_t>(entryPointer->Cache.CacheSize);
            }
            else if (entryPointer->Cache.Level == 2)
            {
                l2Bytes += static_cast<std::uint64_t>(entryPointer->Cache.CacheSize);
            }
            else if (entryPointer->Cache.Level == 3)
            {
                l3Bytes += static_cast<std::uint64_t>(entryPointer->Cache.CacheSize);
            }
        }

        if (entryPointer->Size == 0)
        {
            break;
        }
        offsetBytes += entryPointer->Size;
    }

    m_cpuPackageCount = std::max(1, packageCount);
    m_cpuPhysicalCoreCount = std::max(1, physicalCoreCount);
    m_cpuLogicalCoreCount = logicalCoreCount > 0
        ? logicalCoreCount
        : static_cast<int>(::GetActiveProcessorCount(ALL_PROCESSOR_GROUPS));
    m_cpuL1CacheBytes = l1Bytes;
    m_cpuL2CacheBytes = l2Bytes;
    m_cpuL3CacheBytes = l3Bytes;
}

void HardwareDock::refreshSystemVolumeInfo()
{
    QString systemDrive = qEnvironmentVariable("SystemDrive");
    if (systemDrive.isEmpty())
    {
        systemDrive = QStringLiteral("C:");
    }

    QString rootPath = systemDrive;
    if (!rootPath.endsWith('\\'))
    {
        rootPath += QLatin1Char('\\');
    }

    ULARGE_INTEGER freeAvailableBytes{};
    ULARGE_INTEGER totalBytes{};
    ULARGE_INTEGER totalFreeBytes{};
    if (::GetDiskFreeSpaceExW(
        reinterpret_cast<LPCWSTR>(rootPath.utf16()),
        &freeAvailableBytes,
        &totalBytes,
        &totalFreeBytes) == TRUE)
    {
        m_systemVolumeTotalBytes = static_cast<std::uint64_t>(totalBytes.QuadPart);
        m_systemVolumeFreeBytes = static_cast<std::uint64_t>(totalFreeBytes.QuadPart);
    }

    wchar_t volumeNameBuffer[MAX_PATH] = {};
    if (::GetVolumeInformationW(
        reinterpret_cast<LPCWSTR>(rootPath.utf16()),
        volumeNameBuffer,
        MAX_PATH,
        nullptr,
        nullptr,
        nullptr,
        nullptr,
        0) == TRUE)
    {
        const QString volumeNameText = QString::fromWCharArray(volumeNameBuffer).trimmed();
        if (!volumeNameText.isEmpty())
        {
            m_systemVolumeText = QStringLiteral("%1 (%2)").arg(volumeNameText, systemDrive);
            return;
        }
    }

    m_systemVolumeText = rootPath;
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

    double diskReadBytesPerSec = 0.0;
    double diskWriteBytesPerSec = 0.0;
    std::vector<DiskRateSample> diskSampleList;
    if (sampleDiskRates(&diskSampleList))
    {
        for (const DiskRateSample& sample : diskSampleList)
        {
            diskReadBytesPerSec += std::max(0.0, sample.readBytesPerSec);
            diskWriteBytesPerSec += std::max(0.0, sample.writeBytesPerSec);
        }
    }
    else
    {
        diskReadBytesPerSec = 0.0;
        diskWriteBytesPerSec = 0.0;
    }

    double networkRxBytesPerSec = 0.0;
    double networkTxBytesPerSec = 0.0;
    std::vector<NetworkRateSample> networkSampleList;
    if (sampleNetworkRates(&networkSampleList))
    {
        for (const NetworkRateSample& sample : networkSampleList)
        {
            networkRxBytesPerSec += std::max(0.0, sample.rxBytesPerSec);
            networkTxBytesPerSec += std::max(0.0, sample.txBytesPerSec);
        }
    }
    else
    {
        networkRxBytesPerSec = 0.0;
        networkTxBytesPerSec = 0.0;
    }

    double gpuUsagePercent = 0.0;
    std::vector<GpuUsageSample> gpuSampleList;
    if (sampleGpuUsages(&gpuSampleList))
    {
        for (const GpuUsageSample& sample : gpuSampleList)
        {
            gpuUsagePercent = std::max(gpuUsagePercent, sample.overallUsagePercent);
        }
    }
    else
    {
        gpuUsagePercent = 0.0;
    }

    std::vector<CpuPowerSnapshot> powerInfoList;
    sampleCpuPowerInfo(&powerInfoList);

    ++m_sampleCounter;
    updateOverviewText(totalCpuUsage, memoryUsagePercent);
    updateUtilizationView(
        coreUsageList,
        memoryUsagePercent,
        diskReadBytesPerSec,
        diskWriteBytesPerSec,
        networkRxBytesPerSec,
        networkTxBytesPerSec,
        gpuUsagePercent);
    updateAdditionalDiskUtilizationDevices(diskSampleList);
    updateAdditionalNetworkUtilizationDevices(networkSampleList);
    updateAdditionalGpuUtilizationDevices(gpuSampleList);
    updateCpuDetailTable(coreUsageList, powerInfoList);
    updateTaskManagerDetailLabels(
        coreUsageList,
        powerInfoList,
        memoryUsagePercent,
        diskReadBytesPerSec,
        diskWriteBytesPerSec,
        networkRxBytesPerSec,
        networkTxBytesPerSec,
        gpuUsagePercent);
    // 高度重排只在 resize/tab 切换时执行，避免每秒重算导致核心图容器抖动。

    // 周期刷新策略：
    // - 传感器每 5 秒异步更新一次；
    // - 静态文本每 60 秒异步更新一次（兼顾信息时效与系统开销）。
    if ((m_sampleCounter % 5) == 1)
    {
        requestAsyncSensorRefresh();
    }
    if ((m_sampleCounter % 60) == 1)
    {
        requestAsyncStaticInfoRefresh();
    }
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

bool HardwareDock::sampleDiskRates(std::vector<DiskRateSample>* sampleListOut)
{
    if (sampleListOut == nullptr)
    {
        return false;
    }

    if (m_diskPerfQueryHandle == nullptr)
    {
        PDH_HQUERY queryHandle = nullptr;
        if (::PdhOpenQueryW(nullptr, 0, &queryHandle) != ERROR_SUCCESS || queryHandle == nullptr)
        {
            return false;
        }

        PDH_HCOUNTER readCounterHandle = nullptr;
        PDH_HCOUNTER writeCounterHandle = nullptr;
        const PDH_STATUS addReadStatus = ::PdhAddEnglishCounterW(
            queryHandle,
            L"\\PhysicalDisk(*)\\Disk Read Bytes/sec",
            0,
            &readCounterHandle);
        const PDH_STATUS addWriteStatus = ::PdhAddEnglishCounterW(
            queryHandle,
            L"\\PhysicalDisk(*)\\Disk Write Bytes/sec",
            0,
            &writeCounterHandle);
        if (addReadStatus != ERROR_SUCCESS || addWriteStatus != ERROR_SUCCESS)
        {
            ::PdhCloseQuery(queryHandle);
            return false;
        }

        m_diskPerfQueryHandle = queryHandle;
        m_diskReadCounterHandle = readCounterHandle;
        m_diskWriteCounterHandle = writeCounterHandle;
        ::PdhCollectQueryData(queryHandle);
    }

    const PDH_HQUERY queryHandle = reinterpret_cast<PDH_HQUERY>(m_diskPerfQueryHandle);
    if (::PdhCollectQueryData(queryHandle) != ERROR_SUCCESS)
    {
        return false;
    }

    DWORD readBufferSize = 0;
    DWORD readItemCount = 0;
    PDH_STATUS readQueryStatus = ::PdhGetFormattedCounterArrayW(
        reinterpret_cast<PDH_HCOUNTER>(m_diskReadCounterHandle),
        PDH_FMT_DOUBLE,
        &readBufferSize,
        &readItemCount,
        nullptr);
    if (readQueryStatus != PDH_MORE_DATA || readBufferSize == 0 || readItemCount == 0)
    {
        sampleListOut->clear();
        return true;
    }

    std::vector<unsigned char> readBuffer(readBufferSize);
    PDH_FMT_COUNTERVALUE_ITEM_W* readItemPointer =
        reinterpret_cast<PDH_FMT_COUNTERVALUE_ITEM_W*>(readBuffer.data());
    readQueryStatus = ::PdhGetFormattedCounterArrayW(
        reinterpret_cast<PDH_HCOUNTER>(m_diskReadCounterHandle),
        PDH_FMT_DOUBLE,
        &readBufferSize,
        &readItemCount,
        readItemPointer);
    if (readQueryStatus != ERROR_SUCCESS)
    {
        return false;
    }

    DWORD writeBufferSize = 0;
    DWORD writeItemCount = 0;
    PDH_STATUS writeQueryStatus = ::PdhGetFormattedCounterArrayW(
        reinterpret_cast<PDH_HCOUNTER>(m_diskWriteCounterHandle),
        PDH_FMT_DOUBLE,
        &writeBufferSize,
        &writeItemCount,
        nullptr);
    if (writeQueryStatus != PDH_MORE_DATA || writeBufferSize == 0 || writeItemCount == 0)
    {
        return false;
    }

    std::vector<unsigned char> writeBuffer(writeBufferSize);
    PDH_FMT_COUNTERVALUE_ITEM_W* writeItemPointer =
        reinterpret_cast<PDH_FMT_COUNTERVALUE_ITEM_W*>(writeBuffer.data());
    writeQueryStatus = ::PdhGetFormattedCounterArrayW(
        reinterpret_cast<PDH_HCOUNTER>(m_diskWriteCounterHandle),
        PDH_FMT_DOUBLE,
        &writeBufferSize,
        &writeItemCount,
        writeItemPointer);
    if (writeQueryStatus != ERROR_SUCCESS)
    {
        return false;
    }

    sampleListOut->clear();
    sampleListOut->reserve(readItemCount);
    for (DWORD readIndex = 0; readIndex < readItemCount; ++readIndex)
    {
        const PDH_FMT_COUNTERVALUE_ITEM_W& readItem = readItemPointer[readIndex];
        const QString instanceNameText = QString::fromWCharArray(
            readItem.szName != nullptr ? readItem.szName : L"").trimmed();
        if (instanceNameText.isEmpty() || instanceNameText == QStringLiteral("_Total"))
        {
            continue;
        }
        if (readItem.FmtValue.CStatus != ERROR_SUCCESS)
        {
            continue;
        }

        DiskRateSample sample;
        sample.instanceNameText = instanceNameText;
        sample.displayNameText = simplifyDiskInstanceName(instanceNameText);
        sample.readBytesPerSec = std::max(0.0, readItem.FmtValue.doubleValue);
        for (DWORD writeIndex = 0; writeIndex < writeItemCount; ++writeIndex)
        {
            const PDH_FMT_COUNTERVALUE_ITEM_W& writeItem = writeItemPointer[writeIndex];
            const QString writeInstanceNameText = QString::fromWCharArray(
                writeItem.szName != nullptr ? writeItem.szName : L"").trimmed();
            if (QString::compare(writeInstanceNameText, instanceNameText, Qt::CaseInsensitive) == 0
                && writeItem.FmtValue.CStatus == ERROR_SUCCESS)
            {
                sample.writeBytesPerSec = std::max(0.0, writeItem.FmtValue.doubleValue);
                break;
            }
        }
        sampleListOut->push_back(sample);
    }
    return true;
}

bool HardwareDock::sampleDiskRate(double* readBytesPerSecOut, double* writeBytesPerSecOut)
{
    if (readBytesPerSecOut == nullptr || writeBytesPerSecOut == nullptr)
    {
        return false;
    }

    std::vector<DiskRateSample> sampleList;
    const bool sampleOk = sampleDiskRates(&sampleList);
    if (!sampleOk)
    {
        return false;
    }

    double totalReadBytesPerSec = 0.0;
    double totalWriteBytesPerSec = 0.0;
    for (const DiskRateSample& sample : sampleList)
    {
        totalReadBytesPerSec += std::max(0.0, sample.readBytesPerSec);
        totalWriteBytesPerSec += std::max(0.0, sample.writeBytesPerSec);
    }
    *readBytesPerSecOut = totalReadBytesPerSec;
    *writeBytesPerSecOut = totalWriteBytesPerSec;
    return true;
}

bool HardwareDock::sampleNetworkRate(double* rxBytesPerSecOut, double* txBytesPerSecOut)
{
    if (rxBytesPerSecOut == nullptr || txBytesPerSecOut == nullptr)
    {
        return false;
    }

    MIB_IF_TABLE2* tablePointer = nullptr;
    if (::GetIfTable2(&tablePointer) != NO_ERROR || tablePointer == nullptr)
    {
        return false;
    }

    std::uint64_t totalRxBytes = 0;
    std::uint64_t totalTxBytes = 0;
    std::uint64_t primaryTrafficBytes = 0;
    QString primaryAdapterName;
    std::uint64_t primaryLinkBitsPerSecond = 0;
    for (ULONG rowIndex = 0; rowIndex < tablePointer->NumEntries; ++rowIndex)
    {
        const MIB_IF_ROW2& rowValue = tablePointer->Table[rowIndex];
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

        // 对齐任务管理器展示口径：
        // - 选择当前“累计流量最高”的活动网卡作为主展示网卡；
        // - 记录其链路速率，供详情页显示。
        const std::uint64_t rowTrafficBytes = static_cast<std::uint64_t>(rowValue.InOctets)
            + static_cast<std::uint64_t>(rowValue.OutOctets);
        if (rowTrafficBytes >= primaryTrafficBytes)
        {
            primaryTrafficBytes = rowTrafficBytes;
            primaryAdapterName = QString::fromWCharArray(rowValue.Alias);
            primaryLinkBitsPerSecond = std::max<std::uint64_t>(
                static_cast<std::uint64_t>(rowValue.ReceiveLinkSpeed),
                static_cast<std::uint64_t>(rowValue.TransmitLinkSpeed));
        }
    }
    ::FreeMibTable(tablePointer);

    m_primaryNetworkAdapterName = primaryAdapterName;
    m_primaryNetworkLinkBitsPerSecond = primaryLinkBitsPerSecond;

    const qint64 nowMs = QDateTime::currentMSecsSinceEpoch();
    if (m_lastNetworkSampleMs <= 0)
    {
        m_lastNetworkSampleMs = nowMs;
        m_lastNetworkRxBytes = totalRxBytes;
        m_lastNetworkTxBytes = totalTxBytes;
        *rxBytesPerSecOut = 0.0;
        *txBytesPerSecOut = 0.0;
        return true;
    }

    const qint64 elapsedMs = nowMs - m_lastNetworkSampleMs;
    if (elapsedMs <= 0)
    {
        return false;
    }

    const std::uint64_t deltaRx = totalRxBytes >= m_lastNetworkRxBytes
        ? (totalRxBytes - m_lastNetworkRxBytes)
        : 0;
    const std::uint64_t deltaTx = totalTxBytes >= m_lastNetworkTxBytes
        ? (totalTxBytes - m_lastNetworkTxBytes)
        : 0;
    m_lastNetworkSampleMs = nowMs;
    m_lastNetworkRxBytes = totalRxBytes;
    m_lastNetworkTxBytes = totalTxBytes;

    *rxBytesPerSecOut = static_cast<double>(deltaRx) * 1000.0 / static_cast<double>(elapsedMs);
    *txBytesPerSecOut = static_cast<double>(deltaTx) * 1000.0 / static_cast<double>(elapsedMs);
    return true;
}

bool HardwareDock::sampleNetworkRates(std::vector<NetworkRateSample>* sampleListOut)
{
    if (sampleListOut == nullptr)
    {
        return false;
    }

    MIB_IF_TABLE2* tablePointer = nullptr;
    if (::GetIfTable2(&tablePointer) != NO_ERROR || tablePointer == nullptr)
    {
        return false;
    }

    sampleListOut->clear();
    sampleListOut->reserve(tablePointer->NumEntries);
    const qint64 nowMs = QDateTime::currentMSecsSinceEpoch();
    std::uint64_t primaryTrafficBytes = 0;
    QString primaryAdapterName;
    std::uint64_t primaryLinkBitsPerSecond = 0;
    for (ULONG rowIndex = 0; rowIndex < tablePointer->NumEntries; ++rowIndex)
    {
        const MIB_IF_ROW2& rowValue = tablePointer->Table[rowIndex];
        if (rowValue.OperStatus != IfOperStatusUp)
        {
            continue;
        }
        if (rowValue.Type == IF_TYPE_SOFTWARE_LOOPBACK)
        {
            continue;
        }

        NetworkRateSample sample;
        sample.interfaceKey = interfaceLuidToKey(static_cast<std::uint64_t>(rowValue.InterfaceLuid.Value));
        sample.displayNameText = QString::fromWCharArray(rowValue.Alias).trimmed();
        if (sample.displayNameText.isEmpty())
        {
            sample.displayNameText = QString::fromWCharArray(rowValue.Description).trimmed();
        }
        sample.linkBitsPerSecond = std::max<std::uint64_t>(
            static_cast<std::uint64_t>(rowValue.ReceiveLinkSpeed),
            static_cast<std::uint64_t>(rowValue.TransmitLinkSpeed));
        sample.totalRxBytes = static_cast<std::uint64_t>(rowValue.InOctets);
        sample.totalTxBytes = static_cast<std::uint64_t>(rowValue.OutOctets);

        const int deviceIndex = ensureNetworkUtilizationDevice(
            sample,
            static_cast<int>(sampleListOut->size()));
        if (deviceIndex >= 0 && deviceIndex < static_cast<int>(m_networkUtilDevices.size()))
        {
            NetworkUtilizationDevice& device = m_networkUtilDevices[static_cast<std::size_t>(deviceIndex)];
            const qint64 elapsedMs = nowMs - device.lastSampleMs;
            if (device.hasPreviousSample && elapsedMs > 0)
            {
                const std::uint64_t deltaRx = sample.totalRxBytes >= device.lastRxBytes
                    ? (sample.totalRxBytes - device.lastRxBytes)
                    : 0;
                const std::uint64_t deltaTx = sample.totalTxBytes >= device.lastTxBytes
                    ? (sample.totalTxBytes - device.lastTxBytes)
                    : 0;
                sample.rxBytesPerSec = static_cast<double>(deltaRx) * 1000.0 / static_cast<double>(elapsedMs);
                sample.txBytesPerSec = static_cast<double>(deltaTx) * 1000.0 / static_cast<double>(elapsedMs);
            }
            device.lastRxBytes = sample.totalRxBytes;
            device.lastTxBytes = sample.totalTxBytes;
            device.lastSampleMs = nowMs;
            device.linkBitsPerSecond = sample.linkBitsPerSecond;
            device.hasPreviousSample = true;
        }
        const std::uint64_t trafficBytes = sample.totalRxBytes + sample.totalTxBytes;
        if (trafficBytes >= primaryTrafficBytes)
        {
            primaryTrafficBytes = trafficBytes;
            primaryAdapterName = sample.displayNameText;
            primaryLinkBitsPerSecond = sample.linkBitsPerSecond;
        }
        sampleListOut->push_back(sample);
    }
    ::FreeMibTable(tablePointer);
    m_primaryNetworkAdapterName = primaryAdapterName;
    m_primaryNetworkLinkBitsPerSecond = primaryLinkBitsPerSecond;
    return true;
}

bool HardwareDock::sampleGpuUsages(std::vector<GpuUsageSample>* sampleListOut)
{
    if (sampleListOut == nullptr)
    {
        return false;
    }

    // oneGiBInBytes 用途：把 DXGI 字节字段转换为任务管理器常见 GiB 文本。
    constexpr double oneGiBInBytes = 1024.0 * 1024.0 * 1024.0;
    IDXGIFactory6* factoryPointer = nullptr;
    const HRESULT createFactoryStatus = ::CreateDXGIFactory1(IID_PPV_ARGS(&factoryPointer));
    if (FAILED(createFactoryStatus) || factoryPointer == nullptr)
    {
        return false;
    }

    sampleListOut->clear();
    for (UINT adapterIndex = 0;; ++adapterIndex)
    {
        IDXGIAdapter1* adapterPointer = nullptr;
        const HRESULT enumStatus = factoryPointer->EnumAdapters1(adapterIndex, &adapterPointer);
        if (enumStatus == DXGI_ERROR_NOT_FOUND)
        {
            break;
        }
        if (FAILED(enumStatus) || adapterPointer == nullptr)
        {
            continue;
        }

        DXGI_ADAPTER_DESC1 adapterDesc{};
        adapterPointer->GetDesc1(&adapterDesc);
        if ((adapterDesc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) != 0)
        {
            adapterPointer->Release();
            continue;
        }

        GpuUsageSample sample;
        sample.adapterKey = packLuidKey(adapterDesc.AdapterLuid);
        sample.adapterIndex = static_cast<int>(adapterIndex);
        sample.displayNameText = QString::fromWCharArray(adapterDesc.Description).trimmed();
        sample.dedicatedMemoryGiB = static_cast<double>(adapterDesc.DedicatedVideoMemory) / oneGiBInBytes;

        IDXGIAdapter3* adapter3Pointer = nullptr;
        const HRESULT queryInterfaceStatus = adapterPointer->QueryInterface(
            IID_PPV_ARGS(&adapter3Pointer));
        if (SUCCEEDED(queryInterfaceStatus) && adapter3Pointer != nullptr)
        {
            DXGI_QUERY_VIDEO_MEMORY_INFO localMemoryInfo{};
            DXGI_QUERY_VIDEO_MEMORY_INFO nonLocalMemoryInfo{};
            const HRESULT localStatus = adapter3Pointer->QueryVideoMemoryInfo(
                0,
                DXGI_MEMORY_SEGMENT_GROUP_LOCAL,
                &localMemoryInfo);
            const HRESULT nonLocalStatus = adapter3Pointer->QueryVideoMemoryInfo(
                0,
                DXGI_MEMORY_SEGMENT_GROUP_NON_LOCAL,
                &nonLocalMemoryInfo);
            if (SUCCEEDED(localStatus))
            {
                sample.dedicatedUsedGiB =
                    static_cast<double>(localMemoryInfo.CurrentUsage) / oneGiBInBytes;
                sample.dedicatedBudgetGiB =
                    static_cast<double>(localMemoryInfo.Budget) / oneGiBInBytes;
            }
            if (SUCCEEDED(nonLocalStatus))
            {
                sample.sharedUsedGiB =
                    static_cast<double>(nonLocalMemoryInfo.CurrentUsage) / oneGiBInBytes;
                sample.sharedBudgetGiB =
                    static_cast<double>(nonLocalMemoryInfo.Budget) / oneGiBInBytes;
            }
            adapter3Pointer->Release();
        }

        if (sample.sharedBudgetGiB <= 0.0)
        {
            MEMORYSTATUSEX memoryStatus{};
            memoryStatus.dwLength = sizeof(memoryStatus);
            if (::GlobalMemoryStatusEx(&memoryStatus) == TRUE)
            {
                const double totalMemoryGiB =
                    static_cast<double>(memoryStatus.ullTotalPhys) / oneGiBInBytes;
                sample.sharedBudgetGiB = std::max(0.5, totalMemoryGiB * 0.5);
            }
        }

        ensureGpuUtilizationDevice(sample, static_cast<int>(sampleListOut->size()));
        sampleListOut->push_back(sample);
        adapterPointer->Release();
    }
    factoryPointer->Release();

    if (sampleListOut->empty())
    {
        return true;
    }

    if (m_gpuPerfQueryHandle == nullptr)
    {
        PDH_HQUERY queryHandle = nullptr;
        if (::PdhOpenQueryW(nullptr, 0, &queryHandle) != ERROR_SUCCESS || queryHandle == nullptr)
        {
            return true;
        }

        PDH_HCOUNTER counterHandle = nullptr;
        const PDH_STATUS addStatus = ::PdhAddEnglishCounterW(
            queryHandle,
            L"\\GPU Engine(*)\\Utilization Percentage",
            0,
            &counterHandle);
        if (addStatus != ERROR_SUCCESS || counterHandle == nullptr)
        {
            ::PdhCloseQuery(queryHandle);
            return true;
        }

        m_gpuPerfQueryHandle = queryHandle;
        m_gpuCounterHandle = counterHandle;
        ::PdhCollectQueryData(queryHandle);
        ::Sleep(1);
        ::PdhCollectQueryData(queryHandle);
    }
    else
    {
        const PDH_HQUERY queryHandle = reinterpret_cast<PDH_HQUERY>(m_gpuPerfQueryHandle);
        if (::PdhCollectQueryData(queryHandle) != ERROR_SUCCESS)
        {
            return true;
        }

        DWORD bufferSize = 0;
        DWORD itemCount = 0;
        PDH_STATUS queryStatus = ::PdhGetFormattedCounterArrayW(
            reinterpret_cast<PDH_HCOUNTER>(m_gpuCounterHandle),
            PDH_FMT_DOUBLE,
            &bufferSize,
            &itemCount,
            nullptr);
        if (queryStatus == PDH_MORE_DATA && bufferSize > 0 && itemCount > 0)
        {
            std::vector<unsigned char> rawBuffer(bufferSize);
            PDH_FMT_COUNTERVALUE_ITEM_W* itemPointer =
                reinterpret_cast<PDH_FMT_COUNTERVALUE_ITEM_W*>(rawBuffer.data());
            queryStatus = ::PdhGetFormattedCounterArrayW(
                reinterpret_cast<PDH_HCOUNTER>(m_gpuCounterHandle),
                PDH_FMT_DOUBLE,
                &bufferSize,
                &itemCount,
                itemPointer);
            if (queryStatus == ERROR_SUCCESS)
            {
                for (DWORD itemIndex = 0; itemIndex < itemCount; ++itemIndex)
                {
                    const PDH_FMT_COUNTERVALUE_ITEM_W& itemValue = itemPointer[itemIndex];
                    if (itemValue.FmtValue.CStatus != ERROR_SUCCESS)
                    {
                        continue;
                    }

                    const QString engineNameText = QString::fromWCharArray(
                        itemValue.szName != nullptr ? itemValue.szName : L"");
                    std::uint64_t adapterKey = 0;
                    if (!parseGpuAdapterKeyFromEngineName(engineNameText, &adapterKey))
                    {
                        continue;
                    }

                    const QString engineKeyText = resolveGpuEngineKeyFromCounter(engineNameText);
                    if (engineKeyText.isEmpty())
                    {
                        continue;
                    }

                    GpuUsageSample* samplePointer = nullptr;
                    for (GpuUsageSample& sample : *sampleListOut)
                    {
                        if (sample.adapterKey == adapterKey)
                        {
                            samplePointer = &sample;
                            break;
                        }
                    }
                    if (samplePointer == nullptr)
                    {
                        continue;
                    }

                    const double engineUsagePercent =
                        std::clamp(itemValue.FmtValue.doubleValue, 0.0, 100.0);
                    if (engineKeyText == QStringLiteral("3d"))
                    {
                        samplePointer->usage3DPercent =
                            std::max(samplePointer->usage3DPercent, engineUsagePercent);
                    }
                    else if (engineKeyText == QStringLiteral("copy"))
                    {
                        samplePointer->usageCopyPercent =
                            std::max(samplePointer->usageCopyPercent, engineUsagePercent);
                    }
                    else if (engineKeyText == QStringLiteral("video_encode"))
                    {
                        samplePointer->usageVideoEncodePercent =
                            std::max(samplePointer->usageVideoEncodePercent, engineUsagePercent);
                    }
                    else if (engineKeyText == QStringLiteral("video_decode"))
                    {
                        samplePointer->usageVideoDecodePercent =
                            std::max(samplePointer->usageVideoDecodePercent, engineUsagePercent);
                    }
                    samplePointer->overallUsagePercent =
                        std::max(samplePointer->overallUsagePercent, engineUsagePercent);
                }
            }
        }
    }

    // aggregate* 用途：维护旧聚合 GPU 页的兼容展示，动态设备页使用 sampleListOut。
    GpuUsageSample aggregateSample = sampleListOut->front();
    for (const GpuUsageSample& sample : *sampleListOut)
    {
        aggregateSample.overallUsagePercent =
            std::max(aggregateSample.overallUsagePercent, sample.overallUsagePercent);
        aggregateSample.usage3DPercent =
            std::max(aggregateSample.usage3DPercent, sample.usage3DPercent);
        aggregateSample.usageCopyPercent =
            std::max(aggregateSample.usageCopyPercent, sample.usageCopyPercent);
        aggregateSample.usageVideoEncodePercent =
            std::max(aggregateSample.usageVideoEncodePercent, sample.usageVideoEncodePercent);
        aggregateSample.usageVideoDecodePercent =
            std::max(aggregateSample.usageVideoDecodePercent, sample.usageVideoDecodePercent);
    }
    m_gpuAdapterNameText = aggregateSample.displayNameText;
    m_gpuDedicatedMemoryGiB = aggregateSample.dedicatedMemoryGiB;
    m_gpuDedicatedUsedGiB = aggregateSample.dedicatedUsedGiB;
    m_gpuDedicatedBudgetGiB = aggregateSample.dedicatedBudgetGiB;
    m_gpuSharedUsedGiB = aggregateSample.sharedUsedGiB;
    m_gpuSharedBudgetGiB = aggregateSample.sharedBudgetGiB;
    m_gpuUsage3DPercent = aggregateSample.usage3DPercent;
    m_gpuUsageCopyPercent = aggregateSample.usageCopyPercent;
    m_gpuUsageVideoEncodePercent = aggregateSample.usageVideoEncodePercent;
    m_gpuUsageVideoDecodePercent = aggregateSample.usageVideoDecodePercent;
    return true;
}

bool HardwareDock::sampleGpuUsage(double* gpuUsagePercentOut)
{
    if (gpuUsagePercentOut == nullptr)
    {
        return false;
    }

    if (m_gpuPerfQueryHandle == nullptr)
    {
        PDH_HQUERY queryHandle = nullptr;
        if (::PdhOpenQueryW(nullptr, 0, &queryHandle) != ERROR_SUCCESS || queryHandle == nullptr)
        {
            return false;
        }

        PDH_HCOUNTER counterHandle = nullptr;
        const PDH_STATUS addStatus = ::PdhAddEnglishCounterW(
            queryHandle,
            L"\\GPU Engine(*)\\Utilization Percentage",
            0,
            &counterHandle);
        if (addStatus != ERROR_SUCCESS || counterHandle == nullptr)
        {
            ::PdhCloseQuery(queryHandle);
            return false;
        }

        m_gpuPerfQueryHandle = queryHandle;
        m_gpuCounterHandle = counterHandle;
        ::PdhCollectQueryData(queryHandle);
        *gpuUsagePercentOut = 0.0;
        return true;
    }

    const PDH_HQUERY queryHandle = reinterpret_cast<PDH_HQUERY>(m_gpuPerfQueryHandle);
    if (::PdhCollectQueryData(queryHandle) != ERROR_SUCCESS)
    {
        return false;
    }

    DWORD bufferSize = 0;
    DWORD itemCount = 0;
    PDH_STATUS queryStatus = ::PdhGetFormattedCounterArrayW(
        reinterpret_cast<PDH_HCOUNTER>(m_gpuCounterHandle),
        PDH_FMT_DOUBLE,
        &bufferSize,
        &itemCount,
        nullptr);
    if (queryStatus != PDH_MORE_DATA || bufferSize == 0 || itemCount == 0)
    {
        m_gpuUsage3DPercent = 0.0;
        m_gpuUsageCopyPercent = 0.0;
        m_gpuUsageVideoEncodePercent = 0.0;
        m_gpuUsageVideoDecodePercent = 0.0;
        *gpuUsagePercentOut = 0.0;
        sampleGpuMemoryInfoByDxgi();
        return true;
    }

    std::vector<unsigned char> rawBuffer(bufferSize);
    PDH_FMT_COUNTERVALUE_ITEM_W* itemPtr = reinterpret_cast<PDH_FMT_COUNTERVALUE_ITEM_W*>(rawBuffer.data());
    queryStatus = ::PdhGetFormattedCounterArrayW(
        reinterpret_cast<PDH_HCOUNTER>(m_gpuCounterHandle),
        PDH_FMT_DOUBLE,
        &bufferSize,
        &itemCount,
        itemPtr);
    if (queryStatus != ERROR_SUCCESS)
    {
        m_gpuUsage3DPercent = 0.0;
        m_gpuUsageCopyPercent = 0.0;
        m_gpuUsageVideoEncodePercent = 0.0;
        m_gpuUsageVideoDecodePercent = 0.0;
        *gpuUsagePercentOut = 0.0;
        sampleGpuMemoryInfoByDxgi();
        return true;
    }

    // 任务管理器“总体 GPU”近似值：
    // - 先按引擎分类记录峰值；
    // - 再取四类引擎中的最大值作为总体利用率。
    double usage3DPercent = 0.0;
    double usageCopyPercent = 0.0;
    double usageVideoEncodePercent = 0.0;
    double usageVideoDecodePercent = 0.0;
    double peakUsage = 0.0;
    for (DWORD indexValue = 0; indexValue < itemCount; ++indexValue)
    {
        const PDH_FMT_COUNTERVALUE_ITEM_W& itemValue = itemPtr[indexValue];
        if (itemValue.FmtValue.CStatus != ERROR_SUCCESS)
        {
            continue;
        }

        // engineUsagePercent 用途：当前计数器样本值，限制在 0~100。
        const double engineUsagePercent = std::clamp(itemValue.FmtValue.doubleValue, 0.0, 100.0);
        const QString engineNameText = QString::fromWCharArray(
            itemValue.szName != nullptr ? itemValue.szName : L"");
        const QString engineKeyText = resolveGpuEngineKeyFromCounter(engineNameText);
        if (engineKeyText == QStringLiteral("3d"))
        {
            usage3DPercent = std::max(usage3DPercent, engineUsagePercent);
        }
        else if (engineKeyText == QStringLiteral("copy"))
        {
            usageCopyPercent = std::max(usageCopyPercent, engineUsagePercent);
        }
        else if (engineKeyText == QStringLiteral("video_encode"))
        {
            usageVideoEncodePercent = std::max(usageVideoEncodePercent, engineUsagePercent);
        }
        else if (engineKeyText == QStringLiteral("video_decode"))
        {
            usageVideoDecodePercent = std::max(usageVideoDecodePercent, engineUsagePercent);
        }

        peakUsage = std::max(peakUsage, engineUsagePercent);
    }

    m_gpuUsage3DPercent = usage3DPercent;
    m_gpuUsageCopyPercent = usageCopyPercent;
    m_gpuUsageVideoEncodePercent = usageVideoEncodePercent;
    m_gpuUsageVideoDecodePercent = usageVideoDecodePercent;
    *gpuUsagePercentOut = std::clamp(peakUsage, 0.0, 100.0);
    sampleGpuMemoryInfoByDxgi();
    return true;
}

bool HardwareDock::sampleGpuMemoryInfoByDxgi()
{
    // oneGiBInBytes 用途：字节到 GiB 的统一换算系数。
    constexpr double oneGiBInBytes = 1024.0 * 1024.0 * 1024.0;

    IDXGIFactory6* factoryPointer = nullptr;
    const HRESULT createFactoryStatus = ::CreateDXGIFactory1(IID_PPV_ARGS(&factoryPointer));
    if (FAILED(createFactoryStatus) || factoryPointer == nullptr)
    {
        return false;
    }

    bool querySuccess = false;
    for (UINT adapterIndex = 0;; ++adapterIndex)
    {
        IDXGIAdapter1* adapterPointer = nullptr;
        const HRESULT enumStatus = factoryPointer->EnumAdapters1(adapterIndex, &adapterPointer);
        if (enumStatus == DXGI_ERROR_NOT_FOUND)
        {
            break;
        }
        if (FAILED(enumStatus) || adapterPointer == nullptr)
        {
            continue;
        }

        DXGI_ADAPTER_DESC1 adapterDesc{};
        adapterPointer->GetDesc1(&adapterDesc);
        if ((adapterDesc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) != 0)
        {
            adapterPointer->Release();
            continue;
        }

        IDXGIAdapter3* adapter3Pointer = nullptr;
        const HRESULT queryInterfaceStatus = adapterPointer->QueryInterface(
            IID_PPV_ARGS(&adapter3Pointer));
        if (SUCCEEDED(queryInterfaceStatus) && adapter3Pointer != nullptr)
        {
            DXGI_QUERY_VIDEO_MEMORY_INFO localMemoryInfo{};
            DXGI_QUERY_VIDEO_MEMORY_INFO nonLocalMemoryInfo{};
            const HRESULT localStatus = adapter3Pointer->QueryVideoMemoryInfo(
                0,
                DXGI_MEMORY_SEGMENT_GROUP_LOCAL,
                &localMemoryInfo);
            const HRESULT nonLocalStatus = adapter3Pointer->QueryVideoMemoryInfo(
                0,
                DXGI_MEMORY_SEGMENT_GROUP_NON_LOCAL,
                &nonLocalMemoryInfo);
            if (SUCCEEDED(localStatus) && SUCCEEDED(nonLocalStatus))
            {
                m_gpuDedicatedUsedGiB = static_cast<double>(localMemoryInfo.CurrentUsage) / oneGiBInBytes;
                m_gpuDedicatedBudgetGiB = static_cast<double>(localMemoryInfo.Budget) / oneGiBInBytes;
                m_gpuSharedUsedGiB = static_cast<double>(nonLocalMemoryInfo.CurrentUsage) / oneGiBInBytes;
                m_gpuSharedBudgetGiB = static_cast<double>(nonLocalMemoryInfo.Budget) / oneGiBInBytes;

                // 某些设备 non-local budget 可能返回 0，回退到“物理内存一半”近似值。
                if (m_gpuSharedBudgetGiB <= 0.0)
                {
                    MEMORYSTATUSEX memoryStatus{};
                    memoryStatus.dwLength = sizeof(memoryStatus);
                    if (::GlobalMemoryStatusEx(&memoryStatus) == TRUE)
                    {
                        const double totalMemoryGiB =
                            static_cast<double>(memoryStatus.ullTotalPhys) / oneGiBInBytes;
                        m_gpuSharedBudgetGiB = std::max(0.5, totalMemoryGiB * 0.5);
                    }
                }

                const QString adapterNameText = QString::fromWCharArray(adapterDesc.Description).trimmed();
                if (!adapterNameText.isEmpty())
                {
                    m_gpuAdapterNameText = adapterNameText;
                }
                querySuccess = true;
            }
            adapter3Pointer->Release();
        }

        adapterPointer->Release();
        if (querySuccess)
        {
            break;
        }
    }

    factoryPointer->Release();
    return querySuccess;
}

bool HardwareDock::sampleSystemPerformanceSnapshot(SystemPerformanceSnapshot* snapshotOut) const
{
    if (snapshotOut == nullptr)
    {
        return false;
    }

    PERFORMANCE_INFORMATION perfInfo{};
    perfInfo.cb = sizeof(perfInfo);
    if (::GetPerformanceInfo(&perfInfo, sizeof(perfInfo)) == FALSE)
    {
        return false;
    }

    // pageSizeBytes 用途：把页数指标统一转换为字节单位。
    const std::uint64_t pageSizeBytes = static_cast<std::uint64_t>(perfInfo.PageSize);
    snapshotOut->processCount = static_cast<std::uint32_t>(perfInfo.ProcessCount);
    snapshotOut->threadCount = static_cast<std::uint32_t>(perfInfo.ThreadCount);
    snapshotOut->handleCount = static_cast<std::uint32_t>(perfInfo.HandleCount);
    snapshotOut->commitTotalBytes = static_cast<std::uint64_t>(perfInfo.CommitTotal) * pageSizeBytes;
    snapshotOut->commitLimitBytes = static_cast<std::uint64_t>(perfInfo.CommitLimit) * pageSizeBytes;
    snapshotOut->cachedBytes = static_cast<std::uint64_t>(perfInfo.SystemCache) * pageSizeBytes;
    snapshotOut->pagedPoolBytes = static_cast<std::uint64_t>(perfInfo.KernelPaged) * pageSizeBytes;
    snapshotOut->nonPagedPoolBytes = static_cast<std::uint64_t>(perfInfo.KernelNonpaged) * pageSizeBytes;
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
    const double memoryUsagePercent,
    const double diskReadBytesPerSec,
    const double diskWriteBytesPerSec,
    const double networkRxBytesPerSec,
    const double networkTxBytesPerSec,
    const double gpuUsagePercent)
{
    // averageCpuUsage 用途：CPU 平均占用，用于标题和左侧导航卡片。
    double averageCpuUsage = 0.0;
    if (!coreUsageList.empty())
    {
        for (const double usageValue : coreUsageList)
        {
            averageCpuUsage += usageValue;
        }
        averageCpuUsage /= static_cast<double>(coreUsageList.size());
    }

    if (m_utilizationSummaryLabel != nullptr)
    {
        m_utilizationSummaryLabel->setText(
            QStringLiteral("CPU 总体利用率：%1%    内存：%2%    逻辑处理器：%3")
            .arg(averageCpuUsage, 0, 'f', 1)
            .arg(memoryUsagePercent, 0, 'f', 1)
            .arg(coreUsageList.size()));
    }

    if (m_cpuModelLabel != nullptr && !m_cpuModelText.isEmpty())
    {
        m_cpuModelLabel->setText(m_cpuModelText);
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
            .arg(indexValue, 2, 10, QLatin1Char('0'))
            .arg(usageValue, 5, 'f', 1, QLatin1Char(' ')));
        appendCoreSeriesPoint(chartEntry, usageValue);
    }

    // 内存子页：更新摘要与折线趋势。
    if (m_memoryUtilSummaryLabel != nullptr)
    {
        m_memoryUtilSummaryLabel->setText(
            QStringLiteral("当前内存占用：%1%").arg(memoryUsagePercent, 0, 'f', 1));
    }
    if (m_memoryCompositionHistoryWidget != nullptr)
    {
        MemoryCompositionHistoryWidget::CompositionSample memorySample;
        memorySample.usedPercent = memoryUsagePercent;

        SystemPerformanceSnapshot perfSnapshot;
        const bool perfOk = sampleSystemPerformanceSnapshot(&perfSnapshot);
        MEMORYSTATUSEX memoryStatus{};
        memoryStatus.dwLength = sizeof(memoryStatus);
        const bool memoryStatusOk = (::GlobalMemoryStatusEx(&memoryStatus) == TRUE);
        if (perfOk && memoryStatusOk && memoryStatus.ullTotalPhys > 0ULL)
        {
            const double totalPhysicalBytes = static_cast<double>(memoryStatus.ullTotalPhys);
            memorySample.cachedPercent = static_cast<double>(perfSnapshot.cachedBytes) / totalPhysicalBytes * 100.0;
            memorySample.pagedPoolPercent = static_cast<double>(perfSnapshot.pagedPoolBytes) / totalPhysicalBytes * 100.0;
            memorySample.nonPagedPoolPercent = static_cast<double>(perfSnapshot.nonPagedPoolBytes) / totalPhysicalBytes * 100.0;
        }
        m_memoryCompositionHistoryWidget->appendSample(memorySample);
    }

    // 磁盘子页：更新读写速率摘要与折线趋势。
    if (m_diskUtilSummaryLabel != nullptr)
    {
        m_diskUtilSummaryLabel->setText(
            QStringLiteral("读取：%1    写入：%2")
            .arg(formatRateText(diskReadBytesPerSec))
            .arg(formatRateText(diskWriteBytesPerSec)));
    }
    appendFilledSeriesPoint(
        m_diskReadLineSeries,
        m_diskReadBaselineSeries,
        m_diskUtilAxisX,
        m_diskUtilAxisY,
        diskReadBytesPerSec,
        0.0);
    appendFilledSeriesPoint(
        m_diskWriteLineSeries,
        m_diskWriteBaselineSeries,
        m_diskUtilAxisX,
        m_diskUtilAxisY,
        diskWriteBytesPerSec,
        0.0);
    updateSharedSeriesAxisRange(
        m_diskReadLineSeries,
        m_diskWriteLineSeries,
        m_diskUtilAxisX,
        m_diskUtilAxisY,
        0.0);

    // 网络子页：更新上下行速率摘要与折线趋势。
    if (m_networkUtilSummaryLabel != nullptr)
    {
        m_networkUtilSummaryLabel->setText(
            QStringLiteral("接收：%1    发送：%2")
            .arg(formatRateText(networkRxBytesPerSec))
            .arg(formatRateText(networkTxBytesPerSec)));
    }
    appendFilledSeriesPoint(
        m_networkRxLineSeries,
        m_networkRxBaselineSeries,
        m_networkUtilAxisX,
        m_networkUtilAxisY,
        networkRxBytesPerSec,
        0.0);
    appendFilledSeriesPoint(
        m_networkTxLineSeries,
        m_networkTxBaselineSeries,
        m_networkUtilAxisX,
        m_networkUtilAxisY,
        networkTxBytesPerSec,
        0.0);
    updateSharedSeriesAxisRange(
        m_networkRxLineSeries,
        m_networkTxLineSeries,
        m_networkUtilAxisX,
        m_networkUtilAxisY,
        0.0);

    // GPU 子页：更新利用率摘要与折线趋势。
    if (m_gpuUtilSummaryLabel != nullptr)
    {
        m_gpuUtilSummaryLabel->setText(
            QStringLiteral("GPU 当前利用率：%1%    3D：%2%    Copy：%3%")
            .arg(gpuUsagePercent, 0, 'f', 1)
            .arg(m_gpuUsage3DPercent, 0, 'f', 1)
            .arg(m_gpuUsageCopyPercent, 0, 'f', 1));
    }

    for (GpuEngineChartEntry& chartEntry : m_gpuEngineCharts)
    {
        double usagePercent = 0.0;
        if (chartEntry.engineKeyText == QStringLiteral("3d"))
        {
            usagePercent = m_gpuUsage3DPercent;
        }
        else if (chartEntry.engineKeyText == QStringLiteral("copy"))
        {
            usagePercent = m_gpuUsageCopyPercent;
        }
        else if (chartEntry.engineKeyText == QStringLiteral("video_encode"))
        {
            usagePercent = m_gpuUsageVideoEncodePercent;
        }
        else if (chartEntry.engineKeyText == QStringLiteral("video_decode"))
        {
            usagePercent = m_gpuUsageVideoDecodePercent;
        }
        appendFilledSeriesPoint(
            chartEntry.lineSeries,
            chartEntry.baselineSeries,
            chartEntry.axisX,
            chartEntry.axisY,
            usagePercent,
            0.0);
        if (chartEntry.titleLabel != nullptr)
        {
            chartEntry.titleLabel->setText(
                QStringLiteral("%1  %2%")
                .arg(chartEntry.displayNameText)
                .arg(usagePercent, 0, 'f', 1));
        }
    }

    appendFilledSeriesPoint(
        m_gpuDedicatedMemoryLineSeries,
        m_gpuDedicatedMemoryBaselineSeries,
        m_gpuDedicatedMemoryAxisX,
        m_gpuDedicatedMemoryAxisY,
        m_gpuDedicatedUsedGiB,
        0.0);
    appendFilledSeriesPoint(
        m_gpuSharedMemoryLineSeries,
        m_gpuSharedMemoryBaselineSeries,
        m_gpuSharedMemoryAxisX,
        m_gpuSharedMemoryAxisY,
        m_gpuSharedUsedGiB,
        0.0);
    if (m_gpuDedicatedMemoryAxisY != nullptr)
    {
        const double dedicatedUpperGiB = std::max(
            0.5,
            (m_gpuDedicatedBudgetGiB > 0.0 ? m_gpuDedicatedBudgetGiB : m_gpuDedicatedMemoryGiB));
        m_gpuDedicatedMemoryAxisY->setRange(0.0, dedicatedUpperGiB);
    }
    if (m_gpuSharedMemoryAxisY != nullptr)
    {
        const double sharedUpperGiB = std::max(0.5, m_gpuSharedBudgetGiB);
        m_gpuSharedMemoryAxisY->setRange(0.0, sharedUpperGiB);
    }

    updateUtilizationSidebarCards(
        averageCpuUsage,
        memoryUsagePercent,
        diskReadBytesPerSec,
        diskWriteBytesPerSec,
        networkRxBytesPerSec,
        networkTxBytesPerSec,
        gpuUsagePercent);
}

void HardwareDock::updateUtilizationSidebarCards(
    const double cpuUsagePercent,
    const double memoryUsagePercent,
    const double diskReadBytesPerSec,
    const double diskWriteBytesPerSec,
    const double networkRxBytesPerSec,
    const double networkTxBytesPerSec,
    const double gpuUsagePercent)
{
    if (m_cpuNavCard != nullptr)
    {
        m_cpuNavCard->setSubtitleText(
            QStringLiteral("%1%  %2 GHz")
            .arg(cpuUsagePercent, 0, 'f', 0)
            .arg(m_lastCpuSpeedGhz, 0, 'f', 2));
        m_cpuNavCard->appendSample(cpuUsagePercent);
    }

    MEMORYSTATUSEX memoryStatus{};
    memoryStatus.dwLength = sizeof(memoryStatus);
    if (m_memoryNavCard != nullptr && ::GlobalMemoryStatusEx(&memoryStatus) == TRUE)
    {
        const double totalGiB = static_cast<double>(memoryStatus.ullTotalPhys) / (1024.0 * 1024.0 * 1024.0);
        const double usedGiB =
            static_cast<double>(memoryStatus.ullTotalPhys - memoryStatus.ullAvailPhys) / (1024.0 * 1024.0 * 1024.0);
        const double cachedPercent = std::clamp(
            100.0 - memoryUsagePercent,
            0.0,
            100.0);
        const int historyCapacity = std::max(1, m_memoryNavCard->sampleCapacity());
        m_memoryNavUsedHistoryPercent.push_back(memoryUsagePercent);
        m_memoryNavCachedHistoryPercent.push_back(cachedPercent);
        while (static_cast<int>(m_memoryNavUsedHistoryPercent.size()) > historyCapacity)
        {
            m_memoryNavUsedHistoryPercent.erase(m_memoryNavUsedHistoryPercent.begin());
        }
        while (static_cast<int>(m_memoryNavCachedHistoryPercent.size()) > historyCapacity)
        {
            m_memoryNavCachedHistoryPercent.erase(m_memoryNavCachedHistoryPercent.begin());
        }

        QVector<double> usedSampleList;
        QVector<double> cachedSampleList;
        usedSampleList.reserve(static_cast<int>(m_memoryNavUsedHistoryPercent.size()));
        cachedSampleList.reserve(static_cast<int>(m_memoryNavCachedHistoryPercent.size()));
        for (const double usedSampleValue : m_memoryNavUsedHistoryPercent)
        {
            usedSampleList.push_back(std::clamp(usedSampleValue, 0.0, 100.0));
        }
        for (const double cachedSampleValue : m_memoryNavCachedHistoryPercent)
        {
            cachedSampleList.push_back(std::clamp(cachedSampleValue, 0.0, 100.0));
        }

        m_memoryNavCard->setSubtitleText(
            QStringLiteral("用 %1/%2 GB / 余 %3%")
            .arg(usedGiB, 0, 'f', 1)
            .arg(totalGiB, 0, 'f', 1)
            .arg(cachedPercent, 0, 'f', 0));
        m_memoryNavCard->setSampleSeries(usedSampleList, cachedSampleList);
    }

    if (m_diskNavCard != nullptr)
    {
        rebuildDualRateNavCard(
            m_diskNavCard,
            &m_diskNavReadHistoryBytesPerSec,
            &m_diskNavWriteHistoryBytesPerSec,
            diskReadBytesPerSec,
            diskWriteBytesPerSec,
            &m_diskNavAutoScaleBytesPerSec,
            QStringLiteral("读 %1 / 写 %2")
            .arg(formatRateText(diskReadBytesPerSec))
            .arg(formatRateText(diskWriteBytesPerSec)));
    }

    if (m_networkNavCard != nullptr)
    {
        rebuildDualRateNavCard(
            m_networkNavCard,
            &m_networkNavRxHistoryBytesPerSec,
            &m_networkNavTxHistoryBytesPerSec,
            networkRxBytesPerSec,
            networkTxBytesPerSec,
            &m_networkNavAutoScaleBytesPerSec,
            QStringLiteral("下 %1 / 上 %2")
            .arg(formatRateText(networkRxBytesPerSec))
            .arg(formatRateText(networkTxBytesPerSec)));
    }

    if (m_gpuNavCard != nullptr)
    {
        m_gpuNavCard->setSubtitleText(
            QStringLiteral("%1%  %2/%3 GB")
            .arg(gpuUsagePercent, 0, 'f', 0)
            .arg(m_gpuDedicatedUsedGiB, 0, 'f', 1)
            .arg((m_gpuDedicatedBudgetGiB > 0.0 ? m_gpuDedicatedBudgetGiB : m_gpuDedicatedMemoryGiB), 0, 'f', 1));
        m_gpuNavCard->appendSample(gpuUsagePercent);
    }
}

void HardwareDock::updateAdditionalDiskUtilizationDevices(const std::vector<DiskRateSample>& sampleList)
{
    for (int sampleIndex = 0; sampleIndex < static_cast<int>(sampleList.size()); ++sampleIndex)
    {
        const DiskRateSample& sample = sampleList[static_cast<std::size_t>(sampleIndex)];
        const int deviceIndex = ensureDiskUtilizationDevice(sample, sampleIndex);
        if (deviceIndex < 0 || deviceIndex >= static_cast<int>(m_diskUtilDevices.size()))
        {
            continue;
        }
        updateDiskUtilizationDevice(m_diskUtilDevices[static_cast<std::size_t>(deviceIndex)], sample);
    }
}

void HardwareDock::updateAdditionalNetworkUtilizationDevices(const std::vector<NetworkRateSample>& sampleList)
{
    for (int sampleIndex = 0; sampleIndex < static_cast<int>(sampleList.size()); ++sampleIndex)
    {
        const NetworkRateSample& sample = sampleList[static_cast<std::size_t>(sampleIndex)];
        const int deviceIndex = ensureNetworkUtilizationDevice(sample, sampleIndex);
        if (deviceIndex < 0 || deviceIndex >= static_cast<int>(m_networkUtilDevices.size()))
        {
            continue;
        }
        updateNetworkUtilizationDevice(m_networkUtilDevices[static_cast<std::size_t>(deviceIndex)], sample);
    }
}

void HardwareDock::updateAdditionalGpuUtilizationDevices(const std::vector<GpuUsageSample>& sampleList)
{
    for (int sampleIndex = 0; sampleIndex < static_cast<int>(sampleList.size()); ++sampleIndex)
    {
        const GpuUsageSample& sample = sampleList[static_cast<std::size_t>(sampleIndex)];
        const int deviceIndex = ensureGpuUtilizationDevice(sample, sampleIndex);
        if (deviceIndex < 0 || deviceIndex >= static_cast<int>(m_gpuUtilDevices.size()))
        {
            continue;
        }
        updateGpuUtilizationDevice(m_gpuUtilDevices[static_cast<std::size_t>(deviceIndex)], sample);
    }
}

void HardwareDock::updateDiskUtilizationDevice(
    DiskUtilizationDevice& device,
    const DiskRateSample& sample)
{
    if (device.summaryLabel != nullptr)
    {
        device.summaryLabel->setText(
            QStringLiteral("读取：%1    写入：%2")
            .arg(formatRateText(sample.readBytesPerSec))
            .arg(formatRateText(sample.writeBytesPerSec)));
    }

    appendFilledSeriesPoint(
        device.readLineSeries,
        device.readBaselineSeries,
        device.axisX,
        device.axisY,
        sample.readBytesPerSec,
        0.0);
    appendFilledSeriesPoint(
        device.writeLineSeries,
        device.writeBaselineSeries,
        device.axisX,
        device.axisY,
        sample.writeBytesPerSec,
        0.0);
    updateSharedSeriesAxisRange(
        device.readLineSeries,
        device.writeLineSeries,
        device.axisX,
        device.axisY,
        0.0);

    if (device.navCard != nullptr)
    {
        rebuildDualRateNavCard(
            device.navCard,
            &device.readHistoryBytesPerSec,
            &device.writeHistoryBytesPerSec,
            sample.readBytesPerSec,
            sample.writeBytesPerSec,
            &device.navAutoScaleBytesPerSec,
            QStringLiteral("读 %1 / 写 %2")
            .arg(formatRateText(sample.readBytesPerSec))
            .arg(formatRateText(sample.writeBytesPerSec)));
    }

    if (device.detailLabel != nullptr)
    {
        const double totalRate = std::max(0.0, sample.readBytesPerSec)
            + std::max(0.0, sample.writeBytesPerSec);
        const double approxPercent = std::clamp(
            totalRate / std::max(1.0, device.navAutoScaleBytesPerSec) * 100.0,
            0.0,
            100.0);
        device.detailLabel->setText(
            QStringLiteral(
                "活动时间(近似): %1%\n"
                "读取速度: %2\n"
                "写入速度: %3\n"
                "性能计数器实例: %4")
            .arg(approxPercent, 0, 'f', 1)
            .arg(formatRateText(sample.readBytesPerSec))
            .arg(formatRateText(sample.writeBytesPerSec))
            .arg(sample.instanceNameText));
    }
}

void HardwareDock::updateNetworkUtilizationDevice(
    NetworkUtilizationDevice& device,
    const NetworkRateSample& sample)
{
    if (device.summaryLabel != nullptr)
    {
        device.summaryLabel->setText(
            QStringLiteral("接收：%1    发送：%2")
            .arg(formatRateText(sample.rxBytesPerSec))
            .arg(formatRateText(sample.txBytesPerSec)));
    }

    appendFilledSeriesPoint(
        device.rxLineSeries,
        device.rxBaselineSeries,
        device.axisX,
        device.axisY,
        sample.rxBytesPerSec,
        0.0);
    appendFilledSeriesPoint(
        device.txLineSeries,
        device.txBaselineSeries,
        device.axisX,
        device.axisY,
        sample.txBytesPerSec,
        0.0);
    updateSharedSeriesAxisRange(
        device.rxLineSeries,
        device.txLineSeries,
        device.axisX,
        device.axisY,
        0.0);

    if (device.navCard != nullptr)
    {
        rebuildDualRateNavCard(
            device.navCard,
            &device.rxHistoryBytesPerSec,
            &device.txHistoryBytesPerSec,
            sample.rxBytesPerSec,
            sample.txBytesPerSec,
            &device.navAutoScaleBytesPerSec,
            QStringLiteral("下 %1 / 上 %2")
            .arg(formatRateText(sample.rxBytesPerSec))
            .arg(formatRateText(sample.txBytesPerSec)));
    }

    if (device.detailLabel != nullptr)
    {
        const double linkMbps = static_cast<double>(sample.linkBitsPerSecond) / (1000.0 * 1000.0);
        device.detailLabel->setText(
            QStringLiteral(
                "适配器: %1\n"
                "发送: %2\n"
                "接收: %3\n"
                "链路速度: %4 Mbps")
            .arg(sample.displayNameText.isEmpty() ? QStringLiteral("N/A") : sample.displayNameText)
            .arg(formatRateText(sample.txBytesPerSec))
            .arg(formatRateText(sample.rxBytesPerSec))
            .arg(linkMbps > 0.0 ? QString::number(linkMbps, 'f', 1) : QStringLiteral("N/A")));
    }
}

void HardwareDock::updateGpuUtilizationDevice(
    GpuUtilizationDevice& device,
    const GpuUsageSample& sample)
{
    if (device.adapterTitleLabel != nullptr)
    {
        device.adapterTitleLabel->setText(
            sample.displayNameText.isEmpty() ? QStringLiteral("N/A") : sample.displayNameText);
    }
    if (device.summaryLabel != nullptr)
    {
        device.summaryLabel->setText(
            QStringLiteral("GPU 当前利用率：%1%    3D：%2%    Copy：%3%")
            .arg(sample.overallUsagePercent, 0, 'f', 1)
            .arg(sample.usage3DPercent, 0, 'f', 1)
            .arg(sample.usageCopyPercent, 0, 'f', 1));
    }

    for (GpuEngineChartEntry& chartEntry : device.engineCharts)
    {
        double usagePercent = 0.0;
        if (chartEntry.engineKeyText == QStringLiteral("3d"))
        {
            usagePercent = sample.usage3DPercent;
        }
        else if (chartEntry.engineKeyText == QStringLiteral("copy"))
        {
            usagePercent = sample.usageCopyPercent;
        }
        else if (chartEntry.engineKeyText == QStringLiteral("video_encode"))
        {
            usagePercent = sample.usageVideoEncodePercent;
        }
        else if (chartEntry.engineKeyText == QStringLiteral("video_decode"))
        {
            usagePercent = sample.usageVideoDecodePercent;
        }
        appendFilledSeriesPoint(
            chartEntry.lineSeries,
            chartEntry.baselineSeries,
            chartEntry.axisX,
            chartEntry.axisY,
            usagePercent,
            0.0);
        if (chartEntry.titleLabel != nullptr)
        {
            chartEntry.titleLabel->setText(
                QStringLiteral("%1  %2%")
                .arg(chartEntry.displayNameText)
                .arg(usagePercent, 0, 'f', 1));
        }
    }

    appendFilledSeriesPoint(
        device.dedicatedMemoryLineSeries,
        device.dedicatedMemoryBaselineSeries,
        device.dedicatedMemoryAxisX,
        device.dedicatedMemoryAxisY,
        sample.dedicatedUsedGiB,
        0.0);
    appendFilledSeriesPoint(
        device.sharedMemoryLineSeries,
        device.sharedMemoryBaselineSeries,
        device.sharedMemoryAxisX,
        device.sharedMemoryAxisY,
        sample.sharedUsedGiB,
        0.0);
    if (device.dedicatedMemoryAxisY != nullptr)
    {
        const double dedicatedUpperGiB = std::max(
            0.5,
            sample.dedicatedBudgetGiB > 0.0 ? sample.dedicatedBudgetGiB : sample.dedicatedMemoryGiB);
        device.dedicatedMemoryAxisY->setRange(0.0, dedicatedUpperGiB);
    }
    if (device.sharedMemoryAxisY != nullptr)
    {
        device.sharedMemoryAxisY->setRange(0.0, std::max(0.5, sample.sharedBudgetGiB));
    }
    if (device.dedicatedMemoryChartView != nullptr
        && device.dedicatedMemoryChartView->chart() != nullptr)
    {
        device.dedicatedMemoryChartView->chart()->setTitle(
            QStringLiteral("专用 GPU 内存利用率  %1 / %2 GiB")
            .arg(sample.dedicatedUsedGiB, 0, 'f', 2)
            .arg((sample.dedicatedBudgetGiB > 0.0 ? sample.dedicatedBudgetGiB : sample.dedicatedMemoryGiB), 0, 'f', 2));
    }
    if (device.sharedMemoryChartView != nullptr
        && device.sharedMemoryChartView->chart() != nullptr)
    {
        device.sharedMemoryChartView->chart()->setTitle(
            QStringLiteral("共享 GPU 内存利用率  %1 / %2 GiB")
            .arg(sample.sharedUsedGiB, 0, 'f', 2)
            .arg(sample.sharedBudgetGiB, 0, 'f', 2));
    }
    if (device.detailLabel != nullptr)
    {
        device.detailLabel->setText(
            QStringLiteral(
                "利用率: %1%\n"
                "3D: %2%   Copy: %3%   Video Encode: %4%   Video Decode: %5%\n"
                "专用显存: %6 / %7 GiB\n"
                "共享显存: %8 / %9 GiB\n"
                "适配器索引: %10")
            .arg(sample.overallUsagePercent, 0, 'f', 1)
            .arg(sample.usage3DPercent, 0, 'f', 1)
            .arg(sample.usageCopyPercent, 0, 'f', 1)
            .arg(sample.usageVideoEncodePercent, 0, 'f', 1)
            .arg(sample.usageVideoDecodePercent, 0, 'f', 1)
            .arg(sample.dedicatedUsedGiB, 0, 'f', 2)
            .arg((sample.dedicatedBudgetGiB > 0.0 ? sample.dedicatedBudgetGiB : sample.dedicatedMemoryGiB), 0, 'f', 2)
            .arg(sample.sharedUsedGiB, 0, 'f', 2)
            .arg(sample.sharedBudgetGiB, 0, 'f', 2)
            .arg(sample.adapterIndex));
    }
    if (device.navCard != nullptr)
    {
        device.navCard->setSubtitleText(
            QStringLiteral("%1%  %2/%3 GB")
            .arg(sample.overallUsagePercent, 0, 'f', 0)
            .arg(sample.dedicatedUsedGiB, 0, 'f', 1)
            .arg((sample.dedicatedBudgetGiB > 0.0 ? sample.dedicatedBudgetGiB : sample.dedicatedMemoryGiB), 0, 'f', 1));
        device.navCard->appendSample(sample.overallUsagePercent);
    }
}

void HardwareDock::updateTaskManagerDetailLabels(
    const std::vector<double>& coreUsageList,
    const std::vector<CpuPowerSnapshot>& powerInfoList,
    const double memoryUsagePercent,
    const double diskReadBytesPerSec,
    const double diskWriteBytesPerSec,
    const double networkRxBytesPerSec,
    const double networkTxBytesPerSec,
    const double gpuUsagePercent)
{
    // 平均 CPU 利用率用于 CPU 详情页核心统计。
    double averageCpuUsage = 0.0;
    if (!coreUsageList.empty())
    {
        for (const double usageValue : coreUsageList)
        {
            averageCpuUsage += usageValue;
        }
        averageCpuUsage /= static_cast<double>(coreUsageList.size());
    }

    // 计算 CPU 当前速度与基准速度（取全部核心平均值）。
    double currentMhzSum = 0.0;
    double maxMhzSum = 0.0;
    int cpuPowerCount = 0;
    for (const CpuPowerSnapshot& snapshot : powerInfoList)
    {
        if (snapshot.currentMhz > 0)
        {
            currentMhzSum += static_cast<double>(snapshot.currentMhz);
        }
        if (snapshot.maxMhz > 0)
        {
            maxMhzSum += static_cast<double>(snapshot.maxMhz);
        }
        ++cpuPowerCount;
    }
    const double currentCpuGhz = cpuPowerCount > 0
        ? (currentMhzSum / static_cast<double>(cpuPowerCount) / 1000.0)
        : 0.0;
    const double baseCpuGhz = cpuPowerCount > 0
        ? (maxMhzSum / static_cast<double>(cpuPowerCount) / 1000.0)
        : 0.0;
    m_lastCpuSpeedGhz = currentCpuGhz;

    // 系统性能快照用于进程/线程/句柄与提交内存统计。
    SystemPerformanceSnapshot perfSnapshot;
    const bool perfOk = sampleSystemPerformanceSnapshot(&perfSnapshot);

    // uptimeSeconds 用途：系统已运行秒数，显示为任务管理器风格时间串。
    const std::uint64_t uptimeSeconds = static_cast<std::uint64_t>(::GetTickCount64() / 1000ULL);
    if (m_cpuUtilPrimaryDetailLabel != nullptr)
    {
        m_cpuUtilPrimaryDetailLabel->setText(
            QStringLiteral(
                "利用率: %1%\n"
                "速度: %2 GHz\n"
                "进程: %3\n"
                "线程: %4\n"
                "句柄: %5\n"
                "正常运行时间: %6")
            .arg(averageCpuUsage, 0, 'f', 1)
            .arg(currentCpuGhz, 0, 'f', 2)
            .arg(perfOk ? QString::number(perfSnapshot.processCount) : QStringLiteral("N/A"))
            .arg(perfOk ? QString::number(perfSnapshot.threadCount) : QStringLiteral("N/A"))
            .arg(perfOk ? QString::number(perfSnapshot.handleCount) : QStringLiteral("N/A"))
            .arg(formatDurationText(uptimeSeconds)));
    }

    if (m_cpuUtilSecondaryDetailLabel != nullptr)
    {
        m_cpuUtilSecondaryDetailLabel->setText(
            QStringLiteral(
                "基准速度: %1 GHz\n"
                "插槽: %2\n"
                "内核: %3\n"
                "逻辑处理器: %4\n"
                "L1缓存: %5\n"
                "L2缓存: %6\n"
                "L3缓存: %7")
            .arg(baseCpuGhz, 0, 'f', 2)
            .arg(m_cpuPackageCount > 0 ? QString::number(m_cpuPackageCount) : QStringLiteral("N/A"))
            .arg(m_cpuPhysicalCoreCount > 0 ? QString::number(m_cpuPhysicalCoreCount) : QStringLiteral("N/A"))
            .arg(m_cpuLogicalCoreCount > 0 ? QString::number(m_cpuLogicalCoreCount) : QStringLiteral("N/A"))
            .arg(m_cpuL1CacheBytes > 0 ? bytesToReadableText(static_cast<double>(m_cpuL1CacheBytes)) : QStringLiteral("N/A"))
            .arg(m_cpuL2CacheBytes > 0 ? bytesToReadableText(static_cast<double>(m_cpuL2CacheBytes)) : QStringLiteral("N/A"))
            .arg(m_cpuL3CacheBytes > 0 ? bytesToReadableText(static_cast<double>(m_cpuL3CacheBytes)) : QStringLiteral("N/A")));
    }

    MEMORYSTATUSEX memoryStatus{};
    memoryStatus.dwLength = sizeof(memoryStatus);
    const bool memoryStatusOk = (::GlobalMemoryStatusEx(&memoryStatus) == TRUE);
    if (memoryStatusOk)
    {
        const double totalGiB = static_cast<double>(memoryStatus.ullTotalPhys) / (1024.0 * 1024.0 * 1024.0);
        const double availableGiB = static_cast<double>(memoryStatus.ullAvailPhys) / (1024.0 * 1024.0 * 1024.0);
        const double usedGiB = totalGiB - availableGiB;
        if (m_memoryCapacityLabel != nullptr)
        {
            m_memoryCapacityLabel->setText(QStringLiteral("%1 GB").arg(totalGiB, 0, 'f', 1));
        }
        if (m_memoryUtilPrimaryDetailLabel != nullptr)
        {
            m_memoryUtilPrimaryDetailLabel->setText(
                QStringLiteral(
                    "使用中(含缓存): %1 GB\n"
                    "可用: %2 GB\n"
                    "已提交: %3 / %4\n"
                    "已缓存: %5\n"
                    "分页池: %6\n"
                    "非分页池: %7")
                .arg(usedGiB, 0, 'f', 1)
                .arg(availableGiB, 0, 'f', 1)
                .arg(perfOk ? bytesToReadableText(static_cast<double>(perfSnapshot.commitTotalBytes)) : QStringLiteral("N/A"))
                .arg(perfOk ? bytesToReadableText(static_cast<double>(perfSnapshot.commitLimitBytes)) : QStringLiteral("N/A"))
                .arg(perfOk ? bytesToReadableText(static_cast<double>(perfSnapshot.cachedBytes)) : QStringLiteral("N/A"))
                .arg(perfOk ? bytesToReadableText(static_cast<double>(perfSnapshot.pagedPoolBytes)) : QStringLiteral("N/A"))
                .arg(perfOk ? bytesToReadableText(static_cast<double>(perfSnapshot.nonPagedPoolBytes)) : QStringLiteral("N/A")));
        }
    }

    if (m_memoryUtilSecondaryDetailLabel != nullptr)
    {
        ULONGLONG installedMemoryKb = 0;
        ::GetPhysicallyInstalledSystemMemory(&installedMemoryKb);
        const double installedBytes = static_cast<double>(installedMemoryKb) * 1024.0;
        const double reservedBytes = memoryStatusOk
            ? std::max(0.0, installedBytes - static_cast<double>(memoryStatus.ullTotalPhys))
            : 0.0;
        m_memoryUtilSecondaryDetailLabel->setText(
            QStringLiteral(
                "速度: %1 MHz\n"
                "已使用插槽: %2/%3\n"
                "外形规格: %4\n"
                "硬件保留内存: %5")
            .arg(m_memorySpeedMhz > 0 ? QString::number(m_memorySpeedMhz) : QStringLiteral("N/A"))
            .arg(m_memorySlotUsed > 0 ? QString::number(m_memorySlotUsed) : QStringLiteral("N/A"))
            .arg(m_memorySlotTotal > 0 ? QString::number(m_memorySlotTotal) : QStringLiteral("N/A"))
            .arg(m_memoryFormFactorText.isEmpty() ? QStringLiteral("N/A") : m_memoryFormFactorText)
            .arg(bytesToReadableText(reservedBytes)));
    }

    if ((m_sampleCounter % 15) == 1)
    {
        refreshSystemVolumeInfo();
    }
    if (m_diskUtilDetailLabel != nullptr)
    {
        const double diskTotalRate = std::max(0.0, diskReadBytesPerSec) + std::max(0.0, diskWriteBytesPerSec);
        const double diskApproxPercent = std::clamp(
            diskTotalRate / std::max(1.0, m_diskNavAutoScaleBytesPerSec) * 100.0,
            0.0,
            100.0);
        m_diskUtilDetailLabel->setText(
            QStringLiteral(
                "活动时间(近似): %1%\n"
                "读取速度: %2\n"
                "写入速度: %3\n"
                "系统卷: %4\n"
                "总容量: %5\n"
                "可用: %6")
            .arg(diskApproxPercent, 0, 'f', 1)
            .arg(formatRateText(diskReadBytesPerSec))
            .arg(formatRateText(diskWriteBytesPerSec))
            .arg(m_systemVolumeText.isEmpty() ? QStringLiteral("N/A") : m_systemVolumeText)
            .arg(m_systemVolumeTotalBytes > 0
                ? bytesToReadableText(static_cast<double>(m_systemVolumeTotalBytes))
                : QStringLiteral("N/A"))
            .arg(m_systemVolumeFreeBytes > 0
                ? bytesToReadableText(static_cast<double>(m_systemVolumeFreeBytes))
                : QStringLiteral("N/A")));
    }

    if (m_networkUtilDetailLabel != nullptr)
    {
        const QString adapterText = m_primaryNetworkAdapterName.isEmpty()
            ? QStringLiteral("N/A")
            : m_primaryNetworkAdapterName;
        const double linkMbps = static_cast<double>(m_primaryNetworkLinkBitsPerSecond) / (1000.0 * 1000.0);
        m_networkUtilDetailLabel->setText(
            QStringLiteral(
                "适配器: %1\n"
                "发送: %2\n"
                "接收: %3\n"
                "链路速度: %4 Mbps")
            .arg(adapterText)
            .arg(formatRateText(networkTxBytesPerSec))
            .arg(formatRateText(networkRxBytesPerSec))
            .arg(linkMbps > 0.0 ? QString::number(linkMbps, 'f', 1) : QStringLiteral("N/A")));
    }

    if (m_gpuAdapterTitleLabel != nullptr)
    {
        m_gpuAdapterTitleLabel->setText(
            m_gpuAdapterNameText.isEmpty() ? QStringLiteral("N/A") : m_gpuAdapterNameText);
    }
    if (m_gpuDedicatedMemoryChartView != nullptr
        && m_gpuDedicatedMemoryChartView->chart() != nullptr)
    {
        m_gpuDedicatedMemoryChartView->chart()->setTitle(
            QStringLiteral("专用 GPU 内存利用率  %1 / %2 GiB")
            .arg(m_gpuDedicatedUsedGiB, 0, 'f', 2)
            .arg(m_gpuDedicatedBudgetGiB > 0.0 ? m_gpuDedicatedBudgetGiB : m_gpuDedicatedMemoryGiB, 0, 'f', 2));
    }
    if (m_gpuSharedMemoryChartView != nullptr
        && m_gpuSharedMemoryChartView->chart() != nullptr)
    {
        m_gpuSharedMemoryChartView->chart()->setTitle(
            QStringLiteral("共享 GPU 内存利用率  %1 / %2 GiB")
            .arg(m_gpuSharedUsedGiB, 0, 'f', 2)
            .arg(m_gpuSharedBudgetGiB, 0, 'f', 2));
    }

    if (m_gpuUtilDetailLabel != nullptr)
    {
        m_gpuUtilDetailLabel->setText(
            QStringLiteral(
                "利用率: %1%\n"
                "3D: %2%   Copy: %3%   Video Encode: %4%   Video Decode: %5%\n"
                "专用显存: %6 / %7 GiB\n"
                "共享显存: %8 / %9 GiB\n"
                "驱动版本: %10\n"
                "驱动日期: %11\n"
                "PNP: %12")
            .arg(gpuUsagePercent, 0, 'f', 1)
            .arg(m_gpuUsage3DPercent, 0, 'f', 1)
            .arg(m_gpuUsageCopyPercent, 0, 'f', 1)
            .arg(m_gpuUsageVideoEncodePercent, 0, 'f', 1)
            .arg(m_gpuUsageVideoDecodePercent, 0, 'f', 1)
            .arg(m_gpuDedicatedUsedGiB, 0, 'f', 2)
            .arg((m_gpuDedicatedBudgetGiB > 0.0 ? m_gpuDedicatedBudgetGiB : m_gpuDedicatedMemoryGiB), 0, 'f', 2)
            .arg(m_gpuSharedUsedGiB, 0, 'f', 2)
            .arg(m_gpuSharedBudgetGiB, 0, 'f', 2)
            .arg(m_gpuDriverVersionText.isEmpty() ? QStringLiteral("N/A") : m_gpuDriverVersionText)
            .arg(m_gpuDriverDateText.isEmpty() ? QStringLiteral("N/A") : m_gpuDriverDateText)
            .arg(m_gpuPnpDeviceIdText.isEmpty() ? QStringLiteral("N/A") : m_gpuPnpDeviceIdText));
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
    if (chartEntry.lineSeries == nullptr
        || chartEntry.baselineSeries == nullptr
        || chartEntry.axisX == nullptr
        || chartEntry.axisY == nullptr)
    {
        return;
    }

    chartEntry.lineSeries->append(m_sampleCounter, usagePercent);
    chartEntry.baselineSeries->append(m_sampleCounter, 0.0);
    while (chartEntry.lineSeries->count() > m_historyLength)
    {
        chartEntry.lineSeries->remove(0);
    }
    while (chartEntry.baselineSeries->count() > m_historyLength)
    {
        chartEntry.baselineSeries->remove(0);
    }

    const QList<QPointF> pointList = chartEntry.lineSeries->points();
    if (!pointList.isEmpty())
    {
        const double firstX = pointList.first().x();
        const double lastX = pointList.last().x();
        if (qFuzzyCompare(firstX, lastX))
        {
            chartEntry.axisX->setRange(firstX - 1.0, lastX + 1.0);
        }
        else
        {
            chartEntry.axisX->setRange(firstX, lastX);
        }
    }
    chartEntry.axisY->setRange(0.0, 100.0);
}

void HardwareDock::appendGeneralSeriesPoint(
    QLineSeries* lineSeries,
    QValueAxis* axisX,
    QValueAxis* axisY,
    const double sampleValue,
    const double minAxisYValue)
{
    if (lineSeries == nullptr || axisX == nullptr || axisY == nullptr)
    {
        return;
    }

    lineSeries->append(m_sampleCounter, sampleValue);
    while (lineSeries->count() > m_historyLength)
    {
        lineSeries->remove(0);
    }

    const QList<QPointF> pointList = lineSeries->points();
    if (pointList.isEmpty())
    {
        return;
    }

    const double firstX = pointList.first().x();
    const double lastX = pointList.last().x();
    if (qFuzzyCompare(firstX, lastX))
    {
        axisX->setRange(firstX - 1.0, lastX + 1.0);
    }
    else
    {
        axisX->setRange(firstX, lastX);
    }
    double maxYValue = minAxisYValue + 1.0;
    for (const QPointF& pointValue : pointList)
    {
        maxYValue = std::max(maxYValue, pointValue.y());
    }
    axisY->setRange(minAxisYValue, maxYValue * 1.15);
}

void HardwareDock::appendFilledSeriesPoint(
    QLineSeries* lineSeries,
    QLineSeries* baselineSeries,
    QValueAxis* axisX,
    QValueAxis* axisY,
    const double sampleValue,
    const double minAxisYValue)
{
    if (lineSeries == nullptr
        || baselineSeries == nullptr
        || axisX == nullptr
        || axisY == nullptr)
    {
        return;
    }

    // lineSeries 用途：保存真实采样曲线；baselineSeries 用途：保存同一 X 坐标上的下边界。
    lineSeries->append(m_sampleCounter, sampleValue);
    baselineSeries->append(m_sampleCounter, minAxisYValue);
    while (lineSeries->count() > m_historyLength)
    {
        lineSeries->remove(0);
    }
    while (baselineSeries->count() > m_historyLength)
    {
        baselineSeries->remove(0);
    }

    const QList<QPointF> pointList = lineSeries->points();
    if (pointList.isEmpty())
    {
        return;
    }

    const double firstX = pointList.first().x();
    const double lastX = pointList.last().x();
    if (qFuzzyCompare(firstX, lastX))
    {
        axisX->setRange(firstX - 1.0, lastX + 1.0);
    }
    else
    {
        axisX->setRange(firstX, lastX);
    }

    // maxYValue 用途：按单条曲线可见历史设置纵轴上限，共轴双线稍后由 updateSharedSeriesAxisRange 再统一。
    double maxYValue = minAxisYValue + 1.0;
    for (const QPointF& pointValue : pointList)
    {
        maxYValue = std::max(maxYValue, pointValue.y());
    }
    axisY->setRange(minAxisYValue, maxYValue * 1.15);
}

void HardwareDock::updateSharedSeriesAxisRange(
    QLineSeries* primaryLineSeries,
    QLineSeries* secondaryLineSeries,
    QValueAxis* axisX,
    QValueAxis* axisY,
    const double minAxisYValue)
{
    if (axisX == nullptr || axisY == nullptr)
    {
        return;
    }

    const QList<QPointF> primaryPointList =
        primaryLineSeries != nullptr ? primaryLineSeries->points() : QList<QPointF>();
    const QList<QPointF> secondaryPointList =
        secondaryLineSeries != nullptr ? secondaryLineSeries->points() : QList<QPointF>();
    if (primaryPointList.isEmpty() && secondaryPointList.isEmpty())
    {
        return;
    }

    // firstXValue 用途：两条曲线可见区域的最左侧采样 X 值。
    double firstXValue = std::numeric_limits<double>::max();
    // lastXValue 用途：两条曲线可见区域的最右侧采样 X 值。
    double lastXValue = std::numeric_limits<double>::lowest();
    // maxYValue 用途：两条曲线当前可见历史中的共同最大值。
    double maxYValue = minAxisYValue + 1.0;

    const auto accumulatePointRange =
        [&firstXValue, &lastXValue, &maxYValue](const QList<QPointF>& pointList)
        {
            if (pointList.isEmpty())
            {
                return;
            }

            firstXValue = std::min(firstXValue, pointList.first().x());
            lastXValue = std::max(lastXValue, pointList.last().x());
            for (const QPointF& pointValue : pointList)
            {
                maxYValue = std::max(maxYValue, pointValue.y());
            }
        };

    accumulatePointRange(primaryPointList);
    accumulatePointRange(secondaryPointList);

    if (qFuzzyCompare(firstXValue, lastXValue))
    {
        axisX->setRange(firstXValue - 1.0, lastXValue + 1.0);
    }
    else
    {
        axisX->setRange(firstXValue, lastXValue);
    }
    axisY->setRange(minAxisYValue, maxYValue * 1.15);
}

void HardwareDock::rebuildDualRateNavCard(
    PerformanceNavCard* navCard,
    std::vector<double>* primaryHistoryOut,
    std::vector<double>* secondaryHistoryOut,
    const double primaryBytesPerSecond,
    const double secondaryBytesPerSecond,
    double* upperBoundBytesPerSecondOut,
    const QString& subtitleText)
{
    if (navCard == nullptr
        || primaryHistoryOut == nullptr
        || secondaryHistoryOut == nullptr
        || upperBoundBytesPerSecondOut == nullptr)
    {
        return;
    }

    // safePrimaryBytesPerSecond 用途：主序列安全速率值，过滤异常负值。
    const double safePrimaryBytesPerSecond = std::max(0.0, primaryBytesPerSecond);
    // safeSecondaryBytesPerSecond 用途：次序列安全速率值，过滤异常负值。
    const double safeSecondaryBytesPerSecond = std::max(0.0, secondaryBytesPerSecond);
    const int historyCapacity = std::max(1, navCard->sampleCapacity());

    primaryHistoryOut->push_back(safePrimaryBytesPerSecond);
    secondaryHistoryOut->push_back(safeSecondaryBytesPerSecond);
    while (static_cast<int>(primaryHistoryOut->size()) > historyCapacity)
    {
        primaryHistoryOut->erase(primaryHistoryOut->begin());
    }
    while (static_cast<int>(secondaryHistoryOut->size()) > historyCapacity)
    {
        secondaryHistoryOut->erase(secondaryHistoryOut->begin());
    }

    // historyPeakBytesPerSecond 用途：缩略图可见历史中的真实峰值。
    double historyPeakBytesPerSecond = 0.0;
    for (const double historyValue : *primaryHistoryOut)
    {
        historyPeakBytesPerSecond = std::max(historyPeakBytesPerSecond, historyValue);
    }
    for (const double historyValue : *secondaryHistoryOut)
    {
        historyPeakBytesPerSecond = std::max(historyPeakBytesPerSecond, historyValue);
    }

    // 加一点顶部留白，避免峰值直接顶到边框。
    *upperBoundBytesPerSecondOut = std::max(1.0, historyPeakBytesPerSecond * 1.08);

    QVector<double> primaryPercentSampleList;
    QVector<double> secondaryPercentSampleList;
    primaryPercentSampleList.reserve(static_cast<int>(primaryHistoryOut->size()));
    secondaryPercentSampleList.reserve(static_cast<int>(secondaryHistoryOut->size()));
    for (const double historyValue : *primaryHistoryOut)
    {
        primaryPercentSampleList.push_back(std::clamp(
            historyValue / *upperBoundBytesPerSecondOut * 100.0,
            0.0,
            100.0));
    }
    for (const double historyValue : *secondaryHistoryOut)
    {
        secondaryPercentSampleList.push_back(std::clamp(
            historyValue / *upperBoundBytesPerSecondOut * 100.0,
            0.0,
            100.0));
    }

    navCard->setSubtitleText(subtitleText);
    navCard->setSampleSeries(primaryPercentSampleList, secondaryPercentSampleList);
}

QString HardwareDock::formatRateText(const double bytesPerSecondValue) const
{
    return bytesPerSecondToText(bytesPerSecondValue);
}

void HardwareDock::refreshStaticHardwareTexts(const bool forceRefresh)
{
    if (forceRefresh)
    {
        requestAsyncStaticInfoRefresh();
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

void HardwareDock::requestAsyncStaticInfoRefresh()
{
    // expectedFlag 用途：原子刷新锁 CAS 期望值（false=当前无任务）。
    bool expectedFlag = false;
    if (!m_staticInfoRefreshing.compare_exchange_strong(expectedFlag, true))
    {
        return;
    }

    QPointer<HardwareDock> safeThis(this);
    std::thread([safeThis]() {
        const QString overviewBaseText = buildOverviewStaticTextSnapshot();
        const QString peripheralOverviewText = buildOverviewPeripheralTextSnapshot();
        const QString overviewText = overviewBaseText
            + QStringLiteral("\n[硬件设备总览]\n")
            + peripheralOverviewText;
        const QString gpuText = buildGpuStaticTextSnapshot();
        const QString memoryText = buildMemoryStaticTextSnapshot();
        const MemoryHardwareSummarySnapshot memorySummary = queryMemoryHardwareSummarySnapshot();
        const GpuHardwareSummarySnapshot gpuSummary = queryGpuHardwareSummarySnapshot();

        if (safeThis.isNull())
        {
            return;
        }

        const bool invokeOk = QMetaObject::invokeMethod(
            safeThis.data(),
            [safeThis, overviewText, gpuText, memoryText, memorySummary, gpuSummary]()
            {
                if (safeThis.isNull())
                {
                    return;
                }

                safeThis->m_cachedOverviewStaticText = overviewText;
                safeThis->m_cachedGpuStaticText = gpuText;
                safeThis->m_cachedMemoryStaticText = memoryText;
                safeThis->m_memorySpeedMhz = memorySummary.speedMhz;
                safeThis->m_memorySlotUsed = memorySummary.usedSlots;
                safeThis->m_memorySlotTotal = memorySummary.totalSlots;
                safeThis->m_memoryFormFactorText = memorySummary.formFactorText;
                safeThis->m_gpuAdapterNameText = gpuSummary.adapterNameText;
                safeThis->m_gpuDriverVersionText = gpuSummary.driverVersionText;
                safeThis->m_gpuDriverDateText = gpuSummary.driverDateText;
                safeThis->m_gpuPnpDeviceIdText = gpuSummary.pnpDeviceIdText;
                safeThis->m_gpuDedicatedMemoryGiB = gpuSummary.dedicatedMemoryGiB;
                safeThis->refreshStaticHardwareTexts(false);
                safeThis->m_staticInfoRefreshing.store(false);
            },
            Qt::QueuedConnection);

        if (!invokeOk && !safeThis.isNull())
        {
            safeThis->m_staticInfoRefreshing.store(false);
        }
        }).detach();
}

void HardwareDock::requestAsyncSensorRefresh()
{
    // expectedFlag 用途：原子刷新锁 CAS 期望值（false=当前无任务）。
    bool expectedFlag = false;
    if (!m_sensorRefreshing.compare_exchange_strong(expectedFlag, true))
    {
        return;
    }

    // event 用途：串联本次 CPU 传感器读取与日志输出，便于追踪失败原因。
    kLogEvent event;
    QPointer<HardwareDock> safeThis(this);
    std::thread([safeThis, event]() {
        const SensorProbeResult temperatureProbeResult = queryCpuTemperatureProbeResult();
        const SensorProbeResult voltageProbeResult = queryCpuVoltageProbeResult();
        if (safeThis.isNull())
        {
            return;
        }

        const bool invokeOk = QMetaObject::invokeMethod(
            safeThis.data(),
            [safeThis, event, temperatureProbeResult, voltageProbeResult]()
            {
                if (safeThis.isNull())
                {
                    return;
                }

                // previousSensorPartList 用途：拆分上一份缓存，分别保留温度/电压的最后有效值。
                const QStringList previousSensorPartList = safeThis->m_cachedSensorText.split('|');
                QString cachedTemperatureText =
                    previousSensorPartList.size() >= 1 ? previousSensorPartList.at(0) : QStringLiteral("N/A");
                QString cachedVoltageText =
                    previousSensorPartList.size() >= 2 ? previousSensorPartList.at(1) : QStringLiteral("N/A");

                if (isReadableSensorValue(temperatureProbeResult.valueText))
                {
                    cachedTemperatureText = temperatureProbeResult.valueText;
                }
                if (isReadableSensorValue(voltageProbeResult.valueText))
                {
                    cachedVoltageText = voltageProbeResult.valueText;
                }
                if (!isReadableSensorValue(cachedTemperatureText))
                {
                    cachedTemperatureText = QStringLiteral("N/A");
                }
                if (!isReadableSensorValue(cachedVoltageText))
                {
                    cachedVoltageText = QStringLiteral("N/A");
                }

                safeThis->m_cachedSensorText = QStringLiteral("%1|%2")
                    .arg(cachedTemperatureText)
                    .arg(cachedVoltageText);

                // previousLogSignatureText 用途：上一轮日志签名，用于控制失败/恢复日志去重。
                const QString previousLogSignatureText = safeThis->m_lastSensorLogSignatureText;
                const QString logSignatureText =
                    buildSensorProbeSignatureText(QStringLiteral("温度"), temperatureProbeResult)
                    + QStringLiteral("||")
                    + buildSensorProbeSignatureText(QStringLiteral("电压"), voltageProbeResult);
                if (logSignatureText != previousLogSignatureText)
                {
                    safeThis->m_lastSensorLogSignatureText = logSignatureText;

                    // hasUnexpectedFailure 用途：只把脚本失败、权限异常、执行超时等真正异常升为 WARN。
                    // allProbeSucceeded 用途：只有温度/电压均恢复可读时才输出恢复日志，避免 N/A 常态被误报。
                    const bool hasUnexpectedFailure =
                        (!temperatureProbeResult.success && !temperatureProbeResult.expectedUnavailable)
                        || (!voltageProbeResult.success && !voltageProbeResult.expectedUnavailable);
                    const bool allProbeSucceeded = temperatureProbeResult.success && voltageProbeResult.success;
                    if (hasUnexpectedFailure)
                    {
                        warn << event
                             << "[HardwareDock] CPU传感器读取失败："
                             << buildSensorProbeLogFragment(QStringLiteral("温度"), temperatureProbeResult)
                             << "；"
                             << buildSensorProbeLogFragment(QStringLiteral("电压"), voltageProbeResult)
                             << eol;
                    }
                    else if (allProbeSucceeded && !previousLogSignatureText.isEmpty())
                    {
                        info << event
                             << "[HardwareDock] CPU传感器读取恢复："
                             << buildSensorProbeLogFragment(QStringLiteral("温度"), temperatureProbeResult)
                             << "；"
                             << buildSensorProbeLogFragment(QStringLiteral("电压"), voltageProbeResult)
                             << eol;
                    }
                }
                safeThis->m_sensorRefreshing.store(false);
            },
            Qt::QueuedConnection);

        if (!invokeOk && !safeThis.isNull())
        {
            warn << event << "[HardwareDock] CPU传感器结果回投UI线程失败。" << eol;
            safeThis->m_sensorRefreshing.store(false);
        }
        }).detach();
}

QString HardwareDock::buildOverviewStaticText() const
{
    return buildOverviewStaticTextSnapshot();
}

QString HardwareDock::buildGpuStaticText() const
{
    return buildGpuStaticTextSnapshot();
}

QString HardwareDock::buildMemoryStaticText() const
{
    return buildMemoryStaticTextSnapshot();
}

QString HardwareDock::buildCpuSensorText(const bool forceRefresh)
{
    // 强制刷新场景改为“异步触发”，保证调用方不阻塞 UI 线程。
    if (forceRefresh)
    {
        requestAsyncSensorRefresh();
    }

    if (!m_cachedSensorText.isEmpty())
    {
        return m_cachedSensorText;
    }
    return QStringLiteral("N/A|N/A");
}
