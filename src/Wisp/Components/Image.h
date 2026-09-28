#pragma once
#include "Wisp/Core.h"
#include "Wisp/DrawList.h"

namespace wisp {

// ---- Image ------------------------------------------------------------------

class Image : public Widget
{
public:
	WISP_WIDGET_TYPE(Image)

	Image(const Brush& brush) : m_Brush(brush) {}

	glm::vec2 ComputeDesiredSize() const override
	{
		if (m_OverrideSize.x > 0) return m_OverrideSize;
		return { 64.0f, 64.0f }; // default, caller should set size
	}

	void ArrangeChildren(const Geometry&, ArrangedChildren&) const override {}

	int OnPaint(const PaintArgs& args, const Geometry& geom, DrawList& drawList, int layerId, const WidgetStyleContext& style) const override
	{
		glm::vec2 absPos = geom.LocalToAbsolute({ 0, 0 });
		Brush b = m_Brush;
		b.Tint *= style.ColorAndOpacity;
		drawList.AddImage(layerId, absPos, geom.LocalSize, b, args.InheritedClipIndex);
		return layerId;
	}

	void SetOverrideSize(const glm::vec2& s) { m_OverrideSize = s; InvalidateLayout(); }

private:
	Brush     m_Brush;
	glm::vec2 m_OverrideSize = { 0, 0 };
};


} // namespace wisp
