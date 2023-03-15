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

#include "config.h"

#include <fcntl.h>
#include <string.h>
#if HAVE_UNISTD_H
#   include <unistd.h>
#endif

#include "avassert.h"
#include "buffer.h"
#include "common.h"
#include "hwcontext.h"
#include "hwcontext_internal.h"
#include "hwcontext_ni_quad.h"
#include "libavutil/imgutils.h"
#include "mem.h"
#include "pixdesc.h"
#include "pixfmt.h"

static enum AVPixelFormat supported_pixel_formats[] = {
    AV_PIX_FMT_YUV420P, AV_PIX_FMT_YUYV422, AV_PIX_FMT_UYVY422,
    AV_PIX_FMT_NV12,    AV_PIX_FMT_ARGB,    AV_PIX_FMT_RGBA,
    AV_PIX_FMT_ABGR,    AV_PIX_FMT_BGRA,    AV_PIX_FMT_YUV420P10LE,
    AV_PIX_FMT_NV16,    AV_PIX_FMT_BGR0,    AV_PIX_FMT_P010LE,
    AV_PIX_FMT_BGRP};

static inline void ni_align_free(void *opaque, uint8_t *data)
{
    if (data)
    {
        free(data);
    }
}

static inline void ni_frame_free(void *opaque, uint8_t *data)
{
    if (data)
    {
        //assuming for hwframes there is no data0,1,2?
        //TODO use int32t device_handle to kill the buffer!
        niFrameSurface1_t* p_data3 = (niFrameSurface1_t*)data;
        if (p_data3->ui16FrameIdx != 0)
        {
            ni_hwframe_buffer_recycle(p_data3, p_data3->device_handle);
        }
        free(p_data3);
    }
}

static int ni_device_create(AVHWDeviceContext *ctx, const char *device,
                            AVDictionary *opts, int flags)
{
    AVNIDeviceContext *ni_hw_ctx;
    char *blk_name;
    uint8_t *fw_rev;
    int i, card_cnt = 0, ret = 0;
    ni_device_handle_t fd;
    uint32_t max_io_size;
    ni_device_t ni_devices;

    ni_hw_ctx              = (AVNIDeviceContext *)ctx->hwctx;
    ni_hw_ctx->uploader_ID = -2; // -1 is auto load balance, default -2 invalid

    if (device) {
        /* parse device string and fail if incorrect */
        av_log(ctx, AV_LOG_VERBOSE, "%s %s\n", __func__, device);
        ni_hw_ctx->uploader_ID = atoi(device);
        av_log(ctx, AV_LOG_DEBUG, "%s: given uploader ID %d\n", __func__,
               ni_hw_ctx->uploader_ID);
        if (ni_hw_ctx->uploader_ID < -1) {
            av_log(ctx, AV_LOG_ERROR, "%s: uploader ID %d must be >= -1.\n",
                   __func__, ni_hw_ctx->uploader_ID);
            return AVERROR_UNKNOWN;
        }
    }

    for (i = 0; i < NI_MAX_DEVICE_CNT; i++) {
        ni_hw_ctx->cards[i] = NI_INVALID_DEVICE_HANDLE;
    }

    /* Scan all cards on the host, only look at NETINT cards */
    if (ni_rsrc_list_all_devices(&ni_devices) == NI_RETCODE_SUCCESS) {
        // Note: this only checks for Netint encoders
        for (i = 0; i < ni_devices.xcoder_cnt[NI_DEVICE_TYPE_ENCODER]; i++) {
            blk_name =
                &(ni_devices.xcoders[NI_DEVICE_TYPE_ENCODER][i].blk_name[0]);
            fw_rev =
                &(ni_devices.xcoders[NI_DEVICE_TYPE_ENCODER][i].fw_rev[0]);
            av_log(ctx, AV_LOG_DEBUG, "%s blk name %s\n", __func__, blk_name);
            fd = ni_device_open(blk_name, &max_io_size);
            if (fd != NI_INVALID_DEVICE_HANDLE) {
                ni_hw_ctx->cards[card_cnt++] = fd;
                if (ni_rsrc_is_fw_compat(fw_rev) == 2) {
                    av_log(ctx, AV_LOG_WARNING,
                           "WARNING - Query %s FW "
                           "version: %s is below the minimum support "
                           "version for this SW version. Some features may "
                           "be missing.\n",
                           blk_name, fw_rev);
                }
            }
        }
    } else {
        ret = AVERROR_UNKNOWN;
    }

    return ret;
}

static void ni_device_uninit(AVHWDeviceContext *ctx)
{
    AVNIDeviceContext *ni_hw_ctx;
    int i;

    ni_hw_ctx = (AVNIDeviceContext *)ctx->hwctx;

    av_log(ctx, AV_LOG_VERBOSE, "%s\n", __func__);

    for (i = 0; i < NI_MAX_DEVICE_CNT; i++) {
        ni_device_handle_t fd = ni_hw_ctx->cards[i];
        if (fd != NI_INVALID_DEVICE_HANDLE) {
            ni_hw_ctx->cards[i] = NI_INVALID_DEVICE_HANDLE;
            ni_device_close(fd);
        } else {
            break;
        }
    }

    return;
}

static int ni_frames_get_constraints(AVHWDeviceContext *ctx,
                                     const void *hwconfig,
                                     AVHWFramesConstraints *constraints)
{
    int i;
    int num_pix_fmts_supported;

    num_pix_fmts_supported = FF_ARRAY_ELEMS(supported_pixel_formats);

    constraints->valid_sw_formats = av_malloc_array(num_pix_fmts_supported + 1,
                                                    sizeof(*constraints->valid_sw_formats));
    if (!constraints->valid_sw_formats) {
        return AVERROR(ENOMEM);
    }

    for (i = 0; i < num_pix_fmts_supported; i++) {
        constraints->valid_sw_formats[i] = supported_pixel_formats[i];
    }
    constraints->valid_sw_formats[num_pix_fmts_supported] = AV_PIX_FMT_NONE;

    constraints->valid_hw_formats = av_malloc_array(2, sizeof(*constraints->valid_hw_formats));
    if (!constraints->valid_hw_formats) {
        return AVERROR(ENOMEM);
    }

    constraints->valid_hw_formats[0] = AV_PIX_FMT_NI_QUAD;
    constraints->valid_hw_formats[1] = AV_PIX_FMT_NONE;

    return 0;
}

