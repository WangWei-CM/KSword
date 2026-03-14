#include "../Framework.h"

#include <algorithm>  // std::find_if
#include <cmath>      // std::isfinite
#include <utility>    // std::move

#include <QApplication> // QApplication::instance
#include <QCoreApplication> // QCoreApplication::instance
#include <QMessageBox>  // 阻塞式按钮选择对话框
#include <QMetaObject>  // invokeMethod
#include <QObject>      // qobject_cast
#include <QPushButton>  // QMessageBox::addButton 返回按钮类型
#include <QThread>      // 判断当前线程是否 UI 线程

namespace
{
    // findTaskByPidMutable 作用：
    // - 在可写任务容器中按 PID 查找任务迭代器。
    // 参数 tasks：任务容器（可写）。
    // 参数 pid：目标 PID。
    // 返回值：找到则返回对应迭代器，否则返回 end()。
    std::vector<kProgressTask>::iterator findTaskByPidMutable(
        std::vector<kProgressTask>& tasks,
        const int pid)
    {
        return std::find_if(
            tasks.begin(),
            tasks.end(),
            [pid](const kProgressTask& taskItem) { return taskItem.pid == pid; });
    }

    // showOptionsDialogOnUiThread 作用：
    // - 在 UI 线程创建并执行阻塞式选项对话框；
    // - 返回用户点击的是第几个按钮（从 1 开始）。
    // 参数 prompt：提示文本。
    // 参数 options：按钮文本数组。
    // 返回值：
    // - 1..N 表示用户选择；
    // - 0 表示关闭窗口或没有选择。
    int showOptionsDialogOnUiThread(const std::string& prompt, const std::vector<std::string>& options)
    {
        // 无选项直接返回 0，避免弹出空对话框。
        if (options.empty())
        {
            return 0;
        }

        QMessageBox optionDialog;
        optionDialog.setWindowTitle(QStringLiteral("任务操作"));
        optionDialog.setIcon(QMessageBox::Question);
        optionDialog.setText(QString::fromUtf8(prompt.c_str()));

        // 把选项按顺序追加为按钮，并记录映射关系。
        std::vector<QAbstractButton*> buttonHandles;
        buttonHandles.reserve(options.size());
        for (const std::string& optionText : options)
        {
            QAbstractButton* optionButton = optionDialog.addButton(
                QString::fromUtf8(optionText.c_str()),
                QMessageBox::ActionRole);
            buttonHandles.push_back(optionButton);
        }

        // 阻塞执行，直到用户点某个按钮或关闭窗口。
        optionDialog.exec();
        QAbstractButton* clickedButton = optionDialog.clickedButton();
        if (clickedButton == nullptr)
        {
            return 0;
        }

        // 把按钮指针反查回 1-based 序号。
        for (std::size_t index = 0; index < buttonHandles.size(); ++index)
        {
            if (buttonHandles[index] == clickedButton)
            {
                return static_cast<int>(index + 1);
            }
        }
        return 0;
    }
} // namespace

// 全局进度管理器定义（extern 声明位于 Framework.h）。
kProgress kPro;

kProgress::kProgress() = default;

int kProgress::add(const std::string& taskName, const std::string& stepName)
{
    std::lock_guard<std::mutex> lockGuard(m_mutex);

    // 分配 PID，并创建初始任务对象。
    const int newPid = m_nextPid++;
    kProgressTask newTask;
    newTask.pid = newPid;
    newTask.taskName = taskName;
    newTask.stepName = stepName;
    newTask.stepCode = 0;
    newTask.progress = 0.0f;
    newTask.hiddenInList = false;
    newTask.hideProgressBarTemporarily = false;

    // 追加到容器并递增修订号，驱动 UI 刷新。
    m_tasks.push_back(std::move(newTask));
    ++m_revision;
    return newPid;
}

