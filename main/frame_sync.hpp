#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <mutex>
#include <unordered_map>

struct FrameSyncInfo {
    std::uint64_t frameId = 0;
    std::uint64_t sourcePts = 0;
    std::uint64_t vencPts = 0;
    std::int64_t captureUnixMs = 0;
    int channel = -1;
    bool exactPtsMatch = false;
};

// Register each source frame once, before it branches to motion analysis and VENC.
// The VENC callback later consumes the matching record using the encoder PTS.
class FrameSyncRegistry {
public:
    static FrameSyncRegistry& instance();

    FrameSyncInfo registerSourceFrame(int channel,
                                      std::uint64_t sourcePts,
                                      std::int64_t captureUnixMs);

    bool matchEncodedFrame(int channel,
                           std::uint64_t vencPts,
                           FrameSyncInfo& result);

    void clear();

private:
    FrameSyncRegistry() = default;

    static constexpr std::size_t kMaxPendingPerChannel = 512;

    std::atomic<std::uint64_t> nextFrameId_{1};
    std::mutex mutex_;
    std::unordered_map<int, std::deque<FrameSyncInfo>> pending_;
};
