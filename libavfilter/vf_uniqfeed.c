/*
 * uniqFEED video filter
 */

#include "config_components.h"

#include <inttypes.h>
#include <stdio.h>
#include <string.h>

#include "libavutil/error.h"
#include "libavutil/mem.h"
#include "libavutil/opt.h"
#include "avfilter.h"
#include "filters.h"
#include "video.h"
#include "uf_render_metadata_provider.h"

#ifdef USE_UF_RENDERLIB
#include <libswscale/swscale.h>

#ifndef __cplusplus
/* The upstream header uses typedef-style names in C prototypes but only
 * declares struct tags. Provide C aliases locally so it can be consumed from C.
 */
struct UfContext;
struct UfImage;
struct UfMetadata;
struct UfFeeds;
typedef struct UfContext UfContext;
typedef struct UfImage UfImage;
typedef struct UfMetadata UfMetadata;
typedef struct UfFeeds UfFeeds;
typedef int UfImageFormat;
#endif
#include <uf/renderlib/UfRenderInterface.h>
#endif

typedef struct UniqfeedContext {
    const AVClass *class;
    char *project_path;
    char *metadata_dir;
    int passthrough_on_failure;

#ifdef USE_UF_RENDERLIB
    UfContext *ctx;
    const UfMetadataProviderVTable *metadata_provider;
    void *metadata_provider_opaque;
    uint64_t frame_index;
    uint64_t metadata_frame_count;
    uint32_t expected_width;
    uint32_t expected_height;
    int render_disabled;
#endif
} UniqfeedContext;

#define OFFSET(x) offsetof(UniqfeedContext, x)
#define FLAGS AV_OPT_FLAG_FILTERING_PARAM | AV_OPT_FLAG_VIDEO_PARAM

static const AVOption uniqfeed_options[] = {
    { "project_path", "Path to uniqFEED project directory", OFFSET(project_path), AV_OPT_TYPE_STRING, {.str = NULL}, 0, 0, FLAGS },
    { "metadata_dir", "Optional metadata directory with md-XXXXXX.bin files", OFFSET(metadata_dir), AV_OPT_TYPE_STRING, {.str = NULL}, 0, 0, FLAGS },
    { "passthrough_on_failure", "Disable uniqFEED and pass through frames on recoverable errors", OFFSET(passthrough_on_failure), AV_OPT_TYPE_BOOL, {.i64 = 0}, 0, 1, FLAGS },
    { NULL }
};

AVFILTER_DEFINE_CLASS(uniqfeed);

#ifdef USE_UF_RENDERLIB
static uint64_t count_render_metadata_frames(const char *metadata_dir)
{
    FILE *file;
    uint64_t frame_index = 0;

    if (!metadata_dir)
        return 0;

    while (1) {
        char filename[4096];

        if (snprintf(filename, sizeof(filename), "%s/md-%06" PRIu64 ".bin",
                     metadata_dir, frame_index) >= (int)sizeof(filename))
            break;

        file = fopen(filename, "rb");
        if (!file)
            break;

        fclose(file);
        frame_index++;
    }

    return frame_index;
}

static UfMetadata *load_render_metadata_from_provider(UniqfeedContext *s,
                                                      uint64_t frame_index,
                                                      int64_t render_tid,
                                                      const AVFrame *frame)
{
    const UfMetadataProviderVTable *provider = s->metadata_provider;
    uint8_t *metadata_blob = NULL;
    size_t metadata_blob_size = 0;
    UfMetadata *metadata;
    int ret;

    if (!provider || !provider->get_metadata_blob)
        return NULL;

    ret = provider->get_metadata_blob(frame_index,
                                      0,
                                      render_tid,
                                      frame,
                                      &metadata_blob,
                                      &metadata_blob_size,
                                      s->metadata_provider_opaque);
    if (ret < 0)
        return NULL;

    if (!metadata_blob || metadata_blob_size == 0) {
        metadata = uFCreateMetadata(NULL, 0);
    } else if (metadata_blob_size > UINT32_MAX) {
        metadata = NULL;
    } else {
        metadata = uFCreateMetadata(metadata_blob, (uint32_t)metadata_blob_size);
    }

    if (provider->release_metadata_blob)
        provider->release_metadata_blob(metadata_blob,
                                        metadata_blob_size,
                                        s->metadata_provider_opaque);

    return metadata;
}

