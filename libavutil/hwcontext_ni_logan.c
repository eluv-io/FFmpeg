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
#if HAVE_UNISTD_H
#   include <unistd.h>
#endif


#include "avassert.h"
#include "buffer.h"
#include "common.h"
#include "hwcontext.h"
#include "hwcontext_internal.h"
#include "hwcontext_ni_logan.h"
#include "libavutil/imgutils.h"
#include "mem.h"
#include "pixdesc.h"
#include "pixfmt.h"
#include "ni_util_logan.h"

static enum AVPixelFormat supported_pixel_formats[] = {
  AV_PIX_FMT_YUV420P,
  AV_PIX_FMT_YUV420P10BE,
  AV_PIX_FMT_YUV420P10LE
};

typedef struct NILOGANDeviceContext {
  ni_device_handle_t  handle;
} NILOGANDeviceContext;

static inline void ni_logan_buffer_free(void *opaque, uint8_t *data)
{
  ni_logan_aligned_free(data);
}

static inline void ni_logan_frame_free(void *opaque, uint8_t *data)
{
  av_log(NULL, AV_LOG_VERBOSE, "ni_logan_frame_free\n");
  if (data)
  {
    ni_logan_session_context_t *p_ctx = (ni_logan_session_context_t *) opaque;
    ni_logan_hwframe_surface_t* p_data3 = (ni_logan_hwframe_surface_t *) data; //assuming for hwframes there is no data0,1,2?
    //TODO use int32t device_handle to kill the buffer!
    av_log(NULL, AV_LOG_VERBOSE, "ni_logan_frame_free:%d, %p\n", p_data3->i8FrameIdx, data);
    if (p_data3->i8FrameIdx != NI_LOGAN_INVALID_HW_FRAME_IDX)
    {
#ifdef _WIN32
      int64_t handle = (((int64_t) p_data3->device_handle_ext) << 32) | p_data3->device_handle;
      ni_logan_decode_buffer_free(p_data3, (ni_device_handle_t) handle, p_ctx->event_handle);
#else
      ni_logan_decode_buffer_free(p_data3, (ni_device_handle_t) p_data3->device_handle, p_ctx->event_handle);
#endif
    }
    free(data);
  }
}

static int ni_logan_device_create(AVHWDeviceContext *ctx,
                            const char *device,
                            AVDictionary *opts,
                            int flags)
{
  AVNILOGANDeviceContext *ni_logan_ctx;
  int ret = 0;

  av_log(ctx, AV_LOG_VERBOSE, "ni_logan_device_create %s\n", device);

  ni_logan_ctx = (AVNILOGANDeviceContext *) ctx->hwctx;
  ni_logan_ctx->device_idx = (int) NI_INVALID_DEVICE_HANDLE;
  if (device)
  {
    ni_logan_ctx->device_idx = atoi(device);
    if (ni_logan_ctx->device_idx < 0)
    {
      av_log(ctx, AV_LOG_ERROR, "ni_logan_device_create(): error device index = %d\n",
             ni_logan_ctx->device_idx);
      return AVERROR(EINVAL);
    }
  }

  return ret;
}

static int ni_logan_frames_get_constraints(AVHWDeviceContext *ctx,
                                     const void *hwconfig,
                                     AVHWFramesConstraints *constraints)
{
  int i;

  int num_pix_fmts_supported;

  num_pix_fmts_supported = FF_ARRAY_ELEMS(supported_pixel_formats);

  constraints->valid_sw_formats = av_malloc_array(num_pix_fmts_supported + 1,
                                  sizeof(*constraints->valid_sw_formats));

  if (!constraints->valid_sw_formats)
    return AVERROR(ENOMEM);

  for (i = 0; i < num_pix_fmts_supported; i++)
    constraints->valid_sw_formats[i] = supported_pixel_formats[i];

  constraints->valid_sw_formats[num_pix_fmts_supported] = AV_PIX_FMT_NONE;

  constraints->valid_hw_formats = av_malloc_array(2, sizeof(*constraints->valid_hw_formats));

  if (!constraints->valid_hw_formats)
    return AVERROR(ENOMEM);

  constraints->valid_hw_formats[0] = AV_PIX_FMT_NI_LOGAN;
  constraints->valid_hw_formats[1] = AV_PIX_FMT_NONE;

  return 0;
}