static int ni_get_buffer(AVHWFramesContext *ctx, AVFrame *frame)
{
    int ret = 0;
    uint8_t *buf;
    uint32_t buf_size;
    ni_frame_t *xfme;
    NIFramesContext *ni_ctx = ctx->internal->priv;
    ni_session_data_io_t dst_session_io_data;
    ni_session_data_io_t * p_dst_session_data = &dst_session_io_data;
    bool isnv12frame = (ctx->sw_format == AV_PIX_FMT_NV12 ||
                        ctx->sw_format == AV_PIX_FMT_P010LE);

    av_log(ctx, AV_LOG_TRACE, "hwcontext_ni.c:ni_get_buffer()\n");

    // alloc dest avframe buff
    memset(p_dst_session_data, 0, sizeof(dst_session_io_data));
    ret = ni_frame_buffer_alloc(&p_dst_session_data->data.frame, ctx->width,
                                ctx->height, 0, 1, // codec type does not matter, metadata exists
                                ni_ctx->api_ctx.bit_depth_factor, 1, !isnv12frame);
    if (ret != 0) {
        return AVERROR(ENOMEM);
    }

    xfme = &p_dst_session_data->data.frame;
    buf_size = xfme->data_len[0] + xfme->data_len[1] +
               xfme->data_len[2] + xfme->data_len[3];
    buf = xfme->p_data[0];
    memset(buf, 0, buf_size);
    frame->buf[0] = av_buffer_create(buf, buf_size, ni_frame_free, NULL, 0);
    if (!frame->buf[0]) {
        return AVERROR(ENOMEM);
    }
    frame->data[3] = xfme->p_buffer + xfme->data_len[0] + xfme->data_len[1] +
                     xfme->data_len[2];
    frame->format = AV_PIX_FMT_NI_QUAD;
    frame->width = ctx->width;
    frame->height = ctx->height;

    return 0;
}

static int ni_transfer_get_formats(AVHWFramesContext *ctx,
                                   enum AVHWFrameTransferDirection dir,
                                   enum AVPixelFormat **formats)
{
    enum AVPixelFormat *fmts;

    fmts = av_malloc_array(2, sizeof(*fmts));
    if (!fmts) {
        return AVERROR(ENOMEM);
    }

    fmts[0] = ctx->sw_format;
    fmts[1] = AV_PIX_FMT_NONE;

    *formats = fmts;

    return 0;
}

static void ni_frames_uninit(AVHWFramesContext *ctx)
{
    NIFramesContext *ni_ctx = ctx->internal->priv;
    int dev_dec_idx = ni_ctx->uploader_device_id; //Supplied by init_hw_device ni=<name>:<id> or ni_hwupload=<id>

    av_log(ctx, AV_LOG_DEBUG, "%s: only close if upload instance, poolsize=%d "
                              "devid=%d\n",
                              __func__, ctx->initial_pool_size, dev_dec_idx);

    if (dev_dec_idx != -2 && ctx->initial_pool_size >= 0)
    {
        if (ni_ctx->src_session_io_data.data.frame.p_buffer) {
            av_log(ctx, AV_LOG_DEBUG, "%s:free upload src frame buffer\n",
                   __func__);
            ni_frame_buffer_free(&ni_ctx->src_session_io_data.data.frame);
        }
        av_log(ctx, AV_LOG_VERBOSE, "SessionID = %d!\n", ni_ctx->api_ctx.session_id);
        if (ni_ctx->api_ctx.session_id != NI_INVALID_SESSION_ID) {
            ni_device_session_close(&ni_ctx->api_ctx, 1, NI_DEVICE_TYPE_UPLOAD);
        }
        ni_device_session_context_clear(&ni_ctx->api_ctx);

        //only upload frames init allocates these ones
        av_freep(&ni_ctx->surface_ptrs);
        av_freep(&ni_ctx->surfaces_internal);
    }

    if (ni_ctx->suspended_device_handle != -1)
    {
        av_log(ctx, AV_LOG_DEBUG, "%s: close file handle, =%d\n",
               __func__, ni_ctx->suspended_device_handle);
        ni_device_close(ni_ctx->suspended_device_handle);
    }
}

static AVBufferRef *ni_pool_alloc(void *opaque,
#if LIBAVUTIL_VERSION_MAJOR >= 58 || LIBAVUTIL_VERSION_MAJOR >= 57 && LIBAVUTIL_VERSION_MINOR >= 17
                                  size_t size)
#else
                                  int size)
#endif
{
    AVHWFramesContext    *ctx = (AVHWFramesContext*)opaque;
    NIFramesContext       *s = ctx->internal->priv;
    AVNIFramesContext *frames_hwctx = ctx->hwctx;

    if (s->nb_surfaces_used < frames_hwctx->nb_surfaces) {
        s->nb_surfaces_used++;
        return av_buffer_create((uint8_t*)(s->surfaces_internal + s->nb_surfaces_used - 1),
                                sizeof(*frames_hwctx->surfaces), NULL, NULL, 0);
    }

    return NULL;
}

static int ni_init_surface(AVHWFramesContext *ctx, niFrameSurface1_t *surf)
{
    /* Fill with dummy values. This data is never used. */
    surf->ui16FrameIdx    = 0;
    surf->ui16session_ID  = 0;
    surf->ui32nodeAddress = 0;
    surf->device_handle   = 0;
    surf->bit_depth       = 0;
    surf->encoding_type   = 0;
    surf->output_idx      = 0;
    surf->src_cpu         = 0;

    return 0;
}

static int ni_init_pool(AVHWFramesContext *ctx)
{
    NIFramesContext              *s = ctx->internal->priv; //NI
    AVNIFramesContext *frames_hwctx = ctx->hwctx;          //NI
    int i, ret;

    av_log(ctx, AV_LOG_ERROR, "ctx->initial_pool_size = %d\n", ctx->initial_pool_size);

    if (ctx->initial_pool_size <= 0) {
        av_log(ctx, AV_LOG_ERROR, "NI requires a fixed frame pool size\n");
        return AVERROR(EINVAL);
    }

    s->surfaces_internal = av_calloc(ctx->initial_pool_size,
                                     sizeof(*s->surfaces_internal));
    if (!s->surfaces_internal) {
        return AVERROR(ENOMEM);
    }

    for (i = 0; i < ctx->initial_pool_size; i++) {
        ret = ni_init_surface(ctx, &s->surfaces_internal[i]);
        if (ret < 0) {
            return ret;
        }
    }

    ctx->internal->pool_internal = av_buffer_pool_init2(sizeof(niFrameSurface1_t),
                                                        ctx, ni_pool_alloc, NULL);
    if (!ctx->internal->pool_internal) {
        return AVERROR(ENOMEM);
    }

    frames_hwctx->surfaces = s->surfaces_internal;
    frames_hwctx->nb_surfaces = ctx->initial_pool_size;

    return 0;
}

