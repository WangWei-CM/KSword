#include "AsyncLogTextView.h"
#include "AsyncOperation.h"
#include "CodeTextEdit.h"
#include "CodeTextSurfaceColors.h"
#include "CompleteExport.h"
#include "../theme.h"

#include <QAction>
#include <QAccessibleWidget>
#include <QFocusEvent>
#include <QDrag>
#include <QMimeData>
#include <QStyleHints>
#include <QStyle>
#include <QApplication>
#include <QClipboard>
#include <QContextMenuEvent>
#include <QFontInfo>
#include <QFontMetricsF>
#include <QGlyphRun>
#include <QHideEvent>
#include <QImage>
#include <QKeyEvent>
#include <QMenu>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QPointer>
#include <QResizeEvent>
#include <QScrollBar>
#include <QShowEvent>
#include <QTextBlock>
#include <QTextBoundaryFinder>
#include <QTextCursor>
#include <QTextDocument>
#include <QTextLayout>
#include <QTextOption>
#include <QTimer>
#include <QWheelEvent>
#include <algorithm>
#include <cmath>
#include <limits>
#include <iterator>
#include <map>
#include <vector>

namespace ks::ui
{
namespace
{
    constexpr int kInset = 10;
    template<class T> std::shared_ptr<const T> retained(std::shared_ptr<T> value)
    {
        return RetainCompleteExportSnapshot<T>(std::move(value));
    }

    struct RevisionOrigin
    {
        quint64 first = 0, last = 0, origin = 0, lastEnd = 0;
    };
    struct LogSource
    {
        QString raw; // QTextDocument::toRawText: preserves NBSP and soft breaks.
        QString plain; // Exactly QTextDocument::toPlainText, for copy/search.
        quint64 revision = 0, origin = 0, lastEmptyRevision = 0;
        int blocks = 1;
        int maximumBlocks = 0;
        std::vector<RevisionOrigin> history{{0, 0, 0, 0}};
    };
    enum class CommandKind { Append, Maximum, Snapshot };
    struct Command
    {
        CommandKind kind = CommandKind::Append;
        std::shared_ptr<const QString> text;
        int maximum = 0;
        quint64 revision = 0, snapshot = 0;
    };
    struct SourceRequest
    {
        std::shared_ptr<const LogSource> before;
        std::shared_ptr<const std::vector<Command>> commands;
    };
    struct SnapshotReply
    {
        quint64 id = 0, revision = 0;
        std::shared_ptr<const QString> text;
        quint64 origin = 0, lastEmptyRevision = 0;
    };
    struct SourceReply
    {
        std::shared_ptr<const LogSource> source;
        std::vector<SnapshotReply> snapshots;
    };

    SourceReply prepareSource(const SourceRequest& request)
    {
        auto next = std::make_shared<LogSource>(*request.before);
        QTextDocument document;
        document.setLayoutEnabled(false);
        document.setUndoRedoEnabled(false);
        QTextCursor cursor(&document);
        cursor.insertText(next->raw);
        int maximum = next->maximumBlocks;
        SourceReply reply;
        for (const Command& command : *request.commands)
        {
            if (command.kind == CommandKind::Snapshot)
            {
                reply.snapshots.push_back({command.snapshot, next->revision,
                    retained(std::make_shared<QString>(document.toRawText())), next->origin, next->lastEmptyRevision});
                continue;
            }
            // Temporarily disable the limit so its exact prefix removal can be
            // measured independently of insertText's CRLF/Unicode normalization.
            document.setMaximumBlockCount(0);
            if (command.kind == CommandKind::Append)
            {
                cursor.movePosition(QTextCursor::End);
                if (!document.isEmpty()) cursor.insertBlock();
                cursor.insertText(*command.text);
            }
            else maximum = command.maximum;
            const int beforeLimit = document.characterCount();
            document.setMaximumBlockCount(maximum);
            next->origin += static_cast<quint64>(beforeLimit - document.characterCount());
            next->revision = command.revision;
            if (document.isEmpty()) next->lastEmptyRevision = command.revision;
            const quint64 end = next->origin + static_cast<quint64>(document.characterCount() - 1);
            auto& history = next->history;
            if (history.back().origin == next->origin)
            {
                history.back().last = next->revision;
                history.back().lastEnd = end;
            }
            else history.push_back({next->revision, next->revision, next->origin, end});
            // Coalesced no-eviction runs cost one entry. An old range whose
            // entire document was evicted can no longer produce a valid hit.
            const auto keep = std::find_if(history.begin(), history.end(), [&](const RevisionOrigin& entry) {
                return entry.lastEnd >= next->origin;
            });
            if (keep != history.begin()) history.erase(history.begin(), keep);
        }
        next->raw = document.toRawText();
        next->plain = document.toPlainText();
        next->blocks = document.blockCount();
        next->maximumBlocks = maximum;
        reply.source = retained(std::move(next));
        return reply;
    }

    // Qt cursors follow an insertion at their end and clamp prefix deletions.
    // The only way an earlier interior cursor can reach the end is an empty
    // intermediate document; this folds every accepted append/limit command.
    int adjustLogPosition(int position, int previousLength, int length, quint64 removed, bool emptied)
    {
        if (emptied || position == previousLength) return length;
        return removed >= static_cast<quint64>(position) ? 0 : position - static_cast<int>(removed);
    }

    struct Paragraph
    {
        int start = 0, length = 0, firstLine = 0;
        qreal top = 0;
        std::unique_ptr<QTextLayout> layout;
    };
    struct VisualLine
    {
        int paragraph = 0, start = 0, length = 0;
        qreal top = 0, height = 0;
        QTextLine line;
    };
    struct LayoutRequest
    {
        std::shared_ptr<const LogSource> source;
        QFont font;
        int width = 0, dpiX = 0, dpiY = 0;
        qreal dpr = 1;
        quint64 epoch = 0;
    };
    struct LogLayout
    {
        // The paint device remains alive for every QTextLayout. No GUI device,
        // widget, QTextDocument or borrowed buffer crosses the worker boundary.
        QImage metrics;
        LayoutRequest identity;
        std::vector<Paragraph> paragraphs;
        std::vector<VisualLine> lines;
        std::vector<int> characters, wordStarts, wordEnds;
        qreal height = 0;
    };
    using LayoutReply = std::shared_ptr<const LogLayout>;

