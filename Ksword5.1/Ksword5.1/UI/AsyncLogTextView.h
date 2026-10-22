#pragma once

#include <QAbstractScrollArea>
#include <QString>
#include <functional>
#include <memory>

namespace ks::ui
{
    // Read-only, wrapping log. Normalization and complete paragraph layout are
    // value-only worker jobs; the GUI draws individual visible QTextLines.
    // Positions use QTextCursor's UTF-16 offsets, including paragraph separators.
    class AsyncLogTextView final : public QAbstractScrollArea
    {
        Q_OBJECT
    public:
        explicit AsyncLogTextView(QWidget* parent = nullptr);
        ~AsyncLogTextView() override;
        void appendPlainText(const QString& text);
        void setMaximumBlockCount(int count);
        bool isEmpty() const;
        void setPlaceholderText(const QString& text);
        bool isReadOnly() const { return true; }
        bool isPreparing() const;
        QString toPlainText() const;
        QString selectedText() const;
        int cursorPosition() const;
        int anchorPosition() const;
        // A previous append-only revision can be mapped through known prefix
        // evictions. Refuses revisions/ranges whose original text is gone.
        bool selectRange(int anchor, int position, quint64 revision);
        void ensureCursorVisible();
        // Captures complete raw Qt document text (NBSP/soft breaks preserved), even if earlier
        // accepted appends are still being normalized. Never waits on the GUI.
        void withTextSnapshot(std::function<void(QString, quint64)> callback);

    public slots:
        void copy();
        void selectAll();

    signals:
        void textChanged();
        void selectionChanged();

    protected:
        void paintEvent(QPaintEvent* event) override;
        void resizeEvent(QResizeEvent* event) override;
        void showEvent(QShowEvent* event) override;
        void hideEvent(QHideEvent* event) override;
        void changeEvent(QEvent* event) override;
        bool event(QEvent* event) override;
        void scrollContentsBy(int dx, int dy) override;
        void keyPressEvent(QKeyEvent* event) override;
        void mousePressEvent(QMouseEvent* event) override;
        void mouseMoveEvent(QMouseEvent* event) override;
        void mouseReleaseEvent(QMouseEvent* event) override;
        void mouseDoubleClickEvent(QMouseEvent* event) override;
        void wheelEvent(QWheelEvent* event) override;
        void focusInEvent(QFocusEvent* event) override;
        void focusOutEvent(QFocusEvent* event) override;
        void contextMenuEvent(QContextMenuEvent* event) override;

    private:
        friend class AsyncLogTextAccessible;
        struct State;
        std::unique_ptr<State> d;
    };
}
