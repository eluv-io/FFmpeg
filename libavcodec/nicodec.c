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

#include <ni_rsrc_api.h>
#include "nicodec.h"
#include "nidec.h"
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
#include "libavutil/hwcontext_ni.h"

static inline void ni_align_free(void *opaque, uint8_t *data)
{
  ni_buf_t *buf = (ni_buf_t *)opaque;
  if (buf)
  {
    ni_decoder_frame_buffer_pool_return_buf(buf, (ni_buf_pool_t *)buf->pool);
  }
}

static inline void ni_frame_free(void *opaque, uint8_t *data)
{
  if (data)
  {
    ni_hwframe_surface_t* p_data3 = (ni_hwframe_surface_t*)((uint8_t*)data); //for hwframes there is no data0,1,2
    //TODO use int32t device_handle to kill the buffer!
    if (p_data3->i8FrameIdx != NI_INVALID_HW_FRAME_IDX)
    {
      ni_decode_buffer_free(p_data3, p_data3->device_handle);
    }
    free(p_data3);
  }
}

static inline void ni_align_free_nop(void *opaque, uint8_t *data)
{
}

static inline void ni_free(void *opaque, uint8_t *data)
{
  free(data);
}

int ff_xcoder_dec_init(AVCodecContext *avctx,
                       XCoderH264DecContext *s)
{
  /* ToDo: call xcode_dec_open to open a decoder instance */
  int ret;
  ni_encoder_params_t *p_param = &s->api_param;

  s->api_ctx.hw_id = s->dev_dec_idx;
  s->api_ctx.decoder_low_delay = s->low_delay;
  strcpy(s->api_ctx.dev_xcoder, s->dev_xcoder);

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
    s->dev_dec_idx = s->api_ctx.hw_id;
    av_log(avctx, AV_LOG_VERBOSE, "XCoder %s Index %d (inst: %d) opened successfully\n",
           s->dev_xcoder_name, s->dev_dec_idx, s->api_ctx.session_id);

    if (s->hwFrames || p_param->dec_input_params.hwframes)
    {
      if (!avctx->hw_device_ctx)
      {
        av_log(avctx, AV_LOG_DEBUG, "nicodec.c:ff_xcoder_dec_init() hwdevice_ctx_create\n");

        av_hwdevice_ctx_create(&avctx->hw_device_ctx, AV_HWDEVICE_TYPE_NI, NULL, NULL, 0); //create with null device 
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

      s->hwfc->format = AV_PIX_FMT_NI;
      s->hwfc->width = avctx->width;
      s->hwfc->height = avctx->height;

      s->hwfc->sw_format = avctx->sw_pix_fmt;
      s->hwfc->initial_pool_size = -1; //Decoder has its own dedicated pool

      ret = av_hwframe_ctx_init(avctx->hw_frames_ctx);
      avctx->pix_fmt = AV_PIX_FMT_NI;
      s->api_ctx.hw_action = NI_CODEC_HW_ENABLE;
    }
    else
    {
      avctx->pix_fmt = avctx->sw_pix_fmt; //reassign in case above conditions alter value
      s->api_ctx.hw_action = NI_CODEC_HW_NONE;//
    }
  }

  return ret;
}

int ff_xcoder_dec_close(AVCodecContext *avctx,
                        XCoderH264DecContext *s)
{
  ni_retcode_t ret = NI_RETCODE_FAILURE;

  ret = ni_device_session_close(&s->api_ctx, s->eos, NI_DEVICE_TYPE_DECODER);
  if (NI_RETCODE_SUCCESS != ret)
  {
    av_log(avctx, AV_LOG_ERROR, "Failed to close Decode Session (status = %d)\n", ret);
  }

  ni_encoder_params_t *p_param = &s->api_param; //dec params in union with enc params struct
  if (p_param->dec_input_params.hwframes)
  {
#if 1//def XCODER_IO_RW_ENABLED // asume always enable XCODER_IO_RW_ENABLED
    av_log(avctx, AV_LOG_VERBOSE, "File BLK handle %d close suspended to frames Uninit\n", s->api_ctx.blk_io_handle); //suspended_device_handle
    AVHWFramesContext *ctx;
    NIFramesContext *dst_ctx;
    ctx = (AVHWFramesContext*)avctx->hw_frames_ctx->data;
    dst_ctx = ctx->internal->priv;
    dst_ctx->suspended_device_handle = s->api_ctx.blk_io_handle;
#else
    av_log(avctx, AV_LOG_VERBOSE, "File handle %d close suspended to frames Uninit\n", s->api_ctx.device_handle); //suspended_device_handle
    AVHWFramesContext *ctx;
    NIFramesContext *dst_ctx;
    ctx = (AVHWFramesContext*)avctx->hw_frames_ctx->data;
    dst_ctx = ctx->internal->priv;
    dst_ctx->suspended_device_handle = s->api_ctx.device_handle;
#endif

#ifdef __linux__
#if 1//def XCODER_IO_RW_ENABLED
    ni_device_close(s->api_ctx.device_handle);
#else
    ni_device_close(s->api_ctx.blk_io_handle);
#endif
#endif
  }
  else
  {
#ifdef _WIN32
    ni_device_close(s->api_ctx.device_handle);
#elif __linux__
    ni_device_close(s->api_ctx.device_handle);
    ni_device_close(s->api_ctx.blk_io_handle);
#endif
  }

  s->api_ctx.device_handle = NI_INVALID_DEVICE_HANDLE;
  s->api_ctx.blk_io_handle = NI_INVALID_DEVICE_HANDLE;

  return 0;
}

/*!******************************************************************************
 *  \brief  Extract custom sei payload data from AVPacket,
 *  and save it to ni_packet_t
 *
 *  \param AVCodecContext *avctx - avcodec context
 *  \param AVPacket *pkt - AVPacket
 *  \param int start_index - pkt data index of custom sei first byte after SEI type
 *  \param ni_packet_t *p_packet - netint internal packet
 *  \param uint8_t sei_type - type of SEI
 *  \param int got_slice - whether got vcl in the pkt data, 1 means got
 *
 *  \return - 0 on success, non-0 on failure 
 *******************************************************************************/
