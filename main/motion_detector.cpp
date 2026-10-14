#include "motion_detector.hpp"

#include "mini_json.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>

MotionDetector::MotionDetector(int width, int height)
    : width_(width),
      height_(height),
      config_(defaultMotionConfig()),
      activeMask_(static_cast<std::size_t>(width * height), 0),
      backgroundQ8_(static_cast<std::size_t>(width * height), 0),
      rawForeground_(static_cast<std::size_t>(width * height), 0),
      filteredForeground_(static_cast<std::size_t>(width * height), 0),
      visited_(static_cast<std::size_t>(width * height), 0) {
    queue_.reserve(static_cast<std::size_t>(width * height));
    status_.analysisWidth    = width_;
    status_.analysisHeight   = height_;
    status_.lastChangeUnixMs = nowUnixMs();
    rebuildMaskLocked();
}

void MotionDetector::setConfig(const MotionConfig& config) {
    std::lock_guard<std::mutex> lock(mutex_);
    config_ = config;
    rebuildMaskLocked();
    resetBackgroundLocked();
    status_.lastError.clear();
}

void MotionDetector::setNotify(const NotifyConfig& notify) {
    std::lock_guard<std::mutex> lock(mutex_);
    config_.notify = notify;
}

MotionConfig MotionDetector::getConfig() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return config_;
}

void MotionDetector::resetBackground() {
    std::lock_guard<std::mutex> lock(mutex_);
    resetBackgroundLocked();
}

void MotionDetector::setError(const std::string& error) {
    std::lock_guard<std::mutex> lock(mutex_);
    status_.lastError = error;
}

void MotionDetector::setEventCallback(EventCallback callback) {
    std::lock_guard<std::mutex> lock(eventsMutex_);
    eventCallback_ = std::move(callback);
}

void MotionDetector::recordEvent(const MotionEvent& event) {
    EventCallback callback;
    {
        std::lock_guard<std::mutex> lock(eventsMutex_);
        events_.push_back(event);
        while (events_.size() > kMaxEvents) {
            events_.pop_front();
        }
        callback = eventCallback_;
    }
    if (callback) {
        callback(event);
    }
}

bool MotionDetector::lastEvent(MotionEvent& event) const {
    std::lock_guard<std::mutex> lock(eventsMutex_);
    if (events_.empty())
        return false;
    event = events_.back();
    return true;
}

std::vector<MotionEvent> MotionDetector::recentEvents(std::uint64_t afterEventId, std::size_t limit) const {
    std::vector<MotionEvent> result;
    std::lock_guard<std::mutex> lock(eventsMutex_);
    for (const MotionEvent& event : events_) {
        if (event.eventId <= afterEventId)
            continue;
        result.push_back(event);
        if (limit > 0 && result.size() >= limit)
            break;
    }
    return result;
}

bool MotionDetector::pointInPolygon(double x, double y, const std::vector<NormalizedPoint>& polygon) const {
    bool inside         = false;
    const std::size_t n = polygon.size();
    for (std::size_t i = 0, j = n - 1; i < n; j = i++) {
        const NormalizedPoint& pi = polygon[i];
        const NormalizedPoint& pj = polygon[j];
        const bool intersects     = ((pi.y > y) != (pj.y > y)) && (x < (pj.x - pi.x) * (y - pi.y) / ((pj.y - pi.y) + 1e-12) + pi.x);
        if (intersects)
            inside = !inside;
    }
    return inside;
}

void MotionDetector::rebuildMaskLocked() {
    std::fill(activeMask_.begin(), activeMask_.end(), 0);

    bool hasInclude = false;
    for (const MotionZone& zone : config_.zones) {
        if (zone.type == ZoneType::Include) {
            hasInclude = true;
            break;
        }
    }

    if (hasInclude) {
        for (int y = 0; y < height_; ++y) {
            const double ny = (static_cast<double>(y) + 0.5) / static_cast<double>(height_);
            for (int x = 0; x < width_; ++x) {
                const double nx = (static_cast<double>(x) + 0.5) / static_cast<double>(width_);
                bool included   = false;
                for (const MotionZone& zone : config_.zones) {
                    if (zone.type == ZoneType::Include && pointInPolygon(nx, ny, zone.points)) {
                        included = true;
                        break;
                    }
                }
                if (!included)
                    continue;

                bool excluded = false;
                for (const MotionZone& zone : config_.zones) {
                    if (zone.type == ZoneType::Exclude && pointInPolygon(nx, ny, zone.points)) {
                        excluded = true;
                        break;
                    }
                }
                if (!excluded)
                    activeMask_[static_cast<std::size_t>(y * width_ + x)] = 1;
            }
        }
    }

    status_.activeMaskPixels = static_cast<int>(std::count(activeMask_.begin(), activeMask_.end(), static_cast<std::uint8_t>(1)));
    status_.configured       = hasInclude && status_.activeMaskPixels > 0;
    status_.enabled          = config_.enabled;
}

