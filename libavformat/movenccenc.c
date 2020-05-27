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
#include "movenccenc.h"
#include "avc.h"
#include "avio_internal.h"
#include "movenc.h"
#include "libavcodec/h2645_parse.h"
#include "libavcodec/internal.h"
#include "libavutil/aes.h"
#include "libavutil/avassert.h"
#include "libavutil/intreadwrite.h"
#include "libavutil/random_seed.h"

#define AES_BLOCK_SIZE (16)

static int auxiliary_info_alloc_size(MOVMuxCencContext* ctx, int size)
{
    size_t new_alloc_size;

    if (ctx->auxiliary_info_size + size > ctx->auxiliary_info_alloc_size) {
        new_alloc_size = FFMAX(ctx->auxiliary_info_size + size, ctx->auxiliary_info_alloc_size * 2);
        if (av_reallocp(&ctx->auxiliary_info, new_alloc_size)) {
            return AVERROR(ENOMEM);
        }

        ctx->auxiliary_info_alloc_size = new_alloc_size;
    }

    return 0;
}

static int auxiliary_info_write(MOVMuxCencContext* ctx,
                                const uint8_t *buf_in, int size)
{
    int ret;

    ret = auxiliary_info_alloc_size(ctx, size);
    if (ret) {
        return ret;
    }
    memcpy(ctx->auxiliary_info + ctx->auxiliary_info_size, buf_in, size);
    ctx->auxiliary_info_size += size;

    return 0;
}

static int auxiliary_info_add_subsample(MOVMuxCencContext* ctx,
    uint16_t clear_bytes, uint32_t encrypted_bytes)
{
    uint8_t* p;
    int ret;

    if (!ctx->use_subsamples) {
        return 0;
    }

    ret = auxiliary_info_alloc_size(ctx, 6);
    if (ret) {
        return ret;
    }

    p = ctx->auxiliary_info + ctx->auxiliary_info_size;

    AV_WB16(p, clear_bytes);
    p += sizeof(uint16_t);

    AV_WB32(p, encrypted_bytes);

    ctx->auxiliary_info_size += 6;
    ctx->subsample_count++;

    return 0;
}

void ff_mov_cenc_auxiliary_info_reset(MOVMuxCencContext* ctx) {
    ctx->auxiliary_info_size = 0;
    ctx->auxiliary_info_entries = 0;
}

/**
 * Encrypt the input buffer and write using avio_write
 *
 * For AES-CBC, the block chain starting with MOVMuxCencContext->aes_cbc_iv is
 * wholly contained in this function; i.e. the chain does not span multiple
 * calls.
 */
static void mov_cenc_write_encrypted(MOVMuxCencContext* ctx, AVIOContext *pb,
                                     const uint8_t *buf_in, int size)
{
    uint8_t chunk[4000];
    const uint8_t* cur_pos = buf_in;
    int size_left = size;
    int cur_size, block_count, enc_size, remaining_size;
    uint8_t aes_cbc_iv[AES_BLOCK_SIZE];

    /* pattern encryption repeats every 10 blocks */
    av_assert2(sizeof(chunk) % (AES_BLOCK_SIZE * 10) == 0);

    if (ctx->encryption_scheme == MOV_ENC_CENC_AES_CBC_PATTERN) {
        /* use constant IV and reset the chain for each sample/subsample */
        memcpy(aes_cbc_iv, ctx->aes_cbc_iv, sizeof(aes_cbc_iv));
    }
    
    while (size_left > 0) {
        cur_size = FFMIN((unsigned int)size_left, sizeof(chunk));
        if (ctx->encryption_scheme == MOV_ENC_CENC_AES_CTR) {
            av_aes_ctr_crypt(ctx->aes_ctr, chunk, cur_pos, cur_size);
            avio_write(pb, chunk, cur_size);
        } else { /* MOV_ENC_CENC_AES_CBC_PATTERN */
            /* remaining data smaller than a full block remains unencrypted */
            block_count = cur_size / AES_BLOCK_SIZE;
            enc_size = block_count * AES_BLOCK_SIZE;
            remaining_size = cur_size - enc_size;
            /* same call for both pattern and full sample encryption */
            av_aes_crypt(ctx->aes_cbc, chunk, cur_pos, block_count, aes_cbc_iv, 0);
            avio_write(pb, chunk, enc_size);
            avio_write(pb, cur_pos + enc_size, remaining_size);
        }
        cur_pos += cur_size;
        size_left -= cur_size;
    }
}

