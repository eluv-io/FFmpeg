/*
 * XCoder Codec Lib Wrapper
 * Copyright (c) 2018 NetInt
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

/**
 * @file
 * XCoder codec lib wrapper.
 */

#include "nicodec.h"
#include "get_bits.h"
#include "internal.h"
#include "libavcodec/h264.h"
#include "libavcodec/h264_sei.h"
#include "libavcodec/hevc.h"
#include "libavcodec/hevc_sei.h"
#include "libavutil/eval.h"
#include "libavutil/hdr_dynamic_metadata.h"
#include "libavutil/hwcontext.h"
#include "libavutil/hwcontext_internal.h"
#include "libavutil/hwcontext_ni_quad.h"
#include "libavutil/imgutils.h"
#include "libavutil/intreadwrite.h"
#include "libavutil/mastering_display_metadata.h"
#include "libavutil/pixdesc.h"
#include "nidec.h"

#include <math.h>
#include <ni_av_codec.h>
#include <ni_rsrc_api.h>

#define NAL_264(X) ((X) & (0x1F))
#define NAL_265(X) (((X)&0x7E) >> 1)
#define MAX_HEADERS_SIZE 1000

static const char *const var_names[] = {
  "in_w", "iw",   ///< width  of the input video
  "in_h", "ih",   ///< height of the input video
  "out_w", "ow",  ///< width  of the cropped video
  "out_h", "oh",  ///< height of the cropped video
  "x",
  "y",
  NULL
};

enum var_name {
  VAR_IN_W, VAR_IW,
  VAR_IN_H, VAR_IH,
  VAR_OUT_W, VAR_OW,
  VAR_OUT_H, VAR_OH,
  VAR_X,
  VAR_Y,
  VAR_VARS_NB
};

static inline void ni_align_free(void *opaque, uint8_t *data)
{
  ni_buf_t *buf = (ni_buf_t *)opaque;
  if (buf)
  {
    ni_decoder_frame_buffer_pool_return_buf(buf, (ni_buf_pool_t *)buf->pool);
  }
}

static inline void ni_align_free_nop(void *opaque, uint8_t *data)
{
}

static inline void ni_free(void *opaque, uint8_t *data)
{
  free(data);
}

static inline void ni_frame_free(void *opaque, uint8_t *data)
{
  if (data)
  {
    niFrameSurface1_t* p_data3 = (niFrameSurface1_t*)data;
    int ret;
    if (p_data3->ui16FrameIdx != 0)
    {
      av_log(NULL, AV_LOG_DEBUG, "Recycle trace ui16FrameIdx = [%d] DevHandle %d\n", p_data3->ui16FrameIdx, p_data3->device_handle);
      ret = ni_hwframe_buffer_recycle(p_data3, p_data3->device_handle);
      if (ret != NI_RETCODE_SUCCESS)
      {
        av_log(NULL, AV_LOG_ERROR, "ERROR Failed to recycle trace ui16frameidx = [%d] DevHandle %d\n", p_data3->ui16FrameIdx, p_data3->device_handle);
      }
    }
    free(data);
  }
}

static enum AVPixelFormat ni_supported_pixel_formats[] =
{
  AV_PIX_FMT_YUV420P, //0
  AV_PIX_FMT_YUV420P10LE,
  AV_PIX_FMT_NV12,
  AV_PIX_FMT_P010LE,
  AV_PIX_FMT_NONE, //convert RGB to unused 
  AV_PIX_FMT_NONE,
  AV_PIX_FMT_NONE,
  AV_PIX_FMT_NONE,
  AV_PIX_FMT_NONE, //8
  AV_PIX_FMT_NONE,
  AV_PIX_FMT_NONE,
  AV_PIX_FMT_NONE,
  AV_PIX_FMT_NONE, //12
  AV_PIX_FMT_NI_QUAD_8_TILE_4X4,
  AV_PIX_FMT_NI_QUAD_10_TILE_4X4,
  AV_PIX_FMT_NONE, //15
};

static inline int ni_pix_fmt_2_ff_pix_fmt(ni_pix_fmt_t pix_fmt)
{
  return ni_supported_pixel_formats[pix_fmt];
}

int parse_symbolic_decoder_param(XCoderH264DecContext *s)
{
  ni_decoder_input_params_t *pdec_param = &s->api_param.dec_input_params;
  int i, ret;
  double res;
  double var_values[VAR_VARS_NB];

  if (pdec_param == NULL)
  {
    return AVERROR_INVALIDDATA;
  }
  
  for (i = 0; i < NI_MAX_NUM_OF_DECODER_OUTPUTS; i++)
  {
    /*Set output width and height*/
    var_values[VAR_IN_W] = var_values[VAR_IW] = pdec_param->crop_whxy[i][0];
    var_values[VAR_IN_H] = var_values[VAR_IH] = pdec_param->crop_whxy[i][1];
    var_values[VAR_OUT_W] = var_values[VAR_OW] = pdec_param->crop_whxy[i][0];
    var_values[VAR_OUT_H] = var_values[VAR_OH] = pdec_param->crop_whxy[i][1];
    if (pdec_param->cr_expr[i][0][0] && pdec_param->cr_expr[i][1][0])
    {
        if (av_expr_parse_and_eval(&res, pdec_param->cr_expr[i][0], var_names,
                                   var_values, NULL, NULL, NULL, NULL, NULL, 0,
                                   s) < 0) {
            return AVERROR_INVALIDDATA;
        }
        var_values[VAR_OUT_W] = var_values[VAR_OW] = (double)floor(res);
        if (av_expr_parse_and_eval(&res, pdec_param->cr_expr[i][1], var_names,
                                   var_values, NULL, NULL, NULL, NULL, NULL, 0,
                                   s) < 0) {
            return AVERROR_INVALIDDATA;
        }
        var_values[VAR_OUT_H] = var_values[VAR_OH] = (double)floor(res);
        /* evaluate again ow as it may depend on oh */
        ret = av_expr_parse_and_eval(&res, pdec_param->cr_expr[i][0], var_names,
                                     var_values, NULL, NULL, NULL, NULL, NULL,
                                     0, s);
        if (ret < 0) {
            return AVERROR_INVALIDDATA;
        }
        var_values[VAR_OUT_W] = var_values[VAR_OW] = (double)floor(res);
        pdec_param->crop_whxy[i][0]                = (int)var_values[VAR_OUT_W];
        pdec_param->crop_whxy[i][1]                = (int)var_values[VAR_OUT_H];
    } 
    /*Set output crop offset X,Y*/
    if (pdec_param->cr_expr[i][2][0])
    {
        ret = av_expr_parse_and_eval(&res, pdec_param->cr_expr[i][2], var_names,
                                     var_values, NULL, NULL, NULL, NULL, NULL,
                                     0, s);
        if (ret < 0) {
            return AVERROR_INVALIDDATA;
        }
      var_values[VAR_X] = res;
      pdec_param->crop_whxy[i][2] = floor(var_values[VAR_X]);
    }
    if (pdec_param->cr_expr[i][3][0])
    {
        ret = av_expr_parse_and_eval(&res, pdec_param->cr_expr[i][3], var_names,
                                     var_values, NULL, NULL, NULL, NULL, NULL,
                                     0, s);
        if (ret < 0) {
            return AVERROR_INVALIDDATA;
        }
      var_values[VAR_Y] = res;
      pdec_param->crop_whxy[i][3] = floor(var_values[VAR_Y]);
    }
    /*Set output Scale*/
    /*Reset OW and OH to next lower even number*/
    var_values[VAR_OUT_W] = var_values[VAR_OW] =
        (double)(pdec_param->crop_whxy[i][0] -
                 (pdec_param->crop_whxy[i][0] % 2));
    var_values[VAR_OUT_H] = var_values[VAR_OH] =
        (double)(pdec_param->crop_whxy[i][1] -
                 (pdec_param->crop_whxy[i][1] % 2));
    if (pdec_param->sc_expr[i][0][0] && pdec_param->sc_expr[i][1][0])
    {
        if (av_expr_parse_and_eval(&res, pdec_param->sc_expr[i][0], var_names,
                                   var_values, NULL, NULL, NULL, NULL, NULL, 0,
                                   s) < 0) {
            return AVERROR_INVALIDDATA;
        }
        pdec_param->scale_wh[i][0] = ceil(res);
        ret = av_expr_parse_and_eval(&res, pdec_param->sc_expr[i][1], var_names,
                                     var_values, NULL, NULL, NULL, NULL, NULL,
                                     0, s);
        if (ret < 0) {
            return AVERROR_INVALIDDATA;
        }
        pdec_param->scale_wh[i][1] = ceil(res);
    }
  }
  return 0;
}

