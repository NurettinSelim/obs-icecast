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

#include "kick-api.h"

#include <sys/types.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <netinet/in.h>
#include <poll.h>
#include <unistd.h>
#include <string.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <errno.h>

#include <curl/curl.h>
#include <CommonCrypto/CommonDigest.h>
#include <CoreFoundation/CoreFoundation.h>
#include <Security/Security.h>

#include <obs.h>
#include <util/dstr.h>
#include <util/platform.h>
#include <util/threading.h>

#define KICK_AUTHORIZE_URL "https://id.kick.com/oauth/authorize"
#define KICK_TOKEN_URL "https://id.kick.com/oauth/token"
#define KICK_CHANNELS_URL "https://api.kick.com/public/v1/channels"
#define KICK_KEYCHAIN_SERVICE "io.github.nurettinselim.obs-icecast.kick"

#define KICK_CONNECT_TIMEOUT_SEC 10L
#define KICK_TOTAL_TIMEOUT_SEC 20L
#define KICK_MAX_BODY (64 * 1024)
#define KICK_MAX_REQUEST 8192

/* ------------------------------------------------------------------ */
/* HTTPS                                                              */
/* ------------------------------------------------------------------ */

struct http_ctx {
	struct dstr body;
	const volatile bool *abort;
};

static size_t on_body(char *ptr, size_t size, size_t nmemb, void *userdata)
{
	struct http_ctx *ctx = userdata;
	const size_t n = size * nmemb;

	/* Refusing the bytes makes curl fail the transfer: no endless body. */
	if (ctx->body.len + n > KICK_MAX_BODY)
		return 0;
	dstr_ncat(&ctx->body, ptr, n);
	return n;
}

static int on_progress(void *userdata, curl_off_t dltotal, curl_off_t dlnow,
		       curl_off_t ultotal, curl_off_t ulnow)
{
	UNUSED_PARAMETER(dltotal);
	UNUSED_PARAMETER(dlnow);
	UNUSED_PARAMETER(ultotal);
	UNUSED_PARAMETER(ulnow);

	const struct http_ctx *ctx = userdata;
	return ctx->abort && os_atomic_load_bool(ctx->abort) ? 1 : 0;
}

static void http_request(const char *method, const char *url,
			 const char *bearer, const char *content_type,
			 const char *body, const volatile bool *abort,
			 struct kick_response *resp)
{
	memset(resp, 0, sizeof(*resp));

	CURL *curl = curl_easy_init();
	if (!curl) {
		snprintf(resp->error, sizeof(resp->error),
			 "could not start an HTTPS request");
		return;
	}

	struct http_ctx ctx = {0};
	ctx.abort = abort;

	struct curl_slist *headers = NULL;
	struct dstr line = {0};
	if (bearer) {
		dstr_printf(&line, "Authorization: Bearer %s", bearer);
		headers = curl_slist_append(headers, line.array);
	}
	if (content_type) {
		dstr_printf(&line, "Content-Type: %s", content_type);
		headers = curl_slist_append(headers, line.array);
	}
	headers = curl_slist_append(headers, "Accept: application/json");

	curl_easy_setopt(curl, CURLOPT_URL, url);
	if (strcmp(method, "GET") != 0)
		curl_easy_setopt(curl, CURLOPT_CUSTOMREQUEST, method);
	if (body) {
		curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body);
		curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, (long)strlen(body));
	}
	curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
	curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, on_body);
	curl_easy_setopt(curl, CURLOPT_WRITEDATA, &ctx);
	curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 0L);
	curl_easy_setopt(curl, CURLOPT_XFERINFOFUNCTION, on_progress);
	curl_easy_setopt(curl, CURLOPT_XFERINFODATA, &ctx);
	curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, KICK_CONNECT_TIMEOUT_SEC);
	curl_easy_setopt(curl, CURLOPT_TIMEOUT, KICK_TOTAL_TIMEOUT_SEC);
	curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
	curl_easy_setopt(curl, CURLOPT_USERAGENT, "obs-icecast/" PLUGIN_VERSION);

	const CURLcode rc = curl_easy_perform(curl);
	if (rc == CURLE_OK) {
		curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &resp->status);
	} else {
		resp->aborted = rc == CURLE_ABORTED_BY_CALLBACK;
		snprintf(resp->error, sizeof(resp->error), "%s",
			 resp->aborted ? "cancelled" : curl_easy_strerror(rc));
	}

	/* The body buffer changes hands; kick_response_free releases it. */
	resp->body = ctx.body.len ? ctx.body.array : NULL;
	if (!resp->body)
		dstr_free(&ctx.body);

	curl_slist_free_all(headers);
	/* The bearer line held a token; do not leave it in freed memory. */
	if (line.array)
		memset(line.array, 0, line.capacity);
	dstr_free(&line);
	curl_easy_cleanup(curl);
}

