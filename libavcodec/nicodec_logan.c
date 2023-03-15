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

#include <ni_rsrc_api_logan.h>
#include <ni_av_codec_logan.h>
#include "nicodec_logan.h"
#include "nidec_logan.h"
#include "libavutil/pixdesc.h"
#include "libavutil/imgutils.h"
#include "libavutil/mastering_display_metadata.h"
#include "libavutil/hdr_dynamic_metadata.h"
#include "get_bits.h"
#include "internal.h"
#include "libavutil/intreadwrite.h"
#include "libavcodec/hevc.h"
#include "libavcodec/hevc_sei.h"
#include "libavcodec/h264.h"
#include "libavcodec/h264_sei.h"
#include "libavutil/hwcontext.h"
#include "libavutil/hwcontext_internal.h"
#include "libavutil/hwcontext_ni_logan.h"

static inline void ni_logan_buf_pool_free(void *opaque, uint8_t *data)
{
  if (data)
  {
    ni_logan_buf_t *buf = (ni_logan_buf_t *)opaque;
    ni_logan_decoder_frame_buffer_pool_return_buf(buf, (ni_logan_buf_pool_t *)buf->pool);
  }
}

static void ni_logan_frame_free(void *opaque, uint8_t *data)
{
  if (data)
  {
    ni_event_handle_t event_handle = (ni_event_handle_t) opaque;
    ni_logan_hwframe_surface_t* p_data3 = (ni_logan_hwframe_surface_t*) data; //for hwframes there is no data0,1,2
    //TODO use int32t device_handle to kill the buffer!
    if (p_data3->i8FrameIdx != NI_LOGAN_INVALID_HW_FRAME_IDX)
    {
#ifdef _WIN32
      int64_t handle = (((int64_t) p_data3->device_handle_ext) << 32) | p_data3->device_handle;
      ni_logan_decode_buffer_free(p_data3, (ni_device_handle_t) handle, event_handle);
#else
      ni_logan_decode_buffer_free(p_data3, p_data3->device_handle, event_handle);
#endif
    }
    free(p_data3);
  }
}

static inline void ni_logan_align_free_nop(void *opaque, uint8_t *data)
{
}

static inline void ni_logan_free(void *opaque, uint8_t *data)
{
  free(data);
}

int ff_xcoder_logan_dec_init(AVCodecContext *avctx,
                             XCoderLoganDecContext *s)
{
  /* ToDo: call xcode_dec_open to open a decoder instance */
  int ret;
  ni_logan_encoder_params_t *p_param = &s->api_param;

  s->api_ctx.hw_id = s->dev_dec_idx;
  if(s->low_delay)
  {
    s->api_ctx.decoder_low_delay = s->low_delay;
  }
  else
  {
    s->api_ctx.decoder_low_delay = s->api_param.dec_input_params.lowdelay;
  }
  strcpy(s->api_ctx.dev_xcoder, s->dev_xcoder);

  ret = ni_logan_device_session_open(&s->api_ctx, NI_LOGAN_DEVICE_TYPE_DECODER);
  if (ret != 0)
  {
    av_log(avctx, AV_LOG_ERROR, "Failed to open decoder (status = %d), "
           "resource unavailable\n", ret);
    ret = AVERROR_EXTERNAL;
    ff_xcoder_logan_dec_close(avctx, s);
  }
  else
  {
    s->dev_xcoder_name = s->api_ctx.dev_xcoder_name;
    s->blk_xcoder_name = s->api_ctx.blk_xcoder_name;
    s->dev_dec_idx = s->api_ctx.hw_id;
    av_log(avctx, AV_LOG_VERBOSE, "XCoder %s Index %d (inst: %d) opened successfully\n",
           s->dev_xcoder_name, s->dev_dec_idx, s->api_ctx.session_id);

    if (s->hwFrames || p_param->dec_input_params.hwframes)
    {
      if (!avctx->hw_device_ctx)
      {
        av_log(avctx, AV_LOG_DEBUG, "nicodec.c:ff_xcoder_logan_dec_init() hwdevice_ctx_create\n");

        av_hwdevice_ctx_create(&avctx->hw_device_ctx, AV_HWDEVICE_TYPE_NI_LOGAN, NULL, NULL, 0); //create with null device
      }
      if (!avctx->hw_frames_ctx)
      {
        avctx->hw_frames_ctx = av_hwframe_ctx_alloc(avctx->hw_device_ctx);

        if (!avctx->hw_frames_ctx)
        {
          ret = AVERROR(ENOMEM);
          return ret;
        }
      }
      s->hwfc = (AVHWFramesContext*)avctx->hw_frames_ctx->data;

      s->hwfc->format = AV_PIX_FMT_NI_LOGAN;
      s->hwfc->width = avctx->width;
      s->hwfc->height = avctx->height;

      s->hwfc->sw_format = avctx->sw_pix_fmt;
      s->hwfc->initial_pool_size = -1; //Decoder has its own dedicated pool

      ret = av_hwframe_ctx_init(avctx->hw_frames_ctx);
      avctx->pix_fmt = AV_PIX_FMT_NI_LOGAN;
      s->api_ctx.hw_action = NI_LOGAN_CODEC_HW_ENABLE;
    }
    else
    {
      avctx->pix_fmt = avctx->sw_pix_fmt; //reassign in case above conditions alter value
      s->api_ctx.hw_action = NI_LOGAN_CODEC_HW_NONE;//
    }
  }

  return ret;
}

int ff_xcoder_logan_dec_close(AVCodecContext *avctx, XCoderLoganDecContext *s)
{
  AVHWFramesContext *ctx;
  NILOGANFramesContext *dst_ctx;
  ni_logan_retcode_t ret = NI_LOGAN_RETCODE_FAILURE;
  ni_logan_encoder_params_t *p_param = &s->api_param; //dec params in union with enc params struct

  ret = ni_logan_device_session_close(&s->api_ctx, s->eos, NI_LOGAN_DEVICE_TYPE_DECODER);
  if (NI_LOGAN_RETCODE_SUCCESS != ret)
  {
    av_log(avctx, AV_LOG_ERROR, "Failed to close Decode Session (status = %d)\n", ret);
  }

  if (p_param->dec_input_params.hwframes)
  {
    av_log(avctx, AV_LOG_VERBOSE, "File BLK handle %d close suspended to frames Uninit\n", s->api_ctx.blk_io_handle); //suspended_device_handle
    ctx = (AVHWFramesContext*)avctx->hw_frames_ctx->data;
    dst_ctx = ctx->internal->priv;
    dst_ctx->suspended_device_handle = s->api_ctx.blk_io_handle;

#ifdef __linux__
    ni_logan_device_close(s->api_ctx.device_handle);
#endif
  }
  else
  {
#ifdef _WIN32
    ni_logan_device_close(s->api_ctx.device_handle);
#elif __linux__
    ni_logan_device_close(s->api_ctx.device_handle);
    ni_logan_device_close(s->api_ctx.blk_io_handle);
#endif
  }

  s->api_ctx.device_handle = NI_INVALID_DEVICE_HANDLE;
  s->api_ctx.blk_io_handle = NI_INVALID_DEVICE_HANDLE;

  return 0;
}

/*!******************************************************************************
 *  \brief  Extract custom sei payload data from AVPacket,
 *  and save it to ni_logan_packet_t
 *
 *  \param AVCodecContext *avctx - avcodec context
 *  \param AVPacket *pkt - AVPacket
 *  \param int start_index - pkt data index of custom sei first byte after SEI type
 *  \param ni_logan_packet_t *p_packet - netint internal packet
 *  \param uint8_t sei_type - type of SEI
 *  \param int got_slice - whether got vcl in the pkt data, 1 means got
 *
 *  \return - 0 on success, non-0 on failure
 *******************************************************************************/
