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

#include <cstdint>
#include <initializer_list>
#include <string>

// Color tokens of the plugin's UI (dock and report dialog), derived from the
// active OBS theme's palette. Pure C++ (no Qt, no libobs) so the contrast
// guarantees can be unit-tested: tests/ui_tokens_tests.cpp.
//
// Hybrid theming: surfaces (window, cards, fields) follow OBS's palette so the
// dock sits naturally in Yami, light themes and the system high-contrast mode;
// the Trigglow brand comes in through the fixed accents below (the web app's
// color tokens) and through square shapes, fonts and icons
// (ui-theme.hpp). Every text token is pushed until it reaches WCAG AA (4.5:1)
// against the backgrounds it is drawn on, and every component border
// (fields, secondary buttons, focus ring) reaches 3:1, whatever the theme.
namespace trigglow::uitokens {

struct Rgb {
	uint8_t r = 0;
	uint8_t g = 0;
	uint8_t b = 0;

	bool operator==(const Rgb &other) const { return r == other.r && g == other.g && b == other.b; }
	bool operator!=(const Rgb &other) const { return !(*this == other); }
};

// "#rrggbb" or "rrggbb"; anything else parses as black.
Rgb FromHex(const char *hex);
// Lowercase "#rrggbb".
std::string Hex(Rgb color);

// a * t + b * (1 - t), per channel in sRGB, rounded (t clamped to [0, 1]).
Rgb Mix(Rgb a, Rgb b, double t);
// WCAG 2.x relative luminance and contrast ratio (1..21).
double RelativeLuminance(Rgb color);
double ContrastRatio(Rgb a, Rgb b);

// `color` itself if it already reaches `minRatio` against every background;
// otherwise `color` mixed step by step toward black or white (whichever
// contrasts more with the backgrounds) until it does, or the pole itself.
Rgb EnsureContrast(Rgb color, std::initializer_list<Rgb> backgrounds, double minRatio);

// Brand constants: the Trigglow web palette since its 2026-10 rework
// (apps/web/src/theme-tokens.css). The web's token names are historical
// ("pulse-cyan" is now the violet accent); these use what the colors are.
// No gradient any more: the primary button is one solid violet.
namespace brand {
constexpr Rgb kInk{0x16, 0x13, 0x18};          // --color-pulse-ink
constexpr Rgb kMist{0xef, 0xe9, 0xe1};         // --color-pulse-mist
constexpr Rgb kViolet{0xa1, 0x8a, 0xff};       // --color-pulse-cyan: the accent (text, icons, focus)
constexpr Rgb kVioletStrong{0x74, 0x52, 0xee}; // --color-pulse-accent-strong: filled buttons, white text 5.0:1
constexpr Rgb kVioletDeep{0x5b, 0x3f, 0xd6};   // Accent text and focus on light themes (AA on white)
constexpr Rgb kCoral{0xff, 0x8a, 0x5c};        // --color-pulse-magenta: the second accent
constexpr Rgb kAmber{0xf4, 0xb5, 0x45};        // --color-pulse-amber
constexpr Rgb kDanger{0xf2, 0x64, 0x5f};       // --color-pulse-danger
constexpr Rgb kOnAccent{0xff, 0xff, 0xff};     // --color-primary-foreground: text on the filled button
} // namespace brand

enum class Scheme {
	Dark,
	Light,
	HighContrast,
};

// The QPalette roles the tokens are derived from (QPalette::Active group).
struct PaletteInput {
	Rgb window;
	Rgb base;
	Rgb text;
	Rgb highlight;
	Rgb highlightedText;
	// The OS reports a high-contrast mode (Windows: SPI_GETHIGHCONTRAST).
	bool systemHighContrast = false;
};

struct Tokens {
	Scheme scheme = Scheme::Dark;

	Rgb surface;     // Dock background (the theme's window color).
	Rgb surfaceCard; // Cards (the theme's base color).
	Rgb field;       // Combo boxes, spin boxes, line edits: a step darker than the card.
	Rgb text;
	Rgb textMuted;   // Labels, details, hints.
	Rgb placeholder; // Placeholder text in fields (QSS placeholder-text-color): AA on the field.
	Rgb line;        // Decorative hairlines (card borders, dividers, progress track).
	Rgb lineStrong;  // Field and secondary-button borders (WCAG 1.4.11: 3:1).

	Rgb accent;       // Cyan fill.
	Rgb accentText;   // Cyan text and icons (darkened on light themes).
	Rgb accentTint;   // Chip and secondary-hover background; the user's messages to Trigi.
	Rgb accentBorder; // Border of the user's messages and of Trigi's avatar (decorative).
	Rgb codeTint;     // `code` in Trigi's replies, on the reply bubble (the field color).

	Rgb warning; // Amber fill (rules, progress chunk on dark themes).
	Rgb warningText;
	Rgb warningTint;   // Update banner and warning chip background.
	Rgb progressChunk; // Filling progress bar.

	Rgb danger; // Red fill (Error rule).
	Rgb dangerText;
	Rgb dangerTint;       // Disable button and error banner background.
	Rgb dangerBorder;     // Disable button border.
	Rgb dangerButtonText; // Disable button text.

	Rgb focus;        // Focus ring of fields, secondary buttons, links.
	Rgb focusPrimary; // Focus ring of the gradient primary button.
	Rgb disabledFg;   // Disabled text (opaque: QSS has no opacity).

	// Primary button, checked check box and the Active rule. On high
	// contrast the gradient collapses into the system highlight.
	bool useGradient = true;
	Rgb gradientStart;
	Rgb gradientEnd;
	Rgb gradientHoverStart;
	Rgb gradientHoverEnd;
	Rgb gradientPressedStart;
	Rgb gradientPressedEnd;
	Rgb onGradient; // Text and icons on the primary button.
};

// Dark when white text reads better than black on the window color; high
// contrast when the OS says so, or (other systems) when window, base and text
// are near pure black/white extremes.
Scheme DetectScheme(const PaletteInput &palette);
Tokens Compute(const PaletteInput &palette);

} // namespace trigglow::uitokens
