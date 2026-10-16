#include "app_api.hpp"
#include "event_payload.hpp"
#include "mini_json.hpp"
#include "video_stack.hpp"

#include <cstdlib>
#include <stdexcept>

namespace {

std::string jsonMessage(bool ok, const std::string& message) {
    mini_json::Value root = mini_json::Value::object();
    root["ok"]            = ok;
    if (ok) {
        root["message"] = message;
    } else {
        root["error"] = message;
    }
    return root.dump(2);
}

}  // namespace

std::string AppApi::configJsonForApi(const MotionConfig& config) const {
    mini_json::Value root = mini_json::parse(motionConfigToJson(config, 2));
    mini_json::Value& rtsp = root["rtsp"];
    const bool restartRequired = config.rtsp.enabled != gRtspRuntimeConfig.enabled ||
                                 config.rtsp.port != gRtspRuntimeConfig.port ||
                                 config.rtsp.path != gRtspRuntimeConfig.path;
    rtsp["active"] = gRtspStarted.load();
    rtsp["active_port"] = gRtspStarted.load() ? gRtspRuntimeConfig.port : 0;
    rtsp["active_path"] = gRtspStarted.load() ? gRtspRuntimeConfig.path : "";
    rtsp["restart_required"] = restartRequired;
    return root.dump(2);
}

