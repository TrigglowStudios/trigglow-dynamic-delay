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

// v1.0.0 one panel: what the delay applies to (src/apply-to.*) and the router
// following the dock's check box (src/delay-router.hpp).
//
// 1. The setting: "apply_to" in stream-mode-settings.json, all of OBS by
//    default, and the migration of 1.0 test builds' "mode_enabled" (true by
//    default there) to all of OBS.
// 2. The router: every control (the dock's button, hotkeys, obs-websocket,
//    Stream Deck) goes to the selected mode only, and switches with the box.

#include "apply-to.hpp"
#include "check.hpp"
#include "delay-router.hpp"

#include <string>
#include <vector>

using namespace trigglow;

namespace {

// ---- 1. The setting --------------------------------------------------------

void TestDefaults()
{
	testcheck::g_currentTest = "defaults";
	// No settings file, or one from before the choice: all of OBS.
	const ApplyToSetting none = ParseApplyTo(ApplyToType::Missing, "", false);
	CHECK(none.value == ApplyTo::All);
	CHECK(!none.droppedModeEnabled);
	CHECK(!none.warn);
}

void TestMigrationFromTestBuilds()
{
	testcheck::g_currentTest = "migration";
	// 1.0 test builds saved "mode_enabled": true (their default). 1.0 was never
	// released: everyone starts on all of OBS, and the key is dropped.
	const ApplyToSetting fromTrue = ParseApplyTo(ApplyToType::Missing, "", true);
	CHECK(fromTrue.value == ApplyTo::All);
	CHECK(fromTrue.droppedModeEnabled);
	CHECK(!fromTrue.warn);
	// "mode_enabled": false the same way (its value is never read).
	const ApplyToSetting fromFalse = ParseApplyTo(ApplyToType::Missing, "", true);
	CHECK(fromFalse.value == ApplyTo::All);
	CHECK(fromFalse.droppedModeEnabled);
	// A file saved by this version (apply_to) next to a leftover mode_enabled:
	// apply_to wins, whatever mode_enabled said.
	const ApplyToSetting both = ParseApplyTo(ApplyToType::String, "stream", true);
	CHECK(both.value == ApplyTo::Stream);
	CHECK(both.droppedModeEnabled);
	const ApplyToSetting bothAll = ParseApplyTo(ApplyToType::String, "all", true);
	CHECK(bothAll.value == ApplyTo::All);
}

void TestValues()
{
	testcheck::g_currentTest = "values";
	CHECK(ParseApplyTo(ApplyToType::String, "stream", false).value == ApplyTo::Stream);
	CHECK(!ParseApplyTo(ApplyToType::String, "stream", false).warn);
	CHECK(ParseApplyTo(ApplyToType::String, "all", false).value == ApplyTo::All);
	CHECK(!ParseApplyTo(ApplyToType::String, "all", false).warn);
	// Anything else is all of OBS, with a warning: a typo never moves the
	// delay off the recording silently.
	for (const char *typo : {"Stream", "STREAM", "only_stream", "", " stream", "scene"}) {
		const ApplyToSetting s = ParseApplyTo(ApplyToType::String, typo, false);
		CHECK(s.value == ApplyTo::All);
		CHECK(s.warn);
	}
	const ApplyToSetting other = ParseApplyTo(ApplyToType::Other, "", false);
	CHECK(other.value == ApplyTo::All);
	CHECK(other.warn);
}

void TestRoundTrip()
{
	testcheck::g_currentTest = "round trip";
	for (ApplyTo value : {ApplyTo::All, ApplyTo::Stream}) {
		const ApplyToSetting back = ParseApplyTo(ApplyToType::String, ApplyToName(value), false);
		CHECK(back.value == value);
		CHECK(!back.warn);
	}
	CHECK(std::string(ApplyToName(ApplyTo::All)) == "all");
	CHECK(std::string(ApplyToName(ApplyTo::Stream)) == "stream");
}

void TestControlsGoToStream()
{
	testcheck::g_currentTest = "controls go to stream";
	CHECK(!ControlsGoToStream(ApplyTo::All, true));
	CHECK(!ControlsGoToStream(ApplyTo::All, false));
	CHECK(ControlsGoToStream(ApplyTo::Stream, true));
	// Ticked, but the relay doesn't exist on this system: scene mode, which
	// is also what the dock shows then.
	CHECK(!ControlsGoToStream(ApplyTo::Stream, false));
}

// ---- 2. The router ----------------------------------------------------------

// Records every call it gets.
class FakeTarget final : public DelayTarget {
public:
	explicit FakeTarget(const char *mode) : mode_(mode) {}
	void Enable() override { calls.push_back("enable"); }
	void Disable() override { calls.push_back("disable"); }
	void Toggle() override { calls.push_back("toggle"); }
	void AdjustDelay(int delta) override { calls.push_back("adjust " + std::to_string(delta)); }
	void SetDelay(uint32_t seconds) override { calls.push_back("set " + std::to_string(seconds)); }
	bool Dump() override
	{
		calls.push_back("dump");
		return dumpResult;
	}
	void ApplyPreset(int index) override { calls.push_back("preset " + std::to_string(index)); }
	RouterStatus Status() const override
	{
		RouterStatus s;
		s.mode = mode_;
		return s;
	}

