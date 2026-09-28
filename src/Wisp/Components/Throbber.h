#pragma once

#include "Wisp/Core.h"
#include "Wisp/DrawList.h"
#include "Wisp/Application.h"
#include <cmath>

namespace wisp {

class Throbber : public Widget
{
public:
	WISP_WIDGET_TYPE(Throbber)

	Throbber& SetDotCount(int n) { m_DotCount = n > 0 ? n : 1; InvalidateLayout(); return *this; }
	Throbber& SetDotRadius(float r) { m_DotRadius = r; InvalidateLayout(); return *this; }
	Throbber& SetSpeed(float s) { m_Speed = s; return *this; }
	Throbber& SetColour(const glm::vec4& c) { m_Colour = c; return *this; }

	glm::vec2 ComputeDesiredSize() const override
	{
		float w = m_DotCount * m_DotRadius * 2.0f + (m_DotCount - 1) * m_DotRadius;
		return { w, m_DotRadius * 2.0f };
	}

	void ArrangeChildren(const Geometry&, ArrangedChildren&) const override {}

	int OnPaint(const PaintArgs& args, const Geometry& geom, DrawList& drawList, int layerId, const WidgetStyleContext&) const override
	{
		glm::vec2 pos = geom.LocalToAbsolute({0,0});
		float w = geom.LocalSize.x, h = geom.LocalSize.y;
		float cy = pos.y + h * 0.5f;
		float spacing = (w - m_DotRadius * 2.0f) / (float)(m_DotCount - 1 > 0 ? m_DotCount - 1 : 1);

		float t = UIApplication::GetAnimTime() * m_Speed;
		for (int i = 0; i < m_DotCount; i++)
		{
			float phase = std::fmod(t + i * 0.2f, 1.0f);
			// Pulse: rise and fall over the phase cycle
			float alpha = phase < 0.5f ? phase * 2.0f : (1.0f - phase) * 2.0f;
			float r = m_DotRadius * (0.5f + 0.5f * alpha);
			glm::vec4 col = m_Colour;
			col.a *= 0.3f + 0.7f * alpha;
			float cx = pos.x + m_DotRadius + i * spacing;
			drawList.AddBox(layerId, {cx - r, cy - r}, {r * 2, r * 2}, col, args.InheritedClipIndex);
		}
		return layerId + 1;
	}

private:
	int m_DotCount = 5;
	float m_DotRadius = 4.0f;
	float m_Speed = 2.0f;
	glm::vec4 m_Colour{0.3f,0.7f,0.9f,1};
};

using SThrobber = Throbber;

} // namespace wisp
