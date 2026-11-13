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

#include "settings-ui.hpp"
#include "api-client.hpp"
#include "auth-manager.hpp"
#include "delay-router.hpp"
#include "diagnostics-collector.hpp"
#include "logging.hpp"
#include "report-bug-dialog.hpp"
#include "scene-combo-box.hpp"
#include "settings-ui-parts.hpp"
#include "stream-delay-controller.hpp"
#include "ui-tasks.hpp"
#include "update-checker.hpp"

#include <plugin-support.h>

#include <algorithm>
#include <cmath>

#include <QAccessible>
#include <QApplication>
#include <QCheckBox>
#include <QClipboard>
#include <QComboBox>
#include <QDesktopServices>
#include <QEvent>
#include <QFrame>
#include <QGuiApplication>
#include <QHBoxLayout>
#include <QLabel>
#include <QMouseEvent>
#include <QPainter>
#include <QPixmap>
#include <QPointer>
#include <QProgressBar>
#include <QPushButton>
#include <QResizeEvent>
#include <QScrollArea>
#include <QSignalBlocker>
#include <QSpinBox>
#include <QString>
#include <QTimer>
#include <QUrl>
#include <QVBoxLayout>
#include <QWheelEvent>

namespace trigglow {

namespace {
using namespace dockparts;

constexpr const char *kComponent = "settings-ui";

// The lock inside a locked field, where its arrows were.
constexpr int kFieldLockSize = 12;

// The seconds viewers really get: the effective delay (whole seconds, the same
// for audio and video) once the controller knows it, never above the
// configured one.
uint32_t AppliedDelaySeconds(const BufferModeStatus &status)
{
	return status.effectiveDelaySeconds > 0 ? std::min(status.effectiveDelaySeconds, status.delaySeconds)
						: status.delaySeconds;
}

// "60", "30" or "29,97": whole rates without decimals.
QString FormatFps(const QLocale &locale, double fps)
{
	const double whole = std::round(fps);
	return std::abs(fps - whole) < 0.01 ? locale.toString(static_cast<int>(whole)) : locale.toString(fps, 'f', 2);
}

// Per-minute rates: one decimal below 10 ("2,4"), none above ("37").
QString FormatRate(const QLocale &locale, double perMinute)
{
	return locale.toString(perMinute, 'f', perMinute < 10.0 ? 1 : 0);
}

const char *HealthCauseKey(PipelineLoad cause)
{
	switch (cause) {
	case PipelineLoad::Graphics:
		return "Dock.Health.Cause.Graphics";
	case PipelineLoad::Encode:
		return "Dock.Health.Cause.Encode";
	case PipelineLoad::Decode:
		return "Dock.Health.Cause.Decode";
	case PipelineLoad::None:
		break;
	}
	return "Dock.Health.Cause.Other";
}
} // namespace

// A pixmap painted as is, whatever the enabled state: a QLabel grays its
// pixmap out when disabled, and the field lock lives inside a disabled field.
class FieldIcon : public QWidget {
public:
	explicit FieldIcon(QWidget *parent) : QWidget(parent) { setAttribute(Qt::WA_TransparentForMouseEvents); }

	void SetPixmap(const QPixmap &pixmap)
	{
		pixmap_ = pixmap;
		update();
	}

protected:
	void paintEvent(QPaintEvent * /*event*/) override
	{
		if (pixmap_.isNull())
			return;
		QPainter painter(this);
		const QSizeF size = pixmap_.deviceIndependentSize();
		painter.drawPixmap(QPointF((width() - size.width()) / 2.0, (height() - size.height()) / 2.0), pixmap_);
	}

private:
	QPixmap pixmap_;
};

TrigglowDelayDock::TrigglowDelayDock(BufferModeController &bufferController, StreamDelayController &streamController,
				     DelayRouter &router, AuthManager &authManager, QWidget *parent)
	: QWidget(parent),
	  bufferController_(bufferController),
	  streamController_(streamController),
	  router_(router),
	  authManager_(authManager),
	  updateTasks_(uitasks::Registry())
{
	BuildUi();
	CheckForUpdateAsync();

	bufferController_.SetStatusChangedCallback([this](const BufferModeStatus &status) { OnStatusChanged(status); });
	bufferController_.SetSceneListRefreshCallback([this] { RefreshAvailableScenes(); });
	bufferController_.SetThemeChangedCallback([this] { ApplyTheme(); });
	authManager_.SetStatusChangedCallback([this] { RefreshAccountUi(); });
	// Styles everything, then cascades into RefreshAccountUi() -> Refresh(),
	// so this alone initializes the whole dock's state.
	ApplyTheme(/*force=*/true);
}

void TrigglowDelayDock::SetRole(QWidget *widget, const char *qssRole, ui::FontRole fontRole)
{
	SetRole(widget, qssRole);
	fontRoles_.emplace_back(widget, fontRole);
}

void TrigglowDelayDock::SetRole(QWidget *widget, const char *qssRole)
{
	widget->setProperty("tgRole", QString::fromLatin1(qssRole));
}

QLocale TrigglowDelayDock::UiLocale() const
{
	// OBS's UI language, not the OS one: numbers follow the strings around
	// them ("15.910 MB" and "41,2 s" in Spanish).
	const char *locale = obs_get_locale();
	return locale && *locale ? QLocale(QString::fromUtf8(locale).replace(QLatin1Char('-'), QLatin1Char('_')))
				 : QLocale();
}

QLabel *TrigglowDelayDock::MakeEyebrow(const QString &text, QWidget *buddy, QWidget *parent)
{
	// Field labels ("LIVE SCENE") sit ABOVE their control, in mono uppercase,
	// and are tied to it with setBuddy() for screen readers.
	auto *label = new QLabel(text, parent ? parent : this);
	SetRole(label, "eyebrow", ui::FontRole::Eyebrow);
	if (buddy)
		label->setBuddy(buddy);
	return label;
}

void TrigglowDelayDock::AddFieldLock(QWidget *field)
{
	// A lock where a locked field's arrows were (ui-theme.cpp hides them while
	// the field is disabled), so the field itself says it can't change now;
	// shown by RefreshSceneFields() while the delay runs. Its own layout on the
	// field only places the lock: the field's value and popup are untouched.
	auto *layout = new QHBoxLayout(field);
	layout->setSizeConstraint(QLayout::SetNoConstraint);
	layout->setContentsMargins(0, 0, 9, 0);
	layout->setSpacing(0);
	layout->addStretch(1);
	auto *lock = new FieldIcon(field);
	lock->setFixedSize(kFieldLockSize, kFieldLockSize);
	lock->setVisible(false);
	layout->addWidget(lock, 0, Qt::AlignVCenter);
	fieldLocks_.emplace_back(field, lock);
}

QWidget *TrigglowDelayDock::MakeNoteRow(QWidget *parent, QLabel *&icon, const QString &text)
{
	// An inline icon and a wrapping hint: the lock and info lines.
	auto *row = new QWidget(parent);
	auto *layout = new QHBoxLayout(row);
	layout->setContentsMargins(0, 2, 0, 0);
	layout->setSpacing(6);
	icon = MakeIconLabel(row, kInlineIconSize);
	auto *label = new QLabel(text, row);
	label->setWordWrap(true);
	SetRole(label, "hint", ui::FontRole::Hint);
	layout->addWidget(icon, 0, Qt::AlignTop);
	layout->addWidget(label, 1);
	row->setVisible(false);
	return row;
}

QWidget *TrigglowDelayDock::MakeCheckRow(QWidget *parent, QCheckBox *&check, QLabel *&text, const QString &label)
{
	// The check box's text as a word-wrapping label (a QCheckBox can't wrap,
	// which pushed narrow docks wider than their window). Clicking it toggles
	// (eventFilter()).
	auto *row = new QWidget(parent);
	auto *layout = new QHBoxLayout(row);
	layout->setContentsMargins(0, 0, 0, 0);
	layout->setSpacing(8);
	check = new QCheckBox(row);
	check->setFocusPolicy(Qt::TabFocus);
	check->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);
	check->setAccessibleName(label);
	text = new QLabel(label, row);
	text->setObjectName(QStringLiteral("tgCheckText")); // Dims while the box is locked (ExtraStyleSheet()).
	text->setWordWrap(true);
	text->setBuddy(check);
	text->setCursor(Qt::PointingHandCursor);
	text->installEventFilter(this);
	layout->addWidget(check, 0, Qt::AlignTop);
	layout->addWidget(text, 1);
	return row;
}

void TrigglowDelayDock::GuardWheel(QWidget *field)
{
	// See eventFilter(): the wheel over a field without focus scrolls the dock.
	field->setFocusPolicy(Qt::StrongFocus);
	field->installEventFilter(this);
}