static int ni_logan_get_buffer(AVHWFramesContext *ctx, AVFrame *frame)
{
  int ret = 0, buf_size;
  uint8_t *buf;
  ni_logan_frame_t *xfme;
  NILOGANFramesContext *ni_logan_ctx = ctx->internal->priv;
  ni_logan_session_data_io_t dst_session_io_data = { 0 };
  ni_logan_session_data_io_t * p_dst_session_data = &dst_session_io_data;

  av_log(ctx, AV_LOG_TRACE, "ni_logan_get_buffer() enter\n");

  //alloc dest avframe buff
  ret = ni_logan_frame_buffer_alloc(&(p_dst_session_data->data.frame),
                              ctx->width,
                              ctx->height,
                              0,
                              1, //codec type does not matter, metadata exists
                              ni_logan_ctx->api_ctx.bit_depth_factor,
                              1);
  if (ret != 0)
    return AVERROR(ENOMEM);

  xfme = &(p_dst_session_data->data.frame);
  buf_size = xfme->data_len[0] + xfme->data_len[1] + xfme->data_len[2] + xfme->data_len[3];
  buf = xfme->p_data[0];
  memset(buf, 0, buf_size);
  frame->buf[0] = av_buffer_create(buf, buf_size, ni_logan_frame_free, &ni_logan_ctx->api_ctx, 0);
  buf = frame->buf[0]->data;
  if (!frame->buf[0])
    return AVERROR(ENOMEM);

  // init AVFrame
  frame->data[3] = (uint8_t*) xfme->p_buffer + xfme->data_len[0] + xfme->data_len[1] + xfme->data_len[2];
  ((ni_logan_hwframe_surface_t *)frame->data[3])->i8FrameIdx = NI_LOGAN_INVALID_HW_FRAME_IDX;
  frame->format = AV_PIX_FMT_NI_LOGAN;
  frame->width = ctx->width;
  frame->height = ctx->height;
  av_log(ctx, AV_LOG_TRACE, "ni_logan_get_buffer() exit\n");

  return 0;
}

static int ni_logan_transfer_get_formats(AVHWFramesContext *ctx,
                                   enum AVHWFrameTransferDirection dir,
                                   enum AVPixelFormat **formats)
{
  enum AVPixelFormat *fmts;

  fmts = av_malloc_array(2, sizeof(*fmts));
  if (!fmts)
    return AVERROR(ENOMEM);

  fmts[0] = ctx->sw_format;
  fmts[1] = AV_PIX_FMT_NONE;

  *formats = fmts;

  return 0;
}

static void ni_logan_frames_uninit(AVHWFramesContext *ctx)
{
  NILOGANFramesContext *s = ctx->internal->priv;
  NILOGANFramesContext *ni_logan_ctx = ctx->internal->priv;
  AVNILOGANDeviceContext * ni_logan_dev_ctx = ctx->device_ctx->hwctx;
  int dev_idx = ni_logan_dev_ctx->device_idx; //Supplied by init_hw_device ni=<name>:<id>

  av_log(ctx, AV_LOG_TRACE, "ni_logan_frames_uninit() :only close if upload instance, poolsize=%d devid=%d\n",
         ctx->initial_pool_size, dev_idx);
  if (dev_idx != -1)
  {
    av_log(ctx, AV_LOG_VERBOSE, "SessionID = %d!\n", ni_logan_ctx->api_ctx.session_id);
    if (ni_logan_ctx->api_ctx.session_id != 0) //assume 0 in invalid ID
    {
      ni_logan_device_session_close(&ni_logan_ctx->api_ctx, 1, NI_LOGAN_DEVICE_TYPE_UPLOAD);
    }
    //only upload frames init allocates these ones
    av_freep(&s->surface_ptrs);
    av_freep(&s->surfaces_internal);
    av_freep(&s->rsrc_ctx);
  }

  if (ni_logan_ctx->src_session_io_data)
  {
    if (ni_logan_ctx->src_session_io_data->data.frame.p_buffer)
    {
      av_log(ctx, AV_LOG_TRACE, "ni_logan_frames_uninit free p_buffer\n");
      ni_logan_frame_buffer_free(&ni_logan_ctx->src_session_io_data->data.frame);
    }
    av_freep(&ni_logan_ctx->src_session_io_data);
  }

  if (ni_logan_ctx->suspended_device_handle != NI_INVALID_DEVICE_HANDLE)
  {
    av_log(ctx, AV_LOG_TRACE, "ni_logan_frames_uninit(): close suspended device "
	   "handle, =%" SIZE_SPECIFIER "\n",
	   (int64_t) ni_logan_ctx->suspended_device_handle);
    ni_logan_device_close(ni_logan_ctx->suspended_device_handle);
  }

  ni_logan_device_session_context_clear(&s->api_ctx);
}

static AVBufferRef *ni_logan_pool_alloc(void *opaque, int size)
{
  AVHWFramesContext    *ctx = (AVHWFramesContext*)opaque;
  NILOGANFramesContext       *s = ctx->internal->priv;
  AVNILOGANFramesContext *frames_hwctx = ctx->hwctx;

  if (s->nb_surfaces_used < frames_hwctx->nb_surfaces) {
    s->nb_surfaces_used++;
    return av_buffer_create((uint8_t*)(s->surfaces_internal + s->nb_surfaces_used - 1),
                            sizeof(*frames_hwctx->surfaces), NULL, NULL, 0);
  }

  return NULL;
}