static int ff_xcoder_logan_extract_custom_sei(AVCodecContext *avctx, AVPacket *pkt, int start_index,
                                              ni_logan_packet_t *p_packet, uint8_t sei_type, int got_slice)
{
  int i;
  uint8_t *udata;
  uint8_t *sei_data;
  int len = 0;
  int sei_size = 0; // default is 0
  int index = start_index;
  int sei_index = 0;

  av_log(avctx, AV_LOG_TRACE, "%s() enter\n", __FUNCTION__);
  if (p_packet->p_all_custom_sei == NULL)
  {
    /* max size */
    p_packet->p_all_custom_sei = (ni_logan_all_custom_sei_t *)malloc(sizeof(ni_logan_all_custom_sei_t));
    if (p_packet->p_all_custom_sei == NULL)
    {
      av_log(avctx, AV_LOG_ERROR, "failed to allocate all custom sei buffer.\n");
      return AVERROR(ENOMEM);
    }
    memset(p_packet->p_all_custom_sei, 0, sizeof(ni_logan_custom_sei_t));
  }

  sei_index = p_packet->p_all_custom_sei->custom_sei_cnt;
  if (sei_index >= NI_LOGAN_MAX_CUSTOM_SEI_CNT)
  {
    av_log(avctx, AV_LOG_WARNING, "number of custom sei in current frame is out of limit(%d).\n",
           NI_LOGAN_MAX_CUSTOM_SEI_CNT);
    return AVERROR(EINVAL);
  }
  sei_data = p_packet->p_all_custom_sei->ni_custom_sei[sei_index].custom_sei_data;

  /*! extract SEI payload size.
   *  the first byte after SEI type is the SEI payload size.
   *  if the first byte is 255(0xFF), it means the SEI payload size is more than 255.
   *  in this case, to get the SEI payload size is to do a summation.
   *  the end of SEI size is the first non-0xFF value.
   *  for example, 0xFF 0xFF 0x08, the SEI payload size equals to (0xFF+0xFF+0x08).
   */
  while ((index < pkt->size) && (pkt->data[index] == 255))
  {
    sei_size += pkt->data[index++];
  }

  if (index >= pkt->size)
  {
    av_log(avctx, AV_LOG_WARNING, "custom sei corrupted: length truncated.\n");
    return AVERROR(EINVAL);
  }
  sei_size += pkt->data[index++];

  /* check sei size*/
  if (sei_size > NI_LOGAN_MAX_CUSTOM_SEI_SZ)
  {
    av_log(avctx, AV_LOG_WARNING, "custom sei corrupted: size(%d) out of limit(%d).\n",
           sei_size, NI_LOGAN_MAX_CUSTOM_SEI_SZ);
    return AVERROR(EINVAL);
  }

  udata = &pkt->data[index];

  /* extract SEI payload data
   * SEI payload data in NAL is EBSP(Encapsulated Byte Sequence Payload),
   * need change EBSP to RBSP(Raw Byte Sequence Payload) for exact size
  */
  for (i = 0; (i < (pkt->size - index)) && len < sei_size; i++)
  {
    /* if the latest 3-byte data pattern matchs '00 00 03' which means udata[i] is an escaping byte,
     * discard udata[i]. */
    if (i >= 2 && udata[i - 2] == 0 && udata[i - 1] == 0 && udata[i] == 3)
    {
      continue;
    }
    sei_data[len++] = udata[i];
  }

  if (len != sei_size)
  {
    av_log(avctx, AV_LOG_WARNING, "custom sei corrupted: data truncated, "
           "requied size:%d, actual size:%d.\n", sei_size, len);
    return AVERROR(EINVAL);
  }

  p_packet->p_all_custom_sei->ni_custom_sei[sei_index].custom_sei_size = sei_size;
  p_packet->p_all_custom_sei->ni_custom_sei[sei_index].custom_sei_type = sei_type;
  if (got_slice)
  {
    p_packet->p_all_custom_sei->ni_custom_sei[sei_index].custom_sei_loc = NI_LOGAN_CUSTOM_SEI_LOC_AFTER_VCL;
  }
  else
  {
    p_packet->p_all_custom_sei->ni_custom_sei[sei_index].custom_sei_loc = NI_LOGAN_CUSTOM_SEI_LOC_BEFORE_VCL;
  }
  p_packet->p_all_custom_sei->custom_sei_cnt ++;
  av_log(avctx, AV_LOG_TRACE, "%s() exit, custom_sei_cnt=%d, size=%d type=%d\n",
         __FUNCTION__, p_packet->p_all_custom_sei->custom_sei_cnt, sei_size, sei_type);

  return 0;
}

/*!******************************************************************************
 *  \brief  detect custom SEI payload data in AVPacket data,
 *          custom SEI has two meanings:
 *          a. the SEI type is not in the standard protocol, which is added by customes,
 *             for example SEI type 100, note that custom SEI is not user data unregistered SEI.
 *          b. the SEI NAL location does not conform to protocol. It's after VCL NALs.
 *          So there are cases to handle here:
 *          case a: enable custom_sei, detext custom SEIs before VCL.
 *          case b: enable custom_sei and enable_check_packet, detect custom SEIs before VCL,
 *                  and all SEIs after VCL.
 *          all of these SEIs are passthroughed in the same places after encoding.
 *
 *  \param AVCodecContext *avctx - avcodec context
 *  \param XCoderLoganDecContext *s - netint decoder context
 *  \param AVPacket *pkt - AVPacket
 *  \param int start_index - pkt data index of custom sei first byte after SEI type
 *  \param ni_logan_packet_t *p_packet - netint internal packet
 *
 *  \return - 0 on success or not detect correct custom sei, non-0 on failure
 *******************************************************************************/
