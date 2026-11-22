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

// Which outputs stream mode names as "live without the delay" (other-outputs.hpp).

#include "check.hpp"
#include "other-outputs.hpp"

#include <cstring>
#include <string>
#include <vector>

using namespace trigglow::outputs;

#define CHECK_STR(a, b) CHECK(std::string(a) == std::string(b))

int main()
{
	testcheck::g_currentTest = "other outputs";

	// Stream outputs only: never a recording, the virtual camera or a replay buffer.
	CHECK(IsStreamOutputType("rtmp_output"));
	CHECK(IsStreamOutputType("ffmpeg_mpegts_muxer"));
	CHECK(IsStreamOutputType("whip_output"));
	CHECK(IsStreamOutputType("ftl_output"));
	CHECK(!IsStreamOutputType("ffmpeg_muxer"));
	CHECK(!IsStreamOutputType("mp4_output"));
	CHECK(!IsStreamOutputType("virtualcam_output"));
	CHECK(!IsStreamOutputType("replay_buffer"));
	CHECK(!IsStreamOutputType(""));

	// The names the three supported plugins give their outputs.
	CHECK(ClassifyOutputName("aitum_multi_output_Kick") == Plugin::AitumMultistream);
	CHECK(ClassifyOutputName("aitum_multi_output_") == Plugin::AitumMultistream);
	CHECK(ClassifyOutputName("multi-output") == Plugin::MultiRtmp);
	CHECK(ClassifyOutputName("vertical_canvas_stream") == Plugin::AitumVertical);
	CHECK(ClassifyOutputName("vertical_canvas_stream_0") == Plugin::AitumVertical);
	CHECK(ClassifyOutputName("multi-output-2") == Plugin::Other);
	CHECK(ClassifyOutputName("aitum_multi_service_Kick") == Plugin::Other);
	CHECK(ClassifyOutputName("Source Record") == Plugin::Other);
	CHECK(ClassifyOutputName("") == Plugin::Other);

	CHECK_STR(PluginKey(Plugin::AitumMultistream), "aitum");
	CHECK_STR(PluginKey(Plugin::MultiRtmp), "multirtmp");
	CHECK_STR(PluginKey(Plugin::AitumVertical), "vertical");
	CHECK_STR(PluginKey(Plugin::Other), "other");

	// One warning per plugin, in a fixed order, with how many outputs it runs.
	{
		const std::vector<OtherOutput> none;
		CHECK(SummarizeOtherOutputs(none).empty());
		const std::vector<OtherOutput> outputs = {
			{Plugin::Other, "Source Record"},    {Plugin::AitumMultistream, "aitum_multi_output_Kick"},
			{Plugin::MultiRtmp, "multi-output"}, {Plugin::AitumMultistream, "aitum_multi_output_YouTube"},
			{Plugin::MultiRtmp, "multi-output"}, {Plugin::AitumVertical, "vertical_canvas_stream"},
		};
		const std::vector<std::string> w = SummarizeOtherOutputs(outputs);
		CHECK(w.size() == 4);
		CHECK_STR(w[0], "other_aitum:2");
		CHECK_STR(w[1], "other_multirtmp:2");
		CHECK_STR(w[2], "other_vertical:1");
		CHECK_STR(w[3], "other_other:1");
	}
	return testcheck::Finish("other_outputs_tests");
}
