#include "CodeEditorWidget.h"

// ============================================================
// CodeEditorWidget.cpp
// 作用：
// - 实现“即时窗口”可复用代码编辑器；
// - 提供行号、括号高亮、查找替换、跳转行、文本文件读写。
// ============================================================

#include "../theme.h"

#include <QBuffer>
#include <QFile>
#include <QFileDialog>
#include <QFontDatabase>
#include <QFrame>
#include <QHBoxLayout>
#include <QJsonDocument>
#include <QJsonParseError>
#include <QLabel>
#include <QLineEdit>
#include <QPaintEvent>
#include <QPainter>
#include <QPlainTextEdit>
#include <QPixmap>
#include <QResizeEvent>
#include <QShortcut>
#include <QSize>
#include <QSvgRenderer>
#include <QSyntaxHighlighter>
#include <QTextBlock>
#include <QTextDocument>
#include <QTextStream>
#include <QToolButton>
#include <QVBoxLayout>
#include <QXmlStreamReader>
#include <QXmlStreamWriter>
#include <QStringConverter>

#include <algorithm>

namespace
{
    // buildToolButtonStyle：
    // - 统一工具按钮样式，去掉边框让图标本体更突出；
    // - hover/pressed 仅保留轻量底色反馈，避免 SVG 被边框吃掉。
    QString buildToolButtonStyle()
    {
        return QStringLiteral(
            "QToolButton{"
            "  border:none;"
            "  border-radius:4px;"
            "  padding:1px;"
            "  background:transparent;"
            "  color:%1;"
            "}"
            "QToolButton:hover{"
            "  background:%2;"
            "  color:#FFFFFF;"
            "}"
            "QToolButton:pressed{"
            "  background:%3;"
            "  color:#FFFFFF;"
            "}")
            .arg(KswordTheme::TextPrimaryHex())
            .arg(KswordTheme::PrimaryBlueHex)
            .arg(KswordTheme::PrimaryBluePressedHex);
    }

    // buildInputStyle：
    // - 统一输入框样式，适配深浅色。
    QString buildInputStyle()
    {
        return QStringLiteral(
            "QLineEdit{border:1px solid %1;border-radius:3px;padding:2px 6px;background:%2;color:%3;}"
            "QLineEdit:focus{border:1px solid %4;}")
            .arg(KswordTheme::BorderHex())
            .arg(KswordTheme::SurfaceHex())
            .arg(KswordTheme::TextPrimaryHex())
            .arg(KswordTheme::PrimaryBlueHex);
    }

    // buildToolbarSvgIcon：
    // - 从 SVG 资源生成工具栏图标；
    // - 统一用主题蓝着色，避免深色模式下图标发黑看不清。
    QIcon buildToolbarSvgIcon(const QString& resourcePath, const QSize& iconSize = QSize(22, 22))
    {
        QSvgRenderer renderer(resourcePath);
        if (!renderer.isValid())
        {
            return QIcon(resourcePath);
        }

        QPixmap iconPixmap(iconSize);
        iconPixmap.fill(Qt::transparent);

        QPainter painter(&iconPixmap);
        painter.setRenderHint(QPainter::Antialiasing, true);
        renderer.render(&painter, QRectF(0, 0, iconSize.width(), iconSize.height()));
        painter.setCompositionMode(QPainter::CompositionMode_SourceIn);
        painter.fillRect(iconPixmap.rect(), KswordTheme::PrimaryBlueColor);
        painter.end();

        return QIcon(iconPixmap);
    }

    // isOpenBracket：
    // - 判断字符是否是左括号。
    bool isOpenBracket(const QChar ch)
    {
        return ch == QChar('(') || ch == QChar('[') || ch == QChar('{');
    }

    // isCloseBracket：
    // - 判断字符是否是右括号。
    bool isCloseBracket(const QChar ch)
    {
        return ch == QChar(')') || ch == QChar(']') || ch == QChar('}');
    }

    // pairBracket：
    // - 返回匹配括号字符。
    QChar pairBracket(const QChar ch)
    {
        if (ch == QChar('('))
        {
            return QChar(')');
        }
        if (ch == QChar('['))
        {
            return QChar(']');
        }
        if (ch == QChar('{'))
        {
            return QChar('}');
        }
        if (ch == QChar(')'))
        {
            return QChar('(');
        }
        if (ch == QChar(']'))
        {
            return QChar('[');
        }
        if (ch == QChar('}'))
        {
            return QChar('{');
        }
        return QChar();
    }

    // FileDecodeResult：
    // - 承载文本文件解码结果和会话元数据。
    struct FileDecodeResult
    {
        QString text;
        QStringConverter::Encoding encoding = QStringConverter::Utf8;
        bool hasBom = false;
        QString lineEndingText = QStringLiteral("\n");
        bool success = false;
    };

    // readAllTextWithEncoding：
    // - 按指定编码读取完整文本。
    QString readAllTextWithEncoding(const QByteArray& rawBytes, const QStringConverter::Encoding encoding)
    {
        QBuffer byteBuffer;
        byteBuffer.setData(rawBytes);
        if (!byteBuffer.open(QIODevice::ReadOnly))
        {
            return QString();
        }

        QTextStream textStream(&byteBuffer);
        textStream.setEncoding(encoding);
        return textStream.readAll();
    }

    // detectDominantLineEnding：
    // - 统计文本主导换行风格。
    QString detectDominantLineEnding(const QString& textValue)
    {
        int crlfCount = 0;
        int lfCount = 0;
        int crCount = 0;

        for (int index = 0; index < textValue.size(); ++index)
        {
            const QChar currentChar = textValue.at(index);
            if (currentChar == QChar('\r'))
            {
                if ((index + 1) < textValue.size() && textValue.at(index + 1) == QChar('\n'))
                {
                    ++crlfCount;
                    ++index;
                }
                else
                {
                    ++crCount;
                }
            }
            else if (currentChar == QChar('\n'))
            {
                ++lfCount;
            }
        }

        if (crlfCount >= lfCount && crlfCount >= crCount)
        {
            return QStringLiteral("\r\n");
        }
        if (lfCount >= crCount)
        {
            return QStringLiteral("\n");
        }
        return QStringLiteral("\r");
    }