void TrigglowDelayDock::BuildUi()
{
	// Everything lives in a scroll area: when the dock is dragged small (in
	// either direction) the content keeps its natural size and scrolls,
	// instead of Qt squashing rows on top of each other. Rows that can
	// reflow (see ApplyNarrowLayout) switch to a column below kNarrowWidth.
	auto *outer = new QVBoxLayout(this);
	outer->setContentsMargins(0, 0, 0, 0);
	auto *scroll = new QScrollArea(this);
	scroll->setObjectName(QStringLiteral("tgDockScroll"));
	// Out of the tab order: as a focus stop it was the dock's first one and
	// showed no focus at all. Keyboard users still scroll it: QScrollArea
	// brings each focused child into view.
	scroll->setFocusPolicy(Qt::NoFocus);
	scroll->setWidgetResizable(true);
	scroll->setFrameShape(QFrame::NoFrame);
	scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
	auto *content = new QWidget(scroll);
	content->setObjectName(QStringLiteral("tgDockContent"));
	scroll->setWidget(content);
	outer->addWidget(scroll);

	auto *root = new QVBoxLayout(content);
	root->setContentsMargins(12, 12, 12, 12);
	root->setSpacing(10);

	// --- Update notice: hidden until/unless CheckForUpdateAsync() (fired
	// from the constructor) actually finds a newer published release. Above
	// even the status card -- rare and important when it does show, and
	// costs nothing (no reserved space) while hidden. The whole banner is a
	// single focusable button with a layout inside. ---
	updateNoticeButton_ = new QPushButton(this);
	updateNoticeButton_->setObjectName(QStringLiteral("tgUpdateBanner"));
	updateNoticeButton_->setCursor(Qt::PointingHandCursor);
	updateNoticeButton_->setFocusPolicy(Qt::TabFocus);
	updateNoticeButton_->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Minimum);
	updateNoticeButton_->setVisible(false);
	{
		auto *bannerLayout = new QHBoxLayout(updateNoticeButton_);
		bannerLayout->setContentsMargins(10, 8, 10, 8);
		bannerLayout->setSpacing(10);
		updateIcon_ = MakeIconLabel(updateNoticeButton_, kAccountIconSize);
		auto *texts = new QVBoxLayout();
		texts->setSpacing(1);
		updateTitle_ = new QLabel(T("Dock.Update.Title"), updateNoticeButton_);
		SetRole(updateTitle_, "strong", ui::FontRole::Strong);
		updateVersion_ = new QLabel(updateNoticeButton_);
		SetRole(updateVersion_, "dataHint", ui::FontRole::DataHint);
		texts->addWidget(updateTitle_);
		texts->addWidget(updateVersion_);
		updateExternalIcon_ = MakeIconLabel(updateNoticeButton_, kInlineIconSize);
		bannerLayout->addWidget(updateIcon_, 0, Qt::AlignTop);
		bannerLayout->addLayout(texts, 1);
		bannerLayout->addWidget(updateExternalIcon_, 0, Qt::AlignVCenter);
		for (QLabel *label : {updateIcon_, updateTitle_, updateVersion_, updateExternalIcon_})
			label->setAttribute(Qt::WA_TransparentForMouseEvents);
	}
	root->addWidget(updateNoticeButton_);
	connect(updateNoticeButton_, &QPushButton::clicked, this, [this] {
		if (!updateReleaseUrl_.isEmpty())
			QDesktopServices::openUrl(QUrl(updateReleaseUrl_));
	});

	// --- Status card: rule, eyebrow + chip, icon + headline, detail, then
	// what only one mode has (the fill progress and the degradation notice
	// for all of OBS; where it sends, stats and notes for only the stream).
	// Always the first thing the user sees. ---
	statusCard_ = new QFrame(this);
	statusCard_->setProperty("tgCard", QStringLiteral("true"));
	statusCard_->setFocusPolicy(Qt::NoFocus);
	{
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
		eyebrowRow->addWidget(MakeEyebrow(T("Dock.Status.Eyebrow"), nullptr, statusCard_));
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
		stateLabel_->setWordWrap(true); // Below ~250 px "NO SE PUDO ACTIVAR" wraps instead of clipping.
		SetRole(stateLabel_, "headline", ui::FontRole::Headline);
		headlineRow->addWidget(statusIcon_, 0, Qt::AlignVCenter);
		headlineRow->addWidget(stateLabel_, 1);
		body->addLayout(headlineRow);

		detailLabel_ = new QLabel(statusCard_);
		detailLabel_->setWordWrap(true);
		SetRole(detailLabel_, "muted");
		body->addWidget(detailLabel_);

		fillRow_ = new QWidget(statusCard_);
		auto *fillLayout = new QVBoxLayout(fillRow_);
		fillLayout->setContentsMargins(0, 8, 0, 0);
		fillLayout->setSpacing(5);
		fillBar_ = new QProgressBar(fillRow_);
		fillBar_->setTextVisible(false);
		fillBar_->setFixedHeight(4);
		auto *fillTexts = new QHBoxLayout();
		fillProgressLabel_ = new QLabel(fillRow_);
		SetRole(fillProgressLabel_, "hint", ui::FontRole::Hint);
		fillRemainingLabel_ = new QLabel(fillRow_);
		SetRole(fillRemainingLabel_, "dataHint", ui::FontRole::DataHint);
		fillRemainingLabel_->setProperty("tgTone", QStringLiteral("warning"));
		fillTexts->addWidget(fillProgressLabel_);
		fillTexts->addStretch(1);
		fillTexts->addWidget(fillRemainingLabel_);
		fillLayout->addWidget(fillBar_);
		fillLayout->addLayout(fillTexts);
		fillRow_->setVisible(false);
		body->addWidget(fillRow_);

		BuildStreamStatusRows(body);

		// Degradation notice (RefreshPipelineHealth): below everything the
		// state says, after a hairline, so the headline and the seconds chip
		// stay what the card is about -- the delay itself is intact. Amber
		// only on the icon and the title; the explanation is plain muted text.
		healthNotice_ = new QWidget(statusCard_);
		auto *healthLayout = new QVBoxLayout(healthNotice_);
		healthLayout->setContentsMargins(0, 8, 0, 0);
		healthLayout->setSpacing(4);
		healthLayout->addWidget(MakeDivider(healthNotice_));
		healthLayout->addSpacing(4);
		auto *healthTitleRow = new QHBoxLayout();
		healthTitleRow->setSpacing(6);
		healthIcon_ = MakeIconLabel(healthNotice_, kInlineIconSize);
		healthTitle_ = new QLabel(T("Dock.Health.Title"), healthNotice_);
		healthTitle_->setWordWrap(true);
		SetRole(healthTitle_, "strong", ui::FontRole::Strong);
		healthTitle_->setProperty("tgTone", QStringLiteral("warning"));
		healthTitleRow->addWidget(healthIcon_, 0, Qt::AlignVCenter);
		healthTitleRow->addWidget(healthTitle_, 1);
		healthLayout->addLayout(healthTitleRow);
		healthBody_ = new QLabel(healthNotice_);
		healthBody_->setWordWrap(true);
		SetRole(healthBody_, "muted");
		healthLayout->addWidget(healthBody_);
		healthStats_ = new QLabel(healthNotice_);
		healthStats_->setWordWrap(true);
		SetRole(healthStats_, "hint", ui::FontRole::Hint);
		healthStats_->setVisible(false);
		healthLayout->addWidget(healthStats_);
		healthNotice_->setVisible(false);
		body->addWidget(healthNotice_);
		cardLayout->addLayout(body);
	}
	root->addWidget(statusCard_);

	// --- Free-account row -- see RefreshAccountUi(). Its own card, right
	// under the status, so it's the first thing a logged-out user notices,
	// before they've configured anything and hit an Error state at Enable()
	// time. One for both modes. ---
	auto *accountCard = new QFrame(this);
	accountCard->setProperty("tgCard", QStringLiteral("true"));
	{
		auto *accountColumn = new QVBoxLayout(accountCard);
		accountColumn->setContentsMargins(12, 10, 12, 10);
		accountColumn->setSpacing(8);
		auto *accountRow = new QHBoxLayout();
		accountRow->setSpacing(10);
		auto *accountLayout = new QBoxLayout(QBoxLayout::LeftToRight);
		accountLayout_ = accountLayout;
		accountLayout->setSpacing(10);
		accountIcon_ = MakeIconLabel(accountCard, kAccountIconSize);
		accountLabel_ = new QLabel(accountCard);
		accountLabel_->setWordWrap(true);
		accountLabel_->setTextFormat(Qt::RichText);
		accountButton_ = new QPushButton(accountCard);
		accountButton_->setProperty("tgVariant", QStringLiteral("secondary"));
		accountButton_->setFocusPolicy(Qt::TabFocus);
		SetRole(accountButton_, "button", ui::FontRole::Button);
		accountLayout->addWidget(accountLabel_, /*stretch=*/1);
		accountLayout->addWidget(accountButton_, 0, Qt::AlignLeft | Qt::AlignVCenter);
		accountRow->addWidget(accountIcon_, 0, Qt::AlignVCenter);
		accountRow->addLayout(accountLayout, 1);
		accountColumn->addLayout(accountRow);

		signInButton_ = new QPushButton(accountCard);
		signInButton_->setProperty("tgVariant", QStringLiteral("primary"));
		signInButton_->setFocusPolicy(Qt::TabFocus);
		signInButton_->setCursor(Qt::PointingHandCursor);
		signInButton_->setToolTip(T("Dock.Account.OpensBrowser"));
		signInButton_->setIconSize(kButtonIconBox);
		signInButton_->setLayoutDirection(Qt::RightToLeft); // Icon after the text: it opens the browser.
		SetRole(signInButton_, "button", ui::FontRole::Button);
		accountColumn->addWidget(signInButton_);

		copyLinkButton_ = new QPushButton(T("Dock.Account.CopyLink"), accountCard);
		copyLinkButton_->setProperty("tgVariant", QStringLiteral("secondary"));
		copyLinkButton_->setProperty("tgSize", QStringLiteral("large"));
		copyLinkButton_->setFocusPolicy(Qt::TabFocus);
		copyLinkButton_->setIconSize(kButtonIconBox);
		SetRole(copyLinkButton_, "button", ui::FontRole::Button);
		copyLinkButton_->setVisible(false);
		accountColumn->addWidget(copyLinkButton_);
	}
	root->addWidget(accountCard);

	// --- The one primary action, full width: "Turn delay on" (brand
	// gradient) while off, "Turn delay off" (tinted danger) from the moment
	// it starts until it is switched off again. Through the router, like the
	// hotkeys: the selected mode only. ---
	auto *buttonRow = new QBoxLayout(QBoxLayout::LeftToRight);
	buttonRow_ = buttonRow;
	buttonRow->setSpacing(10);
	enableButton_ = new QPushButton(T("Dock.Enable"), this);
	enableButton_->setProperty("tgVariant", QStringLiteral("primary"));
	disableButton_ = new QPushButton(T("Dock.Disable"), this);
	disableButton_->setProperty("tgVariant", QStringLiteral("danger"));
	// Only the stream, while it runs: Dump (what viewers haven't seen yet is
	// never sent) and, after OBS stopped, Stop sending now.
	dumpButton_ = new QPushButton(T("Stream.DumpShort"), this);
	dumpButton_->setToolTip(T("Stream.DumpHelp"));
	dumpButton_->setAccessibleName(T("Stream.Dump"));
	dumpButton_->setAccessibleDescription(T("Stream.DumpHelp"));
	cutButton_ = new QPushButton(T("Stream.Cut"), this);
	cutButton_->setToolTip(T("Stream.CutHelp"));
	cutButton_->setAccessibleDescription(T("Stream.CutHelp"));
	for (QPushButton *button : {dumpButton_, cutButton_}) {
		button->setProperty("tgVariant", QStringLiteral("secondary"));
		button->setProperty("tgSize", QStringLiteral("large"));
		button->setVisible(false);
	}
	for (QPushButton *button : {enableButton_, disableButton_, dumpButton_, cutButton_}) {
		button->setFocusPolicy(Qt::TabFocus);
		button->setIconSize(kButtonIconBox);
		SetRole(button, "button", ui::FontRole::Button);
	}
	buttonRow->addWidget(enableButton_);
	buttonRow->addWidget(disableButton_);
	root->addLayout(buttonRow);
	root->addWidget(dumpButton_);
	root->addWidget(cutButton_);

	// --- Requirements: only while Enable is disabled, so it never looks
	// clickable-but-broken without saying why. ---
	requirements_ = new QWidget(this);
	{
		auto *reqLayout = new QVBoxLayout(requirements_);
		reqLayout->setContentsMargins(2, 0, 0, 0);
		reqLayout->setSpacing(4);
		auto makeRow = [this, reqLayout](QLabel *&icon, QLabel *&text, const QString &label) {
			auto *row = new QHBoxLayout();
			row->setSpacing(8);
			icon = MakeIconLabel(requirements_, kInlineIconSize);
			text = new QLabel(label, requirements_);
			SetRole(text, "hint", ui::FontRole::Hint);
			row->addWidget(icon, 0, Qt::AlignVCenter);
			row->addWidget(text, 1);
			reqLayout->addLayout(row);
		};
		makeRow(reqAccountIcon_, reqAccountText_, T("Dock.Requirement.Account"));
		makeRow(reqSceneIcon_, reqSceneText_, T("Dock.Requirement.Scene"));
	}
	requirements_->setVisible(false);
	root->addWidget(requirements_);

	hintLabel_ = new QLabel(T("Dock.Hint"), this);
	hintLabel_->setWordWrap(true);
	SetRole(hintLabel_, "hint", ui::FontRole::Hint);
	root->addWidget(hintLabel_);

	root->addWidget(BuildDelayCard());
	scenesCard_ = BuildScenesCard();
	root->addWidget(scenesCard_);
	warningsCard_ = BuildWarningsCard();
	root->addWidget(warningsCard_);

	// --- Everything else, folded: one card, a section each. Delay by scene
	// only shows with "only the stream". ---
	auto *moreCard = new QFrame(this);
	moreCard->setProperty("tgCard", QStringLiteral("true"));
	{
		auto *layout = new QVBoxLayout(moreCard);
		layout->setContentsMargins(6, 6, 6, 6);
		layout->setSpacing(0);
		rulesSection_ = AddSection(layout, T("Stream.Rules"), false);
		BuildRules(qobject_cast<QVBoxLayout *>(rulesSection_->body->layout()));
		rulesDivider_ = MakeDivider(moreCard);
		layout->addWidget(rulesDivider_);
		Section *presets = AddSection(layout, T("Dock.Section.Presets"), false);
		BuildPresets(qobject_cast<QVBoxLayout *>(presets->body->layout()));
		layout->addWidget(MakeDivider(moreCard));
		optionsSection_ = AddSection(layout, T("Dock.Section.Options"), false);
		BuildOptions(qobject_cast<QVBoxLayout *>(optionsSection_->body->layout()));
	}
	root->addWidget(moreCard);

	// The space left over goes above the footer: in a tall dock the footer
	// sits on its bottom edge; when the content is taller it follows the
	// content and scrolls with it.
	root->addStretch(1);

	// --- Footer: version on the left, "Report a problem" on the
	// right. The report link stays enabled in every state -- a user hitting
	// lag or a stuck buffer needs it exactly when things are going wrong. ---
	root->addWidget(MakeDivider(this));
	{
		// Version | link; the link moves under it in narrow docks.
		auto *footer = new QBoxLayout(QBoxLayout::LeftToRight);
		footerRow_ = footer;
		footer->setContentsMargins(0, 0, 0, 0);
		footer->setSpacing(8);
		auto *versionRow = new QHBoxLayout();
		versionRow->setSpacing(8);
		auto *version = new QLabel(T("Dock.Version").arg(QString::fromUtf8(PLUGIN_VERSION)), this);
		SetRole(version, "dataMuted", ui::FontRole::Data);
		reportBugButton_ = new QPushButton(T("Dock.ReportBug.Button"), this);
		reportBugButton_->setProperty("tgVariant", QStringLiteral("link"));
		reportBugButton_->setFocusPolicy(Qt::TabFocus);
		reportBugButton_->setCursor(Qt::PointingHandCursor);
		reportBugButton_->setToolTip(T("Dock.ReportBug.Tooltip"));
		reportBugButton_->setIconSize(kLinkIconBox);
		SetRole(reportBugButton_, "link", ui::FontRole::Strong);
		versionRow->addWidget(version);
		versionRow->addStretch(1);
		footer->addLayout(versionRow, 1);
		footer->addWidget(reportBugButton_, 0, Qt::AlignLeft | Qt::AlignVCenter);
		root->addLayout(footer);
	}

	connect(enableButton_, &QPushButton::clicked, this, [this] {
		TRIGGLOW_LOG_INFO(kComponent, "Turn delay on pressed in dock (%s)",
				  StreamOnly() ? "only the stream" : "all of OBS");
		router_.Enable();
		Refresh();
	});
	connect(disableButton_, &QPushButton::clicked, this, [this] {
		TRIGGLOW_LOG_INFO(kComponent, "Turn delay off pressed in dock (%s)",
				  StreamOnly() ? "only the stream" : "all of OBS");
		router_.Disable();
		Refresh();
	});
	connect(dumpButton_, &QPushButton::clicked, this, [this] {
		router_.Dump();
		Refresh();
	});
	connect(cutButton_, &QPushButton::clicked, this, [this] {
		streamController_.CutDrain();
		Refresh();
	});
	connect(accountButton_, &QPushButton::clicked, this, [this] {
		if (authManager_.IsLoggedIn()) {
			authManager_.Logout();
		} else if (authManager_.IsLoggingIn()) {
			authManager_.CancelLogin();
		}
	});
	connect(signInButton_, &QPushButton::clicked, this, [this] {
		if (authManager_.IsLoggedIn() && authManager_.AccountInactive()) {
			// "Open trigglow.com": the account page explains the account's state.
			QDesktopServices::openUrl(QUrl(QString::fromUtf8(api::web::kAppSupport)));
			return;
		}
		if (!authManager_.IsLoggedIn() && !authManager_.IsLoggingIn()) {
			TRIGGLOW_LOG_INFO(kComponent, "Sign in pressed in dock");
			linkCopied_ = false;
			authManager_.StartLogin();
		}
	});
	connect(copyLinkButton_, &QPushButton::clicked, this, [this] {
		const QString url = QString::fromStdString(authManager_.PendingUrl());
		if (url.isEmpty())
			return;
		QGuiApplication::clipboard()->setText(url);
		linkCopied_ = true;
		RefreshAccountUi();
	});
	connect(reportBugButton_, &QPushButton::clicked, this, [this] { OnReportBugClicked(); });

	fillTimer_ = new QTimer(this);
	fillTimer_->setSingleShot(true);
	connect(fillTimer_, &QTimer::timeout, this, [this] {
		if (!detached_)
			bufferController_.OnFillTimerElapsed();
	});

	fillProgressTimer_ = new QTimer(this);
	connect(fillProgressTimer_, &QTimer::timeout, this, [this] { UpdateFillProgress(); });

	// Started and stopped by Refresh() (scene mode Filling/Active only).
	healthTimer_ = new QTimer(this);
	healthTimer_->setInterval(1000);
	connect(healthTimer_, &QTimer::timeout, this, [this] {
		if (detached_)
			return;
		bufferController_.Tick(); // v0.7.0: the storage codec's once-a-second duties.
		RefreshPipelineHealth(bufferController_.GetStatus());
	});
}