static int ni_init_internal_session(AVHWFramesContext *ctx)
{
    NIFramesContext              *s = ctx->internal->priv;
    ni_log_set_level(ff_to_ni_log_level(av_log_get_level()));
    av_log(ctx, AV_LOG_ERROR, "hwcontext_ni:ni_init_internal_session()\n");
    if (ni_device_session_context_init(&(s->api_ctx)) < 0) {
        av_log(ctx, AV_LOG_ERROR, "ni init context failure\n");
        return -1;
    }

    return 0;
}

static void init_split_rsrc(NIFramesContext *ni_ctx, int w, int h)
{
    int i;
    ni_split_context_t* p_split_ctx = &ni_ctx->split_ctx;
    memset(p_split_ctx, 0, sizeof(ni_split_context_t));
    for (i = 0; i < 3; i++)
    {
        ni_ctx->split_ctx.w[i] = w;
        ni_ctx->split_ctx.h[i] = h;
        ni_ctx->split_ctx.f[i] = -1;
    }
}

static int ni_frames_init(AVHWFramesContext *ctx) //hwupload runs this on hwupload_config_output
{
  NIFramesContext *ni_ctx = ctx->internal->priv;
  AVNIDeviceContext *device_hwctx = (AVNIDeviceContext *) ctx->device_ctx->hwctx;
  int linesize_aligned,height_aligned;
  int pool_size,ret;
  AVNIFramesContext *frames_hwctx = (AVNIFramesContext *)ctx->hwctx;

  av_log(ctx, AV_LOG_ERROR, "%s: Enter, supplied poolsize = %d, devid=%d\n",
         __func__, ctx->initial_pool_size, device_hwctx->uploader_ID);

  ni_ctx->suspended_device_handle = -1;
  ni_ctx->uploader_device_id = -2; // -1 auto load balance, default -2 invalid
  pool_size = ctx->initial_pool_size;
  if (device_hwctx->uploader_ID < -1) {
      if (pool_size > -1) // ffmpeg does not specify init_hw_device for decoder
                          // - so decoder device_hwctx->uploader_ID is always -1
      {
          av_log(ctx, AV_LOG_ERROR, "%s Error no uploader device selected!\n",
                 __func__);
          return AVERROR(EINVAL);
      }
  }

  ret = ni_init_internal_session(ctx);
  if (ret < 0) {
      return AVERROR(EINVAL);
  }

  init_split_rsrc(ni_ctx, ctx->width, ctx->height);
  if (pool_size <= -1) // None upload init returns here
  {
    av_log(ctx, AV_LOG_INFO, "%s: poolsize code %d, this code recquires no host pool\n",
           __func__, pool_size);
    return ret;
  }
  else if (pool_size == 0)
  {
    /*Kept in private NIFramesContext for future reference, the device context version gets overwritten*/
    ni_ctx->uploader_device_id = device_hwctx->uploader_ID; 
    pool_size = ctx->initial_pool_size = 3;
    av_log(ctx, AV_LOG_INFO, "%s: Pool_size autoset to %d\n", __func__, pool_size);
  }

  if ((ctx->width & 0x1) || (ctx->height & 0x1)) {
      av_log(ctx, AV_LOG_ERROR, "Odd resolution %dx%d not permitted\n",
             ctx->width, ctx->height);
      return AVERROR(EINVAL);
  }

  linesize_aligned = ctx->width;
  if (QUADRA)
  {
      linesize_aligned = NI_VPU_CEIL(linesize_aligned, 2);
  }
  else
  {
    linesize_aligned = ((ctx->width + 31) / 32) * 32;
    if (linesize_aligned < NI_MIN_WIDTH)
    {
        // p_param->cfg_enc_params.conf_win_right += NI_MIN_WIDTH - ctx->width;
        linesize_aligned = NI_MIN_WIDTH;
    }
    else if (linesize_aligned > ctx->width)
    {
        // p_param->cfg_enc_params.conf_win_right += linesize_aligned -
        // ctx->width;
    }
  }
  ctx->width = linesize_aligned;

  height_aligned = ctx->height;
  if (QUADRA)
  {
      ctx->height = NI_VPU_CEIL(height_aligned, 2);
  }
  else
  {
    height_aligned = ((ctx->height + 15) / 16) * 16;
    if (height_aligned < NI_MIN_HEIGHT)
    {
        // p_param->cfg_enc_params.conf_win_bottom += NI_MIN_HEIGHT -
        // ctx->height;
        ctx->height    = NI_MIN_HEIGHT;
        height_aligned = NI_MIN_HEIGHT;
    }
    else if (height_aligned > ctx->height)
    {
        // p_param->cfg_enc_params.conf_win_bottom += height_aligned -
        // ctx->height;
        ctx->height = height_aligned;
    }
  }

  ni_ctx->api_ctx.active_video_width = ctx->width;
  ni_ctx->api_ctx.active_video_height = ctx->height;

  switch (ctx->sw_format)
  {
    case AV_PIX_FMT_YUV420P:
      ni_ctx->api_ctx.bit_depth_factor = 1;
      ni_ctx->api_ctx.src_bit_depth = 8;
      ni_ctx->api_ctx.pixel_format = NI_PIX_FMT_YUV420P;
      break;
    case AV_PIX_FMT_YUV420P10LE:
      ni_ctx->api_ctx.bit_depth_factor = 2;
      ni_ctx->api_ctx.src_bit_depth = 10;
      ni_ctx->api_ctx.src_endian = NI_FRAME_LITTLE_ENDIAN;
      ni_ctx->api_ctx.pixel_format = NI_PIX_FMT_YUV420P10LE;
      break;
    case AV_PIX_FMT_NV12:
      ni_ctx->api_ctx.bit_depth_factor = 1;
      ni_ctx->api_ctx.src_bit_depth = 8;
      ni_ctx->api_ctx.pixel_format = NI_PIX_FMT_NV12;
      break;
    case AV_PIX_FMT_P010LE:
      ni_ctx->api_ctx.bit_depth_factor = 2;
      ni_ctx->api_ctx.src_bit_depth = 10;
      ni_ctx->api_ctx.pixel_format = NI_PIX_FMT_P010LE;
      ni_ctx->api_ctx.src_endian = NI_FRAME_LITTLE_ENDIAN;
      break;
    case AV_PIX_FMT_RGBA:
        ni_ctx->api_ctx.bit_depth_factor = 4;
        ni_ctx->api_ctx.src_bit_depth    = 32;
        ni_ctx->api_ctx.src_endian       = NI_FRAME_LITTLE_ENDIAN;
        ni_ctx->api_ctx.pixel_format     = NI_PIX_FMT_RGBA;
        break;
    case AV_PIX_FMT_BGRA:
        ni_ctx->api_ctx.bit_depth_factor = 4;
        ni_ctx->api_ctx.src_bit_depth    = 32;
        ni_ctx->api_ctx.src_endian       = NI_FRAME_LITTLE_ENDIAN;
        ni_ctx->api_ctx.pixel_format     = NI_PIX_FMT_BGRA;
        break;
    case AV_PIX_FMT_ABGR:
        ni_ctx->api_ctx.bit_depth_factor = 4;
        ni_ctx->api_ctx.src_bit_depth    = 32;
        ni_ctx->api_ctx.src_endian       = NI_FRAME_LITTLE_ENDIAN;
        ni_ctx->api_ctx.pixel_format     = NI_PIX_FMT_ABGR;
        break;
    case AV_PIX_FMT_ARGB:
        ni_ctx->api_ctx.bit_depth_factor = 4;
        ni_ctx->api_ctx.src_bit_depth    = 32;
        ni_ctx->api_ctx.src_endian       = NI_FRAME_LITTLE_ENDIAN;
        ni_ctx->api_ctx.pixel_format     = NI_PIX_FMT_ARGB;
        break;
    case AV_PIX_FMT_BGR0:
        ni_ctx->api_ctx.bit_depth_factor = 4;
        ni_ctx->api_ctx.src_bit_depth    = 32;
        ni_ctx->api_ctx.src_endian       = NI_FRAME_LITTLE_ENDIAN;
        ni_ctx->api_ctx.pixel_format     = NI_PIX_FMT_BGR0;
        break;
    case AV_PIX_FMT_BGRP:
        ni_ctx->api_ctx.bit_depth_factor = 1;
        ni_ctx->api_ctx.src_bit_depth    = 24;
        ni_ctx->api_ctx.src_endian       = NI_FRAME_LITTLE_ENDIAN;
        ni_ctx->api_ctx.pixel_format     = NI_PIX_FMT_BGRP;
        break;
    case AV_PIX_FMT_YUYV422:
        ni_ctx->api_ctx.bit_depth_factor = 1;
        ni_ctx->api_ctx.src_bit_depth    = 8;
        ni_ctx->api_ctx.src_endian       = NI_FRAME_LITTLE_ENDIAN;
        ni_ctx->api_ctx.pixel_format     = NI_PIX_FMT_YUYV422;
        break;
    case AV_PIX_FMT_UYVY422:
        ni_ctx->api_ctx.bit_depth_factor = 1;
        ni_ctx->api_ctx.src_bit_depth    = 8;
        ni_ctx->api_ctx.src_endian       = NI_FRAME_LITTLE_ENDIAN;
        ni_ctx->api_ctx.pixel_format     = NI_PIX_FMT_UYVY422;
        break;
    case AV_PIX_FMT_NV16:
        ni_ctx->api_ctx.bit_depth_factor = 1;
        ni_ctx->api_ctx.src_bit_depth    = 8;
        ni_ctx->api_ctx.src_endian       = NI_FRAME_LITTLE_ENDIAN;
        ni_ctx->api_ctx.pixel_format     = NI_PIX_FMT_NV16;
        break;
    default:
        av_log(ctx, AV_LOG_ERROR, "Pixel format not supported by device.\n");
        return AVERROR(EINVAL);
  }

  if (ctx->width > NI_MAX_RESOLUTION_WIDTH ||
    ctx->height > NI_MAX_RESOLUTION_HEIGHT ||
    ctx->width * ctx->height > NI_MAX_RESOLUTION_AREA)
  {
    av_log(ctx, AV_LOG_ERROR, "Error XCoder resolution %dx%d not supported\n",
      ctx->width, ctx->height);
    av_log(ctx, AV_LOG_ERROR, "Max Supported Width: %d Height %d Area %d\n",
      NI_MAX_RESOLUTION_WIDTH, NI_MAX_RESOLUTION_HEIGHT, NI_MAX_RESOLUTION_AREA);
    return AVERROR_EXTERNAL;
  } else if (ni_ctx->uploader_device_id >= -1) {
      // leave it to ni_device_session_open to handle uploader session open
      // based on api_ctx.hw_id set to proper value
  } else {
      av_log(ctx, AV_LOG_ERROR, "Error XCoder command line options");
      return AVERROR(EINVAL);
  }

  av_log(ctx, AV_LOG_VERBOSE,
         "pixel sw_format=%d width = %d height = %d outformat=%d "
         "uploader_device_id=%d\n",
         ctx->sw_format, ctx->width, ctx->height, ctx->format,
         ni_ctx->uploader_device_id);

  ni_ctx->api_ctx.hw_id = ni_ctx->uploader_device_id;
  ni_ctx->api_ctx.keep_alive_timeout = frames_hwctx->keep_alive_timeout;
  if (0 == ni_ctx->api_ctx.keep_alive_timeout )
  {
    ni_ctx->api_ctx.keep_alive_timeout = NI_DEFAULT_KEEP_ALIVE_TIMEOUT;
  }
  ret = ni_device_session_open(&ni_ctx->api_ctx, NI_DEVICE_TYPE_UPLOAD);
  if (ret < 0)
  {
    av_log(ctx, AV_LOG_ERROR, "Error Something wrong in xcoder open\n");
    ni_frames_uninit(ctx);
    return AVERROR_EXTERNAL;
  }
  else
  {
      av_log(ctx, AV_LOG_VERBOSE,
             "XCoder %s.%d (inst: %d) opened successfully\n",
             ni_ctx->api_ctx.dev_xcoder_name, ni_ctx->api_ctx.hw_id,
             ni_ctx->api_ctx.session_id);
  }
  memset(&ni_ctx->src_session_io_data, 0, sizeof(ni_session_data_io_t));
  ret = ni_device_session_init_framepool(&ni_ctx->api_ctx, pool_size, 0);
  if (ret < 0)
  {
    return ret;
  }

  if (!ctx->pool) {
    ret = ni_init_pool(ctx);
    if (ret < 0) {
      av_log(ctx, AV_LOG_ERROR, "Error creating an internal frame pool\n");
      return ret;
    }
  }
  return 0;
}

