#include "NotificationCardManager.h"

#include "../Framework.h"
#include "../Internationalization/LanguageManager.h"
#include "../theme.h"

#include <QApplication>
#include <QClipboard>
#include <QCursor>
#include <QDateTime>
#include <QGuiApplication>
#include <QHBoxLayout>
#include <QLabel>
#include <QPointer>
#include <QProgressBar>
#include <QPropertyAnimation>
#include <QScreen>
#include <QTimer>
#include <QToolButton>
#include <QVBoxLayout>
#include <QWindow>

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>

#include <algorithm>
#include <cmath>
#include <utility>

namespace
{
    constexpr int kCardWidth = 400;
    constexpr int kScreenMargin = 12;
    constexpr int kCardSpacing = 8;
    constexpr int kAnimationDurationMs = 150;
    constexpr int kCardBackgroundAlpha = 191; // 75%

    QPointer<ks::ui::NotificationCardManager> g_notificationCardManager;

    QString toUiText(const std::string& utf8Text)
    {
        return QString::fromUtf8(utf8Text.data(), static_cast<int>(utf8Text.size()));
    }

    QColor levelColor(const kLogLevel level)
    {
        switch (level)
        {
        case kLogLevel::Debug: return QColor(QStringLiteral("#5A9BD5"));
        case kLogLevel::Info: return QColor(QStringLiteral("#3CB371"));
        case kLogLevel::Warn: return QColor(QStringLiteral("#E6A23C"));
        case kLogLevel::Error: return QColor(QStringLiteral("#E05252"));
        case kLogLevel::Fatal: return QColor(QStringLiteral("#C0392B"));
        default: return QColor(KswordTheme::PrimaryBlueHex);
        }
    }

    QString logCopyText(const kEvent& eventItem)
    {
        return QStringLiteral("%1\n%2\n%3\n文件：%4\n函数：%5\nGUID：%6")
            .arg(QString::fromStdString(LogLevelToString(eventItem.level)))
            .arg(QString::fromStdString(FormatTimeToString(eventItem.timestamp)).right(8))
            .arg(toUiText(eventItem.content))
            .arg(toUiText(eventItem.fileLocation))
            .arg(toUiText(eventItem.functionName))
            .arg(QString::fromStdString(GuidToString(eventItem.guid)));
    }

    QString progressCopyText(const kProgressTask& taskItem)
    {
        const int percent = std::clamp(static_cast<int>(std::lround(taskItem.progress * 100.0f)), 0, 100);
        return QStringLiteral("%1\n%2\n%3%")
            .arg(toUiText(taskItem.taskName))
            .arg(toUiText(taskItem.stepName))
            .arg(percent);
    }

    QRect resolveScreenBounds(QWidget* const hostWindow)
    {
        QScreen* targetScreen = nullptr;
        if (hostWindow != nullptr)
        {
            targetScreen = QGuiApplication::screenAt(hostWindow->frameGeometry().center());
            if (targetScreen == nullptr && hostWindow->windowHandle() != nullptr)
            {
                targetScreen = hostWindow->windowHandle()->screen();
            }
        }
        if (targetScreen == nullptr)
        {
            targetScreen = QGuiApplication::primaryScreen();
        }
        return targetScreen != nullptr ? targetScreen->availableGeometry() : QRect(0, 0, 1920, 1080);
    }
}

namespace ks::ui
{
    class NotificationCard final : public QWidget
    {
    public:
        enum class Kind
        {
            Log,
            Progress
        };

