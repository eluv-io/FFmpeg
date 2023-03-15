/*
 * NetInt XCoder H.264 Encoder
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

#include "nienc.h"

static const AVOption enc_options[] = {
    {"enc",
     "Select which encoder to use by index. First is 0, second is 1, and so "
     "on.",
     OFFSETENC(dev_enc_idx),
     AV_OPT_TYPE_INT,
     {.i64 = BEST_DEVICE_LOAD},
     -1,
     INT_MAX,
     VE,
     "enc"},

    {"encname",
     "Select which encoder to use by NVMe block device name, e.g. "
     "/dev/nvme0n1.",
     OFFSETENC(dev_blk_name),
     AV_OPT_TYPE_STRING,
     {0},
     0,
     0,
     VE,
     "encname"},

    {"iosize",
     "Specify a custom NVMe IO transfer size (multiples of 4096 only).",
     OFFSETENC(nvme_io_size),
     AV_OPT_TYPE_INT,
     {.i64 = BEST_DEVICE_LOAD},
     -1,
     INT_MAX,
     VE,
     "iosize"},
    {"xcoder-params",
     "Set the XCoder configuration using a :-separated list of key=value "
     "parameters.",
     OFFSETENC(xcoder_opts),
     AV_OPT_TYPE_STRING,
     {0},
     0,
     0,
     VE},
    {"xcoder-gop",
     "Set the XCoder custom gop using a :-separated list of key=value "
     "parameters.",
     OFFSETENC(xcoder_gop),
     AV_OPT_TYPE_STRING,
     {0},
     0,
     0,
     VE},

    {"keep_alive_timeout",
     "Specify a custom session keep alive timeout in seconds.",
     OFFSETENC(keep_alive_timeout),
     AV_OPT_TYPE_INT,
     {.i64 = NI_DEFAULT_KEEP_ALIVE_TIMEOUT},
     NI_MIN_KEEP_ALIVE_TIMEOUT,
     NI_MAX_KEEP_ALIVE_TIMEOUT,
     VE,
     "keep_alive_timeout"},
    {NULL}};

static const AVClass jpeg_xcoderenc_class = {
    .class_name = "jpeg_ni_quadra_enc",
    .item_name  = av_default_item_name,
    .option     = enc_options,
    .version    = LIBAVUTIL_VERSION_INT,
};

AVCodec ff_jpeg_ni_quadra_encoder = {
    .name = "jpeg_ni_quadra_enc",
    .long_name =
        NULL_IF_CONFIG_SMALL("JPEG NetInt Quadra encoder v" NI_XCODER_REVISION),
    .type = AVMEDIA_TYPE_VIDEO,
    .id   = AV_CODEC_ID_MJPEG,
    .init = xcoder_encode_init,
// FFmpeg-n4.4+ has no more .send_frame;
#if (LIBAVCODEC_VERSION_MAJOR >= 59 || LIBAVCODEC_VERSION_MAJOR >= 58 && LIBAVCODEC_VERSION_MINOR >= 134)
    .receive_packet = ff_xcoder_receive_packet,
#else
    .send_frame     = xcoder_send_frame,
    .receive_packet = xcoder_receive_packet,
#endif
    .encode2        = xcoder_encode_frame,
    .close          = xcoder_encode_close,
    .priv_data_size = sizeof(XCoderH265EncContext),
    .priv_class     = &jpeg_xcoderenc_class,
    .capabilities   = AV_CODEC_CAP_DELAY,
    .pix_fmts = // Quadra encoder preprocessor can convert 10-bit input to 8-bit
                // before encoding to Jpeg
    (const enum AVPixelFormat[]){AV_PIX_FMT_YUVJ420P, AV_PIX_FMT_NI_QUAD,
                                 AV_PIX_FMT_NONE},
// Needed for hwframe on FFmpeg-n4.3+
#if (LIBAVCODEC_VERSION_MAJOR >= 59 || LIBAVCODEC_VERSION_MAJOR >= 58 && LIBAVCODEC_VERSION_MINOR >= 82)
    .hw_configs = ff_ni_enc_hw_configs,
#endif
};
