#include "CodeTextEdit.h"

// ============================================================
// CodeTextEdit.cpp
// 作用：
// - 实现 CodeTextEdit 的行号绘制、括号匹配与高亮合并；
// - 把文本控件细节从 CodeEditorWidget.cpp 拆分出来，降低单文件体积；
// - 维持与主题系统一致的视觉样式。
// ============================================================

#include "../theme.h"

#include <QFontDatabase>
#include <QFrame>
#include <QPaintEvent>
#include <QPainter>
#include <QResizeEvent>
#include <QSyntaxHighlighter>
#include <QTextBlock>
#include <QTextDocument>
#include <QTimer>

#include <algorithm>

namespace
{
    // isOpenBracket：
    // - 判断字符是否为左括号。
    bool isOpenBracket(const QChar ch)
    {
        return ch == QChar('(') || ch == QChar('[') || ch == QChar('{');
    }

    // isCloseBracket：
    // - 判断字符是否为右括号。
    bool isCloseBracket(const QChar ch)
    {
        return ch == QChar(')') || ch == QChar(']') || ch == QChar('}');
    }

    // pairBracket：
    // - 返回括号的配对字符。
    QChar pairBracket(const QChar ch)
    {
        if (ch == QChar('(')) return QChar(')');
        if (ch == QChar('[')) return QChar(']');
        if (ch == QChar('{')) return QChar('}');
        if (ch == QChar(')')) return QChar('(');
        if (ch == QChar(']')) return QChar('[');
        if (ch == QChar('}')) return QChar('{');
        return QChar();
    }
}

class BracketHighlighter final : public QSyntaxHighlighter
{
public:
    // 构造函数：绑定目标文档。
    explicit BracketHighlighter(QTextDocument* documentPointer)
        : QSyntaxHighlighter(documentPointer)
    {
    }

protected:
    // highlightBlock：
    // - 按括号类型设置颜色，提升结构可读性。
    void highlightBlock(const QString& textValue) override
    {
        QTextCharFormat roundFormat;
        roundFormat.setForeground(QColor(95, 175, 255));

        QTextCharFormat squareFormat;
        squareFormat.setForeground(QColor(120, 200, 90));

        QTextCharFormat braceFormat;
        braceFormat.setForeground(QColor(255, 170, 90));

        for (int index = 0; index < textValue.size(); ++index)
        {
            const QChar currentChar = textValue.at(index);
            if (currentChar == QChar('(') || currentChar == QChar(')'))
            {
                setFormat(index, 1, roundFormat);
                continue;
            }
            if (currentChar == QChar('[') || currentChar == QChar(']'))
            {
                setFormat(index, 1, squareFormat);
                continue;
            }
            if (currentChar == QChar('{') || currentChar == QChar('}'))
            {
                setFormat(index, 1, braceFormat);
            }
        }
    }
};

class LineNumberArea final : public QWidget
{
public:
    // 构造函数：保存主编辑器指针。
    explicit LineNumberArea(CodeTextEdit* ownerPointer)
        : QWidget(ownerPointer)
        , m_owner(ownerPointer)
    {
    }

    // sizeHint：返回行号区域建议宽度。
    QSize sizeHint() const override
    {
        if (m_owner == nullptr)
        {
            return QSize(0, 0);
        }
        return QSize(m_owner->lineNumberAreaWidth(), 0);
    }

protected:
    // paintEvent：转发给主编辑器统一绘制。
    void paintEvent(QPaintEvent* eventPointer) override
    {
        if (m_owner != nullptr)
        {
            m_owner->paintLineNumberArea(eventPointer);
        }
    }

private:
    // m_owner：主编辑器指针。
    CodeTextEdit* m_owner = nullptr;
};

