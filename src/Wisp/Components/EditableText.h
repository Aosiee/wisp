#pragma once

#include "Wisp/Core.h"
#include "Wisp/DrawList.h"
#include "Wisp/FontCache.h"
#include "Wisp/Application.h"
#include "Wisp/Clipboard.h"
#include "Wisp/Style.h"
#include "Wisp/Theme.h"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <functional>
#include <string>

namespace wisp {

class EditableText : public Widget
{
public:
	WISP_WIDGET_TYPE(EditableText)

	EditableText(FontCache* fc, FontHandle font, float fontSize, const std::string& text = "")
		: m_FontCache(fc), m_Font(font), m_FontSize(fontSize), m_Text(text), m_CaretPos((int)text.size()), m_Anchor(m_CaretPos)
	{
		SetCursor(CursorType::TextEditBeam);
		SetSupportsFocus(true);
		m_Style = GetDefaultStyle();
	}

	static EditableTextStyle GetDefaultStyle()
	{
		EditableTextStyle s;
		s.Background.Tint         = { 0.08f, 0.08f, 0.12f, 1.0f };
		s.HoveredBackground.Tint  = { 0.1f, 0.1f, 0.15f, 1.0f };
		s.FocusedBackground.Tint  = { 0.12f, 0.12f, 0.18f, 1.0f };
		return s;
	}

	const std::string& GetText() const { return m_Text; }
	void SetText(const std::string& t) { m_Text = t; m_CaretPos = (int)t.size(); m_Anchor = m_CaretPos; InvalidateLayout(); }
	void SetOnTextChanged(std::function<void(const std::string&)> cb) { m_OnTextChanged = std::move(cb); }
	void SetReadOnly(bool ro) { m_ReadOnly = ro; }
	void SetWrapMode(WrapMode m) { m_WrapMode = m; InvalidateLayout(); }
	void SetAutoWrap(bool b) { m_AutoWrap = b; InvalidateLayout(); }

	glm::vec2 ComputeDesiredSize() const override
	{
		if (!m_FontCache) return { 100, 20 };
		float wrapW = -1.0f;
		if (m_WrapMode != WrapMode::None)
			wrapW = m_AutoWrap ? m_CachedWrapWidth : -1.0f;
		auto s = m_FontCache->MeasureText(m_Font, m_Text.empty() ? " " : m_Text, m_FontSize, wrapW, m_WrapMode);
		return { std::max(s.x + 16.0f, 100.0f), std::max(s.y + 8.0f, 20.0f) };
	}

	void ArrangeChildren(const Geometry&, ArrangedChildren&) const override {}

