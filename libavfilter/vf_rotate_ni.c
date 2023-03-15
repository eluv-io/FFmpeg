/*
 * Copyright (c) 2013 Stefano Sabatini
 * Copyright (c) 2008 Vitor Sessak
 * Copyright (c) 2022 NETINT Technologies Inc.
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
 * rotation filter, based on the FFmpeg rotate filter
*/

#include <string.h>

#include "libavutil/eval.h"
#include "libavutil/parseutils.h"
#include "libavutil/opt.h"

#include "internal.h"
#include "nifilter.h"

#define BUFFER_WIDTH_PIXEL_ALIGNMENT 16

static const char * const var_names[] = {
    "in_w" , "iw",  ///< width of the input video
    "in_h" , "ih",  ///< height of the input video
    "out_w", "ow",  ///< width of the input video
    "out_h", "oh",  ///< height of the input video
    "hsub", "vsub",
    NULL
};

enum var_name {
    VAR_IN_W , VAR_IW,
    VAR_IN_H , VAR_IH,
    VAR_OUT_W, VAR_OW,
    VAR_OUT_H, VAR_OH,
    VAR_HSUB, VAR_VSUB,
    VAR_VARS_NB
};

typedef struct NetIntRotContext {
    const AVClass *class;

    char *angle_expr_str;
    AVExpr *angle_expr;

    char *outw_expr_str, *outh_expr_str;
    int outw, outh;

    char *fillcolor_str;
    uint8_t fillcolor[4];
    bool fillcolor_enable;

    int hsub, vsub;

    double var_values[VAR_VARS_NB];

    AVBufferRef *out_frames_ref;

    ni_session_context_t api_ctx;
    ni_session_data_io_t api_dst_frame;

    ni_frame_config_t output_frame_config;

    bool initialized;
    bool session_opened;
    int64_t keep_alive_timeout;
} NetIntRotContext;

#define OFFSET(x) offsetof(NetIntRotContext, x)
#define FLAGS (AV_OPT_FLAG_FILTERING_PARAM|AV_OPT_FLAG_VIDEO_PARAM)

static const AVOption rotate_options[] = {
    { "angle",     "set angle (in radians)",       OFFSET(angle_expr_str), AV_OPT_TYPE_STRING, {.str="0"},     0, 0, FLAGS },
    { "a",         "set angle (in radians)",       OFFSET(angle_expr_str), AV_OPT_TYPE_STRING, {.str="0"},     0, 0, FLAGS },
    { "out_w",     "set output width expression",  OFFSET(outw_expr_str),  AV_OPT_TYPE_STRING, {.str="iw"},    0, 0, FLAGS },
    { "ow",        "set output width expression",  OFFSET(outw_expr_str),  AV_OPT_TYPE_STRING, {.str="iw"},    0, 0, FLAGS },
    { "out_h",     "set output height expression", OFFSET(outh_expr_str),  AV_OPT_TYPE_STRING, {.str="ih"},    0, 0, FLAGS },
    { "oh",        "set output height expression", OFFSET(outh_expr_str),  AV_OPT_TYPE_STRING, {.str="ih"},    0, 0, FLAGS },
    { "fillcolor", "set background fill color",    OFFSET(fillcolor_str),  AV_OPT_TYPE_STRING, {.str="black"}, 0, 0, FLAGS },
    { "c",         "set background fill color",    OFFSET(fillcolor_str),  AV_OPT_TYPE_STRING, {.str="black"}, 0, 0, FLAGS },
    { "keep_alive_timeout",
      "specify a custom session keep alive timeout in seconds",
      OFFSET(keep_alive_timeout),
      AV_OPT_TYPE_INT64,
      { .i64 = NI_DEFAULT_KEEP_ALIVE_TIMEOUT },
      NI_MIN_KEEP_ALIVE_TIMEOUT,
      NI_MAX_KEEP_ALIVE_TIMEOUT,
      FLAGS },
    { NULL }
};

AVFILTER_DEFINE_CLASS(rotate);

