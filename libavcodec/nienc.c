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

#ifdef __linux__
#include <unistd.h>
#include <arpa/inet.h>
#endif
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
#include "libavutil/hwcontext_ni.h"
#include "bytestream.h"
#include "nienc.h"

#define ODD2EVEN(X) ((X&1)&&(X>31))?(X+1):(X)
#define BR_SHIFT  6
#define CPB_SHIFT 4
#ifdef _WIN32
//Just enable this macro in windows. This feature will open the device in encode init. So the first will take less time.
#define NI_ENCODER_OPEN_DEVICE 1
#endif
// NETINT_INTERNAL - currently only for internal testing
//static ni_encoder_change_params_t *g_enc_change_params = NULL;

//extern const char * const g_xcoder_preset_names[3];
//extern const char * const g_xcoder_log_names[7];

#ifdef NIENC_MULTI_THREAD
threadpool_t pool;
int sessionCounter = 0;

typedef struct _write_thread_arg_struct_t
{
  pthread_mutex_t mutex; //mutex
  pthread_cond_t cond;   //cond
  int running;
  XCoderH265EncContext *ctx;
  ni_retcode_t ret;
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

static void init_gop_param(ni_custom_gop_params_t *gopParam, ni_encoder_params_t *param)
{
  int i;
  int j;
  int gopSize;
  int gopPreset = param->hevc_enc_params.gop_preset_index;

  // GOP_PRESET_IDX_CUSTOM
  if (gopPreset == 0)
  {
    memcpy(gopParam, &param->hevc_enc_params.custom_gop_params,
           sizeof(ni_custom_gop_params_t));
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

static int check_low_delay_flag(ni_encoder_params_t *param,
                                ni_custom_gop_params_t *gopParam)
{
  int i;
  int minVal = 0;
  int low_delay = 0;
  int gopPreset = param->hevc_enc_params.gop_preset_index;

  if (gopPreset == 0) // // GOP_PRESET_IDX_CUSTOM
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

static int get_num_reorder_of_gop_structure(ni_encoder_params_t *param)
{
  int i;
  int j;
  int ret_num_reorder = 0;
  ni_custom_gop_params_t gopParam;

  init_gop_param(&gopParam, param);
  for(i = 0; i < gopParam.custom_gop_size; i++)
  {
    int check_reordering_num = 0;
    int num_reorder = 0;

    ni_gop_params_t *gopPicParam = &gopParam.pic_param[i];

    for(j = 0; j < gopParam.custom_gop_size; j++)
    {
      ni_gop_params_t *gopPicParamCand = &gopParam.pic_param[j];
      if (gopPicParamCand->poc_offset <= gopPicParam->poc_offset)
        check_reordering_num = j;
    }

    for(j = 0; j < check_reordering_num; j++)
    {
      ni_gop_params_t *gopPicParamCand = &gopParam.pic_param[j];

      if (gopPicParamCand->temporal_id <= gopPicParam->temporal_id &&
          gopPicParamCand->poc_offset > gopPicParam->poc_offset)
        num_reorder++;
    }
    ret_num_reorder = num_reorder;
  }
  return ret_num_reorder;
}

static int get_max_dec_pic_buffering_of_gop_structure(ni_encoder_params_t *param)
{
  int max_dec_pic_buffering;
  max_dec_pic_buffering = FFMIN(16/*MAX_NUM_REF*/, FFMAX(get_num_reorder_of_gop_structure(param) + 2, 6 /*maxnumreference in spec*/) + 1);
  return max_dec_pic_buffering;
}

static int get_poc_of_gop_structure(ni_encoder_params_t *param,
                                    uint32_t frame_idx)
{
  int low_delay;
  int gopSize;
  int poc;
  int gopIdx;
  int gopNum;
  ni_custom_gop_params_t gopParam;

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

static uint32_t encode_buffering_period_sei(ni_encoder_params_t *p_param,
                                            XCoderH265EncContext *ctx,
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
  init_put_bits(&pbc, p_buf, NI_MAX_SEI_DATA);

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
  put_bit_byte_size += insert_emulation_prevent_bytes(
    p_buf + 8, put_bit_byte_size - 8);

  return put_bit_byte_size;
}

static uint32_t encode_pic_timing_sei2(ni_encoder_params_t *p_param,
                                       XCoderH265EncContext *ctx,
                                       uint8_t *p_buf, int is_i_or_idr,
                                       int is_idr, uint32_t frame_idx)
{
  PutBitContext pbc;
  int32_t payload_bit_size = 0, payload_byte_size = 0, put_bit_byte_size = 0;
  uint32_t pic_dpb_output_delay = 0;
  int num_reorder_pic;
  uint64_t poc_pic;

  init_put_bits(&pbc, p_buf, NI_MAX_SEI_DATA);

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
  put_bit_byte_size += insert_emulation_prevent_bytes(
    p_buf + 8, put_bit_byte_size - 8);

  return put_bit_byte_size;
}

#define SAMPLE_SPS_MAX_SUB_LAYERS_MINUS1 0
#define MAX_VPS_MAX_SUB_LAYERS 16
#define MAX_CPB_COUNT 16
#define MAX_DURATION 0.5

static void setVui(AVCodecContext *avctx, ni_encoder_params_t *p_param,
                   XCoderH265EncContext *ctx,
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

  init_put_bits(&pbcPutBitContext, p_param->ui8VuiRbsp, NI_MAX_VUI_SIZE);
    
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

  // for YUVJ420P pix fmt, indicate it's full range video
  if (! video_full_range_flag && AV_PIX_FMT_YUVJ420P == avctx->pix_fmt)
  {
    av_log(avctx, AV_LOG_DEBUG, "setVui set video_full_range_flag for YUVJ420P "
           "pix fmt.\n");
    video_full_range_flag = 1;
  }

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
  {   // H.265 Only VUI parameters
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
  {   // H.264 Only VUI parameters
    put_bits(&pbcPutBitContext, 1, 1);  //  fixed_frame_rate_flag=1
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
    int max_num_reorder_frames = ni_get_num_reorder_of_gop_structure(p_param);
    set_ue_golomb_long(&pbcPutBitContext, max_num_reorder_frames);
    // max_dec_frame_buffering
    int num_ref_frames = ni_get_num_ref_frame_of_gop_structure(p_param);
    int max_dec_frame_buffering = (num_ref_frames > max_num_reorder_frames ?
                                   num_ref_frames : max_num_reorder_frames);
    set_ue_golomb_long(&pbcPutBitContext, max_dec_frame_buffering);
  }

  p_param->ui32VuiDataSizeBits = put_bits_count(&pbcPutBitContext);
  p_param->ui32VuiDataSizeBytes = (p_param->ui32VuiDataSizeBits + 7) / 8;
  flush_put_bits(&pbcPutBitContext);      // flush bits
}

// convert FFmpeg ROIs to NetInt ROI map
static int set_roi_map(AVCodecContext *avctx, const AVFrameSideData *sd,
                       int nb_roi, int width, int height, int intra_qp)
{
  XCoderH265EncContext *ctx = avctx->priv_data;
  int i, j, r, ctu;
  const AVRegionOfInterest *roi = (const AVRegionOfInterest*)sd->data;
  uint32_t self_size = roi->self_size;
  uint8_t set_qp = 0;
  float f_value;

  if (AV_CODEC_ID_H264 == avctx->codec_id)
  {
    // roi for H.264 is specified for 16x16 pixel macroblocks - 1 MB
    // is stored in each custom map entry.
    // number of MBs in each row/column
    uint32_t mbWidth = (width + 16 - 1) >> 4;
    uint32_t mbHeight = (height + 16 - 1) >> 4;
    uint32_t numMbs = mbWidth * mbHeight;
    uint32_t customMapSize = sizeof(ni_enc_avc_roi_custom_map_t) * numMbs;
    // make the QP map size 16-aligned to meet VPU requirement for subsequent
    // SEI due to layout of data sent to encoder
    customMapSize = ((customMapSize + 15) / 16) * 16;

    if (! ctx->avc_roi_map)
    {
      ctx->avc_roi_map = (ni_enc_avc_roi_custom_map_t*)malloc(customMapSize);
      if (! ctx->avc_roi_map)
      {
        av_log(avctx, AV_LOG_ERROR, "Error set_roi_map malloc failed.\n");
        return AVERROR(ENOMEM);
      }
    }

    // init to range midpoint
    memset(ctx->avc_roi_map, 0, customMapSize);
    for (i = 0; i < numMbs; i++)
    {
      ctx->avc_roi_map[i].field.mb_qp = NI_QP_MID_POINT;
    }

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

      f_value = roi->qoffset.num * 1.0f / roi->qoffset.den;
      f_value = av_clipf(f_value, -1.0, 1.0);
      set_qp = (int)(f_value * NI_INTRA_QP_RANGE) + NI_QP_MID_POINT;
      av_log(avctx, AV_LOG_TRACE, "set_roi_map roi %d top %d bot %d left %d "
             "right %d offset %d/%d set_qp %d\n", r, roi->top, roi->bottom,
             roi->left, roi->right, roi->qoffset.num, roi->qoffset.den, set_qp);

      // copy ROI MBs QPs into custom map
      for (j = 0; j < mbHeight; j++)
      {
        for (i = 0; i < mbWidth; i++)
        {
          if (((int)(i % mbWidth) >= (int)((roi->left + 15) / 16) - 1) &&
              ((int)(i % mbWidth) <= (int)((roi->right + 15) / 16) - 1) &&
              ((int)(j % mbHeight) >= (int)((roi->top + 15) / 16) - 1) &&
              ((int)(j % mbHeight) <= (int)((roi->bottom + 15) / 16) - 1))
          {
            ctx->avc_roi_map[i + j * mbWidth].field.mb_qp = set_qp;
          }
        }
      }
    } // for each roi

    // average qp is set to midpoint of qp range to work with qp offset
    ctx->api_ctx.roi_len = customMapSize;
    ctx->api_ctx.roi_avg_qp = NI_QP_MID_POINT;
  }
  else if (AV_CODEC_ID_HEVC == avctx->codec_id)
  {
    // ROI for H.265 is specified for 32x32 pixel subCTU blocks -
    // 4 subCTU QPs are stored in each custom CTU map entry.
    // number of CTUs/sub CTUs in each row/column
    uint32_t ctuWidth = (width + 64 - 1) >> 6;
    uint32_t ctuHeight = (height + 64 - 1) >> 6;
    uint32_t subCtuWidth = ctuWidth * 2;
    uint32_t subCtuHeight = ctuHeight * 2;
    uint32_t numSubCtus = subCtuWidth * subCtuHeight;
    uint32_t customMapSize = sizeof(ni_enc_hevc_roi_custom_map_t) *
    ctuWidth * ctuHeight;
    customMapSize = ((customMapSize + 15) / 16) * 16;

    if (! ctx->hevc_sub_ctu_roi_buf)
    {
      ctx->hevc_sub_ctu_roi_buf = (uint8_t *)malloc(numSubCtus);
      if (! ctx->hevc_sub_ctu_roi_buf)
      {
        av_log(avctx, AV_LOG_ERROR, "Error set_roi_map malloc failed.\n");
        return AVERROR(ENOMEM);
      }
    }

    if (! ctx->hevc_roi_map)
    {
      ctx->hevc_roi_map = (ni_enc_hevc_roi_custom_map_t *)malloc(customMapSize);
      if (! ctx->hevc_roi_map)
      {
        free(ctx->hevc_sub_ctu_roi_buf);
        ctx->hevc_sub_ctu_roi_buf = NULL;
        av_log(avctx, AV_LOG_ERROR, "Error set_roi_map malloc 2 failed.\n");
        return AVERROR(ENOMEM);
      }
    }

    // init to range midpoint
    memset(ctx->hevc_roi_map, 0, customMapSize);
    memset(ctx->hevc_sub_ctu_roi_buf, NI_QP_MID_POINT, numSubCtus);
    for (r = nb_roi - 1; r >= 0; r--)
    {
      roi = (const AVRegionOfInterest*)(sd->data + self_size * r);
      if (! roi->qoffset.den)
      {
        av_log(avctx, AV_LOG_ERROR, "AVRegionOfInterest.qoffset.den "
               "must not be zero.\n");
        continue;
      }

      f_value = roi->qoffset.num * 1.0f / roi->qoffset.den;
      f_value = av_clipf(f_value, -1.0, 1.0);
      set_qp = (int)(f_value * NI_INTRA_QP_RANGE) + NI_QP_MID_POINT;
      av_log(avctx, AV_LOG_TRACE, "set_roi_map roi %d top %d bot %d left %d "
             "right %d offset %d/%d set_qp %d\n", r, roi->top, roi->bottom,
             roi->left, roi->right, roi->qoffset.num, roi->qoffset.den, set_qp);

      for (j = 0; j < subCtuHeight; j++)
      {
        for (i = 0; i < subCtuWidth; i++)
        {
          if (((int)(i % subCtuWidth) >= (int)((roi->left + 31) / 32) - 1) &&
              ((int)(i % subCtuWidth) <= (int)((roi->right + 31) / 32) - 1) &&
              ((int)(j % subCtuHeight) >= (int)((roi->top + 31) / 32) - 1) &&
              ((int)(j % subCtuHeight) <= (int)((roi->bottom + 31) / 32) - 1))
          {
            ctx->hevc_sub_ctu_roi_buf[i + j * subCtuWidth] = set_qp;
          }
        }
      }
    } // for each roi

    // load into final custom map and calculate average qp
    for (i = 0; i < ctuHeight; i++)
    {
      uint8_t *ptr = &ctx->hevc_sub_ctu_roi_buf[subCtuWidth * i * 2];
      for (j = 0; j < ctuWidth; j++, ptr += 2)
      {
        ctu = i * ctuWidth + j;
        ctx->hevc_roi_map[ctu].field.sub_ctu_qp_0 = *ptr;
        ctx->hevc_roi_map[ctu].field.sub_ctu_qp_1 = *(ptr + 1);
        ctx->hevc_roi_map[ctu].field.sub_ctu_qp_2 = *(ptr + subCtuWidth);
        ctx->hevc_roi_map[ctu].field.sub_ctu_qp_3 = *(ptr + subCtuWidth + 1);
      }
    }
    // average qp is set to midpoint of qp range to work with qp offset
    ctx->api_ctx.roi_len = customMapSize;
    ctx->api_ctx.roi_avg_qp = NI_QP_MID_POINT;
  }
  return 0;
}

static int xcoder_encoder_headers(AVCodecContext *avctx)
{
  // use a copy of encoder context, take care to restore original config
  // cropping setting
  XCoderH265EncContext ctx;
  XCoderH265EncContext *s = avctx->priv_data;
  ni_encoder_params_t *p_param = &s->api_param;

  memcpy(&ctx, avctx->priv_data, sizeof(XCoderH265EncContext));

  int orig_conf_win_right = p_param->hevc_enc_params.conf_win_right;
  int orig_conf_win_bottom = p_param->hevc_enc_params.conf_win_bottom;

  int linesize_aligned = ((avctx->width + 7) / 8) * 8;
  if (avctx->codec_id == AV_CODEC_ID_H264)
  {
    linesize_aligned = ((avctx->width + 15) / 16) * 16;
  }

  if (linesize_aligned < NI_MIN_WIDTH)
  {
    p_param->hevc_enc_params.conf_win_right += NI_MIN_WIDTH - avctx->width;
    linesize_aligned = NI_MIN_WIDTH;
  }
  else if (linesize_aligned > avctx->width)
  {
    p_param->hevc_enc_params.conf_win_right += linesize_aligned - avctx->width;
  }
  p_param->source_width = linesize_aligned;

  int height_aligned = ((avctx->height + 7) / 8) * 8;
  if (avctx->codec_id == AV_CODEC_ID_H264) {
    height_aligned = ((avctx->height + 15) / 16) * 16;
  }

  if (height_aligned < NI_MIN_HEIGHT)
  {
    p_param->hevc_enc_params.conf_win_bottom += NI_MIN_HEIGHT - avctx->height;
    p_param->source_height = NI_MIN_HEIGHT;
    height_aligned = NI_MIN_HEIGHT;
  }
  else if (height_aligned > avctx->height)
  {
    p_param->hevc_enc_params.conf_win_bottom += height_aligned - avctx->height;
    p_param->source_height = height_aligned;
  }

  // set color metrics
  enum AVColorPrimaries color_primaries = avctx->color_primaries;
  enum AVColorTransferCharacteristic color_trc = avctx->color_trc;
  enum AVColorSpace color_space = avctx->colorspace;
  int video_full_range_flag = 0;

  // DolbyVision support
  if (5 == p_param->dolby_vision_profile && AV_CODEC_ID_HEVC == avctx->codec_id)
  {
    color_primaries = color_trc = color_space = 2;
    video_full_range_flag = 1;
  }

  if ((5 == p_param->dolby_vision_profile &&
       AV_CODEC_ID_HEVC == avctx->codec_id) ||
      color_primaries == AVCOL_PRI_BT2020 ||
      color_trc == AVCOL_TRC_SMPTE2084 ||
      color_trc == AVCOL_TRC_ARIB_STD_B67 ||
      color_space == AVCOL_SPC_BT2020_NCL ||
      color_space == AVCOL_SPC_BT2020_CL)
  {
    p_param->hdrEnableVUI = 1;
    setVui(avctx, p_param, &ctx,
           color_primaries, color_trc, color_space, video_full_range_flag);
    av_log(avctx, AV_LOG_VERBOSE, "XCoder HDR color info color_primaries: %d "
           "color_trc: %d  color_space %d video_full_range_flag %d sar %d/%d\n",
           color_primaries, color_trc, color_space, video_full_range_flag,
           avctx->sample_aspect_ratio.num, avctx->sample_aspect_ratio.den);
  }
  else
  {
    p_param->hdrEnableVUI = 0;
    setVui(avctx, p_param, &ctx,
           color_primaries, color_trc, color_space, video_full_range_flag);
  }

  int ret = -1;
  ctx.api_ctx.hw_id = ctx.dev_enc_idx;
  strcpy(ctx.api_ctx.dev_xcoder, ctx.dev_xcoder);
  ret = ni_device_session_open(&ctx.api_ctx, NI_DEVICE_TYPE_ENCODER);

  ctx.dev_xcoder_name = ctx.api_ctx.dev_xcoder_name;
  ctx.blk_xcoder_name = ctx.api_ctx.blk_xcoder_name;
  ctx.dev_enc_idx = ctx.api_ctx.hw_id;

  if (ret != 0)
  {
    av_log(avctx, AV_LOG_ERROR, "Failed to open encoder (status = %d), "
           "critical error or resource unavailable\n", ret);
    ret = AVERROR_EXTERNAL;

    return ret;
  }
  else
  {
    av_log(avctx, AV_LOG_VERBOSE, "XCoder %s Index %d (inst: %d) opened successfully\n",
           ctx.dev_xcoder_name, ctx.dev_enc_idx, ctx.api_ctx.session_id);
  }

  int recv;
  ni_packet_t *xpkt = &ctx.api_pkt.data.packet;
  ni_packet_buffer_alloc(xpkt, NI_MAX_TX_SZ);

  while (1)
  {
    recv = ni_device_session_read(&ctx.api_ctx, &(ctx.api_pkt),
                                  NI_DEVICE_TYPE_ENCODER);

    if (recv > 0)
    {
      free(avctx->extradata);
      avctx->extradata_size = recv - NI_FW_ENC_BITSTREAM_META_DATA_SIZE;
      avctx->extradata = av_mallocz(avctx->extradata_size +
                                    AV_INPUT_BUFFER_PADDING_SIZE);
      memcpy(avctx->extradata,
             (uint8_t*)xpkt->p_data + NI_FW_ENC_BITSTREAM_META_DATA_SIZE,
             avctx->extradata_size);

      av_log(avctx, AV_LOG_VERBOSE, "Xcoder encoder headers len: %d\n",
             avctx->extradata_size);
      break;
    }
    else if (recv == NI_RETCODE_SUCCESS)
    {
      continue;
    }
    else
    {
      av_log(avctx, AV_LOG_ERROR, "Xcoder encoder headers error: %d", recv);
      break;
    }
  }

  // close and clean up the temporary session
  ret = ni_device_session_close(&ctx.api_ctx, ctx.encoder_eof,
                                NI_DEVICE_TYPE_ENCODER);
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

  p_param->hevc_enc_params.conf_win_right = orig_conf_win_right;
  p_param->hevc_enc_params.conf_win_bottom = orig_conf_win_bottom;

  return (recv < 0 ? recv : ret);
}

static int xcoder_setup_encoder(AVCodecContext *avctx)
{
  XCoderH265EncContext *s = avctx->priv_data;
  int ret = 0;
  ni_encoder_params_t *p_param = &s->api_param;
  ni_encoder_params_t *pparams = NULL;
  ni_session_run_state_t prev_state = s->api_ctx.session_run_state;
 
  av_log(avctx, AV_LOG_DEBUG, "XCoder setup device encoder\n");
  //s->api_ctx.session_id = NI_INVALID_SESSION_ID;
  ni_device_session_context_init(&(s->api_ctx));
  s->api_ctx.session_run_state = prev_state;

  s->api_ctx.codec_format = NI_CODEC_FORMAT_H264;
  if (avctx->codec_id == AV_CODEC_ID_HEVC)
  {
    s->api_ctx.codec_format = NI_CODEC_FORMAT_H265;
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
      SESSION_RUN_STATE_SEQ_CHANGE_DRAINING != s->api_ctx.session_run_state)
  {
    av_log(avctx, AV_LOG_INFO, "Session state: %d allocate frame fifo.\n", s->api_ctx.session_run_state);
    // FIFO 4 * FPS length of frames
    s->fme_fifo_capacity = 4 * avctx->time_base.den / (avctx->time_base.num * avctx->ticks_per_frame);
    if (s->fme_fifo_capacity > 240)
	    s->fme_fifo_capacity = 240;
    s->fme_fifo = av_fifo_alloc(s->fme_fifo_capacity * sizeof(AVFrame));
  }
  else
  {
    av_log(avctx, AV_LOG_INFO, "Session seq change, fifo size: %lu.\n",
           av_fifo_size(s->fme_fifo) / sizeof(AVFrame));
  }

  if (! s->fme_fifo)
  {
    return AVERROR(ENOMEM);
  }
  s->eos_fme_received = 0;

  //Xcoder User Configuration
  ret = ni_encoder_init_default_params(p_param, avctx->time_base.den, (avctx->time_base.num * avctx->ticks_per_frame), avctx->bit_rate, ODD2EVEN(avctx->width), ODD2EVEN(avctx->height));
  if (ret == NI_RETCODE_PARAM_ERROR_WIDTH_TOO_BIG)
  {
    av_log(avctx, AV_LOG_ERROR, "Invalid Picture Width: too big\n");
    return AVERROR_EXTERNAL;
  }
  if (ret == NI_RETCODE_PARAM_ERROR_WIDTH_TOO_SMALL)
  {
    av_log(avctx, AV_LOG_ERROR, "Invalid Picture Width: too small\n");
    return AVERROR_EXTERNAL;
  }
  if (ret == NI_RETCODE_PARAM_ERROR_HEIGHT_TOO_BIG)
  {
    av_log(avctx, AV_LOG_ERROR, "Invalid Picture Height: too big\n");
    return AVERROR_EXTERNAL;
  }
  if (ret == NI_RETCODE_PARAM_ERROR_HEIGHT_TOO_SMALL)
  {
    av_log(avctx, AV_LOG_ERROR, "Invalid Picture Height: too small\n");
    return AVERROR_EXTERNAL;
  }
  if (ret == NI_RETCODE_PARAM_ERROR_AREA_TOO_BIG)
  {
    av_log(avctx, AV_LOG_ERROR, "Invalid Picture Width x Height: exceeds %d\n", NI_MAX_RESOLUTION_AREA);
    return AVERROR_EXTERNAL;
  }
  if (ret < 0)
  {
    int i;
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

  av_log(avctx, AV_LOG_DEBUG, "pix_fmt is %d, sw_pix_fmt is %d\n", avctx->pix_fmt, avctx->sw_pix_fmt);
  if (avctx->pix_fmt != AV_PIX_FMT_NI)
  {
    av_log(avctx, AV_LOG_DEBUG, "sw_pix_fmt assigned to pix_fmt was %d, is now %d\n", avctx->pix_fmt, avctx->sw_pix_fmt);
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
      av_log(avctx, AV_LOG_ERROR, "Error: pixel format %d not supported.\n", avctx->sw_pix_fmt);
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
        int parse_ret = ni_encoder_params_set_value(p_param, en->key, en->value, &s->api_ctx);
        switch (parse_ret)
        {
          case NI_RETCODE_PARAM_INVALID_NAME:
            av_log(avctx, AV_LOG_ERROR, "Unknown option: %s.\n", en->key);
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
            av_log(avctx, AV_LOG_ERROR, "Invalid value for %s: %s.\n", en->key, en->value);
            return AVERROR_EXTERNAL;
          case NI_RETCODE_PARAM_GOP_INTRA_INCOMPATIBLE:
            av_log(avctx, AV_LOG_ERROR, "Invalid value for %s: %s incompatible with GOP structure.\n", en->key, en->value);
            return AVERROR_EXTERNAL;
          case NI_RETCODE_FAILURE:
            av_log(avctx, AV_LOG_ERROR, "Generic failure during xcoder-params setting for %s\n", en->key);
            return AVERROR_EXTERNAL;
          default:
            break;
        }
      }
      av_dict_free(&dict);
      
      if (ni_encoder_params_check(p_param, s->api_ctx.codec_format) !=
          NI_RETCODE_SUCCESS)
      {
          av_log(avctx, AV_LOG_ERROR, "Validate encode parameters failed\n");
  	  return AVERROR_EXTERNAL;
      }

    }
  }

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
            av_log(avctx, AV_LOG_ERROR, "Unknown option: %s.\n", en->key);
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
            av_log(avctx, AV_LOG_ERROR, "Invalid value for GOP param %s: %s.\n", en->key, en->value);
            return AVERROR_EXTERNAL;
          case NI_RETCODE_FAILURE:
            av_log(avctx, AV_LOG_ERROR, "Generic failure during xcoder-params setting for %s\n", en->key);
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
  pparams = &s->api_param;
  switch (pparams->hevc_enc_params.gop_preset_index)
  {
    /* dtsOffset is the max number of non-reference frames in a GOP
     * (derived from x264/5 algo) In case of IBBBP the first dts of the I frame should be input_pts-(3*ticks_per_frame)
     * In case of IBP the first dts of the I frame should be input_pts-(1*ticks_per_frame)
     * thus we ensure pts>dts in all cases
     * */
    case 1 /*PRESET_IDX_ALL_I*/:
    case 2 /*PRESET_IDX_IPP*/:
    case 6 /*PRESET_IDX_IPPPP*/:
    case 9 /*PRESET_IDX_SP*/:
      s->dtsOffset = 0;
      break;
    /* ts requires dts/pts of I frame not same when there are B frames in streams */
    case 3 /*PRESET_IDX_IBBB*/:
    case 7 /*PRESET_IDX_IBBBB*/:
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

  s->total_frames_received = 0;
  s->gop_offset_count = 0;
  av_log(avctx, AV_LOG_INFO, "dts offset: %" PRId64 ", gop_offset_count: %d\n",
         s->dtsOffset, s->gop_offset_count);

  if (0 == strcmp(s->dev_xcoder, LIST_DEVICES_STR))
  {
    av_log(avctx, AV_LOG_DEBUG, "XCoder: printing out all xcoder devices and their load, and exit ...\n");
    ni_rsrc_print_all_devices_capability();
    return AVERROR_EXIT;
  }
  //overwrite the nvme io size here with a custom value if it was provided
  if (s->nvme_io_size > 0)
  {
    s->api_ctx.max_nvme_io_size = s->nvme_io_size;
    av_log(avctx, AV_LOG_VERBOSE, "Custom NVMe IO Size set to = %d\n", s->api_ctx.max_nvme_io_size);
  }

  //overwrite keep alive timeout value here with a custom value if it was provided
  s->api_ctx.keep_alive_timeout = s->keep_alive_timeout;
  av_log(avctx, AV_LOG_VERBOSE, "Custom NVMe Keep Alive Timeout set to = %d\n", s->api_ctx.keep_alive_timeout);

  s->encoder_eof = 0;
  s->roi_side_data_size = s->nb_rois = 0;
  s->av_rois = NULL;
  s->avc_roi_map = NULL;
  s->hevc_sub_ctu_roi_buf = NULL;
  s->hevc_roi_map = NULL;
  avctx->bit_rate = pparams->bitrate;

  s->api_ctx.src_bit_depth = 8;
  s->api_ctx.src_endian = NI_FRAME_LITTLE_ENDIAN;
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
      s->api_ctx.src_endian = NI_FRAME_BIG_ENDIAN;
    }
  }

  // DolbyVision, HRD and AUD settings
  if (AV_CODEC_ID_HEVC == avctx->codec_id)
  {
    if (5 == pparams->dolby_vision_profile)
    {
      pparams->hrd_enable = pparams->enable_aud = 1;
      pparams->hevc_enc_params.forced_header_enable = NI_ENC_REPEAT_HEADERS_ALL_I_FRAMES;
      pparams->hevc_enc_params.decoding_refresh_type = 2;
    }
    if (pparams->hrd_enable)
    {
      pparams->hevc_enc_params.rc.enable_rate_control = 1;
    }
  }

  // init HW AVFRAME pool
  int i;
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

  // init HDR SEI stuff
  s->api_ctx.sei_hdr_content_light_level_info_len =
  s->api_ctx.light_level_data_len =
  s->api_ctx.sei_hdr_mastering_display_color_vol_len =
  s->api_ctx.mdcv_max_min_lum_data_len = 0;
  s->api_ctx.p_master_display_meta_data = NULL;

  // init HRD SEI stuff (TBD: value after recovery ?)
  s->au_cpb_removal_delay_minus1 = 0;

  memset( &(s->api_fme), 0, sizeof(ni_session_data_io_t) );
  memset( &(s->api_pkt), 0, sizeof(ni_session_data_io_t) );

  if (avctx->width > 0 && avctx->height > 0)
  {
    ni_frame_buffer_alloc(&(s->api_fme.data.frame),
                          ODD2EVEN(avctx->width),
                          ODD2EVEN(avctx->height),
                          0,
                          0,
                          s->api_ctx.bit_depth_factor,
                          (s->buffered_fme.format == AV_PIX_FMT_NI));
  }

  // generate encoded bitstream headers in advance if configured to do so
  if (pparams->generate_enc_hdrs)
  {
    ret = xcoder_encoder_headers(avctx);
  }

  return ret;
}

#if NI_ENCODER_OPEN_DEVICE
static int xcoder_open_encoder_device(AVCodecContext *avctx)
{
  XCoderH265EncContext *ctx = avctx->priv_data;
  ni_encoder_params_t *p_param = &ctx->api_param; // NETINT_INTERNAL - currently only for internal testing

  int frame_width = 0;
  int frame_height = 0;
  frame_width = ODD2EVEN(avctx->width);
  frame_height = ODD2EVEN(avctx->height);

  // if frame stride size is not as we expect it,
  // adjust using xcoder-params conf_win_right
  int linesize_aligned = ((avctx->width + 7) / 8) * 8;
  if (avctx->codec_id == AV_CODEC_ID_H264)
  {
    linesize_aligned = ((avctx->width + 15) / 16) * 16;
  }

  if (linesize_aligned < NI_MIN_WIDTH)
  {
    p_param->hevc_enc_params.conf_win_right += NI_MIN_WIDTH - frame_width;
    linesize_aligned = NI_MIN_WIDTH;
  }
  else if (linesize_aligned > frame_width)
  {
    p_param->hevc_enc_params.conf_win_right += linesize_aligned - frame_width;
  }
  p_param->source_width = linesize_aligned;

  int height_aligned = ((frame_height + 7) / 8) * 8;
  if (avctx->codec_id == AV_CODEC_ID_H264) {
    height_aligned = ((frame_height + 15) / 16) * 16;
  }

  if (height_aligned < NI_MIN_HEIGHT)
  {
    p_param->hevc_enc_params.conf_win_bottom += NI_MIN_HEIGHT - frame_height;
    p_param->source_height = NI_MIN_HEIGHT;
    height_aligned = NI_MIN_HEIGHT;
  }
  else if (height_aligned > frame_height)
  {
    p_param->hevc_enc_params.conf_win_bottom += height_aligned - frame_height;

    p_param->source_height = height_aligned;
  }

  //Force frame color metrics if specified in command line
  enum AVColorPrimaries color_primaries;// = frame->color_primaries;
  enum AVColorTransferCharacteristic color_trc;// = frame->color_trc;
  enum AVColorSpace color_space;// = frame->colorspace;

  //if ((frame->color_primaries != avctx->color_primaries) && (avctx->color_primaries != AVCOL_PRI_UNSPECIFIED))
  {
    color_primaries = avctx->color_primaries;
  }
  //if ((frame->color_trc != avctx->color_trc) && (avctx->color_trc != AVCOL_TRC_UNSPECIFIED))
  {
    color_trc = avctx->color_trc;
  }
  //if ((frame->colorspace != avctx->colorspace) && (avctx->colorspace != AVCOL_SPC_UNSPECIFIED))
  {
    color_space = avctx->colorspace;
  }

  int video_full_range_flag = 0;

  // DolbyVision support
  if (5 == p_param->dolby_vision_profile &&
      AV_CODEC_ID_HEVC == avctx->codec_id)
  {
    color_primaries = color_trc = color_space = 2;
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
    setVui(avctx, p_param, ctx,
           color_primaries, color_trc, color_space, video_full_range_flag);
    av_log(avctx, AV_LOG_VERBOSE, "XCoder HDR color info color_primaries: %d "
           "color_trc: %d color_space %d video_full_range_flag %d sar %d/%d\n",
           color_primaries, color_trc, color_space, video_full_range_flag,
           avctx->sample_aspect_ratio.num, avctx->sample_aspect_ratio.den);
  }
  else
  {
    p_param->hdrEnableVUI = 0;
    setVui(avctx, p_param, ctx,
           color_primaries, color_trc, color_space, video_full_range_flag);
  }

  av_log(avctx, AV_LOG_VERBOSE, "XCoder frame width/height %dx%d"
         " conf_win_right %d  conf_win_bottom %d , color primaries %u trc %u "
         "space %u\n",
         avctx->width, avctx->height,
         p_param->hevc_enc_params.conf_win_right, p_param->hevc_enc_params.conf_win_bottom,
         avctx->color_primaries, avctx->color_trc, avctx->colorspace);
  int ret = -1;
  ctx->api_ctx.hw_id = ctx->dev_enc_idx;
  strcpy(ctx->api_ctx.dev_xcoder, ctx->dev_xcoder);
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
           "critical error or resource unavailable\n", ret);
    ret = AVERROR_EXTERNAL;
    // xcoder_encode_close(avctx); will be called at codec close
    return ret;
  }
  else
  {
    av_log(avctx, AV_LOG_VERBOSE, "XCoder %s Index %d (inst: %d) opened successfully\n",
           ctx->dev_xcoder_name, ctx->dev_enc_idx, ctx->api_ctx.session_id);
  }

  return ret;
}
#endif

av_cold int xcoder_encode_init(AVCodecContext *avctx)
{
  XCoderH265EncContext *ctx = avctx->priv_data;
  int ret;
  ni_log_set_level(ff_to_ni_log_level(av_log_get_level()));

  av_log(avctx, AV_LOG_DEBUG, "XCoder encode init\n");

  if (ctx->dev_xcoder == NULL)
  {
    av_log(avctx, AV_LOG_ERROR, "Error: XCoder encode options dev_xcoder is null\n");
    return AVERROR_INVALIDDATA;
  }
  else
  {
    av_log(avctx, AV_LOG_VERBOSE, "XCoder options: dev_xcoder: %s dev_enc_idx %d\n",
           ctx->dev_xcoder, ctx->dev_enc_idx);
  }

  if (ctx->api_ctx.session_run_state == SESSION_RUN_STATE_SEQ_CHANGE_DRAINING)
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

  if ((ret = xcoder_setup_encoder(avctx)) < 0)
  {
    xcoder_encode_close(avctx);
    return ret;
  }

#if NI_ENCODER_OPEN_DEVICE
  if ((ret = xcoder_open_encoder_device(avctx)) < 0)
  {
    xcoder_encode_close(avctx);
    return ret;
  }
#endif
  ctx->vpu_reset = 0;

  return 0;
}

static int is_input_fifo_empty(XCoderH265EncContext *ctx)
{
  return av_fifo_size(ctx->fme_fifo) < sizeof(AVFrame);
}

static int is_input_fifo_full(XCoderH265EncContext *ctx)
{
  return av_fifo_space(ctx->fme_fifo) < sizeof(AVFrame);
}

static int xcoder_encode_reset(AVCodecContext *avctx)
{
  XCoderH265EncContext *ctx = avctx->priv_data;
  av_log(avctx, AV_LOG_WARNING, "XCoder encode reset\n");
  ctx->vpu_reset = 1;
  xcoder_encode_close(avctx);
  return xcoder_encode_init(avctx);
}

static int enqueue_frame(AVCodecContext *avctx, const AVFrame *inframe)
{
  int ret;
  XCoderH265EncContext *ctx = avctx->priv_data;

  // expand frame buffer fifo if not enough space
  if (is_input_fifo_full(ctx))
  {
    ret = av_fifo_realloc2(ctx->fme_fifo, 2 * av_fifo_size(ctx->fme_fifo));
    if (ret < 0)
    {
      av_log(avctx, AV_LOG_ERROR, "Enc av_fifo_realloc2 NO MEMORY !!!\n");
      return ret;
    }
    if ((av_fifo_size(ctx->fme_fifo) / sizeof(AVFrame) % 100) == 0)
    {
      av_log(avctx, AV_LOG_INFO, "Enc fifo being extended to: %lu\n",
             av_fifo_size(ctx->fme_fifo) / sizeof(AVFrame));
    }
    av_assert0(0 == av_fifo_size(ctx->fme_fifo) % sizeof(AVFrame));
  }

  AVFrame *input_fme = av_frame_alloc();
  ret = av_frame_ref(input_fme, inframe);

  if (ret < 0)
  {
    av_log(avctx, AV_LOG_ERROR, "Enc av_frame_ref input_fme ERROR !!!\n");
    return ret;
  }
  av_fifo_generic_write(ctx->fme_fifo, input_fme, sizeof(*input_fme), NULL);
  av_log(avctx, AV_LOG_DEBUG, "fme queued pts:%" PRId64 ", fifo size: %lu\n",
         input_fme->pts, av_fifo_size(ctx->fme_fifo) / sizeof(AVFrame));
  return ret;
}

#ifdef NIENC_MULTI_THREAD
static void* write_frame_thread(void* arg)
{
  write_thread_arg_struct_t *args = (write_thread_arg_struct_t *) arg;
  XCoderH265EncContext *ctx = args->ctx;
  int ret;
  int sent;

  pthread_mutex_lock(&args->mutex);
  args->running = 1;
  av_log(ctx, AV_LOG_DEBUG, "write_frame_thread: session_id %d, device_handle %d\n",
         ctx->api_ctx.session_id, ctx->api_ctx.device_handle);

  av_log(ctx, AV_LOG_DEBUG, "write_frame_thread: ctx %p\n", ctx);
  
  sent = ni_device_session_write(&ctx->api_ctx, &ctx->api_fme, NI_DEVICE_TYPE_ENCODER);

  av_log(ctx, AV_LOG_DEBUG, "write_frame_thread: size %d sent to xcoder\n", sent);
  
  if (NI_RETCODE_ERROR_RESOURCE_UNAVAILABLE == sent)
  {
    av_log(ctx, AV_LOG_DEBUG, "write_frame_thread(): Sequence Change in progress, returning EAGAIN\n");
    ret = AVERROR(EAGAIN);
  }
  else if (NI_RETCODE_ERROR_VPU_RECOVERY == sent)
  {
    sent = xcoder_encode_reset(ctx);
  }

  if (sent < 0)
  {
    ret = AVERROR(EIO);
  }
  else
  {
    //pushing input pts in circular FIFO
    ctx->api_ctx.enc_pts_list[ctx->api_ctx.enc_pts_w_idx % NI_FIFO_SZ] = ctx->api_fme.data.frame.pts;
    ctx->api_ctx.enc_pts_w_idx ++;
    ret = 0;
  }

  args->ret = ret;
  av_log(ctx, AV_LOG_DEBUG, "write_frame_thread: ret %d\n", ret);
  pthread_cond_signal(&args->cond);
  args->running = 0;
  pthread_mutex_unlock(&args->mutex);
  return NULL;
}
#endif

int xcoder_encode_close(AVCodecContext *avctx)
{
  XCoderH265EncContext *ctx = avctx->priv_data;
  ni_retcode_t ret = NI_RETCODE_FAILURE;

#ifdef NIENC_MULTI_THREAD
  sessionCounter--;
  if (sessionCounter == 0)
  {
    threadpool_destroy(&pool);
  }
#endif 

  int i;
  for (i = 0; i < MAX_NUM_FRAMEPOOL_HWAVFRAME; i++)
  {
    av_frame_free(&(ctx->sframe_pool[i])); //any remaining stored AVframes that have not been unref will die here
    ctx->sframe_pool[i] = NULL;
  }

  ret = ni_device_session_close(&ctx->api_ctx, ctx->encoder_eof, NI_DEVICE_TYPE_ENCODER);
  if (NI_RETCODE_SUCCESS != ret)
  {
    av_log(avctx, AV_LOG_ERROR, "Failed to close Encoder Session (status = %d)\n", ret);
  }
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

  if (ctx->api_ctx.p_master_display_meta_data)
  {
    free(ctx->api_ctx.p_master_display_meta_data);
    ctx->api_ctx.p_master_display_meta_data = NULL;
  }

  av_log(avctx, AV_LOG_DEBUG, "XCoder encode close (status = %d)\n", ret);
  ni_frame_buffer_free( &(ctx->api_fme.data.frame) );
  ni_packet_buffer_free( &(ctx->api_pkt.data.packet) );

  av_log(avctx, AV_LOG_DEBUG, "fifo size: %lu\n", av_fifo_size(ctx->fme_fifo) / sizeof(AVFrame));
  if (! ctx->vpu_reset &&
      ctx->api_ctx.session_run_state != SESSION_RUN_STATE_SEQ_CHANGE_DRAINING)
  {
    av_fifo_free(ctx->fme_fifo);
    av_log(avctx, AV_LOG_DEBUG, " , freed.\n");
  }
  else
  {
    av_log(avctx, AV_LOG_DEBUG, " , kept.\n");
  }

  ni_rsrc_free_device_context(ctx->rsrc_ctx);
  ctx->rsrc_ctx = NULL;

  free(ctx->g_enc_change_params);
  ctx->g_enc_change_params = NULL;
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

int xcoder_send_frame(AVCodecContext *avctx, const AVFrame *frame)
{
  XCoderH265EncContext *ctx = avctx->priv_data;
  int ret = 0;
  int sent;
  int i;
  int orig_avctx_width = avctx->width, orig_avctx_height = avctx->height;
  AVFrameSideData *side_data;
  AVHWFramesContext *avhwf_ctx;
  NIFramesContext *nif_src_ctx;
  uint8_t *cc_data = NULL;
  int cc_size = 0;
  // close caption data and its size after emulation prevention processing
  uint8_t cc_data_emu_prevent[NI_MAX_SEI_DATA];
  int cc_size_emu_prevent;
  uint8_t *udu_sei = NULL;
  uint8_t udu_sei_type = 0;
  int udu_sei_size = 0;
  int ext_udu_sei_size = 0;
  bool ishwframe;  
  int format_in_use;

#ifdef NIENC_MULTI_THREAD
  av_log(avctx, AV_LOG_DEBUG, "xcoder_send_frame start 000 %p, session_id %d, device_handle %d\n",
         ctx->api_ctx.session_info, ctx->api_ctx.session_id, ctx->api_ctx.device_handle);
  if ((ctx->api_ctx.session_id != NI_INVALID_SESSION_ID) && (ctx->api_ctx.device_handle != NI_INVALID_DEVICE_HANDLE))
  {
    av_log(avctx, AV_LOG_DEBUG, "xcoder_send_frame start 000 %p\n", ctx->api_ctx.session_info);
    if (ctx->api_ctx.session_info != NULL)
    {
      write_thread_arg_struct_t *write_thread_args = (write_thread_arg_struct_t *)ctx->api_ctx.session_info;
      pthread_mutex_lock(&write_thread_args->mutex);
      av_log(avctx, AV_LOG_DEBUG, "thread start waiting session_id %d\n", ctx->api_ctx.session_id);
      if (write_thread_args->running == 1)
      {
        pthread_cond_wait(&write_thread_args->cond, &write_thread_args->mutex);
        av_log(avctx, AV_LOG_DEBUG, "thread get waiting session_id %d\n", ctx->api_ctx.session_id);
      }
      if (write_thread_args->ret == AVERROR(EAGAIN))
      {
        av_log(avctx, AV_LOG_ERROR, "xcoder_send_frame: ret %d\n", write_thread_args->ret);
        pthread_mutex_unlock(&write_thread_args->mutex);
        free(write_thread_args);
        ctx->api_ctx.session_info = NULL;
        return AVERROR(EAGAIN);
      }
      pthread_mutex_unlock(&write_thread_args->mutex);
      free(write_thread_args);
      ctx->api_ctx.session_info = NULL;
      av_log(avctx, AV_LOG_DEBUG, "thread free session_id %d\n", ctx->api_ctx.session_id);
    }
  }
#endif
  ni_encoder_params_t *p_param = &ctx->api_param; // NETINT_INTERNAL - currently only for internal testing

  av_log(avctx, AV_LOG_DEBUG, "XCoder send frame, pkt_size %d %dx%d  avctx: "
         "%dx%d\n",
         frame ? frame->pkt_size : -1, frame ? frame->width : -1,
         frame ? frame->height : -1, avctx->width, avctx->height);

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
    if (SESSION_RUN_STATE_QUEUED_FRAME_DRAINING == ctx->api_ctx.session_run_state)
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
    av_log(avctx, AV_LOG_DEBUG, "XCoder send frame #%"PRIu64"\n",
           ctx->api_ctx.frame_num);

    // queue up the frame if fifo is NOT empty, or sequence change ongoing !
    if (!is_input_fifo_empty(ctx) ||
        SESSION_RUN_STATE_SEQ_CHANGE_DRAINING == ctx->api_ctx.session_run_state)
    {
      enqueue_frame(avctx, frame);

      if (SESSION_RUN_STATE_SEQ_CHANGE_DRAINING ==
          ctx->api_ctx.session_run_state)
      {
        av_log(avctx, AV_LOG_TRACE, "XCoder doing sequence change, frame #"
               "%"PRIu64" queued and return 0 !\n", ctx->api_ctx.frame_num);
        return 0;
      }
    }
    else
    {
      ret = av_frame_ref(&ctx->buffered_fme, frame);
    }
  }

  if (is_input_fifo_empty(ctx))
  {
    av_log(avctx, AV_LOG_DEBUG,
           "no frame in fifo to send, just send/receive ..\n");
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

  int frame_width = ODD2EVEN(ctx->buffered_fme.width);
  int frame_height = ODD2EVEN(ctx->buffered_fme.height);

  // leave encoder instance open to when the first frame buffer arrives so that
  // its stride size is known and handled accordingly.
#if NI_ENCODER_OPEN_DEVICE
  if ((ctx->started == 0) &&
      ((ctx->buffered_fme.width != avctx->width) ||
       (ctx->buffered_fme.height != avctx->height) ||
       (ctx->buffered_fme.color_primaries != avctx->color_primaries) ||
       (ctx->buffered_fme.color_trc != avctx->color_trc) ||
       (ctx->buffered_fme.colorspace != avctx->colorspace)))
#else
  if (ctx->started == 0)
#endif
  {
#if NI_ENCODER_OPEN_DEVICE
    av_log(avctx, AV_LOG_INFO, "WARNING reopen device Width: %d-%d, Height: %d-%d, color_primaries: %d-%d, color_trc: %d-%d, color_space: %d-%d\n",
           ctx->buffered_fme.width, avctx->width,
           ctx->buffered_fme.height, avctx->height,
           ctx->buffered_fme.color_primaries, avctx->color_primaries,
           ctx->buffered_fme.color_trc, avctx->color_trc,
           ctx->buffered_fme.colorspace, avctx->colorspace);
    // close and clean up the temporary session
    ret = ni_device_session_close(&ctx->api_ctx, ctx->encoder_eof,
                                  NI_DEVICE_TYPE_ENCODER);
#ifdef _WIN32
    ni_device_close(ctx->api_ctx.device_handle);
#elif __linux__
    ni_device_close(ctx.api_ctx.device_handle);
    ni_device_close(ctx.api_ctx.blk_io_handle);
#endif
    ctx->api_ctx.device_handle = NI_INVALID_DEVICE_HANDLE;
    ctx->api_ctx.blk_io_handle = NI_INVALID_DEVICE_HANDLE;

    // Errror when set this parameters in ni_encoder_params_set_value !!!!!!
    p_param->hevc_enc_params.conf_win_right = 0;
    p_param->hevc_enc_params.conf_win_bottom = 0;

#endif
    // if frame stride size is not as we expect it,
    // adjust using xcoder-params conf_win_right
    int linesize_aligned = ((frame_width + 7) / 8) * 8;
    if (avctx->codec_id == AV_CODEC_ID_H264)
    {
      linesize_aligned = ((frame_width + 15) / 16) * 16;
    }

    if (linesize_aligned < NI_MIN_WIDTH)
    {
      p_param->hevc_enc_params.conf_win_right += NI_MIN_WIDTH - frame_width;
      linesize_aligned = NI_MIN_WIDTH;
    }
    else if (linesize_aligned > frame_width)
    {
      p_param->hevc_enc_params.conf_win_right += linesize_aligned - frame_width;
    }
    p_param->source_width = linesize_aligned;

    int height_aligned = ((frame_height + 7) / 8) * 8;
    if (avctx->codec_id == AV_CODEC_ID_H264)
    {
      height_aligned = ((frame_height + 15) / 16) * 16;
    }

    if (height_aligned < NI_MIN_HEIGHT)
    {
      p_param->hevc_enc_params.conf_win_bottom += NI_MIN_HEIGHT - frame_height;
      p_param->source_height = NI_MIN_HEIGHT;
      height_aligned = NI_MIN_HEIGHT;
    }
    else if (height_aligned > frame_height)
    {
      p_param->hevc_enc_params.conf_win_bottom += height_aligned - frame_height;
      p_param->source_height = height_aligned;
    }

    //Force frame color metrics if specified in command line
    enum AVColorPrimaries color_primaries = ctx->buffered_fme.color_primaries;
    enum AVColorTransferCharacteristic color_trc = ctx->buffered_fme.color_trc;
    enum AVColorSpace color_space = ctx->buffered_fme.colorspace;

    if ((ctx->buffered_fme.color_primaries != avctx->color_primaries) &&
        (avctx->color_primaries != AVCOL_PRI_UNSPECIFIED))
    {
      color_primaries = avctx->color_primaries;
    }
    if ((ctx->buffered_fme.color_trc != avctx->color_trc) &&
        (avctx->color_trc != AVCOL_TRC_UNSPECIFIED))
    {
      color_trc = avctx->color_trc;
    }
    if ((ctx->buffered_fme.colorspace != avctx->colorspace) &&
        (avctx->colorspace != AVCOL_SPC_UNSPECIFIED))
    {
      color_space = avctx->colorspace;
    }

    int video_full_range_flag = 0;

    // DolbyVision support
    if (5 == p_param->dolby_vision_profile &&
        AV_CODEC_ID_HEVC == avctx->codec_id)
    {
      color_primaries = color_trc = color_space = 2;
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
      setVui(avctx, p_param, ctx,
             color_primaries, color_trc, color_space, video_full_range_flag);
      av_log(avctx, AV_LOG_VERBOSE, "XCoder HDR color info color_primaries: %d "
             "color_trc: %d color_space %d video_full_range_flag %d sar %d/%d\n",
             color_primaries, color_trc, color_space, video_full_range_flag,
             avctx->sample_aspect_ratio.num, avctx->sample_aspect_ratio.den);
    }
    else
    {
      p_param->hdrEnableVUI = 0;
      setVui(avctx, p_param, ctx,
             color_primaries, color_trc, color_space, video_full_range_flag);
    }

    av_log(avctx, AV_LOG_VERBOSE, "XCoder buffered_fme.linesize: %d/%d/%d "
           "width/height %dx%d conf_win_right %d  conf_win_bottom %d , "
           "color primaries %u trc %u space %u\n",
           ctx->buffered_fme.linesize[0], ctx->buffered_fme.linesize[1],
           ctx->buffered_fme.linesize[2],
           ctx->buffered_fme.width, ctx->buffered_fme.height,
           p_param->hevc_enc_params.conf_win_right,
           p_param->hevc_enc_params.conf_win_bottom,
           color_primaries, color_trc, color_space);

    ctx->api_ctx.hw_id = ctx->dev_enc_idx;
    if (ctx->dev_xcoder == NULL)
    {
      av_log(avctx, AV_LOG_ERROR, "xcoder_send_frame(): XCoder encode options dev_xcoder is null\n");
      return AVERROR_EXTERNAL;
    }
    else
    {
      strcpy(ctx->api_ctx.dev_xcoder, ctx->dev_xcoder);
    }

    ishwframe = (ctx->buffered_fme.format == AV_PIX_FMT_NI);

    //p_param->rootBufId = (ishwframe) ? ((ni_hwframe_surface_t*)((uint8_t*)frame->data[3]))->ui16FrameIdx : 0;
    if (ishwframe)
    {
      ctx->api_ctx.hw_action = NI_CODEC_HW_ENABLE;
      ctx->api_ctx.sender_handle = ((ni_hwframe_surface_t*)((uint8_t*)ctx->buffered_fme.data[3]))->device_handle;
      av_log(avctx, AV_LOG_VERBOSE, "XCoder frame sender_handle:%d, hw_id:%d\n",
             ctx->api_ctx.sender_handle, ctx->api_ctx.hw_id);
    }

    if (ctx->buffered_fme.hw_frames_ctx && ctx->api_ctx.hw_id == -1)
    {     
      
      avhwf_ctx = (AVHWFramesContext*)ctx->buffered_fme.hw_frames_ctx->data;
      nif_src_ctx = avhwf_ctx->internal->priv;
      ctx->api_ctx.hw_id = nif_src_ctx->api_ctx.hw_id;
      av_log(avctx, AV_LOG_VERBOSE, "xcoder_send_frame: hw_id -1 collocated to %d \n", ctx->api_ctx.hw_id);
    }

    int ret = ni_device_session_open(&ctx->api_ctx, NI_DEVICE_TYPE_ENCODER);
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
             "critical error or resource unavailable\n", ret);
      ret = AVERROR_EXTERNAL;
      // xcoder_encode_close(avctx); will be called at codec close
      return ret;
    }
    else
    {
      av_log(avctx, AV_LOG_VERBOSE, "XCoder %s Index %d (inst: %d) opened successfully\n",
             ctx->dev_xcoder_name, ctx->dev_enc_idx, ctx->api_ctx.session_id);
    }
  }

  if (ctx->started == 0)
  {
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
    av_log(avctx, AV_LOG_INFO, "xcoder_send_frame resolution change %dx%d "
           "-> %dx%d\n", avctx->width, avctx->height,
           ctx->buffered_fme.width, ctx->buffered_fme.height);
    ctx->api_ctx.session_run_state = SESSION_RUN_STATE_SEQ_CHANGE_DRAINING;
    ctx->eos_fme_received = 1;

    // have to queue this frame if not done so: an empty queue
    if (is_input_fifo_empty(ctx))
    {
      av_log(avctx, AV_LOG_TRACE, "resolution change when fifo empty, frame "
             "#%"PRIu64" being queued ..\n", ctx->api_ctx.frame_num);
      av_frame_unref(&ctx->buffered_fme);
      enqueue_frame(avctx, frame);
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

  if (SESSION_RUN_STATE_SEQ_CHANGE_DRAINING == ctx->api_ctx.session_run_state ||
      (ctx->eos_fme_received && is_input_fifo_empty(ctx)))
  {
    av_log(avctx, AV_LOG_DEBUG, "XCoder start flushing\n");
    ctx->api_fme.data.frame.end_of_stream = 1;
    ctx->encoder_flushing = 1;
  }
  else
  {
    // NETINT_INTERNAL - currently only for internal testing
    // allocate memory for reconf parameters only once and reuse it
    if (! ctx->g_enc_change_params)
    {
      ctx->g_enc_change_params = calloc(1, sizeof(ni_encoder_change_params_t));
    }

    ctx->api_fme.data.frame.extra_data_len = NI_APP_ENC_FRAME_META_DATA_SIZE;
    ctx->g_enc_change_params->enable_option = 0;
    ctx->api_fme.data.frame.reconf_len = 0;

    switch (p_param->reconf_demo_mode)
    {
      case XCODER_TEST_RECONF_BR:
        if (ctx->api_ctx.frame_num == p_param->reconf_hash[ctx->reconfigCount][0])
        {
          ctx->g_enc_change_params->enable_option = NI_SET_CHANGE_PARAM_RC_TARGET_RATE;
          ctx->g_enc_change_params->bitRate = p_param->reconf_hash[ctx->reconfigCount][1];
          ctx->api_fme.data.frame.extra_data_len += sizeof(ni_encoder_change_params_t);
          ctx->api_fme.data.frame.reconf_len = sizeof(ni_encoder_change_params_t);
          ctx->reconfigCount ++;
        }
        break;
      case XCODER_TEST_RECONF_INTRAPRD:
        if (ctx->api_ctx.frame_num == p_param->reconf_hash[ctx->reconfigCount][0])
        {
          ctx->g_enc_change_params->enable_option = NI_SET_CHANGE_PARAM_INTRA_PARAM;
          ctx->g_enc_change_params->intraQP =
          p_param->reconf_hash[ctx->reconfigCount][1];
          ctx->g_enc_change_params->intraPeriod =
          p_param->reconf_hash[ctx->reconfigCount][2];
          ctx->g_enc_change_params->repeatHeaders =
          p_param->reconf_hash[ctx->reconfigCount][3];
          av_log(avctx, AV_LOG_TRACE, "xcoder_send_frame: frame #%lu reconf "
                 "intraQP %d intraPeriod %d repeatHeaders %d\n",
                 ctx->api_ctx.frame_num, ctx->g_enc_change_params->intraQP,
                 ctx->g_enc_change_params->intraPeriod,
                 ctx->g_enc_change_params->repeatHeaders);

          ctx->api_fme.data.frame.extra_data_len += sizeof(ni_encoder_change_params_t);
          ctx->api_fme.data.frame.reconf_len = sizeof(ni_encoder_change_params_t);
          ctx->reconfigCount ++;
        }
        break;
    case XCODER_TEST_RECONF_LONG_TERM_REF:
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
        ctx->reconfigCount ++;
      }
      break;
    case XCODER_TEST_RECONF_VUI_HRD:
      // the reconf file format for this is:
      // <frame-number>:<vui-file-name-in-digits>,<number-of-bits-of-vui-rbsp>
      if (ctx->api_ctx.frame_num == p_param->reconf_hash[ctx->reconfigCount][0])
      {
        char file_name[64];
        snprintf(file_name, 64, "%d",
                 p_param->reconf_hash[ctx->reconfigCount][1]);
        FILE *vui_file = fopen(file_name, "rb");
        if (! vui_file)
        {
          av_log(avctx, AV_LOG_ERROR, "Error VUI reconf file: %s\n", file_name);
        }
        else
        {
          int nb_bytes_by_bits =
          (p_param->reconf_hash[ctx->reconfigCount][2] + 7) / 8;
          size_t nb_bytes = fread(ctx->g_enc_change_params->vuiRbsp,
                                  1, NI_MAX_VUI_SIZE, vui_file);
          if (nb_bytes != nb_bytes_by_bits)
          {
            av_log(avctx, AV_LOG_ERROR, "Error VUI file size %d bytes != "
                   "specified %d bits (%d bytes) !\n", (int)nb_bytes,
                   p_param->reconf_hash[ctx->reconfigCount][2], nb_bytes_by_bits);
          }
          else
          {
            ctx->g_enc_change_params->enable_option =
            NI_SET_CHANGE_PARAM_VUI_HRD_PARAM;
            ctx->g_enc_change_params->encodeVuiRbsp = 1;
            ctx->g_enc_change_params->vuiDataSizeBits =
            p_param->reconf_hash[ctx->reconfigCount][2];
            ctx->g_enc_change_params->vuiDataSizeBytes = nb_bytes;
            av_log(avctx, AV_LOG_DEBUG, "Reconf VUI %d bytes (%d bits)\n",
                   (int)nb_bytes, p_param->reconf_hash[ctx->reconfigCount][2]);
            ctx->api_fme.data.frame.extra_data_len += sizeof(ni_encoder_change_params_t);
            ctx->api_fme.data.frame.reconf_len = sizeof(ni_encoder_change_params_t);
            ctx->reconfigCount++;
          }

          fclose(vui_file);
        }
      }
      break;
    case XCODER_TEST_RECONF_RC:
      if (ctx->api_ctx.frame_num == p_param->reconf_hash[ctx->reconfigCount][0])
      {
        ctx->g_enc_change_params->enable_option = NI_SET_CHANGE_PARAM_RC;
        ctx->g_enc_change_params->hvsQPEnable =
        p_param->reconf_hash[ctx->reconfigCount][1];
        ctx->g_enc_change_params->hvsQpScale =
        p_param->reconf_hash[ctx->reconfigCount][2];
        ctx->g_enc_change_params->vbvBufferSize =
        p_param->reconf_hash[ctx->reconfigCount][3];
        ctx->g_enc_change_params->mbLevelRcEnable =
        p_param->reconf_hash[ctx->reconfigCount][4];
        ctx->g_enc_change_params->fillerEnable =
        p_param->reconf_hash[ctx->reconfigCount][5];
        av_log(avctx, AV_LOG_TRACE, "xcoder_send_frame: frame #%lu reconf "
               "hvsQPEnable %d hvsQpScale %d vbvBufferSize %d mbLevelRcEnable "
               "%d fillerEnable %d\n",
               ctx->api_ctx.frame_num, ctx->g_enc_change_params->hvsQPEnable,
               ctx->g_enc_change_params->hvsQpScale,
               ctx->g_enc_change_params->vbvBufferSize,
               ctx->g_enc_change_params->mbLevelRcEnable,
               ctx->g_enc_change_params->fillerEnable);

        ctx->api_fme.data.frame.extra_data_len +=
        sizeof(ni_encoder_change_params_t);
        ctx->api_fme.data.frame.reconf_len = sizeof(ni_encoder_change_params_t);
        ctx->reconfigCount ++;
      }
      break;
    case XCODER_TEST_RECONF_RC_MIN_MAX_QP:
      if (ctx->api_ctx.frame_num == p_param->reconf_hash[ctx->reconfigCount][0])
      {
        ctx->g_enc_change_params->enable_option = NI_SET_CHANGE_PARAM_RC_MIN_MAX_QP;
        ctx->g_enc_change_params->minQpI =
        p_param->reconf_hash[ctx->reconfigCount][1];
        ctx->g_enc_change_params->maxQpI =
        p_param->reconf_hash[ctx->reconfigCount][2];
        ctx->g_enc_change_params->maxDeltaQp =
        p_param->reconf_hash[ctx->reconfigCount][3];
        ctx->g_enc_change_params->minQpP =
        p_param->reconf_hash[ctx->reconfigCount][4];
        ctx->g_enc_change_params->minQpB =
        p_param->reconf_hash[ctx->reconfigCount][5];
        ctx->g_enc_change_params->maxQpP =
        p_param->reconf_hash[ctx->reconfigCount][6];
        ctx->g_enc_change_params->maxQpB =
        p_param->reconf_hash[ctx->reconfigCount][7];
        av_log(avctx, AV_LOG_TRACE, "xcoder_send_frame: frame #%lu reconf "
               "minQpI %d maxQpI %d maxDeltaQp %d minQpP "
               "%d minQpB %d maxQpP %d maxQpB %d\n",
               ctx->api_ctx.frame_num, ctx->g_enc_change_params->minQpI,
               ctx->g_enc_change_params->maxQpI,
               ctx->g_enc_change_params->maxDeltaQp,
               ctx->g_enc_change_params->minQpP,
               ctx->g_enc_change_params->minQpB,
               ctx->g_enc_change_params->maxQpP,
               ctx->g_enc_change_params->maxQpB);

        ctx->api_fme.data.frame.extra_data_len +=
        sizeof(ni_encoder_change_params_t);
        ctx->api_fme.data.frame.reconf_len = sizeof(ni_encoder_change_params_t);
        ctx->reconfigCount ++;
      }
      break;
      case XCODER_TEST_RECONF_OFF:
      default:
        ;
    }

    // NetInt long term reference frame support
    side_data = av_frame_get_side_data(&ctx->buffered_fme,
                                       AV_FRAME_DATA_NETINT_LONG_TERM_REF);
    if (side_data && (side_data->size == sizeof(AVNetintLongTermRef)))
    {
      AVNetintLongTermRef *ltr = (AVNetintLongTermRef*)side_data->data;

      ctx->api_fme.data.frame.use_cur_src_as_long_term_pic
      = ltr->use_cur_src_as_long_term_pic;
      ctx->api_fme.data.frame.use_long_term_ref
      = ltr->use_long_term_ref;
    }

    // NetInt target bitrate reconfiguration support
    side_data = av_frame_get_side_data(&ctx->buffered_fme,
                                       AV_FRAME_DATA_NETINT_BITRATE);
    if (side_data && (side_data->size == sizeof(int32_t)))
    {
      int32_t bitrate = *((int32_t *)side_data->data);
      ctx->g_enc_change_params->enable_option |= NI_SET_CHANGE_PARAM_RC_TARGET_RATE;
      ctx->g_enc_change_params->bitRate = bitrate;
      if (ctx->api_fme.data.frame.reconf_len == 0)
      {
        ctx->api_fme.data.frame.extra_data_len += sizeof(ni_encoder_change_params_t);
        ctx->api_fme.data.frame.reconf_len = sizeof(ni_encoder_change_params_t);
        ctx->reconfigCount++;
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
      // size of: start code + NAL unit header + payload type byte +
      //          payload size byte + payload + rbsp trailing bits, default HEVC
      ctx->api_ctx.light_level_data_len = 4;
      ctx->api_ctx.sei_hdr_content_light_level_info_len = 8 + 4 + 1;
      if (AV_CODEC_ID_H264 == avctx->codec_id)
      {
        ctx->api_ctx.sei_hdr_content_light_level_info_len--;
      }

      uint16_t max_content_light_level =
      htons(((AVContentLightMetadata *)hdr_side_data->data)->MaxCLL);
      uint16_t max_pic_average_light_level =
      htons(((AVContentLightMetadata *)hdr_side_data->data)->MaxFALL);
      
      av_log(avctx, AV_LOG_TRACE, "content light level info, MaxCLL %u "
             "MaxFALL %u\n", 
             ((AVContentLightMetadata *)hdr_side_data->data)->MaxCLL,
             ((AVContentLightMetadata *)hdr_side_data->data)->MaxFALL);
      memcpy(ctx->api_ctx.ui8_light_level_data,
             &max_content_light_level, sizeof(uint16_t));
      memcpy(&ctx->api_ctx.ui8_light_level_data[2],
             &max_pic_average_light_level,
             sizeof(uint16_t));

      if (AV_RB24(ctx->api_ctx.ui8_light_level_data) == 0 ||
          AV_RB24(ctx->api_ctx.ui8_light_level_data) == 1 ||
          AV_RB24(ctx->api_ctx.ui8_light_level_data) == 2 ||
          (AV_RB24(ctx->api_ctx.ui8_light_level_data) == 3 &&
           ctx->api_ctx.ui8_light_level_data[3] != 0 &&
           ctx->api_ctx.ui8_light_level_data[3] != 1 &&
           ctx->api_ctx.ui8_light_level_data[3] != 2 &&
           ctx->api_ctx.ui8_light_level_data[3] != 3))
      {
        ctx->api_ctx.sei_hdr_content_light_level_info_len++;
        ctx->api_ctx.light_level_data_len++;
        memmove(&ctx->api_ctx.ui8_light_level_data[3],
                &ctx->api_ctx.ui8_light_level_data[2], 2);
        ctx->api_ctx.ui8_light_level_data[2] = 0x3;
      }
    }

    // mastering display color volume
    hdr_side_data = av_frame_get_side_data(
      &ctx->buffered_fme, AV_FRAME_DATA_MASTERING_DISPLAY_METADATA);
    if (hdr_side_data &&
        hdr_side_data->size == sizeof(AVMasteringDisplayMetadata))
    {
      uint8_t *p_temp = NULL;
      ctx->api_ctx.mdcv_max_min_lum_data_len = 8;
      ctx->api_ctx.sei_hdr_mastering_display_color_vol_len = 8 + 6*2 + 2*2 + 2*4 + 1;
      if (AV_CODEC_ID_H264 == avctx->codec_id)
      {
        ctx->api_ctx.sei_hdr_mastering_display_color_vol_len--;
      }

      AVMasteringDisplayMetadata *p_src =
      (AVMasteringDisplayMetadata*)hdr_side_data->data;
      // save a copy
      if (ctx->api_ctx.p_master_display_meta_data)
      {
        free(ctx->api_ctx.p_master_display_meta_data);
        ctx->api_ctx.p_master_display_meta_data = NULL;
      }
      ctx->api_ctx.p_master_display_meta_data =
      malloc(sizeof(AVMasteringDisplayMetadata));
      if (! ctx->api_ctx.p_master_display_meta_data)
      {
        return AVERROR(ENOMEM);
      }
      memcpy(ctx->api_ctx.p_master_display_meta_data, p_src,
             sizeof(AVMasteringDisplayMetadata));

      const int luma_den = 10000;
      uint32_t uint32_t_tmp = htonl((uint32_t)(lrint(luma_den * av_q2d(p_src->max_luminance))));
      memcpy(ctx->api_ctx.ui8_mdcv_max_min_lum_data,
             &uint32_t_tmp, sizeof(uint32_t));
      
      uint32_t_tmp = htonl((uint32_t)(lrint(luma_den * av_q2d(p_src->min_luminance))));
      memcpy(&ctx->api_ctx.ui8_mdcv_max_min_lum_data[4],
             &uint32_t_tmp, sizeof(uint32_t));

      p_temp = (uint8_t *)&uint32_t_tmp; // cppcheck-suppress objectIndex
      if (AV_RB24(p_temp) == 0 ||// cppcheck-suppress objectIndex
          AV_RB24(p_temp) == 1 ||
          AV_RB24(p_temp) == 2 ||
          (AV_RB24(p_temp) == 3 &&
           ctx->api_ctx.ui8_mdcv_max_min_lum_data[7] != 0 &&
           ctx->api_ctx.ui8_mdcv_max_min_lum_data[7] != 1 &&
           ctx->api_ctx.ui8_mdcv_max_min_lum_data[7] != 2 &&
           ctx->api_ctx.ui8_mdcv_max_min_lum_data[7] != 3))
      {
        ctx->api_ctx.sei_hdr_mastering_display_color_vol_len++;
        ctx->api_ctx.mdcv_max_min_lum_data_len++;
        if (ctx->api_ctx.ui8_mdcv_max_min_lum_data[3] == 0)
        {
          memmove(&ctx->api_ctx.ui8_mdcv_max_min_lum_data[5],
                  &ctx->api_ctx.ui8_mdcv_max_min_lum_data[4],
                  sizeof(uint32_t));
          ctx->api_ctx.ui8_mdcv_max_min_lum_data[5] = 0x3;
        }
        else
        {
          memmove(&ctx->api_ctx.ui8_mdcv_max_min_lum_data[7],
                  &ctx->api_ctx.ui8_mdcv_max_min_lum_data[6], 2);
          ctx->api_ctx.ui8_mdcv_max_min_lum_data[6] = 0x3;
        }
      }
    }

    // SEI (HDR10+)
    uint8_t hdr10p_buf[NI_MAX_SEI_DATA];
    int hdr10p_num_bytes = 0;
    int hdr10p_num_bytes_nal_payload = 0;
    AVFrameSideData *s_data = av_frame_get_side_data(
      &ctx->buffered_fme, AV_FRAME_DATA_DYNAMIC_HDR_PLUS);
    if (s_data && s_data->size == sizeof(AVDynamicHDRPlus))
    {
      AVDynamicHDRPlus *hdrp = (AVDynamicHDRPlus *)s_data->data;
      int w, i, j;
      PutBitContext pb;
      uint32_t ui_tmp;
      init_put_bits(&pb, hdr10p_buf, NI_MAX_SEI_DATA);

      // HDR10+ SEI header bytes
      // itu_t_t35_provider_code and itu_t_t35_provider_oriented_code are
      // contained in the first 4 bytes of payload; pb has all the data until
      // start of trailer
      put_bits(&pb, 8, 0);
      put_bits(&pb, 8, 0x3c); // u16 itu_t_t35_provider_code = 0x003c
      put_bits(&pb, 8, 0);
      put_bits(&pb, 8, 0x01); // u16 itu_t_t35_provider_oriented_code = 0x0001
      put_bits(&pb, 8, 4); // u8 application_identifier = 0x04
      put_bits(&pb, 8, 0); // u8 application version = 0x00
      put_bits(&pb, 2, hdrp->num_windows);
      av_log(avctx, AV_LOG_TRACE, "hdr10+ num_windows %u\n", hdrp->num_windows);
      for (w = 1; w < hdrp->num_windows; w++)
      {
        put_bits(&pb, 16, hdrp->params[w - 1].window_upper_left_corner_x.num);
        put_bits(&pb, 16, hdrp->params[w - 1].window_upper_left_corner_y.num);
        put_bits(&pb, 16, hdrp->params[w - 1].window_lower_right_corner_x.num);
        put_bits(&pb, 16, hdrp->params[w - 1].window_lower_right_corner_y.num);
        put_bits(&pb, 16, hdrp->params[w - 1].center_of_ellipse_x);
        put_bits(&pb, 16, hdrp->params[w - 1].center_of_ellipse_y);
        put_bits(&pb, 8, hdrp->params[w - 1].rotation_angle);
        put_bits(&pb, 16, hdrp->params[w - 1].semimajor_axis_internal_ellipse);
        put_bits(&pb, 16, hdrp->params[w - 1].semimajor_axis_external_ellipse);
        put_bits(&pb, 16, hdrp->params[w - 1].semiminor_axis_external_ellipse);
        put_bits(&pb, 1, hdrp->params[w - 1].overlap_process_option);
      }

      // values are scaled up according to standard spec
      ui_tmp = lrint(10000 * av_q2d(hdrp->targeted_system_display_maximum_luminance));
      put_bits(&pb, 27, ui_tmp);
      put_bits(&pb, 1, hdrp->targeted_system_display_actual_peak_luminance_flag);
      av_log(avctx, AV_LOG_TRACE, "hdr10+ targeted_system_display_maximum_luminance %d\n", ui_tmp);
      av_log(avctx, AV_LOG_TRACE, "hdr10+ targeted_system_display_actual_peak_luminance_flag %u\n",
             hdrp->targeted_system_display_actual_peak_luminance_flag);

      if (hdrp->targeted_system_display_actual_peak_luminance_flag)
      {
        put_bits(&pb, 5,
                 hdrp->num_rows_targeted_system_display_actual_peak_luminance);
        put_bits(&pb, 5,
                 hdrp->num_cols_targeted_system_display_actual_peak_luminance);
        av_log(avctx, AV_LOG_TRACE, "hdr10+ num_rows_targeted_system_display_actual_peak_luminance x num_cols_targeted_system_display_actual_peak_luminance %u x %u\n", 
               hdrp->num_rows_targeted_system_display_actual_peak_luminance,
               hdrp->num_cols_targeted_system_display_actual_peak_luminance);

        for (i = 0; i < hdrp->num_rows_targeted_system_display_actual_peak_luminance; i++)
        {
          for (j = 0; j < hdrp->num_cols_targeted_system_display_actual_peak_luminance; j++)
          {
            ui_tmp = lrint(15 * av_q2d(hdrp->targeted_system_display_actual_peak_luminance[i][j]));
            put_bits(&pb, 4, ui_tmp);
            av_log(avctx, AV_LOG_TRACE, "hdr10+ targeted_system_display_actual_peak_luminance[%d][%d] %d\n", i, j, ui_tmp);
          }
        }
      }

      for (w = 0; w < hdrp->num_windows; w++)
      {
        for (i = 0; i < 3; i++)
        {
          ui_tmp = lrint(100000 * av_q2d(hdrp->params[w].maxscl[i]));
          put_bits(&pb, 17, ui_tmp);
          av_log(avctx, AV_LOG_TRACE, "hdr10+ maxscl[%d][%d] %d\n", w, i,
                 ui_tmp);
        }
        ui_tmp = lrint(100000 * av_q2d(hdrp->params[w].average_maxrgb));
        put_bits(&pb, 17, ui_tmp);
        av_log(avctx, AV_LOG_TRACE, "hdr10+ average_maxrgb[%d] %d\n",
               w, ui_tmp);

        put_bits(&pb, 4, hdrp->params[w].num_distribution_maxrgb_percentiles);
        av_log(avctx, AV_LOG_TRACE,
               "hdr10+ num_distribution_maxrgb_percentiles[%d] %d\n",
               w, hdrp->params[w].num_distribution_maxrgb_percentiles);

        for (i = 0; i < hdrp->params[w].num_distribution_maxrgb_percentiles; i++)
        {
          put_bits(&pb, 7, hdrp->params[w].distribution_maxrgb[i].percentage);
          ui_tmp = lrint(100000 * av_q2d(hdrp->params[w].distribution_maxrgb[i].percentile));
          put_bits(&pb, 17, ui_tmp);
          av_log(avctx, AV_LOG_TRACE, "hdr10+ distribution_maxrgb_percentage[%d][%d] %u\n",
                 w, i, hdrp->params[w].distribution_maxrgb[i].percentage);
          av_log(avctx, AV_LOG_TRACE, "hdr10+ distribution_maxrgb_percentile[%d][%d] %d\n",
                 w, i, ui_tmp);
        }

        ui_tmp = lrint(1000 * av_q2d(hdrp->params[w].fraction_bright_pixels));
        put_bits(&pb, 10, ui_tmp);
        av_log(avctx, AV_LOG_TRACE, "hdr10+ fraction_bright_pixels[%d] %d\n",
               w, ui_tmp);
      }

      put_bits(&pb, 1, hdrp->mastering_display_actual_peak_luminance_flag);
      av_log(avctx, AV_LOG_TRACE, 
             "hdr10+ mastering_display_actual_peak_luminance_flag %u\n",
             hdrp->mastering_display_actual_peak_luminance_flag);
      if (hdrp->mastering_display_actual_peak_luminance_flag)
      {
        put_bits(&pb, 5, hdrp->num_rows_mastering_display_actual_peak_luminance);
        put_bits(&pb, 5, hdrp->num_cols_mastering_display_actual_peak_luminance);
        av_log(avctx, AV_LOG_TRACE, "hdr10+ num_rows_mastering_display_actual_peak_luminance x num_cols_mastering_display_actual_peak_luminance %u x %u\n",
               hdrp->num_rows_mastering_display_actual_peak_luminance,
               hdrp->num_cols_mastering_display_actual_peak_luminance);

        for (i = 0; i < hdrp->num_rows_mastering_display_actual_peak_luminance; i++)
        {
          for (j = 0; j < hdrp->num_cols_mastering_display_actual_peak_luminance; j++)
          {
            ui_tmp = lrint(15 * av_q2d(hdrp->mastering_display_actual_peak_luminance[i][j]));
            put_bits(&pb, 4, ui_tmp);
            av_log(avctx, AV_LOG_TRACE, "hdr10+ mastering_display_actual_peak_luminance[%d][%d] %d\n", i, j, ui_tmp);
          }
        }
      }

      for (w = 0; w < hdrp->num_windows; w++)
      {
        put_bits(&pb, 1, hdrp->params[w].tone_mapping_flag);
        av_log(avctx, AV_LOG_TRACE, "hdr10+ tone_mapping_flag[%d] %u\n",
               w, hdrp->params[w].tone_mapping_flag);

        if (hdrp->params[w].tone_mapping_flag)
        {
          ui_tmp = lrint(4095 * av_q2d(hdrp->params[w].knee_point_x));
          put_bits(&pb, 12, ui_tmp);
          av_log(avctx, AV_LOG_TRACE, "hdr10+ knee_point_x[%d] %u\n",
                 w, ui_tmp);

          ui_tmp = lrint(4095 * av_q2d(hdrp->params[w].knee_point_y));
          put_bits(&pb, 12, ui_tmp);
          av_log(avctx, AV_LOG_TRACE, "hdr10+ knee_point_y[%d] %u\n",
                 w, ui_tmp);

          put_bits(&pb, 4, hdrp->params[w].num_bezier_curve_anchors);
          av_log(avctx, AV_LOG_TRACE,
                 "hdr10+ num_bezier_curve_anchors[%d] %u\n",
                 w, hdrp->params[w].num_bezier_curve_anchors);
          for (i = 0; i < hdrp->params[w].num_bezier_curve_anchors; i++)
          {
            ui_tmp = lrint(1023 * av_q2d(hdrp->params[w].bezier_curve_anchors[i]));
            put_bits(&pb, 10, ui_tmp);
            av_log(avctx, AV_LOG_TRACE,
                   "hdr10+ bezier_curve_anchors[%d][%d] %d\n", w, i, ui_tmp);
          }
        }

        put_bits(&pb, 1, hdrp->params[w].color_saturation_mapping_flag);
        av_log(avctx, AV_LOG_TRACE, 
               "hdr10+ color_saturation_mapping_flag[%d] %u\n",
               w, hdrp->params[w].color_saturation_mapping_flag);
        if (hdrp->params[w].color_saturation_mapping_flag)
        {
          ui_tmp = lrint(8 * av_q2d(hdrp->params[w].color_saturation_weight));
          put_bits(&pb, 6, ui_tmp);
          av_log(avctx, AV_LOG_TRACE, "hdr10+ color_saturation_weight[%d] %d\n",
                 w, ui_tmp);
        }
      } // num_windows

      hdr10p_num_bytes_nal_payload = hdr10p_num_bytes =
      (put_bits_count(&pb) + 7) / 8;
      av_log(avctx, AV_LOG_TRACE, "hdr10+ total bits: %d -> bytes %d\n",
             put_bits_count(&pb), hdr10p_num_bytes);
      flush_put_bits(&pb);

      hdr10p_num_bytes += insert_emulation_prevent_bytes(
        hdr10p_buf, hdr10p_num_bytes);

      // set header info fields and extra size based on codec
      if (AV_CODEC_ID_HEVC == avctx->codec_id)
      {
        ctx->api_fme.data.frame.sei_hdr_plus_len = NI_HDR10P_SEI_HDR_HEVC_LEN +
        hdr10p_num_bytes + NI_RBSP_TRAILING_BITS_LEN;
        ctx->api_fme.data.frame.sei_total_len +=
        ctx->api_fme.data.frame.sei_hdr_plus_len;
        ctx->api_ctx.itu_t_t35_hdr10p_sei_hdr_hevc[7] =
        hdr10p_num_bytes_nal_payload + NI_RBSP_TRAILING_BITS_LEN;
      }
      else if (AV_CODEC_ID_H264 == avctx->codec_id)
      {
        ctx->api_fme.data.frame.sei_hdr_plus_len = NI_HDR10P_SEI_HDR_H264_LEN +
        hdr10p_num_bytes + NI_RBSP_TRAILING_BITS_LEN;
        ctx->api_fme.data.frame.sei_total_len +=
        ctx->api_fme.data.frame.sei_hdr_plus_len;
        ctx->api_ctx.itu_t_t35_hdr10p_sei_hdr_h264[6] =
        hdr10p_num_bytes_nal_payload + NI_RBSP_TRAILING_BITS_LEN;
      }
      else
      {
        av_log(avctx, AV_LOG_ERROR, "xcoder_send_frame: codec %d not "
               "supported for SEI !\n", avctx->codec_id);
        ctx->api_fme.data.frame.sei_hdr_plus_len = 0;
      }
    } // hdr10+

    // SEI (close caption)
    side_data = av_frame_get_side_data(
      &ctx->buffered_fme, AV_FRAME_DATA_A53_CC);
    if (side_data && side_data->size > 0)
    {
      cc_data = side_data->data;
      cc_size = side_data->size;

      memcpy(cc_data_emu_prevent, cc_data, cc_size);
      cc_size_emu_prevent = cc_size + insert_emulation_prevent_bytes(
        cc_data_emu_prevent, cc_size);

      if (cc_size_emu_prevent != cc_size)
      {
        av_log(avctx, AV_LOG_TRACE, "xcoder_send_frame: close caption "
               "emulation prevention bytes added: %d\n",
               cc_size_emu_prevent - cc_size);
      }

      // set header info fields and extra size based on codec
      if (AV_CODEC_ID_HEVC == avctx->codec_id)
      {
        ctx->api_fme.data.frame.sei_cc_len =
        NI_CC_SEI_HDR_HEVC_LEN + cc_size_emu_prevent + NI_CC_SEI_TRAILER_LEN;
        ctx->api_fme.data.frame.sei_total_len +=
        ctx->api_fme.data.frame.sei_cc_len;
        ctx->api_ctx.itu_t_t35_cc_sei_hdr_hevc[7] = cc_size + 11;
        ctx->api_ctx.itu_t_t35_cc_sei_hdr_hevc[16] = (cc_size / 3) | 0xc0;
      }
      else if (AV_CODEC_ID_H264 == avctx->codec_id)
      {
        ctx->api_fme.data.frame.sei_cc_len =
        NI_CC_SEI_HDR_H264_LEN + cc_size_emu_prevent + NI_CC_SEI_TRAILER_LEN;
        ctx->api_fme.data.frame.sei_total_len +=
        ctx->api_fme.data.frame.sei_cc_len;
        ctx->api_ctx.itu_t_t35_cc_sei_hdr_h264[6] = cc_size + 11;
        ctx->api_ctx.itu_t_t35_cc_sei_hdr_h264[15] = (cc_size / 3) | 0xc0;
      }
      else
      {
        av_log(avctx, AV_LOG_ERROR, "xcoder_send_frame: codec %d not "
               "supported for SEI !\n", avctx->codec_id);
        cc_data = NULL;
        cc_size = 0;
      }
    }

    // supply QP map if ROI enabled and if ROIs passed in
    const AVFrameSideData *p_sd = av_frame_get_side_data(
      &ctx->buffered_fme, AV_FRAME_DATA_REGIONS_OF_INTEREST);
    if (p_param->hevc_enc_params.roi_enable && p_sd)
    {
      int is_new_rois = 1;
      const AVRegionOfInterest *roi = NULL;
      uint32_t self_size = 0;
      roi = (const AVRegionOfInterest*)p_sd->data;
      self_size = roi->self_size;
      if (! self_size || p_sd->size % self_size)
      {
        av_log(avctx, AV_LOG_ERROR, "Invalid AVRegionOfInterest.self_size, "
               "sd size %d self_size %u\n", p_sd->size, self_size);
        return AVERROR(EINVAL);
      }

      int nb_roi = p_sd->size / self_size;
      // update ROI(s) if new/different from last one
      if (0 == ctx->nb_rois || 0 == ctx->roi_side_data_size || ! ctx->av_rois ||
          ctx->nb_rois != nb_roi || ctx->roi_side_data_size != p_sd->size ||
          memcmp(ctx->av_rois, p_sd->data, p_sd->size))
      {
        ctx->roi_side_data_size = p_sd->size;
        ctx->nb_rois = nb_roi;
        free(ctx->av_rois);
        ctx->av_rois = malloc(p_sd->size);
        if (! ctx->av_rois)
        {
          av_log(avctx, AV_LOG_ERROR, "malloc ROI sidedata failed.\n");
          return AVERROR(ENOMEM);
        }
        memcpy(ctx->av_rois, p_sd->data, p_sd->size);
      }
      else
      {
        is_new_rois = 0;
      }

      if (is_new_rois)
      {
        if (set_roi_map(avctx, p_sd, nb_roi, p_param->source_width,
                        p_param->source_height,
                        p_param->hevc_enc_params.rc.intra_qp))
        {
          av_log(avctx, AV_LOG_ERROR, "set_roi_map failed\n");
        }
      }
      // ROI data in the frame
      ctx->api_fme.data.frame.extra_data_len += ctx->api_ctx.roi_len;
      ctx->api_fme.data.frame.roi_len = ctx->api_ctx.roi_len;
    }

    // if ROI cache is enabled, supply cached QP map if no ROI sidedata is
    // passed in with this frame
    if (p_param->hevc_enc_params.roi_enable && ! p_sd && p_param->cacheRoi)
    {
      ctx->api_fme.data.frame.extra_data_len += ctx->api_ctx.roi_len;
      ctx->api_fme.data.frame.roi_len = ctx->api_ctx.roi_len;

      av_log(avctx, AV_LOG_TRACE, "xcoder_send_frame: supply cached QP map.\n");
    }

    ctx->api_fme.data.frame.pts = ctx->buffered_fme.pts;
    ctx->api_fme.data.frame.dts = ctx->buffered_fme.pkt_dts;
    ctx->api_fme.data.frame.video_width = ODD2EVEN(avctx->width);
    ctx->api_fme.data.frame.video_height = ODD2EVEN(avctx->height);
    ctx->api_fme.data.frame.ni_pict_type = 0;

    if (ctx->api_ctx.force_frame_type)
    {
      switch (ctx->buffered_fme.pict_type)
      {
        case AV_PICTURE_TYPE_I:
          ctx->api_fme.data.frame.ni_pict_type = PIC_TYPE_FORCE_IDR;
          break;
        case AV_PICTURE_TYPE_P:
          ctx->api_fme.data.frame.ni_pict_type = PIC_TYPE_P;
          break;
        default:
          ;
      }
    }
    else if (AV_PICTURE_TYPE_I == ctx->buffered_fme.pict_type)
    {
      ctx->api_fme.data.frame.force_key_frame = 1;
      ctx->api_fme.data.frame.ni_pict_type = PIC_TYPE_FORCE_IDR;
    }

    av_log(avctx, AV_LOG_TRACE, "xcoder_send_frame: #%"PRIu64" ni_pict_type %d"
           " forced_header_enable %d intraPeriod %d\n", ctx->api_ctx.frame_num,
           ctx->api_fme.data.frame.ni_pict_type,
           p_param->hevc_enc_params.forced_header_enable,
           p_param->hevc_enc_params.intra_period);

    // send HDR SEI when:
    // - repeatHeaders = 0. Insert on first frame only (IDR)
    // - repeatHeaders = 1. Insert on every I-frame including I-frames
    //   generated on the intraPeriod interval as well as I-frames that are
    //   forced.
    int send_sei_with_idr = 0;
    if (0 == ctx->api_ctx.frame_num ||
        PIC_TYPE_FORCE_IDR == ctx->api_fme.data.frame.ni_pict_type ||
        (p_param->hevc_enc_params.forced_header_enable &&
         p_param->hevc_enc_params.intra_period &&
         0 == (ctx->api_ctx.frame_num % p_param->hevc_enc_params.intra_period)))
    {
      send_sei_with_idr = 1;
    }

    // DolbyVision (HRD SEI), HEVC only for now
    uint8_t hrd_buf[NI_MAX_SEI_DATA];
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

    // HDR SEI
    if (ctx->api_ctx.sei_hdr_content_light_level_info_len && send_sei_with_idr)
    {
      ctx->api_fme.data.frame.sei_hdr_content_light_level_info_len =
      ctx->api_ctx.sei_hdr_content_light_level_info_len;
      ctx->api_fme.data.frame.sei_total_len +=
      ctx->api_fme.data.frame.sei_hdr_content_light_level_info_len;
    }

    if (ctx->api_ctx.sei_hdr_mastering_display_color_vol_len &&
        send_sei_with_idr)
    {
      ctx->api_fme.data.frame.sei_hdr_mastering_display_color_vol_len =
      ctx->api_ctx.sei_hdr_mastering_display_color_vol_len;
      ctx->api_fme.data.frame.sei_total_len +=
      ctx->api_fme.data.frame.sei_hdr_mastering_display_color_vol_len;
    }

    if (p_param->hevc_enc_params.preferred_transfer_characteristics >= 0 &&
        send_sei_with_idr)
    {
      if (AV_CODEC_ID_H264 == avctx->codec_id)
      {
        ctx->api_fme.data.frame.preferred_characteristics_data_len = 9;
      }
      else
      {
        ctx->api_fme.data.frame.preferred_characteristics_data_len = 10;
      }
      ctx->api_ctx.preferred_characteristics_data = (uint8_t)p_param->hevc_enc_params.preferred_transfer_characteristics;
      ctx->api_fme.data.frame.sei_total_len += ctx->api_fme.data.frame.preferred_characteristics_data_len;
    }

    side_data = av_frame_get_side_data(&ctx->buffered_fme,
                                       AV_FRAME_DATA_NETINT_UDU_SEI);
    if (side_data && side_data->size > 0)
    {
      uint8_t *sei_data = (uint8_t *)side_data->data;
      int i, sei_len = 0;
      udu_sei_type = 0x05;

     /*
      * worst case:
      * even size: each 2B plus a escape 1B
      * odd size: each 2B plus a escape 1B + 1 byte
      */
      udu_sei = malloc(side_data->size * 3 / 2);
      if (udu_sei == NULL)
      {
        av_log(avctx, AV_LOG_ERROR, "failed to allocate memory for sei data.\n");
        ret = AVERROR(ENOMEM);
        return ret;
      }

      for (i = 0; i < side_data->size; i++)
      {
        if ((2 <= i) && !udu_sei[sei_len - 2] && !udu_sei[sei_len - 1] && (sei_data[i] <= 0x03))
        {
          /* insert 0x3 as emulation_prevention_three_byte */
          udu_sei[sei_len++] = 0x03;
        }
        udu_sei[sei_len++] = sei_data[i];
      }

      udu_sei_size = side_data->size;
      ext_udu_sei_size = sei_len;

      if (AV_CODEC_ID_H264 == avctx->codec_id)
      {
        /* 4B long start code + 1B nal header + 1B SEI type + Bytes of payload length + Bytes of SEI payload + 1B trailing */
        sei_len = 6 + ((udu_sei_size + 0xFE) / 0xFF) + ext_udu_sei_size + 1;
      }
      else
      {
        /* 4B long start code + 2B nal header + 1B SEI type + Bytes of payload length + Bytes of SEI payload + 1B trailing */
        sei_len = 7 + ((udu_sei_size + 0xFE) / 0xFF) + ext_udu_sei_size + 1;
      }

      /* if the total sei data is about to exceed maximum size allowed, discard the user data unregistered SEI data */
      if ((ctx->api_fme.data.frame.sei_total_len + sei_len) > NI_ENC_MAX_SEI_BUF_SIZE)
      {
        av_log(avctx, AV_LOG_WARNING, "xcoder_send_frame: sei total length %u, udu sei_len %u exceeds maximum sei size %u. discard it.\n",
               ctx->api_fme.data.frame.sei_total_len, sei_len, NI_ENC_MAX_SEI_BUF_SIZE);
        free(udu_sei);
        udu_sei = NULL;
        udu_sei_size = 0;
      }
      else
      {
        ctx->api_fme.data.frame.sei_total_len += sei_len;
      }
    }

    side_data = av_frame_get_side_data(&ctx->buffered_fme,
                                       AV_FRAME_DATA_NETINT_CUSTOM_SEI);
    if (side_data && side_data->size > 0)
    {
      int i = 0;
      int64_t local_pts = ctx->buffered_fme.pts;
      ni_all_custom_sei_t *src_all_custom_sei = (ni_all_custom_sei_t *)side_data->data;
      ni_custom_sei_t *src_custom_sei = NULL;
      uint8_t *src_sei_data = NULL;
      int custom_sei_size = 0;
      int custom_sei_size_trans = 0;
      uint8_t custom_sei_type;
      uint8_t sei_idx = 0;
      int sei_len = 0;
      ni_all_custom_sei_t *dst_all_custom_sei = NULL;
      ni_custom_sei_t *dst_custom_sei = NULL;
      uint8_t *dst_sei_data = NULL;

      dst_all_custom_sei = malloc(sizeof(ni_all_custom_sei_t));
      if (dst_all_custom_sei == NULL)
      {
        av_log(avctx, AV_LOG_ERROR, "failed to allocate memory for custom sei data, len:%d.\n", 
               sizeof(ni_all_custom_sei_t));
        ret = AVERROR(ENOMEM);
        return ret;
      }
      memset(dst_all_custom_sei, 0 ,sizeof(ni_all_custom_sei_t));

      for (sei_idx = 0; sei_idx < src_all_custom_sei->custom_sei_cnt; sei_idx++)
      {
        src_custom_sei = &src_all_custom_sei->ni_custom_sei[sei_idx];

        custom_sei_type = src_custom_sei->custom_sei_type;
        custom_sei_size = src_custom_sei->custom_sei_size;
        src_sei_data = src_custom_sei->custom_sei_data;

        dst_custom_sei = &dst_all_custom_sei->ni_custom_sei[sei_idx];
        dst_sei_data = dst_custom_sei->custom_sei_data;

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
        while (custom_sei_size_trans > 0)
        {
          dst_sei_data[sei_len++] = (custom_sei_size_trans > 0xFF ? 0xFF : (uint8_t)custom_sei_size_trans);
          custom_sei_size_trans -= 0xFF;
        }

        // payload data
        for (i = 0; (i < custom_sei_size) && (sei_len < (NI_MAX_CUSTOM_SEI_SZ - 2)); i++)
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
          av_log(avctx, AV_LOG_WARNING, "xcoder_send_frame: sei RBSP size out of limit(%d), "
                 "idx=%u, type=%u, size=%d, custom_sei_loc=%d.\n", NI_MAX_CUSTOM_SEI_SZ,
                 sei_idx, custom_sei_type, custom_sei_size, src_custom_sei->custom_sei_loc);
          free(dst_all_custom_sei);
          dst_all_custom_sei = NULL;
          break;
        }

        // trailing byte
        dst_sei_data[sei_len++] = 0x80;

        dst_custom_sei->custom_sei_size = sei_len;
        dst_custom_sei->custom_sei_type = custom_sei_type;
        dst_custom_sei->custom_sei_loc = src_custom_sei->custom_sei_loc;
        av_log(avctx, AV_LOG_TRACE, "xcoder_send_frame: sei idx=%u,type=%u, len=%d, custom_sei_loc=%d\n",
               sei_idx, custom_sei_type, sei_len, dst_custom_sei->custom_sei_loc);
      }

      if (dst_all_custom_sei)
      {
        dst_all_custom_sei->custom_sei_cnt = src_all_custom_sei->custom_sei_cnt;
        ctx->api_ctx.pkt_custom_sei[local_pts % NI_FIFO_SZ] = dst_all_custom_sei;
        av_log(avctx, AV_LOG_TRACE, "xcoder_send_frame: sei number %d pts %" PRId64 ".\n",
               dst_all_custom_sei->custom_sei_cnt, local_pts);
      }
    }

    if (ctx->api_fme.data.frame.sei_total_len > NI_ENC_MAX_SEI_BUF_SIZE)
    {
      av_log(avctx, AV_LOG_ERROR, "xcoder_send_frame: sei total length %u exceeds maximum sei size %u.\n",
             ctx->api_fme.data.frame.sei_total_len, NI_ENC_MAX_SEI_BUF_SIZE);
      ret = AVERROR(EINVAL);
      return ret;
    }

    ctx->api_fme.data.frame.extra_data_len += ctx->api_fme.data.frame.sei_total_len;
    //FW accomodation: whatever add reconfig size to allocation if sei or roi are present
    if ((ctx->api_fme.data.frame.sei_total_len ||
         ctx->api_fme.data.frame.roi_len)
        && !ctx->api_fme.data.frame.reconf_len)
    {
      ctx->api_fme.data.frame.extra_data_len += sizeof(ni_encoder_change_params_t);
    }

    ishwframe = (ctx->buffered_fme.format == AV_PIX_FMT_NI);
    if (ctx->api_ctx.auto_dl_handle != NI_INVALID_DEVICE_HANDLE)
    {
      ishwframe = 0;
      format_in_use = avctx->sw_pix_fmt;
      av_log(avctx, AV_LOG_TRACE, "xcoder_send_frame: Autodownload mode, disable hw frame\n");
    }
    else
    {
      format_in_use = ctx->buffered_fme.format;
    }

    if (ishwframe)
    {
      ret = sizeof(ni_hwframe_surface_t);
    }
    else
    {
      ret = av_image_get_buffer_size(format_in_use,
                                     ctx->buffered_fme.width,
                                     ctx->buffered_fme.height, 1);
    }
#if FF_API_PKT_PTS
    av_log(avctx, AV_LOG_TRACE, "xcoder_send_frame: pts=%" PRId64 ", pkt_dts=%" PRId64 ", pkt_pts=%" PRId64 "\n",
           ctx->buffered_fme.pts, ctx->buffered_fme.pkt_dts,
           ctx->buffered_fme.pkt_pts);
#endif
    av_log(avctx, AV_LOG_TRACE, "xcoder_send_frame: buffered_fme.format=%d, "
           "width=%d, height=%d, pict_type=%d, key_frame=%d, size=%d\n",
           format_in_use, ctx->buffered_fme.width,
           ctx->buffered_fme.height, ctx->buffered_fme.pict_type,
           ctx->buffered_fme.key_frame, ret);

    if (ret < 0)
    {
      return ret;
    }

    if (ishwframe)
    {
      ni_frame_buffer_alloc_hwenc(&(ctx->api_fme.data.frame),
                                  ODD2EVEN(ctx->buffered_fme.width),
                                  ODD2EVEN(ctx->buffered_fme.height),
                                  ctx->api_fme.data.frame.extra_data_len);
      if (!ctx->api_fme.data.frame.p_data[3])
      {
        return AVERROR(ENOMEM);
      }
      uint8_t *dsthw = ctx->api_fme.data.frame.p_data[3];
      const uint8_t *srchw = (const uint8_t *)ctx->buffered_fme.data[3];
      av_log(avctx, AV_LOG_TRACE, "dst=%p src=%p, len =%d\n", dsthw, srchw, ctx->api_fme.data.frame.data_len[3]);
      memcpy(dsthw, srchw, ctx->api_fme.data.frame.data_len[3]);
      av_log(avctx, AV_LOG_TRACE, "session_id:%u, FrameIdx:%d, %d, W-%u, H-%u, bit_depth:%d, encoding_type:%d\n",
             ((ni_hwframe_surface_t *)dsthw)->ui16SessionID,
             ((ni_hwframe_surface_t *)dsthw)->i8FrameIdx,
             ((ni_hwframe_surface_t *)dsthw)->i8InstID,
             ((ni_hwframe_surface_t *)dsthw)->ui16width,
             ((ni_hwframe_surface_t *)dsthw)->ui16height,
             ((ni_hwframe_surface_t *)dsthw)->bit_depth,
             ((ni_hwframe_surface_t *)dsthw)->encoding_type);
    }
    else
    {
      int dst_stride[NI_MAX_NUM_DATA_POINTERS] = {0};
      int dst_height_aligned[NI_MAX_NUM_DATA_POINTERS] = {0};
      int src_height[NI_MAX_NUM_DATA_POINTERS] = {0};
      
      src_height[0] = frame_height;
      src_height[1] = src_height[2] = frame_height / 2;

      ni_get_hw_yuv420p_dim(frame_width, frame_height,
                            ctx->api_ctx.bit_depth_factor,
                            avctx->codec_id == AV_CODEC_ID_H264,
                            dst_stride, dst_height_aligned);

      // alignment(16) extra padding for H.264 encoding
      ni_frame_buffer_alloc_v3(&(ctx->api_fme.data.frame),
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
        av_log(avctx, AV_LOG_TRACE, "xcoder_send_frame: api_fme.data_len[0]=%d, "
               "buffered_fme.linesize=%d/%d/%d, dst alloc linesize = %d/%d/%d, "
               "src height = %d/%d%d, dst height aligned = %d/%d/%d, "
               "ctx->api_fme.force_key_frame=%d, extra_data_len=%d "
               "sei_size=%u (hdr_content_light_level %u hdr_mastering_display_color_vol %u hdr10+ %u hrd %u) "
               "reconf_size=%u roi_size=%u force_pic_qp=%u udu_sei_size=%u "
               "use_cur_src_as_long_term_pic %u use_long_term_ref %u\n",
               ctx->api_fme.data.frame.data_len[0],
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
               hrd_sei_len,
               ctx->api_fme.data.frame.reconf_len,
               ctx->api_fme.data.frame.roi_len,
               ctx->api_fme.data.frame.force_pic_qp,
               udu_sei_size,
               ctx->api_fme.data.frame.use_cur_src_as_long_term_pic,
               ctx->api_fme.data.frame.use_long_term_ref);

        ni_copy_hw_yuv420p(ctx->api_fme.data.frame.p_data, ctx->buffered_fme.data,
                           ctx->buffered_fme.width, ctx->buffered_fme.height,
                           ctx->api_ctx.bit_depth_factor,
                           dst_stride, dst_height_aligned,
                           ctx->buffered_fme.linesize, src_height);

        av_log(avctx, AV_LOG_TRACE, "After memcpy p_data 0:0x%p, 1:0x%p, 2:0x%p  "
               "len:0:%d 1:%d 2:%d\n",
               ctx->api_fme.data.frame.p_data[0],
               ctx->api_fme.data.frame.p_data[1],
               ctx->api_fme.data.frame.p_data[2],
               ctx->api_fme.data.frame.data_len[0],
               ctx->api_fme.data.frame.data_len[1],
               ctx->api_fme.data.frame.data_len[2]);
      }
      else
      {
        av_log(avctx, AV_LOG_TRACE, "xcoder_send_frame:Autodownload to be run\n");
        avhwf_ctx = (AVHWFramesContext*)ctx->buffered_fme.hw_frames_ctx->data;
        nif_src_ctx = avhwf_ctx->internal->priv;

        ni_hwframe_surface_t *src_surf = (ni_hwframe_surface_t*)((uint8_t*)ctx->buffered_fme.data[3]);
        ni_session_data_io_t * p_session_data = &(ctx->api_fme);
      
        ret = ni_device_session_hwdl(&nif_src_ctx->api_ctx, p_session_data, src_surf);
        if (ret <= 0)
        {
          av_log(avctx, AV_LOG_ERROR, "nienc.c:ni_hwdl_frame() failed to retrieve frame\n");
          return AVERROR_EXTERNAL;
        }
      }
    }
    // fill in extra data (excluding meta data hdr)
    uint8_t *dst = (uint8_t *)ctx->api_fme.data.frame.p_data[3] + 
                   ctx->api_fme.data.frame.data_len[3] +
                   NI_APP_ENC_FRAME_META_DATA_SIZE;

    // fill in reconfig data, if enabled
    // FW accomodation: whatever add reconfig size to dst if sei or roi or reconfig are present
    if ((ctx->api_fme.data.frame.reconf_len || ctx->api_fme.data.frame.roi_len
         || ctx->api_fme.data.frame.sei_total_len) && ctx->g_enc_change_params)
    {
      memcpy(dst, ctx->g_enc_change_params, sizeof(ni_encoder_change_params_t));
      dst += sizeof(ni_encoder_change_params_t);
    }

    // fill in ROI map, if enabled
    if (ctx->api_fme.data.frame.roi_len)
    {
      if (AV_CODEC_ID_H264 == avctx->codec_id && ctx->avc_roi_map)
      {
        memcpy(dst, ctx->avc_roi_map, ctx->api_fme.data.frame.roi_len);
        dst += ctx->api_fme.data.frame.roi_len;
      }
      else if (AV_CODEC_ID_HEVC == avctx->codec_id && ctx->hevc_roi_map)
      {
        memcpy(dst, ctx->hevc_roi_map, ctx->api_fme.data.frame.roi_len);
        dst += ctx->api_fme.data.frame.roi_len;
      }
    }

    // HDR mastering display color volume
    if (ctx->api_fme.data.frame.sei_hdr_mastering_display_color_vol_len)
    {
      dst[0] = dst[1] = dst[2] = 0;
      dst[3] = 1;
      if (AV_CODEC_ID_HEVC == avctx->codec_id)
      {
        dst[4] = 0x4e;
        dst[5] = 1;
        dst[6] = 0x89;  // payload type=137
        dst[7] = 0x18;  // payload size=24
        dst += 8;
      }
      else
      {
        dst[4] = 0x6;
        dst[5] = 0x89;  // payload type=137
        dst[6] = 0x18;  // payload size=24
        dst += 7;
      }

      ni_enc_mastering_display_colour_volume_t *p_mdcv =
      (ni_enc_mastering_display_colour_volume_t*)dst;
      AVMasteringDisplayMetadata *p_src =
      (AVMasteringDisplayMetadata*)ctx->api_ctx.p_master_display_meta_data;

      const int chroma_den = 50000;
      const int luma_den = 10000;
      uint16_t dp00 = 0, dp01 = 0, dp10 = 0, dp11 = 0, dp20 = 0, dp21 = 0,
      wpx = 0, wpy = 0;
      if (p_src->has_primaries)
      {
        // this is stored in r,g,b order and needs to be in g.b,r order
        // when sent to encoder
        dp00 = lrint(chroma_den * av_q2d(p_src->display_primaries[1][0]));
        p_mdcv->display_primaries[0][0] = htons((uint16_t)dp00);
        dp01 = lrint(chroma_den * av_q2d(p_src->display_primaries[1][1]));
        p_mdcv->display_primaries[0][1] = htons((uint16_t)dp01);
        dp10 = lrint(chroma_den * av_q2d(p_src->display_primaries[2][0]));
        p_mdcv->display_primaries[1][0] = htons((uint16_t)dp10);
        dp11 = lrint(chroma_den * av_q2d(p_src->display_primaries[2][1]));
        p_mdcv->display_primaries[1][1] = htons((uint16_t)dp11);
        dp20 = lrint(chroma_den * av_q2d(p_src->display_primaries[0][0]));
        p_mdcv->display_primaries[2][0] = htons((uint16_t)dp20);
        dp21 = lrint(chroma_den * av_q2d(p_src->display_primaries[0][1]));
        p_mdcv->display_primaries[2][1] = htons((uint16_t)dp21);

        wpx = lrint(chroma_den * av_q2d(p_src->white_point[0]));
        p_mdcv->white_point_x = htons((uint16_t)wpx);
        wpy = lrint(chroma_den * av_q2d(p_src->white_point[1]));
        p_mdcv->white_point_y = htons((uint16_t)wpy);
      }

      av_log(avctx, AV_LOG_TRACE, "mastering display color volume, primaries "
             "%u/%u/%u/%u/%u/%u white_point_x/y %u/%u max/min_lumi %u/%u\n",
             (uint16_t)dp00, (uint16_t)dp01, (uint16_t)dp10,
             (uint16_t)dp11, (uint16_t)dp20, (uint16_t)dp21,
             (uint16_t)wpx,  (uint16_t)wpy,
             (uint32_t)(luma_den * av_q2d(p_src->max_luminance)),
             (uint32_t)(luma_den * av_q2d(p_src->min_luminance)));

      dst += 6 * 2 + 2 * 2;
      if (p_src->has_luminance)
      {
        memcpy(dst, ctx->api_ctx.ui8_mdcv_max_min_lum_data,
               ctx->api_ctx.mdcv_max_min_lum_data_len);
        dst += ctx->api_ctx.mdcv_max_min_lum_data_len;
      }
      *dst = 0x80;
      dst++;
    }

    // HDR content light level info
    if (ctx->api_fme.data.frame.sei_hdr_content_light_level_info_len)
    {
      dst[0] = dst[1] = dst[2] = 0;
      dst[3] = 1;
      if (AV_CODEC_ID_HEVC == avctx->codec_id)
      {
        dst[4] = 0x4e;
        dst[5] = 1;
        dst[6] = 0x90;  // payload type=144
        dst[7] = 4;     // payload size=4
        dst += 8;
      }
      else
      {
        dst[4] = 0x6;
        dst[5] = 0x90;  // payload type=144
        dst[6] = 4;     // payload size=4
        dst += 7;
      }

      memcpy(dst, ctx->api_ctx.ui8_light_level_data,
             ctx->api_ctx.light_level_data_len);
      dst += ctx->api_ctx.light_level_data_len;
      *dst = 0x80;
      dst++;
    }

    //HLG preferred characteristics SEI
    if (ctx->api_fme.data.frame.preferred_characteristics_data_len)
    {
      dst[0] = dst[1] = dst[2] = 0;
      dst[3] = 1;
      if (AV_CODEC_ID_HEVC == avctx->codec_id)
      {
        dst[4] = 0x4e;
        dst[5] = 1;
        dst[6] = 0x93;  // payload type=147
        dst[7] = 1;     // payload size=1
        dst += 8;
      }
      else
      {
        dst[4] = 0x6;
        dst[5] = 0x93;  // payload type=147
        dst[6] = 1;     // payload size=1
        dst += 7;
      }
      *dst = ctx->api_ctx.preferred_characteristics_data;
      dst++;
      *dst = 0x80;
      dst++;
    }
    ctx->sentFrame = 1;
    // close caption
    if (ctx->api_fme.data.frame.sei_cc_len && cc_data && cc_size)
    {
      if (AV_CODEC_ID_HEVC == avctx->codec_id)
      {
        memcpy(dst, ctx->api_ctx.itu_t_t35_cc_sei_hdr_hevc,
               NI_CC_SEI_HDR_HEVC_LEN);
        dst += NI_CC_SEI_HDR_HEVC_LEN;
        memcpy(dst, cc_data_emu_prevent, cc_size_emu_prevent);
        dst += cc_size_emu_prevent;
        memcpy(dst, ctx->api_ctx.sei_trailer, NI_CC_SEI_TRAILER_LEN);
        dst += NI_CC_SEI_TRAILER_LEN;
      }
      else if (AV_CODEC_ID_H264 == avctx->codec_id)
      {
        memcpy(dst, ctx->api_ctx.itu_t_t35_cc_sei_hdr_h264,
               NI_CC_SEI_HDR_H264_LEN);
        dst += NI_CC_SEI_HDR_H264_LEN;
        memcpy(dst, cc_data_emu_prevent, cc_size_emu_prevent);
        dst += cc_size_emu_prevent;
        memcpy(dst, ctx->api_ctx.sei_trailer, NI_CC_SEI_TRAILER_LEN);
        dst += NI_CC_SEI_TRAILER_LEN;
      }
    }

    // HDR10+
    if (ctx->api_fme.data.frame.sei_hdr_plus_len)
    {
      if (AV_CODEC_ID_HEVC == avctx->codec_id)
      {
        memcpy(dst, ctx->api_ctx.itu_t_t35_hdr10p_sei_hdr_hevc,
               NI_HDR10P_SEI_HDR_HEVC_LEN);
        dst += NI_HDR10P_SEI_HDR_HEVC_LEN;
        memcpy(dst, hdr10p_buf, hdr10p_num_bytes);
        dst += hdr10p_num_bytes;
        *dst = ctx->api_ctx.sei_trailer[1];
        dst += NI_RBSP_TRAILING_BITS_LEN;
      }
      else if (AV_CODEC_ID_H264 == avctx->codec_id)
      {
        memcpy(dst, ctx->api_ctx.itu_t_t35_hdr10p_sei_hdr_h264,
               NI_HDR10P_SEI_HDR_H264_LEN);
        dst += NI_HDR10P_SEI_HDR_H264_LEN;
        memcpy(dst, hdr10p_buf, hdr10p_num_bytes);
        dst += hdr10p_num_bytes;
        *dst = ctx->api_ctx.sei_trailer[1];
        dst += NI_RBSP_TRAILING_BITS_LEN;
      }
    }

    // HRD SEI
    if (hrd_sei_len)
    {
      memcpy(dst, hrd_buf, hrd_sei_len);
      dst += hrd_sei_len;
    }

    if (udu_sei && udu_sei_size)
    {
      int payload_size = udu_sei_size;
      *dst++ = 0x00;   //long start code
      *dst++ = 0x00;
      *dst++ = 0x00;
      *dst++ = 0x01;
      if (AV_CODEC_ID_H264 == avctx->codec_id)
      {
        *dst++ = 0x06;   //nal type: SEI
      }
      else
      {
        *dst++ = 0x4e;   //nal type: SEI
        *dst++ = 0x01;
      }
      *dst++ = udu_sei_type;   //SEI type: user data unregistered or user customization

      /* original payload size */
      while (payload_size > 0)
      {
        *dst++ = (payload_size > 0xFF ? 0xFF : (uint8_t)payload_size);
        payload_size -= 0xFF;
      }

      /* extended payload data */
      memcpy(dst, udu_sei, ext_udu_sei_size);
      dst += ext_udu_sei_size;

      /* trailing byte */
      *dst = 0x80;
      dst++;

      free(udu_sei);
      udu_sei = NULL;
      udu_sei_size = 0;
    }
  }
#ifdef NIENC_MULTI_THREAD
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
    else if (NI_RETCODE_ERROR_VPU_RECOVERY == sent)
    {
      sent = xcoder_encode_reset(avctx);
    }

    if (sent < 0)
    {
      ret = AVERROR(EIO);
    }
    else
    {
      if (frame && ishwframe)
      {
        av_frame_ref(ctx->sframe_pool[ctx->aFree_Avframes_list[ctx->freeHead]], frame);
        av_log(avctx, AV_LOG_DEBUG, "AVframe_index = %d popped from head %d\n", ctx->aFree_Avframes_list[ctx->freeHead], ctx->freeHead);
        if (deq_free_frames(ctx)!= 0)
        {
          ret = AVERROR_EXTERNAL;
          return ret;
        }
      }
      //pushing input pts in circular FIFO
      ctx->api_ctx.enc_pts_list[ctx->api_ctx.enc_pts_w_idx % NI_FIFO_SZ] = ctx->api_fme.data.frame.pts;
      ctx->api_ctx.enc_pts_w_idx ++;
      ret = 0;
    }
  }
  else if (ishwframe)
  {
    sent = ni_device_session_write(&ctx->api_ctx, &ctx->api_fme, NI_DEVICE_TYPE_ENCODER);
    //ctx->sframe_pool[((niFrameSurface1_t*)((uint8_t*)frame->data[3]))->i8FrameIdx] = av_buffer_ref(frame);
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
      av_frame_ref(ctx->sframe_pool[((niFrameSurface1_t*)((uint8_t*)frame->data[3]))->i8FrameIdx], frame);
      av_log(avctx, AV_LOG_DEBUG, "AVframe_index = %d popped from head %d\n", ctx->aFree_Avframes_list[ctx->freeHead], ctx->freeHead);
      if (deq_free_frames(ctx) != 0)
      {
        ret = AVERROR_EXTERNAL;
        return ret;
      }
      //av_frame_ref(ctx->sframe_pool[((ni_hwframe_surface_t*)((uint8_t*)frame->data[3]))->ui16FrameIdx], frame);
      ret = 0;
    }
  }
  else
  {
    av_log(avctx, AV_LOG_DEBUG, "xcoder_send_frame start 111 %p, session_info %d, device_handle %d\n",
           ctx->api_ctx.session_info, ctx->api_ctx.session_id, ctx->api_ctx.device_handle);
    if ((ctx->api_ctx.session_id != NI_INVALID_SESSION_ID) && (ctx->api_ctx.device_handle != NI_INVALID_DEVICE_HANDLE))
    {
      av_log(avctx, AV_LOG_DEBUG, "xcoder_send_frame start 111 %p\n", ctx->api_ctx.session_info);
      write_thread_arg_struct_t *write_thread_args = (write_thread_arg_struct_t *)malloc(sizeof(write_thread_arg_struct_t));
      pthread_mutex_init(&write_thread_args->mutex, NULL);
      pthread_cond_init(&write_thread_args->cond, NULL);
      write_thread_args->running = 0;
      write_thread_args->ctx = ctx;
      av_log(avctx, AV_LOG_DEBUG, "xcoder_send_frame: session_id %d, device_handle %d\n", ctx->api_ctx.session_id, ctx->api_ctx.device_handle);
      av_log(avctx, AV_LOG_DEBUG, "xcoder_send_frame: ctx %p\n", write_thread_args->ctx);
      ctx->api_ctx.session_info = (void *)write_thread_args;
      write_thread_args->running = 1;
      int ret = threadpool_auto_add_task_thread(&pool, write_frame_thread, write_thread_args, 1);
      if (ret < 0)
      {
        av_log(avctx, AV_LOG_ERROR, "failed to add_task_thread to threadpool\n");
        return ret;
      }
    }
  }
