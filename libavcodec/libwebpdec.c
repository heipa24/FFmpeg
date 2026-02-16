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

#include "decode.h"
#include "codec_internal.h"
#include "libavutil/avutil.h"
#include "libavutil/opt.h"
#include "libavformat/avformat.h"
#include "libavformat/avio.h"

#include <webp/demux.h>
#include <webp/decode.h>

typedef struct AnimatedWebPContext
{
    const AVClass *class;
    WebPAnimDecoderOptions dec_options;
    WebPAnimDecoder *dec;
    AVBufferRef *file_content;
    WebPData webp_data; // references |file_content|
    uint32_t loop_to_send;
    uint32_t loop_sent;

    // --- Options ---
    int ignore_loop;
} AnimatedWebPContext;

// Initialize the decoder context
static av_cold int decode_libwebp_init(AVCodecContext *avctx)
{
    AnimatedWebPContext *s = avctx->priv_data;

    if (!WebPAnimDecoderOptionsInit(&s->dec_options))
    {
        av_log(avctx, AV_LOG_DEBUG, "Cannot initialize WebPAnimDecoderOptions\n");
        return AVERROR(ENOMEM);
    }
    s->dec_options.color_mode = MODE_RGBA;
    s->dec_options.use_threads = 1;
    s->file_content = NULL;
    s->loop_sent = 0;

    avctx->pix_fmt = AV_PIX_FMT_RGBA;
    avctx->pkt_timebase = av_make_q(1, 1000);
    avctx->framerate = av_make_q(1, 0);

    av_log(avctx, AV_LOG_DEBUG, "Animated WebP decoder initialized.\n");
    return 0; // Success
}

// Decode one frame of the animated WebP
// This function will be called multiple times by FFmpeg.
// The first call receives the AVPacket with the full WebP file.
// Subsequent calls receive empty AVPacket until all frames are decoded.
static int decode_libwebp_frame(AVCodecContext *avctx, AVFrame *p,
                                int *got_frame, AVPacket *avpkt)
{
    av_log(avctx, AV_LOG_INFO, "decode_libwebp_frame()\n");

    WebPAnimInfo anim_info;
    uint8_t *frame_rgba;
    int timestamp_ms;

    AnimatedWebPContext *s = avctx->priv_data;
    int ret = avpkt->size;

    // Initialization Phase (First Call)
    // |avpkt| contains the entire file.
    if (!s->dec)
    {
        if (!avpkt || avpkt->size <= 0)
        {
            // Should not happen on the first call, but check anyway.
            av_log(avctx, AV_LOG_ERROR, "No input data provided on first call.\n");
            return AVERROR(EINVAL);
        }

        // Store entire WebP file in memory.
        s->file_content = av_buffer_ref(avpkt->buf);
        s->webp_data.bytes = s->file_content->data;
        s->webp_data.size = s->file_content->size;

        s->dec = WebPAnimDecoderNew(&s->webp_data, &s->dec_options);
        if (!s->dec)
        {
            av_log(avctx, AV_LOG_ERROR, "Error creating WebPAnimDecoder.\n");
            av_buffer_unref(&s->file_content);
            return AVERROR(ENOMEM);
        }

        WebPAnimDecoderGetInfo(s->dec, &anim_info);

        s->loop_to_send = s->ignore_loop ? 1 : anim_info.loop_count;
        avctx->width = anim_info.canvas_width;
        avctx->coded_width = anim_info.canvas_width;
        avctx->height = anim_info.canvas_height;
        avctx->coded_height = anim_info.canvas_height;
        avctx->framerate = av_make_q(1, 0);
    }

    if (!WebPAnimDecoderHasMoreFrames(s->dec))
    {
        s->loop_sent++;
        WebPAnimDecoderReset(s->dec);
    }

    if (s->loop_sent >= s->loop_to_send)
    {
        av_log(avctx, AV_LOG_DEBUG, "End of animated WebP stream.\n");
        return AVERROR_EOF;
    }


    if (!WebPAnimDecoderGetNext(s->dec, &frame_rgba, &timestamp_ms))
    {
        av_log(avctx, AV_LOG_ERROR, "Error getting next frame from WebPAnimDecoder.\n");
        return AVERROR(EINVAL);
    }

    ret = ff_get_buffer(avctx, p, 0);
    if (ret)
    {
        av_log(avctx, AV_LOG_ERROR, "Failed to allocated buffer.\n");
        return AVERROR(ENOMEM);
    }

    p->width = avctx->width;
    p->height = avctx->height;
    p->format = AV_PIX_FMT_RGBA;
    p->pts = timestamp_ms;
    p->pkt_dts = 0;
    p->pict_type = AV_PICTURE_TYPE_I;

    memcpy(p->data[0], frame_rgba, p->width * p->height * 4);

    *got_frame = 1;
    return ret;
}

static av_cold int decode_libwebp_close(AVCodecContext *avctx)
{
    AnimatedWebPContext *s = avctx->priv_data;
    av_buffer_unref(&s->file_content);
    if (s->dec)
    {
        WebPAnimDecoderDelete(s->dec);
        s->dec = NULL;
    }
    return 0;
}

static const AVOption options[] = {
    {"ignore_loop", "ignore loop setting (netscape extension)", offsetof(AnimatedWebPContext, ignore_loop), AV_OPT_TYPE_BOOL, {.i64 = 1}, 0, 1, AV_OPT_FLAG_DECODING_PARAM},
    {NULL}};

static const AVClass libwebp_decoder_class = {
    .class_name = "libwebp_decoder",
    .item_name = av_default_item_name,
    .option = options,
    .version = LIBAVUTIL_VERSION_INT,
    .category = AV_CLASS_CATEGORY_DECODER,
};

// Define the AVCodec structure for FFmpeg
const FFCodec ff_libwebp_decoder = {
    .p.name         = "libwebp",
    CODEC_LONG_NAME("libwebp image/animation decoder"),
    .p.type         = AVMEDIA_TYPE_VIDEO,
    .p.id           = AV_CODEC_ID_WEBP,
    .p.priv_class   = &libwebp_decoder_class,
    .priv_data_size = sizeof(AnimatedWebPContext),
    .p.wrapper_name = "libwebp",
    .init           = decode_libwebp_init,
    FF_CODEC_DECODE_CB(decode_libwebp_frame),
    .close          = decode_libwebp_close,
    .p.capabilities = AV_CODEC_CAP_DR1 | AV_CODEC_CAP_DELAY,
};
