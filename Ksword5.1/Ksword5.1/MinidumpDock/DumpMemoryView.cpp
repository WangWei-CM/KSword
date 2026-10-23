// ============================================================
// DumpMemoryView.cpp
// 作用：实现转储虚拟内存查看器的安全地址解析、重开文件读取与分页。
// ============================================================

#include "DumpMemoryView.h"
#include "../UI/AsyncOperation.h"
#include "../UI/PrimaryPageStyle.h"
#include "../UI/FlatButtonTheme.h"
#include "../UI/ToolbarMetrics.h"
#include "../UI/StructuredFieldView.h"

#include "Internationalization/LanguageManager.h"
#include "MinidumpFormat.h"
#include "UI/MemoryWorkbench/HexView.h"
#include "UI/MemoryWorkbench/SnapshotWorkbenchWidget.h"
#include "theme.h"

#include <QApplication>
#include <QClipboard>
#include <QComboBox>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QEvent>
#include <QSignalBlocker>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QPushButton>
#include <QPointer>
#include <QRegularExpression>
#include <QTextBrowser>
#include <QVBoxLayout>

#include <algorithm>
#include <limits>
#include <memory>
#include <utility>


// 只读来源与请求只持自包含值；worker 不借用解析器映射或页面成员。
struct DumpMemorySource
{
    QString filePath;
    std::uint64_t fileSize = 0;
    std::int64_t modifiedUtcMs = -1;
    std::uint64_t faultingAddress = 0;
    std::uint32_t pointerSize = 8;
    std::vector<ks::minidump::DumpMemoryRange> ranges;
    std::vector<ks::minidump::ModuleEntry> modules;
    std::vector<ks::minidump::MemoryRegionEntry> regions;
};

enum class DumpMemoryReadStatus
{
    Success, Canceled, NotCaptured, NoFile, FileChanged, OffsetInvalid, OpenFailed, ReadFailed
};

struct DumpMemoryReadResult
{
    DumpMemoryReadStatus status = DumpMemoryReadStatus::ReadFailed;
    std::uint64_t sourceGeneration = 0;
    std::uint64_t address = 0;
    std::uint64_t fileOffset = 0;
    int rangeIndex = -1;
    std::size_t moduleIndex = static_cast<std::size_t>(-1);
    std::size_t regionIndex = static_cast<std::size_t>(-1);
    QByteArray bytes;
    QString errorText;
};

namespace
{
    // kMaximumReadBytes：单次读取硬上限，避免用户输入或畸形目录触发大分配。
    constexpr std::uint64_t kMaximumReadBytes = 64ull * 1024ull;
    // kInitialContentProbeBytes：首次打开时每个范围的只读抽样窗口。它只用于避免
    // 默认页恰好是全零块，不改变用户按地址查看任何已捕获范围的能力。
    constexpr std::uint64_t kInitialContentProbeBytes = 512;


    // Begin dump memory read value helpers: no QObject, translation or file I/O.
    struct DumpMemoryReadPlan
    {
        DumpMemoryReadStatus status = DumpMemoryReadStatus::OffsetInvalid;
        int rangeIndex = -1;
        std::uint64_t fileOffset = 0;
        std::uint64_t bytes = 0;
    };

    int findDumpRange(const DumpMemorySource& source, const std::uint64_t address)
    {
        const auto upper = std::upper_bound(source.ranges.begin(), source.ranges.end(), address,
            [](const std::uint64_t value, const ks::minidump::DumpMemoryRange& range)
            { return value < range.virtualAddress; });
        const int upperIndex = static_cast<int>(upper - source.ranges.begin());
        constexpr int kOverlapProbeDepth = 8;
        for (int index = upperIndex - 1, probes = 0;
             index >= 0 && probes < kOverlapProbeDepth; --index, ++probes)
        {
            const auto& range = source.ranges[static_cast<std::size_t>(index)];
            if (address >= range.virtualAddress && address - range.virtualAddress < range.bytes)
                return index;
        }
        return -1;
    }

