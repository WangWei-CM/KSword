// HexView.Panels.cpp
// 作用：HexView 的面板接线——查找条、跳转条、解释器面板、快捷键、查找高亮、导出、主题刷新。
//
// 快捷键（全部 Qt::WidgetWithChildrenShortcut 上下文，只在焦点位于本控件或其子控件内时触发）：
//   Ctrl+F 查找、Ctrl+G 跳转、F3 / Shift+F3 下一个 / 上一个、Esc 关闭当前面板。
// Esc 只在有条打开时启用：两个条都关着时它是禁用的，Esc 落到画布自己的处理（取消半字节 / 折叠选区）。

#include "HexView.h"

#include "../../theme.h"
#include "HexCanvasFormat.h"
#include "HexInspectorPanel.h"
#include "HexViewFormat.h"
#include "HexViewSettings.h"

#include <QApplication>
#include <QEvent>
#include <QKeySequence>
#include <QPointer>
#include <QShowEvent>
#include <QShortcut>
#include <QSplitter>

#include <algorithm>

namespace ks::ui
{
    // 搭建面板层：连接查找条与跳转条，创建快捷键，恢复解释器面板偏好。
    void HexView::buildPanels()
    {
        // 查找条的两个回调：数据取缓冲的隐式共享拷贝，选区取画布选区。
        m_findBar->setSourceGetter([this]() {
            HexFindBar::SourceView view;
            view.data = m_buffer;
            view.base = m_baseAddress;
            return view;
        });
        m_findBar->setSelectionGetter([this](std::uint64_t* firstOut, std::uint64_t* lastOut) {
            const std::optional<HexCanvas::AddressRange> range = m_canvas->selectedRange();
            if (!range.has_value())
            {
                return false;
            }
            *firstOut = range->first;
            *lastOut = range->last;
            return true;
        });
        connect(m_findBar, &HexFindBar::matchFound, this, &HexView::onFindMatch);
        connect(m_findBar, &HexFindBar::highlightsChanged, this,
            [this](const std::vector<HexFindBar::AddressRange>& ranges) {
                m_findRanges = ranges;
                applyFindHighlights();
            });
        connect(m_findBar, &HexFindBar::closeRequested, this, [this]() { closeFindBar(); });
        connect(m_gotoBar, &HexGotoBar::gotoRequested, this, &HexView::onGotoRequested);
        connect(m_gotoBar, &HexGotoBar::closeRequested, this, [this]() { closeGotoBar(); });

        // 快捷键：统一的创建辅助，全部限定在本控件及其子控件内。
        const auto makeShortcut = [this](const QKeySequence& sequence) {
            QShortcut* shortcut = new QShortcut(sequence, this);
            shortcut->setContext(Qt::WidgetWithChildrenShortcut);
            return shortcut;
        };
        connect(makeShortcut(QKeySequence(Qt::CTRL | Qt::Key_F)), &QShortcut::activated, this, [this]() { openFind(); });
        connect(makeShortcut(QKeySequence(Qt::CTRL | Qt::Key_G)), &QShortcut::activated, this, [this]() { openGoto(); });
        connect(makeShortcut(QKeySequence(Qt::Key_F3)), &QShortcut::activated, this, [this]() { onFindNextShortcut(true); });
        connect(makeShortcut(QKeySequence(Qt::SHIFT | Qt::Key_F3)), &QShortcut::activated, this,
            [this]() { onFindNextShortcut(false); });

        // Esc：只在有条打开时启用（见文件头）。
        m_escapeShortcut = makeShortcut(QKeySequence(Qt::Key_Escape));
        m_escapeShortcut->setEnabled(false);
        connect(m_escapeShortcut, &QShortcut::activated, this, [this]() { onEscape(); });

        // 解释器面板：按偏好恢复显隐（读失败退回 false，见 HexViewSettings.h）。
        if (hexview_settings::LoadInspectorVisible())
        {
            applyInspectorVisible(true);
        }
    }

    // ======================== 查找 / 跳转条的打开与关闭 ========================

    // 打开查找条。
    void HexView::openFind()
    {
        m_findBar->open();
        updateEscapeShortcut();
    }

