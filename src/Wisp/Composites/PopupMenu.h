#pragma once

// The one popup-menu look. A menu is a list of PopupMenuEntry;
// BuildPopupMenu turns it into a shadowed, rounded popup of 26 px rows
// (check/icon column, label, right-aligned shortcut, an inset accent hover,
// disabled rows greyed) with separators and section headers. ShowPopupMenu
// opens it through the app's popup layer. Menu bars, context menus and
// dropdown lists all go through this, so they read as one product.

#include "Wisp/Core.h"
#include "Wisp/Application.h"
#include "Wisp/DrawList.h"
#include "Wisp/FontCache.h"
#include "Wisp/Icons.h"
#include "Wisp/StyleTokens.h"
#include "Wisp/Components/BoxPanel.h"

#include <algorithm>
#include <cmath>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace wisp {

struct PopupMenuEntry
{
	std::string Label;
	std::string Icon;      // an icons:: string, drawn in the leading column
	std::string Shortcut;  // right-aligned hint ("Ctrl+S")
	bool Checkable = false;
	bool Checked = false;
	bool Enabled = true;
	bool Separator = false;
	bool Header = false;   // a non-interactive section label
	// An action that opens its own nested popup (a rename prompt) keeps the
	// menu's dismissal off - the deferred flush would destroy that prompt.
	bool KeepOpen = false;
	std::function<void()> Action;

	static PopupMenuEntry MakeSeparator() { PopupMenuEntry e; e.Separator = true; return e; }
	static PopupMenuEntry MakeHeader(std::string label) { PopupMenuEntry e; e.Header = true; e.Label = std::move(label); return e; }
};

namespace popupmenu {

inline constexpr float kRowHeight = 26.0f;
inline constexpr float kSeparatorHeight = 9.0f;
inline constexpr float kHeaderHeight = 24.0f;
inline constexpr float kPad = 4.0f;          // frame padding
inline constexpr float kLeadColumn = 30.0f;  // the check/icon column
inline constexpr float kShortcutGap = 36.0f;

// UTF-8 text painted from the pen on the baseline; returns the advance.
inline float PaintRun(DrawList& dl, FontCache* fc, FontHandle font, float size, glm::vec2 pen,
	const std::string& text, const glm::vec4& tint, int layer, int clip)
{
	const float startX = pen.x;
	const uint8_t* p = (const uint8_t*)text.data();
	const uint8_t* e = p + text.size();
	uint32_t prev = 0;
	while (p < e)
	{
		uint32_t cp;
		if ((*p & 0x80) == 0) { cp = *p; p += 1; }
		else if ((*p & 0xE0) == 0xC0 && p + 1 < e) { cp = ((*p & 0x1F) << 6) | (p[1] & 0x3F); p += 2; }
		else if ((*p & 0xF0) == 0xE0 && p + 2 < e) { cp = ((*p & 0x0F) << 12) | ((p[1] & 0x3F) << 6) | (p[2] & 0x3F); p += 3; }
		else { p++; continue; }
		const GlyphInfo* g = fc->GetGlyph(font, cp, size);
		if (!g)
			continue;
		if (prev)
			pen.x += fc->GetKerning(font, prev, cp, size);
		AddGlyph(dl, layer, pen, *g, tint, clip);
		pen.x += g->AdvanceX;
		prev = cp;
	}
	return pen.x - startX;
}

} // namespace popupmenu

// One row of a popup menu (also used standalone for list-style popups).
class PopupMenuRow : public Widget
{
public:
	WISP_WIDGET_TYPE(PopupMenuRow)

	PopupMenuRow(FontCache* fc, FontHandle font, float fontSize, PopupMenuEntry entry)
		: m_FontCache(fc), m_Font(font), m_FontSize(fontSize), m_Entry(std::move(entry))
	{
		if (!m_Entry.Separator && !m_Entry.Header && m_Entry.Enabled)
			SetCursor(CursorType::Hand);
	}

	glm::vec2 ComputeDesiredSize() const override
	{
		if (m_Entry.Separator)
			return { 0.0f, popupmenu::kSeparatorHeight };
		const float labelW = m_FontCache ? m_FontCache->MeasureText(LabelFont(), m_Entry.Label, LabelSize()).x : 0.0f;
		const float shortcutW = (!m_Entry.Shortcut.empty() && m_FontCache)
			? m_FontCache->MeasureText(m_Font, m_Entry.Shortcut, tokens::FontSmall).x + popupmenu::kShortcutGap : 0.0f;
		return { popupmenu::kLeadColumn + labelW + shortcutW + 16.0f,
			m_Entry.Header ? popupmenu::kHeaderHeight : popupmenu::kRowHeight };
	}

	void ArrangeChildren(const Geometry&, ArrangedChildren&) const override {}

