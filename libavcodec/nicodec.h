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

#include <ni_device_api.h>
#include "libavutil/hwcontext_ni_quad.h"
#include "libavutil/hwcontext.h"

#define NI_NAL_VPS_BIT (0x01)
#define NI_NAL_SPS_BIT (0x01 << 1)
#define NI_NAL_PPS_BIT (0x01 << 2)
#define NI_GENERATE_ALL_NAL_HEADER_BIT (0x01 << 3)

/* enum for specifying xcoder device/coder index; can be specified in either
   decoder or encoder options. */
enum {
  BEST_DEVICE_INST = -2,
  BEST_DEVICE_LOAD = -1
};

enum {
    HW_FRAMES_OFF = 0,
    HW_FRAMES_ON = 1
};

#ifdef NI_DEC_GSTREAMER_SUPPORT
typedef struct _GsData {
    void *opaque;
    AVBufferRef *buf0;
} GsData;
#endif

typedef struct XCoderH264DecContext {
  AVClass *avclass;

  char *dev_xcoder_name;          /* dev name of the xcoder card to use */
  char *blk_xcoder_name;          /* blk name of the xcoder card to use */
  int dev_dec_idx;                /* user-specified decoder index */
  char *dev_blk_name;             /* user-specified decoder block device name */
  int keep_alive_timeout;         /* keep alive timeout setting */
  ni_device_context_t *rsrc_ctx;  /* resource management context */

  ni_session_context_t api_ctx;
  ni_xcoder_params_t api_param;
  ni_session_data_io_t api_pkt;

  AVPacket buffered_pkt;
  AVPacket lone_sei_pkt;
  
  // stream header copied/saved from AVCodecContext.extradata
  int got_first_key_frame;
  uint8_t *extradata;
  int extradata_size;

  int64_t current_pts;
  unsigned long long offset;

  int started;
  int draining;
  int flushing;
  int is_lone_sei_pkt;
  int eos;
  AVHWFramesContext    *frames;

  /* below are all command line options */
  char *xcoder_opts;
  int enable_user_data_sei_passthru;
  int custom_sei_type;
  int low_delay;
  int pkt_nal_bitmap;

#ifdef NI_DEC_GSTREAMER_SUPPORT
  // GStreamer support: use pkt offset to save/retrieve associated GS data
  void *cur_gs_opaque;
  AVBufferRef *cur_gs_buf0;
  GsData gs_data[NI_FIFO_SZ];
  uint64_t gs_opaque_offsets_index_min[NI_FIFO_SZ];
  uint64_t gs_opaque_offsets_index[NI_FIFO_SZ];
#endif
} XCoderH264DecContext;

typedef struct XCoderH265EncContext {
  AVClass *avclass;

  char *dev_xcoder_name;          /* dev name of the xcoder card to use */
  char *blk_xcoder_name;          /* blk name of the xcoder card to use */
  int dev_enc_idx;                /* user-specified encoder index */
  char *dev_blk_name;             /* user-specified encoder block device name */
  int nvme_io_size;               /* custom nvme io size */
  int keep_alive_timeout;         /* keep alive timeout setting */
  ni_device_context_t *rsrc_ctx;  /* resource management context */
  unsigned long xcode_load_pixel; /* xcode load in pixels by this encode task */
  
  // frame fifo, to be used for sequence change frame buffering
  AVFifoBuffer *fme_fifo;
  int eos_fme_received;
  AVFrame buffered_fme; // buffered frame for sequence change handling

  ni_session_data_io_t  api_pkt; /* used for receiving bitstream from xcoder */
  ni_session_data_io_t   api_fme; /* used for sending YUV data to xcoder */
  ni_session_context_t api_ctx;
  ni_xcoder_params_t api_param;

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

  int encoder_flushing;
  int encoder_eof;
  
  // ROI
  int roi_side_data_size;
  AVRegionOfInterest *av_rois;  // last passed in AVRegionOfInterest
  int nb_rois;

  /* backup copy of original values of -enc command line option */
  int  orig_dev_enc_idx;

  AVFrame *sframe_pool[MAX_NUM_FRAMEPOOL_HWAVFRAME];
  int aFree_Avframes_list[MAX_NUM_FRAMEPOOL_HWAVFRAME + 1];
  int freeHead;
  int freeTail;

  /* below are all command line options */
  char *xcoder_opts;
  char *xcoder_gop;

  int reconfigCount;
  // actual enc_change_params is in ni_session_context !

} XCoderH265EncContext;

// copy maximum number of bytes of a string from src to dst, ensuring null byte
// terminated
static inline void ff_xcoder_strncpy(char *dst, const char *src, int max) {
    if (dst && src && max) {
        *dst = '\0';
        strncpy(dst, src, max);
        *(dst + max - 1) = '\0';
    }
}

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

int parse_symbolic_decoder_param(XCoderH264DecContext *s);

int retrieve_frame(AVCodecContext *avctx, AVFrame *data, int *got_frame,
                   ni_frame_t *xfme);
int ff_xcoder_add_headers(AVCodecContext *avctx, AVPacket *pkt,
                          uint8_t *extradata, int extradata_size);
#endif /* AVCODEC_NICODEC_H */
