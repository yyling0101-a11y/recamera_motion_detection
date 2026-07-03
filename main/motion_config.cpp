#include "motion_config.hpp"

#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <sstream>
#include <stdexcept>

namespace {

using mini_json::Value;

int readInt(const Value& root, const char* key, int fallback) {
    return root.contains(key) ? root.at(key).asInt() : fallback;
}

double readDouble(const Value& root, const char* key, double fallback) {
    return root.contains(key) ? root.at(key).asNumber() : fallback;
}

bool readBool(const Value& root, const char* key, bool fallback) {
    return root.contains(key) ? root.at(key).asBool() : fallback;
}

std::string readString(const Value& root, const char* key, const std::string& fallback) {
    return root.contains(key) ? root.at(key).asString() : fallback;
}

void validateConfig(const MotionConfig& c) {
    if (c.version != 1) throw std::runtime_error("Only config version 1 is supported");
    if (c.pixelThreshold < 1 || c.pixelThreshold > 255) throw std::runtime_error("pixel_threshold must be 1..255");
    if (c.minMotionPixels < 1 || c.minMotionPixels > 1000000) throw std::runtime_error("min_motion_pixels is out of range");
    if (c.minBlobPixels < 1 || c.minBlobPixels > 1000000) throw std::runtime_error("min_blob_pixels is out of range");
    if (c.triggerFrames < 1 || c.triggerFrames > 1000) throw std::runtime_error("trigger_frames must be 1..1000");
    if (c.clearFrames < 1 || c.clearFrames > 1000) throw std::runtime_error("clear_frames must be 1..1000");
    if (c.warmupFrames < 0 || c.warmupFrames > 10000) throw std::runtime_error("warmup_frames must be 0..10000");
    if (!(c.backgroundAlpha > 0.0 && c.backgroundAlpha <= 1.0)) throw std::runtime_error("background_alpha must be in (0, 1]");
    if (!(c.foregroundAlpha >= 0.0 && c.foregroundAlpha <= 1.0)) throw std::runtime_error("foreground_alpha must be in [0, 1]");

    for (const MotionZone& zone : c.zones) {
        if (zone.id.empty()) throw std::runtime_error("Every zone requires a non-empty id");
        if (zone.points.size() < 3) throw std::runtime_error("Zone " + zone.id + " requires at least 3 points");
        if (zone.points.size() > 128) throw std::runtime_error("Zone " + zone.id + " has too many points (max 128)");
        for (const NormalizedPoint& p : zone.points) {
            if (p.x < 0.0 || p.x > 1.0 || p.y < 0.0 || p.y > 1.0) {
                throw std::runtime_error("Zone " + zone.id + " contains a point outside [0, 1]");
            }
        }
    }
}

}  // namespace

MotionConfig defaultMotionConfig() {
    MotionConfig config;
    config.enabled = true;
    return config;
}