#else
  sent = ni_device_session_write(&ctx->api_ctx, &ctx->api_fme, NI_DEVICE_TYPE_ENCODER);
  av_log(avctx, AV_LOG_DEBUG, "xcoder_send_frame: pts %lld dts %lld size %d sent to xcoder\n",
         ctx->api_fme.data.frame.pts, ctx->api_fme.data.frame.dts, sent);

  // return EIO at error
  if (NI_RETCODE_ERROR_VPU_RECOVERY == sent)
  {
    ret = xcoder_encode_reset(avctx);
    if (ret < 0)
    {
      av_log(avctx, AV_LOG_ERROR, "xcoder_send_frame(): VPU recovery failed:%d, "
             "returning EIO\n", sent);
      ret = AVERROR(EIO);
    }
    return ret;
  }
  else if (sent < 0)
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
    if (ctx->api_ctx.status == NI_RETCODE_NVME_SC_WRITE_BUFFER_FULL)
    {
      if (ctx->api_param.strict_timeout_mode)
      {
        av_log(avctx, AV_LOG_ERROR, "xcoder_send_frame(): Error Strict timeout period exceeded, returning EAGAIN\n");
        ret = AVERROR(EAGAIN);
      }
      else
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
  else
  {
    if (!ctx->eos_fme_received && ishwframe)
    {
      av_frame_ref(ctx->sframe_pool[ctx->aFree_Avframes_list[ctx->freeHead]], &ctx->buffered_fme);
      av_log(avctx, AV_LOG_DEBUG, "AVframe_index = %d popped from free head %d\n", ctx->aFree_Avframes_list[ctx->freeHead], ctx->freeHead);
      av_log(avctx, AV_LOG_TRACE, "ctx->buffered_fme->data[3] %p sframe_pool[%d]->data[3] %p\n",
             ctx->buffered_fme.data[3], ctx->aFree_Avframes_list[ctx->freeHead],
             ctx->sframe_pool[ctx->aFree_Avframes_list[ctx->freeHead]]->data[3]);
      if (ctx->sframe_pool[ctx->aFree_Avframes_list[ctx->freeHead]]->data[3])
      {
        av_log(avctx, AV_LOG_DEBUG, "sframe_pool[%d] ui16FrameIdx %u, device_handle %d\n",
               ctx->aFree_Avframes_list[ctx->freeHead],
               ((ni_hwframe_surface_t*)((uint8_t*)ctx->sframe_pool[ctx->aFree_Avframes_list[ctx->freeHead]]->data[3]))->i8FrameIdx,
               ((ni_hwframe_surface_t*)((uint8_t*)ctx->sframe_pool[ctx->aFree_Avframes_list[ctx->freeHead]]->data[3]))->device_handle);
        av_log(avctx, AV_LOG_TRACE, "xcoder_send_frame: after ref sframe_pool, hw frame av_buffer_get_ref_count=%d, data[3]=%p\n",
               av_buffer_get_ref_count(ctx->sframe_pool[ctx->aFree_Avframes_list[ctx->freeHead]]->buf[0]),
               ctx->sframe_pool[ctx->aFree_Avframes_list[ctx->freeHead]]->data[3]);
      }
      if (deq_free_frames(ctx) != 0)
      {
        av_log(avctx, AV_LOG_ERROR, "free frames is empty\n");
        ret = AVERROR_EXTERNAL;
        return ret;
      }
    }

    // only if it's NOT sequence change flushing (in which case only the eos
    // was sent and not the first sc pkt) AND
    // only after successful sending will it be removed from fifo
    if (SESSION_RUN_STATE_SEQ_CHANGE_DRAINING != ctx->api_ctx.session_run_state)
    {
      if (! is_input_fifo_empty(ctx))
      {
        av_fifo_drain(ctx->fme_fifo, sizeof(AVFrame));
        av_log(avctx, AV_LOG_DEBUG, "fme popped pts:%" PRId64 ", "
               "fifo size: %lu\n",  ctx->buffered_fme.pts,
               av_fifo_size(ctx->fme_fifo) / sizeof(AVFrame));
      }
      av_frame_unref(&ctx->buffered_fme);
    }
    else
    {
      av_log(avctx, AV_LOG_TRACE, "XCoder frame(eos) sent, sequence changing! NO fifo pop !\n");
    }

    //pushing input pts in circular FIFO
    ctx->api_ctx.enc_pts_list[ctx->api_ctx.enc_pts_w_idx % NI_FIFO_SZ] = ctx->api_fme.data.frame.pts;
    ctx->api_ctx.enc_pts_w_idx++;
    ret = 0;

    // have another check before return: if no more frames in fifo to send and
    // we've got eos (NULL) frame from upper stream, flag for flushing
    if (ctx->eos_fme_received && is_input_fifo_empty(ctx))
    {
      av_log(avctx, AV_LOG_DEBUG, "Upper stream EOS frame received, fifo empty, start flushing ..\n");
      ctx->encoder_flushing = 1;
    }
  }
#endif
  if (ctx->encoder_flushing)
  {
    av_log(avctx, AV_LOG_DEBUG, "xcoder_send_frame flushing ..\n");
    ret = ni_device_session_flush(&ctx->api_ctx, NI_DEVICE_TYPE_ENCODER);
  }

  return ret;
}

