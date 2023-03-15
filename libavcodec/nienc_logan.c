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

#include "libavutil/mastering_display_metadata.h"
#include "libavcodec/put_bits.h"
#include "libavcodec/golomb.h"
#include "libavcodec/hevc.h"
#include "libavcodec/hevc_sei.h"
#include "libavcodec/h264.h"
#include "libavcodec/h264_sei.h"
#include "libavutil/hdr_dynamic_metadata.h"
#include "libavutil/hwcontext.h"
#include "libavutil/hwcontext_internal.h"
#include "libavutil/hwcontext_ni_logan.h"
#include "bytestream.h"
#include "nienc_logan.h"
#include "ni_av_codec_logan.h"

#define ODD2EVEN(X) ((X&1)&&(X>31))?(X+1):(X)
#define BR_SHIFT  6
#define CPB_SHIFT 4

#ifdef NIENC_MULTI_THREAD
threadpool_t pool;
int sessionCounter = 0;

typedef struct _write_thread_arg_struct_t
{
  pthread_mutex_t mutex; //mutex
  pthread_cond_t cond;   //cond
  int running;
  XCoderLoganEncContext *ctx;
  ni_logan_retcode_t ret;
}write_thread_arg_struct_t;
#endif

typedef enum
{
  SLICE_TYPE_B = 0,
  SLICE_TYPE_P = 1,
  SLICE_TYPE_I = 2,
  SLICE_TYPE_MP = 3
} slice_type_t;

typedef enum
{
  GOP_PRESET_CUSTOM        = 0,
  GOP_PRESET_I_1           = 1,
  GOP_PRESET_P_1           = 2,
  GOP_PRESET_B_1           = 3,
  GOP_PRESET_BP_2          = 4,
  GOP_PRESET_BBBP_3        = 5,
  GOP_PRESET_LP_4          = 6,
  GOP_PRESET_LD_4          = 7,
  GOP_PRESET_RA_8          = 8,
  // single_ref
  GOP_PRESET_SP_1          = 9,
  GOP_PRESET_BSP_2         = 10,
  GOP_PRESET_BBBSP_3       = 11,
  GOP_PRESET_LSP_4         = 12,

  // newly added
  GOP_PRESET_BBP_3         = 13,
  GOP_PRESET_BBSP_3        = 14,
  GOP_PRESET_BBBBBBBP_8    = 15,
  GOP_PRESET_BBBBBBBSP_8   = 16,
  NUM_GOP_PRESET_NUM       = 17,
} gop_preset_t;

static const int32_t GOP_SIZE[NUM_GOP_PRESET_NUM] = {0, 1, 1, 1, 2, 4, 4, 4, 8, 1, 2, 4, 4};
static const int32_t LT_GOP_PRESET_I_1[6] = {SLICE_TYPE_I,  1, 0, 0, 0, 0};
static const int32_t LT_GOP_PRESET_P_1[6] = {SLICE_TYPE_MP, 1, 1, 0, 0, -1};
static const int32_t LT_GOP_PRESET_B_1[6] = {SLICE_TYPE_B,  1, 1, 0, 0, -1};
// gop_size = 2
static const int32_t LT_GOP_PRESET_BP_2[12] =
{
  SLICE_TYPE_MP, 2, 1, 0, 0, -2,
  SLICE_TYPE_B,  1, 3, 0, 0, 2,
};
// gop_size = 4
static const int32_t LT_GOP_PRESET_BBBP_4[24] =
{
  SLICE_TYPE_MP, 4, 1, 0, 0, -4,
  SLICE_TYPE_B,  2, 3, 0, 0, 4,
  SLICE_TYPE_B,  1, 5, 0, 0, 2,
  SLICE_TYPE_B,  3, 5, 0, 2, 4,
};

static const int32_t LT_GOP_PRESET_LP_4[24] =
{
  SLICE_TYPE_MP, 1, 5, 0, 0, -4,
  SLICE_TYPE_MP, 2, 3, 0, 1, 0,
  SLICE_TYPE_MP, 3, 5, 0, 2, 0,
  SLICE_TYPE_MP, 4, 1, 0, 3, 0,
};
static const int32_t LT_GOP_PRESET_LD_4[24] =
{
  SLICE_TYPE_B, 1, 5, 0, 0, -4,
  SLICE_TYPE_B, 2, 3, 0, 1, 0,
  SLICE_TYPE_B, 3, 5, 0, 2, 0,
  SLICE_TYPE_B, 4, 1, 0, 3, 0,
};
// gop_size = 8
static const int32_t LT_GOP_PRESET_RA_8[48] =
{
  SLICE_TYPE_B, 8, 1, 0, 0, -8,
  SLICE_TYPE_B, 4, 3, 0, 0, 8,
  SLICE_TYPE_B, 2, 5, 0, 0, 4,
  SLICE_TYPE_B, 1, 8, 0, 0, 2,
  SLICE_TYPE_B, 3, 8, 0, 2, 4,
  SLICE_TYPE_B, 6, 5, 0, 4, 8,
  SLICE_TYPE_B, 5, 8, 0, 4, 6,
  SLICE_TYPE_B, 7, 8, 0, 6, 8,
};
// single-ref-P
static const int32_t LT_GOP_PRESET_SP_1[6] = {SLICE_TYPE_P, 1, 1, 0, 0, -1};

static const int32_t LT_GOP_PRESET_BSP_2[12] =
{
  SLICE_TYPE_P, 2, 1, 0, 0, -2,
  SLICE_TYPE_B, 1, 3, 0, 0, 2,
};
static const int32_t LT_GOP_PRESET_BBBSP_4[24] =
{
  SLICE_TYPE_P, 4, 1, 0, 0, -4,
  SLICE_TYPE_B, 2, 3, 0, 0, 4,
  SLICE_TYPE_B, 1, 5, 0, 0, 2,
  SLICE_TYPE_B, 3, 5, 0, 2, 4,
};
static const int32_t LT_GOP_PRESET_LSP_4[24] =
{
  SLICE_TYPE_P, 1, 5, 0, 0, -4,
  SLICE_TYPE_P, 2, 3, 0, 1, 0,
  SLICE_TYPE_P, 3, 5, 0, 2, 0,
  SLICE_TYPE_P, 4, 1, 0, 3, 0,
};

static const int32_t LT_GOP_PRESET_BBP_3[18] =
{
  SLICE_TYPE_MP, 3, 1, 0, 0, -3,
  SLICE_TYPE_B, 1, 3, 0, 0, 3,
  SLICE_TYPE_B, 2, 6, 0, 1, 3,
};

static const int32_t LT_GOP_PRESET_BBSP_3[18] =
{
  SLICE_TYPE_P, 3, 1, 0, 0, 0,
  SLICE_TYPE_B, 1, 3, 0, 0, 3,
  SLICE_TYPE_B, 2, 6, 0, 1, 3,
};

static const int32_t LT_GOP_PRESET_BBBBBBBP_8[48] =
{
  SLICE_TYPE_MP, 8, 1, 0, 0, -8,
  SLICE_TYPE_B, 4, 3, 0, 0, 8,
  SLICE_TYPE_B, 2, 5, 0, 0, 4,
  SLICE_TYPE_B, 1, 8, 0, 0, 2,
  SLICE_TYPE_B, 3, 8, 0, 2, 4,
  SLICE_TYPE_B, 6, 5, 0, 4, 8,
  SLICE_TYPE_B, 5, 8, 0, 4, 6,
  SLICE_TYPE_B, 7, 8, 0, 6, 8,
};
static const int32_t LT_GOP_PRESET_BBBBBBBSP_8[48] =
{
  SLICE_TYPE_P, 8, 1, 0, 0, 0,
  SLICE_TYPE_B, 4, 3, 0, 0, 8,
  SLICE_TYPE_B, 2, 5, 0, 0, 4,
  SLICE_TYPE_B, 1, 8, 0, 0, 2,
  SLICE_TYPE_B, 3, 8, 0, 2, 4,
  SLICE_TYPE_B, 6, 5, 0, 4, 8,
  SLICE_TYPE_B, 5, 8, 0, 4, 6,
  SLICE_TYPE_B, 7, 8, 0, 6, 8,
};
static const int32_t* GOP_PRESET[NUM_GOP_PRESET_NUM] =
{
  NULL,
  LT_GOP_PRESET_I_1,
  LT_GOP_PRESET_P_1,
  LT_GOP_PRESET_B_1,
  LT_GOP_PRESET_BP_2,
  LT_GOP_PRESET_BBBP_4,
  LT_GOP_PRESET_LP_4,
  LT_GOP_PRESET_LD_4,
  LT_GOP_PRESET_RA_8,

  LT_GOP_PRESET_SP_1,
  LT_GOP_PRESET_BSP_2,
  LT_GOP_PRESET_BBBSP_4,
  LT_GOP_PRESET_LSP_4,

  LT_GOP_PRESET_BBP_3    ,
  LT_GOP_PRESET_BBSP_3   ,
  LT_GOP_PRESET_BBBBBBBP_8 ,
  LT_GOP_PRESET_BBBBBBBSP_8,
};

static void init_gop_param(ni_logan_custom_gop_params_t *gopParam,
                           ni_logan_encoder_params_t *param)
{
  int i;
  int j;
  int gopSize;
  int gopPreset = param->hevc_enc_params.gop_preset_index;

  // GOP_PRESET_IDX_CUSTOM
  if (gopPreset == 0)
  {
    memcpy(gopParam, &param->hevc_enc_params.custom_gop_params,
           sizeof(ni_logan_custom_gop_params_t));
  }
  else
  {
    const int32_t*  src_gop = GOP_PRESET[gopPreset];
    gopSize = GOP_SIZE[gopPreset];
    gopParam->custom_gop_size = gopSize;
    for(i = 0, j = 0; i < gopSize; i++)
    {
      gopParam->pic_param[i].pic_type      = src_gop[j++];
      gopParam->pic_param[i].poc_offset    = src_gop[j++];
      gopParam->pic_param[i].pic_qp        = src_gop[j++] + param->hevc_enc_params.rc.intra_qp;
      gopParam->pic_param[i].temporal_id   = src_gop[j++];
      gopParam->pic_param[i].ref_poc_L0    = src_gop[j++];
      gopParam->pic_param[i].ref_poc_L1    = src_gop[j++];
    }
  }
}

static int check_low_delay_flag(ni_logan_encoder_params_t *param,
                                ni_logan_custom_gop_params_t *gopParam)
{
  int i;
  int minVal = 0;
  int low_delay = 0;
  int gopPreset = param->hevc_enc_params.gop_preset_index;

  // GOP_PRESET_IDX_CUSTOM
  if (gopPreset == 0)
  {
    if (gopParam->custom_gop_size > 1)
    {
      minVal = gopParam->pic_param[0].poc_offset;
      low_delay = 1;
      for (i = 1; i < gopParam->custom_gop_size; i++)
      {
        if (minVal > gopParam->pic_param[i].poc_offset)
        {
          low_delay = 0;
          break;
        }
        else
        {
          minVal = gopParam->pic_param[i].poc_offset;
        }
      }
    }
  }
  else if (gopPreset == 1 || gopPreset == 2 || gopPreset == 3 ||
           gopPreset == 6 || gopPreset == 7 || gopPreset == 9)
  {
    low_delay = 1;
  }

  return low_delay;
}

static int get_num_reorder_of_gop_structure(ni_logan_encoder_params_t *param)
{
  int i;
  int j;
  int ret_num_reorder = 0;
  ni_logan_custom_gop_params_t gopParam;

  init_gop_param(&gopParam, param);
  for(i = 0; i < gopParam.custom_gop_size; i++)
  {
    int check_reordering_num = 0;
    int num_reorder = 0;

    ni_logan_gop_params_t *gopPicParam = &gopParam.pic_param[i];

    for(j = 0; j < gopParam.custom_gop_size; j++)
    {
      ni_logan_gop_params_t *gopPicParamCand = &gopParam.pic_param[j];
      if (gopPicParamCand->poc_offset <= gopPicParam->poc_offset)
        check_reordering_num = j;
    }

    for(j = 0; j < check_reordering_num; j++)
    {
      ni_logan_gop_params_t *gopPicParamCand = &gopParam.pic_param[j];

      if (gopPicParamCand->temporal_id <= gopPicParam->temporal_id &&
          gopPicParamCand->poc_offset > gopPicParam->poc_offset)
        num_reorder++;
    }
    ret_num_reorder = num_reorder;
  }
  return ret_num_reorder;
}

static int get_max_dec_pic_buffering_of_gop_structure(ni_logan_encoder_params_t *param)
{
  int max_dec_pic_buffering;
  max_dec_pic_buffering = FFMIN(16/*MAX_NUM_REF*/, FFMAX(get_num_reorder_of_gop_structure(param) + 2, 6 /*maxnumreference in spec*/) + 1);
  return max_dec_pic_buffering;
}

static int get_poc_of_gop_structure(ni_logan_encoder_params_t *param,
                                    uint32_t frame_idx)
{
  int low_delay;
  int gopSize;
  int poc;
  int gopIdx;
  int gopNum;
  ni_logan_custom_gop_params_t gopParam;

  init_gop_param(&gopParam, param);
  gopSize = gopParam.custom_gop_size;
  low_delay = check_low_delay_flag(param, &gopParam);

  if (low_delay)
  {
    poc = frame_idx;
  }
  else
  {
    gopIdx = frame_idx % gopSize;
    gopNum = frame_idx / gopSize;
    poc = gopParam.pic_param[gopIdx].poc_offset + (gopSize * gopNum);
  }
  //printf("get_poc_of_gop_structure frameIdx=%d, poc=%d, low_delay=%d, gopIdx=%d, gopNum=%d, gopSize=%d \n", frame_idx, poc, low_delay, gopIdx, gopNum, gopSize);

  poc += gopSize - 1; // use gop_size - 1 as offset
  return poc;
}

static inline int calc_scale(uint32_t x)
{
  static uint8_t lut[16] = {4, 0, 1, 0, 2, 0, 1, 0, 3, 0, 1, 0, 2, 0, 1, 0};
  int y, z = (((x & 0xffff) - 1) >> 27) & 16;
  x >>= z;
  z += y = (((x & 0xff) - 1) >> 28) & 8;
  x >>= y;
  z += y = (((x & 0xf) - 1) >> 29) & 4;
  x >>= y;
  return z + lut[x&0xf];
}

static inline int clip3(int min, int max, int a)
{
  return FFMIN(FFMAX(min, a), max);
}

static inline int calc_length(uint32_t x)
{
  static uint8_t lut[16] = {4, 3, 2, 2, 1, 1, 1, 1, 0, 0, 0, 0, 0, 0, 0, 0};
  int y, z = (((x >> 16) - 1) >> 27) & 16;
  x >>= z ^ 16;
  z += y = ((x - 0x100) >> 28) & 8;
  x >>= y ^ 8;
  z += y = ((x - 0x10) >> 29) & 4;
  x >>= y ^ 4;
  return z + lut[x];
}

static uint32_t encode_buffering_period_sei(ni_logan_encoder_params_t *p_param,
                                            XCoderLoganEncContext *ctx,
                                            uint32_t frame_idx,
                                            uint8_t *p_buf)
{
  PutBitContext pbc;
  int32_t payload_bit_size = 0, payload_byte_size = 0, put_bit_byte_size = 0;
  uint32_t nal_initial_cpb_removal_delay, nal_initial_cpb_removal_offset;
  int i;
  uint32_t concatenation_flag = get_poc_of_gop_structure(p_param, frame_idx) == 0 ? 1 : 0;
  if (ctx->api_ctx.frame_num == 0)
  {
    concatenation_flag = 1;
  }
  init_put_bits(&pbc, p_buf, NI_LOGAN_MAX_SEI_DATA);

  payload_bit_size += 1; // bp_seq_parameter_set_id=0, 1 bit
  payload_bit_size += 1; // irap_cpb_params_present_flag=0, 1 bit
  payload_bit_size += 1; // concatenation_flag, 1 bit
  // au_cpb_removal_delay_delta_minus1
  payload_bit_size += (ctx->au_cpb_removal_delay_length_minus1 + 1);

  // nal_hrd_parameters_present_flag=1
  // CpbCnt = cpb_cnt_minus1[0] + 1 = 0 + 1
  for (i = 0; i < 1; i++)
  {
    // nal_initial_cpb_removal_delay
    payload_bit_size += ctx->initial_cpb_removal_delay_length_minus1 + 1;
    // nal_initial_cpb_removal_offset
    payload_bit_size += ctx->initial_cpb_removal_delay_length_minus1 + 1;
  }

  // vcl_hrd_parameters_present_flag=0

  put_bits32(&pbc, 1); // NAL start code
  // NAL unit header nal_unit_type=39, layer_id=0, temporal_id_plus1=1
  put_bits(&pbc, 16, (39 << 9) | (0 << 3) | 1);
  put_bits(&pbc, 8, 0); // payload_type=0 (buffering_period)
  payload_byte_size = (payload_bit_size + 7) / 8;
  put_bits(&pbc, 8, payload_byte_size);// payload size (byte)

  // buffering period
  set_ue_golomb_long(&pbc, 0); // bp_seq_parameter_set_id=0
  put_bits(&pbc, 1, 0); // irap_cpb_params_present_flag=0
  put_bits(&pbc, 1, concatenation_flag); // concatenation_flag
  // au_cpb_removal_delay_delta_minus1=0
  put_bits(&pbc, ctx->au_cpb_removal_delay_length_minus1 + 1, 0);

  nal_initial_cpb_removal_delay =
  (uint32_t)(90000 * ctx->cpb_size_unscale / ctx->bit_rate_unscale);
  nal_initial_cpb_removal_offset =
  (uint32_t)((90000 * ctx->cpb_size_unscale / ctx->bit_rate_unscale) -
             nal_initial_cpb_removal_delay);

  // nal_hrd_parameters_present_flag=1
  // CpbCnt = cpb_cnt_minus1[0] + 1 = 0 + 1
  for (i = 0; i < 1; i++)
  {
    // nal_initial_cpb_removal_delay
    put_bits(&pbc, ctx->initial_cpb_removal_delay_length_minus1 + 1,
             nal_initial_cpb_removal_delay);
    // nal_initial_cpb_removal_offset
    put_bits(&pbc, ctx->initial_cpb_removal_delay_length_minus1 + 1,
             nal_initial_cpb_removal_offset);
  }

  // vcl_hrd_parameters_present_flag=0

  if (payload_bit_size % 8)
  {
    // fill in bit 1 and padding 0s for byte alignment
    put_bits(&pbc, 1, 1/*payload_bit_equal_to_one*/);
    for (i = 0; i < 8 - (payload_bit_size % 8) - 1; i++)
    {
      put_bits(&pbc, 1, 0/*payload_bit_equal_to_zero*/);
    }
  }

  // rbsp trailing stop bit and alignment padding 0s
  put_bits(&pbc, 8, 0x80);

  flush_put_bits(&pbc);
  put_bit_byte_size = (put_bits_count(&pbc) + 7) / 8;

  // emulation prevention checking of payload, skipping start code (4B) +
  // NAL header (2B) + payload type (1B) + payload size (1B) = 8B
  put_bit_byte_size += ni_logan_insert_emulation_prevent_bytes(
    p_buf + 8, put_bit_byte_size - 8);

  return put_bit_byte_size;
}

