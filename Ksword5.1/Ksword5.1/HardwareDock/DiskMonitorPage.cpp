#include "DiskMonitorPage.h"

// ============================================================
// DiskMonitorPage.cpp
// 作用：
// 1) 实现硬件 Dock 下的“硬盘监控”独立页；
// 2) 通过 Toolhelp + GetProcessIoCounters 计算每进程读写速率；
// 3) 尝试用 ETW FileIo 提供文件级“磁盘活动”，不可用时回退按进程聚合。
// ============================================================

#include "../theme.h"

#include <QAbstractItemView>
#include <QCheckBox>
#include <QDateTime>
#include <QHeaderView>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QSignalBlocker>
#include <QSplitter>
#include <QTableWidget>
#include <QTableWidgetItem>
#include <QTimer>
#include <QVBoxLayout>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstring>
#include <string>
#include <utility>

#include <TlHelp32.h>
// evntrace/evntcons/tdh 提供 ETW 实时会话和事件属性解码能力。
#include <evntrace.h>
#include <evntcons.h>
#include <tdh.h>

// 旧版 SDK 可能没有暴露全部磁盘/文件 legacy kernel flag，按 evntrace.h 兼容值兜底。
#ifndef EVENT_TRACE_FLAG_DISK_IO
#define EVENT_TRACE_FLAG_DISK_IO 0x00000100
#endif
#ifndef EVENT_TRACE_FLAG_DISK_FILE_IO
#define EVENT_TRACE_FLAG_DISK_FILE_IO 0x00000200
#endif
#ifndef EVENT_TRACE_FLAG_FILE_IO
#define EVENT_TRACE_FLAG_FILE_IO 0x02000000
#endif

#pragma comment(lib, "Advapi32.lib")
#pragma comment(lib, "Tdh.lib")

namespace
{
    constexpr double kActiveBytesPerSecondThreshold = 1.0; // kActiveBytesPerSecondThreshold：判定“活跃 IO”的最小 B/s。
    constexpr int kRefreshIntervalMs = 1000;               // kRefreshIntervalMs：页面刷新周期。
    constexpr wchar_t kDiskMonitorEtwSessionName[] = L"KswordDiskMonitorFileIo"; // kDiskMonitorEtwSessionName：ETW 会话名。
    constexpr GUID kDiskMonitorEtwSessionGuid =
        { 0x2b1f0d2a, 0x0d85, 0x4cd7, { 0xa5, 0x1e, 0xf5, 0x21, 0x5c, 0x48, 0x73, 0xe2 } };

    // ProcessColumn：
    // - 作用：定义进程级磁盘速率表列序；
    // - 调用：填充表格、设置列宽与勾选同步时复用。
    enum ProcessColumn
    {
        ProcessColumnChecked = 0,
        ProcessColumnPid,
        ProcessColumnName,
        ProcessColumnReadRate,
        ProcessColumnWriteRate,
        ProcessColumnTotalRate,
        ProcessColumnResponse,
        ProcessColumnReadOps,
        ProcessColumnWriteOps,
        ProcessColumnPath,
        ProcessColumnCount
    };

    // ActivityColumn：
    // - 作用：定义下方“磁盘活动”表列序；
    // - 调用：按用户勾选 PID 输出聚合活动。
    enum ActivityColumn
    {
        ActivityColumnPid = 0,
        ActivityColumnProcess,
        ActivityColumnFile,
        ActivityColumnReadRate,
        ActivityColumnWriteRate,
        ActivityColumnTotalRate,
        ActivityColumnResponse,
        ActivityColumnDetail,
        ActivityColumnCount
    };

    // UniqueHandle：
    // - 作用：对 Win32 HANDLE 做 RAII 封装；
    // - 处理：析构时自动 CloseHandle；
    // - 返回：valid() 表示句柄是否可用。
    class UniqueHandle final
    {
    public:
        explicit UniqueHandle(HANDLE handleValue = nullptr)
            : m_handle(handleValue)
        {
        }

        ~UniqueHandle()
        {
            reset(nullptr);
        }

        UniqueHandle(const UniqueHandle&) = delete;
        UniqueHandle& operator=(const UniqueHandle&) = delete;

        UniqueHandle(UniqueHandle&& other) noexcept
            : m_handle(other.m_handle)
        {
            other.m_handle = nullptr;
        }

        UniqueHandle& operator=(UniqueHandle&& other) noexcept
        {
            if (this == &other)
            {
                return *this;
            }
            reset(nullptr);
            m_handle = other.m_handle;
            other.m_handle = nullptr;
            return *this;
        }

        HANDLE get() const
        {
            return m_handle;
        }

        bool valid() const
        {
            return m_handle != nullptr && m_handle != INVALID_HANDLE_VALUE;
        }

        void reset(HANDLE newHandle)
        {
            if (m_handle != nullptr && m_handle != INVALID_HANDLE_VALUE)
            {
                ::CloseHandle(m_handle);
            }
            m_handle = newHandle;
        }

    private:
        HANDLE m_handle = nullptr; // m_handle：当前持有的 Win32 句柄。
    };

    // steadyTickMs：
    // - 作用：返回单调时钟毫秒值；
    // - 处理：只用于相邻采样差值，不映射真实时间；
    // - 返回：steady_clock 毫秒计数。
    std::uint64_t steadyTickMs()
    {
        const auto nowValue = std::chrono::steady_clock::now().time_since_epoch();
        return static_cast<std::uint64_t>(
            std::chrono::duration_cast<std::chrono::milliseconds>(nowValue).count());
    }

    // fileTimeToUInt64：
    // - 作用：把 FILETIME 转成 100ns 计数；
    // - 处理：高低位拼接；
    // - 返回：64 位 FILETIME 数值。
    std::uint64_t fileTimeToUInt64(const FILETIME& fileTimeValue)
    {
        return (static_cast<std::uint64_t>(fileTimeValue.dwHighDateTime) << 32U)
            | static_cast<std::uint64_t>(fileTimeValue.dwLowDateTime);
    }

    // queryProcessPath：
    // - 作用：通过已打开进程句柄读取镜像路径；
    // - 处理：优先 QueryFullProcessImageNameW；
    // - 返回：成功时返回完整路径，失败时返回空字符串。
    QString queryProcessPath(const HANDLE processHandle)
    {
        if (processHandle == nullptr || processHandle == INVALID_HANDLE_VALUE)
        {
            return QString();
        }

        std::vector<wchar_t> pathBuffer(32768, L'\0');
        DWORD pathLength = static_cast<DWORD>(pathBuffer.size());
        if (::QueryFullProcessImageNameW(processHandle, 0, pathBuffer.data(), &pathLength) == FALSE)
        {
            return QString();
        }
        if (pathLength == 0)
        {
            return QString();
        }
        return QString::fromWCharArray(pathBuffer.data(), static_cast<int>(pathLength));
    }

    // queryProcessCreateTime：
    // - 作用：读取进程创建时间，用于识别 PID 复用；
    // - 处理：调用 GetProcessTimes，只关心 creationTime；
    // - 返回：成功时返回 FILETIME 100ns，失败时返回 0。
    std::uint64_t queryProcessCreateTime(const HANDLE processHandle)
    {
        if (processHandle == nullptr || processHandle == INVALID_HANDLE_VALUE)
        {
            return 0;
        }

        FILETIME createTime{};
        FILETIME exitTime{};
        FILETIME kernelTime{};
        FILETIME userTime{};
        if (::GetProcessTimes(processHandle, &createTime, &exitTime, &kernelTime, &userTime) == FALSE)
        {
            return 0;
        }
        return fileTimeToUInt64(createTime);
    }

    // isSameProcessIdentity：
    // - 作用：判断当前 PID 的历史基线是否仍属于同一进程实例；
    // - 处理：两侧创建时间都可用时必须相同，否则允许降级复用 PID 基线；
    // - 返回：true 表示可用于计算差值。
    bool isSameProcessIdentity(
        const std::uint64_t currentCreateTime100ns,
        const std::uint64_t previousCreateTime100ns)
    {
        if (currentCreateTime100ns != 0 && previousCreateTime100ns != 0)
        {
            return currentCreateTime100ns == previousCreateTime100ns;
        }
        return true;
    }

    // deltaOrZero：
    // - 作用：计算单调累计计数器的正向差值；
    // - 处理：遇到计数器回绕/重置时返回 0；
    // - 返回：currentValue - previousValue 或 0。
    std::uint64_t deltaOrZero(
        const std::uint64_t currentValue,
        const std::uint64_t previousValue)
    {
        return currentValue >= previousValue ? currentValue - previousValue : 0;
    }

