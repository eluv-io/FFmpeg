/*
 * NetInt XCoder H.264/HEVC Encoder common code
 * Copyright (c) 2018-2019 NetInt
 *
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
#include "nienc.h"
#include "bytestream.h"
#include "libavcodec/h264.h"
#include "libavcodec/h264_sei.h"
#include "libavcodec/hevc.h"
#include "libavcodec/hevc_sei.h"
#include "libavcodec/put_bits.h"
#include "libavutil/hdr_dynamic_metadata.h"
#include "libavutil/hwcontext.h"
#include "libavutil/hwcontext_internal.h"
#include "libavutil/hwcontext_ni_quad.h"
#include "libavutil/mastering_display_metadata.h"
#include "ni_av_codec.h"
#include "ni_util.h"
#include "put_bits.h"

#include <unistd.h>
#if ((LIBAVCODEC_VERSION_MAJOR >= 59 || LIBAVCODEC_VERSION_MAJOR >= 58 && LIBAVCODEC_VERSION_MINOR >= 134))
#include "encode.h"
#endif

static ni_enc_avc_roi_custom_map_t *g_avc_roi_map = NULL;
// H.265 test roi buffer for up to 8k resolution H.265 - 32 x 32 sub CTUs
static uint8_t *g_hevc_sub_ctu_roi_buf = NULL;
// H.265 custom map buffer for up to 8k resolution  - 64x64 CTU Regions
static ni_enc_hevc_roi_custom_map_t *g_hevc_roi_map = NULL;
// actual ROI map is stored in individual session context !

//extern const char * const g_xcoder_preset_names[3];
//extern const char * const g_xcoder_log_names[7];

// sequence change not working with MULTI_THREAD
// disable MULTI_THREAD because this is only needed for 8K parallel encode on 2 cards (4K each)
//#define MULTI_THREAD
#ifdef MULTI_THREAD
#undef MULIT_THREAD
#endif

#ifdef MULTI_THREAD
typedef struct _write_thread_arg_struct_t
{
  uint32_t session_id;
  pthread_t thread;
  XCoderH265EncContext *ctx;
  ni_retcode_t ret;
}write_thread_arg_struct_t;

static write_thread_arg_struct_t write_thread_args[NI_MAX_NUM_SESSIONS];

static uint8_t find_session_idx(uint32_t sid)
{
  uint8_t i;
  for (i = 0; i < NI_MAX_NUM_SESSIONS; i++)
  {
    if (write_thread_args[i].session_id == sid)
    {
      break;
    }
  }
  return i;
}
#endif

static int expand_ni_frame(AVCodecContext *avctx, ni_frame_t *dst,
                           const ni_frame_t *src, const int dst_stride[],
                           int raw_width, int raw_height,
                           enum AVPixelFormat format) {
    int i, j, h, nb_planes, tenBit;
    int vpad[3], hpad[3], src_height[3], src_width[3], src_stride[3];
    uint8_t *src_line, *dst_line, *sample, *dest, YUVsample;
    uint16_t lastidx;

    nb_planes = av_pix_fmt_count_planes(format);

    switch (format) {
    case AV_PIX_FMT_YUV420P:
        /* width of source frame for each plane in pixels */
        src_width[0] = FFALIGN(raw_width, 2);
        src_width[1] = FFALIGN(raw_width, 2) / 2;
        src_width[2] = FFALIGN(raw_width, 2) / 2;

        /* height of source frame for each plane in pixels */
        src_height[0] = FFALIGN(raw_height, 2);
        src_height[1] = FFALIGN(raw_height, 2) / 2;
        src_height[2] = FFALIGN(raw_height, 2) / 2;

        /* stride of source frame for each plane in bytes */
        src_stride[0] = FFALIGN(src_width[0], 128);
        src_stride[1] = FFALIGN(src_width[1], 128);
        src_stride[2] = FFALIGN(src_width[2], 128);

        tenBit = 0;

        /* horizontal padding needed for each plane in bytes */
        hpad[0] = dst_stride[0] - src_width[0];
        hpad[1] = dst_stride[1] - src_width[1];
        hpad[2] = dst_stride[2] - src_width[2];

        /* vertical padding needed for each plane in pixels */
        vpad[0] = NI_MIN_HEIGHT - src_height[0];
        vpad[1] = NI_MIN_HEIGHT / 2 - src_height[1];
        vpad[2] = NI_MIN_HEIGHT / 2 - src_height[2];

        break;

    case AV_PIX_FMT_YUV420P10LE:
        /* width of source frame for each plane in pixels */
        src_width[0] = FFALIGN(raw_width, 2);
        src_width[1] = FFALIGN(raw_width, 2) / 2;
        src_width[2] = FFALIGN(raw_width, 2) / 2;

        /* height of source frame for each plane in pixels */
        src_height[0] = FFALIGN(raw_height, 2);
        src_height[1] = FFALIGN(raw_height, 2) / 2;
        src_height[2] = FFALIGN(raw_height, 2) / 2;

        /* stride of source frame for each plane in bytes */
        src_stride[0] = FFALIGN(src_width[0] * 2, 128);
        src_stride[1] = FFALIGN(src_width[1] * 2, 128);
        src_stride[2] = FFALIGN(src_width[2] * 2, 128);

        tenBit = 1;

        /* horizontal padding needed for each plane in bytes */
        hpad[0] = dst_stride[0] - src_width[0] * 2;
        hpad[1] = dst_stride[1] - src_width[1] * 2;
        hpad[2] = dst_stride[2] - src_width[2] * 2;

        /* vertical padding needed for each plane in pixels */
        vpad[0] = NI_MIN_HEIGHT - src_height[0];
        vpad[1] = NI_MIN_HEIGHT / 2 - src_height[1];
        vpad[2] = NI_MIN_HEIGHT / 2 - src_height[2];

        break;

    case AV_PIX_FMT_NV12:
        /* width of source frame for each plane in pixels */
        src_width[0] = FFALIGN(raw_width, 2);
        src_width[1] = FFALIGN(raw_width, 2);
        src_width[2] = 0;

        /* height of source frame for each plane in pixels */
        src_height[0] = FFALIGN(raw_height, 2);
        src_height[1] = FFALIGN(raw_height, 2) / 2;
        src_height[2] = 0;

        /* stride of source frame for each plane in bytes */
        src_stride[0] = FFALIGN(src_width[0], 128);
        src_stride[1] = FFALIGN(src_width[1], 128);
        src_stride[2] = 0;

        tenBit = 0;

        /* horizontal padding needed for each plane in bytes */
        hpad[0] = dst_stride[0] - src_width[0];
        hpad[1] = dst_stride[1] - src_width[1];
        hpad[2] = 0;

        /* vertical padding for each plane in pixels */
        vpad[0] = NI_MIN_HEIGHT - src_height[0];
        vpad[1] = NI_MIN_HEIGHT / 2 - src_height[1];
        vpad[2] = 0;

        break;

    case AV_PIX_FMT_P010LE:
        /* width of source frame for each plane in pixels */
        src_width[0] = FFALIGN(raw_width, 2);
        src_width[1] = FFALIGN(raw_width, 2);
        src_width[2] = 0;

        /* height of source frame for each plane in pixels */
        src_height[0] = FFALIGN(raw_height, 2);
        src_height[1] = FFALIGN(raw_height, 2) / 2;
        src_height[2] = 0;

        /* stride of source frame for each plane in bytes */
        src_stride[0] = FFALIGN(src_width[0] * 2, 128);
        src_stride[1] = FFALIGN(src_width[1] * 2, 128);
        src_stride[2] = 0;

        tenBit = 1;

        /* horizontal padding needed for each plane in bytes */
        hpad[0] = dst_stride[0] - src_width[0] * 2;
        hpad[1] = dst_stride[1] - src_width[1] * 2;
        hpad[2] = 0;

        /* vertical padding for each plane in pixels */
        vpad[0] = NI_MIN_HEIGHT - src_height[0];
        vpad[1] = NI_MIN_HEIGHT / 2 - src_height[1];
        vpad[2] = 0;

        break;

    default:
        av_log(avctx, AV_LOG_ERROR, "Invalid pixel format %s\n",
               av_get_pix_fmt_name(format));
        return -1;
    }

    for (i = 0; i < nb_planes; i++) {
        dst_line = dst->p_data[i];
        src_line = src->p_data[i];

        for (h = 0; h < src_height[i]; h++) {
            memcpy(dst_line, src_line, src_width[i] * (tenBit + 1));

            /* Add horizontal padding */
            if (hpad[i]) {
                lastidx = src_width[i];

                if (tenBit) {
                    sample = &src_line[(lastidx - 1) * 2];
                    dest   = &dst_line[lastidx * 2];

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

            src_line += src_stride[i];
            dst_line += dst_stride[i];
        }

        /* Pad the height by duplicating the last line */
        src_line = dst_line - dst_stride[i];

        for (h = 0; h < vpad[i]; h++) {
            memcpy(dst_line, src_line, dst_stride[i]);
            dst_line += dst_stride[i];
        }
    }

    return 0;
}
#if 0
// convert FFmpeg ROIs to NetInt ROI map
static int set_roi_map(AVCodecContext *avctx, const AVFrameSideData *sd,
                       int nb_roi, int width, int height, int customMapSize)
{
  XCoderH265EncContext *ctx = avctx->priv_data;
  int i, j, k, m, r;
  const AVRegionOfInterest *roi = (const AVRegionOfInterest*)sd->data;
  uint32_t self_size = roi->self_size;
  int32_t set_qp = 0;
  uint32_t sumQp = 0;

  uint32_t max_cu_size = (avctx->codec_id == AV_CODEC_ID_H264) ? 16 : 64;

  // for H.264, select ROI Map Block Unit Size: 16x16
  // for H.265, select ROI Map Block Unit Size: 64x64
  uint32_t roiMapBlockUnitSize = (avctx->codec_id == AV_CODEC_ID_H264) ? 16 : 64;
  uint32_t mbWidth = ((width+max_cu_size-1)& (~(max_cu_size - 1))) / roiMapBlockUnitSize;
  uint32_t mbHeight = ((height+max_cu_size-1)& (~(max_cu_size - 1))) / roiMapBlockUnitSize;
  uint32_t numMbs = mbWidth * mbHeight;
  uint32_t subMbWidth = roiMapBlockUnitSize / 8;
  uint32_t subMbHeight = subMbWidth;
  uint32_t subNumMbs = subMbWidth * subMbHeight;

  // init ipcm_flag to 0, roiAbsQp_falg to 0 (qp delta), and qp_info to 0
  memset(g_quad_roi_map, 0, customMapSize);
#if 0
  for (i = 0; i < numMbs; i++)
  {
    for (j = 0; j < subNumMbs; j++)
    {
      g_quad_roi_map[i*subNumMbs+j].field.qp_info = NI_DEFAULT_INTRA_QP;
    }
  }
#endif

  // iterate ROI list from the last as regions are defined in order of
  // decreasing importance.
  for (r = nb_roi - 1; r >= 0; r--)
  {
    roi = (const AVRegionOfInterest*)(sd->data + self_size * r);
    if (! roi->qoffset.den)
    {
      av_log(avctx, AV_LOG_ERROR, "AVRegionOfInterest.qoffset.den "
             "must not be zero.\n");
      continue;
    }

    set_qp = (int32_t)((float)roi->qoffset.num * 1.0f /
                       (float)roi->qoffset.den * NI_INTRA_QP_RANGE);
    set_qp = av_clip(set_qp, NI_MIN_QP_DELTA, NI_MAX_QP_DELTA);
    // Adjust qp delta range (-25 to 25) to (0 to 63): 0 to 0, -1 to 1, -2 to 2 ... 1 to 63, 2 to 62 ...
    // Theoretically the possible qp delta range is (-32 to 31)
    set_qp = (NI_MAX_QP_INFO + 1 - set_qp ) % (NI_MAX_QP_INFO + 1);

    av_log(avctx, AV_LOG_INFO, "set_roi_map: left %d right %d top %d bottom %d num %d den %d set_qp %d\n",
            roi->left, roi->right, roi->top, roi->bottom, roi->qoffset.num, roi->qoffset.den, set_qp);

    // copy ROI MBs QPs into custom map
    for (j = 0; j < mbHeight; j++) {
        for (i = 0; i < mbWidth; i++) {
            k = j * (int)mbWidth + i;

            for (m = 0; m < subNumMbs; m++) {
                if (((int)(i % mbWidth) >=
                     (int)((roi->left + roiMapBlockUnitSize - 1) /
                           roiMapBlockUnitSize) -
                         1) &&
                    ((int)(i % mbWidth) <=
                     (int)((roi->right + roiMapBlockUnitSize - 1) /
                           roiMapBlockUnitSize) -
                         1) &&
                    ((int)(j % mbHeight) >=
                     (int)((roi->top + roiMapBlockUnitSize - 1) /
                           roiMapBlockUnitSize) -
                         1) &&
                    ((int)(j % mbHeight) <=
                     (int)((roi->bottom + roiMapBlockUnitSize - 1) /
                           roiMapBlockUnitSize) -
                         1)) {
                    g_quad_roi_map[k * subNumMbs + m].field.ipcm_flag =
                        0; // don't force skip mode
                    g_quad_roi_map[k * subNumMbs + m].field.roiAbsQp_flag =
                        0; // delta QP
                    g_quad_roi_map[k * subNumMbs + m].field.qp_info = set_qp;
                    // av_log(avctx, AV_LOG_INFO, "## x %d y %d index %d\n", i,
                    // j, k*subNumMbs+m);
                }
            }
            sumQp += g_quad_roi_map[k * subNumMbs].field.qp_info;
        }
    }
  }

  ctx->api_ctx.roi_len = customMapSize;
  ctx->api_ctx.roi_avg_qp = (sumQp + (numMbs>>1)) / numMbs + NI_DEFAULT_INTRA_QP; // round off

  return 0;
}
#endif
static int xcoder_encoder_headers(AVCodecContext *avctx)
{
  // use a copy of encoder context, take care to restore original config
  // cropping setting
  XCoderH265EncContext ctx;
  ni_xcoder_params_t *p_param;
  ni_packet_t *xpkt;
  int orig_conf_win_right;
  int orig_conf_win_bottom;
  int linesize_aligned, height_aligned;
  int ret, recv;

  memcpy(&ctx, (XCoderH265EncContext *)(avctx->priv_data),
         sizeof(XCoderH265EncContext));

  p_param = (ni_xcoder_params_t *)ctx.api_ctx.p_session_config;

  orig_conf_win_right  = p_param->cfg_enc_params.conf_win_right;
  orig_conf_win_bottom = p_param->cfg_enc_params.conf_win_bottom;

  linesize_aligned = avctx->width;
  if (linesize_aligned < NI_MIN_WIDTH)
  {
      p_param->cfg_enc_params.conf_win_right +=
          (NI_MIN_WIDTH - avctx->width) / 2 * 2;
      linesize_aligned = NI_MIN_WIDTH;
  }
  else
  {
      if (avctx->sw_pix_fmt == AV_PIX_FMT_NI_QUAD_8_TILE_4X4 ||
          avctx->sw_pix_fmt == AV_PIX_FMT_NI_QUAD_10_TILE_4X4) 
      {
          linesize_aligned = FFALIGN(avctx->width, 4);
          p_param->cfg_enc_params.conf_win_right +=
              (linesize_aligned - avctx->width) / 2 * 2;
      } 
      else 
      {
          linesize_aligned = FFALIGN(avctx->width, 2);
          p_param->cfg_enc_params.conf_win_right +=
              (linesize_aligned - avctx->width) / 2 * 2;
      }
  }
  p_param->source_width = linesize_aligned;

  height_aligned = avctx->height;
  if (height_aligned < NI_MIN_HEIGHT)
  {
      p_param->cfg_enc_params.conf_win_bottom +=
          (NI_MIN_HEIGHT - avctx->height) / 2 * 2;
      height_aligned = NI_MIN_HEIGHT;
  }
  else
  {
      if (avctx->sw_pix_fmt == AV_PIX_FMT_NI_QUAD_8_TILE_4X4 ||
          avctx->sw_pix_fmt == AV_PIX_FMT_NI_QUAD_10_TILE_4X4) 
      {
          height_aligned = FFALIGN(avctx->height, 4);
          p_param->cfg_enc_params.conf_win_bottom +=
              (height_aligned - avctx->height) / 4 * 4;
      } 
      else 
      {
          height_aligned = FFALIGN(avctx->height, 2);
          p_param->cfg_enc_params.conf_win_bottom +=
              (height_aligned - avctx->height) / 2 * 2;
      }
  }
  p_param->source_height = height_aligned;

  ctx.api_ctx.hw_id = ctx.dev_enc_idx;
  ff_xcoder_strncpy(ctx.api_ctx.blk_dev_name, ctx.dev_blk_name,
                    NI_MAX_DEVICE_NAME_LEN);
  ret = ni_device_session_open(&ctx.api_ctx, NI_DEVICE_TYPE_ENCODER);

  ctx.dev_xcoder_name = ctx.api_ctx.dev_xcoder_name;
  ctx.blk_xcoder_name = ctx.api_ctx.blk_xcoder_name;
  ctx.dev_enc_idx = ctx.api_ctx.hw_id;

  if (ret != 0)
  {
      av_log(avctx, AV_LOG_ERROR,
             "Failed to open encoder (status = %d), "
             "resource unavailable\n",
             ret);
      ret = AVERROR_EXTERNAL;
      goto end;
  } else {
      av_log(avctx, AV_LOG_VERBOSE,
             "XCoder %s.%d (inst: %d) opened successfully\n",
             ctx.dev_xcoder_name, ctx.dev_enc_idx, ctx.api_ctx.session_id);
  }

  xpkt = &ctx.api_pkt.data.packet;
  ni_packet_buffer_alloc(xpkt, NI_MAX_TX_SZ);

  while (1)
  {
    recv = ni_device_session_read(&ctx.api_ctx, &(ctx.api_pkt),
                                  NI_DEVICE_TYPE_ENCODER);

    if (recv > 0)
    {
        av_freep(&avctx->extradata);
        avctx->extradata_size = recv - (int)ctx.api_ctx.meta_size;
        avctx->extradata =
            av_mallocz(avctx->extradata_size + AV_INPUT_BUFFER_PADDING_SIZE);
        memcpy(avctx->extradata,
               (uint8_t *)xpkt->p_data + ctx.api_ctx.meta_size,
               avctx->extradata_size);
        av_log(avctx, AV_LOG_VERBOSE, "Xcoder encoder headers len: %d\n",
               avctx->extradata_size);
        break;
    }
  }

end:

    // close and clean up the temporary session
    if (ret != 0)
    {
        ni_device_session_close(&ctx.api_ctx, ctx.encoder_eof,
                                NI_DEVICE_TYPE_ENCODER);
    }
    else
    {
        ret = ni_device_session_close(&ctx.api_ctx, ctx.encoder_eof,
                                      NI_DEVICE_TYPE_ENCODER);
    }
#ifdef _WIN32
  ni_device_close(ctx.api_ctx.device_handle);
#elif __linux__
  ni_device_close(ctx.api_ctx.device_handle);
  ni_device_close(ctx.api_ctx.blk_io_handle);
#endif
  ctx.api_ctx.device_handle = NI_INVALID_DEVICE_HANDLE;
  ctx.api_ctx.blk_io_handle = NI_INVALID_DEVICE_HANDLE;

  ni_packet_buffer_free( &(ctx.api_pkt.data.packet) );

  ni_rsrc_free_device_context(ctx.rsrc_ctx);
  ctx.rsrc_ctx = NULL;

  p_param->cfg_enc_params.conf_win_right  = orig_conf_win_right;
  p_param->cfg_enc_params.conf_win_bottom = orig_conf_win_bottom;

  return ret;
}

static int xcoder_encoder_header_check_set(AVCodecContext *avctx)
{
  XCoderH265EncContext *ctx = avctx->priv_data;
  ni_xcoder_params_t *p_param;
  // set color metrics
  enum AVColorPrimaries color_primaries = avctx->color_primaries;
  enum AVColorTransferCharacteristic color_trc = avctx->color_trc;
  enum AVColorSpace color_space = avctx->colorspace;

  p_param = (ni_xcoder_params_t *)ctx->api_ctx.p_session_config;
  p_param->cfg_enc_params.videoFullRange       = 0;

  // DolbyVision support
  if (5 == p_param->dolby_vision_profile && AV_CODEC_ID_HEVC == avctx->codec_id)
  {
      color_primaries                    = AVCOL_PRI_UNSPECIFIED;
      color_trc                          = AVCOL_TRC_UNSPECIFIED;
      color_space                        = AVCOL_SPC_UNSPECIFIED;
      p_param->cfg_enc_params.hrdEnable = p_param->cfg_enc_params.EnableAUD = 1;
      p_param->cfg_enc_params.forced_header_enable                          = 1;
      p_param->cfg_enc_params.videoFullRange                                = 1;
  }

  if ((5 == p_param->dolby_vision_profile &&
       AV_CODEC_ID_HEVC == avctx->codec_id) ||
      color_primaries != AVCOL_PRI_UNSPECIFIED ||
      color_trc != AVCOL_TRC_UNSPECIFIED ||
      color_space != AVCOL_SPC_UNSPECIFIED) {
      p_param->cfg_enc_params.colorDescPresent = 1;
      p_param->cfg_enc_params.colorPrimaries   = color_primaries;
      p_param->cfg_enc_params.colorTrc         = color_trc;
      p_param->cfg_enc_params.colorSpace       = color_space;

      av_log(avctx, AV_LOG_VERBOSE,
             "XCoder HDR color info color_primaries: %d "
             "color_trc: %d  color_space %d\n",
             color_primaries, color_trc, color_space);
  }
  if (avctx->color_range == AVCOL_RANGE_JPEG ||
      AV_PIX_FMT_YUVJ420P == avctx->pix_fmt ||
      AV_PIX_FMT_YUVJ420P == avctx->sw_pix_fmt) {
      p_param->cfg_enc_params.videoFullRange = 1;
  }

  return 0;
}

static int xcoder_setup_encoder(AVCodecContext *avctx)
{
  XCoderH265EncContext *s = avctx->priv_data;
  int i, ret = 0;
  uint32_t  xcoder_timeout;
  ni_xcoder_params_t *p_param       = &s->api_param;
  ni_xcoder_params_t *pparams       = NULL;
  ni_session_run_state_t prev_state = s->api_ctx.session_run_state;

  av_log(avctx, AV_LOG_VERBOSE, "XCoder setup device encoder\n");
  //s->api_ctx.session_id = NI_INVALID_SESSION_ID;
  if (ni_device_session_context_init(&(s->api_ctx)) < 0)
  {
      av_log(avctx, AV_LOG_ERROR,
             "Error XCoder init encoder context failure\n");
      return AVERROR_EXTERNAL;
  }

  switch (avctx->codec_id)
  {
  case AV_CODEC_ID_HEVC:
      s->api_ctx.codec_format = NI_CODEC_FORMAT_H265;
      break;
  case AV_CODEC_ID_AV1:
      s->api_ctx.codec_format = NI_CODEC_FORMAT_AV1;
      break;
  case AV_CODEC_ID_MJPEG:
      s->api_ctx.codec_format = NI_CODEC_FORMAT_JPEG;
      break;
  default:
      s->api_ctx.codec_format = NI_CODEC_FORMAT_H264;
      break;
  }

  s->api_ctx.session_run_state = prev_state;
  s->av_rois = NULL;
  s->firstPktArrived = 0;
  s->spsPpsArrived = 0;
  s->spsPpsHdrLen = 0;
  s->p_spsPpsHdr = NULL;
  s->xcode_load_pixel = 0;
  s->reconfigCount = 0;
  s->latest_dts = 0;
  s->first_frame_pts = INT_MIN;

  if (SESSION_RUN_STATE_SEQ_CHANGE_DRAINING != s->api_ctx.session_run_state)
  {
      av_log(avctx, AV_LOG_INFO, "Session state: %d allocate frame fifo.\n",
          s->api_ctx.session_run_state);
      s->fme_fifo = av_fifo_alloc(sizeof(AVFrame));
  }
  else
  {
      av_log(avctx, AV_LOG_INFO, "Session seq change, fifo size: %lu.\n",
          av_fifo_size(s->fme_fifo) / sizeof(AVFrame));
  }

  if (!s->fme_fifo)
  {
      return AVERROR(ENOMEM);
  }
  s->eos_fme_received = 0;

  //Xcoder User Configuration
  ret = ni_encoder_init_default_params(
      p_param, avctx->framerate.num,
      avctx->framerate.den, avctx->bit_rate,
      avctx->width, avctx->height, s->api_ctx.codec_format);
  switch (ret)
  {
  case NI_RETCODE_PARAM_ERROR_WIDTH_TOO_BIG:
    if (avctx->codec_id == AV_CODEC_ID_AV1)
        av_log(avctx, AV_LOG_ERROR, "Invalid Picture Width: exceeds %d\n",
               NI_PARAM_AV1_MAX_WIDTH);
    else
        av_log(avctx, AV_LOG_ERROR, "Invalid Picture Width: too big\n");
    return AVERROR_EXTERNAL;
  case NI_RETCODE_PARAM_ERROR_WIDTH_TOO_SMALL:
    av_log(avctx, AV_LOG_ERROR, "Invalid Picture Width: too small\n");
    return AVERROR_EXTERNAL;
  case NI_RETCODE_PARAM_ERROR_HEIGHT_TOO_BIG:
    if (avctx->codec_id == AV_CODEC_ID_AV1)
        av_log(avctx, AV_LOG_ERROR, "Invalid Picture Height: exceeds %d\n",
               NI_PARAM_AV1_MAX_HEIGHT);
    else
        av_log(avctx, AV_LOG_ERROR, "Invalid Picture Height: too big\n");
    return AVERROR_EXTERNAL;
  case NI_RETCODE_PARAM_ERROR_HEIGHT_TOO_SMALL:
    av_log(avctx, AV_LOG_ERROR, "Invalid Picture Height: too small\n");
    return AVERROR_EXTERNAL;
  case NI_RETCODE_PARAM_ERROR_AREA_TOO_BIG:
    if (avctx->codec_id == AV_CODEC_ID_AV1)
        av_log(avctx, AV_LOG_ERROR,
               "Invalid Picture Width x Height: exceeds %d\n",
               NI_PARAM_AV1_MAX_AREA);
    else
        av_log(avctx, AV_LOG_ERROR,
               "Invalid Picture Width x Height: exceeds %d\n",
               NI_MAX_RESOLUTION_AREA);
    return AVERROR_EXTERNAL;
  case NI_RETCODE_PARAM_ERROR_PIC_WIDTH:
    av_log(avctx, AV_LOG_ERROR, "Invalid Picture Width\n");
    return AVERROR_EXTERNAL;
  case NI_RETCODE_PARAM_ERROR_PIC_HEIGHT:
    av_log(avctx, AV_LOG_ERROR, "Invalid Picture Height\n");
    return AVERROR_EXTERNAL;
  default:
    if (ret < 0)
    {
      av_log(avctx, AV_LOG_ERROR, "Error setting preset or log.\n");
      av_log(avctx, AV_LOG_INFO, "Possible presets:");
      for (i = 0; g_xcoder_preset_names[i]; i++)
        av_log(avctx, AV_LOG_INFO, " %s", g_xcoder_preset_names[i]);
      av_log(avctx, AV_LOG_INFO, "\n");

      av_log(avctx, AV_LOG_INFO, "Possible log:");
      for (i = 0; g_xcoder_log_names[i]; i++)
        av_log(avctx, AV_LOG_INFO, " %s", g_xcoder_log_names[i]);
      av_log(avctx, AV_LOG_INFO, "\n");

      return AVERROR(EINVAL);
    }
    break;
  }

  av_log(avctx, AV_LOG_INFO, "pix_fmt is %d, sw_pix_fmt is %d\n", avctx->pix_fmt, avctx->sw_pix_fmt);
  if (avctx->pix_fmt != AV_PIX_FMT_NI_QUAD)
  {
    av_log(avctx, AV_LOG_INFO, "sw_pix_fmt assigned to pix_fmt was %d, is now %d\n", avctx->pix_fmt, avctx->sw_pix_fmt);
    avctx->sw_pix_fmt = avctx->pix_fmt;
  }
  else
  {
      if ((avctx->height >= NI_MIN_HEIGHT) && (avctx->width >= NI_MIN_WIDTH)) {
          p_param->hwframes = 1;
      } 
      else if (avctx->sw_pix_fmt == AV_PIX_FMT_NI_QUAD_8_TILE_4X4 ||
               avctx->sw_pix_fmt == AV_PIX_FMT_NI_QUAD_10_TILE_4X4) {
          av_log(avctx, AV_LOG_ERROR, "Invalid Picture Height or Width: too small\n");
          return AVERROR_EXTERNAL;
      }
  }

  switch (avctx->sw_pix_fmt)
  {
  case AV_PIX_FMT_YUV420P:
  case AV_PIX_FMT_YUVJ420P:
  case AV_PIX_FMT_YUV420P10LE:
  case AV_PIX_FMT_NV12:
  case AV_PIX_FMT_P010LE:
  case AV_PIX_FMT_NI_QUAD_8_TILE_4X4:
  case AV_PIX_FMT_NI_QUAD_10_TILE_4X4:
    break;
  default:
    av_log(avctx, AV_LOG_ERROR, "Pixfmt %s not supported in Quadra encoder\n",
           av_get_pix_fmt_name(avctx->sw_pix_fmt));
    return AVERROR_INVALIDDATA;
  }

  if (s->xcoder_opts)
  {
    AVDictionary *dict = NULL;
    AVDictionaryEntry *en = NULL;

    if (!av_dict_parse_string(&dict, s->xcoder_opts, "=", ":", 0))
    {
      while ((en = av_dict_get(dict, "", en, AV_DICT_IGNORE_SUFFIX)))
      {
        int parse_ret = ni_encoder_params_set_value(p_param, en->key, en->value);

        switch (parse_ret)
        {
          case NI_RETCODE_PARAM_INVALID_NAME:
            av_log(avctx, AV_LOG_ERROR,
                   "Unknown option: %s.\n", en->key);
            return AVERROR_EXTERNAL;
          case NI_RETCODE_PARAM_ERROR_TOO_BIG:
            av_log(avctx, AV_LOG_ERROR, "Invalid %s: too big\n", en->key);
            return AVERROR_EXTERNAL;
          case NI_RETCODE_PARAM_ERROR_TOO_SMALL:
            av_log(avctx, AV_LOG_ERROR, "Invalid %s: too small\n", en->key);
            return AVERROR_EXTERNAL;
          case NI_RETCODE_PARAM_ERROR_OOR:
            av_log(avctx, AV_LOG_ERROR, "Invalid %s: out of range\n", en->key);
            return AVERROR_EXTERNAL;
          case NI_RETCODE_PARAM_ERROR_ZERO:
            av_log(avctx, AV_LOG_ERROR, "Error setting option %s to value 0\n", en->key);
            return AVERROR_EXTERNAL;
          case NI_RETCODE_PARAM_INVALID_VALUE:
            av_log(avctx, AV_LOG_ERROR,
                   "Invalid value for %s: %s.\n", en->key, en->value);
            return AVERROR_EXTERNAL;
          case NI_RETCODE_PARAM_WARNING_DEPRECATED:
            av_log(avctx, AV_LOG_WARNING, "Parameter %s is deprecated\n", en->key);
            break;
          default:
            break;
        }
      }
      av_dict_free(&dict);
    }
  }

  if (p_param->enable_vfr) {
      // in the vfr mode, we use the default framerate
      // using the time_base to initial timing info
      p_param->cfg_enc_params.frame_rate = 30;
      s->api_ctx.prev_fps                = 30;
      s->api_ctx.last_change_framenum    = 0;
      s->api_ctx.fps_change_detect_count = 0;
  }

  av_log(avctx, AV_LOG_ERROR, "p_param->hwframes = %d\n", p_param->hwframes);
  if (s->xcoder_gop)
  {
    AVDictionary *dict = NULL;
    AVDictionaryEntry *en = NULL;

    if (!av_dict_parse_string(&dict, s->xcoder_gop, "=", ":", 0))
    {
      while ((en = av_dict_get(dict, "", en, AV_DICT_IGNORE_SUFFIX)))
      {
        int parse_ret = ni_encoder_gop_params_set_value(p_param, en->key, en->value);

        switch (parse_ret)
        {
          case NI_RETCODE_PARAM_INVALID_NAME:
            av_log(avctx, AV_LOG_ERROR,
                   "Unknown option: %s.\n", en->key);
            return AVERROR_EXTERNAL;
          case NI_RETCODE_PARAM_ERROR_TOO_BIG:
            av_log(avctx, AV_LOG_ERROR, "Invalid custom GOP parameters: %s too big\n", en->key);
            return AVERROR_EXTERNAL;
          case NI_RETCODE_PARAM_ERROR_TOO_SMALL:
            av_log(avctx, AV_LOG_ERROR, "Invalid custom GOP parameters: %s too small\n", en->key);
            return AVERROR_EXTERNAL;
          case NI_RETCODE_PARAM_ERROR_OOR:
            av_log(avctx, AV_LOG_ERROR, "Invalid custom GOP parameters: %s out of range \n", en->key);
            return AVERROR_EXTERNAL;
          case NI_RETCODE_PARAM_ERROR_ZERO:
             av_log(avctx, AV_LOG_ERROR, "Invalid custom GOP paramaters: Error setting option %s to value 0 \n", en->key);
             return AVERROR_EXTERNAL;
          case NI_RETCODE_PARAM_INVALID_VALUE:
            av_log(avctx, AV_LOG_ERROR,
                   "Invalid value for GOP param %s: %s.\n", en->key, en->value);
            return AVERROR_EXTERNAL;
          default:
            break;
        }
      }
      av_dict_free(&dict);
    }
  }
  if (s->nvme_io_size > 0 && s->nvme_io_size % 4096 != 0)
  {
    av_log(avctx, AV_LOG_ERROR, "Error XCoder iosize is not 4KB aligned!\n");
    return AVERROR_EXTERNAL;
  }

  s->api_ctx.p_session_config = &s->api_param;
  pparams = (ni_xcoder_params_t *)s->api_ctx.p_session_config;
  if (QUADRA)
  {
      switch (pparams->cfg_enc_params.gopSize) {
      /* dtsOffset is the max number of non-reference frames in a GOP
       * (derived from x264/5 algo) In case of IBBBP the first dts of the I frame should be input_pts-(3*ticks_per_frame)
       * In case of IBP the first dts of the I frame should be input_pts-(1*ticks_per_frame)
       * thus we ensure pts>dts in all cases
       * */
      case 1:
      case 2:
      case 3:
      case 4:
      case 5:
      case 6:
      case 7:
      case 8:
          s->dtsOffset = pparams->cfg_enc_params.gopSize - 1;
          break;
      default: // gopSize 0 (adaptive GOP)
        s->dtsOffset = 7;
        av_log(avctx, AV_LOG_VERBOSE, "dts offset default to 7\n");
        break;
      }
      if (pparams->cfg_enc_params.custom_gop_params.custom_gop_size)
          s->dtsOffset =
              pparams->cfg_enc_params.custom_gop_params.custom_gop_size - 1;
  }
  else
  {
      switch (pparams->cfg_enc_params.gop_preset_index) {
      /* dtsOffset is the max number of non-reference frames in a GOP
       * (derived from x264/5 algo) In case of IBBBP the first dts of the I frame should be input_pts-(3*ticks_per_frame)
       * In case of IBP the first dts of the I frame should be input_pts-(1*ticks_per_frame)
       * thus we ensure pts>dts in all cases
       * */
      case 1 /*PRESET_IDX_ALL_I*/:
      case 2 /*PRESET_IDX_IPP*/:
      case 3 /*PRESET_IDX_IBBB*/:
      case 6 /*PRESET_IDX_IPPPP*/:
      case 7 /*PRESET_IDX_IBBBB*/:
      case 9 /*PRESET_IDX_SP*/:
        s->dtsOffset = 0;
        break;
      case 4 /*PRESET_IDX_IBPBP*/:
        s->dtsOffset = 1;
        break;
      case 5 /*PRESET_IDX_IBBBP*/:
        s->dtsOffset = 2;
        break;
      case 8 /*PRESET_IDX_RA_IB*/:
        s->dtsOffset = 3;
        break;
      default:
        // TBD need user to specify offset
        s->dtsOffset = 7;
        av_log(avctx, AV_LOG_VERBOSE, "dts offset default to 7, TBD\n");
        break;
      }
    if (1 == pparams->force_frame_type)
    {
      s->dtsOffset = 7;
    }
    //printf("dts offset: %lld \n", s->dtsOffset);
  }

  s->total_frames_received = 0;
  s->gop_offset_count = 0;
  av_log(avctx, AV_LOG_INFO, "dts offset: %ld, gop_offset_count: %d\n",
         s->dtsOffset, s->gop_offset_count);

  //overwrite the nvme io size here with a custom value if it was provided
  if (s->nvme_io_size > 0)
  {
    s->api_ctx.max_nvme_io_size = s->nvme_io_size;
    av_log(avctx, AV_LOG_VERBOSE, "Custom NVME IO Size set to = %u\n",
           s->api_ctx.max_nvme_io_size);
    printf("Encoder user specified NVMe IO Size set to: %u\n",
           s->api_ctx.max_nvme_io_size);
  }

  // overwrite keep alive timeout value here with a custom value if it was
  // provided
  // if xcoder option is set then overwrite the (legacy) decoder option
  xcoder_timeout = s->api_param.cfg_enc_params.keep_alive_timeout;
  if (xcoder_timeout != NI_DEFAULT_KEEP_ALIVE_TIMEOUT)
  {
      s->api_ctx.keep_alive_timeout = xcoder_timeout;
  }
  else
  {
      s->api_ctx.keep_alive_timeout = s->keep_alive_timeout;
  }
  av_log(avctx, AV_LOG_VERBOSE, "Custom NVME Keep Alive Timeout set to = %d\n",
         s->api_ctx.keep_alive_timeout);

  s->encoder_eof = 0;
  avctx->bit_rate = pparams->bitrate;

  s->api_ctx.src_bit_depth = 8;
  s->api_ctx.src_endian = NI_FRAME_LITTLE_ENDIAN;
  s->api_ctx.roi_len = 0;
  s->api_ctx.roi_avg_qp = 0;
  s->api_ctx.bit_depth_factor = 1;
  if (AV_PIX_FMT_YUV420P10BE == avctx->sw_pix_fmt ||
      AV_PIX_FMT_YUV420P10LE == avctx->sw_pix_fmt ||
      AV_PIX_FMT_P010LE == avctx->sw_pix_fmt || 
      AV_PIX_FMT_NI_QUAD_10_TILE_4X4 == avctx->sw_pix_fmt)
  {
    s->api_ctx.bit_depth_factor = 2;
    s->api_ctx.src_bit_depth = 10;
    if (AV_PIX_FMT_YUV420P10BE == avctx->sw_pix_fmt)
    {
        s->api_ctx.src_endian = NI_FRAME_BIG_ENDIAN;
    }
  }
  switch (avctx->sw_pix_fmt)
  {
      case AV_PIX_FMT_NV12:
      case AV_PIX_FMT_P010LE:
          pparams->cfg_enc_params.planar = NI_PIXEL_PLANAR_FORMAT_SEMIPLANAR;
          break;
      case AV_PIX_FMT_NI_QUAD_8_TILE_4X4:
      case AV_PIX_FMT_NI_QUAD_10_TILE_4X4:
          pparams->cfg_enc_params.planar = NI_PIXEL_PLANAR_FORMAT_TILED4X4;
          break;
      default:
          pparams->cfg_enc_params.planar = NI_PIXEL_PLANAR_FORMAT_PLANAR;
          break;
  }
  
  if (1)
  {
    s->freeHead = 0;
    s->freeTail = 0;
    for (i = 0; i < MAX_NUM_FRAMEPOOL_HWAVFRAME; i++)
    {
      s->sframe_pool[i] = av_frame_alloc();
      if (!s->sframe_pool[i]) {
        return AVERROR(ENOMEM);
      }
      s->aFree_Avframes_list[i] = i;
      s->freeTail++;
    }
    s->aFree_Avframes_list[i] = -1;
  }

  // init HDR SEI stuff
  s->api_ctx.sei_hdr_content_light_level_info_len =
      s->api_ctx.light_level_data_len =
      s->api_ctx.sei_hdr_mastering_display_color_vol_len =
      s->api_ctx.mdcv_max_min_lum_data_len = 0;
  s->api_ctx.p_master_display_meta_data = NULL;

  memset( &(s->api_fme), 0, sizeof(ni_session_data_io_t) );
  memset( &(s->api_pkt), 0, sizeof(ni_session_data_io_t) );

  s->api_pkt.data.packet.av1_buffer_index = 0;

  if (avctx->width > 0 && avctx->height > 0)
  {
      bool isnv12frame = (avctx->sw_pix_fmt == AV_PIX_FMT_NV12 ||
                          avctx->sw_pix_fmt == AV_PIX_FMT_P010LE);
      ni_frame_buffer_alloc(&(s->api_fme.data.frame), avctx->width,
                            avctx->height, 0, 0, s->api_ctx.bit_depth_factor, 1,
                            !isnv12frame);
  }

  //validate encoded bitstream headers struct for encoder open
  xcoder_encoder_header_check_set(avctx);

  // aspect ratio
  p_param->cfg_enc_params.aspectRatioWidth  = avctx->sample_aspect_ratio.num;
  p_param->cfg_enc_params.aspectRatioHeight = avctx->sample_aspect_ratio.den;

  // generate encoded bitstream headers in advance if configured to do so
  if ((pparams->generate_enc_hdrs) && (avctx->codec_id != AV_CODEC_ID_MJPEG)) {
      ret = xcoder_encoder_headers(avctx);
  }

  // original resolution this stream started with, this is used by encoder sequence change
  s->api_ctx.ori_width = avctx->width;
  s->api_ctx.ori_height = avctx->height;
  s->api_ctx.ori_bit_depth_factor = s->api_ctx.bit_depth_factor;
  s->api_ctx.ori_pix_fmt = avctx->sw_pix_fmt;

  return ret;
}

av_cold int xcoder_encode_init(AVCodecContext *avctx)
{
  XCoderH265EncContext *ctx = avctx->priv_data;
  AVHWFramesContext *avhwf_ctx;
  int ret;
#if 0
  /* stop logging into the terminal (Reza) */
  ni_log_set_level(ff_to_ni_log_level(av_log_get_level()));
#endif

  av_log(avctx, AV_LOG_VERBOSE, "XCoder encode init\n");

   if (ctx->api_ctx.session_run_state == SESSION_RUN_STATE_SEQ_CHANGE_DRAINING)
  {
    ctx->dev_enc_idx = ctx->orig_dev_enc_idx;
  }
  else
  {
    ctx->orig_dev_enc_idx = ctx->dev_enc_idx;
  }

#ifdef MULTI_THREAD
  static int initData = 0;
  int i;
  if (initData == 0)
  {
    for (i = 0; i < NI_MAX_NUM_SESSIONS; i++)
    {
      write_thread_args[i].session_id = NI_INVALID_SESSION_ID;
    }
    initData = 1;
  }
#endif

  if ((ret = xcoder_setup_encoder(avctx)) < 0)
  {
      xcoder_encode_close(avctx);
      return ret;
  }

  if (!avctx->hw_device_ctx) {
      if (avctx->hw_frames_ctx) {
          avhwf_ctx = (AVHWFramesContext *)avctx->hw_frames_ctx->data;
          avctx->hw_device_ctx = av_buffer_ref(avhwf_ctx->device_ref);
      }
  }

  return 0;
}

#ifdef MULTI_THREAD
static void* write_frame_thread(void* arg)
{
  write_thread_arg_struct_t *args = (write_thread_arg_struct_t *) arg;
  XCoderH265EncContext *ctx = args->ctx;
  int ret;
  int sent;

  av_log(ctx, AV_LOG_DEBUG, "write_frame_thread: session_id %d\n", args->session_id);

  av_log(ctx, AV_LOG_DEBUG, "write_frame_thread: ctx %p\n", ctx);

  sent = ni_device_session_write(&ctx->api_ctx, &ctx->api_fme, NI_DEVICE_TYPE_ENCODER);

  av_log(ctx, AV_LOG_DEBUG, "write_frame_thread: size %d sent to xcoder\n", sent);

  if (NI_RETCODE_ERROR_RESOURCE_UNAVAILABLE == sent)
  {
    av_log(ctx, AV_LOG_DEBUG, "write_frame_thread(): Sequence Change in progress, returning EAGAIN\n");
    ret = AVERROR(EAGAIN);
  }
  else
  {
    if (sent == -1)
    {
      ret = AVERROR(EAGAIN);
    }
    else
    {
      //pushing input pts in circular FIFO
      ctx->api_ctx.enc_pts_list[ctx->api_ctx.enc_pts_w_idx % NI_FIFO_SZ] = ctx->api_fme.data.frame.pts;
      ctx->api_ctx.enc_pts_w_idx++;
      ret = 0;
    }
  }
  args->ret = ret;
  av_log(ctx, AV_LOG_DEBUG, "write_frame_thread: ret %d\n", ret);
  pthread_exit(NULL);
}
#endif

int xcoder_encode_close(AVCodecContext *avctx)
{
  XCoderH265EncContext *ctx = avctx->priv_data;
  ni_retcode_t ret = NI_RETCODE_FAILURE;
  int i;

#ifdef MULTI_THREAD
  uint8_t id = find_session_idx(ctx->api_ctx.session_id);
  if (id < NI_MAX_NUM_SESSIONS)
  {
    pthread_cancel(write_thread_args[id].thread);
    write_thread_args[id].session_id = NI_INVALID_SESSION_ID;
  }
#endif

  for (i = 0; i < MAX_NUM_FRAMEPOOL_HWAVFRAME; i++)
  {
    AVFrame *frame = NULL;
    frame = ctx->sframe_pool[i];
    if (frame && frame->data[3])
    {
        ((niFrameSurface1_t *)((uint8_t *)frame->data[3]))->device_handle =
            (int32_t)((int64_t)(ctx->api_ctx.blk_io_handle) &
                      0xFFFFFFFF); // update handle to most recent alive
    }
    av_frame_free(&(ctx->sframe_pool[i])); //any remaining stored AVframes that have not been unref will die here
    ctx->sframe_pool[i] = NULL;
  }

  ret = ni_device_session_close(&ctx->api_ctx, ctx->encoder_eof,
                                NI_DEVICE_TYPE_ENCODER);
  if (NI_RETCODE_SUCCESS != ret)
  {
    av_log(avctx, AV_LOG_ERROR, "Failed to close Encoder Session (status = %d)\n", ret);
  }

  av_log(avctx, AV_LOG_VERBOSE, "XCoder encode close: session_run_state %d\n", ctx->api_ctx.session_run_state);
  if (ctx->api_ctx.session_run_state != SESSION_RUN_STATE_SEQ_CHANGE_DRAINING)
  {
    av_log(avctx, AV_LOG_VERBOSE, "XCoder encode close: close blk_io_handle %d device_handle %d\n", ctx->api_ctx.blk_io_handle, ctx->api_ctx.device_handle);
#ifdef _WIN32
    ni_device_close(ctx->api_ctx.device_handle);
#elif __linux__
    ni_device_close(ctx->api_ctx.device_handle);
    ni_device_close(ctx->api_ctx.blk_io_handle);
#endif
    ctx->api_ctx.device_handle = NI_INVALID_DEVICE_HANDLE;
    ctx->api_ctx.blk_io_handle = NI_INVALID_DEVICE_HANDLE;
    ctx->api_ctx.auto_dl_handle = NI_INVALID_DEVICE_HANDLE;
    ctx->api_ctx.sender_handle = NI_INVALID_DEVICE_HANDLE;
  }

  av_log(avctx, AV_LOG_VERBOSE, "XCoder encode close (status = %d)\n", ret);
  
  if (ctx->api_fme.data.frame.buffer_size)
  {
      ni_frame_buffer_free(&(ctx->api_fme.data.frame));
  }
  ni_packet_buffer_free( &(ctx->api_pkt.data.packet) );
  if (AV_CODEC_ID_AV1 == avctx->codec_id &&
      ctx->api_pkt.data.packet.av1_buffer_index)
      ni_packet_buffer_free_av1(&(ctx->api_pkt.data.packet));

  av_log(avctx, AV_LOG_DEBUG, "fifo size: %lu\n",
         av_fifo_size(ctx->fme_fifo) / sizeof(AVFrame));
  if (ctx->api_ctx.session_run_state != SESSION_RUN_STATE_SEQ_CHANGE_DRAINING)
  {
    av_fifo_free(ctx->fme_fifo);
    av_log(avctx, AV_LOG_DEBUG, " , freed.\n");
  }
  else
  {
    av_log(avctx, AV_LOG_DEBUG, " , kept.\n");
  }

  ni_device_session_context_clear(&ctx->api_ctx);

  ni_rsrc_free_device_context(ctx->rsrc_ctx);
  ctx->rsrc_ctx = NULL;

  free(ctx->av_rois);
  ctx->av_rois = NULL;
  if (ctx->p_spsPpsHdr) {
      free(ctx->p_spsPpsHdr);
      ctx->p_spsPpsHdr = NULL;
  }

  if (avctx->hw_device_ctx) {
      av_buffer_unref(&avctx->hw_device_ctx);
  }
  ctx->started = 0;

  return 0;
}


int xcoder_encode_sequence_change(AVCodecContext *avctx, int width, int height, int bit_depth_factor)
{
  XCoderH265EncContext *ctx = avctx->priv_data;
  ni_retcode_t ret = NI_RETCODE_FAILURE;
  ni_xcoder_params_t *p_param       = &ctx->api_param;
  ni_xcoder_params_t *pparams = (ni_xcoder_params_t *)ctx->api_ctx.p_session_config;

  av_log(avctx, AV_LOG_VERBOSE, "XCoder encode sequence change: session_run_state %d\n", ctx->api_ctx.session_run_state);

  ret = ni_device_session_sequence_change(&ctx->api_ctx, width, height, bit_depth_factor, NI_DEVICE_TYPE_ENCODER);

  if (NI_RETCODE_SUCCESS != ret)
  {
    av_log(avctx, AV_LOG_ERROR, "Failed to send Sequence Change to Encoder Session (status = %d)\n", ret);
    return ret;
  }

  // update AvCodecContext
  if (avctx->pix_fmt != AV_PIX_FMT_NI_QUAD)
  {
    av_log(avctx, AV_LOG_INFO, "sw_pix_fmt assigned to pix_fmt was %d, is now %d\n", avctx->pix_fmt, avctx->sw_pix_fmt);
    avctx->sw_pix_fmt = avctx->pix_fmt;
  }
  else
  {
      if ((avctx->height >= NI_MIN_HEIGHT) && (avctx->width >= NI_MIN_WIDTH)) {
          p_param->hwframes = 1;
      }
  }

  switch (avctx->sw_pix_fmt)
  {
      case AV_PIX_FMT_YUV420P:
      case AV_PIX_FMT_YUV420P10:
      case AV_PIX_FMT_YUV420P12:
      case AV_PIX_FMT_NV12:
      case AV_PIX_FMT_P010LE:
      case AV_PIX_FMT_NI_QUAD_8_TILE_4X4:
      case AV_PIX_FMT_NI_QUAD_10_TILE_4X4:
        break;
      case AV_PIX_FMT_RGBA:
        av_log(avctx, AV_LOG_ERROR, "AV_PIX_FMT_RGBA format not supported for encoding\n");
      case AV_PIX_FMT_YUV422P:
      case AV_PIX_FMT_YUV422P10:
      case AV_PIX_FMT_YUV422P12:
      case AV_PIX_FMT_GBRP:
      case AV_PIX_FMT_GBRP10:
      case AV_PIX_FMT_GBRP12:
      case AV_PIX_FMT_YUV444P:
      case AV_PIX_FMT_YUV444P10:
      case AV_PIX_FMT_YUV444P12:
      case AV_PIX_FMT_GRAY8:
      case AV_PIX_FMT_GRAY10:
      case AV_PIX_FMT_GRAY12:
        return AVERROR_INVALIDDATA;
      default:
        break;
  }

  // update session context
  ctx->api_ctx.bit_depth_factor = bit_depth_factor;
  ctx->api_ctx.src_bit_depth = (bit_depth_factor == 1) ? 8 : 10;
  ctx->api_ctx.src_endian = (AV_PIX_FMT_YUV420P10BE == avctx->sw_pix_fmt) ? NI_FRAME_BIG_ENDIAN : NI_FRAME_LITTLE_ENDIAN;
  ctx->api_ctx.ready_to_close = 0;
  ctx->api_ctx.frame_num = 0; // need to reset frame_num because pkt_num is set to 1 when header received after sequnce change, and low delay mode compares frame_num and pkt_num
  ctx->api_ctx.pkt_num = 0; // also need to reset pkt_num because before header received, pkt_num > frame_num will also cause low delay mode stuck
  ctx->api_pkt.data.packet.end_of_stream = 0;

  switch (avctx->sw_pix_fmt)
  {
      case AV_PIX_FMT_NV12:
      case AV_PIX_FMT_P010LE:
          pparams->cfg_enc_params.planar = NI_PIXEL_PLANAR_FORMAT_SEMIPLANAR;
          break;
      case AV_PIX_FMT_NI_QUAD_8_TILE_4X4:
      case AV_PIX_FMT_NI_QUAD_10_TILE_4X4:
          pparams->cfg_enc_params.planar = NI_PIXEL_PLANAR_FORMAT_TILED4X4;
          break;
      default:
          pparams->cfg_enc_params.planar = NI_PIXEL_PLANAR_FORMAT_PLANAR;
          break;
  }
  return ret;
}

static int xcoder_encode_reset(AVCodecContext *avctx)
{
  av_log(avctx, AV_LOG_WARNING, "XCoder encode reset\n");
  xcoder_encode_close(avctx);
  return xcoder_encode_init(avctx);
}

// frame fifo operations
static int is_input_fifo_empty(XCoderH265EncContext *s)
{
  return av_fifo_size(s->fme_fifo) < sizeof(AVFrame);
}

static int enqueue_frame(AVCodecContext *avctx, const AVFrame *inframe)
{
  XCoderH265EncContext *ctx = avctx->priv_data;
  int ret;

  // expand frame buffer fifo if not enough space
  if (av_fifo_space(ctx->fme_fifo) < sizeof(AVFrame))
  {
    ret = av_fifo_realloc2(ctx->fme_fifo,
                           av_fifo_size(ctx->fme_fifo) + sizeof(AVFrame));
    if (ret < 0)
    {
      av_log(avctx, AV_LOG_ERROR, "Enc av_fifo_realloc2 NO MEMORY !!!\n");
      return ret;
    }
    if (((av_fifo_size(ctx->fme_fifo)+av_fifo_space(ctx->fme_fifo))  / sizeof(AVFrame) % 100) == 0)
    {
      av_log(avctx, AV_LOG_INFO, "Enc fifo being extended to: %lu\n",
             (av_fifo_size(ctx->fme_fifo)+av_fifo_space(ctx->fme_fifo)) / sizeof(AVFrame));
    }
    av_assert0(0 == (av_fifo_size(ctx->fme_fifo)+av_fifo_space(ctx->fme_fifo)) % sizeof(AVFrame));
  }

  if (inframe == &ctx->buffered_fme)
  {
    // For FFmpeg-n4.4+ receive_packet interface the buffered_fme is fetched from
    // ff_alloc_get_frame rather than passed as function argument. So we need to
    // judge whether they are the same object. If they are the same NO need to do
    // any reference before queue operation.
    av_fifo_generic_write(ctx->fme_fifo, (void *)inframe, sizeof(*inframe), NULL);
  }
  else
  {
    AVFrame temp_frame;
    memset(&temp_frame, 0, sizeof(AVFrame));
    // In case double free for external input frame and our buffered frame.
    av_frame_ref(&temp_frame, inframe);
    av_fifo_generic_write(ctx->fme_fifo, &temp_frame, sizeof(*inframe), NULL);
  }

  av_log(avctx, AV_LOG_DEBUG, "fme queued, fifo size: %lu\n",
         av_fifo_size(ctx->fme_fifo) / sizeof(AVFrame));

  return 0;
}

int xcoder_send_frame(AVCodecContext *avctx, const AVFrame *frame)
{
  XCoderH265EncContext *ctx = avctx->priv_data;
  bool ishwframe;
  bool isnv12frame;
  bool alignment_2pass_wa;
  int format_in_use;
  int ret = 0;
  int sent;
  int i, j;
  int orig_avctx_width = avctx->width;
  int orig_avctx_height = avctx->height;
  ni_xcoder_params_t *p_param;
  int need_to_copy = 1;
  AVHWFramesContext *avhwf_ctx;
  NIFramesContext *nif_src_ctx;
  AVFrameSideData *side_data;
  const AVFrame *first_frame = NULL;
  // employ a ni_frame_t as a data holder to convert/prepare for side data
  // of the passed in frame
  ni_frame_t dec_frame    = {0};
  ni_aux_data_t *aux_data = NULL;
  // data buffer for various SEI: HDR mastering display color volume, HDR
  // content light level, close caption, User data unregistered, HDR10+ etc.
  int send_sei_with_idr;
  uint8_t mdcv_data[NI_MAX_SEI_DATA];
  uint8_t cll_data[NI_MAX_SEI_DATA];
  uint8_t cc_data[NI_MAX_SEI_DATA];
  uint8_t udu_data[NI_MAX_SEI_DATA];
  uint8_t hdrp_data[NI_MAX_SEI_DATA];

  av_log(avctx, AV_LOG_VERBOSE, "XCoder send frame\n");

#ifdef MULTI_THREAD
  if (ctx->api_ctx.session_id!=NI_INVALID_SESSION_ID)
  {
    uint8_t id = find_session_idx(ctx->api_ctx.session_id);
    if (id < NI_MAX_NUM_SESSIONS)
    { //find the previous session
      av_log(avctx, AV_LOG_DEBUG, "xcoder_send_frame previous: find_session_idx %d\n", id);
      av_log(avctx, AV_LOG_DEBUG, "xcoder_send_frame previous: ctx->api_ctx.session_id %d\n", ctx->api_ctx.session_id);
      if (write_thread_args[id].thread)
      {
        av_log(avctx, AV_LOG_DEBUG, "xcoder_send_frame: pthread_join start thread %d\n", write_thread_args[id].thread);
        pthread_join(write_thread_args[id].thread, NULL);
        av_log(avctx, AV_LOG_DEBUG, "xcoder_send_frame: pthread_join end write_thread_args[id].ret %d\n", write_thread_args[id].ret);
        write_thread_args[id].thread = 0;
        write_thread_args[id].session_id = NI_INVALID_SESSION_ID;
        if (write_thread_args[id].ret == AVERROR(EAGAIN))
        {
          av_log(avctx, AV_LOG_ERROR, "xcoder_send_frame: ret %d\n", write_thread_args[id].ret);
          return AVERROR(EAGAIN);
        }
        else
        {
          /*
          // only if it's NOT sequence change flushing (in which case only the eos
          // was sent and not the first sc pkt) AND
          // only after successful sending will it be removed from fifo
          if (SESSION_RUN_STATE_SEQ_CHANGE_DRAINING !=
              ctx->api_ctx.session_run_state)
          {
            if (! is_input_fifo_empty(ctx))
            {
              av_fifo_drain(ctx->fme_fifo, sizeof(AVFrame));
              av_log(avctx, AV_LOG_DEBUG, "fme popped, fifo size: %lu\n",
                     av_fifo_size(ctx->fme_fifo) / sizeof(AVFrame));
            }
            av_frame_unref(&ctx->buffered_fme);
          }
          else
          {
            av_log(avctx, AV_LOG_TRACE, "XCoder frame(eos) sent, sequence changing!"
                   " NO fifo pop !\n");
          }
          */
        }
      }
    }
  }
#endif

  // NETINT_INTERNAL - currently only for internal testing
  p_param = (ni_xcoder_params_t *) ctx->api_ctx.p_session_config;
  alignment_2pass_wa = (p_param->cfg_enc_params.lookAheadDepth &&
                       ((avctx->codec_id == AV_CODEC_ID_HEVC) ||
                        (avctx->codec_id == AV_CODEC_ID_AV1)));

  // leave encoder instance open to when the first frame buffer arrives so that
  // its stride size is known and handled accordingly.
  if (ctx->started == 0)
  {
    if (!is_input_fifo_empty(ctx))
    {
      av_log(avctx, AV_LOG_VERBOSE, "first frame: use fme from av_fifo_generic_peek\n");
      av_fifo_generic_peek(ctx->fme_fifo, &ctx->buffered_fme,
                           sizeof(AVFrame), NULL);
      ctx->buffered_fme.extended_data = ctx->buffered_fme.data;
      first_frame = &ctx->buffered_fme;

    }
    else if (frame)
    {
      av_log(avctx, AV_LOG_VERBOSE, "first frame: use input frame\n");
      first_frame = frame;
    }
    else
    {
      av_log(avctx, AV_LOG_ERROR, "first frame: NULL is unexpected!\n");
    }
  }
  else if (ctx->api_ctx.session_run_state == SESSION_RUN_STATE_SEQ_CHANGE_OPENING)
  {
    if (!is_input_fifo_empty(ctx))
    {
      av_log(avctx, AV_LOG_VERBOSE, "first frame: use fme from av_fifo_generic_peek\n");
      av_fifo_generic_peek(ctx->fme_fifo, &ctx->buffered_fme,
                           sizeof(AVFrame), NULL);
      ctx->buffered_fme.extended_data = ctx->buffered_fme.data;
      first_frame = &ctx->buffered_fme;
    }
    else
    {
      av_log(avctx, AV_LOG_ERROR, "No buffered frame - Sequence Change Fail");
      ret = AVERROR_EXTERNAL;
      // xcoder_encode_close(avctx); will be called at codec close
      return ret;
    }
  }

  /*
  if ((SESSION_RUN_STATE_SEQ_CHANGE_OPENING == ctx->api_ctx.session_run_state) &&
      first_frame->hw_frames_ctx)
  {
    av_buffer_unref(avctx->hw_frames_ctx);
    avctx->hw_frames_ctx = av_buffer_ref(first_frame->hw_frames_ctx);
  }
  */
  if (first_frame && ctx->started == 0)
  {
      // if frame stride size is not as we expect it,
      // adjust using xcoder-params conf_win_right
      int linesize_aligned = first_frame->width;
      int height_aligned = first_frame->height;
      ishwframe = first_frame->format == AV_PIX_FMT_NI_QUAD;
      if (QUADRA) {
          if (linesize_aligned < NI_MIN_WIDTH) {
              p_param->cfg_enc_params.conf_win_right +=
                  (NI_MIN_WIDTH - first_frame->width) / 2 * 2;
              linesize_aligned = NI_MIN_WIDTH;
          } else {
              if (avctx->sw_pix_fmt == AV_PIX_FMT_NI_QUAD_8_TILE_4X4 ||
                  avctx->sw_pix_fmt == AV_PIX_FMT_NI_QUAD_10_TILE_4X4) {
                  linesize_aligned = FFALIGN(first_frame->width, 4);
                  p_param->cfg_enc_params.conf_win_right +=
                      (linesize_aligned - first_frame->width) / 2 * 2;
              } else {
                  linesize_aligned = FFALIGN(first_frame->width, 2);
                  p_param->cfg_enc_params.conf_win_right +=
                      (linesize_aligned - first_frame->width) / 2 * 2;
              }
              // linesize_aligned = ((((frame->width *
              // ctx->api_ctx.bit_depth_factor) + 15) / 16) * 16) /
              // ctx->api_ctx.bit_depth_factor; av_log(avctx, AV_LOG_DEBUG,
              // "xcoder_send_frame frame->width %d
              // ctx->api_ctx.bit_depth_factor %d linesize_aligned %d\n",
              // frame->width, ctx->api_ctx.bit_depth_factor, linesize_aligned);
          }
      } else {
          linesize_aligned = ((first_frame->width + 31) / 32) * 32;
          if (linesize_aligned < NI_MIN_WIDTH) {
              p_param->cfg_enc_params.conf_win_right +=
                  NI_MIN_WIDTH - first_frame->width;
              linesize_aligned = NI_MIN_WIDTH;
          } else if (linesize_aligned > first_frame->width) {
              p_param->cfg_enc_params.conf_win_right +=
                  linesize_aligned - first_frame->width;
          }
      }
    p_param->source_width = linesize_aligned;

    if (QUADRA)
    {
      if (height_aligned < NI_MIN_HEIGHT)
      {
          p_param->cfg_enc_params.conf_win_bottom +=
              (NI_MIN_HEIGHT - first_frame->height) / 2 * 2;
          height_aligned = NI_MIN_HEIGHT;
      }
      else
      {
          if (avctx->sw_pix_fmt == AV_PIX_FMT_NI_QUAD_8_TILE_4X4 || 
              avctx->sw_pix_fmt == AV_PIX_FMT_NI_QUAD_10_TILE_4X4) 
          {
              height_aligned = FFALIGN(first_frame->height, 4);
              p_param->cfg_enc_params.conf_win_bottom +=
                  (height_aligned - first_frame->height) / 4 * 4;
          }
          else
          {
              height_aligned = FFALIGN(first_frame->height, 2);
              p_param->cfg_enc_params.conf_win_bottom +=
                  (height_aligned - first_frame->height) / 2 * 2;
          }
      }
      p_param->source_height = height_aligned;
    }
    else
    {
      height_aligned = ((first_frame->height + 7) / 8) * 8;
      if (avctx->codec_id == AV_CODEC_ID_H264) {
        height_aligned = ((first_frame->height + 15) / 16) * 16;
      }
      if (height_aligned < NI_MIN_HEIGHT)
      {
          p_param->cfg_enc_params.conf_win_bottom +=
              NI_MIN_HEIGHT - first_frame->height;
          p_param->source_height = NI_MIN_HEIGHT;
          height_aligned         = NI_MIN_HEIGHT;
      }
      else if (height_aligned > first_frame->height)
      {
          p_param->cfg_enc_params.conf_win_bottom +=
              height_aligned - first_frame->height;
          if (avctx->codec_id != AV_CODEC_ID_H264) {
              p_param->source_height = height_aligned;
          }
      }
    }

    if (avctx->color_primaries == AVCOL_PRI_UNSPECIFIED) {
        avctx->color_primaries = first_frame->color_primaries;
    }
    if (avctx->color_trc == AVCOL_TRC_UNSPECIFIED) {
        avctx->color_trc = first_frame->color_trc;
    }
    if (avctx->colorspace == AVCOL_SPC_UNSPECIFIED) {
        avctx->colorspace = first_frame->colorspace;
    }
    avctx->color_range = first_frame->color_range;

    xcoder_encoder_header_check_set(avctx);

    av_log(avctx, AV_LOG_VERBOSE,
           "XCoder frame->linesize: %d/%d/%d frame width/height %dx%d"
           " conf_win_right %d  conf_win_bottom %d , color primaries %u trc %u "
           "space %u\n",
           first_frame->linesize[0], first_frame->linesize[1],
           first_frame->linesize[2], first_frame->width, first_frame->height,
           p_param->cfg_enc_params.conf_win_right,
           p_param->cfg_enc_params.conf_win_bottom,
           first_frame->color_primaries, first_frame->color_trc,
           first_frame->colorspace);

    if (SESSION_RUN_STATE_SEQ_CHANGE_OPENING != ctx->api_ctx.session_run_state)
    {
        // sequence change backup / restore encoder device handles, hw_id and
        // block device name, so no need to overwrite hw_id/blk_dev_name to user
        // set values
        ctx->api_ctx.hw_id = ctx->dev_enc_idx;

        ff_xcoder_strncpy(ctx->api_ctx.blk_dev_name, ctx->dev_blk_name,
                          NI_MAX_DEVICE_NAME_LEN);
    }

    p_param->rootBufId = (ishwframe) ? ((niFrameSurface1_t*)((uint8_t*)first_frame->data[3]))->ui16FrameIdx : 0;
    if (ishwframe)
    {
      ctx->api_ctx.hw_action = NI_CODEC_HW_ENABLE;
      ctx->api_ctx.sender_handle = (ni_device_handle_t)(
          (int64_t)(((niFrameSurface1_t *)((uint8_t *)first_frame->data[3]))
                        ->device_handle));
    }

    if (first_frame->hw_frames_ctx && ctx->api_ctx.hw_id == -1 &&
        0 == strcmp(ctx->api_ctx.blk_dev_name, "")) {
        ctx->api_ctx.hw_id = ni_get_cardno(first_frame);
        av_log(avctx, AV_LOG_VERBOSE,
               "xcoder_send_frame: hw_id -1, empty blk_dev_name, collocated "
               "to %d\n",
               ctx->api_ctx.hw_id);
    }

    // AUD insertion has to be handled differently in the firmware
    // if it is global header
    if (p_param->cfg_enc_params.EnableAUD) {
        if (avctx->flags & AV_CODEC_FLAG_GLOBAL_HEADER) {
            p_param->cfg_enc_params.EnableAUD = NI_ENABLE_AUD_FOR_GLOBAL_HEADER;
        }

        av_log(avctx, AV_LOG_VERBOSE,
               "%s: EnableAUD %d global header flag %d\n", __FUNCTION__,
               (p_param->cfg_enc_params.EnableAUD),
               (avctx->flags & AV_CODEC_FLAG_GLOBAL_HEADER) ? 1 : 0);
    }

    ret = ni_device_session_open(&ctx->api_ctx, NI_DEVICE_TYPE_ENCODER);

    // // As the file handle may change we need to assign back
    ctx->dev_xcoder_name = ctx->api_ctx.dev_xcoder_name;
    ctx->blk_xcoder_name = ctx->api_ctx.blk_xcoder_name;
    ctx->dev_enc_idx = ctx->api_ctx.hw_id;

    if (ret == NI_RETCODE_INVALID_PARAM)
    {
      av_log(avctx, AV_LOG_ERROR, "%s\n", ctx->api_ctx.param_err_msg);
    }
    if (ret != 0)
    {
      av_log(avctx, AV_LOG_ERROR, "Failed to open encoder (status = %d), "
             "resource unavailable\n", ret);
      ret = AVERROR_EXTERNAL;
      // xcoder_encode_close(avctx); will be called at codec close
      return ret;
    }
    else
    {
        av_log(avctx, AV_LOG_VERBOSE,
               "XCoder %s.%d (inst: %d) opened successfully\n",
               ctx->dev_xcoder_name, ctx->dev_enc_idx, ctx->api_ctx.session_id);
    }

    // set up ROI map if in ROI demo mode
    // Note: this is for demo purpose, and its direct access to QP map in
    //       session context is not the usual way to do ROI; the normal way is
    //       through side data of AVFrame in libavcodec, or aux data of ni_frame
    //       in libxcoder
    if (p_param->cfg_enc_params.roi_enable &&
        (1 == p_param->roi_demo_mode || 2 == p_param->roi_demo_mode)) {
        int sumQp = 0, ctu;
        // mode 1: Set QP for center 1/3 of picture to highest - lowest quality
        // the rest to lowest - highest quality;
        // mode non-1: reverse of mode 1
        int importanceLevelCentre = p_param->roi_demo_mode == 1 ? 40 : 10;
        int importanceLevelRest   = p_param->roi_demo_mode == 1 ? 10 : 40;
        if (QUADRA) {
            uint32_t block_size, max_cu_size, customMapSize;
            uint32_t mbWidth;
            uint32_t mbHeight;
            uint32_t numMbs;
            uint32_t roiMapBlockUnitSize;
            uint32_t entryPerMb;

            max_cu_size = avctx->codec_id == AV_CODEC_ID_H264 ? 16: 64;
            
            // AV1 non-8x8-aligned resolution is implicitly cropped due to Quadra HW limitation
            if (AV_CODEC_ID_AV1 == avctx->codec_id)
            {
                linesize_aligned = (linesize_aligned / 8) * 8;
                height_aligned = (height_aligned / 8) * 8;
            }
            
            // (ROI map version >= 1) each QP info takes 8-bit, represent 8 x 8
            // pixel block
            block_size =
                ((linesize_aligned + max_cu_size - 1) & (~(max_cu_size - 1))) *
                ((height_aligned + max_cu_size - 1) & (~(max_cu_size - 1))) /
                (8 * 8);

            // need to align to 64 bytes
            customMapSize = ((block_size + 63) & (~63));
            if (!ctx->api_ctx.roi_map) {
                ctx->api_ctx.roi_map =
                    (ni_enc_quad_roi_custom_map *)calloc(1, customMapSize);
            }
            if (!ctx->api_ctx.roi_map) {
                return AVERROR(ENOMEM);
            }

            // for H.264, select ROI Map Block Unit Size: 16x16
            // for H.265, select ROI Map Block Unit Size: 64x64
            roiMapBlockUnitSize = avctx->codec_id == AV_CODEC_ID_H264 ? 16 : 64;

            mbWidth =
                ((linesize_aligned + max_cu_size - 1) & (~(max_cu_size - 1))) /
                roiMapBlockUnitSize;
            mbHeight =
                ((height_aligned + max_cu_size - 1) & (~(max_cu_size - 1))) /
                roiMapBlockUnitSize;
            numMbs = mbWidth * mbHeight;

            // copy roi MBs QPs into custom map
            // number of qp info (8x8) per mb or ctb
            entryPerMb = (roiMapBlockUnitSize / 8) * (roiMapBlockUnitSize / 8);

            for (i = 0; i < numMbs; i++) {
                bool bIsCenter = (i % mbWidth > mbWidth / 3) && (i % mbWidth < mbWidth * 2 / 3);
                for (j = 0; j < entryPerMb; j++) {
                    /*
                    g_quad_roi_map[i*4+j].field.skip_flag = 0; // don't force
                    skip mode g_quad_roi_map[i*4+j].field.roiAbsQp_flag = 1; //
                    absolute QP g_quad_roi_map[i*4+j].field.qp_info = bIsCenter
                    ? importanceLevelCentre : importanceLevelRest;
                    */
                    ctx->api_ctx.roi_map[i * entryPerMb + j].field.ipcm_flag =
                        0; // don't force skip mode
                    ctx->api_ctx.roi_map[i * entryPerMb + j]
                        .field.roiAbsQp_flag = 1; // absolute QP
                    ctx->api_ctx.roi_map[i * entryPerMb + j].field.qp_info =
                        bIsCenter ? importanceLevelCentre : importanceLevelRest;
                }
                sumQp += ctx->api_ctx.roi_map[i * entryPerMb].field.qp_info;
            }
            ctx->api_ctx.roi_len = customMapSize;
            ctx->api_ctx.roi_avg_qp =
                // NOLINTNEXTLINE(clang-analyzer-core.DivideZero)
                (sumQp + (numMbs >> 1)) / numMbs; // round off
        } else if (avctx->codec_id == AV_CODEC_ID_H264) {
            // roi for H.264 is specified for 16x16 pixel macroblocks - 1 MB
            // is stored in each custom map entry

            // number of MBs in each row
            uint32_t mbWidth = (linesize_aligned + 16 - 1) >> 4;
            // number of MBs in each column
            uint32_t mbHeight = (height_aligned + 16 - 1) >> 4;
            uint32_t numMbs   = mbWidth * mbHeight;
            uint32_t customMapSize =
                sizeof(ni_enc_avc_roi_custom_map_t) * numMbs;
            g_avc_roi_map =
                (ni_enc_avc_roi_custom_map_t *)calloc(1, customMapSize);
            if (!g_avc_roi_map) {
                return AVERROR(ENOMEM);
            }

            // copy roi MBs QPs into custom map
            for (i = 0; i < numMbs; i++) {
                if ((i % mbWidth > mbWidth / 3) &&
                    (i % mbWidth < mbWidth * 2 / 3)) {
                    g_avc_roi_map[i].field.mb_qp = importanceLevelCentre;
                } else {
                    g_avc_roi_map[i].field.mb_qp = importanceLevelRest;
                }
                sumQp += g_avc_roi_map[i].field.mb_qp;
            }
            ctx->api_ctx.roi_len = customMapSize;
            ctx->api_ctx.roi_avg_qp =
                (sumQp + (numMbs >> 1)) / numMbs; // round off
        } else if (avctx->codec_id == AV_CODEC_ID_HEVC) {
            // roi for H.265 is specified for 32x32 pixel subCTU blocks - 4
            // subCTU QPs are stored in each custom CTU map entry

            // number of CTUs in each row
            uint32_t ctuWidth = (linesize_aligned + 64 - 1) >> 6;
            // number of CTUs in each column
            uint32_t ctuHeight = (height_aligned + 64 - 1) >> 6;
            // number of sub CTUs in each row
            uint32_t subCtuWidth = ctuWidth * 2;
            // number of CTUs in each column
            uint32_t subCtuHeight = ctuHeight * 2;
            uint32_t numSubCtus   = subCtuWidth * subCtuHeight;

            g_hevc_sub_ctu_roi_buf = (uint8_t *)malloc(numSubCtus);
            if (!g_hevc_sub_ctu_roi_buf) {
                return AVERROR(ENOMEM);
            }
            for (i = 0; i < numSubCtus; i++) {
                if ((i % subCtuWidth > subCtuWidth / 3) &&
                    (i % subCtuWidth < subCtuWidth * 2 / 3)) {
                    g_hevc_sub_ctu_roi_buf[i] = importanceLevelCentre;
                } else {
                    g_hevc_sub_ctu_roi_buf[i] = importanceLevelRest;
                }
            }
            g_hevc_roi_map = (ni_enc_hevc_roi_custom_map_t *)calloc(
                1, sizeof(ni_enc_hevc_roi_custom_map_t) * ctuWidth * ctuHeight);
            if (!g_hevc_roi_map) {
                return AVERROR(ENOMEM);
            }

            for (i = 0; i < ctuHeight; i++) {
                uint8_t *ptr = &g_hevc_sub_ctu_roi_buf[subCtuWidth * i * 2];
                for (j = 0; j < ctuWidth; j++, ptr += 2) {
                    ctu = (int)(i * ctuWidth + j);
                    g_hevc_roi_map[ctu].field.sub_ctu_qp_0 = *ptr;
                    g_hevc_roi_map[ctu].field.sub_ctu_qp_1 = *(ptr + 1);
                    g_hevc_roi_map[ctu].field.sub_ctu_qp_2 =
                        *(ptr + subCtuWidth);
                    g_hevc_roi_map[ctu].field.sub_ctu_qp_3 =
                        *(ptr + subCtuWidth + 1);
                    sumQp += (g_hevc_roi_map[ctu].field.sub_ctu_qp_0 +
                              g_hevc_roi_map[ctu].field.sub_ctu_qp_1 +
                              g_hevc_roi_map[ctu].field.sub_ctu_qp_2 +
                              g_hevc_roi_map[ctu].field.sub_ctu_qp_3);
                }
            }
            ctx->api_ctx.roi_len =
                ctuWidth * ctuHeight * sizeof(ni_enc_hevc_roi_custom_map_t);
            ctx->api_ctx.roi_avg_qp =
                (sumQp + (numSubCtus >> 1)) / numSubCtus; // round off.
        }
    }
  } //end if(first_frame && ctx->started == 0)

  av_log(avctx, AV_LOG_VERBOSE, "XCoder send frame, pkt_size %d\n",
         frame ? frame->pkt_size : -1);
#if 0
  if (frame)
    printf("*** NI enc In avframe pts: %lld  pkt_dts : %lld  best_effort : %lld \n", frame->pts, frame->pkt_dts, frame->best_effort_timestamp);
#endif

  if (ctx->encoder_flushing)
  {
     if (! frame && is_input_fifo_empty(ctx))
    {
      av_log(avctx, AV_LOG_DEBUG, "XCoder EOF: null frame && fifo empty\n");
      return AVERROR_EOF;
    }
  }

  if (! frame)
  {
    if (is_input_fifo_empty(ctx))
    {
      ctx->eos_fme_received = 1;
      av_log(avctx, AV_LOG_DEBUG, "null frame, eos_fme_received = 1\n");
    }
    else
    {
      avctx->internal->draining = 0;
      av_log(avctx, AV_LOG_DEBUG, "null frame, but fifo not empty, clear draining = 0\n");
    }
  }
  else
  {
    av_log(avctx, AV_LOG_DEBUG, "XCoder send frame #%"PRIu64"\n",
           ctx->api_ctx.frame_num);

    // queue up the frame if fifo is NOT empty, or: sequence change ongoing !
    if (! is_input_fifo_empty(ctx) ||
        SESSION_RUN_STATE_SEQ_CHANGE_DRAINING == ctx->api_ctx.session_run_state)
    {
      enqueue_frame(avctx, frame);

      if (SESSION_RUN_STATE_SEQ_CHANGE_DRAINING ==
          ctx->api_ctx.session_run_state)
      {
        av_log(avctx, AV_LOG_TRACE, "XCoder doing sequence change, frame #%"PRIu64" "
               "queued and return 0 !\n", ctx->api_ctx.frame_num);
        return 0;
      }
    }
    else if (frame != &ctx->buffered_fme)
    {
      // For FFmpeg-n4.4+ receive_packet interface the buffered_fme is fetched from
      // ff_alloc_get_frame rather than passed as function argument. So we need to
      // judge whether they are the same object. If they are the same NO need to
      // reference.
      ret = av_frame_ref(&ctx->buffered_fme, frame);
    }
  }

  if (ctx->started == 0)
  {
    ctx->api_fme.data.frame.start_of_stream = 1;
    ctx->started = 1;
    //#ifdef QUADRA
    #if 0
    // reconfig self-test code
    ctx->reconfigCount = 0;
    if (p_param->reconf_demo_mode == XCODER_TEST_RECONF_BR)
    {
        p_param->reconf_hash[ctx->reconfigCount][0] =  25;
        //p_param->reconf_hash[ctx->reconfigCount][1] =  100000000;
        p_param->reconf_hash[ctx->reconfigCount][1] =  5000000;
        p_param->reconf_hash[ctx->reconfigCount+1][0] =  -1;
    }
    /*
    else if (p_param->reconf_demo_mode == XCODER_TEST_RECONF_INTRAPRD)
    {
        p_param->reconf_hash[ctx->reconfigCount][0] =  25;
        p_param->reconf_hash[ctx->reconfigCount][1] =  10; // intraQP
        p_param->reconf_hash[ctx->reconfigCount][2] =  5;  // intraPeriod
        p_param->reconf_hash[ctx->reconfigCount][3] =  1;  // repeatHeaders
        p_param->reconf_hash[ctx->reconfigCount+1][0] =  -1;
    }
    */
    else if (p_param->reconf_demo_mode == XCODER_TEST_RECONF_VUI_HRD)
    {
        p_param->reconf_hash[ctx->reconfigCount][0] =  25;
        p_param->reconf_hash[ctx->reconfigCount][1] =  1;   // colorDescPresent
        p_param->reconf_hash[ctx->reconfigCount][2] =  255; // colorPrimaries
        p_param->reconf_hash[ctx->reconfigCount][3] =  255; // colorTrc
        p_param->reconf_hash[ctx->reconfigCount][4] =  255; // colorSpace
        p_param->reconf_hash[ctx->reconfigCount][5] =  16;  // aspectRatioWidth
        p_param->reconf_hash[ctx->reconfigCount][6] =  11;  // aspectRatioHeight
        p_param->reconf_hash[ctx->reconfigCount][7] =  1;   // videoFullRange
        p_param->reconf_hash[ctx->reconfigCount+1][0] =  -1;
    }
    else if (p_param->reconf_demo_mode == XCODER_TEST_RECONF_LONG_TERM_REF)
    {
        p_param->reconf_hash[ctx->reconfigCount][0] =  10;  // long term ref frame poc
        p_param->reconf_hash[ctx->reconfigCount][1] =  1; // set LTR
        p_param->reconf_hash[ctx->reconfigCount][2] =  0; // ref LTR is not supported
        p_param->reconf_hash[ctx->reconfigCount+1][0] =  13;  // long term ref frame poc
        p_param->reconf_hash[ctx->reconfigCount+1][1] =  1; // set LTR
        p_param->reconf_hash[ctx->reconfigCount+1][2] =  0;
        p_param->reconf_hash[ctx->reconfigCount+2][0] =  16;  // long term ref frame poc
        p_param->reconf_hash[ctx->reconfigCount+2][1] =  1;  // set LTR
        p_param->reconf_hash[ctx->reconfigCount+2][2] =  0;
        p_param->reconf_hash[ctx->reconfigCount+3][0] =  17;  // long term ref frame poc
        p_param->reconf_hash[ctx->reconfigCount+3][1] =  1;  // set LTR
        p_param->reconf_hash[ctx->reconfigCount+3][2] =  0;
        p_param->reconf_hash[ctx->reconfigCount+4][0] =  24;  // long term ref frame poc
        p_param->reconf_hash[ctx->reconfigCount+4][1] =  1; // set LTR
        p_param->reconf_hash[ctx->reconfigCount+4][2] =  0;
        p_param->reconf_hash[ctx->reconfigCount+5][0] =  25;  // long term ref frame poc
        p_param->reconf_hash[ctx->reconfigCount+5][1] =  1; // set LTR
        p_param->reconf_hash[ctx->reconfigCount+5][2] =  0;
        p_param->reconf_hash[ctx->reconfigCount+6][0] =  -1; // stop
    }
    /*
    else if (p_param->reconf_demo_mode == XCODER_TEST_RECONF_RC_MIN_MAX_QP)
    {
        p_param->reconf_hash[ctx->reconfigCount][0] =  25;
        p_param->reconf_hash[ctx->reconfigCount][1] =  10; // minQpI
        p_param->reconf_hash[ctx->reconfigCount][2] =  12; // maxQpI
        p_param->reconf_hash[ctx->reconfigCount][3] =  30; // minQpPB
        p_param->reconf_hash[ctx->reconfigCount][4] =  32; // maxQpPB
        p_param->reconf_hash[ctx->reconfigCount+1][0] =  -1;
    }
    */
    else if (p_param->reconf_demo_mode == XCODER_TEST_RECONF_LTR_INTERVAL)
    {
        p_param->reconf_hash[ctx->reconfigCount][0] =  25; // current frame
        p_param->reconf_hash[ctx->reconfigCount][1] =  5; // reconfig LTR interval to 5
        p_param->reconf_hash[ctx->reconfigCount+1][0] =  -1;
    }
    else if (p_param->reconf_demo_mode == XCODER_TEST_INVALID_REF_FRAME)
    {
        p_param->reconf_hash[ctx->reconfigCount][0] =  24; // current frame
        p_param->reconf_hash[ctx->reconfigCount][1] =  21; // invalidate ref frame poc
        p_param->reconf_hash[ctx->reconfigCount+1][0] =  -1;
    }
    /*
    else if (p_param->reconf_demo_mode == XCODER_TEST_FORCE_IDR_FRAME)
    {
        p_param->reconf_hash[ctx->reconfigCount][0] =  10;
        p_param->reconf_hash[ctx->reconfigCount+1][0] =  -1;
    }
    */
#endif
    }
    else if (ctx->api_ctx.session_run_state == SESSION_RUN_STATE_SEQ_CHANGE_OPENING) {
        ctx->api_fme.data.frame.start_of_stream = 1;
    }
    else {
        ctx->api_fme.data.frame.start_of_stream = 0;
    }
    if (is_input_fifo_empty(ctx)) {
        av_log(avctx, AV_LOG_DEBUG,
               "no frame in fifo to send, just send/receive ..\n");
        if (ctx->eos_fme_received) {
            av_log(avctx, AV_LOG_DEBUG,
                   "no frame in fifo to send, send eos ..\n");
        }
    } else {
        av_log(avctx, AV_LOG_DEBUG, "av_fifo_generic_peek fme\n");
        av_fifo_generic_peek(ctx->fme_fifo, &ctx->buffered_fme, sizeof(AVFrame),
                             NULL);
        ctx->buffered_fme.extended_data = ctx->buffered_fme.data;
    }

    if (!ctx->eos_fme_received) {
        int8_t bit_depth = 1;
        ishwframe        = ctx->buffered_fme.format == AV_PIX_FMT_NI_QUAD;
        if (ishwframe) {
            // Superframe early cleanup of unused outputs
            niFrameSurface1_t *pOutExtra;
            if (ctx->buffered_fme.buf[1]) {
                // NOLINTNEXTLINE(clang-diagnostic-incompatible-pointer-types)
                pOutExtra= (niFrameSurface1_t *)ctx->buffered_fme.buf[1]->data;
                if (pOutExtra->ui16FrameIdx != 0) {
                    av_log(avctx, AV_LOG_DEBUG, "Unref unused index %d\n",
                           pOutExtra->ui16FrameIdx);
                } else {
                    av_log(avctx, AV_LOG_ERROR,
                           "ERROR: Should not be getting superframe with dead "
                           "outputs\n");
                }
                av_buffer_unref(&ctx->buffered_fme.buf[1]);
                if (ctx->buffered_fme.buf[2]) {
                    // NOLINTNEXTLINE(clang-diagnostic-incompatible-pointer-types)
                    pOutExtra = (niFrameSurface1_t *)ctx->buffered_fme.buf[2]->data;
                    if (pOutExtra->ui16FrameIdx != 0) {
                        av_log(avctx, AV_LOG_DEBUG, "Unref unused index %d\n",
                               pOutExtra->ui16FrameIdx);
                    } else {
                        av_log(
                            avctx, AV_LOG_ERROR,
                            "ERROR: Should not be getting superframe with dead "
                            "outputs\n");
                    }
                    av_buffer_unref(&ctx->buffered_fme.buf[2]);
                }
            }

            pOutExtra = (niFrameSurface1_t *)ctx->buffered_fme.data[3];
            bit_depth = pOutExtra->bit_depth;

            switch (bit_depth) {
            case 1:
            case 2:
                break;
            default:
                av_log(avctx, AV_LOG_ERROR, "ERROR: Unknown bit depth %d!\n", bit_depth);
                return AVERROR_INVALIDDATA;
            }
        } else {
            if (AV_PIX_FMT_YUV420P10BE == ctx->buffered_fme.format ||
                AV_PIX_FMT_YUV420P10LE == ctx->buffered_fme.format ||
                AV_PIX_FMT_P010LE == ctx->buffered_fme.format) {
                bit_depth = 2;
            }
        }

#ifdef SEQUENCE_CHANGE_INJECT
        if ((ctx->buffered_fme.height && ctx->buffered_fme.width) &&
            (ctx->buffered_fme.height != avctx->height ||
             ctx->buffered_fme.width != avctx->width ||
             bit_depth != ctx->api_ctx.bit_depth_factor ||
             (ctx->api_ctx.frame_num > 0 && ctx->api_ctx.frame_num % 10 == 0)))
#else
        if ((ctx->buffered_fme.height && ctx->buffered_fme.width) &&
            (ctx->buffered_fme.height != avctx->height ||
             ctx->buffered_fme.width != avctx->width ||
             bit_depth != ctx->api_ctx.bit_depth_factor))
#endif
        {
            av_log(avctx, AV_LOG_INFO,
                   "xcoder_send_frame resolution change %dx%d "
                   "-> %dx%d or bit depth change %d -> %d\n",
                   avctx->width, avctx->height, ctx->buffered_fme.width,
                   ctx->buffered_fme.height, ctx->api_ctx.bit_depth_factor,
                   bit_depth);

            ctx->api_ctx.session_run_state =
                SESSION_RUN_STATE_SEQ_CHANGE_DRAINING;
            ctx->eos_fme_received = 1;

            // have to queue this frame if not done so: an empty queue
            if (is_input_fifo_empty(ctx)) {
                av_log(avctx, AV_LOG_TRACE,
                       "resolution change when fifo empty, frame "
                       "#%" PRIu64 " being queued ..\n",
                       ctx->api_ctx.frame_num);
                // unref buffered frame (this buffered frame is taken from input
                // AVFrame) because we are going to send EOS (instead of sending
                // buffered frame)
                if (frame != &ctx->buffered_fme) {
                    // For FFmpeg-n4.4+ receive_packet interface the buffered_fme is fetched from
                    // ff_alloc_get_frame rather than passed as function argument. So we need to
                    // judge whether they are the same object. If they are the same do NOT unreference
                    // any of them because we need to enqueue it later.
                    av_frame_unref(&ctx->buffered_fme);
                }
                enqueue_frame(avctx, frame);
            }
        }
    }

    ctx->api_fme.data.frame.preferred_characteristics_data_len = 0;
    ctx->api_fme.data.frame.end_of_stream                      = 0;
    ctx->api_fme.data.frame.force_key_frame =
        ctx->api_fme.data.frame.use_cur_src_as_long_term_pic =
            ctx->api_fme.data.frame.use_long_term_ref = 0;

    ctx->api_fme.data.frame.sei_total_len =
        ctx->api_fme.data.frame.sei_cc_offset = ctx->api_fme.data.frame
                                                    .sei_cc_len =
            ctx->api_fme.data.frame.sei_hdr_mastering_display_color_vol_offset =
                ctx->api_fme.data.frame
                    .sei_hdr_mastering_display_color_vol_len =
                    ctx->api_fme.data.frame
                        .sei_hdr_content_light_level_info_offset =
                        ctx->api_fme.data.frame
                            .sei_hdr_content_light_level_info_len =
                            ctx->api_fme.data.frame.sei_hdr_plus_offset =
                                ctx->api_fme.data.frame.sei_hdr_plus_len = 0;

    ctx->api_fme.data.frame.roi_len      = 0;
    ctx->api_fme.data.frame.reconf_len   = 0;
    ctx->api_fme.data.frame.force_pic_qp = 0;

    if (SESSION_RUN_STATE_SEQ_CHANGE_DRAINING ==
            ctx->api_ctx.session_run_state ||
        (ctx->eos_fme_received && is_input_fifo_empty(ctx))) {
        av_log(avctx, AV_LOG_VERBOSE, "XCoder start flushing\n");
        ctx->api_fme.data.frame.end_of_stream = 1;
        ctx->encoder_flushing                 = 1;
    } else {
        format_in_use = ctx->buffered_fme.format;
        // NETINT_INTERNAL - currently only for internal testing
        // reset encoder change data buffer for reconf parameters
        if (p_param->reconf_demo_mode > XCODER_TEST_RECONF_OFF &&
            p_param->reconf_demo_mode < XCODER_TEST_RECONF_END) {
            memset(ctx->api_ctx.enc_change_params, 0,
                   sizeof(ni_encoder_change_params_t));
        }

        // extra data starts with metadata header, various aux data sizes
        // have been reset above
        ctx->api_fme.data.frame.extra_data_len =
            NI_APP_ENC_FRAME_META_DATA_SIZE;

        ctx->api_fme.data.frame.ni_pict_type    = 0;

        switch (p_param->reconf_demo_mode) {
        case XCODER_TEST_RECONF_BR:
            if (ctx->api_ctx.frame_num ==
                p_param->reconf_hash[ctx->reconfigCount][0]) {
                aux_data = ni_frame_new_aux_data(
                    &dec_frame, NI_FRAME_AUX_DATA_BITRATE, sizeof(int32_t));
                if (!aux_data) {
                    return AVERROR(ENOMEM);
                }
                *((int32_t *)aux_data->data) =
                    p_param->reconf_hash[ctx->reconfigCount][1];

                ctx->reconfigCount++;
                if (p_param->cfg_enc_params.hrdEnable)
                {
                    ctx->api_fme.data.frame.force_key_frame = 1;
                    ctx->api_fme.data.frame.ni_pict_type = PIC_TYPE_IDR;
                }
            }
            break;
        /* // not required by customer
        case XCODER_TEST_RECONF_INTRAPRD:
            if (ctx->api_ctx.frame_num ==
                p_param->reconf_hash[ctx->reconfigCount][0]) {
                ctx->api_ctx.enc_change_params->enable_option |=
                    NI_SET_CHANGE_PARAM_INTRA_PARAM;
                ctx->api_ctx.enc_change_params->intraQP =
                    p_param->reconf_hash[ctx->reconfigCount][1];
                ctx->api_ctx.enc_change_params->intraPeriod =
                    p_param->reconf_hash[ctx->reconfigCount][2];
                ctx->api_ctx.enc_change_params->repeatHeaders =
                    p_param->reconf_hash[ctx->reconfigCount][3];
                av_log(avctx, AV_LOG_TRACE,
                       "xcoder_send_frame: frame #%lu reconf "
                       "intraQP %d intraPeriod %d repeatHeaders %d\n",
                       ctx->api_ctx.frame_num,
                       ctx->api_ctx.enc_change_params->intraQP,
                       ctx->api_ctx.enc_change_params->intraPeriod,
                       ctx->api_ctx.enc_change_params->repeatHeaders);

                ctx->api_fme.data.frame.reconf_len =
                    sizeof(ni_encoder_change_params_t);
                ctx->reconfigCount++;
            }
            break;
        */
        // reconfig VUI parameters
        case XCODER_TEST_RECONF_VUI_HRD:
            if (ctx->api_ctx.frame_num ==
                p_param->reconf_hash[ctx->reconfigCount][0]) {
                aux_data = ni_frame_new_aux_data(&dec_frame,
                                                 NI_FRAME_AUX_DATA_VUI,
                                                 sizeof(ni_vui_hrd_t));
                if (!aux_data) {
                    return AVERROR(ENOMEM);
                }
                ni_vui_hrd_t *vui = (ni_vui_hrd_t *)aux_data->data;
                vui->colorDescPresent =
                    p_param->reconf_hash[ctx->reconfigCount][1];
                vui->colorPrimaries =
                    p_param->reconf_hash[ctx->reconfigCount][2];
                vui->colorTrc =
                    p_param->reconf_hash[ctx->reconfigCount][3];
                vui->colorSpace =
                    p_param->reconf_hash[ctx->reconfigCount][4];
                vui->aspectRatioWidth =
                    p_param->reconf_hash[ctx->reconfigCount][5];
                vui->aspectRatioHeight =
                    p_param->reconf_hash[ctx->reconfigCount][6];
                vui->videoFullRange =
                    p_param->reconf_hash[ctx->reconfigCount][7];
                av_log(avctx, AV_LOG_TRACE,
                       "xcoder_send_frame: frame #%lu reconf "
                       "vui colorDescPresent %d colorPrimaries %d "
                        "colorTrc %d colorSpace %d aspectRatioWidth %d "
                       "aspectRatioHeight %d videoFullRange %d\n",
                       ctx->api_ctx.frame_num, vui->colorDescPresent,
                       vui->colorPrimaries, vui->colorTrc,
                       vui->colorSpace, vui->aspectRatioWidth,
                       vui->aspectRatioHeight, vui->videoFullRange);

                ctx->reconfigCount++;
            }
            break;
        // long term ref
        case XCODER_TEST_RECONF_LONG_TERM_REF:
            // the reconf file data line format for this is:
            // <frame-number>:useCurSrcAsLongtermPic,useLongtermRef where
            // values will stay the same on every frame until changed.
            if (ctx->api_ctx.frame_num ==
                p_param->reconf_hash[ctx->reconfigCount][0]) {
                AVFrameSideData *ltr_sd;
                AVNetintLongTermRef *p_ltr;
                ltr_sd = av_frame_new_side_data(
                    &ctx->buffered_fme, AV_FRAME_DATA_NETINT_LONG_TERM_REF,
                    sizeof(AVNetintLongTermRef));
                if (ltr_sd) {
                    p_ltr = (AVNetintLongTermRef *)ltr_sd->data;
                    p_ltr->use_cur_src_as_long_term_pic =
                        (uint8_t)p_param->reconf_hash[ctx->reconfigCount][1];
                    p_ltr->use_long_term_ref =
                        (uint8_t)p_param->reconf_hash[ctx->reconfigCount][2];
                    av_log(avctx, AV_LOG_TRACE,
                           "xcoder_send_frame: frame #%lu metadata "
                           "use_cur_src_as_long_term_pic %d use_long_term_ref "
                           "%d\n",
                           ctx->api_ctx.frame_num,
                           p_ltr->use_cur_src_as_long_term_pic,
                           p_ltr->use_long_term_ref);
                }
                ctx->reconfigCount++;
            }
            break;
            /* // not required by customer
            // reconfig min / max QP
            case XCODER_TEST_RECONF_RC_MIN_MAX_QP:
                if (ctx->api_ctx.frame_num ==
                    p_param->reconf_hash[ctx->reconfigCount][0]) {
                    ctx->api_ctx.enc_change_params->enable_option |=
                        NI_SET_CHANGE_PARAM_RC_MIN_MAX_QP;
                    ctx->api_ctx.enc_change_params->minQpI =
                        p_param->reconf_hash[ctx->reconfigCount][1];
                    ctx->api_ctx.enc_change_params->maxQpI =
                        p_param->reconf_hash[ctx->reconfigCount][2];
                    ctx->api_ctx.enc_change_params->minQpPB =
                        p_param->reconf_hash[ctx->reconfigCount][3];
                    ctx->api_ctx.enc_change_params->maxQpPB =
                        p_param->reconf_hash[ctx->reconfigCount][4];

                    ctx->api_fme.data.frame.reconf_len =
                        sizeof(ni_encoder_change_params_t);
                    ctx->reconfigCount++;
                }
                break;
            */
#ifdef QUADRA
        // reconfig LTR interval
        case XCODER_TEST_RECONF_LTR_INTERVAL:
            if (ctx->api_ctx.frame_num ==
                p_param->reconf_hash[ctx->reconfigCount][0]) {
                aux_data = ni_frame_new_aux_data(&dec_frame,
                                                 NI_FRAME_AUX_DATA_LTR_INTERVAL,
                                                 sizeof(int32_t));
                if (!aux_data) {
                    return AVERROR(ENOMEM);
                }
                *((int32_t *)aux_data->data) =
                    p_param->reconf_hash[ctx->reconfigCount][1];
                av_log(avctx, AV_LOG_TRACE,
                       "xcoder_send_frame: frame #%lu reconf "
                       "ltrInterval %d\n",
                       ctx->api_ctx.frame_num,
                       p_param->reconf_hash[ctx->reconfigCount][1]);

                ctx->reconfigCount++;
            }
            break;
        // invalidate reference frames
        case XCODER_TEST_INVALID_REF_FRAME:
            if (ctx->api_ctx.frame_num ==
                p_param->reconf_hash[ctx->reconfigCount][0]) {
                aux_data = ni_frame_new_aux_data(
                    &dec_frame, NI_FRAME_AUX_DATA_INVALID_REF_FRAME,
                    sizeof(int32_t));
                if (!aux_data) {
                    return AVERROR(ENOMEM);
                }
                *((int32_t *)aux_data->data) =
                    p_param->reconf_hash[ctx->reconfigCount][1];
                av_log(avctx, AV_LOG_TRACE,
                       "xcoder_send_frame: frame #%lu reconf "
                       "invalidFrameNum %d\n",
                       ctx->api_ctx.frame_num,
                       p_param->reconf_hash[ctx->reconfigCount][1]);

                ctx->reconfigCount++;
            }
            break;
        // reconfig framerate
        case XCODER_TEST_RECONF_FRAMERATE:
            if (ctx->api_ctx.frame_num ==
                p_param->reconf_hash[ctx->reconfigCount][0]) {
                ni_framerate_t *framerate;

                aux_data = ni_frame_new_aux_data(&dec_frame,
                                                 NI_FRAME_AUX_DATA_FRAMERATE,
                                                 sizeof(ni_framerate_t));
                if (!aux_data) {
                    return AVERROR(ENOMEM);
                }

                framerate = (ni_framerate_t *)aux_data->data;
                framerate->framerate_num =
                    (int32_t)p_param->reconf_hash[ctx->reconfigCount][1];
                framerate->framerate_denom =
                    (int32_t)p_param->reconf_hash[ctx->reconfigCount][2];
                av_log(avctx, AV_LOG_TRACE,
                       "xcoder_send_frame: frame #%lu reconf "
                       "framerate (%d/%d)\n",
                       ctx->api_ctx.frame_num, framerate->framerate_num,
                       framerate->framerate_denom);
                ctx->reconfigCount++;
            }
            break;
        case XCODER_TEST_RECONF_MAX_FRAME_SIZE:
            if (ctx->api_ctx.frame_num ==
                p_param->reconf_hash[ctx->reconfigCount][0]) {
                aux_data = ni_frame_new_aux_data(
                    &dec_frame, NI_FRAME_AUX_DATA_MAX_FRAME_SIZE, sizeof(int32_t));
                if (!aux_data) {
                    return AVERROR(ENOMEM);
                }
                *((int32_t *)aux_data->data) =
                    p_param->reconf_hash[ctx->reconfigCount][1];
                av_log(avctx, AV_LOG_TRACE,
                       "xcoder_send_frame: frame #%lu reconf "
                       "maxFrameSize %d\n",
                       ctx->api_ctx.frame_num,
                       p_param->reconf_hash[ctx->reconfigCount][1]);

                ctx->reconfigCount++;
            }
            break;
            // force IDR frame test code
            /*
            case XCODER_TEST_FORCE_IDR_FRAME:
            if (ctx->api_ctx.frame_num ==
            p_param->reconf_hash[ctx->reconfigCount][0])
            {
                ctx->api_fme.data.frame.force_key_frame = 1;
                ctx->api_fme.data.frame.ni_pict_type = PIC_TYPE_IDR;
                ctx->reconfigCount ++;
            }
            break;
            */
            // force IDR frame through API test code
        case XCODER_TEST_FORCE_IDR_FRAME:
            if (ctx->api_ctx.frame_num ==
                p_param->reconf_hash[ctx->reconfigCount][0]) {
                ni_force_idr_frame_type(&ctx->api_ctx);
                av_log(avctx, AV_LOG_TRACE,
                       "xcoder_send_frame: frame #%lu force IDR frame\n",
                       ctx->api_ctx.frame_num);

                ctx->reconfigCount++;
            }
            break;
            // reconfig bit rate through API test code
        case XCODER_TEST_RECONF_BR_API:
            if (ctx->api_ctx.frame_num ==
                p_param->reconf_hash[ctx->reconfigCount][0]) {
                ni_reconfig_bitrate(
                    &ctx->api_ctx, p_param->reconf_hash[ctx->reconfigCount][1]);
                av_log(avctx, AV_LOG_TRACE,
                       "xcoder_send_frame: frame #%lu API reconfig BR %d\n",
                       ctx->api_ctx.frame_num,
                       p_param->reconf_hash[ctx->reconfigCount][1]);

                ctx->reconfigCount++;
            }
            break;
        case XCODER_TEST_RECONF_VUI_HRD_API:
            if (ctx->api_ctx.frame_num ==
                p_param->reconf_hash[ctx->reconfigCount][0]) {
                ni_vui_hrd_t vui;
                vui.colorDescPresent =
                    p_param->reconf_hash[ctx->reconfigCount][1];
                vui.colorPrimaries =
                    p_param->reconf_hash[ctx->reconfigCount][2];
                vui.colorTrc =
                    p_param->reconf_hash[ctx->reconfigCount][3];
                vui.colorSpace =
                    p_param->reconf_hash[ctx->reconfigCount][4];
                vui.aspectRatioWidth =
                    p_param->reconf_hash[ctx->reconfigCount][5];
                vui.aspectRatioHeight =
                    p_param->reconf_hash[ctx->reconfigCount][6];
                vui.videoFullRange =
                    p_param->reconf_hash[ctx->reconfigCount][7];
                ni_reconfig_vui(&ctx->api_ctx, &vui);
                av_log(avctx, AV_LOG_TRACE,
                       "xcoder_send_frame: frame #%lu reconf "
                       "vui colorDescPresent %d colorPrimaries %d "
                       "colorTrc %d colorSpace %d aspectRatioWidth %d "
                       "aspectRatioHeight %d videoFullRange %d\n",
                       ctx->api_ctx.frame_num, vui.colorDescPresent,
                       vui.colorPrimaries, vui.colorTrc,
                       vui.colorSpace, vui.aspectRatioWidth,
                       vui.aspectRatioHeight, vui.videoFullRange);
                ctx->reconfigCount++;
            }
            break;
        case XCODER_TEST_RECONF_LTR_API:
            if (ctx->api_ctx.frame_num ==
                p_param->reconf_hash[ctx->reconfigCount][0]) {
                ni_long_term_ref_t ltr;
                ltr.use_cur_src_as_long_term_pic =
                    (uint8_t)p_param->reconf_hash[ctx->reconfigCount][1];
                ltr.use_long_term_ref =
                    (uint8_t)p_param->reconf_hash[ctx->reconfigCount][2];

                ni_set_ltr(&ctx->api_ctx, &ltr);
                av_log(avctx, AV_LOG_TRACE,
                       "xcoder_send_frame(): frame #%lu API set LTR\n",
                       ctx->api_ctx.frame_num);
                ctx->reconfigCount++;
            }
            break;
        case XCODER_TEST_RECONF_LTR_INTERVAL_API:
            if (ctx->api_ctx.frame_num ==
                p_param->reconf_hash[ctx->reconfigCount][0]) {
                ni_set_ltr_interval(
                    &ctx->api_ctx, p_param->reconf_hash[ctx->reconfigCount][1]);
                av_log(
                    avctx, AV_LOG_TRACE,
                    "xcoder_send_frame(): frame #%lu API set LTR interval %d\n",
                    ctx->api_ctx.frame_num,
                    p_param->reconf_hash[ctx->reconfigCount][1]);
                ctx->reconfigCount++;
            }
            break;
        case XCODER_TEST_INVALID_REF_FRAME_API:
            if (ctx->api_ctx.frame_num ==
                p_param->reconf_hash[ctx->reconfigCount][0]) {
                ni_set_frame_ref_invalid(
                    &ctx->api_ctx, p_param->reconf_hash[ctx->reconfigCount][1]);
                av_log(
                    avctx, AV_LOG_TRACE,
                    "xcoder_send_frame(): frame #%lu API set frame ref invalid "
                    "%d\n",
                    ctx->api_ctx.frame_num,
                    p_param->reconf_hash[ctx->reconfigCount][1]);
                ctx->reconfigCount++;
            }
            break;
        case XCODER_TEST_RECONF_FRAMERATE_API:
            if (ctx->api_ctx.frame_num ==
                p_param->reconf_hash[ctx->reconfigCount][0]) {
                ni_framerate_t framerate;
                framerate.framerate_num =
                    (int32_t)p_param->reconf_hash[ctx->reconfigCount][1];
                framerate.framerate_denom =
                    (int32_t)p_param->reconf_hash[ctx->reconfigCount][2];
                ni_reconfig_framerate(&ctx->api_ctx, &framerate);
                av_log(avctx, AV_LOG_TRACE,
                       "xcoder_send_frame: frame #%lu API reconfig framerate "
                       "(%d/%d)\n",
                       ctx->api_ctx.frame_num,
                       p_param->reconf_hash[ctx->reconfigCount][1],
                       p_param->reconf_hash[ctx->reconfigCount][2]);

                ctx->reconfigCount++;
            }
            break;
        case XCODER_TEST_RECONF_MAX_FRAME_SIZE_API:
            if (ctx->api_ctx.frame_num ==
                p_param->reconf_hash[ctx->reconfigCount][0]) {
                ni_reconfig_max_frame_size(
                    &ctx->api_ctx, p_param->reconf_hash[ctx->reconfigCount][1]);
                av_log(avctx, AV_LOG_TRACE,
                       "xcoder_send_frame: frame #%lu API reconfig maxFrameSize %d\n",
                       ctx->api_ctx.frame_num,
                       p_param->reconf_hash[ctx->reconfigCount][1]);

                ctx->reconfigCount++;
            }
            break;            
#endif
      case XCODER_TEST_RECONF_OFF:
      default:
        ;
      }

    // long term reference frame support
    side_data = av_frame_get_side_data(&ctx->buffered_fme,
                                       AV_FRAME_DATA_NETINT_LONG_TERM_REF);
    if (side_data && (side_data->size == sizeof(AVNetintLongTermRef))) {
        aux_data =
            ni_frame_new_aux_data(&dec_frame, NI_FRAME_AUX_DATA_LONG_TERM_REF,
                                  sizeof(ni_long_term_ref_t));
        if (aux_data) {
            memcpy(aux_data->data, side_data->data, side_data->size);
        }
    }

    // support VFR
    if (ctx->api_param.enable_vfr)
    {
        int cur_fps = 0, pre_fps = 0;

        pre_fps = ctx->api_ctx.prev_fps;

        if (ctx->buffered_fme.pts > ctx->api_ctx.prev_pts)
        {
          ctx->api_ctx.passed_time_in_timebase_unit += ctx->buffered_fme.pts - ctx->api_ctx.prev_pts;
          ctx->api_ctx.count_frame_num_in_sec++;
          //change the FrameRate for VFR
          //1. Only when the fps change, setting the new bitrate
          //2. The interval between two framerate chagne settings shall be greater than 1 seconds
          //   or at the start the transcoding
          if (ctx->api_ctx.passed_time_in_timebase_unit >= (avctx->time_base.den / avctx->time_base.num))
          {
            cur_fps = ctx->api_ctx.count_frame_num_in_sec;
            if ((ctx->api_ctx.frame_num != 0) && (pre_fps != cur_fps) &&
                ((ctx->api_ctx.frame_num < ctx->api_param.cfg_enc_params.frame_rate) ||
                 (ctx->api_ctx.frame_num - ctx->api_ctx.last_change_framenum >= ctx->api_param.cfg_enc_params.frame_rate)))
            {
              aux_data = ni_frame_new_aux_data(&dec_frame, NI_FRAME_AUX_DATA_FRAMERATE, sizeof(ni_framerate_t));
              if (aux_data)
              {
                ni_framerate_t *framerate = (ni_framerate_t *)aux_data->data;
                framerate->framerate_num = cur_fps;
                framerate->framerate_denom = 1;
              }

              ctx->api_ctx.last_change_framenum = ctx->api_ctx.frame_num;
              ctx->api_ctx.prev_fps = cur_fps;
            }
            ctx->api_ctx.count_frame_num_in_sec = 0;
            ctx->api_ctx.passed_time_in_timebase_unit = 0;
          }
          ctx->api_ctx.prev_pts = ctx->buffered_fme.pts;
        }
        else if (ctx->buffered_fme.pts < ctx->api_ctx.prev_pts)
        {
          //error handle for the case that pts jump back
          //this may cause a little error in the bitrate setting, This little error is acceptable.
          //As long as the subsequent, PTS is normal, it will be repaired quickly.
          ctx->api_ctx.prev_pts = ctx->buffered_fme.pts;
        }
        else
        {
          //do nothing, when the pts of two adjacent frames are the same
          //this may cause a little error in the bitrate setting, This little error is acceptable.
          //As long as the subsequent, PTS is normal, it will be repaired quickly.
        }
    }

    // force pic qp demo mode: initial QP (200 frames) -> QP value specified by
    // ForcePicQpDemoMode (100 frames) -> initial QP (remaining frames)
    if (p_param->force_pic_qp_demo_mode)
    {
      if (ctx->api_ctx.frame_num >= 300)
      {
          ctx->api_fme.data.frame.force_pic_qp =
              p_param->cfg_enc_params.rc.intra_qp;
      }
      else if (ctx->api_ctx.frame_num >= 200)
      {
        ctx->api_fme.data.frame.force_pic_qp = p_param->force_pic_qp_demo_mode;
      }
    }

    // supply QP map if ROI enabled and if ROIs passed in
    // Note: ROI demo mode takes higher priority over side data !
    side_data = av_frame_get_side_data(&ctx->buffered_fme, AV_FRAME_DATA_REGIONS_OF_INTEREST);

    if (!p_param->roi_demo_mode && p_param->cfg_enc_params.roi_enable &&
        side_data) {
        aux_data = ni_frame_new_aux_data(
            &dec_frame, NI_FRAME_AUX_DATA_REGIONS_OF_INTEREST, side_data->size);
        if (aux_data) {
            memcpy(aux_data->data, side_data->data, side_data->size);
        }
    }

    // Note: when ROI demo modes enabled, supply ROI map for the specified range
    //       frames, and 0 map for others
    if (QUADRA && p_param->roi_demo_mode &&
        p_param->cfg_enc_params.roi_enable) {
        if (ctx->api_ctx.frame_num > 90 && ctx->api_ctx.frame_num < 300) {
            ctx->api_fme.data.frame.roi_len = ctx->api_ctx.roi_len;
        } else {
            ctx->api_fme.data.frame.roi_len = 0;
        }
        // when ROI enabled, always have a data buffer for ROI
        // Note: this is handled separately from ROI through side/aux data
        ctx->api_fme.data.frame.extra_data_len += ctx->api_ctx.roi_len;
    }

    // SEI (HDR)
    // content light level info
    if (!(p_param->cfg_enc_params.HDR10CLLEnable)) // not user set
    {
        side_data = av_frame_get_side_data(&ctx->buffered_fme, AV_FRAME_DATA_CONTENT_LIGHT_LEVEL);

        if (side_data && side_data->size == sizeof(AVContentLightMetadata)) {
            aux_data = ni_frame_new_aux_data(
                &dec_frame, NI_FRAME_AUX_DATA_CONTENT_LIGHT_LEVEL,
                sizeof(ni_content_light_level_t));
            if (aux_data) {
                memcpy(aux_data->data, side_data->data, side_data->size);
            }
        }
    } else if ((AV_CODEC_ID_H264 == avctx->codec_id ||
                ctx->api_ctx.bit_depth_factor == 1) &&
               ctx->api_ctx.light_level_data_len == 0)
    // User input maxCLL so create SEIs for h264 and don't touch for (h265 &&
    // hdr10) since that is conveyed in config step
    ////Quadra autoset only for hdr10 format with hevc
    {
        aux_data = ni_frame_new_aux_data(&dec_frame,
                                         NI_FRAME_AUX_DATA_CONTENT_LIGHT_LEVEL,
                                         sizeof(ni_content_light_level_t));
        if (aux_data) {
            ni_content_light_level_t *cll =
                (ni_content_light_level_t *)(aux_data->data);
            cll->max_cll  = p_param->cfg_enc_params.HDR10MaxLight;
            cll->max_fall = p_param->cfg_enc_params.HDR10AveLight;
        }
    }

    // mastering display color volume
    if (!(p_param->cfg_enc_params.HDR10Enable)) // not user set
    {
        side_data = av_frame_get_side_data(&ctx->buffered_fme, AV_FRAME_DATA_MASTERING_DISPLAY_METADATA);
        if (side_data && side_data->size == sizeof(AVMasteringDisplayMetadata))
        {
            aux_data = ni_frame_new_aux_data(
                &dec_frame, NI_FRAME_AUX_DATA_MASTERING_DISPLAY_METADATA,
                sizeof(ni_mastering_display_metadata_t));
            if (aux_data) {
                memcpy(aux_data->data, side_data->data, side_data->size);
            }
        }
    }
    else if ((AV_CODEC_ID_H264 == avctx->codec_id ||
        ctx->api_ctx.bit_depth_factor == 1) &&
        ctx->api_ctx.sei_hdr_mastering_display_color_vol_len == 0)
        // User input masterDisplay so create SEIs for h264 and don't touch for (h265 &&
        // hdr10) since that is conveyed in config step
        ////Quadra autoset only for hdr10 format with hevc
    {
        aux_data = ni_frame_new_aux_data(&dec_frame,
            NI_FRAME_AUX_DATA_MASTERING_DISPLAY_METADATA,
            sizeof(ni_mastering_display_metadata_t));
        if (aux_data) {
            ni_mastering_display_metadata_t *mst_dsp =
                (ni_mastering_display_metadata_t *)(aux_data->data);

            //X, Y display primaries for RGB channels and white point(WP) in units of 0.00002
            //and max, min luminance(L) values in units of 0.0001 nits
            //xy are denom = 50000 num = HDR10dx0/y
            mst_dsp->display_primaries[0][0].den = MASTERING_DISP_CHROMA_DEN;
            mst_dsp->display_primaries[0][1].den = MASTERING_DISP_CHROMA_DEN;
            mst_dsp->display_primaries[1][0].den = MASTERING_DISP_CHROMA_DEN;
            mst_dsp->display_primaries[1][1].den = MASTERING_DISP_CHROMA_DEN;
            mst_dsp->display_primaries[2][0].den = MASTERING_DISP_CHROMA_DEN;
            mst_dsp->display_primaries[2][1].den = MASTERING_DISP_CHROMA_DEN;
            mst_dsp->white_point[0].den = MASTERING_DISP_CHROMA_DEN;
            mst_dsp->white_point[1].den = MASTERING_DISP_CHROMA_DEN;
            mst_dsp->min_luminance.den = MASTERING_DISP_LUMA_DEN;
            mst_dsp->max_luminance.den = MASTERING_DISP_LUMA_DEN;
            // ni_mastering_display_metadata_t has to be filled with R,G,B
            // values, in that order, while HDR10d is filled in order of G,B,R,
            // so do the conversion here.
            mst_dsp->display_primaries[0][0].num = p_param->cfg_enc_params.HDR10dx2;
            mst_dsp->display_primaries[0][1].num = p_param->cfg_enc_params.HDR10dy2;
            mst_dsp->display_primaries[1][0].num = p_param->cfg_enc_params.HDR10dx0;
            mst_dsp->display_primaries[1][1].num = p_param->cfg_enc_params.HDR10dy0;
            mst_dsp->display_primaries[2][0].num = p_param->cfg_enc_params.HDR10dx1;
            mst_dsp->display_primaries[2][1].num = p_param->cfg_enc_params.HDR10dy1;
            mst_dsp->white_point[0].num = p_param->cfg_enc_params.HDR10wx;
            mst_dsp->white_point[1].num = p_param->cfg_enc_params.HDR10wy;
            mst_dsp->min_luminance.num = p_param->cfg_enc_params.HDR10minluma;
            mst_dsp->max_luminance.num = p_param->cfg_enc_params.HDR10maxluma;
            mst_dsp->has_primaries = 1;
            mst_dsp->has_luminance = 1;
        }
    }
    // SEI (HDR10+)
    side_data = av_frame_get_side_data(&ctx->buffered_fme, AV_FRAME_DATA_DYNAMIC_HDR_PLUS);
    if (side_data && side_data->size == sizeof(AVDynamicHDRPlus))
    {
        aux_data = ni_frame_new_aux_data(&dec_frame, NI_FRAME_AUX_DATA_HDR_PLUS,
                                         sizeof(ni_dynamic_hdr_plus_t));
        if (aux_data) {
            memcpy(aux_data->data, side_data->data, side_data->size);
        }
    } // hdr10+

    // SEI (close caption)
    side_data = av_frame_get_side_data(&ctx->buffered_fme, AV_FRAME_DATA_A53_CC);

    if (side_data && side_data->size > 0)
    {
        aux_data = ni_frame_new_aux_data(&dec_frame, NI_FRAME_AUX_DATA_A53_CC,
                                         side_data->size);
        if (aux_data) {
            memcpy(aux_data->data, side_data->data, side_data->size);
        }
    }

    // User data unregistered SEI
    side_data =
        av_frame_get_side_data(&ctx->buffered_fme, AV_FRAME_DATA_NETINT_UDU_SEI);
    if (side_data && side_data->size > 0) {
        aux_data = ni_frame_new_aux_data(&dec_frame, NI_FRAME_AUX_DATA_UDU_SEI,
                                         side_data->size);
        if (aux_data) {
            memcpy(aux_data->data, (uint8_t *)side_data->data, side_data->size);
        }
    }

    if (ctx->api_ctx.force_frame_type) {
        switch (ctx->buffered_fme.pict_type) {
        case AV_PICTURE_TYPE_I:
            ctx->api_fme.data.frame.ni_pict_type = PIC_TYPE_IDR;
            break;
        case AV_PICTURE_TYPE_P:
            ctx->api_fme.data.frame.ni_pict_type = PIC_TYPE_P;
            break;
        default:
            ;
        }
    }
    else if (ctx->buffered_fme.pict_type == AV_PICTURE_TYPE_I)
    {
      ctx->api_fme.data.frame.force_key_frame = 1;
      ctx->api_fme.data.frame.ni_pict_type = PIC_TYPE_IDR;
    }

    av_log(avctx, AV_LOG_TRACE,
           "xcoder_send_frame: #%" PRIu64 " ni_pict_type %d"
           " forced_header_enable %d intraPeriod %d\n",
           ctx->api_ctx.frame_num, ctx->api_fme.data.frame.ni_pict_type,
           p_param->cfg_enc_params.forced_header_enable,
           p_param->cfg_enc_params.intra_period);

    // whether should send SEI with this frame
    send_sei_with_idr = ni_should_send_sei_with_frame(
        &ctx->api_ctx, ctx->api_fme.data.frame.ni_pict_type, p_param);

    // prep for auxiliary data (various SEI, ROI) in encode frame, based on the
    // data returned in decoded frame
    ni_enc_prep_aux_data(&ctx->api_ctx, &ctx->api_fme.data.frame, &dec_frame,
                         ctx->api_ctx.codec_format, send_sei_with_idr,
                         mdcv_data, cll_data, cc_data, udu_data, hdrp_data);

    side_data = av_frame_get_side_data(&ctx->buffered_fme,
                                       AV_FRAME_DATA_NETINT_CUSTOM_SEI);
    if (side_data && side_data->size > 0)
    {
      int64_t local_pts = ctx->buffered_fme.pts;
      uint8_t *p_src_sei_data, *p_dst_sei_data;
      int sei_size;
      uint8_t sei_type;
      int size;
      ni_custom_sei_set_t *src_custom_sei_set, *dst_custom_sei_set;
      ni_custom_sei_t *p_src_custom_sei, *p_dst_custom_sei;

      // if one picture can be skipped, nienc will send that frame but will not
      // receive packet, therefore it will skip the free in receive packet as
      // well and cause memory leak. So check the last pkt_custom_sei_set has
      // been released or not.
      dst_custom_sei_set = ctx->api_ctx.pkt_custom_sei_set[local_pts % NI_FIFO_SZ];
      if (dst_custom_sei_set)
      {
          free(dst_custom_sei_set);
      }

      /* copy the whole SEI data */
      src_custom_sei_set = (ni_custom_sei_set_t *)side_data->data;
      dst_custom_sei_set = malloc(sizeof(ni_custom_sei_set_t));
      if (dst_custom_sei_set == NULL)
      {
          av_log(avctx, AV_LOG_ERROR, "failed to allocate memory for custom sei data\n");
          ret = AVERROR(ENOMEM);
          return ret;
      }
      memset(dst_custom_sei_set, 0, sizeof(ni_custom_sei_set_t));

      /* fill sei data */
      for (i = 0; i < src_custom_sei_set->count; i++)
      {
          int len;
          p_src_custom_sei = &src_custom_sei_set->custom_sei[i];
          sei_size = p_src_custom_sei->size;
          sei_type = p_src_custom_sei->type;
          p_src_sei_data = &p_src_custom_sei->data[0];

          p_dst_custom_sei = &dst_custom_sei_set->custom_sei[i];
          p_dst_sei_data = &p_dst_custom_sei->data[0];
          size = 0;

          // long start code
          p_dst_sei_data[size++] = 0x00;
          p_dst_sei_data[size++] = 0x00;
          p_dst_sei_data[size++] = 0x00;
          p_dst_sei_data[size++] = 0x01;

          if (AV_CODEC_ID_H264 == avctx->codec_id)
          {
            p_dst_sei_data[size++] = 0x06;   //nal type: SEI
          }
          else
          {
            p_dst_sei_data[size++] = 0x4e;   //nal type: SEI
            p_dst_sei_data[size++] = 0x01;
          }

          // SEI type
          p_dst_sei_data[size++] = sei_type;

          // original payload size
          len = sei_size;
          while (len >= 0)
          {
            p_dst_sei_data[size++] = len > 0xff ? 0xff : len;
            len -= 0xff;
          }

          // payload data
          for (j = 0; j < sei_size && size < NI_MAX_SEI_DATA - 1; j++)
          {
              if (j >= 2 && !p_dst_sei_data[size - 2] && !p_dst_sei_data[size - 1] && p_src_sei_data[j] <= 0x03)
              {
                  /* insert 0x3 as emulation_prevention_three_byte */
                  p_dst_sei_data[size++] = 0x03;
              }
              p_dst_sei_data[size++] = p_src_sei_data[j];
          }

          if (j != sei_size)
          {
              av_log(avctx, AV_LOG_WARNING, "%s: sei RBSP size out of limit(%d), "
                     "idx=%u, type=%u, size=%d, custom_sei_loc=%d.\n", __func__,
                     NI_MAX_SEI_DATA, i, sei_type, sei_size, p_src_custom_sei->location);
              free(dst_custom_sei_set);
              break;
          }

          // trailing byte
          p_dst_sei_data[size++] = 0x80;

          p_dst_custom_sei->size = size;
          p_dst_custom_sei->type = sei_type;
          p_dst_custom_sei->location = p_src_custom_sei->location;
          av_log(avctx, AV_LOG_TRACE, "%s: custom sei idx %d type %u len %d loc %d.\n",
                 __func__, i, sei_type, size, p_dst_custom_sei->location);
      }

      dst_custom_sei_set->count = src_custom_sei_set->count;
      ctx->api_ctx.pkt_custom_sei_set[local_pts % NI_FIFO_SZ] = dst_custom_sei_set;
      av_log(avctx, AV_LOG_TRACE, "%s: sei number %d pts %" PRId64 ".\n",
             __func__, dst_custom_sei_set->count, local_pts);
    }

    if (ctx->api_fme.data.frame.sei_total_len > NI_ENC_MAX_SEI_BUF_SIZE)
    {
      av_log(avctx, AV_LOG_ERROR, "xcoder_send_frame: sei total length %u exceeds maximum sei size %u.\n",
             ctx->api_fme.data.frame.sei_total_len, NI_ENC_MAX_SEI_BUF_SIZE);
      ret = AVERROR(EINVAL);
      return ret;
    }

    ctx->api_fme.data.frame.extra_data_len += ctx->api_fme.data.frame.sei_total_len;

    // data layout requirement: leave space for reconfig data if at least one of
    // reconfig, SEI or ROI is present
    // Note: ROI is present when enabled, so use encode config flag instead of
    //       frame's roi_len as it can be 0 indicating a 0'd ROI map setting !
    if (ctx->api_fme.data.frame.reconf_len ||
        ctx->api_fme.data.frame.sei_total_len ||
        p_param->cfg_enc_params.roi_enable) {
        ctx->api_fme.data.frame.extra_data_len +=
            sizeof(ni_encoder_change_params_t);
    }

    ctx->api_fme.data.frame.pts = ctx->buffered_fme.pts;
    ctx->api_fme.data.frame.dts = ctx->buffered_fme.pkt_dts;

    ctx->api_fme.data.frame.video_width = avctx->width;
    ctx->api_fme.data.frame.video_height = avctx->height;

    ishwframe = ctx->buffered_fme.format == AV_PIX_FMT_NI_QUAD;
    if (ctx->api_ctx.auto_dl_handle != 0 || (avctx->height < NI_MIN_HEIGHT) ||
        (avctx->width < NI_MIN_WIDTH)) {
        format_in_use          = avctx->sw_pix_fmt;
        ctx->api_ctx.hw_action = 0;
        ishwframe              = 0;
    }
    isnv12frame = (format_in_use == AV_PIX_FMT_NV12 || format_in_use == AV_PIX_FMT_P010LE);

    if (ishwframe)
    {
      ret = sizeof(niFrameSurface1_t);
    }
    else
    {
      ret = av_image_get_buffer_size(format_in_use,
                                     ctx->buffered_fme.width, ctx->buffered_fme.height, 1);
    }

#if FF_API_PKT_PTS
    // NOLINTNEXTLINE(clang-diagnostic-deprecated-declarations)
    av_log(avctx, AV_LOG_TRACE, "xcoder_send_frame: pts=%" PRId64 ", pkt_dts=%" PRId64 ", pkt_pts=%" PRId64 "\n", ctx->buffered_fme.pts, ctx->buffered_fme.pkt_dts, ctx->buffered_fme.pkt_pts);
#endif
    av_log(avctx, AV_LOG_TRACE, "xcoder_send_frame: frame->format=%d, frame->width=%d, frame->height=%d, frame->pict_type=%d, size=%d\n", format_in_use, ctx->buffered_fme.width, ctx->buffered_fme.height, ctx->buffered_fme.pict_type, ret);
    if (ret < 0)
    {
      return ret;
    }

    int dst_stride[NI_MAX_NUM_DATA_POINTERS]     = {0};
    int height_aligned[NI_MAX_NUM_DATA_POINTERS] = {0};
    int src_height[NI_MAX_NUM_DATA_POINTERS]     = {0};

    if (QUADRA)
    {
        src_height[0] = ctx->buffered_fme.height;
        src_height[1] = ctx->buffered_fme.height / 2;
        src_height[2] = (isnv12frame) ? 0 : (ctx->buffered_fme.height / 2);

        ni_get_hw_yuv420p_dim(ctx->buffered_fme.width,
                              ctx->buffered_fme.height,
                              ctx->api_ctx.bit_depth_factor, isnv12frame,
                              dst_stride, height_aligned);

        av_log(avctx, AV_LOG_TRACE,
               "xcoder_send_frame frame->width %d "
               "ctx->api_ctx.bit_depth_factor %d dst_stride[0/1/2] %d/%d/%d\n",
               ctx->buffered_fme.width, ctx->api_ctx.bit_depth_factor,
               dst_stride[0], dst_stride[1], dst_stride[2]);

        if (alignment_2pass_wa && !ishwframe) {
            if (isnv12frame) {
                // for 2-pass encode output mismatch WA, need to extend (and
                // pad) CbCr plane height, because 1st pass assume input 32
                // align
                height_aligned[1] = FFALIGN(height_aligned[0], 32) / 2;
            } else {
                // for 2-pass encode output mismatch WA, need to extend (and
                // pad) Cr plane height, because 1st pass assume input 32 align
                height_aligned[2] = FFALIGN(height_aligned[0], 32) / 2;
            }
        }
    } else {
        dst_stride[0] = ((frame->width + 31) / 32) * 32;
        if (dst_stride[0] < NI_MIN_WIDTH) {
            dst_stride[0] = NI_MIN_WIDTH;
        }
        dst_stride[0] *= ctx->api_ctx.bit_depth_factor;
        dst_stride[1] = dst_stride[2] = dst_stride[0] / 2;

        height_aligned[0] = ((frame->height + 7) / 8) * 8;
        if (avctx->codec_id == AV_CODEC_ID_H264) {
            height_aligned[0] = ((frame->height + 15) / 16) * 16;
        }
        if (height_aligned[0] < NI_MIN_HEIGHT) {
            height_aligned[0] = NI_MIN_HEIGHT;
        }
        height_aligned[1] = height_aligned[2] = height_aligned[0] / 2;
    }

    // alignment(16) extra padding for H.264 encoding
    if (ishwframe) {
        uint8_t *dsthw;
        const uint8_t *srchw;

        ni_frame_buffer_alloc_hwenc(
            &(ctx->api_fme.data.frame), ctx->buffered_fme.width,
            ctx->buffered_fme.height,
            (int)ctx->api_fme.data.frame.extra_data_len);
        if (!ctx->api_fme.data.frame.p_data[3]) {
            return AVERROR(ENOMEM);
        }
        dsthw       = ctx->api_fme.data.frame.p_data[3];
        srchw = (const uint8_t *)ctx->buffered_fme.data[3];
        av_log(avctx, AV_LOG_TRACE, "dst=%p src=%p len=%d\n", dsthw, srchw,
               ctx->api_fme.data.frame.data_len[3]);
        memcpy(dsthw, srchw, ctx->api_fme.data.frame.data_len[3]);
        av_log(avctx, AV_LOG_TRACE,
               "ctx->buffered_fme.data[3] %p memcpy to %p\n",
               ctx->buffered_fme.data[3], dsthw);
    } else // traditional yuv transfer
    {
       /* if (isnv12frame) {
            ni_frame_buffer_alloc_nv(
                &(ctx->api_fme.data.frame), ctx->buffered_fme.width,
                height_aligned[0], dst_stride,
                (int)ctx->api_fme.data.frame.extra_data_len,
                alignment_2pass_wa);
        } else {
            ni_encoder_frame_buffer_alloc(
                &(ctx->api_fme.data.frame), ctx->buffered_fme.width,
                height_aligned[0], dst_stride,
                (avctx->codec_id == AV_CODEC_ID_H264),
                (int)ctx->api_fme.data.frame.extra_data_len,
                alignment_2pass_wa);
        }*/

        av_log(avctx, AV_LOG_TRACE, "[0] %p stride[0] %u height %u data[1] %p data[3] %p\n",
            ctx->buffered_fme.data[0], dst_stride[0], ctx->buffered_fme.height, ctx->buffered_fme.data[1],
            ctx->buffered_fme.data[3]);
        if (ctx->buffered_fme.data[3] > 0 &&
            ((uintptr_t)ctx->buffered_fme.data[0] % NI_MEM_PAGE_ALIGNMENT) == 0 &&
            dst_stride[0] == ctx->buffered_fme.linesize[0] && //stride aligned
            dst_stride[1] == ctx->buffered_fme.linesize[1] && //stride aligned
            ctx->buffered_fme.height % 2 == 0 && //even height
            ctx->buffered_fme.data[0] + dst_stride[0] * ctx->buffered_fme.height == ctx->buffered_fme.data[1] &&
            (isnv12frame || //contiguous?
            ctx->buffered_fme.data[1] + dst_stride[1] * ctx->buffered_fme.height / 2 == ctx->buffered_fme.data[2]))
        {
            need_to_copy = 0;
            ni_encoder_sw_frame_buffer_alloc(
                !isnv12frame, &(ctx->api_fme.data.frame), ctx->buffered_fme.width,
                height_aligned[0], dst_stride, (avctx->codec_id == AV_CODEC_ID_H264),
                -1, alignment_2pass_wa); //no allocation required
            ctx->api_fme.data.frame.p_buffer = ctx->buffered_fme.data[0];
            ctx->api_fme.data.frame.p_data[0] = ctx->buffered_fme.data[0];
            ctx->api_fme.data.frame.p_data[1] = ctx->buffered_fme.data[1];
            if (!isnv12frame)
                ctx->api_fme.data.frame.p_data[2] = ctx->buffered_fme.data[2];
        }
        else
        {
            ni_encoder_sw_frame_buffer_alloc(
                !isnv12frame, &(ctx->api_fme.data.frame), ctx->buffered_fme.width,
                height_aligned[0], dst_stride, (avctx->codec_id == AV_CODEC_ID_H264),
                (int)ctx->api_fme.data.frame.extra_data_len, alignment_2pass_wa);
        }
        av_log(avctx, AV_LOG_TRACE, "%p need_to_copy %d! pts = %ld\n", ctx->api_fme.data.frame.p_buffer, need_to_copy, ctx->buffered_fme.pts);
       if (!ctx->api_fme.data.frame.p_data[0]) {
           return AVERROR(ENOMEM);
      }

      // if this is indeed sw frame, do the YUV data layout, otherwise may need
      // to do frame download
      if (ctx->buffered_fme.format != AV_PIX_FMT_NI_QUAD) {
          av_log(
              avctx, AV_LOG_TRACE,
              "xcoder_send_frame: fme.data_len[0]=%d, "
              "buf_fme->linesize=%d/%d/%d, dst alloc linesize = %d/%d/%d, "
              "src height = %d/%d/%d, dst height aligned = %d/%d/%d, "
              "force_key_frame=%d, extra_data_len=%d sei_size=%d "
              "(hdr_content_light_level %u hdr_mastering_display_color_vol %u "
              "hdr10+ %u cc %u udu %u prefC %u) roi_size=%u reconf_size=%u "
              "force_pic_qp=%u "
              "use_cur_src_as_long_term_pic %u use_long_term_ref %u\n",
              ctx->api_fme.data.frame.data_len[0],
              ctx->buffered_fme.linesize[0], ctx->buffered_fme.linesize[1],
              ctx->buffered_fme.linesize[2], dst_stride[0], dst_stride[1],
              dst_stride[2], src_height[0], src_height[1], src_height[2],
              height_aligned[0], height_aligned[1], height_aligned[2],
              ctx->api_fme.data.frame.force_key_frame,
              ctx->api_fme.data.frame.extra_data_len,
              ctx->api_fme.data.frame.sei_total_len,
              ctx->api_fme.data.frame.sei_hdr_content_light_level_info_len,
              ctx->api_fme.data.frame.sei_hdr_mastering_display_color_vol_len,
              ctx->api_fme.data.frame.sei_hdr_plus_len,
              ctx->api_fme.data.frame.sei_cc_len,
              ctx->api_fme.data.frame.sei_user_data_unreg_len,
              ctx->api_fme.data.frame.preferred_characteristics_data_len,
              (p_param->cfg_enc_params.roi_enable ? ctx->api_ctx.roi_len : 0),
              ctx->api_fme.data.frame.reconf_len,
              ctx->api_fme.data.frame.force_pic_qp,
              ctx->api_fme.data.frame.use_cur_src_as_long_term_pic,
              ctx->api_fme.data.frame.use_long_term_ref);

          // YUV part of the encoder input data layout
          if (need_to_copy)
          {
              ni_copy_hw_yuv420p(
                  (uint8_t **)(ctx->api_fme.data.frame.p_data),
                  ctx->buffered_fme.data, ctx->buffered_fme.width,
                  ctx->buffered_fme.height, ctx->api_ctx.bit_depth_factor,
                  isnv12frame, p_param->cfg_enc_params.conf_win_right, dst_stride,
                  height_aligned, ctx->buffered_fme.linesize, src_height);
          }
      } else {
          ni_session_data_io_t *p_session_data;
          ni_session_data_io_t niframe;
          niFrameSurface1_t *src_surf;

          av_log(avctx, AV_LOG_DEBUG,
                 "xcoder_send_frame:Autodownload to be run: hdl: %d w: %d h: %d\n",
                 ctx->api_ctx.auto_dl_handle, avctx->width, avctx->height);
          avhwf_ctx =
              (AVHWFramesContext *)ctx->buffered_fme.hw_frames_ctx->data;
          nif_src_ctx = avhwf_ctx->internal->priv;

          src_surf = (niFrameSurface1_t *)ctx->buffered_fme.data[3];

          if (avctx->height < NI_MIN_HEIGHT || avctx->width < NI_MIN_WIDTH) {
              int bit_depth;
              int is_planar;

              p_session_data = &niframe;
              memset(&niframe, 0, sizeof(niframe));
              bit_depth = ((avctx->sw_pix_fmt == AV_PIX_FMT_YUV420P10LE) ||
                           (avctx->sw_pix_fmt == AV_PIX_FMT_P010LE))
                              ? 2
                              : 1;
              is_planar = (avctx->sw_pix_fmt == AV_PIX_FMT_YUV420P) ||
                          (avctx->sw_pix_fmt == AV_PIX_FMT_YUV420P10LE);

              /* Allocate a minimal frame */
              ni_frame_buffer_alloc(&niframe.data.frame, avctx->width,
                                    avctx->height, 0, /* alignment */
                                    1,                /* metadata */
                                    bit_depth, 0,     /* hw_frame_count */
                                    is_planar);
          } else {
              p_session_data = &(ctx->api_fme);
          }

          nif_src_ctx->api_ctx.is_auto_dl = true;
          ret = ni_device_session_hwdl(&nif_src_ctx->api_ctx, p_session_data,
                                       src_surf);
          ishwframe = false;
          if (ret <= 0) {
              av_log(avctx, AV_LOG_ERROR,
                     "nienc.c:ni_hwdl_frame() failed to retrieve frame\n");
              return AVERROR_EXTERNAL;
          }

          if ((avctx->height < NI_MIN_HEIGHT) ||
              (avctx->width < NI_MIN_WIDTH)) {
              expand_ni_frame(avctx, &ctx->api_fme.data.frame,
                              &p_session_data->data.frame, dst_stride,
                              avctx->width, avctx->height, avctx->sw_pix_fmt);

              ni_frame_buffer_free(&niframe.data.frame);
          }
      }
    } // end if hwframe else

    // auxiliary data part of the encoder input data layout
    ni_enc_copy_aux_data(&ctx->api_ctx, &ctx->api_fme.data.frame, &dec_frame,
                         ctx->api_ctx.codec_format, mdcv_data, cll_data,
                         cc_data, udu_data, hdrp_data, ishwframe, isnv12frame);

    ni_frame_buffer_free(&dec_frame);

    // end of encode input frame data layout

    } // end non seq change
#ifdef MULTI_THREAD
  if (ctx->encoder_flushing)
  {
    sent = ni_device_session_write(&ctx->api_ctx, &ctx->api_fme, NI_DEVICE_TYPE_ENCODER);

    av_log(avctx, AV_LOG_DEBUG, "xcoder_send_frame encoder_flushing: size %d sent to xcoder\n", sent);

    if (NI_RETCODE_ERROR_RESOURCE_UNAVAILABLE == sent)
    {
      av_log(avctx, AV_LOG_DEBUG, "xcoder_send_frame(): Sequence Change in progress, returning EAGAIN\n");
      ret = AVERROR(EAGAIN);
      return ret;
    }

    if (sent == -1)
    {
      ret = AVERROR(EAGAIN);
    }
    else
    {
      if (frame && ishwframe)
      {
          av_log(avctx, AV_LOG_TRACE, "AVframe_index = %d at head %d\n",
                 ctx->aFree_Avframes_list[ctx->freeHead], ctx->freeHead);
          av_frame_ref(ctx->sframe_pool[ctx->aFree_Avframes_list[ctx->freeHead]], frame);
          av_log(avctx, AV_LOG_TRACE,
                 "AVframe_index = %d popped from head %d\n",
                 ctx->aFree_Avframes_list[ctx->freeHead], ctx->freeHead);
          if (deq_free_frames(ctx) != 0) {
              ret = AVERROR_EXTERNAL;
              return ret;
          }
        //av_frame_ref(ctx->sframe_pool[((niFrameSurface1_t*)((uint8_t*)frame->data[3]))->ui16FrameIdx], frame);
      }
      // pushing input pts in circular FIFO
      ctx->api_ctx.enc_pts_list[ctx->api_ctx.enc_pts_w_idx % NI_FIFO_SZ] = ctx->api_fme.data.frame.pts;
      ctx->api_ctx.enc_pts_w_idx ++;

      ret = 0;
    }
  }
  //else if (ctx->buffered_fme && ishwframe)
  else if (!ctx->eos_fme_received && ishwframe)
  {
    sent = ni_device_session_write(&ctx->api_ctx, &ctx->api_fme, NI_DEVICE_TYPE_ENCODER);
    //ctx->sframe_pool[((niFrameSurface1_t*)((uint8_t*)frame->data[3]))->ui16FrameIdx] = av_buffer_ref(frame);
    av_log(avctx, AV_LOG_DEBUG, "xcoder_send_frame: size %d sent to xcoder\n", sent);

    if (NI_RETCODE_ERROR_RESOURCE_UNAVAILABLE == sent)
    {
      av_log(avctx, AV_LOG_DEBUG, "xcoder_send_frame(): Sequence Change in progress, returning EAGAIN\n");
      ret = AVERROR(EAGAIN);
      return ret;
    }

    if (sent == -1)
    {
      ret = AVERROR(EAGAIN);
    }
    else
    {
        av_log(avctx, AV_LOG_TRACE, "AVframe_index = %d at head %d\n",
               ctx->aFree_Avframes_list[ctx->freeHead], ctx->freeHead);
        av_frame_ref(ctx->sframe_pool[ctx->aFree_Avframes_list[ctx->freeHead]],
                     ctx->buffered_fme);
        av_log(avctx, AV_LOG_DEBUG, "AVframe_index = %d popped from head %d\n",
               ctx->aFree_Avframes_list[ctx->freeHead], ctx->freeHead);
        if (deq_free_frames(ctx) != 0) {
            ret = AVERROR_EXTERNAL;
            return ret;
        }
      //av_frame_ref(ctx->sframe_pool[((niFrameSurface1_t*)((uint8_t*)frame->data[3]))->ui16FrameIdx], frame);
      ret = 0;
    }
  }
  else
  {
    uint8_t id = find_session_idx(NI_INVALID_SESSION_ID);
    if (id < NI_MAX_NUM_SESSIONS)
    {
      av_log(avctx, AV_LOG_DEBUG, "xcoder_send_frame current: find_session_idx %d\n", id);
      av_log(avctx, AV_LOG_DEBUG, "xcoder_send_frame current: ctx->api_ctx.session_id %d\n", ctx->api_ctx.session_id);
      write_thread_args[id].session_id = ctx->api_ctx.session_id;
      write_thread_args[id].ctx = ctx;
      av_log(avctx, AV_LOG_DEBUG, "xcoder_send_frame: session_id %d\n", write_thread_args[id].session_id);
      av_log(avctx, AV_LOG_DEBUG, "xcoder_send_frame: ctx %p\n", write_thread_args[id].ctx);
      //saved the current session id
      if ((ret = pthread_create(&write_thread_args[id].thread, NULL, write_frame_thread, (void *)&write_thread_args[id])))
      {
        av_log(avctx, AV_LOG_ERROR, "pthread_create failed: %s. Try to increase `ulimit -v` or decrease `ulimit -s`.\n", strerror(ret));
        return AVERROR(ret);
      }
    }
    else
    {
      av_log(avctx, AV_LOG_ERROR,"no more session map for this id %d\n",ctx->api_ctx.session_id);
      return AVERROR_EXTERNAL;
    }
  }

#else
  sent = ni_device_session_write(&ctx->api_ctx, &ctx->api_fme, NI_DEVICE_TYPE_ENCODER);
  av_log(avctx, AV_LOG_DEBUG, "xcoder_send_frame: size %d sent to xcoder\n", sent);

   // device session write does not return resource unavail, may add later when support strict timeout
  /*
  if (NI_RETCODE_ERROR_RESOURCE_UNAVAILABLE == sent)
  {
    //av_log(avctx, AV_LOG_ERROR, "xcoder_send_frame(): failure sent (%d) , "
    //       "returning EIO\n", sent);
    //ret = AVERROR(EIO);

    // if rejected due to sequence change in progress, revert resolution
    // setting and will do it again next time.
    if (ctx->api_fme.data.frame.start_of_stream &&
        (avctx->width != orig_avctx_width ||
         avctx->height != orig_avctx_height))
    {
      avctx->width = orig_avctx_width;
      avctx->height = orig_avctx_height;
    }
    av_log(avctx, AV_LOG_DEBUG, "xcoder_send_frame(): Sequence Change in progress, returning EAGAIN\n");
    ret = AVERROR(EAGAIN);
    return ret;
  }
  */

  // return EIO at error
  if (NI_RETCODE_ERROR_VPU_RECOVERY == sent)
  {
    sent = xcoder_encode_reset(avctx);
    if (sent < 0)
    {
      av_log(avctx, AV_LOG_ERROR, "xcoder_send_frame(): VPU recovery failed:%d, returning EIO\n", sent);
      ret = AVERROR(EIO);
    }
  }
  else if (sent < 0)
  {
    av_log(avctx, AV_LOG_ERROR, "xcoder_send_frame(): failure sent (%d) , "
           "returning EIO\n", sent);
    ret = AVERROR(EIO);

    // if rejected due to sequence change in progress, revert resolution
    // setting and will do it again next time.
    // TODO: don't see how AvCodec width / height could have changed
    if (ctx->api_fme.data.frame.start_of_stream &&
        (avctx->width != orig_avctx_width ||
         avctx->height != orig_avctx_height))
    {
      avctx->width = orig_avctx_width;
      avctx->height = orig_avctx_height;
    }
    return ret;
  }
  else
  {
    /*
    if (sent < 0)
    {
      av_log(avctx, AV_LOG_ERROR, "xcoder_send_frame(): failure sent (%d) , "
             "returning EIO\n", sent);
      ret = AVERROR(EIO);

      // if rejected due to sequence change in progress, revert resolution
      // setting and will do it again next time.
      if (ctx->api_fme.data.frame.start_of_stream &&
          (avctx->width != orig_avctx_width ||
           avctx->height != orig_avctx_height))
      {
        avctx->width = orig_avctx_width;
        avctx->height = orig_avctx_height;
      }
      return ret;
    }
    else
    */
    av_log(avctx, AV_LOG_DEBUG, "xcoder_send_frame(): sent (%d)\n", sent);
    if (sent == 0)
    {
      // TODO: don't see how AvCodec width / height could have changed
      // case of sequence change in progress
      if (ctx->api_fme.data.frame.start_of_stream &&
          (avctx->width != orig_avctx_width ||
           avctx->height != orig_avctx_height))
      {
        avctx->width = orig_avctx_width;
        avctx->height = orig_avctx_height;
      }

      // when buffer_full, drop the frame and return EAGAIN if in strict timeout
      // mode, otherwise buffer the frame and it is to be sent out using encode2
      // API: queue the frame only if not done so yet, i.e. queue is empty
      // *and* it's a valid frame. ToWatch: what are other rc cases ?
      if (ctx->api_ctx.status == NI_RETCODE_NVME_SC_WRITE_BUFFER_FULL)
      {
        ishwframe = ctx->buffered_fme.format == AV_PIX_FMT_NI_QUAD;
        if (ishwframe)
        {
          // Do not queue frames to avoid FFmpeg stuck when multiple HW frames are queued up in nienc, causing decoder unable to acquire buffer, which led to FFmpeg stuck
          av_log(avctx, AV_LOG_ERROR, "xcoder_send_frame(): device WRITE_BUFFER_FULL cause frame drop! (approx. Frame num #%" PRIu64 "\n", ctx->api_ctx.frame_num);
          av_frame_unref(&ctx->buffered_fme);
          ret = 0;
        }
        else
        {
          // TODO: enable when strict timeout mode is added
          /*
          if (ctx->api_param.strict_timeout_mode)
          {
            av_log(avctx, AV_LOG_ERROR, "xcoder_send_frame(): Error Strict timeout period exceeded, returning EAGAIN\n");
            ret = AVERROR(EAGAIN);
          }
          else
          */
          {
            av_log(avctx, AV_LOG_DEBUG, "xcoder_send_frame(): Write buffer full, returning 1\n");
            ret = 1;

            if (frame && is_input_fifo_empty(ctx))
            {
              enqueue_frame(avctx, frame);
            }
          }
        }
      }
    }
    else
    {
      //if (ctx->buffered_fme && ishwframe)//may or may not break hwframe mode, to be tested
      ishwframe = (ctx->buffered_fme.format == AV_PIX_FMT_NI_QUAD) &&
                  (ctx->api_ctx.auto_dl_handle == 0) &&
                  (avctx->height >= NI_MIN_HEIGHT) &&
                  (avctx->width >= NI_MIN_WIDTH);

      if (!ctx->eos_fme_received && ishwframe)
      {
          av_log(avctx, AV_LOG_TRACE, "AVframe_index = %d at head %d\n",
                 ctx->aFree_Avframes_list[ctx->freeHead], ctx->freeHead);
          av_frame_ref(
              ctx->sframe_pool[ctx->aFree_Avframes_list[ctx->freeHead]],
              &ctx->buffered_fme);
          av_log(avctx, AV_LOG_TRACE,
                 "AVframe_index = %d popped from free head %d\n",
                 ctx->aFree_Avframes_list[ctx->freeHead], ctx->freeHead);
          av_log(avctx, AV_LOG_TRACE,
                 "ctx->buffered_fme.data[3] %p sframe_pool[%d]->data[3] %p\n",
                 ctx->buffered_fme.data[3],
                 ctx->aFree_Avframes_list[ctx->freeHead],
                 ctx->sframe_pool[ctx->aFree_Avframes_list[ctx->freeHead]]
                     ->data[3]);
          if (ctx->sframe_pool[ctx->aFree_Avframes_list[ctx->freeHead]]
                  ->data[3]) {
              av_log(avctx, AV_LOG_DEBUG,
                     "nienc.c sframe_pool[%d] trace ui16FrameIdx = [%u] sent\n",
                     ctx->aFree_Avframes_list[ctx->freeHead],
                     ((niFrameSurface1_t
                           *)((uint8_t *)ctx
                                  ->sframe_pool
                                      [ctx->aFree_Avframes_list[ctx->freeHead]]
                                  ->data[3]))
                         ->ui16FrameIdx);
              av_log(
                  avctx, AV_LOG_TRACE,
                  "xcoder_send_frame: after ref sframe_pool, hw frame "
                  "av_buffer_get_ref_count=%d, data[3]=%p\n",
                  av_buffer_get_ref_count(
                      ctx->sframe_pool[ctx->aFree_Avframes_list[ctx->freeHead]]
                          ->buf[0]),
                  ctx->sframe_pool[ctx->aFree_Avframes_list[ctx->freeHead]]
                      ->data[3]);
          }
        if (deq_free_frames(ctx) != 0)
        {
          ret = AVERROR_EXTERNAL;
          return ret;
        }
      //av_frame_ref(ctx->sframe_pool[((niFrameSurface1_t*)((uint8_t*)frame->data[3]))->ui16FrameIdx], frame);
      }

      // only if it's NOT sequence change flushing (in which case only the eos
      // was sent and not the first sc pkt) AND
      // only after successful sending will it be removed from fifo
      // TODO: if ni_device_session_write EOS, returned sent would have been 0, and following condition is always true
      if (SESSION_RUN_STATE_SEQ_CHANGE_DRAINING !=
          ctx->api_ctx.session_run_state)
      {
        if (! is_input_fifo_empty(ctx))
        {
          av_fifo_drain(ctx->fme_fifo, sizeof(AVFrame));
          av_log(avctx, AV_LOG_DEBUG, "fme popped, fifo size: %lu\n",
                 av_fifo_size(ctx->fme_fifo) / sizeof(AVFrame));
        }
        av_frame_unref(&ctx->buffered_fme);
        ishwframe = (ctx->buffered_fme.format == AV_PIX_FMT_NI_QUAD) &&
                    (ctx->api_ctx.auto_dl_handle == 0);
        if (ishwframe)
        {
            if (ctx->buffered_fme.buf[0])
                av_log(avctx, AV_LOG_TRACE, "xcoder_send_frame: after unref buffered_fme, hw frame av_buffer_get_ref_count=%d\n", av_buffer_get_ref_count(ctx->buffered_fme.buf[0]));
            else
                av_log(avctx, AV_LOG_TRACE, "xcoder_send_frame: after unref buffered_fme, hw frame av_buffer_get_ref_count=0 (buf[0] is NULL)\n");
        }
      }
      else
      {
        av_log(avctx, AV_LOG_TRACE, "XCoder frame(eos) sent, sequence changing!"
               " NO fifo pop !\n");
      }

      // pushing input pts in circular FIFO
      ctx->api_ctx.enc_pts_list[ctx->api_ctx.enc_pts_w_idx % NI_FIFO_SZ] = ctx->api_fme.data.frame.pts;
      ctx->api_ctx.enc_pts_w_idx ++;

      // have another check before return: if no more frames in fifo to send and
      // we've got eos (NULL) frame from upper stream, flag for flushing
      if (ctx->eos_fme_received && is_input_fifo_empty(ctx))
      {
        av_log(avctx, AV_LOG_DEBUG, "Upper stream EOS frame received, fifo "
               "empty, start flushing ..\n");
        ctx->encoder_flushing = 1;
      }

      ret = 0;
    }
    // pushing input pts in circular FIFO
    // ctx->api_ctx.enc_pts_list[ctx->api_ctx.enc_pts_w_idx % NI_FIFO_SZ] =
    // ctx->api_fme.data.frame.pts; ctx->api_ctx.enc_pts_w_idx ++;
    // ret = 0;
  }
#endif
  if (ctx->encoder_flushing)
  {
    av_log(avctx, AV_LOG_DEBUG, "xcoder_send_frame flushing ..\n");
    ret = ni_device_session_flush(&ctx->api_ctx, NI_DEVICE_TYPE_ENCODER);
  }

  av_log(avctx, AV_LOG_VERBOSE, "XCoder send frame return %d \n", ret);
  return ret;
}

static int xcoder_encode_reinit(AVCodecContext *avctx)
{
  int ret = 0;
  XCoderH265EncContext *ctx = avctx->priv_data;
  bool ishwframe;
  ni_device_handle_t device_handle = ctx->api_ctx.device_handle;
  ni_device_handle_t blk_io_handle = ctx->api_ctx.blk_io_handle;
  int hw_id = ctx->api_ctx.hw_id;
  char tmp_blk_dev_name[NI_MAX_DEVICE_NAME_LEN];
  int bit_depth = 1;
  int pix_fmt = AV_PIX_FMT_YUV420P;
  int stride, ori_stride;
  bool bIsSmallPicture = false;
  AVFrame temp_frame;

  ff_xcoder_strncpy(tmp_blk_dev_name, ctx->api_ctx.blk_dev_name,
                    NI_MAX_DEVICE_NAME_LEN);

  // re-init avctx's resolution to the changed one that is
  // stored in the first frame of the fifo
  av_fifo_generic_peek(ctx->fme_fifo, &temp_frame , sizeof(AVFrame), NULL);
  temp_frame.extended_data = temp_frame.data;
  av_log(avctx, AV_LOG_INFO, "xcoder_receive_packet resolution "
         "changing %dx%d -> %dx%d "
         "format %d -> %d\n",
         avctx->width, avctx->height,
         temp_frame.width, temp_frame.height,
         avctx->pix_fmt, temp_frame.format);

  avctx->width = temp_frame.width;
  avctx->height = temp_frame.height;
  avctx->pix_fmt = temp_frame.format;
  ishwframe = temp_frame.format == AV_PIX_FMT_NI_QUAD;

  if (ishwframe)
  {
    bit_depth = (uint8_t)((niFrameSurface1_t*)((uint8_t*)temp_frame.data[3]))->bit_depth;
  //int8_t bit_depth; //1 ==8bit per pixel, 2 ==10
  //int8_t encoding_type; //planar
    av_log(avctx, AV_LOG_INFO, "xcoder_receive_packet hw frame bit depth "
           "changing %d -> %d\n",
           ctx->api_ctx.bit_depth_factor, bit_depth);

    if (bit_depth != ctx->api_ctx.bit_depth_factor)
    {
      switch (avctx->sw_pix_fmt)
      {
        case AV_PIX_FMT_YUV420P:
          if (bit_depth == 2)
          {
            avctx->sw_pix_fmt = AV_PIX_FMT_YUV420P10LE;
          }
          break;
        case AV_PIX_FMT_YUV420P10LE:
        case AV_PIX_FMT_YUV420P10BE:
          if (bit_depth == 1)
          {
            avctx->sw_pix_fmt = AV_PIX_FMT_YUV420P;
          }
          break;
        case AV_PIX_FMT_NV12:
          if (bit_depth == 2)
          {
            avctx->sw_pix_fmt = AV_PIX_FMT_P010LE;
          }
          break;
        case AV_PIX_FMT_P010LE:
          if (bit_depth == 1)
          {
            avctx->sw_pix_fmt = AV_PIX_FMT_NV12;
          }
          break;
        case AV_PIX_FMT_NI_QUAD_10_TILE_4X4:
          if (bit_depth == 1)
          {
            avctx->sw_pix_fmt = AV_PIX_FMT_NI_QUAD_8_TILE_4X4;
          }
          break;
        case AV_PIX_FMT_NI_QUAD_8_TILE_4X4:
          if (bit_depth == 2)
          {
            avctx->sw_pix_fmt = AV_PIX_FMT_NI_QUAD_10_TILE_4X4;
          }
          break;
        default:
          break;
      }
    }
    pix_fmt = avctx->sw_pix_fmt;
  }
  else
  {
      switch (temp_frame.format)
      {
        case AV_PIX_FMT_YUV420P:
        case AV_PIX_FMT_NV12:
          bit_depth = 1;
          break;
        case AV_PIX_FMT_YUV420P10LE:
        case AV_PIX_FMT_YUV420P10BE:
        case AV_PIX_FMT_P010LE:
          bit_depth = 2;
          break;
        default:
          break;
      }
      pix_fmt = temp_frame.format;
  }

  ctx->eos_fme_received = 0;
  ctx->encoder_eof = 0;
  ctx->encoder_flushing = 0;
  //ctx->started = 0;
  ctx->firstPktArrived = 0;
  ctx->spsPpsArrived = 0;
  ctx->spsPpsHdrLen = 0;
  if (ctx->p_spsPpsHdr) {
      free(ctx->p_spsPpsHdr);
      ctx->p_spsPpsHdr = NULL;
  }

  stride = FFALIGN(temp_frame.width, 128);
  ori_stride = FFALIGN(ctx->api_ctx.ori_width, 128);
  if (ctx->api_param.cfg_enc_params.lookAheadDepth) {
    av_log(avctx, AV_LOG_DEBUG, "xcoder_encode_reinit 2-pass "
           "lookaheadDepth %d\n",
           ctx->api_param.cfg_enc_params.lookAheadDepth);
      if ((temp_frame.width < 272) ||
         (temp_frame.height < 256)) {
        bIsSmallPicture = true;
      }
  }
  else {
      if ((temp_frame.width < NI_MIN_WIDTH) ||
         (temp_frame.height < NI_MIN_HEIGHT)) {
        bIsSmallPicture = true;
      }
  }

  if (ctx->api_param.cfg_enc_params.multicoreJointMode) {
    av_log(avctx, AV_LOG_DEBUG, "xcoder_encode_reinit multicore "
           "joint mode\n");
      if ((temp_frame.width < 256) ||
         (temp_frame.height < 256)) {
        bIsSmallPicture = true;
      }
  }

  // fast sequence change without close / open only if new resolution < original resolution
  if ((ori_stride*ctx->api_ctx.ori_height < stride*temp_frame.height) ||
      //(ctx->api_ctx.ori_bit_depth_factor < bit_depth) ||
      (ctx->api_ctx.ori_pix_fmt != pix_fmt) ||
      bIsSmallPicture ||
      (avctx->codec_id == AV_CODEC_ID_MJPEG)) {
    xcoder_encode_close(avctx);
    ret = xcoder_encode_init(avctx);
  }
  else {
    if (avctx->codec_id == AV_CODEC_ID_AV1) {

        // AV1 8x8 alignment HW limitation is now worked around by FW cropping input resolution
        if (temp_frame.width % NI_PARAM_AV1_ALIGN_WIDTH_HEIGHT)
            av_log(avctx, AV_LOG_ERROR,
                   "resolution change: AV1 Picture Width not aligned to %d - picture will be cropped\n",
                   NI_PARAM_AV1_ALIGN_WIDTH_HEIGHT);

        if (temp_frame.height % NI_PARAM_AV1_ALIGN_WIDTH_HEIGHT)
            av_log(avctx, AV_LOG_ERROR,
                   "resolution change: AV1 Picture Height not aligned to %d - picture will be cropped\n",
                   NI_PARAM_AV1_ALIGN_WIDTH_HEIGHT);
    }
    ret = xcoder_encode_sequence_change(avctx, temp_frame.width, temp_frame.height, bit_depth);
  }

  // keep device handle(s) open during sequence change to fix mem bin buffer not recycled
  ctx->api_ctx.device_handle  = device_handle;
  ctx->api_ctx.blk_io_handle = blk_io_handle;
  ctx->api_ctx.hw_id = hw_id;
  ff_xcoder_strncpy(ctx->api_ctx.blk_dev_name, tmp_blk_dev_name,
                    NI_MAX_DEVICE_NAME_LEN);
  ctx->api_ctx.session_run_state = SESSION_RUN_STATE_SEQ_CHANGE_OPENING; // this state is referenced when sending first frame after sequence change

  return ret;
}

int xcoder_receive_packet(AVCodecContext *avctx, AVPacket *pkt)
{
  XCoderH265EncContext *ctx = avctx->priv_data;
  int i, ret = 0;
  int recv;
  AVFrame *frame = NULL;
  ni_packet_t *xpkt = &ctx->api_pkt.data.packet;
  bool av1_output_frame = 0;

  av_log(avctx, AV_LOG_VERBOSE, "XCoder receive packet\n");

  if (ctx->encoder_eof)
  {
    av_log(avctx, AV_LOG_VERBOSE, "xcoder_receive_packet: EOS\n");
    return AVERROR_EOF;
  }

  if (ni_packet_buffer_alloc(xpkt, NI_MAX_TX_SZ)) {
      av_log(avctx, AV_LOG_ERROR,
             "xcoder_receive_packet: packet buffer size %d allocation failed\n",
             NI_MAX_TX_SZ);
      return AVERROR(ENOMEM);
  }

  if (avctx->codec_id == AV_CODEC_ID_MJPEG && (!ctx->spsPpsArrived)) {
      ctx->spsPpsArrived = 1;
      // for Jpeg, start pkt_num counter from 1, because unlike video codecs
      // (1st packet is header), there is no header for Jpeg
      ctx->api_ctx.pkt_num = 1;
  }

  while (1)
  {
    xpkt->recycle_index = -1;
    recv = ni_device_session_read(&ctx->api_ctx, &(ctx->api_pkt), NI_DEVICE_TYPE_ENCODER);

    av_log(avctx, AV_LOG_TRACE,
           "XCoder receive packet: xpkt.end_of_stream=%d, xpkt.data_len=%d, "
           "xpkt.frame_type=%d, recv=%d, encoder_flushing=%d, encoder_eof=%d\n",
           xpkt->end_of_stream, xpkt->data_len, xpkt->frame_type, recv,
           ctx->encoder_flushing, ctx->encoder_eof);

    if (recv <= 0)
    {
      ctx->encoder_eof = xpkt->end_of_stream;
      /* not ready ?? */
      if (ctx->encoder_eof || xpkt->end_of_stream)
      {
        if (SESSION_RUN_STATE_SEQ_CHANGE_DRAINING ==
            ctx->api_ctx.session_run_state)
        {
          // after sequence change completes, reset codec state
          av_log(avctx, AV_LOG_INFO, "xcoder_receive_packet 1: sequence "
                 "change completed, return AVERROR(EAGAIN) and will reopen "
                 "codec!\n");

          ret = xcoder_encode_reinit(avctx);
          av_log(avctx, AV_LOG_DEBUG, "xcoder_receive_packet: xcoder_encode_reinit ret %d\n", ret);
          if (ret >= 0)
          {
            ret = AVERROR(EAGAIN);

            xcoder_send_frame(avctx, NULL);

            ctx->api_ctx.session_run_state = SESSION_RUN_STATE_NORMAL;
          }
          break;
        }

        ret = AVERROR_EOF;
        av_log(avctx, AV_LOG_VERBOSE, "xcoder_receive_packet: got encoder_eof, return AVERROR_EOF\n");
        break;
      }
      else
      {
          bool bIsReset = false;
          if (NI_RETCODE_ERROR_VPU_RECOVERY == recv) {
              xcoder_encode_reset(avctx);
              bIsReset = true;
          }
        ret = AVERROR(EAGAIN);
        if ((!ctx->encoder_flushing && !ctx->eos_fme_received) || bIsReset) // if encode session was reset, can't read again with invalid session, must break out first
        {
          av_log(avctx, AV_LOG_TRACE, "xcoder_receive_packet: NOT encoder_"
                 "flushing, NOT eos_fme_received, return AVERROR(EAGAIN)\n");
          break;
        }
      }
    }
    else
    {
      /* got encoded data back */
      int meta_size = ctx->api_ctx.meta_size;
      if (avctx->pix_fmt == AV_PIX_FMT_NI_QUAD && xpkt->recycle_index >= 0 &&
          avctx->height >= NI_MIN_HEIGHT && avctx->width >= NI_MIN_WIDTH &&
          xpkt->recycle_index < NI_GET_MAX_HWDESC_FRAME_INDEX(ctx->api_ctx.ddr_config)) {
          int avframe_index =
              recycle_index_2_avframe_index(ctx, xpkt->recycle_index);
          av_log(avctx, AV_LOG_VERBOSE, "UNREF trace ui16FrameIdx = [%d].\n",
                 xpkt->recycle_index);
          if (avframe_index >= 0 && ctx->sframe_pool[avframe_index]) {
              frame = ctx->sframe_pool[avframe_index];
              ((niFrameSurface1_t *)((uint8_t *)frame->data[3]))
                  ->device_handle =
                  (int32_t)((int64_t)(ctx->api_ctx.blk_io_handle) &
                            0xFFFFFFFF); // update handle to most recent alive

              av_frame_unref(ctx->sframe_pool[avframe_index]);
              av_log(avctx, AV_LOG_DEBUG,
                     "AVframe_index = %d pushed to free tail %d\n",
                     avframe_index, ctx->freeTail);
              enq_free_frames(ctx, avframe_index);
              // enqueue the index back to free
              xpkt->recycle_index = -1;
          } else {
              av_log(avctx, AV_LOG_DEBUG,
                     "can't push to tail - avframe_index %d sframe_pool %p\n",
                     avframe_index, ctx->sframe_pool[avframe_index]);
          }
      }

      if (! ctx->spsPpsArrived)
      {
        ret = AVERROR(EAGAIN);
        ctx->spsPpsArrived = 1;
        ctx->spsPpsHdrLen = recv - meta_size;
        ctx->p_spsPpsHdr   = malloc(ctx->spsPpsHdrLen);
        if (!ctx->p_spsPpsHdr) {
            ret = AVERROR(ENOMEM);
            break;
        }

        memcpy(ctx->p_spsPpsHdr, (uint8_t *)xpkt->p_data + meta_size,
               xpkt->data_len - meta_size);
        //printf("encoder: very first data chunk saved: %d !\n",
        //       ctx->spsPpsHdrLen);

        // start pkt_num counter from 1 to get the real first frame
        ctx->api_ctx.pkt_num = 1;
        // for low-latency mode, keep reading until the first frame is back
        if (ctx->api_param.low_delay_mode)
        {
          av_log(avctx, AV_LOG_TRACE, "XCoder receive packet: low delay mode,"
                 " keep reading until 1st pkt arrives\n");
          continue;
        }
        break;
      }

      // handle pic skip
      if (xpkt->frame_type == 3) // 0=I, 1=P, 2=B, 3=not coded / skip
      {
          ret = AVERROR(EAGAIN);
          if (ctx->first_frame_pts == INT_MIN)
              ctx->first_frame_pts = xpkt->pts;
          if (AV_CODEC_ID_AV1 == avctx->codec_id) {
              ctx->latest_dts = xpkt->pts;
          } else if (ctx->total_frames_received < ctx->dtsOffset) {
              // guess dts
              ctx->latest_dts = ctx->first_frame_pts +
                                ((ctx->gop_offset_count - ctx->dtsOffset) *
                                 avctx->ticks_per_frame);
              ctx->gop_offset_count++;
          } else {
              // get dts from pts FIFO
              ctx->latest_dts =
                  ctx->api_ctx
                      .enc_pts_list[ctx->api_ctx.enc_pts_r_idx % NI_FIFO_SZ];
              ctx->api_ctx.enc_pts_r_idx++;
          }
          if (ctx->latest_dts > xpkt->pts) {
              ctx->latest_dts = xpkt->pts;
          }
          ctx->total_frames_received++;
          
          if (!ctx->encoder_flushing && ! ctx->eos_fme_received)
          {
            av_log(avctx, AV_LOG_TRACE, "xcoder_receive_packet: skip"
                   " picture output, return AVERROR(EAGAIN)\n");
            break;
          }
          else
            continue;
      }

      // handle av1
      if (avctx->codec_id == AV_CODEC_ID_AV1) {
          av_log(
              avctx, AV_LOG_TRACE,
              "xcoder_receive_packet: AV1 xpkt buf %p size %d show_frame %d\n",
              xpkt->p_data, xpkt->data_len, xpkt->av1_show_frame);
          if (!xpkt->av1_show_frame && (ctx->total_frames_received >= 1)) {
              xpkt->av1_p_buffer[xpkt->av1_buffer_index]    = xpkt->p_buffer;
              xpkt->av1_p_data[xpkt->av1_buffer_index]      = xpkt->p_data;
              xpkt->av1_buffer_size[xpkt->av1_buffer_index] = xpkt->buffer_size;
              xpkt->av1_data_len[xpkt->av1_buffer_index]    = xpkt->data_len;
              xpkt->av1_buffer_index++;
              xpkt->p_buffer    = NULL;
              xpkt->p_data      = NULL;
              xpkt->buffer_size = 0;
              xpkt->data_len    = 0;
              if (xpkt->av1_buffer_index >= MAX_AV1_ENCODER_GOP_NUM) {
                  av_log(avctx, AV_LOG_ERROR,
                         "xcoder_receive_packet: recv AV1 not shown frame "
                         "number %d >= %d, return AVERROR_EXTERNAL\n",
                         xpkt->av1_buffer_index, MAX_AV1_ENCODER_GOP_NUM);
                  ret = AVERROR_EXTERNAL;
                  break;
              } else if (!ctx->encoder_flushing && !ctx->eos_fme_received) {
                  av_log(avctx, AV_LOG_TRACE,
                         "xcoder_receive_packet: recv AV1 not shown frame, "
                         "return AVERROR(EAGAIN)\n");
                  ret = AVERROR(EAGAIN);
                  break;
              } else {
                  if (ni_packet_buffer_alloc(xpkt, NI_MAX_TX_SZ)) {
                      av_log(avctx, AV_LOG_ERROR,
                             "xcoder_receive_packet: AV1 packet buffer size %d "
                             "allocation failed during flush\n",
                             NI_MAX_TX_SZ);
                      ret = AVERROR(ENOMEM);
                      break;
                  }
                  av_log(avctx, AV_LOG_TRACE,
                         "xcoder_receive_packet: recv AV1 not shown frame "
                         "during flush, continue..\n");
                  continue;
              }
          }
      }

      uint32_t nalu_type = 0;
      const uint8_t *p_start_code;
      uint32_t stc = -1;
      uint32_t copy_len = 0;
      uint8_t *p_src = (uint8_t*)xpkt->p_data + meta_size;
      uint8_t *p_end = p_src + (xpkt->data_len - meta_size);
      int64_t local_pts = xpkt->pts;
      int total_custom_sei_size = 0;
      int custom_sei_count = 0;
      ni_custom_sei_set_t *p_custom_sei_set;

      p_custom_sei_set = ctx->api_ctx.pkt_custom_sei_set[local_pts % NI_FIFO_SZ];
      if (p_custom_sei_set != NULL)
      {
        custom_sei_count = p_custom_sei_set->count;
        for (i = 0; i < p_custom_sei_set->count; i++)
        {
          total_custom_sei_size += p_custom_sei_set->custom_sei[i].size;
        }
      }

      if (custom_sei_count)
      {
        // if HRD or custom sei enabled, search for pic_timing or custom SEI insertion point by
        // skipping non-VCL until video data is found.
        p_start_code = p_src;
        if(AV_CODEC_ID_HEVC == avctx->codec_id)
        {
          do
          {
            stc = -1;
            p_start_code = avpriv_find_start_code(p_start_code, p_end, &stc);
            nalu_type = (stc >> 1) & 0x3F;
          } while (nalu_type > HEVC_NAL_RSV_VCL31);

          // calc. length to copy
          copy_len = p_start_code - 5 - p_src;
        }
        else if(AV_CODEC_ID_H264 == avctx->codec_id)
        {
          do
          {
            stc = -1;
            p_start_code = avpriv_find_start_code(p_start_code, p_end, &stc);
            nalu_type = stc & 0x1F;
          } while (nalu_type > H264_NAL_IDR_SLICE);

          // calc. length to copy
          copy_len = p_start_code - 5 - p_src;
        }
        else
        {
          av_log(avctx, AV_LOG_ERROR, "xcoder_receive packet: codec %d not "
               "supported for SEI !\n", avctx->codec_id);
        }
      }

      if (avctx->codec_id == AV_CODEC_ID_MJPEG && !ctx->firstPktArrived) {
          // there is no header for Jpeg, so skip header copy
          ctx->firstPktArrived = 1;
          if (ctx->first_frame_pts == INT_MIN)
              ctx->first_frame_pts = xpkt->pts;
      }

      if (! ctx->firstPktArrived)
      {
        int sizeof_spspps_attached_to_idr = ctx->spsPpsHdrLen;
        if ((avctx->flags & AV_CODEC_FLAG_GLOBAL_HEADER) &&
            (avctx->codec_id != AV_CODEC_ID_AV1)) {
            sizeof_spspps_attached_to_idr = 0;
        }
        ctx->firstPktArrived = 1;
        if (ctx->first_frame_pts == INT_MIN)
            ctx->first_frame_pts = xpkt->pts;

#if (LIBAVCODEC_VERSION_MAJOR >= 59)
        ret = ff_get_encode_buffer(avctx, pkt, xpkt->data_len - meta_size + sizeof_spspps_attached_to_idr + total_custom_sei_size, 0);
#else
        ret = ff_alloc_packet2(avctx, pkt, xpkt->data_len - meta_size + sizeof_spspps_attached_to_idr + total_custom_sei_size,
                               xpkt->data_len - meta_size + sizeof_spspps_attached_to_idr + total_custom_sei_size);
#endif
        if (! ret)
        {
          uint8_t *p_dst, *p_side_data;

          // fill in AVC/HEVC sidedata
          if ((avctx->flags & AV_CODEC_FLAG_GLOBAL_HEADER) &&
              (avctx->extradata_size != ctx->spsPpsHdrLen ||
               (memcmp(avctx->extradata, ctx->p_spsPpsHdr, ctx->spsPpsHdrLen) !=
                0))) {
              avctx->extradata_size = ctx->spsPpsHdrLen;
              av_freep(&avctx->extradata);
              avctx->extradata = av_mallocz(avctx->extradata_size +
                                            AV_INPUT_BUFFER_PADDING_SIZE);
              if (!avctx->extradata) {
                  av_log(avctx, AV_LOG_ERROR,
                         "Cannot allocate AVC/HEVC header of size %d.\n",
                         avctx->extradata_size);
                  return AVERROR(ENOMEM);
              }
              memcpy(avctx->extradata, ctx->p_spsPpsHdr, avctx->extradata_size);
          }

          p_side_data = av_packet_new_side_data(
              pkt, AV_PKT_DATA_NEW_EXTRADATA, ctx->spsPpsHdrLen);
          if (p_side_data)
          {
              memcpy(p_side_data, ctx->p_spsPpsHdr, ctx->spsPpsHdrLen);
          }

          p_dst = pkt->data;
          if (sizeof_spspps_attached_to_idr)
          {
              memcpy(p_dst, ctx->p_spsPpsHdr, ctx->spsPpsHdrLen);
              p_dst += ctx->spsPpsHdrLen;
          }

          if (custom_sei_count)
          {
              // copy buf_period
              memcpy(p_dst, p_src, copy_len);
              p_dst += copy_len;

              for (i = 0; i < custom_sei_count; i++)
              {
                  // copy custom sei
                  ni_custom_sei_t *p_custom_sei = &p_custom_sei_set->custom_sei[i];
                  if (p_custom_sei->location == NI_CUSTOM_SEI_LOC_AFTER_VCL)
                  {
                      break;
                  }
                  memcpy(p_dst, &p_custom_sei->data[0], p_custom_sei->size);
                  p_dst += p_custom_sei->size;
              }

              // copy the IDR data
              memcpy(p_dst, p_src + copy_len,
                     xpkt->data_len - meta_size - copy_len);
              p_dst += xpkt->data_len - meta_size - copy_len;

              // copy custom sei after slice
              for (; i < custom_sei_count; i++)
              {
                  ni_custom_sei_t *p_custom_sei = &p_custom_sei_set->custom_sei[i];
                  memcpy(p_dst, &p_custom_sei->data[0], p_custom_sei->size);
                  p_dst += p_custom_sei->size;
              }
          }
          else
          {
              memcpy(p_dst, (uint8_t*)xpkt->p_data + meta_size,
                     xpkt->data_len - meta_size);
          }
        }
      }
      else
      {
          int temp_index;
          uint32_t data_len = xpkt->data_len - meta_size + total_custom_sei_size;
          if (avctx->codec_id == AV_CODEC_ID_AV1) {
              av1_output_frame = 1;
              for (temp_index = 0; temp_index < xpkt->av1_buffer_index;
                   temp_index++) {
                  data_len += xpkt->av1_data_len[temp_index] - meta_size;
              }
          }
          // av_log(avctx, AV_LOG_TRACE, "xcoder_receive_packet: AV1 total pkt
          // size %d\n", data_len);

#if (LIBAVCODEC_VERSION_MAJOR >= 59)
          ret = ff_get_encode_buffer(avctx, pkt, data_len, 0);
#else
          ret = ff_alloc_packet2(avctx, pkt, data_len, data_len);
#endif
          if (!ret) {
              uint8_t *p_dst = pkt->data;
              if (avctx->codec_id == AV_CODEC_ID_AV1) {
                  for (temp_index = 0; temp_index < xpkt->av1_buffer_index;
                       temp_index++) {
                      memcpy(p_dst,
                             (uint8_t *)xpkt->av1_p_data[temp_index] +
                                 meta_size,
                             xpkt->av1_data_len[temp_index] - meta_size);
                      // av_log(avctx, AV_LOG_TRACE, "xcoder_receive_packet: AV1
                      // copy xpkt buf %p size %d\n",
                      // (uint8_t*)xpkt->av1_p_data[temp_index] + meta_size,
                      // xpkt->av1_data_len[temp_index] - meta_size);
                      p_dst += (xpkt->av1_data_len[temp_index] - meta_size);
                  }
              }

              if (custom_sei_count)
              {
                  // copy buf_period
                  memcpy(p_dst, p_src, copy_len);
                  p_dst += copy_len;
    
                  for (i = 0; i < custom_sei_count; i++)
                  {
                      // copy custom sei
                      ni_custom_sei_t *p_custom_sei = &p_custom_sei_set->custom_sei[i];
                      if (p_custom_sei->location == NI_CUSTOM_SEI_LOC_AFTER_VCL)
                      {
                          break;
                      }
                      memcpy(p_dst, &p_custom_sei->data[0], p_custom_sei->size);
                      p_dst += p_custom_sei->size;
                  }
    
                  // copy the IDR data
                  memcpy(p_dst, p_src + copy_len,
                         xpkt->data_len - meta_size - copy_len);
                  p_dst += xpkt->data_len - meta_size - copy_len;
    
                  // copy custom sei after slice
                  for (; i < custom_sei_count; i++)
                  {
                      ni_custom_sei_t *p_custom_sei = &p_custom_sei_set->custom_sei[i];
                      memcpy(p_dst, &p_custom_sei->data[0], p_custom_sei->size);
                      p_dst += p_custom_sei->size;
                  }
              } else {
                  memcpy(p_dst, (uint8_t *)xpkt->p_data + meta_size,
                         xpkt->data_len - meta_size);
              }
          }
      }

      // free buffer
      if (custom_sei_count)
      {
        free(p_custom_sei_set);
        ctx->api_ctx.pkt_custom_sei_set[local_pts % NI_FIFO_SZ] = NULL;
      }

      if (!ret)
      {
        if (xpkt->frame_type == 0)
        {
          pkt->flags |= AV_PKT_FLAG_KEY;
        }

        pkt->pts = xpkt->pts;
        /* to ensure pts>dts for all frames, we assign a guess pts for the first 'dtsOffset' frames and then the pts from input stream
         * is extracted from input pts FIFO.
         * if GOP = IBBBP and PTSs = 0 1 2 3 4 5 .. then out DTSs = -3 -2 -1 0 1 ... and -3 -2 -1 are the guessed values
         * if GOP = IBPBP and PTSs = 0 1 2 3 4 5 .. then out DTSs = -1 0 1 2 3 ... and -1 is the guessed value
         * the number of guessed values is equal to dtsOffset
         */
        if (AV_CODEC_ID_AV1 == avctx->codec_id) {
            pkt->dts = pkt->pts;
            av_log(avctx, AV_LOG_TRACE, "Packet dts (av1): %ld\n", pkt->dts);
        } else if (ctx->total_frames_received < ctx->dtsOffset) {
            // guess dts
            pkt->dts = ctx->first_frame_pts +
                       ((ctx->gop_offset_count - ctx->dtsOffset) *
                        avctx->ticks_per_frame);
            ctx->gop_offset_count++;
            av_log(avctx, AV_LOG_TRACE, "Packet dts (guessed): %ld\n",
                   pkt->dts);
        } else {
            // get dts from pts FIFO
            pkt->dts =
                ctx->api_ctx
                    .enc_pts_list[ctx->api_ctx.enc_pts_r_idx % NI_FIFO_SZ];
            ctx->api_ctx.enc_pts_r_idx++;
            av_log(avctx, AV_LOG_TRACE, "Packet dts: %ld\n", pkt->dts);
        }
        if (ctx->total_frames_received >= 1)
        {
          if (pkt->dts < ctx->latest_dts)
          {
            av_log(NULL, AV_LOG_WARNING, "dts: %ld < latest_dts: %ld.\n",
                    pkt->dts, ctx->latest_dts);
          }
        }
        if(pkt->dts > pkt->pts)
        {
          av_log(NULL, AV_LOG_WARNING, "dts: %ld, pts: %ld. Forcing dts = pts \n",
                  pkt->dts, pkt->pts);
          pkt->dts = pkt->pts;
          av_log(avctx, AV_LOG_TRACE, "Force dts to: %ld\n", pkt->dts);
        }
        ctx->total_frames_received++;
        ctx->latest_dts = pkt->dts;
        // TODO: avg_frame_qp will be moved to libxcoder log after updating libxcoder to refer to ffmpeg log level
        av_log(avctx, AV_LOG_DEBUG, "XCoder recv pkt #%" PRId64 ""
               " pts %" PRId64 "  dts %" PRId64 "  size %d  st_index %d frame_type %u avg qp %u\n",
               ctx->api_ctx.pkt_num - 1, pkt->pts, pkt->dts, pkt->size,
               pkt->stream_index, xpkt->frame_type, xpkt->avg_frame_qp);

        // printf("\n         Mux: pts  %lld  dts %lld   size %d  st_index %d \n\n", pkt->pts, pkt->dts, pkt->size, pkt->stream_index);
      }
      ctx->encoder_eof = xpkt->end_of_stream;
      if (ctx->encoder_eof &&
        SESSION_RUN_STATE_SEQ_CHANGE_DRAINING ==
        ctx->api_ctx.session_run_state)
      {
        // after sequence change completes, reset codec state
        av_log(avctx, AV_LOG_DEBUG, "xcoder_receive_packet 2: sequence change "
          "completed, return 0 and will reopen codec !\n");
        ret = xcoder_encode_reinit(avctx);
        av_log(avctx, AV_LOG_DEBUG, "xcoder_receive_packet: xcoder_encode_reinit ret %d\n", ret);
        if (ret >= 0)
        {
          xcoder_send_frame(avctx, NULL);
          ctx->api_ctx.session_run_state = SESSION_RUN_STATE_NORMAL;
        }
      }
      break;
    }
  }

  if ((AV_CODEC_ID_AV1 == avctx->codec_id) && xpkt->av1_buffer_index &&
      av1_output_frame) {
      av_log(avctx, AV_LOG_TRACE,
             "xcoder_receive_packet: ni_packet_buffer_free_av1 %d packtes\n",
             xpkt->av1_buffer_index);
      ni_packet_buffer_free_av1(xpkt);
  }

  av_log(avctx, AV_LOG_VERBOSE, "xcoder_receive_packet: return %d\n", ret);
  return ret;
}

// for FFmpeg 4.4+
#if (LIBAVCODEC_VERSION_MAJOR >= 59 || LIBAVCODEC_VERSION_MAJOR >= 58 && LIBAVCODEC_VERSION_MINOR >= 134)
int ff_xcoder_receive_packet(AVCodecContext *avctx, AVPacket *pkt)
{
    XCoderH265EncContext *ctx = avctx->priv_data;
    AVFrame *frame = &ctx->buffered_fme;
    int ret;

    ret = ff_encode_get_frame(avctx, frame);
    if (!ctx->encoder_flushing && ret >= 0 || ret == AVERROR_EOF)
    {
        ret = xcoder_send_frame(avctx, (ret == AVERROR_EOF ? NULL : frame));
        if (ret < 0 && ret != AVERROR_EOF)
        {
            av_frame_unref(frame);
            return ret;
        }
    }
    // Once send_frame returns EOF go on receiving packets until EOS is met.
    return xcoder_receive_packet(avctx, pkt);
}
#endif

int xcoder_encode_frame(AVCodecContext *avctx, AVPacket *pkt,
                        const AVFrame *frame, int *got_packet)
{
  XCoderH265EncContext *ctx = avctx->priv_data;
  int ret;

  av_log(avctx, AV_LOG_VERBOSE, "XCoder encode frame\n");

  if (!ctx->encoder_flushing)
  {
    ret = xcoder_send_frame(avctx, frame);
    if (ret < 0)
    {
      return ret;
    }
  }

  ret = xcoder_receive_packet(avctx, pkt);
  if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF)
  {
    *got_packet = 0;
  }
  else if (ret < 0)
  {
    return ret;
  }
  else
  {
    *got_packet = 1;
  }

  return 0;
}

