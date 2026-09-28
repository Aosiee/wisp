#pragma once

#include "Wisp/Core.h"
#include "Wisp/DrawList.h"
#include "Wisp/FontCache.h"
#include "Wisp/Application.h"
#include "Wisp/Docking/DockSpace.h"
#include <algorithm>
#include <cstring>
#include <string>
#include <unordered_map>
#include <vector>

namespace wisp {

class WidgetReflector : public Widget
{
public:
	WISP_WIDGET_TYPE(WidgetReflector)

	WidgetReflector(FontCache* fc, FontHandle font, float fontSize)
		: m_FontCache(fc), m_Font(font), m_FontSize(fontSize)
	{
		if (UIApplication::IsValid())
			UIApplication::Get().SetPickExclusion(this);
	}

	~WidgetReflector()
	{
		// The selection highlight is painted per-window by UIApplication;
		// if the reflector dies the outline must not outlive it.
		if (UIApplication::IsValid())
			UIApplication::Get().ClearSelectionHighlight();
	}

	glm::vec2 ComputeDesiredSize() const override { return { 360, 420 }; }
	void ArrangeChildren(const Geometry&, ArrangedChildren&) const override {}

	int OnPaint(const PaintArgs& args, const Geometry& geom, DrawList& drawList, int layerId, const WidgetStyleContext&) const override
	{
		glm::vec2 pos = geom.LocalToAbsolute({0,0});
		float w = geom.LocalSize.x, h = geom.LocalSize.y;
		int clip = args.InheritedClipIndex;

		drawList.AddBox(layerId, pos, {w,h}, {0.05f,0.05f,0.09f,1}, clip);

		// Header + mode buttons
		PaintText(drawList, layerId+1, {pos.x+4, pos.y+4}, "Widget Reflector", {0.3f,0.8f,1.0f,1}, clip);

		DrawModeButtons(drawList, layerId+1, pos, w, clip);

		float treeTop = pos.y + 52;
		float treeHeight = h * 0.58f;
		float inspTop = treeTop + treeHeight + 4;
		const float rowH = 22;
		const float textScale = 0.62f;

		// Rebuild tree snapshot
		RebuildTree();

		// Auto-scroll to the selected widget after pick/select
		if (m_ScrollToSelected && m_Selected)
		{
			int selIdx = -1;
			for (size_t i = 0; i < m_Rows.size(); i++)
				if (m_Rows[i].Target == m_Selected) { selIdx = (int)i; break; }
			if (selIdx >= 0)
			{
				float selTop = (float)selIdx * rowH;
				float selBottom = selTop + rowH;
				if (selTop < m_ScrollOffset)
					m_ScrollOffset = selTop;
				else if (selBottom > m_ScrollOffset + treeHeight)
					m_ScrollOffset = selBottom - treeHeight;
				float maxScroll = std::max(0.0f, (float)m_Rows.size() * rowH - treeHeight);
				m_ScrollOffset = std::max(0.0f, std::min(m_ScrollOffset, maxScroll));
			}
			m_ScrollToSelected = false;
		}

		int treeClip = drawList.PushClip({pos.x, treeTop, w, treeHeight}, clip);

		// Tree rows
		float y = treeTop - m_ScrollOffset;
		m_RowRects.clear();
		m_TreeHeight = treeHeight;
		const float kIndent = 16.0f;
		const glm::vec4 guideCol = { 0.28f, 0.28f, 0.36f, 1.0f };
		for (size_t ri = 0; ri < m_Rows.size(); ri++)
		{
			auto& row = m_Rows[ri];
			if (y + rowH < treeTop || y > treeTop + treeHeight) { y += rowH; continue; }
			float indX = pos.x + 4 + row.Depth * kIndent;
			float midY = y + rowH * 0.5f;

			if (row.Target == m_Selected)
				drawList.AddBox(layerId+1, {pos.x+1, y}, {w-2, rowH-1}, {0.2f,0.35f,0.55f,0.6f}, treeClip);

			// Tree guide lines (connectors to parent)
			if (row.Depth > 0)
			{
				float guideX = std::round(pos.x + 4 + (row.Depth - 1) * kIndent + 2.5f) + 0.5f;
				float nodeX = std::round(indX + 2.5f) + 0.5f;
				float midYc = std::round(midY) + 0.5f;
				bool nextIsDescendant = (ri + 1 < m_Rows.size()) && (m_Rows[ri + 1].Depth >= row.Depth);
				// Horizontal segment into the node
				drawList.AddBox(layerId+2, {guideX - 0.5f, midYc - 0.5f}, {nodeX - guideX + 1.0f, 1}, guideCol, treeClip);
				// Vertical segment: full height when a following sibling continues
				// the subtree, otherwise stop at the L-corner
				if (nextIsDescendant)
					drawList.AddBox(layerId+2, {guideX - 0.5f, y}, {1, rowH}, guideCol, treeClip);
				else
					drawList.AddBox(layerId+2, {guideX - 0.5f, y}, {1, midY - y}, guideCol, treeClip);
			}

			if (row.HasChildren)
			{
				float ay = y + (rowH - 5) * 0.5f;
				drawList.AddBox(layerId+2, {indX, ay}, {5, 5},
					row.Expanded ? glm::vec4(0.9f,0.7f,0.2f,1) : glm::vec4(0.5f,0.5f,0.6f,1), treeClip);
			}
			float textX = indX + (row.HasChildren ? 8 : 4);
			float inkTop, inkBottom;
			MeasureInkBox(row.TypeName, textScale, inkTop, inkBottom);
			float textY = y + (rowH - (inkBottom - inkTop)) * 0.5f - inkTop;
			PaintText(drawList, layerId+2, {textX, textY}, row.TypeName,
				row.Vis == Visibility::Collapsed ? glm::vec4(0.45f,0.45f,0.5f,1)
				: (row.Enabled ? glm::vec4(0.8f,0.8f,0.85f,1) : glm::vec4(0.55f,0.55f,0.6f,1)), treeClip, textScale);

			m_RowRects.push_back({row.Target, {pos.x+1, y, w-2, rowH}});
			y += rowH;
		}

		// Inspector
		drawList.AddBox(layerId+1, {pos.x, inspTop}, {w, h - (inspTop - pos.y) - 1}, {0.06f,0.06f,0.1f,1}, clip);
		PaintText(drawList, layerId+2, {pos.x+4, inspTop+3}, "Inspector", {0.5f,0.8f,0.5f,1}, clip);

		if (m_Selected)
		{
			float iy = inspTop + 24;
			auto infoLine = [&](const std::string& label, const std::string& value, const glm::vec4& col = {0.7f,0.7f,0.75f,1}) {
				PaintText(drawList, layerId+2, {pos.x+4, iy}, label + ": " + value, col, clip, 0.7f);
				iy += 16;
			};

			// Dock model dump for a selected DockSpace.
			if (std::strcmp(m_Selected->GetTypeName(), "DockSpace") == 0)
			{
				std::string dump = static_cast<DockSpace*>(m_Selected)->DumpModel();
				PaintText(drawList, layerId+2, {pos.x+4, iy}, "Dock model:", {0.5f,0.8f,0.5f,1}, clip, 0.7f);
				iy += 14;
				size_t from = 0;
				int lines = 0;
				while (from < dump.size() && lines < 12)
				{
					size_t nl = dump.find('\n', from);
					std::string line = dump.substr(from, nl == std::string::npos ? std::string::npos : nl - from);
					PaintText(drawList, layerId+2, {pos.x+8, iy}, line, {0.6f,0.6f,0.65f,1}, clip, 0.55f);
					iy += 13;
					lines++;
					if (nl == std::string::npos) break;
					from = nl + 1;
				}
			}
			else
			{
				infoLine("Type", m_Selected->GetTypeName(), {0.9f,0.8f,0.4f,1});

			for (auto& r : m_AllRects)
			{
				if (r.Target == m_Selected)
				{
					char buf[64];
					snprintf(buf, sizeof(buf), "(%.0f, %.0f)", r.AbsPos.x, r.AbsPos.y);
					infoLine("Pos", buf);
					snprintf(buf, sizeof(buf), "(%.0f x %.0f)", r.AbsSize.x, r.AbsSize.y);
					infoLine("Size", buf);
					snprintf(buf, sizeof(buf), "depth %d", r.Depth);
					infoLine("Depth", buf);
					break;
				}
			}

			const char* visNames[] = {"Visible","Hidden","Collapsed","HitTestInvisible","SelfHitTestInvisible"};
			infoLine("Visibility", visNames[(int)m_Selected->GetVisibility()]);
			infoLine("Enabled", m_Selected->IsEnabled() ? "true" : "false", m_Selected->IsEnabled() ? glm::vec4(0.5f,0.9f,0.5f,1) : glm::vec4(0.9f,0.4f,0.4f,1));
			infoLine("Focusable", m_Selected->SupportsFocus() ? "true" : "false");
			infoLine("Clipping", m_Selected->GetClipping() == Clipping::Inherit ? "Inherit" : "Custom");
			infoLine("Theme", m_Selected->GetTheme() ? "set" : "inherited");
			}
		}
		else
		{
			PaintText(drawList, layerId+2, {pos.x+4, inspTop+26}, "(click a row to select)", {0.5f,0.5f,0.55f,1}, clip);
		}

		// Highlight — pick-mode hover target (orange) or selected widget
		// (yellow). Cross-window: the outline is painted per-window by
		// UIApplication on the window that CONTAINS the target, so picking
		// or selecting a widget in any window outlines it in ITS window.
		if (UIApplication::IsValid())
		{
			Widget* hlTarget = nullptr;
			glm::vec4 hlCol{ 0, 0, 0, 0 };
			if (UIApplication::Get().IsPickModeActive())
			{
				hlTarget = UIApplication::Get().GetPickedWidget();
				hlCol = {1.0f, 0.55f, 0.1f, 0.95f};
			}
			else if (m_Selected)
			{
				hlTarget = m_Selected;
				hlCol = {1.0f, 0.8f, 0.2f, 0.9f};
			}

			if (hlTarget)
				UIApplication::Get().SetSelectionHighlight(hlTarget, hlCol);
			else
				UIApplication::Get().ClearSelectionHighlight();
		}

		return layerId + 3;
	}

