/*
 * MOV CENC (Common Encryption) writer
 * Copyright (c) 2015 Eran Kornblau <erankor at gmail dot com>
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

#ifndef AVFORMAT_MOVENCCENC_H
#define AVFORMAT_MOVENCCENC_H

#include "libavutil/aes_ctr.h"
#include "avformat.h"
#include "avio.h"
#include "isom.h"

#define CENC_KID_SIZE (16)

struct MOVTrack;

typedef struct {
    MOVEncryptionScheme encryption_scheme;
    struct AVAES* aes_cbc;
    uint8_t aes_cbc_iv[16]; /* AES_BLOCK_SIZE */
    struct AVAESCTR* aes_ctr;
    uint8_t* auxiliary_info;
    size_t auxiliary_info_size;
    size_t auxiliary_info_alloc_size;
    uint32_t auxiliary_info_entries;
    uint8_t* auxiliary_info_sizes;
    size_t  auxiliary_info_sizes_alloc_size;
    int64_t auxiliary_info_offset;

    /* subsample support */
    int use_subsamples;
    uint16_t subsample_count;
    size_t auxiliary_info_subsample_start; /* location to write subsample_count */

    AVCodecParserContext *parser;
    AVCodecContext *parser_avctx;
} MOVMuxCencContext;

/**
 * Initialize a CENC context
 * @param key encryption key, must have a length of AES_CTR_KEY_SIZE
 * @param use_subsamples when enabled parts of a packet can be encrypted, otherwise the whole packet is encrypted
 */
int ff_mov_cenc_init(MOVMuxCencContext* ctx, AVCodecParameters *par,
                     MOVEncryptionScheme encryption_scheme, uint8_t* encryption_key,
                     uint8_t* encryption_iv, int encryption_iv_len,
                     int use_subsamples, int bitexact);

/**
 * Free a CENC context
 */
void ff_mov_cenc_free(MOVMuxCencContext* ctx);

/**
 * Write a fully encrypted packet
 */
int ff_mov_cenc_write_packet(MOVMuxCencContext* ctx, AVIOContext *pb, const uint8_t *buf_in, int size);

/**
 * Parse AVC NAL units from Annex B format. The NAL size and header, and VCL
 * slice headers, are written in the clear while the body is encrypted.
 * 
 * Returns bytes written, or < 0 on error
 */
int ff_mov_cenc_avc_parse_nal_units(AVFormatContext *s, MOVMuxCencContext* ctx,
                                    AVIOContext *pb, AVPacket *pkt);

/**
 * Write AVC NAL units that are in MP4 format. The NAL size and header, and VCL
 * slice headers, are written in the clear while the body is encrypted.
 * 
 * Returns bytes written, or < 0 on error
 */
int ff_mov_cenc_avc_write_nal_units(AVFormatContext *s, MOVMuxCencContext* ctx,
                                    AVIOContext *pb, AVPacket *pkt);

/**
 * Write the cenc atoms that should reside inside senc
 * The senc can reside in either traf or trak
 */
int ff_mov_cenc_write_senc_tag(MOVMuxCencContext* ctx, AVIOContext *pb, int64_t moof_offset);

/**
 * Write the cenc atoms that should reside inside stbl
 * Write the senc first to get the auxiliary_info_offset for saio
 */
void ff_mov_cenc_write_stbl_atoms(MOVMuxCencContext* ctx, AVIOContext *pb);

/**
 * Write the sinf atom, contained inside stsd
 */
int ff_mov_cenc_write_sinf_tag(struct MOVTrack* track, AVIOContext *pb, uint8_t* kid);

/**
 * Reset aux info for next moof
 */
void ff_mov_cenc_auxiliary_info_reset(MOVMuxCencContext* ctx);

#endif /* AVFORMAT_MOVENCCENC_H */
