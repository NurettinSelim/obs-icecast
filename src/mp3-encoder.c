/*
 * obs-icecast - audio-only MP3 streaming output for OBS Studio
 * Copyright (C) 2026 Nurettin Selim
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <https://www.gnu.org/licenses/>.
 */

#include "mp3-encoder.h"

#include <libavcodec/avcodec.h>
#include <libavutil/opt.h>
#include <libavutil/channel_layout.h>
#include <libavutil/version.h>

struct mp3_encoder {
	obs_encoder_t *encoder;
	const AVCodec *codec;
	AVCodecContext *ctx;
	AVFrame *frame;
	AVPacket *pkt;
	int64_t total_samples;
	int channels;
};

/* ------------------------------------------------------------------ */

static const char *mp3_getname(void *unused)
{
	UNUSED_PARAMETER(unused);
	return obs_module_text("MP3Encoder");
}

static void *mp3_create(obs_data_t *settings, obs_encoder_t *encoder)
{
	struct mp3_encoder *enc = bzalloc(sizeof(*enc));
	enc->encoder = encoder;

	/* Find libmp3lame in OBS's bundled FFmpeg */
	enc->codec = avcodec_find_encoder_by_name("libmp3lame");
	if (!enc->codec) {
		blog(LOG_ERROR,
		     "[icecast_mp3] libmp3lame encoder not found in FFmpeg");
		bfree(enc);
		return NULL;
	}

	enc->ctx = avcodec_alloc_context3(enc->codec);
	if (!enc->ctx) {
		blog(LOG_ERROR,
		     "[icecast_mp3] failed to allocate codec context");
		bfree(enc);
		return NULL;
	}

	int bitrate = (int)obs_data_get_int(settings, "bitrate");
	if (bitrate == 0)
		bitrate = 128;

	struct obs_audio_info aoi;
	obs_get_audio_info(&aoi);

	enc->ctx->bit_rate = (int64_t)bitrate * 1000;
	enc->ctx->sample_rate = aoi.samples_per_sec;
	enc->ctx->sample_fmt = AV_SAMPLE_FMT_FLTP;

	/* MP3 supports mono or stereo only — force downmix if needed */
	int ch = (aoi.speakers == SPEAKERS_MONO) ? 1 : 2;

#if LIBAVUTIL_VERSION_INT >= AV_VERSION_INT(57, 28, 100)
	av_channel_layout_default(&enc->ctx->ch_layout, ch);
#else
	enc->ctx->channels = ch;
	enc->ctx->channel_layout = av_get_default_channel_layout(ch);
#endif

	int ret = avcodec_open2(enc->ctx, enc->codec, NULL);
	if (ret < 0) {
		char errbuf[256];
		av_strerror(ret, errbuf, sizeof(errbuf));
		blog(LOG_ERROR, "[icecast_mp3] avcodec_open2 failed: %s",
		     errbuf);
		avcodec_free_context(&enc->ctx);
		bfree(enc);
		return NULL;
	}

	enc->channels = ch;

	/* Pre-allocate frame with codec's frame_size (1152 for MP3) */
	enc->frame = av_frame_alloc();
	enc->frame->nb_samples = enc->ctx->frame_size;
	enc->frame->format = enc->ctx->sample_fmt;
	enc->frame->sample_rate = enc->ctx->sample_rate;
#if LIBAVUTIL_VERSION_INT >= AV_VERSION_INT(57, 28, 100)
	av_channel_layout_copy(&enc->frame->ch_layout, &enc->ctx->ch_layout);
#else
	enc->frame->channels = enc->ctx->channels;
	enc->frame->channel_layout = enc->ctx->channel_layout;
#endif
	av_frame_get_buffer(enc->frame, 0);

	enc->pkt = av_packet_alloc();
	enc->total_samples = 0;

	blog(LOG_INFO,
	     "[icecast_mp3] encoder created: %d kbps, %d Hz, %d ch",
	     bitrate, enc->ctx->sample_rate, enc->channels);

	return enc;
}

static void mp3_destroy(void *data)
{
	struct mp3_encoder *enc = data;
	if (enc->pkt)
		av_packet_free(&enc->pkt);
	if (enc->frame)
		av_frame_free(&enc->frame);
	if (enc->ctx)
		avcodec_free_context(&enc->ctx);
	bfree(enc);
}