static char *form_escape(const char *s)
{
	char *esc = curl_easy_escape(NULL, s ? s : "", 0);
	char *out = bstrdup(esc ? esc : "");
	curl_free(esc);
	return out;
}

/* key=value&... with every value URL-encoded. NULL ends the list. */
static char *form_encode(const char *first_key, ...)
{
	struct dstr out = {0};
	va_list ap;
	va_start(ap, first_key);
	for (const char *key = first_key; key; key = va_arg(ap, const char *)) {
		char *val = form_escape(va_arg(ap, const char *));
		dstr_catf(&out, "%s%s=%s", out.len ? "&" : "", key, val);
		bfree(val);
	}
	va_end(ap);
	return out.array;
}

static void free_secret(char *s)
{
	if (s) {
		memset(s, 0, strlen(s));
		bfree(s);
	}
}

void kick_response_free(struct kick_response *resp)
{
	bfree(resp->body);
	resp->body = NULL;
}

char *kick_response_reason(const struct kick_response *resp)
{
	if (resp->status == 0)
		return bstrdup(resp->error[0] ? resp->error : "no answer");

	struct dstr out = {0};
	obs_data_t *d = resp->body ? obs_data_create_from_json(resp->body)
				   : NULL;
	const char *msg = NULL;
	if (d) {
		const char *keys[] = {"message", "error_description", "error"};
		for (size_t i = 0; i < sizeof(keys) / sizeof(keys[0]); i++) {
			const char *v = obs_data_get_string(d, keys[i]);
			if (v && *v) {
				msg = v;
				break;
			}
		}
	}
	if (msg)
		dstr_printf(&out, "HTTP %ld: %.160s", resp->status, msg);
	else
		dstr_printf(&out, "HTTP %ld", resp->status);
	obs_data_release(d);
	return out.array;
}

void kick_global_init(void)
{
	curl_global_init(CURL_GLOBAL_DEFAULT);
}

void kick_global_free(void)
{
	curl_global_cleanup();
}

void kick_token_from_code(const char *client_id, const char *client_secret,
			  const char *code, const char *code_verifier,
			  const volatile bool *abort,
			  struct kick_response *resp)
{
	char *body = form_encode("grant_type", "authorization_code",
				 "client_id", client_id, "client_secret",
				 client_secret, "redirect_uri",
				 KICK_REDIRECT_URI, "code_verifier",
				 code_verifier, "code", code, NULL);
	http_request("POST", KICK_TOKEN_URL, NULL,
		     "application/x-www-form-urlencoded", body, abort, resp);
	free_secret(body);
}

void kick_token_from_refresh(const char *client_id, const char *client_secret,
			     const char *refresh_token,
			     const volatile bool *abort,
			     struct kick_response *resp)
{
	char *body = form_encode("grant_type", "refresh_token", "client_id",
				 client_id, "client_secret", client_secret,
				 "refresh_token", refresh_token, NULL);
	http_request("POST", KICK_TOKEN_URL, NULL,
		     "application/x-www-form-urlencoded", body, abort, resp);
	free_secret(body);
}

void kick_patch_title(const char *access_token, const char *title,
		      const volatile bool *abort, struct kick_response *resp)
{
	/* obs_data writes real JSON: quotes, backslashes and UTF-8 survive. */
	obs_data_t *d = obs_data_create();
	obs_data_set_string(d, "stream_title", title);
	char *body = bstrdup(obs_data_get_json(d));
	obs_data_release(d);

	http_request("PATCH", KICK_CHANNELS_URL, access_token,
		     "application/json", body, abort, resp);
	bfree(body);
}

void kick_get_channel(const char *access_token, const volatile bool *abort,
		      struct kick_response *resp)
{
	http_request("GET", KICK_CHANNELS_URL, access_token, NULL, NULL, abort,
		     resp);
}

