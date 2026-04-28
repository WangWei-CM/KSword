#include "DirectKernelCallMonitorWidget.h"

// ============================================================
// DirectKernelCallMonitorWidget.cpp
// 作用：
// 1) 用 ETW System Syscall Provider 采集系统调用事件；
// 2) 通过 TDH 解码事件字段，关联 PID / 调用号 / 调用地址；
// 3) 解析 ntdll/win32u 导出桩，辅助把系统调用号转换为服务名。
// ============================================================

#include "MonitorTextViewer.h"
#include "../theme.h"

#include <QAbstractItemView>
#include <QAction>
#include <QApplication>
#include <QByteArray>
#include <QCheckBox>
#include <QClipboard>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QGridLayout>
#include <QHeaderView>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QMessageBox>
#include <QMetaObject>
#include <QPoint>
#include <QPointer>
#include <QPushButton>
#include <QRegularExpression>
#include <QSignalBlocker>
#include <QSpinBox>
#include <QTableWidget>
#include <QTableWidgetItem>
#include <QTextStream>
#include <QTimer>
#include <QVBoxLayout>

#include <algorithm>
#include <cstring>
#include <optional>
#include <utility>

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#include <TlHelp32.h>
#include <evntrace.h>
#include <evntcons.h>
#include <tdh.h>

#pragma comment(lib, "Advapi32.lib")
#pragma comment(lib, "Tdh.lib")

namespace
{
    constexpr int kRoleGlobalSearchText = Qt::UserRole;
    constexpr int kRoleProcessSearchText = Qt::UserRole + 1;
    constexpr int kRoleServiceSearchText = Qt::UserRole + 2;
    constexpr int kRoleDetailText = Qt::UserRole + 3;

    constexpr GUID kSystemTraceControlGuid =
        { 0x9e814aad, 0x3204, 0x11d2, { 0x9a, 0x82, 0x00, 0x60, 0x08, 0xa8, 0x69, 0x39 } };
    constexpr GUID kSystemSyscallProviderGuid =
        { 0x434286f7, 0x6f1b, 0x45bb, { 0xb3, 0x7e, 0x95, 0xf6, 0x23, 0x04, 0x6c, 0x7c } };

    QString blueButtonStyle()
    {
        return QStringLiteral(
            "QPushButton{color:%1;background:%5;border:1px solid %2;border-radius:3px;padding:4px 8px;}"
            "QPushButton:hover{background:%3;color:#FFFFFF;border:1px solid %3;}"
            "QPushButton:pressed{background:%4;color:#FFFFFF;}"
            "QPushButton:disabled{color:%6;border:1px solid %2;background:%5;}")
            .arg(KswordTheme::PrimaryBlueHex)
            .arg(KswordTheme::PrimaryBlueBorderHex)
            .arg(QStringLiteral("#2E8BFF"))
            .arg(KswordTheme::PrimaryBluePressedHex)
            .arg(KswordTheme::SurfaceHex())
            .arg(KswordTheme::TextSecondaryHex());
    }

    QString blueInputStyle()
    {
        return QStringLiteral(
            "QLineEdit,QComboBox,QSpinBox{border:1px solid %2;border-radius:3px;background:%3;color:%4;padding:2px 6px;}"
            "QLineEdit:focus,QComboBox:focus,QSpinBox:focus{border:1px solid %1;}")
            .arg(KswordTheme::PrimaryBlueHex)
            .arg(KswordTheme::BorderHex())
            .arg(KswordTheme::SurfaceHex())
            .arg(KswordTheme::TextPrimaryHex());
    }

    QString blueHeaderStyle()
    {
        return QStringLiteral(
            "QHeaderView::section{color:%1;background:%2;border:1px solid %3;padding:4px;font-weight:600;}")
            .arg(KswordTheme::PrimaryBlueHex)
            .arg(KswordTheme::SurfaceHex())
            .arg(KswordTheme::BorderHex());
    }

    QString buildStatusStyle(const QString& colorHex)
    {
        return QStringLiteral("color:%1;font-weight:600;").arg(colorHex);
    }

    QString monitorInfoColorHex()
    {
        return KswordTheme::PrimaryBlueHex;
    }

    QString monitorSuccessColorHex()
    {
        return QStringLiteral("#16A34A");
    }

    QString monitorWarningColorHex()
    {
        return QStringLiteral("#D97706");
    }

    QString monitorErrorColorHex()
    {
        return QStringLiteral("#DC2626");
    }

    QString monitorIdleColorHex()
    {
        return KswordTheme::TextSecondaryHex();
    }

    QPushButton* createIconButton(QWidget* parentWidget, const QString& iconPath, const QString& tooltipText)
    {
        QPushButton* buttonPointer = new QPushButton(QIcon(iconPath), QString(), parentWidget);
        buttonPointer->setToolTip(tooltipText);
        buttonPointer->setFixedSize(QSize(30, 28));
        buttonPointer->setStyleSheet(blueButtonStyle());
        return buttonPointer;
    }

    QTableWidgetItem* createReadOnlyItem(const QString& text)
    {
        QTableWidgetItem* item = new QTableWidgetItem(text);
        item->setFlags(item->flags() & ~Qt::ItemIsEditable);
        return item;
    }

    QString guidToText(const GUID& guidValue)
    {
        wchar_t buffer[64] = {};
        if (::StringFromGUID2(guidValue, buffer, static_cast<int>(std::size(buffer))) <= 0)
        {
            return QStringLiteral("{00000000-0000-0000-0000-000000000000}");
        }
        return QString::fromWCharArray(buffer);
    }

    QString formatAddress(const std::uint64_t addressValue)
    {
        if (addressValue == 0)
        {
            return QStringLiteral("<未知>");
        }
        return QStringLiteral("0x%1")
            .arg(static_cast<qulonglong>(addressValue), 16, 16, QChar(u'0'))
            .toUpper();
    }

    QString normalizeName(const QString& text)
    {
        QString normalized = text.toLower();
        normalized.remove(QRegularExpression(QStringLiteral("[^a-z0-9]")));
        return normalized;
    }

    bool textMatch(
        const QString& sourceText,
        const QString& patternText,
        const bool useRegex,
        const Qt::CaseSensitivity caseSensitivity)
    {
        if (patternText.trimmed().isEmpty())
        {
            return true;
        }

        if (!useRegex)
        {
            return sourceText.contains(patternText, caseSensitivity);
        }

        QRegularExpression::PatternOptions options = QRegularExpression::NoPatternOption;
        if (caseSensitivity == Qt::CaseInsensitive)
        {
            options |= QRegularExpression::CaseInsensitiveOption;
        }
        const QRegularExpression regex(patternText, options);
        return regex.isValid() && regex.match(sourceText).hasMatch();
    }

    QString trimmedWideString(const wchar_t* textPointer, const int characterCount)
    {
        if (textPointer == nullptr || characterCount <= 0)
        {
            return QString();
        }

        QString text = QString::fromWCharArray(textPointer, characterCount);
        while (text.endsWith(QChar(u'\0')))
        {
            text.chop(1);
        }
        return text.trimmed();
    }

    QString trimmedAnsiString(const char* textPointer, const int byteCount)
    {
        if (textPointer == nullptr || byteCount <= 0)
        {
            return QString();
        }

        QByteArray bytes(textPointer, byteCount);
        while (!bytes.isEmpty() && bytes.endsWith('\0'))
        {
            bytes.chop(1);
        }
        return QString::fromLocal8Bit(bytes).trimmed();
    }

    QString decodedValueText(
        const unsigned char* dataPointer,
        const ULONG dataSize,
        const USHORT inType,
        std::uint64_t* numericValueOut,
        bool* hasNumericValueOut)
    {
        if (numericValueOut != nullptr)
        {
            *numericValueOut = 0;
        }
        if (hasNumericValueOut != nullptr)
        {
            *hasNumericValueOut = false;
        }
        if (dataPointer == nullptr || dataSize == 0)
        {
            return QString();
        }

        auto setNumeric = [&](const std::uint64_t value) {
            if (numericValueOut != nullptr)
            {
                *numericValueOut = value;
            }
            if (hasNumericValueOut != nullptr)
            {
                *hasNumericValueOut = true;
            }
        };

        switch (inType)
        {
        case TDH_INTYPE_UNICODESTRING:
            return trimmedWideString(
                reinterpret_cast<const wchar_t*>(dataPointer),
                static_cast<int>(dataSize / sizeof(wchar_t)));
        case TDH_INTYPE_ANSISTRING:
            return trimmedAnsiString(reinterpret_cast<const char*>(dataPointer), static_cast<int>(dataSize));
        case TDH_INTYPE_INT8:
        {
            const std::int8_t value = *reinterpret_cast<const std::int8_t*>(dataPointer);
            setNumeric(static_cast<std::uint64_t>(static_cast<std::int64_t>(value)));
            return QString::number(value);
        }
        case TDH_INTYPE_UINT8:
        {
            const std::uint8_t value = *reinterpret_cast<const std::uint8_t*>(dataPointer);
            setNumeric(value);
            return QString::number(value);
        }
        case TDH_INTYPE_INT16:
        {
            std::int16_t value = 0;
            if (dataSize >= sizeof(value))
            {
                std::memcpy(&value, dataPointer, sizeof(value));
            }
            setNumeric(static_cast<std::uint64_t>(static_cast<std::int64_t>(value)));
            return QString::number(value);
        }
        case TDH_INTYPE_UINT16:
        {
            std::uint16_t value = 0;
            if (dataSize >= sizeof(value))
            {
                std::memcpy(&value, dataPointer, sizeof(value));
            }
            setNumeric(value);
            return QString::number(value);
        }
        case TDH_INTYPE_INT32:
        {
            std::int32_t value = 0;
            if (dataSize >= sizeof(value))
            {
                std::memcpy(&value, dataPointer, sizeof(value));
            }
            setNumeric(static_cast<std::uint64_t>(static_cast<std::int64_t>(value)));
            return QString::number(value);
        }
        case TDH_INTYPE_UINT32:
        case TDH_INTYPE_HEXINT32:
        {
            std::uint32_t value = 0;
            if (dataSize >= sizeof(value))
            {
                std::memcpy(&value, dataPointer, sizeof(value));
            }
            setNumeric(value);
            return inType == TDH_INTYPE_HEXINT32
                ? QStringLiteral("0x%1").arg(value, 8, 16, QChar(u'0')).toUpper()
                : QString::number(value);
        }
        case TDH_INTYPE_INT64:
        {
            std::int64_t value = 0;
            if (dataSize >= sizeof(value))
            {
                std::memcpy(&value, dataPointer, sizeof(value));
            }
            setNumeric(static_cast<std::uint64_t>(value));
            return QString::number(static_cast<qlonglong>(value));
        }
        case TDH_INTYPE_UINT64:
        case TDH_INTYPE_HEXINT64:
        case TDH_INTYPE_POINTER:
        {
            std::uint64_t value = 0;
            if (dataSize >= sizeof(value))
            {
                std::memcpy(&value, dataPointer, sizeof(value));
            }
            else if (dataSize >= sizeof(std::uint32_t))
            {
                std::uint32_t value32 = 0;
                std::memcpy(&value32, dataPointer, sizeof(value32));
                value = value32;
            }
            setNumeric(value);
            return (inType == TDH_INTYPE_UINT64)
                ? QString::number(static_cast<qulonglong>(value))
                : formatAddress(value);
        }
        case TDH_INTYPE_BOOLEAN:
        {
            std::uint32_t value = 0;
            if (dataSize >= sizeof(value))
            {
                std::memcpy(&value, dataPointer, sizeof(value));
            }
            else
            {
                value = dataPointer[0];
            }
            setNumeric(value);
            return value != 0 ? QStringLiteral("true") : QStringLiteral("false");
        }
        case TDH_INTYPE_GUID:
            if (dataSize >= sizeof(GUID))
            {
                GUID guidValue{};
                std::memcpy(&guidValue, dataPointer, sizeof(guidValue));
                return guidToText(guidValue);
            }
            break;
        default:
            break;
        }

        QStringList byteList;
        const ULONG visibleBytes = std::min<ULONG>(dataSize, 32);
        for (ULONG indexValue = 0; indexValue < visibleBytes; ++indexValue)
        {
            byteList << QStringLiteral("%1").arg(dataPointer[indexValue], 2, 16, QChar(u'0')).toUpper();
        }
        if (dataSize > visibleBytes)
        {
            byteList << QStringLiteral("...");
        }
        return byteList.join(QStringLiteral(" "));
    }

