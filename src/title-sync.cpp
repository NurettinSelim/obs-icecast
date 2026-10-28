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

#include "title-sync.h"

#include <obs.h>
#include <util/platform.h>
#include <util/threading.h>

extern "C" {
#include "icy-protocol.h"
}
#include "kick-api.h"

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <functional>

/* How long the browser has to come back from id.kick.com. */
static constexpr int KICK_SIGNIN_TIMEOUT_SEC = 300;

/* Refresh this long before Kick's stated expiry, not at it. */
static constexpr long long KICK_EXPIRY_MARGIN_SEC = 60;

static long long now_sec()
{
	return (long long)(os_gettime_ns() / 1000000000ULL);
}

/* ------------------------------------------------------------------ */

enum class Outcome { Ok, Retry, Fail, NeedsSignIn, Drop };

struct TitleSync::Attempt {
	Outcome outcome;
	std::string detail;
};

/*
 * One destination's worker. Holds at most one pending title — a newer post
 * replaces it, and also cuts short a retry wait for an older one, so the
 * last title typed is the one that lands.
 */
class TitleLane {
public:
	TitleLane(const char *name,
		  std::function<Outcome(const std::string &, std::string &)> send,
		  std::function<bool()> wanted,
		  std::function<void(TitleState, int, const std::string &)> report)
		: name(name),
		  send(std::move(send)),
		  wanted(std::move(wanted)),
		  report(std::move(report)),
		  worker([this] { run(); })
	{
	}

	~TitleLane() { stop(); }

	void post(std::string title)
	{
		{
			std::lock_guard<std::mutex> lock(m);
			pending = std::move(title);
			hasPending = true;
		}
		cv.notify_one();
	}

	void stop()
	{
		{
			std::lock_guard<std::mutex> lock(m);
			quit = true;
		}
		cv.notify_one();
		if (worker.joinable())
			worker.join();
	}

private:
	void run()
	{
		static const int delays[] = {5, 10, 20};
		constexpr int maxRetries = sizeof(delays) / sizeof(delays[0]);

		std::unique_lock<std::mutex> lock(m);
		for (;;) {
			cv.wait(lock, [this] { return quit || hasPending; });
			if (quit)
				return;

			std::string title = std::move(pending);
			hasPending = false;

			for (int attempt = 0;; attempt++) {
				lock.unlock();
				Outcome out = Outcome::Drop;
				std::string detail;
				if (wanted()) {
					report(attempt ? TitleState::Retrying
						       : TitleState::Sending,
					       attempt, std::string());
					out = send(title, detail);
				}
				lock.lock();
				if (quit)
					return;

				if (out == Outcome::Retry && attempt < maxRetries &&
				    !hasPending) {
					blog(LOG_WARNING,
					     "[obs-icecast] %s title not sent "
					     "(%s); retry %d/%d in %d s",
					     name, detail.c_str(), attempt + 1,
					     maxRetries, delays[attempt]);
					report(TitleState::Retrying, attempt + 1,
					       detail);
					cv.wait_for(lock,
						    std::chrono::seconds(
							    delays[attempt]),
						    [this] {
							    return quit ||
								   hasPending;
						    });
					if (quit)
						return;
					if (hasPending)
						break;
					continue;
				}

				/* A newer title is waiting; it supersedes this
				 * one's failure as well as its retries. */
				if (out != Outcome::Ok && hasPending)
					break;

				switch (out) {
				case Outcome::Ok:
					report(TitleState::Done, attempt, detail);
					break;
				case Outcome::Drop:
					blog(LOG_INFO,
					     "[obs-icecast] %s title not sent: "
					     "off air",
					     name);
					report(TitleState::Dropped, attempt,
					       detail);
					break;
				case Outcome::NeedsSignIn:
					report(TitleState::NeedsSignIn, attempt,
					       detail);
					break;
				default:
					blog(LOG_WARNING,
					     "[obs-icecast] %s title failed: %s",
					     name, detail.c_str());
					report(TitleState::Failed, attempt,
					       detail);
					break;
				}
				break;
			}
		}
	}

	const char *name;
	std::function<Outcome(const std::string &, std::string &)> send;
	std::function<bool()> wanted;
	std::function<void(TitleState, int, const std::string &)> report;

	std::mutex m;
	std::condition_variable cv;
	std::string pending;
	bool hasPending = false;
	bool quit = false;

	/* Last member: the thread must start after everything above. */
	std::thread worker;
};

/* ------------------------------------------------------------------ */

