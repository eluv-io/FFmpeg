/*
 * Copyright (c) 2007 Bobby Bingham
 * Copyright (c) 2021 NetInt
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
 * audio and video splitter
 */

#include <stdio.h>

#include "libavutil/attributes.h"
#include "libavutil/internal.h"
#include "libavutil/hwcontext.h"
#include "libavutil/hwcontext_internal.h"
#include "libavutil/hwcontext_ni_quad.h"
#include "libavutil/mem.h"
#include "libavutil/opt.h"
#include "nifilter.h"
#include <ni_device_api.h>

#define FF_INTERNAL_FIELDS 1
#include "framequeue.h"

#include "avfilter.h"
#include "audio.h"
#include "formats.h"
#include "internal.h"
#include "video.h"

typedef struct SplitContext {
    const AVClass *class;
    int nb_output0;
    int nb_output1;
    int nb_output2;
    int total_outputs;
    int frame_contexts_applied;
    ni_split_context_t src_ctx;
    AVBufferRef *out_frames_ref[3];
} SplitContext;

static int init_out_hwctxs(AVFilterContext *ctx)
{
  SplitContext *s = ctx->priv;
  AVHWFramesContext *in_frames_ctx;
  AVHWFramesContext *out_frames_ctx[3];

  enum AVPixelFormat out_format;
  int i,j;

  if (!ctx->inputs[0]->hw_frames_ctx) {
    return AVERROR(EINVAL);
  }
  
  in_frames_ctx = (AVHWFramesContext*)ctx->inputs[0]->hw_frames_ctx->data;
  if (s->src_ctx.enabled == 1)
  {
    for (i = 0; i < 3; i++)
    {
      s->out_frames_ref[i] = av_hwframe_ctx_alloc(in_frames_ctx->device_ref);
      if (!s->out_frames_ref[i])
        return AVERROR(ENOMEM);
      out_frames_ctx[i] = (AVHWFramesContext*)s->out_frames_ref[i]->data;

      out_frames_ctx[i]->format = AV_PIX_FMT_NI_QUAD;
      out_frames_ctx[i]->width = s->src_ctx.w[i];
      out_frames_ctx[i]->height = s->src_ctx.h[i];

      if (s->src_ctx.f[i] == -1)
      {
        return AVERROR(EINVAL);
      }

      if (s->src_ctx.f[i] == NI_PIXEL_PLANAR_FORMAT_PLANAR)//yuv420p or p10
      {
        out_format = (s->src_ctx.f8b[i] == 1) ? AV_PIX_FMT_YUV420P
                                                : AV_PIX_FMT_YUV420P10LE;
      } 
      else if (s->src_ctx.f[i] == NI_PIXEL_PLANAR_FORMAT_TILED4X4) // tiled
      {
          out_format = (s->src_ctx.f8b[i] == 1) ? AV_PIX_FMT_NI_QUAD_8_TILE_4X4
                           : AV_PIX_FMT_NI_QUAD_10_TILE_4X4;
      }
      else //NV12
      {
        out_format = (s->src_ctx.f8b[i] == 1) ? AV_PIX_FMT_NV12 : AV_PIX_FMT_P010LE;
      }
      out_frames_ctx[i]->sw_format = out_format;
      out_frames_ctx[i]->initial_pool_size = -1; //already has its own pool

      /* Don't check return code, this will intentionally fail */
      av_hwframe_ctx_init(s->out_frames_ref[i]);

      ff_ni_clone_hwframe_ctx(in_frames_ctx, out_frames_ctx[i], NULL);
    }

    for (i = 0; i < ctx->nb_outputs; i++)
    {
      av_buffer_unref(&ctx->outputs[i]->hw_frames_ctx);
      if (i < s->nb_output0)
      {
        j = 0;
      }
      else if (i < s->nb_output0 + s->nb_output1)
      {
        j = 1;
      }
      else
      {
        j = 2;
      }
      ctx->outputs[i]->hw_frames_ctx = av_buffer_ref(s->out_frames_ref[j]);

      av_log(ctx, AV_LOG_DEBUG, "NI:%s:out\n",
             (s->src_ctx.f[j] == 0) ? "semiplanar"
             : (s->src_ctx.f[j] == 2) ? "tiled"
                                   : "planar");
      if (!ctx->outputs[i]->hw_frames_ctx)
        return AVERROR(ENOMEM);

      av_log(ctx, AV_LOG_DEBUG,
             "ni_split superframe config_output_hwctx[%d] %p\n", i,
             ctx->outputs[i]->hw_frames_ctx);
    }
  }
  else 
  {//no possibility of using extra outputs
    for (i = 0; i < ctx->nb_outputs; i++)
    {
      av_buffer_unref(&ctx->outputs[i]->hw_frames_ctx);
      if (i < s->nb_output0)
      {
        ctx->outputs[i]->hw_frames_ctx = av_buffer_ref(ctx->inputs[0]->hw_frames_ctx);
      }
      if (!ctx->outputs[i]->hw_frames_ctx)
        return AVERROR(ENOMEM);

      av_log(ctx, AV_LOG_DEBUG, "ni_split config_output_hwctx[%d] %p\n", i,
             ctx->outputs[i]->hw_frames_ctx);
    }
    av_log(ctx, AV_LOG_DEBUG, "ni_split config_output_hwctx set direct to output\n");
  }
  return 0;
}

