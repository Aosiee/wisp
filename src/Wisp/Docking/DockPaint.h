#pragma once

#include "Wisp/Core.h"
#include "Wisp/DrawList.h"
#include "Wisp/FontCache.h"
#include <string>

namespace wisp { namespace dockpaint {

// Shared text helpers for dock widgets (same conventions as WidgetReflector).

inline void PaintText(DrawList& dl, FontCache* fc, FontHandle font, float pixelSize, int layer, const glm::vec2& pos,
	const std::string& text, const glm::vec4& tint, int clip, float scale = 1.0f)
{
	if (!fc || text.empty()) return;
	float px = pos.x, py = pos.y;
	const uint8_t* p = (const uint8_t*)text.data();
	const uint8_t* e = p + text.size();
	while (p < e)
	{
		uint32_t cp;
		if ((*p & 0x80) == 0) { cp = *p; p++; }
		else if ((*p & 0xE0) == 0xC0 && p + 1 < e) { cp = ((*p & 0x1F) << 6) | (p[1] & 0x3F); p += 2; }
		else if ((*p & 0xF0) == 0xE0 && p + 2 < e) { cp = ((*p & 0x0F) << 12) | ((p[1] & 0x3F) << 6) | (p[2] & 0x3F); p += 3; }
		else { p++; continue; }
		const GlyphInfo* gi = fc->GetGlyph(font, cp, pixelSize);
		if (gi)
		{
			glm::vec2 uv[4] = { {gi->AtlasU0, gi->AtlasV0}, {gi->AtlasU1, gi->AtlasV0}, {gi->AtlasU1, gi->AtlasV1}, {gi->AtlasU0, gi->AtlasV1} };
			dl.AddText(layer, { px, py + gi->BearingY * scale }, std::string(1, (char)cp), uv,
				gi->AdvanceX * scale, gi->Height * scale, tint, clip);
			px += gi->AdvanceX * scale;
		}
	}
}

inline void MeasureInkBox(FontCache* fc, FontHandle font, const std::string& text, float pixelSize, float scale,
	float& outTop, float& outBottom)
{
	outTop = 0.0f;
	outBottom = 0.0f;
	if (!fc || text.empty()) return;
	bool first = true;
	const uint8_t* p = (const uint8_t*)text.data();
	const uint8_t* e = p + text.size();
	while (p < e)
	{
		uint32_t cp;
		if ((*p & 0x80) == 0) { cp = *p; p++; }
		else if ((*p & 0xE0) == 0xC0 && p + 1 < e) { cp = ((*p & 0x1F) << 6) | (p[1] & 0x3F); p += 2; }
		else if ((*p & 0xF0) == 0xE0 && p + 2 < e) { cp = ((*p & 0x0F) << 12) | ((p[1] & 0x3F) << 6) | (p[2] & 0x3F); p += 3; }
		else { p++; continue; }
		const GlyphInfo* gi = fc->GetGlyph(font, cp, pixelSize);
		if (!gi) continue;
		float top = gi->BearingY * scale;
		float bottom = (gi->BearingY + gi->Height) * scale;
		if (first) { outTop = top; outBottom = bottom; first = false; }
		else { outTop = std::min(outTop, top); outBottom = std::max(outBottom, bottom); }
	}
}

// Rough width of a string at the given size (advance sum).
inline float MeasureWidth(FontCache* fc, FontHandle font, const std::string& text, float pixelSize, float scale = 1.0f)
{
	if (!fc || text.empty()) return 0.0f;
	float w = 0.0f;
	const uint8_t* p = (const uint8_t*)text.data();
	const uint8_t* e = p + text.size();
	while (p < e)
	{
		uint32_t cp;
		if ((*p & 0x80) == 0) { cp = *p; p++; }
		else if ((*p & 0xE0) == 0xC0 && p + 1 < e) { cp = ((*p & 0x1F) << 6) | (p[1] & 0x3F); p += 2; }
		else if ((*p & 0xF0) == 0xE0 && p + 2 < e) { cp = ((*p & 0x0F) << 12) | ((p[1] & 0x3F) << 6) | (p[2] & 0x3F); p += 3; }
		else { p++; continue; }
		const GlyphInfo* gi = fc->GetGlyph(font, cp, pixelSize);
		if (gi) w += gi->AdvanceX * scale;
	}
	return w;
}

}} // namespace wisp::dockpaint