static av_cold int init(AVFilterContext *ctx)
{
    NetIntRotContext *rot = ctx->priv;

    av_log(ctx, AV_LOG_DEBUG, "Entered %s\n", __func__);

    if (!strcmp(rot->fillcolor_str, "none"))
    {
        rot->fillcolor_enable = false;
    }
    else if (av_parse_color(rot->fillcolor, rot->fillcolor_str, -1, ctx) >= 0)
    {
        rot->fillcolor_enable = true;
    }
    else
    {
        av_log(ctx, AV_LOG_DEBUG, "Exiting %s\n", __func__);
        return AVERROR(EINVAL);
    }

    av_log(ctx, AV_LOG_DEBUG, "Exiting %s\n", __func__);
    return 0;
}

static av_cold void uninit(AVFilterContext *ctx)
{
    NetIntRotContext *rot = ctx->priv;

    av_log(ctx, AV_LOG_DEBUG, "Entered %s\n", __func__);

    av_expr_free(rot->angle_expr);
    rot->angle_expr = NULL;

    if (rot->api_dst_frame.data.frame.p_buffer)
    {
        ni_frame_buffer_free(&rot->api_dst_frame.data.frame);
    }

    if (rot->session_opened)
    {
        /* Close operation will free the device frames */
        ni_device_session_close(&rot->api_ctx, 1, NI_DEVICE_TYPE_SCALER);
        ni_device_session_context_clear(&rot->api_ctx);
    }

    av_buffer_unref(&rot->out_frames_ref);

    av_log(ctx, AV_LOG_DEBUG, "Exiting %s\n", __func__);
}

static int query_formats(AVFilterContext *ctx)
{
    static const enum AVPixelFormat pix_fmts[] = { AV_PIX_FMT_NI_QUAD, AV_PIX_FMT_NONE };
    AVFilterFormats *fmts_list = NULL;

    av_log(ctx, AV_LOG_DEBUG, "Entered %s\n", __func__);

    fmts_list = ff_make_format_list(pix_fmts);
    if (!fmts_list)
    {
        av_log(ctx, AV_LOG_DEBUG, "Exiting %s\n", __func__);
        return AVERROR(ENOMEM);
    }

    av_log(ctx, AV_LOG_DEBUG, "Exiting %s\n", __func__);
    return ff_set_common_formats(ctx, fmts_list);
}

static double get_rotated_w(void *opaque, double angle)
{
    NetIntRotContext *rot = opaque;
    double inw = rot->var_values[VAR_IN_W];
    double inh = rot->var_values[VAR_IN_H];
    float sinx = (float)sin(angle);
    float cosx = (float)cos(angle);

    return FFMAX(0, inh * sinx) + FFMAX(0, -inw * cosx) +
           FFMAX(0, inw * cosx) + FFMAX(0, -inh * sinx);
}

static double get_rotated_h(void *opaque, double angle)
{
    NetIntRotContext *rot = opaque;
    double inw = rot->var_values[VAR_IN_W];
    double inh = rot->var_values[VAR_IN_H];
    float sinx = (float)sin(angle);
    float cosx = (float)cos(angle);

    return FFMAX(0, -inh * cosx) + FFMAX(0, -inw * sinx) +
           FFMAX(0,  inh * cosx) + FFMAX(0,  inw * sinx);
}

static double (* const func1[])(void *, double) = {
    get_rotated_w,
    get_rotated_h,
    NULL
};

static const char * const func1_names[] = {
    "rotw",
    "roth",
    NULL
};