static int xcoder_encode_reinit(AVCodecContext *avctx)
{
  int ret = 0;
  XCoderH265EncContext *ctx = avctx->priv_data;

  ctx->eos_fme_received = 0;
  ctx->encoder_eof = 0;
  ctx->encoder_flushing = 0;

  if (ctx->api_ctx.pts_table && ctx->api_ctx.dts_queue)
  {
    xcoder_encode_close(avctx);
  }
  ctx->started = 0;
  ctx->firstPktArrived = 0;
  ctx->spsPpsArrived = 0;
  ctx->spsPpsHdrLen = 0;
  ctx->p_spsPpsHdr = NULL;

  // and re-init avctx's resolution to the changed one that is
  // stored in the first frame of the fifo
  AVFrame tmp_fme;
  av_fifo_generic_peek(ctx->fme_fifo, &tmp_fme, sizeof(AVFrame), NULL);
  av_log(avctx, AV_LOG_INFO, "xcoder_receive_packet resolution "
         "changing %dx%d -> %dx%d\n", avctx->width, avctx->height,
         tmp_fme.width, tmp_fme.height);
  avctx->width = tmp_fme.width;
  avctx->height = tmp_fme.height;

  ret = xcoder_encode_init(avctx);
  ctx->api_ctx.session_run_state = SESSION_RUN_STATE_NORMAL;

  while ((ret >= 0) && !is_input_fifo_empty(ctx))
  {
    ctx->api_ctx.session_run_state = SESSION_RUN_STATE_QUEUED_FRAME_DRAINING;
    ret = xcoder_send_frame(avctx, NULL);

    // new resolution changes or buffer full should break flush.
    // if needed, add new cases here
    if (SESSION_RUN_STATE_SEQ_CHANGE_DRAINING == ctx->api_ctx.session_run_state)
    {
      av_log(avctx, AV_LOG_DEBUG, "xcoder_encode_reinit(): break flush queued frames, "
             "resolution changes again, session_run_state=%d, status=%d\n",
             ctx->api_ctx.session_run_state, ctx->api_ctx.status);
      break;
    }
    else if (NI_RETCODE_NVME_SC_WRITE_BUFFER_FULL == ctx->api_ctx.status)
    {
      ctx->api_ctx.session_run_state = SESSION_RUN_STATE_NORMAL;
      av_log(avctx, AV_LOG_DEBUG, "xcoder_encode_reinit(): break flush queued frames, "
            "because of buffer full, session_run_state=%d, status=%d\n",
            ctx->api_ctx.session_run_state, ctx->api_ctx.status);
      break;
    }
    else
    {
      ctx->api_ctx.session_run_state = SESSION_RUN_STATE_NORMAL;
      av_log(avctx, AV_LOG_DEBUG, "xcoder_encode_reinit(): continue to flush queued frames, "
             "ret=%d\n", ret);
    }
  }

  return ret;
}

