#pragma once

#include "Wisp/Core.h"
#include "Wisp/DrawList.h"
#include "Wisp/FontCache.h"
#include "Wisp/Application.h"
#include "Wisp/Style.h"
#include "Wisp/Theme.h"
#include "Wisp/Composites/PopupMenu.h"
#include "Wisp/Components/Button.h"
#include "Wisp/Components/TextBlock.h"
#include "Wisp/Icons.h"
#include "Wisp/StyleTokens.h"
#include <functional>
#include <string>
#include <vector>

namespace wisp {

	// A hover-opened submenu row (the tree dropdown's branches).
	class SubmenuRow : public Button
	{
	public:
		WISP_WIDGET_TYPE(SubmenuRow)
		// The enter callback receives the row's absolute position (cached
		// at paint) so the submenu opens beside the HOVERED row, not a
		// fixed offset.
		void SetOnEntered(std::function<void(const glm::vec2& absPos)> fn) { m_OnEntered = std::move(fn); }
		Reply OnMouseEnter() override
		{
			Button::OnMouseEnter();
			if (m_OnEntered)
				m_OnEntered(m_CachedAbsPos);
			return Reply::Unhandled();
		}
		int OnPaint(const PaintArgs& args, const Geometry& geom, DrawList& drawList, int layerId, const WidgetStyleContext& style) const override
		{
			m_CachedAbsPos = geom.LocalToAbsolute({ 0, 0 });
			return Button::OnPaint(args, geom, drawList, layerId, style);
		}
	private:
		std::function<void(const glm::vec2& absPos)> m_OnEntered;
		mutable glm::vec2 m_CachedAbsPos{ 0.0f };
	};

	// The tree model (the nested view-mode sections - the | -categorized
	// paths): the branches open the hover submenus, the leaves select.
	struct DropdownNode
	{
		std::string Label;
		std::string Value;
		bool IsBranch = false;
		std::vector<DropdownNode> Children;

		bool operator==(const DropdownNode& other) const
		{
			return Label == other.Label && Value == other.Value && IsBranch == other.IsBranch && Children == other.Children;
		}
	};

	class Dropdown : public Widget
	{
	public:
	WISP_WIDGET_TYPE(Dropdown)

	Dropdown(FontCache* fc, FontHandle font, float fontSize)
		: m_FontCache(fc), m_Font(font), m_FontSize(fontSize)
	{
		SetSupportsFocus(true);
		m_BgStyle = PanelStyle();
		m_BgStyle.Background.Tint = tokens::Field;
	}

	// The section-header marker (the leading \x01 byte) - such an
	// option renders as a non-selectable dim header row in the popup
	// (the legacy flat sections; the tree options supersede it).
	static bool IsHeaderOption(const std::string& s) { return !s.empty() && s[0] == '\x01'; }
	static std::string HeaderLabel(const std::string& s) { return s.substr(1); }

	void SetOptions(const std::vector<std::string>& opts) { m_Options = opts; m_Nodes.clear(); InvalidateLayout(); }
	// The tree options (the nested sections): the flat m_Options mirrors
	// the leaves for the selected-text display, in the tree order.
	void SetTreeOptions(const std::vector<DropdownNode>& nodes)
	{
		m_Nodes = nodes;
		m_Options.clear();
		m_NodeValues.clear();
		for (const DropdownNode& node : m_Nodes)
			CollectLeafLabels(node, m_Options, m_NodeValues);
		InvalidateLayout();
	}
	void SetSelectedIndex(int idx) { m_SelectedIdx = idx; InvalidateLayout(); }
	void SetSelectedValue(const std::string& value)
	{
		for (size_t i = 0; i < m_Options.size(); i++)
			if (m_NodeValues[i] == value)
			{
				m_SelectedIdx = (int)i;
				break;
			}
		InvalidateLayout();
	}
	int  GetSelectedIndex() const { return m_SelectedIdx; }
	const std::string& GetSelectedText() const { return m_Options.empty() ? m_Empty : m_Options[m_SelectedIdx]; }
	// The selected leaf's VALUE (the tree's view ID - empty for the flat
	// options).
	std::string GetSelectedValue() const
	{
		return (m_SelectedIdx >= 0 && m_SelectedIdx < (int)m_NodeValues.size()) ? m_NodeValues[m_SelectedIdx] : std::string();
	}
	void SetOnSelectionChanged(std::function<void(int, const std::string&)> cb) { m_OnChanged = std::move(cb); }

	glm::vec2 ComputeDesiredSize() const override
	{
		if (!m_FontCache) return { 120, 24 };
		float maxW = 0;
		for (auto& o : m_Options)
		{
			auto s = m_FontCache->MeasureText(m_Font, o, m_FontSize);
			maxW = std::max(maxW, s.x);
		}
		return { std::max(maxW + 36.0f, 120.0f), tokens::FieldHeight + 2.0f };
	}

	void ArrangeChildren(const Geometry&, ArrangedChildren&) const override {}