    QString eventNameFromInfo(const unsigned char* infoBuffer, const TRACE_EVENT_INFO* traceInfo)
    {
        if (infoBuffer == nullptr || traceInfo == nullptr)
        {
            return QString();
        }

        auto textAtOffset = [&](const ULONG offsetValue) -> QString {
            if (offsetValue == 0)
            {
                return QString();
            }
            const wchar_t* textPointer = reinterpret_cast<const wchar_t*>(infoBuffer + offsetValue);
            return QString::fromWCharArray(textPointer).trimmed();
        };

        QString eventName = textAtOffset(traceInfo->EventNameOffset);
        if (!eventName.isEmpty())
        {
            return eventName;
        }
        eventName = textAtOffset(traceInfo->TaskNameOffset);
        if (!eventName.isEmpty())
        {
            return eventName;
        }
        return textAtOffset(traceInfo->OpcodeNameOffset);
    }

    std::optional<std::uint32_t> tryReadSyscallNumberFromStub(const unsigned char* functionPointer)
    {
        if (functionPointer == nullptr)
        {
            return std::nullopt;
        }

        for (std::size_t offsetValue = 0; offsetValue + sizeof(std::uint32_t) < 32; ++offsetValue)
        {
            if (functionPointer[offsetValue] != 0xB8)
            {
                continue;
            }

            std::uint32_t syscallNumber = 0;
            std::memcpy(&syscallNumber, functionPointer + offsetValue + 1, sizeof(syscallNumber));
            if (syscallNumber < 0x10000U)
            {
                return syscallNumber;
            }
        }
        return std::nullopt;
    }

    bool serviceNameAllowed(const QString& exportName, const QStringList& prefixList)
    {
        for (const QString& prefixText : prefixList)
        {
            if (exportName.startsWith(prefixText, Qt::CaseSensitive))
            {
                return true;
            }
        }
        return false;
    }

    void appendSyscallExportsFromModule(
        const wchar_t* moduleName,
        const QStringList& prefixList,
        std::unordered_map<std::uint32_t, DirectKernelCallMonitorWidget::SyscallMapEntry>* mapPointer)
    {
        if (moduleName == nullptr || mapPointer == nullptr)
        {
            return;
        }

        HMODULE moduleHandle = ::GetModuleHandleW(moduleName);
        if (moduleHandle == nullptr)
        {
            moduleHandle = ::LoadLibraryW(moduleName);
        }
        if (moduleHandle == nullptr)
        {
            return;
        }

        const auto* basePointer = reinterpret_cast<const unsigned char*>(moduleHandle);
        const auto* dosHeader = reinterpret_cast<const IMAGE_DOS_HEADER*>(basePointer);
        if (dosHeader->e_magic != IMAGE_DOS_SIGNATURE)
        {
            return;
        }

        const auto* ntHeaders = reinterpret_cast<const IMAGE_NT_HEADERS*>(basePointer + dosHeader->e_lfanew);
        if (ntHeaders->Signature != IMAGE_NT_SIGNATURE)
        {
            return;
        }

        const IMAGE_DATA_DIRECTORY& exportDirectoryInfo =
            ntHeaders->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXPORT];
        if (exportDirectoryInfo.VirtualAddress == 0 || exportDirectoryInfo.Size == 0)
        {
            return;
        }

        const auto* exportDirectory = reinterpret_cast<const IMAGE_EXPORT_DIRECTORY*>(
            basePointer + exportDirectoryInfo.VirtualAddress);
        const auto* nameRvaArray = reinterpret_cast<const DWORD*>(basePointer + exportDirectory->AddressOfNames);
        const auto* ordinalArray = reinterpret_cast<const WORD*>(basePointer + exportDirectory->AddressOfNameOrdinals);
        const auto* functionRvaArray = reinterpret_cast<const DWORD*>(basePointer + exportDirectory->AddressOfFunctions);
        const QString moduleText = QString::fromWCharArray(moduleName);

        for (DWORD indexValue = 0; indexValue < exportDirectory->NumberOfNames; ++indexValue)
        {
            const char* exportNamePointer = reinterpret_cast<const char*>(basePointer + nameRvaArray[indexValue]);
            const QString exportName = QString::fromLatin1(exportNamePointer);
            if (!serviceNameAllowed(exportName, prefixList))
            {
                continue;
            }

            const WORD ordinalValue = ordinalArray[indexValue];
            if (ordinalValue >= exportDirectory->NumberOfFunctions)
            {
                continue;
            }

            const DWORD functionRva = functionRvaArray[ordinalValue];
            if (functionRva >= exportDirectoryInfo.VirtualAddress
                && functionRva < exportDirectoryInfo.VirtualAddress + exportDirectoryInfo.Size)
            {
                continue;
            }

            const unsigned char* functionPointer = basePointer + functionRva;
            const std::optional<std::uint32_t> syscallNumber = tryReadSyscallNumberFromStub(functionPointer);
            if (!syscallNumber.has_value())
            {
                continue;
            }

            DirectKernelCallMonitorWidget::SyscallMapEntry& entry = (*mapPointer)[*syscallNumber];
            entry.syscallNumber = *syscallNumber;
            if (entry.serviceName.isEmpty())
            {
                entry.serviceName = exportName;
                entry.sourceModule = moduleText;
            }
            else if (!entry.serviceName.split(QStringLiteral(" / ")).contains(exportName))
            {
                entry.serviceName += QStringLiteral(" / %1").arg(exportName);
                entry.sourceModule += QStringLiteral(" / %1").arg(moduleText);
            }
        }
    }

    void stopActiveKswordTraceSessionsByPrefix(const QStringList& sessionPrefixList)
    {
        constexpr ULONG kQuerySessionCapacity = 96;
        constexpr ULONG kTraceNameChars = 1024;
        constexpr ULONG kLogFileChars = 1024;
        constexpr ULONG kPropertyBufferSize =
            sizeof(EVENT_TRACE_PROPERTIES)
            + (kTraceNameChars + kLogFileChars) * sizeof(wchar_t);

        std::vector<std::vector<unsigned char>> propertyBufferList(
            kQuerySessionCapacity,
            std::vector<unsigned char>(kPropertyBufferSize, 0));
        std::vector<EVENT_TRACE_PROPERTIES*> propertyPointerList(kQuerySessionCapacity, nullptr);

        for (ULONG indexValue = 0; indexValue < kQuerySessionCapacity; ++indexValue)
        {
            auto* properties = reinterpret_cast<EVENT_TRACE_PROPERTIES*>(propertyBufferList[indexValue].data());
            properties->Wnode.BufferSize = kPropertyBufferSize;
            properties->LoggerNameOffset = sizeof(EVENT_TRACE_PROPERTIES);
            properties->LogFileNameOffset =
                sizeof(EVENT_TRACE_PROPERTIES) + kTraceNameChars * sizeof(wchar_t);
            propertyPointerList[indexValue] = properties;
        }

        ULONG sessionCount = kQuerySessionCapacity;
        const ULONG queryStatus = ::QueryAllTracesW(
            propertyPointerList.data(),
            kQuerySessionCapacity,
            &sessionCount);
        if (queryStatus != ERROR_SUCCESS && queryStatus != ERROR_MORE_DATA)
        {
            return;
        }

        for (ULONG indexValue = 0; indexValue < sessionCount && indexValue < kQuerySessionCapacity; ++indexValue)
        {
            const EVENT_TRACE_PROPERTIES* properties = propertyPointerList[indexValue];
            if (properties == nullptr || properties->LoggerNameOffset == 0)
            {
                continue;
            }

            const wchar_t* loggerNamePointer = reinterpret_cast<const wchar_t*>(
                propertyBufferList[indexValue].data() + properties->LoggerNameOffset);
            const QString loggerNameText = QString::fromWCharArray(loggerNamePointer).trimmed();
            if (loggerNameText.isEmpty())
            {
                continue;
            }

            const bool shouldStop = std::any_of(
                sessionPrefixList.begin(),
                sessionPrefixList.end(),
                [&loggerNameText](const QString& prefixText) {
                    return !prefixText.trimmed().isEmpty()
                        && loggerNameText.startsWith(prefixText, Qt::CaseInsensitive);
                });
            if (!shouldStop)
            {
                continue;
            }

            const std::wstring loggerNameWide = loggerNameText.toStdWString();
            std::vector<unsigned char> stopBuffer(
                sizeof(EVENT_TRACE_PROPERTIES) + (loggerNameWide.size() + 1) * sizeof(wchar_t),
                0);
            auto* stopProperties = reinterpret_cast<EVENT_TRACE_PROPERTIES*>(stopBuffer.data());
            stopProperties->Wnode.BufferSize = static_cast<ULONG>(stopBuffer.size());
            stopProperties->LoggerNameOffset = sizeof(EVENT_TRACE_PROPERTIES);
            wchar_t* stopLoggerNamePointer = reinterpret_cast<wchar_t*>(
                stopBuffer.data() + stopProperties->LoggerNameOffset);
            ::wcscpy_s(stopLoggerNamePointer, loggerNameWide.size() + 1, loggerNameWide.c_str());
            ::ControlTraceW(0, stopLoggerNamePointer, stopProperties, EVENT_TRACE_CONTROL_STOP);
        }
    }
}

