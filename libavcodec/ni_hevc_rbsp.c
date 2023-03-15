/*
 * NetInt HEVC RBSP parser common source code
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
 *
 * An RBSP parser according to the HEVC syntax template. Using FFmpeg put_bits
 * module for the bit write operations.
 */

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// commented out since it's not used
//#include <ni_rsrc_api.h>

#include <libavcodec/avcodec.h>
#include <libavcodec/cbs_h265.h>
#include <libavcodec/h2645_parse.h>
#include <libavcodec/hevc.h>
#include <libavcodec/hevc_ps.h>
#include <libavcodec/hevc_sei.h>
#include <libavcodec/hevcdec.h>
#include <libavutil/internal.h>

#include "ni_hevc_rbsp.h"

static const uint32_t ni_bit_set_mask[] = {
    0x00000001, 0x00000002, 0x00000004, 0x00000008, 0x00000010, 0x00000020,
    0x00000040, 0x00000080, 0x00000100, 0x00000200, 0x00000400, 0x00000800,
    0x00001000, 0x00002000, 0x00004000, 0x00008000, 0x00010000, 0x00020000,
    0x00040000, 0x00080000, 0x00100000, 0x00200000, 0x00400000, 0x00800000,
    0x01000000, 0x02000000, 0x04000000, 0x08000000, 0x10000000, 0x20000000,
    0x40000000, 0x80000000};

int ni_bitstream_init(ni_bitstream_t *stream) {
    memset(stream, 0, sizeof(ni_bitstream_t));
    stream->pb_buf = av_mallocz(MAX_PUT_BUF_SIZE);
    if (!stream->pb_buf) {
        return AVERROR(ENOMEM);
    }
    init_put_bits(&stream->pbc, stream->pb_buf, MAX_PUT_BUF_SIZE);
    return 0;
}

void ni_bitstream_deinit(ni_bitstream_t *stream) { av_freep(&stream->pb_buf); }

void ni_bitstream_reset(ni_bitstream_t *stream) {
    init_put_bits(&stream->pbc, stream->pb_buf, MAX_PUT_BUF_SIZE);
}

void ni_bitstream_fetch(const ni_bitstream_t *stream, uint8_t *buf,
                        size_t size) {
    memcpy(buf, stream->pb_buf, size);
}

int ni_bitstream_count(ni_bitstream_t *stream) {
    return put_bits_count(&stream->pbc);
}

static uint8_t ni_bitstream_dump_last_byte(ni_bitstream_t *stream) {
    return stream->pb_buf[ni_bitstream_count(stream) / 8 - 1];
}

/**
 * \brief Write a byte to a byte aligned bitstream
 * \param stream  stream the data is to be appended to
 * \param data  input data
 */
static void ni_put_byte(ni_bitstream_t *stream, uint8_t data) {
    const uint8_t emulation_prevention_three_byte = 0x03;
    av_assert0(stream->cur_bits == 0);

    if (stream->zero_cnt == 2 && data < 4) {
        put_bits(&stream->pbc, 8, emulation_prevention_three_byte);
        stream->zero_cnt = 0;
    }
    stream->zero_cnt = data == 0 ? stream->zero_cnt + 1 : 0;
    put_bits(&stream->pbc, 8, data);
    flush_put_bits(&stream->pbc);
}

/**
 * \brief Write bits to bitstream
 *        Buffers individual bits untill they make a full byte.
 * \param stream  stream the data is to be appended to
 * \param data  input data
 * \param bits  number of bits to write from data to stream
 */
void ni_put_bits(ni_bitstream_t *stream, uint8_t bits, const uint32_t data) {
    while (bits--) {
        stream->cache <<= 1;

        if (data & ni_bit_set_mask[bits]) {
            stream->cache |= 1;
        }
        stream->cur_bits++;

        // write byte to output
        if (stream->cur_bits == 8) {
            stream->cur_bits = 0;
            ni_put_byte(stream, stream->cache);
        }
    }
}

static unsigned ni_math_floor_log2(unsigned value) {
    unsigned result = 0;
    av_assert0(value > 0);

    for (int i = 4; i >= 0; --i) {
        unsigned bits  = 1ull << i;
        unsigned shift = value >= (1 << bits) ? bits : 0;
        result += shift;
        value >>= shift;
    }

    return result;
}

/**
 * \brief Write unsigned Exp-Golomb bit string
 */
static void ni_set_ue_golomb(ni_bitstream_t *stream, uint32_t code_num) {
    unsigned code_num_log2 = ni_math_floor_log2(code_num + 1);
    unsigned prefix        = 1 << code_num_log2;
    unsigned suffix        = code_num + 1 - prefix;
    unsigned num_bits      = code_num_log2 * 2 + 1;
    unsigned value         = prefix | suffix;

    ni_put_bits(stream, num_bits, value);
}

/**
 * \brief Write signed Exp-Golomb bit string
 */
static void ni_set_se_golomb(ni_bitstream_t *stream, int32_t data) {
    // Map positive values to even and negative to odd values.
    uint32_t code_num = data <= 0 ? (-data) << 1 : (data << 1) - 1;
    ni_set_ue_golomb(stream, code_num);
}

/**
 * \brief Add rbsp_trailing_bits syntax element, which aligns the bitstream.
 */
