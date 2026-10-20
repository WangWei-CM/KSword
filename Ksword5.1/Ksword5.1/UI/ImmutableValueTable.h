#pragma once
#include "ImmutableValueTableModel.h"
#include "VisibleTableWidget.h"
#include "../Internationalization/LanguageManager.h"
#include <functional>
#include <memory>

namespace ks::ui
{
    struct ImmutableTableHeader
    {
        QString key, source;
        ImmutableTableHeader(QString sourceText) : source(std::move(sourceText)) {}
        ImmutableTableHeader(QString contextKey, QString sourceText) : key(std::move(contextKey)), source(std::move(sourceText)) {}
    };

    // Ordinary read-only single-selection value table. Professional/custom models retain their own view.
    class ImmutableValueTable final : public TableActionTableView
    {
    public:
        using SourceFactory = std::function<ImmutableTableSource(const ks::i18n::SourceTextSnapshot&)>;
        ImmutableValueTable(QWidget* parent, QVector<ImmutableTableHeader> headers);
        ~ImmutableValueTable() override;
        void setSourceFactory(SourceFactory factory);
        void setQuery(const QString& query);
        void clearRows();
        int rowCount() const;
        int columnCount() const;
        int currentRow() const;
        QVariant cell(int row, int column, int role = Qt::DisplayRole) const;
        QVariant currentCell(int column, int role = Qt::DisplayRole) const;
        QString rowKey(int row) const;
        void selectRow(int row, int column = 0);
        void selectKey(const QString& key, int column = 0);
        ImmutableTableSource canonicalSource() const;
        bool isPreparing() const;
        std::function<void()> currentRowChanged, sourceReady;
        std::function<void(int)> rowDoubleClicked;
        std::function<void(const QString&)> preparationFailed;
    protected:
        void showEvent(QShowEvent* event) override;
        void changeEvent(QEvent* event) override;
    private:
        struct State;
        std::unique_ptr<State> m_state;
        void request(bool formatting);
        void startPending();
        void publish(ImmutableTableViewValues projection, quint64 revision);
    };
}