static int config_props(AVFilterLink *outlink)
{
    AVFilterContext *ctx = outlink->src;
    NetIntRotContext *rot = ctx->priv;
    AVFilterLink *inlink = ctx->inputs[0];
    AVHWFramesContext *in_frames_ctx, *out_frames_ctx;
    const AVPixFmtDescriptor *pixdesc = av_pix_fmt_desc_get(inlink->format);
    int ret;
    double res;
    char *expr;

    av_log(ctx, AV_LOG_DEBUG, "Entered %s\n", __func__);

    rot->hsub = pixdesc->log2_chroma_w;
    rot->vsub = pixdesc->log2_chroma_h;

    rot->var_values[VAR_IN_W] = rot->var_values[VAR_IW] = inlink->w;
    rot->var_values[VAR_IN_H] = rot->var_values[VAR_IH] = inlink->h;
    rot->var_values[VAR_HSUB] = 1<<rot->hsub;
    rot->var_values[VAR_VSUB] = 1<<rot->vsub;
    rot->var_values[VAR_OUT_W] = rot->var_values[VAR_OW] = NAN;
    rot->var_values[VAR_OUT_H] = rot->var_values[VAR_OH] = NAN;

    av_expr_free(rot->angle_expr);
    rot->angle_expr = NULL;
    ret = av_expr_parse(&rot->angle_expr,
                        // NOLINTNEXTLINE(clang-analyzer-deadcode.DeadStores)
                        expr = rot->angle_expr_str,
                        var_names,
                        func1_names,
                        func1,
                        NULL,
                        NULL,
                        0,
                        ctx);
    if (ret < 0)
    {
        av_log(ctx,
               AV_LOG_ERROR,
               "Error occurred parsing angle expression '%s'\n",
               rot->angle_expr_str);
        return ret;
    }

#define SET_SIZE_EXPR(name, opt_name) do {                                         \
    ret = av_expr_parse_and_eval(&res, expr = rot->name##_expr_str,                \
                                 var_names, rot->var_values,                       \
                                 func1_names, func1, NULL, NULL, rot, 0, ctx);     \
    if (ret < 0 || isnan(res) || isinf(res) || res <= 0) {                         \
        av_log(ctx, AV_LOG_ERROR,                                                  \
               "Error parsing or evaluating expression for option %s: "            \
               "invalid expression '%s' or non-positive or indefinite value %f\n", \
               opt_name, expr, res);                                               \
        return ret;                                                                \
    }                                                                              \
} while (0)

    /* evaluate width and height */
    av_expr_parse_and_eval(&res,
                           // NOLINTNEXTLINE(clang-analyzer-deadcode.DeadStores)
                           expr = rot->outw_expr_str,
                           var_names,
                           rot->var_values,
                           func1_names,
                           func1,
                           NULL,
                           NULL,
                           rot,
                           0,
                           ctx);
    rot->var_values[VAR_OUT_W] = rot->var_values[VAR_OW] = res;
    // NOLINTNEXTLINE(bugprone-incorrect-roundings, bugprone-narrowing-conversions)
    rot->outw = res + 0.5;
    SET_SIZE_EXPR(outh, "out_h");
    rot->var_values[VAR_OUT_H] = rot->var_values[VAR_OH] = res;
    // NOLINTNEXTLINE(bugprone-incorrect-roundings, bugprone-narrowing-conversions)
    rot->outh = res + 0.5;

    /* evaluate the width again, as it may depend on the evaluated output height */
    SET_SIZE_EXPR(outw, "out_w");
    rot->var_values[VAR_OUT_W] = rot->var_values[VAR_OW] = res;
    // NOLINTNEXTLINE(bugprone-incorrect-roundings, bugprone-narrowing-conversions)
    rot->outw = res + 0.5;

    outlink->w = rot->outw;
    outlink->h = rot->outh;

    if (outlink->w > NI_MAX_RESOLUTION_WIDTH || 
        outlink->h > NI_MAX_RESOLUTION_HEIGHT) {
        av_log(ctx, AV_LOG_ERROR, "Resolution %dx%d > %dx%d is not allowed\n",
               outlink->w, outlink->h,
               NI_MAX_RESOLUTION_WIDTH, NI_MAX_RESOLUTION_HEIGHT);
        return AVERROR(EINVAL);
    }

    in_frames_ctx = (AVHWFramesContext *) ctx->inputs[0]->hw_frames_ctx->data;

    if (in_frames_ctx->sw_format == AV_PIX_FMT_NI_QUAD_8_TILE_4X4 ||
        in_frames_ctx->sw_format == AV_PIX_FMT_NI_QUAD_10_TILE_4X4) {
        av_log(ctx, AV_LOG_ERROR, "tile4x4 not supported\n");
        return AVERROR(EINVAL);
    }


    rot->out_frames_ref = av_hwframe_ctx_alloc(in_frames_ctx->device_ref);
    if (!rot->out_frames_ref)
    {
        av_log(ctx, AV_LOG_DEBUG, "Exiting %s\n", __func__);
        return AVERROR(ENOMEM);
    }

    out_frames_ctx = (AVHWFramesContext *) rot->out_frames_ref->data;

    out_frames_ctx->format = AV_PIX_FMT_NI_QUAD;
    out_frames_ctx->width = rot->outw;
    out_frames_ctx->height = rot->outh;
    out_frames_ctx->sw_format = in_frames_ctx->sw_format;
    out_frames_ctx->initial_pool_size = NI_ROTATE_ID; // Repurposed as identity code

    av_buffer_unref(&ctx->outputs[0]->hw_frames_ctx);
    ctx->outputs[0]->hw_frames_ctx = av_buffer_ref(rot->out_frames_ref);

    if (!ctx->outputs[0]->hw_frames_ctx)
    {
        av_log(ctx, AV_LOG_DEBUG, "Exiting %s\n", __func__);
        return AVERROR(ENOMEM);
    }

    av_log(ctx, AV_LOG_DEBUG, "Exiting %s\n", __func__);
    return 0;
}

