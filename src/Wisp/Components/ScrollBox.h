#pragma once
#include "Wisp/Core.h"
#include "Wisp/DrawList.h"
#include <functional>

namespace wisp {

// ---- ScrollBox -------------------------------------------------------------

class ScrollBox : public Widget
{
public:
	WISP_WIDGET_TYPE(ScrollBox)

	void SetChild(std::unique_ptr<Widget> child) { m_Child = std::move(child); if (m_Child) SetupParentChild(this, m_Child.get()); }
	ScrollBox& SetViewportHeight(float h) { m_ForcedHeight = h; InvalidateLayout(); return *this; }



	glm::vec2 ComputeDesiredSize() const override
	{
		if (!m_Child) return { 0, 0 };
		if (!IsVisibleForLayout(m_Child->GetVisibility())) return { 0, 0 };
		glm::vec2 d = m_Child->ComputeDesiredSize();
		m_Child->SetCachedDesiredSize(d);
		if (m_ForcedHeight > 0) return { d.x, m_ForcedHeight };
		return { d.x, 200.0f };
	}

	void ArrangeChildren(const Geometry& geom, ArrangedChildren& out) const override
	{
		if (!m_Child) return;
		if (!IsVisibleForLayout(m_Child->GetVisibility())) return;
		glm::vec2 childSize = m_Child->GetCachedDesiredSize();
		glm::vec2 viewSize = geom.LocalSize;
		if (m_ForcedHeight > 0) viewSize.y = m_ForcedHeight;
		childSize.x = std::max(childSize.x, viewSize.x - m_ScrollBarWidth);
		childSize.y = std::max(childSize.y, viewSize.y);
		glm::vec2 offset = { -m_ScrollOffset.x, -m_ScrollOffset.y };
		out.Add(m_Child.get(), geom.MakeChild(offset, childSize));
	}

	int OnPaint(const PaintArgs& args, const Geometry& geom, DrawList& drawList, int layerId, const WidgetStyleContext& style) const override
	{
		if (!m_Child) return layerId;

		m_CachedViewportWidth  = geom.LocalSize.x;
		m_CachedViewportHeight = m_ForcedHeight > 0 ? m_ForcedHeight : geom.LocalSize.y;

		// Clamp scroll offset
		glm::vec2 contentSize = m_Child->GetCachedDesiredSize();
		float viewH = m_CachedViewportHeight;
		float maxScrollY = std::max(0.0f, contentSize.y - viewH);
		m_ScrollOffset.y = std::max(0.0f, std::min(m_ScrollOffset.y, maxScrollY));
		float maxScrollX = std::max(0.0f, contentSize.x - geom.LocalSize.x + m_ScrollBarWidth);
		m_ScrollOffset.x = std::max(0.0f, std::min(m_ScrollOffset.x, maxScrollX));

		glm::vec2 absPos = geom.LocalToAbsolute({ 0, 0 });
		ApplyWidgetClipping(this, drawList, args.InheritedClipIndex, absPos, geom.LocalSize);
		int clipIdx = drawList.PushClip({ absPos.x, absPos.y, geom.LocalSize.x, viewH }, args.InheritedClipIndex);

		// Paint child with clip index propagated
		ArrangedChildren arranged;
		ArrangeChildren(geom, arranged);
		int nextLayer = layerId;
		WidgetStyleContext cs = style;
		PaintArgs childArgs = args;
		childArgs.InheritedClipIndex = clipIdx;
		for (auto& c : arranged)
		{
			cs.ColorAndOpacity = style.ColorAndOpacity * c.WidgetPtr->GetRenderTint();
			cs.ColorAndOpacity.a *= c.WidgetPtr->GetRenderOpacity();
			nextLayer = c.WidgetPtr->OnPaint(childArgs, c.Geom, drawList, nextLayer, cs);
		}

		// Scrollbar (vertical only for now)
		if (maxScrollY > 0.0f)
		{
			float trackH = viewH;
			float thumbH = (viewH / contentSize.y) * trackH;
			thumbH = std::max(thumbH, 16.0f);
			float thumbY = (m_ScrollOffset.y / maxScrollY) * (trackH - thumbH);
			float barX = absPos.x + geom.LocalSize.x - m_ScrollBarWidth;
		m_ScrollBarAbsX = barX;
		m_ScrollBarAbsY = absPos.y;

			drawList.AddBox(nextLayer, { barX, absPos.y }, { m_ScrollBarWidth, trackH }, glm::vec4(0.1f, 0.1f, 0.13f, 1.0f), clipIdx);
			drawList.AddBox(nextLayer + 1, { barX + 1, absPos.y + thumbY }, { m_ScrollBarWidth - 2, thumbH }, glm::vec4(0.3f, 0.3f, 0.35f, 1.0f), clipIdx);
			nextLayer += 2;
		}

		return nextLayer;
	}

