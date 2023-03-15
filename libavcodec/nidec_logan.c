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

#include "nidec_logan.h"

int xcoder_logan_decode_close(AVCodecContext *avctx)
{
  XCoderLoganDecContext *s = avctx->priv_data;
  av_log(avctx, AV_LOG_DEBUG, "XCoder decode close\n");

  /* this call shall release resource based on s->api_ctx */
  ff_xcoder_logan_dec_close(avctx, s);

  ni_logan_packet_buffer_free(&s->api_pkt.data.packet);
  ni_logan_device_session_context_clear(&s->api_ctx);

  av_packet_unref(&s->buffered_pkt);
  av_packet_unref(&s->seq_hdr_pkt);

  free(s->extradata);
  s->extradata = NULL;
  s->extradata_size = 0;
  s->got_first_key_frame = 0;

  ni_logan_rsrc_free_device_context(s->rsrc_ctx);
  s->rsrc_ctx = NULL;

  return 0;
}

static int xcoder_logan_setup_decoder(AVCodecContext *avctx)
{
  XCoderLoganDecContext *s = avctx->priv_data;

  av_log(avctx, AV_LOG_DEBUG, "XCoder setup device decoder\n");

  ni_logan_device_session_context_init(&(s->api_ctx));

  s->api_ctx.codec_format = NI_LOGAN_CODEC_FORMAT_H264;
  if (avctx->codec_id == AV_CODEC_ID_HEVC)
  {
    s->api_ctx.codec_format = NI_LOGAN_CODEC_FORMAT_H265;
  }

  if (0 == strcmp(s->dev_xcoder, LIST_DEVICES_STR))
  {
    av_log(avctx, AV_LOG_DEBUG, "XCoder: printing out all xcoder devices and their load, and exit ...\n");
    ni_logan_rsrc_print_all_devices_capability();
    return AVERROR_EXIT;
  }
  else if (avctx->width > NI_LOGAN_MAX_RESOLUTION_WIDTH ||
           avctx->height > NI_LOGAN_MAX_RESOLUTION_HEIGHT ||
           avctx->width * avctx->height > NI_LOGAN_MAX_RESOLUTION_AREA)
  {
    av_log(avctx, AV_LOG_ERROR, "Error XCoder resolution %dx%d not supported\n",
           avctx->width, avctx->height);
    av_log(avctx, AV_LOG_ERROR, "Max Supported Width: %d Height %d Area %d\n",
           NI_LOGAN_MAX_RESOLUTION_WIDTH, NI_LOGAN_MAX_RESOLUTION_HEIGHT, NI_LOGAN_MAX_RESOLUTION_AREA);
    return AVERROR_EXTERNAL;
  }

  s->offset = 0LL;
  s->draining = 0;
  s->api_ctx.pic_reorder_delay = avctx->has_b_frames;
  s->api_ctx.bit_depth_factor = 1;

  if (AV_PIX_FMT_YUV420P10BE == avctx->sw_pix_fmt ||
      AV_PIX_FMT_YUV420P10LE == avctx->sw_pix_fmt)
  {
    s->api_ctx.bit_depth_factor = 2;
  }

  return 0;
}

