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

#include "radioco-dock.h"
#include "title-sync.h"
#include "kick-api.h"

#include <obs.h>
#include <obs-module.h>
#include <obs-frontend-api.h>
#include <util/platform.h>
#include <util/config-file.h>

#include <QWidget>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QVBoxLayout>
#include <QComboBox>
#include <QLineEdit>
#include <QSpinBox>
#include <QCheckBox>
#include <QPushButton>
#include <QLabel>
#include <QStringList>
#include <QDialog>
#include <QDialogButtonBox>
#include <QGroupBox>
#include <QDesktopServices>
#include <QUrl>
#include <QTime>
#include <QTimer>
#include <QElapsedTimer>
#include <QMetaObject>
#include <QPointer>
#include <QIcon>
#include <QPixmap>
#include <QPainter>
#include <QPainterPath>
#include <QTransform>

#define DOCK_ID "radioco_dock"
#define DOCK_TITLE "Radio.co"

/* Status colors, shared by the connection line and the title line. */
#define COLOR_BUSY "#e6a817"
#define COLOR_LIVE "#27ae60"
#define COLOR_ERROR "#e05a4e"

/*
 * radio.co accepts a title only once the station has switched to the live
 * source; sent the instant the handshake completes, it returns 200 and is
 * silently dropped. 15 s is comfortably past that switch.
 */
#define RADIO_TITLE_DELAY_MS 15000

/* ------------------------------------------------------------------ */

/*
 * The gear is painted, not typed: U+2699 renders as an empty box under the
 * OBS theme's font, and the plugin ships no icon assets. Three rounded bars
 * through the centre make six teeth — few and chunky so the shape still
 * reads as a gear at 16 px; a disc joins them and the hub is cut out.
 * Drawn at 64 px and scaled down by QIcon, so it stays sharp on Retina.
 */
static QIcon make_gear_icon(const QColor &color)
{
	const qreal s = 64.0;
	const qreal c = s / 2.0;

	QPainterPath path;
	for (int i = 0; i < 3; i++) {
		QPainterPath tooth;
		tooth.addRoundedRect(QRectF(-s * 0.15, -s * 0.46, s * 0.30,
					    s * 0.92),
				     s * 0.05, s * 0.05);
		QTransform t;
		t.translate(c, c);
		t.rotate(i * 60.0);
		path.addPath(t.map(tooth));
	}
	path.addEllipse(QPointF(c, c), s * 0.32, s * 0.32);
	path = path.simplified();

	QPainterPath hub;
	hub.addEllipse(QPointF(c, c), s * 0.175, s * 0.175);
	path = path.subtracted(hub);

	QPixmap pm((int)s, (int)s);
	pm.fill(Qt::transparent);
	QPainter p(&pm);
	p.setRenderHint(QPainter::Antialiasing);
	p.fillPath(path, color);
	p.end();

	return QIcon(pm);
}

/* ------------------------------------------------------------------ */

/*
 * What is actually on an OBS audio track, as libobs sees it. A source feeds
 * mix N only if it is assigned to N, active, unmuted and not monitor-only —
 * the same four tests obs-audio.c applies when it builds the mix.
 */
struct track_scan {
	int track;         /* 0-based mixer index */
	bool log;          /* also write per-source lines to the OBS log */
	int on_air;
	QStringList names; /* contributing source names, enumeration order */
};

static bool scan_track_source(void *param, obs_source_t *src)
{
	auto *scan = static_cast<track_scan *>(param);

	/* obs_enum_sources also yields group sources; groups carry no audio
	 * flag, so this test drops them. Do not "fix" it into a type check. */
	if (!(obs_source_get_output_flags(src) & OBS_SOURCE_AUDIO))
		return true;

	const char *name = obs_source_get_name(src);

	/*
	 * Four reasons a source contributes nothing, checked in this order —
	 * first hit wins. Track assignment comes first because that is what
	 * the combo above this label controls.
	 */
	const char *reason = nullptr;
	if (!(obs_source_get_audio_mixers(src) & (1u << scan->track)))
		reason = "not on this track";
	else if (!obs_source_active(src))
		reason = "not in the active scene";
	else if (obs_source_muted(src))
		reason = "muted";
	else if (obs_source_get_monitoring_type(src) ==
		 OBS_MONITORING_TYPE_MONITOR_ONLY)
		reason = "monitor only";

	if (!reason) {
		scan->on_air++;
		scan->names << QString::fromUtf8(name ? name : "");
	}

	if (scan->log)
		blog(LOG_INFO, "[obs-icecast] track %d: '%s' %s",
		     scan->track + 1, name ? name : "(unnamed)",
		     reason ? reason : "-> on air");

	return true;
}

/*
 * True once OBS_FRONTEND_EVENT_FINISHED_LOADING has fired.
 *
 * obs_frontend_get_streaming_output() is NOT safe before that, despite the
 * frontend API's null-callbacks guard: the callback itself dereferences
 * main->outputHandler unconditionally (frontend OBSStudioAPI.cpp:402) and
 * that unique_ptr is still null while modules are loading. Calling it from
 * this dock's constructor — which runs in obs_module_post_load — segfaults
 * OBS at startup with KERN_INVALID_ADDRESS 0x38.
 */
static bool frontend_ready = false;

/*
 * Which mix OBS's own stream output encodes — "what the video platform hears".
 *
 * Simple output mode hardcodes mixer 0 (frontend SimpleOutput.cpp:200,202);
 * Advanced mode reads AdvOut/TrackIndex, 1-based (AdvancedOutput.cpp:184).
 * A live streaming output is authoritative over both, so ask it first — that
 * also covers services that impose their own track.
 */
static int obs_stream_mixer_index(void)
{
	if (!frontend_ready)
		return 0;

	obs_output_t *out = obs_frontend_get_streaming_output();
	if (out) {
		obs_encoder_t *enc = obs_output_get_audio_encoder(out, 0);
		const int idx = enc ? (int)obs_encoder_get_mixer_index(enc)
				    : -1;
		obs_output_release(out);
		if (idx >= 0 && idx < MAX_AUDIO_MIXES)
			return idx;
	}

	config_t *cfg = obs_frontend_get_profile_config();
	if (cfg) {
		const char *mode = config_get_string(cfg, "Output", "Mode");
		if (mode && strcmp(mode, "Advanced") == 0) {
			const int idx =
				(int)config_get_int(cfg, "AdvOut",
						    "TrackIndex") - 1;
			if (idx >= 0 && idx < MAX_AUDIO_MIXES)
				return idx;
		}
	}

	return 0;
}

/* ------------------------------------------------------------------ */

class RadioCoDock : public QWidget {
	Q_OBJECT

public:
	explicit RadioCoDock(QWidget *parent = nullptr);
	~RadioCoDock() override;

