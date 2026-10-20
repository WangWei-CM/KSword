#pragma once
#include <QObject>
#include <QPointer>
#include <QVector>
#include <functional>
#include <utility>

namespace ks::ui
{
    enum class ScopedObjectDiscoveryResult { Item, Complete, Yield };
    // 在明确root下逐条读取QObject子边，避免findChildren预先物化整树。
    // enter只决定当前节点及其子树是否可收录；调用方负责窗口/可见边界和结构失效。
    class ScopedObjectDiscoveryCursor
    {
        struct Frame { QPointer<QObject> object; qsizetype child = 0; bool entered = false; };
        QVector<Frame> m_stack;
        std::function<bool(QObject*)> m_enter;
    public:
        void reset(QObject* root = nullptr, std::function<bool(QObject*)> enter = {})
        {
            m_stack.clear();
            m_enter = std::move(enter);
            if (root) m_stack.append({root, 0, false});
        }
        ScopedObjectDiscoveryResult next(QPointer<QObject>& output, int& remainingOperations)
        {
            output.clear();
            while (!m_stack.isEmpty())
            {
                if (remainingOperations <= 0) return ScopedObjectDiscoveryResult::Yield;
                --remainingOperations;
                Frame& frame = m_stack.last();
                if (!frame.object) { m_stack.removeLast(); continue; }
                if (!frame.entered)
                {
                    frame.entered = true;
                    const QPointer<QObject> current = frame.object;
                    if (m_enter && !m_enter(current.data())) { m_stack.removeLast(); continue; }
                    if (!current) { m_stack.removeLast(); continue; }
                    output = current;
                    return ScopedObjectDiscoveryResult::Item;
                }
                const QObjectList& children = frame.object->children();
                if (frame.child >= children.size()) { m_stack.removeLast(); continue; }
                // 同一UI线程中立即转弱引用，不跨事件循环保留QObjectList中的裸指针。
                const QPointer<QObject> child(children.at(frame.child++));
                if (child && child->parent() == frame.object) m_stack.append({child, 0, false});
            }
            return ScopedObjectDiscoveryResult::Complete;
        }
    };
}
