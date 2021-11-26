/*
 * XCoder Codec Lib Wrapper
 * Copyright (c) 2018 NetInt
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
 * XCoder codec lib wrapper header.
 */

#ifndef AVCODEC_NICODEC_H
#define AVCODEC_NICODEC_H

#include <stdbool.h>
#include <time.h>
#include "avcodec.h"
#include "libavutil/fifo.h"
#include "libavutil/hwcontext.h"
#include "libavutil/hwcontext_ni.h"

#include <ni_device_api.h>

#define NI_NAL_VPS_BIT                  (0x01)
#define NI_NAL_SPS_BIT                  (0x01<<1)
#define NI_NAL_PPS_BIT                  (0x01<<2)
#define NI_GENERATE_ALL_NAL_HEADER_BIT  (0x01<<3)

/* enum for specifying xcoder device/coder index; can be specified in either
   decoder or encoder options. */
enum {
  BEST_DEVICE_INST = -2,
  BEST_DEVICE_LOAD = -1
};

/* enum for specifying hardware accelbrate index */
enum {
  HW_FRAMES_OFF = 0,
  HW_FRAMES_ON = 1
};

typedef struct XCoderH264DecContext {
  AVClass *avclass;

  char *dev_xcoder;         /* from the user command, which device allocation method we use */
  char *dev_xcoder_name;    /* dev name of the xcoder card to use */
  char *blk_xcoder_name;    /* blk name of the xcoder card to use */
  int  dev_dec_idx;         /* index of the decoder on the xcoder card */
  int  nvme_io_size;        /* custom nvme io size */
  int  keep_alive_timeout;    /* keep alive timeout setting */
  ni_device_context_t *rsrc_ctx;  /* resource management context */

  ni_session_context_t api_ctx;
  ni_encoder_params_t  api_param;
  ni_session_data_io_t   api_pkt;
  AVPacket buffered_pkt;

  // stream header copied/saved from AVCodecContext.extradata
  int got_first_idr;
  uint8_t *extradata;
  int extradata_size;

  int64_t current_pts;
  unsigned long long offset;

  int started;
  int draining;
  int flushing;
  int eos;
  int vpu_reset;
  AVHWFramesContext    *hwfc;

  /* below are all command line options */
  char *xcoder_opts;
  int enable_user_data_sei_passthru;
  int enable_check_packet;  // check source packet. Skip SEI payloads after VCL
  int custom_sei;
  int low_delay;
  int pkt_nal_bitmap;
  int hwFrames;
} XCoderH264DecContext;

typedef struct XCoderH265EncContext {
  AVClass *avclass;

  char *dev_xcoder;         /* from the user command, which device allocation method we use */
  char *dev_xcoder_name;    /* dev name of the xcoder card to use */
  char *blk_xcoder_name;    /* blk name of the xcoder card to use */
  int  dev_enc_idx;         /* index of the encoder on the xcoder card */
  int  nvme_io_size;        /* custom nvme io size */
  uint8_t d_serial_number[20]; /*Serial number of card (dec) in use*/
  uint8_t e_serial_number[20]; /*Serial number of card (enc) in use*/
  int  keep_alive_timeout;    /* keep alive timeout setting */
  ni_device_context_t *rsrc_ctx;  /* resource management context */
  unsigned long xcode_load_pixel; /* xcode load in pixels by this encode task */

  // frame fifo, to be used for sequence change frame buffering
  AVFifoBuffer *fme_fifo;
  int fme_fifo_capacity;
  int eos_fme_received;
  AVFrame buffered_fme;

  ni_session_data_io_t  api_pkt; /* used for receiving bitstream from xcoder */
  ni_session_data_io_t   api_fme; /* used for sending YUV data to xcoder */
  ni_session_context_t api_ctx;
  ni_encoder_params_t  api_param;

  int started;
  uint8_t *p_spsPpsHdr;
  int spsPpsHdrLen;
  int spsPpsArrived;
  int firstPktArrived;
  int64_t dtsOffset;
  int gop_offset_count;/*this is a counter to guess the pts only dtsOffset times*/
  uint64_t total_frames_received;
  int64_t first_frame_pts;
  int64_t latest_dts;
  int vpu_reset;
  int encoder_flushing;
  int encoder_eof;

  // ROI
  int roi_side_data_size;
  AVRegionOfInterest *av_rois;  // last passed in AVRegionOfInterest
  int nb_rois;
  ni_enc_avc_roi_custom_map_t *avc_roi_map; // actual AVC/HEVC map(s)
  uint8_t *hevc_sub_ctu_roi_buf;
  ni_enc_hevc_roi_custom_map_t *hevc_roi_map;

  /* backup copy of original values of -enc command line option */
  int  orig_dev_enc_idx;

  // for hw trancoding
  // refer the hw frame when sending to encoder,
  // unrefer the hw frame after received the encoded packet.
  // Then it can recycle the HW frame buffer
  AVFrame *sframe_pool[MAX_NUM_FRAMEPOOL_HWAVFRAME];
  int aFree_Avframes_list[MAX_NUM_FRAMEPOOL_HWAVFRAME+1];
  int freeHead;
  int freeTail;

 /* below are all command line options */
  char *xcoder_opts;
  char *xcoder_gop;

  int reconfigCount;
  ni_encoder_change_params_t *g_enc_change_params;

  // low delay mode flags
  int gotPacket; /* used to stop receiving packets when a packet is already received */
  int sentFrame; /* used to continue receiving packets when a frame is sent and a packet is not yet received */

  // HRD parameters
  uint32_t au_cpb_removal_delay_length_minus1;
  uint32_t dpb_output_delay_length_minus1;
  uint32_t initial_cpb_removal_delay_length_minus1;
  int64_t bit_rate_unscale;
  int64_t cpb_size_unscale;
  uint32_t au_cpb_removal_delay_minus1;

} XCoderH265EncContext;

int ff_xcoder_dec_close(AVCodecContext *avctx,
                        XCoderH264DecContext *s);

int ff_xcoder_dec_init(AVCodecContext *avctx,
                       XCoderH264DecContext *s);

int ff_xcoder_dec_send(AVCodecContext *avctx,
                       XCoderH264DecContext *s,
                       AVPacket *pkt);

int ff_xcoder_dec_receive(AVCodecContext *avctx,
                          XCoderH264DecContext *s,
                          AVFrame *frame,
                          bool wait);

int ff_xcoder_dec_is_flushing(AVCodecContext *avctx,
                              XCoderH264DecContext *s);

int ff_xcoder_dec_flush(AVCodecContext *avctx,
                        XCoderH264DecContext *s);

int retrieve_frame(AVCodecContext *avctx, AVFrame *data, int *got_frame,
                   ni_frame_t *xfme);
int ff_xcoder_add_headers(AVCodecContext *avctx, AVPacket *pkt);
#endif /* AVCODEC_NICODEC_H */
