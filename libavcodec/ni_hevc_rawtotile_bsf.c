/*
 * NetInt HEVC raw-to-tile BSF common source code
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
 * This bitstream filter re-encode HEVC NALUs (i.e. VPS, SPS, PPS and slice
 * header) with tile flags. The slice segment address will be assigned in the
 * slices or tiles respectively so as to repack them later.
 */

#include <libavutil/opt.h>
#include <libavutil/internal.h>
#include <math.h>

#include "avcodec.h"
#if (LIBAVCODEC_VERSION_MAJOR >= 59 || LIBAVCODEC_VERSION_MAJOR >= 58 && LIBAVCODEC_VERSION_MINOR >= 91)
#include "bsf_internal.h"
#else
#include "bsf.h"
#endif
#include "hevc.h"
#include "cbs.h"
#include "cbs_h265.h"
#include "ni_hevc_rbsp.h"


typedef struct HEVCFtoTileContext {
    AVPacket *buffer_pkt;
    CodedBitstreamContext *cbc;
    CodedBitstreamFragment temporal_unit;

    int width;
    int height;
    int column; //total tile number in column
    int row;    //total tile number in row
    int x;
    int y;

    ni_bitstream_t stream;
} HEVCFtoTileContext;


static int hevc_rawtotile_encode_vps(HEVCFtoTileContext *s, AVBSFContext *ctx)
{
    CodedBitstreamH265Context *priv = s->cbc->priv_data;
    ni_write_nal_header(&s->stream, HEVC_NAL_VPS, 0, 1);
    return ni_hevc_encode_nal_vps(&s->stream, priv->active_vps);
}

static int hevc_rawtotile_encode_sps(HEVCFtoTileContext *s, AVBSFContext *ctx)
{
    CodedBitstreamH265Context *priv = s->cbc->priv_data;
    ni_write_nal_header(&s->stream, HEVC_NAL_SPS, 0, 1);
    return ni_hevc_encode_nal_sps(&s->stream, priv->active_sps, s->width, s->height);
}

static int hevc_rawtotile_encode_pps(HEVCFtoTileContext *s, AVBSFContext *ctx)
{
    CodedBitstreamH265Context *priv = s->cbc->priv_data;
    ni_write_nal_header(&s->stream, HEVC_NAL_PPS, 0, 1);
    return ni_hevc_encode_nal_pps(&s->stream, priv->active_pps, 2, s->column, s->row);
}

//TODO(tyroun): one extra memcpy for slice data, try to eliminate it
static int hevc_rawtotile_slice(HEVCFtoTileContext *s, AVBSFContext *ctx, CodedBitstreamUnit *unit)
{
    int i;
    H265RawSlice *slice = unit->content;
    CodedBitstreamH265Context *priv = s->cbc->priv_data;

    ni_write_nal_header(&s->stream, slice->header.nal_unit_header.nal_unit_type, 0, 1);

    ni_hevc_encode_nal_slice_header(&s->stream, &slice->header, priv->active_sps, priv->active_pps,
                                       s->width /* width */, s->height /* height  */, 2 /* enable tile */,
                                       s->x, s->y, 1 /* independent */);

    for (i = 0; i < slice->data_size; i++) {
        ni_put_bits(&s->stream, 8, slice->data[i]);
    }

    return 0;
}

