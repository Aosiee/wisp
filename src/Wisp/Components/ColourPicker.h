#pragma once

#include "Wisp/Core.h"
#include "Wisp/DrawList.h"
#include "Wisp/FontCache.h"
#include "Wisp/Application.h"
#include "Wisp/Style.h"
#include "Wisp/Theme.h"
#include "Wisp/ColourMath.h"
#include "Wisp/Components/BoxPanel.h"
#include "Wisp/Components/SizeBox.h"
#include "Wisp/Components/Spacer.h"
#include "Wisp/Components/TextBlock.h"
#include "Wisp/Components/Border.h"
#include "Wisp/Components/ValueBox.h"
#include "Wisp/Components/EditableText.h"
#include <algorithm>
#include <functional>
#include <memory>
#include <string>

namespace wisp {

template<typename T> class TValueBox;
class EditableText;

// ---- Shared state ----------------------------------------------------------
//
// Owned by the trigger ColourPicker and shared with its popup. The popup
// outlives the trigger safely (it may survive a panel rebuild that destroys
// the trigger): every popup widget captures this shared_ptr, never the
// trigger pointer.

// ---- Colour mode -----------------------------------------------------------

// RGB: alpha locked to 1.0 and the alpha bar hidden (opaque colours only).
// RGBA: full 4-channel editing (default).
enum class ColourMode : uint8_t { RGBA, RGB };

struct ColourPickerState
{
	glm::vec4 Colour{ 1.0f, 1.0f, 1.0f, 1.0f };
	std::function<void(const glm::vec4&)> OnChanged;    // continuous (drag/type)
	std::function<void(const glm::vec4&)> OnCommitted;  // once, popup closed
	bool Open = false;
	bool LockAlpha = false; // ColourMode::RGB - every mutation clamps a=1

	// Popup control handles, for in-place refresh when the colour changes
	// from another control. Non-owning; cleared by the popup's destructor
	// before the tree frees them.
	TValueBox<float>* BoxR = nullptr;
	TValueBox<float>* BoxG = nullptr;
	TValueBox<float>* BoxB = nullptr;
	EditableText* HexField = nullptr;
};

inline void UpdateColourPickerControls(ColourPickerState& s)
{
	if (s.BoxR) s.BoxR->SetValue(s.Colour.r);
	if (s.BoxG) s.BoxG->SetValue(s.Colour.g);
	if (s.BoxB) s.BoxB->SetValue(s.Colour.b);
	if (s.HexField)
	{
		std::string want = RgbaToHex(s.Colour);
		if (s.HexField->GetText() != want)
			s.HexField->SetText(want);
	}
}

inline void NotifyColourPickerChanged(const std::shared_ptr<ColourPickerState>& s)
{
	if (s->LockAlpha) s->Colour.a = 1.0f;
	UpdateColourPickerControls(*s);
	if (s->OnChanged) s->OnChanged(s->Colour);
}

// ---- Shared swatch painting ------------------------------------------------
//
// A checkerboard base with the colour painted over it at its own alpha:
// a semi-transparent colour reads as "colour + transparency" instead of
// vanishing into whatever is behind the swatch (the trigger swatch on the
// dark settings panel looked almost translucent for low-alpha config
// colours - the bug this fixes).

inline void PaintColourSwatch(DrawList& drawList, int layerId, const glm::vec2& pos, const glm::vec2& size,
	const glm::vec4& colour, float borderWidth, const glm::vec4& borderColour, int clip)
{
	const float tile = 4.0f;
	int cols = (int)std::ceil(size.x / tile);
	int rows = (int)std::ceil(size.y / tile);
	drawList.AddBox(layerId, pos, size, { 0.35f, 0.35f, 0.40f, 1.0f }, clip);
	for (int y = 0; y < rows; y++)
	{
		for (int x = (y % 2); x < cols; x += 2)
		{
			glm::vec2 p{ pos.x + x * tile, pos.y + y * tile };
			glm::vec2 s{ std::min(tile, size.x - (p.x - pos.x)), std::min(tile, size.y - (p.y - pos.y)) };
			drawList.AddBox(layerId, p, s, { 0.55f, 0.55f, 0.60f, 1.0f }, clip);
		}
	}
	float radii[4] = { 2, 2, 2, 2 };
	drawList.AddRoundedBox(layerId + 1, pos, size, colour, radii, borderWidth, borderColour, clip);
}

// ---- SV plane --------------------------------------------------------------

class ColourPickerPlane : public Widget
{
public:
	WISP_WIDGET_TYPE(ColourPickerPlane)