    // tableHeaderStyle：
    // - 作用：生成硬盘监控表头样式；
    // - 处理：直接使用项目主题色；
    // - 返回：可赋给 QTableWidget 的样式片段。
    QString tableHeaderStyle()
    {
        return QStringLiteral(
            "QHeaderView::section{"
            "  color:%1;"
            "  background:%2;"
            "  border:1px solid %3;"
            "  padding:4px;"
            "  font-weight:600;"
            "}"
            "QTableWidget{"
            "  gridline-color:%3;"
            "  background:%4;"
            "  alternate-background-color:%2;"
            "  color:%1;"
            "}"
            "QTableWidget::item:selected{"
            "  background:%5;"
            "  color:#FFFFFF;"
            "}")
            .arg(KswordTheme::TextPrimaryHex())
            .arg(KswordTheme::SurfaceAltHex())
            .arg(KswordTheme::BorderHex())
            .arg(KswordTheme::SurfaceHex())
            .arg(KswordTheme::PrimaryBlueHex);
    }

    // trimWideString：
    // - 作用：把 ETW 属性中的 UTF-16 缓冲转成 QString；
    // - 处理：裁掉第一个 NUL 之后的填充；
    // - 返回：清理后的文本。
    QString trimWideString(const wchar_t* textPointer, const int charCount)
    {
        if (textPointer == nullptr || charCount <= 0)
        {
            return QString();
        }

        QString textValue = QString::fromWCharArray(textPointer, charCount);
        const int nullIndex = textValue.indexOf(QChar(u'\0'));
        if (nullIndex >= 0)
        {
            textValue.truncate(nullIndex);
        }
        return textValue.trimmed();
    }

    // readScalar：
    // - 作用：从 ETW 属性缓冲读取固定宽度整数；
    // - 处理：长度不足时返回 false；
    // - 返回：true 表示 valueOut 已填充。
    template <typename TValue>
    bool readScalar(const std::vector<unsigned char>& dataBuffer, TValue* valueOut)
    {
        if (valueOut == nullptr || dataBuffer.size() < sizeof(TValue))
        {
            return false;
        }

        TValue localValue{};
        std::memcpy(&localValue, dataBuffer.data(), sizeof(TValue));
        *valueOut = localValue;
        return true;
    }

    // normalizeEtwPropertyName：
    // - 作用：把 ETW 属性名标准化成小写字母数字；
    // - 处理：忽略空格、下划线等差异；
    // - 返回：可用于启发式匹配的属性名。
    QString normalizeEtwPropertyName(const QString& propertyNameText)
    {
        QString normalizedText;
        normalizedText.reserve(propertyNameText.size());
        for (const QChar ch : propertyNameText.toLower())
        {
            if (ch.isLetterOrNumber())
            {
                normalizedText.push_back(ch);
            }
        }
        return normalizedText;
    }

    // isFileObjectProperty：
    // - 作用：识别 FileObject / FileKey 之类可作为路径映射键的属性；
    // - 处理：Kernel FileIo 事件不同版本字段名略有差异；
    // - 返回：true 表示该属性可用于 m_filePathByObject。
    bool isFileObjectProperty(const QString& normalizedName)
    {
        return normalizedName == QStringLiteral("fileobject")
            || normalizedName == QStringLiteral("filekey")
            || normalizedName == QStringLiteral("fileid")
            || normalizedName == QStringLiteral("fileobj")
            || normalizedName == QStringLiteral("fileobjectkey");
    }

    // isTransferSizeProperty：
    // - 作用：识别读写事件中的字节数属性；
    // - 处理：兼容 IoSize/TransferSize/Size/Length 等字段名；
    // - 返回：true 表示可作为传输字节数。
    bool isTransferSizeProperty(const QString& normalizedName)
    {
        return normalizedName == QStringLiteral("iosize")
            || normalizedName == QStringLiteral("transfersize")
            || normalizedName == QStringLiteral("size")
            || normalizedName == QStringLiteral("length")
            || normalizedName == QStringLiteral("bytecount")
            || normalizedName == QStringLiteral("bytes");
    }

    // isDurationProperty：
    // - 作用：识别 ETW 事件中可能表示耗时的属性；
    // - 处理：不同 Provider 可能使用 ElapsedTime/Duration；
    // - 返回：true 表示可用于响应时间估算。
    bool isDurationProperty(const QString& normalizedName)
    {
        return normalizedName == QStringLiteral("duration")
            || normalizedName == QStringLiteral("elapsedtime")
            || normalizedName == QStringLiteral("responsetime")
            || normalizedName == QStringLiteral("iotime");
    }

    // isFileNameProperty：
    // - 作用：识别 ETW 文件名属性；
    // - 处理：Name/FileName/OpenPath 等都可能出现；
    // - 返回：true 表示 valueText 可作为路径候选。
    bool isFileNameProperty(const QString& normalizedName)
    {
        return normalizedName == QStringLiteral("filename")
            || normalizedName == QStringLiteral("name")
            || normalizedName == QStringLiteral("pathname")
            || normalizedName == QStringLiteral("filepath")
            || normalizedName == QStringLiteral("openpath")
            || normalizedName == QStringLiteral("filepathname");
    }

    // etwOpcodeLooksLikeRead：
    // - 作用：通过事件名与 opcode 判断是否是读事件；
    // - 处理：Kernel FileIo 读事件通常 event id/opcode 名带 Read；
    // - 返回：true 表示作为读取累计。
    bool etwOpcodeLooksLikeRead(const QString& eventNameText, const UCHAR opcodeValue)
    {
        return eventNameText.contains(QStringLiteral("Read"), Qt::CaseInsensitive)
            || opcodeValue == 67;
    }

    // etwOpcodeLooksLikeWrite：
    // - 作用：通过事件名与 opcode 判断是否是写事件；
    // - 处理：Kernel FileIo 写事件通常 event id/opcode 名带 Write；
    // - 返回：true 表示作为写入累计。
    bool etwOpcodeLooksLikeWrite(const QString& eventNameText, const UCHAR opcodeValue)
    {
        return eventNameText.contains(QStringLiteral("Write"), Qt::CaseInsensitive)
            || opcodeValue == 68;
    }

    // queryEventNameFromTdh：
    // - 作用：从 TRACE_EVENT_INFO 中提取任务名/opcode 名；
    // - 处理：优先 EventNameOffset，其次 TaskName + OpcodeName；
    // - 返回：成功时返回事件名；manifest 不含名称时返回 OpcodeN；TDH 失败时返回空字符串。
    QString queryEventNameFromTdh(
        const EVENT_RECORD* eventRecord,
        std::vector<unsigned char>* eventInfoBufferOut = nullptr)
    {
        if (eventRecord == nullptr)
        {
            return QString();
        }

        DWORD infoBufferSize = 0;
        ULONG status = ::TdhGetEventInformation(
            const_cast<EVENT_RECORD*>(eventRecord),
            0,
            nullptr,
            nullptr,
            &infoBufferSize);
        if (status != ERROR_INSUFFICIENT_BUFFER || infoBufferSize == 0)
        {
            return QString();
        }

        std::vector<unsigned char> infoBuffer(infoBufferSize, 0);
        auto* eventInfo = reinterpret_cast<PTRACE_EVENT_INFO>(infoBuffer.data());
        status = ::TdhGetEventInformation(
            const_cast<EVENT_RECORD*>(eventRecord),
            0,
            nullptr,
            eventInfo,
            &infoBufferSize);
        if (status != ERROR_SUCCESS || eventInfo == nullptr)
        {
            return QString();
        }

        QString eventNameText;
        if (eventInfo->EventNameOffset != 0)
        {
            eventNameText = QString::fromWCharArray(reinterpret_cast<const wchar_t*>(
                infoBuffer.data() + eventInfo->EventNameOffset)).trimmed();
        }
        if (eventNameText.isEmpty() && eventInfo->TaskNameOffset != 0)
        {
            eventNameText = QString::fromWCharArray(reinterpret_cast<const wchar_t*>(
                infoBuffer.data() + eventInfo->TaskNameOffset)).trimmed();
        }
        if (eventInfo->OpcodeNameOffset != 0)
        {
            const QString opcodeText = QString::fromWCharArray(reinterpret_cast<const wchar_t*>(
                infoBuffer.data() + eventInfo->OpcodeNameOffset)).trimmed();
            if (!opcodeText.isEmpty())
            {
                eventNameText = eventNameText.isEmpty()
                    ? opcodeText
                    : QStringLiteral("%1/%2").arg(eventNameText, opcodeText);
            }
        }

        if (eventInfoBufferOut != nullptr)
        {
            *eventInfoBufferOut = std::move(infoBuffer);
        }
        // manifest 不含友好名称时保留 opcode 数字，后续仍可按已知 FileIo opcode 识别读写。
        if (eventNameText.isEmpty())
        {
            eventNameText = QStringLiteral("Opcode%1")
                .arg(static_cast<unsigned int>(eventRecord->EventHeader.EventDescriptor.Opcode));
        }
        return eventNameText;
    }
}

