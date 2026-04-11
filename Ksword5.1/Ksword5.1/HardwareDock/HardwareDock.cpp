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
#include "../UI/PerformanceNavCard.h"

#include <QAbstractScrollArea>
#include <QAbstractItemView>
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
#include <QPointer>
#include <QProcess>
#include <QResizeEvent>
#include <QScrollArea>
#include <QShowEvent>
#include <QStackedWidget>
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
        chartView->setMinimumHeight(56);
        appendTransparentBackgroundStyle(chartView);
        return chartView;
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

    // extractSensorValueFromOutput 作用：
    // - 从 PowerShell 文本中提取第一行可用值；
    // - 若输出为空、超时或失败文本，则回退为 N/A。
    QString extractSensorValueFromOutput(const QString& rawOutputText)
    {
        const QString firstLineText = rawOutputText
            .split('\n', Qt::SkipEmptyParts)
            .value(0)
            .trimmed();
        if (firstLineText.isEmpty())
        {
            return QStringLiteral("N/A");
        }
        if (firstLineText == QStringLiteral("<无输出>")
            || firstLineText.contains(QStringLiteral("PowerShell"))
            || firstLineText.contains(QStringLiteral("失败"))
            || firstLineText.contains(QStringLiteral("超时")))
        {
            return QStringLiteral("N/A");
        }
        return firstLineText;
    }

    // queryCpuTemperatureText 作用：
    // - 查询 CPU 温度第一可用值（单位 °C）；
    // - 按“Libre/OpenHardwareMonitor -> ACPI -> Thermal Zone 计数器”顺序回退；
    // - 不可读时返回 N/A。
    QString queryCpuTemperatureText()
    {
        const QString temperatureScript = QStringLiteral(
            "$ErrorActionPreference='SilentlyContinue'; "
            "function Format-Temp([double]$value){ "
            "  if([double]::IsNaN($value) -or [double]::IsInfinity($value)){return $null}; "
            "  if($value -lt -30 -or $value -gt 130){return $null}; "
            "  return ([math]::Round($value,1)).ToString() + '°C'; "
            "}; "
            "$result=$null; "
            "$sensorNamespaces=@('root/LibreHardwareMonitor','root/OpenHardwareMonitor'); "
            "foreach($ns in $sensorNamespaces){ "
            "  if($result){break}; "
            "  $sensors=Get-CimInstance -Namespace $ns -ClassName Sensor -ErrorAction SilentlyContinue; "
            "  if($null -eq $sensors){continue}; "
            "  $cpuTemps=$sensors | Where-Object { "
            "    $_.SensorType -eq 'Temperature' -and "
            "    (($_.Name -match 'Package|CPU') -or ($_.Identifier -match '/cpu')) "
            "  } | Sort-Object @{Expression={if($_.Name -match 'Package'){0}else{1}}}, Name; "
            "  foreach($sensor in $cpuTemps){ "
            "    $temp=Format-Temp ([double]$sensor.Value); "
            "    if($null -ne $temp){$result=$temp; break}; "
            "  } "
            "}; "
            "if(-not $result){ "
            "  $zoneTemps=Get-CimInstance -Namespace root/wmi -ClassName MSAcpi_ThermalZoneTemperature -ErrorAction SilentlyContinue | "
            "    Select-Object -ExpandProperty CurrentTemperature; "
            "  foreach($raw in $zoneTemps){ "
            "    if($null -eq $raw){continue}; "
            "    $temp=Format-Temp ((([double]$raw)/10.0)-273.15); "
            "    if($null -ne $temp){$result=$temp; break}; "
            "  } "
            "}; "
            "if(-not $result){ "
            "  $counter=Get-Counter '\\Thermal Zone Information(*)\\Temperature' -ErrorAction SilentlyContinue; "
            "  if($null -ne $counter){ "
            "    foreach($sample in $counter.CounterSamples){ "
            "      $raw=[double]$sample.CookedValue; "
            "      if($raw -gt 200){$raw=($raw/10.0)-273.15}; "
            "      $temp=Format-Temp $raw; "
            "      if($null -ne $temp){$result=$temp; break}; "
            "    } "
            "  } "
            "}; "
            "if($result){$result}else{'N/A'}");
        return extractSensorValueFromOutput(queryPowerShellTextSync(temperatureScript, 4200));
    }

    // queryCpuVoltageText 作用：
    // - 查询 CPU 电压第一可用值（单位 V）；
    // - 不可读时返回 N/A。
    QString queryCpuVoltageText()
    {
        const QString voltageScript = QStringLiteral(
            "$v=Get-CimInstance Win32_Processor | Select-Object -First 1 -ExpandProperty CurrentVoltage; "
            "if($null -eq $v -or $v -eq 0){'N/A'}else{([math]::Round($v*0.1,2)).ToString() + 'V'}");
        return extractSensorValueFromOutput(queryPowerShellTextSync(voltageScript, 2200));
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

    // 启动阶段先填充占位文本，避免首帧等待 PowerShell 导致窗口卡住。
    m_cachedOverviewStaticText = QStringLiteral("硬件概览加载中，请稍候...");
    m_cachedGpuStaticText = QStringLiteral("显卡信息加载中，请稍候...");
    m_cachedMemoryStaticText = QStringLiteral("内存信息加载中，请稍候...");
    m_cachedSensorText = QStringLiteral("N/A|N/A");
    if (m_cpuModelLabel != nullptr)
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

    // 首次显示阶段延迟一帧再重排，确保滚动区 viewport 高度已经稳定。
    QTimer::singleShot(0, this, [this]()
    {
        adjustUtilizationChartHeights();
    });
    // 某些 Dock 场景首次显示后仍会继续布局，补一帧延迟可避免 CPU 子页首帧挤压。
    QTimer::singleShot(80, this, [this]()
    {
        adjustUtilizationChartHeights();
    });
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
                // 进入“利用率”总页时刷新一次高度，修复首次进入 CPU 子页时尚未正确撑开的问题。
                adjustUtilizationChartHeights();
                QTimer::singleShot(0, this, [this]()
                {
                    adjustUtilizationChartHeights();
                });
                QTimer::singleShot(80, this, [this]()
                {
                    adjustUtilizationChartHeights();
                });
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
    m_utilizationSidebarList->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    m_utilizationSidebarList->setVerticalScrollMode(QAbstractItemView::ScrollPerPixel);
    m_utilizationSidebarList->setSelectionMode(QAbstractItemView::SingleSelection);
    m_utilizationSidebarList->setSpacing(3);
    m_utilizationSidebarList->setFixedWidth(286);
    m_utilizationSidebarList->setStyleSheet(
        QStringLiteral(
            "QListWidget{border:none;background:transparent;}"
            "QListWidget::item{border:none;padding:0px;margin:0px;}"
            "QListWidget::item:selected{background:transparent;}"));
    appendTransparentBackgroundStyle(m_utilizationSidebarList);
    m_utilizationBodyLayout->addWidget(m_utilizationSidebarList, 0);

    m_utilizationDetailStack = new QStackedWidget(m_utilizationPage);
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

    // addCardItem 作用：
    // - 向左侧导航列表追加一个性能卡片；
    // - 卡片包含标题、副标题和缩略折线，后续按采样动态刷新。
    auto addCardItem =
        [this](PerformanceNavCard*& cardOut, const QString& titleText, const QColor& accentColor)
        {
            QListWidgetItem* itemPointer = new QListWidgetItem();
            cardOut = new PerformanceNavCard(m_utilizationSidebarList);
            cardOut->setTitleText(titleText);
            cardOut->setSubtitleText(QStringLiteral("采样中..."));
            cardOut->setAccentColor(accentColor);
            itemPointer->setSizeHint(cardOut->sizeHint());
            m_utilizationSidebarList->addItem(itemPointer);
            m_utilizationSidebarList->setItemWidget(itemPointer, cardOut);
        };

    addCardItem(m_cpuNavCard, QStringLiteral("CPU"), QColor(90, 178, 255));
    addCardItem(m_memoryNavCard, QStringLiteral("内存"), QColor(184, 99, 255));
    addCardItem(m_diskNavCard, QStringLiteral("磁盘"), QColor(104, 204, 116));
    addCardItem(m_networkNavCard, QStringLiteral("以太网"), QColor(230, 149, 76));
    addCardItem(m_gpuNavCard, QStringLiteral("GPU"), QColor(105, 173, 255));

    // 磁盘/网络缩略图改为双折线：
    // - 蓝线表示读取/下行；
    // - 黄线表示写入/上行。
    if (m_diskNavCard != nullptr)
    {
        m_diskNavCard->setSeriesColors(QColor(80, 170, 255), QColor(255, 190, 105));
    }
    if (m_networkNavCard != nullptr)
    {
        m_networkNavCard->setSeriesColors(QColor(80, 170, 255), QColor(255, 190, 105));
    }
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

    const int boundedIndex = std::clamp(selectedRowIndex, 0, pageCount - 1);
    m_utilizationDetailStack->setCurrentIndex(boundedIndex);

    if (m_cpuNavCard != nullptr)
    {
        m_cpuNavCard->setSelectedState(boundedIndex == 0);
    }
    if (m_memoryNavCard != nullptr)
    {
        m_memoryNavCard->setSelectedState(boundedIndex == 1);
    }
    if (m_diskNavCard != nullptr)
    {
        m_diskNavCard->setSelectedState(boundedIndex == 2);
    }
    if (m_networkNavCard != nullptr)
    {
        m_networkNavCard->setSelectedState(boundedIndex == 3);
    }
    if (m_gpuNavCard != nullptr)
    {
        m_gpuNavCard->setSelectedState(boundedIndex == 4);
    }

    // 选项切换后立即重算大图高度，避免首帧出现滚动条。
    adjustUtilizationChartHeights();
    // 延迟到事件循环末尾再重算一次，确保拿到切页后的真实 viewport 高度。
    QTimer::singleShot(0, this, [this]()
    {
        adjustUtilizationChartHeights();
    });
    // 补一次短延迟重排，规避 Dock 动画/布局链导致的 CPU 首帧高度过小。
    QTimer::singleShot(80, this, [this]()
    {
        adjustUtilizationChartHeights();
    });
}

