#pragma once

#include "../UI/RetainedCleanupJob.h"
#include <QDir>
#include <QDebug>
#include <QProcess>
#include <QStringList>
#include <atomic>
#include <memory>
#include <mutex>

namespace ks::registry::external
{
    struct Runtime final
    {
        std::atomic_bool stopped{false};
        std::atomic_bool finished{false};
        std::mutex admission;
        bool reserved = false;
        QString temporaryRoot; // Generated QTemporaryDir path only; no QObject/native process crosses threads.
        void requestStop() noexcept { stopped.store(true); }
    };

    class Session final
    {
    public:
        class Work final
        {
        public:
            ~Work() { finish(); } // Also settles a request rejected before native work was started.
            void finish() noexcept
            {
                if (m_runtime->finished.exchange(true)) return;
                m_job->notifyReadyToAttempt();
            }
            const std::shared_ptr<Runtime>& runtime() const { return m_runtime; }
        private:
            Work(std::shared_ptr<Runtime> runtime, std::shared_ptr<ks::ui::RetainedCleanupJob> job)
                : m_runtime(std::move(runtime)), m_job(std::move(job)) {}
            const std::shared_ptr<Runtime> m_runtime;
            const std::shared_ptr<ks::ui::RetainedCleanupJob> m_job;
            friend class Session;
        };

        static std::shared_ptr<Session> create()
        {
            auto runtime = std::make_shared<Runtime>();
            auto job = ks::ui::RetainedCleanupJob::create([runtime]() {
                // finished publishes all worker writes and means its local QProcess is already destroyed.
                if (!runtime->temporaryRoot.isEmpty())
                {
                    QDir temporary(runtime->temporaryRoot);
                    if (temporary.exists() && !temporary.removeRecursively())
                    {
                        try { qWarning().noquote() << "KSword::Registry external action cleanup failed:" << runtime->temporaryRoot; } catch (...) {}
                        return false; // Retain the generated path and lease for explicit Main Retry.
                    }
                    runtime->temporaryRoot.clear();
                }
                return true;
            }, 0, [runtime]() { runtime->requestStop(); }, [runtime]() { return runtime->finished.load(); });
            if (!job) return {};
            return std::shared_ptr<Session>(new Session(std::move(runtime),std::move(job)));
        }
        ~Session() { stop(); }
        Session(const Session&) = delete;
        Session& operator=(const Session&) = delete;
        std::shared_ptr<Work> reserve()
        {
            std::lock_guard<std::mutex> lock(m_runtime->admission);
            if (m_runtime->reserved || m_runtime->stopped.load()) return {};
            auto work = std::shared_ptr<Work>(new Work(m_runtime,m_job));
            m_runtime->reserved = true;
            return work;
        }
        void stop() noexcept
        {
            {
                std::lock_guard<std::mutex> lock(m_runtime->admission);
                m_runtime->requestStop();
                if (!m_runtime->reserved) m_runtime->finished.store(true);
            }
            m_job->notifyReadyToAttempt(); // Wrapper destruction requests initial cleanup exactly once.
        }
        const std::shared_ptr<ks::ui::RetainedCleanupJob>& job() const { return m_job; }
    private:
        Session(std::shared_ptr<Runtime> runtime,std::shared_ptr<ks::ui::RetainedCleanupJob> job)
            : m_runtime(std::move(runtime)),m_job(std::move(job)) {}
        const std::shared_ptr<Runtime> m_runtime;
        const std::shared_ptr<ks::ui::RetainedCleanupJob> m_job;
    };

    struct ProcessResult
    {
        bool ok = false;
        bool canceled = false;
        int exitCode = -1;
        QString error;
    };

    // Called only by a value worker. The actual child must be NotRunning before its local QProcess dies.
    inline ProcessResult runProcess(Runtime& runtime,const QString& program,const QStringList& arguments)
    {
        ProcessResult result;
        if (runtime.stopped.load()) { result.canceled = true; return result; }
        QProcess process;
        process.setProgram(program);
        process.setArguments(arguments);
        process.start();
        if (!process.waitForStarted(-1))
        {
            result.error = process.errorString();
            return result;
        }
        while (process.state() != QProcess::NotRunning)
        {
            if (runtime.stopped.load())
            {
                result.canceled = true;
                process.kill();
            }
            process.waitForFinished(50); // Poll/wait only here, never the GUI; failure is not a termination receipt.
        }
        result.canceled = result.canceled || runtime.stopped.load();
        result.exitCode = process.exitCode();
        result.ok = !result.canceled && process.exitStatus() == QProcess::NormalExit && result.exitCode == 0;
        result.error = QString::fromLocal8Bit(process.readAllStandardError()).trimmed();
        return result;
    }
}
