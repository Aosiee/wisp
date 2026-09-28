#pragma once

#include "Wisp/Core.h"
#include "Wisp/DrawList.h"
#include "Wisp/FontCache.h"
#include <algorithm>
#include <cmath>
#include <string>
#include <vector>

namespace wisp {

struct BarEntry
{
	std::string Label;
	float Value;
	glm::vec4 Color{0.3f,0.6f,0.9f,1};
};

class BarChart : public Widget
{
public:
	WISP_WIDGET_TYPE(BarChart)

	BarChart(FontCache* fc, FontHandle font, float fontSize)
		: m_FontCache(fc), m_Font(font), m_FontSize(fontSize) {}

	void SetBars(const std::vector<BarEntry>& bars) { m_Bars = bars; }

	glm::vec2 ComputeDesiredSize() const override { return { 300, 150 }; }
	void ArrangeChildren(const Geometry&, ArrangedChildren&) const override {}

	int OnPaint(const PaintArgs& args, const Geometry& geom, DrawList& drawList, int layerId, const WidgetStyleContext&) const override
	{
		glm::vec2 pos = geom.LocalToAbsolute({0,0});
		float w = geom.LocalSize.x, h = geom.LocalSize.y;
		int clip = args.InheritedClipIndex;
		float margin = 20;
		float gx = pos.x + margin, gy = pos.y + margin;
		float gw = w - margin*2, gh = h - margin*2;

		drawList.AddBox(layerId, pos, {w,h}, {0.04f,0.04f,0.08f,1}, clip);

		if (m_Bars.empty()) return layerId + 1;

		float maxVal = 0;
		for (auto& b : m_Bars) maxVal = std::max(maxVal, b.Value);
		if (maxVal < 0.001f) maxVal = 1;

		float barW = gw / (float)m_Bars.size() * 0.7f;
		float gap = gw / (float)m_Bars.size() * 0.3f;

		for (size_t i = 0; i < m_Bars.size(); i++)
		{
			float bx = gx + (float)i * (gw / (float)m_Bars.size()) + gap * 0.5f;
			float bh = (m_Bars[i].Value / maxVal) * gh;
			float by = gy + gh - bh;

			drawList.AddBox(layerId+1, {bx, by}, {barW, bh}, m_Bars[i].Color, clip);

			if (m_FontCache)
			{
				char buf[32];
				snprintf(buf, sizeof(buf), "%.1f", m_Bars[i].Value);
				auto ms = m_FontCache->MeasureText(m_Font, buf, m_FontSize * 0.6f);
				float lx = bx + barW*0.5f - ms.x*0.5f;
				float ly = by - ms.y - 2;
				PaintText(drawList, layerId+2, {lx, ly}, buf, {0.8f,0.8f,0.85f,1}, clip, m_FontSize * 0.6f);

				// Label below
				auto lm = m_FontCache->MeasureText(m_Font, m_Bars[i].Label, m_FontSize * 0.55f);
				float llx = bx + barW*0.5f - lm.x*0.5f;
				float lly = gy + gh + 4;
				PaintText(drawList, layerId+2, {llx, lly}, m_Bars[i].Label, {0.5f,0.5f,0.6f,1}, clip, m_FontSize * 0.55f);
			}
		}

		return layerId + 3;
	}

private:
	void PaintText(DrawList& dl, int layer, const glm::vec2& pos, const std::string& text,
		const glm::vec4& tint, int clip, float pixelSize) const
	{
		if (!m_FontCache || text.empty()) return;
		float px = pos.x, py = pos.y;
		const uint8_t* p = (const uint8_t*)text.data();
		const uint8_t* e = p + text.size();
		while (p < e)
		{
			uint32_t cp;
			if((*p&0x80)==0){cp=*p;p++;}
			else if((*p&0xE0)==0xC0&&p+1<e){cp=((*p&0x1F)<<6)|(p[1]&0x3F);p+=2;}
			else if((*p&0xF0)==0xE0&&p+2<e){cp=((*p&0x0F)<<12)|((p[1]&0x3F)<<6)|(p[2]&0x3F);p+=3;}
			else{p++;continue;}
			const GlyphInfo* gi = m_FontCache->GetGlyph(m_Font, cp, pixelSize);
			if(gi)
			{
				glm::vec2 uv[4]={{gi->AtlasU0,gi->AtlasV0},{gi->AtlasU1,gi->AtlasV0},{gi->AtlasU1,gi->AtlasV1},{gi->AtlasU0,gi->AtlasV1}};
				dl.AddText(layer, {px, py + gi->BearingY}, std::string(1,(char)cp), uv, gi->AdvanceX, gi->Height, tint, clip);
				px += gi->AdvanceX;
			}
		}
	}

	FontCache* m_FontCache;
	FontHandle m_Font;
	float m_FontSize;
	std::vector<BarEntry> m_Bars;
};

using SBarChart = BarChart;

} // namespace wisp