	int OnPaint(const PaintArgs& args, const Geometry& geom, DrawList& drawList, int layerId, const WidgetStyleContext& style) const override
	{
		if (m_StyleDirty)
		{
			m_Style = ResolveStyle(this, GetDefaultStyle());
			m_StyleDirty = false;
		}
		glm::vec2 absPos = geom.LocalToAbsolute({ 0, 0 });
		m_CachedAbsX = absPos.x;
		m_CachedAbsY = absPos.y;
		float w = geom.LocalSize.x, h = geom.LocalSize.y;
		int clipIdx = args.InheritedClipIndex;

		glm::vec4 bgCol = HasFocus() ? m_Style.FocusedBackground.Tint
			: (m_IsHovered ? m_Style.HoveredBackground.Tint : m_Style.Background.Tint);
		drawList.AddBox(layerId, absPos, { w, h }, bgCol, clipIdx);
		drawList.AddBox(layerId + 1, absPos, { w, 1 }, glm::vec4(0.2f, 0.2f, 0.3f, 1.0f), clipIdx);
		drawList.AddBox(layerId + 1, { absPos.x, absPos.y + h - 1 }, { w, 1 }, glm::vec4(0.2f, 0.2f, 0.3f, 1.0f), clipIdx);

		if (!m_FontCache || m_Text.empty()) return layerId + 4;

		FontMetrics metrics = m_FontCache->GetMetrics(m_Font, m_FontSize);
		float startX = std::floor(absPos.x + 4.0f);
		float penX = startX, penY = absPos.y + 4.0f;
		float startPenY = penY;
		int   lineBreaks = 0; // count during paint for invalidation
		float effWrap = m_AutoWrap ? geom.LocalSize.x - 8.0f : -1.0f;
		m_CachedWrapWidth = effWrap;
		if (effWrap <= 0) effWrap = 1e9f;
		float absRight = startX + effWrap;
		int selStart = std::min(m_Anchor, m_CaretPos), selEnd = std::max(m_Anchor, m_CaretPos);
		int glyphIdx = 0;

		// Cache glyph positions for click-to-position (exact match with rendered layout)
		m_GlyphPositions.clear();
		m_GlyphPositions.reserve(m_Text.size());
		const uint8_t* p = (const uint8_t*)m_Text.data();
		const uint8_t* end = p + m_Text.size();
		uint32_t prevCp = 0;
		const uint8_t* lastSpaceP = nullptr;
		float caretX = startX, caretY = penY;

		while (p < end)
		{
			uint32_t cp;
			if      ((*p & 0x80) == 0)      { cp = *p; p += 1; }
			else if ((*p & 0xE0) == 0xC0 && p+1<end) { cp = ((*p & 0x1F)<<6)|(p[1]&0x3F); p+=2; }
			else if ((*p & 0xF0) == 0xE0 && p+2<end) { cp = ((*p & 0x0F)<<12)|((p[1]&0x3F)<<6)|(p[2]&0x3F); p+=3; }
			else { p++; continue; }

			if (glyphIdx == m_CaretPos) { caretX = penX; caretY = penY; }

			if (cp == '\n')
			{
				penX = startX; penY += metrics.LineHeight; lineBreaks++;
				prevCp = 0; lastSpaceP = nullptr;
				m_GlyphPositions.push_back({ penX - m_CachedAbsX, penY - m_CachedAbsY, (float)glyphIdx });
				glyphIdx++; continue;
			}

			const GlyphInfo* gi = m_FontCache->GetGlyph(m_Font, cp);
			if (!gi) { glyphIdx++; continue; }

			float advance = gi->AdvanceX;
			if (prevCp) advance += m_FontCache->GetKerning(m_Font, prevCp, cp);

			// Word-wrap look-ahead at spaces
			if (cp == ' ' && m_WrapMode == WrapMode::Word && effWrap > 0)
			{
				float ww = 0; const uint8_t* lk = p; uint32_t lp = cp;
				while (lk < end) { uint32_t lc; if ((*lk&0x80)==0){lc=*lk;lk++;}else if((*lk&0xE0)==0xC0&&lk+1<end){lc=((*lk&0x1F)<<6)|(lk[1]&0x3F);lk+=2;}else if((*lk&0xF0)==0xE0&&lk+2<end){lc=((*lk&0x0F)<<12)|((lk[1]&0x3F)<<6)|(lk[2]&0x3F);lk+=3;}else break; if(lc==' '||lc=='\n')break; auto* lgi=m_FontCache->GetGlyph(m_Font,lc); float la=lgi?lgi->AdvanceX:0; if(lp!=' ') la+=m_FontCache->GetKerning(m_Font,lp,lc); ww+=std::round(la); lp=lc; }
				if (penX + advance + ww > absRight && penX > startX) { penX = startX; penY += metrics.LineHeight; prevCp = 0; lastSpaceP = nullptr; if (glyphIdx <= m_CaretPos) { caretX = penX; caretY = penY; } m_GlyphPositions.push_back({penX - m_CachedAbsX, penY - m_CachedAbsY, (float)glyphIdx}); glyphIdx++; continue; }
				lastSpaceP = p;
			}

			bool overflow = (effWrap > 0 && penX + advance > absRight && penX > startX);
			if (overflow && m_WrapMode == WrapMode::Word && lastSpaceP)
				{ p = lastSpaceP; penX = startX; penY += metrics.LineHeight; prevCp = 0; lastSpaceP = nullptr; if (glyphIdx <= m_CaretPos) { caretX = penX; caretY = penY; } m_GlyphPositions.push_back({penX - m_CachedAbsX, penY - m_CachedAbsY, (float)glyphIdx}); glyphIdx++; continue; }
			if (overflow && m_WrapMode == WrapMode::Character)
				{ penX = startX; penY += metrics.LineHeight; prevCp = 0; if (glyphIdx <= m_CaretPos) { caretX = penX; caretY = penY; } }

			bool selected = (glyphIdx >= selStart && glyphIdx < selEnd);
			if (selected && HasFocus())
				drawList.AddBox(layerId + 2, { penX, penY }, { advance, metrics.LineHeight }, glm::vec4(0.2f,0.4f,0.7f,0.6f), clipIdx);

			float gx = penX + gi->BearingX;
			float gy = penY + metrics.Ascent + gi->BearingY;
			glm::vec2 uvs[4] = {{gi->AtlasU0,gi->AtlasV0},{gi->AtlasU1,gi->AtlasV0},{gi->AtlasU1,gi->AtlasV1},{gi->AtlasU0,gi->AtlasV1}};
			drawList.AddText(layerId + 3, {gx,gy}, std::string(1,(char)cp), uvs, gi->AdvanceX, gi->Height, glm::vec4(1,1,1,1), clipIdx);
			penX += std::round(advance);
			m_GlyphPositions.push_back({ penX - advance * 0.5f - m_CachedAbsX, penY - m_CachedAbsY, (float)glyphIdx });
			prevCp = cp; glyphIdx++;
		}
		if (m_CaretPos >= glyphIdx) { caretX = penX; caretY = penY; }

		if (HasFocus() && m_BlinkVisible)
			drawList.AddBox(layerId + 4, { caretX, caretY + 2 }, { 2.0f, metrics.LineHeight }, glm::vec4(1,1,1,1), clipIdx);

		// Invalidate immediately if any line break occurred that wasn't measured
		if (m_AutoWrap && m_WrapMode != WrapMode::None && lineBreaks > 0)
		{
			float measuredH = GetCachedDesiredSize().y;
			int measuredLines = (int)(measuredH / metrics.LineHeight);
			if (measuredLines < 1) measuredLines = 1;
			if (lineBreaks + 1 > measuredLines)
				InvalidateLayout();
		}
		m_BlinkVisible = (fmodf(UIApplication::GetAnimTime(), 1.06f) < 0.53f);
		return layerId + 5;
	}