static uint32_t encode_pic_timing_sei2(ni_logan_encoder_params_t *p_param,
                                       XCoderLoganEncContext *ctx,
                                       uint8_t *p_buf, int is_i_or_idr,
                                       int is_idr, uint32_t frame_idx)
{
  PutBitContext pbc;
  int32_t payload_bit_size = 0, payload_byte_size = 0, put_bit_byte_size = 0;
  uint32_t pic_dpb_output_delay = 0;
  int num_reorder_pic;
  uint64_t poc_pic;

  init_put_bits(&pbc, p_buf, NI_LOGAN_MAX_SEI_DATA);

  // frame_field_info_present_flag=0 TBD
  //payload_bit_size += 4/*pic_struct*/ + 2/*source_scan_type*/ + 1/*duplicate_flag*/;

  // CpbDpbDelaysPresentFlag=1
  // au_cpb_removal_delay_length_minus1
  payload_bit_size += (ctx->au_cpb_removal_delay_length_minus1 + 1);
  // pic_dpb_output_delay
  payload_bit_size += (ctx->dpb_output_delay_length_minus1+ 1);

  // sub_pic_hrd_params_present_flag=0

  put_bits32(&pbc, 1); // NAL start code
  // NAL unit header nal_unit_type=39, layer_id=0, temporal_id_plus1=1
  put_bits(&pbc, 16, (39 << 9) | (0 << 3) | 1);
  put_bits(&pbc, 8, 1); // payload_type=1 (picture_timing)
  payload_byte_size = (payload_bit_size + 7) / 8;
  put_bits(&pbc, 8, payload_byte_size);// payload size (byte)

  // pic timing

  num_reorder_pic = get_num_reorder_of_gop_structure(p_param);
  poc_pic = get_poc_of_gop_structure(p_param, frame_idx);
  pic_dpb_output_delay = num_reorder_pic + poc_pic - frame_idx;

  //printf(" ----> num_reorder_pic %d + poc_pic %llu - frame_idx %u\n", num_reorder_pic,
  //poc_pic, frame_idx);
  //printf(" ----> #%u %s au_cpb_removal_delay_minus1 %u  pic_dpb_output_delay %u\n", frame_idx, is_idr ? "is_idr" : " ", ctx->au_cpb_removal_delay_minus1, pic_dpb_output_delay);

  // CpbDpbDelaysPresentFlag=1
  // au_cpb_removal_delay_length_minus1
  put_bits(&pbc, ctx->au_cpb_removal_delay_length_minus1 + 1,
           ctx->au_cpb_removal_delay_minus1);
  ctx->au_cpb_removal_delay_minus1++;

  if (1 == p_param->hevc_enc_params.gop_preset_index &&
      p_param->hevc_enc_params.intra_period)
  {
    if (0 == frame_idx || is_idr ||
        0 == (ctx->au_cpb_removal_delay_minus1 % p_param->hevc_enc_params.intra_period))
    {
      ctx->au_cpb_removal_delay_minus1 = 0;
    }
  }
  else if (is_i_or_idr)
  {
    ctx->au_cpb_removal_delay_minus1 = 0;
  }

  // pic_dpb_output_delay
  put_bits(&pbc, ctx->dpb_output_delay_length_minus1 + 1, pic_dpb_output_delay);

  if (payload_bit_size & 7)
  {
    put_bits(&pbc, 1, 1/*payload_bit_equal_to_one*/);
    put_bits(&pbc, (8 - (payload_bit_size & 7)-1), 0/*payload_bit_equal_to_zero*/);
  }

  // rbsp trailing stop bit and alignment padding 0s
  put_bits(&pbc, 8, 0x80);

  flush_put_bits(&pbc);
  put_bit_byte_size = (put_bits_count(&pbc) + 7) / 8;

  // emulation prevention checking of payload, skipping start code (4B) +
  // NAL header (2B) + payload type (1B) + payload size (1B) = 8B
  put_bit_byte_size += ni_logan_insert_emulation_prevent_bytes(
    p_buf + 8, put_bit_byte_size - 8);

  return put_bit_byte_size;
}

#define SAMPLE_SPS_MAX_SUB_LAYERS_MINUS1 0
#define MAX_VPS_MAX_SUB_LAYERS 16
#define MAX_CPB_COUNT 16
#define MAX_DURATION 0.5

static void set_vui(AVCodecContext *avctx, ni_logan_encoder_params_t *p_param,
                    XCoderLoganEncContext *ctx,
                    enum AVColorPrimaries color_primaries,
                    enum AVColorTransferCharacteristic color_trc,
                    enum AVColorSpace color_space,
                    int video_full_range_flag)
{
  int isHEVC = (AV_CODEC_ID_HEVC == avctx->codec_id ? 1 : 0);
  PutBitContext pbcPutBitContext;
  unsigned int aspect_ratio_idc = 255; // default: extended_sar
  int nal_hrd_parameters_present_flag=1, vcl_hrd_parameters_present_flag=0;
  int layer, cpb;
  int maxcpboutputdelay;
  int maxdpboutputdelay;
  int maxdelay;
  uint32_t vbvbuffersize = (p_param->bitrate / 1000) * p_param->hevc_enc_params.rc.rc_init_delay;
  uint32_t vbvmaxbitrate = p_param->bitrate;
  uint32_t vps_max_sub_layers_minus1 = SAMPLE_SPS_MAX_SUB_LAYERS_MINUS1;
  uint32_t bit_rate_value_minus1[MAX_CPB_COUNT][MAX_VPS_MAX_SUB_LAYERS];
  uint32_t cpb_size_value_minus1[MAX_CPB_COUNT][MAX_VPS_MAX_SUB_LAYERS];
  uint32_t cpb_cnt_minus1[MAX_VPS_MAX_SUB_LAYERS];

  uint32_t fixed_pic_rate_general_flag[MAX_VPS_MAX_SUB_LAYERS];
  uint32_t fixed_pic_rate_within_cvs_flag[MAX_VPS_MAX_SUB_LAYERS];
  uint32_t elemental_duration_in_tc_minus1[MAX_VPS_MAX_SUB_LAYERS];

  uint32_t bit_rate_scale = 2;
  uint32_t cpb_size_scale = 5;
  uint32_t numUnitsInTick = 1000;
  uint32_t timeScale;
  int32_t i32frameRateInfo = p_param->hevc_enc_params.frame_rate;

  init_put_bits(&pbcPutBitContext, p_param->ui8VuiRbsp, NI_LOGAN_MAX_VUI_SIZE);

  if (avctx->sample_aspect_ratio.num==0)
  {
    // sample aspect ratio is 0, don't include aspect_ratio_idc in vui
    put_bits(&pbcPutBitContext, 1, 0);  //  aspect_ratio_info_present_flag=0
  }
  else
  {
    // sample aspect ratio is non-zero, include aspect_ratio_idc in vui
    put_bits(&pbcPutBitContext, 1, 1);  //  aspect_ratio_info_present_flag=1

    if (! av_cmp_q(avctx->sample_aspect_ratio, av_make_q(1, 1)))
    {
      aspect_ratio_idc = 1;
    }
    else if (! av_cmp_q(avctx->sample_aspect_ratio, av_make_q(12, 11)))
    {
      aspect_ratio_idc = 2;
    }
    else if (! av_cmp_q(avctx->sample_aspect_ratio, av_make_q(10, 11)))
    {
      aspect_ratio_idc = 3;
    }
    else if (! av_cmp_q(avctx->sample_aspect_ratio, av_make_q(16, 11)))
    {
      aspect_ratio_idc = 4;
    }
    else if (! av_cmp_q(avctx->sample_aspect_ratio, av_make_q(40, 33)))
    {
      aspect_ratio_idc = 5;
    }
    else if (! av_cmp_q(avctx->sample_aspect_ratio, av_make_q(24, 11)))
    {
      aspect_ratio_idc = 6;
    }
    else if (! av_cmp_q(avctx->sample_aspect_ratio, av_make_q(20, 11)))
    {
      aspect_ratio_idc = 7;
    }
    else if (! av_cmp_q(avctx->sample_aspect_ratio, av_make_q(32, 11)))
    {
      aspect_ratio_idc = 8;
    }
    else if (! av_cmp_q(avctx->sample_aspect_ratio, av_make_q(80, 33)))
    {
      aspect_ratio_idc = 9;
    }
    else if (! av_cmp_q(avctx->sample_aspect_ratio, av_make_q(18, 11)))
    {
      aspect_ratio_idc = 10;
    }
    else if (! av_cmp_q(avctx->sample_aspect_ratio, av_make_q(15, 11)))
    {
      aspect_ratio_idc = 11;
    }
    else if (! av_cmp_q(avctx->sample_aspect_ratio, av_make_q(64, 33)))
    {
      aspect_ratio_idc = 12;
    }
    else if (! av_cmp_q(avctx->sample_aspect_ratio, av_make_q(160, 99)))
    {
      aspect_ratio_idc = 13;
    }
    else if (! av_cmp_q(avctx->sample_aspect_ratio, av_make_q(4, 3)))
    {
      aspect_ratio_idc = 14;
    }
    else if (! av_cmp_q(avctx->sample_aspect_ratio, av_make_q(3, 2)))
    {
      aspect_ratio_idc = 15;
    }
    else if (! av_cmp_q(avctx->sample_aspect_ratio, av_make_q(2, 1)))
    {
      aspect_ratio_idc = 16;
    }

    put_bits(&pbcPutBitContext, 8, aspect_ratio_idc);  // aspect_ratio_idc
    if (255 == aspect_ratio_idc)
    {
      put_bits(&pbcPutBitContext, 16, avctx->sample_aspect_ratio.num);//sar_width
      put_bits(&pbcPutBitContext, 16, avctx->sample_aspect_ratio.den);//sar_height
    }
  }

  put_bits(&pbcPutBitContext, 1, 0);  //  overscan_info_present_flag=0

  // VUI Parameters
  put_bits(&pbcPutBitContext, 1, 1);  //  video_signal_type_present_flag=1
  put_bits(&pbcPutBitContext, 3, 5);  //  video_format=5 (unspecified)
  put_bits(&pbcPutBitContext, 1, video_full_range_flag);
  put_bits(&pbcPutBitContext, 1, 1);  //  colour_description_presenty_flag=1
  put_bits(&pbcPutBitContext, 8, color_primaries);
  put_bits(&pbcPutBitContext, 8, color_trc);
  put_bits(&pbcPutBitContext, 8, color_space);

  put_bits(&pbcPutBitContext, 1, 0);      //  chroma_loc_info_present_flag=0

  if (isHEVC)
  {   // H.265 Only VUI parameters
    put_bits(&pbcPutBitContext, 1, 0);  //  neutral_chroma_indication_flag=0
    put_bits(&pbcPutBitContext, 1, 0);  //  field_seq_flag=0
    put_bits(&pbcPutBitContext, 1, 0);  //  frame_field_info_present_flag=0
    put_bits(&pbcPutBitContext, 1, 0);  //  default_display_window_flag=0
  }

  put_bits(&pbcPutBitContext, 1, 1);      //  vui_timing_info_present_flag=1
  p_param->pos_num_units_in_tick = put_bits_count(&pbcPutBitContext);
  put_bits32(&pbcPutBitContext, 0);    //  vui_num_units_in_tick
  p_param->pos_time_scale = put_bits_count(&pbcPutBitContext);
  put_bits32(&pbcPutBitContext, 0);         //  vui_time_scale

  if (isHEVC)
  {
    // H.265 Only VUI parameters
    put_bits(&pbcPutBitContext, 1, 0);  //  vui_poc_proportional_to_timing_flag=0
    if (! p_param->hrd_enable)
    {
      put_bits(&pbcPutBitContext, 1, 0);  //  vui_hrd_parameters_present_flag=0
    }
    else
    {
      put_bits(&pbcPutBitContext, 1, 1);  //  vui_hrd_parameters_present_flag=1

      put_bits(&pbcPutBitContext, 1, 1); // nal_hrd_parameters_present_flag=1
      put_bits(&pbcPutBitContext, 1, 0); // vcl_hrd_parameters_present_flag=0

      put_bits(&pbcPutBitContext, 1, 0); // sub_pic_hrd_params_present_flag=0

      ctx->initial_cpb_removal_delay_length_minus1 = 23;
      ctx->au_cpb_removal_delay_length_minus1 = 23;

      bit_rate_value_minus1[0][0] = 59374;
      cpb_size_value_minus1[0][0] = 59374;
      cpb_cnt_minus1[0] = 0;
      fixed_pic_rate_general_flag[0] = 1;
      fixed_pic_rate_within_cvs_flag[0] = 1;
      elemental_duration_in_tc_minus1[0] = 0;

      // normalize hrd size and rate to the value / scale notation
      bit_rate_scale = clip3(0, 15, calc_scale(vbvmaxbitrate) - BR_SHIFT);
      bit_rate_value_minus1[0][0] = (vbvmaxbitrate >> (bit_rate_scale + BR_SHIFT)) - 1;

      cpb_size_scale = clip3(0, 15, calc_scale(vbvbuffersize) - CPB_SHIFT);
      cpb_size_value_minus1[0][0] = (vbvbuffersize >> (cpb_size_scale + CPB_SHIFT)) - 1;

      ctx->bit_rate_unscale = (bit_rate_value_minus1[0][0]+1) << (bit_rate_scale + BR_SHIFT);
      ctx->cpb_size_unscale = (cpb_size_value_minus1[0][0]+1) << (cpb_size_scale + CPB_SHIFT);

      if (p_param->fps_denominator != 0 &&
          (p_param->fps_number % p_param->fps_denominator) != 0)
      {
        numUnitsInTick += 1;
        i32frameRateInfo += 1;
      }
      timeScale = i32frameRateInfo * 1000;

      maxcpboutputdelay = (int)(FFMIN(p_param->hevc_enc_params.intra_period * MAX_DURATION * timeScale / numUnitsInTick, INT_MAX));
      maxdpboutputdelay = (int)(get_max_dec_pic_buffering_of_gop_structure(p_param) * MAX_DURATION * timeScale / numUnitsInTick);
      maxdelay = (int)(90000.0 * ctx->cpb_size_unscale / ctx->bit_rate_unscale + 0.5);

      ctx->initial_cpb_removal_delay_length_minus1 =
      2 + clip3(4, 22, 32 - calc_length(maxdelay)) - 1;
      ctx->au_cpb_removal_delay_length_minus1 =
      clip3(4, 31, 32 - calc_length(maxcpboutputdelay)) - 1;
      ctx->dpb_output_delay_length_minus1 =
      clip3(4, 31, 32 - calc_length(maxdpboutputdelay)) - 1;

      put_bits(&pbcPutBitContext, 4, bit_rate_scale); // bit_rate_scale
      put_bits(&pbcPutBitContext, 4, cpb_size_scale); // cpb_size_scale

      put_bits(&pbcPutBitContext, 5, ctx->initial_cpb_removal_delay_length_minus1);
      put_bits(&pbcPutBitContext, 5, ctx->au_cpb_removal_delay_length_minus1);
      put_bits(&pbcPutBitContext, 5, ctx->dpb_output_delay_length_minus1);

      for (layer = 0; layer <= (int32_t)vps_max_sub_layers_minus1; layer++)
      {
        put_bits(&pbcPutBitContext, 1, fixed_pic_rate_general_flag[layer]);

        if (! fixed_pic_rate_general_flag[layer])
        {
          put_bits(&pbcPutBitContext, 1, fixed_pic_rate_within_cvs_flag[layer]);
        }

        if (fixed_pic_rate_within_cvs_flag[layer])
        {
          set_ue_golomb_long(&pbcPutBitContext,
                             elemental_duration_in_tc_minus1[layer]);
        }

        // low_delay_hrd_flag[layer] is not present and inferred to be 0

        set_ue_golomb_long(&pbcPutBitContext, cpb_cnt_minus1[layer]);

        if ((layer == 0 && nal_hrd_parameters_present_flag) ||
            (layer == 1 && vcl_hrd_parameters_present_flag))
        {
          for(cpb = 0; cpb <= (int32_t)cpb_cnt_minus1[layer]; cpb++)
          {
            set_ue_golomb_long(&pbcPutBitContext,
                               bit_rate_value_minus1[cpb][layer]);

            set_ue_golomb_long(&pbcPutBitContext,
                               cpb_size_value_minus1[cpb][layer]);

            // cbr_flag is inferred to be 0 as well ?
            put_bits(&pbcPutBitContext, 1, 0/*cbr_flag[cpb][layer]*/);
          }
        }
      }
    }
    put_bits(&pbcPutBitContext, 1, 0);      //  bitstream_restriction_flag=0
  }
  else
  {
    int max_num_reorder_frames;
    int num_ref_frames;
    int max_dec_frame_buffering;
    // H.264 Only VUI parameters
    if (p_param->enable_vfr)
    {
      put_bits(&pbcPutBitContext, 1, 0);  //  fixed_frame_rate_flag=0
    }
    else
    {
      put_bits(&pbcPutBitContext, 1, 1);  //  fixed_frame_rate_flag=1
    }
    put_bits(&pbcPutBitContext, 1, 0);  //  nal_hrd_parameters_present_flag=0
    put_bits(&pbcPutBitContext, 1, 0);  //  vui_hrd_parameters_present_flag=0
    put_bits(&pbcPutBitContext, 1, 0);  //  pic_struct_present_flag=0

    // this flag is set to 1 for H.264 to reduce decode delay, and fill in
    // the rest of the section accordingly
    put_bits(&pbcPutBitContext, 1, 1);  //  bitstream_restriction_flag=1
    put_bits(&pbcPutBitContext, 1, 1);  //  motion_vectors_over_pic_boundaries_flag=1
    set_ue_golomb_long(&pbcPutBitContext, 2); // max_bytes_per_pic_denom=2 (default)
    set_ue_golomb_long(&pbcPutBitContext, 1); // max_bits_per_mb_denom=1 (default)
    set_ue_golomb_long(&pbcPutBitContext, 15); // log2_max_mv_length_horizontal=15 (default)
    set_ue_golomb_long(&pbcPutBitContext, 15); // log2_max_mv_length_vertical=15 (default)

    // max_num_reorder_frames (0 for low delay gops)
    max_num_reorder_frames = ni_logan_get_num_reorder_of_gop_structure(p_param);
    set_ue_golomb_long(&pbcPutBitContext, max_num_reorder_frames);
    // max_dec_frame_buffering
    num_ref_frames = ni_logan_get_num_ref_frame_of_gop_structure(p_param);
    max_dec_frame_buffering = (num_ref_frames > max_num_reorder_frames ?
                               num_ref_frames : max_num_reorder_frames);
    set_ue_golomb_long(&pbcPutBitContext, max_dec_frame_buffering);
  }

  p_param->ui32VuiDataSizeBits = put_bits_count(&pbcPutBitContext);
  p_param->ui32VuiDataSizeBytes = (p_param->ui32VuiDataSizeBits + 7) / 8;
  flush_put_bits(&pbcPutBitContext);      // flush bits
}