static bool mp3_encode(void *data, struct encoder_frame *frame,
		       struct encoder_packet *packet, bool *received_packet)
{
	struct mp3_encoder *enc = data;
	*received_packet = false;

	/* Unref any leftover packet data from previous call */
	av_packet_unref(enc->pkt);

	/* Ensure our frame buffer is writable */
	av_frame_make_writable(enc->frame);

	/* Copy audio from OBS frame into our AVFrame */
	size_t plane_size = enc->ctx->frame_size * sizeof(float);
	for (int i = 0; i < enc->channels; i++)
		memcpy(enc->frame->data[i], frame->data[i], plane_size);

	enc->frame->pts = enc->total_samples;

	/* Send frame to encoder */
	int ret = avcodec_send_frame(enc->ctx, enc->frame);
	if (ret < 0 && ret != AVERROR(EAGAIN)) {
		blog(LOG_ERROR, "[icecast_mp3] avcodec_send_frame: %d", ret);
		return false;
	}

	/* Receive encoded packet */
	ret = avcodec_receive_packet(enc->ctx, enc->pkt);
	if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) {
		return true; /* Need more input */
	} else if (ret < 0) {
		blog(LOG_ERROR, "[icecast_mp3] avcodec_receive_packet: %d",
		     ret);
		return false;
	}

	enc->total_samples += enc->ctx->frame_size;

	packet->data = enc->pkt->data;
	packet->size = enc->pkt->size;
	packet->type = OBS_ENCODER_AUDIO;
	packet->timebase_num = 1;
	packet->timebase_den = enc->ctx->sample_rate;
	packet->pts = enc->pkt->pts;
	packet->dts = enc->pkt->dts;
	packet->keyframe = true; /* Every MP3 frame is a keyframe */

	*received_packet = true;
	return true;
}

static void mp3_defaults(obs_data_t *settings)
{
	obs_data_set_default_int(settings, "bitrate", 128);
}

static obs_properties_t *mp3_properties(void *unused)
{
	UNUSED_PARAMETER(unused);
	obs_properties_t *props = obs_properties_create();

	obs_property_t *bitrate = obs_properties_add_list(
		props, "bitrate", obs_module_text("Bitrate"),
		OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_INT);
	obs_property_list_add_int(bitrate, "64 kbps", 64);
	obs_property_list_add_int(bitrate, "96 kbps", 96);
	obs_property_list_add_int(bitrate, "128 kbps", 128);
	obs_property_list_add_int(bitrate, "160 kbps", 160);
	obs_property_list_add_int(bitrate, "192 kbps", 192);
	obs_property_list_add_int(bitrate, "256 kbps", 256);
	obs_property_list_add_int(bitrate, "320 kbps", 320);

	return props;
}

static bool mp3_get_extra_data(void *data, uint8_t **extra_data, size_t *size)
{
	struct mp3_encoder *enc = data;
	if (enc->ctx->extradata && enc->ctx->extradata_size > 0) {
		*extra_data = enc->ctx->extradata;
		*size = enc->ctx->extradata_size;
		return true;
	}
	return false;
}

static void mp3_get_audio_info(void *data, struct audio_convert_info *info)
{
	UNUSED_PARAMETER(data);

	struct obs_audio_info aoi;
	obs_get_audio_info(&aoi);

	info->format = AUDIO_FORMAT_FLOAT_PLANAR;
	info->samples_per_sec = aoi.samples_per_sec;
	/* Force stereo or mono — MP3 doesn't support surround */
	info->speakers = (aoi.speakers == SPEAKERS_MONO) ? SPEAKERS_MONO
							  : SPEAKERS_STEREO;
}

static size_t mp3_get_frame_size(void *data)
{
	UNUSED_PARAMETER(data);
	return 1152; /* MP3 standard frame size */
}

/* ------------------------------------------------------------------ */

struct obs_encoder_info icecast_mp3_encoder = {
	.id = "icecast_mp3",
	.type = OBS_ENCODER_AUDIO,
	.codec = "mp3",
	.get_name = mp3_getname,
	.create = mp3_create,
	.destroy = mp3_destroy,
	.encode = mp3_encode,
	.get_defaults = mp3_defaults,
	.get_properties = mp3_properties,
	.get_extra_data = mp3_get_extra_data,
	.get_audio_info = mp3_get_audio_info,
	.get_frame_size = mp3_get_frame_size,
};
