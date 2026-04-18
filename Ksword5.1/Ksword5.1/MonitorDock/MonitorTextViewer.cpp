#include "MonitorTextViewer.h"
#include "../theme.h"
#include "../UI/CodeEditorWidget.h"

// ============================================================
// MonitorTextViewer.cpp
// 作用：
// 1) 统一创建只读文本查看窗口；
// 2) 使用纯只读文本框展示详情，降低监控详情查看链路的崩溃风险；
// 3) 非模态显示，方便用户边看事件列表边对照原始详情。
// ============================================================

#include <QDialog>
#include <QDialogButtonBox>
#include <QVBoxLayout>

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
        dialogPointer->setObjectName(QStringLiteral("MonitorTextViewerDialog"));
        dialogPointer->setWindowTitle(titleText.trimmed().isEmpty() ? QStringLiteral("详情查看") : titleText);
        dialogPointer->resize(980, 720);
        dialogPointer->setModal(false);
        // 详情弹窗强制使用不透明背景，避免浅色模式下出现黑底。
        dialogPointer->setStyleSheet(KswordTheme::OpaqueDialogStyle(dialogPointer->objectName()));

        QVBoxLayout* layoutPointer = new QVBoxLayout(dialogPointer);
        layoutPointer->setContentsMargins(6, 6, 6, 6);
        layoutPointer->setSpacing(6);

        // editorWidget 用途：
        // - 统一复用项目内置 CodeEditorWidget，满足“默认使用内部编辑器”规范；
        // - 只读展示 ETW/WMI 等返回详情文本。
        CodeEditorWidget* editorWidget = new CodeEditorWidget(dialogPointer);
        editorWidget->setReadOnly(true);
        editorWidget->setText(contentText);
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
