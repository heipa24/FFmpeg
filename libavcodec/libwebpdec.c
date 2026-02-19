/*
 * LibWebP decoder
 * Copyright (c) 2025 Peter Xia
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
 * LibWebP decoder
 */

#include "avcodec.h"
#include "codec_internal.h"
#include "decode.h"
#include "libavutil/avassert.h"
#include "libavutil/imgutils.h"
#include "libavutil/opt.h"

#include <webp/demux.h>
#include <webp/decode.h>

typedef struct AnimatedWebPContext {
    const AVClass *class;
    WebPAnimDecoderOptions dec_options;
    WebPAnimDecoder *dec;
    AVBufferRef *file_content;
    WebPData webp_data;
    uint32_t loop_count;
    uint32_t loop_sent;
    uint32_t frame_count;
    uint32_t frame_sent;
    int prev_timestamp_ms;
    int ignore_loop;
    int infinite_loop;
    int file_has_infinite_loop;
    int first_frame_pts;
    int64_t timestamp_offset;
} AnimatedWebPContext;

static av_cold int libwebp_decode_init(AVCodecContext *avctx)
{
    AnimatedWebPContext *s = avctx->priv_data;

    if (!WebPAnimDecoderOptionsInit(&s->dec_options)) {
        av_log(avctx, AV_LOG_ERROR, "Cannot initialize WebPAnimDecoderOptions\n");
        return AVERROR_EXTERNAL;
    }
    s->dec_options.color_mode = MODE_RGBA;
    s->dec_options.use_threads = 1;
    s->file_content = NULL;
    s->dec = NULL;
    s->loop_sent = 0;
    s->frame_sent = 0;
    s->prev_timestamp_ms = 0;
    s->infinite_loop = 0;
    s->file_has_infinite_loop = 0;
    s->first_frame_pts = -1;
    s->timestamp_offset = 0;

    avctx->pix_fmt = AV_PIX_FMT_RGBA;
    avctx->pkt_timebase = av_make_q(1, 1000);

    return 0;
}

static int libwebp_decode_frame(AVCodecContext *avctx, AVFrame *p,
                                int *got_frame, AVPacket *avpkt)
{
    AnimatedWebPContext *s = avctx->priv_data;
    WebPAnimInfo anim_info;
    uint8_t *frame_rgba;
    int timestamp_ms;
    int ret;

    if (!s->dec) {
        if (!avpkt || avpkt->size <= 0)
            return AVERROR(EINVAL);

        s->file_content = av_buffer_ref(avpkt->buf);
        if (!s->file_content)
            return AVERROR(ENOMEM);

        s->webp_data.bytes = s->file_content->data;
        s->webp_data.size = s->file_content->size;

        s->dec = WebPAnimDecoderNew(&s->webp_data, &s->dec_options);
        if (!s->dec) {
            av_log(avctx, AV_LOG_ERROR, "Error creating WebPAnimDecoder.\n");
            av_buffer_unref(&s->file_content);
            return AVERROR_EXTERNAL;
        }

        if (!WebPAnimDecoderGetInfo(s->dec, &anim_info)) {
            av_log(avctx, AV_LOG_ERROR, "Error getting WebP animation info.\n");
            WebPAnimDecoderDelete(s->dec);
            s->dec = NULL;
            av_buffer_unref(&s->file_content);
            return AVERROR_EXTERNAL;
        }

        s->loop_count = anim_info.loop_count;
        s->frame_count = anim_info.frame_count;
        s->file_has_infinite_loop = (anim_info.loop_count == 0);
        s->infinite_loop = s->file_has_infinite_loop && !s->ignore_loop;
        if (s->file_has_infinite_loop && s->ignore_loop)
            s->loop_count = 1;

        av_log(avctx, AV_LOG_DEBUG,
               "WebP: %ux%u, %u frames, loop_count=%u (effective=%u, infinite=%d)\n",
               anim_info.canvas_width, anim_info.canvas_height,
               anim_info.frame_count, anim_info.loop_count, s->loop_count, s->infinite_loop);

        avctx->width = anim_info.canvas_width;
        avctx->coded_width = anim_info.canvas_width;
        avctx->height = anim_info.canvas_height;
        avctx->coded_height = anim_info.canvas_height;

        if (anim_info.frame_count > 0)
            avctx->framerate = av_make_q(1000, 1);
    } else if (!avpkt || avpkt->size <= 0) {
        if (!WebPAnimDecoderHasMoreFrames(s->dec)) {
            if (!s->infinite_loop && s->loop_sent >= s->loop_count) {
                *got_frame = 0;
                return 0;
            }
            s->timestamp_offset += s->prev_timestamp_ms;
            s->loop_sent++;
            if (!s->infinite_loop && s->loop_sent >= s->loop_count) {
                *got_frame = 0;
                return 0;
            }
            WebPAnimDecoderReset(s->dec);
            s->frame_sent = 0;
            s->prev_timestamp_ms = 0;
            s->first_frame_pts = -1;
            av_log(avctx, AV_LOG_DEBUG, "Loop %u/%u (flush)\n", s->loop_sent + 1,
                   s->infinite_loop ? 0 : s->loop_count);
        } else {
            return 0;
        }
    }

    if (!WebPAnimDecoderHasMoreFrames(s->dec)) {
        s->timestamp_offset += s->prev_timestamp_ms;
        s->loop_sent++;
        if (!s->infinite_loop && s->loop_sent >= s->loop_count) {
            *got_frame = 0;
            return 0;
        }
        WebPAnimDecoderReset(s->dec);
        s->frame_sent = 0;
        s->prev_timestamp_ms = 0;
        s->first_frame_pts = -1;
        av_log(avctx, AV_LOG_DEBUG, "Loop %u/%u\n", s->loop_sent + 1,
               s->infinite_loop ? 0 : s->loop_count);
    }

    if (!WebPAnimDecoderGetNext(s->dec, &frame_rgba, &timestamp_ms)) {
        av_log(avctx, AV_LOG_ERROR, "Error getting next frame from WebPAnimDecoder.\n");
        return AVERROR_EXTERNAL;
    }

    if (!frame_rgba || avctx->width <= 0 || avctx->height <= 0) {
        av_log(avctx, AV_LOG_ERROR, "Invalid frame data: frame_rgba=%p, width=%d, height=%d\n",
               frame_rgba, avctx->width, avctx->height);
        return AVERROR_EXTERNAL;
    }

    ret = ff_get_buffer(avctx, p, 0);
    if (ret < 0)
        return ret;

    p->width = avctx->width;
    p->height = avctx->height;
    p->format = AV_PIX_FMT_RGBA;
    p->pts = timestamp_ms + s->timestamp_offset;
    p->pkt_dts = timestamp_ms + s->timestamp_offset;
    p->pict_type = AV_PICTURE_TYPE_I;
    p->flags |= AV_FRAME_FLAG_KEY;

    if (s->first_frame_pts < 0)
        s->first_frame_pts = timestamp_ms;

    if (s->frame_sent > 0)
        p->duration = timestamp_ms - s->prev_timestamp_ms;

    s->prev_timestamp_ms = timestamp_ms;
    s->frame_sent++;

    if (p->linesize[0] < avctx->width * 4) {
        av_log(avctx, AV_LOG_ERROR, "Linesize too small: %d < %d\n",
               p->linesize[0], avctx->width * 4);
        return AVERROR_EXTERNAL;
    }

    av_image_copy_plane(p->data[0], p->linesize[0],
                        frame_rgba, avctx->width * 4,
                        avctx->width * 4, avctx->height);

    *got_frame = 1;

    if (WebPAnimDecoderHasMoreFrames(s->dec) || s->infinite_loop ||
        s->loop_sent < s->loop_count) {
        return 0;
    }

    return avpkt ? avpkt->size : 0;
}

