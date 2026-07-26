#include "radioco-dock.h"

#include <obs.h>
#include <obs-module.h>
#include <obs-frontend-api.h>
#include <util/platform.h>

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
#include <QDialog>
#include <QDialogButtonBox>
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

private slots:
	void onConnectClicked();
	void onUpdateClicked();
	void onApplyNameClicked();
	void onTick();
	void onFieldChanged();
	void onSettingsClicked();

private:
	void startOutput(bool asAutoStart);
	void stopOutput();
	void releaseOutput();
	void buildSettingsDialog();
	void loadSettings();
	void saveSettings();
	void refreshControls();
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
	QLineEdit *nowPlayingEdit = nullptr;
	QPushButton *updateButton = nullptr;
	QCheckBox *followObsBox = nullptr;
	QPushButton *connectButton = nullptr;
	QLabel *statusLabel = nullptr;
	QDialog *settingsDialog = nullptr;
	QPushButton *settingsButton = nullptr;
	QTimer *tickTimer = nullptr;

	obs_output_t *output = nullptr;
	obs_encoder_t *encoder = nullptr;

	QElapsedTimer liveTimer;
	QString connectedName;
	bool pendingRestart = false;
	bool autoStarted = false;
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

	layout->addLayout(form);

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
	settingsDialog->show();
	settingsDialog->raise();
	settingsDialog->activateWindow();
}

RadioCoDock::RadioCoDock(QWidget *parent) : QWidget(parent)
{
	setObjectName(DOCK_ID);

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

	followObsBox = new QCheckBox(
		QStringLiteral("Connect with OBS \"Start Streaming\""), this);
	followObsBox->setChecked(false);
	followObsBox->setToolTip(QStringLiteral(
		"Also connect this audio stream when OBS starts streaming video."));
	root->addWidget(followObsBox);

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

	/* Persist on every change, and keep the buttons in step. */
	connect(protocolBox, &QComboBox::currentIndexChanged, this,
		&RadioCoDock::onFieldChanged);
	connect(bitrateBox, &QComboBox::currentIndexChanged, this,
		&RadioCoDock::onFieldChanged);
	connect(portSpin, &QSpinBox::valueChanged, this,
		&RadioCoDock::onFieldChanged);
	connect(followObsBox, &QCheckBox::toggled, this,
		&RadioCoDock::onFieldChanged);
	for (QLineEdit *e : {serverEdit, usernameEdit, mountEdit,
			     passwordEdit, stationEdit, nowPlayingEdit})
		connect(e, &QLineEdit::textChanged, this,
			&RadioCoDock::onFieldChanged);

	loadSettings();

	tickTimer = new QTimer(this);
	tickTimer->setInterval(1000);
	connect(tickTimer, &QTimer::timeout, this, &RadioCoDock::onTick);
	tickTimer->start();

	refreshControls();
}

RadioCoDock::~RadioCoDock()
{
	if (tickTimer)
		tickTimer->stop();
	releaseOutput();
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
		statusLabel->setText(QStringLiteral("Set a server address"));
		return;
	}

	const int bitrate = bitrateBox->currentData().toInt();

	obs_data_t *es = obs_data_create();
	obs_data_set_int(es, "bitrate", bitrate);
	encoder = obs_audio_encoder_create("shoutcast_mp3", "Radio.co MP3", es,
					   0, nullptr);
	obs_data_release(es);

	if (!encoder) {
		statusLabel->setText(
			QStringLiteral("Failed to create MP3 encoder"));
		return;
	}
	obs_encoder_set_audio(encoder, obs_get_audio());

	obs_data_t *os = buildOutputSettings();
	output = obs_output_create("shoutcast_output", "Radio.co Output", os,
				   nullptr);
	obs_data_release(os);

	if (!output) {
		statusLabel->setText(
			QStringLiteral("Failed to create output"));
		releaseOutput();
		return;
	}

	signal_handler_t *sh = obs_output_get_signal_handler(output);
	signal_handler_connect(sh, "start", handle_start, this);
	signal_handler_connect(sh, "stop", handle_stop, this);
	signal_handler_connect(sh, "reconnect", handle_reconnect, this);
	signal_handler_connect(sh, "reconnect_success",
			       handle_reconnect_success, this);

	/* butt reconnected indefinitely; 20 attempts at 1 s matches it. */
	obs_output_set_reconnect_settings(output, 20, 1);
	obs_output_set_audio_encoder(output, encoder, 0);

	autoStarted = asAutoStart;
	connectedName = stationEdit->text();
	statusLabel->setText(QStringLiteral("Connecting…"));

	if (!obs_output_start(output)) {
		const char *err = obs_output_get_last_error(output);
		statusLabel->setText(err && *err
					     ? QString::fromUtf8(err)
					     : QStringLiteral("Failed to start"));
		releaseOutput();
	}

	refreshControls();
}