    DumpMemoryReadPlan planDumpRead(const DumpMemorySource& source,
        const std::uint64_t address, const std::uint64_t selectedBytes)
    {
        DumpMemoryReadPlan plan;
        plan.rangeIndex = findDumpRange(source, address);
        if (plan.rangeIndex < 0)
        {
            plan.status = DumpMemoryReadStatus::NotCaptured;
            return plan;
        }
        if (source.filePath.isEmpty())
        {
            plan.status = DumpMemoryReadStatus::NoFile;
            return plan;
        }
        const auto& range = source.ranges[static_cast<std::size_t>(plan.rangeIndex)];
        const auto offsetInRange = address - range.virtualAddress;
        plan.bytes = std::min(range.bytes - offsetInRange,
            std::min(selectedBytes, kMaximumReadBytes));
        if (plan.bytes == 0 || range.fileOffset > std::numeric_limits<std::uint64_t>::max() - offsetInRange)
            return plan;
        plan.fileOffset = range.fileOffset + offsetInRange;
        if (plan.fileOffset > static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max())
            || plan.fileOffset > source.fileSize || plan.bytes > source.fileSize - plan.fileOffset)
            return plan;
        plan.status = DumpMemoryReadStatus::Success;
        return plan;
    }

    template<class Probe, class Canceled>
    std::uint64_t preferredDumpAddress(const DumpMemorySource& source, Probe probe, Canceled canceled)
    {
        if (source.ranges.empty()) return 0;
        if (source.faultingAddress != 0 && findDumpRange(source, source.faultingAddress) >= 0)
            return source.faultingAddress;
        for (const auto& range : source.ranges)
        {
            if (canceled()) break;
            const auto probeBytes = std::min(range.bytes, kInitialContentProbeBytes);
            if (probeBytes == 0
                || range.fileOffset > static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max())
                || range.fileOffset > source.fileSize || probeBytes > source.fileSize - range.fileOffset)
                continue;
            if (probe(range.fileOffset, probeBytes)) return range.virtualAddress;
        }
        return source.ranges.front().virtualAddress;
    }
    // End dump memory read value helpers.

    // renderResult 的语言重入只重译同一规范来源，不能重置用户页或再次读盘。
    // 精确比较查看器使用的元数据，不能仅按路径/大小把新映射误认成旧结果。
    bool sameDumpMemorySource(const DumpMemorySource& source,
        const ks::minidump::DumpParseResult& result)
    {
        return source.filePath == result.filePath && source.fileSize == result.fileSize
            && source.modifiedUtcMs == result.fileLastModifiedUtcMs
            && source.faultingAddress == result.faultingAddress && source.pointerSize == result.pointerSize
            && source.ranges.size() == result.capturedMemoryRanges.size()
            && std::equal(source.ranges.begin(), source.ranges.end(), result.capturedMemoryRanges.begin(),
                [](const auto& left, const auto& right) {
                    return left.virtualAddress == right.virtualAddress && left.fileOffset == right.fileOffset
                        && left.bytes == right.bytes && left.source == right.source;
                })
            && source.modules.size() == result.modules.size()
            && std::equal(source.modules.begin(), source.modules.end(), result.modules.begin(),
                [](const auto& left, const auto& right) {
                    return left.name == right.name && left.base == right.base && left.size == right.size;
                })
            && source.regions.size() == result.memoryRegions.size()
            && std::equal(source.regions.begin(), source.regions.end(), result.memoryRegions.begin(),
                [](const auto& left, const auto& right) {
                    return left.base == right.base && left.size == right.size && left.state == right.state
                        && left.protect == right.protect && left.type == right.type && left.source == right.source;
                });
    }

    struct DumpMemoryReadRequest
    {
        std::shared_ptr<const DumpMemorySource> source;
        std::uint64_t sourceGeneration = 0;
        std::uint64_t address = 0;
        std::uint64_t bytes = 0;
        bool chooseInitialAddress = false;
    };

    bool dumpFileIdentityMatches(const DumpMemorySource& source)
    {
        const QFileInfo info(source.filePath);
        return info.exists() && info.isFile() && info.size() >= 0
            && static_cast<std::uint64_t>(info.size()) == source.fileSize
            && (source.modifiedUtcMs < 0
                || info.lastModified().toUTC().toMSecsSinceEpoch() == source.modifiedUtcMs);
    }

    DumpMemoryReadResult readDumpMemory(const DumpMemoryReadRequest& request,
        const ks::ui::AsyncOperationToken& token)
    {
        DumpMemoryReadResult result;
        result.sourceGeneration = request.sourceGeneration;
        result.address = request.address;
        const auto canceled = [&token]() { return token.isCanceled(); };
        if (canceled()) { result.status = DumpMemoryReadStatus::Canceled; return result; }
        if (!request.source) { result.status = DumpMemoryReadStatus::NoFile; return result; }
        const auto& source = *request.source;
        // 手动地址先保留原来的范围错误优先级；默认地址在同一个后台任务里探测。
        auto plan = planDumpRead(source, result.address, request.bytes);
        if (!request.chooseInitialAddress && plan.status != DumpMemoryReadStatus::Success)
        {
            result.status = plan.status;
            return result;
        }
        if (source.filePath.isEmpty()) { result.status = DumpMemoryReadStatus::NoFile; return result; }
        if (!dumpFileIdentityMatches(source)) { result.status = DumpMemoryReadStatus::FileChanged; return result; }
        if (canceled()) { result.status = DumpMemoryReadStatus::Canceled; return result; }
        QFile file(source.filePath);
        if (!file.open(QIODevice::ReadOnly))
        {
            result.status = DumpMemoryReadStatus::OpenFailed;
            result.errorText = file.errorString();
            return result;
        }
        if (request.chooseInitialAddress)
        {
            result.address = preferredDumpAddress(source,
                [&file, &canceled](const std::uint64_t offset, const std::uint64_t count) {
                    if (canceled() || !file.seek(static_cast<qint64>(offset))) return false;
                    if (canceled()) return false;
                    const QByteArray bytes = file.read(static_cast<qint64>(count));
                    return !canceled() && bytes.size() == static_cast<qsizetype>(count)
                        && std::any_of(bytes.cbegin(), bytes.cend(),
                            [](const char byte) { return static_cast<unsigned char>(byte) != 0; });
                }, canceled);
            plan = planDumpRead(source, result.address, request.bytes);
        }
        if (canceled()) { result.status = DumpMemoryReadStatus::Canceled; return result; }
        if (plan.status != DumpMemoryReadStatus::Success) { result.status = plan.status; return result; }
        result.rangeIndex = plan.rangeIndex;
        result.fileOffset = plan.fileOffset;
        if (!file.seek(static_cast<qint64>(plan.fileOffset)))
        {
            result.status = DumpMemoryReadStatus::ReadFailed;
            return result;
        }
        if (canceled()) { result.status = DumpMemoryReadStatus::Canceled; return result; }
        result.bytes = file.read(static_cast<qint64>(plan.bytes));
        if (canceled()) { result.bytes.clear(); result.status = DumpMemoryReadStatus::Canceled; return result; }
        // 同一路径替换、截断或修改发生在探测/读取期间时，也不提交旧范围的字节。
        if (!dumpFileIdentityMatches(source) || file.size() < 0
            || static_cast<std::uint64_t>(file.size()) != source.fileSize)
        {
            result.bytes.clear();
            result.status = DumpMemoryReadStatus::FileChanged;
            return result;
        }
        if (result.bytes.size() != static_cast<qsizetype>(plan.bytes))
        {
            result.bytes.clear();
            result.status = DumpMemoryReadStatus::ReadFailed;
            return result;
        }
        // Preserve original first-match order, but perform the full metadata search off GUI.
        for (std::size_t index = 0; index < source.modules.size(); ++index)
        {
            if ((index & 255U) == 0U && canceled()) { result.status = DumpMemoryReadStatus::Canceled; return result; }
            const auto& module = source.modules[index];
            if (module.size != 0 && result.address >= module.base && result.address - module.base < module.size)
            { result.moduleIndex = index; break; }
        }
        for (std::size_t index = 0; index < source.regions.size(); ++index)
        {
            if ((index & 255U) == 0U && canceled()) { result.status = DumpMemoryReadStatus::Canceled; return result; }
            const auto& region = source.regions[index];
            if (region.size != 0 && result.address >= region.base && result.address - region.base < region.size)
            { result.regionIndex = index; break; }
        }
        if (canceled()) { result.status = DumpMemoryReadStatus::Canceled; return result; }
        result.status = DumpMemoryReadStatus::Success;
        return result;
    }

    QString inputStyle()
    {
        return QStringLiteral(
            "QLineEdit,QComboBox{"
            "  border:1px solid %2;"
            "  border-radius:4px;"
            "  background:%3;"
            "  color:%4;"
            "  padding:2px 6px;"
            "}"
            "QLineEdit:focus,QComboBox:focus{ border:1px solid %1; }")
            .arg(KswordTheme::PrimaryBlueHex)
            .arg(KswordTheme::BorderHex())
            .arg(KswordTheme::SurfaceHex())
            .arg(KswordTheme::TextPrimaryHex());
    }

    bool parseUnsignedAddress(const QString& text, std::uint64_t* const valueOut)
    {
        if (valueOut == nullptr)
        {
            return false;
        }
        QString normalized = text.trimmed();
        if (normalized.isEmpty())
        {
            return false;
        }
        int base = 10;
        if (normalized.startsWith(QStringLiteral("0x"), Qt::CaseInsensitive))
        {
            normalized = normalized.mid(2);
            base = 16;
        }
        else if (normalized.contains(QRegularExpression(QStringLiteral("[A-Fa-f]"))))
        {
            // 裸十六进制地址在调试场景很常见，如 FFFFF8058282A8E0。
            base = 16;
        }
        bool ok = false;
        const qulonglong value = normalized.toULongLong(&ok, base);
        if (!ok)
        {
            return false;
        }
        *valueOut = static_cast<std::uint64_t>(value);
        return true;
    }
}

