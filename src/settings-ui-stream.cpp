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

// The dock's parts that only "Apply the delay only to the stream" shows
// (stream mode, StreamDelayController): where it sends, the relay's stats,
// Dump, the settings to check, Delay by scene and the delay buffer, plus the
// style rules for the dock's own components. TrigglowDelayDock members; the
// rest of the dock is settings-ui.cpp.

#include "settings-ui.hpp"

#include <QCheckBox>
#include <QComboBox>
#include <QFrame>
#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QRegularExpression>
#include <QResizeEvent>
#include <QSpinBox>
#include <QStyleOptionComboBox>
#include <QStylePainter>
#include <QVBoxLayout>

#include <algorithm>
#include <cmath>

extern "C" {
#include <obs-frontend-api.h>
}

#include "auth-manager.hpp"
#include "packet-delay-core.hpp"
#include "settings-ui-parts.hpp"
#include "stream-delay-controller.hpp"

namespace trigglow {

using namespace dockparts;

namespace {

QString SinkStateText(relay::SinkState state)
{
	switch (state) {
	case relay::SinkState::Waiting:
		return T("Stream.SinkState.Waiting");
	case relay::SinkState::Connecting:
		return T("Stream.SinkState.Connecting");
	case relay::SinkState::Live:
		return T("Stream.SinkState.Live");
	case relay::SinkState::Reconnecting:
		return T("Stream.SinkState.Reconnecting");
	case relay::SinkState::Failed:
		return T("Stream.SinkState.Failed");
	case relay::SinkState::Done:
		return T("Stream.SinkState.Done");
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

// Two labels side by side when both fit on one line, the second under the
// first when they don't (narrow docks, longer translations). The labels wrap
// as a last resort, so the row never widens the dock. A `chip` second label
// (a pill: "Sending · 2.5 Mbps") never wraps, keeps its own width and sits
// at the left when it goes under the first.
class FitRow final : public QWidget {
public:
	FitRow(QLabel *first, QLabel *second, QWidget *parent, bool chip = false)
		: QWidget(parent),
		  first_(first),
		  second_(second),
		  chip_(chip)
	{
		first_->setWordWrap(true);
		second_->setWordWrap(!chip);
		layout_ = new QBoxLayout(QBoxLayout::LeftToRight, this);
		layout_->setSizeConstraint(QLayout::SetNoConstraint);
		layout_->setContentsMargins(0, 0, 0, 0);
		layout_->setSpacing(kGap);
		layout_->addWidget(first, 1);
		layout_->addWidget(second, 0, chip ? Qt::AlignRight | Qt::AlignVCenter : Qt::Alignment());
		if (!chip)
			second_->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
	}

	// After a label's text changed.
	void Refit() { Fit(width()); }

protected:
	void resizeEvent(QResizeEvent *event) override
	{
		QWidget::resizeEvent(event);
		Fit(event->size().width());
	}

private:
	static constexpr int kGap = 8;

	void Fit(int width)
	{
		if (width <= 0)
			return;
		const QMargins m = contentsMargins();
		const int secondWidth = second_->isHidden() ? 0
					: chip_             ? second_->sizeHint().width()
							    : TextWidth(second_);
		const bool fits = TextWidth(first_) + kGap + secondWidth <= width - m.left() - m.right();
		const auto direction = fits ? QBoxLayout::LeftToRight : QBoxLayout::TopToBottom;
		if (layout_->direction() == direction)
			return;
		layout_->setDirection(direction);
		layout_->setSpacing(fits ? kGap : (chip_ ? 4 : 2));
		if (chip_)
			layout_->setAlignment(second_, (fits ? Qt::AlignRight : Qt::AlignLeft) | Qt::AlignVCenter);
		else
			second_->setAlignment((fits ? Qt::AlignRight : Qt::AlignLeft) | Qt::AlignVCenter);
		updateGeometry();
	}

	// One line of the label's text (a wrapping label's size hint isn't).
	static int TextWidth(const QLabel *label)
	{
		const QMargins m = label->contentsMargins();
		return label->fontMetrics().horizontalAdvance(label->text()) + m.left() + m.right() +
		       2 * label->margin() + 2;
	}

	QLabel *first_;
	QLabel *second_;
	bool chip_ = false;
	QBoxLayout *layout_ = nullptr;
};

// A combo box whose closed text ends in "..." when the dock is too narrow for
// it, instead of being cut off (the list still shows the whole text).
class ElidingComboBox final : public QComboBox {
public:
	using QComboBox::QComboBox;

protected:
	void paintEvent(QPaintEvent *) override
	{
		QStylePainter painter(this);
		QStyleOptionComboBox option;
		initStyleOption(&option);
		painter.drawComplexControl(QStyle::CC_ComboBox, option);
		const QRect field =
			style()->subControlRect(QStyle::CC_ComboBox, &option, QStyle::SC_ComboBoxEditField, this);
		option.currentText = option.fontMetrics.elidedText(option.currentText, Qt::ElideRight, field.width());
		painter.drawControl(QStyle::CE_ComboBoxLabel, option);
	}
};

} // namespace

// ---- Building ----------------------------------------------------------------

void TrigglowDelayDock::BuildStreamStatusRows(QVBoxLayout *body)
{
	// Where the stream goes: read only, the streamer's own OBS service, with
	// the relay's state and rate while it runs.
	// The chip goes under the service name when both don't fit.
	sendsLabel_ = new QLabel(statusCard_);
	sendsLabel_->setToolTip(T("Stream.SendsToHelp"));
	SetRole(sendsLabel_, "hint", ui::FontRole::Hint);
	sendsState_ = new QLabel(statusCard_);
	SetRole(sendsState_, "chip", ui::FontRole::Chip);
	sendsState_->setVisible(false);
	sendsRow_ = new FitRow(sendsLabel_, sendsState_, statusCard_, /*chip=*/true);
	sendsRow_->setContentsMargins(0, 4, 0, 0);
	sendsRow_->setVisible(false);
	body->addWidget(sendsRow_);

	statsLabel_ = new QLabel(statusCard_);
	statsLabel_->setWordWrap(true);
	SetRole(statsLabel_, "dataHint", ui::FontRole::DataHint);
	statsLabel_->setVisible(false);
	body->addSpacing(2);
	body->addWidget(statsLabel_);

	// The scene rule applied last, and the last problem.
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
}

QFrame *TrigglowDelayDock::BuildWarningsCard()
{
	// OBS settings worth changing for the stream (keyframe interval, OBS's own
	// stream delay): their own amber card, opened the first time something
	// shows up.
	auto *card = new QFrame(this);
	card->setProperty("tgBanner", QStringLiteral("warning"));
	auto *layout = new QVBoxLayout(card);
	layout->setContentsMargins(6, 6, 6, 6);
	layout->setSpacing(0);
	warningsSection_ = AddSection(layout, T("Stream.Section.Warnings"), true);
	warningsSection_->header->setProperty("tgTone", QStringLiteral("warning"));
	warningsLayout_ = qobject_cast<QVBoxLayout *>(warningsSection_->body->layout());
	card->setVisible(false);
	return card;
}

void TrigglowDelayDock::BuildRules(QVBoxLayout *body)
{
	QWidget *host = body->parentWidget();
	auto *help = new QLabel(T("Stream.RulesHelp"), host);
	help->setWordWrap(true);
	SetRole(help, "hint", ui::FontRole::Hint);
	body->addWidget(help);

	rulesList_ = new QVBoxLayout();
	rulesList_->setSpacing(6);
	body->addLayout(rulesList_);
	rulesEmpty_ = new QLabel(T("Stream.Rules.Empty"), host);
	rulesEmpty_->setWordWrap(true);
	SetRole(rulesEmpty_, "dataHint", ui::FontRole::DataHint);
	body->addWidget(rulesEmpty_);

	addRule_ = new QPushButton(T("Stream.Rules.Add"), host);
	addRule_->setProperty("tgVariant", QStringLiteral("secondary"));
	addRule_->setProperty("tgSize", QStringLiteral("large"));
	addRule_->setFocusPolicy(Qt::TabFocus);
	addRule_->setIconSize(kButtonIconBox);
	addRule_->setToolTip(T("Stream.Rules.AddHelp"));
	SetRole(addRule_, "button", ui::FontRole::Button);
	body->addWidget(addRule_);

	for (const SceneRule &rule : streamController_.SceneRules())
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

QWidget *TrigglowDelayDock::BuildBufferBlock(QWidget *host)
{
	// Delay buffer: one combo (Automatic follows the drive type, see
	// drive-kind.hpp), how far a live raise is instant on the eyebrow's row,
	// and one hint line. RefreshBuffer() fills in the texts.
	auto *block = new QWidget(host);
	auto *layout = new QVBoxLayout(block);
	layout->setContentsMargins(0, 0, 0, 0);
	layout->setSpacing(5);
	auto *bufferLabel = MakeEyebrow(T("Stream.Buffer"), nullptr, block);
	bufferInstant_ = new QLabel(block);
	bufferInstant_->setToolTip(T("Stream.Buffer.InstantHelp"));
	SetRole(bufferInstant_, "dataHint", ui::FontRole::DataHint);
	// Side by side, or stacked when the dock is too narrow for both.
	bufferHead_ = new FitRow(bufferLabel, bufferInstant_, block);
	bufferCombo_ = new ElidingComboBox(block);
	// Item 0's text names the detected drive (RefreshBuffer()).
	bufferCombo_->addItems({T("Stream.Buffer.AutoUnknown"), T("Stream.Buffer.Memory"), T("Stream.Buffer.Disk")});
	bufferCombo_->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
	bufferCombo_->setMinimumContentsLength(8);
	bufferCombo_->setAccessibleName(T("Stream.Buffer"));
	bufferCombo_->setToolTip(T("Stream.Buffer.Help"));
	GuardWheel(bufferCombo_);
	bufferLabel->setBuddy(bufferCombo_);
	bufferHint_ = new QLabel(block);
	bufferHint_->setWordWrap(true);
	bufferHint_->setToolTip(T("Stream.Buffer.Help"));
	SetRole(bufferHint_, "hint", ui::FontRole::Hint);
	layout->addWidget(bufferHead_);
	layout->addWidget(bufferCombo_);
	layout->addWidget(bufferHint_);
	connect(bufferCombo_, qOverload<int>(&QComboBox::currentIndexChanged), this, [this](int index) {
		if (refreshing_ || index < 0)
			return;
		streamController_.SetBufferStorage(static_cast<relay::BufferStorage>(index));
		SettingsChanged();
		Refresh();
	});
	return block;
}

// ---- Theme ---------------------------------------------------------------------

QString TrigglowDelayDock::ExtraStyleSheet() const
{
	// Components only this dock has, from the same tokens as ui-theme.cpp's
	// sheet: section headers, preset chips, the 5 s steps, icon buttons and
	// the danger tone of a tag.
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

	s << QStringLiteral("QLabel[tgRole=\"tag\"][tgTone=\"danger\"], QLabel[tgRole=\"chip\"][tgTone=\"danger\"] "
			    "{ color: %1; background-color: %2; border-color: %1; }")
			.arg(c(t.dangerText), c(t.dangerTint));
	// A check box's label while the box is locked.
	s << QStringLiteral("QLabel#tgCheckText:disabled { color: %1; }").arg(disabled);
	return s.join(QLatin1Char('\n'));
}

// ---- Scene rules ---------------------------------------------------------------

void TrigglowDelayDock::AddRuleRow(const std::string &scene, int action, int seconds)
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
	row.scene->setAccessibleName(T("Stream.Rules.Scene"));
	GuardWheel(row.scene);

	row.remove = new QPushButton(row.card);
	row.remove->setProperty("tgVariant", QStringLiteral("icon"));
	row.remove->setFocusPolicy(Qt::TabFocus);
	row.remove->setIconSize(QSize(kInlineIconSize, kInlineIconSize));
	row.remove->setCursor(Qt::PointingHandCursor);

	auto *top = new QHBoxLayout();
	top->setSpacing(6);
	top->addWidget(MakeEyebrow(T("Stream.Rules.Scene"), row.scene, row.card), 1, Qt::AlignBottom);
	top->addWidget(row.remove, 0, Qt::AlignRight);
	layout->addLayout(top);
	layout->addWidget(row.scene);

	row.action = new QComboBox(row.card);
	row.action->addItems({T("Stream.Rules.None"), T("Stream.Rules.On"), T("Stream.Rules.Off")});
	row.action->setCurrentIndex(std::clamp(action, 0, 2));
	row.action->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
	row.action->setMinimumContentsLength(6);
	row.action->setAccessibleName(T("Stream.Rules.Action"));
	GuardWheel(row.action);

	row.seconds = new QSpinBox(row.card);
	row.seconds->setRange(0, static_cast<int>(relay::kMaxDelaySeconds));
	row.seconds->setSpecialValueText(T("Stream.Rules.Keep"));
	row.seconds->setSuffix(T("Dock.Delay.Suffix"));
	row.seconds->setKeyboardTracking(false);
	row.seconds->setValue(seconds);
	row.seconds->setAccessibleName(T("Dock.Delay.Label"));
	row.seconds->setToolTip(T("Stream.Rules.SecondsHelp"));
	row.seconds->setEnabled(row.action->currentIndex() == static_cast<int>(SceneAction::DelayOn));
	GuardWheel(row.seconds);

	layout->addSpacing(3);
	layout->addWidget(MakeEyebrow(T("Stream.Rules.Action"), row.action, row.card));
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
				r.remove->setAccessibleName(T("Stream.Rules.RemoveName").arg(name));
		}
		RulesChanged();
	});
	connect(row.action, qOverload<int>(&QComboBox::currentIndexChanged), this, [this, secondsSpin](int index) {
		secondsSpin->setEnabled(index == static_cast<int>(SceneAction::DelayOn));
		RulesChanged();
	});
	connect(row.seconds, qOverload<int>(&QSpinBox::valueChanged), this, [this] { RulesChanged(); });
	connect(row.remove, &QPushButton::clicked, this, [this, card] { RemoveRuleRow(card); });