    // normalizeLineEndingForSaving：
    // - 写回文件前统一换行风格，避免混合换行持续扩散。
    QString normalizeLineEndingForSaving(const QString& textValue, const QString& lineEndingText)
    {
        QString normalizedText = textValue;
        normalizedText.replace(QStringLiteral("\r\n"), QStringLiteral("\n"));
        normalizedText.replace(QChar('\r'), QChar('\n'));

        if (lineEndingText == QStringLiteral("\r\n"))
        {
            return normalizedText.replace(QStringLiteral("\n"), QStringLiteral("\r\n"));
        }
        if (lineEndingText == QStringLiteral("\r"))
        {
            return normalizedText.replace(QChar('\n'), QChar('\r'));
        }
        return normalizedText;
    }

    // buildEncodingDisplayText：
    // - 转换编码展示文案（含 BOM 标记）。
    QString buildEncodingDisplayText(const QStringConverter::Encoding encoding, const bool hasBom)
    {
        QString encodingName = QStringLiteral("UTF-8");
        switch (encoding)
        {
        case QStringConverter::Utf8:
            encodingName = QStringLiteral("UTF-8");
            break;
        case QStringConverter::Utf16LE:
            encodingName = QStringLiteral("UTF-16 LE");
            break;
        case QStringConverter::Utf16BE:
            encodingName = QStringLiteral("UTF-16 BE");
            break;
        case QStringConverter::System:
            encodingName = QStringLiteral("本地编码");
            break;
        default:
            encodingName = QStringLiteral("UTF-8");
            break;
        }

        if (hasBom)
        {
            encodingName += QStringLiteral(" BOM");
        }
        return encodingName;
    }

    // stripKnownBom：
    // - 移除常见 BOM 头并返回是否命中。
    QByteArray stripKnownBom(const QByteArray& fileBytes, bool* hadBomOut)
    {
        QByteArray payload = fileBytes;
        bool hadBom = false;
        if (payload.startsWith("\xEF\xBB\xBF"))
        {
            payload.remove(0, 3);
            hadBom = true;
        }
        else if (payload.size() >= 2
            && static_cast<unsigned char>(payload.at(0)) == 0xFF
            && static_cast<unsigned char>(payload.at(1)) == 0xFE)
        {
            payload.remove(0, 2);
            hadBom = true;
        }
        else if (payload.size() >= 2
            && static_cast<unsigned char>(payload.at(0)) == 0xFE
            && static_cast<unsigned char>(payload.at(1)) == 0xFF)
        {
            payload.remove(0, 2);
            hadBom = true;
        }

        if (hadBomOut != nullptr)
        {
            *hadBomOut = hadBom;
        }
        return payload;
    }

    // decodeTextFileBytesAuto：
    // - 自动识别 BOM / UTF-8 / 本地编码。
    FileDecodeResult decodeTextFileBytesAuto(const QByteArray& fileBytes)
    {
        FileDecodeResult result;
        result.success = true;

        if (fileBytes.startsWith("\xEF\xBB\xBF"))
        {
            result.encoding = QStringConverter::Utf8;
            result.hasBom = true;
            result.text = QString::fromUtf8(fileBytes.constData() + 3, fileBytes.size() - 3);
        }
        else if (fileBytes.size() >= 2
            && static_cast<unsigned char>(fileBytes.at(0)) == 0xFF
            && static_cast<unsigned char>(fileBytes.at(1)) == 0xFE)
        {
            result.encoding = QStringConverter::Utf16LE;
            result.hasBom = true;
            result.text = readAllTextWithEncoding(fileBytes.mid(2), QStringConverter::Utf16LE);
        }
        else if (fileBytes.size() >= 2
            && static_cast<unsigned char>(fileBytes.at(0)) == 0xFE
            && static_cast<unsigned char>(fileBytes.at(1)) == 0xFF)
        {
            result.encoding = QStringConverter::Utf16BE;
            result.hasBom = true;
            result.text = readAllTextWithEncoding(fileBytes.mid(2), QStringConverter::Utf16BE);
        }
        else
        {
            const QString utf8Text = QString::fromUtf8(fileBytes);
            if (!fileBytes.isEmpty() && utf8Text.contains(QChar::ReplacementCharacter))
            {
                result.encoding = QStringConverter::System;
                result.hasBom = false;
                result.text = QString::fromLocal8Bit(fileBytes);
            }
            else
            {
                result.encoding = QStringConverter::Utf8;
                result.hasBom = false;
                result.text = utf8Text;
            }
        }

        result.lineEndingText = detectDominantLineEnding(result.text);
        return result;
    }

    // decodeTextFileBytesForced：
    // - 以调用方指定编码读取文本。
    FileDecodeResult decodeTextFileBytesForced(const QByteArray& fileBytes, QStringConverter::Encoding forcedEncoding)
    {
        FileDecodeResult result;
        result.success = true;
        result.encoding = forcedEncoding;

        bool hadBom = false;
        const QByteArray payload = stripKnownBom(fileBytes, &hadBom);
        result.hasBom = hadBom;

        switch (forcedEncoding)
        {
        case QStringConverter::Utf8:
            result.text = QString::fromUtf8(payload);
            break;
        case QStringConverter::Utf16LE:
            result.text = readAllTextWithEncoding(payload, QStringConverter::Utf16LE);
            break;
        case QStringConverter::Utf16BE:
            result.text = readAllTextWithEncoding(payload, QStringConverter::Utf16BE);
            break;
        case QStringConverter::System:
            result.text = QString::fromLocal8Bit(payload);
            break;
        default:
            result.encoding = QStringConverter::Utf8;
            result.text = QString::fromUtf8(payload);
            break;
        }

        result.lineEndingText = detectDominantLineEnding(result.text);
        return result;
    }