	void handleFrontendEvent(obs_frontend_event event);
	void shutdownOutput();

public slots:
	void onOutputStarted();
	void onOutputStopped(int code);
	void onOutputReconnecting();
	void onOutputReconnected();
	void onHotkeyConnect();
	void onHotkeyDisconnect();
	void onTitleStatus(int dest, int state, int attempt,
			   const QString &detail);
	void onKickConnectFinished(bool ok, const QString &text);

private slots:
	void onConnectClicked();
	void onUpdateClicked();
	void onApplyNameClicked();
	void onTick();
	void onFieldChanged();
	void onSettingsClicked();
	void onTrackChanged();
	void onKickConnectClicked();
	void onKickDisconnectClicked();
	void onKickSecretEntered();

private:
	void startOutput(bool asAutoStart);
	void stopOutput();
	void releaseOutput();
	void buildSettingsDialog();
	void loadSettings();
	void saveSettings();
	enum class StatusKind { Idle, Busy, Live, Error };
	static QString styleFor(StatusKind kind);
	void setStatus(const QString &text, StatusKind kind);
	void setKickAccount(const QString &text, StatusKind kind);
	bool kickEnabled() const;
	void sendRadioTitle(const char *why);
	void scheduleRadioTitle();
	void renderTitleStatus();
	RadioTarget currentTarget() const;
	void refreshControls();
	void updateTrackStatus();
	int selectedMixerIndex() const;
	obs_data_t *buildOutputSettings() const;

	QComboBox *protocolBox = nullptr;
	QLineEdit *serverEdit = nullptr;
	QSpinBox *portSpin = nullptr;
	QLineEdit *usernameEdit = nullptr;
	QLineEdit *mountEdit = nullptr;
	QLineEdit *passwordEdit = nullptr;
	QLineEdit *stationEdit = nullptr;
	QPushButton *applyNameButton = nullptr;
	QComboBox *bitrateBox = nullptr;
	QSpinBox *reconnectRetriesSpin = nullptr;
	QSpinBox *reconnectDelaySpin = nullptr;
	QComboBox *trackBox = nullptr;
	QLabel *trackInfoLabel = nullptr;
	QLabel *silentWarnLabel = nullptr;
	QLineEdit *nowPlayingEdit = nullptr;
	QPushButton *updateButton = nullptr;
	QCheckBox *followObsBox = nullptr;
	QPushButton *connectButton = nullptr;
	QLabel *statusLabel = nullptr;
	QCheckBox *kickTitleBox = nullptr;
	QLabel *titleStatusLabel = nullptr;
	QLineEdit *kickClientIdEdit = nullptr;
	QLineEdit *kickSecretEdit = nullptr;
	QPushButton *kickConnectButton = nullptr;
	QPushButton *kickDisconnectButton = nullptr;
	QLabel *kickAccountLabel = nullptr;
	QDialog *settingsDialog = nullptr;
	QPushButton *settingsButton = nullptr;
	QTimer *tickTimer = nullptr;

	obs_output_t *output = nullptr;
	obs_encoder_t *encoder = nullptr;

	TitleSync *titles = nullptr;
	RadioTarget liveTarget;  /* the endpoint the live output connected to */
	int connectGeneration = 0; /* invalidates pending title timers */
	QString kickChannel;       /* slug of the signed-in Kick account */

	/* Last outcome per destination, for the title line. */
	struct TitleOutcome {
		int state = -1; /* a TitleState, -1 before the first send */
		int attempt = 0;
		QString detail;
		QTime at;
	};
	TitleOutcome radioOutcome, kickOutcome;

	obs_hotkey_id connectHotkey = OBS_INVALID_HOTKEY_ID;
	obs_hotkey_id disconnectHotkey = OBS_INVALID_HOTKEY_ID;

	QElapsedTimer liveTimer;
	QString connectedName;
	bool pendingRestart = false;
	bool autoStarted = false;
	int activeMixerIndex = -1;
	bool shuttingDown = false;
	bool loading = false;
};

/*
 * QPointer, not a raw pointer: obs_frontend_add_dock_by_id reparents this
 * widget into an OBSDock owned by the main window, so Qt destroys it during
 * frontend teardown — before obs_module_unload runs. A raw pointer would be
 * dangling by then and deleting it double-frees.
 */
static QPointer<RadioCoDock> dock;

/* ------------------------------------------------------------------ */
/* libobs signal callbacks — these fire on libobs threads and must not   */
/* touch widgets. Everything hops to the Qt main thread.                 */
/* ------------------------------------------------------------------ */

static void handle_start(void *data, calldata_t *cd)
{
	UNUSED_PARAMETER(cd);
	QMetaObject::invokeMethod(static_cast<RadioCoDock *>(data),
				  "onOutputStarted", Qt::QueuedConnection);
}

static void handle_stop(void *data, calldata_t *cd)
{
	long long code = 0;
	calldata_get_int(cd, "code", &code);
	QMetaObject::invokeMethod(static_cast<RadioCoDock *>(data),
				  "onOutputStopped", Qt::QueuedConnection,
				  Q_ARG(int, (int)code));
}

static void handle_reconnect(void *data, calldata_t *cd)
{
	UNUSED_PARAMETER(cd);
	QMetaObject::invokeMethod(static_cast<RadioCoDock *>(data),
				  "onOutputReconnecting", Qt::QueuedConnection);
}

static void handle_reconnect_success(void *data, calldata_t *cd)
{
	UNUSED_PARAMETER(cd);
	QMetaObject::invokeMethod(static_cast<RadioCoDock *>(data),
				  "onOutputReconnected", Qt::QueuedConnection);
}

/*
 * Hotkey callbacks run on libobs's hotkey thread. Separate Connect and
 * Disconnect keys rather than one toggle: a Stream Deck button pressed twice
 * by someone unsure whether the first press landed must not take the
 * broadcast off air.
 */
static void hotkey_connect(void *data, obs_hotkey_id id, obs_hotkey_t *key,
			   bool pressed)
{
	UNUSED_PARAMETER(id);
	UNUSED_PARAMETER(key);
	if (pressed)
		QMetaObject::invokeMethod(static_cast<RadioCoDock *>(data),
					  "onHotkeyConnect",
					  Qt::QueuedConnection);
}

static void hotkey_disconnect(void *data, obs_hotkey_id id, obs_hotkey_t *key,
			      bool pressed)
{
	UNUSED_PARAMETER(id);
	UNUSED_PARAMETER(key);
	if (pressed)
		QMetaObject::invokeMethod(static_cast<RadioCoDock *>(data),
					  "onHotkeyDisconnect",
					  Qt::QueuedConnection);
}

/* ------------------------------------------------------------------ */

/*
 * The station's connection details are set once and then never touched, so
 * they live in a dialog behind the gear button instead of the dock body.
 * The widgets stay RadioCoDock members and keep their onFieldChanged wiring —
 * only their parent moves — so load/save/buildOutputSettings are unaffected.
 * The dialog is a child of the dock, so Qt frees it with the dock; nothing
 * here is ever deleted by hand.
 */
