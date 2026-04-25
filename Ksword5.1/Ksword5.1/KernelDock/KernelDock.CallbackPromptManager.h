#pragma once

#include "../Framework.h"
#include "KernelDock.CallbackIntercept.h"

#include <QDialog>
#include <QPointer>

#include <atomic>
#include <deque>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

class QLabel;
class QPushButton;
class QTimer;
class QWidget;
class QCloseEvent;

class CallbackPromptManager final : public QObject
{
    Q_OBJECT

public:
    static CallbackPromptManager* ensureGlobalManager(QWidget* hostWindow = nullptr);
    static CallbackPromptManager* globalManager();
    static void shutdownGlobalManager();

    explicit CallbackPromptManager(QWidget* hostWindow = nullptr, QObject* parent = nullptr);
    ~CallbackPromptManager() override;

    void setHostWindow(QWidget* hostWindow);
    void start();
    void stop();

signals:
    void logLineGenerated(const QString& logText);

private:
    class DecisionPopupDialog final : public QDialog
    {
    public:
        explicit DecisionPopupDialog(CallbackPromptManager* owner, QWidget* parent = nullptr);

    protected:
        void closeEvent(QCloseEvent* event) override;

    private:
        CallbackPromptManager* m_owner = nullptr;
    };

    struct WaitWorkerContext
    {
        int workerTag = 0;
        std::atomic_bool running{ false };
        std::unique_ptr<std::thread> thread;
        std::mutex ioMutex;
        void* deviceHandle = nullptr;
    };

private:
    void initializePopupUi();
    void initializePopupConnections();
    void appendManagerLog(const QString& logText);
    void startWorkersIfNeeded();
    void stopWorkersIfNeeded();
    void runWaitWorkerLoop(int workerTag);
    void onEventArrivedOnUiThread(const KSWORD_ARK_CALLBACK_EVENT_PACKET& eventPacket);
    void enqueueEvent(const KSWORD_ARK_CALLBACK_EVENT_PACKET& eventPacket);
    bool shouldAutoAllowByRegex(const KSWORD_ARK_CALLBACK_EVENT_PACKET& eventPacket) const;
    void tryShowNextEvent();
    void updatePopupContent(const KSWORD_ARK_CALLBACK_EVENT_PACKET& eventPacket);
    void updateCountdownLabel();
    void finishCurrentEventWithDecision(quint32 decision, bool fromTimeoutOrClose);
    void onPopupClosedByUser();
    bool sendAnswerToDriver(const KSWORD_ARK_GUID128& eventGuid, quint32 decision);
    void cancelAllPendingDecisionsBestEffort();
    quint64 currentUtc100ns() const;
    quint32 currentSessionId() const;
    void movePopupToBottomRight();

private:
    QWidget* m_hostWindow = nullptr;
    std::atomic_bool m_running{ false };
    std::vector<std::unique_ptr<WaitWorkerContext>> m_workerList;

    mutable std::mutex m_queueMutex;
    std::deque<KSWORD_ARK_CALLBACK_EVENT_PACKET> m_eventQueue;

    bool m_hasCurrentEvent = false;
    KSWORD_ARK_CALLBACK_EVENT_PACKET m_currentEvent{};
    qint64 m_remainingTimeoutMs = 0;

    QPointer<DecisionPopupDialog> m_popupDialog;
    QLabel* m_eventGuidValueLabel = nullptr;
    QLabel* m_callbackTypeValueLabel = nullptr;
    QLabel* m_operationValueLabel = nullptr;
    QLabel* m_targetValueLabel = nullptr;
    QLabel* m_initiatorIconLabel = nullptr;
    QLabel* m_initiatorValueLabel = nullptr;
    QLabel* m_sessionIdValueLabel = nullptr;
    QLabel* m_ruleValueLabel = nullptr;
    QPushButton* m_allowButton = nullptr;
    QPushButton* m_denyButton = nullptr;
    QPushButton* m_detailButton = nullptr;
    QTimer* m_countdownTimer = nullptr;
};