    // tryFormatJsonText：
    // - 尝试识别并格式化 JSON。
    bool tryFormatJsonText(const QString& inputText, QString* formattedTextOut)
    {
        const QString trimmedText = inputText.trimmed();
        if (trimmedText.size() < 2)
        {
            return false;
        }

        const QChar firstChar = trimmedText.front();
        const QChar lastChar = trimmedText.back();
        const bool looksLikeJson =
            (firstChar == QChar('{') && lastChar == QChar('}'))
            || (firstChar == QChar('[') && lastChar == QChar(']'));
        if (!looksLikeJson)
        {
            return false;
        }

        QJsonParseError parseError;
        const QJsonDocument jsonDocument = QJsonDocument::fromJson(trimmedText.toUtf8(), &parseError);
        if (parseError.error != QJsonParseError::NoError || jsonDocument.isNull())
        {
            return false;
        }

        QString formattedText = QString::fromUtf8(jsonDocument.toJson(QJsonDocument::Indented));
        if (formattedText.endsWith(QChar('\n')))
        {
            formattedText.chop(1);
        }

        if (formattedTextOut != nullptr)
        {
            *formattedTextOut = formattedText;
        }
        return true;
    }

    // tryFormatXmlText：
    // - 尝试识别并格式化 XML。
    bool tryFormatXmlText(const QString& inputText, QString* formattedTextOut)
    {
        const QString trimmedText = inputText.trimmed();
        if (trimmedText.size() < 3 || !trimmedText.startsWith(QChar('<')) || !trimmedText.endsWith(QChar('>')))
        {
            return false;
        }
        if (!trimmedText.contains(QStringLiteral("</"))
            && !trimmedText.contains(QStringLiteral("/>"))
            && !trimmedText.startsWith(QStringLiteral("<?xml")))
        {
            return false;
        }

        QXmlStreamReader xmlReader(trimmedText);
        QString formattedXmlText;
        QXmlStreamWriter xmlWriter(&formattedXmlText);
        xmlWriter.setAutoFormatting(true);
        xmlWriter.setAutoFormattingIndent(2);

        while (!xmlReader.atEnd())
        {
            xmlReader.readNext();
            if (xmlReader.tokenType() == QXmlStreamReader::Invalid)
            {
                break;
            }
            xmlWriter.writeCurrentToken(xmlReader);
        }

        if (xmlReader.hasError())
        {
            return false;
        }

        if (formattedTextOut != nullptr)
        {
            *formattedTextOut = formattedXmlText;
        }
        return true;
    }

    // autoFormatStructuredText：
    // - 默认自动格式化 JSON / XML。
    QString autoFormatStructuredText(const QString& inputText, QString* detectedKindOut)
    {
        if (detectedKindOut != nullptr)
        {
            detectedKindOut->clear();
        }

        // 超大文本跳过结构化格式化，优先保证编辑器交互流畅。
        constexpr int kAutoFormatMaxChars = 2 * 1024 * 1024;
        if (inputText.size() > kAutoFormatMaxChars)
        {
            return inputText;
        }

        QString formattedText;
        if (tryFormatJsonText(inputText, &formattedText))
        {
            if (detectedKindOut != nullptr)
            {
                *detectedKindOut = QStringLiteral("JSON");
            }
            return formattedText;
        }

        if (tryFormatXmlText(inputText, &formattedText))
        {
            if (detectedKindOut != nullptr)
            {
                *detectedKindOut = QStringLiteral("XML");
            }
            return formattedText;
        }

        return inputText;
    }
}

class BracketHighlighter final : public QSyntaxHighlighter
{
public:
    // 构造函数：绑定目标文本文档。
    explicit BracketHighlighter(QTextDocument* document)
        : QSyntaxHighlighter(document)
    {
    }

protected:
    // highlightBlock：按括号类型设置颜色。
    void highlightBlock(const QString& text) override
    {
        QTextCharFormat roundFormat;
        roundFormat.setForeground(QColor(95, 175, 255));

        QTextCharFormat squareFormat;
        squareFormat.setForeground(QColor(120, 200, 90));

        QTextCharFormat braceFormat;
        braceFormat.setForeground(QColor(255, 170, 90));

        for (int index = 0; index < text.size(); ++index)
        {
            const QChar ch = text.at(index);
            if (ch == QChar('(') || ch == QChar(')'))
            {
                setFormat(index, 1, roundFormat);
                continue;
            }
            if (ch == QChar('[') || ch == QChar(']'))
            {
                setFormat(index, 1, squareFormat);
                continue;
            }
            if (ch == QChar('{') || ch == QChar('}'))
            {
                setFormat(index, 1, braceFormat);
            }
        }
    }
};

class CodeTextEdit;

class LineNumberArea final : public QWidget
{
public:
    // 构造函数：保存主编辑器指针。
    explicit LineNumberArea(CodeTextEdit* owner);

    // sizeHint：返回行号区域宽度。
    QSize sizeHint() const override;

protected:
    // paintEvent：转发给主编辑器统一绘制。
    void paintEvent(QPaintEvent* event) override;

private:
    // m_owner：主代码编辑器。
    CodeTextEdit* m_owner = nullptr;
};

class CodeTextEdit final : public QPlainTextEdit
{
public:
    // 构造函数：初始化行号、字体和括号匹配高亮。
    explicit CodeTextEdit(QWidget* parent = nullptr)
        : QPlainTextEdit(parent)
    {
        QFont fixedFont = QFontDatabase::systemFont(QFontDatabase::FixedFont);
        fixedFont.setPointSize(std::max(12, fixedFont.pointSize()));
        setFont(fixedFont);
        setTabStopDistance(QFontMetricsF(fixedFont).horizontalAdvance(QChar(' ')) * 4.0);
        setLineWrapMode(QPlainTextEdit::WidgetWidth);
        setFrameShape(QFrame::NoFrame);

        m_lineNumberArea = new LineNumberArea(this);
        m_bracketHighlighter = new BracketHighlighter(document());

        connect(this, &QPlainTextEdit::blockCountChanged, this, [this](int)
            {
                setViewportMargins(lineNumberAreaWidth(), 0, 0, 0);
            });

        connect(this, &QPlainTextEdit::updateRequest, this, [this](const QRect& rect, int deltaY)
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
                refreshExtraSelections();
            });

        connect(this, &QPlainTextEdit::textChanged, this, [this]()
            {
                refreshExtraSelections();
            });

