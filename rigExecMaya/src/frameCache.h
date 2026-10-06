// Evaluated frames kept for playback, and the worker pool that fills them.
//
// A frame is keyed by EVERYTHING the node sends the worker: the time and
// every channel value. A lookup compares the whole key, so an edit to any
// channel simply misses -- there is nothing to invalidate, and a stale frame
// cannot be served. The pool evaluates frames ahead of playback in parallel,
// one worker process per thread, while the node's own worker stays free for
// interactive edits.
#ifndef RIGEXEC_MAYA_FRAME_CACHE_H
#define RIGEXEC_MAYA_FRAME_CACHE_H

#include "workerProcess.h"

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <list>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

namespace rigExecMaya {

using Frame = std::shared_ptr<const std::vector<uint8_t>>;

class FrameCache
{
public:
    explicit FrameCache(size_t capacity) : _capacity(capacity) {}

    Frame Find(double time, const std::vector<double> &values);
    void Insert(double time, const std::vector<double> &values, Frame frame);
    size_t Size() const;
    void Clear();

private:
    struct Entry {
        double time;
        std::vector<double> values;
        Frame frame;
    };
    static uint64_t _Hash(double time, const std::vector<double> &values);

    mutable std::mutex _mutex;
    size_t _capacity;
    // Most recently used first; the map points into it.
    std::list<Entry> _entries;
    std::unordered_multimap<uint64_t, std::list<Entry>::iterator> _index;
};

// A job for the pool: one frame's full key.
struct FillJob {
    double time;
    std::vector<double> values;
};

class FillPool
{
public:
    FillPool(std::string launcher, std::string rigRoot, std::string stage,
             std::string rigPath, std::shared_ptr<FrameCache> cache,
             size_t workers);
    ~FillPool();

    // Replaces whatever is still queued: the newest request describes the
    // animation as it is now, and a queued job from an older one would only
    // fill a key nothing looks up any more.
    void Submit(std::vector<FillJob> jobs);

    bool Idle() const;
    size_t Pending() const;

private:
    void _Run(size_t index);

    std::string _launcher, _rigRoot, _stage, _rigPath;
    std::shared_ptr<FrameCache> _cache;
    mutable std::mutex _mutex;
    std::condition_variable _wake;
    std::deque<FillJob> _queue;
    std::atomic<size_t> _running{0};
    bool _stopping = false;
    std::vector<std::thread> _threads;
};

}  // namespace rigExecMaya

#endif