void RadioCoDock::buildSettingsDialog()
{
	settingsDialog = new QDialog(this);
	settingsDialog->setObjectName(QStringLiteral("radioco_settings"));
	settingsDialog->setWindowTitle(QStringLiteral("Radio.co Settings"));

	auto *layout = new QVBoxLayout(settingsDialog);
	auto *form = new QFormLayout();
	form->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);

	protocolBox = new QComboBox(settingsDialog);
	protocolBox->addItem(QStringLiteral("Icecast (HTTP SOURCE)"), 0);
	protocolBox->addItem(QStringLiteral("SHOUTcast v1 (legacy ICY)"), 1);
	form->addRow(QStringLiteral("Protocol"), protocolBox);

	serverEdit = new QLineEdit(settingsDialog);
	form->addRow(QStringLiteral("Server"), serverEdit);

	portSpin = new QSpinBox(settingsDialog);
	portSpin->setRange(1, 65535);
	portSpin->setValue(80);
	form->addRow(QStringLiteral("Port"), portSpin);

	usernameEdit = new QLineEdit(settingsDialog);
	form->addRow(QStringLiteral("Username"), usernameEdit);

	mountEdit = new QLineEdit(settingsDialog);
	form->addRow(QStringLiteral("Mount"), mountEdit);

	passwordEdit = new QLineEdit(settingsDialog);
	passwordEdit->setEchoMode(QLineEdit::Password);
	form->addRow(QStringLiteral("Password"), passwordEdit);

	stationEdit = new QLineEdit(settingsDialog);
	applyNameButton =
		new QPushButton(QStringLiteral("Apply Name"), settingsDialog);
	applyNameButton->setEnabled(false);
	auto *stationRow = new QHBoxLayout();
	stationRow->setContentsMargins(0, 0, 0, 0);
	stationRow->addWidget(stationEdit, 1);
	stationRow->addWidget(applyNameButton);
	form->addRow(QStringLiteral("Station name"), stationRow);

	bitrateBox = new QComboBox(settingsDialog);
	for (int br : {64, 96, 128, 160, 192, 256, 320})
		bitrateBox->addItem(QString::number(br), br);
	bitrateBox->setCurrentIndex(2); /* 128 */
	form->addRow(QStringLiteral("Bitrate"), bitrateBox);
	reconnectRetriesSpin = new QSpinBox(settingsDialog);
	reconnectRetriesSpin->setRange(0, 100);
	reconnectRetriesSpin->setValue(20);
	reconnectRetriesSpin->setSpecialValueText(QStringLiteral("Off"));
	reconnectRetriesSpin->setToolTip(
		QStringLiteral("Takes effect on the next connect"));
	form->addRow(QStringLiteral("Reconnect attempts"),
		     reconnectRetriesSpin);

	reconnectDelaySpin = new QSpinBox(settingsDialog);
	reconnectDelaySpin->setRange(1, 60);
	reconnectDelaySpin->setValue(1);
	reconnectDelaySpin->setSuffix(QStringLiteral(" s"));
	reconnectDelaySpin->setToolTip(
		QStringLiteral("Takes effect on the next connect"));
	form->addRow(QStringLiteral("Reconnect delay"), reconnectDelaySpin);

	trackBox = new QComboBox(settingsDialog);
	trackBox->addItem(QStringLiteral("Same as OBS stream"), -1);
	for (int i = 0; i < MAX_AUDIO_MIXES; i++)
		trackBox->addItem(QStringLiteral("Track %1").arg(i + 1), i);
	trackBox->setToolTip(QStringLiteral(
		"Which OBS audio track feeds the radio stream. "
		"\"Same as OBS stream\" follows the track the video platform "
		"gets. Assign sources to tracks in Edit \u2192 Advanced Audio "
		"Properties."));
	form->addRow(QStringLiteral("Audio track"), trackBox);

	trackInfoLabel = new QLabel(settingsDialog);
	trackInfoLabel->setWordWrap(true);
	form->addRow(QString(), trackInfoLabel);

	layout->addLayout(form);

	/*
	 * Kick's title is set through its API with a user token, which needs a
	 * Kick developer app of the station's own (kick.com/settings/developer).
	 * The client ID is public and saved with the other settings; the secret
	 * goes straight to the keychain and is never shown again.
	 */
	auto *kickGroup = new QGroupBox(QStringLiteral("Kick title"),
					settingsDialog);
	auto *kickForm = new QFormLayout(kickGroup);
	kickForm->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);

	kickClientIdEdit = new QLineEdit(kickGroup);
	kickForm->addRow(QStringLiteral("Client ID"), kickClientIdEdit);

	kickSecretEdit = new QLineEdit(kickGroup);
	kickSecretEdit->setEchoMode(QLineEdit::Password);
	kickSecretEdit->setToolTip(QStringLiteral(
		"Saved in the macOS keychain when you leave the field, then "
		"cleared from this window"));
	kickForm->addRow(QStringLiteral("Client secret"), kickSecretEdit);

	auto *redirectEdit = new QLineEdit(
		QStringLiteral(KICK_REDIRECT_URI), kickGroup);
	redirectEdit->setReadOnly(true);
	redirectEdit->setToolTip(QStringLiteral(
		"Enter exactly this as the Redirect URL of your Kick app"));
	kickForm->addRow(QStringLiteral("Redirect URL"), redirectEdit);

	kickConnectButton =
		new QPushButton(QStringLiteral("Connect Kick"), kickGroup);
	kickConnectButton->setToolTip(QStringLiteral(
		"Opens Kick in your browser. Sign in as the channel whose "
		"title should change."));
	kickDisconnectButton =
		new QPushButton(QStringLiteral("Disconnect"), kickGroup);
	kickAccountLabel = new QLabel(kickGroup);
	kickAccountLabel->setWordWrap(true);
	kickAccountLabel->setTextInteractionFlags(Qt::TextSelectableByMouse);
	auto *kickRow = new QHBoxLayout();
	kickRow->setContentsMargins(0, 0, 0, 0);
	kickRow->addWidget(kickConnectButton);
	kickRow->addWidget(kickDisconnectButton);
	kickRow->addWidget(kickAccountLabel, 1);
	kickForm->addRow(QString(), kickRow);

	layout->addWidget(kickGroup);

	/*
	 * Close, not OK/Cancel: every field persists as it changes through
	 * onFieldChanged(), so there is nothing to commit or roll back.
	 */
	auto *buttons = new QDialogButtonBox(QDialogButtonBox::Close,
					     settingsDialog);
	connect(buttons, &QDialogButtonBox::rejected, settingsDialog,
		&QDialog::hide);
	layout->addWidget(buttons);
}

/*
 * Modeless: Apply Name lives in the dialog but reports through the dock's
 * status label, which has to stay visible while the dialog is open.
 */
void RadioCoDock::onSettingsClicked()
{
	updateTrackStatus();
	settingsDialog->show();
	settingsDialog->raise();
	settingsDialog->activateWindow();
}