QFrame *TrigglowDelayDock::BuildDelayCard()
{
	// "DELAY": the selected mode's seconds (both change live, 1-600 s), 5 s
	// steps, and the three presets as quick picks (seconds only; the hotkey
	// presets also turn the delay on).
	// Folds like the sections below: the header says "Delay" and, folded,
	// the seconds.
	auto *card = new QFrame(this);
	card->setProperty("tgCard", QStringLiteral("true"));
	auto *cardLayout = new QVBoxLayout(card);
	cardLayout->setContentsMargins(6, 6, 6, 6);
	cardLayout->setSpacing(0);
	delaySection_ = AddSection(cardLayout, T("Dock.Delay.Label"), true);
	auto *layout = qobject_cast<QVBoxLayout *>(delaySection_->body->layout());

	secondsSpin_ = new QSpinBox(card);
	// Typed values apply on Enter or when the field loses focus, not on every
	// digit: "300" must not pass through 3 and 30 on a live stream.
	secondsSpin_->setRange(static_cast<int>(BufferModeController::kMinDelaySeconds),
			       static_cast<int>(BufferModeController::kMaxDelaySeconds));
	secondsSpin_->setKeyboardTracking(false);
	secondsSpin_->setSuffix(T("Dock.Delay.Suffix"));
	secondsSpin_->setAccessibleName(T("Dock.Delay.Label"));
	GuardWheel(secondsSpin_);

	auto *row = new QHBoxLayout();
	row->setSpacing(6);
	row->addWidget(secondsSpin_, 1);
	auto makeStep = [this, card, row](const QString &text, const char *nameKey, int delta) {
		auto *button = new QPushButton(text, card);
		button->setProperty("tgVariant", QStringLiteral("step"));
		button->setFocusPolicy(Qt::TabFocus);
		button->setAccessibleName(T(nameKey));
		button->setToolTip(T(nameKey));
		SetRole(button, "step", ui::FontRole::Data);
		row->addWidget(button);
		connect(button, &QPushButton::clicked, this, [this, delta] {
			router_.AdjustDelay(delta);
			SettingsChanged();
			Refresh();
		});
		return button;
	};
	lessButton_ = makeStep(QStringLiteral("-5"), "Dock.Delay.Less", -5);
	moreButton_ = makeStep(QStringLiteral("+5"), "Dock.Delay.More", 5);
	layout->addLayout(row);
	connect(secondsSpin_, QOverload<int>::of(&QSpinBox::valueChanged), this, [this](int value) {
		if (refreshing_)
			return;
		router_.SetDelay(static_cast<uint32_t>(value));
		SettingsChanged();
		Refresh();
	});

	// "Presets" then the chips; the label goes above them in narrow docks.
	chipRow_ = new QBoxLayout(QBoxLayout::LeftToRight);
	chipRow_->setSpacing(6);
	auto *chipsLabel = new QLabel(T("Dock.QuickPick"), card);
	SetRole(chipsLabel, "hint", ui::FontRole::Hint);
	chipRow_->addWidget(chipsLabel, 0, Qt::AlignLeft | Qt::AlignVCenter);
	auto *chips = new QHBoxLayout();
	chips->setSpacing(6);
	for (int i = 0; i < 3; ++i) {
		auto *chip = new QPushButton(card);
		chip->setProperty("tgVariant", QStringLiteral("chip"));
		chip->setFocusPolicy(Qt::TabFocus);
		chip->setToolTip(T("Dock.PresetHelp").arg(i + 1));
		SetRole(chip, "chip", ui::FontRole::Chip);
		chips->addWidget(chip);
		presetChips_[i] = chip;
		connect(chip, &QPushButton::clicked, this, [this, i] {
			router_.SetDelay(streamController_.Preset(i));
			SettingsChanged();
			Refresh();
		});
	}
	chips->addStretch(1);
	chipRow_->addLayout(chips, 1);
	layout->addLayout(chipRow_);
	return card;
}

QFrame *TrigglowDelayDock::BuildScenesCard()
{
	// All of OBS only: which scene is delayed, and what shows while the delay
	// builds up.
	// Folds too: folded, the header names the live scene.
	auto *card = new QFrame(this);
	card->setProperty("tgCard", QStringLiteral("true"));
	auto *cardLayout = new QVBoxLayout(card);
	cardLayout->setContentsMargins(6, 6, 6, 6);
	cardLayout->setSpacing(0);
	scenesSection_ = AddSection(cardLayout, T("Dock.Section.Scenes"), true);
	auto *layout = qobject_cast<QVBoxLayout *>(scenesSection_->body->layout());
	layout->setSpacing(5);

	liveSceneCombo_ = new SceneComboBox(card);
	liveSceneCombo_->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
	liveSceneCombo_->setMinimumContentsLength(8);
	liveSceneCombo_->setToolTip(T("Dock.Scene.Live.Tooltip"));
	liveSceneCombo_->setAccessibleName(T("Dock.Scene.Live"));
	// Nothing preselected until the user picks a scene (index -1 shows this),
	// so choosing the first one is a real selection, and the field never
	// shows a scene while the requirement list says none is chosen.
	liveSceneCombo_->setPlaceholderText(T("Dock.Scene.Live.Placeholder"));
	liveSceneCombo_->SetRefreshCallback(
		[this] { RefreshSceneCombo(liveSceneCombo_, bufferController_.GetStatus().liveSceneName, false); });
	{
		// "LIVE SCENE *": the asterisk (accent) marks it required, like the
		// web's support form.
		auto *row = new QHBoxLayout();
		row->setSpacing(3);
		row->addWidget(MakeEyebrow(T("Dock.Scene.Live"), liveSceneCombo_, card));
		auto *required = new QLabel(QStringLiteral("*"), card);
		SetRole(required, "eyebrow", ui::FontRole::Eyebrow);
		required->setProperty("tgTone", QStringLiteral("accent"));
		required->setToolTip(T("Dock.Required"));
		required->setAccessibleName(T("Dock.Required"));
		row->addWidget(required);
		row->addStretch(1);
		layout->addLayout(row);
	}
	layout->addWidget(liveSceneCombo_);
	AddFieldLock(liveSceneCombo_);
	RefreshSceneCombo(liveSceneCombo_, bufferController_.GetStatus().liveSceneName, false);

	// v1.0.0: more delayed scenes under the first one, each removable, and
	// "Add scene". One buffer holds them all, so more scenes cost no more RAM.
	extraScenesList_ = new QVBoxLayout();
	extraScenesList_->setSpacing(5);
	layout->addLayout(extraScenesList_);
	addSceneButton_ = new QPushButton(T("Dock.Scene.Add"), card);
	addSceneButton_->setProperty("tgVariant", QStringLiteral("secondary"));
	addSceneButton_->setFocusPolicy(Qt::TabFocus);
	addSceneButton_->setIconSize(kButtonIconBox);
	addSceneButton_->setToolTip(T("Dock.Scene.Add.Tooltip"));
	SetRole(addSceneButton_, "button", ui::FontRole::Button);
	layout->addWidget(addSceneButton_);
	connect(addSceneButton_, &QPushButton::clicked, this, [this] {
		AddExtraSceneRow({});
		RefreshDelayedScenes(bufferController_.GetStatus());
		if (!extraSceneRows_.empty())
			extraSceneRows_.back().combo->setFocus(Qt::TabFocusReason);
	});
	{
		const std::vector<std::string> &saved = bufferController_.GetStatus().liveSceneNames;
		for (size_t i = 1; i < saved.size(); ++i)
			AddExtraSceneRow(saved[i]);
	}

	// While the delay runs, with two or more delayed scenes: one button each.
	// The one pressed goes into the delay; viewers see the switch one delay
	// later, never live.
	switchBlock_ = new QWidget(card);
	{
		auto *switchLayout = new QVBoxLayout(switchBlock_);
		switchLayout->setContentsMargins(0, 4, 0, 0);
		switchLayout->setSpacing(5);
		switchLayout->addWidget(MakeEyebrow(T("Dock.Scene.Switch"), nullptr, switchBlock_));
		switchList_ = new QVBoxLayout();
		switchList_->setSpacing(5);
		switchLayout->addLayout(switchList_);
	}
	switchBlock_->setVisible(false);
	layout->addWidget(switchBlock_);

	layout->addSpacing(7);
	loadingSceneCombo_ = new SceneComboBox(card);
	loadingSceneCombo_->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
	loadingSceneCombo_->setMinimumContentsLength(8);
	loadingSceneCombo_->setToolTip(T("Dock.Scene.Loading.Tooltip"));
	loadingSceneCombo_->setAccessibleName(T("Dock.Scene.Loading"));
	loadingSceneCombo_->SetRefreshCallback([this] {
		RefreshSceneCombo(loadingSceneCombo_, bufferController_.GetStatus().loadingSceneName, true);
	});
	{
		// "LOADING SCENE   Optional": the marker in the hint style, not a
		// second mono uppercase eyebrow, so the two never read as one label
		// when a narrow dock (240 px) leaves no room between them.
		auto *row = new QHBoxLayout();
		row->setSpacing(8);
		row->addWidget(MakeEyebrow(T("Dock.Scene.Loading"), loadingSceneCombo_, card), 0, Qt::AlignBottom);
		row->addStretch(1);
		auto *optional = new QLabel(T("Dock.Optional"), card);
		SetRole(optional, "hint", ui::FontRole::Hint);
		row->addWidget(optional, 0, Qt::AlignBottom);
		layout->addLayout(row);
	}
	layout->addWidget(loadingSceneCombo_);
	AddFieldLock(loadingSceneCombo_);
	RefreshSceneCombo(loadingSceneCombo_, bufferController_.GetStatus().loadingSceneName, true);

	// While the delay runs (Filling, Active) the scenes and the quality are
	// locked: changing them would restart the buffer under a live stream. The
	// seconds aren't (v1.0.0: the delay changes in place). This line says so,
	// visibly, not only in a tooltip (RefreshSceneFields).
	layout->addSpacing(2);
	delayLockedRow_ = MakeNoteRow(card, delayLockedIcon_, T("Dock.Delay.Locked"));
	layout->addWidget(delayLockedRow_);
	// v1.0.0: all of OBS delays the live scene only. Switching Program to
	// another scene while the delay runs would show that scene live, so the
	// dock says what to do instead, while it runs.
	sceneSwitchRow_ = MakeNoteRow(card, sceneSwitchIcon_, T("Dock.SceneSwitch.Note"));
	layout->addWidget(sceneSwitchRow_);
	for (QLabel *label : sceneSwitchRow_->findChildren<QLabel *>()) {
		if (label != sceneSwitchIcon_)
			sceneSwitchText_ = label;
	}

	for (QWidget *control : std::initializer_list<QWidget *>{liveSceneCombo_, loadingSceneCombo_})
		GuardWheel(control);
	connect(liveSceneCombo_, QOverload<int>::of(&QComboBox::currentIndexChanged), this, [this](int) {
		DelayedScenesChanged();
		RefreshFitEstimate(); // Different scene = different resolution/fps to estimate against.
	});
	connect(loadingSceneCombo_, QOverload<int>::of(&QComboBox::currentIndexChanged), this, [this](int index) {
		bufferController_.SetLoadingScene(index <= 0 ? std::string{}
							     : loadingSceneCombo_->currentText().toStdString());
	});
	return card;
}