static int init_out_pool(AVFilterContext *ctx)
{
    NetIntRotContext *rot = ctx->priv;
    AVHWFramesContext *out_frames_context;

    if (!ctx->inputs[0]->hw_frames_ctx)
    {
        return AVERROR(EINVAL);
    }

    out_frames_context = (AVHWFramesContext*)rot->out_frames_ref->data;

    /* Don't check return code, this will intentionally fail */
    av_hwframe_ctx_init(rot->out_frames_ref);

    /* Create frame pool on device */
    return ff_ni_build_frame_pool(&rot->api_ctx,
                                  out_frames_context->width,
                                  out_frames_context->height,
                                  out_frames_context->sw_format,
                                  DEFAULT_NI_FILTER_POOL_SIZE);
}

static int filter_frame(AVFilterLink *inlink, AVFrame *in)
{
    AVFilterContext *ctx = inlink->dst;
    AVFilterLink *outlink = inlink->dst->outputs[0];
    AVFrame *out = NULL;
    NetIntRotContext *rot = ctx->priv;
    AVBufferRef *out_buffer_ref = rot->out_frames_ref;
    AVHWFramesContext *in_frames_context = (AVHWFramesContext *) in->hw_frames_ctx->data;
    AVNIDeviceContext *av_ni_device_context = (AVNIDeviceContext *) in_frames_context->device_ctx->hwctx;
    ni_retcode_t ni_retcode = NI_RETCODE_SUCCESS;
    niFrameSurface1_t *frame_surface = (niFrameSurface1_t *) in->data[3], *frame_surface2 = NULL;
    ni_frame_config_t input_frame_config = {0};
    uint32_t scaler_format;
    int retcode = 0, rgba_color = 255 /* black opaque */, card_number = (int) in->opaque;
    int aligned_picture_width, rotated_picture_width, rotated_picture_height;
    double angle;

    av_log(ctx, AV_LOG_DEBUG, "Entered %s\n", __func__);

    if (!frame_surface)
    {
        av_log(ctx, AV_LOG_ERROR, "ni rotate filter frame_surface should not be NULL\n");
        return AVERROR(EINVAL);
    }

    if (!rot->initialized)
    {
        if (in_frames_context->sw_format == AV_PIX_FMT_BGRP)
        {
            av_log(ctx, AV_LOG_ERROR, "bgrp not supported\n");
            retcode = AVERROR(EINVAL);
            goto FAIL;
        }

        ni_retcode = ni_device_session_context_init(&rot->api_ctx);
        if (ni_retcode != NI_RETCODE_SUCCESS)
        {
            av_log(ctx, AV_LOG_ERROR, "ni rotate filter session context init failed with %d\n", ni_retcode);
            retcode = AVERROR(EINVAL);
            goto FAIL;
        }

        rot->api_ctx.device_handle = rot->api_ctx.blk_io_handle = av_ni_device_context->cards[card_number];

        rot->api_ctx.hw_id = card_number;
        rot->api_ctx.device_type = NI_DEVICE_TYPE_SCALER;
        rot->api_ctx.scaler_operation = NI_SCALER_OPCODE_ROTATE;
        rot->api_ctx.keep_alive_timeout = rot->keep_alive_timeout;

        ni_retcode = ni_device_session_open(&rot->api_ctx, NI_DEVICE_TYPE_SCALER);
        if (ni_retcode != NI_RETCODE_SUCCESS)
        {
            av_log(ctx, AV_LOG_ERROR, "ni rotate filter device session open failed with %d\n", ni_retcode);
            retcode = AVERROR(EAGAIN);
            goto FAIL;
        }

        rot->session_opened = true;

        ni_retcode = init_out_pool(inlink->dst);
        if (ni_retcode != NI_RETCODE_SUCCESS)
        {
            av_log(ctx, AV_LOG_ERROR, "ni rotate filter init out pool failed with %d\n", ni_retcode);
            goto FAIL;
        }

        ff_ni_clone_hwframe_ctx(in_frames_context, (AVHWFramesContext *)out_buffer_ref->data,
                                &rot->api_ctx);

        if (in->color_range == AVCOL_RANGE_JPEG)
        {
            av_log(ctx, AV_LOG_WARNING, "Full color range input, limited color output\n");
        }

        rot->initialized = true;
    }

    av_log(ctx, AV_LOG_DEBUG, "outlink %dx%d\n", outlink->w, outlink->h);

    ni_retcode = ni_frame_buffer_alloc_hwenc(&rot->api_dst_frame.data.frame,
                                             outlink->w,
                                             outlink->h,
                                             0);
    if (ni_retcode != NI_RETCODE_SUCCESS)
    {
        av_log(ctx, AV_LOG_ERROR, "ni rotate filter frame buffer alloc hwenc failed with %d\n", ni_retcode);
        retcode = AVERROR(ENOMEM);
        goto FAIL;
    }

    // Input.

    scaler_format = ff_ni_ffmpeg_to_gc620_pix_fmt(in_frames_context->sw_format);
    input_frame_config.picture_format = scaler_format;

    input_frame_config.rgba_color = frame_surface->ui32nodeAddress;
    input_frame_config.frame_index = frame_surface->ui16FrameIdx;

    aligned_picture_width = FFALIGN(in->width, BUFFER_WIDTH_PIXEL_ALIGNMENT);

    angle = av_expr_eval(rot->angle_expr, rot->var_values, rot);
    if (angle == 0.0)
    {
        // input_frame_config.orientation = 0; // initialized to zero, unnecessary assignment
        input_frame_config.picture_width = in->width;
        input_frame_config.picture_height = in->height;

        input_frame_config.rectangle_width = FFMIN(outlink->w, in->width);
        input_frame_config.rectangle_height = FFMIN(outlink->h, in->height);

        rotated_picture_width = in->width;
        rotated_picture_height = in->height;
    }
    else if ((angle == -M_PI_2 * 3.0) || (angle == M_PI_2)) // -270.0° || 90.0°
    {
        input_frame_config.orientation = 1;
        input_frame_config.picture_width = aligned_picture_width;
        input_frame_config.picture_height = in->height;

        input_frame_config.rectangle_width = FFMIN(outlink->w, in->height);
        input_frame_config.rectangle_height = FFMIN(outlink->h, in->width);

        rotated_picture_width = in->height;
        rotated_picture_height = aligned_picture_width;
    }
    else if ((angle == -M_PI) || (angle == M_PI)) // -180.0° || 180.0°
    {
        input_frame_config.orientation = 2;
        input_frame_config.picture_width = aligned_picture_width;
        input_frame_config.picture_height = in->height;

        input_frame_config.rectangle_width = FFMIN(outlink->w, in->width);
        input_frame_config.rectangle_height = FFMIN(outlink->h, in->height);

        rotated_picture_width = aligned_picture_width;
        rotated_picture_height = in->height;
    }
    else if ((angle == -M_PI_2) || (angle == M_PI_2 * 3.0)) // -90.0° || 270.0°
    {
        input_frame_config.orientation = 3;
        input_frame_config.picture_width = aligned_picture_width;
        input_frame_config.picture_height = in->height;

        input_frame_config.rectangle_width = FFMIN(outlink->w, in->height);
        input_frame_config.rectangle_height = FFMIN(outlink->h, in->width);

        rotated_picture_width = in->height;
        rotated_picture_height = aligned_picture_width;
    }
    else
    {
        av_log(ctx, AV_LOG_ERROR, "ni rotate filter does not support rotation of %.1f radians\n", angle);
        retcode = AVERROR(EINVAL);
        goto FAIL;
    }

    input_frame_config.rectangle_x =
        (rotated_picture_width > input_frame_config.rectangle_width) ?
        (rotated_picture_width / 2) - (input_frame_config.rectangle_width / 2) : 0;
    input_frame_config.rectangle_y =
        (rotated_picture_height > input_frame_config.rectangle_height) ?
        (rotated_picture_height / 2) - (input_frame_config.rectangle_height / 2) : 0;
    if (aligned_picture_width - in->width)
    {
        switch (input_frame_config.orientation)
        {
        case 1: // 90°
            input_frame_config.rectangle_y =
                (in->width > input_frame_config.rectangle_height) ?
                (in->width / 2) - (input_frame_config.rectangle_height / 2) : 0;
            break;
        case 2: // 180°
            input_frame_config.rectangle_x =
                aligned_picture_width - in->width +
                ((in->width > input_frame_config.rectangle_width) ?
                 (in->width / 2) - (input_frame_config.rectangle_width / 2) : 0);
            break;
        case 3: // 270°
            input_frame_config.rectangle_y =
                aligned_picture_width - in->width +
                ((in->width > input_frame_config.rectangle_height) ?
                 (in->width / 2) - (input_frame_config.rectangle_height / 2) : 0);
            break;
        }
    }

    // use ni_device_config_frame() instead of ni_device_alloc_frame()
    // such that input_frame_config's orientation can be configured
    ni_retcode = ni_device_config_frame(&rot->api_ctx, &input_frame_config);
    if (ni_retcode != NI_RETCODE_SUCCESS)
    {
        av_log(ctx, AV_LOG_ERROR, "ni rotate filter device config input frame failed with %d\n", ni_retcode);
        retcode = AVERROR(EAGAIN);
        goto FAIL;
    }

    // Output.

    if (rot->fillcolor_enable)
    {
        rgba_color = (rot->fillcolor[3] << 24) |
                     (rot->fillcolor[0] << 16) |
                     (rot->fillcolor[1] <<  8) |
                      rot->fillcolor[2];
    }

    rot->output_frame_config.picture_width = outlink->w;
    rot->output_frame_config.picture_height = outlink->h;
    rot->output_frame_config.rectangle_width = input_frame_config.rectangle_width;
    rot->output_frame_config.rectangle_height = input_frame_config.rectangle_height;
    rot->output_frame_config.rectangle_x =
        (rot->output_frame_config.picture_width > rot->output_frame_config.rectangle_width) ?
        (rot->output_frame_config.picture_width / 2) - (rot->output_frame_config.rectangle_width / 2) : 0;
    rot->output_frame_config.rectangle_y =
        (rot->output_frame_config.picture_height > rot->output_frame_config.rectangle_height) ?
        (rot->output_frame_config.picture_height / 2) - (rot->output_frame_config.rectangle_height / 2) : 0;
    rot->output_frame_config.rgba_color = rgba_color;

    ni_retcode = ni_device_alloc_frame(&rot->api_ctx,
                                       rot->output_frame_config.picture_width,
                                       rot->output_frame_config.picture_height,
                                       scaler_format,
                                       NI_SCALER_FLAG_IO,
                                       rot->output_frame_config.rectangle_width,
                                       rot->output_frame_config.rectangle_height,
                                       rot->output_frame_config.rectangle_x,
                                       rot->output_frame_config.rectangle_y,
                                       rot->output_frame_config.rgba_color,
                                       -1,
                                       NI_DEVICE_TYPE_SCALER);

    if (ni_retcode != NI_RETCODE_SUCCESS)
    {
        av_log(ctx, AV_LOG_ERROR, "ni rotate filter device alloc output frame failed with %d\n", ni_retcode);
        retcode = AVERROR(ENOMEM);
        goto FAIL;
    }

    out = av_frame_alloc();
    if (!out)
    {
        av_log(ctx, AV_LOG_ERROR, "ni rotate filter av_frame_alloc returned NULL\n");
        retcode = AVERROR(ENOMEM);
        goto FAIL;
    }

    av_frame_copy_props(out, in);

    out->width = rot->outw;
    out->height = rot->outh;
    out->format = AV_PIX_FMT_NI_QUAD;
    out->color_range = AVCOL_RANGE_MPEG;

    out->hw_frames_ctx = av_buffer_ref(out_buffer_ref);
    out->data[3] = av_malloc(sizeof(niFrameSurface1_t));
    if (!out->data[3])
    {
        av_log(ctx, AV_LOG_ERROR, "ni rotate filter av_alloc returned NULL\n");
        retcode = AVERROR(ENOMEM);
        goto FAIL;
    }
    memcpy(out->data[3], frame_surface, sizeof(niFrameSurface1_t));

    ni_retcode = ni_device_session_read_hwdesc(&rot->api_ctx,
                                               &rot->api_dst_frame,
                                               NI_DEVICE_TYPE_SCALER);
    if (ni_retcode != NI_RETCODE_SUCCESS)
    {
        av_log(ctx, AV_LOG_ERROR, "ni rotate filter read hwdesc failed with %d\n", ni_retcode);
        retcode = AVERROR(ENOMEM);
        goto FAIL;
    }

    frame_surface2 = (niFrameSurface1_t *) rot->api_dst_frame.data.frame.p_data[3];

    frame_surface = (niFrameSurface1_t *) out->data[3];
    frame_surface->ui16FrameIdx = frame_surface2->ui16FrameIdx;
    frame_surface->ui16session_ID = frame_surface2->ui16session_ID;
    frame_surface->device_handle = frame_surface2->device_handle;
    frame_surface->output_idx = frame_surface2->output_idx;
    frame_surface->src_cpu = frame_surface2->src_cpu;
    frame_surface->ui32nodeAddress = 0;
    frame_surface->ui16width = out->width;
    frame_surface->ui16height = out->height;

    out->buf[0] = av_buffer_create(out->data[3],
                                   sizeof(niFrameSurface1_t),
                                   ff_ni_frame_free,
                                   NULL,
                                   0);
    if (!out->buf[0])
    {
        av_log(ctx, AV_LOG_ERROR, "ni rotate filter av_buffer_create returned NULL\n");
        retcode = AVERROR(ENOMEM);
        goto FAIL;
    }

    av_log(ctx, AV_LOG_DEBUG, "Exiting %s\n", __func__);

    av_frame_free(&in);
    return ff_filter_frame(inlink->dst->outputs[0], out);

FAIL:
    av_log(ctx, AV_LOG_DEBUG, "Exiting %s\n", __func__);

    av_frame_free(&in);
    av_frame_free(&out);
    return retcode;
}

