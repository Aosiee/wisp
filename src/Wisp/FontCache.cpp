#define STB_TRUETYPE_IMPLEMENTATION
#define STBTT_STATIC
#include <stb_truetype.h>

#include "Wisp/FontCache.h"
#include "TinyEngine/Core/Log.h"
#include <algorithm>
#include <cstring>

namespace wisp {

FontCache::FontCache()
{
	m_AtlasData.resize(m_AtlasWidth * m_AtlasHeight * 4, 0);
}

FontCache::~FontCache() = default;

FontHandle FontCache::AddFont(const uint8_t* ttfData, size_t ttfSize)
{
	FontFace face;
	face.TtfData.assign(ttfData, ttfData + ttfSize);
	face.StbFontOffset = 0;

	// Validate the font
	stbtt_fontinfo info;
	if (!stbtt_InitFont(&info, face.TtfData.data(), stbtt_GetFontOffsetForIndex(face.TtfData.data(), 0)))
		return InvalidFont;

	FontHandle handle = (FontHandle)m_Faces.size();
	m_Faces.push_back(std::move(face));
	return handle;
}

FontCache::FontFace* FontCache::GetFace(FontHandle handle)
{
	if (handle >= m_Faces.size()) return nullptr;
	return &m_Faces[handle];
}

const GlyphInfo* FontCache::GetGlyph(FontHandle font, uint32_t codepoint, float pixelSize)
{
	FontFace* face = GetFace(font);
	if (!face || pixelSize <= 0.0f) return nullptr;

	GlyphKey key{ font, codepoint, pixelSize };
	auto it = m_GlyphCache.find(key);
	if (it != m_GlyphCache.end())
		return &it->second;

	GlyphInfo info;
	if (!RasterizeGlyph(font, codepoint, pixelSize, info))
		return nullptr;

	auto result = m_GlyphCache.emplace(key, std::move(info));
	return &result.first->second;
}

bool FontCache::RasterizeGlyph(FontHandle font, uint32_t codepoint, float pixelSize, GlyphInfo& outInfo)
{
	FontFace* face = GetFace(font);
	if (!face) return false;

	stbtt_fontinfo stbInfo;
	if (!stbtt_InitFont(&stbInfo, face->TtfData.data(), stbtt_GetFontOffsetForIndex(face->TtfData.data(), face->StbFontOffset)))
		return false;

	// The size is per-call: every widget rasterizes at the size IT draws
	// at, independent of whatever any other widget last measured.
	float scale = stbtt_ScaleForPixelHeight(&stbInfo, pixelSize);

	int advanceWidth, leftSideBearing;
	stbtt_GetCodepointHMetrics(&stbInfo, codepoint, &advanceWidth, &leftSideBearing);

	// Subpixel-prefilter for crisp rendering at integer positions
	int x0, y0, x1, y1;
	stbtt_GetCodepointBitmapBoxSubpixel(&stbInfo, codepoint, scale, scale, 0.0f, 0.0f, &x0, &y0, &x1, &y1);

	int bw = x1 - x0;
	int bh = y1 - y0;

	outInfo.Codepoint = codepoint;
	outInfo.AdvanceX = advanceWidth * scale;
	outInfo.BearingX = (float)leftSideBearing * scale;
	outInfo.BearingY = (float)y0;
	outInfo.Width  = (float)bw;
	outInfo.Height = (float)bh;

	// Pack into atlas
	if (bw > 0 && bh > 0)
	{
		uint32_t atlasX, atlasY;
		if (!PackRect((uint32_t)bw, (uint32_t)bh, atlasX, atlasY))
		{
			GrowAtlas();
			if (!PackRect((uint32_t)bw, (uint32_t)bh, atlasX, atlasY))
				return false;
		}

		// Rasterize with subpixel positioning for crisp edges
		std::vector<uint8_t> bitmap(bw * bh);
		stbtt_MakeCodepointBitmapSubpixel(&stbInfo, bitmap.data(), bw, bh, bw, scale, scale,
			0.0f, 0.0f, codepoint);

		// Blit into atlas
		for (int row = 0; row < bh; ++row)
		{
			for (int col = 0; col < bw; ++col)
			{
				uint32_t dstIdx = ((atlasY + row) * m_AtlasWidth + (atlasX + col)) * 4;
				m_AtlasData[dstIdx + 0] = 255;
				m_AtlasData[dstIdx + 1] = 255;
				m_AtlasData[dstIdx + 2] = 255;
				m_AtlasData[dstIdx + 3] = bitmap[row * bw + col];
			}
		}
		m_AtlasDirty = true;

		outInfo.AtlasU0 = (float)atlasX / (float)m_AtlasWidth;
		outInfo.AtlasV0 = (float)atlasY / (float)m_AtlasHeight;
		outInfo.AtlasU1 = (float)(atlasX + bw) / (float)m_AtlasWidth;
		outInfo.AtlasV1 = (float)(atlasY + bh) / (float)m_AtlasHeight;
	}
	else
	{
		// Space glyphs — zero area in atlas
		outInfo.AtlasU0 = outInfo.AtlasV0 = 0.0f;
		outInfo.AtlasU1 = outInfo.AtlasV1 = 0.0f;
	}

	return true;
}

bool FontCache::PackRect(uint32_t w, uint32_t h, uint32_t& outX, uint32_t& outY)
{
	const uint32_t padding = 1;

	// Try to fit in current shelf
	if (m_ShelfX + w + padding * 2 <= m_AtlasWidth)
	{
		outX = m_ShelfX + padding;
		outY = m_ShelfY + padding;
		m_ShelfX += w + padding * 2;
		m_ShelfMaxHeight = std::max(m_ShelfMaxHeight, h + padding * 2);
		return true;
	}

	// Start a new shelf
	uint32_t nextShelfY = m_ShelfY + m_ShelfMaxHeight + padding;
	if (nextShelfY + h + padding * 2 > m_AtlasHeight)
		return false; // atlas is full

	m_ShelfY = nextShelfY;
	m_ShelfX = 0;
	m_ShelfMaxHeight = 0;

	outX = padding;
	outY = m_ShelfY + padding;
	m_ShelfX = w + padding * 2;
	m_ShelfMaxHeight = h + padding * 2;
	return true;
}

void FontCache::GrowAtlas()
{
	uint32_t oldW = m_AtlasWidth;
	uint32_t oldH = m_AtlasHeight;

	// Double the axis that makes it more square
	if (m_AtlasWidth <= m_AtlasHeight)
		m_AtlasWidth = std::min(m_AtlasWidth * 2, 4096u);
	else
		m_AtlasHeight = std::min(m_AtlasHeight * 2, 4096u);

	if (m_AtlasWidth == oldW && m_AtlasHeight == oldH)
		return; // maxed out

	std::vector<uint8_t> newData(m_AtlasWidth * m_AtlasHeight * 4, 0);
	for (uint32_t row = 0; row < oldH; ++row)
	{
		std::memcpy(newData.data() + row * m_AtlasWidth * 4,
			m_AtlasData.data() + row * oldW * 4, oldW * 4);
	}
	m_AtlasData = std::move(newData);

	// Reset shelf state — invalidated glyph cache entries need re-baking
	m_ShelfY = 0;
	m_ShelfMaxHeight = 0;
	m_ShelfX = 0;
	m_AtlasDirty = true;

	// Clear glyph cache — all atlas UVs are invalid after resize
	m_GlyphCache.clear();
}

float FontCache::GetKerning(FontHandle font, uint32_t left, uint32_t right, float pixelSize)
{
	FontFace* face = GetFace(font);
	if (!face) return 0.0f;

	stbtt_fontinfo stbInfo;
	if (!stbtt_InitFont(&stbInfo, face->TtfData.data(), stbtt_GetFontOffsetForIndex(face->TtfData.data(), face->StbFontOffset)))
		return 0.0f;

	return stbtt_GetCodepointKernAdvance(&stbInfo, left, right) * stbtt_ScaleForPixelHeight(&stbInfo, pixelSize);
}

glm::vec2 FontCache::MeasureText(FontHandle font, const std::string& text, float pixelSize, float wrapWidth, WrapMode wrapMode)
{
	FontFace* face = GetFace(font);
	if (!face) return { 0.0f, 0.0f };

	stbtt_fontinfo stbInfo;
	if (!stbtt_InitFont(&stbInfo, face->TtfData.data(), stbtt_GetFontOffsetForIndex(face->TtfData.data(), face->StbFontOffset)))
		return { 0.0f, 0.0f };

	float scale = stbtt_ScaleForPixelHeight(&stbInfo, pixelSize);

	int ascent, descent, lineGap;
	stbtt_GetFontVMetrics(&stbInfo, &ascent, &descent, &lineGap);
	float lineHeight = (ascent - descent + lineGap) * scale;

	float lineWidth = 0.0f, maxLineWidth = 0.0f;
	int numLines = 1;

	const uint8_t* p = (const uint8_t*)text.data();
	const uint8_t* end = p + text.size();
	uint32_t prevCodepoint = 0;

	while (p < end)
	{
		uint32_t cp;
		if ((*p & 0x80) == 0)      { cp = *p; p += 1; }
		else if ((*p & 0xE0) == 0xC0 && p + 1 < end) { cp = ((*p & 0x1F) << 6) | (p[1] & 0x3F); p += 2; }
		else if ((*p & 0xF0) == 0xE0 && p + 2 < end) { cp = ((*p & 0x0F) << 12) | ((p[1] & 0x3F) << 6) | (p[2] & 0x3F); p += 3; }
		else { p++; continue; }

		if (cp == '\n')
		{
			maxLineWidth = std::max(maxLineWidth, lineWidth);
			lineWidth = 0.0f;
			numLines++;
			prevCodepoint = 0;
			continue;
		}

		int advanceWidth, lsb;
		stbtt_GetCodepointHMetrics(&stbInfo, cp, &advanceWidth, &lsb);
		float charWidth = advanceWidth * scale;

		// Kerning
		if (prevCodepoint)
			charWidth += stbtt_GetCodepointKernAdvance(&stbInfo, prevCodepoint, cp) * scale;

		if (cp == ' ' && wrapMode == WrapMode::Word && wrapWidth > 0.0f)
		{
			// Look ahead: measure next word width
			float wordW = 0.0f;
			const uint8_t* look = p;
			uint32_t lookPrev = cp;
			while (look < end)
			{
				uint32_t lcp;
				if      ((*look & 0x80) == 0)      { lcp = *look; look += 1; }
				else if ((*look & 0xE0) == 0xC0 && look+1<end) { lcp = ((*look & 0x1F)<<6)|(look[1]&0x3F); look+=2; }
				else if ((*look & 0xF0) == 0xE0 && look+2<end) { lcp = ((*look & 0x0F)<<12)|((look[1]&0x3F)<<6)|(look[2]&0x3F); look+=3; }
				else break;
				if (lcp == ' ' || lcp == '\n') break;
				int a, l;
				stbtt_GetCodepointHMetrics(&stbInfo, lcp, &a, &l);
				float cw = a * scale;
				if (lookPrev != ' ')
					cw += stbtt_GetCodepointKernAdvance(&stbInfo, lookPrev, lcp) * scale;
				wordW += std::round(cw);
				lookPrev = lcp;
			}
			float spaceW = charWidth;
			if (prevCodepoint)
				spaceW += stbtt_GetCodepointKernAdvance(&stbInfo, prevCodepoint, cp) * scale;
			if (lineWidth + spaceW + wordW > wrapWidth && lineWidth > 0.0f)
			{
				maxLineWidth = std::max(maxLineWidth, lineWidth);
				lineWidth = 0.0f;
				numLines++;
				prevCodepoint = 0;
				continue; // skip drawing the space, start new line
			}
		}

		if (wrapWidth > 0.0f && lineWidth + charWidth > wrapWidth && lineWidth > 0.0f)
		{
			maxLineWidth = std::max(maxLineWidth, lineWidth);
			lineWidth = charWidth;
			numLines++;
		}
		else
		{
			lineWidth += charWidth;
		}
		prevCodepoint = cp;
	}

	maxLineWidth = std::max(maxLineWidth, lineWidth);

	return { maxLineWidth, lineHeight * (float)numLines };
}

FontMetrics FontCache::GetMetrics(FontHandle font, float pixelSize)
{
	FontMetrics m = {};
	FontFace* face = GetFace(font);
	if (!face) return m;

	stbtt_fontinfo stbInfo;
	if (!stbtt_InitFont(&stbInfo, face->TtfData.data(), stbtt_GetFontOffsetForIndex(face->TtfData.data(), face->StbFontOffset)))
		return m;

	int ascent, descent, lineGap;
	stbtt_GetFontVMetrics(&stbInfo, &ascent, &descent, &lineGap);
	float scale = stbtt_ScaleForPixelHeight(&stbInfo, pixelSize);

	m.Ascent  = ascent * scale;
	m.Descent = descent * scale;
	m.LineGap = lineGap * scale;
	m.LineHeight = m.Ascent - m.Descent + m.LineGap;
	return m;
}

} // namespace wisp
