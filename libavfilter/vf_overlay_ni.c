/*
 * Copyright (c) 2010 Stefano Sabatini
 * Copyright (c) 2010 Baptiste Coudurier
 * Copyright (c) 2007 Bobby Bingham
 * Copyright (c) 2021 NetInt
 *
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
 * overlay one video on top of another
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
#include "libavutil/hwcontext.h"
#include "internal.h"
#include "drawutils.h"
#include "framesync.h"
#include "video.h"
#include "nifilter.h"
#include <ni_device_api.h>

static const char *const var_names[] = {
    "main_w",    "W", ///< width  of the main    video
    "main_h",    "H", ///< height of the main    video
    "overlay_w", "w", ///< width  of the overlay video
    "overlay_h", "h", ///< height of the overlay video
    "hsub",
    "vsub",
    "x",
    "y",
    NULL
};

enum var_name {
    VAR_MAIN_W,    VAR_MW,
    VAR_MAIN_H,    VAR_MH,
    VAR_OVERLAY_W, VAR_OW,
    VAR_OVERLAY_H, VAR_OH,
    VAR_HSUB,
    VAR_VSUB,
    VAR_X,
    VAR_Y,
    VAR_VARS_NB
};

#define MAIN    0
#define OVERLAY 1

#define R 0
#define G 1
#define B 2
#define A 3

#define Y 0
#define U 1
#define V 2

enum OverlayFormat {
    OVERLAY_FORMAT_YUV420,
    OVERLAY_FORMAT_YUV422,
    OVERLAY_FORMAT_YUV444,
    OVERLAY_FORMAT_RGB,
    OVERLAY_FORMAT_GBRP,
    OVERLAY_FORMAT_AUTO,
    OVERLAY_FORMAT_NB
};

typedef struct NetIntOverlayContext {
    const AVClass *class;
    int x, y;                   ///< position of overlaid picture

    uint8_t main_is_packed_rgb;
    uint8_t main_rgba_map[4];
    uint8_t main_has_alpha;
    uint8_t overlay_is_packed_rgb;
    uint8_t overlay_rgba_map[4];
    uint8_t overlay_has_alpha;
    int alpha_format;

    FFFrameSync fs;

    int main_pix_step[4];       ///< steps per pixel for each plane of the main output
    int overlay_pix_step[4];    ///< steps per pixel for each plane of the overlay
    int hsub, vsub;             ///< chroma subsampling values
    const AVPixFmtDescriptor *main_desc; ///< format descriptor for main input

    double var_values[VAR_VARS_NB];
    char *x_expr, *y_expr;

    AVExpr *x_pexpr, *y_pexpr;

    ni_session_context_t api_ctx;
    ni_session_data_io_t api_dst_frame;

    AVBufferRef* out_frames_ref;

    int initialized;
    int session_opened;
    int keep_alive_timeout; /* keep alive timeout setting */
} NetIntOverlayContext;

static av_cold void uninit(AVFilterContext *ctx)
{
    NetIntOverlayContext *s = ctx->priv;

    ff_framesync_uninit(&s->fs);
    av_expr_free(s->x_pexpr); s->x_pexpr = NULL;
    av_expr_free(s->y_pexpr); s->y_pexpr = NULL;

    if (s->api_dst_frame.data.frame.p_buffer)
        ni_frame_buffer_free(&s->api_dst_frame.data.frame);

    if (s->session_opened) {
        /* Close operation will free the device frames */
        ni_device_session_close(&s->api_ctx, 1, NI_DEVICE_TYPE_SCALER);
        ni_device_session_context_clear(&s->api_ctx);
    }

    av_buffer_unref(&s->out_frames_ref);
}

static inline int normalize_xy(double d, int chroma_sub)
{
    if (isnan(d))
        return INT_MAX;
    return (int)d & ~((1 << chroma_sub) - 1);
}

static void eval_expr(AVFilterContext *ctx)
{
    NetIntOverlayContext *s = ctx->priv;

    s->var_values[VAR_X] = av_expr_eval(s->x_pexpr, s->var_values, NULL);
    s->var_values[VAR_Y] = av_expr_eval(s->y_pexpr, s->var_values, NULL);
    s->var_values[VAR_X] = av_expr_eval(s->x_pexpr, s->var_values, NULL);
    s->x = normalize_xy(s->var_values[VAR_X], s->hsub);
    s->y = normalize_xy(s->var_values[VAR_Y], s->vsub);
}