/* Kick's docs show expires_in as a string; real responses send a number. */
static long long data_get_number(obs_data_t *d, const char *key)
{
	obs_data_item_t *item = obs_data_item_byname(d, key);
	long long v = 0;
	if (item) {
		if (obs_data_item_gettype(item) == OBS_DATA_NUMBER)
			v = obs_data_item_get_int(item);
		else if (obs_data_item_gettype(item) == OBS_DATA_STRING)
			v = atoll(obs_data_item_get_string(item));
		obs_data_item_release(&item);
	}
	return v;
}

bool kick_parse_tokens(const char *body, char **access_token,
		       char **refresh_token, long long *expires_in)
{
	*access_token = NULL;
	*refresh_token = NULL;
	*expires_in = 0;

	obs_data_t *d = body ? obs_data_create_from_json(body) : NULL;
	if (!d)
		return false;

	const char *access = obs_data_get_string(d, "access_token");
	const char *refresh = obs_data_get_string(d, "refresh_token");
	const bool ok = access && *access && refresh && *refresh;
	if (ok) {
		*access_token = bstrdup(access);
		*refresh_token = bstrdup(refresh);
		*expires_in = data_get_number(d, "expires_in");
	}
	obs_data_release(d);
	return ok;
}

bool kick_parse_channel(const char *body, char **slug, char **stream_title)
{
	*slug = NULL;
	*stream_title = NULL;

	obs_data_t *d = body ? obs_data_create_from_json(body) : NULL;
	if (!d)
		return false;

	obs_data_array_t *arr = obs_data_get_array(d, "data");
	obs_data_t *first = arr && obs_data_array_count(arr)
				    ? obs_data_array_item(arr, 0)
				    : NULL;
	if (first) {
		*slug = bstrdup(obs_data_get_string(first, "slug"));
		*stream_title =
			bstrdup(obs_data_get_string(first, "stream_title"));
		obs_data_release(first);
	}
	obs_data_array_release(arr);
	obs_data_release(d);
	return *slug && **slug;
}

/* ------------------------------------------------------------------ */
/* Browser sign-in: PKCE and the loopback redirect listener           */
/* ------------------------------------------------------------------ */

struct kick_auth {
	int fds[2]; /* 127.0.0.1 and ::1 — "localhost" may resolve to either */
	int nfds;
	char verifier[64];
	char state[32];
	char *url;
};

static void base64url(const uint8_t *in, size_t len, char *out)
{
	static const char tbl[] =
		"ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";
	size_t o = 0;
	for (size_t i = 0; i < len; i += 3) {
		const uint32_t n = (uint32_t)in[i] << 16 |
				   (i + 1 < len ? (uint32_t)in[i + 1] << 8 : 0) |
				   (i + 2 < len ? (uint32_t)in[i + 2] : 0);
		out[o++] = tbl[(n >> 18) & 63];
		out[o++] = tbl[(n >> 12) & 63];
		if (i + 1 < len)
			out[o++] = tbl[(n >> 6) & 63];
		if (i + 2 < len)
			out[o++] = tbl[n & 63];
	}
	out[o] = '\0';
}

/* nbytes of CSPRNG output, base64url without padding. */
static bool random_token(size_t nbytes, char *out, size_t out_len)
{
	uint8_t buf[48];
	if (nbytes > sizeof(buf) || (nbytes + 2) / 3 * 4 + 1 > out_len)
		return false;
	if (SecRandomCopyBytes(kSecRandomDefault, nbytes, buf) != errSecSuccess)
		return false;
	base64url(buf, nbytes, out);
	memset(buf, 0, sizeof(buf));
	return true;
}

static int listen_loopback(int family)
{
	int fd = socket(family, SOCK_STREAM, 0);
	if (fd < 0)
		return -1;

	int one = 1;
	setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));

	int rc;
	if (family == AF_INET6) {
		setsockopt(fd, IPPROTO_IPV6, IPV6_V6ONLY, &one, sizeof(one));
		struct sockaddr_in6 a = {0};
		a.sin6_len = sizeof(a);
		a.sin6_family = AF_INET6;
		a.sin6_port = htons(KICK_REDIRECT_PORT);
		a.sin6_addr = in6addr_loopback;
		rc = bind(fd, (struct sockaddr *)&a, sizeof(a));
	} else {
		struct sockaddr_in a = {0};
		a.sin_len = sizeof(a);
		a.sin_family = AF_INET;
		a.sin_port = htons(KICK_REDIRECT_PORT);
		a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
		rc = bind(fd, (struct sockaddr *)&a, sizeof(a));
	}

	if (rc < 0 || listen(fd, 4) < 0) {
		const int saved = errno;
		close(fd);
		errno = saved;
		return -1;
	}
	return fd;
}