	explicit ColourPickerPlane(std::shared_ptr<ColourPickerState> state)
		: m_State(std::move(state)) {}

	glm::vec2 ComputeDesiredSize() const override { return m_Desired; }
	void SetDesiredSize(const glm::vec2& s) { m_Desired = s; }

	void ArrangeChildren(const Geometry&, ArrangedChildren&) const override {}

	int OnPaint(const PaintArgs& args, const Geometry& geom, DrawList& drawList, int layerId, const WidgetStyleContext&) const override
	{
		glm::vec2 absPos = geom.LocalToAbsolute({ 0, 0 });
		glm::vec2 size = geom.LocalSize;
		int clip = args.InheritedClipIndex;

		m_CachedPos = absPos;
		m_CachedSize = size;

		// True SV plane = horizontal white→hue ramp + a vertical black ramp
		// whose ALPHA goes 0 (top) → 1 (bottom). Both functions are linear
		// in one axis only, so the batcher's triangulated quads interpolate
		// them EXACTLY. (A single 4-corner gradient is only true-bilinear
		// along the TL-BR diagonal - the off-diagonal half of the plane
		// renders desaturated.)
		glm::vec3 hsv = RgbToHsv(glm::vec3(m_State->Colour));
		glm::vec3 pure = HsvToRgb({ hsv.x, 1.0f, 1.0f });
		glm::vec4 white{ 1.0f, 1.0f, 1.0f, 1.0f };
		glm::vec4 hueCol{ pure.r, pure.g, pure.b, 1.0f };
		drawList.AddGradient(layerId, absPos, size, white, hueCol, hueCol, white, clip);
		drawList.AddGradient(layerId + 1, absPos, size,
			{ 0, 0, 0, 0 }, { 0, 0, 0, 0 }, { 0, 0, 0, 1 }, { 0, 0, 0, 1 }, clip);

		// Selection marker: outer white + inner dark ring, visible on any
		// background.
		glm::vec2 centre = absPos + glm::vec2(hsv.y * size.x, (1.0f - hsv.z) * size.y);
		std::vector<glm::vec2> outer, inner;
		for (int i = 0; i <= 12; i++)
		{
			float a = (float)i / 12.0f * 6.2831853f;
			glm::vec2 d{ std::cos(a), std::sin(a) };
			outer.push_back(centre + d * 6.0f);
			inner.push_back(centre + d * 5.0f);
		}
		drawList.AddPolyline(layerId + 2, outer, 2.0f, { 1, 1, 1, 1 }, clip);
		drawList.AddPolyline(layerId + 2, inner, 1.5f, { 0, 0, 0, 0.8f }, clip);

		return layerId + 3;
	}

	Reply OnMouseButtonDown(const glm::vec2& localPos, int button) override
	{
		if (button != 0) return Reply::Unhandled();
		m_Dragging = true;
		UpdateFromPosition(localPos);
		return Reply::Handled_().CaptureMouse();
	}

	Reply OnMouseMove(const glm::vec2& localPos) override
	{
		if (!m_Dragging) return Reply::Unhandled();
		UpdateFromPosition(localPos);
		return Reply::Handled_();
	}

	Reply OnMouseButtonUp(const glm::vec2&, int button) override
	{
		if (button != 0) return Reply::Unhandled();
		m_Dragging = false;
		return Reply::Handled_().ReleaseCapture();
	}

private:
	void UpdateFromPosition(const glm::vec2& localPos)
	{
		if (m_CachedSize.x < 1.0f || m_CachedSize.y < 1.0f) return;
		float s = std::clamp(localPos.x / m_CachedSize.x, 0.0f, 1.0f);
		float v = 1.0f - std::clamp(localPos.y / m_CachedSize.y, 0.0f, 1.0f);
		glm::vec3 hsv = RgbToHsv(glm::vec3(m_State->Colour));
		glm::vec3 rgb = HsvToRgb({ hsv.x, s, v });
		glm::vec4 next{ rgb.r, rgb.g, rgb.b, m_State->Colour.a };
		if (next != m_State->Colour)
		{
			m_State->Colour = next;
			NotifyColourPickerChanged(m_State);
		}
	}

