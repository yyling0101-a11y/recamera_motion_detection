#pragma once

#include "motion_config.hpp"

#include <chrono>
#include <cstdint>
#include <deque>
#include <mutex>
#include <string>
#include <vector>


struct MotionStatus {
    bool enabled    = false;
    bool configured = false;
    bool warmingUp  = true;
    bool motion     = false;

    std::uint64_t frameCount      = 0;
    std::uint64_t eventId         = 0;
    std::int64_t lastChangeUnixMs = 0;
    std::int64_t lastMotionUnixMs = 0;

    int analysisWidth     = 0;
    int analysisHeight    = 0;
    int activeMaskPixels  = 0;
    int motionPixels      = 0;
    int largestBlobPixels = 0;
    double motionRatio    = 0.0;

    bool bboxValid = false;
    int bboxX      = 0;
    int bboxY      = 0;
    int bboxW      = 0;
    int bboxH      = 0;

    int consecutiveMotionFrames = 0;
    int consecutiveQuietFrames  = 0;
    std::string lastError;

    std::uint64_t frameId      = 0;
    std::uint64_t sourcePts    = 0;
    std::int64_t captureUnixMs = 0;
};

class MotionDetector {
public:
    MotionDetector(int width, int height);

    void setConfig(const MotionConfig& config);
    MotionConfig getConfig() const;

    void resetBackground();
    void setError(const std::string& error);

    void processGrayFrame(const std::uint8_t* yPlane, int sourceWidth, int sourceHeight, int sourceStride, std::uint64_t frameId, std::uint64_t sourcePts, std::int64_t captureUnixMs);

    MotionStatus getStatus() const;
    std::string statusJson() const;
    std::string recentFramesJson(std::uint64_t afterFrameId, std::size_t limit = 100) const;

private:
    bool pointInPolygon(double x, double y, const std::vector<NormalizedPoint>& polygon) const;
    void rebuildMaskLocked();
    void resetBackgroundLocked();
    void commitHistoryLocked();
    std::int64_t nowUnixMs() const;

    const int width_;
    const int height_;

    mutable std::mutex mutex_;
    MotionConfig config_;
    MotionStatus status_;

    std::vector<std::uint8_t> activeMask_;
    std::vector<std::uint16_t> backgroundQ8_;
    std::vector<std::uint8_t> rawForeground_;
    std::vector<std::uint8_t> filteredForeground_;
    std::vector<std::uint8_t> visited_;
    std::vector<int> queue_;

    bool backgroundReady_ = false;
    int framesSinceReset_ = 0;

    std::deque<MotionStatus> history_;
    static constexpr std::size_t kMaxHistoryFrames = 180;
};