static int config_output(AVFilterLink *link)
{
  //config output sets all outputs at a time since there's no 
  //easy way to track the target output based on inlink.
  //fairly trivial assignments here so no performance worries
  AVFilterContext *ctx = link->src;
  SplitContext *s = ctx->priv;
  int i, ret;
  for (i = 0; i < ctx->nb_outputs; i++)
  {
    if (i < s->nb_output0)
    {
      ctx->outputs[i]->w = s->src_ctx.w[0];
      ctx->outputs[i]->h = s->src_ctx.h[0];
    }
    else if (i < s->nb_output0 + s->nb_output1)
    {
      ctx->outputs[i]->w = s->src_ctx.w[1];
      ctx->outputs[i]->h = s->src_ctx.h[1];
    }
    else 
    {
      ctx->outputs[i]->w = s->src_ctx.w[2];
      ctx->outputs[i]->h = s->src_ctx.h[2];
    }
    av_log(ctx, AV_LOG_DEBUG, "ni_split config_output[%d] w x h = %d x %d\n",
           i, ctx->outputs[i]->w, ctx->outputs[i]->h);
  }
  if (s->frame_contexts_applied == 0)
  {
    s->frame_contexts_applied = 1; //run once per set ni_split, not per output
    ret = init_out_hwctxs(ctx);
    if (ret < 0)
      return ret;
  }
  return 0;
}