void HardwareDock::adjustUtilizationChartHeights()
{
    // ===================== CPU 页：按核心网格动态压缩高度 =====================
    if (m_utilizationCpuSubPage != nullptr
        && m_coreChartHostWidget != nullptr
        && m_coreChartGridLayout != nullptr
        && !m_coreChartEntries.empty())
    {
        // applyFixedHeightIfChanged 作用：
        // - 仅在目标高度变化时写入最小/最大高度，避免无意义重排触发递归 resize；
        // - widgetPointer：待设置控件；heightValue：目标固定高度（像素）。
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

        // chartViewportHeight 用途：滚动区当前可见高度，作为核心网格高度分配基线。
        int chartViewportHeight = 0;
        if (m_coreChartScrollArea != nullptr && m_coreChartScrollArea->viewport() != nullptr)
        {
            chartViewportHeight = m_coreChartScrollArea->viewport()->height();
        }
        if (chartViewportHeight <= 0 && m_coreChartScrollArea != nullptr)
        {
            chartViewportHeight = m_coreChartScrollArea->height();
        }
        if (chartViewportHeight < 120)
        {
            // cpuReferenceHeight 用途：CPU 页稳定参考高度，避免初始化阶段拿到过小 viewport。
            int cpuReferenceHeight = 0;
            if (m_utilizationDetailStack != nullptr)
            {
                cpuReferenceHeight = m_utilizationDetailStack->contentsRect().height();
            }
            if (cpuReferenceHeight <= 0)
            {
                cpuReferenceHeight = m_utilizationCpuSubPage->contentsRect().height();
            }
            chartViewportHeight = std::max(120, cpuReferenceHeight / 2);
        }

        // availableChartAreaHeight 用途：核心图总可用高度；取滚动区可见高度阻断“高度递增反馈环”。
        const int availableChartAreaHeight = std::max(1, chartViewportHeight);
        const int gridRows = std::max(1, m_cpuCoreGridRowCount);
        const int gridSpacing = std::max(0, m_coreChartGridLayout->verticalSpacing());
        // cellHeight 用途：每个逻辑处理器小卡片高度；过小时允许滚动条接管，不强行拉伸父容器。
        const int cellHeight = std::max(
            18,
            (availableChartAreaHeight - gridSpacing * (gridRows - 1)) / gridRows);

        for (CoreChartEntry& chartEntry : m_coreChartEntries)
        {
            if (chartEntry.containerWidget != nullptr)
            {
                applyFixedHeightIfChanged(chartEntry.containerWidget, cellHeight);
            }
            if (chartEntry.chartView != nullptr)
            {
                const int chartHeight = std::max(10, cellHeight - 12);
                applyFixedHeightIfChanged(chartEntry.chartView, chartHeight);
            }
        }

        const int hostHeight = gridRows * cellHeight + gridSpacing * (gridRows - 1);
        applyFixedHeightIfChanged(m_coreChartHostWidget, hostHeight);
        if (m_coreChartScrollArea != nullptr)
        {
            // 这里不再把滚动区本身锁成固定高度，避免布局在每次重算后持续推高父容器。
            m_coreChartScrollArea->setMinimumHeight(0);
            m_coreChartScrollArea->setMaximumHeight(QWIDGETSIZE_MAX);
        }

        if (m_cpuUtilPrimaryDetailLabel != nullptr)
        {
            m_cpuUtilPrimaryDetailLabel->setMaximumHeight(QWIDGETSIZE_MAX);
        }
        if (m_cpuUtilSecondaryDetailLabel != nullptr)
        {
            m_cpuUtilSecondaryDetailLabel->setMaximumHeight(QWIDGETSIZE_MAX);
        }
    }

    // ===================== 其他页：按页面高度比例压缩主图 =====================
    auto adjustMainChartHeight =
        [](
            QWidget* pageWidget,
            QChartView* chartView,
            const double ratioValue,
            const int minHeightValue,
            const int reserveHeightValue)
        {
            if (pageWidget == nullptr || chartView == nullptr)
            {
                return;
            }
            const int pageHeight = pageWidget->contentsRect().height();
            const int maxAllowedHeight = std::max(minHeightValue, pageHeight - reserveHeightValue);
            const int expectedHeight = static_cast<int>(std::round(static_cast<double>(pageHeight) * ratioValue));
            const int finalHeight = std::clamp(expectedHeight, minHeightValue, maxAllowedHeight);
            chartView->setMinimumHeight(finalHeight);
            chartView->setMaximumHeight(finalHeight);
        };

    adjustMainChartHeight(m_utilizationMemorySubPage, m_memoryUtilChartView, 0.36, 56, 116);
    adjustMainChartHeight(m_utilizationDiskSubPage, m_diskUtilChartView, 0.40, 58, 120);
    adjustMainChartHeight(m_utilizationNetworkSubPage, m_networkUtilChartView, 0.40, 58, 120);

    if (m_memoryUtilPrimaryDetailLabel != nullptr)
    {
        m_memoryUtilPrimaryDetailLabel->setMaximumHeight(QWIDGETSIZE_MAX);
    }
    if (m_memoryUtilSecondaryDetailLabel != nullptr)
    {
        m_memoryUtilSecondaryDetailLabel->setMaximumHeight(QWIDGETSIZE_MAX);
    }
    if (m_diskUtilDetailLabel != nullptr)
    {
        m_diskUtilDetailLabel->setMaximumHeight(QWIDGETSIZE_MAX);
    }
    if (m_networkUtilDetailLabel != nullptr)
    {
        m_networkUtilDetailLabel->setMaximumHeight(QWIDGETSIZE_MAX);
    }

    // GPU 页：四个引擎图 + 两条显存曲线全部动态压缩。
    if (m_utilizationGpuSubPage != nullptr)
    {
        // applyMaxHeightIfChanged 作用：
        // - 仅调整最大高度，最小高度保持 0，防止子控件反向撑大父布局；
        // - widgetPointer：目标控件；maxHeightValue：目标最大高度（像素）。
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
            44);
        const int summaryHeight = m_gpuUtilSummaryLabel != nullptr
            ? m_gpuUtilSummaryLabel->sizeHint().height()
            : 20;
        const int detailHeight = m_gpuUtilDetailLabel != nullptr
            ? m_gpuUtilDetailLabel->sizeHint().height()
            : 22;
        // reservedHeight 用途：GPU 页非图表区预留高度（含布局间距与上下边距）。
        const int reservedHeight = titleHeight + summaryHeight + detailHeight + 38;
        const int availableHeight = std::max(120, gpuReferenceHeight - reservedHeight);

        // engineAreaHeight 用途：分配给 2x2 引擎图区域的高度。
        const int engineAreaHeight = static_cast<int>(std::round(static_cast<double>(availableHeight) * 0.52));
        const int memoryAreaEachHeight = std::max(34, (availableHeight - engineAreaHeight - 8) / 2);
        if (m_gpuEngineHostWidget != nullptr && m_gpuEngineGridLayout != nullptr)
        {
            const int rowSpacing = std::max(0, m_gpuEngineGridLayout->verticalSpacing());
            const int cellHeight = std::max(30, (engineAreaHeight - rowSpacing) / 2);
            for (GpuEngineChartEntry& chartEntry : m_gpuEngineCharts)
            {
                if (chartEntry.chartView != nullptr)
                {
                    applyMaxHeightIfChanged(chartEntry.chartView, std::max(18, cellHeight - 10));
                }
                if (chartEntry.titleLabel != nullptr)
                {
                    chartEntry.titleLabel->setMinimumHeight(14);
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
            m_gpuUtilDetailLabel->setMaximumHeight(QWIDGETSIZE_MAX);
        }
    }
}

void HardwareDock::initializeUtilizationCpuSubTab()
{
    m_utilizationCpuSubPage = new QWidget(m_utilizationDetailStack);
    appendTransparentBackgroundStyle(m_utilizationCpuSubPage);
    QVBoxLayout* cpuSubLayout = new QVBoxLayout(m_utilizationCpuSubPage);
    cpuSubLayout->setContentsMargins(4, 4, 4, 4);
    cpuSubLayout->setSpacing(6);

    QHBoxLayout* headerLayout = new QHBoxLayout();
    QLabel* titleLabel = new QLabel(QStringLiteral("CPU"), m_utilizationCpuSubPage);
    titleLabel->setStyleSheet(
        QStringLiteral("font-size:46px;font-weight:700;color:%1;")
        .arg(KswordTheme::TextPrimaryHex()));
    m_cpuModelLabel = new QLabel(QStringLiteral("检测中..."), m_utilizationCpuSubPage);
    m_cpuModelLabel->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
    m_cpuModelLabel->setStyleSheet(
        QStringLiteral("font-size:15px;font-weight:500;color:%1;")
        .arg(KswordTheme::TextPrimaryHex()));
    headerLayout->addWidget(titleLabel, 0);
    headerLayout->addStretch(1);
    headerLayout->addWidget(m_cpuModelLabel, 0);
    cpuSubLayout->addLayout(headerLayout, 0);

    m_utilizationSummaryLabel = new QLabel(QStringLiteral("30 秒内的利用率 %"), m_utilizationCpuSubPage);
    m_utilizationSummaryLabel->setStyleSheet(
        QStringLiteral("color:%1;font-size:14px;font-weight:600;").arg(buildStatusColor().name()));
    cpuSubLayout->addWidget(m_utilizationSummaryLabel, 0);

    m_coreChartScrollArea = new QScrollArea(m_utilizationCpuSubPage);
    m_coreChartScrollArea->setWidgetResizable(true);
    m_coreChartScrollArea->setFrameShape(QFrame::NoFrame);
    // 当核心数量很多时允许内部滚动，避免核心图反向把整个 Dock 顶高到屏幕外。
    m_coreChartScrollArea->setVerticalScrollBarPolicy(Qt::ScrollBarAsNeeded);
    m_coreChartScrollArea->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    m_coreChartScrollArea->setSizeAdjustPolicy(QAbstractScrollArea::AdjustIgnored);
    appendTransparentBackgroundStyle(m_coreChartScrollArea);
    m_coreChartHostWidget = new QWidget(m_coreChartScrollArea);
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
    appendTransparentBackgroundStyle(m_utilizationMemorySubPage);
    QVBoxLayout* memorySubLayout = new QVBoxLayout(m_utilizationMemorySubPage);
    memorySubLayout->setContentsMargins(4, 4, 4, 4);
    memorySubLayout->setSpacing(6);

    QHBoxLayout* headerLayout = new QHBoxLayout();
    QLabel* titleLabel = new QLabel(QStringLiteral("内存"), m_utilizationMemorySubPage);
    titleLabel->setStyleSheet(
        QStringLiteral("font-size:46px;font-weight:700;color:%1;")
        .arg(KswordTheme::TextPrimaryHex()));
    m_memoryCapacityLabel = new QLabel(QStringLiteral("读取中..."), m_utilizationMemorySubPage);
    m_memoryCapacityLabel->setStyleSheet(
        QStringLiteral("font-size:31px;font-weight:500;color:%1;")
        .arg(KswordTheme::TextPrimaryHex()));
    headerLayout->addWidget(titleLabel, 0);
    headerLayout->addStretch(1);
    headerLayout->addWidget(m_memoryCapacityLabel, 0);
    memorySubLayout->addLayout(headerLayout, 0);

    m_memoryUtilSummaryLabel = new QLabel(QStringLiteral("内存使用量"), m_utilizationMemorySubPage);
    m_memoryUtilSummaryLabel->setStyleSheet(
        QStringLiteral("color:%1;font-size:14px;font-weight:600;").arg(buildStatusColor().name()));
    memorySubLayout->addWidget(m_memoryUtilSummaryLabel, 0);

    m_memoryUtilLineSeries = new QLineSeries(m_utilizationMemorySubPage);
    m_memoryUtilLineSeries->setColor(QColor(184, 99, 255));
    for (int indexValue = 0; indexValue < m_historyLength; ++indexValue)
    {
        m_memoryUtilLineSeries->append(indexValue, 0.0);
    }

    QChart* memoryChart = new QChart();
    memoryChart->addSeries(m_memoryUtilLineSeries);
    memoryChart->legend()->hide();
    memoryChart->setBackgroundVisible(false);
    memoryChart->setBackgroundRoundness(0);
    memoryChart->setMargins(QMargins(0, 0, 0, 0));
    memoryChart->setTitle(QStringLiteral("内存使用率趋势"));

    m_memoryUtilAxisX = new QValueAxis(memoryChart);
    m_memoryUtilAxisX->setRange(0, m_historyLength);
    m_memoryUtilAxisX->setLabelsVisible(false);
    m_memoryUtilAxisX->setGridLineVisible(true);
    m_memoryUtilAxisX->setGridLineColor(QColor(184, 99, 255, 35));

    m_memoryUtilAxisY = new QValueAxis(memoryChart);
    m_memoryUtilAxisY->setRange(0.0, 100.0);
    m_memoryUtilAxisY->setLabelsVisible(false);
    m_memoryUtilAxisY->setGridLineVisible(true);
    m_memoryUtilAxisY->setGridLineColor(QColor(184, 99, 255, 35));

    memoryChart->addAxis(m_memoryUtilAxisX, Qt::AlignBottom);
    memoryChart->addAxis(m_memoryUtilAxisY, Qt::AlignLeft);
    m_memoryUtilLineSeries->attachAxis(m_memoryUtilAxisX);
    m_memoryUtilLineSeries->attachAxis(m_memoryUtilAxisY);

    m_memoryUtilChartView = createNoFrameChartView(memoryChart, m_utilizationMemorySubPage);
    memorySubLayout->addWidget(m_memoryUtilChartView, 1);

    QHBoxLayout* detailLayout = new QHBoxLayout();
    detailLayout->setSpacing(16);
    m_memoryUtilPrimaryDetailLabel = new QLabel(QStringLiteral("内存参数采样中..."), m_utilizationMemorySubPage);
    m_memoryUtilSecondaryDetailLabel = new QLabel(QStringLiteral("硬件参数读取中..."), m_utilizationMemorySubPage);
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
    appendTransparentBackgroundStyle(m_utilizationDiskSubPage);
    QVBoxLayout* diskSubLayout = new QVBoxLayout(m_utilizationDiskSubPage);
    diskSubLayout->setContentsMargins(4, 4, 4, 4);
    diskSubLayout->setSpacing(6);

    QLabel* titleLabel = new QLabel(QStringLiteral("磁盘"), m_utilizationDiskSubPage);
    titleLabel->setStyleSheet(
        QStringLiteral("font-size:46px;font-weight:700;color:%1;")
        .arg(KswordTheme::TextPrimaryHex()));
    diskSubLayout->addWidget(titleLabel, 0);

    m_diskUtilSummaryLabel = new QLabel(QStringLiteral("磁盘采样初始化中..."), m_utilizationDiskSubPage);
    m_diskUtilSummaryLabel->setStyleSheet(
        QStringLiteral("color:%1;font-size:14px;font-weight:600;").arg(buildStatusColor().name()));
    diskSubLayout->addWidget(m_diskUtilSummaryLabel, 0);

    m_diskReadLineSeries = new QLineSeries(m_utilizationDiskSubPage);
    m_diskReadLineSeries->setName(QStringLiteral("读取"));
    m_diskReadLineSeries->setColor(QColor(80, 170, 255));
    m_diskWriteLineSeries = new QLineSeries(m_utilizationDiskSubPage);
    m_diskWriteLineSeries->setName(QStringLiteral("写入"));
    m_diskWriteLineSeries->setColor(QColor(255, 190, 105));
    for (int indexValue = 0; indexValue < m_historyLength; ++indexValue)
    {
        m_diskReadLineSeries->append(indexValue, 0.0);
        m_diskWriteLineSeries->append(indexValue, 0.0);
    }

    QChart* diskChart = new QChart();
    diskChart->addSeries(m_diskReadLineSeries);
    diskChart->addSeries(m_diskWriteLineSeries);
    diskChart->legend()->setVisible(true);
    diskChart->legend()->setAlignment(Qt::AlignBottom);
    diskChart->setBackgroundVisible(false);
    diskChart->setBackgroundRoundness(0);
    diskChart->setMargins(QMargins(0, 0, 0, 0));
    diskChart->setTitle(QStringLiteral("磁盘读写速率趋势"));

    m_diskUtilAxisX = new QValueAxis(diskChart);
    m_diskUtilAxisX->setRange(0, m_historyLength);
    m_diskUtilAxisX->setLabelsVisible(false);
    m_diskUtilAxisX->setGridLineVisible(false);

    m_diskUtilAxisY = new QValueAxis(diskChart);
    m_diskUtilAxisY->setRange(0.0, 1.0);
    m_diskUtilAxisY->setLabelsVisible(false);
    m_diskUtilAxisY->setGridLineVisible(false);

    diskChart->addAxis(m_diskUtilAxisX, Qt::AlignBottom);
    diskChart->addAxis(m_diskUtilAxisY, Qt::AlignLeft);
    m_diskReadLineSeries->attachAxis(m_diskUtilAxisX);
    m_diskReadLineSeries->attachAxis(m_diskUtilAxisY);
    m_diskWriteLineSeries->attachAxis(m_diskUtilAxisX);
    m_diskWriteLineSeries->attachAxis(m_diskUtilAxisY);

    m_diskUtilChartView = createNoFrameChartView(diskChart, m_utilizationDiskSubPage);
    diskSubLayout->addWidget(m_diskUtilChartView, 1);

    m_diskUtilDetailLabel = new QLabel(QStringLiteral("磁盘参数采样中..."), m_utilizationDiskSubPage);
    m_diskUtilDetailLabel->setWordWrap(false);
    m_diskUtilDetailLabel->setStyleSheet(
        QStringLiteral("font-size:14px;color:%1;").arg(KswordTheme::TextPrimaryHex()));
    diskSubLayout->addWidget(m_diskUtilDetailLabel, 0);

    m_utilizationDetailStack->addWidget(m_utilizationDiskSubPage);
}

void HardwareDock::initializeUtilizationNetworkSubTab()
{
    m_utilizationNetworkSubPage = new QWidget(m_utilizationDetailStack);
    appendTransparentBackgroundStyle(m_utilizationNetworkSubPage);
    QVBoxLayout* networkSubLayout = new QVBoxLayout(m_utilizationNetworkSubPage);
    networkSubLayout->setContentsMargins(4, 4, 4, 4);
    networkSubLayout->setSpacing(6);

    QLabel* titleLabel = new QLabel(QStringLiteral("以太网"), m_utilizationNetworkSubPage);
    titleLabel->setStyleSheet(
        QStringLiteral("font-size:46px;font-weight:700;color:%1;")
        .arg(KswordTheme::TextPrimaryHex()));
    networkSubLayout->addWidget(titleLabel, 0);

    m_networkUtilSummaryLabel = new QLabel(QStringLiteral("网络采样初始化中..."), m_utilizationNetworkSubPage);
    m_networkUtilSummaryLabel->setStyleSheet(
        QStringLiteral("color:%1;font-size:14px;font-weight:600;").arg(buildStatusColor().name()));
    networkSubLayout->addWidget(m_networkUtilSummaryLabel, 0);

    m_networkRxLineSeries = new QLineSeries(m_utilizationNetworkSubPage);
    m_networkRxLineSeries->setName(QStringLiteral("下行"));
    m_networkRxLineSeries->setColor(QColor(92, 190, 255));
    m_networkTxLineSeries = new QLineSeries(m_utilizationNetworkSubPage);
    m_networkTxLineSeries->setName(QStringLiteral("上行"));
    m_networkTxLineSeries->setColor(QColor(153, 129, 255));
    for (int indexValue = 0; indexValue < m_historyLength; ++indexValue)
    {
        m_networkRxLineSeries->append(indexValue, 0.0);
        m_networkTxLineSeries->append(indexValue, 0.0);
    }

    QChart* networkChart = new QChart();
    networkChart->addSeries(m_networkRxLineSeries);
    networkChart->addSeries(m_networkTxLineSeries);
    networkChart->legend()->setVisible(true);
    networkChart->legend()->setAlignment(Qt::AlignBottom);
    networkChart->setBackgroundVisible(false);
    networkChart->setBackgroundRoundness(0);
    networkChart->setMargins(QMargins(0, 0, 0, 0));
    networkChart->setTitle(QStringLiteral("网络收发速率趋势"));

    m_networkUtilAxisX = new QValueAxis(networkChart);
    m_networkUtilAxisX->setRange(0, m_historyLength);
    m_networkUtilAxisX->setLabelsVisible(false);
    m_networkUtilAxisX->setGridLineVisible(false);

    m_networkUtilAxisY = new QValueAxis(networkChart);
    m_networkUtilAxisY->setRange(0.0, 1.0);
    m_networkUtilAxisY->setLabelsVisible(false);
    m_networkUtilAxisY->setGridLineVisible(false);

    networkChart->addAxis(m_networkUtilAxisX, Qt::AlignBottom);
    networkChart->addAxis(m_networkUtilAxisY, Qt::AlignLeft);
    m_networkRxLineSeries->attachAxis(m_networkUtilAxisX);
    m_networkRxLineSeries->attachAxis(m_networkUtilAxisY);
    m_networkTxLineSeries->attachAxis(m_networkUtilAxisX);
    m_networkTxLineSeries->attachAxis(m_networkUtilAxisY);

    m_networkUtilChartView = createNoFrameChartView(networkChart, m_utilizationNetworkSubPage);
    networkSubLayout->addWidget(m_networkUtilChartView, 1);

    m_networkUtilDetailLabel = new QLabel(QStringLiteral("网络参数采样中..."), m_utilizationNetworkSubPage);
    m_networkUtilDetailLabel->setWordWrap(false);
    m_networkUtilDetailLabel->setStyleSheet(
        QStringLiteral("font-size:14px;color:%1;").arg(KswordTheme::TextPrimaryHex()));
    networkSubLayout->addWidget(m_networkUtilDetailLabel, 0);

    m_utilizationDetailStack->addWidget(m_utilizationNetworkSubPage);
}

void HardwareDock::initializeUtilizationGpuSubTab()
{
    m_utilizationGpuSubPage = new QWidget(m_utilizationDetailStack);
    appendTransparentBackgroundStyle(m_utilizationGpuSubPage);
    QVBoxLayout* gpuSubLayout = new QVBoxLayout(m_utilizationGpuSubPage);
    gpuSubLayout->setContentsMargins(4, 4, 4, 4);
    gpuSubLayout->setSpacing(6);

    QHBoxLayout* headerLayout = new QHBoxLayout();
    QLabel* titleLabel = new QLabel(QStringLiteral("GPU"), m_utilizationGpuSubPage);
    titleLabel->setStyleSheet(
        QStringLiteral("font-size:46px;font-weight:700;color:%1;")
        .arg(KswordTheme::TextPrimaryHex()));
    m_gpuAdapterTitleLabel = new QLabel(QStringLiteral("适配器读取中..."), m_utilizationGpuSubPage);
    m_gpuAdapterTitleLabel->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
    m_gpuAdapterTitleLabel->setStyleSheet(
        QStringLiteral("font-size:18px;font-weight:500;color:%1;")
        .arg(KswordTheme::TextPrimaryHex()));
    headerLayout->addWidget(titleLabel, 0);
    headerLayout->addStretch(1);
    headerLayout->addWidget(m_gpuAdapterTitleLabel, 0);
    gpuSubLayout->addLayout(headerLayout, 0);

    m_gpuUtilSummaryLabel = new QLabel(QStringLiteral("GPU采样初始化中..."), m_utilizationGpuSubPage);
    m_gpuUtilSummaryLabel->setStyleSheet(
        QStringLiteral("color:%1;font-size:14px;font-weight:600;").arg(buildStatusColor().name()));
    gpuSubLayout->addWidget(m_gpuUtilSummaryLabel, 0);

    // GPU 引擎四宫格：
    // - 对齐任务管理器的 3D / Copy / Video Encode / Video Decode；
    // - 每个引擎独立曲线和标题，便于定位瓶颈引擎。
    m_gpuEngineHostWidget = new QWidget(m_utilizationGpuSubPage);
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
            appendTransparentBackgroundStyle(cellWidget);
            QVBoxLayout* cellLayout = new QVBoxLayout(cellWidget);
            cellLayout->setContentsMargins(0, 0, 0, 0);
            cellLayout->setSpacing(2);

            QLabel* cellTitle = new QLabel(displayNameText, cellWidget);
            cellTitle->setStyleSheet(
                QStringLiteral("font-size:14px;font-weight:600;color:%1;")
                .arg(KswordTheme::TextPrimaryHex()));
            cellLayout->addWidget(cellTitle, 0);

            QLineSeries* lineSeries = new QLineSeries(cellWidget);
            lineSeries->setColor(lineColor);
            for (int historyIndex = 0; historyIndex < m_historyLength; ++historyIndex)
            {
                lineSeries->append(historyIndex, 0.0);
            }

            QChart* chartPointer = new QChart();
            chartPointer->addSeries(lineSeries);
            chartPointer->legend()->hide();
            chartPointer->setBackgroundVisible(false);
            chartPointer->setBackgroundRoundness(0);
            chartPointer->setMargins(QMargins(0, 0, 0, 0));

            QValueAxis* axisX = new QValueAxis(chartPointer);
            axisX->setRange(0, m_historyLength);
            axisX->setLabelsVisible(false);
            axisX->setGridLineVisible(true);
            axisX->setGridLineColor(QColor(lineColor.red(), lineColor.green(), lineColor.blue(), 40));

            QValueAxis* axisY = new QValueAxis(chartPointer);
            axisY->setRange(0.0, 100.0);
            axisY->setLabelsVisible(false);
            axisY->setGridLineVisible(true);
            axisY->setGridLineColor(QColor(lineColor.red(), lineColor.green(), lineColor.blue(), 40));

            chartPointer->addAxis(axisX, Qt::AlignBottom);
            chartPointer->addAxis(axisY, Qt::AlignLeft);
            lineSeries->attachAxis(axisX);
            lineSeries->attachAxis(axisY);

            QChartView* chartView = createNoFrameChartView(chartPointer, cellWidget);
            cellLayout->addWidget(chartView, 1);
            m_gpuEngineGridLayout->addWidget(cellWidget, rowIndex, columnIndex);

            GpuEngineChartEntry chartEntry;
            chartEntry.engineKeyText = engineKeyText;
            chartEntry.displayNameText = displayNameText;
            chartEntry.titleLabel = cellTitle;
            chartEntry.chartView = chartView;
            chartEntry.lineSeries = lineSeries;
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
    m_gpuDedicatedMemoryLineSeries->setColor(QColor(92, 167, 255));
    m_gpuSharedMemoryLineSeries = new QLineSeries(m_utilizationGpuSubPage);
    m_gpuSharedMemoryLineSeries->setColor(QColor(113, 185, 255));
    for (int indexValue = 0; indexValue < m_historyLength; ++indexValue)
    {
        m_gpuDedicatedMemoryLineSeries->append(indexValue, 0.0);
        m_gpuSharedMemoryLineSeries->append(indexValue, 0.0);
    }

    auto createGpuMemoryChart =
        [this](
            const QString& titleText,
            QLineSeries* lineSeries,
            QValueAxis** axisXOut,
            QValueAxis** axisYOut,
            QChartView** chartViewOut)
        {
            QChart* chartPointer = new QChart();
            chartPointer->addSeries(lineSeries);
            chartPointer->legend()->hide();
            chartPointer->setBackgroundVisible(false);
            chartPointer->setBackgroundRoundness(0);
            chartPointer->setMargins(QMargins(0, 0, 0, 0));
            chartPointer->setTitle(titleText);

            QValueAxis* axisX = new QValueAxis(chartPointer);
            axisX->setRange(0, m_historyLength);
            axisX->setLabelsVisible(false);
            axisX->setGridLineVisible(true);
            axisX->setGridLineColor(QColor(92, 167, 255, 35));

            QValueAxis* axisY = new QValueAxis(chartPointer);
            axisY->setRange(0.0, 1.0);
            axisY->setLabelsVisible(false);
            axisY->setGridLineVisible(true);
            axisY->setGridLineColor(QColor(92, 167, 255, 35));

            chartPointer->addAxis(axisX, Qt::AlignBottom);
            chartPointer->addAxis(axisY, Qt::AlignLeft);
            lineSeries->attachAxis(axisX);
            lineSeries->attachAxis(axisY);

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
                *chartViewOut = createNoFrameChartView(chartPointer, m_utilizationGpuSubPage);
            }
        };

    createGpuMemoryChart(
        QStringLiteral("专用 GPU 内存利用率"),
        m_gpuDedicatedMemoryLineSeries,
        &m_gpuDedicatedMemoryAxisX,
        &m_gpuDedicatedMemoryAxisY,
        &m_gpuDedicatedMemoryChartView);
    createGpuMemoryChart(
        QStringLiteral("共享 GPU 内存利用率"),
        m_gpuSharedMemoryLineSeries,
        &m_gpuSharedMemoryAxisX,
        &m_gpuSharedMemoryAxisY,
        &m_gpuSharedMemoryChartView);

    gpuSubLayout->addWidget(m_gpuDedicatedMemoryChartView, 0);
    gpuSubLayout->addWidget(m_gpuSharedMemoryChartView, 0);

    m_gpuUtilDetailLabel = new QLabel(QStringLiteral("GPU参数采样中..."), m_utilizationGpuSubPage);
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
        appendTransparentBackgroundStyle(chartEntry.containerWidget);
        QVBoxLayout* containerLayout = new QVBoxLayout(chartEntry.containerWidget);
        containerLayout->setContentsMargins(4, 4, 4, 4);
        containerLayout->setSpacing(2);

        chartEntry.titleLabel = new QLabel(
            QStringLiteral("CPU %1").arg(coreIndex),
            chartEntry.containerWidget);
        chartEntry.titleLabel->setStyleSheet(
            QStringLiteral("color:%1;font-weight:600;").arg(buildStatusColor().name()));
        chartEntry.titleLabel->setAlignment(Qt::AlignLeft | Qt::AlignVCenter);
        chartEntry.titleLabel->setMinimumWidth(128);
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

    adjustUtilizationChartHeights();
}

