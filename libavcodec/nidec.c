/*
 * NetInt XCoder H.264/HEVC Decoder common code
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

/**
 * @file
 * XCoder decoder.
 */

#include "nidec.h"
#include "fftools/ffmpeg.h"
#include "libavutil/hwcontext.h"
#include "libavutil/hwcontext_ni_quad.h"

#define USER_DATA_UNREGISTERED_SEI_PAYLOAD_TYPE 5

int xcoder_decode_close(AVCodecContext *avctx)
{
  XCoderH264DecContext *s = avctx->priv_data;
  av_log(avctx, AV_LOG_VERBOSE, "XCoder decode close\n");

  /* this call shall release resource based on s->api_ctx */
  ff_xcoder_dec_close(avctx, s);

  av_packet_unref(&s->buffered_pkt);
  av_packet_unref(&s->lone_sei_pkt);

  free(s->extradata);
  s->extradata           = NULL;
  s->extradata_size      = 0;
  s->got_first_key_frame = 0;

  ni_rsrc_free_device_context(s->rsrc_ctx);
  s->rsrc_ctx = NULL;

  return 0;
}

static int xcoder_setup_decoder(AVCodecContext *avctx)
{
  XCoderH264DecContext *s = avctx->priv_data;
  ni_xcoder_params_t *p_param =
      &s->api_param; // dec params in union with enc params struct
  int min_resolution_width, min_resolution_height;

  int ret = 0;

  av_log(avctx, AV_LOG_VERBOSE, "XCoder setup device decoder\n");
  //s->api_ctx.session_id = NI_INVALID_SESSION_ID;
  if (ni_device_session_context_init(&(s->api_ctx)) < 0) {
      av_log(avctx, AV_LOG_ERROR,
             "Error XCoder init decoder context failure\n");
      return AVERROR_EXTERNAL;
  }

  min_resolution_width  = NI_MIN_RESOLUTION_WIDTH;
  min_resolution_height = NI_MIN_RESOLUTION_HEIGHT;

  // Check codec id or format as well as profile idc.
  switch (avctx->codec_id) {
    case AV_CODEC_ID_HEVC:
      s->api_ctx.codec_format = NI_CODEC_FORMAT_H265;
      switch (avctx->profile)
      {
        case FF_PROFILE_HEVC_MAIN:
        case FF_PROFILE_HEVC_MAIN_10:
        case FF_PROFILE_HEVC_MAIN_STILL_PICTURE:
        case FF_PROFILE_UNKNOWN:
          break;
        default:
          av_log(avctx, AV_LOG_ERROR, "Error: profile %d not supported.\n", avctx->profile);
          return AVERROR_INVALIDDATA;
      }
      break;
    case AV_CODEC_ID_VP9:
      s->api_ctx.codec_format = NI_CODEC_FORMAT_VP9;
      switch (avctx->profile)
      {
        case FF_PROFILE_VP9_0:
        case FF_PROFILE_VP9_2:
        case FF_PROFILE_UNKNOWN:
          break;
        default:
          av_log(avctx, AV_LOG_ERROR, "Error: profile %d not supported.\n", avctx->profile);
          return AVERROR_INVALIDDATA;
      }
      break;
    case AV_CODEC_ID_MJPEG:
      s->api_ctx.codec_format = NI_CODEC_FORMAT_JPEG;
      min_resolution_width    = NI_MIN_RESOLUTION_WIDTH_JPEG;
      min_resolution_height   = NI_MIN_RESOLUTION_HEIGHT_JPEG;
      switch (avctx->profile)
      {
        case FF_PROFILE_MJPEG_HUFFMAN_BASELINE_DCT:
        case FF_PROFILE_UNKNOWN:
          break;
        default:
          av_log(avctx, AV_LOG_ERROR, "Error: profile %d not supported.\n", avctx->profile);
          return AVERROR_INVALIDDATA;
      }
      break;
    default:
      s->api_ctx.codec_format = NI_CODEC_FORMAT_H264;
      switch (avctx->profile)
      {
        case FF_PROFILE_H264_BASELINE:
        case FF_PROFILE_H264_CONSTRAINED_BASELINE:
        case FF_PROFILE_H264_MAIN:
        case FF_PROFILE_H264_EXTENDED:
        case FF_PROFILE_H264_HIGH:
        case FF_PROFILE_H264_HIGH_10:
        case FF_PROFILE_UNKNOWN:
          break;
        default:
          av_log(avctx, AV_LOG_ERROR, "Error: profile %d not supported.\n", avctx->profile);
          return AVERROR_INVALIDDATA;
      }
      break;
  }

  if (avctx->width > NI_MAX_RESOLUTION_WIDTH ||
      avctx->height > NI_MAX_RESOLUTION_HEIGHT ||
      avctx->width * avctx->height > NI_MAX_RESOLUTION_AREA) {
      av_log(avctx, AV_LOG_ERROR,
             "Error XCoder resolution %dx%d not supported\n", avctx->width,
             avctx->height);
      av_log(avctx, AV_LOG_ERROR, "Max Supported Width: %d Height %d Area %d\n",
             NI_MAX_RESOLUTION_WIDTH, NI_MAX_RESOLUTION_HEIGHT,
             NI_MAX_RESOLUTION_AREA);
      return AVERROR_EXTERNAL;
  } else if (avctx->width < min_resolution_width ||
             avctx->height < min_resolution_height) {
      av_log(avctx, AV_LOG_ERROR,
             "Error XCoder resolution %dx%d not supported\n", avctx->width,
             avctx->height);
      av_log(avctx, AV_LOG_ERROR, "Min Supported Width: %d Height %d\n",
             min_resolution_width, min_resolution_height);
      return AVERROR_EXTERNAL;
  }

  s->offset = 0LL;

  s->draining = 0;

  s->api_ctx.pic_reorder_delay = avctx->has_b_frames;
  s->api_ctx.bit_depth_factor = 1;
  if (AV_PIX_FMT_YUV420P10BE == avctx->pix_fmt ||
      AV_PIX_FMT_YUV420P10LE == avctx->pix_fmt ||
      AV_PIX_FMT_P010LE == avctx->pix_fmt)
  {
    s->api_ctx.bit_depth_factor = 2;
  }
   av_log(avctx, AV_LOG_VERBOSE, "xcoder_setup_decoder: pix_fmt %u bit_depth_factor %u\n", avctx->pix_fmt, s->api_ctx.bit_depth_factor);

  //Xcoder User Configuration
  if (ni_decoder_init_default_params(p_param, avctx->framerate.num, avctx->framerate.den, avctx->bit_rate, avctx->width, avctx->height) < 0)
  {

    av_log(avctx, AV_LOG_INFO, "Error setting params\n");

    return AVERROR(EINVAL);
  }

  if (s->xcoder_opts)
  {
    AVDictionary *dict = NULL;
    AVDictionaryEntry *en = NULL;

    if (av_dict_parse_string(&dict, s->xcoder_opts, "=", ":", 0))
    {
      av_log(avctx, AV_LOG_ERROR, "Xcoder options provided contain error(s)\n");
      return AVERROR_EXTERNAL;
    }
    else
    {
      while ((en = av_dict_get(dict, "", en, AV_DICT_IGNORE_SUFFIX)))
      {
        int parse_ret = ni_decoder_params_set_value(p_param, en->key, en->value);
        switch (parse_ret)
        {
        case NI_RETCODE_PARAM_INVALID_NAME:
          av_log(avctx, AV_LOG_ERROR, "Unknown option: %s.\n", en->key);
          return AVERROR_EXTERNAL;
        case NI_RETCODE_PARAM_ERROR_TOO_BIG:
          av_log(avctx, AV_LOG_ERROR, "Invalid %s: too big, max char len = %d\n", en->key, NI_MAX_PPU_PARAM_EXPR_CHAR);
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
          av_log(avctx, AV_LOG_ERROR, "Invalid value for %s: %s.\n", en->key, en->value);
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

    for (size_t i = 0; i < NI_MAX_NUM_OF_DECODER_OUTPUTS; i++) {
      if (p_param->dec_input_params.crop_mode[i] != NI_DEC_CROP_MODE_AUTO) {
        continue;
      }
      for (size_t j = 0; j < 4; j++) {
        if (strlen(p_param->dec_input_params.cr_expr[i][j])) {
          av_log(avctx, AV_LOG_ERROR, "Setting crop parameters without setting crop mode to manual?\n");
          return AVERROR_EXTERNAL;
        }
      }
    }
  }
  parse_symbolic_decoder_param(s);
  return 0;
}

int xcoder_decode_init(AVCodecContext *avctx)
{
  int ret = 0;
  XCoderH264DecContext *s = avctx->priv_data;
  const AVPixFmtDescriptor *desc;
  ni_xcoder_params_t *p_param = &s->api_param;
  ni_log_set_level(ff_to_ni_log_level(av_log_get_level()));

  av_log(avctx, AV_LOG_VERBOSE, "XCoder decode init\n");

  avctx->sw_pix_fmt = avctx->pix_fmt;

  //av_log(avctx, AV_LOG_VERBOSE, "XCoder setup device decoder: pix_fmt set to AV_PIX_FMT_NI_QUAD\n");//maybe later check for hwcontext first then apply this

  desc = av_pix_fmt_desc_get(avctx->sw_pix_fmt);
  av_log(avctx, AV_LOG_VERBOSE, "width: %d height: %d sw_pix_fmt: %s\n",
         avctx->width, avctx->height, desc ? desc->name : "NONE");

  if (0 == avctx->width || 0 == avctx->height)
  {
    av_log(avctx, AV_LOG_ERROR, "Error probing input stream\n");
    return AVERROR_INVALIDDATA;
  }

  switch (avctx->pix_fmt)
  {
    case AV_PIX_FMT_YUV420P:
    case AV_PIX_FMT_YUV420P10BE:
    case AV_PIX_FMT_YUV420P10LE:
    case AV_PIX_FMT_YUVJ420P:
    case AV_PIX_FMT_GRAY8:
      break;
    default:
      av_log(avctx, AV_LOG_ERROR, "Error: pixel format %s not supported.\n",
             desc ? desc->name : "NONE");
      return AVERROR_INVALIDDATA;
  }

  av_log(avctx, AV_LOG_VERBOSE, "(avctx->field_order = %d)\n", avctx->field_order);
  if (avctx->field_order > AV_FIELD_PROGRESSIVE)
  { //AVFieldOrder with bottom or top coding order represents interlaced video
    av_log(avctx, AV_LOG_ERROR, "interlaced video not supported!\n");
    return AVERROR_INVALIDDATA;
  }

  if ((ret = xcoder_setup_decoder(avctx)) < 0)
  {
    return ret;
  }

  //--------reassign pix format based on user param------------//
  if (p_param->dec_input_params.semi_planar[0])
  {
    if (avctx->sw_pix_fmt == AV_PIX_FMT_YUV420P10BE ||
      avctx->sw_pix_fmt == AV_PIX_FMT_YUV420P10LE ||
      avctx->sw_pix_fmt == AV_PIX_FMT_YUV420P)
    {
      av_log(avctx, AV_LOG_VERBOSE, "XCoder decode init: YV12 forced to NV12\n");
      avctx->sw_pix_fmt = (avctx->sw_pix_fmt == AV_PIX_FMT_YUV420P) ? AV_PIX_FMT_NV12 : AV_PIX_FMT_P010LE;
    }
  }
  if (p_param->dec_input_params.force_8_bit[0])
  {
    if (avctx->sw_pix_fmt == AV_PIX_FMT_YUV420P10BE ||
      avctx->sw_pix_fmt == AV_PIX_FMT_YUV420P10LE ||
      avctx->sw_pix_fmt == AV_PIX_FMT_P010LE)
    {
      av_log(avctx, AV_LOG_VERBOSE, "XCoder decode init: 10Bit input forced to 8bit\n");
      avctx->sw_pix_fmt = (avctx->sw_pix_fmt == AV_PIX_FMT_P010LE) ? AV_PIX_FMT_NV12 : AV_PIX_FMT_YUV420P;
      s->api_ctx.bit_depth_factor = 1;
    }
  }
  if (p_param->dec_input_params.hwframes)
  { //need to set before open decoder
    s->api_ctx.hw_action = NI_CODEC_HW_ENABLE;
  }
  else
  {
    s->api_ctx.hw_action = NI_CODEC_HW_NONE;
  }
  //------reassign pix format based on user param done--------//

  if (s->custom_sei_type == USER_DATA_UNREGISTERED_SEI_PAYLOAD_TYPE ||
      p_param->dec_input_params.custom_sei_passthru == USER_DATA_UNREGISTERED_SEI_PAYLOAD_TYPE)
  {
    // use SW passthru only
    s->api_ctx.enable_user_data_sei_passthru = 0;
  }
  else
  {
    s->api_ctx.enable_user_data_sei_passthru = s->enable_user_data_sei_passthru;
  }

  av_log(avctx, AV_LOG_VERBOSE, "api_ctx %p api_ctx/s: user_data_sei_passthru = %d/%d, custom_sei_type = %d\n", 
         &s->api_ctx, s->api_ctx.enable_user_data_sei_passthru, s->enable_user_data_sei_passthru, s->custom_sei_type);

  // reference h264_decode_init in h264dec.c
  if (avctx->ticks_per_frame == 1)
  {
    if (avctx->time_base.den < INT_MAX / 2)
    {
      avctx->time_base.den *= 2;
    }
    else
      avctx->time_base.num /= 2;
  }

  avctx->ticks_per_frame = 2;

  s->started = 0;
  memset(&s->api_pkt, 0, sizeof(ni_packet_t));
  s->pkt_nal_bitmap = 0;

#ifdef NI_DEC_GSTREAMER_SUPPORT
  s->cur_gs_opaque = NULL;
  s->cur_gs_buf0 = NULL;
  int i = 0;
  for (i = 0; i < NI_FIFO_SZ; i++) {
      s->gs_data[i].opaque = NULL;
      s->gs_data[i].buf0 = NULL;
  }
#endif

  av_log(avctx, AV_LOG_VERBOSE, "XCoder decode init: time_base = %d/%d, frame rate = %d/%d, ticks_per_frame=%d\n", avctx->time_base.num, avctx->time_base.den, avctx->framerate.num, avctx->framerate.den, avctx->ticks_per_frame);

  // overwrite keep alive timeout value here with a custom value if it was
  // provided
  // if xcoder option is set then overwrite the (legacy) decoder option
  uint32_t xcoder_timeout = s->api_param.dec_input_params.keep_alive_timeout;
  if (xcoder_timeout != NI_DEFAULT_KEEP_ALIVE_TIMEOUT) 
  {
      s->api_ctx.keep_alive_timeout = xcoder_timeout;
  } 
  else 
  {
      s->api_ctx.keep_alive_timeout = s->keep_alive_timeout;
  }
  av_log(avctx, AV_LOG_VERBOSE, "Custom NVME Keep Alive Timeout set to %d\n",
         s->api_ctx.keep_alive_timeout);
  s->api_ctx.decoder_low_delay = s->low_delay =
      s->api_param.dec_input_params.decoder_low_delay;

  s->api_ctx.p_session_config = &s->api_param;

  if ((ret = ff_xcoder_dec_init(avctx, s)) < 0)
  {
    goto done;
  }

  s->current_pts = 0;

done:
  //if ( (NI_INVALID_DEVICE_HANDLE == s->api_ctx.blk_io_handle) || (NI_INVALID_DEVICE_HANDLE == s->api_ctx.device_handle) )
  //{
  //  xcoder_decode_close(avctx);
  //}
  return ret;
}

// reset and restart when xcoder decoder resets
int xcoder_decode_reset(AVCodecContext *avctx)
{
  XCoderH264DecContext *s = avctx->priv_data;
  ni_retcode_t ret = NI_RETCODE_FAILURE;
  av_log(avctx, AV_LOG_VERBOSE, "XCoder decode reset\n");

  ni_device_session_close(&s->api_ctx, s->eos, NI_DEVICE_TYPE_DECODER);

  ni_device_session_context_clear(&s->api_ctx);

#ifdef _WIN32
  ni_device_close(s->api_ctx.device_handle);
#elif __linux__
  ni_device_close(s->api_ctx.device_handle);
  ni_device_close(s->api_ctx.blk_io_handle);
#endif
  s->api_ctx.device_handle = NI_INVALID_DEVICE_HANDLE;
  s->api_ctx.blk_io_handle = NI_INVALID_DEVICE_HANDLE;

  ni_packet_buffer_free(&(s->api_pkt.data.packet));
  int64_t bcp_current_pts = s->current_pts;
  ret = xcoder_decode_init(avctx);
  s->current_pts = bcp_current_pts;
  s->api_ctx.session_run_state = SESSION_RUN_STATE_RESETTING;
  return ret;
}

static int xcoder_send_receive(AVCodecContext *avctx,
                               XCoderH264DecContext *s,
                               AVFrame *frame, bool wait)
{
  int ret;

  /* send any pending data from buffered packet */
  while (s->buffered_pkt.size)
  {
    ret = ff_xcoder_dec_send(avctx, s, &s->buffered_pkt);
    if (ret == AVERROR(EAGAIN))
      break;
    else if (ret < 0)
      return ret;
    s->buffered_pkt.size -= ret;
    s->buffered_pkt.data += ret;
    if (s->buffered_pkt.size <= 0)
    {
    }

#ifdef NI_DEC_GSTREAMER_SUPPORT
    // Check the buffer, if not NULL means has aroundwrap issue.
    if (s->gs_data[(s->api_ctx.pkt_index - 1) % NI_FIFO_SZ].opaque != NULL ||
        s->gs_data[(s->api_ctx.pkt_index - 1) % NI_FIFO_SZ].buf0 != NULL) {
        av_log(avctx, AV_LOG_ERROR,
               "ERROR: GS opaque should be full and be overwrited please "
               "increasing NI_FIFO_SZ (%d)!\n",
               NI_FIFO_SZ);
    }
    // pkt is sent out, store GS data based on pkt offset so it can be
    // retrieved when the decoded frame is returned
    s->gs_data[(s->api_ctx.pkt_index - 1) % NI_FIFO_SZ].opaque =
        s->cur_gs_opaque;
    s->gs_data[(s->api_ctx.pkt_index - 1) % NI_FIFO_SZ].buf0 = s->cur_gs_buf0;
    s->cur_gs_opaque = NULL;
    s->cur_gs_buf0 = NULL;

    s->gs_opaque_offsets_index_min[(s->api_ctx.pkt_index - 1) % NI_FIFO_SZ] =
        s->api_ctx.pkt_offsets_index_min[(s->api_ctx.pkt_index - 1) % NI_FIFO_SZ];

    s->gs_opaque_offsets_index[(s->api_ctx.pkt_index - 1) % NI_FIFO_SZ] =
        s->api_ctx.pkt_offsets_index[(s->api_ctx.pkt_index - 1) % NI_FIFO_SZ];

    av_log(avctx, AV_LOG_DEBUG, "pkt # %"PRIu64" bytes %d offset %"PRIu64" "
                                "%"PRIu64" opaque %p buf0 %p\n", (s->api_ctx.pkt_index - 1) % NI_FIFO_SZ,
           s->buffered_pkt.size,
           s->gs_opaque_offsets_index_min[(s->api_ctx.pkt_index - 1) % NI_FIFO_SZ],
           s->gs_opaque_offsets_index[(s->api_ctx.pkt_index - 1) % NI_FIFO_SZ],
           s->gs_data[(s->api_ctx.pkt_index - 1) % NI_FIFO_SZ].opaque,
           s->gs_data[(s->api_ctx.pkt_index - 1) % NI_FIFO_SZ].buf0);
#endif

    av_packet_unref(&s->buffered_pkt);
  }

  /* check for new frame */
  return ff_xcoder_dec_receive(avctx, s, frame, wait);
}

int xcoder_receive_frame(AVCodecContext *avctx, AVFrame *frame)
{
  XCoderH264DecContext *s = avctx->priv_data;
  int ret;

  const AVPixFmtDescriptor *desc;

  av_log(avctx, AV_LOG_VERBOSE, "XCoder receive frame\n");

  /* 
   * reference mediacodec_receive_frame in mediacodec.c.
   *
   * After we have buffered an input packet, check if the codec is in the
   * flushing state. If it is, we need to call ff_xcoder_dec_flush.
   *
   * ff_xcoder_dec_flush returns 0 if the flush cannot be performed on
   * the codec (because the user retains frames). The codec stays in the
   * flushing state.
   *
   * ff_xcoder_dec_flush returns 1 if the flush can actually be
   * performed on the codec. The codec leaves the flushing state and can
   * process again packets.
   *
   * ff_xcoder_dec_flush returns a negative value if an error has
   * occurred.
   *
   * NetInt: for now we don't consider the case of user retaining the frame
   *         (connected decoder-encoder case), so the return can only be 1
   *         (flushed successfully), or < 0 (failure)
   */
  if (ff_xcoder_dec_is_flushing(avctx, s))
  {
    if (!ff_xcoder_dec_flush(avctx, s))
    {
      return AVERROR(EAGAIN);
    }
  }

  // give priority to sending data to decoder
  if (s->buffered_pkt.size == 0)
  {
    ret = ff_decode_get_packet(avctx, &s->buffered_pkt);
    if (ret < 0) {
        av_log(avctx, AV_LOG_VERBOSE, "ff_decode_get_packet 1 rc: %s\n",
               av_err2str(ret));
    }
    else
    {
        av_log(avctx, AV_LOG_DEBUG, "ff_decode_get_packet 1 rc: Success\n");
#ifdef NI_DEC_GSTREAMER_SUPPORT
            AVFrame frame_copy = {0};

            /* avoid ff_get_buffer failure in case HW AVFrame */
            AVBufferRef *tmp_ctx = avctx->hw_frames_ctx;
            enum AVPixelFormat tmp_fmt = avctx->pix_fmt;
            avctx->hw_frames_ctx = NULL;
            avctx->pix_fmt = avctx->sw_pix_fmt;

            av_frame_copy_props(&frame_copy, frame);

            // retrieve/save from GStreamer info of this pkt which is the
            // returned frame's opaque and buf[0]
            ff_get_buffer(avctx, &frame_copy, 0);
            s->cur_gs_opaque = frame_copy.opaque;
            s->cur_gs_buf0 = frame_copy.buf[0];
            frame_copy.opaque = NULL;
            frame_copy.buf[0] = NULL;

            avctx->hw_frames_ctx = tmp_ctx;
            avctx->pix_fmt = tmp_fmt;
            av_log(avctx, AV_LOG_DEBUG, "ff_decode_get_packet 1 rc: Success pkt size "
                                        "= %d call ff_get_buffer return opaque %p buf0 %p\n",
                   s->buffered_pkt.size, s->cur_gs_opaque, s->cur_gs_buf0);

            av_frame_unref(&frame_copy);
#endif
    }
  }

  /* flush buffered packet and check for new frame */
  ret = xcoder_send_receive(avctx, s, frame, false);
  if (NI_RETCODE_ERROR_VPU_RECOVERY == ret)
  {
    ret = xcoder_decode_reset(avctx);
    if (0 == ret)
    {
      return AVERROR(EAGAIN);
    }
    else
    {
      return ret;
    }
  }
  else if (ret != AVERROR(EAGAIN))
    return ret;

  /* skip fetching new packet if we still have one buffered */
  if (s->buffered_pkt.size > 0)
    return xcoder_send_receive(avctx, s, frame, true);

  /* fetch new packet or eof */
  ret = ff_decode_get_packet(avctx, &s->buffered_pkt);
  if (ret < 0) {
      av_log(avctx, AV_LOG_VERBOSE, "ff_decode_get_packet 2 rc: %s\n",
             av_err2str(ret));
  }
  else
  {
      av_log(avctx, AV_LOG_DEBUG, "ff_decode_get_packet 2 rc: Success\n");

#ifdef NI_DEC_GSTREAMER_SUPPORT
        AVFrame frame_copy = {0};
        av_frame_copy_props(&frame_copy, frame);

        ff_get_buffer(avctx, &frame_copy, 0);
        s->cur_gs_opaque = frame_copy.opaque;
        s->cur_gs_buf0 = frame_copy.buf[0];
        frame_copy.opaque = NULL;
        frame_copy.buf[0] = NULL;

        av_log(avctx, AV_LOG_DEBUG, "ff_decode_get_packet 2 rc: Success pkt size = "
                                    "%d call ff_get_buffer return opaque %p buf0 %p\n",
               s->buffered_pkt.size, s->cur_gs_opaque, s->cur_gs_buf0);

        av_frame_unref(&frame_copy);
#endif
    }

  if (ret == AVERROR_EOF)
  {
    AVPacket null_pkt = {0};
    ret = ff_xcoder_dec_send(avctx, s, &null_pkt);

    /* ToDelete: mark end of stream; this should be signalled by Lib 
       s->eos = 1; */

#ifdef NI_DEC_GSTREAMER_SUPPORT
    if (ret < 0 && ret != AVERROR(EAGAIN))
#else
    if (ret < 0)
#endif
      return ret;
  }
  else if (ret < 0)
    return ret;
  else
  {
    av_log(avctx, AV_LOG_VERBOSE, "width: %d  height: %d\n", avctx->width, avctx->height);
    desc = av_pix_fmt_desc_get(avctx->pix_fmt);
    av_log(avctx, AV_LOG_VERBOSE, "pix_fmt: %s\n", desc ? desc->name : "NONE");
  }

  /* crank decoder with new packet */
  return xcoder_send_receive(avctx, s, frame, true);
}
