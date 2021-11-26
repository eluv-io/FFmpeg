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

#ifndef AVCODEC_NIENC_H
#define AVCODEC_NIENC_H

#include <ni_rsrc_api.h>
#include <ni_util.h>
#include <ni_device_api.h>

#include "libavutil/internal.h"

#include "avcodec.h"
#include "internal.h"
#include "libavutil/opt.h"
#include "libavutil/imgutils.h"
// Needed for yuvbypass on FFmpeg-n4.3+
#if ((LIBAVCODEC_VERSION_MAJOR >= 58) && (LIBAVCODEC_VERSION_MINOR >= 82))
#include "hwconfig.h"
#endif

#include "nicodec.h"

int xcoder_encode_init(AVCodecContext *avctx);
  
int xcoder_encode_close(AVCodecContext *avctx);

int xcoder_send_frame(AVCodecContext *avctx, const AVFrame *frame);

int xcoder_receive_packet(AVCodecContext *avctx, AVPacket *pkt);

int xcoder_encode_frame(AVCodecContext *avctx, AVPacket *pkt,
const AVFrame *frame, int *got_packet);

bool free_frames_isempty(XCoderH265EncContext *ctx);

bool free_frames_isfull(XCoderH265EncContext *ctx);

int deq_free_frames(XCoderH265EncContext *ctx);

int enq_free_frames(XCoderH265EncContext *ctx, int idx);

int recycle_index_2_avframe_index(XCoderH265EncContext *ctx, uint32_t recycleIndex);

// Needed for yuvbypass on FFmpeg-n4.3+
#if ((LIBAVCODEC_VERSION_MAJOR >= 58) && (LIBAVCODEC_VERSION_MINOR >= 82))
extern const AVCodecHWConfigInternal *ff_ni_enc_hw_configs[];
#endif

#endif /* AVCODEC_NIENC_H */
