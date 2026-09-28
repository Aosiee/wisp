#pragma once

#include "Wisp/Core.h"
#include "Wisp/DrawList.h"
#include "Wisp/FontCache.h"
#include "Wisp/Style.h"
#include "Wisp/Theme.h"
#include "Wisp/Docking/DockModel.h"
#include "Wisp/Docking/DockPaint.h"
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace wisp {

class DockSpace;

class TabWell : public Widget
{
public:
	WISP_WIDGET_TYPE(TabWell)

	TabWell() { m_Style = GetDefaultStyle(); }

	// The content is shared with the dock's panel cache and outlives this
	// chrome widget when its window closes - detach the parent link so no
	// live parent-chain walk can reach a dead TabWell.
	~TabWell()
	{
		if (m_Content)
			ClearParent(m_Content.get());
	}

	static TabStyle GetDefaultStyle()
	{
		TabStyle s;
		s.Active.Tint = { 0.13f, 0.13f, 0.2f, 1.0f };
		s.Inactive.Tint = { 0.08f, 0.08f, 0.13f, 1.0f };
		s.Hovered.Tint = { 0.17f, 0.17f, 0.24f, 1.0f };
		s.ContentBackground.Tint = { 0.06f, 0.06f, 0.1f, 1.0f };
		s.CloseButton.Tint = { 0.15f, 0.15f, 0.2f, 1.0f };
		return s;
	}

	void SetDockSpace(DockSpace* ds, dock::NodeId nodeId)
	{
		m_DockSpace = ds;
		m_NodeId = nodeId;
	}
	void SetFont(FontCache* fc, FontHandle font, float fontSize)
	{
		m_FontCache = fc;
		m_Font = font;
		m_FontSize = fontSize;
	}

	void SetTabs(const std::vector<dock::TabEntry>& tabs, const std::vector<std::string>& displayNames, int activeIndex)
	{
		m_Tabs = tabs;
		m_DisplayNames = displayNames;
		m_ActiveIndex = activeIndex;
		if (m_ActiveIndex < 0 || m_ActiveIndex >= (int)m_Tabs.size())
			m_ActiveIndex = m_Tabs.empty() ? 0 : (int)m_Tabs.size() - 1;
		InvalidateLayout();
	}

	// Active tab's cached content (owned by the DockSpace panel cache).
	void SetContent(std::shared_ptr<Widget> content)
	{
		m_Content = std::move(content);
		if (m_Content) SetupParentChild(this, m_Content.get());
		InvalidateLayout();
	}

	Widget* GetContentRaw() const { return m_Content.get(); }
	dock::NodeId GetNodeId() const { return m_NodeId; }
	const glm::vec4& GetLastAbsRect() const { return m_LastAbsRect; }
	const std::vector<dock::TabEntry>& GetTabs() const { return m_Tabs; }
	int GetActiveIndex() const { return m_ActiveIndex; }
	const std::vector<glm::vec4>& GetTabRects() const { return m_TabRects; } // abs x,y,w,h

	glm::vec2 ComputeDesiredSize() const override
	{
		float contentW = 0.0f, contentH = 0.0f;
		if (m_Content)
		{
			glm::vec2 d = m_Content->ComputeDesiredSize();
			m_Content->SetCachedDesiredSize(d);
			contentW = d.x;
			contentH = d.y;
		}
		float barW = 4.0f;
		for (const auto& n : m_DisplayNames)
			barW += dockpaint::MeasureWidth(m_FontCache, m_Font, n, 1.0f) + 44.0f;
		return { std::max(contentW, barW), m_Style.TabHeight + contentH };
	}

	void ArrangeChildren(const Geometry& geom, ArrangedChildren& out) const override
	{
		if (!m_Content) return;
		glm::vec2 contentPos = { 0.0f, m_Style.TabHeight };
		glm::vec2 contentSize = { geom.LocalSize.x, std::max(0.0f, geom.LocalSize.y - m_Style.TabHeight) };
		out.Add(m_Content.get(), geom.MakeChild(contentPos, contentSize));
	}

	int OnPaint(const PaintArgs& args, const Geometry& geom, DrawList& drawList, int layerId, const WidgetStyleContext&) const override
	{
		if (m_StyleDirty)
		{
			m_Style = ResolveStyle(this, GetDefaultStyle());
			m_StyleDirty = false;
		}

		glm::vec2 absPos = geom.LocalToAbsolute({ 0, 0 });
		m_LastAbsRect = { absPos.x, absPos.y, geom.LocalSize.x, geom.LocalSize.y };
		int clip = args.InheritedClipIndex;

		// Content background
		drawList.AddBox(layerId, { absPos.x, absPos.y + m_Style.TabHeight },
			{ geom.LocalSize.x, std::max(0.0f, geom.LocalSize.y - m_Style.TabHeight) },
			m_Style.ContentBackground.Tint, clip);

		// Tab bar background strip
		drawList.AddBox(layerId + 1, absPos, { geom.LocalSize.x, m_Style.TabHeight }, glm::vec4(0.05f, 0.05f, 0.08f, 1.0f), clip);

		// Tabs
		glm::vec2 mouse = UIApplication::IsValid() ? UIApplication::Get().GetMousePos() : glm::vec2(-1, -1);
		m_TabRects.clear();
		float x = absPos.x + 2.0f;
		for (int i = 0; i < (int)m_Tabs.size(); i++)
		{
			float labelW = dockpaint::MeasureWidth(m_FontCache, m_Font, m_DisplayNames[i], 1.0f);
			float w = labelW + 40.0f;
			bool active = (i == m_ActiveIndex);
			bool hovered = (mouse.x >= x && mouse.x <= x + w && mouse.y >= absPos.y && mouse.y <= absPos.y + m_Style.TabHeight);
			m_TabRects.push_back({ x, absPos.y, w, m_Style.TabHeight });

			glm::vec4 bg = active ? m_Style.Active.Tint : (hovered ? m_Style.Hovered.Tint : m_Style.Inactive.Tint);
			drawList.AddBox(layerId + 2, { x, absPos.y }, { w, m_Style.TabHeight }, bg, clip);
			if (active)
				drawList.AddBox(layerId + 2, { x, absPos.y }, { w, 2.0f }, m_Style.Active.Tint * 1.6f, clip);

			// Label
			float inkTop, inkBottom;
			dockpaint::MeasureInkBox(m_FontCache, m_Font, m_DisplayNames[i], 1.0f, inkTop, inkBottom);
			float ly = absPos.y + (m_Style.TabHeight - (inkBottom - inkTop)) * 0.5f - inkTop;
			dockpaint::PaintText(drawList, m_FontCache, m_Font, layerId + 3,
				{ x + 8.0f, ly }, m_DisplayNames[i],
				active ? m_Style.LabelActiveTint : m_Style.LabelTint, clip, 1.0f);

			// Close button (x)
			float cx = x + w - 16.0f;
			float cy = absPos.y + (m_Style.TabHeight - 12.0f) * 0.5f;
			bool closeHovered = (mouse.x >= cx - 2.0f && mouse.x <= cx + 14.0f && mouse.y >= cy - 2.0f && mouse.y <= cy + 14.0f);
			if (closeHovered)
				drawList.AddBox(layerId + 3, { cx - 2.0f, cy - 2.0f }, { 14.0f, 14.0f }, m_Style.CloseButton.Tint, clip);
			drawList.AddBox(layerId + 4, { cx + 4.0f, cy + 5.5f }, { 4.0f, 1.0f }, m_Style.CloseTint, clip);
			drawList.AddBox(layerId + 4, { cx + 5.5f, cy + 4.0f }, { 1.0f, 4.0f }, m_Style.CloseTint, clip);

			x += w + 2.0f;
		}

		// Content
		if (m_Content)
		{
			// Panel content is hard-clipped to the well's content area so a
			// short pane can never paint over a neighbouring dock panel.
			int contentClip = drawList.PushClip(
				{ absPos.x, absPos.y + m_Style.TabHeight, geom.LocalSize.x, std::max(0.0f, geom.LocalSize.y - m_Style.TabHeight) },
				args.InheritedClipIndex);
			ArrangedChildren arranged;
			ArrangeChildren(geom, arranged);
			PaintArgs childArgs = args;
			childArgs.InheritedClipIndex = contentClip;
			WidgetStyleContext cs;
			cs.ColorAndOpacity = m_Content->GetRenderTint();
			cs.ColorAndOpacity.a *= m_Content->GetRenderOpacity();
			for (auto& child : arranged)
				child.WidgetPtr->OnPaint(childArgs, child.Geom, drawList, layerId + 5, cs);
		}
		return layerId + 6;
	}

	// Tab index under a point (absolute coords), or -1.
	int HitTestTabs(const glm::vec2& absPos) const
	{
		for (int i = 0; i < (int)m_TabRects.size(); i++)
		{
			const auto& r = m_TabRects[i];
			if (absPos.x >= r.x && absPos.x <= r.x + r.z && absPos.y >= r.y && absPos.y <= r.y + r.w)
				return i;
		}
		return -1;
	}

	// True if the point is over a tab's close button.
	bool HitTestClose(const glm::vec2& absPos, int tabIndex) const
	{
		if (tabIndex < 0 || tabIndex >= (int)m_TabRects.size()) return false;
		const auto& r = m_TabRects[tabIndex];
		float cx = r.x + r.z - 16.0f;
		float cy = r.y + (r.w - 12.0f) * 0.5f;
		return absPos.x >= cx - 2.0f && absPos.x <= cx + 14.0f && absPos.y >= cy - 2.0f && absPos.y <= cy + 14.0f;
	}

	// Defined in DockSpace.h (needs the complete DockSpace type).
	Reply OnMouseButtonDown(const glm::vec2& absPos, int button) override;
	Reply OnMouseButtonUp(const glm::vec2& absPos, int button) override;
	void  OnDragDetected() override { m_PressedTab = -1; }

	void ForEachChild(std::function<void(Widget*)> fn) override
	{
		if (m_Content) fn(m_Content.get());
	}

private:
	DockSpace* m_DockSpace = nullptr;
	dock::NodeId m_NodeId = 0;
	FontCache* m_FontCache = nullptr;
	FontHandle m_Font = 0;
	float m_FontSize = 13.0f;

	std::vector<dock::TabEntry> m_Tabs;
	std::vector<std::string> m_DisplayNames;
	int m_ActiveIndex = 0;
	std::shared_ptr<Widget> m_Content;

	mutable std::vector<glm::vec4> m_TabRects;
	mutable glm::vec4 m_LastAbsRect{ 0, 0, 0, 0 };
	mutable TabStyle m_Style;
	mutable bool m_StyleDirty = true;
	int m_PressedTab = -1;
};

} // namespace wisp