	std::vector<std::string> calls;
	bool dumpResult = false;

private:
	std::string mode_;
};

// Every control once, as hotkeys.cpp and websocket-vendor.cpp send them.
void DriveAll(DelayRouter &router)
{
	router.Enable();
	router.Disable();
	router.Toggle();
	router.SkipToLive();
	router.AdjustDelay(5);
	router.AdjustDelay(-5);
	router.SetDelay(42);
	router.Dump();
	router.ApplyPreset(2);
}

const std::vector<std::string> kAllCalls = {"enable",    "disable", "toggle", "disable", "adjust 5",
					    "adjust -5", "set 42",  "dump",   "preset 2"};

void TestRouterFollowsCheckBox()
{
	testcheck::g_currentTest = "router follows the check box";
	FakeTarget scene("scene");
	FakeTarget stream("stream");
	// The dock's check box and whether stream mode can run, as
	// StreamDelayController::HandlesControls() combines them.
	ApplyTo choice = ApplyTo::All; // The default.
	bool available = true;
	DelayRouter router(scene, stream, [&] { return ControlsGoToStream(choice, available); });

	// Default: all of OBS. Nothing reaches stream mode.
	CHECK(!router.StreamMode());
	DriveAll(router);
	CHECK(scene.calls == kAllCalls);
	CHECK(stream.calls.empty());
	CHECK(router.Status().mode == "scene");

	// Ticked: only stream mode gets them.
	scene.calls.clear();
	choice = ApplyTo::Stream;
	CHECK(router.StreamMode());
	DriveAll(router);
	CHECK(stream.calls == kAllCalls);
	CHECK(scene.calls.empty());
	CHECK(router.Status().mode == "stream");

	// Unticked again: back to scene mode at once, no restart.
	stream.calls.clear();
	choice = ApplyTo::All;
	router.Toggle();
	CHECK(scene.calls == std::vector<std::string>{"toggle"});
	CHECK(stream.calls.empty());

	// Ticked but stream mode can't run here: scene mode.
	scene.calls.clear();
	choice = ApplyTo::Stream;
	available = false;
	CHECK(!router.StreamMode());
	router.Enable();
	CHECK(scene.calls == std::vector<std::string>{"enable"});
	CHECK(stream.calls.empty());
	CHECK(router.Status().mode == "scene");
}

void TestDumpResult()
{
	testcheck::g_currentTest = "dump result";
	FakeTarget scene("scene");
	FakeTarget stream("stream");
	bool streamOnly = false;
	DelayRouter router(scene, stream, [&] { return streamOnly; });
	stream.dumpResult = true;
	// Scene mode never has anything to dump, and the stream's answer is not
	// asked for then.
	CHECK(!router.Dump());
	CHECK(stream.calls.empty());
	streamOnly = true;
	CHECK(router.Dump());
	stream.dumpResult = false;
	CHECK(!router.Dump());
}

} // namespace

int main()
{
	TestDefaults();
	TestMigrationFromTestBuilds();
	TestValues();
	TestRoundTrip();
	TestControlsGoToStream();
	TestRouterFollowsCheckBox();
	TestDumpResult();
	return testcheck::Finish("apply_to_tests");
}