int ff_xcoder_dec_init(AVCodecContext *avctx, XCoderH264DecContext *s)
{
  /* ToDo: call xcode_dec_open to open a decoder instance */
  int ret = 0;
  ni_xcoder_params_t *p_param = &s->api_param;

  s->api_ctx.hw_id = s->dev_dec_idx;
  s->api_ctx.decoder_low_delay = 0;
  ff_xcoder_strncpy(s->api_ctx.blk_dev_name, s->dev_blk_name,
                    NI_MAX_DEVICE_NAME_LEN);

  ret = ni_device_session_open(&s->api_ctx, NI_DEVICE_TYPE_DECODER);
  if (ret != 0)
  {
    av_log(avctx, AV_LOG_ERROR, "Failed to open decoder (status = %d), "
           "resource unavailable\n", ret);
    ret = AVERROR_EXTERNAL;
    ff_xcoder_dec_close(avctx, s);
  }
  else
  {
      s->dev_xcoder_name = s->api_ctx.dev_xcoder_name;
      s->blk_xcoder_name = s->api_ctx.blk_xcoder_name;
      s->dev_dec_idx     = s->api_ctx.hw_id;
      av_log(avctx, AV_LOG_VERBOSE,
             "XCoder %s.%d (inst: %d) opened successfully\n",
             s->dev_xcoder_name, s->dev_dec_idx, s->api_ctx.session_id);

      if (p_param->dec_input_params.hwframes) {
          if (!avctx->hw_device_ctx) // avctx->hw_frames_ctx)
          {
              char buf[64] = {0};
              av_log(avctx, AV_LOG_DEBUG,
                     "nicodec.c:ff_xcoder_dec_init() hwdevice_ctx_create\n");
              av_hwdevice_ctx_create(&avctx->hw_device_ctx, AV_HWDEVICE_TYPE_NI_QUADRA,
                                     buf, NULL, 0); // create with null device
          }
          if (!avctx->hw_frames_ctx) {
              avctx->hw_frames_ctx = av_hwframe_ctx_alloc(avctx->hw_device_ctx);

              if (!avctx->hw_frames_ctx) {
                  ret = AVERROR(ENOMEM);
                  return ret;
              }
          }
          s->frames = (AVHWFramesContext *)avctx->hw_frames_ctx->data;

          s->frames->format = AV_PIX_FMT_NI_QUAD;
          s->frames->width  = avctx->width;
          s->frames->height = avctx->height;

          s->frames->sw_format = avctx->sw_pix_fmt;
          // Decoder has its own dedicated pool
          s->frames->initial_pool_size = -1;

          ret = av_hwframe_ctx_init(avctx->hw_frames_ctx);

          avctx->pix_fmt       = AV_PIX_FMT_NI_QUAD;
          s->api_ctx.hw_action = NI_CODEC_HW_ENABLE;
      } else {
          // reassign in case above conditions alter value
          avctx->pix_fmt       = avctx->sw_pix_fmt;
          s->api_ctx.hw_action = NI_CODEC_HW_NONE;
      }
  }

  return ret;
}

int ff_xcoder_dec_close(AVCodecContext *avctx, XCoderH264DecContext *s)
{
    ni_session_context_t *p_ctx = &s->api_ctx;

    if (p_ctx)
    {
        // dec params in union with enc params struct
        ni_retcode_t ret;
        ni_xcoder_params_t *p_param = &s->api_param; 

        ret = ni_device_session_close(p_ctx, s->eos, NI_DEVICE_TYPE_DECODER);
        if (NI_RETCODE_SUCCESS != ret)
        {
            av_log(avctx, AV_LOG_ERROR,
                   "Failed to close Decode Session (status = %d)\n", ret);
        }
        ni_device_session_context_clear(p_ctx);

        if (p_param->dec_input_params.hwframes)
        {
            av_log(avctx, AV_LOG_ERROR,
                   "File BLK handle %d close suspended to frames Uninit\n",
                   p_ctx->blk_io_handle); // suspended_device_handle
            if (avctx->hw_frames_ctx)
            {
                AVHWFramesContext *ctx =
                    (AVHWFramesContext *)avctx->hw_frames_ctx->data;
                if (ctx)
                {
                    NIFramesContext *dst_ctx = ctx->internal->priv;
                    if (dst_ctx)
                    {
                        dst_ctx->suspended_device_handle = p_ctx->blk_io_handle;
                    }
                }
            }
#ifdef __linux__
            ni_device_close(p_ctx->device_handle);
#endif
        }
        else
        {
#ifdef _WIN32
            ni_device_close(p_ctx->device_handle);
#elif __linux__
            ni_device_close(p_ctx->device_handle);
            ni_device_close(p_ctx->blk_io_handle);
#endif
        }
        p_ctx->device_handle = NI_INVALID_DEVICE_HANDLE;
        p_ctx->blk_io_handle = NI_INVALID_DEVICE_HANDLE;
    }

    return 0;
}

/*!******************************************************************************
 * \brief  Extract custom sei payload data from AVPacket,
 *  and save it to ni_packet_t
 *
 * \param AVCodecContext *avctx - avcodec context
 * \param AVPacket *pkt - AVPacket
 * \param long index - pkt data index of custom sei first byte after SEI type
 * \param ni_packet_t *p_packet - netint internal packet
 * \param uint8_t sei_type - type of SEI
 * \param int vcl_found - whether got vcl in the pkt data, 1 means got
 *
 * \return - 0 on success, non-0 on failure
 ********************************************************************************/
static int ff_xcoder_extract_custom_sei(AVCodecContext *avctx, AVPacket *pkt, long index,
                                        ni_packet_t *p_packet, uint8_t sei_type, int vcl_found)
{
  int i, len;
  uint8_t *udata;
  uint8_t *sei_data;
  int sei_size;
  int sei_index;
  ni_custom_sei_t *p_custom_sei;

  av_log(avctx, AV_LOG_TRACE, "%s() enter\n", __FUNCTION__);

  if (p_packet->p_custom_sei_set == NULL)
  {
    /* max size */
    p_packet->p_custom_sei_set = (ni_custom_sei_set_t *)malloc(sizeof(ni_custom_sei_set_t));
    if (p_packet->p_custom_sei_set == NULL)
    {
      av_log(avctx, AV_LOG_ERROR, "failed to allocate all custom sei buffer.\n");
      return AVERROR(ENOMEM);
    }
    memset(p_packet->p_custom_sei_set, 0, sizeof(ni_custom_sei_set_t));
  }

  sei_index = p_packet->p_custom_sei_set->count;
  p_custom_sei = &p_packet->p_custom_sei_set->custom_sei[sei_index];
  if (sei_index >= NI_MAX_CUSTOM_SEI_CNT)
  {
    av_log(avctx, AV_LOG_WARNING, "number of custom sei in current frame is out of limit(%d).\n",
           NI_MAX_CUSTOM_SEI_CNT);
    return AVERROR(EINVAL);
  }
  sei_data = &p_custom_sei->data[0];

  /*! extract SEI payload size.
   *  the first byte after SEI type is the SEI payload size.
   *  if the first byte is 255(0xFF), it means the SEI payload size is more than 255.
   *  in this case, to get the SEI payload size is to do a summation.
   *  the end of SEI size is the first non-0xFF value.
   *  for example, 0xFF 0xFF 0x08, the SEI payload size equals to (0xFF+0xFF+0x08).
   */
  sei_size = 0;
  while (index < pkt->size && pkt->data[index] == 0xff)
  {
    sei_size += pkt->data[index++];
  }

  if (index >= pkt->size)
  {
    av_log(avctx, AV_LOG_WARNING, "custom sei corrupted: length truncated.\n");
    return AVERROR(EINVAL);
  }
  sei_size += pkt->data[index++];

  if (sei_size > NI_MAX_SEI_DATA)
  {
    av_log(avctx, AV_LOG_WARNING, "custom sei corrupted: size(%d) out of limit(%d).\n",
           sei_size, NI_MAX_SEI_DATA);
    return AVERROR(EINVAL);
  }

  udata = &pkt->data[index];

  /* set SEI payload type at the first byte */
  sei_data[0] = sei_type;

  /* extract SEI payload data
   * SEI payload data in NAL is EBSP(Encapsulated Byte Sequence Payload), 
   * need change EBSP to RBSP(Raw Byte Sequence Payload) for exact size
   */
  for (i = 0, len = 0; (i < pkt->size - index) && len < sei_size; i++, len++)
  {
    /* if the latest 3-byte data pattern matchs '00 00 03' which means udata[i] is an escaping byte,
     * discard udata[i]. */
    if (i >= 2 && udata[i - 2] == 0 && udata[i - 1] == 0 && udata[i] == 3)
    {
        len--;
        continue;
    }
    sei_data[len] = udata[i];
  }

  if (len != sei_size)
  {
    av_log(avctx, AV_LOG_WARNING, "custom sei corrupted: data truncated, "
           "required size:%d, actual size:%d.\n", sei_size, len);
    return AVERROR(EINVAL);
  }

  p_custom_sei->type = sei_type;
  p_custom_sei->size = sei_size;
  p_custom_sei->location = vcl_found ? NI_CUSTOM_SEI_LOC_AFTER_VCL : NI_CUSTOM_SEI_LOC_BEFORE_VCL;

  p_packet->p_custom_sei_set->count++;

  av_log(avctx, AV_LOG_TRACE, "%s() exit, custom sei size=%d type=%d\n",
         __FUNCTION__, sei_size, sei_type);

  return 0;
}

