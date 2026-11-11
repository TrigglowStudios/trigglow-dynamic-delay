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

#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QEvent>
#include <QFrame>
#include <QHBoxLayout>
#include <QLabel>
#include <QMouseEvent>
#include <QPushButton>
#include <QResizeEvent>
#include <QSignalBlocker>
#include <QSpinBox>
#include <QTimer>
#include <QVBoxLayout>

#include <algorithm>
#include <cmath>

extern "C" {
#include <obs-frontend-api.h>
}

#include "auth-manager.hpp"
#include "i18n.hpp"
#include "packet-delay-core.hpp"
#include "stream-delay-controller.hpp"

namespace trigglow {

namespace {

// Same sizes as scene mode's dock (settings-ui.cpp).
constexpr int kStatusIconSize = 18;
constexpr int kAccountIconSize = 16;
constexpr int kInlineIconSize = 14;
constexpr int kButtonIconSize = 15;
constexpr int kButtonIconGap = 7;
const QSize kButtonIconBox(kButtonIconSize + kButtonIconGap, kButtonIconSize);
const QSize kSectionIconBox(kInlineIconSize + 6, kInlineIconSize);
// Rows side by side reflow to a column below these dock widths.
constexpr int kTinyWidth = 250;
constexpr int kNarrowWidth = 300;

QString Tr(const char *key)
{
	return QString::fromUtf8(Str(key));
}

QLabel *MakeIconLabel(QWidget *parent, int size)
{
	auto *label = new QLabel(parent);
	label->setFixedSize(size, size);
	label->setAlignment(Qt::AlignCenter);
	return label;
}

QFrame *MakeDivider(QWidget *parent)
{
	auto *divider = new QFrame(parent);
	divider->setProperty("tgDivider", QStringLiteral("true"));
	divider->setFixedHeight(1);
	return divider;
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

QString SinkStateTone(relay::SinkState state)
{
	switch (state) {
	case relay::SinkState::Live:
		return QStringLiteral("accent");
	case relay::SinkState::Connecting:
	case relay::SinkState::Reconnecting:
	case relay::SinkState::Waiting:
		return QStringLiteral("warning");
	case relay::SinkState::Failed:
		return QStringLiteral("danger");
	case relay::SinkState::Done:
		break;
	}
	return QStringLiteral("muted");
}

QString Megabytes(uint64_t bytes)
{
	return QString::number(static_cast<double>(bytes) / (1024.0 * 1024.0), 'f', 1);
}

// "font-family: ...; font-size: ...; font-weight: ...;" for the panel-only
// rules, with the same sizes ui-theme.cpp gives the role.
QString FontCss(const QString &family, double basePt, double delta, int weight)
{
	QString css;
	if (!family.isEmpty())
		css += QStringLiteral("font-family: \"%1\"; ").arg(family);
	const double size = std::max(7.5, std::round((basePt + delta) * 2.0) / 2.0);
	css += QStringLiteral("font-size: %1pt; font-weight: %2;").arg(size, 0, 'f', 1).arg(weight);
	return css;
}

// The streamer's OBS service as Settings > Stream names it: the service
// ("Twitch") or, for a custom server, its host.
QString ObsServiceName()
{
	obs_service_t *service = obs_frontend_get_streaming_service(); // Not a new reference.
	if (!service)
		return {};
	obs_data_t *settings = obs_service_get_settings(service);
	QString name = QString::fromUtf8(obs_data_get_string(settings, "service"));
	if (name.isEmpty())
		name = QString::fromStdString(relay::HostOf(obs_data_get_string(settings, "server")));
	obs_data_release(settings);
	return name;
}

void OnFrontendEvent(enum obs_frontend_event event, void *data)
{
	if (event == OBS_FRONTEND_EVENT_THEME_CHANGED)
		static_cast<StreamModePanel *>(data)->ApplyTheme();
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
	obs_frontend_add_event_callback(OnFrontendEvent, this);
	frontendCallback_ = true;
	ApplyTheme(/*force=*/true); // Styles everything, then Refresh().
}

StreamModePanel::~StreamModePanel()
{
	if (frontendCallback_)
		obs_frontend_remove_event_callback(OnFrontendEvent, this);
}

void StreamModePanel::SetHandlesControlsChanged(std::function<void(bool)> callback)
{
	onHandlesChanged_ = std::move(callback);
	haveLastHandles_ = false;
	Refresh();
}

void StreamModePanel::SetRole(QWidget *widget, const char *qssRole, ui::FontRole fontRole)
{
	widget->setProperty("tgRole", QString::fromLatin1(qssRole));
	fontRoles_.emplace_back(widget, fontRole);
}

void StreamModePanel::GuardWheel(QWidget *field)
{
	field->setFocusPolicy(Qt::StrongFocus);
	field->installEventFilter(this);
}

// ---- Building --------------------------------------------------------------

void StreamModePanel::BuildUi()
{
	auto *root = new QVBoxLayout(this);
	root->setContentsMargins(12, 12, 12, 12);
	root->setSpacing(10);

	// --- Mode card: the switch between stream mode and scene mode, and which
	// of the two the hotkeys and Stream Deck drive right now. ---
	auto *modeCard = new QFrame(this);
	modeCard->setProperty("tgCard", QStringLiteral("true"));
	{
		auto *layout = new QVBoxLayout(modeCard);
		layout->setContentsMargins(12, 10, 12, 10);
		layout->setSpacing(6);
		auto *eyebrow = new QLabel(Tr("Stream.Title"), modeCard);
		SetRole(eyebrow, "eyebrow", ui::FontRole::Eyebrow);
		layout->addWidget(eyebrow);

		modeCheck_ = new QCheckBox(modeCard);
		modeCheck_->setFocusPolicy(Qt::TabFocus);
		modeCheck_->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);
		modeCheck_->setAccessibleName(Tr("Stream.Use"));
		// The check box's text as a wrapping label: a QCheckBox can't wrap.
		modeText_ = new QLabel(Tr("Stream.Use"), modeCard);
		modeText_->setWordWrap(true);
		modeText_->setBuddy(modeCheck_);
		modeText_->setCursor(Qt::PointingHandCursor);
		modeText_->installEventFilter(this);
		auto *checkRow = new QHBoxLayout();
		checkRow->setSpacing(8);
		checkRow->addWidget(modeCheck_, 0, Qt::AlignTop);
		checkRow->addWidget(modeText_, 1);
		layout->addLayout(checkRow);

		auto *hotkeysRow = new QHBoxLayout();
		hotkeysRow->setSpacing(6);
		hotkeysIcon_ = MakeIconLabel(modeCard, kInlineIconSize);
		hotkeysLabel_ = new QLabel(modeCard);
		hotkeysLabel_->setWordWrap(true);
		SetRole(hotkeysLabel_, "hint", ui::FontRole::Hint);
		hotkeysRow->addWidget(hotkeysIcon_, 0, Qt::AlignTop);
		hotkeysRow->addWidget(hotkeysLabel_, 1);
		layout->addLayout(hotkeysRow);
	}
	root->addWidget(modeCard);
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
	controls->setSpacing(10);
	root->addWidget(controlsBox_);

	controls->addWidget(BuildStatusCard());
	controls->addWidget(BuildAccountCard());

	// --- Actions: one primary button (on, or off in the danger style), then
	// Back to live and Dump while the delay runs, Stop sending while draining. ---
	onButton_ = new QPushButton(Tr("Stream.Enable"), controlsBox_);
	onButton_->setProperty("tgVariant", QStringLiteral("primary"));
	offButton_ = new QPushButton(Tr("Stream.Disable"), controlsBox_);
	offButton_->setProperty("tgVariant", QStringLiteral("danger"));
	liveButton_ = new QPushButton(Tr("Stream.SkipLive"), controlsBox_);
	liveButton_->setToolTip(Tr("Stream.SkipLiveHelp"));
	liveButton_->setAccessibleDescription(Tr("Stream.SkipLiveHelp"));
	dumpButton_ = new QPushButton(Tr("Stream.DumpShort"), controlsBox_);
	dumpButton_->setToolTip(Tr("Stream.DumpHelp"));
	dumpButton_->setAccessibleName(Tr("Stream.Dump"));
	dumpButton_->setAccessibleDescription(Tr("Stream.DumpHelp"));
	cutButton_ = new QPushButton(Tr("Stream.Cut"), controlsBox_);
	cutButton_->setToolTip(Tr("Stream.CutHelp"));
	cutButton_->setAccessibleDescription(Tr("Stream.CutHelp"));
	for (QPushButton *button : {liveButton_, dumpButton_, cutButton_}) {
		button->setProperty("tgVariant", QStringLiteral("secondary"));
		button->setProperty("tgSize", QStringLiteral("large"));
	}
	for (QPushButton *button : {onButton_, offButton_, liveButton_, dumpButton_, cutButton_}) {
		button->setFocusPolicy(Qt::TabFocus);
		button->setIconSize(kButtonIconBox);
		SetRole(button, "button", ui::FontRole::Button);
	}
	controls->addWidget(onButton_);
	controls->addWidget(offButton_);
	actionRow_ = new QBoxLayout(QBoxLayout::LeftToRight);
	actionRow_->setSpacing(8);
	actionRow_->addWidget(liveButton_, 1);
	actionRow_->addWidget(dumpButton_, 1);
	controls->addLayout(actionRow_);
	controls->addWidget(cutButton_);
	connect(onButton_, &QPushButton::clicked, this, [this] {
		controller_.Enable();
		Refresh();
	});
	connect(offButton_, &QPushButton::clicked, this, [this] {
		controller_.Disable();
		Refresh();
	});
	connect(liveButton_, &QPushButton::clicked, this, [this] {
		controller_.Disable();
		Refresh();
	});
	connect(dumpButton_, &QPushButton::clicked, this, [this] {
		controller_.Dump();
		Refresh();
	});
	connect(cutButton_, &QPushButton::clicked, this, [this] {
		controller_.CutDrain();
		Refresh();
	});

	controls->addWidget(BuildDelayCard());

	// --- Settings worth changing in OBS: their own amber card, opened the
	// first time something shows up. ---
	auto *warningsCard = new QFrame(controlsBox_);
	warningsCard->setProperty("tgBanner", QStringLiteral("warning"));
	{
		auto *layout = new QVBoxLayout(warningsCard);
		layout->setContentsMargins(6, 6, 6, 6);
		layout->setSpacing(0);
		warningsSection_ = AddSection(layout, Tr("Stream.Section.Warnings"), true);
		warningsSection_->header->setProperty("tgTone", QStringLiteral("warning"));
		BuildWarnings(qobject_cast<QVBoxLayout *>(warningsSection_->body->layout()));
	}
	warningsList_ = warningsCard;
	warningsCard->setVisible(false);
	controls->addWidget(warningsCard);

	// --- Everything else, folded: one card, a section each. ---
	auto *moreCard = new QFrame(controlsBox_);
	moreCard->setProperty("tgCard", QStringLiteral("true"));
	{
		auto *layout = new QVBoxLayout(moreCard);
		layout->setContentsMargins(6, 6, 6, 6);
		layout->setSpacing(0);
		rulesSection_ = AddSection(layout, Tr("Stream.Rules"), false);
		BuildRules(qobject_cast<QVBoxLayout *>(rulesSection_->body->layout()));
		layout->addWidget(MakeDivider(moreCard));
		Section *presets = AddSection(layout, Tr("Stream.Section.Presets"), false);
		BuildPresets(qobject_cast<QVBoxLayout *>(presets->body->layout()));
		layout->addWidget(MakeDivider(moreCard));
		Section *options = AddSection(layout, Tr("Stream.Section.Options"), false);
		BuildOptions(qobject_cast<QVBoxLayout *>(options->body->layout()));
	}
	controls->addWidget(moreCard);

	root->addStretch(1);
}

QFrame *StreamModePanel::BuildStatusCard()
{
	// Rule, eyebrow + seconds chip, icon + headline, detail, stats, and a
	// line each for the last problem and the scene rule applied.
	statusCard_ = new QFrame(controlsBox_);
	statusCard_->setProperty("tgCard", QStringLiteral("true"));
	auto *cardLayout = new QVBoxLayout(statusCard_);
	cardLayout->setContentsMargins(0, 0, 0, 0);
	cardLayout->setSpacing(0);
	statusRule_ = new QFrame(statusCard_);
	statusRule_->setFixedHeight(2);
	statusRule_->setProperty("tgRule", QStringLiteral("none"));
	cardLayout->addWidget(statusRule_);

	auto *body = new QVBoxLayout();
	body->setContentsMargins(12, 11, 12, 12);
	body->setSpacing(4);
	auto *eyebrowRow = new QHBoxLayout();
	eyebrowRow->setSpacing(6);
	auto *eyebrow = new QLabel(Tr("Dock.Status.Eyebrow"), statusCard_);
	SetRole(eyebrow, "eyebrow", ui::FontRole::Eyebrow);
	eyebrowRow->addWidget(eyebrow);
	eyebrowRow->addStretch(1);
	statusChip_ = new QLabel(statusCard_);
	SetRole(statusChip_, "chip", ui::FontRole::Chip);
	statusChip_->setVisible(false);
	eyebrowRow->addWidget(statusChip_);
	body->addLayout(eyebrowRow);

	auto *headlineRow = new QHBoxLayout();
	headlineRow->setSpacing(8);
	statusIcon_ = MakeIconLabel(statusCard_, kStatusIconSize);
	stateLabel_ = new QLabel(statusCard_);
	stateLabel_->setWordWrap(true);
	SetRole(stateLabel_, "headline", ui::FontRole::Headline);
	headlineRow->addWidget(statusIcon_, 0, Qt::AlignVCenter);
	headlineRow->addWidget(stateLabel_, 1);
	body->addLayout(headlineRow);

	detailLabel_ = new QLabel(statusCard_);
	detailLabel_->setWordWrap(true);
	detailLabel_->setProperty("tgRole", QStringLiteral("muted"));
	body->addWidget(detailLabel_);

	// Where the stream goes: read only, the streamer's own OBS service, with
	// the relay's state and rate while it runs.
	sendsRow_ = new QWidget(statusCard_);
	{
		auto *layout = new QHBoxLayout(sendsRow_);
		layout->setContentsMargins(0, 4, 0, 0);
		layout->setSpacing(8);
		sendsLabel_ = new QLabel(sendsRow_);
		sendsLabel_->setWordWrap(true);
		sendsLabel_->setToolTip(Tr("Stream.SendsToHelp"));
		SetRole(sendsLabel_, "hint", ui::FontRole::Hint);
		sendsState_ = new QLabel(sendsRow_);
		SetRole(sendsState_, "chip", ui::FontRole::Chip);
		sendsState_->setVisible(false);
		layout->addWidget(sendsLabel_, 1);
		layout->addWidget(sendsState_, 0, Qt::AlignVCenter);
	}
	body->addWidget(sendsRow_);

	statsLabel_ = new QLabel(statusCard_);
	statsLabel_->setWordWrap(true);
	SetRole(statsLabel_, "dataHint", ui::FontRole::DataHint);
	statsLabel_->setVisible(false);
	body->addSpacing(2);
	body->addWidget(statsLabel_);

	auto makeLine = [this, body](QWidget *&row, QLabel *&icon, QLabel *&label, const char *tone) {
		row = new QWidget(statusCard_);
		auto *layout = new QHBoxLayout(row);
		layout->setContentsMargins(0, 4, 0, 0);
		layout->setSpacing(6);
		icon = MakeIconLabel(row, kInlineIconSize);
		label = new QLabel(row);
		label->setWordWrap(true);
		SetRole(label, "hint", ui::FontRole::Hint);
		label->setProperty("tgTone", QString::fromLatin1(tone));
		layout->addWidget(icon, 0, Qt::AlignTop);
		layout->addWidget(label, 1);
		row->setVisible(false);
		body->addWidget(row);
	};
	makeLine(ruleRow_, ruleIcon_, ruleLabel_, "accent");
	makeLine(noticeRow_, noticeIcon_, noticeLabel_, "danger");
	cardLayout->addLayout(body);
	return statusCard_;
}

QFrame *StreamModePanel::BuildAccountCard()
{
	accountCard_ = new QFrame(controlsBox_);
	accountCard_->setProperty("tgCard", QStringLiteral("true"));
	auto *column = new QVBoxLayout(accountCard_);
	column->setContentsMargins(12, 10, 12, 10);
	column->setSpacing(8);
	auto *row = new QHBoxLayout();
	row->setSpacing(10);
	accountIcon_ = MakeIconLabel(accountCard_, kAccountIconSize);
	accountLabel_ = new QLabel(accountCard_);
	accountLabel_->setWordWrap(true);
	row->addWidget(accountIcon_, 0, Qt::AlignTop);
	row->addWidget(accountLabel_, 1);
	column->addLayout(row);
	signInButton_ = new QPushButton(Tr("Stream.Account.SignIn"), accountCard_);
	signInButton_->setProperty("tgVariant", QStringLiteral("primary"));
	signInButton_->setFocusPolicy(Qt::TabFocus);
	signInButton_->setCursor(Qt::PointingHandCursor);
	signInButton_->setToolTip(Tr("Dock.Account.OpensBrowser"));
	signInButton_->setIconSize(kButtonIconBox);
	signInButton_->setLayoutDirection(Qt::RightToLeft); // Icon after the text: it opens the browser.
	SetRole(signInButton_, "button", ui::FontRole::Button);
	column->addWidget(signInButton_);
	connect(signInButton_, &QPushButton::clicked, this, [this] {
		auth_.StartLogin();
		Refresh();
	});
	accountCard_->setVisible(false);
	return accountCard_;
}

QFrame *StreamModePanel::BuildDelayCard()
{
	// "DELAY": the seconds, 5 s steps, and the three presets as quick picks.
	auto *card = new QFrame(controlsBox_);
	card->setProperty("tgCard", QStringLiteral("true"));
	auto *layout = new QVBoxLayout(card);
	layout->setContentsMargins(12, 12, 12, 12);
	layout->setSpacing(6);

	delaySpin_ = new QSpinBox(card);
	delaySpin_->setRange(1, static_cast<int>(relay::kMaxDelaySeconds));
	delaySpin_->setSuffix(Tr("Stream.Seconds"));
	delaySpin_->setKeyboardTracking(false);
	delaySpin_->setAccessibleName(Tr("Stream.Delay"));
	delaySpin_->setToolTip(Tr("Stream.DelayHelp").arg(relay::kMaxDelaySeconds));
	GuardWheel(delaySpin_);
	auto *eyebrow = new QLabel(Tr("Stream.Delay"), card);
	SetRole(eyebrow, "eyebrow", ui::FontRole::Eyebrow);
	eyebrow->setBuddy(delaySpin_);
	layout->addWidget(eyebrow);

	auto *row = new QHBoxLayout();
	row->setSpacing(6);
	row->addWidget(delaySpin_, 1);
	auto makeStep = [this, card, row](const QString &text, const char *nameKey, int delta) {
		auto *button = new QPushButton(text, card);
		button->setProperty("tgVariant", QStringLiteral("step"));
		button->setFocusPolicy(Qt::TabFocus);
		button->setAccessibleName(Tr(nameKey));
		button->setToolTip(Tr(nameKey));
		SetRole(button, "step", ui::FontRole::Data);
		row->addWidget(button);
		connect(button, &QPushButton::clicked, this, [this, delta] {
			controller_.AdjustDelay(delta);
			SettingsChanged();
			Refresh();
		});
		return button;
	};
	lessButton_ = makeStep(QStringLiteral("-5"), "Stream.Delay.Less", -5);
	moreButton_ = makeStep(QStringLiteral("+5"), "Stream.Delay.More", 5);
	layout->addLayout(row);
	connect(delaySpin_, qOverload<int>(&QSpinBox::valueChanged), this, [this](int value) {
		if (refreshing_)
			return;
		controller_.SetDelaySeconds(static_cast<uint32_t>(value));
		SettingsChanged();
		Refresh();
	});

	// "Presets" then the chips; the label goes above them in narrow docks.
	chipRow_ = new QBoxLayout(QBoxLayout::LeftToRight);
	chipRow_->setSpacing(6);
	auto *chipsLabel = new QLabel(Tr("Stream.QuickPick"), card);
	SetRole(chipsLabel, "hint", ui::FontRole::Hint);
	chipRow_->addWidget(chipsLabel, 0, Qt::AlignLeft | Qt::AlignVCenter);
	auto *chips = new QHBoxLayout();
	chips->setSpacing(6);
	for (int i = 0; i < 3; ++i) {
		auto *chip = new QPushButton(card);
		chip->setProperty("tgVariant", QStringLiteral("chip"));
		chip->setFocusPolicy(Qt::TabFocus);
		chip->setToolTip(Tr("Stream.PresetHelp").arg(i + 1));
		SetRole(chip, "chip", ui::FontRole::Chip);
		chips->addWidget(chip);
		presetChips_[i] = chip;
		connect(chip, &QPushButton::clicked, this, [this, i] {
			controller_.SetDelaySeconds(controller_.Preset(i));
			SettingsChanged();
			Refresh();
		});
	}
	chips->addStretch(1);
	chipRow_->addLayout(chips, 1);
	layout->addLayout(chipRow_);
	return card;
}

StreamModePanel::Section *StreamModePanel::AddSection(QVBoxLayout *parent, const QString &title, bool open)
{
	auto owned = std::make_unique<Section>();
	Section *section = owned.get();
	sections_.push_back(std::move(owned));
	QWidget *host = parent->parentWidget();

	auto *headerRow = new QHBoxLayout();
	headerRow->setContentsMargins(0, 0, 4, 0);
	headerRow->setSpacing(6);
	section->header = new QPushButton(title, host);
	section->header->setProperty("tgVariant", QStringLiteral("section"));
	section->header->setCheckable(true);
	section->header->setFocusPolicy(Qt::TabFocus);
	section->header->setCursor(Qt::PointingHandCursor);
	section->header->setIconSize(kSectionIconBox);
	section->header->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
	SetRole(section->header, "section", ui::FontRole::Strong);
	section->count = new QLabel(host);
	SetRole(section->count, "tag", ui::FontRole::Chip);
	section->count->setVisible(false);
	headerRow->addWidget(section->header, 1);
	headerRow->addWidget(section->count, 0, Qt::AlignVCenter);
	parent->addLayout(headerRow);

	section->body = new QWidget(host);
	auto *bodyLayout = new QVBoxLayout(section->body);
	bodyLayout->setContentsMargins(8, 4, 8, 10);
	bodyLayout->setSpacing(6);
	parent->addWidget(section->body);

	connect(section->header, &QPushButton::clicked, this,
		[this, section] { SetSectionOpen(*section, !section->open); });
	SetSectionOpen(*section, open);
	return section;
}

void StreamModePanel::SetSectionOpen(Section &section, bool open)
{
	section.open = open;
	{
		const QSignalBlocker block(section.header);
		section.header->setChecked(open);
	}
	section.body->setVisible(open);
	const QString tone = section.header->property("tgTone").toString();
	SetButtonIcon(section.header, open ? ui::IconId::ChevronDown : ui::IconId::ChevronRight,
		      ToneColor(tone.isEmpty() ? QStringLiteral("text") : tone));
}

void StreamModePanel::BuildWarnings(QVBoxLayout *body)
{
	warningsLayout_ = body;
}

void StreamModePanel::BuildRules(QVBoxLayout *body)
{
	QWidget *host = body->parentWidget();
	auto *help = new QLabel(Tr("Stream.RulesHelp"), host);
	help->setWordWrap(true);
	SetRole(help, "hint", ui::FontRole::Hint);
	body->addWidget(help);

	rulesList_ = new QVBoxLayout();
	rulesList_->setSpacing(6);
	body->addLayout(rulesList_);
	rulesEmpty_ = new QLabel(Tr("Stream.Rules.Empty"), host);
	rulesEmpty_->setWordWrap(true);
	SetRole(rulesEmpty_, "dataHint", ui::FontRole::DataHint);
	body->addWidget(rulesEmpty_);

	addRule_ = new QPushButton(Tr("Stream.Rules.Add"), host);
	addRule_->setProperty("tgVariant", QStringLiteral("secondary"));
	addRule_->setProperty("tgSize", QStringLiteral("large"));
	addRule_->setFocusPolicy(Qt::TabFocus);
	addRule_->setIconSize(kButtonIconBox);
	addRule_->setToolTip(Tr("Stream.Rules.AddHelp"));
	SetRole(addRule_, "button", ui::FontRole::Button);
	body->addWidget(addRule_);

	for (const SceneRule &rule : controller_.SceneRules())
		AddRuleRow(rule.scene, static_cast<int>(rule.action), static_cast<int>(rule.seconds));
	connect(addRule_, &QPushButton::clicked, this, [this] {
		obs_source_t *current = obs_frontend_get_current_scene();
		const std::string name = current ? obs_source_get_name(current) : "";
		obs_source_release(current);
		AddRuleRow(name, static_cast<int>(SceneAction::DelayOn), 0);
		RulesChanged();
		if (!ruleRows_.empty())
			ruleRows_.back().scene->setFocus(Qt::TabFocusReason);
	});
}

void StreamModePanel::BuildPresets(QVBoxLayout *body)
{
	QWidget *host = body->parentWidget();
	auto *help = new QLabel(Tr("Stream.Presets.Help"), host);
	help->setWordWrap(true);
	SetRole(help, "hint", ui::FontRole::Hint);
	body->addWidget(help);

	presetRow_ = new QBoxLayout(QBoxLayout::LeftToRight);
	presetRow_->setSpacing(8);
	for (int i = 0; i < 3; ++i) {
		auto *column = new QVBoxLayout();
		column->setSpacing(5);
		auto *spin = new QSpinBox(host);
		spin->setRange(1, static_cast<int>(relay::kMaxDelaySeconds));
		spin->setSuffix(Tr("Stream.Seconds"));
		spin->setKeyboardTracking(false);
		spin->setValue(static_cast<int>(controller_.Preset(i)));
		spin->setToolTip(Tr("Stream.PresetHelp").arg(i + 1));
		spin->setAccessibleName(Tr("Stream.Preset.Label").arg(i + 1));
		GuardWheel(spin);
		auto *label = new QLabel(Tr("Stream.Preset.Label").arg(i + 1), host);
		SetRole(label, "eyebrow", ui::FontRole::Eyebrow);
		label->setBuddy(spin);
		column->addWidget(label);
		column->addWidget(spin);
		presetRow_->addLayout(column, 1);
		presetSpins_[i] = spin;
		connect(spin, qOverload<int>(&QSpinBox::valueChanged), this, [this, i](int value) {
			if (refreshing_)
				return;
			controller_.SetPreset(i, static_cast<uint32_t>(value));
			SettingsChanged();
			Refresh();
		});
	}
	body->addLayout(presetRow_);
}

void StreamModePanel::BuildOptions(QVBoxLayout *body)
{
	QWidget *host = body->parentWidget();

	// Since the history spills to disk this is no longer a setting: with the
	// spill every live raise is instant; in RAM only (the disk couldn't be
	// used) up to the highest preset or scene rule. Refresh() fills it in.
	auto *keepLabel = new QLabel(Tr("Stream.Keep"), host);
	SetRole(keepLabel, "eyebrow", ui::FontRole::Eyebrow);
	keepValue_ = new QLabel(host);
	keepValue_->setWordWrap(true);
	keepValue_->setTextInteractionFlags(Qt::TextSelectableByMouse);
	keepHelp_ = new QLabel(host);
	keepHelp_->setWordWrap(true);
	SetRole(keepHelp_, "hint", ui::FontRole::Hint);
	body->addWidget(keepLabel);
	body->addWidget(keepValue_);
	body->addWidget(keepHelp_);

	// Check boxes with wrapping labels (clicking the label toggles).
	auto addCheck = [this, host, body](QCheckBox *&check, QLabel *&text, const char *key) {
		check = new QCheckBox(host);
		check->setFocusPolicy(Qt::TabFocus);
		check->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);
		check->setAccessibleName(Tr(key));
		text = new QLabel(Tr(key), host);
		text->setWordWrap(true);
		text->setBuddy(check);
		text->setCursor(Qt::PointingHandCursor);
		text->installEventFilter(this);
		auto *row = new QHBoxLayout();
		row->setSpacing(8);
		row->addWidget(check, 0, Qt::AlignTop);
		row->addWidget(text, 1);
		body->addSpacing(4);
		body->addLayout(row);
	};
	addCheck(drainCheck_, drainText_, "Stream.Drain");
	connect(drainCheck_, &QCheckBox::toggled, this, [this](bool on) {
		if (refreshing_)
			return;
		controller_.SetDrainOnStop(on);
		SettingsChanged();
	});
	addCheck(overlayCheck_, overlayText_, "Stream.Overlay");
	connect(overlayCheck_, &QCheckBox::toggled, this, [this](bool on) {
		if (refreshing_)
			return;
		controller_.SetShowOverlay(on);
		SettingsChanged();
		Refresh();
	});