DiskMonitorPage::DiskMonitorPage(QWidget* parent)
    : QWidget(parent)
{
    // 构造顺序：先搭建 UI，再连接事件，最后做一次初始采样并启动定时器。
    initializeUi();
    initializeConnections();
    startFileActivityEtw();
    refreshNow();

    m_refreshTimer = new QTimer(this);
    m_refreshTimer->setInterval(kRefreshIntervalMs);
    connect(m_refreshTimer, &QTimer::timeout, this, [this]()
    {
        refreshNow();
    });
    m_refreshTimer->start();
}

DiskMonitorPage::~DiskMonitorPage()
{
    if (m_refreshTimer != nullptr)
    {
        m_refreshTimer->stop();
    }
    stopFileActivityEtw(true);
}

void DiskMonitorPage::initializeUi()
{
    m_rootLayout = new QVBoxLayout(this);
    m_rootLayout->setContentsMargins(6, 6, 6, 6);
    m_rootLayout->setSpacing(6);

    QHBoxLayout* headerLayout = new QHBoxLayout();
    headerLayout->setContentsMargins(0, 0, 0, 0);
    headerLayout->setSpacing(8);

    m_titleLabel = new QLabel(QStringLiteral("硬盘监控"), this);
    m_titleLabel->setStyleSheet(
        QStringLiteral("font-size:18px;font-weight:700;color:%1;")
        .arg(KswordTheme::TextPrimaryHex()));
    headerLayout->addWidget(m_titleLabel, 0);

    m_statusLabel = new QLabel(QStringLiteral("正在建立采样基线..."), this);
    m_statusLabel->setStyleSheet(
        QStringLiteral("font-size:13px;color:%1;").arg(KswordTheme::TextSecondaryHex()));
    headerLayout->addWidget(m_statusLabel, 1);

    m_refreshButton = new QPushButton(QStringLiteral("刷新"), this);
    m_refreshButton->setToolTip(QStringLiteral("立即刷新进程磁盘 IO 计数器"));
    m_refreshButton->setStyleSheet(KswordTheme::ThemedButtonStyle());
    headerLayout->addWidget(m_refreshButton, 0);
    m_rootLayout->addLayout(headerLayout, 0);

    m_summaryLabel = new QLabel(QStringLiteral("读: 0 B/s    写: 0 B/s    勾选进程: 0"), this);
    m_summaryLabel->setStyleSheet(
        QStringLiteral("font-size:13px;font-weight:600;color:%1;")
        .arg(KswordTheme::TextPrimaryHex()));
    m_rootLayout->addWidget(m_summaryLabel, 0);

    QHBoxLayout* filterLayout = new QHBoxLayout();
    filterLayout->setContentsMargins(0, 0, 0, 0);
    filterLayout->setSpacing(8);

    m_filterEdit = new QLineEdit(this);
    m_filterEdit->setPlaceholderText(QStringLiteral("过滤进程名、PID 或路径"));
    m_filterEdit->setClearButtonEnabled(true);
    filterLayout->addWidget(m_filterEdit, 1);

    m_onlyActiveCheckBox = new QCheckBox(QStringLiteral("仅显示有磁盘 IO"), this);
    m_onlyActiveCheckBox->setToolTip(QStringLiteral("只显示当前采样周期读/写速率大于 0 的进程"));
    filterLayout->addWidget(m_onlyActiveCheckBox, 0);

    m_selectActiveButton = new QPushButton(QStringLiteral("勾选活跃进程"), this);
    m_selectActiveButton->setToolTip(QStringLiteral("勾选当前采样周期存在读写活动的进程"));
    m_selectActiveButton->setStyleSheet(KswordTheme::ThemedButtonStyle());
    filterLayout->addWidget(m_selectActiveButton, 0);

    m_clearSelectionButton = new QPushButton(QStringLiteral("清空勾选"), this);
    m_clearSelectionButton->setToolTip(QStringLiteral("清空下方磁盘活动表的 PID 过滤条件"));
    m_clearSelectionButton->setStyleSheet(KswordTheme::ThemedButtonStyle());
    filterLayout->addWidget(m_clearSelectionButton, 0);
    m_rootLayout->addLayout(filterLayout, 0);

    m_splitter = new QSplitter(Qt::Vertical, this);
    m_rootLayout->addWidget(m_splitter, 1);

    m_processTable = new QTableWidget(this);
    configureTableWidget(m_processTable);
    m_processTable->setColumnCount(ProcessColumnCount);
    m_processTable->setHorizontalHeaderLabels({
        QStringLiteral("选择"),
        QStringLiteral("PID"),
        QStringLiteral("进程"),
        QStringLiteral("读字节/s"),
        QStringLiteral("写字节/s"),
        QStringLiteral("总字节/s"),
        QStringLiteral("响应时间"),
        QStringLiteral("读次数/s"),
        QStringLiteral("写次数/s"),
        QStringLiteral("路径")
        });
    m_processTable->horizontalHeader()->setSectionResizeMode(ProcessColumnChecked, QHeaderView::ResizeToContents);
    m_processTable->horizontalHeader()->setSectionResizeMode(ProcessColumnPid, QHeaderView::ResizeToContents);
    m_processTable->horizontalHeader()->setSectionResizeMode(ProcessColumnName, QHeaderView::ResizeToContents);
    m_processTable->horizontalHeader()->setSectionResizeMode(ProcessColumnPath, QHeaderView::Stretch);
    m_splitter->addWidget(m_processTable);

    m_activityTable = new QTableWidget(this);
    configureTableWidget(m_activityTable);
    m_activityTable->setColumnCount(ActivityColumnCount);
    m_activityTable->setHorizontalHeaderLabels({
        QStringLiteral("PID"),
        QStringLiteral("进程"),
        QStringLiteral("文件"),
        QStringLiteral("读字节/s"),
        QStringLiteral("写字节/s"),
        QStringLiteral("总字节/s"),
        QStringLiteral("响应时间"),
        QStringLiteral("说明")
        });
    m_activityTable->horizontalHeader()->setSectionResizeMode(ActivityColumnPid, QHeaderView::ResizeToContents);
    m_activityTable->horizontalHeader()->setSectionResizeMode(ActivityColumnProcess, QHeaderView::ResizeToContents);
    m_activityTable->horizontalHeader()->setSectionResizeMode(ActivityColumnFile, QHeaderView::Stretch);
    m_activityTable->horizontalHeader()->setSectionResizeMode(ActivityColumnDetail, QHeaderView::Stretch);
    m_splitter->addWidget(m_activityTable);
    m_splitter->setStretchFactor(0, 2);
    m_splitter->setStretchFactor(1, 1);
    m_splitter->setSizes({ 420, 260 });
}