// return 1 if need to prepend saved header to pkt data, 0 otherwise
int ff_xcoder_add_headers(AVCodecContext *avctx, AVPacket *pkt,
                          uint8_t *extradata, int extradata_size)
{
    XCoderH264DecContext *s = avctx->priv_data;
    int ret = 0;
    const uint8_t *ptr      = pkt->data;
    const uint8_t *end      = pkt->data + pkt->size;
    uint32_t stc;
    uint8_t nalu_type;

    // check key frame packet only
    if (!(pkt->flags & AV_PKT_FLAG_KEY) || !pkt->data || !extradata ||
        !extradata_size) {
        return ret;
    }

    if (s->extradata_size == extradata_size &&
        memcmp(s->extradata, extradata, extradata_size) == 0) {
        av_log(avctx, AV_LOG_TRACE, "%s extradata unchanged.\n", __FUNCTION__);
        return ret;
    }

    // extradata (headers) non-existing or changed: save/update it in the
    // session storage
    free(s->extradata);
    s->extradata_size      = 0;
    s->got_first_key_frame = 0;
    s->extradata           = malloc(extradata_size);
    if (!s->extradata) {
        av_log(avctx, AV_LOG_ERROR, "%s memory allocation failed !\n",
               __FUNCTION__);
        return ret;
    }

    memcpy(s->extradata, extradata, extradata_size);
    s->extradata_size = extradata_size;
    // prepend header by default (assuming no header found in the pkt itself)
    ret = 1;
    // and we've got the first key frame of this stream
    s->got_first_key_frame = 1;

    // then determine if it needs to be prepended to this packet for stream
    // decoding
    while (ptr < end) {
        stc = -1;
        ptr = avpriv_find_start_code(ptr, end, &stc);
        if (ptr == end) {
            break;
        }

        if (AV_CODEC_ID_H264 == avctx->codec_id) {
            nalu_type = stc & 0x1f;

            // If SPS/PPS already exists, no need to prepend it again;
            // we use one of the SPS/PPS to simplify the checking.
            if (H264_NAL_SPS == nalu_type || H264_NAL_PPS == nalu_type) {
                ret = 0;
                break;
            } else if (nalu_type >= H264_NAL_SLICE &&
                       nalu_type <= H264_NAL_IDR_SLICE) {
                // VCL types result in stop of parsing, assuming header is
                // at front of pkt (if existing).
                break;
            }
        } else if (AV_CODEC_ID_HEVC == avctx->codec_id) {
            nalu_type = (stc >> 1) & 0x3F;

            // when header (VPS/SPS/PPS) already exists, no need to prepend it
            // again; we use one of the VPS/SPS/PPS to simplify the checking.
            if (HEVC_NAL_VPS == nalu_type || HEVC_NAL_SPS == nalu_type ||
                HEVC_NAL_PPS == nalu_type) {
                ret = 0;
                break;
            } else if (nalu_type >= HEVC_NAL_TRAIL_N &&
                       nalu_type <= HEVC_NAL_RSV_VCL31) {
                // VCL types results in stop of parsing, assuming header is
                // at front of pkt (if existing).
                break;
            }
        } else {
            av_log(avctx, AV_LOG_DEBUG, "%s not AVC/HEVC codec: %d, skip!\n",
                   __FUNCTION__, avctx->codec_id);
            ret = 0;
            break;
        }
    }

    return ret;
}

// check if the packet is SEI only and also, check if getting the header of
// streams in decoder low delay mode, and update its value
static int xcoder_packet_parse(AVCodecContext *avctx, XCoderH264DecContext *s,
                               AVPacket *pkt, ni_packet_t *p_packet)
{
    ni_xcoder_params_t *p_param = &s->api_param;
    int pkt_sei_alone = 1;
    int vcl_found = 0;
    int low_delay      = (s->api_ctx.decoder_low_delay == -1) ? 0 : s->low_delay;
    int pkt_nal_bitmap = s->pkt_nal_bitmap;
    const uint8_t *ptr = pkt->data;
    const uint8_t *end = pkt->data + pkt->size;
    uint32_t stc;
    uint8_t nalu_type;
    uint8_t sei_type = 0;
    int ret = 0, nalu_count = 0;

    if (pkt_nal_bitmap & NI_GENERATE_ALL_NAL_HEADER_BIT) {
        av_log(avctx, AV_LOG_TRACE,
               "xcoder_packet_parse(): already find the header of streams.\n");
        low_delay = 0;
    }

    while (((s->custom_sei_type != NI_INVALID_SEI_TYPE || p_param->dec_input_params.custom_sei_passthru != NI_INVALID_SEI_TYPE) || low_delay) && ptr < end)
    {
        stc = -1;
        ptr = avpriv_find_start_code(ptr, end, &stc);
        if (ptr == end)
        {
            if (0 == nalu_count)
            {
                pkt_sei_alone = 0;
                av_log(avctx, AV_LOG_TRACE, "%s(): no NAL found in pkt.\n",
                       __FUNCTION__);
            }
            break;
        }
        nalu_count++;

        if (AV_CODEC_ID_H264 == avctx->codec_id)
        {
            nalu_type = stc & 0x1f;

            //check whether the packet is sei alone
            pkt_sei_alone = (pkt_sei_alone && H264_NAL_SEI == nalu_type);

            // Enable decoder low delay mode on sequence change
            if (low_delay) {
                switch (nalu_type) {
                case H264_NAL_SPS:
                    pkt_nal_bitmap |= NI_NAL_SPS_BIT;
                    break;
                case H264_NAL_PPS:
                    pkt_nal_bitmap |= NI_NAL_PPS_BIT;
                    break;
                default:
                    break;
                }

                if (pkt_nal_bitmap == (NI_NAL_SPS_BIT | NI_NAL_PPS_BIT)) {
                    // Packets before the header is sent cannot be decoded.
                    // So set packet num to zero here.
                    av_log(avctx, AV_LOG_TRACE,
                           "xcoder_packet_parse(): Detect SPS, PPS and IDR, "
                           "enable decoder low delay mode.\n");
                    s->api_ctx.decoder_low_delay = low_delay;
                    pkt_nal_bitmap |= NI_GENERATE_ALL_NAL_HEADER_BIT;
                    low_delay = 0;
                }
            }

            // check whether the packet contains SEI NAL after VCL units
            if (H264_NAL_SEI == nalu_type)
            {
                sei_type = *ptr;
                if (vcl_found || (s->custom_sei_type == sei_type || p_param->dec_input_params.custom_sei_passthru == sei_type))
                {
                    // SEI after VCL found or SEI type indicated then extract the SEI unit;
                    ret = ff_xcoder_extract_custom_sei(avctx, pkt, ptr + 1 - pkt->data, p_packet, sei_type, vcl_found);
                    if (ret != 0)
                    {
                        return ret;
                    }
                }
            }
            else if ((nalu_type >= H264_NAL_SLICE) && (nalu_type <= H264_NAL_IDR_SLICE))
            {
                vcl_found = 1;
            }
        } else if (AV_CODEC_ID_HEVC == avctx->codec_id) {
            nalu_type = (stc >> 1) & 0x3F;

            // check whether the packet is sei alone
            pkt_sei_alone = (pkt_sei_alone && (HEVC_NAL_SEI_PREFIX == nalu_type || HEVC_NAL_SEI_SUFFIX == nalu_type));

            // Enable decoder low delay mode on sequence change
            if (low_delay) {
                switch (nalu_type) {
                case HEVC_NAL_VPS:
                    pkt_nal_bitmap |= NI_NAL_VPS_BIT;
                    break;
                case HEVC_NAL_SPS:
                    pkt_nal_bitmap |= NI_NAL_SPS_BIT;
                    break;
                case HEVC_NAL_PPS:
                    pkt_nal_bitmap |= NI_NAL_PPS_BIT;
                    break;
                default:
                    break;
                }

                if (pkt_nal_bitmap == (NI_NAL_VPS_BIT | NI_NAL_SPS_BIT | NI_NAL_PPS_BIT)) {
                    av_log(avctx, AV_LOG_TRACE,
                           "xcoder_packet_parse(): Detect VPS, SPS, PPS and IDR, "
                           "enable decoder low delay mode.\n");
                    s->api_ctx.decoder_low_delay = low_delay;
                    pkt_nal_bitmap |= NI_GENERATE_ALL_NAL_HEADER_BIT;
                    low_delay = 0;
                }
            }

            // check whether the packet contains SEI NAL after VCL units
            if (HEVC_NAL_SEI_PREFIX == nalu_type || HEVC_NAL_SEI_SUFFIX == nalu_type)
            {
                sei_type = *(ptr + 1);
                if (vcl_found || (s->custom_sei_type == sei_type || p_param->dec_input_params.custom_sei_passthru == sei_type))
                {
                    // SEI after VCL found or SEI type indicated then extract the SEI unit;
                    ret = ff_xcoder_extract_custom_sei(avctx, pkt, ptr + 2 - pkt->data, p_packet, sei_type, vcl_found);
                    if (ret != 0)
                    {
                        return ret;
                    }
                }
            }
            else if ((nalu_type >= HEVC_NAL_TRAIL_N) && (nalu_type <= HEVC_NAL_RSV_VCL31))
            {
              vcl_found = 1;
            }
        } 
        else
        {
            av_log(avctx, AV_LOG_DEBUG, "%s() wrong codec %d !\n",
                   __FUNCTION__, avctx->codec_id);
            pkt_sei_alone = 0;
            break;
        }
    }

    if (nalu_count > 0)
    {
      s->is_lone_sei_pkt = pkt_sei_alone;
    }

    return 0;
}