static int ff_xcoder_logan_detect_custom_sei(AVCodecContext *avctx, XCoderLoganDecContext *s,
                                             AVPacket *pkt, ni_logan_packet_t *p_packet)
{
  int ret = 0;
  const uint8_t *ptr = NULL;
  const uint8_t *end = NULL;
  uint8_t custom_sei_type = s->custom_sei;
  uint8_t chk_pkt = s->enable_check_packet | s->api_param.dec_input_params.check_packet;
  uint8_t nalu_type;
  uint8_t sei_type;
  uint32_t stc = -1;
  int got_slice = 0;

  if(s->api_param.dec_input_params.custom_sei_passthru != NI_LOGAN_INVALID_SEI_TYPE)
  {
    custom_sei_type = s->api_param.dec_input_params.custom_sei_passthru;
  }
  av_log(avctx, AV_LOG_TRACE, "%s(): custom SEI type %d\n", __FUNCTION__, custom_sei_type);

  if (!pkt->data || !avctx)
  {
    return ret;
  }

  // search custom sei in the lone buffer
  // if there is a custom sei in the lone sei, the firmware can't recoginze it.
  // passthrough the custom sei here.
  if (s->api_ctx.lone_sei_size)
  {
    av_log(avctx, AV_LOG_TRACE, "%s(): detect in lone SEI, size=%d\n",
           __FUNCTION__, s->api_ctx.lone_sei_size);
    ptr = s->api_ctx.buf_lone_sei;
    end = s->api_ctx.buf_lone_sei + s->api_ctx.lone_sei_size;
    stc = -1;
    ptr = avpriv_find_start_code(ptr, end, &stc);
    while (ptr < end)
    {
      if (avctx->codec_id == AV_CODEC_ID_H264)
      { // if h264
        nalu_type = stc & 0x1F;
        sei_type = *ptr;
        if ((nalu_type == H264_NAL_SEI) && (sei_type == custom_sei_type))
        {
          /* extract SEI payload, store in ni_logan_packet and pass to libxcoder. */
          ret = ff_xcoder_logan_extract_custom_sei(avctx, pkt, ptr + 1 - pkt->data, p_packet, sei_type, got_slice);
          if (ret == AVERROR(ENOMEM))
          {
            return ret;
          }
          else if (ret != 0)
          {
            if ((p_packet->p_all_custom_sei) && (p_packet->p_all_custom_sei->custom_sei_cnt == 0))
            {
              free(p_packet->p_all_custom_sei);
              p_packet->p_all_custom_sei = NULL;
            }
            return 0;
          }
        }
      }
      else if (avctx->codec_id == AV_CODEC_ID_HEVC)
      { //if hevc
        nalu_type = (stc >> 1) & 0x3F;
        sei_type = *(ptr + 1);
        // check nalu_type, check nuh_temporal_id_plus1 = 1, check sei_pype
        if ((nalu_type == HEVC_NAL_SEI_PREFIX) && (*ptr == 1) && (sei_type == custom_sei_type))
        {
          /* extract SEI payload, store in ni_logan_packet and pass to libxcoder. */
          ret = ff_xcoder_logan_extract_custom_sei(avctx, pkt, ptr + 2 - pkt->data, p_packet, sei_type, got_slice);
          if (ret == AVERROR(ENOMEM))
          {
            return ret;
          }
          else if (ret != 0)
          {
            if ((p_packet->p_all_custom_sei) && (p_packet->p_all_custom_sei->custom_sei_cnt == 0))
            {
              free(p_packet->p_all_custom_sei);
              p_packet->p_all_custom_sei = NULL;
            }
            return 0;
          }
        }
      }
      else
      {
        av_log(avctx, AV_LOG_ERROR, "%s wrong codec %d !\n", __FUNCTION__,
               avctx->codec_id);
        break;
      }

      stc = -1;
      ptr = avpriv_find_start_code(ptr, end, &stc);
    }
  }

  // search custom sei in the packet
  av_log(avctx, AV_LOG_TRACE, "%s(): detect in packet, size=%d\n",
         __FUNCTION__, pkt->size);
  ptr = pkt->data;
  end = pkt->data + pkt->size;
  stc = -1;
  ptr = avpriv_find_start_code(ptr, end, &stc);
  while (ptr < end)
  {
    if (avctx->codec_id == AV_CODEC_ID_H264)
    { // if h264
      nalu_type = stc & 0x1F;
      sei_type = *ptr;
      if ((nalu_type == H264_NAL_SEI) &&
          ((sei_type == custom_sei_type) || (got_slice && chk_pkt)))
      {
        /* extract SEI payload, store in ni_logan_packet and pass to libxcoder. */
        ret = ff_xcoder_logan_extract_custom_sei(avctx, pkt, ptr + 1 - pkt->data, p_packet, sei_type, got_slice);
        if (ret == AVERROR(ENOMEM))
        {
          return ret;
        }
        else if (ret != 0)
        {
          return 0;
        }
      }
      else if ((nalu_type >= H264_NAL_SLICE) && (nalu_type <= H264_NAL_IDR_SLICE)) //VCL
      {
        ret = 0;
        got_slice = 1;
        /* if disable check packet and VCL is found, then stop searching for SEI after VCL. */
        if (!chk_pkt)
        {
          break;
        }
      }
    }
    else if (avctx->codec_id == AV_CODEC_ID_HEVC)
    { //if hevc
      nalu_type = (stc >> 1) & 0x3F;
      sei_type = *(ptr + 1);
      // check nalu_type, check nuh_temporal_id_plus1 = 1, check sei_pype
      // if enable chk_pkt, continue search SEI after VCL
      if ((nalu_type == HEVC_NAL_SEI_PREFIX) && (*ptr == 1) &&
          ((sei_type == custom_sei_type) || (got_slice && chk_pkt)))
      {
        /* extract SEI payload, store in ni_logan_packet and pass to libxcoder. */
        ret = ff_xcoder_logan_extract_custom_sei(avctx, pkt, ptr + 2 - pkt->data, p_packet, sei_type, got_slice);
        if (ret == AVERROR(ENOMEM))
        {
          return ret;
        }
        else if (ret != 0)
        {
          return 0;
        }
      }
      else if (nalu_type >= HEVC_NAL_TRAIL_N && nalu_type <= HEVC_NAL_RSV_VCL31) //found VCL
      {
        ret = 0;
        got_slice = 1;
        /* if disable check packet and VCL is found, then stop searching for SEI after VCL. */
        if (!chk_pkt)
        {
          break;
        }
      }
    }
    else
    {
      av_log(avctx, AV_LOG_ERROR, "%s wrong codec %d !\n", __FUNCTION__,
             avctx->codec_id);
      break;
    }

    stc = -1;
    ptr = avpriv_find_start_code(ptr, end, &stc);
  }

  if (p_packet->p_all_custom_sei)
  {
    av_log(avctx, AV_LOG_TRACE, "%s(): total custom SEI number %d\n", __FUNCTION__,
          p_packet->p_all_custom_sei->custom_sei_cnt);
  }
  else
  {
    av_log(avctx, AV_LOG_TRACE, "%s(): no custom SEI detected\n", __FUNCTION__);
  }

  return ret;
}

// return 1 if need to prepend saved header to pkt data, 0 otherwise
int ff_xcoder_logan_add_headers(AVCodecContext *avctx, AVPacket *pkt,
                                uint8_t *extradata, int extradata_size)
{
  XCoderLoganDecContext *s = avctx->priv_data;
  int ret = 0;
  const uint8_t *ptr = pkt->data;
  const uint8_t *end = pkt->data + pkt->size;
  uint32_t stc = -1;
  uint8_t nalu_type = 0;

  if (!pkt->data || !avctx)
  {
    return ret;
  }

  while (ptr < end)
  {
    stc = -1;
    ptr = avpriv_find_start_code(ptr, end, &stc);
    if (ptr == end)
    {
      break;
    }

    if (AV_CODEC_ID_H264 == avctx->codec_id)
    {
      nalu_type = stc & 0x1f;

      // update extra data on sequence change by resetting got_first_key_frame
      if (s->got_first_key_frame && pkt->flags & AV_PKT_FLAG_KEY)
      {
        if (s->extradata_size != extradata_size ||
            memcmp(s->extradata, extradata, s->extradata_size))
        {
          s->got_first_key_frame = 0;
        }
      }

      // Find the first packet containing key frame
      if (!s->got_first_key_frame && pkt->flags & AV_PKT_FLAG_KEY)
      {
        free(s->extradata);
        s->extradata = malloc(extradata_size);
        if (!s->extradata)
        {
          av_log(avctx, AV_LOG_ERROR, "%s memory allocation failed !\n", __FUNCTION__);
          ret = 0;
          break;
        }
        av_log(avctx, AV_LOG_TRACE, "%s size %d\n", __FUNCTION__, extradata_size);
        memcpy(s->extradata, extradata, extradata_size);
        s->extradata_size = extradata_size;
        s->got_first_key_frame = 1;
        ret = 1;
        break;
      }

      // If SPS/PPS already exists, no need to prepend it again;
      // we use one of the header info to simplify the checking.
      if (H264_NAL_SPS == nalu_type || H264_NAL_PPS == nalu_type)
      {
        // save the header if not done yet for subsequent comparison
        if (! s->extradata_size || ! s->extradata)
        {
          s->extradata = malloc(extradata_size);
          if (! s->extradata)
          {
            av_log(avctx, AV_LOG_ERROR, "%s memory allocation failed !\n",
                   __FUNCTION__);
            ret = 0;
            break;
          }
          av_log(avctx, AV_LOG_TRACE, "%s size %d\n", __FUNCTION__, extradata_size);
          memcpy(s->extradata, extradata, extradata_size);
          s->extradata_size = extradata_size;
        }
        s->got_first_key_frame = 1;
        ret = 0;
        break;
      }
      else if (nalu_type >= H264_NAL_SLICE && nalu_type <= H264_NAL_IDR_SLICE)
      {
        // VCL types results in no header inserted
        ret = 0;
        break;
      }
    }
    else if (AV_CODEC_ID_HEVC == avctx->codec_id)
    {
      nalu_type = (stc >> 1) & 0x3F;

      // IRAP picture types include: BLA, CRA, IDR and IRAP reserve types,
      // 16-23, and insert header in front of IRAP at start or if header changes
      if (s->got_first_key_frame && pkt->flags & AV_PKT_FLAG_KEY)
      {
        if (s->extradata_size != extradata_size ||
            memcmp(s->extradata, extradata, s->extradata_size))
        {
          s->got_first_key_frame = 0;
        }
      }

      if (! s->got_first_key_frame && pkt->flags & AV_PKT_FLAG_KEY)
      {
        free(s->extradata);
        s->extradata = malloc(extradata_size);
        if (! s->extradata)
        {
          av_log(avctx, AV_LOG_ERROR, "%s memory allocation failed !\n",
                 __FUNCTION__);
          ret = 0;
          break;
        }
        av_log(avctx, AV_LOG_TRACE, "%s size %d\n", __FUNCTION__, extradata_size);
        memcpy(s->extradata, extradata, extradata_size);
        s->extradata_size = extradata_size;
        s->got_first_key_frame = 1;
        ret = 1;
        break;
      }

      // when header (VPS/SPS/PPS) already exists, no need to prepend it again;
      // we use one of the header info to simplify the checking.
      if (HEVC_NAL_VPS == nalu_type || HEVC_NAL_SPS == nalu_type ||
          HEVC_NAL_PPS == nalu_type)
      {
        // save the header if not done yet for subsequent comparison
        if (! s->extradata_size || ! s->extradata)
        {
          s->extradata = malloc(extradata_size);
          if (! s->extradata)
          {
            av_log(avctx, AV_LOG_ERROR, "%s memory allocation failed !\n",
                   __FUNCTION__);
            ret = 0;
            break;
          }
          av_log(avctx, AV_LOG_TRACE, "%s size %d\n", __FUNCTION__, extradata_size);
          memcpy(s->extradata, extradata, extradata_size);
          s->extradata_size = extradata_size;
        }
        s->got_first_key_frame = 1;
        ret = 0;
        break;
      }
      else if (nalu_type >= HEVC_NAL_TRAIL_N && nalu_type <= HEVC_NAL_RSV_VCL31)
      {
        // VCL types results in no header inserted
        ret = 0;
        break;
      }
    }
    else
    {
      av_log(avctx, AV_LOG_ERROR, "%s wrong codec %d!\n", __FUNCTION__, avctx->codec_id);
      break;
    }
  }

  return ret;
}