/**
 * Start writing a packet
 */
static int mov_cenc_start_packet(MOVMuxCencContext* ctx)
{
    int ret = 0;

    /**
     * write the iv 
     * 
     * omit for cbcs - the per-sample iv is zero when using a constant iv
     */
    if (ctx->encryption_scheme == MOV_ENC_CENC_AES_CTR) {
        ret = auxiliary_info_write(ctx, av_aes_ctr_get_iv(ctx->aes_ctr), AES_CTR_IV_SIZE);
        if (ret) {
            return ret;
        }
    }
    
    /* subsample clear/protected data counts are not needed for full sample encryption */
    if (!ctx->use_subsamples) {
        return 0;
    }

    /* write a zero subsample count */
    ctx->auxiliary_info_subsample_start = ctx->auxiliary_info_size;
    ctx->subsample_count = 0;
    ret = auxiliary_info_write(ctx, (uint8_t*)&ctx->subsample_count, sizeof(ctx->subsample_count));
    if (ret) {
        return ret;
    }

    return 0;
}

/**
 * Finalize a packet
 */
static int mov_cenc_end_packet(MOVMuxCencContext* ctx)
{
    size_t new_alloc_size;
    size_t sample_iv_size;

    if (ctx->encryption_scheme == MOV_ENC_CENC_AES_CTR) {
        sample_iv_size = AES_CTR_IV_SIZE;
        av_aes_ctr_increment_iv(ctx->aes_ctr);
        if (!ctx->use_subsamples) {
            ctx->auxiliary_info_entries++;
            return 0;
        }
    } else { /* MOV_ENC_CENC_AES_CBC_PATTERN */
        sample_iv_size = 0; /* constant iv */
        if (!ctx->use_subsamples) {
            /* no aux info needed */
            return 0;
        }
    }

    /* add the auxiliary info entry size */
    if (ctx->auxiliary_info_entries >= ctx->auxiliary_info_sizes_alloc_size) {
        new_alloc_size = ctx->auxiliary_info_entries * 2 + 1;
        if (av_reallocp(&ctx->auxiliary_info_sizes, new_alloc_size)) {
            return AVERROR(ENOMEM);
        }

        ctx->auxiliary_info_sizes_alloc_size = new_alloc_size;
    }
    ctx->auxiliary_info_sizes[ctx->auxiliary_info_entries] =
        sample_iv_size + ctx->auxiliary_info_size - ctx->auxiliary_info_subsample_start;
    ctx->auxiliary_info_entries++;

    /* update the subsample count */
    AV_WB16(ctx->auxiliary_info + ctx->auxiliary_info_subsample_start, ctx->subsample_count);

    return 0;
}

int ff_mov_cenc_write_packet(MOVMuxCencContext* ctx, AVIOContext *pb,
                          const uint8_t *buf_in, int size)
{
    int ret;

    ret = mov_cenc_start_packet(ctx);
    if (ret) {
        return ret;
    }

    ret = auxiliary_info_add_subsample(ctx, 0, size);
    if (ret) {
        return ret;
    }

    mov_cenc_write_encrypted(ctx, pb, buf_in, size);

    ret = mov_cenc_end_packet(ctx);
    if (ret) {
        return ret;
    }

    return 0;
}