void DiskMonitorPage::initializeConnections()
{
    if (m_refreshButton != nullptr)
    {
        connect(m_refreshButton, &QPushButton::clicked, this, [this]()
        {
            refreshNow();
        });
    }

    if (m_filterEdit != nullptr)
    {
        connect(m_filterEdit, &QLineEdit::textChanged, this, [this]()
        {
            updateProcessTable(m_lastSampleList);
        });
    }

    if (m_onlyActiveCheckBox != nullptr)
    {
        connect(m_onlyActiveCheckBox, &QCheckBox::toggled, this, [this]()
        {
            updateProcessTable(m_lastSampleList);
        });
    }

    if (m_clearSelectionButton != nullptr)
    {
        connect(m_clearSelectionButton, &QPushButton::clicked, this, [this]()
        {
            m_selectedPidSet.clear();
            updateProcessTable(m_lastSampleList);
            updateActivityTable(m_lastSampleList);
            updateSummaryLabels(m_lastSampleList);
        });
    }

    if (m_selectActiveButton != nullptr)
    {
        connect(m_selectActiveButton, &QPushButton::clicked, this, [this]()
        {
            for (const ProcessDiskSample& sample : m_lastSampleList)
            {
                if (sample.totalBytesPerSec > kActiveBytesPerSecondThreshold)
                {
                    m_selectedPidSet.insert(sample.pid);
                }
            }
            updateProcessTable(m_lastSampleList);
            updateActivityTable(m_lastSampleList);
            updateSummaryLabels(m_lastSampleList);
        });
    }

    if (m_processTable != nullptr)
    {
        connect(m_processTable, &QTableWidget::itemChanged, this, [this](QTableWidgetItem* itemPointer)
        {
            if (itemPointer == nullptr || m_updatingProcessTable)
            {
                return;
            }
            if (itemPointer->column() != ProcessColumnChecked)
            {
                return;
            }
            syncSelectionFromTable();
            updateActivityTable(m_lastSampleList);
            updateSummaryLabels(m_lastSampleList);
        });
    }
}

void DiskMonitorPage::configureTableWidget(QTableWidget* tableWidget) const
{
    if (tableWidget == nullptr)
    {
        return;
    }

    // 表格统一按监控面板风格配置，避免用户误编辑采样结果。
    tableWidget->setAlternatingRowColors(true);
    tableWidget->setSelectionBehavior(QAbstractItemView::SelectRows);
    tableWidget->setSelectionMode(QAbstractItemView::SingleSelection);
    tableWidget->setEditTriggers(QAbstractItemView::NoEditTriggers);
    tableWidget->setSortingEnabled(true);
    tableWidget->verticalHeader()->setVisible(false);
    tableWidget->verticalHeader()->setDefaultSectionSize(24);
    tableWidget->horizontalHeader()->setStretchLastSection(false);
    tableWidget->setStyleSheet(tableHeaderStyle());
}

void DiskMonitorPage::refreshNow()
{
    std::vector<ProcessDiskSample> sampleList = collectProcessDiskSamples();
    std::sort(
        sampleList.begin(),
        sampleList.end(),
        [](const ProcessDiskSample& left, const ProcessDiskSample& right)
        {
            if (!qFuzzyCompare(left.totalBytesPerSec + 1.0, right.totalBytesPerSec + 1.0))
            {
                return left.totalBytesPerSec > right.totalBytesPerSec;
            }
            return left.pid < right.pid;
        });

    pruneStaleSelection(sampleList);
    m_lastSampleList = sampleList;
    m_lastFileActivityList = consumeFileActivitySamples(m_lastSampleList);

    updateProcessTable(m_lastSampleList);
    updateActivityTable(m_lastSampleList);
    updateSummaryLabels(m_lastSampleList);

    if (m_statusLabel != nullptr)
    {
        const std::uint32_t etwStatus = m_fileActivityEtwLastStatus.load();
        const QString etwStateText = m_fileActivityEtwRunning.load()
            ? QStringLiteral("ETW文件级")
            : QStringLiteral("PID聚合");
        m_statusLabel->setText(
            QStringLiteral("最近刷新：%1，进程数：%2，活动来源：%3%4")
            .arg(QDateTime::currentDateTime().toString(QStringLiteral("HH:mm:ss")))
            .arg(static_cast<int>(m_lastSampleList.size()))
            .arg(etwStateText)
            .arg((!m_fileActivityEtwRunning.load() && etwStatus != ERROR_SUCCESS)
                ? QStringLiteral("(%1)").arg(etwStatus)
                : QString()));
    }
}

std::vector<DiskMonitorPage::ProcessDiskSample> DiskMonitorPage::collectProcessDiskSamples()
{
    std::vector<ProcessDiskSample> sampleList;
    const std::uint64_t currentTickMs = steadyTickMs();

    UniqueHandle snapshotHandle(::CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0));
    if (!snapshotHandle.valid())
    {
        return sampleList;
    }

    PROCESSENTRY32W processEntry{};
    processEntry.dwSize = sizeof(processEntry);
    if (::Process32FirstW(snapshotHandle.get(), &processEntry) == FALSE)
    {
        return sampleList;
    }

    std::unordered_set<std::uint32_t> observedPidSet;
    do
    {
        ProcessDiskSample sample;
        sample.pid = static_cast<std::uint32_t>(processEntry.th32ProcessID);
        sample.threadCount = static_cast<std::uint32_t>(processEntry.cntThreads);
        sample.processName = QString::fromWCharArray(processEntry.szExeFile).trimmed();
        if (sample.processName.isEmpty())
        {
            sample.processName = QStringLiteral("<PID %1>").arg(sample.pid);
        }

        observedPidSet.insert(sample.pid);

        // System Idle Process / System 可能无法常规打开；失败时仍保留行，便于用户确认 PID 存在。
        UniqueHandle processHandle(::OpenProcess(
            PROCESS_QUERY_LIMITED_INFORMATION,
            FALSE,
            static_cast<DWORD>(sample.pid)));
        if (processHandle.valid())
        {
            sample.processImagePath = queryProcessPath(processHandle.get());
            const std::uint64_t createTime100ns = queryProcessCreateTime(processHandle.get());

            IO_COUNTERS ioCounters{};
            if (::GetProcessIoCounters(processHandle.get(), &ioCounters) != FALSE)
            {
                sample.rawReadBytes = static_cast<std::uint64_t>(ioCounters.ReadTransferCount);
                sample.rawWriteBytes = static_cast<std::uint64_t>(ioCounters.WriteTransferCount);
                sample.rawOtherBytes = static_cast<std::uint64_t>(ioCounters.OtherTransferCount);
                sample.rawReadOps = static_cast<std::uint64_t>(ioCounters.ReadOperationCount);
                sample.rawWriteOps = static_cast<std::uint64_t>(ioCounters.WriteOperationCount);
                sample.rawOtherOps = static_cast<std::uint64_t>(ioCounters.OtherOperationCount);
                sample.countersReady = true;

                const auto baselineIt = m_baselineByPid.find(sample.pid);
                if (baselineIt != m_baselineByPid.end()
                    && isSameProcessIdentity(createTime100ns, baselineIt->second.identityCreateTime100ns)
                    && currentTickMs > baselineIt->second.sampleTickMs)
                {
                    const double deltaSeconds =
                        static_cast<double>(currentTickMs - baselineIt->second.sampleTickMs) / 1000.0;
                    if (deltaSeconds > 0.0)
                    {
                        const std::uint64_t deltaReadBytes =
                            deltaOrZero(sample.rawReadBytes, baselineIt->second.rawReadBytes);
                        const std::uint64_t deltaWriteBytes =
                            deltaOrZero(sample.rawWriteBytes, baselineIt->second.rawWriteBytes);
                        const std::uint64_t deltaOtherBytes =
                            deltaOrZero(sample.rawOtherBytes, baselineIt->second.rawOtherBytes);
                        const std::uint64_t deltaReadOps =
                            deltaOrZero(sample.rawReadOps, baselineIt->second.rawReadOps);
                        const std::uint64_t deltaWriteOps =
                            deltaOrZero(sample.rawWriteOps, baselineIt->second.rawWriteOps);
                        const std::uint64_t deltaOtherOps =
                            deltaOrZero(sample.rawOtherOps, baselineIt->second.rawOtherOps);

                        sample.readBytesPerSec = static_cast<double>(deltaReadBytes) / deltaSeconds;
                        sample.writeBytesPerSec = static_cast<double>(deltaWriteBytes) / deltaSeconds;
                        sample.otherBytesPerSec = static_cast<double>(deltaOtherBytes) / deltaSeconds;
                        sample.totalBytesPerSec = sample.readBytesPerSec + sample.writeBytesPerSec;
                        sample.readOpsPerSec = static_cast<double>(deltaReadOps) / deltaSeconds;
                        sample.writeOpsPerSec = static_cast<double>(deltaWriteOps) / deltaSeconds;

                        const std::uint64_t transferOps = deltaReadOps + deltaWriteOps;
                        const std::uint64_t transferBytes = deltaReadBytes + deltaWriteBytes;
                        if (transferOps > 0)
                        {
                            // 说明：用户态 IO_COUNTERS 没有真实完成耗时字段。
                            // 这里用 1 秒窗口内活跃请求的平均间隔作为轻量估算，只用于排序和趋势提示。
                            sample.responseTimeMs = (deltaSeconds * 1000.0) / static_cast<double>(transferOps);
                            if (transferBytes == 0 && deltaOtherOps > 0)
                            {
                                sample.responseTimeMs =
                                    (deltaSeconds * 1000.0) / static_cast<double>(deltaOtherOps);
                            }
                        }
                        else if (deltaOtherOps > 0)
                        {
                            sample.responseTimeMs =
                                (deltaSeconds * 1000.0) / static_cast<double>(deltaOtherOps);
                        }
                        sample.rateReady = true;
                    }
                }

                ProcessDiskBaseline baseline;
                baseline.identityCreateTime100ns = createTime100ns;
                baseline.sampleTickMs = currentTickMs;
                baseline.rawReadBytes = sample.rawReadBytes;
                baseline.rawWriteBytes = sample.rawWriteBytes;
                baseline.rawOtherBytes = sample.rawOtherBytes;
                baseline.rawReadOps = sample.rawReadOps;
                baseline.rawWriteOps = sample.rawWriteOps;
                baseline.rawOtherOps = sample.rawOtherOps;
                m_baselineByPid[sample.pid] = baseline;
            }
        }

        sampleList.push_back(std::move(sample));
    } while (::Process32NextW(snapshotHandle.get(), &processEntry) != FALSE);

    // 移除已经退出进程的历史基线，避免 PID 复用时旧样本长期残留。
    for (auto baselineIt = m_baselineByPid.begin(); baselineIt != m_baselineByPid.end();)
    {
        if (observedPidSet.find(baselineIt->first) == observedPidSet.end())
        {
            baselineIt = m_baselineByPid.erase(baselineIt);
        }
        else
        {
            ++baselineIt;
        }
    }

    return sampleList;
}