static void ni_rbsp_trailing_bits(ni_bitstream_t *stream) {
    ni_put_bits(stream, 1, 1);
    if ((stream->cur_bits & 7) != 0) {
        ni_put_bits(stream, 8 - (stream->cur_bits & 7), 0);
    }
}

/**
 * \brief Add rbsp_trailing_bits syntax element, which aligns the bitstream.
 */
void ni_write_nal_header(ni_bitstream_t *bitstream, const uint8_t nal_type,
                         const uint8_t temporal_id, const int long_start_code) {
    uint8_t byte;

    // Some useful constants
    const uint8_t start_code_prefix_one_3bytes = 0x01;
    const uint8_t zero                         = 0x00;

    // zero_byte (0x00) shall be present in the byte stream NALU of VPS, SPS
    // and PPS, or the first NALU of an access unit
    if (long_start_code) {
        put_bits(&bitstream->pbc, 8, zero);
    }

    // start_code_prefix_one_3bytes
    put_bits(&bitstream->pbc, 8, zero);
    put_bits(&bitstream->pbc, 8, zero);
    ni_put_bits(bitstream, 8, start_code_prefix_one_3bytes);

    // Handle header bits with full bytes instead of using bitstream
    // forbidden_zero_flag(1) + nal_unit_type(6) + 1bit of nuh_layer_id
    byte = nal_type << 1;
    ni_put_bits(bitstream, 8, byte);

    // 5bits of nuh_layer_id + nuh_temporal_id_plus1(3)
    byte = (temporal_id + 1) & 7;
    ni_put_bits(bitstream, 8, byte);
}

static void write_raw_ptl(ni_bitstream_t *pb,
                          const H265RawProfileTierLevel *ptl,
                          int max_sub_layers_minus1) {
    int i;

    ni_put_bits(pb, 2, ptl->general_profile_space);
    ni_put_bits(pb, 1, ptl->general_tier_flag);
    ni_put_bits(pb, 5, ptl->general_profile_idc);

    // ni_put_bits(pb, 32, 3 << 29); only general_profile_compatibility_flag [1]
    // and [2]
    for (i = 0; i < 32; i++) {
        ni_put_bits(pb, 1, ptl->general_profile_compatibility_flag[i]);
    }

    ni_put_bits(
        pb, 1,
        ptl->general_progressive_source_flag); // general_progressive_source_flag
    ni_put_bits(
        pb, 1,
        ptl->general_interlaced_source_flag); // general_interlaced_source_flag
    ni_put_bits(
        pb, 1,
        ptl->general_non_packed_constraint_flag); // general_non_packed_constraint_flag
    ni_put_bits(
        pb, 1,
        ptl->general_frame_only_constraint_flag); // general_frame_only_constraint_flag

    av_assert0(!ptl->general_one_picture_only_constraint_flag);
    av_assert0(!ptl->general_inbld_flag);
    ni_put_bits(pb, 7, 0);
    ni_put_bits(pb, 1, ptl->general_one_picture_only_constraint_flag);
    ni_put_bits(pb, 24, 0);
    ni_put_bits(pb, 11, 0);
    ni_put_bits(pb, 1, ptl->general_inbld_flag);

    // end Profile Tier

    ni_put_bits(pb, 8, ptl->general_level_idc); // general_level_idc

    if (max_sub_layers_minus1 > 1) {
        // TODO(tyroun) sub layers support
        printf("not support sub layers yet\n");
        av_assert0(0);
        return;
    }

    for (i = 0; i < max_sub_layers_minus1; i++) {
        ni_put_bits(pb, 1, ptl->sub_layer_profile_present_flag[i]);
        ni_put_bits(pb, 1, ptl->sub_layer_level_present_flag[i]);
    }

    if (max_sub_layers_minus1 > 0) {
        for (i = max_sub_layers_minus1; i < 8; i++) {
            ni_put_bits(pb, 2, 0); // reserved_zero_2bits
        }
    }
}

int ni_hevc_encode_nal_vps(ni_bitstream_t *pb, const H265RawVPS *vps) {
    int i;

    ni_put_bits(pb, 4, vps->vps_video_parameter_set_id);
    ni_put_bits(pb, 2, 3);
    ni_put_bits(pb, 6, vps->vps_max_layers_minus1);
    ni_put_bits(pb, 3, vps->vps_max_sub_layers_minus1);
    ni_put_bits(pb, 1, vps->vps_temporal_id_nesting_flag);
    ni_put_bits(pb, 16, 0xffff);

    write_raw_ptl(pb, &vps->profile_tier_level, vps->vps_max_sub_layers_minus1);

    ni_put_bits(pb, 1, vps->vps_sub_layer_ordering_info_present_flag);
    for (i = vps->vps_sub_layer_ordering_info_present_flag
                 ? 0
                 : vps->vps_max_sub_layers_minus1;
         i <= vps->vps_max_sub_layers_minus1; i++) {
        ni_set_ue_golomb(pb, vps->vps_max_dec_pic_buffering_minus1[i]);
        ni_set_ue_golomb(pb, vps->vps_max_num_reorder_pics[i]);
        ni_set_ue_golomb(pb, vps->vps_max_latency_increase_plus1[i]);
    }

    ni_put_bits(pb, 6, vps->vps_max_layer_id);
    ni_set_ue_golomb(pb, vps->vps_num_layer_sets_minus1);

    if (vps->vps_num_layer_sets_minus1 > 0) {
        avpriv_report_missing_feature(NULL, "Writing layer_id_included_flag");
        return AVERROR_PATCHWELCOME;
    }

    ni_put_bits(pb, 1, vps->vps_timing_info_present_flag);
    if (vps->vps_timing_info_present_flag) {
        ni_put_bits(pb, 32, vps->vps_num_units_in_tick);
        ni_put_bits(pb, 32, vps->vps_time_scale);
        ni_put_bits(pb, 1, vps->vps_poc_proportional_to_timing_flag);
        if (vps->vps_poc_proportional_to_timing_flag)
            ni_set_ue_golomb(pb, vps->vps_num_ticks_poc_diff_one_minus1);

        ni_set_ue_golomb(pb, vps->vps_num_hrd_parameters);
        if (vps->vps_num_hrd_parameters) {
            avpriv_report_missing_feature(NULL, "Writing HRD parameters");
            return AVERROR_PATCHWELCOME;
        }
    }

    ni_put_bits(pb, 1, 0); // extension flag

    ni_rbsp_trailing_bits(pb);

    return 0;
}

