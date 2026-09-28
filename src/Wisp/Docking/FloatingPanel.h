#pragma once

#include "Wisp/Core.h"
#include "Wisp/DrawList.h"
#include "Wisp/FontCache.h"
#include "Wisp/Style.h"
#include "Wisp/Theme.h"
#include "Wisp/Application.h"
#include "Wisp/Docking/DockModel.h"
#include "Wisp/Docking/DockPaint.h"
#include <functional>
#include <memory>
#include <string>

namespace wisp {

class DockSpace;

class FloatingPanel : public Widget
{
public:
	WISP_WIDGET_TYPE(FloatingPanel)

	FloatingPanel() { m_Style = GetDefaultStyle(); }

	static DockStyle GetDefaultStyle()
	{
		DockStyle s;
		s.Divider.Tint = { 0.16f, 0.16f, 0.24f, 1.0f };
		s.FloatingTitleBar.Tint = { 0.14f, 0.14f, 0.22f, 1.0f };
		s.FloatingTitleBarInactive.Tint = { 0.1f, 0.1f, 0.16f, 1.0f };
		s.DiamondIdle.Tint = { 0.1f, 0.1f, 0.16f, 1.0f };
		s.DiamondHovered.Tint = { 0.25f, 0.4f, 0.6f, 1.0f };
		return s;
	}

	void SetDockSpace(DockSpace* ds, dock::NodeId floatingId)
	{
		m_DockSpace = ds;
		m_FloatingId = floatingId;
	}
	void SetFont(FontCache* fc, FontHandle font, float fontSize)
	{
		m_FontCache = fc;
		m_Font = font;
		m_FontSize = fontSize;
	}
	void SetTitle(const std::string& t) { m_Title = t; }
	void SetActive(bool a) { m_Active = a; }
	void SetContent(std::unique_ptr<Widget> c)
	{
		m_Content = std::move(c);
		if (m_Content) SetupParentChild(this, m_Content.get());
	}

	dock::NodeId GetFloatingNodeId() const { return m_FloatingId; }
	const glm::vec4& GetLastAbsRect() const { return m_LastAbsRect; }

	glm::vec2 ComputeDesiredSize() const override
	{
		float cw = 240.0f, ch = 0.0f;
		if (m_Content)
		{
			glm::vec2 d = m_Content->ComputeDesiredSize();
			m_Content->SetCachedDesiredSize(d);
			cw = std::max(cw, d.x);
			ch = d.y;
		}
		return { cw, m_Style.FloatingTitleHeight + ch };
	}

	void ArrangeChildren(const Geometry& geom, ArrangedChildren& out) const override
	{
		if (!m_Content) return;
		glm::vec2 pos = { m_DragDelta.x, m_Style.FloatingTitleHeight + m_DragDelta.y };
		glm::vec2 size = { geom.LocalSize.x, std::max(0.0f, geom.LocalSize.y - m_Style.FloatingTitleHeight) };
		out.Add(m_Content.get(), geom.MakeChild(pos, size));
	}

	int OnPaint(const PaintArgs& args, const Geometry& geom, DrawList& drawList, int layerId, const WidgetStyleContext&) const override
	{
		if (m_StyleDirty)
		{
			m_Style = ResolveStyle(this, GetDefaultStyle());
			m_StyleDirty = false;
		}

		glm::vec2 base = geom.LocalToAbsolute({ 0, 0 });
		glm::vec2 absPos = base + m_DragDelta;
		m_LastAbsRect = { base.x, base.y, geom.LocalSize.x, geom.LocalSize.y }; // resting rect (drag delta separate)
		int clip = args.InheritedClipIndex;
		float th = m_Style.FloatingTitleHeight;

		// Panel body
		drawList.AddBox(layerId, absPos, geom.LocalSize, glm::vec4(0.07f, 0.07f, 0.1f, 0.98f), clip);
		// Title bar
		glm::vec4 bar = m_Active ? m_Style.FloatingTitleBar.Tint : m_Style.FloatingTitleBarInactive.Tint;
		drawList.AddBox(layerId + 1, absPos, { geom.LocalSize.x, th }, bar, clip);

		// Title text
		float inkTop, inkBottom;
		dockpaint::MeasureInkBox(m_FontCache, m_Font, m_Title, 1.0f, inkTop, inkBottom);
		float ly = absPos.y + (th - (inkBottom - inkTop)) * 0.5f - inkTop;
		dockpaint::PaintText(drawList, m_FontCache, m_Font, layerId + 2, { absPos.x + 8.0f, ly },
			m_Title, m_Style.FloatingTitleTint, clip, 1.0f);

		// Close button
		float cx = absPos.x + geom.LocalSize.x - 16.0f;
		float cy = absPos.y + (th - 12.0f) * 0.5f;
		glm::vec2 mouse = UIApplication::IsValid() ? UIApplication::Get().GetMousePos() : glm::vec2(-1, -1);
		bool closeHover = (mouse.x >= cx - 2.0f && mouse.x <= cx + 14.0f && mouse.y >= cy - 2.0f && mouse.y <= cy + 14.0f);
		if (closeHover)
			drawList.AddBox(layerId + 2, { cx - 2.0f, cy - 2.0f }, { 14.0f, 14.0f }, { 0.2f, 0.2f, 0.28f, 1.0f }, clip);
		drawList.AddBox(layerId + 3, { cx + 4.0f, cy + 5.5f }, { 4.0f, 1.0f }, { 0.7f, 0.7f, 0.75f, 1.0f }, clip);
		drawList.AddBox(layerId + 3, { cx + 5.5f, cy + 4.0f }, { 1.0f, 4.0f }, { 0.7f, 0.7f, 0.75f, 1.0f }, clip);

		// Content (hard-clipped to the panel body)
		int contentLayer = layerId + 4;
		if (m_Content)
		{
			int bodyClip = drawList.PushClip(
				{ absPos.x, absPos.y + m_Style.FloatingTitleHeight, geom.LocalSize.x, std::max(0.0f, geom.LocalSize.y - m_Style.FloatingTitleHeight) },
				args.InheritedClipIndex);
			ArrangedChildren arranged;
			ArrangeChildren(geom, arranged);
			PaintArgs childArgs = args;
			childArgs.InheritedClipIndex = bodyClip;
			WidgetStyleContext cs;
			cs.ColorAndOpacity = m_Content->GetRenderTint();
			cs.ColorAndOpacity.a *= m_Content->GetRenderOpacity();
			for (auto& child : arranged)
				contentLayer = child.WidgetPtr->OnPaint(childArgs, child.Geom, drawList, contentLayer, cs);
		}

		// Resize grip (bottom-right corner) — drawn above whatever the content
		// subtree returned so it can never be covered.
		float g = 12.0f;
		float gx = absPos.x + geom.LocalSize.x - g;
		float gy = absPos.y + geom.LocalSize.y - g;
		drawList.AddBox(contentLayer + 1, { gx, gy }, { g, g }, glm::vec4(0.2f, 0.2f, 0.28f, 1.0f), clip);
		return contentLayer + 2;
	}

