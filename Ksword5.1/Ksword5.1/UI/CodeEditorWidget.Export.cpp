#include "../Framework.h"
#include "CodeEditorWidget.h"
#include "../Internationalization/LanguageManager.h"

#include <QByteArray>
#include <QFileDialog>
#include <QLabel>
#include <QMessageBox>
#include <QPointer>
#include <QSaveFile>

// setReadOnlyExportEnabled：更新明确的只读导出资格，正文与文件会话保持原身份。
void CodeEditorWidget::setReadOnlyExportEnabled(const bool enabled)
{
    if (m_readOnlyExportEnabled == enabled)
    {
        return;
    }
    m_readOnlyExportEnabled = enabled;
    refreshReadOnlyUiState();
}

// exportReadOnlyText：用户选择目标后导出完整 UTF-8 原文，失败不直接覆盖旧目标。
void CodeEditorWidget::exportReadOnlyText()
{
    if (!m_readOnlyMode || !m_readOnlyExportEnabled || m_destroying)
    {
        return;
    }

    // 文件选择器只提供导出目标；关闭窗口或取消后不访问已销毁的编辑器。
    const QPointer<CodeEditorWidget> self(this); // self：模态选择器返回后的生命周期校验。
    const QString targetPath = QFileDialog::getSaveFileName(
        this,
        ks::i18n::text(QStringLiteral("ui.code_editor.export.title"), QStringLiteral("导出完整文本")),
        QStringLiteral("export.txt"),
        ks::i18n::text(QStringLiteral("ui.code_editor.export.filter"),
            QStringLiteral("文本文件 (*.txt *.log);;所有文件 (*.*)")));
    if (!self || !m_readOnlyMode || !m_readOnlyExportEnabled || targetPath.trimmed().isEmpty())
    {
        return;
    }

    // 普通消息框接入应用级 ThemedMessageBox，不另建背景或按钮皮肤。
    const auto showFailure = [this](const QString& detail) // showFailure：同步显示状态回执和真实写入错误。
    {
        setOperationStatus(QStringLiteral("导出失败：%1"), {detail}, 0,
            QStringLiteral("ui.code_editor.export.failed"));
        const QString message = m_statusLabel->text(); // Same explicit status, with the native error kept verbatim.
        QMessageBox::warning(this,
            ks::i18n::text(QStringLiteral("ui.code_editor.export.title"), QStringLiteral("导出完整文本")),
            message);
    };

    // 禁止降级为直接覆盖：只写临时文件，完整提交前保留既有目标。
    QSaveFile outputFile(targetPath); // outputFile：由 Qt 原子替换所选目标。
    outputFile.setDirectWriteFallback(false);
    if (!outputFile.open(QIODevice::WriteOnly))
    {
        showFailure(outputFile.errorString());
        return;
    }

    // text() 保留未编辑原文的 CRLF 和全部内容，不使用选区或重新格式化文本。
    const QByteArray bytes = text().toUtf8(); // bytes：完整文档的 UTF-8 字节，不添加 BOM。
    if (outputFile.write(bytes) != bytes.size())
    {
        const QString error = outputFile.errorString(); // error：取消临时写入前捕获真实错误。
        outputFile.cancelWriting();
        showFailure(error);
        return;
    }
    if (!outputFile.commit())
    {
        showFailure(outputFile.errorString());
        return;
    }

    // 导出结果只更新回执，不修改当前路径、编码、原文缓存或文档 modified 状态。
    setOperationStatus(QStringLiteral("已导出完整文本：%1（UTF-8）"), {targetPath}, 0,
        QStringLiteral("ui.code_editor.export.success"));
}
