#pragma once
#include "http_server.hpp"
#include "motion_detector.hpp"
#include "notifier.hpp"
#include "snapshot_store.hpp"
#include <atomic>
#include <string>
struct AppApi {
    MotionDetector& detector;
    WebhookNotifier& notifier;
    SnapshotStore& gSnapshots;
    std::atomic<bool>& gSnapshotChannelActive;
    std::atomic<bool>& gRtspStarted;
    RtspConfig gRtspRuntimeConfig;
    int gSnapshotWidth;
    int gSnapshotHeight;
    int gSnapshotFps;
    std::string configPath;
    std::string ip;
    std::string rtspUrl;
    std::string apiBase;
    int httpPort;
    HttpResponse handle(const HttpRequest& request);
    std::string configJsonForApi(const MotionConfig& config) const;
};