	std::shared_ptr<ColourPickerState> m_State;
	glm::vec2 m_Desired{ 160.0f, 160.0f };
	mutable glm::vec2 m_CachedPos{ 0, 0 };
	mutable glm::vec2 m_CachedSize{ 0, 0 };
	mutable bool m_Dragging = false;
};

// ---- 1-D ramp bar (hue / alpha) --------------------------------------------

class ColourPickerBar : public Widget
{
public:
	WISP_WIDGET_TYPE(ColourPickerBar)

	using RampPainter = std::function<void(DrawList&, int, const glm::vec2&, const glm::vec2&, int)>;

	ColourPickerBar(std::function<float()> getValue, std::function<void(float)> setValue,
		std::function<void(const glm::vec4&)> onChanged, RampPainter ramp, glm::vec4 thumbColour)
		: m_GetValue(std::move(getValue)), m_SetValue(std::move(setValue)),
		  m_OnChanged(std::move(onChanged)), m_Ramp(std::move(ramp)), m_ThumbColour(thumbColour) {}

	glm::vec2 ComputeDesiredSize() const override { return { 200.0f, 14.0f }; }
	void ArrangeChildren(const Geometry&, ArrangedChildren&) const override {}

	int OnPaint(const PaintArgs& args, const Geometry& geom, DrawList& drawList, int layerId, const WidgetStyleContext&) const override
	{
		glm::vec2 absPos = geom.LocalToAbsolute({ 0, 0 });
		glm::vec2 size = geom.LocalSize;
		int clip = args.InheritedClipIndex;
		m_CachedPos = absPos;
		m_CachedSize = size;

		if (m_Ramp) m_Ramp(drawList, layerId, absPos, size, clip);

		float v = m_GetValue ? m_GetValue() : 0.0f;
		float x = absPos.x + std::clamp(v, 0.0f, 1.0f) * (size.x - 3.0f);
		// White thumb column with dark 1px edges (visible on any ramp).
		drawList.AddBox(layerId + 1, { x, absPos.y }, { 3.0f, size.y }, m_ThumbColour, clip);
		drawList.AddBox(layerId + 1, { x - 1.0f, absPos.y }, { 1.0f, size.y }, { 0, 0, 0, 0.9f }, clip);
		drawList.AddBox(layerId + 1, { x + 3.0f, absPos.y }, { 1.0f, size.y }, { 0, 0, 0, 0.9f }, clip);

		return layerId + 2;
	}

	Reply OnMouseButtonDown(const glm::vec2& localPos, int button) override
	{
		if (button != 0) return Reply::Unhandled();
		m_Dragging = true;
		UpdateFromPosition(localPos.x);
		return Reply::Handled_().CaptureMouse();
	}

	Reply OnMouseMove(const glm::vec2& localPos) override
	{
		if (!m_Dragging) return Reply::Unhandled();
		UpdateFromPosition(localPos.x);
		return Reply::Handled_();
	}

	Reply OnMouseButtonUp(const glm::vec2&, int button) override
	{
		if (button != 0) return Reply::Unhandled();
		m_Dragging = false;
		return Reply::Handled_().ReleaseCapture();
	}

private:
	void UpdateFromPosition(float localX)
	{
		if (m_CachedSize.x < 1.0f) return;
		float v = std::clamp(localX / m_CachedSize.x, 0.0f, 1.0f);
		if (m_SetValue) m_SetValue(v);
		if (m_OnChanged) m_OnChanged({});
	}

