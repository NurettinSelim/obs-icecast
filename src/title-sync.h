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

#include <QObject>
#include <QString>

#include <atomic>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

/* Where a title goes. Values travel through Qt signals as int. */
enum class TitleDest { Radio = 0, Kick = 1 };

/* What happened to the latest title on one destination. */
enum class TitleState {
	Sending = 0,
	Done,
	Retrying,    /* attempt failed for lack of an answer; waiting */
	Failed,      /* gave up, or the server refused it outright */
	NeedsSignIn, /* Kick only: the stored sign-in no longer works */
	Dropped,     /* radio went off air before the title could land */
};

/* The radio.co endpoint, snapshotted when the output connects. */
struct RadioTarget {
	std::string server;
	std::string username;
	std::string password;
	std::string mount;
	int port = 0;
	int protocol = 0;
};

class TitleLane;
struct kick_auth;

/*
 * Pushes the dock's Stream name to radio.co (ICY metadata) and to Kick
 * (PATCH /public/v1/channels), each on its own worker thread so neither ever
 * blocks the UI or the other. A destination holds at most one pending title
 * and the newest wins. When an attempt gets no answer, it is retried after
 * 5, 10 and 20 s, then reported as failed; a refusal is reported at once.
 *
 * Kick secrets — the app's client secret and the OAuth tokens — live only in
 * the login keychain (kick-api.h). Tokens are refreshed on demand and the
 * rotated refresh token is written back after every refresh.
 */
class TitleSync : public QObject {
	Q_OBJECT

public:
	explicit TitleSync(QObject *parent = nullptr);
	~TitleSync() override;

	void sendRadio(const QString &title, const RadioTarget &target);
	/* Radio retries stop as soon as this goes false. */
	void setRadioLive(bool live);
	void sendKick(const QString &title);

	void setKickClientId(const QString &clientId);
	bool kickHasSecret() const;
	bool setKickSecret(const QString &secret);
	/* Opens the redirect listener; on success url is what to open. */
	bool beginKickConnect(QString &url, QString &error);
	void disconnectKick();

	/* Stops and joins every thread. Idempotent; call before teardown. */
	void shutdown();

signals:
	/* dest is a TitleDest, state a TitleState. */
	void titleStatus(int dest, int state, int attempt,
			 const QString &detail);
	/* ok: text is the connected channel's slug; else the reason. */
	void kickConnectFinished(bool ok, const QString &text);

private:
	struct Attempt;
	Attempt radioAttempt(const std::string &title);
	Attempt kickAttempt(const std::string &title);
	Attempt kickRefreshLocked();
	bool kickLoadLocked();
	void kickStoreLocked();
	void runKickConnect(kick_auth *auth, std::string clientId);
	void cancelKickConnect();
	void report(TitleDest dest, TitleState state, int attempt,
		    const std::string &detail);

	std::unique_ptr<TitleLane> radioLane;
	std::unique_ptr<TitleLane> kickLane;

	std::mutex radioMutex;
	RadioTarget radioTarget;
	std::atomic<bool> radioLive{false};

	/* Set from the UI thread; never held across a network call. */
	mutable std::mutex stateMutex;
	std::string kickClientId;
	bool kickTokensDirty = true; /* keychain changed under the cache */
	bool kickSecretDirty = true;

	/* Held by workers across token refresh and use, so a rotated
	 * refresh token is never used twice. Never taken by the UI thread. */
	std::mutex tokenMutex;
	std::string accessToken;
	std::string refreshToken;
	long long accessExpiresAt = 0; /* os_gettime_ns() / 1e9 scale */
	std::string clientSecret;

	std::thread authThread;
	volatile bool authAbort = false;
	volatile bool requestAbort = false;
	bool stopped = false;
};
