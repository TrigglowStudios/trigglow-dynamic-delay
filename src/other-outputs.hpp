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

// v1.1.0: stream outputs other plugins run next to OBS's own (multistream and
// vertical plugins). With "Apply the delay only to the stream" the relay only
// holds OBS's own stream output: those outputs connect to their platform
// themselves, live, and libobs offers no hook before they do (its "starting"
// signal fires after the output has connected). So stream mode names them in
// the dock, while they run, so the streamer knows they go out without the
// delay and can delay all of OBS instead (where every output of the main
// canvas is delayed, and the vertical canvas with it). The two most used
// multistream plugins and the vertical one are told apart by the names they
// give their outputs; anything else is "another plugin".
//
// Pure (no libobs): the listing itself is in stream-delay-controller.cpp.
// Tested in tests/other_outputs_tests.cpp.

#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

namespace trigglow::outputs {

enum class Plugin : unsigned char {
	AitumMultistream, // Aitum Multistream: outputs "aitum_multi_output_<name>".
	MultiRtmp,        // Multiple RTMP outputs (sorayuki): every output is "multi-output".
	AitumVertical,    // Aitum Vertical: "vertical_canvas_stream..." (its own canvas).
	Other,            // Any other plugin's stream output.
};

// Output type ids that send a stream to a platform (OBS's own and what the
// plugins above use): rtmp_output, ffmpeg_mpegts_muxer (SRT/RIST), whip_output,
// ftl_output. A recording, the virtual camera or a replay buffer never counts.
bool IsStreamOutputType(std::string_view outputTypeId);

// Which plugin an output's name points at (see the enum).
Plugin ClassifyOutputName(std::string_view outputName);

// The locale key suffix under "Stream.Warn.other_": "aitum", "multirtmp",
// "vertical", "other".
const char *PluginKey(Plugin plugin);

struct OtherOutput {
	Plugin plugin = Plugin::Other;
	std::string name;
};

// One warning per plugin seen, "other_<key>:<count>", in enum order: the form
// StreamDelayStatus::warnings carries and the dock formats (count as %1).
std::vector<std::string> SummarizeOtherOutputs(const std::vector<OtherOutput> &outputs);

} // namespace trigglow::outputs
