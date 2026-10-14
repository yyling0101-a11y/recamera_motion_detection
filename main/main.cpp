#include "http_server.hpp"
#include "mini_json.hpp"
#include "motion_config.hpp"
#include "motion_detector.hpp"
#include "notifier.hpp"
#include "rtsp_demo.h"
#include "snapshot_store.hpp"

#include <arpa/inet.h>
#include <atomic>
#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ifaddrs.h>
#include <signal.h>
#include <string>
#include <unistd.h>
#include <vector>

#include "frame_sync.hpp"

#include <chrono>

extern "C" {
#include "cvi_sys.h"
#include "cvi_venc.h"
#include "video.h"
}

namespace {

constexpr video_ch_index_t kSnapshotChannel = VIDEO_CH0;
constexpr video_ch_index_t kMotionChannel   = VIDEO_CH1;
constexpr video_ch_index_t kRtspChannel     = VIDEO_CH2;
constexpr int kMotionWidth                = 320;
constexpr int kMotionHeight               = 180;
constexpr int kMotionFps                  = 10;
constexpr int kRtspWidth                  = 1920;
constexpr int kRtspHeight                 = 1080;
constexpr int kRtspFps                    = 30;

std::atomic<bool> gVideoStarted(false);
std::atomic<bool> gRtspStarted(false);

constexpr const char* kProgramVersion = "2.0.0";
constexpr int kApiVersion             = 2;

int gSnapshotWidth  = 1280;
int gSnapshotHeight = 720;
int gSnapshotFps    = 10;

SnapshotStore gSnapshots(16);
std::atomic<bool> gSnapshotChannelActive(false);

int environmentInt(const char* name, int fallback) {
    const char* value = std::getenv(name);
    if (value == nullptr || value[0] == '\0')
        return fallback;
    const int parsed = std::atoi(value);
    return parsed > 0 ? parsed : fallback;
}

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

mini_json::Value normalizedBbox(const MotionEvent& event) {
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

int snapshotFrameCallback(void* data, void* args, void* userData) {
    (void)args;
    (void)userData;

    VENC_STREAM_S* stream = static_cast<VENC_STREAM_S*>(data);
    if (stream == nullptr || stream->pstPack == nullptr || stream->u32PackCount == 0)
        return CVI_FAILURE;

    std::vector<std::uint8_t> buffer;
    for (CVI_U32 index = 0; index < stream->u32PackCount; ++index) {
        VENC_PACK_S* pack = &stream->pstPack[index];
        if (pack->pu8Addr == nullptr || pack->u32Offset > pack->u32Len)
            return CVI_FAILURE;
        const CVI_U8* begin = pack->pu8Addr + pack->u32Offset;
        buffer.insert(buffer.end(), begin, begin + (pack->u32Len - pack->u32Offset));
    }

    if (buffer.size() < 4)
        return CVI_SUCCESS;

    static std::atomic<std::uint64_t> snapshotFrameId{0};
    gSnapshots.push(buffer.data(), buffer.size(), ++snapshotFrameId, unixMsNow(), gSnapshotWidth, gSnapshotHeight);
    return CVI_SUCCESS;
}

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

std::string getLocalIp() {
    const char* forced = std::getenv("MOTION_DEVICE_IP");
    if (forced != nullptr && forced[0] != '\0')
        return forced;

    ifaddrs* interfaces = nullptr;
    if (getifaddrs(&interfaces) != 0)
        return "127.0.0.1";

    const char* preferred[] = {"eth0", "wlan0", "usb0", "end0"};
    std::string fallback;
    for (ifaddrs* current = interfaces; current != nullptr; current = current->ifa_next) {
        if (current->ifa_addr == nullptr || current->ifa_addr->sa_family != AF_INET)
            continue;
        const sockaddr_in* address = reinterpret_cast<const sockaddr_in*>(current->ifa_addr);
        char ip[INET_ADDRSTRLEN]   = {0};
        if (inet_ntop(AF_INET, &address->sin_addr, ip, sizeof(ip)) == nullptr)
            continue;
        if (std::strcmp(ip, "127.0.0.1") == 0)
            continue;
        if (fallback.empty())
            fallback = ip;
        for (const char* name : preferred) {
            if (std::strcmp(current->ifa_name, name) == 0) {
                const std::string result = ip;
                freeifaddrs(interfaces);
                return result;
            }
        }
    }
    freeifaddrs(interfaces);
    return fallback.empty() ? "127.0.0.1" : fallback;
}

int setH264Gop(VENC_CHN channel, CVI_U32 newGop) {
    VENC_CHN_ATTR_S attributes;
    std::memset(&attributes, 0, sizeof(attributes));
    CVI_S32 result = CVI_VENC_GetChnAttr(channel, &attributes);
    if (result != CVI_SUCCESS)
        return result;

    switch (attributes.stRcAttr.enRcMode) {
        case VENC_RC_MODE_H264CBR:
            attributes.stRcAttr.stH264Cbr.u32Gop = newGop;
            break;
        case VENC_RC_MODE_H264VBR:
            attributes.stRcAttr.stH264Vbr.u32Gop = newGop;
            break;
        case VENC_RC_MODE_H264AVBR:
            attributes.stRcAttr.stH264AVbr.u32Gop = newGop;
            break;
        case VENC_RC_MODE_H264FIXQP:
            attributes.stRcAttr.stH264FixQp.u32Gop = newGop;
            break;
        default:
            std::fprintf(stderr, "[video] unsupported H264 RC mode=%d; GOP unchanged\n", static_cast<int>(attributes.stRcAttr.enRcMode));
            return CVI_FAILURE;
    }

    result = CVI_VENC_SetChnAttr(channel, &attributes);
    if (result == CVI_SUCCESS)
        CVI_VENC_RequestIDR(channel, CVI_TRUE);
    return result;
}

int motionFrameCallback(void* data, void* args, void* userData) {
    (void)args;

    MotionDetector* detector      = static_cast<MotionDetector*>(userData);
    VIDEO_FRAME_INFO_S* frameInfo = static_cast<VIDEO_FRAME_INFO_S*>(data);

    if (detector == nullptr || frameInfo == nullptr)
        return CVI_FAILURE;

    VIDEO_FRAME_S* frame = &frameInfo->stVFrame;

    if (frame->u64PhyAddr[0] == 0 || frame->u32Length[0] == 0) {
        detector->setError("NV21 Y plane has no physical address or length");
        return CVI_FAILURE;
    }

    CVI_U8* mapped = static_cast<CVI_U8*>(CVI_SYS_Mmap(frame->u64PhyAddr[0], frame->u32Length[0]));

    if (mapped == nullptr) {
        detector->setError("CVI_SYS_Mmap failed for NV21 Y plane");
        return CVI_FAILURE;
    }

    const int stride = frame->u32Stride[0] > 0 ? static_cast<int>(frame->u32Stride[0]) : static_cast<int>(frame->u32Width);

    static std::atomic<uint64_t> frameIdCounter{0};

    const uint64_t frameId = ++frameIdCounter;

    const uint64_t sourcePts = frame->u64PTS;

    const int64_t captureUnixMs = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count();

    // ============================
    // 调用新接口（7参数）
    // ============================
    detector->processGrayFrame(mapped, static_cast<int>(frame->u32Width), static_cast<int>(frame->u32Height), stride, frameId, sourcePts, captureUnixMs);

    CVI_SYS_Munmap(mapped, frame->u32Length[0]);

    return CVI_SUCCESS;
}

void stopVideoStack() {
    if (gVideoStarted.exchange(false))
        deinitVideo();
    if (gRtspStarted.exchange(false))
        deinitRtsp();
}

void signalHandler(int signalNumber) {
    std::fprintf(stderr, "\n[main] signal=%d, shutting down\n", signalNumber);
    stopVideoStack();
    _exit(128 + signalNumber);
}

int startVideoStack(MotionDetector& detector) {
    if (geteuid() != 0) {
        std::fprintf(stderr, "[fatal] reCamera video SDK must run as root\n");
        return -EPERM;
    }

    int result = initVideo();
    if (result != 0) {
        std::fprintf(stderr, "[video] initVideo failed: %d\n", result);
        return result;
    }

    video_ch_param_t motionParam{};
    motionParam.format = VIDEO_FORMAT_NV21;
    motionParam.width  = kMotionWidth;
    motionParam.height = kMotionHeight;
    motionParam.fps    = kMotionFps;
    result             = setupVideo(kMotionChannel, &motionParam);
    if (result != 0) {
        std::fprintf(stderr, "[video] setup motion channel failed: %d\n", result);
        deinitVideo();
        return result;
    }

    result = registerVideoFrameHandler(kMotionChannel, 0, motionFrameCallback, &detector);
    if (result != 0) {
        std::fprintf(stderr, "[video] register motion callback failed: %d\n", result);
        deinitVideo();
        return result;
    }

    if (gSnapshotFps > 0) {
        video_ch_param_t snapshotParam{};
        snapshotParam.format = VIDEO_FORMAT_JPEG;
        snapshotParam.width  = static_cast<std::uint32_t>(gSnapshotWidth);
        snapshotParam.height = static_cast<std::uint32_t>(gSnapshotHeight);
        snapshotParam.fps    = static_cast<std::uint8_t>(gSnapshotFps);
        result               = setupVideo(kSnapshotChannel, &snapshotParam);
        if (result != 0) {
            std::fprintf(stderr, "[video] setup snapshot channel failed: %d (continuing without snapshots)\n", result);
        } else {
            result = registerVideoFrameHandler(kSnapshotChannel, 0, snapshotFrameCallback, nullptr);
            if (result != 0) {
                std::fprintf(stderr, "[video] register snapshot callback failed: %d\n", result);
            } else {
                gSnapshotChannelActive.store(true);
                std::printf("[video] snapshot channel %dx%d@%dfps ready\n", gSnapshotWidth, gSnapshotHeight, gSnapshotFps);
            }
        }
    }

    video_ch_param_t rtspParam{};
    rtspParam.format = VIDEO_FORMAT_H264;
    rtspParam.width  = kRtspWidth;
    rtspParam.height = kRtspHeight;
    rtspParam.fps    = kRtspFps;
    result           = setupVideo(kRtspChannel, &rtspParam);
    if (result != 0) {
        std::fprintf(stderr, "[video] setup H264 channel failed: %d\n", result);
        deinitVideo();
        return result;
    }

    result = registerVideoFrameHandler(kRtspChannel, 0, fpStreamingSendToRtsp, nullptr);
    if (result != 0) {
        std::fprintf(stderr, "[video] register RTSP callback failed: %d\n", result);
        deinitVideo();
        return result;
    }

    const std::uint8_t mask = static_cast<std::uint8_t>(1U << static_cast<unsigned>(kRtspChannel));
    result                  = initRtsp(mask);
    if (result != 0) {
        std::fprintf(stderr, "[rtsp] initRtsp failed: %d\n", result);
        deinitVideo();
        return result;
    }
    gRtspStarted.store(true);

    result = startVideo();
    if (result != 0) {
        std::fprintf(stderr, "[video] startVideo failed: %d\n", result);
        if (gRtspStarted.exchange(false))
            deinitRtsp();
        deinitVideo();
        return result;
    }
    gVideoStarted.store(true);

    const CVI_S32 gopResult = setH264Gop(static_cast<VENC_CHN>(kRtspChannel), kRtspFps);
    if (gopResult != CVI_SUCCESS) {
        std::fprintf(stderr, "[video] warning: failed to set GOP: %d (0x%x)\n", gopResult, static_cast<unsigned int>(gopResult));
    }
    return 0;
}

// 必须和 initRtsp() 启用的 VENC 通道一致。
// initRtsp(1U << 0) 对应 0；initRtsp(1U << 2) 对应 2。
constexpr int kRtspVencChannel = 0;

std::int64_t currentUnixMs() {
    using namespace std::chrono;

    return duration_cast<milliseconds>(system_clock::now().time_since_epoch()).count();
}

}  // namespace

int main() {
    signal(SIGINT, signalHandler);
    signal(SIGTERM, signalHandler);

    const char* configEnvironment = std::getenv("MOTION_CONFIG_PATH");
    const std::string configPath  = configEnvironment != nullptr && configEnvironment[0] != '\0' ? configEnvironment : "/userdata/motion_roi.json";

    gSnapshotWidth  = environmentInt("MOTION_SNAPSHOT_WIDTH", gSnapshotWidth);
    gSnapshotHeight = environmentInt("MOTION_SNAPSHOT_HEIGHT", gSnapshotHeight);
    gSnapshotFps    = environmentInt("MOTION_SNAPSHOT_FPS", gSnapshotFps);
    const char* snapshotFpsEnvironment = std::getenv("MOTION_SNAPSHOT_FPS");
    if (snapshotFpsEnvironment != nullptr && std::atoi(snapshotFpsEnvironment) == 0)
        gSnapshotFps = 0;  // explicitly disabled

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

    MotionDetector detector(kMotionWidth, kMotionHeight);
    detector.setConfig(config);

    WebhookNotifier notifier;
    notifier.configure(config.notify);

    const int videoResult = startVideoStack(detector);
    if (videoResult != 0)
        return EXIT_FAILURE;

    const std::string ip          = getLocalIp();
    const std::string rtspUrl     = "rtsp://" + ip + ":8554/live0";
    const std::string apiBase     = "http://" + ip + ":" + std::to_string(httpPort);

    // Motion transitions are pushed to the configured webhook from the
    // notifier thread; the video callback only builds the payload.
    detector.setEventCallback([&](const MotionEvent& event) {
        const NotifyConfig notify = notifier.config();
        if (!notify.enabled || notify.url.empty() || !eventSelected(notify, event.type))
            return;

        mini_json::Value payload = eventPayload(event, ip, httpPort, notify);

        JpegSnapshot snapshot;
        if (notify.includeImage && gSnapshots.atOrBefore(event.captureUnixMs, snapshot) &&
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
    std::printf("RTSP: %s\n", rtspUrl.c_str());
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
    server.setHandler([&](const HttpRequest& request) {
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
            root["analysis_width"]  = kMotionWidth;
            root["analysis_height"] = kMotionHeight;
            root["analysis_fps"]    = kMotionFps;

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
            response.body = motionConfigToJson(detector.getConfig(), 2);
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
                std::string saveError;
                if (!saveMotionConfigFile(configPath, newConfig, saveError)) {
                    response.status = 500;
                    response.body   = jsonMessage(false, saveError);
                    return response;
                }
                detector.setConfig(newConfig);
                notifier.configure(newConfig.notify);
                response.body = motionConfigToJson(newConfig, 2);
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
    });

    server.start();
    stopVideoStack();
    return EXIT_SUCCESS;
}