	std::function<float()> m_GetValue;
	std::function<void(float)> m_SetValue;
	std::function<void(const glm::vec4&)> m_OnChanged;
	RampPainter m_Ramp;
	glm::vec4 m_ThumbColour{ 1, 1, 1, 1 };
	mutable glm::vec2 m_CachedPos{ 0, 0 };
	mutable glm::vec2 m_CachedSize{ 0, 0 };
	mutable bool m_Dragging = false;
};

// ---- Popup preview swatch --------------------------------------------------

class ColourPickerPreview : public Widget
{
public:
	WISP_WIDGET_TYPE(ColourPickerPreview)

	explicit ColourPickerPreview(std::shared_ptr<ColourPickerState> state)
		: m_State(std::move(state)) {}

	glm::vec2 ComputeDesiredSize() const override { return { 104.0f, 24.0f }; }
	void ArrangeChildren(const Geometry&, ArrangedChildren&) const override {}

	int OnPaint(const PaintArgs& args, const Geometry& geom, DrawList& drawList, int layerId, const WidgetStyleContext&) const override
	{
		glm::vec2 absPos = geom.LocalToAbsolute({ 0, 0 });
		int clip = args.InheritedClipIndex;
		PaintColourSwatch(drawList, layerId, absPos, geom.LocalSize, m_State->Colour,
			1.0f, { 0.4f, 0.4f, 0.5f, 1.0f }, clip);
		return layerId + 2;
	}

private:
	std::shared_ptr<ColourPickerState> m_State;
};

// ---- Popup root (fires OnColourCommitted on destruction) --------------------

class ColourPickerPopup : public Widget
{
public:
	WISP_WIDGET_TYPE(ColourPickerPopup)

	explicit ColourPickerPopup(std::shared_ptr<ColourPickerState> state)
		: m_State(std::move(state)) {}

	~ColourPickerPopup() override
	{
		if (!m_State) return;
		// Null the control handles first (the tree frees them right after
		// this destructor body), then report the final colour.
		m_State->BoxR = m_State->BoxG = m_State->BoxB = nullptr;
		m_State->HexField = nullptr;
		m_State->Open = false;
		if (m_State->OnCommitted) m_State->OnCommitted(m_State->Colour);
	}

	void SetContent(std::unique_ptr<Widget> content)
	{
		m_Content = std::move(content);
		if (m_Content) SetupParentChild(this, m_Content.get());
	}

	glm::vec2 ComputeDesiredSize() const override
	{
		return m_Content ? m_Content->ComputeDesiredSize() : glm::vec2{ 0, 0 };
	}

	void ArrangeChildren(const Geometry& geom, ArrangedChildren& out) const override
	{
		if (m_Content) out.Add(m_Content.get(), geom);
	}

	int OnPaint(const PaintArgs& args, const Geometry& geom, DrawList& drawList, int layerId, const WidgetStyleContext& style) const override
	{
		if (!m_Content) return layerId;
		WidgetStyleContext cs = style;
		cs.ColorAndOpacity = style.ColorAndOpacity * m_Content->GetRenderTint();
		cs.ColorAndOpacity.a *= m_Content->GetRenderOpacity();
		return m_Content->OnPaint(args, geom, drawList, layerId, cs);
	}

	Reply OnKeyDown(int key, int mods) override
	{
		if (key == 256)
		{
			UIApplication::Get().RequestDismissPopups();
			return Reply::Handled_();
		}
		return Reply::Unhandled();
	}

	void ForEachChild(std::function<void(Widget*)> fn) override
	{
		if (m_Content) fn(m_Content.get());
	}

private:
	std::shared_ptr<ColourPickerState> m_State;
	std::unique_ptr<Widget> m_Content;
};

// ---- ColourPicker (trigger + popup) ----------------------------------------

class ColourPicker : public Widget
{
public:
	WISP_WIDGET_TYPE(ColourPicker)

	ColourPicker(FontCache* fc, FontHandle font, float fontSize)
		: m_FontCache(fc), m_Font(font), m_FontSize(fontSize)
	{
		SetSupportsFocus(true);
		m_Style = GetDefaultStyle();
	}

	static ColourPickerStyle GetDefaultStyle()
	{
		ColourPickerStyle s;
		return s;
	}