static int ni_to_avframe_copy(AVHWFramesContext *hwfc, AVFrame *dst,
                              const ni_frame_t *src) {
    int src_linesize[4], src_height[4];
    int i, h, nb_planes;
    uint8_t *src_line, *dst_line;

    nb_planes = av_pix_fmt_count_planes(hwfc->sw_format);

    switch (hwfc->sw_format) {
    case AV_PIX_FMT_YUV420P:
        src_linesize[0] = FFALIGN(dst->width, 128);
        src_linesize[1] = FFALIGN(dst->width / 2, 128);
        src_linesize[2] = src_linesize[1];
        src_linesize[3] = 0;

        src_height[0] = dst->height;
        src_height[1] = FFALIGN(dst->height, 2) / 2;
        src_height[2] = src_height[1];
        src_height[3] = 0;
        break;

    case AV_PIX_FMT_YUV420P10LE:
        src_linesize[0] = FFALIGN(dst->width * 2, 128);
        src_linesize[1] = FFALIGN(dst->width, 128);
        src_linesize[2] = src_linesize[1];
        src_linesize[3] = 0;

        src_height[0] = dst->height;
        src_height[1] = FFALIGN(dst->height, 2) / 2;
        src_height[2] = src_height[1];
        src_height[3] = 0;
        break;

    case AV_PIX_FMT_NV12:
        src_linesize[0] = FFALIGN(dst->width, 128);
        src_linesize[1] = FFALIGN(dst->width, 128);
        src_linesize[2] = 0;
        src_linesize[3] = 0;

        src_height[0] = dst->height;
        src_height[1] = FFALIGN(dst->height, 2) / 2;
        src_height[2] = 0;
        src_height[3] = 0;
        break;

    case AV_PIX_FMT_NV16:
        src_linesize[0] = dst->width;
        src_linesize[1] = dst->width;
        src_linesize[2] = 0;
        src_linesize[3] = 0;

        src_height[0] = dst->height;
        src_height[1] = dst->height;
        src_height[2] = 0;
        src_height[3] = 0;
        break;

    case AV_PIX_FMT_YUYV422:
    case AV_PIX_FMT_UYVY422:
        src_linesize[0] = FFALIGN(dst->width, 16) * 2;
        src_linesize[1] = 0;
        src_linesize[2] = 0;
        src_linesize[3] = 0;

        src_height[0] = dst->height;
        src_height[1] = 0;
        src_height[2] = 0;
        src_height[3] = 0;
        break;

    case AV_PIX_FMT_P010LE:
        src_linesize[0] = FFALIGN(dst->width * 2, 128);
        src_linesize[1] = FFALIGN(dst->width * 2, 128);
        src_linesize[2] = 0;
        src_linesize[3] = 0;

        src_height[0] = dst->height;
        src_height[1] = FFALIGN(dst->height, 2) / 2;
        src_height[2] = 0;
        src_height[3] = 0;
        break;

    case AV_PIX_FMT_RGBA:
    case AV_PIX_FMT_BGRA:
    case AV_PIX_FMT_ABGR:
    case AV_PIX_FMT_ARGB:
    case AV_PIX_FMT_BGR0:
        src_linesize[0] = FFALIGN(dst->width, 16) * 4;
        src_linesize[1] = 0;
        src_linesize[2] = 0;
        src_linesize[3] = 0;

        src_height[0] = dst->height;
        src_height[1] = 0;
        src_height[2] = 0;
        src_height[3] = 0;
        break;

    case AV_PIX_FMT_BGRP:
        src_linesize[0] = dst->width;
        src_linesize[1] = dst->width;
        src_linesize[2] = dst->width;
        src_linesize[3] = 0;

        src_height[0] = dst->height;
        src_height[1] = dst->height;
        src_height[2] = dst->height;
        src_height[3] = 0;
        break;

    default:
        av_log(hwfc, AV_LOG_ERROR, "Unsupported pixel format %s\n",
               av_get_pix_fmt_name(hwfc->sw_format));
        return AVERROR(EINVAL);
    }

    for (i = 0; i < nb_planes; i++) {
        dst_line = dst->data[i];
        src_line = src->p_data[i];

        for (h = 0; h < src_height[i]; h++) {
            memcpy(dst_line, src_line,
                   FFMIN(src_linesize[i], dst->linesize[i]));
            dst_line += dst->linesize[i];
            src_line += src_linesize[i];
        }
    }

    return 0;
}