static int ni_logan_init_surface(AVHWFramesContext *ctx, ni_logan_hwframe_surface_t *surf)
{
  /* Fill with dummy values. This data is never used. */
  surf->i8FrameIdx        = NI_LOGAN_INVALID_HW_FRAME_IDX;
  surf->i8InstID          = 0;
  surf->ui16SessionID     = NI_LOGAN_INVALID_SESSION_ID;
  surf->device_handle     = NI_INVALID_DEVICE_HANDLE;
  surf->device_handle_ext = NI_INVALID_DEVICE_HANDLE;
  surf->bit_depth         = 0;
  surf->encoding_type     = 0;
  surf->seq_change        = 0;

  return 0;
}

static int ni_logan_init_pool(AVHWFramesContext *ctx)
{
  NILOGANFramesContext              *s = ctx->internal->priv; //NI
  AVNILOGANFramesContext *frames_hwctx = ctx->hwctx;          //NI

  int i, ret = 0;

  av_log(ctx, AV_LOG_VERBOSE, "ni_logan_init_pool() enter, pool_size=%d\n", ctx->initial_pool_size);
  if (ctx->initial_pool_size <= 0) {
    av_log(ctx, AV_LOG_ERROR, "ni_logan_init_pool(): NI requires a fixed frame pool size\n");
    return AVERROR(EINVAL);
  }

  s->surfaces_internal = av_mallocz_array(ctx->initial_pool_size,
                                          sizeof(*s->surfaces_internal));
  if (!s->surfaces_internal)
    return AVERROR(ENOMEM);

  for (i = 0; i < ctx->initial_pool_size; i++) {
    ret = ni_logan_init_surface(ctx, &s->surfaces_internal[i]);
    if (ret < 0)
      return ret;
  }

  ctx->internal->pool_internal = av_buffer_pool_init2(sizeof(ni_logan_hwframe_surface_t),
                                                      ctx, ni_logan_pool_alloc, NULL);
  if (!ctx->internal->pool_internal)
    return AVERROR(ENOMEM);

  frames_hwctx->surfaces = s->surfaces_internal;
  frames_hwctx->nb_surfaces = ctx->initial_pool_size;

  return 0;
}

static int ni_logan_init_internal_session(AVHWFramesContext *ctx)
{
  NILOGANFramesContext *s = ctx->internal->priv;
  ni_logan_session_context_t *p_ctx = &s->api_ctx;

  ni_logan_device_session_context_init(p_ctx);

#ifdef _WIN32
  p_ctx->event_handle = ni_logan_create_event();
  if (p_ctx->event_handle == NI_INVALID_EVENT_HANDLE)
  {
    return AVERROR(EINVAL);
  }

  p_ctx->thread_event_handle = ni_logan_create_event();
  if (p_ctx->thread_event_handle == NI_INVALID_EVENT_HANDLE)
  {
    return AVERROR(EINVAL);
  }
#endif

  return 0;
}