	Reply OnMouseButtonDown(const glm::vec2& absPos, int button) override
	{
		if (button != 0) return Reply::Unhandled();

		// Mode buttons first
		for (auto& mb : m_ModeRects)
		{
			if (absPos.x >= mb.Rect.x && absPos.x <= mb.Rect.x + mb.Rect.z &&
				absPos.y >= mb.Rect.y && absPos.y <= mb.Rect.y + mb.Rect.w)
			{
				switch (mb.Button)
				{
				case ReflectorButton::PickHit:
					UIApplication::Get().SetPickMode(UIApplication::PickMode::HitTest);
					UIApplication::Get().SetWidgetPickCallback([this](Widget* w) { SelectWidget(w); });
					break;
				case ReflectorButton::PickAbs:
					UIApplication::Get().SetPickMode(UIApplication::PickMode::Absolute);
					UIApplication::Get().SetWidgetPickCallback([this](Widget* w) { SelectWidget(w); });
					break;
				case ReflectorButton::PickFocus:
					UIApplication::Get().SetPickMode(UIApplication::PickMode::Focusable);
					UIApplication::Get().SetWidgetPickCallback([this](Widget* w) { SelectWidget(w); });
					break;
				case ReflectorButton::PickStop:
					UIApplication::Get().SetPickMode(UIApplication::PickMode::None);
					UIApplication::Get().CancelWidgetPick();
					break;
				case ReflectorButton::Clear:
					m_Selected = nullptr;
					break;
				}
				return Reply::Handled_();
			}
		}

		// Tree rows
		for (auto& rr : m_RowRects)
		{
			if (absPos.x >= rr.Rect.x && absPos.x <= rr.Rect.x + rr.Rect.z &&
				absPos.y >= rr.Rect.y && absPos.y <= rr.Rect.y + rr.Rect.w)
			{
				m_Selected = rr.Target;
				float indX = rr.Rect.x + 4 + DepthOf(rr.Target) * 16;
				if (absPos.x >= indX && absPos.x <= indX + 8 && HasChildrenOf(rr.Target))
				{
					auto it = m_Expanded.find(rr.Target);
					bool cur = it != m_Expanded.end() ? it->second : DefaultExpanded(rr.Target);
					m_Expanded[rr.Target] = !cur;
				}
				return Reply::Handled_();
			}
		}
		return Reply::Unhandled();
	}