static int av_to_niframe_copy(AVHWFramesContext *hwfc, const int dst_stride[4],
                              ni_frame_t *dst, const AVFrame *src) {
    int src_height[4], hpad[4], vpad[4];
    int i, j, h, nb_planes;
    uint8_t *src_line, *dst_line, YUVsample, *sample, *dest;
    uint16_t lastidx;
    bool tenBit;

    nb_planes = av_pix_fmt_count_planes(hwfc->sw_format);

    switch (src->format) {
    case AV_PIX_FMT_YUV420P:
        hpad[0] = FFMAX(dst_stride[0] - src->linesize[0], 0);
        hpad[1] = FFMAX(dst_stride[1] - src->linesize[1], 0);
        hpad[2] = FFMAX(dst_stride[2] - src->linesize[2], 0);
        hpad[3] = 0;

        src_height[0] = src->height;
        src_height[1] = FFALIGN(src->height, 2) / 2;
        src_height[2] = FFALIGN(src->height, 2) / 2;
        src_height[3] = 0;

        vpad[0] = FFALIGN(src_height[0], 2) - src_height[0];
        vpad[1] = FFALIGN(src_height[1], 2) - src_height[1];
        vpad[2] = FFALIGN(src_height[2], 2) - src_height[2];
        vpad[3] = 0;

        tenBit = false;
        break;

    case AV_PIX_FMT_YUV420P10LE:
        hpad[0] = FFMAX(dst_stride[0] - src->linesize[0], 0);
        hpad[1] = FFMAX(dst_stride[1] - src->linesize[1], 0);
        hpad[2] = FFMAX(dst_stride[2] - src->linesize[2], 0);
        hpad[3] = 0;

        src_height[0] = src->height;
        src_height[1] = FFALIGN(src->height, 2) / 2;
        src_height[2] = FFALIGN(src->height, 2) / 2;
        src_height[3] = 0;

        vpad[0] = FFALIGN(src_height[0], 2) - src_height[0];
        vpad[1] = FFALIGN(src_height[1], 2) - src_height[1];
        vpad[2] = FFALIGN(src_height[2], 2) - src_height[2];
        vpad[3] = 0;

        tenBit = true;
        break;

    case AV_PIX_FMT_NV12:
        hpad[0] = FFMAX(dst_stride[0] - src->linesize[0], 0);
        hpad[1] = FFMAX(dst_stride[1] - src->linesize[1], 0);
        hpad[2] = 0;
        hpad[3] = 0;

        src_height[0] = src->height;
        src_height[1] = FFALIGN(src->height, 2) / 2;
        src_height[2] = 0;
        src_height[3] = 0;

        vpad[0] = FFALIGN(src_height[0], 2) - src_height[0];
        vpad[1] = FFALIGN(src_height[1], 2) - src_height[1];
        vpad[2] = 0;
        vpad[3] = 0;

        tenBit = false;
        break;
    case AV_PIX_FMT_NV16:
        hpad[0] = 0;
        hpad[1] = 0;
        hpad[2] = 0;
        hpad[3] = 0;

        src_height[0] = src->height;
        src_height[1] = src->height;
        src_height[2] = 0;
        src_height[3] = 0;

        vpad[0] = 0;
        vpad[1] = 0;
        vpad[2] = 0;
        vpad[3] = 0;

        tenBit = false;
        break;

    case AV_PIX_FMT_P010LE:
        hpad[0] = FFMAX(dst_stride[0] - src->linesize[0], 0);
        hpad[1] = FFMAX(dst_stride[1] - src->linesize[1], 0);
        hpad[2] = 0;
        hpad[3] = 0;

        src_height[0] = src->height;
        src_height[1] = FFALIGN(src->height, 2) / 2;
        src_height[2] = 0;
        src_height[3] = 0;

        vpad[0] = FFALIGN(src_height[0], 2) - src_height[0];
        vpad[1] = FFALIGN(src_height[1], 2) - src_height[1];
        vpad[2] = 0;
        vpad[3] = 0;

        tenBit = true;
        break;

    case AV_PIX_FMT_RGBA:
    case AV_PIX_FMT_BGRA:
    case AV_PIX_FMT_ABGR:
    case AV_PIX_FMT_ARGB:
    case AV_PIX_FMT_BGR0:
        hpad[0] = FFMAX(dst_stride[0] - src->linesize[0], 0);
        hpad[1] = 0;
        hpad[2] = 0;
        hpad[3] = 0;

        src_height[0] = src->height;
        src_height[1] = 0;
        src_height[2] = 0;
        src_height[3] = 0;

        vpad[0] = 0;
        vpad[1] = 0;
        vpad[2] = 0;
        vpad[3] = 0;

        tenBit = false;
        break;

    case AV_PIX_FMT_BGRP:
        hpad[0] = 0;
        hpad[1] = 0;
        hpad[2] = 0;
        hpad[3] = 0;

        src_height[0] = src->height;
        src_height[1] = src->height;
        src_height[2] = src->height;
        src_height[3] = 0;

        vpad[0] = 0;
        vpad[1] = 0;
        vpad[2] = 0;
        vpad[3] = 0;

        tenBit = false;
        break;

    case AV_PIX_FMT_YUYV422:
    case AV_PIX_FMT_UYVY422:
        hpad[0] = FFMAX(dst_stride[0] - src->linesize[0], 0);
        hpad[1] = 0;
        hpad[2] = 0;
        hpad[3] = 0;

        src_height[0] = src->height;
        src_height[1] = 0;
        src_height[2] = 0;
        src_height[3] = 0;

        vpad[0] = 0;
        vpad[1] = 0;
        vpad[2] = 0;
        vpad[3] = 0;

        tenBit = false;
        break;

    default:
        av_log(hwfc, AV_LOG_ERROR, "Pixel format %s not supported\n",
               av_get_pix_fmt_name(src->format));
        break;
    }

    for (i = 0; i < nb_planes; i++) {
        dst_line = dst->p_data[i];
        src_line = src->data[i];

        for (h = 0; h < src_height[i]; h++) {
            memcpy(dst_line, src_line, FFMIN(src->linesize[i], dst_stride[i]));

            if (hpad[i]) {
                lastidx = src->linesize[i];

                if (tenBit) {
                    sample = &src_line[lastidx - 2];
                    dest   = &dst_line[lastidx];

                    /* two bytes per sample */
                    for (j = 0; j < hpad[i] / 2; j++) {
                        memcpy(dest, sample, 2);
                        dest += 2;
                    }
                } else {
                    YUVsample = dst_line[lastidx - 1];
                    memset(&dst_line[lastidx], YUVsample, hpad[i]);
                }
            }

            src_line += src->linesize[i];
            dst_line += dst_stride[i];
        }

        /* Extend the height by cloning the last line */
        src_line = dst_line - dst_stride[i];
        for (h = 0; h < vpad[i]; h++) {
            memcpy(dst_line, src_line, dst_stride[i]);
            dst_line += dst_stride[i];
        }
    }

    return 0;
}