std::vector<DiskMonitorPage::FileActivitySample> DiskMonitorPage::consumeFileActivitySamples(
    const std::vector<ProcessDiskSample>& sampleList)
{
    const std::uint64_t nowMs = steadyTickMs();
    double deltaSeconds = 1.0;
    if (m_lastFileActivityDrainMs != 0 && nowMs > m_lastFileActivityDrainMs)
    {
        deltaSeconds = std::max(0.001, static_cast<double>(nowMs - m_lastFileActivityDrainMs) / 1000.0);
    }
    m_lastFileActivityDrainMs = nowMs;

    std::unordered_map<std::uint32_t, QString> processNameByPid;
    processNameByPid.reserve(sampleList.size());
    for (const ProcessDiskSample& sample : sampleList)
    {
        processNameByPid[sample.pid] = sample.processName;
    }

    QHash<QString, FileActivityAccumulator> activitySnapshot;
    {
        std::lock_guard<std::mutex> lock(m_fileActivityMutex);
        activitySnapshot = m_fileActivityByKey;
        m_fileActivityByKey.clear();
    }

    std::vector<FileActivitySample> resultList;
    resultList.reserve(static_cast<std::size_t>(activitySnapshot.size()));
    for (auto activityIt = activitySnapshot.constBegin(); activityIt != activitySnapshot.constEnd(); ++activityIt)
    {
        const FileActivityAccumulator& accumulator = activityIt.value();
        if (accumulator.filePath.trimmed().isEmpty())
        {
            continue;
        }

        FileActivitySample sample;
        sample.pid = accumulator.pid;
        const auto processNameIt = processNameByPid.find(sample.pid);
        sample.processName = processNameIt != processNameByPid.end()
            ? processNameIt->second
            : QStringLiteral("<PID %1>").arg(sample.pid);
        sample.filePath = accumulator.filePath;
        sample.readBytesPerSec = static_cast<double>(accumulator.readBytes) / deltaSeconds;
        sample.writeBytesPerSec = static_cast<double>(accumulator.writeBytes) / deltaSeconds;
        sample.eventCount = accumulator.eventCount;
        if (accumulator.responseCount > 0)
        {
            sample.responseTimeMs =
                accumulator.responseMsTotal / static_cast<double>(accumulator.responseCount);
            sample.responseAvailable = true;
        }
        resultList.push_back(std::move(sample));
    }

    std::sort(
        resultList.begin(),
        resultList.end(),
        [](const FileActivitySample& left, const FileActivitySample& right)
        {
            const double leftTotal = left.readBytesPerSec + left.writeBytesPerSec;
            const double rightTotal = right.readBytesPerSec + right.writeBytesPerSec;
            if (!qFuzzyCompare(leftTotal + 1.0, rightTotal + 1.0))
            {
                return leftTotal > rightTotal;
            }
            if (left.pid != right.pid)
            {
                return left.pid < right.pid;
            }
            return left.filePath < right.filePath;
        });

    return resultList;
}

void DiskMonitorPage::pruneStaleSelection(const std::vector<ProcessDiskSample>& sampleList)
{
    std::unordered_set<std::uint32_t> alivePidSet;
    for (const ProcessDiskSample& sample : sampleList)
    {
        alivePidSet.insert(sample.pid);
    }

    for (auto selectedIt = m_selectedPidSet.begin(); selectedIt != m_selectedPidSet.end();)
    {
        if (alivePidSet.find(*selectedIt) == alivePidSet.end())
        {
            selectedIt = m_selectedPidSet.erase(selectedIt);
        }
        else
        {
            ++selectedIt;
        }
    }
}

void DiskMonitorPage::updateProcessTable(const std::vector<ProcessDiskSample>& sampleList)
{
    if (m_processTable == nullptr)
    {
        return;
    }

    QSignalBlocker tableSignalBlocker(m_processTable);
    m_updatingProcessTable = true;
    m_processTable->setSortingEnabled(false);
    m_processTable->clearContents();
    m_processTable->setRowCount(0);

    int rowIndex = 0;
    for (const ProcessDiskSample& sample : sampleList)
    {
        if (!sampleMatchesFilter(sample))
        {
            continue;
        }

        m_processTable->insertRow(rowIndex);

        QTableWidgetItem* checkItem = createReadOnlyItem(QString());
        checkItem->setFlags((checkItem->flags() | Qt::ItemIsUserCheckable) & ~Qt::ItemIsEditable);
        checkItem->setData(Qt::UserRole, static_cast<qulonglong>(sample.pid));
        applyProcessRowCheckState(checkItem, sample.pid);
        setTableItemText(m_processTable, rowIndex, ProcessColumnChecked, checkItem);

        setTableItemText(
            m_processTable,
            rowIndex,
            ProcessColumnPid,
            createNumericItem(QString::number(sample.pid), static_cast<double>(sample.pid)));
        setTableItemText(m_processTable, rowIndex, ProcessColumnName, createReadOnlyItem(sample.processName));
        setTableItemText(
            m_processTable,
            rowIndex,
            ProcessColumnReadRate,
            createNumericItem(formatBytesPerSecond(sample.readBytesPerSec), sample.readBytesPerSec));
        setTableItemText(
            m_processTable,
            rowIndex,
            ProcessColumnWriteRate,
            createNumericItem(formatBytesPerSecond(sample.writeBytesPerSec), sample.writeBytesPerSec));
        setTableItemText(
            m_processTable,
            rowIndex,
            ProcessColumnTotalRate,
            createNumericItem(formatBytesPerSecond(sample.totalBytesPerSec), sample.totalBytesPerSec));
        setTableItemText(
            m_processTable,
            rowIndex,
            ProcessColumnResponse,
            createNumericItem(formatMilliseconds(sample.responseTimeMs), sample.responseTimeMs));
        setTableItemText(
            m_processTable,
            rowIndex,
            ProcessColumnReadOps,
            createNumericItem(formatOpsPerSecond(sample.readOpsPerSec), sample.readOpsPerSec));
        setTableItemText(
            m_processTable,
            rowIndex,
            ProcessColumnWriteOps,
            createNumericItem(formatOpsPerSecond(sample.writeOpsPerSec), sample.writeOpsPerSec));
        setTableItemText(
            m_processTable,
            rowIndex,
            ProcessColumnPath,
            createReadOnlyItem(sample.processImagePath.isEmpty()
                ? QStringLiteral("<权限不足或系统进程>")
                : sample.processImagePath));

        ++rowIndex;
    }

    m_processTable->setSortingEnabled(true);
    m_updatingProcessTable = false;
}

