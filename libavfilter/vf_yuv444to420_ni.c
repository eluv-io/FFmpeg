/*
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
 * yuv444 to yuv420
 */

#ifndef __APPLE__
#include <stdio.h>
#include "libavutil/attributes.h"
#include "libavutil/avstring.h"
#include "libavutil/internal.h"
#include "libavutil/mem.h"
#include "libavutil/opt.h"

#include "avfilter.h"
#include "audio.h"
#include "filters.h"
#include "formats.h"
#include "internal.h"
#include "video.h"

#include <ni_device_api.h>

typedef struct TransContext {
    const AVClass *class;
    int nb_output0;
    int nb_output1;
    int mode;
} TransContext;

static av_cold int trans_init(AVFilterContext *ctx)
{
    int i, ret;

    for (i = 0; i < 2; i++) {
        AVFilterPad pad = { 0 };

        pad.type = ctx->filter->inputs[0].type;
        pad.name = av_asprintf("output%d", i);
        if (!pad.name)
            return AVERROR(ENOMEM);

#if (LIBAVFILTER_VERSION_MAJOR >= 8)
        if ((ret = ff_append_outpad(ctx, &pad)) < 0) {
#else
        if ((ret = ff_insert_outpad(ctx, i, &pad)) < 0) {
#endif
            av_freep(&pad.name);
            return ret;
        }
    }

    return 0;
}

static av_cold void trans_uninit(AVFilterContext *ctx)
{
    int i;

    for (i = 0; i < 2; i++)
        av_freep(&ctx->output_pads[i].name);
}

static int query_formats(AVFilterContext *ctx)
{
    AVFilterFormats *formats;
    int ret;
    enum AVPixelFormat input_pix_fmt = AV_PIX_FMT_YUV444P;
    enum AVPixelFormat output_pix_fmt = AV_PIX_FMT_YUV420P;

    if (ctx->inputs[0]) {
        formats = NULL;
        if ((ret = ff_add_format(&formats, input_pix_fmt)) < 0)
            return ret;
#if (LIBAVFILTER_VERSION_MAJOR >= 8 || LIBAVFILTER_VERSION_MAJOR >= 7 && LIBAVFILTER_VERSION_MINOR >= 110)
        if ((ret = ff_formats_ref(formats, &ctx->inputs[0]->outcfg.formats)) < 0)
#else
        if ((ret = ff_formats_ref(formats, &ctx->inputs[0]->out_formats)) < 0)
#endif
            return ret;
    }
    if (ctx->outputs[0]) {
        formats = NULL;

        if ((ret = ff_add_format(&formats, output_pix_fmt)) < 0)
            return ret;
#if (LIBAVFILTER_VERSION_MAJOR >= 8 || LIBAVFILTER_VERSION_MAJOR >= 7 && LIBAVFILTER_VERSION_MINOR >= 110)
        if ((ret = ff_formats_ref(formats, &ctx->inputs[0]->incfg.formats)) < 0)
#else
        if ((ret = ff_formats_ref(formats, &ctx->outputs[0]->in_formats)) < 0)
#endif
            return ret;
    }
    if (ctx->outputs[1]) {
        formats = NULL;

        if ((ret = ff_add_format(&formats, output_pix_fmt)) < 0)
            return ret;
#if (LIBAVFILTER_VERSION_MAJOR >= 8 || LIBAVFILTER_VERSION_MAJOR >= 7 && LIBAVFILTER_VERSION_MINOR >= 110)
        if ((ret = ff_formats_ref(formats, &ctx->inputs[0]->incfg.formats)) < 0)
#else
        if ((ret = ff_formats_ref(formats, &ctx->outputs[1]->in_formats)) < 0)
#endif
            return ret;
    }

    return 0;
}

static int filter_frame(AVFilterLink *inlink, AVFrame *frame)
{
    AVFilterContext *ctx = inlink->dst;
    TransContext *trans_ctx = inlink->dst->priv;
    AVFrame *out;
    int luma_size, ret;

    ////output1
    ctx->outputs[1]->format = AV_PIX_FMT_YUV420P;
    out = ff_get_video_buffer(ctx->outputs[1], ctx->outputs[1]->w, ctx->outputs[1]->h);
    if (!out) {
        av_frame_free(&frame);
	    return AVERROR(ENOMEM);
    }
    av_frame_copy_props(out, frame);
    out->format = ctx->outputs[1]->format;
    luma_size = out->width * out->height;

    if (trans_ctx->mode == 0) {
        memcpy(out->data[0], frame->data[1], sizeof(char) * luma_size);

        int uv_420_linesize = out->width / 2;
        int uv_444_linesize = out->width;

        for (int i = 0; i < out->height / 2; i++) {
            for (int j = 0; j < out->width / 2; j++) {
                memcpy(frame->data[1] + i * uv_420_linesize + j,
                       frame->data[2] + (2 * i * uv_444_linesize) + 2 * j,
                       sizeof(char));
                memcpy(frame->data[2] + i * uv_420_linesize + j,
                       frame->data[2] + (2 * i * uv_444_linesize) + (2 * j + 1),
                       sizeof(char));
                memcpy(out->data[1] + i * uv_420_linesize + j,
                       frame->data[2] + (((2 * i) + 1) * uv_444_linesize) +
                           (2 * j),
                       sizeof(char));
                memcpy(out->data[2] + i * uv_420_linesize + j,
                       frame->data[2] + (((2 * i) + 1) * uv_444_linesize) +
                           (2 * j + 1),
                       sizeof(char));
            }
        }
        ret = ff_filter_frame(ctx->outputs[1], out);
        if (ret)
            return ret;

        ////output0
        ctx->outputs[0]->format = AV_PIX_FMT_YUV420P;
        frame->format           = AV_PIX_FMT_YUV420P;
        frame->linesize[1]      = frame->linesize[1] / 2;
        frame->linesize[2]      = frame->linesize[2] / 2;

        ret = ff_filter_frame(ctx->outputs[0], frame);
    } else {

        int uv_420_linesize = out->width / 2;
        int uv_444_linesize = out->width;

        for (int i = 0; i < out->height / 2; i++) {
            for (int j = 0; j < out->width / 2; j++) {
                memcpy(out->data[1] + i * uv_420_linesize + j,
                       frame->data[1] + (2 * i * uv_444_linesize) + (2 * j + 1),
                       sizeof(char));
                memcpy(out->data[0] + 2 * i * uv_444_linesize + 2 * j,
                       frame->data[1] + (((2 * i) + 1) * uv_444_linesize) +
                           (2 * j),
                       sizeof(char) * 2);
                memcpy(frame->data[1] + i * uv_420_linesize + j,
                       frame->data[1] + (2 * i * uv_444_linesize) + 2 * j,
                       sizeof(char));

                memcpy(out->data[2] + i * uv_420_linesize + j,
                       frame->data[2] + (2 * i * uv_444_linesize) + (2 * j + 1),
                       sizeof(char));
                memcpy(out->data[0] + (2 * i + 1) * uv_444_linesize + 2 * j,
                       frame->data[2] + (((2 * i) + 1) * uv_444_linesize) +
                           (2 * j),
                       sizeof(char) * 2);
                memcpy(frame->data[2] + i * uv_420_linesize + j,
                       frame->data[2] + (2 * i * uv_444_linesize) + 2 * j,
                       sizeof(char));
            }
        }
        ret = ff_filter_frame(ctx->outputs[1], out);
        if (ret)
            return ret;

        ////output0
        ctx->outputs[0]->format = AV_PIX_FMT_YUV420P;
        frame->format           = AV_PIX_FMT_YUV420P;
        frame->linesize[1]      = frame->linesize[1] / 2;
        frame->linesize[2]      = frame->linesize[2] / 2;

        ret = ff_filter_frame(ctx->outputs[0], frame);
    }

    return ret;
}

#define OFFSET(x) offsetof(TransContext, x)
#define FLAGS (AV_OPT_FLAG_VIDEO_PARAM | AV_OPT_FLAG_FILTERING_PARAM)

static const AVOption options[] = {
    {"output0",
     "yuv420 of output0",
     OFFSET(nb_output0),
     AV_OPT_TYPE_INT,
     {.i64 = 0},
     0,
     INT_MAX,
     FLAGS},
    {"output1",
     "yuv420 of output1",
     OFFSET(nb_output1),
     AV_OPT_TYPE_INT,
     {.i64 = 0},
     0,
     INT_MAX,
     FLAGS},
    {"mode",
     "filter mode 0 have better PSNR 1 can decode as 420.",
     OFFSET(mode),
     AV_OPT_TYPE_INT,
     {.i64 = 0},
     0,
     1,
     FLAGS,
     "mode"},
    {NULL}};

#define trans_options options

AVFILTER_DEFINE_CLASS(trans);

static const AVFilterPad avfilter_vf_trans_inputs[] = {
    {
        .name         = "default",
        .type         = AVMEDIA_TYPE_VIDEO,
        .filter_frame = filter_frame,
    },
#if (LIBAVFILTER_VERSION_MAJOR < 8)
    { NULL }
#endif
};

AVFilter ff_vf_yuv444to420_ni_quadra = {
    .name          = "ni_quadra_yuv444to420",
    .description   = NULL_IF_CONFIG_SMALL("NetInt Quadra YUV444 to YUV420 v" NI_XCODER_REVISION),
    .priv_size     = sizeof(TransContext),
    .priv_class    = &trans_class,
    .init          = trans_init,
    .uninit        = trans_uninit,
#if (LIBAVFILTER_VERSION_MAJOR >= 8)
    FILTER_QUERY_FUNC(query_formats),
    FILTER_INPUTS(avfilter_vf_trans_inputs),
#else
    .query_formats = query_formats,
    .inputs        = avfilter_vf_trans_inputs,
#endif
    .flags         = AVFILTER_FLAG_DYNAMIC_OUTPUTS,
};
#else
#include "avfilter.h"
AVFilter ff_vf_yuv444to420_ni_quadra = {};
#endif