// hwupload runs this on hwupload_config_output
static int ni_logan_frames_init(AVHWFramesContext *ctx)
{
  NILOGANFramesContext *ni_logan_ctx = ctx->internal->priv;
  AVNILOGANDeviceContext *device_hwctx = (AVNILOGANDeviceContext *) ctx->device_ctx->hwctx;
  int dev_idx = device_hwctx->device_idx;
  int linesize_aligned,height_aligned;
  int pool_size,ret;
  ni_log_set_level(ff_to_ni_log_level(av_log_get_level()));

  av_log(ctx, AV_LOG_VERBOSE, "ni_logan_frames_init() enter, supplied poolsize = %d, dev_idx=%d\n",
         ctx->initial_pool_size, dev_idx);
  
  ni_logan_ctx->suspended_device_handle = NI_INVALID_DEVICE_HANDLE;
  pool_size = ctx->initial_pool_size;
  if (dev_idx == -1)
  {
    if (pool_size != -1) // ffmpeg does not sepcify init_hw_device for decoder - so decoder dev_dec_idx is always -1
    {
      av_log(ctx, AV_LOG_ERROR, "ni_logan_frames_init(): No device selected!\n");
      return AVERROR(EINVAL);
    }
  }

  ret = ni_logan_init_internal_session(ctx);
  if (ret < 0)
  {
    return ret;
  }
  
  if (pool_size == -1) // decoder returns here
  {
    // Init event handler for decoder since it does not invoke
    // ni_logan_device_session_open here but the event handler is necessary for
    // Windows event handle.
    av_log(ctx, AV_LOG_VERBOSE, "ni_logan_frames_init(): Invalid poolsize, assumed decoder mode\n");
    return ret;
  }
  else if (pool_size == 0)
  {
    uint32_t pixel_area = ctx->width * ctx->height * (1 + (AV_PIX_FMT_YUV420P10BE == ctx->sw_format)
                         + (AV_PIX_FMT_YUV420P10LE == ctx->sw_format));
    if (pixel_area < NI_LOGAN_NUM_OF_PIXELS_720P)
    {
      pool_size = ctx->initial_pool_size = 22;
    }
    else
    {
      pool_size = ctx->initial_pool_size = 20;
    }

    av_log(ctx, AV_LOG_VERBOSE, "ni_logan_frames_init(): Pool_size autoset to %d\n", pool_size);
  }

  linesize_aligned = ((ctx->width + 31) / 32) * 32;
  if (linesize_aligned < NI_LOGAN_MIN_WIDTH)
  {
    linesize_aligned = NI_LOGAN_MIN_WIDTH;
  }

  ctx->width = linesize_aligned;

  height_aligned = ((ctx->height + 15) / 16) * 16;
  if (height_aligned < NI_LOGAN_MIN_HEIGHT)
  {
    ctx->height = NI_LOGAN_MIN_HEIGHT;
    height_aligned = NI_LOGAN_MIN_HEIGHT;
  }
  else if (height_aligned > ctx->height)
  {
    ctx->height = height_aligned;
  }

  ni_logan_ctx->api_ctx.active_video_width = ctx->width;
  ni_logan_ctx->api_ctx.active_video_height = ctx->height;

  switch (ctx->sw_format)
  {
    case AV_PIX_FMT_YUV420P:
      ni_logan_ctx->api_ctx.bit_depth_factor = 1;
      ni_logan_ctx->api_ctx.src_bit_depth = 8;
      ni_logan_ctx->api_ctx.pixel_format = NI_LOGAN_PIX_FMT_YUV420P;
      break;
    case AV_PIX_FMT_YUV420P10LE:
      ni_logan_ctx->api_ctx.bit_depth_factor = 2;
      ni_logan_ctx->api_ctx.src_bit_depth = 10;
      ni_logan_ctx->api_ctx.src_endian = NI_LOGAN_FRAME_LITTLE_ENDIAN;
      ni_logan_ctx->api_ctx.pixel_format = NI_LOGAN_PIX_FMT_YUV420P10LE;
      break;
    default:
      return AVERROR(EINVAL);
  }

  if (ctx->width > NI_LOGAN_MAX_RESOLUTION_WIDTH ||
      ctx->height > NI_LOGAN_MAX_RESOLUTION_HEIGHT ||
      ctx->width * ctx->height > NI_LOGAN_MAX_RESOLUTION_AREA)
  {
    av_log(ctx, AV_LOG_ERROR, "ni_logan_frames_init(): Error XCoder resolution %dx%d not supported\n",
           ctx->width, ctx->height);
    av_log(ctx, AV_LOG_ERROR, "Max Supported Width: %d Height %d Area %d\n",
           NI_LOGAN_MAX_RESOLUTION_WIDTH, NI_LOGAN_MAX_RESOLUTION_HEIGHT, NI_LOGAN_MAX_RESOLUTION_AREA);
    return AVERROR_EXTERNAL;
  }
  else if (dev_idx >= 0)
  {
    /* allocate based on what user specifies */
    if ((ni_logan_ctx->rsrc_ctx = ni_logan_rsrc_allocate_simple_direct(NI_LOGAN_DEVICE_TYPE_DECODER, dev_idx)) == NULL)
    {
      av_log(ctx, AV_LOG_ERROR, "ni_logan_frames_init(): Error XCoder resource allocation: inst %d not available!\n",
             dev_idx);
      return AVERROR_EXTERNAL;
    }
  }
  else
  {
    av_log(ctx, AV_LOG_ERROR, "ni_logan_frames_init(): Error XCoder command line options");
    return AVERROR(EINVAL);
  }

  av_log(ctx, AV_LOG_VERBOSE, "ni_logan_frames_init(): pixel sw_format=%d width = %d height = %d outformat=%d\n",
         ctx->sw_format, ctx->width, ctx->height, ctx->format);

  ni_logan_ctx->api_ctx.hw_id = dev_idx;
  ret = ni_logan_device_session_open(&ni_logan_ctx->api_ctx, NI_LOGAN_DEVICE_TYPE_UPLOAD);
  if (ret < 0)
  {
    av_log(ctx, AV_LOG_ERROR, "ni_logan_frames_init(): Error Something wrong in xcoder open\n");
    ni_logan_frames_uninit(ctx);
    return AVERROR_EXTERNAL;
  }
  else
  {
    av_log(ctx, AV_LOG_VERBOSE, "ni_logan_frames_init(): XCoder %s.%d (inst: %d) opened successfully\n",
           ni_logan_ctx->rsrc_ctx->p_device_info->dev_name, dev_idx, ni_logan_ctx->api_ctx.session_id);
  }

  ret = ni_logan_device_session_init_framepool(&ni_logan_ctx->api_ctx, pool_size);
  if (ret < 0)
  {
    return ret;
  }

  // init src_session_io_data
  ni_logan_ctx->src_session_io_data =  malloc(sizeof(ni_logan_session_data_io_t));
  if(!ni_logan_ctx->src_session_io_data)
  {
    av_log(ctx, AV_LOG_ERROR, "ni_logan_frames_init(): Error alloc src_session_io_data memory\n");
    return AVERROR(ENOMEM);
  }
  memset(ni_logan_ctx->src_session_io_data, 0, sizeof(ni_logan_session_data_io_t));
  ni_logan_ctx->src_session_io_data->data.frame.extra_data_len = NI_LOGAN_APP_ENC_FRAME_META_DATA_SIZE;

  if (!ctx->pool) {
    ret = ni_logan_init_pool(ctx);
    if (ret < 0) {
      av_log(ctx, AV_LOG_ERROR, "ni_logan_frames_init(): Error creating an internal frame pool\n");
      return ret;
    }
  }
  return 0;
}

