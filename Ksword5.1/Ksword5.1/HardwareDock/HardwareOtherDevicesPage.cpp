#include "HardwareOtherDevicesPage.h"

// ============================================================
// HardwareOtherDevicesPage.cpp
// 作用：
// 1) 在 HardwareDock 内侧边 Tab 展示 CPU/内存/GPU 以外的硬件设备详细清单；
// 2) 使用 PowerShell/CIM 在后台线程采集，避免 UI 卡顿；
// 3) 面向排障场景保留原始 PNP ID、驱动版本、接口、状态等字段。
// ============================================================

#include "../theme.h"
#include "../UI/CodeEditorWidget.h"

#include <QDateTime>
#include <QHBoxLayout>
#include <QLabel>
#include <QMetaObject>
#include <QPointer>
#include <QProcess>
#include <QPushButton>
#include <QVBoxLayout>

#include <thread>

namespace
{
    // runInventoryPowerShellTextSync 作用：
    // - 同步执行硬件清单采集脚本；
    // - scriptText 为 PowerShell 命令文本；
    // - timeoutMs 为等待超时时间；
    // - 返回 stdout 文本，失败时返回可展示诊断。
    QString runInventoryPowerShellTextSync(const QString& scriptText, const int timeoutMs)
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

        // waitStartedOk 用途：确认 PowerShell 进程已启动。
        const bool waitStartedOk = process.waitForStarted(1500);
        if (!waitStartedOk)
        {
            return QStringLiteral("PowerShell启动失败，无法采集其他硬件设备。");
        }

        // waitFinishedOk 用途：避免异常 WMI Provider 长时间阻塞界面刷新。
        const bool waitFinishedOk = process.waitForFinished(timeoutMs);
        if (!waitFinishedOk)
        {
            process.kill();
            process.waitForFinished(800);
            return QStringLiteral("PowerShell执行超时（%1 ms），其他硬件设备清单未完整生成。")
                .arg(timeoutMs);
        }

        const QString standardOutputText = QString::fromLocal8Bit(process.readAllStandardOutput()).trimmed();
        const QString standardErrorText = QString::fromLocal8Bit(process.readAllStandardError()).trimmed();
        if (process.exitStatus() != QProcess::NormalExit || process.exitCode() != 0)
        {
            return QStringLiteral("PowerShell执行失败。\nExitCode=%1\nError=%2")
                .arg(process.exitCode())
                .arg(standardErrorText.isEmpty() ? QStringLiteral("<空>") : standardErrorText);
        }
        return standardOutputText.isEmpty() ? QStringLiteral("<无输出>") : standardOutputText;
    }
}

HardwareOtherDevicesPage::HardwareOtherDevicesPage(QWidget* parent)
    : QWidget(parent)
{
    // 构造流程：先搭建 UI，再建立连接，最后异步拉取首轮设备清单。
    initializeUi();
    initializeConnections();
    refreshDeviceInventoryAsync(false);
}

void HardwareOtherDevicesPage::initializeUi()
{
    // 根布局保持轻量，外层 HardwareDock 负责尺寸分配。
    m_rootLayout = new QVBoxLayout(this);
    m_rootLayout->setContentsMargins(6, 6, 6, 6);
    m_rootLayout->setSpacing(6);

    QHBoxLayout* headerLayout = new QHBoxLayout();
    headerLayout->setContentsMargins(0, 0, 0, 0);
    headerLayout->setSpacing(8);

    QLabel* titleLabel = new QLabel(QStringLiteral("其他硬件设备"), this);
    titleLabel->setStyleSheet(
        QStringLiteral("font-size:18px;font-weight:700;color:%1;")
        .arg(KswordTheme::TextPrimaryHex()));
    headerLayout->addWidget(titleLabel, 0);

    m_statusLabel = new QLabel(QStringLiteral("正在采集设备清单..."), this);
    m_statusLabel->setStyleSheet(
        QStringLiteral("font-size:13px;color:%1;").arg(KswordTheme::TextSecondaryHex()));
    headerLayout->addWidget(m_statusLabel, 1);

    m_refreshButton = new QPushButton(QStringLiteral("刷新"), this);
    m_refreshButton->setToolTip(QStringLiteral("重新枚举主板、存储、外设、PNP、驱动等硬件信息"));
    headerLayout->addWidget(m_refreshButton, 0);
    m_rootLayout->addLayout(headerLayout, 0);

    m_inventoryEditor = new CodeEditorWidget(this);
    m_inventoryEditor->setReadOnly(true);
    m_inventoryEditor->setText(QStringLiteral("设备清单加载中，请稍候..."));
    m_rootLayout->addWidget(m_inventoryEditor, 1);
}

