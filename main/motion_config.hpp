#pragma once

#include "mini_json.hpp"

#include <cstdint>
#include <string>
#include <vector>

struct NormalizedPoint {
    double x = 0.0;
    double y = 0.0;
};

enum class ZoneType {
    Include,
    Exclude,
};

struct MotionZone {
    std::string id;
    std::string name;
    ZoneType type = ZoneType::Include;
    std::vector<NormalizedPoint> points;
};

// Optional push channel. When enabled, motion_start / motion_end events are
// POSTed as JSON to the configured URL (for example a Home Assistant webhook).
struct NotifyConfig {
    bool enabled = false;
    std::string url;
    std::string deviceName = "reCamera";
    std::vector<std::string> events;  // empty = every supported event
    bool includeImage     = true;
    int imageMaxBytes     = 512 * 1024;
    int timeoutMs         = 3000;
    int retries           = 2;
};

struct MotionConfig {
    int version = 1;
    bool enabled = true;

    int pixelThreshold = 20;
    int minMotionPixels = 120;
    int minBlobPixels = 40;
    int triggerFrames = 3;
    int clearFrames = 5;
    int warmupFrames = 20;

    double backgroundAlpha = 0.04;
    double foregroundAlpha = 0.002;

    std::vector<MotionZone> zones;

    NotifyConfig notify;
};

MotionConfig defaultMotionConfig();
MotionConfig parseMotionConfig(const std::string& jsonText);
std::string motionConfigToJson(const MotionConfig& config, int indent = 2);

bool loadMotionConfigFile(const std::string& path, MotionConfig& config, std::string& error);
// Parses only the notify block; used to keep push settings when a client posts
// a zone-only configuration.
NotifyConfig parseNotifyConfig(const mini_json::Value& root, const NotifyConfig& fallback);
bool notifyConfigHasBlock(const std::string& jsonText);
bool saveMotionConfigFile(const std::string& path, const MotionConfig& config, std::string& error);
