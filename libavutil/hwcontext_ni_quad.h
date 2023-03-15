/*
* This file is part of FFmpeg.
*
* FFmpeg is free software; you can redistribute it and/or
* modify it under the terms of the GNU Lesser General Public
* License as published by the Free Software Foundation; either
* version 2.1 of the License, or (at your option) any later version.
*
* FFmpeg is distributed in the hope that it will be useful,
* but WITHOUT ANY WARRANTY; without even the implied warranty of
* MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
* Lesser General Public License for more details.
*
* You should have received a copy of the GNU Lesser General Public
* License along with FFmpeg; if not, write to the Free Software
* Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA
*/

#ifndef AVUTIL_HWCONTEXT_NI_QUAD_H
#define AVUTIL_HWCONTEXT_NI_QUAD_H

#include "hwcontext.h"
#include <ni_device_api.h>
#include <ni_rsrc_api.h>
#include <ni_util.h>

enum
{
  NI_MEMTYPE_VIDEO_MEMORY_NONE,
  NI_MEMTYPE_VIDEO_MEMORY_DECODER_TARGET,
  NI_MEMTYPE_VIDEO_MEMORY_HWUPLOAD_TARGET,
};

typedef enum _ni_filter_poolsize_code {
    NI_DECODER_ID = -1,
    NI_SCALE_ID   = -2,
    NI_PAD_ID     = -3,
    NI_CROP_ID    = -4,
    NI_OVERLAY_ID = -5,
    NI_ROI_ID     = -6,
    NI_BG_ID      = -7,
    NI_STACK_ID   = -8,
    NI_ROTATE_ID  = -9,
    NI_DRAWBOX_ID = -10,
} ni_filter_poolsize_code;

typedef struct NIFramesContext {
  niFrameSurface1_t *surfaces_internal;
  int                nb_surfaces_used;
  niFrameSurface1_t **surface_ptrs;
  ni_session_context_t api_ctx;//for down/uploading frames
  ni_session_data_io_t src_session_io_data; // for upload frame to be sent up
  // int pc_height, pc_width, pc_crop_bottom, pc_crop_right; //precropped values
  ni_split_context_t split_ctx;
  ni_device_handle_t suspended_device_handle;
  int                uploader_device_id; //same one passed to libxcoder session open
} NIFramesContext;

/**
* This struct is allocated as AVHWDeviceContext.hwctx
*/
typedef struct AVNIDeviceContext {
    int uploader_ID;

    ni_device_handle_t cards[NI_MAX_DEVICE_CNT];
} AVNIDeviceContext;

/**
* This struct is allocated as AVHWFramesContext.hwctx
*/
typedef struct AVNIFramesContext {
  niFrameSurface1_t *surfaces;
  int            nb_surfaces;
  int            keep_alive_timeout;
  int            frame_type;
#ifdef NI_DEC_GSTREAMER_SUPPORT
  int            dev_dec_idx;                /* index of the decoder on the xcoder card */
#endif
} AVNIFramesContext;

static inline int ni_get_cardno(const AVFrame *frame) {
    int cardno;

#ifdef NI_DEC_GSTREAMER_SUPPORT
    AVHWFramesContext *pAVHFWCtx = (AVHWFramesContext *) frame->hw_frames_ctx->data;
    AVNIFramesContext *pAVNIFramesCtx = (AVNIFramesContext *) pAVHFWCtx->hwctx;
    cardno            =  pAVNIFramesCtx->dev_dec_idx;
#else
    cardno      = (int)frame->opaque;
#endif
    return cardno;
}

#endif /* AVUTIL_HWCONTEXT_NI_H */
