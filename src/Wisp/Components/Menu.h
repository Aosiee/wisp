#pragma once

#include "Wisp/Core.h"
#include "Wisp/DrawList.h"
#include "Wisp/FontCache.h"
#include "Wisp/Application.h"
#include "Wisp/Components/Button.h"
#include "Wisp/Components/TextBlock.h"
#include "Wisp/Components/Border.h"
#include "Wisp/Components/BoxPanel.h"
#include <functional>
#include <string>
#include <vector>

namespace wisp {

struct MenuItem
{
	std::string Label;
	std::string Shortcut;
	std::function<void()> OnClick;
	std::shared_ptr<class Menu> SubMenu;
	bool bSeparator = false;
	bool bEnabled = true;
};

class Menu : public Widget
{
public:
	WISP_WIDGET_TYPE(Menu)

	Menu(FontCache* fc, FontHandle font, float fontSize)
		: m_FontCache(fc), m_Font(font), m_FontSize(fontSize) {}

	MenuItem& AddItem(const std::string& label, std::function<void()> onClick = nullptr)
	{
		m_Items.push_back({label, "", std::move(onClick)});
		return m_Items.back();
	}

	MenuItem& AddSeparator()
	{
		m_Items.push_back({"", "", nullptr});
		m_Items.back().bSeparator = true;
		return m_Items.back();
	}

	void AddSubMenu(const std::string& label, std::shared_ptr<Menu> sub)
	{
		m_Items.push_back({label, "", nullptr, std::move(sub)});
	}

	glm::vec2 ComputeDesiredSize() const override
	{
		float maxW = 0, totalH = 0;
		for (auto& item : m_Items)
		{
			if (item.bSeparator) { totalH += 6; continue; }
			float w = m_FontCache->MeasureText(m_Font, item.Label, m_FontSize).x;
			float shortcutW = item.Shortcut.empty() ? 0 : m_FontCache->MeasureText(m_Font, item.Shortcut, m_FontSize).x + 40;
			maxW = std::max(maxW, w + shortcutW);
			totalH += m_FontSize * 1.8f;
		}
		return { maxW + 60, std::max(totalH + 8, 4.0f) };
	}

	void ArrangeChildren(const Geometry&, ArrangedChildren&) const override {}

	int OnPaint(const PaintArgs& args, const Geometry& geom, DrawList& drawList, int layerId, const WidgetStyleContext& style) const override
	{
		glm::vec2 absPos = geom.LocalToAbsolute({0, 0});
		float w = geom.LocalSize.x;
		int clipIdx = args.InheritedClipIndex;

		drawList.AddBox(layerId, absPos, {w, geom.LocalSize.y}, glm::vec4(0.1f, 0.1f, 0.16f, 0.97f), clipIdx);

		float y = absPos.y + 4;
		int rowLayer = layerId + 1;
		for (size_t i = 0; i < m_Items.size(); i++)
		{
			auto& item = m_Items[i];
			if (item.bSeparator)
			{
				drawList.AddBox(rowLayer, {absPos.x + 8, y + 2}, {w - 16, 1}, glm::vec4(0.25f, 0.25f, 0.35f, 1), clipIdx);
				y += 6;
				continue;
			}

			float rowH = m_FontSize * 1.8f;
			bool hovered = ((int)i == m_HoveredIdx);
			if (hovered && item.bEnabled)
				drawList.AddBox(rowLayer, {absPos.x + 2, y}, {w - 4, rowH}, glm::vec4(0.25f, 0.5f, 0.8f, 0.8f), clipIdx);

			if (m_FontCache)
			{
				glm::vec4 c = item.bEnabled ? glm::vec4(0.9f, 0.9f, 0.95f, 1) : glm::vec4(0.4f, 0.4f, 0.5f, 1);
				PaintText(drawList, item.Label, absPos.x + 10, y + 4, c, rowLayer + 1, clipIdx);

				if (!item.Shortcut.empty())
					PaintText(drawList, item.Shortcut, absPos.x + w - 50, y + 4, glm::vec4(0.5f, 0.5f, 0.6f, 1), rowLayer + 1, clipIdx);

				if (item.SubMenu)
				{
					PaintText(drawList, ">", absPos.x + w - 18, y + 4, glm::vec4(0.5f, 0.5f, 0.6f, 1), rowLayer + 1, clipIdx);
				}
			}
			y += rowH;
		}
		return rowLayer + 2;
	}

