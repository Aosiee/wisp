#pragma once

#include "Wisp/Core.h"
#include "Wisp/DrawList.h"
#include "Wisp/FontCache.h"
#include <functional>
#include <string>
#include <vector>

namespace wisp {

class RadioButtons : public Widget
{
public:
	WISP_WIDGET_TYPE(RadioButtons)

	RadioButtons(FontCache* fc, FontHandle font, float fontSize)
		: m_FontCache(fc), m_Font(font), m_FontSize(fontSize)
	{
		SetSupportsFocus(true);
	}

	void SetOptions(const std::vector<std::string>& opts)
	{
		m_Options = opts;
		if (m_SelectedIdx >= (int)opts.size()) m_SelectedIdx = opts.empty() ? -1 : 0;
		InvalidateLayout();
	}

	void SetSelectedIndex(int idx)
	{
		if (idx >= (int)m_Options.size()) return;
		m_SelectedIdx = idx;
	}

	int GetSelectedIndex() const { return m_SelectedIdx; }
	void SetOnSelectionChanged(std::function<void(int)> cb) { m_OnChanged = std::move(cb); }

	glm::vec2 ComputeDesiredSize() const override
	{
		float maxW = 0;
		if (m_FontCache)
		{
			for (auto& o : m_Options)
				maxW = std::max(maxW, m_FontCache->MeasureText(m_Font, o, m_FontSize).x);
		}
		float rowH = m_FontSize + 8.0f;
		return { maxW + 24.0f, rowH * (float)std::max(1, (int)m_Options.size()) };
	}

	void ArrangeChildren(const Geometry&, ArrangedChildren&) const override {}

	int OnPaint(const PaintArgs& args, const Geometry& geom, DrawList& drawList, int layerId, const WidgetStyleContext&) const override
	{
		glm::vec2 pos = geom.LocalToAbsolute({0,0});
		m_CachedAbsPos = pos;
		m_CachedSize = geom.LocalSize;
		int clip = args.InheritedClipIndex;
		float rowH = m_FontSize + 8.0f;
		float circleR = std::min(8.0f, m_FontSize * 0.45f);

		for (size_t i = 0; i < m_Options.size(); i++)
		{
			float cy = pos.y + i * rowH + rowH * 0.5f;
			glm::vec4 ring = (int)i == m_SelectedIdx ? glm::vec4(0.3f,0.6f,0.9f,1) : glm::vec4(0.4f,0.4f,0.5f,1);
			drawList.AddBox(layerId, {pos.x + 2, cy - circleR}, {circleR*2, circleR*2}, ring, clip);
			drawList.AddBox(layerId+1, {pos.x + 4, cy - circleR + 2}, {(circleR-2)*2, (circleR-2)*2}, {0.05f,0.05f,0.09f,1}, clip);
			if ((int)i == m_SelectedIdx)
				drawList.AddBox(layerId+2, {pos.x + 5, cy - circleR + 3}, {(circleR-3)*2, (circleR-3)*2}, {0.8f,0.9f,1.0f,1}, clip);

			if (m_FontCache)
			{
				PaintText(drawList, layerId+1, {pos.x + circleR*2 + 8, cy - m_FontSize*0.5f}, m_Options[i],
					(int)i == m_SelectedIdx ? glm::vec4(0.9f,0.9f,1.0f,1) : glm::vec4(0.7f,0.7f,0.8f,1), clip);
			}
		}
		return layerId + 3;
	}

	Reply OnMouseButtonDown(const glm::vec2& absPos, int button) override
	{
		if (button != 0) return Reply::Unhandled();
		glm::vec2 pos = m_CachedAbsPos;
		float rowH = m_FontSize + 8.0f;
		float w = m_CachedSize.x;
		if (absPos.x < pos.x || absPos.x > pos.x + w || absPos.y < pos.y) return Reply::Unhandled();
		int idx = (int)((absPos.y - pos.y) / rowH);
		if (idx >= 0 && idx < (int)m_Options.size())
		{
			m_SelectedIdx = idx;
			if (m_OnChanged) m_OnChanged(idx);
			return Reply::Handled_();
		}
		return Reply::Unhandled();
	}

	bool SupportsFocus() const override { return true; }
	Reply OnKeyDown(int key, int) override
	{
		if (key == 262 || key == 264) // right / down
		{
			if (!m_Options.empty())
			{
				m_SelectedIdx = (m_SelectedIdx + 1) % (int)m_Options.size();
				if (m_OnChanged) m_OnChanged(m_SelectedIdx);
			}
			return Reply::Handled_();
		}
		if (key == 263 || key == 265) // left / up
		{
			if (!m_Options.empty())
			{
				m_SelectedIdx = (m_SelectedIdx - 1 + (int)m_Options.size()) % (int)m_Options.size();
				if (m_OnChanged) m_OnChanged(m_SelectedIdx);
			}
			return Reply::Handled_();
		}
		return Reply::Unhandled();
	}

	void ForEachChild(std::function<void(Widget*)>) override {}

private:
	void PaintText(DrawList& dl, int layer, const glm::vec2& pos, const std::string& text,
		const glm::vec4& tint, int clip) const
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
			const GlyphInfo* gi = m_FontCache->GetGlyph(m_Font, cp, m_FontSize);
			if(gi)
			{
				glm::vec2 uv[4]={{gi->AtlasU0,gi->AtlasV0},{gi->AtlasU1,gi->AtlasV0},{gi->AtlasU1,gi->AtlasV1},{gi->AtlasU0,gi->AtlasV1}};
				dl.AddText(layer, {px, py + gi->BearingY}, std::string(1,(char)cp), uv, gi->AdvanceX, gi->Height, tint, clip);
				px += gi->AdvanceX;
			}
		}
	}

	FontCache* m_FontCache;
	FontHandle m_Font;
	float m_FontSize;
	std::vector<std::string> m_Options;
	int m_SelectedIdx = 0;
	std::function<void(int)> m_OnChanged;
	mutable glm::vec2 m_CachedAbsPos{0,0};
	mutable glm::vec2 m_CachedSize{0,0};
};

using SRadioButtons = RadioButtons;

} // namespace wisp
