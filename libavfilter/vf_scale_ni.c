/*
 * Copyright (c) 2007 Bobby Bingham
 * Copyright (c) 2020 NetInt
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
 * scale video filter
 */

#ifndef __APPLE__
#include <stdio.h>
#include <string.h>

#include "avfilter.h"
#include "formats.h"
#include "internal.h"

 // Needed for FFmpeg-n4.3+
#if (LIBAVFILTER_VERSION_MAJOR >= 8 || LIBAVFILTER_VERSION_MAJOR >= 7 && LIBAVFILTER_VERSION_MINOR >= 85)
#include "scale_eval.h"
#else
#include "scale.h"
#endif
#include "video.h"
#include "libavutil/avstring.h"
#include "libavutil/internal.h"
#include "libavutil/mathematics.h"
#include "libavutil/opt.h"
#include "libavutil/parseutils.h"
#include "libavutil/pixdesc.h"
#include "libavutil/imgutils.h"
#include "libavutil/avassert.h"
#include "libswscale/swscale.h"
#include "nifilter.h"

enum OutputFormat {
    OUTPUT_FORMAT_YUV420P,
    OUTPUT_FORMAT_YUYV422,
    OUTPUT_FORMAT_UYVY422,
    OUTPUT_FORMAT_NV12,
    OUTPUT_FORMAT_ARGB,
    OUTPUT_FORMAT_RGBA,
    OUTPUT_FORMAT_ABGR,
    OUTPUT_FORMAT_BGRA,
    OUTPUT_FORMAT_YUV420P10LE,
    OUTPUT_FORMAT_NV16,
    OUTPUT_FORMAT_BGR0,
    OUTPUT_FORMAT_P010LE,
    OUTPUT_FORMAT_BGRP,
    OUTPUT_FORMAT_AUTO,
    OUTPUT_FORMAT_NB
};

enum AVPixelFormat ff_output_fmt[] = {
    AV_PIX_FMT_YUV420P, AV_PIX_FMT_YUYV422, AV_PIX_FMT_UYVY422,
    AV_PIX_FMT_NV12,    AV_PIX_FMT_ARGB,    AV_PIX_FMT_RGBA,
    AV_PIX_FMT_ABGR,    AV_PIX_FMT_BGRA,    AV_PIX_FMT_YUV420P10LE,
    AV_PIX_FMT_NV16,    AV_PIX_FMT_BGR0,    AV_PIX_FMT_P010LE,
    AV_PIX_FMT_BGRP};

typedef struct NetIntScaleContext {
    const AVClass *class;
    AVDictionary *opts;

    /**
     * New dimensions. Special values are:
     *   0 = original width/height
     *  -1 = keep original aspect
     *  -N = try to keep aspect but make sure it is divisible by N
     */
    int w, h;
    char *size_str;

    char *w_expr;               ///< width  expression string
    char *h_expr;               ///< height expression string

    int force_original_aspect_ratio;
    int force_divisible_by;
    int format;

    enum AVPixelFormat out_format;
    AVBufferRef *out_frames_ref;

    ni_session_context_t api_ctx;
    ni_session_data_io_t api_dst_frame;
    ni_scaler_params_t params;

    int initialized;
    int session_opened;
    int keep_alive_timeout; /* keep alive timeout setting */
} NetIntScaleContext;

AVFilter ff_vf_scale_ni;