	Reply OnMouseWheel(const glm::vec2&, float delta) override
	{
		m_ScrollOffset -= delta * 24.0f;
		m_ScrollOffset = std::max(0.0f, m_ScrollOffset);
		float maxScroll = std::max(0.0f, (float)m_Rows.size() * 22.0f - m_TreeHeight);
		m_ScrollOffset = std::min(m_ScrollOffset, maxScroll);
		return Reply::Handled_();
	}

	void SelectWidget(Widget* w)
	{
		m_Selected = w;
		if (!w) return;
		// Collapse every previously expanded node, then expand exactly the
		// root → w path in the next tree rebuild so only relevant nodes stay
		// open. The parent-chain expansion is a best-effort fallback for
		// widgets the tree walk cannot reach.
		m_Expanded.clear();
		for (Widget* a = w->GetParent(); a; a = a->GetParent())
			m_Expanded[a] = true;
		m_PendingPathTarget = w;
		m_ScrollToSelected = true;
	}
	Widget* GetSelected() const { return m_Selected; }

private:
	struct Row
	{
		Widget* Target = nullptr;
		std::string TypeName;
		int Depth = 0;
		bool HasChildren = false;
		bool Expanded = false;
		bool Enabled = true;
		Visibility Vis = Visibility::Visible;
		glm::vec2 AbsPos{0,0};
		glm::vec2 AbsSize{0,0};
	};

