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

#pragma once

#include <QElapsedTimer>
#include <QImage>
#include <QLocale>
#include <QPointer>
#include <QString>
#include <QWidget>

#include <functional>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "background-tasks.hpp"
#include "buffer-mode-controller.hpp"
#include "ui-theme.hpp"

class QLabel;
class QSpinBox;
class QBoxLayout;
class QVBoxLayout;
class QCheckBox;
class QComboBox;
class QFrame;
class QProgressBar;
class QPushButton;
class QTimer;
class QResizeEvent;

// TrigglowDelayDock is the plugin's dock: since v1.0.0 ONE panel for both
// delay engines. By default the delay applies to all of OBS (scene mode,
// BufferModeController): the loading scene shows while the buffer fills, then
// Program, the stream AND the recording are delayed. One advanced check box,
// "Apply the delay only to the stream" (apply-to.hpp), switches to stream
// mode (StreamDelayController): only the platform gets the delay, recording
// and preview stay live.
//
// Top to bottom: the update banner, the status card (one state for the
// selected mode), the account card, the one "Turn delay on" / "Turn delay off"
// button, the delay card (seconds, -5/+5, presets), the scenes card (all of
// OBS only), stream mode's "Settings to check", and the folded sections
// (Delay by scene, Hotkey presets, Options with the advanced check box), then
// the footer. The button goes through DelayRouter, like the hotkeys and the
// Stream Deck, so all of them drive the selected mode only.
//
// It is a thin view: every control calls the controllers (or the router), and
// the dock repaints from their status -- the same callbacks a hotkey press
// fires, so the dock and the hotkeys can never disagree. Stream mode's own
// parts are built and refreshed in settings-ui-stream.cpp.
//
// Look: ui-theme.hpp (tokens from OBS's palette, brand fonts, SVG icons), one
// style sheet set on the dock and recomputed when OBS switches theme. Widgets
// carry dynamic properties (tgRole, tgTone, tgVariant...) instead of their own
// setStyleSheet() calls, so colors live in one place.
//
// Ownership: once passed to ObsFrontendBridge::AddDock() ->
// obs_frontend_add_dock_by_id(), OBS's dock system owns this widget's
// lifetime. plugin-main.cpp must not delete it manually; it calls Detach()
// before the controllers go away.
namespace trigglow {

class SceneComboBox;
class AuthManager;
class DelayRouter;
class FieldIcon;
class StreamDelayController;
struct StreamDelayStatus;
struct UpdateCheckResult;

class TrigglowDelayDock : public QWidget {
	Q_OBJECT

public:
	TrigglowDelayDock(BufferModeController &bufferController, StreamDelayController &streamController,
			  DelayRouter &router, AuthManager &authManager, QWidget *parent = nullptr);

	// Settings changed through the dock that live in stream-mode-settings.json
	// (what the delay applies to, presets, scene rules...), to persist them.
	void SetSettingsChanged(std::function<void()> callback) { onSettingsChanged_ = std::move(callback); }
	// Stream mode's status changed (its controller's status callback).
	void OnStreamStatusChanged();
	// Stops every timer and makes every refresh a no-op: called on module
	// unload, before the controllers go away, when OBS hasn't destroyed the
	// dock yet.
	void Detach();

#ifdef TRIGGLOW_TEST_SKIP_ACCOUNT_GATE
	// Local test builds only (plugin-main.cpp's dock shots): the dock's
	// content rendered offscreen at `width` logical px on the dock's
	// background, with the folded sections open or not. Never shown, never
	// sends input anywhere.
	QImage RenderForTest(int width, bool sectionsOpen);
#endif

