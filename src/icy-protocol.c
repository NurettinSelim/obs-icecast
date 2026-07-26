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

#include "icy-protocol.h"

#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <netdb.h>
#include <unistd.h>
#include <string.h>
#include <stdio.h>
#include <stdarg.h>
#include <errno.h>

#include <obs.h>

#define ICY_TIMEOUT_SEC 10
#define ICY_RECV_BUF 4096

/* Use MSG_NOSIGNAL on Linux; SO_NOSIGPIPE is set on macOS instead */
#ifdef MSG_NOSIGNAL
#define SEND_FLAGS MSG_NOSIGNAL
#else
#define SEND_FLAGS 0
#endif

/* ------------------------------------------------------------------ */
/* Helpers                                                            */
/* ------------------------------------------------------------------ */

/*
 * Append formatted text to buf, tracking the running length.
 * Returns false on truncation so a malformed request is never sent.
 */
static bool buf_appendf(char *buf, size_t cap, int *len, const char *fmt, ...)
{
	if (*len < 0 || (size_t)*len >= cap)
		return false;

	va_list ap;
	va_start(ap, fmt);
	int n = vsnprintf(buf + *len, cap - (size_t)*len, fmt, ap);
	va_end(ap);

	if (n < 0 || (size_t)n >= cap - (size_t)*len)
		return false;

	*len += n;
	return true;
}

/* HTTP status code from a response buffer, or -1. */
static int http_status_code(const char *resp)
{
	const char *sp = strchr(resp, ' ');
	if (!sp)
		return -1;
	sp++;
	for (int i = 0; i < 3; i++) {
		if (sp[i] < '0' || sp[i] > '9')
			return -1;
	}
	return (sp[0] - '0') * 100 + (sp[1] - '0') * 10 + (sp[2] - '0');
}