DirectKernelCallMonitorWidget::DirectKernelCallMonitorWidget(QWidget* parent)
    : QWidget(parent)
{
    kLogEvent event;
    info << event << "[DirectKernelCallMonitorWidget] 初始化直接内核调用监控页。" << eol;

    initializeUi();
    initializeConnections();
    reloadSyscallMap();
    updateActionState();
    updateStatusLabel();
}

DirectKernelCallMonitorWidget::~DirectKernelCallMonitorWidget()
{
    stopCaptureInternal(true);
    if (m_uiUpdateTimer != nullptr)
    {
        m_uiUpdateTimer->stop();
    }
    if (m_filterDebounceTimer != nullptr)
    {
        m_filterDebounceTimer->stop();
    }

    kLogEvent event;
    info << event << "[DirectKernelCallMonitorWidget] 直接内核调用监控页已析构。" << eol;
}

void DirectKernelCallMonitorWidget::initializeUi()
{
    m_rootLayout = new QVBoxLayout(this);
    m_rootLayout->setContentsMargins(6, 6, 6, 6);
    m_rootLayout->setSpacing(6);

    m_controlPanel = new QWidget(this);
    QGridLayout* controlLayout = new QGridLayout(m_controlPanel);
    controlLayout->setContentsMargins(6, 6, 6, 6);
    controlLayout->setHorizontalSpacing(6);
    controlLayout->setVerticalSpacing(6);

    controlLayout->addWidget(new QLabel(QStringLiteral("目标 PID"), m_controlPanel), 0, 0);
    m_targetPidEdit = new QLineEdit(m_controlPanel);
    m_targetPidEdit->setPlaceholderText(QStringLiteral("多个 PID 用逗号/空格分隔；留空需勾选全局采集"));
    m_targetPidEdit->setStyleSheet(blueInputStyle());
    controlLayout->addWidget(m_targetPidEdit, 0, 1, 1, 3);

    m_globalCaptureCheck = new QCheckBox(QStringLiteral("全局采集"), m_controlPanel);
    m_globalCaptureCheck->setToolTip(QStringLiteral("采集全系统 syscall 事件，事件量可能很大"));
    controlLayout->addWidget(m_globalCaptureCheck, 0, 4);

    m_resolveAddressCheck = new QCheckBox(QStringLiteral("解析调用地址"), m_controlPanel);
    m_resolveAddressCheck->setChecked(true);
    m_resolveAddressCheck->setToolTip(QStringLiteral("尝试把事件中的调用地址映射到进程模块，用于识别疑似直接 syscall"));
    controlLayout->addWidget(m_resolveAddressCheck, 0, 5);

    controlLayout->addWidget(new QLabel(QStringLiteral("最大行数"), m_controlPanel), 1, 0);
    m_maxRowsSpin = new QSpinBox(m_controlPanel);
    m_maxRowsSpin->setRange(1000, 100000);
    m_maxRowsSpin->setSingleStep(1000);
    m_maxRowsSpin->setValue(12000);
    m_maxRowsSpin->setStyleSheet(blueInputStyle());
    controlLayout->addWidget(m_maxRowsSpin, 1, 1);

    controlLayout->addWidget(new QLabel(QStringLiteral("缓冲区(KB)"), m_controlPanel), 1, 2);
    m_bufferSizeSpin = new QSpinBox(m_controlPanel);
    m_bufferSizeSpin->setRange(64, 4096);
    m_bufferSizeSpin->setSingleStep(64);
    m_bufferSizeSpin->setValue(512);
    m_bufferSizeSpin->setStyleSheet(blueInputStyle());
    controlLayout->addWidget(m_bufferSizeSpin, 1, 3);

    m_reloadMapButton = createIconButton(
        m_controlPanel,
        QStringLiteral(":/Icon/process_refresh.svg"),
        QStringLiteral("重新解析 ntdll/win32u syscall 号映射"));
    m_startButton = createIconButton(
        m_controlPanel,
        QStringLiteral(":/Icon/process_start.svg"),
        QStringLiteral("开始直接内核调用监控"));
    m_stopButton = createIconButton(
        m_controlPanel,
        QStringLiteral(":/Icon/process_terminate.svg"),
        QStringLiteral("停止监控"));
    m_pauseButton = createIconButton(
        m_controlPanel,
        QStringLiteral(":/Icon/process_pause.svg"),
        QStringLiteral("暂停事件入表"));
    m_clearButton = createIconButton(
        m_controlPanel,
        QStringLiteral(":/Icon/log_clear.svg"),
        QStringLiteral("清空当前事件表"));
    m_exportButton = createIconButton(
        m_controlPanel,
        QStringLiteral(":/Icon/log_export.svg"),
        QStringLiteral("导出当前可见事件为 TSV"));

    QHBoxLayout* buttonLayout = new QHBoxLayout();
    buttonLayout->setContentsMargins(0, 0, 0, 0);
    buttonLayout->setSpacing(6);
    buttonLayout->addWidget(m_reloadMapButton);
    buttonLayout->addWidget(m_startButton);
    buttonLayout->addWidget(m_stopButton);
    buttonLayout->addWidget(m_pauseButton);
    buttonLayout->addWidget(m_clearButton);
    buttonLayout->addWidget(m_exportButton);
    buttonLayout->addStretch(1);
    controlLayout->addLayout(buttonLayout, 1, 4, 1, 2);

    m_mapStatusLabel = new QLabel(QStringLiteral("syscall 映射：待解析"), m_controlPanel);
    m_mapStatusLabel->setStyleSheet(buildStatusStyle(monitorIdleColorHex()));
    controlLayout->addWidget(m_mapStatusLabel, 2, 0, 1, 3);

    m_statusLabel = new QLabel(QStringLiteral("● 空闲"), m_controlPanel);
    m_statusLabel->setStyleSheet(buildStatusStyle(monitorIdleColorHex()));
    controlLayout->addWidget(m_statusLabel, 2, 3, 1, 3);
    m_rootLayout->addWidget(m_controlPanel, 0);

    m_filterPanel = new QWidget(this);
    QGridLayout* filterLayout = new QGridLayout(m_filterPanel);
    filterLayout->setContentsMargins(6, 6, 6, 6);
    filterLayout->setHorizontalSpacing(6);
    filterLayout->setVerticalSpacing(6);

    filterLayout->addWidget(new QLabel(QStringLiteral("进程"), m_filterPanel), 0, 0);
    m_processFilterEdit = new QLineEdit(m_filterPanel);
    m_processFilterEdit->setPlaceholderText(QStringLiteral("PID / TID / 进程名"));
    m_processFilterEdit->setStyleSheet(blueInputStyle());
    filterLayout->addWidget(m_processFilterEdit, 0, 1);

    filterLayout->addWidget(new QLabel(QStringLiteral("服务"), m_filterPanel), 0, 2);
    m_serviceFilterEdit = new QLineEdit(m_filterPanel);
    m_serviceFilterEdit->setPlaceholderText(QStringLiteral("Nt/Zw/NtUser/NtGdi 或调用号"));
    m_serviceFilterEdit->setStyleSheet(blueInputStyle());
    filterLayout->addWidget(m_serviceFilterEdit, 0, 3);

    filterLayout->addWidget(new QLabel(QStringLiteral("详情"), m_filterPanel), 0, 4);
    m_detailFilterEdit = new QLineEdit(m_filterPanel);
    m_detailFilterEdit->setPlaceholderText(QStringLiteral("调用地址 / 判定 / 字段详情"));
    m_detailFilterEdit->setStyleSheet(blueInputStyle());
    filterLayout->addWidget(m_detailFilterEdit, 0, 5);

    filterLayout->addWidget(new QLabel(QStringLiteral("全字段"), m_filterPanel), 1, 0);
    m_globalFilterEdit = new QLineEdit(m_filterPanel);
    m_globalFilterEdit->setPlaceholderText(QStringLiteral("对整行文本做统一过滤"));
    m_globalFilterEdit->setStyleSheet(blueInputStyle());
    filterLayout->addWidget(m_globalFilterEdit, 1, 1, 1, 3);

    m_regexCheck = new QCheckBox(QStringLiteral("正则"), m_filterPanel);
    m_caseCheck = new QCheckBox(QStringLiteral("区分大小写"), m_filterPanel);
    m_invertCheck = new QCheckBox(QStringLiteral("反向"), m_filterPanel);
    m_keepBottomCheck = new QCheckBox(QStringLiteral("保持贴底"), m_filterPanel);
    m_keepBottomCheck->setChecked(true);
    filterLayout->addWidget(m_regexCheck, 1, 4);
    filterLayout->addWidget(m_caseCheck, 1, 5);
    filterLayout->addWidget(m_invertCheck, 2, 0);
    filterLayout->addWidget(m_keepBottomCheck, 2, 1);

    m_clearFilterButton = createIconButton(
        m_filterPanel,
        QStringLiteral(":/Icon/log_clear.svg"),
        QStringLiteral("清空筛选条件"));
    filterLayout->addWidget(m_clearFilterButton, 2, 2);

    m_filterStatusLabel = new QLabel(QStringLiteral("筛选结果：0 / 0"), m_filterPanel);
    m_filterStatusLabel->setStyleSheet(buildStatusStyle(monitorIdleColorHex()));
    filterLayout->addWidget(m_filterStatusLabel, 2, 3, 1, 3);
    m_rootLayout->addWidget(m_filterPanel, 0);

    m_eventTable = new QTableWidget(this);
    m_eventTable->setColumnCount(EventColumnCount);
    m_eventTable->setHorizontalHeaderLabels(QStringList{
        QStringLiteral("时间(100ns)"),
        QStringLiteral("PID / TID"),
        QStringLiteral("进程"),
        QStringLiteral("调用号"),
        QStringLiteral("服务名"),
        QStringLiteral("判定"),
        QStringLiteral("调用地址"),
        QStringLiteral("事件名"),
        QStringLiteral("详情")
    });
    m_eventTable->setAlternatingRowColors(true);
    m_eventTable->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_eventTable->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_eventTable->setSelectionMode(QAbstractItemView::ExtendedSelection);
    m_eventTable->setContextMenuPolicy(Qt::CustomContextMenu);
    m_eventTable->verticalHeader()->setVisible(false);
    m_eventTable->horizontalHeader()->setStyleSheet(blueHeaderStyle());
    m_eventTable->horizontalHeader()->setSectionResizeMode(EventColumnTime100ns, QHeaderView::ResizeToContents);
    m_eventTable->horizontalHeader()->setSectionResizeMode(EventColumnPidTid, QHeaderView::ResizeToContents);
    m_eventTable->horizontalHeader()->setSectionResizeMode(EventColumnProcess, QHeaderView::ResizeToContents);
    m_eventTable->horizontalHeader()->setSectionResizeMode(EventColumnSyscallNumber, QHeaderView::ResizeToContents);
    m_eventTable->horizontalHeader()->setSectionResizeMode(EventColumnServiceName, QHeaderView::ResizeToContents);
    m_eventTable->horizontalHeader()->setSectionResizeMode(EventColumnVerdict, QHeaderView::ResizeToContents);
    m_eventTable->horizontalHeader()->setSectionResizeMode(EventColumnCallAddress, QHeaderView::ResizeToContents);
    m_eventTable->horizontalHeader()->setSectionResizeMode(EventColumnEventName, QHeaderView::ResizeToContents);
    m_eventTable->horizontalHeader()->setSectionResizeMode(EventColumnDetail, QHeaderView::Stretch);
    m_rootLayout->addWidget(m_eventTable, 1);

    m_uiUpdateTimer = new QTimer(this);
    m_uiUpdateTimer->setInterval(100);

    m_filterDebounceTimer = new QTimer(this);
    m_filterDebounceTimer->setInterval(180);
    m_filterDebounceTimer->setSingleShot(true);
}

