#pragma once
#include "Wisp/Core.h"
#include "Wisp/DrawList.h"
#include "Wisp/FontCache.h"

namespace wisp {

// ---- TextBlock -------------------------------------------------------------

class TextBlock : public Widget
{
public:
	WISP_WIDGET_TYPE(TextBlock)

	TextBlock(FontCache* fontCache, FontHandle font, float fontSize, const std::string& text, const glm::vec4& colour = { 1,1,1,1 })
		: m_FontCache(fontCache), m_Font(font), m_FontSize(fontSize), m_Text(text), m_Colour(colour) { SetVisibility(Visibility::SelfHitTestInvisible); }

	void SetText(const std::string& t)
	{
		if (m_Text == t)
			return;
		m_Text = t;
		// A content-sized block (an auto slot in a row) cached its OLD
		// text's width as the wrap width - measured against it, a longer
		// new text would wrap there for good ("of 2.0 ms budget" on three
		// lines). Re-measure unwrapped; a real width limit re-applies at
		// the next paint.
		if (m_WrapFromContent)
			m_CachedWrapWidth = -1.0f;
		InvalidateLayout();
	}
	TextBlock& Text(const std::string& t) { SetText(t); return *this; }
	void SetColour(const glm::vec4& c) { m_Colour = c; }

	void SetWrapMode(WrapMode m) { m_WrapMode = m; InvalidateLayout(); }
	void SetWrapWidth(float w) { m_WrapWidth = w; InvalidateLayout(); }
	void SetAutoWrap(bool b) { m_AutoWrap = b; InvalidateLayout(); }

	glm::vec2 ComputeDesiredSize() const override
	{
		// A font reload invalidates the cached measurement (the
		// glyph metrics changed under us).
		if (m_FontCache && m_MeasuredGeneration != m_FontCache->GetLayoutGeneration())
		{
			m_MeasuredGeneration = m_FontCache->GetLayoutGeneration();
			m_CachedWrapWidth = -1.0f;
		}

		if (!m_FontCache) return { 0, 0 };
		float wrapW = -1.0f;
		if (m_WrapMode != WrapMode::None)
			wrapW = m_AutoWrap ? m_CachedWrapWidth : m_WrapWidth;
		if (m_AutoWrap && m_CachedWrapWidth <= 0)
			wrapW = -1.0f;
		const glm::vec2 measured = m_FontCache->MeasureText(m_Font, m_Text, m_FontSize, wrapW, m_WrapMode);
		m_MeasuredWidth = measured.x;
		return measured;
	}

	void ArrangeChildren(const Geometry&, ArrangedChildren&) const override {}