	Reply OnMouseEnter() override { m_IsHovered = true; return Reply::Unhandled(); }
	Reply OnMouseLeave() override { m_IsHovered = false; return Reply::Unhandled(); }

	Reply OnMouseButtonDown(const glm::vec2& screenPos, int button) override
	{
		if (button != 0) return Reply::Unhandled();
		int pos = GlyphIndexFromPixel(screenPos.x, screenPos.y);
		auto now = std::chrono::steady_clock::now();

		auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(now - m_LastClickTime).count();
		if (elapsed < 500 && std::abs(screenPos.x - m_LastClickX) < 24.0f && std::abs(screenPos.y - m_LastClickY) < 24.0f)
		{
			int start = pos, end = pos;
			while (start > 0 && IsWordChar(m_Text[start - 1])) start--;
			while (end < (int)m_Text.size() && IsWordChar(m_Text[end])) end++;
			m_Anchor = start;
			m_CaretPos = end;
			m_LastClickTime = std::chrono::steady_clock::time_point{};
			return Reply::Handled_().SetFocus();
		}
		else
		{
		if (m_ReadOnly) return Reply::Handled_().SetFocus();

		int pos = GlyphIndexFromPixel(screenPos.x, screenPos.y);
		auto now = std::chrono::steady_clock::now();
		auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(now - m_LastClickTime).count();

		if (elapsed < 500 && std::abs(screenPos.x - m_LastClickX) < 24.0f && std::abs(screenPos.y - m_LastClickY) < 24.0f)
		{
			int start = pos, end = pos;
			while (start > 0 && IsWordChar(m_Text[start - 1])) start--;
			while (end < (int)m_Text.size() && IsWordChar(m_Text[end])) end++;
			m_Anchor = start;
			m_CaretPos = end;
			m_LastClickTime = std::chrono::steady_clock::time_point{};
			return Reply::Handled_().SetFocus();
		}

		m_CaretPos = pos; m_Anchor = pos;
		m_LastClickTime = now;
		m_LastClickX = screenPos.x;
		m_LastClickY = screenPos.y;
		return Reply::Handled_().SetFocus();
		}
	}

	Reply OnMouseMove(const glm::vec2& screenPos) override
	{
		if (!m_Dragging) return Reply::Unhandled();
		m_CaretPos = GlyphIndexFromPixel(screenPos.x, screenPos.y);
		return Reply::Handled_();
	}

	Reply OnMouseButtonUp(const glm::vec2&, int button) override
	{
		if (button != 0) return Reply::Unhandled();
		m_Dragging = false;
		return Reply::Handled_();
	}