	struct RowRect { Widget* Target; glm::vec4 Rect; }; // x,y,w,h absolute
	enum class ReflectorButton { PickHit, PickAbs, PickFocus, PickStop, Clear };
	struct ModeRect { ReflectorButton Button; glm::vec4 Rect; };
	struct HlRect { Widget* Target; glm::vec2 AbsPos; glm::vec2 AbsSize; int Depth; };

	bool DefaultExpanded(Widget* w) const
	{
		int depth = DepthOf(w);
		return depth < 2;
	}

	int DepthOf(Widget* w) const
	{
		int d = 0;
		for (Widget* p = w->GetParent(); p; p = p->GetParent()) d++;
		return d;
	}

	bool HasChildrenOf(Widget* w) const
	{
		int count = 0;
		w->ForEachChild([&](Widget*) { count++; });
		return count > 0;
	}

	void DrawModeButtons(DrawList& dl, int layer, const glm::vec2& pos, float w, int clip) const
	{
		m_ModeRects.clear();
		auto cur = UIApplication::IsValid() ? UIApplication::Get().GetPickMode() : UIApplication::PickMode::None;

		struct ModeDef { ReflectorButton Button; const char* Label; };
		const ModeDef defs[] = {
			{ ReflectorButton::PickHit, "Hit" },
			{ ReflectorButton::PickAbs, "Abs" },
			{ ReflectorButton::PickFocus, "Focus" },
			{ ReflectorButton::PickStop, "Stop" },
			{ ReflectorButton::Clear, "Clear" },
		};

		float bx = pos.x + 4;
		float by = pos.y + 24;
		for (auto& d : defs)
		{
			float bw = d.Button == ReflectorButton::Clear ? 46 : 40;
			bool active =
				(d.Button == ReflectorButton::PickHit   && cur == UIApplication::PickMode::HitTest) ||
				(d.Button == ReflectorButton::PickAbs   && cur == UIApplication::PickMode::Absolute) ||
				(d.Button == ReflectorButton::PickFocus && cur == UIApplication::PickMode::Focusable);
			glm::vec4 bg = active ? glm::vec4(0.2f,0.4f,0.6f,1) : glm::vec4(0.1f,0.1f,0.16f,1);
			dl.AddBox(layer, {bx, by}, {bw, 20}, bg, clip);
			float inkTop, inkBottom;
			MeasureInkBox(d.Label, 0.7f, inkTop, inkBottom);
			float ly = by + (20.0f - (inkBottom - inkTop)) * 0.5f - inkTop;
			PaintText(dl, layer+1, {bx + 5, ly}, d.Label,
				active ? glm::vec4(1,1,1,1) : glm::vec4(0.7f,0.7f,0.75f,1), clip, 0.7f);
			m_ModeRects.push_back({d.Button, {bx, by, bw, 20}});
			bx += bw + 4;
		}

		// Status text right of buttons
		const char* status = "off";
		switch (cur)
		{
		case UIApplication::PickMode::HitTest: status = "pick: hit-test"; break;
		case UIApplication::PickMode::Absolute: status = "pick: absolute"; break;
		case UIApplication::PickMode::Focusable: status = "pick: focusable"; break;
		default: break;
		}
		float sInkTop, sInkBottom;
		MeasureInkBox(status, 0.7f, sInkTop, sInkBottom);
		float sy = by + (20.0f - (sInkBottom - sInkTop)) * 0.5f - sInkTop;
		PaintText(dl, layer, {bx + 6, sy}, status,
			cur != UIApplication::PickMode::None ? glm::vec4(1.0f,0.7f,0.2f,1) : glm::vec4(0.5f,0.5f,0.55f,1), clip, 0.7f);
	}

