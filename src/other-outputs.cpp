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

#include "other-outputs.hpp"

#include <array>

namespace trigglow::outputs {

namespace {
bool StartsWith(std::string_view s, std::string_view prefix)
{
	return s.size() >= prefix.size() && s.compare(0, prefix.size(), prefix) == 0;
}
} // namespace

bool IsStreamOutputType(std::string_view outputTypeId)
{
	return outputTypeId == "rtmp_output" || outputTypeId == "ffmpeg_mpegts_muxer" ||
	       outputTypeId == "whip_output" || outputTypeId == "ftl_output";
}

Plugin ClassifyOutputName(std::string_view outputName)
{
	// Aitum Multistream (multistream.cpp, StartOutput): "aitum_multi_output_" + the output's name.
	if (StartsWith(outputName, "aitum_multi_output_"))
		return Plugin::AitumMultistream;
	// Multiple RTMP outputs (push-widget.cpp, StartStreaming): the literal "multi-output".
	if (outputName == "multi-output")
		return Plugin::MultiRtmp;
	// Aitum Vertical (vertical-canvas.cpp): its stream outputs and services start with "vertical_canvas".
	if (StartsWith(outputName, "vertical_canvas"))
		return Plugin::AitumVertical;
	return Plugin::Other;
}

const char *PluginKey(Plugin plugin)
{
	switch (plugin) {
	case Plugin::AitumMultistream:
		return "aitum";
	case Plugin::MultiRtmp:
		return "multirtmp";
	case Plugin::AitumVertical:
		return "vertical";
	case Plugin::Other:
		break;
	}
	return "other";
}

std::vector<std::string> SummarizeOtherOutputs(const std::vector<OtherOutput> &outputs)
{
	constexpr std::array<Plugin, 4> kOrder = {Plugin::AitumMultistream, Plugin::MultiRtmp, Plugin::AitumVertical,
						  Plugin::Other};
	std::vector<std::string> warnings;
	for (Plugin plugin : kOrder) {
		size_t count = 0;
		for (const OtherOutput &o : outputs)
			if (o.plugin == plugin)
				++count;
		if (count > 0)
			warnings.push_back(std::string("other_") + PluginKey(plugin) + ":" + std::to_string(count));
	}
	return warnings;
}

} // namespace trigglow::outputs
