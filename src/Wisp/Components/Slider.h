#pragma once

#include "Wisp/Core.h"
#include "Wisp/DrawList.h"
#include "Wisp/Style.h"
#include "Wisp/Theme.h"
#include <functional>

namespace wisp {

// ---- Slider ----------------------------------------------------------------

class Slider : public Widget
{
public:
	WISP_WIDGET_TYPE(Slider)

	Slider() { SetSupportsFocus(true); m_Style = GetDefaultStyle(); }

	static SliderStyle GetDefaultStyle()
	{
		SliderStyle s;
		s.Track.Tint         = { 0.15f, 0.15f, 0.2f, 1.0f };
		s.Fill.Tint          = { 0.3f, 0.5f, 0.8f, 1.0f };
		s.Thumb.Tint         = { 0.25f, 0.45f, 0.75f, 1.0f };
		s.ThumbHovered.Tint  = { 0.35f, 0.55f, 0.85f, 1.0f };
		s.ThumbDragged.Tint  = { 0.4f, 0.6f, 0.9f, 1.0f };
		return s;
	}

	float GetValue() const { return m_Value; }
	void SetValue(float v) { m_Value = std::max(0.0f, std::min(1.0f, v)); }
	void SetStepSize(float s) { m_StepSize = s; }
	void SetOnValueChanged(std::function<void(float)> cb) { m_OnValueChanged = std::move(cb); }

	glm::vec2 ComputeDesiredSize() const override { return { 200.0f, 24.0f }; }
	void ArrangeChildren(const Geometry&, ArrangedChildren&) const override {}

	int OnPaint(const PaintArgs& args, const Geometry& geom, DrawList& drawList, int layerId, const WidgetStyleContext&) const override
	{
		if (m_StyleDirty)
		{
			m_Style = ResolveStyle(this, GetDefaultStyle());
			m_StyleDirty = false;
		}
		glm::vec2 absPos = geom.LocalToAbsolute({ 0, 0 });
		float w = geom.LocalSize.x;
		float h = geom.LocalSize.y;
		float midY = absPos.y + h * 0.5f;

		m_CachedAbsX = absPos.x;
		m_CachedWidth = w;

		float trackH = m_Style.TrackHeight;
		float thumbR = m_Style.ThumbSize * 0.5f;

		drawList.AddBox(layerId, { absPos.x, midY - trackH * 0.5f }, { w, trackH },
			m_Style.Track.Tint, args.InheritedClipIndex);

		float fillW = w * m_Value;
		if (fillW > 0)
			drawList.AddBox(layerId + 1, { absPos.x, midY - trackH * 0.5f }, { fillW, trackH },
				m_Style.Fill.Tint, args.InheritedClipIndex);

		float thumbX = absPos.x + (w - thumbR * 2) * m_Value;
		glm::vec4 thumbCol = m_Dragging ? m_Style.ThumbDragged.Tint
			: (m_IsHovered ? m_Style.ThumbHovered.Tint : m_Style.Thumb.Tint);
		drawList.AddBox(layerId + 2, { thumbX, midY - thumbR }, { thumbR * 2, thumbR * 2 }, thumbCol, args.InheritedClipIndex);

		return layerId + 3;
	}

	Reply OnMouseEnter() override { m_IsHovered = true; return Reply::Unhandled(); }
	Reply OnMouseLeave() override { m_IsHovered = false; return Reply::Unhandled(); }

	Reply OnMouseButtonDown(const glm::vec2& screenPos, int button) override
	{
		if (button != 0) return Reply::Unhandled();
		m_Dragging = true;
		UpdateValueFromPosition(screenPos.x);
		return Reply::Handled_().CaptureMouse();
	}

	Reply OnMouseMove(const glm::vec2& screenPos) override
	{
		if (!m_Dragging) return Reply::Unhandled();
		UpdateValueFromPosition(screenPos.x);
		return Reply::Handled_();
	}

	Reply OnMouseButtonUp(const glm::vec2&, int button) override
	{
		if (button != 0) return Reply::Unhandled();
		m_Dragging = false;
		return Reply::Handled_().ReleaseCapture();
	}

	bool SupportsFocus() const override { return m_SupportsFocus; }
	NavigationReply OnNavigation(const FocusEvent& ev) override
	{
		if (ev.Direction == 0) { SetValue(m_Value - m_StepSize); return NavigationReply::Stop; }
		if (ev.Direction == 1) { SetValue(m_Value + m_StepSize); return NavigationReply::Stop; }
		return NavigationReply::Escape;
	}

private:
	void UpdateValueFromPosition(float absX)
	{
		float newVal = (absX - m_CachedAbsX) / std::max(1.0f, m_CachedWidth);
		newVal = std::max(0.0f, std::min(1.0f, newVal));
		if (m_StepSize > 0)
			newVal = std::round(newVal / m_StepSize) * m_StepSize;
		if (newVal != m_Value)
		{
			m_Value = newVal;
			if (m_OnValueChanged) m_OnValueChanged(m_Value);
		}
	}

	mutable SliderStyle m_Style;
	float m_Value = 0.5f;
	float m_StepSize = 0.0f;
	std::function<void(float)> m_OnValueChanged;
	mutable float m_CachedAbsX = 0.0f;
	mutable float m_CachedWidth = 200.0f;
	mutable bool  m_IsHovered = false;
	mutable bool  m_Dragging = false;
};

// S-prefix alias
using SSlider = Slider;

} // namespace wisp