        explicit NotificationCard(const Kind kind)
            : QWidget(nullptr)
            , m_kind(kind)
        {
            setAttribute(Qt::WA_TranslucentBackground, true);
            setAttribute(Qt::WA_ShowWithoutActivating, true);
            setWindowFlags(
                Qt::Tool |
                Qt::FramelessWindowHint |
                Qt::WindowStaysOnTopHint |
                Qt::WindowDoesNotAcceptFocus);
            setFocusPolicy(Qt::NoFocus);
            setFixedWidth(kCardWidth);

            m_frame = new QWidget(this);
            m_frame->setObjectName(QStringLiteral("ksNotificationCardFrame"));
            QVBoxLayout* rootLayout = new QVBoxLayout(this);
            rootLayout->setContentsMargins(0, 0, 0, 0);
            rootLayout->addWidget(m_frame);

            QVBoxLayout* frameLayout = new QVBoxLayout(m_frame);
            frameLayout->setContentsMargins(14, 10, 10, 12);
            frameLayout->setSpacing(7);

            QHBoxLayout* titleLayout = new QHBoxLayout();
            titleLayout->setSpacing(6);
            m_titleLabel = new QLabel(m_frame);
            m_titleLabel->setObjectName(QStringLiteral("ksNotificationCardTitle"));
            m_titleLabel->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
            titleLayout->addWidget(m_titleLabel, 1);
            m_copyButton = new QToolButton(m_frame);
            m_copyButton->setObjectName(QStringLiteral("ksNotificationCardCopy"));
            m_copyButton->setText(ks::i18n::text(QStringLiteral("notification.copy"), QStringLiteral("复制")));
            m_copyButton->setToolTip(ks::i18n::text(QStringLiteral("notification.copy.tooltip"), QStringLiteral("复制卡片内容到剪贴板")));
            m_copyButton->setAutoRaise(true);
            m_copyButton->setFocusPolicy(Qt::NoFocus);
            titleLayout->addWidget(m_copyButton, 0, Qt::AlignTop);
            frameLayout->addLayout(titleLayout);

            m_bodyLabel = new QLabel(m_frame);
            m_bodyLabel->setObjectName(QStringLiteral("ksNotificationCardBody"));
            m_bodyLabel->setWordWrap(true);
            m_bodyLabel->setTextInteractionFlags(Qt::NoTextInteraction);
            frameLayout->addWidget(m_bodyLabel);

            if (m_kind == Kind::Progress)
            {
                m_progressBar = new QProgressBar(m_frame);
                m_progressBar->setObjectName(QStringLiteral("ksNotificationCardProgress"));
                m_progressBar->setRange(0, 100);
                m_progressBar->setTextVisible(true);
                frameLayout->addWidget(m_progressBar);
            }

            connect(m_copyButton, &QToolButton::clicked, this, [this]() {
                if (QClipboard* clipboard = QApplication::clipboard())
                {
                    clipboard->setText(m_copyText);
                }
            });

            refreshVisuals();
        }

        void setLogEvent(const kEvent& eventItem)
        {
            m_titleLabel->setText(
                QStringLiteral("%1  %2")
                .arg(QString::fromStdString(LogLevelToString(eventItem.level)))
                .arg(QString::fromStdString(FormatTimeToString(eventItem.timestamp)).right(8)));
            m_bodyLabel->setText(toUiText(eventItem.content));
            m_copyText = logCopyText(eventItem);
            m_accentColor = levelColor(eventItem.level);
            refreshVisuals();
            adjustToContent();
        }

        void setProgressTask(const kProgressTask& taskItem)
        {
            m_titleLabel->setText(toUiText(taskItem.taskName));
            m_bodyLabel->setText(toUiText(taskItem.stepName));
            if (m_progressBar != nullptr)
            {
                m_progressBar->setValue(std::clamp(static_cast<int>(std::lround(taskItem.progress * 100.0f)), 0, 100));
            }
            m_copyText = progressCopyText(taskItem);
            m_accentColor = QColor(KswordTheme::PrimaryBlueHex);
            refreshVisuals();
            adjustToContent();
        }

