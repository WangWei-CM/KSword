#pragma once

#include <QMap>
#include <QPoint>
#include <QRect>
#include <QString>
#include <QVector>
#include <memory>
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
        QVector<Reply> take();
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