static int hevc_rawtotile_filter(AVBSFContext *ctx, AVPacket *out)
{
    HEVCFtoTileContext *s = ctx->priv_data;
    CodedBitstreamFragment *td = &s->temporal_unit;
    int i, ret, new_size, nb_slices, unit_offset;

    ret = ff_bsf_get_packet_ref(ctx, s->buffer_pkt);
    if (ret < 0) {
        // EOF
        return ret;
    }

    ret = ff_cbs_read_packet(s->cbc, td, s->buffer_pkt);
    if (ret < 0) {
        av_log(ctx, AV_LOG_WARNING, "Failed to parse temporal unit.\n");
        goto passthrough;
    }

    nb_slices = 0;
    unit_offset = 0;
    for (i = 0; i < td->nb_units; i++) {
        CodedBitstreamUnit *unit = &td->units[i];

        if (unit->type == HEVC_NAL_VPS) {
            ret = hevc_rawtotile_encode_vps(s, ctx);
            if (ret < 0) {
                av_log(ctx, AV_LOG_ERROR, "failed to re-encode vps\n");
                av_assert0(0);
            }
        } else if (unit->type == HEVC_NAL_SPS) {
            ret = hevc_rawtotile_encode_sps(s, ctx);
            if (ret < 0) {
                av_log(ctx, AV_LOG_ERROR, "failed to re-encode sps\n");
                av_assert0(0);
            }
        } else if (unit->type == HEVC_NAL_PPS) {
            ret = hevc_rawtotile_encode_pps(s, ctx);
            if (ret < 0) {
                av_log(ctx, AV_LOG_ERROR, "failed to re-encode pps\n");
                av_assert0(0);
            }
        } else if (unit->type <= HEVC_NAL_RSV_VCL31) {
            if (nb_slices == 0) {
                unit_offset = i;
            }
            nb_slices++;
        }
    }

    if (nb_slices == 0) {
        ret = AVERROR(EAGAIN);
        goto end;
    }

    for (i = 0; i < nb_slices; i++) {
        CodedBitstreamUnit *unit = &td->units[i + unit_offset];
        if (unit->type <= HEVC_NAL_RSV_VCL31) {
            ret = hevc_rawtotile_slice(s, ctx, unit);
            if (ret < 0) {
                av_log(ctx, AV_LOG_ERROR, "failed to re-encode slice header\n");
                av_assert0(0);
            }
            break;
        }
    }

    new_size = ni_bitstream_count(&s->stream) / 8;
    ret = av_new_packet(out, new_size);
    if (ret < 0) {
        return ret;
    }

    av_packet_copy_props(out, s->buffer_pkt);

    ni_bitstream_fetch(&s->stream, out->data, new_size);
    ni_bitstream_reset(&s->stream);

end:
    av_packet_unref(s->buffer_pkt);
#if (LIBAVCODEC_VERSION_MAJOR >= 59 || LIBAVCODEC_VERSION_MAJOR >= 58 && LIBAVCODEC_VERSION_MINOR >= 134)
    ff_cbs_fragment_reset(td);
#elif (LIBAVCODEC_VERSION_MAJOR >= 58) && (LIBAVCODEC_VERSION_MINOR >= 54)
    ff_cbs_fragment_reset(s->cbc, td);
#else
    ff_cbs_fragment_uninit(s->cbc, td);
#endif
    return ret;

passthrough:
    av_packet_move_ref(out, s->buffer_pkt);
#if (LIBAVCODEC_VERSION_MAJOR >= 59 || LIBAVCODEC_VERSION_MAJOR >= 58 && LIBAVCODEC_VERSION_MINOR >= 134)
    ff_cbs_fragment_reset(td);
#elif (LIBAVCODEC_VERSION_MAJOR >= 58 && LIBAVCODEC_VERSION_MINOR >= 54)
    ff_cbs_fragment_reset(s->cbc, td);
#else
    ff_cbs_fragment_uninit(s->cbc, td);
#endif
    return 0;
}

static int hevc_rawtotile_init(AVBSFContext *ctx)
{
    HEVCFtoTileContext *s = ctx->priv_data;
    CodedBitstreamFragment *td = &s->temporal_unit;
    int ret;

    s->buffer_pkt = av_packet_alloc();
    if (!s->buffer_pkt)
        return AVERROR(ENOMEM);

    ret = ff_cbs_init(&s->cbc, AV_CODEC_ID_HEVC, ctx);
    if (ret < 0)
        return ret;

    s->cbc->decompose_unit_types    = NULL;
    s->cbc->nb_decompose_unit_types = 0;

//    s->cbc->decompose_unit_types    = (CodedBitstreamUnitType*)decompose_unit_types;
//    s->cbc->nb_decompose_unit_types = FF_ARRAY_ELEMS(decompose_unit_types);

    /* extradata is a must */
//    if (!ctx->par_in->extradata_size)
//        return AVERROR(ENODEV);

//    ret = ff_cbs_read_extradata(s->cbc, td, ctx->par_in);
//    if (ret < 0) {
//        av_log(ctx, AV_LOG_ERROR, "Failed to parse extradata.\n");
//        return ret;
//    }

#if (LIBAVCODEC_VERSION_MAJOR >= 59 || LIBAVCODEC_VERSION_MAJOR >= 58 && LIBAVCODEC_VERSION_MINOR >= 134)
    ff_cbs_fragment_reset(td);
#elif (LIBAVCODEC_VERSION_MAJOR >= 58 && LIBAVCODEC_VERSION_MINOR >= 54)
    ff_cbs_fragment_reset(s->cbc, td);
#else
    ff_cbs_fragment_uninit(s->cbc, td);
#endif

    ni_bitstream_init(&s->stream);

    return 0;
}