RadioCoDock::RadioCoDock(QWidget *parent) : QWidget(parent)
{
	setObjectName(DOCK_ID);

	titles = new TitleSync(this);

	buildSettingsDialog();

	auto *root = new QVBoxLayout(this);

	auto *form = new QFormLayout();
	form->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);
	form->setRowWrapPolicy(QFormLayout::WrapLongRows);

	nowPlayingEdit = new QLineEdit(this);
	updateButton = new QPushButton(QStringLiteral("Update"), this);
	updateButton->setEnabled(false);
	auto *songRow = new QHBoxLayout();
	songRow->setContentsMargins(0, 0, 0, 0);
	songRow->addWidget(nowPlayingEdit, 1);
	songRow->addWidget(updateButton);
	form->addRow(QStringLiteral("Stream name"), songRow);

	root->addLayout(form);

	/* Hidden until a Kick account is connected in the settings dialog. */
	kickTitleBox = new QCheckBox(QStringLiteral("Also set the Kick title"),
				     this);
	kickTitleBox->setChecked(true);
	kickTitleBox->setToolTip(QStringLiteral(
		"Update sends the stream name to Kick too, and it is sent "
		"when OBS starts streaming."));
	root->addWidget(kickTitleBox);

	titleStatusLabel = new QLabel(this);
	titleStatusLabel->setTextFormat(Qt::RichText);
	titleStatusLabel->setWordWrap(true);
	titleStatusLabel->setVisible(false);
	root->addWidget(titleStatusLabel);

	followObsBox = new QCheckBox(
		QStringLiteral("Connect with OBS \"Start Streaming\""), this);
	followObsBox->setChecked(false);
	followObsBox->setToolTip(QStringLiteral(
		"Also connect this audio stream when OBS starts streaming video."));
	root->addWidget(followObsBox);

	silentWarnLabel = new QLabel(this);
	silentWarnLabel->setWordWrap(true);
	silentWarnLabel->setVisible(false);
	root->addWidget(silentWarnLabel);

	auto *bottom = new QHBoxLayout();
	connectButton = new QPushButton(QStringLiteral("Connect"), this);
	statusLabel = new QLabel(QStringLiteral("Idle"), this);
	statusLabel->setTextInteractionFlags(Qt::TextSelectableByMouse);
	settingsButton = new QPushButton(this);
	settingsButton->setIcon(make_gear_icon(
		palette().color(QPalette::WindowText)));
	settingsButton->setIconSize(QSize(16, 16));
	settingsButton->setFlat(true);
	settingsButton->setFixedWidth(28);
	settingsButton->setToolTip(QStringLiteral("Connection settings"));
	settingsButton->setAccessibleName(QStringLiteral("Settings"));
	bottom->addWidget(connectButton);
	bottom->addWidget(statusLabel, 1);
	bottom->addWidget(settingsButton);
	root->addLayout(bottom);

	root->addStretch(1);

	connect(connectButton, &QPushButton::clicked, this,
		&RadioCoDock::onConnectClicked);
	connect(updateButton, &QPushButton::clicked, this,
		&RadioCoDock::onUpdateClicked);
	connect(applyNameButton, &QPushButton::clicked, this,
		&RadioCoDock::onApplyNameClicked);
	connect(settingsButton, &QPushButton::clicked, this,
		&RadioCoDock::onSettingsClicked);
	connect(kickConnectButton, &QPushButton::clicked, this,
		&RadioCoDock::onKickConnectClicked);
	connect(kickDisconnectButton, &QPushButton::clicked, this,
		&RadioCoDock::onKickDisconnectClicked);
	connect(kickSecretEdit, &QLineEdit::editingFinished, this,
		&RadioCoDock::onKickSecretEntered);
	connect(titles, &TitleSync::titleStatus, this,
		&RadioCoDock::onTitleStatus);
	connect(titles, &TitleSync::kickConnectFinished, this,
		&RadioCoDock::onKickConnectFinished);

	/* Persist on every change, and keep the buttons in step. */
	connect(protocolBox, &QComboBox::currentIndexChanged, this,
		&RadioCoDock::onFieldChanged);
	connect(bitrateBox, &QComboBox::currentIndexChanged, this,
		&RadioCoDock::onFieldChanged);
	connect(portSpin, &QSpinBox::valueChanged, this,
		&RadioCoDock::onFieldChanged);
	connect(reconnectRetriesSpin, &QSpinBox::valueChanged, this,
		&RadioCoDock::onFieldChanged);
	connect(reconnectDelaySpin, &QSpinBox::valueChanged, this,
		&RadioCoDock::onFieldChanged);
	connect(followObsBox, &QCheckBox::toggled, this,
		&RadioCoDock::onFieldChanged);
	connect(kickTitleBox, &QCheckBox::toggled, this,
		&RadioCoDock::onFieldChanged);
	connect(kickTitleBox, &QCheckBox::toggled, this,
		&RadioCoDock::renderTitleStatus);
	connect(kickClientIdEdit, &QLineEdit::textChanged, this,
		[this](const QString &id) { titles->setKickClientId(id); });
	/* The secret field is deliberately absent: it is never saved here. */
	for (QLineEdit *e : {serverEdit, usernameEdit, mountEdit,
			     passwordEdit, stationEdit, nowPlayingEdit,
			     kickClientIdEdit})
		connect(e, &QLineEdit::textChanged, this,
			&RadioCoDock::onFieldChanged);

	/* Switching tracks reconnects, so it gets its own slot rather than
	 * the shared onFieldChanged, which only saves. */
	connect(trackBox, &QComboBox::currentIndexChanged, this,
		&RadioCoDock::onTrackChanged);

	/* Registered before loadSettings, which restores their bindings. */
	connectHotkey = obs_hotkey_register_frontend(
		"obs_icecast.connect", "Radio.co: Connect", hotkey_connect,
		this);
	disconnectHotkey = obs_hotkey_register_frontend(
		"obs_icecast.disconnect", "Radio.co: Disconnect",
		hotkey_disconnect, this);

	loadSettings();

	tickTimer = new QTimer(this);
	tickTimer->setInterval(1000);
	connect(tickTimer, &QTimer::timeout, this, &RadioCoDock::onTick);
	tickTimer->start();

	updateTrackStatus();
	refreshControls();
}

RadioCoDock::~RadioCoDock()
{
	if (tickTimer)
		tickTimer->stop();
	releaseOutput();
	if (connectHotkey != OBS_INVALID_HOTKEY_ID)
		obs_hotkey_unregister(connectHotkey);
	if (disconnectHotkey != OBS_INVALID_HOTKEY_ID)
		obs_hotkey_unregister(disconnectHotkey);
}

/* ------------------------------------------------------------------ */

obs_data_t *RadioCoDock::buildOutputSettings() const
{
	obs_data_t *s = obs_data_create();

	obs_data_set_int(s, "protocol", protocolBox->currentData().toInt());
	obs_data_set_string(s, "server",
			    serverEdit->text().toUtf8().constData());
	obs_data_set_int(s, "port", portSpin->value());
	obs_data_set_string(s, "username",
			    usernameEdit->text().toUtf8().constData());
	obs_data_set_string(s, "mount",
			    mountEdit->text().toUtf8().constData());
	obs_data_set_string(s, "password",
			    passwordEdit->text().toUtf8().constData());
	obs_data_set_string(s, "station_name",
			    stationEdit->text().toUtf8().constData());
	obs_data_set_string(s, "genre", "Various");
	obs_data_set_string(s, "url", "");
	obs_data_set_bool(s, "is_public", false);
	obs_data_set_int(s, "bitrate", bitrateBox->currentData().toInt());

	return s;
}

