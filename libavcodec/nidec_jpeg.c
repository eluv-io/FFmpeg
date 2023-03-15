/*
 * XCoder JPEG Decoder
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

#include "nidec.h"
// Needed for hwframe on FFmpeg-n4.3+
#if (LIBAVCODEC_VERSION_MAJOR >= 59 || LIBAVCODEC_VERSION_MAJOR >= 58 && LIBAVCODEC_VERSION_MINOR >= 82)
#include "hwconfig.h"
#else
#include "hwaccel.h"
#endif
#include "profiles.h"

static const AVCodecHWConfigInternal *ff_ni_quad_hw_configs[] = {
  &(const AVCodecHWConfigInternal) {
  .public = {
    .pix_fmt = AV_PIX_FMT_NI_QUAD,
    .methods = AV_CODEC_HW_CONFIG_METHOD_HW_FRAMES_CTX |
    AV_CODEC_HW_CONFIG_METHOD_AD_HOC | AV_CODEC_HW_CONFIG_METHOD_HW_DEVICE_CTX,
    .device_type = AV_HWDEVICE_TYPE_NI_QUADRA,
  },
    .hwaccel = NULL,
},
NULL
};

static const AVOption dec_options[] = {
    {"dec",
     "Select which decoder to use by index. First is 0, second is 1, and so "
     "on.",
     OFFSETDEC(dev_dec_idx),
     AV_OPT_TYPE_INT,
     {.i64 = BEST_DEVICE_LOAD},
     -1,
     INT_MAX,
     VD,
     "dec"},
    {"decname",
     "Select which decoder to use by NVMe block device name, e.g. "
     "/dev/nvme0n1.",
     OFFSETDEC(dev_blk_name),
     AV_OPT_TYPE_STRING,
     {0},
     0,
     0,
     VD,
     "decname"},
    {"xcoder-params",
     "Set the XCoder configuration using a :-separated list of key=value "
     "parameters.",
     OFFSETDEC(xcoder_opts),
     AV_OPT_TYPE_STRING,
     {0},
     0,
     0,
     VD},
    {"keep_alive_timeout",
     "Specify a custom session keep alive timeout in seconds.",
     OFFSETDEC(keep_alive_timeout),
     AV_OPT_TYPE_INT,
     {.i64 = NI_DEFAULT_KEEP_ALIVE_TIMEOUT},
     NI_MIN_KEEP_ALIVE_TIMEOUT,
     NI_MAX_KEEP_ALIVE_TIMEOUT,
     VD,
     "keep_alive_timeout"},
    {NULL},
};

#define JPEG_NI_QUADRA_DEC "jpeg_ni_quadra_dec"

static const AVClass jpeg_xcoderdec_class = {
    .class_name = JPEG_NI_QUADRA_DEC,
    .item_name  = av_default_item_name,
    .option     = dec_options,
    .version    = LIBAVUTIL_VERSION_INT,
};

AVCodec ff_jpeg_ni_quadra_decoder = {
    .name           = JPEG_NI_QUADRA_DEC,
    .long_name      = NULL_IF_CONFIG_SMALL("JPEG NetInt Quadra decoder v" NI_XCODER_REVISION),
    .type           = AVMEDIA_TYPE_VIDEO,
    .id             = AV_CODEC_ID_MJPEG,
    .hw_configs     = ff_ni_quad_hw_configs,
    .caps_internal  = FF_CODEC_CAP_SETS_PKT_DTS,
    .capabilities   = AV_CODEC_CAP_AVOID_PROBING | AV_CODEC_CAP_DELAY | AV_CODEC_CAP_HARDWARE,
    .init           = xcoder_decode_init,
    .close          = xcoder_decode_close,
    .receive_frame  = xcoder_receive_frame,
    .priv_class     = &jpeg_xcoderdec_class,
    .priv_data_size = sizeof(XCoderH264DecContext),
    .pix_fmts       = (const enum AVPixelFormat[]){ AV_PIX_FMT_YUVJ420P, AV_PIX_FMT_NI_QUAD,
                                                    AV_PIX_FMT_NONE },
};