struct kick_auth *kick_auth_begin(const char *client_id, char *error,
				  size_t error_len)
{
	struct kick_auth *auth = bzalloc(sizeof(*auth));

	if (!random_token(32, auth->verifier, sizeof(auth->verifier)) ||
	    !random_token(16, auth->state, sizeof(auth->state))) {
		snprintf(error, error_len, "No random bytes for the sign-in");
		bfree(auth);
		return NULL;
	}

	const int fd4 = listen_loopback(AF_INET);
	if (fd4 < 0) {
		snprintf(error, error_len,
			 "Port %d is busy (%s); close what uses it and retry",
			 KICK_REDIRECT_PORT, strerror(errno));
		bfree(auth);
		return NULL;
	}
	auth->fds[auth->nfds++] = fd4;
	/* IPv6 loopback is optional: browsers fall back to 127.0.0.1. */
	const int fd6 = listen_loopback(AF_INET6);
	if (fd6 >= 0)
		auth->fds[auth->nfds++] = fd6;

	uint8_t digest[CC_SHA256_DIGEST_LENGTH];
	CC_SHA256(auth->verifier, (CC_LONG)strlen(auth->verifier), digest);
	char challenge[48];
	base64url(digest, sizeof(digest), challenge);

	char *id = form_escape(client_id);
	char *redirect = form_escape(KICK_REDIRECT_URI);
	char *scope = form_escape(KICK_SCOPES);
	struct dstr url = {0};
	dstr_printf(&url,
		    KICK_AUTHORIZE_URL "?response_type=code&client_id=%s"
				       "&redirect_uri=%s&scope=%s"
				       "&code_challenge=%s"
				       "&code_challenge_method=S256&state=%s",
		    id, redirect, scope, challenge, auth->state);
	bfree(id);
	bfree(redirect);
	bfree(scope);
	auth->url = url.array;

	return auth;
}

const char *kick_auth_url(const struct kick_auth *auth)
{
	return auth->url;
}

const char *kick_auth_verifier(const struct kick_auth *auth)
{
	return auth->verifier;
}

static int hex_digit(char c)
{
	if (c >= '0' && c <= '9')
		return c - '0';
	if (c >= 'a' && c <= 'f')
		return c - 'a' + 10;
	if (c >= 'A' && c <= 'F')
		return c - 'A' + 10;
	return -1;
}

/* Value of key in an a=b&c=d query, URL-decoded, bstrdup'd; NULL if absent. */
static char *query_param(const char *query, const char *key)
{
	const size_t klen = strlen(key);
	const char *p = query;
	while (p && *p) {
		const char *end = strchr(p, '&');
		const size_t len = end ? (size_t)(end - p) : strlen(p);
		if (len > klen && strncmp(p, key, klen) == 0 && p[klen] == '=') {
			const char *v = p + klen + 1;
			const size_t vlen = len - klen - 1;
			char *out = bmalloc(vlen + 1);
			size_t o = 0;
			for (size_t i = 0; i < vlen; i++) {
				int hi, lo;
				if (v[i] == '%' && i + 2 < vlen &&
				    (hi = hex_digit(v[i + 1])) >= 0 &&
				    (lo = hex_digit(v[i + 2])) >= 0) {
					out[o++] = (char)(hi << 4 | lo);
					i += 2;
				} else {
					out[o++] = v[i] == '+' ? ' ' : v[i];
				}
			}
			out[o] = '\0';
			return out;
		}
		p = end ? end + 1 : NULL;
	}
	return NULL;
}

static void send_page(int fd, int status, const char *message)
{
	struct dstr page = {0};
	dstr_printf(&page,
		    "<!doctype html><meta charset=\"utf-8\">"
		    "<title>obs-icecast</title>"
		    "<body style=\"font:16px -apple-system,sans-serif;"
		    "margin:3em\"><p>%s</p></body>",
		    message);

	struct dstr resp = {0};
	dstr_printf(&resp,
		    "HTTP/1.1 %d %s\r\n"
		    "Content-Type: text/html; charset=utf-8\r\n"
		    "Content-Length: %zu\r\n"
		    "Cache-Control: no-store\r\n"
		    "Connection: close\r\n\r\n%s",
		    status, status == 200 ? "OK" : "Not Found", page.len,
		    page.array);
	send(fd, resp.array, resp.len, 0);
	dstr_free(&resp);
	dstr_free(&page);
}