// check if the packet is SEI only, 1 yes, 0 otherwise
// check if getting the header of streams in decoder low delay mode, and update its value
// check the new sequence headers and cache them.
static int xcoder_logan_packet_parse(AVCodecContext *avctx, XCoderLoganDecContext *s,
                                     AVPacket *pkt, ni_logan_packet_t *p_packet)
{
  int pkt_sei_alone = 0;
  int got_slice = 0;
  int low_delay = (s->low_delay == 0)?s->api_param.dec_input_params.lowdelay:s->low_delay;
  int pkt_nal_bitmap = 0;
  int chk_pkt = s->enable_check_packet | s->api_param.dec_input_params.check_packet;
  const uint8_t *ptr = pkt->data;
  const uint8_t *end = pkt->data + pkt->size;
  uint32_t stc = -1;
  uint8_t nalu_type = 0;
  int nalu_count = 0;

  if (!pkt->data || !pkt->size || !avctx)
  {
    return pkt_sei_alone;
  }

  if (s->pkt_nal_bitmap & NI_LOGAN_GENERATE_ALL_NAL_HEADER_BIT)
  {
    av_log(avctx, AV_LOG_TRACE, "%s(): already find the header of streams.\n", __FUNCTION__);
    low_delay = 0;
  }

  pkt_sei_alone = 1;
  while ((pkt_sei_alone || low_delay || chk_pkt) && (ptr < end))
  {
    stc = -1;
    ptr = avpriv_find_start_code(ptr, end, &stc);
    if (ptr == end)
    {
      if (0 == nalu_count)
      {
        pkt_sei_alone = 0;
        av_log(avctx, AV_LOG_TRACE, "%s(): no NAL found in pkt.\n", __FUNCTION__);
      }
      break;
    }
    nalu_count++;

    if (AV_CODEC_ID_H264 == avctx->codec_id)
    {
      nalu_type = stc & 0x1F;

      //check whether the packet is sei alone
      pkt_sei_alone = (pkt_sei_alone && H264_NAL_SEI == nalu_type);

      //check whether the packet contains SEI NAL after VCL NAL units
      if (got_slice && (H264_NAL_SEI == nalu_type))
      {
        chk_pkt = 0;
        // 5 bytes = 3 bytes start code + 1 byte nal type(0x06) + 1 byte sei type
        p_packet->len_of_sei_after_vcl = (end - ptr) + 5;
        av_log(avctx, AV_LOG_TRACE, "%s(): found SEI NAL after VCL NAL, len = %d.\n",
               __FUNCTION__, p_packet->len_of_sei_after_vcl);
      }
      else if ((nalu_type >= H264_NAL_SLICE) && (nalu_type <= H264_NAL_IDR_SLICE)) //VCL
      {
        got_slice = 1;
      }

      // FFmpeg stores a complete frame in each AVPacket. A complete frame
      // includes headers, SEIs, video slices, and other NALs such as access
      // unit delimiters, etc. The decoder expects a complete frame for each
      // packet writing to the decoder. Otherwise it can cause all sorts of
      // problems. However in some cases the first NALU could be unit delimiter
      // so we need to force to check NALU split to collect all the headers in
      // case of decoder VPU recovery.
      switch (nalu_type)
      {
        case H264_NAL_SPS:
          chk_pkt = 1;
          pkt_nal_bitmap |= NI_LOGAN_NAL_SPS_BIT;
          break;
        case H264_NAL_PPS:
          chk_pkt = 1;
          pkt_nal_bitmap |= NI_LOGAN_NAL_PPS_BIT;
          break;
        case H264_NAL_SEI:
        case H264_NAL_AUD:
          chk_pkt = 1;
          break;
        default:
          chk_pkt = s->enable_check_packet | s->api_param.dec_input_params.check_packet;
          break;
      }

      // Set decoder low delay mode one-time.
      if (pkt_nal_bitmap & (NI_LOGAN_NAL_SPS_BIT | NI_LOGAN_NAL_PPS_BIT))
      {
        av_log(avctx, AV_LOG_TRACE, "%s(): Detect SPS, PPS and IDR, enable "
               "decoder low delay mode.\n", __FUNCTION__);
        pkt_nal_bitmap |= NI_LOGAN_GENERATE_ALL_NAL_HEADER_BIT;
        if (low_delay)
        {
          s->api_ctx.decoder_low_delay = low_delay;
          low_delay = 0;
        }

        // Update cached packet including SPS+PPS+IDR slice. A complete frame
        // needs to be cached for stream with intraPeriod = 0
        if (pkt != &s->seq_hdr_pkt && got_slice)
        {
          av_packet_unref(&s->seq_hdr_pkt);
          av_packet_ref(&s->seq_hdr_pkt, pkt);
        }
      }
    }
    else if (AV_CODEC_ID_HEVC == avctx->codec_id)
    {
      nalu_type = (stc >> 1) & 0x3F;

      //check whether the packet is sei alone
      pkt_sei_alone = (pkt_sei_alone && (HEVC_NAL_SEI_PREFIX == nalu_type ||
                                         HEVC_NAL_SEI_SUFFIX == nalu_type));

      //check whether the packet contains SEI NAL after VCL NAL units
      if (got_slice && (HEVC_NAL_SEI_PREFIX == nalu_type || HEVC_NAL_SEI_SUFFIX == nalu_type))
      {
        chk_pkt = 0;
        // 5 bytes = 3 bytes start code + 2 bytes nal type(0x4e 0x01)
        p_packet->len_of_sei_after_vcl = (end - ptr) + 5;
        av_log(avctx, AV_LOG_TRACE, "%s(): found SEI NAL after VCL NAL, len = %d.\n",
               __FUNCTION__, p_packet->len_of_sei_after_vcl);
      }
      else if ((nalu_type >= HEVC_NAL_TRAIL_N) && (nalu_type <= HEVC_NAL_RSV_VCL31)) //VCL
      {
        got_slice = 1;
      }

      // FFmpeg stores a complete frame in each AVPacket. A complete frame
      // includes headers, SEIs, video slices, and other NALs such as access
      // unit delimiters, etc. The decoder expects a complete frame for each
      // packet writing to the decoder. Otherwise it can cause all sorts of
      // problems. However in some cases the first NALU could be unit delimiter
      // so we need to force to check NALU split to collect all the headers in
      // case of decoder VPU recovery.
      switch (nalu_type)
      {
        case HEVC_NAL_VPS:
          chk_pkt = 1;
          pkt_nal_bitmap |= NI_LOGAN_NAL_VPS_BIT;
          break;
        case HEVC_NAL_SPS:
          chk_pkt = 1;
          pkt_nal_bitmap |= NI_LOGAN_NAL_SPS_BIT;
          break;
        case HEVC_NAL_PPS:
          chk_pkt = 1;
          pkt_nal_bitmap |= NI_LOGAN_NAL_PPS_BIT;
          break;
        case HEVC_NAL_AUD:
        case HEVC_NAL_EOS_NUT:
        case HEVC_NAL_EOB_NUT:
        case HEVC_NAL_FD_NUT:
        case HEVC_NAL_SEI_PREFIX:
        case HEVC_NAL_SEI_SUFFIX:
          chk_pkt = 1;
          break;
        default:
          chk_pkt = s->enable_check_packet | s->api_param.dec_input_params.check_packet;
          break;
      }

      // Set decoder low delay mode one-time.
      if (pkt_nal_bitmap & (NI_LOGAN_NAL_VPS_BIT | NI_LOGAN_NAL_SPS_BIT | NI_LOGAN_NAL_PPS_BIT))
      {
        av_log(avctx, AV_LOG_TRACE, "%s(): Detect VPS, SPS, PPS and IDR, "
               "enable decoder low delay mode.\n", __FUNCTION__);
        pkt_nal_bitmap |= NI_LOGAN_GENERATE_ALL_NAL_HEADER_BIT;
        if (low_delay)
        {
          s->api_ctx.decoder_low_delay = low_delay;
          low_delay = 0;
        }

        // Update cached packet including VPS+SPS+PPS+IDR slice. A complete frame
        // needs to be cached for stream with intraPeriod = 0
        if (pkt != &s->seq_hdr_pkt && got_slice)
        {
          av_packet_unref(&s->seq_hdr_pkt);
          av_packet_ref(&s->seq_hdr_pkt, pkt);
        }
      }
    }
    else
    {
      av_log(avctx, AV_LOG_ERROR, "%s() wrong codec %d !\n",
             __FUNCTION__, avctx->codec_id);
      pkt_sei_alone = 0;
      break;
    }
  }

  s->pkt_nal_bitmap |= pkt_nal_bitmap;
  return pkt_sei_alone;
}

