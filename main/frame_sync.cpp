#include "frame_sync.hpp"

#include <algorithm>
#include <iterator>

FrameSyncRegistry& FrameSyncRegistry::instance() {
    static FrameSyncRegistry registry;
    return registry;
}

FrameSyncInfo FrameSyncRegistry::registerSourceFrame(int channel,
                                                     std::uint64_t sourcePts,
                                                     std::int64_t captureUnixMs) {
    FrameSyncInfo info;
    info.frameId = nextFrameId_.fetch_add(1, std::memory_order_relaxed);
    info.sourcePts = sourcePts;
    info.captureUnixMs = captureUnixMs;
    info.channel = channel;

    std::lock_guard<std::mutex> lock(mutex_);
    auto& queue = pending_[channel];
    queue.push_back(info);
    while (queue.size() > kMaxPendingPerChannel) {
        queue.pop_front();
    }
    return info;
}

bool FrameSyncRegistry::matchEncodedFrame(int channel,
                                          std::uint64_t vencPts,
                                          FrameSyncInfo& result) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto mapIt = pending_.find(channel);
    if (mapIt == pending_.end() || mapIt->second.empty()) {
        return false;
    }

    auto& queue = mapIt->second;
    auto exactIt = std::find_if(queue.begin(), queue.end(), [vencPts](const FrameSyncInfo& item) {
        return item.sourcePts == vencPts;
    });

    if (exactIt != queue.end()) {
        result = *exactIt;
        result.vencPts = vencPts;
        result.exactPtsMatch = true;
        queue.erase(queue.begin(), std::next(exactIt));
        return true;
    }

    // Some pipelines rewrite PTS while keeping frame order. In that case,
    // use FIFO as a controlled fallback and emit a warning in the caller.
    result = queue.front();
    result.vencPts = vencPts;
    result.exactPtsMatch = false;
    queue.pop_front();
    return true;
}

void FrameSyncRegistry::clear() {
    std::lock_guard<std::mutex> lock(mutex_);
    pending_.clear();
    nextFrameId_.store(1, std::memory_order_relaxed);
}