static av_cold int init_dict(AVFilterContext *ctx, AVDictionary **opts)
{
    NetIntScaleContext *scale = ctx->priv;

    if (scale->size_str && (scale->w_expr || scale->h_expr)) {
        av_log(ctx, AV_LOG_ERROR,
               "Size and width/height expressions cannot be set at the same time.\n");
            return AVERROR(EINVAL);
    }

    if (scale->w_expr && !scale->h_expr)
        FFSWAP(char *, scale->w_expr, scale->size_str);

    if (scale->size_str) {
        char buf[32];
        int ret;

        if ((ret = av_parse_video_size(&scale->w, &scale->h, scale->size_str)) < 0) {
            av_log(ctx, AV_LOG_ERROR,
                   "Invalid size '%s'\n", scale->size_str);
            return ret;
        }
        snprintf(buf, sizeof(buf)-1, "%d", scale->w);
        av_opt_set(scale, "w", buf, 0);
        snprintf(buf, sizeof(buf)-1, "%d", scale->h);
        av_opt_set(scale, "h", buf, 0);
    }
    if (!scale->w_expr)
        av_opt_set(scale, "w", "iw", 0);
    if (!scale->h_expr)
        av_opt_set(scale, "h", "ih", 0);

    av_log(ctx, AV_LOG_VERBOSE, "w:%s h:%s\n", scale->w_expr, scale->h_expr);

    scale->opts = *opts;
    *opts = NULL;

    return 0;
}

static av_cold void uninit(AVFilterContext *ctx)
{
    NetIntScaleContext *scale = ctx->priv;

    av_dict_free(&scale->opts);

    if (scale->api_dst_frame.data.frame.p_buffer)
        ni_frame_buffer_free(&scale->api_dst_frame.data.frame);

    if (scale->session_opened) {
        /* Close operation will free the device frames */
        ni_device_session_close(&scale->api_ctx, 1, NI_DEVICE_TYPE_SCALER);
        ni_device_session_context_clear(&scale->api_ctx);
    }

    av_buffer_unref(&scale->out_frames_ref);
}

static int query_formats(AVFilterContext *ctx)
{
    static const enum AVPixelFormat pix_fmts[] =
        {AV_PIX_FMT_NI_QUAD, AV_PIX_FMT_NONE};
    AVFilterFormats *formats;

    formats = ff_make_format_list(pix_fmts);

    if (!formats)
        return AVERROR(ENOMEM);

    return ff_set_common_formats(ctx, formats);
}

static int init_out_pool(AVFilterContext *ctx) {
    NetIntScaleContext *s = ctx->priv;
    AVHWFramesContext *out_frames_ctx;

    if (!ctx->inputs[0]->hw_frames_ctx) {
        return AVERROR(EINVAL);
    }

    out_frames_ctx   = (AVHWFramesContext*)s->out_frames_ref->data;

    /* Don't check return code, this will intentionally fail */
    av_hwframe_ctx_init(s->out_frames_ref);

    /* Create frame pool on device */
    return ff_ni_build_frame_pool(&s->api_ctx, out_frames_ctx->width,
                                  out_frames_ctx->height, s->out_format,
                                  DEFAULT_NI_FILTER_POOL_SIZE);
}