void MotionDetector::resetBackgroundLocked() {
    std::fill(backgroundQ8_.begin(), backgroundQ8_.end(), 0);
    std::fill(rawForeground_.begin(), rawForeground_.end(), 0);
    std::fill(filteredForeground_.begin(), filteredForeground_.end(), 0);
    backgroundReady_  = false;
    framesSinceReset_ = 0;

    status_.warmingUp               = true;
    status_.motion                  = false;
    status_.motionPixels            = 0;
    status_.largestBlobPixels       = 0;
    status_.motionRatio             = 0.0;
    status_.bboxValid               = false;
    status_.consecutiveMotionFrames = 0;
    status_.consecutiveQuietFrames  = 0;
    status_.lastChangeUnixMs        = nowUnixMs();
}

std::int64_t MotionDetector::nowUnixMs() const {
    using namespace std::chrono;
    return duration_cast<milliseconds>(system_clock::now().time_since_epoch()).count();
}

void MotionDetector::processGrayFrame(const std::uint8_t* yPlane, int sourceWidth, int sourceHeight, int sourceStride, std::uint64_t frameId, std::uint64_t sourcePts, std::int64_t captureUnixMs) {
    if (yPlane == nullptr)
        return;

    MotionEvent pendingEvent;
    bool hasPendingEvent = false;

    std::unique_lock<std::mutex> lock(mutex_);
    ++status_.frameCount;
    status_.frameId       = frameId;
    status_.sourcePts     = sourcePts;
    status_.captureUnixMs = captureUnixMs > 0 ? captureUnixMs : nowUnixMs();
    status_.enabled       = config_.enabled;

    if (!config_.enabled || !status_.configured) {
        status_.motion            = false;
        status_.warmingUp         = true;
        status_.motionPixels      = 0;
        status_.largestBlobPixels = 0;
        status_.motionRatio       = 0.0;
        status_.bboxValid         = false;
        commitHistoryLocked();
        return;
    }

    if (sourceWidth != width_ || sourceHeight != height_ || sourceStride < sourceWidth) {
        status_.lastError = "Unexpected analysis frame size/stride";
        commitHistoryLocked();
        return;
    }

    if (!backgroundReady_) {
        for (int y = 0; y < height_; ++y) {
            const std::uint8_t* row = yPlane + static_cast<std::size_t>(y * sourceStride);
            for (int x = 0; x < width_; ++x) {
                backgroundQ8_[static_cast<std::size_t>(y * width_ + x)] = static_cast<std::uint16_t>(row[x]) << 8U;
            }
        }
        backgroundReady_  = true;
        framesSinceReset_ = 1;
        status_.warmingUp = config_.warmupFrames > 1;
        commitHistoryLocked();
        return;
    }

    ++framesSinceReset_;
    status_.warmingUp = framesSinceReset_ <= config_.warmupFrames;

    const int bgAlphaQ16 = std::max(1, static_cast<int>(std::lround(config_.backgroundAlpha * 65536.0)));
    const int fgAlphaQ16 = std::max(0, static_cast<int>(std::lround(config_.foregroundAlpha * 65536.0)));

    for (int y = 0; y < height_; ++y) {
        const std::uint8_t* row = yPlane + static_cast<std::size_t>(y * sourceStride);
        for (int x = 0; x < width_; ++x) {
            const std::size_t index = static_cast<std::size_t>(y * width_ + x);
            if (!activeMask_[index]) {
                rawForeground_[index] = 0;
                continue;
            }

            const int currentQ8   = static_cast<int>(row[x]) << 8;
            const int bgQ8        = static_cast<int>(backgroundQ8_[index]);
            const int diff        = std::abs((currentQ8 - bgQ8) >> 8);
            const bool foreground = diff >= config_.pixelThreshold;
            rawForeground_[index] = foreground ? 1 : 0;

            const int alphaQ16 = foreground ? fgAlphaQ16 : bgAlphaQ16;
            if (alphaQ16 > 0) {
                const int delta      = currentQ8 - bgQ8;
                int updated          = bgQ8 + static_cast<int>((static_cast<long long>(delta) * alphaQ16) >> 16);
                updated              = std::max(0, std::min(65535, updated));
                backgroundQ8_[index] = static_cast<std::uint16_t>(updated);
            }
        }
    }

    std::fill(filteredForeground_.begin(), filteredForeground_.end(), 0);
    for (int y = 1; y < height_ - 1; ++y) {
        for (int x = 1; x < width_ - 1; ++x) {
            const std::size_t index = static_cast<std::size_t>(y * width_ + x);
            if (!activeMask_[index])
                continue;
            int neighbors = 0;
            for (int dy = -1; dy <= 1; ++dy) {
                for (int dx = -1; dx <= 1; ++dx) {
                    neighbors += rawForeground_[static_cast<std::size_t>((y + dy) * width_ + (x + dx))];
                }
            }
            filteredForeground_[index] = neighbors >= 4 ? 1 : 0;
        }
    }

    std::fill(visited_.begin(), visited_.end(), 0);
    int totalMotionPixels = 0;
    int largestBlob       = 0;
    int bestMinX          = 0;
    int bestMinY          = 0;
    int bestMaxX          = 0;
    int bestMaxY          = 0;

    for (int y = 0; y < height_; ++y) {
        for (int x = 0; x < width_; ++x) {
            const int start = y * width_ + x;
            if (!filteredForeground_[static_cast<std::size_t>(start)])
                continue;
            ++totalMotionPixels;
            if (visited_[static_cast<std::size_t>(start)])
                continue;

            queue_.clear();
            queue_.push_back(start);
            visited_[static_cast<std::size_t>(start)] = 1;
            std::size_t head                          = 0;
            int area                                  = 0;
            int minX                                  = x;
            int minY                                  = y;
            int maxX                                  = x;
            int maxY                                  = y;

            while (head < queue_.size()) {
                const int index = queue_[head++];
                const int cy    = index / width_;
                const int cx    = index - cy * width_;
                ++area;
                minX = std::min(minX, cx);
                minY = std::min(minY, cy);
                maxX = std::max(maxX, cx);
                maxY = std::max(maxY, cy);

                static const int kDx[4] = {1, -1, 0, 0};
                static const int kDy[4] = {0, 0, 1, -1};
                for (int k = 0; k < 4; ++k) {
                    const int nx = cx + kDx[k];
                    const int ny = cy + kDy[k];
                    if (nx < 0 || nx >= width_ || ny < 0 || ny >= height_)
                        continue;
                    const int ni = ny * width_ + nx;
                    if (!filteredForeground_[static_cast<std::size_t>(ni)] || visited_[static_cast<std::size_t>(ni)])
                        continue;
                    visited_[static_cast<std::size_t>(ni)] = 1;
                    queue_.push_back(ni);
                }
            }

            if (area > largestBlob) {
                largestBlob = area;
                bestMinX    = minX;
                bestMinY    = minY;
                bestMaxX    = maxX;
                bestMaxY    = maxY;
            }
        }
    }

    status_.motionPixels      = totalMotionPixels;
    status_.largestBlobPixels = largestBlob;
    status_.motionRatio       = status_.activeMaskPixels > 0 ? static_cast<double>(totalMotionPixels) / static_cast<double>(status_.activeMaskPixels) : 0.0;
    status_.bboxValid         = largestBlob >= config_.minBlobPixels;
    if (status_.bboxValid) {
        status_.bboxX = bestMinX;
        status_.bboxY = bestMinY;
        status_.bboxW = bestMaxX - bestMinX + 1;
        status_.bboxH = bestMaxY - bestMinY + 1;
    }

    const bool candidate = !status_.warmingUp && totalMotionPixels >= config_.minMotionPixels && largestBlob >= config_.minBlobPixels;

    if (candidate) {
        ++status_.consecutiveMotionFrames;
        status_.consecutiveQuietFrames = 0;
        status_.lastMotionUnixMs       = status_.captureUnixMs;
        if (!status_.motion && status_.consecutiveMotionFrames >= config_.triggerFrames) {
            status_.motion = true;
            ++status_.eventId;
            status_.lastChangeUnixMs = status_.lastMotionUnixMs;
            std::fprintf(stderr,
                         "[motion] START event=%llu pixels=%d blob=%d ratio=%.4f bbox=%d,%d,%d,%d\n",
                         static_cast<unsigned long long>(status_.eventId),
                         status_.motionPixels,
                         status_.largestBlobPixels,
                         status_.motionRatio,
                         status_.bboxX,
                         status_.bboxY,
                         status_.bboxW,
                         status_.bboxH);
            pendingEvent                = makeEventLocked("motion_start");
            hasPendingEvent             = true;
        }
    } else {
        status_.consecutiveMotionFrames = 0;
        ++status_.consecutiveQuietFrames;
        if (status_.motion && status_.consecutiveQuietFrames >= config_.clearFrames) {
            status_.motion           = false;
            status_.lastChangeUnixMs = status_.captureUnixMs;
            std::fprintf(stderr, "[motion] END event=%llu\n", static_cast<unsigned long long>(status_.eventId));
            pendingEvent    = makeEventLocked("motion_end");
            hasPendingEvent = true;
        }
    }

    commitHistoryLocked();

    lock.unlock();
    if (hasPendingEvent)
        recordEvent(pendingEvent);
}