void DirectKernelCallMonitorWidget::initializeConnections()
{
    connect(m_reloadMapButton, &QPushButton::clicked, this, [this]() {
        reloadSyscallMap();
    });
    connect(m_startButton, &QPushButton::clicked, this, [this]() {
        startCapture();
    });
    connect(m_stopButton, &QPushButton::clicked, this, [this]() {
        stopCapture();
    });
    connect(m_pauseButton, &QPushButton::clicked, this, [this]() {
        setCapturePaused(!m_capturePaused.load());
    });
    connect(m_clearButton, &QPushButton::clicked, this, [this]() {
        if (m_eventTable != nullptr)
        {
            m_eventTable->clearContents();
            m_eventTable->setRowCount(0);
        }
        updateActionState();
        updateStatusLabel();
        applyFilter();
    });
    connect(m_exportButton, &QPushButton::clicked, this, [this]() {
        exportVisibleRowsToTsv();
    });

    const auto bindFilterEdit = [this](QLineEdit* editPointer) {
        if (editPointer != nullptr)
        {
            connect(editPointer, &QLineEdit::textChanged, this, [this]() {
                scheduleFilterApply();
            });
        }
    };
    bindFilterEdit(m_processFilterEdit);
    bindFilterEdit(m_serviceFilterEdit);
    bindFilterEdit(m_detailFilterEdit);
    bindFilterEdit(m_globalFilterEdit);

    connect(m_regexCheck, &QCheckBox::toggled, this, [this]() {
        scheduleFilterApply();
    });
    connect(m_caseCheck, &QCheckBox::toggled, this, [this]() {
        scheduleFilterApply();
    });
    connect(m_invertCheck, &QCheckBox::toggled, this, [this]() {
        scheduleFilterApply();
    });
    connect(m_clearFilterButton, &QPushButton::clicked, this, [this]() {
        clearFilter();
    });

    connect(m_eventTable, &QTableWidget::customContextMenuRequested, this, [this](const QPoint& position) {
        showEventContextMenu(position);
    });
    connect(m_eventTable, &QTableWidget::itemDoubleClicked, this, [this](QTableWidgetItem* itemPointer) {
        if (itemPointer != nullptr)
        {
            openEventDetailViewerForRow(itemPointer->row());
        }
    });
    connect(m_uiUpdateTimer, &QTimer::timeout, this, [this]() {
        flushPendingRows();
    });
    connect(m_filterDebounceTimer, &QTimer::timeout, this, [this]() {
        applyFilter();
    });
}

void DirectKernelCallMonitorWidget::reloadSyscallMap()
{
    std::unordered_map<std::uint32_t, SyscallMapEntry> newMap;
    appendSyscallExportsFromModule(
        L"ntdll.dll",
        QStringList{ QStringLiteral("Nt"), QStringLiteral("Zw") },
        &newMap);
    appendSyscallExportsFromModule(
        L"win32u.dll",
        QStringList{ QStringLiteral("NtUser"), QStringLiteral("NtGdi") },
        &newMap);

    {
        std::lock_guard<std::mutex> lock(m_syscallMapMutex);
        m_syscallMap = std::move(newMap);
    }

    if (m_mapStatusLabel != nullptr)
    {
        m_mapStatusLabel->setText(QStringLiteral("syscall 映射：%1 项（ntdll/win32u）")
            .arg(static_cast<qulonglong>(m_syscallMap.size())));
        m_mapStatusLabel->setStyleSheet(buildStatusStyle(
            m_syscallMap.empty() ? monitorWarningColorHex() : monitorSuccessColorHex()));
    }

    kLogEvent event;
    info << event << "[DirectKernelCallMonitorWidget] 已解析 syscall 映射, count="
        << m_syscallMap.size() << eol;
}