DumpMemoryView::DumpMemoryView(QWidget* parent)
    : QWidget(parent)
{
    m_readOperation = new ks::ui::AsyncOperation(this);
    auto* const rootLayout = new QVBoxLayout(this);
    rootLayout->setContentsMargins(8, 8, 8, 8);
    rootLayout->setSpacing(8);

    auto* const toolbarLayout = new QHBoxLayout();
    toolbarLayout->setContentsMargins(0, 0, 0, 0);
    toolbarLayout->setSpacing(6);
    m_addressLabel = new QLabel(this);
    m_addressEdit = new QLineEdit(this);
    m_addressEdit->setClearButtonEnabled(true);
    m_readSizeLabel = new QLabel(this);
    m_readSizeCombo = new QComboBox(this);
    m_readSizeCombo->addItem(QStringLiteral("256 B"), 256);
    m_readSizeCombo->addItem(QStringLiteral("1 KiB"), 1024);
    m_readSizeCombo->addItem(QStringLiteral("4 KiB"), 4 * 1024);
    m_readSizeCombo->addItem(QStringLiteral("16 KiB"), 16 * 1024);
    m_readSizeCombo->addItem(QStringLiteral("64 KiB"), 64 * 1024);
    m_readSizeCombo->setCurrentIndex(2);
    m_readButton = new QPushButton(this);
    m_previousButton = new QPushButton(this);
    m_nextButton = new QPushButton(this);
    m_addressEdit->setStyleSheet(inputStyle());
    m_readSizeCombo->setStyleSheet(inputStyle());
    // 专业内存内容保持自绘，外部地址与长度控件采用共享输入外观。
    ks::ui::StyleStandaloneInput(m_addressEdit);
    ks::ui::StylePrimaryCombo(m_readSizeCombo);
    for (QPushButton* const button : { m_readButton, m_previousButton, m_nextButton })
    {
        ks::ui::ApplyFlatButtonTheme(button, ks::ui::FlatButtonTone::Neutral,
            ks::ui::FlatButtonAppearance::Solid);
    }

    toolbarLayout->addWidget(m_addressLabel);
    toolbarLayout->addWidget(m_addressEdit, 1);
    toolbarLayout->addWidget(m_readSizeLabel);
    toolbarLayout->addWidget(m_readSizeCombo);
    toolbarLayout->addWidget(m_readButton);
    toolbarLayout->addWidget(m_previousButton);
    toolbarLayout->addWidget(m_nextButton);
    ks::ui::NormalizeToolbarRow(toolbarLayout);
    rootLayout->addLayout(toolbarLayout);

    // 普通映射说明使用共享值模型；隐藏时保留完整文档，复制/导出不依赖已显示行。
    m_messageView = new ks::ui::StructuredFieldView(this);
    m_messageView->setValueModelProjection();
    m_messageView->setMaximumHeight(145);
    rootLayout->addWidget(m_messageView);

    m_memoryEditor = new ks::ui::SnapshotWorkbenchWidget(this);
    m_memoryEditor->setEditable(false);
    m_hexEditor = m_memoryEditor->hexEditor();
    rootLayout->addWidget(m_memoryEditor, 1);

    connect(m_readButton, &QPushButton::clicked, this, [this]() { loadCurrentInput(); });
    connect(m_addressEdit, &QLineEdit::returnPressed, this, [this]() { loadCurrentInput(); });
    connect(m_previousButton, &QPushButton::clicked, this, [this]() { goPreviousPage(); });
    connect(m_nextButton, &QPushButton::clicked, this, [this]() { goNextPage(); });
    connect(m_readSizeCombo, &QComboBox::currentIndexChanged, this, [this](const int)
        {
            if (m_currentRangeIndex >= 0)
            {
                loadAddress(m_currentAddress);
            }
        });
    // 用户编辑优先于迟到的默认推荐或旧地址读取；取消不等待正在执行的系统 I/O。
    connect(m_addressEdit, &QLineEdit::textEdited, this, [this]() {
        if (m_readOperation->isInFlight())
        {
            m_readOperation->cancel();
            // 旧系统 I/O 仍在途时保持读取中；实际执行槽释放后恢复原说明。
        }
    });
    m_readOperation->setStateChangedCallback([this]() { updateReadControls(); });
    retranslateUi();
    clearData();
}