static void write_raw_scaling_list(ni_bitstream_t *pb,
                                   const H265RawScalingList *sl) {
    unsigned int size_id, matrix_id;
    int i, coef_num;

    for (size_id = 0; size_id < 4; size_id++) {
        for (matrix_id = 0; matrix_id < 6;
             matrix_id += ((size_id == 3) ? 3 : 1)) {
            ni_put_bits(pb, 1,
                        sl->scaling_list_pred_mode_flag[size_id][matrix_id]);

            if (!sl->scaling_list_pred_mode_flag[size_id][matrix_id]) {
                ni_set_ue_golomb(
                    pb,
                    sl->scaling_list_pred_matrix_id_delta[size_id][matrix_id]);
            } else {
                coef_num = FFMIN(64, 1 << (4 + (size_id << 1)));
                if (size_id > 1) {
                    ni_set_se_golomb(
                        pb,
                        sl->scaling_list_dc_coef_minus8[size_id][matrix_id]);
                }
                for (i = 0; i < coef_num; i++) {
                    ni_set_se_golomb(
                        pb,
                        sl->scaling_list_delta_coeff[size_id][matrix_id][i]);
                }
            }
        }
    }
}

static void write_raw_VUI(ni_bitstream_t *pb, const H265RawSPS *sps) {
    const H265RawVUI *vui = &sps->vui;

    if (vui->aspect_ratio_info_present_flag) {
        ni_put_bits(pb, 1, vui->aspect_ratio_info_present_flag);
        ni_put_bits(pb, 8, vui->aspect_ratio_idc);
        if (vui->aspect_ratio_idc == 255) {
            ni_put_bits(pb, 16, vui->sar_width);
            ni_put_bits(pb, 16, vui->sar_height);
        }
    } else
        ni_put_bits(pb, 1, vui->aspect_ratio_info_present_flag);

    // IF aspect ratio info
    // ENDIF

    if (vui->overscan_info_present_flag) {
        ni_put_bits(pb, 1, vui->overscan_info_present_flag);
        ni_put_bits(pb, 1, vui->overscan_appropriate_flag);
    } else
        ni_put_bits(pb, 1, vui->overscan_info_present_flag);

    // IF overscan info
    // ENDIF

    if (vui->video_signal_type_present_flag) {
        ni_put_bits(pb, 1, vui->video_signal_type_present_flag);
        ni_put_bits(pb, 3, vui->video_format);
        ni_put_bits(pb, 1, vui->video_full_range_flag);

        if (vui->colour_description_present_flag) {
            ni_put_bits(pb, 1, vui->colour_description_present_flag);
            ni_put_bits(pb, 8, vui->colour_primaries);
            ni_put_bits(pb, 8, vui->transfer_characteristics);
            ni_put_bits(pb, 8, vui->matrix_coefficients);
        } else
            ni_put_bits(pb, 1, vui->colour_description_present_flag);
    } else
        ni_put_bits(pb, 1, vui->video_signal_type_present_flag);

    // IF video type
    // ENDIF

    if (vui->chroma_loc_info_present_flag) {
        ni_put_bits(pb, 1, vui->chroma_loc_info_present_flag);
        ni_set_ue_golomb(pb, vui->chroma_sample_loc_type_top_field);
        ni_set_ue_golomb(pb, vui->chroma_sample_loc_type_bottom_field);
    } else
        ni_put_bits(pb, 1, vui->chroma_loc_info_present_flag);

    // IF chroma loc info
    // ENDIF

    ni_put_bits(pb, 1, vui->neutral_chroma_indication_flag);
    ni_put_bits(pb, 1, vui->field_seq_flag); // 0: frames, 1: fields
    ni_put_bits(pb, 1, vui->frame_field_info_present_flag);
    ni_put_bits(pb, 1, vui->default_display_window_flag);

    // IF default display window
    // ENDIF

    ni_put_bits(pb, 1, vui->vui_timing_info_present_flag);
    if (vui->vui_timing_info_present_flag) {
        ni_put_bits(pb, 32, vui->vui_num_units_in_tick);
        ni_put_bits(pb, 32, vui->vui_time_scale);

        ni_put_bits(pb, 1, vui->vui_poc_proportional_to_timing_flag);
        ni_put_bits(pb, 1, vui->vui_hrd_parameters_present_flag);
    }

    ni_put_bits(pb, 1, vui->bitstream_restriction_flag);
    if (vui->bitstream_restriction_flag) {
        ni_put_bits(pb, 1, vui->tiles_fixed_structure_flag);
        ni_put_bits(pb, 1, vui->motion_vectors_over_pic_boundaries_flag);
        ni_put_bits(pb, 1, vui->restricted_ref_pic_lists_flag);
        ni_set_ue_golomb(pb, vui->min_spatial_segmentation_idc);//  0, 4095);
        ni_set_ue_golomb(pb, vui->max_bytes_per_pic_denom);//       0, 16);
        ni_set_ue_golomb(pb, vui->max_bits_per_min_cu_denom);//     0, 16);
        ni_set_ue_golomb(pb, vui->log2_max_mv_length_horizontal);// 0, 16);
        ni_set_ue_golomb(pb, vui->log2_max_mv_length_vertical);//   0, 16);
    }

    // IF bitstream restriction
    // ENDIF
}