int ff_xcoder_logan_dec_send(AVCodecContext *avctx,
                             XCoderLoganDecContext *s,
                             AVPacket *pkt)
{
  /* call ni_logan_decoder_session_write to send compressed video packet to the decoder
     instance */
  int need_draining = 0;
  size_t size;
  ni_logan_packet_t *xpkt = &s->api_pkt.data.packet;
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
    // Once VPU recovery, the s->draining may be lost during session reset. And
    // so is the EOS indicator which might be lost in decoder session read. Here
    // we try to recover EOS indicator so as to return EOF in this calling.
    s->eos |= (s->draining && s->vpu_reset);
  }

  if (s->draining && s->eos)
  {
    av_log(avctx, AV_LOG_DEBUG, "Decoder is draining, eos\n");
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

    memset(xpkt, 0, sizeof(ni_logan_packet_t));
    xpkt->pts = pkt->pts;
    xpkt->dts = pkt->dts;
    xpkt->flags = pkt->flags;
    xpkt->video_width = avctx->width;
    xpkt->video_height = avctx->height;
    xpkt->p_data = NULL;
    xpkt->data_len = pkt->size;
    xpkt->p_all_custom_sei = NULL;
    xpkt->len_of_sei_after_vcl = 0;

    if (extradata_size > 0 && extradata_size != s->extradata_size &&
        ff_xcoder_logan_add_headers(avctx, pkt, extradata, extradata_size))
    {
      if (avctx->extradata_size > s->api_ctx.max_nvme_io_size * 2)
      {
        av_log(avctx, AV_LOG_ERROR, "%s extradata_size %d exceeding max size "
               "supported: %d\n", __FUNCTION__, s->extradata_size,
               s->api_ctx.max_nvme_io_size * 2);
      }
      else
      {
        av_log(avctx, AV_LOG_DEBUG, "%s extradata_size %d copied to pkt start.\n",
               __FUNCTION__, s->extradata_size);
        s->api_ctx.prev_size = s->extradata_size;
        memcpy(s->api_ctx.p_leftover, s->extradata, s->extradata_size);
      }
    }
    if ((s->low_delay || s->api_param.dec_input_params.lowdelay) &&
        s->got_first_key_frame && !(s->pkt_nal_bitmap & NI_LOGAN_GENERATE_ALL_NAL_HEADER_BIT))
    {
      if(s->low_delay)
        s->api_ctx.decoder_low_delay = s->low_delay;
      else
        s->api_ctx.decoder_low_delay = s->api_param.dec_input_params.lowdelay;
      s->pkt_nal_bitmap |= NI_LOGAN_GENERATE_ALL_NAL_HEADER_BIT;
      av_log(avctx, AV_LOG_TRACE, "%s got first IDR in decoder low delay mode, "
             "delay time %dms, pkt_nal_bitmap %d\n", __FUNCTION__, s->api_ctx.decoder_low_delay,
             s->pkt_nal_bitmap);
    }

    // check if the packet is SEI only, save it to be sent with the next data frame.
    // check if getting the header of streams in decoder low delay mode, and update its value.
    // check if new sequence headers come and cache them.
    if (xcoder_logan_packet_parse(avctx, s, pkt, xpkt))
    {
      // skip the packet if it's corrupted and/or exceeding lone SEI buf size
      if (pkt->size + s->api_ctx.lone_sei_size <= NI_LOGAN_MAX_SEI_DATA)
      {
        memcpy(s->api_ctx.buf_lone_sei + s->api_ctx.lone_sei_size,
               pkt->data, pkt->size);
        s->api_ctx.lone_sei_size += pkt->size;
        av_log(avctx, AV_LOG_TRACE, "%s pkt lone SEI, saved, and return %d\n",
               __FUNCTION__, pkt->size);
      }
      else
      {
        av_log(avctx, AV_LOG_WARNING, "lone SEI size %d > buf size %ld, "
               "corrupted? skipped ..\n", pkt->size, NI_LOGAN_MAX_SEI_DATA);
      }

      xpkt->data_len = 0;
      return pkt->size;
    }

    if (s->custom_sei != NI_LOGAN_INVALID_SEI_TYPE || s->api_param.dec_input_params.custom_sei_passthru != NI_LOGAN_INVALID_SEI_TYPE)
    {
      ret = ff_xcoder_logan_detect_custom_sei(avctx, s, pkt, xpkt);
      if (ret != 0)
      {
        goto fail;
      }
    }

    // embed lone SEI saved previously (if any) to send to decoder
    if (s->api_ctx.lone_sei_size)
    {
      av_log(avctx, AV_LOG_TRACE, "%s copy over lone SEI data size: %d\n",
             __FUNCTION__, s->api_ctx.lone_sei_size);
      memcpy((uint8_t *)s->api_ctx.p_leftover + s->api_ctx.prev_size,
             s->api_ctx.buf_lone_sei, s->api_ctx.lone_sei_size);
      s->api_ctx.prev_size += s->api_ctx.lone_sei_size;
      s->api_ctx.lone_sei_size = 0;
    }

    if ((pkt->size + s->api_ctx.prev_size) > 0)
    {
      ni_logan_packet_buffer_alloc(xpkt, (pkt->size + s->api_ctx.prev_size - xpkt->len_of_sei_after_vcl));
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

  av_log(avctx, AV_LOG_DEBUG, "%s: pkt->size=%d\n", __FUNCTION__, pkt->size);

  if (s->started == 0)
  {
    xpkt->start_of_stream = 1;
    s->started = 1;
  }

  if (need_draining && !s->draining)
  {
    av_log(avctx, AV_LOG_DEBUG, "Sending End Of Stream signal\n");
    xpkt->end_of_stream = 1;
    xpkt->data_len = 0;

    av_log(avctx, AV_LOG_TRACE, "ni_logan_packet_copy before: size=%d, s->prev_size=%d, send_size=%d, "
           "len_of_sei_after_slice=%d (end of stream)\n",
           pkt->size, s->api_ctx.prev_size, send_size, xpkt->len_of_sei_after_vcl);
    if (new_packet)
    {
      extra_prev_size = s->api_ctx.prev_size;
      send_size = ni_logan_packet_copy(xpkt->p_data, pkt->data, (pkt->size - xpkt->len_of_sei_after_vcl),
                                 s->api_ctx.p_leftover, &s->api_ctx.prev_size);
      // increment offset of data sent to decoder and save it
      xpkt->pos = s->offset;
      if (s->api_ctx.is_dec_pkt_512_aligned)
      {
        s->offset += send_size;
      }
      else
      {
        s->offset += pkt->size + extra_prev_size;
      }
    }
    av_log(avctx, AV_LOG_TRACE, "ni_logan_packet_copy after: size=%d, s->prev_size=%d, send_size=%d, "
           "xpkt->data_len=%d, len_of_sei_after_slice=%d (end of stream)\n",
           pkt->size, s->api_ctx.prev_size, send_size, xpkt->data_len, xpkt->len_of_sei_after_vcl);

    if (send_size < 0)
    {
      av_log(avctx, AV_LOG_ERROR, "Failed to copy pkt (status = %d)\n",
             send_size);
      ret = AVERROR_EXTERNAL;
      goto fail;
    }
    if (s->api_ctx.is_dec_pkt_512_aligned)
    {
      xpkt->data_len = send_size;
    }
    else
    {
      xpkt->data_len += extra_prev_size;
    }

    sent = 0;
    if (xpkt->data_len > 0)
    {
      sent = ni_logan_device_session_write(&(s->api_ctx), &(s->api_pkt), NI_LOGAN_DEVICE_TYPE_DECODER);
    }
    if (sent < 0)
    {
      av_log(avctx, AV_LOG_ERROR, "Failed to send eos signal (status = %d)\n",
             sent);
      if (NI_LOGAN_RETCODE_ERROR_VPU_RECOVERY == sent)
      {
        ret = xcoder_logan_decode_reset(avctx);
        if (0 == ret)
        {
          ret = AVERROR(EAGAIN);
        }
      }
      else
      {
        ret = AVERROR_EOF;
      }
      goto fail;
    }
    av_log(avctx, AV_LOG_DEBUG, "Queued eos (status = %d) ts=%llu\n",
           sent, xpkt->pts);
    s->draining = 1;
    s->vpu_reset = 0;

    ni_logan_device_session_flush(&(s->api_ctx), NI_LOGAN_DEVICE_TYPE_DECODER);
  }
  else
  {
#if 0
    if (pkt->pts == AV_NOPTS_VALUE)
      av_log(avctx, AV_LOG_DEBUG, "DEC avpkt pts : NOPTS size %d  pos %lld \n",
       pkt->size,  pkt->pos);
    else
      av_log(avctx, AV_LOG_DEBUG, "DEC avpkt pts : %lld  dts : %lld  size %d  pos %lld \n", pkt->pts, pkt->dts, pkt->size,
       pkt->pos);
#endif
    av_log(avctx, AV_LOG_TRACE, "ni_logan_packet_copy before: size=%d, s->prev_size=%d, send_size=%d, len_of_sei_after_slice=%d\n",
           pkt->size, s->api_ctx.prev_size, send_size, xpkt->len_of_sei_after_vcl);
    if (new_packet)
    {
      extra_prev_size = s->api_ctx.prev_size;
      send_size = ni_logan_packet_copy(xpkt->p_data, pkt->data, (pkt->size - xpkt->len_of_sei_after_vcl),
                                 s->api_ctx.p_leftover, &s->api_ctx.prev_size);
      // increment offset of data sent to decoder and save it
      xpkt->pos = s->offset;
      if (s->api_ctx.is_dec_pkt_512_aligned)
      {
        s->offset += send_size;
      }
      else
      {
        s->offset += pkt->size + extra_prev_size;
      }
    }
    av_log(avctx, AV_LOG_TRACE, "ni_logan_packet_copy after: size=%d, s->prev_size=%d, send_size=%d, "
           "xpkt->data_len=%d, len_of_sei_after_slice=%d\n",
           pkt->size, s->api_ctx.prev_size, send_size, xpkt->data_len, xpkt->len_of_sei_after_vcl);

    if (send_size < 0)
    {
      av_log(avctx, AV_LOG_ERROR, "Failed to copy pkt (status = %d)\n",
             send_size);
      ret = AVERROR_EXTERNAL;
      goto fail;
    }
    if (s->api_ctx.is_dec_pkt_512_aligned)
    {
      xpkt->data_len = send_size;
    }
    else
    {
      xpkt->data_len += extra_prev_size;
    }

    sent = 0;
    if (xpkt->data_len > 0)
    {
      sent = ni_logan_device_session_write(&s->api_ctx, &(s->api_pkt), NI_LOGAN_DEVICE_TYPE_DECODER);
      av_log(avctx, AV_LOG_DEBUG, "%s pts=%" PRIi64 ", dts=%" PRIi64 ", "
             "pos=%" PRIi64 ", sent=%d\n", __FUNCTION__, pkt->pts, pkt->dts,
             pkt->pos, sent);
    }
    if (sent < 0)
    {
      av_log(avctx, AV_LOG_ERROR, "Failed to send compressed pkt (status = "
                                  "%d)\n", sent);
      if (NI_LOGAN_RETCODE_ERROR_VPU_RECOVERY == sent)
      {
        ret = xcoder_logan_decode_reset(avctx);
        if (0 == ret)
        {
          ret = AVERROR(EAGAIN);
        }
      }
      else
      {
        ret = AVERROR_EOF;
      }
      goto fail;
    }
    else if (sent == 0)
    {
      av_log(avctx, AV_LOG_DEBUG, "Queued input buffer size=0\n");
    }
    else if (sent < size)
    {
      /* partial sent; keep trying */
      av_log(avctx, AV_LOG_DEBUG, "Queued input buffer size=%d\n", sent);
    }
  }

  if (sent != 0)
  {
    //keep the current pkt to resend next time
    ni_logan_packet_buffer_free(xpkt);
  }

  if (xpkt->data_len == 0)
  {
    /* if this packet is done sending, free any sei buffer. */
    free(xpkt->p_all_custom_sei);
    xpkt->p_all_custom_sei = NULL;
  }

  if (sent == 0)
  {
    return AVERROR(EAGAIN);
  }

  return sent;

fail:
  ni_logan_packet_buffer_free(xpkt);
  free(xpkt->p_all_custom_sei);
  xpkt->p_all_custom_sei = NULL;

  return ret;
}

int retrieve_logan_frame(AVCodecContext *avctx, AVFrame *data, int *got_frame,
                         ni_logan_frame_t *xfme)
{
  XCoderLoganDecContext *s = avctx->priv_data;

  int buf_size = xfme->data_len[0] + xfme->data_len[1] + xfme->data_len[2] + xfme->data_len[3];
  uint8_t *buf = xfme->p_data[0];
  int stride = 0;
  int res = 0;
  AVHWFramesContext *ctx;
  NILOGANFramesContext *dst_ctx;
  AVFrame *frame = data;
  bool is_hw = xfme->data_len[3] > 0;
  ni_aux_data_t *aux_data = NULL;
  AVFrameSideData *av_side_data = NULL;

  av_log(avctx, AV_LOG_TRACE, "%s: buf %p data_len [%d %d %d %d] buf_size %d\n",
         __FUNCTION__, buf, xfme->data_len[0], xfme->data_len[1],
         xfme->data_len[2], xfme->data_len[3], buf_size);

  if(is_hw)
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
        av_log(avctx, AV_LOG_VERBOSE, "First frame, set hw_frame_context to copy decode sessions threads\n");
        res = ni_logan_device_session_copy(&s->api_ctx, &dst_ctx->api_ctx);
        if (NI_LOGAN_RETCODE_SUCCESS != res)
        {
          return res;
        }
        av_log(avctx, AV_LOG_VERBOSE, "%s: blk_io_handle %d device_handle %d\n",
               __FUNCTION__, s->api_ctx.blk_io_handle, s->api_ctx.device_handle);
      }
    }
  }

  av_log(avctx, AV_LOG_DEBUG, "decoding %" PRId64 " frame ...\n", s->api_ctx.frame_num);

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

  av_log(avctx, AV_LOG_DEBUG, "XFRAME SIZE: %d, STRIDE: %d\n", buf_size, stride);

  if (!is_hw && (stride == 0 || buf_size < stride * avctx->height))
  {
    av_log(avctx, AV_LOG_ERROR, "Packet too small (%d)\n", buf_size);
    return AVERROR_INVALIDDATA;
  }

  frame->key_frame = 0;
  switch (xfme->ni_logan_pict_type)
  {
  case LOGAN_PIC_TYPE_I:
    frame->pict_type = AV_PICTURE_TYPE_I;
    break;
  case LOGAN_PIC_TYPE_IDR:
  case LOGAN_PIC_TYPE_CRA:
    frame->pict_type = AV_PICTURE_TYPE_I;
    frame->key_frame = 1;
    break;
  case LOGAN_PIC_TYPE_P:
      frame->pict_type = AV_PICTURE_TYPE_P;
      break;
  case LOGAN_PIC_TYPE_B:
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
    frame->buf[0] = av_buffer_create(buf, buf_size, ni_logan_frame_free,
                                     (void *) s->api_ctx.event_handle, 0);
  }
  else
  {
    frame->buf[0] = av_buffer_create(buf, buf_size, ni_logan_buf_pool_free, xfme->dec_buf, 0);
  }

  buf = frame->buf[0]->data;

  // retrieve side data if available
  ni_logan_dec_retrieve_aux_data(xfme);

