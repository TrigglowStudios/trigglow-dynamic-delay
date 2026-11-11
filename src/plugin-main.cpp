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

#include <filesystem>
#include <memory>
#include <vector>

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QPointer>
#include <QScrollArea>
#include <QToolButton>
#include <QVBoxLayout>

extern "C" {
#include <obs-module.h>
#include <obs-frontend-api.h>
#include <plugin-support.h>
}

#ifdef TRIGGLOW_HAVE_FFMPEG
extern "C" {
#include <libavutil/avutil.h>
}
#endif

#include "audio-delay-filter.hpp"
#include "auth-manager.hpp"
#include "buffer-mode-controller.hpp"
#include "bug-report.hpp"
#include "delay-router.hpp"
#include "hotkeys.hpp"
#include "i18n.hpp"
#include "logging.hpp"
#include "obs-frontend-bridge.hpp"
#include "overlay-badge.hpp"
#include "settings-ui.hpp"
#include "stream-delay-controller.hpp"
#include "stream-mode-panel.hpp"
#include "ui-tasks.hpp"
#include "ui-theme.hpp"
#include "video-delay-filter.hpp"
#include "websocket-vendor.hpp"

OBS_DECLARE_MODULE()
OBS_MODULE_USE_DEFAULT_LOCALE(PLUGIN_NAME, "en-US")
OBS_MODULE_AUTHOR("Trigglow (VirosMs)")

// Required by OBS's plugin listing (Help > About, and the plugins manager) --
// was missing entirely before the OBS store submission pass (2026-08-26).
MODULE_EXPORT const char *obs_module_description(void)
{
	return "Delays your stream's video and audio by a configurable number of seconds without ever "
	       "reconnecting the stream -- built for stream snipers, spoiler-safe reactions, and chat-timing.";
}

