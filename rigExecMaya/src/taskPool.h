// A small pool of worker threads for the runtime's task dispatch
// (RigExecRuntimeReader::SetTaskDispatch): the rig's independent parts of
// one evaluation run side by side, as they do in usdview.
#ifndef RIGEXEC_MAYA_TASK_POOL_H
#define RIGEXEC_MAYA_TASK_POOL_H

#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <deque>
#include <functional>
#include <mutex>
#include <thread>
#include <vector>

namespace rigExecMaya {

class TaskPool
{
public:
    explicit TaskPool(size_t workers);
    ~TaskPool();

    // Queues a task; tasks may queue more.
    void Run(std::function<void()> task);
    // Returns once every queued task, and every task they queued, is done.
    // The calling thread works through the queue meanwhile.
    void Wait();

    // The one pool interactive evaluation shares; none when this machine
    // has a single core.
    static TaskPool *Shared();
    static void ReleaseShared();

private:
    bool _RunOne();
    // Looks for work a little while before sleeping: a rig hands its
    // threads work in quick steps, and waking a sleeping thread for each
    // costs more than the step.
    bool _Linger(const std::function<bool()> &done) const;
    void _Work();

    std::mutex _mutex;
    std::condition_variable _wake;
    std::deque<std::function<void()>> _tasks;
    std::atomic<size_t> _queued{0};
    std::atomic<size_t> _unfinished{0};
    std::atomic<bool> _stopping{false};
    size_t _sleeping = 0;
    std::vector<std::thread> _threads;
};

}  // namespace rigExecMaya

#endif