	Reply OnMouseMove(const glm::vec2& screenPos) override
	{
		m_CachedAbsX = 0; // updated during paint
		return Reply::Unhandled();
	}

	Reply OnMouseButtonDown(const glm::vec2& screenPos, int button) override
	{
		if (button != 0) return Reply::Unhandled();
		int idx = HitTest(screenPos);
		if (idx >= 0 && idx < (int)m_Items.size() && m_Items[idx].bEnabled && !m_Items[idx].bSeparator)
		{
			if (m_Items[idx].OnClick)
			{
				m_Items[idx].OnClick();
				UIApplication::Get().RequestDismissPopups();
			}
		}
		return Reply::Handled_();
	}

	int HitTest(const glm::vec2& screenPos) const
	{
		float y = m_CachedAbsY + 4;
		for (size_t i = 0; i < m_Items.size(); i++)
		{
			auto& item = m_Items[i];
			float rowH = item.bSeparator ? 6.0f : m_FontSize * 1.8f;
			if (screenPos.y >= y && screenPos.y < y + rowH && screenPos.x >= m_CachedAbsX && screenPos.x < m_CachedAbsX + m_CachedWidth)
				return (int)i;
			y += rowH;
		}
		return -1;
	}

	void ShowAt(float x, float y)
	{
		m_CachedAbsX = x;
		m_CachedAbsY = y;
		m_CachedWidth = ComputeDesiredSize().x;
		auto wrapper = std::make_unique<Border>();
		auto menuPtr = this;
		// We show the menu itself as a popup
	}

	std::vector<MenuItem>& GetItems() { return m_Items; }

private:
	void PaintText(DrawList& dl, const std::string& text, float x, float y, const glm::vec4& col, int layer, int clip) const
	{
		FontMetrics m = m_FontCache->GetMetrics(m_Font, m_FontSize);
		const uint8_t* p = (const uint8_t*)text.data();
		const uint8_t* e = p + text.size();
		float px = x;
		while (p < e)
		{
			uint32_t cp;
			if((*p&0x80)==0){cp=*p;p++;}else if((*p&0xE0)==0xC0&&p+1<e){cp=((*p&0x1F)<<6)|(p[1]&0x3F);p+=2;}else if((*p&0xF0)==0xE0&&p+2<e){cp=((*p&0x0F)<<12)|((p[1]&0x3F)<<6)|(p[2]&0x3F);p+=3;}else{p++;continue;}
			const GlyphInfo* g = m_FontCache->GetGlyph(m_Font, cp, m_FontSize);
			if(g){glm::vec2 uv[4]={{g->AtlasU0,g->AtlasV0},{g->AtlasU1,g->AtlasV0},{g->AtlasU1,g->AtlasV1},{g->AtlasU0,g->AtlasV1}};dl.AddText(layer,{px+g->BearingX,y+m.Ascent+g->BearingY},std::string(1,(char)cp),uv,g->AdvanceX,g->Height,col,clip);px+=g->AdvanceX;}
		}
	}

	FontCache* m_FontCache;
	FontHandle m_Font;
	float m_FontSize = 14;
	std::vector<MenuItem> m_Items;
	int m_HoveredIdx = -1;
	mutable float m_CachedAbsX = 0, m_CachedAbsY = 0, m_CachedWidth = 0;
};

struct MenuBarEntry
{
	std::string Label;
	std::shared_ptr<Menu> MenuPtr;
};

class MenuBar : public Widget
{
public:
	WISP_WIDGET_TYPE(MenuBar)

	MenuBar(FontCache* fc, FontHandle font, float fontSize)
		: m_FontCache(fc), m_Font(font), m_FontSize(fontSize) {}

	void AddMenu(const std::string& label, std::shared_ptr<Menu> menu)
	{
		m_Entries.push_back({label, std::move(menu)});
	}