static void short_term_ref_pic_set(ni_bitstream_t *pb,
                                   const H265RawSTRefPicSet *p_st_rps,
                                   int st_rps_idx, const H265RawSPS *sps) {
    int i, ref_rps_idx, num_delta_pocs;
    const H265RawSTRefPicSet *ref;

    if (st_rps_idx > 0) {
        ni_put_bits(pb, 1, p_st_rps->inter_ref_pic_set_prediction_flag);
    }

    if (p_st_rps->inter_ref_pic_set_prediction_flag) {
        if (st_rps_idx == sps->num_short_term_ref_pic_sets) {
            ni_set_ue_golomb(pb, p_st_rps->delta_idx_minus1);
        }

        ref_rps_idx    = st_rps_idx - (p_st_rps->delta_idx_minus1 + 1);
        ref            = &sps->st_ref_pic_set[ref_rps_idx];
        num_delta_pocs = ref->num_negative_pics + ref->num_positive_pics;

        ni_put_bits(pb, 1, p_st_rps->delta_rps_sign);
        ni_set_ue_golomb(pb, p_st_rps->abs_delta_rps_minus1);

        for (i = 0; i <= num_delta_pocs; i++) {
            ni_put_bits(pb, 1, p_st_rps->used_by_curr_pic_flag[i]);
            if (!p_st_rps->used_by_curr_pic_flag[i]) {
                ni_put_bits(pb, 1, p_st_rps->use_delta_flag[i]);
            }
        }
    } else {
        ni_set_ue_golomb(pb, p_st_rps->num_negative_pics);
        ni_set_ue_golomb(pb, p_st_rps->num_positive_pics);

        for (i = 0; i < p_st_rps->num_negative_pics; i++) {
            ni_set_ue_golomb(pb, p_st_rps->delta_poc_s0_minus1[i]);
            ni_put_bits(pb, 1, p_st_rps->used_by_curr_pic_s0_flag[i]);
        }

        for (i = 0; i < p_st_rps->num_positive_pics; i++) {
            ni_set_ue_golomb(pb, p_st_rps->delta_poc_s1_minus1[i]);
            ni_put_bits(pb, 1, p_st_rps->used_by_curr_pic_s1_flag[i]);
        }
    }
}

static void write_raw_sps_extension(ni_bitstream_t *pb, const H265RawSPS *sps) {
    ni_put_bits(pb, 1, sps->sps_extension_present_flag);
    if (sps->sps_extension_present_flag) {
        ni_put_bits(pb, 1, sps->sps_range_extension_flag);
        ni_put_bits(pb, 1, sps->sps_multilayer_extension_flag);
        ni_put_bits(pb, 1, sps->sps_3d_extension_flag);
        ni_put_bits(pb, 1, sps->sps_scc_extension_flag);
        ni_put_bits(pb, 4, sps->sps_extension_4bits);
    }

    if (sps->sps_range_extension_flag) {
        ni_put_bits(pb, 1, sps->transform_skip_rotation_enabled_flag);
        ni_put_bits(pb, 1, sps->transform_skip_context_enabled_flag);
        ni_put_bits(pb, 1, sps->implicit_rdpcm_enabled_flag);
        ni_put_bits(pb, 1, sps->explicit_rdpcm_enabled_flag);
        ni_put_bits(pb, 1, sps->extended_precision_processing_flag);
        ni_put_bits(pb, 1, sps->intra_smoothing_disabled_flag);
        ni_put_bits(pb, 1, sps->high_precision_offsets_enabled_flag);
        ni_put_bits(pb, 1, sps->persistent_rice_adaptation_enabled_flag);
        ni_put_bits(pb, 1, sps->cabac_bypass_alignment_enabled_flag);
    }
    av_assert0(!sps->sps_multilayer_extension_flag);
    av_assert0(!sps->sps_3d_extension_flag);
    av_assert0(!sps->sps_scc_extension_flag);
    av_assert0(!sps->sps_extension_4bits);
}