DumpMemoryView::~DumpMemoryView()
{
    // 必须先关闭共享投递门禁，随后 Qt 子树和页面状态才可开始销毁。
    m_readOperation->close();
}

void DumpMemoryView::changeEvent(QEvent* event)
{
    QWidget::changeEvent(event);
    if (event && event->type() == QEvent::LanguageChange) retranslateUi();
}

void DumpMemoryView::setDumpData(const ks::minidump::DumpParseResult& result)
{
    if (m_source && sameDumpMemorySource(*m_source, result))
    {
        retranslateUi();
        return;
    }
    const QPointer<DumpMemoryView> alive(this);
    clearData();
    if (!alive) return;
    auto source = std::make_shared<DumpMemorySource>();
    source->filePath = result.filePath;
    source->fileSize = result.fileSize;
    source->modifiedUtcMs = result.fileLastModifiedUtcMs;
    source->faultingAddress = result.faultingAddress;
    source->pointerSize = result.pointerSize;
    source->ranges = result.capturedMemoryRanges;
    source->modules = result.modules;
    source->regions = result.memoryRegions;
    m_source = std::move(source);
    if (m_source->ranges.empty())
    {
        m_viewState = ViewState::NoRanges;
        refreshDiagnostic();
        return;
    }
    // 仅显示无需 I/O 的回退地址；故障地址优先及非零推荐由后台统一决定。
    m_addressEdit->setText(formatHex(m_source->ranges.front().virtualAddress));
    // 先初始化解码架构，再让出事件循环；后台返回时保留用户在等待期间的选择。
    const auto architecture = m_source->pointerSize == 4
        ? ks::ui::DisassemblyArchitecture::X86 : ks::ui::DisassemblyArchitecture::X64;
    const auto sourceGeneration = m_sourceGeneration;
    m_memoryEditor->setSnapshot(QByteArray(), m_source->ranges.front().virtualAddress, architecture);
    if (!alive || sourceGeneration != m_sourceGeneration) return;
    m_architectureInitialized = true;
    submitRead(0, true);
}