void DiskMonitorPage::updateActivityTable(const std::vector<ProcessDiskSample>& sampleList)
{
    if (m_activityTable == nullptr)
    {
        return;
    }

    std::vector<FileActivitySample> selectedFileActivityList;
    selectedFileActivityList.reserve(m_lastFileActivityList.size());
    for (const FileActivitySample& fileActivity : m_lastFileActivityList)
    {
        if (m_selectedPidSet.find(fileActivity.pid) != m_selectedPidSet.end())
        {
            selectedFileActivityList.push_back(fileActivity);
        }
    }

    if (!selectedFileActivityList.empty())
    {
        std::sort(
            selectedFileActivityList.begin(),
            selectedFileActivityList.end(),
            [](const FileActivitySample& left, const FileActivitySample& right)
            {
                const double leftTotal = left.readBytesPerSec + left.writeBytesPerSec;
                const double rightTotal = right.readBytesPerSec + right.writeBytesPerSec;
                if (!qFuzzyCompare(leftTotal + 1.0, rightTotal + 1.0))
                {
                    return leftTotal > rightTotal;
                }
                return left.filePath < right.filePath;
            });

        m_activityTable->setSortingEnabled(false);
        m_activityTable->clearContents();
        m_activityTable->setRowCount(static_cast<int>(selectedFileActivityList.size()));

        for (int rowIndex = 0; rowIndex < static_cast<int>(selectedFileActivityList.size()); ++rowIndex)
        {
            const FileActivitySample& sample = selectedFileActivityList[static_cast<std::size_t>(rowIndex)];
            const double totalBytesPerSec = sample.readBytesPerSec + sample.writeBytesPerSec;
            const QString responseText = sample.responseAvailable
                ? formatMilliseconds(sample.responseTimeMs)
                : QStringLiteral("ETW未提供");
            const QString detailText = QStringLiteral("ETW FileIo 文件级聚合，事件数:%1")
                .arg(sample.eventCount);

            setTableItemText(
                m_activityTable,
                rowIndex,
                ActivityColumnPid,
                createNumericItem(QString::number(sample.pid), static_cast<double>(sample.pid)));
            setTableItemText(m_activityTable, rowIndex, ActivityColumnProcess, createReadOnlyItem(sample.processName));
            setTableItemText(m_activityTable, rowIndex, ActivityColumnFile, createReadOnlyItem(sample.filePath));
            setTableItemText(
                m_activityTable,
                rowIndex,
                ActivityColumnReadRate,
                createNumericItem(formatBytesPerSecond(sample.readBytesPerSec), sample.readBytesPerSec));
            setTableItemText(
                m_activityTable,
                rowIndex,
                ActivityColumnWriteRate,
                createNumericItem(formatBytesPerSecond(sample.writeBytesPerSec), sample.writeBytesPerSec));
            setTableItemText(
                m_activityTable,
                rowIndex,
                ActivityColumnTotalRate,
                createNumericItem(formatBytesPerSecond(totalBytesPerSec), totalBytesPerSec));
            setTableItemText(
                m_activityTable,
                rowIndex,
                ActivityColumnResponse,
                createNumericItem(responseText, sample.responseAvailable ? sample.responseTimeMs : 0.0));
            setTableItemText(m_activityTable, rowIndex, ActivityColumnDetail, createReadOnlyItem(detailText));
        }

        m_activityTable->setSortingEnabled(true);
        return;
    }

    std::vector<ProcessDiskSample> selectedSampleList;
    selectedSampleList.reserve(m_selectedPidSet.size());
    for (const ProcessDiskSample& sample : sampleList)
    {
        if (m_selectedPidSet.find(sample.pid) != m_selectedPidSet.end())
        {
            selectedSampleList.push_back(sample);
        }
    }

    std::sort(
        selectedSampleList.begin(),
        selectedSampleList.end(),
        [](const ProcessDiskSample& left, const ProcessDiskSample& right)
        {
            if (!qFuzzyCompare(left.totalBytesPerSec + 1.0, right.totalBytesPerSec + 1.0))
            {
                return left.totalBytesPerSec > right.totalBytesPerSec;
            }
            return left.pid < right.pid;
        });

    m_activityTable->setSortingEnabled(false);
    m_activityTable->clearContents();
    m_activityTable->setRowCount(static_cast<int>(selectedSampleList.size()));

    for (int rowIndex = 0; rowIndex < static_cast<int>(selectedSampleList.size()); ++rowIndex)
    {
        const ProcessDiskSample& sample = selectedSampleList[static_cast<std::size_t>(rowIndex)];
        const QString fileText = sample.processImagePath.isEmpty()
            ? QStringLiteral("<按进程聚合；文件路径需 ETW FileIo 增强>")
            : sample.processImagePath;
        const QString detailText = sample.rateReady
            ? QStringLiteral("线程:%1  其它IO:%2  当前为 PID 聚合视图；物理磁盘落盘需 ETW 校准")
                .arg(sample.threadCount)
                .arg(formatBytesPerSecond(sample.otherBytesPerSec))
            : QStringLiteral("等待下一轮采样建立速率基线");

        setTableItemText(
            m_activityTable,
            rowIndex,
            ActivityColumnPid,
            createNumericItem(QString::number(sample.pid), static_cast<double>(sample.pid)));
        setTableItemText(m_activityTable, rowIndex, ActivityColumnProcess, createReadOnlyItem(sample.processName));
        setTableItemText(m_activityTable, rowIndex, ActivityColumnFile, createReadOnlyItem(fileText));
        setTableItemText(
            m_activityTable,
            rowIndex,
            ActivityColumnReadRate,
            createNumericItem(formatBytesPerSecond(sample.readBytesPerSec), sample.readBytesPerSec));
        setTableItemText(
            m_activityTable,
            rowIndex,
            ActivityColumnWriteRate,
            createNumericItem(formatBytesPerSecond(sample.writeBytesPerSec), sample.writeBytesPerSec));
        setTableItemText(
            m_activityTable,
            rowIndex,
            ActivityColumnTotalRate,
            createNumericItem(formatBytesPerSecond(sample.totalBytesPerSec), sample.totalBytesPerSec));
        setTableItemText(
            m_activityTable,
            rowIndex,
            ActivityColumnResponse,
            createNumericItem(formatMilliseconds(sample.responseTimeMs), sample.responseTimeMs));
        setTableItemText(m_activityTable, rowIndex, ActivityColumnDetail, createReadOnlyItem(detailText));
    }

    m_activityTable->setSortingEnabled(true);
}

void DiskMonitorPage::updateSummaryLabels(const std::vector<ProcessDiskSample>& sampleList)
{
    double totalReadBytesPerSec = 0.0;
    double totalWriteBytesPerSec = 0.0;
    int activeProcessCount = 0;
    for (const ProcessDiskSample& sample : sampleList)
    {
        totalReadBytesPerSec += sample.readBytesPerSec;
        totalWriteBytesPerSec += sample.writeBytesPerSec;
        if (sample.totalBytesPerSec > kActiveBytesPerSecondThreshold)
        {
            ++activeProcessCount;
        }
    }

    if (m_summaryLabel != nullptr)
    {
        const QString modeText = m_fileActivityEtwRunning.load()
            ? QStringLiteral("ETW文件级")
            : QStringLiteral("按 PID 聚合");
        m_summaryLabel->setText(
            QStringLiteral("读: %1    写: %2    活跃进程: %3    勾选进程: %4    模式: %5")
            .arg(formatBytesPerSecond(totalReadBytesPerSec))
            .arg(formatBytesPerSecond(totalWriteBytesPerSec))
            .arg(activeProcessCount)
            .arg(static_cast<int>(m_selectedPidSet.size()))
            .arg(modeText));
    }
}

void DiskMonitorPage::syncSelectionFromTable()
{
    if (m_processTable == nullptr)
    {
        return;
    }

    for (int rowIndex = 0; rowIndex < m_processTable->rowCount(); ++rowIndex)
    {
        QTableWidgetItem* checkItem = m_processTable->item(rowIndex, ProcessColumnChecked);
        if (checkItem == nullptr)
        {
            continue;
        }

        const std::uint32_t pid =
            static_cast<std::uint32_t>(checkItem->data(Qt::UserRole).toULongLong());
        if (pid == 0 && checkItem->data(Qt::UserRole).toULongLong() == 0ULL)
        {
            // PID 0 是合法系统进程，不能直接跳过；这里仅保留显式注释说明。
        }

        if (checkItem->checkState() == Qt::Checked)
        {
            m_selectedPidSet.insert(pid);
        }
        else
        {
            m_selectedPidSet.erase(pid);
        }
    }
}

