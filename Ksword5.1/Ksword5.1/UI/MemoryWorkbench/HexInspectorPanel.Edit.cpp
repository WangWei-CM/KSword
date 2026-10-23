// HexInspectorPanel.Edit.cpp
// 作用：数据解释器面板的行内编辑——能否编辑的判定、Enter 提交时的编码与暂存、错误显示。
//
// 铁律（与画布一致）：
// - 本文件没有任何一次目标内存读写，也不直接碰 MemoryDiffOverlay。写入只经画布公开的
//   HexCanvas::stageBytes 暂存（它与键盘编辑、粘贴、填充共用同一套只读/范围/未加载检查与 Stage 调用），
//   真正写入由宿主的写事务负责。
// - 任何失败都要给出原因：编码失败、画布预检失败、Stage 拒绝都显示在状态条，不静默。
// - 信号分工（宿主可以同时连画布与面板而不会重复收到同一件事）：
//   * 暂存成功只有画布的 editStaged（stageBytes 发出），面板没有自己的 editStaged；
//   * 画布 stageBytes 拒绝的原因，画布已经发了 editRejected，面板只显示、不再发第二遍；
//   * 面板自己产生的拒绝（编码失败、不能开始编辑）才由面板的 editRejected 发出。

#include "HexInspectorPanel.h"
#include "HexInspectorWidgets.h"

#include <QByteArray>

#include <string_view>

namespace ks::ui
{
    namespace
    {
        // EncodeFailureText：把 EncodeValue 的失败状态翻译成给用户看的中文，并带上"该键入什么"的提示。
        // 传入：状态与行数据（类型名决定提示措辞）；传出：原因文本，Ok 返回空串。
        QString EncodeFailureText(ksword::memwb::EncodeStatus status, const HexInspectorRowData& row)
        {
            switch (status)
            {
            case ksword::memwb::EncodeStatus::BadNumber:
                if (row.typeKey == QStringLiteral("f32") || row.typeKey == QStringLiteral("f64"))
                {
                    return QStringLiteral("不是合法的浮点数，可以输入 1.5、-2e3、NaN 或 Inf");
                }
                if (row.typeKey == QStringLiteral("ptr"))
                {
                    return QStringLiteral("不是合法的指针值，请输入十进制或 0x 开头的十六进制");
                }
                return QStringLiteral("不是合法的整数，请输入十进制或 0x 开头的十六进制");
            case ksword::memwb::EncodeStatus::OutOfRange:
                return QStringLiteral("超出 %1 的取值范围").arg(row.typeName);
            case ksword::memwb::EncodeStatus::Overflow:
                return QStringLiteral("数值超过 64 位，放不进任何整数类型");
            case ksword::memwb::EncodeStatus::UnknownType:
                return QStringLiteral("该类型不支持编辑");
            case ksword::memwb::EncodeStatus::Ok:
                break;
            }
            return QString();
        }
    }

    // 某行当前为什么不能编辑；空串表示可以。
    // 判定顺序：没有画布/没有数据 -> 画布只读 -> 类型本身只读 -> 字节不足。
    QString HexInspectorPanel::editBlockedReason(int row) const
    {
        if (row < 0 || row >= m_rows->rowCount())
        {
            return QStringLiteral("没有这一行");
        }
        HexCanvas* canvasPointer = m_canvas.data();
        if (canvasPointer == nullptr || !m_window.hasData)
        {
            return QStringLiteral("没有数据，不能编辑");
        }
        if (!canvasPointer->isEditable() || canvasPointer->overlay() == nullptr)
        {
            return QStringLiteral("当前为只读视图，不能编辑");
        }
        const HexInspectorRowData& rowData = m_rows->rowAt(row);
        if (!rowData.editable)
        {
            return QStringLiteral("%1 是只读类型，不能直接编辑").arg(rowData.typeName);
        }
        if (!rowData.available)
        {
            return QStringLiteral("该行的字节不足，不能编辑（悬停该行查看原因）");
        }
        return QString();
    }