static int ni_hwdl_frame(AVHWFramesContext *hwfc, AVFrame *dst,
                         const AVFrame *src) {
    NIFramesContext *ctx = hwfc->internal->priv;
    ni_session_data_io_t session_io_data;
    ni_session_data_io_t *p_session_data = &session_io_data;
    niFrameSurface1_t *src_surf          = (niFrameSurface1_t *)src->data[3];
    int ret;
    int pixel_format;

    memset(&session_io_data, 0, sizeof(ni_session_data_io_t));

    av_log(hwfc, AV_LOG_VERBOSE,
           "%s handle %d trace ui16FrameIdx = [%d] SID %d\n", __func__,
           src_surf->device_handle, src_surf->ui16FrameIdx,
           src_surf->ui16session_ID);

    av_log(hwfc, AV_LOG_DEBUG, "%s hwdl processed h/w = %d/%d\n", __func__,
           src->height, src->width);

    switch (hwfc->sw_format) {
    case AV_PIX_FMT_YUV420P:
        pixel_format = NI_PIX_FMT_YUV420P;
        break;
    case AV_PIX_FMT_YUV420P10LE:
        pixel_format = NI_PIX_FMT_YUV420P10LE;
        break;
    case AV_PIX_FMT_NV12:
        pixel_format = NI_PIX_FMT_NV12;
        break;
    case AV_PIX_FMT_NV16:
        pixel_format = NI_PIX_FMT_NV16;
        break;
    case AV_PIX_FMT_YUYV422:
        pixel_format = NI_PIX_FMT_YUYV422;
        break;
    case AV_PIX_FMT_UYVY422:
        pixel_format = NI_PIX_FMT_UYVY422;
        break;
    case AV_PIX_FMT_P010LE:
        pixel_format = NI_PIX_FMT_P010LE;
        break;
    case AV_PIX_FMT_RGBA:
        pixel_format = NI_PIX_FMT_RGBA;
        break;
    case AV_PIX_FMT_BGRA:
        pixel_format = NI_PIX_FMT_BGRA;
        break;
    case AV_PIX_FMT_ABGR:
        pixel_format = NI_PIX_FMT_ABGR;
        break;
    case AV_PIX_FMT_ARGB:
        pixel_format = NI_PIX_FMT_ARGB;
        break;
    case AV_PIX_FMT_BGR0:
        pixel_format = NI_PIX_FMT_BGR0;
        break;
    case AV_PIX_FMT_BGRP:
        pixel_format = NI_PIX_FMT_BGRP;
        break;
    default:
        av_log(hwfc, AV_LOG_ERROR, "Pixel format not supported.\n");
        return AVERROR(EINVAL);
    }

    ret = ni_frame_buffer_alloc_dl(&(p_session_data->data.frame), src->width,
                                   src->height, pixel_format);
    if (ret != NI_RETCODE_SUCCESS) {
        av_log(hwfc, AV_LOG_ERROR, "%s Cannot allocate ni_frame\n", __func__);
        return AVERROR(ENOMEM);
    }

    ctx->api_ctx.is_auto_dl = false;
    ret = ni_device_session_hwdl(&ctx->api_ctx, p_session_data, src_surf);
    if (ret <= 0) {
        av_log(hwfc, AV_LOG_DEBUG, "%s failed to retrieve frame\n", __func__);
        ni_frame_buffer_free(&p_session_data->data.frame);
        return AVERROR_EXTERNAL;
    }

    ret = ni_to_avframe_copy(hwfc, dst, &p_session_data->data.frame);
    if (ret < 0) {
        av_log(hwfc, AV_LOG_ERROR, "Can't copy frame %d\n", ret);
        ni_frame_buffer_free(&p_session_data->data.frame);
        return ret;
    }

    dst->format = hwfc->sw_format;

    av_frame_copy_props(dst, src);
    ni_frame_buffer_free(&p_session_data->data.frame);

    return 0;
}