// TODO refactor with ff_mov_cenc_avc_write_nal_units
int ff_mov_cenc_avc_parse_nal_units(AVFormatContext *s, MOVMuxCencContext* ctx,
                                    AVIOContext *pb, AVPacket *pkt)
{
    int encsize, nalsize, naltype, ret, slice_header_len;
    int clear_bytes = 0;
    int encrypted_bytes = 0;
    int nal_index = 0;
    int size = 0;
    const uint8_t *start = pkt->data;
    const uint8_t *end = start + pkt->size;
    const uint8_t *nal_start, *nal_end;

    ret = mov_cenc_start_packet(ctx);
    if (ret) {
        return ret;
    }

    uint8_t *parsed_buf_out;
    int parsed_buf_out_size;
    av_parser_parse2(ctx->parser, ctx->parser_avctx, &parsed_buf_out,
        &parsed_buf_out_size, pkt->data, pkt->size, pkt->pts, pkt->dts, pkt->pos);
    H2645NAL* nals = (H2645NAL*)avpriv_h264_extract_nals(ctx->parser);

    nal_start = ff_avc_find_startcode(start, end);
    for (;;) {
        while (nal_start < end && !*(nal_start++));
        if (nal_start == end) {
            if (ctx->subsample_count == 0) {
                /* must write at least one subsample before exiting */
                auxiliary_info_add_subsample(ctx, clear_bytes, encrypted_bytes);
            }
            break;
        }

        nal_end = ff_avc_find_startcode(nal_start, end);
        nalsize = nal_end - nal_start;
        avio_wb32(pb, nalsize);
        clear_bytes += 4;

        naltype = *nal_start & 0x1f;
        int header_bits = nals[nal_index].slice_header_len_bits;
        slice_header_len = (header_bits + 7) / 8;
        if ((naltype == 1 || naltype == 5) &&
             nalsize >= slice_header_len + AES_BLOCK_SIZE)
        {
            avio_write(pb, nal_start, slice_header_len);
            clear_bytes += slice_header_len;

            encsize = nalsize - slice_header_len;
            mov_cenc_write_encrypted(ctx, pb, nal_start + slice_header_len, encsize);
            encrypted_bytes += encsize;

            auxiliary_info_add_subsample(ctx, clear_bytes, encrypted_bytes);
            clear_bytes = 0;
            encrypted_bytes = 0;
        } else {
            avio_write(pb, nal_start, nalsize);
            clear_bytes += nalsize;
        }

        size += 4 + nal_end - nal_start;
        nal_start = nal_end;
        nal_index++;
    }

    ret = mov_cenc_end_packet(ctx);
    if (ret) {
        return ret;
    }

    return size;
}

