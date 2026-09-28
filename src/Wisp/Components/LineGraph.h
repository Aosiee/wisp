#pragma once

#include "Wisp/Core.h"
#include "Wisp/DrawList.h"
#include "Wisp/FontCache.h"
#include "Wisp/Style.h"
#include "Wisp/Components/GraphData.h"
#include <algorithm>
#include <cmath>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace wisp {

struct GraphStyle
{
	WISP_STYLE_TYPE(GraphStyle)
	glm::vec4 Background{0.04f,0.04f,0.08f,1};
	glm::vec4 GridLine{0.1f,0.1f,0.15f,1};
	glm::vec4 AxisLabel{0.5f,0.5f,0.6f,1};
	float GridLineThickness = 1.0f;
	std::vector<glm::vec4> SeriesPalette;

	glm::vec4 BudgetLineGood{0.15f,0.7f,0.25f,1};
	glm::vec4 BudgetLineWarn{0.9f,0.65f,0.15f,1};
	glm::vec4 BudgetLineBad{0.9f,0.15f,0.15f,1};

	glm::vec4 HighWaterline{0.9f,0.3f,0.3f,1};
	glm::vec4 LowWaterline{0.3f,0.3f,0.9f,1};

	glm::vec4 SpikeMarker{1.0f,0.2f,0.2f,1};
	glm::vec4 LegendBg{0.06f,0.06f,0.1f,0.85f};

	bool operator==(const GraphStyle&) const = default;
};

inline GraphStyle GetDefaultGraphStyle()
{
	GraphStyle s;
	s.SeriesPalette = {
		{0.3f,0.7f,0.9f,1},
		{0.9f,0.5f,0.3f,1},
		{0.5f,0.9f,0.4f,1},
		{0.9f,0.8f,0.2f,1},
		{0.7f,0.4f,0.9f,1},
		{0.3f,0.8f,0.7f,1},
	};
	return s;
}

struct BudgetLine
{
	float Y;
	glm::vec4 Color;
	std::string Label;
};

struct Waterline
{
	float Y = 0;
	glm::vec4 Color{1,1,1,1};
	bool Enabled = false;
};

struct SpikeTag
{
	uint32_t SampleIndex;
	std::string Label;
	glm::vec4 Color{1,0.2f,0.2f,1};
};

class LineGraph : public Widget
{
public:
	WISP_WIDGET_TYPE(LineGraph)

	LineGraph(FontCache* fc, FontHandle font, float fontSize)
		: m_FontCache(fc), m_Font(font), m_FontSize(fontSize)
	{
		m_Style = GetDefaultGraphStyle();
	}

	void SetData(std::shared_ptr<GraphData> data) { m_Data = std::move(data); }
	void ShowSeries(const std::vector<std::string>& names) { m_SeriesNames = names; }
	void SetVisibleWindow(uint32_t n) { m_VisibleWindow = n; }
	void SetSeriesLineStyle(size_t idx, LineStyle s)
	{
		if (idx >= m_SeriesLineStyles.size()) m_SeriesLineStyles.resize(idx + 1, LineStyle::Solid);
		m_SeriesLineStyles[idx] = s;
	}
	void SetShowSeriesLabels(bool show) { m_ShowSeriesLabels = show; }

	void AddBudgetLine(float y, const glm::vec4& color, const std::string& label = "")
	{
		m_BudgetLines.push_back({y, color, label});
	}

	void SetHighWaterline(float y, const glm::vec4& color)
	{
		m_HighWater.Enabled = true;
		m_HighWater.Y = y;
		m_HighWater.Color = color;
	}

	void SetLowWaterline(float y, const glm::vec4& color)
	{
		m_LowWater.Enabled = true;
		m_LowWater.Y = y;
		m_LowWater.Color = color;
	}

	void SetHighWaterEnabled(bool e) { m_HighWater.Enabled = e; }
	void SetLowWaterEnabled(bool e)  { m_LowWater.Enabled = e; }

	void AddSpikeTag(uint32_t sampleIdx, const std::string& label, const glm::vec4& color = {1,0.2f,0.2f,1})
	{
		m_SpikeTags.push_back({sampleIdx, label, color});
	}

	glm::vec2 ComputeDesiredSize() const override { return { 400, 180 }; }
	void ArrangeChildren(const Geometry&, ArrangedChildren&) const override {}