	int OnPaint(const PaintArgs& args, const Geometry& geom, DrawList& drawList, int layerId, const WidgetStyleContext& style) const override
	{
		if (!m_FontCache || m_Text.empty()) return layerId;

		FontMetrics metrics = m_FontCache->GetMetrics(m_Font, m_FontSize);
		glm::vec4 tint = style.ColorAndOpacity * m_Colour;
		tint.a *= GetRenderOpacity();

		float penX = std::floor(geom.LocalToAbsolute({ 0, 0 }).x);
		float penY = std::round(geom.LocalToAbsolute({ 0, 0 }).y);
		float absRight = penX + geom.LocalSize.x - m_PaddingRight; // account for padding
		float effectiveWrap = m_WrapWidth;
		if (m_AutoWrap && effectiveWrap <= 0)
			effectiveWrap = geom.LocalSize.x;
		m_CachedWrapWidth = (effectiveWrap > 0) ? effectiveWrap : -1.0f;
		m_WrapFromContent = m_AutoWrap && m_WrapWidth <= 0 && geom.LocalSize.x <= m_MeasuredWidth + 0.5f;

		// The single-line ellipsis - the text truncates with
		// "..." against the widget's ARRANGED width (the fixed label/
		// description columns) instead of overprinting the next column.
		std::string displayText = m_Text;
		if (m_WrapMode == WrapMode::None)
		{
			const float limit = geom.LocalSize.x - m_PaddingRight;
			if (limit > 0.0f && m_FontCache->MeasureText(m_Font, m_Text, m_FontSize).x > limit)
			{
				const float ellipsisW = m_FontCache->MeasureText(m_Font, "...", m_FontSize).x;
				while (!displayText.empty()
					&& m_FontCache->MeasureText(m_Font, displayText, m_FontSize).x + ellipsisW > limit)
					displayText.resize(displayText.size() - 1);
				displayText += "...";
			}
		}

		const uint8_t* p = (const uint8_t*)displayText.data();
		const uint8_t* end = p + displayText.size();
		uint32_t prevCp = 0;
		// The wrap decisions run on the UNROUNDED line width, the same
		// arithmetic MeasureText uses - the pen's per-glyph rounding made a
		// label arranged at exactly its measured width overflow by a pixel,
		// wrap its last word, re-measure narrower and ratchet down to one
		// word per line ("Capture / All").
		const float wrapLimit = absRight - penX + 0.5f;
		float lineW = 0.0f;

		while (p < end)
		{
			uint32_t cp;
			if      ((*p & 0x80) == 0)      { cp = *p; p += 1; }
			else if ((*p & 0xE0) == 0xC0 && p+1<end) { cp = ((*p & 0x1F)<<6)|(p[1]&0x3F); p+=2; }
			else if ((*p & 0xF0) == 0xE0 && p+2<end) { cp = ((*p & 0x0F)<<12)|((p[1]&0x3F)<<6)|(p[2]&0x3F); p+=3; }
			else { p++; continue; }

			if (cp == '\n') { penX = std::floor(geom.LocalToAbsolute({0,0}).x); lineW = 0.0f; penY += metrics.LineHeight; prevCp = 0; continue; }

			const GlyphInfo* gi = m_FontCache->GetGlyph(m_Font, cp, m_FontSize);
			if (!gi) continue;

			float advance = gi->AdvanceX;
			if (prevCp)
				advance += m_FontCache->GetKerning(m_Font, prevCp, cp, m_FontSize);

			// A Word-mode overflow mid-word means the word alone is wider than
			// the line (the space look-ahead below moves every word that fits
			// to the next line): break it here, like MeasureText does. The old
			// rewind to the last space re-drew the word on the next line over
			// the glyphs already painted on this one ("(scenes: / (scenes:").
			bool overflow = (effectiveWrap > 0.0f && lineW + advance > wrapLimit + 0.01f && lineW > 0.0f);
			if (overflow && m_WrapMode != WrapMode::None)
			{
				penX = std::floor(geom.LocalToAbsolute({0,0}).x);
				lineW = 0.0f;
				penY += metrics.LineHeight;
				prevCp = 0;
			}

			AddGlyph(drawList, layerId, { penX, std::round(penY + metrics.Ascent) }, *gi, tint, args.InheritedClipIndex);

			// Word wrap: at each space, move the next word down if it will not fit
			if (cp == ' ')
			{
				// Look ahead: if next word won't fit, break now
				if (m_WrapMode == WrapMode::Word)
				{
					float wordWidth = 0;
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
						const GlyphInfo* lgi = m_FontCache->GetGlyph(m_Font, lcp, m_FontSize);
						float la = lgi ? lgi->AdvanceX : 0;
						la += m_FontCache->GetKerning(m_Font, lookPrev, lcp, m_FontSize);
						wordWidth += la;
						lookPrev = lcp;
					}
					if (lineW + advance + wordWidth > wrapLimit && lineW > 0.0f)
					{
						penX = std::floor(geom.LocalToAbsolute({0,0}).x);
						lineW = 0.0f;
						penY += metrics.LineHeight;
						prevCp = 0;
						continue; // skip drawing the space, start fresh on new line
					}
				}
			}

			// The unrounded advance, as MeasureText sums it (AddGlyph snaps
			// each quad) - a rounded pen painted a monospace run ~0.4 px per
			// glyph wider than it measured, so the next text in a row
			// overprinted its tail.
			penX += advance;
			lineW += advance;
			prevCp = cp;
		}
		// If text wrapped beyond measured height, invalidate for next-frame reflow
		if (m_AutoWrap && m_WrapMode != WrapMode::None && m_FontCache)
		{
			float paintedH = penY - std::round(geom.LocalToAbsolute({0,0}).y);
			float measuredH = GetCachedDesiredSize().y;
			if (paintedH > measuredH + 1.0f)
				InvalidateLayout();
		}
		return layerId;
	}

private:
	FontCache* m_FontCache;
	FontHandle m_Font;
	float      m_FontSize;
	std::string m_Text;
	glm::vec4  m_Colour = { 1,1,1,1 };
	WrapMode        m_WrapMode = WrapMode::Word;
	bool            m_AutoWrap = true;
	float           m_WrapWidth = -1.0f;
	mutable float   m_CachedWrapWidth = -1.0f;
	mutable float   m_MeasuredWidth = 0.0f;
	mutable bool    m_WrapFromContent = false;
	mutable uint64_t m_MeasuredGeneration = 0;
	float           m_PaddingRight = 0.0f;
};


} // namespace wisp