	// Recomputes tokens, fonts and icons from the application palette (OBS's
	// active theme). Wired to OBS_FRONTEND_EVENT_THEME_CHANGED and to palette
	// change events; a no-op when nothing changed.
	void ApplyTheme(bool force = false);

protected:
	void changeEvent(QEvent *event) override;
	// Wheel guard: the mouse wheel over a combo/spin box that doesn't have
	// focus scrolls the dock instead of silently changing the live scene,
	// the seconds or the quality.
	bool eventFilter(QObject *watched, QEvent *event) override;

private:
	// A collapsible section: a header button (chevron, title, count chip)
	// over a body that hides when folded; `box` holds both.
	struct Section {
		QWidget *box = nullptr;
		QPushButton *header = nullptr;
		QLabel *count = nullptr;
		QWidget *body = nullptr;
		bool open = false;
		// Shown in the header's chip while folded ("30 s", the live scene).
		QString summary;
	};
	// One scene rule (stream mode): scene, action, seconds, remove.
	struct RuleRow {
		QFrame *card = nullptr;
		QComboBox *scene = nullptr;
		QComboBox *action = nullptr;
		QSpinBox *seconds = nullptr;
		QPushButton *remove = nullptr;
	};

	void BuildUi();
	// Small builders: a field label above its control, the lock inside a
	// locked field, an icon + hint line (hidden until shown), and a check box
	// with a wrapping label.
	QLabel *MakeEyebrow(const QString &text, QWidget *buddy, QWidget *parent = nullptr);
	void AddFieldLock(QWidget *field);
	QWidget *MakeNoteRow(QWidget *parent, QLabel *&icon, const QString &text);
	QWidget *MakeCheckRow(QWidget *parent, QCheckBox *&check, QLabel *&text, const QString &label);
	QFrame *BuildDelayCard();
	QFrame *BuildScenesCard();
	void BuildOptions(QVBoxLayout *body);
	Section *AddSection(QVBoxLayout *parent, const QString &title, bool open);
	void SetSectionOpen(Section &section, bool open);
	void SetSectionSummary(Section &section, const QString &summary);
	void BuildPresets(QVBoxLayout *body);
	// settings-ui-stream.cpp: stream mode's parts.
	void BuildStreamStatusRows(QVBoxLayout *body);
	QFrame *BuildWarningsCard();
	void BuildRules(QVBoxLayout *body);
	QWidget *BuildBufferBlock(QWidget *host);
	void AddRuleRow(const std::string &scene, int action, int seconds);
	void RemoveRuleRow(QFrame *card);
	void RulesChanged();
	void RefreshStreamStatus(const StreamDelayStatus &status);
	void RefreshDestination(const StreamDelayStatus &status);
	void RefreshWarnings(const StreamDelayStatus &status);
	void RefreshBuffer(const StreamDelayStatus &status);
	// Rules for components only this dock has (section headers, preset chips,
	// the 5 s steps, icon buttons), from the same tokens.
	QString ExtraStyleSheet() const;

	// True while the controls go to stream mode ("only the stream" ticked
	// and stream mode can run here): what the dock shows.
	bool StreamOnly() const;
	// The advanced check box: switches the mode, carrying the seconds over so
	// the delay card keeps showing one value.
	void SetApplyToStream(bool streamOnly);
	// Shows the parts of the selected mode and hides the other's.
	void ApplyModeVisibility(bool streamOnly);
	void SettingsChanged();
	// The whole dock from both controllers' status.
	void Refresh();
	void RefreshSceneStatus(const BufferModeStatus &status);
	void RefreshSceneFields(const BufferModeStatus &status);
	void RefreshDelayCard(uint32_t delaySeconds, bool streamOnly);
	void RefreshApplyTo(const BufferModeStatus &scene, const StreamDelayStatus &stream);
	void GuardWheel(QWidget *field);