MotionConfig parseMotionConfig(const std::string& jsonText) {
    const Value root = mini_json::parse(jsonText);
    if (!root.isObject()) throw std::runtime_error("Motion config root must be a JSON object");

    MotionConfig config = defaultMotionConfig();
    config.version = readInt(root, "version", config.version);
    config.enabled = readBool(root, "enabled", config.enabled);
    config.pixelThreshold = readInt(root, "pixel_threshold", config.pixelThreshold);
    config.minMotionPixels = readInt(root, "min_motion_pixels", config.minMotionPixels);
    config.minBlobPixels = readInt(root, "min_blob_pixels", config.minBlobPixels);
    config.triggerFrames = readInt(root, "trigger_frames", config.triggerFrames);
    config.clearFrames = readInt(root, "clear_frames", config.clearFrames);
    config.warmupFrames = readInt(root, "warmup_frames", config.warmupFrames);
    config.backgroundAlpha = readDouble(root, "background_alpha", config.backgroundAlpha);
    config.foregroundAlpha = readDouble(root, "foreground_alpha", config.foregroundAlpha);

    if (root.contains("zones")) {
        const Value& zonesValue = root.at("zones");
        if (!zonesValue.isArray()) throw std::runtime_error("zones must be an array");
        for (const Value& zoneValue : zonesValue.asArray()) {
            if (!zoneValue.isObject()) throw std::runtime_error("Each zone must be an object");
            MotionZone zone;
            zone.id = readString(zoneValue, "id", "");
            zone.name = readString(zoneValue, "name", zone.id);
            const std::string type = readString(zoneValue, "type", "include");
            if (type == "include") {
                zone.type = ZoneType::Include;
            } else if (type == "exclude" || type == "whitelist" || type == "ignore") {
                zone.type = ZoneType::Exclude;
            } else {
                throw std::runtime_error("Zone " + zone.id + " has unsupported type: " + type);
            }

            if (!zoneValue.contains("points") || !zoneValue.at("points").isArray()) {
                throw std::runtime_error("Zone " + zone.id + " requires a points array");
            }
            for (const Value& pointValue : zoneValue.at("points").asArray()) {
                if (!pointValue.isArray() || pointValue.asArray().size() != 2) {
                    throw std::runtime_error("Every point must be [x, y]");
                }
                NormalizedPoint p;
                p.x = pointValue[0].asNumber();
                p.y = pointValue[1].asNumber();
                zone.points.push_back(p);
            }
            config.zones.push_back(zone);
        }
    }

    validateConfig(config);
    return config;
}

std::string motionConfigToJson(const MotionConfig& config, int indent) {
    Value root = Value::object();
    root["version"] = config.version;
    root["enabled"] = config.enabled;
    root["pixel_threshold"] = config.pixelThreshold;
    root["min_motion_pixels"] = config.minMotionPixels;
    root["min_blob_pixels"] = config.minBlobPixels;
    root["trigger_frames"] = config.triggerFrames;
    root["clear_frames"] = config.clearFrames;
    root["warmup_frames"] = config.warmupFrames;
    root["background_alpha"] = config.backgroundAlpha;
    root["foreground_alpha"] = config.foregroundAlpha;

    Value zones = Value::array();
    for (const MotionZone& zone : config.zones) {
        Value item = Value::object();
        item["id"] = zone.id;
        item["name"] = zone.name;
        item["type"] = zone.type == ZoneType::Include ? "include" : "exclude";
        Value points = Value::array();
        for (const NormalizedPoint& p : zone.points) {
            Value pair = Value::array();
            pair.pushBack(p.x);
            pair.pushBack(p.y);
            points.pushBack(pair);
        }
        item["points"] = points;
        zones.pushBack(item);
    }
    root["zones"] = zones;
    return root.dump(indent);
}

bool loadMotionConfigFile(const std::string& path, MotionConfig& config, std::string& error) {
    std::ifstream input(path.c_str(), std::ios::binary);
    if (!input) {
        error = std::string("Cannot open config file: ") + std::strerror(errno);
        return false;
    }
    std::ostringstream buffer;
    buffer << input.rdbuf();
    try {
        config = parseMotionConfig(buffer.str());
        return true;
    } catch (const std::exception& e) {
        error = e.what();
        return false;
    }
}

bool saveMotionConfigFile(const std::string& path, const MotionConfig& config, std::string& error) {
    const std::string temporary = path + ".tmp";
    {
        std::ofstream output(temporary.c_str(), std::ios::binary | std::ios::trunc);
        if (!output) {
            error = std::string("Cannot write temporary config: ") + std::strerror(errno);
            return false;
        }
        output << motionConfigToJson(config, 2) << '\n';
        output.flush();
        if (!output) {
            error = "Failed while writing temporary config";
            return false;
        }
    }
    if (std::rename(temporary.c_str(), path.c_str()) != 0) {
        error = std::string("Cannot replace config file: ") + std::strerror(errno);
        std::remove(temporary.c_str());
        return false;
    }
    return true;
}
