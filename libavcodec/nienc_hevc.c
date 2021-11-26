/*
 * NetInt XCoder HEVC Encoder
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


#define OFFSETENC(x) offsetof(XCoderH265EncContext, x)
#define VE AV_OPT_FLAG_VIDEO_PARAM | AV_OPT_FLAG_ENCODING_PARAM
static const AVOption enc_options[] = {
  { "xcoder",    "Select which XCoder card to use.",  OFFSETENC(dev_xcoder),
    AV_OPT_TYPE_STRING, { .str = "bestload" }, CHAR_MIN, CHAR_MAX, VE, "xcoder" },
  { "bestload",      "Pick the least loaded XCoder/encoder available.", 0, AV_OPT_TYPE_CONST,
    { .str = "bestload" }, 0, 0, VE, "xcoder" },

  { "bestinst",      "Pick the XCoder/encoder with the least number of running encoding instances.", 0, AV_OPT_TYPE_CONST,
    { .str = "bestinst" }, 0, 0, VE, "xcoder" },

  { "list",      "List the available XCoder cards.", 0, AV_OPT_TYPE_CONST,
    { .str = "list" }, 0, 0, VE, "xcoder" },

  { "enc",       "Select which encoder to use by index. First is 0, second is 1, and so on.", OFFSETENC(dev_enc_idx),
    AV_OPT_TYPE_INT, { .i64 = BEST_DEVICE_LOAD }, -1, INT_MAX, VE, "enc" },
  
  { "iosize",       "Specify a custom NVMe IO transfer size (multiples of 4096 only).", OFFSETENC(nvme_io_size),
    AV_OPT_TYPE_INT, { .i64 = BEST_DEVICE_LOAD }, -1, INT_MAX, VE, "iosize" },
    
  { "keep_alive_timeout",       "Specify a custom session keep alive timeout in seconds.", OFFSETENC(keep_alive_timeout),
    AV_OPT_TYPE_INT, { .i64 = NI_DEFAULT_KEEP_ALIVE_TIMEOUT }, NI_MIN_KEEP_ALIVE_TIMEOUT, NI_MAX_KEEP_ALIVE_TIMEOUT, VE, "keep_alive_timeout" },

  { "xcoder-params", "Set the XCoder configuration using a :-separated list of key=value parameters", OFFSETENC(xcoder_opts), 
    AV_OPT_TYPE_STRING, { 0 }, 0, 0, VE },

  { "xcoder-gop", "Set the XCoder custom gop using a :-separated list of key=value parameters", OFFSETENC(xcoder_gop), 
  AV_OPT_TYPE_STRING, { 0 }, 0, 0, VE },



  { NULL }
};

static const AVClass h265_xcoderenc_class = {
  .class_name = "h265_ni_enc",
  .item_name = av_default_item_name,
  .option = enc_options,
  .version = LIBAVUTIL_VERSION_INT,
};

AVCodec ff_h265_ni_encoder = {
  .name           = "h265_ni_enc",
  .long_name      = NULL_IF_CONFIG_SMALL("H.265 NetInt encoder v" NI_XCODER_REVISION),
  .type           = AVMEDIA_TYPE_VIDEO,
  .id             = AV_CODEC_ID_H265,
  .init           = xcoder_encode_init,
//  .send_frame     = xcoder_send_frame,
//  .receive_packet = xcoder_receive_packet,
  .encode2        = xcoder_encode_frame,
  .close          = xcoder_encode_close,
  .priv_data_size = sizeof(XCoderH265EncContext),
  .priv_class     = &h265_xcoderenc_class,
  .capabilities   = AV_CODEC_CAP_DELAY,
  .pix_fmts = (const enum AVPixelFormat[]) {
                                            AV_PIX_FMT_YUV420P,
                                            AV_PIX_FMT_YUV420P10BE,
                                            AV_PIX_FMT_YUV420P10LE,
                                            AV_PIX_FMT_YUVJ420P,
                                            AV_PIX_FMT_NI,
                                            AV_PIX_FMT_NONE},
// Needed for yuvbypass on FFmpeg-n4.3+
#if ((LIBAVCODEC_VERSION_MAJOR >= 58) && (LIBAVCODEC_VERSION_MINOR >= 82))
  .hw_configs     = ff_ni_enc_hw_configs,
#endif
};