void TrigglowDelayDock::AddExtraSceneRow(const std::string &scene)
{
	QWidget *host = scenesSection_->body;
	ExtraSceneRow r;
	r.row = new QWidget(host);
	auto *rowLayout = new QHBoxLayout(r.row);
	rowLayout->setContentsMargins(0, 0, 0, 0);
	rowLayout->setSpacing(6);
	r.combo = new SceneComboBox(r.row);
	r.combo->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
	r.combo->setMinimumContentsLength(8);
	r.combo->setPlaceholderText(T("Dock.Scene.Live.Placeholder"));
	r.combo->setAccessibleName(T("Dock.Scene.Live"));
	r.combo->setToolTip(T("Dock.Scene.Live.Tooltip"));
	SceneComboBox *combo = r.combo;
	r.combo->SetRefreshCallback([this, combo] {
		RefreshSceneCombo(combo, combo->currentIndex() < 0 ? std::string{} : combo->currentText().toStdString(),
				  false);
	});
	RefreshSceneCombo(r.combo, scene, false);
	GuardWheel(r.combo);
	AddFieldLock(r.combo);
	if (themeApplied_) {
		// Rows rebuilt after the theme was applied draw their lock now.
		FieldIcon *lock = fieldLocks_.back().second;
		lock->SetPixmap(ui::IconPixmap(ui::IconId::Lock, ToneColor(QStringLiteral("muted")), kFieldLockSize,
					       lock->devicePixelRatioF()));
	}

	r.remove = new QPushButton(r.row);
	r.remove->setProperty("tgVariant", QStringLiteral("icon"));
	r.remove->setFocusPolicy(Qt::TabFocus);
	r.remove->setIconSize(QSize(kInlineIconSize, kInlineIconSize));
	r.remove->setCursor(Qt::PointingHandCursor);
	r.remove->setToolTip(T("Dock.Scene.Remove"));
	r.remove->setAccessibleName(
		T("Dock.Scene.RemoveName")
			.arg(scene.empty() ? T("Dock.Scene.Live.Placeholder") : QString::fromStdString(scene)));
	rowLayout->addWidget(r.combo, 1);
	rowLayout->addWidget(r.remove, 0, Qt::AlignVCenter);
	extraScenesList_->addWidget(r.row);

	QWidget *row = r.row;
	QPushButton *remove = r.remove;
	connect(r.combo, QOverload<int>::of(&QComboBox::currentIndexChanged), this, [this, combo, remove](int) {
		remove->setAccessibleName(T("Dock.Scene.RemoveName").arg(combo->currentText()));
		DelayedScenesChanged();
	});
	connect(r.remove, &QPushButton::clicked, this, [this, row] { RemoveExtraSceneRow(row); });
	if (themeApplied_)
		SetButtonIcon(r.remove, ui::IconId::Trash, ui::ToQColor(theme_.tokens.textMuted), Qt::AlignCenter);
	extraSceneRows_.push_back(r);
}

void TrigglowDelayDock::RemoveExtraSceneRow(QWidget *row)
{
	auto it = std::find_if(extraSceneRows_.begin(), extraSceneRows_.end(),
			       [row](const ExtraSceneRow &r) { return r.row == row; });
	if (it == extraSceneRows_.end())
		return;
	SceneComboBox *combo = it->combo;
	fieldLocks_.erase(std::remove_if(fieldLocks_.begin(), fieldLocks_.end(),
					 [combo](const auto &entry) { return entry.first == combo; }),
			  fieldLocks_.end());
	const size_t index = static_cast<size_t>(it - extraSceneRows_.begin());
	extraSceneRows_.erase(it);
	row->hide();
	row->deleteLater();
	DelayedScenesChanged();
	if (index < extraSceneRows_.size())
		extraSceneRows_[index].remove->setFocus(Qt::TabFocusReason);
	else if (addSceneButton_->isVisible())
		addSceneButton_->setFocus(Qt::TabFocusReason);
}

void TrigglowDelayDock::DelayedScenesChanged()
{
	if (refreshing_ || detached_)
		return;
	std::vector<std::string> names;
	if (liveSceneCombo_->currentIndex() >= 0)
		names.push_back(liveSceneCombo_->currentText().toStdString());
	for (const ExtraSceneRow &r : extraSceneRows_) {
		if (r.combo->currentIndex() >= 0)
			names.push_back(r.combo->currentText().toStdString());
	}
	bufferController_.SetLiveScenes(std::move(names));
}

void TrigglowDelayDock::RefreshDelayedScenes(const BufferModeStatus &status)
{
	const bool busy = status.state == BufferModeState::Filling || status.state == BufferModeState::Active;
	const std::vector<std::string> &list = status.liveSceneNames;

	// The rows follow the list when it changed elsewhere (the saved list
	// restored, a duplicate dropped); a new row with no scene yet stays.
	std::vector<std::string> rowNames;
	for (const ExtraSceneRow &r : extraSceneRows_) {
		if (r.combo->currentIndex() >= 0)
			rowNames.push_back(r.combo->currentText().toStdString());
	}
	const std::vector<std::string> tail(list.size() > 1 ? list.begin() + 1 : list.end(), list.end());
	if (rowNames != tail) {
		for (const ExtraSceneRow &r : extraSceneRows_) {
			SceneComboBox *combo = r.combo;
			fieldLocks_.erase(std::remove_if(fieldLocks_.begin(), fieldLocks_.end(),
							 [combo](const auto &entry) { return entry.first == combo; }),
					  fieldLocks_.end());
			r.row->hide();
			r.row->deleteLater();
		}
		extraSceneRows_.clear();
		for (const std::string &name : tail)
			AddExtraSceneRow(name);
	}
	for (const ExtraSceneRow &r : extraSceneRows_) {
		r.combo->setEnabled(!busy);
		r.remove->setVisible(!busy);
	}
	addSceneButton_->setVisible(!busy && !status.liveSceneName.empty() &&
				    1 + extraSceneRows_.size() < BufferModeController::kMaxDelayedScenes);

	// The switch buttons, rebuilt when the list changed.
	if (switchNames_ != list) {
		for (QPushButton *button : switchButtons_) {
			button->hide();
			button->deleteLater();
		}
		switchButtons_.clear();
		for (const std::string &name : list) {
			const QString text = QString::fromStdString(name);
			auto *button = new QPushButton(switchBlock_);
			button->setProperty("tgVariant", QStringLiteral("chip"));
			button->setFocusPolicy(Qt::TabFocus);
			button->setCursor(Qt::PointingHandCursor);
			button->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Fixed);
			button->setText(button->fontMetrics().elidedText(text, Qt::ElideRight, 220));
			button->setToolTip(T("Dock.Scene.Switch.Tooltip").arg(text));
			button->setAccessibleName(T("Dock.Scene.Switch.Name").arg(text));
			SetRole(button, "chip", ui::FontRole::Chip);
			connect(button, &QPushButton::clicked, this,
				[this, name] { bufferController_.SelectDelayedScene(name); });
			switchList_->addWidget(button);
			switchButtons_.push_back(button);
		}
		switchNames_ = list;
	}
	for (size_t i = 0; i < switchButtons_.size() && i < list.size(); ++i) {
		const bool selected = list[i] == status.selectedSceneName;
		ui::SetStyleProperty(switchButtons_[i], "tgActive",
				     selected ? QStringLiteral("true") : QStringLiteral("false"));
		switchButtons_[i]->setAccessibleDescription(selected ? T("Dock.Scene.Switch.Selected") : QString());
	}
	switchBlock_->setVisible(busy && list.size() >= 2);
	if (sceneSwitchText_)
		sceneSwitchText_->setText(T(list.size() >= 2 ? "Dock.SceneSwitch.NoteMulti" : "Dock.SceneSwitch.Note"));
}

TrigglowDelayDock::Section *TrigglowDelayDock::AddSection(QVBoxLayout *parent, const QString &title, bool open)
{
	auto owned = std::make_unique<Section>();
	Section *section = owned.get();
	sections_.push_back(std::move(owned));

	section->box = new QWidget(parent->parentWidget());
	auto *boxLayout = new QVBoxLayout(section->box);
	boxLayout->setContentsMargins(0, 0, 0, 0);
	boxLayout->setSpacing(0);
	auto *headerRow = new QHBoxLayout();
	headerRow->setContentsMargins(0, 0, 4, 0);
	headerRow->setSpacing(6);
	section->header = new QPushButton(title, section->box);
	section->header->setProperty("tgVariant", QStringLiteral("section"));
	section->header->setCheckable(true);
	section->header->setFocusPolicy(Qt::TabFocus);
	section->header->setCursor(Qt::PointingHandCursor);
	section->header->setIconSize(kSectionIconBox);
	section->header->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
	SetRole(section->header, "section", ui::FontRole::Strong);
	section->count = new QLabel(section->box);
	SetRole(section->count, "tag", ui::FontRole::Chip);
	section->count->setVisible(false);
	headerRow->addWidget(section->header, 1);
	headerRow->addWidget(section->count, 0, Qt::AlignVCenter);
	boxLayout->addLayout(headerRow);

	section->body = new QWidget(section->box);
	auto *bodyLayout = new QVBoxLayout(section->body);
	bodyLayout->setContentsMargins(8, 4, 8, 10);
	bodyLayout->setSpacing(6);
	boxLayout->addWidget(section->body);
	parent->addWidget(section->box);

	connect(section->header, &QPushButton::clicked, this,
		[this, section] { SetSectionOpen(*section, !section->open); });
	SetSectionOpen(*section, open);
	return section;
}

void TrigglowDelayDock::SetSectionOpen(Section &section, bool open)
{
	section.open = open;
	{
		const QSignalBlocker block(section.header);
		section.header->setChecked(open);
	}
	section.body->setVisible(open);
	section.count->setVisible(!open && !section.summary.isEmpty());
	if (!themeApplied_)
		return;
	const QString tone = section.header->property("tgTone").toString();
	SetButtonIcon(section.header, open ? ui::IconId::ChevronDown : ui::IconId::ChevronRight,
		      ToneColor(tone.isEmpty() ? QStringLiteral("text") : tone));
}

void TrigglowDelayDock::SetSectionSummary(Section &section, const QString &summary)
{
	if (section.summary == summary)
		return;
	section.summary = summary;
	// Long scene names are cut so the chip never widens a narrow dock.
	section.count->setText(section.count->fontMetrics().elidedText(summary, Qt::ElideRight, 140));
	section.count->setToolTip(summary);
	section.count->setVisible(!section.open && !summary.isEmpty());
}

void TrigglowDelayDock::BuildPresets(QVBoxLayout *body)
{
	// The three presets, for both modes: each has a hotkey (and a Stream Deck
	// key) that sets its seconds and turns the delay on; the delay card's
	// chips set the seconds only.
	QWidget *host = body->parentWidget();
	auto *help = new QLabel(T("Dock.Presets.Help"), host);
	help->setWordWrap(true);
	SetRole(help, "hint", ui::FontRole::Hint);
	body->addWidget(help);

	presetRow_ = new QBoxLayout(QBoxLayout::LeftToRight);
	presetRow_->setSpacing(8);
	for (int i = 0; i < 3; ++i) {
		auto *column = new QVBoxLayout();
		column->setSpacing(5);
		auto *spin = new QSpinBox(host);
		spin->setRange(1, static_cast<int>(BufferModeController::kMaxDelaySeconds));
		spin->setSuffix(T("Dock.Delay.Suffix"));
		spin->setKeyboardTracking(false);
		spin->setValue(static_cast<int>(streamController_.Preset(i)));
		spin->setToolTip(T("Dock.PresetHelp").arg(i + 1));
		spin->setAccessibleName(T("Dock.Preset.Label").arg(i + 1));
		GuardWheel(spin);
		column->addWidget(MakeEyebrow(T("Dock.Preset.Label").arg(i + 1), spin, host));
		column->addWidget(spin);
		presetRow_->addLayout(column, 1);
		presetSpins_[i] = spin;
		connect(spin, qOverload<int>(&QSpinBox::valueChanged), this, [this, i](int value) {
			if (refreshing_)
				return;
			streamController_.SetPreset(i, static_cast<uint32_t>(value));
			SettingsChanged();
			Refresh();
		});
	}
	body->addLayout(presetRow_);
}