static int do_open_encoder_device(AVCodecContext *avctx,
                                  XCoderLoganEncContext *ctx,
                                  ni_logan_encoder_params_t *p_param)
{
  int ret;
  int frame_width;
  int frame_height;
  int linesize_aligned;
  int height_aligned;
  int video_full_range_flag = 0;
  AVFrame *in_frame = &ctx->buffered_fme;
  NILOGANFramesContext *nif_src_ctx;
  AVHWFramesContext *avhwf_ctx;
  enum AVColorPrimaries color_primaries;
  enum AVColorTransferCharacteristic color_trc;
  enum AVColorSpace color_space;

  if (in_frame->width > 0 && in_frame->height > 0)
  {
    frame_width = ODD2EVEN(in_frame->width);
    frame_height = ODD2EVEN(in_frame->height);
    color_primaries = in_frame->color_primaries;
    color_trc = in_frame->color_trc;
    color_space = in_frame->colorspace;
    // Force frame color metrics if specified in command line
    if (in_frame->color_primaries != avctx->color_primaries &&
        avctx->color_primaries != AVCOL_PRI_UNSPECIFIED)
    {
      color_primaries = avctx->color_primaries;
    }
    if (in_frame->color_trc != avctx->color_trc &&
        avctx->color_trc != AVCOL_TRC_UNSPECIFIED)
    {
      color_trc = avctx->color_trc;
    }
    if (in_frame->colorspace != avctx->colorspace &&
        avctx->colorspace != AVCOL_SPC_UNSPECIFIED)
    {
      color_space = avctx->colorspace;
    }
  }
  else
  {
    frame_width = ODD2EVEN(avctx->width);
    frame_height = ODD2EVEN(avctx->height);
    color_primaries = avctx->color_primaries;
    color_trc = avctx->color_trc;
    color_space = avctx->colorspace;
  }

  // if frame stride size is not as we expect it,
  // adjust using xcoder-params conf_win_right
  linesize_aligned = ((frame_width + 7) / 8) * 8;
  if (avctx->codec_id == AV_CODEC_ID_H264)
  {
    linesize_aligned = ((frame_width + 15) / 16) * 16;
  }

  if (linesize_aligned < NI_LOGAN_MIN_WIDTH)
  {
    p_param->hevc_enc_params.conf_win_right += NI_LOGAN_MIN_WIDTH - frame_width;
    linesize_aligned = NI_LOGAN_MIN_WIDTH;
  }
  else if (linesize_aligned > frame_width)
  {
    p_param->hevc_enc_params.conf_win_right += linesize_aligned - frame_width;
  }
  p_param->source_width = linesize_aligned;

  height_aligned = ((frame_height + 7) / 8) * 8;
  if (avctx->codec_id == AV_CODEC_ID_H264)
  {
    height_aligned = ((frame_height + 15) / 16) * 16;
  }

  if (height_aligned < NI_LOGAN_MIN_HEIGHT)
  {
    p_param->hevc_enc_params.conf_win_bottom += NI_LOGAN_MIN_HEIGHT - frame_height;
    p_param->source_height = NI_LOGAN_MIN_HEIGHT;
    height_aligned = NI_LOGAN_MIN_HEIGHT;
  }
  else if (height_aligned > frame_height)
  {
    p_param->hevc_enc_params.conf_win_bottom += height_aligned - frame_height;
    p_param->source_height = height_aligned;
  }

  // DolbyVision support
  if (5 == p_param->dolby_vision_profile &&
      AV_CODEC_ID_HEVC == avctx->codec_id)
  {
    color_primaries = color_trc = color_space = 2;
    video_full_range_flag = 1;
  }

  // According to the pixel format or color range from the incoming video
  if (avctx->color_range == AVCOL_RANGE_JPEG ||
      avctx->pix_fmt == AV_PIX_FMT_YUVJ420P)
  {
    av_log(avctx, AV_LOG_DEBUG, "%s set video_full_range_flag\n", __FUNCTION__);
    video_full_range_flag = 1;
  }

  // HDR HLG support
  if ((5 == p_param->dolby_vision_profile &&
       AV_CODEC_ID_HEVC == avctx->codec_id) ||
      color_primaries == AVCOL_PRI_BT2020 ||
      color_trc == AVCOL_TRC_SMPTE2084 ||
      color_trc == AVCOL_TRC_ARIB_STD_B67 ||
      color_space == AVCOL_SPC_BT2020_NCL ||
      color_space == AVCOL_SPC_BT2020_CL)
  {
    p_param->hdrEnableVUI = 1;
    set_vui(avctx, p_param, ctx,
           color_primaries, color_trc, color_space, video_full_range_flag);
    av_log(avctx, AV_LOG_VERBOSE, "XCoder HDR color info color_primaries: %d "
           "color_trc: %d color_space %d video_full_range_flag %d sar %d/%d\n",
           color_primaries, color_trc, color_space, video_full_range_flag,
           avctx->sample_aspect_ratio.num, avctx->sample_aspect_ratio.den);
  }
  else
  {
    p_param->hdrEnableVUI = 0;
    set_vui(avctx, p_param, ctx,
            color_primaries, color_trc, color_space, video_full_range_flag);
  }

  ctx->api_ctx.hw_id = ctx->dev_enc_idx;
  strcpy(ctx->api_ctx.dev_xcoder, ctx->dev_xcoder);

  if (in_frame->width > 0 && in_frame->height > 0)
  {
    av_log(avctx, AV_LOG_VERBOSE, "XCoder buffered_fme.linesize: %d/%d/%d "
           "width/height %dx%d conf_win_right %d  conf_win_bottom %d , "
           "color primaries %u trc %u space %u\n",
           in_frame->linesize[0], in_frame->linesize[1], in_frame->linesize[2],
           in_frame->width, in_frame->height,
           p_param->hevc_enc_params.conf_win_right,
           p_param->hevc_enc_params.conf_win_bottom,
           color_primaries, color_trc, color_space);

    if (in_frame->format == AV_PIX_FMT_NI_LOGAN)
    {
      ni_logan_hwframe_surface_t *surface = (ni_logan_hwframe_surface_t *)in_frame->data[3];
#ifdef _WIN32
      int64_t handle = (((int64_t) surface->device_handle_ext) << 32) | surface->device_handle;
      ctx->api_ctx.sender_handle = (ni_device_handle_t) handle;
#else
      ctx->api_ctx.sender_handle = (ni_device_handle_t) surface->device_handle;
#endif
      ctx->api_ctx.hw_action = NI_LOGAN_CODEC_HW_ENABLE;
      av_log(avctx, AV_LOG_VERBOSE, "XCoder frame sender_handle:%p, hw_id:%d\n",
             (void *) ctx->api_ctx.sender_handle, ctx->api_ctx.hw_id);
    }

    if (in_frame->hw_frames_ctx && ctx->api_ctx.hw_id == -1)
    {
      avhwf_ctx = (AVHWFramesContext*) in_frame->hw_frames_ctx->data;
      nif_src_ctx = avhwf_ctx->internal->priv;
      ctx->api_ctx.hw_id = nif_src_ctx->api_ctx.hw_id;
      av_log(avctx, AV_LOG_VERBOSE, "%s: hw_id -1 collocated to %d \n",
             __FUNCTION__, ctx->api_ctx.hw_id);
    }
  }
  else
  {
    av_log(avctx, AV_LOG_VERBOSE, "XCoder frame width/height %dx%d conf_win_right"
           " %d  conf_win_bottom %d color primaries %u trc %u space %u\n",
           avctx->width, avctx->height, p_param->hevc_enc_params.conf_win_right,
           p_param->hevc_enc_params.conf_win_bottom, avctx->color_primaries,
           avctx->color_trc, avctx->colorspace);
  }

  ret = ni_logan_device_session_open(&ctx->api_ctx, NI_LOGAN_DEVICE_TYPE_ENCODER);
  // As the file handle may change we need to assign back
  ctx->dev_xcoder_name = ctx->api_ctx.dev_xcoder_name;
  ctx->blk_xcoder_name = ctx->api_ctx.blk_xcoder_name;
  ctx->dev_enc_idx = ctx->api_ctx.hw_id;

  if (ret == NI_LOGAN_RETCODE_INVALID_PARAM)
  {
    av_log(avctx, AV_LOG_ERROR, "%s\n", ctx->api_ctx.param_err_msg);
  }
  if (ret != 0)
  {
    av_log(avctx, AV_LOG_ERROR, "Failed to open encoder (status = %d), "
           "critical error or resource unavailable\n", ret);
    ret = AVERROR_EXTERNAL;
    // ff_xcoder_logan_encode_close(avctx); will be called at codec close
    return ret;
  }
  else
  {
    av_log(avctx, AV_LOG_VERBOSE, "XCoder %s Index %d (inst: %d) opened "
           "successfully\n", ctx->dev_xcoder_name, ctx->dev_enc_idx,
           ctx->api_ctx.session_id);
  }

  return ret;
}

static void do_close_encoder_device(XCoderLoganEncContext *ctx)
{
  ni_logan_device_session_close(&ctx->api_ctx, ctx->encoder_eof,
                          NI_LOGAN_DEVICE_TYPE_ENCODER);
#ifdef _WIN32
  ni_logan_device_close(ctx->api_ctx.device_handle);
#elif __linux__
  ni_logan_device_close(ctx->api_ctx.device_handle);
  ni_logan_device_close(ctx->api_ctx.blk_io_handle);
#endif
  ctx->api_ctx.device_handle = NI_INVALID_DEVICE_HANDLE;
  ctx->api_ctx.blk_io_handle = NI_INVALID_DEVICE_HANDLE;
  ctx->api_ctx.auto_dl_handle = NI_INVALID_DEVICE_HANDLE;
  ctx->api_ctx.sender_handle = NI_INVALID_DEVICE_HANDLE;
}

static int xcoder_logan_encoder_headers(AVCodecContext *avctx)
{
  // use a copy of encoder context, take care to restore original config
  // cropping setting
  int ret, recv, orig_conf_win_right, orig_conf_win_bottom;
  ni_logan_packet_t *xpkt;
  XCoderLoganEncContext ctx;
  XCoderLoganEncContext *s = avctx->priv_data;
  ni_logan_encoder_params_t *p_param = &s->api_param;

  memcpy(&ctx, avctx->priv_data, sizeof(XCoderLoganEncContext));

  orig_conf_win_right = p_param->hevc_enc_params.conf_win_right;
  orig_conf_win_bottom = p_param->hevc_enc_params.conf_win_bottom;

  ret = do_open_encoder_device(avctx, &ctx, p_param);
  if (ret < 0)
  {
    return ret;
  }

  xpkt = &ctx.api_pkt.data.packet;
  ni_logan_packet_buffer_alloc(xpkt, NI_LOGAN_MAX_TX_SZ);

  for (; ;)
  {
    recv = ni_logan_device_session_read(&ctx.api_ctx, &(ctx.api_pkt),
                                  NI_LOGAN_DEVICE_TYPE_ENCODER);

    if (recv > 0)
    {
      free(avctx->extradata);
      avctx->extradata_size = recv - NI_LOGAN_FW_ENC_BITSTREAM_META_DATA_SIZE;
      avctx->extradata = av_mallocz(avctx->extradata_size +
                                    AV_INPUT_BUFFER_PADDING_SIZE);
      memcpy(avctx->extradata,
             (uint8_t*)xpkt->p_data + NI_LOGAN_FW_ENC_BITSTREAM_META_DATA_SIZE,
             avctx->extradata_size);

      av_log(avctx, AV_LOG_VERBOSE, "%s len: %d\n",
             __FUNCTION__, avctx->extradata_size);
      break;
    }
    else if (recv == NI_LOGAN_RETCODE_SUCCESS)
    {
      continue;
    }
    else
    {
      av_log(avctx, AV_LOG_ERROR, "%s error: %d", __FUNCTION__, recv);
      break;
    }
  }

  do_close_encoder_device(&ctx);

  ni_logan_packet_buffer_free(&ctx.api_pkt.data.packet);
  ni_logan_rsrc_free_device_context(ctx.rsrc_ctx);
  ctx.rsrc_ctx = NULL;

  p_param->hevc_enc_params.conf_win_right = orig_conf_win_right;
  p_param->hevc_enc_params.conf_win_bottom = orig_conf_win_bottom;

  return (recv < 0 ? recv : ret);
}

static int xcoder_logan_setup_encoder(AVCodecContext *avctx)
{
  XCoderLoganEncContext *s = avctx->priv_data;
  int i, ret = 0;
  ni_logan_encoder_params_t *p_param = &s->api_param;
  ni_logan_encoder_params_t *pparams = NULL;
  ni_logan_session_run_state_t prev_state = s->api_ctx.session_run_state;

  av_log(avctx, AV_LOG_DEBUG, "%s\n", __FUNCTION__);
  //s->api_ctx.session_id = NI_LOGAN_INVALID_SESSION_ID;
  ni_logan_device_session_context_init(&(s->api_ctx));
  s->api_ctx.session_run_state = prev_state;

  s->api_ctx.codec_format = NI_LOGAN_CODEC_FORMAT_H264;
  if (avctx->codec_id == AV_CODEC_ID_HEVC)
  {
    s->api_ctx.codec_format = NI_LOGAN_CODEC_FORMAT_H265;
  }

  s->firstPktArrived = 0;
  s->spsPpsArrived = 0;
  s->spsPpsHdrLen = 0;
  s->p_spsPpsHdr = NULL;
  s->xcode_load_pixel = 0;
  s->reconfigCount = 0;
  s->gotPacket = 0;
  s->sentFrame = 0;
  s->latest_dts = 0;

  if (! s->vpu_reset &&
      LOGAN_SESSION_RUN_STATE_SEQ_CHANGE_DRAINING != s->api_ctx.session_run_state)
  {
    av_log(avctx, AV_LOG_INFO, "Session state: %d allocate frame fifo.\n",
           s->api_ctx.session_run_state);
    // FIFO 4 * FPS length of frames
    s->fme_fifo_capacity = 4 * avctx->time_base.den / (avctx->time_base.num * avctx->ticks_per_frame);
    s->fme_fifo = av_fifo_alloc(s->fme_fifo_capacity * sizeof(AVFrame));
  }
  else
  {
    av_log(avctx, AV_LOG_INFO, "Session seq change, fifo size: %" PRIu64 "\n",
           av_fifo_size(s->fme_fifo) / sizeof(AVFrame));
  }

  if (! s->fme_fifo)
  {
    return AVERROR(ENOMEM);
  }
  s->eos_fme_received = 0;

  //Xcoder User Configuration
  ret = ni_logan_encoder_init_default_params(p_param, avctx->framerate.num, avctx->framerate.den,
                                       avctx->bit_rate, ODD2EVEN(avctx->width),
                                       ODD2EVEN(avctx->height));
  if (ret == NI_LOGAN_RETCODE_PARAM_ERROR_WIDTH_TOO_BIG)
  {
    av_log(avctx, AV_LOG_ERROR, "Invalid Picture Width: too big\n");
    return AVERROR_EXTERNAL;
  }
  if (ret == NI_LOGAN_RETCODE_PARAM_ERROR_WIDTH_TOO_SMALL)
  {
    av_log(avctx, AV_LOG_ERROR, "Invalid Picture Width: too small\n");
    return AVERROR_EXTERNAL;
  }
  if (ret == NI_LOGAN_RETCODE_PARAM_ERROR_HEIGHT_TOO_BIG)
  {
    av_log(avctx, AV_LOG_ERROR, "Invalid Picture Height: too big\n");
    return AVERROR_EXTERNAL;
  }
  if (ret == NI_LOGAN_RETCODE_PARAM_ERROR_HEIGHT_TOO_SMALL)
  {
    av_log(avctx, AV_LOG_ERROR, "Invalid Picture Height: too small\n");
    return AVERROR_EXTERNAL;
  }
  if (ret == NI_LOGAN_RETCODE_PARAM_ERROR_AREA_TOO_BIG)
  {
    av_log(avctx, AV_LOG_ERROR, "Invalid Picture Width x Height: exceeds %d\n",
           NI_LOGAN_MAX_RESOLUTION_AREA);
    return AVERROR_EXTERNAL;
  }
  if (ret < 0)
  {
    int i;
    av_log(avctx, AV_LOG_ERROR, "Error setting preset or log.\n");
    av_log(avctx, AV_LOG_INFO, "Possible presets:");
    for (i = 0; g_logan_xcoder_preset_names[i]; i++)
      av_log(avctx, AV_LOG_INFO, " %s", g_logan_xcoder_preset_names[i]);
    av_log(avctx, AV_LOG_INFO, "\n");

    av_log(avctx, AV_LOG_INFO, "Possible log:");
    for (i = 0; g_logan_xcoder_log_names[i]; i++)
      av_log(avctx, AV_LOG_INFO, " %s", g_logan_xcoder_log_names[i]);
    av_log(avctx, AV_LOG_INFO, "\n");

    return AVERROR(EINVAL);
  }

  av_log(avctx, AV_LOG_DEBUG, "pix_fmt is %d, sw_pix_fmt is %d\n",
         avctx->pix_fmt, avctx->sw_pix_fmt);
  if (avctx->pix_fmt != AV_PIX_FMT_NI_LOGAN)
  {
    av_log(avctx, AV_LOG_DEBUG, "sw_pix_fmt assigned to pix_fmt was %d, "
           "is now %d\n", avctx->pix_fmt, avctx->sw_pix_fmt);
    avctx->sw_pix_fmt = avctx->pix_fmt;
  }
  else
  {
    p_param->hwframes = 1;
    av_log(avctx, AV_LOG_DEBUG, "p_param->hwframes = %d\n", p_param->hwframes);
  }

  switch (avctx->sw_pix_fmt)
  {
    case AV_PIX_FMT_YUV420P:
    case AV_PIX_FMT_YUV420P10BE:
    case AV_PIX_FMT_YUV420P10LE:
    case AV_PIX_FMT_YUVJ420P:
      break;
    default:
      av_log(avctx, AV_LOG_ERROR, "Error: pixel format %d not supported.\n",
             avctx->sw_pix_fmt);
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
        int parse_ret = ni_logan_encoder_params_set_value(p_param, en->key, en->value, &s->api_ctx);
        switch (parse_ret)
        {
          case NI_LOGAN_RETCODE_PARAM_INVALID_NAME:
            av_log(avctx, AV_LOG_ERROR, "Unknown option: %s.\n", en->key);
            return AVERROR_EXTERNAL;
          case NI_LOGAN_RETCODE_PARAM_ERROR_TOO_BIG:
            av_log(avctx, AV_LOG_ERROR, "Invalid %s: too big\n", en->key);
            return AVERROR_EXTERNAL;
          case NI_LOGAN_RETCODE_PARAM_ERROR_TOO_SMALL:
            av_log(avctx, AV_LOG_ERROR, "Invalid %s: too small\n", en->key);
            return AVERROR_EXTERNAL;
          case NI_LOGAN_RETCODE_PARAM_ERROR_OOR:
            av_log(avctx, AV_LOG_ERROR, "Invalid %s: out of range\n", en->key);
            return AVERROR_EXTERNAL;
          case NI_LOGAN_RETCODE_PARAM_ERROR_ZERO:
            av_log(avctx, AV_LOG_ERROR, "Error setting option %s to value 0\n", en->key);
            return AVERROR_EXTERNAL;
          case NI_LOGAN_RETCODE_PARAM_INVALID_VALUE:
            av_log(avctx, AV_LOG_ERROR, "Invalid value for %s: %s.\n", en->key, en->value);
            return AVERROR_EXTERNAL;
          case NI_LOGAN_RETCODE_PARAM_GOP_INTRA_INCOMPATIBLE:
            av_log(avctx, AV_LOG_ERROR, "Invalid value for %s: %s incompatible with GOP structure.\n", en->key, en->value);
            return AVERROR_EXTERNAL;
          case NI_LOGAN_RETCODE_FAILURE:
            av_log(avctx, AV_LOG_ERROR, "Generic failure during xcoder-params setting for %s\n", en->key);
            return AVERROR_EXTERNAL;
          default:
            break;
        }
      }
      av_dict_free(&dict);

      if (ni_logan_encoder_params_check(p_param, s->api_ctx.codec_format) !=
          NI_LOGAN_RETCODE_SUCCESS)
      {
        av_log(avctx, AV_LOG_ERROR, "Validate encode parameters failed\n");
        return AVERROR_EXTERNAL;
      }
    }
  }

  if (p_param->enable_vfr)
  {
    //in the vfr mode, Customer WangSu may reset time base to a very large value, such as 1000.
    //At this time, the calculated framerate depends on timebase and ticket_per_frame is incorrect.
    //So we choose to set the default framerate 30.
    //If the calucluated framerate is correct, we will keep the original calculated framerate value
    //Assume the frame between 5-120 is correct.
    //using the time_base to initial timing info
    if (p_param->hevc_enc_params.frame_rate < 5 || p_param->hevc_enc_params.frame_rate > 120)
    {
      p_param->hevc_enc_params.frame_rate = 30;
    }
    s->api_ctx.ui32timing_scale = avctx->time_base.den;
    s->api_ctx.ui32num_unit_in_tick = avctx->time_base.num;
    s->api_ctx.prev_bitrate = p_param->bitrate;
    s->api_ctx.init_bitrate = p_param->bitrate;
    s->api_ctx.last_change_framenum = 0;
    s->api_ctx.fps_change_detect_count = 0;
  }

  if (s->xcoder_gop)
  {
    AVDictionary *dict = NULL;
    AVDictionaryEntry *en = NULL;

    if (!av_dict_parse_string(&dict, s->xcoder_gop, "=", ":", 0))
    {
      while ((en = av_dict_get(dict, "", en, AV_DICT_IGNORE_SUFFIX)))
      {
        int parse_ret = ni_logan_encoder_gop_params_set_value(p_param, en->key, en->value);
        switch (parse_ret)
        {
          case NI_LOGAN_RETCODE_PARAM_INVALID_NAME:
            av_log(avctx, AV_LOG_ERROR, "Unknown option: %s.\n", en->key);
            return AVERROR_EXTERNAL;
          case NI_LOGAN_RETCODE_PARAM_ERROR_TOO_BIG:
            av_log(avctx, AV_LOG_ERROR, "Invalid custom GOP parameters: %s too big\n", en->key);
            return AVERROR_EXTERNAL;
          case NI_LOGAN_RETCODE_PARAM_ERROR_TOO_SMALL:
            av_log(avctx, AV_LOG_ERROR, "Invalid custom GOP parameters: %s too small\n", en->key);
            return AVERROR_EXTERNAL;
          case NI_LOGAN_RETCODE_PARAM_ERROR_OOR:
            av_log(avctx, AV_LOG_ERROR, "Invalid custom GOP parameters: %s out of range \n", en->key);
            return AVERROR_EXTERNAL;
          case NI_LOGAN_RETCODE_PARAM_ERROR_ZERO:
             av_log(avctx, AV_LOG_ERROR, "Invalid custom GOP paramaters: Error setting option %s to value 0\n", en->key);
             return AVERROR_EXTERNAL;
          case NI_LOGAN_RETCODE_PARAM_INVALID_VALUE:
            av_log(avctx, AV_LOG_ERROR, "Invalid value for GOP param %s: %s.\n", en->key, en->value);
            return AVERROR_EXTERNAL;
          case NI_LOGAN_RETCODE_FAILURE:
            av_log(avctx, AV_LOG_ERROR, "Generic failure during xcoder-params setting for %s\n", en->key);
            return AVERROR_EXTERNAL;
          default:
            break;
        }
      }
      av_dict_free(&dict);
    }
  }

  s->api_ctx.p_session_config = &s->api_param;
  pparams = &s->api_param;
  switch (pparams->hevc_enc_params.gop_preset_index)
  {
    /* dts_offset is the max number of non-reference frames in a GOP
     * (derived from x264/5 algo) In case of IBBBP the first dts of the I frame should be input_pts-(3*ticks_per_frame)
     * In case of IBP the first dts of the I frame should be input_pts-(1*ticks_per_frame)
     * thus we ensure pts>dts in all cases
     * */
    case 1 /*PRESET_IDX_ALL_I*/:
    case 2 /*PRESET_IDX_IPP*/:
    case 6 /*PRESET_IDX_IPPPP*/:
    case 9 /*PRESET_IDX_SP*/:
      s->dts_offset = 0;
      break;
    /* ts requires dts/pts of I frame not same when there are B frames in streams */
    case 3 /*PRESET_IDX_IBBB*/:
    case 4 /*PRESET_IDX_IBPBP*/:
    case 7 /*PRESET_IDX_IBBBB*/:
      s->dts_offset = 1;
      break;
    case 5 /*PRESET_IDX_IBBBP*/:
      s->dts_offset = 2;
      break;
    case 8 /*PRESET_IDX_RA_IB*/:
      s->dts_offset = 3;
      break;
    default:
      // TBD need user to specify offset
      s->dts_offset = 7;
      av_log(avctx, AV_LOG_VERBOSE, "dts offset default to 7, TBD\n");
      break;
  }
  if (1 == pparams->force_frame_type)
  {
    s->dts_offset = 7;
  }

  av_log(avctx, AV_LOG_INFO, "dts offset: %d\n", s->dts_offset);

  if (0 == strcmp(s->dev_xcoder, LIST_DEVICES_STR))
  {
    av_log(avctx, AV_LOG_DEBUG, "XCoder: printing out all xcoder devices and "
           "their load, and exit ...\n");
    ni_logan_rsrc_print_all_devices_capability();
    return AVERROR_EXIT;
  }

  //overwrite keep alive timeout value here with a custom value if it was provided
  // provided
  // if xcoder option is set then overwrite the (legacy) decoder option
  uint32_t xcoder_timeout = s->api_param.hevc_enc_params.keep_alive_timeout;
  if (xcoder_timeout != NI_LOGAN_DEFAULT_KEEP_ALIVE_TIMEOUT)
  {
    s->api_ctx.keep_alive_timeout = xcoder_timeout;
  }
  else
  {
    s->api_ctx.keep_alive_timeout = s->keep_alive_timeout;
  }
  av_log(avctx, AV_LOG_VERBOSE, "Custom NVMe Keep Alive Timeout set to = %d\n",
         s->api_ctx.keep_alive_timeout);
  //overwrite set_high_priority value here with a custom value if it was provided
  uint32_t xcoder_high_priority = s->api_param.hevc_enc_params.set_high_priority;
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
  avctx->bit_rate = pparams->bitrate;
  s->total_frames_received = 0;
  s->encoder_eof = 0;
  s->roi_side_data_size = s->nb_rois = 0;
  s->av_rois = NULL;
  s->avc_roi_map = NULL;
  s->hevc_sub_ctu_roi_buf = NULL;
  s->hevc_roi_map = NULL;
  s->api_ctx.src_bit_depth = 8;
  s->api_ctx.src_endian = NI_LOGAN_FRAME_LITTLE_ENDIAN;
  s->api_ctx.roi_len = 0;
  s->api_ctx.roi_avg_qp = 0;
  s->api_ctx.bit_depth_factor = 1;
  if (AV_PIX_FMT_YUV420P10BE == avctx->sw_pix_fmt ||
      AV_PIX_FMT_YUV420P10LE == avctx->sw_pix_fmt)
  {
    s->api_ctx.bit_depth_factor = 2;
    s->api_ctx.src_bit_depth = 10;
    if (AV_PIX_FMT_YUV420P10BE == avctx->sw_pix_fmt)
    {
      s->api_ctx.src_endian = NI_LOGAN_FRAME_BIG_ENDIAN;
    }
  }

  // DolbyVision, HRD and AUD settings
  if (AV_CODEC_ID_HEVC == avctx->codec_id)
  {
    if (5 == pparams->dolby_vision_profile)
    {
      pparams->hrd_enable = pparams->enable_aud = 1;
      pparams->hevc_enc_params.forced_header_enable = NI_LOGAN_ENC_REPEAT_HEADERS_ALL_KEY_FRAMES;
      pparams->hevc_enc_params.decoding_refresh_type = 2;
    }
    if (pparams->hrd_enable)
    {
      pparams->hevc_enc_params.rc.enable_rate_control = 1;
    }
  }

  // init HW AVFRAME pool
  s->freeHead = 0;
  s->freeTail = 0;
  for (i = 0; i < LOGAN_MAX_NUM_FRAMEPOOL_HWAVFRAME; i++)
  {
    s->sframe_pool[i] = av_frame_alloc();
    if (!s->sframe_pool[i])
    {
      return AVERROR(ENOMEM);
    }
    s->aFree_Avframes_list[i] = i;
    s->freeTail++;
  }
  s->aFree_Avframes_list[i] = -1;

  // init HDR SEI stuff
  s->api_ctx.sei_hdr_content_light_level_info_len =
  s->api_ctx.light_level_data_len =
  s->api_ctx.sei_hdr_mastering_display_color_vol_len =
  s->api_ctx.mdcv_max_min_lum_data_len = 0;
  s->api_ctx.p_master_display_meta_data = NULL;

  // init HRD SEI stuff (TBD: value after recovery ?)
  s->au_cpb_removal_delay_minus1 = 0;

  memset( &(s->api_fme), 0, sizeof(ni_logan_session_data_io_t) );
  memset( &(s->api_pkt), 0, sizeof(ni_logan_session_data_io_t) );

  if (avctx->width > 0 && avctx->height > 0)
  {
    ni_logan_frame_buffer_alloc(&(s->api_fme.data.frame),
                          ODD2EVEN(avctx->width),
                          ODD2EVEN(avctx->height),
                          0,
                          0,
                          s->api_ctx.bit_depth_factor,
                          (s->buffered_fme.format == AV_PIX_FMT_NI_LOGAN));
  }

  // generate encoded bitstream headers in advance if configured to do so
  if (pparams->generate_enc_hdrs)
  {
    ret = xcoder_logan_encoder_headers(avctx);
  }

  return ret;
}