void DirectKernelCallMonitorWidget::startCapture()
{
    if (m_captureRunning.load())
    {
        if (m_capturePaused.load())
        {
            setCapturePaused(false);
        }
        return;
    }

    if (m_captureThread != nullptr && m_captureThread->joinable())
    {
        m_captureThread->join();
        m_captureThread.reset();
    }

    const std::set<std::uint32_t> pidSet = parsePidSet(m_targetPidEdit != nullptr ? m_targetPidEdit->text() : QString());
    const bool captureAll = m_globalCaptureCheck != nullptr && m_globalCaptureCheck->isChecked();
    if (pidSet.empty() && !captureAll)
    {
        QMessageBox::information(
            this,
            QStringLiteral("直接内核调用监控"),
            QStringLiteral("请先输入目标 PID，或勾选“全局采集”。"));
        return;
    }

    if (captureAll)
    {
        const QMessageBox::StandardButton answer = QMessageBox::question(
            this,
            QStringLiteral("直接内核调用监控"),
            QStringLiteral("全局 syscall 事件量可能极大，建议只在短时间窗口内采集。是否继续？"),
            QMessageBox::Yes | QMessageBox::No,
            QMessageBox::No);
        if (answer != QMessageBox::Yes)
        {
            return;
        }
    }

    if (m_eventTable != nullptr)
    {
        m_eventTable->clearContents();
        m_eventTable->setRowCount(0);
    }
    {
        std::lock_guard<std::mutex> lock(m_pendingMutex);
        m_pendingRows.clear();
    }
    {
        std::lock_guard<std::mutex> lock(m_cacheMutex);
        m_processNameCache.clear();
        m_moduleRangeCache.clear();
    }
    {
        std::lock_guard<std::mutex> lock(m_captureConfigMutex);
        m_capturePidSet = pidSet;
    }

    const int bufferSizeKb = m_bufferSizeSpin != nullptr ? m_bufferSizeSpin->value() : 512;
    m_captureAllProcesses.store(captureAll);
    m_resolveCallAddress.store(m_resolveAddressCheck == nullptr || m_resolveAddressCheck->isChecked());
    m_captureRunning.store(true);
    m_capturePaused.store(false);
    m_captureStopFlag.store(false);
    m_sessionHandle.store(0);
    m_traceHandle.store(0);
    m_sessionName = QStringLiteral("KswordDirectKernelCall");
    stopActiveKswordTraceSessionsByPrefix(QStringList{ m_sessionName });

    if (m_captureProgressPid == 0)
    {
        m_captureProgressPid = kPro.add("监控", "直接内核调用监控");
    }
    kPro.set(m_captureProgressPid, "准备 System Syscall ETW 会话", 0, 10.0f);

    if (m_uiUpdateTimer != nullptr && !m_uiUpdateTimer->isActive())
    {
        m_uiUpdateTimer->start();
    }
    updateActionState();
    updateStatusLabel();

    QPointer<DirectKernelCallMonitorWidget> guardThis(this);
    m_captureThread = std::make_unique<std::thread>([guardThis, bufferSizeKb]() {
        if (guardThis == nullptr)
        {
            return;
        }

        const std::wstring sessionNameWide = guardThis->m_sessionName.toStdWString();
        const ULONG traceNameBytes = static_cast<ULONG>((sessionNameWide.size() + 1) * sizeof(wchar_t));
        const ULONG propertyBufferSize = static_cast<ULONG>(sizeof(EVENT_TRACE_PROPERTIES) + traceNameBytes);
        std::vector<unsigned char> propertyBuffer(propertyBufferSize, 0);
        auto* properties = reinterpret_cast<EVENT_TRACE_PROPERTIES*>(propertyBuffer.data());
        properties->Wnode.BufferSize = propertyBufferSize;
        properties->Wnode.ClientContext = 2;
        properties->Wnode.Flags = WNODE_FLAG_TRACED_GUID;
        properties->Wnode.Guid = kSystemTraceControlGuid;
        properties->LogFileMode = EVENT_TRACE_REAL_TIME_MODE | EVENT_TRACE_SYSTEM_LOGGER_MODE;
        properties->LoggerNameOffset = sizeof(EVENT_TRACE_PROPERTIES);
        properties->BufferSize = static_cast<ULONG>(bufferSizeKb);
        properties->MinimumBuffers = 32;
        properties->MaximumBuffers = 128;

        wchar_t* loggerNamePointer = reinterpret_cast<wchar_t*>(propertyBuffer.data() + properties->LoggerNameOffset);
        ::wcscpy_s(loggerNamePointer, sessionNameWide.size() + 1, sessionNameWide.c_str());

        TRACEHANDLE sessionHandle = 0;
        ULONG startStatus = ::StartTraceW(&sessionHandle, loggerNamePointer, properties);
        if (startStatus == ERROR_ALREADY_EXISTS)
        {
            ::ControlTraceW(0, loggerNamePointer, properties, EVENT_TRACE_CONTROL_STOP);
            startStatus = ::StartTraceW(&sessionHandle, loggerNamePointer, properties);
        }

        if (startStatus != ERROR_SUCCESS)
        {
            QMetaObject::invokeMethod(qApp, [guardThis, startStatus]() {
                if (guardThis == nullptr)
                {
                    return;
                }
                guardThis->m_captureRunning.store(false);
                guardThis->m_capturePaused.store(false);
                guardThis->m_statusLabel->setText(QStringLiteral("● StartTrace失败:%1").arg(startStatus));
                guardThis->m_statusLabel->setStyleSheet(buildStatusStyle(monitorErrorColorHex()));
                guardThis->updateActionState();
                kPro.set(guardThis->m_captureProgressPid, "System Syscall 会话启动失败", 0, 100.0f);
            }, Qt::QueuedConnection);
            return;
        }

        guardThis->m_sessionHandle.store(static_cast<std::uint64_t>(sessionHandle));
        kPro.set(guardThis->m_captureProgressPid, "启用 SystemSyscallProvider", 0, 30.0f);

        const ULONG enableStatus = ::EnableTraceEx2(
            sessionHandle,
            const_cast<GUID*>(&kSystemSyscallProviderGuid),
            EVENT_CONTROL_CODE_ENABLE_PROVIDER,
            TRACE_LEVEL_VERBOSE,
            SYSTEM_SYSCALL_KW_GENERAL,
            0,
            0,
            nullptr);
        if (enableStatus != ERROR_SUCCESS)
        {
            ::ControlTraceW(sessionHandle, loggerNamePointer, properties, EVENT_TRACE_CONTROL_STOP);
            guardThis->m_sessionHandle.store(0);
            QMetaObject::invokeMethod(qApp, [guardThis, enableStatus]() {
                if (guardThis == nullptr)
                {
                    return;
                }
                guardThis->m_captureRunning.store(false);
                guardThis->m_capturePaused.store(false);
                guardThis->m_statusLabel->setText(QStringLiteral("● EnableTrace失败:%1").arg(enableStatus));
                guardThis->m_statusLabel->setStyleSheet(buildStatusStyle(monitorErrorColorHex()));
                guardThis->updateActionState();
                kPro.set(guardThis->m_captureProgressPid, "SystemSyscallProvider 启用失败", 0, 100.0f);
            }, Qt::QueuedConnection);
            return;
        }

        EVENT_TRACE_LOGFILEW traceLogFile{};
        traceLogFile.LoggerName = loggerNamePointer;
        traceLogFile.ProcessTraceMode = PROCESS_TRACE_MODE_REAL_TIME | PROCESS_TRACE_MODE_EVENT_RECORD;
        traceLogFile.EventRecordCallback = &DirectKernelCallMonitorWidget::eventRecordCallback;
        traceLogFile.Context = guardThis.data();

        TRACEHANDLE traceHandle = ::OpenTraceW(&traceLogFile);
        if (traceHandle == INVALID_PROCESSTRACE_HANDLE)
        {
            ::ControlTraceW(sessionHandle, loggerNamePointer, properties, EVENT_TRACE_CONTROL_STOP);
            guardThis->m_sessionHandle.store(0);
            const ULONG lastError = ::GetLastError();
            QMetaObject::invokeMethod(qApp, [guardThis, lastError]() {
                if (guardThis == nullptr)
                {
                    return;
                }
                guardThis->m_captureRunning.store(false);
                guardThis->m_capturePaused.store(false);
                guardThis->m_statusLabel->setText(QStringLiteral("● OpenTrace失败:%1").arg(lastError));
                guardThis->m_statusLabel->setStyleSheet(buildStatusStyle(monitorErrorColorHex()));
                guardThis->updateActionState();
                kPro.set(guardThis->m_captureProgressPid, "OpenTrace 失败", 0, 100.0f);
            }, Qt::QueuedConnection);
            return;
        }

        guardThis->m_traceHandle.store(static_cast<std::uint64_t>(traceHandle));
        kPro.set(guardThis->m_captureProgressPid, "接收 syscall 事件", 0, 55.0f);

        const ULONG processStatus = ::ProcessTrace(&traceHandle, 1, nullptr, nullptr);
        const std::uint64_t ownedTraceHandle = guardThis->m_traceHandle.exchange(0);
        if (ownedTraceHandle != 0)
        {
            ::CloseTrace(static_cast<TRACEHANDLE>(ownedTraceHandle));
        }

        const std::uint64_t ownedSessionHandle = guardThis->m_sessionHandle.exchange(0);
        if (ownedSessionHandle != 0)
        {
            ::ControlTraceW(
                static_cast<TRACEHANDLE>(ownedSessionHandle),
                loggerNamePointer,
                properties,
                EVENT_TRACE_CONTROL_STOP);
        }

        QMetaObject::invokeMethod(qApp, [guardThis, processStatus]() {
            if (guardThis == nullptr)
            {
                return;
            }
            guardThis->m_captureRunning.store(false);
            guardThis->m_capturePaused.store(false);
            if (guardThis->m_uiUpdateTimer != nullptr)
            {
                guardThis->flushPendingRows();
                guardThis->m_uiUpdateTimer->stop();
            }
            if (processStatus == ERROR_SUCCESS)
            {
                guardThis->m_statusLabel->setText(QStringLiteral("● 已停止"));
                guardThis->m_statusLabel->setStyleSheet(buildStatusStyle(monitorIdleColorHex()));
            }
            else
            {
                guardThis->m_statusLabel->setText(QStringLiteral("● ProcessTrace结束:%1").arg(processStatus));
                guardThis->m_statusLabel->setStyleSheet(buildStatusStyle(monitorWarningColorHex()));
            }
            guardThis->updateActionState();
            guardThis->updateStatusLabel();
            kPro.set(guardThis->m_captureProgressPid, "直接内核调用监控结束", 0, 100.0f);
        }, Qt::QueuedConnection);
    });
}

void DirectKernelCallMonitorWidget::stopCapture()
{
    stopCaptureInternal(false);
}

void DirectKernelCallMonitorWidget::stopCaptureInternal(bool waitForThread)
{
    m_captureStopFlag.store(true);

    const std::uint64_t ownedTraceHandle = m_traceHandle.exchange(0);
    if (ownedTraceHandle != 0)
    {
        ::CloseTrace(static_cast<TRACEHANDLE>(ownedTraceHandle));
    }

    const std::uint64_t ownedSessionHandle = m_sessionHandle.exchange(0);
    if (ownedSessionHandle != 0)
    {
        const std::wstring sessionNameWide = m_sessionName.toStdWString();
        std::vector<unsigned char> propertyBuffer(
            sizeof(EVENT_TRACE_PROPERTIES) + (sessionNameWide.size() + 1) * sizeof(wchar_t),
            0);
        auto* properties = reinterpret_cast<EVENT_TRACE_PROPERTIES*>(propertyBuffer.data());
        properties->Wnode.BufferSize = static_cast<ULONG>(propertyBuffer.size());
        properties->LoggerNameOffset = sizeof(EVENT_TRACE_PROPERTIES);
        wchar_t* loggerNamePointer = reinterpret_cast<wchar_t*>(propertyBuffer.data() + properties->LoggerNameOffset);
        if (!sessionNameWide.empty())
        {
            ::wcscpy_s(loggerNamePointer, sessionNameWide.size() + 1, sessionNameWide.c_str());
        }

        ::ControlTraceW(
            static_cast<TRACEHANDLE>(ownedSessionHandle),
            loggerNamePointer,
            properties,
            EVENT_TRACE_CONTROL_STOP);
    }

    if (m_captureThread == nullptr || !m_captureThread->joinable())
    {
        m_captureThread.reset();
        m_captureRunning.store(false);
        m_capturePaused.store(false);
        if (m_uiUpdateTimer != nullptr)
        {
            m_uiUpdateTimer->stop();
        }
        updateActionState();
        updateStatusLabel();
        return;
    }

    if (waitForThread)
    {
        m_captureThread->join();
        m_captureThread.reset();
        m_captureRunning.store(false);
        m_capturePaused.store(false);
        if (m_uiUpdateTimer != nullptr)
        {
            m_uiUpdateTimer->stop();
        }
        updateActionState();
        updateStatusLabel();
        return;
    }

    if (m_statusLabel != nullptr)
    {
        m_statusLabel->setText(QStringLiteral("● 停止中..."));
        m_statusLabel->setStyleSheet(buildStatusStyle(monitorWarningColorHex()));
    }
    std::unique_ptr<std::thread> joinThread = std::move(m_captureThread);
    QPointer<DirectKernelCallMonitorWidget> guardThis(this);
    std::thread([joinThread = std::move(joinThread), guardThis]() mutable {
        if (joinThread != nullptr && joinThread->joinable())
        {
            joinThread->join();
        }
        QMetaObject::invokeMethod(qApp, [guardThis]() {
            if (guardThis == nullptr)
            {
                return;
            }
            guardThis->m_captureRunning.store(false);
            guardThis->m_capturePaused.store(false);
            if (guardThis->m_uiUpdateTimer != nullptr)
            {
                guardThis->flushPendingRows();
                guardThis->m_uiUpdateTimer->stop();
            }
            guardThis->updateActionState();
            guardThis->updateStatusLabel();
        }, Qt::QueuedConnection);
    }).detach();

    updateActionState();
}