bool free_frames_isempty(XCoderH265EncContext *ctx)
{
  return  (ctx->freeHead == ctx->freeTail);
}

bool free_frames_isfull(XCoderH265EncContext *ctx)
{
  return  (ctx->freeHead == ((ctx->freeTail == MAX_NUM_FRAMEPOOL_HWAVFRAME) ? 0 : ctx->freeTail + 1));
}

int deq_free_frames(XCoderH265EncContext *ctx)
{
  if (free_frames_isempty(ctx))
  {
    return -1;
  }
  ctx->aFree_Avframes_list[ctx->freeHead] = -1;
  ctx->freeHead = (ctx->freeHead == MAX_NUM_FRAMEPOOL_HWAVFRAME) ? 0 : ctx->freeHead + 1;
  return 0;
}

int enq_free_frames(XCoderH265EncContext *ctx, int idx)
{
  if (free_frames_isfull(ctx))
  {
    return -1;
  }
  ctx->aFree_Avframes_list[ctx->freeTail] = idx;
  ctx->freeTail = (ctx->freeTail == MAX_NUM_FRAMEPOOL_HWAVFRAME) ? 0 : ctx->freeTail + 1;
  return 0;
}

int recycle_index_2_avframe_index(XCoderH265EncContext *ctx, uint32_t recycleIndex)
{
  int i;
  for (i = 0; i < MAX_NUM_FRAMEPOOL_HWAVFRAME; i++)
  {
    if (ctx->sframe_pool[i]->data[3])
    {
      if (((niFrameSurface1_t*)((uint8_t*)ctx->sframe_pool[i]->data[3]))->ui16FrameIdx == recycleIndex)
      {
        return i;
      }
      else
      {
        //av_log(NULL, AV_LOG_TRACE, "sframe_pool[%d] ui16FrameIdx %u != recycleIndex %u\n", i, ((niFrameSurface1_t*)((uint8_t*)ctx->sframe_pool[i]->data[3]))->ui16FrameIdx, recycleIndex);
      }
    }
    else
    {
      //av_log(NULL, AV_LOG_TRACE, "sframe_pool[%d] data[3] NULL\n", i);
    }
  }
  return -1;
}

// Needed for hwframe on FFmpeg-n4.3+
#if (LIBAVCODEC_VERSION_MAJOR >= 59 || LIBAVCODEC_VERSION_MAJOR >= 58 && LIBAVCODEC_VERSION_MINOR >= 82)
const AVCodecHWConfigInternal *ff_ni_enc_hw_configs[] = {
  HW_CONFIG_ENCODER_FRAMES(NI_QUAD,  NI_QUADRA),
  HW_CONFIG_ENCODER_DEVICE(NV12, NI_QUADRA),
  HW_CONFIG_ENCODER_DEVICE(P010, NI_QUADRA),
  HW_CONFIG_ENCODER_DEVICE(YUV420P, NI_QUADRA),
  HW_CONFIG_ENCODER_DEVICE(YUV420P10, NI_QUADRA),
  NULL,
};
#endif