#ifdef NI_DEC_GSTREAMER_SUPPORT
  int i;
  // retrieve the GStreamer data based on frame's packet offset
  if (0 == s->api_ctx.frame_pkt_offset)
  {
      frame->opaque = s->gs_data[0].opaque;
      frame->buf[1] = s->gs_data[0].buf0;
      s->gs_data[0].opaque = NULL;
      s->gs_data[0].buf0 = NULL;

      av_log(avctx, AV_LOG_DEBUG, "pos 0 pkt opaque %p buf0 %p retrieved\n",
             frame->opaque, frame->buf[1]);
  }
  else
  {
      for (i = 0; i < NI_LOGAN_FIFO_SZ; i++)
      {
          if (s->api_ctx.frame_pkt_offset >= s->gs_opaque_offsets_index_min[i]
              && s->api_ctx.frame_pkt_offset < s->gs_opaque_offsets_index[i])
          {
              frame->opaque = s->gs_data[i].opaque;
              frame->buf[1] = s->gs_data[i].buf0;
              s->gs_data[i].opaque = NULL;
              s->gs_data[i].buf0 = NULL;

              av_log(avctx, AV_LOG_DEBUG, "pos %d pkt opaque %p buf0 %p retrieved\n",
                     i, frame->opaque, frame->buf[1]);
              break;
          }
          if (i == NI_LOGAN_FIFO_SZ -1 )
          {
              av_log(avctx, AV_LOG_ERROR, "ERROR: NO GS opaque found, consider "
                     "increasing NI_LOGAN_FIFO_SZ (%d)!\n", NI_LOGAN_FIFO_SZ);
          }
      }
  }
