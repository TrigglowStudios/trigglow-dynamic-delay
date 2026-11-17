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

#include "overlay-badge.hpp"
#include "logging.hpp"
#include "overlay-layout.hpp"
#include "ui-theme.hpp"

#include <algorithm>

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QFont>
#include <QFontDatabase>
#include <QFontMetricsF>
#include <QPainter>
#include <QSaveFile>
#include <QThread>

namespace trigglow::ui {

namespace {

constexpr const char *kComponent = "overlay-badge";

// Brand tokens fixed for the overlay, independent of the OBS theme: the
// Trigglow web palette since its 2026-10 rework (apps/web/src/theme-tokens.css).
const QColor kInk(0x16, 0x13, 0x18);    // --color-pulse-ink
const QColor kMist(0xef, 0xe9, 0xe1);   // --color-pulse-mist
const QColor kViolet(0xa1, 0x8a, 0xff); // --color-pulse-cyan, the accent (7.2:1 on the plate)

QColor WithAlpha(QColor color, double alpha)
{
	color.setAlphaF(static_cast<float>(alpha));
	return color;
}

// The bundled family when there is one; the system UI font otherwise. Weights
// are the files' own (Figtree 600 / 700), so Qt never synthesizes a bolder
// face from another weight.
QFont BadgeFont(const QString &family, int weight, qreal pixelSize)
{
	QFont font;
	if (!family.isEmpty()) {
		font.setFamilies({family});
	} else {
		font = QFontDatabase::systemFont(QFontDatabase::GeneralFont);
	}
	font.setWeight(static_cast<QFont::Weight>(weight));
	font.setPixelSize(std::max(1, qRound(pixelSize)));
	// Drawn under a 2.5x transform: outlines, not hinted 1x glyphs.
	font.setHintingPreference(QFont::PreferNoHinting);
	font.setStyleStrategy(QFont::PreferAntialias);
	return font;
}

// Any space drawn as a no-break space (U+00A0), which Figtree and every
// fallback UI font have: Figtree has neither the narrow no-break space
// (U+202F) nor the thin space, so a translation using one would make Qt
// borrow that glyph from another font.
QString WithNoBreakSpaces(QString text)
{
	for (QChar &c : text) {
		if (c.isSpace())
			c = QChar(0x00A0);
	}
	return text;
}

} // namespace

QImage RenderDelayBadge(const QString &label, const QString &value, const QString &signature)
{
	using namespace overlaylayout;
	const int width = qRound(kDesignWidth * kRenderScale);
	const int height = qRound(kDesignHeight * kRenderScale);
	QImage image(width, height, QImage::Format_ARGB32_Premultiplied);
	image.fill(Qt::transparent);

	QPainter p(&image);
	p.setRenderHint(QPainter::Antialiasing);
	p.setRenderHint(QPainter::TextAntialiasing);
	p.setRenderHint(QPainter::SmoothPixmapTransform);
	// Everything below is in design units (the 1x, 200x72 image, all plate).
	p.scale(kRenderScale, kRenderScale);
	constexpr qreal kW = kDesignWidth;
	constexpr qreal kH = kPlateHeight;
	constexpr qreal kRadius = 12.0; // The web's cards, scaled to the badge.

	// 1 px dark ring: separates the plate from bright footage (snow, sky, menus).
	p.setPen(QPen(WithAlpha(Qt::black, 0.55), 1.0));
	p.setBrush(Qt::NoBrush);
	p.drawRoundedRect(QRectF(0.5, 0.5, kW - 1.0, kH - 1.0), kRadius, kRadius);

	// Plate: the web's ink at 92 % keeps the mist text above 14:1 on any
	// footage. One flat surface, no gradient (the web dropped its brand wash).
	p.setPen(Qt::NoPen);
	p.setBrush(WithAlpha(kInk, 0.92));
	p.drawRoundedRect(QRectF(1.0, 1.0, kW - 2.0, kH - 2.0), kRadius - 0.5, kRadius - 0.5);

	// Inner hairline (--color-pulse-line: mist at 10 %).
	p.setPen(QPen(WithAlpha(kMist, 0.10), 1.0));
	p.setBrush(Qt::NoBrush);
	p.drawRoundedRect(QRectF(1.5, 1.5, kW - 3.0, kH - 3.0), kRadius - 1.0, kRadius - 1.0);

	// Dynamic Delay mark, vertically centred.
	constexpr qreal kMarkSize = 40.0;
	constexpr qreal kMarkLeft = 10.0;
	const QImage mark(QStringLiteral(":/trigglow/overlay/dynamic-delay-mark.png"));
	if (!mark.isNull())
		p.drawImage(QRectF(kMarkLeft, (kH - kMarkSize) / 2.0, kMarkSize, kMarkSize), mark);

	// Text column right of the mark: "DELAY" in violet and the seconds in
	// mist on the first line, the signature under them. Each line shrinks on
	// its own if a fallback font runs wider than Figtree.
	constexpr qreal kTextLeft = 58.0;
	constexpr qreal kTextRight = kW - 12.0;
	constexpr qreal kValueBaseline = 37.0;
	constexpr qreal kSignatureBaseline = 55.0;
	constexpr qreal kGap = 7.0;
	const BrandFonts &fonts = Fonts();
	const QString labelFamily = fonts.display;
	const QString valueFamily = fonts.displayBold.isEmpty() ? fonts.display : fonts.displayBold;
	const int valueWeight = fonts.displayBold.isEmpty() && !fonts.display.isEmpty() ? 600 : 700;
	const QString labelText = WithNoBreakSpaces(label.toUpper());
	const QString valueText = WithNoBreakSpaces(value);

	// No label (the badge without the seconds): the value starts the line.
	const qreal gap = labelText.isEmpty() ? 0.0 : kGap;
	qreal factor = 1.0;
	QFont labelFont;
	QFont valueFont;
	qreal labelWidth = 0.0;
	for (int attempt = 0; attempt < 12; ++attempt) {
		labelFont = BadgeFont(labelFamily, 600, 14.0 * factor);
		labelFont.setLetterSpacing(QFont::AbsoluteSpacing, 1.2 * factor);
		valueFont = BadgeFont(valueFamily, valueWeight, 26.0 * factor);
#if QT_VERSION >= QT_VERSION_CHECK(6, 7, 0)
		valueFont.setFeature(QFont::Tag("tnum"), 1); // Tabular figures: 5 s and 60 s line up.
#endif
		labelWidth = QFontMetricsF(labelFont).horizontalAdvance(labelText);
		const qreal valueWidth = QFontMetricsF(valueFont).horizontalAdvance(valueText);
		if (kTextLeft + labelWidth + gap + valueWidth <= kTextRight)
			break;
		factor *= 0.95;
	}
	p.setFont(labelFont);
	p.setPen(kViolet);
	p.drawText(QPointF(kTextLeft, kValueBaseline), labelText);
	p.setFont(valueFont);
	p.setPen(kMist);
	p.drawText(QPointF(kTextLeft + labelWidth + gap, kValueBaseline), valueText);

	// "Dynamic Delay by Trigglow": caption-sized, mist at 70 % (8.9:1 on the
	// plate), drawn as given -- it's a name, never uppercased.
	const QString signatureText = WithNoBreakSpaces(signature.trimmed());
	if (!signatureText.isEmpty()) {
		QFont font;
		qreal shrink = 1.0;
		for (int attempt = 0; attempt < 12; ++attempt) {
			font = BadgeFont(labelFamily, 600, kSignaturePixelSize * shrink);
			if (kTextLeft + QFontMetricsF(font).horizontalAdvance(signatureText) <= kTextRight)
				break;
			shrink *= 0.95;
		}
		p.setFont(font);
		p.setPen(WithAlpha(kMist, 0.70));
		p.drawText(QPointF(kTextLeft, kSignatureBaseline), signatureText);
	}
	p.end();
	return image;
}

DelayBadgeFiles::DelayBadgeFiles(QString directory) : directory_(std::move(directory)) {}

QString DelayBadgeFiles::PathFor(uint32_t seconds, const QString &label, const QString &value, const QString &signature)
{
	seconds = overlaylayout::ClampSeconds(seconds);
	return PathForFile(seconds, QString::fromStdString(overlaylayout::BadgeFileName(seconds)), label, value,
			   signature);
}

QString DelayBadgeFiles::BrandPath(const QString &name, const QString &byline)
{
	return PathForFile(0, QString::fromStdString(overlaylayout::BadgeBrandFileName()), QString(), name, byline);
}

QString DelayBadgeFiles::PathForFile(uint32_t seconds, const QString &fileName, const QString &label,
				     const QString &value, const QString &signature)
{
	if (directory_.isEmpty())
		return {};
	// The name carries the render version, so a PNG an older build drew
	// differently is never picked up, not even off the UI thread below.
	const QString path = QDir(directory_).filePath(fileName);
	// In memory only: every value is redrawn the first time it is asked for
	// in a session, before the badge is shown, so a PNG left on disk with
	// other texts or fonts (OBS language switched since) is never what OBS
	// displays.
	const QString key = QStringLiteral("%1\x1f%2\x1f%3\x1f%4\x1f%5\x1f%6")
				    .arg(label, value, signature, Fonts().display, Fonts().displayBold, Fonts().mono);

	if (renderedKeys_.value(seconds) == key && QFileInfo::exists(path))
		return path;

	// QPainter text on a QImage stays on the UI thread (see the header).
	if (QCoreApplication::instance() && QThread::currentThread() != QCoreApplication::instance()->thread()) {
		TRIGGLOW_LOG_WARN(kComponent, "badge for %us requested off the UI thread; not rendering it", seconds);
		return QFileInfo::exists(path) ? path : QString();
	}

	QDir().mkpath(directory_);
	if (!staleFilesRemoved_) {
		staleFilesRemoved_ = true;
		RemoveStaleFiles();
	}
	const QImage image = RenderDelayBadge(label, value, signature);
	// QSaveFile: OBS never reads a half-written PNG.
	QSaveFile file(path);
	const bool ok = file.open(QIODevice::WriteOnly) && image.save(&file, "PNG") && file.commit();
	if (!ok) {
		if (!warned_.value(seconds)) {
			warned_.insert(seconds, true);
			// File name only: the full path carries the Windows user name.
			TRIGGLOW_LOG_WARN(kComponent, "could not write the delay badge %s: %s",
					  QFileInfo(path).fileName().toUtf8().constData(),
					  file.errorString().toUtf8().constData());
		}
		return QFileInfo::exists(path) ? path : QString();
	}
	renderedKeys_.insert(seconds, key);
	return path;
}

void DelayBadgeFiles::RemoveStaleFiles()
{
	// Right before the first badge of the session is written, i.e. before OBS
	// is pointed at a current one: until then the image source may still name
	// an old file, which it only shows while the badge is visible.
	const QDir dir(directory_);
	int removed = 0;
	for (const QString &name : dir.entryList({QStringLiteral("delay-badge-*.png")}, QDir::Files)) {
		if (overlaylayout::IsStaleBadgeFile(name.toStdString()) && QFile::remove(dir.filePath(name)))
			++removed;
	}
	if (removed > 0)
		TRIGGLOW_LOG_INFO(kComponent, "removed %d delay badge(s) drawn by an older version", removed);
}

} // namespace trigglow::ui
