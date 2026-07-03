#include "rtsp_demo.h"

#include "app_ipcam_comm.h"
#include "app_ipcam_venc.h"
#include "cvi_venc.h"
#include "frame_sei.hpp"
#include "frame_sync.hpp"

#include <chrono>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <vector>

static APP_PARAM_RTSP_T g_rtsp;
static CVI_BOOL g_mutexInitialized = CVI_FALSE;

static std::int64_t nowUnixMs() {
    using namespace std::chrono;
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
}

static void* delayedIdr(void* argument) {
    const VENC_CHN channel = (VENC_CHN)(intptr_t)argument;
    usleep(500 * 1000);
    const CVI_S32 result = CVI_VENC_RequestIDR(channel, CVI_TRUE);
    fprintf(stderr, "[rtsp] RequestIDR channel=%d result=%d (0x%x)\n", channel, result, (unsigned int)result);
    return NULL;
}

static void onPlay(int references, void* argument) {
    const VENC_CHN channel = (VENC_CHN)(intptr_t)argument;
    fprintf(stderr, "[rtsp] PLAY channel=%d references=%d\n", channel, references);
    pthread_t thread;
    if (pthread_create(&thread, NULL, delayedIdr, (void*)(intptr_t)channel) == 0) {
        pthread_detach(thread);
    }
}

static void onConnect(const char* ip, CVI_VOID* argument) {
    (void)argument;
    fprintf(stderr, "[rtsp] client connected: %s\n", ip ? ip : "unknown");
}

static void onDisconnect(const char* ip, CVI_VOID* argument) {
    (void)argument;
    fprintf(stderr, "[rtsp] client disconnected: %s\n", ip ? ip : "unknown");
}

static int loadParameters(void) {
    memset(&g_rtsp, 0, sizeof(g_rtsp));
    g_rtsp.port = 8554;
    for (int i = 0; i < MAX_RTSP_SESSION; ++i) {
        g_rtsp.VencChn[i]                   = i;
        g_rtsp.SessionAttr[i].video.bitrate = 4096;
        g_rtsp.SessionAttr[i].video.codec   = RTSP_VIDEO_NONE;
        g_rtsp.SessionAttr[i].audio.codec   = RTSP_AUDIO_NONE;
        g_rtsp.SessionAttr[i].maxConnNum    = 4;
    }
    return CVI_SUCCESS;
}

static int initializeSessionAttributes(void) {
    APP_PARAM_VENC_CTX_S* context = app_ipcam_Venc_Param_Get();
    if (context == NULL) {
        fprintf(stderr, "[rtsp] app_ipcam_Venc_Param_Get returned NULL\n");
        return CVI_FAILURE;
    }

    for (CVI_S32 i = 0; i < g_rtsp.session_cnt; ++i) {
        const VENC_CHN channel = g_rtsp.VencChn[i];
        if (channel < 0 || channel >= context->s32VencChnCnt) {
            fprintf(stderr, "[rtsp] invalid encoder channel=%d count=%d\n", channel, context->s32VencChnCnt);
            return CVI_FAILURE;
        }

        const PAYLOAD_TYPE_E payload = context->astVencChnCfg[channel].enType;
        if (payload == PT_H264) {
            g_rtsp.SessionAttr[i].video.codec = RTSP_VIDEO_H264;
        } else if (payload == PT_H265) {
            g_rtsp.SessionAttr[i].video.codec = RTSP_VIDEO_H265;
        } else if (payload == PT_JPEG || payload == PT_MJPEG) {
            g_rtsp.SessionAttr[i].video.codec = RTSP_VIDEO_JPEG;
        } else {
            fprintf(stderr, "[rtsp] unsupported payload=%d on channel=%d\n", payload, channel);
            return CVI_FAILURE;
        }

        snprintf(g_rtsp.SessionAttr[i].name, sizeof(g_rtsp.SessionAttr[i].name), "live%d", i);
        g_rtsp.SessionAttr[i].reuseFirstSource = 1;
        g_rtsp.SessionAttr[i].video.play       = onPlay;
        g_rtsp.SessionAttr[i].video.playArg    = (void*)(intptr_t)channel;
    }
    return CVI_SUCCESS;
}

