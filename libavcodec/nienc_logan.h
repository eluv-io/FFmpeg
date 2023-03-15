/*
 * NetInt XCoder H.264/HEVC Encoder common code header
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

#ifndef AVCODEC_NIENC_LOGAN_H
#define AVCODEC_NIENC_LOGAN_H

#include <ni_rsrc_api_logan.h>
#include <ni_util_logan.h>
#include <ni_device_api_logan.h>

#include "libavutil/internal.h"

#include "avcodec.h"
#include "encode.h"
#include "internal.h"
#include "libavutil/opt.h"
#include "libavutil/imgutils.h"
// Needed for yuvbypass on FFmpeg-n4.3+
#if (LIBAVCODEC_VERSION_MAJOR >= 59 || (LIBAVCODEC_VERSION_MAJOR >= 58 && LIBAVCODEC_VERSION_MINOR >= 82))
#include "hwconfig.h"
#endif

#include "nicodec_logan.h"

int ff_xcoder_logan_encode_init(AVCodecContext *avctx);
  
int ff_xcoder_logan_encode_close(AVCodecContext *avctx);

int ff_xcoder_logan_receive_packet(AVCodecContext *avctx, AVPacket *pkt);

int ff_xcoder_logan_encode_frame(AVCodecContext *avctx, AVPacket *pkt,
                           const AVFrame *frame, int *got_packet);

bool free_logan_frames_isempty(XCoderLoganEncContext *ctx);

bool free_logan_frames_isfull(XCoderLoganEncContext *ctx);

int deq_logan_free_frames(XCoderLoganEncContext *ctx);

int enq_logan_free_frames(XCoderLoganEncContext *ctx, int idx);

int recycle_logan_index_2_avframe_index(XCoderLoganEncContext *ctx, uint32_t recycleIndex);

// Needed for yuvbypass on FFmpeg-n4.3+
#if (LIBAVCODEC_VERSION_MAJOR >= 59 || (LIBAVCODEC_VERSION_MAJOR >= 58 && LIBAVCODEC_VERSION_MINOR >= 82))
extern const AVCodecHWConfigInternal *ff_ni_logan_enc_hw_configs[];
#endif

#endif /* AVCODEC_NIENC_LOGAN_H */