static int set_expr(AVExpr **pexpr, const char *expr, const char *option, void *log_ctx)
{
    int ret;
    AVExpr *old = NULL;

    if (*pexpr)
        old = *pexpr;
    ret = av_expr_parse(pexpr, expr, var_names,
                        NULL, NULL, NULL, NULL, 0, log_ctx);
    if (ret < 0) {
        av_log(log_ctx, AV_LOG_ERROR,
               "Error when evaluating the expression '%s' for %s\n",
               expr, option);
        *pexpr = old;
        return ret;
    }

    av_expr_free(old);
    return 0;
}

static const enum AVPixelFormat alpha_pix_fmts[] = {
    AV_PIX_FMT_YUVA420P, AV_PIX_FMT_YUVA422P, AV_PIX_FMT_YUVA444P,
    AV_PIX_FMT_ARGB, AV_PIX_FMT_ABGR, AV_PIX_FMT_RGBA,
    AV_PIX_FMT_BGRA, AV_PIX_FMT_GBRAP, AV_PIX_FMT_NONE
};

static int query_formats(AVFilterContext *ctx)
{
    /* We only accept hardware frames */
    static const enum AVPixelFormat pix_fmts[] =
        {AV_PIX_FMT_NI_QUAD, AV_PIX_FMT_NONE};
    AVFilterFormats *formats;

    formats = ff_make_format_list(pix_fmts);

    if (!formats)
        return AVERROR(ENOMEM);

    return ff_set_common_formats(ctx, formats);
}

static int config_input_overlay(AVFilterLink *inlink)
{
    AVFilterContext *ctx  = inlink->dst;
    NetIntOverlayContext  *s = inlink->dst->priv;
    int ret;
    AVHWFramesContext *in_frames_ctx;
    const AVPixFmtDescriptor *pix_desc;

    in_frames_ctx = (AVHWFramesContext *) inlink->hw_frames_ctx->data;

    if (!in_frames_ctx) {
        return AVERROR(EINVAL);
    }

    pix_desc = av_pix_fmt_desc_get(in_frames_ctx->sw_format);
    av_image_fill_max_pixsteps(s->overlay_pix_step, NULL, pix_desc);

    /* Finish the configuration by evaluating the expressions
       now when both inputs are configured. */
    s->var_values[VAR_MAIN_W   ] = s->var_values[VAR_MW] = ctx->inputs[MAIN   ]->w;
    s->var_values[VAR_MAIN_H   ] = s->var_values[VAR_MH] = ctx->inputs[MAIN   ]->h;
    s->var_values[VAR_OVERLAY_W] = s->var_values[VAR_OW] = ctx->inputs[OVERLAY]->w;
    s->var_values[VAR_OVERLAY_H] = s->var_values[VAR_OH] = ctx->inputs[OVERLAY]->h;
    s->var_values[VAR_HSUB]  = 1<<pix_desc->log2_chroma_w;
    s->var_values[VAR_VSUB]  = 1<<pix_desc->log2_chroma_h;
    s->var_values[VAR_X]     = NAN;
    s->var_values[VAR_Y]     = NAN;

    if ((ret = set_expr(&s->x_pexpr,      s->x_expr,      "x",      ctx)) < 0 ||
        (ret = set_expr(&s->y_pexpr,      s->y_expr,      "y",      ctx)) < 0)
        return ret;

    s->overlay_is_packed_rgb =
        ff_fill_rgba_map(s->overlay_rgba_map, inlink->format) >= 0;
    s->overlay_has_alpha = ff_fmt_is_in(inlink->format, alpha_pix_fmts);

    av_log(ctx, AV_LOG_VERBOSE,
           "main w:%d h:%d fmt:%s overlay w:%d h:%d fmt:%s\n",
           ctx->inputs[MAIN]->w, ctx->inputs[MAIN]->h,
           av_get_pix_fmt_name(ctx->inputs[MAIN]->format),
           ctx->inputs[OVERLAY]->w, ctx->inputs[OVERLAY]->h,
           av_get_pix_fmt_name(ctx->inputs[OVERLAY]->format));
    return 0;
}