static int destroyServer(void) {
    if (g_rtsp.pstServerCtx == NULL)
        return CVI_SUCCESS;

    CVI_RTSP_Stop(g_rtsp.pstServerCtx);
    if (g_mutexInitialized)
        pthread_mutex_lock(&g_rtsp.mutex);

    for (CVI_S32 i = 0; i < g_rtsp.session_cnt; ++i) {
        if (g_rtsp.bStart[i] && g_rtsp.pstSession[i] != NULL) {
            CVI_RTSP_DestroySession(g_rtsp.pstServerCtx, g_rtsp.pstSession[i]);
            g_rtsp.pstSession[i] = NULL;
            g_rtsp.bStart[i]     = CVI_FALSE;
        }
    }

    if (g_mutexInitialized)
        pthread_mutex_unlock(&g_rtsp.mutex);
    CVI_RTSP_Destroy(&g_rtsp.pstServerCtx);
    g_rtsp.pstServerCtx = NULL;

    if (g_mutexInitialized) {
        pthread_mutex_destroy(&g_rtsp.mutex);
        g_mutexInitialized = CVI_FALSE;
    }
    FrameSyncRegistry::instance().clear();
    fprintf(stderr, "[rtsp] server destroyed\n");
    return CVI_SUCCESS;
}

static int createServer(void) {
    CVI_S32 result = initializeSessionAttributes();
    if (result != CVI_SUCCESS)
        return result;

    CVI_RTSP_CONFIG config;
    memset(&config, 0, sizeof(config));
    config.port       = g_rtsp.port;
    config.timeout    = 10;
    config.maxConnNum = 4;

    result = CVI_RTSP_Create(&g_rtsp.pstServerCtx, &config);
    if (result < 0 || g_rtsp.pstServerCtx == NULL) {
        fprintf(stderr, "[rtsp] CVI_RTSP_Create failed: %d\n", result);
        return result < 0 ? result : CVI_FAILURE;
    }

    if (pthread_mutex_init(&g_rtsp.mutex, NULL) != 0) {
        CVI_RTSP_Destroy(&g_rtsp.pstServerCtx);
        return CVI_FAILURE;
    }
    g_mutexInitialized = CVI_TRUE;

    result = CVI_RTSP_Start(g_rtsp.pstServerCtx);
    if (result < 0) {
        fprintf(stderr, "[rtsp] CVI_RTSP_Start failed: %d\n", result);
        destroyServer();
        return result;
    }

    pthread_mutex_lock(&g_rtsp.mutex);
    for (CVI_S32 i = 0; i < g_rtsp.session_cnt; ++i) {
        CVI_RTSP_SESSION_ATTR* attr = &g_rtsp.SessionAttr[i];
        result                      = CVI_RTSP_CreateSession(g_rtsp.pstServerCtx, attr, &g_rtsp.pstSession[i]);
        fprintf(stderr, "[rtsp] session name=%s channel=%d codec=%d result=%d session=%p\n", attr->name, g_rtsp.VencChn[i], attr->video.codec, result, (void*)g_rtsp.pstSession[i]);
        if (result < 0 || g_rtsp.pstSession[i] == NULL) {
            pthread_mutex_unlock(&g_rtsp.mutex);
            destroyServer();
            return result < 0 ? result : CVI_FAILURE;
        }
        g_rtsp.bStart[i] = CVI_TRUE;
    }

    memset(&g_rtsp.listener, 0, sizeof(g_rtsp.listener));
    g_rtsp.listener.onConnect    = onConnect;
    g_rtsp.listener.argConn      = &g_rtsp;
    g_rtsp.listener.onDisconnect = onDisconnect;
    g_rtsp.listener.argDisconn   = &g_rtsp;
    result                       = CVI_RTSP_SetListener(g_rtsp.pstServerCtx, &g_rtsp.listener);
    pthread_mutex_unlock(&g_rtsp.mutex);

    if (result < 0) {
        fprintf(stderr, "[rtsp] CVI_RTSP_SetListener failed: %d\n", result);
        destroyServer();
        return result;
    }
    return CVI_SUCCESS;
}