    // 双击 / Enter / F2 激活某行。
    void HexInspectorPanel::onRowActivated(int row)
    {
        beginEditRow(row);
    }

    // 打开行内编辑器。
    bool HexInspectorPanel::beginEditRow(int row)
    {
        // contentChanged 是排队信号；开始输入前先同步来源，不能对尚未刷新显示的旧目标编辑。
        refresh(false);
        // 被拒绝时把原因显示在状态条并发信号：只读视图下"双击没反应"是最糟的体验。
        const QString reason = editBlockedReason(row);
        if (!reason.isEmpty())
        {
            m_status->setMessage(HexInspectorStatusBar::Kind::Error, reason);
            emit editRejected(reason);
            return false;
        }

        // 记下编辑开始时的插入点：提交时写到这里，即使期间窗口刷新也不会写偏。
        const HexInspectorRowData& rowData = m_rows->rowAt(row);
        m_status->clearMessage();
        m_editRow = row;
        m_editAddress = m_window.address;
        m_editSourceRevision = m_window.sourceRevision;
        m_editOverlay = m_window.overlay;
        m_editIdentityKey = m_window.identityKey;
        return m_rows->beginEdit(row, rowData.valueCopy);
    }

    // Enter 提交：先编码，再暂存；任何一步失败都保持编辑器打开并显示原因。
    void HexInspectorPanel::onEditCommitted(int row, const QString& text)
    {
        if (row != m_editRow || !m_rows->isEditing())
        {
            return;
        }
        // 复制一份行数据：后面刷新会重建行列表，引用会失效。
        const HexInspectorRowData rowData = m_rows->rowAt(row);

        // 编码：类型名是固定英文键，文本按 UTF-8 交给解释器逻辑层。
        const QByteArray typeBytes = rowData.typeKey.toLatin1();
        const QByteArray textBytes = text.toUtf8();
        std::vector<std::uint8_t> bytes;
        const ksword::memwb::EncodeStatus status = ksword::memwb::EncodeValue(
            std::string_view(typeBytes.constData(), static_cast<std::size_t>(typeBytes.size())),
            std::string_view(textBytes.constData(), static_cast<std::size_t>(textBytes.size())),
            m_order,
            m_pointerWidth,
            bytes);
        if (status != ksword::memwb::EncodeStatus::Ok)
        {
            showEditError(EncodeFailureText(status, rowData), true);
            return;
        }
        stageEdit(rowData, bytes, text);
    }

    // Esc 或点到别处取消：不产生任何暂存，清掉上一次的错误消息。
    void HexInspectorPanel::onEditCancelled(int row)
    {
        Q_UNUSED(row);
        m_editRow = -1;
        if (m_status->kind() == HexInspectorStatusBar::Kind::Error)
        {
            m_status->clearMessage();
        }
    }

