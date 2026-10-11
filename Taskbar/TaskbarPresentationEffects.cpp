#include "Taskbar.h"
#include "Override.h"

#include <QColor>
#include <QGraphicsOpacityEffect>
#include <QPainter>
#include <QPropertyAnimation>
#include <QResizeEvent>
#include <QVariantAnimation>

namespace
{
// 将 Logo 着色为指定前景色，同时保留 PNG 原有 alpha，避免把 QLabel 的透明区域填成实色矩形。
QPixmap tintPixmapPreservingAlpha(const QPixmap& source, const QColor& color)
{
    if (source.isNull())
    {
        return QPixmap();
    }

    QPixmap tinted(source.size());
    tinted.fill(Qt::transparent);
    QPainter painter(&tinted);
    painter.setCompositionMode(QPainter::CompositionMode_Source);
    painter.drawPixmap(0, 0, source);
    painter.setCompositionMode(QPainter::CompositionMode_SourceIn);
    painter.fillRect(tinted.rect(), color);
    return tinted;
}
}

void Taskbar::applyTaskbarTheme(bool earthquakeAlert, const QColor& backgroundColor)
{
    // 警报态整条 Taskbar 使用白色前景，确保红色或暗红色背景上的地震信息清晰可读。
    const QColor foreground = earthquakeAlert ? QColor(Qt::white) : QColor(QStringLiteral("#00FFFF"));
    const QString foregroundName = foreground.name();
    centralWidget->setStyleSheet(QStringLiteral("background-color: %1;").arg(backgroundColor.name()));
    contentLabel->setStyleSheet(QStringLiteral("border: none; background: transparent; padding: 5px 0; color: %1; font-size: 12px;")
        .arg(foregroundName));
    timeLabel->setStyleSheet(QStringLiteral("border: none; background: transparent; color: %1; font-size: 12px;")
        .arg(foregroundName));
    notificationSourceLabel->setStyleSheet(QStringLiteral("background: transparent; color: %1; font-size: 10px;")
        .arg(foregroundName));
    notificationTitleLabel->setStyleSheet(QStringLiteral("background: transparent; color: %1; font-size: 12px; font-weight: 600;")
        .arg(foregroundName));
    notificationBodyLabel->setStyleSheet(QStringLiteral("background: transparent; color: %1; font-size: 11px;")
        .arg(foregroundName));
    uploadSpeedLabel->setStyleSheet(QStringLiteral("border: none; background: transparent; color: %1; font-size: 10px;")
        .arg(foregroundName));
    downloadSpeedLabel->setStyleSheet(QStringLiteral("border: none; background: transparent; color: %1; font-size: 10px;")
        .arg(foregroundName));
    for (QLabel* bar : cpuBars)
    {
        if (bar != nullptr)
        {
            bar->setStyleSheet(QStringLiteral("background-color: %1;").arg(foregroundName));
        }
    }
    m_leftSpectrum->setBarColor(foreground);
    m_rightSpectrum->setBarColor(foreground);
    if (logoLabel != nullptr && !logoPixmap.isNull())
    {
        const QPixmap displayPixmap = earthquakeAlert
            ? tintPixmapPreservingAlpha(logoPixmap, foreground)
            : logoPixmap;
        logoLabel->setPixmap(displayPixmap.scaled(QSize(QWIDGETSIZE_MAX, logoLabel->height()),
            Qt::KeepAspectRatio, Qt::SmoothTransformation));
    }
    for (GlowIconButton* button : { lockBtn, toolBtn, settingsBtn, overviewBtn, userBtn })
    {
        if (button != nullptr)
        {
            button->setTintColor(foreground);
        }
    }
    if (exitBtn != nullptr)
    {
        static_cast<GlowIconButton*>(exitBtn)->setTintColor(foreground);
    }
}

void Taskbar::startAlertFlashCycle()
{
    // 亮红到暗红使用 500ms 线性渐变，动画结束后立即跳回亮红并开启下一周期。
    if (!earthquakePresentation || alertFlashAnimation == nullptr)
    {
        return;
    }

    alertFlashAnimation->stop();
    alertFlashAnimation->setStartValue(QColor(QStringLiteral("#D90000")));
    alertFlashAnimation->setEndValue(QColor(QStringLiteral("#480000")));
    alertFlashAnimation->start();
}

void Taskbar::flashNotificationBackground()
{
    // 提亮层先瞬时显示，再在线性 500ms 内淡出，重复消息会从新的亮态重新计时。
    if (notificationFlashWidget == nullptr || notificationFlashOpacity == nullptr ||
        notificationFlashAnimation == nullptr)
    {
        return;
    }

    notificationFlashAnimation->stop();
    notificationFlashWidget->raise();
    notificationFlashOpacity->setOpacity(1.0);
    notificationFlashAnimation->setStartValue(1.0);
    notificationFlashAnimation->setEndValue(0.0);
    notificationFlashAnimation->start();
}

void Taskbar::resizeEvent(QResizeEvent* event)
{
    // 先让 QMainWindow 更新 centralWidget 几何，再同步整条提亮层的覆盖范围。
    QMainWindow::resizeEvent(event);
    if (notificationFlashWidget != nullptr && centralWidget != nullptr)
    {
        notificationFlashWidget->setGeometry(centralWidget->rect());
        notificationFlashWidget->raise();
    }
}