static void hevc_rawtotile_flush(AVBSFContext *ctx)
{
    HEVCFtoTileContext *s = ctx->priv_data;

    ni_bitstream_reset(&s->stream);
    av_packet_unref(s->buffer_pkt);
#if (LIBAVCODEC_VERSION_MAJOR >= 59 || LIBAVCODEC_VERSION_MAJOR >= 58 && LIBAVCODEC_VERSION_MINOR >= 134)
    ff_cbs_fragment_reset(&s->temporal_unit);
#elif (LIBAVCODEC_VERSION_MAJOR >= 58 && LIBAVCODEC_VERSION_MINOR >= 54)
    ff_cbs_fragment_reset(s->cbc, &s->temporal_unit);
#else
    ff_cbs_fragment_uninit(s->cbc, &s->temporal_unit);
#endif
}

static void hevc_rawtotile_close(AVBSFContext *ctx)
{
    HEVCFtoTileContext *s = ctx->priv_data;

    ni_bitstream_deinit(&s->stream);
    av_packet_free(&s->buffer_pkt);
#if (LIBAVCODEC_VERSION_MAJOR >= 59 || LIBAVCODEC_VERSION_MAJOR >= 58 && LIBAVCODEC_VERSION_MINOR >= 134)
    ff_cbs_fragment_free(&s->temporal_unit);
#elif (LIBAVCODEC_VERSION_MAJOR >= 58 && LIBAVCODEC_VERSION_MINOR >= 54)
    ff_cbs_fragment_free(s->cbc, &s->temporal_unit);
#else
    ff_cbs_fragment_uninit(s->cbc, &s->temporal_unit);
#endif
    ff_cbs_close(&s->cbc);
}

static const enum AVCodecID hevc_rawtotile_codec_ids[] = {
        AV_CODEC_ID_HEVC, AV_CODEC_ID_NONE,
};

#define OFFSET(x) offsetof(HEVCFtoTileContext, x)

static const AVOption options[] = {
        {"width", NULL, OFFSET(width), AV_OPT_TYPE_INT, {.i64 = 1280}, 0, 8192, 0, 0},
        {"height", NULL, OFFSET(height), AV_OPT_TYPE_INT, {.i64 = 720}, 0, 8192, 0, 0},
        {"column", NULL, OFFSET(column), AV_OPT_TYPE_INT, {.i64 = 0}, 0, 128, 0, 0},  //support 128 columns max
        {"row", NULL, OFFSET(row), AV_OPT_TYPE_INT, {.i64 = 0}, 0, 128, 0, 0},  //support 128 rows max
        {"x", NULL, OFFSET(x), AV_OPT_TYPE_INT, {.i64 = 0}, 0, 8192, 0, 0},  //support 8192 columns max
        {"y", NULL, OFFSET(y), AV_OPT_TYPE_INT, {.i64 = 0}, 0, 8192, 0, 0},  //support 8192 rows max
        { NULL },
};

static const AVClass hevc_rawtotile_class = {
        .class_name = "hevc_rawtotile",
        .item_name  = av_default_item_name,
        .option     = options,
        .version    = LIBAVUTIL_VERSION_INT,
};

const AVBitStreamFilter ff_hevc_rawtotile_bsf = {
        .name           = "hevc_rawtotile",
        .priv_data_size = sizeof(HEVCFtoTileContext),
        .priv_class     = &hevc_rawtotile_class,
        .init           = hevc_rawtotile_init,
        .flush          = hevc_rawtotile_flush,
        .close          = hevc_rawtotile_close,
        .filter         = hevc_rawtotile_filter,
        .codec_ids      = hevc_rawtotile_codec_ids,
};