int ni_hevc_encode_nal_sps(ni_bitstream_t *pb, const H265RawSPS *sps, int width,
                           int height) {
    int i, start;

    if (!sps) {
        return AVERROR(EINVAL);
    }

    // TODO: profile IDC and level IDC should be defined later on
    ni_put_bits(pb, 4, sps->sps_video_parameter_set_id);
    ni_put_bits(pb, 3, sps->sps_max_sub_layers_minus1);
    ni_put_bits(pb, 1, sps->sps_temporal_id_nesting_flag);

    write_raw_ptl(pb, &sps->profile_tier_level, sps->sps_max_sub_layers_minus1);

    ni_set_ue_golomb(pb, sps->sps_seq_parameter_set_id);
    ni_set_ue_golomb(pb, sps->chroma_format_idc);

    if (sps->chroma_format_idc == 3) {
        ni_put_bits(pb, 1, sps->separate_colour_plane_flag);
    }

    ni_set_ue_golomb(pb, width);
    ni_set_ue_golomb(pb, height);

    ni_put_bits(pb, 1, sps->conformance_window_flag);
    if (sps->conformance_window_flag) {
        ni_set_ue_golomb(pb, sps->conf_win_left_offset);
        ni_set_ue_golomb(pb, sps->conf_win_right_offset);
        ni_set_ue_golomb(pb, sps->conf_win_top_offset);
        ni_set_ue_golomb(pb, sps->conf_win_bottom_offset);
    }

    ni_set_ue_golomb(pb, sps->bit_depth_luma_minus8);
    ni_set_ue_golomb(pb, sps->bit_depth_chroma_minus8);
    ni_set_ue_golomb(pb, sps->log2_max_pic_order_cnt_lsb_minus4);

    ni_put_bits(pb, 1, sps->sps_sub_layer_ordering_info_present_flag);

    // for each layer
    start = sps->sps_sub_layer_ordering_info_present_flag
                ? 0
                : sps->sps_max_sub_layers_minus1;
    for (i = start; i < sps->sps_max_sub_layers_minus1 + 1; i++) {
        ni_set_ue_golomb(pb, sps->sps_max_dec_pic_buffering_minus1[i]);
        ni_set_ue_golomb(pb, sps->sps_max_num_reorder_pics[i]);
        ni_set_ue_golomb(pb, sps->sps_max_latency_increase_plus1[i]);
    }
    // end for

    ni_set_ue_golomb(pb, sps->log2_min_luma_coding_block_size_minus3);
    ni_set_ue_golomb(pb, sps->log2_diff_max_min_luma_coding_block_size);
    ni_set_ue_golomb(pb, sps->log2_min_luma_transform_block_size_minus2);
    ni_set_ue_golomb(pb, sps->log2_diff_max_min_luma_transform_block_size);
    ni_set_ue_golomb(pb, sps->max_transform_hierarchy_depth_inter);
    ni_set_ue_golomb(pb, sps->max_transform_hierarchy_depth_intra);

    // scaling list
    ni_put_bits(pb, 1, sps->scaling_list_enabled_flag);
    if (sps->scaling_list_enabled_flag) {
        // Signal scaling list data for custom lists
        ni_put_bits(pb, 1, sps->sps_scaling_list_data_present_flag);
        if (sps->sps_scaling_list_data_present_flag) {
            write_raw_scaling_list(pb, &sps->scaling_list);
        }
    }

    ni_put_bits(pb, 1, sps->amp_enabled_flag);
    ni_put_bits(pb, 1, sps->sample_adaptive_offset_enabled_flag);
    ni_put_bits(pb, 1, sps->pcm_enabled_flag);
    if (sps->pcm_enabled_flag) {
        ni_put_bits(pb, 4, sps->pcm_sample_bit_depth_luma_minus1);
        ni_put_bits(pb, 4, sps->pcm_sample_bit_depth_chroma_minus1);
        ni_set_ue_golomb(pb, sps->log2_min_pcm_luma_coding_block_size_minus3);
        ni_set_ue_golomb(pb, sps->log2_diff_max_min_pcm_luma_coding_block_size);
        ni_put_bits(pb, 1, sps->pcm_loop_filter_disabled_flag);
    }

    ni_set_ue_golomb(pb, sps->num_short_term_ref_pic_sets);
    for (i = 0; i < sps->num_short_term_ref_pic_sets; i++) {
        short_term_ref_pic_set(pb, &sps->st_ref_pic_set[i], i, sps);
    }

    // IF num short term ref pic sets
    // ENDIF

    av_assert0(!sps->long_term_ref_pics_present_flag);
    ni_put_bits(
        pb, 1,
        sps->long_term_ref_pics_present_flag); // long_term_ref_pics_present_flag
                                               // TODO(tyroun): netint not
                                               // encode long term ref yet

    // IF long_term_ref_pics_present
    // ENDIF

    ni_put_bits(pb, 1, sps->sps_temporal_mvp_enabled_flag);
    ni_put_bits(pb, 1, sps->strong_intra_smoothing_enabled_flag);
    ni_put_bits(pb, 1, sps->vui_parameters_present_flag);

    if (sps->vui_parameters_present_flag) {
        write_raw_VUI(pb, sps);
    }

    write_raw_sps_extension(pb, sps);

    ni_rbsp_trailing_bits(pb);

    return 0;
}