MotionEvent MotionDetector::makeEventLocked(const char* type) const {
    MotionEvent event;
    event.eventId           = status_.eventId;
    event.type              = type;
    event.unixMs            = nowUnixMs();
    event.captureUnixMs     = status_.captureUnixMs;
    event.frameId           = status_.frameId;
    event.sourcePts         = status_.sourcePts;
    event.motionPixels      = status_.motionPixels;
    event.largestBlobPixels = status_.largestBlobPixels;
    event.motionRatio       = status_.motionRatio;
    event.bboxValid         = status_.bboxValid;
    event.bboxX             = status_.bboxX;
    event.bboxY             = status_.bboxY;
    event.bboxW             = status_.bboxW;
    event.bboxH             = status_.bboxH;
    event.analysisWidth     = status_.analysisWidth;
    event.analysisHeight    = status_.analysisHeight;
    return event;
}

void MotionDetector::commitHistoryLocked() {
    history_.push_back(status_);
    while (history_.size() > kMaxHistoryFrames) {
        history_.pop_front();
    }
}

MotionStatus MotionDetector::getStatus() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return status_;
}

namespace {

mini_json::Value motionStatusToJson(const MotionStatus& status, std::int64_t responseUnixMs) {
    mini_json::Value root = mini_json::Value::object();
    root["enabled"]       = status.enabled;
    root["configured"]    = status.configured;
    root["warming_up"]    = status.warmingUp;
    root["motion"]        = status.motion;

    // mini_json stores numeric values as double. Keep 64-bit IDs as strings.
    root["frame_id"]    = std::to_string(status.frameId);
    root["source_pts"]  = std::to_string(status.sourcePts);
    root["frame_count"] = static_cast<double>(status.frameCount);
    root["event_id"]    = static_cast<double>(status.eventId);

    // timestamp_unix_ms remains for compatibility, but now means the capture
    // time of this exact source frame rather than the HTTP response time.
    root["timestamp_unix_ms"]         = static_cast<double>(status.captureUnixMs);
    root["capture_unix_ms"]           = static_cast<double>(status.captureUnixMs);
    root["response_unix_ms"]          = static_cast<double>(responseUnixMs);
    root["last_change_unix_ms"]       = static_cast<double>(status.lastChangeUnixMs);
    root["last_motion_unix_ms"]       = static_cast<double>(status.lastMotionUnixMs);
    root["analysis_width"]            = status.analysisWidth;
    root["analysis_height"]           = status.analysisHeight;
    root["active_mask_pixels"]        = status.activeMaskPixels;
    root["motion_pixels"]             = status.motionPixels;
    root["largest_blob_pixels"]       = status.largestBlobPixels;
    root["motion_ratio"]              = status.motionRatio;
    root["consecutive_motion_frames"] = status.consecutiveMotionFrames;
    root["consecutive_quiet_frames"]  = status.consecutiveQuietFrames;
    root["last_error"]                = status.lastError;

    if (status.bboxValid && status.analysisWidth > 0 && status.analysisHeight > 0) {
        mini_json::Value bbox = mini_json::Value::array();
        bbox.pushBack(static_cast<double>(status.bboxX) / status.analysisWidth);
        bbox.pushBack(static_cast<double>(status.bboxY) / status.analysisHeight);
        bbox.pushBack(static_cast<double>(status.bboxW) / status.analysisWidth);
        bbox.pushBack(static_cast<double>(status.bboxH) / status.analysisHeight);
        root["bbox"] = bbox;
    } else {
        root["bbox"] = mini_json::Value();
    }
    return root;
}

}  // namespace

