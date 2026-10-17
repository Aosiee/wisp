#pragma once

#include "Wisp/Core.h"
#include "Wisp/DrawList.h"
#include "Wisp/FontCache.h"
#include "Wisp/Application.h"
#include "Wisp/Style.h"
#include "Wisp/Theme.h"
#include <functional>
#include <string>
#include <vector>

namespace wisp {

class Dropdown : public Widget
{
public:
	WISP_WIDGET_TYPE(Dropdown)

	Dropdown(FontCache* fc, FontHandle font, float fontSize)
		: m_FontCache(fc), m_Font(font), m_FontSize(fontSize)
	{
		SetSupportsFocus(true);
		m_BgStyle = PanelStyle();
		m_BgStyle.Background.Tint = { 0.12f, 0.12f, 0.18f, 1.0f };
	}

		// The section-header marker (the leading \x01 byte) - such an
		// option renders as a non-selectable dim header row in the popup
		// (the nested view-mode sections).
		static bool IsHeaderOption(const std::string& s) { return !s.empty() && s[0] == '\x01'; }
		static std::string HeaderLabel(const std::string& s) { return s.substr(1); }

		void SetOptions(const std::vector<std::string>& opts) { m_Options = opts; InvalidateLayout(); }
	void SetSelectedIndex(int idx) { m_SelectedIdx = idx; InvalidateLayout(); }
	int  GetSelectedIndex() const { return m_SelectedIdx; }
	const std::string& GetSelectedText() const { return m_Options.empty() ? m_Empty : m_Options[m_SelectedIdx]; }
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
		return { std::max(maxW + 30.0f, 120.0f), 24.0f };
	}

	void ArrangeChildren(const Geometry&, ArrangedChildren&) const override {}

	int OnPaint(const PaintArgs& args, const Geometry& geom, DrawList& drawList, int layerId, const WidgetStyleContext& style) const override
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

		glm::vec4 bg = m_IsHovered ? m_BgStyle.Background.Tint * 1.3f : m_BgStyle.Background.Tint;
		drawList.AddBox(layerId, absPos, {w,h}, bg, clipIdx);
		drawList.AddBox(layerId+1, absPos, {w,1}, glm::vec4(0.25f,0.25f,0.35f,1), clipIdx);
		drawList.AddBox(layerId+1, {absPos.x, absPos.y+h-1}, {w,1}, glm::vec4(0.25f,0.25f,0.35f,1), clipIdx);

		// Arrow indicator
		drawList.AddBox(layerId+2, {absPos.x+w-20, absPos.y+8}, {8,4}, glm::vec4(0.5f,0.5f,0.6f,1), clipIdx);

		// Selected text
		if (!m_Options.empty() && m_FontCache)
		{
			FontMetrics m = m_FontCache->GetMetrics(m_Font, m_FontSize);
			float tx = absPos.x + 6, ty = absPos.y + (h - m.LineHeight) * 0.5f;
			m_FontCache->MeasureText(m_Font, m_Options[m_SelectedIdx], m_FontSize);
			const uint8_t* p = (const uint8_t*)m_Options[m_SelectedIdx].data();
			const uint8_t* e = p + m_Options[m_SelectedIdx].size();
			while (p < e)
			{
				uint32_t cp;
				if((*p&0x80)==0){cp=*p;p++;}else if((*p&0xE0)==0xC0&&p+1<e){cp=((*p&0x1F)<<6)|(p[1]&0x3F);p+=2;}else if((*p&0xF0)==0xE0&&p+2<e){cp=((*p&0x0F)<<12)|((p[1]&0x3F)<<6)|(p[2]&0x3F);p+=3;}else{p++;continue;}
				const GlyphInfo* gi = m_FontCache->GetGlyph(m_Font, cp, m_FontSize);
				if(gi){glm::vec2 uv[4]={{gi->AtlasU0,gi->AtlasV0},{gi->AtlasU1,gi->AtlasV0},{gi->AtlasU1,gi->AtlasV1},{gi->AtlasU0,gi->AtlasV1}};drawList.AddText(layerId+3,{tx,ty+m.Ascent+gi->BearingY},std::string(1,(char)cp),uv,gi->AdvanceX,gi->Height,glm::vec4(1,1,1,1),clipIdx);tx+=gi->AdvanceX;}
			}
		}
		return layerId+4;
	}

	Reply OnMouseEnter() override { m_IsHovered = true; return Reply::Unhandled(); }
	Reply OnMouseLeave() override { m_IsHovered = false; return Reply::Unhandled(); }

	Reply OnMouseButtonDown(const glm::vec2&, int button) override
	{
		if (button != 0) return Reply::Unhandled();
		if (m_Options.empty()) return Reply::Handled_();

		auto list = std::make_unique<VerticalBox>(BoxOrientation::Vertical);
		for (int i = 0; i < (int)m_Options.size(); i++)
		{
			int idx = i;
			// The section headers: a dim, non-interactive row (the nested
			// view-mode sections) - the items below read indented.
			if (IsHeaderOption(m_Options[i]))
			{
				auto label = std::make_unique<TextBlock>(m_FontCache, m_Font, m_FontSize * 0.85f, HeaderLabel(m_Options[i]),
					glm::vec4(0.5f, 0.55f, 0.65f, 1.0f));
				list->AddChild(std::move(label));
				continue;
			}
			auto label = std::make_unique<TextBlock>(m_FontCache, m_Font, m_FontSize, m_Options[i],
				i == m_SelectedIdx ? glm::vec4(0.3f,0.7f,1.0f,1) : glm::vec4(0.8f,0.8f,0.8f,1));
			auto row = std::make_unique<Button>();
			row->SetNormalBrush({glm::vec4(0.08f,0.08f,0.12f,1)});
			row->SetHoveredBrush({glm::vec4(0.15f,0.15f,0.22f,1)});
			row->SetPressedBrush({glm::vec4(0.05f,0.05f,0.1f,1)});
			row->SetOnClicked([this, idx]() {
				SetSelectedIndex(idx);
				m_OnChanged(idx, m_Options[idx]);
				UIApplication::Get().RequestDismissPopups();
			});
			row->SetChild(std::move(label));
			list->AddChild(std::move(row));
		}

		auto popupBorder = std::make_unique<Border>();
		popupBorder->SetBrush({glm::vec4(0.08f,0.08f,0.12f,0.95f)});
		popupBorder->SetPadding(2);
		popupBorder->SetChild(std::move(list));

		auto sized = std::make_unique<SizeBox>();
		sized->SetWidthOverride(m_CachedWidth);
		sized->SetChild(std::move(popupBorder));

		UIApplication::Get().ShowPopupAt(std::move(sized), {m_CachedAbsX, m_CachedAbsY + GetCachedDesiredSize().y});
		return Reply::Handled_();
	}

	bool SupportsFocus() const override { return m_SupportsFocus; }
	
	Reply OnKeyDown(int key, int mods) override
	{
		if (key == 257 || key == 32) // Enter or Space
		{
			OnMouseButtonDown({0,0}, 0);
			return Reply::Handled_();
		}
		return Reply::Unhandled();
	}
private:
	FontCache* m_FontCache;
	FontHandle m_Font;
	float      m_FontSize;
	std::vector<std::string> m_Options;
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