// TODO combine with ff_mov_cenc_avc_parse_nal_units
int ff_mov_cenc_avc_write_nal_units(AVFormatContext *s, MOVMuxCencContext* ctx,
                                    AVIOContext *pb, AVPacket *pkt)
{
    int encsize, j, nalsize, naltype, ret, slice_header_len;
    int clear_bytes = 0;
    int encrypted_bytes = 0;
    int nal_index = 0;
    int size = 0;
    const uint8_t *buf_in = pkt->data;
    int remaining = pkt->size;
    AVCodecParameters *par = s->streams[pkt->stream_index]->codecpar;
    const int nal_length_size = (par->extradata[4] & 0x3) + 1;
    
    ret = mov_cenc_start_packet(ctx);
    if (ret) {
        return ret;
    }

    uint8_t *parsed_buf_out;
    int parsed_buf_out_size;
    av_parser_parse2(ctx->parser, ctx->parser_avctx, &parsed_buf_out,
        &parsed_buf_out_size, pkt->data, pkt->size, pkt->pts, pkt->dts, pkt->pos);
    H2645NAL* nals = (H2645NAL*)avpriv_h264_extract_nals(ctx->parser);

    // TODO maybe use parsed info above to not parse again below
    while (remaining > 0) {
        /* parse the nal size */
        if (remaining < nal_length_size) {
            av_log(s, AV_LOG_ERROR, "CENC-AVC: remaining size %d smaller than nal length header %d\n", remaining, nal_length_size);
            return -1;
        }

        avio_write(pb, buf_in, nal_length_size);
        nalsize = 0;
        for (j = 0; j < nal_length_size; j++) {
            nalsize = (nalsize << 8) | *buf_in++;
        }
        clear_bytes += nal_length_size;
        remaining -= nal_length_size;
        
        if (nalsize <= 0 || nalsize > remaining) {
            av_log(s, AV_LOG_ERROR, "CENC-AVC: nal size %d, remaining %d\n", nalsize, remaining);
            return -1;
        }

        /**
         * Only encrypt slice data 1 H264_NAL_SLICE and 5 H264_NAL_IDR_SLICE.
         * Leave other nal types and VCL (video) slice headers clear, per Apple
         * MPEG-2 HLS encryption and CENC specs.
         */
        naltype = *buf_in & 0x1f;
        int header_bits = nals[nal_index].slice_header_len_bits;
        slice_header_len = (header_bits + 7) / 8;
        if ((naltype == 1 || naltype == 5) &&
             nalsize >= slice_header_len + AES_BLOCK_SIZE)
        {
            avio_write(pb, buf_in, slice_header_len);
            clear_bytes += slice_header_len;

            encsize = nalsize - slice_header_len;
            mov_cenc_write_encrypted(ctx, pb, buf_in + slice_header_len, encsize);
            encrypted_bytes += encsize;

            auxiliary_info_add_subsample(ctx, clear_bytes, encrypted_bytes);
            clear_bytes = 0;
            encrypted_bytes = 0;
        } else {
            avio_write(pb, buf_in, nalsize);
            clear_bytes += nalsize;
            if (remaining - nalsize <= 0 && ctx->subsample_count == 0) {
                /* must write at least one subsample before exiting */
                auxiliary_info_add_subsample(ctx, clear_bytes, encrypted_bytes);
            }
        }
        remaining -= nalsize;
        size += 4 + nalsize;
        buf_in += nalsize;
        nal_index++;
    }

    ret = mov_cenc_end_packet(ctx);
    if (ret) {
        return ret;
    }

    return size;
}

/* TODO: reuse this function from movenc.c */
static int64_t update_size(AVIOContext *pb, int64_t pos)
{
    int64_t curpos = avio_tell(pb);
    avio_seek(pb, pos, SEEK_SET);
    avio_wb32(pb, curpos - pos); /* rewrite size */
    avio_seek(pb, curpos, SEEK_SET);

    return curpos - pos;
}

int ff_mov_cenc_write_senc_tag(MOVMuxCencContext* ctx, AVIOContext *pb, int64_t moof_offset)
{
    int64_t pos;

    if (ctx->auxiliary_info_entries == 0) {
        return 0;
    }

    pos = avio_tell(pb);
    avio_wb32(pb, 0); /* size */
    ffio_wfourcc(pb, "senc");
    avio_wb32(pb, ctx->use_subsamples ? 0x02 : 0); /* version & flags */
    avio_wb32(pb, ctx->auxiliary_info_entries); /* entry count */
    ctx->auxiliary_info_offset = avio_tell(pb) - moof_offset;
    avio_write(pb, ctx->auxiliary_info, ctx->auxiliary_info_size);
    return update_size(pb, pos);
}

static int mov_cenc_write_saio_tag(MOVMuxCencContext* ctx, AVIOContext *pb)
{
    int64_t pos;
    uint8_t version;

    if (ctx->auxiliary_info_entries == 0) {
        return 0;
    }

    pos = avio_tell(pb);
    avio_wb32(pb, 0); /* size */
    ffio_wfourcc(pb, "saio");
    version = ctx->auxiliary_info_offset > 0xffffffff ? 1 : 0;
    avio_w8(pb, version);
    avio_wb24(pb, 0); /* flags */
    avio_wb32(pb, 1); /* entry count */
    if (version) {
        avio_wb64(pb, ctx->auxiliary_info_offset);
    } else {
        avio_wb32(pb, ctx->auxiliary_info_offset);
    }
    return update_size(pb, pos);
}

