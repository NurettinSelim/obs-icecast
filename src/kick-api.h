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

#pragma once

#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Kick's public API, as far as setting a stream title needs it: the OAuth 2.1
 * authorization-code flow with PKCE (id.kick.com), token refresh, and
 * PATCH /public/v1/channels. Stateless like icy-protocol.c — no Qt, no
 * threads of its own. Every call blocks; the caller runs it off the UI thread.
 *
 * The redirect URI must match the Kick app's setting byte for byte. Kick's
 * docs ask for "localhost", not 127.0.0.1: their Next.js frontend rewrites
 * the first 127.0.0.1 in the authorize URL.
 */
#define KICK_REDIRECT_PORT 51934
#define KICK_REDIRECT_URI "http://localhost:51934/callback"
#define KICK_SCOPES "channel:read channel:write"

/* Keychain accounts under the plugin's Kick service. */
#define KICK_KEY_CLIENT_SECRET "client_secret"
#define KICK_KEY_TOKENS "tokens"

/*
 * One HTTPS exchange. status is 0 when no HTTP answer arrived at all (DNS,
 * connect, TLS, timeout, abort) — the case worth retrying. body is the raw
 * response, NULL if empty; free the struct with kick_response_free.
 */
struct kick_response {
	long status;
	bool aborted;
	char *body;
	char error[256];
};

void kick_response_free(struct kick_response *resp);

/*
 * A short human-readable reason from a failed response: Kick's "message",
 * OAuth's "error_description"/"error", the transport error, or the status.
 * Returned with bstrdup; bfree it.
 */
char *kick_response_reason(const struct kick_response *resp);

/* Process-wide libcurl setup; called from obs_module_load / _unload. */
void kick_global_init(void);
void kick_global_free(void);

/*
 * abort is polled while a request is in flight; once it reads true the
 * transfer stops within about a second. It may be NULL.
 */
void kick_token_from_code(const char *client_id, const char *client_secret,
			  const char *code, const char *code_verifier,
			  const volatile bool *abort,
			  struct kick_response *resp);
void kick_token_from_refresh(const char *client_id, const char *client_secret,
			     const char *refresh_token,
			     const volatile bool *abort,
			     struct kick_response *resp);
void kick_patch_title(const char *access_token, const char *title,
		      const volatile bool *abort, struct kick_response *resp);
/* The token owner's own channel (no query parameters). */
void kick_get_channel(const char *access_token, const volatile bool *abort,
		      struct kick_response *resp);

/*
 * Token endpoint body -> fields. expires_in is seconds from now. Returns
 * false unless both tokens are present. Strings are bstrdup'd.
 */
bool kick_parse_tokens(const char *body, char **access_token,
		       char **refresh_token, long long *expires_in);

/* GET channel body -> slug and stream_title of the first entry. */
bool kick_parse_channel(const char *body, char **slug, char **stream_title);

/*
 * One browser sign-in. kick_auth_begin binds the loopback listener on
 * KICK_REDIRECT_PORT and prepares PKCE and state, so a busy port is reported
 * before the browser opens. kick_auth_wait then serves the redirect and
 * returns the authorization code (bfree it), or NULL with error set — on
 * timeout, on abort (error left empty), or when Kick reports a denial.
 */
struct kick_auth;

struct kick_auth *kick_auth_begin(const char *client_id, char *error,
				  size_t error_len);
const char *kick_auth_url(const struct kick_auth *auth);
const char *kick_auth_verifier(const struct kick_auth *auth);
char *kick_auth_wait(struct kick_auth *auth, int timeout_sec,
		     const volatile bool *abort, char *error,
		     size_t error_len);
void kick_auth_free(struct kick_auth *auth);

/*
 * Generic passwords in the login keychain, service
 * "io.github.nurettinselim.obs-icecast.kick". The item's access list trusts
 * the app that created it — OBS — so OBS reads them without a prompt and
 * any other app must ask. kick_keychain_get returns a bmalloc'd copy or NULL.
 */
char *kick_keychain_get(const char *account);
bool kick_keychain_set(const char *account, const char *value);
bool kick_keychain_has(const char *account);
void kick_keychain_delete(const char *account);

#ifdef __cplusplus
}
#endif
