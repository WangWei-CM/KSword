#include "../shared/evidence/SerialTransactionQueue.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <future>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

namespace
{
    using ksword::evidence::SerialTransactionQueue;
    using namespace std::chrono_literals;
    int checks = 0;

    void require(const bool condition, const char* message)
    {
        ++checks;
        if (!condition)
        {
            throw std::runtime_error(message);
        }
    }

    void waitFor(std::future<void>& completion, const char* message)
    {
        require(completion.wait_for(5s) == std::future_status::ready, message);
        completion.get();
    }

    // A failed assertion must still release a blocked task before queue teardown.
    struct Gate
    {
        std::promise<void> release;
        std::shared_future<void> opened = release.get_future().share();
        bool signaled = false;
        void open()
        {
            if (!signaled)
            {
                signaled = true;
                release.set_value();
            }
        }
        ~Gate() { open(); }
    };

    void fifoAndSingleWorker()
    {
        std::vector<int> executionOrder;
        std::vector<std::thread::id> threads;
        std::atomic<int> active{0};
        std::atomic<int> maximum{0};
        {
            SerialTransactionQueue queue(256);
            Gate gate;
            auto started = std::make_shared<std::promise<void>>();
            auto startedFuture = started->get_future();
            require(queue.trySubmit([started, opened = gate.opened] {
                started->set_value();
                opened.wait();
            }), "blocker admitted");
            waitFor(startedFuture, "blocker started");
            for (int i = 0; i < 200; ++i)
            {
                require(queue.trySubmit([&, i] {
                    const int nowActive = active.fetch_add(1) + 1;
                    maximum.store(std::max(maximum.load(), nowActive));
                    executionOrder.push_back(i);
                    threads.push_back(std::this_thread::get_id());
                    std::this_thread::yield();
                    active.fetch_sub(1);
                }), "FIFO task admitted");
            }
            gate.open();
        }
        require(executionOrder.size() == 200, "all admitted FIFO tasks drained");
        for (int i = 0; i < 200; ++i)
        {
            require(executionOrder[static_cast<std::size_t>(i)] == i, "FIFO preserved");
        }
        require(maximum.load() == 1 && active.load() == 0, "maximum one task in flight");
        require(std::all_of(threads.begin(), threads.end(), [&](std::thread::id id) {
            return id == threads.front() && id != std::this_thread::get_id();
        }), "one dedicated worker executes all tasks");
    }

    void boundedAdmissionAndStopDrain()
    {
        std::atomic<int> performed{0};
        std::atomic<int> rejectedExecutions{0};
        {
            SerialTransactionQueue queue(2);
            Gate gate;
            auto started = std::make_shared<std::promise<void>>();
            auto startedFuture = started->get_future();
            require(queue.trySubmit([&, started, opened = gate.opened] {
                started->set_value();
                opened.wait();
                ++performed;
            }), "running task admitted");
            waitFor(startedFuture, "running task entered");
            require(queue.trySubmit([&] { ++performed; }), "first pending admitted");
            require(queue.trySubmit([&] { ++performed; }), "second pending admitted");
            require(!queue.trySubmit([&] { ++rejectedExecutions; }), "full queue rejects");
            require(!queue.trySubmit({}), "empty task rejects");
            queue.stopAccepting();
            queue.stopAccepting();
            require(!queue.trySubmit([&] { ++rejectedExecutions; }), "stopped queue rejects");
            require(performed.load() == 0, "stop does not run tasks on caller");
            gate.open();
        }
        require(performed.load() == 3, "stop and destruction drain all accepted work");
        require(rejectedExecutions.load() == 0, "rejected tasks never execute");
        SerialTransactionQueue zeroCapacity(0);
        require(!zeroCapacity.trySubmit([&] { ++rejectedExecutions; }), "zero capacity rejects");
    }

    void outlivesRequesterAndJoins()
    {
        struct Requester {};
        auto requester = std::make_unique<Requester>();
        std::atomic<int> sideEffects{0};
        std::weak_ptr<int> taskOwnedPayload;
        {
            SerialTransactionQueue processQueue(4);
            Gate gate;
            auto started = std::make_shared<std::promise<void>>();
            auto startedFuture = started->get_future();
            require(processQueue.trySubmit([started, opened = gate.opened] {
                started->set_value();
                opened.wait();
            }), "requester test blocker admitted");
            waitFor(startedFuture, "requester test blocker started");
            auto payload = std::make_shared<int>(17);
            taskOwnedPayload = payload;
            require(processQueue.trySubmit([payload, &sideEffects] {
                sideEffects.fetch_add(*payload);
            }), "requester transaction admitted");
            payload.reset();
            requester.reset();
            require(!taskOwnedPayload.expired(), "accepted task retains required dependency");
            require(sideEffects.load() == 0, "transaction remains queued after requester closes");
            gate.open();
        }
        require(sideEffects.load() == 17, "destructor joins requester-independent transaction");
        require(taskOwnedPayload.expired(), "finished task dependencies released");
    }

