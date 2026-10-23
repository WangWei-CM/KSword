#include "StructuredFieldView.h"
#include "FieldTreeModel.h"

#include <QComboBox>
#include <QItemSelectionModel>
#include <QLineEdit>
#include <QPointer>
#include <QScrollBar>
#include <QSignalBlocker>
#include <QThreadPool>
#include <QTreeView>

namespace ks::ui
{
    struct StructuredFieldViewState
    {
        FieldDocument document;
        std::shared_ptr<const FieldTreeSelection> selection;
        QHash<QString, bool> expansion;
        QString query, selected, beforeSearchSelected;
        int column = 0, scroll = 0, horizontalScroll = 0;
        int beforeSearchColumn = 0, beforeSearchScroll = 0;
        StructuredFieldView::Presentation presentation = StructuredFieldView::Presentation::Sections;
        bool searching = false;

        bool matches(const FieldDocument& current) const
        {
            return document.title == current.title && document.nodes.isSharedWith(current.nodes);
        }
    };

    std::shared_ptr<const StructuredFieldViewState> StructuredFieldView::captureViewState() const
    {
        if (!m_valueTree || m_documentOnly) return {};
        flushValueSelection();
        auto state = std::make_shared<StructuredFieldViewState>();
        const auto displayed = m_valueModel->snapshot();
        // 已展示旧画面时读状态属于该快照；池复用尚未发布时，逻辑状态已由 restore 归属新规范对象。
        state->document = m_valuePublished && displayed ? displayed->document : m_document;
        if (m_valueSelection) state->selection = std::make_shared<FieldTreeSelection>(*m_valueSelection);
        state->expansion = m_valueExpansion;
        state->query = m_search->text();
        state->presentation = m_presentation;
        state->searching = m_valueSearchActive;
        const auto current = m_valueTree->currentIndex();
        const bool projected = m_valueTree->isVisible() && current.isValid() && !m_valueRestoreSelection;
        state->selected = projected ? current.data(FieldTreeKeyRole).toString() : m_valueSelected;
        state->column = projected ? current.column() : m_valueColumn;
        state->scroll = projected ? m_valueTree->verticalScrollBar()->value() : m_valueScroll;
        state->horizontalScroll = m_valueRestoreHorizontalScroll ? m_valueHorizontalScroll
            : m_valueTree->horizontalScrollBar()->value();
        state->beforeSearchSelected = m_valueBeforeSearchSelected;
        state->beforeSearchColumn = m_valueBeforeSearchColumn;
        state->beforeSearchScroll = m_valueBeforeSearchScroll;
        return state;
    }

    bool StructuredFieldView::restoreViewState(const std::shared_ptr<const StructuredFieldViewState>& state)
    {
        if (!m_valueTree || m_documentOnly) return false;
        // The caller may pass its own member by reference and release it from
        // a synchronous selection callback. Keep the immutable value alive.
        const auto retainedState = state;
        const bool accepted = retainedState && retainedState->matches(m_document);
        // 编辑器池可能刚换到另一行；只比较隐式共享身份，不扫描规范字段。
        // 普通刷新保留旧画面，但池复用不能把上一行数据短暂投到新对象下面。
        const bool displayedDocumentMatches = [this]
        {
            const auto displayed = m_valueModel->snapshot();
            return displayed && displayed->document.title == m_document.title
                && displayed->document.nodes.isSharedWith(m_document.nodes);
        }();
        if (!displayedDocumentMatches) m_valuePublished = false;
        if (m_valueCancel) m_valueCancel->store(true, std::memory_order_relaxed);
        const quint64 restoreTicket = ++m_valueTicket;
        // Read-only selection bases are immutable/COW values. Old large bases
        // and expansion hashes retire on the pool; no worker visits this view.
        auto oldSelection = std::move(m_valueSelection);
        auto oldExpansion = std::move(m_valueExpansion);
        QThreadPool::globalInstance()->start([oldSelection = std::move(oldSelection), oldExpansion = std::move(oldExpansion)]() mutable {
            oldSelection.reset(); oldExpansion.clear();
        });
        m_valueSelection = accepted && retainedState->selection
            ? std::make_shared<FieldTreeSelection>(*retainedState->selection) : std::make_shared<FieldTreeSelection>();
        m_valueExpansion = accepted ? retainedState->expansion : QHash<QString, bool>();
        // Clear the physical old occupant even when both documents have the
        // same topology (which otherwise needs no model reset). Suppress our
        // selection observer while retaining the new compact logical basis.
        const QPointer<StructuredFieldView> alive(this);
        const bool wasApplying = m_valueApplying;
        m_valueApplying = true;
        m_valueTree->selectionModel()->clear();
        if (!alive) return accepted;
        // A selection callback may start a newer projection or a nested pool
        // restore. Unwind only this synchronous observer-suppression scope;
        // never continue with the superseded row's reading state.
        m_valueApplying = wasApplying;
        if (m_valueTicket != restoreTicket) return accepted;
        m_valuePrepared = false;
        m_valueClearPending = false;
        m_valueRestoreSelection = true;
        m_valueSelected = accepted ? retainedState->selected : QString();
        m_valueColumn = accepted ? retainedState->column : 0;
        m_valueScroll = accepted ? retainedState->scroll : 0;
        m_valueHorizontalScroll = accepted ? retainedState->horizontalScroll : 0;
        m_valueRestoreHorizontalScroll = true;
        m_valueBeforeSearchSelected = accepted ? retainedState->beforeSearchSelected : QString();
        m_valueBeforeSearchColumn = accepted ? retainedState->beforeSearchColumn : 0;
        m_valueBeforeSearchScroll = accepted ? retainedState->beforeSearchScroll : 0;
        m_valueSearchActive = accepted && retainedState->searching;
        {
            const QSignalBlocker blocked(m_search);
            m_search->setText(accepted ? retainedState->query : QString());
        }
        {
            m_presentation = accepted ? retainedState->presentation : Presentation::Sections;
            const QSignalBlocker blocked(m_style);
            m_style->setCurrentIndex(m_presentation == Presentation::Tree ? 1 : 0);
        }
        applyPresentation();
        if (!alive || m_valueTicket != restoreTicket) return accepted;
        // Existing worker projection retains only present canonical keys and
        // incrementally restores exposed rows; all-selection stays compact.
        scheduleValueProjection();
        return accepted;
    }
}