    // 打开跳转条。
    void HexView::openGoto()
    {
        m_gotoBar->open();
        updateEscapeShortcut();
    }

    // 关闭查找条：取消搜索、清高亮、隐藏；焦点原先在条内就还给画布（条隐藏后 Qt 会把焦点丢给别处）。
    void HexView::closeFindBar()
    {
        QWidget* focus = QApplication::focusWidget();
        const bool hadFocus = (focus != nullptr) && m_findBar->isAncestorOf(focus);
        m_findBar->deactivate();
        m_findBar->hide();
        updateEscapeShortcut();
        if (hadFocus)
        {
            m_canvas->setFocus(Qt::OtherFocusReason);
        }
    }

    // 关闭跳转条：同上。
    void HexView::closeGotoBar()
    {
        QWidget* focus = QApplication::focusWidget();
        const bool hadFocus = (focus != nullptr) && m_gotoBar->isAncestorOf(focus);
        m_gotoBar->hide();
        updateEscapeShortcut();
        if (hadFocus)
        {
            m_canvas->setFocus(Qt::OtherFocusReason);
        }
    }

    // Esc 快捷键只在至少有一个条可见时启用。
    void HexView::updateEscapeShortcut()
    {
        const bool anyOpen = m_findBar->isVisibleTo(this) || m_gotoBar->isVisibleTo(this);
        m_escapeShortcut->setEnabled(anyOpen);
    }

    // Esc：焦点在跳转条里先关跳转条；否则先关查找条，再关跳转条。
    void HexView::onEscape()
    {
        QWidget* focus = QApplication::focusWidget();
        const bool gotoOpen = m_gotoBar->isVisibleTo(this);
        const bool findOpen = m_findBar->isVisibleTo(this);
        if (gotoOpen && focus != nullptr && m_gotoBar->isAncestorOf(focus))
        {
            closeGotoBar();
            return;
        }
        if (findOpen)
        {
            closeFindBar();
            return;
        }
        if (gotoOpen)
        {
            closeGotoBar();
        }
    }

    // F3 / Shift+F3：查找条没开先打开；输入框里没有文字就停在打开（让用户输入）。
    void HexView::onFindNextShortcut(bool forward)
    {
        if (!m_findBar->isVisibleTo(this))
        {
            openFind();
            if (m_findBar->patternText().isEmpty())
            {
                return;
            }
        }
        if (forward)
        {
            m_findBar->findNext();
        }
        else
        {
            m_findBar->findPrevious();
        }
    }

    // ======================== 查找命中与高亮 ========================

    // 命中：选中命中范围（插入点在命中起点、锚点在末字节），不在视口内就滚动到可见。
    void HexView::onFindMatch(quint64 first, quint64 length, bool wrapped)
    {
        Q_UNUSED(wrapped);
        if (length == 0)
        {
            return;
        }
        const std::uint64_t last = first + (length - 1ULL);
        m_canvas->setCaretAddress(last, false, false);
        m_canvas->setCaretAddress(first, true, false);

        // 已经整体可见就保持不动（Nearest），否则居中，避免命中贴着视口边缘。
        const bool visible = m_visibleValid && first >= m_visibleFirst && last <= m_visibleLast;
        m_canvas->scrollToAddress(first, visible ? HexCanvas::ScrollAlign::Nearest : HexCanvas::ScrollAlign::Center);
    }

    // 把当前查找高亮区间交给画布；为空则清除该层。颜色每次现取，主题变化后重新调用即可。
    void HexView::applyFindHighlights()
    {
        if (m_findRanges.empty())
        {
            m_canvas->clearHighlightRanges(kFindHighlightLayer);
            return;
        }
        m_canvas->setHighlightRanges(
            kFindHighlightLayer,
            m_findRanges,
            KswordTheme::AccentColor(KswordTheme::AccentRole::Yellow),
            QStringLiteral("查找命中"));
    }

    // 主题变化：高亮色是在设置时取的，调色板变化后需要重新取。
    void HexView::changeEvent(QEvent* event)
    {
        QWidget::changeEvent(event);
        if (event != nullptr
            && (event->type() == QEvent::PaletteChange || event->type() == QEvent::ApplicationPaletteChange)
            && !m_findRanges.empty())
        {
            applyFindHighlights();
        }
    }