	Reply OnMouseButtonDown(const glm::vec2& screenPos, int button) override
	{
		if (button != 0) return Reply::Unhandled();

		float contentH = m_Child ? m_Child->GetCachedDesiredSize().y : 0.0f;
		float trackH = m_CachedViewportHeight;
		if (trackH <= 0 || contentH <= trackH) return Reply::Unhandled();

		// Check if click is on the scrollbar (using absolute screen coords)
		if (screenPos.x < m_ScrollBarAbsX)
			return Reply::Unhandled();

		float thumbH = (trackH / contentH) * trackH;
		thumbH = std::max(thumbH, 16.0f);
		float maxScroll = contentH - trackH;
		float thumbTravel = trackH - thumbH;

		float localY = screenPos.y - m_ScrollBarAbsY;
		float thumbCenter = localY - thumbH * 0.5f;
		m_ScrollOffset.y = (thumbCenter / thumbTravel) * maxScroll;
		m_ScrollOffset.y = std::max(0.0f, std::min(m_ScrollOffset.y, maxScroll));

		m_DraggingScrollbar = true;
		m_DragStartOffset = m_ScrollOffset.y;
		m_DragStartMouseY = screenPos.y;
		return Reply::Handled_().CaptureMouse();
	}

	Reply OnMouseMove(const glm::vec2& screenPos) override
	{
		if (!m_DraggingScrollbar) return Reply::Unhandled();

		float contentH = m_Child ? m_Child->GetCachedDesiredSize().y : 0.0f;
		float trackH = m_CachedViewportHeight;
		if (trackH <= 0 || contentH <= trackH) return Reply::Unhandled();

		float thumbH = (trackH / contentH) * trackH;
		thumbH = std::max(thumbH, 16.0f);
		float maxScroll = contentH - trackH;
		float thumbTravel = trackH - thumbH;

		float delta = screenPos.y - m_DragStartMouseY;
		float scrollDelta = (delta / thumbTravel) * maxScroll;
		m_ScrollOffset.y = std::max(0.0f, std::min(m_DragStartOffset + scrollDelta, maxScroll));
		return Reply::Handled_();
	}

	Reply OnMouseButtonUp(const glm::vec2&, int button) override
	{
		if (button == 0 && m_DraggingScrollbar)
		{
			m_DraggingScrollbar = false;
			return Reply::Handled_().ReleaseCapture();
		}
		return Reply::Unhandled();
	}

	Reply OnMouseWheel(const glm::vec2&, float delta) override
	{
		m_ScrollOffset.y -= delta * 20.0f;
		return Reply::Handled_();
	}

	bool SupportsFocus() const override { return true; }
	void ForEachChild(std::function<void(Widget*)> fn) override { if (m_Child) fn(m_Child.get()); }

private:
	std::unique_ptr<Widget> m_Child;
	mutable glm::vec2 m_ScrollOffset = { 0, 0 };
	float m_ScrollBarWidth = 10.0f;
	float m_ForcedHeight = -1.0f;  // <0 = use parent-allocated height

	// Cache for scrollbar interaction
	mutable float m_CachedViewportWidth = 0.0f;
	mutable float m_CachedViewportHeight = 0.0f;
	mutable float m_ScrollBarAbsX = 0.0f;
	mutable float m_ScrollBarAbsY = 0.0f;
	mutable bool  m_DraggingScrollbar = false;
	mutable float m_DragStartOffset = 0.0f;
	mutable float m_DragStartMouseY = 0.0f;
};

} // namespace wisp