void DumpMemoryView::clearData()
{
    const QPointer<DumpMemoryView> alive(this);
    m_readOperation->cancel();
    if (!alive) return;
    ++m_sourceGeneration;
    m_source.reset();
    m_lastRead.reset();
    m_viewState = ViewState::Idle;
    m_architectureInitialized = false;
    m_currentAddress = 0;
    m_currentReadBytes = 0;
    m_currentRangeIndex = -1;
    m_memoryEditor->clear();
    if (!alive) return;
    m_addressEdit->clear();
    if (!alive) return;
    refreshDiagnostic();
    if (!alive) return;
    updateReadControls();
}

void DumpMemoryView::retranslateUi()
{
    if (m_addressLabel == nullptr)
    {
        return;
    }
    m_addressLabel->setText(ks::i18n::text(
        QStringLiteral("minidump.memory_view.address_label"), QStringLiteral("虚拟地址")));
    m_addressEdit->setPlaceholderText(ks::i18n::text(
        QStringLiteral("minidump.memory_view.address_placeholder"),
        QStringLiteral("输入虚拟地址，或 模块名+偏移")));
    m_addressEdit->setToolTip(ks::i18n::text(
        QStringLiteral("minidump.memory_view.address_tooltip"),
        QStringLiteral("支持 0x 地址、裸十六进制地址，以及 模块名+偏移。")));
    m_readSizeLabel->setText(ks::i18n::text(
        QStringLiteral("minidump.memory_view.read_size_label"), QStringLiteral("读取大小")));
    const QSignalBlocker sizeSignals(m_readSizeCombo);
    const QVariant previousSize = m_readSizeCombo->currentData();
    m_readSizeCombo->setItemText(0, ks::i18n::text(
        QStringLiteral("minidump.memory_view.size.256b"), QStringLiteral("256 字节")));
    m_readSizeCombo->setItemText(1, ks::i18n::text(
        QStringLiteral("minidump.memory_view.size.1k"), QStringLiteral("1 KiB")));
    m_readSizeCombo->setItemText(2, ks::i18n::text(
        QStringLiteral("minidump.memory_view.size.4k"), QStringLiteral("4 KiB")));
    m_readSizeCombo->setItemText(3, ks::i18n::text(
        QStringLiteral("minidump.memory_view.size.16k"), QStringLiteral("16 KiB")));
    m_readSizeCombo->setItemText(4, ks::i18n::text(
        QStringLiteral("minidump.memory_view.size.64k"), QStringLiteral("64 KiB")));
    const int previousSizeIndex = m_readSizeCombo->findData(previousSize);
    if (previousSizeIndex >= 0)
    {
        m_readSizeCombo->setCurrentIndex(previousSizeIndex);
    }
    m_readButton->setText(ks::i18n::text(
        QStringLiteral("minidump.memory_view.action.read"), QStringLiteral("读取")));
    m_previousButton->setText(ks::i18n::text(
        QStringLiteral("minidump.memory_view.action.previous"), QStringLiteral("上一页")));
    m_nextButton->setText(ks::i18n::text(
        QStringLiteral("minidump.memory_view.action.next"), QStringLiteral("下一页")));
    refreshDiagnostic();
}

