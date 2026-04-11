#include "MonitorTextViewer.h"

// ============================================================
// MonitorTextViewer.cpp
// 作用：
// 1) 统一创建只读文本查看窗口；
// 2) 使用纯只读文本框展示详情，降低监控详情查看链路的崩溃风险；
// 3) 非模态显示，方便用户边看事件列表边对照原始详情。
// ============================================================

#include <QDialog>
#include <QDialogButtonBox>
#include <QFontDatabase>
#include <QPlainTextEdit>
#include <QVBoxLayout>

#include <algorithm>

namespace monitor_text_viewer
{
    void showReadOnlyTextWindow(
        QWidget* parentWidget,
        const QString& titleText,
        const QString& contentText,
        const QString& virtualPathText)
    {
        QDialog* dialogPointer = new QDialog(parentWidget);
        dialogPointer->setAttribute(Qt::WA_DeleteOnClose, true);
        dialogPointer->setWindowTitle(titleText.trimmed().isEmpty() ? QStringLiteral("详情查看") : titleText);
        dialogPointer->resize(980, 720);
        dialogPointer->setModal(false);

        QVBoxLayout* layoutPointer = new QVBoxLayout(dialogPointer);
        layoutPointer->setContentsMargins(6, 6, 6, 6);
        layoutPointer->setSpacing(6);

        QPlainTextEdit* editorWidget = new QPlainTextEdit(dialogPointer);
        editorWidget->setReadOnly(true);
        editorWidget->setLineWrapMode(QPlainTextEdit::NoWrap);
        QFont fixedFont = QFontDatabase::systemFont(QFontDatabase::FixedFont);
        fixedFont.setPointSize(std::max(11, fixedFont.pointSize()));
        editorWidget->setFont(fixedFont);
        editorWidget->setPlainText(contentText);
        editorWidget->setToolTip(virtualPathText.trimmed().isEmpty() ? titleText : virtualPathText);

        QDialogButtonBox* buttonBoxPointer = new QDialogButtonBox(QDialogButtonBox::Close, dialogPointer);
        QObject::connect(buttonBoxPointer, &QDialogButtonBox::rejected, dialogPointer, &QDialog::reject);
        QObject::connect(buttonBoxPointer, &QDialogButtonBox::accepted, dialogPointer, &QDialog::accept);

        layoutPointer->addWidget(editorWidget, 1);
        layoutPointer->addWidget(buttonBoxPointer, 0);

        dialogPointer->show();
        dialogPointer->raise();
        dialogPointer->activateWindow();
    }
}
