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

// v1.0.0: the dock's stream mode section (StreamDelayController). It sits
// above scene mode's dock (TrigglowDelayDock), which plugin-main.cpp folds
// into a "Scene mode" section while stream mode handles the controls.
//
// Look: the same design system as scene mode's dock (ui-theme.hpp): one style
// sheet built from OBS's palette, plus a few panel-only rules (collapsible
// section headers, preset chips, icon buttons) built from the same tokens,
// recomputed when OBS switches theme. Widgets carry tgRole/tgTone/tgVariant
// properties instead of their own colors. Every string through Str().

#include <QPointer>
#include <QWidget>

#include <functional>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "ui-theme.hpp"

class QBoxLayout;
class QCheckBox;
class QComboBox;
class QFrame;
class QLabel;
class QPushButton;
class QSpinBox;
class QTimer;
class QVBoxLayout;

namespace trigglow {

class AuthManager;
class StreamDelayController;
struct StreamDelayStatus;

class StreamModePanel : public QWidget {
	Q_OBJECT

public:
	StreamModePanel(StreamDelayController &controller, AuthManager &auth, QWidget *parent = nullptr);
	~StreamModePanel() override;

	// Called whenever stream mode starts or stops handling the controls, so
	// plugin-main.cpp can fold or unfold scene mode's section.
	void SetHandlesControlsChanged(std::function<void(bool)> callback);
	// Settings changed through the panel (to persist them).
	void SetSettingsChanged(std::function<void()> callback) { onSettingsChanged_ = std::move(callback); }

	// The header of the "Scene mode" fold under the panel (plugin-main.cpp's
	// BuildDockContainer): a toggle and a line saying which mode handles the
	// delay and the hotkeys now, and why scene mode is folded. Kept current by
	// the panel, which also themes `parent` (the dock's container) with its
	// style sheet; `onToggled` shows or hides scene mode.
	QWidget *CreateSceneHeader(QWidget *parent, std::function<void(bool open)> onToggled);
	void SetSceneSectionOpen(bool open);

	void Refresh();
	// Stops every timer and makes Refresh() a no-op: called on module unload,
	// before the controller goes away, when OBS hasn't destroyed the dock yet.
	void Detach();

	// Recomputes tokens, fonts and icons from the application palette (OBS's
	// active theme). Wired to OBS_FRONTEND_EVENT_THEME_CHANGED and to palette
	// change events; a no-op when nothing changed.
	void ApplyTheme(bool force = false);

protected:
	void changeEvent(QEvent *event) override;
	void resizeEvent(QResizeEvent *event) override;
	// Wheel guard (as in scene mode's dock): the wheel over a field that
	// doesn't have focus scrolls the dock instead of changing the value.
	bool eventFilter(QObject *watched, QEvent *event) override;

private:
	// A collapsible section: a header button (chevron, title, count chip)
	// over a body that hides when folded.
	struct Section {
		QPushButton *header = nullptr;
		QLabel *count = nullptr;
		QWidget *body = nullptr;
		bool open = false;
	};
	// One scene rule: scene, action, seconds, remove.
	struct RuleRow {
		QFrame *card = nullptr;
		QComboBox *scene = nullptr;
		QComboBox *action = nullptr;
		QSpinBox *seconds = nullptr;
		QPushButton *remove = nullptr;
	};

	void BuildUi();
	QFrame *BuildStatusCard();
	QFrame *BuildAccountCard();
	QFrame *BuildDelayCard();
	Section *AddSection(QVBoxLayout *parent, const QString &title, bool open);
	void SetSectionOpen(Section &section, bool open);
	void BuildRules(QVBoxLayout *body);
	void BuildPresets(QVBoxLayout *body);
	void BuildOptions(QVBoxLayout *body);
	void BuildWarnings(QVBoxLayout *body);

	void RefreshStatus(const StreamDelayStatus &status);
	void RefreshAccount();
	void RefreshDestination(const StreamDelayStatus &status);
	void RefreshWarnings(const StreamDelayStatus &status);
	void RefreshPresetChips(uint32_t delaySeconds);
	void RefreshModeLines(const StreamDelayStatus &status);
	void AddRuleRow(const std::string &scene, int action, int seconds);
	void RemoveRuleRow(QFrame *card);
	void RulesChanged();
	void SettingsChanged();
	void ApplyNarrowLayout(int width);
	void ApplyIcons();