static int config_props(AVFilterLink *outlink)
{
    AVFilterContext *ctx = outlink->src;
    AVFilterLink *inlink0 = outlink->src->inputs[0];
    AVFilterLink *inlink = outlink->src->inputs[0];
    AVHWFramesContext *in_frames_ctx;
    AVHWFramesContext *out_frames_ctx;
    NetIntScaleContext *scale = ctx->priv;
    int w, h, ret, h_shift, v_shift;

    if ((ret = ff_scale_eval_dimensions(ctx,
                                        scale->w_expr, scale->h_expr,
                                        inlink, outlink,
                                        &w, &h)) < 0)
        goto fail;

    /* Note that force_original_aspect_ratio may overwrite the previous set
     * dimensions so that it is not divisible by the set factors anymore
     * unless force_divisible_by is defined as well */
    if (scale->force_original_aspect_ratio) {
        int tmp_w = av_rescale(h, inlink->w, inlink->h);
        int tmp_h = av_rescale(w, inlink->h, inlink->w);

        if (scale->force_original_aspect_ratio == 1) {
             w = FFMIN(tmp_w, w);
             h = FFMIN(tmp_h, h);
             if (scale->force_divisible_by > 1) {
                 // round down
                 w = w / scale->force_divisible_by * scale->force_divisible_by;
                 h = h / scale->force_divisible_by * scale->force_divisible_by;
             }
        } else {
             w = FFMAX(tmp_w, w);
             h = FFMAX(tmp_h, h);
             if (scale->force_divisible_by > 1) {
                 // round up
                 w = (w + scale->force_divisible_by - 1) / scale->force_divisible_by * scale->force_divisible_by;
                 h = (h + scale->force_divisible_by - 1) / scale->force_divisible_by * scale->force_divisible_by;
             }
        }
    }

    if (w > NI_MAX_RESOLUTION_WIDTH || h > NI_MAX_RESOLUTION_HEIGHT) {
        av_log(ctx, AV_LOG_ERROR, "Scaled value (%dx%d) > 8192 not allowed\n", w, h);
        return AVERROR(EINVAL);
    }

    if ((w <= 0) || (h <= 0)) {
        av_log(ctx, AV_LOG_ERROR, "Scaled value (%dx%d) not allowed\n", w, h);
        return AVERROR(EINVAL);
    }

    in_frames_ctx = (AVHWFramesContext *)ctx->inputs[0]->hw_frames_ctx->data;

    if (in_frames_ctx->sw_format == AV_PIX_FMT_BGRP) {
        av_log(ctx, AV_LOG_ERROR, "bgrp not supported\n");
        return AVERROR(EINVAL);
    }
    if (in_frames_ctx->sw_format == AV_PIX_FMT_NI_QUAD_8_TILE_4X4 ||
        in_frames_ctx->sw_format == AV_PIX_FMT_NI_QUAD_10_TILE_4X4) {
        av_log(ctx, AV_LOG_ERROR, "tile4x4 not supported\n");
        return AVERROR(EINVAL);
    }

    /* Set the output format */
    if (scale->format == OUTPUT_FORMAT_AUTO) {
        scale->out_format = in_frames_ctx->sw_format;
    } else {
        scale->out_format = ff_output_fmt[scale->format];
    }

    av_pix_fmt_get_chroma_sub_sample(scale->out_format, &h_shift, &v_shift);

    outlink->w = FFALIGN(w, (1 << h_shift));
    outlink->h = FFALIGN(h, (1 << v_shift));

    /* TODO: make algorithm configurable */

    if (inlink0->sample_aspect_ratio.num){
        outlink->sample_aspect_ratio = av_mul_q((AVRational){outlink->h * inlink0->w, outlink->w * inlink0->h}, inlink0->sample_aspect_ratio);
    } else
        outlink->sample_aspect_ratio = inlink0->sample_aspect_ratio;

    av_log(ctx, AV_LOG_VERBOSE,
           "w:%d h:%d fmt:%s sar:%d/%d -> w:%d h:%d fmt:%s sar:%d/%d\n",
           inlink->w, inlink->h, av_get_pix_fmt_name(inlink->format),
           inlink->sample_aspect_ratio.num, inlink->sample_aspect_ratio.den,
           outlink->w, outlink->h, av_get_pix_fmt_name(outlink->format),
           outlink->sample_aspect_ratio.num, outlink->sample_aspect_ratio.den);

    scale->out_frames_ref = av_hwframe_ctx_alloc(in_frames_ctx->device_ref);
    if (!scale->out_frames_ref)
        return AVERROR(ENOMEM);

    out_frames_ctx = (AVHWFramesContext *)scale->out_frames_ref->data;

    out_frames_ctx->format    = AV_PIX_FMT_NI_QUAD;
    out_frames_ctx->width     = outlink->w;
    out_frames_ctx->height    = outlink->h;
    out_frames_ctx->sw_format = scale->out_format;
    out_frames_ctx->initial_pool_size =
        NI_SCALE_ID; // Repurposed as identity code

    av_buffer_unref(&ctx->outputs[0]->hw_frames_ctx);
    ctx->outputs[0]->hw_frames_ctx = av_buffer_ref(scale->out_frames_ref);

    if (!ctx->outputs[0]->hw_frames_ctx)
        return AVERROR(ENOMEM);

    return 0;

fail:
    return ret;
}

