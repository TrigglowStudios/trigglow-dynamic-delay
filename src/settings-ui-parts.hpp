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

// Sizes and small widget helpers the dock's two source files share
// (settings-ui.cpp, settings-ui-stream.cpp). Internal to the dock.

#include <QFrame>
#include <QLabel>
#include <QSize>
#include <QString>

#include "i18n.hpp"

namespace trigglow::dockparts {

// Icon sizes (logical px), from the proposal's component spec.
constexpr int kStatusIconSize = 18;
constexpr int kAccountIconSize = 16;
constexpr int kInlineIconSize = 14;
constexpr int kButtonIconSize = 15;
// Icon box of a button: the glyph plus its gap to the text.
constexpr int kButtonIconGap = 7;
inline const QSize kButtonIconBox(kButtonIconSize + kButtonIconGap, kButtonIconSize);
inline const QSize kLinkIconBox(kInlineIconSize + 5, kInlineIconSize);
inline const QSize kSectionIconBox(kInlineIconSize + 6, kInlineIconSize);
// Rows side by side reflow to a column below these dock widths.
constexpr int kTinyWidth = 250;
constexpr int kNarrowWidth = 300;

// Str() (i18n.hpp) returns const char*/UTF-8 -- every user-facing string of
// the dock goes through this tiny wrapper instead of a bare
// QString::fromUtf8(Str(...)) at each call site.
inline QString T(const char *key)
{
	return QString::fromUtf8(Str(key));
}

inline QLabel *MakeIconLabel(QWidget *parent, int size)
{
	auto *label = new QLabel(parent);
	label->setFixedSize(size, size);
	label->setAlignment(Qt::AlignCenter);
	return label;
}

inline QFrame *MakeDivider(QWidget *parent)
{
	auto *divider = new QFrame(parent);
	divider->setProperty("tgDivider", QStringLiteral("true"));
	divider->setFixedHeight(1);
	return divider;
}

} // namespace trigglow::dockparts
