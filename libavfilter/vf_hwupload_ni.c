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

#include "libavutil/buffer.h"
#include "libavutil/hwcontext.h"
#include "libavutil/hwcontext_ni.h"
#include "libavutil/hwcontext_internal.h"
#include "libavutil/log.h"
#include "libavutil/opt.h"

#include "avfilter.h"
#include "formats.h"
#include "internal.h"
#include "video.h"

typedef struct NiUploadContext {
    const AVClass *class;
    int device_idx;

    AVBufferRef *hwdevice;
    AVBufferRef *hwframe;
} NiUploadContext;

static av_cold int niupload_init(AVFilterContext *ctx)
{
    NiUploadContext *s = ctx->priv;
    char buf[64] = { 0 };

    snprintf(buf, sizeof(buf), "%d", s->device_idx);

    return av_hwdevice_ctx_create(&s->hwdevice, AV_HWDEVICE_TYPE_NI, buf, NULL, 0);
}

static av_cold void niupload_uninit(AVFilterContext *ctx)
{
    NiUploadContext *s = ctx->priv;

    av_buffer_unref(&s->hwframe);
    av_buffer_unref(&s->hwdevice);
}

static int niupload_query_formats(AVFilterContext *ctx)
{
    NiUploadContext *nictx = ctx->priv;
    AVHWFramesConstraints *constraints = NULL;
    const enum AVPixelFormat *input_pix_fmts, *output_pix_fmts;
    AVFilterFormats *input_formats = NULL;
    int err, i;

    if (!nictx->hwdevice)
        return AVERROR(ENOMEM);

    constraints = av_hwdevice_get_hwframe_constraints(nictx->hwdevice, NULL);
    if (!constraints) {
        err = AVERROR(EINVAL);
        goto fail;
    }

    input_pix_fmts  = constraints->valid_sw_formats;
    output_pix_fmts = constraints->valid_hw_formats;

    input_formats = ff_make_format_list(output_pix_fmts);
    if (!input_formats) {
        err = AVERROR(ENOMEM);
        goto fail;
    }
    if (input_pix_fmts) {
        for (i = 0; input_pix_fmts[i] != AV_PIX_FMT_NONE; i++) {
            err = ff_add_format(&input_formats, input_pix_fmts[i]);
            if (err < 0)
                goto fail;
        }
    }

    if ((err = ff_formats_ref(input_formats, &ctx->inputs[0]->outcfg.formats)) < 0 ||
        (err = ff_formats_ref(ff_make_format_list(output_pix_fmts),
                              &ctx->outputs[0]->incfg.formats)) < 0)
        goto fail;

    av_hwframe_constraints_free(&constraints);
    return 0;

fail:
    av_buffer_unref(&nictx->hwdevice);
    av_hwframe_constraints_free(&constraints);
    return err;
}

static int niupload_config_output(AVFilterLink *outlink)
{
    AVFilterContext *ctx = outlink->src;
    AVFilterLink *inlink = ctx->inputs[0];
    NiUploadContext *s = ctx->priv;

    AVHWFramesContext *hwframe_ctx;
    int ret;

    av_buffer_unref(&s->hwframe);
    s->hwframe = av_hwframe_ctx_alloc(s->hwdevice);
    if (!s->hwframe)
        return AVERROR(ENOMEM);

    hwframe_ctx            = (AVHWFramesContext*)s->hwframe->data;
    hwframe_ctx->format    = AV_PIX_FMT_NI;
    hwframe_ctx->sw_format = inlink->format;
    hwframe_ctx->width     = inlink->w;
    hwframe_ctx->height    = inlink->h;

    ret = av_hwframe_ctx_init(s->hwframe);
    if (ret < 0)
        return ret;

    outlink->hw_frames_ctx = av_buffer_ref(s->hwframe);
    if (!outlink->hw_frames_ctx)
        return AVERROR(ENOMEM);

    return 0;
}

static int niupload_filter_frame(AVFilterLink *link, AVFrame *in)
{
    AVFilterContext   *ctx = link->dst;
    AVFilterLink  *outlink = ctx->outputs[0];

    AVFrame *out = NULL;
    int ret;

    out = ff_get_video_buffer(outlink, outlink->w, outlink->h);
    if (!out) {
        ret = AVERROR(ENOMEM);
        goto fail;
    }

    out->width  = in->width;
    out->height = in->height;

    ret = av_hwframe_transfer_data(out, in, 0);
    if (ret < 0) {
        av_log(ctx, AV_LOG_ERROR, "niupload_filter_frame(): Error transferring data to the NI devices\n");
        goto fail;
    }

    ret = av_frame_copy_props(out, in);
    if (ret < 0)
        goto fail;

    av_frame_free(&in);

    return ff_filter_frame(ctx->outputs[0], out);
fail:
    av_frame_free(&in);
    av_frame_free(&out);
    return ret;
}

#define OFFSET(x) offsetof(NiUploadContext, x)
#define FLAGS (AV_OPT_FLAG_FILTERING_PARAM | AV_OPT_FLAG_VIDEO_PARAM)
static const AVOption niupload_options[] = {
    { "device", "Number of the device to use", OFFSET(device_idx), AV_OPT_TYPE_INT, { .i64 = 0 }, 0, INT_MAX, FLAGS },
    { NULL },
};

AVFILTER_DEFINE_CLASS(niupload);

static const AVFilterPad niupload_inputs[] = {
    {
        .name         = "default",
        .type         = AVMEDIA_TYPE_VIDEO,
        .filter_frame = niupload_filter_frame,
    },
    { NULL }
};

static const AVFilterPad niupload_outputs[] = {
    {
        .name         = "default",
        .type         = AVMEDIA_TYPE_VIDEO,
        .config_props = niupload_config_output,
    },
    { NULL }
};

AVFilter ff_vf_hwupload_ni = {
    .name        = "ni_hwupload",
    .description = NULL_IF_CONFIG_SMALL("Upload a system memory frame to a Netint device."),

    .init      = niupload_init,
    .uninit    = niupload_uninit,

    .query_formats = niupload_query_formats,

    .priv_size  = sizeof(NiUploadContext),
    .priv_class = &niupload_class,

    .inputs    = niupload_inputs,
    .outputs   = niupload_outputs,

    .flags_internal = FF_FILTER_FLAG_HWFRAME_AWARE,
};
