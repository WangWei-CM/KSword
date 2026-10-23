#pragma once
// Test-only replacement for the QSaveFile include. No actual filesystem access.
#include <QByteArray>
#include <QIODevice>
#include <QString>
#include <algorithm>
#include <atomic>
#include <map>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <thread>

struct FakeFileState
{
    QByteArray data;
    bool directFallback = true, committed = false, canceled = false, text = false;
    bool openOk = true, flushOk = true, commitOk = true;
    int commitCalls = 0;
    std::thread::id workerThread;
};
inline std::mutex fakeFilesMutex;
inline std::map<QString, std::shared_ptr<FakeFileState>> fakeFiles;
inline std::atomic_bool throwFakeFileConstruction{ false };
inline std::shared_ptr<FakeFileState> fakeFile(const QString& path)
{
    std::lock_guard lock(fakeFilesMutex);
    auto& state = fakeFiles[path];
    if (!state) state = std::make_shared<FakeFileState>();
    return state;
}
class QSaveFile
{
public:
    QSaveFile()
    {
        if (throwFakeFileConstruction.exchange(false)) throw std::runtime_error("fake-construction-failure");
    }
    void setFileName(const QString& path) { m_state = fakeFile(path); }
    void setDirectWriteFallback(bool value) { m_state->directFallback = value; }
    bool open(QIODevice::OpenMode flags)
    {
        m_state->workerThread = std::this_thread::get_id();
        m_state->text = flags.testFlag(QIODevice::Text);
        return m_state->openOk;
    }
    qint64 write(const char* bytes, qint64 length)
    {
        // Exercise positive short writes through the real controller as well.
        const qint64 count = std::min(length, qint64(11));
        QByteArray data(bytes, count);
        if (m_state->text) data.replace("\n", "\r\n");
        m_state->data.append(data);
        return count;
    }
    bool flush() { return m_state->flushOk; }
    bool commit() { ++m_state->commitCalls; m_state->committed = m_state->commitOk; return m_state->commitOk; }
    void cancelWriting() { if (m_state) m_state->canceled = true; }
    QString errorString() const { return QStringLiteral("fake-QSaveFile-error"); }
private:
    std::shared_ptr<FakeFileState> m_state;
};