static void write_raw_pps_extension(ni_bitstream_t *pb, const H265RawPPS *pps) {
    ni_put_bits(pb, 1, pps->pps_extension_present_flag);
    if (pps->pps_extension_present_flag) {
        ni_put_bits(pb, 1, pps->pps_range_extension_flag);
        ni_put_bits(pb, 1, pps->pps_multilayer_extension_flag);
        ni_put_bits(pb, 1, pps->pps_3d_extension_flag);
        ni_put_bits(pb, 1, pps->pps_scc_extension_flag);
        ni_put_bits(pb, 4, pps->pps_extension_4bits);
    }
    av_assert0(!pps->pps_range_extension_flag);
    av_assert0(!pps->pps_multilayer_extension_flag);
    av_assert0(!pps->pps_3d_extension_flag);
    av_assert0(!pps->pps_scc_extension_flag);
    av_assert0(!pps->pps_extension_4bits);
}

/*
 * force_tile: 0 for ignoring this flag. 1 for disabling tile. 2 for enabling
 * tile.
 * */
int ni_hevc_encode_nal_pps(ni_bitstream_t *pb, const H265RawPPS *pps,
                           uint8_t force_tile, int columns, int rows) {
    int i;

    if (!pps || force_tile > 2) {
        return AVERROR(EINVAL);
    }

    ni_set_ue_golomb(pb, pps->pps_pic_parameter_set_id);
    ni_set_ue_golomb(pb, pps->pps_seq_parameter_set_id);
    ni_put_bits(pb, 1, 0); /* dependent_slice_segments_enabled_flag */
    ni_put_bits(pb, 1, pps->output_flag_present_flag);
    ni_put_bits(pb, 3, pps->num_extra_slice_header_bits);
    ni_put_bits(pb, 1, pps->sign_data_hiding_enabled_flag);
    ni_put_bits(pb, 1, pps->cabac_init_present_flag);

    ni_set_ue_golomb(pb, pps->num_ref_idx_l0_default_active_minus1);
    ni_set_ue_golomb(pb, pps->num_ref_idx_l1_default_active_minus1);

    // If tiles and slices = tiles is enabled, signal QP in the slice header.
    // Keeping the PPS constant for OMAF etc Keep QP constant here also if it
    // will be only set at CU level.
    ni_set_se_golomb(pb, pps->init_qp_minus26);

    ni_put_bits(pb, 1, pps->constrained_intra_pred_flag);
    ni_put_bits(pb, 1, pps->transform_skip_enabled_flag);
    ni_put_bits(pb, 1, pps->cu_qp_delta_enabled_flag);

    if (pps->cu_qp_delta_enabled_flag) {
        // Use separate QP for each LCU when rate control is enabled.
        ni_set_ue_golomb(pb, pps->diff_cu_qp_delta_depth);
    }

    ni_set_se_golomb(pb, pps->pps_cb_qp_offset);
    ni_set_se_golomb(pb, pps->pps_cr_qp_offset);
    ni_put_bits(pb, 1, pps->pps_slice_chroma_qp_offsets_present_flag);
    ni_put_bits(pb, 1, pps->weighted_pred_flag);
    ni_put_bits(pb, 1, pps->weighted_bipred_flag);
    ni_put_bits(pb, 1, pps->transquant_bypass_enabled_flag);
    ni_put_bits(
        pb, 1,
        !!((force_tile == 2) || (!force_tile && pps->tiles_enabled_flag)));
    // wavefronts
    ni_put_bits(pb, 1, pps->entropy_coding_sync_enabled_flag);

    if (force_tile) {
        if (force_tile == 2) {
            ni_set_ue_golomb(pb, columns - 1);
            ni_set_ue_golomb(pb, rows - 1);

            ni_put_bits(pb, 1, 1); // uniform_spacing_flag must be 1
            // loop_filter_across_tiles_enabled_flag must be 0
            ni_put_bits(pb, 1, pps->loop_filter_across_tiles_enabled_flag);
        }
    } else if (pps->tiles_enabled_flag) {
        ni_set_ue_golomb(pb, columns - 1);
        ni_set_ue_golomb(pb, rows - 1);

        ni_put_bits(pb, 1, pps->uniform_spacing_flag);

        if (!pps->uniform_spacing_flag) {
            for (i = 0; i < pps->num_tile_columns_minus1; ++i) {
                ni_set_ue_golomb(pb, pps->column_width_minus1[i]);
            }
            for (i = 0; i < pps->num_tile_rows_minus1; ++i) {
                ni_set_ue_golomb(pb, pps->row_height_minus1[i]);
            }
        }
        ni_put_bits(pb, 1, pps->loop_filter_across_tiles_enabled_flag);
    }

    ni_put_bits(pb, 1, pps->pps_loop_filter_across_slices_enabled_flag);
    ni_put_bits(pb, 1, pps->deblocking_filter_control_present_flag);

    if (pps->deblocking_filter_control_present_flag) {
        ni_put_bits(pb, 1, pps->deblocking_filter_override_enabled_flag);
        ni_put_bits(pb, 1, pps->pps_deblocking_filter_disabled_flag);
        if (!pps->pps_deblocking_filter_disabled_flag) {
            ni_set_se_golomb(pb, pps->pps_beta_offset_div2);
            ni_set_se_golomb(pb, pps->pps_tc_offset_div2);
        }
    }

    ni_put_bits(pb, 1, pps->pps_scaling_list_data_present_flag);
    av_assert0(!pps->pps_scaling_list_data_present_flag);

    ni_put_bits(pb, 1, pps->lists_modification_present_flag);
    ni_set_ue_golomb(pb, pps->log2_parallel_merge_level_minus2);
    ni_put_bits(pb, 1, pps->slice_segment_header_extension_present_flag);

    write_raw_pps_extension(pb, pps);

    ni_rbsp_trailing_bits(pb);

    return 0;
}

