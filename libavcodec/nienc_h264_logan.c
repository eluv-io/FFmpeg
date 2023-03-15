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

#include "nienc_logan.h"


#define OFFSETENC(x) offsetof(XCoderLoganEncContext, x)
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
    
  { "keep_alive_timeout",       "Specify a custom session keep alive timeout in seconds.", OFFSETENC(keep_alive_timeout),
    AV_OPT_TYPE_INT, { .i64 = NI_LOGAN_DEFAULT_KEEP_ALIVE_TIMEOUT }, NI_LOGAN_MIN_KEEP_ALIVE_TIMEOUT, NI_LOGAN_MAX_KEEP_ALIVE_TIMEOUT, VE, "keep_alive_timeout" },

  { "xcoder-params", "Set the XCoder configuration using a :-separated list of key=value parameters", OFFSETENC(xcoder_opts), 
    AV_OPT_TYPE_STRING, { 0 }, 0, 0, VE },

  { "xcoder-gop", "Set the XCoder custom gop using a :-separated list of key=value parameters", OFFSETENC(xcoder_gop), 
  AV_OPT_TYPE_STRING, { 0 }, 0, 0, VE },

  { "set_high_priority",       "Specify a custom session set high priority in 0 or 1", OFFSETENC(set_high_priority),
    AV_OPT_TYPE_INT, { .i64 = 0 }, 0, 1, VE, "set_high_priority" },


  { NULL }
};

static const AVClass h264_xcoderenc_class = {
  .class_name = "h264_ni_logan_enc",
  .item_name = av_default_item_name,
  .option = enc_options,
  .version = LIBAVUTIL_VERSION_INT,
};

AVCodec ff_h264_ni_logan_encoder = {
  .name           = "h264_ni_logan_enc",
  .long_name      = NULL_IF_CONFIG_SMALL("H.264 NetInt Logan encoder v" NI_LOGAN_XCODER_REVISION),
  .type           = AVMEDIA_TYPE_VIDEO,
  .id             = AV_CODEC_ID_H264,
  .init           = ff_xcoder_logan_encode_init,
  .receive_packet = ff_xcoder_logan_receive_packet,
  .encode2        = ff_xcoder_logan_encode_frame,
  .close          = ff_xcoder_logan_encode_close,
  .priv_data_size = sizeof(XCoderLoganEncContext),
  .priv_class     = &h264_xcoderenc_class,
  .capabilities   = AV_CODEC_CAP_DELAY,
  .pix_fmts = (const enum AVPixelFormat[]) {
                                            AV_PIX_FMT_YUV420P,
                                            AV_PIX_FMT_YUV420P10BE,
                                            AV_PIX_FMT_YUV420P10LE,
                                            AV_PIX_FMT_YUVJ420P,
                                            AV_PIX_FMT_NI_LOGAN,
                                            AV_PIX_FMT_NONE},
// Needed for yuvbypass on FFmpeg-n4.3+
#if (LIBAVCODEC_VERSION_MAJOR >= 59 || LIBAVCODEC_VERSION_MAJOR >= 58 && LIBAVCODEC_VERSION_MINOR >= 82)
  .hw_configs     = ff_ni_logan_enc_hw_configs,
#endif
};
