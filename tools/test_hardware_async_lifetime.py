"""Replay the production static-info worker with an instrumented Qt shim.

No GUI, driver, CIM or GPU call is made. A real worker thread runs the extracted
production function; guarded page access is forbidden outside the UI thread.
"""

import os
from pathlib import Path
import shutil
import subprocess
import unittest


ROOT = Path(__file__).resolve().parents[1]
SOURCE = ROOT / "Ksword5.1/Ksword5.1/HardwareDock/HardwareDock.cpp"


def function_text(source, signature):
    start = source.index(signature)
    opening = source.index("{", start)
    depth = 0
    for position in range(opening, len(source)):
        if source[position] == "{":
            depth += 1
        elif source[position] == "}":
            depth -= 1
            if depth == 0:
                return source[start : position + 1]
    raise AssertionError("Unclosed production function")


SHIM = r'''
#include <atomic>
#include <deque>
#include <functional>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

static const auto uiThread = std::this_thread::get_id();
static void check(bool condition) {
    if (!condition) throw std::runtime_error("lifetime replay invariant failed");
}
class QString {
public:
    std::string value;
    QString() = default;
    QString(const char* text) : value(text) {}
    explicit QString(std::string text) : value(std::move(text)) {}
    QString trimmed() const { return *this; }
    bool isEmpty() const { return value.empty(); }
    friend bool operator==(const QString& a, const QString& b) { return a.value == b.value; }
    friend QString operator+(const QString& a, const QString& b) { return QString(a.value + b.value); }
};
#define QStringLiteral(text) QString(text)
#define emit
class QObject {};
static QObject application;
static QObject* currentApplication = &application;
class QCoreApplication {
public:
    static QObject* instance() { return currentApplication; }
};
namespace Qt { enum ConnectionType { QueuedConnection }; }
static std::deque<std::function<void()>> uiQueue;
class QMetaObject {
public:
    template<class F> static bool invokeMethod(QObject* receiver, F callback, Qt::ConnectionType) {
        check(receiver == &application);
        check(std::this_thread::get_id() != uiThread);
        uiQueue.emplace_back(std::move(callback));
        return true;
    }
};
static std::deque<std::function<void()>> workerQueue;
class QThreadPool {
public:
    static QThreadPool* globalInstance() { static QThreadPool pool; return &pool; }
    template<class F> void start(F callback) { workerQueue.emplace_back(std::move(callback)); }
};
template<class T> class QPointer {
    T* pointer;
    std::shared_ptr<std::atomic_bool> alive;
public:
    explicit QPointer(T* value) : pointer(value), alive(value->alive) {}
    bool isNull() const {
        check(std::this_thread::get_id() == uiThread);
        return !alive->load();
    }
    T* data() const { return isNull() ? nullptr : pointer; }
    T* operator->() const { check(!isNull()); return pointer; }
};
struct MemoryHardwareSummarySnapshot {
    int speedMhz = 3200, usedSlots = 2, totalSlots = 4;
    QString formFactorText = QStringLiteral("DIMM");
};
struct GpuHardwareSummarySnapshot {
    QString adapterNameText = QStringLiteral("GPU");
    QString driverVersionText = QStringLiteral("version");
    QString driverDateText = QStringLiteral("date");
    QString pnpDeviceIdText = QStringLiteral("device");
    double dedicatedMemoryGiB = 8;
};
struct FieldNodes {
    FieldNodes& operator+=(const FieldNodes&) { return *this; }
};
namespace ks { namespace ui {
struct FieldDocument { FieldNodes nodes; bool isEmpty() const { return true; } };
}}
static int overviewQueries, detailQueries, signalsDelivered, refreshCalls;
static ks::ui::FieldDocument buildOverviewFieldsSnapshot() { ++overviewQueries; return {}; }
static ks::ui::FieldDocument buildOverviewPeripheralFieldsSnapshot(bool details) {
    if (details) ++detailQueries;
    return {};
}
static ks::ui::FieldDocument buildGpuFieldsSnapshot() { ++detailQueries; return {}; }
static ks::ui::FieldDocument buildMemoryFieldsSnapshot() { ++detailQueries; return {}; }
static MemoryHardwareSummarySnapshot queryMemoryHardwareSummarySnapshot() { ++detailQueries; return {}; }
static GpuHardwareSummarySnapshot queryGpuHardwareSummarySnapshot() { ++detailQueries; return {}; }
static ks::ui::FieldDocument buildGpuHardwareSummaryFields(const GpuHardwareSummarySnapshot&, const ks::ui::FieldDocument&) { return {}; }
class HardwareDock : public QObject {
public:
    std::shared_ptr<std::atomic_bool> alive = std::make_shared<std::atomic_bool>(true);
    std::atomic_bool m_staticInfoRefreshing{false};
    bool m_hardwareDetailsSamplingEnabled = false;
    ks::ui::FieldDocument m_cachedOverviewFields, m_cachedGpuFields, m_cachedMemoryFields;
    int m_memorySpeedMhz = 0, m_memorySlotUsed = 0, m_memorySlotTotal = 0;
    QString m_memoryFormFactorText, m_gpuAdapterNameText, m_gpuDriverVersionText;
    QString m_gpuDriverDateText, m_gpuPnpDeviceIdText;
    double m_gpuDedicatedMemoryGiB = 0;
    std::function<void()> onOverview;
    ~HardwareDock() { alive->store(false); }
    void requestAsyncStaticInfoRefresh();
    void staticOverviewFieldsChanged(const ks::ui::FieldDocument&) {
        ++signalsDelivered;
        const auto callback = onOverview;
        if (callback) callback();
    }
    void refreshStaticHardwareTexts(bool) { check(alive->load()); ++refreshCalls; }
};
'''

