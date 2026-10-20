// HexCanvas.Edit.cpp
// 作用：HexCanvas 的编辑侧——公开的暂存入口 stageBytes、半字节流式输入、ASCII 直接写入、
// 粘贴、选区填充、Backspace 回退。
//
// 铁律（不变式 1）：
// - 本文件里没有任何一次内存读写。所有编辑都落到 MemoryDiffOverlay::Stage 这一个入口，
//   真正写入目标内存由宿主的写事务负责。
// - 所有编辑手势（键盘、粘贴、填充）以及解释器面板的行内编辑都经过公开的 stageBytes，
//   只读/范围/未加载检查与"暂存成功"信号因此只有一处。
// - 只读模式（未 setEditable 或没有 overlay）下，按键输入静默忽略，不产生任何暂存；
//   填充/粘贴这类显式命令会发 editRejected 告诉用户原因。

#include "HexCanvas.h"
#include "HexCanvasFormat.h"

#include <QClipboard>
#include <QGuiApplication>
#include <QPointer>

#include <algorithm>
#include <limits>

namespace ks::ui
{
    namespace
    {
        // StageReasonText：把暂存失败原因翻译成给用户看的中文。
        // 传入：暂存状态；传出：原因文本，Ok 返回空串。
        QString StageReasonText(ksword::memwb::StageStatus status)
        {
            switch (status)
            {
            case ksword::memwb::StageStatus::Empty:
                return QStringLiteral("没有可写入的字节");
            case ksword::memwb::StageStatus::AddressOverflow:
                return QStringLiteral("写入范围超出 64 位地址空间");
            case ksword::memwb::StageStatus::OutOfWindow:
                return QStringLiteral("写入范围超出当前已读取的数据窗口");
            case ksword::memwb::StageStatus::UnreadBytes:
                return QStringLiteral("写入范围内含有尚未读取到的字节，不能编辑");
            case ksword::memwb::StageStatus::TooLarge:
                return QStringLiteral("暂存的修改总量超过上限");
            case ksword::memwb::StageStatus::Ok:
                break;
            }
            return QString();
        }

        // HexDigitOf：把字符转成 0..15 的十六进制数字值。
        // 传入：字符；传出：数值，不是十六进制数字字符返回 -1。
        int HexDigitOf(QChar ch)
        {
            const char16_t code = ch.unicode();
            if (code >= u'0' && code <= u'9')
            {
                return static_cast<int>(code - u'0');
            }
            if (code >= u'a' && code <= u'f')
            {
                return static_cast<int>(code - u'a') + 10;
            }
            if (code >= u'A' && code <= u'F')
            {
                return static_cast<int>(code - u'A') + 10;
            }
            return -1;
        }

        // ReadOnlyReason：只读视图下显式命令被拒绝的原因（函数内静态，避免重复构造）。
        const QString& ReadOnlyReason()
        {
            static const QString reason = QStringLiteral("当前为只读视图，不能编辑");
            return reason;
        }
    }

    // 取消尚未完成的半字节。
    void HexCanvas::cancelNibble()
    {
        if (!m_nibbleActive)
        {
            return;
        }
        m_nibbleActive = false;
        m_nibbleHigh = 0;
        m_nibbleAddress = 0;
        viewport()->update();
    }

    // 发出编辑被拒绝信号。
    void HexCanvas::rejectEdit(const QString& reason)
    {
        emit editRejected(reason);
    }

