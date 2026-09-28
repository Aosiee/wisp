#pragma once

#include "Wisp/Core.h"
#include "Wisp/DrawList.h"
#include "Wisp/Application.h"
#include "Wisp/Style.h"
#include "Wisp/Theme.h"
#include <algorithm>
#include <cmath>

namespace wisp {

class ProgressBar : public Widget
{
public:
	WISP_WIDGET_TYPE(ProgressBar)

	ProgressBar() { m_Style = GetDefaultStyle(); }

	static ProgressBarStyle GetDefaultStyle()
	{
		ProgressBarStyle s;
		s.Track.Tint   = { 0.08f, 0.08f, 0.12f, 1.0f };
		s.Fill.Tint    = { 0.3f, 0.6f, 1.0f, 1.0f };
		s.Marquee.Tint = { 0.3f, 0.6f, 1.0f, 1.0f };
		return s;
	}

	ProgressBar& SetValue(float v)       { m_Value = v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v); return *this; }
	float        GetValue() const        { return m_Value; }

	ProgressBar& SetFillBrush(const Brush& b)  { m_Style.Fill = b; m_Style.Marquee = b; m_HasUserStyle = true; return *this; }
	ProgressBar& SetTrackBrush(const Brush& b) { m_Style.Track = b; m_HasUserStyle = true; return *this; }

	ProgressBar& SetIndeterminate(bool b) { m_Indeterminate = b; return *this; }
	bool          IsIndeterminate() const { return m_Indeterminate; }

	ProgressBar& SetHeight(float h)       { m_Height = h; InvalidateLayout(); return *this; }

	glm::vec2 ComputeDesiredSize() const override
	{
		return { 120.0f, std::max(m_Height, 4.0f) };
	}

	void ArrangeChildren(const Geometry&, ArrangedChildren&) const override {}

	int OnPaint(const PaintArgs& args, const Geometry& geom, DrawList& drawList, int layerId, const WidgetStyleContext& style) const override
	{
		if (m_StyleDirty && !m_HasUserStyle)
		{
			m_Style = ResolveStyle(this, GetDefaultStyle());
			m_StyleDirty = false;
		}
		glm::vec2 absPos = geom.LocalToAbsolute({0, 0});
		float w = geom.LocalSize.x, h = geom.LocalSize.y;
		int clipIdx = args.InheritedClipIndex;

		drawList.AddBox(layerId, absPos, {w, h}, m_Style.Track.Tint, clipIdx);

		if (m_Indeterminate)
		{
			float stripeW = w * 0.35f;
			float t = fmodf(UIApplication::GetAnimTime() * 0.8f, 1.0f);
			float x0 = t * (w + stripeW) - stripeW;
			drawList.AddBox(layerId + 1, {absPos.x + x0, absPos.y}, {stripeW, h}, m_Style.Marquee.Tint, clipIdx);
		}
		else if (m_Value > 0.0f)
		{
			drawList.AddBox(layerId + 1, absPos, {w * m_Value, h}, m_Style.Fill.Tint, clipIdx);
		}
		return layerId + 2;
	}

private:
	mutable ProgressBarStyle m_Style;
	bool m_HasUserStyle = false;
	float  m_Value = 0.0f;
	float  m_Height = 16.0f;
	bool   m_Indeterminate = false;
};

using SProgressBar = ProgressBar;

} // namespace wisp
