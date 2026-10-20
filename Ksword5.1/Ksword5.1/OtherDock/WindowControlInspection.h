#pragma once

#include <QMap>
#include <QPoint>
#include <QRect>
#include <QString>
#include <QVector>
#include <memory>
#include <deque>
#include <limits>
#include <algorithm>
#include <Windows.h>

class QWidget;

namespace ks::control_inspection
{
    enum class View { Controls, Raw, Native };
    struct Node
    {
        QString id, parent, name, type;
        QRect bounds; // Physical screen pixels, never Qt logical coordinates.
        QRect hostBounds;
        HWND window = nullptr, host = nullptr;
        bool offscreen = false, enabled = true;
        QMap<QString, QString> properties;
    };
    struct Request
    {
        quint64 generation = 0;
        HWND root = nullptr, excluded = nullptr;
        View view = View::Controls;
        DWORD pid = 0, tid = 0;
    };
    struct Reply
    {
        quint64 generation = 0, serial = 0;
        QVector<Node> nodes;
        Node hit;
        HRESULT error = S_OK;
        bool done = false, hover = false, picked = false, mutation = false;
        bool fromTree = false;
        QString mutationId, mutationProperty;
    };

    // Value-only mailbox. A full tree queue pauses traversal, never truncates
    // canonical discovery. Control replies bypass tree backlog; only replaceable
    // ordinary hover snapshots coalesce, never picks or mutation results.
    struct ReplyMailbox
    {
        static constexpr std::size_t TreeBatchCapacity = 8;
        std::deque<Reply> tree, control;
        bool treeFull() const { return tree.size() >= TreeBatchCapacity; }
        bool empty() const { return tree.empty() && control.empty(); }
        void clear() { tree.clear(); control.clear(); }
        void push(Reply reply)
        {
            if (!reply.hover && !reply.mutation) { tree.push_back(std::move(reply)); return; }
            if (reply.hover && !reply.picked && !reply.mutation)
            {
                const auto previous = std::find_if(control.begin(), control.end(), [](const Reply& value)
                    { return value.hover && !value.picked && !value.mutation; });
                if (previous != control.end()) { *previous = std::move(reply); return; }
            }
            control.push_back(std::move(reply));
        }
        QVector<Reply> take(qsizetype maximum, bool controlOnly = false)
        {
            QVector<Reply> result;
            while (result.size() < maximum && !control.empty())
            { result.push_back(std::move(control.front())); control.pop_front(); }
            while (!controlOnly && result.size() < maximum && !tree.empty())
            { result.push_back(std::move(tree.front())); tree.pop_front(); }
            return result;
        }
    };

    // The worker owns COM objects. Its mailbox contains value snapshots only.
    // Closing a dialog cancels without joining a provider call on the UI thread.
    class Collector
    {
    public:
        Collector();
        ~Collector();
        void scan(const Request& request);
        void hit(const Request& request, QPoint physical, quint64 serial, bool picked);
        void details(const Request& request, const QString& id, quint64 serial);
        void mutate(const Request& request, const QString& id, const QString& property,
            const QString& value, quint64 serial);
        void cancel();
        QVector<Reply> take(qsizetype maximum = std::numeric_limits<qsizetype>::max());
        QVector<Reply> takeControl(qsizetype maximum = 1);
        bool hasReplies();
        bool takeDirty();
    private:
        struct State;
        std::shared_ptr<State> m_state;
    };

    bool belongs(HWND window, HWND root, HWND excluded);
    QVector<HWND> relatedRoots(HWND root, HWND excluded);
    QRect physicalBounds(HWND window);
    QString nativeKey(HWND window);

    // Used by the real hook and by input regression tests. Cancellation cannot
    // leak the release of a left button whose press was already consumed.
    struct PickGate
    {
        bool armed = false, consuming = false;
        QPoint pressed;
        bool mouse(UINT message, QPoint point, bool inScope, bool& completed);
        void cancel() { armed = false; }
    };

    QWidget* CreatePage(HWND target, DWORD pid, DWORD tid, quint64 creationTime,
        QWidget* parent);
}
