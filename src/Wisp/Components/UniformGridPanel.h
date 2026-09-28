#pragma once

#include "Wisp/Core.h"
#include "Wisp/DrawList.h"
#include <memory>
#include <vector>

namespace wisp {

class UniformGridPanel : public Widget
{
public:
	WISP_WIDGET_TYPE(UniformGridPanel)

	struct Cell
	{
		int Row = 0, Col = 0;
		std::unique_ptr<Widget> Child;
	};

	Cell& AddChild(int row, int col, std::unique_ptr<Widget> w)
	{
		m_Cells.push_back({row, col, std::move(w)});
		SetupParentChild(this, m_Cells.back().Child.get());
		m_Rows = std::max(m_Rows, row + 1);
		m_Cols = std::max(m_Cols, col + 1);
		InvalidateLayout();
		return m_Cells.back();
	}

	glm::vec2 ComputeDesiredSize() const override
	{
		float maxW = 0, maxH = 0;
		for (auto& c : m_Cells)
		{
			if (!IsVisibleForLayout(c.Child->GetVisibility())) continue;
			glm::vec2 d = c.Child->ComputeDesiredSize();
			c.Child->SetCachedDesiredSize(d);
			maxW = std::max(maxW, d.x);
			maxH = std::max(maxH, d.y);
		}
		return { maxW * (float)m_Cols, maxH * (float)m_Rows };
	}

	void ArrangeChildren(const Geometry& geom, ArrangedChildren& out) const override
	{
		if (m_Cells.empty()) return;
		float cellW = geom.LocalSize.x / (float)m_Cols;
		float cellH = geom.LocalSize.y / (float)m_Rows;
		for (auto& c : m_Cells)
		{
			if (!IsVisibleForLayout(c.Child->GetVisibility())) continue;
			glm::vec2 cellPos = { (float)c.Col * cellW, (float)c.Row * cellH };
			out.Add(c.Child.get(), geom.MakeChild(cellPos, {cellW, cellH}));
		}
	}

	void ForEachChild(std::function<void(Widget*)> fn) override
	{
		for (auto& c : m_Cells)
			if (c.Child) fn(c.Child.get());
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
	std::vector<Cell> m_Cells;
	int m_Rows = 0, m_Cols = 0;
};

using SUniformGridPanel = UniformGridPanel;

} // namespace wisp