    LayoutReply prepareLayout(const LayoutRequest& request, const AsyncOperationToken& token)
    {
        if (token.isCanceled()) return {};
        auto result = std::make_shared<LogLayout>();
        result->identity = request;
        result->metrics = QImage(1, 1, QImage::Format_ARGB32_Premultiplied);
        result->metrics.setDevicePixelRatio(request.dpr);
        result->metrics.setDotsPerMeterX(qRound(request.dpiX / 0.0254));
        result->metrics.setDotsPerMeterY(qRound(request.dpiY / 0.0254));
        const QFontMetricsF metrics(request.font, &result->metrics);
        QTextOption option;
        option.setWrapMode(QTextOption::WrapAtWordBoundaryOrAnywhere);
        option.setTabStopDistance(metrics.horizontalAdvance(QLatin1Char(' ')) * 4);
        const QString& raw = request.source->raw;
        int start = 0;
        while (start <= raw.size())
        {
            if (token.isCanceled()) return {};
            const qsizetype separator = raw.indexOf(QChar::ParagraphSeparator, start);
            const int end = separator < 0 ? static_cast<int>(raw.size()) : static_cast<int>(separator);
            Paragraph paragraph;
            paragraph.start = start;
            paragraph.length = end - start;
            paragraph.top = result->height;
            paragraph.firstLine = static_cast<int>(result->lines.size());
            paragraph.layout = std::make_unique<QTextLayout>(raw.mid(start, end - start), request.font, &result->metrics);
            paragraph.layout->setTextOption(option);
            paragraph.layout->setCacheEnabled(true);
            paragraph.layout->beginLayout();
            for (;;)
            {
                QTextLine line = paragraph.layout->createLine();
                if (!line.isValid()) break;
                line.setLeadingIncluded(true);
                line.setLineWidth(std::max(1, request.width));
                line.setPosition(QPointF(0, result->height - paragraph.top));
                const qreal height = std::max<qreal>(1, line.height() + (line.leading() < 0 ? std::ceil(line.leading()) : 0));
                result->lines.push_back({static_cast<int>(result->paragraphs.size()),
                    start + line.textStart(), line.textLength(), result->height, height, line});
                result->height += height;
                if (token.isCanceled()) return {};
            }
            paragraph.layout->endLayout();
            if (result->lines.size() == static_cast<size_t>(paragraph.firstLine))
            {
                const qreal height = std::max<qreal>(1, metrics.height());
                result->lines.push_back({static_cast<int>(result->paragraphs.size()), start, 0, result->height, height, {}});
                result->height += height;
            }
            // Force shaping, glyph runs and cursor attributes to finish on the
            // worker. GUI draw()/xToCursor() never starts an unlaid paragraph.
            paragraph.layout->nextCursorPosition(0);
            for (size_t i = static_cast<size_t>(paragraph.firstLine); i < result->lines.size(); ++i)
            {
                if (token.isCanceled()) return {};
                if (result->lines[i].line.isValid()) result->lines[i].line.glyphRuns();
            }
            result->paragraphs.push_back(std::move(paragraph));
            if (separator < 0) break;
            start = end + 1;
        }
        // Boundary tables are prepared once, outside GUI input handlers. In
        // particular Ctrl+Right/double-click never scans a megabyte-long word.
        QTextBoundaryFinder characters(QTextBoundaryFinder::Grapheme, raw);
        result->characters.push_back(0);
        for (int at = characters.toNextBoundary(); at >= 0; at = characters.toNextBoundary())
        {
            result->characters.push_back(at);
            if ((result->characters.size() & 4095) == 0 && token.isCanceled()) return {};
        }
        QTextBoundaryFinder words(QTextBoundaryFinder::Word, raw);
        for (int at = 0; at >= 0; at = words.toNextBoundary())
        {
            const auto reason = words.boundaryReasons();
            if (reason & QTextBoundaryFinder::StartOfItem) result->wordStarts.push_back(at);
            if (reason & QTextBoundaryFinder::EndOfItem) result->wordEnds.push_back(at);
            if (token.isCanceled()) return {};
        }
        return retained(std::move(result));
    }

    int nextBoundary(const std::vector<int>& boundaries, int position, int fallback)
    {
        const auto at = std::upper_bound(boundaries.begin(), boundaries.end(), position);
        return at == boundaries.end() ? fallback : *at;
    }
    int previousBoundary(const std::vector<int>& boundaries, int position)
    {
        const auto at = std::lower_bound(boundaries.begin(), boundaries.end(), position);
        return at == boundaries.begin() ? 0 : *std::prev(at);
    }
    class Gutter final : public QWidget
    {
    public:
        Gutter(QWidget* parent, std::function<void(QPaintEvent*)> paint)
            : QWidget(parent), m_paint(std::move(paint))
        {
            setObjectName(QStringLiteral("code_editor_gutter"));
            setAttribute(Qt::WA_OpaquePaintEvent);
        }
    protected:
        void paintEvent(QPaintEvent* event) override { m_paint(event); }
    private:
        std::function<void(QPaintEvent*)> m_paint;
    };
    struct CopyValue
    {
        std::shared_ptr<const QString> raw;
        quint64 revision = 0;
        int first = 0, length = 0;
    };
    struct SelectAllPoint
    {
        bool hasText = false;
        quint64 revision = 0;
    };
}

struct AsyncLogTextView::State
{
    AsyncLogTextView* q;
    AsyncOperation* sourceOperation;
    AsyncOperation* layoutOperation;
    CompleteExportOperation* copyOperation;
    QWidget* gutter;
    QTimer* dragTimer;
    QTimer* caretTimer;
    bool caretOn = true;
    std::shared_ptr<const LogSource> source = retained(std::make_shared<LogSource>());
    LayoutReply layout;
    std::vector<Command> pending;
    std::shared_ptr<const std::vector<Command>> sourceCommands; // 已冻结的最早批次；拒绝/失败时保持所有权，无分配恢复。
    std::map<quint64, std::function<void(const SnapshotReply&)>> callbacks;
    QString placeholder;
    QString failure;
    QPalette surfacePalette;
    CodeTextSurfaceColors surface;
    bool surfaceKnown = false;
    QFont applicationFont = QApplication::font();
    double fontScale = 1;
    quint64 revision = 0, nextSnapshot = 0, epoch = 0;
    int cursor = 0, anchor = 0, maximum = 0;
    quint64 selectionSequence = 0;
    bool selectAllPending = false;
    std::shared_ptr<SelectAllPoint> selectAllPoint;
    qreal desiredX = -1;
    bool sourceBusy = false, layoutPending = true, layoutQueued = false;
    bool followBottom = true, revealCursor = false, updatingFont = false;
    bool fontQueued = false, dragging = false, wordDrag = false;
    int wordFirst = 0, wordLast = 0;
    QPoint dragPoint, dragStart;
    bool mightStartTextDrag = false;
    int pressPosition = 0;
    std::shared_ptr<const LogSource> pressSource;