void TrigglowDelayDock::BuildOptions(QVBoxLayout *body)
{
	QWidget *host = body->parentWidget();

	// --- All of OBS: the minimum quality, the RAM estimate and the budget. ---
	qualityBlock_ = new QWidget(host);
	{
		auto *layout = new QVBoxLayout(qualityBlock_);
		layout->setContentsMargins(0, 0, 0, 0);
		layout->setSpacing(5);
		minResolutionCombo_ = new QComboBox(qualityBlock_);
		minResolutionCombo_->addItem(QStringLiteral("480p"), 480);
		minResolutionCombo_->addItem(QStringLiteral("720p"), 720);
		minResolutionCombo_->addItem(QStringLiteral("1080p"), 1080);
		// Floors above 1080p only matter with an OBS output that big: the
		// buffer never exceeds the output (bufferfit::Compute()), so with a
		// 1080p output they buffer at 1080p, like the 1080p entry.
		minResolutionCombo_->addItem(T("Dock.Quality.Option1440"), 1440);
		minResolutionCombo_->addItem(T("Dock.Quality.Option2160"), 2160);
		minResolutionCombo_->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
		minResolutionCombo_->setMinimumContentsLength(10);
		minResolutionCombo_->setToolTip(T("Dock.Quality.Tooltip"));
		minResolutionCombo_->setAccessibleName(T("Dock.Quality.Label"));
		GuardWheel(minResolutionCombo_);
		layout->addWidget(MakeEyebrow(T("Dock.Quality.Label"), minResolutionCombo_, qualityBlock_));
		layout->addWidget(minResolutionCombo_);
		AddFieldLock(minResolutionCombo_);

		// The RAM estimate, live-updated by RefreshFitEstimate() whenever the
		// seconds or the quality change -- see that method and
		// BufferModeController::EstimateBufferFit's comment. Hidden while the
		// degradation notice shows (RefreshPipelineHealth()), which then says
		// what the buffer really holds.
		layout->addSpacing(2);
		fitRow_ = new QWidget(qualityBlock_);
		auto *fitLayout = new QHBoxLayout(fitRow_);
		fitLayout->setContentsMargins(0, 0, 0, 0);
		fitLayout->setSpacing(6);
		fitIcon_ = MakeIconLabel(fitRow_, kInlineIconSize);
		fitLabel_ = new QLabel(fitRow_);
		fitLabel_->setWordWrap(true);
		SetRole(fitLabel_, "hint", ui::FontRole::Hint);
		fitLayout->addWidget(fitIcon_, 0, Qt::AlignTop);
		fitLayout->addWidget(fitLabel_, 1);
		layout->addWidget(fitRow_);

		// Informational only, computed once from real hardware where possible
		// (VideoDelayFilter::GetBufferBudgetBytes, see src/hardware-info.hpp)
		// -- "aconsejar segun el hardware, pero a su eleccion": never
		// restricts the seconds or the quality, just shows what the choices
		// are actually working with.
		const uint64_t budgetMb = bufferController_.GetBufferBudgetBytes() / (1024 * 1024);
		auto *budgetRow = new QHBoxLayout();
		budgetRow->setSpacing(8);
		auto *budgetKey = new QLabel(T("Dock.Budget.Label"), qualityBlock_);
		budgetKey->setWordWrap(true);
		SetRole(budgetKey, "hint", ui::FontRole::Hint);
		budgetValue_ =
			new QLabel(T("Dock.Budget.Value").arg(UiLocale().toString(static_cast<qulonglong>(budgetMb))),
				   qualityBlock_);
		SetRole(budgetValue_, "data", ui::FontRole::Data);
		budgetValue_->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
		budgetKey->setBuddy(budgetValue_);
		budgetRow->addWidget(budgetKey, 1);
		budgetRow->addWidget(budgetValue_, 0);
		layout->addLayout(budgetRow);
	}
	body->addWidget(qualityBlock_);
	connect(minResolutionCombo_, QOverload<int>::of(&QComboBox::currentIndexChanged), this, [this](int index) {
		if (refreshing_ || index < 0)
			return;
		bufferController_.SetMinResolutionHeight(
			static_cast<uint32_t>(minResolutionCombo_->itemData(index).toInt()));
		RefreshFitEstimate();
	});

	// --- Only the stream: the delay buffer and sending the rest on stop. ---
	bufferBlock_ = BuildBufferBlock(host);
	body->addWidget(bufferBlock_);
	drainRow_ = MakeCheckRow(host, drainCheck_, drainText_, T("Stream.Drain"));
	body->addWidget(drainRow_);
	connect(drainCheck_, &QCheckBox::toggled, this, [this](bool on) {
		if (refreshing_)
			return;
		streamController_.SetDrainOnStop(on);
		SettingsChanged();
	});

	// --- The delay badge: one check box and one corner for both modes. ---
	body->addSpacing(2);
	body->addWidget(MakeDivider(host));
	body->addSpacing(2);
	body->addWidget(MakeCheckRow(host, overlayCheck_, overlayText_, T("Dock.Overlay.Label")));
	body->addWidget(MakeCheckRow(host, overlaySecondsCheck_, overlaySecondsText_, T("Dock.Overlay.Seconds.Label")));
	overlaySecondsCheck_->setToolTip(T("Dock.Overlay.Seconds.Tooltip"));
	overlaySecondsCheck_->setAccessibleDescription(T("Dock.Overlay.Seconds.Tooltip"));
	overlaySecondsText_->setToolTip(T("Dock.Overlay.Seconds.Tooltip"));
	overlayCornerCombo_ = new QComboBox(host);
	overlayCornerCombo_->addItem(T("Dock.Overlay.Corner.TopLeft"), 0);
	overlayCornerCombo_->addItem(T("Dock.Overlay.Corner.TopRight"), 1);
	overlayCornerCombo_->addItem(T("Dock.Overlay.Corner.BottomLeft"), 2);
	overlayCornerCombo_->addItem(T("Dock.Overlay.Corner.BottomRight"), 3);
	overlayCornerCombo_->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
	overlayCornerCombo_->setMinimumContentsLength(8);
	overlayCornerCombo_->setToolTip(T("Dock.Overlay.Corner.Tooltip"));
	overlayCornerCombo_->setAccessibleName(T("Dock.Overlay.Position"));
	GuardWheel(overlayCornerCombo_);
	body->addSpacing(2);
	body->addWidget(MakeEyebrow(T("Dock.Overlay.Position"), overlayCornerCombo_, host));
	body->addWidget(overlayCornerCombo_);
	connect(overlayCheck_, &QCheckBox::toggled, this, [this](bool checked) {
		if (refreshing_)
			return;
		bufferController_.SetShowOverlay(checked);
		streamController_.SetShowOverlay(checked);
		SettingsChanged();
		Refresh();
	});
	connect(overlaySecondsCheck_, &QCheckBox::toggled, this, [this](bool checked) {
		if (refreshing_)
			return;
		// Kept by the stream controller for both modes; scene mode's badge
		// asks for its image again.
		streamController_.SetOverlayShowSeconds(checked);
		bufferController_.RefreshOverlay();
		SettingsChanged();
	});
	connect(overlayCornerCombo_, QOverload<int>::of(&QComboBox::currentIndexChanged), this, [this](int index) {
		if (refreshing_ || index < 0)
			return;
		const auto corner = static_cast<uint32_t>(overlayCornerCombo_->itemData(index).toInt());
		bufferController_.SetOverlayCorner(corner);
		streamController_.SetOverlayCorner(corner);
		SettingsChanged();
	});

	// --- Advanced: what the delay applies to. Off (the default): all of OBS.
	// Locked while a delay runs, like the scene pickers. ---
	body->addSpacing(2);
	body->addWidget(MakeDivider(host));
	body->addSpacing(2);
	body->addWidget(MakeEyebrow(T("Dock.Advanced"), nullptr, host));
	body->addWidget(MakeCheckRow(host, applyCheck_, applyText_, T("Dock.ApplyTo.Stream")));
	applyCheck_->setToolTip(T("Dock.ApplyTo.Tooltip"));
	applyText_->setToolTip(T("Dock.ApplyTo.Tooltip"));
	applyCheck_->setAccessibleDescription(T("Dock.ApplyTo.Tooltip"));
	applyHint_ = new QLabel(host);
	applyHint_->setWordWrap(true);
	applyHint_->setToolTip(T("Dock.ApplyTo.Tooltip"));
	applyHint_->setContentsMargins(24, 0, 0, 0); // Under the label, past the box.
	SetRole(applyHint_, "hint", ui::FontRole::Hint);
	body->addWidget(applyHint_);
	applyLockRow_ = MakeNoteRow(host, applyLockIcon_, T("Dock.ApplyTo.Locked"));
	applyLockRow_->setContentsMargins(24, 0, 0, 0);
	body->addWidget(applyLockRow_);
	connect(applyCheck_, &QCheckBox::toggled, this, [this](bool on) {
		if (refreshing_)
			return;
		SetApplyToStream(on);
	});
}

void TrigglowDelayDock::SettingsChanged()
{
	if (onSettingsChanged_)
		onSettingsChanged_();
}

bool TrigglowDelayDock::StreamOnly() const
{
	return streamController_.HandlesControls();
}

void TrigglowDelayDock::SetApplyToStream(bool streamOnly)
{
	// One delay value on screen: the new mode starts from the seconds shown.
	const uint32_t seconds = StreamOnly() ? streamController_.DelaySeconds()
					      : bufferController_.GetStatus().delaySeconds;
	streamController_.SetApplyTo(streamOnly ? ApplyTo::Stream : ApplyTo::All);
	router_.SetDelay(seconds);
	SettingsChanged();
	Refresh();
}

void TrigglowDelayDock::OnStreamStatusChanged()
{
	if (detached_)
		return;
	// Stream mode ticks once a second: the whole dock while it's selected
	// (stats, where it sends), only the advanced check box's lock otherwise.
	if (StreamOnly())
		Refresh();
	else if (themeApplied_)
		RefreshApplyTo(bufferController_.GetStatus(), streamController_.GetStatus());
}

#ifdef TRIGGLOW_TEST_SKIP_ACCOUNT_GATE
QImage TrigglowDelayDock::RenderForTest(int width, bool sectionsOpen, int minHeight)
{
	auto *scroll = findChild<QScrollArea *>(QStringLiteral("tgDockScroll"));
	QWidget *content = scroll ? scroll->widget() : nullptr;
	if (!content || !content->layout())
		return {};
	for (const std::unique_ptr<Section> &section : sections_) {
		if (section.get() != warningsSection_)
			SetSectionOpen(*section, sectionsOpen);
	}
	Refresh();
	ApplyNarrowLayout(width);
	// While resizable, the scroll area snaps the content back to its viewport.
	scroll->setWidgetResizable(false);
	content->ensurePolished();
	content->resize(width, 100);
	content->layout()->activate();
	QLayout *layout = content->layout();
	const int fitted = layout->hasHeightForWidth() ? layout->heightForWidth(width) : layout->sizeHint().height();
	// A taller dock than the content (minHeight) shows where the footer sits.
	const int height = std::max(fitted, minHeight);
	content->resize(width, height);
	layout->activate();
	QImage image(content->size(), QImage::Format_ARGB32_Premultiplied);
	image.fill(QApplication::palette().color(QPalette::Window));
	{
		QPainter painter(&image);
		content->render(&painter, QPoint(), QRegion(), QWidget::DrawChildren);
	}
	// Back to the dock's own size.
	ApplyNarrowLayout(this->width());
	scroll->setWidgetResizable(true);
	return image;
}
#endif

void TrigglowDelayDock::Detach()
{
	detached_ = true;
	for (QTimer *timer : {fillTimer_, fillProgressTimer_, healthTimer_}) {
		if (timer)
			timer->stop();
	}
	setEnabled(false);
}

void TrigglowDelayDock::ApplyTheme(bool force)
{
	if (applyingTheme_)
		return;
	applyingTheme_ = true;
	// The application palette is what OBS sets from the active theme; the
	// dock's own palette could be skewed by our style sheet.
	const double basePt = ui::BaseFontPointSize(this);
	ui::Theme theme = ui::BuildTheme(QApplication::palette(), basePt);
	if (force || !themeApplied_ || theme.styleSheet != theme_.styleSheet) {
		theme_ = std::move(theme);
		themeApplied_ = true;
		setStyleSheet(theme_.styleSheet + QLatin1Char('\n') + ExtraStyleSheet());
		fontRoles_.erase(std::remove_if(fontRoles_.begin(), fontRoles_.end(),
						[](const auto &entry) { return entry.first.isNull(); }),
				 fontRoles_.end());
		for (auto &[widget, role] : fontRoles_) {
			if (widget)
				ui::ApplyFont(widget, role, theme_.basePt);
		}
		ApplyStaticIcons();
		RefreshAccountUi(); // Re-tints every state-dependent icon (cascades into RefreshFromStatus).
		FitUpdateBanner();
	}
	applyingTheme_ = false;
}

void TrigglowDelayDock::FitUpdateBanner()
{
	if (!updateNoticeButton_ || !updateNoticeButton_->layout())
		return;
	updateNoticeButton_->ensurePolished();
	for (QLabel *label : {updateTitle_, updateVersion_})
		label->ensurePolished();
	updateNoticeButton_->setMinimumHeight(updateNoticeButton_->layout()->sizeHint().height());
	updateNoticeButton_->setMaximumHeight(QWIDGETSIZE_MAX);
}

void TrigglowDelayDock::changeEvent(QEvent *event)
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
		// Icon labels hold pixmaps rendered for the old screen's scale.
		if (themeApplied_)
			ApplyTheme(/*force=*/true);
		break;
#endif
	default:
		break;
	}
}

bool TrigglowDelayDock::eventFilter(QObject *watched, QEvent *event)
{
	if (event->type() == QEvent::MouseButtonRelease) {
		// A check box's wrapping label toggles it, like the box's own text.
		QCheckBox *check = nullptr;
		if (watched == overlayText_)
			check = overlayCheck_;
		else if (watched == overlaySecondsText_)
			check = overlaySecondsCheck_;
		else if (watched == drainText_)
			check = drainCheck_;
		else if (watched == applyText_)
			check = applyCheck_;
		if (check) {
			auto *label = static_cast<QLabel *>(watched);
			auto *mouse = static_cast<QMouseEvent *>(event);
			if (mouse->button() == Qt::LeftButton && check->isEnabled() &&
			    label->rect().contains(mouse->position().toPoint()))
				check->click();
			return false;
		}
	}
	if (event->type() == QEvent::Wheel) {
		auto *widget = qobject_cast<QWidget *>(watched);
		if (widget && !widget->hasFocus()) {
			// Ignored + filtered: Qt hands the event to the parent chain,
			// so the scroll area scrolls the dock instead.
			event->ignore();
			return true;
		}
	}
	return QWidget::eventFilter(watched, event);
}

QColor TrigglowDelayDock::ToneColor(const QString &tone) const
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

