#include "MemoryRowCanvas.h"
#include "HexCanvasFormat.h"
#include "../../theme.h"
#include "../../Internationalization/LanguageManager.h"
#include <QClipboard>
#include <QContextMenuEvent>
#include <QFontDatabase>
#include <QGuiApplication>
#include <QHelpEvent>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QPainter>
#include <QPointer>
#include <QScrollBar>
#include <QToolTip>
#include <QWheelEvent>
#include <algorithm>
#include <limits>

namespace ks::ui
{
    namespace
    {
        constexpr int kPadding = 8;
        std::uint64_t lastByte(const MemoryDisplayRow& row)
        {
            std::uint64_t length = static_cast<std::uint64_t>(row.bytes.size());
            for (const auto& token : row.tokens)
                if (token.length && token.address >= row.address)
                    length = std::max(length, token.address - row.address + token.length);
            return length && length - 1 <= UINT64_MAX - row.address ? row.address + length - 1 : row.address;
        }
    }

    MemoryRowCanvas::MemoryRowCanvas(QWidget* parent) : QAbstractScrollArea(parent)
    {
        setFrameShape(QFrame::NoFrame);
        setFocusPolicy(Qt::StrongFocus);
        setProperty("ksword_disable_smooth_scroll", true);
        viewport()->setMouseTracking(true);
        baseFont_ = hexcanvas_format::BuildFixedFont();
        setFont(baseFont_);
        setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOn);
        connect(verticalScrollBar(), &QScrollBar::valueChanged, this, [this] {
            viewport()->update();
            notifyVisibleRange();
        });
        connect(horizontalScrollBar(), &QScrollBar::valueChanged, viewport(), qOverload<>(&QWidget::update));
        updateMetrics();
    }

    int MemoryRowCanvas::gutterWidth() const { return branchGutterVisible_ ? 30 : 0; }
    int MemoryRowCanvas::addressWidth() const { return (addressDigits_ + 2) * characterWidth_ + kPadding * 2; }
    int MemoryRowCanvas::bytesWidth() const
    {
        if (!bytesVisible_) return 0;
        if (byteWidthOverride_ >= 0) return std::clamp(byteWidthOverride_, 40, std::max(40, viewport()->width() - addressWidth() - 40));
        if (textPriority_) return std::clamp(viewport()->width() / 4, 60, 240);
        return std::min(maximumRowBytes_ * characterWidth_ * 3 + kPadding * 2, std::max(80, viewport()->width() / 2));
    }
    int MemoryRowCanvas::contentStart() const { return gutterWidth() + addressWidth() + bytesWidth(); }

    QVector<MemoryRowCanvas::TokenHit> MemoryRowCanvas::tokenRects(int row) const
    {
        QVector<TokenHit> result;
        if (row < 0 || row >= rows_.size()) return result;
        int x = contentStart() + kPadding - (wrapContent_ ? 0 : horizontalScrollBar()->value());
        int y = rowRect(row).top();
        const int right = std::max(contentStart() + kPadding + characterWidth_, viewport()->width() - kPadding);
        const QFontMetrics fm(font());
        const auto& tokens = rows_[row].tokens;
        for (int i = 0; i < tokens.size(); ++i)
        {
            const auto& token = tokens[i];
            const int width = std::max(1, fm.horizontalAdvance(token.text));
            if (wrapContent_ && x > contentStart() + kPadding && x + width > right)
            {
                x = contentStart() + kPadding;
                y += lineHeight_;
            }
            const auto last = token.length && token.length - 1 <= UINT64_MAX - token.address
                ? token.address + token.length - 1 : token.address;
            result.push_back({QRect(x, y, width, lineHeight_), token.address, last, i});
            x += width;
        }
        return result;
    }

    int MemoryRowCanvas::rowHeight(int row) const
    {
        if (!wrapContent_ || row < 0 || row >= rows_.size()) return lineHeight_;
        const int width = std::max(characterWidth_, viewport()->width() - contentStart() - kPadding * 2);
        int used = 0, lines = 1;
        const QFontMetrics fm(font());
        for (const auto& token : rows_[row].tokens)
        {
            const int size = fm.horizontalAdvance(token.text);
            if (used > 0 && used + size > width) { ++lines; used = 0; }
            used += size;
        }
        return lines * lineHeight_;
    }
    QRect MemoryRowCanvas::rowRect(int row) const
    {
        const int line = row >= 0 && row < rowLineStarts_.size() ? rowLineStarts_[row] : 0;
        const int y = headerHeight_ + (line - verticalScrollBar()->value()) * lineHeight_;
        return QRect(0, y, viewport()->width(), rowHeight(row));
    }
    QRect MemoryRowCanvas::contentRect(int row) const
    {
        const QRect r = rowRect(row);
        return QRect(contentStart() + kPadding, r.top(), std::max(1, r.width() - contentStart() - kPadding * 2), lineHeight_);
    }
    int MemoryRowCanvas::rowAt(const QPoint& pos) const
    {
        if (pos.y() < headerHeight_ || rowLineStarts_.size() < 2) return -1;
        const int line = verticalScrollBar()->value() + (pos.y() - headerHeight_) / lineHeight_;
        if (line >= rowLineStarts_.back()) return -1;
        return static_cast<int>(std::upper_bound(rowLineStarts_.begin(), rowLineStarts_.end(), line) - rowLineStarts_.begin()) - 1;
    }
    int MemoryRowCanvas::firstVisibleRow() const
    {
        if (rows_.isEmpty() || rowLineStarts_.size() < 2) return 0;
        const auto index = static_cast<int>(std::upper_bound(rowLineStarts_.begin(), rowLineStarts_.end(), verticalScrollBar()->value()) - rowLineStarts_.begin()) - 1;
        return std::clamp(index, 0, static_cast<int>(rows_.size()) - 1);
    }
    int MemoryRowCanvas::visibleRowCount() const
    {
        const int first = firstVisibleRow();
        int count = 0, y = rowRect(first).top();
        for (int i = first; i < rows_.size() && y < viewport()->height(); ++i)
        { y += rowHeight(i); ++count; }
        return std::max(1, count);
    }
    void MemoryRowCanvas::updateMetrics()
    {
        const QFontMetrics fm(font());
        characterWidth_ = std::max(1, fm.horizontalAdvance(QLatin1Char('0')));
        lineHeight_ = std::max(18, fm.height() + 6);
        headerHeight_ = lineHeight_ + 4;
        updateScrollBars();
    }
    void MemoryRowCanvas::updateScrollBars()
    {
        const int first = firstVisibleRow();
        const int withinRow = first < rowLineStarts_.size() ? verticalScrollBar()->value() - rowLineStarts_[first] : 0;
        rowLineStarts_.clear(); rowLineStarts_.push_back(0);
        for (int i = 0; i < rows_.size(); ++i)
            rowLineStarts_.push_back(rowLineStarts_.back() + std::max(1, rowHeight(i) / lineHeight_));
        const int visibleLines = std::max(1, (viewport()->height() - headerHeight_) / lineHeight_);
        verticalScrollBar()->setSingleStep(1);
        verticalScrollBar()->setPageStep(visibleLines);
        verticalScrollBar()->setRange(0, std::max(0, rowLineStarts_.back() - visibleLines));
        if (first < rows_.size()) verticalScrollBar()->setValue(rowLineStarts_[first]
            + std::clamp(withinRow, 0, rowLineStarts_[first + 1] - rowLineStarts_[first] - 1));
        int width = 0;
        const QFontMetrics fm(font());
        for (const auto& row : rows_)
        {
            int rowWidth = 0;
            for (const auto& token : row.tokens) rowWidth += fm.horizontalAdvance(token.text);
            width = std::max(width, rowWidth);
        }
        horizontalScrollBar()->setRange(0, wrapContent_ ? 0 : std::max(0, width + contentStart() + kPadding * 2 - viewport()->width()));
        horizontalScrollBar()->setPageStep(viewport()->width());
        viewport()->update();
    }
    void MemoryRowCanvas::setRows(QVector<MemoryDisplayRow> rows, bool preserveViewport)
    {
        std::optional<std::uint64_t> top;
        const int oldFirst = firstVisibleRow();
        const int subLine = oldFirst < rowLineStarts_.size() ? verticalScrollBar()->value() - rowLineStarts_[oldFirst] : 0;
        if (preserveViewport && oldFirst < rows_.size()) top = rows_[oldFirst].address;
        const auto previous = selection_;
        const auto pending = pendingNavigationAddress_;
        rows_ = std::move(rows);
        maximumRowBytes_ = 1;
        rowsByAddress_.clear();
        for (int i = 0; i < rows_.size(); ++i)
        {
            maximumRowBytes_ = std::max(maximumRowBytes_, static_cast<int>(rows_[i].bytes.size()));
            rowsByAddress_[rows_[i].address].push_back(i);
        }
        addressDigits_ = addressBits_ > 32 || std::any_of(rows_.begin(), rows_.end(), [](const auto& row) { return row.address > 0xFFFFFFFFULL; }) ? 16 : 8;
        selectedRow_ = -1;
        updateScrollBars();
        int first = 0;
        if (top) for (int i = 0; i < rows_.size(); ++i) if (rows_[i].address <= *top) first = i;
        verticalScrollBar()->setValue(preserveViewport && first < rows_.size() ? rowLineStarts_[first]
            + std::clamp(subLine, 0, rowLineStarts_[first + 1] - rowLineStarts_[first] - 1) : 0);
        if (previous && preserveViewport) selectRange(previous->first, previous->second, false);
        else { selection_.reset(); if (!rows_.isEmpty()) setSelectedRow(0, false); }
        pendingNavigationAddress_ = rows_.isEmpty() ? std::nullopt : pending;
        if (pendingNavigationAddress_)
            for (int i = 0; i < rows_.size(); ++i)
                if (rows_[i].selectable && *pendingNavigationAddress_ >= rows_[i].address
                    && *pendingNavigationAddress_ <= lastByte(rows_[i]))
                { setSelectedRow(i); break; }
        notifyVisibleRange();
        viewport()->update();
    }
    void MemoryRowCanvas::setSelectedRow(int row, bool ensureVisible)
    {
        if (row < 0 || row >= rows_.size() || !rows_[row].selectable) return;
        selectedRow_ = row;
        selectRange(rows_[row].address, lastByte(rows_[row]), ensureVisible);
    }
    void MemoryRowCanvas::selectRange(std::uint64_t first, std::uint64_t last, bool ensureVisible)
    {
        cancelPendingNavigation();
        if (last < first) std::swap(first, last);
        selectedRow_ = -1;
        for (int i = 0; i < rows_.size(); ++i)
            if (rows_[i].selectable && first >= rows_[i].address && first <= lastByte(rows_[i])) { selectedRow_ = i; break; }
        if (selectedRow_ < 0) { selection_.reset(); viewport()->update(); return; }
        const auto before = selection_;
        selection_ = std::make_pair(first, last);
        if (ensureVisible)
        {
            QRect target = contentRect(selectedRow_);
            for (const auto& token : tokenRects(selectedRow_))
                if (token.first <= first && token.last >= first) { target = token.rect; break; }
            if (target.top() < headerHeight_) verticalScrollBar()->setValue(verticalScrollBar()->value() + (target.top() - headerHeight_) / lineHeight_);
            else if (target.bottom() >= viewport()->height())
                verticalScrollBar()->setValue(verticalScrollBar()->value() + (target.bottom() - viewport()->height() + lineHeight_) / lineHeight_);
        }
        viewport()->update();
        if (selection_ != before) emit selectionChanged(first, last);
    }
    std::optional<std::pair<std::uint64_t, std::uint64_t>> MemoryRowCanvas::selectedRange() const { return selection_; }
    void MemoryRowCanvas::scrollToAddress(std::uint64_t address)
    {
        cancelPendingNavigation();
        for (int i = 0; i < rows_.size(); ++i) if (address >= rows_[i].address && address <= lastByte(rows_[i]))
        { verticalScrollBar()->setValue(rowLineStarts_[i]); return; }
    }
    std::optional<std::uint64_t> MemoryRowCanvas::addressForVisualLine(int linesFromTop) const
    {
        if (rows_.isEmpty() || rowLineStarts_.size() < 2) return std::nullopt;
        const int line = std::clamp(verticalScrollBar()->value() + linesFromTop, 0, rowLineStarts_.back() - 1);
        const int row = static_cast<int>(std::upper_bound(rowLineStarts_.begin(), rowLineStarts_.end(), line) - rowLineStarts_.begin()) - 1;
        const int y = headerHeight_ + (line - verticalScrollBar()->value()) * lineHeight_;
        for (const auto& token : tokenRects(row))
            if (token.rect.top() == y && rows_[row].tokens[token.token].length) return token.first;
        return rows_[row].address;
    }
    std::optional<std::pair<std::uint64_t, std::uint64_t>> MemoryRowCanvas::hitRange(const QPoint& pos) const
    {
        const int index = rowAt(pos);
        if (index < 0 || !rows_[index].selectable) return std::nullopt;
        const auto& row = rows_[index];
        const int hexStart = gutterWidth() + addressWidth();
        if (bytesVisible_ && pos.x() >= hexStart && pos.x() < contentStart())
        {
            const int byte = (pos.x() - hexStart - kPadding) / (3 * characterWidth_);
            if (pos.x() < hexStart + kPadding || byte < 0 || byte >= row.bytes.size()) return std::nullopt;
            return std::make_pair(row.address + byte, row.address + byte);
        }
        if (pos.x() >= contentStart())
        {
            for (const auto& token : tokenRects(index)) if (token.rect.contains(pos) && row.tokens[token.token].length)
                return std::make_pair(token.first, token.last);
            return std::nullopt;
        }
        return std::make_pair(row.address, lastByte(row));
    }
    std::optional<std::uint64_t> MemoryRowCanvas::addressAt(const QPoint& pos) const
    {
        const auto range = hitRange(pos);
        return range ? std::optional<std::uint64_t>(range->first) : std::nullopt;
    }
    QColor MemoryRowCanvas::tokenColor(MemoryTokenRole role) const
    {
        using A = KswordTheme::AccentRole;
        switch (role)
        {
        case MemoryTokenRole::Mnemonic: return KswordTheme::AccentColor(A::Blue);
        case MemoryTokenRole::Register: return KswordTheme::AccentColor(A::Purple);
        case MemoryTokenRole::Address: return KswordTheme::AccentColor(A::Cyan);
        case MemoryTokenRole::Number: return KswordTheme::AccentColor(A::Orange);
        case MemoryTokenRole::Comment: return KswordTheme::TextSecondaryColor();
        case MemoryTokenRole::Invalid: return KswordTheme::ErrorColor();
        default: return KswordTheme::TextPrimaryColor();
        }
    }
    void MemoryRowCanvas::paintEvent(QPaintEvent*)
    {
        QPainter p(viewport());
        const QColor base = KswordTheme::SurfaceColor();
        p.fillRect(viewport()->rect(), base);
        p.setFont(font());
        const int start = firstVisibleRow();
        const int hexStart = gutterWidth() + addressWidth();
        const auto selected = [this](std::uint64_t first, std::uint64_t last) {
            return selection_ && first <= selection_->second && last >= selection_->first;
        };
        const QColor selectedBg = KswordTheme::BlendColors(base, KswordTheme::AccentColor(KswordTheme::AccentRole::Blue), 65);
        p.save();
        p.setClipRect(QRect(0, headerHeight_, viewport()->width(), viewport()->height() - headerHeight_));
        for (int i = start; i < rows_.size(); ++i)
        {
            const QRect rect = rowRect(i);
            if (rect.top() >= viewport()->height()) break;
            const auto& row = rows_[i];
            if (i == selectedRow_) p.fillRect(rect, KswordTheme::BlendColors(base, selectedBg, 30));
            p.setPen(KswordTheme::TextSecondaryColor());
            p.drawText(QRect(gutterWidth() + kPadding, rect.top(), addressWidth() - kPadding * 2, lineHeight_),
                Qt::AlignVCenter | Qt::AlignLeft, hexcanvas_format::FormatAddress(row.address, addressDigits_));
            if (bytesVisible_)
            {
                p.save(); p.setClipRect(QRect(hexStart, rect.top(), bytesWidth(), rect.height()), Qt::IntersectClip);
                for (int b = 0; b < row.bytes.size(); ++b)
                {
                    QRect cell(hexStart + kPadding + b * characterWidth_ * 3, rect.top(), characterWidth_ * 2, lineHeight_);
                    const auto address = row.address + b;
                    QColor background = base;
                    if (b < row.changeKinds.size())
                    {
                        const auto kind = row.changeKinds[b];
                        if (kind == ksword::memwb::ByteChangeKind::Pending) background = KswordTheme::BlendColors(base, KswordTheme::AccentColor(KswordTheme::AccentRole::Orange), 45);
                        else if (kind == ksword::memwb::ByteChangeKind::ExternalChange) background = KswordTheme::BlendColors(base, KswordTheme::AccentColor(KswordTheme::AccentRole::Cyan), 35);
                    }
                    if (selected(address, address)) background = selectedBg;
                    p.fillRect(cell, background);
                    const quint8 valid = b < row.validMask.size() ? row.validMask[b] : 0;
                    p.setPen(KswordTheme::EnsureTextContrast(valid == 1 ? KswordTheme::TextPrimaryColor() : KswordTheme::TextSecondaryColor(), background));
                    const QString text = valid == 2 ? QStringLiteral("··") : valid == 0 ? QStringLiteral("??")
                        : QString::number(static_cast<unsigned char>(row.bytes[b]), 16).rightJustified(2, QLatin1Char('0')).toUpper();
                    p.drawText(cell, Qt::AlignVCenter | Qt::AlignLeft, text);
                }
                p.restore();
            }
            p.save(); p.setClipRect(QRect(contentStart(), rect.top(), viewport()->width() - contentStart(), rect.height()), Qt::IntersectClip);
            for (const auto& hit : tokenRects(i))
            {
                const auto& token = row.tokens[hit.token];
                const bool active = token.length && selected(hit.first, hit.last);
                auto kind = ksword::memwb::ByteChangeKind::Unchanged;
                for (int b = 0; b < row.changeKinds.size(); ++b)
                {
                    const auto address = row.address + b;
                    if (!token.length || address < hit.first || address > hit.last) continue;
                    const auto changed = row.changeKinds[b];
                    if (changed == ksword::memwb::ByteChangeKind::Pending) { kind = changed; break; }
                    if (changed == ksword::memwb::ByteChangeKind::ExternalChange || kind == ksword::memwb::ByteChangeKind::Unchanged) kind = changed;
                }
                QColor background = base;
                if (kind == ksword::memwb::ByteChangeKind::Pending) background = KswordTheme::BlendColors(base, KswordTheme::AccentColor(KswordTheme::AccentRole::Orange), 45);
                else if (kind == ksword::memwb::ByteChangeKind::ExternalChange) background = KswordTheme::BlendColors(base, KswordTheme::AccentColor(KswordTheme::AccentRole::Cyan), 35);
                else if (kind == ksword::memwb::ByteChangeKind::SelfWritten) background = KswordTheme::BlendColors(base, KswordTheme::AccentColor(KswordTheme::AccentRole::Green), 20);
                if (active) background = selectedBg;
                p.fillRect(hit.rect, background);
                p.setPen(KswordTheme::EnsureTextContrast(tokenColor(token.role), background));
                p.drawText(hit.rect, Qt::AlignVCenter | Qt::AlignLeft, token.text);
            }
            p.restore();
            if (branchGutterVisible_ && row.branchTarget)
            {
                const QColor color = KswordTheme::AccentColor(KswordTheme::AccentRole::Blue);
                p.setPen(QPen(color, i == selectedRow_ ? 2 : 1));
                const int y = rect.top() + lineHeight_ / 2;
                int targetY = *row.branchTarget < row.address ? headerHeight_ + 2 : viewport()->height() - 2;
                const auto targetRows = rowsByAddress_.constFind(*row.branchTarget);
                if (targetRows != rowsByAddress_.cend())
                {
                    // Preserve the original first matching row at/after the painted start,
                    // including repeated addresses and offscreen continuation arrows.
                    const auto target = std::lower_bound(targetRows->cbegin(), targetRows->cend(), start);
                    if (target != targetRows->cend()) targetY = rowRect(*target).top() + lineHeight_ / 2;
                }
                const int lane = 5 + (i % 3) * 6;
                p.drawLine(lane, y, 26, y); p.drawLine(lane, y, lane, targetY); p.drawLine(lane, targetY, 25, targetY);
                p.drawLine(25, targetY, 21, targetY - 3); p.drawLine(25, targetY, 21, targetY + 3);
            }
        }
        p.restore();
        p.setPen(KswordTheme::BorderColor());
        p.drawLine(hexStart, 0, hexStart, viewport()->height());
        if (bytesVisible_) p.drawLine(contentStart(), 0, contentStart(), viewport()->height());
        p.fillRect(QRect(0, 0, viewport()->width(), headerHeight_), KswordTheme::SurfaceAltColor());
        p.setPen(KswordTheme::TextSecondaryColor());
        p.drawText(QRect(gutterWidth() + kPadding, 0, addressWidth(), headerHeight_), Qt::AlignVCenter, ks::i18n::sourceText(QStringLiteral("地址")));
        if (bytesVisible_) p.drawText(QRect(hexStart + kPadding, 0, bytesWidth() - kPadding, headerHeight_), Qt::AlignVCenter, QStringLiteral("HEX"));
        p.drawText(QRect(contentStart() + kPadding, 0, std::max(1, viewport()->width() - contentStart()), headerHeight_), Qt::AlignVCenter, ks::i18n::sourceText(contentTitle_));
    }
    void MemoryRowCanvas::notifyVisibleRange()
    {
        if (rows_.isEmpty()) return;
        const int first = firstVisibleRow();
        const int last = std::min(static_cast<int>(rows_.size()) - 1, first + visibleRowCount() - 1);
        if (first < rows_.size()) emit visibleRangeChanged(rows_[first].address, lastByte(rows_[last]));
    }
    void MemoryRowCanvas::resizeEvent(QResizeEvent* event)
    { QAbstractScrollArea::resizeEvent(event); updateScrollBars(); notifyVisibleRange(); }
    void MemoryRowCanvas::mousePressEvent(QMouseEvent* event)
    {
        if (event->button() != Qt::LeftButton) { QAbstractScrollArea::mousePressEvent(event); return; }
        if (bytesVisible_ && std::abs(event->position().toPoint().x() - contentStart()) <= 4)
        { resizingBytes_ = true; return; }
        const auto range = hitRange(event->position().toPoint());
        if (range)
        {
            if (!(event->modifiers() & Qt::ShiftModifier) || !selection_)
            { dragAnchor_ = range->first; dragAnchorLast_ = range->second; }
            else
            {
                dragAnchor_ = selection_->first;
                dragAnchorLast_ = dragAnchor_;
                if (event->position().toPoint().x() >= contentStart())
                    for (const auto& token : tokenRects(selectedRow_))
                        if (token.first <= dragAnchor_ && token.last >= dragAnchor_)
                        { dragAnchor_ = token.first; dragAnchorLast_ = token.last; break; }
            }
            dragging_ = true;
            selectRange(std::min(dragAnchor_, range->first), std::max(dragAnchorLast_, range->second), false);
        }
        setFocus(Qt::MouseFocusReason);
    }
    void MemoryRowCanvas::mouseMoveEvent(QMouseEvent* event)
    {
        if (resizingBytes_) { byteWidthOverride_ = event->position().toPoint().x() - gutterWidth() - addressWidth(); updateScrollBars(); return; }
        viewport()->setCursor(bytesVisible_ && std::abs(event->position().toPoint().x() - contentStart()) <= 4 ? Qt::SplitHCursor : Qt::IBeamCursor);
        if (!dragging_) return;
        const auto range = hitRange(event->position().toPoint());
        if (range) selectRange(std::min(dragAnchor_, range->first), std::max(dragAnchorLast_, range->second), false);
    }
    void MemoryRowCanvas::mouseReleaseEvent(QMouseEvent*) { dragging_ = false; resizingBytes_ = false; }
    void MemoryRowCanvas::mouseDoubleClickEvent(QMouseEvent* event)
    { const int row = rowAt(event->position().toPoint()); if (row >= 0 && rows_[row].selectable) { setSelectedRow(row); emit rowActivated(row); } }
    void MemoryRowCanvas::wheelEvent(QWheelEvent* event)
    {
        if (event->modifiers() & Qt::ControlModifier) { zoomBy(event->angleDelta().y() > 0 ? 1 : -1); event->accept(); return; }
        const int delta = event->angleDelta().y();
        const int lines = event->pixelDelta().y() ? std::max(1, std::abs(event->pixelDelta().y()) / lineHeight_) : std::max(1, std::abs(delta) / 120) * 3;
        if (!delta && !event->pixelDelta().y()) { QAbstractScrollArea::wheelEvent(event); return; }
        const int direction = (delta ? delta : event->pixelDelta().y()) > 0 ? -1 : 1;
        cancelPendingNavigation();
        auto* bar = verticalScrollBar();
        if ((direction < 0 && bar->value() == 0) || (direction > 0 && bar->value() == bar->maximum())) emit requestMore(direction, lines);
        else bar->setValue(bar->value() + direction * lines);
        event->accept();
    }
    void MemoryRowCanvas::keyPressEvent(QKeyEvent* event)
    {
        if (event->key() == Qt::Key_C && event->modifiers() == (Qt::ControlModifier | Qt::ShiftModifier))
        {
            bool complete = false;
            const auto bytes = selectedBytes(&complete);
            if (complete) QGuiApplication::clipboard()->setText(QString::fromLatin1(bytes.toHex(' ').toUpper()));
            event->accept(); return;
        }
        if (event->matches(QKeySequence::Copy)) { QGuiApplication::clipboard()->setText(selectedText()); event->accept(); return; }
        int offset = 0;
        if (event->key() == Qt::Key_Up) offset = -1;
        else if (event->key() == Qt::Key_Down) offset = 1;
        else if (event->key() == Qt::Key_PageUp) offset = -visibleRowCount();
        else if (event->key() == Qt::Key_PageDown) offset = visibleRowCount();
        if (offset)
        {
            int next = selectedRow_ < 0 ? 0 : selectedRow_ + offset;
            const int direction = offset < 0 ? -1 : 1;
            while (next >= 0 && next < rows_.size() && !rows_[next].selectable) next += direction;
            if (next < 0 || next >= rows_.size())
            {
                if (selectedRow_ >= 0 && selectedRow_ < rows_.size())
                {
                    const auto& row = rows_[selectedRow_];
                    const auto edge = direction < 0 ? row.address : lastByte(row);
                    if ((direction < 0 && edge > 0) || (direction > 0 && edge < UINT64_MAX))
                        pendingNavigationAddress_ = direction < 0 ? edge - 1 : edge + 1;
                }
                const QPointer<MemoryRowCanvas> self(this);
                emit requestMore(direction, std::abs(offset));
                if (!self) return;
            }
            else setSelectedRow(next);
            event->accept(); return;
        }
        QAbstractScrollArea::keyPressEvent(event);
    }
    void MemoryRowCanvas::contextMenuEvent(QContextMenuEvent* event)
    {
        QPoint pos = event->pos();
        if (event->reason() == QContextMenuEvent::Keyboard)
        {
            pos = contentRect(selectedRow_).topLeft() + QPoint(1, lineHeight_ / 2);
            if (selection_) for (const auto& token : tokenRects(selectedRow_))
                if (token.first <= selection_->first && token.last >= selection_->first) { pos = token.rect.center(); break; }
        }
        const int row = rowAt(pos);
        const auto hit = hitRange(pos);
        if (row >= 0 && hit && (!selection_ || hit->first < selection_->first || hit->first > selection_->second))
            selectRange(hit->first, hit->second, false);
        emit contextMenuRequested(pos);
        event->accept();
    }
    bool MemoryRowCanvas::viewportEvent(QEvent* event)
    {
        if (event->type() == QEvent::ToolTip)
        {
            const auto* help = static_cast<QHelpEvent*>(event);
            const auto address = addressAt(help->pos());
            const int row = rowAt(help->pos());
            if (row >= 0)
            {
                QString text;
                const auto& data = rows_[row];
                for (int b = 0; b < data.bytes.size(); ++b)
                {
                    if (b) text += QLatin1Char(' ');
                    const quint8 mask = b < data.validMask.size() ? data.validMask[b] : 0;
                    text += mask == 0 ? QStringLiteral("??") : mask == 2 ? QStringLiteral("··")
                        : QString::number(static_cast<unsigned char>(data.bytes[b]), 16).rightJustified(2, QLatin1Char('0')).toUpper();
                }
                if (address) text.prepend(hexcanvas_format::FormatAddress(*address, addressDigits_) + QLatin1Char('\n'));
                QToolTip::showText(help->globalPos(), text, viewport());
            }
            return true;
        }
        return QAbstractScrollArea::viewportEvent(event);
    }
    void MemoryRowCanvas::changeEvent(QEvent* event)
    {
        QAbstractScrollArea::changeEvent(event);
        if (event->type() == QEvent::FontChange) updateMetrics();
        else if (event->type() == QEvent::ApplicationPaletteChange || event->type() == QEvent::PaletteChange || event->type() == QEvent::LanguageChange) viewport()->update();
    }
    void MemoryRowCanvas::setContentTitle(QString title) { contentTitle_ = std::move(title); viewport()->update(); }
    void MemoryRowCanvas::setAddressBits(int bits)
    {
        addressBits_ = bits > 32 ? 64 : 32;
        addressDigits_ = addressBits_ > 32 || std::any_of(rows_.begin(), rows_.end(), [](const auto& row) { return row.address > 0xFFFFFFFFULL; }) ? 16 : 8;
        updateScrollBars();
    }
    void MemoryRowCanvas::setBytesVisible(bool visible) { bytesVisible_ = visible; updateScrollBars(); }
    void MemoryRowCanvas::setTextPriority(bool enabled) { textPriority_ = enabled; updateScrollBars(); }
    void MemoryRowCanvas::setBranchGutterVisible(bool visible) { branchGutterVisible_ = visible; updateScrollBars(); }
    void MemoryRowCanvas::setWrapContent(bool wrap) { wrapContent_ = wrap; updateScrollBars(); }
    void MemoryRowCanvas::setZoomSteps(int steps)
    {
        steps = std::clamp(steps, -4, 12);
        if (zoomSteps_ == steps) return;
        zoomSteps_ = steps;
        QFont adjusted = baseFont_;
        adjusted.setPointSizeF(std::max(6.0, (baseFont_.pointSizeF() > 0 ? baseFont_.pointSizeF() : 10.0) + steps));
        setFont(adjusted); updateMetrics(); emit zoomChanged(steps);
    }
    void MemoryRowCanvas::zoomBy(int steps) { setZoomSteps(zoomSteps_ + steps); }
    QString MemoryRowCanvas::selectedText() const
    {
        if (!selection_) return QString();
        QString result;
        bool addedRow = false;
        for (const auto& row : rows_)
        {
            QString line;
            for (const auto& token : row.tokens)
                if (token.length && token.address <= selection_->second && token.address + token.length - 1 >= selection_->first) line += token.text;
            if (!line.isEmpty()) { if (addedRow) result += QLatin1Char('\n'); result += line; addedRow = true; }
        }
        return result;
    }
    QByteArray MemoryRowCanvas::selectedBytes(bool* complete) const
    {
        if (complete) *complete = false;
        QByteArray result;
        if (!selection_) return result;
        auto next = selection_->first;
        for (const auto& row : rows_)
            for (int b = 0; b < row.bytes.size(); ++b)
            {
                const auto address = row.address + b;
                if (address < next || address > selection_->second) continue;
                if (address != next || b >= row.validMask.size() || row.validMask[b] != 1) return QByteArray();
                result += row.bytes[b];
                if (next == selection_->second) { if (complete) *complete = true; return result; }
                ++next;
            }
        return QByteArray();
    }
}