CASES = r'''
static void reset() {
    check(workerQueue.empty() && uiQueue.empty());
    overviewQueries = detailQueries = signalsDelivered = refreshCalls = 0;
    currentApplication = &application;
}
static void work() {
    check(!workerQueue.empty());
    auto callback = std::move(workerQueue.front()); workerQueue.pop_front();
    std::exception_ptr failure;
    std::thread worker([&] { try { callback(); } catch (...) { failure = std::current_exception(); } });
    worker.join();
    if (failure) std::rethrow_exception(failure);
}
static void deliver() {
    check(!uiQueue.empty());
    auto callback = std::move(uiQueue.front()); uiQueue.pop_front(); callback();
}
static int runCases() {
    // Normal overview delivery plus duplicate suppression and a subsequent refresh.
    reset();
    {
        HardwareDock page;
        page.requestAsyncStaticInfoRefresh(); page.requestAsyncStaticInfoRefresh();
        check(workerQueue.size() == 1 && page.m_staticInfoRefreshing.load());
        work(); check(page.m_cachedOverviewFields.isEmpty()); deliver();
        check(overviewQueries == 1 && detailQueries == 0 && signalsDelivered == 1);
        check(!page.m_staticInfoRefreshing.load() && refreshCalls == 1);
        page.requestAsyncStaticInfoRefresh(); work(); deliver();
        check(overviewQueries == 2 && !page.m_staticInfoRefreshing.load());
    }
    // The page can disappear before the worker starts, or after the callback queues.
    for (bool beforeWorker : {true, false}) {
        reset(); auto page = std::make_unique<HardwareDock>();
        page->requestAsyncStaticInfoRefresh();
        if (beforeWorker) page.reset();
        work(); page.reset(); deliver();
        check(signalsDelivered == 0 && refreshCalls == 0);
    }
    // No application receiver must restore the in-flight flag synchronously.
    reset();
    {
        HardwareDock page; currentApplication = nullptr; page.requestAsyncStaticInfoRefresh();
        check(!page.m_staticInfoRefreshing.load() && workerQueue.empty());
    }
    // Upgrading scope during an overview request queues exactly one details follow-up.
    reset();
    {
        HardwareDock page; page.requestAsyncStaticInfoRefresh();
        page.m_hardwareDetailsSamplingEnabled = true; work(); deliver();
        check(detailQueries == 0 && workerQueue.size() == 1);
        work(); deliver();
        check(detailQueries == 5 && page.m_memorySpeedMhz == 3200);
        check(page.m_gpuDedicatedMemoryGiB == 8 && !page.m_staticInfoRefreshing.load());
    }
    // A synchronous signal consumer may delete the page during result delivery.
    reset();
    {
        auto page = std::make_unique<HardwareDock>();
        page->onOverview = [&] { page.reset(); };
        page->requestAsyncStaticInfoRefresh(); work(); deliver();
        check(!page && signalsDelivered == 1 && refreshCalls == 0);
    }
    check(workerQueue.empty() && uiQueue.empty());
    std::cout << "PASS: 7 production hardware worker lifecycle scenarios\n";
    return 0;
}
int main() {
    try { return runCases(); }
    catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
'''