av_cold int ff_xcoder_logan_encode_init(AVCodecContext *avctx)
{
  XCoderLoganEncContext *ctx = avctx->priv_data;
  int ret;

  ni_log_set_level(ff_to_ni_log_level(av_log_get_level()));

  av_log(avctx, AV_LOG_DEBUG, "%s\n", __FUNCTION__);

  if (ctx->dev_xcoder == NULL)
  {
    av_log(avctx, AV_LOG_ERROR, "Error: XCoder option dev_xcoder is null\n");
    return AVERROR_INVALIDDATA;
  }
  else
  {
    av_log(avctx, AV_LOG_VERBOSE, "XCoder options: dev_xcoder: %s dev_enc_idx "
           "%d\n", ctx->dev_xcoder, ctx->dev_enc_idx);
  }

  if (ctx->api_ctx.session_run_state == LOGAN_SESSION_RUN_STATE_SEQ_CHANGE_DRAINING)
  {
    ctx->dev_enc_idx = ctx->orig_dev_enc_idx;
  }
  else
  {
    ctx->orig_dev_enc_idx = ctx->dev_enc_idx;
  }

#ifdef NIENC_MULTI_THREAD
  if (sessionCounter == 0)
  {
    threadpool_init(&pool);
  }
  sessionCounter++;
#endif

  if ((ret = xcoder_logan_setup_encoder(avctx)) < 0)
  {
    ff_xcoder_logan_encode_close(avctx);
    return ret;
  }

#ifdef _WIN32
  // For windows opening the encoder when init will take less time.
  // If HW frame detected then open in xcoder_send_frame function.
  if (avctx->pix_fmt != AV_PIX_FMT_NI_LOGAN)
  {
    // NETINT_INTERNAL - currently only for internal testing
    ni_logan_encoder_params_t *p_param = &ctx->api_param;
    ret = do_open_encoder_device(avctx, ctx, p_param);
    if (ret < 0)
    {
      ff_xcoder_logan_encode_close(avctx);
      return ret;
    }
  }
#endif
  ctx->vpu_reset = 0;

  return 0;
}

static int is_logan_input_fifo_empty(XCoderLoganEncContext *ctx)
{
  return av_fifo_size(ctx->fme_fifo) < sizeof(AVFrame);
}

static int is_logan_input_fifo_full(XCoderLoganEncContext *ctx)
{
  return av_fifo_space(ctx->fme_fifo) < sizeof(AVFrame);
}

static int xcoder_logan_encode_reset(AVCodecContext *avctx)
{
  XCoderLoganEncContext *ctx = avctx->priv_data;
  av_log(avctx, AV_LOG_WARNING, "%s\n", __FUNCTION__);
  ctx->vpu_reset = 1;
  ff_xcoder_logan_encode_close(avctx);
  return ff_xcoder_logan_encode_init(avctx);
}