int ff_xcoder_dec_send(AVCodecContext *avctx, XCoderH264DecContext *s, AVPacket *pkt)
{
  /* call ni_decoder_session_write to send compressed video packet to the decoder
     instance */
  int need_draining = 0;
  size_t size;
  ni_packet_t *xpkt = &(s->api_pkt.data.packet);
  int ret;
  int sent;
  int send_size = 0;
  int new_packet = 0;
  int extra_prev_size = 0;

  size = pkt->size;

  if (s->flushing)
  {
    av_log(avctx, AV_LOG_ERROR, "Decoder is flushing and cannot accept new "
                                "buffer until all output buffers have been released\n");
    return AVERROR_EXTERNAL;
  }

  if (pkt->size == 0)
  {
    need_draining = 1;
  }

  if (s->draining && s->eos)
  {
    av_log(avctx, AV_LOG_VERBOSE, "Decoder is draining, eos\n");
    return AVERROR_EOF;
  }

  if (xpkt->data_len == 0)
  {
#if (LIBAVCODEC_VERSION_MAJOR >= 59 || LIBAVCODEC_VERSION_MAJOR >= 58 && LIBAVCODEC_VERSION_MINOR >= 91)
    AVBSFContext *bsf = avctx->internal->bsf;
#else
    AVBSFContext *bsf = avctx->internal->filter.bsfs[0];
#endif
    uint8_t *extradata = bsf ? bsf->par_out->extradata : avctx->extradata;
    int extradata_size = bsf ? bsf->par_out->extradata_size : avctx->extradata_size;

    memset(xpkt, 0, sizeof(ni_packet_t));
    xpkt->pts = pkt->pts;
    xpkt->dts = pkt->dts;
    xpkt->flags        = pkt->flags;
    xpkt->video_width = avctx->width;
    xpkt->video_height = avctx->height;
    xpkt->p_data = NULL;
    xpkt->data_len = pkt->size;

    if (pkt->flags & AV_PKT_FLAG_KEY && extradata_size > 0 &&
        ff_xcoder_add_headers(avctx, pkt, extradata, extradata_size)) {
        if (extradata_size > s->api_ctx.max_nvme_io_size * 2) {
            av_log(avctx, AV_LOG_ERROR,
                   "ff_xcoder_dec_send extradata_size %d "
                   "exceeding max size supported: %d\n",
                   extradata_size, s->api_ctx.max_nvme_io_size * 2);
        } else {
            av_log(avctx, AV_LOG_VERBOSE,
                   "ff_xcoder_dec_send extradata_size %d "
                   "copied to pkt start.\n",
                   s->extradata_size);

            s->api_ctx.prev_size = s->extradata_size;
            memcpy(s->api_ctx.p_leftover, s->extradata, s->extradata_size);
        }
    }

    // If there was lone custom sei in the last packet and the firmware would
    // fail to recoginze it. So passthrough the custom sei here.
    if (s->lone_sei_pkt.size > 0)
    {
      // No need to check the return value here because the lone_sei_pkt was
      // parsed before. Here it is only to extract the SEI data.
      xcoder_packet_parse(avctx, s, &s->lone_sei_pkt, xpkt);
    }

    ret = xcoder_packet_parse(avctx, s, pkt, xpkt);
    if (ret != 0)
    {
      goto fail;
    }

    // If the current packet is a lone SEI, save it to be sent with the next
    // packet. And also check if getting the first packet containing key frame
    // in decoder low delay mode.
    if (s->is_lone_sei_pkt)
    {
        av_packet_ref(&s->lone_sei_pkt, pkt);
        xpkt->data_len = 0;
        free(xpkt->p_custom_sei_set);
        xpkt->p_custom_sei_set = NULL;
        if (s->low_delay && s->got_first_key_frame &&
            !(s->pkt_nal_bitmap & NI_GENERATE_ALL_NAL_HEADER_BIT)) {
            // Packets before the IDR is sent cannot be decoded. So
            // set packet num to zero here.
            s->api_ctx.decoder_low_delay = s->low_delay;
            s->api_ctx.pkt_num = 0;
            s->pkt_nal_bitmap |= NI_GENERATE_ALL_NAL_HEADER_BIT;
            av_log(avctx, AV_LOG_TRACE,
                   "ff_xcoder_dec_send got first IDR in decoder low delay "
                   "mode, "
                   "delay time %dms, pkt_nal_bitmap %d\n",
                   s->low_delay, s->pkt_nal_bitmap);
        }
        av_log(avctx, AV_LOG_TRACE, "ff_xcoder_dec_send pkt lone SEI, saved, "
               "and return %d\n", pkt->size);
        return pkt->size;
    }

    // Send the previous saved lone SEI packet to the decoder
    if (s->lone_sei_pkt.size > 0)
    {
        av_log(avctx, AV_LOG_TRACE, "ff_xcoder_dec_send copy over lone SEI "
               "data size: %d\n", s->lone_sei_pkt.size);
        memcpy(s->api_ctx.p_leftover + s->api_ctx.prev_size,
               s->lone_sei_pkt.data, s->lone_sei_pkt.size);
        s->api_ctx.prev_size += s->lone_sei_pkt.size;
        av_packet_unref(&s->lone_sei_pkt);
    }

    if (pkt->size + s->api_ctx.prev_size > 0)
    {
      ni_packet_buffer_alloc(xpkt, (pkt->size + s->api_ctx.prev_size));
      if (!xpkt->p_data)
      {
        ret = AVERROR(ENOMEM);
        goto fail;
      }
    }
    new_packet = 1;
  }
  else
  {
    send_size = xpkt->data_len;
  }

  av_log(avctx, AV_LOG_VERBOSE, "ff_xcoder_dec_send: pkt->size=%d\n", pkt->size);

  if (s->started == 0)
  {
    xpkt->start_of_stream = 1;
    s->started = 1;
  }

  if (need_draining && !s->draining)
  {
    av_log(avctx, AV_LOG_VERBOSE, "Sending End Of Stream signal\n");
    xpkt->end_of_stream = 1;
    xpkt->data_len = 0;

    av_log(avctx, AV_LOG_TRACE, "ni_packet_copy before: size=%d, s->prev_size=%d, send_size=%d (end of stream)\n", pkt->size, s->api_ctx.prev_size, send_size);
    if (new_packet)
    {
      extra_prev_size = s->api_ctx.prev_size;
      send_size = ni_packet_copy(xpkt->p_data, pkt->data, pkt->size, s->api_ctx.p_leftover, &s->api_ctx.prev_size);
      // increment offset of data sent to decoder and save it
      xpkt->pos = (long long)s->offset;
      s->offset += pkt->size + extra_prev_size;
    }
    av_log(avctx, AV_LOG_TRACE, "ni_packet_copy after: size=%d, s->prev_size=%d, send_size=%d, xpkt->data_len=%d (end of stream)\n", pkt->size, s->api_ctx.prev_size, send_size, xpkt->data_len);

    if (send_size < 0)
    {
      av_log(avctx, AV_LOG_ERROR, "Failed to copy pkt (status = "
                                  "%d)\n",
             send_size);
      ret = AVERROR_EXTERNAL;
      goto fail;
    }
    xpkt->data_len += extra_prev_size;

    sent = 0;
    if (xpkt->data_len > 0)
    {
      sent = ni_device_session_write(&(s->api_ctx), &(s->api_pkt), NI_DEVICE_TYPE_DECODER);
    }
    if (sent < 0)
    {
      av_log(avctx, AV_LOG_ERROR, "Failed to send eos signal (status = %d)\n",
             sent);
      if (NI_RETCODE_ERROR_VPU_RECOVERY == sent)
      {
        ret = xcoder_decode_reset(avctx);
        if (0 == ret)
        {
          ret = AVERROR(EAGAIN);
        }
      }
      else
      {
        ret = AVERROR(EIO);
      }
      goto fail;
    }
    av_log(avctx, AV_LOG_VERBOSE, "Queued eos (status = %d) ts=%llu\n",
           sent, xpkt->pts);
    s->draining = 1;

    ni_device_session_flush(&(s->api_ctx), NI_DEVICE_TYPE_DECODER);
  }
  else
  {
    av_log(avctx, AV_LOG_TRACE, "ni_packet_copy before: size=%d, s->prev_size=%d, send_size=%d\n", pkt->size, s->api_ctx.prev_size, send_size);
    if (new_packet)
    {
      extra_prev_size = s->api_ctx.prev_size;
      send_size = ni_packet_copy(xpkt->p_data, pkt->data, pkt->size, s->api_ctx.p_leftover, &s->api_ctx.prev_size);
      // increment offset of data sent to decoder and save it
      xpkt->pos = (long long)s->offset;
      s->offset += pkt->size + extra_prev_size;
    }
    av_log(avctx, AV_LOG_TRACE, "ni_packet_copy after: size=%d, s->prev_size=%d, send_size=%d, xpkt->data_len=%d\n", pkt->size, s->api_ctx.prev_size, send_size, xpkt->data_len);

    if (send_size < 0)
    {
      av_log(avctx, AV_LOG_ERROR, "Failed to copy pkt (status = "
                                  "%d)\n",
             send_size);
      ret = AVERROR_EXTERNAL;
      goto fail;
    }
    xpkt->data_len += extra_prev_size;

    sent = 0;
    if (xpkt->data_len > 0)
    {
      sent = ni_device_session_write(&s->api_ctx, &(s->api_pkt), NI_DEVICE_TYPE_DECODER);
      av_log(avctx, AV_LOG_VERBOSE, "ff_xcoder_dec_send pts=%" PRIi64 ", dts=%" PRIi64 ", pos=%" PRIi64 ", sent=%d\n", pkt->pts, pkt->dts, pkt->pos, sent);
    }
    if (sent < 0)
    {
      av_log(avctx, AV_LOG_ERROR, "Failed to send compressed pkt (status = "
                                  "%d)\n",
             sent);
      if (NI_RETCODE_ERROR_VPU_RECOVERY == sent)
      {
        ret = xcoder_decode_reset(avctx);
        if (0 == ret)
        {
          ret = AVERROR(EAGAIN);
        }
      }
      else
      {
        ret = AVERROR(EIO);
      }
      goto fail;
    }
    else if (sent == 0)
    {
      av_log(avctx, AV_LOG_VERBOSE, "Queued input buffer size=0\n");
    }
    else if (sent < size)
    { /* partial sent; keep trying */
      av_log(avctx, AV_LOG_VERBOSE, "Queued input buffer size=%d\n", sent);
    }
  }

  if (xpkt->data_len == 0)
  {
    /* if this packet is done sending, free any sei buffer. */
    free(xpkt->p_custom_sei_set);
    xpkt->p_custom_sei_set = NULL;
  }

  if (sent != 0)
  {
    //keep the current pkt to resend next time
    ni_packet_buffer_free(xpkt);
    return sent;
  }
  else
  {
    return AVERROR(EAGAIN);
  }
  

fail:
  ni_packet_buffer_free(xpkt);
  free(xpkt->p_custom_sei_set);
  xpkt->p_custom_sei_set = NULL;
  s->draining = 1;
  s->eos = 1;

  return ret;
}