        setViewportMargins(lineNumberAreaWidth(), 0, 0, 0);
        refreshExtraSelections();
    }

    // 析构函数：释放括号高亮对象。
    ~CodeTextEdit() override
    {
        delete m_bracketHighlighter;
        m_bracketHighlighter = nullptr;
    }

    // lineNumberAreaWidth：按文档行数计算行号宽度。
    int lineNumberAreaWidth() const
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

    // paintLineNumberArea：绘制可视区域行号。
    void paintLineNumberArea(QPaintEvent* event)
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

    // gotoLine：跳转到指定 1 基行号。
    bool gotoLine(int oneBasedLine)
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

protected:
    // resizeEvent：窗口变化时同步行号区域几何。
    void resizeEvent(QResizeEvent* event) override
    {
        QPlainTextEdit::resizeEvent(event);
        const QRect rect = contentsRect();
        m_lineNumberArea->setGeometry(rect.left(), rect.top(), lineNumberAreaWidth(), rect.height());
    }

private:
    // refreshExtraSelections：当前行和括号匹配高亮。
    void refreshExtraSelections()
    {
        QList<QTextEdit::ExtraSelection> extraSelections;

        QTextEdit::ExtraSelection lineSelection;
        lineSelection.cursor = textCursor();
        lineSelection.cursor.clearSelection();
        lineSelection.format.setProperty(QTextFormat::FullWidthSelection, true);
        lineSelection.format.setBackground(KswordTheme::IsDarkModeEnabled() ? QColor(42, 42, 42) : QColor(232, 241, 255));
        extraSelections.push_back(lineSelection);

        const QString allText = toPlainText();
        if (!allText.isEmpty())
        {
            int bracketPos = -1;
            QChar bracketCh;
            const int cursorPos = textCursor().position();

            if (cursorPos > 0 && (isOpenBracket(allText.at(cursorPos - 1)) || isCloseBracket(allText.at(cursorPos - 1))))
            {
                bracketPos = cursorPos - 1;
                bracketCh = allText.at(bracketPos);
            }
            else if (cursorPos < allText.size() && (isOpenBracket(allText.at(cursorPos)) || isCloseBracket(allText.at(cursorPos))))
            {
                bracketPos = cursorPos;
                bracketCh = allText.at(bracketPos);
            }

            if (bracketPos >= 0)
            {
                int pairPos = -1;
                const QChar pairCh = pairBracket(bracketCh);
                if (isOpenBracket(bracketCh))
                {
                    int depth = 0;
                    for (int i = bracketPos; i < allText.size(); ++i)
                    {
                        if (allText.at(i) == bracketCh) ++depth;
                        if (allText.at(i) == pairCh) --depth;
                        if (depth == 0)
                        {
                            pairPos = i;
                            break;
                        }
                    }
                }
                else
                {
                    int depth = 0;
                    for (int i = bracketPos; i >= 0; --i)
                    {
                        if (allText.at(i) == bracketCh) ++depth;
                        if (allText.at(i) == pairCh) --depth;
                        if (depth == 0)
                        {
                            pairPos = i;
                            break;
                        }
                    }
                }

                auto appendBracketSelection = [this, &extraSelections](int pos, const QColor& bg)
                    {
                        QTextEdit::ExtraSelection sel;
                        sel.cursor = textCursor();
                        sel.cursor.setPosition(pos);
                        sel.cursor.movePosition(QTextCursor::NextCharacter, QTextCursor::KeepAnchor);
                        sel.format.setForeground(QColor(255, 255, 255));
                        sel.format.setBackground(bg);
                        extraSelections.push_back(sel);
                    };

                const QColor matchedBg = QColor(62, 142, 240);
                appendBracketSelection(bracketPos, pairPos >= 0 ? matchedBg : QColor(196, 67, 67));
                if (pairPos >= 0)
                {
                    appendBracketSelection(pairPos, matchedBg);
                }
            }
        }

        setExtraSelections(extraSelections);
    }

private:
    // m_lineNumberArea：行号区域。
    QWidget* m_lineNumberArea = nullptr;

    // m_bracketHighlighter：括号着色器。
    BracketHighlighter* m_bracketHighlighter = nullptr;

    friend class LineNumberArea;
};

QSize LineNumberArea::sizeHint() const
{
    if (m_owner == nullptr)
    {
        return QSize(0, 0);
    }
    return QSize(m_owner->lineNumberAreaWidth(), 0);
}

LineNumberArea::LineNumberArea(CodeTextEdit* owner)
    : QWidget(owner)
    , m_owner(owner)
{
}

void LineNumberArea::paintEvent(QPaintEvent* event)
{
    if (m_owner != nullptr)
    {
        m_owner->paintLineNumberArea(event);
    }
}

CodeEditorWidget::CodeEditorWidget(QWidget* parent)
    : QWidget(parent)
{
    initializeUi();
    initializeConnections();
    applyThemeStyle();
    refreshReadOnlyUiState();
    updateStatusText();
}

CodeEditorWidget::~CodeEditorWidget() = default;

QString CodeEditorWidget::text() const
{
    return (m_editor == nullptr) ? QString() : m_editor->toPlainText();
}

void CodeEditorWidget::setText(const QString& plainText)
{
    if (m_editor == nullptr)
    {
        return;
    }

    m_editor->setPlainText(applyStructuredAutoFormatIfNeeded(plainText));
    resetFileSessionMetadata();
    updateStatusText();
}

QString CodeEditorWidget::currentFilePath() const
{
    return m_currentFilePath;
}

void CodeEditorWidget::setCurrentFilePath(const QString& filePath)
{
    m_currentFilePath = filePath;
    updateStatusText();
}

void CodeEditorWidget::setReadOnly(const bool readOnly)
{
    if (m_readOnlyMode == readOnly)
    {
        return;
    }

    m_readOnlyMode = readOnly;
    refreshReadOnlyUiState();
    updateStatusText();
}

bool CodeEditorWidget::isReadOnly() const
{
    return m_readOnlyMode;
}

QString CodeEditorWidget::currentEncodingDisplayText() const
{
    return m_fileSessionAvailable
        ? buildEncodingDisplayText(m_fileEncoding, m_fileHasBom)
        : QStringLiteral("未知");
}