static UfMetadata *load_render_metadata_from_files(const char *metadata_dir, uint64_t frame_index)
{
    FILE *file;
    UfMetadata *metadata;
    long metadata_size;
    char *filename;
    uint8_t *buffer = NULL;
    int filename_len;

    if (!metadata_dir)
        return uFCreateMetadata(NULL, 0);

    filename_len = snprintf(NULL, 0, "%s/md-%06" PRIu64 ".bin",
                            metadata_dir, frame_index);
    if (filename_len < 0)
        return uFCreateMetadata(NULL, 0);

    filename = av_malloc(filename_len + 1);
    if (!filename)
        return NULL;

    snprintf(filename, filename_len + 1, "%s/md-%06" PRIu64 ".bin",
             metadata_dir, frame_index);

    file = fopen(filename, "rb");
    if (!file) {
        av_free(filename);
        return uFCreateMetadata(NULL, 0);
    }

    if (fseek(file, 0, SEEK_END) < 0) {
        fclose(file);
        av_free(filename);
        return uFCreateMetadata(NULL, 0);
    }

    metadata_size = ftell(file);
    if (metadata_size <= 0 || fseek(file, 0, SEEK_SET) < 0) {
        fclose(file);
        av_free(filename);
        return uFCreateMetadata(NULL, 0);
    }

    buffer = av_malloc(metadata_size);
    if (!buffer) {
        fclose(file);
        av_free(filename);
        return NULL;
    }

    if (fread(buffer, 1, metadata_size, file) != (size_t)metadata_size) {
        av_free(buffer);
        fclose(file);
        av_free(filename);
        return uFCreateMetadata(NULL, 0);
    }

    fclose(file);
    metadata = uFCreateMetadata(buffer, metadata_size);
    if (!metadata)
        av_free(buffer);
    av_free(filename);

    return metadata;
}

static UfImage *create_render_image_from_frame(const AVFrame *frame)
{
    struct SwsContext *scale_ctx;
    UfImage *image;
    uint8_t *dst_data;
    uint32_t dst_stride;

    image = uFCreateImage(frame->width, frame->height, R8G8B8_UINT);
    if (!image)
        return NULL;

    if (uFGetImageHostBuffer(image, (void **)&dst_data) != 0 || !dst_data ||
        uFGetImageStride(image, &dst_stride) != 0) {
        uFDestroyImage(image);
        return NULL;
    }

    scale_ctx = sws_getContext(frame->width, frame->height, frame->format,
                               frame->width, frame->height, AV_PIX_FMT_RGB24,
                               SWS_BILINEAR, NULL, NULL, NULL);
    if (!scale_ctx) {
        uFDestroyImage(image);
        return NULL;
    }

    {
        uint8_t *rgb_data[4] = { dst_data, NULL, NULL, NULL };
        int rgb_linesize[4] = { (int)dst_stride, 0, 0, 0 };

        sws_scale(scale_ctx, (const uint8_t * const *)frame->data, frame->linesize,
                  0, frame->height, rgb_data, rgb_linesize);
    }

    sws_freeContext(scale_ctx);
    return image;
}