    // 跳转条请求跳转：选中目标字节并居中，状态条给出确认。
    void HexView::onGotoRequested(quint64 address)
    {
        m_canvas->setCaretAddress(address, false, false);
        m_canvas->scrollToAddress(address, HexCanvas::ScrollAlign::Center);
        const int digits = hexview_format::AddressDigitsFor(m_baseAddress, m_buffer.isEmpty() ? m_baseAddress : lastAddress());
        showStatusMessage(
            HexViewStatusBar::Kind::Info,
            QStringLiteral("已跳转到 %1").arg(hexcanvas_format::FormatAddress(address, digits)));
    }

    // 状态条显示一条瞬时消息。
    void HexView::showStatusMessage(HexViewStatusBar::Kind kind, const QString& text)
    {
        m_status->setMessage(kind, text);
    }

    // ======================== 解释器面板 ========================

    // 按需创建解释器面板：放在分割器第二格，初始隐藏。
    void HexView::ensureInspector()
    {
        if (m_inspector != nullptr)
        {
            return;
        }
        m_inspector = new HexInspectorPanel(m_splitter);
        m_inspector->setCanvas(m_canvas);
        m_splitter->addWidget(m_inspector);
        m_splitter->setStretchFactor(0, 1);
        m_splitter->setStretchFactor(1, 0);
        m_inspector->hide();
    }

    // 解释器面板（首次调用创建）。
    HexInspectorPanel* HexView::panel()
    {
        ensureInspector();
        return m_inspector;
    }

    // 应用显隐：显示时按建议宽度给面板一块初始宽度（不超过分割器宽度的 45%），之后宽度由用户拖动；同步开关按钮的勾选状态。
    void HexView::applyInspectorVisible(bool visible)
    {
        if (visible)
        {
            ensureInspector();
            m_inspector->show();
            const int total = m_splitter->width() > 0 ? m_splitter->width() : 900;
            const int panelWidth = (std::min)(m_inspector->sizeHint().width(), total * 45 / 100);
            m_splitter->setSizes(QList<int>{ (std::max)(1, total - panelWidth), panelWidth });
        }
        else if (m_inspector != nullptr)
        {
            m_inspector->hide();
        }
        m_inspectorShown = visible;

        // 勾选状态与实际显隐保持一致；屏蔽信号，避免代码调用被当成"用户点按钮"而写偏好。
        if (m_inspectorButton->isChecked() != visible)
        {
            const QSignalBlocker blocker(m_inspectorButton);
            m_inspectorButton->setChecked(visible);
        }
    }

    // 用户点了工具栏开关：显隐并持久化。
    void HexView::onInspectorToggled(bool checked)
    {
        applyInspectorVisible(checked);
        hexview_settings::SaveInspectorVisible(checked);
    }

    // 代码调用：只显隐，不写偏好。
    void HexView::setInspectorVisible(bool visible)
    {
        applyInspectorVisible(visible);
    }

    // 解释器面板是否显示。
    bool HexView::inspectorVisible() const
    {
        return m_inspectorShown;
    }

    // ======================== 导出 ========================

    // Freeze the already acquired complete value; never fetch a new target range.
    bool HexView::captureExport(hexexport::Kind kind, hexexport::ExportSnapshot& value, QString& error) const
    {
        value.kind = kind;
        value.bytes = m_buffer;
        value.baseAddress = m_baseAddress;
        value.sourceVersion = m_bufferGeneration;
        value.offset = 0;
        value.length = m_buffer.size();
        if (kind == hexexport::Kind::SelectedHex)
        {
            std::uint64_t first = 0;
            std::uint64_t last = 0;
            if (!selectionRange(first, last) || first >= static_cast<std::uint64_t>(m_buffer.size()) || last <= first)
            {
                error = QStringLiteral("导出失败：未选中有效字节");
                return false;
            }
            const auto end = (std::min)(last, static_cast<std::uint64_t>(m_buffer.size()));
            value.offset = static_cast<qsizetype>(first);
            value.length = static_cast<qsizetype>(end - first);
        }
        else if (m_buffer.isEmpty())
        {
            error = QStringLiteral("导出失败：当前无数据");
            return false;
        }
        return true;
    }