bool CodeEditorWidget::openLocalFile(const QString& filePath)
{
    return loadLocalFile(filePath, false, QStringConverter::Utf8);
}

bool CodeEditorWidget::openLocalFileWithEncoding(const QString& filePath, const QStringConverter::Encoding encoding)
{
    return loadLocalFile(filePath, true, encoding);
}

bool CodeEditorWidget::reopenCurrentFileWithEncoding(const QStringConverter::Encoding encoding)
{
    if (m_currentFilePath.trimmed().isEmpty())
    {
        m_statusLabel->setText(QStringLiteral("重开失败：当前无文件路径。"));
        return false;
    }

    return loadLocalFile(m_currentFilePath, true, encoding);
}

void CodeEditorWidget::initializeUi()
{
    m_rootLayout = new QVBoxLayout(this);
    m_rootLayout->setContentsMargins(0, 0, 0, 0);
    m_rootLayout->setSpacing(6);

    m_toolbarWidget = new QWidget(this);
    m_toolbarLayout = new QHBoxLayout(m_toolbarWidget);
    m_toolbarLayout->setContentsMargins(0, 0, 0, 0);
    m_toolbarLayout->setSpacing(4);

    // buildButton：
    // - 统一构建工具按钮；
    // - 所有按钮图标固定来自 qrc 的 SVG 图标库。
    auto buildButton = [this](const QString& iconPath, const QString& tip) -> QToolButton*
        {
            QToolButton* button = new QToolButton(m_toolbarWidget);
            button->setIcon(buildToolbarSvgIcon(iconPath));
            button->setIconSize(QSize(22, 22));
            button->setToolTip(tip);
            button->setAutoRaise(true);
            button->setFixedSize(24, 24);
            return button;
        };

    // 工具栏图标语义映射：
    // - 与功能一一对应，避免使用 MainLogo 造成误导。
    m_newButton = buildButton(QStringLiteral(":/Icon/codeeditor_new.svg"), QStringLiteral("新建 Ctrl+N"));
    m_openButton = buildButton(QStringLiteral(":/Icon/codeeditor_open.svg"), QStringLiteral("打开 Ctrl+O"));
    m_saveButton = buildButton(QStringLiteral(":/Icon/codeeditor_save.svg"), QStringLiteral("保存 Ctrl+S"));
    m_saveAsButton = buildButton(QStringLiteral(":/Icon/codeeditor_save_as.svg"), QStringLiteral("另存为 Ctrl+Shift+S"));
    m_undoButton = buildButton(QStringLiteral(":/Icon/codeeditor_undo.svg"), QStringLiteral("撤销 Ctrl+Z"));
    m_redoButton = buildButton(QStringLiteral(":/Icon/codeeditor_redo.svg"), QStringLiteral("重做 Ctrl+Y"));
    m_cutButton = buildButton(QStringLiteral(":/Icon/codeeditor_cut.svg"), QStringLiteral("剪切 Ctrl+X"));
    m_copyButton = buildButton(QStringLiteral(":/Icon/codeeditor_copy.svg"), QStringLiteral("复制 Ctrl+C"));
    m_pasteButton = buildButton(QStringLiteral(":/Icon/codeeditor_paste.svg"), QStringLiteral("粘贴 Ctrl+V"));
    m_findButton = buildButton(QStringLiteral(":/Icon/codeeditor_find.svg"), QStringLiteral("查找 Ctrl+F"));
    m_replaceButton = buildButton(QStringLiteral(":/Icon/codeeditor_replace.svg"), QStringLiteral("替换 Ctrl+H"));
    m_gotoButton = buildButton(QStringLiteral(":/Icon/codeeditor_goto.svg"), QStringLiteral("跳转行 Ctrl+G"));
    m_wrapButton = buildButton(QStringLiteral(":/Icon/codeeditor_wrap.svg"), QStringLiteral("切换自动换行"));

    m_toolbarLayout->addWidget(m_newButton);
    m_toolbarLayout->addWidget(m_openButton);
    m_toolbarLayout->addWidget(m_saveButton);
    m_toolbarLayout->addWidget(m_saveAsButton);
    m_toolbarLayout->addSpacing(4);
    m_toolbarLayout->addWidget(m_undoButton);
    m_toolbarLayout->addWidget(m_redoButton);
    m_toolbarLayout->addWidget(m_cutButton);
    m_toolbarLayout->addWidget(m_copyButton);
    m_toolbarLayout->addWidget(m_pasteButton);
    m_toolbarLayout->addSpacing(4);
    m_toolbarLayout->addWidget(m_findButton);
    m_toolbarLayout->addWidget(m_replaceButton);
    m_toolbarLayout->addWidget(m_gotoButton);
    m_toolbarLayout->addWidget(m_wrapButton);
    m_toolbarLayout->addStretch(1);
    m_rootLayout->addWidget(m_toolbarWidget);

    m_findPanel = new QWidget(this);
    m_findLayout = new QHBoxLayout(m_findPanel);
    m_findLayout->setContentsMargins(0, 0, 0, 0);
    m_findLayout->setSpacing(4);

    m_findEdit = new QLineEdit(m_findPanel);
    m_findEdit->setPlaceholderText(QStringLiteral("查找"));
    m_replaceEdit = new QLineEdit(m_findPanel);
    m_replaceEdit->setPlaceholderText(QStringLiteral("替换为"));

    m_findPrevButton = new QToolButton(m_findPanel);
    m_findPrevButton->setText(QStringLiteral("↑"));
    m_findNextButton = new QToolButton(m_findPanel);
    m_findNextButton->setText(QStringLiteral("↓"));
    m_replaceOneButton = new QToolButton(m_findPanel);
    m_replaceOneButton->setText(QStringLiteral("替换"));
    m_replaceAllButton = new QToolButton(m_findPanel);
    m_replaceAllButton->setText(QStringLiteral("全部"));
    m_findCloseButton = new QToolButton(m_findPanel);
    m_findCloseButton->setText(QStringLiteral("关闭"));

    m_findLayout->addWidget(m_findEdit, 1);
    m_findLayout->addWidget(m_replaceEdit, 1);
    m_findLayout->addWidget(m_findPrevButton);
    m_findLayout->addWidget(m_findNextButton);
    m_findLayout->addWidget(m_replaceOneButton);
    m_findLayout->addWidget(m_replaceAllButton);
    m_findLayout->addWidget(m_findCloseButton);
    m_findPanel->setVisible(false);
    m_rootLayout->addWidget(m_findPanel);

    m_gotoPanel = new QWidget(this);
    m_gotoLayout = new QHBoxLayout(m_gotoPanel);
    m_gotoLayout->setContentsMargins(0, 0, 0, 0);
    m_gotoLayout->setSpacing(4);
    m_gotoLineEdit = new QLineEdit(m_gotoPanel);
    m_gotoLineEdit->setPlaceholderText(QStringLiteral("行号(从1开始)"));
    m_gotoApplyButton = new QToolButton(m_gotoPanel);
    m_gotoApplyButton->setText(QStringLiteral("执行"));
    m_gotoCloseButton = new QToolButton(m_gotoPanel);
    m_gotoCloseButton->setText(QStringLiteral("关闭"));
    m_gotoLayout->addWidget(new QLabel(QStringLiteral("跳转行:"), m_gotoPanel));
    m_gotoLayout->addWidget(m_gotoLineEdit, 1);
    m_gotoLayout->addWidget(m_gotoApplyButton);
    m_gotoLayout->addWidget(m_gotoCloseButton);
    m_gotoPanel->setVisible(false);
    m_rootLayout->addWidget(m_gotoPanel);

    m_editor = new CodeTextEdit(this);
    m_editor->setPlaceholderText(QStringLiteral("即时窗口：支持行号、括号匹配、查找替换、跳转行。"));
    m_rootLayout->addWidget(m_editor, 1);

    m_statusLabel = new QLabel(QStringLiteral("就绪。"), this);
    m_rootLayout->addWidget(m_statusLabel);
}

