#pragma once
#include "Wisp/Core.h"
#include "Wisp/Icons.h"
#include <cmath>
#include "Wisp/FontCache.h"
#include "Wisp/Application.h"
#include "Wisp/StyleTokens.h"
#include "Wisp/DrawList.h"
#include "Wisp/Style.h"
#include "Wisp/Theme.h"
#include <functional>

namespace wisp {

// ---- CheckBox --------------------------------------------------------------

class CheckBox : public Widget
{
public:
	WISP_WIDGET_TYPE(CheckBox)

	CheckBox() { m_Style = GetDefaultStyle(); SetSupportsFocus(true); }

	static CheckBoxStyle GetDefaultStyle()
	{
		CheckBoxStyle s;
		s.BoxUnchecked.Tint = tokens::Field;
		s.BoxChecked.Tint   = tokens::Accent;
		s.BoxHovered.Tint   = tokens::FieldHover;
		s.CheckMark.Tint    = tokens::TextOnAccent;
		return s;
	}

	void SetLabel(const std::string& label) { m_Label = label; }
	bool IsChecked() const { return m_Checked; }
	void SetChecked(bool c) { m_Checked = c; }
	void SetOnStateChanged(std::function<void(bool)> cb) { m_OnStateChanged = std::move(cb); }

	glm::vec2 ComputeDesiredSize() const override
	{
		return { m_Style.BoxSize + m_LabelWidth + m_Style.LabelPadding.Left, m_Style.BoxSize + 4.0f };
	}

	void ArrangeChildren(const Geometry&, ArrangedChildren&) const override {}

	int OnPaint(const PaintArgs& args, const Geometry& geom, DrawList& drawList, int layerId, const WidgetStyleContext&) const override
	{
		if (m_StyleDirty)
		{
			m_Style = ResolveStyle(this, GetDefaultStyle());
			m_StyleDirty = false;
		}
		glm::vec2 absPos = geom.LocalToAbsolute({ 0, 0 });
		float bs = m_Style.BoxSize;
		float boxY = std::round(absPos.y + std::max(0.0f, (geom.LocalSize.y - bs) * 0.5f));

		// A rounded field box; checked = the accent fill + the TinyIcons
		// check.
		const float r = tokens::RadiusSmall;
		const float rr[4] = { r, r, r, r };
		if (m_Checked)
		{
			drawList.AddRoundedBox(layerId, { absPos.x, boxY }, { bs, bs },
				m_IsHovered ? tokens::AccentHover : m_Style.BoxChecked.Tint, rr, 0.0f, glm::vec4(0.0f), args.InheritedClipIndex);
			if (UIApplication::IsValid() && UIApplication::Get().GetFontCache())
			{
				FontCache* fc = UIApplication::Get().GetFontCache();
				const FontHandle icon = UIApplication::Get().GetFont(FontRole::Icons);
				const float px = bs - 2.0f;
				if (const GlyphInfo* g = fc->GetGlyph(icon, icons::cp::Check, px))
				{
					const FontMetrics m = fc->GetMetrics(icon, px);
					AddGlyph(drawList, layerId + 1, { absPos.x + (bs - g->AdvanceX) * 0.5f, boxY + (bs - m.LineHeight) * 0.5f + m.Ascent },
						*g, m_Style.CheckMark.Tint, args.InheritedClipIndex);
				}
			}
		}
		else
		{
			drawList.AddRoundedBox(layerId, { absPos.x, boxY }, { bs, bs },
				m_IsHovered ? m_Style.BoxHovered.Tint : m_Style.BoxUnchecked.Tint, rr, 1.0f,
				m_IsHovered ? tokens::TextMuted : tokens::BorderStrong, args.InheritedClipIndex);
		}

		return layerId + 2;
	}

	Reply OnMouseEnter() override { m_IsHovered = true; return Reply::Unhandled(); }
	Reply OnMouseLeave() override { m_IsHovered = false; return Reply::Unhandled(); }

	Reply OnMouseButtonDown(const glm::vec2&, int button) override
	{
		if (button != 0) return Reply::Unhandled();
		return Reply::Handled_().CaptureMouse();
	}

	Reply OnMouseButtonUp(const glm::vec2&, int button) override
	{
		if (button != 0) return Reply::Unhandled();
		if (m_IsHovered)
		{
			m_Checked = !m_Checked;
			if (m_OnStateChanged) m_OnStateChanged(m_Checked);
		}
		return Reply::Handled_().ReleaseCapture();
	}

	bool SupportsFocus() const override { return true; }

	Reply OnKeyDown(int key, int) override
	{
		if (key == 32) { m_Checked = !m_Checked; if (m_OnStateChanged) m_OnStateChanged(m_Checked); return Reply::Handled_(); }
		return Reply::Unhandled();
	}

private:
	mutable CheckBoxStyle m_Style;
	std::string m_Label;
	bool m_Checked = false;
	float m_LabelWidth = 0.0f;
	mutable bool m_IsHovered = false;
	std::function<void(bool)> m_OnStateChanged;
};


} // namespace wisp