        void refreshVisuals()
        {
            QColor backgroundColor(KswordTheme::SurfaceColorHex());
            backgroundColor.setAlpha(kCardBackgroundAlpha);
            const QColor accent = m_accentColor.isValid()
                ? m_accentColor
                : QColor(KswordTheme::PrimaryBlueHex);
            m_frame->setStyleSheet(QStringLiteral(
                "#ksNotificationCardFrame{"
                "background-color:%1;border:1px solid %2;border-left:4px solid %3;border-radius:8px;"
                "}"
                "#ksNotificationCardTitle{color:%4;font-weight:600;}"
                "#ksNotificationCardBody{color:%4;}"
                "#ksNotificationCardCopy{color:%3;border:1px solid transparent;border-radius:4px;padding:2px 5px;}"
                "#ksNotificationCardCopy:hover{background:%5;border-color:%3;}"
                "#ksNotificationCardProgress{border:1px solid %2;border-radius:4px;text-align:center;color:%4;background:%6;height:16px;}"
                "#ksNotificationCardProgress::chunk{background:%3;border-radius:3px;}")
                .arg(backgroundColor.name(QColor::HexArgb))
                .arg(KswordTheme::BorderColorHex())
                .arg(accent.name())
                .arg(KswordTheme::TextPrimaryColorHex())
                .arg(KswordTheme::RgbaColorName(accent, 36))
                .arg(KswordTheme::SurfaceMutedColorHex()));
        }

        void animateTo(const QPoint& targetPosition, const bool animate)
        {
            if (!isVisible())
            {
                move(targetPosition);
                setWindowOpacity(0.0);
                show();
                QPropertyAnimation* fadeIn = new QPropertyAnimation(this, "windowOpacity", this);
                fadeIn->setDuration(kAnimationDurationMs);
                fadeIn->setStartValue(0.0);
                fadeIn->setEndValue(1.0);
                fadeIn->start(QAbstractAnimation::DeleteWhenStopped);
                return;
            }

            if (!animate || pos() == targetPosition)
            {
                move(targetPosition);
                return;
            }
            QPropertyAnimation* moveAnimation = new QPropertyAnimation(this, "pos", this);
            moveAnimation->setDuration(kAnimationDurationMs);
            moveAnimation->setEasingCurve(QEasingCurve::OutCubic);
            moveAnimation->setStartValue(pos());
            moveAnimation->setEndValue(targetPosition);
            moveAnimation->start(QAbstractAnimation::DeleteWhenStopped);
        }

        void dismiss(const bool animate)
        {
            if (!animate)
            {
                hide();
                deleteLater();
                return;
            }
            QPropertyAnimation* fadeOut = new QPropertyAnimation(this, "windowOpacity", this);
            fadeOut->setDuration(kAnimationDurationMs);
            fadeOut->setStartValue(windowOpacity());
            fadeOut->setEndValue(0.0);
            connect(fadeOut, &QPropertyAnimation::finished, this, [this]() {
                hide();
                deleteLater();
            });
            fadeOut->start(QAbstractAnimation::DeleteWhenStopped);
        }

    protected:
        bool nativeEvent(const QByteArray& eventType, void* message, qintptr* result) override
        {
            Q_UNUSED(eventType);
            MSG* nativeMessage = static_cast<MSG*>(message);
            if (nativeMessage != nullptr && nativeMessage->message == WM_NCHITTEST && result != nullptr)
            {
                // 使用 Qt 的全局光标逻辑坐标，并转换到按钮自身坐标系：
                // - 避免直接使用 Win32 物理坐标导致高 DPI 下命中区域偏移；
                // - 避免把嵌套在 m_frame 中的按钮矩形误当作卡片坐标。
                const QPoint cardPosition = mapFromGlobal(QCursor::pos());
                const bool copyButtonHit = m_copyButton != nullptr
                    && m_copyButton->isVisible()
                    && m_copyButton->rect().contains(m_copyButton->mapFrom(this, cardPosition));
                if (!copyButtonHit)
                {
                    *result = HTTRANSPARENT;
                    return true;
                }
            }
            return QWidget::nativeEvent(eventType, message, result);
        }

    private:
        void adjustToContent()
        {
            const int bodyWidth = kCardWidth - 42;
            m_bodyLabel->setMaximumWidth(bodyWidth);
            m_bodyLabel->adjustSize();
            layout()->activate();
            adjustSize();
            setFixedWidth(kCardWidth);
        }