int xcoder_receive_packet(AVCodecContext *avctx, AVPacket *pkt)
{
  XCoderH265EncContext *ctx = avctx->priv_data;
  ni_encoder_params_t *p_param = &ctx->api_param;
  int ret = 0;
  int recv;
  AVFrame *frame = NULL;
  ni_packet_t *xpkt = &ctx->api_pkt.data.packet;

  av_log(avctx, AV_LOG_DEBUG, "XCoder receive packet\n");

  if (ctx->encoder_eof)
  {
    av_log(avctx, AV_LOG_TRACE, "xcoder_receive_packet: EOS\n");
    return AVERROR_EOF;
  }

  ni_packet_buffer_alloc(xpkt, NI_MAX_TX_SZ);
  while (1)
  {
    xpkt->recycle_index = -1;
    recv = ni_device_session_read(&ctx->api_ctx, &ctx->api_pkt, NI_DEVICE_TYPE_ENCODER);

    av_log(avctx, AV_LOG_TRACE, "XCoder receive packet: xpkt.end_of_stream=%d, xpkt.data_len=%d, recv=%d, encoder_flushing=%d, encoder_eof=%d\n",
           xpkt->end_of_stream, xpkt->data_len, recv, ctx->encoder_flushing, ctx->encoder_eof);

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
          if (ret >= 0)
          {
            ret = AVERROR(EAGAIN);
          }
          break;
        }

        ret = AVERROR_EOF;
        av_log(avctx, AV_LOG_TRACE, "xcoder_receive_packet: got encoder_eof, "
               "return AVERROR_EOF\n");
        break;
      }
      else
      {
        if (NI_RETCODE_ERROR_VPU_RECOVERY == recv)
        {
          ret = xcoder_encode_reset(avctx);
          if (ret < 0)
          {
            av_log(avctx, AV_LOG_ERROR, "xcoder_receive_packet(): VPU recovery failed:%d, returning EIO\n", recv);
            ret = AVERROR(EIO);
          }
          return ret;
        }

        if (recv < 0)
        {
          if ((NI_RETCODE_ERROR_INVALID_SESSION == recv) && !ctx->started)  // session may be in recovery state, return EAGAIN
          {
            av_log(avctx, AV_LOG_ERROR, "XCoder receive packet: VPU might be reset, invalid session id\n");
            ret = AVERROR(EAGAIN);
          }
          else
          {
            av_log(avctx, AV_LOG_ERROR, "XCoder receive packet: Persistent failure, returning EIO,ret=%d\n", recv);
            ret = AVERROR(EIO);
          }
          ctx->gotPacket = 0;
          ctx->sentFrame = 0;
          break;
        }

        if (ctx->api_param.low_delay_mode && ctx->sentFrame && !ctx->gotPacket)
        {
          av_log(avctx, AV_LOG_TRACE, "XCoder receive packet: low delay mode,"
                 " keep reading until pkt arrives\n");
          continue;
        }

        ctx->gotPacket = 0;
        ctx->sentFrame = 0;
        if (!is_input_fifo_empty(ctx) &&
            (SESSION_RUN_STATE_NORMAL == ctx->api_ctx.session_run_state) &&
            (NI_RETCODE_NVME_SC_WRITE_BUFFER_FULL != ctx->api_ctx.status))
        {
          ctx->api_ctx.session_run_state = SESSION_RUN_STATE_QUEUED_FRAME_DRAINING;
          ret = xcoder_send_frame(avctx, NULL);

          // if session_run_state is changed in xcoder_send_frame, keep it
          if (SESSION_RUN_STATE_QUEUED_FRAME_DRAINING == ctx->api_ctx.session_run_state)
          {
            ctx->api_ctx.session_run_state = SESSION_RUN_STATE_NORMAL;
          }
          if (ret < 0)
          {
            av_log(avctx, AV_LOG_ERROR, "xcoder_receive_packet(): xcoder_send_frame 1 error, ret=%d\n",
                   ret);
            return ret;
          }
          continue;
        }
        ret = AVERROR(EAGAIN);
        if (! ctx->encoder_flushing && ! ctx->eos_fme_received)
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
      if (avctx->pix_fmt == AV_PIX_FMT_NI && xpkt->recycle_index >= 0 && xpkt->recycle_index < 1056)
      {
        av_log(avctx, AV_LOG_VERBOSE,
               "UNREF index %d.\n", xpkt->recycle_index);
        int avframe_index = recycle_index_2_avframe_index(ctx, xpkt->recycle_index);
        if (avframe_index >=0 && ctx->sframe_pool[avframe_index])
        {
          frame = ctx->sframe_pool[avframe_index];

          av_log(avctx, AV_LOG_TRACE, "xcoder_receive_packet: after ref sframe_pool, hw frame av_buffer_get_ref_count=%d, data[3]=%p\n",
                 av_buffer_get_ref_count(ctx->sframe_pool[avframe_index]->buf[0]),
                 ctx->sframe_pool[avframe_index]->data[3]);
          av_frame_unref(ctx->sframe_pool[avframe_index]);
          av_log(avctx, AV_LOG_DEBUG, "AVframe_index = %d pushed to free tail %d\n", avframe_index, ctx->freeTail);
          if (enq_free_frames(ctx, avframe_index) != 0)
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

      int meta_size = NI_FW_ENC_BITSTREAM_META_DATA_SIZE;
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
          av_log(avctx, AV_LOG_TRACE, "XCoder receive packet: low delay mode,"
                 " keep reading until 1st pkt arrives\n");
          continue;
        }
        break;
      }
      ctx->gotPacket = 1;
      ctx->sentFrame = 0;

      uint8_t pic_timing_buf[NI_MAX_SEI_DATA];
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
      ni_all_custom_sei_t *ni_all_custom_sei;
      ni_custom_sei_t *ni_custom_sei;
      if (ctx->api_ctx.pkt_custom_sei[local_pts % NI_FIFO_SZ])
      {
        ni_all_custom_sei = ctx->api_ctx.pkt_custom_sei[local_pts % NI_FIFO_SZ];
        custom_sei_cnt = ni_all_custom_sei->custom_sei_cnt;
        for (sei_idx = 0; sei_idx < custom_sei_cnt; sei_idx ++)
        {
          total_custom_sei_len += ni_all_custom_sei->ni_custom_sei[sei_idx].custom_sei_size;
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
          av_log(avctx, AV_LOG_ERROR, "xcoder_receive packet: codec %d not "
                 "supported for SEI !\n", avctx->codec_id);
        }

        if (p_param->hrd_enable)
        {
          if (HEVC_NAL_IDR_W_RADL == nalu_type || HEVC_NAL_IDR_N_LP == nalu_type)
          {
            is_idr = 1;
          }

          int is_i_or_idr = (PIC_TYPE_I   == xpkt->frame_type ||
                             PIC_TYPE_IDR == xpkt->frame_type ||
                             PIC_TYPE_CRA == xpkt->frame_type);
          pic_timing_sei_len = encode_pic_timing_sei2(
            p_param, ctx, pic_timing_buf, is_i_or_idr, is_idr, xpkt->pts);
          // returned pts is display number
        }
      }

      if (! ctx->firstPktArrived)
      {
        int sizeof_spspps_attached_to_idr = ctx->spsPpsHdrLen;

        // if not enable forced repeat header, check AV_CODEC_FLAG_GLOBAL_HEADER flag
        // to determine whether to add a SPS/PPS header in the first packat
        if ((avctx->flags & AV_CODEC_FLAG_GLOBAL_HEADER) &&
            (p_param->hevc_enc_params.forced_header_enable != NI_ENC_REPEAT_HEADERS_ALL_I_FRAMES))
        {
          sizeof_spspps_attached_to_idr = 0;
        }
        ctx->firstPktArrived = 1;
        ctx->first_frame_pts = xpkt->pts;
        ret = ff_alloc_packet2(avctx, pkt,
                               xpkt->data_len - meta_size + sizeof_spspps_attached_to_idr + total_custom_sei_len + pic_timing_sei_len,
                               xpkt->data_len - meta_size + sizeof_spspps_attached_to_idr + total_custom_sei_len + pic_timing_sei_len);

        if (! ret)
        {
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
              av_log(avctx, AV_LOG_ERROR,
                     "Cannot allocate AVC/HEVC header of size %d.\n",
                     avctx->extradata_size);
              return AVERROR(ENOMEM);
            }
            memcpy(avctx->extradata, ctx->p_spsPpsHdr, avctx->extradata_size);
          }

          uint8_t *p_side_data = av_packet_new_side_data(
              pkt, AV_PKT_DATA_NEW_EXTRADATA, ctx->spsPpsHdrLen);
          if (p_side_data)
          {
            memcpy(p_side_data, ctx->p_spsPpsHdr, ctx->spsPpsHdrLen);
          }

          uint8_t *p_dst = pkt->data;
          
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
              ni_custom_sei = &ni_all_custom_sei->ni_custom_sei[sei_idx];
              if (ni_custom_sei->custom_sei_loc == NI_CUSTOM_SEI_LOC_AFTER_VCL)
              {
                break;
              }
              memcpy(p_dst, ni_custom_sei->custom_sei_data, ni_custom_sei->custom_sei_size);
              p_dst += ni_custom_sei->custom_sei_size;
              sei_idx ++;
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
              ni_custom_sei = &ni_all_custom_sei->ni_custom_sei[sei_idx];
              memcpy(p_dst, ni_custom_sei->custom_sei_data, ni_custom_sei->custom_sei_size);
              p_dst += ni_custom_sei->custom_sei_size;
              sei_idx ++;
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
          free(ctx->api_ctx.pkt_custom_sei[local_pts % NI_FIFO_SZ]);
          ctx->api_ctx.pkt_custom_sei[local_pts % NI_FIFO_SZ] = NULL;
        }
      }
      else
      {
        // insert header when intraRefresh is enabled for every
        // intraRefreshMinPeriod frames, pkt counting starts from 1, e.g. for
        // cycle of 100, the header is forced on frame 102, 202, ...;
        // note that api_ctx.pkt_num returned is the actual index + 1
        int intra_refresh_hdr_sz = 0;
        if (ctx->p_spsPpsHdr && ctx->spsPpsHdrLen &&
            p_param->hevc_enc_params.forced_header_enable &&
            (1 == p_param->hevc_enc_params.intra_mb_refresh_mode ||
             2 == p_param->hevc_enc_params.intra_mb_refresh_mode ||
             3 == p_param->hevc_enc_params.intra_mb_refresh_mode) &&
            p_param->ui32minIntraRefreshCycle > 0 &&
            ctx->api_ctx.pkt_num > 3 &&
            0 == ((ctx->api_ctx.pkt_num - 3) % p_param->ui32minIntraRefreshCycle))
        {
          intra_refresh_hdr_sz = ctx->spsPpsHdrLen;
          av_log(avctx, AV_LOG_TRACE, "xcoder_receive_packet pkt %" PRId64 " "
                 " force header on intraRefreshMinPeriod %u\n",
                 ctx->api_ctx.pkt_num - 1, p_param->ui32minIntraRefreshCycle);
        }

        ret = ff_alloc_packet2(avctx, pkt, xpkt->data_len - meta_size + total_custom_sei_len + pic_timing_sei_len + intra_refresh_hdr_sz,
                               xpkt->data_len - meta_size + total_custom_sei_len + pic_timing_sei_len + intra_refresh_hdr_sz);

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
              ni_custom_sei = &ni_all_custom_sei->ni_custom_sei[sei_idx];
              if (ni_custom_sei->custom_sei_loc == NI_CUSTOM_SEI_LOC_AFTER_VCL)
              {
                break;
              }
              memcpy(p_dst, ni_custom_sei->custom_sei_data, ni_custom_sei->custom_sei_size);
              p_dst += ni_custom_sei->custom_sei_size;
              sei_idx ++;
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
              ni_custom_sei = &ni_all_custom_sei->ni_custom_sei[sei_idx];
              memcpy(p_dst, ni_custom_sei->custom_sei_data, ni_custom_sei->custom_sei_size);
              p_dst += ni_custom_sei->custom_sei_size;
              sei_idx ++;
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
          free(ctx->api_ctx.pkt_custom_sei[local_pts % NI_FIFO_SZ]);
          ctx->api_ctx.pkt_custom_sei[local_pts % NI_FIFO_SZ] = NULL;
        }
      }
      if (!ret)
      {
        if (PIC_TYPE_IDR == xpkt->frame_type ||
            PIC_TYPE_CRA == xpkt->frame_type)
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
        if (ctx->total_frames_received < ctx->dtsOffset)
        {
          // guess dts
          pkt->dts = ctx->first_frame_pts + ((ctx->gop_offset_count - ctx->dtsOffset) * avctx->ticks_per_frame);
          ctx->gop_offset_count++;
        }
        else
        {
          // get dts from pts FIFO
          pkt->dts = ctx->api_ctx.enc_pts_list[ctx->api_ctx.enc_pts_r_idx % NI_FIFO_SZ];
          ctx->api_ctx.enc_pts_r_idx ++;
        }
        if (ctx->total_frames_received >= 1)
        {
          if (pkt->dts < ctx->latest_dts)
          {
            av_log(avctx, AV_LOG_WARNING, "dts: %ld < latest_dts: %ld.\n",
                   pkt->dts, ctx->latest_dts);
          }
        }
        if(pkt->dts > pkt->pts)
        {
          av_log(avctx, AV_LOG_WARNING, "dts: %ld, pts: %ld. Forcing dts = pts \n",
                 pkt->dts, pkt->dts);
          pkt->dts = pkt->pts;
        }
        ctx->total_frames_received++;
        ctx->latest_dts = pkt->dts;
        av_log(avctx, AV_LOG_DEBUG, "xcoder_receive_packet pkt %" PRId64 ""
               " pts %" PRId64 "  dts %" PRId64 "  size %d  st_index %d \n",
               ctx->api_ctx.pkt_num - 1, pkt->pts, pkt->dts, pkt->size,
               pkt->stream_index);
      }
      ctx->encoder_eof = xpkt->end_of_stream;

      if (ctx->encoder_eof &&
          SESSION_RUN_STATE_SEQ_CHANGE_DRAINING ==
          ctx->api_ctx.session_run_state)
      {
        // after sequence change completes, reset codec state
        av_log(avctx, AV_LOG_TRACE, "xcoder_receive_packet 2: sequence change "
               "completed, return 0 and will reopen codec !\n");
        ret = xcoder_encode_reinit(avctx);
      }
      break;
    }
  }

  return ret;
}

int xcoder_encode_frame(AVCodecContext *avctx, AVPacket *pkt,
                        const AVFrame *frame, int *got_packet)
{
  XCoderH265EncContext *ctx = avctx->priv_data;
  int ret;

  av_log(avctx, AV_LOG_DEBUG, "XCoder encode frame\n");

  ret = xcoder_send_frame(avctx, frame);
  // return immediately for critical errors
  if (AVERROR(ENOMEM) == ret || AVERROR_EXTERNAL == ret ||
      (ret < 0 && ctx->encoder_eof))
  {
    return ret;
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
      if (((ni_hwframe_surface_t*)((uint8_t*)ctx->sframe_pool[i]->data[3]))->i8FrameIdx == recycleIndex)
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
#if ((LIBAVCODEC_VERSION_MAJOR >= 58) && (LIBAVCODEC_VERSION_MINOR >= 82))
const AVCodecHWConfigInternal *ff_ni_enc_hw_configs[] = {
  HW_CONFIG_ENCODER_FRAMES(NI,  NI),
  HW_CONFIG_ENCODER_DEVICE(YUV420P, NI),
  HW_CONFIG_ENCODER_DEVICE(YUV420P10, NI),
  NULL,
};
#endif