TitleSync::TitleSync(QObject *parent) : QObject(parent)
{
	radioLane = std::make_unique<TitleLane>(
		"radio.co",
		[this](const std::string &t, std::string &detail) {
			Attempt a = radioAttempt(t);
			detail = a.detail;
			return a.outcome;
		},
		[this] { return radioLive.load(); },
		[this](TitleState s, int n, const std::string &d) {
			report(TitleDest::Radio, s, n, d);
		});

	/* Kick takes a title whether or not the channel is live. */
	kickLane = std::make_unique<TitleLane>(
		"Kick",
		[this](const std::string &t, std::string &detail) {
			Attempt a = kickAttempt(t);
			detail = a.detail;
			return a.outcome;
		},
		[] { return true; },
		[this](TitleState s, int n, const std::string &d) {
			report(TitleDest::Kick, s, n, d);
		});
}

TitleSync::~TitleSync()
{
	shutdown();
}

void TitleSync::shutdown()
{
	if (stopped)
		return;
	stopped = true;

	os_atomic_set_bool(&requestAbort, true);
	cancelKickConnect();
	radioLane->stop();
	kickLane->stop();
}

/*
 * Signals are emitted from worker threads; the dock lives on the UI thread,
 * so Qt queues them there.
 */
void TitleSync::report(TitleDest dest, TitleState state, int attempt,
		       const std::string &detail)
{
	emit titleStatus((int)dest, (int)state, attempt,
			 QString::fromStdString(detail));
}

/* ------------------------------------------------------------------ */
/* radio.co                                                            */
/* ------------------------------------------------------------------ */

void TitleSync::sendRadio(const QString &title, const RadioTarget &target)
{
	if (stopped)
		return;
	{
		std::lock_guard<std::mutex> lock(radioMutex);
		radioTarget = target;
	}
	radioLane->post(title.toStdString());
}

void TitleSync::setRadioLive(bool live)
{
	radioLive = live;
}

TitleSync::Attempt TitleSync::radioAttempt(const std::string &title)
{
	RadioTarget t;
	{
		std::lock_guard<std::mutex> lock(radioMutex);
		t = radioTarget;
	}

	icy_params params = {};
	params.server = t.server.c_str();
	params.port = t.port;
	params.password = t.password.c_str();
	params.username = t.username.c_str();
	params.mount = t.mount.c_str();
	params.protocol = (enum stream_protocol)t.protocol;

	char error[512] = {0};
	const int rc = icy_update_metadata(&params, title.c_str(), error,
					   sizeof(error));
	if (rc == 0) {
		blog(LOG_INFO, "[icy] metadata updated: %s", title.c_str());
		return {Outcome::Ok, std::string()};
	}
	/* -1: no answer, worth another try. -2: the server said no. */
	return {rc == -1 ? Outcome::Retry : Outcome::Fail, error};
}

/* ------------------------------------------------------------------ */
/* Kick                                                                */
/* ------------------------------------------------------------------ */

void TitleSync::sendKick(const QString &title)
{
	if (stopped)
		return;
	kickLane->post(title.toStdString());
}

void TitleSync::setKickClientId(const QString &clientId)
{
	std::lock_guard<std::mutex> lock(stateMutex);
	kickClientId = clientId.trimmed().toStdString();
}

bool TitleSync::kickHasSecret() const
{
	return kick_keychain_has(KICK_KEY_CLIENT_SECRET);
}

bool TitleSync::setKickSecret(const QString &secret)
{
	const QByteArray utf8 = secret.trimmed().toUtf8();
	if (utf8.isEmpty())
		return false;
	const bool ok = kick_keychain_set(KICK_KEY_CLIENT_SECRET,
					  utf8.constData());
	std::lock_guard<std::mutex> lock(stateMutex);
	kickSecretDirty = true;
	return ok;
}

void TitleSync::disconnectKick()
{
	cancelKickConnect();
	kick_keychain_delete(KICK_KEY_TOKENS);
	std::lock_guard<std::mutex> lock(stateMutex);
	kickTokensDirty = true;
	blog(LOG_INFO, "[obs-icecast] kick: disconnected, tokens removed");
}

/*
 * Caller holds tokenMutex. Re-reads the keychain when the UI changed it
 * (new secret, disconnect, fresh sign-in); otherwise uses the cache.
 */
bool TitleSync::kickLoadLocked()
{
	bool tokensDirty, secretDirty;
	{
		std::lock_guard<std::mutex> lock(stateMutex);
		tokensDirty = kickTokensDirty;
		secretDirty = kickSecretDirty;
		kickTokensDirty = kickSecretDirty = false;
	}

	if (secretDirty) {
		char *s = kick_keychain_get(KICK_KEY_CLIENT_SECRET);
		clientSecret = s ? s : "";
		if (s) {
			memset(s, 0, strlen(s));
			bfree(s);
		}
	}

	if (tokensDirty) {
		accessToken.clear();
		refreshToken.clear();
		accessExpiresAt = 0;

		char *json = kick_keychain_get(KICK_KEY_TOKENS);
		obs_data_t *d = json ? obs_data_create_from_json(json)
				     : nullptr;
		if (d) {
			accessToken = obs_data_get_string(d, "access_token");
			refreshToken = obs_data_get_string(d, "refresh_token");
			accessExpiresAt = obs_data_get_int(d, "expires_at");
			obs_data_release(d);
		}
		if (json) {
			memset(json, 0, strlen(json));
			bfree(json);
		}
	}

	return !refreshToken.empty();
}