void CodeEditorWidget::initializeConnections()
{
    connect(m_newButton, &QToolButton::clicked, this, [this]()
        {
            if (m_readOnlyMode)
            {
                return;
            }
            m_editor->clear();
            m_currentFilePath.clear();
            resetFileSessionMetadata();
            updateStatusText();
        });

    connect(m_openButton, &QToolButton::clicked, this, [this]()
        {
            openTextFile();
        });

    connect(m_saveButton, &QToolButton::clicked, this, [this]()
        {
            saveTextFile(false);
        });

    connect(m_saveAsButton, &QToolButton::clicked, this, [this]()
        {
            saveTextFile(true);
        });

    connect(m_undoButton, &QToolButton::clicked, m_editor, &QPlainTextEdit::undo);
    connect(m_redoButton, &QToolButton::clicked, m_editor, &QPlainTextEdit::redo);
    connect(m_cutButton, &QToolButton::clicked, m_editor, &QPlainTextEdit::cut);
    connect(m_copyButton, &QToolButton::clicked, m_editor, &QPlainTextEdit::copy);
    connect(m_pasteButton, &QToolButton::clicked, m_editor, &QPlainTextEdit::paste);

    connect(m_findButton, &QToolButton::clicked, this, [this]()
        {
            openFindReplacePanel(false);
        });

    connect(m_replaceButton, &QToolButton::clicked, this, [this]()
        {
            openFindReplacePanel(true);
        });

    connect(m_gotoButton, &QToolButton::clicked, this, [this]()
        {
            openGotoPanel();
        });

    connect(m_wrapButton, &QToolButton::clicked, this, [this]()
        {
            const bool enableWrap = (m_editor->lineWrapMode() == QPlainTextEdit::NoWrap);
            m_editor->setLineWrapMode(enableWrap ? QPlainTextEdit::WidgetWidth : QPlainTextEdit::NoWrap);
        });

    connect(m_findPrevButton, &QToolButton::clicked, this, [this]()
        {
            findByDirection(false);
        });

    connect(m_findNextButton, &QToolButton::clicked, this, [this]()
        {
            findByDirection(true);
        });

    connect(m_replaceOneButton, &QToolButton::clicked, this, [this]()
        {
            replaceCurrentSelection();
        });

    connect(m_replaceAllButton, &QToolButton::clicked, this, [this]()
        {
            const int replacedCount = replaceAllMatches();
            m_statusLabel->setText(QStringLiteral("替换完成：%1 处。").arg(replacedCount));
        });

    connect(m_findCloseButton, &QToolButton::clicked, this, [this]()
        {
            m_findPanel->setVisible(false);
        });

    connect(m_findEdit, &QLineEdit::returnPressed, this, [this]()
        {
            findByDirection(true);
        });

    connect(m_gotoApplyButton, &QToolButton::clicked, this, [this]()
        {
            jumpToInputLine();
        });

    connect(m_gotoCloseButton, &QToolButton::clicked, this, [this]()
        {
            m_gotoPanel->setVisible(false);
        });

    connect(m_gotoLineEdit, &QLineEdit::returnPressed, this, [this]()
        {
            jumpToInputLine();
        });

    connect(m_editor, &QPlainTextEdit::cursorPositionChanged, this, [this]()
        {
            updateStatusText();
        });

    connect(m_editor, &QPlainTextEdit::textChanged, this, [this]()
        {
            updateStatusText();
        });

    new QShortcut(QKeySequence::Find, this, [this]()
        {
            openFindReplacePanel(false);
        });

    new QShortcut(QKeySequence::Replace, this, [this]()
        {
            openFindReplacePanel(true);
        });

    new QShortcut(QKeySequence(QStringLiteral("Ctrl+G")), this, [this]()
        {
            openGotoPanel();
        });

    new QShortcut(QKeySequence::FindNext, this, [this]()
        {
            findByDirection(true);
        });

    new QShortcut(QKeySequence::FindPrevious, this, [this]()
        {
            findByDirection(false);
        });

    new QShortcut(QKeySequence::Save, this, [this]()
        {
            saveTextFile(false);
        });

    new QShortcut(QKeySequence::Open, this, [this]()
        {
            openTextFile();
        });

    new QShortcut(QKeySequence::New, this, [this]()
        {
            m_newButton->click();
        });
}