static int ff_xcoder_extract_custom_sei(AVCodecContext *avctx, AVPacket *pkt, int start_index,
                                        ni_packet_t *p_packet, uint8_t sei_type, int got_slice)
{
  int i;
  uint8_t *udata;
  uint8_t *sei_data;
  int len = 0;
  int sei_size = 0; // default is 0
  int index = start_index;
  int sei_index = 0;

  av_log(avctx, AV_LOG_TRACE, "ff_xcoder_extract_custom_sei() enter\n");
  if (p_packet->p_all_custom_sei == NULL)
  {
    /* max size */
    p_packet->p_all_custom_sei = (ni_all_custom_sei_t *)malloc(sizeof(ni_all_custom_sei_t));
    if (p_packet->p_all_custom_sei == NULL)
    {
     av_log(avctx, AV_LOG_ERROR, "failed to allocate all custom sei buffer.\n");
      return AVERROR(ENOMEM);
    }
    memset(p_packet->p_all_custom_sei, 0, sizeof(ni_custom_sei_t));
  }

  sei_index = p_packet->p_all_custom_sei->custom_sei_cnt;
  if (sei_index >= NI_MAX_CUSTOM_SEI_CNT)
  {
    av_log(avctx, AV_LOG_WARNING, "number of custom sei in current frame is out of limit(%d).\n",
           NI_MAX_CUSTOM_SEI_CNT);
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
  if (sei_size > NI_MAX_CUSTOM_SEI_SZ)
  {
    av_log(avctx, AV_LOG_WARNING, "custom sei corrupted: size(%d) out of limit(%d).\n",
           sei_size, NI_MAX_CUSTOM_SEI_SZ);
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
    p_packet->p_all_custom_sei->ni_custom_sei[sei_index].custom_sei_loc = NI_CUSTOM_SEI_LOC_AFTER_VCL;
  }
  else
  {
    p_packet->p_all_custom_sei->ni_custom_sei[sei_index].custom_sei_loc = NI_CUSTOM_SEI_LOC_BEFORE_VCL;
  }
  p_packet->p_all_custom_sei->custom_sei_cnt ++;
  av_log(avctx, AV_LOG_TRACE, "ff_xcoder_extract_custom_sei() exit, "
         "custom_sei_cnt=%d, size=%d, type=%d\n",
         p_packet->p_all_custom_sei->custom_sei_cnt, sei_size, sei_type);

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
 *  \param XCoderH264DecContext *s - netint decoder context
 *  \param AVPacket *pkt - AVPacket
 *  \param int start_index - pkt data index of custom sei first byte after SEI type
 *  \param ni_packet_t *p_packet - netint internal packet
 *
 *  \return - 0 on success or not detect correct custom sei, non-0 on failure 
 *******************************************************************************/
static int ff_xcoder_detect_custom_sei(AVCodecContext *avctx, XCoderH264DecContext *s, AVPacket *pkt, ni_packet_t *p_packet)
{
  int ret = 0;
  const uint8_t *ptr = NULL;
  const uint8_t *end = NULL;
  uint8_t custom_sei_type = s->custom_sei;
  uint8_t chk_pkt = s->enable_check_packet;
  uint8_t nalu_type;
  uint8_t sei_type;
  uint32_t stc = -1;
  int got_slice = 0;

  av_log(avctx, AV_LOG_TRACE, "ff_xcoder_detect_custom_sei(): custom SEI type %d\n", custom_sei_type);

  if (!pkt->data || !avctx)
  {
    return ret;
  }

  // search custom sei in the lone buffer
  // if there is a custom sei in the lone sei, the firmware can't recoginze it. 
  // passthrough the custom sei here.
  if (s->api_ctx.lone_sei_size)
  {
    av_log(avctx, AV_LOG_TRACE, "ff_xcoder_detect_custom_sei(): detect in lone SEI, size=%d\n",
           s->api_ctx.lone_sei_size);
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
          /* extract SEI payload, store in ni_packet and pass to libxcoder. */
          ret = ff_xcoder_extract_custom_sei(avctx, pkt, ptr + 1 - pkt->data, p_packet, sei_type, got_slice);
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
          /* extract SEI payload, store in ni_packet and pass to libxcoder. */
          ret = ff_xcoder_extract_custom_sei(avctx, pkt, ptr + 2 - pkt->data, p_packet, sei_type, got_slice);
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
        av_log(avctx, AV_LOG_ERROR, "%s wrong codec %d !\n", __func__,
               avctx->codec_id);
        break;
      }

      stc = -1;
      ptr = avpriv_find_start_code(ptr, end, &stc);
    }
  }

  // search custom sei in the packet
  av_log(avctx, AV_LOG_TRACE, "ff_xcoder_detect_custom_sei(): detect in packet, size=%d\n", pkt->size);
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
        /* extract SEI payload, store in ni_packet and pass to libxcoder. */
        ret = ff_xcoder_extract_custom_sei(avctx, pkt, ptr + 1 - pkt->data, p_packet, sei_type, got_slice);
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
        /* extract SEI payload, store in ni_packet and pass to libxcoder. */
        ret = ff_xcoder_extract_custom_sei(avctx, pkt, ptr + 2 - pkt->data, p_packet, sei_type, got_slice);
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
      av_log(avctx, AV_LOG_ERROR, "%s wrong codec %d !\n", __func__,
             avctx->codec_id);
      break;
    }

    stc = -1;
    ptr = avpriv_find_start_code(ptr, end, &stc);
  }

  if (p_packet->p_all_custom_sei)
  {
    av_log(avctx, AV_LOG_TRACE, "ff_xcoder_detect_custom_sei(): total custom SEI number %d\n",
          p_packet->p_all_custom_sei->custom_sei_cnt);
  }
  else
  {
    av_log(avctx, AV_LOG_TRACE, "ff_xcoder_detect_custom_sei(): no custom SEI detected\n");
  }

  return ret;
}

