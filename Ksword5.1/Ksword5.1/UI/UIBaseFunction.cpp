#include "UI_All.h"
#include "../theme.h"

#include <QLabel>
#include <QVBoxLayout>
#include <QWidget>

QWidget* createBasicPlaceholder(const QString& tipText/* = "Placeholder panel"*/)
{
    // Allocate the placeholder without a parent. The caller or the layout that
    // receives the widget is responsible for transferring ownership into Qt's
    // normal parent-child object tree.
    QWidget* placeholder = new QWidget();

    // Let the placeholder fill whatever dock/page area requested it, while the
    // border makes unfinished panels visible during development and testing.
    placeholder->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
    placeholder->setStyleSheet(
        QStringLiteral(
            "border: 2px solid %1; "
            "background-color: transparent; "
            "border-radius: 0px;")
            .arg(KswordTheme::InfoColor().name(QColor::HexRgb)));

    // The label carries the caller-provided hint text and stays centered so the
    // placeholder remains useful even when the containing panel is resized.
    QLabel* tipLabel = new QLabel(tipText, placeholder);
    tipLabel->setStyleSheet(
        QStringLiteral("color:%1; font-size:14px;")
            .arg(KswordTheme::InfoColor().name(QColor::HexRgb)));
    tipLabel->setAlignment(Qt::AlignCenter);

    // A zero-margin vertical layout keeps the label centered in the full widget
    // rectangle and returns the finished placeholder to the caller.
    QVBoxLayout* layout = new QVBoxLayout(placeholder);
    layout->addWidget(tipLabel);
    layout->setContentsMargins(0, 0, 0, 0);

    return placeholder;
}