/* Process a received frame */
static int filter_frame(AVFilterLink *link, AVFrame *in)
{
    NetIntScaleContext *scale = link->dst->priv;
    AVFilterLink *outlink = link->dst->outputs[0];
    AVFrame *out = NULL;
    niFrameSurface1_t* frame_surface,*new_frame_surface;
    AVHWFramesContext *pAVHFWCtx;
    AVNIDeviceContext *pAVNIDevCtx;
    ni_retcode_t retcode;
    int scaler_format, cardno;
    uint16_t tempFID;

    frame_surface = (niFrameSurface1_t *) in->data[3];
    if (frame_surface == NULL) {
        return AVERROR(EINVAL);
    }

    pAVHFWCtx = (AVHWFramesContext *) in->hw_frames_ctx->data;
    pAVNIDevCtx       = (AVNIDeviceContext *)pAVHFWCtx->device_ctx->hwctx;
    cardno            = ni_get_cardno(in);

    if (!scale->initialized) {
        retcode = ni_device_session_context_init(&scale->api_ctx);
        if (retcode < 0) {
            av_log(link->dst, AV_LOG_ERROR,
                   "ni scale filter session context init failure\n");
            goto fail;
        }

        scale->api_ctx.device_handle = pAVNIDevCtx->cards[cardno];
        scale->api_ctx.blk_io_handle = pAVNIDevCtx->cards[cardno];

        scale->api_ctx.hw_id             = cardno;
        scale->api_ctx.device_type       = NI_DEVICE_TYPE_SCALER;
        scale->api_ctx.scaler_operation  = NI_SCALER_OPCODE_SCALE;
        scale->api_ctx.keep_alive_timeout = scale->keep_alive_timeout;

        av_log(link->dst, AV_LOG_ERROR,
               "Open scaler session to card %d, hdl %d, blk_hdl %d\n", cardno,
               scale->api_ctx.device_handle, scale->api_ctx.blk_io_handle);

        retcode =
            ni_device_session_open(&scale->api_ctx, NI_DEVICE_TYPE_SCALER);
        if (retcode < 0) {
            av_log(link->dst, AV_LOG_ERROR,
                   "Can't open device session on card %d\n", cardno);
            goto fail;
        }

        scale->session_opened = 1;

        if (scale->params.filterblit) {
            retcode = ni_scaler_set_params(&scale->api_ctx, &(scale->params));
            if (retcode < 0)
                goto fail;
        }

        retcode = init_out_pool(link->dst);

        if (retcode < 0)
        {
            av_log(link->dst, AV_LOG_ERROR, 
                   "Internal output allocation failed rc = %d\n", retcode);
            goto fail;
        }

        ff_ni_clone_hwframe_ctx(
            pAVHFWCtx, (AVHWFramesContext *)scale->out_frames_ref->data,
            &scale->api_ctx);

        if (in->color_range == AVCOL_RANGE_JPEG) {
            av_log(link->dst, AV_LOG_ERROR,
                   "WARNING: Full color range input, limited color output\n");
        }

        scale->initialized = 1;
    }

    scaler_format = ff_ni_ffmpeg_to_gc620_pix_fmt(pAVHFWCtx->sw_format);

    retcode = ni_frame_buffer_alloc_hwenc(&scale->api_dst_frame.data.frame,
                                          outlink->w,
                                          outlink->h,
                                          0);

    if (retcode != NI_RETCODE_SUCCESS)
    {
        retcode = AVERROR(ENOMEM);
        goto fail;
    }

    /*
     * Allocate device input frame. This call won't actually allocate a frame,
     * but sends the incoming hardware frame index to the scaler manager
     */
    retcode = ni_device_alloc_frame(&scale->api_ctx,             //
                                    FFALIGN(in->width, 2),       //
                                    FFALIGN(in->height, 2),      //
                                    scaler_format,               //
                                    0,                           //
                                    0,                           //
                                    0,                           //
                                    0,                           //
                                    0,                           //
                                    0,                           //
                                    frame_surface->ui16FrameIdx, //
                                    NI_DEVICE_TYPE_SCALER);

    if (retcode != NI_RETCODE_SUCCESS)
    {
        av_log(link->dst, AV_LOG_DEBUG,
               "Can't assign input frame %d\n", retcode);
        retcode = AVERROR(ENOMEM);
        goto fail;
    }

    scaler_format = ff_ni_ffmpeg_to_gc620_pix_fmt(scale->out_format);

    /* Allocate hardware device destination frame. This acquires a frame from the pool */
    retcode = ni_device_alloc_frame(&scale->api_ctx,
                          FFALIGN(outlink->w,2),
                          FFALIGN(outlink->h,2),
                          scaler_format,
                          NI_SCALER_FLAG_IO,
                          0,
                          0,
                          0,
                          0,
                          0,
                          -1,
                          NI_DEVICE_TYPE_SCALER);

    if (retcode != NI_RETCODE_SUCCESS)
    {
        av_log(link->dst, AV_LOG_DEBUG,
               "Can't allocate device output frame %d\n", retcode);
        retcode = AVERROR(ENOMEM);
        goto fail;
    }

    out = av_frame_alloc();
    if (!out)
    {
        retcode = AVERROR(ENOMEM);
        goto fail;
    }

    av_frame_copy_props(out,in);

    out->width  = outlink->w;
    out->height = outlink->h;

    out->format = AV_PIX_FMT_NI_QUAD;

    /* Quadra 2D engine always outputs limited color range */
    out->color_range = AVCOL_RANGE_MPEG;

    /* Reference the new hw frames context */
    out->hw_frames_ctx = av_buffer_ref(scale->out_frames_ref);

    out->data[3] = av_malloc(sizeof(niFrameSurface1_t));

    if (!out->data[3])
    {
        retcode = AVERROR(ENOMEM);
        goto fail;
    }

    /* Copy the frame surface from the incoming frame */
    memcpy(out->data[3], in->data[3], sizeof(niFrameSurface1_t));

    /* Set the new frame index */
    retcode = ni_device_session_read_hwdesc(&scale->api_ctx, &scale->api_dst_frame,
                                            NI_DEVICE_TYPE_SCALER);

    if (retcode != NI_RETCODE_SUCCESS) {
        av_log(link->dst, AV_LOG_ERROR,
               "Can't acquire output frame %d\n",retcode);
        retcode = AVERROR(ENOMEM);
        goto fail;
    }

    tempFID = frame_surface->ui16FrameIdx;
    frame_surface = (niFrameSurface1_t *)out->data[3];
    new_frame_surface = (niFrameSurface1_t *)scale->api_dst_frame.data.frame.p_data[3];
    frame_surface->ui16FrameIdx = new_frame_surface->ui16FrameIdx;
    frame_surface->ui16session_ID = new_frame_surface->ui16session_ID;
    frame_surface->device_handle = new_frame_surface->device_handle;
    frame_surface->output_idx     = new_frame_surface->output_idx;
    frame_surface->src_cpu = new_frame_surface->src_cpu;
    frame_surface->bit_depth = ((scale->out_format == AV_PIX_FMT_YUV420P10LE) ||
                                (scale->out_format == AV_PIX_FMT_P010LE))
                                   ? 2
                                   : 1;

    /* Remove ni-split specific assets */
    frame_surface->ui32nodeAddress = 0;

    frame_surface->ui16width  = out->width;
    frame_surface->ui16height = out->height;

    av_log(link->dst, AV_LOG_DEBUG,
           "vf_scale_ni.c:IN trace ui16FrameIdx = [%d] --> out [%d] \n",
           tempFID, frame_surface->ui16FrameIdx);

    out->buf[0] = av_buffer_create(out->data[3], sizeof(niFrameSurface1_t),
                                   ff_ni_frame_free, NULL, 0);

    av_frame_free(&in);

    return ff_filter_frame(link->dst->outputs[0], out);

fail:
    av_frame_free(&in);
    av_frame_free(&out);
    return retcode;
}