bool DumpMemoryView::readAddressText(const QString& text, std::uint64_t* const addressOut) const
{
    if (parseUnsignedAddress(text, addressOut))
    {
        return true;
    }
    const int plusIndex = text.lastIndexOf(QLatin1Char('+'));
    if (plusIndex <= 0 || plusIndex == text.size() - 1)
    {
        return false;
    }
    std::uint64_t offset = 0;
    if (!parseUnsignedAddress(text.mid(plusIndex + 1), &offset))
    {
        return false;
    }
    const QString moduleText = text.left(plusIndex).trimmed();
    if (!m_source) return false;
    for (const ks::minidump::ModuleEntry& module : m_source->modules)
    {
        const QString fileName = QFileInfo(module.name).fileName();
        const QString nativeModulePath = QDir::toNativeSeparators(module.name);
        const QString nativeModuleText = QDir::toNativeSeparators(moduleText);
        if (module.name.compare(moduleText, Qt::CaseInsensitive) != 0 &&
            nativeModulePath.compare(nativeModuleText, Qt::CaseInsensitive) != 0 &&
            fileName.compare(moduleText, Qt::CaseInsensitive) != 0)
        {
            continue;
        }
        if (module.base > std::numeric_limits<std::uint64_t>::max() - offset)
        {
            return false;
        }
        *addressOut = module.base + offset;
        return true;
    }
    return false;
}

int DumpMemoryView::findRangeIndex(const std::uint64_t address) const
{
    return m_source ? findDumpRange(*m_source, address) : -1;
}

void DumpMemoryView::loadCurrentInput()
{
    // 禁用按钮之外也保护 Enter，运行中的分页不能基于旧成功页重复计算目标。
    if (m_readOperation->isInFlight()) return;
    std::uint64_t address = 0;
    if (!readAddressText(m_addressEdit->text(), &address))
    {
        m_readOperation->cancel();
        m_memoryEditor->clear();
        m_lastRead.reset();
        m_viewState = ViewState::InvalidInput;
        refreshDiagnostic();
        return;
    }
    loadAddress(address);
}

void DumpMemoryView::loadAddress(const std::uint64_t address)
{
    if (m_readOperation->isInFlight()) return;
    submitRead(address, false);
}

void DumpMemoryView::submitRead(const std::uint64_t address, const bool chooseInitialAddress)
{
    m_viewState = ViewState::Reading;
    refreshDiagnostic();
    const DumpMemoryReadRequest request{ m_source, m_sourceGeneration, address,
        selectedReadBytes(), chooseInitialAddress };
    m_readOperation->submit<DumpMemoryReadRequest, DumpMemoryReadResult>(request,
        [](const DumpMemoryReadRequest& value, const ks::ui::AsyncOperationToken& token) {
            return readDumpMemory(value, token);
        },
        [this](const DumpMemoryReadResult& result, const std::uint64_t) {
            finishRead(result);
            return true; // 拒绝过期来源也必须完成控制器收尾，不能占住 active。
        },
        [this](std::exception_ptr) {
            m_lastRead = std::make_shared<DumpMemoryReadResult>();
            m_viewState = ViewState::ReadResult;
            m_memoryEditor->clear();
            refreshDiagnostic();
        });
}

void DumpMemoryView::finishRead(const DumpMemoryReadResult& result)
{
    if (result.sourceGeneration != m_sourceGeneration
        || result.status == DumpMemoryReadStatus::Canceled) return;
    m_lastRead = std::make_shared<DumpMemoryReadResult>(result);
    m_viewState = ViewState::ReadResult;
    if (result.status != DumpMemoryReadStatus::Success)
    {
        m_memoryEditor->clear();
        refreshDiagnostic();
        return;
    }
    m_currentAddress = result.address;
    m_currentReadBytes = static_cast<std::uint64_t>(result.bytes.size());
    m_currentRangeIndex = result.rangeIndex;
    m_addressEdit->setText(formatHex(result.address));
    const auto architecture = m_architectureInitialized
        ? m_memoryEditor->currentArchitecture()
        : (m_source->pointerSize == 4 ? ks::ui::DisassemblyArchitecture::X86
                                    : ks::ui::DisassemblyArchitecture::X64);
    const QString sourceIdentity = QStringLiteral("dump:%1:%2:%3")
        .arg(m_source->filePath).arg(m_source->fileSize).arg(m_source->modifiedUtcMs);
    const QPointer<DumpMemoryView> alive(this);
    const auto sourceGeneration = m_sourceGeneration;
    m_memoryEditor->setSnapshot(result.bytes, result.address, architecture, result.address, sourceIdentity);
    if (!alive || sourceGeneration != m_sourceGeneration) return;
    m_memoryEditor->setEditable(false);
    m_architectureInitialized = true;
    refreshDiagnostic();
}