	glm::vec2 ComputeDesiredSize() const override
	{
		float w = 0;
		for (auto& e : m_Entries)
			w += m_FontCache->MeasureText(m_Font, e.Label, m_FontSize).x + 24;
		return { std::max(w, 100.0f), m_FontSize * 1.8f + 4 };
	}

	void ArrangeChildren(const Geometry&, ArrangedChildren&) const override {}

	int OnPaint(const PaintArgs& args, const Geometry& geom, DrawList& drawList, int layerId, const WidgetStyleContext& style) const override
	{
		glm::vec2 absPos = geom.LocalToAbsolute({0, 0});
		float w = geom.LocalSize.x, h = geom.LocalSize.y;
		int clipIdx = args.InheritedClipIndex;

		drawList.AddBox(layerId, absPos, {w, h}, glm::vec4(0.08f, 0.08f, 0.14f, 1), clipIdx);

		m_CachedAbsX = absPos.x;
		m_CachedAbsY = absPos.y;
		m_CachedH = h;

		float x = absPos.x + 4;
		for (size_t i = 0; i < m_Entries.size(); i++)
		{
			float tw = m_FontCache->MeasureText(m_Font, m_Entries[i].Label, m_FontSize).x;
			bool hovered = ((int)i == m_HoveredIdx);
			if (hovered)
				drawList.AddBox(layerId + 1, {x - 2, absPos.y}, {tw + 20, h}, glm::vec4(0.2f, 0.35f, 0.6f, 1), clipIdx);

			glm::vec4 c = hovered ? glm::vec4(1,1,1,1) : glm::vec4(0.8f, 0.8f, 0.85f, 1);
			FontMetrics m = m_FontCache->GetMetrics(m_Font, m_FontSize);
			const uint8_t* p = (const uint8_t*)m_Entries[i].Label.data();
			const uint8_t* e = p + m_Entries[i].Label.size();
			float px = x + 8;
			float ty = absPos.y + (h - m.LineHeight) * 0.5f;
			while (p < e)
			{
				uint32_t cp;
				if((*p&0x80)==0){cp=*p;p++;}else if((*p&0xE0)==0xC0&&p+1<e){cp=((*p&0x1F)<<6)|(p[1]&0x3F);p+=2;}else if((*p&0xF0)==0xE0&&p+2<e){cp=((*p&0x0F)<<12)|((p[1]&0x3F)<<6)|(p[2]&0x3F);p+=3;}else{p++;continue;}
				const GlyphInfo* g = m_FontCache->GetGlyph(m_Font, cp, m_FontSize);
				if(g){glm::vec2 uv[4]={{g->AtlasU0,g->AtlasV0},{g->AtlasU1,g->AtlasV0},{g->AtlasU1,g->AtlasV1},{g->AtlasU0,g->AtlasV1}};drawList.AddText(layerId+3,{px+g->BearingX,ty+m.Ascent+g->BearingY},std::string(1,(char)cp),uv,g->AdvanceX,g->Height,c,clipIdx);px+=g->AdvanceX;}
			}

			x += tw + 24;
		}
		m_EntryRightEdge = x;
		return layerId + 4;
	}

	Reply OnMouseMove(const glm::vec2& screenPos) override
	{
		m_HoveredIdx = HitTestHeader(screenPos);
		return Reply::Unhandled();
	}

	Reply OnMouseButtonDown(const glm::vec2& screenPos, int button) override
	{
		if (button != 0) return Reply::Unhandled();
		int idx = HitTestHeader(screenPos);
		if (idx >= 0 && idx < (int)m_Entries.size())
		{
			float x = GetHeaderX(idx);
			ShowMenuPopup(idx, x, m_CachedAbsY + m_CachedH);
		}
		return Reply::Handled_();
	}

private:
	int HitTestHeader(const glm::vec2& screenPos) const
	{
		float x = m_CachedAbsX + 4;
		for (size_t i = 0; i < m_Entries.size(); i++)
		{
			float tw = m_FontCache->MeasureText(m_Font, m_Entries[i].Label, m_FontSize).x + 20;
			if (screenPos.x >= x && screenPos.x < x + tw && screenPos.y >= m_CachedAbsY && screenPos.y < m_CachedAbsY + m_CachedH)
				return (int)i;
			x += tw + 4;
		}
		return -1;
	}