void RadioCoDock::startOutput(bool asAutoStart)
{
	if (output)
		return;

	if (serverEdit->text().trimmed().isEmpty()) {
		setStatus(QStringLiteral("Set a server address"),
			  StatusKind::Error);
		return;
	}

	const int bitrate = bitrateBox->currentData().toInt();
	const int track = selectedMixerIndex();
	activeMixerIndex = track;

	obs_data_t *es = obs_data_create();
	obs_data_set_int(es, "bitrate", bitrate);
	encoder = obs_audio_encoder_create("icecast_mp3", "Radio.co MP3", es,
					   (size_t)track, nullptr);
	obs_data_release(es);

	if (!encoder) {
		setStatus(QStringLiteral("Failed to create MP3 encoder"),
			  StatusKind::Error);
		return;
	}
	obs_encoder_set_audio(encoder, obs_get_audio());

	blog(LOG_INFO, "[obs-icecast] streaming OBS audio track %d%s",
	     track + 1,
	     trackBox->currentData().toInt() < 0 ? " (same as OBS stream)"
						 : "");

	track_scan scan{track, true, 0, {}};
	obs_enum_sources(scan_track_source, &scan);
	if (scan.on_air == 0)
		blog(LOG_WARNING,
		     "[obs-icecast] track %d has no audio sources; the "
		     "stream will be silent",
		     track + 1);

	obs_data_t *os = buildOutputSettings();
	output = obs_output_create("icecast_output", "Radio.co Output", os,
				   nullptr);
	obs_data_release(os);

	if (!output) {
		setStatus(QStringLiteral("Failed to create output"),
			  StatusKind::Error);
		releaseOutput();
		return;
	}

	signal_handler_t *sh = obs_output_get_signal_handler(output);
	signal_handler_connect(sh, "start", handle_start, this);
	signal_handler_connect(sh, "stop", handle_stop, this);
	signal_handler_connect(sh, "reconnect", handle_reconnect, this);
	signal_handler_connect(sh, "reconnect_success",
			       handle_reconnect_success, this);

	/* Defaults match butt: 20 attempts at 1 s. */
	obs_output_set_reconnect_settings(output,
					  reconnectRetriesSpin->value(),
					  reconnectDelaySpin->value());
	obs_output_set_audio_encoder(output, encoder, 0);

	autoStarted = asAutoStart;
	connectedName = stationEdit->text();
	liveTarget = currentTarget();
	setStatus(QStringLiteral("Connecting…"), StatusKind::Busy);

	if (!obs_output_start(output)) {
		const char *err = obs_output_get_last_error(output);
		setStatus(err && *err ? QString::fromUtf8(err)
				      : QStringLiteral("Failed to start"),
			  StatusKind::Error);
		releaseOutput();
	}

	refreshControls();
}

void RadioCoDock::stopOutput()
{
	if (!output)
		return;
	obs_output_stop(output);
	setStatus(QStringLiteral("Disconnecting…"), StatusKind::Busy);
}

/*
 * Final teardown of the streaming side. Idempotent, and safe to call while a
 * broadcast is live: the signal handlers are disconnected first so no queued
 * slot can fire against a widget that is about to disappear.
 */
void RadioCoDock::shutdownOutput()
{
	shuttingDown = true;
	pendingRestart = false;

	if (tickTimer)
		tickTimer->stop();

	if (output) {
		obs_output_stop(output);
		releaseOutput();
	}

	/* Joins the title workers while the plugin's code is still loaded. */
	if (titles)
		titles->shutdown();
}

void RadioCoDock::releaseOutput()
{
	if (output) {
		signal_handler_t *sh = obs_output_get_signal_handler(output);
		signal_handler_disconnect(sh, "start", handle_start, this);
		signal_handler_disconnect(sh, "stop", handle_stop, this);
		signal_handler_disconnect(sh, "reconnect", handle_reconnect,
					  this);
		signal_handler_disconnect(sh, "reconnect_success",
					  handle_reconnect_success, this);
		obs_output_release(output);
		output = nullptr;
	}
	if (encoder) {
		obs_encoder_release(encoder);
		encoder = nullptr;
	}
	autoStarted = false;
	activeMixerIndex = -1;
	connectGeneration++;
	if (titles)
		titles->setRadioLive(false);
}

/* ------------------------------------------------------------------ */

void RadioCoDock::onConnectClicked()
{
	if (output)
		stopOutput();
	else
		startOutput(false);
}

/*
 * A hotkey start counts as manual, like the Connect button: stopping the OBS
 * stream later leaves it on air even when "Connect with OBS Start Streaming"
 * is ticked.
 */
void RadioCoDock::onHotkeyConnect()
{
	blog(LOG_INFO, "[obs-icecast] hotkey: connect (%s)",
	     output ? "already connected, ignored" : "connecting");
	if (shuttingDown || output)
		return;
	startOutput(false);
}

void RadioCoDock::onHotkeyDisconnect()
{
	blog(LOG_INFO, "[obs-icecast] hotkey: disconnect (%s)",
	     output ? "disconnecting" : "not connected, ignored");
	if (shuttingDown || !output)
		return;
	stopOutput();
}

/*
 * Every press sends, even an unchanged title: after a failure, pressing
 * Update again is the obvious retry. A radio feed that is still connecting
 * gets the title from scheduleRadioTitle() once it is live.
 */
void RadioCoDock::onUpdateClicked()
{
	const QString song = nowPlayingEdit->text();
	if (song.isEmpty())
		return;

	if (output && obs_output_active(output))
		sendRadioTitle("Update");
	if (kickEnabled())
		titles->sendKick(song);
}

void RadioCoDock::sendRadioTitle(const char *why)
{
	const QString song = nowPlayingEdit->text();
	if (song.isEmpty() || !output || !obs_output_active(output))
		return;

	blog(LOG_INFO, "[obs-icecast] sending stream name to radio.co (%s)",
	     why);
	titles->sendRadio(song, liveTarget);
}

/*
 * Each (re)connect is a new source on the server, which starts without our
 * title. The generation check drops timers from a connection that has since
 * gone away.
 */
void RadioCoDock::scheduleRadioTitle()
{
	titles->setRadioLive(true);
	const int generation = ++connectGeneration;
	QTimer::singleShot(RADIO_TITLE_DELAY_MS, this, [this, generation] {
		if (generation == connectGeneration && !shuttingDown)
			sendRadioTitle("after connect");
	});
}

bool RadioCoDock::kickEnabled() const
{
	return !kickChannel.isEmpty() && kickTitleBox->isChecked();
}

RadioTarget RadioCoDock::currentTarget() const
{
	RadioTarget t;
	t.server = serverEdit->text().toStdString();
	t.username = usernameEdit->text().toStdString();
	t.password = passwordEdit->text().toStdString();
	t.mount = mountEdit->text().toStdString();
	t.port = portSpin->value();
	t.protocol = protocolBox->currentData().toInt();
	return t;
}

void RadioCoDock::onApplyNameClicked()
{
	if (!output)
		return;

	/*
	 * ice-name travels only in the handshake, so the name cannot change
	 * on a live connection — reconnect instead. The restart is fired from
	 * onOutputStopped, never synchronously from here.
	 */
	pendingRestart = true;
	applyNameButton->setEnabled(false);
	stopOutput();
	setStatus(QStringLiteral("Applying name…"), StatusKind::Busy);
}

void RadioCoDock::onFieldChanged()
{
	if (loading)
		return;
	saveSettings();
	refreshControls();
}

void RadioCoDock::onTrackChanged()
{
	if (loading)
		return;

	saveSettings();
	updateTrackStatus();

	/*
	 * The mixer index is fixed when the encoder is created, so switching
	 * tracks on air means a reconnect. Same mechanism as Apply Name: the
	 * restart is fired from onOutputStopped, never synchronously here.
	 */
	if (output) {
		pendingRestart = true;
		stopOutput();
		setStatus(QStringLiteral("Switching audio track\u2026"),
			  StatusKind::Busy);
	}

	refreshControls();
}