void DiskMonitorPage::startFileActivityEtw()
{
    if (m_fileActivityEtwThread != nullptr && m_fileActivityEtwThread->joinable())
    {
        return;
    }

    m_fileActivityEtwStopRequested.store(false);
    m_fileActivityEtwLastStatus.store(ERROR_SUCCESS);

    m_fileActivityEtwThread = std::make_unique<std::thread>([this]()
    {
        const std::wstring sessionNameWide(kDiskMonitorEtwSessionName);
        const ULONG traceNameBytes =
            static_cast<ULONG>((sessionNameWide.size() + 1) * sizeof(wchar_t));
        const ULONG propertyBufferSize =
            static_cast<ULONG>(sizeof(EVENT_TRACE_PROPERTIES) + traceNameBytes);
        std::vector<unsigned char> propertyBuffer(propertyBufferSize, 0);
        auto* properties = reinterpret_cast<EVENT_TRACE_PROPERTIES*>(propertyBuffer.data());
        properties->Wnode.BufferSize = propertyBufferSize;
        properties->Wnode.ClientContext = 2;
        properties->Wnode.Flags = WNODE_FLAG_TRACED_GUID;
        // 使用私有 GUID + SYSTEM_LOGGER_MODE，避免和全局 SystemTraceControlGuid 会话互相抢占。
        properties->Wnode.Guid = kDiskMonitorEtwSessionGuid;
        properties->LogFileMode = EVENT_TRACE_REAL_TIME_MODE | EVENT_TRACE_SYSTEM_LOGGER_MODE;
        properties->LoggerNameOffset = sizeof(EVENT_TRACE_PROPERTIES);
        properties->EnableFlags =
            EVENT_TRACE_FLAG_FILE_IO | EVENT_TRACE_FLAG_DISK_FILE_IO | EVENT_TRACE_FLAG_DISK_IO;
        properties->FlushTimer = 1;
        properties->BufferSize = 256;
        properties->MinimumBuffers = 16;
        properties->MaximumBuffers = 64;

        wchar_t* loggerNamePointer = reinterpret_cast<wchar_t*>(
            propertyBuffer.data() + properties->LoggerNameOffset);
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
            m_fileActivityEtwLastStatus.store(startStatus);
            m_fileActivityEtwRunning.store(false);
            return;
        }

        m_fileActivityEtwSessionHandle.store(static_cast<std::uint64_t>(sessionHandle));
        m_fileActivityEtwRunning.store(true);

        EVENT_TRACE_LOGFILEW traceLogFile{};
        traceLogFile.LoggerName = loggerNamePointer;
        traceLogFile.ProcessTraceMode = PROCESS_TRACE_MODE_REAL_TIME | PROCESS_TRACE_MODE_EVENT_RECORD;
        traceLogFile.EventRecordCallback = &DiskMonitorPage::fileActivityEtwCallback;
        traceLogFile.Context = this;

        TRACEHANDLE traceHandle = ::OpenTraceW(&traceLogFile);
        if (traceHandle == INVALID_PROCESSTRACE_HANDLE)
        {
            const ULONG lastError = ::GetLastError();
            m_fileActivityEtwLastStatus.store(lastError);
            m_fileActivityEtwRunning.store(false);
            m_fileActivityEtwSessionHandle.store(0);
            ::ControlTraceW(sessionHandle, loggerNamePointer, properties, EVENT_TRACE_CONTROL_STOP);
            return;
        }

        m_fileActivityEtwTraceHandle.store(static_cast<std::uint64_t>(traceHandle));
        const ULONG processStatus = ::ProcessTrace(&traceHandle, 1, nullptr, nullptr);
        m_fileActivityEtwLastStatus.store(processStatus);

        const std::uint64_t ownedTraceHandle = m_fileActivityEtwTraceHandle.exchange(0);
        if (ownedTraceHandle != 0)
        {
            ::CloseTrace(static_cast<TRACEHANDLE>(ownedTraceHandle));
        }

        const std::uint64_t ownedSessionHandle = m_fileActivityEtwSessionHandle.exchange(0);
        if (ownedSessionHandle != 0)
        {
            ::ControlTraceW(
                static_cast<TRACEHANDLE>(ownedSessionHandle),
                loggerNamePointer,
                properties,
                EVENT_TRACE_CONTROL_STOP);
        }

        m_fileActivityEtwRunning.store(false);
    });
}

void DiskMonitorPage::stopFileActivityEtw(const bool waitForThread)
{
    m_fileActivityEtwStopRequested.store(true);

    const std::uint64_t ownedTraceHandle = m_fileActivityEtwTraceHandle.exchange(0);
    if (ownedTraceHandle != 0)
    {
        ::CloseTrace(static_cast<TRACEHANDLE>(ownedTraceHandle));
    }

    const std::uint64_t ownedSessionHandle = m_fileActivityEtwSessionHandle.exchange(0);
    if (ownedSessionHandle != 0)
    {
        const std::wstring sessionNameWide(kDiskMonitorEtwSessionName);
        std::vector<unsigned char> propertyBuffer(
            sizeof(EVENT_TRACE_PROPERTIES) + (sessionNameWide.size() + 1) * sizeof(wchar_t),
            0);
        auto* properties = reinterpret_cast<EVENT_TRACE_PROPERTIES*>(propertyBuffer.data());
        properties->Wnode.BufferSize = static_cast<ULONG>(propertyBuffer.size());
        properties->LoggerNameOffset = sizeof(EVENT_TRACE_PROPERTIES);
        wchar_t* loggerNamePointer = reinterpret_cast<wchar_t*>(
            propertyBuffer.data() + properties->LoggerNameOffset);
        ::wcscpy_s(loggerNamePointer, sessionNameWide.size() + 1, sessionNameWide.c_str());
        ::ControlTraceW(
            static_cast<TRACEHANDLE>(ownedSessionHandle),
            loggerNamePointer,
            properties,
            EVENT_TRACE_CONTROL_STOP);
    }

    if (m_fileActivityEtwThread == nullptr || !m_fileActivityEtwThread->joinable())
    {
        m_fileActivityEtwThread.reset();
        m_fileActivityEtwRunning.store(false);
        return;
    }

    if (waitForThread)
    {
        m_fileActivityEtwThread->join();
        m_fileActivityEtwThread.reset();
        m_fileActivityEtwRunning.store(false);
        return;
    }

    std::unique_ptr<std::thread> joinThread = std::move(m_fileActivityEtwThread);
    std::thread([joinThread = std::move(joinThread)]() mutable
    {
        if (joinThread != nullptr && joinThread->joinable())
        {
            joinThread->join();
        }
    }).detach();
}

void WINAPI DiskMonitorPage::fileActivityEtwCallback(struct _EVENT_RECORD* eventRecordPointer)
{
    if (eventRecordPointer == nullptr || eventRecordPointer->UserContext == nullptr)
    {
        return;
    }

    auto* pagePointer = reinterpret_cast<DiskMonitorPage*>(eventRecordPointer->UserContext);
    pagePointer->handleFileActivityEtwEvent(eventRecordPointer);
}