static void write_raw_slice_header_independent(ni_bitstream_t *pb,
                                               H265RawSliceHeader *slice,
                                               const H265RawSPS *sps,
                                               const H265RawPPS *pps) {
    const H265RawSTRefPicSet *rps;
    int i, idx_size, entry_size, num_pic_total_curr = 0;

    for (i = 0; i < pps->num_extra_slice_header_bits; i++) {
        ni_put_bits(pb, 1, 0); // slice_reserved_undetermined_flag
    }
    ni_set_ue_golomb(pb, slice->slice_type);

    av_assert0(!pps->output_flag_present_flag);
    av_assert0(!sps->separate_colour_plane_flag);

    if (slice->nal_unit_header.nal_unit_type != HEVC_NAL_IDR_W_RADL &&
        slice->nal_unit_header.nal_unit_type != HEVC_NAL_IDR_N_LP) {
        ni_put_bits(pb, sps->log2_max_pic_order_cnt_lsb_minus4 + 4,
                    slice->slice_pic_order_cnt_lsb);

        ni_put_bits(pb, 1, slice->short_term_ref_pic_set_sps_flag);
        if (!slice->short_term_ref_pic_set_sps_flag) {
            rps = &slice->short_term_ref_pic_set;
            short_term_ref_pic_set(pb, rps, sps->num_short_term_ref_pic_sets,
                                   sps);
        } else if (slice->short_term_ref_pic_set_sps_flag &&
                   sps->num_short_term_ref_pic_sets > 1) {
            idx_size = av_log2(sps->num_short_term_ref_pic_sets - 1) + 1;
            ni_put_bits(pb, idx_size, slice->short_term_ref_pic_set_idx);
            rps = &sps->st_ref_pic_set[slice->short_term_ref_pic_set_idx];
        } else {
            rps = &sps->st_ref_pic_set[0];
        }

        for (i = 0; i < rps->num_negative_pics; i++)
            if (rps->used_by_curr_pic_s0_flag[i])
                ++num_pic_total_curr;

        for (i = 0; i < rps->num_positive_pics; i++)
            if (rps->used_by_curr_pic_s1_flag[i])
                ++num_pic_total_curr;

        if (sps->sps_temporal_mvp_enabled_flag) {
            ni_put_bits(pb, 1, slice->slice_temporal_mvp_enabled_flag);
        }

        if (pps->pps_curr_pic_ref_enabled_flag) {
            ++num_pic_total_curr;
        }
    }

    if (sps->sample_adaptive_offset_enabled_flag) {
        ni_put_bits(pb, 1, slice->slice_sao_luma_flag);
        if (!sps->separate_colour_plane_flag && sps->chroma_format_idc > 0) {
            ni_put_bits(pb, 1, slice->slice_sao_chroma_flag);
        }
    }

    if (slice->slice_type == HEVC_SLICE_P ||
        slice->slice_type == HEVC_SLICE_B) {
        ni_put_bits(pb, 1, slice->num_ref_idx_active_override_flag);
        if (slice->num_ref_idx_active_override_flag) {
            ni_set_ue_golomb(pb, slice->num_ref_idx_l0_active_minus1);
            if (slice->slice_type == HEVC_SLICE_B) {
                ni_set_ue_golomb(pb, slice->num_ref_idx_l1_active_minus1);
            }
        }

        if (pps->lists_modification_present_flag && num_pic_total_curr > 1) {
            entry_size = av_log2(num_pic_total_curr - 1) + 1;

            ni_put_bits(pb, 1, slice->ref_pic_list_modification_flag_l0);
            if (slice->ref_pic_list_modification_flag_l0) {
                for (i = 0; i <= slice->num_ref_idx_l0_active_minus1; i++) {
                    ni_put_bits(pb, entry_size, slice->list_entry_l0[i]);
                }
            }

            if (slice->slice_type == HEVC_SLICE_B) {
                ni_put_bits(pb, 1, slice->ref_pic_list_modification_flag_l1);
                if (slice->ref_pic_list_modification_flag_l1) {
                    for (i = 0; i <= slice->num_ref_idx_l1_active_minus1; i++) {
                        ni_put_bits(pb, entry_size, slice->list_entry_l1[i]);
                    }
                }
            }
        }

        if (slice->slice_type == HEVC_SLICE_B) {
            ni_put_bits(pb, 1, slice->mvd_l1_zero_flag);
        }

        if (pps->cabac_init_present_flag) {
            ni_put_bits(pb, 1, slice->cabac_init_flag);
        }

        // Temporal Motion Vector Prediction flags
        if (slice->slice_temporal_mvp_enabled_flag) {
            if (slice->slice_type == HEVC_SLICE_B) {
                // Always use L0 for prediction
                ni_put_bits(pb, 1, slice->collocated_from_l0_flag);
            }

            if (slice->collocated_from_l0_flag) {
                if (slice->num_ref_idx_l0_active_minus1 > 0) {
                    ni_set_ue_golomb(pb, slice->collocated_ref_idx);
                }
            } else {
                if (slice->num_ref_idx_l1_active_minus1 > 0) {
                    ni_set_ue_golomb(pb, slice->collocated_ref_idx);
                }
            }
        }

        av_assert0(!pps->weighted_pred_flag);
        av_assert0(!pps->weighted_bipred_flag);

        ni_set_ue_golomb(pb, slice->five_minus_max_num_merge_cand);

        if (sps->motion_vector_resolution_control_idc == 2) {
            ni_put_bits(pb, 1, slice->use_integer_mv_flag);
        }
    }

    ni_set_se_golomb(pb, slice->slice_qp_delta);

    av_assert0(!pps->pps_slice_chroma_qp_offsets_present_flag);
    av_assert0(!pps->pps_slice_act_qp_offsets_present_flag);
    av_assert0(!pps->chroma_qp_offset_list_enabled_flag);
    av_assert0(!pps->deblocking_filter_override_enabled_flag);
    av_assert0(!slice->deblocking_filter_override_flag);

    if (pps->pps_loop_filter_across_slices_enabled_flag &&
        (slice->slice_sao_chroma_flag || slice->slice_sao_luma_flag ||
         !slice->slice_deblocking_filter_disabled_flag)) {
        ni_put_bits(pb, 1, slice->slice_loop_filter_across_slices_enabled_flag);
    }
}