void DirectKernelCallMonitorWidget::setCapturePaused(bool paused)
{
    if (!m_captureRunning.load())
    {
        return;
    }
    m_capturePaused.store(paused);
    updateActionState();
    updateStatusLabel();
}

void DirectKernelCallMonitorWidget::updateActionState()
{
    const bool running = m_captureRunning.load();
    const bool paused = m_capturePaused.load();
    const bool hasRows = m_eventTable != nullptr && m_eventTable->rowCount() > 0;

    if (m_targetPidEdit != nullptr)
    {
        m_targetPidEdit->setEnabled(!running);
    }
    if (m_globalCaptureCheck != nullptr)
    {
        m_globalCaptureCheck->setEnabled(!running);
    }
    if (m_bufferSizeSpin != nullptr)
    {
        m_bufferSizeSpin->setEnabled(!running);
    }
    if (m_reloadMapButton != nullptr)
    {
        m_reloadMapButton->setEnabled(!running);
    }
    if (m_startButton != nullptr)
    {
        m_startButton->setEnabled(!running || paused);
        m_startButton->setIcon(QIcon(paused
            ? QStringLiteral(":/Icon/process_resume.svg")
            : QStringLiteral(":/Icon/process_start.svg")));
        m_startButton->setToolTip(paused
            ? QStringLiteral("继续直接内核调用监控")
            : QStringLiteral("开始直接内核调用监控"));
    }
    if (m_stopButton != nullptr)
    {
        m_stopButton->setEnabled(running);
    }
    if (m_pauseButton != nullptr)
    {
        m_pauseButton->setEnabled(running);
        m_pauseButton->setIcon(QIcon(paused
            ? QStringLiteral(":/Icon/process_resume.svg")
            : QStringLiteral(":/Icon/process_pause.svg")));
        m_pauseButton->setToolTip(paused
            ? QStringLiteral("继续事件入表")
            : QStringLiteral("暂停事件入表"));
    }
    if (m_clearButton != nullptr)
    {
        m_clearButton->setEnabled(!running && hasRows);
    }
    if (m_exportButton != nullptr)
    {
        m_exportButton->setEnabled(hasRows);
    }
}

void DirectKernelCallMonitorWidget::updateStatusLabel()
{
    if (m_statusLabel == nullptr)
    {
        return;
    }
    const int eventCount = m_eventTable != nullptr ? m_eventTable->rowCount() : 0;
    QString targetText;
    if (m_captureAllProcesses.load())
    {
        targetText = QStringLiteral("全局");
    }
    else
    {
        std::lock_guard<std::mutex> lock(m_captureConfigMutex);
        targetText = QStringLiteral("PID=%1").arg(static_cast<qulonglong>(m_capturePidSet.size()));
    }

    if (m_captureRunning.load())
    {
        if (m_capturePaused.load())
        {
            m_statusLabel->setText(QStringLiteral("● 已暂停  %1 | 事件=%2").arg(targetText).arg(eventCount));
            m_statusLabel->setStyleSheet(buildStatusStyle(monitorWarningColorHex()));
        }
        else
        {
            m_statusLabel->setText(QStringLiteral("● 监听中  %1 | 事件=%2").arg(targetText).arg(eventCount));
            m_statusLabel->setStyleSheet(buildStatusStyle(monitorInfoColorHex()));
        }
    }
    else
    {
        m_statusLabel->setText(QStringLiteral("● 空闲  事件=%1").arg(eventCount));
        m_statusLabel->setStyleSheet(buildStatusStyle(monitorIdleColorHex()));
    }
}

void WINAPI DirectKernelCallMonitorWidget::eventRecordCallback(struct _EVENT_RECORD* eventRecordPtr)
{
    if (eventRecordPtr == nullptr)
    {
        return;
    }
    EVENT_RECORD* eventRecord = reinterpret_cast<EVENT_RECORD*>(eventRecordPtr);
    if (eventRecord->UserContext == nullptr)
    {
        return;
    }
    auto* widget = reinterpret_cast<DirectKernelCallMonitorWidget*>(eventRecord->UserContext);
    widget->enqueueEventFromRecord(eventRecordPtr);
}

void DirectKernelCallMonitorWidget::enqueueEventFromRecord(const struct _EVENT_RECORD* eventRecordPtr)
{
    const EVENT_RECORD* eventRecord = reinterpret_cast<const EVENT_RECORD*>(eventRecordPtr);
    if (eventRecord == nullptr || m_captureStopFlag.load() || m_capturePaused.load())
    {
        return;
    }
    const std::uint32_t pidValue = static_cast<std::uint32_t>(eventRecord->EventHeader.ProcessId);
    if (!shouldCapturePid(pidValue))
    {
        return;
    }

    CapturedEventRow row = buildRowFromRecord(eventRecordPtr);
    {
        std::lock_guard<std::mutex> lock(m_pendingMutex);
        m_pendingRows.push_back(std::move(row));
    }
}

DirectKernelCallMonitorWidget::CapturedEventRow DirectKernelCallMonitorWidget::buildRowFromRecord(
    const struct _EVENT_RECORD* eventRecordPtr)
{
    const EVENT_RECORD* eventRecord = reinterpret_cast<const EVENT_RECORD*>(eventRecordPtr);
    CapturedEventRow row;
    if (eventRecord == nullptr)
    {
        return row;
    }

    row.time100nsText = QString::number(static_cast<qlonglong>(eventRecord->EventHeader.TimeStamp.QuadPart));
    row.pid = static_cast<std::uint32_t>(eventRecord->EventHeader.ProcessId);
    row.tid = static_cast<std::uint32_t>(eventRecord->EventHeader.ThreadId);
    row.pidTidText = QStringLiteral("%1 / %2").arg(row.pid).arg(row.tid);
    row.processText = processNameForPid(row.pid);
    row.eventName = QStringLiteral("Event_%1").arg(eventRecord->EventHeader.EventDescriptor.Id);

    QString decodedEventName;
    const std::vector<DecodedProperty> propertyList = decodeEventProperties(eventRecordPtr, &decodedEventName);
    if (!decodedEventName.trimmed().isEmpty())
    {
        row.eventName = decodedEventName.trimmed();
    }

    QStringList propertyLineList;
    std::optional<std::uint32_t> syscallNumber;
    std::optional<std::uint64_t> callAddress;
    for (const DecodedProperty& property : propertyList)
    {
        propertyLineList << QStringLiteral("%1=%2").arg(property.name, property.valueText);
        const QString normalized = normalizeName(property.name);
        if (property.hasNumericValue)
        {
            const bool looksLikeSyscallNumber = normalized.contains(QStringLiteral("syscall"))
                || normalized.contains(QStringLiteral("systemcall"))
                || normalized.contains(QStringLiteral("servicenumber"))
                || normalized.contains(QStringLiteral("serviceid"))
                || normalized.contains(QStringLiteral("syscallid"));
            if (!syscallNumber.has_value() && looksLikeSyscallNumber && property.numericValue < 0x10000ULL)
            {
                syscallNumber = static_cast<std::uint32_t>(property.numericValue);
            }

            const bool looksLikeAddress = normalized.contains(QStringLiteral("calladdress"))
                || normalized.contains(QStringLiteral("returnaddress"))
                || normalized.contains(QStringLiteral("instructionpointer"))
                || normalized == QStringLiteral("ip")
                || normalized == QStringLiteral("pc")
                || normalized.contains(QStringLiteral("programcounter"));
            if (!callAddress.has_value() && looksLikeAddress && property.numericValue > 0x10000ULL)
            {
                callAddress = property.numericValue;
            }
        }
    }

    if (!syscallNumber.has_value() && eventRecord->UserData != nullptr && eventRecord->UserDataLength >= sizeof(std::uint32_t))
    {
        std::uint32_t candidate = 0;
        std::memcpy(&candidate, eventRecord->UserData, sizeof(candidate));
        if (candidate < 0x10000U)
        {
            syscallNumber = candidate;
            propertyLineList << QStringLiteral("fallback.raw0.syscallCandidate=%1").arg(candidate);
        }
    }

    if (syscallNumber.has_value())
    {
        row.hasSyscallNumber = true;
        row.syscallNumber = *syscallNumber;
        row.syscallNumberText = QStringLiteral("%1 / 0x%2")
            .arg(row.syscallNumber)
            .arg(row.syscallNumber, 4, 16, QChar(u'0'))
            .toUpper();
        row.serviceName = serviceNameForNumber(row.syscallNumber);
    }
    else
    {
        row.syscallNumberText = QStringLiteral("<未知>");
        row.serviceName = QStringLiteral("<未解析>");
    }

    if (callAddress.has_value())
    {
        row.callAddress = *callAddress;
        row.callAddressText = formatAddress(row.callAddress);
    }
    else
    {
        row.callAddressText = QStringLiteral("<未知>");
    }

    QString callModuleText;
    if (row.callAddress != 0 && m_resolveCallAddress.load())
    {
        callModuleText = moduleNameForAddress(row.pid, row.callAddress);
    }

    if (row.callAddress == 0)
    {
        row.verdictText = QStringLiteral("待判定");
    }
    else if (callModuleText.compare(QStringLiteral("ntdll.dll"), Qt::CaseInsensitive) == 0
        || callModuleText.compare(QStringLiteral("win32u.dll"), Qt::CaseInsensitive) == 0)
    {
        row.verdictText = QStringLiteral("常规导出桩");
    }
    else if (!callModuleText.trimmed().isEmpty())
    {
        row.verdictText = QStringLiteral("疑似直接调用:%1").arg(callModuleText);
    }
    else
    {
        row.verdictText = QStringLiteral("疑似直接调用");
    }

    row.detailText = QStringLiteral("%1 | %2 | %3")
        .arg(row.verdictText, row.callAddressText, row.serviceName);
    row.detailAllText = QStringLiteral(
        "Provider: System Syscall (%1)\n"
        "EventId: %2\n"
        "EventName: %3\n"
        "PID/TID: %4\n"
        "Process: %5\n"
        "Syscall: %6\n"
        "Service: %7\n"
        "CallAddress: %8\n"
        "Verdict: %9\n"
        "Properties:\n%10")
        .arg(guidToText(eventRecord->EventHeader.ProviderId))
        .arg(eventRecord->EventHeader.EventDescriptor.Id)
        .arg(row.eventName)
        .arg(row.pidTidText)
        .arg(row.processText)
        .arg(row.syscallNumberText)
        .arg(row.serviceName)
        .arg(callModuleText.isEmpty() ? row.callAddressText : QStringLiteral("%1 (%2)").arg(row.callAddressText, callModuleText))
        .arg(row.verdictText)
        .arg(propertyLineList.isEmpty() ? QStringLiteral("<无 TDH 字段>") : propertyLineList.join(QChar(u'\n')));
    row.globalSearchText = QStringLiteral("%1 | %2 | %3 | %4 | %5 | %6 | %7 | %8")
        .arg(row.time100nsText)
        .arg(row.pidTidText)
        .arg(row.processText)
        .arg(row.syscallNumberText)
        .arg(row.serviceName)
        .arg(row.verdictText)
        .arg(row.callAddressText)
        .arg(row.detailAllText);
    return row;
}