namespace {

constexpr const char *kComponent = "plugin-main";
constexpr const char *kDockId = "trigglow_dynamic_delay_dock";
constexpr const char *kDockTitle = "Trigglow Dynamic Delay";
constexpr const char *kBufferSettingsFile = "buffer-mode-settings.json";
// v1.0.0 stream mode: its own file, so scene mode's stays exactly as before.
constexpr const char *kStreamSettingsFile = "stream-mode-settings.json";

// All plugin-lifetime state lives in one place, constructed in
// obs_module_load() and torn down in obs_module_unload(). No globals spread
// across the other files — every other module gets what it needs by
// reference/pointer from here.
//
// Buffer mode (BufferModeController) only, as of 2026-08-24. DelayController
// (native obs_output_set_delay reconnect mode) and its hotkeys.hpp/cpp
// wiring aren't instantiated here anymore -- live testing confirmed buffer
// mode works well and having two mechanisms + a mode selector was confusing
// for a non-technical streamer. DelayController's source stays in the repo
// (may come back) but is now dead code from the module's point of view;
// hotkeys.hpp/cpp was repointed to drive BufferModeController instead of
// deleted, since Stream Deck hotkey compatibility is a flagship feature
// that still needs a home.
struct PluginState {
	trigglow::ObsFrontendBridge bridge;
	// Renders the live overlay's PNGs (one per seconds value) into the
	// plugin's config folder; the bridge asks it through
	// SetOverlayImageProvider(). Outlives bufferController (declared before it).
	std::unique_ptr<trigglow::ui::DelayBadgeFiles> badgeFiles;
	// Constructed before bufferController so Enable() can be wired to gate on
	// it from the moment it's created (see SetAuthorizationCheck below) --
	// see docs/ACCOUNT_GATE.md for why this gate exists at all.
	std::unique_ptr<trigglow::AuthManager> authManager;
	std::unique_ptr<trigglow::BufferModeController> bufferController;
	// v1.0.0 stream mode (the delay on the encoded stream) and the router the
	// hotkeys and obs-websocket go through.
	std::unique_ptr<trigglow::StreamDelayController> streamController;
	std::unique_ptr<trigglow::DelayRouter> router;
	std::unique_ptr<trigglow::DelayHotkeys> hotkeys;
	// Owned by the dock container (Qt parent), like the dock itself: OBS may
	// destroy it first, hence the QPointer.
	QPointer<trigglow::StreamModePanel> streamPanel;
	// Not owned past AddDock(): OBS's dock system takes ownership of the
	// QWidget once obs_frontend_add_dock_by_id() returns true. Kept here
	// only as a non-owning reference for potential future use (e.g. forcing
	// a refresh), never deleted from this side.
	trigglow::TrigglowDelayDock *dock = nullptr;
	// False only when buffer-mode-settings.json exists, couldn't be parsed and
	// couldn't be set aside either (see LoadBufferSettings): then the file is
	// left untouched on exit instead of being replaced by the defaults.
	bool saveSettingsOnExit = true;
};

PluginState *g_state = nullptr;

// Registers the bundled Figtree/Geist Mono (data/fonts) for the plugin's own
// widgets and the overlay -- never as OBS's UI font -- and logs ONE line
// either way. Without them every role falls back to OBS's UI font.
void LoadBrandFonts()
{
	char *rawDir = obs_module_file("fonts");
	const QString dir = rawDir ? QString::fromUtf8(rawDir) : QString();
	bfree(rawDir);
	const trigglow::ui::BrandFontsReport report = trigglow::ui::LoadBrandFonts(dir);
	// The folder relative to the plugin, never its absolute path: OBS's log is
	// attached to support tickets, and a portable OBS lives under the user's
	// profile folder.
	const QByteArray where = dir.isEmpty() ? QByteArray("(no data/fonts folder)")
					       : QByteArray("the plugin's data/fonts folder");
	const QByteArray loaded = report.loaded.join(QStringLiteral(", ")).toUtf8();
	const QByteArray missing = report.missing.join(QStringLiteral(", ")).toUtf8();
	if (!report.failed.isEmpty()) {
		TRIGGLOW_LOG_WARN(kComponent,
				  "brand fonts: could not register %s in %s (loaded: %s); those roles use OBS's UI "
				  "font",
				  report.failed.join(QStringLiteral(", ")).toUtf8().constData(), where.constData(),
				  loaded.isEmpty() ? "none" : loaded.constData());
	} else if (!report.missing.isEmpty()) {
		TRIGGLOW_LOG_INFO(kComponent,
				  "brand fonts not bundled in %s (missing: %s; loaded: %s): using OBS's UI font",
				  where.constData(), missing.constData(),
				  loaded.isEmpty() ? "none" : loaded.constData());
	} else {
		TRIGGLOW_LOG_INFO(kComponent, "brand fonts loaded from %s: %s", where.constData(), loaded.constData());
	}
}

// Moves an unreadable settings file out of the way (rename, or copy if that
// fails) to "<file>.<yyyyMMdd-HHmmss>.invalid" next to it, so the defaults
// SaveBufferSettings() writes on exit never silently replace what the user (or
// support) put there. Returns the new path, or an empty string if neither
// worked.
QString SetAsideInvalidSettingsFile(const QString &path)
{
	const QString stamp = QDateTime::currentDateTime().toString(QStringLiteral("yyyyMMdd-HHmmss"));
	QString target = path + QLatin1Char('.') + stamp + QStringLiteral(".invalid");
	// Never overwrite an earlier copy (two bad starts in the same second).
	for (int n = 2; QFileInfo::exists(target); ++n)
		target = path + QLatin1Char('.') + stamp + QLatin1Char('-') + QString::number(n) +
			 QStringLiteral(".invalid");
	if (QFile::rename(path, target) || QFile::copy(path, target))
		return target;
	return {};
}

// "buffer_output_cap" and (v0.6.0) "codec_workers" are hidden support
// switches with no UI anywhere (see BufferModeController::LoadSettings): only
// a literal JSON false turns one off. Absent (or JSON null, which libobs
// drops on load) or true keeps it on, and so does any other type ("false" as
// a string, 0...), with a WARN: a typo must never quietly change how the
// buffer works. `what` names the feature for that WARN.
bool ReadHiddenSwitch(obs_data_t *data, const char *key, const char *what)
{
	obs_data_item_t *item = obs_data_item_byname(data, key);
	if (!item)
		return true;
	bool on = true;
	if (obs_data_item_gettype(item) == OBS_DATA_BOOLEAN) {
		on = obs_data_item_get_bool(item);
	} else {
		TRIGGLOW_LOG_WARN(kComponent,
				  "\"%s\" in %s is not a JSON boolean -- keeping %s on. Only a literal false (no "
				  "quotes) turns it off; this value is dropped from the file when OBS closes",
				  key, kBufferSettingsFile, what);
	}
	obs_data_item_release(&item);
	return on;
}

// v0.7.0 "buffer_codec", the same strict way (storage::ParseBufferCodec()):
// absent (or null) is "auto", and only the exact strings "auto" and "mjpeg"
// count; anything else ("MJPEG", "hevc", false, 0) keeps auto with a WARN, so
// a typo never changes the codec silently. True = auto.
bool ReadBufferCodec(obs_data_t *data)
{
	obs_data_item_t *item = obs_data_item_byname(data, "buffer_codec");
	trigglow::storage::SettingType type = trigglow::storage::SettingType::Missing;
	std::string text;
	if (item) {
		if (obs_data_item_gettype(item) == OBS_DATA_STRING) {
			type = trigglow::storage::SettingType::String;
			const char *value = obs_data_item_get_string(item);
			text = value ? value : "";
		} else {
			type = trigglow::storage::SettingType::Other;
		}
		obs_data_item_release(&item);
	}
	const trigglow::storage::BufferCodecSetting setting = trigglow::storage::ParseBufferCodec(type, text);
	if (setting.warn)
		TRIGGLOW_LOG_WARN(kComponent,
				  "\"buffer_codec\" in %s is neither \"auto\" nor \"mjpeg\" -- keeping auto; only the "
				  "literal string \"mjpeg\" forces MJPEG. This value is dropped from the file when OBS "
				  "closes",
				  kBufferSettingsFile);
	return setting.mode == trigglow::storage::BufferCodecMode::Auto;
}

// False when the settings file exists but can't be parsed AND couldn't be set
// aside: the caller must then not overwrite it on exit.
bool LoadBufferSettings(trigglow::BufferModeController &controller)
{
	char *rawPath = obs_module_get_config_path(obs_current_module(), kBufferSettingsFile);
	QString path = rawPath ? QString::fromUtf8(rawPath) : QString();
	bfree(rawPath);
	if (path.isEmpty())
		return true;

	const QByteArray pathUtf8 = path.toUtf8();
	if (!QFileInfo::exists(path)) {
		TRIGGLOW_LOG_INFO(kComponent, "no existing settings file, using defaults");
		return true;
	}
	obs_data_t *data = obs_data_create_from_json_file(pathUtf8.constData());
	if (!data) {
		// Exists but isn't readable JSON (hand-edited, truncated...). libobs
		// already logged the parse error; keep the file for whoever edited it.
		// File names only: the full path carries the Windows user name.
		const QString setAside = SetAsideInvalidSettingsFile(path);
		if (setAside.isEmpty()) {
			TRIGGLOW_LOG_WARN(kComponent,
					  "settings file %s is not valid JSON and could not be renamed or copied -- "
					  "using defaults this session and leaving the file untouched on exit",
					  kBufferSettingsFile);
			return false;
		}
		TRIGGLOW_LOG_WARN(kComponent,
				  "settings file %s is not valid JSON -- moved it to %s next to it and using "
				  "defaults (the file is written again when OBS closes)",
				  kBufferSettingsFile, QFileInfo(setAside).fileName().toUtf8().constData());
		return true;
	}

	uint32_t delaySeconds = static_cast<uint32_t>(obs_data_get_int(data, "delay_seconds"));
	if (delaySeconds == 0 && !obs_data_has_user_value(data, "delay_seconds"))
		delaySeconds = 5;
	uint32_t minResolutionHeight = static_cast<uint32_t>(obs_data_get_int(data, "min_resolution_height"));
	if (minResolutionHeight == 0)
		minResolutionHeight = 720;
	const char *liveScene = obs_data_get_string(data, "live_scene");
	const char *loadingScene = obs_data_get_string(data, "loading_scene");

	bool showOverlay = obs_data_has_user_value(data, "show_overlay") ? obs_data_get_bool(data, "show_overlay")
									 : true;
	// false brings back v0.5.0's buffer sizing. Read only here, at load.
	const bool bufferOutputCap = ReadHiddenSwitch(data, "buffer_output_cap", "the output-resolution cap");
	// v0.6.0 kill switch: false runs the MJPEG encode/decode on OBS's
	// graphics thread again (no codec worker threads). Read only here, at load.
	const bool codecWorkers = ReadHiddenSwitch(data, "codec_workers", "the codec worker threads");
	// v0.7.0: "mjpeg" keeps the buffer off the hardware storage codec. Read only here, at load.
	const bool bufferCodecAuto = ReadBufferCodec(data);

	controller.LoadSettings(delaySeconds, minResolutionHeight, liveScene ? liveScene : "",
				loadingScene ? loadingScene : "", showOverlay,
				static_cast<uint32_t>(obs_data_get_int(data, "overlay_corner")), bufferOutputCap,
				codecWorkers, bufferCodecAuto);

	TRIGGLOW_LOG_INFO(kComponent,
			  "settings loaded (delay=%us, min_res=%up, live=\"%s\", loading=\"%s\", output_cap=%s, "
			  "codec_workers=%s, buffer_codec=%s)",
			  delaySeconds, minResolutionHeight, liveScene ? liveScene : "",
			  loadingScene ? loadingScene : "", bufferOutputCap ? "on" : "off", codecWorkers ? "on" : "off",
			  bufferCodecAuto ? "auto" : "mjpeg");
	obs_data_release(data);
	return true;
}

void SaveBufferSettings(const trigglow::BufferModeController &controller)
{
	char *rawPath = obs_module_get_config_path(obs_current_module(), kBufferSettingsFile);
	QString path = rawPath ? QString::fromUtf8(rawPath) : QString();
	bfree(rawPath);
	if (path.isEmpty())
		return;

	QFileInfo info(path);
	QDir().mkpath(info.absolutePath());

	auto snapshot = controller.SaveSettings();

	obs_data_t *data = obs_data_create();
	obs_data_set_int(data, "delay_seconds", snapshot.delaySeconds);
	obs_data_set_int(data, "min_resolution_height", snapshot.minResolutionHeight);
	obs_data_set_string(data, "live_scene", snapshot.liveSceneName.c_str());
	obs_data_set_string(data, "loading_scene", snapshot.loadingSceneName.c_str());
	obs_data_set_bool(data, "show_overlay", snapshot.showOverlay);
	obs_data_set_int(data, "overlay_corner", snapshot.overlayCorner);
	// This file is rewritten from scratch here, so the hidden switch has to
	// be written back or a support-set false would be lost on the next exit.
	// Only when off: everyone else's file stays exactly as before.
	if (!snapshot.bufferOutputCap)
		obs_data_set_bool(data, "buffer_output_cap", false);
	if (!snapshot.codecWorkers)
		obs_data_set_bool(data, "codec_workers", false);
	if (!snapshot.bufferCodecAuto)
		obs_data_set_string(data, "buffer_codec", "mjpeg");
	obs_data_save_json(data, path.toUtf8().constData());
	obs_data_release(data);

	// File name only: the full path carries the Windows user name.
	TRIGGLOW_LOG_INFO(kComponent, "settings saved (%s in the plugin's OBS config folder)", kBufferSettingsFile);
}

QString ModuleConfigPath(const char *file)
{
	char *rawPath = obs_module_get_config_path(obs_current_module(), file);
	QString path = rawPath ? QString::fromUtf8(rawPath) : QString();
	bfree(rawPath);
	return path;
}

// Stream mode settings. The extra destinations' stream keys are kept here the
// same way OBS keeps the streamer's own key in its service.json.
void LoadStreamSettings(trigglow::StreamDelayController &controller)
{
	const QString path = ModuleConfigPath(kStreamSettingsFile);
	if (path.isEmpty() || !QFileInfo::exists(path))
		return;
	obs_data_t *data = obs_data_create_from_json_file(path.toUtf8().constData());
	if (!data) {
		const QString setAside = SetAsideInvalidSettingsFile(path);
		TRIGGLOW_LOG_WARN(kComponent, "settings file %s is not valid JSON -- %s; using defaults",
				  kStreamSettingsFile, setAside.isEmpty() ? "left as is" : "moved aside");
		return;
	}
	if (obs_data_has_user_value(data, "mode_enabled"))
		controller.SetModeEnabled(obs_data_get_bool(data, "mode_enabled"));
	if (obs_data_has_user_value(data, "delay_seconds"))
		controller.SetDelaySeconds(static_cast<uint32_t>(obs_data_get_int(data, "delay_seconds")));
	if (obs_data_has_user_value(data, "keep_seconds"))
		controller.SetKeepSeconds(static_cast<uint32_t>(obs_data_get_int(data, "keep_seconds")));
	if (obs_data_has_user_value(data, "drain_on_stop"))
		controller.SetDrainOnStop(obs_data_get_bool(data, "drain_on_stop"));
	if (obs_data_has_user_value(data, "show_overlay"))
		controller.SetShowOverlay(obs_data_get_bool(data, "show_overlay"));
	if (obs_data_has_user_value(data, "overlay_corner"))
		controller.SetOverlayCorner(static_cast<uint32_t>(obs_data_get_int(data, "overlay_corner")));
	for (int i = 0; i < 3; ++i) {
		const std::string key = "preset" + std::to_string(i + 1);
		if (obs_data_has_user_value(data, key.c_str()))
			controller.SetPreset(i, static_cast<uint32_t>(obs_data_get_int(data, key.c_str())));
	}
	std::vector<trigglow::SceneRule> rules;
	if (obs_data_array_t *array = obs_data_get_array(data, "scene_rules")) {
		const size_t count = obs_data_array_count(array);
		for (size_t i = 0; i < count; ++i) {
			obs_data_t *item = obs_data_array_item(array, i);
			trigglow::SceneRule rule;
			rule.scene = obs_data_get_string(item, "scene");
			const long long action = obs_data_get_int(item, "action");
			rule.action = action == 1   ? trigglow::SceneAction::DelayOn
				      : action == 2 ? trigglow::SceneAction::DelayOff
						    : trigglow::SceneAction::None;
			rule.seconds = static_cast<uint32_t>(obs_data_get_int(item, "seconds"));
			obs_data_release(item);
			if (!rule.scene.empty())
				rules.push_back(std::move(rule));
		}
		obs_data_array_release(array);
	}
	controller.SetSceneRules(std::move(rules));
	std::vector<trigglow::ExtraDestination> extras;
	if (obs_data_array_t *array = obs_data_get_array(data, "destinations")) {
		const size_t count = obs_data_array_count(array);
		for (size_t i = 0; i < count; ++i) {
			obs_data_t *item = obs_data_array_item(array, i);
			trigglow::ExtraDestination extra;
			extra.name = obs_data_get_string(item, "name");
			extra.server = obs_data_get_string(item, "server");
			extra.key = obs_data_get_string(item, "key");
			extra.enabled = !obs_data_has_user_value(item, "enabled") || obs_data_get_bool(item, "enabled");
			obs_data_release(item);
			if (!extra.server.empty())
				extras.push_back(std::move(extra));
		}
		obs_data_array_release(array);
	}
	controller.SetExtraDestinations(std::move(extras));
	obs_data_release(data);
	TRIGGLOW_LOG_INFO(kComponent, "stream mode settings loaded (%s, delay=%us, keep=%us, %zu extra destination(s))",
			  controller.ModeEnabled() ? "on" : "off", controller.DelaySeconds(), controller.KeepSeconds(),
			  controller.ExtraDestinations().size());
}

void SaveStreamSettings(const trigglow::StreamDelayController &controller)
{
	const QString path = ModuleConfigPath(kStreamSettingsFile);
	if (path.isEmpty())
		return;
	QDir().mkpath(QFileInfo(path).absolutePath());
	obs_data_t *data = obs_data_create();
	obs_data_set_bool(data, "mode_enabled", controller.ModeEnabled());
	obs_data_set_int(data, "delay_seconds", controller.DelaySeconds());
	obs_data_set_int(data, "keep_seconds", controller.KeepSeconds());
	obs_data_set_bool(data, "drain_on_stop", controller.DrainOnStop());
	obs_data_set_bool(data, "show_overlay", controller.ShowOverlay());
	obs_data_set_int(data, "overlay_corner", controller.OverlayCorner());
	for (int i = 0; i < 3; ++i)
		obs_data_set_int(data, ("preset" + std::to_string(i + 1)).c_str(), controller.Preset(i));
	obs_data_array_t *rules = obs_data_array_create();
	for (const trigglow::SceneRule &rule : controller.SceneRules()) {
		obs_data_t *item = obs_data_create();
		obs_data_set_string(item, "scene", rule.scene.c_str());
		obs_data_set_int(item, "action", static_cast<long long>(rule.action));
		obs_data_set_int(item, "seconds", rule.seconds);
		obs_data_array_push_back(rules, item);
		obs_data_release(item);
	}
	obs_data_set_array(data, "scene_rules", rules);
	obs_data_array_release(rules);
	obs_data_array_t *array = obs_data_array_create();
	for (const trigglow::ExtraDestination &extra : controller.ExtraDestinations()) {
		obs_data_t *item = obs_data_create();
		obs_data_set_string(item, "name", extra.name.c_str());
		obs_data_set_string(item, "server", extra.server.c_str());
		obs_data_set_string(item, "key", extra.key.c_str());
		obs_data_set_bool(item, "enabled", extra.enabled);
		obs_data_array_push_back(array, item);
		obs_data_release(item);
	}
	obs_data_set_array(data, "destinations", array);
	obs_data_array_release(array);
	obs_data_save_json_safe(data, path.toUtf8().constData(), "tmp", "bak");
	obs_data_release(data);
}

// The dock: stream mode on top, scene mode's dock below in a section that
// folds while stream mode handles the controls.
QWidget *BuildDockContainer(trigglow::StreamModePanel *panel, trigglow::TrigglowDelayDock *sceneDock)
{
	auto *container = new QWidget();
	auto *layout = new QVBoxLayout(container);
	layout->setContentsMargins(0, 0, 0, 0);
	layout->setSpacing(0);

	auto *panelScroll = new QScrollArea(container);
	panelScroll->setWidgetResizable(true);
	panelScroll->setFrameShape(QFrame::NoFrame);
	panelScroll->setWidget(panel);
	layout->addWidget(panelScroll, 1);

	auto *header = new QToolButton(container);
	header->setText(QString::fromUtf8(Str("Stream.SceneSection")));
	header->setCheckable(true);
	header->setChecked(true);
	header->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
	header->setArrowType(Qt::DownArrow);
	header->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
	layout->addWidget(header);
	sceneDock->setParent(container);
	layout->addWidget(sceneDock, 1);

	QObject::connect(header, &QToolButton::toggled, container, [header, sceneDock](bool open) {
		header->setArrowType(open ? Qt::DownArrow : Qt::RightArrow);
		sceneDock->setVisible(open);
	});
	panel->SetHandlesControlsChanged([header](bool streamHandles) { header->setChecked(!streamHandles); });
	return container;
}

} // namespace