static int init_out_pool(AVFilterContext *ctx) {
    NetIntOverlayContext *s = ctx->priv;
    AVHWFramesContext *out_frames_ctx;

    if (!ctx->inputs[0]->hw_frames_ctx) {
        return AVERROR(EINVAL);
    }

    out_frames_ctx = (AVHWFramesContext *)s->out_frames_ref->data;

    /* Don't check return code, this will intentionally fail */
    av_hwframe_ctx_init(s->out_frames_ref);

    /* Create frame pool on device */
    return ff_ni_build_frame_pool(&s->api_ctx, out_frames_ctx->width,
                                  out_frames_ctx->height, out_frames_ctx->sw_format,
                                  DEFAULT_NI_FILTER_POOL_SIZE);
}

static int process_frame(FFFrameSync *fs)
{
    AVFilterContext      *ctx = fs->parent;
    NetIntOverlayContext *s = (NetIntOverlayContext *) ctx->priv;
    AVHWFramesContext    *main_frame_ctx,*ovly_frame_ctx;
    AVNIDeviceContext *pAVNIDevCtx;
    AVFilterLink         *inlink,*outlink;
    AVFrame              *frame = NULL;
    AVFrame              *overlay = NULL;
    AVFrame              *out = NULL;
    niFrameSurface1_t    *frame_surface,*new_frame_surface;
    int flags, main_cardno, ovly_cardno;
    int main_scaler_format, ovly_scaler_format;
    ni_retcode_t retcode;
    uint16_t tempFIDOverlay = 0;
    uint16_t tempFIDFrame   = 0;

    /* ff_framesync_get_frame() always returns 0 for hw frames */
    ff_framesync_get_frame(fs, OVERLAY, &overlay, 0);

    if (!overlay) {
        ff_framesync_get_frame(fs, MAIN, &frame, 1);
        return ff_filter_frame(ctx->outputs[0], frame);
    }

    ff_framesync_get_frame(fs, MAIN, &frame, 0);

    frame->pts =
        av_rescale_q(fs->pts, fs->time_base, ctx->outputs[0]->time_base);

    inlink = ctx->inputs[MAIN];

    if (overlay)
    {
        s->var_values[VAR_OVERLAY_W] = s->var_values[VAR_OW] = overlay->width;
        s->var_values[VAR_OVERLAY_H] = s->var_values[VAR_OH] = overlay->height;
    }

    s->var_values[VAR_MAIN_W   ] = s->var_values[VAR_MW] = frame->width;
    s->var_values[VAR_MAIN_H   ] = s->var_values[VAR_MH] = frame->height;

    //This can satisfy some customers or demos to modify the location when using ni_overlay
    set_expr(&s->x_pexpr, s->x_expr,"x", ctx);
    set_expr(&s->y_pexpr, s->y_expr,"y", ctx);

    eval_expr(ctx);
    av_log(ctx, AV_LOG_DEBUG, "x:%f xi:%d y:%f yi:%d\n",
           s->var_values[VAR_X], s->x,
           s->var_values[VAR_Y], s->y);

    main_frame_ctx = (AVHWFramesContext *) frame->hw_frames_ctx->data;
    main_scaler_format = ff_ni_ffmpeg_to_gc620_pix_fmt(main_frame_ctx->sw_format);
    outlink = ctx->outputs[0];

    main_cardno = ni_get_cardno(frame);

    if (overlay)
    {
        ovly_frame_ctx = (AVHWFramesContext *) overlay->hw_frames_ctx->data;
        ovly_scaler_format = ff_ni_ffmpeg_to_gc620_pix_fmt(ovly_frame_ctx->sw_format);
        ovly_cardno        = ni_get_cardno(overlay);

        if (main_cardno != ovly_cardno) {
            av_log(ctx, AV_LOG_ERROR,
                   "Main/Overlay frames on different cards\n");
            return AVERROR(EINVAL);
        }
    }
    else
    {
        ovly_scaler_format = 0;
    }

    if (!s->initialized) {
        retcode = ni_device_session_context_init(&s->api_ctx);
        if (retcode < 0) {
            av_log(ctx, AV_LOG_ERROR,
                   "ni overlay filter session context init failure\n");
            return retcode;
        }

        pAVNIDevCtx = (AVNIDeviceContext *)main_frame_ctx->device_ctx->hwctx;
        s->api_ctx.device_handle = pAVNIDevCtx->cards[main_cardno];
        s->api_ctx.blk_io_handle = pAVNIDevCtx->cards[main_cardno];

        s->api_ctx.hw_id             = main_cardno;
        s->api_ctx.device_type       = NI_DEVICE_TYPE_SCALER;
        s->api_ctx.scaler_operation  = NI_SCALER_OPCODE_OVERLAY;
        s->api_ctx.keep_alive_timeout = s->keep_alive_timeout;

        retcode = ni_device_session_open(&s->api_ctx, NI_DEVICE_TYPE_SCALER);
        if (retcode < 0) {
            av_log(ctx, AV_LOG_ERROR, "Can't open device session on card %d\n",
                   main_cardno);
            return retcode;
        }

        s->session_opened = 1;

        retcode = init_out_pool(inlink->dst);

        if (retcode < 0)
        {
            av_log(ctx, AV_LOG_ERROR,
                   "Internal output allocation failed rc = %d\n", retcode);
            return retcode;
        }

        ff_ni_clone_hwframe_ctx(main_frame_ctx,
                                (AVHWFramesContext *)s->out_frames_ref->data,
                                &s->api_ctx);

        if ((frame && frame->color_range == AVCOL_RANGE_JPEG) ||
            (overlay && overlay->color_range == AVCOL_RANGE_JPEG)) {
            av_log(ctx, AV_LOG_ERROR,
                   "WARNING: Full color range input, limited color output\n");
        }

        s->initialized = 1;
    }

    /* Allocate a ni_frame for the overlay output */
    retcode = ni_frame_buffer_alloc_hwenc(&s->api_dst_frame.data.frame,
                                          outlink->w,
                                          outlink->h,
                                          0);

    if (retcode != NI_RETCODE_SUCCESS)
    {
        return AVERROR(ENOMEM);
    }

    if (overlay)
    {
      frame_surface = (niFrameSurface1_t *)overlay->data[3];
      tempFIDOverlay = frame_surface->ui16FrameIdx;
    }
    else
    {
      frame_surface = NULL;
    }
    /*
     * Allocate device input frame for overlay picture. This call won't actually
     * allocate a frame, but sends the incoming hardware frame index to the
     * scaler manager.
     */
    retcode = ni_device_alloc_frame(
        &s->api_ctx,                                             //
        overlay ? FFALIGN(overlay->width, 2) : 0,                //
        overlay ? FFALIGN(overlay->height, 2) : 0,               //
        ovly_scaler_format,                                      //
        0,                                                       //
        overlay ? FFALIGN(overlay->width, 2) : 0,                //
        overlay ? FFALIGN(overlay->height, 2) : 0,               //
        s->x,                                                    //
        s->y,                                                    //
        frame_surface ? (int)frame_surface->ui32nodeAddress : 0, //
        frame_surface ? frame_surface->ui16FrameIdx : 0,         //
        NI_DEVICE_TYPE_SCALER);

    if (retcode != NI_RETCODE_SUCCESS)
    {
        av_log(ctx, AV_LOG_DEBUG, "Can't assign frame for overlay input %d\n",
               retcode);
        return AVERROR(ENOMEM);
    }

    frame_surface = (niFrameSurface1_t *) frame->data[3];
    if (frame_surface == NULL) {
        return AVERROR(EINVAL);
    }

    tempFIDFrame = frame_surface->ui16FrameIdx;
    /*
     * Allocate device output frame from the pool. We also send down the frame index
     * of the background frame to the scaler manager.
     */
    flags = (s->alpha_format ? NI_SCALER_FLAG_PA : 0) | NI_SCALER_FLAG_IO;
    retcode = ni_device_alloc_frame(&s->api_ctx,                    //
                                    FFALIGN(frame->width, 2),       //
                                    FFALIGN(frame->height, 2),      //
                                    main_scaler_format,             //
                                    flags,                          //
                                    FFALIGN(frame->width, 2),       //
                                    FFALIGN(frame->height, 2),      //
                                    0,                              // x
                                    0,                              // y
                                    frame_surface->ui32nodeAddress, //
                                    frame_surface->ui16FrameIdx,    //
                                    NI_DEVICE_TYPE_SCALER);

    if (retcode != NI_RETCODE_SUCCESS)
    {
        av_log(ctx, AV_LOG_DEBUG, "Can't allocate frame for output %d\n",
               retcode);
        return AVERROR(ENOMEM);
    }

    out = av_frame_alloc();
    if (!out)
    {
        return AVERROR(ENOMEM);
    }

    av_frame_copy_props(out,frame);

    out->width = outlink->w;
    out->height = outlink->h;
    out->format = AV_PIX_FMT_NI_QUAD;

    /* Quadra 2D engine always outputs limited color range */
    out->color_range = AVCOL_RANGE_MPEG;

    /* Reference the new hw frames context */
    out->hw_frames_ctx = av_buffer_ref(s->out_frames_ref);
    out->data[3] = av_malloc(sizeof(niFrameSurface1_t));

    if (!out->data[3])
    {
        av_frame_free(&out);
        return AVERROR(ENOMEM);
    }

    /* Copy the frame surface from the incoming frame */
    memcpy(out->data[3], frame->data[3], sizeof(niFrameSurface1_t));

    /* Set the new frame index */
    retcode = ni_device_session_read_hwdesc(&s->api_ctx, &s->api_dst_frame,
                                            NI_DEVICE_TYPE_SCALER);

    if (retcode != NI_RETCODE_SUCCESS) {
        av_log(ctx, AV_LOG_ERROR,
               "Can't acquire output frame %d\n", retcode);
        av_frame_free(&out);
        return AVERROR(ENOMEM);
    }

    frame_surface = (niFrameSurface1_t *) out->data[3];
    new_frame_surface = (niFrameSurface1_t *) s->api_dst_frame.data.frame.p_data[3];
    frame_surface->ui16FrameIdx = new_frame_surface->ui16FrameIdx;
    frame_surface->ui16session_ID = new_frame_surface->ui16session_ID;
    frame_surface->device_handle = new_frame_surface->device_handle;
    frame_surface->output_idx     = new_frame_surface->output_idx;
    frame_surface->src_cpu = new_frame_surface->src_cpu;

    /* Remove ni-split specific assets */
    frame_surface->ui32nodeAddress = 0;

    frame_surface->ui16width = out->width;
    frame_surface->ui16height = out->height;

    av_log(
        ctx, AV_LOG_DEBUG,
        "vf_overlay_ni.c:IN trace ui16FrameIdx = [%d] and [%d] --> out [%d] \n",
        tempFIDFrame, tempFIDOverlay, frame_surface->ui16FrameIdx);

    out->buf[0] = av_buffer_create(out->data[3], sizeof(niFrameSurface1_t), ff_ni_frame_free, NULL, 0);

    return ff_filter_frame(ctx->outputs[0], out);

}