static int ni_hwup_frame(AVHWFramesContext *hwfc, AVFrame *dst, const AVFrame *src)
{
    NIFramesContext *ctx = hwfc->internal->priv;
    ni_session_data_io_t *p_src_session_data;
    niFrameSurface1_t *dst_surf;
    AVFrame *src_frame;
    int ret = 0;
    int dst_stride[4];
    int pixel_format;
    bool isSemiPlanar;

    dst_surf = (niFrameSurface1_t *)dst->data[3];

    if (dst_surf == NULL || dst->hw_frames_ctx == NULL) {
        av_log(hwfc, AV_LOG_ERROR, "Invalid hw frame\n");
        return AVERROR(EINVAL);
    }

    // memset(&ctx->src_session_io_data, 0, sizeof(ni_session_data_io_t));
    p_src_session_data = &ctx->src_session_io_data;

    switch (src->format) {
    /* 8-bit YUV420 planar */
    case AV_PIX_FMT_YUV420P:
        dst_stride[0] = FFALIGN(src->width, 128);
        dst_stride[1] = FFALIGN((src->width / 2), 128);
        dst_stride[2] = dst_stride[1];
        dst_stride[3] = 0;

        pixel_format = NI_PIX_FMT_YUV420P;
        isSemiPlanar = false;
        break;

    /* 10-bit YUV420 planar, little-endian, least significant bits */
    case AV_PIX_FMT_YUV420P10LE:
        dst_stride[0] = FFALIGN(src->width * 2, 128);
        dst_stride[1] = FFALIGN(src->width, 128);
        dst_stride[2] = dst_stride[1];
        dst_stride[3] = 0;

        pixel_format = NI_PIX_FMT_YUV420P10LE;
        isSemiPlanar = false;
        break;

    /* 8-bit YUV420 semi-planar */
    case AV_PIX_FMT_NV12:
        dst_stride[0] = FFALIGN(src->width, 128);
        dst_stride[1] = dst_stride[0];
        dst_stride[2] = 0;
        dst_stride[3] = 0;

        pixel_format = NI_PIX_FMT_NV12;
        isSemiPlanar = true;
        break;

    /* 8-bit yuv422 semi-planar */
    case AV_PIX_FMT_NV16:
        dst_stride[0] = src->width;
        dst_stride[1] = dst_stride[0];
        dst_stride[2] = 0;
        dst_stride[3] = 0;

        pixel_format = NI_PIX_FMT_NV16;
        isSemiPlanar = false;
        break;

    /*8-bit yuv422 planar */
    case AV_PIX_FMT_YUYV422:
        dst_stride[0] = FFALIGN(src->width, 16) * 2;
        dst_stride[1] = 0;
        dst_stride[2] = 0;
        dst_stride[3] = 0;

        pixel_format = NI_PIX_FMT_YUYV422;
        isSemiPlanar = false;
        break;

    case AV_PIX_FMT_UYVY422:
        dst_stride[0] = FFALIGN(src->width, 16) * 2;
        dst_stride[1] = 0;
        dst_stride[2] = 0;
        dst_stride[3] = 0;

        pixel_format = NI_PIX_FMT_UYVY422;
        isSemiPlanar = false;
        break;

    /* 10-bit YUV420 semi-planar, little endian, most significant bits */
    case AV_PIX_FMT_P010LE:
        dst_stride[0] = FFALIGN(src->width * 2, 128);
        dst_stride[1] = dst_stride[0];
        dst_stride[2] = 0;
        dst_stride[3] = 0;

        pixel_format = NI_PIX_FMT_P010LE;
        isSemiPlanar = true;
        break;

    /* 32-bit RGBA packed */
    case AV_PIX_FMT_RGBA:
        /* RGBA for the scaler has a 16-byte width/64-byte stride alignment */
        dst_stride[0] = FFALIGN(src->width, 16) * 4;
        dst_stride[1] = 0;
        dst_stride[2] = 0;
        dst_stride[3] = 0;

        pixel_format = NI_PIX_FMT_RGBA;
        isSemiPlanar = false;
        break;

    case AV_PIX_FMT_BGRA:
        dst_stride[0] = FFALIGN(src->width, 16) * 4;
        dst_stride[1] = 0;
        dst_stride[2] = 0;
        dst_stride[3] = 0;

        pixel_format = NI_PIX_FMT_BGRA;
        isSemiPlanar = false;
        break;

    case AV_PIX_FMT_ABGR:
        dst_stride[0] = FFALIGN(src->width, 16) * 4;
        dst_stride[1] = 0;
        dst_stride[2] = 0;
        dst_stride[3] = 0;

        pixel_format = NI_PIX_FMT_ABGR;
        isSemiPlanar = false;
        break;

    case AV_PIX_FMT_ARGB:
        dst_stride[0] = FFALIGN(src->width, 16) * 4;
        dst_stride[1] = 0;
        dst_stride[2] = 0;
        dst_stride[3] = 0;

        pixel_format = NI_PIX_FMT_ARGB;
        isSemiPlanar = false;
        break;

    case AV_PIX_FMT_BGR0:
        dst_stride[0] = FFALIGN(src->width, 16) * 4;
        dst_stride[1] = 0;
        dst_stride[2] = 0;
        dst_stride[3] = 0;

        pixel_format = NI_PIX_FMT_BGR0;
        isSemiPlanar = false;
        break;

    /* 24-bit BGR planar not supported */
    /*
    case AV_PIX_FMT_BGRP:
        dst_stride[0] = src->width;
        dst_stride[1] = src->width;
        dst_stride[2] = src->width;
        dst_stride[3] = 0;

        pixel_format = NI_PIX_FMT_BGRP;
        isSemiPlanar = false;
        break;
    */
    default:
        av_log(hwfc, AV_LOG_ERROR, "Pixel format %s not supported by device %s\n",
               av_get_pix_fmt_name(src->format), hwfc->internal->hw_type->name);
        return AVERROR(EINVAL);
    }

    if (!p_src_session_data->data.frame.p_buffer) {
        // allocate only once per upload Session when we have frame info
        p_src_session_data->data.frame.extra_data_len =
            NI_APP_ENC_FRAME_META_DATA_SIZE;

        ret = ni_frame_buffer_alloc_pixfmt(&p_src_session_data->data.frame,
                                           pixel_format, src->width,
                                           src->height, dst_stride,
                                           1, // force to av_codec_id_h264 for max compat
                                           (int)p_src_session_data->data.frame.extra_data_len);
        if (ret < 0) {
            av_log(hwfc, AV_LOG_ERROR, "Cannot allocate ni_frame %d\n", ret);
            return ret;
        }
    }

    ret = av_to_niframe_copy(hwfc, dst_stride, &p_src_session_data->data.frame, src);
    if (ret < 0) {
        av_log(hwfc, AV_LOG_ERROR, "%s can't copy frame\n", __func__);
        return AVERROR(EINVAL);
    }

    ret = ni_device_session_hwup(&ctx->api_ctx, p_src_session_data, dst_surf);
    if (ret < 0) {
        av_log(hwfc, AV_LOG_ERROR, "%s failed to upload frame %d\n",
               __func__, ret);
        return AVERROR_EXTERNAL;
    }

    dst_surf->ui16width = ctx->split_ctx.w[0] = src->width;
    dst_surf->ui16height = ctx->split_ctx.h[0] = src->height;
    dst_surf->ui32nodeAddress = 0; // always 0 offset for upload
    dst_surf->encoding_type = isSemiPlanar ? NI_PIXEL_PLANAR_FORMAT_SEMIPLANAR
                                           : NI_PIXEL_PLANAR_FORMAT_PLANAR;

    av_log(hwfc, AV_LOG_VERBOSE, "%s trace ui16FrameIdx = [%u] hdl %d SID%d\n",
           __func__, dst_surf->ui16FrameIdx, dst_surf->device_handle,
           dst_surf->ui16session_ID);

    // Update frames context
    ctx->split_ctx.f[0] = (int)dst_surf->encoding_type;

    /* Remove the const qualifier */
    src_frame         = (AVFrame *)src;
    // NOLINTNEXTLINE(clang-diagnostic-int-to-void-pointer-cast)
    src_frame->opaque = (void *)ctx->api_ctx.hw_id;

    av_frame_copy_props(dst, src); // should get the metadata right
    av_log(hwfc, AV_LOG_DEBUG, "%s Upload frame w/h %d/%d crop r/b %lu/%lu\n",
           __func__, dst->width, dst->height, dst->crop_right, dst->crop_bottom);

    return ret;
}