static int mov_cenc_write_saiz_tag(MOVMuxCencContext* ctx, AVIOContext *pb)
{
    int64_t pos;
    /* constant iv for cbcs, so sample iv is always zero */
    uint8_t iv_size = ctx->encryption_scheme == MOV_ENC_CENC_AES_CTR ? AES_CTR_IV_SIZE : 0;

    if (ctx->auxiliary_info_entries == 0) {
        return 0;
    }

    pos = avio_tell(pb);
    avio_wb32(pb, 0); /* size */
    ffio_wfourcc(pb, "saiz");
    avio_wb32(pb, 0); /* version & flags */
    avio_w8(pb, ctx->use_subsamples ? 0 : iv_size); /* default size */
    avio_wb32(pb, ctx->auxiliary_info_entries); /* entry count */
    if (ctx->use_subsamples) {
        avio_write(pb, ctx->auxiliary_info_sizes, ctx->auxiliary_info_entries);
    }
    return update_size(pb, pos);
}

void ff_mov_cenc_write_stbl_atoms(MOVMuxCencContext* ctx, AVIOContext *pb)
{
    mov_cenc_write_saio_tag(ctx, pb);
    mov_cenc_write_saiz_tag(ctx, pb);
}

static int mov_cenc_write_schi_tag(MOVTrack* track, AVIOContext *pb, uint8_t* kid)
{
    size_t iv_size;
    int64_t pos = avio_tell(pb);
    avio_wb32(pb, 0);     /* size */
    ffio_wfourcc(pb, "schi");

    if (track->cenc.encryption_scheme == MOV_ENC_CENC_AES_CTR) {
        avio_wb32(pb, 16 + CENC_KID_SIZE);  /* size */
        ffio_wfourcc(pb, "tenc");
        avio_wb32(pb, 0);                   /* version & flags */
        avio_wb24(pb, 1);                   /* default_isProtected */
        avio_w8(pb, AES_CTR_IV_SIZE);       /* default_Per_Sample_IV_Size */
        avio_write(pb, kid, CENC_KID_SIZE); /* default_KID */
    } else { /* MOV_ENC_CENC_AES_CBC_PATTERN */
        iv_size = sizeof(track->cenc.aes_cbc_iv);
        avio_wb32(pb, 16 + CENC_KID_SIZE + 1 + iv_size);  /* size */
        ffio_wfourcc(pb, "tenc");
        avio_w8(pb, 1);                     /* version */
        avio_wb24(pb, 0);                   /* flags */
        avio_w8(pb, 0);                     /* reserved */
        avio_w8(pb, track->cenc.use_subsamples ? 0x19 : 0); /* default_crypt_byte_block, default_skip_byte_block */
        avio_w8(pb, 1);                     /* default_isProtected */
        avio_w8(pb, 0);                     /* default_Per_Sample_IV_Size */
        avio_write(pb, kid, CENC_KID_SIZE); /* default_KID */
        avio_w8(pb, iv_size);               /* default_constant_IV_size */
        avio_write(pb, track->cenc.aes_cbc_iv, iv_size); /* default_constant_IV */
    }

    return update_size(pb, pos);
}

