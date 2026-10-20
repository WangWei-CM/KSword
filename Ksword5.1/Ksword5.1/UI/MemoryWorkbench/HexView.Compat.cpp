// HexView.Compat.cpp
// 作用：HexView 的"旧缓冲模型兼容层"：页提供者、setBuffer / clearBuffer、setReference / clearReference、
// setByteQuiet、jumpToAddress，以及画布编辑（暂存 / 丢弃）到缓冲的同步与 byteEdited 信号。
//
// 核心不变式（头文件第四节）：叠加层补丁集合 == { 地址 : 缓冲[地址] != 基线[地址] }。
// - 画布的数据源是基线（BufferProvider），编辑只落在叠加层补丁里，所以画布显示恒等于缓冲，
//   编辑、回滚、撤销都不必让页缓存失效；只有换基线（setBuffer / setReference / clearReference）才刷新页缓存。
// - 缓冲的写入集中在两处：syncBufferFromOverlay（画布编辑）与 setByteQuiet（宿主回滚）。

#include "HexView.h"

#include "HexCanvasFormat.h"
#include "HexViewFormat.h"

#include <QPointer>
#include <algorithm>
#include <cstring>
#include <limits>

namespace ks::ui
{
    namespace
    {
        // kPageBytes：页大小，与 HexViewport 保持一致。
        constexpr std::uint64_t kPageBytes = ksword::memwb::HexViewport::kPageBytes;

        // kOverlayIdentity：叠加层的目标身份串。HexView 只有一个目标，固定即可。
        constexpr const char* kOverlayIdentity = "hexview";

        // ToVector：QByteArray 转叠加层使用的字节向量（叠加层是 Qt-free 的）。
        std::vector<std::uint8_t> ToVector(const QByteArray& bytes)
        {
            const std::uint8_t* begin = reinterpret_cast<const std::uint8_t*>(bytes.constData());
            return std::vector<std::uint8_t>(begin, begin + bytes.size());
        }
    }

    // BufferProvider：画布的页提供者，从 HexView 的基线切页同步回填。
    // 读的是 HexView 的成员（基线与基址），所以换基线后只需让画布 refresh()，不必重建提供者。
    class HexView::BufferProvider final : public IHexPageProvider
    {
    public:
        // 构造：owner 为所属 HexView（非拥有，生命周期长于本对象）。
        explicit BufferProvider(HexView* owner)
            : m_owner(owner)
        {
        }

        // RequestPages：逐页切片并同步回填；页与基线的交集之外的字节标记为无效。
        // 传入：页范围列表与代次；传出：无。
        void RequestPages(const std::vector<HexFetchRange>& ranges, std::uint64_t sourceRevision) override
        {
            const QByteArray& source = m_owner->m_baseline;
            const std::uint64_t base = m_owner->m_baseAddress;
            const std::uint64_t dataSize = static_cast<std::uint64_t>(source.size());
            if (dataSize == 0)
            {
                return;
            }
            const std::uint64_t dataLast = base + (dataSize - 1ULL);

            for (const HexFetchRange& range : ranges)
            {
                for (std::uint64_t index = 0; index < range.pageCount; ++index)
                {
                    // pageStart/pageLast：该页的起止地址（页对齐，不会溢出）。
                    const std::uint64_t pageStart = range.firstPageStart + index * kPageBytes;
                    const std::uint64_t pageLast = pageStart + (kPageBytes - 1ULL);

                    // bytes/mask：回填的页数据与有效掩码，默认全部无效。
                    QByteArray bytes(static_cast<qsizetype>(kPageBytes), '\0');
                    QByteArray mask(static_cast<qsizetype>(kPageBytes), '\0');

                    // 页与基线的交集：[copyFirst, copyLast]。
                    const std::uint64_t copyFirst = (std::max)(pageStart, base);
                    const std::uint64_t copyLast = (std::min)(pageLast, dataLast);
                    if (copyFirst <= copyLast)
                    {
                        const qsizetype count = static_cast<qsizetype>(copyLast - copyFirst + 1ULL);
                        const qsizetype inPage = static_cast<qsizetype>(copyFirst - pageStart);
                        const qsizetype inData = static_cast<qsizetype>(copyFirst - base);
                        std::memcpy(bytes.data() + inPage, source.constData() + inData, static_cast<std::size_t>(count));
                        std::memset(mask.data() + inPage, 1, static_cast<std::size_t>(count));
                    }
                    m_owner->m_canvas->deliverPage(pageStart, bytes, mask, sourceRevision);
                }
            }
        }

    private:
        HexView* m_owner;   // 所属 HexView（非拥有）
    };

