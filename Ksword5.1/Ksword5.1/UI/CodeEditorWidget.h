#pragma once

// ============================================================
// CodeEditorWidget.h
// 作用：
// - 提供可复用的“即时窗口代码编辑器”组件；
// - 集成行号、查找替换、跳转行、文件读写、括号高亮与匹配；
// - 后续所有需要“类似记事本/代码编辑器输入框”的场景可直接复用。
// ============================================================

#include <QString>
#include <QWidget>

class QLabel;
class QLineEdit;
class QToolButton;
class QVBoxLayout;
class QHBoxLayout;
class QWidget;

class CodeTextEdit;

// CodeEditorWidget：
// - 统一的文本编辑器外壳；
// - 对外提供基础文本读写与当前文件路径访问。
class CodeEditorWidget final : public QWidget
{
    Q_OBJECT

public:
    // 构造函数：
    // - parent：Qt 父控件，可为空。
    explicit CodeEditorWidget(QWidget* parent = nullptr);

    // 析构函数：
    // - 资源由 Qt 父子机制自动回收。
    ~CodeEditorWidget() override;

    // text：
    // - 返回当前编辑器文本内容。
    QString text() const;

    // setText：
    // - 覆盖设置编辑器文本内容。
    // 入参 plainText：目标文本。
    void setText(const QString& plainText);

    // currentFilePath：
    // - 返回当前文件路径（未保存时为空）。
    QString currentFilePath() const;

    // setCurrentFilePath：
    // - 外部设置当前文件路径标签。
    // 入参 filePath：文件路径。
    void setCurrentFilePath(const QString& filePath);

    // setReadOnly：
    // - 设置编辑器是否只读；
    // - 只读模式下会禁用写入类按钮与替换功能。
    // 入参 readOnly：true=只读，false=可编辑。
    void setReadOnly(bool readOnly);

    // isReadOnly：
    // - 返回当前是否只读。
    bool isReadOnly() const;

private:
    // initializeUi：
    // - 创建工具栏、查找替换面板、跳转面板、编辑区与状态栏。
    void initializeUi();

    // initializeConnections：
    // - 连接按钮、快捷键与编辑器状态回调。
    void initializeConnections();

    // applyThemeStyle：
    // - 统一应用工具按钮/输入框样式。
    void applyThemeStyle();

    // openFindReplacePanel：
    // - 显示查找替换面板；
    // - replaceEnabled=true 时同时激活替换输入框。
    void openFindReplacePanel(bool replaceEnabled);

    // openGotoPanel：
    // - 显示跳转行面板并聚焦输入。
    void openGotoPanel();

    // closeInlinePanels：
    // - 隐藏查找替换与跳转行面板。
    void closeInlinePanels();

    // updateStatusText：
    // - 刷新底部状态栏文本（行号、列号、长度、路径）。
    void updateStatusText();

    // findByDirection：
    // - 执行向前/向后查找（非阻塞）。
    // 入参 forward：true=向后查找，false=向前查找。
    // 返回：true=命中，false=未命中。
    bool findByDirection(bool forward);

    // replaceCurrentSelection：
    // - 若当前命中查找文本，则替换为目标文本。
    void replaceCurrentSelection();

    // replaceAllMatches：
    // - 执行全文替换并返回替换次数。
    int replaceAllMatches();

    // jumpToInputLine：
    // - 按跳转面板输入定位到指定行。
    void jumpToInputLine();

    // openTextFile：
    // - 打开 UTF-8 文本文件到编辑器。
    void openTextFile();

    // saveTextFile：
    // - 保存文本到文件。
    // 入参 forceSaveAs：true=强制另存为，false=优先覆盖当前路径。
    void saveTextFile(bool forceSaveAs);

    // refreshReadOnlyUiState：
    // - 根据只读状态更新按钮可用性和面板可见性。
    void refreshReadOnlyUiState();

private:
    // m_rootLayout：根布局。
    QVBoxLayout* m_rootLayout = nullptr;

    // m_toolbarLayout：顶部快捷操作按钮布局。
    QHBoxLayout* m_toolbarLayout = nullptr;

    // m_toolbarWidget：顶部工具栏容器。
    QWidget* m_toolbarWidget = nullptr;

    // m_newButton：新建文本按钮。
    QToolButton* m_newButton = nullptr;

    // m_openButton：打开文本按钮。
    QToolButton* m_openButton = nullptr;

    // m_saveButton：保存文本按钮。
    QToolButton* m_saveButton = nullptr;

    // m_saveAsButton：另存为按钮。
    QToolButton* m_saveAsButton = nullptr;

    // m_undoButton：撤销按钮。
    QToolButton* m_undoButton = nullptr;

    // m_redoButton：重做按钮。
    QToolButton* m_redoButton = nullptr;

    // m_cutButton：剪切按钮。
    QToolButton* m_cutButton = nullptr;

    // m_copyButton：复制按钮。
    QToolButton* m_copyButton = nullptr;

    // m_pasteButton：粘贴按钮。
    QToolButton* m_pasteButton = nullptr;

    // m_findButton：打开查找面板按钮。
    QToolButton* m_findButton = nullptr;

    // m_replaceButton：打开替换面板按钮。
    QToolButton* m_replaceButton = nullptr;

    // m_gotoButton：打开跳转面板按钮。
    QToolButton* m_gotoButton = nullptr;

    // m_wrapButton：切换自动换行按钮。
    QToolButton* m_wrapButton = nullptr;

    // m_findPanel：查找替换面板。
    QWidget* m_findPanel = nullptr;

    // m_findLayout：查找替换面板布局。
    QHBoxLayout* m_findLayout = nullptr;

    // m_findEdit：查找输入框。
    QLineEdit* m_findEdit = nullptr;

    // m_replaceEdit：替换输入框。
    QLineEdit* m_replaceEdit = nullptr;

    // m_findPrevButton：查找上一个按钮。
    QToolButton* m_findPrevButton = nullptr;

    // m_findNextButton：查找下一个按钮。
    QToolButton* m_findNextButton = nullptr;

    // m_replaceOneButton：替换当前命中按钮。
    QToolButton* m_replaceOneButton = nullptr;

    // m_replaceAllButton：全部替换按钮。
    QToolButton* m_replaceAllButton = nullptr;

    // m_findCloseButton：关闭查找面板按钮。
    QToolButton* m_findCloseButton = nullptr;

    // m_gotoPanel：跳转行面板。
    QWidget* m_gotoPanel = nullptr;

    // m_gotoLayout：跳转行面板布局。
    QHBoxLayout* m_gotoLayout = nullptr;

    // m_gotoLineEdit：跳转行输入框。
    QLineEdit* m_gotoLineEdit = nullptr;

    // m_gotoApplyButton：执行跳转按钮。
    QToolButton* m_gotoApplyButton = nullptr;

    // m_gotoCloseButton：关闭跳转面板按钮。
    QToolButton* m_gotoCloseButton = nullptr;

    // m_editor：核心代码编辑器（行号 + 括号高亮）。
    CodeTextEdit* m_editor = nullptr;

    // m_statusLabel：底部状态信息标签。
    QLabel* m_statusLabel = nullptr;

    // m_currentFilePath：当前文件路径（用于保存覆盖）。
    QString m_currentFilePath;

    // m_replaceEnabled：标记查找面板当前是否显示替换控件。
    bool m_replaceEnabled = false;

    // m_readOnlyMode：标记当前是否只读模式。
    bool m_readOnlyMode = false;
};
