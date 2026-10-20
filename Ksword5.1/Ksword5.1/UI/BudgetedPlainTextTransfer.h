#pragma once

#include <QElapsedTimer>
#include <QPlainTextEdit>
#include <QPointer>
#include <QStringList>
#include <QTextCursor>
#include <QTextDocument>
#include <QThread>
#include <QTimer>

#include <algorithm>
#include <cstdint>
#include <functional>
#include <limits>
#include <utility>

namespace ks::ui
{
    // GUI-only transfer for an initially empty professional plain-text editor.
    // Every Qt document access stays on its owner thread. A turn inserts/selects
    // bounded UTF-16 spans and yields; snapshots leave whole-text joining to a
    // value worker. It never caps/truncates the canonical text.
    // Callers must not programmatically edit/replace the document while busy.
    // The editor is read-only until completion; cancel before save acceptance.
    class BudgetedPlainTextTransfer final : public QObject
    {
    public:
        using Completion = std::function<void(bool, QStringList)>;

        explicit BudgetedPlainTextTransfer(QPlainTextEdit* editor)
            : QObject(editor), m_editor(editor) {}

        bool isBusy() const { return m_busy; }

        bool load(QString text, Completion finished)
        {
            assertOwner();
            if (m_busy || !m_editor || !m_editor->document()
                || m_editor->document()->characterCount() != 1
                || m_editor->maximumBlockCount() != 0
                || text.size() > std::numeric_limits<int>::max() - 1) return false;
            if (!begin(true, std::move(finished))) return false;
            const QPointer<BudgetedPlainTextTransfer> self(this);
            m_text = std::move(text);
            m_cursor = QTextCursor(m_document);
            m_document->setUndoRedoEnabled(false); // Initially empty: no user undo history to discard.
            if (!self || !m_editor || !m_document) return false;
            queueTurn();
            return true;
        }

        bool snapshot(Completion finished)
        {
            assertOwner();
            if (m_busy || !m_editor || !m_editor->document()) return false;
            if (!begin(false, std::move(finished))) return false;
            m_total = std::max(0, m_document->characterCount() - 1); // Exclude Qt's final terminator.
            queueTurn();
            return true;
        }

        void cancel(bool notify = false)
        {
            assertOwner();
            if (!m_busy) return;
            finish(false, notify);
        }

    private:
        void assertOwner() const { Q_ASSERT(QThread::currentThread() == thread()); }

        bool begin(bool loading, Completion finished)
        {
            const QPointer<BudgetedPlainTextTransfer> self(this);
            m_loading = loading;
            m_busy = true;
            ++m_epoch;
            m_offset = 0;
            m_total = 0;
            m_parts.clear();
            m_document = m_editor->document();
            m_revision = m_document->revision();
            m_wasReadOnly = m_editor->isReadOnly();
            m_wasUndoEnabled = m_document->isUndoRedoEnabled();
            m_finished = std::move(finished);
            m_editor->setReadOnly(true);
            return self && m_editor && m_document;
        }

        void queueTurn()
        {
            const auto epoch = m_epoch;
            QTimer::singleShot(0, this, [this, epoch]() {
                if (m_busy && m_epoch == epoch) turn();
            });
        }

        bool valid() const
        {
            return m_editor && m_document && m_editor->document() == m_document
                && m_document->revision() == m_revision;
        }

        static int loadEnd(const QString& text, int offset)
        {
            int end = static_cast<int>(std::min<qsizetype>(text.size(), static_cast<qsizetype>(offset) + Span));
            if (end < text.size() && end > offset)
            {
                const QChar previous = text.at(end - 1), next = text.at(end);
                if ((previous.isHighSurrogate() && next.isLowSurrogate())
                    || (previous == QChar::CarriageReturn && next == QChar::LineFeed)) --end;
            }
            return end;
        }