    // 析构：画布持有叠加层与页提供者的裸指针，二者是本类的成员（先于子控件销毁），
    // 所以先让画布放手，再释放提供者；子控件随后由 Qt 销毁。
    HexView::~HexView()
    {
        if (m_canvas != nullptr)
        {
            m_canvas->setOverlay(nullptr);
            m_canvas->setPageProvider(nullptr);
        }
        delete m_provider;
        m_provider = nullptr;
    }

    // 把缓冲模型接到画布上：创建页提供者，装上叠加层，默认只读。
    void HexView::attachBufferModel()
    {
        m_provider = new BufferProvider(this);
        m_canvas->setOverlay(&m_overlay);
        m_canvas->setPageProvider(m_provider);
        m_canvas->setEditable(false);
    }

    // 缓冲最后一个字节的地址（缓冲非空时有意义）。
    std::uint64_t HexView::lastAddress() const
    {
        return m_baseAddress + (static_cast<std::uint64_t>(m_buffer.size()) - 1ULL);
    }

    // 以当前基线重建叠加层：换一个全新的叠加层对象（补丁与"上次读取"都清空），再载入基线。
    // 传入：previousRead 非空时作为"上次读取"（先载入它、再用基线刷新，叠加层就把它留作上一份）。
    // 载入失败（基线终点超出 uint64）时叠加层保持无基线，视图可看不可编辑。
    void HexView::installBaseline(const QByteArray& previousRead)
    {
        m_overlay = ksword::memwb::MemoryDiffOverlay();
        if (m_baseline.isEmpty())
        {
            return;
        }

        // mask：基线每个字节都是真实读到的。
        const std::vector<std::uint8_t> mask(static_cast<std::size_t>(m_baseline.size()), 1);
        if (!previousRead.isEmpty() && previousRead.size() == m_baseline.size())
        {
            m_overlay.LoadBaseline(kOverlayIdentity, m_baseAddress, ToVector(previousRead), mask);
            m_overlay.RefreshBaseline(kOverlayIdentity, m_baseAddress, ToVector(m_baseline), mask);
            return;
        }
        m_overlay.LoadBaseline(kOverlayIdentity, m_baseAddress, ToVector(m_baseline), mask);
    }

    // 把"缓冲与基线不同的字节"按连续段暂存成补丁（橙色）。
    // 传出：false 表示某段暂存失败（总量超限或叠加层没有基线），此时叠加层已不可信，调用方必须退回无参照。
    bool HexView::applyDiffPatches()
    {
        const std::size_t size = static_cast<std::size_t>(m_buffer.size());
        const char* current = m_buffer.constData();
        const char* baseline = m_baseline.constData();

        std::size_t index = 0;
        while (index < size)
        {
            if (current[index] == baseline[index])
            {
                ++index;
                continue;
            }

            // 找到一段连续差异 [index, runEnd)，整段一次暂存。
            std::size_t runEnd = index + 1U;
            while (runEnd < size && current[runEnd] != baseline[runEnd])
            {
                ++runEnd;
            }
            const std::vector<std::uint8_t> payload(
                reinterpret_cast<const std::uint8_t*>(current) + index,
                reinterpret_cast<const std::uint8_t*>(current) + runEnd);
            if (m_overlay.Stage(m_baseAddress + static_cast<std::uint64_t>(index), payload)
                != ksword::memwb::StageStatus::Ok)
            {
                return false;
            }
            index = runEnd;
        }
        return true;
    }

    // 数据整块被替换（setBuffer / clearBuffer）之后的统一收尾：
    // 查找状态作废、跳转条按新范围更新、状态条恢复三段显示。
    void HexView::onDataReplaced()
    {
        m_findBar->dataChanged();
        if (m_buffer.isEmpty())
        {
            m_gotoBar->clearSpace();
        }
        else
        {
            m_gotoBar->setSpace(m_baseAddress, lastAddress(), m_canvas->bytesPerRow());
        }
        m_status->clearMessage();
        refreshStatus();
    }

    // 整块替换内容。
    void HexView::setBuffer(std::uint64_t baseAddress, const QByteArray& bytes)
    {
        // content：base + 长度超过 2^64 时截断到放得下的部分（与画布的 setStaticData 同一规则）。
        QByteArray content = bytes;
        if (!content.isEmpty())
        {
            const std::uint64_t room = std::numeric_limits<std::uint64_t>::max() - baseAddress;
            if (static_cast<std::uint64_t>(content.size()) - 1ULL > room)
            {
                content = content.left(static_cast<qsizetype>(room + 1ULL));
            }
        }

        // 先换成员再通知画布：画布装新空间时会立即向提供者要页，那时基线必须已经就绪。
        ++m_bufferGeneration;
        m_baseAddress = baseAddress;
        m_buffer = content;
        if (m_editNotificationDepth)
        {
            m_batchDirty = true;
            m_batchRangeComplete = false;
        }
        m_baseline = content;
        m_hasReference = false;
        installBaseline();

        if (m_buffer.isEmpty())
        {
            m_canvas->clearAddressSpace();
        }
        else
        {
            m_canvas->setAddressSpace(m_baseAddress, lastAddress());
        }
        onDataReplaced();
    }