/*!******************************************************************************
*  \brief  Download frame from the NI devices
*
*  \param[in]   ctx    FFmpeg hardware frames context
*  \param[in]   dst    input hardware frames, fmt AV_PIX_FMT_NI_LOGAN.
*  \param[out]  src    output frames, fmt YUV420P, etc.
*
*  \return On success    0
*          On failure    <0
*******************************************************************************/
static int ni_logan_hwdl_frame(AVHWFramesContext *ctx, AVFrame *dst, const AVFrame *src)
{
  int ret = 0, buf_size;
  uint8_t *buf;
  ni_logan_frame_t *xfme;
  NILOGANFramesContext *ni_logan_ctx = ctx->internal->priv;
  ni_logan_session_data_io_t session_io_data = { 0 };
  ni_logan_session_data_io_t * p_session_data = &session_io_data;

  ni_logan_hwframe_surface_t* src_surf = (ni_logan_hwframe_surface_t*) src->data[3];
  av_log(ctx, AV_LOG_VERBOSE, "ni_logan_hwdl_frame(): dev_handle=%" SIZE_SPECIFIER ""
	 ", FrameIdx=%d, SessionID=%d\n",
         src_surf->device_handle,
         src_surf->i8FrameIdx,
         src_surf->ui16SessionID);

  av_log(ctx, AV_LOG_DEBUG, "ni_logan_hwdl_frame(): processed width=%d, height=%d\n",
         src->width, src->height);

  ret = ni_logan_frame_buffer_alloc(&(p_session_data->data.frame),
                              ni_logan_ctx->pc_width, ni_logan_ctx->pc_height,
                              src_surf->encoding_type == NI_LOGAN_CODEC_FORMAT_H264,
                              1,
                              src_surf->bit_depth,
                              0);
  
  if (NI_LOGAN_RETCODE_SUCCESS != ret)
  {
    return AVERROR_EXTERNAL;
  }

  ret = ni_logan_device_session_hwdl(&ni_logan_ctx->api_ctx, p_session_data, src_surf);
  if (ret <= 0)
  {
    av_log(ctx, AV_LOG_DEBUG, "ni_logan_hwdl_frame(): failed to retrieve frame\n");
    return AVERROR_EXTERNAL;
  }

  xfme = &(p_session_data->data.frame);
  buf_size = xfme->data_len[0] + xfme->data_len[1] + xfme->data_len[2];
  buf = xfme->p_data[0];
    
  dst->buf[0] = av_buffer_create(buf, buf_size, ni_logan_buffer_free, NULL, 0);
  buf = dst->buf[0]->data;
  if (!dst->buf[0])
    return AVERROR(ENOMEM);

  av_log(ctx, AV_LOG_DEBUG, "ni_logan_hwdl_frame(): fill array, linesize[0]=%d, fmt=%d, width=%d, height=%d\n",
         dst->linesize[0], ctx->sw_format, ni_logan_ctx->pc_width, ni_logan_ctx->pc_height);
  if ((ret = av_image_fill_arrays(dst->data, dst->linesize,
       buf, ctx->sw_format,
       ni_logan_ctx->pc_width,
       ni_logan_ctx->pc_height, 1)) < 0)
  {
    av_buffer_unref(&dst->buf[0]);
    return ret;
  }

  dst->format = ctx->sw_format;
  dst->width = ni_logan_ctx->pc_width;
  dst->height = ni_logan_ctx->pc_height;
  dst->crop_bottom = ni_logan_ctx->pc_crop_bottom;
  dst->crop_right = ni_logan_ctx->pc_crop_right;

  av_log(ctx, AV_LOG_DEBUG, "dst crop right %" SIZE_SPECIFIER " bot "
	 "%" SIZE_SPECIFIER " width %d height %d\n",
         dst->crop_right,
         dst->crop_bottom,
         dst->width,
         dst->height);
  av_frame_apply_cropping(dst, 0); //0 for simplicity
  av_log(ctx, AV_LOG_DEBUG, "POST dst crop right %" SIZE_SPECIFIER " bot"
	 "%" SIZE_SPECIFIER " width %d height %d\n",
         dst->crop_right,
         dst->crop_bottom,
         dst->width,
         dst->height);
  av_frame_copy_props(dst, src);//should about get the metadata right
  dst->format = ctx->sw_format;

  av_log(ctx, AV_LOG_VERBOSE, "ni_logan_hwdl_frame: frame->width=%d, frame->height=%d, "
         "crop top %" SIZE_SPECIFIER " bottom %" SIZE_SPECIFIER " "
         "left %" SIZE_SPECIFIER " right %" SIZE_SPECIFIER ", "
         "frame->format=%d, frame->linesize=%d/%d/%d\n",
         dst->width, dst->height,
         dst->crop_top, dst->crop_bottom,
         dst->crop_left, dst->crop_right,
         dst->format, dst->linesize[0], dst->linesize[1], dst->linesize[2]);

  return ret;
}

