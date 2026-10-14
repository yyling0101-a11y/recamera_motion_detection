#pragma once

#include <cstdint>
#include <deque>
#include <mutex>
#include <vector>

struct JpegSnapshot {
    std::vector<std::uint8_t> data;
    std::int64_t captureUnixMs = 0;
    std::uint64_t frameId      = 0;
    int width                  = 0;
    int height                 = 0;
};

// Keeps the most recent hardware-encoded JPEG frames so that HTTP clients can
// grab a live still and motion events can be matched with the frame they were
// detected on. All copies are small (tens of kilobytes) and bounded.
class SnapshotStore {
public:
    explicit SnapshotStore(std::size_t capacity = 16) : capacity_(capacity == 0 ? 1 : capacity) {}

    void push(const std::uint8_t* data, std::size_t size, std::uint64_t frameId, std::int64_t captureUnixMs, int width, int height) {
        if (data == nullptr || size == 0)
            return;

        JpegSnapshot snapshot;
        snapshot.data.assign(data, data + size);
        snapshot.frameId       = frameId;
        snapshot.captureUnixMs = captureUnixMs;
        snapshot.width         = width;
        snapshot.height        = height;

        std::lock_guard<std::mutex> lock(mutex_);
        snapshots_.push_back(std::move(snapshot));
        while (snapshots_.size() > capacity_) {
            snapshots_.pop_front();
        }
        ++published_;
    }

    bool latest(JpegSnapshot& out) const {
        std::lock_guard<std::mutex> lock(mutex_);
        if (snapshots_.empty())
            return false;
        out = snapshots_.back();
        return true;
    }

    // Returns the newest snapshot captured at or before captureUnixMs, falling
    // back to the oldest one still in the ring when the requested instant has
    // already been evicted.
    bool atOrBefore(std::int64_t captureUnixMs, JpegSnapshot& out) const {
        std::lock_guard<std::mutex> lock(mutex_);
        if (snapshots_.empty())
            return false;

        for (auto it = snapshots_.rbegin(); it != snapshots_.rend(); ++it) {
            if (it->captureUnixMs <= captureUnixMs) {
                out = *it;
                return true;
            }
        }
        out = snapshots_.front();
        return true;
    }

    std::uint64_t publishedCount() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return published_;
    }

    std::size_t size() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return snapshots_.size();
    }

private:
    mutable std::mutex mutex_;
    std::deque<JpegSnapshot> snapshots_;
    const std::size_t capacity_;
    std::uint64_t published_ = 0;
};
