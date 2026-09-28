#pragma once
#include "Wisp/Core.h"
#include "Wisp/DrawList.h"

namespace wisp {

// ---- ScaleBox (DPI scaling) ------------------------------------------------

class ScaleBox : public Widget
{
public:
	WISP_WIDGET_TYPE(ScaleBox)

	void SetScale(float s) { m_Scale = s; InvalidateLayout(); }
	void SetChild(std::unique_ptr<Widget> child) { m_Child = std::move(child); if (m_Child) SetupParentChild(this, m_Child.get()); }

	glm::vec2 ComputeDesiredSize() const override
	{
		if (!m_Child) return { 0, 0 };
		glm::vec2 d = m_Child->ComputeDesiredSize();
		m_Child->SetCachedDesiredSize(d);
		return d / m_Scale;
	}

	void ArrangeChildren(const Geometry& geom, ArrangedChildren& out) const override
	{
		if (!m_Child) return;
		Geometry scaled = geom;
		scaled.AccumulatedTransform[0][0] *= m_Scale;
		scaled.AccumulatedTransform[1][1] *= m_Scale;
		out.Add(m_Child.get(), scaled);
	}

	int OnPaint(const PaintArgs& args, const Geometry& geom, DrawList& drawList, int layerId, const WidgetStyleContext& style) const override
	{
		if (!m_Child) return layerId;
		glm::vec2 absPos = geom.LocalToAbsolute({0,0});
		PaintArgs childArgs = args;
		childArgs.InheritedClipIndex = ApplyWidgetClipping(this, drawList, args.InheritedClipIndex, absPos, geom.LocalSize);
		ArrangedChildren arranged;
		ArrangeChildren(geom, arranged);
		for (auto& c : arranged)
		{
			WidgetStyleContext cs;
			cs.ColorAndOpacity = style.ColorAndOpacity * c.WidgetPtr->GetRenderTint();
			cs.ColorAndOpacity.a *= c.WidgetPtr->GetRenderOpacity();
			return c.WidgetPtr->OnPaint(childArgs, c.Geom, drawList, layerId, cs);
		}
		return layerId;
	}

	void ForEachChild(std::function<void(Widget*)> fn) override { if (m_Child) fn(m_Child.get()); }

private:
	float m_Scale = 1.0f;
	std::unique_ptr<Widget> m_Child;
};


} // namespace wisp