	int OnPaint(const PaintArgs& args, const Geometry& geom, DrawList& drawList, int layerId, const WidgetStyleContext&) const override
	{
		if (m_StyleDirty)
		{
			m_BgStyle = ResolveStyle(this, m_BgStyle);
			m_StyleDirty = false;
		}

		glm::vec2 absPos = geom.LocalToAbsolute({0,0});
		m_CachedAbsX = absPos.x;
		m_CachedAbsY = absPos.y;
		m_CachedWidth = geom.LocalSize.x;
		float w = geom.LocalSize.x, h = geom.LocalSize.y;
		int clipIdx = args.InheritedClipIndex;

		// The field look + a chevron icon.
		const glm::vec4 bg = m_IsHovered ? tokens::FieldHover : m_BgStyle.Background.Tint;
		const float r = tokens::RadiusSmall;
		const float rr[4] = { r, r, r, r };
		drawList.AddRoundedBox(layerId, absPos, { w, h }, bg, rr, 1.0f,
			(UIApplication::IsValid() && UIApplication::Get().GetFocusWidget() == this) ? tokens::Accent : (m_IsHovered ? tokens::BorderStrong : tokens::Border), clipIdx);

		if (m_FontCache)
		{
			const FontHandle iconFont = UIApplication::FontOr(FontRole::Icons, m_Font);
			const float iconPx = 13.0f;
			const FontMetrics im = m_FontCache->GetMetrics(iconFont, iconPx);
			popupmenu::PaintRun(drawList, m_FontCache, iconFont, iconPx,
				{ std::round(absPos.x + w - 20.0f), std::round(absPos.y + (h - im.LineHeight) * 0.5f + im.Ascent) },
				icons::ChevronDown, m_IsHovered ? tokens::TextPrimary : tokens::TextSecondary, layerId + 2, clipIdx);
		}

		// Selected text (truncated against the chevron)
		if (!m_Options.empty() && m_FontCache)
		{
			FontMetrics m = m_FontCache->GetMetrics(m_Font, m_FontSize);
			const float ty = std::round(absPos.y + (h - m.LineHeight) * 0.5f + m.Ascent);
			std::string text = m_Options[m_SelectedIdx];
			const float limit = w - 32.0f;
			if (limit > 0.0f && m_FontCache->MeasureText(m_Font, text, m_FontSize).x > limit)
			{
				while (!text.empty() && m_FontCache->MeasureText(m_Font, text + "...", m_FontSize).x > limit)
					text.pop_back();
				text += "...";
			}
			popupmenu::PaintRun(drawList, m_FontCache, m_Font, m_FontSize, { std::round(absPos.x + 8.0f), ty },
				text, tokens::TextPrimary, layerId + 3, clipIdx);
		}
		return layerId+4;
	}

	Reply OnMouseEnter() override { m_IsHovered = true; return Reply::Unhandled(); }
	Reply OnMouseLeave() override { m_IsHovered = false; return Reply::Unhandled(); }

	Reply OnMouseButtonDown(const glm::vec2&, int button) override
	{
		if (button != 0) return Reply::Unhandled();
		if (m_Options.empty() && m_Nodes.empty()) return Reply::Handled_();

		std::unique_ptr<Widget> list;
		if (!m_Nodes.empty())
		{
			// The tree popup (the nested sections - the branches open the
			// hover submenus).
			auto leafIndex = std::make_shared<int>(0);
			list = BuildTreePopup(m_Nodes, leafIndex, m_CachedAbsX, 1);
		}
		else
		{
			// The typeable list (every flat dropdown filters as you type,
			// like the popup menus).
			std::vector<PopupMenuEntry> entries;
			for (int i = 0; i < (int)m_Options.size(); i++)
			{
				int idx = i;
				// The section headers: a dim, non-interactive row (the
				// legacy flat sections) - the items below read indented.
				if (IsHeaderOption(m_Options[i]))
				{
					entries.push_back(PopupMenuEntry::MakeHeader(HeaderLabel(m_Options[i])));
					continue;
				}
				PopupMenuEntry e;
				e.Label = m_Options[i];
				e.Checkable = true;
				e.Checked = (i == m_SelectedIdx);
				e.Action = [this, idx]() {
					SetSelectedIndex(idx);
					m_OnChanged(idx, m_Options[idx]);
				};
				entries.push_back(std::move(e));
			}
			list = std::make_unique<FilterablePopupList>(m_FontCache, m_Font, m_FontSize, std::move(entries));
		}

		auto sized = std::make_unique<PopupFrame>(std::move(list), std::max(m_CachedWidth - popupmenu::kPad * 2.0f, 140.0f));
		// The popup anchors below the dropdown's own height (the cached
		// desired size is zero until the first arrangement - the
		// ComputeDesiredSize's floor covers the click-time case).
		float anchorY = m_CachedAbsY + (GetCachedDesiredSize().y > 0.0f ? GetCachedDesiredSize().y : ComputeDesiredSize().y);
		UIApplication::Get().ShowPopupAt(std::move(sized), {m_CachedAbsX, anchorY + 2.0f});
		return Reply::Handled_();
	}

