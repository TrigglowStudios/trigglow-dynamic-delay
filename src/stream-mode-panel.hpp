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
// into a "Scene mode" section while stream mode handles the controls. Plain
// widgets on OBS's own theme; every string through Str().

#include <QWidget>

#include <functional>

class QCheckBox;
class QLabel;
class QLineEdit;
class QPushButton;
class QSpinBox;
class QTableWidget;
class QTimer;

namespace trigglow {

class AuthManager;
class StreamDelayController;

class StreamModePanel : public QWidget {
	Q_OBJECT

public:
	StreamModePanel(StreamDelayController &controller, AuthManager &auth, QWidget *parent = nullptr);

	// Called whenever stream mode starts or stops handling the controls, so
	// plugin-main.cpp can fold or unfold scene mode's section.
	void SetHandlesControlsChanged(std::function<void(bool)> callback);
	// Settings changed through the panel (to persist them).
	void SetSettingsChanged(std::function<void()> callback) { onSettingsChanged_ = std::move(callback); }

	void Refresh();
	// Stops every timer and makes Refresh() a no-op: called on module unload,
	// before the controller goes away, when OBS hasn't destroyed the dock yet.
	void Detach();

private:
	bool detached_ = false;
	void BuildUi();
	void RefreshDestinations();
	void AddDestination();
	void RemoveSelectedDestination();
	void SettingsChanged();

	StreamDelayController &controller_;
	AuthManager &auth_;
	std::function<void(bool)> onHandlesChanged_;
	std::function<void()> onSettingsChanged_;
	bool lastHandles_ = false;
	bool haveLastHandles_ = false;
	bool refreshing_ = false;

	QCheckBox *modeCheck_ = nullptr;
	QLabel *stateLabel_ = nullptr;
	QLabel *detailLabel_ = nullptr;
	QPushButton *toggleButton_ = nullptr;
	QPushButton *liveButton_ = nullptr;
	QPushButton *cutButton_ = nullptr;
	QSpinBox *delaySpin_ = nullptr;
	QLabel *statsLabel_ = nullptr;
	QLabel *warningLabel_ = nullptr;
	QLabel *accountLabel_ = nullptr;
	QPushButton *signInButton_ = nullptr;
	QSpinBox *keepSpin_ = nullptr;
	QCheckBox *drainCheck_ = nullptr;
	QTableWidget *destTable_ = nullptr;
	QLineEdit *destName_ = nullptr;
	QLineEdit *destServer_ = nullptr;
	QLineEdit *destKey_ = nullptr;
	QPushButton *destAdd_ = nullptr;
	QPushButton *destRemove_ = nullptr;
	QWidget *controlsBox_ = nullptr;
	QTimer *timer_ = nullptr;
};

} // namespace trigglow
