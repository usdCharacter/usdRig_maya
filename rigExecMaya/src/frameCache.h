// Evaluated frames kept for playback, and the thread pool that fills them.
//
// A frame is keyed by EVERYTHING the rig is posed with: every channel value
// (the bake is one frame, so time itself is not an input). A lookup compares
// the whole key, so an edit to any channel simply misses -- there is
// nothing to invalidate, and a stale frame cannot be served. The pool
// evaluates frames ahead of playback in parallel, one evaluator per thread,
// while the node's own evaluator stays free for interactive edits.
#ifndef RIGEXEC_MAYA_FRAME_CACHE_H
#define RIGEXEC_MAYA_FRAME_CACHE_H

#include "rigEvaluator.h"

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

    Frame Find(const std::vector<double> &values);
    void Insert(const std::vector<double> &values, Frame frame);
    size_t Size() const;
    void Clear();

private:
    struct Entry {
        std::vector<double> values;
        Frame frame;
    };
    static uint64_t _Hash(const std::vector<double> &values);

    mutable std::mutex _mutex;
    size_t _capacity;
    // Most recently used first; the map points into it.
    std::list<Entry> _entries;
    std::unordered_multimap<uint64_t, std::list<Entry>::iterator> _index;
};

// A job for the pool: one frame's full key, and its time (which orders
// the queue: nearest the playhead first).
struct FillJob {
    double time;
    std::vector<double> values;
};

class FillPool
{
public:
    FillPool(std::shared_ptr<const RigSetup> setup,
             std::shared_ptr<FrameCache> cache, size_t workers);
    ~FillPool();

    // Replaces whatever is still queued: the newest request describes the
    // animation as it is now, and a queued job from an older one would only
    // fill a key nothing looks up any more.
    void Submit(std::vector<FillJob> jobs);

    bool Idle() const;
    size_t Pending() const;

private:
    void _Run(size_t index);

    std::shared_ptr<const RigSetup> _setup;
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
