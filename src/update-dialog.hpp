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

#include <QDialog>
#include <QString>

#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <string>

#include "background-tasks.hpp"
#include "release-info.hpp"
#include "ui-theme.hpp"

class QLabel;
class QProgressBar;
class QPushButton;
class QTimer;
class QWidget;

// v1.1.0: the "new version" window. Opens by itself the first time OBS
// starts after a version is published (once per version), and from the dock's
// update card. Non-modal: OBS stays usable behind it.
//
// Windows, installed with the installer (updater::CanInstallInPlace()):
// "Update now" downloads the release's installer, checks it against the
// SHA-256 GitHub announces, starts it, and once Setup has admin rights asks
// OBS to close; Setup installs and opens OBS again (update-installer.hpp).
// While OBS streams or records, or the delay runs, the button waits (the
// window says why): closing OBS then would cut the stream.
//
// Anywhere else (macOS, Linux, a manual install): "Download" opens the
// plugin's page on trigglow.com, never GitHub.
namespace trigglow {

class UpdateDialog : public QDialog {
	Q_OBJECT

public:
	struct Options {
		std::string currentVersion; // PLUGIN_VERSION.
		std::string latestVersion;
		std::optional<release::Installer> installer; // Set only when CanInstallInPlace().
		std::filesystem::path configDir;             // The plugin's config folder (updates/ goes there).
		// Why updating now would cut something ("" when it wouldn't): the
		// stream, a recording, the delay. Polled every second.
		std::function<QString()> busyReason;
		// Asks OBS to close (main window close()).
		std::function<void()> closeObs;
	};

	UpdateDialog(Options options, QWidget *parent = nullptr);
	~UpdateDialog() override;

	const std::string &LatestVersion() const { return options_.latestVersion; }

protected:
	void changeEvent(QEvent *event) override;
	void closeEvent(QCloseEvent *event) override;
	void reject() override;

private:
	enum class Step { Ready, Downloading, Starting, Closing, Failed };

	void BuildUi();
	void ApplyTheme();
	void SetStep(Step step);
	void Refresh();
	void ShowError(const QString &text);
	void StartUpdate();
	void CancelDownload();
	void OnDownloaded(int error, const std::filesystem::path &file);
	void OnLaunched(int outcome, unsigned long code);
	void SetProgress(uint64_t received, uint64_t total);
	QString FormatMegabytes(uint64_t bytes) const;
	bool InPlace() const { return options_.installer.has_value(); }

	Options options_;
	Step step_ = Step::Ready;
	ui::Theme theme_;
	bool applyingTheme_ = false;

	QLabel *badgeIcon_ = nullptr;
	QLabel *eyebrow_ = nullptr;
	QLabel *title_ = nullptr;
	QLabel *versions_ = nullptr;
	QLabel *body_ = nullptr;
	QPushButton *notesButton_ = nullptr;
	QWidget *busyRow_ = nullptr;
	QLabel *busyIcon_ = nullptr;
	QLabel *busyText_ = nullptr;
	QWidget *progressBlock_ = nullptr;
	QLabel *progressTitle_ = nullptr;
	QLabel *progressValue_ = nullptr;
	QProgressBar *progressBar_ = nullptr;
	QWidget *messageRow_ = nullptr;
	QLabel *messageIcon_ = nullptr;
	QLabel *messageText_ = nullptr;
	bool messageIsError_ = false;
	QPushButton *laterButton_ = nullptr;
	QPushButton *primaryButton_ = nullptr;
	QTimer *busyTimer_ = nullptr;
	QString lastBusy_;

	// Download and launch jobs (ui-tasks.hpp). One group per attempt, so
	// "Cancel" aborts it alone; reset() cancels and waits.
	std::unique_ptr<tasks::Group> jobs_;
};

} // namespace trigglow