std::string MotionDetector::statusJson() const {
    const MotionStatus status = getStatus();
    return motionStatusToJson(status, nowUnixMs()).dump(2);
}

namespace {

mini_json::Value motionEventToJson(const MotionEvent& event) {
    mini_json::Value root     = mini_json::Value::object();
    root["event_id"]          = static_cast<double>(event.eventId);
    root["type"]              = event.type;
    root["unix_ms"]           = static_cast<double>(event.unixMs);
    root["capture_unix_ms"]   = static_cast<double>(event.captureUnixMs);
    root["frame_id"]          = std::to_string(event.frameId);
    root["motion_pixels"]     = event.motionPixels;
    root["largest_blob_pixels"] = event.largestBlobPixels;
    root["motion_ratio"]      = event.motionRatio;
    root["analysis_width"]    = event.analysisWidth;
    root["analysis_height"]   = event.analysisHeight;

    if (event.bboxValid && event.analysisWidth > 0 && event.analysisHeight > 0) {
        mini_json::Value bbox = mini_json::Value::array();
        bbox.pushBack(static_cast<double>(event.bboxX) / event.analysisWidth);
        bbox.pushBack(static_cast<double>(event.bboxY) / event.analysisHeight);
        bbox.pushBack(static_cast<double>(event.bboxW) / event.analysisWidth);
        bbox.pushBack(static_cast<double>(event.bboxH) / event.analysisHeight);
        root["bbox"] = bbox;

        mini_json::Value pixels = mini_json::Value::array();
        pixels.pushBack(event.bboxX);
        pixels.pushBack(event.bboxY);
        pixels.pushBack(event.bboxW);
        pixels.pushBack(event.bboxH);
        root["bbox_pixels"] = pixels;
    } else {
        root["bbox"] = mini_json::Value();
    }
    return root;
}

}  // namespace

