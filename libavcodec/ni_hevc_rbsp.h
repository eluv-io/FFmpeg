/*
 * NetInt HEVC RBSP parser common code header
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

#ifndef ENCODER_STATE_BITSTREAM_H_
#define ENCODER_STATE_BITSTREAM_H_

#include "put_bits.h"

#define  MAX_PUT_BUF_SIZE (2*1024*1024)

typedef struct ni_bitstream_t {
    PutBitContext pbc;
    uint8_t *pb_buf;
    uint8_t cache;
    uint8_t cur_bits;
    uint8_t zero_cnt;
} ni_bitstream_t;

extern int ni_bitstream_init(ni_bitstream_t *stream);
extern void ni_bitstream_deinit(ni_bitstream_t *stream);
extern void ni_bitstream_reset(ni_bitstream_t *stream);
extern void ni_bitstream_fetch(const ni_bitstream_t *stream, uint8_t *buf, size_t size);
extern int ni_bitstream_count(ni_bitstream_t *stream);
extern void ni_put_bits(ni_bitstream_t *stream, uint8_t bits, const uint32_t data);
extern void ff_ni_write_nal_header(ni_bitstream_t *stream, const uint8_t nal_type,
        const uint8_t temporal_id, const int long_start_code);
extern int ff_ni_hevc_encode_nal_slice_header(ni_bitstream_t *stream, void *ctx,
        H265RawSliceHeader *slice, const H265RawSPS *sps, const H265RawPPS *pps,
        int width, int height, uint8_t force_tile, int x, int y, int independent);
extern int ff_ni_hevc_encode_nal_vps(ni_bitstream_t *stream, void *ctx,
        const H265RawVPS *vps);
extern int ff_ni_hevc_encode_nal_sps(ni_bitstream_t *stream, void *ctx,
        const H265RawSPS *sps, int width, int height);
extern int ff_ni_hevc_encode_nal_pps(ni_bitstream_t *stream, void *ctx,
        const H265RawPPS *pps, uint8_t force_tile,
        int columns, int rows);
#endif // ENCODER_STATE_BITSTREAM_H_