int xcoder_logan_decode_init(AVCodecContext *avctx)
{
  int ret = 0;
  XCoderLoganDecContext *s = avctx->priv_data;
  const AVPixFmtDescriptor *desc;
  ni_logan_encoder_params_t *p_param = &s->api_param;

  ni_log_set_level(ff_to_ni_log_level(av_log_get_level()));

  av_log(avctx, AV_LOG_DEBUG, "XCoder decode init pix_fmt %d\n",
         avctx->pix_fmt);

  if (s->dev_xcoder == NULL)
  {
    av_log(avctx, AV_LOG_ERROR, "Error: XCoder decode options dev_xcoder is null\n");
    return AVERROR_INVALIDDATA;
  }
  else
  {
    av_log(avctx, AV_LOG_VERBOSE, "XCoder options: dev_xcoder: %s dev_dec_idx %d\n",
           s->dev_xcoder, s->dev_dec_idx);
  }

  avctx->sw_pix_fmt = avctx->pix_fmt;

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
      break;
    default:
      av_log(avctx, AV_LOG_ERROR, "Error: pixel format %s not supported.\n",
             desc ? desc->name : "NONE");
      return AVERROR_INVALIDDATA;
  }

  // Check profile idc
  if (avctx->codec_id == AV_CODEC_ID_H264)
  {
    switch (avctx->profile)
    {
      case FF_PROFILE_H264_BASELINE:
      case FF_PROFILE_H264_CONSTRAINED_BASELINE:
      case FF_PROFILE_H264_MAIN:
      case FF_PROFILE_H264_EXTENDED:
      case FF_PROFILE_H264_HIGH:
      case FF_PROFILE_H264_HIGH_10:
        break;
      default:
        av_log(avctx, AV_LOG_ERROR, "Error: profile %d not supported.\n", avctx->profile);
        return AVERROR_INVALIDDATA;
    }
  }
  else if (avctx->codec_id == AV_CODEC_ID_HEVC)
  {
    switch (avctx->profile)
    {
      case FF_PROFILE_HEVC_MAIN:
      case FF_PROFILE_HEVC_MAIN_10:
      case FF_PROFILE_HEVC_MAIN_STILL_PICTURE:
        break;
      default:
        av_log(avctx, AV_LOG_ERROR, "Error: profile %d not supported.\n", avctx->profile);
        return AVERROR_INVALIDDATA;
    }
  }

  av_log(avctx, AV_LOG_DEBUG, "(avctx->field_order = %d)\n", avctx->field_order);
  if (avctx->field_order > AV_FIELD_PROGRESSIVE)
  { //AVFieldOrder with bottom or top coding order represents interlaced video
    av_log(avctx, AV_LOG_ERROR, "interlaced video not supported!\n");
    return AVERROR_INVALIDDATA;
  }

  if ((ret = xcoder_logan_setup_decoder(avctx)) < 0)
  {
    return ret;
  }

  // reference h264_decode_init in h264dec.c
  if (avctx->ticks_per_frame == 1)
  {
    if (avctx->time_base.den < INT_MAX / 2)
    {
      avctx->time_base.den *= 2;
    }
    else
    {
      avctx->time_base.num /= 2;
    }
  }

  avctx->ticks_per_frame = 2;

  s->started = 0;
  memset(&s->api_pkt, 0, sizeof(ni_logan_packet_t));
  s->got_first_key_frame = 0;
  s->pkt_nal_bitmap = 0;

#ifdef NI_DEC_GSTREAMER_SUPPORT
  s->cur_gs_opaque = NULL;
  s->cur_gs_buf0 = NULL;
  int i = 0;
  for (i = 0; i < NI_LOGAN_FIFO_SZ; i++) {
    s->gs_data[i].opaque = NULL;
    s->gs_data[i].buf0 = NULL;
  }
