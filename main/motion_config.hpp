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
};

MotionConfig defaultMotionConfig();
MotionConfig parseMotionConfig(const std::string& jsonText);
std::string motionConfigToJson(const MotionConfig& config, int indent = 2);

bool loadMotionConfigFile(const std::string& path, MotionConfig& config, std::string& error);
bool saveMotionConfigFile(const std::string& path, const MotionConfig& config, std::string& error);
