#include "http_server.hpp"
#include "mini_json.hpp"
#include "motion_config.hpp"
#include "motion_detector.hpp"
#include "rtsp_demo.h"

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

#include "frame_sync.hpp"

#include <chrono>

extern "C" {
#include "cvi_sys.h"
#include "cvi_venc.h"
#include "video.h"
}

namespace {

constexpr video_ch_index_t kMotionChannel = VIDEO_CH1;
constexpr video_ch_index_t kRtspChannel   = VIDEO_CH2;
constexpr int kMotionWidth                = 320;
constexpr int kMotionHeight               = 180;
constexpr int kMotionFps                  = 10;
constexpr int kRtspWidth                  = 1920;
constexpr int kRtspHeight                 = 1080;
constexpr int kRtspFps                    = 30;

std::atomic<bool> gVideoStarted(false);
std::atomic<bool> gRtspStarted(false);

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

    const int videoResult = startVideoStack(detector);
    if (videoResult != 0)
        return EXIT_FAILURE;

    const std::string ip = getLocalIp();
    std::printf("\nreCamera motion detector is running\n");
    std::printf("RTSP: rtsp://%s:8554/onvif\n", ip.c_str());
    std::printf("API : http://%s:%d/api/status\n", ip.c_str(), httpPort);
    std::printf("Config file: %s\n\n", configPath.c_str());
    std::fflush(stdout);

    HttpServer server(httpPort);
    server.setHandler([&](const HttpRequest& request) {
        HttpResponse response;

        if (request.method == "GET" && request.path == "/") {
            response.contentType = "text/plain; charset=utf-8";
            response.body =
                "reCamera Motion Detection\n"
                "GET  /api/health\n"
                "GET  /api/status\n"
                "GET  /api/config\n"
                "POST /api/config\n"
                "POST /api/background/reset\n";
            return response;
        }

        if (request.method == "GET" && request.path == "/api/health") {
            mini_json::Value root   = mini_json::Value::object();
            root["ok"]              = true;
            root["device_ip"]       = ip;
            root["rtsp_url"]        = "rtsp://" + ip + ":8554/onvif";
            root["analysis_width"]  = kMotionWidth;
            root["analysis_height"] = kMotionHeight;
            root["analysis_fps"]    = kMotionFps;
            response.body           = root.dump(2);
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
                const MotionConfig newConfig = parseMotionConfig(request.body);
                std::string saveError;
                if (!saveMotionConfigFile(configPath, newConfig, saveError)) {
                    response.status = 500;
                    response.body   = jsonMessage(false, saveError);
                    return response;
                }
                detector.setConfig(newConfig);
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

        response.status = 404;
        response.body   = jsonMessage(false, "endpoint not found");
        return response;
    });

    server.start();
    stopVideoStack();
    return EXIT_SUCCESS;
}