#endif

  av_log(avctx, AV_LOG_VERBOSE, "XCoder decode init: time_base = %d/%d, "
         "frame rate = %d/%d, ticks_per_frame=%d\n", avctx->time_base.num,
         avctx->time_base.den, avctx->framerate.num, avctx->framerate.den,
         avctx->ticks_per_frame);

  //Xcoder User Configuration

  if (ni_logan_decoder_init_default_params(p_param, avctx->framerate.num,
      avctx->framerate.den, avctx->bit_rate, avctx->width, avctx->height) < 0)
  {
    av_log(avctx, AV_LOG_INFO, "Error setting params\n");
    return AVERROR(EINVAL);
  }

  if (s->xcoder_opts)
  {
    AVDictionary *dict = NULL;
    AVDictionaryEntry *dec = NULL;

    if (! av_dict_parse_string(&dict, s->xcoder_opts, "=", ":", 0))
    {
      while ((dec = av_dict_get(dict, "", dec, AV_DICT_IGNORE_SUFFIX)))
      {
        int parse_ret = ni_logan_decoder_params_set_value(p_param, dec->key,
                                                    dec->value);
        if (NI_LOGAN_RETCODE_SUCCESS != parse_ret)
        {
          av_log(avctx, AV_LOG_ERROR, "Error parsing xcoder-params: %d\n",
                 parse_ret);
          return AVERROR_EXTERNAL;
        }
      }
      av_dict_free(&dict);
    }
  }

  // overwrite keep alive timeout value here with a custom value if it was
  // provided
  // if xcoder option is set then overwrite the (legacy) decoder option
  uint32_t xcoder_timeout = s->api_param.dec_input_params.keep_alive_timeout;
  if (xcoder_timeout != NI_LOGAN_DEFAULT_KEEP_ALIVE_TIMEOUT)
  {
    s->api_ctx.keep_alive_timeout = xcoder_timeout;
  }
  else
  {
    s->api_ctx.keep_alive_timeout = s->keep_alive_timeout;
  }
  av_log(avctx, AV_LOG_VERBOSE, "Custom NVME Keep Alive Timeout set to %d\n",
         s->api_ctx.keep_alive_timeout);

  //overwrite set_high_priority value here with a custom value if it was provided
  uint32_t xcoder_high_priority = s->api_param.dec_input_params.set_high_priority;
  if(xcoder_high_priority != 0)
  {
    s->api_ctx.set_high_priority = xcoder_high_priority;
  }
  else
  {
    s->api_ctx.set_high_priority = s->set_high_priority;
  }
  av_log(avctx, AV_LOG_VERBOSE, "Custom NVMe set_high_priority set to = %d\n",
         s->api_ctx.set_high_priority);
  //hwframes can be set from 'out=hw' or 'hwframes 1' param
  p_param->dec_input_params.hwframes = s->hwFrames | p_param->dec_input_params.hwframes;

  if (s->hwFrames || p_param->dec_input_params.hwframes)
  {
    s->api_ctx.hw_action = NI_LOGAN_CODEC_HW_ENABLE;
    av_log(avctx, AV_LOG_TRACE, "xcoder_logan_decode_init: enable hw codec\n");
  }
  else
  {
    s->api_ctx.hw_action = NI_LOGAN_CODEC_HW_NONE;//
  }

  s->api_ctx.p_session_config = &s->api_param;

  if ((ret = ff_xcoder_logan_dec_init(avctx, s)) < 0)
  {
    goto done;
  }

  s->current_pts = 0;

done:
  if (NI_INVALID_DEVICE_HANDLE == s->api_ctx.blk_io_handle ||
      NI_INVALID_DEVICE_HANDLE == s->api_ctx.device_handle)
  {
    xcoder_logan_decode_close(avctx);
  }

  return ret;
}

// reset and restart when xcoder decoder resets
int xcoder_logan_decode_reset(AVCodecContext *avctx)
{
  XCoderLoganDecContext *s = avctx->priv_data;
  ni_logan_retcode_t ret = NI_LOGAN_RETCODE_FAILURE;
  int64_t bcp_current_pts = s->current_pts;
  int draining = s->draining;

  av_log(avctx, AV_LOG_WARNING, "XCoder decode reset\n");

  s->vpu_reset = 1;

  ret = ni_logan_device_session_close(&s->api_ctx, s->eos, NI_LOGAN_DEVICE_TYPE_DECODER);

#ifdef _WIN32
  ni_logan_device_close(s->api_ctx.device_handle);
#elif __linux__
  ni_logan_device_close(s->api_ctx.device_handle);
  ni_logan_device_close(s->api_ctx.blk_io_handle);
#endif
  s->api_ctx.device_handle = NI_INVALID_DEVICE_HANDLE;
  s->api_ctx.blk_io_handle = NI_INVALID_DEVICE_HANDLE;

  ni_logan_packet_buffer_free(&s->api_pkt.data.packet);
  ni_logan_device_session_context_clear(&s->api_ctx);

  ret = xcoder_logan_decode_init(avctx);
  s->draining = draining;  // recover the draining state when resetting.
  s->current_pts = bcp_current_pts;
  s->api_ctx.session_run_state = LOGAN_SESSION_RUN_STATE_RESETTING;

  // On VPU recovery send the cached sequence headers with IDR first.
  if (s->seq_hdr_pkt.size > 0 &&
      ff_xcoder_logan_dec_send(avctx, s, &s->seq_hdr_pkt) < 0)
  {
    s->vpu_reset = 0;
    return AVERROR_EXTERNAL;
  }

  return ret;
}

