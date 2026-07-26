/*
 * obs-shoutcast - audio-only MP3 streaming output for OBS Studio
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

#include "shoutcast-output.h"
#include "icy-protocol.h"

#include <pthread.h>
#include <util/threading.h>
#include <media-io/audio-io.h>

struct shoutcast_output {
	obs_output_t *output;
	int sock_fd;
	pthread_mutex_t write_mutex;
	pthread_t connect_thread;
	volatile bool connecting;
	os_event_t *stop_event;
	char *last_song;
};

/*
 * A metadata update runs on a detached thread and must never dereference
 * struct shoutcast_output — an update racing shutdown would be a
 * use-after-free — so everything it needs is copied at dispatch time.
 */
struct metadata_job {
	char *server;
	char *username;
	char *password;
	char *mount;
	char *song;
	int port;
	int protocol;
};

/* ------------------------------------------------------------------ */

static const char *shoutcast_getname(void *unused)
{
	UNUSED_PARAMETER(unused);
	return obs_module_text("SHOUTcastOutput");
}

static void *shoutcast_create(obs_data_t *settings, obs_output_t *output)
{
	UNUSED_PARAMETER(settings);

	struct shoutcast_output *out = bzalloc(sizeof(*out));
	out->output = output;
	out->sock_fd = -1;
	pthread_mutex_init(&out->write_mutex, NULL);
	os_event_init(&out->stop_event, OS_EVENT_TYPE_MANUAL);

	return out;
}

static void shoutcast_destroy(void *data)
{
	struct shoutcast_output *out = data;

	if (out->sock_fd >= 0)
		icy_disconnect(out->sock_fd);

	os_event_destroy(out->stop_event);
	pthread_mutex_destroy(&out->write_mutex);
	bfree(out->last_song);
	bfree(out);
}

/* ------------------------------------------------------------------ */

static void *connect_thread(void *data)
{
	struct shoutcast_output *out = data;
	obs_data_t *settings = obs_output_get_settings(out->output);

	audio_t *audio = obs_get_audio();
	int channels = audio ? (int)audio_output_get_channels(audio) : 0;
	int samplerate = audio ? (int)audio_output_get_sample_rate(audio) : 0;

	struct icy_params params = {
		.server = obs_data_get_string(settings, "server"),
		.port = (int)obs_data_get_int(settings, "port"),
		.password = obs_data_get_string(settings, "password"),
		.username = obs_data_get_string(settings, "username"),
		.mount = obs_data_get_string(settings, "mount"),
		.name = obs_data_get_string(settings, "station_name"),
		.genre = obs_data_get_string(settings, "genre"),
		.url = obs_data_get_string(settings, "url"),
		.is_public = obs_data_get_bool(settings, "is_public"),
		.bitrate = (int)obs_data_get_int(settings, "bitrate"),
		/* Audio may not be initialised yet; keep ice-audio-info sane. */
		.channels = channels > 0 ? channels : 2,
		.samplerate = samplerate > 0 ? samplerate : 44100,
		.protocol = (int)obs_data_get_int(settings, "protocol"),
	};
	if (params.bitrate == 0)
		params.bitrate = 128;

	char error[512] = {0};
	int fd = icy_connect(&params, error, sizeof(error));

	obs_data_release(settings);

	/* Check if stop was signalled while we were connecting */
	if (os_event_try(out->stop_event) == 0) {
		if (fd >= 0)
			icy_disconnect(fd);
		out->connecting = false;
		return NULL;
	}

	if (fd < 0) {
		blog(LOG_ERROR, "[shoutcast] connection failed: %s", error);
		obs_output_signal_stop(out->output, OBS_OUTPUT_CONNECT_FAILED);
		out->connecting = false;
		return NULL;
	}

	pthread_mutex_lock(&out->write_mutex);
	out->sock_fd = fd;
	pthread_mutex_unlock(&out->write_mutex);

	out->connecting = false;
	obs_output_begin_data_capture(out->output, 0);

	blog(LOG_INFO, "[shoutcast] streaming started");
	return NULL;
}

static bool shoutcast_start(void *data)
{
	struct shoutcast_output *out = data;

	/*
	 * Both calls are mandatory for an encoded output. Without
	 * obs_output_initialize_encoders the audio encoder is never started,
	 * so obs_output_begin_data_capture yields no packets and the socket
	 * stays silent even though the handshake succeeded.
	 */
	if (!obs_output_can_begin_data_capture(out->output, 0))
		return false;
	if (!obs_output_initialize_encoders(out->output, 0)) {
		blog(LOG_ERROR, "[shoutcast] failed to initialize encoders");
		return false;
	}

	os_event_reset(out->stop_event);
	out->connecting = true;

	int ret = pthread_create(&out->connect_thread, NULL, connect_thread,
				 out);
	if (ret != 0) {
		blog(LOG_ERROR, "[shoutcast] failed to create connect thread");
		out->connecting = false;
		return false;
	}

	return true;
}

static void shoutcast_stop(void *data, uint64_t ts)
{
	struct shoutcast_output *out = data;
	UNUSED_PARAMETER(ts);

	os_event_signal(out->stop_event);

	/* Wait for connection thread if it's still running */
	if (out->connecting)
		pthread_join(out->connect_thread, NULL);

	obs_output_end_data_capture(out->output);

	pthread_mutex_lock(&out->write_mutex);
	if (out->sock_fd >= 0) {
		icy_disconnect(out->sock_fd);
		out->sock_fd = -1;
	}
	pthread_mutex_unlock(&out->write_mutex);

	blog(LOG_INFO, "[shoutcast] streaming stopped");
}