std::vector<DirectKernelCallMonitorWidget::DecodedProperty> DirectKernelCallMonitorWidget::decodeEventProperties(
    const struct _EVENT_RECORD* eventRecordPtr,
    QString* eventNameOut) const
{
    const EVENT_RECORD* eventRecord = reinterpret_cast<const EVENT_RECORD*>(eventRecordPtr);
    std::vector<DecodedProperty> propertyList;
    if (eventRecord == nullptr)
    {
        return propertyList;
    }

    ULONG infoBufferSize = 0;
    ULONG status = ::TdhGetEventInformation(
        const_cast<EVENT_RECORD*>(eventRecord),
        0,
        nullptr,
        nullptr,
        &infoBufferSize);
    if (status != ERROR_INSUFFICIENT_BUFFER || infoBufferSize == 0)
    {
        return propertyList;
    }

    std::vector<unsigned char> infoBuffer(infoBufferSize, 0);
    auto* traceInfo = reinterpret_cast<TRACE_EVENT_INFO*>(infoBuffer.data());
    status = ::TdhGetEventInformation(
        const_cast<EVENT_RECORD*>(eventRecord),
        0,
        nullptr,
        traceInfo,
        &infoBufferSize);
    if (status != ERROR_SUCCESS)
    {
        return propertyList;
    }

    if (eventNameOut != nullptr)
    {
        *eventNameOut = eventNameFromInfo(infoBuffer.data(), traceInfo);
    }

    propertyList.reserve(traceInfo->TopLevelPropertyCount);
    for (ULONG propertyIndex = 0; propertyIndex < traceInfo->TopLevelPropertyCount; ++propertyIndex)
    {
        const EVENT_PROPERTY_INFO& propertyInfo = traceInfo->EventPropertyInfoArray[propertyIndex];
        if ((propertyInfo.Flags & PropertyStruct) != 0 || propertyInfo.NameOffset == 0)
        {
            continue;
        }

        const wchar_t* propertyNamePointer = reinterpret_cast<const wchar_t*>(
            infoBuffer.data() + propertyInfo.NameOffset);
        const QString propertyName = QString::fromWCharArray(propertyNamePointer).trimmed();
        if (propertyName.isEmpty())
        {
            continue;
        }

        PROPERTY_DATA_DESCRIPTOR descriptor{};
        descriptor.PropertyName = reinterpret_cast<ULONGLONG>(propertyNamePointer);
        descriptor.ArrayIndex = ULONG_MAX;

        ULONG propertySize = 0;
        status = ::TdhGetPropertySize(
            const_cast<EVENT_RECORD*>(eventRecord),
            0,
            nullptr,
            1,
            &descriptor,
            &propertySize);
        if (status != ERROR_SUCCESS || propertySize == 0 || propertySize > 4096)
        {
            continue;
        }

        std::vector<unsigned char> propertyBuffer(propertySize, 0);
        status = ::TdhGetProperty(
            const_cast<EVENT_RECORD*>(eventRecord),
            0,
            nullptr,
            1,
            &descriptor,
            propertySize,
            propertyBuffer.data());
        if (status != ERROR_SUCCESS)
        {
            continue;
        }

        DecodedProperty decodedProperty;
        decodedProperty.name = propertyName;
        decodedProperty.valueText = decodedValueText(
            propertyBuffer.data(),
            propertySize,
            propertyInfo.nonStructType.InType,
            &decodedProperty.numericValue,
            &decodedProperty.hasNumericValue);
        propertyList.push_back(std::move(decodedProperty));
    }
    return propertyList;
}

QString DirectKernelCallMonitorWidget::serviceNameForNumber(std::uint32_t syscallNumber) const
{
    std::lock_guard<std::mutex> lock(m_syscallMapMutex);
    const auto found = m_syscallMap.find(syscallNumber);
    if (found == m_syscallMap.end())
    {
        return QStringLiteral("<未命中映射>");
    }
    return found->second.serviceName;
}

QString DirectKernelCallMonitorWidget::processNameForPid(std::uint32_t pid)
{
    if (pid == 0)
    {
        return QStringLiteral("System");
    }

    {
        std::lock_guard<std::mutex> lock(m_cacheMutex);
        const auto found = m_processNameCache.find(pid);
        if (found != m_processNameCache.end())
        {
            return found->second;
        }
    }

    QString processText = QStringLiteral("PID %1").arg(pid);
    HANDLE processHandle = ::OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, static_cast<DWORD>(pid));
    if (processHandle != nullptr)
    {
        std::vector<wchar_t> pathBuffer(32768, L'\0');
        DWORD pathLength = static_cast<DWORD>(pathBuffer.size());
        if (::QueryFullProcessImageNameW(processHandle, 0, pathBuffer.data(), &pathLength) != FALSE && pathLength > 0)
        {
            const QString imagePath = QString::fromWCharArray(pathBuffer.data(), static_cast<int>(pathLength));
            const QString fileName = QFileInfo(imagePath).fileName();
            processText = fileName.isEmpty()
                ? QStringLiteral("PID %1").arg(pid)
                : QStringLiteral("%1 (%2)").arg(fileName).arg(pid);
        }
        ::CloseHandle(processHandle);
    }

    {
        std::lock_guard<std::mutex> lock(m_cacheMutex);
        m_processNameCache[pid] = processText;
    }
    return processText;
}

QString DirectKernelCallMonitorWidget::moduleNameForAddress(std::uint32_t pid, std::uint64_t addressValue)
{
    if (pid == 0 || addressValue == 0)
    {
        return QString();
    }

    {
        std::lock_guard<std::mutex> lock(m_cacheMutex);
        if (m_moduleRangeCache.find(pid) == m_moduleRangeCache.end())
        {
            refreshModuleRangesForPid(pid);
        }
        const auto found = m_moduleRangeCache.find(pid);
        if (found != m_moduleRangeCache.end())
        {
            for (const ModuleRange& range : found->second)
            {
                if (addressValue >= range.startAddress && addressValue < range.endAddress)
                {
                    return range.moduleName;
                }
            }
        }
    }
    return QString();
}

void DirectKernelCallMonitorWidget::refreshModuleRangesForPid(std::uint32_t pid)
{
    std::vector<ModuleRange> rangeList;
    HANDLE snapshotHandle = ::CreateToolhelp32Snapshot(
        TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32,
        static_cast<DWORD>(pid));
    if (snapshotHandle != INVALID_HANDLE_VALUE)
    {
        MODULEENTRY32W moduleEntry{};
        moduleEntry.dwSize = sizeof(moduleEntry);
        if (::Module32FirstW(snapshotHandle, &moduleEntry) != FALSE)
        {
            do
            {
                ModuleRange range;
                range.startAddress = reinterpret_cast<std::uint64_t>(moduleEntry.modBaseAddr);
                range.endAddress = range.startAddress + static_cast<std::uint64_t>(moduleEntry.modBaseSize);
                range.moduleName = QString::fromWCharArray(moduleEntry.szModule);
                range.imagePath = QString::fromWCharArray(moduleEntry.szExePath);
                rangeList.push_back(std::move(range));
            } while (::Module32NextW(snapshotHandle, &moduleEntry) != FALSE);
        }
        ::CloseHandle(snapshotHandle);
    }

    m_moduleRangeCache[pid] = std::move(rangeList);
}

std::set<std::uint32_t> DirectKernelCallMonitorWidget::parsePidSet(const QString& text) const
{
    std::set<std::uint32_t> pidSet;
    const QStringList tokenList = text.split(QRegularExpression(QStringLiteral("[,;\\s]+")), Qt::SkipEmptyParts);
    for (const QString& token : tokenList)
    {
        QString normalized = token.trimmed();
        bool ok = false;
        std::uint32_t pid = 0;
        if (normalized.startsWith(QStringLiteral("0x"), Qt::CaseInsensitive))
        {
            pid = normalized.mid(2).toUInt(&ok, 16);
        }
        else
        {
            pid = normalized.toUInt(&ok, 10);
        }
        if (ok && pid != 0)
        {
            pidSet.insert(pid);
        }
    }
    return pidSet;
}

bool DirectKernelCallMonitorWidget::shouldCapturePid(std::uint32_t pid) const
{
    if (m_captureAllProcesses.load())
    {
        return true;
    }
    std::lock_guard<std::mutex> lock(m_captureConfigMutex);
    return m_capturePidSet.find(pid) != m_capturePidSet.end();
}

void DirectKernelCallMonitorWidget::flushPendingRows()
{
    if (m_eventTable == nullptr)
    {
        return;
    }

    std::vector<CapturedEventRow> rowList;
    {
        std::lock_guard<std::mutex> lock(m_pendingMutex);
        rowList.swap(m_pendingRows);
    }
    if (rowList.empty())
    {
        return;
    }

    m_eventTable->setUpdatesEnabled(false);
    for (const CapturedEventRow& rowValue : rowList)
    {
        appendEventRow(rowValue);
    }
    const int maxRows = m_maxRowsSpin != nullptr ? m_maxRowsSpin->value() : 12000;
    while (m_eventTable->rowCount() > maxRows)
    {
        m_eventTable->removeRow(0);
    }
    m_eventTable->setUpdatesEnabled(true);

    applyFilter();
    updateActionState();
    updateStatusLabel();

    if (m_keepBottomCheck != nullptr && m_keepBottomCheck->isChecked())
    {
        m_eventTable->scrollToBottom();
    }
}