CodeTextEdit::CodeTextEdit(QWidget* parent)
    : QPlainTextEdit(parent)
{
    QFont fixedFont = QFontDatabase::systemFont(QFontDatabase::FixedFont);
    fixedFont.setPointSize(std::max(12, fixedFont.pointSize()));
    setFont(fixedFont);
    setTabStopDistance(QFontMetricsF(fixedFont).horizontalAdvance(QChar(' ')) * 4.0);
    setLineWrapMode(QPlainTextEdit::NoWrap);
    setFrameShape(QFrame::NoFrame);

    m_lineNumberArea = new LineNumberArea(this);
    m_bracketHighlighter = new BracketHighlighter(document());

    m_extraSelectionTimer = new QTimer(this);
    m_extraSelectionTimer->setSingleShot(true);
    m_extraSelectionTimer->setInterval(18);
    connect(m_extraSelectionTimer, &QTimer::timeout, this, [this]()
        {
            refreshExtraSelections();
        });

    connect(this, &QPlainTextEdit::blockCountChanged, this, [this](int)
        {
            setViewportMargins(lineNumberAreaWidth(), 0, 0, 0);
        });

    connect(this, &QPlainTextEdit::updateRequest, this, [this](const QRect& rect, const int deltaY)
        {
            if (deltaY != 0)
            {
                m_lineNumberArea->scroll(0, deltaY);
            }
            else
            {
                m_lineNumberArea->update(0, rect.y(), m_lineNumberArea->width(), rect.height());
            }
            if (rect.contains(viewport()->rect()))
            {
                setViewportMargins(lineNumberAreaWidth(), 0, 0, 0);
            }
        });

    connect(this, &QPlainTextEdit::cursorPositionChanged, this, [this]()
        {
            scheduleRefreshExtraSelections();
        });

    connect(this, &QPlainTextEdit::textChanged, this, [this]()
        {
            scheduleRefreshExtraSelections();
        });

    setViewportMargins(lineNumberAreaWidth(), 0, 0, 0);
    scheduleRefreshExtraSelections();
}

CodeTextEdit::~CodeTextEdit()
{
    delete m_bracketHighlighter;
    m_bracketHighlighter = nullptr;
}

int CodeTextEdit::lineNumberAreaWidth() const
{
    int digits = 1;
    int maxLines = std::max(1, blockCount());
    while (maxLines >= 10)
    {
        maxLines /= 10;
        ++digits;
    }
    return 8 + fontMetrics().horizontalAdvance(QChar('9')) * digits;
}

void CodeTextEdit::paintLineNumberArea(QPaintEvent* event)
{
    QPainter painter(m_lineNumberArea);
    painter.fillRect(event->rect(), KswordTheme::IsDarkModeEnabled() ? QColor(30, 30, 30) : QColor(242, 246, 252));

    QTextBlock block = firstVisibleBlock();
    int blockNumber = block.blockNumber();
    int top = static_cast<int>(blockBoundingGeometry(block).translated(contentOffset()).top());
    int bottom = top + static_cast<int>(blockBoundingRect(block).height());

    while (block.isValid() && top <= event->rect().bottom())
    {
        if (block.isVisible() && bottom >= event->rect().top())
        {
            painter.setPen(KswordTheme::IsDarkModeEnabled() ? QColor(170, 170, 170) : QColor(98, 106, 120));
            painter.drawText(
                0,
                top,
                m_lineNumberArea->width() - 4,
                fontMetrics().height(),
                Qt::AlignRight,
                QString::number(blockNumber + 1));
        }

        block = block.next();
        top = bottom;
        bottom = top + static_cast<int>(blockBoundingRect(block).height());
        ++blockNumber;
    }
}

bool CodeTextEdit::gotoLine(const int oneBasedLine)
{
    if (oneBasedLine <= 0)
    {
        return false;
    }

    const QTextBlock block = document()->findBlockByLineNumber(oneBasedLine - 1);
    if (!block.isValid())
    {
        return false;
    }

    QTextCursor cursor(document());
    cursor.setPosition(block.position());
    setTextCursor(cursor);
    centerCursor();
    return true;
}

void CodeTextEdit::setExternalExtraSelections(const QList<QTextEdit::ExtraSelection>& selections)
{
    m_externalExtraSelections = selections;
    scheduleRefreshExtraSelections();
}

void CodeTextEdit::resizeEvent(QResizeEvent* event)
{
    QPlainTextEdit::resizeEvent(event);
    const QRect rect = contentsRect();
    m_lineNumberArea->setGeometry(rect.left(), rect.top(), lineNumberAreaWidth(), rect.height());
}

void CodeTextEdit::scheduleRefreshExtraSelections()
{
    if (m_extraSelectionTimer != nullptr)
    {
        m_extraSelectionTimer->start();
    }
}

