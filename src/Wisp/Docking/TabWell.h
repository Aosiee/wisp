#pragma once

#include "Wisp/Core.h"
#include "Wisp/DrawList.h"
#include "Wisp/FontCache.h"
#include "Wisp/Style.h"
#include "Wisp/Theme.h"
#include "Wisp/Docking/DockModel.h"
#include "Wisp/Docking/DockPaint.h"
#include <algorithm>
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
			barW += dockpaint::MeasureWidth(m_FontCache, m_Font, n, m_FontSize, 1.0f) + 44.0f;
		// The strip scrolls when the tabs overflow, so the desired width
		// must not demand the full tab sum (that pinned the well wide and
		// forced neighbours to shrink instead).
		barW = std::min(barW, 400.0f);
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

		// ---- Tab strip (scrollable) ----
		// Pass 1: natural widths + scroll bookkeeping. Tabs are sized to
		// their label - the text always renders at full scale (never
		// shrunk to fit) - and the strip scrolls horizontally when the
		// tabs overflow the well width.
		// The arrow slots sit at the strip's OWN edges and take space:
		// while overflowing with arrows shown, the tab strip is inset so
		// no tab can end up behind an arrow.
		const float arrowSlot = 22.0f;
		const bool arrowsShown = m_ShowScrollArrows;
		float stripX = absPos.x;
		float stripW = geom.LocalSize.x;
		std::vector<float> widths(m_Tabs.size());
		float totalW = 2.0f;
		for (int i = 0; i < (int)m_Tabs.size(); i++)
		{
			float labelW = dockpaint::MeasureWidth(m_FontCache, m_Font, m_DisplayNames[i], m_FontSize, 1.0f);
			widths[i] = std::max(labelW + 40.0f, 60.0f);
			totalW += widths[i] + 2.0f;
		}
		bool fullOverflow = totalW > stripW;
		if (fullOverflow && arrowsShown)
		{
			stripX += arrowSlot;
			stripW = std::max(40.0f, stripW - arrowSlot * 2.0f);
		}
		m_Overflow = totalW > stripW;
		m_MaxScrollX = std::max(0.0f, totalW - stripW);
		m_ScrollX = std::clamp(m_ScrollX, 0.0f, m_MaxScrollX);

		// When the active tab changes, bring it into view once - manual
		// scrolling after that is left alone.
		if (m_ActiveIndex >= 0 && m_ActiveIndex < (int)m_Tabs.size())
		{
			if (m_ActiveIndex != m_LastActiveForScroll)
			{
				m_LastActiveForScroll = m_ActiveIndex;
				if (m_Overflow)
				{
					float ax = 2.0f;
					for (int i = 0; i < m_ActiveIndex; i++) ax += widths[i] + 2.0f;
					if (ax < m_ScrollX)
						m_ScrollX = std::max(0.0f, ax - 2.0f);
					else if (ax + widths[m_ActiveIndex] > m_ScrollX + stripW)
						m_ScrollX = std::min(m_MaxScrollX, ax + widths[m_ActiveIndex] - stripW + 2.0f);
				}
			}
		}

		// Pass 2: draw the strip, clipped so scrolled-out tabs can never
		// paint past the bar (onto a divider or a neighbouring panel).
		int stripClip = drawList.PushClip({ stripX, absPos.y, stripW, m_Style.TabHeight }, clip);
		glm::vec2 mouse = UIApplication::IsValid() ? UIApplication::Get().GetMousePos() : glm::vec2(-1, -1);
		m_TabRects.clear();
		float x = stripX + 2.0f - m_ScrollX;
		for (int i = 0; i < (int)m_Tabs.size(); i++)
		{
			float w = widths[i];
			bool active = (i == m_ActiveIndex);
			bool hovered = (mouse.x >= x && mouse.x <= x + w && mouse.y >= absPos.y && mouse.y <= absPos.y + m_Style.TabHeight);
			m_TabRects.push_back({ x, absPos.y, w, m_Style.TabHeight });

			glm::vec4 bg = active ? m_Style.Active.Tint : (hovered ? m_Style.Hovered.Tint : m_Style.Inactive.Tint);
			drawList.AddBox(layerId + 2, { x, absPos.y }, { w, m_Style.TabHeight }, bg, stripClip);
			if (active)
				drawList.AddBox(layerId + 2, { x, absPos.y }, { w, 2.0f }, m_Style.Active.Tint * 1.6f, stripClip);

			// Label (full scale - tab widths follow the text, not the
			// other way round).
			float inkTop, inkBottom;
			dockpaint::MeasureInkBox(m_FontCache, m_Font, m_DisplayNames[i], m_FontSize, 1.0f, inkTop, inkBottom);
			float ly = absPos.y + (m_Style.TabHeight - (inkBottom - inkTop)) * 0.5f - inkTop;
			dockpaint::PaintText(drawList, m_FontCache, m_Font, m_FontSize, layerId + 3,
				{ x + 8.0f, ly }, m_DisplayNames[i],
				active ? m_Style.LabelActiveTint : m_Style.LabelTint, stripClip, 1.0f);

			// Close button (x)
			float cx = x + w - 16.0f;
			float cy = absPos.y + (m_Style.TabHeight - 12.0f) * 0.5f;
			bool closeHovered = (mouse.x >= cx - 2.0f && mouse.x <= cx + 14.0f && mouse.y >= cy - 2.0f && mouse.y <= cy + 14.0f);
			if (closeHovered)
				drawList.AddBox(layerId + 3, { cx - 2.0f, cy - 2.0f }, { 14.0f, 14.0f }, m_Style.CloseButton.Tint, stripClip);
			drawList.AddBox(layerId + 4, { cx + 4.0f, cy + 5.5f }, { 4.0f, 1.0f }, m_Style.CloseTint, stripClip);
			drawList.AddBox(layerId + 4, { cx + 5.5f, cy + 4.0f }, { 1.0f, 4.0f }, m_Style.CloseTint, stripClip);

			x += w + 2.0f;
		}

		// Scroll arrows at the strip's OWN edges, only while overflowing
		// (and only when the toggle is on). They occupy their edge slots -
		// the tab strip above is inset by the same amount, so the arrows
		// never cover a tab.
		ComputeScrollArrowRects(absPos, geom.LocalSize.x, arrowsShown);
		if (m_Overflow && arrowsShown)
		{
			auto paintArrow = [&](const glm::vec4& r, const char* glyph) {
				bool hovered = (mouse.x >= r.x && mouse.x <= r.x + r.z && mouse.y >= r.y && mouse.y <= r.y + r.w);
				drawList.AddBox(layerId + 5, { r.x, r.y }, { r.z, r.w },
					hovered ? m_Style.Hovered.Tint : glm::vec4(0.11f, 0.11f, 0.18f, 1.0f), clip);
				float inkTop, inkBottom;
				dockpaint::MeasureInkBox(m_FontCache, m_Font, glyph, m_FontSize, 1.0f, inkTop, inkBottom);
				float gy = r.y + (r.w - (inkBottom - inkTop)) * 0.5f - inkTop;
				dockpaint::PaintText(drawList, m_FontCache, m_Font, m_FontSize, layerId + 6,
					{ r.x + (r.z - 7.0f) * 0.5f, gy }, glyph, glm::vec4(0.75f, 0.75f, 0.8f, 1.0f), clip, 1.0f);
			};
			paintArrow(m_ScrollArrowL, "<");
			paintArrow(m_ScrollArrowR, ">");
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
				child.WidgetPtr->OnPaint(childArgs, child.Geom, drawList, layerId + 7, cs);
		}
		return layerId + 8;
	}

	// Scroll arrows (shown only while the strip overflows). Toggleable: an
	// app that wants a plain clipped strip can turn them off. When shown
	// they sit on their OWN sides and take space - the tab strip ends
	// before the arrow, so no tab ever sits half-behind one.
	void SetShowScrollArrows(bool show) { m_ShowScrollArrows = show; InvalidateLayout(); }
	bool GetShowScrollArrows() const { return m_ShowScrollArrows; }

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

	// Scroll-arrow hit test: -1 = left, +1 = right, 0 = neither (the
	// arrows only exist while the strip overflows).
	int HitTestScrollArrow(const glm::vec2& absPos) const
	{
		if (!m_Overflow) return 0;
		auto inside = [&](const glm::vec4& r) {
			return r.z > 0.0f && absPos.x >= r.x && absPos.x <= r.x + r.z
				&& absPos.y >= r.y && absPos.y <= r.y + r.w;
		};
		if (inside(m_ScrollArrowL)) return -1;
		if (inside(m_ScrollArrowR)) return 1;
		return 0;
	}

	// Horizontal scroll: wheel over the tab strip pans the tabs. The strip
	// only scrolls while it overflows (the content area below the bar
	// keeps the panel's own wheel handling).
	Reply OnMouseWheel(const glm::vec2& absPos, float delta) override
	{
		if (!m_Overflow || m_MaxScrollX <= 0.0f) return Reply::Unhandled();
		if (absPos.y < m_LastAbsRect.y || absPos.y > m_LastAbsRect.y + m_Style.TabHeight)
			return Reply::Unhandled();
		m_ScrollX = std::clamp(m_ScrollX - delta * 48.0f, 0.0f, m_MaxScrollX);
		return Reply::Handled_();
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
	void ComputeScrollArrowRects(const glm::vec2& absPos, float barW, bool arrowsShown) const
	{
		if (!arrowsShown)
		{
			m_ScrollArrowL = { 0, 0, 0, 0 };
			m_ScrollArrowR = { 0, 0, 0, 0 };
			return;
		}
		const float arrowW = 20.0f, arrowH = 20.0f;
		float ay = absPos.y + (m_Style.TabHeight - arrowH) * 0.5f;
		m_ScrollArrowL = { absPos.x + 2.0f, ay, arrowW, arrowH };
		m_ScrollArrowR = { absPos.x + barW - arrowW - 2.0f, ay, arrowW, arrowH };
	}

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

	// Horizontal tab-strip scroll state (updated during paint; consumed by
	// the wheel/arrow handlers between paints).
	mutable float m_ScrollX = 0.0f;
	mutable float m_MaxScrollX = 0.0f;
	mutable bool m_Overflow = false;
	mutable int m_LastActiveForScroll = -1;
	mutable glm::vec4 m_ScrollArrowL{ 0, 0, 0, 0 };
	mutable glm::vec4 m_ScrollArrowR{ 0, 0, 0, 0 };
	bool m_ShowScrollArrows = true;
};

} // namespace wisp