bool obs_module_load(void)
{
	// The OBS this runs in and where it loaded the plugin from: with two copies
	// installed (OBS 33's folder and the legacy one for OBS 31 and 32) the log
	// says which one is in use. OBS's own module list logs the same paths.
	const char *modulePath = obs_get_module_binary_path(obs_current_module());
	obs_log(LOG_INFO, "loading Trigglow Dynamic Delay v%s on OBS Studio %s, from %s", PLUGIN_VERSION,
		obs_get_version_string(), modulePath ? modulePath : "(unknown path)");

	// v0.7.1: the oldest OBS supported, from buildspec.json's obsCompat. The
	// plugin is built against that libobs; an older OBS may lack functions it
	// calls, so it stops here with a clear line in the log instead.
	constexpr uint32_t kObsMinVersion = MAKE_SEMANTIC_VERSION(
		TRIGGLOW_OBS_MIN_VERSION_MAJOR, TRIGGLOW_OBS_MIN_VERSION_MINOR, TRIGGLOW_OBS_MIN_VERSION_PATCH);
	if (obs_get_version() < kObsMinVersion) {
		obs_log(LOG_ERROR, "Trigglow Dynamic Delay v%s requires OBS Studio %s or newer, running %s: not loaded",
			PLUGIN_VERSION, TRIGGLOW_OBS_MIN_VERSION_STRING, obs_get_version_string());
		return false;
	}

#ifdef TRIGGLOW_HAVE_FFMPEG
	// v0.3.0 Phase 0 smoke test (see docs/ROADMAP.md) -- proves FFmpeg
	// actually links AND loads correctly at runtime inside a real OBS
	// process, not just that it compiles. A static-link mistake or a
	// missing/mismatched runtime DLL would otherwise only surface as an
	// opaque plugin-load failure with no clue why. No codec logic uses
	// FFmpeg yet; this is purely a "the dependency is wired up" check.
	obs_log(LOG_INFO, "FFmpeg linked: %s", av_version_info());
#endif

	// Registers the OBS filters that buffer mode orchestrates under the hood
	// (see obs-frontend-bridge.cpp's EnsureBufferWrapperScene /
	// EnsureAudioDelayFilters). The user never adds either manually via the
	// Filters dialog -- see video-delay-filter.hpp / audio-delay-filter.hpp.
	trigglow::VideoDelayFilter::Register();
	trigglow::AudioDelayFilter::Register();

	// Background jobs (sign-in, Trigi, tickets, the update check): before
	// anything below can start one. Stopped first thing in
	// obs_module_unload().
	trigglow::uitasks::Start();

	g_state = new PluginState();

	// Before the dock and the first overlay image use them.
	LoadBrandFonts();
	// Resolved here: obs_current_module() is only valid during obs_module_load().
	{
		char *rawDir = obs_module_get_config_path(obs_current_module(), "overlay");
		g_state->badgeFiles =
			std::make_unique<trigglow::ui::DelayBadgeFiles>(rawDir ? QString::fromUtf8(rawDir) : QString());
		bfree(rawDir);
		g_state->bridge.SetOverlayImageProvider([files = g_state->badgeFiles.get()](uint32_t seconds) {
			const QString label = QString::fromUtf8(Str("Overlay.Label"));
			const QString value = QString::fromUtf8(Str("Overlay.Value")).arg(seconds);
			const QString signature = QString::fromUtf8(Str("Overlay.Signature"));
			return files->PathFor(seconds, label, value, signature).toStdString();
		});
	}

	g_state->authManager = std::make_unique<trigglow::AuthManager>();
	g_state->authManager->Init();
	// Log copies that versions before 0.6.0 left in %TEMP% (0.6.0 keeps the
	// redacted log in memory only).
	trigglow::DeleteLeftoverSupportLogCopies();
	// Marks this OBS's log, so "Report a problem" attaches it and never
	// another OBS's (a portable one keeps its logs next to its own config).
	{
		std::vector<std::filesystem::path> logFolders;
		logFolders.push_back(trigglow::ObsLogFolderFrom(g_state->authManager->ConfigDir()));
		if (char *profile = obs_frontend_get_current_profile_path()) {
			logFolders.push_back(trigglow::ObsLogFolderFrom(std::filesystem::u8path(profile)));
			bfree(profile);
		}
		trigglow::MarkCurrentObsLog(std::move(logFolders));
	}

	g_state->bufferController = std::make_unique<trigglow::BufferModeController>(g_state->bridge);
	// Free account gate (see docs/ACCOUNT_GATE.md): both the dock's Enable
	// button and the hotkey/Stream Deck path call straight into
	// BufferModeController::Enable()/Toggle(), so this single check covers
	// both. Captured by raw pointer, not the unique_ptr: AuthManager
	// outlives BufferModeController (constructed first above, destroyed
	// last in obs_module_unload()'s PluginState teardown), so this is always
	// valid for as long as bufferController itself is.
	// A suspended or inactive account (HTTP 403 from the server) blocks it too.
	g_state->bufferController->SetAuthorizationCheck(
		[auth = g_state->authManager.get()] { return auth->CanEnable(); });

	g_state->saveSettingsOnExit = LoadBufferSettings(*g_state->bufferController);

	// v1.0.0 stream mode: same free-account gate as scene mode.
	g_state->streamController = std::make_unique<trigglow::StreamDelayController>();
	g_state->streamController->SetAuthorizationCheck(
		[auth = g_state->authManager.get()] { return auth->CanEnable(); });
	g_state->streamController->SetOverlayImageProvider([files = g_state->badgeFiles.get()](uint32_t seconds) {
		const QString label = QString::fromUtf8(Str("Overlay.Label"));
		const QString value = QString::fromUtf8(Str("Overlay.Value")).arg(seconds);
		const QString signature = QString::fromUtf8(Str("Overlay.Signature"));
		return files->PathFor(seconds, label, value, signature).toStdString();
	});
	LoadStreamSettings(*g_state->streamController);
	g_state->streamController->Init();
	g_state->router =
		std::make_unique<trigglow::DelayRouter>(*g_state->bufferController, *g_state->streamController);

	g_state->hotkeys = std::make_unique<trigglow::DelayHotkeys>(*g_state->router);
	g_state->hotkeys->Init();

	auto *dock = new trigglow::TrigglowDelayDock(*g_state->bufferController, *g_state->authManager);
	g_state->dock = dock;
	auto *panel = new trigglow::StreamModePanel(*g_state->streamController, *g_state->authManager);
	g_state->streamPanel = panel;
	panel->SetSettingsChanged([] {
		if (g_state && g_state->streamController)
			SaveStreamSettings(*g_state->streamController);
	});
	g_state->streamController->SetStatusCallback([] {
		if (!g_state)
			return;
		if (g_state->streamPanel)
			g_state->streamPanel->Refresh();
		trigglow::websocket::NotifyStatus();
	});
	g_state->bridge.AddDock(kDockId, kDockTitle, BuildDockContainer(panel, dock));

	TRIGGLOW_LOG_INFO(kComponent, "loaded successfully");
	return true;
}