    // 清空内容，基址保留。
    void HexView::clearBuffer()
    {
        setBuffer(m_baseAddress, QByteArray());
    }

    // 设置变更着色参照。
    bool HexView::setReference(const QByteArray& original, const QByteArray& previousRead)
    {
        // 空缓冲：没有什么可着色，只有 original 也为空才算"等长"。
        if (m_buffer.isEmpty())
        {
            return original.isEmpty();
        }

        // 尺寸不符：旧控件规则"引用尺寸与缓冲尺寸不符时不着色"——清除参照并报告失败。
        if (original.size() != m_buffer.size())
        {
            clearReference();
            return false;
        }

        // 新基线 = original；previousRead 尺寸不符被忽略（只失去冷色，不影响橙色）。
        m_baseline = original;
        m_hasReference = true;
        const bool previousUsable = !previousRead.isEmpty() && previousRead.size() == original.size();
        installBaseline(previousUsable ? previousRead : QByteArray());

        // 缓冲与基线的差异以补丁体现为橙色；暂存失败（总量超限）则整个退回无参照。
        if (!applyDiffPatches())
        {
            clearReference();
            return false;
        }

        // 换了基线：画布清缓存、换代次、重新向提供者要可见页（选区与滚动位置保持不变）。
        m_canvas->refresh();
        return true;
    }

    // 基线回到当前缓冲，着色全部消失。
    void HexView::clearReference()
    {
        m_hasReference = false;
        if (m_buffer.isEmpty())
        {
            return;
        }
        m_baseline = m_buffer;
        installBaseline();
        m_canvas->refresh();
    }

    // 是否设置过有效参照。
    bool HexView::hasReference() const
    {
        return m_hasReference;
    }

    // 静默改一个字节。
    bool HexView::setByteQuiet(std::uint64_t absoluteAddress, std::uint8_t value)
    {
        // 范围：先比较后相减，不会回绕。
        if (m_buffer.isEmpty() || absoluteAddress < m_baseAddress
            || absoluteAddress - m_baseAddress >= static_cast<std::uint64_t>(m_buffer.size()))
        {
            return false;
        }
        const qsizetype offset = static_cast<qsizetype>(absoluteAddress - m_baseAddress);
        if (static_cast<std::uint8_t>(m_buffer.at(offset)) == value)
        {
            return true;
        }

        if (m_overlay.HasBaseline())
        {
            // 补丁与"是否不同于基线"保持一致：回到基线值就丢补丁，否则暂存。
            // 先暂存再写缓冲：暂存失败（总量超限）时什么都没改，直接返回 false。
            if (static_cast<std::uint8_t>(m_baseline.at(offset)) == value)
            {
                m_overlay.Discard(absoluteAddress, 1);
            }
            else if (m_overlay.Stage(absoluteAddress, std::vector<std::uint8_t>{ value })
                     != ksword::memwb::StageStatus::Ok)
            {
                return false;
            }
            m_buffer[offset] = static_cast<char>(value);
            m_canvas->notifyOverlayChanged();
        }
        else
        {
            // 叠加层没有基线（缓冲贴着地址空间末端）：不分基线与补丁，直接改缓冲并让基线同步，靠刷新重画。
            m_buffer[offset] = static_cast<char>(value);
            m_baseline = m_buffer;
            m_canvas->refresh();
        }

        // Quiet rollback inside a legacy slot is part of the same final batch.
        if (m_editNotificationDepth) noteEditedRange(absoluteAddress, absoluteAddress);
        // 缓冲内容变了：查找状态作废。
        m_findBar->dataChanged();
        return true;
    }

    void HexView::noteEditedRange(std::uint64_t first, std::uint64_t last)
    {
        if (!m_batchDirty) { m_batchFirst = first; m_batchLast = last; }
        else { m_batchFirst = (std::min)(m_batchFirst, first); m_batchLast = (std::max)(m_batchLast, last); }
        m_batchDirty = true;
    }