        int snapshotEnd() const
        {
            int end = m_offset + std::min(Span, m_total - m_offset);
            if (end < m_total && end > m_offset
                && m_document->characterAt(end - 1).isHighSurrogate()
                && m_document->characterAt(end).isLowSurrogate()) --end;
            return end;
        }

        void turn()
        {
            assertOwner();
            if (!valid()) { finish(false, true); return; }
            const QPointer<BudgetedPlainTextTransfer> self(this);
            const auto epoch = m_epoch;
            QElapsedTimer budget;
            budget.start();
            int spans = 0;
            do
            {
                if (m_loading)
                {
                    if (m_offset == m_text.size()) { finish(true, true); return; }
                    const int end = loadEnd(m_text, m_offset);
                    // The cursor is independent of the user's selection/scroll position.
                    QTextCursor cursor = m_cursor;
                    cursor.insertText(m_text.mid(m_offset, end - m_offset));
                    if (!self || !m_busy || m_epoch != epoch) return;
                    if (!m_editor || !m_document || m_editor->document() != m_document)
                    { finish(false, true); return; }
                    m_cursor = std::move(cursor);
                    m_offset = end;
                    m_revision = m_document->revision();
                }
                else
                {
                    if (m_offset == m_total) { finish(true, true); return; }
                    const int end = snapshotEnd();
                    QTextCursor cursor(m_document);
                    cursor.setPosition(m_offset);
                    cursor.setPosition(end, QTextCursor::KeepAnchor);
                    QString part = cursor.selectedText();
                    // QTextDocument::toPlainText's documented ASCII substitutions.
                    part.replace(QChar(0x00a0), QLatin1Char(' '));
                    part.replace(QChar::ParagraphSeparator, QChar::LineFeed);
                    part.replace(QChar::LineSeparator, QChar::LineFeed);
                    m_parts.push_back(std::move(part));
                    m_offset = end;
                }
                ++spans;
                if (!valid()) { finish(false, true); return; }
            } while (spans < MaxSpansPerTurn && budget.elapsed() < TurnBudgetMs);
            queueTurn();
        }

        void finish(bool success, bool notify)
        {
            const QPointer<BudgetedPlainTextTransfer> self(this);
            const auto editor = m_editor;
            const auto document = m_document;
            const bool loading = m_loading;
            const bool restoreReadOnly = m_wasReadOnly;
            const bool restoreUndo = m_wasUndoEnabled;
            auto completion = std::move(m_finished);
            QStringList parts = success ? std::move(m_parts) : QStringList();
            m_parts.clear();
            m_text.clear();
            m_cursor = QTextCursor();
            m_document.clear();
            m_busy = false;
            const auto settledEpoch = ++m_epoch;
            if (editor && document && editor->document() == document)
            {
                if (loading)
                {
                    document->setUndoRedoEnabled(restoreUndo);
                    if (!self || m_epoch != settledEpoch || !editor || !document) return;
                    if (success) document->setModified(false);
                    if (!self || m_epoch != settledEpoch || !editor || !document) return;
                    if (success) editor->setTextCursor(QTextCursor(document));
                    if (!self || m_epoch != settledEpoch || !editor || !document) return;
                }
                editor->setReadOnly(restoreReadOnly);
            }
            if (self && m_epoch == settledEpoch && notify && completion) completion(success, std::move(parts));
        }

        static constexpr int Span = 4096;
        static constexpr int MaxSpansPerTurn = 8;
        static constexpr qint64 TurnBudgetMs = 3;
        QPointer<QPlainTextEdit> m_editor;
        QPointer<QTextDocument> m_document;
        QTextCursor m_cursor;
        QString m_text;
        QStringList m_parts;
        Completion m_finished;
        std::uint64_t m_epoch = 0;
        int m_offset = 0;
        int m_total = 0;
        int m_revision = 0;
        bool m_busy = false;
        bool m_loading = false;
        bool m_wasReadOnly = false;
        bool m_wasUndoEnabled = false;
    };
}