/*!******************************************************************************
*  \brief  Upload frame to the NI devices
*
*  \param[in]   ctx    FFmpeg hardware frames context
*  \param[in]   dst    input frames, fmt YUV420P, etc.
*  \param[out]  src    output hardware frames, fmt AV_PIX_FMT_NI_LOGAN.
*
*  \return On success    0
*          On failure    <0
*******************************************************************************/
static int ni_logan_hwup_frame(AVHWFramesContext *ctx, AVFrame *dst, const AVFrame *src)
{
  NILOGANFramesContext *ni_logan_ctx = ctx->internal->priv;
  int ret = 0;
  int dst_stride[4],plane_height[4];
  int height_aligned[4]; //HEIGHT padding for enc
  int padding_height;
  int pixel_format;
  int i,nb_planes=0;
  const AVPixFmtDescriptor *desc;
  ni_logan_session_data_io_t * p_src_session_data = ni_logan_ctx->src_session_io_data;
  ni_logan_hwframe_surface_t* dst_surf = (ni_logan_hwframe_surface_t*)dst->data[3];

  av_log(ctx, AV_LOG_VERBOSE, "ni_logan_hwup_frame() enter\n");
  if (!src)
  {
    av_log(ctx, AV_LOG_ERROR, "ni_logan_hwup_frame(): Src frame is empty! eof?\n");
  }

  if (ret < 0)
  {
    av_log(ctx, AV_LOG_ERROR, "ni_logan_hwup_frame(): failed to allocate surf\n");
    return AVERROR_EXTERNAL;
  }


  //alloc src avframe buff--------------
  switch (src->format)
  {
    case AV_PIX_FMT_YUV420P:
    case AV_PIX_FMT_YUV420P10LE:
      if (src->width < NI_LOGAN_MIN_WIDTH)
      {
        dst_stride[0] = FFALIGN(NI_LOGAN_MIN_WIDTH, 32) * ni_logan_ctx->api_ctx.bit_depth_factor;
      }
      else
      {
        dst_stride[0] = FFALIGN(src->width,32) * ni_logan_ctx->api_ctx.bit_depth_factor;
      }
      dst_stride[1] = dst_stride[2] = dst_stride[0]/2;
      height_aligned[0] = ((src->height + 7) / 8) * 8;
      if (1){//avctx->codec_id == AV_CODEC_ID_H264) {
          height_aligned[0] = ((src->height + 15) / 16) * 16; //force to this for max compat
      }
      if (height_aligned[0] < NI_LOGAN_MIN_HEIGHT)
      {
          height_aligned[0] = NI_LOGAN_MIN_HEIGHT;
      }
      height_aligned[1] = height_aligned[2] = height_aligned[0] / 2;
      break;
    default:
      av_log(ctx, AV_LOG_ERROR, "ni_logan_hwup_frame(): Error Pixel format %s not supported by device %s\n",
             av_get_pix_fmt_name(src->format),ctx->internal->hw_type->name);
      return AVERROR(EINVAL);
  }

  switch (ctx->sw_format)
  {
    case AV_PIX_FMT_YUV420P:
      pixel_format = NI_LOGAN_PIX_FMT_YUV420P;
      break;
    case AV_PIX_FMT_YUV420P10LE:
      pixel_format = NI_LOGAN_PIX_FMT_YUV420P10LE;
      break;
    default:
      av_log(ctx, AV_LOG_ERROR, "ni_logan_hwup_frame(): Error Pixel format %s not supported by devices %s\n",
             av_get_pix_fmt_name(src->format),ctx->internal->hw_type->name);
      return AVERROR(EINVAL);
  }

  ret = ni_logan_frame_buffer_alloc_v4(&(p_src_session_data->data.frame),
    pixel_format, src->width, src->height, dst_stride,
    1,
    p_src_session_data->data.frame.extra_data_len);

  if (ret < 0)
  {
    av_log(ctx, AV_LOG_ERROR, "ni_logan_hwup_frame(): Error Cannot allocate ni_logan_frame %d\n", ret);
    return ret;
  }

  if (!p_src_session_data->data.frame.p_data[0])
  {
    return AVERROR(ENOMEM);
  }

  switch (src->format)
  {
    case AV_PIX_FMT_YUV420P:
    case AV_PIX_FMT_YUV420P10LE:
      plane_height[0] = src->height;
      plane_height[1] = src->height / 2;
      plane_height[2] = src->height / 2;
      plane_height[3] = 0;
      break;
    default:
      av_log(ctx, AV_LOG_ERROR, "ni_logan_hwup_frame(): Error Pixel format %s not supported by device %s\n",
             av_get_pix_fmt_name(src->format),ctx->internal->hw_type->name);
      return AVERROR(EINVAL);
  }

  desc = av_pix_fmt_desc_get(src->format);
  for (i = 0; i < desc->nb_components; i++)
  {
    nb_planes = FFMAX(desc->comp[i].plane, nb_planes);
  }
  nb_planes++;

  av_log(ctx, AV_LOG_TRACE, "ni_logan_hwup_frame: src linesize = %d/%d/%d "
         "dst alloc linesize = %d/%d/%d  height = %d/%d/%d\n",
         src->linesize[0], src->linesize[1], src->linesize[2],
         dst_stride[0], dst_stride[1], dst_stride[2],
         plane_height[0], plane_height[1], plane_height[2]);

  for (i = 0; i < nb_planes; i++)
  {
    int height = plane_height[i];
    uint8_t *dest = p_src_session_data->data.frame.p_data[i];
    const uint8_t *srce = (const uint8_t *)src->data[i];
    for (; height > 0; height--)
    {
      memcpy(dest, srce, FFMIN(src->linesize[i], dst_stride[i]));
      dest += dst_stride[i];      
      srce += src->linesize[i];
    }

    // height padding if needed
    switch (src->format)
    {
      case AV_PIX_FMT_YUV420P:
      case AV_PIX_FMT_YUV420P10LE:
        /*
         * TODO: This should probably be removed for Quadra. NI_LOGAN_MIN_HEIGHT == 128
         *       is smaller than what Quadra can support. This looks like a T408
         *       requirement.
         */
        padding_height = height_aligned[i] - plane_height[i];
        break;
      default:
        av_log(ctx, AV_LOG_ERROR, "ni_logan_hwup_frame(): Error Pixel format %s not supported by device %s\n",
               av_get_pix_fmt_name(src->format),ctx->internal->hw_type->name);
        return AVERROR(EINVAL);
    }

    if (padding_height > 0)
    {
      av_log(ctx, AV_LOG_TRACE, "ni_logan_hwup_frame(): plane %d padding %d\n",
             i, padding_height);

      srce = dest - dst_stride[i];
      for (; padding_height > 0; padding_height--) {
        memcpy(dest, srce, dst_stride[i]);
        dest += dst_stride[i];
      }
    }
  }

  ret = ni_logan_device_session_hwup(&ni_logan_ctx->api_ctx, p_src_session_data, dst_surf);
  if (ret < 0)
  {
    av_log(ctx, AV_LOG_ERROR, "ni_logan_hwup_frame(): failed to upload frame\n");
    return AVERROR_EXTERNAL;
  }

  if (!dst->hw_frames_ctx)
  {
    dst->hw_frames_ctx = av_hwframe_ctx_alloc(ctx->device_ref);
    //((AVHWFramesContext*)dst->hw_frames_ctx->data)->format = AV_PIX_FMT_NI_LOGAN_QUAD;
  }
  av_log(ctx, AV_LOG_DEBUG, "ni_logan_hwup_frame(): Assigning hw_frames_ctx\n");
  ((AVHWFramesContext*)dst->hw_frames_ctx->data)->internal->priv = ni_logan_ctx;

  //set additional info to hwdesc
  dst_surf->ui16width = src->width;
  dst_surf->ui16height = src->height;
  //dst_surf->ui32nodeAddress = 0; //always 0 offset for upload
  //dst_surf->encoding_type = NI_LOGAN_PIXEL_PLANAR_FORMAT_PLANAR;
  ////Update frames context
  //ctx->f[0] = dst_surf->encoding_type;

  av_log(ctx, AV_LOG_VERBOSE, "ni_logan_hwup_frame(): dev_handle=%" SIZE_SPECIFIER ""
	 ", FrameIdx=%d, SessionID=%d\n",
         dst_surf->device_handle,
         dst_surf->i8FrameIdx,
         dst_surf->ui16SessionID);
  //Update frames context
  //ctx->split_ctx.f[0] = dst_surf->encoding_type;

  //av_log(ctx, AV_LOG_INFO, "original height width %d/%d, processed h/w = %d/%d\n",
  //    ctx->pc_height, ctx->pc_width, src->height, src->width);
  //
  //ret = ni_logan_frame_buffer_alloc(&(p_session_data->data.frame), ctx->pc_width, ctx->pc_height,
  //    src_surf->encoding_type, 1,
  //    src_surf->bit_depth,
  //    false);
  if (NI_LOGAN_RETCODE_SUCCESS != ret)
  {
    return AVERROR_EXTERNAL;
  }

  av_frame_copy_props(dst, src);//should about get the metadata right

  av_log(ctx, AV_LOG_DEBUG, "ni_logan_hwup_frame(): Upload frame w/h "
	 "%d/%d crop r/b %" SIZE_SPECIFIER "/%" SIZE_SPECIFIER "\n",
	 dst->width, dst->height, dst->crop_right, dst->crop_bottom);

  return ret;

}