    void HexView::finishExport(bool ok, const QString& message)
    {
        const QPointer<HexView> guard(this);
        showStatusMessage(ok ? HexViewStatusBar::Kind::Info : HexViewStatusBar::Kind::Error, message);
        if (guard) emit exportFinished(ok, message);
    }

    // Existing direct callers require an actual completion result, not admission.
    // They share bounded atomic serialization; all interactive actions use startExport.
    bool HexView::runExport(hexexport::Kind kind, const QString& path, QString* messageOut)
    {
        hexexport::ExportSnapshot value;
        QString message;
        bool ok = captureExport(kind, value, message);
        if (ok)
        {
            QString error;
            ok = hexexport::WriteSnapshotFile(path, value, &error);
            if (!ok) message = QStringLiteral("导出失败：%1").arg(error);
            else if (kind == hexexport::Kind::Binary)
                message = QStringLiteral("导出完成：%1 字节 -> %2").arg(static_cast<qulonglong>(value.length)).arg(path);
            else if (kind == hexexport::Kind::HexDump)
                message = QStringLiteral("导出完成：十六进制文本 -> %1").arg(path);
            else
                message = QStringLiteral("导出完成：%1 字节十六进制数据 -> %2").arg(static_cast<qulonglong>(value.length)).arg(path);
        }
        if (messageOut) *messageOut = message;
        finishExport(ok, message);
        return ok;
    }

    bool HexView::startExport(std::shared_ptr<const hexexport::ExportSnapshot> value, const QString& path)
    {
        if (!m_exportState)
        {
            m_exportState = std::make_unique<ExportState>();
            m_exportState->operation = std::make_unique<CompleteExportOperation>(this);
        }
        const QPointer<HexView> guard(this);
        m_exportState->operation->reconcile();
        if (!guard || m_exportState->operation->isBusy()) return false;

        const auto kind = value->kind;
        const auto length = value->length;
        const auto sourceVersion = value->sourceVersion;
        const auto identity = QString::number(static_cast<qulonglong>(value->baseAddress), 16)
            + QChar(u':') + QString::number(value->offset) + QChar(u':') + QString::number(value->length);
        const auto control = value->control;
        auto input = MakeCompleteExportInput(value, identity, sourceVersion, std::nullopt,
            ks::i18n::LanguageManager::instance().sourceTextSnapshot(),
            [](const hexexport::ExportSnapshot& frozen, CompleteExportSink& sink, const ks::i18n::SourceTextSnapshot&) {
                return hexexport::SerializeSnapshot(frozen, sink);
            });
        input.fileMode = kind == hexexport::Kind::Binary ? CompleteExportFileMode::Binary : CompleteExportFileMode::NativeText;
        input.utf8Bom = false;
        auto* operation = m_exportState->operation.get();
        const auto journal = operation->journal();
        m_exportState->control = control;
        const auto accepted = operation->startFile(path, std::move(input),
            [guard, control, journal, kind, length, path](const CompleteExportReceipt& receipt) {
                if (!guard) return;
                const bool unknown = receipt.phase == CompleteExportPhase::OutcomeUnavailable;
                const bool canceled = !unknown && control->phase.load() == hexexport::ExportControl::Phase::Canceled;
                const bool ok = receipt.succeeded();
                QString message;
                if (unknown)
                    message = QStringLiteral("导出结果尚未确认，请核对目标文件：%1").arg(path);
                else if (canceled)
                    message = QStringLiteral("导出已取消");
                else if (ok && kind == hexexport::Kind::Binary)
                    message = QStringLiteral("导出完成：%1 字节 -> %2").arg(static_cast<qulonglong>(length)).arg(path);
                else if (ok && kind == hexexport::Kind::HexDump)
                    message = QStringLiteral("导出完成：十六进制文本 -> %1").arg(path);
                else if (ok)
                    message = QStringLiteral("导出完成：%1 字节十六进制数据 -> %2").arg(static_cast<qulonglong>(length)).arg(path);
                else
                {
                    QString error = QStringLiteral("未开始写入文件，请重试导出。");
                    if (receipt.error == CompleteExportError::OpenFailed)
                        error = QStringLiteral("无法写入文件：%1（%2）").arg(path, receipt.nativeError);
                    else if (receipt.error == CompleteExportError::WriteFailed || receipt.error == CompleteExportError::FlushFailed
                        || receipt.error == CompleteExportError::CommitFailed)
                        error = QStringLiteral("写入不完整：%1（%2）").arg(path, receipt.nativeError);
                    message = QStringLiteral("导出失败：%1").arg(error);
                }
                if (!unknown && receipt.terminal()) journal->acknowledge(receipt.id);
                if (guard->m_exportState && guard->m_exportState->control == control) guard->m_exportState->control.reset();
                guard->finishExport(ok, message);
                if (guard && !ok && !canceled) hexexport::ShowExportError(guard, message);
            });
        if (!guard) return accepted != 0;
        if (accepted == 0)
        {
            if (m_exportState->control == control) m_exportState->control.reset();
            return false;
        }
        if (m_exportState->control == control)
            showStatusMessage(HexViewStatusBar::Kind::Info, QStringLiteral("正在准备完整导出；再次点击可取消准备。"));
        return true;
    }

