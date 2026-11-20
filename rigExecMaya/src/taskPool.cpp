#include "taskPool.h"

#include <algorithm>
#include <chrono>
#include <memory>

namespace rigExecMaya {

namespace {
std::unique_ptr<TaskPool> gShared;

// How long a thread out of work keeps looking before it sleeps.
constexpr std::chrono::microseconds kLinger(200);
}  // namespace

TaskPool::TaskPool(size_t workers)
{
    for (size_t i = 0; i < workers; ++i) {
        _threads.emplace_back([this] { _Work(); });
    }
}

TaskPool::~TaskPool()
{
    {
        std::lock_guard<std::mutex> lock(_mutex);
        _stopping = true;
    }
    _wake.notify_all();
    for (std::thread &thread : _threads) {
        thread.join();
    }
}

void
TaskPool::Run(std::function<void()> task)
{
    bool wake;
    {
        std::lock_guard<std::mutex> lock(_mutex);
        _tasks.push_back(std::move(task));
        ++_queued;
        ++_unfinished;
        wake = _sleeping > 0;
    }
    if (wake) {
        _wake.notify_one();
    }
}

bool
TaskPool::_RunOne()
{
    if (_queued.load(std::memory_order_acquire) == 0) {
        return false;
    }
    std::function<void()> task;
    {
        std::lock_guard<std::mutex> lock(_mutex);
        if (_tasks.empty()) {
            return false;
        }
        task = std::move(_tasks.front());
        _tasks.pop_front();
        --_queued;
    }
    task();
    if (_unfinished.fetch_sub(1, std::memory_order_acq_rel) == 1) {
        bool wake;
        {
            std::lock_guard<std::mutex> lock(_mutex);
            wake = _sleeping > 0;
        }
        if (wake) {
            _wake.notify_all();
        }
    }
    return true;
}

bool
TaskPool::_Linger(const std::function<bool()> &done) const
{
    const auto until = std::chrono::steady_clock::now() + kLinger;
    while (std::chrono::steady_clock::now() < until) {
        if (_queued.load(std::memory_order_acquire) > 0 || done()) {
            return true;
        }
        std::this_thread::yield();
    }
    return false;
}

void
TaskPool::Wait()
{
    const auto done = [this] {
        return _unfinished.load(std::memory_order_acquire) == 0;
    };
    while (!done()) {
        if (_RunOne() || _Linger(done)) {
            continue;
        }
        std::unique_lock<std::mutex> lock(_mutex);
        ++_sleeping;
        _wake.wait(lock, [&] { return done() || !_tasks.empty(); });
        --_sleeping;
    }
}

void
TaskPool::_Work()
{
    const auto stopping = [this] {
        return _stopping.load(std::memory_order_acquire);
    };
    while (!stopping()) {
        if (_RunOne() || _Linger(stopping)) {
            continue;
        }
        std::unique_lock<std::mutex> lock(_mutex);
        ++_sleeping;
        _wake.wait(lock, [&] { return stopping() || !_tasks.empty(); });
        --_sleeping;
    }
}

TaskPool *
TaskPool::Shared()
{
    if (!gShared) {
        const size_t cores = std::max(1u, std::thread::hardware_concurrency());
        if (cores < 2) {
            return nullptr;
        }
        // The calling thread works too.
        gShared.reset(new TaskPool(std::min<size_t>(cores - 1, 15)));
    }
    return gShared.get();
}

void
TaskPool::ReleaseShared()
{
    gShared.reset();
}

}  // namespace rigExecMaya