    void concurrentProducersAndWorkerSubmission()
    {
        std::atomic<int> performed{0};
        std::atomic<int> admitted{0};
        std::atomic<int> active{0};
        std::atomic<int> concurrent{0};
        {
            SerialTransactionQueue queue(1024);
            std::vector<std::thread> producers;
            for (int producer = 0; producer < 4; ++producer)
            {
                producers.emplace_back([&] {
                    for (int task = 0; task < 100; ++task)
                    {
                        if (queue.trySubmit([&] {
                            if (active.fetch_add(1) != 0)
                            {
                                ++concurrent;
                            }
                            std::this_thread::yield();
                            ++performed;
                            --active;
                        }))
                        {
                            ++admitted;
                        }
                    }
                });
            }
            for (auto& producer : producers)
            {
                producer.join();
            }
        }
        require(admitted.load() == 400 && performed.load() == 400,
            "concurrent producers retain every admitted task");
        require(concurrent.load() == 0 && active.load() == 0,
            "concurrent producers still execute serially");

        auto childDone = std::make_shared<std::promise<void>>();
        auto childFuture = childDone->get_future();
        auto childAdmitted = std::make_shared<std::promise<bool>>();
        auto admissionFuture = childAdmitted->get_future();
        SerialTransactionQueue queue(2);
        require(queue.trySubmit([&queue, childAdmitted, childDone] {
            childAdmitted->set_value(queue.trySubmit([childDone] { childDone->set_value(); }));
        }), "worker submission parent admitted");
        require(admissionFuture.wait_for(5s) == std::future_status::ready,
            "task may submit without holding queue lock");
        require(admissionFuture.get(), "worker submission child admitted");
        waitFor(childFuture, "worker submission child completed");
    }

    void destructorWaitsForAcceptedWork()
    {
        std::atomic<int> performed{0};
        auto queue = std::make_unique<SerialTransactionQueue>(2);
        Gate gate;
        auto started = std::make_shared<std::promise<void>>();
        auto startedFuture = started->get_future();
        require(queue->trySubmit([&, started, opened = gate.opened] {
            started->set_value();
            opened.wait();
            ++performed;
        }), "destructor test running task admitted");
        waitFor(startedFuture, "destructor test running task started");
        require(queue->trySubmit([&] { ++performed; }), "destructor test waiting task admitted");
        std::promise<void> destroying;
        auto destroyingFuture = destroying.get_future();
        auto destroyed = std::async(std::launch::async, [&] {
            destroying.set_value();
            queue.reset();
        });
        const bool destroyerStarted = destroyingFuture.wait_for(5s) == std::future_status::ready;
        const bool waitedForGate = destroyed.wait_for(20ms) == std::future_status::timeout;
        gate.open();
        const bool eventuallyJoined = destroyed.wait_for(5s) == std::future_status::ready;
        destroyed.get();
        require(destroyerStarted && waitedForGate, "destructor waits while accepted task is blocked");
        require(eventuallyJoined && performed.load() == 2, "destructor drains waiting task before return");
    }

    void reportsExceptionsAndContinues()
    {
        std::atomic<int> reported{0};
        std::atomic<int> subsequent{0};
        std::string receivedMessage;
        {
            SerialTransactionQueue queue(4, [&](std::exception_ptr failure) {
                try
                {
                    std::rethrow_exception(failure);
                }
                catch (const std::runtime_error& error)
                {
                    receivedMessage = error.what();
                }
                ++reported;
            });
            auto done = std::make_shared<std::promise<void>>();
            auto doneFuture = done->get_future();
            require(queue.trySubmit([] { throw std::runtime_error("transaction failure"); }),
                "throwing task admitted");
            require(queue.trySubmit([&, done] { ++subsequent; done->set_value(); }),
                "task following exception admitted");
            waitFor(doneFuture, "worker survived task exception");
            require(queue.unhandledTaskFailures() == 1, "failure count is observable");
        }
        require(reported.load() == 1 && receivedMessage == "transaction failure",
            "failure handler receives original exception");
        require(subsequent.load() == 1, "subsequent task executes exactly once");

        {
            SerialTransactionQueue queue(4, [](std::exception_ptr) {
                throw std::runtime_error("reporter failure");
            });
            auto done = std::make_shared<std::promise<void>>();
            auto doneFuture = done->get_future();
            require(queue.trySubmit([] { throw 42; }), "nonstandard exception task admitted");
            require(queue.trySubmit([done] { done->set_value(); }), "task after reporter failure admitted");
            waitFor(doneFuture, "worker survived reporter exception");
            require(queue.unhandledTaskFailures() == 1, "reporter exception preserves failure count");
        }
        {
            SerialTransactionQueue queue;
            auto done = std::make_shared<std::promise<void>>();
            auto doneFuture = done->get_future();
            require(queue.trySubmit([] { throw 13; }), "default-reporter failure admitted");
            require(queue.trySubmit([done] { done->set_value(); }), "default-reporter continuation admitted");
            waitFor(doneFuture, "worker survived default-reporter failure");
            require(queue.unhandledTaskFailures() == 1, "default-reporter failure remains observable");
        }
    }
}

int main()
{
    try
    {
        fifoAndSingleWorker();
        boundedAdmissionAndStopDrain();
        outlivesRequesterAndJoins();
        concurrentProducersAndWorkerSubmission();
        destructorWaitsForAcceptedWork();
        reportsExceptionsAndContinues();
        std::cout << "PASS SerialTransactionQueue " << checks << " checks\n";
        return 0;
    }
    catch (const std::exception& error)
    {
        std::cerr << "FAIL SerialTransactionQueue: " << error.what() << '\n';
        return 1;
    }
}