void TrigglowDelayDock::SetIcon(QLabel *label, ui::IconId icon, const QString &tone, int size)
{
	label->setPixmap(ui::IconPixmap(icon, ToneColor(tone), size, label->devicePixelRatioF()));
}

void TrigglowDelayDock::SetButtonIcon(QPushButton *button, ui::IconId icon, const QColor &color,
				      Qt::Alignment alignment)
{
	button->setIcon(ui::Icon(icon, color, ui::ToQColor(theme_.tokens.disabledFg), alignment));
}

void TrigglowDelayDock::ApplyStaticIcons()
{
	const uitokens::Tokens &t = theme_.tokens;
	SetIcon(updateIcon_, ui::IconId::Download, QStringLiteral("warning"), kAccountIconSize);
	SetIcon(updateExternalIcon_, ui::IconId::ExternalLink, QStringLiteral("muted"), kInlineIconSize);
	SetIcon(delayLockedIcon_, ui::IconId::Lock, QStringLiteral("muted"), kInlineIconSize);
	SetIcon(sceneSwitchIcon_, ui::IconId::Info, QStringLiteral("muted"), kInlineIconSize);
	SetIcon(healthIcon_, ui::IconId::Sliders, QStringLiteral("warning"), kInlineIconSize);
	SetIcon(applyLockIcon_, ui::IconId::Lock, QStringLiteral("muted"), kInlineIconSize);
	SetIcon(ruleIcon_, ui::IconId::Info, QStringLiteral("accent"), kInlineIconSize);
	SetIcon(noticeIcon_, ui::IconId::AlertTriangle, QStringLiteral("danger"), kInlineIconSize);
	for (const auto &[field, lock] : fieldLocks_)
		lock->SetPixmap(ui::IconPixmap(ui::IconId::Lock, ToneColor(QStringLiteral("muted")), kFieldLockSize,
					       lock->devicePixelRatioF()));
	SetButtonIcon(enableButton_, ui::IconId::Shield, ui::ToQColor(t.onGradient));
	SetButtonIcon(disableButton_, ui::IconId::X, ui::ToQColor(t.dangerButtonText));
	SetButtonIcon(dumpButton_, ui::IconId::Scissors, ui::ToQColor(t.text));
	SetButtonIcon(cutButton_, ui::IconId::X, ui::ToQColor(t.text));
	SetButtonIcon(addRule_, ui::IconId::Plus, ui::ToQColor(t.text));
	SetButtonIcon(addSceneButton_, ui::IconId::Plus, ui::ToQColor(t.text));
	for (const ExtraSceneRow &row : extraSceneRows_)
		SetButtonIcon(row.remove, ui::IconId::Trash, ui::ToQColor(t.textMuted), Qt::AlignCenter);
	for (const std::unique_ptr<Section> &section : sections_)
		SetSectionOpen(*section, section->open);
	for (const RuleRow &row : ruleRows_)
		SetButtonIcon(row.remove, ui::IconId::Trash, ui::ToQColor(t.textMuted), Qt::AlignCenter);
	for (auto &[icon, label] : warningRows_)
		SetIcon(icon, ui::IconId::AlertTriangle, QStringLiteral("warning"), kInlineIconSize);
	// Right to left: the icon follows the text, so the gap goes on its left.
	SetButtonIcon(signInButton_, ui::IconId::ExternalLink, ui::ToQColor(t.onGradient), Qt::AlignRight);
	SetButtonIcon(reportBugButton_, ui::IconId::Flag, ui::ToQColor(t.accentText));
}

void TrigglowDelayDock::RefreshFitEstimate()
{
	auto estimate =
		bufferController_.EstimateBufferFit(static_cast<uint32_t>(secondsSpin_->value()),
						    static_cast<uint32_t>(minResolutionCombo_->currentData().toInt()));

	// Options is folded by default: it opens once when the estimate starts
	// saying the seconds don't fit, so that warning is never hidden.
	const bool warns = estimate.width > 0 && estimate.height > 0 && !estimate.fitsFullDuration;
	if (warns && !fitWarned_ && optionsSection_ && !optionsSection_->open)
		SetSectionOpen(*optionsSection_, true);
	fitWarned_ = warns;

	if (estimate.width == 0 || estimate.height == 0) {
		// No live scene chosen yet, or it doesn't resolve -- nothing
		// concrete to estimate against.
		SetIcon(fitIcon_, ui::IconId::Info, QStringLiteral("muted"), kInlineIconSize);
		fitLabel_->setTextFormat(Qt::PlainText);
		fitLabel_->setText(T("Dock.Fit.ChooseScene"));
		ui::SetStyleProperty(fitLabel_, "tgTone", QStringLiteral("muted"));
		return;
	}

	const QString width = QString::number(estimate.width);
	const QString height = QString::number(estimate.height);
	if (estimate.fitsFullDuration) {
		// "RAM fits the full <b>30 s</b> at 1920x1080.": the figure in the
		// body color, 600.
		const QString figure =
			QStringLiteral("<span style=\"color:%1; font-weight:600;\">%2</span>")
				.arg(ui::ToQColor(theme_.tokens.text).name(),
				     T("Dock.Status.Seconds").arg(secondsSpin_->value()).toHtmlEscaped());
		SetIcon(fitIcon_, ui::IconId::Check, QStringLiteral("accent"), kInlineIconSize);
		fitLabel_->setTextFormat(Qt::RichText);
		fitLabel_->setText(T("Dock.Fit.FullyFits").toHtmlEscaped().arg(figure, width, height));
		ui::SetStyleProperty(fitLabel_, "tgTone", QStringLiteral("muted"));
	} else {
		// The whole seconds the delay really uses (bufferfit's effective
		// delay), so the line agrees with the status chip's "41 / 60 s"; the
		// ring's fractional span only when not even one second fits. While the
		// delay runs both fields are locked: no advice to change them then.
		const QLocale locale = UiLocale();
		const QString fitted = estimate.effectiveSeconds > 0 ? locale.toString(estimate.effectiveSeconds)
								     : locale.toString(estimate.actualSeconds, 'f', 1);
		const BufferModeState state = bufferController_.GetStatus().state;
		const bool locked = state == BufferModeState::Filling || state == BufferModeState::Active;
		// Advice to lower the seconds still applies while it runs (v1.0.0:
		// they change live); only the quality is locked then.
		SetIcon(fitIcon_, ui::IconId::AlertTriangle, QStringLiteral("warning"), kInlineIconSize);
		fitLabel_->setTextFormat(Qt::PlainText);
		fitLabel_->setText(T(locked ? "Dock.Fit.TrimmedLocked" : "Dock.Fit.Trimmed")
					   .arg(fitted)
					   .arg(secondsSpin_->value())
					   .arg(width, height));
		ui::SetStyleProperty(fitLabel_, "tgTone", QStringLiteral("warning"));
	}
}

void TrigglowDelayDock::OnStatusChanged(const BufferModeStatus &status)
{
	if (detached_)
		return;
	Refresh();

	if (status.state != BufferModeState::Filling) {
		DisarmFillTimer();
		return;
	}
	// Once per fill window (Enable(), a refill): every other update while
	// Filling -- the overlay switched on or off, another corner -- leaves the
	// countdown running instead of starting it over.
	// The window lasts the seconds that really apply: the ring only has to
	// hold the effective delay when the RAM budget trimmed it.
	if (!fillArmed_ || status.fillGeneration != armedFillGeneration_) {
		armedFillGeneration_ = status.fillGeneration;
		ArmFillTimer(status.fillWindowMs > 0 ? status.fillWindowMs : AppliedDelaySeconds(status) * 1000U);
	}
}

void TrigglowDelayDock::Refresh()
{
	if (detached_ || !themeApplied_)
		return;
	const BufferModeStatus scene = bufferController_.GetStatus();
	const StreamDelayStatus stream = streamController_.GetStatus();
	const bool streamOnly = StreamOnly();

	refreshing_ = true;
	ApplyModeVisibility(streamOnly);
	if (streamOnly)
		RefreshStreamStatus(stream);
	else
		RefreshSceneStatus(scene);
	RefreshSceneFields(scene);
	RefreshDelayCard(streamOnly ? stream.delaySeconds : scene.delaySeconds, streamOnly);
	for (int i = 0; i < 3; ++i) {
		const int preset = static_cast<int>(streamController_.Preset(i));
		if (presetSpins_[i]->value() != preset)
			presetSpins_[i]->setValue(preset);
	}

	// The badge: one control for both modes, showing the selected mode's
	// values (the dock writes both, plugin-main.cpp aligns them at load).
	const bool showOverlay = streamOnly ? stream.showOverlay : scene.showOverlay;
	const uint32_t corner = streamOnly ? stream.overlayCorner : scene.overlayCorner;
	overlayCheck_->setChecked(showOverlay);
	const int cornerIndex = overlayCornerCombo_->findData(static_cast<int>(corner));
	if (cornerIndex >= 0)
		overlayCornerCombo_->setCurrentIndex(cornerIndex);
	overlayCornerCombo_->setEnabled(showOverlay);
	overlaySecondsCheck_->setChecked(streamController_.OverlayShowSeconds());
	overlaySecondsCheck_->setEnabled(showOverlay);
	const QString overlayTip = T(streamOnly ? "Dock.Overlay.Tooltip.Stream" : "Dock.Overlay.Tooltip");
	overlayCheck_->setToolTip(overlayTip);
	overlayCheck_->setAccessibleDescription(overlayTip);
	overlayText_->setToolTip(overlayTip);

	if (streamOnly) {
		drainCheck_->setChecked(stream.drainOnStop);
		RefreshBuffer(stream);
		RefreshDestination(stream);
		RefreshWarnings(stream);
	}
	RefreshApplyTo(scene, stream);
	refreshing_ = false;

	// Scene mode's storage codec duties and degradation notice: polled only
	// while there is a buffer to ask about (in either mode's view).
	const bool sceneBusy = scene.state == BufferModeState::Filling || scene.state == BufferModeState::Active;
	if (sceneBusy && !healthTimer_->isActive())
		healthTimer_->start();
	else if (!sceneBusy)
		healthTimer_->stop();
	RefreshPipelineHealth(scene); // Also sets the card's accessible name.
	if (!streamOnly)
		RefreshFitEstimate();
}

void TrigglowDelayDock::ApplyModeVisibility(bool streamOnly)
{
	// All of OBS: the scene pickers, the minimum quality and the RAM
	// estimate. Only the stream: where it sends, Dump, the settings checks,
	// Delay by scene, the delay buffer and sending the rest.
	scenesCard_->setVisible(!streamOnly);
	qualityBlock_->setVisible(!streamOnly);
	bufferBlock_->setVisible(streamOnly);
	drainRow_->setVisible(streamOnly);
	rulesSection_->box->setVisible(streamOnly);
	rulesDivider_->setVisible(streamOnly);
	sendsRow_->setVisible(streamOnly);
	if (!streamOnly) {
		for (QWidget *widget : std::initializer_list<QWidget *>{statsLabel_, ruleRow_, noticeRow_,
									warningsCard_, dumpButton_, cutButton_})
			widget->setVisible(false);
	} else {
		fillRow_->setVisible(false);
		hintLabel_->setVisible(false);
	}
}