static const CVI_U8* skipAnnexBStartCode(const CVI_U8* data, CVI_U32 size, CVI_U32& payloadSize) {
    payloadSize = size;
    if (data == NULL)
        return NULL;
    if (size >= 4 && data[0] == 0 && data[1] == 0 && data[2] == 0 && data[3] == 1) {
        payloadSize -= 4;
        return data + 4;
    }
    if (size >= 3 && data[0] == 0 && data[1] == 0 && data[2] == 1) {
        payloadSize -= 3;
        return data + 3;
    }
    return data;
}

static bool isVclNal(const CVI_U8* data, CVI_U32 size, CVI_RTSP_VIDEO_CODEC codec) {
    CVI_U32 payloadSize = 0;
    const CVI_U8* nal   = skipAnnexBStartCode(data, size, payloadSize);
    if (nal == NULL || payloadSize == 0)
        return false;

    if (codec == RTSP_VIDEO_H264) {
        const unsigned nalType = nal[0] & 0x1fU;
        return nalType >= 1U && nalType <= 5U;
    }
    if (codec == RTSP_VIDEO_H265) {
        const unsigned nalType = (nal[0] >> 1U) & 0x3fU;
        return nalType <= 31U;
    }
    return false;
}

int fpStreamingSendToRtsp(void* pData, void* pArgs, void* pUserData) {
    (void)pUserData;
    if (pData == NULL || pArgs == NULL)
        return CVI_FAILURE;

    APP_DATA_CTX_S* dataContext       = (APP_DATA_CTX_S*)pArgs;
    APP_VENC_CHN_CFG_S* encoderConfig = (APP_VENC_CHN_CFG_S*)dataContext->stDataParam.pParam;
    VENC_STREAM_S* stream             = (VENC_STREAM_S*)pData;
    if (encoderConfig == NULL || stream == NULL || stream->pstPack == NULL) {
        return CVI_FAILURE;
    }

    int sessionIndex = -1;
    for (int i = 0; i < g_rtsp.session_cnt; ++i) {
        if (g_rtsp.VencChn[i] == encoderConfig->VencChn) {
            sessionIndex = i;
            break;
        }
    }
    if (sessionIndex < 0 || !g_rtsp.bStart[sessionIndex] || g_rtsp.pstServerCtx == NULL || g_rtsp.pstSession[sessionIndex] == NULL) {
        return CVI_SUCCESS;
    }
    if (stream->u32PackCount == 0)
        return CVI_SUCCESS;

    const CVI_U64 vencPts = stream->pstPack[0].u64PTS;
    FrameSyncInfo sync;
    if (!FrameSyncRegistry::instance().matchEncodedFrame(encoderConfig->VencChn, vencPts, sync)) {
        // This fallback keeps the stream alive, but HTTP cannot exactly match it.
        sync.frameId       = vencPts;
        sync.sourcePts     = vencPts;
        sync.vencPts       = vencPts;
        sync.captureUnixMs = nowUnixMs();
        sync.channel       = encoderConfig->VencChn;
        sync.exactPtsMatch = false;
    } else if (!sync.exactPtsMatch) {
        fprintf(stderr,
                "[frame-sync] FIFO fallback channel=%d frame=%llu sourcePts=%llu vencPts=%llu\n",
                encoderConfig->VencChn,
                (unsigned long long)sync.frameId,
                (unsigned long long)sync.sourcePts,
                (unsigned long long)sync.vencPts);
    }

    const CVI_RTSP_VIDEO_CODEC codec = g_rtsp.SessionAttr[sessionIndex].video.codec;
    std::vector<std::uint8_t> sei;
    if (codec == RTSP_VIDEO_H264) {
        sei = buildFrameSyncSei(FrameVideoCodec::H264, sync);
    } else if (codec == RTSP_VIDEO_H265) {
        sei = buildFrameSyncSei(FrameVideoCodec::H265, sync);
    }

    CVI_RTSP_DATA data;
    memset(&data, 0, sizeof(data));
    data.timestamp = vencPts;  // Kept for future SDK versions; current cvi_rtsp ignores it.

    const CVI_U32 inputCount = stream->u32PackCount > CVI_RTSP_DATA_MAX_BLOCK ? CVI_RTSP_DATA_MAX_BLOCK : stream->u32PackCount;

    bool canInsertSei   = !sei.empty() && inputCount < CVI_RTSP_DATA_MAX_BLOCK;
    CVI_U32 insertIndex = 0;
    if (canInsertSei) {
        insertIndex = inputCount;
        for (CVI_U32 i = 0; i < inputCount; ++i) {
            VENC_PACK_S* pack = &stream->pstPack[i];
            if (pack->pu8Addr == NULL || pack->u32Offset > pack->u32Len) {
                return CVI_FAILURE;
            }
            const CVI_U8* ptr = pack->pu8Addr + pack->u32Offset;
            const CVI_U32 len = pack->u32Len - pack->u32Offset;
            if (isVclNal(ptr, len, codec)) {
                insertIndex = i;
                break;
            }
        }
    }

    CVI_U32 outputCount = 0;
    for (CVI_U32 i = 0; i <= inputCount; ++i) {
        if (canInsertSei && i == insertIndex) {
            data.dataPtr[outputCount] = sei.data();
            data.dataLen[outputCount] = static_cast<CVI_U32>(sei.size());
            ++outputCount;
        }
        if (i == inputCount)
            break;

        VENC_PACK_S* pack = &stream->pstPack[i];
        if (pack->pu8Addr == NULL || pack->u32Offset > pack->u32Len) {
            return CVI_FAILURE;
        }
        data.dataPtr[outputCount] = pack->pu8Addr + pack->u32Offset;
        data.dataLen[outputCount] = pack->u32Len - pack->u32Offset;
        ++outputCount;
    }
    data.blockCnt = outputCount;

    const CVI_S32 result = CVI_RTSP_WriteFrame(g_rtsp.pstServerCtx, g_rtsp.pstSession[sessionIndex]->video, &data);
    if (result != CVI_SUCCESS) {
        fprintf(stderr, "[rtsp] CVI_RTSP_WriteFrame channel=%d frame=%llu failed: %d (0x%x)\n", encoderConfig->VencChn, (unsigned long long)sync.frameId, result, (unsigned int)result);
    }
    return result;
}

int initRtsp(uint8_t channelEnableMask) {
    loadParameters();
    FrameSyncRegistry::instance().clear();
    int count = 0;
    for (int channel = 0; channel < MAX_RTSP_SESSION; ++channel) {
        if ((channelEnableMask & (1U << channel)) != 0U) {
            g_rtsp.VencChn[count++] = channel;
        }
    }
    if (count == 0) {
        fprintf(stderr, "[rtsp] no channel enabled, mask=0x%02x\n", channelEnableMask);
        return CVI_FAILURE;
    }
    g_rtsp.session_cnt = count;
    return createServer();
}

int deinitRtsp(void) {
    return destroyServer();
}

int requestRtspIdr(void) {
    if (g_rtsp.session_cnt <= 0)
        return CVI_FAILURE;
    return CVI_VENC_RequestIDR(g_rtsp.VencChn[0], CVI_TRUE);
}
