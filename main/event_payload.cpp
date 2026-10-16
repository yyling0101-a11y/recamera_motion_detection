#include "event_payload.hpp"
#include <chrono>

std::int64_t unixMsNow() {
    using namespace std::chrono;
    return duration_cast<milliseconds>(system_clock::now().time_since_epoch()).count();
}
std::string base64Encode(const std::uint8_t* data, std::size_t size) {
    static const char* kAlphabet = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    out.reserve(((size + 2) / 3) * 4);
    std::size_t index = 0;
    while (index + 2 < size) {
        const std::uint32_t triple = (static_cast<std::uint32_t>(data[index]) << 16) |
                                     (static_cast<std::uint32_t>(data[index + 1]) << 8) |
                                     static_cast<std::uint32_t>(data[index + 2]);
        out.push_back(kAlphabet[(triple >> 18) & 0x3F]);
        out.push_back(kAlphabet[(triple >> 12) & 0x3F]);
        out.push_back(kAlphabet[(triple >> 6) & 0x3F]);
        out.push_back(kAlphabet[triple & 0x3F]);
        index += 3;
    }
    const std::size_t remaining = size - index;
    if (remaining == 1) {
        const std::uint32_t triple = static_cast<std::uint32_t>(data[index]) << 16;
        out.push_back(kAlphabet[(triple >> 18) & 0x3F]);
        out.push_back(kAlphabet[(triple >> 12) & 0x3F]);
        out.push_back('=');
        out.push_back('=');
    } else if (remaining == 2) {
        const std::uint32_t triple = (static_cast<std::uint32_t>(data[index]) << 16) |
                                     (static_cast<std::uint32_t>(data[index + 1]) << 8);
        out.push_back(kAlphabet[(triple >> 18) & 0x3F]);
        out.push_back(kAlphabet[(triple >> 12) & 0x3F]);
        out.push_back(kAlphabet[(triple >> 6) & 0x3F]);
        out.push_back('=');
    }
    return out;
}

bool eventSelected(const NotifyConfig& notify, const std::string& type) {
    if (notify.events.empty())
        return true;
    for (const std::string& wanted : notify.events) {
        if (wanted == type || wanted == "*")
            return true;
    }
    return false;
}

static mini_json::Value normalizedBbox(const MotionEvent& event) {
    if (!event.bboxValid || event.analysisWidth <= 0 || event.analysisHeight <= 0)
        return mini_json::Value();
    mini_json::Value bbox = mini_json::Value::array();
    bbox.pushBack(static_cast<double>(event.bboxX) / event.analysisWidth);
    bbox.pushBack(static_cast<double>(event.bboxY) / event.analysisHeight);
    bbox.pushBack(static_cast<double>(event.bboxW) / event.analysisWidth);
    bbox.pushBack(static_cast<double>(event.bboxH) / event.analysisHeight);
    return bbox;
}

// Payload shared by the webhook push and the /api/events history so that both
// consumers see exactly the same schema.
mini_json::Value eventPayload(const MotionEvent& event, const std::string& deviceIp, int httpPort, const NotifyConfig& notify) {
    const std::string apiBase = "http://" + deviceIp + ":" + std::to_string(httpPort);

    mini_json::Value root       = mini_json::Value::object();
    root["source"]              = "recamera_motion_detection";
    root["schema"]              = kApiVersion;
    root["program_version"]     = kProgramVersion;
    root["event"]               = event.type;
    // The app owns notification wording. HA forwards these fields to its
    // notification channels without needing app-specific copy.
    if (event.type == "motion_start") {
        root["title"] = "选区运动告警";
        root["message"] = "检测到选区内运动，运动像素 " + std::to_string(event.motionPixels) + "。";
    } else if (event.type == "test") {
        root["title"] = "选区运动检测测试通知";
        root["message"] = "手机推送链路已连通。";
    }
    root["event_id"]            = static_cast<double>(event.eventId);
    root["device_name"]         = notify.deviceName;
    root["device_ip"]           = deviceIp;
    root["api_base"]            = apiBase;
    root["snapshot_url"]        = apiBase + "/api/snapshot.jpg?event_id=" + std::to_string(event.eventId);
    root["unix_ms"]             = static_cast<double>(event.unixMs);
    root["capture_unix_ms"]     = static_cast<double>(event.captureUnixMs);
    root["frame_id"]            = std::to_string(event.frameId);
    root["motion_pixels"]       = event.motionPixels;
    root["largest_blob_pixels"] = event.largestBlobPixels;
    root["motion_ratio"]        = event.motionRatio;
    root["analysis_width"]      = event.analysisWidth;
    root["analysis_height"]     = event.analysisHeight;
    root["bbox"]                = normalizedBbox(event);
    return root;
}