void TrigglowDelayDock::RefreshSceneStatus(const BufferModeStatus &status)
{
	// Each state differs by rule, icon, headline color AND text -- never by
	// color alone.
	QString headline;
	QString detail = QString::fromStdString(status.message);
	QString rule = QStringLiteral("none");
	QString tone = QStringLiteral("text");
	QString chipTone;
	// What viewers really get -- the same seconds the live overlay shows, from
	// Filling on. The RAM budget can trim the configured delay (60 s asked,
	// 35 s fit): then the chip reads "35 / 60 s" in amber and the detail says
	// why (Filling's comes from the controller, Status.FillingTrimmed).
	const uint32_t applied = AppliedDelaySeconds(status);
	const bool trimmed = applied < status.delaySeconds;
	QString chipText = trimmed ? T("Dock.Status.SecondsOf").arg(applied).arg(status.delaySeconds)
				   : T("Dock.Status.Seconds").arg(applied);
	ui::IconId icon = ui::IconId::Circle;
	QString iconTone = QStringLiteral("muted");
	QString stateName;
	switch (status.state) {
	case BufferModeState::Inactive:
		stateName = QStringLiteral("scene.inactive");
		headline = T("Dock.Status.Inactive");
		if (detail.isEmpty())
			detail = T("Dock.Status.Inactive.Detail");
		break;
	case BufferModeState::Filling:
		stateName = QStringLiteral("scene.filling");
		headline = T("Dock.Status.Filling");
		rule = QStringLiteral("warning");
		tone = QStringLiteral("warning");
		chipTone = QStringLiteral("warning");
		icon = ui::IconId::Clock;
		iconTone = QStringLiteral("warning");
		break;
	case BufferModeState::Active:
		stateName = QStringLiteral("scene.active");
		headline = T("Dock.Status.Active");
		detail = trimmed ? T("Dock.Status.Active.DetailTrimmed").arg(applied).arg(status.delaySeconds)
				 : T("Dock.Status.Active.Detail").arg(applied);
		rule = QStringLiteral("active");
		chipTone = trimmed ? QStringLiteral("warning") : QStringLiteral("accent");
		icon = ui::IconId::Shield;
		iconTone = QStringLiteral("accent");
		break;
	case BufferModeState::Error:
		stateName = QStringLiteral("scene.error");
		headline = T("Dock.Status.Error");
		rule = QStringLiteral("danger");
		tone = QStringLiteral("danger");
		icon = ui::IconId::AlertTriangle;
		iconTone = QStringLiteral("danger");
		break;
	}
	stateLabel_->setText(headline);
	ui::SetStyleProperty(stateLabel_, "tgTone", tone);
	ui::SetStyleProperty(statusRule_, "tgRule", rule);
	SetIcon(statusIcon_, icon, iconTone, kStatusIconSize);
	statusChip_->setVisible(!chipTone.isEmpty());
	if (!chipTone.isEmpty()) {
		statusChip_->setText(chipText);
		ui::SetStyleProperty(statusChip_, "tgTone", chipTone);
	}
	detailLabel_->setText(detail);
	detailLabel_->setVisible(!detail.isEmpty());
	fillRow_->setVisible(status.state == BufferModeState::Filling);
	statusAccessibleName_ = QStringLiteral("%1: %2. %3").arg(T("Dock.Status.Eyebrow"), headline, detail);
	AnnounceStatus(stateName, status.state == BufferModeState::Error,
		       QStringLiteral("%1. %2").arg(headline, detail));

	const bool busy = status.state == BufferModeState::Filling || status.state == BufferModeState::Active;
	// Also requires a live scene chosen -- previously Enable was clickable
	// with no scene selected, which just bounced back into an Error state
	// ("Elige primero una escena en directo") instead of preventing the
	// click in the first place (2026-08-26 UX pass). Same reasoning for the
	// free-account gate: BufferModeController::Enable() already refuses and
	// sets Error if authManager_ says logged out, but disabling the button
	// up front is a clearer signal than a click that visibly bounces. The
	// requirement list right below says which of the two is missing.
	// A suspended or inactive account blocks Enable too (the account row says why).
	const bool loggedIn = authManager_.CanEnable();
	// The saved live scene counts once it's in the picker (a deleted scene or
	// the helper scene isn't); until OBS has loaded the collection the picker
	// is still empty and the saved name is trusted.
	const QString liveName = QString::fromStdString(status.liveSceneName);
	const int liveIndex = liveName.isEmpty() ? -1 : liveSceneCombo_->findText(liveName);
	const bool hasScene = liveIndex >= 0 || (!liveName.isEmpty() && liveSceneCombo_->count() == 0);
	enableButton_->setEnabled(!busy && hasScene && loggedIn);
	disableButton_->setEnabled(busy);
	// One button at a time, full width: on while off, off from the moment it
	// starts filling until it is switched off again.
	enableButton_->setVisible(!busy);
	disableButton_->setVisible(busy);

	requirements_->setVisible(!busy && !(hasScene && loggedIn));
	auto requirement = [this](QLabel *icon, QLabel *text, bool done, const char *key) {
		SetIcon(icon, done ? ui::IconId::Check : ui::IconId::Circle,
			done ? QStringLiteral("accent") : QStringLiteral("muted"), kInlineIconSize);
		ui::SetStyleProperty(text, "tgTone", done ? QStringLiteral("text") : QStringLiteral("muted"));
		text->setAccessibleName(T(done ? "Dock.Requirement.Done" : "Dock.Requirement.Pending").arg(T(key)));
	};
	requirement(reqAccountIcon_, reqAccountText_, loggedIn, "Dock.Requirement.Account");
	requirement(reqSceneIcon_, reqSceneText_, hasScene, "Dock.Requirement.Scene");
	reqSceneIcon_->setVisible(true);
	reqSceneText_->setVisible(true);
	hintLabel_->setVisible(!busy);
}

void TrigglowDelayDock::RefreshSceneFields(const BufferModeStatus &status)
{
	// v1.0.0: the delayed scenes' rows first, so the locks below cover them.
	RefreshDelayedScenes(status);
	// Scenes and quality are locked while the delay runs (a disabled control
	// is also out of the tab order): no arrows, the card's background and a
	// lock inside each (ui-theme.cpp, tgLocked), and the line under the
	// pickers says how to unlock them -- also what screen readers hear.
	const bool busy = status.state == BufferModeState::Filling || status.state == BufferModeState::Active;
	liveSceneCombo_->setEnabled(!busy);
	loadingSceneCombo_->setEnabled(!busy);
	minResolutionCombo_->setEnabled(!busy);
	for (const auto &[field, lock] : fieldLocks_) {
		ui::SetStyleProperty(field, "tgLocked", busy ? QStringLiteral("true") : QStringLiteral("false"));
		lock->setVisible(busy);
		field->setAccessibleDescription(busy ? T("Dock.Delay.Locked") : QString());
	}
	delayLockedRow_->setVisible(busy);
	sceneSwitchRow_->setVisible(busy);

	const QSignalBlocker blockQuality(minResolutionCombo_);
	const int qualityIndex = minResolutionCombo_->findData(static_cast<int>(status.minResolutionHeight));
	if (qualityIndex >= 0)
		minResolutionCombo_->setCurrentIndex(qualityIndex);

	const QString liveName = QString::fromStdString(status.liveSceneName);
	const int liveIndex = liveName.isEmpty() ? -1 : liveSceneCombo_->findText(liveName);
	const QSignalBlocker blockLive(liveSceneCombo_);
	liveSceneCombo_->setCurrentIndex(liveIndex); // -1: the placeholder, no scene shown as chosen.
	// Placeholder color (see the tgEmpty rule in ui-theme.cpp).
	ui::SetStyleProperty(liveSceneCombo_, "tgEmpty",
			     liveIndex < 0 ? QStringLiteral("true") : QStringLiteral("false"));

	const QSignalBlocker blockLoading(loadingSceneCombo_);
	const int loadingIndex =
		status.loadingSceneName.empty()
			? 0
			: loadingSceneCombo_->findText(QString::fromStdString(status.loadingSceneName));
	loadingSceneCombo_->setCurrentIndex(loadingIndex >= 0 ? loadingIndex : 0);

	QString summary = liveIndex >= 0 ? liveName : QString();
	if (!summary.isEmpty() && status.liveSceneNames.size() > 1)
		summary += QStringLiteral(" +%1").arg(status.liveSceneNames.size() - 1);
	SetSectionSummary(*scenesSection_, summary);
	// The live scene is required: folded without one, the section opens so
	// the picker is in sight.
	if (liveIndex < 0 && !busy && !scenesSection_->open)
		SetSectionOpen(*scenesSection_, true);
}

void TrigglowDelayDock::RefreshDelayCard(uint32_t delaySeconds, bool streamOnly)
{
	if (secondsSpin_->value() != static_cast<int>(delaySeconds))
		secondsSpin_->setValue(static_cast<int>(delaySeconds));
	SetSectionSummary(*delaySection_, T("Dock.Status.Seconds").arg(delaySeconds));
	const QString tip = streamOnly ? T("Dock.Delay.Tooltip.Stream").arg(BufferModeController::kMaxDelaySeconds)
				       : T("Dock.Delay.Tooltip");
	secondsSpin_->setToolTip(tip);
	secondsSpin_->setAccessibleDescription(tip);
	lessButton_->setEnabled(delaySeconds > BufferModeController::kMinDelaySeconds);
	moreButton_->setEnabled(delaySeconds < BufferModeController::kMaxDelaySeconds);
	for (int i = 0; i < 3; ++i) {
		const uint32_t value = streamController_.Preset(i);
		presetChips_[i]->setText(T("Dock.Status.Seconds").arg(value));
		presetChips_[i]->setAccessibleName(T("Dock.Preset.Label").arg(i + 1) + QStringLiteral(": ") +
						   T("Dock.Status.Seconds").arg(value));
		ui::SetStyleProperty(presetChips_[i], "tgActive",
				     value == delaySeconds ? QStringLiteral("true") : QStringLiteral("false"));
	}
}

void TrigglowDelayDock::RefreshApplyTo(const BufferModeStatus &scene, const StreamDelayStatus &stream)
{
	// Never switched under a running delay: the other mode would start from
	// nothing while this one stopped, and viewers would see a jump. Locked
	// like the scene pickers, with the line saying how to unlock it.
	const bool sceneBusy = scene.state == BufferModeState::Filling || scene.state == BufferModeState::Active;
	const bool streamBusy = stream.delayOn || stream.state == StreamDelayState::Draining;
	const bool locked = sceneBusy || streamBusy;
	const bool wasRefreshing = refreshing_;
	refreshing_ = true;
	applyCheck_->setChecked(stream.available && stream.applyTo == ApplyTo::Stream);
	refreshing_ = wasRefreshing;
	applyCheck_->setEnabled(stream.available && !locked);
	applyText_->setEnabled(stream.available && !locked);
	applyLockRow_->setVisible(stream.available && locked);
	applyHint_->setText(T(stream.available ? "Dock.ApplyTo.Hint" : "Dock.ApplyTo.Unavailable"));
}

void TrigglowDelayDock::AnnounceStatus(const QString &state, bool error, const QString &message)
{
	// Screen readers learn about the change without the user having to go
	// looking for it, errors first. Only on real state changes, not every
	// refresh. No NameChanged of our own: setAccessibleName() already sends
	// one when the card's name changes (Qt 6), and a second one made screen
	// readers read the change twice.
	if (hasAnnouncedState_ && announcedState_ == state)
		return;
	const bool first = !hasAnnouncedState_;
	hasAnnouncedState_ = true;
	announcedState_ = state;
	if (first || !QAccessible::isActive())
		return;
#if QT_VERSION >= QT_VERSION_CHECK(6, 8, 0)
	Announce(message, error);
#else
	// No announcement event before Qt 6.8: the card's NameChanged is what
	// screen readers get, plus an alert for errors.
	(void)message;
	if (error) {
		QAccessibleEvent event(statusCard_, QAccessible::Alert);
		QAccessible::updateAccessibility(&event);
	}
#endif
}

void TrigglowDelayDock::Announce(const QString &message, bool assertive)
{
	if (!QAccessible::isActive() || message.isEmpty())
		return;
#if QT_VERSION >= QT_VERSION_CHECK(6, 8, 0)
	QAccessibleAnnouncementEvent event(statusCard_, message);
	event.setPoliteness(assertive ? QAccessible::AnnouncementPoliteness::Assertive
				      : QAccessible::AnnouncementPoliteness::Polite);
	QAccessible::updateAccessibility(&event);
#else
	(void)assertive;
#endif
}

void TrigglowDelayDock::RefreshPipelineHealth(const BufferModeStatus &status)
{
	const bool busy = status.state == BufferModeState::Filling || status.state == BufferModeState::Active;
	PipelineHealth health;
	if (busy)
		health = bufferController_.GetPipelineHealth();
	const bool show = busy && health.running && health.Degraded();

	if (show) {
		// What was lowered (resolution, capture rate or both), why, and that
		// the delay is untouched: the rungs never shorten it.
		const QLocale locale = UiLocale();
		const QString cause = T(HealthCauseKey(health.cause));
		const QString seconds = QString::number(AppliedDelaySeconds(status));
		const bool smaller = health.fullWidth > 0 && health.fullHeight > 0 &&
				     (health.width < health.fullWidth || health.height < health.fullHeight);
		const bool slower = health.captureFps > 0.0 && health.captureFps < health.outputFps - 0.01;
		const QString width = QString::number(health.width);
		const QString height = QString::number(health.height);
		const QString fullWidth = QString::number(health.fullWidth);
		const QString fullHeight = QString::number(health.fullHeight);
		const QString fps = FormatFps(locale, health.captureFps);
		const QString fullFps = FormatFps(locale, health.outputFps);
		QString text;
		if (smaller && slower)
			text = T("Dock.Health.Both")
				       .arg(width, height, fps, fullWidth, fullHeight, fullFps, cause, seconds);
		else if (smaller)
			text = T("Dock.Health.Resolution").arg(width, height, fullWidth, fullHeight, cause, seconds);
		else if (slower)
			text = T("Dock.Health.FrameRate").arg(fps, fullFps, cause, seconds);
		else
			text = T("Dock.Health.Other").arg(cause, seconds);
		healthBody_->setText(text);

		// The fine print, only with a rate above 0 (one decimal shows 0.05).
		const bool repeated = health.repeatedPerMinute >= 0.05;
		const bool dropped = health.droppedPerMinute >= 0.05;
		const QString repeatedRate = FormatRate(locale, health.repeatedPerMinute);
		const QString droppedRate = FormatRate(locale, health.droppedPerMinute);
		if (repeated && dropped)
			healthStats_->setText(T("Dock.Health.RepeatedDropped").arg(repeatedRate, droppedRate));
		else if (repeated)
			healthStats_->setText(T("Dock.Health.Repeated").arg(repeatedRate));
		else if (dropped)
			healthStats_->setText(T("Dock.Health.Dropped").arg(droppedRate));
		healthStats_->setVisible(repeated || dropped);
	}

	// Screen readers hear the notice appear, go away (with the delay still
	// on) or change cause -- not every rung change, nor the once-a-second
	// poll: the card's accessible name only changes with the first two.
	const PipelineLoad cause = show ? health.cause : PipelineLoad::None;
	const bool appeared = show && !healthShown_;
	const bool gone = !show && healthShown_;
	const bool causeChanged = show && healthShown_ && cause != healthCause_;
	healthCause_ = cause;
	if (show != healthShown_) {
		healthShown_ = show;
		healthNotice_->setVisible(show);
		// The notice says what the buffer holds now; the RAM estimate in the
		// settings card (full size) would contradict it.
		fitRow_->setVisible(!show);
	}
	UpdateStatusAccessibleName();
	if (appeared || causeChanged)
		Announce(QStringLiteral("%1. %2").arg(healthTitle_->text(), healthBody_->text()), false);
	else if (gone && busy)
		Announce(T("Dock.Health.Restored"), false);
}