// obs-websocket registers its plugin API while it loads, after this module's
// obs_module_load(): the vendor requests go in here.
void obs_module_post_load(void)
{
	if (g_state && g_state->router)
		trigglow::websocket::Register(*g_state->router);
}

void obs_module_unload(void)
{
	trigglow::websocket::Unregister();
	// Before anything is torn down: aborts every network call still in
	// flight (a Trigi answer can take up to 90 s), drops every result not yet
	// delivered and waits for those threads, at most 2 s in all. Nothing of
	// this plugin runs on another thread, or from Qt's event queue, after OBS
	// unloads it.
	trigglow::uitasks::Shutdown();

	if (!g_state)
		return;

	if (g_state->saveSettingsOnExit)
		SaveBufferSettings(*g_state->bufferController);

	// Hotkeys must be torn down before we free the controller they point
	// back into.
	if (g_state->hotkeys)
		g_state->hotkeys->Shutdown();
	// Cuts the relay (and whatever it was still sending) and joins its threads.
	if (g_state->streamController) {
		SaveStreamSettings(*g_state->streamController);
		g_state->streamController->SetStatusCallback(nullptr);
		g_state->streamController->Shutdown();
	}
	if (g_state->streamPanel)
		g_state->streamPanel->Detach();
	g_state->streamPanel = nullptr;
	// Cancels any in-flight login poll timer before teardown (its network
	// calls already ended above) -- see AuthManager::Shutdown().
	if (g_state->authManager)
		g_state->authManager->Shutdown();
	g_state->bridge.Shutdown();

	// g_state->dock is intentionally NOT deleted here — see PluginState's
	// comment: OBS's dock system owns it after AddDock().
	delete g_state;
	g_state = nullptr;

	obs_log(LOG_INFO, "unloaded");
}