	Reply OnKeyDown(int key, int mods) override
	{
		bool shift = (mods & 1) != 0;
		bool ctrl  = (mods & 2) != 0;

		if (ctrl && key == 65) // Ctrl+A — always allowed
		{
			m_Anchor = 0; m_CaretPos = (int)m_Text.size();
			ResetBlink(); return Reply::Handled_();
		}
		if (ctrl && key == 67) // Ctrl+C — always allowed
		{
			if (m_Anchor != m_CaretPos)
			{
				int s = std::min(m_Anchor, m_CaretPos);
				int e = std::max(m_Anchor, m_CaretPos);
				Clipboard::Set(m_Text.substr(s, e - s));
			}
			return Reply::Handled_();
		}
		if (m_ReadOnly) return Reply::Unhandled();
		if (ctrl && key == 86) // Ctrl+V — paste
		{
			std::string clip = Clipboard::Get();
			if (!clip.empty())
			{
				DeleteSelection();
				m_Text.insert(m_CaretPos, clip);
				m_CaretPos += (int)clip.size(); m_Anchor = m_CaretPos;
				NotifyChanged(); ResetBlink();
			}
			return Reply::Handled_();
		}
		if (ctrl && key == 88) // Ctrl+X — cut
		{
			if (m_Anchor != m_CaretPos)
			{
				int s = std::min(m_Anchor, m_CaretPos);
				int e = std::max(m_Anchor, m_CaretPos);
				Clipboard::Set(m_Text.substr(s, e - s));
				m_Text.erase(s, e - s);
				m_CaretPos = m_Anchor = s;
				NotifyChanged(); ResetBlink();
			}
			return Reply::Handled_();
		}

		switch (key)
		{
		case 256: // Escape — clear selection, keep cursor
			m_Anchor = m_CaretPos;
			ResetBlink(); return Reply::Handled_();

		case 257: // Enter
			if (shift)
			{
				DeleteSelection();
				m_Text.insert(m_CaretPos, "\n");
				m_CaretPos++; m_Anchor = m_CaretPos;
				NotifyChanged(); ResetBlink();
				return Reply::Handled_();
			}
			if (m_IsEditing)
			{
				m_IsEditing = false;
				m_Anchor = m_CaretPos;
				ResetBlink();
				return Reply::Handled_();
			}
			else
			{
				m_IsEditing = true;
				m_Anchor = 0; m_CaretPos = (int)m_Text.size();
				ResetBlink();
				return Reply::Handled_();
			}

		case 259: // Backspace
			if (m_Anchor != m_CaretPos)
			{
				int s = std::min(m_Anchor, m_CaretPos);
				int e = std::max(m_Anchor, m_CaretPos);
				m_Text.erase(s, e - s);
				m_CaretPos = m_Anchor = s;
				NotifyChanged();
			}
			else if (m_CaretPos > 0)
			{
				m_Text.erase(m_CaretPos - 1, 1);
				m_CaretPos--; m_Anchor = m_CaretPos;
				NotifyChanged();
			}
			ResetBlink(); return Reply::Handled_();

		case 261: // Delete
			if (m_Anchor != m_CaretPos)
			{
				int s = std::min(m_Anchor, m_CaretPos);
				int e = std::max(m_Anchor, m_CaretPos);
				m_Text.erase(s, e - s);
				m_CaretPos = m_Anchor = s;
				NotifyChanged();
			}
			else if (m_CaretPos < (int)m_Text.size())
			{
				m_Text.erase(m_CaretPos, 1);
				NotifyChanged();
			}
			ResetBlink(); return Reply::Handled_();

		case 263: // Left
			if (!m_IsEditing) return Reply::Unhandled();
			if (ctrl)
			{
				// Ctrl+Left: move to previous word boundary
				while (m_CaretPos > 0 && m_Text[m_CaretPos - 1] == ' ') m_CaretPos--;
				while (m_CaretPos > 0 && m_Text[m_CaretPos - 1] != ' ') m_CaretPos--;
			}
			else if (m_CaretPos > 0) m_CaretPos--;
			if (!shift) m_Anchor = m_CaretPos;
			ResetBlink(); return Reply::Handled_();

		case 262: // Right
			if (!m_IsEditing) return Reply::Unhandled();
			if (ctrl)
			{
				// Ctrl+Right: move to next word boundary
				int len = (int)m_Text.size();
				while (m_CaretPos < len && m_Text[m_CaretPos] != ' ') m_CaretPos++;
				while (m_CaretPos < len && m_Text[m_CaretPos] == ' ') m_CaretPos++;
			}
			else if (m_CaretPos < (int)m_Text.size()) m_CaretPos++;
			if (!shift) m_Anchor = m_CaretPos;
			ResetBlink(); return Reply::Handled_();

		case 268: // Home
			m_CaretPos = 0;
			if (!shift) m_Anchor = 0;
			ResetBlink(); return Reply::Handled_();

		case 269: // End
			m_CaretPos = (int)m_Text.size();
			if (!shift) m_Anchor = m_CaretPos;
			ResetBlink(); return Reply::Handled_();

		case 265: // Up — Home on single-line
			if (!m_IsEditing) return Reply::Unhandled();
			m_CaretPos = 0;
			if (!shift) m_Anchor = 0;
			ResetBlink(); return Reply::Handled_();

		case 264: // Down — End on single-line
			if (!m_IsEditing) return Reply::Unhandled();
			m_CaretPos = (int)m_Text.size();
			if (!shift) m_Anchor = m_CaretPos;
			ResetBlink(); return Reply::Handled_();

		default: return Reply::Unhandled();
		}
	}