/*
 * The combo stores -1 for "Same as OBS stream"; every other consumer wants a
 * real 0-based mixer index, resolved fresh so a profile or output-mode change
 * in OBS is picked up without the user touching this dialog.
 */
int RadioCoDock::selectedMixerIndex() const
{
	const int data = trackBox->currentData().toInt();
	return data < 0 ? obs_stream_mixer_index() : data;
}

void RadioCoDock::onTick()
{
	if (shuttingDown)
		return;

	if (output && obs_output_active(output)) {
		const qint64 secs = liveTimer.isValid()
					    ? liveTimer.elapsed() / 1000
					    : 0;
		setStatus(
			QStringLiteral("● Live %1:%2:%3")
				.arg(secs / 3600, 2, 10, QLatin1Char('0'))
				.arg((secs / 60) % 60, 2, 10, QLatin1Char('0'))
				.arg(secs % 60, 2, 10, QLatin1Char('0')),
			StatusKind::Live);
	}

	/*
	 * Following OBS's stream track means following it when it moves —
	 * switching output mode or AdvOut/TrackIndex changes the answer, and
	 * the mixer index cannot be retuned on a live encoder. Restart only
	 * from a settled live output, so this never races a pending restart.
	 */
	if (output && obs_output_active(output) && !pendingRestart &&
	    activeMixerIndex >= 0 && selectedMixerIndex() != activeMixerIndex) {
		blog(LOG_INFO,
		     "[obs-icecast] OBS stream track moved %d -> %d; "
		     "reconnecting",
		     activeMixerIndex + 1, selectedMixerIndex() + 1);
		pendingRestart = true;
		stopOutput();
		setStatus(QStringLiteral("Following OBS stream track\u2026"),
			  StatusKind::Busy);
		return;
	}
	updateTrackStatus();
	refreshControls();
}

/* ------------------------------------------------------------------ */

void RadioCoDock::onOutputStarted()
{
	liveTimer.start();
	connectedName = stationEdit->text();
	setStatus(QStringLiteral("● Live 00:00:00"), StatusKind::Live);
	scheduleRadioTitle();
	refreshControls();
}

void RadioCoDock::onOutputStopped(int code)
{
	const char *err = output ? obs_output_get_last_error(output) : nullptr;
	const bool wasAuto = autoStarted;

	releaseOutput();
	liveTimer.invalidate();

	if (err && *err)
		setStatus(QString::fromUtf8(err), StatusKind::Error);
	else if (code != OBS_OUTPUT_SUCCESS)
		setStatus(QStringLiteral("Disconnected (code %1)").arg(code),
			  StatusKind::Error);
	else
		setStatus(QStringLiteral("Idle"), StatusKind::Idle);

	if (pendingRestart && !shuttingDown) {
		pendingRestart = false;
		startOutput(wasAuto);
		return;
	}

	refreshControls();
}

void RadioCoDock::onOutputReconnecting()
{
	connectGeneration++;
	titles->setRadioLive(false);
	setStatus(QStringLiteral("Reconnecting…"), StatusKind::Busy);
}

void RadioCoDock::onOutputReconnected()
{
	liveTimer.start();
	setStatus(QStringLiteral("● Live 00:00:00"), StatusKind::Live);
	scheduleRadioTitle();
}

/* ------------------------------------------------------------------ */

QString RadioCoDock::styleFor(StatusKind kind)
{
	switch (kind) {
	case StatusKind::Busy:
		return QStringLiteral("color: " COLOR_BUSY ";");
	case StatusKind::Live:
		return QStringLiteral("color: " COLOR_LIVE ";");
	case StatusKind::Error:
		return QStringLiteral("color: " COLOR_ERROR ";");
	case StatusKind::Idle:
		break;
	}
	return QString();
}

void RadioCoDock::setStatus(const QString &text, StatusKind kind)
{
	statusLabel->setText(text);
	statusLabel->setStyleSheet(styleFor(kind));
}

void RadioCoDock::setKickAccount(const QString &text, StatusKind kind)
{
	kickAccountLabel->setText(text);
	kickAccountLabel->setStyleSheet(styleFor(kind));
}

void RadioCoDock::refreshControls()
{
	const bool active = output != nullptr;

	connectButton->setText(active ? QStringLiteral("Disconnect")
				      : QStringLiteral("Connect"));
	updateButton->setEnabled((active || kickEnabled()) &&
				 !nowPlayingEdit->text().isEmpty());

	kickTitleBox->setVisible(!kickChannel.isEmpty());
	kickDisconnectButton->setEnabled(!kickChannel.isEmpty());

	const bool nameChanged =
		active && stationEdit->text() != connectedName;
	applyNameButton->setEnabled(nameChanged);
	applyNameButton->setToolTip(
		nameChanged
			? QStringLiteral("Reconnects the stream (brief dropout)")
			: QStringLiteral(
				  "The station name is sent when connecting"));

	/*
	 * Usable while live — switching reconnects — but locked during the
	 * connect/disconnect window, where a second stopOutput() would race
	 * a pending restart.
	 */
	trackBox->setEnabled(!active || obs_output_active(output));
}

/*
 * Polled from onTick() at 1 Hz rather than driven by signals: track
 * assignment, mute and monitoring are per-source signals with no global
 * equivalent, and scene membership arrives separately again. One enumeration
 * covers all of them with no per-source handler bookkeeping.
 */
void RadioCoDock::updateTrackStatus()
{
	track_scan scan{selectedMixerIndex(), false, 0, {}};
	obs_enum_sources(scan_track_source, &scan);

	const bool silent = scan.on_air == 0;
	const bool following = trackBox->currentData().toInt() < 0;

	if (silent)
		silentWarnLabel->setText(
			QStringLiteral("\u26a0 Track %1 has no audio \u2014 "
				       "this stream is silent.")
				.arg(scan.track + 1));
	silentWarnLabel->setVisible(silent);

	/* The detailed list is only worth building while it is on screen. */
	if (!settingsDialog->isVisible())
		return;

	/*
	 * When following OBS, name the track it resolved to — otherwise the
	 * combo says "Same as OBS stream" and nothing says which mix that is.
	 */
	const QString prefix =
		following ? QStringLiteral("OBS streams track %1. ")
				    .arg(scan.track + 1)
			  : QString();

	trackInfoLabel->setText(
		prefix + (silent ? QStringLiteral(
					   "No audio sources on this track.")
				 : QStringLiteral("On air: %1")
					   .arg(scan.names.join(
						   QStringLiteral(", ")))));
}

