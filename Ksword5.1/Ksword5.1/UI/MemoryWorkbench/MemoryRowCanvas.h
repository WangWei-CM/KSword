#pragma once

#include "../../../../shared/evidence/memory_workbench/MemoryDiffOverlay.h"
#include <QAbstractScrollArea>
#include <QByteArray>
#include <QHash>
#include <QString>
#include <QVector>
#include <optional>
#include <cstdint>

namespace ks::ui
{
    // Address-backed rows shared by disassembly and decoded text. No target I/O.
    enum class MemoryTokenRole { Plain, Mnemonic, Register, Number, Address, Comment, Invalid };
    struct MemoryRowToken
    {
        QString text;
        std::uint64_t address = 0;
        std::uint64_t length = 0;
        MemoryTokenRole role = MemoryTokenRole::Plain;
    };
    struct MemoryDisplayRow
    {
        std::uint64_t address = 0;
        QByteArray bytes;
        QVector<quint8> validMask;
        QVector<ksword::memwb::ByteChangeKind> changeKinds;
        QVector<MemoryRowToken> tokens;
        std::optional<std::uint64_t> branchTarget;
        bool selectable = true;
    };

    class MemoryRowCanvas final : public QAbstractScrollArea
    {
        Q_OBJECT
    public:
        explicit MemoryRowCanvas(QWidget* parent = nullptr);
        void setRows(QVector<MemoryDisplayRow> rows, bool preserveViewport = true);
        const QVector<MemoryDisplayRow>& rows() const { return rows_; }
        int selectedRow() const { return selectedRow_; }
        void setSelectedRow(int row, bool ensureVisible = true);
        void selectRange(std::uint64_t first, std::uint64_t last, bool ensureVisible = true);
        std::optional<std::pair<std::uint64_t, std::uint64_t>> selectedRange() const;
        std::optional<std::uint64_t> addressAt(const QPoint& viewportPosition) const;
        int rowAt(const QPoint& viewportPosition) const;
        QRect rowRect(int row) const;
        QRect contentRect(int row) const;
        void setContentTitle(QString title);
        void setAddressBits(int bits);
        void setBytesVisible(bool visible);
        bool bytesVisible() const { return bytesVisible_; }
        void setTextPriority(bool enabled);
        void setBranchGutterVisible(bool visible);
        void setZoomSteps(int steps);
        int zoomSteps() const { return zoomSteps_; }
        void zoomBy(int steps);
        void setWrapContent(bool wrap);
        bool wrapContent() const { return wrapContent_; }
        QString selectedText() const;
        QByteArray selectedBytes(bool* complete = nullptr) const;
        void scrollToAddress(std::uint64_t address);
        void cancelPendingNavigation() { pendingNavigationAddress_.reset(); }
        std::optional<std::uint64_t> addressForVisualLine(int linesFromTop = 0) const;
        QSize minimumSizeHint() const override { return QSize(1, 1); }
        QSize sizeHint() const override { return QSize(760, 420); }
    signals:
        void selectionChanged(quint64 first, quint64 last);
        void rowActivated(int row);
        void contextMenuRequested(const QPoint& viewportPosition);
        void visibleRangeChanged(quint64 first, quint64 last);
        // Ask the owner to rebase its bounded decode window at either edge.
        void requestMore(int direction, int lines);
        void zoomChanged(int steps);
    protected:
        void paintEvent(QPaintEvent* event) override;
        void resizeEvent(QResizeEvent* event) override;
        void mousePressEvent(QMouseEvent* event) override;
        void mouseMoveEvent(QMouseEvent* event) override;
        void mouseReleaseEvent(QMouseEvent* event) override;
        void mouseDoubleClickEvent(QMouseEvent* event) override;
        void wheelEvent(QWheelEvent* event) override;
        void keyPressEvent(QKeyEvent* event) override;
        void contextMenuEvent(QContextMenuEvent* event) override;
        bool viewportEvent(QEvent* event) override;
        void changeEvent(QEvent* event) override;
    private:
        struct TokenHit { QRect rect; std::uint64_t first; std::uint64_t last; int token; };
        void updateMetrics();
        void updateScrollBars();
        void notifyVisibleRange();
        QVector<TokenHit> tokenRects(int row) const;
        std::optional<std::pair<std::uint64_t, std::uint64_t>> hitRange(const QPoint& pos) const;
        int gutterWidth() const;
        int addressWidth() const;
        int bytesWidth() const;
        int contentStart() const;
        int rowHeight(int row) const;
        int visibleRowCount() const;
        int firstVisibleRow() const;
        QColor tokenColor(MemoryTokenRole role) const;
        QVector<MemoryDisplayRow> rows_;
        // Geometry-only caches rebuilt with rows, independent of theme/selection/scroll.
        int maximumRowBytes_ = 1;
        QHash<quint64, QVector<int>> rowsByAddress_;
        QVector<int> rowLineStarts_;
        QString contentTitle_;
        int selectedRow_ = -1;
        std::optional<std::pair<std::uint64_t, std::uint64_t>> selection_;
        // Keyboard continuation survives placeholder rows until bytes arrive.
        std::optional<std::uint64_t> pendingNavigationAddress_;
        std::uint64_t dragAnchor_ = 0;
        std::uint64_t dragAnchorLast_ = 0;
        bool dragging_ = false;
        bool resizingBytes_ = false;
        bool bytesVisible_ = true;
        bool textPriority_ = false;
        bool branchGutterVisible_ = false;
        bool wrapContent_ = false;
        int byteWidthOverride_ = -1;
        int addressBits_ = 64;
        int addressDigits_ = 16;
        int zoomSteps_ = 0;
        int lineHeight_ = 22;
        int characterWidth_ = 8;
        int headerHeight_ = 26;
        QFont baseFont_;
    };
}