    // 暂存一次编辑。
    // 传入：行数据（取类型名）、编码好的字节、用户键入的文本（用于结果消息）。
    // 传出：true 表示已关闭编辑器（含"值没有变化"）；false 表示被拒绝，原因已显示、编辑器保持打开。
    // 流程：先看新值是否与画布上显示的当前值完全相同（相同则不暂存、不发任何画布信号）；
    // 否则调用画布的 stageBytes——只读/范围/未加载/叠加层窗口等全部检查与原因文案都在那里。
    bool HexInspectorPanel::stageEdit(
        const HexInspectorRowData& row,
        const std::vector<std::uint8_t>& bytes,
        const QString& typedText)
    {
        // 活体检查：画布可能在编辑期间被换掉，提交时再确认一次。
        HexCanvas* canvasPointer = m_canvas.data();
        if (canvasPointer == nullptr)
        {
            showEditError(QStringLiteral("没有关联的十六进制视图，不能编辑"), true);
            return false;
        }
        // 再次核对冻结的来源：新目标可能尚未发出排队的 contentChanged，不能仅比较地址/字节。
        const auto* currentOverlay = canvasPointer->overlay();
        if (canvasPointer->sourceRevision() != m_editSourceRevision || currentOverlay != m_editOverlay
            || currentOverlay == nullptr || currentOverlay->IdentityKey() != m_editIdentityKey)
        {
            m_rows->endEdit();
            m_editRow = -1;
            refresh(true);
            showEditError(QStringLiteral("目标进程或访问后端已改变，请重新读取后再应用改动。"), true);
            return false;
        }

        // 当前值：目标范围内每个字节的"所见值"。只有在画布允许编辑、且每个字节都有值并且与新值逐字节相同时，
        // 才判定为"值没有变化"（只读或有字节看不到时，交给 stageBytes 去给出对应的拒绝原因）。
        const std::uint64_t editAddress = m_editAddress;
        bool unchanged = canvasPointer->isEditable() && canvasPointer->overlay() != nullptr;
        for (std::size_t offset = 0; unchanged && offset < bytes.size(); ++offset)
        {
            const HexCanvas::CellState cell = canvasPointer->cellStateAt(editAddress + static_cast<std::uint64_t>(offset));
            unchanged = cell.hasValue && cell.value == bytes[offset];
        }

        if (!unchanged)
        {
            QByteArray payload(reinterpret_cast<const char*>(bytes.data()), static_cast<qsizetype>(bytes.size()));
            const QPointer<HexInspectorPanel> self(this);
            const QPointer<HexCanvas> source(canvasPointer);
            const auto stagedRevision = m_editSourceRevision;
            const auto* stagedOverlay = m_editOverlay;
            const std::string stagedIdentity = m_editIdentityKey;
            const int editedRow = m_editRow;
            const auto resolved = [self, source, stagedRevision, stagedOverlay, stagedIdentity,
                editedRow, row, typedText, editAddress, length = bytes.size()](bool ok,const QString& reason)
            {
                if (!self || !source || self->m_canvas != source || source->sourceRevision() != stagedRevision
                    || source->overlay() != stagedOverlay || !stagedOverlay || stagedOverlay->IdentityKey() != stagedIdentity)
                    return;
                self->m_rows->endEdit(); self->m_editRow = -1;
                self->refresh(true);
                if (!self) return;
                if (ok)
                {
                    self->m_status->setMessage(HexInspectorStatusBar::Kind::Info,
                        QStringLiteral("已暂存 %1 = %2（%3 个字节，地址 0x%4）")
                            .arg(row.typeName).arg(typedText.trimmed()).arg(length)
                            .arg(QString::number(editAddress,16).toUpper()));
                    return;
                }
                // A CPU-stage failure is still an editing failure: restore
                // exactly the submitted text, never claim it was staged.
                if (source->isEditable() && self->m_rows->beginEdit(editedRow,typedText))
                {
                    self->m_editRow = editedRow;
                    self->m_editAddress = editAddress;
                    self->m_editSourceRevision = stagedRevision;
                    self->m_editOverlay = stagedOverlay;
                    self->m_editIdentityKey = stagedIdentity;
                }
                self->showEditError(reason,false);
            };
            QString reason;
            // Callback is synchronous for legacy hosts and completion-based
            // for a workbench. Success text waits for actual value projection.
            return canvasPointer->stageBytes(editAddress,payload,&reason,{},resolved);
        }

        m_rows->endEdit();
        m_editRow = -1;
        refresh(true);
        m_status->setMessage(HexInspectorStatusBar::Kind::Info, QStringLiteral("值没有变化，没有产生暂存"));
        return true;
    }

    // 显示编辑错误：编辑器边框变红、状态条显示原因；emitSignal 为真时再发面板自己的 editRejected。
    // 传入：原因文本、是否发信号（画布 stageBytes 的拒绝画布已经发过，传 false 避免重复）。编辑器保持打开。
    void HexInspectorPanel::showEditError(const QString& reason, bool emitSignal)
    {
        m_rows->setEditError(true);
        m_status->setMessage(HexInspectorStatusBar::Kind::Error, reason);
        if (emitSignal)
        {
            emit editRejected(reason);
        }
    }
}