/*
 * One connection to the listener. Returns 1 with *code set when the
 * redirect carried a valid code, -1 with error set when it carried a
 * refusal or a wrong state, 0 for anything else (favicon, a browser's
 * speculative preconnect that never sends, garbage) — keep waiting.
 */
static int serve_connection(struct kick_auth *auth, int fd, char **code,
			    char *error, size_t error_len)
{
	const struct timeval tv = {.tv_sec = 5};
	setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
	int one = 1;
	setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof(one));

	char req[KICK_MAX_REQUEST];
	size_t len = 0;
	while (len < sizeof(req) - 1) {
		const ssize_t n = recv(fd, req + len, sizeof(req) - 1 - len, 0);
		if (n <= 0)
			break;
		len += (size_t)n;
		req[len] = '\0';
		if (strstr(req, "\r\n\r\n"))
			break;
	}
	req[len] = '\0';

	if (strncmp(req, "GET /callback", 13) != 0 ||
	    (req[13] != '?' && req[13] != ' ')) {
		if (len)
			send_page(fd, 404, "Not found.");
		return 0;
	}

	char *line_end = strstr(req, " HTTP/");
	if (line_end)
		*line_end = '\0';
	const char *query = req[13] == '?' ? req + 14 : "";

	char *state = query_param(query, "state");
	char *err = query_param(query, "error");
	char *got = query_param(query, "code");
	int result;

	if (!state || strcmp(state, auth->state) != 0) {
		/* Not our request — a stale tab or someone else. */
		snprintf(error, error_len,
			 "Sign-in answer did not match this request; "
			 "press Connect Kick again");
		send_page(fd, 200,
			  "This sign-in is out of date. Go back to OBS and "
			  "press <b>Connect Kick</b> again.");
		result = -1;
	} else if (err || !got || !*got) {
		snprintf(error, error_len, "Kick did not grant access (%.80s)",
			 err ? err : "no code");
		send_page(fd, 200,
			  "Kick did not grant access. Go back to OBS and try "
			  "again.");
		result = -1;
	} else {
		*code = got;
		got = NULL;
		send_page(fd, 200,
			  "obs-icecast received the Kick sign-in. You can "
			  "close this tab and go back to OBS.");
		result = 1;
	}

	bfree(state);
	bfree(err);
	free_secret(got);
	return result;
}

char *kick_auth_wait(struct kick_auth *auth, int timeout_sec,
		     const volatile bool *abort, char *error,
		     size_t error_len)
{
	const uint64_t deadline =
		os_gettime_ns() + (uint64_t)timeout_sec * 1000000000ULL;
	error[0] = '\0';

	while (os_gettime_ns() < deadline) {
		if (abort && os_atomic_load_bool(abort))
			return NULL;

		struct pollfd pfd[2];
		for (int i = 0; i < auth->nfds; i++)
			pfd[i] = (struct pollfd){.fd = auth->fds[i],
						 .events = POLLIN};

		/* Short ticks so an abort is noticed within a quarter second. */
		const int n = poll(pfd, (nfds_t)auth->nfds, 250);
		if (n < 0 && errno != EINTR) {
			snprintf(error, error_len, "Sign-in listener failed: %s",
				 strerror(errno));
			return NULL;
		}

		for (int i = 0; n > 0 && i < auth->nfds; i++) {
			if (!(pfd[i].revents & POLLIN))
				continue;
			const int conn = accept(auth->fds[i], NULL, NULL);
			if (conn < 0)
				continue;
			char *code = NULL;
			const int r = serve_connection(auth, conn, &code, error,
						       error_len);
			close(conn);
			if (r > 0)
				return code;
			if (r < 0)
				return NULL;
		}
	}

	snprintf(error, error_len,
		 "No answer from the browser within %d minutes",
		 timeout_sec / 60);
	return NULL;
}