// return 1 if need to prepend saved header to pkt data, 0 otherwise
int ff_xcoder_add_headers(AVCodecContext *avctx, AVPacket *pkt)
{
  XCoderH264DecContext *s = avctx->priv_data;
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
      nalu_type = stc & 0x1F;

      // update saved header if it has changed
      if (s->got_first_idr && H264_NAL_IDR_SLICE == nalu_type)
      {
        if (s->extradata_size != avctx->extradata_size ||
            memcmp(s->extradata, avctx->extradata, s->extradata_size))
        {
          s->got_first_idr = 0;
        }
      }

      if (! s->got_first_idr && H264_NAL_IDR_SLICE == nalu_type)
      {
        free(s->extradata);
        s->extradata = malloc(avctx->extradata_size);
        if (! s->extradata)
        {
          av_log(avctx, AV_LOG_ERROR, "ff_xcoder_add_headers memory allocation "
                 "failed !\n");
          ret = 0;
          break;
        }
        av_log(avctx, AV_LOG_TRACE, "ff_xcoder_add_headers size %d\n",
               avctx->extradata_size);
        memcpy(s->extradata, avctx->extradata, avctx->extradata_size);
        s->extradata_size = avctx->extradata_size;
        s->got_first_idr = 1;
        ret = 1;
        break;
      }

      // when header (SPS/PPS) already exists, no need to prepend it again;
      // we use one of the header info to simplify the checking.
      if (H264_NAL_SPS == nalu_type || H264_NAL_PPS == nalu_type)
      {
        // save the header if not done yet for subsequent comparison
        if (! s->extradata_size || ! s->extradata)
        {
          s->extradata = malloc(avctx->extradata_size);
          if (! s->extradata)
          {
            av_log(avctx, AV_LOG_ERROR, "ff_xcoder_add_headers memory "
                   "allocation failed !\n");
            ret = 0;
            break;
          }
          av_log(avctx, AV_LOG_TRACE, "ff_xcoder_add_headers size %d\n",
                 avctx->extradata_size);
          memcpy(s->extradata, avctx->extradata, avctx->extradata_size);
          s->extradata_size = avctx->extradata_size;
        }
        s->got_first_idr = 1;
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
      if (s->got_first_idr && (nalu_type >= HEVC_NAL_BLA_W_LP &&
                               nalu_type <= HEVC_NAL_IRAP_VCL23))
      {
        if (s->extradata_size != avctx->extradata_size ||
            memcmp(s->extradata, avctx->extradata, s->extradata_size))
        {
          s->got_first_idr = 0;
        }
      }

      if (! s->got_first_idr && (nalu_type >= HEVC_NAL_BLA_W_LP &&
                                 nalu_type <= HEVC_NAL_IRAP_VCL23))
      {
        free(s->extradata);
        s->extradata = malloc(avctx->extradata_size);
        if (! s->extradata)
        {
          av_log(avctx, AV_LOG_ERROR, "ff_xcoder_add_headers memory allocation "
                 "failed !\n");
          ret = 0;
          break;
        }
        av_log(avctx, AV_LOG_TRACE, "ff_xcoder_add_headers size %d\n",
               avctx->extradata_size);
        memcpy(s->extradata, avctx->extradata, avctx->extradata_size);
        s->extradata_size = avctx->extradata_size;
        s->got_first_idr = 1;
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
          s->extradata = malloc(avctx->extradata_size);
          if (! s->extradata)
          {
            av_log(avctx, AV_LOG_ERROR, "ff_xcoder_add_headers memory "
                   "allocation failed !\n");
            ret = 0;
            break;
          }
          av_log(avctx, AV_LOG_TRACE, "ff_xcoder_add_headers size %d\n",
                 avctx->extradata_size);
          memcpy(s->extradata, avctx->extradata, avctx->extradata_size);
          s->extradata_size = avctx->extradata_size;
        }
        s->got_first_idr = 1;
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
      av_log(avctx, AV_LOG_ERROR, "ff_xcoder_add_headers wrong codec %d !\n",
             avctx->codec_id);
      break;
    }
  }
  return ret;
}

// check if the packet is SEI only, 1 yes, 0 otherwise
// check if getting the header of streams in decoder low delay mode, and update its value
// check the new sequence header after VPU recovery, abandon the AVPackets before.
static int xcoder_packet_parse(AVCodecContext *avctx, XCoderH264DecContext *s, AVPacket *pkt, ni_packet_t *p_packet)
{
  int pkt_sei_alone = 0;
  int got_slice = 0;
  int low_delay = s->low_delay;
  int pkt_nal_bitmap = s->pkt_nal_bitmap;
  int chk_pkt = s->enable_check_packet;
  const uint8_t *ptr = pkt->data;
  const uint8_t *end = pkt->data + pkt->size;
  uint32_t stc = -1;
  uint8_t nalu_type = 0;
  int nalu_count = 0;

  if (!pkt->data || !avctx)
  {
    return pkt_sei_alone;
  }

  if (pkt_nal_bitmap & NI_GENERATE_ALL_NAL_HEADER_BIT)
  {
    av_log(avctx, AV_LOG_TRACE, "xcoder_packet_parse(): already find the header of streams.\n");
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
        av_log(avctx, AV_LOG_TRACE, "xcoder_packet_parse(): no NAL found in pkt.\n");
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
        av_log(avctx, AV_LOG_TRACE, "xcoder_packet_parse(): found SEI NAL after VCL NAL, len = %d.\n",
               p_packet->len_of_sei_after_vcl);
      }
      else if ((nalu_type >= H264_NAL_SLICE) && (nalu_type <= H264_NAL_IDR_SLICE)) //VCL
      {
        got_slice = 1;
      }

      switch (nalu_type)
      {
        case H264_NAL_SPS:
          pkt_nal_bitmap |= NI_NAL_SPS_BIT;
          break;
        case H264_NAL_PPS:
          pkt_nal_bitmap |= NI_NAL_PPS_BIT;
          break;
        default:
          break;
      }

      // The NALU of SPS/PPS could be packed in one AVC AVPacket, set
      // decoder low delay mode once.
      if (pkt_nal_bitmap & (NI_NAL_SPS_BIT | NI_NAL_PPS_BIT))
      {
        av_log(avctx, AV_LOG_TRACE, "xcoder_packet_parse(): Detect SPS, PPS and IDR, enable decoder low delay mode.\n");
        pkt_nal_bitmap |= NI_GENERATE_ALL_NAL_HEADER_BIT;
        s->vpu_reset = 0;
        if (low_delay)
        {
          s->api_ctx.decoder_low_delay = low_delay;
          low_delay = 0;
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
        av_log(avctx, AV_LOG_TRACE, "xcoder_packet_parse(): found SEI NAL after VCL NAL, len = %d.\n",
               p_packet->len_of_sei_after_vcl);
      }
      else if ((nalu_type >= HEVC_NAL_TRAIL_N) && (nalu_type <= HEVC_NAL_RSV_VCL31)) //VCL
      {
        got_slice = 1;
      }

      switch (nalu_type)
      {
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

      // The NALU of VPS/SPS/PPS could be packed in one HEVC AVPacket, set
      // decoder low delay mode once.
      if (pkt_nal_bitmap & (NI_NAL_VPS_BIT | NI_NAL_SPS_BIT | NI_NAL_PPS_BIT))
      {
        av_log(avctx, AV_LOG_TRACE, "xcoder_packet_parse(): Detect VPS, SPS, PPS and IDR, enable decoder low delay mode.\n");
        pkt_nal_bitmap |= NI_GENERATE_ALL_NAL_HEADER_BIT;
        s->vpu_reset = 0;
        if (low_delay)
        {
          s->api_ctx.decoder_low_delay = low_delay;
          low_delay = 0;
        }
      }
    }
    else
    {
      av_log(avctx, AV_LOG_ERROR, "xcoder_packet_parse() wrong codec %d !\n",
             avctx->codec_id);
      pkt_sei_alone = 0;
      break;
    }
  }

  s->pkt_nal_bitmap = pkt_nal_bitmap;
  return pkt_sei_alone;
}