	int OnPaint(const PaintArgs& args, const Geometry& geom, DrawList& dl, int layerId, const WidgetStyleContext&) const override
	{
		const glm::vec2 pos = geom.LocalToAbsolute({ 0, 0 });
		const glm::vec2 size = geom.LocalSize;
		const int clip = args.InheritedClipIndex;
		if (m_Entry.Separator)
		{
			dl.AddBox(layerId, { pos.x + 8.0f, std::round(pos.y + size.y * 0.5f) }, { size.x - 16.0f, 1.0f }, tokens::Separator, clip);
			return layerId + 1;
		}
		if (!m_FontCache)
			return layerId + 1;

		const bool interactive = !m_Entry.Header && m_Entry.Enabled;
		const bool hot = interactive && m_Hovered;
		if (hot)
		{
			const float r[4] = { tokens::RadiusSmall, tokens::RadiusSmall, tokens::RadiusSmall, tokens::RadiusSmall };
			dl.AddRoundedBox(layerId, { pos.x + 2.0f, pos.y + 1.0f }, { size.x - 4.0f, size.y - 2.0f },
				m_Pressed ? tokens::AccentPressed : tokens::Accent, r, 0.0f, glm::vec4(0.0f), clip);
		}

		const glm::vec4 textCol = m_Entry.Header ? tokens::TextMuted
			: (!m_Entry.Enabled ? tokens::TextDisabled : (hot ? tokens::TextOnAccent : tokens::TextPrimary));
		const glm::vec4 subCol = hot ? tokens::WithAlpha(tokens::TextOnAccent, 0.8f) : tokens::TextMuted;

		const FontMetrics fm = m_FontCache->GetMetrics(LabelFont(), LabelSize());
		const float baseline = std::round(pos.y + (size.y - fm.LineHeight) * 0.5f + fm.Ascent);

		// Lead column: the check (checkable rows) or the entry's icon.
		const char* lead = m_Entry.Checkable ? (m_Entry.Checked ? icons::Check : nullptr)
			: (m_Entry.Icon.empty() ? nullptr : m_Entry.Icon.c_str());
		if (lead)
		{
			const FontHandle iconFont = UIApplication::FontOr(FontRole::Icons, m_Font);
			const float iconPx = 15.0f;
			const FontMetrics im = m_FontCache->GetMetrics(iconFont, iconPx);
			const float iy = std::round(pos.y + (size.y - im.LineHeight) * 0.5f + im.Ascent);
			const glm::vec4 iconCol = m_Entry.Checkable ? (hot ? tokens::TextOnAccent : tokens::Accent)
				: (!m_Entry.Enabled ? tokens::TextDisabled : (hot ? tokens::TextOnAccent : tokens::TextSecondary));
			popupmenu::PaintRun(dl, m_FontCache, iconFont, iconPx, { pos.x + 9.0f, iy }, lead, iconCol, layerId + 1, clip);
		}

		const float labelX = m_Entry.Header ? pos.x + 10.0f : pos.x + popupmenu::kLeadColumn;
		popupmenu::PaintRun(dl, m_FontCache, LabelFont(), LabelSize(), { labelX, baseline }, m_Entry.Label, textCol, layerId + 1, clip);

		if (!m_Entry.Shortcut.empty())
		{
			const float sw = m_FontCache->MeasureText(m_Font, m_Entry.Shortcut, tokens::FontSmall).x;
			const FontMetrics sm = m_FontCache->GetMetrics(m_Font, tokens::FontSmall);
			const float sb = std::round(pos.y + (size.y - sm.LineHeight) * 0.5f + sm.Ascent);
			popupmenu::PaintRun(dl, m_FontCache, m_Font, tokens::FontSmall, { std::round(pos.x + size.x - sw - 12.0f), sb },
				m_Entry.Shortcut, subCol, layerId + 1, clip);
		}
		return layerId + 2;
	}

	Reply OnMouseEnter() override { m_Hovered = true; return Reply::Unhandled(); }
	Reply OnMouseLeave() override { m_Hovered = false; m_Pressed = false; return Reply::Unhandled(); }

	Reply OnMouseButtonDown(const glm::vec2&, int button) override
	{
		if (button != 0 || m_Entry.Separator || m_Entry.Header)
			return Reply::Handled_();
		m_Pressed = m_Entry.Enabled;
		return Reply::Handled_().CaptureMouse();
	}