	int OnPaint(const PaintArgs& args, const Geometry& geom, DrawList& drawList, int layerId, const WidgetStyleContext&) const override
	{
		glm::vec2 pos = geom.LocalToAbsolute({0,0});
		float w = geom.LocalSize.x, h = geom.LocalSize.y;
		int clip = args.InheritedClipIndex;
		float marginLeft = 40, marginRight = 10, marginTop = 10, marginBottom = 20;
		float gx = pos.x + marginLeft, gy = pos.y + marginTop;
		float gw = w - marginLeft - marginRight, gh = h - marginTop - marginBottom;

		// Background
		drawList.AddBox(layerId, pos, {w,h}, m_Style.Background, clip);

		if (!m_Data || m_SeriesNames.empty()) return layerId + 1;

		// Collect visible samples
		uint32_t totalSamples = 0;
		for (auto& name : m_SeriesNames)
		{
			auto* s = m_Data->Find(name);
			if (s) totalSamples = std::max(totalSamples, s->Count());
		}
		if (totalSamples == 0) return layerId + 1;
		uint32_t window = m_VisibleWindow > 0 ? std::min(m_VisibleWindow, totalSamples) : totalSamples;
		uint32_t startIdx = totalSamples - window;

		// Compute actual min/max across all visible series
		float actualMin = 0, actualMax = 1;
		bool first = true;
		for (auto& name : m_SeriesNames)
		{
			auto* s = m_Data->Find(name);
			if (!s) continue;
			for (uint32_t i = 0; i < window; i++)
			{
				float v = s->Sample(startIdx + i);
				if (first) { actualMin = v; actualMax = v; first = false; }
				else { actualMin = std::min(actualMin, v); actualMax = std::max(actualMax, v); }
			}
		}
		if (first) return layerId + 1;
		if (actualMax - actualMin < 0.001f) { actualMin -= 1; actualMax += 1; }

		// Budget/waterlines extend the range
		for (auto& bl : m_BudgetLines) { actualMin = std::min(actualMin, bl.Y); actualMax = std::max(actualMax, bl.Y); }
		if (m_HighWater.Enabled) { actualMin = std::min(actualMin, m_HighWater.Y); actualMax = std::max(actualMax, m_HighWater.Y); }
		if (m_LowWater.Enabled)  { actualMin = std::min(actualMin, m_LowWater.Y); actualMax = std::max(actualMax, m_LowWater.Y); }

		// Damped lerp toward actual bounds
		float damp = 0.35f;
		m_DispMin = m_DispMin + (actualMin - m_DispMin) * damp;
		m_DispMax = m_DispMax + (actualMax - m_DispMax) * damp;

		// Snap to nice bounds
		float range = m_DispMax - m_DispMin;
		float niceStep = std::pow(10.0f, std::floor(std::log10(range * 0.5f)));
		if (range / niceStep < 2.5f) niceStep *= 0.5f;
		m_DispMin = std::floor(m_DispMin / niceStep) * niceStep;
		m_DispMax = std::ceil(m_DispMax / niceStep) * niceStep;
		if (m_DispMax - m_DispMin < 0.001f) { m_DispMin -= niceStep; m_DispMax += niceStep; }
		float dispRange = m_DispMax - m_DispMin;

		auto toX = [&](uint32_t sampleIdx) -> float {
			float t = (float)(sampleIdx - startIdx) / (float)(window - 1 > 0 ? window - 1 : 1);
			return gx + t * gw;
		};
		auto toY = [&](float val) -> float {
			float t = (val - m_DispMin) / dispRange;
			return gy + gh - t * gh;
		};

		// Grid lines
		float gridStep = niceStep;
		for (float v = m_DispMin; v <= m_DispMax + gridStep*0.5f; v += gridStep)
		{
			float y = toY(v);
			drawList.AddLine(layerId+1, {gx,y}, {gx+gw,y}, m_Style.GridLineThickness, m_Style.GridLine, clip);
		}

		// Waterlines
		if (m_HighWater.Enabled)
		{
			float wy = toY(m_HighWater.Y);
			drawList.AddLine(layerId+2, {gx,wy}, {gx+gw,wy}, 1.5f, m_HighWater.Color, clip, LineStyle::Dashed);
		}
		if (m_LowWater.Enabled)
		{
			float wy = toY(m_LowWater.Y);
			drawList.AddLine(layerId+2, {gx,wy}, {gx+gw,wy}, 1.5f, m_LowWater.Color, clip, LineStyle::Dashed);
		}

		// Budget lines
		for (auto& bl : m_BudgetLines)
		{
			float by = toY(bl.Y);
			drawList.AddLine(layerId+3, {gx,by}, {gx+gw,by}, 1.5f, bl.Color, clip);
			if (m_FontCache && !bl.Label.empty())
			{
				auto ms = m_FontCache->MeasureText(m_Font, bl.Label, m_FontSize * 0.55f);
				float lx = gx + gw - ms.x - 4;
				float ly = by - ms.y - 1;
				glm::vec4 labelCol = bl.Color; labelCol.a = 0.85f;
				PaintText(drawList, layerId+3, {lx, ly}, bl.Label, labelCol, clip, m_FontSize * 0.55f);
			}
		}

		// Series polylines
		for (size_t si = 0; si < m_SeriesNames.size(); si++)
		{
			auto* s = m_Data->Find(m_SeriesNames[si]);
			if (!s) continue;
			glm::vec4 col = si < m_Style.SeriesPalette.size() ? m_Style.SeriesPalette[si] : glm::vec4(0.7f);
			LineStyle ls = si < m_SeriesLineStyles.size() ? m_SeriesLineStyles[si] : LineStyle::Solid;
			std::vector<glm::vec2> pts;
			for (uint32_t i = 0; i < window; i++)
			{
				float v = s->Sample(startIdx + i);
				pts.push_back({toX(startIdx + i), toY(v)});
			}
			if (pts.size() >= 2)
				drawList.AddPolyline(layerId+4, pts, 1.8f, col, clip, ls);

			// Series label at far right edge, translucent on top of line
			if (m_ShowSeriesLabels && m_FontCache && !pts.empty())
			{
				glm::vec2 lastPt = pts.back();
				auto ms = m_FontCache->MeasureText(m_Font, m_SeriesNames[si], m_FontSize * 0.55f);
				float lx = lastPt.x - ms.x - 4;
				float ly = lastPt.y - ms.y - 2;
				// Clamp to graph bounds
				if (lx < gx + 2) lx = gx + 2;
				if (ly < gy) ly = gy;
				if (ly + ms.y > gy + gh) ly = gy + gh - ms.y;
				// Small background box behind text for readability
				drawList.AddBox(layerId+4, {lx - 2, ly - 1}, {ms.x + 4, ms.y + 2},
					{0,0,0,0.45f}, clip);
				glm::vec4 labelCol = col; labelCol.a = 1.0f;
				PaintText(drawList, layerId+4, {lx, ly}, m_SeriesNames[si], labelCol, clip, m_FontSize * 0.55f);
			}
		}

		// Spike tags
		for (auto& sp : m_SpikeTags)
		{
			if (sp.SampleIndex < startIdx || sp.SampleIndex >= startIdx + window) continue;
			auto* s0 = m_SeriesNames.empty() ? nullptr : m_Data->Find(m_SeriesNames[0]);
			if (!s0) continue;
			float val = s0->Sample(sp.SampleIndex);
			float sx = toX(sp.SampleIndex), sy = toY(val);
			// Vertical line from data point upward
			drawList.AddLine(layerId+5, {sx, sy}, {sx, sy - 12}, 1.5f, sp.Color, clip);
			// Small diamond marker
			drawList.AddBox(layerId+5, {sx - 3, sy - 3}, {6, 6}, sp.Color, clip);
			// Label
			if (m_FontCache && !sp.Label.empty())
			{
				auto ms = m_FontCache->MeasureText(m_Font, sp.Label, m_FontSize * 0.7f);
				float lx = sx - ms.x * 0.5f, ly = sy - 16 - ms.y;
				PaintText(drawList, layerId+5, {lx, ly}, sp.Label, sp.Color, clip, m_FontSize * 0.7f);
			}
		}

		// Axis labels
		if (m_FontCache)
		{
			for (float v = m_DispMin; v <= m_DispMax + gridStep*0.5f; v += gridStep)
			{
				char buf[32];
				if (std::abs(v) < 0.01f) snprintf(buf, sizeof(buf), "0");
				else if (std::abs(v) >= 100) snprintf(buf, sizeof(buf), "%.0f", v);
				else if (std::abs(v) >= 10) snprintf(buf, sizeof(buf), "%.0f", v);
				else snprintf(buf, sizeof(buf), "%.1f", v);
				auto ms = m_FontCache->MeasureText(m_Font, buf, m_FontSize * 0.6f);
				float ay = toY(v) - ms.y * 0.5f;
				PaintText(drawList, layerId+6, {gx - ms.x - 4, ay}, buf, m_Style.AxisLabel, clip, m_FontSize * 0.6f);
			}
		}

		// Legend
		float legX = gx + gw - 5, legY = gy + 5;
		for (size_t si = 0; si < m_SeriesNames.size(); si++)
		{
			glm::vec4 col = si < m_Style.SeriesPalette.size() ? m_Style.SeriesPalette[si] : glm::vec4(0.7f);
			drawList.AddBox(layerId+7, {legX, legY}, {10, 10}, col, clip);
			if (m_FontCache)
			{
				auto* s = m_Data->Find(m_SeriesNames[si]);
				char buf[64];
				float latest = s ? s->Latest() : 0;
				snprintf(buf, sizeof(buf), "%s %.1f", m_SeriesNames[si].c_str(), latest);
				PaintText(drawList, layerId+7, {legX + 14, legY - 1}, buf, {0.8f,0.8f,0.85f,1}, clip, m_FontSize);
			}
			legY += 16;
		}

		return layerId + 8;
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
	mutable GraphStyle m_Style;
	std::shared_ptr<GraphData> m_Data;
	std::vector<std::string> m_SeriesNames;
	std::vector<LineStyle>   m_SeriesLineStyles;
	bool m_ShowSeriesLabels = true;
	uint32_t m_VisibleWindow = 300;
	std::vector<BudgetLine> m_BudgetLines;
	mutable Waterline m_HighWater;
	mutable Waterline m_LowWater;
	std::vector<SpikeTag> m_SpikeTags;
	mutable float m_DispMin = 0, m_DispMax = 100;
};

using SLineGraph = LineGraph;

} // namespace wisp