void DiskMonitorPage::handleFileActivityEtwEvent(const struct _EVENT_RECORD* eventRecordPointer)
{
    const EVENT_RECORD* eventRecord = reinterpret_cast<const EVENT_RECORD*>(eventRecordPointer);
    if (eventRecord == nullptr || m_fileActivityEtwStopRequested.load())
    {
        return;
    }

    std::vector<unsigned char> eventInfoBuffer;
    const QString eventNameText = queryEventNameFromTdh(eventRecord, &eventInfoBuffer);
    if (eventInfoBuffer.empty())
    {
        return;
    }

    const bool isReadEvent = etwOpcodeLooksLikeRead(
        eventNameText,
        eventRecord->EventHeader.EventDescriptor.Opcode);
    const bool isWriteEvent = etwOpcodeLooksLikeWrite(
        eventNameText,
        eventRecord->EventHeader.EventDescriptor.Opcode);
    const bool isNameEvent =
        eventNameText.contains(QStringLiteral("Name"), Qt::CaseInsensitive)
        || eventNameText.contains(QStringLiteral("Create"), Qt::CaseInsensitive)
        || eventNameText.contains(QStringLiteral("File"), Qt::CaseInsensitive);
    if (!isReadEvent && !isWriteEvent && !isNameEvent)
    {
        return;
    }

    auto* eventInfo = reinterpret_cast<PTRACE_EVENT_INFO>(eventInfoBuffer.data());
    std::uint64_t fileObjectValue = 0;
    std::uint64_t transferSize = 0;
    std::uint64_t durationValue = 0;
    QString filePathText;

    for (ULONG indexValue = 0; indexValue < eventInfo->TopLevelPropertyCount; ++indexValue)
    {
        const EVENT_PROPERTY_INFO& propertyInfo = eventInfo->EventPropertyInfoArray[indexValue];
        if ((propertyInfo.Flags & PropertyStruct) != 0)
        {
            continue;
        }

        const wchar_t* propertyNamePointer = reinterpret_cast<const wchar_t*>(
            eventInfoBuffer.data() + propertyInfo.NameOffset);
        const QString propertyNameText = propertyNamePointer != nullptr
            ? QString::fromWCharArray(propertyNamePointer)
            : QString();
        const QString normalizedName = normalizeEtwPropertyName(propertyNameText);

        PROPERTY_DATA_DESCRIPTOR descriptor{};
        descriptor.PropertyName = reinterpret_cast<ULONGLONG>(propertyNamePointer);
        descriptor.ArrayIndex = ULONG_MAX;

        ULONG propertySize = 0;
        ULONG status = ::TdhGetPropertySize(
            const_cast<EVENT_RECORD*>(eventRecord),
            0,
            nullptr,
            1,
            &descriptor,
            &propertySize);
        if (status != ERROR_SUCCESS || propertySize == 0)
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

        const USHORT inTypeValue = propertyInfo.nonStructType.InType;
        if (inTypeValue == TDH_INTYPE_UNICODESTRING)
        {
            const QString stringValue = trimWideString(
                reinterpret_cast<const wchar_t*>(propertyBuffer.data()),
                static_cast<int>(propertyBuffer.size() / sizeof(wchar_t)));
            if (isFileNameProperty(normalizedName)
                || stringValue.startsWith(QStringLiteral("\\"))
                || stringValue.contains(QStringLiteral(":\\")))
            {
                filePathText = stringValue;
            }
            continue;
        }

        std::uint64_t numericValue = 0;
        if (inTypeValue == TDH_INTYPE_POINTER
            || inTypeValue == TDH_INTYPE_HEXINT64
            || inTypeValue == TDH_INTYPE_UINT64)
        {
            readScalar(propertyBuffer, &numericValue);
        }
        else if (inTypeValue == TDH_INTYPE_UINT32 || inTypeValue == TDH_INTYPE_HEXINT32)
        {
            std::uint32_t value32 = 0;
            if (readScalar(propertyBuffer, &value32))
            {
                numericValue = value32;
            }
        }
        else if (inTypeValue == TDH_INTYPE_UINT16)
        {
            std::uint16_t value16 = 0;
            if (readScalar(propertyBuffer, &value16))
            {
                numericValue = value16;
            }
        }

        if (numericValue == 0)
        {
            continue;
        }
        if (isFileObjectProperty(normalizedName))
        {
            fileObjectValue = numericValue;
        }
        else if (isTransferSizeProperty(normalizedName))
        {
            transferSize = numericValue;
        }
        else if (isDurationProperty(normalizedName))
        {
            durationValue = numericValue;
        }
    }

    std::lock_guard<std::mutex> lock(m_fileActivityMutex);
    if (!filePathText.isEmpty() && fileObjectValue != 0)
    {
        m_filePathByObject[fileObjectValue] = filePathText;
        if (m_filePathByObject.size() > 65536)
        {
            // 文件对象映射只是辅助把后续 Read/Write 关联到路径，过大时清理可接受。
            m_filePathByObject.clear();
            m_filePathByObject[fileObjectValue] = filePathText;
        }
    }
    if (filePathText.isEmpty() && fileObjectValue != 0)
    {
        const auto pathIt = m_filePathByObject.find(fileObjectValue);
        if (pathIt != m_filePathByObject.end())
        {
            filePathText = pathIt->second;
        }
    }
    if (filePathText.isEmpty() || (!isReadEvent && !isWriteEvent))
    {
        return;
    }

    const std::uint32_t pid = static_cast<std::uint32_t>(eventRecord->EventHeader.ProcessId);
    const QString keyText = QStringLiteral("%1|%2").arg(pid).arg(filePathText.toLower());
    FileActivityAccumulator& accumulator = m_fileActivityByKey[keyText];
    accumulator.pid = pid;
    accumulator.filePath = filePathText;
    if (isReadEvent)
    {
        accumulator.readBytes += transferSize;
    }
    if (isWriteEvent)
    {
        accumulator.writeBytes += transferSize;
    }
    if (durationValue > 0)
    {
        // 多数内核 ETW 时间字段按 100ns 表示；若系统版本返回其它单位，仍仅作为近似展示。
        accumulator.responseMsTotal += static_cast<double>(durationValue) / 10000.0;
        ++accumulator.responseCount;
    }
    ++accumulator.eventCount;
}

QTableWidgetItem* DiskMonitorPage::createReadOnlyItem(const QString& text) const
{
    QTableWidgetItem* itemPointer = new QTableWidgetItem(text);
    itemPointer->setFlags(itemPointer->flags() & ~Qt::ItemIsEditable);
    itemPointer->setToolTip(text);
    return itemPointer;
}

QTableWidgetItem* DiskMonitorPage::createNumericItem(
    const QString& text,
    const double numericValue) const
{
    QTableWidgetItem* itemPointer = createReadOnlyItem(text);
    itemPointer->setData(Qt::UserRole, numericValue);
    itemPointer->setTextAlignment(Qt::AlignRight | Qt::AlignVCenter);
    return itemPointer;
}

void DiskMonitorPage::setTableItemText(
    QTableWidget* tableWidget,
    const int rowIndex,
    const int columnIndex,
    QTableWidgetItem* itemPointer) const
{
    if (tableWidget == nullptr || itemPointer == nullptr)
    {
        delete itemPointer;
        return;
    }

    tableWidget->setItem(rowIndex, columnIndex, itemPointer);
}

void DiskMonitorPage::applyProcessRowCheckState(
    QTableWidgetItem* checkItem,
    const std::uint32_t pid) const
{
    if (checkItem == nullptr)
    {
        return;
    }

    checkItem->setCheckState(
        m_selectedPidSet.find(pid) != m_selectedPidSet.end()
        ? Qt::Checked
        : Qt::Unchecked);
}

QString DiskMonitorPage::processSearchText(const ProcessDiskSample& sample) const
{
    return QStringLiteral("%1 %2 %3")
        .arg(sample.pid)
        .arg(sample.processName)
        .arg(sample.processImagePath)
        .toLower();
}

bool DiskMonitorPage::sampleMatchesFilter(const ProcessDiskSample& sample) const
{
    if (m_onlyActiveCheckBox != nullptr
        && m_onlyActiveCheckBox->isChecked()
        && sample.totalBytesPerSec <= kActiveBytesPerSecondThreshold)
    {
        return false;
    }

    const QString filterText = m_filterEdit != nullptr
        ? m_filterEdit->text().trimmed().toLower()
        : QString();
    if (filterText.isEmpty())
    {
        return true;
    }

    return processSearchText(sample).contains(filterText);
}

QString DiskMonitorPage::formatBytesPerSecond(const double bytesPerSecond) const
{
    return QStringLiteral("%1/s").arg(formatBytes(bytesPerSecond));
}

QString DiskMonitorPage::formatBytes(const double bytesValue) const
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

QString DiskMonitorPage::formatOpsPerSecond(const double opsPerSecond) const
{
    const double safeValue = std::max(0.0, opsPerSecond);
    if (safeValue < 10.0)
    {
        return QStringLiteral("%1").arg(safeValue, 0, 'f', 2);
    }
    if (safeValue < 1000.0)
    {
        return QStringLiteral("%1").arg(safeValue, 0, 'f', 1);
    }
    return QStringLiteral("%1").arg(safeValue, 0, 'f', 0);
}

QString DiskMonitorPage::formatMilliseconds(const double milliseconds) const
{
    if (milliseconds <= 0.0 || !std::isfinite(milliseconds))
    {
        return QStringLiteral("N/A");
    }
    if (milliseconds < 1.0)
    {
        return QStringLiteral("%1 ms").arg(milliseconds, 0, 'f', 3);
    }
    if (milliseconds < 100.0)
    {
        return QStringLiteral("%1 ms").arg(milliseconds, 0, 'f', 2);
    }
    return QStringLiteral("%1 ms").arg(milliseconds, 0, 'f', 1);
}