    // 暂存前的预检：不碰叠加层，只回答"这段字节现在能不能编辑"。
    // 传入：起始地址、字节数、原因输出（可为空指针）；传出：是否允许。原因可直接给用户看。
    // 检查顺序：只读 -> 空 -> 64 位溢出 -> 范围在地址空间内 -> 每个字节"屏幕上看得到值"。
    // 最后一项逐字节、遇到第一个没有值的字节就早退，所以耗时最多与页缓存容量（约 1 MiB）成正比，
    // 不会因为一个巨大的选区而卡住界面。
    bool HexCanvas::precheckStage(std::uint64_t address, std::uint64_t length, QString* reasonOut) const
    {
        const auto fail = [reasonOut](const QString& reason) {
            if (reasonOut != nullptr)
            {
                *reasonOut = reason;
            }
            return false;
        };

        // 只读：没允许编辑，或者没有暂存叠加层。
        if (!m_editable || m_overlay == nullptr)
        {
            return fail(ReadOnlyReason());
        }
        if (length == 0)
        {
            return fail(QStringLiteral("没有可写入的字节"));
        }

        // 溢出与范围：先比较后加减，终点 = address + (length - 1) 不会回绕。
        // 单字节的文案沿用"该地址/该字节"，多字节说"写入范围"。
        const bool single = (length == 1ULL);
        if (length - 1ULL > std::numeric_limits<std::uint64_t>::max() - address)
        {
            return fail(QStringLiteral("写入范围超出 64 位地址空间"));
        }
        const std::uint64_t last = address + (length - 1ULL);
        if (!m_hasSpace || !m_viewport.ContainsAddress(address) || !m_viewport.ContainsAddress(last))
        {
            return fail(single
                ? QStringLiteral("该地址不在可编辑的地址空间内")
                : QStringLiteral("写入范围超出了地址空间"));
        }

        // A connected workbench validates captured baseline masks on its
        // worker. Avoid a second per-byte scan on GUI for a large fill/paste.
        if (m_stageSubmitter) return true;

        // 逐字节：缓存里读到了就放行；没读到但已有暂存补丁的字节仍然有值可显示（与 resolveCore 同一规则）。
        for (std::uint64_t offset = 0; offset < length; ++offset)
        {
            const std::uint64_t current = address + offset;
            if (m_viewport.PeekByte(current).state == ByteState::Valid)
            {
                continue;
            }
            const CellCore core = resolveCore(current);
            if (core.hasValue)
            {
                continue;
            }

            // 区分"还没读到"和"读过但读不到"，前者稍等即可，后者需要换通道或目标。
            if (core.state == ByteState::Unreadable)
            {
                return fail(single
                    ? QStringLiteral("该字节不可读，不能编辑")
                    : QStringLiteral("写入范围内有字节不可读，不能编辑"));
            }
            return fail(single
                ? QStringLiteral("该字节尚未加载，不能编辑")
                : QStringLiteral("写入范围内有字节尚未加载，不能编辑"));
        }
        return true;
    }

    // 暂存一段字节：公开的唯一写入入口。
    // 传入：起始地址、字节、同步原因输出、成功动作与实际结果回调；传出：是否接受请求。
    // 独立宿主同步 Stage；工作台的值副本在 worker 处理，实际成功回投后再重绘并发 editStaged。
    // reasonOut 只在当前调用栈内写入，异步回调绝不借用调用者的栈指针。
    bool HexCanvas::stageBytes(std::uint64_t address, const QByteArray& bytes, QString* reasonOut,
        std::function<void()> afterStaged, StageCompletion resolved)
    {
        // reject：统一的失败出口，原因同时写给调用方与信号订阅者。
        const auto reject = [this, reasonOut, &resolved](const QString& reason) {
            if (reasonOut != nullptr)
            {
                *reasonOut = reason;
            }
            const QPointer<HexCanvas> self(this);
            rejectEdit(reason);
            if (self && resolved) resolved(false, reason);
            return false;
        };

        QString reason;
        if (!precheckStage(address, static_cast<std::uint64_t>(bytes.size()), &reason))
        {
            return reject(reason);
        }

        if (m_stageSubmitter)
        {
            const QPointer<HexCanvas> self(this);
            const auto submit = m_stageSubmitter;
            const auto completed = [self, overlay = m_overlay, source = sourceRevision(), address, length = static_cast<quint64>(bytes.size()),
                afterStaged = std::move(afterStaged), resolved](bool ok, const QString& failure)
            {
                if (!self) return;
                const auto current = [self, overlay, source] {
                    return self && self->m_overlay == overlay && self->sourceRevision() == source;
                };
                if (!current())
                {
                    if (resolved) resolved(false, QStringLiteral("操作已取消"));
                    return;
                }
                if (!ok)
                {
                    QString visible = failure;
                    if (failure.startsWith(QLatin1StringView("stage rejected (")))
                    {
                        const auto first = failure.lastIndexOf(u'(') + 1;
                        const auto last = failure.lastIndexOf(u')');
                        bool parsed = false;
                        const auto status = failure.mid(first,last-first).toInt(&parsed);
                        if (parsed && status >= static_cast<int>(ksword::memwb::StageStatus::Empty)
                            && status <= static_cast<int>(ksword::memwb::StageStatus::TooLarge))
                            visible = StageReasonText(static_cast<ksword::memwb::StageStatus>(status));
                    }
                    else if (failure == QLatin1StringView("timeout"))
                        visible = StageReasonText(ksword::memwb::StageStatus::UnreadBytes);
                    else if (failure == QLatin1StringView("cancelled"))
                        visible = QStringLiteral("操作已取消");
                    self->rejectEdit(visible);
                    if (self && resolved) resolved(false,visible);
                    return;
                }
                self->viewport()->update();
                self->scheduleContentChanged();
                emit self->editStaged(address, length);
                if (current() && afterStaged) afterStaged();
                if (self && resolved) resolved(current(), current() ? QString() : QStringLiteral("操作已取消"));
            };
            const bool accepted = submit(address, bytes, completed);
            if (!self) return accepted;
            if (!accepted) return reject(ReadOnlyReason());
            if (reasonOut) reasonOut->clear();
            return true;
        }

        // QByteArray -> vector<uint8_t>：MemoryDiffOverlay 是 Qt-free 的，这里做类型适配。
        const std::vector<std::uint8_t> payload(
            reinterpret_cast<const std::uint8_t*>(bytes.constData()),
            reinterpret_cast<const std::uint8_t*>(bytes.constData()) + bytes.size());
        const ksword::memwb::StageStatus status = m_overlay->Stage(address, payload);
        if (status != ksword::memwb::StageStatus::Ok)
        {
            return reject(StageReasonText(status));
        }

        // 成功：先让订阅者能读到最终状态（重绘 + 排队的 contentChanged），再发 editStaged。
        if (reasonOut != nullptr)
        {
            reasonOut->clear();
        }
        viewport()->update();
        scheduleContentChanged();
        const QPointer<HexCanvas> self(this);
        emit editStaged(address, static_cast<quint64>(bytes.size()));
        if (self && afterStaged) afterStaged();
        if (self && resolved) resolved(true,{});
        return true;
    }