static av_cold int libwebp_decode_close(AVCodecContext *avctx)
{
    AnimatedWebPContext *s = avctx->priv_data;

    if (s->dec) {
        WebPAnimDecoderDelete(s->dec);
        s->dec = NULL;
    }
    av_buffer_unref(&s->file_content);

    return 0;
}

static void libwebp_decode_flush(AVCodecContext *avctx)
{
    AnimatedWebPContext *s = avctx->priv_data;

    if (s->dec) {
        WebPAnimDecoderReset(s->dec);
        s->loop_sent = 0;
        s->frame_sent = 0;
        s->prev_timestamp_ms = 0;
        s->first_frame_pts = -1;
        s->timestamp_offset = 0;
    }
}

static const AVOption options[] = {
    { "ignore_loop", "ignore loop setting", offsetof(AnimatedWebPContext, ignore_loop), AV_OPT_TYPE_BOOL, { .i64 = 1 }, 0, 1, AV_OPT_FLAG_DECODING_PARAM },
    { NULL },
};

static const AVClass libwebp_decoder_class = {
    .class_name = "libwebp",
    .item_name = av_default_item_name,
    .option = options,
    .version = LIBAVUTIL_VERSION_INT,
    .category = AV_CLASS_CATEGORY_DECODER,
};

const FFCodec ff_libwebp_decoder = {
    .p.name         = "libwebp",
    CODEC_LONG_NAME("libwebp WebP image/animation decoder"),
    .p.type         = AVMEDIA_TYPE_VIDEO,
    .p.id           = AV_CODEC_ID_WEBP,
    .p.priv_class   = &libwebp_decoder_class,
    .priv_data_size = sizeof(AnimatedWebPContext),
    .p.wrapper_name = "libwebp",
    .init           = libwebp_decode_init,
    FF_CODEC_DECODE_CB(libwebp_decode_frame),
    .close          = libwebp_decode_close,
    .flush          = libwebp_decode_flush,
    .p.capabilities = AV_CODEC_CAP_DR1 | AV_CODEC_CAP_DELAY,
};
