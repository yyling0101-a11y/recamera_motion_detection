#pragma once

#include "motion_config.hpp"
#include "motion_detector.hpp"
#include "snapshot_store.hpp"

#include <atomic>

namespace video_stack {
constexpr int kMotionWidth = 320;
constexpr int kMotionHeight = 180;
constexpr int kMotionFps = 10;

extern std::atomic<bool> gRtspStarted;
extern RtspConfig gRtspRuntimeConfig;
extern int gSnapshotWidth;
extern int gSnapshotHeight;
extern int gSnapshotFps;
extern SnapshotStore gSnapshots;
extern std::atomic<bool> gSnapshotChannelActive;

void stopVideoStack();
void signalHandler(int signalNumber);
int startVideoStack(MotionDetector& detector, const RtspConfig& rtspConfig);
}  // namespace video_stack