static int ni_transfer_data_to(AVHWFramesContext *hwfc, AVFrame *dst,
                               const AVFrame *src)
{
    int err;
    niFrameSurface1_t *dst_surf;

    if (src->width > hwfc->width || src->height > hwfc->height) {
        return AVERROR(EINVAL);
    }

    /* should check against MAX frame size */
    err = ni_hwup_frame(hwfc, dst, src);
    if (err) {
        return err;
    }

    dst_surf = (niFrameSurface1_t *)(dst->data[3]);

    av_log(hwfc, AV_LOG_VERBOSE,
           "hwcontext.c:ni_hwup_frame() dst_surf FID %d %d\n",
           dst_surf->ui16FrameIdx, dst_surf->ui16session_ID);

    return 0;
}

static int ni_transfer_data_from(AVHWFramesContext *hwfc, AVFrame *dst,
                                 const AVFrame *src)
{
    if (dst->width > hwfc->width || dst->height > hwfc->height) {
        av_log(hwfc, AV_LOG_ERROR, "Invalid frame dimensions\n");
        return AVERROR(EINVAL);
    }

    return ni_hwdl_frame(hwfc, dst, src);
}

const HWContextType ff_hwcontext_type_ni_quadra = {
    // QUADRA
    .type = AV_HWDEVICE_TYPE_NI_QUADRA,
    .name = "NI_QUADRA",

    .device_hwctx_size = sizeof(AVNIDeviceContext),
    .frames_hwctx_size = sizeof(AVNIFramesContext),
    .frames_priv_size  = sizeof(NIFramesContext),

    .device_create = ni_device_create,
    .device_uninit = ni_device_uninit,

    .frames_get_constraints = ni_frames_get_constraints,

    .frames_init   = ni_frames_init,
    .frames_uninit = ni_frames_uninit,

    .frames_get_buffer = ni_get_buffer,

    .transfer_get_formats = ni_transfer_get_formats,
    .transfer_data_to     = ni_transfer_data_to,
    .transfer_data_from   = ni_transfer_data_from,

    .pix_fmts =
        (const enum AVPixelFormat[]){AV_PIX_FMT_NI_QUAD, AV_PIX_FMT_NONE},
};