void CodeEditorWidget::applyThemeStyle()
{
    const QString toolStyle = buildToolButtonStyle();
    const QString inputStyle = buildInputStyle();

    const QList<QToolButton*> buttonList{
        m_newButton,m_openButton,m_saveButton,m_saveAsButton,m_undoButton,m_redoButton,m_cutButton,m_copyButton,m_pasteButton,
        m_findButton,m_replaceButton,m_gotoButton,m_wrapButton,m_findPrevButton,m_findNextButton,m_replaceOneButton,m_replaceAllButton,
        m_findCloseButton,m_gotoApplyButton,m_gotoCloseButton
    };
    for (QToolButton* button : buttonList)
    {
        if (button != nullptr)
        {
            button->setStyleSheet(toolStyle);
        }
    }

    m_findEdit->setStyleSheet(inputStyle);
    m_replaceEdit->setStyleSheet(inputStyle);
    m_gotoLineEdit->setStyleSheet(inputStyle);
}

void CodeEditorWidget::refreshReadOnlyUiState()
{
    if (m_editor != nullptr)
    {
        m_editor->setReadOnly(m_readOnlyMode);
    }

    // 写入类按钮：只读模式下统一禁用。
    if (m_newButton != nullptr) m_newButton->setEnabled(!m_readOnlyMode);
    if (m_openButton != nullptr) m_openButton->setEnabled(!m_readOnlyMode);
    if (m_saveButton != nullptr) m_saveButton->setEnabled(!m_readOnlyMode);
    if (m_saveAsButton != nullptr) m_saveAsButton->setEnabled(!m_readOnlyMode);
    if (m_undoButton != nullptr) m_undoButton->setEnabled(!m_readOnlyMode);
    if (m_redoButton != nullptr) m_redoButton->setEnabled(!m_readOnlyMode);
    if (m_cutButton != nullptr) m_cutButton->setEnabled(!m_readOnlyMode);
    if (m_pasteButton != nullptr) m_pasteButton->setEnabled(!m_readOnlyMode);
    if (m_replaceButton != nullptr) m_replaceButton->setEnabled(!m_readOnlyMode);
    if (m_replaceOneButton != nullptr) m_replaceOneButton->setEnabled(!m_readOnlyMode);
    if (m_replaceAllButton != nullptr) m_replaceAllButton->setEnabled(!m_readOnlyMode);

    // 只读模式下隐藏替换输入，保留查找与跳转能力。
    if (m_readOnlyMode)
    {
        m_replaceEdit->setVisible(false);
        m_replaceOneButton->setVisible(false);
        m_replaceAllButton->setVisible(false);
    }
}

void CodeEditorWidget::openFindReplacePanel(const bool replaceEnabled)
{
    const bool effectiveReplaceEnabled = replaceEnabled && !m_readOnlyMode;
    m_replaceEnabled = effectiveReplaceEnabled;
    m_findPanel->setVisible(true);
    m_gotoPanel->setVisible(false);
    m_replaceEdit->setVisible(effectiveReplaceEnabled);
    m_replaceOneButton->setVisible(effectiveReplaceEnabled);
    m_replaceAllButton->setVisible(effectiveReplaceEnabled);
    m_findEdit->setFocus(Qt::ShortcutFocusReason);
    m_findEdit->selectAll();
}

void CodeEditorWidget::openGotoPanel()
{
    m_findPanel->setVisible(false);
    m_gotoPanel->setVisible(true);
    m_gotoLineEdit->setFocus(Qt::ShortcutFocusReason);
    m_gotoLineEdit->selectAll();
}

void CodeEditorWidget::closeInlinePanels()
{
    m_findPanel->setVisible(false);
    m_gotoPanel->setVisible(false);
}

void CodeEditorWidget::updateStatusText()
{
    const QTextCursor cursor = m_editor->textCursor();
    const QString fileName = m_currentFilePath.trimmed().isEmpty() ? QStringLiteral("<未命名>") : m_currentFilePath;
    m_statusLabel->setText(QStringLiteral("行:%1 列:%2 字符:%3 文件:%4 模式:%5 编码:%6")
        .arg(cursor.blockNumber() + 1)
        .arg(cursor.positionInBlock() + 1)
        .arg(m_editor->toPlainText().size())
        .arg(fileName)
        .arg(m_readOnlyMode ? QStringLiteral("只读") : QStringLiteral("可编辑"))
        .arg(currentEncodingDisplayText()));
}

bool CodeEditorWidget::findByDirection(const bool forward)
{
    const QString keyText = m_findEdit->text();
    if (keyText.isEmpty())
    {
        m_statusLabel->setText(QStringLiteral("查找失败：请输入查找内容。"));
        return false;
    }

    QTextDocument::FindFlags flags;
    if (!forward) flags |= QTextDocument::FindBackward;

    bool found = m_editor->find(keyText, flags);
    if (!found)
    {
        QTextCursor cursor = m_editor->textCursor();
        cursor.movePosition(forward ? QTextCursor::Start : QTextCursor::End);
        m_editor->setTextCursor(cursor);
        found = m_editor->find(keyText, flags);
    }

    m_statusLabel->setText(found
        ? QStringLiteral("查找成功：%1").arg(keyText)
        : QStringLiteral("查找结束：未找到 %1").arg(keyText));
    return found;
}

void CodeEditorWidget::replaceCurrentSelection()
{
    if (m_readOnlyMode)
    {
        return;
    }

    const QString findText = m_findEdit->text();
    if (findText.isEmpty())
    {
        m_statusLabel->setText(QStringLiteral("替换失败：查找文本为空。"));
        return;
    }

    QTextCursor cursor = m_editor->textCursor();
    if (!cursor.hasSelection() || cursor.selectedText() != findText)
    {
        if (!findByDirection(true))
        {
            return;
        }
        cursor = m_editor->textCursor();
    }

    cursor.insertText(m_replaceEdit->text());
    m_editor->setTextCursor(cursor);
    m_statusLabel->setText(QStringLiteral("已替换当前命中。"));
    findByDirection(true);
}