	void RebuildTree() const
	{
		m_Rows.clear();
		m_AllRects.clear();
		// The reflector's tree spans ALL windows (cross-window fix): every
		// rooted context contributes its subtree, so a widget picked or
		// selected in any window shows up, expands to, and stays selected.
		if (!UIApplication::IsValid()) return;

		std::vector<WindowContext*> contexts;
		UIApplication::Get().ForEachContext([&](WindowContext& c) {
			if (c.Root)
				contexts.push_back(&c);
		});

		if (contexts.empty()) return;

		// Resolve a pending pick: expand exactly the root → target path
		// (searched across every window).
		if (m_PendingPathTarget)
		{
			for (WindowContext* ctx : contexts)
			{
				Geometry rootGeom;
				rootGeom.LocalSize = { (float)ctx->ViewportW, (float)ctx->ViewportH };
				std::vector<Widget*> path;
				if (FindPathTo(ctx->Root.get(), rootGeom, m_PendingPathTarget, path))
				{
					for (Widget* a : path)
						if (a != m_PendingPathTarget)
							m_Expanded[a] = true;
					break;
				}
			}
			m_PendingPathTarget = nullptr;
		}

		for (size_t ci = 0; ci < contexts.size(); ci++)
		{
			WindowContext* ctx = contexts[ci];
			Geometry rootGeom;
			rootGeom.LocalSize = { (float)ctx->ViewportW, (float)ctx->ViewportH };
			CollectTree(ctx->Root.get(), rootGeom, 0, true, ci == 0 ? -1 : (int)ci);
		}

		if (m_Selected)
		{
			bool stillThere = false;
			for (auto& r : m_AllRects)
				if (r.Target == m_Selected) { stillThere = true; break; }
			if (!stillThere) m_Selected = nullptr;
		}
	}

	// Full-depth walk: every widget's geometry is recorded in m_AllRects
	// (for pick highlights); display rows only include widgets on the
	// expanded path of the tree. Non-default windows label their root row
	// with a window index so multiple "Overlay" roots stay distinguishable.
	void CollectTree(Widget* w, const Geometry& geom, int depth, bool displayable, int windowIndex = -1) const
	{
		if (!w || w == this) return;

		bool hasChildren = HasChildrenOf(w);
		bool expanded = false;
		if (hasChildren)
		{
			auto it = m_Expanded.find(w);
			expanded = it != m_Expanded.end() ? it->second : DefaultExpanded(w);
		}

		if (displayable)
		{
			Row row;
			row.Target = w;
			row.TypeName = (windowIndex >= 0 && depth == 0)
				? "[Win" + std::to_string(windowIndex) + "] " + w->GetTypeName()
				: w->GetTypeName();
			row.Depth = depth;
			row.Enabled = w->IsEnabled();
			row.Vis = w->GetVisibility();
			row.AbsPos = geom.LocalToAbsolute({0,0});
			row.AbsSize = geom.LocalSize;
			row.HasChildren = hasChildren;
			row.Expanded = expanded;
			m_Rows.push_back(row);
		}

		m_AllRects.push_back({w, geom.LocalToAbsolute({0,0}), geom.LocalSize, depth});

		if (hasChildren)
		{
			ArrangedChildren arranged;
			w->ArrangeChildren(geom, arranged);
			for (auto& c : arranged)
				CollectTree(c.WidgetPtr, c.Geom, depth + 1, displayable && expanded, windowIndex);
		}
	}