void DumpMemoryView::updateReadControls()
{
    // cancel 后旧系统 I/O 仍可能在途，必须按实际执行槽而非 isBusy() 解锁。
    const bool ready = !m_readOperation->isInFlight();
    m_readButton->setEnabled(ready);
    m_previousButton->setEnabled(ready);
    m_nextButton->setEnabled(ready);
    m_readSizeCombo->setEnabled(ready);
    if (ready && m_viewState == ViewState::Reading)
    {
        m_viewState = m_lastRead ? ViewState::ReadResult : ViewState::Idle;
        refreshDiagnostic();
    }
}

void DumpMemoryView::refreshDiagnostic()
{
    switch (m_viewState)
    {
    case ViewState::Idle:
        setDiagnostic(ks::i18n::text(QStringLiteral("minidump.memory_view.status.idle"),
            QStringLiteral("请选择一个已捕获的虚拟地址。")));
        return;
    case ViewState::NoRanges:
        setDiagnostic(ks::i18n::text(QStringLiteral("minidump.memory_view.status.no_ranges"),
            QStringLiteral("转储中没有可直接读取的虚拟内存范围。")));
        return;
    case ViewState::Reading:
        setDiagnostic(ks::i18n::sourceText(QStringLiteral("读取中…")));
        return;
    case ViewState::InvalidInput:
        setDiagnostic(ks::i18n::text(QStringLiteral("minidump.memory_view.status.invalid_address"),
            QStringLiteral("请输入有效的虚拟地址，或“模块名+偏移”。")));
        return;
    case ViewState::ReadResult:
        break;
    }
    if (!m_lastRead) return;
    switch (m_lastRead->status)
    {
    case DumpMemoryReadStatus::NotCaptured:
        setDiagnostic(ks::i18n::text(QStringLiteral("minidump.memory_view.status.not_captured"),
            QStringLiteral("地址 %1 不在当前转储捕获的虚拟内存范围内。"))
                .arg(formatHex(m_lastRead->address)));
        return;
    case DumpMemoryReadStatus::NoFile:
        setDiagnostic(ks::i18n::text(QStringLiteral("minidump.memory_view.status.no_file"),
            QStringLiteral("没有与当前内存范围关联的转储文件。")));
        return;
    case DumpMemoryReadStatus::FileChanged:
        setDiagnostic(ks::i18n::text(QStringLiteral("minidump.memory_view.status.file_changed"),
            QStringLiteral("转储文件已变更，请重新解析后再读取内存。")));
        return;
    case DumpMemoryReadStatus::OffsetInvalid:
        setDiagnostic(ks::i18n::text(QStringLiteral("minidump.memory_view.status.offset_invalid"),
            QStringLiteral("文件偏移超出可读取范围。")));
        return;
    case DumpMemoryReadStatus::OpenFailed:
        setDiagnostic(ks::i18n::text(QStringLiteral("minidump.memory_view.status.open_failed"),
            QStringLiteral("无法打开转储文件：%1")).arg(m_lastRead->errorText));
        return;
    case DumpMemoryReadStatus::ReadFailed:
        setDiagnostic(ks::i18n::text(QStringLiteral("minidump.memory_view.status.read_failed"),
            QStringLiteral("读取失败或文件内容不足。")));
        return;
    case DumpMemoryReadStatus::Canceled:
        return;
    case DumpMemoryReadStatus::Success:
        break;
    }
    if (!m_source || m_currentRangeIndex < 0
        || static_cast<std::size_t>(m_currentRangeIndex) >= m_source->ranges.size()) return;
    const auto address = m_lastRead->address;
    const auto& range = m_source->ranges[static_cast<std::size_t>(m_currentRangeIndex)];
    ks::ui::FieldDocument details;
    details.section(QStringLiteral("转储映射"));
    details.field(QStringLiteral("虚拟地址"), formatHex(address));
    const std::uint64_t rangeEnd = range.bytes <= std::numeric_limits<std::uint64_t>::max() - range.virtualAddress
        ? range.virtualAddress + range.bytes - 1 : std::numeric_limits<std::uint64_t>::max();
    details.field(QStringLiteral("捕获范围起点"), formatHex(range.virtualAddress));
    details.field(QStringLiteral("捕获范围终点"), formatHex(rangeEnd));
    details.field(QStringLiteral("捕获大小（字节）"), QString::number(range.bytes));
    details.field(QStringLiteral("文件偏移"), formatHex(m_lastRead->fileOffset));
    details.field(QStringLiteral("来源"), range.source, true);
    if (m_lastRead->moduleIndex < m_source->modules.size())
    {
        const auto& module = m_source->modules[m_lastRead->moduleIndex];
        details.field(QStringLiteral("所属模块"), module.name);
        details.field(QStringLiteral("模块偏移"), formatHex(address - module.base));
    }
    if (m_lastRead->regionIndex < m_source->regions.size())
    {
        const auto& region = m_source->regions[m_lastRead->regionIndex];
        details.section(QStringLiteral("内存区域"));
        details.field(QStringLiteral("基址"), formatHex(region.base));
        details.field(QStringLiteral("状态"), region.state, true);
        details.field(QStringLiteral("保护"), region.protect, true);
        details.field(QStringLiteral("类型"), region.type, true);
    }
    details.field(QStringLiteral("本次读取（字节）"), QString::number(m_lastRead->bytes.size()));
    m_messageView->setDocument(details);
}