	void resizeEvent(QResizeEvent *event) override;
	// Tags `widget` with a QSS role (tgRole) and remembers its font role, so
	// ApplyTheme() can re-apply capitalization/letter spacing on theme change.
	void SetRole(QWidget *widget, const char *qssRole, ui::FontRole fontRole);
	void SetRole(QWidget *widget, const char *qssRole);
	QColor ToneColor(const QString &tone) const;
	void SetIcon(QLabel *label, ui::IconId icon, const QString &tone, int size);
	// The icon sits at `alignment` inside the button's (wider) icon size, so
	// the gap to the text is part of the icon box.
	void SetButtonIcon(QPushButton *button, ui::IconId icon, const QColor &color,
			   Qt::Alignment alignment = Qt::AlignLeft);
	// OBS themes give QPushButton a fixed height; the banner takes the
	// height of the layout inside it instead.
	void FitUpdateBanner();
	// Icons that don't depend on state (banner, footer, buttons).
	void ApplyStaticIcons();
	// On a real state change (`state` names it, per mode), announces
	// `message` (headline and detail) to screen readers -- assertively for
	// errors.
	void AnnounceStatus(const QString &state, bool error, const QString &message);
	// A QAccessibleAnnouncementEvent on the status card (Qt 6.8+; earlier Qt
	// has no such event and this does nothing), only while a screen reader
	// is active.
	void Announce(const QString &message, bool assertive);
	QLocale UiLocale() const;
	// Reflows rows that don't fit side by side at the current dock width.
	void ApplyNarrowLayout(int width);
	void OnStatusChanged(const BufferModeStatus &status);

	// Degradation notice (v0.6.0): asks BufferModeController::GetPipelineHealth()
	// (cheap, never blocks on the graphics thread) whether the buffer runs
	// below full quality, and shows, updates or hides the notice under the
	// status card's detail. Runs every second on healthTimer_ while the delay
	// is Filling or Active (the only states with a buffer) and on every
	// status change; anywhere else the notice is hidden and nothing is polled.
	void RefreshPipelineHealth(const BufferModeStatus &status);
	// The status card's accessible name -- state, detail and, while shown,
	// the degradation notice's title -- and its description (what the notice
	// says was lowered).
	void UpdateStatusAccessibleName();

	// Wired to authManager_'s own status-changed callback (same pattern as
	// bufferController_'s), so the account row and the Enable button's
	// visual disabled state update the moment login completes/expires --
	// without the user needing to touch any other control first. The real
	// enforcement is BufferModeController::Enable()'s own check (see
	// SetAuthorizationCheck in plugin-main.cpp); this is UX only.
	void RefreshAccountUi();

	// Single-shot fill countdown, armed by OnStatusChanged() once per fill
	// window -- when BufferModeStatus::fillGeneration changes while Filling --
	// for the seconds that window has to fill (same pattern DelayController's
	// own apply watchdog used).
	// v1.0.0: in ms (BufferModeStatus::fillWindowMs): a live raise fills only
	// its extra seconds, a change while Filling what's left of the window.
	void ArmFillTimer(uint32_t milliseconds);
	void DisarmFillTimer();

	// Live "18 s left" countdown + progress bar shown while Filling --
	// added 2026-08-26 after the MVP-complete pass: before this, Filling
	// just showed a static "Llenando buffer..." for the whole wait with no
	// indication of how much longer, which is exactly the kind of silent
	// wait a user has no way to distinguish from the plugin being stuck.
	// Ticks fillProgressTimer_ against fillElapsed_/fillTotalSeconds_;
	// doesn't touch anything RefreshSceneStatus already owns (color,
	// controls-enabled state) since no real status change is happening,
	// just the passage of time within the same Filling status.
	void UpdateFillProgress();

	// Shared by liveSceneCombo_ and loadingSceneCombo_ — repopulates
	// `combo` from bufferController_.ListAvailableScenes() and reselects
	// `currentValue` without re-triggering signals.
	void RefreshSceneCombo(SceneComboBox *combo, const std::string &currentValue, bool includeNoneOption);

	// Runs RefreshSceneCombo() for both scene combos, re-selecting whatever
	// BufferModeController::GetStatus() currently holds. Wired to
	// BufferModeController::SetSceneListRefreshCallback in the constructor
	// so this fires automatically once OBS finishes loading the scene
	// collection -- see that callback's comment for the bug this fixes
	// (both combos coming up blank on every fresh OBS start/reinstall).
	void RefreshAvailableScenes();

	// Live-updates fitLabel_ from bufferController_.EstimateBufferFit() for
	// whatever's currently in secondsSpin_/minResolutionCombo_ -- called
	// whenever either changes, so the user sees the tradeoff of their
	// choice immediately, before pressing Enable. "Aconsejar segun el
	// hardware, pero a su eleccion": never blocks anything, just warns. It's
	// the RAM estimate (rung 0 of the buffer): what the buffer runs at right
	// now under load is the degradation notice's job.
	void RefreshFitEstimate();

