#pragma once

#include <stdbool.h>
#include <stddef.h>

enum stream_protocol {
	PROTOCOL_ICECAST = 0, /* HTTP SOURCE — default, works with Radio.co */
	PROTOCOL_SHOUTCAST_V1, /* Legacy ICY protocol */
};

struct icy_params {
	const char *server;
	int port;
	const char *password;
	const char *username; /* Icecast only (default: "source") */
	const char *mount;    /* Icecast only (default: "/") */
	const char *name;
	const char *genre;
	const char *url;
	bool is_public;
	int bitrate;
	int channels;   /* from OBS audio, for ice-audio-info */
	int samplerate; /* from OBS audio, for ice-audio-info */
	enum stream_protocol protocol;
};

/*
 * Connect to a streaming server.
 * Supports both Icecast (HTTP SOURCE) and SHOUTcast v1 (ICY) protocols.
 * Returns a connected socket fd on success, -1 on failure.
 */
int icy_connect(const struct icy_params *params, char *error_buf,
		size_t error_len);

/*
 * Push a now-playing line. The string is sent verbatim as the whole title.
 * Opens and closes its own short-lived connection, so it is safe to call
 * from a background thread while audio is streaming.
 * Returns 0 on success, -1 on failure.
 */
int icy_update_metadata(const struct icy_params *params, const char *song,
			char *error_buf, size_t error_len);

/*
 * Send raw audio data over an established connection.
 * Returns 0 on success, -1 on failure.
 */
int icy_send(int sock_fd, const void *data, size_t len);

/*
 * Gracefully disconnect from the server.
 */
void icy_disconnect(int sock_fd);
