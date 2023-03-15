/*
 * NetInt HEVC tile bitstream filter common source code
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
 * Extract tile rows and columns number from HEVC AVPacket extra-data using
 * FFmpeg coded bitstream APIs.
 */

#include "config.h"

#include "libavutil/avassert.h"

#include "avcodec.h"
#include "bsf.h"
#include "cbs.h"
#include "cbs_h265.h"

#include "ni_hevc_extradata.h"

int av_hevc_extract_tiles_from_extradata(uint8_t *extradata,
                                         size_t extradata_size, int *tile_row,
                                         int *tile_col) {
#if CONFIG_HEVC_FRAME_SPLIT_BSF
    int i, ret;
    AVCodecParameters par_in;
    CodedBitstreamContext *cbc;
    CodedBitstreamFragment td;
    CodedBitstreamUnit *unit;
    H265RawPPS *pps;

    if (!extradata || !tile_row || !tile_col) {
        ret = AVERROR(EINVAL);
        av_log(NULL, AV_LOG_ERROR, "invalid arguments\n");
        return ret;
    }

    ret = ff_cbs_init(&cbc, AV_CODEC_ID_HEVC, NULL);
    if (ret < 0) {
        av_log(NULL, AV_LOG_ERROR, "failed to initialize cbc\n");
        return ret;
    }

    memset(&par_in, 0, sizeof(AVCodecParameters));
    par_in.extradata      = extradata;
    par_in.extradata_size = extradata_size;

    memset(&td, 0, sizeof(CodedBitstreamFragment));

    ret = ff_cbs_read_extradata(cbc, &td, &par_in);
    if (ret < 0) {
        av_log(NULL, AV_LOG_ERROR, "failed to read extradata\n");
        goto out;
    }

    for (i = 0; i < td.nb_units; i++) {
        unit = &td.units[i];
        if (unit->type == HEVC_NAL_PPS) {
            pps = (H265RawPPS *)unit->content;
            *tile_row =
                pps->tiles_enabled_flag ? pps->num_tile_rows_minus1 + 1 : 1;
            *tile_col =
                pps->tiles_enabled_flag ? pps->num_tile_columns_minus1 + 1 : 1;
            break;
        }
    }

out:
#if (LIBAVCODEC_VERSION_MAJOR >= 59 || LIBAVCODEC_VERSION_MAJOR >= 58 && LIBAVCODEC_VERSION_MINOR >= 134)
    ff_cbs_fragment_free(&td);
#elif (LIBAVCODEC_VERSION_MAJOR >= 58) && (LIBAVCODEC_VERSION_MINOR >= 54)
    ff_cbs_fragment_free(cbc, &td);
#else
    ff_cbs_fragment_uninit(cbc, &td);
#endif
    ff_cbs_close(&cbc);
    return ret;
#else
    return AVERROR(ENOSYS);
#endif
}