void kick_auth_free(struct kick_auth *auth)
{
	if (!auth)
		return;
	for (int i = 0; i < auth->nfds; i++)
		close(auth->fds[i]);
	memset(auth->verifier, 0, sizeof(auth->verifier));
	bfree(auth->url);
	bfree(auth);
}

/* ------------------------------------------------------------------ */
/* Keychain                                                           */
/* ------------------------------------------------------------------ */

static CFMutableDictionaryRef keychain_query(const char *account)
{
	CFMutableDictionaryRef q = CFDictionaryCreateMutable(
		NULL, 0, &kCFTypeDictionaryKeyCallBacks,
		&kCFTypeDictionaryValueCallBacks);
	CFStringRef service = CFStringCreateWithCString(
		NULL, KICK_KEYCHAIN_SERVICE, kCFStringEncodingUTF8);
	CFStringRef acct =
		CFStringCreateWithCString(NULL, account, kCFStringEncodingUTF8);

	CFDictionarySetValue(q, kSecClass, kSecClassGenericPassword);
	CFDictionarySetValue(q, kSecAttrService, service);
	CFDictionarySetValue(q, kSecAttrAccount, acct);

	CFRelease(service);
	CFRelease(acct);
	return q;
}

char *kick_keychain_get(const char *account)
{
	CFMutableDictionaryRef q = keychain_query(account);
	CFDictionarySetValue(q, kSecReturnData, kCFBooleanTrue);
	CFDictionarySetValue(q, kSecMatchLimit, kSecMatchLimitOne);

	CFTypeRef data = NULL;
	const OSStatus st = SecItemCopyMatching(q, &data);
	CFRelease(q);

	if (st != errSecSuccess) {
		if (st != errSecItemNotFound)
			blog(LOG_WARNING,
			     "[obs-icecast] kick: keychain read of '%s' "
			     "failed (OSStatus %d)",
			     account, (int)st);
		return NULL;
	}

	const CFIndex len = CFDataGetLength(data);
	char *out = bmalloc((size_t)len + 1);
	memcpy(out, CFDataGetBytePtr(data), (size_t)len);
	out[len] = '\0';
	CFRelease(data);
	return out;
}

bool kick_keychain_set(const char *account, const char *value)
{
	CFDataRef data = CFDataCreate(NULL, (const UInt8 *)value,
				      (CFIndex)strlen(value));
	CFMutableDictionaryRef q = keychain_query(account);

	CFMutableDictionaryRef change = CFDictionaryCreateMutable(
		NULL, 0, &kCFTypeDictionaryKeyCallBacks,
		&kCFTypeDictionaryValueCallBacks);
	CFDictionarySetValue(change, kSecValueData, data);

	OSStatus st = SecItemUpdate(q, change);
	if (st == errSecItemNotFound) {
		CFStringRef label = CFStringCreateWithFormat(
			NULL, NULL, CFSTR("obs-icecast Kick %s"), account);
		CFDictionarySetValue(q, kSecValueData, data);
		CFDictionarySetValue(q, kSecAttrLabel, label);
		st = SecItemAdd(q, NULL);
		CFRelease(label);
	}

	CFRelease(change);
	CFRelease(q);
	CFRelease(data);

	if (st != errSecSuccess)
		blog(LOG_ERROR,
		     "[obs-icecast] kick: keychain write of '%s' failed "
		     "(OSStatus %d)",
		     account, (int)st);
	return st == errSecSuccess;
}

bool kick_keychain_has(const char *account)
{
	/* Attributes only: no secret is read, so this never asks for access. */
	CFMutableDictionaryRef q = keychain_query(account);
	CFDictionarySetValue(q, kSecReturnAttributes, kCFBooleanTrue);
	CFDictionarySetValue(q, kSecMatchLimit, kSecMatchLimitOne);

	CFTypeRef attrs = NULL;
	const OSStatus st = SecItemCopyMatching(q, &attrs);
	CFRelease(q);
	if (attrs)
		CFRelease(attrs);
	return st == errSecSuccess;
}

void kick_keychain_delete(const char *account)
{
	CFMutableDictionaryRef q = keychain_query(account);
	const OSStatus st = SecItemDelete(q);
	CFRelease(q);

	if (st != errSecSuccess && st != errSecItemNotFound)
		blog(LOG_WARNING,
		     "[obs-icecast] kick: keychain delete of '%s' failed "
		     "(OSStatus %d)",
		     account, (int)st);
}
