#ifndef RECAMERA_MOTION_RTSP_DEMO_H
#define RECAMERA_MOTION_RTSP_DEMO_H

#include <pthread.h>
#include <stdint.h>

#include "cvi_type.h"
#include "linux/cvi_common.h"
#include "rtsp.h"

#ifdef __cplusplus
extern "C" {
#endif

#define MAX_RTSP_SESSION 6

typedef struct APP_PARAM_RTSP_S {
    CVI_S32 session_cnt;
    CVI_S32 port;
    CVI_BOOL bStart[MAX_RTSP_SESSION];
    VENC_CHN VencChn[MAX_RTSP_SESSION];
    CVI_RTSP_SESSION* pstSession[MAX_RTSP_SESSION];
    CVI_RTSP_SESSION_ATTR SessionAttr[MAX_RTSP_SESSION];
    CVI_RTSP_STATE_LISTENER listener;
    CVI_RTSP_CTX* pstServerCtx;
    pthread_mutex_t mutex;
} APP_PARAM_RTSP_T;

int initRtsp(uint8_t channelEnableMask);
int deinitRtsp(void);
int fpStreamingSendToRtsp(void* pData, void* pArgs, void* pUserData);
int requestRtspIdr(void);

#ifdef __cplusplus
}
#endif

#endif
