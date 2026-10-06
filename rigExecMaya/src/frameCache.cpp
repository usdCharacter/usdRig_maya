#include "frameCache.h"

#include <cstring>

namespace rigExecMaya {

uint64_t
FrameCache::_Hash(double time, const std::vector<double> &values)
{
    // FNV-1a over the key's bytes: the same bytes the lookup then compares.
    uint64_t hash = 1469598103934665603ull;
    const auto mix = [&hash](const void *data, size_t size) {
        const auto *bytes = static_cast<const uint8_t *>(data);
        for (size_t i = 0; i < size; ++i) {
            hash = (hash ^ bytes[i]) * 1099511628211ull;
        }
    };
    mix(&time, sizeof(time));
    mix(values.data(), values.size() * sizeof(double));
    return hash;
}

Frame
FrameCache::Find(double time, const std::vector<double> &values)
{
    const uint64_t hash = _Hash(time, values);
    std::lock_guard<std::mutex> lock(_mutex);
    auto range = _index.equal_range(hash);
    for (auto it = range.first; it != range.second; ++it) {
        const Entry &entry = *it->second;
        if (entry.time == time && entry.values == values) {
            _entries.splice(_entries.begin(), _entries, it->second);
            return entry.frame;
        }
    }
    return nullptr;
}

void
FrameCache::Insert(double time, const std::vector<double> &values,
                   Frame frame)
{
    if (!frame || _capacity == 0) {
        return;
    }
    const uint64_t hash = _Hash(time, values);
    std::lock_guard<std::mutex> lock(_mutex);
    auto range = _index.equal_range(hash);
    for (auto it = range.first; it != range.second; ++it) {
        if (it->second->time == time && it->second->values == values) {
            return;
        }
    }
    _entries.push_front(Entry{time, values, std::move(frame)});
    _index.emplace(hash, _entries.begin());
    while (_entries.size() > _capacity) {
        const auto last = std::prev(_entries.end());
        const uint64_t lastHash = _Hash(last->time, last->values);
        auto victims = _index.equal_range(lastHash);
        for (auto it = victims.first; it != victims.second; ++it) {
            if (it->second == last) {
                _index.erase(it);
                break;
            }
        }
        _entries.pop_back();
    }
}

size_t
FrameCache::Size() const
{
    std::lock_guard<std::mutex> lock(_mutex);
    return _entries.size();
}

void
FrameCache::Clear()
{
    std::lock_guard<std::mutex> lock(_mutex);
    _entries.clear();
    _index.clear();
}

FillPool::FillPool(std::string launcher, std::string rigRoot,
                   std::string stage, std::string rigPath,
                   std::shared_ptr<FrameCache> cache, size_t workers)
    : _launcher(std::move(launcher)), _rigRoot(std::move(rigRoot)),
      _stage(std::move(stage)), _rigPath(std::move(rigPath)),
      _cache(std::move(cache))
{
    for (size_t i = 0; i < workers; ++i) {
        _threads.emplace_back(&FillPool::_Run, this, i);
    }
}

FillPool::~FillPool()
{
    {
        std::lock_guard<std::mutex> lock(_mutex);
        _stopping = true;
        _queue.clear();
    }
    _wake.notify_all();
    for (std::thread &thread : _threads) {
        thread.join();
    }
}

void
FillPool::Submit(std::vector<FillJob> jobs)
{
    {
        std::lock_guard<std::mutex> lock(_mutex);
        _queue.clear();
        for (FillJob &job : jobs) {
            _queue.push_back(std::move(job));
        }
    }
    _wake.notify_all();
}

bool
FillPool::Idle() const
{
    std::lock_guard<std::mutex> lock(_mutex);
    return _queue.empty() && _running == 0;
}

size_t
FillPool::Pending() const
{
    std::lock_guard<std::mutex> lock(_mutex);
    return _queue.size() + _running;
}

void
FillPool::_Run(size_t)
{
    // Each thread owns its worker process outright, started on its first
    // job so a scene that never plays back never pays for the pool.
    std::unique_ptr<WorkerProcess> worker;
    for (;;) {
        FillJob job;
        {
            std::unique_lock<std::mutex> lock(_mutex);
            _wake.wait(lock, [this] { return _stopping || !_queue.empty(); });
            if (_stopping) {
                return;
            }
            job = std::move(_queue.front());
            _queue.pop_front();
            ++_running;
        }
        if (!_cache->Find(job.time, job.values)) {
            std::string error;
            if (!worker || !worker->IsRunning()) {
                worker.reset(new WorkerProcess());
                if (!worker->Start(_launcher, _rigRoot, _stage, _rigPath,
                                   &error)) {
                    worker.reset();
                }
            }
            std::vector<uint8_t> bytes;
            if (worker && worker->Evaluate(job.time, job.values, &bytes,
                                           &error)) {
                _cache->Insert(job.time, job.values,
                               std::make_shared<const std::vector<uint8_t>>(
                                   std::move(bytes)));
            }
        }
        --_running;
    }
}

}  // namespace rigExecMaya