static const AVFilterPad avfilter_vf_rotate_inputs[] = {
    {
        .name = "default",
        .type = AVMEDIA_TYPE_VIDEO,
        .filter_frame = filter_frame,
    },
#if (LIBAVFILTER_VERSION_MAJOR < 8)
    { NULL }
#endif
};

static const AVFilterPad avfilter_vf_rotate_outputs[] = {
    {
        .name = "default",
        .type = AVMEDIA_TYPE_VIDEO,
        .config_props = config_props,
    },
#if (LIBAVFILTER_VERSION_MAJOR < 8)
    { NULL }
#endif
};

AVFilter ff_vf_rotate_ni_quadra = {
    .name = "ni_quadra_rotate",
    .description = NULL_IF_CONFIG_SMALL("NetInt Quadra rotate the input video v" NI_XCODER_REVISION),
    .priv_size = sizeof(NetIntRotContext),
    .priv_class = &rotate_class,
    .init = init,
    .uninit = uninit,
#if (LIBAVFILTER_VERSION_MAJOR >= 8)
    FILTER_QUERY_FUNC(query_formats),
    FILTER_INPUTS(avfilter_vf_rotate_inputs),
    FILTER_OUTPUTS(avfilter_vf_rotate_outputs),
#else
    .query_formats = query_formats,
    .inputs = avfilter_vf_rotate_inputs,
    .outputs = avfilter_vf_rotate_outputs,
#endif
    .flags_internal = FF_FILTER_FLAG_HWFRAME_AWARE,
};