HttpResponse AppApi::handle(const HttpRequest& request) {
    HttpResponse response;

    if (request.method == "GET" && request.path == "/") {
        response.contentType = "text/plain; charset=utf-8";
        response.body =
            "reCamera Motion Detection\n"
            "GET  /api/health\n"
            "GET  /api/version\n"
            "GET  /api/status\n"
            "GET  /api/config\n"
            "POST /api/config\n"
            "POST /api/background/reset\n"
            "GET  /api/snapshot.jpg[?event_id=N|?at=UNIX_MS]\n"
            "GET  /api/events[?after_id=N&limit=N]\n"
            "GET  /api/frames[?after_frame_id=N&limit=N]\n"
            "GET  /api/notify\n"
            "POST /api/notify\n"
            "POST /api/notify/test\n";
        return response;
    }

    if (request.method == "GET" && request.path == "/api/health") {
        mini_json::Value root   = mini_json::Value::object();
        root["ok"]              = true;
        root["device_ip"]       = ip;
        root["rtsp_url"]        = rtspUrl;
        root["api_base"]        = apiBase;
        root["analysis_width"]  = video_stack::kMotionWidth;
        root["analysis_height"] = video_stack::kMotionHeight;
        root["analysis_fps"]    = video_stack::kMotionFps;

        mini_json::Value rtsp = mini_json::Value::object();
        rtsp["enabled"] = gRtspStarted.load();
        rtsp["configured_enabled"] = detector.getConfig().rtsp.enabled;
        rtsp["port"] = gRtspStarted.load() ? gRtspRuntimeConfig.port : 0;
        rtsp["path"] = gRtspStarted.load() ? gRtspRuntimeConfig.path : "";
        rtsp["url"] = rtspUrl;
        rtsp["restart_required"] = detector.getConfig().rtsp.enabled != gRtspRuntimeConfig.enabled ||
                                    detector.getConfig().rtsp.port != gRtspRuntimeConfig.port ||
                                    detector.getConfig().rtsp.path != gRtspRuntimeConfig.path;
        root["rtsp"] = rtsp;
        mini_json::Value mqtt = mini_json::Value::object();
        mqtt["enabled"] = false;
        mqtt["supported"] = false;
        mqtt["reason"] = "MQTT client is not included in this build";
        root["mqtt"] = mqtt;

        mini_json::Value snapshot      = mini_json::Value::object();
        snapshot["enabled"]            = gSnapshotChannelActive.load();
        snapshot["width"]              = gSnapshotWidth;
        snapshot["height"]             = gSnapshotHeight;
        snapshot["fps"]                = gSnapshotFps;
        snapshot["buffered_frames"]    = static_cast<double>(gSnapshots.size());
        snapshot["published_frames"]   = static_cast<double>(gSnapshots.publishedCount());
        snapshot["url"]                = apiBase + "/api/snapshot.jpg";
        root["snapshot"]               = snapshot;

        const NotifyConfig notify = notifier.config();
        mini_json::Value notifyJson  = mini_json::Value::object();
        notifyJson["enabled"]        = notify.enabled;
        notifyJson["url_configured"] = !notify.url.empty();
        root["notify"]               = notifyJson;
        response.body                = root.dump(2);
        return response;
    }

    if (request.method == "GET" && request.path == "/api/version") {
        mini_json::Value root = mini_json::Value::object();
        root["ok"]            = true;
        root["program"]       = "recamera_motion_detection";
        root["version"]       = kProgramVersion;
        root["api"]           = kApiVersion;
        root["device_ip"]     = ip;
        root["rtsp_url"]      = rtspUrl;
        response.body         = root.dump(2);
        return response;
    }

    if (request.method == "GET" && request.path == "/api/status") {
        response.body = detector.statusJson();
        return response;
    }

    if (request.method == "GET" && request.path == "/api/config") {
        response.body = configJsonForApi(detector.getConfig());
        return response;
    }

    if (request.method == "POST" && request.path == "/api/config") {
        try {
            MotionConfig newConfig = parseMotionConfig(request.body);
            // A zone-only upload (for example from the legacy Windows UI)
            // must not silently disable event pushing.
            if (!notifyConfigHasBlock(request.body)) {
                newConfig.notify = detector.getConfig().notify;
            }
            // Older clients post only motion fields. Preserve output
            // configuration just like notify, instead of resetting RTSP.
            if (!rtspConfigHasBlock(request.body)) {
                newConfig.rtsp = detector.getConfig().rtsp;
            }
            std::string saveError;
            if (!saveMotionConfigFile(configPath, newConfig, saveError)) {
                response.status = 500;
                response.body   = jsonMessage(false, saveError);
                return response;
            }
            detector.setConfig(newConfig);
            notifier.configure(newConfig.notify);
            response.body = configJsonForApi(newConfig);
            return response;
        } catch (const std::exception& e) {
            response.status = 400;
            response.body   = jsonMessage(false, e.what());
            return response;
        }
    }

    if (request.method == "POST" && request.path == "/api/background/reset") {
        detector.resetBackground();
        response.body = jsonMessage(true, "background reset");
        return response;
    }

    if (request.method == "GET" && request.path == "/api/snapshot.jpg") {
        if (!gSnapshotChannelActive.load()) {
            response.status = 503;
            response.body   = jsonMessage(false, "snapshot channel disabled (set MOTION_SNAPSHOT_FPS>0)");
            return response;
        }

        const std::string eventIdText = httpQueryParam(request.query, "event_id");
        const std::string atText      = httpQueryParam(request.query, "at");

        JpegSnapshot snapshot;
        bool found = false;
        if (!eventIdText.empty()) {
            const std::uint64_t eventId = std::strtoull(eventIdText.c_str(), nullptr, 10);
            for (const MotionEvent& event : detector.recentEvents(0, 0)) {
                if (event.eventId == eventId) {
                    found = gSnapshots.atOrBefore(event.captureUnixMs, snapshot);
                    break;
                }
            }
            if (!found) {
                response.status = 404;
                response.body   = jsonMessage(false, "no buffered snapshot for event_id " + eventIdText);
                return response;
            }
        } else if (!atText.empty()) {
            found = gSnapshots.atOrBefore(static_cast<std::int64_t>(std::strtoll(atText.c_str(), nullptr, 10)), snapshot);
        } else {
            found = gSnapshots.latest(snapshot);
        }

        if (!found || snapshot.data.empty()) {
            response.status = 404;
            response.body   = jsonMessage(false, "no snapshot available yet");
            return response;
        }

        response.contentType = "image/jpeg";
        response.body.assign(reinterpret_cast<const char*>(snapshot.data.data()), snapshot.data.size());
        return response;
    }

    if (request.method == "GET" && request.path == "/api/events") {
        const std::string afterText = httpQueryParam(request.query, "after_id");
        const std::string limitText = httpQueryParam(request.query, "limit");
        const std::uint64_t afterId = afterText.empty() ? 0 : std::strtoull(afterText.c_str(), nullptr, 10);
        const std::size_t limit     = limitText.empty() ? 50 : static_cast<std::size_t>(std::strtoul(limitText.c_str(), nullptr, 10));
        response.body               = detector.eventsJson(afterId, limit);
        return response;
    }

    if (request.method == "GET" && request.path == "/api/frames") {
        const std::string afterText = httpQueryParam(request.query, "after_frame_id");
        const std::string limitText = httpQueryParam(request.query, "limit");
        const std::uint64_t afterId = afterText.empty() ? 0 : std::strtoull(afterText.c_str(), nullptr, 10);
        const std::size_t limit     = limitText.empty() ? 100 : static_cast<std::size_t>(std::strtoul(limitText.c_str(), nullptr, 10));
        response.body               = detector.recentFramesJson(afterId, limit);
        return response;
    }

    if (request.path == "/api/notify" || request.path == "/api/notify/test") {
        NotifyConfig notify = notifier.config();

        if (request.method == "POST" && request.path == "/api/notify") {
            try {
                const mini_json::Value root = mini_json::parse(request.body);
                if (!root.isObject())
                    throw std::runtime_error("notify payload must be a JSON object");
                notify = parseNotifyConfig(root, notify);

                MotionConfig stored = detector.getConfig();
                stored.notify       = notify;
                std::string saveError;
                if (!saveMotionConfigFile(configPath, stored, saveError)) {
                    response.status = 500;
                    response.body   = jsonMessage(false, saveError);
                    return response;
                }
                // Update the push target without rebuilding the background
                // model, so an already warmed detector keeps running.
                detector.setNotify(notify);
                notifier.configure(notify);
            } catch (const std::exception& e) {
                response.status = 400;
                response.body   = jsonMessage(false, e.what());
                return response;
            }
        } else if (request.method != "GET" && request.method != "POST") {
            response.status = 405;
            response.body   = jsonMessage(false, "use GET or POST");
            return response;
        }

        if (request.path == "/api/notify/test") {
            if (!notify.enabled || notify.url.empty()) {
                response.status = 400;
                response.body   = jsonMessage(false, "notify is not configured");
                return response;
            }
            MotionEvent event;
            detector.lastEvent(event);
            event.type            = "test";
            event.unixMs          = unixMsNow();
            event.captureUnixMs   = event.unixMs;
            mini_json::Value payload = eventPayload(event, ip, httpPort, notify);
            JpegSnapshot snapshot;
            if (gSnapshots.latest(snapshot) && static_cast<int>(snapshot.data.size()) <= notify.imageMaxBytes) {
                mini_json::Value image   = mini_json::Value::object();
                image["encoding"]        = "base64";
                image["content_type"]    = "image/jpeg";
                image["width"]           = snapshot.width;
                image["height"]          = snapshot.height;
                image["capture_unix_ms"] = static_cast<double>(snapshot.captureUnixMs);
                image["bytes"]           = static_cast<double>(snapshot.data.size());
                image["data"]            = base64Encode(snapshot.data.data(), snapshot.data.size());
                payload["image"]         = image;
            }
            const bool queued = notifier.enqueue(payload.dump(), "test");
            response.status   = queued ? 200 : 503;
            response.body     = jsonMessage(queued, queued ? "test notification queued" : "notification queue unavailable");
            return response;
        }

        const NotifierStats stats = notifier.stats();
        mini_json::Value root     = mini_json::Value::object();
        root["enabled"]           = notify.enabled;
        root["url"]               = notify.url;
        root["device_name"]       = notify.deviceName;
        mini_json::Value events   = mini_json::Value::array();
        for (const std::string& event : notify.events) {
            events.pushBack(event);
        }
        root["events"]            = events;
        root["include_image"]     = notify.includeImage;
        root["image_max_bytes"]   = notify.imageMaxBytes;
        root["timeout_ms"]        = notify.timeoutMs;
        root["retries"]           = notify.retries;

        mini_json::Value statsJson        = mini_json::Value::object();
        statsJson["queued"]               = static_cast<double>(stats.queued);
        statsJson["sent"]                 = static_cast<double>(stats.sent);
        statsJson["failed"]               = static_cast<double>(stats.failed);
        statsJson["dropped"]              = static_cast<double>(stats.dropped);
        statsJson["last_attempt_unix_ms"] = static_cast<double>(stats.lastAttemptUnixMs);
        statsJson["last_success_unix_ms"] = static_cast<double>(stats.lastSuccessUnixMs);
        statsJson["last_http_status"]     = stats.lastHttpStatus;
        statsJson["last_error"]           = stats.lastError;
        root["stats"]                     = statsJson;
        response.body                     = root.dump(2);
        return response;
    }

    response.status = 404;
    response.body   = jsonMessage(false, "endpoint not found");
    return response;
}