static int enqueue_logan_frame(AVCodecContext *avctx, const AVFrame *inframe)
{
  int ret;
  XCoderLoganEncContext *ctx = avctx->priv_data;

  // expand frame buffer fifo if not enough space
  if (is_logan_input_fifo_full(ctx))
  {
    ret = av_fifo_realloc2(ctx->fme_fifo, 2 * av_fifo_size(ctx->fme_fifo));
    if (ret < 0)
    {
      av_log(avctx, AV_LOG_ERROR, "Enc av_fifo_realloc2 NO MEMORY !!!\n");
      return ret;
    }
    if ((av_fifo_size(ctx->fme_fifo) / sizeof(AVFrame) % 100) == 0)
    {
      av_log(avctx, AV_LOG_INFO, "Enc fifo being extended to: %" PRIu64 "\n",
             av_fifo_size(ctx->fme_fifo) / sizeof(AVFrame));
    }
    av_assert0(0 == av_fifo_size(ctx->fme_fifo) % sizeof(AVFrame));
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

  av_log(avctx, AV_LOG_DEBUG, "fme queued pts:%" PRId64 ", fifo size: %" PRIu64 "\n",
         inframe->pts, av_fifo_size(ctx->fme_fifo) / sizeof(AVFrame));

  return ret;
}

#ifdef NIENC_MULTI_THREAD
static void* write_frame_thread(void* arg)
{
  write_thread_arg_struct_t *args = (write_thread_arg_struct_t *) arg;
  XCoderLoganEncContext *ctx = args->ctx;
  int ret;
  int sent;

  pthread_mutex_lock(&args->mutex);
  args->running = 1;
  av_log(ctx, AV_LOG_DEBUG, "%s: session_id %d, device_handle %d\n",
         __FUNCTION__, ctx->api_ctx.session_id, ctx->api_ctx.device_handle);

  av_log(ctx, AV_LOG_DEBUG, "%s: ctx %p\n", __FUNCTION__, ctx);

  sent = ni_logan_device_session_write(&ctx->api_ctx, &ctx->api_fme, NI_LOGAN_DEVICE_TYPE_ENCODER);

  av_log(ctx, AV_LOG_DEBUG, "%s: size %d sent to xcoder\n", __FUNCTION__, sent);

  if (NI_LOGAN_RETCODE_ERROR_RESOURCE_UNAVAILABLE == sent)
  {
    av_log(ctx, AV_LOG_DEBUG, "%s(): Sequence Change in progress, returning "
           "EAGAIN\n", __FUNCTION__);
    ret = AVERROR(EAGAIN);
  }
  else if (NI_LOGAN_RETCODE_ERROR_VPU_RECOVERY == sent)
  {
    sent = xcoder_logan_encode_reset(ctx);
  }

  if (sent < 0)
  {
    ret = AVERROR(EIO);
  }
  else
  {
    //pushing input pts in circular FIFO
    ctx->api_ctx.enc_pts_list[ctx->api_ctx.enc_pts_w_idx % NI_LOGAN_FIFO_SZ] = ctx->api_fme.data.frame.pts;
    ctx->api_ctx.enc_pts_w_idx++;
    ret = 0;
  }

  args->ret = ret;
  av_log(ctx, AV_LOG_DEBUG, "%s: ret %d\n", __FUNCTION__, ret);
  pthread_cond_signal(&args->cond);
  args->running = 0;
  pthread_mutex_unlock(&args->mutex);
  return NULL;
}
#endif

int ff_xcoder_logan_encode_close(AVCodecContext *avctx)
{
  XCoderLoganEncContext *ctx = avctx->priv_data;
  ni_logan_retcode_t ret = NI_LOGAN_RETCODE_FAILURE;
  int i;

#ifdef NIENC_MULTI_THREAD
  sessionCounter--;
  if (sessionCounter == 0)
  {
    threadpool_destroy(&pool);
  }
#endif

  for (i = 0; i < LOGAN_MAX_NUM_FRAMEPOOL_HWAVFRAME; i++)
  {
    //any remaining stored AVframes that have not been unref will die here
    av_frame_free(&(ctx->sframe_pool[i]));
    ctx->sframe_pool[i] = NULL;
  }

  do_close_encoder_device(ctx);

  if (ctx->api_ctx.p_master_display_meta_data)
  {
    free(ctx->api_ctx.p_master_display_meta_data);
    ctx->api_ctx.p_master_display_meta_data = NULL;
  }

  av_log(avctx, AV_LOG_DEBUG, "%s (status = %d)\n", __FUNCTION__, ret);
  ni_logan_frame_buffer_free( &(ctx->api_fme.data.frame) );
  ni_logan_packet_buffer_free( &(ctx->api_pkt.data.packet) );

  av_log(avctx, AV_LOG_DEBUG, "fifo size: %" PRIu64 "\n",
         av_fifo_size(ctx->fme_fifo) / sizeof(AVFrame));
  if (! ctx->vpu_reset &&
      ctx->api_ctx.session_run_state != LOGAN_SESSION_RUN_STATE_SEQ_CHANGE_DRAINING)
  {
    av_fifo_free(ctx->fme_fifo);
    av_log(avctx, AV_LOG_DEBUG, " , freed.\n");
  }
  else
  {
    av_log(avctx, AV_LOG_DEBUG, " , kept.\n");
  }

  ni_logan_device_session_context_clear(&ctx->api_ctx);
  ni_logan_rsrc_free_device_context(ctx->rsrc_ctx);
  ctx->rsrc_ctx = NULL;

  free(ctx->p_spsPpsHdr);
  ctx->p_spsPpsHdr = NULL;

  free(ctx->av_rois);
  free(ctx->avc_roi_map);
  free(ctx->hevc_sub_ctu_roi_buf);
  free(ctx->hevc_roi_map);
  ctx->av_rois = NULL;
  ctx->avc_roi_map = NULL;
  ctx->hevc_sub_ctu_roi_buf = NULL;
  ctx->hevc_roi_map = NULL;
  ctx->roi_side_data_size = ctx->nb_rois = 0;
  ctx->started = 0;

  return 0;
}

static int xcoder_send_frame(AVCodecContext *avctx, const AVFrame *frame)
{
  XCoderLoganEncContext *ctx = avctx->priv_data;
  int ret = 0;
  int sent;
  int orig_avctx_width = avctx->width, orig_avctx_height = avctx->height;
  AVFrameSideData *side_data;
  AVHWFramesContext *avhwf_ctx;
  NILOGANFramesContext *nif_src_ctx;
  int is_hwframe;
  int format_in_use;
  int frame_width, frame_height;

#ifdef NIENC_MULTI_THREAD
  av_log(avctx, AV_LOG_DEBUG, "%s start %p, session_id %d, device_handle %d\n",
         __FUNCTION__, ctx->api_ctx.session_info, ctx->api_ctx.session_id,
         ctx->api_ctx.device_handle);
  if (ctx->api_ctx.session_id != NI_LOGAN_INVALID_SESSION_ID &&
      ctx->api_ctx.device_handle != NI_INVALID_DEVICE_HANDLE)
  {
    av_log(avctx, AV_LOG_DEBUG, "%s start %p\n", __FUNCTION__,
           ctx->api_ctx.session_info);
    if (ctx->api_ctx.session_info != NULL)
    {
      write_thread_arg_struct_t *write_thread_args = (write_thread_arg_struct_t *)ctx->api_ctx.session_info;
      pthread_mutex_lock(&write_thread_args->mutex);
      av_log(avctx, AV_LOG_DEBUG, "thread start waiting session_id %d\n",
             ctx->api_ctx.session_id);
      if (write_thread_args->running == 1)
      {
        pthread_cond_wait(&write_thread_args->cond, &write_thread_args->mutex);
        av_log(avctx, AV_LOG_DEBUG, "thread get waiting session_id %d\n",
               ctx->api_ctx.session_id);
      }
      if (write_thread_args->ret == AVERROR(EAGAIN))
      {
        av_log(avctx, AV_LOG_ERROR, "%s: ret %d\n", __FUNCTION__,
               write_thread_args->ret);
        pthread_mutex_unlock(&write_thread_args->mutex);
        free(write_thread_args);
        ctx->api_ctx.session_info = NULL;
        return AVERROR(EAGAIN);
      }
      pthread_mutex_unlock(&write_thread_args->mutex);
      free(write_thread_args);
      ctx->api_ctx.session_info = NULL;
      av_log(avctx, AV_LOG_DEBUG, "thread free session_id %d\n",
             ctx->api_ctx.session_id);
    }
  }
#endif
  ni_logan_encoder_params_t *p_param = &ctx->api_param; // NETINT_INTERNAL - currently only for internal testing

  av_log(avctx, AV_LOG_DEBUG, "%s pkt_size %d %dx%d  avctx: %dx%d\n", __FUNCTION__,
         frame ? frame->pkt_size : -1, frame ? frame->width : -1,
         frame ? frame->height : -1, avctx->width, avctx->height);

  if (ctx->encoder_flushing)
  {
    if (! frame && is_logan_input_fifo_empty(ctx))
    {
      av_log(avctx, AV_LOG_DEBUG, "XCoder EOF: null frame && fifo empty\n");
      return AVERROR_EOF;
    }
  }

  if (! frame)
  {
    if (LOGAN_SESSION_RUN_STATE_QUEUED_FRAME_DRAINING == ctx->api_ctx.session_run_state)
    {
      av_log(avctx, AV_LOG_DEBUG, "null frame, send queued frame\n");
    }
    else
    {
      ctx->eos_fme_received = 1;
      av_log(avctx, AV_LOG_DEBUG, "null frame, ctx->eos_fme_received = 1\n");
    }
  }
  else
  {
    av_log(avctx, AV_LOG_DEBUG, "%s #%"PRIu64"\n", __FUNCTION__,
           ctx->api_ctx.frame_num);

    // queue up the frame if fifo is NOT empty, or sequence change ongoing !
    if (!is_logan_input_fifo_empty(ctx) ||
        LOGAN_SESSION_RUN_STATE_SEQ_CHANGE_DRAINING == ctx->api_ctx.session_run_state)
    {
      enqueue_logan_frame(avctx, frame);

      if (LOGAN_SESSION_RUN_STATE_SEQ_CHANGE_DRAINING ==
          ctx->api_ctx.session_run_state)
      {
        av_log(avctx, AV_LOG_TRACE, "XCoder doing sequence change, frame #"
               "%"PRIu64" queued and return 0 !\n", ctx->api_ctx.frame_num);
        return 0;
      }
    }
    else if (frame != &ctx->buffered_fme)
    {
      // For FFmpeg-n4.4+ receive_packet interface the buffered_fme is fetched from
      // ff_alloc_get_frame rather than passed as function argument. So we need to
      // judge whether they are the same object. If they are the same NO need to do
      // any reference.
      ret = av_frame_ref(&ctx->buffered_fme, frame);
    }
  }

  if (is_logan_input_fifo_empty(ctx))
  {
    av_log(avctx, AV_LOG_DEBUG, "no frame in fifo to send, just send/receive ..\n");
    if (ctx->eos_fme_received)
    {
      av_log(avctx, AV_LOG_DEBUG, "no frame in fifo to send, send eos ..\n");
      // if received eos but not sent any frame, there is no need to continue the following process
      if (ctx->started == 0)
      {
        av_log(avctx, AV_LOG_DEBUG, "session is not open, send eos, return EOF\n");
        return AVERROR_EOF;
      }
    }
  }
  else
  {
    av_fifo_generic_peek(ctx->fme_fifo, &ctx->buffered_fme,
                         sizeof(AVFrame), NULL);
    ctx->buffered_fme.extended_data = ctx->buffered_fme.data;
  }

  frame_width = ODD2EVEN(ctx->buffered_fme.width);
  frame_height = ODD2EVEN(ctx->buffered_fme.height);
  is_hwframe = (ctx->buffered_fme.format == AV_PIX_FMT_NI_LOGAN);

  // leave encoder instance open to when the first frame buffer arrives so that
  // its stride size is known and handled accordingly.
  if (ctx->started == 0)
  {
#ifdef _WIN32
    if (ctx->buffered_fme.width != avctx->width ||
       ctx->buffered_fme.height != avctx->height ||
       ctx->buffered_fme.color_primaries != avctx->color_primaries ||
       ctx->buffered_fme.color_trc != avctx->color_trc ||
       ctx->buffered_fme.colorspace != avctx->colorspace)
    {
      av_log(avctx, AV_LOG_INFO, "WARNING reopen device Width: %d-%d, "
             "Height: %d-%d, color_primaries: %d-%d, color_trc: %d-%d, "
             "color_space: %d-%d\n",
             ctx->buffered_fme.width, avctx->width,
             ctx->buffered_fme.height, avctx->height,
             ctx->buffered_fme.color_primaries, avctx->color_primaries,
             ctx->buffered_fme.color_trc, avctx->color_trc,
             ctx->buffered_fme.colorspace, avctx->colorspace);
      do_close_encoder_device(ctx);
      // Errror when set this parameters in ni_logan_encoder_params_set_value !!!!!!
      p_param->hevc_enc_params.conf_win_right = 0;
      p_param->hevc_enc_params.conf_win_bottom = 0;

      if ((ret = do_open_encoder_device(avctx, ctx, p_param)) < 0)
      {
        return ret;
      }
    }
    else if (is_hwframe) // if hw-frame detected for windows then open here.
#endif
    {
      if ((ret = do_open_encoder_device(avctx, ctx, p_param)) < 0)
      {
        return ret;
      }
    }
    ctx->api_fme.data.frame.start_of_stream = 1;
    ctx->started = 1;
  }
  else
  {
    ctx->api_fme.data.frame.start_of_stream = 0;
  }

  if ((ctx->buffered_fme.height && ctx->buffered_fme.width) &&
      (ctx->buffered_fme.height != avctx->height ||
       ctx->buffered_fme.width != avctx->width))
  {
    av_log(avctx, AV_LOG_INFO, "%s resolution change %dx%d -> %dx%d\n",
           __FUNCTION__, avctx->width, avctx->height, ctx->buffered_fme.width,
           ctx->buffered_fme.height);
    ctx->api_ctx.session_run_state = LOGAN_SESSION_RUN_STATE_SEQ_CHANGE_DRAINING;
    ctx->eos_fme_received = 1;

    // have to queue this frame if not done so: an empty queue
    if (is_logan_input_fifo_empty(ctx))
    {
      av_log(avctx, AV_LOG_TRACE, "%s resolution change when fifo empty, frame "
             "#%"PRIu64" being queued\n", __FUNCTION__, ctx->api_ctx.frame_num);
      if (frame != &ctx->buffered_fme)
      {
        // For FFmpeg-n4.4+ receive_packet interface the buffered_fme is fetched from
        // ff_alloc_get_frame rather than passed as function argument. So we need to
        // judge whether they are the same object. If they are the same do NOT
        // unreference any of them because we need to enqueue it later.
        av_frame_unref(&ctx->buffered_fme);
      }
      enqueue_logan_frame(avctx, frame);
    }
  }

  ctx->api_fme.data.frame.preferred_characteristics_data_len = 0;
  ctx->api_fme.data.frame.end_of_stream = 0;
  ctx->api_fme.data.frame.force_key_frame
  = ctx->api_fme.data.frame.use_cur_src_as_long_term_pic
  = ctx->api_fme.data.frame.use_long_term_ref = 0;

  ctx->api_fme.data.frame.sei_total_len
  = ctx->api_fme.data.frame.sei_cc_offset = ctx->api_fme.data.frame.sei_cc_len
  = ctx->api_fme.data.frame.sei_hdr_mastering_display_color_vol_offset
  = ctx->api_fme.data.frame.sei_hdr_mastering_display_color_vol_len
  = ctx->api_fme.data.frame.sei_hdr_content_light_level_info_offset
  = ctx->api_fme.data.frame.sei_hdr_content_light_level_info_len
  = ctx->api_fme.data.frame.sei_hdr_plus_offset
  = ctx->api_fme.data.frame.sei_hdr_plus_len = 0;

  ctx->api_fme.data.frame.roi_len = 0;
  ctx->api_fme.data.frame.reconf_len = 0;
  ctx->api_fme.data.frame.force_pic_qp = 0;

  // employ a ni_logan_frame_t to represent decode frame when using new libxcoder
  // API to prepare side data
  ni_logan_frame_t dec_frame = {0};
  ni_aux_data_t *aux_data = NULL;

  if (LOGAN_SESSION_RUN_STATE_SEQ_CHANGE_DRAINING == ctx->api_ctx.session_run_state ||
      (ctx->eos_fme_received && is_logan_input_fifo_empty(ctx)))
  {
    av_log(avctx, AV_LOG_DEBUG, "XCoder start flushing\n");
    ctx->api_fme.data.frame.end_of_stream = 1;
    ctx->encoder_flushing = 1;
  }
  else
  {
    // NETINT_INTERNAL - currently only for internal testing
    // allocate memory for reconf parameters only once and reuse it
    if (! ctx->api_ctx.enc_change_params &&
        p_param->reconf_demo_mode > LOGAN_XCODER_TEST_RECONF_OFF &&
        p_param->reconf_demo_mode <= LOGAN_XCODER_TEST_RECONF_RC_MIN_MAX_QP)
    {
      ctx->api_ctx.enc_change_params =
      calloc(1, sizeof(ni_logan_encoder_change_params_t));
      if (! ctx->api_ctx.enc_change_params)
      {
        return AVERROR(ENOMEM);
      }
    }
    if (ctx->api_ctx.enc_change_params)
    {
      memset(ctx->api_ctx.enc_change_params,
             0, sizeof(ni_logan_encoder_change_params_t));
    }

    ctx->api_fme.data.frame.extra_data_len = NI_LOGAN_APP_ENC_FRAME_META_DATA_SIZE;

    switch (p_param->reconf_demo_mode)
    {
      case LOGAN_XCODER_TEST_RECONF_BR:
        if (ctx->api_ctx.frame_num == p_param->reconf_hash[ctx->reconfigCount][0])
        {
          ctx->api_ctx.enc_change_params->enable_option |=
          NI_LOGAN_SET_CHANGE_PARAM_RC_TARGET_RATE;
          ctx->api_ctx.enc_change_params->bitRate =
          p_param->reconf_hash[ctx->reconfigCount][1];

          ctx->api_fme.data.frame.extra_data_len +=
          sizeof(ni_logan_encoder_change_params_t);
          ctx->api_fme.data.frame.reconf_len =
          sizeof(ni_logan_encoder_change_params_t);

          ctx->reconfigCount++;
        }
        break;
      case LOGAN_XCODER_TEST_RECONF_INTRAPRD:
        if (ctx->api_ctx.frame_num == p_param->reconf_hash[ctx->reconfigCount][0])
        {
          ctx->api_ctx.enc_change_params->enable_option |=
          NI_LOGAN_SET_CHANGE_PARAM_INTRA_PARAM;
          ctx->api_ctx.enc_change_params->intraQP =
          p_param->reconf_hash[ctx->reconfigCount][1];
          ctx->api_ctx.enc_change_params->intraPeriod =
          p_param->reconf_hash[ctx->reconfigCount][2];
          ctx->api_ctx.enc_change_params->repeatHeaders =
          p_param->reconf_hash[ctx->reconfigCount][3];
          av_log(avctx, AV_LOG_TRACE, "xcoder_send_frame: frame #%lu reconf "
                 "intraQP %d intraPeriod %d repeatHeaders %d\n",
                 ctx->api_ctx.frame_num,
                 ctx->api_ctx.enc_change_params->intraQP,
                 ctx->api_ctx.enc_change_params->intraPeriod,
                 ctx->api_ctx.enc_change_params->repeatHeaders);

          ctx->api_fme.data.frame.extra_data_len +=
          sizeof(ni_logan_encoder_change_params_t);
          ctx->api_fme.data.frame.reconf_len =
          sizeof(ni_logan_encoder_change_params_t);

          ctx->reconfigCount++;
        }
        break;
    case LOGAN_XCODER_TEST_RECONF_LONG_TERM_REF:
      // the reconf file data line format for this is:
      // <frame-number>:useCurSrcAsLongtermPic,useLongtermRef where
      // values will stay the same on every frame until changed.
      if (ctx->api_ctx.frame_num >= p_param->reconf_hash[ctx->reconfigCount][0])
      {
        AVFrameSideData *ltr_sd;
        AVNetintLongTermRef *p_ltr;
        ltr_sd = av_frame_new_side_data(&ctx->buffered_fme,
                                        AV_FRAME_DATA_NETINT_LONG_TERM_REF,
                                        sizeof(AVNetintLongTermRef));
        if (ltr_sd)
        {
          p_ltr = (AVNetintLongTermRef *)ltr_sd->data;
          p_ltr->use_cur_src_as_long_term_pic
          = p_param->reconf_hash[ctx->reconfigCount][1];
          p_ltr->use_long_term_ref
          = p_param->reconf_hash[ctx->reconfigCount][2];
        }
      }
      if (ctx->api_ctx.frame_num + 1 ==
          p_param->reconf_hash[ctx->reconfigCount + 1][0])
      {
        ctx->reconfigCount++;
      }
      break;
    case LOGAN_XCODER_TEST_RECONF_VUI_HRD:
      // the reconf file format for this is:
      // <frame-number>:<vui-file-name-in-digits>,<number-of-bits-of-vui-rbsp>
      if (ctx->api_ctx.frame_num == p_param->reconf_hash[ctx->reconfigCount][0])
      {
        char file_name[64];
        FILE *vui_file;
        snprintf(file_name, 64, "%d",
                 p_param->reconf_hash[ctx->reconfigCount][1]);
        vui_file = fopen(file_name, "rb");
        if (! vui_file)
        {
          av_log(avctx, AV_LOG_ERROR, "Error VUI reconf file: %s\n", file_name);
        }
        else
        {
          int nb_bytes_by_bits =
          (p_param->reconf_hash[ctx->reconfigCount][2] + 7) / 8;
          size_t nb_bytes = fread(ctx->api_ctx.enc_change_params->vuiRbsp,
                                  1, NI_LOGAN_MAX_VUI_SIZE, vui_file);
          if (nb_bytes != nb_bytes_by_bits)
          {
            av_log(avctx, AV_LOG_ERROR, "Error VUI file size %d bytes != "
                   "specified %d bits (%d bytes) !\n", (int)nb_bytes,
                   p_param->reconf_hash[ctx->reconfigCount][2], nb_bytes_by_bits);
          }
          else
          {
            ctx->api_ctx.enc_change_params->enable_option |=
            NI_LOGAN_SET_CHANGE_PARAM_VUI_HRD_PARAM;
            ctx->api_ctx.enc_change_params->encodeVuiRbsp = 1;
            ctx->api_ctx.enc_change_params->vuiDataSizeBits =
            p_param->reconf_hash[ctx->reconfigCount][2];
            ctx->api_ctx.enc_change_params->vuiDataSizeBytes = nb_bytes;
            av_log(avctx, AV_LOG_DEBUG, "Reconf VUI %d bytes (%d bits)\n",
                   (int)nb_bytes, p_param->reconf_hash[ctx->reconfigCount][2]);
            ctx->api_fme.data.frame.extra_data_len +=
            sizeof(ni_logan_encoder_change_params_t);
            ctx->api_fme.data.frame.reconf_len =
            sizeof(ni_logan_encoder_change_params_t);

            ctx->reconfigCount++;
          }

          fclose(vui_file);
        }
      }
      break;
    case LOGAN_XCODER_TEST_RECONF_RC:
      if (ctx->api_ctx.frame_num == p_param->reconf_hash[ctx->reconfigCount][0])
      {
        ctx->api_ctx.enc_change_params->enable_option |= NI_LOGAN_SET_CHANGE_PARAM_RC;
        ctx->api_ctx.enc_change_params->hvsQPEnable =
        p_param->reconf_hash[ctx->reconfigCount][1];
        ctx->api_ctx.enc_change_params->hvsQpScale =
        p_param->reconf_hash[ctx->reconfigCount][2];
        ctx->api_ctx.enc_change_params->vbvBufferSize =
        p_param->reconf_hash[ctx->reconfigCount][3];
        ctx->api_ctx.enc_change_params->mbLevelRcEnable =
        p_param->reconf_hash[ctx->reconfigCount][4];
        ctx->api_ctx.enc_change_params->fillerEnable =
        p_param->reconf_hash[ctx->reconfigCount][5];
        av_log(avctx, AV_LOG_TRACE, "xcoder_send_frame: frame #%lu reconf "
               "hvsQPEnable %d hvsQpScale %d vbvBufferSize %d mbLevelRcEnable "
               "%d fillerEnable %d\n",
               ctx->api_ctx.frame_num,
               ctx->api_ctx.enc_change_params->hvsQPEnable,
               ctx->api_ctx.enc_change_params->hvsQpScale,
               ctx->api_ctx.enc_change_params->vbvBufferSize,
               ctx->api_ctx.enc_change_params->mbLevelRcEnable,
               ctx->api_ctx.enc_change_params->fillerEnable);

        ctx->api_fme.data.frame.extra_data_len +=
        sizeof(ni_logan_encoder_change_params_t);
        ctx->api_fme.data.frame.reconf_len = sizeof(ni_logan_encoder_change_params_t);
        ctx->reconfigCount++;
      }
      break;
    case LOGAN_XCODER_TEST_RECONF_RC_MIN_MAX_QP:
      if (ctx->api_ctx.frame_num == p_param->reconf_hash[ctx->reconfigCount][0])
      {
        ctx->api_ctx.enc_change_params->enable_option |=
        NI_LOGAN_SET_CHANGE_PARAM_RC_MIN_MAX_QP;
        ctx->api_ctx.enc_change_params->minQpI =
        p_param->reconf_hash[ctx->reconfigCount][1];
        ctx->api_ctx.enc_change_params->maxQpI =
        p_param->reconf_hash[ctx->reconfigCount][2];
        ctx->api_ctx.enc_change_params->maxDeltaQp =
        p_param->reconf_hash[ctx->reconfigCount][3];
        ctx->api_ctx.enc_change_params->minQpP =
        p_param->reconf_hash[ctx->reconfigCount][4];
        ctx->api_ctx.enc_change_params->minQpB =
        p_param->reconf_hash[ctx->reconfigCount][5];
        ctx->api_ctx.enc_change_params->maxQpP =
        p_param->reconf_hash[ctx->reconfigCount][6];
        ctx->api_ctx.enc_change_params->maxQpB =
        p_param->reconf_hash[ctx->reconfigCount][7];
        av_log(avctx, AV_LOG_TRACE, "xcoder_send_frame: frame #%lu reconf "
               "minQpI %d maxQpI %d maxDeltaQp %d minQpP "
               "%d minQpB %d maxQpP %d maxQpB %d\n",
               ctx->api_ctx.frame_num, ctx->api_ctx.enc_change_params->minQpI,
               ctx->api_ctx.enc_change_params->maxQpI,
               ctx->api_ctx.enc_change_params->maxDeltaQp,
               ctx->api_ctx.enc_change_params->minQpP,
               ctx->api_ctx.enc_change_params->minQpB,
               ctx->api_ctx.enc_change_params->maxQpP,
               ctx->api_ctx.enc_change_params->maxQpB);

        ctx->api_fme.data.frame.extra_data_len +=
        sizeof(ni_logan_encoder_change_params_t);
        ctx->api_fme.data.frame.reconf_len = sizeof(ni_logan_encoder_change_params_t);
        ctx->reconfigCount++;
      }
      break;
      case LOGAN_XCODER_TEST_RECONF_OFF:
      default:
        ;
    }

    // long term reference frame support
    side_data = av_frame_get_side_data(&ctx->buffered_fme,
                                       AV_FRAME_DATA_NETINT_LONG_TERM_REF);
    if (side_data && (side_data->size == sizeof(AVNetintLongTermRef)))
    {
      aux_data = ni_logan_frame_new_aux_data(&dec_frame, NI_FRAME_AUX_DATA_LONG_TERM_REF,
                                       sizeof(ni_long_term_ref_t));
      if (aux_data)
      {
        memcpy(aux_data->data, side_data->data, side_data->size);
      }

      if (ctx->api_fme.data.frame.reconf_len == 0)
      {
        ctx->reconfigCount++;
      }
    }

    // NetInt target bitrate reconfiguration support
    side_data = av_frame_get_side_data(&ctx->buffered_fme,
                                       AV_FRAME_DATA_NETINT_BITRATE);
    if (side_data && (side_data->size == sizeof(int32_t)))
    {
      if (ctx->api_param.enable_vfr)
      {
        //ctx->api_params.hevc_enc_params.frame_rate is the default framerate when vfr enabled
        int32_t bitrate = *((int32_t *)side_data->data);
        ctx->api_ctx.init_bitrate = bitrate;
        bitrate = bitrate * ctx->api_param.hevc_enc_params.frame_rate / ctx->api_ctx.prev_fps;
        *(int32_t *)side_data->data = bitrate;
        ctx->api_ctx.prev_bitrate = bitrate;
      }

      aux_data = ni_logan_frame_new_aux_data(&dec_frame, NI_FRAME_AUX_DATA_BITRATE,
                                       sizeof(int32_t));
      if (aux_data)
      {
        memcpy(aux_data->data, side_data->data, side_data->size);
      }

      if (ctx->api_fme.data.frame.reconf_len == 0)
      {
        ctx->reconfigCount++;
      }
    }

    // NetInt support VFR by reconfig bitrate and vui
    if (ctx->api_param.enable_vfr)
    {
      int32_t cur_fps = 0, bit_rate = 0;

      if (ctx->buffered_fme.pts > ctx->api_ctx.prev_pts)
      {
        ctx->api_ctx.passed_time_in_timebase_unit += ctx->buffered_fme.pts - ctx->api_ctx.prev_pts;
        ctx->api_ctx.count_frame_num_in_sec++;
        //change the bitrate for VFR
        //1. Only when the fps change, setting the new bitrate
        //2. The interval between two bitrate change settings shall be greater than 1 seconds(hardware limiation)
        //   or at the start the transcoding, we should detect the init frame rate(30) and the actual framerate
        if (ctx->api_ctx.passed_time_in_timebase_unit >= (avctx->time_base.den / avctx->time_base.num))
        {
          cur_fps = ctx->api_ctx.count_frame_num_in_sec;
          bit_rate = (int)(ctx->api_param.hevc_enc_params.frame_rate * (ctx->api_ctx.init_bitrate / cur_fps));
          if ((ctx->api_ctx.frame_num != 0) && (bit_rate != ctx->api_ctx.prev_bitrate) &&
              ((ctx->api_ctx.frame_num < ctx->api_param.hevc_enc_params.frame_rate) || 
               ((uint32_t)(ctx->api_ctx.frame_num - ctx->api_ctx.last_change_framenum) >= ctx->api_param.hevc_enc_params.frame_rate)))
          {
            //adjust the upper and lower limits of bitrate each time
            bit_rate = av_clip(bit_rate, ctx->api_ctx.prev_bitrate / 2, ctx->api_ctx.prev_bitrate * 3 / 2);

            aux_data = ni_logan_frame_new_aux_data(&dec_frame, NI_FRAME_AUX_DATA_BITRATE,
                                         sizeof(int32_t));
            if (aux_data)
            {
              memcpy(aux_data->data, &bit_rate, sizeof(int32_t));
            }

            ctx->api_ctx.prev_bitrate = bit_rate;
            ctx->api_ctx.last_change_framenum = ctx->api_ctx.frame_num;
            ctx->api_ctx.prev_fps     = cur_fps;
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
        p_param->hevc_enc_params.rc.intra_qp;
      }
      else if (ctx->api_ctx.frame_num >= 200)
      {
        ctx->api_fme.data.frame.force_pic_qp = p_param->force_pic_qp_demo_mode;
      }
    }
    // END NETINT_INTERNAL - currently only for internal testing

    // SEI (HDR)
    // content light level info
    AVFrameSideData *hdr_side_data;

    hdr_side_data = av_frame_get_side_data(
      &ctx->buffered_fme, AV_FRAME_DATA_CONTENT_LIGHT_LEVEL);
    if (hdr_side_data && hdr_side_data->size == sizeof(AVContentLightMetadata))
    {
      aux_data = ni_logan_frame_new_aux_data(&dec_frame,
                                       NI_FRAME_AUX_DATA_CONTENT_LIGHT_LEVEL,
                                       sizeof(ni_content_light_level_t));
      if (aux_data)
      {
        memcpy(aux_data->data, hdr_side_data->data, hdr_side_data->size);
      }
    }

    // mastering display color volume
    hdr_side_data = av_frame_get_side_data(
      &ctx->buffered_fme, AV_FRAME_DATA_MASTERING_DISPLAY_METADATA);
    if (hdr_side_data &&
        hdr_side_data->size == sizeof(AVMasteringDisplayMetadata))
    {
      aux_data = ni_logan_frame_new_aux_data(
        &dec_frame, NI_FRAME_AUX_DATA_MASTERING_DISPLAY_METADATA,
        sizeof(ni_mastering_display_metadata_t));
      if (aux_data)
      {
        memcpy(aux_data->data, hdr_side_data->data, hdr_side_data->size);
      }
    }

    // SEI (HDR10+)
    AVFrameSideData *s_data = av_frame_get_side_data(
      &ctx->buffered_fme, AV_FRAME_DATA_DYNAMIC_HDR_PLUS);
    if (s_data && s_data->size == sizeof(AVDynamicHDRPlus))
    {
      aux_data = ni_logan_frame_new_aux_data(&dec_frame, NI_FRAME_AUX_DATA_HDR_PLUS,
                                       sizeof(ni_dynamic_hdr_plus_t));
      if (aux_data)
      {
        memcpy(aux_data->data, s_data->data, s_data->size);
      }
    } // hdr10+

    // SEI (close caption)
    side_data = av_frame_get_side_data(&ctx->buffered_fme,AV_FRAME_DATA_A53_CC);
    if (side_data && side_data->size > 0)
    {
      aux_data = ni_logan_frame_new_aux_data(&dec_frame, NI_FRAME_AUX_DATA_A53_CC,
                                       side_data->size);
      if (aux_data)
      {
        memcpy(aux_data->data, side_data->data, side_data->size);
      }
    }

    // supply QP map if ROI enabled and if ROIs passed in
    const AVFrameSideData *p_sd = av_frame_get_side_data(
      &ctx->buffered_fme, AV_FRAME_DATA_REGIONS_OF_INTEREST);
    if (p_param->hevc_enc_params.roi_enable && p_sd)
    {
      aux_data = ni_logan_frame_new_aux_data(&dec_frame,
                                       NI_FRAME_AUX_DATA_REGIONS_OF_INTEREST,
                                       p_sd->size);
      if (aux_data)
      {
        memcpy(aux_data->data, p_sd->data, p_sd->size);
      }
    }

    // User data unregistered SEI
    side_data = av_frame_get_side_data(&ctx->buffered_fme,
                                       AV_FRAME_DATA_NETINT_UDU_SEI);
    if (side_data && side_data->size > 0)
    {
      uint8_t *sei_data = (uint8_t *)side_data->data;
      aux_data = ni_logan_frame_new_aux_data(&dec_frame, NI_FRAME_AUX_DATA_UDU_SEI,
                                       side_data->size);
      if (aux_data)
      {
        memcpy(aux_data->data, (uint8_t *)side_data->data, side_data->size);
      }
    }

    ctx->api_fme.data.frame.pts = ctx->buffered_fme.pts;
    ctx->api_fme.data.frame.dts = ctx->buffered_fme.pkt_dts;
    ctx->api_fme.data.frame.video_width = ODD2EVEN(avctx->width);
    ctx->api_fme.data.frame.video_height = ODD2EVEN(avctx->height);

    ctx->api_fme.data.frame.ni_logan_pict_type = 0;

    if (ctx->api_ctx.force_frame_type)
    {
      switch (ctx->buffered_fme.pict_type)
      {
        case AV_PICTURE_TYPE_I:
          ctx->api_fme.data.frame.ni_logan_pict_type = LOGAN_PIC_TYPE_FORCE_IDR;
          break;
        case AV_PICTURE_TYPE_P:
          ctx->api_fme.data.frame.ni_logan_pict_type = LOGAN_PIC_TYPE_P;
          break;
        default:
          ;
      }
    }
    else if (AV_PICTURE_TYPE_I == ctx->buffered_fme.pict_type)
    {
      ctx->api_fme.data.frame.force_key_frame = 1;
      ctx->api_fme.data.frame.ni_logan_pict_type = LOGAN_PIC_TYPE_FORCE_IDR;
    }

    // whether should send SEI with this frame
    int send_sei_with_idr = ni_logan_should_send_sei_with_frame(
      &ctx->api_ctx, ctx->api_fme.data.frame.ni_logan_pict_type, p_param);

    av_log(avctx, AV_LOG_TRACE, "%s: #%"PRIu64" ni_logan_pict_type %d "
           "forced_header_enable %d intraPeriod %d send_sei_with_idr: %s\n",
           __FUNCTION__,
           ctx->api_ctx.frame_num, ctx->api_fme.data.frame.ni_logan_pict_type,
           p_param->hevc_enc_params.forced_header_enable,
           p_param->hevc_enc_params.intra_period,
           send_sei_with_idr ? "Yes" : "No");

    // data buffer for various SEI: HDR mastering display color volume, HDR
    // content light level, close caption, User data unregistered, HDR10+ etc.
    uint8_t mdcv_data[NI_LOGAN_MAX_SEI_DATA];
    uint8_t cll_data[NI_LOGAN_MAX_SEI_DATA];
    uint8_t cc_data[NI_LOGAN_MAX_SEI_DATA];
    uint8_t udu_data[NI_LOGAN_MAX_SEI_DATA];
    uint8_t hdrp_data[NI_LOGAN_MAX_SEI_DATA];

    // prep for auxiliary data (various SEI, ROI) in encode frame, based on the
    // data returned in decoded frame
    ni_logan_enc_prep_aux_data(&ctx->api_ctx, &ctx->api_fme.data.frame, &dec_frame,
                         ctx->api_ctx.codec_format, send_sei_with_idr,
                         mdcv_data, cll_data, cc_data, udu_data, hdrp_data);

    // DolbyVision (HRD SEI), HEVC only for now
    uint8_t hrd_buf[NI_LOGAN_MAX_SEI_DATA];
    uint32_t hrd_sei_len = 0; // HRD SEI size in bytes
    if (AV_CODEC_ID_HEVC == avctx->codec_id && p_param->hrd_enable)
    {
      if (send_sei_with_idr)
      {
        hrd_sei_len += encode_buffering_period_sei(p_param, ctx,
                                                   ctx->api_ctx.frame_num + 1,
                                                   hrd_buf);
      }
      //printf(" ^^^^ frame_num %u  idr %d\n", ctx->api_ctx.frame_num, send_sei_with_idr);
      // pic_timing SEI will inserted after encoding
      ctx->api_fme.data.frame.sei_total_len += hrd_sei_len;
    }

    side_data = av_frame_get_side_data(&ctx->buffered_fme,
                                       AV_FRAME_DATA_NETINT_CUSTOM_SEI);
    if (side_data && side_data->size > 0)
    {
      int i = 0;
      int64_t local_pts = ctx->buffered_fme.pts;
      ni_logan_all_custom_sei_t *src_all_custom_sei = (ni_logan_all_custom_sei_t *)side_data->data;
      ni_logan_custom_sei_t *src_custom_sei = NULL;
      uint8_t *src_sei_data = NULL;
      int custom_sei_size = 0;
      int custom_sei_size_trans = 0;
      uint8_t custom_sei_type;
      uint8_t sei_idx;
      int sei_len;
      ni_logan_all_custom_sei_t *dst_all_custom_sei = NULL;
      ni_logan_custom_sei_t *dst_custom_sei = NULL;
      uint8_t *dst_sei_data = NULL;

      dst_all_custom_sei = malloc(sizeof(ni_logan_all_custom_sei_t));
      if (dst_all_custom_sei == NULL)
      {
        av_log(avctx, AV_LOG_ERROR, "failed to allocate memory for custom sei "
               "data, len:%" PRIu64 ".\n", sizeof(ni_logan_all_custom_sei_t));
        ret = AVERROR(ENOMEM);
        return ret;
      }
      memset(dst_all_custom_sei, 0 ,sizeof(ni_logan_all_custom_sei_t));

      for (sei_idx = 0; sei_idx < src_all_custom_sei->custom_sei_cnt; sei_idx++)
      {
        src_custom_sei = &src_all_custom_sei->ni_custom_sei[sei_idx];

        custom_sei_type = src_custom_sei->custom_sei_type;
        custom_sei_size = src_custom_sei->custom_sei_size;
        src_sei_data = src_custom_sei->custom_sei_data;

        dst_custom_sei = &dst_all_custom_sei->ni_custom_sei[sei_idx];
        dst_sei_data = dst_custom_sei->custom_sei_data;
        sei_len = 0;

        /* fill sei buffer */
        // long start code
        dst_sei_data[sei_len++] = 0x00;
        dst_sei_data[sei_len++] = 0x00;
        dst_sei_data[sei_len++] = 0x00;
        dst_sei_data[sei_len++] = 0x01;
        if (AV_CODEC_ID_H264 == avctx->codec_id)
        {
          dst_sei_data[sei_len++] = 0x06;   //nal type: SEI
        }
        else
        {
          dst_sei_data[sei_len++] = 0x4e;   //nal type: SEI
          dst_sei_data[sei_len++] = 0x01;
        }

        // SEI type
        dst_sei_data[sei_len++] = custom_sei_type;

        // original payload size
        custom_sei_size_trans = custom_sei_size;
        while (custom_sei_size_trans >= 0)
        {
          dst_sei_data[sei_len++] = (custom_sei_size_trans > 0xFF ? 0xFF : (uint8_t)custom_sei_size_trans);
          custom_sei_size_trans -= 0xFF;
        }

        // payload data
        for (i = 0; (i < custom_sei_size) && (sei_len < (NI_LOGAN_MAX_CUSTOM_SEI_SZ - 2)); i++)
        {
          if ((2 <= i) && !dst_sei_data[sei_len - 2] && !dst_sei_data[sei_len - 1] && (src_sei_data[i] <= 0x03))
          {
            /* insert 0x3 as emulation_prevention_three_byte */
            dst_sei_data[sei_len++] = 0x03;
          }
          dst_sei_data[sei_len++] = src_sei_data[i];
        }

        if (i != custom_sei_size)
        {
          av_log(avctx, AV_LOG_WARNING, "%s: sei RBSP size out of limit(%d), "
                 "idx=%u, type=%u, size=%d, custom_sei_loc=%d\n", __FUNCTION__,
                 NI_LOGAN_MAX_CUSTOM_SEI_SZ, sei_idx, custom_sei_type,
                 custom_sei_size, src_custom_sei->custom_sei_loc);
          free(dst_all_custom_sei);
          dst_all_custom_sei = NULL;
          break;
        }

        // trailing byte
        dst_sei_data[sei_len++] = 0x80;

        dst_custom_sei->custom_sei_size = sei_len;
        dst_custom_sei->custom_sei_type = custom_sei_type;
        dst_custom_sei->custom_sei_loc = src_custom_sei->custom_sei_loc;
        av_log(avctx, AV_LOG_TRACE, "%s: sei idx=%u,type=%u, len=%d, "
               "custom_sei_loc=%d\n", __FUNCTION__, sei_idx, custom_sei_type,
               sei_len, dst_custom_sei->custom_sei_loc);
      }

      if (dst_all_custom_sei)
      {
        dst_all_custom_sei->custom_sei_cnt = src_all_custom_sei->custom_sei_cnt;
        ctx->api_ctx.pkt_custom_sei[local_pts % NI_LOGAN_FIFO_SZ] = dst_all_custom_sei;
        av_log(avctx, AV_LOG_TRACE, "%s: sei number %d pts %" PRId64 ".\n",
               __FUNCTION__, dst_all_custom_sei->custom_sei_cnt, local_pts);
      }
    }

    if (ctx->api_fme.data.frame.sei_total_len > NI_LOGAN_ENC_MAX_SEI_BUF_SIZE)
    {
      av_log(avctx, AV_LOG_ERROR, "%s: sei total length %u exceeds maximum sei "
             "size %u.\n", __FUNCTION__, ctx->api_fme.data.frame.sei_total_len,
             NI_LOGAN_ENC_MAX_SEI_BUF_SIZE);
      ret = AVERROR(EINVAL);
      return ret;
    }

    ctx->api_fme.data.frame.extra_data_len += ctx->api_fme.data.frame.sei_total_len;
    // FW layout requirement: leave space for reconfig data if SEI and/or ROI
    // is present
    if ((ctx->api_fme.data.frame.sei_total_len ||
         ctx->api_fme.data.frame.roi_len)
        && !ctx->api_fme.data.frame.reconf_len)
    {
      ctx->api_fme.data.frame.extra_data_len += sizeof(ni_logan_encoder_change_params_t);
    }

    if (ctx->api_ctx.auto_dl_handle != NI_INVALID_DEVICE_HANDLE)
    {
      is_hwframe = 0;
      format_in_use = avctx->sw_pix_fmt;
      av_log(avctx, AV_LOG_TRACE, "%s: Autodownload mode, disable hw frame\n",
             __FUNCTION__);
    }
    else
    {
      format_in_use = ctx->buffered_fme.format;
    }

    if (is_hwframe)
    {
      ret = sizeof(ni_logan_hwframe_surface_t);
    }
    else
    {
      ret = av_image_get_buffer_size(format_in_use,
                                     ctx->buffered_fme.width,
                                     ctx->buffered_fme.height, 1);
    }
#if FF_API_PKT_PTS
    av_log(avctx, AV_LOG_TRACE, "%s: pts=%" PRId64 ", pkt_dts=%" PRId64 ", "
           "pkt_pts=%" PRId64 "\n", __FUNCTION__, ctx->buffered_fme.pts,
           ctx->buffered_fme.pkt_dts, ctx->buffered_fme.pkt_pts);
#endif
    av_log(avctx, AV_LOG_TRACE, "%s: buffered_fme.format=%d, width=%d, "
           "height=%d, pict_type=%d, key_frame=%d, size=%d\n", __FUNCTION__,
           format_in_use, ctx->buffered_fme.width, ctx->buffered_fme.height,
           ctx->buffered_fme.pict_type, ctx->buffered_fme.key_frame, ret);

    if (ret < 0)
    {
      return ret;
    }

    if (is_hwframe)
    {
      uint8_t *dsthw;
      const uint8_t *srchw;
      ni_logan_frame_buffer_alloc_hwenc(&(ctx->api_fme.data.frame),
                                  ODD2EVEN(ctx->buffered_fme.width),
                                  ODD2EVEN(ctx->buffered_fme.height),
                                  ctx->api_fme.data.frame.extra_data_len);
      if (!ctx->api_fme.data.frame.p_data[3])
      {
        return AVERROR(ENOMEM);
      }

      dsthw = ctx->api_fme.data.frame.p_data[3];
      srchw = (const uint8_t *) ctx->buffered_fme.data[3];
      av_log(avctx, AV_LOG_TRACE, "dst=%p src=%p, len =%d\n", dsthw, srchw, ctx->api_fme.data.frame.data_len[3]);
      memcpy(dsthw, srchw, ctx->api_fme.data.frame.data_len[3]);
      av_log(avctx, AV_LOG_TRACE, "session_id:%u, FrameIdx:%d, %d, W-%u, H-%u, bit_depth:%d, encoding_type:%d\n",
             ((ni_logan_hwframe_surface_t *)dsthw)->ui16SessionID,
             ((ni_logan_hwframe_surface_t *)dsthw)->i8FrameIdx,
             ((ni_logan_hwframe_surface_t *)dsthw)->i8InstID,
             ((ni_logan_hwframe_surface_t *)dsthw)->ui16width,
             ((ni_logan_hwframe_surface_t *)dsthw)->ui16height,
             ((ni_logan_hwframe_surface_t *)dsthw)->bit_depth,
             ((ni_logan_hwframe_surface_t *)dsthw)->encoding_type);
    }
    else
    {
      int dst_stride[NI_LOGAN_MAX_NUM_DATA_POINTERS] = {0};
      int dst_height_aligned[NI_LOGAN_MAX_NUM_DATA_POINTERS] = {0};
      int src_height[NI_LOGAN_MAX_NUM_DATA_POINTERS] = {0};

      src_height[0] = ctx->buffered_fme.height;
      src_height[1] = src_height[2] = ctx->buffered_fme.height / 2;

      ni_logan_get_hw_yuv420p_dim(frame_width, frame_height,
                            ctx->api_ctx.bit_depth_factor,
                            avctx->codec_id == AV_CODEC_ID_H264,
                            dst_stride, dst_height_aligned);

      // alignment(16) extra padding for H.264 encoding
      ni_logan_encoder_frame_buffer_alloc(&(ctx->api_fme.data.frame),
                                    ODD2EVEN(ctx->buffered_fme.width),
                                    ODD2EVEN(ctx->buffered_fme.height),
                                    dst_stride,
                                    (avctx->codec_id == AV_CODEC_ID_H264),
                                    ctx->api_fme.data.frame.extra_data_len,
                                    ctx->api_ctx.bit_depth_factor);
      if (!ctx->api_fme.data.frame.p_data[0])
      {
        return AVERROR(ENOMEM);
      }

      if (ctx->api_ctx.auto_dl_handle == NI_INVALID_DEVICE_HANDLE)
      {
        av_log(avctx, AV_LOG_TRACE, "%s: api_fme.data_len[0]=%d,"
               "buffered_fme.linesize=%d/%d/%d, dst alloc linesize = %d/%d/%d, "
               "src height = %d/%d%d, dst height aligned = %d/%d/%d, "
               "ctx->api_fme.force_key_frame=%d, extra_data_len=%d sei_size=%u "
               "(hdr_content_light_level %u hdr_mastering_display_color_vol %u "
               "hdr10+ %u cc %u udu %u prefC %u hrd %u) "
               "reconf_size=%u roi_size=%u force_pic_qp=%u "
               "use_cur_src_as_long_term_pic %u use_long_term_ref %u\n",
               __FUNCTION__, ctx->api_fme.data.frame.data_len[0],
               ctx->buffered_fme.linesize[0],
               ctx->buffered_fme.linesize[1],
               ctx->buffered_fme.linesize[2],
               dst_stride[0], dst_stride[1], dst_stride[2],
               src_height[0], src_height[1], src_height[2],
               dst_height_aligned[0], dst_height_aligned[1], dst_height_aligned[2],
               ctx->api_fme.data.frame.force_key_frame,
               ctx->api_fme.data.frame.extra_data_len,
               ctx->api_fme.data.frame.sei_total_len,
               ctx->api_fme.data.frame.sei_hdr_content_light_level_info_len,
               ctx->api_fme.data.frame.sei_hdr_mastering_display_color_vol_len,
               ctx->api_fme.data.frame.sei_hdr_plus_len,
               ctx->api_fme.data.frame.sei_cc_len,
               ctx->api_fme.data.frame.sei_user_data_unreg_len,
               ctx->api_fme.data.frame.preferred_characteristics_data_len,
               hrd_sei_len,
               ctx->api_fme.data.frame.reconf_len,
               ctx->api_fme.data.frame.roi_len,
               ctx->api_fme.data.frame.force_pic_qp,
               ctx->api_fme.data.frame.use_cur_src_as_long_term_pic,
               ctx->api_fme.data.frame.use_long_term_ref);

        // YUV part of the encoder input data layout
        ni_logan_copy_hw_yuv420p((uint8_t **) ctx->api_fme.data.frame.p_data,
                           ctx->buffered_fme.data, ctx->buffered_fme.width,
                           ctx->buffered_fme.height,
                           ctx->api_ctx.bit_depth_factor,
                           dst_stride, dst_height_aligned,
                           ctx->buffered_fme.linesize, src_height);

        av_log(avctx, AV_LOG_TRACE, "After memcpy p_data 0:0x%p, 1:0x%p, 2:0x%p"
               " len:0:%d 1:%d 2:%d\n",
               ctx->api_fme.data.frame.p_data[0],
               ctx->api_fme.data.frame.p_data[1],
               ctx->api_fme.data.frame.p_data[2],
               ctx->api_fme.data.frame.data_len[0],
               ctx->api_fme.data.frame.data_len[1],
               ctx->api_fme.data.frame.data_len[2]);
      }
      else
      {
        ni_logan_hwframe_surface_t *src_surf;
        ni_logan_session_data_io_t *p_session_data;
        av_log(avctx, AV_LOG_TRACE, "%s: Autodownload to be run\n", __FUNCTION__);
        avhwf_ctx = (AVHWFramesContext*)ctx->buffered_fme.hw_frames_ctx->data;
        nif_src_ctx = avhwf_ctx->internal->priv;
        src_surf = (ni_logan_hwframe_surface_t*)ctx->buffered_fme.data[3];
        p_session_data = &ctx->api_fme;

        ret = ni_logan_device_session_hwdl(&nif_src_ctx->api_ctx, p_session_data, src_surf);
        if (ret <= 0)
        {
          av_log(avctx, AV_LOG_ERROR, "nienc.c:ni_logan_hwdl_frame() failed to retrieve frame\n");
          return AVERROR_EXTERNAL;
        }
      }
    }

    // auxiliary data part of the encoder input data layout
    ni_logan_enc_copy_aux_data(&ctx->api_ctx, &ctx->api_fme.data.frame, &dec_frame,
                         ctx->api_ctx.codec_format, mdcv_data, cll_data,
                         cc_data, udu_data, hdrp_data);
    ni_logan_frame_buffer_free(&dec_frame);

    // fill in HRD SEI if available
    if (hrd_sei_len)
    {
      uint8_t *dst = (uint8_t *)ctx->api_fme.data.frame.p_data[3] +
      ctx->api_fme.data.frame.data_len[3] + NI_LOGAN_APP_ENC_FRAME_META_DATA_SIZE;

      // skip data portions already filled in until the HRD SEI part;
      // reserve reconfig size if any of sei, roi or reconfig is present
      if (ctx->api_fme.data.frame.reconf_len ||
          ctx->api_fme.data.frame.roi_len ||
          ctx->api_fme.data.frame.sei_total_len)
      {
        dst += sizeof(ni_logan_encoder_change_params_t);
      }

      // skip any of the following data types enabled, to get to HRD location:
      // - ROI map
      // - HDR mastering display color volume
      // - HDR content light level info
      // - HLG preferred characteristics SEI
      // - close caption
      // - HDR10+
      // - User data unregistered SEI
      dst += ctx->api_fme.data.frame.roi_len +
      ctx->api_fme.data.frame.sei_hdr_mastering_display_color_vol_len +
      ctx->api_fme.data.frame.sei_hdr_content_light_level_info_len +
      ctx->api_fme.data.frame.preferred_characteristics_data_len +
      ctx->api_fme.data.frame.sei_cc_len +
      ctx->api_fme.data.frame.sei_hdr_plus_len +
      ctx->api_fme.data.frame.sei_user_data_unreg_len;

      memcpy(dst, hrd_buf, hrd_sei_len);
    }

    ctx->sentFrame = 1;
  }
#ifdef NIENC_MULTI_THREAD
  if (ctx->encoder_flushing)
  {
    sent = ni_logan_device_session_write(&ctx->api_ctx, &ctx->api_fme, NI_LOGAN_DEVICE_TYPE_ENCODER);

    av_log(avctx, AV_LOG_DEBUG, "%s encoder_flushing: size %d sent to xcoder\n",
           __FUNCTION__, sent);

    if (NI_LOGAN_RETCODE_ERROR_RESOURCE_UNAVAILABLE == sent)
    {
      av_log(avctx, AV_LOG_DEBUG, "%s(): Sequence Change in progress, return "
             " EAGAIN\n", __FUNCTION__);
      ret = AVERROR(EAGAIN);
      return ret;
    }
    else if (NI_LOGAN_RETCODE_ERROR_VPU_RECOVERY == sent)
    {
      sent = xcoder_logan_encode_reset(avctx);
    }

    if (sent < 0)
    {
      ret = AVERROR(EIO);
    }
    else
    {
      if (frame && is_hwframe)
      {
        av_frame_ref(ctx->sframe_pool[ctx->aFree_Avframes_list[ctx->freeHead]], frame);
        av_log(avctx, AV_LOG_DEBUG, "AVframe_index = %d popped from head %d\n",
               ctx->aFree_Avframes_list[ctx->freeHead], ctx->freeHead);
        if (deq_logan_free_frames(ctx)!= 0)
        {
          ret = AVERROR_EXTERNAL;
          return ret;
        }
      }
      //pushing input pts in circular FIFO
      ctx->api_ctx.enc_pts_list[ctx->api_ctx.enc_pts_w_idx % NI_LOGAN_FIFO_SZ] = ctx->api_fme.data.frame.pts;
      ctx->api_ctx.enc_pts_w_idx++;
      ret = 0;
    }
  }
  else if (is_hwframe)
  {
    sent = ni_logan_device_session_write(&ctx->api_ctx, &ctx->api_fme, NI_LOGAN_DEVICE_TYPE_ENCODER);
    //ctx->sframe_pool[((niFrameSurface1_t*)((uint8_t*)frame->data[3]))->i8FrameIdx] = av_buffer_ref(frame);
    av_log(avctx, AV_LOG_DEBUG, "%s: size %d sent to xcoder\n",
           __FUNCTION__, sent);

    if (NI_LOGAN_RETCODE_ERROR_RESOURCE_UNAVAILABLE == sent)
    {
      av_log(avctx, AV_LOG_DEBUG, "%s(): Sequence Change in progress, return "
             "EAGAIN\n", __FUNCTION__);
      ret = AVERROR(EAGAIN);
      return ret;
    }

    if (sent == -1)
    {
      ret = AVERROR(EAGAIN);
    }
    else
    {
      av_frame_ref(ctx->sframe_pool[((ni_logan_hwframe_surface_t*)((uint8_t*)frame->data[3]))->i8FrameIdx], frame);
      av_log(avctx, AV_LOG_DEBUG, "AVframe_index = %d popped from head %d\n",
             ctx->aFree_Avframes_list[ctx->freeHead], ctx->freeHead);
      if (deq_logan_free_frames(ctx) != 0)
      {
        ret = AVERROR_EXTERNAL;
        return ret;
      }
      //av_frame_ref(ctx->sframe_pool[((ni_logan_hwframe_surface_t*)((uint8_t*)frame->data[3]))->ui16FrameIdx], frame);
      ret = 0;
    }
  }
  else
  {
    av_log(avctx, AV_LOG_DEBUG, "%s start 111 %p, session_info %d, "
           "device_handle %d\n", __FUNCTION__, ctx->api_ctx.session_info,
           ctx->api_ctx.session_id, ctx->api_ctx.device_handle);
    if ((ctx->api_ctx.session_id != NI_LOGAN_INVALID_SESSION_ID) && (ctx->api_ctx.device_handle != NI_INVALID_DEVICE_HANDLE))
    {
      av_log(avctx, AV_LOG_DEBUG, "%s start 111 %p\n",
             __FUNCTION__, ctx->api_ctx.session_info);
      write_thread_arg_struct_t *write_thread_args = (write_thread_arg_struct_t *)malloc(sizeof(write_thread_arg_struct_t));
      pthread_mutex_init(&write_thread_args->mutex, NULL);
      pthread_cond_init(&write_thread_args->cond, NULL);
      write_thread_args->running = 0;
      write_thread_args->ctx = ctx;
      av_log(avctx, AV_LOG_DEBUG, "%s: session_id %d, device_handle %d\n",
             __FUNCTION__, ctx->api_ctx.session_id, ctx->api_ctx.device_handle);
      av_log(avctx, AV_LOG_DEBUG, "%s: ctx %p\n",
             __FUNCTION__, write_thread_args->ctx);
      ctx->api_ctx.session_info = (void *)write_thread_args;
      write_thread_args->running = 1;
      ret = threadpool_auto_add_task_thread(&pool, write_frame_thread, write_thread_args, 1);
      if (ret < 0)
      {
        av_log(avctx, AV_LOG_ERROR, "failed to add_task_thread to threadpool\n");
        return ret;
      }
    }
  }
#else
  sent = ni_logan_device_session_write(&ctx->api_ctx, &ctx->api_fme, NI_LOGAN_DEVICE_TYPE_ENCODER);
  av_log(avctx, AV_LOG_DEBUG, "%s: pts %lld dts %lld size %d sent to xcoder\n",
         __FUNCTION__, ctx->api_fme.data.frame.pts, ctx->api_fme.data.frame.dts, sent);

  // return EIO at error
  if (NI_LOGAN_RETCODE_ERROR_VPU_RECOVERY == sent)
  {
    ret = xcoder_logan_encode_reset(avctx);
    if (ret < 0)
    {
      av_log(avctx, AV_LOG_ERROR, "%s(): VPU recovery failed:%d, return EIO\n",
             __FUNCTION__, sent);
      ret = AVERROR(EIO);
    }
    return ret;
  }
  else if (sent < 0)
  {
    av_log(avctx, AV_LOG_ERROR, "%s(): failure sent (%d) , return EIO\n",
           __FUNCTION__, sent);
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
  else if (sent == 0)
  {
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
    if (ctx->api_ctx.status == NI_LOGAN_RETCODE_NVME_SC_WRITE_BUFFER_FULL)
    {
      if (ctx->api_param.strict_timeout_mode)
      {
        av_log(avctx, AV_LOG_ERROR, "%s: Error Strict timeout period exceeded, "
               "return EAGAIN\n", __FUNCTION__);
        ret = AVERROR(EAGAIN);
      }
      else
      {
        av_log(avctx, AV_LOG_DEBUG, "%s: Write buffer full, returning 1\n",
               __FUNCTION__);
        ret = 1;
        if (frame && is_logan_input_fifo_empty(ctx))
        {
          enqueue_logan_frame(avctx, frame);
        }
      }
    }
  }
  else
  {
    if (!ctx->eos_fme_received && is_hwframe)
    {
      av_frame_ref(ctx->sframe_pool[ctx->aFree_Avframes_list[ctx->freeHead]], &ctx->buffered_fme);
      av_log(avctx, AV_LOG_DEBUG, "AVframe_index = %d popped from free head %d\n", ctx->aFree_Avframes_list[ctx->freeHead], ctx->freeHead);
      av_log(avctx, AV_LOG_TRACE, "ctx->buffered_fme.data[3] %p sframe_pool[%d]->data[3] %p\n",
             ctx->buffered_fme.data[3], ctx->aFree_Avframes_list[ctx->freeHead],
             ctx->sframe_pool[ctx->aFree_Avframes_list[ctx->freeHead]]->data[3]);
      if (ctx->sframe_pool[ctx->aFree_Avframes_list[ctx->freeHead]]->data[3])
      {
        av_log(avctx, AV_LOG_DEBUG, "sframe_pool[%d] ui16FrameIdx %u, device_handle %" PRId64 ".\n",
               ctx->aFree_Avframes_list[ctx->freeHead],
               ((ni_logan_hwframe_surface_t*)((uint8_t*)ctx->sframe_pool[ctx->aFree_Avframes_list[ctx->freeHead]]->data[3]))->i8FrameIdx,
               ((ni_logan_hwframe_surface_t*)((uint8_t*)ctx->sframe_pool[ctx->aFree_Avframes_list[ctx->freeHead]]->data[3]))->device_handle);
        av_log(avctx, AV_LOG_TRACE, "%s: after ref sframe_pool, hw frame av_buffer_get_ref_count=%d, data[3]=%p\n",
               __FUNCTION__, av_buffer_get_ref_count(ctx->sframe_pool[ctx->aFree_Avframes_list[ctx->freeHead]]->buf[0]),
               ctx->sframe_pool[ctx->aFree_Avframes_list[ctx->freeHead]]->data[3]);
      }
      if (deq_logan_free_frames(ctx) != 0)
      {
        av_log(avctx, AV_LOG_ERROR, "free frames is empty\n");
        ret = AVERROR_EXTERNAL;
        return ret;
      }
    }

    // only if it's NOT sequence change flushing (in which case only the eos
    // was sent and not the first sc pkt) AND
    // only after successful sending will it be removed from fifo
    if (LOGAN_SESSION_RUN_STATE_SEQ_CHANGE_DRAINING != ctx->api_ctx.session_run_state)
    {
      if (! is_logan_input_fifo_empty(ctx))
      {
        av_fifo_drain(ctx->fme_fifo, sizeof(AVFrame));
        av_log(avctx, AV_LOG_DEBUG, "fme popped pts:%" PRId64 ", "
               "fifo size: %" PRIu64 "\n",  ctx->buffered_fme.pts,
               av_fifo_size(ctx->fme_fifo) / sizeof(AVFrame));
      }
      av_frame_unref(&ctx->buffered_fme);
    }
    else
    {
      av_log(avctx, AV_LOG_TRACE, "XCoder frame(eos) sent, sequence changing! NO fifo pop !\n");
    }

    //pushing input pts in circular FIFO
    ctx->api_ctx.enc_pts_list[ctx->api_ctx.enc_pts_w_idx % NI_LOGAN_FIFO_SZ] = ctx->api_fme.data.frame.pts;
    ctx->api_ctx.enc_pts_w_idx++;
    ret = 0;

    // have another check before return: if no more frames in fifo to send and
    // we've got eos (NULL) frame from upper stream, flag for flushing
    if (ctx->eos_fme_received && is_logan_input_fifo_empty(ctx))
    {
      av_log(avctx, AV_LOG_DEBUG, "Upper stream EOS frame received, fifo empty, start flushing ..\n");
      ctx->encoder_flushing = 1;
    }
  }
#endif
  if (ctx->encoder_flushing)
  {
    av_log(avctx, AV_LOG_DEBUG, "%s flushing ..\n", __FUNCTION__);
    ret = ni_logan_device_session_flush(&ctx->api_ctx, NI_LOGAN_DEVICE_TYPE_ENCODER);
  }

  return ret;
}

static int xcoder_logan_encode_reinit(AVCodecContext *avctx)
{
  int ret = 0;
  AVFrame tmp_fme;
  XCoderLoganEncContext *ctx = avctx->priv_data;
  ni_logan_session_run_state_t prev_state = ctx->api_ctx.session_run_state;

  ctx->eos_fme_received = 0;
  ctx->encoder_eof = 0;
  ctx->encoder_flushing = 0;

  if (ctx->api_ctx.pts_table && ctx->api_ctx.dts_queue)
  {
    ff_xcoder_logan_encode_close(avctx);
    ctx->api_ctx.session_run_state = prev_state;
  }
  ctx->started = 0;
  ctx->firstPktArrived = 0;
  ctx->spsPpsArrived = 0;
  ctx->spsPpsHdrLen = 0;
  ctx->p_spsPpsHdr = NULL;

  // and re-init avctx's resolution to the changed one that is
  // stored in the first frame of the fifo
  av_fifo_generic_peek(ctx->fme_fifo, &tmp_fme, sizeof(AVFrame), NULL);
  av_log(avctx, AV_LOG_INFO, "%s resolution changing %dx%d -> %dx%d\n",
         __FUNCTION__, avctx->width, avctx->height, tmp_fme.width, tmp_fme.height);
  avctx->width = tmp_fme.width;
  avctx->height = tmp_fme.height;

  ret = ff_xcoder_logan_encode_init(avctx);
  ctx->api_ctx.session_run_state = LOGAN_SESSION_RUN_STATE_NORMAL;

  while ((ret >= 0) && !is_logan_input_fifo_empty(ctx))
  {
    ctx->api_ctx.session_run_state = LOGAN_SESSION_RUN_STATE_QUEUED_FRAME_DRAINING;
    ret = xcoder_send_frame(avctx, NULL);

    // new resolution changes or buffer full should break flush.
    // if needed, add new cases here
    if (LOGAN_SESSION_RUN_STATE_SEQ_CHANGE_DRAINING == ctx->api_ctx.session_run_state)
    {
      av_log(avctx, AV_LOG_DEBUG, "%s(): break flush queued frames, "
             "resolution changes again, session_run_state=%d, status=%d\n",
             __FUNCTION__, ctx->api_ctx.session_run_state, ctx->api_ctx.status);
      break;
    }
    else if (NI_LOGAN_RETCODE_NVME_SC_WRITE_BUFFER_FULL == ctx->api_ctx.status)
    {
      ctx->api_ctx.session_run_state = LOGAN_SESSION_RUN_STATE_NORMAL;
      av_log(avctx, AV_LOG_DEBUG, "%s(): break flush queued frames, "
            "because of buffer full, session_run_state=%d, status=%d\n",
            __FUNCTION__, ctx->api_ctx.session_run_state, ctx->api_ctx.status);
      break;
    }
    else
    {
      ctx->api_ctx.session_run_state = LOGAN_SESSION_RUN_STATE_NORMAL;
      av_log(avctx, AV_LOG_DEBUG, "%s(): continue to flush queued frames, "
             "ret=%d\n", __FUNCTION__, ret);
    }
  }

  return ret;
}

static int xcoder_logan_receive_packet(AVCodecContext *avctx, AVPacket *pkt)
{
  XCoderLoganEncContext *ctx = avctx->priv_data;
  ni_logan_encoder_params_t *p_param = &ctx->api_param;
  int ret = 0;
  int recv;
  ni_logan_packet_t *xpkt = &ctx->api_pkt.data.packet;

  av_log(avctx, AV_LOG_DEBUG, "%s\n", __FUNCTION__);

  if (ctx->encoder_eof)
  {
    av_log(avctx, AV_LOG_TRACE, "%s: EOS\n", __FUNCTION__);
    return AVERROR_EOF;
  }

  ni_logan_packet_buffer_alloc(xpkt, NI_LOGAN_MAX_TX_SZ);
  while (1)
  {
    xpkt->recycle_index = -1;
    recv = ni_logan_device_session_read(&ctx->api_ctx, &ctx->api_pkt, NI_LOGAN_DEVICE_TYPE_ENCODER);

    av_log(avctx, AV_LOG_TRACE, "%s: xpkt.end_of_stream=%d, xpkt.data_len=%d, "
           "recv=%d, encoder_flushing=%d, encoder_eof=%d\n", __FUNCTION__,
           xpkt->end_of_stream, xpkt->data_len, recv, ctx->encoder_flushing,
           ctx->encoder_eof);

    if (recv <= 0)
    {
      ctx->encoder_eof = xpkt->end_of_stream;
      /* not ready ?? */
      if (ctx->encoder_eof || xpkt->end_of_stream)
      {
        if (LOGAN_SESSION_RUN_STATE_SEQ_CHANGE_DRAINING ==
            ctx->api_ctx.session_run_state)
        {
          // after sequence change completes, reset codec state
          av_log(avctx, AV_LOG_INFO, "%s 1: sequence change completed, return "
                 "EAGAIN and will reopen " "codec!\n", __FUNCTION__);

          ret = xcoder_logan_encode_reinit(avctx);
          if (ret >= 0)
          {
            ret = AVERROR(EAGAIN);
          }
          break;
        }

        ret = AVERROR_EOF;
        av_log(avctx, AV_LOG_TRACE, "%s: got encoder_eof, "
               "return AVERROR_EOF\n", __FUNCTION__);
        break;
      }
      else
      {
        if (NI_LOGAN_RETCODE_ERROR_VPU_RECOVERY == recv)
        {
          ret = xcoder_logan_encode_reset(avctx);
          if (ret < 0)
          {
            av_log(avctx, AV_LOG_ERROR, "%s(): VPU recovery failed:%d, "
                   "returning EIO\n", __FUNCTION__, recv);
            ret = AVERROR(EIO);
          }
          return ret;
        }

        if (recv < 0)
        {
          if ((NI_LOGAN_RETCODE_ERROR_INVALID_SESSION == recv) && !ctx->started)  // session may be in recovery state, return EAGAIN
          {
            av_log(avctx, AV_LOG_ERROR, "%s: VPU might be reset, "
                   "invalid session id\n", __FUNCTION__);
            ret = AVERROR(EAGAIN);
          }
          else
          {
            av_log(avctx, AV_LOG_ERROR, "%s: Persistent failure, "
                   "returning EIO,ret=%d\n", __FUNCTION__, recv);
            ret = AVERROR(EIO);
          }
          ctx->gotPacket = 0;
          ctx->sentFrame = 0;
          break;
        }

        if (ctx->api_param.low_delay_mode && ctx->sentFrame && !ctx->gotPacket)
        {
          av_log(avctx, AV_LOG_TRACE, "%s: low delay mode, keep reading until "
                 "pkt arrives\n", __FUNCTION__);
          continue;
        }

        ctx->gotPacket = 0;
        ctx->sentFrame = 0;
        if (!is_logan_input_fifo_empty(ctx) &&
            (LOGAN_SESSION_RUN_STATE_NORMAL == ctx->api_ctx.session_run_state) &&
            (NI_LOGAN_RETCODE_NVME_SC_WRITE_BUFFER_FULL != ctx->api_ctx.status))
        {
          ctx->api_ctx.session_run_state = LOGAN_SESSION_RUN_STATE_QUEUED_FRAME_DRAINING;
          ret = xcoder_send_frame(avctx, NULL);

          // if session_run_state is changed in xcoder_send_frame, keep it
          if (LOGAN_SESSION_RUN_STATE_QUEUED_FRAME_DRAINING == ctx->api_ctx.session_run_state)
          {
            ctx->api_ctx.session_run_state = LOGAN_SESSION_RUN_STATE_NORMAL;
          }
          if (ret < 0)
          {
            av_log(avctx, AV_LOG_ERROR, "%s(): xcoder_send_frame 1 error, "
                   "ret=%d\n", __FUNCTION__, ret);
            return ret;
          }
          continue;
        }
        ret = AVERROR(EAGAIN);
        if (! ctx->encoder_flushing && ! ctx->eos_fme_received)
        {
          av_log(avctx, AV_LOG_TRACE, "%s: NOT encoder_flushing, NOT "
                 "eos_fme_received, return AVERROR(EAGAIN)\n", __FUNCTION__);
          break;
        }
      }
    }
    else
    {
      /* got encoded data back */
      int meta_size = NI_LOGAN_FW_ENC_BITSTREAM_META_DATA_SIZE;
      if (avctx->pix_fmt == AV_PIX_FMT_NI_LOGAN && xpkt->recycle_index >= 0 && xpkt->recycle_index < 1056)
      {
        int avframe_index;
        av_log(avctx, AV_LOG_VERBOSE, "UNREF index %d.\n", xpkt->recycle_index);
        avframe_index = recycle_logan_index_2_avframe_index(ctx, xpkt->recycle_index);
        if (avframe_index >=0 && ctx->sframe_pool[avframe_index])
        {
          AVFrame *frame = ctx->sframe_pool[avframe_index];
          void *opaque = av_buffer_get_opaque(frame->buf[0]);
          // This opaque would carry the valid event handle to help release the
          // hwframe surface descriptor for windows target.
          opaque = (void *) ctx->api_ctx.event_handle;
          av_log(avctx, AV_LOG_TRACE, "%s: after ref sframe_pool, hw frame "
                 "av_buffer_get_ref_count=%d, data[3]=%p event handle:%p\n",
                 __FUNCTION__, av_buffer_get_ref_count(frame->buf[0]),
                 frame->data[3], opaque);
          av_frame_unref(frame);
          av_log(avctx, AV_LOG_DEBUG, "AVframe_index = %d pushed to free tail "
                 "%d\n", avframe_index, ctx->freeTail);
          if (enq_logan_free_frames(ctx, avframe_index) != 0)
          {
            av_log(avctx, AV_LOG_ERROR, "free frames is full\n");
          }
          av_log(avctx, AV_LOG_DEBUG, "enq head %d, tail %d\n",ctx->freeHead, ctx->freeTail);
          //enqueue the index back to free
          xpkt->recycle_index = -1;
        }
        else
        {
          av_log(avctx, AV_LOG_DEBUG, "can't push to tail - avframe_index %d sframe_pool %p\n",
                 avframe_index, ctx->sframe_pool[avframe_index]);
        }
      }

      if (! ctx->spsPpsArrived)
      {
        ret = AVERROR(EAGAIN);
        ctx->spsPpsArrived = 1;
        ctx->spsPpsHdrLen = recv - meta_size;
        ctx->p_spsPpsHdr = malloc(ctx->spsPpsHdrLen);
        if (! ctx->p_spsPpsHdr)
        {
          ret = AVERROR(ENOMEM);
          break;
        }

        memcpy(ctx->p_spsPpsHdr, (uint8_t*)xpkt->p_data + meta_size, xpkt->data_len - meta_size);

        // start pkt_num counter from 1 to get the real first frame
        ctx->api_ctx.pkt_num = 1;
        // for low-latency mode, keep reading until the first frame is back
        if (ctx->api_param.low_delay_mode)
        {
          av_log(avctx, AV_LOG_TRACE, "%s: low delay mode, keep reading until "
                 "1st pkt arrives\n", __FUNCTION__);
          continue;
        }
        break;
      }
      ctx->gotPacket = 1;
      ctx->sentFrame = 0;

      uint8_t pic_timing_buf[NI_LOGAN_MAX_SEI_DATA];
      uint32_t pic_timing_sei_len = 0;
      int nalu_type = 0;
      const uint8_t *p_start_code;
      uint32_t stc = -1;
      uint32_t copy_len = 0;
      uint8_t *p_src = (uint8_t*)xpkt->p_data + meta_size;
      uint8_t *p_end = p_src + (xpkt->data_len - meta_size);
      int is_idr = 0;
      int64_t local_pts = xpkt->pts;
      int custom_sei_cnt = 0;
      int total_custom_sei_len = 0;
      int sei_idx = 0;
      ni_logan_all_custom_sei_t *ni_logan_all_custom_sei;
      ni_logan_custom_sei_t *ni_custom_sei;
      if (ctx->api_ctx.pkt_custom_sei[local_pts % NI_LOGAN_FIFO_SZ])
      {
        ni_logan_all_custom_sei = ctx->api_ctx.pkt_custom_sei[local_pts % NI_LOGAN_FIFO_SZ];
        custom_sei_cnt = ni_logan_all_custom_sei->custom_sei_cnt;
        for (sei_idx = 0; sei_idx < custom_sei_cnt; sei_idx++)
        {
          total_custom_sei_len += ni_logan_all_custom_sei->ni_custom_sei[sei_idx].custom_sei_size;
        }
      }

      if (p_param->hrd_enable || custom_sei_cnt)
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
          av_log(avctx, AV_LOG_ERROR, "%s: codec %d not supported for SEI !\n",
                 __FUNCTION__, avctx->codec_id);
        }

        if (p_param->hrd_enable)
        {
          int is_i_or_idr;
          if (HEVC_NAL_IDR_W_RADL == nalu_type || HEVC_NAL_IDR_N_LP == nalu_type)
          {
            is_idr = 1;
          }
          is_i_or_idr = (LOGAN_PIC_TYPE_I   == xpkt->frame_type ||
                         LOGAN_PIC_TYPE_IDR == xpkt->frame_type ||
                         LOGAN_PIC_TYPE_CRA == xpkt->frame_type);
          pic_timing_sei_len = encode_pic_timing_sei2(p_param, ctx,
                               pic_timing_buf, is_i_or_idr, is_idr, xpkt->pts);
          // returned pts is display number
        }
      }

      if (! ctx->firstPktArrived)
      {
        int sizeof_spspps_attached_to_idr = ctx->spsPpsHdrLen;

        // if not enable forced repeat header, check AV_CODEC_FLAG_GLOBAL_HEADER flag
        // to determine whether to add a SPS/PPS header in the first packat
        if ((avctx->flags & AV_CODEC_FLAG_GLOBAL_HEADER) &&
            (p_param->hevc_enc_params.forced_header_enable != NI_LOGAN_ENC_REPEAT_HEADERS_ALL_KEY_FRAMES) &&
             (p_param->hevc_enc_params.forced_header_enable != NI_LOGAN_ENC_REPEAT_HEADERS_ALL_I_FRAMES))
        {
          sizeof_spspps_attached_to_idr = 0;
        }
        ctx->firstPktArrived = 1;
        ctx->first_frame_pts = xpkt->pts;

        ret = ff_get_encode_buffer(avctx, pkt, xpkt->data_len - meta_size + sizeof_spspps_attached_to_idr + total_custom_sei_len + pic_timing_sei_len, 0);
        if (! ret)
        {
          uint8_t *p_side_data, *p_dst;
          // fill in AVC/HEVC sidedata
          if ((avctx->flags & AV_CODEC_FLAG_GLOBAL_HEADER) &&
              (avctx->extradata_size != ctx->spsPpsHdrLen ||
               memcmp(avctx->extradata, ctx->p_spsPpsHdr, ctx->spsPpsHdrLen)))
          {
            avctx->extradata_size = ctx->spsPpsHdrLen;
            free(avctx->extradata);
            avctx->extradata = av_mallocz(avctx->extradata_size +
                                          AV_INPUT_BUFFER_PADDING_SIZE);
            if (! avctx->extradata)
            {
              av_log(avctx, AV_LOG_ERROR, "Cannot allocate AVC/HEVC header of "
                     "size %d.\n", avctx->extradata_size);
              return AVERROR(ENOMEM);
            }
            memcpy(avctx->extradata, ctx->p_spsPpsHdr, avctx->extradata_size);
          }

          p_side_data = av_packet_new_side_data(pkt, AV_PKT_DATA_NEW_EXTRADATA,
                                                ctx->spsPpsHdrLen);
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

          // 1st pkt, skip buffering_period SEI and insert pic_timing SEI
          if (pic_timing_sei_len || custom_sei_cnt)
          {
            // copy buf_period
            memcpy(p_dst, p_src, copy_len);
            p_dst += copy_len;

            // copy custom sei before slice
            sei_idx = 0;
            while (sei_idx < custom_sei_cnt)
            {
              ni_custom_sei = &ni_logan_all_custom_sei->ni_custom_sei[sei_idx];
              if (ni_custom_sei->custom_sei_loc == NI_LOGAN_CUSTOM_SEI_LOC_AFTER_VCL)
              {
                break;
              }
              memcpy(p_dst, ni_custom_sei->custom_sei_data, ni_custom_sei->custom_sei_size);
              p_dst += ni_custom_sei->custom_sei_size;
              sei_idx++;
            }

            // copy pic_timing
            if (pic_timing_sei_len)
            {
              memcpy(p_dst, pic_timing_buf, pic_timing_sei_len);
              p_dst += pic_timing_sei_len;
            }

            // copy the IDR data
            memcpy(p_dst, p_src + copy_len,
                   xpkt->data_len - meta_size - copy_len);
            p_dst += (xpkt->data_len - meta_size - copy_len);

            // copy custom sei after slice
            while (sei_idx < custom_sei_cnt)
            {
              ni_custom_sei = &ni_logan_all_custom_sei->ni_custom_sei[sei_idx];
              memcpy(p_dst, ni_custom_sei->custom_sei_data, ni_custom_sei->custom_sei_size);
              p_dst += ni_custom_sei->custom_sei_size;
              sei_idx++;
            }
          }
          else
          {
            memcpy(p_dst, (uint8_t*)xpkt->p_data + meta_size,
                   xpkt->data_len - meta_size);
          }
        }

        // free buffer
        if (custom_sei_cnt)
        {
          free(ctx->api_ctx.pkt_custom_sei[local_pts % NI_LOGAN_FIFO_SZ]);
          ctx->api_ctx.pkt_custom_sei[local_pts % NI_LOGAN_FIFO_SZ] = NULL;
        }
      }
      else
      {
        // insert header when intraRefresh is enabled and forced header mode is 1 (all key frames)
        // for every intraRefreshMinPeriod key frames, pkt counting starts from 1, e.g. for
        // cycle of 100, the header is forced on frame 102, 202, ...;
        // note that api_ctx.pkt_num returned is the actual index + 1
        int intra_refresh_hdr_sz = 0;
        if (ctx->p_spsPpsHdr && ctx->spsPpsHdrLen &&
            (p_param->hevc_enc_params.forced_header_enable == NI_LOGAN_ENC_REPEAT_HEADERS_ALL_KEY_FRAMES) &&
            (1 == p_param->hevc_enc_params.intra_mb_refresh_mode ||
             2 == p_param->hevc_enc_params.intra_mb_refresh_mode ||
             3 == p_param->hevc_enc_params.intra_mb_refresh_mode) &&
            p_param->ui32minIntraRefreshCycle > 0 &&
            ctx->api_ctx.pkt_num > 3 &&
            0 == ((ctx->api_ctx.pkt_num - 3) % p_param->ui32minIntraRefreshCycle))
        {
          intra_refresh_hdr_sz = ctx->spsPpsHdrLen;
          av_log(avctx, AV_LOG_TRACE, "%s pkt %" PRId64 " force header on "
                 "intraRefreshMinPeriod %u\n", __FUNCTION__,
                 ctx->api_ctx.pkt_num - 1, p_param->ui32minIntraRefreshCycle);
        }

        ret = ff_get_encode_buffer(avctx, pkt, xpkt->data_len - meta_size + total_custom_sei_len + pic_timing_sei_len + intra_refresh_hdr_sz, 0);
        if (! ret)
        {
          uint8_t *p_dst = pkt->data;
          if (intra_refresh_hdr_sz)
          {
            memcpy(p_dst, ctx->p_spsPpsHdr, intra_refresh_hdr_sz);
            p_dst += intra_refresh_hdr_sz;
          }
          // insert pic_timing if required
          if (pic_timing_sei_len || custom_sei_cnt)
          {
            // for non-IDR, skip AUD and insert
            // for IDR, skip AUD VPS SPS PPS buf_period and insert
            memcpy(p_dst, p_src, copy_len);
            p_dst += copy_len;

            // copy custom sei before slice
            sei_idx = 0;
            while (sei_idx < custom_sei_cnt)
            {
              ni_custom_sei = &ni_logan_all_custom_sei->ni_custom_sei[sei_idx];
              if (ni_custom_sei->custom_sei_loc == NI_LOGAN_CUSTOM_SEI_LOC_AFTER_VCL)
              {
                break;
              }
              memcpy(p_dst, ni_custom_sei->custom_sei_data, ni_custom_sei->custom_sei_size);
              p_dst += ni_custom_sei->custom_sei_size;
              sei_idx++;
            }

            // copy pic_timing
            if (pic_timing_sei_len)
            {
              memcpy(p_dst, pic_timing_buf, pic_timing_sei_len);
              p_dst += pic_timing_sei_len;
            }

            // copy the video data
            memcpy(p_dst, p_src + copy_len,
                   xpkt->data_len - meta_size - copy_len);
            p_dst += (xpkt->data_len - meta_size - copy_len);

            // copy custom sei after slice
            while (sei_idx < custom_sei_cnt)
            {
              ni_custom_sei = &ni_logan_all_custom_sei->ni_custom_sei[sei_idx];
              memcpy(p_dst, ni_custom_sei->custom_sei_data, ni_custom_sei->custom_sei_size);
              p_dst += ni_custom_sei->custom_sei_size;
              sei_idx++;
            }
          }
          else
          {
            memcpy(p_dst, (uint8_t*)xpkt->p_data + meta_size,
                   xpkt->data_len - meta_size);
          }
        }

        // free buffer
        if (custom_sei_cnt)
        {
          free(ctx->api_ctx.pkt_custom_sei[local_pts % NI_LOGAN_FIFO_SZ]);
          ctx->api_ctx.pkt_custom_sei[local_pts % NI_LOGAN_FIFO_SZ] = NULL;
        }
      }
      if (!ret)
      {
        if (LOGAN_PIC_TYPE_IDR == xpkt->frame_type ||
            LOGAN_PIC_TYPE_CRA == xpkt->frame_type)
        {
          pkt->flags |= AV_PKT_FLAG_KEY;
        }
        pkt->pts = xpkt->pts;
        /* to ensure pts>dts for all frames, we assign a guess pts for the first 'dts_offset' frames and then the pts from input stream
         * is extracted from input pts FIFO.
         * if GOP = IBBBP and PTSs = 0 1 2 3 4 5 .. then out DTSs = -3 -2 -1 0 1 ... and -3 -2 -1 are the guessed values
         * if GOP = IBPBP and PTSs = 0 1 2 3 4 5 .. then out DTSs = -1 0 1 2 3 ... and -1 is the guessed value
         * the number of guessed values is equal to dts_offset
         */
        if (ctx->total_frames_received < ctx->dts_offset)
        {
          // guess dts
          pkt->dts = ctx->first_frame_pts + (ctx->total_frames_received - ctx->dts_offset) * avctx->ticks_per_frame;
        }
        else
        {
          // get dts from pts FIFO
          pkt->dts = ctx->api_ctx.enc_pts_list[ctx->api_ctx.enc_pts_r_idx % NI_LOGAN_FIFO_SZ];
          ctx->api_ctx.enc_pts_r_idx++;
        }

        if (ctx->total_frames_received >= 1)
        {
          if (pkt->dts < ctx->latest_dts)
          {
            av_log(avctx, AV_LOG_WARNING, "dts: %" PRId64 ". < latest_dts: "
                   "%" PRId64 ".\n", pkt->dts, ctx->latest_dts);
          }
        }

        if (pkt->dts > pkt->pts)
        {
          av_log(avctx, AV_LOG_WARNING, "dts: %" PRId64 ", pts: %" PRId64 ". "
                 "Forcing dts = pts\n", pkt->dts, pkt->pts);
          pkt->dts = pkt->pts;
        }
        ctx->total_frames_received++;
        ctx->latest_dts = pkt->dts;
        av_log(avctx, AV_LOG_DEBUG, "%s pkt %" PRId64 " pts %" PRId64 " "
               "dts %" PRId64 "  size %d  st_index %d \n", __FUNCTION__,
               ctx->api_ctx.pkt_num - 1, pkt->pts, pkt->dts, pkt->size,
               pkt->stream_index);
      }
      ctx->encoder_eof = xpkt->end_of_stream;

      if (ctx->encoder_eof &&
          LOGAN_SESSION_RUN_STATE_SEQ_CHANGE_DRAINING ==
          ctx->api_ctx.session_run_state)
      {
        // after sequence change completes, reset codec state
        av_log(avctx, AV_LOG_TRACE, "%s 2: sequence change completed, "
               "return 0 and will reopen codec !\n", __FUNCTION__);
        ret = xcoder_logan_encode_reinit(avctx);
      }
      else if(!is_logan_input_fifo_empty(ctx) &&
              (LOGAN_SESSION_RUN_STATE_NORMAL == ctx->api_ctx.session_run_state) &&
              (NI_LOGAN_RETCODE_NVME_SC_WRITE_BUFFER_FULL != ctx->api_ctx.status))
      {
        ctx->api_ctx.session_run_state = LOGAN_SESSION_RUN_STATE_QUEUED_FRAME_DRAINING;
        ret = xcoder_send_frame(avctx, NULL);

        // if session_run_state is changed in xcoder_send_frame, keep it
        if (LOGAN_SESSION_RUN_STATE_QUEUED_FRAME_DRAINING == ctx->api_ctx.session_run_state)
        {
          ctx->api_ctx.session_run_state = LOGAN_SESSION_RUN_STATE_NORMAL;
        }
        if (ret < 0)
        {
          av_log(avctx, AV_LOG_ERROR, "%s: xcoder_send_frame 2 error, ret=%d\n",
                 __FUNCTION__, ret);
          return ret;
        }
      }
      break;
    }
  }

  return ret;
}

int ff_xcoder_logan_encode_frame(AVCodecContext *avctx, AVPacket *pkt,
                           const AVFrame *frame, int *got_packet)
{
  XCoderLoganEncContext *ctx = avctx->priv_data;
  int ret;

  av_log(avctx, AV_LOG_DEBUG, "%s\n", __FUNCTION__);

  ret = xcoder_send_frame(avctx, frame);
  // return immediately for critical errors
  if (AVERROR(ENOMEM) == ret || AVERROR_EXTERNAL == ret ||
      (ret < 0 && ctx->encoder_eof))
  {
    return ret;
  }

  ret = xcoder_logan_receive_packet(avctx, pkt);
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

int ff_xcoder_logan_receive_packet(AVCodecContext *avctx, AVPacket *pkt)
{
  XCoderLoganEncContext *ctx = avctx->priv_data;
  AVFrame *frame = &ctx->buffered_fme;
  int ret;

  ret = ff_encode_get_frame(avctx, frame);
  if (!ctx->encoder_flushing && ret >= 0 || ret == AVERROR_EOF)
  {
    ret = xcoder_send_frame(avctx, (ret == AVERROR_EOF ? NULL : frame));
    if (ret < 0 && ret != AVERROR_EOF)
    {
      return ret;
    }
  }
  // Once send_frame returns EOF go on receiving packets until EOS is met.
  return xcoder_logan_receive_packet(avctx, pkt);
}

bool free_logan_frames_isempty(XCoderLoganEncContext *ctx)
{
  return  (ctx->freeHead == ctx->freeTail);
}

bool free_logan_frames_isfull(XCoderLoganEncContext *ctx)
{
  return  (ctx->freeHead == ((ctx->freeTail == LOGAN_MAX_NUM_FRAMEPOOL_HWAVFRAME) ? 0 : ctx->freeTail + 1));
}

int deq_logan_free_frames(XCoderLoganEncContext *ctx)
{
  if (free_logan_frames_isempty(ctx))
  {
    return -1;
  }
  ctx->aFree_Avframes_list[ctx->freeHead] = -1;
  ctx->freeHead = (ctx->freeHead == LOGAN_MAX_NUM_FRAMEPOOL_HWAVFRAME) ? 0 : ctx->freeHead + 1;
  return 0;
}

int enq_logan_free_frames(XCoderLoganEncContext *ctx, int idx)
{
  if (free_logan_frames_isfull(ctx))
  {
    return -1;
  }
  ctx->aFree_Avframes_list[ctx->freeTail] = idx;
  ctx->freeTail = (ctx->freeTail == LOGAN_MAX_NUM_FRAMEPOOL_HWAVFRAME) ? 0 : ctx->freeTail + 1;
  return 0;
}

int recycle_logan_index_2_avframe_index(XCoderLoganEncContext *ctx, uint32_t recycleIndex)
{
  int i;
  for (i = 0; i < LOGAN_MAX_NUM_FRAMEPOOL_HWAVFRAME; i++)
  {
    if (ctx->sframe_pool[i]->data[3])
    {
      if (((ni_logan_hwframe_surface_t*)((uint8_t*)ctx->sframe_pool[i]->data[3]))->i8FrameIdx == recycleIndex)
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

// Needed for yuvbypass on FFmpeg-n4.3+
#if (LIBAVCODEC_VERSION_MAJOR >= 59 || (LIBAVCODEC_VERSION_MAJOR >= 58 && LIBAVCODEC_VERSION_MINOR >= 82))
const AVCodecHWConfigInternal *ff_ni_logan_enc_hw_configs[] = {
  HW_CONFIG_ENCODER_FRAMES(NI_LOGAN,  NI_LOGAN),
  HW_CONFIG_ENCODER_DEVICE(YUV420P, NI_LOGAN),
  HW_CONFIG_ENCODER_DEVICE(YUV420P10, NI_LOGAN),
  NULL,
};
#endif