int CodeEditorWidget::replaceAllMatches()
{
    if (m_readOnlyMode)
    {
        return 0;
    }

    const QString findText = m_findEdit->text();
    if (findText.isEmpty())
    {
        return 0;
    }

    const QString replaceText = m_replaceEdit->text();
    QTextCursor backupCursor = m_editor->textCursor();
    QTextCursor headCursor = m_editor->textCursor();
    headCursor.movePosition(QTextCursor::Start);
    m_editor->setTextCursor(headCursor);

    int hitCount = 0;
    while (m_editor->find(findText))
    {
        QTextCursor hitCursor = m_editor->textCursor();
        hitCursor.insertText(replaceText);
        ++hitCount;
    }

    m_editor->setTextCursor(backupCursor);
    return hitCount;
}

void CodeEditorWidget::jumpToInputLine()
{
    bool parseOk = false;
    const int lineNumber = m_gotoLineEdit->text().trimmed().toInt(&parseOk, 10);
    if (!parseOk)
    {
        m_statusLabel->setText(QStringLiteral("跳转失败：行号格式无效。"));
        return;
    }

    if (!m_editor->gotoLine(lineNumber))
    {
        m_statusLabel->setText(QStringLiteral("跳转失败：行号越界。"));
        return;
    }

    m_gotoPanel->setVisible(false);
    m_statusLabel->setText(QStringLiteral("已跳转到第 %1 行。").arg(lineNumber));
}

void CodeEditorWidget::openTextFile()
{
    if (m_readOnlyMode)
    {
        return;
    }

    const QString filePath = QFileDialog::getOpenFileName(
        this,
        QStringLiteral("打开文本"),
        QString(),
        QStringLiteral("Text Files (*.txt *.log *.ini *.json *.xml *.cpp *.h *.py);;All Files (*.*)"));

    if (filePath.trimmed().isEmpty())
    {
        return;
    }

    openLocalFile(filePath);
}

bool CodeEditorWidget::loadLocalFile(
    const QString& filePath,
    const bool forceEncoding,
    const QStringConverter::Encoding forcedEncoding)
{
    const QString normalizedPath = filePath.trimmed();
    if (normalizedPath.isEmpty())
    {
        m_statusLabel->setText(QStringLiteral("打开失败：文件路径为空。"));
        return false;
    }

    QFile inputFile(normalizedPath);
    if (!inputFile.open(QIODevice::ReadOnly))
    {
        m_statusLabel->setText(QStringLiteral("打开失败：无法读取文件。"));
        return false;
    }

    const QByteArray fileBytes = inputFile.readAll();
    inputFile.close();

    const FileDecodeResult decodeResult = forceEncoding
        ? decodeTextFileBytesForced(fileBytes, forcedEncoding)
        : decodeTextFileBytesAuto(fileBytes);

    if (!decodeResult.success)
    {
        m_statusLabel->setText(QStringLiteral("打开失败：解码失败。"));
        return false;
    }

    QString detectedKind;
    const QString displayText = applyStructuredAutoFormatIfNeeded(decodeResult.text, &detectedKind);
    m_editor->setPlainText(displayText);

    m_currentFilePath = normalizedPath;
    m_fileEncoding = decodeResult.encoding;
    m_fileHasBom = decodeResult.hasBom;
    m_fileLineEnding = decodeResult.lineEndingText;
    m_fileSessionAvailable = true;

    const QString autoFormatHint = detectedKind.isEmpty()
        ? QString()
        : QStringLiteral("，已自动格式化%1").arg(detectedKind);
    m_statusLabel->setText(QStringLiteral("打开成功：%1（%2%3）")
        .arg(normalizedPath)
        .arg(currentEncodingDisplayText())
        .arg(autoFormatHint));
    return true;
}

void CodeEditorWidget::saveTextFile(const bool forceSaveAs)
{
    if (m_readOnlyMode)
    {
        return;
    }

    QString targetPath = m_currentFilePath;
    if (forceSaveAs || targetPath.trimmed().isEmpty())
    {
        targetPath = QFileDialog::getSaveFileName(
            this,
            QStringLiteral("保存文本"),
            targetPath.trimmed().isEmpty() ? QStringLiteral("immediate.txt") : targetPath,
            QStringLiteral("Text Files (*.txt *.log *.ini *.json *.xml *.cpp *.h *.py);;All Files (*.*)"));
    }

    if (targetPath.trimmed().isEmpty())
    {
        return;
    }

    QFile outputFile(targetPath);
    if (!outputFile.open(QIODevice::WriteOnly | QIODevice::Truncate))
    {
        m_statusLabel->setText(QStringLiteral("保存失败：无法写入文件。"));
        return;
    }

    const QStringConverter::Encoding targetEncoding = m_fileSessionAvailable
        ? m_fileEncoding
        : QStringConverter::Utf8;
    const bool targetHasBom = m_fileSessionAvailable ? m_fileHasBom : false;
    const QString targetLineEnding = m_fileLineEnding.isEmpty()
        ? detectDominantLineEnding(m_editor->toPlainText())
        : m_fileLineEnding;
    const QString normalizedText = normalizeLineEndingForSaving(m_editor->toPlainText(), targetLineEnding);

    QTextStream outputStream(&outputFile);
    outputStream.setEncoding(targetEncoding);
    outputStream.setGenerateByteOrderMark(targetHasBom);
    outputStream << normalizedText;
    outputStream.flush();
    outputFile.close();

    m_currentFilePath = targetPath;
    m_fileEncoding = targetEncoding;
    m_fileHasBom = targetHasBom;
    m_fileLineEnding = targetLineEnding;
    m_fileSessionAvailable = true;
    updateStatusText();
    m_statusLabel->setText(QStringLiteral("保存成功：%1（%2）").arg(targetPath, currentEncodingDisplayText()));
}

void CodeEditorWidget::resetFileSessionMetadata()
{
    m_fileEncoding = QStringConverter::Utf8;
    m_fileHasBom = false;
    m_fileLineEnding = QStringLiteral("\n");
    m_fileSessionAvailable = false;
}

QString CodeEditorWidget::applyStructuredAutoFormatIfNeeded(const QString& inputText, QString* detectedKindOut) const
{
    return autoFormatStructuredText(inputText, detectedKindOut);
}