	ColourPicker& SetColour(const glm::vec4& c)
	{
		m_State->Colour = c;
		if (m_State->LockAlpha) m_State->Colour.a = 1.0f;
		if (m_State->Open) NotifyColourPickerChanged(m_State);
		return *this;
	}
	glm::vec4 GetColour() const { return m_State->Colour; }
	ColourPicker& SetOnColourChanged(std::function<void(const glm::vec4&)> cb) { m_State->OnChanged = std::move(cb); return *this; }
	ColourPicker& SetOnColourCommitted(std::function<void(const glm::vec4&)> cb) { m_State->OnCommitted = std::move(cb); return *this; }

	// RGB mode locks alpha to 1.0 and hides the alpha bar; RGBA (default)
	// edits all four channels. SetShowAlpha is the pre-mode convenience.
	ColourPicker& SetColourMode(ColourMode mode)
	{
		m_State->LockAlpha = (mode == ColourMode::RGB);
		m_ShowAlpha = (mode == ColourMode::RGBA);
		if (m_State->LockAlpha && m_State->Colour.a != 1.0f)
		{
			m_State->Colour.a = 1.0f;
			if (m_State->OnChanged) m_State->OnChanged(m_State->Colour);
		}
		return *this;
	}
	ColourMode GetColourMode() const { return m_State->LockAlpha ? ColourMode::RGB : ColourMode::RGBA; }
	ColourPicker& SetShowAlpha(bool b) { return SetColourMode(b ? ColourMode::RGBA : ColourMode::RGB); }
	ColourPicker& SetShowHex(bool b) { m_ShowHex = b; return *this; }
	ColourPicker& SetShowRgb(bool b) { m_ShowRgb = b; return *this; }
	ColourPicker& SetSwatchSize(const glm::vec2& s) { m_SwatchSize = s; InvalidateLayout(); return *this; }

	ColourPicker& SetLabel(const std::string& label)
	{
		m_Label = label;
		m_LabelBlock = label.empty() ? nullptr
			: std::make_unique<TextBlock>(m_FontCache, m_Font, m_FontSize, label, m_Style.LabelTint);
		if (m_LabelBlock) SetupParentChild(this, m_LabelBlock.get());
		InvalidateLayout();
		return *this;
	}

	glm::vec2 ComputeDesiredSize() const override
	{
		glm::vec2 labelSize{ 0, 0 };
		if (m_LabelBlock)
		{
			labelSize = m_LabelBlock->ComputeDesiredSize();
			m_LabelBlock->SetCachedDesiredSize(labelSize);
		}
		return { labelSize.x + (labelSize.x > 0 ? 6.0f : 0.0f) + m_SwatchSize.x,
			std::max(labelSize.y, m_SwatchSize.y) };
	}

	void ArrangeChildren(const Geometry& geom, ArrangedChildren& out) const override
	{
		if (!m_LabelBlock) return;
		glm::vec2 labelSize = m_LabelBlock->ComputeDesiredSize();
		float y = (geom.LocalSize.y - labelSize.y) * 0.5f;
		out.Add(m_LabelBlock.get(), geom.MakeChild({ 0, y }, labelSize));
	}

	int OnPaint(const PaintArgs& args, const Geometry& geom, DrawList& drawList, int layerId, const WidgetStyleContext& style) const override
	{
		if (m_StyleDirty)
		{
			m_Style = ResolveStyle(this, GetDefaultStyle());
			m_StyleDirty = false;
		}

		glm::vec2 absPos = geom.LocalToAbsolute({ 0, 0 });
		m_CachedAbsPos = absPos;
		m_CachedSize = geom.LocalSize;

		int clip = args.InheritedClipIndex;
		glm::vec2 swatchPos = absPos;
		swatchPos.y += (geom.LocalSize.y - m_SwatchSize.y) * 0.5f;
		if (m_LabelBlock) swatchPos.x += m_LabelBlock->GetCachedDesiredSize().x + 6.0f;

		glm::vec4 border = m_Style.SwatchBorder;
		if (m_IsPressed) border = m_Style.SwatchBorderPressed;
		else if (m_IsHovered) border = m_Style.SwatchBorderHovered;
		PaintColourSwatch(drawList, layerId, swatchPos, m_SwatchSize, m_State->Colour,
			m_Style.SwatchBorderWidth, border, clip);

		if (m_LabelBlock)
		{
			ArrangedChildren arranged;
			ArrangeChildren(geom, arranged);
			WidgetStyleContext cs = style;
			cs.ColorAndOpacity = style.ColorAndOpacity * m_LabelBlock->GetRenderTint();
			cs.ColorAndOpacity.a *= m_LabelBlock->GetRenderOpacity();
			return m_LabelBlock->OnPaint(args, arranged[0].Geom, drawList, layerId + 2, cs);
		}
		return layerId + 2;
	}