	// Defined in DockSpace.h (needs the complete DockSpace type).
	Reply OnMouseButtonDown(const glm::vec2& absPos, int button) override;
	Reply OnMouseMove(const glm::vec2& absPos) override;
	Reply OnMouseButtonUp(const glm::vec2& absPos, int button) override;
	void  OnDragDetected() override { m_PressMode = 0; }

	void ForEachChild(std::function<void(Widget*)> fn) override
	{
		if (m_Content) fn(m_Content.get());
	}

	bool IsTitleArea(const glm::vec2& absPos) const
	{
		glm::vec2 p = { m_LastAbsRect.x + m_DragDelta.x, m_LastAbsRect.y + m_DragDelta.y };
		return absPos.y >= p.y && absPos.y <= p.y + m_Style.FloatingTitleHeight
			&& absPos.x >= p.x && absPos.x <= p.x + m_LastAbsRect.z;
	}
	bool IsCloseArea(const glm::vec2& absPos) const
	{
		glm::vec2 p = { m_LastAbsRect.x + m_DragDelta.x, m_LastAbsRect.y + m_DragDelta.y };
		float cx = p.x + m_LastAbsRect.z - 16.0f;
		float cy = p.y + (m_Style.FloatingTitleHeight - 12.0f) * 0.5f;
		return absPos.x >= cx - 2.0f && absPos.x <= cx + 14.0f && absPos.y >= cy - 2.0f && absPos.y <= cy + 14.0f;
	}
	bool IsResizeGrip(const glm::vec2& absPos) const
	{
		glm::vec2 p = { m_LastAbsRect.x + m_DragDelta.x, m_LastAbsRect.y + m_DragDelta.y };
		return absPos.x >= p.x + m_LastAbsRect.z - 12.0f && absPos.x <= p.x + m_LastAbsRect.z
			&& absPos.y >= p.y + m_LastAbsRect.w - 12.0f && absPos.y <= p.y + m_LastAbsRect.w;
	}

	void ResetGestureState()
	{
		m_PressMode = 0;
		m_DragDelta = { 0, 0 };
		m_PressStart = { 0, 0 };
		m_ResizeStart = { 0, 0 };
		m_ModelPosStart = { 0, 0 };
	}

private:
	DockSpace* m_DockSpace = nullptr;
	dock::NodeId m_FloatingId = 0;
	FontCache* m_FontCache = nullptr;
	FontHandle m_Font = 0;
	float m_FontSize = 13.0f;
	std::string m_Title = "Floating";
	std::unique_ptr<Widget> m_Content;
	bool m_Active = false;

	mutable glm::vec4 m_LastAbsRect{ 0, 0, 0, 0 };
	mutable DockStyle m_Style;
	mutable bool m_StyleDirty = true;

	int m_PressMode = 0; // 0 none, 1 title move, 2 resize
	glm::vec2 m_DragDelta{ 0, 0 };
	glm::vec2 m_PressStart{ 0, 0 };
	glm::vec2 m_ResizeStart{ 0, 0 };
	glm::vec2 m_ModelPosStart{ 0, 0 };
};

} // namespace wisp