        Kind m_kind;
        QWidget* m_frame = nullptr;
        QLabel* m_titleLabel = nullptr;
        QLabel* m_bodyLabel = nullptr;
        QProgressBar* m_progressBar = nullptr;
        QToolButton* m_copyButton = nullptr;
        QColor m_accentColor;
        QString m_copyText;
    };

    struct NotificationCardRecord
    {
        NotificationCard::Kind kind = NotificationCard::Kind::Log;
        QPointer<NotificationCard> card;
        int progressPid = 0;
        qint64 expiresAtMs = 0;
    };

    NotificationCardManager::NotificationCardManager(
        QWidget* const mainWindow,
        QWidget* const clientAnchor,
        QObject* const parent)
        : QObject(parent)
        , m_mainWindow(mainWindow)
        , m_clientAnchor(clientAnchor)
    {
        g_notificationCardManager = this;
        m_lastLogRevision = KswordARKEventEntry.Revision();
        m_knownLogCount = KswordARKEventEntry.Snapshot().size();
        m_lastProgressRevision = kPro.Revision();

        m_refreshTimer = new QTimer(this);
        m_refreshTimer->setInterval(100);
        connect(m_refreshTimer, &QTimer::timeout, this, [this]() { refreshFromManagers(); });
        m_refreshTimer->start();
    }

    NotificationCardManager::~NotificationCardManager()
    {
        if (g_notificationCardManager == this)
        {
            g_notificationCardManager = nullptr;
        }
        clearCards();
    }

    void NotificationCardManager::applySettings(const ks::settings::AppearanceSettings& settings)
    {
        const bool wasEnabled = m_settings.notificationCardsEnabled;
        m_settings = settings;
        if (!m_settings.notificationCardsEnabled)
        {
            clearCards();
            return;
        }
        if (!wasEnabled)
        {
            m_lastLogRevision = KswordARKEventEntry.Revision();
            m_knownLogCount = KswordARKEventEntry.Snapshot().size();
            m_lastProgressRevision = 0;
        }
        reflowCards(true);
    }

    void NotificationCardManager::refreshVisuals()
    {
        for (const std::unique_ptr<NotificationCardRecord>& record : m_cards)
        {
            if (record != nullptr && record->card != nullptr)
            {
                record->card->refreshVisuals();
            }
        }
        reflowCards(false);
    }

    void NotificationCardManager::onHostGeometryChanged()
    {
        reflowCards(true);
    }

    void NotificationCardManager::clearCards()
    {
        for (const std::unique_ptr<NotificationCardRecord>& record : m_cards)
        {
            if (record != nullptr && record->card != nullptr)
            {
                record->card->dismiss(false);
            }
        }
        m_cards.clear();
        m_overflowProgressTaskIds.clear();
    }

    bool NotificationCardManager::isProgressTaskOverflowed(const int pid) const
    {
        return std::find(m_overflowProgressTaskIds.cbegin(), m_overflowProgressTaskIds.cend(), pid)
            != m_overflowProgressTaskIds.cend();
    }

    QWidget* NotificationCardManager::hostWindow() const
    {
        return m_mainWindow;
    }

    void NotificationCardManager::refreshFromManagers()
    {
        if (!m_settings.notificationCardsEnabled)
        {
            return;
        }
        refreshLogCards();
        refreshProgressCards();
        removeExpiredLogCards();
        reflowCards(true);
    }

    void NotificationCardManager::refreshLogCards()
    {
        const std::size_t revision = KswordARKEventEntry.Revision();
        if (revision == m_lastLogRevision)
        {
            return;
        }
        const std::vector<kEvent> snapshot = KswordARKEventEntry.Snapshot();
        if (snapshot.size() < m_knownLogCount)
        {
            // 日志被清空后不补发清空前后的历史记录，下一条新日志再正常显示。
            m_knownLogCount = snapshot.size();
            m_lastLogRevision = revision;
            return;
        }
        for (std::size_t index = m_knownLogCount; index < snapshot.size(); ++index)
        {
            if (static_cast<int>(snapshot[index].level) >= m_settings.notificationMinimumLevel)
            {
                appendLogCard(snapshot[index]);
            }
        }
        m_knownLogCount = snapshot.size();
        m_lastLogRevision = revision;
    }