void RadioCoDock::handleFrontendEvent(obs_frontend_event event)
{
	switch (event) {
	case OBS_FRONTEND_EVENT_FINISHED_LOADING:
		/* Only now is it safe to ask the frontend about outputs. */
		frontend_ready = true;
		updateTrackStatus();
		break;

	case OBS_FRONTEND_EVENT_EXIT:
		/*
		 * This is the last safe point to touch the output: OBS
		 * destroys the frontend (and with it this widget) before
		 * obs_module_unload runs.
		 */
		if (settingsDialog)
			settingsDialog->hide();
		saveSettings();
		shutdownOutput();
		break;

	case OBS_FRONTEND_EVENT_STREAMING_STARTED:
		/* Kick takes a title live or not; set it as the show starts. */
		if (kickEnabled() && !nowPlayingEdit->text().isEmpty()) {
			blog(LOG_INFO, "[obs-icecast] OBS stream started; "
				       "sending stream name to Kick");
			titles->sendKick(nowPlayingEdit->text());
		}
		if (followObsBox->isChecked() && !output)
			startOutput(true);
		break;

	case OBS_FRONTEND_EVENT_STREAMING_STOPPED:
		/*
		 * Only tear down a feed this checkbox started. A manually
		 * connected broadcast survives stopping the video stream.
		 */
		if (followObsBox->isChecked() && output && autoStarted)
			stopOutput();
		break;

	default:
		break;
	}
}

/* ------------------------------------------------------------------ */

void RadioCoDock::loadSettings()
{
	loading = true;

	char *path = obs_module_config_path("settings.json");
	obs_data_t *s = path ? obs_data_create_from_json_file_safe(path, "bak")
			     : nullptr;
	bfree(path);

	if (!s)
		s = obs_data_create();

	obs_data_set_default_int(s, "protocol", 0);
	obs_data_set_default_string(s, "server", "");
	obs_data_set_default_int(s, "port", 80);
	obs_data_set_default_string(s, "username", "source");
	obs_data_set_default_string(s, "mount", "/");
	obs_data_set_default_string(s, "password", "");
	obs_data_set_default_string(s, "station_name", "OBS Stream");
	obs_data_set_default_int(s, "bitrate", 128);
	obs_data_set_default_int(s, "reconnect_retries", 20);
	obs_data_set_default_int(s, "reconnect_delay", 1);
	obs_data_set_default_int(s, "mixer_index", -1);
	obs_data_set_default_string(s, "song", "");
	obs_data_set_default_bool(s, "follow_obs", false);
	obs_data_set_default_bool(s, "kick_title", true);
	obs_data_set_default_string(s, "kick_client_id", "");
	obs_data_set_default_string(s, "kick_channel", "");

	const int protocol = (int)obs_data_get_int(s, "protocol");
	protocolBox->setCurrentIndex(protocolBox->findData(protocol) >= 0
					     ? protocolBox->findData(protocol)
					     : 0);
	serverEdit->setText(
		QString::fromUtf8(obs_data_get_string(s, "server")));
	portSpin->setValue((int)obs_data_get_int(s, "port"));
	usernameEdit->setText(
		QString::fromUtf8(obs_data_get_string(s, "username")));
	mountEdit->setText(QString::fromUtf8(obs_data_get_string(s, "mount")));
	passwordEdit->setText(
		QString::fromUtf8(obs_data_get_string(s, "password")));
	stationEdit->setText(
		QString::fromUtf8(obs_data_get_string(s, "station_name")));

	const int bitrate = (int)obs_data_get_int(s, "bitrate");
	const int bitrateIdx = bitrateBox->findData(bitrate);
	bitrateBox->setCurrentIndex(bitrateIdx >= 0 ? bitrateIdx : 2);
	reconnectRetriesSpin->setValue(
		(int)obs_data_get_int(s, "reconnect_retries"));
	reconnectDelaySpin->setValue(
		(int)obs_data_get_int(s, "reconnect_delay"));

	/*
	 * -1 means "Same as OBS stream". Anything else is a 0-based mixer
	 * index and must exist in the combo: a settings file from another
	 * machine, hand-edited, or written by a future version must never
	 * hand libobs an out-of-range index.
	 */
	const int track = (int)obs_data_get_int(s, "mixer_index");
	const int trackIdx = trackBox->findData(track);
	trackBox->setCurrentIndex(trackIdx >= 0 ? trackIdx : 0);

	nowPlayingEdit->setText(
		QString::fromUtf8(obs_data_get_string(s, "song")));
	followObsBox->setChecked(obs_data_get_bool(s, "follow_obs"));

	kickTitleBox->setChecked(obs_data_get_bool(s, "kick_title"));
	kickClientIdEdit->setText(
		QString::fromUtf8(obs_data_get_string(s, "kick_client_id")));
	titles->setKickClientId(kickClientIdEdit->text());
	kickChannel = QString::fromUtf8(obs_data_get_string(s, "kick_channel"));
	kickSecretEdit->setPlaceholderText(
		titles->kickHasSecret() ? QStringLiteral("Saved in Keychain")
					: QString());
	if (kickChannel.isEmpty())
		setKickAccount(QStringLiteral("Not connected"),
			       StatusKind::Idle);
	else
		setKickAccount(QStringLiteral("Connected: %1").arg(kickChannel),
			       StatusKind::Live);

	/*
	 * OBS persists only its own frontend hotkeys (in the profile), so a
	 * plugin's bindings travel in its own settings file. A missing array
	 * loads as no bindings.
	 */
	obs_data_array_t *keys = obs_data_get_array(s, "hotkey_connect");
	obs_hotkey_load(connectHotkey, keys);
	const size_t nConnect = obs_data_array_count(keys);
	obs_data_array_release(keys);
	keys = obs_data_get_array(s, "hotkey_disconnect");
	obs_hotkey_load(disconnectHotkey, keys);
	const size_t nDisconnect = obs_data_array_count(keys);
	obs_data_array_release(keys);
	blog(LOG_INFO,
	     "[obs-icecast] hotkeys loaded: connect %zu, disconnect %zu "
	     "binding(s)",
	     nConnect, nDisconnect);

	obs_data_release(s);
	loading = false;
}

void RadioCoDock::saveSettings()
{
	/* obs_data_save_json_safe will not create the directory. */
	char *dir = obs_module_config_path(nullptr);
	if (dir) {
		os_mkdirs(dir);
		bfree(dir);
	}

	char *path = obs_module_config_path("settings.json");
	if (!path)
		return;

	obs_data_t *s = obs_data_create();
	obs_data_set_int(s, "protocol", protocolBox->currentData().toInt());
	obs_data_set_string(s, "server",
			    serverEdit->text().toUtf8().constData());
	obs_data_set_int(s, "port", portSpin->value());
	obs_data_set_string(s, "username",
			    usernameEdit->text().toUtf8().constData());
	obs_data_set_string(s, "mount",
			    mountEdit->text().toUtf8().constData());
	obs_data_set_string(s, "password",
			    passwordEdit->text().toUtf8().constData());
	obs_data_set_string(s, "station_name",
			    stationEdit->text().toUtf8().constData());
	obs_data_set_int(s, "bitrate", bitrateBox->currentData().toInt());
	obs_data_set_int(s, "reconnect_retries",
			 reconnectRetriesSpin->value());
	obs_data_set_int(s, "reconnect_delay", reconnectDelaySpin->value());
	obs_data_set_int(s, "mixer_index", trackBox->currentData().toInt());
	obs_data_set_string(s, "song",
			    nowPlayingEdit->text().toUtf8().constData());
	obs_data_set_bool(s, "follow_obs", followObsBox->isChecked());
	obs_data_set_bool(s, "kick_title", kickTitleBox->isChecked());
	obs_data_set_string(s, "kick_client_id",
			    kickClientIdEdit->text().trimmed().toUtf8().constData());
	obs_data_set_string(s, "kick_channel",
			    kickChannel.toUtf8().constData());

	obs_data_array_t *keys = obs_hotkey_save(connectHotkey);
	obs_data_set_array(s, "hotkey_connect", keys);
	obs_data_array_release(keys);
	keys = obs_hotkey_save(disconnectHotkey);
	obs_data_set_array(s, "hotkey_disconnect", keys);
	obs_data_array_release(keys);

	obs_data_save_json_safe(s, path, "tmp", "bak");

	obs_data_release(s);
	bfree(path);
}

