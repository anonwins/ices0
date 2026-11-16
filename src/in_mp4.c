/* in_mp4.c
 * Plugin to read MP4 / M4A files as PCM
 *
 * Copyright (c) 2004 Brendan Cully <brendan@xiph.org>
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA  02111-1307, USA.
 *
 * $Id: in_mp4.c 10345 2005-11-05 23:37:18Z brendan $
 *
 * Demux and decode with FFmpeg. Ubuntu no longer ships libmp4v2.
 */

#include "config.h"
#include "in_mp4.h"
#include "metadata.h"
#include "replaygain.h"

#include <stdlib.h>
#include <string.h>

#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/channel_layout.h>
#include <libavutil/dict.h>
#include <libavutil/error.h>
#include <libavutil/samplefmt.h>
#include <libswresample/swresample.h>

/* -- data structures -- */
typedef struct {
	AVFormatContext *fmt;
	AVCodecContext *codec;
	SwrContext *swr;
	AVPacket *pkt;
	AVFrame *frame;
	int stream_index;
	int draining;
	int eof;
	int16_t *pcm;
	int pcm_capacity;
	int pcm_samples;
	int pcm_pos;
} mp4_in_t;

/* -- static prototypes -- */
static void ices_mp4_read_metadata(AVFormatContext *fmt, int stream_index);
static int ices_mp4_pull(mp4_in_t *mp4_data);
static ssize_t ices_mp4_readpcm(input_stream_t *self, size_t len, int16_t *left, int16_t *right);
static int ices_mp4_close(input_stream_t *self);

static void ices_mp4_log_av_error(const char *what, int err) {
	char buf[128];

	av_strerror(err, buf, sizeof(buf));
	ices_log_error("%s: %s", what, buf);
}

static const char *ices_mp4_dict_value(const AVDictionary *dict, const char *key) {
	const AVDictionaryEntry *entry;

	if (!dict)
		return NULL;
	entry = av_dict_get(dict, key, NULL, 0);
	return entry ? entry->value : NULL;
}

static void ices_mp4_read_metadata(AVFormatContext *fmt, int stream_index) {
	const AVDictionary *dicts[2];
	const char *artist = NULL;
	const char *title = NULL;
	const char *gain = NULL;
	int i;

	dicts[0] = fmt->metadata;
	dicts[1] = fmt->streams[stream_index]->metadata;
	for (i = 0; i < 2; i++) {
		if (!artist)
			artist = ices_mp4_dict_value(dicts[i], "artist");
		if (!title)
			title = ices_mp4_dict_value(dicts[i], "title");
		if (!gain)
			gain = ices_mp4_dict_value(dicts[i], "REPLAYGAIN_TRACK_GAIN");
		if (!gain)
			gain = ices_mp4_dict_value(dicts[i], "replaygain_track_gain");
	}

	if (artist)
		ices_log_debug("Artist: %s", artist);
	if (title)
		ices_log_debug("Title: %s", title);
	if (artist || title)
		ices_metadata_set(artist, title);
	if (gain)
		rg_set_track_gain(atof(gain));
}

static int ices_mp4_convert(mp4_in_t *mp4_data) {
	int out_samples;
	uint8_t *out_planes[1];
	int converted;

	out_samples = swr_get_out_samples(mp4_data->swr, mp4_data->frame->nb_samples);
	if (out_samples < 0) {
		ices_mp4_log_av_error("ices_mp4_open: Could not size the PCM buffer", out_samples);
		return -1;
	}
	if (out_samples > mp4_data->pcm_capacity) {
		int16_t *grown = realloc(mp4_data->pcm, (size_t)out_samples * 2 * sizeof(int16_t));

		if (!grown) {
			ices_log_error("Malloc failed in ices_mp4_readpcm");
			return -1;
		}
		mp4_data->pcm = grown;
		mp4_data->pcm_capacity = out_samples;
	}

	out_planes[0] = (uint8_t *)mp4_data->pcm;
	converted = swr_convert(mp4_data->swr, out_planes, out_samples,
				(const uint8_t **)mp4_data->frame->extended_data,
				mp4_data->frame->nb_samples);
	av_frame_unref(mp4_data->frame);
	if (converted < 0) {
		ices_mp4_log_av_error("ices_mp4_readpcm: Resample failed", converted);
		return -1;
	}
	mp4_data->pcm_samples = converted;
	mp4_data->pcm_pos = 0;
	return converted;
}