	Reply OnMouseEnter() override { m_IsHovered = true; return Reply::Unhandled(); }
	Reply OnMouseLeave() override { m_IsHovered = false; m_IsPressed = false; return Reply::Unhandled(); }

	Reply OnMouseButtonDown(const glm::vec2&, int button) override
	{
		if (button != 0) return Reply::Unhandled();
		m_IsPressed = true;
		OpenPopup();
		return Reply::Handled_();
	}

	Reply OnMouseButtonUp(const glm::vec2&, int button) override
	{
		if (button != 0) return Reply::Unhandled();
		m_IsPressed = false;
		return Reply::Handled_();
	}

	bool SupportsFocus() const override { return m_SupportsFocus; }

	Reply OnKeyDown(int key, int mods) override
	{
		if (key == 257 || key == 32) // Enter or Space
		{
			OpenPopup();
			return Reply::Handled_();
		}
		if (key == 256 && m_State->Open) // Escape
		{
			UIApplication::Get().RequestDismissPopups();
			return Reply::Handled_();
		}
		return Reply::Unhandled();
	}

	void ForEachChild(std::function<void(Widget*)> fn) override
	{
		if (m_LabelBlock) fn(m_LabelBlock.get());
	}

private:
	void OpenPopup()
	{
		if (!UIApplication::IsValid() || m_State->Open) return;

		auto root = std::make_unique<ColourPickerPopup>(m_State);
		root->SetContent(BuildPopupContent());

		glm::vec2 desired = root->ComputeDesiredSize();
		glm::vec2 viewport = UIApplication::Get().GetViewportSize();
		glm::vec2 anchor{ m_CachedAbsPos.x, m_CachedAbsPos.y + m_CachedSize.y + 2.0f };
		if (anchor.y + desired.y > viewport.y - 4.0f && m_CachedAbsPos.y - desired.y - 2.0f > 0.0f)
			anchor.y = m_CachedAbsPos.y - desired.y - 2.0f;

		m_State->Open = true;
		UIApplication::Get().ShowPopupAt(std::move(root), anchor);
	}