    void NotificationCardManager::refreshProgressCards()
    {
        const std::size_t revision = kPro.Revision();
        if (revision == m_lastProgressRevision)
        {
            return;
        }
        const std::vector<kProgressTask> snapshot = kPro.Snapshot();
        std::vector<int> activeTaskIds;
        activeTaskIds.reserve(snapshot.size());
        for (const kProgressTask& taskItem : snapshot)
        {
            if (taskItem.hiddenInList)
            {
                continue;
            }
            activeTaskIds.push_back(taskItem.pid);
            auto existingIterator = std::find_if(
                m_cards.begin(),
                m_cards.end(),
                [&taskItem](const std::unique_ptr<NotificationCardRecord>& record) {
                    return record != nullptr
                        && record->kind == NotificationCard::Kind::Progress
                        && record->progressPid == taskItem.pid;
                });
            if (existingIterator == m_cards.end())
            {
                appendProgressCard(taskItem);
            }
            else if ((*existingIterator)->card != nullptr)
            {
                (*existingIterator)->card->setProgressTask(taskItem);
            }
        }

        for (std::size_t index = 0; index < m_cards.size();)
        {
            const std::unique_ptr<NotificationCardRecord>& record = m_cards[index];
            if (record != nullptr
                && record->kind == NotificationCard::Kind::Progress
                && std::find(activeTaskIds.cbegin(), activeTaskIds.cend(), record->progressPid) == activeTaskIds.cend())
            {
                removeRecordAt(index, false);
                continue;
            }
            ++index;
        }
        m_lastProgressRevision = revision;
    }

    void NotificationCardManager::removeExpiredLogCards()
    {
        const qint64 nowMs = QDateTime::currentMSecsSinceEpoch();
        for (std::size_t index = 0; index < m_cards.size();)
        {
            const std::unique_ptr<NotificationCardRecord>& record = m_cards[index];
            if (record != nullptr
                && record->kind == NotificationCard::Kind::Log
                && record->expiresAtMs > 0
                && record->expiresAtMs <= nowMs)
            {
                removeRecordAt(index, true);
                continue;
            }
            ++index;
        }
    }

    void NotificationCardManager::appendLogCard(const kEvent& eventItem)
    {
        auto record = std::make_unique<NotificationCardRecord>();
        record->kind = NotificationCard::Kind::Log;
        record->card = new NotificationCard(NotificationCard::Kind::Log);
        record->card->setLogEvent(eventItem);
        if (m_settings.notificationLogDisplaySeconds > 0)
        {
            record->expiresAtMs = QDateTime::currentMSecsSinceEpoch()
                + static_cast<qint64>(m_settings.notificationLogDisplaySeconds) * 1000;
        }
        m_cards.push_back(std::move(record));
    }

    void NotificationCardManager::appendProgressCard(const kProgressTask& taskItem)
    {
        auto record = std::make_unique<NotificationCardRecord>();
        record->kind = NotificationCard::Kind::Progress;
        record->progressPid = taskItem.pid;
        record->card = new NotificationCard(NotificationCard::Kind::Progress);
        record->card->setProgressTask(taskItem);
        m_cards.push_back(std::move(record));
    }

    void NotificationCardManager::removeRecordAt(const std::size_t index, const bool animate)
    {
        if (index >= m_cards.size())
        {
            return;
        }
        if (m_cards[index] != nullptr && m_cards[index]->card != nullptr)
        {
            m_cards[index]->card->dismiss(animate);
        }
        m_cards.erase(m_cards.begin() + static_cast<std::ptrdiff_t>(index));
    }