    // 编辑完成后前进到下一字节：选区折叠到新插入点并滚动到可见。
    void HexCanvas::advanceAfterEdit()
    {
        const ksword::memwb::HexViewport::Selection before = m_viewport.GetSelection();
        m_viewport.MoveCaretByBytes(1, false);
        applySelectionChange(before);
        ensureCaretVisible();
    }

    // 处理一个输入字符。
    // 传入：字符；传出：是否已消费。
    // Hex 面板：十六进制数字。第一个半字节只记下并显示预览；第二个半字节才暂存并前进。
    // ASCII 面板：可见字符（0x20..0x7E）直接写入当前字节并前进。
    bool HexCanvas::handleTextInput(QChar ch)
    {
        if (!m_hasSpace || !m_editable || m_overlay == nullptr)
        {
            return false;
        }
        const std::uint64_t caret = m_viewport.GetSelection().caret;

        if (m_viewport.Pane() == ActivePane::Hex)
        {
            const int digit = HexDigitOf(ch);
            if (digit < 0)
            {
                return false;
            }

            // 第一个半字节：预检通过后只记状态，不暂存。
            if (!m_nibbleActive)
            {
                QString reason;
                if (!precheckStage(caret, 1ULL, &reason))
                {
                    rejectEdit(reason);
                    return true;
                }
                m_nibbleActive = true;
                m_nibbleHigh = digit;
                m_nibbleAddress = caret;
                viewport()->update();
                return true;
            }

            // 第二个半字节：先清掉半字节状态，再暂存，成功则前进。
            const char value = static_cast<char>((m_nibbleHigh << 4) | digit);
            const std::uint64_t target = m_nibbleAddress;
            cancelNibble();
            const QPointer<HexCanvas> self(this);
            stageBytes(target, QByteArray(1, value), nullptr, [self] { if (self) self->advanceAfterEdit(); });
            return true;
        }

        // ASCII 面板：只接受可见 ASCII，其余字符不消费，交还给基类。
        const char16_t code = ch.unicode();
        if (code < 0x20 || code > 0x7E)
        {
            return false;
        }
        QString reason;
        if (!precheckStage(caret, 1ULL, &reason))
        {
            rejectEdit(reason);
            return true;
        }
        const QPointer<HexCanvas> self(this);
        stageBytes(caret, QByteArray(1, static_cast<char>(code)), nullptr,
            [self] { if (self) self->advanceAfterEdit(); });
        return true;
    }