static int init_framesync(AVFilterContext *ctx)
{
    NetIntOverlayContext *s = ctx->priv;
    int ret, i;

    s->fs.on_event = process_frame;
    s->fs.opaque   = s;
    ret = ff_framesync_init(&s->fs, ctx, ctx->nb_inputs);
    if (ret < 0)
        return ret;

    for (i = 0; i < ctx->nb_inputs; i++) {
        FFFrameSyncIn *in = &s->fs.in[i];
        in->before    = EXT_STOP;
        in->after     = EXT_INFINITY;
        in->sync      = i ? 1 : 2;
        in->time_base = ctx->inputs[i]->time_base;
    }

    return ff_framesync_configure(&s->fs);
}

static int config_output(AVFilterLink *outlink)
{
    AVFilterContext *ctx = outlink->src;
    NetIntOverlayContext *s = ctx->priv;
    AVHWFramesContext *in_frames_ctx,*in_frames_ctx_ovly;
    AVHWFramesContext *out_frames_ctx;
    int ret;

    av_log(ctx, AV_LOG_DEBUG, "Output is of %s.\n", av_get_pix_fmt_name(outlink->format));

    outlink->w = ctx->inputs[MAIN]->w;
    outlink->h = ctx->inputs[MAIN]->h;
    outlink->frame_rate = ctx->inputs[MAIN]->frame_rate;
    outlink->time_base = ctx->inputs[MAIN]->time_base;

    ret = init_framesync(ctx);
    if (ret < 0)
        return ret;

    in_frames_ctx = (AVHWFramesContext *)ctx->inputs[0]->hw_frames_ctx->data;

    if (in_frames_ctx->sw_format == AV_PIX_FMT_BGRP) {
        av_log(ctx, AV_LOG_ERROR, "bgrp not supported for background\n");
        return AVERROR(EINVAL);
    }
    if (in_frames_ctx->sw_format == AV_PIX_FMT_NI_QUAD_8_TILE_4X4 ||
        in_frames_ctx->sw_format == AV_PIX_FMT_NI_QUAD_10_TILE_4X4) {
        av_log(ctx, AV_LOG_ERROR, "tile4x4 not supported for background\n");
        return AVERROR(EINVAL);
    }

    in_frames_ctx_ovly = (AVHWFramesContext *)ctx->inputs[1]->hw_frames_ctx->data;

    if (in_frames_ctx_ovly->sw_format == AV_PIX_FMT_BGRP) {
        av_log(ctx, AV_LOG_ERROR, "bgrp not supported for overlay\n");
        return AVERROR(EINVAL);
    }
    if (in_frames_ctx_ovly->sw_format == AV_PIX_FMT_NI_QUAD_8_TILE_4X4 ||
        in_frames_ctx_ovly->sw_format == AV_PIX_FMT_NI_QUAD_10_TILE_4X4) {
        av_log(ctx, AV_LOG_ERROR, "tile4x4 not supported for overlay\n");
        return AVERROR(EINVAL);
    }

    s->out_frames_ref = av_hwframe_ctx_alloc(in_frames_ctx->device_ref);
    if (!s->out_frames_ref)
        return AVERROR(ENOMEM);

    out_frames_ctx = (AVHWFramesContext *)s->out_frames_ref->data;

    out_frames_ctx->format    = AV_PIX_FMT_NI_QUAD;
    out_frames_ctx->width     = outlink->w;
    out_frames_ctx->height    = outlink->h;
    out_frames_ctx->sw_format = in_frames_ctx->sw_format;
    out_frames_ctx->initial_pool_size =
        NI_OVERLAY_ID; // Repurposed as identity code

    av_buffer_unref(&ctx->outputs[0]->hw_frames_ctx);
    ctx->outputs[0]->hw_frames_ctx = av_buffer_ref(s->out_frames_ref);

    if (!ctx->outputs[0]->hw_frames_ctx)
        return AVERROR(ENOMEM);

    return 0;
}