static int xcoder_logan_send_receive(AVCodecContext *avctx,
                                     XCoderLoganDecContext *s,
                                     AVFrame *frame, bool wait)
{
  int ret;

  if (s->buffered_pkt.size > 0)
  {
    ret = ff_xcoder_logan_dec_send(avctx, s, &s->buffered_pkt);
    if (ret == AVERROR(EAGAIN))
    {
      av_log(avctx, AV_LOG_DEBUG, "ff_xcoder_logan_dec_send() return eagain\n");
    }
    else if (ret < 0)
    {
      return ret;
    }
    else
    {
#ifdef NI_DEC_GSTREAMER_SUPPORT
    // pkt is sent out, store GS data based on pkt offset so it can be
    // retrieved when the decoded frame is returned
    s->gs_data[(s->api_ctx.pkt_index - 1) % NI_LOGAN_FIFO_SZ].opaque =
    s->cur_gs_opaque;
    s->gs_data[(s->api_ctx.pkt_index - 1) % NI_LOGAN_FIFO_SZ].buf0 = s->cur_gs_buf0;
    s->cur_gs_opaque = NULL;
    s->cur_gs_buf0 = NULL;

    s->gs_opaque_offsets_index_min[(s->api_ctx.pkt_index - 1) % NI_LOGAN_FIFO_SZ] =
    s->api_ctx.pkt_offsets_index_min[(s->api_ctx.pkt_index - 1) % NI_LOGAN_FIFO_SZ];

    s->gs_opaque_offsets_index[(s->api_ctx.pkt_index - 1) % NI_LOGAN_FIFO_SZ] =
    s->api_ctx.pkt_offsets_index[(s->api_ctx.pkt_index - 1) % NI_LOGAN_FIFO_SZ];

    av_log(avctx, AV_LOG_DEBUG, "pkt # %"PRIu64" bytes %d offset %"PRIu64" "
           "%"PRIu64" opaque %p buf0 %p\n", (s->api_ctx.pkt_index - 1) % NI_LOGAN_FIFO_SZ,
           s->buffered_pkt.size,
           s->gs_opaque_offsets_index_min[(s->api_ctx.pkt_index - 1) % NI_LOGAN_FIFO_SZ],
           s->gs_opaque_offsets_index[(s->api_ctx.pkt_index - 1) % NI_LOGAN_FIFO_SZ],
           s->gs_data[(s->api_ctx.pkt_index - 1) % NI_LOGAN_FIFO_SZ].opaque,
	   s->gs_data[(s->api_ctx.pkt_index - 1) % NI_LOGAN_FIFO_SZ].buf0);
#endif

      av_packet_unref(&s->buffered_pkt);
    }
  }

  /* check for new frame */
  ret = ff_xcoder_logan_dec_receive(avctx, s, frame, wait);
  if (NI_LOGAN_RETCODE_ERROR_VPU_RECOVERY == ret)
  {
    av_log(avctx, AV_LOG_ERROR, "Failed to receive frame because of VPU recovery\n");
    ret = xcoder_logan_decode_reset(avctx);
    if (0 == ret)
    {
      return AVERROR(EAGAIN);
    }
  }

  return ret;
}

