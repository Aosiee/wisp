#pragma once

#include "Wisp/Core.h"
#include "Wisp/DrawList.h"
#include "Wisp/Style.h"
#include "Wisp/Theme.h"
#include "Wisp/StyleTokens.h"

namespace wisp {

struct SeparatorStyle
{
	WISP_STYLE_TYPE(SeparatorStyle)
	Brush Line;
	bool operator==(const SeparatorStyle&) const = default;
};

inline SeparatorStyle GetDefaultSeparatorStyle()
{
	SeparatorStyle s;
	s.Line.Tint = wisp::tokens::Separator;
	return s;
}

class Separator : public Widget
{
public:
	WISP_WIDGET_TYPE(Separator)

	Separator(bool horizontal = true)
		: m_Horizontal(horizontal)
	{
		m_Style = GetDefaultSeparatorStyle();
	}

	Separator& SetThickness(float t) { m_Thickness = t; InvalidateLayout(); return *this; }
	Separator& SetColour(const glm::vec4& c) { m_Colour = c; return *this; }

	glm::vec2 ComputeDesiredSize() const override
	{
		if (m_Horizontal) return { 20.0f, m_Thickness };
		return { m_Thickness, 20.0f };
	}

	void ArrangeChildren(const Geometry&, ArrangedChildren&) const override {}

	int OnPaint(const PaintArgs& args, const Geometry& geom, DrawList& drawList, int layerId, const WidgetStyleContext&) const override
	{
		if (m_StyleDirty)
		{
			m_Style = ResolveStyle(this, GetDefaultSeparatorStyle());
			m_StyleDirty = false;
		}

		glm::vec2 pos = geom.LocalToAbsolute({0,0});
		glm::vec4 col = m_Colour.a > 0 ? m_Colour : m_Style.Line.Tint;
		drawList.AddBox(layerId, pos, geom.LocalSize, col, args.InheritedClipIndex);
		return layerId + 1;
	}

private:
	bool m_Horizontal = true;
	float m_Thickness = 1.0f;
	glm::vec4 m_Colour{0,0,0,0};
	mutable SeparatorStyle m_Style;
};

using SSeparator = Separator;

} // namespace wisp
