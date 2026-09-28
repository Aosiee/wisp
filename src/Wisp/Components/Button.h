#pragma once
#include "Wisp/Core.h"
#include "Wisp/DrawList.h"
#include "Wisp/Animation.h"
#include "Wisp/Style.h"
#include "Wisp/Theme.h"
#include <functional>

namespace wisp {

// ---- Button ----------------------------------------------------------------

class Button : public Widget
{
public:
	WISP_WIDGET_TYPE(Button)

	Button() { SetCursor(CursorType::Hand); SetSupportsFocus(true); m_Style = GetDefaultStyle(); }

	static ButtonStyle GetDefaultStyle()
	{
		ButtonStyle s;
		s.Normal.Tint   = { 0.2f, 0.2f, 0.25f, 1.0f };
		s.Hovered.Tint  = { 0.25f, 0.25f, 0.3f, 1.0f };
		s.Pressed.Tint  = { 0.15f, 0.15f, 0.2f, 1.0f };
		s.Disabled.Tint = { 0.12f, 0.12f, 0.15f, 1.0f };
		return s;
	}

	Button& SetNormalBrush(const Brush& b)   { m_Style.Normal = b; m_HasUserStyle = true; return *this; }
	Button& SetHoveredBrush(const Brush& b)  { m_Style.Hovered = b; m_HasUserStyle = true; return *this; }
	Button& SetPressedBrush(const Brush& b)  { m_Style.Pressed = b; m_HasUserStyle = true; return *this; }
	Button& SetOnClicked(std::function<void()> cb) { m_OnClicked = std::move(cb); return *this; }
	void SetChild(std::unique_ptr<Widget> child)
	{
		m_Child = std::move(child);
		if (m_Child) SetupParentChild(this, m_Child.get());
	}


	glm::vec2 ComputeDesiredSize() const override
	{
		if (!m_Child) return { 0, 0 };
		glm::vec2 d = m_Child->ComputeDesiredSize();
		m_Child->SetCachedDesiredSize(d);
		return d + glm::vec2(m_Style.ContentPadding.Left + m_Style.ContentPadding.Right,
			m_Style.ContentPadding.Top + m_Style.ContentPadding.Bottom);
	}

	void ArrangeChildren(const Geometry& geom, ArrangedChildren& out) const override
	{
		if (!m_Child) return;
		glm::vec2 childPos = { m_Style.ContentPadding.Left, m_Style.ContentPadding.Top };
		glm::vec2 childSize = geom.LocalSize - glm::vec2(
			m_Style.ContentPadding.Left + m_Style.ContentPadding.Right,
			m_Style.ContentPadding.Top + m_Style.ContentPadding.Bottom);
		if (m_IsPressed) { childPos.x += m_Style.PressedContentOffset.x; childPos.y += m_Style.PressedContentOffset.y; }
		out.Add(m_Child.get(), geom.MakeChild(childPos, childSize));
	}

	int OnPaint(const PaintArgs& args, const Geometry& geom, DrawList& drawList, int layerId, const WidgetStyleContext& style) const override
	{
		if (m_StyleDirty && !m_HasUserStyle)
		{
			m_Style = ResolveStyle(this, GetDefaultStyle());
			m_StyleDirty = false;
		}
		// Register for animation tick if curve is playing
		if (m_HoverAnim.IsPlaying())
			const_cast<Button*>(this)->RequestAnimationTick();

		// Compute lerped tint between Normal and Hovered
		glm::vec4 tint = m_Style.Normal.Tint;
		if (m_IsPressed)
			tint = m_Style.Pressed.Tint;
		else if (m_HoverAlpha > 0.001f)
			tint = glm::mix(m_Style.Normal.Tint, m_Style.Hovered.Tint, m_HoverAlpha);

		glm::vec2 absPos = geom.LocalToAbsolute({ 0, 0 });
		drawList.AddBox(layerId, absPos, geom.LocalSize, tint, args.InheritedClipIndex);

		PaintArgs childArgs = args;
		childArgs.InheritedClipIndex = ApplyWidgetClipping(this, drawList, args.InheritedClipIndex, absPos, geom.LocalSize);
		ArrangedChildren arranged;
		ArrangeChildren(geom, arranged);
		int nextLayer = layerId + 1;
		WidgetStyleContext cs = style;
		for (auto& c : arranged)
		{
			cs.ColorAndOpacity = style.ColorAndOpacity * c.WidgetPtr->GetRenderTint();
			cs.ColorAndOpacity.a *= c.WidgetPtr->GetRenderOpacity();
			nextLayer = c.WidgetPtr->OnPaint(childArgs, c.Geom, drawList, nextLayer, cs);
		}
		return nextLayer;
	}

	Reply OnMouseEnter() override
	{
		m_IsHovered = true;
		m_HoverAnim.Play(m_HoverDuration, m_HoverEasing);
		RequestAnimationTick();
		return Reply::Unhandled();
	}

	Reply OnMouseLeave() override
	{
		m_IsHovered = false;
		m_IsPressed = false;
		m_HoverAnim.Reverse();
		RequestAnimationTick();
		return Reply::Unhandled();
	}

	Reply OnMouseButtonDown(const glm::vec2& localPos, int button) override
	{
		if (button != 0) return Reply::Unhandled();
		m_IsPressed = true;
		return Reply::Handled_().CaptureMouse();
	}

	Reply OnMouseButtonUp(const glm::vec2& localPos, int button) override
	{
		if (button != 0) return Reply::Unhandled();
		if (m_IsPressed && m_IsHovered && m_OnClicked)
			m_OnClicked();
		m_IsPressed = false;
		return Reply::Handled_().ReleaseCapture();
	}

	void OnFocusReceived() override
	{
		m_IsHovered = true;
		m_HoverAnim.Play(m_HoverDuration, m_HoverEasing);
		RequestAnimationTick();
	}

	void OnFocusLost() override
	{
		m_IsHovered = false;
		m_IsPressed = false;
		m_HoverAnim.Reverse();
		RequestAnimationTick();
	}

	void TickAnimation(float dt) override
	{
		if (!m_HoverAnim.IsPlaying()) return;
		m_HoverAnim.Tick(dt);
		m_HoverAlpha = m_HoverAnim.GetLerp();
		if (m_HoverAnim.IsPlaying())
			RequestAnimationTick();
	}

	void ForEachChild(std::function<void(Widget*)> fn) override { if (m_Child) fn(m_Child.get()); }

private:
	mutable ButtonStyle m_Style;
	bool                m_HasUserStyle = false;
	std::function<void()> m_OnClicked;
	std::unique_ptr<Widget> m_Child;
	mutable bool m_IsHovered = false;
	bool m_IsPressed = false;

	// Animation
	mutable CurveSequence m_HoverAnim;
	mutable float m_HoverAlpha = 0.0f;
	float m_HoverDuration = 0.12f;
	Easing m_HoverEasing = Easing::QuadEaseOut;
};


} // namespace wisp
