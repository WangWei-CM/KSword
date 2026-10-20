#pragma once

#include <QCoreApplication>
#include <QObject>
#include <QPointer>
#include <QThread>
#include <QStringList>
#include <cstdint>
#include <functional>
#include <utility>

namespace ks::ui
{
    // 仅在全部退出资源就绪后执行启动；重试保留意图，取消清除意图。
    class DeferredApplicationRestart final
    {
    public:
        using Launch = std::function<bool()>;
        bool accept(Launch launch)
        {
            if (!launch || m_launch) return false;
            m_launch = std::move(launch);
            return true;
        }
        bool hasIntent() const { return static_cast<bool>(m_launch); }
        void cancel() { m_launch = {}; }
        bool launchWhenReady()
        {
            const auto launch = std::move(m_launch);
            return launch && launch(); // callback可销毁UI拥有者，之后不再访问成员。
        }
    private:
        Launch m_launch;
    };

    inline QStringList ArgumentsWithoutRestartPredecessorWait(QStringList arguments)
    {
        const auto key = QStringLiteral("--ksword-crash-restart-wait-pid");
        for (int index = 1; index < arguments.size();)
        {
            if (arguments.at(index).compare(key, Qt::CaseInsensitive) != 0)
            {
                ++index;
                continue;
            }
            arguments.removeAt(index);
            if (index < arguments.size())
            {
                bool numeric = false;
                (void)arguments.at(index).toULongLong(&numeric, 0);
                if (numeric) arguments.removeAt(index);
            }
        }
        return arguments;
    }

    inline QStringList ArgumentsWithRestartPredecessorWait(QStringList arguments, qint64 processId)
    {
        arguments = ArgumentsWithoutRestartPredecessorWait(std::move(arguments));
        const auto key = QStringLiteral("--ksword-crash-restart-wait-pid");
        arguments.push_back(key);
        arguments.push_back(QString::number(processId));
        return arguments;
    }

    // Windows argv转义：保留空参数、引号及收尾反斜线，避免重启改变其它参数。
    inline QString QuoteCoordinatedRestartArgument(const QString& argument)
    {
        QString result(QLatin1Char('"'));
        int slashes = 0;
        for (const QChar character : argument)
        {
            if (character == QLatin1Char('\\')) { ++slashes; continue; }
            result += QString(slashes * (character == QLatin1Char('"') ? 2 : 1), QLatin1Char('\\'));
            slashes = 0;
            if (character == QLatin1Char('"')) result += QLatin1Char('\\');
            result += character;
        }
        result += QString(slashes * 2, QLatin1Char('\\'));
        result += QLatin1Char('"');
        return result;
    }

    namespace coordinated_restart_detail
    {
        struct State
        {
            QPointer<QObject> owner;
            std::function<bool(DeferredApplicationRestart::Launch)> restart;
            std::uint64_t generation = 0;
        };
        inline State& state() { static State value; return value; }
    }

    // 仅UI线程登记/请求；不扫描窗口，不以静态裸this保活窗口。
    inline void InstallCoordinatedApplicationRestart(QObject* owner,
        std::function<bool(DeferredApplicationRestart::Launch)> restart)
    {
        Q_ASSERT(owner && QThread::currentThread() == owner->thread());
        auto& state = coordinated_restart_detail::state();
        state.owner = owner;
        state.restart = std::move(restart);
        const auto generation = ++state.generation;
        QObject::connect(owner, &QObject::destroyed, [generation]()
        {
            auto& current = coordinated_restart_detail::state();
            if (current.generation == generation)
            {
                current.owner.clear();
                current.restart = {};
            }
        });
    }

    inline void RemoveCoordinatedApplicationRestart(QObject* owner)
    {
        auto& state = coordinated_restart_detail::state();
        if (state.owner == owner)
        {
            state.owner.clear();
            state.restart = {};
            ++state.generation;
        }
    }

    inline bool RequestCoordinatedApplicationRestart(DeferredApplicationRestart::Launch launch)
    {
        Q_ASSERT(!QCoreApplication::instance() ||
            QThread::currentThread() == QCoreApplication::instance()->thread());
        const auto& state = coordinated_restart_detail::state();
        if (!state.owner || !state.restart) return false;
        const auto restart = state.restart;
        return restart(std::move(launch));
    }
}