	// Wired to reportBugButton_: opens ReportBugDialog (report-bug-dialog.hpp),
	// which collects a short description and submits a real support ticket
	// to trigglow.com with OBS's redacted log attached when ticked -- see
	// that dialog's own header comment. Modal, but never blocks Enable/
	// Disable or anything else outside of itself while open.
	void OnReportBugClicked();

	// Kicks off update-checker.hpp's CheckForUpdate() on a background
	// thread (same RunHttp-style pattern ReportBugDialog uses for its own
	// HTTP calls), once, from the constructor. A public GitHub API GET, no
	// new trigglow.com backend needed -- see that header's comment for why
	// this is the plugin's only update-awareness at all right now.
	void CheckForUpdateAsync();
	// Marshaled back onto the UI thread by CheckForUpdateAsync(); only ever
	// called when a real update is available (a failed/inconclusive check
	// just stays silent, never shown to the user -- see
	// UpdateCheckResult::checked's comment).
	void ShowUpdateNotice(const UpdateCheckResult &result);

	BufferModeController &bufferController_;
	StreamDelayController &streamController_;
	DelayRouter &router_;
	AuthManager &authManager_;
	std::function<void()> onSettingsChanged_;
	bool detached_ = false;
	// Set while Refresh() writes values into the controls, so their change
	// signals don't write them back.
	bool refreshing_ = false;

	ui::Theme theme_;
	bool themeApplied_ = false;
	bool applyingTheme_ = false;
	std::vector<std::pair<QPointer<QWidget>, ui::FontRole>> fontRoles_;
	bool hasAnnouncedState_ = false;
	QString announcedState_;

