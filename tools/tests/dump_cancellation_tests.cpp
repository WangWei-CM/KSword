// Pure request-lifetime/worker tests: no Qt, dumps, events or system targets.
#include "../../Ksword5.1/Ksword5.1/MinidumpDock/DumpCancellation.h"
#include <array>
#include <future>
#include <iostream>
#include <stdexcept>

using namespace ks::minidump;
static int checks = 0;
void require(bool value, const char* message)
{
    ++checks;
    if (!value) { throw std::runtime_error(message); }
}

int main()
{
    CheckDumpCancellation();
    auto cancelled = std::make_shared<std::atomic_bool>(true);
    auto active = std::make_shared<std::atomic_bool>(false);
    {
        ScopedDumpCancellation outer(cancelled);
        bool threw = false;
        try { CheckDumpCancellation(); } catch (const DumpWorkCancelled&) { threw = true; }
        require(threw, "cancelled scope must stop");
        {
            ScopedDumpCancellation inner(active);
            CheckDumpCancellation();
        }
        threw = false;
        try { CheckDumpCancellation(); } catch (const DumpWorkCancelled&) { threw = true; }
        require(threw, "nested scope must restore previous request");
        auto isolated = std::async(std::launch::async, [] { CheckDumpCancellation(); return true; });
        require(isolated.get(), "scope must not cancel unrelated worker");
    }
    CheckDumpCancellation();

    // Cancellation at every stage must stop all later stage calls and unwind
    // resources once, without any GUI lifetime dependency.
    for (int cancelAfter = 0; cancelAfter < 4; ++cancelAfter)
    {
        auto token = std::make_shared<std::atomic_bool>(false);
        std::array<int, 4> calls{};
        int released = 0;
        try
        {
            ScopedDumpCancellation scope(token);
            struct Resource { int& released; ~Resource() { ++released; } } resource{released};
            for (int phase = 0; phase < 4; ++phase)
            {
                CheckDumpCancellation();
                ++calls[phase];
                if (phase == cancelAfter) { token->store(true); }
                CheckDumpCancellation();
            }
        }
        catch (const DumpWorkCancelled&) {}
        require(released == 1, "request resources must release exactly once");
        for (int phase = 0; phase < 4; ++phase)
        {
            require(calls[phase] == (phase <= cancelAfter ? 1 : 0), "cancelled request started another stage");
        }
    }

    // Hiding a page has no token operation: the accepted request still runs all
    // phases and leaves its returned data complete.
    {
        ScopedDumpCancellation scope(active);
        int phases = 0;
        for (int phase = 0; phase < 4; ++phase) { CheckDumpCancellation(); ++phases; }
        require(phases == 4, "uncancelled request must retain all stages");
    }

    std::mutex mutex;
    std::unique_lock<std::mutex> holder(mutex);
    auto token = std::make_shared<std::atomic_bool>(false);
    std::promise<void> waiting;
    auto worker = std::async(std::launch::async, [&]
    {
        ScopedDumpCancellation scope(token);
        waiting.set_value();
        try { auto lock = LockDumpMutex(mutex); return false; }
        catch (const DumpWorkCancelled&) { return true; }
    });
    waiting.get_future().wait();
    token->store(true);
    require(worker.wait_for(std::chrono::seconds(2)) == std::future_status::ready,
            "cancelled lock waiter must not wait for holder");
    require(worker.get(), "cancelled lock waiter must report cancellation");
    require(holder.owns_lock(), "cancellation must not unlock another session");
    holder.unlock();
    {
        ScopedDumpCancellation scope(active);
        try { auto lock = LockDumpMutex(mutex); throw std::runtime_error("mock setup failure"); }
        catch (const std::runtime_error&) {}
        require(mutex.try_lock(), "exception during session setup must release serialization");
        mutex.unlock();
    }
    {
        ScopedDumpCancellation scope(cancelled);
        bool threw = false;
        try { auto lock = LockDumpMutex(mutex); } catch (const DumpWorkCancelled&) { threw = true; }
        require(threw, "already-cancelled request must not acquire session");
        require(mutex.try_lock(), "cancelled session must leave mutex free");
        mutex.unlock();
    }
    std::cout << "PASS " << checks << " checks\n";
}