void kProgress::set(const int pid, const std::string& stepName, const int stepCode, const float progressValue)
{
    std::lock_guard<std::mutex> lockGuard(m_mutex);

    // 查找目标任务，找不到则静默返回，避免中断业务流程。
    const auto taskIterator = findTaskByPidMutable(m_tasks, pid);
    if (taskIterator == m_tasks.end())
    {
        return;
    }

    // 更新步骤文本、业务状态码与进度值。
    taskIterator->stepName = stepName;
    taskIterator->stepCode = stepCode;
    taskIterator->progress = normalizeProgress(progressValue);

    // 当进度到 1.0 时隐藏卡片（满足“完成后隐藏”需求）。
    taskIterator->hiddenInList = (taskIterator->progress >= 1.0f);

    // 完成状态下不再需要临时隐藏逻辑，统一复位。
    if (taskIterator->hiddenInList)
    {
        taskIterator->hideProgressBarTemporarily = false;
    }

    // 数据变更后递增修订号，通知 UI 重绘。
    ++m_revision;
}

int kProgress::UI(const int pid, const std::string& prompt, const std::vector<std::string>& options)
{
    // 弹框前先临时隐藏目标任务进度条。
    setProgressBarHiddenForUi(pid, true);

    // 通过 Qt 应用对象拿到 UI 线程上下文。
    QApplication* appInstance = qobject_cast<QApplication*>(QCoreApplication::instance());
    if (appInstance == nullptr)
    {
        // 无 QApplication 时无法弹窗，恢复进度条并返回 0。
        setProgressBarHiddenForUi(pid, false);
        return 0;
    }

    int selectedIndex = 0;

    // 若当前就是 UI 线程，直接弹框；否则阻塞调用到 UI 线程执行。
    if (QThread::currentThread() == appInstance->thread())
    {
        selectedIndex = showOptionsDialogOnUiThread(prompt, options);
    }
    else
    {
        QMetaObject::invokeMethod(
            appInstance,
            [&selectedIndex, &prompt, &options]()
            {
                selectedIndex = showOptionsDialogOnUiThread(prompt, options);
            },
            Qt::BlockingQueuedConnection);
    }

    // 弹框结束后恢复进度条显示状态。
    setProgressBarHiddenForUi(pid, false);
    return selectedIndex;
}

std::vector<kProgressTask> kProgress::Snapshot() const
{
    std::lock_guard<std::mutex> lockGuard(m_mutex);
    return m_tasks;
}

std::size_t kProgress::Revision() const
{
    std::lock_guard<std::mutex> lockGuard(m_mutex);
    return m_revision;
}

float kProgress::normalizeProgress(const float rawProgress)
{
    // 非有限值直接按 0 处理，避免 NaN/Inf 污染 UI。
    if (!std::isfinite(rawProgress))
    {
        return 0.0f;
    }

    // 兼容两种输入格式：
    // 1) 0~1   ：比例格式；
    // 2) 0~100 ：百分比格式（如 70.0f）。
    float normalizedValue = rawProgress;
    if (normalizedValue > 1.0f)
    {
        normalizedValue /= 100.0f;
    }

    // 进度钳制到 [0,1] 区间，确保进度条安全显示。
    if (normalizedValue < 0.0f)
    {
        normalizedValue = 0.0f;
    }
    if (normalizedValue > 1.0f)
    {
        normalizedValue = 1.0f;
    }
    return normalizedValue;
}

void kProgress::setProgressBarHiddenForUi(const int pid, const bool hidden)
{
    std::lock_guard<std::mutex> lockGuard(m_mutex);

    // 查找 PID 对应任务，若不存在则直接返回。
    const auto taskIterator = findTaskByPidMutable(m_tasks, pid);
    if (taskIterator == m_tasks.end())
    {
        return;
    }

    // 卡片已因完成而隐藏时，无需再处理“进度条临时隐藏”状态。
    if (taskIterator->hiddenInList)
    {
        return;
    }

    // 仅在状态真正变化时递增修订号，避免无意义刷新。
    if (taskIterator->hideProgressBarTemporarily != hidden)
    {
        taskIterator->hideProgressBarTemporarily = hidden;
        ++m_revision;
    }
}