static int ni_logan_transfer_data_to(AVHWFramesContext *ctx, AVFrame *dst,
                               const AVFrame *src)
{
  int ret = 0;

  av_log(ctx, AV_LOG_VERBOSE, "ni_logan_transfer_data_to() enter\n");

  if (src->width > ctx->width || src->height > ctx->height)
  {
    av_log(ctx, AV_LOG_ERROR, "ni_logan_transfer_data_to(): parameter error, dst=%dx%d, src=%dx%d\n",
           dst->width, dst->height, src->width, src->height);
    return AVERROR(EINVAL);
  }

  ret = ni_logan_hwup_frame(ctx, dst, src);
  if (ret < 0)
  {
    av_log(ctx, AV_LOG_ERROR, "ni_logan_transfer_data_to(): ni_logan_hwup_frame failed, ret=%d\n", ret);
    av_frame_free(&dst);
  }
  ret = 0;
  return ret;
}

static int ni_logan_transfer_data_from(AVHWFramesContext *ctx, AVFrame *dst,
                                 const AVFrame *src)
{
  AVFrame *map;
  int ret = 0;

  av_log(ctx, AV_LOG_VERBOSE, "ni_logan_transfer_data_from() enter\n");
  if (dst->width > ctx->width || dst->height > ctx->height)
  {
    av_log(ctx, AV_LOG_ERROR, "ni_logan_transfer_data_from(): parameter error, dst=%dx%d, src=%dx%d\n",
           dst->width, dst->height, src->width, src->height);
    return AVERROR(EINVAL);
  }

  map = av_frame_alloc();
  if (!map)
    return AVERROR(ENOMEM);
  
  ret = ni_logan_hwdl_frame(ctx, map, src);
  if (ret < 0)
  {
    av_log(ctx, AV_LOG_ERROR, "ni_logan_transfer_data_from(): ni_logan_hwdl_frame failed, ret=%d\n", ret);
    goto fail;
  }

  //Roy?
  if (dst->format != map->format)
  {
    av_log(ctx, AV_LOG_ERROR, "ni_logan_transfer_data_from(): format error, %d, %d\n",
           dst->format, map->format);
  }
  dst->format = map->format;
  ret = av_frame_copy(dst, map);

  if (ret < 0)
  {
    av_log(ctx, AV_LOG_ERROR, "ni_logan_transfer_data_from(): av_frame_copy failed, ret=%d\n", ret);
    goto fail;
  }
fail:
  av_frame_free(&map);
  return ret;
}

const HWContextType ff_hwcontext_type_ni_logan = {
  .type = AV_HWDEVICE_TYPE_NI_LOGAN,
  .name = "NI_LOGAN",

  .device_hwctx_size = sizeof(AVNILOGANDeviceContext),
  .device_priv_size  = sizeof(NILOGANDeviceContext),
  .frames_hwctx_size = sizeof(AVNILOGANFramesContext),
  .frames_priv_size  = sizeof(NILOGANFramesContext),

  .device_create = ni_logan_device_create,

  .frames_get_constraints = ni_logan_frames_get_constraints,

  .frames_init   = ni_logan_frames_init,
  .frames_uninit = ni_logan_frames_uninit,

  .frames_get_buffer = ni_logan_get_buffer,

  .transfer_get_formats = ni_logan_transfer_get_formats,
  .transfer_data_to     = ni_logan_transfer_data_to,
  .transfer_data_from   = ni_logan_transfer_data_from,

  .pix_fmts = (const enum AVPixelFormat[]) {
    AV_PIX_FMT_NI_LOGAN,
    AV_PIX_FMT_NONE
  },
};