std::string MotionDetector::eventsJson(std::uint64_t afterEventId, std::size_t limit) const {
    const std::vector<MotionEvent> events = recentEvents(afterEventId, limit);

    mini_json::Value root  = mini_json::Value::object();
    mini_json::Value array = mini_json::Value::array();
    std::uint64_t newest   = afterEventId;
    for (const MotionEvent& event : events) {
        array.pushBack(motionEventToJson(event));
        newest = std::max(newest, event.eventId);
    }
    root["events"]           = array;
    root["latest_event_id"]  = static_cast<double>(newest);
    root["response_unix_ms"] = static_cast<double>(nowUnixMs());
    return root.dump(2);
}

std::string MotionDetector::recentFramesJson(std::uint64_t afterFrameId, std::size_t limit) const {
    std::lock_guard<std::mutex> lock(mutex_);

    mini_json::Value root             = mini_json::Value::object();
    mini_json::Value frames           = mini_json::Value::array();
    const std::int64_t responseUnixMs = nowUnixMs();

    std::size_t count = 0;
    for (const MotionStatus& item : history_) {
        if (item.frameId <= afterFrameId) {
            continue;
        }
        frames.pushBack(motionStatusToJson(item, responseUnixMs));
        if (++count >= limit) {
            break;
        }
    }

    root["frames"]           = frames;
    root["latest_frame_id"]  = std::to_string(status_.frameId);
    root["response_unix_ms"] = static_cast<double>(responseUnixMs);
    return root.dump(2);
}
