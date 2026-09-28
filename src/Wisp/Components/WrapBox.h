#pragma once

#include "Wisp/Core.h"
#include "Wisp/DrawList.h"
#include <memory>
#include <vector>

namespace wisp {

class WrapBox : public Widget
{
public:
	WISP_WIDGET_TYPE(WrapBox)

	struct WrapSlot
	{
		std::unique_ptr<Widget> Child;
		Slot SlotProps;
	};

	WrapSlot& AddSlot(std::unique_ptr<Widget> w)
	{
		m_Slots.push_back({std::move(w), Slot()});
		SetupParentChild(this, m_Slots.back().Child.get());
		InvalidateLayout();
		return m_Slots.back();
	}

	WrapSlot& AddChild(std::unique_ptr<Widget> w) { return AddSlot(std::move(w)); }

	glm::vec2 ComputeDesiredSize() const override
	{
		float maxW = 0, maxH = 0;
		for (auto& s : m_Slots)
		{
			if (!IsVisibleForLayout(s.Child->GetVisibility())) continue;
			glm::vec2 d = s.Child->ComputeDesiredSize();
			s.Child->SetCachedDesiredSize(d);
			maxW = std::max(maxW, d.x);
			maxH = std::max(maxH, d.y);
		}
		// Wrap width unknown at measure: report a single-row size; the
		// one-frame settle adjusts height after arrange
		float totalW = 0;
		for (auto& s : m_Slots)
			if (IsVisibleForLayout(s.Child->GetVisibility()))
				totalW += s.Child->GetCachedDesiredSize().x + s.SlotProps.PaddingSize().x;
		return { std::min(totalW, 300.0f), maxH + 8.0f };
	}

	void ArrangeChildren(const Geometry& geom, ArrangedChildren& out) const override
	{
		float availW = geom.LocalSize.x;
		float penX = 0, penY = 0, rowH = 0;

		for (auto& s : m_Slots)
		{
			if (!IsVisibleForLayout(s.Child->GetVisibility())) continue;
			glm::vec2 desired = s.Child->GetCachedDesiredSize();
			glm::vec2 pad = s.SlotProps.PaddingSize();
			float cellW = desired.x + pad.x;
			float cellH = desired.y + pad.y;

			if (penX > 0 && penX + cellW > availW)
			{
				penX = 0;
				penY += rowH;
				rowH = 0;
			}

			glm::vec2 slotPos = { penX + s.SlotProps.PaddingLeft, penY + s.SlotProps.PaddingTop };
			glm::vec2 slotSize = { cellW - pad.x, cellH - pad.y };
			glm::vec2 childPos, childSize;
			ResolveAlignment(slotPos, slotSize, desired, s.SlotProps.Align.H, s.SlotProps.Align.V, childPos, childSize);
			out.Add(s.Child.get(), geom.MakeChild(childPos, childSize));

			penX += cellW;
			rowH = std::max(rowH, cellH);
		}
	}

	void ForEachChild(std::function<void(Widget*)> fn) override
	{
		for (auto& s : m_Slots)
			if (s.Child) fn(s.Child.get());
	}

	int OnPaint(const PaintArgs& args, const Geometry& geom, DrawList& drawList, int layerId, const WidgetStyleContext& style) const override
	{
		glm::vec2 absPos = geom.LocalToAbsolute({0,0});
		PaintArgs childArgs = args;
		childArgs.InheritedClipIndex = ApplyWidgetClipping(this, drawList, args.InheritedClipIndex, absPos, geom.LocalSize);
		ArrangedChildren arranged;
		ArrangeChildren(geom, arranged);
		int nextLayer = layerId;
		WidgetStyleContext childStyle = style;
		for (auto& child : arranged)
		{
			childStyle.ColorAndOpacity = style.ColorAndOpacity * child.WidgetPtr->GetRenderTint();
			childStyle.ColorAndOpacity.a *= child.WidgetPtr->GetRenderOpacity();
			nextLayer = child.WidgetPtr->OnPaint(childArgs, child.Geom, drawList, nextLayer, childStyle);
		}
		return nextLayer;
	}

private:
	std::vector<WrapSlot> m_Slots;
};

using SWrapBox = WrapBox;

} // namespace wisp