int ff_mov_cenc_write_sinf_tag(MOVTrack* track, AVIOContext *pb, uint8_t* kid)
{
    int64_t pos = avio_tell(pb);

    /* sinf */
    avio_wb32(pb, 0);               /* size */
    ffio_wfourcc(pb, "sinf");

    /* frma */
    avio_wb32(pb, 12);              /* size */
    ffio_wfourcc(pb, "frma");
    avio_wl32(pb, track->tag);

    /* schm */
    avio_wb32(pb, 20);              /* size */
    ffio_wfourcc(pb, "schm");
    avio_wb32(pb, 0);               /* version & flags */
    ffio_wfourcc(pb, track->cenc.encryption_scheme == MOV_ENC_CENC_AES_CTR ?
                 "cenc" : "cbcs");  /* scheme type*/
    avio_wb32(pb, 0x10000);         /* scheme version */

    /* schi */
    mov_cenc_write_schi_tag(track, pb, kid);

    return update_size(pb, pos);
}

static int mov_cenc_ctr_init(MOVMuxCencContext* ctx, uint8_t* key, int bitexact)
{
    int ret;

    av_assert0(ctx->aes_ctr == NULL);

    ctx->aes_ctr = av_aes_ctr_alloc();
    if (!ctx->aes_ctr) {
        return AVERROR(ENOMEM);
    }

    ret = av_aes_ctr_init(ctx->aes_ctr, key);
    if (ret != 0) {
        return ret;
    }

    if (!bitexact) {
        av_aes_ctr_set_random_iv(ctx->aes_ctr);
    }

    return 0;
}

static int mov_cenc_cbc_init(MOVMuxCencContext* ctx, uint8_t* key,
                             uint8_t* iv, int iv_len)
{
    int ret;
    uint32_t* iv_part;

    av_assert0(ctx->aes_cbc == NULL);

    ctx->aes_cbc = av_aes_alloc();
    if (!ctx->aes_cbc) {
        return AVERROR(ENOMEM);
    }

    ret = av_aes_init(ctx->aes_cbc, key, 128, 0);
    if (ret != 0) {
        return ret;
    }

    if (ctx->use_subsamples) {
        /* hard-coded to 1 encrypted 9 clear; signaled in the tenc atom */
        av_aes_set_pattern(ctx->aes_cbc, 1, 9);
    }

    if (iv_len == AES_BLOCK_SIZE) {
        memcpy(ctx->aes_cbc_iv, iv, iv_len);
    } else {
        iv_part = (uint32_t*)ctx->aes_cbc_iv;
        iv_part[0] = av_get_random_seed();
        iv_part[1] = av_get_random_seed();
        iv_part[2] = av_get_random_seed();
        iv_part[3] = av_get_random_seed();
    }

    return 0;
}

int ff_mov_cenc_init(MOVMuxCencContext* ctx, AVCodecParameters *par,
                     MOVEncryptionScheme encryption_scheme, uint8_t* key,
                     uint8_t* iv, int iv_len,
                     int use_subsamples, int bitexact)
{
    int ret;
    
    ctx->encryption_scheme = encryption_scheme;
    ctx->use_subsamples = use_subsamples;

    ctx->parser = av_parser_init(par->codec_id);
    ctx->parser_avctx = avcodec_alloc_context3(NULL);
    if (!ctx->parser_avctx)
        return AVERROR(ENOMEM);
    ret = avcodec_parameters_to_context(ctx->parser_avctx, par);
    if (ret < 0)
        return ret;
    // We only want to parse frame headers
    ctx->parser->flags |= PARSER_FLAG_COMPLETE_FRAMES;

    switch (encryption_scheme) {
    case MOV_ENC_CENC_AES_CTR:
        return mov_cenc_ctr_init(ctx, key, bitexact);
        break;
    case MOV_ENC_CENC_AES_CBC_PATTERN:
        return mov_cenc_cbc_init(ctx, key, iv, iv_len);
        break;
    }
    return 0;
}

void ff_mov_cenc_free(MOVMuxCencContext* ctx)
{
    av_aes_ctr_free(ctx->aes_ctr);
    if (ctx->encryption_scheme == MOV_ENC_CENC_AES_CBC_PATTERN && ctx->aes_cbc) {
        av_freep(&ctx->aes_cbc);
    }
    av_freep(&ctx->auxiliary_info);
}
