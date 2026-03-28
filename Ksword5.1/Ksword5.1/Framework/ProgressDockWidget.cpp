#include "ProgressDockWidget.h"
#include "../theme.h"

#include <QFrame>        // 任务卡片容器
#include <QLabel>        // 任务与步骤文本
#include <QProgressBar>  // 进度条显示
#include <QScrollArea>   // 列表滚动容器
#include <QTimer>        // 周期刷新
#include <QVBoxLayout>   // 纵向布局

ProgressDockWidget::ProgressDockWidget(QWidget* parent)
    : QWidget(parent)
{
    // 构造时完成 UI 与定时器初始化，并先做一次首刷。
    initializeUi();
    initializeRefreshTimer();
    refreshTaskCards(true);
}

void ProgressDockWidget::initializeUi()
{
    // 根布局：填满整个 Dock，可自适应大小变化。
    m_rootLayout = new QVBoxLayout(this);
    m_rootLayout->setContentsMargins(0, 0, 0, 0);
    m_rootLayout->setSpacing(0);

    // 滚动区域：用于容纳可增长的任务卡片列表。
    m_scrollArea = new QScrollArea(this);
    m_scrollArea->setWidgetResizable(true);
    m_scrollArea->setFrameShape(QFrame::NoFrame);

    // 滚动内容控件：真正承载卡片布局的中间层。
    m_scrollContent = new QWidget(m_scrollArea);
    m_cardLayout = new QVBoxLayout(m_scrollContent);
    m_cardLayout->setContentsMargins(8, 8, 8, 8);
    m_cardLayout->setSpacing(8);

    // 空列表提示：无任务时展示，提升可读性。
    m_emptyTipLabel = new QLabel(QStringLiteral("当前没有进行中的任务。"), m_scrollContent);
    m_emptyTipLabel->setAlignment(Qt::AlignCenter);
    m_emptyTipLabel->setStyleSheet(
        QStringLiteral("color:%1; font-size:13px;")
        .arg(KswordTheme::TextSecondaryHex()));
    m_cardLayout->addWidget(m_emptyTipLabel);
    m_cardLayout->addStretch(1);

    // 把内容控件挂到滚动区域，再挂到根布局。
    m_scrollArea->setWidget(m_scrollContent);
    m_rootLayout->addWidget(m_scrollArea);
}

void ProgressDockWidget::initializeRefreshTimer()
{
    // 刷新周期 250ms，与日志面板保持一致。
    m_refreshTimer = new QTimer(this);
    m_refreshTimer->setInterval(250);

    // 定时器触发时尝试增量刷新，revision 未变则跳过。
    connect(
        m_refreshTimer,
        &QTimer::timeout,
        this,
        [this]()
        {
            refreshTaskCards(false);
        });
    m_refreshTimer->start();
}

void ProgressDockWidget::refreshTaskCards(const bool forceRefresh)
{
    const std::size_t currentRevision = kPro.Revision();
    if (!forceRefresh && currentRevision == m_lastRevision)
    {
        return;
    }
    m_lastRevision = currentRevision;

    // 拉取任务快照并先清空旧卡片。
    const std::vector<kProgressTask> taskSnapshot = kPro.Snapshot();
    clearCardLayout();

    int visibleTaskCount = 0;

    // 遍历快照，跳过 hiddenInList 的完成任务。
    for (const kProgressTask& taskItem : taskSnapshot)
    {
        if (taskItem.hiddenInList)
        {
            continue;
        }

        QWidget* cardWidget = createTaskCardWidget(taskItem);
        m_cardLayout->addWidget(cardWidget);
        ++visibleTaskCount;
    }

    // 空任务时显示提示；有任务时隐藏提示。
    m_emptyTipLabel->setVisible(visibleTaskCount == 0);
    if (visibleTaskCount == 0)
    {
        m_cardLayout->addWidget(m_emptyTipLabel);
    }

    // 在最底部加弹簧，保证卡片总是顶对齐。
    m_cardLayout->addStretch(1);
}

void ProgressDockWidget::clearCardLayout()
{
    // 逐项弹出并删除布局项，保证卡片完全释放。
    while (QLayoutItem* layoutItem = m_cardLayout->takeAt(0))
    {
        if (QWidget* childWidget = layoutItem->widget())
        {
            // m_emptyTipLabel 是长期复用对象，不在这里销毁。
            if (childWidget != m_emptyTipLabel)
            {
                childWidget->deleteLater();
            }
        }
        delete layoutItem;
    }
}

QWidget* ProgressDockWidget::createTaskCardWidget(const kProgressTask& taskItem) const
{
    // 卡片容器：轻边框 + 圆角，便于视觉分组。
    QFrame* cardFrame = new QFrame();
    cardFrame->setFrameShape(QFrame::StyledPanel);
    cardFrame->setStyleSheet(
        QStringLiteral(
        "QFrame {"
        "  border:1px solid %1;"
        "  border-radius:4px;"
        "  background:%2;"
        "}")
        .arg(KswordTheme::BorderHex(), KswordTheme::SurfaceHex()));

    // 卡片内部布局：任务标题、步骤、进度条依次排列。
    QVBoxLayout* cardLayout = new QVBoxLayout(cardFrame);
    cardLayout->setContentsMargins(10, 8, 10, 8);
    cardLayout->setSpacing(6);

    // 顶部标题：展示任务名与 PID，便于排查/定位。
    QLabel* titleLabel = new QLabel(
        QStringLiteral("%1  (PID:%2)")
        .arg(QString::fromUtf8(taskItem.taskName.c_str()))
        .arg(taskItem.pid),
        cardFrame);
    titleLabel->setStyleSheet(
        QStringLiteral("font-weight:600; color:%1;")
        .arg(KswordTheme::TextPrimaryHex()));
    cardLayout->addWidget(titleLabel);

    // 第二行：步骤文本，反映当前执行阶段。
    QLabel* stepLabel = new QLabel(
        QStringLiteral("步骤：%1")
        .arg(QString::fromUtf8(taskItem.stepName.c_str())),
        cardFrame);
    stepLabel->setStyleSheet(
        QStringLiteral("color:%1;")
        .arg(KswordTheme::TextSecondaryHex()));
    cardLayout->addWidget(stepLabel);

    // 进度条：显示百分比，支持 UI 对话期间临时隐藏。
    QProgressBar* progressBar = new QProgressBar(cardFrame);
    progressBar->setRange(0, 100);
    progressBar->setValue(static_cast<int>(taskItem.progress * 100.0f + 0.5f));
    progressBar->setFormat(QStringLiteral("%p%"));
    progressBar->setVisible(!taskItem.hideProgressBarTemporarily);
    cardLayout->addWidget(progressBar);

    return cardFrame;
}