int CodeTextEdit::findPairInSameBlock(const int bracketPos, const QChar bracketCh) const
{
    const QTextBlock textBlock = document()->findBlock(bracketPos);
    if (!textBlock.isValid())
    {
        return -1;
    }

    const QString blockText = textBlock.text();
    const int localPos = bracketPos - textBlock.position();
    if (localPos < 0 || localPos >= blockText.size())
    {
        return -1;
    }

    const QChar pairCh = pairBracket(bracketCh);
    if (isOpenBracket(bracketCh))
    {
        int depth = 0;
        for (int index = localPos; index < blockText.size(); ++index)
        {
            if (blockText.at(index) == bracketCh) ++depth;
            if (blockText.at(index) == pairCh) --depth;
            if (depth == 0)
            {
                return textBlock.position() + index;
            }
        }
        return -1;
    }

    int depth = 0;
    for (int index = localPos; index >= 0; --index)
    {
        if (blockText.at(index) == bracketCh) ++depth;
        if (blockText.at(index) == pairCh) --depth;
        if (depth == 0)
        {
            return textBlock.position() + index;
        }
    }
    return -1;
}

int CodeTextEdit::findPairWithLimitedScan(const int bracketPos, const QChar bracketCh, const int maxScanCount) const
{
    const QChar pairCh = pairBracket(bracketCh);
    const int totalChars = std::max(0, document()->characterCount() - 1);
    if (totalChars <= 0 || bracketPos < 0 || bracketPos >= totalChars)
    {
        return -1;
    }

    const int step = isOpenBracket(bracketCh) ? 1 : -1;
    int depth = 0;
    int scannedChars = 0;
    for (int index = bracketPos;
        index >= 0 && index < totalChars && scannedChars < maxScanCount;
        index += step, ++scannedChars)
    {
        const QChar currentChar = document()->characterAt(index);
        if (currentChar == bracketCh) ++depth;
        if (currentChar == pairCh) --depth;
        if (depth == 0)
        {
            return index;
        }
    }
    return -1;
}

void CodeTextEdit::refreshExtraSelections()
{
    QList<QTextEdit::ExtraSelection> extraSelections = m_externalExtraSelections;

    QTextEdit::ExtraSelection lineSelection;
    lineSelection.cursor = textCursor();
    lineSelection.cursor.clearSelection();
    lineSelection.format.setProperty(QTextFormat::FullWidthSelection, true);
    lineSelection.format.setBackground(KswordTheme::IsDarkModeEnabled() ? QColor(42, 42, 42) : QColor(232, 241, 255));
    extraSelections.push_back(lineSelection);

    const int totalChars = std::max(0, document()->characterCount() - 1);
    const int cursorPos = textCursor().position();
    int bracketPos = -1;
    QChar bracketCh;
    if (cursorPos > 0)
    {
        const QChar prevChar = document()->characterAt(cursorPos - 1);
        if (isOpenBracket(prevChar) || isCloseBracket(prevChar))
        {
            bracketPos = cursorPos - 1;
            bracketCh = prevChar;
        }
    }
    if (bracketPos < 0 && cursorPos < totalChars)
    {
        const QChar currentChar = document()->characterAt(cursorPos);
        if (isOpenBracket(currentChar) || isCloseBracket(currentChar))
        {
            bracketPos = cursorPos;
            bracketCh = currentChar;
        }
    }

    if (bracketPos >= 0)
    {
        int pairPos = findPairInSameBlock(bracketPos, bracketCh);
        if (pairPos < 0)
        {
            pairPos = findPairWithLimitedScan(bracketPos, bracketCh, 200000);
        }

        auto appendBracketSelection = [this, &extraSelections](const int pos, const QColor& bg)
            {
                QTextEdit::ExtraSelection selection;
                selection.cursor = textCursor();
                selection.cursor.setPosition(pos);
                selection.cursor.movePosition(QTextCursor::NextCharacter, QTextCursor::KeepAnchor);
                selection.format.setForeground(QColor(255, 255, 255));
                selection.format.setBackground(bg);
                extraSelections.push_back(selection);
            };

        const QColor matchedBg = QColor(62, 142, 240);
        appendBracketSelection(bracketPos, pairPos >= 0 ? matchedBg : QColor(196, 67, 67));
        if (pairPos >= 0)
        {
            appendBracketSelection(pairPos, matchedBg);
        }
    }

    setExtraSelections(extraSelections);
}