int retrieve_frame(AVCodecContext *avctx, AVFrame *data, int *got_frame,
                   ni_frame_t *xfme)
{
  XCoderH264DecContext *s = avctx->priv_data;
  ni_xcoder_params_t *p_param =
      &s->api_param; // dec params in union with enc params struct
  int num_extra_outputs = (p_param->dec_input_params.enable_out1 > 0) + (p_param->dec_input_params.enable_out2 > 0);
  uint32_t buf_size = xfme->data_len[0] + xfme->data_len[1] +
                      xfme->data_len[2] + xfme->data_len[3];
  uint8_t *buf = xfme->p_data[0];
  uint8_t *buf1, *buf2;
  bool is_hw;
  int frame_planar;
  int stride = 0;
  int res = 0;
  AVHWFramesContext *ctx   = NULL;
  NIFramesContext *dst_ctx = NULL;
  AVFrame *frame = data;
  ni_aux_data_t *aux_data       = NULL;
  AVFrameSideData *av_side_data = NULL;
  ni_session_data_io_t session_io_data1;
  ni_session_data_io_t session_io_data2;
  ni_session_data_io_t * p_session_data1 = &session_io_data1;
  ni_session_data_io_t * p_session_data2 = &session_io_data2;
  niFrameSurface1_t* p_data3;
  niFrameSurface1_t* p_data3_1;
  niFrameSurface1_t* p_data3_2;

  av_log(avctx, AV_LOG_TRACE,
         "retrieve_frame: buf %p data_len [%d %d %d %d] buf_size %u\n", buf,
         xfme->data_len[0], xfme->data_len[1], xfme->data_len[2],
         xfme->data_len[3], buf_size);

  memset(p_session_data1, 0, sizeof(ni_session_data_io_t));
  memset(p_session_data2, 0, sizeof(ni_session_data_io_t));

  switch (avctx->sw_pix_fmt) {
  case AV_PIX_FMT_NV12:
  case AV_PIX_FMT_P010LE:
      frame_planar = NI_PIXEL_PLANAR_FORMAT_SEMIPLANAR;
      break;
  case AV_PIX_FMT_NI_QUAD_8_TILE_4X4:
  case AV_PIX_FMT_NI_QUAD_10_TILE_4X4:
      frame_planar = NI_PIXEL_PLANAR_FORMAT_TILED4X4;
      break;
  default:
      frame_planar = NI_PIXEL_PLANAR_FORMAT_PLANAR;
      break;
  }

  if (num_extra_outputs)
  {
      ni_frame_buffer_alloc(&(p_session_data1->data.frame), 1,
                            1, // width height does not matter//codec id does
                               // not matter//no metadata
                            1, 0, 1, 1, frame_planar);
      buf1 = p_session_data1->data.frame.p_data[0];
      if (num_extra_outputs > 1) {
          ni_frame_buffer_alloc(&(p_session_data2->data.frame), 1,
                                1, // width height does not matter
                                1, 0, 1, 1, frame_planar);
          buf2 = p_session_data2->data.frame.p_data[0];
    }
  }

  is_hw = xfme->data_len[3] > 0;
  if (is_hw)
  {
    if (frame->hw_frames_ctx) 
    {
      ctx = (AVHWFramesContext*)frame->hw_frames_ctx->data;
      dst_ctx = ctx->internal->priv;
    }
    if (s->api_ctx.frame_num == 1)
    {
      if (frame->hw_frames_ctx) 
      {
        av_log(avctx, AV_LOG_ERROR, "First frame, set hw_frame_context to copy decode sessions threads\n");
        res = ni_device_session_copy(&s->api_ctx, &dst_ctx->api_ctx);
        if (NI_RETCODE_SUCCESS != res)
        {
          return res;
        }
        av_log(avctx, AV_LOG_VERBOSE, "retrieve_frame: blk_io_handle %d device_handle %d\n", s->api_ctx.blk_io_handle, s->api_ctx.device_handle);
      }
    }
  }

  av_log(avctx, AV_LOG_VERBOSE, "decoding %" PRId64 " frame ...\n", s->api_ctx.frame_num);

  if (avctx->width <= 0)
  {
    av_log(avctx, AV_LOG_ERROR, "width is not set\n");
    return AVERROR_INVALIDDATA;
  }
  if (avctx->height <= 0)
  {
    av_log(avctx, AV_LOG_ERROR, "height is not set\n");
    return AVERROR_INVALIDDATA;
  }

  stride = s->api_ctx.active_video_width;

  av_log(avctx, AV_LOG_VERBOSE, "XFRAME SIZE: %d, STRIDE: %d\n", buf_size, stride);

  if (!is_hw && (stride == 0 || buf_size < stride * avctx->height))
  {
    av_log(avctx, AV_LOG_ERROR, "Packet too small (%d)\n", buf_size);
    return AVERROR_INVALIDDATA;
  }

  frame->key_frame = 0;
  if(xfme->ni_pict_type & 0x10) //key frame marker
  	frame->key_frame = 1;
  switch (xfme->ni_pict_type & 0xF)
  {
  case DECODER_PIC_TYPE_IDR:
      frame->key_frame = 1;
  case PIC_TYPE_I:
      frame->pict_type = AV_PICTURE_TYPE_I;
      break;
  case PIC_TYPE_P:
      frame->pict_type = AV_PICTURE_TYPE_P;
      break;
  case PIC_TYPE_B:
      frame->pict_type = AV_PICTURE_TYPE_B;
      break;
  default:
      frame->pict_type = AV_PICTURE_TYPE_NONE;
  }

  res = ff_decode_frame_props(avctx, frame);
  if (res < 0)
    return res;

  frame->pkt_pos = avctx->internal->last_pkt_props->pos;
  frame->pkt_duration = avctx->internal->last_pkt_props->duration;

  if ((res = av_image_check_size(xfme->video_width, xfme->video_height, 0, avctx)) < 0)
    return res;

  if (is_hw)
  {
    frame->buf[0] = av_buffer_create(buf, buf_size, ni_frame_free, NULL, 0);
    if (num_extra_outputs)
    {
        frame->buf[1] =
            av_buffer_create(buf1, (int)(buf_size / 3), ni_frame_free, NULL, 0);
        buf1 = frame->buf[1]->data;
        memcpy(buf1, buf + sizeof(niFrameSurface1_t),
               sizeof(niFrameSurface1_t)); // copy hwdesc to new buffer
        if (num_extra_outputs > 1) {
            frame->buf[2] = av_buffer_create(buf2, (int)(buf_size / 3),
                                             ni_frame_free, NULL, 0);
            buf2          = frame->buf[2]->data;
            memcpy(buf2, buf + 2 * sizeof(niFrameSurface1_t),
                   sizeof(niFrameSurface1_t));
        }
    }
  }
  else
  {
    frame->buf[0] = av_buffer_create(buf, buf_size, ni_align_free, xfme->dec_buf, 0);
  }
  av_log(avctx, AV_LOG_TRACE,
         "retrieve_frame: is_hw %d frame->buf[0] %p buf %p buf_size %u "
         "num_extra_outputs %d pkt_duration %ld\n",
         is_hw, frame->buf[0], buf, buf_size, num_extra_outputs,
         frame->pkt_duration);

  buf = frame->buf[0]->data;

#ifdef NI_DEC_GSTREAMER_SUPPORT
  int i;
  // retrieve the GStreamer data based on frame's packet offset
  if (avctx->codec_id == AV_CODEC_ID_MJPEG) {
      // The FW would not return frame_pkt_offset when codec is JPEG
      // And there would never have B frames so we can use frame index to
      // locate the real pkt index.
      i                    = (s->api_ctx.frame_num - 1) % NI_FIFO_SZ;
      frame->opaque        = s->gs_data[i].opaque;
      frame->buf[3]        = s->gs_data[i].buf0;
      s->gs_data[i].opaque = NULL;
      s->gs_data[i].buf0   = NULL;
  }else {
      if (0 == s->api_ctx.frame_pkt_offset) {
          frame->opaque        = s->gs_data[0].opaque;
          frame->buf[3]        = s->gs_data[0].buf0;
          s->gs_data[0].opaque = NULL;
          s->gs_data[0].buf0   = NULL;

          av_log(avctx, AV_LOG_DEBUG, "pos 0 pkt opaque %p buf0 %p retrieved\n",
                 frame->opaque, frame->buf[1]);
      } else {
          for (i = 0; i < NI_FIFO_SZ; i++) {
              if (s->api_ctx.frame_pkt_offset >=
                      s->gs_opaque_offsets_index_min[i] &&
                  s->api_ctx.frame_pkt_offset < s->gs_opaque_offsets_index[i]) {
                  frame->opaque        = s->gs_data[i].opaque;
                  frame->buf[3]        = s->gs_data[i].buf0;
                  s->gs_data[i].opaque = NULL;
                  s->gs_data[i].buf0   = NULL;

                  av_log(avctx, AV_LOG_DEBUG,
                         "pos %d pkt opaque %p buf0 %p retrieved\n", i,
                         frame->opaque, frame->buf[1]);
                  break;
              }
              if (i == NI_FIFO_SZ - 1) {
                  av_log(avctx, AV_LOG_ERROR,
                         "ERROR: NO GS opaque found, consider "
                         "increasing NI_FIFO_SZ (%d)!\n",
                         NI_FIFO_SZ);
              }
          }
      }
  }
#endif

  // retrieve side data if available
  ni_dec_retrieve_aux_data(xfme);

  // update avctx framerate with timing info
  if (xfme->vui_time_scale && xfme->vui_num_units_in_tick) {
      if (AV_CODEC_ID_H264 == avctx->codec_id) {
          av_reduce(&avctx->framerate.den, &avctx->framerate.num,
                    xfme->vui_num_units_in_tick * avctx->ticks_per_frame,
                    xfme->vui_time_scale, 1 << 30);
      } else if (AV_CODEC_ID_H265 == avctx->codec_id) {
          av_reduce(&avctx->framerate.den, &avctx->framerate.num,
                    xfme->vui_num_units_in_tick, xfme->vui_time_scale, 1 << 30);
      }
  }

  // User Data Unregistered SEI if available
  av_log(avctx, AV_LOG_VERBOSE, "#SEI# UDU (offset=%u len=%u)\n",
         xfme->sei_user_data_unreg_offset, xfme->sei_user_data_unreg_len);
  if (xfme->sei_user_data_unreg_offset)
  {
      if ((aux_data = ni_frame_get_aux_data(xfme, NI_FRAME_AUX_DATA_UDU_SEI))) {
          av_side_data = av_frame_new_side_data(
              frame, AV_FRAME_DATA_NETINT_UDU_SEI, aux_data->size);

          if (!av_side_data) {
              return AVERROR(ENOMEM);
          } else {
              memcpy(av_side_data->data, aux_data->data, aux_data->size);
          }
          av_log(avctx, AV_LOG_VERBOSE, "UDU SEI added (len=%d type=5)\n",
                 xfme->sei_user_data_unreg_len);
      } else {
          av_log(avctx, AV_LOG_ERROR, "UDU SEI dropped! (len=%d type=5)\n",
                 xfme->sei_user_data_unreg_len);
      }
  }

  // close caption data if available
  av_log(avctx, AV_LOG_VERBOSE, "#SEI# CC (offset=%u len=%u)\n",
         xfme->sei_cc_offset, xfme->sei_cc_len);
  if ((aux_data = ni_frame_get_aux_data(xfme, NI_FRAME_AUX_DATA_A53_CC))) {
      av_side_data =
          av_frame_new_side_data(frame, AV_FRAME_DATA_A53_CC, aux_data->size);

      if (!av_side_data) {
          return AVERROR(ENOMEM);
      } else {
          memcpy(av_side_data->data, aux_data->data, aux_data->size);
      }
  }

  // hdr10 sei data if available
  av_log(avctx, AV_LOG_VERBOSE, "#SEI# MDCV (offset=%u len=%u)\n",
         xfme->sei_hdr_mastering_display_color_vol_offset,
         xfme->sei_hdr_mastering_display_color_vol_len);
  if ((aux_data = ni_frame_get_aux_data(
           xfme, NI_FRAME_AUX_DATA_MASTERING_DISPLAY_METADATA))) {
      AVMasteringDisplayMetadata *mdm =
          av_mastering_display_metadata_create_side_data(frame);
      if (!mdm) {
          return AVERROR(ENOMEM);
      } else {
          memcpy(mdm, aux_data->data, aux_data->size);
      }
  }

  av_log(avctx, AV_LOG_VERBOSE, "#SEI# CLL (offset=%u len=%u)\n",
         xfme->sei_hdr_content_light_level_info_offset,
         xfme->sei_hdr_content_light_level_info_len);
  if ((aux_data = ni_frame_get_aux_data(
           xfme, NI_FRAME_AUX_DATA_CONTENT_LIGHT_LEVEL))) {
      AVContentLightMetadata *clm =
          av_content_light_metadata_create_side_data(frame);
      if (!clm) {
          return AVERROR(ENOMEM);
      } else {
          memcpy(clm, aux_data->data, aux_data->size);
      }
  }

  // hdr10+ sei data if available
  av_log(avctx, AV_LOG_VERBOSE, "#SEI# HDR10+ (offset=%u len=%u)\n",
         xfme->sei_hdr_plus_offset, xfme->sei_hdr_plus_len);
  if ((aux_data = ni_frame_get_aux_data(xfme, NI_FRAME_AUX_DATA_HDR_PLUS))) {
      AVDynamicHDRPlus *hdrp = av_dynamic_hdr_plus_create_side_data(frame);

      if (!hdrp) {
          return AVERROR(ENOMEM);
      } else {
          memcpy(hdrp, aux_data->data, aux_data->size);
      }
  } // hdr10+ sei

  // remember to clean up auxiliary data of ni_frame after their use
  ni_frame_wipe_aux_data(xfme);

  if (xfme->p_custom_sei_set)
  {
    AVBufferRef *sei_ref = av_buffer_create((uint8_t *)xfme->p_custom_sei_set,
                                            sizeof(ni_custom_sei_set_t),
                                            ni_free, NULL, 0);
    if (! sei_ref ||
        ! av_frame_new_side_data_from_buf(frame, AV_FRAME_DATA_NETINT_CUSTOM_SEI,
                                    sei_ref))
    {
        return AVERROR(ENOMEM);
    }
    xfme->p_custom_sei_set = NULL;
  }

  frame->pkt_dts = xfme->dts;
  frame->pts     = xfme->pts;
  if (xfme->pts != NI_NOPTS_VALUE)
  {
      s->current_pts = frame->pts;
  }
  else
  {
      if (!s->api_ctx.ready_to_close) {
          s->current_pts += frame->pkt_duration;
          frame->pts = s->current_pts;
      }
  }

  if (is_hw)
  {
    p_data3 = (niFrameSurface1_t*)(xfme->p_buffer + xfme->data_len[0] + xfme->data_len[1] + xfme->data_len[2]);
    frame->data[3] = xfme->p_buffer + xfme->data_len[0] + xfme->data_len[1] + xfme->data_len[2];

    av_log(avctx, AV_LOG_DEBUG, "retrieve_frame: OUT0 data[3] trace ui16FrameIdx = [%d], device_handle=%d bitdep=%d, WxH %d x %d\n",
           p_data3->ui16FrameIdx,
           p_data3->device_handle,
           p_data3->bit_depth,
           p_data3->ui16width,
           p_data3->ui16height);
    
    if (num_extra_outputs)
    {
      p_data3_1 = (niFrameSurface1_t*)buf1;
      //p_data3_1->bit_depth = (p_data3_1->bit_depth == 1) ? 1 : (int8_t)s->api_ctx.bit_depth_factor;
      //p_data3_1->no_crop = 1;
      av_log(avctx, AV_LOG_DEBUG, "retrieve_frame: OUT1 data[3] trace ui16FrameIdx = [%d], device_handle=%d bitdep=%d, WxH %d x %d\n",
             p_data3_1->ui16FrameIdx,
             p_data3_1->device_handle,
             p_data3_1->bit_depth,
             p_data3_1->ui16width,
             p_data3_1->ui16height);
      if (num_extra_outputs > 1)
      {
        p_data3_2 = (niFrameSurface1_t*)buf2;
        //p_data3_2->bit_depth = (p_data3_2->bit_depth == 1) ? 1 : (int8_t)s->api_ctx.bit_depth_factor;
        //p_data3_2->no_crop = 1;
        av_log(avctx, AV_LOG_DEBUG, "retrieve_frame: OUT2 data[3] trace ui16FrameIdx = [%d], device_handle=%d bitdep=%d, WxH %d x %d\n",
               p_data3_2->ui16FrameIdx,
               p_data3_2->device_handle,
               p_data3_2->bit_depth,
               p_data3_2->ui16width,
               p_data3_2->ui16height);
      }
    }
  }
  av_log(avctx, AV_LOG_VERBOSE, "retrieve_frame: frame->buf[0]=%p, frame->data=%p, frame->pts=%" PRId64 ", frame size=%d, s->current_pts=%" PRId64 ", frame->pkt_pos=%" PRId64 ", frame->pkt_duration=%" PRId64 " sei size %d offset %u\n",
         frame->buf[0], frame->data, frame->pts,
         buf_size, s->current_pts, frame->pkt_pos,
         frame->pkt_duration, xfme->sei_cc_len, xfme->sei_cc_offset);

  /* av_buffer_ref(avpkt->buf); */
  if (!frame->buf[0])
    return AVERROR(ENOMEM);

  if (!is_hw &&
      ((res = av_image_fill_arrays(
            frame->data, frame->linesize, buf, avctx->sw_pix_fmt,
            (int)(s->api_ctx.active_video_width / s->api_ctx.bit_depth_factor),
            s->api_ctx.active_video_height, 1)) < 0)) {
      av_buffer_unref(&frame->buf[0]);
      return res;
  }

  av_log(avctx, AV_LOG_VERBOSE, "retrieve_frame: success av_image_fill_arrays "
         "return %d\n", res);
  if (QUADRA)
  {
    if (!is_hw)
    {
      //frame->linesize[0] = (((frame->width * s->api_ctx.bit_depth_factor) + 63) / 64) * 64;
      //frame->linesize[1] = frame->linesize[2] = (((frame->width / 2 * s->api_ctx.bit_depth_factor) + 63) / 64) * 64;
      frame->linesize[1] = frame->linesize[2] = (((frame->width / ((frame_planar == 0) ? 1 : 2) * s->api_ctx.bit_depth_factor) + 127) / 128) * 128;
      frame->linesize[2] = (frame_planar == 0) ? 0 : frame->linesize[1];
      //frame->data[1] = frame->data[0] + (frame->linesize[0] * frame->height);
      frame->data[2] = (frame_planar == 0) ? 0 : frame->data[1] + (frame->linesize[1] * frame->height / 2);
    }
  }
  else
  {
    frame->width = s->api_ctx.active_video_width;
    frame->height = s->api_ctx.active_video_height;
  }  

  frame->crop_top = xfme->crop_top;
  if (QUADRA)
  {
    frame->crop_bottom = frame->height - xfme->crop_bottom; // ppu auto crop should have cropped out padding, crop_bottom should be 0
  }
  else
  {
    frame->crop_bottom = s->api_ctx.active_video_height - xfme->crop_bottom;  
  }
  frame->crop_left = xfme->crop_left;
  if (QUADRA)
  {
    frame->crop_right = frame->width - xfme->crop_right; // ppu auto crop should have cropped out padding, crop_right should be 0
  }
  else
  {
    frame->crop_right = s->api_ctx.active_video_width - xfme->crop_right;
  }

#if 0
  frame->crop_top =    0;//PPu autocrops out padding //xfme->crop_top;
  frame->crop_bottom = 0;//PPu autocrops out padding //s->api_ctx.active_video_height - xfme->crop_bottom;
  frame->crop_left =   0;//PPu autocrops out padding //xfme->crop_left;
  frame->crop_right =  0;//PPu autocrops out padding //s->api_ctx.active_video_width - xfme->crop_right;
#endif

#if 0
  if(p_param->dec_input_params.crop_mode[0] != NI_DEC_CROP_MODE_DISABLE && p_param->dec_input_params.scale_wh[0][0])
  {
    frame->crop_top = frame->crop_left = frame->crop_right = frame->crop_bottom = 0; //Quadra has already cropped
  }
#endif

  if (is_hw && frame->hw_frames_ctx && dst_ctx != NULL) {
      av_log(avctx, AV_LOG_TRACE,
             "retrieve_frame: hw_frames_ctx av_buffer_get_ref_count=%d\n",
             av_buffer_get_ref_count(frame->hw_frames_ctx));
      // dst_ctx->pc_height = frame->height;
      // dst_ctx->pc_crop_bottom = frame->crop_bottom;
      // dst_ctx->pc_width = frame->width;
      // dst_ctx->pc_crop_right = frame->crop_right;
      dst_ctx->split_ctx.enabled = (num_extra_outputs >= 1) ? 1 : 0;
      dst_ctx->split_ctx.w[0]    = p_data3->ui16width;
      dst_ctx->split_ctx.h[0]    = p_data3->ui16height;
      dst_ctx->split_ctx.f[0]    = (int)p_data3->encoding_type;
      dst_ctx->split_ctx.f8b[0]  = (int)p_data3->bit_depth;
      dst_ctx->split_ctx.w[1] =
          (num_extra_outputs >= 1) ? p_data3_1->ui16width : 0;
      dst_ctx->split_ctx.h[1] =
          (num_extra_outputs >= 1) ? p_data3_1->ui16height : 0;
      dst_ctx->split_ctx.f[1] =
          (num_extra_outputs >= 1) ? p_data3_1->encoding_type : 0;
      dst_ctx->split_ctx.f8b[1] =
          (num_extra_outputs >= 1) ? p_data3_1->bit_depth : 0;
      dst_ctx->split_ctx.w[2] =
          (num_extra_outputs == 2) ? p_data3_2->ui16width : 0;
      dst_ctx->split_ctx.h[2] =
          (num_extra_outputs == 2) ? p_data3_2->ui16height : 0;
      dst_ctx->split_ctx.f[2] =
          (num_extra_outputs == 2) ? p_data3_2->encoding_type : 0;
      dst_ctx->split_ctx.f8b[2] =
          (num_extra_outputs == 2) ? p_data3_2->bit_depth : 0;
#if 0
    for (i = 0; i < num_extra_outputs + 1; i++)
    {
      bool use_out_2 = (p_param->dec_input_params.enable_out1 == 0 && i == 1); //in case out0 enabled, out1 disabled out2 enabled
      if (p_param->dec_input_params.crop_mode[i + use_out_2] == NI_DEC_CROP_MODE_DISABLE && 
          p_param->dec_input_params.scale_wh[i + use_out_2][0] == 0)
      {//crop info from decoded headers
        dst_ctx->split_ctx.crop_meta_data_rltb[i][0] = frame->crop_right;
        dst_ctx->split_ctx.crop_meta_data_rltb[i][1] = frame->crop_left;
        dst_ctx->split_ctx.crop_meta_data_rltb[i][2] = frame->crop_top;
        dst_ctx->split_ctx.crop_meta_data_rltb[i][3] = frame->crop_bottom; 
      }
      else
      {//crop has been applied, no offsets, just the window
        dst_ctx->split_ctx.crop_meta_data_rltb[i][0] = 0;
        dst_ctx->split_ctx.crop_meta_data_rltb[i][1] = 0;
        dst_ctx->split_ctx.crop_meta_data_rltb[i][2] = 0;
        dst_ctx->split_ctx.crop_meta_data_rltb[i][3] = 0;
      }
    }
#endif
  }
  *got_frame = 1;
  return buf_size;
}