static int create_frame_from_render_image(const UfImage *image,
                                          const AVFrame *source_frame,
                                          AVFrame **processed_frame)
{
    struct SwsContext *scale_ctx;
    UfImage *rgb_image = NULL;
    AVFrame *frame;
    uint8_t *rgb_data;
    uint32_t width;
    uint32_t height;
    uint32_t stride;
    UfImageFormat format;
    int ret;

    ret = uFGetImageFormat(image, &format);
    if (ret != 0)
        return AVERROR_EXTERNAL;

    if (format != R8G8B8_UINT) {
        rgb_image = uFConvertImage(image, R8G8B8_UINT);
        if (!rgb_image)
            return AVERROR_EXTERNAL;
        image = rgb_image;
    }

    if (uFGetImageSize(image, &width, &height) != 0 ||
        uFGetImageHostBuffer(image, (void **)&rgb_data) != 0 ||
        uFGetImageStride(image, &stride) != 0 || !rgb_data) {
        ret = AVERROR_EXTERNAL;
        goto end;
    }

    if ((int)width != source_frame->width || (int)height != source_frame->height) {
        ret = AVERROR(EINVAL);
        goto end;
    }

    frame = av_frame_alloc();
    if (!frame)
        return AVERROR(ENOMEM);

    frame->format = source_frame->format;
    frame->width = source_frame->width;
    frame->height = source_frame->height;

    ret = av_frame_get_buffer(frame, 32);
    if (ret < 0) {
        av_frame_free(&frame);
        return ret;
    }

    scale_ctx = sws_getContext(source_frame->width, source_frame->height, AV_PIX_FMT_RGB24,
                               source_frame->width, source_frame->height, source_frame->format,
                               SWS_BILINEAR, NULL, NULL, NULL);
    if (!scale_ctx) {
        av_frame_free(&frame);
        ret = AVERROR(EINVAL);
        goto end;
    }

    {
        const uint8_t *src_data[4] = { rgb_data, NULL, NULL, NULL };
        int src_linesize[4] = { (int)stride, 0, 0, 0 };

        sws_scale(scale_ctx, src_data, src_linesize, 0, source_frame->height,
                  frame->data, frame->linesize);
    }

    sws_freeContext(scale_ctx);

    ret = av_frame_copy_props(frame, source_frame);
    if (ret < 0) {
        av_frame_free(&frame);
        goto end;
    }

    frame->pts = source_frame->pts;
    frame->best_effort_timestamp = source_frame->best_effort_timestamp;
    *processed_frame = frame;
    ret = 0;

end:
    if (rgb_image)
        uFDestroyImage(rgb_image);
    return ret;
}

static int uniqfeed_process_frame(AVFilterContext *ctx, AVFrame *frame, AVFrame **processed_frame)
{
    UniqfeedContext *s = ctx->priv;
    UfFeeds *feeds = NULL;
    UfImage *input_image = NULL;
    UfMetadata *metadata = NULL;
    const UfImage *feed_image = NULL;
    int ret;
    uint64_t frame_index = s->frame_index++;
    int64_t render_tid = frame->pts == AV_NOPTS_VALUE ? (int64_t)frame_index : frame->pts;

    *processed_frame = NULL;

    if (frame->width != (int)s->expected_width || frame->height != (int)s->expected_height)
        return AVERROR(EINVAL);

    if (s->metadata_frame_count > 0 && frame_index >= s->metadata_frame_count)
        return AVERROR(EINVAL);

    input_image = create_render_image_from_frame(frame);
    if (!input_image) {
        ret = AVERROR_EXTERNAL;
        goto end;
    }

    if (s->metadata_provider) {
        metadata = load_render_metadata_from_provider(s, frame_index, render_tid, frame);
        if (!metadata && s->metadata_dir)
            metadata = load_render_metadata_from_files(s->metadata_dir, frame_index);
    } else {
        metadata = load_render_metadata_from_files(s->metadata_dir, frame_index);
    }

    if (!metadata) {
        ret = AVERROR_EXTERNAL;
        goto end;
    }

    feeds = uFRenderFeeds(s->ctx, metadata, render_tid, input_image);
    if (!feeds) {
        ret = 0;
        goto end;
    }

    feed_image = uFGetFeedsImage(feeds, 0);
    if (!feed_image) {
        ret = AVERROR_EXTERNAL;
        goto end;
    }

    ret = create_frame_from_render_image(feed_image, frame, processed_frame);

end:
    if (feeds)
        uFDestroyFeeds(feeds);
    if (metadata)
        uFDestroyMetadata(metadata);
    if (input_image)
        uFDestroyImage(input_image);

    return ret;
}
#endif

