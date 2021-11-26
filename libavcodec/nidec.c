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

int xcoder_decode_close(AVCodecContext *avctx)
{
  XCoderH264DecContext *s = avctx->priv_data;
  av_log(avctx, AV_LOG_DEBUG, "XCoder decode close\n");

  /* this call shall release resource based on s->api_ctx */
  ff_xcoder_dec_close(avctx, s);

  av_packet_unref(&s->buffered_pkt);

  free(s->extradata);
  s->extradata = NULL;
  s->extradata_size = 0;
  s->got_first_idr = 0;

  ni_rsrc_free_device_context(s->rsrc_ctx);
  s->rsrc_ctx = NULL;

  return 0;
}

static int xcoder_setup_decoder(AVCodecContext *avctx)
{
  XCoderH264DecContext *s = avctx->priv_data;
  ni_encoder_params_t *p_param = &s->api_param;

  av_log(avctx, AV_LOG_DEBUG, "XCoder setup device decoder\n");
  //s->api_ctx.session_id = NI_INVALID_SESSION_ID;
  ni_device_session_context_init(&(s->api_ctx));
  s->api_ctx.codec_format = NI_CODEC_FORMAT_H264;
  if (avctx->codec_id == AV_CODEC_ID_HEVC)
  {
    s->api_ctx.codec_format = NI_CODEC_FORMAT_H265;
  }

  if (0 == strcmp(s->dev_xcoder, LIST_DEVICES_STR))
  {
    av_log(avctx, AV_LOG_DEBUG, "XCoder: printing out all xcoder devices and their load, and exit ...\n");
    ni_rsrc_print_all_devices_capability();
    return AVERROR_EXIT;
  }
  else if (avctx->width > NI_MAX_RESOLUTION_WIDTH ||
           avctx->height > NI_MAX_RESOLUTION_HEIGHT ||
           avctx->width * avctx->height > NI_MAX_RESOLUTION_AREA)
  {
    av_log(avctx, AV_LOG_ERROR, "Error XCoder resolution %dx%d not supported\n",
           avctx->width, avctx->height);
    av_log(avctx, AV_LOG_ERROR, "Max Supported Width: %d Height %d Area %d\n",
           NI_MAX_RESOLUTION_WIDTH, NI_MAX_RESOLUTION_HEIGHT, NI_MAX_RESOLUTION_AREA);
    return AVERROR_EXTERNAL;
  }

  s->offset = 0LL;

  s->draining = 0;

  s->api_ctx.bit_depth_factor = 1;
  if (AV_PIX_FMT_YUV420P10BE == avctx->sw_pix_fmt ||
      AV_PIX_FMT_YUV420P10LE == avctx->sw_pix_fmt)
  {
    s->api_ctx.bit_depth_factor = 2;
  }

  return 0;
}

int xcoder_decode_init(AVCodecContext *avctx)
{
  int ret = 0;
  XCoderH264DecContext *s = avctx->priv_data;
  const AVPixFmtDescriptor *desc;
  ni_encoder_params_t *p_param = &s->api_param;

  ni_log_set_level(ff_to_ni_log_level(av_log_get_level()));

  av_log(avctx, AV_LOG_DEBUG, "XCoder decode init\n");

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

  av_log(avctx, AV_LOG_DEBUG, "(avctx->field_order = %d)\n", avctx->field_order);
  if (avctx->field_order > AV_FIELD_PROGRESSIVE)
  { //AVFieldOrder with bottom or top coding order represents interlaced video
    av_log(avctx, AV_LOG_ERROR, "interlaced video not supported!\n");
    return AVERROR_INVALIDDATA;
  }

  if (s->nvme_io_size > 0 && s->nvme_io_size % 4096 != 0)
  {
    av_log(avctx, AV_LOG_ERROR, "Error XCoder iosize is not 4KB aligned!\n");
    return AVERROR_EXTERNAL;
  }

  if ((ret = xcoder_setup_decoder(avctx)) < 0)
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
      avctx->time_base.num /= 2;
  }

  avctx->ticks_per_frame = 2;

  s->started = 0;
  memset(&s->api_pkt, 0, sizeof(ni_packet_t));
  s->extradata = NULL;
  s->extradata_size = 0;
  s->got_first_idr = 0;
  s->pkt_nal_bitmap = 0;

  av_log(avctx, AV_LOG_VERBOSE, "XCoder decode init: time_base = %d/%d, frame rate = %d/%d, ticks_per_frame=%d\n", avctx->time_base.num, avctx->time_base.den, avctx->framerate.num, avctx->framerate.den, avctx->ticks_per_frame);

  //overwrite the nvme io size here with a custom value if it was provided
  if (s->nvme_io_size > 0)
  {
    s->api_ctx.max_nvme_io_size = s->nvme_io_size;
    av_log(avctx, AV_LOG_VERBOSE, "Custom NVMEe IO Size set to = %d\n", s->api_ctx.max_nvme_io_size);
  }

  //overwrite keep alive timeout value here with a custom value if it was provided
  s->api_ctx.keep_alive_timeout = s->keep_alive_timeout;
  av_log(avctx, AV_LOG_VERBOSE, "Custom NVMEe Keep Alive Timeout set to = %d\n", s->api_ctx.keep_alive_timeout);

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

    if (! av_dict_parse_string(&dict, s->xcoder_opts, "=", ":", 0))
    {
      while ((en = av_dict_get(dict, "", en, AV_DICT_IGNORE_SUFFIX)))
      {
        int parse_ret = ni_encoder_params_set_value(p_param, en->key,
                                                    en->value, &s->api_ctx);
        if (NI_RETCODE_SUCCESS != parse_ret)
        {
          av_log(avctx, AV_LOG_ERROR, "Error parsing xcoder-params: %d\n",
                 parse_ret);
          return AVERROR_EXTERNAL;
        }
      }
      av_dict_free(&dict);
    }
  }

  //hwframes can be set from 'out=hw' or 'hwframes 1' param
  p_param->dec_input_params.hwframes = s->hwFrames | p_param->dec_input_params.hwframes;

  if (s->hwFrames || p_param->dec_input_params.hwframes)
  {
    s->api_ctx.hw_action = NI_CODEC_HW_ENABLE;
    av_log(avctx, AV_LOG_TRACE, "xcoder_decode_init: enable hw codec\n");
  }
  else
  {
    s->api_ctx.hw_action = NI_CODEC_HW_NONE;//
  }

  s->api_ctx.p_session_config = &s->api_param;

  if ((ret = ff_xcoder_dec_init(avctx, s)) < 0)
  {
    goto done;
  }

  s->current_pts = 0;

