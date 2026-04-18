#pragma once

// ============================================================
// CodeTextEdit.h
// 作用：
// - 提供 CodeEditorWidget 的核心文本控件；
// - 集成行号区、当前行高亮、括号匹配与外部高亮合并能力；
// - 对外暴露行跳转与外部高亮注入接口。
// ============================================================

#include <QList>
#include <QPlainTextEdit>
#include <QTextEdit>

class QPaintEvent;
class QResizeEvent;
class QSyntaxHighlighter;
class QTimer;
class QWidget;

// CodeTextEdit：
// - 统一代码文本编辑器实现；
// - 供 CodeEditorWidget 组合复用。
class CodeTextEdit final : public QPlainTextEdit
{
public:
    // 构造函数：
    // - parent：父控件，可为空。
    explicit CodeTextEdit(QWidget* parent = nullptr);

    // 析构函数：
    // - 释放内部括号着色器对象。
    ~CodeTextEdit() override;

    // lineNumberAreaWidth：
    // - 按当前行数动态计算行号区域宽度。
    int lineNumberAreaWidth() const;

    // paintLineNumberArea：
    // - 绘制行号区域内容。
    // 入参 event：行号区绘制事件。
    void paintLineNumberArea(QPaintEvent* event);

    // gotoLine：
    // - 跳转到指定 1 基行号并居中显示。
    // 入参 oneBasedLine：目标行号（从 1 开始）。
    // 返回：true=跳转成功；false=越界或无效。
    bool gotoLine(int oneBasedLine);

    // setExternalExtraSelections：
    // - 注入外层命中高亮（例如查找全部命中）；
    // - 会与当前行/括号高亮合并显示。
    // 入参 selections：额外高亮集合。
    void setExternalExtraSelections(const QList<QTextEdit::ExtraSelection>& selections);

protected:
    // resizeEvent：
    // - 编辑区尺寸变化时同步行号区几何。
    void resizeEvent(QResizeEvent* event) override;

private:
    // scheduleRefreshExtraSelections：
    // - 节流触发高亮刷新。
    void scheduleRefreshExtraSelections();

    // findPairInSameBlock：
    // - 优先在当前文本块内查找括号配对位置。
    int findPairInSameBlock(int bracketPos, QChar bracketCh) const;

    // findPairWithLimitedScan：
    // - 在全文做上限扫描查找配对位置。
    int findPairWithLimitedScan(int bracketPos, QChar bracketCh, int maxScanCount) const;

    // refreshExtraSelections：
    // - 刷新当前行高亮、括号高亮与外部命中高亮。
    void refreshExtraSelections();

private:
    // m_lineNumberArea：行号区域控件。
    QWidget* m_lineNumberArea = nullptr;

    // m_bracketHighlighter：括号着色器对象。
    QSyntaxHighlighter* m_bracketHighlighter = nullptr;

    // m_extraSelectionTimer：高亮刷新节流计时器。
    QTimer* m_extraSelectionTimer = nullptr;

    // m_externalExtraSelections：外层注入的额外高亮集合。
    QList<QTextEdit::ExtraSelection> m_externalExtraSelections;

    friend class LineNumberArea;
};