	std::unique_ptr<Widget> BuildPopupContent()
	{
		auto state = m_State;

		auto border = std::make_unique<Border>();
		border->SetBrush({ m_Style.PopupBackground });
		border->SetPadding(m_Style.PopupPadding);

		auto vbox = std::make_unique<VerticalBox>(BoxOrientation::Vertical);

		// ---- Top row: plane | right column ----
		auto topRow = std::make_unique<HorizontalBox>(BoxOrientation::Horizontal);
		{
			auto plane = std::make_unique<ColourPickerPlane>(state);
			plane->SetDesiredSize(m_Style.PlaneSize);
			auto planeSize = std::make_unique<SizeBox>();
			planeSize->SetWidthOverride(m_Style.PlaneSize.x);
			planeSize->SetHeightOverride(m_Style.PlaneSize.y);
			planeSize->SetChild(std::move(plane));
			topRow->AddChild(std::move(planeSize));

			topRow->AddChild(std::make_unique<Spacer>(glm::vec2(8.0f, 1.0f)));

			auto rightCol = std::make_unique<VerticalBox>(BoxOrientation::Vertical);
			{
				auto previewSize = std::make_unique<SizeBox>();
				previewSize->SetWidthOverride(104.0f);
				previewSize->SetHeightOverride(24.0f);
				previewSize->SetChild(std::make_unique<ColourPickerPreview>(state));
				rightCol->AddChild(std::move(previewSize));

				if (m_ShowHex)
				{
					rightCol->AddChild(std::make_unique<Spacer>(glm::vec2(1.0f, 4.0f)));
					auto hex = std::make_unique<EditableText>(m_FontCache, m_Font, m_FontSize, RgbaToHex(state->Colour));
					hex->SetOnTextChanged([state](const std::string& text) {
						glm::vec4 parsed = state->Colour; // HexToRgba leaves a untouched for 6 digits
						if (HexToRgba(text, parsed))
						{
							// 8-digit input sets alpha; 6 digits keep it (RGB
							// mode clamps back to 1 in NotifyColourPickerChanged).
							if (parsed != state->Colour)
							{
								state->Colour = parsed;
								NotifyColourPickerChanged(state);
							}
						}
					});
					state->HexField = hex.get();
					auto hexSize = std::make_unique<SizeBox>();
					hexSize->SetWidthOverride(104.0f);
					hexSize->SetChild(std::move(hex));
					rightCol->AddChild(std::move(hexSize));
				}

				if (m_ShowRgb)
				{
					rightCol->AddChild(std::make_unique<Spacer>(glm::vec2(1.0f, 4.0f)));

					auto boxR = std::make_unique<TValueBox<float>>(m_FontCache, m_Font, m_FontSize, state->Colour.r);
					boxR->SetShowButtons(false);
					boxR->SetStep(0.01f);
					boxR->SetClampRange(0.0f, 1.0f);
					boxR->SetColourIndicator({ 1, 1, 1, 1 });
					boxR->SetIndicatorLabel("R");
					boxR->SetOnValueCommitted([state](float v) {
						if (state->Colour.r != v)
						{
							state->Colour.r = v;
							NotifyColourPickerChanged(state);
						}
					});
					state->BoxR = boxR.get();
					rightCol->AddChild(std::move(boxR));

					auto boxG = std::make_unique<TValueBox<float>>(m_FontCache, m_Font, m_FontSize, state->Colour.g);
					boxG->SetShowButtons(false);
					boxG->SetStep(0.01f);
					boxG->SetClampRange(0.0f, 1.0f);
					boxG->SetColourIndicator({ 1, 1, 1, 1 });
					boxG->SetIndicatorLabel("G");
					boxG->SetOnValueCommitted([state](float v) {
						if (state->Colour.g != v)
						{
							state->Colour.g = v;
							NotifyColourPickerChanged(state);
						}
					});
					state->BoxG = boxG.get();
					rightCol->AddChild(std::move(boxG));

					auto boxB = std::make_unique<TValueBox<float>>(m_FontCache, m_Font, m_FontSize, state->Colour.b);
					boxB->SetShowButtons(false);
					boxB->SetStep(0.01f);
					boxB->SetClampRange(0.0f, 1.0f);
					boxB->SetColourIndicator({ 1, 1, 1, 1 });
					boxB->SetIndicatorLabel("B");
					boxB->SetOnValueCommitted([state](float v) {
						if (state->Colour.b != v)
						{
							state->Colour.b = v;
							NotifyColourPickerChanged(state);
						}
					});
					state->BoxB = boxB.get();
					rightCol->AddChild(std::move(boxB));
				}
			}
			topRow->AddChild(std::move(rightCol));
		}
		vbox->AddChild(std::move(topRow));

		// ---- Hue bar ----
		vbox->AddChild(std::make_unique<Spacer>(glm::vec2(1.0f, 6.0f)));
		auto hueBar = std::make_unique<ColourPickerBar>(
			[state]() { return RgbToHsv(glm::vec3(state->Colour)).x; },
			[state](float h) {
				glm::vec3 hsv = RgbToHsv(glm::vec3(state->Colour));
				glm::vec3 rgb = HsvToRgb({ h, hsv.y, hsv.z });
				glm::vec4 next{ rgb.r, rgb.g, rgb.b, state->Colour.a };
				if (next != state->Colour)
				{
					state->Colour = next;
					NotifyColourPickerChanged(state);
				}
			},
			[](const glm::vec4&) {},
			[](DrawList& dl, int layer, const glm::vec2& pos, const glm::vec2& size, int clip) {
				static const glm::vec3 keys[7] = {
					{ 1, 0, 0 }, { 1, 1, 0 }, { 0, 1, 0 },
					{ 0, 1, 1 }, { 0, 0, 1 }, { 1, 0, 1 }, { 1, 0, 0 }
				};
				float segW = size.x / 6.0f;
				for (int i = 0; i < 6; i++)
				{
					glm::vec4 c0{ keys[i].r, keys[i].g, keys[i].b, 1.0f };
					glm::vec4 c1{ keys[i + 1].r, keys[i + 1].g, keys[i + 1].b, 1.0f };
					dl.AddGradient(layer, { pos.x + i * segW, pos.y }, { segW + 0.5f, size.y }, c0, c1, c1, c0, clip);
				}
			},
			m_Style.BarThumb);
		{
			auto hueSize = std::make_unique<SizeBox>();
			hueSize->SetWidthOverride(272.0f);
			hueSize->SetHeightOverride(m_Style.BarThickness);
			hueSize->SetChild(std::move(hueBar));
			vbox->AddChild(std::move(hueSize));
		}

		// ---- Alpha bar ----
		if (m_ShowAlpha)
		{
			vbox->AddChild(std::make_unique<Spacer>(glm::vec2(1.0f, 6.0f)));
			auto alphaBar = std::make_unique<ColourPickerBar>(
				[state]() { return state->Colour.a; },
				[state](float a) {
					if (state->Colour.a != a)
					{
						state->Colour.a = a;
						NotifyColourPickerChanged(state);
					}
				},
				[](const glm::vec4&) {},
				[state](DrawList& dl, int layer, const glm::vec2& pos, const glm::vec2& size, int clip) {
					const float tile = 4.0f;
					int cols = (int)std::ceil(size.x / tile);
					int rows = (int)std::ceil(size.y / tile);
					dl.AddBox(layer, pos, size, { 0.42f, 0.42f, 0.47f, 1.0f }, clip);
					for (int y = 0; y < rows; y++)
					{
						for (int x = (y % 2); x < cols; x += 2)
						{
							glm::vec2 p{ pos.x + x * tile, pos.y + y * tile };
							glm::vec2 s{ std::min(tile, size.x - (p.x - pos.x)), std::min(tile, size.y - (p.y - pos.y)) };
							dl.AddBox(layer, p, s, { 0.58f, 0.58f, 0.63f, 1.0f }, clip);
						}
					}
					glm::vec3 rgb(state->Colour);
					dl.AddGradient(layer + 1, pos, size,
						{ rgb.r, rgb.g, rgb.b, 0.0f }, { rgb.r, rgb.g, rgb.b, 0.0f },
						{ rgb.r, rgb.g, rgb.b, 1.0f }, { rgb.r, rgb.g, rgb.b, 1.0f }, clip);
				},
				m_Style.BarThumb);
			auto alphaSize = std::make_unique<SizeBox>();
			alphaSize->SetWidthOverride(272.0f);
			alphaSize->SetHeightOverride(m_Style.BarThickness);
			alphaSize->SetChild(std::move(alphaBar));
			vbox->AddChild(std::move(alphaSize));
		}

		border->SetChild(std::move(vbox));
		return border;
	}

	FontCache* m_FontCache = nullptr;
	FontHandle m_Font;
	float m_FontSize = 14.0f;
	std::shared_ptr<ColourPickerState> m_State = std::make_shared<ColourPickerState>();
	mutable ColourPickerStyle m_Style;
	glm::vec2 m_SwatchSize{ 20.0f, 16.0f };
	bool m_ShowAlpha = true;
	bool m_ShowHex = true;
	bool m_ShowRgb = true;
	std::string m_Label;
	std::unique_ptr<TextBlock> m_LabelBlock;
	mutable bool m_IsHovered = false;
	mutable bool m_IsPressed = false;
	mutable glm::vec2 m_CachedAbsPos{ 0, 0 };
	mutable glm::vec2 m_CachedSize{ 20.0f, 16.0f };
};

using SColourPicker = ColourPicker;

} // namespace wisp
