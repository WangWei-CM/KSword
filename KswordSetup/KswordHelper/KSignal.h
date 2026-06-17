#ifndef KSWORD_HELPER_KSIGNAL_HEAD_FILE
#define KSWORD_HELPER_KSIGNAL_HEAD_FILE

#include <cstddef>
#include <functional>
#include <mutex>
#include <vector>

// KSignal is a small signal-slot template independent of Qt. It stores copyable
// std::function slots and returns numeric connection ids for later disconnect().
template <typename... Args>
class KSignal {
public:
    // Slot is the callable signature accepted by connect().
    typedef std::function<void(Args...)> Slot;

    // The constructor starts with no slots and the first connection id equal to 1.
    KSignal()
        : m_nextId(1), m_slots() {
    }

    // connect stores a slot and returns its connection id. Empty std::function
    // values are ignored and return 0 so callers can detect no-op connections.
    std::size_t connect(const Slot& slot) {
        if (!slot) {
            return 0;
        }

        std::lock_guard<std::mutex> lock(m_mutex);
        const std::size_t id = m_nextId++;
        m_slots.push_back(Connection(id, slot));
        return id;
    }

    // disconnect removes the slot with id and returns true when one was found.
    bool disconnect(std::size_t id) {
        std::lock_guard<std::mutex> lock(m_mutex);
        for (typename std::vector<Connection>::iterator it = m_slots.begin(); it != m_slots.end(); ++it) {
            if (it->id == id) {
                m_slots.erase(it);
                return true;
            }
        }
        return false;
    }

    // clear removes every connected slot and returns no value.
    void clear() {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_slots.clear();
    }

    // size returns the current slot count under lock.
    std::size_t size() const {
        std::lock_guard<std::mutex> lock(m_mutex);
        return m_slots.size();
    }

    // isEmpty returns true when no slots are connected.
    bool isEmpty() const {
        return size() == 0;
    }

    // emitSignal invokes a snapshot of slots. The mutex is not held while slots
    // execute, so slots may connect/disconnect without deadlocking this signal.
    void emitSignal(Args... args) const {
        std::vector<Slot> snapshot;
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            snapshot.reserve(m_slots.size());
            for (typename std::vector<Connection>::const_iterator it = m_slots.begin(); it != m_slots.end(); ++it) {
                snapshot.push_back(it->slot);
            }
        }

        for (typename std::vector<Slot>::const_iterator it = snapshot.begin(); it != snapshot.end(); ++it) {
            (*it)(args...);
        }
    }

    // operator() is a shorthand for emitSignal(). It returns no value.
    void operator()(Args... args) const {
        emitSignal(args...);
    }

private:
    // Connection binds a stable id to one slot callable.
    struct Connection {
        Connection(std::size_t connectionId, const Slot& connectionSlot)
            : id(connectionId), slot(connectionSlot) {
        }

        std::size_t id;
        Slot slot;
    };

    // m_mutex protects m_nextId and m_slots for simple multi-threaded use.
    mutable std::mutex m_mutex;

    // m_nextId monotonically assigns nonzero connection ids.
    std::size_t m_nextId;

    // m_slots owns all active slot callables.
    std::vector<Connection> m_slots;
};

#endif
