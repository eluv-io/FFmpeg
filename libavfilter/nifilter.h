/*
 * XCoder Filter Lib Wrapper
 *
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
 * XCoder codec lib wrapper.
 */

#ifndef AVFILTER_NIFILTER_H
#define AVFILTER_NIFILTER_H

#include "libavutil/pixdesc.h"
#include "libavutil/imgutils.h"
#include "libavutil/hwcontext_internal.h"
#include "libavutil/hwcontext.h"
#include "libavutil/hwcontext_ni_quad.h"

#include <ni_device_api.h>

#define DEFAULT_NI_FILTER_POOL_SIZE     4

int ff_ni_ffmpeg_to_gc620_pix_fmt(enum AVPixelFormat pix_fmt);
int ff_ni_copy_device_to_host_frame(AVFrame *dst, const ni_frame_t *src, int pix_fmt);
int ff_ni_copy_host_to_device_frame(ni_frame_t *dst, const AVFrame *src, int pix_fmt);
int ff_ni_build_frame_pool(ni_session_context_t *ctx,int width,int height, enum AVPixelFormat out_format,int pool_size);

void ff_ni_frame_free(void *opaque, uint8_t *data);
void ff_ni_clone_hwframe_ctx(AVHWFramesContext *in_frames_ctx,
                             AVHWFramesContext *out_frames_ctx,
                             ni_session_context_t *ctx);

#endif