void DirectKernelCallMonitorWidget::appendEventRow(const CapturedEventRow& rowValue)
{
    const int row = m_eventTable->rowCount();
    m_eventTable->insertRow(row);

    QTableWidgetItem* timeItem = createReadOnlyItem(rowValue.time100nsText);
    timeItem->setData(kRoleProcessSearchText, QStringLiteral("%1 | %2").arg(rowValue.pidTidText, rowValue.processText));
    timeItem->setData(kRoleServiceSearchText, QStringLiteral("%1 | %2").arg(rowValue.syscallNumberText, rowValue.serviceName));
    timeItem->setData(kRoleGlobalSearchText, rowValue.globalSearchText);
    timeItem->setData(kRoleDetailText, rowValue.detailAllText);

    m_eventTable->setItem(row, EventColumnTime100ns, timeItem);
    m_eventTable->setItem(row, EventColumnPidTid, createReadOnlyItem(rowValue.pidTidText));
    m_eventTable->setItem(row, EventColumnProcess, createReadOnlyItem(rowValue.processText));
    m_eventTable->setItem(row, EventColumnSyscallNumber, createReadOnlyItem(rowValue.syscallNumberText));
    m_eventTable->setItem(row, EventColumnServiceName, createReadOnlyItem(rowValue.serviceName));
    m_eventTable->setItem(row, EventColumnVerdict, createReadOnlyItem(rowValue.verdictText));
    m_eventTable->setItem(row, EventColumnCallAddress, createReadOnlyItem(rowValue.callAddressText));
    m_eventTable->setItem(row, EventColumnEventName, createReadOnlyItem(rowValue.eventName));
    m_eventTable->setItem(row, EventColumnDetail, createReadOnlyItem(rowValue.detailText));
}

void DirectKernelCallMonitorWidget::scheduleFilterApply()
{
    if (m_filterDebounceTimer != nullptr)
    {
        m_filterDebounceTimer->start();
    }
}

void DirectKernelCallMonitorWidget::applyFilter()
{
    if (m_eventTable == nullptr)
    {
        return;
    }

    const QString processFilter = m_processFilterEdit != nullptr ? m_processFilterEdit->text() : QString();
    const QString serviceFilter = m_serviceFilterEdit != nullptr ? m_serviceFilterEdit->text() : QString();
    const QString detailFilter = m_detailFilterEdit != nullptr ? m_detailFilterEdit->text() : QString();
    const QString globalFilter = m_globalFilterEdit != nullptr ? m_globalFilterEdit->text() : QString();
    const bool useRegex = m_regexCheck != nullptr && m_regexCheck->isChecked();
    const bool invertMatch = m_invertCheck != nullptr && m_invertCheck->isChecked();
    const Qt::CaseSensitivity caseSensitivity =
        (m_caseCheck != nullptr && m_caseCheck->isChecked())
        ? Qt::CaseSensitive
        : Qt::CaseInsensitive;

    int visibleCount = 0;
    for (int row = 0; row < m_eventTable->rowCount(); ++row)
    {
        QTableWidgetItem* timeItem = m_eventTable->item(row, EventColumnTime100ns);
        const QString processText = timeItem != nullptr ? timeItem->data(kRoleProcessSearchText).toString() : QString();
        const QString serviceText = timeItem != nullptr ? timeItem->data(kRoleServiceSearchText).toString() : QString();
        const QString globalText = timeItem != nullptr ? timeItem->data(kRoleGlobalSearchText).toString() : QString();
        const QString detailText = m_eventTable->item(row, EventColumnDetail) != nullptr
            ? m_eventTable->item(row, EventColumnDetail)->text()
            : QString();

        bool matched = textMatch(processText, processFilter, useRegex, caseSensitivity)
            && textMatch(serviceText, serviceFilter, useRegex, caseSensitivity)
            && textMatch(detailText, detailFilter, useRegex, caseSensitivity)
            && textMatch(globalText, globalFilter, useRegex, caseSensitivity);
        if (invertMatch)
        {
            matched = !matched;
        }

        m_eventTable->setRowHidden(row, !matched);
        if (matched)
        {
            ++visibleCount;
        }
    }

    if (m_filterStatusLabel != nullptr)
    {
        m_filterStatusLabel->setText(QStringLiteral("筛选结果：%1 / %2").arg(visibleCount).arg(m_eventTable->rowCount()));
        m_filterStatusLabel->setStyleSheet(buildStatusStyle(
            visibleCount > 0 ? monitorSuccessColorHex() : monitorIdleColorHex()));
    }
}

void DirectKernelCallMonitorWidget::clearFilter()
{
    const QSignalBlocker processBlocker(m_processFilterEdit);
    const QSignalBlocker serviceBlocker(m_serviceFilterEdit);
    const QSignalBlocker detailBlocker(m_detailFilterEdit);
    const QSignalBlocker globalBlocker(m_globalFilterEdit);
    const QSignalBlocker regexBlocker(m_regexCheck);
    const QSignalBlocker caseBlocker(m_caseCheck);
    const QSignalBlocker invertBlocker(m_invertCheck);

    if (m_processFilterEdit != nullptr)
    {
        m_processFilterEdit->clear();
    }
    if (m_serviceFilterEdit != nullptr)
    {
        m_serviceFilterEdit->clear();
    }
    if (m_detailFilterEdit != nullptr)
    {
        m_detailFilterEdit->clear();
    }
    if (m_globalFilterEdit != nullptr)
    {
        m_globalFilterEdit->clear();
    }
    if (m_regexCheck != nullptr)
    {
        m_regexCheck->setChecked(false);
    }
    if (m_caseCheck != nullptr)
    {
        m_caseCheck->setChecked(false);
    }
    if (m_invertCheck != nullptr)
    {
        m_invertCheck->setChecked(false);
    }
    applyFilter();
}

void DirectKernelCallMonitorWidget::exportVisibleRowsToTsv()
{
    if (m_eventTable == nullptr || m_eventTable->rowCount() == 0)
    {
        QMessageBox::information(this, QStringLiteral("导出"), QStringLiteral("当前没有可导出的事件。"));
        return;
    }

    const QString filePath = QFileDialog::getSaveFileName(
        this,
        QStringLiteral("导出直接内核调用事件"),
        QStringLiteral("direct-kernel-call-events.tsv"),
        QStringLiteral("TSV 文件 (*.tsv);;所有文件 (*.*)"));
    if (filePath.isEmpty())
    {
        return;
    }

    QFile outputFile(filePath);
    if (!outputFile.open(QIODevice::WriteOnly | QIODevice::Text | QIODevice::Truncate))
    {
        QMessageBox::warning(this, QStringLiteral("导出"), QStringLiteral("无法写入文件：%1").arg(filePath));
        return;
    }

    QTextStream stream(&outputFile);
    QStringList headerList;
    for (int column = 0; column < m_eventTable->columnCount(); ++column)
    {
        headerList << m_eventTable->horizontalHeaderItem(column)->text();
    }
    stream << headerList.join(QChar(u'\t')) << QChar(u'\n');

    for (int row = 0; row < m_eventTable->rowCount(); ++row)
    {
        if (m_eventTable->isRowHidden(row))
        {
            continue;
        }
        QStringList cellList;
        for (int column = 0; column < m_eventTable->columnCount(); ++column)
        {
            QString text = m_eventTable->item(row, column) != nullptr
                ? m_eventTable->item(row, column)->text()
                : QString();
            text.replace(QChar(u'\t'), QChar(u' '));
            text.replace(QChar(u'\n'), QChar(u' '));
            cellList << text;
        }
        stream << cellList.join(QChar(u'\t')) << QChar(u'\n');
    }
    outputFile.close();
}

void DirectKernelCallMonitorWidget::showEventContextMenu(const QPoint& position)
{
    if (m_eventTable == nullptr)
    {
        return;
    }
    QTableWidgetItem* itemPointer = m_eventTable->itemAt(position);
    if (itemPointer == nullptr)
    {
        return;
    }

    const int row = itemPointer->row();
    QMenu menu(this);
    QAction* detailAction = menu.addAction(QStringLiteral("查看详情"));
    QAction* copyRowAction = menu.addAction(QStringLiteral("复制当前行"));
    QAction* copyDetailAction = menu.addAction(QStringLiteral("复制详情"));
    QAction* selectedAction = menu.exec(m_eventTable->viewport()->mapToGlobal(position));
    if (selectedAction == nullptr)
    {
        return;
    }

    if (selectedAction == detailAction)
    {
        openEventDetailViewerForRow(row);
        return;
    }

    QString detailText;
    QTableWidgetItem* timeItem = m_eventTable->item(row, EventColumnTime100ns);
    if (timeItem != nullptr)
    {
        detailText = timeItem->data(kRoleDetailText).toString();
    }
    if (selectedAction == copyDetailAction)
    {
        QApplication::clipboard()->setText(detailText);
        return;
    }

    if (selectedAction == copyRowAction)
    {
        QStringList cellList;
        for (int column = 0; column < m_eventTable->columnCount(); ++column)
        {
            cellList << (m_eventTable->item(row, column) != nullptr ? m_eventTable->item(row, column)->text() : QString());
        }
        QApplication::clipboard()->setText(cellList.join(QChar(u'\t')));
    }
}

void DirectKernelCallMonitorWidget::openEventDetailViewerForRow(int rowIndex)
{
    if (m_eventTable == nullptr || rowIndex < 0 || rowIndex >= m_eventTable->rowCount())
    {
        return;
    }
    QTableWidgetItem* timeItem = m_eventTable->item(rowIndex, EventColumnTime100ns);
    if (timeItem == nullptr)
    {
        return;
    }
    const QString detailText = timeItem->data(kRoleDetailText).toString();
    monitor_text_viewer::showReadOnlyTextWindow(
        this,
        QStringLiteral("直接内核调用详情"),
        detailText,
        QStringLiteral("monitor/direct-kernel-call/%1.txt").arg(rowIndex + 1));
}