	row.remove->setAccessibleName(T("Stream.Rules.RemoveName").arg(row.scene->currentText()));
	row.remove->setToolTip(T("Stream.Rules.Remove"));
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

void TrigglowDelayDock::RemoveRuleRow(QFrame *card)
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

void TrigglowDelayDock::RulesChanged()
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
	streamController_.SetSceneRules(std::move(rules));
	rulesEmpty_->setVisible(ruleRows_.empty());
	rulesSection_->count->setText(QString::number(ruleRows_.size()));
	rulesSection_->count->setVisible(!ruleRows_.empty());
	SettingsChanged();
}

// ---- Refresh -------------------------------------------------------------------

void TrigglowDelayDock::RefreshDestination(const StreamDelayStatus &status)
{
	const bool running = (status.engaged || status.state == StreamDelayState::Draining) &&
			     !status.relay.sinks.empty();
	QString name = running ? QString::fromStdString(status.relay.sinks.front().name) : QString();
	if (name.isEmpty())
		name = ObsServiceName();
	sendsLabel_->setText(name.isEmpty() ? T("Stream.SendsTo.Default") : T("Stream.SendsTo").arg(name));
	sendsState_->setVisible(running);
	auto *row = static_cast<FitRow *>(sendsRow_);
	if (!running) {
		row->Refit();
		return;
	}
	const relay::SinkStatus &sink = status.relay.sinks.front();
	QString state = SinkStateText(sink.state);
	if (sink.state == relay::SinkState::Live)
		state += QStringLiteral(" · ") + QString::number(sink.sentMbps, 'f', 1) + QStringLiteral(" Mbps");
	sendsState_->setText(state);
	ui::SetStyleProperty(sendsState_, "tgTone", SinkStateTone(sink.state));
	row->Refit();
}