    // Backspace：有半字节就取消半字节；否则回退一个字节并丢弃该字节上的暂存补丁。
    void HexCanvas::backspaceStep()
    {
        if (!m_hasSpace)
        {
            return;
        }
        if (m_nibbleActive)
        {
            cancelNibble();
            return;
        }
        // 回退并丢弃补丁属于编辑手势，只读模式下不做任何事。
        if (!m_editable || m_overlay == nullptr)
        {
            return;
        }
        const ksword::memwb::HexViewport::Selection before = m_viewport.GetSelection();
        if (before.caret == m_viewport.FirstAddress())
        {
            return;
        }

        m_viewport.MoveCaretByBytes(-1, false);
        applySelectionChange(before);
        const std::uint64_t target = m_viewport.GetSelection().caret;
        if (m_discardSubmitter)
        {
            const QPointer<HexCanvas> self(this);
            const auto* overlay = m_overlay;
            const auto source = sourceRevision();
            const auto submit = m_discardSubmitter;
            submit(target, 1, [self, overlay, source, target](bool ok, std::uint64_t discarded) {
                if (!self || !ok || !discarded || self->m_overlay != overlay || self->sourceRevision() != source) return;
                self->viewport()->update();
                self->scheduleContentChanged();
                emit self->editDiscarded(target, discarded);
            });
            if (self) self->ensureCaretVisible();
            return;
        }
        if (m_overlay->Discard(target, 1) > 0)
        {
            // 丢弃成功：显示值回到基线，通知订阅者后再发 editDiscarded。
            viewport()->update();
            scheduleContentChanged();
            emit editDiscarded(target, 1);
        }
        ensureCaretVisible();
    }

    // 用同一个字节值填充选区。
    // 传入：字节值（00/FF/90 等）。不弹对话框；失败发 editRejected。
    void HexCanvas::fillSelection(quint8 value)
    {
        const std::optional<AddressRange> range = selectedRange();
        if (!range.has_value())
        {
            return;
        }
        if (!m_editable || m_overlay == nullptr)
        {
            rejectEdit(ReadOnlyReason());
            return;
        }

        // 长度 = last - first + 1：整个 64 位空间的全选会让它回绕为 0，先挡住，再与暂存上限比较，
        // 这样不会为一个注定被拒绝的请求分配巨大缓冲。
        const std::uint64_t span = range->last - range->first;
        if (span == std::numeric_limits<std::uint64_t>::max()
            || span + 1ULL > ksword::memwb::kMemoryDiffOverlayMaxPendingBytes)
        {
            rejectEdit(QStringLiteral("选区过大，超过暂存上限"));
            return;
        }

        // 先预检再分配：选区里有没读到的字节时，不去构造一个注定被拒绝的缓冲。
        const std::uint64_t length = span + 1ULL;
        QString reason;
        if (!precheckStage(range->first, length, &reason))
        {
            rejectEdit(reason);
            return;
        }
        const QByteArray bytes(static_cast<qsizetype>(length), static_cast<char>(value));
        stageBytes(range->first, bytes);
    }

    // 把剪贴板内容粘贴到选区起点。
    // Hex 面板按十六进制文本解析（失败整体拒绝）；ASCII 面板按 UTF-8 字节原样写入。
    // 成功后把选区设为刚粘贴的范围，让用户看到改了哪里。
    void HexCanvas::pasteFromClipboard()
    {
        const std::optional<AddressRange> range = selectedRange();
        if (!range.has_value())
        {
            return;
        }
        if (!m_editable || m_overlay == nullptr)
        {
            rejectEdit(ReadOnlyReason());
            return;
        }

        // raw：要写入的字节。
        const QString text = QGuiApplication::clipboard()->text();
        QByteArray raw;
        if (m_viewport.Pane() == ActivePane::Hex)
        {
            QString error;
            if (!hexcanvas_format::ParseHexText(text, &raw, &error))
            {
                rejectEdit(error);
                return;
            }
        }
        else
        {
            raw = text.toUtf8();
            if (raw.isEmpty())
            {
                rejectEdit(QStringLiteral("剪贴板里没有可粘贴的文本"));
                return;
            }
        }

        // 越界检查：从起点到地址空间末尾放不下就整体拒绝，不截断。
        const std::uint64_t start = range->first;
        const std::uint64_t length = static_cast<std::uint64_t>(raw.size());
        if (length - 1ULL > m_viewport.LastAddress() - start)
        {
            rejectEdit(QStringLiteral("粘贴内容超出地址空间末尾"));
            return;
        }

        const QPointer<HexCanvas> self(this);
        stageBytes(start, raw, nullptr, [self, start, length]
        {
            if (!self) return;
            const auto before = self->m_viewport.GetSelection();
            self->m_viewport.SetCaret(start, false);
            self->m_viewport.SetCaret(start + (length - 1ULL), true);
            self->applySelectionChange(before);
            self->ensureCaretVisible();
        });
    }
}