/*
 * force_tile: 0 for ignoring this flag. 1 for disabling tile. 2 for enabling
 * tile.
 * */
int ni_hevc_encode_nal_slice_header(ni_bitstream_t *pb,
                                    H265RawSliceHeader *slice,
                                    const H265RawSPS *sps,
                                    const H265RawPPS *pps, int width,
                                    int height, uint8_t force_tile, int x,
                                    int y, int independent) {
    int i;
    int first_slice_segment_in_pic;
    int slice_segment_addr, len;
    int MinCbLog2SizeY;
    int CtbLog2SizeY;
    int CtbSizeY;
    int PicWidthInCtbsY;
    int PicHeightInCtbsY;
    int PicSizeInCtbsY;

    if (!pps || !slice || force_tile > 2) {
        return AVERROR(EINVAL);
    }

    first_slice_segment_in_pic = (x == 0 && y == 0) ? 1 : 0;
    ni_put_bits(pb, 1, first_slice_segment_in_pic);

    if (slice->nal_unit_header.nal_unit_type >= 16 &&
        slice->nal_unit_header.nal_unit_type <= 23) {
        ni_put_bits(pb, 1, slice->no_output_of_prior_pics_flag);
    }

    ni_set_ue_golomb(pb, slice->slice_pic_parameter_set_id);

    if (!first_slice_segment_in_pic) {
        MinCbLog2SizeY = sps->log2_min_luma_coding_block_size_minus3 + 3;
        CtbLog2SizeY =
            MinCbLog2SizeY + sps->log2_diff_max_min_luma_coding_block_size;
        //    int MinCbSizeY = 1 << MinCbLog2SizeY;
        CtbSizeY = 1 << CtbLog2SizeY;
        //    int PicWidthInMinCbsY = width / MinCbSizeY;
        PicWidthInCtbsY = (width + CtbSizeY - 1) / CtbSizeY;
        //    int PicHeightInMinCbsY = height / MinCbSizeY;
        PicHeightInCtbsY = (height + CtbSizeY - 1) / CtbSizeY;
        //    int PicSizeInMinCbsY = PicWidthInMinCbsY * PicHeightInMinCbsY;
        PicSizeInCtbsY = PicWidthInCtbsY * PicHeightInCtbsY;
        len            = av_ceil_log2_c(PicSizeInCtbsY);

        slice_segment_addr = y / CtbSizeY * PicWidthInCtbsY + x / CtbSizeY;

        ni_put_bits(pb, len, slice_segment_addr);
    }

    if (independent) {
        write_raw_slice_header_independent(pb, slice, sps, pps);
    }

    //if (pps->tiles_enabled_flag || pps->entropy_coding_sync_enabled_flag)
    //{
    if (force_tile == 2 || (!force_tile && pps->tiles_enabled_flag) ||
        pps->entropy_coding_sync_enabled_flag) {
        ni_set_ue_golomb(pb, slice->num_entry_point_offsets);
        if (slice->num_entry_point_offsets > 0) {
            ni_set_ue_golomb(pb, slice->offset_len_minus1);
            for (i = 0; i < slice->num_entry_point_offsets; i++) {
                ni_put_bits(pb, slice->offset_len_minus1 + 1,
                            slice->entry_point_offset_minus1[i]);
            }
        }
    }

    av_assert0(!pps->slice_segment_header_extension_present_flag);

    ni_rbsp_trailing_bits(pb);

    return 0;
}