int ff_xcoder_dec_receive(AVCodecContext *avctx, XCoderH264DecContext *s,
                          AVFrame *frame, bool wait)
{
  /* call xcode_dec_receive to get a decoded YUV frame from the decoder
     instance */
  int ret = 0;
  int got_frame = 0;
  ni_session_data_io_t session_io_data;
  ni_session_data_io_t * p_session_data = &session_io_data;
  int alloc_mem, height, actual_width, cropped_width, cropped_height;
  bool bSequenceChange = 0;
  int frame_planar;

  if (s->draining && s->eos)
  {
    return AVERROR_EOF;
  }
  // TODO: 
  // (HW_FRAME_EXTRA_BUFFER-1) where FW defines HW_FRAME_EXTRA_BUFFER 3
  // may need to pass it as a param to FW
  memset(p_session_data, 0, sizeof(ni_session_data_io_t));

#ifndef NI_DEC_GSTREAMER_SUPPORT
  // Disable burst control in Gstreamer.
  // Burst control retrun EAGAIN during draining decoder, this interrupt drain process
  if (s->api_ctx.frame_num % 2 == 0)
  {
    s->api_ctx.burst_control = (s->api_ctx.burst_control == 0 ? 1 : 0); //toggle
  }
  if (s->api_ctx.burst_control)
  {
    av_log(avctx, AV_LOG_DEBUG, "ff_xcoder_dec_receive burst return%" PRId64 " frame\n", s->api_ctx.frame_num);
    return AVERROR(EAGAIN);
  }
#endif

  // if active video resolution has been obtained we just use it as it's the
  // exact size of frame to be returned, otherwise we use what we are told by
  // upper stream as the initial setting and it will be adjusted.
  // width = s->api_ctx.active_video_width > 0 ? s->api_ctx.active_video_width :
  // avctx->width;
  height =
      (int)(s->api_ctx.active_video_height > 0 ? s->api_ctx.active_video_height
                                               : avctx->height);
  actual_width =
      (int)(s->api_ctx.actual_video_width > 0 ? s->api_ctx.actual_video_width
                                              : avctx->width);

  // allocate memory only after resolution is known (buffer pool set up)
  alloc_mem = (s->api_ctx.active_video_width > 0 && 
               s->api_ctx.active_video_height > 0 ? 1 : 0);
  switch (avctx->sw_pix_fmt) {
  case AV_PIX_FMT_NV12:
  case AV_PIX_FMT_P010LE:
      frame_planar = NI_PIXEL_PLANAR_FORMAT_SEMIPLANAR;
      break;
  case AV_PIX_FMT_NI_QUAD_8_TILE_4X4:
  case AV_PIX_FMT_NI_QUAD_10_TILE_4X4:
      frame_planar = NI_PIXEL_PLANAR_FORMAT_TILED4X4;
      break;
  default:
      frame_planar = NI_PIXEL_PLANAR_FORMAT_PLANAR;
      break;
  }

  if (avctx->pix_fmt != AV_PIX_FMT_NI_QUAD)
  {
      ret = ni_decoder_frame_buffer_alloc(
          s->api_ctx.dec_fme_buf_pool, &(p_session_data->data.frame), alloc_mem,
          actual_width, height, (avctx->codec_id == AV_CODEC_ID_H264),
          s->api_ctx.bit_depth_factor, frame_planar);
  }
  else
  {
      ret = ni_frame_buffer_alloc(&(p_session_data->data.frame), actual_width,
                                  height, (avctx->codec_id == AV_CODEC_ID_H264),
                                  1, s->api_ctx.bit_depth_factor, 3,
                                  frame_planar);
  }

  if (NI_RETCODE_SUCCESS != ret)
  {
    return AVERROR_EXTERNAL;
  }

  if (avctx->pix_fmt != AV_PIX_FMT_NI_QUAD)
  {
    ret = ni_device_session_read(&s->api_ctx, p_session_data, NI_DEVICE_TYPE_DECODER);
  }
  else
  {
    ret = ni_device_session_read_hwdesc(&s->api_ctx, p_session_data, NI_DEVICE_TYPE_DECODER);
  }

  if (ret == 0)
  {

    s->eos = p_session_data->data.frame.end_of_stream;
    if (avctx->pix_fmt != AV_PIX_FMT_NI_QUAD) {
        ni_decoder_frame_buffer_free(&(p_session_data->data.frame));
    } else {
        ni_frame_buffer_free(&(p_session_data->data.frame));
    }
    return AVERROR(EAGAIN);

  }
  else if (ret > 0)
  {
    int dec_ff_pix_fmt;

    if (p_session_data->data.frame.flags & AV_PKT_FLAG_DISCARD) {
        av_log(avctx, AV_LOG_DEBUG,
               "Current frame is dropped when AV_PKT_FLAG_DISCARD is set\n");
        if (avctx->pix_fmt != AV_PIX_FMT_NI_QUAD) {
            ni_decoder_frame_buffer_free(&(p_session_data->data.frame));
        } else {
            ni_frame_free(NULL, p_session_data->data.frame.p_data[0]); // recycle frame mem bin buffer & free p_buffer
        }
        return AVERROR(EAGAIN);
    }

    av_log(avctx, AV_LOG_VERBOSE, "Got output buffer pts=%lld "
                                  "dts=%lld eos=%d sos=%d\n",
           p_session_data->data.frame.pts, p_session_data->data.frame.dts,
           p_session_data->data.frame.end_of_stream, p_session_data->data.frame.start_of_stream);

    s->eos = p_session_data->data.frame.end_of_stream;

    // update ctxt resolution if change has been detected
    frame->width = cropped_width = p_session_data->data.frame.video_width; // ppu auto crop reports wdith as cropped width
    frame->height = cropped_height = p_session_data->data.frame.video_height; // ppu auto crop reports heigth as cropped height
    //cropped_width = p_session_data->data.frame.crop_right;
    //cropped_height = p_session_data->data.frame.crop_bottom;

    if (cropped_width != avctx->width || cropped_height != avctx->height)

    {
      av_log(avctx, AV_LOG_WARNING, "ff_xcoder_dec_receive: resolution "
             "changed: %dx%d to %dx%d\n", avctx->width, avctx->height,
            cropped_width, cropped_height);
      avctx->width = cropped_width;
      avctx->height = cropped_height;
      bSequenceChange = 1;
    }

    dec_ff_pix_fmt = ni_pix_fmt_2_ff_pix_fmt(s->api_ctx.pixel_format);

    // If the codec is Jpeg or color range detected is a full range,
    // yuv420p from xxx_ni_quadra_dec means a full range.
    // Change it to yuvj420p so that FFmpeg can process it as a full range.
    if ((avctx->pix_fmt != AV_PIX_FMT_NI_QUAD) &&
        (dec_ff_pix_fmt == AV_PIX_FMT_YUV420P) &&
        ((avctx->codec_id == AV_CODEC_ID_MJPEG) ||
         (avctx->color_range == AVCOL_RANGE_JPEG)))
    {
       avctx->sw_pix_fmt = avctx->pix_fmt = dec_ff_pix_fmt = AV_PIX_FMT_YUVJ420P;
       avctx->color_range = AVCOL_RANGE_JPEG;
    }

    if (avctx->sw_pix_fmt != dec_ff_pix_fmt)
    {
      av_log(avctx, AV_LOG_VERBOSE, "update sw_pix_fmt from %d to %d\n",
             avctx->sw_pix_fmt, dec_ff_pix_fmt);
      avctx->sw_pix_fmt = dec_ff_pix_fmt;
      if (avctx->pix_fmt != AV_PIX_FMT_NI_QUAD)
      {
        avctx->pix_fmt = avctx->sw_pix_fmt;
      }
      bSequenceChange = 1;
    }
    
    frame->format = avctx->pix_fmt; 

    av_log(avctx, AV_LOG_VERBOSE, "ff_xcoder_dec_receive: frame->format %d, sw_pix_fmt = %d\n", frame->format, avctx->sw_pix_fmt);

    if (avctx->pix_fmt == AV_PIX_FMT_NI_QUAD)
    {
      if (bSequenceChange)
      {
        AVHWFramesContext *ctx;
        NIFramesContext *dst_ctx;

        av_buffer_unref(&avctx->hw_frames_ctx);
        avctx->hw_frames_ctx = av_hwframe_ctx_alloc(avctx->hw_device_ctx);
        if (!avctx->hw_frames_ctx) 
        {
          ret = AVERROR(ENOMEM);
          return ret;
        }
        
        s->frames = (AVHWFramesContext*)avctx->hw_frames_ctx->data;
        s->frames->format = AV_PIX_FMT_NI_QUAD;
        s->frames->width     = avctx->width;
        s->frames->height     = avctx->height;
        s->frames->sw_format = avctx->sw_pix_fmt;
        s->frames->initial_pool_size = -1; //Decoder has its own dedicated pool
        ret = av_hwframe_ctx_init(avctx->hw_frames_ctx);
        if (ret < 0)
        {
          return ret;
        }
        
        ctx = (AVHWFramesContext*)avctx->hw_frames_ctx->data;
        dst_ctx = ctx->internal->priv;
        av_log(avctx, AV_LOG_ERROR, "ff_xcoder_dec_receive: sequence change, set hw_frame_context to copy decode sessions threads\n");
        ret = ni_device_session_copy(&s->api_ctx, &dst_ctx->api_ctx);
        if (NI_RETCODE_SUCCESS != ret)
        {
          return ret;
        }
      }
      frame->hw_frames_ctx = av_buffer_ref(avctx->hw_frames_ctx);

#ifdef NI_DEC_GSTREAMER_SUPPORT
      /* Set the hw_id/card number */
      AVHWFramesContext *hwframes = (AVHWFramesContext*)avctx->hw_frames_ctx->data;
      AVNIFramesContext *ni_hw_ctx;
      ni_hw_ctx = (AVNIFramesContext *)hwframes->hwctx;
      ni_hw_ctx->dev_dec_idx = s->dev_dec_idx;
#else
      /* Set the hw_id/card number in the opaque field */
      // NOLINTNEXTLINE(clang-diagnostic-int-to-void-pointer-cast)
      frame->opaque = (void *)s->dev_dec_idx;
#endif
    }
    if (s->api_ctx.frame_num == 1)
    {
        av_log(avctx, AV_LOG_DEBUG, "NI:%s:out\n",
               (frame_planar == 0)   ? "semiplanar"
               : (frame_planar == 2) ? "tiled"
                                     : "planar");
    }
    retrieve_frame(avctx, frame, &got_frame, &(p_session_data->data.frame));
    
    av_log(avctx, AV_LOG_VERBOSE, "ff_xcoder_dec_receive: got_frame=%d, frame->width=%d, frame->height=%d, crop top %" SIZE_SPECIFIER " bottom %" SIZE_SPECIFIER " left %" SIZE_SPECIFIER " right %" SIZE_SPECIFIER ", frame->format=%d, frame->linesize=%d/%d/%d\n", got_frame, frame->width, frame->height, frame->crop_top, frame->crop_bottom, frame->crop_left, frame->crop_right, frame->format, frame->linesize[0], frame->linesize[1], frame->linesize[2]);

#if FF_API_PKT_PTS
    FF_DISABLE_DEPRECATION_WARNINGS
    frame->pkt_pts = frame->pts;
    FF_ENABLE_DEPRECATION_WARNINGS
#endif
    frame->best_effort_timestamp = frame->pts;

    av_log(avctx, AV_LOG_VERBOSE, "ff_xcoder_dec_receive: pkt_timebase= %d/%d, frame_rate=%d/%d, frame->pts=%" PRId64 ", frame->pkt_dts=%" PRId64 "\n", avctx->pkt_timebase.num, avctx->pkt_timebase.den, avctx->framerate.num, avctx->framerate.den, frame->pts, frame->pkt_dts);

    // release buffer ownership and let frame owner return frame buffer to 
    // buffer pool later
    p_session_data->data.frame.dec_buf = NULL;

    free(p_session_data->data.frame.p_custom_sei_set);
    p_session_data->data.frame.p_custom_sei_set = NULL;
  }
  else
  {
    av_log(avctx, AV_LOG_ERROR, "Failed to get output buffer (status = %d)\n",
           ret);
    
    if (NI_RETCODE_ERROR_VPU_RECOVERY == ret)
    {
      av_log(avctx, AV_LOG_WARNING, "ff_xcoder_dec_receive VPU recovery, need to reset ..\n");
      ni_decoder_frame_buffer_free(&(p_session_data->data.frame));
      return ret;
    }

    return AVERROR(EIO);
  }

  ret = 0;

  return ret;
}

int ff_xcoder_dec_is_flushing(AVCodecContext *avctx, XCoderH264DecContext *s)
{
  return s->flushing;
}

int ff_xcoder_dec_flush(AVCodecContext *avctx, XCoderH264DecContext *s)
{
  s->draining = 0;
  s->flushing = 0;
  s->eos = 0;

#if 0
  int ret;
  ret = ni_device_session_flush(s, NI_DEVICE_TYPE_DECODER);
  if (ret < 0) {
    av_log(avctx, AV_LOG_ERROR, "Failed to flush decoder (status = %d)\n", ret);
    return AVERROR_EXTERNAL;
  }
#endif

  /* Future: for now, always return 1 to indicate the codec has been flushed
     and it leaves the flushing state and can process again ! will consider
     case of user retaining frames in HW "surface" usage */
  return 1;
}