#endif

  // User Data Unregistered SEI if available
  if ((s->enable_user_data_sei_passthru || s->api_param.dec_input_params.enable_user_data_sei_passthru) &&
      (aux_data = ni_logan_frame_get_aux_data(xfme, NI_FRAME_AUX_DATA_UDU_SEI)))
  {
    av_side_data = av_frame_new_side_data(frame, AV_FRAME_DATA_NETINT_UDU_SEI,
                                          aux_data->size);
    if (! av_side_data)
    {
      return AVERROR(ENOMEM);
    }
    else
    {
      memcpy(av_side_data->data, aux_data->data, aux_data->size);
    }
  }
  // close caption data if available
  if (aux_data = ni_logan_frame_get_aux_data(xfme, NI_FRAME_AUX_DATA_A53_CC))
  {
    av_side_data = av_frame_new_side_data(frame, AV_FRAME_DATA_A53_CC,
                                          aux_data->size);
    if (! av_side_data)
    {
      return AVERROR(ENOMEM);
    }
    else
    {
      memcpy(av_side_data->data, aux_data->data, aux_data->size);
    }
  }

  // hdr10 sei data if available
  if (aux_data = ni_logan_frame_get_aux_data(
        xfme, NI_FRAME_AUX_DATA_MASTERING_DISPLAY_METADATA))
  {
    AVMasteringDisplayMetadata *mdm =
    av_mastering_display_metadata_create_side_data(frame);

    if (! mdm)
    {
      return AVERROR(ENOMEM);
    }
    else
    {
      memcpy(mdm, aux_data->data, aux_data->size);
    }
  }

  if (aux_data = ni_logan_frame_get_aux_data(xfme,
                                       NI_FRAME_AUX_DATA_CONTENT_LIGHT_LEVEL))
  {
    AVContentLightMetadata *clm = 
    av_content_light_metadata_create_side_data(frame);
    if (! clm)
    {
      return AVERROR(ENOMEM);
    }
    else
    {
      memcpy(clm, aux_data->data, aux_data->size);
    }
  }

  // hdr10+ sei data if available
  if (aux_data = ni_logan_frame_get_aux_data(xfme, NI_FRAME_AUX_DATA_HDR_PLUS))
  {
    AVDynamicHDRPlus *hdrp = av_dynamic_hdr_plus_create_side_data(frame);
    if (! hdrp)
    {
      return AVERROR(ENOMEM);
    }
    else
    {
      memcpy(hdrp, aux_data->data, aux_data->size);
    }
  } // hdr10+ sei

  // remember to clean up auxiliary data of ni_logan_frame after their use
  ni_logan_frame_wipe_aux_data(xfme);

  if (xfme->p_custom_sei)
  {
    AVBufferRef *sei_ref = av_buffer_create(xfme->p_custom_sei,
                                            sizeof(ni_logan_all_custom_sei_t),
                                            ni_logan_free, NULL, 0);
    if (! sei_ref ||
        ! av_frame_new_side_data_from_buf(frame, AV_FRAME_DATA_NETINT_CUSTOM_SEI,
                                    sei_ref))
    {
        return AVERROR(ENOMEM);
    }
    xfme->p_custom_sei = NULL;
  }

  frame->pkt_dts = xfme->dts;
  frame->pts = xfme->pts;
  if (xfme->pts != NI_LOGAN_NOPTS_VALUE)
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
    ni_logan_hwframe_surface_t* p_data3;
    p_data3 = (ni_logan_hwframe_surface_t*)((uint8_t*)xfme->p_buffer
                                + xfme->data_len[0] + xfme->data_len[1]
                                + xfme->data_len[2]);
    frame->data[3] = (uint8_t*) p_data3;
    av_log(avctx, AV_LOG_DEBUG, "%s: OUT0 data[3] i8FrameIdx=%d, device_handle=%ld"
           " bitdep=%d, WxH %d x %d\n", __FUNCTION__,
           p_data3->i8FrameIdx,
           p_data3->device_handle,
           p_data3->bit_depth,
           p_data3->ui16width,
           p_data3->ui16height);
  }

  av_log(avctx, AV_LOG_DEBUG, "%s: frame->buf[0]=%p, "
         "frame->data=%p, frame->pts=%" PRId64 ", frame size=%d, "
         "s->current_pts=%" PRId64 ", frame->pkt_pos=%" PRId64 ", "
         "frame->pkt_duration=%" PRId64 " sei size %d offset %u\n",
         __FUNCTION__, frame->buf[0], frame->data, frame->pts, buf_size,
         s->current_pts, frame->pkt_pos, frame->pkt_duration,
         xfme->sei_cc_len, xfme->sei_cc_offset);

  /* av_buffer_ref(avpkt->buf); */
  if (!frame->buf[0])
  {
    return AVERROR(ENOMEM);
  }

  av_log(avctx, AV_LOG_DEBUG, "%s: fill array, linesize[0]=%d, fmt=%d, width=%d"
         ", height=%d\n", __FUNCTION__, frame->linesize[0], avctx->sw_pix_fmt,
         s->api_ctx.active_video_width, s->api_ctx.active_video_height);
  if (!is_hw && ((res = av_image_fill_arrays(frame->data, frame->linesize,
                                              buf, avctx->sw_pix_fmt,
                                              s->api_ctx.active_video_width,
                                              s->api_ctx.active_video_height, 1)) < 0))
  {
    av_buffer_unref(&frame->buf[0]);
    return res;
  }

  av_log(avctx, AV_LOG_DEBUG, "%s: success av_image_fill_arrays return %d\n",
         __FUNCTION__, res);
  frame->width = s->api_ctx.active_video_width;
  frame->height = s->api_ctx.active_video_height;
  frame->crop_top = xfme->crop_top;
  frame->crop_bottom = s->api_ctx.active_video_height - xfme->crop_bottom;
  frame->crop_left = xfme->crop_left;
  frame->crop_right = s->api_ctx.active_video_width - xfme->crop_right;

  if (is_hw)
  {
    av_log(avctx, AV_LOG_TRACE, "%s: hw frame av_buffer_get_ref_count=%d\n",
           __FUNCTION__, av_buffer_get_ref_count(frame->buf[0]));
    dst_ctx->pc_width = frame->width;
    dst_ctx->pc_height = frame->height;
    dst_ctx->pc_crop_bottom = frame->crop_bottom;
    dst_ctx->pc_crop_right = frame->crop_right;
  }

  *got_frame = 1;
  return buf_size;
}