done:
  if ( (NI_INVALID_DEVICE_HANDLE == s->api_ctx.blk_io_handle) || (NI_INVALID_DEVICE_HANDLE == s->api_ctx.device_handle) )
  {
    xcoder_decode_close(avctx);
  }
  return ret;
}

// reset and restart when xcoder decoder resets
int xcoder_decode_reset(AVCodecContext *avctx)
{
  XCoderH264DecContext *s = avctx->priv_data;
  ni_retcode_t ret = NI_RETCODE_FAILURE;
  int64_t bcp_current_pts = s->current_pts;
  int draining = s->draining;

  av_log(avctx, AV_LOG_WARNING, "XCoder decode reset\n");

  s->vpu_reset = 1;
  ret = ni_device_session_close(&s->api_ctx, s->eos, NI_DEVICE_TYPE_DECODER);

#ifdef _WIN32
  ni_device_close(s->api_ctx.device_handle);
#elif __linux__
  ni_device_close(s->api_ctx.device_handle);
  ni_device_close(s->api_ctx.blk_io_handle);
#endif
  s->api_ctx.device_handle = NI_INVALID_DEVICE_HANDLE;
  s->api_ctx.blk_io_handle = NI_INVALID_DEVICE_HANDLE;

  ni_packet_buffer_free(&(s->api_pkt.data.packet));
  ret = xcoder_decode_init(avctx);
  s->draining = draining;  // recover the draining state when resetting.
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
  if (s->buffered_pkt.size)
  {
    ret = ff_xcoder_dec_send(avctx, s, &s->buffered_pkt);
    if (ret == AVERROR(EAGAIN))
    {
      av_log(avctx, AV_LOG_DEBUG, "ff_xcoder_dec_send() return eagain\n");
    }
    else if (ret < 0)
    {
      return ret;
    }
    else
    {
      av_packet_unref(&s->buffered_pkt);
    }
  }

  /* check for new frame */
  ret = ff_xcoder_dec_receive(avctx, s, frame, wait);
  if (NI_RETCODE_ERROR_VPU_RECOVERY == ret)
  {
    av_log(avctx, AV_LOG_ERROR, "Failed to receive frame because of VPU recovery\n");
    ret = xcoder_decode_reset(avctx);
    if (0 == ret)
    {
      return AVERROR(EAGAIN);
    }
  }

  return ret;
}

int xcoder_receive_frame(AVCodecContext *avctx, AVFrame *frame)
{
  XCoderH264DecContext *s = avctx->priv_data;
  int ret;
  const AVPixFmtDescriptor *desc;

  av_log(avctx, AV_LOG_DEBUG, "XCoder receive frame\n");

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
    if (ret < 0)
    {
      av_log(avctx, AV_LOG_DEBUG, "ff_decode_get_packet 1 rc: %s\n", av_err2str(ret));
    }
    else
    {
      av_log(avctx, AV_LOG_DEBUG, "ff_decode_get_packet 1 rc: Success\n");
    }

  }

  /* flush buffered packet and check for new frame */
  ret = xcoder_send_receive(avctx, s, frame, false);
  if (ret != AVERROR(EAGAIN))
    return ret;

  /* skip fetching new packet if we still have one buffered */
  if (s->buffered_pkt.size > 0)
    return xcoder_send_receive(avctx, s, frame, true);

  /* fetch new packet or eof */
  ret = ff_decode_get_packet(avctx, &s->buffered_pkt);
  if (ret < 0)
  {
    av_log(avctx, AV_LOG_DEBUG, "ff_decode_get_packet 2 rc: %s\n", av_err2str(ret));
  }
  else
  {
    av_log(avctx, AV_LOG_DEBUG, "ff_decode_get_packet 2 rc: Success\n");
  }

  if (ret == AVERROR_EOF)
  {
    AVPacket null_pkt = {0};
    ret = ff_xcoder_dec_send(avctx, s, &null_pkt);

    /* ToDelete: mark end of stream; this should be signalled by Lib 
       s->eos = 1; */

    if (ret < 0)
      return ret;
  }
  else if (ret < 0)
    return ret;
  else
  {
    av_log(avctx, AV_LOG_DEBUG, "width: %d  height: %d\n", avctx->width, avctx->height);
    desc = av_pix_fmt_desc_get(avctx->pix_fmt);
    av_log(avctx, AV_LOG_DEBUG, "pix_fmt: %s\n", desc ? desc->name : "NONE");
  }

  /* crank decoder with new packet */
  return xcoder_send_receive(avctx, s, frame, true);
}
