#include "video_stack.hpp"

#include "event_payload.hpp"
#include "rtsp_demo.h"

#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <signal.h>
#include <unistd.h>
#include <vector>

extern "C" {
#include "cvi_sys.h"
#include "cvi_venc.h"
#include "video.h"
}

namespace video_stack {
namespace {
constexpr video_ch_index_t kSnapshotChannel = VIDEO_CH0;
constexpr video_ch_index_t kMotionChannel = VIDEO_CH1;
constexpr video_ch_index_t kRtspChannel = VIDEO_CH2;
constexpr int kRtspWidth = 1920;
constexpr int kRtspHeight = 1080;
constexpr int kRtspFps = 30;
std::atomic<bool> gVideoStarted(false);
}  // namespace

std::atomic<bool> gRtspStarted(false);
RtspConfig gRtspRuntimeConfig;

int gSnapshotWidth  = 1280;
int gSnapshotHeight = 720;
int gSnapshotFps    = 10;

SnapshotStore gSnapshots(16);
std::atomic<bool> gSnapshotChannelActive(false);

static int snapshotFrameCallback(void* data, void* args, void* userData) {
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

static int setH264Gop(VENC_CHN channel, CVI_U32 newGop) {
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

static int motionFrameCallback(void* data, void* args, void* userData) {
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

int startVideoStack(MotionDetector& detector, const RtspConfig& rtspConfig) {
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

    if (rtspConfig.enabled) {
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
        result = initRtsp(mask, rtspConfig.port, rtspConfig.path.c_str());
        if (result != 0) {
            std::fprintf(stderr, "[rtsp] initRtsp failed: %d\n", result);
            deinitVideo();
            return result;
        }
        gRtspStarted.store(true);
    }

    result = startVideo();
    if (result != 0) {
        std::fprintf(stderr, "[video] startVideo failed: %d\n", result);
        if (gRtspStarted.exchange(false))
            deinitRtsp();
        deinitVideo();
        return result;
    }
    gVideoStarted.store(true);

    if (rtspConfig.enabled) {
        const CVI_S32 gopResult = setH264Gop(static_cast<VENC_CHN>(kRtspChannel), kRtspFps);
        if (gopResult != CVI_SUCCESS) {
            std::fprintf(stderr, "[video] warning: failed to set GOP: %d (0x%x)\n", gopResult, static_cast<unsigned int>(gopResult));
        }
    }
    return 0;
}

}  // namespace video_stack