int ff_xcoder_dec_send(AVCodecContext *avctx,
                       XCoderH264DecContext *s,
                       AVPacket *pkt)
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
    // Once VPU reset, the s->draining would be recovered during session reset.
    // And so is the s->eos in case that it lost in the last session read.
    s->eos |= (s->draining && s->vpu_reset);
  }

  if (s->draining && s->eos)
  {
    av_log(avctx, AV_LOG_DEBUG, "Decoder is draining, eos\n");
    return AVERROR_EOF;
  }

  if (xpkt->data_len == 0)
  {
    memset(xpkt, 0, sizeof(ni_packet_t));
    xpkt->pts = pkt->pts;
    xpkt->dts = pkt->dts;
    //    xpkt->pos = pkt->pos;
    xpkt->video_width = avctx->width;
    xpkt->video_height = avctx->height;
    xpkt->p_data = NULL;
    xpkt->data_len = pkt->size;
    xpkt->p_all_custom_sei = NULL;
    xpkt->len_of_sei_after_vcl = 0;

    if (avctx->extradata_size > 0 &&
        (avctx->extradata_size != s->extradata_size) &&
        ff_xcoder_add_headers(avctx, pkt))
    {
      if (avctx->extradata_size > s->api_ctx.max_nvme_io_size * 2)
      {
        av_log(avctx, AV_LOG_ERROR, "ff_xcoder_dec_send extradata_size %d "
               "exceeding max size supported: %d\n", avctx->extradata_size,
               s->api_ctx.max_nvme_io_size * 2);
      }
      else
      {
        av_log(avctx, AV_LOG_DEBUG, "ff_xcoder_dec_send extradata_size %d "
               "copied to pkt start.\n", avctx->extradata_size);
        s->api_ctx.prev_size = avctx->extradata_size;
        memcpy(s->api_ctx.p_leftover, avctx->extradata, avctx->extradata_size);
      }
    }

    if (s->low_delay && s->got_first_idr && !(s->pkt_nal_bitmap & NI_GENERATE_ALL_NAL_HEADER_BIT))
    {
      s->api_ctx.decoder_low_delay = s->low_delay;
      s->pkt_nal_bitmap |= NI_GENERATE_ALL_NAL_HEADER_BIT;
      av_log(avctx, AV_LOG_TRACE, "ff_xcoder_dec_send got first IDR in decoder low delay mode, "
             "delay time %dms, pkt_nal_bitmap %d\n", s->low_delay, s->pkt_nal_bitmap);
    }

    // check if the packet is SEI only, save it to be sent with the next data frame.
    // check if getting the header of streams in decoder low delay mode, and update its value.
    // check if VPU recovery, abandon the AVPackets before the next sequence header is met.
    if (xcoder_packet_parse(avctx, s, pkt, xpkt))
    {
      // skip the packet if it's corrupted and/or exceeding lone SEI buf size
      if (pkt->size + s->api_ctx.lone_sei_size <= NI_MAX_SEI_DATA)
      {
        memcpy(s->api_ctx.buf_lone_sei + s->api_ctx.lone_sei_size,
               pkt->data, pkt->size);
        s->api_ctx.lone_sei_size += pkt->size;

        av_log(avctx, AV_LOG_TRACE, "ff_xcoder_dec_send pkt lone SEI, saved, "
               "and return %d\n", pkt->size);
      }
      else
      {
        av_log(avctx, AV_LOG_WARNING, "lone SEI size %d > buf size %ld, "
               "corrupted? skipped ..\n", pkt->size, NI_MAX_SEI_DATA);
      }

      xpkt->data_len = 0;
      return pkt->size;
    }

    if (s->custom_sei != NI_INVALID_SEI_TYPE)
    {
      ret = ff_xcoder_detect_custom_sei(avctx, s, pkt, xpkt);
      if (ret != 0)
      {
        goto fail;
      }
    }

    // embed lone SEI saved previously (if any) to send to decoder
    if (s->api_ctx.lone_sei_size)
    {
      av_log(avctx, AV_LOG_TRACE, "ff_xcoder_dec_send copy over lone SEI "
             "data size: %d\n", s->api_ctx.lone_sei_size);

      memcpy((uint8_t *)s->api_ctx.p_leftover + s->api_ctx.prev_size,
             s->api_ctx.buf_lone_sei, s->api_ctx.lone_sei_size);
      s->api_ctx.prev_size += s->api_ctx.lone_sei_size;
      s->api_ctx.lone_sei_size = 0;
    }

    if ((pkt->size + s->api_ctx.prev_size) > 0)
    {
      ni_packet_buffer_alloc(xpkt, (pkt->size + s->api_ctx.prev_size - xpkt->len_of_sei_after_vcl));
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

  // The vpu_reset indicator will not be unset until the next sequence header
  // is met before which the AVPacket will NOT be sent and it will return 0 to
  // the caller.
  if (s->vpu_reset)
  {
    ni_packet_buffer_free(xpkt);
    return 0;
  }

  av_log(avctx, AV_LOG_DEBUG, "ff_xcoder_dec_send: pkt->size=%d\n", pkt->size);

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

    av_log(avctx, AV_LOG_TRACE, "ni_packet_copy before: size=%d, s->prev_size=%d, send_size=%d, "
           "len_of_sei_after_slice=%d (end of stream)\n",
           pkt->size, s->api_ctx.prev_size, send_size, xpkt->len_of_sei_after_vcl);
    if (new_packet)
    {
      extra_prev_size = s->api_ctx.prev_size;
      send_size = ni_packet_copy(xpkt->p_data, pkt->data, (pkt->size - xpkt->len_of_sei_after_vcl),
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
    av_log(avctx, AV_LOG_TRACE, "ni_packet_copy after: size=%d, s->prev_size=%d, send_size=%d, "
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
        ret = AVERROR_EOF;
      }
      goto fail;
    }
    av_log(avctx, AV_LOG_DEBUG, "Queued eos (status = %d) ts=%llu\n",
           sent, xpkt->pts);
    s->draining = 1;

    ni_device_session_flush(&(s->api_ctx), NI_DEVICE_TYPE_DECODER);
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
    av_log(avctx, AV_LOG_TRACE, "ni_packet_copy before: size=%d, s->prev_size=%d, send_size=%d, len_of_sei_after_slice=%d\n",
           pkt->size, s->api_ctx.prev_size, send_size, xpkt->len_of_sei_after_vcl);
    if (new_packet)
    {
      extra_prev_size = s->api_ctx.prev_size;
      send_size = ni_packet_copy(xpkt->p_data, pkt->data, (pkt->size - xpkt->len_of_sei_after_vcl),
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
    av_log(avctx, AV_LOG_TRACE, "ni_packet_copy after: size=%d, s->prev_size=%d, send_size=%d, "
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
      sent = ni_device_session_write(&s->api_ctx, &(s->api_pkt), NI_DEVICE_TYPE_DECODER);
      av_log(avctx, AV_LOG_DEBUG, "ff_xcoder_dec_send pts=%" PRIi64 ", dts=%" PRIi64 ", pos=%" PRIi64 ", sent=%d\n", pkt->pts, pkt->dts, pkt->pos, sent);
    }
    if (sent < 0)
    {
      av_log(avctx, AV_LOG_ERROR, "Failed to send compressed pkt (status = "
                                  "%d)\n", sent);
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
        ret = AVERROR_EOF;
      }
      goto fail;
    }
    else if (sent == 0)
    {
      av_log(avctx, AV_LOG_DEBUG, "Queued input buffer size=0\n");
    }
    else if (sent < size)
    { /* partial sent; keep trying */
      av_log(avctx, AV_LOG_DEBUG, "Queued input buffer size=%d\n", sent);
    }
  }

  if (sent != 0)
  {
    //keep the current pkt to resend next time
    ni_packet_buffer_free(xpkt);
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
  ni_packet_buffer_free(xpkt);
  free(xpkt->p_all_custom_sei);
  xpkt->p_all_custom_sei = NULL;

  return ret;
}

int retrieve_frame(AVCodecContext *avctx, AVFrame *data, int *got_frame,
                   ni_frame_t *xfme)
{
  XCoderH264DecContext *s = avctx->priv_data;

  int buf_size = xfme->data_len[0] + xfme->data_len[1] + xfme->data_len[2] + xfme->data_len[3];
  uint8_t *buf = xfme->p_data[0];
  int stride = 0;
  int res = 0;
  AVHWFramesContext *ctx;
  NIFramesContext *dst_ctx;
  AVFrame *frame = data;
  bool is_hw = xfme->data_len[3] > 0;

  av_log(avctx, AV_LOG_TRACE, "retrieve_frame: buf %p data_len [%d %d %d %d] buf_size %d\n",
         buf, xfme->data_len[0], xfme->data_len[1], xfme->data_len[2], xfme->data_len[3], buf_size);

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
        res = ni_device_session_copy(&s->api_ctx, &dst_ctx->api_ctx);
        if (NI_RETCODE_SUCCESS != res)
        {
          return res;
        }
        av_log(avctx, AV_LOG_VERBOSE, "retrieve_frame: blk_io_handle %d device_handle %d\n", s->api_ctx.blk_io_handle, s->api_ctx.device_handle);
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
  switch (xfme->ni_pict_type)
  {
  case PIC_TYPE_I:
    frame->pict_type = AV_PICTURE_TYPE_I;
    break;
  case PIC_TYPE_IDR:
  case PIC_TYPE_CRA:
    frame->pict_type = AV_PICTURE_TYPE_I;
    frame->key_frame = 1;
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
  }
  else
  {
    frame->buf[0] = av_buffer_create(buf, buf_size, ni_align_free, xfme->dec_buf, 0);
  }

  buf = frame->buf[0]->data;

  // User Data Unregistered SEI if available
  if (s->enable_user_data_sei_passthru &&
      xfme->sei_user_data_unreg_len && xfme->sei_user_data_unreg_offset)
  {
    uint8_t *sei_buf = (uint8_t *)xfme->p_data[0] +
    xfme->sei_user_data_unreg_offset;
    AVBufferRef *sei_ref = av_buffer_create(sei_buf,
                                            xfme->sei_user_data_unreg_len,
                                            ni_align_free_nop, NULL, 0);
    if (! sei_ref ||
        ! av_frame_new_side_data_from_buf(frame, AV_FRAME_DATA_NETINT_UDU_SEI,
                                          sei_ref))
    {
      return AVERROR(ENOMEM);
    }
  }

  // close caption data if available
  if (xfme->sei_cc_len && xfme->sei_cc_offset)
  {
    uint8_t *sei_buf = (uint8_t *)xfme->p_data[0] + xfme->sei_cc_offset;
    AVBufferRef *sei_ref = av_buffer_create(sei_buf, xfme->sei_cc_len,
                                            ni_align_free_nop, NULL, 0);
    if (! sei_ref ||
        ! av_frame_new_side_data_from_buf(frame, AV_FRAME_DATA_A53_CC,
                                          sei_ref))
    {
      return AVERROR(ENOMEM);
    }
  }

  // hdr10 sei data if available
  if (xfme->sei_hdr_mastering_display_color_vol_len &&
      xfme->sei_hdr_mastering_display_color_vol_offset)
  {
    const int chroma_den = 50000;
    const int luma_den = 10000;
    AVMasteringDisplayMetadata *mdm;
    ni_dec_mastering_display_colour_volume_t *pColourVolume;

    mdm = av_mastering_display_metadata_create_side_data(frame);
    if (!mdm)
    {
      return AVERROR(ENOMEM);
    }

    pColourVolume = (ni_dec_mastering_display_colour_volume_t *)(
      (uint8_t*)xfme->p_data[0] + xfme->sei_hdr_mastering_display_color_vol_offset);

    // HEVC uses a g,b,r ordering, which we convert to a more natural r,g,b,
    // this is so we are compatible with FFmpeg default soft decoder
    mdm->display_primaries[0][0].num = pColourVolume->display_primaries_x[2];
    mdm->display_primaries[0][0].den = chroma_den;
    mdm->display_primaries[0][1].num = pColourVolume->display_primaries_y[2];
    mdm->display_primaries[0][1].den = chroma_den;
    mdm->display_primaries[1][0].num = pColourVolume->display_primaries_x[0];
    mdm->display_primaries[1][0].den = chroma_den;
    mdm->display_primaries[1][1].num = pColourVolume->display_primaries_y[0];
    mdm->display_primaries[1][1].den = chroma_den;
    mdm->display_primaries[2][0].num = pColourVolume->display_primaries_x[1];
    mdm->display_primaries[2][0].den = chroma_den;
    mdm->display_primaries[2][1].num = pColourVolume->display_primaries_y[1];
    mdm->display_primaries[2][1].den = chroma_den;

    mdm->white_point[0].num = pColourVolume->white_point_x;
    mdm->white_point[0].den = chroma_den;
    mdm->white_point[1].num = pColourVolume->white_point_y;
    mdm->white_point[1].den = chroma_den;

    mdm->min_luminance.num = pColourVolume->min_display_mastering_luminance;
    mdm->min_luminance.den = luma_den;
    mdm->max_luminance.num = pColourVolume->max_display_mastering_luminance;
    mdm->max_luminance.den = luma_den;

    mdm->has_luminance = mdm->has_primaries = 1;
  }

  if (xfme->sei_hdr_content_light_level_info_len &&
      xfme->sei_hdr_content_light_level_info_offset)
  {
    AVContentLightMetadata *clm;
    ni_content_light_level_info_t *pLightLevel;

    clm = av_content_light_metadata_create_side_data(frame);
    if (! clm)
    {
      return AVERROR(ENOMEM);
    }

    pLightLevel = (ni_content_light_level_info_t *)(
      (uint8_t*)xfme->p_data[0] + xfme->sei_hdr_content_light_level_info_offset);
    clm->MaxCLL  = pLightLevel->max_content_light_level;
    clm->MaxFALL = pLightLevel->max_pic_average_light_level;
  }

  // hdr10+ sei data if available
  if (xfme->sei_hdr_plus_len && xfme->sei_hdr_plus_offset)
  {
    uint8_t *sei_buf = (uint8_t *)xfme->p_data[0] + xfme->sei_hdr_plus_offset;
    AVDynamicHDRPlus *hdrp = av_dynamic_hdr_plus_create_side_data(frame);
    GetBitContext gb;
    int w, i, j, i_limit, j_limit;

    if (! hdrp)
    {
      return AVERROR(ENOMEM);
    }
    init_get_bits8(&gb, sei_buf, xfme->sei_hdr_plus_len);

    hdrp->itu_t_t35_country_code = 0xB5;
    hdrp->application_version = 0;
    // first 7 bytes of t35 SEI data header already matched HDR10+, and:
    skip_bits(&gb, 7 * 8);

    // num_windows u(2)
    hdrp->num_windows = get_bits(&gb, 2);
    av_log(avctx, AV_LOG_TRACE, "hdr10+ num_windows %u\n", hdrp->num_windows);
    if (! (1 == hdrp->num_windows || 2 == hdrp->num_windows ||
           3 == hdrp->num_windows))
    {
      // wrong format and skip this HDR10+ SEI
      
    }
    else
    {
      // the following block will be skipped for hdrp->num_windows == 1
      for (w = 1; w < hdrp->num_windows; w++)
      {
        hdrp->params[w - 1].window_upper_left_corner_x = av_make_q(
          get_bits(&gb, 16), 1);
        hdrp->params[w - 1].window_upper_left_corner_y = av_make_q(
          get_bits(&gb, 16), 1);
        hdrp->params[w - 1].window_lower_right_corner_x = av_make_q(
          get_bits(&gb, 16), 1);
        hdrp->params[w - 1].window_lower_right_corner_y = av_make_q(
          get_bits(&gb, 16), 1);
        hdrp->params[w - 1].center_of_ellipse_x = get_bits(&gb, 16);
        hdrp->params[w - 1].center_of_ellipse_y = get_bits(&gb, 16);
        hdrp->params[w - 1].rotation_angle = get_bits(&gb, 8);
        hdrp->params[w - 1].semimajor_axis_internal_ellipse =
        get_bits(&gb, 16);
        hdrp->params[w - 1].semimajor_axis_external_ellipse =
        get_bits(&gb, 16);
        hdrp->params[w - 1].semiminor_axis_external_ellipse =
          get_bits(&gb, 16);
        hdrp->params[w - 1].overlap_process_option = 
        (enum AVHDRPlusOverlapProcessOption)get_bits(&gb, 1);
      }

      // values are scaled down according to standard spec
      hdrp->targeted_system_display_maximum_luminance.num = get_bits(&gb, 27);
      hdrp->targeted_system_display_maximum_luminance.den = 10000;

      hdrp->targeted_system_display_actual_peak_luminance_flag =
      get_bits(&gb, 1);

      av_log(avctx, AV_LOG_TRACE, "hdr10+ targeted_system_display_maximum_luminance %d\n", hdrp->targeted_system_display_maximum_luminance.num);
      av_log(avctx, AV_LOG_TRACE, "hdr10+ targeted_system_display_actual_peak_luminance_flag %u\n", hdrp->targeted_system_display_actual_peak_luminance_flag);

      if (hdrp->targeted_system_display_actual_peak_luminance_flag)
      {
        i_limit = hdrp->num_rows_targeted_system_display_actual_peak_luminance =
        get_bits(&gb, 5);
        j_limit = hdrp->num_cols_targeted_system_display_actual_peak_luminance =
        get_bits(&gb, 5);

        av_log(avctx, AV_LOG_TRACE, "hdr10+ num_rows_targeted_system_display_actual_peak_luminance x num_cols_targeted_system_display_actual_peak_luminance %u x %u\n", 
               i_limit, j_limit);
               
        i_limit = i_limit > 25 ? 25 : i_limit;
        j_limit = j_limit > 25 ? 25 : j_limit;
        for (i = 0; i < i_limit; i++)
          for (j = 0; j < j_limit; j++)
          {
            hdrp->targeted_system_display_actual_peak_luminance[i][j].num =
            get_bits(&gb, 4);
            hdrp->targeted_system_display_actual_peak_luminance[i][j].den = 15;
            av_log(avctx, AV_LOG_TRACE, "hdr10+ targeted_system_display_actual_peak_luminance[%d][%d] %d\n", i, j, 
                   hdrp->targeted_system_display_actual_peak_luminance[i][j].num);
          }
      }

      for (w = 0; w < hdrp->num_windows; w++)
      {
        for (i = 0; i < 3; i++)
        {
          hdrp->params[w].maxscl[i].num = get_bits(&gb, 17);
          hdrp->params[w].maxscl[i].den = 100000;
          av_log(avctx, AV_LOG_TRACE, "hdr10+ maxscl[%d][%d] %d\n", w, i,
                 hdrp->params[w].maxscl[i].num);
        }
        hdrp->params[w].average_maxrgb.num = get_bits(&gb, 17);
        hdrp->params[w].average_maxrgb.den = 100000;
        av_log(avctx, AV_LOG_TRACE, "hdr10+ average_maxrgb[%d] %d\n",
               w, hdrp->params[w].average_maxrgb.num);

        i_limit = hdrp->params[w].num_distribution_maxrgb_percentiles = 
        get_bits(&gb, 4);
        av_log(avctx, AV_LOG_TRACE, 
               "hdr10+ num_distribution_maxrgb_percentiles[%d] %d\n",
               w, hdrp->params[w].num_distribution_maxrgb_percentiles);

        i_limit = i_limit > 15 ? 15 : i_limit;
        for (i = 0; i < i_limit; i++)
        {
          hdrp->params[w].distribution_maxrgb[i].percentage = get_bits(&gb, 7);
          hdrp->params[w].distribution_maxrgb[i].percentile.num = 
          get_bits(&gb, 17);
          hdrp->params[w].distribution_maxrgb[i].percentile.den = 100000;
          av_log(avctx, AV_LOG_TRACE, "hdr10+ distribution_maxrgb_percentage[%d][%d] %u\n",
                 w, i, hdrp->params[w].distribution_maxrgb[i].percentage);
          av_log(avctx, AV_LOG_TRACE, "hdr10+ distribution_maxrgb_percentile[%d][%d] %d\n",
                 w, i, hdrp->params[w].distribution_maxrgb[i].percentile.num);
        }
        
        hdrp->params[w].fraction_bright_pixels.num = get_bits(&gb, 10);
        hdrp->params[w].fraction_bright_pixels.den = 1000;
        av_log(avctx, AV_LOG_TRACE, "hdr10+ fraction_bright_pixels[%d] %d\n",
               w, hdrp->params[w].fraction_bright_pixels.num);
      }

      hdrp->mastering_display_actual_peak_luminance_flag = get_bits(&gb, 1);
      av_log(avctx, AV_LOG_TRACE, 
             "hdr10+ mastering_display_actual_peak_luminance_flag %u\n",
             hdrp->mastering_display_actual_peak_luminance_flag);
      if (hdrp->mastering_display_actual_peak_luminance_flag)
      {
        i_limit = hdrp->num_rows_mastering_display_actual_peak_luminance =
        get_bits(&gb, 5);
        j_limit = hdrp->num_cols_mastering_display_actual_peak_luminance =
        get_bits(&gb, 5);
        av_log(avctx, AV_LOG_TRACE, "hdr10+ num_rows_mastering_display_actual_peak_luminance x num_cols_mastering_display_actual_peak_luminance %u x %u\n", 
               i_limit, j_limit);

        i_limit = i_limit > 25 ? 25 : i_limit;
        j_limit = j_limit > 25 ? 25 : j_limit;
        for (i = 0; i < i_limit; i++)
          for (j = 0; j < j_limit; j++)
          {
            hdrp->mastering_display_actual_peak_luminance[i][j].num = 
            get_bits(&gb, 4);
            hdrp->mastering_display_actual_peak_luminance[i][j].den = 15;
            av_log(avctx, AV_LOG_TRACE, "hdr10+ mastering_display_actual_peak_luminance[%d][%d] %d\n", i, j,
                   hdrp->mastering_display_actual_peak_luminance[i][j].num);
          }
      }

      for (w = 0; w < hdrp->num_windows; w++)
      {
        hdrp->params[w].tone_mapping_flag = get_bits(&gb, 1);
        av_log(avctx, AV_LOG_TRACE, "hdr10+ tone_mapping_flag[%d] %u\n",
               w, hdrp->params[w].tone_mapping_flag);

        if (hdrp->params[w].tone_mapping_flag)
        {
          hdrp->params[w].knee_point_x.num = get_bits(&gb, 12);
          hdrp->params[w].knee_point_x.den = 4095;
          hdrp->params[w].knee_point_y.num = get_bits(&gb, 12);
          hdrp->params[w].knee_point_y.den = 4095;
          av_log(avctx, AV_LOG_TRACE, "hdr10+ knee_point_x[%d] %u\n",
               w, hdrp->params[w].knee_point_x.num);
          av_log(avctx, AV_LOG_TRACE, "hdr10+ knee_point_y[%d] %u\n",
               w, hdrp->params[w].knee_point_y.num);

          hdrp->params[w].num_bezier_curve_anchors = get_bits(&gb, 4);
          av_log(avctx, AV_LOG_TRACE, 
                 "hdr10+ num_bezier_curve_anchors[%d] %u\n",
                 w, hdrp->params[w].num_bezier_curve_anchors);
          for (i = 0; i < hdrp->params[w].num_bezier_curve_anchors; i++)
          {
            hdrp->params[w].bezier_curve_anchors[i].num = get_bits(&gb, 10);
            hdrp->params[w].bezier_curve_anchors[i].den = 1023;
            av_log(avctx, AV_LOG_TRACE, 
                   "hdr10+ bezier_curve_anchors[%d][%d] %d\n",
                   w, i, hdrp->params[w].bezier_curve_anchors[i].num);
          }
        }
          
        hdrp->params[w].color_saturation_mapping_flag = get_bits(&gb, 1);
        av_log(avctx, AV_LOG_TRACE, 
               "hdr10+ color_saturation_mapping_flag[%d] %u\n",
               w, hdrp->params[w].color_saturation_mapping_flag);
        if (hdrp->params[w].color_saturation_mapping_flag)
        {
          hdrp->params[w].color_saturation_weight.num = get_bits(&gb, 6);
          hdrp->params[w].color_saturation_weight.den = 8;
          av_log(avctx, AV_LOG_TRACE, "hdr10+ color_saturation_weight[%d] %d\n",
                 w, hdrp->params[w].color_saturation_weight.num);
        }
      } // num_windows 

    } // correct num_windows
  } // hdr10+ sei

  if (xfme->p_custom_sei)
  {
    AVBufferRef *sei_ref = av_buffer_create(xfme->p_custom_sei,
                                            sizeof(ni_all_custom_sei_t),
                                            ni_free, NULL, 0);
    if (! sei_ref ||
        ! av_frame_new_side_data_from_buf(frame, AV_FRAME_DATA_NETINT_CUSTOM_SEI,
                                    sei_ref))
    {
        return AVERROR(ENOMEM);
    }
    xfme->p_custom_sei = NULL;
  }

  frame->pkt_dts = xfme->pts;
  if (xfme->pts != NI_NOPTS_VALUE)
  {
    frame->pts = xfme->pts;
  }
  else
  {
    s->current_pts += frame->pkt_duration;
    frame->pts = s->current_pts;
  }

  if(is_hw)
  {
    ni_hwframe_surface_t* p_data3;
    p_data3 = (ni_hwframe_surface_t*)((uint8_t*)xfme->p_buffer
                                + xfme->data_len[0] + xfme->data_len[1]
                                + xfme->data_len[2]);
    
    frame->data[3] = (uint8_t*)(xfme->p_buffer + xfme->data_len[0] + xfme->data_len[1] + xfme->data_len[2]);
    
    av_log(avctx, AV_LOG_DEBUG, "retrieve_frame: OUT0 data[3] i8FrameIdx=%d, device_handle=%d bitdep=%d, WxH %d x %d\n",
           p_data3->i8FrameIdx,
           p_data3->device_handle,
           p_data3->bit_depth,
           p_data3->ui16width,
           p_data3->ui16height);
  }

  av_log(avctx, AV_LOG_DEBUG, "retrieve_frame: frame->buf[0]=%p, frame->data=%p, frame->pts=%" PRId64 ", frame size=%d, s->current_pts=%" PRId64 ", frame->pkt_pos=%" PRId64 ", frame->pkt_duration=%" PRId64 " sei size %d offset %u\n", frame->buf[0], frame->data, frame->pts, buf_size, s->current_pts, frame->pkt_pos, frame->pkt_duration, xfme->sei_cc_len, xfme->sei_cc_offset);

  /* av_buffer_ref(avpkt->buf); */
  if (!frame->buf[0])
    return AVERROR(ENOMEM);
  av_log(avctx, AV_LOG_DEBUG, "retrieve_frame: fill array, linesize[0]=%d, fmt=%d, width=%d, height=%d\n",
         frame->linesize[0], avctx->sw_pix_fmt, s->api_ctx.active_video_width, s->api_ctx.active_video_height);
  if (!is_hw && ((res = av_image_fill_arrays(frame->data, frame->linesize,
                                              buf, avctx->sw_pix_fmt,
                                              s->api_ctx.active_video_width,
                                              s->api_ctx.active_video_height, 1)) < 0))
  {
    av_buffer_unref(&frame->buf[0]);
    return res;
  }

  av_log(avctx, AV_LOG_DEBUG, "retrieve_frame: success av_image_fill_arrays "
         "return %d\n", res);
  frame->width = s->api_ctx.active_video_width;
  frame->height = s->api_ctx.active_video_height;
  frame->crop_top = xfme->crop_top;
  frame->crop_bottom = s->api_ctx.active_video_height - xfme->crop_bottom;
  frame->crop_left = xfme->crop_left;
  frame->crop_right = s->api_ctx.active_video_width - xfme->crop_right;

  if (is_hw)
  {
    av_log(avctx, AV_LOG_TRACE, "retrieve_frame: hw frame av_buffer_get_ref_count=%d\n",
           av_buffer_get_ref_count(frame->buf[0]));
    dst_ctx->pc_width = frame->width;
    dst_ctx->pc_height = frame->height;
    dst_ctx->pc_crop_bottom = frame->crop_bottom;
    dst_ctx->pc_crop_right = frame->crop_right;
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
  int width, height, alloc_mem;
  int avctx_bit_depth = 0;
  int ishwframe = (avctx->pix_fmt == AV_PIX_FMT_NI);

  if (s->draining && s->eos)
  {
    return AVERROR_EOF;
  }

  // if active video resolution has been obtained we just use it as it's the 
  // exact size of frame to be returned, otherwise we use what we are told by 
  // upper stream as the initial setting and it will be adjusted.
  width = s->api_ctx.active_video_width > 0 ? s->api_ctx.active_video_width :  avctx->width;
  height = s->api_ctx.active_video_height > 0 ? s->api_ctx.active_video_height : avctx->height;

  // allocate memory only after resolution is known (buffer pool set up)
  alloc_mem = (s->api_ctx.active_video_width > 0) &&
              (s->api_ctx.active_video_height > 0 ? 1 : 0);
  memset(p_session_data, 0, sizeof(ni_session_data_io_t));
  if (!ishwframe)
  {
    ret = ni_decoder_frame_buffer_alloc(
        s->api_ctx.dec_fme_buf_pool, &(p_session_data->data.frame), alloc_mem,
        width, height,
        (avctx->codec_id == AV_CODEC_ID_H264), s->api_ctx.bit_depth_factor);
  }
  else
  {
    ret = ni_frame_buffer_alloc(&(p_session_data->data.frame),
                                width,
                                height,
                                (avctx->codec_id == AV_CODEC_ID_H264),
                                1,
                                s->api_ctx.bit_depth_factor,
                                1);

  }

  if (NI_RETCODE_SUCCESS != ret)
  {
    return AVERROR_EXTERNAL;
  }

  if (!ishwframe)
  {
    ret = ni_device_session_read(&s->api_ctx, p_session_data, NI_DEVICE_TYPE_DECODER);
  }
  else
  {
    ret = ni_device_session_read_hwdesc(&s->api_ctx, p_session_data);
  }
  if (ret == 0)
  {
    s->eos = p_session_data->data.frame.end_of_stream;
    if (!ishwframe)
    {
      ni_decoder_frame_buffer_free(&(p_session_data->data.frame));
    }
    else
    {
      ni_frame_buffer_free(&(p_session_data->data.frame));
    }
    return AVERROR(EAGAIN);
  }
  else if (ret > 0)
  {
    av_log(avctx, AV_LOG_DEBUG, "Got output buffer pts=%lld "
                                  "dts=%lld eos=%d sos=%d\n",
           p_session_data->data.frame.pts, p_session_data->data.frame.dts,
           p_session_data->data.frame.end_of_stream, p_session_data->data.frame.start_of_stream);

    s->eos = p_session_data->data.frame.end_of_stream;

    // update ctxt resolution if change has been detected
    frame->width = p_session_data->data.frame.video_width;
    frame->height = p_session_data->data.frame.video_height;

    if (ishwframe)
    {
      avctx_bit_depth = p_session_data->data.frame.bit_depth;
    }
    else
    {
      avctx_bit_depth = (avctx->pix_fmt == AV_PIX_FMT_YUV420P10LE)?10:8;
    }

    if (frame->width != avctx->width || frame->height != avctx->height || avctx_bit_depth  != p_session_data->data.frame.bit_depth)
    {
      av_log(avctx, AV_LOG_WARNING, "ff_xcoder_dec_receive: sequence "
             "changed: %dx%d %dbits to %dx%d %dbits\n", avctx->width, avctx->height, avctx_bit_depth, 
             frame->width, frame->height, p_session_data->data.frame.bit_depth); 
      avctx->width = frame->width;
      avctx->height = frame->height;

      if (ishwframe)
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

    if (avctx->pix_fmt == AV_PIX_FMT_NI)
    {
      frame->hw_frames_ctx = av_buffer_ref(avctx->hw_frames_ctx);
    }

    retrieve_frame(avctx, frame, &got_frame, &(p_session_data->data.frame));
    
    av_log(avctx, AV_LOG_DEBUG, "ff_xcoder_dec_receive: got_frame=%d, frame->width=%d, frame->height=%d, crop top %" SIZE_SPECIFIER " bottom %" SIZE_SPECIFIER " left %" SIZE_SPECIFIER " right %" SIZE_SPECIFIER ", frame->format=%d, frame->linesize=%d/%d/%d\n", got_frame, frame->width, frame->height, frame->crop_top, frame->crop_bottom, frame->crop_left, frame->crop_right, frame->format, frame->linesize[0], frame->linesize[1], frame->linesize[2]);

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
    av_log(avctx, AV_LOG_DEBUG, "ff_xcoder_dec_receive: pkt_timebase= %d/%d, frame_rate=%d/%d, frame->pts=%" PRId64 ", frame->pkt_dts=%" PRId64 "\n", avctx->pkt_timebase.num, avctx->pkt_timebase.den, avctx->framerate.num, avctx->framerate.den, frame->pts, frame->pkt_dts);

    // release buffer ownership and let frame owner return frame buffer to 
    // buffer pool later
    p_session_data->data.frame.dec_buf = NULL;

    free(p_session_data->data.frame.p_custom_sei);
    p_session_data->data.frame.p_custom_sei = NULL;
  }
  else
  {
    av_log(avctx, AV_LOG_ERROR, "Failed to get output buffer (status = %d)\n",
           ret);
    
    if (NI_RETCODE_ERROR_VPU_RECOVERY == ret)
    {
      av_log(avctx, AV_LOG_WARNING, "ff_xcoder_dec_receive VPU recovery, need to reset ..\n");
      if (avctx->pix_fmt != AV_PIX_FMT_NI)
      {
        ni_decoder_frame_buffer_free(&(p_session_data->data.frame));
      }
      else
      {
        ni_frame_buffer_free(&(p_session_data->data.frame));
      }
      return ret;
    }

    return AVERROR_EOF;
  }

  ret = 0;

  return ret;
}

int ff_xcoder_dec_is_flushing(AVCodecContext *avctx,
                              XCoderH264DecContext *s)
{
  return s->flushing;
}

int ff_xcoder_dec_flush(AVCodecContext *avctx,
                        XCoderH264DecContext *s)
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