    // 把叠加层里 [address, address+length) 的"叠加后取值"同步进缓冲，并对每个真正变化的字节发 byteEdited。
    // 传入：画布刚暂存或丢弃的范围。信号在缓冲与查找状态都更新完之后才发，槽里可以安全地回滚。
    void HexView::syncBufferFromOverlay(std::uint64_t address, std::uint64_t length)
    {
        if (length == 0 || m_buffer.isEmpty() || address < m_baseAddress)
        {
            return;
        }
        const std::uint64_t offset = address - m_baseAddress;
        const std::uint64_t size = static_cast<std::uint64_t>(m_buffer.size());
        if (offset >= size)
        {
            return;
        }
        const std::uint64_t count = (std::min)(length, size - offset);

        // effective：叠加后的取值（补丁值或基线字节）。
        const ksword::memwb::MaterializedBytes effective = m_overlay.Materialize(address, count);
        if (!effective.ok || effective.bytes.size() != static_cast<std::size_t>(count))
        {
            return;
        }

        // Change：一个字节的变化记录。
        struct Change
        {
            std::uint64_t address;  // 绝对地址
            std::uint8_t oldValue;  // 缓冲里的旧值
            std::uint8_t newValue;  // 叠加后的新值
        };
        std::vector<Change> changes;
        const char* current = m_buffer.constData() + offset;
        for (std::uint64_t index = 0; index < count; ++index)
        {
            if (effective.validMask[static_cast<std::size_t>(index)] == 0)
            {
                continue;
            }
            const std::uint8_t newValue = effective.bytes[static_cast<std::size_t>(index)];
            const std::uint8_t oldValue = static_cast<std::uint8_t>(current[index]);
            if (newValue != oldValue)
            {
                changes.push_back(Change{ address + index, oldValue, newValue });
            }
        }
        if (changes.empty())
        {
            return;
        }

        // 先把全部变化写进缓冲（只在这里写，m_buffer.data() 在此处首次分离），再通知查找，最后逐字节发信号。
        char* destination = m_buffer.data() + offset;
        for (const Change& change : changes)
        {
            destination[change.address - address] = static_cast<char>(change.newValue);
        }
        const QPointer<HexView> alive(this);
        const auto generation = m_bufferGeneration;
        ++m_editNotificationDepth;
        noteEditedRange(changes.front().address, changes.back().address);
        m_findBar->dataChanged();
        if (!alive) return;
        for (const Change& change : changes)
        {
            if (generation != m_bufferGeneration) break;
            emit byteEdited(change.address, change.oldValue, change.newValue);
            if (!alive) return;
            if (generation != m_bufferGeneration) break; // Never publish old byte identities after replacement.
        }
        --m_editNotificationDepth;
        if (!m_editNotificationDepth && m_batchDirty)
        {
            const bool complete = m_batchRangeComplete;
            const auto first = complete ? m_batchFirst : m_baseAddress;
            const auto changedLength = complete ? m_batchLast - m_batchFirst + 1
                : static_cast<std::uint64_t>(m_buffer.size());
            m_batchDirty = false;
            m_batchRangeComplete = true;
            emit bytesEdited(first, changedLength, complete);
        }
    }

    // 画布暂存成功：同步缓冲。
    void HexView::onEditStaged(quint64 address, quint64 length)
    {
        syncBufferFromOverlay(address, length);
    }

    // 画布丢弃了暂存（Backspace）：字节回到基线值，缓冲同步回退。
    void HexView::onEditDiscarded(quint64 address, quint64 length)
    {
        syncBufferFromOverlay(address, length);
    }

    // 选中并滚动到某地址。
    bool HexView::jumpToAddress(std::uint64_t absoluteAddress)
    {
        if (m_buffer.isEmpty())
        {
            showStatusMessage(HexViewStatusBar::Kind::Warning, QStringLiteral("跳转失败：当前没有数据"));
            return false;
        }

        const std::uint64_t last = lastAddress();
        if (absoluteAddress < m_baseAddress || absoluteAddress > last)
        {
            const int digits = hexview_format::AddressDigitsFor(m_baseAddress, last);
            showStatusMessage(
                HexViewStatusBar::Kind::Warning,
                QStringLiteral("跳转失败：地址 %1 超出数据范围 [%2, %3]")
                    .arg(hexcanvas_format::FormatAddress(absoluteAddress, digits))
                    .arg(hexcanvas_format::FormatAddress(m_baseAddress, digits))
                    .arg(hexcanvas_format::FormatAddress(last, digits)));
            return false;
        }

        // 选中该字节并居中显示。
        m_canvas->setCaretAddress(absoluteAddress, false, false);
        m_canvas->scrollToAddress(absoluteAddress, HexCanvas::ScrollAlign::Center);
        return true;
    }
}