/* Standard alphabet A-Za-z0-9+/ with '=' padding, NUL-terminated. */
static void b64_encode(const char *in, char *out, size_t out_len)
{
	static const char tbl[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZ"
				  "abcdefghijklmnopqrstuvwxyz"
				  "0123456789+/";
	size_t in_len = strlen(in);
	size_t o = 0;

	for (size_t i = 0; i < in_len; i += 3) {
		size_t rem = in_len - i;
		unsigned v = (unsigned)(unsigned char)in[i] << 16;
		if (rem > 1)
			v |= (unsigned)(unsigned char)in[i + 1] << 8;
		if (rem > 2)
			v |= (unsigned)(unsigned char)in[i + 2];

		if (o + 4 >= out_len)
			break;

		out[o++] = tbl[(v >> 18) & 0x3F];
		out[o++] = tbl[(v >> 12) & 0x3F];
		out[o++] = rem > 1 ? tbl[(v >> 6) & 0x3F] : '=';
		out[o++] = rem > 2 ? tbl[v & 0x3F] : '=';
	}
	out[o] = '\0';
}

/*
 * Unreserved A-Za-z0-9-_.~ pass through; every other BYTE becomes %XX in
 * uppercase hex. Byte-wise over UTF-8 — this is what makes non-ASCII titles
 * round-trip. Callers size out at strlen(in) * 3 + 1.
 */
static void uri_encode(const char *in, char *out, size_t out_len)
{
	static const char hex[] = "0123456789ABCDEF";
	size_t o = 0;

	for (const unsigned char *p = (const unsigned char *)in; *p; p++) {
		unsigned char c = *p;
		bool unreserved = (c >= 'A' && c <= 'Z') ||
				  (c >= 'a' && c <= 'z') ||
				  (c >= '0' && c <= '9') || c == '-' ||
				  c == '_' || c == '.' || c == '~';

		if (unreserved) {
			if (o + 1 >= out_len)
				break;
			out[o++] = (char)c;
		} else {
			if (o + 3 >= out_len)
				break;
			out[o++] = '%';
			out[o++] = hex[c >> 4];
			out[o++] = hex[c & 0x0F];
		}
	}
	out[o] = '\0';
}

/* NULL/empty -> "/"; otherwise ensure exactly one leading '/'. */
static void normalize_mount(const char *in, char *out, size_t out_len)
{
	if (!in || !*in) {
		snprintf(out, out_len, "/");
		return;
	}
	while (*in == '/')
		in++;
	snprintf(out, out_len, "/%s", in);
}

/* Build "Basic <base64>" credentials for the Icecast/harbor auth header. */
static void build_basic_auth(const struct icy_params *params, char *out,
			     size_t out_len)
{
	const char *user = (params->username && *params->username)
				   ? params->username
				   : "source";
	char userpass[512];

	snprintf(userpass, sizeof(userpass), "%s:%s", user,
		 params->password ? params->password : "");
	b64_encode(userpass, out, out_len);
}

/* ------------------------------------------------------------------ */
/* TCP connection                                                     */
/* ------------------------------------------------------------------ */

static int icy_tcp_connect(const char *server, int port, char *error_buf,
			   size_t error_len)
{
	struct addrinfo hints = {0};
	struct addrinfo *res = NULL;
	char port_str[16];
	int sock_fd = -1;

	snprintf(port_str, sizeof(port_str), "%d", port);

	hints.ai_family = AF_UNSPEC;
	hints.ai_socktype = SOCK_STREAM;
	hints.ai_protocol = IPPROTO_TCP;

	int ret = getaddrinfo(server, port_str, &hints, &res);
	if (ret != 0) {
		snprintf(error_buf, error_len,
			 "DNS resolution failed for %s: %s", server,
			 gai_strerror(ret));
		return -1;
	}

	for (struct addrinfo *ai = res; ai != NULL; ai = ai->ai_next) {
		sock_fd = socket(ai->ai_family, ai->ai_socktype,
				 ai->ai_protocol);
		if (sock_fd < 0)
			continue;
		if (connect(sock_fd, ai->ai_addr, ai->ai_addrlen) == 0)
			break;
		close(sock_fd);
		sock_fd = -1;
	}
	freeaddrinfo(res);

	if (sock_fd < 0) {
		snprintf(error_buf, error_len, "Failed to connect to %s:%d: %s",
			 server, port, strerror(errno));
		return -1;
	}

	/* Socket options */
#ifdef SO_NOSIGPIPE
	int nosigpipe = 1;
	setsockopt(sock_fd, SOL_SOCKET, SO_NOSIGPIPE, &nosigpipe,
		   sizeof(nosigpipe));
#endif

	int nodelay = 1;
	setsockopt(sock_fd, IPPROTO_TCP, TCP_NODELAY, &nodelay,
		   sizeof(nodelay));

	struct timeval tv = {.tv_sec = ICY_TIMEOUT_SEC, .tv_usec = 0};
	setsockopt(sock_fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
	setsockopt(sock_fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

	return sock_fd;
}

/* ------------------------------------------------------------------ */
/* Handshakes                                                         */
/* ------------------------------------------------------------------ */

static int icy_handshake_shoutcast(int sock_fd,
				   const struct icy_params *params,
				   char *error_buf, size_t error_len)
{
	/* 1. Send password */
	char auth[512];
	int auth_len = snprintf(auth, sizeof(auth), "%s\r\n",
				params->password ? params->password : "");
	if (send(sock_fd, auth, auth_len, SEND_FLAGS) < 0) {
		snprintf(error_buf, error_len, "Failed to send password: %s",
			 strerror(errno));
		close(sock_fd);
		return -1;
	}

	/* 2. Read server response — expect "OK" */
	char recv_buf[ICY_RECV_BUF];
	ssize_t n = recv(sock_fd, recv_buf, sizeof(recv_buf) - 1, 0);
	if (n <= 0) {
		snprintf(error_buf, error_len,
			 "No response from server (connection may have been rejected)");
		close(sock_fd);
		return -1;
	}
	recv_buf[n] = '\0';

	blog(LOG_DEBUG, "[icy] server response: %s", recv_buf);

	if (strstr(recv_buf, "OK") == NULL) {
		snprintf(error_buf, error_len,
			 "Server rejected authentication: %.128s", recv_buf);
		close(sock_fd);
		return -1;
	}

	/* 3. Send ICY headers */
	char headers[2048];
	int hdr_len = snprintf(
		headers, sizeof(headers),
		"icy-name:%s\r\n"
		"icy-genre:%s\r\n"
		"icy-url:%s\r\n"
		"icy-pub:%d\r\n"
		"icy-br:%d\r\n"
		"content-type:audio/mpeg\r\n"
		"\r\n",
		params->name ? params->name : "OBS Stream",
		params->genre ? params->genre : "Various",
		params->url ? params->url : "",
		params->is_public ? 1 : 0, params->bitrate);

	if (send(sock_fd, headers, hdr_len, SEND_FLAGS) < 0) {
		snprintf(error_buf, error_len,
			 "Failed to send ICY headers: %s", strerror(errno));
		close(sock_fd);
		return -1;
	}

	blog(LOG_INFO, "[icy] connected to %s:%d (source port %d, %d kbps)",
	     params->server, params->port, params->port + 1, params->bitrate);

	return sock_fd;
}

static int icy_handshake_icecast(int sock_fd, const struct icy_params *params,
				 char *error_buf, size_t error_len)
{
	const char *url = params->url ? params->url : "";
	char mount[256];
	char auth_b64[768];
	char headers[2048];
	int hdr_len = 0;
	bool ok;

	normalize_mount(params->mount, mount, sizeof(mount));
	build_basic_auth(params, auth_b64, sizeof(auth_b64));

	/* Fall back to sane values so ice-audio-info is never malformed. */
	int channels = params->channels > 0 ? params->channels : 2;
	int samplerate = params->samplerate > 0 ? params->samplerate : 44100;

	ok = buf_appendf(headers, sizeof(headers), &hdr_len,
			 "SOURCE %s HTTP/1.0\r\n"
			 "Authorization: Basic %s\r\n"
			 "Host: %s:%d\r\n"
			 "User-Agent: obs-shoutcast/%s\r\n"
			 "Content-Type: audio/mpeg\r\n"
			 "ice-name: %s\r\n"
			 "ice-genre: %s\r\n",
			 mount, auth_b64, params->server, params->port,
			 PLUGIN_VERSION,
			 params->name ? params->name : "OBS Stream",
			 params->genre ? params->genre : "Various");

	/* Omit ice-url entirely when no URL is configured. */
	if (ok && *url)
		ok = buf_appendf(headers, sizeof(headers), &hdr_len,
				 "ice-url: %s\r\n", url);

	if (ok)
		ok = buf_appendf(
			headers, sizeof(headers), &hdr_len,
			"ice-public: %d\r\n"
			"ice-bitrate: %d\r\n"
			"ice-audio-info: ice-bitrate=%d;ice-channels=%d;ice-samplerate=%d\r\n"
			"\r\n",
			params->is_public ? 1 : 0, params->bitrate,
			params->bitrate, channels, samplerate);

	if (!ok) {
		snprintf(error_buf, error_len,
			 "Handshake headers too large to send");
		close(sock_fd);
		return -1;
	}

	if (send(sock_fd, headers, (size_t)hdr_len, SEND_FLAGS) < 0) {
		snprintf(error_buf, error_len,
			 "Failed to send SOURCE request: %s", strerror(errno));
		close(sock_fd);
		return -1;
	}

	char recv_buf[ICY_RECV_BUF];
	ssize_t n = recv(sock_fd, recv_buf, sizeof(recv_buf) - 1, 0);
	if (n <= 0) {
		snprintf(error_buf, error_len,
			 "No response from server (connection may have been rejected)");
		close(sock_fd);
		return -1;
	}
	recv_buf[n] = '\0';

	blog(LOG_DEBUG, "[icecast] server response: %s", recv_buf);

	int status = http_status_code(recv_buf);
	if (status != 200) {
		switch (status) {
		case 401:
			snprintf(error_buf, error_len,
				 "Invalid username or password");
			break;
		case 403:
			snprintf(error_buf, error_len,
				 "Mount point %s is already in use", mount);
			break;
		case 404:
			snprintf(error_buf, error_len,
				 "Mount point %s not found on server", mount);
			break;
		default: {
			/* Report only the first response line. */
			char first[160];
			size_t i = 0;
			while (recv_buf[i] && recv_buf[i] != '\r' &&
			       recv_buf[i] != '\n' && i < sizeof(first) - 1) {
				first[i] = recv_buf[i];
				i++;
			}
			first[i] = '\0';
			snprintf(error_buf, error_len,
				 "Server rejected connection: %.128s", first);
			break;
		}
		}
		close(sock_fd);
		return -1;
	}

	blog(LOG_INFO, "[icecast] connected to %s:%d%s (%d kbps)",
	     params->server, params->port, mount, params->bitrate);

	return sock_fd;
}

/* ------------------------------------------------------------------ */
/* Public API                                                         */
/* ------------------------------------------------------------------ */

int icy_connect(const struct icy_params *params, char *error_buf,
		size_t error_len)
{
	/*
	 * SHOUTcast v1 splits its ports: the configured port is the admin /
	 * listener port and the source port is port + 1. Verified against
	 * Radio.co's maple endpoint — 4192 never answers a password
	 * handshake, 4193 replies "OK2\r\nicy-caps:11". butt does the same
	 * (src/shoutcast.cpp:66-217). Metadata still goes to the base port,
	 * which is why icy_update_metadata does not apply this offset.
	 */
	const bool shoutcast = params->protocol == PROTOCOL_SHOUTCAST_V1;
	const int port = shoutcast ? params->port + 1 : params->port;

	int sock_fd = icy_tcp_connect(params->server, port, error_buf,
				      error_len);
	if (sock_fd < 0)
		return -1;

	if (shoutcast)
		return icy_handshake_shoutcast(sock_fd, params, error_buf,
					       error_len);

	return icy_handshake_icecast(sock_fd, params, error_buf, error_len);
}

int icy_update_metadata(const struct icy_params *params, const char *song,
			char *error_buf, size_t error_len)
{
	if (!song || !*song) {
		snprintf(error_buf, error_len, "No song text to send");
		return -1;
	}

	char request[2048];
	int len = 0;
	bool ok;

	size_t song_cap = strlen(song) * 3 + 1;
	char *song_enc = bmalloc(song_cap);
	uri_encode(song, song_enc, song_cap);

	if (params->protocol == PROTOCOL_SHOUTCAST_V1) {
		const char *pw = params->password ? params->password : "";
		size_t pw_cap = strlen(pw) * 3 + 1;
		char *pw_enc = bmalloc(pw_cap);
		uri_encode(pw, pw_enc, pw_cap);

		ok = buf_appendf(
			request, sizeof(request), &len,
			"GET /admin.cgi?pass=%s&mode=updinfo&song=%s&url= HTTP/1.0\r\n"
			"User-Agent: ShoutcastDSP (Mozilla Compatible)\r\n"
			"Host: %s:%d\r\n"
			"\r\n",
			pw_enc, song_enc, params->server, params->port);

		bfree(pw_enc);
	} else {
		char mount[256];
		char auth_b64[768];

		normalize_mount(params->mount, mount, sizeof(mount));
		build_basic_auth(params, auth_b64, sizeof(auth_b64));

		/*
		 * One free-text line as a single song= parameter. The server
		 * also accepts artist=/title= and would rejoin them with
		 * " - "; that reformatting is deliberately avoided.
		 */
		ok = buf_appendf(
			request, sizeof(request), &len,
			"GET /admin/metadata?mode=updinfo&mount=%s&song=%s HTTP/1.0\r\n"
			"Authorization: Basic %s\r\n"
			"Host: %s:%d\r\n"
			"User-Agent: obs-shoutcast/%s\r\n"
			"\r\n",
			mount, song_enc, auth_b64, params->server,
			params->port, PLUGIN_VERSION);
	}

	bfree(song_enc);

	if (!ok) {
		snprintf(error_buf, error_len, "Metadata request too large");
		return -1;
	}

	int sock_fd = icy_tcp_connect(params->server, params->port, error_buf,
				      error_len);
	if (sock_fd < 0)
		return -1;

	if (send(sock_fd, request, (size_t)len, SEND_FLAGS) < 0) {
		snprintf(error_buf, error_len,
			 "Failed to send metadata request: %s",
			 strerror(errno));
		close(sock_fd);
		return -1;
	}

	char recv_buf[ICY_RECV_BUF];
	ssize_t n = recv(sock_fd, recv_buf, sizeof(recv_buf) - 1, 0);
	close(sock_fd);

	if (n <= 0) {
		snprintf(error_buf, error_len,
			 "No response to metadata update");
		return -1;
	}
	recv_buf[n] = '\0';

	int status = http_status_code(recv_buf);
	if (status != 200) {
		snprintf(error_buf, error_len,
			 "Metadata update rejected (HTTP %d)", status);
		return -1;
	}

	return 0;
}

int icy_send(int sock_fd, const void *data, size_t len)
{
	const uint8_t *ptr = data;
	size_t remaining = len;

	while (remaining > 0) {
		ssize_t sent = send(sock_fd, ptr, remaining, SEND_FLAGS);
		if (sent <= 0) {
			blog(LOG_WARNING, "[icy] send failed: %s",
			     sent == 0 ? "connection closed"
				       : strerror(errno));
			return -1;
		}
		ptr += sent;
		remaining -= (size_t)sent;
	}
	return 0;
}

void icy_disconnect(int sock_fd)
{
	if (sock_fd >= 0) {
		shutdown(sock_fd, SHUT_RDWR);
		close(sock_fd);
	}
}
