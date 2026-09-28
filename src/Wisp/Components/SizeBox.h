#pragma once
#include "Wisp/Core.h"
#include "Wisp/DrawList.h"
#include <algorithm>

namespace wisp {

class SizeBox : public Widget
{
public:
	WISP_WIDGET_TYPE(SizeBox)

	// Overrides — explicitly set the desired size
	SizeBox& SetWidthOverride(float w)   { m_WidthOverride = w; InvalidateLayout(); return *this; }
	SizeBox& SetHeightOverride(float h)  { m_HeightOverride = h; InvalidateLayout(); return *this; }
	void ClearWidthOverride()            { m_WidthOverride = -1; InvalidateLayout(); }
	void ClearHeightOverride()           { m_HeightOverride = -1; InvalidateLayout(); }

	// Min/max constraints — clamp desired size
	SizeBox& SetMinWidth(float w)        { m_MinWidth = w; InvalidateLayout(); return *this; }
	SizeBox& SetMinHeight(float h)       { m_MinHeight = h; InvalidateLayout(); return *this; }
	SizeBox& SetMaxWidth(float w)        { m_MaxWidth = w; InvalidateLayout(); return *this; }
	SizeBox& SetMaxHeight(float h)       { m_MaxHeight = h; InvalidateLayout(); return *this; }
	void ClearMinWidth()                 { m_MinWidth = -1; InvalidateLayout(); }
	void ClearMinHeight()                { m_MinHeight = -1; InvalidateLayout(); }
	void ClearMaxWidth()                 { m_MaxWidth = -1; InvalidateLayout(); }
	void ClearMaxHeight()                { m_MaxHeight = -1; InvalidateLayout(); }

	// Aspect ratio — desired height = width / ratio (when width is set)
	SizeBox& SetAspectRatio(float r)     { m_AspectRatio = r; InvalidateLayout(); return *this; }
	void ClearAspectRatio()              { m_AspectRatio = -1; InvalidateLayout(); }

	void SetChild(std::unique_ptr<Widget> child) { m_Child = std::move(child); if (m_Child) SetupParentChild(this, m_Child.get()); }

	glm::vec2 ComputeDesiredSize() const override
	{
		glm::vec2 d = m_Child ? m_Child->ComputeDesiredSize() : glm::vec2(0);
		if (m_Child) m_Child->SetCachedDesiredSize(d);

		SizeMode sm = GetSizeMode();
		if (m_WidthOverride  > 0 && sm != SizeMode::AutoWidth  && sm != SizeMode::AutoBoth) d.x = m_WidthOverride;
		if (m_HeightOverride > 0 && sm != SizeMode::AutoHeight && sm != SizeMode::AutoBoth) d.y = m_HeightOverride;

		// Aspect ratio: if width is constrained and height is auto, derive height
		if (m_AspectRatio > 0 && d.x > 0 && (sm == SizeMode::AutoHeight || sm == SizeMode::AutoBoth || m_HeightOverride <= 0))
			d.y = d.x / m_AspectRatio;

		// Clamp to min/max
		if (m_MinWidth  > 0) d.x = std::max(d.x, m_MinWidth);
		if (m_MinHeight > 0) d.y = std::max(d.y, m_MinHeight);
		if (m_MaxWidth  > 0) d.x = std::min(d.x, m_MaxWidth);
		if (m_MaxHeight > 0) d.y = std::min(d.y, m_MaxHeight);

		return d;
	}

	void ArrangeChildren(const Geometry& geom, ArrangedChildren& out) const override
	{
		if (!m_Child) return;
		glm::vec2 childSize = geom.LocalSize;
		SizeMode sm = GetSizeMode();
		if (m_WidthOverride  > 0 && sm != SizeMode::AutoWidth  && sm != SizeMode::AutoBoth) childSize.x = m_WidthOverride;
		if (m_HeightOverride > 0 && sm != SizeMode::AutoHeight && sm != SizeMode::AutoBoth) childSize.y = m_HeightOverride;

		// Clamp child geometry to min/max
		if (m_MinWidth  > 0) childSize.x = std::max(childSize.x, m_MinWidth);
		if (m_MinHeight > 0) childSize.y = std::max(childSize.y, m_MinHeight);
		if (m_MaxWidth  > 0) childSize.x = std::min(childSize.x, m_MaxWidth);
		if (m_MaxHeight > 0) childSize.y = std::min(childSize.y, m_MaxHeight);

		// Aspect ratio on arranged size
		if (m_AspectRatio > 0 && childSize.x > 0 && (sm == SizeMode::AutoHeight || sm == SizeMode::AutoBoth))
			childSize.y = childSize.x / m_AspectRatio;

		if (sm == SizeMode::AutoHeight || sm == SizeMode::AutoBoth)
			childSize.y = std::max(childSize.y, m_Child->GetCachedDesiredSize().y);
		if (sm == SizeMode::AutoWidth || sm == SizeMode::AutoBoth)
			childSize.x = std::max(childSize.x, m_Child->GetCachedDesiredSize().x);

		out.Add(m_Child.get(), geom.MakeChild({ 0, 0 }, childSize));
	}

	int OnPaint(const PaintArgs& args, const Geometry& geom, DrawList& drawList, int layerId, const WidgetStyleContext& style) const override
	{
		if (!m_Child) return layerId;
		glm::vec2 absPos = geom.LocalToAbsolute({0,0});
		glm::vec2 clipSize = geom.LocalSize;
		SizeMode sm = GetSizeMode();
		if (m_WidthOverride  > 0 && sm != SizeMode::AutoWidth  && sm != SizeMode::AutoBoth) clipSize.x = m_WidthOverride;
		if (m_HeightOverride > 0 && sm != SizeMode::AutoHeight && sm != SizeMode::AutoBoth) clipSize.y = m_HeightOverride;
		PaintArgs childArgs = args;
		childArgs.InheritedClipIndex = ApplyWidgetClipping(this, drawList, args.InheritedClipIndex, absPos, clipSize);
		ArrangedChildren arranged;
		ArrangeChildren(geom, arranged);
		WidgetStyleContext cs = style;
		for (auto& c : arranged)
		{
			cs.ColorAndOpacity = style.ColorAndOpacity * c.WidgetPtr->GetRenderTint();
			cs.ColorAndOpacity.a *= c.WidgetPtr->GetRenderOpacity();
			return c.WidgetPtr->OnPaint(childArgs, c.Geom, drawList, layerId, cs);
		}
		return layerId;
	}

	void ForEachChild(std::function<void(Widget*)> fn) override { if (m_Child) fn(m_Child.get()); }

private:
	float m_WidthOverride  = -1;
	float m_HeightOverride = -1;
	float m_MinWidth  = -1;
	float m_MinHeight = -1;
	float m_MaxWidth  = -1;
	float m_MaxHeight = -1;
	float m_AspectRatio = -1;
	std::unique_ptr<Widget> m_Child;
};

} // namespace wisp