static int decoder_logan_frame_alloc(AVCodecContext *avctx, XCoderLoganDecContext *s,
                                     ni_logan_session_data_io_t *p_session_data)
{
  int ret, width, height, alloc_mem;

  // If active video resolution has been obtained we just use it as it's the
  // exact size of frame to be returned, otherwise we use what we are told by
  // upper stream as the initial setting and it will be adjusted.
  width = s->api_ctx.active_video_width > 0 ? s->api_ctx.active_video_width : avctx->width;
  height = s->api_ctx.active_video_height > 0 ? s->api_ctx.active_video_height : avctx->height;

  // allocate memory only after resolution is known (buffer pool set up)
  alloc_mem = (s->api_ctx.active_video_width > 0) &&
              (s->api_ctx.active_video_height > 0 ? 1 : 0);

  // HW frame
  if (avctx->pix_fmt == AV_PIX_FMT_NI_LOGAN)
  {
    ret = ni_logan_frame_buffer_alloc(&p_session_data->data.frame,
                                width,
                                height,
                                avctx->codec_id == AV_CODEC_ID_H264,
                                1,
                                s->api_ctx.bit_depth_factor,
                                1);
  }
  else
  {
    ret = ni_logan_decoder_frame_buffer_alloc(s->api_ctx.dec_fme_buf_pool,
                                        &p_session_data->data.frame,
                                        alloc_mem,
                                        width,
                                        height,
                                        avctx->codec_id == AV_CODEC_ID_H264,
                                        s->api_ctx.bit_depth_factor);
  }

  return ret;
}

static void decoder_logan_frame_free(AVCodecContext *avctx,
                                     ni_logan_session_data_io_t *p_session_data)
{
  if (avctx->pix_fmt == AV_PIX_FMT_NI_LOGAN)
  {
    ni_logan_frame_buffer_free(&p_session_data->data.frame);
  }
  else
  {
    ni_logan_decoder_frame_buffer_free(&p_session_data->data.frame);
  }
}

int ff_xcoder_logan_dec_receive(AVCodecContext *avctx, XCoderLoganDecContext *s,
                                AVFrame *frame, bool wait)
{
  /* call xcode_dec_receive to get a decoded YUV frame from the decoder
     instance */
  int ret = 0;
  int got_frame = 0;
  ni_logan_session_data_io_t session_io_data;
  ni_logan_session_data_io_t *p_session_data = &session_io_data;
  int avctx_bit_depth = 0;
  int is_hw_frm = (avctx->pix_fmt == AV_PIX_FMT_NI_LOGAN);

  if (s->draining && s->eos)
  {
    return AVERROR_EOF;
  }

  memset(p_session_data, 0, sizeof(ni_logan_session_data_io_t));

  ret = decoder_logan_frame_alloc(avctx, s, p_session_data);
  if (NI_LOGAN_RETCODE_SUCCESS != ret)
  {
    return AVERROR_EXTERNAL;
  }

  if (is_hw_frm)
  {
    ret = ni_logan_device_session_read_hwdesc(&s->api_ctx, p_session_data);
  }
  else
  {
    ret = ni_logan_device_session_read(&s->api_ctx, p_session_data, NI_LOGAN_DEVICE_TYPE_DECODER);
  }

  if (ret == 0)
  {
    s->eos = p_session_data->data.frame.end_of_stream;
    decoder_logan_frame_free(avctx, p_session_data);
    return AVERROR(EAGAIN);
  }
  else if (ret > 0)
  {
    if (s->vpu_reset)
    {
      // On decoder VPU recovery the first received frame corresponding to the
      // cached seq_hdr_pkt should be dropped since the data is outdated.
      s->vpu_reset = 0;
      decoder_logan_frame_free(avctx, p_session_data);
      return AVERROR(EAGAIN);
    }

    if (p_session_data->data.frame.flags & AV_PKT_FLAG_DISCARD)
    {
      decoder_logan_frame_free(avctx, p_session_data);
      return AVERROR(EAGAIN);
    }

    av_log(avctx, AV_LOG_DEBUG, "Got output buffer pts=%lld dts=%lld eos=%d sos=%d\n",
           p_session_data->data.frame.pts, p_session_data->data.frame.dts,
           p_session_data->data.frame.end_of_stream, p_session_data->data.frame.start_of_stream);

    s->eos = p_session_data->data.frame.end_of_stream;

    // update ctxt resolution if change has been detected
    frame->width = p_session_data->data.frame.video_width;
    frame->height = p_session_data->data.frame.video_height;

    if (is_hw_frm)
    {
      avctx_bit_depth = p_session_data->data.frame.bit_depth;
    }
    else
    {
      avctx_bit_depth = (avctx->pix_fmt == AV_PIX_FMT_YUV420P10LE)?10:8;
    }

    if (frame->width != avctx->width || frame->height != avctx->height || avctx_bit_depth  != p_session_data->data.frame.bit_depth)
    {
      av_log(avctx, AV_LOG_WARNING, "%s: sequence changed: %dx%d %dbits to "
             "%dx%d %dbits\n", __FUNCTION__, avctx->width, avctx->height,
             avctx_bit_depth, frame->width, frame->height,
             p_session_data->data.frame.bit_depth);
      avctx->width = frame->width;
      avctx->height = frame->height;

      if (is_hw_frm)
      {
        s->hwfc->width = frame->width;
        s->hwfc->height = frame->height;
      }
      else
      {
        avctx->sw_pix_fmt = (p_session_data->data.frame.bit_depth == 10)? AV_PIX_FMT_YUV420P10LE : AV_PIX_FMT_YUV420P;
        avctx->pix_fmt = avctx->sw_pix_fmt;
      }
    }

    frame->format = avctx->pix_fmt;

    if (avctx->pix_fmt == AV_PIX_FMT_NI_LOGAN)
    {
      frame->hw_frames_ctx = av_buffer_ref(avctx->hw_frames_ctx);
    }

    retrieve_logan_frame(avctx, frame, &got_frame, &p_session_data->data.frame);

    av_log(avctx, AV_LOG_DEBUG, "%s: got_frame=%d, frame->width=%d, frame->height=%d, "
           "crop top %" SIZE_SPECIFIER " bottom %" SIZE_SPECIFIER " left "
           "%" SIZE_SPECIFIER " right %" SIZE_SPECIFIER ", frame->format=%d, "
           "frame->linesize=%d/%d/%d\n", __FUNCTION__, got_frame, frame->width,
           frame->height, frame->crop_top, frame->crop_bottom, frame->crop_left,
           frame->crop_right, frame->format,
           frame->linesize[0], frame->linesize[1], frame->linesize[2]);

#if FF_API_PKT_PTS
    FF_DISABLE_DEPRECATION_WARNINGS
    frame->pkt_pts = frame->pts;
    FF_ENABLE_DEPRECATION_WARNINGS
#endif
    frame->best_effort_timestamp = frame->pts;
#if 0
    av_log(avctx, AV_LOG_DEBUG, "\n   NI dec out frame: pts  %lld  pkt_dts  %lld   pkt_pts  %lld \n\n", frame->pts, frame->pkt_dts,
     frame->pkt_pts);
#endif
    av_log(avctx, AV_LOG_DEBUG, "%s: pkt_timebase= %d/%d, frame_rate=%d/%d, "
           "frame->pts=%" PRId64 ", frame->pkt_dts=%" PRId64 "\n", __FUNCTION__,
           avctx->pkt_timebase.num, avctx->pkt_timebase.den, avctx->framerate.num,
           avctx->framerate.den, frame->pts, frame->pkt_dts);

    // release buffer ownership and let frame owner return frame buffer to
    // buffer pool later
    p_session_data->data.frame.dec_buf = NULL;
    free(p_session_data->data.frame.p_custom_sei);
    p_session_data->data.frame.p_custom_sei = NULL;
  }
  else
  {
    av_log(avctx, AV_LOG_ERROR, "Failed to get output buffer (status=%d)\n", ret);

    if (NI_LOGAN_RETCODE_ERROR_VPU_RECOVERY == ret)
    {
      av_log(avctx, AV_LOG_WARNING, "%s VPU recovery, need to reset\n", __FUNCTION__);
      decoder_logan_frame_free(avctx, p_session_data);
      return ret;
    }

    return AVERROR_EOF;
  }

  ret = 0;

  return ret;
}

int ff_xcoder_logan_dec_is_flushing(AVCodecContext *avctx,
                                    XCoderLoganDecContext *s)
{
  return s->flushing;
}

int ff_xcoder_logan_dec_flush(AVCodecContext *avctx,
                              XCoderLoganDecContext *s)
{
  s->draining = 0;
  s->flushing = 0;
  s->eos = 0;

#if 0
  int ret;
  ret = ni_logan_device_session_flush(s, NI_LOGAN_DEVICE_TYPE_DECODER);
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