	Reply OnMouseButtonUp(const glm::vec2&, int button) override
	{
		if (button != 0)
			return Reply::Unhandled();
		const bool fire = m_Pressed && m_Hovered && m_Entry.Enabled;
		m_Pressed = false;
		if (fire)
		{
			// Deferred dismissal: the action may tear down the popup this row
			// lives in, so the row only requests it (the app flushes after
			// dispatch - the dock context menu's rule).
			if (m_Entry.Action)
				m_Entry.Action();
			if (!m_Entry.KeepOpen && UIApplication::IsValid())
				UIApplication::Get().RequestDismissPopups();
		}
		return Reply::Handled_().ReleaseCapture();
	}

private:
	FontHandle LabelFont() const { return m_Entry.Header ? UIApplication::FontOr(FontRole::SemiBold, m_Font) : m_Font; }
	float LabelSize() const { return m_Entry.Header ? tokens::FontSmall : m_FontSize; }

	FontCache* m_FontCache;
	FontHandle m_Font;
	float m_FontSize;
	PopupMenuEntry m_Entry;
	bool m_Hovered = false;
	bool m_Pressed = false;
};

// The popup chrome: a soft shadow, a rounded Popup-coloured card with a
// strong 1 px border, uniform padding around one child.
class PopupFrame : public Widget
{
public:
	WISP_WIDGET_TYPE(PopupFrame)

	explicit PopupFrame(std::unique_ptr<Widget> child, float minWidth = 0.0f, float padding = popupmenu::kPad)
		: m_Child(std::move(child)), m_MinWidth(minWidth), m_Padding(padding)
	{
		if (m_Child)
			SetupParentChild(this, m_Child.get());
	}

	glm::vec2 ComputeDesiredSize() const override
	{
		glm::vec2 d{ 0.0f };
		if (m_Child)
		{
			d = m_Child->ComputeDesiredSize();
			m_Child->SetCachedDesiredSize(d);
		}
		return { std::max(d.x, m_MinWidth) + m_Padding * 2.0f, d.y + m_Padding * 2.0f };
	}

	void ArrangeChildren(const Geometry& geom, ArrangedChildren& out) const override
	{
		if (m_Child)
			out.Add(m_Child.get(), geom.MakeChild({ m_Padding, m_Padding }, geom.LocalSize - glm::vec2(m_Padding * 2.0f)));
	}

	int OnPaint(const PaintArgs& args, const Geometry& geom, DrawList& dl, int layerId, const WidgetStyleContext& style) const override
	{
		const glm::vec2 pos = geom.LocalToAbsolute({ 0, 0 });
		const float r = tokens::RadiusMedium + 1.0f;
		const float rr[4] = { r, r, r, r };
		const float sr[4] = { r + 2.0f, r + 2.0f, r + 2.0f, r + 2.0f };
		dl.AddRoundedBox(layerId, pos + glm::vec2(-1.0f, 3.0f), geom.LocalSize + glm::vec2(2.0f, 3.0f), tokens::WithAlpha(tokens::Shadow, 0.28f), sr, 0.0f, glm::vec4(0.0f), args.InheritedClipIndex);
		dl.AddRoundedBox(layerId, pos + glm::vec2(0.0f, 1.0f), geom.LocalSize + glm::vec2(0.0f, 1.0f), tokens::WithAlpha(tokens::Shadow, 0.35f), rr, 0.0f, glm::vec4(0.0f), args.InheritedClipIndex);
		dl.AddRoundedBox(layerId + 1, pos, geom.LocalSize, tokens::Popup, rr, 1.0f, tokens::BorderStrong, args.InheritedClipIndex);
		int next = layerId + 2;
		ArrangedChildren arranged;
		ArrangeChildren(geom, arranged);
		for (auto& c : arranged)
			next = c.WidgetPtr->OnPaint(args, c.Geom, dl, next, style);
		return next;
	}

	void ForEachChild(std::function<void(Widget*)> fn) override { if (m_Child) fn(m_Child.get()); }

private:
	std::unique_ptr<Widget> m_Child;
	float m_MinWidth = 0.0f;
	float m_Padding = popupmenu::kPad;
};

inline std::unique_ptr<Widget> BuildPopupMenu(FontCache* fc, FontHandle font, float fontSize,
	std::vector<PopupMenuEntry> entries, float minWidth = 200.0f)
{
	// A menu where some rows carry icons keeps one aligned label column.
	auto list = std::make_unique<VerticalBox>(BoxOrientation::Vertical);
	for (PopupMenuEntry& e : entries)
		list->AddChild(std::make_unique<PopupMenuRow>(fc, font, fontSize, std::move(e)));
	return std::make_unique<PopupFrame>(std::move(list), minWidth);
}

inline void ShowPopupMenu(FontCache* fc, FontHandle font, float fontSize,
	std::vector<PopupMenuEntry> entries, const glm::vec2& at, float minWidth = 200.0f)
{
	if (!UIApplication::IsValid())
		return;
	UIApplication::Get().ShowPopupAt(BuildPopupMenu(fc, font, fontSize, std::move(entries), minWidth), at);
}

} // namespace wisp
