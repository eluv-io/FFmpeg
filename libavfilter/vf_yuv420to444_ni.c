/*
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
 * yuv420 to yuv444
 */

#ifndef __APPLE__
#include "avfilter.h"
#include "formats.h"
#include "libavutil/common.h"
#include "libavutil/eval.h"
#include "libavutil/avstring.h"
#include "libavutil/pixdesc.h"
#include "libavutil/imgutils.h"
#include "libavutil/mathematics.h"
#include "libavutil/opt.h"
#include "libavutil/timestamp.h"
#include "internal.h"
#include "drawutils.h"
#include "framesync.h"
#include "video.h"

#include <ni_device_api.h>

typedef struct YUVTransContext {
    const AVClass *class;
    FFFrameSync fs;
    int mode;
} YUVTransContext;

static av_cold void uninit(AVFilterContext *ctx)
{
    YUVTransContext *s = ctx->priv;

    ff_framesync_uninit(&s->fs);
}

static int query_formats(AVFilterContext *ctx)
{
    AVFilterFormats *formats;
    int ret;
    enum AVPixelFormat input_pix_fmt = AV_PIX_FMT_YUV420P;
    enum AVPixelFormat output_pix_fmt = AV_PIX_FMT_YUV444P;

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
    if (ctx->inputs[1]) {
        formats = NULL;
        if ((ret = ff_add_format(&formats, input_pix_fmt)) < 0)
            return ret;
#if (LIBAVFILTER_VERSION_MAJOR >= 8 || LIBAVFILTER_VERSION_MAJOR >= 7 && LIBAVFILTER_VERSION_MINOR >= 110)
        if ((ret = ff_formats_ref(formats, &ctx->inputs[1]->outcfg.formats)) < 0)
#else
        if ((ret = ff_formats_ref(formats, &ctx->inputs[1]->out_formats)) < 0)
#endif
            return ret;
    }
    if (ctx->outputs[0]) {
        formats = NULL;

        if ((ret = ff_add_format(&formats, output_pix_fmt)) < 0)
            return ret;
#if (LIBAVFILTER_VERSION_MAJOR >= 8 || LIBAVFILTER_VERSION_MAJOR >= 7 && LIBAVFILTER_VERSION_MINOR >= 110)
        if ((ret = ff_formats_ref(formats, &ctx->outputs[0]->incfg.formats)) < 0)
#else
        if ((ret = ff_formats_ref(formats, &ctx->outputs[0]->in_formats)) < 0)
#endif
            return ret;
    }

    return 0;
}

static int config_output(AVFilterLink *outlink)
{
    AVFilterContext *ctx = outlink->src;
    YUVTransContext *s = ctx->priv;
    int ret;

    if ((ret = ff_framesync_init_dualinput(&s->fs, ctx)) < 0)
        return ret;

    outlink->w = ctx->inputs[0]->w;
    outlink->h = ctx->inputs[0]->h;
    outlink->format = AV_PIX_FMT_YUV444P;
    outlink->time_base = ctx->inputs[0]->time_base;
    av_log(ctx, AV_LOG_INFO,
           "output w:%d h:%d fmt:%s \n",
           outlink->w, outlink->h,
           av_get_pix_fmt_name(outlink->format));

    return ff_framesync_configure(&s->fs);
}