#define OFFSET(x) offsetof(NetIntScaleContext, x)
#define FLAGS (AV_OPT_FLAG_VIDEO_PARAM | AV_OPT_FLAG_FILTERING_PARAM)

/* clang-format off */
static const AVOption scale_options[] = {
    { "w",     "Output video width",          OFFSET(w_expr),    AV_OPT_TYPE_STRING,        .flags = FLAGS },
    { "width", "Output video width",          OFFSET(w_expr),    AV_OPT_TYPE_STRING,        .flags = FLAGS },
    { "h",     "Output video height",         OFFSET(h_expr),    AV_OPT_TYPE_STRING,        .flags = FLAGS },
    { "height","Output video height",         OFFSET(h_expr),    AV_OPT_TYPE_STRING,        .flags = FLAGS },
    { "size",   "set video size",          OFFSET(size_str), AV_OPT_TYPE_STRING, {.str = NULL}, 0, FLAGS },
    { "s",      "set video size",          OFFSET(size_str), AV_OPT_TYPE_STRING, {.str = NULL}, 0, FLAGS },
    { "force_original_aspect_ratio", "decrease or increase w/h if necessary to keep the original AR", OFFSET(force_original_aspect_ratio), AV_OPT_TYPE_INT, { .i64 = 0}, 0, 2, FLAGS, "force_oar" },
    { "format", "set_output_format", OFFSET(format), AV_OPT_TYPE_INT, {.i64=OUTPUT_FORMAT_AUTO}, 0, OUTPUT_FORMAT_NB-1, FLAGS, "format" },
        { "yuv420p", "", 0, AV_OPT_TYPE_CONST, {.i64=OUTPUT_FORMAT_YUV420P}, .flags = FLAGS, .unit = "format" },
        { "yuyv422", "", 0, AV_OPT_TYPE_CONST, {.i64=OUTPUT_FORMAT_YUYV422}, .flags = FLAGS, .unit = "format" },
        { "uyvy422", "", 0, AV_OPT_TYPE_CONST, {.i64=OUTPUT_FORMAT_UYVY422}, .flags = FLAGS, .unit = "format" },
        { "nv12", "", 0, AV_OPT_TYPE_CONST, {.i64=OUTPUT_FORMAT_NV12}, .flags = FLAGS, .unit = "format" },
        { "argb", "", 0, AV_OPT_TYPE_CONST, {.i64=OUTPUT_FORMAT_ARGB}, .flags = FLAGS, .unit = "format" },
        { "rgba", "", 0, AV_OPT_TYPE_CONST, {.i64=OUTPUT_FORMAT_RGBA}, .flags = FLAGS, .unit = "format" },
        { "abgr", "", 0, AV_OPT_TYPE_CONST, {.i64=OUTPUT_FORMAT_ABGR}, .flags = FLAGS, .unit = "format" },
        { "bgra", "", 0, AV_OPT_TYPE_CONST, {.i64=OUTPUT_FORMAT_BGRA}, .flags = FLAGS, .unit = "format" },
        { "yuv420p10le", "", 0, AV_OPT_TYPE_CONST, {.i64=OUTPUT_FORMAT_YUV420P10LE}, .flags = FLAGS, .unit = "format" },
        { "nv16", "", 0, AV_OPT_TYPE_CONST, {.i64=OUTPUT_FORMAT_NV16}, .flags = FLAGS, .unit = "format" },
        { "bgr0", "", 0, AV_OPT_TYPE_CONST, {.i64=OUTPUT_FORMAT_BGR0}, .flags = FLAGS, .unit = "format" },
        { "p010le", "", 0, AV_OPT_TYPE_CONST, {.i64=OUTPUT_FORMAT_P010LE}, .flags = FLAGS, .unit = "format" },
        { "bgrp", "", 0, AV_OPT_TYPE_CONST, {.i64=OUTPUT_FORMAT_BGRP}, .flags = FLAGS, .unit = "format" },
        { "auto", "", 0, AV_OPT_TYPE_CONST, {.i64=OUTPUT_FORMAT_AUTO}, .flags = FLAGS, .unit="format"},
    { "disable",  NULL, 0, AV_OPT_TYPE_CONST, {.i64 = 0 }, 0, 0, FLAGS, "force_oar" },
    { "decrease", NULL, 0, AV_OPT_TYPE_CONST, {.i64 = 1 }, 0, 0, FLAGS, "force_oar" },
    { "increase", NULL, 0, AV_OPT_TYPE_CONST, {.i64 = 2 }, 0, 0, FLAGS, "force_oar" },
    { "force_divisible_by", "enforce that the output resolution is divisible by a defined integer when force_original_aspect_ratio is used", OFFSET(force_divisible_by), AV_OPT_TYPE_INT, { .i64 = 1}, 1, 256, FLAGS },
    { "filterblit", "filterblit enable", OFFSET(params.filterblit), AV_OPT_TYPE_BOOL, {.i64=0}, 0, 1, FLAGS },

    {"keep_alive_timeout",
     "Specify a custom session keep alive timeout in seconds.",
     OFFSET(keep_alive_timeout),
     AV_OPT_TYPE_INT,
     {.i64 = NI_DEFAULT_KEEP_ALIVE_TIMEOUT},
     NI_MIN_KEEP_ALIVE_TIMEOUT,
     NI_MAX_KEEP_ALIVE_TIMEOUT,
     FLAGS,
     "keep_alive_timeout"},
    { NULL }
};
/* clang-format on */