	// Status card: eyebrow + seconds chip, icon + headline, detail, and while
	// Filling a 4 px progress bar with "12 / 30 s" and "18 s left".
	QFrame *statusCard_ = nullptr;
	QFrame *statusRule_ = nullptr; // 2 px top rule: amber Filling, gradient Active, red Error.
	QLabel *statusChip_ = nullptr;
	QLabel *statusIcon_ = nullptr;
	QLabel *stateLabel_ = nullptr; // Headline.
	QLabel *detailLabel_ = nullptr;
	QWidget *fillRow_ = nullptr;
	QProgressBar *fillBar_ = nullptr;
	QLabel *fillProgressLabel_ = nullptr;
	QLabel *fillRemainingLabel_ = nullptr;
	// Stream mode only: where it sends (the streamer's OBS service, with the
	// relay's state while it runs), the relay's stats, the scene rule applied
	// last and the last problem.
	QWidget *sendsRow_ = nullptr;
	QLabel *sendsLabel_ = nullptr;
	QLabel *sendsState_ = nullptr;
	QLabel *statsLabel_ = nullptr;
	QWidget *ruleRow_ = nullptr;
	QLabel *ruleIcon_ = nullptr;
	QLabel *ruleLabel_ = nullptr;
	QWidget *noticeRow_ = nullptr;
	QLabel *noticeIcon_ = nullptr;
	QLabel *noticeLabel_ = nullptr;
	// "Status: Delay on. Viewers see..." without the degradation notice.
	QString statusAccessibleName_;
	// Degradation notice (see RefreshPipelineHealth()): a hairline, the
	// sliders icon and title in the warning tone, what was lowered and why
	// (the delay itself never changes), and the repeat/drop rates when > 0.
	QWidget *healthNotice_ = nullptr;
	QLabel *healthIcon_ = nullptr;
	QLabel *healthTitle_ = nullptr;
	QLabel *healthBody_ = nullptr;
	QLabel *healthStats_ = nullptr;
	QTimer *healthTimer_ = nullptr;
	bool healthShown_ = false;
	// The cause the notice gave last (None while hidden): a new cause is
	// announced, a new rung for the same cause isn't.
	PipelineLoad healthCause_ = PipelineLoad::None;
	// All of OBS only: the live and loading scene pickers.
	QFrame *scenesCard_ = nullptr;
	SceneComboBox *liveSceneCombo_ = nullptr;
	SceneComboBox *loadingSceneCombo_ = nullptr;
	// Delay card: the seconds of the selected mode (1-600, live), 5 s steps
	// and the three presets as quick picks.
	QSpinBox *secondsSpin_ = nullptr;
	QPushButton *lessButton_ = nullptr;
	QPushButton *moreButton_ = nullptr;
	QBoxLayout *chipRow_ = nullptr;
	QPushButton *presetChips_[3] = {nullptr, nullptr, nullptr};
	// "Turn the delay off to change the scenes or the minimum quality.": shown
	// under the scene pickers while those are locked (Filling, Active).
	// v1.0.0: the seconds stay editable, the delay changes live.
	QWidget *delayLockedRow_ = nullptr;
	QLabel *delayLockedIcon_ = nullptr;
	// v1.0.0: while the delay runs, that switching scenes needs stream mode
	// (scene mode delays the live scene only).
	QWidget *sceneSwitchRow_ = nullptr;
	QLabel *sceneSwitchIcon_ = nullptr;
	// (field, lock): the lock drawn inside the live/loading scene pickers and
	// the quality while they're locked (Filling, Active), where their arrows
	// were.
	std::vector<std::pair<QWidget *, FieldIcon *>> fieldLocks_;
	// "Calidad minima" -- floor on the delayed video's resolution; see
	// BufferModeStatus::minResolutionHeight / VideoDelayFilter::EnsureRingSized.
	// In Options with the RAM estimate, all of OBS only (qualityBlock_).
	QWidget *qualityBlock_ = nullptr;
	QComboBox *minResolutionCombo_ = nullptr;
	// The delay badge ("Delay 30 s" on what viewers see while the delay is
	// on): one check box and one corner for both modes, written to both
	// controllers.
	QCheckBox *overlayCheck_ = nullptr;
	// The check box's text, as a word-wrapping label (a QCheckBox can't wrap,
	// which pushed narrow docks wider than their window). Clicking it toggles.
	QLabel *overlayText_ = nullptr;
	QComboBox *overlayCornerCombo_ = nullptr; // Which canvas corner the overlay sits in.
	// "Show the seconds on the badge": off leaves only the mark and the name.
	QCheckBox *overlaySecondsCheck_ = nullptr;
	QLabel *overlaySecondsText_ = nullptr;
	// Rows that reflow to a column when the dock is narrow -- see ApplyNarrowLayout().
	QBoxLayout *accountLayout_ = nullptr; // Text | inline button; stacked when narrow.
	QBoxLayout *buttonRow_ = nullptr;
	QBoxLayout *footerRow_ = nullptr;
	// Shows what secondsSpin_/minResolutionCombo_'s current combo would
	// actually achieve in RAM -- see RefreshFitEstimate(). fitRow_ holds both;
	// hidden while the degradation notice shows.
	QWidget *fitRow_ = nullptr;
	QLabel *fitIcon_ = nullptr;
	QLabel *fitLabel_ = nullptr;
	QLabel *budgetValue_ = nullptr;
	QPushButton *enableButton_ = nullptr;
	QPushButton *disableButton_ = nullptr;
	// Under a disabled Enable button: which of the two prerequisites is missing.
	QWidget *requirements_ = nullptr;
	QLabel *reqAccountIcon_ = nullptr;
	QLabel *reqAccountText_ = nullptr;
	QLabel *reqSceneIcon_ = nullptr;
	QLabel *reqSceneText_ = nullptr;
	QLabel *hintLabel_ = nullptr;
	// Stream mode: drop what viewers haven't seen yet, and stop sending the
	// last seconds after OBS stopped.
	QPushButton *dumpButton_ = nullptr;
	QPushButton *cutButton_ = nullptr;
	QTimer *fillTimer_ = nullptr;

	// Stream mode's "Settings to check": its own amber card, opened the first
	// time something shows up.
	QFrame *warningsCard_ = nullptr;
	QVBoxLayout *warningsLayout_ = nullptr;
	std::vector<std::pair<QLabel *, QLabel *>> warningRows_; // (icon, text)
	int lastWarningCount_ = 0;