	cornerCombo_ = new QComboBox(host);
	cornerCombo_->addItems({Tr("Stream.Corner.TopLeft"), Tr("Stream.Corner.TopRight"),
				Tr("Stream.Corner.BottomLeft"), Tr("Stream.Corner.BottomRight")});
	cornerCombo_->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
	cornerCombo_->setMinimumContentsLength(8);
	cornerCombo_->setAccessibleName(Tr("Stream.Overlay.Position"));
	GuardWheel(cornerCombo_);
	auto *cornerLabel = new QLabel(Tr("Stream.Overlay.Position"), host);
	SetRole(cornerLabel, "eyebrow", ui::FontRole::Eyebrow);
	cornerLabel->setBuddy(cornerCombo_);
	body->addSpacing(2);
	body->addWidget(cornerLabel);
	body->addWidget(cornerCombo_);
	connect(cornerCombo_, qOverload<int>(&QComboBox::currentIndexChanged), this, [this](int index) {
		if (refreshing_ || index < 0)
			return;
		controller_.SetOverlayCorner(static_cast<uint32_t>(index));
		SettingsChanged();
	});
}

QWidget *StreamModePanel::CreateSceneHeader(QWidget *parent, std::function<void(bool open)> onToggled)
{
	onSceneToggled_ = std::move(onToggled);
	auto *header = new QWidget(parent);
	auto *layout = new QVBoxLayout(header);
	layout->setContentsMargins(0, 0, 0, 0);
	layout->setSpacing(0);
	layout->addWidget(MakeDivider(header));
	auto *inner = new QVBoxLayout();
	inner->setContentsMargins(12, 8, 12, 8);
	inner->setSpacing(2);
	sceneToggle_ = new QPushButton(Tr("Stream.SceneSection"), header);
	sceneToggle_->setProperty("tgVariant", QStringLiteral("section"));
	sceneToggle_->setCheckable(true);
	sceneToggle_->setFocusPolicy(Qt::TabFocus);
	sceneToggle_->setCursor(Qt::PointingHandCursor);
	sceneToggle_->setIconSize(kSectionIconBox);
	SetRole(sceneToggle_, "section", ui::FontRole::Strong);
	sceneHint_ = new QLabel(header);
	sceneHint_->setWordWrap(true);
	SetRole(sceneHint_, "hint", ui::FontRole::Hint);
	sceneHint_->setContentsMargins(6, 0, 6, 0);
	inner->addWidget(sceneToggle_);
	inner->addWidget(sceneHint_);
	layout->addLayout(inner);
	connect(sceneToggle_, &QPushButton::clicked, this, [this] { SetSceneSectionOpen(!sceneOpen_); });

	// The whole container takes the panel's style sheet: the panel's scroll
	// area (tgDockScroll) turns transparent like scene mode's, and the header
	// gets the section style. Scene mode's dock keeps its own sheet.
	themedPeers_.emplace_back(parent);
	parent->setStyleSheet(styleSheet());
	SetSceneSectionOpen(sceneOpen_);
	Refresh();
	return header;
}

void StreamModePanel::SetSceneSectionOpen(bool open)
{
	sceneOpen_ = open;
	if (sceneToggle_) {
		const QSignalBlocker block(sceneToggle_);
		sceneToggle_->setChecked(open);
		SetButtonIcon(sceneToggle_, open ? ui::IconId::ChevronDown : ui::IconId::ChevronRight,
			      ToneColor(QStringLiteral("text")));
	}
	if (onSceneToggled_)
		onSceneToggled_(open);
}

// ---- Theme -----------------------------------------------------------------

QString StreamModePanel::ExtraStyleSheet() const
{
	// Panel-only components, from the same tokens as ui-theme.cpp's sheet:
	// section headers, preset chips, the 5 s steps, icon buttons and the
	// danger tone of a tag.
	const uitokens::Tokens &t = theme_.tokens;
	auto c = [](uitokens::Rgb color) {
		return QString::fromStdString(uitokens::Hex(color));
	};
	const double base = theme_.basePt;
	const ui::BrandFonts &fonts = ui::Fonts();
	const QString strong = FontCss(QString(), base, 0.0, 600);
	const QString mono = FontCss(fonts.mono, base, -1.5, 500);
	const QString data = FontCss(fonts.mono, base, 0.0, 500);
	const QString text = c(t.text);
	const QString muted = c(t.textMuted);
	const QString line = c(t.line);
	const QString lineStrong = c(t.lineStrong);
	const QString accentText = c(t.accentText);
	const QString accentTint = c(t.accentTint);
	const QString focus = c(t.focus);
	const QString disabled = c(t.disabledFg);

	QStringList s;
	s << QStringLiteral(
		     "QPushButton[tgVariant=\"section\"] { background: transparent; color: %1; border: 1px solid "
		     "transparent; border-radius: 8px; padding: 0px 6px; text-align: left; height: 28px; "
		     "min-height: 28px; max-height: 28px; margin: 0px; %2 }")
			.arg(text, strong);
	s << QStringLiteral(
		     "QPushButton[tgVariant=\"section\"]:hover, QPushButton[tgVariant=\"section\"]:checked:hover "
		     "{ color: %1; background-color: %2; }")
			.arg(accentText, accentTint);
	s << QStringLiteral("QPushButton[tgVariant=\"section\"]:checked { background: transparent; }");
	s << QStringLiteral("QPushButton[tgVariant=\"section\"][tgTone=\"warning\"] { color: %1; }")
			.arg(c(t.warningText));
	s << QStringLiteral("QPushButton[tgVariant=\"section\"]:focus { border: 2px solid %1; }").arg(focus);

	s << QStringLiteral(
		     "QPushButton[tgVariant=\"chip\"] { background: transparent; color: %1; border: 1px solid %2; "
		     "border-radius: 11px; padding: 0px 10px; height: 22px; min-height: 22px; max-height: 22px; "
		     "margin: 0px; %3 }")
			.arg(muted, lineStrong, mono);
	s << QStringLiteral(
		     "QPushButton[tgVariant=\"chip\"]:hover, QPushButton[tgVariant=\"chip\"][tgActive=\"true\"] { "
		     "color: %1; border-color: %1; background-color: %2; }")
			.arg(accentText, accentTint);
	s << QStringLiteral("QPushButton[tgVariant=\"chip\"]:focus { border: 2px solid %1; }").arg(focus);
	s << QStringLiteral("QPushButton[tgVariant=\"chip\"]:disabled { color: %1; border-color: %2; background: "
			    "transparent; }")
			.arg(disabled, line);

	// 28 + 2 x 1 border = 30 px: the spin box's height.
	s << QStringLiteral(
		     "QPushButton[tgVariant=\"step\"] { background: transparent; color: %1; border: 1px solid %2; "
		     "border-radius: 10px; padding: 0px 10px; height: 28px; min-height: 28px; max-height: 28px; "
		     "margin: 0px; %3 }")
			.arg(text, lineStrong, data);
	s << QStringLiteral(
		     "QPushButton[tgVariant=\"step\"]:hover { color: %1; border-color: %1; background-color: %2; }")
			.arg(accentText, accentTint);
	s << QStringLiteral("QPushButton[tgVariant=\"step\"]:pressed { background-color: %1; padding-top: 1px; }")
			.arg(accentTint);
	s << QStringLiteral("QPushButton[tgVariant=\"step\"]:focus { border: 2px solid %1; }").arg(focus);
	s << QStringLiteral("QPushButton[tgVariant=\"step\"]:disabled { color: %1; border-color: %2; }")
			.arg(disabled, line);

	s << QStringLiteral(
		"QPushButton[tgVariant=\"icon\"] { background: transparent; border: 1px solid transparent; "
		"border-radius: 8px; padding: 0px; margin: 0px; width: 26px; height: 26px; min-width: 26px; "
		"max-width: 26px; min-height: 26px; max-height: 26px; }");
	s << QStringLiteral("QPushButton[tgVariant=\"icon\"]:hover { background-color: %1; border-color: %2; }")
			.arg(c(t.dangerTint), c(t.dangerBorder));
	s << QStringLiteral("QPushButton[tgVariant=\"icon\"]:focus { border: 2px solid %1; }").arg(focus);

	s << QStringLiteral(
		     "QLabel[tgRole=\"tag\"][tgTone=\"danger\"], QLabel[tgRole=\"chip\"][tgTone=\"danger\"] { color: %1; background-color: %2; border-color: "
		     "%1; }")
			.arg(c(t.dangerText), c(t.dangerTint));
	return s.join(QLatin1Char('\n'));
}

void StreamModePanel::ApplyTheme(bool force)
{
	if (applyingTheme_ || detached_)
		return;
	applyingTheme_ = true;
	const double basePt = ui::BaseFontPointSize(this);
	ui::Theme theme = ui::BuildTheme(QApplication::palette(), basePt);
	if (force || !themeApplied_ || theme.styleSheet != theme_.styleSheet) {
		theme_ = std::move(theme);
		themeApplied_ = true;
		fontRoles_.erase(std::remove_if(fontRoles_.begin(), fontRoles_.end(),
						[](const auto &entry) { return entry.first.isNull(); }),
				 fontRoles_.end());
		const QString sheet = theme_.styleSheet + QLatin1Char('\n') + ExtraStyleSheet();
		setStyleSheet(sheet);
		for (const QPointer<QWidget> &peer : themedPeers_) {
			if (peer)
				peer->setStyleSheet(sheet);
		}
		for (auto &[widget, role] : fontRoles_) {
			if (widget)
				ui::ApplyFont(widget, role, theme_.basePt);
		}
		ApplyIcons();
		Refresh();
	}
	applyingTheme_ = false;
}

void StreamModePanel::changeEvent(QEvent *event)
{
	QWidget::changeEvent(event);
	switch (event->type()) {
	case QEvent::PaletteChange:
	case QEvent::ApplicationPaletteChange:
	case QEvent::ApplicationFontChange:
		ApplyTheme();
		break;
#if QT_VERSION >= QT_VERSION_CHECK(6, 6, 0)
	case QEvent::DevicePixelRatioChange:
		if (themeApplied_)
			ApplyTheme(/*force=*/true);
		break;
#endif
	default:
		break;
	}
}

QColor StreamModePanel::ToneColor(const QString &tone) const
{
	const uitokens::Tokens &t = theme_.tokens;
	if (tone == QLatin1String("accent"))
		return ui::ToQColor(t.accentText);
	if (tone == QLatin1String("warning"))
		return ui::ToQColor(t.warningText);
	if (tone == QLatin1String("danger"))
		return ui::ToQColor(t.dangerText);
	if (tone == QLatin1String("text"))
		return ui::ToQColor(t.text);
	return ui::ToQColor(t.textMuted);
}

void StreamModePanel::SetIcon(QLabel *label, ui::IconId icon, const QString &tone, int size)
{
	label->setPixmap(ui::IconPixmap(icon, ToneColor(tone), size, label->devicePixelRatioF()));
}

void StreamModePanel::SetButtonIcon(QPushButton *button, ui::IconId icon, const QColor &color, Qt::Alignment alignment)
{
	button->setIcon(ui::Icon(icon, color, ui::ToQColor(theme_.tokens.disabledFg), alignment));
}

void StreamModePanel::ApplyIcons()
{
	const uitokens::Tokens &t = theme_.tokens;
	const QColor text = ui::ToQColor(t.text);
	SetIcon(hotkeysIcon_, ui::IconId::Keyboard, QStringLiteral("muted"), kInlineIconSize);
	SetIcon(ruleIcon_, ui::IconId::Info, QStringLiteral("accent"), kInlineIconSize);
	SetIcon(noticeIcon_, ui::IconId::AlertTriangle, QStringLiteral("danger"), kInlineIconSize);
	SetButtonIcon(onButton_, ui::IconId::Shield, ui::ToQColor(t.onGradient));
	SetButtonIcon(offButton_, ui::IconId::X, ui::ToQColor(t.dangerButtonText));
	SetButtonIcon(liveButton_, ui::IconId::Radio, text);
	SetButtonIcon(dumpButton_, ui::IconId::Scissors, text);
	SetButtonIcon(cutButton_, ui::IconId::X, text);
	SetButtonIcon(signInButton_, ui::IconId::ExternalLink, ui::ToQColor(t.onGradient), Qt::AlignRight);
	SetButtonIcon(addRule_, ui::IconId::Plus, text);
	for (const std::unique_ptr<Section> &section : sections_)
		SetSectionOpen(*section, section->open);
	if (sceneToggle_)
		SetButtonIcon(sceneToggle_, sceneOpen_ ? ui::IconId::ChevronDown : ui::IconId::ChevronRight, text);
	const QColor muted = ui::ToQColor(t.textMuted);
	for (const RuleRow &row : ruleRows_)
		SetButtonIcon(row.remove, ui::IconId::Trash, muted, Qt::AlignCenter);
	for (auto &[icon, label] : warningRows_)
		SetIcon(icon, ui::IconId::AlertTriangle, QStringLiteral("warning"), kInlineIconSize);
	RefreshAccount();
}

// ---- Events ----------------------------------------------------------------

void StreamModePanel::resizeEvent(QResizeEvent *event)
{
	QWidget::resizeEvent(event);
	ApplyNarrowLayout(event->size().width());
}

void StreamModePanel::ApplyNarrowLayout(int width)
{
	// Back to live | Dump stack in very narrow docks; the three presets and
	// the quick-pick label in narrow ones.
	const bool tiny = width < kTinyWidth;
	const auto direction = tiny ? QBoxLayout::TopToBottom : QBoxLayout::LeftToRight;
	if (actionRow_)
		actionRow_->setDirection(direction);
	const auto narrow = width < kNarrowWidth ? QBoxLayout::TopToBottom : QBoxLayout::LeftToRight;
	if (presetRow_)
		presetRow_->setDirection(narrow);
	if (chipRow_)
		chipRow_->setDirection(narrow);
}

bool StreamModePanel::eventFilter(QObject *watched, QEvent *event)
{
	if (event->type() == QEvent::MouseButtonRelease) {
		QCheckBox *check = nullptr;
		if (watched == modeText_)
			check = modeCheck_;
		else if (watched == drainText_)
			check = drainCheck_;
		else if (watched == overlayText_)
			check = overlayCheck_;
		auto *label = qobject_cast<QLabel *>(watched);
		auto *mouse = static_cast<QMouseEvent *>(event);
		if (check && label && mouse->button() == Qt::LeftButton && check->isEnabled() &&
		    label->rect().contains(mouse->position().toPoint()))
			check->click();
		if (check)
			return false;
	}
	if (event->type() == QEvent::Wheel) {
		auto *widget = qobject_cast<QWidget *>(watched);
		if (widget && !widget->hasFocus()) {
			event->ignore();
			return true;
		}
	}
	return QWidget::eventFilter(watched, event);
}

// ---- Scene rules -----------------------------------------------------------

void StreamModePanel::AddRuleRow(const std::string &scene, int action, int seconds)
{
	QWidget *host = rulesList_->parentWidget();
	RuleRow row;
	row.card = new QFrame(host);
	row.card->setProperty("tgCard", QStringLiteral("inset"));
	auto *layout = new QVBoxLayout(row.card);
	layout->setContentsMargins(10, 8, 8, 10);
	layout->setSpacing(5);

	row.scene = new QComboBox(row.card);
	char **names = obs_frontend_get_scene_names();
	for (char **n = names; n && *n; ++n)
		row.scene->addItem(QString::fromUtf8(*n));
	bfree(names);
	const QString sceneName = QString::fromStdString(scene);
	if (!sceneName.isEmpty() && row.scene->findText(sceneName) < 0)
		row.scene->addItem(sceneName); // A scene that no longer exists keeps its rule.
	row.scene->setCurrentText(sceneName);
	row.scene->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
	row.scene->setMinimumContentsLength(6);
	row.scene->setAccessibleName(Tr("Stream.Rules.Scene"));
	GuardWheel(row.scene);

	row.remove = new QPushButton(row.card);
	row.remove->setProperty("tgVariant", QStringLiteral("icon"));
	row.remove->setFocusPolicy(Qt::TabFocus);
	row.remove->setIconSize(QSize(kInlineIconSize, kInlineIconSize));
	row.remove->setCursor(Qt::PointingHandCursor);

	auto *sceneLabel = new QLabel(Tr("Stream.Rules.Scene"), row.card);
	SetRole(sceneLabel, "eyebrow", ui::FontRole::Eyebrow);
	sceneLabel->setBuddy(row.scene);
	auto *top = new QHBoxLayout();
	top->setSpacing(6);
	top->addWidget(sceneLabel, 1, Qt::AlignBottom);
	top->addWidget(row.remove, 0, Qt::AlignRight);
	layout->addLayout(top);
	layout->addWidget(row.scene);

	row.action = new QComboBox(row.card);
	row.action->addItems({Tr("Stream.Rules.None"), Tr("Stream.Rules.On"), Tr("Stream.Rules.Off")});
	row.action->setCurrentIndex(std::clamp(action, 0, 2));
	row.action->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
	row.action->setMinimumContentsLength(6);
	row.action->setAccessibleName(Tr("Stream.Rules.Action"));
	GuardWheel(row.action);

	row.seconds = new QSpinBox(row.card);
	row.seconds->setRange(0, static_cast<int>(relay::kMaxDelaySeconds));
	row.seconds->setSpecialValueText(Tr("Stream.Rules.Keep"));
	row.seconds->setSuffix(Tr("Stream.Seconds"));
	row.seconds->setKeyboardTracking(false);
	row.seconds->setValue(seconds);
	row.seconds->setAccessibleName(Tr("Stream.Delay"));
	row.seconds->setToolTip(Tr("Stream.Rules.SecondsHelp"));
	row.seconds->setEnabled(row.action->currentIndex() == static_cast<int>(SceneAction::DelayOn));
	GuardWheel(row.seconds);

	auto *actionLabel = new QLabel(Tr("Stream.Rules.Action"), row.card);
	SetRole(actionLabel, "eyebrow", ui::FontRole::Eyebrow);
	actionLabel->setBuddy(row.action);
	layout->addSpacing(3);
	layout->addWidget(actionLabel);
	auto *bottom = new QHBoxLayout();
	bottom->setSpacing(6);
	bottom->addWidget(row.action, 3);
	bottom->addWidget(row.seconds, 2);
	layout->addLayout(bottom);

	rulesList_->addWidget(row.card);
	QFrame *card = row.card;
	QSpinBox *secondsSpin = row.seconds;
	connect(row.scene, &QComboBox::currentTextChanged, this, [this, card](const QString &name) {
		for (const RuleRow &r : ruleRows_) {
			if (r.card == card)
				r.remove->setAccessibleName(Tr("Stream.Rules.RemoveName").arg(name));
		}
		RulesChanged();
	});
	connect(row.action, qOverload<int>(&QComboBox::currentIndexChanged), this, [this, secondsSpin](int index) {
		secondsSpin->setEnabled(index == static_cast<int>(SceneAction::DelayOn));
		RulesChanged();
	});
	connect(row.seconds, qOverload<int>(&QSpinBox::valueChanged), this, [this] { RulesChanged(); });
	connect(row.remove, &QPushButton::clicked, this, [this, card] { RemoveRuleRow(card); });

	row.remove->setAccessibleName(Tr("Stream.Rules.RemoveName").arg(row.scene->currentText()));
	row.remove->setToolTip(Tr("Stream.Rules.Remove"));
	if (themeApplied_) {
		SetButtonIcon(row.remove, ui::IconId::Trash, ui::ToQColor(theme_.tokens.textMuted), Qt::AlignCenter);
		for (QWidget *w : row.card->findChildren<QLabel *>())
			ui::ApplyFont(w, ui::FontRole::Eyebrow, theme_.basePt);
	}
	ruleRows_.push_back(row);
	rulesEmpty_->setVisible(false);
	rulesSection_->count->setText(QString::number(ruleRows_.size()));
	rulesSection_->count->setVisible(true);
}

void StreamModePanel::RemoveRuleRow(QFrame *card)
{
	auto it = std::find_if(ruleRows_.begin(), ruleRows_.end(), [card](const RuleRow &r) { return r.card == card; });
	if (it == ruleRows_.end())
		return;
	const size_t index = static_cast<size_t>(it - ruleRows_.begin());
	ruleRows_.erase(it);
	card->hide();
	card->deleteLater();
	RulesChanged();
	// Focus goes to the next rule's remove button, or to "Add scene".
	if (index < ruleRows_.size())
		ruleRows_[index].remove->setFocus(Qt::TabFocusReason);
	else
		addRule_->setFocus(Qt::TabFocusReason);
}

void StreamModePanel::RulesChanged()
{
	std::vector<SceneRule> rules;
	for (const RuleRow &row : ruleRows_) {
		if (row.scene->currentText().isEmpty())
			continue;
		SceneRule rule;
		rule.scene = row.scene->currentText().toStdString();
		rule.action = static_cast<SceneAction>(row.action->currentIndex());
		rule.seconds = static_cast<uint32_t>(row.seconds->value());
		rules.push_back(std::move(rule));
	}
	controller_.SetSceneRules(std::move(rules));
	rulesEmpty_->setVisible(ruleRows_.empty());
	rulesSection_->count->setText(QString::number(ruleRows_.size()));
	rulesSection_->count->setVisible(!ruleRows_.empty());
	SettingsChanged();
}

// ---- Where the stream goes ------------------------------------------------

void StreamModePanel::RefreshDestination(const StreamDelayStatus &status)
{
	const bool running = (status.engaged || status.state == StreamDelayState::Draining) &&
			     !status.relay.sinks.empty();
	QString name = running ? QString::fromStdString(status.relay.sinks.front().name) : QString();
	if (name.isEmpty())
		name = ObsServiceName();
	sendsLabel_->setText(name.isEmpty() ? Tr("Stream.SendsTo.Default") : Tr("Stream.SendsTo").arg(name));
	sendsState_->setVisible(running);
	if (!running)
		return;
	const relay::SinkStatus &sink = status.relay.sinks.front();
	QString state = SinkStateText(sink.state);
	if (sink.state == relay::SinkState::Live)
		state += QStringLiteral(" · ") + QString::number(sink.sentMbps, 'f', 1) + QStringLiteral(" Mbps");
	sendsState_->setText(state);
	ui::SetStyleProperty(sendsState_, "tgTone", SinkStateTone(sink.state));
}

// ---- Refresh ---------------------------------------------------------------

void StreamModePanel::SettingsChanged()
{
	if (onSettingsChanged_)
		onSettingsChanged_();
}

void StreamModePanel::Detach()
{
	detached_ = true;
	if (timer_)
		timer_->stop();
	if (frontendCallback_) {
		obs_frontend_remove_event_callback(OnFrontendEvent, this);
		frontendCallback_ = false;
	}
	setEnabled(false);
}

void StreamModePanel::RefreshStatus(const StreamDelayStatus &status)
{
	// Each state differs by rule, icon, headline color AND text -- never by
	// color alone (as in scene mode's status card).
	QString headline;
	QString detail;
	QString rule = QStringLiteral("none");
	QString tone = QStringLiteral("text");
	QString chip;
	QString chipTone;
	ui::IconId icon = ui::IconId::Circle;
	QString iconTone = QStringLiteral("muted");
	const uint32_t applied = status.relay.delaySeconds;
	const QString seconds = applied != status.delaySeconds && applied > 0
					? Tr("Dock.Status.SecondsOf").arg(applied).arg(status.delaySeconds)
					: Tr("Dock.Status.Seconds").arg(status.delaySeconds);

	if (!status.available) {
		headline = Tr("Stream.Head.Unavailable");
		detail = Tr("Stream.Unavailable");
		icon = ui::IconId::Info;
	} else if (status.dumpRestoreIn > 0 && status.engaged) {
		headline = Tr("Stream.Head.Dumped");
		detail = Tr("Stream.Detail.Dumped").arg(status.dumpRestoreIn);
		rule = QStringLiteral("warning");
		tone = QStringLiteral("warning");
		icon = ui::IconId::Scissors;
		iconTone = QStringLiteral("warning");
		chip = Tr("Stream.Chip.Back").arg(status.dumpRestoreIn);
		chipTone = QStringLiteral("warning");
	} else {
		switch (status.state) {
		case StreamDelayState::Idle:
			if (!status.ineligible.empty()) {
				const std::string key = "Stream.Ineligible." + status.ineligible;
				headline = Tr("Stream.Head.Ineligible");
				detail = Tr(key.c_str());
				rule = QStringLiteral("warning");
				tone = QStringLiteral("warning");
				icon = ui::IconId::AlertTriangle;
				iconTone = QStringLiteral("warning");
			} else if (status.delayOn) {
				headline = Tr("Stream.Head.Armed");
				detail = Tr("Stream.Detail.Armed").arg(status.delaySeconds);
				icon = ui::IconId::Shield;
				iconTone = QStringLiteral("accent");
				chip = Tr("Dock.Status.Seconds").arg(status.delaySeconds);
				chipTone = QStringLiteral("accent");
			} else {
				headline = Tr("Stream.Head.Idle");
				detail = Tr("Stream.Detail.Idle");
			}
			break;
		case StreamDelayState::Live:
			headline = Tr("Stream.Head.Live");
			detail = Tr("Stream.Detail.Live");
			rule = QStringLiteral("warning");
			tone = QStringLiteral("warning");
			icon = ui::IconId::Radio;
			iconTone = QStringLiteral("warning");
			break;
		case StreamDelayState::Delayed:
			headline = Tr("Stream.Head.Delayed");
			detail = Tr("Stream.Detail.DelayedFor").arg(applied > 0 ? applied : status.delaySeconds);
			rule = QStringLiteral("active");
			icon = ui::IconId::Shield;
			iconTone = QStringLiteral("accent");
			chip = seconds;
			chipTone = QStringLiteral("accent");
			break;
		case StreamDelayState::Draining:
			headline = Tr("Stream.Head.Draining");
			detail = Tr("Stream.Detail.DrainingFor").arg(applied);
			rule = QStringLiteral("warning");
			tone = QStringLiteral("warning");
			icon = ui::IconId::Clock;
			iconTone = QStringLiteral("warning");
			chip = Tr("Dock.Status.Seconds").arg(applied);
			chipTone = QStringLiteral("warning");
			break;
		}
	}
	stateLabel_->setText(headline);
	ui::SetStyleProperty(stateLabel_, "tgTone", tone);
	ui::SetStyleProperty(statusRule_, "tgRule", rule);
	SetIcon(statusIcon_, icon, iconTone, kStatusIconSize);
	statusChip_->setVisible(!chip.isEmpty());
	if (!chip.isEmpty()) {
		statusChip_->setText(chip);
		ui::SetStyleProperty(statusChip_, "tgTone", chipTone);
	}
	detailLabel_->setText(detail);
	detailLabel_->setVisible(!detail.isEmpty());
	statusCard_->setAccessibleName(QStringLiteral("%1: %2. %3").arg(Tr("Dock.Status.Eyebrow"), headline, detail));

	QString stats;
	if (status.engaged || status.state == StreamDelayState::Draining) {
		const relay::RelayStatus &r = status.relay;
		QString video = QString::fromStdString(r.videoCodec);
		if (r.width > 0)
			video += QStringLiteral(" %1x%2").arg(r.width).arg(r.height);
		// RAM and disk separately: with the spill most of the history is on
		// disk and RAM stays small.
		if (r.spillActive || r.historyDiskBytes > 0)
			stats = Tr("Stream.StatsDisk")
					.arg(QString::number(r.historySeconds, 'f', 0))
					.arg(Megabytes(r.historyRamBytes))
					.arg(Megabytes(r.historyDiskBytes))
					.arg(QString::number(r.bitrateMbps, 'f', 1))
					.arg(video);
		else
			stats = Tr("Stream.Stats")
					.arg(QString::number(r.historySeconds, 'f', 0))
					.arg(Megabytes(r.historyRamBytes))
					.arg(QString::number(r.bitrateMbps, 'f', 1))
					.arg(video);
	}
	statsLabel_->setText(stats);
	statsLabel_->setVisible(!stats.isEmpty());

	const bool ruleShown = !status.ruleScene.empty() && status.engaged && status.dumpRestoreIn == 0;
	ruleLabel_->setText(ruleShown ? Tr("Stream.RuleApplied").arg(QString::fromStdString(status.ruleScene))
				      : QString());
	ruleRow_->setVisible(ruleShown);
	noticeLabel_->setText(status.lastEvent.empty()
				      ? QString()
				      : Tr("Stream.LastEvent").arg(QString::fromStdString(status.lastEvent)));
	noticeRow_->setVisible(!status.lastEvent.empty());

	// Actions.
	const bool canEnable = auth_.CanEnable();
	onButton_->setVisible(status.available && !status.delayOn);
	onButton_->setEnabled(canEnable);
	onButton_->setToolTip(canEnable ? QString() : Tr("Stream.Account.Required"));
	offButton_->setVisible(status.available && status.delayOn);
	const bool running = status.delayOn && status.engaged;
	liveButton_->setVisible(running);
	dumpButton_->setVisible(running);
	dumpButton_->setEnabled(status.dumpRestoreIn == 0 && status.relay.delaySeconds > 0);
	cutButton_->setVisible(status.state == StreamDelayState::Draining);
}

void StreamModePanel::RefreshAccount()
{
	if (!accountCard_)
		return;
	// Only when turning the delay on needs a sign-in: signed in, the account
	// lives in scene mode's dock below.
	if (auth_.CanEnable()) {
		accountCard_->setVisible(false);
		return;
	}
	accountCard_->setVisible(true);
	if (auth_.IsLoggingIn() && !auth_.PendingUserCode().empty()) {
		accountLabel_->setText(
			Tr("Stream.Account.Pending").arg(QString::fromStdString(auth_.PendingUserCode())));
		SetIcon(accountIcon_, ui::IconId::Clock, QStringLiteral("accent"), kAccountIconSize);
		signInButton_->setVisible(false);
	} else {
		accountLabel_->setText(Tr("Stream.Account.Required"));
		SetIcon(accountIcon_, ui::IconId::Lock, QStringLiteral("muted"), kAccountIconSize);
		signInButton_->setVisible(!auth_.IsLoggingIn());
	}
}

void StreamModePanel::RefreshWarnings(const StreamDelayStatus &status)
{
	QStringList texts;
	bool keyintWarned = false;
	for (const std::string &w : status.warnings) {
		const size_t colon = w.find(':');
		const std::string name = w.substr(0, colon);
		keyintWarned = keyintWarned || name.rfind("keyint", 0) == 0;
		const std::string key = "Stream.Warn." + name;
		QString text = Tr(key.c_str());
		if (colon != std::string::npos)
			text = text.arg(QString::fromStdString(w.substr(colon + 1)));
		texts << text;
	}
	// The relay measures the real keyframe interval while streaming.
	const double keyint = status.relay.keyframeIntervalSeconds;
	if (!keyintWarned && (status.engaged || status.state == StreamDelayState::Draining) && keyint > 4.5)
		texts << Tr("Stream.KeyframeWarning").arg(QString::number(keyint, 'f', 1));

	while (warningRows_.size() < static_cast<size_t>(texts.size())) {
		QWidget *host = warningsSection_->body;
		auto *row = new QHBoxLayout();
		row->setSpacing(6);
		QLabel *icon = MakeIconLabel(host, kInlineIconSize);
		auto *label = new QLabel(host);
		label->setWordWrap(true);
		label->setProperty("tgRole", QStringLiteral("muted"));
		row->addWidget(icon, 0, Qt::AlignTop);
		row->addWidget(label, 1);
		warningsLayout_->addLayout(row);
		SetIcon(icon, ui::IconId::AlertTriangle, QStringLiteral("warning"), kInlineIconSize);
		warningRows_.emplace_back(icon, label);
	}
	for (size_t i = 0; i < warningRows_.size(); ++i) {
		const bool shown = i < static_cast<size_t>(texts.size());
		warningRows_[i].first->setVisible(shown);
		warningRows_[i].second->setVisible(shown);
		warningRows_[i].second->setText(shown ? texts[static_cast<int>(i)] : QString());
	}
	const int count = static_cast<int>(texts.size());
	warningsList_->setVisible(count > 0);
	warningsSection_->count->setText(QString::number(count));
	warningsSection_->count->setVisible(count > 0);
	ui::SetStyleProperty(warningsSection_->count, "tgTone", QStringLiteral("warning"));
	// Opened when the first warning shows up; after that the streamer decides.
	if (count > 0 && lastWarningCount_ == 0)
		SetSectionOpen(*warningsSection_, true);
	lastWarningCount_ = count;
}

void StreamModePanel::RefreshPresetChips(uint32_t delaySeconds)
{
	for (int i = 0; i < 3; ++i) {
		const uint32_t value = controller_.Preset(i);
		presetChips_[i]->setText(Tr("Dock.Status.Seconds").arg(value));
		presetChips_[i]->setAccessibleName(Tr("Stream.Preset.Label").arg(i + 1) + QStringLiteral(": ") +
						   Tr("Dock.Status.Seconds").arg(value));
		ui::SetStyleProperty(presetChips_[i], "tgActive",
				     value == delaySeconds ? QStringLiteral("true") : QStringLiteral("false"));
	}
}

void StreamModePanel::RefreshModeLines(const StreamDelayStatus &status)
{
	// Which mode the hotkeys and Stream Deck drive right now, and why scene
	// mode is folded (or not).
	const bool handles = controller_.HandlesControls();
	QString hotkeys;
	if (!status.available)
		hotkeys = Tr("Stream.Unavailable");
	else if (handles)
		hotkeys = Tr("Stream.Hotkeys.Stream");
	else if (!status.modeEnabled)
		hotkeys = Tr("Stream.Hotkeys.SceneOff");
	else
		hotkeys = Tr("Stream.Hotkeys.SceneThis");
	hotkeysLabel_->setText(hotkeys);
	if (sceneHint_)
		sceneHint_->setText(handles ? Tr("Stream.Scene.Folded") : Tr("Stream.Scene.Active"));
}

void StreamModePanel::Refresh()
{
	if (detached_ || !themeApplied_)
		return;
	refreshing_ = true;
	const StreamDelayStatus status = controller_.GetStatus();

	modeCheck_->setEnabled(status.available);
	modeCheck_->setChecked(status.available && status.modeEnabled);
	controlsBox_->setVisible(status.available && status.modeEnabled);
	if (delaySpin_->value() != static_cast<int>(status.delaySeconds))
		delaySpin_->setValue(static_cast<int>(status.delaySeconds));
	// While a stream runs, what the relay does; before, what it will do
	// (the spill, unless the disk then can't be used).
	const bool relayRunning = status.engaged || status.state == StreamDelayState::Draining;
	const bool onDisk = relayRunning ? status.relay.spillActive : true;
	const uint32_t instant =
		relayRunning ? status.relay.instantRaiseSeconds
			     : (onDisk ? relay::kMaxDelaySeconds : std::max(status.delaySeconds, status.keepSeconds));
	keepValue_->setText(Tr(onDisk ? "Stream.Keep.Disk" : "Stream.Keep.Ram").arg(instant));
	keepHelp_->setText(Tr(onDisk ? "Stream.KeepHelp" : "Stream.KeepHelp.Ram"));
	keepValue_->setAccessibleName(Tr("Stream.Keep"));
	keepValue_->setAccessibleDescription(keepHelp_->text());
	for (int i = 0; i < 3; ++i) {
		const int preset = static_cast<int>(controller_.Preset(i));
		if (presetSpins_[i]->value() != preset)
			presetSpins_[i]->setValue(preset);
	}
	drainCheck_->setChecked(status.drainOnStop);
	overlayCheck_->setChecked(status.showOverlay);
	cornerCombo_->setCurrentIndex(static_cast<int>(status.overlayCorner));
	cornerCombo_->setEnabled(status.showOverlay);
	const bool atMax = status.delaySeconds >= relay::kMaxDelaySeconds;
	lessButton_->setEnabled(status.delaySeconds > 1);
	moreButton_->setEnabled(!atMax);

	RefreshStatus(status);
	RefreshAccount();
	RefreshDestination(status);
	RefreshWarnings(status);
	RefreshPresetChips(status.delaySeconds);
	RefreshModeLines(status);
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