	Reply OnTextInput(uint32_t codepoint) override
	{
		if (m_ReadOnly) return Reply::Unhandled();
		if (codepoint < 32) return Reply::Unhandled();

		DeleteSelection();

		char utf8[5] = {};
		int len = 0;
		if (codepoint < 0x80)      { utf8[0] = (char)codepoint; len = 1; }
		else if (codepoint < 0x800) { utf8[0] = 0xC0|(codepoint>>6); utf8[1] = 0x80|(codepoint&0x3F); len = 2; }
		else                       { utf8[0] = 0xE0|(codepoint>>12); utf8[1] = 0x80|((codepoint>>6)&0x3F); utf8[2] = 0x80|(codepoint&0x3F); len = 3; }

		m_Text.insert(m_CaretPos, utf8, len);
		m_CaretPos += len; m_Anchor = m_CaretPos;
		NotifyChanged(); ResetBlink();
		return Reply::Handled_();
	}

	bool SupportsFocus() const override { return m_SupportsFocus; }
	void OnFocusReceived() override { m_HasFocus = true; ResetBlink(); }
	void OnFocusLost() override { m_HasFocus = false; m_Anchor = m_CaretPos; m_IsEditing = false; }
	bool HasFocus() const { return m_HasFocus; }

	NavigationReply OnNavigation(const FocusEvent& ev) override
	{
		if (m_IsEditing) return NavigationReply::Stop;
		return NavigationReply::Escape;
	}

private:
	void NotifyChanged() { if (m_OnTextChanged) m_OnTextChanged(m_Text); InvalidateLayout(); }
	void ResetBlink() { m_BlinkVisible = true; }

	void DeleteSelection()
	{
		if (m_Anchor == m_CaretPos) return;
		int s = std::min(m_Anchor, m_CaretPos);
		int e = std::max(m_Anchor, m_CaretPos);
		m_Text.erase(s, e - s);
		m_CaretPos = m_Anchor = s;
	}

	int GlyphIndexFromPixel(float pixelX, float pixelY) const
	{
		float localX = pixelX - m_CachedAbsX;
		float localY = pixelY - m_CachedAbsY;
		int bestIdx = 0;
		float bestDist = 1e9f;
		for (auto& gp : m_GlyphPositions)
		{
			float dist = std::abs(localX - gp.x) + std::abs(localY - gp.y) * 1.5f;
			if (dist < bestDist) { bestDist = dist; bestIdx = (int)gp.z; }
		}
		if (bestIdx > 0 && localX > 0 && m_GlyphPositions[bestIdx].x < localX)
			bestIdx = std::min(bestIdx + 1, (int)m_Text.size());
		return bestIdx;
	}

	static bool IsWordChar(char c)
	{
		return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_';
	}

	FontCache* m_FontCache;
	FontHandle m_Font;
	float      m_FontSize;
	mutable EditableTextStyle m_Style;
	std::string m_Text;
	int        m_CaretPos = 0;
	int        m_Anchor = 0;
	bool       m_Dragging = false;
	bool       m_IsEditing = false;
	mutable bool m_IsHovered = false;
	bool         m_HasFocus = false;
	bool         m_ReadOnly = false;
	mutable bool m_BlinkVisible = true;
	WrapMode     m_WrapMode = WrapMode::None;
	bool         m_AutoWrap = false;
	mutable float m_CachedWrapWidth = -1.0f;
	mutable float m_CachedAbsX = 0.0f;
	mutable float m_CachedAbsY = 0.0f;
	mutable std::vector<glm::vec3> m_GlyphPositions;
	std::function<void(const std::string&)> m_OnTextChanged;
	std::chrono::steady_clock::time_point m_LastClickTime;
	float m_LastClickX = 0.0f;
	float m_LastClickY = 0.0f;
};

using SEditableText = EditableText;

} // namespace wisp
