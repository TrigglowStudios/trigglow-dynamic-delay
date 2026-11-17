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

#include "apply-to.hpp"

namespace trigglow {

ApplyToSetting ParseApplyTo(ApplyToType type, const std::string &text, bool hasModeEnabled)
{
	ApplyToSetting out;
	out.droppedModeEnabled = hasModeEnabled;
	switch (type) {
	case ApplyToType::Missing:
		// Settings from before the choice, test builds' "mode_enabled"
		// included: all of OBS.
		break;
	case ApplyToType::String:
		if (text == "stream")
			out.value = ApplyTo::Stream;
		else if (text != "all")
			out.warn = true;
		break;
	case ApplyToType::Other:
		out.warn = true;
		break;
	}
	return out;
}

const char *ApplyToName(ApplyTo value)
{
	return value == ApplyTo::Stream ? "stream" : "all";
}

bool ControlsGoToStream(ApplyTo choice, bool streamModeAvailable)
{
	return choice == ApplyTo::Stream && streamModeAvailable;
}

} // namespace trigglow