    void NotificationCardManager::reflowCards(const bool animate)
    {
        if (!m_settings.notificationCardsEnabled || m_cards.empty())
        {
            m_overflowProgressTaskIds.clear();
            return;
        }

        QRect targetBounds;
        const bool useMainWindowClientArea =
            m_settings.notificationDisplayPlacement == ks::settings::NotificationDisplayPlacement::MainWindow
            && m_mainWindow != nullptr
            && !m_mainWindow->isMinimized()
            && m_clientAnchor != nullptr
            && m_clientAnchor->isVisible();
        if (useMainWindowClientArea)
        {
            targetBounds = QRect(m_clientAnchor->mapToGlobal(QPoint(0, 0)), m_clientAnchor->size());
        }
        else
        {
            targetBounds = resolveScreenBounds(m_mainWindow);
        }
        if (targetBounds.width() <= 0 || targetBounds.height() <= 0)
        {
            return;
        }

        // 日志优先让位：当混合堆叠超过可用高度时，持续注销最旧日志。
        const auto occupiedHeight = [this]() {
            int total = 0;
            int count = 0;
            for (const std::unique_ptr<NotificationCardRecord>& record : m_cards)
            {
                if (record != nullptr && record->card != nullptr)
                {
                    total += record->card->sizeHint().height();
                    ++count;
                }
            }
            return total + std::max(0, count - 1) * kCardSpacing;
        };
        const int usableHeight = std::max(1, targetBounds.height() - 2 * kScreenMargin);
        while (occupiedHeight() > usableHeight)
        {
            const auto oldestLogIterator = std::find_if(
                m_cards.cbegin(),
                m_cards.cend(),
                [](const std::unique_ptr<NotificationCardRecord>& record) {
                    return record != nullptr && record->kind == NotificationCard::Kind::Log;
                });
            if (oldestLogIterator == m_cards.cend())
            {
                break;
            }
            const std::size_t index = static_cast<std::size_t>(std::distance(m_cards.cbegin(), oldestLogIterator));
            removeRecordAt(index, true);
        }

        m_overflowProgressTaskIds.clear();
        const int xPosition = targetBounds.right() - kScreenMargin - kCardWidth + 1;
        int cursorY = m_settings.notificationStackDirection == ks::settings::NotificationStackDirection::BottomUp
            ? targetBounds.bottom() - kScreenMargin + 1
            : targetBounds.top() + kScreenMargin;

        // 新卡片靠近堆叠起点，符合右下或右上通知对最新事件的预期。
        for (auto iterator = m_cards.rbegin(); iterator != m_cards.rend(); ++iterator)
        {
            const std::unique_ptr<NotificationCardRecord>& record = *iterator;
            if (record == nullptr || record->card == nullptr)
            {
                continue;
            }
            const int cardHeight = std::max(record->card->sizeHint().height(), record->card->height());
            int yPosition = cursorY;
            if (m_settings.notificationStackDirection == ks::settings::NotificationStackDirection::BottomUp)
            {
                yPosition = cursorY - cardHeight;
                cursorY = yPosition - kCardSpacing;
            }
            else
            {
                cursorY += cardHeight + kCardSpacing;
            }
            const QRect cardRect(xPosition, yPosition, kCardWidth, cardHeight);
            if (record->kind == NotificationCard::Kind::Progress
                && !targetBounds.adjusted(kScreenMargin, kScreenMargin, -kScreenMargin, -kScreenMargin).contains(cardRect))
            {
                m_overflowProgressTaskIds.push_back(record->progressPid);
            }
            record->card->animateTo(cardRect.topLeft(), animate);
        }
    }

    bool isProgressTaskNotificationOverflowed(const int pid)
    {
        return g_notificationCardManager != nullptr
            && g_notificationCardManager->isProgressTaskOverflowed(pid);
    }

    QWidget* notificationCardHostWindow()
    {
        return g_notificationCardManager != nullptr
            ? g_notificationCardManager->hostWindow()
            : nullptr;
    }
}