void DumpMemoryView::goPreviousPage()
{
    if (m_readOperation->isInFlight()) return;
    if (m_currentRangeIndex < 0)
    {
        loadCurrentInput();
        return;
    }
    const ks::minidump::DumpMemoryRange& current =
        m_source->ranges[static_cast<std::size_t>(m_currentRangeIndex)];
    if (m_currentAddress > current.virtualAddress)
    {
        const std::uint64_t previous = m_currentAddress - current.virtualAddress >= selectedReadBytes()
            ? m_currentAddress - selectedReadBytes()
            : current.virtualAddress;
        loadAddress(previous);
        return;
    }
    for (int index = m_currentRangeIndex - 1; index >= 0; --index)
    {
        const ks::minidump::DumpMemoryRange& previous =
            m_source->ranges[static_cast<std::size_t>(index)];
        if (previous.bytes == 0)
        {
            continue;
        }
        const std::uint64_t pageBytes = std::min(previous.bytes, selectedReadBytes());
        const auto pageOffset = previous.bytes - pageBytes;
        if (previous.virtualAddress > std::numeric_limits<std::uint64_t>::max() - pageOffset) continue;
        loadAddress(previous.virtualAddress + pageOffset);
        return;
    }
}

void DumpMemoryView::goNextPage()
{
    if (m_readOperation->isInFlight()) return;
    if (m_currentRangeIndex < 0)
    {
        loadCurrentInput();
        return;
    }
    const ks::minidump::DumpMemoryRange& current =
        m_source->ranges[static_cast<std::size_t>(m_currentRangeIndex)];
    const std::uint64_t consumed = m_currentAddress - current.virtualAddress;
    if (m_currentReadBytes != 0 && m_currentReadBytes <= current.bytes
        && consumed <= current.bytes - m_currentReadBytes)
    {
        const std::uint64_t next = m_currentAddress + m_currentReadBytes;
        if (next > m_currentAddress && next - current.virtualAddress < current.bytes)
        {
            loadAddress(next);
            return;
        }
    }
    for (std::size_t index = static_cast<std::size_t>(m_currentRangeIndex) + 1;
         index < m_source->ranges.size(); ++index)
    {
        if (m_source->ranges[index].bytes != 0)
        {
            loadAddress(m_source->ranges[index].virtualAddress);
            return;
        }
    }
}

std::uint64_t DumpMemoryView::selectedReadBytes() const
{
    const QVariant data = m_readSizeCombo->currentData();
    bool ok = false;
    const qulonglong selected = data.toULongLong(&ok);
    return ok ? std::clamp<std::uint64_t>(selected, 1, kMaximumReadBytes) : 4ull * 1024ull;
}

void DumpMemoryView::setDiagnostic(const QString& text)
{
    if (m_messageView != nullptr)
    {
        m_messageView->setDocument(ks::ui::FieldDocument{}.note(text), true); // 新读取/错误诊断立即替换上一地址字段。
    }
}

QString DumpMemoryView::formatHex(const std::uint64_t value)
{
    return QStringLiteral("0x%1").arg(QString::number(value, 16).toUpper());
}

ks::ui::PageNavigationCatalog DumpMemoryView::navigationCatalog(const DumpMemoryView* existing)
{
    return ks::ui::SnapshotWorkbenchWidget::navigationCatalog(existing ? existing->m_memoryEditor : nullptr);
}
QWidget* DumpMemoryView::activateNavigationDescriptor(const QString& id)
{
    const QPointer<DumpMemoryView> self(this);
    const QPointer<ks::ui::SnapshotWorkbenchWidget> editor(m_memoryEditor);
    if (!editor) return nullptr;
    const QPointer<QWidget> target(editor->activateNavigationDescriptor(id));
    if (!self || !editor || !target || self->m_memoryEditor != editor || editor->parentWidget() != self) return nullptr;
    return target.data();
}