	bool SupportsFocus() const override { return m_SupportsFocus; }
	
	Reply OnKeyDown(int key, int) override
	{
		if (key == 257 || key == 32) // Enter or Space
		{
			OnMouseButtonDown({0,0}, 0);
			return Reply::Handled_();
		}
		return Reply::Unhandled();
	}
private:
	// The recursive tree popup (the branches open the hover submenus).
	// anchorX = the level's popup's left edge (the submenus cascade
	// right). keepCount = this level's popup depth (the main popup = 1):
	// a branch's hover keeps the popups through ITS depth (its own
	// ancestors) and replaces the deeper siblings - dismissing to 1
	// unconditionally would destroy the hovered row's own submenu (the
	// nested-branch crash).
	std::unique_ptr<Widget> BuildTreePopup(const std::vector<DropdownNode>& nodes, const std::shared_ptr<int>& leafIndex, float anchorX, int keepCount)
	{
		auto list = std::make_unique<VerticalBox>(BoxOrientation::Vertical);
		for (const DropdownNode& node : nodes)
		{
			if (node.IsBranch)
			{
				auto row = std::make_unique<SubmenuRow>();
				row->SetNormalBrush({ tokens::WithAlpha(tokens::Popup, 0.0f) });
				row->SetHoveredBrush({ tokens::Hover });
				row->SetPressedBrush({ tokens::Pressed });
				row->SetContentPadding({ popupmenu::kLeadColumn, 5.0f, 8.0f, 5.0f });
				{
					auto rowBox = std::make_unique<HorizontalBox>(BoxOrientation::Horizontal);
					*rowBox
						+ rowBox->AddSlot().Fill(1.0f)[ std::make_unique<TextBlock>(m_FontCache, m_Font, m_FontSize, node.Label, tokens::TextPrimary) ]
						+ rowBox->AddSlot()[ std::make_unique<TextBlock>(m_FontCache, m_Font, m_FontSize, icons::ChevronRight, tokens::TextMuted) ];
					row->SetChild(std::move(rowBox));
				}
				row->SetOnEntered([this, node, leafIndex, anchorX, keepCount](const glm::vec2& rowAbsPos) {
					// The new submenu replaces the previously hovered one
					// at THIS depth - the ancestor popups (the rows above
					// this level) stay, the deeper siblings go.
					UIApplication::Get().DismissPopupsBeyond(keepCount);
					auto subList = BuildTreePopup(node.Children, leafIndex, anchorX + m_CachedWidth, keepCount + 1);
					auto sized = std::make_unique<PopupFrame>(std::move(subList), std::max(m_CachedWidth - popupmenu::kPad * 2.0f, 140.0f));
					// The submenu opens beside this level's popup at the
					// HOVERED row's height (the cascade).
					UIApplication::Get().ShowPopupAt(std::move(sized), { anchorX + m_CachedWidth, rowAbsPos.y });
				});
				list->AddChild(std::move(row));
			}
			else
			{
				// The leaf's TREE-order index (the m_NodeValues parallel of
				// m_Options - an open-order counter would misalign the
				// selected row when the submenus open out of tree order).
				int idx = -1;
				for (size_t k = 0; k < m_NodeValues.size(); k++)
					if (m_NodeValues[k] == node.Value) { idx = (int)k; break; }
				if (idx < 0)
					idx = (*leafIndex)++;
				PopupMenuEntry e;
				e.Label = node.Label;
				e.Checkable = true;
				e.Checked = (idx == m_SelectedIdx);
				e.Action = [this, idx, node]() {
					SetSelectedIndex(idx);
					m_OnChanged(idx, node.Value);
				};
				list->AddChild(std::make_unique<PopupMenuRow>(m_FontCache, m_Font, m_FontSize, std::move(e)));
			}
		}
		return list;
	}

	static void CollectLeafLabels(const DropdownNode& node, std::vector<std::string>& labels, std::vector<std::string>& values)
	{
		if (!node.IsBranch)
		{
			labels.push_back(node.Label);
			values.push_back(node.Value);
			return;
		}
		for (const DropdownNode& child : node.Children)
			CollectLeafLabels(child, labels, values);
	}

	FontCache* m_FontCache;
	FontHandle m_Font;
	float      m_FontSize;
	std::vector<std::string> m_Options;
	std::vector<std::string> m_NodeValues; // the leaves' values (parallel to m_Options)
	std::vector<DropdownNode> m_Nodes;
	int        m_SelectedIdx = 0;
	std::string m_Empty;
	mutable bool m_IsHovered = false;
	mutable float m_CachedAbsX = 0.0f;
	mutable float m_CachedAbsY = 0.0f;
	mutable float m_CachedWidth = 0.0f;
	mutable PanelStyle m_BgStyle;
	std::function<void(int, const std::string&)> m_OnChanged;
};

using SDropdown = Dropdown;

} // namespace wisp