static int do_blend(FFFrameSync *fs)
{
    AVFilterContext *ctx = fs->parent;
    YUVTransContext *trans_ctx = fs->parent->priv;
    AVFrame *mainpic, *second, *out;
    int luma_size;

    ff_framesync_get_frame(fs, 0, &mainpic, 0);
    ff_framesync_get_frame(fs, 1, &second, 0);

    mainpic->pts =
        av_rescale_q(fs->pts, fs->time_base, ctx->outputs[0]->time_base);
    {
        //allocate a new buffer, data is null
        out = ff_get_video_buffer(ctx->outputs[0], ctx->outputs[0]->w, ctx->outputs[0]->h);
        if (!out) {
            return AVERROR(ENOMEM);
        }

        av_frame_copy_props(out, mainpic);
        out->format = ctx->outputs[0]->format;

        luma_size = out->width * out->height;

        if (trans_ctx->mode == 0) {
            // y compnent
            if (mainpic->linesize[0] == out->linesize[0]) {
                memcpy(out->data[0], mainpic->data[0],
                       sizeof(char) * luma_size);
            } else {
                for (int i = 0; i < out->height; i++) {
                    memcpy(out->data[0] + i * out->linesize[0],
                           mainpic->data[0] + i * mainpic->linesize[0],
                           out->linesize[0]);
                }
            }

            //u compnent
            if (second->linesize[0] == out->linesize[1]) {
                memcpy(out->data[1], second->data[0], sizeof(char) * luma_size);
            } else {
                for (int i = 0; i < out->height; i++) {
                    memcpy(out->data[1] + i * out->linesize[0],
                           second->data[0] + i * second->linesize[0],
                           out->linesize[0]);
                }
            }

            //v compnent
            if (mainpic->linesize[1] == out->linesize[1] / 2) {
                int uv_420_linesize = out->width / 2;
                int uv_444_linesize = out->width;

                for (int i = 0; i < out->height / 2; i++) {
                    for (int j = 0; j < out->width / 2; j++) {
                        memcpy(out->data[2] + (2 * i * uv_444_linesize) + 2 * j,
                               mainpic->data[1] + i * uv_420_linesize + j,
                               sizeof(char));
                        memcpy(out->data[2] + 2 * (i * uv_444_linesize) +
                                   (2 * j + 1),
                               mainpic->data[2] + i * uv_420_linesize + j,
                               sizeof(char));
                        memcpy(out->data[2] + ((2 * i + 1) * uv_444_linesize) +
                                   2 * j,
                               second->data[1] + i * uv_420_linesize + j,
                               sizeof(char));
                        memcpy(out->data[2] + ((2 * i + 1) * uv_444_linesize) +
                                   (2 * j + 1),
                               second->data[2] + i * uv_420_linesize + j,
                               sizeof(char));
                    }
                }
            }
        } else if (trans_ctx->mode == 1) {
            // y compnent
            if (mainpic->linesize[0] == out->linesize[0]) {
                memcpy(out->data[0], mainpic->data[0],
                       sizeof(char) * luma_size);
            } else {
                for (int i = 0; i < out->height; i++) {
                    memcpy(out->data[0] + i * out->linesize[0],
                           mainpic->data[0] + i * mainpic->linesize[0],
                           out->linesize[0]);
                }
            }

            // uv compnent
            if (mainpic->linesize[1] == out->linesize[1] / 2) {
                int uv_420_linesize = out->width / 2;
                int uv_444_linesize = out->width;

                for (int i = 0; i < out->height / 2; i++) {
                    for (int j = 0; j < out->width / 2; j++) {
                        memcpy(out->data[1] + (2 * i * uv_444_linesize) + 2 * j,
                               mainpic->data[1] + i * uv_420_linesize + j,
                               sizeof(char));
                        memcpy(out->data[1] + (2 * i * uv_444_linesize) +
                                   (2 * j + 1),
                               second->data[1] + i * uv_420_linesize + j,
                               sizeof(char));
                        memcpy(out->data[1] + ((2 * i + 1) * uv_444_linesize) +
                                   2 * j,
                               second->data[0] + 2 * i * uv_444_linesize +
                                   2 * j,
                               sizeof(char) * 2);

                        memcpy(out->data[2] + (2 * i * uv_444_linesize) + 2 * j,
                               mainpic->data[2] + i * uv_420_linesize + j,
                               sizeof(char));
                        memcpy(out->data[2] + 2 * (i * uv_444_linesize) +
                                   (2 * j + 1),
                               second->data[2] + i * uv_420_linesize + j,
                               sizeof(char));
                        memcpy(out->data[2] + ((2 * i + 1) * uv_444_linesize) +
                                   2 * j,
                               second->data[0] + (2 * i + 1) * uv_444_linesize +
                                   2 * j,
                               sizeof(char) * 2);
                    }
                }
            }
        }
    }

    return ff_filter_frame(ctx->outputs[0], out);
}

static av_cold int init(AVFilterContext *ctx)
{
    YUVTransContext *s = ctx->priv;

    s->fs.on_event = do_blend;
    s->fs.opt_shortest = 1;//force termination when the shortest input terminates
    s->fs.opt_eof_action = EOF_ACTION_ENDALL;

    return 0;
}

static int activate(AVFilterContext *ctx)
{
    YUVTransContext *s = ctx->priv;

    return ff_framesync_activate(&s->fs);
}

#define OFFSET(x) offsetof(YUVTransContext, x)
#define FLAGS (AV_OPT_FLAG_VIDEO_PARAM|AV_OPT_FLAG_FILTERING_PARAM)

static const AVOption YUVTrans_options[] = {
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

// NOLINTNEXTLINE(clang-diagnostic-deprecated-declarations)
FRAMESYNC_DEFINE_CLASS(YUVTrans, YUVTransContext, fs);

static const AVFilterPad avfilter_vf_YUVTrans_inputs[] = {
    {
        .name         = "input0",
        .type         = AVMEDIA_TYPE_VIDEO,
    },
    {
        .name         = "input1",
        .type         = AVMEDIA_TYPE_VIDEO,
    },
#if (LIBAVFILTER_VERSION_MAJOR < 8)
    { NULL }
#endif
};

static const AVFilterPad avfilter_vf_YUVTrans_outputs[] = {
    {
        .name          = "default",
        .type          = AVMEDIA_TYPE_VIDEO,
        .config_props  = config_output,
    },
#if (LIBAVFILTER_VERSION_MAJOR < 8)
    { NULL }
#endif
};

AVFilter ff_vf_yuv420to444_ni_quadra = {
    .name          = "ni_quadra_yuv420to444",
    .description   = NULL_IF_CONFIG_SMALL("NetInt Quadra YUV420 to YUV444 v" NI_XCODER_REVISION),
    .preinit       = YUVTrans_framesync_preinit,
    .init          = init,
    .uninit        = uninit,
    .priv_size     = sizeof(YUVTransContext),
    .priv_class    = &YUVTrans_class,
    .activate      = activate,
#if (LIBAVFILTER_VERSION_MAJOR >= 8)
    FILTER_INPUTS(avfilter_vf_YUVTrans_inputs),
    FILTER_OUTPUTS(avfilter_vf_YUVTrans_outputs),
    FILTER_QUERY_FUNC(query_formats),
#else
    .inputs        = avfilter_vf_YUVTrans_inputs,
    .outputs       = avfilter_vf_YUVTrans_outputs,
    .query_formats = query_formats,
#endif
    .flags         = AVFILTER_FLAG_SUPPORT_TIMELINE_INTERNAL |
                     AVFILTER_FLAG_SLICE_THREADS,
};
#else
#include "avfilter.h"
AVFilter ff_vf_yuv420to444_ni_quadra = {};
#endif