	void SetRole(QWidget *widget, const char *qssRole, ui::FontRole fontRole);
	QColor ToneColor(const QString &tone) const;
	void SetIcon(QLabel *label, ui::IconId icon, const QString &tone, int size);
	void SetButtonIcon(QPushButton *button, ui::IconId icon, const QColor &color,
			   Qt::Alignment alignment = Qt::AlignLeft);
	void GuardWheel(QWidget *field);
	QString ExtraStyleSheet() const;

	StreamDelayController &controller_;
	AuthManager &auth_;
	std::function<void(bool)> onHandlesChanged_;
	std::function<void()> onSettingsChanged_;
	std::function<void(bool)> onSceneToggled_;
	bool detached_ = false;
	bool lastHandles_ = false;
	bool haveLastHandles_ = false;
	bool refreshing_ = false;
	bool frontendCallback_ = false;

	ui::Theme theme_;
	bool themeApplied_ = false;
	bool applyingTheme_ = false;
	std::vector<std::pair<QPointer<QWidget>, ui::FontRole>> fontRoles_;
	// Widgets outside the panel that take its style sheet (the scene header).
	std::vector<QPointer<QWidget>> themedPeers_;

	// Mode: the check box, its wrapping label and who handles the hotkeys.
	QCheckBox *modeCheck_ = nullptr;
	QLabel *modeText_ = nullptr;
	QLabel *hotkeysIcon_ = nullptr;
	QLabel *hotkeysLabel_ = nullptr;
	QWidget *controlsBox_ = nullptr;

	// Status card.
	QFrame *statusCard_ = nullptr;
	QFrame *statusRule_ = nullptr;
	QLabel *statusChip_ = nullptr;
	QLabel *statusIcon_ = nullptr;
	QLabel *stateLabel_ = nullptr;
	QLabel *detailLabel_ = nullptr;
	QLabel *statsLabel_ = nullptr;
	QWidget *noticeRow_ = nullptr;
	QLabel *noticeIcon_ = nullptr;
	QLabel *noticeLabel_ = nullptr;
	QWidget *ruleRow_ = nullptr;
	QLabel *ruleIcon_ = nullptr;
	QLabel *ruleLabel_ = nullptr;
	QWidget *sendsRow_ = nullptr;
	QLabel *sendsLabel_ = nullptr;
	QLabel *sendsState_ = nullptr;

	// Account card: only when turning the delay on needs a sign-in.
	QFrame *accountCard_ = nullptr;
	QLabel *accountIcon_ = nullptr;
	QLabel *accountLabel_ = nullptr;
	QPushButton *signInButton_ = nullptr;

	// Actions.
	QPushButton *onButton_ = nullptr;
	QPushButton *offButton_ = nullptr;
	QBoxLayout *actionRow_ = nullptr;
	QPushButton *liveButton_ = nullptr;
	QPushButton *dumpButton_ = nullptr;
	QPushButton *cutButton_ = nullptr;

	// Delay card.
	QSpinBox *delaySpin_ = nullptr;
	QPushButton *lessButton_ = nullptr;
	QPushButton *moreButton_ = nullptr;
	QBoxLayout *chipRow_ = nullptr;
	QPushButton *presetChips_[3] = {nullptr, nullptr, nullptr};

	// Sections.
	std::vector<std::unique_ptr<Section>> sections_;
	Section *warningsSection_ = nullptr;
	Section *rulesSection_ = nullptr;
	int lastWarningCount_ = 0;

	QWidget *warningsList_ = nullptr;
	QVBoxLayout *warningsLayout_ = nullptr;
	std::vector<std::pair<QLabel *, QLabel *>> warningRows_; // (icon, text)

	QVBoxLayout *rulesList_ = nullptr;
	std::vector<RuleRow> ruleRows_;
	QLabel *rulesEmpty_ = nullptr;
	QPushButton *addRule_ = nullptr;

	QBoxLayout *presetRow_ = nullptr;
	QSpinBox *presetSpins_[3] = {nullptr, nullptr, nullptr};

	// How far a live raise is instant (read-only since the spill).
	QLabel *keepValue_ = nullptr;
	QLabel *keepHelp_ = nullptr;
	QCheckBox *drainCheck_ = nullptr;
	QLabel *drainText_ = nullptr;
	QCheckBox *overlayCheck_ = nullptr;
	QLabel *overlayText_ = nullptr;
	QComboBox *cornerCombo_ = nullptr;

	// Scene mode fold (CreateSceneHeader()).
	QPushButton *sceneToggle_ = nullptr;
	QLabel *sceneHint_ = nullptr;
	bool sceneOpen_ = true;

	QTimer *timer_ = nullptr;
};

} // namespace trigglow