	// Depth-first path search mirroring CollectTree's walk (same skip rule,
	// same child enumeration) — returns the node sequence leaf → root.
	bool FindPathTo(Widget* node, const Geometry& geom, Widget* target, std::vector<Widget*>& out) const
	{
		if (!node || node == this) return false;
		if (node == target) { out.push_back(node); return true; }
		ArrangedChildren arranged;
		node->ArrangeChildren(geom, arranged);
		for (auto& c : arranged)
		{
			if (FindPathTo(c.WidgetPtr, c.Geom, target, out))
			{
				out.push_back(node);
				return true;
			}
		}
		return false;
	}

	void PaintText(DrawList& dl, int layer, const glm::vec2& pos, const std::string& text,
		const glm::vec4& tint, int clip, float scale = 1.0f) const
	{
		if (!m_FontCache || text.empty()) return;
		float px = pos.x, py = pos.y;
		const uint8_t* p = (const uint8_t*)text.data();
		const uint8_t* e = p + text.size();
		while (p < e)
		{
			uint32_t cp;
			if((*p&0x80)==0){cp=*p;p++;}
			else if((*p&0xE0)==0xC0&&p+1<e){cp=((*p&0x1F)<<6)|(p[1]&0x3F);p+=2;}
			else if((*p&0xF0)==0xE0&&p+2<e){cp=((*p&0x0F)<<12)|((p[1]&0x3F)<<6)|(p[2]&0x3F);p+=3;}
			else{p++;continue;}
			const GlyphInfo* gi = m_FontCache->GetGlyph(m_Font, cp);
			if(gi)
			{
				glm::vec2 uv[4]={{gi->AtlasU0,gi->AtlasV0},{gi->AtlasU1,gi->AtlasV0},{gi->AtlasU1,gi->AtlasV1},{gi->AtlasU0,gi->AtlasV1}};
				dl.AddText(layer, {px, py + gi->BearingY * scale}, std::string(1,(char)cp), uv,
					gi->AdvanceX * scale, gi->Height * scale, tint, clip);
				px += gi->AdvanceX * scale;
			}
		}
	}

	// Visual ink box of text relative to the pen origin, at the given scale.
	// Glyph quads are drawn at (penY + BearingY*scale), so ink spans
	// [min(BearingY*scale), max((BearingY+Height)*scale)] relative to penY.
	void MeasureInkBox(const std::string& text, float scale, float& outTop, float& outBottom) const
	{
		outTop = 0.0f;
		outBottom = 0.0f;
		if (!m_FontCache || text.empty()) return;
		bool first = true;
		const uint8_t* p = (const uint8_t*)text.data();
		const uint8_t* e = p + text.size();
		while (p < e)
		{
			uint32_t cp;
			if((*p&0x80)==0){cp=*p;p++;}
			else if((*p&0xE0)==0xC0&&p+1<e){cp=((*p&0x1F)<<6)|(p[1]&0x3F);p+=2;}
			else if((*p&0xF0)==0xE0&&p+2<e){cp=((*p&0x0F)<<12)|((p[1]&0x3F)<<6)|(p[2]&0x3F);p+=3;}
			else{p++;continue;}
			const GlyphInfo* gi = m_FontCache->GetGlyph(m_Font, cp);
			if (!gi) continue;
			float top = gi->BearingY * scale;
			float bottom = (gi->BearingY + gi->Height) * scale;
			if (first) { outTop = top; outBottom = bottom; first = false; }
			else { outTop = std::min(outTop, top); outBottom = std::max(outBottom, bottom); }
		}
	}

	FontCache* m_FontCache;
	FontHandle m_Font;
	float m_FontSize;

	mutable std::vector<Row> m_Rows;
	mutable std::vector<HlRect> m_AllRects;
	mutable std::vector<RowRect> m_RowRects;
	mutable std::vector<ModeRect> m_ModeRects;
	mutable std::unordered_map<Widget*, bool> m_Expanded;
	mutable float m_ScrollOffset = 0.0f;
	mutable float m_TreeHeight = 260.0f;
	mutable bool  m_ScrollToSelected = false;
	mutable Widget* m_Selected = nullptr;
	mutable Widget* m_PendingPathTarget = nullptr;
};

using SWidgetReflector = WidgetReflector;

} // namespace wisp