void TrigglowDelayDock::UpdateStatusAccessibleName()
{
	// The name changes only with the state and when the notice appears or
	// goes (setAccessibleName() sends NameChanged even for the same text, so
	// it's only called on a real change); what was lowered, which changes
	// with each rung, goes in the description.
	QString name = statusAccessibleName_;
	if (healthShown_)
		name += QStringLiteral(" %1.").arg(healthTitle_->text());
	if (statusCard_->accessibleName() != name)
		statusCard_->setAccessibleName(name);
	const QString description = healthShown_ ? healthBody_->text() : QString();
	if (statusCard_->accessibleDescription() != description)
		statusCard_->setAccessibleDescription(description);
}

void TrigglowDelayDock::resizeEvent(QResizeEvent *event)
{
	QWidget::resizeEvent(event);
	ApplyNarrowLayout(event->size().width());
}

void TrigglowDelayDock::ApplyNarrowLayout(int width)
{
	// Side-by-side rows turn into stacked ones when there's not enough room
	// for both columns: the buttons below 250 px; the footer, the account
	// row's inline button (under its text, next to the icon), the presets
	// and the quick-pick label below 300 px.
	const bool tiny = width < kTinyWidth;
	const bool narrow = width < kNarrowWidth;
	const auto stacked = QBoxLayout::TopToBottom;
	const auto sideBySide = QBoxLayout::LeftToRight;
	if (accountLayout_)
		accountLayout_->setDirection(narrow ? stacked : sideBySide);
	if (presetRow_)
		presetRow_->setDirection(narrow ? stacked : sideBySide);
	if (chipRow_)
		chipRow_->setDirection(narrow ? stacked : sideBySide);
	if (buttonRow_)
		buttonRow_->setDirection(tiny ? stacked : sideBySide);
	if (footerRow_)
		footerRow_->setDirection(narrow ? stacked : sideBySide);
}

void TrigglowDelayDock::RefreshAccountUi()
{
	using LoginError = AuthManager::LoginError;
	const LoginError error = authManager_.LastError();
	ui::IconId icon = ui::IconId::Lock;
	QString iconTone = QStringLiteral("muted");
	QString text;
	QString textTone = QStringLiteral("text");
	QString inlineText;
	QString primaryText;
	bool showCopy = false;

	if (authManager_.IsLoggedIn() && authManager_.AccountInactive()) {
		linkCopied_ = false;
		icon = ui::IconId::AlertTriangle;
		iconTone = QStringLiteral("danger");
		textTone = QStringLiteral("danger");
		text = T("Dock.Account.Inactive").toHtmlEscaped();
		primaryText = T("Dock.Account.OpenTrigglow");
		inlineText = T("Dock.Account.SignOut");
	} else if (authManager_.IsLoggedIn()) {
		linkCopied_ = false;
		icon = ui::IconId::User;
		const QString name = QString::fromStdString(authManager_.DisplayName());
		// "Signed in as <b>VirosMs</b>": the name in 600, escaped (it's user data).
		text = name.isEmpty() ? T("Dock.Account.FreeAccountConnected").toHtmlEscaped()
				      : T("Dock.Account.SignedInAs")
						.toHtmlEscaped()
						.arg(QStringLiteral("<span style=\"font-weight:600;\">%1</span>")
							     .arg(name.toHtmlEscaped()));
		inlineText = T("Dock.Account.SignOut");
	} else if (authManager_.IsLoggingIn()) {
		inlineText = T("Dock.Account.Cancel");
		// The page must show the same code: that is how the user knows the
		// page they confirm on is this OBS's request.
		const QString code = QString::fromStdString(authManager_.PendingUserCode());
		showCopy = !authManager_.PendingUrl().empty();
		if (error == LoginError::Browser) {
			icon = ui::IconId::AlertTriangle;
			iconTone = QStringLiteral("danger");
			text = T("Dock.Account.Error.Browser").toHtmlEscaped();
		} else {
			icon = ui::IconId::Clock;
			iconTone = QStringLiteral("warning");
			text = T("Dock.Account.WaitingConfirmation").toHtmlEscaped();
		}
		if (!code.isEmpty())
			text += QStringLiteral("<br>") +
				T("Dock.Account.ConfirmCode")
					.toHtmlEscaped()
					.arg(QStringLiteral("<span style=\"font-weight:600;\">%1</span>")
						     .arg(code.toHtmlEscaped()));
	} else {
		linkCopied_ = false;
		primaryText = T("Dock.Account.SignIn");
		text = T("Dock.Account.RequireLogin").toHtmlEscaped();
		if (error != LoginError::None) {
			icon = ui::IconId::AlertTriangle;
			iconTone = QStringLiteral("danger");
			textTone = QStringLiteral("danger");
			if (error == LoginError::Network) {
				text = T("Dock.Account.Error.Network").toHtmlEscaped();
				primaryText = T("Dock.Account.Retry");
			} else if (error == LoginError::Expired) {
				text = T("Dock.Account.Error.Expired").toHtmlEscaped();
				primaryText = T("Dock.Account.Retry");
			} else if (error == LoginError::SessionEnded) {
				text = T("Dock.Account.SessionEnded").toHtmlEscaped();
			} else if (error == LoginError::Denied) {
				text = T("Dock.Account.Error.Denied").toHtmlEscaped();
			} else if (error == LoginError::Failed) {
				text = T("Dock.Account.Error.Failed").toHtmlEscaped();
				primaryText = T("Dock.Account.Retry");
			} else if (error == LoginError::RateLimited) {
				text = T("Dock.Account.Error.RateLimited").toHtmlEscaped();
			} else if (error == LoginError::Outdated) {
				text = T("Dock.Account.Error.Outdated").toHtmlEscaped();
			}
		}
	}

	SetIcon(accountIcon_, icon, iconTone, kAccountIconSize);
	accountLabel_->setText(text);
	ui::SetStyleProperty(accountLabel_, "tgTone", textTone);
	accountButton_->setVisible(!inlineText.isEmpty());
	accountButton_->setText(inlineText);
	accountButton_->setToolTip(authManager_.IsLoggingIn() ? T("Dock.Account.Waiting") : QString());
	signInButton_->setVisible(!primaryText.isEmpty());
	signInButton_->setText(primaryText);
	copyLinkButton_->setVisible(showCopy);
	copyLinkButton_->setText(linkCopied_ ? T("Dock.Account.LinkCopied") : T("Dock.Account.CopyLink"));
	// Copies to the clipboard, opens nothing: a copy icon, not external-link.
	SetButtonIcon(copyLinkButton_, linkCopied_ ? ui::IconId::Check : ui::IconId::Copy,
		      ui::ToQColor(theme_.tokens.text));

	// Re-derives the Enable button's visual enabled state too (real
	// enforcement lives in each controller's Enable() itself, see
	// SetAuthorizationCheck) -- single source of truth for that condition,
	// see RefreshSceneStatus()'s and RefreshStreamStatus()'s enableButton_ lines.
	Refresh();
}

void TrigglowDelayDock::ArmFillTimer(uint32_t milliseconds)
{
	if (!fillTimer_)
		return;
	fillTimer_->setInterval(static_cast<int>(std::min<uint32_t>(milliseconds, INT32_MAX)));
	fillTimer_->start();
	fillArmed_ = true;

	fillTotalMs_ = milliseconds;
	fillTotalSeconds_ = (milliseconds + 999) / 1000;
	fillElapsed_.start();
	if (fillProgressTimer_)
		fillProgressTimer_->start(250);
	UpdateFillProgress(); // Show the countdown immediately, not after the first 250ms tick.
}

void TrigglowDelayDock::DisarmFillTimer()
{
	fillArmed_ = false;
	if (fillTimer_)
		fillTimer_->stop();
	if (fillProgressTimer_)
		fillProgressTimer_->stop();
}

void TrigglowDelayDock::UpdateFillProgress()
{
	if (!fillBar_ || fillTotalMs_ == 0)
		return;
	// Only touches the bar and its two texts, never the tones -- RefreshFromStatus
	// already set the amber Filling look once and owns it; this just shows
	// how much of that same status has elapsed.
	const qint64 totalMs = static_cast<qint64>(fillTotalMs_);
	const qint64 elapsedMs = std::clamp<qint64>(fillElapsed_.elapsed(), 0, totalMs);
	fillBar_->setRange(0, static_cast<int>(totalMs));
	fillBar_->setValue(static_cast<int>(elapsedMs));
	const int elapsedSeconds = static_cast<int>(elapsedMs / 1000);
	const int remainingSeconds = static_cast<int>(std::ceil((totalMs - elapsedMs) / 1000.0));
	fillProgressLabel_->setText(T("Dock.Status.Filling.Progress").arg(elapsedSeconds).arg(fillTotalSeconds_));
	fillRemainingLabel_->setText(T("Dock.Status.Filling.Countdown").arg(remainingSeconds));
}

void TrigglowDelayDock::RefreshSceneCombo(SceneComboBox *combo, const std::string &currentValue, bool includeNoneOption)
{
	const QSignalBlocker block(combo);
	QString previousValue = QString::fromStdString(currentValue);
	combo->clear();
	// The loading picker's empty choice shows the Trigglow "adding delay" video.
	if (includeNoneOption)
		combo->addItem(T("Dock.Scene.Loading.Default"));
	for (const auto &name : bufferController_.ListAvailableScenes())
		combo->addItem(QString::fromStdString(name));

	// The live picker shows its placeholder (-1) unless the saved scene is in
	// the list; the loading picker falls back to "None" (0).
	int idx = previousValue.isEmpty() ? -1 : combo->findText(previousValue);
	if (idx < 0 && includeNoneOption)
		idx = 0;
	combo->setCurrentIndex(idx);
}

void TrigglowDelayDock::RefreshAvailableScenes()
{
	RefreshSceneCombo(liveSceneCombo_, bufferController_.GetStatus().liveSceneName, false);
	RefreshSceneCombo(loadingSceneCombo_, bufferController_.GetStatus().loadingSceneName, true);
	// The combos above are repopulated under a QSignalBlocker (see
	// RefreshSceneCombo), so selecting the live scene for the first time
	// here won't have gone through the normal currentIndexChanged path --
	// refresh explicitly: whether the saved scene is in the list decides the
	// requirement list and the Enable button, and the estimate (refreshed at
	// the end of Refresh()) mustn't keep saying "choose a scene" when one was
	// actually restored.
	Refresh();
}

void TrigglowDelayDock::OnReportBugClicked()
{
	TRIGGLOW_LOG_INFO(kComponent, "Report a problem pressed in dock");

	// ReportBugDialog does everything itself: Trigi (the AI support
	// assistant) first when it is available, then a real support ticket
	// (category "dynamic_delay", from the account when signed in) with OBS's
	// redacted log attached if the user leaves it ticked -- see its own
	// header comment. The diagnostics provider is for the support dialog's
	// Trigi page (opt-in technical data, support-diagnostics.hpp). exec()
	// (modal) is fine here: this is a deliberate, occasional action, not
	// something that needs to coexist with using the rest of the dock.
	ReportBugDialog dialog(authManager_, [this] { return CollectDiagnostics(bufferController_); }, this);
	dialog.exec();
}

void TrigglowDelayDock::CheckForUpdateAsync()
{
	// CheckForUpdate() is a BLOCKING network call (update-checker.hpp): one
	// of this dock's background jobs (ui-tasks.hpp), aborted when the dock is
	// destroyed or OBS closes; the result only ever reaches a live dock.
	QPointer<TrigglowDelayDock> self(this);
	const std::string currentVersion = PLUGIN_VERSION;
	uitasks::RunAsync<UpdateCheckResult>(
		updateTasks_, "update-checker",
		[currentVersion](const tasks::CancelToken &cancel) { return CheckForUpdate(currentVersion, &cancel); },
		[self](const UpdateCheckResult &result) {
			// Silent on any failure/inconclusive result AND when already
			// up to date -- see UpdateCheckResult::checked's comment: this
			// is a nice-to-have notice, never an error surfaced to the
			// user, and staying quiet when there's nothing to say is the
			// correct "no news" case, not a bug.
			if (!self || !result.checked || !result.updateAvailable)
				return;
			self->ShowUpdateNotice(result);
		});
}

void TrigglowDelayDock::ShowUpdateNotice(const UpdateCheckResult &result)
{
	const QString latest = QString::fromUtf8(result.latestVersion.c_str());
	updateVersion_->setText(T("Dock.Update.Version").arg(latest, QString::fromUtf8(PLUGIN_VERSION)));
	const QString action = T("Dock.Update.Action").arg(latest);
	updateNoticeButton_->setAccessibleName(action);
	updateNoticeButton_->setToolTip(action);
	updateReleaseUrl_ = QString::fromUtf8(result.releaseUrl.c_str());
	updateNoticeButton_->setVisible(true);
	FitUpdateBanner();
}

} // namespace trigglow
