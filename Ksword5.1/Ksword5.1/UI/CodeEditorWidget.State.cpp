#include "CodeEditorWidget.h"

#include <QAction>
#include <QComboBox>
#include <QLineEdit>
#include <QLayout>
#include <QPlainTextEdit>
#include <QPointer>
#include <QScrollBar>
#include <QSignalBlocker>
#include <QTextCursor>
#include <QTextDocument>
#include <QToolButton>
#include <algorithm>

struct CodeEditorReadOnlyViewState
{
    QString query, gotoText;
    int position = 0, anchor = 0, vertical = 0, horizontal = 0;
    int queryCursor = 0, querySelection = -1, querySelectionLength = 0;
    int syntax = 0;
    bool findOpen = false, gotoOpen = false, wrap = true;
    bool matchCase = false, wholeWord = false, whitespace = false;
};

std::shared_ptr<const CodeEditorReadOnlyViewState> CodeEditorWidget::captureReadOnlyViewState() const
{
    if (!m_readOnlyMode || m_destroying) return {};
    // The private CodeTextEdit type lives in the implementation TU. Its one
    // direct, explicitly named public base is the shared editor's stable port.
    const auto* edit = findChild<QPlainTextEdit*>(QStringLiteral("code_editor_text"), Qt::FindDirectChildrenOnly);
    if (!edit) return {};
    auto state = std::make_shared<CodeEditorReadOnlyViewState>();
    const auto cursor = edit->textCursor();
    state->position = cursor.position(); state->anchor = cursor.anchor();
    state->vertical = edit->verticalScrollBar()->value();
    state->horizontal = edit->horizontalScrollBar()->value();
    state->query = m_findEdit->text(); state->gotoText = m_gotoLineEdit->text();
    state->queryCursor = m_findEdit->cursorPosition();
    state->querySelection = m_findEdit->selectionStart();
    state->querySelectionLength = m_findEdit->selectedText().size();
    state->findOpen = !m_findPanel->isHidden(); state->gotoOpen = !m_gotoPanel->isHidden();
    state->wrap = wordWrapEnabled(); state->syntax = m_languageCombo->currentIndex();
    state->matchCase = m_matchCaseButton->isChecked(); state->wholeWord = m_wholeWordButton->isChecked();
    state->whitespace = m_whitespaceAction->isChecked();
    return state;
}

bool CodeEditorWidget::restoreReadOnlyViewState(const std::shared_ptr<const CodeEditorReadOnlyViewState>& state)
{
    if (!m_readOnlyMode || m_destroying) return false;
    const auto retainedState = state; // caller member may be reset by synchronous callbacks
    const QPointer<CodeEditorWidget> alive(this);
    const QPointer<QPlainTextEdit> edit = findChild<QPlainTextEdit*>(QStringLiteral("code_editor_text"), Qt::FindDirectChildrenOnly);
    if (!edit) return false;
    const quint64 generation = ++m_readOnlyViewRestoreGeneration;
    const auto current = [&] { return alive && edit && generation == m_readOnlyViewRestoreGeneration; };
    const CodeEditorReadOnlyViewState defaults;
    const auto& value = retainedState ? *retainedState : defaults;
    const auto blocked = [&](QObject* object, const auto& operation)
    {
        const QPointer<QObject> target(object);
        QSignalBlocker blocker(object);
        operation();
        if (!target) blocker.dismiss();
        return current();
    };
    if (!blocked(m_findEdit, [&] { m_findEdit->setText(value.query); })
        || !blocked(m_gotoLineEdit, [&] { m_gotoLineEdit->setText(value.gotoText); })
        || !blocked(m_matchCaseButton, [&] { m_matchCaseButton->setChecked(value.matchCase); })
        || !blocked(m_wholeWordButton, [&] { m_wholeWordButton->setChecked(value.wholeWord); })) return true;
    if (value.querySelection >= 0 && value.querySelectionLength > 0)
        m_findEdit->setSelection(value.querySelection, value.querySelectionLength);
    else m_findEdit->setCursorPosition(value.queryCursor);
    if (!current()) return true;
    setWordWrapEnabled(value.wrap);
    if (!current()) return true;
    m_languageCombo->setCurrentIndex(std::clamp(value.syntax, 0, std::max(0, m_languageCombo->count() - 1)));
    if (!current()) return true;
    m_whitespaceAction->setChecked(value.whitespace);
    if (!current()) return true;
    // Setting visibility directly preserves panel state without stealing focus.
    m_findPanel->setVisible(value.findOpen);
    if (!current()) return true;
    m_gotoPanel->setVisible(value.gotoOpen);
    if (!current()) return true;
    // Restoring panel/wrap state changes the real text viewport height. Settle
    // this component's small fixed layout before scrollbar ranges clamp values.
    if (layout()) layout()->activate();
    if (!current()) return true;
    const int last = std::max(0, edit->document()->characterCount() - 1);
    QTextCursor cursor(edit->document());
    cursor.setPosition(std::clamp(value.anchor, 0, last));
    cursor.setPosition(std::clamp(value.position, 0, last), QTextCursor::KeepAnchor);
    edit->setTextCursor(cursor);
    if (!current()) return true;
    edit->verticalScrollBar()->setValue(value.vertical);
    if (!current()) return true;
    edit->horizontalScrollBar()->setValue(value.horizontal);
    if (!current()) return true;
    updateFindHighlights();
    return true;
}
