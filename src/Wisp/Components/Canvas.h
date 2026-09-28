#pragma once
#include "Wisp/Core.h"
#include "Wisp/DrawList.h"

namespace wisp {

// ---- Canvas ----------------------------------------------------------------

struct CanvasSlot
{
	glm::vec2 Position = { 0, 0 };
	glm::vec2 Size = { 0, 0 };
	glm::vec2 Anchor = { 0, 0 }; // 0-1 normalized: 0=left/top, 1=right/bottom
	glm::vec2 Alignment = { 0, 0 }; // 0-1: 0.5=center
};

class Canvas : public Widget
{
public:
	WISP_WIDGET_TYPE(Canvas)

	CanvasSlot& AddChild(std::unique_ptr<Widget> child)
	{
		SetupParentChild(this, child.get());
		m_Children.push_back({ std::move(child), {} });
		InvalidateLayout();
		return m_Children.back().Slot;
	}

	glm::vec2 ComputeDesiredSize() const override
	{
		// Measure children so GetCachedDesiredSize works during arrange
		for (auto& entry : m_Children)
		{
			if (!IsVisibleForLayout(entry.WidgetPtr->GetVisibility())) continue;
			glm::vec2 d = entry.WidgetPtr->ComputeDesiredSize();
			entry.WidgetPtr->SetCachedDesiredSize(d);
		}
		return { 0, 0 };
	}

	void ArrangeChildren(const Geometry& geom, ArrangedChildren& out) const override
	{
		for (auto& entry : m_Children)
		{
			if (!IsVisibleForLayout(entry.WidgetPtr->GetVisibility())) continue;
			glm::vec2 pos = geom.LocalSize * entry.Slot.Anchor + entry.Slot.Position;
			glm::vec2 size = entry.Slot.Size;
			if (size.x <= 0) size.x = entry.WidgetPtr->GetCachedDesiredSize().x;
			if (size.y <= 0) size.y = entry.WidgetPtr->GetCachedDesiredSize().y;
			pos -= size * entry.Slot.Alignment;
			out.Add(entry.WidgetPtr.get(), geom.MakeChild(pos, size));
		}
	}

	int OnPaint(const PaintArgs& args, const Geometry& geom, DrawList& drawList, int layerId, const WidgetStyleContext& style) const override
	{
		glm::vec2 absPos = geom.LocalToAbsolute({0,0});
		PaintArgs childArgs = args;
		childArgs.InheritedClipIndex = ApplyWidgetClipping(this, drawList, args.InheritedClipIndex, absPos, geom.LocalSize);
		ArrangedChildren arranged;
		ArrangeChildren(geom, arranged);
		int nextLayer = layerId;
		WidgetStyleContext cs = style;
		for (auto& c : arranged)
		{
			cs.ColorAndOpacity = style.ColorAndOpacity * c.WidgetPtr->GetRenderTint();
			cs.ColorAndOpacity.a *= c.WidgetPtr->GetRenderOpacity();
			nextLayer = c.WidgetPtr->OnPaint(childArgs, c.Geom, drawList, nextLayer, cs);
		}
		return nextLayer;
	}

	void ForEachChild(std::function<void(Widget*)> fn) override { for (auto& e : m_Children) fn(e.WidgetPtr.get()); }

private:
	struct ChildEntry { std::unique_ptr<Widget> WidgetPtr; CanvasSlot Slot; };
	std::vector<ChildEntry> m_Children;
};


} // namespace wisp