void RadioCoDock::stopOutput()
{
	if (!output)
		return;
	obs_output_stop(output);
	statusLabel->setText(QStringLiteral("Disconnecting…"));
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
}

/* ------------------------------------------------------------------ */

void RadioCoDock::onConnectClicked()
{
	if (output)
		stopOutput();
	else
		startOutput(false);
}

void RadioCoDock::onUpdateClicked()
{
	if (!output)
		return;

	const QString song = nowPlayingEdit->text();
	if (song.isEmpty())
		return;

	obs_data_t *d = obs_data_create();
	obs_data_set_string(d, "song", song.toUtf8().constData());
	obs_output_update(output, d);
	obs_data_release(d);
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
	statusLabel->setText(QStringLiteral("Applying name…"));
}

void RadioCoDock::onFieldChanged()
{
	if (loading)
		return;
	saveSettings();
	refreshControls();
}

void RadioCoDock::onTick()
{
	if (shuttingDown)
		return;

	if (output && obs_output_active(output)) {
		const qint64 secs = liveTimer.isValid()
					    ? liveTimer.elapsed() / 1000
					    : 0;
		statusLabel->setText(
			QStringLiteral("● Live %1:%2:%3")
				.arg(secs / 3600, 2, 10, QLatin1Char('0'))
				.arg((secs / 60) % 60, 2, 10, QLatin1Char('0'))
				.arg(secs % 60, 2, 10, QLatin1Char('0')));
	}
	refreshControls();
}

/* ------------------------------------------------------------------ */

void RadioCoDock::onOutputStarted()
{
	liveTimer.start();
	connectedName = stationEdit->text();
	statusLabel->setText(QStringLiteral("● Live 00:00:00"));
	refreshControls();
}

void RadioCoDock::onOutputStopped(int code)
{
	const char *err = output ? obs_output_get_last_error(output) : nullptr;
	const bool wasAuto = autoStarted;

	releaseOutput();
	liveTimer.invalidate();

	if (err && *err)
		statusLabel->setText(QString::fromUtf8(err));
	else if (code != OBS_OUTPUT_SUCCESS)
		statusLabel->setText(
			QStringLiteral("Disconnected (code %1)").arg(code));
	else
		statusLabel->setText(QStringLiteral("Idle"));

	if (pendingRestart && !shuttingDown) {
		pendingRestart = false;
		startOutput(wasAuto);
		return;
	}

	refreshControls();
}

void RadioCoDock::onOutputReconnecting()
{
	statusLabel->setText(QStringLiteral("Reconnecting…"));
}

void RadioCoDock::onOutputReconnected()
{
	liveTimer.start();
	statusLabel->setText(QStringLiteral("● Live 00:00:00"));
}

/* ------------------------------------------------------------------ */

void RadioCoDock::refreshControls()
{
	const bool active = output != nullptr;

	connectButton->setText(active ? QStringLiteral("Disconnect")
				      : QStringLiteral("Connect"));
	updateButton->setEnabled(active && !nowPlayingEdit->text().isEmpty());

	const bool nameChanged =
		active && stationEdit->text() != connectedName;
	applyNameButton->setEnabled(nameChanged);
	applyNameButton->setToolTip(
		nameChanged
			? QStringLiteral("Reconnects the stream (brief dropout)")
			: QStringLiteral(
				  "The station name is sent when connecting"));
}

void RadioCoDock::handleFrontendEvent(obs_frontend_event event)
{
	switch (event) {
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
	obs_data_set_default_string(s, "song", "");
	obs_data_set_default_bool(s, "follow_obs", false);

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

	nowPlayingEdit->setText(
		QString::fromUtf8(obs_data_get_string(s, "song")));
	followObsBox->setChecked(obs_data_get_bool(s, "follow_obs"));

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
	obs_data_set_string(s, "song",
			    nowPlayingEdit->text().toUtf8().constData());
	obs_data_set_bool(s, "follow_obs", followObsBox->isChecked());

	obs_data_save_json_safe(s, path, "tmp", "bak");

	obs_data_release(s);
	bfree(path);
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
		blog(LOG_ERROR, "[obs-shoutcast] failed to register dock");
		/* Registration failed, so nothing reparented it — we own it. */
		delete dock;
		dock = nullptr;
		return;
	}

	obs_frontend_add_event_callback(frontend_event, dock);
	blog(LOG_INFO, "[obs-shoutcast] " DOCK_TITLE " dock registered");
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
