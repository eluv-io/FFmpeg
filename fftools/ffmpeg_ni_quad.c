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

#include <stdlib.h>

#include "libavutil/dict.h"
#include "libavutil/hwcontext.h"
#include "libavutil/hwcontext_ni_quad.h"
#include "libavutil/mem.h"
#include "libavutil/opt.h"
//#include "libavcodec/qsv.h"

#include "ffmpeg.h"
// declaration here is valid only above FFmpeg v4.2.1
#if ((LIBAVCODEC_VERSION_MAJOR >= 58) && (LIBAVCODEC_VERSION_MINOR > 54))
static AVBufferRef *hw_device_ctx;
#endif
//char *qsv_device = NULL;

static int ni_quad_get_buffer(AVCodecContext *s, AVFrame *frame, int flags)
{
    InputStream *ist = s->opaque;

    return av_hwframe_get_buffer(ist->hw_frames_ctx, frame, 0);
}

static void ni_quad_uninit(AVCodecContext *s)
{
    InputStream *ist = s->opaque;
    av_buffer_unref(&ist->hw_frames_ctx);
}

static int ni_quad_device_init(InputStream *ist)
{
    int err;
    AVDictionary *dict = NULL;
    av_log(NULL, AV_LOG_ERROR, "NI DEVICE INIT DEBUG\n");
    //if (qsv_device) {
    //    err = av_dict_set(&dict, "child_device", qsv_device, 0);
    //    if (err < 0)
    //        return err;
    //}

    err = av_hwdevice_ctx_create(&hw_device_ctx, AV_HWDEVICE_TYPE_NI_QUADRA,
        ist->hwaccel_device, dict, 0);
    if (err < 0) {
        av_log(NULL, AV_LOG_ERROR, "Error creating a NI device\n");
        goto err_out;
    }

err_out:
    if (dict)
        av_dict_free(&dict);

    return err;
}

int ni_quad_init(AVCodecContext *s)
{
  InputStream *ist = s->opaque; //why set to opaque? who uses it?
  AVHWFramesContext *frames_ctx;
  AVNIFramesContext *frames_hwctx;
  int ret;
  av_log(NULL, AV_LOG_ERROR, "NI INIT ffmpeg_ni_quad.c\n");
  if (!hw_device_ctx) {
    ret = ni_quad_device_init(ist);
    if (ret < 0)
      return ret;
  }

  av_buffer_unref(&ist->hw_frames_ctx);
  av_log(NULL, AV_LOG_ERROR, "NI INIT ffmpeg_ni_quad.c, time to alloc hwframectx\n");
  ist->hw_frames_ctx = av_hwframe_ctx_alloc(hw_device_ctx);
  if (!ist->hw_frames_ctx)
    return AVERROR(ENOMEM);
  s->hw_frames_ctx = ist->hw_frames_ctx;
  frames_ctx = (AVHWFramesContext*)ist->hw_frames_ctx->data;
  frames_hwctx = frames_ctx->hwctx;

  frames_ctx->width = FFALIGN(s->coded_width, 32);
  frames_ctx->height = FFALIGN(s->coded_height, 32);
  frames_ctx->format = AV_PIX_FMT_NI_QUAD;
  frames_ctx->sw_format = s->sw_pix_fmt;
  frames_ctx->initial_pool_size = 64;
  frames_hwctx->frame_type = NI_MEMTYPE_VIDEO_MEMORY_DECODER_TARGET;

  ret = av_hwframe_ctx_init(ist->hw_frames_ctx); //might need to set the hw_frames_ctx format
  if (ret < 0) {
    av_log(NULL, AV_LOG_ERROR, "Error initializing a NI frame pool\n");
    return ret;
  }
  //there's a retrieve data command option too //int  (*hwaccel_retrieve_data)(AVCodecContext *s, AVFrame *frame);
  ist->hwaccel_get_buffer = ni_quad_get_buffer;
  ist->hwaccel_uninit = ni_quad_uninit;

  return 0;
}
