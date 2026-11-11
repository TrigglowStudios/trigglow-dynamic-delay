/*
Trigglow Dynamic Delay for OBS
Copyright (C) 2026 Trigglow (VirosMs)

This program is free software; you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation; either version 2 of the License, or
(at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License along
with this program. If not, see <https://www.gnu.org/licenses/>
*/

#include "stream-mode-panel.hpp"

#include <QCheckBox>
#include <QComboBox>
#include <QFormLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QSpinBox>
#include <QTableWidget>
#include <QTimer>
#include <QVBoxLayout>

#include "auth-manager.hpp"
#include "i18n.hpp"
#include "packet-delay-core.hpp"
#include "stream-delay-controller.hpp"

namespace trigglow {

namespace {

QString Tr(const char *key)
{
	return QString::fromUtf8(Str(key));
}

QString SinkStateText(relay::SinkState state)
{
	switch (state) {
	case relay::SinkState::Waiting:
		return Tr("Stream.SinkState.Waiting");
	case relay::SinkState::Connecting:
		return Tr("Stream.SinkState.Connecting");
	case relay::SinkState::Live:
		return Tr("Stream.SinkState.Live");
	case relay::SinkState::Reconnecting:
		return Tr("Stream.SinkState.Reconnecting");
	case relay::SinkState::Failed:
		return Tr("Stream.SinkState.Failed");
	case relay::SinkState::Done:
		return Tr("Stream.SinkState.Done");
	}
	return {};
}

QString Megabytes(uint64_t bytes)
{
	return QString::number(static_cast<double>(bytes) / (1024.0 * 1024.0), 'f', 1);
}

} // namespace

StreamModePanel::StreamModePanel(StreamDelayController &controller, AuthManager &auth, QWidget *parent)
	: QWidget(parent),
	  controller_(controller),
	  auth_(auth)
{
	BuildUi();
	timer_ = new QTimer(this);
	timer_->setInterval(1000);
	connect(timer_, &QTimer::timeout, this, &StreamModePanel::Refresh);
	timer_->start();
	Refresh();
}

void StreamModePanel::SetHandlesControlsChanged(std::function<void(bool)> callback)
{
	onHandlesChanged_ = std::move(callback);
	haveLastHandles_ = false;
	Refresh();
}

void StreamModePanel::BuildUi()
{
	auto *root = new QVBoxLayout(this);
	root->setContentsMargins(8, 8, 8, 8);
	root->setSpacing(6);

	auto *title = new QLabel(Tr("Stream.Title"), this);
	QFont titleFont = title->font();
	titleFont.setBold(true);
	titleFont.setPointSizeF(titleFont.pointSizeF() * 1.15);
	title->setFont(titleFont);
	root->addWidget(title);

	modeCheck_ = new QCheckBox(Tr("Stream.Use"), this);
	root->addWidget(modeCheck_);
	connect(modeCheck_, &QCheckBox::toggled, this, [this](bool on) {
		if (refreshing_)
			return;
		controller_.SetModeEnabled(on);
		SettingsChanged();
		Refresh();
	});

	controlsBox_ = new QWidget(this);
	auto *controls = new QVBoxLayout(controlsBox_);
	controls->setContentsMargins(0, 0, 0, 0);
	controls->setSpacing(6);
	root->addWidget(controlsBox_);

	stateLabel_ = new QLabel(controlsBox_);
	QFont stateFont = stateLabel_->font();
	stateFont.setBold(true);
	stateLabel_->setFont(stateFont);
	stateLabel_->setWordWrap(true);
	controls->addWidget(stateLabel_);
	detailLabel_ = new QLabel(controlsBox_);
	detailLabel_->setWordWrap(true);
	controls->addWidget(detailLabel_);

	auto *buttons = new QHBoxLayout();
	toggleButton_ = new QPushButton(controlsBox_);
	liveButton_ = new QPushButton(Tr("Stream.SkipLive"), controlsBox_);
	buttons->addWidget(toggleButton_, 2);
	buttons->addWidget(liveButton_, 1);
	controls->addLayout(buttons);
	connect(toggleButton_, &QPushButton::clicked, this, [this] {
		controller_.Toggle();
		Refresh();
	});
	connect(liveButton_, &QPushButton::clicked, this, [this] {
		controller_.Disable();
		Refresh();
	});

	cutButton_ = new QPushButton(Tr("Stream.Cut"), controlsBox_);
	controls->addWidget(cutButton_);
	connect(cutButton_, &QPushButton::clicked, this, [this] {
		controller_.CutDrain();
		Refresh();
	});

	auto *delayRow = new QHBoxLayout();
	delayRow->addWidget(new QLabel(Tr("Stream.Delay"), controlsBox_));
	delaySpin_ = new QSpinBox(controlsBox_);
	delaySpin_->setRange(1, static_cast<int>(relay::kMaxDelaySeconds));
	delaySpin_->setSuffix(Tr("Stream.Seconds"));
	delaySpin_->setKeyboardTracking(false);
	delayRow->addWidget(delaySpin_, 1);
	connect(delaySpin_, qOverload<int>(&QSpinBox::valueChanged), this, [this](int value) {
		if (refreshing_)
			return;
		controller_.SetDelaySeconds(static_cast<uint32_t>(value));
		SettingsChanged();
	});
	auto addStep = [this, delayRow](const QString &text, int delta) {
		auto *button = new QPushButton(text, controlsBox_);
		button->setMaximumWidth(48);
		delayRow->addWidget(button);
		connect(button, &QPushButton::clicked, this, [this, delta] {
			controller_.AdjustDelay(delta);
			SettingsChanged();
			Refresh();
		});
	};
	addStep(QStringLiteral("-5"), -5);
	addStep(QStringLiteral("+5"), 5);
	controls->addLayout(delayRow);

	auto *presets = new QHBoxLayout();
	for (int seconds : {10, 30, 60, 120, 300}) {
		auto *button = new QPushButton(QString::number(seconds) + Tr("Stream.Seconds"), controlsBox_);
		presets->addWidget(button);
		connect(button, &QPushButton::clicked, this, [this, seconds] {
			controller_.SetDelaySeconds(static_cast<uint32_t>(seconds));
			SettingsChanged();
			Refresh();
		});
	}
	controls->addLayout(presets);

	statsLabel_ = new QLabel(controlsBox_);
	statsLabel_->setWordWrap(true);
	controls->addWidget(statsLabel_);
	warningLabel_ = new QLabel(controlsBox_);
	warningLabel_->setWordWrap(true);
	controls->addWidget(warningLabel_);

	auto *accountRow = new QHBoxLayout();
	accountLabel_ = new QLabel(controlsBox_);
	accountLabel_->setWordWrap(true);
	signInButton_ = new QPushButton(Tr("Stream.Account.SignIn"), controlsBox_);
	accountRow->addWidget(accountLabel_, 1);
	accountRow->addWidget(signInButton_);
	controls->addLayout(accountRow);
	connect(signInButton_, &QPushButton::clicked, this, [this] {
		auth_.StartLogin();
		Refresh();
	});

	// Destinations (multistream).
	auto *destBox = new QGroupBox(Tr("Stream.Destinations"), controlsBox_);
	auto *destLayout = new QVBoxLayout(destBox);
	auto *destHelp = new QLabel(Tr("Stream.Dest.Help"), destBox);
	destHelp->setWordWrap(true);
	destLayout->addWidget(destHelp);
	destTable_ = new QTableWidget(0, 3, destBox);
	destTable_->setHorizontalHeaderLabels(
		{Tr("Stream.Col.Destination"), Tr("Stream.Col.State"), Tr("Stream.Col.Sent")});
	destTable_->horizontalHeader()->setSectionResizeMode(0, QHeaderView::Stretch);
	destTable_->verticalHeader()->setVisible(false);
	destTable_->setSelectionBehavior(QAbstractItemView::SelectRows);
	destTable_->setSelectionMode(QAbstractItemView::SingleSelection);
	destTable_->setEditTriggers(QAbstractItemView::NoEditTriggers);
	destTable_->setMinimumHeight(90);
	destLayout->addWidget(destTable_);
	auto *form = new QFormLayout();
	destName_ = new QLineEdit(destBox);
	destServer_ = new QLineEdit(destBox);
	destServer_->setPlaceholderText(QStringLiteral("rtmps://..."));
	destKey_ = new QLineEdit(destBox);
	destKey_->setEchoMode(QLineEdit::Password);
	form->addRow(Tr("Stream.Dest.Name"), destName_);
	form->addRow(Tr("Stream.Dest.Server"), destServer_);
	form->addRow(Tr("Stream.Dest.Key"), destKey_);
	destLayout->addLayout(form);
	auto *destButtons = new QHBoxLayout();
	destAdd_ = new QPushButton(Tr("Stream.Dest.Add"), destBox);
	destRemove_ = new QPushButton(Tr("Stream.Dest.Remove"), destBox);
	destButtons->addWidget(destAdd_);
	destButtons->addWidget(destRemove_);
	destLayout->addLayout(destButtons);
	connect(destAdd_, &QPushButton::clicked, this, &StreamModePanel::AddDestination);
	connect(destRemove_, &QPushButton::clicked, this, &StreamModePanel::RemoveSelectedDestination);
	controls->addWidget(destBox);

	// Options.
	auto *keepRow = new QHBoxLayout();
	keepRow->addWidget(new QLabel(Tr("Stream.Keep"), controlsBox_));
	keepSpin_ = new QSpinBox(controlsBox_);
	keepSpin_->setRange(10, static_cast<int>(relay::kMaxDelaySeconds));
	keepSpin_->setSuffix(Tr("Stream.Seconds"));
	keepSpin_->setToolTip(Tr("Stream.KeepHelp"));
	keepSpin_->setKeyboardTracking(false);
	keepRow->addWidget(keepSpin_, 1);
	controls->addLayout(keepRow);
	connect(keepSpin_, qOverload<int>(&QSpinBox::valueChanged), this, [this](int value) {
		if (refreshing_)
			return;
		controller_.SetKeepSeconds(static_cast<uint32_t>(value));
		SettingsChanged();
	});
	drainCheck_ = new QCheckBox(Tr("Stream.Drain"), controlsBox_);
	controls->addWidget(drainCheck_);
	connect(drainCheck_, &QCheckBox::toggled, this, [this](bool on) {
		if (refreshing_)
			return;
		controller_.SetDrainOnStop(on);
		SettingsChanged();
	});

	auto *overlayRow = new QHBoxLayout();
	overlayCheck_ = new QCheckBox(Tr("Stream.Overlay"), controlsBox_);
	cornerCombo_ = new QComboBox(controlsBox_);
	cornerCombo_->addItems({Tr("Stream.Corner.TopLeft"), Tr("Stream.Corner.TopRight"),
				Tr("Stream.Corner.BottomLeft"), Tr("Stream.Corner.BottomRight")});
	overlayRow->addWidget(overlayCheck_, 1);
	overlayRow->addWidget(cornerCombo_);
	controls->addLayout(overlayRow);
	connect(overlayCheck_, &QCheckBox::toggled, this, [this](bool on) {
		if (refreshing_)
			return;
		controller_.SetShowOverlay(on);
		SettingsChanged();
	});
	connect(cornerCombo_, qOverload<int>(&QComboBox::currentIndexChanged), this, [this](int index) {
		if (refreshing_ || index < 0)
			return;
		controller_.SetOverlayCorner(static_cast<uint32_t>(index));
		SettingsChanged();
	});

	settingsWarnings_ = new QLabel(controlsBox_);
	settingsWarnings_->setWordWrap(true);
	controls->addWidget(settingsWarnings_);
}

void StreamModePanel::AddDestination()
{
	const QString server = destServer_->text().trimmed();
	if (!(server.startsWith(QStringLiteral("rtmp://")) || server.startsWith(QStringLiteral("rtmps://")))) {
		destServer_->setFocus();
		return;
	}
	std::vector<ExtraDestination> extras = controller_.ExtraDestinations();
	ExtraDestination extra;
	extra.name = destName_->text().trimmed().toStdString();
	extra.server = server.toStdString();
	extra.key = destKey_->text().trimmed().toStdString();
	extras.push_back(extra);
	controller_.SetExtraDestinations(std::move(extras));
	destName_->clear();
	destServer_->clear();
	destKey_->clear();
	SettingsChanged();
	RefreshDestinations();
}

void StreamModePanel::RemoveSelectedDestination()
{
	const int row = destTable_->currentRow();
	// Row 0 is always the streamer's own OBS service.
	if (row <= 0)
		return;
	std::vector<ExtraDestination> extras = controller_.ExtraDestinations();
	const size_t index = static_cast<size_t>(row - 1);
	if (index >= extras.size())
		return;
	extras.erase(extras.begin() + static_cast<long>(index));
	controller_.SetExtraDestinations(std::move(extras));
	SettingsChanged();
	RefreshDestinations();
}

void StreamModePanel::SettingsChanged()
{
	if (onSettingsChanged_)
		onSettingsChanged_();
}

void StreamModePanel::RefreshDestinations()
{
	const StreamDelayStatus status = controller_.GetStatus();
	const auto &extras = controller_.ExtraDestinations();
	destTable_->setRowCount(static_cast<int>(1 + extras.size()));
	auto setRow = [this](int row, const QString &name, const QString &state, const QString &sent) {
		destTable_->setItem(row, 0, new QTableWidgetItem(name));
		destTable_->setItem(row, 1, new QTableWidgetItem(state));
		destTable_->setItem(row, 2, new QTableWidgetItem(sent));
	};
	const bool live = status.engaged || status.state == StreamDelayState::Draining;
	if (live && !status.relay.sinks.empty()) {
		destTable_->setRowCount(static_cast<int>(status.relay.sinks.size()));
		for (size_t i = 0; i < status.relay.sinks.size(); ++i) {
			const relay::SinkStatus &sink = status.relay.sinks[i];
			QString state = SinkStateText(sink.state);
			if (!sink.error.empty() && sink.state != relay::SinkState::Live)
				state +=
					QStringLiteral(" (") + QString::fromStdString(sink.error) + QStringLiteral(")");
			const QString sent = sink.state == relay::SinkState::Live
						     ? QString::number(sink.sentMbps, 'f', 1) + QStringLiteral(" Mbps")
						     : Megabytes(sink.bytesSent) + QStringLiteral(" MB");
			setRow(static_cast<int>(i), QString::fromStdString(sink.name), state, sent);
		}
		return;
	}
	setRow(0, Tr("Stream.Dest.Primary"), QString(), QString());
	for (size_t i = 0; i < extras.size(); ++i) {
		const ExtraDestination &extra = extras[i];
		const QString name = extra.name.empty() ? QString::fromStdString(relay::HostOf(extra.server))
							: QString::fromStdString(extra.name);
		setRow(static_cast<int>(i + 1), name, QString(), QString());
	}
}

void StreamModePanel::Detach()
{
	detached_ = true;
	if (timer_)
		timer_->stop();
	setEnabled(false);
}

void StreamModePanel::Refresh()
{
	if (detached_)
		return;
	refreshing_ = true;
	const StreamDelayStatus status = controller_.GetStatus();

	modeCheck_->setEnabled(status.available);
	modeCheck_->setChecked(status.available && status.modeEnabled);
	controlsBox_->setVisible(status.available && status.modeEnabled);
	if (delaySpin_->value() != static_cast<int>(status.delaySeconds))
		delaySpin_->setValue(static_cast<int>(status.delaySeconds));
	if (keepSpin_->value() != static_cast<int>(status.keepSeconds))
		keepSpin_->setValue(static_cast<int>(status.keepSeconds));
	drainCheck_->setChecked(status.drainOnStop);

	QString state;
	QString detail;
	switch (status.state) {
	case StreamDelayState::Idle:
		state = status.delayOn ? Tr("Stream.State.IdleArmed").arg(status.delaySeconds)
				       : Tr("Stream.State.Idle");
		detail = Tr("Stream.Detail.Idle");
		break;
	case StreamDelayState::Live:
		state = Tr("Stream.State.Live");
		detail = Tr("Stream.Detail.Live");
		break;
	case StreamDelayState::Delayed:
		state = Tr("Stream.State.Delayed").arg(status.relay.delaySeconds);
		detail = Tr("Stream.Detail.Delayed");
		break;
	case StreamDelayState::Draining:
		state = Tr("Stream.State.Draining").arg(status.relay.delaySeconds);
		detail = Tr("Stream.Detail.Draining");
		break;
	}
	if (!status.available) {
		state = Tr("Stream.Unavailable");
		detail.clear();
	} else if (!status.ineligible.empty() && status.state == StreamDelayState::Idle) {
		const std::string key = "Stream.Ineligible." + status.ineligible;
		detail = Tr(key.c_str());
	}
	stateLabel_->setText(state);
	detailLabel_->setText(detail);

	toggleButton_->setText(status.delayOn ? Tr("Stream.Disable") : Tr("Stream.Enable"));
	liveButton_->setVisible(status.delayOn && status.engaged);
	cutButton_->setVisible(status.state == StreamDelayState::Draining);

	QString stats;
	QString warning;
	if (status.engaged || status.state == StreamDelayState::Draining) {
		const relay::RelayStatus &r = status.relay;
		QString video = QString::fromStdString(r.videoCodec);
		if (r.width > 0)
			video += QStringLiteral(" %1x%2").arg(r.width).arg(r.height);
		stats = Tr("Stream.Stats")
				.arg(QString::number(r.historySeconds, 'f', 0))
				.arg(Megabytes(r.historyBytes))
				.arg(QString::number(r.bitrateMbps, 'f', 1))
				.arg(video);
		if (r.keyframeIntervalSeconds > 4.5)
			warning = Tr("Stream.KeyframeWarning").arg(QString::number(r.keyframeIntervalSeconds, 'f', 1));
	}
	if (!status.lastEvent.empty())
		warning += (warning.isEmpty() ? QString() : QStringLiteral("\n")) +
			   Tr("Stream.LastEvent").arg(QString::fromStdString(status.lastEvent));
	statsLabel_->setText(stats);
	statsLabel_->setVisible(!stats.isEmpty());
	warningLabel_->setText(warning);
	warningLabel_->setVisible(!warning.isEmpty());

	overlayCheck_->setChecked(status.showOverlay);
	cornerCombo_->setCurrentIndex(static_cast<int>(status.overlayCorner));
	cornerCombo_->setEnabled(status.showOverlay);
	QStringList settingsText;
	for (const std::string &w : status.warnings) {
		const size_t colon = w.find(':');
		const std::string key = "Stream.Warn." + w.substr(0, colon);
		QString text = Tr(key.c_str());
		if (colon != std::string::npos)
			text = text.arg(QString::fromStdString(w.substr(colon + 1)));
		settingsText << text;
	}
	settingsWarnings_->setText(settingsText.join(QStringLiteral("\n")));
	settingsWarnings_->setVisible(!settingsText.isEmpty());

	if (auth_.CanEnable()) {
		accountLabel_->setText(Tr("Stream.Account.SignedIn").arg(QString::fromStdString(auth_.DisplayName())));
		signInButton_->setVisible(false);
	} else if (auth_.IsLoggingIn() && !auth_.PendingUserCode().empty()) {
		accountLabel_->setText(
			Tr("Stream.Account.Pending").arg(QString::fromStdString(auth_.PendingUserCode())));
		signInButton_->setVisible(false);
	} else {
		accountLabel_->setText(Tr("Stream.Account.Required"));
		signInButton_->setVisible(!auth_.IsLoggingIn());
	}

	RefreshDestinations();
	refreshing_ = false;

	const bool handles = controller_.HandlesControls();
	if (!haveLastHandles_ || handles != lastHandles_) {
		lastHandles_ = handles;
		haveLastHandles_ = true;
		if (onHandlesChanged_)
			onHandlesChanged_(handles);
	}
}

} // namespace trigglow