static const AVClass scale_class = {
    .class_name       = "ni_scale",
    .item_name        = av_default_item_name,
    .option           = scale_options,
    .version          = LIBAVUTIL_VERSION_INT,
    .category         = AV_CLASS_CATEGORY_FILTER,
};

static const AVFilterPad avfilter_vf_scale_inputs[] = {
    {
        .name         = "default",
        .type         = AVMEDIA_TYPE_VIDEO,
        .filter_frame = filter_frame,
    },
#if (LIBAVFILTER_VERSION_MAJOR < 8)
    { NULL }
#endif
};

static const AVFilterPad avfilter_vf_scale_outputs[] = {
    {
        .name         = "default",
        .type         = AVMEDIA_TYPE_VIDEO,
        .config_props = config_props,
    },
#if (LIBAVFILTER_VERSION_MAJOR < 8)
    { NULL }
#endif
};

AVFilter ff_vf_scale_ni_quadra = {
    .name            = "ni_quadra_scale",
    .description     = NULL_IF_CONFIG_SMALL("NetInt Quadra video scaler v" NI_XCODER_REVISION),
    .init_dict       = init_dict,
    .uninit          = uninit,
    .priv_size       = sizeof(NetIntScaleContext),
    .priv_class      = &scale_class,
    .flags_internal  = FF_FILTER_FLAG_HWFRAME_AWARE,
#if (LIBAVFILTER_VERSION_MAJOR >= 8)
    FILTER_INPUTS(avfilter_vf_scale_inputs),
    FILTER_OUTPUTS(avfilter_vf_scale_outputs),
    FILTER_QUERY_FUNC(query_formats),
#else
    .inputs          = avfilter_vf_scale_inputs,
    .outputs         = avfilter_vf_scale_outputs,
    .query_formats   = query_formats,
#endif
};
#else
#include "avfilter.h"
AVFilter ff_vf_scale_ni_quadra = {};
#endif
