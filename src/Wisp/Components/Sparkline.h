#pragma once

#include "Wisp/Core.h"
#include "Wisp/DrawList.h"
#include "Wisp/Components/GraphData.h"
#include <algorithm>
#include <memory>
#include <vector>

namespace wisp {

class Sparkline : public Widget
{
public:
	WISP_WIDGET_TYPE(Sparkline)

	void SetSeries(GraphSeries* s) { m_Series = s; }
	void SetColor(const glm::vec4& c) { m_Color = c; }
	void SetWindow(uint32_t n) { m_Window = n; }

	glm::vec2 ComputeDesiredSize() const override { return { 100, 24 }; }
	void ArrangeChildren(const Geometry&, ArrangedChildren&) const override {}

	int OnPaint(const PaintArgs& args, const Geometry& geom, DrawList& drawList, int layerId, const WidgetStyleContext&) const override
	{
		glm::vec2 pos = geom.LocalToAbsolute({0,0});
		float w = geom.LocalSize.x, h = geom.LocalSize.y;

		if (!m_Series || m_Series->Count() < 2) return layerId;

		uint32_t total = m_Series->Count();
		uint32_t window = m_Window > 0 ? std::min(m_Window, total) : total;
		uint32_t start = total - window;

		float minV = 0, maxV = 1;
		bool first = true;
		for (uint32_t i = 0; i < window; i++)
		{
			float v = m_Series->Sample(start + i);
			if (first) { minV = v; maxV = v; first = false; }
			else { minV = std::min(minV, v); maxV = std::max(maxV, v); }
		}
		if (first) return layerId;
		if (maxV - minV < 0.001f) { minV -= 1; maxV += 1; }
		float range = maxV - minV;

		std::vector<glm::vec2> pts;
		for (uint32_t i = 0; i < window; i++)
		{
			float v = m_Series->Sample(start + i);
			float x = pos.x + ((float)i / (float)(window - 1)) * w;
			float y = pos.y + h - ((v - minV) / range) * h;
			pts.push_back({x, y});
		}
		drawList.AddPolyline(layerId, pts, 1.2f, m_Color, args.InheritedClipIndex);
		return layerId + 1;
	}

private:
	GraphSeries* m_Series = nullptr;
	glm::vec4 m_Color{0.3f,0.7f,0.9f,1};
	uint32_t m_Window = 100;
};

using SSparkline = Sparkline;

} // namespace wisp