/* ------------------------------------------------------------------ */
/* Titles: radio.co and Kick                                            */
/* ------------------------------------------------------------------ */

void RadioCoDock::onTitleStatus(int dest, int state, int attempt,
				const QString &detail)
{
	TitleOutcome &o = dest == (int)TitleDest::Kick ? kickOutcome
						       : radioOutcome;
	o.state = state;
	o.attempt = attempt;
	o.detail = detail;
	o.at = QTime::currentTime();

	if (dest == (int)TitleDest::Kick &&
	    state == (int)TitleState::NeedsSignIn)
		setKickAccount(QStringLiteral("Sign-in no longer valid: press "
					      "Connect Kick"),
			       StatusKind::Error);

	renderTitleStatus();
}

/*
 * One line, one colored part per destination, e.g.
 * "radio.co ✓ 21:04 · Kick: retry 1/3…". Details go to the tooltip and the
 * OBS log; the line stays short enough for a narrow dock.
 */
void RadioCoDock::renderTitleStatus()
{
	struct Part {
		const char *name;
		const TitleOutcome *o;
	};
	QStringList parts, tips;

	for (const Part &p : {Part{"radio.co", &radioOutcome},
			      Part{"Kick", &kickOutcome}}) {
		if (p.o->state < 0)
			continue;
		if (p.o == &kickOutcome && !kickEnabled())
			continue;

		const QString name = QString::fromLatin1(p.name);
		QString text;
		const char *color = nullptr;
		switch ((TitleState)p.o->state) {
		case TitleState::Sending:
			text = QStringLiteral("%1: sending\u2026").arg(name);
			color = COLOR_BUSY;
			break;
		case TitleState::Done:
			text = QStringLiteral("%1 \u2713 %2")
				       .arg(name, p.o->at.toString(
							  QStringLiteral("HH:mm")));
			color = COLOR_LIVE;
			break;
		case TitleState::Retrying:
			text = QStringLiteral("%1: retry %2/3\u2026")
				       .arg(name)
				       .arg(p.o->attempt);
			color = COLOR_BUSY;
			break;
		case TitleState::Failed:
			text = QStringLiteral("%1 \u2717 not set").arg(name);
			color = COLOR_ERROR;
			break;
		case TitleState::NeedsSignIn:
			text = QStringLiteral("%1 \u2717 sign in again (gear)")
				       .arg(name);
			color = COLOR_ERROR;
			break;
		case TitleState::Dropped:
			text = QStringLiteral("%1: not sent (off air)").arg(name);
			break;
		}

		parts << (color ? QStringLiteral("<span style=\"color:%1\">"
						 "%2</span>")
					  .arg(QLatin1String(color),
					       text.toHtmlEscaped())
				: text.toHtmlEscaped());
		if (!p.o->detail.isEmpty())
			tips << QStringLiteral("%1: %2").arg(name, p.o->detail);
	}

	titleStatusLabel->setText(parts.join(QStringLiteral(" \u00b7 ")));
	titleStatusLabel->setToolTip(tips.join(QLatin1Char('\n')));
	titleStatusLabel->setVisible(!parts.isEmpty());
}

void RadioCoDock::onKickSecretEntered()
{
	const QString secret = kickSecretEdit->text();
	if (secret.trimmed().isEmpty())
		return;

	/* Cleared either way: the secret must not linger in a widget. */
	kickSecretEdit->clear();
	if (titles->setKickSecret(secret)) {
		kickSecretEdit->setPlaceholderText(
			QStringLiteral("Saved in Keychain"));
		blog(LOG_INFO, "[obs-icecast] kick: client secret saved to "
			       "the keychain");
	} else {
		setKickAccount(QStringLiteral("Could not save the secret to "
					      "the keychain (see log)"),
			       StatusKind::Error);
	}
}

void RadioCoDock::onKickConnectClicked()
{
	/* A secret typed but not yet confirmed still counts. */
	onKickSecretEntered();
	saveSettings();

	QString url, error;
	if (!titles->beginKickConnect(url, error)) {
		setKickAccount(error, StatusKind::Error);
		return;
	}
	setKickAccount(QStringLiteral("Waiting for the browser\u2026"),
		       StatusKind::Busy);
	QDesktopServices::openUrl(QUrl::fromEncoded(url.toUtf8()));
}

void RadioCoDock::onKickConnectFinished(bool ok, const QString &text)
{
	if (!ok) {
		setKickAccount(text, StatusKind::Error);
		return;
	}

	kickChannel = text;
	kickOutcome = TitleOutcome();
	setKickAccount(QStringLiteral("Connected: %1").arg(kickChannel),
		       StatusKind::Live);
	saveSettings();
	renderTitleStatus();
	refreshControls();
}

void RadioCoDock::onKickDisconnectClicked()
{
	titles->disconnectKick();
	kickChannel.clear();
	kickOutcome = TitleOutcome();
	setKickAccount(QStringLiteral("Not connected"), StatusKind::Idle);
	saveSettings();
	renderTitleStatus();
	refreshControls();
}

/* ------------------------------------------------------------------ */
/* C entry points                                                       */
/* ------------------------------------------------------------------ */

static void frontend_event(enum obs_frontend_event event, void *private_data)
{
	auto *d = static_cast<RadioCoDock *>(private_data);
	if (d)
		d->handleFrontendEvent(event);
}

void radioco_dock_init(void)
{
	if (dock)
		return;

	dock = new RadioCoDock();

	if (!obs_frontend_add_dock_by_id(DOCK_ID, DOCK_TITLE, dock)) {
		blog(LOG_ERROR, "[obs-icecast] failed to register dock");
		/* Registration failed, so nothing reparented it — we own it. */
		delete dock;
		dock = nullptr;
		return;
	}

	obs_frontend_add_event_callback(frontend_event, dock);
	blog(LOG_INFO, "[obs-icecast] " DOCK_TITLE " dock registered");
}

void radioco_dock_free(void)
{
	/*
	 * Deliberately does NOT call obs_frontend_remove_dock or
	 * obs_frontend_remove_event_callback, and does NOT delete the widget.
	 *
	 * By the time obs_module_unload runs, OBS has already torn the
	 * frontend down: those calls log "Tried to call ... with no
	 * callbacks!" and do nothing, and the widget itself is already gone
	 * because obs_frontend_add_dock_by_id reparented it into an OBSDock
	 * owned by the main window. Deleting it here double-frees and OBS
	 * dies mid-shutdown.
	 *
	 * plugins/decklink-output-ui does the same: it only stops its outputs
	 * from unload and never calls a frontend API there. The QPointer is
	 * already null in the normal shutdown path; the guard only matters if
	 * a module is unloaded while the frontend is somehow still alive.
	 */
	if (dock)
		dock->shutdownOutput();
	dock = nullptr;
}

#include "radioco-dock.moc"