	// The folded sections: Delay by scene (stream mode only), Hotkey presets
	// and Options.
	std::vector<std::unique_ptr<Section>> sections_;
	Section *warningsSection_ = nullptr;
	Section *rulesSection_ = nullptr;
	Section *optionsSection_ = nullptr;
	// The delay and scenes cards fold too (open by default).
	Section *delaySection_ = nullptr;
	Section *scenesSection_ = nullptr;
	QWidget *rulesDivider_ = nullptr;
	QVBoxLayout *rulesList_ = nullptr;
	std::vector<RuleRow> ruleRows_;
	QLabel *rulesEmpty_ = nullptr;
	QPushButton *addRule_ = nullptr;
	QBoxLayout *presetRow_ = nullptr;
	QSpinBox *presetSpins_[3] = {nullptr, nullptr, nullptr};
	// The RAM estimate said "only fits..." last time: Options opens once when
	// it starts saying so, so the warning isn't hidden in a fold.
	bool fitWarned_ = false;

	// Options, stream mode only: where the delay buffer lives (Automatic /
	// Memory / Disk), how far a live raise is instant, one hint line; and
	// sending the last seconds when the stream stops.
	QWidget *bufferBlock_ = nullptr;
	QWidget *bufferHead_ = nullptr; // FitRow: the eyebrow and bufferInstant_.
	QComboBox *bufferCombo_ = nullptr;
	QLabel *bufferInstant_ = nullptr;
	QLabel *bufferHint_ = nullptr;
	QWidget *drainRow_ = nullptr;
	QCheckBox *drainCheck_ = nullptr;
	QLabel *drainText_ = nullptr;

	// Options > Advanced: "Apply the delay only to the stream", its hint, and
	// the lock line while a delay runs.
	QCheckBox *applyCheck_ = nullptr;
	QLabel *applyText_ = nullptr;
	QLabel *applyHint_ = nullptr;
	QWidget *applyLockRow_ = nullptr;
	QLabel *applyLockIcon_ = nullptr;

	// Free-account row (see RefreshAccountUi()): icon + text, an inline
	// secondary button (Sign out / Cancel) and, below, a full-width one
	// (Sign in / Try again, or Copy link when the browser didn't open).
	QLabel *accountIcon_ = nullptr;
	QLabel *accountLabel_ = nullptr;
	QPushButton *accountButton_ = nullptr;
	QPushButton *signInButton_ = nullptr;
	QPushButton *copyLinkButton_ = nullptr;
	bool linkCopied_ = false;

	// See OnReportBugClicked(). Always enabled, regardless of buffer state.
	QPushButton *reportBugButton_ = nullptr;

	// See CheckForUpdateAsync()/ShowUpdateNotice(). Hidden (setVisible(false))
	// until/unless a real update is found -- Qt layouts skip hidden widgets
	// entirely, so this reserves no space while empty. updateReleaseUrl_ is
	// what its click handler opens. The whole banner is one button.
	QPushButton *updateNoticeButton_ = nullptr;
	QLabel *updateIcon_ = nullptr;
	QLabel *updateTitle_ = nullptr;
	QLabel *updateVersion_ = nullptr;
	QLabel *updateExternalIcon_ = nullptr;
	QString updateReleaseUrl_;

	// See UpdateFillProgress()'s comment. fillElapsed_ is armed alongside
	// fillTimer_ in ArmFillTimer(); fillTotalSeconds_ is what the countdown
	// counts down FROM.
	QTimer *fillProgressTimer_ = nullptr;
	QElapsedTimer fillElapsed_;
	uint32_t fillTotalSeconds_ = 0; // Rounded up, for the "12 / 30 s" text.
	uint32_t fillTotalMs_ = 0;
	// The fill window fillTimer_ is armed for (BufferModeStatus::fillGeneration);
	// fillArmed_ is false whenever the timer is disarmed.
	uint32_t armedFillGeneration_ = 0;
	bool fillArmed_ = false;

	// The update check's background job (ui-tasks.hpp). Last member, so it is
	// destroyed first: a check still running is aborted with the dock.
	tasks::Group updateTasks_;
};

} // namespace trigglow
