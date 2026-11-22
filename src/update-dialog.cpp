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

#include "update-dialog.hpp"
#include "api-client.hpp"
#include "logging.hpp"
#include "settings-ui-parts.hpp"
#include "ui-tasks.hpp"
#include "update-installer.hpp"

extern "C" {
#include <obs-module.h>
}

#include <QApplication>
#include <QCloseEvent>
#include <QDesktopServices>
#include <QEvent>
#include <QHBoxLayout>
#include <QLabel>
#include <QLocale>
#include <QPointer>
#include <QProgressBar>
#include <QPushButton>
#include <QTimer>
#include <QUrl>
#include <QVBoxLayout>

namespace trigglow {

using dockparts::kAccountIconSize;
using dockparts::kButtonIconBox;
using dockparts::kInlineIconSize;
using dockparts::MakeIconLabel;
using dockparts::T;

namespace {

constexpr const char *kComponent = "update-dialog";
constexpr int kMinimumWidth = 380;
constexpr int kPreferredWidth = 440;
constexpr int kBadgeSize = 36;
// How long the plugin waits for Setup to get admin rights (the UAC prompt
// stays open as long as the streamer leaves it).
constexpr unsigned long kLaunchTimeoutMs = 10 * 60 * 1000;

bool OpenWeb(const std::string &url)
{
	if (!api::IsAllowedWebUrl(url))
		return false;
	return QDesktopServices::openUrl(QUrl(QString::fromStdString(url)));
}

bool ObsSpeaksSpanish()
{
	const char *locale = obs_get_locale();
	return locale && locale[0] == 'e' && locale[1] == 's';
}

QLabel *MakeLabel(QWidget *parent, const char *role, bool wrap = true)
{
	auto *label = new QLabel(parent);
	label->setProperty("tgRole", QString::fromLatin1(role));
	label->setWordWrap(wrap);
	label->setTextFormat(Qt::PlainText);
	return label;
}

} // namespace

UpdateDialog::UpdateDialog(Options options, QWidget *parent) : QDialog(parent), options_(std::move(options))
{
	setWindowTitle(T("Update.Window.Title"));
	setWindowFlag(Qt::WindowContextHelpButtonHint, false);
	setMinimumWidth(kMinimumWidth);
	BuildUi();
	ApplyTheme();
	Refresh();

	// The stream, a recording or the delay can start or stop while the window
	// is open: the button follows.
	busyTimer_ = new QTimer(this);
	busyTimer_->setInterval(1000);
	connect(busyTimer_, &QTimer::timeout, this, [this] { Refresh(); });
	busyTimer_->start();

	resize(kPreferredWidth, sizeHint().height());
}

UpdateDialog::~UpdateDialog()
{
	// Aborts a download still running (and waits for it) before the members go.
	jobs_.reset();
}

void UpdateDialog::BuildUi()
{
	auto *root = new QVBoxLayout(this);
	root->setContentsMargins(20, 20, 20, 18);
	root->setSpacing(14);

	// Header: icon badge, then eyebrow, title and versions.
	auto *header = new QHBoxLayout();
	header->setSpacing(14);
	auto *badge = new QFrame(this);
	badge->setObjectName(QStringLiteral("tgUpdateBadge"));
	badge->setFixedSize(kBadgeSize, kBadgeSize);
	auto *badgeLayout = new QVBoxLayout(badge);
	badgeLayout->setContentsMargins(0, 0, 0, 0);
	badgeIcon_ = MakeIconLabel(badge, 18);
	badgeLayout->addWidget(badgeIcon_, 0, Qt::AlignCenter);
	header->addWidget(badge, 0, Qt::AlignTop);

	auto *heading = new QVBoxLayout();
	heading->setSpacing(3);
	eyebrow_ = MakeLabel(this, "eyebrow", false);
	eyebrow_->setText(T("Update.Eyebrow"));
	eyebrow_->setProperty("tgTone", QStringLiteral("accent"));
	title_ = MakeLabel(this, "title");
	title_->setText(T("Update.Title").arg(QString::fromStdString(options_.latestVersion)));
	versions_ = MakeLabel(this, "dataHint", false);
	versions_->setText(T("Dock.Update.Version")
				   .arg(QString::fromStdString(options_.latestVersion),
					QString::fromStdString(api::ClientVersion(options_.currentVersion))));
	heading->addWidget(eyebrow_);
	heading->addWidget(title_);
	heading->addWidget(versions_);
	header->addLayout(heading, 1);
	root->addLayout(header);

	body_ = MakeLabel(this, "muted");
	body_->setText(T(InPlace() ? "Update.Body.InPlace" : "Update.Body.Download"));
	root->addWidget(body_);

	notesButton_ = new QPushButton(T("Update.Notes"), this);
	notesButton_->setProperty("tgVariant", QStringLiteral("link"));
	notesButton_->setCursor(Qt::PointingHandCursor);
	notesButton_->setFocusPolicy(Qt::TabFocus);
	notesButton_->setIconSize(dockparts::kLinkIconBox);
	notesButton_->setToolTip(T("Update.Notes.Tip"));
	connect(notesButton_, &QPushButton::clicked, this, [this] {
		std::string url = std::string(api::web::kChangelog) + "?product=dynamic-delay";
		if (release::IsPlainVersion(options_.latestVersion))
			url += "#v" + options_.latestVersion;
		OpenWeb(url);
	});
	root->addWidget(notesButton_, 0, Qt::AlignLeft);

	// Why "Update now" waits (the stream, a recording, the delay).
	busyRow_ = new QFrame(this);
	busyRow_->setProperty("tgBanner", QStringLiteral("warning"));
	{
		auto *row = new QHBoxLayout(busyRow_);
		row->setContentsMargins(10, 8, 10, 8);
		row->setSpacing(8);
		busyIcon_ = MakeIconLabel(busyRow_, kInlineIconSize);
		busyText_ = MakeLabel(busyRow_, "strong");
		row->addWidget(busyIcon_, 0, Qt::AlignTop);
		row->addWidget(busyText_, 1);
	}
	busyRow_->setVisible(false);
	root->addWidget(busyRow_);

	// Download progress.
	progressBlock_ = new QWidget(this);
	{
		auto *block = new QVBoxLayout(progressBlock_);
		block->setContentsMargins(0, 0, 0, 0);
		block->setSpacing(6);
		auto *line = new QHBoxLayout();
		progressTitle_ = MakeLabel(progressBlock_, "eyebrow", false);
		progressValue_ = MakeLabel(progressBlock_, "dataHint", false);
		progressValue_->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
		line->addWidget(progressTitle_, 1);
		line->addWidget(progressValue_);
		block->addLayout(line);
		progressBar_ = new QProgressBar(progressBlock_);
		progressBar_->setTextVisible(false);
		progressBar_->setRange(0, 1000);
		block->addWidget(progressBar_);
	}
	progressBlock_->setVisible(false);
	root->addWidget(progressBlock_);

	// Errors and "accept the Windows prompt".
	messageRow_ = new QFrame(this);
	messageRow_->setProperty("tgBanner", QStringLiteral("info"));
	{
		auto *row = new QHBoxLayout(messageRow_);
		row->setContentsMargins(10, 8, 10, 8);
		row->setSpacing(8);
		messageIcon_ = MakeIconLabel(messageRow_, kInlineIconSize);
		messageText_ = MakeLabel(messageRow_, "strong");
		row->addWidget(messageIcon_, 0, Qt::AlignTop);
		row->addWidget(messageText_, 1);
	}
	messageRow_->setVisible(false);
	root->addWidget(messageRow_);

	root->addStretch(1);

	auto *buttons = new QHBoxLayout();
	buttons->setSpacing(8);
	buttons->addStretch(1);
	laterButton_ = new QPushButton(T("Update.Later"), this);
	laterButton_->setProperty("tgVariant", QStringLiteral("secondary"));
	laterButton_->setProperty("tgSize", QStringLiteral("large"));
	laterButton_->setFocusPolicy(Qt::TabFocus);
	laterButton_->setCursor(Qt::PointingHandCursor);
	connect(laterButton_, &QPushButton::clicked, this, [this] {
		if (step_ == Step::Downloading)
			CancelDownload();
		else
			reject();
	});
	primaryButton_ = new QPushButton(this);
	primaryButton_->setProperty("tgVariant", QStringLiteral("primary"));
	primaryButton_->setFocusPolicy(Qt::TabFocus);
	primaryButton_->setCursor(Qt::PointingHandCursor);
	primaryButton_->setIconSize(kButtonIconBox);
	primaryButton_->setDefault(true);
	connect(primaryButton_, &QPushButton::clicked, this, [this] { StartUpdate(); });
	buttons->addWidget(laterButton_);
	buttons->addWidget(primaryButton_);
	root->addLayout(buttons);
}

void UpdateDialog::ApplyTheme()
{
	if (applyingTheme_)
		return;
	applyingTheme_ = true;
	const double basePt = ui::BaseFontPointSize(this);
	ui::Theme theme = ui::BuildTheme(QApplication::palette(), basePt);
	if (theme.styleSheet != theme_.styleSheet)
		setStyleSheet(theme.styleSheet);
	theme_ = std::move(theme);
	for (QLabel *label : findChildren<QLabel *>()) {
		const QString role = label->property("tgRole").toString();
		if (role == QLatin1String("eyebrow"))
			ui::ApplyFont(label, ui::FontRole::Eyebrow, basePt);
		else if (role == QLatin1String("title"))
			ui::ApplyFont(label, ui::FontRole::Title, basePt);
		else if (role == QLatin1String("dataHint"))
			ui::ApplyFont(label, ui::FontRole::DataHint, basePt);
		else if (role == QLatin1String("strong"))
			ui::ApplyFont(label, ui::FontRole::Strong, basePt);
	}
	for (QPushButton *button : findChildren<QPushButton *>()) {
		const bool link = button->property("tgVariant").toString() == QLatin1String("link");
		ui::ApplyFont(button, link ? ui::FontRole::Strong : ui::FontRole::Button, basePt);
	}

	const uitokens::Tokens &t = theme_.tokens;
	const qreal dpr = devicePixelRatioF();
	const QColor accent = ui::ToQColor(t.accentText);
	const QColor disabled = ui::ToQColor(t.disabledFg);
	badgeIcon_->setPixmap(ui::IconPixmap(ui::IconId::Download, accent, 18, dpr));
	busyIcon_->setPixmap(ui::IconPixmap(ui::IconId::Lock, ui::ToQColor(t.warningText), kInlineIconSize, dpr));
	messageIcon_->setPixmap(ui::IconPixmap(messageIsError_ ? ui::IconId::AlertTriangle : ui::IconId::Shield,
					       ui::ToQColor(messageIsError_ ? t.dangerText : t.accentText),
					       kInlineIconSize, dpr));
	notesButton_->setIcon(ui::Icon(ui::IconId::ExternalLink, accent, disabled, Qt::AlignLeft));
	primaryButton_->setIcon(ui::Icon(InPlace() ? ui::IconId::Download : ui::IconId::ExternalLink,
					 ui::ToQColor(t.onGradient), disabled, Qt::AlignLeft));
	applyingTheme_ = false;
}

void UpdateDialog::changeEvent(QEvent *event)
{
	QDialog::changeEvent(event);
	if (event->type() == QEvent::ApplicationPaletteChange || event->type() == QEvent::ApplicationFontChange)
		ApplyTheme(); // OBS switched theme while the window is open.
}

void UpdateDialog::closeEvent(QCloseEvent *event)
{
	jobs_.reset(); // A download in flight stops with the window.
	QDialog::closeEvent(event);
}

void UpdateDialog::reject()
{
	jobs_.reset();
	QDialog::reject();
}

void UpdateDialog::SetStep(Step step)
{
	step_ = step;
	Refresh();
}

void UpdateDialog::Refresh()
{
	const QString busy = (InPlace() && options_.busyReason && (step_ == Step::Ready || step_ == Step::Failed))
				     ? options_.busyReason()
				     : QString();
	if (busy != lastBusy_) {
		lastBusy_ = busy;
		busyText_->setText(busy);
		busyRow_->setVisible(!busy.isEmpty());
	}

	progressBlock_->setVisible(step_ == Step::Downloading || step_ == Step::Starting || step_ == Step::Closing);
	switch (step_) {
	case Step::Ready:
	case Step::Failed:
		primaryButton_->setText(step_ == Step::Failed && InPlace()
						? T("Update.Retry")
						: T(InPlace() ? "Update.Now" : "Update.Download"));
		primaryButton_->setEnabled(busy.isEmpty());
		laterButton_->setText(T("Update.Later"));
		laterButton_->setEnabled(true);
		break;
	case Step::Downloading:
		primaryButton_->setText(T("Update.Now"));
		primaryButton_->setEnabled(false);
		laterButton_->setText(T("Update.Cancel"));
		laterButton_->setEnabled(true);
		progressTitle_->setText(T("Update.Downloading"));
		break;
	case Step::Starting:
	case Step::Closing:
		// Setup is already asking Windows for rights: leaving now would leave
		// it waiting for an OBS that never closes (the window's X still can).
		primaryButton_->setEnabled(false);
		laterButton_->setEnabled(false);
		laterButton_->setText(T("Update.Later"));
		progressTitle_->setText(T("Update.Installing"));
		progressValue_->clear();
		break;
	}
}

void UpdateDialog::ShowError(const QString &text)
{
	messageIsError_ = true;
	ui::SetStyleProperty(messageRow_, "tgBanner", QStringLiteral("danger"));
	messageText_->setText(text);
	messageRow_->setVisible(true);
	ApplyTheme();
}

void UpdateDialog::SetProgress(uint64_t received, uint64_t total)
{
	if (step_ != Step::Downloading)
		return;
	const int permille = total ? static_cast<int>(received * 1000 / total) : 0;
	progressBar_->setValue(permille);
	progressValue_->setText(T("Update.Progress").arg(FormatMegabytes(received), FormatMegabytes(total)));
}

QString UpdateDialog::FormatMegabytes(uint64_t bytes) const
{
	const char *locale = obs_get_locale();
	const QLocale ui = locale && *locale
				   ? QLocale(QString::fromUtf8(locale).replace(QLatin1Char('-'), QLatin1Char('_')))
				   : QLocale();
	return ui.toString(static_cast<double>(bytes) / (1024.0 * 1024.0), 'f', 1);
}

void UpdateDialog::StartUpdate()
{
	if (!InPlace()) {
		OpenWeb(api::web::kProduct);
		accept();
		return;
	}
	if (step_ == Step::Downloading || step_ == Step::Starting || step_ == Step::Closing)
		return;
	if (options_.busyReason && !options_.busyReason().isEmpty()) {
		Refresh();
		return;
	}
	messageRow_->setVisible(false);
	jobs_ = std::make_unique<tasks::Group>(uitasks::Registry());
	SetStep(Step::Downloading);
	SetProgress(0, options_.installer->size);
	TRIGGLOW_LOG_INFO(kComponent, "update to v%s requested from OBS", options_.latestVersion.c_str());

	QPointer<UpdateDialog> self(this);
	const release::Installer installer = *options_.installer;
	const std::filesystem::path updatesDir = options_.configDir / "updates";
	const std::string userAgent = api::UserAgent(options_.currentVersion);
	const bool started = jobs_->Run([self, installer, updatesDir, userAgent](const tasks::TaskContext &ctx) {
		int lastPermille = -1;
		auto progress = [&ctx, &lastPermille, self](uint64_t received, uint64_t total) {
			const int permille = total ? static_cast<int>(received * 1000 / total) : 0;
			if (permille == lastPermille)
				return;
			lastPermille = permille;
			ctx.Post([self, received, total] {
				if (self)
					self->SetProgress(received, total);
			});
		};
		updater::DownloadResult result;
		try {
			result = updater::DownloadInstaller(installer, updatesDir, userAgent, progress, &ctx.Token());
		} catch (...) {
			result.error = updater::DownloadError::Disk;
		}
		ctx.Post([self, error = static_cast<int>(result.error), file = result.file] {
			if (self)
				self->OnDownloaded(error, file);
		});
	});
	if (!started) {
		SetStep(Step::Failed);
		ShowError(T("Update.Error.Network"));
	}
}

void UpdateDialog::CancelDownload()
{
	jobs_.reset();
	progressBar_->setValue(0);
	SetStep(Step::Ready);
}

void UpdateDialog::OnDownloaded(int error, const std::filesystem::path &file)
{
	if (step_ != Step::Downloading)
		return;
	switch (static_cast<updater::DownloadError>(error)) {
	case updater::DownloadError::None:
		break;
	case updater::DownloadError::Cancelled:
		SetStep(Step::Ready);
		return;
	case updater::DownloadError::Size:
	case updater::DownloadError::Hash:
	case updater::DownloadError::Redirect:
		SetStep(Step::Failed);
		ShowError(T("Update.Error.Verify"));
		return;
	case updater::DownloadError::Disk:
		SetStep(Step::Failed);
		ShowError(T("Update.Error.Disk"));
		return;
	default:
		SetStep(Step::Failed);
		ShowError(T("Update.Error.Network"));
		return;
	}

	// The window may have sat there: check again right before OBS closes.
	if (options_.busyReason && !options_.busyReason().isEmpty()) {
		SetStep(Step::Ready);
		return;
	}
	progressBar_->setValue(1000);
	SetStep(Step::Starting);
	messageIsError_ = false;
	ui::SetStyleProperty(messageRow_, "tgBanner", QStringLiteral("info"));
	messageText_->setText(T("Update.Starting"));
	messageRow_->setVisible(true);
	ApplyTheme();

	QPointer<UpdateDialog> self(this);
	const std::string sha256 = options_.installer->sha256;
	const bool spanish = ObsSpeaksSpanish();
	const bool started = jobs_ && jobs_->Run([self, file, sha256, spanish](const tasks::TaskContext &ctx) {
		updater::LaunchResult result;
		try {
			result = updater::LaunchInstaller(file, sha256, spanish, kLaunchTimeoutMs, &ctx.Token());
		} catch (...) {
			result.outcome = updater::LaunchOutcome::Failed;
		}
		ctx.Post([self, outcome = static_cast<int>(result.outcome), code = result.code] {
			if (self)
				self->OnLaunched(outcome, code);
		});
	});
	if (!started) {
		SetStep(Step::Failed);
		ShowError(T("Update.Error.Start"));
	}
}

void UpdateDialog::OnLaunched(int outcome, unsigned long code)
{
	if (step_ != Step::Starting)
		return;
	switch (static_cast<updater::LaunchOutcome>(outcome)) {
	case updater::LaunchOutcome::Ready: {
		SetStep(Step::Closing);
		messageText_->setText(T("Update.Closing"));
		// A beat to read it, then OBS closes; Setup waits for it.
		QPointer<UpdateDialog> self(this);
		QTimer::singleShot(900, this, [self] {
			if (self && self->options_.closeObs)
				self->options_.closeObs();
		});
		// Still here a while later: OBS didn't close (its own "are you sure?"
		// was answered no). Setup keeps waiting for it.
		QTimer::singleShot(8000, this, [self] {
			if (!self || self->step_ != Step::Closing)
				return;
			self->SetStep(Step::Failed);
			self->ShowError(T("Update.Error.StillOpen"));
		});
		return;
	}
	case updater::LaunchOutcome::Declined:
		TRIGGLOW_LOG_INFO(kComponent, "update cancelled before it started (installer exit code %lu)", code);
		SetStep(Step::Failed);
		ShowError(T("Update.Error.Declined"));
		return;
	case updater::LaunchOutcome::TimedOut:
		SetStep(Step::Failed);
		ShowError(T("Update.Error.Timeout"));
		return;
	case updater::LaunchOutcome::Cancelled:
		SetStep(Step::Ready);
		messageRow_->setVisible(false);
		return;
	default:
		TRIGGLOW_LOG_WARN(kComponent, "couldn't start the installer (error %lu)", code);
		SetStep(Step::Failed);
		ShowError(T("Update.Error.Start"));
		return;
	}
}

} // namespace trigglow
