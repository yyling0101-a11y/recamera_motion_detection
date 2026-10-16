#include "app_api.hpp"
#include "event_payload.hpp"
#include "http_server.hpp"
#include "mini_json.hpp"
#include "motion_config.hpp"
#include "motion_detector.hpp"
#include "notifier.hpp"
#include "snapshot_store.hpp"
#include "video_stack.hpp"

#include <arpa/inet.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ifaddrs.h>
#include <signal.h>
#include <string>
#include <unistd.h>

namespace {

int environmentInt(const char* name, int fallback) {
    const char* value = std::getenv(name);
    if (value == nullptr || value[0] == '\0')
        return fallback;
    const int parsed = std::atoi(value);
    return parsed > 0 ? parsed : fallback;
}

std::string getLocalIp() {
    const char* forced = std::getenv("MOTION_DEVICE_IP");
    if (forced != nullptr && forced[0] != '\0')
        return forced;

    ifaddrs* interfaces = nullptr;
    if (getifaddrs(&interfaces) != 0)
        return "127.0.0.1";

    const char* preferred[] = {"eth0", "wlan0", "usb0", "end0"};
    std::string best;
    std::size_t bestRank = sizeof(preferred) / sizeof(preferred[0]);
    std::string fallback;
    std::string linkLocal;
    for (ifaddrs* current = interfaces; current != nullptr; current = current->ifa_next) {
        if (current->ifa_addr == nullptr || current->ifa_addr->sa_family != AF_INET)
            continue;
        const sockaddr_in* address = reinterpret_cast<const sockaddr_in*>(current->ifa_addr);
        char ip[INET_ADDRSTRLEN]   = {0};
        if (inet_ntop(AF_INET, &address->sin_addr, ip, sizeof(ip)) == nullptr)
            continue;
        if (std::strcmp(ip, "127.0.0.1") == 0)
            continue;
        // A disconnected Ethernet port may receive a 169.254/16 address.
        // Prefer a routable USB or Wi-Fi address in that case.
        if (std::strncmp(ip, "169.254.", 8) == 0) {
            if (linkLocal.empty())
                linkLocal = ip;
            continue;
        }
        if (fallback.empty())
            fallback = ip;
        for (std::size_t rank = 0; rank < sizeof(preferred) / sizeof(preferred[0]); ++rank) {
            if (rank < bestRank && std::strcmp(current->ifa_name, preferred[rank]) == 0) {
                best = ip;
                bestRank = rank;
            }
        }
    }
    freeifaddrs(interfaces);
    if (!best.empty()) return best;
    if (!fallback.empty()) return fallback;
    return linkLocal.empty() ? "127.0.0.1" : linkLocal;
}

}  // namespace

int main() {
    signal(SIGINT, video_stack::signalHandler);
    signal(SIGTERM, video_stack::signalHandler);

    const char* configEnvironment = std::getenv("MOTION_CONFIG_PATH");
    const std::string configPath  = configEnvironment != nullptr && configEnvironment[0] != '\0' ? configEnvironment : "/userdata/motion_roi.json";

    video_stack::gSnapshotWidth  = environmentInt("MOTION_SNAPSHOT_WIDTH", video_stack::gSnapshotWidth);
    video_stack::gSnapshotHeight = environmentInt("MOTION_SNAPSHOT_HEIGHT", video_stack::gSnapshotHeight);
    video_stack::gSnapshotFps    = environmentInt("MOTION_SNAPSHOT_FPS", video_stack::gSnapshotFps);
    const char* snapshotFpsEnvironment = std::getenv("MOTION_SNAPSHOT_FPS");
    if (snapshotFpsEnvironment != nullptr && std::atoi(snapshotFpsEnvironment) == 0)
        video_stack::gSnapshotFps = 0;  // explicitly disabled

    int httpPort                = 8080;
    const char* portEnvironment = std::getenv("MOTION_HTTP_PORT");
    if (portEnvironment != nullptr && portEnvironment[0] != '\0') {
        httpPort = std::atoi(portEnvironment);
        if (httpPort <= 0 || httpPort > 65535)
            httpPort = 8080;
    }

    MotionConfig config = defaultMotionConfig();
    std::string configError;
    if (loadMotionConfigFile(configPath, config, configError)) {
        std::printf("[config] loaded %s\n", configPath.c_str());
    } else {
        std::fprintf(stderr, "[config] using defaults: %s\n", configError.c_str());
    }

    MotionDetector detector(video_stack::kMotionWidth, video_stack::kMotionHeight);
    detector.setConfig(config);

    WebhookNotifier notifier;
    notifier.configure(config.notify);

    video_stack::gRtspRuntimeConfig = config.rtsp;
    const int videoResult = video_stack::startVideoStack(detector, video_stack::gRtspRuntimeConfig);
    if (videoResult != 0)
        return EXIT_FAILURE;

    const std::string ip = getLocalIp();
    const std::string rtspUrl = video_stack::gRtspStarted.load()
                                    ? "rtsp://" + ip + ":" + std::to_string(video_stack::gRtspRuntimeConfig.port) + video_stack::gRtspRuntimeConfig.path
                                    : std::string();
    const std::string apiBase     = "http://" + ip + ":" + std::to_string(httpPort);

    // Motion transitions are pushed to the configured webhook from the
    // notifier thread; the video callback only builds the payload.
    detector.setEventCallback([&](const MotionEvent& event) {
        const NotifyConfig notify = notifier.config();
        if (!notify.enabled || notify.url.empty() || !eventSelected(notify, event.type))
            return;

        mini_json::Value payload = eventPayload(event, ip, httpPort, notify);

        JpegSnapshot snapshot;
        if (notify.includeImage && video_stack::gSnapshots.atOrBefore(event.captureUnixMs, snapshot) &&
            !snapshot.data.empty() && static_cast<int>(snapshot.data.size()) <= notify.imageMaxBytes) {
            mini_json::Value image      = mini_json::Value::object();
            image["encoding"]           = "base64";
            image["content_type"]       = "image/jpeg";
            image["width"]              = snapshot.width;
            image["height"]             = snapshot.height;
            image["capture_unix_ms"]    = static_cast<double>(snapshot.captureUnixMs);
            image["bytes"]              = static_cast<double>(snapshot.data.size());
            image["data"]               = base64Encode(snapshot.data.data(), snapshot.data.size());
            payload["image"]            = image;
        }

        notifier.enqueue(payload.dump(), event.type + " event=" + std::to_string(event.eventId));
    });

    std::printf("\nreCamera motion detector is running\n");
    std::printf("RTSP: %s\n", rtspUrl.empty() ? "disabled" : rtspUrl.c_str());
    std::printf("API : %s/api/status\n", apiBase.c_str());
    std::printf("Config file: %s\n", configPath.c_str());
    if (config.notify.enabled && !config.notify.url.empty()) {
        std::printf("Notify: %s\n", config.notify.url.c_str());
    } else {
        std::printf("Notify: disabled (POST /api/config with a notify block to enable)\n");
    }
    std::printf("\n");
    std::fflush(stdout);

    HttpServer server(httpPort);
    AppApi api{detector, notifier, video_stack::gSnapshots, video_stack::gSnapshotChannelActive, video_stack::gRtspStarted,
               video_stack::gRtspRuntimeConfig, video_stack::gSnapshotWidth, video_stack::gSnapshotHeight, video_stack::gSnapshotFps,
               configPath, ip, rtspUrl, apiBase, httpPort};
    server.setHandler([&api](const HttpRequest& request) {
        return api.handle(request);
    });

    server.start();
    video_stack::stopVideoStack();
    return EXIT_SUCCESS;
}