/* Decode until one block of stereo PCM is ready, or the file ends. */
static int ices_mp4_pull(mp4_in_t *mp4_data) {
	int ret;

	while (!mp4_data->eof) {
		ret = avcodec_receive_frame(mp4_data->codec, mp4_data->frame);
		if (ret == 0)
			return ices_mp4_convert(mp4_data);
		if (ret != AVERROR(EAGAIN) && ret != AVERROR_EOF) {
			ices_mp4_log_av_error("ices_mp4_readpcm: Decode failed", ret);
			return -1;
		}
		if (mp4_data->draining) {
			uint8_t *out_planes[1];
			int flushed;

			if (mp4_data->pcm_capacity < 1) {
				mp4_data->eof = 1;
				return 0;
			}
			out_planes[0] = (uint8_t *)mp4_data->pcm;
			flushed = swr_convert(mp4_data->swr, out_planes, mp4_data->pcm_capacity, NULL, 0);
			mp4_data->eof = 1;
			if (flushed < 0) {
				ices_mp4_log_av_error("ices_mp4_readpcm: Resample flush failed", flushed);
				return -1;
			}
			mp4_data->pcm_samples = flushed;
			mp4_data->pcm_pos = 0;
			return flushed;
		}

		ret = av_read_frame(mp4_data->fmt, mp4_data->pkt);
		if (ret == AVERROR_EOF) {
			avcodec_send_packet(mp4_data->codec, NULL);
			mp4_data->draining = 1;
			continue;
		}
		if (ret < 0) {
			ices_mp4_log_av_error("ices_mp4_readpcm: Read failed", ret);
			return -1;
		}
		if (mp4_data->pkt->stream_index != mp4_data->stream_index) {
			av_packet_unref(mp4_data->pkt);
			continue;
		}
		ret = avcodec_send_packet(mp4_data->codec, mp4_data->pkt);
		av_packet_unref(mp4_data->pkt);
		if (ret < 0 && ret != AVERROR(EAGAIN)) {
			ices_mp4_log_av_error("ices_mp4_readpcm: Decode failed", ret);
			return -1;
		}
	}

	return 0;
}

/* try to open an MP4 file for decoding. Returns:
 *   0: success
 *   1: not an MP4 file
 *  -1: error opening
 */