void TrigglowDelayDock::RefreshStreamStatus(const StreamDelayStatus &status)
{
	// Each state differs by rule, icon, headline color AND text -- never by
	// color alone (as for all of OBS). Off and On read the same as there.
	QString headline;
	QString detail;
	QString rule = QStringLiteral("none");
	QString tone = QStringLiteral("text");
	QString chip;
	QString chipTone;
	QString stateName;
	ui::IconId icon = ui::IconId::Circle;
	QString iconTone = QStringLiteral("muted");
	const uint32_t applied = status.relay.delaySeconds;
	const QString seconds = applied != status.delaySeconds && applied > 0
					? T("Dock.Status.SecondsOf").arg(applied).arg(status.delaySeconds)
					: T("Dock.Status.Seconds").arg(status.delaySeconds);
	// "Only the stream" was chosen while OBS was already streaming without the
	// relay: it applies from the next stream.
	const bool startedBefore = status.state == StreamDelayState::Idle && status.ineligible.empty() &&
				   obs_frontend_streaming_active();

	if (status.dumpRestoreIn > 0 && status.engaged) {
		stateName = QStringLiteral("stream.dumped");
		headline = T("Stream.Head.Dumped");
		detail = T("Stream.Detail.Dumped").arg(status.dumpRestoreIn);
		rule = QStringLiteral("warning");
		tone = QStringLiteral("warning");
		icon = ui::IconId::Scissors;
		iconTone = QStringLiteral("warning");
		chip = T("Stream.Chip.Back").arg(status.dumpRestoreIn);
		chipTone = QStringLiteral("warning");
	} else if (status.state == StreamDelayState::Idle && (!status.ineligible.empty() || startedBefore)) {
		const std::string key =
			"Stream.Ineligible." + (startedBefore ? std::string("started") : status.ineligible);
		stateName = QStringLiteral("stream.ineligible");
		headline = T("Stream.Head.Ineligible");
		detail = T(key.c_str());
		rule = QStringLiteral("warning");
		tone = QStringLiteral("warning");
		icon = ui::IconId::AlertTriangle;
		iconTone = QStringLiteral("warning");
	} else {
		switch (status.state) {
		case StreamDelayState::Idle:
			if (status.delayOn) {
				stateName = QStringLiteral("stream.armed");
				headline = T("Stream.Head.Armed");
				detail = T("Stream.Detail.Armed").arg(status.delaySeconds);
				icon = ui::IconId::Shield;
				iconTone = QStringLiteral("accent");
				chip = T("Dock.Status.Seconds").arg(status.delaySeconds);
				chipTone = QStringLiteral("accent");
			} else {
				stateName = QStringLiteral("stream.off");
				headline = T("Dock.Status.Inactive");
				detail = T("Stream.Detail.Idle");
			}
			break;
		case StreamDelayState::Live:
			stateName = QStringLiteral("stream.live");
			headline = T("Dock.Status.Inactive");
			detail = T("Stream.Detail.Live");
			break;
		case StreamDelayState::Delayed:
			stateName = QStringLiteral("stream.delayed");
			headline = T("Dock.Status.Active");
			detail = T("Stream.Detail.DelayedFor").arg(applied > 0 ? applied : status.delaySeconds);
			rule = QStringLiteral("active");
			icon = ui::IconId::Shield;
			iconTone = QStringLiteral("accent");
			chip = seconds;
			chipTone = QStringLiteral("accent");
			break;
		case StreamDelayState::Draining:
			stateName = QStringLiteral("stream.draining");
			headline = T("Stream.Head.Draining");
			detail = T("Stream.Detail.DrainingFor").arg(applied);
			rule = QStringLiteral("warning");
			tone = QStringLiteral("warning");
			icon = ui::IconId::Clock;
			iconTone = QStringLiteral("warning");
			chip = T("Dock.Status.Seconds").arg(applied);
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
	statusAccessibleName_ = QStringLiteral("%1: %2. %3").arg(T("Dock.Status.Eyebrow"), headline, detail);
	AnnounceStatus(stateName, false, QStringLiteral("%1. %2").arg(headline, detail));

	QString stats;
	if (status.engaged || status.state == StreamDelayState::Draining) {
		const relay::RelayStatus &r = status.relay;
		QString video = QString::fromStdString(r.videoCodec);
		if (r.width > 0)
			video += QStringLiteral("\u00A0%1x%2").arg(r.width).arg(r.height);
		// RAM and disk separately: with the spill most of the history is on
		// disk and RAM stays small.
		if (r.spillActive || r.historyDiskBytes > 0)
			stats = T("Stream.StatsDisk")
					.arg(QString::number(r.historySeconds, 'f', 0))
					.arg(Megabytes(r.historyRamBytes))
					.arg(Megabytes(r.historyDiskBytes))
					.arg(QString::number(r.bitrateMbps, 'f', 1))
					.arg(video);
		else
			stats = T("Stream.Stats")
					.arg(QString::number(r.historySeconds, 'f', 0))
					.arg(Megabytes(r.historyRamBytes))
					.arg(QString::number(r.bitrateMbps, 'f', 1))
					.arg(video);
	}
	// Narrow docks wrap it between its parts: a number keeps its unit, and a
	// separator ends a line instead of starting the next one.
	static const QRegularExpression numberUnit(QStringLiteral("(\\d) (s|MB|Mbps)\\b"));
	stats.replace(numberUnit, QStringLiteral("\\1\u00A0\\2"));
	stats.replace(QStringLiteral(" \u00B7 "), QStringLiteral("\u00A0\u00B7 "));
	statsLabel_->setText(stats);
	statsLabel_->setVisible(!stats.isEmpty());

	const bool ruleShown = !status.ruleScene.empty() && status.engaged && status.dumpRestoreIn == 0;
	ruleLabel_->setText(ruleShown ? T("Stream.RuleApplied").arg(QString::fromStdString(status.ruleScene))
				      : QString());
	ruleRow_->setVisible(ruleShown);
	noticeLabel_->setText(status.lastEvent.empty()
				      ? QString()
				      : T("Stream.LastEvent").arg(QString::fromStdString(status.lastEvent)));
	noticeRow_->setVisible(!status.lastEvent.empty());

	// The one button, then Dump while it runs and Stop sending while draining.
	const bool loggedIn = authManager_.CanEnable();
	enableButton_->setVisible(!status.delayOn);
	enableButton_->setEnabled(loggedIn);
	disableButton_->setVisible(status.delayOn);
	disableButton_->setEnabled(status.delayOn);
	const bool running = status.delayOn && status.engaged;
	dumpButton_->setVisible(running);
	dumpButton_->setEnabled(status.dumpRestoreIn == 0 && status.relay.delaySeconds > 0);
	cutButton_->setVisible(status.state == StreamDelayState::Draining);

	// Only the account can be missing here (no scene to choose).
	requirements_->setVisible(!status.delayOn && !loggedIn);
	SetIcon(reqAccountIcon_, ui::IconId::Circle, QStringLiteral("muted"), kInlineIconSize);
	ui::SetStyleProperty(reqAccountText_, "tgTone", QStringLiteral("muted"));
	reqAccountText_->setAccessibleName(T("Dock.Requirement.Pending").arg(T("Dock.Requirement.Account")));
	reqSceneIcon_->setVisible(false);
	reqSceneText_->setVisible(false);
}

void TrigglowDelayDock::RefreshWarnings(const StreamDelayStatus &status)
{
	QStringList texts;
	bool keyintWarned = false;
	for (const std::string &w : status.warnings) {
		const size_t colon = w.find(':');
		const std::string name = w.substr(0, colon);
		keyintWarned = keyintWarned || name.rfind("keyint", 0) == 0;
		const std::string key = "Stream.Warn." + name;
		QString text = T(key.c_str());
		if (colon != std::string::npos)
			text = text.arg(QString::fromStdString(w.substr(colon + 1)));
		texts << text;
	}
	// The relay measures the real keyframe interval while streaming.
	const double keyint = status.relay.keyframeIntervalSeconds;
	if (!keyintWarned && (status.engaged || status.state == StreamDelayState::Draining) && keyint > 4.5)
		texts << T("Stream.KeyframeWarning").arg(QString::number(keyint, 'f', 1));

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
	warningsCard_->setVisible(count > 0);
	warningsSection_->count->setText(QString::number(count));
	warningsSection_->count->setVisible(count > 0);
	ui::SetStyleProperty(warningsSection_->count, "tgTone", QStringLiteral("warning"));
	// Opened when the first warning shows up; after that the streamer decides.
	if (count > 0 && lastWarningCount_ == 0)
		SetSectionOpen(*warningsSection_, true);
	lastWarningCount_ = count;
}

void TrigglowDelayDock::RefreshBuffer(const StreamDelayStatus &status)
{
	const char *autoKey = status.driveKind == relay::DriveKind::Ssd   ? "Stream.Buffer.AutoSsd"
			      : status.driveKind == relay::DriveKind::Hdd ? "Stream.Buffer.AutoHdd"
									  : "Stream.Buffer.AutoUnknown";
	bufferCombo_->setItemText(0, T(autoKey));
	bufferCombo_->setCurrentIndex(static_cast<int>(status.bufferStorage));

	// While a stream runs, what its relay does; before, what the next one will
	// do (the spill, unless the disk then can't be used).
	const bool relayRunning = status.engaged || status.state == StreamDelayState::Draining;
	const relay::RelayStatus &r = status.relay;
	const uint32_t instant = relayRunning
					 ? r.instantRaiseSeconds
					 : (status.bufferOnDisk ? relay::kMaxDelaySeconds
								: std::max(status.delaySeconds, status.keepSeconds));
	bufferInstant_->setText(T("Stream.Buffer.Instant").arg(instant));
	static_cast<FitRow *>(bufferHead_)->Refit();

	// Disk asked for, but the spill couldn't start or stopped (disk full,
	// failing, too slow): this stream is in RAM.
	const bool fellBack = relayRunning && status.streamBufferOnDisk && !r.spillActive && !r.spillNote.empty();
	QString hint;
	if (fellBack && status.bufferOnDisk)
		hint = T("Stream.Buffer.HintFallback");
	else
		hint = T(status.bufferOnDisk ? "Stream.Buffer.HintDisk" : "Stream.Buffer.HintMemory");
	if (relayRunning && status.bufferOnDisk != status.streamBufferOnDisk)
		hint += QLatin1Char(' ') + T("Stream.Buffer.HintNext");
	bufferHint_->setText(hint);
	bufferCombo_->setAccessibleDescription(hint + QLatin1Char(' ') + bufferInstant_->text());
}

} // namespace trigglow