void HardwareDock::initializeConnections()
{
    // 暂无额外交互按钮，预留函数用于后续扩展。
}

void HardwareDock::refreshCpuTopologyStaticInfo()
{
    m_cpuModelText = queryCpuBrandTextByCpuid();

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

    double gpuUsagePercent = 0.0;
    if (!sampleGpuUsage(&gpuUsagePercent))
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

bool HardwareDock::sampleDiskRate(double* readBytesPerSecOut, double* writeBytesPerSecOut)
{
    if (readBytesPerSecOut == nullptr || writeBytesPerSecOut == nullptr)
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
            L"\\PhysicalDisk(_Total)\\Disk Read Bytes/sec",
            0,
            &readCounterHandle);
        const PDH_STATUS addWriteStatus = ::PdhAddEnglishCounterW(
            queryHandle,
            L"\\PhysicalDisk(_Total)\\Disk Write Bytes/sec",
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
        return false;
    }

    const PDH_HQUERY queryHandle = reinterpret_cast<PDH_HQUERY>(m_diskPerfQueryHandle);
    if (::PdhCollectQueryData(queryHandle) != ERROR_SUCCESS)
    {
        return false;
    }

    PDH_FMT_COUNTERVALUE readValue{};
    PDH_FMT_COUNTERVALUE writeValue{};
    const PDH_STATUS readStatus = ::PdhGetFormattedCounterValue(
        reinterpret_cast<PDH_HCOUNTER>(m_diskReadCounterHandle),
        PDH_FMT_DOUBLE,
        nullptr,
        &readValue);
    const PDH_STATUS writeStatus = ::PdhGetFormattedCounterValue(
        reinterpret_cast<PDH_HCOUNTER>(m_diskWriteCounterHandle),
        PDH_FMT_DOUBLE,
        nullptr,
        &writeValue);
    if (readStatus != ERROR_SUCCESS || writeStatus != ERROR_SUCCESS)
    {
        return false;
    }

    *readBytesPerSecOut = std::max(0.0, readValue.doubleValue);
    *writeBytesPerSecOut = std::max(0.0, writeValue.doubleValue);
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
    appendGeneralSeriesPoint(
        m_memoryUtilLineSeries,
        m_memoryUtilAxisX,
        m_memoryUtilAxisY,
        memoryUsagePercent,
        0.0);

    // 磁盘子页：更新读写速率摘要与折线趋势。
    if (m_diskUtilSummaryLabel != nullptr)
    {
        m_diskUtilSummaryLabel->setText(
            QStringLiteral("读取：%1    写入：%2")
            .arg(formatRateText(diskReadBytesPerSec))
            .arg(formatRateText(diskWriteBytesPerSec)));
    }
    appendGeneralSeriesPoint(
        m_diskReadLineSeries,
        m_diskUtilAxisX,
        m_diskUtilAxisY,
        diskReadBytesPerSec,
        0.0);
    appendGeneralSeriesPoint(
        m_diskWriteLineSeries,
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
    appendGeneralSeriesPoint(
        m_networkRxLineSeries,
        m_networkUtilAxisX,
        m_networkUtilAxisY,
        networkRxBytesPerSec,
        0.0);
    appendGeneralSeriesPoint(
        m_networkTxLineSeries,
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
        appendGeneralSeriesPoint(
            chartEntry.lineSeries,
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

    appendGeneralSeriesPoint(
        m_gpuDedicatedMemoryLineSeries,
        m_gpuDedicatedMemoryAxisX,
        m_gpuDedicatedMemoryAxisY,
        m_gpuDedicatedUsedGiB,
        0.0);
    appendGeneralSeriesPoint(
        m_gpuSharedMemoryLineSeries,
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
        m_memoryNavCard->setSubtitleText(
            QStringLiteral("%1/%2 GB (%3%)")
            .arg(usedGiB, 0, 'f', 1)
            .arg(totalGiB, 0, 'f', 1)
            .arg(memoryUsagePercent, 0, 'f', 0));
        m_memoryNavCard->appendSample(memoryUsagePercent);
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

    QPointer<HardwareDock> safeThis(this);
    std::thread([safeThis]() {
        const QString temperatureText = queryCpuTemperatureText();
        const QString voltageText = queryCpuVoltageText();
        const QString sensorText = QStringLiteral("%1|%2")
            .arg(temperatureText)
            .arg(voltageText);

        if (safeThis.isNull())
        {
            return;
        }

        const bool invokeOk = QMetaObject::invokeMethod(
            safeThis.data(),
            [safeThis, sensorText]()
            {
                if (safeThis.isNull())
                {
                    return;
                }

                // 采集偶发失败时保留上一份有效值，避免界面温度频繁闪回 N/A。
                const bool newValueValid = (sensorText != QStringLiteral("N/A|N/A"));
                if (newValueValid
                    || safeThis->m_cachedSensorText.isEmpty()
                    || safeThis->m_cachedSensorText == QStringLiteral("N/A|N/A"))
                {
                    safeThis->m_cachedSensorText = sensorText;
                }
                safeThis->m_sensorRefreshing.store(false);
            },
            Qt::QueuedConnection);

        if (!invokeOk && !safeThis.isNull())
        {
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