/* Caller holds tokenMutex. */
void TitleSync::kickStoreLocked()
{
	obs_data_t *d = obs_data_create();
	obs_data_set_string(d, "access_token", accessToken.c_str());
	obs_data_set_string(d, "refresh_token", refreshToken.c_str());
	obs_data_set_int(d, "expires_at", accessExpiresAt);
	std::string json = obs_data_get_json(d);
	obs_data_release(d);

	/* Kept in memory either way, so this session still works. */
	if (!kick_keychain_set(KICK_KEY_TOKENS, json.c_str()))
		blog(LOG_ERROR, "[obs-icecast] kick: new tokens could not be "
				"saved; sign in again after restarting OBS");
	std::fill(json.begin(), json.end(), '\0');
}

/* Caller holds tokenMutex and has loaded the cache. */
TitleSync::Attempt TitleSync::kickRefreshLocked()
{
	std::string clientId;
	{
		std::lock_guard<std::mutex> lock(stateMutex);
		clientId = kickClientId;
	}
	if (clientId.empty() || clientSecret.empty())
		return {Outcome::NeedsSignIn, "Client ID or secret missing"};

	kick_response resp;
	kick_token_from_refresh(clientId.c_str(), clientSecret.c_str(),
				refreshToken.c_str(), &requestAbort, &resp);

	char *access = nullptr, *refresh = nullptr;
	long long expiresIn = 0;
	const bool ok = resp.status == 200 &&
			kick_parse_tokens(resp.body, &access, &refresh,
					  &expiresIn);
	Attempt result{Outcome::Ok, std::string()};

	if (ok) {
		/* The UI may have disconnected while the request ran. */
		bool dirty;
		{
			std::lock_guard<std::mutex> lock(stateMutex);
			dirty = kickTokensDirty;
		}
		if (dirty) {
			result = {Outcome::NeedsSignIn, "disconnected"};
		} else {
			accessToken = access;
			refreshToken = refresh;
			accessExpiresAt = now_sec() + expiresIn;
			kickStoreLocked();
			blog(LOG_INFO,
			     "[obs-icecast] kick: token refreshed (valid %lld s)",
			     expiresIn);
		}
	} else {
		char *why = kick_response_reason(&resp);
		if (resp.status == 0 || resp.status >= 500) {
			result = {Outcome::Retry, why};
		} else {
			/* 400/401: the refresh token is spent or revoked. */
			blog(LOG_WARNING,
			     "[obs-icecast] kick: token refresh refused (%s); "
			     "press Connect Kick again",
			     why);
			result = {Outcome::NeedsSignIn, why};
		}
		bfree(why);
	}

	if (access) {
		memset(access, 0, strlen(access));
		bfree(access);
	}
	if (refresh) {
		memset(refresh, 0, strlen(refresh));
		bfree(refresh);
	}
	kick_response_free(&resp);
	return result;
}

TitleSync::Attempt TitleSync::kickAttempt(const std::string &title)
{
	std::lock_guard<std::mutex> lock(tokenMutex);

	if (!kickLoadLocked())
		return {Outcome::NeedsSignIn, "not connected"};

	if (accessToken.empty() ||
	    now_sec() >= accessExpiresAt - KICK_EXPIRY_MARGIN_SEC) {
		Attempt a = kickRefreshLocked();
		if (a.outcome != Outcome::Ok)
			return a;
	}

	kick_response resp;
	for (int pass = 0;; pass++) {
		kick_patch_title(accessToken.c_str(), title.c_str(),
				 &requestAbort, &resp);

		/* An access token can die early (revoked, clock skew):
		 * refresh once and repeat. */
		if (resp.status != 401 || pass > 0)
			break;
		kick_response_free(&resp);
		Attempt a = kickRefreshLocked();
		if (a.outcome != Outcome::Ok)
			return a;
	}

	Attempt result{Outcome::Ok, std::string()};
	if (resp.status == 200 || resp.status == 204) {
		blog(LOG_INFO, "[obs-icecast] kick: title set: %s",
		     title.c_str());
	} else {
		char *why = kick_response_reason(&resp);
		if (resp.status == 0 || resp.status >= 500)
			result = {Outcome::Retry, why};
		else if (resp.status == 401)
			result = {Outcome::NeedsSignIn, why};
		else
			result = {Outcome::Fail, why};
		bfree(why);
	}
	kick_response_free(&resp);

	/* Read the title back so the log shows what viewers actually see. */
	if (result.outcome == Outcome::Ok) {
		kick_get_channel(accessToken.c_str(), &requestAbort, &resp);
		char *slug = nullptr, *now = nullptr;
		if (resp.status == 200 &&
		    kick_parse_channel(resp.body, &slug, &now))
			blog(LOG_INFO,
			     "[obs-icecast] kick: %s now shows: %s", slug,
			     now ? now : "");
		bfree(slug);
		bfree(now);
		kick_response_free(&resp);
	}
	return result;
}