    // The next export action cancels only preparation; an admitted commit finishes.
    void HexView::runExportWithDialog(hexexport::Kind kind)
    {
        const QPointer<HexView> guard(this);
        if (m_exportState)
        {
            m_exportState->operation->reconcile();
            if (!guard) return;
            if (m_exportState->operation->isBusy())
            {
                const auto control = m_exportState->control;
                const bool canceled = control && (control->cancelPreparation()
                    || control->phase.load() == hexexport::ExportControl::Phase::Canceled);
                showStatusMessage(HexViewStatusBar::Kind::Info,
                    canceled ? QStringLiteral("导出已取消") : QStringLiteral("导出进行中"));
                return;
            }
        }
        hexexport::ExportSnapshot value;
        QString error;
        if (!captureExport(kind, value, error))
        {
            finishExport(false, error);
            return;
        }
        // Retain before the nested dialog: cancel/close also retires a replaced source off GUI.
        value.control = std::make_shared<hexexport::ExportControl>();
        auto snapshot = RetainCompleteExportSnapshot<hexexport::ExportSnapshot>(
            std::make_shared<const hexexport::ExportSnapshot>(std::move(value)));
        const QString path = hexexport::PickSavePath(this, kind);
        if (!guard || path.trimmed().isEmpty()) return;
        if (m_exportState)
        {
            m_exportState->operation->reconcile();
            if (!guard) return;
            if (m_exportState->operation->isBusy()) return;
        }
        if (!startExport(std::move(snapshot), path) && guard)
        {
            const auto message = QStringLiteral("未开始写入文件，请重试导出。");
            finishExport(false, message);
            if (guard) hexexport::ShowExportError(this, message);
        }
    }

    void HexView::showEvent(QShowEvent* event)
    {
        const QPointer<HexView> guard(this);
        QWidget::showEvent(event);
        if (guard && m_exportState) m_exportState->operation->reconcile();
    }

    // 三个不弹对话框的导出。
    bool HexView::exportBinaryTo(const QString& path)
    {
        return runExport(hexexport::Kind::Binary, path);
    }

    bool HexView::exportHexTextTo(const QString& path)
    {
        return runExport(hexexport::Kind::HexDump, path);
    }

    bool HexView::exportSelectedHexTo(const QString& path)
    {
        return runExport(hexexport::Kind::SelectedHex, path);
    }

    // 三个带对话框的导出槽。
    void HexView::exportBinary()
    {
        runExportWithDialog(hexexport::Kind::Binary);
    }

    void HexView::exportHexText()
    {
        runExportWithDialog(hexexport::Kind::HexDump);
    }

    void HexView::exportSelectedHex()
    {
        runExportWithDialog(hexexport::Kind::SelectedHex);
    }
}