class HardwareAsyncLifetimeTests(unittest.TestCase):
    def replay(self, function):
        compiler = shutil.which("g++")
        if compiler is None:
            self.fail("g++ is required for the production-function lifecycle replay")
        scratch = ROOT / ".codex-tmp" / "hardware-lifetime"
        scratch.mkdir(parents=True, exist_ok=True)
        cpp = scratch / "replay.cpp"
        executable = scratch / "replay.exe"
        cpp.write_text(SHIM + function + CASES, encoding="utf-8")
        env = os.environ.copy()
        env["PATH"] = str(Path(compiler).parent) + os.pathsep + env.get("PATH", "")
        env["TEMP"] = env["TMP"] = str(scratch)
        subprocess.run(
            [compiler, "-std=c++17", "-Wall", "-Wextra", "-Werror", "-pthread", str(cpp), "-o", str(executable)],
            check=True, capture_output=True, text=True, env=env, timeout=60,
        )
        return subprocess.run([str(executable)], capture_output=True, text=True, env=env, timeout=15)

    def production_function(self):
        return function_text(SOURCE.read_text(encoding="utf-8-sig"),
                             "void HardwareDock::requestAsyncStaticInfoRefresh()")

    def test_production_worker_lifecycle(self):
        result = self.replay(self.production_function())
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn("PASS: 7", result.stdout)
        print(result.stdout.strip())

    def test_replay_rejects_page_receiver_on_worker_thread(self):
        function = self.production_function()
        mutation = function.replace("            applicationContext,",
                                    "            safeThis.data(),", 1)
        self.assertNotEqual(function, mutation)
        # Keep the captured application context used, so the mutant reaches runtime.
        mutation = mutation.replace("const QString overviewBaseText =",
                                    "(void)applicationContext;\n        const QString overviewBaseText =", 1)
        result = self.replay(mutation)
        self.assertEqual(result.returncode, 1, result.stderr)
        self.assertIn("lifetime replay invariant failed", result.stderr)

    def test_replay_rejects_access_after_signal_deletes_page(self):
        function = self.production_function()
        start = function.index("                // 同步信号接收者")
        end = function.index("                safeThis->refreshStaticHardwareTexts(false);", start)
        mutation = function[:start] + function[end:]
        result = self.replay(mutation)
        self.assertEqual(result.returncode, 1, result.stderr)
        self.assertIn("lifetime replay invariant failed", result.stderr)

    def test_all_page_workers_use_application_receiver(self):
        source = SOURCE.read_text(encoding="utf-8-sig")
        for name in ("R0HardwareHealth", "StaticInfo", "Sensor"):
            with self.subTest(worker=name):
                body = function_text(source, f"void HardwareDock::requestAsync{name}Refresh()")
                self.assertNotIn(".detach()", body)
                self.assertIn("QThreadPool::globalInstance()->start", body)
                dispatch = body.index("QMetaObject::invokeMethod(")
                self.assertNotIn("safeThis.isNull()", body[:dispatch])
                self.assertNotIn("safeThis->", body[:dispatch])
                self.assertNotIn("safeThis.data()", body)
                self.assertIn("applicationContext,", body[dispatch : dispatch + 110])


if __name__ == "__main__":
    unittest.main()