int ices_mp4_open(input_stream_t *self, char *buf, size_t len) {
	mp4_in_t *mp4_data = NULL;
	AVFormatContext *fmt = NULL;
	const AVCodec *codec = NULL;
	AVCodecContext *codec_ctx = NULL;
	SwrContext *swr = NULL;
	AVChannelLayout out_layout;
	int stream_index;
	int ret;

	/* The first box of an MP4 file is ftyp. Leave every other file for the other readers. */
	if (len < 8 || memcmp(buf + 4, "ftyp", 4) != 0)
		return 1;
	if (!self->filesize)
		return 1;

	memset(&out_layout, 0, sizeof(out_layout));
	ret = avformat_open_input(&fmt, self->path, NULL, NULL);
	if (ret < 0) {
		ices_mp4_log_av_error("ices_mp4_open: Could not open MP4", ret);
		return -1;
	}
	ret = avformat_find_stream_info(fmt, NULL);
	if (ret < 0) {
		ices_mp4_log_av_error("ices_mp4_open: Could not read MP4 streams", ret);
		goto errFormat;
	}

	stream_index = av_find_best_stream(fmt, AVMEDIA_TYPE_AUDIO, -1, -1, &codec, 0);
	if (stream_index < 0 || !codec) {
		ices_log_error("ices_mp4_open: No audio track found");
		goto errFormat;
	}

	codec_ctx = avcodec_alloc_context3(codec);
	if (!codec_ctx) {
		ices_log_error("ices_mp4_open: Could not allocate a decoder");
		goto errFormat;
	}
	ret = avcodec_parameters_to_context(codec_ctx, fmt->streams[stream_index]->codecpar);
	if (ret < 0) {
		ices_mp4_log_av_error("ices_mp4_open: Could not copy stream parameters", ret);
		goto errCodec;
	}
	ret = avcodec_open2(codec_ctx, codec, NULL);
	if (ret < 0) {
		ices_mp4_log_av_error("ices_mp4_open: Could not open the decoder", ret);
		goto errCodec;
	}
	if (codec_ctx->sample_rate <= 0 || codec_ctx->ch_layout.nb_channels <= 0) {
		ices_log_error("ices_mp4_open: Audio track has no sample rate or channels");
		goto errCodec;
	}

	av_channel_layout_default(&out_layout, 2);
	ret = swr_alloc_set_opts2(&swr, &out_layout, AV_SAMPLE_FMT_S16, codec_ctx->sample_rate,
				  &codec_ctx->ch_layout, codec_ctx->sample_fmt, codec_ctx->sample_rate,
				  0, NULL);
	av_channel_layout_uninit(&out_layout);
	if (ret < 0 || !swr) {
		ices_log_error("ices_mp4_open: Could not set up stereo output");
		goto errCodec;
	}
	if (swr_init(swr) < 0) {
		ices_log_error("ices_mp4_open: Could not set up stereo output");
		goto errSwr;
	}

	if (!(mp4_data = calloc(1, sizeof(mp4_in_t)))) {
		ices_log_error("Malloc failed in ices_mp4_open");
		goto errSwr;
	}
	mp4_data->pkt = av_packet_alloc();
	mp4_data->frame = av_frame_alloc();
	if (!mp4_data->pkt || !mp4_data->frame) {
		ices_log_error("Malloc failed in ices_mp4_open");
		goto errData;
	}

	ices_mp4_read_metadata(fmt, stream_index);
	ices_log_debug("Found MP4 audio, codec %s, sample rate %d, %d channels",
		       codec->name, codec_ctx->sample_rate, codec_ctx->ch_layout.nb_channels);

	close(self->fd);
	self->fd = -1;
	self->samplerate = (unsigned int)codec_ctx->sample_rate;
	self->channels = 2;
	if (codec_ctx->bit_rate > 0)
		self->bitrate = (unsigned int)(codec_ctx->bit_rate / 1000);

	mp4_data->fmt = fmt;
	mp4_data->codec = codec_ctx;
	mp4_data->swr = swr;
	mp4_data->stream_index = stream_index;
	self->type = ICES_INPUT_MP4;
	self->data = mp4_data;
	self->read = NULL;
	self->readpcm = ices_mp4_readpcm;
	self->close = ices_mp4_close;
	return 0;

 errData:
	if (mp4_data) {
		av_packet_free(&mp4_data->pkt);
		av_frame_free(&mp4_data->frame);
		free(mp4_data);
	}
 errSwr:
	swr_free(&swr);
 errCodec:
	avcodec_free_context(&codec_ctx);
 errFormat:
	avformat_close_input(&fmt);
	return -1;
}

static ssize_t ices_mp4_readpcm(input_stream_t *self, size_t olen, int16_t *left, int16_t *right) {
	mp4_in_t *mp4_data = (mp4_in_t *)self->data;
	int max_samples;
	int available;
	int count;
	int i;

	max_samples = (int)(olen / sizeof(int16_t));
	if (max_samples < 1)
		return 0;

	while (mp4_data->pcm_pos >= mp4_data->pcm_samples) {
		int pulled = ices_mp4_pull(mp4_data);

		if (pulled < 0)
			return -1;
		if (pulled == 0)
			return 0;
	}

	available = mp4_data->pcm_samples - mp4_data->pcm_pos;
	count = available < max_samples ? available : max_samples;
	for (i = 0; i < count; i++) {
		left[i] = mp4_data->pcm[(mp4_data->pcm_pos + i) * 2];
		right[i] = mp4_data->pcm[(mp4_data->pcm_pos + i) * 2 + 1];
	}
	mp4_data->pcm_pos += count;
	return count;
}

static int ices_mp4_close(input_stream_t *self) {
	mp4_in_t *mp4_data = (mp4_in_t *)self->data;

	swr_free(&mp4_data->swr);
	avcodec_free_context(&mp4_data->codec);
	avformat_close_input(&mp4_data->fmt);
	av_packet_free(&mp4_data->pkt);
	av_frame_free(&mp4_data->frame);
	free(mp4_data->pcm);
	free(mp4_data);
	return 0;
}