	float GetHeaderX(int idx) const
	{
		float x = m_CachedAbsX + 4;
		for (int i = 0; i < idx; i++)
			x += m_FontCache->MeasureText(m_Font, m_Entries[i].Label, m_FontSize).x + 24;
		return x;
	}

	void ShowMenuPopup(int idx, float x, float y)
	{
		if (idx < 0 || idx >= (int)m_Entries.size()) return;
		auto& entry = m_Entries[idx];
		auto& menu = entry.MenuPtr;
		auto items = menu->GetItems();

		auto list = std::make_unique<VerticalBox>(BoxOrientation::Vertical);
		for (auto& item : items)
		{
			if (item.bSeparator)
			{
				auto sep = std::make_unique<Border>();
				sep->SetBrush({glm::vec4(0.25f, 0.25f, 0.35f, 1)});
				auto sepSize = std::make_unique<SizeBox>();
				sepSize->SetHeightOverride(1.0f);
				sepSize->SetChild(std::move(sep));
				list->AddChild(std::move(sepSize));
				continue;
			}

			auto row = std::make_unique<Button>();
			row->SetNormalBrush({glm::vec4(0.1f, 0.1f, 0.16f, 1)});
			row->SetHoveredBrush({glm::vec4(0.25f, 0.5f, 0.8f, 0.8f)});
			row->SetPressedBrush({glm::vec4(0.15f, 0.3f, 0.5f, 1)});

			auto label = std::make_unique<TextBlock>(m_FontCache, m_Font, m_FontSize, item.Label, glm::vec4(0.9f, 0.9f, 0.95f, 1));
			auto hbox = std::make_unique<HorizontalBox>(BoxOrientation::Horizontal);
			hbox->AddChild(std::move(label));

			if (!item.Shortcut.empty())
			{
				auto sc = std::make_unique<TextBlock>(m_FontCache, m_Font, m_FontSize, item.Shortcut, glm::vec4(0.5f, 0.5f, 0.6f, 1));
				hbox->AddChild(std::move(sc));
			}
			if (item.SubMenu)
			{
				auto arrow = std::make_unique<TextBlock>(m_FontCache, m_Font, m_FontSize, ">", glm::vec4(0.5f, 0.5f, 0.6f, 1));
				hbox->AddChild(std::move(arrow));
			}

			row->SetChild(std::move(hbox));
			auto onClick = item.OnClick;
			row->SetOnClicked([onClick]() {
				// Deferred dismissal: the click handler only REQUESTS it -
				// DismissPopups destroys this Button, whose own code runs
				// after the callback returns.
				if (onClick) onClick();
				UIApplication::Get().RequestDismissPopups();
			});
			list->AddChild(std::move(row));
		}

		auto border = std::make_unique<Border>();
		border->SetBrush({glm::vec4(0.1f, 0.1f, 0.16f, 0.97f)});
		border->SetPadding(2);
		border->SetChild(std::move(list));

		float menuW = 0;
		for (auto& item : items)
		{
			if (item.bSeparator) continue;
			float w = m_FontCache->MeasureText(m_Font, item.Label, m_FontSize).x + 40;
			if (!item.Shortcut.empty()) w += m_FontCache->MeasureText(m_Font, item.Shortcut, m_FontSize).x + 20;
			if (item.SubMenu) w += 20;
			menuW = std::max(menuW, w);
		}
		menuW = std::max(menuW, 100.0f);

		auto sized = std::make_unique<SizeBox>();
		sized->SetWidthOverride(menuW);
		sized->SetChild(std::move(border));

		UIApplication::Get().ShowPopupAt(std::move(sized), {x, y});
	}

	FontCache* m_FontCache;
	FontHandle m_Font;
	float m_FontSize = 14;
	std::vector<MenuBarEntry> m_Entries;
	int m_HoveredIdx = -1;
	mutable float m_CachedAbsX = 0, m_CachedAbsY = 0, m_CachedH = 0, m_EntryRightEdge = 0;
};

using SMenu = Menu;
using SMenuBar = MenuBar;

} // namespace wisp