/* ------------------------------------------------------------------ */

static void shoutcast_encoded_packet(void *data,
				     struct encoder_packet *packet)
{
	struct shoutcast_output *out = data;

	if (packet->type != OBS_ENCODER_AUDIO)
		return;

	pthread_mutex_lock(&out->write_mutex);

	if (out->sock_fd < 0) {
		pthread_mutex_unlock(&out->write_mutex);
		return;
	}

	if (icy_send(out->sock_fd, packet->data, packet->size) < 0) {
		icy_disconnect(out->sock_fd);
		out->sock_fd = -1;
		pthread_mutex_unlock(&out->write_mutex);
		obs_output_signal_stop(out->output, OBS_OUTPUT_DISCONNECTED);
		return;
	}

	pthread_mutex_unlock(&out->write_mutex);
}

/* ------------------------------------------------------------------ */

static void metadata_job_free(struct metadata_job *job)
{
	bfree(job->server);
	bfree(job->username);
	bfree(job->password);
	bfree(job->mount);
	bfree(job->song);
	bfree(job);
}

static void *metadata_thread(void *arg)
{
	struct metadata_job *job = arg;

	struct icy_params params = {
		.server = job->server,
		.port = job->port,
		.password = job->password,
		.username = job->username,
		.mount = job->mount,
		.protocol = job->protocol,
	};

	char error[512] = {0};
	if (icy_update_metadata(&params, job->song, error, sizeof(error)) == 0)
		blog(LOG_INFO, "[icy] metadata updated: %s", job->song);
	else
		blog(LOG_WARNING, "[icy] metadata update failed: %s", error);

	metadata_job_free(job);
	return NULL;
}

/*
 * Called by libobs whenever the output's settings change. The dock uses it
 * to push a now-playing line without interrupting audio.
 */
static void shoutcast_update(void *data, obs_data_t *settings)
{
	struct shoutcast_output *out = data;

	const char *song = obs_data_get_string(settings, "song");
	if (!song || !*song)
		return;

	/*
	 * Only send while connected, and only when the title actually
	 * changed — otherwise a repeatedly applied identical title would
	 * open a socket every time.
	 */
	pthread_mutex_lock(&out->write_mutex);
	if (out->sock_fd < 0 ||
	    (out->last_song && strcmp(out->last_song, song) == 0)) {
		pthread_mutex_unlock(&out->write_mutex);
		return;
	}
	bfree(out->last_song);
	out->last_song = bstrdup(song);
	pthread_mutex_unlock(&out->write_mutex);

	struct metadata_job *job = bzalloc(sizeof(*job));
	job->server = bstrdup(obs_data_get_string(settings, "server"));
	job->username = bstrdup(obs_data_get_string(settings, "username"));
	job->password = bstrdup(obs_data_get_string(settings, "password"));
	job->mount = bstrdup(obs_data_get_string(settings, "mount"));
	job->song = bstrdup(song);
	job->port = (int)obs_data_get_int(settings, "port");
	job->protocol = (int)obs_data_get_int(settings, "protocol");

	pthread_t thread;
	if (pthread_create(&thread, NULL, metadata_thread, job) != 0) {
		blog(LOG_ERROR,
		     "[icy] failed to create metadata thread; update dropped");
		metadata_job_free(job);
		return;
	}
	pthread_detach(thread);
}

/* ------------------------------------------------------------------ */

static void shoutcast_defaults(obs_data_t *settings)
{
	obs_data_set_default_int(settings, "port", 8000);
	obs_data_set_default_int(settings, "protocol", 0); /* PROTOCOL_ICECAST */
	obs_data_set_default_string(settings, "username", "source");
	obs_data_set_default_string(settings, "mount", "/");
	obs_data_set_default_string(settings, "song", "");
	obs_data_set_default_string(settings, "station_name", "OBS Stream");
	obs_data_set_default_string(settings, "genre", "Various");
	obs_data_set_default_bool(settings, "is_public", false);
	obs_data_set_default_int(settings, "bitrate", 128);
}

static obs_properties_t *shoutcast_properties(void *unused)
{
	UNUSED_PARAMETER(unused);
	obs_properties_t *props = obs_properties_create();

	obs_properties_add_text(props, "server", obs_module_text("Server"),
				OBS_TEXT_DEFAULT);
	obs_properties_add_int(props, "port", obs_module_text("Port"), 1,
			       65535, 1);
	obs_properties_add_text(props, "password",
				obs_module_text("Password"), OBS_TEXT_PASSWORD);
	obs_properties_add_text(props, "station_name",
				obs_module_text("StationName"),
				OBS_TEXT_DEFAULT);
	obs_properties_add_text(props, "genre", obs_module_text("Genre"),
				OBS_TEXT_DEFAULT);
	obs_properties_add_text(props, "url", obs_module_text("URL"),
				OBS_TEXT_DEFAULT);
	obs_properties_add_bool(props, "is_public",
				obs_module_text("Public"));

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

/* ------------------------------------------------------------------ */

struct obs_output_info shoutcast_output_info = {
	.id = "shoutcast_output",
	.flags = OBS_OUTPUT_AUDIO | OBS_OUTPUT_ENCODED,
	.encoded_audio_codecs = "mp3",
	.get_name = shoutcast_getname,
	.create = shoutcast_create,
	.destroy = shoutcast_destroy,
	.start = shoutcast_start,
	.stop = shoutcast_stop,
	.encoded_packet = shoutcast_encoded_packet,
	.get_defaults = shoutcast_defaults,
	.get_properties = shoutcast_properties,
	.update = shoutcast_update,
};