    explicit State(AsyncLogTextView* view) : q(view), sourceOperation(new AsyncOperation(view)),
        layoutOperation(new AsyncOperation(view)), copyOperation(new CompleteExportOperation(view)),
        gutter(new Gutter(view, [this](QPaintEvent* event) { paintGutter(event); })), dragTimer(new QTimer(view)), caretTimer(new QTimer(view))
    {
        QObject::connect(caretTimer, &QTimer::timeout, view, [this] {
            if (!q->hasFocus() || !q->isVisible()) { caretTimer->stop(); return; }
            caretOn = !caretOn; q->viewport()->update();
        });
        dragTimer->setInterval(35);
        QObject::connect(dragTimer, &QTimer::timeout, view, [this] {
            if (!dragging || !ready()) { dragTimer->stop(); return; }
            const int y = dragPoint.y();
            if (y >= 0 && y < q->viewport()->height()) { dragTimer->stop(); return; }
            const int direction = y < 0 ? -1 : 1;
            q->verticalScrollBar()->setValue(q->verticalScrollBar()->value()
                + direction * std::max(1, q->fontMetrics().height()));
            extendDrag(dragPoint);
        });
        QObject::connect(q->verticalScrollBar(), &QScrollBar::valueChanged, view, [this] {
            followBottom = q->verticalScrollBar()->value() == q->verticalScrollBar()->maximum();
            gutter->update();
        });
    }
    const CodeTextSurfaceColors& surfaceColors()
    {
        const QPalette current = q->palette();
        if (!surfaceKnown || surfacePalette != current)
        { surfacePalette = current; surface = CodeTextSurfaceColorsFor(current); surfaceKnown = true; }
        return surface;
    }
    bool ready() const
    {
        return layout && !sourceBusy && !sourceCommands && pending.empty() && !layoutPending
            && layout->identity.source->revision == source->revision;
    }
    int gutterWidth() const
    {
        const int digits = std::max(2, static_cast<int>(QString::number(std::max(1, source->blocks)).size()));
        return 22 + q->fontMetrics().horizontalAdvance(QLatin1Char('9')) * digits;
    }
    bool geometry()
    {
        const QPointer<AsyncLogTextView> guard(q);
        const int width = gutterWidth();
        const QMargins margins(width + kInset, kInset, kInset, kInset);
        if (q->viewportMargins() != margins) q->setViewportMargins(margins);
        if (!guard) return false;
        const QRect rect = q->contentsRect();
        gutter->setGeometry(rect.left(), rect.top(), width, rect.height());
        // 两次几何写入都可能同步分发事件；返回值只读取栈上的弱引用。
        return !guard.isNull();
    }
    qreal scrollTop() const
    {
        if (!layout) return 0;
        const qreal extent = std::max<qreal>(0, layout->height - q->viewport()->height());
        const int maximumValue = q->verticalScrollBar()->maximum();
        return maximumValue ? extent * q->verticalScrollBar()->value() / maximumValue : 0;
    }
    void setScrollTop(qreal value)
    {
        if (!layout) return;
        const qreal extent = std::max<qreal>(0, layout->height - q->viewport()->height());
        q->verticalScrollBar()->setValue(extent > 0 ? qRound(std::clamp(value / extent, qreal(0), qreal(1))
            * q->verticalScrollBar()->maximum()) : 0);
    }
    int lineAtY(qreal y) const
    {
        if (!layout || layout->lines.empty()) return -1;
        const auto at = std::upper_bound(layout->lines.begin(), layout->lines.end(), y,
            [](qreal value, const VisualLine& line) { return value < line.top; });
        return at == layout->lines.begin() ? 0 : static_cast<int>(std::distance(layout->lines.begin(), at) - 1);
    }
    int lineAtPosition(int position) const
    {
        if (!layout || layout->lines.empty()) return -1;
        const auto at = std::upper_bound(layout->lines.begin(), layout->lines.end(), position,
            [](int value, const VisualLine& line) { return value < line.start; });
        return at == layout->lines.begin() ? 0 : static_cast<int>(std::distance(layout->lines.begin(), at) - 1);
    }
    int hit(const QPoint& point) const
    {
        const int index = lineAtY(scrollTop() + point.y());
        if (index < 0) return 0;
        const VisualLine& visual = layout->lines[static_cast<size_t>(index)];
        const Paragraph& paragraph = layout->paragraphs[static_cast<size_t>(visual.paragraph)];
        return visual.line.isValid() ? paragraph.start + visual.line.xToCursor(point.x()) : visual.start;
    }
    void selection(int newAnchor, int newCursor, bool resetX = true, bool manual = true)
    {
        if (manual) { ++selectionSequence; selectAllPending = false; selectAllPoint.reset(); }
        const int length = static_cast<int>(source->raw.size());
        newAnchor = std::clamp(newAnchor, 0, length);
        newCursor = std::clamp(newCursor, 0, length);
        if (resetX) desiredX = -1;
        if (anchor == newAnchor && cursor == newCursor) return;
        anchor = newAnchor;
        cursor = newCursor;
        caretOn = true;
        if (caretTimer->isActive()) caretTimer->start();
        q->viewport()->update(); gutter->update();
        const QPointer<AsyncLogTextView> guard(q);
        emit q->selectionChanged();
#if QT_CONFIG(accessibility)
        if (guard && QAccessible::isActive())
        {
            QAccessibleTextSelectionEvent changed(q, std::min(anchor,cursor), std::max(anchor,cursor));
            QAccessible::updateAccessibility(&changed);
        }
#endif
    }
    void requestLayout()
    {
        layoutPending = true;
        ++epoch;
        // Invalidating active work immediately prevents an old width/font result
        // from being committed in the interval before the coalesced request.
        layoutOperation->cancel();
        if (!q->isVisible() || layoutQueued) return;
        layoutQueued = true;
        QTimer::singleShot(0, q, [this] {
            layoutQueued = false;
            if (!q->isVisible() || sourceBusy || sourceCommands || !pending.empty()) return;
            if (!geometry()) return; // 宿主已销毁时，不再读取 State 或构造请求。
            LayoutRequest request{source, q->font(), std::max(1, q->viewport()->width()),
                q->logicalDpiX(), q->logicalDpiY(), q->devicePixelRatioF(), epoch};
            failure.clear();
            layoutOperation->submit<LayoutRequest, LayoutReply>(request, prepareLayout,
                [this](LayoutReply& prepared, quint64) {
                    if (!prepared) return true;
                    const auto& id = prepared->identity;
                    if (id.epoch != epoch || id.source->revision != source->revision
                        || id.font != q->font() || id.width != q->viewport()->width()
                        || id.dpiX != q->logicalDpiX() || id.dpiY != q->logicalDpiY()
                        || id.dpr != q->devicePixelRatioF() || !q->isVisible()) return true;
                    const bool bottom = followBottom;
                    int topPosition = 0;
                    qreal intra = 0;
                    if (layout)
                    {
                        const int topLine = lineAtY(scrollTop());
                        const auto& line = layout->lines[static_cast<size_t>(topLine)];
                        const quint64 absolute = layout->identity.source->origin + static_cast<quint64>(line.start);
                        topPosition = absolute < source->origin ? 0 : static_cast<int>(absolute - source->origin);
                        intra = scrollTop() - line.top;
                    }
                    layout = std::move(prepared); // Retained owner retires the old complete layout off GUI.
                    layoutPending = false;
                    const qreal extent = std::max<qreal>(0, layout->height - q->viewport()->height());
                    const int range = static_cast<int>(std::min<qreal>(std::ceil(extent), std::numeric_limits<int>::max()));
                    const QPointer<AsyncLogTextView> guard(q);
                    q->verticalScrollBar()->setRange(0, range);
                    if (!guard) return true;
                    q->verticalScrollBar()->setPageStep(std::max(1, q->viewport()->height()));
                    q->verticalScrollBar()->setSingleStep(std::max(1, q->fontMetrics().height()));
                    if (bottom) q->verticalScrollBar()->setValue(range);
                    else
                    {
                        const int index = lineAtPosition(std::min(topPosition, static_cast<int>(source->raw.size())));
                        setScrollTop(layout->lines[static_cast<size_t>(index)].top + intra);
                    }
                    if (!guard) return true;
                    if (revealCursor) { revealCursor = false; q->ensureCursorVisible(); }
                    if (!guard) return true;
                    q->viewport()->update(); gutter->update();
                    return true;
                }, [this](std::exception_ptr) {
                    // Keep the last complete projection. A later append, Show,
                    // resize or font/DPI change can retry, without a timer loop.
                    layoutPending = true;
                    failure = QStringLiteral("后台任务发生异常。");
                    q->viewport()->update();
                });
        });
    }
    void drain()
    {
        if (sourceBusy || (!sourceCommands && pending.empty())) return;
        const QPointer<AsyncLogTextView> admissionOwner(q);
        try
        {
            if (!sourceCommands)
            {
                // 先完成可能失败的 owner/retirement 分配，再无异常地转交 pending。
                // 保留批次本体而非逐条复制，失败后始终先重试它，再消费新命令。
                auto commands = std::make_shared<std::vector<Command>>();
                auto retainedCommands = retained(commands);
                commands->swap(pending);
                sourceCommands = std::move(retainedCommands);
            }
            const SourceRequest request{source, sourceCommands};
            sourceBusy = true;
            failure.clear();
            const auto ticket = sourceOperation->submitTransaction<SourceRequest, SourceReply>(request, prepareSource,
                [this](SourceReply& reply, quint64) {
                    const QPointer<AsyncLogTextView> guard(q);
                    const auto old = source;
                    source = std::move(reply.source);
                    sourceCommands.reset(); // 提交点：规范源已更新，此批 append 永远不再重放。
                    const quint64 removed = source->origin - old->origin;
                    const auto adjusted = [&](int value) {
                        return adjustLogPosition(value, static_cast<int>(old->raw.size()), static_cast<int>(source->raw.size()),
                            removed, source->lastEmptyRevision > old->revision);
                    };
                    const bool moved = cursor != adjusted(cursor) || anchor != adjusted(anchor);
                    cursor = adjusted(cursor); anchor = adjusted(anchor);
                    bool observerFailed = false;
                    try { if (!geometry()) return true; } catch (...) { observerFailed = true; }
                    if (!guard) return true;
                    if (moved)
                    {
                        try { emit q->selectionChanged(); } catch (...) { observerFailed = true; }
                        if (!guard) return true;
                    }
                    if (old->revision != source->revision)
                    {
                        try { emit q->textChanged(); } catch (...) { observerFailed = true; }
                        if (!guard) return true;
#if QT_CONFIG(accessibility)
                        try
                        {
                            if (QAccessible::isActive())
                            {
                                QAccessibleTextUpdateEvent changed(q,0,old->plain,source->plain);
                                QAccessible::updateAccessibility(&changed);
                            }
                        }
                        catch (...) { observerFailed = true; }
#endif
                    }
                    if (!guard) return true;
                    // 回调异常只影响该观察者；后续快照仍按原顺序投递，已提交日志不重复。
                    for (auto& snapshot : reply.snapshots)
                    {
                        const auto found = callbacks.find(snapshot.id);
                        if (found == callbacks.end()) continue;
                        auto callback = std::move(found->second);
                        callbacks.erase(found);
                        try { callback(snapshot); } catch (...) { observerFailed = true; }
                        if (!guard) return true;
                    }
                    sourceBusy = false;
                    try
                    {
                        if (old->revision != source->revision || (layoutPending && !layoutOperation->isBusy())) requestLayout();
                    }
                    catch (...)
                    {
                        observerFailed = true;
                        layoutPending = true;
                    }
                    if (!guard) return true;
                    if (observerFailed)
                    {
                        failure = QStringLiteral("后台任务发生异常。");
                        q->viewport()->update();
                    }
                    // 保留原来的下一轮接续点，等当前事务 settle 后才请求下一批。
                    QTimer::singleShot(0, q, [this] { drain(); });
                    return true;
                }, [this](std::exception_ptr) {
                    // worker/启动失败仍由 sourceCommands 持有原批次，无需分配恢复队列。
                    // 若提交后调度异常，此指针已清空，也不会重放已发布 append。
                    sourceBusy = false;
                    layoutPending = true;
                    failure = QStringLiteral("后台任务发生异常。");
                    q->viewport()->update();
                });
            if (ticket == 0 && admissionOwner)
            {
                // 未准入不会调用 apply/failure；本地批次仍完整保留，等待原有下一次触发。
                sourceBusy = false;
                layoutPending = true;
            }
        }
        catch (...)
        {
            if (!admissionOwner) return;
            sourceBusy = false;
            layoutPending = true;
            failure = QStringLiteral("后台任务发生异常。");
            q->viewport()->update();
        }
    }
    void capture(std::function<void(const SnapshotReply&)> callback)
    {
        if (!sourceBusy && !sourceCommands && pending.empty())
        {
            // Current canonical values already include every admitted append.
            // Share them directly: a read must not re-normalize/re-layout text.
            SnapshotReply snapshot{++nextSnapshot,source->revision,
                std::shared_ptr<const QString>(source,&source->raw),source->origin,source->lastEmptyRevision};
            callback(snapshot);
            return;
        }
        const quint64 id = ++nextSnapshot;
        callbacks.emplace(id, std::move(callback));
        pending.push_back({CommandKind::Snapshot, {}, 0, revision, id});
        drain();
    }
    void publishCopy(std::shared_ptr<const CopyValue> value)
    {
        if (!value || value->length <= 0 || copyOperation->isBusy()) return;
        auto input = MakeCompleteExportInput(value, QStringLiteral("async_log_selection"), value->revision,
            std::nullopt, ks::i18n::SourceTextSnapshot{},
            [](const CopyValue& snapshot, CompleteExportSink& sink, const ks::i18n::SourceTextSnapshot&) {
                QTextDocument document;
                document.setLayoutEnabled(false);
                document.setUndoRedoEnabled(false);
                QTextCursor cursor(&document);
                cursor.insertText(*snapshot.raw);
                return sink.writeText(QStringView(document.toPlainText()).mid(snapshot.first,snapshot.length));
            });
        copyOperation->startCopy(std::move(input), [](const QString& text) {
            if (!QApplication::clipboard()) return false;
            QApplication::clipboard()->setText(text);
            return true;
        }, {});
    }
    void fontChange()
    {
        if (updatingFont) return;
        if (QApplication::font() == applicationFont)
        {
            const QFont current = q->font();
            if (current.pointSizeF() > 0 && applicationFont.pointSizeF() > 0)
                fontScale = current.pointSizeF() / applicationFont.pointSizeF();
            else if (current.pixelSize() > 0 && applicationFont.pixelSize() > 0)
                fontScale = static_cast<double>(current.pixelSize()) / applicationFont.pixelSize();
            else
            {
                const int base = QFontInfo(applicationFont).pixelSize();
                if (base > 0) fontScale = static_cast<double>(QFontInfo(current).pixelSize()) / base;
            }
        }
        if (!geometry()) return;
        requestLayout();
    }
    void applicationFontChange()
    {
        if (fontQueued) return;
        fontQueued = true;
        QTimer::singleShot(0, q, [this] {
            fontQueued = false;
            const QFont updatedApplication = QApplication::font();
            if (applicationFont == updatedApplication) return;
            QFont updated = q->font();
            applicationFont = updatedApplication;
            if (applicationFont.pointSizeF() > 0) updated.setPointSizeF(applicationFont.pointSizeF() * fontScale);
            else if (applicationFont.pixelSize() > 0) updated.setPixelSize(std::max(1, qRound(applicationFont.pixelSize() * fontScale)));
            const QPointer<AsyncLogTextView> guard(q);
            updatingFont = true;
            q->setFont(updated);
            if (!guard) return;
            updatingFont = false;
            if (!geometry()) return;
            requestLayout();
        });
    }
    void paintGutter(QPaintEvent*)
    {
        QPainter painter(gutter);
        const auto& surface = surfaceColors();
        const QColor background = surface.gutter;
        const QColor active = surface.currentNumber;
        painter.fillRect(gutter->rect(), background);
        painter.setPen(surface.divider);
        painter.drawLine(gutter->width() - 1, 0, gutter->width() - 1, gutter->height());
        if (!layout) return;
        painter.setClipRect(gutter->rect().adjusted(0, kInset, 0, -kInset));
        QFont numberFont = q->font();
        if (numberFont.pointSizeF() > 8) numberFont.setPointSizeF(std::max(8.0, numberFont.pointSizeF() - 1));
        const qreal top = scrollTop();
        int first = lineAtY(top);
        const qint64 projectedCursor = static_cast<qint64>(source->origin) + cursor
            - static_cast<qint64>(layout->identity.source->origin);
        for (size_t i = static_cast<size_t>(std::max(0, first)); i < layout->lines.size(); ++i)
        {
            const auto& line = layout->lines[i];
            if (line.top - top > q->viewport()->height()) break;
            const auto& paragraph = layout->paragraphs[static_cast<size_t>(line.paragraph)];
            if (paragraph.firstLine != static_cast<int>(i)) continue;
            const bool current = projectedCursor >= paragraph.start && projectedCursor <= paragraph.start + paragraph.length;
            numberFont.setBold(current); painter.setFont(numberFont);
            painter.setPen(current ? active : surface.gutterText);
            const int y = qRound(line.top - top) + kInset;
            painter.drawText(5, y, gutter->width() - 14, q->fontMetrics().height(),
                Qt::AlignRight | Qt::AlignVCenter, QString::number(line.paragraph + 1));
            if (current) painter.fillRect(3, y + 3, 2, std::max(3, q->fontMetrics().height() - 6), active);
        }
    }
    std::pair<int, int> wordAt(int position) const
    {
        const auto start = std::upper_bound(layout->wordStarts.begin(), layout->wordStarts.end(), position);
        const int first = start == layout->wordStarts.begin() ? 0 : *std::prev(start);
        const auto end = std::upper_bound(layout->wordEnds.begin(), layout->wordEnds.end(), position);
        const int last = end == layout->wordEnds.end() ? static_cast<int>(source->raw.size()) : *end;
        // Whitespace is selected as its own run instead of swallowing a word.
        const auto priorEnd = std::upper_bound(layout->wordEnds.begin(), layout->wordEnds.end(), position);
        const int previousEnd = priorEnd == layout->wordEnds.begin() ? 0 : *std::prev(priorEnd);
        if (previousEnd > first)
        {
            const int next = start == layout->wordStarts.end() ? static_cast<int>(source->raw.size()) : *start;
            return {previousEnd, next};
        }
        return {first, last};
    }
    void extendDrag(const QPoint& point)
    {
        const int position = hit(point);
        if (!wordDrag) { selection(anchor, position); return; }
        const auto word = wordAt(position);
        if (position < wordFirst) selection(wordLast, word.first);
        else selection(wordFirst, word.second);
    }
};

#if QT_CONFIG(accessibility)
class AsyncLogTextAccessible final : public QAccessibleWidget, public QAccessibleTextInterface
{
public:
    explicit AsyncLogTextAccessible(AsyncLogTextView* view) : QAccessibleWidget(view, QAccessible::EditableText) {}
    AsyncLogTextView* view() const { auto* v = static_cast<AsyncLogTextView*>(widget()); return v && v->d ? v : nullptr; }
    void* interface_cast(QAccessible::InterfaceType type) override
    { return type == QAccessible::TextInterface ? static_cast<QAccessibleTextInterface*>(this) : QAccessibleWidget::interface_cast(type); }
    QAccessible::State state() const override
    { auto state = QAccessibleWidget::state(); state.readOnly = true; state.editable = false; state.multiLine = true; state.selectableText = true; return state; }
    QString text(QAccessible::Text type) const override
    { return type == QAccessible::Value && view() ? view()->toPlainText() : QAccessibleWidget::text(type); }
    void selection(int index, int* first, int* last) const override
    { const auto* v = view(); if (first) *first = index == 0 && v ? std::min(v->anchorPosition(), v->cursorPosition()) : -1; if (last) *last = index == 0 && v ? std::max(v->anchorPosition(), v->cursorPosition()) : -1; }
    int selectionCount() const override { const auto* v = view(); return v && v->anchorPosition() != v->cursorPosition() ? 1 : 0; }
    void addSelection(int first, int last) override { setSelection(0, first, last); }
    void removeSelection(int index) override { if (index == 0 && view()) setCursorPosition(view()->cursorPosition()); }
    void setSelection(int index, int first, int last) override
    { if (index == 0) if (auto* v = view()) v->selectRange(first, last, v->d->source->revision); }
    int cursorPosition() const override { return view() ? view()->cursorPosition() : 0; }
    void setCursorPosition(int position) override
    { if (auto* v = view()) v->selectRange(position, position, v->d->source->revision); }
    QString text(int first, int last) const override
    { const auto* v = view(); if (!v) return {}; const QString text = v->toPlainText(); if (last < 0) last = static_cast<int>(text.size()); return first >= 0 && first <= last && last <= text.size() ? text.mid(first, last - first) : QString(); }
    int characterCount() const override { return view() ? static_cast<int>(view()->d->source->raw.size()) : 0; }
    QRect characterRect(int offset) const override
    {
        const auto* v = view(); if (!v || !v->d->layout) return {};
        const auto& state = *v->d;
        const qint64 position = static_cast<qint64>(state.source->origin) + offset - static_cast<qint64>(state.layout->identity.source->origin);
        if (offset < 0 || position < 0 || position > state.layout->identity.source->raw.size()) return {};
        const auto& line = state.layout->lines[static_cast<size_t>(state.lineAtPosition(static_cast<int>(position)))];
        const auto& paragraph = state.layout->paragraphs[static_cast<size_t>(line.paragraph)];
        const qreal x = line.line.isValid() ? line.line.cursorToX(static_cast<int>(position) - paragraph.start) : 0;
        const qreal x2 = line.line.isValid() ? line.line.cursorToX(static_cast<int>(position) - paragraph.start, QTextLine::Trailing) : x + 1;
        const QPoint global = v->viewport()->mapToGlobal(QPoint(qFloor(std::min(x,x2)),qFloor(line.top-state.scrollTop())));
        return QRect(global,QSize(std::max(1,qCeil(std::abs(x2-x))),std::max(1,qCeil(line.height))));
    }
    int offsetAtPoint(const QPoint& point) const override
    {
        const auto* v = view(); if (!v || !v->d->layout) return -1;
        const qint64 position = static_cast<qint64>(v->d->layout->identity.source->origin) + v->d->hit(v->viewport()->mapFromGlobal(point)) - static_cast<qint64>(v->d->source->origin);
        return position < 0 || position > v->d->source->raw.size() ? -1 : static_cast<int>(position);
    }
    void scrollToSubstring(int first, int) override
    {
        if (auto* v = view(); v && v->d->ready())
        { const int row=v->d->lineAtPosition(std::clamp(first,0,static_cast<int>(v->d->source->raw.size()))); v->d->setScrollTop(v->d->layout->lines[static_cast<size_t>(row)].top); }
    }
    QString attributes(int offset, int* first, int* last) const override
    { if (first) *first = offset >= 0 && offset <= characterCount() ? 0 : -1; if (last) *last = offset >= 0 && offset <= characterCount() ? characterCount() : -1; return {}; }
};
static QAccessibleInterface* LogTextAccessibleFactory(const QString&, QObject* object)
{ if (auto* view = qobject_cast<AsyncLogTextView*>(object)) return new AsyncLogTextAccessible(view); return nullptr; }
#endif

AsyncLogTextView::AsyncLogTextView(QWidget* parent) : QAbstractScrollArea(parent)
{
#if QT_CONFIG(accessibility)
    static const bool accessibleInstalled = [] { QAccessible::installFactory(LogTextAccessibleFactory); return true; }();
    Q_UNUSED(accessibleInstalled);
#endif
    // Children can synchronously send construction/theme events to their host.
    // Keep d as a constructed null unique_ptr until State has finished.
    d = std::make_unique<State>(this);
    setProperty("ksword_preserve_custom_font", true);
    setProperty("ksword_detail_keep_layout", true);
    setFont(CodeTextEdit::editorFont());
    setFrameShape(QFrame::NoFrame);
    setBackgroundRole(QPalette::Base);
    setAutoFillBackground(true);
    viewport()->setBackgroundRole(QPalette::Base);
    viewport()->setAutoFillBackground(true);
    viewport()->setCursor(Qt::IBeamCursor);
    setFocusPolicy(Qt::StrongFocus);
    setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    d->fontChange();
}

AsyncLogTextView::~AsyncLogTextView()
{
    d->sourceOperation->close();
    d->layoutOperation->close();
    d->copyOperation->close();
    d->dragTimer->stop();
    d->caretTimer->stop();
    // Child signal/paint closures borrow State and must die before it does.
    delete d->gutter;
    delete d->dragTimer;
    delete d->caretTimer;
    delete d->sourceOperation;
    delete d->layoutOperation;
    delete d->copyOperation;
}

void AsyncLogTextView::appendPlainText(const QString& text)
{
    d->pending.push_back({CommandKind::Append, retained(std::make_shared<QString>(text)), 0, ++d->revision, 0});
    d->layoutPending = true;
    d->drain();
}
void AsyncLogTextView::setMaximumBlockCount(int count)
{
    count = std::max(0, count);
    if (count == d->maximum) return;
    d->maximum = count;
    d->pending.push_back({CommandKind::Maximum, {}, count, ++d->revision, 0});
    d->layoutPending = true;
    d->drain();
}
bool AsyncLogTextView::isEmpty() const { return d->source->raw.isEmpty(); }
void AsyncLogTextView::setPlaceholderText(const QString& text) { d->placeholder = text; viewport()->update(); }
bool AsyncLogTextView::isPreparing() const { return !d->ready(); }
QString AsyncLogTextView::toPlainText() const { return d->source->plain; }
QString AsyncLogTextView::selectedText() const { return d->source->raw.mid(std::min(d->anchor,d->cursor),std::abs(d->cursor-d->anchor)); }
int AsyncLogTextView::cursorPosition() const { return d->cursor; }
int AsyncLogTextView::anchorPosition() const { return d->anchor; }

bool AsyncLogTextView::selectRange(int anchor, int position, quint64 revision)
{
    if (anchor < 0 || position < 0 || revision > d->source->revision) return false;
    const auto& history = d->source->history;
    const auto entry = std::lower_bound(history.begin(), history.end(), revision,
        [](const RevisionOrigin& value, quint64 key) { return value.last < key; });
    if (entry == history.end() || revision < entry->first) return false;
    const quint64 removed = d->source->origin - entry->origin;
    if (static_cast<quint64>(std::min(anchor, position)) < removed) return false;
    const quint64 first = static_cast<quint64>(anchor) - removed;
    const quint64 last = static_cast<quint64>(position) - removed;
    if (std::max(first, last) > static_cast<quint64>(d->source->raw.size())) return false;
    d->selection(static_cast<int>(first), static_cast<int>(last));
    return true;
}
void AsyncLogTextView::withTextSnapshot(std::function<void(QString, quint64)> callback)
{
    if (!callback) return;
    d->capture([callback=std::move(callback)](const SnapshotReply& snapshot) {
        callback(*snapshot.text,snapshot.revision);
    });
}
void AsyncLogTextView::ensureCursorVisible()
{
    if (!d->ready()) { d->revealCursor = true; return; }
    const int index = d->lineAtPosition(d->cursor);
    if (index < 0) return;
    const auto& line = d->layout->lines[static_cast<size_t>(index)];
    const qreal top = d->scrollTop();
    if (line.top < top) d->setScrollTop(line.top);
    else if (line.top + line.height > top + viewport()->height())
        d->setScrollTop(line.top + line.height - viewport()->height());
}
void AsyncLogTextView::selectAll()
{
    const QPointer<AsyncLogTextView> guard(this);
    const quint64 sequence = ++d->selectionSequence;
    d->selectAllPending = true;
    auto point = std::make_shared<SelectAllPoint>();
    d->selectAllPoint = point;
    d->capture([guard, sequence, point](const SnapshotReply& snapshot) {
        point->hasText = !snapshot.text->isEmpty();
        point->revision = snapshot.revision;
        if (!guard || sequence != guard->d->selectionSequence) return;
        guard->d->selectAllPending = false;
        const int end = static_cast<int>(guard->d->source->raw.size());
        const bool selected = point->hasText && guard->d->source->lastEmptyRevision <= snapshot.revision;
        guard->d->selection(selected ? 0 : end,end,true,false);
    });
}
void AsyncLogTextView::copy()
{
    if (d->copyOperation->isBusy()) return;
    const auto all = d->selectAllPending ? d->selectAllPoint : std::shared_ptr<SelectAllPoint>();
    if (!all && d->anchor == d->cursor) return;
    const int anchor = d->anchor, position = d->cursor;
    const int previousLength = static_cast<int>(d->source->raw.size());
    const quint64 origin = d->source->origin, revision = d->source->revision;
    const QPointer<AsyncLogTextView> guard(this);
    // A copy command follows every append accepted before this key press;
    // Ctrl+A then Ctrl+C is ordered even while normalization is in flight.
    d->capture([guard, all, anchor, position, previousLength, origin, revision](const SnapshotReply& snapshot) {
        if (!guard) return;
        int first=0,last=0;
        if (all)
        {
            if (!all->hasText || snapshot.lastEmptyRevision > all->revision) return;
            last=static_cast<int>(snapshot.text->size());
        }
        else
        {
            const quint64 removed=snapshot.origin-origin;
            const bool emptied=snapshot.lastEmptyRevision>revision;
            first=adjustLogPosition(anchor,previousLength,static_cast<int>(snapshot.text->size()),removed,emptied);
            last=adjustLogPosition(position,previousLength,static_cast<int>(snapshot.text->size()),removed,emptied);
        }
        auto value=std::make_shared<CopyValue>();
        value->raw=snapshot.text;value->revision=snapshot.revision;
        value->first=std::min(first,last);value->length=std::abs(last-first);
        guard->d->publishCopy(retained(std::move(value)));
    });
}

void AsyncLogTextView::paintEvent(QPaintEvent*)
{
    QPainter painter(viewport());
    const QPalette colors = palette();
    const QPalette::ColorGroup selectionGroup = !isEnabled() ? QPalette::Disabled : hasFocus() ? QPalette::Active : QPalette::Inactive;
    const QColor base = colors.color(QPalette::Base);
    painter.fillRect(viewport()->rect(), base);
    painter.setFont(font());
    painter.setPen(colors.color(isEnabled() ? QPalette::Active : QPalette::Disabled, QPalette::Text));
    if (!d->layout)
    {
        if (!d->failure.isEmpty() || isPreparing())
        {
            painter.setPen(colors.color(QPalette::PlaceholderText));
            painter.drawText(viewport()->rect(), Qt::AlignTop | Qt::TextWordWrap,
                d->failure.isEmpty() ? ks::i18n::sourceText(QStringLiteral("正在加载...")) : ks::i18n::sourceText(d->failure));
        }
        else if (isEmpty())
        {
            painter.setPen(colors.color(QPalette::PlaceholderText));
            painter.drawText(viewport()->rect(), Qt::AlignTop | Qt::TextWordWrap, ks::i18n::sourceText(d->placeholder));
        }
        return;
    }
    const auto& projection = *d->layout;
    const qreal scroll = d->scrollTop();
    const qint64 delta = static_cast<qint64>(d->source->origin) - static_cast<qint64>(projection.identity.source->origin);
    const qint64 cursor = d->cursor + delta;
    const qint64 anchor = d->anchor + delta;
    const qint64 selectionFirst = std::min(anchor, cursor), selectionLast = std::max(anchor, cursor);
    const int cursorLine = cursor >= 0 && cursor <= projection.identity.source->raw.size()
        ? d->lineAtPosition(static_cast<int>(cursor)) : -1;
    const QColor currentLine = d->surfaceColors().currentLine;
    for (size_t i = static_cast<size_t>(std::max(0, d->lineAtY(scroll))); i < projection.lines.size(); ++i)
    {
        const auto& visual = projection.lines[i];
        if (visual.top - scroll > viewport()->height()) break;
        if (static_cast<int>(i) == cursorLine)
            painter.fillRect(QRectF(0, visual.top - scroll, viewport()->width(), visual.height), currentLine);
        const auto& paragraph = projection.paragraphs[static_cast<size_t>(visual.paragraph)];
        if (!visual.line.isValid()) continue;
        const qint64 first = std::max<qint64>(selectionFirst, paragraph.start);
        const qint64 last = std::min<qint64>(selectionLast, paragraph.start + paragraph.length);
        const bool selected = first < last;
        const int paragraphEnd = paragraph.start + paragraph.length;
        if (selectionFirst <= paragraphEnd && selectionLast > paragraphEnd
            && visual.start + visual.length == paragraphEnd)
        {
            const qreal x = visual.line.cursorToX(paragraph.length);
            painter.fillRect(QRectF(x, visual.top - scroll, std::max<qreal>(0, viewport()->width() - x), visual.height),
                colors.brush(selectionGroup, QPalette::Highlight));
        }
        const QPointF drawOrigin(0, paragraph.top - scroll);
        if (!selected) visual.line.draw(&painter, drawOrigin);
        if (selected)
        {
            // Public QTextLine::draw has no selection overload. Restrict the
            // second draw to visual cells on this visible line; never invoke
            // QTextLayout::draw, which scans from the paragraph's first line.
            const int begin = static_cast<int>(std::max<qint64>(first, visual.start));
            const int end = static_cast<int>(std::min<qint64>(last, visual.start + visual.length));
            QPainterPath cells;
            for (int at = begin; at < end;)
            {
                const qreal leading = visual.line.cursorToX(at - paragraph.start, QTextLine::Leading);
                const qreal trailing = visual.line.cursorToX(at - paragraph.start, QTextLine::Trailing);
                cells.addRect(QRectF(std::min(leading, trailing), visual.top - scroll,
                    std::abs(trailing - leading), visual.height));
                const int next = nextBoundary(projection.characters, at, end);
                at = std::max(at + 1, next);
            }
            QPainterPath unselected;
            unselected.addRect(viewport()->rect());
            painter.save();
            painter.setClipPath(unselected.subtracted(cells), Qt::IntersectClip);
            visual.line.draw(&painter,drawOrigin);
            painter.restore();
            painter.fillPath(cells, colors.brush(selectionGroup, QPalette::Highlight));
            painter.save();
            painter.setClipPath(cells, Qt::IntersectClip);
            painter.setPen(colors.color(selectionGroup, QPalette::HighlightedText));
            visual.line.draw(&painter, drawOrigin);
            painter.restore();
        }
    }
    if (hasFocus() && d->caretOn && cursorLine >= 0)
    {
        const auto& visual = projection.lines[static_cast<size_t>(cursorLine)];
        const auto& paragraph = projection.paragraphs[static_cast<size_t>(visual.paragraph)];
        const qreal x = visual.line.isValid() ? visual.line.cursorToX(static_cast<int>(cursor) - paragraph.start) : 0;
        const qreal width = std::max(1, style()->pixelMetric(QStyle::PM_TextCursorWidth, nullptr, this));
        painter.fillRect(QRectF(x, visual.top - scroll, width, visual.height), colors.brush(QPalette::Text));
    }
    if (isEmpty() && !d->placeholder.isEmpty())
    {
        painter.setPen(colors.color(QPalette::PlaceholderText));
        painter.drawText(viewport()->rect(), Qt::AlignTop | Qt::TextWordWrap, ks::i18n::sourceText(d->placeholder));
    }
    if (!d->failure.isEmpty() || isPreparing())
    {
        painter.setPen(d->failure.isEmpty() ? colors.color(QPalette::PlaceholderText)
            : KswordTheme::EnsureTextContrast(KswordTheme::ErrorColor(),base));
        painter.drawText(viewport()->rect(), Qt::AlignTop | Qt::AlignRight,
            d->failure.isEmpty() ? ks::i18n::sourceText(QStringLiteral("正在加载...")) : ks::i18n::sourceText(d->failure));
    }
}

void AsyncLogTextView::resizeEvent(QResizeEvent* event)
{
    const QPointer<AsyncLogTextView> guard(this);
    QAbstractScrollArea::resizeEvent(event);
    if (!guard || !d) return;
    if (!d->geometry()) return;
    d->requestLayout();
}
void AsyncLogTextView::showEvent(QShowEvent* event)
{
    const QPointer<AsyncLogTextView> guard(this);
    QAbstractScrollArea::showEvent(event);
    if (!guard || !d) return;
    d->applicationFontChange();
    if (!guard || !d->geometry()) return;
    d->drain();
    if (!guard) return;
    if (!d->ready()) d->requestLayout();
}
void AsyncLogTextView::hideEvent(QHideEvent* event)
{
    d->dragging = false; d->mightStartTextDrag = false; d->pressSource.reset();
    d->dragTimer->stop(); d->caretTimer->stop();
    d->layoutOperation->cancel(); ++d->epoch; d->layoutPending = true;
    QAbstractScrollArea::hideEvent(event);
}
void AsyncLogTextView::changeEvent(QEvent* event)
{
    const QPointer<AsyncLogTextView> guard(this);
    QAbstractScrollArea::changeEvent(event);
    if (!guard || !d) return;
    switch (event->type())
    {
    case QEvent::FontChange: d->fontChange(); break;
    case QEvent::ApplicationFontChange: d->applicationFontChange(); break;
    case QEvent::PaletteChange:
    case QEvent::ApplicationPaletteChange:
    case QEvent::EnabledChange:
    case QEvent::LanguageChange:
        viewport()->update(); d->gutter->update(); break;
    default: break;
    }
}
bool AsyncLogTextView::event(QEvent* event)
{
    const QPointer<AsyncLogTextView> guard(this);
    const bool handled = QAbstractScrollArea::event(event);
    if (!guard) return handled;
    if (d && (event->type() == QEvent::DevicePixelRatioChange || event->type() == QEvent::ScreenChangeInternal))
    {
        if (!d->geometry()) return handled;
        d->requestLayout();
    }
    return handled;
}
void AsyncLogTextView::scrollContentsBy(int, int) { viewport()->update(); d->gutter->update(); }

void AsyncLogTextView::keyPressEvent(QKeyEvent* event)
{
    if (event->matches(QKeySequence::Copy)) { copy(); event->accept(); return; }
    if (event->matches(QKeySequence::SelectAll)) { selectAll(); event->accept(); return; }
    if (!d->ready()) { QAbstractScrollArea::keyPressEvent(event); return; }
    const bool shift = event->modifiers().testFlag(Qt::ShiftModifier);
    const bool control = event->modifiers().testFlag(Qt::ControlModifier);
    int position = d->cursor;
    const int length = static_cast<int>(d->source->raw.size());
    const int index = d->lineAtPosition(position);
    const auto& line = d->layout->lines[static_cast<size_t>(index)];
    const auto& paragraph = d->layout->paragraphs[static_cast<size_t>(line.paragraph)];
    bool vertical = false;
    switch (event->key())
    {
    case Qt::Key_Left:
        if (!shift && d->anchor != position) position = std::min(d->anchor, position);
        else position = previousBoundary(control ? d->layout->wordStarts : d->layout->characters, position);
        break;
    case Qt::Key_Right:
        if (!shift && d->anchor != position) position = std::max(d->anchor, position);
        else position = nextBoundary(control ? d->layout->wordStarts : d->layout->characters, position, length);
        break;
    case Qt::Key_Home: position = control ? 0 : line.start; break;
    case Qt::Key_End:
        position = control ? length : line.start + line.length;
        if (!control && line.line.isValid() && line.line.lineNumber() < paragraph.layout->lineCount()-1
            && position > 0 && d->source->raw.at(position-1).isSpace()) --position;
        break;
    case Qt::Key_Up:
    case Qt::Key_Down:
    case Qt::Key_PageUp:
    case Qt::Key_PageDown:
    {
        if (control && (event->key() == Qt::Key_Up || event->key() == Qt::Key_Down))
        {
            verticalScrollBar()->setValue(verticalScrollBar()->value()
                + (event->key() == Qt::Key_Up ? -1 : 1) * fontMetrics().height());
            event->accept(); return;
        }
        vertical = true;
        if (d->desiredX < 0) d->desiredX = line.line.isValid()
            ? line.line.cursorToX(position - paragraph.start) : 0;
        const bool up = event->key() == Qt::Key_Up || event->key() == Qt::Key_PageUp;
        const bool page = event->key() == Qt::Key_PageUp || event->key() == Qt::Key_PageDown;
        const int target = page ? d->lineAtY(line.top + (up ? -1 : 1) * viewport()->height())
            : std::clamp(index + (up ? -1 : 1), 0, static_cast<int>(d->layout->lines.size()) - 1);
        const auto& next = d->layout->lines[static_cast<size_t>(target)];
        const auto& block = d->layout->paragraphs[static_cast<size_t>(next.paragraph)];
        position = next.line.isValid() ? block.start + next.line.xToCursor(d->desiredX) : next.start;
        break;
    }
    default: QAbstractScrollArea::keyPressEvent(event); return;
    }
    const QPointer<AsyncLogTextView> guard(this);
    d->selection(shift ? d->anchor : position, position, !vertical);
    if (!guard) return;
    ensureCursorVisible(); event->accept();
}
void AsyncLogTextView::mousePressEvent(QMouseEvent* event)
{
    if (event->button() != Qt::LeftButton || !d->ready()) { QAbstractScrollArea::mousePressEvent(event); return; }
    const QPointer<AsyncLogTextView> guard(this);
    setFocus(Qt::MouseFocusReason);
    if (!guard) { event->accept(); return; }
    const int position = d->hit(event->position().toPoint());
    if (!event->modifiers().testFlag(Qt::ShiftModifier) && d->anchor != d->cursor
        && position >= std::min(d->anchor,d->cursor) && position < std::max(d->anchor,d->cursor))
    {
        d->mightStartTextDrag = true; d->dragStart = event->position().toPoint();
        d->pressPosition = position; d->pressSource = d->source;
        event->accept(); return;
    }
    d->dragging = true; d->wordDrag = false;
    d->selection(event->modifiers().testFlag(Qt::ShiftModifier) ? d->anchor : position, position);
    event->accept();
}
void AsyncLogTextView::mouseMoveEvent(QMouseEvent* event)
{
    if (d->mightStartTextDrag && d->ready())
    {
        if ((event->position().toPoint()-d->dragStart).manhattanLength() < QApplication::startDragDistance())
        { event->accept(); return; }
        d->mightStartTextDrag = false; d->pressSource.reset();
        const QString text = d->source->plain.mid(std::min(d->anchor,d->cursor),std::abs(d->cursor-d->anchor));
        if (!text.isEmpty())
        {
            auto* drag = new QDrag(this);
            const QPointer<QDrag> lifetime(drag);
            auto* mime = new QMimeData;
            mime->setText(text); drag->setMimeData(mime);
            drag->exec(Qt::CopyAction);
            if (lifetime) lifetime->deleteLater();
        }
        event->accept(); return;
    }
    if (!d->dragging || !d->ready()) { QAbstractScrollArea::mouseMoveEvent(event); return; }
    d->dragPoint = event->position().toPoint();
    const QPointer<AsyncLogTextView> guard(this);
    d->extendDrag(d->dragPoint);
    if (!guard) return;
    if (d->dragPoint.y() < 0 || d->dragPoint.y() >= viewport()->height()) d->dragTimer->start();
    else d->dragTimer->stop();
    event->accept();
}
void AsyncLogTextView::mouseReleaseEvent(QMouseEvent* event)
{
    if (event->button() == Qt::LeftButton)
    {
        const QPointer<AsyncLogTextView> guard(this);
        if (d->mightStartTextDrag && d->pressSource)
        {
            const int position = adjustLogPosition(d->pressPosition,static_cast<int>(d->pressSource->raw.size()),
                static_cast<int>(d->source->raw.size()),d->source->origin-d->pressSource->origin,
                d->source->lastEmptyRevision>d->pressSource->revision);
            d->selection(position,position);
            if (!guard) { event->accept(); return; }
        }
        d->mightStartTextDrag = false; d->pressSource.reset();
        d->dragging = false; d->dragTimer->stop();
    }
    QAbstractScrollArea::mouseReleaseEvent(event);
}
void AsyncLogTextView::mouseDoubleClickEvent(QMouseEvent* event)
{
    if (event->button() != Qt::LeftButton || !d->ready()) { QAbstractScrollArea::mouseDoubleClickEvent(event); return; }
    const auto word = d->wordAt(d->hit(event->position().toPoint()));
    d->mightStartTextDrag = false; d->pressSource.reset();
    d->dragging = true; d->wordDrag = true;
    d->wordFirst = word.first; d->wordLast = word.second;
    d->selection(word.first, word.second); event->accept();
}
void AsyncLogTextView::wheelEvent(QWheelEvent* event)
{
    if (!event->modifiers().testFlag(Qt::ControlModifier)) { QAbstractScrollArea::wheelEvent(event); return; }
    const float delta = event->angleDelta().y() / 120.f;
    if (delta != 0.f)
    {
        QFont changed = font();
        const qreal size = changed.pointSizeF() + delta;
        if (size > 0) { changed.setPointSizeF(size); setFont(changed); }
    }
    event->accept();
}
void AsyncLogTextView::focusInEvent(QFocusEvent* event)
{
    const QPointer<AsyncLogTextView> guard(this);
    QAbstractScrollArea::focusInEvent(event);
    if (!guard || !d) return;
    d->caretOn = true;
    const int flash = QApplication::styleHints()->cursorFlashTime();
    if (flash > 0) d->caretTimer->start(std::max(1, flash / 2));
    viewport()->update();
}
void AsyncLogTextView::focusOutEvent(QFocusEvent* event)
{
    if (d) { d->caretTimer->stop(); d->caretOn = true; viewport()->update(); }
    QAbstractScrollArea::focusOutEvent(event);
}
void AsyncLogTextView::contextMenuEvent(QContextMenuEvent* event)
{
    // Qt's existing translated action vocabulary; the menu itself follows the
    // same explicit opaque theme as other shared context menus.
    auto* menu = new QMenu(this);
    menu->setAttribute(Qt::WA_DeleteOnClose);
    menu->setStyleSheet(KswordTheme::ContextMenuStyle());
    if (!d->failure.isEmpty())
    {
        auto* retry = menu->addAction(ks::i18n::sourceText(QStringLiteral("重试")));
        connect(retry, &QAction::triggered, this, [this] { d->drain(); d->requestLayout(); viewport()->update(); });
        menu->addSeparator();
    }
    QAction* copyAction = menu->addAction(ks::i18n::sourceText(QStringLiteral("复制")));
    copyAction->setShortcut(QKeySequence::Copy);
    copyAction->setEnabled(d->anchor != d->cursor && !d->copyOperation->isBusy());
    connect(copyAction, &QAction::triggered, this, &AsyncLogTextView::copy);
    QAction* allAction = menu->addAction(ks::i18n::sourceText(QStringLiteral("全选")));
    allAction->setShortcut(QKeySequence::SelectAll);
    allAction->setEnabled(!isEmpty() || d->sourceBusy || d->sourceCommands || !d->pending.empty());
    connect(allAction, &QAction::triggered, this, &AsyncLogTextView::selectAll);
    menu->popup(event->globalPos());
    event->accept();
}
}