/**
 * Blend image in src to destination buffer dst at position (x, y).
 */

static int config_input_main(AVFilterLink *inlink)
{
    NetIntOverlayContext *s = inlink->dst->priv;
    AVHWFramesContext *in_frames_ctx;
    const AVPixFmtDescriptor *pix_desc;

    in_frames_ctx = (AVHWFramesContext *) inlink->hw_frames_ctx->data;

    if (!in_frames_ctx) {
        return AVERROR(EINVAL);
    }

    pix_desc = av_pix_fmt_desc_get(in_frames_ctx->sw_format);

    s->hsub = pix_desc->log2_chroma_w;
    s->vsub = pix_desc->log2_chroma_h;

    return 0;
}

static int activate(AVFilterContext *ctx)
{
    NetIntOverlayContext *s = ctx->priv;
    return ff_framesync_activate(&s->fs);
}

#define OFFSET(x) offsetof(NetIntOverlayContext, x)
#define FLAGS (AV_OPT_FLAG_VIDEO_PARAM | AV_OPT_FLAG_FILTERING_PARAM)

static const AVOption overlay_ni_options[] = {
    { "x", "set the x expression", OFFSET(x_expr), AV_OPT_TYPE_STRING, {.str = "0"}, CHAR_MIN, CHAR_MAX, FLAGS },
    { "y", "set the y expression", OFFSET(y_expr), AV_OPT_TYPE_STRING, {.str = "0"}, CHAR_MIN, CHAR_MAX, FLAGS },
    { "eof_action", "Action to take when encountering EOF from secondary input ",
        OFFSET(fs.opt_eof_action), AV_OPT_TYPE_INT, { .i64 = EOF_ACTION_REPEAT },
        EOF_ACTION_REPEAT, EOF_ACTION_PASS, .flags = FLAGS, "eof_action" },
    { "repeat", "Repeat the previous frame.",   0, AV_OPT_TYPE_CONST, { .i64 = EOF_ACTION_REPEAT }, .flags = FLAGS, "eof_action" },
    { "endall", "End both streams.",            0, AV_OPT_TYPE_CONST, { .i64 = EOF_ACTION_ENDALL }, .flags = FLAGS, "eof_action" },
    { "pass",   "Pass through the main input.", 0, AV_OPT_TYPE_CONST, { .i64 = EOF_ACTION_PASS },   .flags = FLAGS, "eof_action" },
    { "shortest", "force termination when the shortest input terminates", OFFSET(fs.opt_shortest), AV_OPT_TYPE_BOOL, { .i64 = 0 }, 0, 1, FLAGS },
    { "repeatlast", "repeat overlay of the last overlay frame", OFFSET(fs.opt_repeatlast), AV_OPT_TYPE_BOOL, {.i64=1}, 0, 1, FLAGS },
    { "alpha", "alpha format", OFFSET(alpha_format), AV_OPT_TYPE_INT, {.i64=0}, 0, 1, FLAGS, "alpha_format" },
        { "straight",      "", 0, AV_OPT_TYPE_CONST, {.i64=0}, .flags = FLAGS, .unit = "alpha_format" },
        { "premultiplied", "", 0, AV_OPT_TYPE_CONST, {.i64=1}, .flags = FLAGS, .unit = "alpha_format" },
    { "keep_alive_timeout",
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

// NOLINTNEXTLINE(clang-diagnostic-deprecated-declarations)
FRAMESYNC_DEFINE_CLASS(overlay_ni, NetIntOverlayContext, fs);

static const AVFilterPad avfilter_vf_overlay_inputs[] = {
    {
        .name         = "main",
        .type         = AVMEDIA_TYPE_VIDEO,
        .config_props = config_input_main,
    },
    {
        .name         = "overlay",
        .type         = AVMEDIA_TYPE_VIDEO,
        .config_props = config_input_overlay,
    },
#if (LIBAVFILTER_VERSION_MAJOR < 8)
    { NULL }
#endif
};

static const AVFilterPad avfilter_vf_overlay_outputs[] = {
    {
        .name          = "default",
        .type          = AVMEDIA_TYPE_VIDEO,
        .config_props  = config_output,
    },
#if (LIBAVFILTER_VERSION_MAJOR < 8)
    { NULL }
#endif
};

AVFilter ff_vf_overlay_ni_quadra = {
    .name          = "ni_quadra_overlay",
    .description   = NULL_IF_CONFIG_SMALL("NetInt Quadra overlay a video source on top of the input v" NI_XCODER_REVISION),
    .preinit       = overlay_ni_framesync_preinit,
    .uninit        = uninit,
    .priv_size     = sizeof(NetIntOverlayContext),
    .priv_class    = &overlay_ni_class,
    .activate      = activate,
#if (LIBAVFILTER_VERSION_MAJOR >= 8)
    FILTER_INPUTS(avfilter_vf_overlay_inputs),
    FILTER_OUTPUTS(avfilter_vf_overlay_outputs),
    FILTER_QUERY_FUNC(query_formats),
#else
    .inputs        = avfilter_vf_overlay_inputs,
    .outputs       = avfilter_vf_overlay_outputs,
    .query_formats = query_formats,
#endif
    .flags_internal= FF_FILTER_FLAG_HWFRAME_AWARE
};
#else
#include "avfilter.h"
AVFilter ff_vf_overlay_ni_quadra = {};
#endif