static av_cold int uniqfeed_init(AVFilterContext *ctx)
{
#ifdef USE_UF_RENDERLIB
    UniqfeedContext *s = ctx->priv;

    if (!s->project_path || !*s->project_path) {
        av_log(ctx, AV_LOG_ERROR, "project_path must be set\n");
        return AVERROR(EINVAL);
    }

    s->ctx = uFCreateContext(s->project_path);
    if (!s->ctx) {
        av_log(ctx, AV_LOG_ERROR, "failed to initialize uniqFEED context\n");
        return AVERROR_EXTERNAL;
    }

    if (uFGetContextResolution(s->ctx, &s->expected_width, &s->expected_height) != 0) {
        av_log(ctx, AV_LOG_ERROR, "failed to query uniqFEED context resolution\n");
        return AVERROR_EXTERNAL;
    }

    s->metadata_provider = NULL;
    s->metadata_provider_opaque = NULL;
    s->frame_index = 0;
    s->render_disabled = 0;

    if (uFGetExternalMetadataProviderV1)
        s->metadata_provider = uFGetExternalMetadataProviderV1();

    if (s->metadata_provider && s->metadata_provider->init) {
        int provider_ret = s->metadata_provider->init(s->project_path,
                                                      s->metadata_dir,
                                                      &s->metadata_provider_opaque);
        if (provider_ret < 0) {
            av_log(ctx, AV_LOG_ERROR, "metadata provider init failed: %d\n", provider_ret);
            return AVERROR_EXTERNAL;
        }
    }

    if (s->metadata_provider) {
        s->metadata_frame_count = 0;
    } else {
        s->metadata_frame_count = count_render_metadata_frames(s->metadata_dir);
    }

    return 0;
#else
    av_log(ctx, AV_LOG_ERROR,
           "uniqfeed filter was built without USE_UF_RENDERLIB support\n");
    return AVERROR(ENOSYS);
#endif
}

static av_cold void uniqfeed_uninit(AVFilterContext *ctx)
{
#ifdef USE_UF_RENDERLIB
    UniqfeedContext *s = ctx->priv;

    if (s->metadata_provider && s->metadata_provider->close)
        s->metadata_provider->close(s->metadata_provider_opaque);

    if (s->ctx)
        uFDestroyContext(s->ctx);

    s->ctx = NULL;
    s->metadata_provider = NULL;
    s->metadata_provider_opaque = NULL;
#endif
}

static int filter_frame(AVFilterLink *inlink, AVFrame *frame)
{
    AVFilterContext *ctx = inlink->dst;
    UniqfeedContext *s = ctx->priv;
    AVFrame *processed_frame = NULL;
    int ret;

#ifdef USE_UF_RENDERLIB
    if (s->render_disabled)
        return ff_filter_frame(ctx->outputs[0], frame);

    ret = uniqfeed_process_frame(ctx, frame, &processed_frame);
    if (ret < 0) {
        if (s->passthrough_on_failure) {
            s->render_disabled = 1;
            av_log(ctx, AV_LOG_WARNING,
                   "uniqFEED failed (%s); disabling uniqFEED and passing through\n",
                   av_err2str(ret));
            return ff_filter_frame(ctx->outputs[0], frame);
        }
        av_frame_free(&frame);
        return ret;
    }

    if (processed_frame) {
        av_frame_free(&frame);
        frame = processed_frame;
    }
#endif

    return ff_filter_frame(ctx->outputs[0], frame);
}

static const AVFilterPad uniqfeed_inputs[] = {
    {
        .name         = "default",
        .type         = AVMEDIA_TYPE_VIDEO,
        .filter_frame = filter_frame,
    },
};

const FFFilter ff_vf_uniqfeed = {
    .p.name        = "uniqfeed",
    .p.description = NULL_IF_CONFIG_SMALL("Apply uniqFEED frame processing."),
    .p.priv_class  = &uniqfeed_class,
    .priv_size     = sizeof(UniqfeedContext),
    .init          = uniqfeed_init,
    .uninit        = uniqfeed_uninit,
    FILTER_INPUTS(uniqfeed_inputs),
    FILTER_OUTPUTS(ff_video_default_filterpad),
};