int xcoder_logan_receive_frame(AVCodecContext *avctx, AVFrame *frame)
{
  XCoderLoganDecContext *s = avctx->priv_data;
  int ret;
  const AVPixFmtDescriptor *desc;

  av_log(avctx, AV_LOG_DEBUG, "XCoder receive frame\n");

  /*
   * reference mediacodec_receive_frame in mediacodec.c.
   *
   * After we have buffered an input packet, check if the codec is in the
   * flushing state. If it is, we need to call ff_xcoder_logan_dec_flush.
   *
   * ff_xcoder_logan_dec_flush returns 0 if the flush cannot be performed on
   * the codec (because the user retains frames). The codec stays in the
   * flushing state.
   *
   * ff_xcoder_logan_dec_flush returns 1 if the flush can actually be
   * performed on the codec. The codec leaves the flushing state and can
   * process again packets.
   *
   * ff_xcoder_logan_dec_flush returns a negative value if an error has
   * occurred.
   *
   * NetInt: for now we don't consider the case of user retaining the frame
   *         (connected decoder-encoder case), so the return can only be 1
   *         (flushed successfully), or < 0 (failure)
   */
  if (ff_xcoder_logan_dec_is_flushing(avctx, s))
  {
    if (!ff_xcoder_logan_dec_flush(avctx, s))
    {
      return AVERROR(EAGAIN);
    }
  }

  // give priority to sending data to decoder
  if (s->buffered_pkt.size == 0)
  {
    ret = ff_decode_get_packet(avctx, &s->buffered_pkt);
    if (ret < 0)
    {
      av_log(avctx, AV_LOG_DEBUG, "ff_decode_get_packet 1 rc: %s\n", av_err2str(ret));
    }
    else
    {
      av_log(avctx, AV_LOG_DEBUG, "ff_decode_get_packet 1 rc: Success\n");

#ifdef NI_DEC_GSTREAMER_SUPPORT
      AVFrame frame_copy = {0};
      av_frame_copy_props(&frame_copy, frame);

      // retrieve/save from GStreamer info of this pkt which is the
      // returned frame's opaque and buf[0]
      ff_get_buffer(avctx, &frame_copy, 0);
      s->cur_gs_opaque = frame_copy.opaque;
      s->cur_gs_buf0 = frame_copy.buf[0];
      frame_copy.opaque = NULL;
      frame_copy.buf[0] = NULL;

      av_log(avctx, AV_LOG_DEBUG, "ff_decode_get_packet 1 rc: Success pkt size "
             "= %d call ff_get_buffer return opaque %p buf0 %p\n",
	     s->buffered_pkt.size, s->cur_gs_opaque, s->cur_gs_buf0);

      av_frame_unref(&frame_copy);
#endif
    }
  }

  /* flush buffered packet and check for new frame */
  ret = xcoder_logan_send_receive(avctx, s, frame, false);
  if (ret != AVERROR(EAGAIN))
  {
    return ret;
  }

  /* skip fetching new packet if we still have one buffered */
  if (s->buffered_pkt.size > 0)
  {
    return xcoder_logan_send_receive(avctx, s, frame, true);
  }

  /* fetch new packet or eof */
  ret = ff_decode_get_packet(avctx, &s->buffered_pkt);
  if (ret < 0)
  {
    av_log(avctx, AV_LOG_DEBUG, "ff_decode_get_packet 2 rc: %s\n", av_err2str(ret));
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
    ret = ff_xcoder_logan_dec_send(avctx, s, &null_pkt);

    av_log(avctx, AV_LOG_DEBUG, "AVERROR_EOF, done ff_xcoder_logan_dec_send, "
           "ret = %d\n", ret);

    if (ret < 0)
    {
      return ret;
    }
  }
  else if (ret < 0)
  {
    av_log(avctx, AV_LOG_DEBUG, "ret < 0 but NOT AVERROR_EOF %d!\n", ret);
    return ret;
  }
  else
  {
    av_log(avctx, AV_LOG_DEBUG, "width: %d  height: %d\n", avctx->width, avctx->height);
    desc = av_pix_fmt_desc_get(avctx->pix_fmt);
    av_log(avctx, AV_LOG_DEBUG, "pix_fmt: %s\n", desc ? desc->name : "NONE");
  }

  /* crank decoder with new packet */
  return xcoder_logan_send_receive(avctx, s, frame, true);
}