static av_cold int split_init(AVFilterContext *ctx)
{
    SplitContext *s = ctx->priv;
    int i, ret;
    av_log(ctx, AV_LOG_DEBUG, "nisplitINIT out0,1,2 = %d %d %d ctx->nb_outputs = %d\n",
        s->nb_output0, s->nb_output1, s->nb_output2,
        ctx->nb_outputs);
    if (s->nb_output2 && s->nb_output1 == 0)
    {
        //swap them for reorder to use out1 first
        s->nb_output1 = s->nb_output2;
        s->nb_output2 = 0;
        av_log(ctx, AV_LOG_DEBUG, "nisplitINIT out2 moved to out1\n");
    }

    s->total_outputs = s->nb_output0 + s->nb_output1 + s->nb_output2;
    //ctx->nb_outputs = s->total_outputs;
    for (i = 0; i < s->total_outputs; i++) {
        char name[32];
        AVFilterPad pad = { 0 };

        snprintf(name, sizeof(name), "output%d", i);
        pad.type = ctx->filter->inputs[0].type;
        pad.name = av_strdup(name);
        pad.config_props = config_output;
        
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


static int query_formats(AVFilterContext *ctx) 
{
    static const enum AVPixelFormat input_pix_fmts[] = {
        AV_PIX_FMT_YUV420P,
        AV_PIX_FMT_YUVJ420P,
        AV_PIX_FMT_YUV420P10LE,
        AV_PIX_FMT_NV12,
        AV_PIX_FMT_P010LE,
        AV_PIX_FMT_NI_QUAD,
        AV_PIX_FMT_NONE,
    };
    static const enum AVPixelFormat output_pix_fmts[] = {
        AV_PIX_FMT_YUV420P,
        AV_PIX_FMT_YUVJ420P,
        AV_PIX_FMT_YUV420P10LE,
        AV_PIX_FMT_NV12,
        AV_PIX_FMT_P010LE,
        AV_PIX_FMT_NI_QUAD,
        AV_PIX_FMT_NONE,
    };
    AVFilterFormats *in_fmts = ff_make_format_list(input_pix_fmts);
    AVFilterFormats *out_fmts = ff_make_format_list(output_pix_fmts);

    // Needed for FFmpeg-n4.4+
#if (LIBAVFILTER_VERSION_MAJOR >= 8 || LIBAVFILTER_VERSION_MAJOR >= 7 && LIBAVFILTER_VERSION_MINOR >= 110)
    // NOLINTNEXTLINE(clang-diagnostic-unused-result)
    ff_formats_ref(in_fmts, &ctx->inputs[0]->outcfg.formats);
    // NOLINTNEXTLINE(clang-diagnostic-unused-result)
    ff_formats_ref(out_fmts, &ctx->outputs[0]->incfg.formats);
#else
    // NOLINTNEXTLINE(clang-diagnostic-unused-result)
    ff_formats_ref(in_fmts, &ctx->inputs[0]->out_formats);
    // NOLINTNEXTLINE(clang-diagnostic-unused-result)
    ff_formats_ref(out_fmts, &ctx->outputs[0]->in_formats);
#endif

    return 0;
}

static int config_input(AVFilterLink *inlink)
{
  AVFilterContext *avctx = inlink->dst;
  SplitContext *s = avctx->priv;
  AVHWFramesContext *ctx;
  ni_split_context_t *p_split_ctx_dst = &s->src_ctx;
  NIFramesContext *src_ctx;
  ni_split_context_t *p_split_ctx_src; 
  //av_log(ctx, AV_LOG_DEBUG, "nisplit config_input 0x%lx\n", inlink->hw_frames_ctx);
  int i;
  s->frame_contexts_applied = -1;
  if (inlink->hw_frames_ctx == NULL)
  {
      for (i = 0; i < 3; i++)
      {
          s->src_ctx.w[i] = inlink->w;
          s->src_ctx.h[i] = inlink->h;
          s->src_ctx.f[i] = -1;
          s->src_ctx.f8b[i] = -1;
      }
  }
  else
  {
      ctx = (AVHWFramesContext*)inlink->hw_frames_ctx->data;
      src_ctx = ctx->internal->priv;
      p_split_ctx_src = &src_ctx->split_ctx;
      memcpy(p_split_ctx_dst, p_split_ctx_src, sizeof(ni_split_context_t));
      for (i = 0; i < 3; i++)
      {
          s->frame_contexts_applied = 0;
          av_log(avctx, AV_LOG_DEBUG, "[%d] %d x %d  f8b %d\n", 
                 i, p_split_ctx_dst->w[i], p_split_ctx_dst->h[i], p_split_ctx_dst->f8b[i]);
                 //p_split_ctx_dst->crop_meta_data_rltb[i][0],
                 //p_split_ctx_dst->crop_meta_data_rltb[i][1], 
                 //p_split_ctx_dst->crop_meta_data_rltb[i][2], 
                 //p_split_ctx_dst->crop_meta_data_rltb[i][3]);
      }
      if (p_split_ctx_dst->enabled == 0)
      {
          for (i = 0; i < 3; i++)
          {
              s->src_ctx.w[i] = inlink->w;
              s->src_ctx.h[i] = inlink->h;
              s->src_ctx.f[i] = -1;
              s->src_ctx.f8b[i] = -1;
          }
      }
  }
  
  return 0;
}



static int filter_ni_frame(AVFilterLink *inlink, AVFrame *frame)
{
    AVFilterContext *ctx = inlink->dst;
    SplitContext *s = ctx->priv;
    int i, ret = AVERROR_EOF;
    int output_index;
#if 0
    bool use_crop_from_ctx = true;

    if (!frame->buf[1])
    {
        use_crop_from_ctx = false; //source frame not from super frame
    }
#endif
    for (i = 0; i < ctx->nb_outputs; i++) 
    {
        AVFrame *buf_out;

        if (ctx->outputs[i]->status_in)
            continue;

        buf_out = av_frame_alloc();
        if (!buf_out) {
          ret = AVERROR(ENOMEM);
          break;
        }
        av_frame_copy_props(buf_out, frame);

        //buf_out->width = frame->width;  //update with real frame info when multioutput ready
        //buf_out->height = frame->height;//
        buf_out->format = frame->format;
        //buf_out->hw_frames_ctx = av_buffer_ref(frame->hw_frames_ctx);//av_buffer_ref(src->hw_frames_ctx);

        if (i < s->nb_output0)
        {
            output_index = 0;
        }
        else if (i < s->nb_output0 + s->nb_output1)
        {
            if (!frame->buf[1])
            {
                ret = AVERROR(ENOMEM);
                av_frame_free(&buf_out);
                break;
            }
            output_index = 1;
        }
        else
        {
            if (!frame->buf[2])
            {
                ret = AVERROR(ENOMEM);
                av_frame_free(&buf_out);
                break;
            }
            output_index = 2;
        }
        buf_out->buf[0]        = av_buffer_ref(frame->buf[output_index]);
        buf_out->hw_frames_ctx = av_buffer_ref(s->out_frames_ref[output_index]);
#if 0
        if (use_crop_from_ctx) //overwrites work from av_frame_copy_props
        {
            buf_out->crop_right = s->src_ctx.crop_meta_data_rltb[output_index][0];
            buf_out->crop_left = s->src_ctx.crop_meta_data_rltb[output_index][1];
            buf_out->crop_top = s->src_ctx.crop_meta_data_rltb[output_index][2];
            buf_out->crop_bottom = s->src_ctx.crop_meta_data_rltb[output_index][3];
        }
#endif 
        buf_out->data[3] = buf_out->buf[0]->data;
        niFrameSurface1_t* p_data3 = (niFrameSurface1_t*)((uint8_t*)buf_out->data[3]);
        
        buf_out->width = ctx->outputs[i]->w = p_data3->ui16width;
        buf_out->height = ctx->outputs[i]->h = p_data3->ui16height;
        

        av_log(ctx, AV_LOG_DEBUG, "output %d supplied WxH = %d x %d FID %d offset %d\n",
               i, buf_out->width, buf_out->height,
                p_data3->ui16FrameIdx, p_data3->ui32nodeAddress);

        ret = ff_filter_frame(ctx->outputs[i], buf_out);
        if (ret < 0)
            break;
    }
    return ret;
}

static int filter_std_frame(AVFilterLink *inlink, AVFrame *frame)
{//basically clone of native split
    AVFilterContext *ctx = inlink->dst;
    SplitContext *s = ctx->priv;
    int i, ret = AVERROR_EOF;
    if (s->nb_output0 < 2)
    {
        av_log(ctx, AV_LOG_ERROR, "ni_split must have at least 2 outputs for Standard split!\n");
        ret = AVERROR(EINVAL);
        return ret;
    }
    if (s->nb_output1) {
        av_log(ctx, AV_LOG_ERROR, "ni_split output1 or output2 param must not be used for Standard splitting!\n");
        ret = AVERROR(E2BIG);
        return ret;
    }

    for (i = 0; i < ctx->nb_outputs; i++) {
        AVFrame *buf_out;

        if (ctx->outputs[i]->status_in)
            continue;
        buf_out = av_frame_clone(frame);
        if (!buf_out) {
            ret = AVERROR(ENOMEM);
            break;
        }

        ret = ff_filter_frame(ctx->outputs[i], buf_out);
        if (ret < 0)
            break;
    }
    return ret;
}

static int filter_frame(AVFilterLink *inlink, AVFrame *frame) //separate hw and sw into different function calls
{
    AVFilterContext *ctx = inlink->dst;
    SplitContext *s = ctx->priv;
    int ret              = AVERROR_EOF;
    av_log(ctx, AV_LOG_TRACE, "out0,1,2 = %d %d %d total = %d\n",
        s->nb_output0, s->nb_output1, s->nb_output2,
        ctx->nb_outputs);
    av_log(ctx, AV_LOG_DEBUG, "ni_split: filter_frame, in format=%d, Sctx %d\n",
      frame->format,
      s->src_ctx.enabled);
    if (frame->format == AV_PIX_FMT_NI_QUAD && s->src_ctx.enabled == 1)
    {
        ret = filter_ni_frame(inlink, frame);
    }
    else
    {
        ret = filter_std_frame(inlink, frame);
    }
    av_frame_free(&frame);
    return ret;
}

#define OFFSET(x) offsetof(SplitContext, x)
#define FLAGS (AV_OPT_FLAG_VIDEO_PARAM | AV_OPT_FLAG_FILTERING_PARAM)
static const AVOption options[] = {
    { "output0", "Copies of output0", OFFSET(nb_output0), AV_OPT_TYPE_INT, { .i64 = 2 }, 0, INT_MAX, FLAGS },
    { "output1", "Copies of output1", OFFSET(nb_output1), AV_OPT_TYPE_INT, { .i64 = 0 }, 0, INT_MAX, FLAGS },
    { "output2", "Copies of output2", OFFSET(nb_output2), AV_OPT_TYPE_INT, { .i64 = 0 }, 0, INT_MAX, FLAGS },
    { NULL }
};

#define split_options options
AVFILTER_DEFINE_CLASS(split);

static av_cold void split_uninit(AVFilterContext *ctx)
{
  int i;
  SplitContext *s = ctx->priv;
  for (i = 0; i < ctx->nb_outputs-1; i++)
    av_freep(&ctx->output_pads[i].name);

  for (i = 0; i < 3; i++)
  {
    if(s->out_frames_ref[i])
      av_buffer_unref(&s->out_frames_ref[i]);
  }
}

static const AVFilterPad avfilter_vf_split_inputs[] = {
    {
        .name         = "default",
        .type         = AVMEDIA_TYPE_VIDEO,
        .config_props = config_input,
        .filter_frame = filter_frame,
    },
#if (LIBAVFILTER_VERSION_MAJOR < 8)
    { NULL }
#endif
};

AVFilter ff_vf_split_ni_quadra = {
    .name = "ni_quadra_split",
    .description =
        NULL_IF_CONFIG_SMALL("NetInt Quadra pass on the input to N video outputs v" NI_XCODER_REVISION),
    .priv_size      = sizeof(SplitContext),
    .priv_class     = &split_class,
    .init           = split_init,
    .uninit         = split_uninit,
    .flags          = AVFILTER_FLAG_DYNAMIC_OUTPUTS,
    .flags_internal = FF_FILTER_FLAG_HWFRAME_AWARE,
#if (LIBAVFILTER_VERSION_MAJOR >= 8)
    FILTER_INPUTS(avfilter_vf_split_inputs),
    FILTER_QUERY_FUNC(query_formats),
#else
    .inputs         = avfilter_vf_split_inputs,
    .query_formats  = query_formats,
#endif
};