void HardwareOtherDevicesPage::initializeConnections()
{
    if (m_refreshButton == nullptr)
    {
        return;
    }

    // 刷新按钮只触发异步任务，避免用户点击后阻塞主线程。
    connect(
        m_refreshButton,
        &QPushButton::clicked,
        this,
        [this]()
        {
            refreshDeviceInventoryAsync(true);
        });
}

void HardwareOtherDevicesPage::refreshDeviceInventoryAsync(const bool forceRefresh)
{
    bool expectedFlag = false;
    if (!m_refreshing.compare_exchange_strong(expectedFlag, true))
    {
        if (forceRefresh && m_statusLabel != nullptr)
        {
            m_statusLabel->setText(QStringLiteral("正在刷新，请等待当前采集完成。"));
        }
        return;
    }

    if (m_statusLabel != nullptr)
    {
        m_statusLabel->setText(forceRefresh
            ? QStringLiteral("正在重新采集设备清单...")
            : QStringLiteral("正在采集设备清单..."));
    }
    if (m_refreshButton != nullptr)
    {
        m_refreshButton->setEnabled(false);
    }

    QPointer<HardwareOtherDevicesPage> safeThis(this);
    std::thread([safeThis]()
    {
        const QString inventoryText = HardwareOtherDevicesPage::buildDeviceInventoryTextSnapshot();
        if (safeThis.isNull())
        {
            return;
        }

        const bool invokeOk = QMetaObject::invokeMethod(
            safeThis.data(),
            [safeThis, inventoryText]()
            {
                if (safeThis.isNull())
                {
                    return;
                }

                if (safeThis->m_inventoryEditor != nullptr)
                {
                    safeThis->m_inventoryEditor->setText(inventoryText);
                }
                if (safeThis->m_statusLabel != nullptr)
                {
                    safeThis->m_statusLabel->setText(
                        QStringLiteral("最近刷新：%1")
                        .arg(QDateTime::currentDateTime().toString(QStringLiteral("yyyy-MM-dd HH:mm:ss"))));
                }
                if (safeThis->m_refreshButton != nullptr)
                {
                    safeThis->m_refreshButton->setEnabled(true);
                }
                safeThis->m_refreshing.store(false);
            },
            Qt::QueuedConnection);

        if (!invokeOk && !safeThis.isNull())
        {
            safeThis->m_refreshing.store(false);
        }
    }).detach();
}