/* ------------------------------------------------------------------ */
/* Connect Kick                                                        */
/* ------------------------------------------------------------------ */

bool TitleSync::beginKickConnect(QString &url, QString &error)
{
	if (stopped)
		return false;
	cancelKickConnect();

	std::string clientId;
	{
		std::lock_guard<std::mutex> lock(stateMutex);
		clientId = kickClientId;
	}
	if (clientId.empty()) {
		error = QStringLiteral("Enter the Client ID first");
		return false;
	}
	if (!kick_keychain_has(KICK_KEY_CLIENT_SECRET)) {
		error = QStringLiteral("Enter the Client secret first");
		return false;
	}

	char err[256] = {0};
	kick_auth *auth = kick_auth_begin(clientId.c_str(), err, sizeof(err));
	if (!auth) {
		error = QString::fromUtf8(err);
		return false;
	}

	url = QString::fromUtf8(kick_auth_url(auth));
	os_atomic_set_bool(&authAbort, false);
	authThread = std::thread([this, auth, clientId] {
		runKickConnect(auth, clientId);
	});
	blog(LOG_INFO, "[obs-icecast] kick: waiting for the browser sign-in "
		       "on " KICK_REDIRECT_URI);
	return true;
}

void TitleSync::cancelKickConnect()
{
	os_atomic_set_bool(&authAbort, true);
	if (authThread.joinable())
		authThread.join();
}

void TitleSync::runKickConnect(kick_auth *auth, std::string clientId)
{
	char err[256] = {0};
	char *code = kick_auth_wait(auth, KICK_SIGNIN_TIMEOUT_SEC, &authAbort,
				    err, sizeof(err));
	const std::string verifier = kick_auth_verifier(auth);
	kick_auth_free(auth);

	if (!code) {
		if (!os_atomic_load_bool(&authAbort)) {
			blog(LOG_WARNING, "[obs-icecast] kick: sign-in failed: %s",
			     err);
			emit kickConnectFinished(false, QString::fromUtf8(err));
		}
		return;
	}

	std::lock_guard<std::mutex> lock(tokenMutex);
	{
		std::lock_guard<std::mutex> state(stateMutex);
		kickSecretDirty = true;
		kickTokensDirty = true;
	}
	kickLoadLocked();

	kick_response resp;
	kick_token_from_code(clientId.c_str(), clientSecret.c_str(), code,
			     verifier.c_str(), &authAbort, &resp);
	memset(code, 0, strlen(code));
	bfree(code);

	char *access = nullptr, *refresh = nullptr;
	long long expiresIn = 0;
	if (resp.status != 200 ||
	    !kick_parse_tokens(resp.body, &access, &refresh, &expiresIn)) {
		char *why = kick_response_reason(&resp);
		kick_response_free(&resp);
		if (!os_atomic_load_bool(&authAbort)) {
			blog(LOG_WARNING,
			     "[obs-icecast] kick: token exchange refused: %s",
			     why);
			emit kickConnectFinished(
				false, QStringLiteral("Kick refused the sign-in "
						      "(%1)")
					       .arg(QString::fromUtf8(why)));
		}
		bfree(why);
		return;
	}
	kick_response_free(&resp);

	/* Disconnect or a second Connect Kick won the race: keep nothing. */
	if (os_atomic_load_bool(&authAbort)) {
		memset(access, 0, strlen(access));
		memset(refresh, 0, strlen(refresh));
		bfree(access);
		bfree(refresh);
		return;
	}

	accessToken = access;
	refreshToken = refresh;
	accessExpiresAt = now_sec() + expiresIn;
	memset(access, 0, strlen(access));
	memset(refresh, 0, strlen(refresh));
	bfree(access);
	bfree(refresh);
	kickStoreLocked();

	/* The slug confirms which account signed in — radyozu, not a
	 * personal account the browser happened to be logged into. */
	QString slug = QStringLiteral("?");
	kick_get_channel(accessToken.c_str(), &authAbort, &resp);
	char *s = nullptr, *title = nullptr;
	if (resp.status == 200 && kick_parse_channel(resp.body, &s, &title))
		slug = QString::fromUtf8(s);
	bfree(s);
	bfree(title);
	kick_response_free(&resp);

	blog(LOG_INFO, "[obs-icecast] kick: connected as %s",
	     slug.toUtf8().constData());
	emit kickConnectFinished(true, slug);
}