QString HardwareOtherDevicesPage::buildDeviceInventoryTextSnapshot()
{
    const QString scriptText = QStringLiteral(
        "$ErrorActionPreference='SilentlyContinue'; "
        "function Format-Size([object]$bytes){ "
        "  if($null -eq $bytes){ return $null }; "
        "  $value=[double]$bytes; "
        "  if($value -ge 1TB){ return ('{0:N2} TB' -f ($value/1TB)) }; "
        "  if($value -ge 1GB){ return ('{0:N2} GB' -f ($value/1GB)) }; "
        "  if($value -ge 1MB){ return ('{0:N2} MB' -f ($value/1MB)) }; "
        "  return ([string]$bytes + ' B'); "
        "}; "
        "function Format-Speed([object]$bits){ "
        "  if($null -eq $bits){ return $null }; "
        "  $value=[double]$bits; "
        "  if($value -ge 1000000000){ return ('{0:N2} Gbps' -f ($value/1000000000)) }; "
        "  if($value -ge 1000000){ return ('{0:N2} Mbps' -f ($value/1000000)) }; "
        "  if($value -ge 1000){ return ('{0:N2} Kbps' -f ($value/1000)) }; "
        "  return ([string]$bits + ' bps'); "
        "}; "
        "function Write-Section([string]$title,[object]$rows){ "
        "  $text=\"`r`n========== $title ==========`r`n\"; "
        "  if($null -eq $rows){ return $text + '<未检测到>' + \"`r`n\" }; "
        "  $array=@($rows); "
        "  if($array.Count -le 0){ return $text + '<未检测到>' + \"`r`n\" }; "
        "  return $text + (($array | Format-List * | Out-String -Width 4096).Trim()) + \"`r`n\"; "
        "}; "
        "$text=''; "
        "$text += '采集时间: ' + (Get-Date -Format 'yyyy-MM-dd HH:mm:ss') + \"`r`n\"; "
        "$cs=Get-CimInstance Win32_ComputerSystem | Select-Object Manufacturer,Model,SystemType,PCSystemType,Domain,Workgroup,UserName,TotalPhysicalMemory,NumberOfProcessors,NumberOfLogicalProcessors; "
        "$board=Get-CimInstance Win32_BaseBoard | Select-Object Manufacturer,Product,Version,SerialNumber,Tag; "
        "$bios=Get-CimInstance Win32_BIOS | Select-Object Manufacturer,Name,SMBIOSBIOSVersion,Version,ReleaseDate,SerialNumber,SMBIOSMajorVersion,SMBIOSMinorVersion; "
        "$enclosure=Get-CimInstance Win32_SystemEnclosure | Select-Object Manufacturer,Model,SerialNumber,SMBIOSAssetTag,ChassisTypes,LockPresent,SecurityStatus; "
        "$processor=Get-CimInstance Win32_Processor | Select-Object SocketDesignation,Name,Manufacturer,Caption,Description,ProcessorId,NumberOfCores,NumberOfEnabledCore,NumberOfLogicalProcessors,MaxClockSpeed,L2CacheSize,L3CacheSize,VirtualizationFirmwareEnabled,SecondLevelAddressTranslationExtensions,VMMonitorModeExtensions; "
        "$memoryArray=Get-CimInstance Win32_PhysicalMemoryArray | ForEach-Object { [pscustomobject]@{Location=$_.Location;Use=$_.Use;MemoryDevices=$_.MemoryDevices;MaxCapacity=Format-Size ([uint64]$_.MaxCapacity * 1KB);MaxCapacityEx=Format-Size $_.MaxCapacityEx} }; "
        "$memoryDevices=Get-CimInstance Win32_PhysicalMemory | ForEach-Object { [pscustomobject]@{BankLabel=$_.BankLabel;DeviceLocator=$_.DeviceLocator;Manufacturer=$_.Manufacturer;PartNumber=$_.PartNumber;SerialNumber=$_.SerialNumber;ConfiguredClockSpeed=$_.ConfiguredClockSpeed;Speed=$_.Speed;Capacity=Format-Size $_.Capacity;FormFactor=$_.FormFactor;MemoryType=$_.MemoryType;SMBIOSMemoryType=$_.SMBIOSMemoryType;DataWidth=$_.DataWidth;TotalWidth=$_.TotalWidth} }; "
        "$disks=Get-CimInstance Win32_DiskDrive | ForEach-Object { [pscustomobject]@{Index=$_.Index;Model=$_.Model;FirmwareRevision=$_.FirmwareRevision;SerialNumber=$_.SerialNumber;InterfaceType=$_.InterfaceType;MediaType=$_.MediaType;Size=Format-Size $_.Size;Partitions=$_.Partitions;BytesPerSector=$_.BytesPerSector;PNPDeviceID=$_.PNPDeviceID;Status=$_.Status} }; "
        "$volumes=Get-CimInstance Win32_LogicalDisk | Select-Object DeviceID,VolumeName,DriveType,FileSystem,@{Name='Size';Expression={Format-Size $_.Size}},@{Name='FreeSpace';Expression={Format-Size $_.FreeSpace}},ProviderName,VolumeSerialNumber; "
        "$controllers=@(Get-CimInstance Win32_IDEController; Get-CimInstance Win32_SCSIController; Get-CimInstance Win32_DiskController) | Select-Object Name,Manufacturer,DeviceID,PNPDeviceID,Status; "
        "$gpu=Get-CimInstance Win32_VideoController | ForEach-Object { [pscustomobject]@{Name=$_.Name;VideoProcessor=$_.VideoProcessor;AdapterRAM=Format-Size $_.AdapterRAM;DriverVersion=$_.DriverVersion;DriverDate=$_.DriverDate;CurrentResolution=([string]$_.CurrentHorizontalResolution + 'x' + [string]$_.CurrentVerticalResolution + '@' + [string]$_.CurrentRefreshRate);PNPDeviceID=$_.PNPDeviceID;Status=$_.Status} }; "
        "$monitors=Get-CimInstance Win32_DesktopMonitor | Select-Object Name,MonitorType,ScreenWidth,ScreenHeight,PixelsPerXLogicalInch,PixelsPerYLogicalInch,PNPDeviceID,Status; "
        "$network=Get-CimInstance Win32_NetworkAdapter | Where-Object { $_.PhysicalAdapter -eq $true -or $_.PNPDeviceID -like 'PCI*' -or $_.PNPDeviceID -like 'USB*' } | ForEach-Object { [pscustomobject]@{Name=$_.Name;Manufacturer=$_.Manufacturer;AdapterType=$_.AdapterType;MACAddress=$_.MACAddress;Speed=Format-Speed $_.Speed;NetConnectionID=$_.NetConnectionID;NetConnectionStatus=$_.NetConnectionStatus;ServiceName=$_.ServiceName;PNPDeviceID=$_.PNPDeviceID;Status=$_.Status} }; "
        "$netConfig=Get-CimInstance Win32_NetworkAdapterConfiguration | Where-Object { $_.MACAddress -or $_.IPAddress } | Select-Object Description,MACAddress,DHCPEnabled,IPAddress,IPSubnet,DefaultIPGateway,DNSServerSearchOrder,DNSDomain; "
        "$audio=Get-CimInstance Win32_SoundDevice | Select-Object Name,Manufacturer,ProductName,DeviceID,PNPDeviceID,Status; "
        "$cameras=Get-CimInstance Win32_PnPEntity | Where-Object { $_.PNPClass -in @('Camera','Image') -or $_.Service -like '*usbvideo*' } | Select-Object Name,Manufacturer,Service,PNPClass,DeviceID,PNPDeviceID,Status; "
        "$usbControllers=Get-CimInstance Win32_USBController | Select-Object Name,Manufacturer,DeviceID,PNPDeviceID,Status; "
        "$usbDevices=Get-CimInstance Win32_PnPEntity | Where-Object { $_.PNPDeviceID -like 'USB*' } | Select-Object Name,Manufacturer,Service,PNPClass,DeviceID,PNPDeviceID,Status -First 120; "
        "$hid=Get-CimInstance Win32_PnPEntity | Where-Object { $_.PNPClass -in @('Keyboard','Mouse','HIDClass','Bluetooth','MEDIA','USB') } | Select-Object Name,Manufacturer,Service,PNPClass,DeviceID,PNPDeviceID,Status -First 160; "
        "$battery=Get-CimInstance Win32_Battery | Select-Object Name,DeviceID,BatteryStatus,EstimatedChargeRemaining,EstimatedRunTime,Chemistry,DesignCapacity,FullChargeCapacity,Status; "
        "$tpm=Get-CimInstance -Namespace root/cimv2/security/microsofttpm -ClassName Win32_Tpm | Select-Object IsEnabled_InitialValue,IsActivated_InitialValue,IsOwned_InitialValue,ManufacturerId,ManufacturerVersion,SpecVersion; "
        "$ports=Get-CimInstance Win32_SerialPort | Select-Object DeviceID,Name,Caption,Description,ProviderType,PNPDeviceID,Status; "
        "$printers=Get-CimInstance Win32_Printer | Select-Object Name,DriverName,PortName,Default,Shared,Network,WorkOffline,Status; "
        "$pci=Get-CimInstance Win32_PnPEntity | Where-Object { $_.PNPDeviceID -like 'PCI*' } | Select-Object Name,Manufacturer,Service,PNPClass,DeviceID,PNPDeviceID,Status -First 180; "
        "$problem=Get-CimInstance Win32_PnPEntity | Where-Object { $_.ConfigManagerErrorCode -ne 0 } | Select-Object Name,PNPClass,ConfigManagerErrorCode,Manufacturer,DeviceID,PNPDeviceID,Status; "
        "$drivers=Get-CimInstance Win32_PnPSignedDriver | Select-Object DeviceName,DeviceClass,Manufacturer,DriverProviderName,DriverVersion,DriverDate,InfName,DeviceID -First 220; "
        "$text += Write-Section '整机/机箱' $cs; "
        "$text += Write-Section '主板' $board; "
        "$text += Write-Section 'BIOS/UEFI' $bios; "
        "$text += Write-Section '机箱/资产标识' $enclosure; "
        "$text += Write-Section '处理器扩展信息' $processor; "
        "$text += Write-Section '内存阵列' $memoryArray; "
        "$text += Write-Section '内存条详细信息' $memoryDevices; "
        "$text += Write-Section '磁盘驱动器' $disks; "
        "$text += Write-Section '逻辑卷' $volumes; "
        "$text += Write-Section '存储控制器' $controllers; "
        "$text += Write-Section '显示适配器' $gpu; "
        "$text += Write-Section '显示器' $monitors; "
        "$text += Write-Section '物理/USB网卡' $network; "
        "$text += Write-Section '网络配置' $netConfig; "
        "$text += Write-Section '音频设备' $audio; "
        "$text += Write-Section '摄像头/图像设备' $cameras; "
        "$text += Write-Section 'USB控制器' $usbControllers; "
        "$text += Write-Section 'USB设备(前120项)' $usbDevices; "
        "$text += Write-Section '键鼠/HID/蓝牙/媒体设备(前160项)' $hid; "
        "$text += Write-Section '电池/UPS' $battery; "
        "$text += Write-Section 'TPM' $tpm; "
        "$text += Write-Section '串口/通信端口' $ports; "
        "$text += Write-Section '打印机' $printers; "
        "$text += Write-Section 'PCI/板载设备(前180项)' $pci; "
        "$text += Write-Section '异常PNP设备' $problem; "
        "$text += Write-Section 'PNP签名驱动(前220项)' $drivers; "
        "$text");
    return runInventoryPowerShellTextSync(scriptText, 18000);
}
