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
#include "stb_image.h"
#include <algorithm>
#include <cstdio>
#include <cstdlib>
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

// ---- Palette data ----------------------------------------------------------
//
// Recent + saved swatches, shared across every picker in the app (saved
// colours are per-editor, Unreal-style). The app layer provides Persist
// (save to disk) and the initial lists; the widgets only mutate the lists.

struct ColourPaletteData
{
	std::vector<glm::vec4> Recent;  // most recent first, capped
	std::vector<glm::vec4> Saved;   // the user's saved swatches
	std::function<void()> Persist;  // app-provided; empty = no persistence
};

// Samples the pixel under the OS cursor (whole screen - other windows AND
// the app's own canvas). Wayland/Hyprland route: `hyprctl cursorpos`
// (logical coords) + `grim -g "<x>,<y> 1x1"` (grim's geometry is logical
// and it scales the output itself). Returns {-1,-1,-1,-1} when sampling
// is unavailable (no hyprctl/grim, or capture failure).
inline glm::vec4 SampleScreenPixelAtCursor()
{
	glm::vec4 fallback{ -1.0f, -1.0f, -1.0f, -1.0f };
	int x = 0, y = 0;
	char buf[128];
	FILE* pipe = popen("hyprctl cursorpos 2>/dev/null", "r");
	if (!pipe) return fallback;
	bool got = fgets(buf, sizeof(buf), pipe) != nullptr;
	pclose(pipe);
	if (!got) return fallback;
	if (std::sscanf(buf, "%d, %d", &x, &y) != 2) return fallback;

	char cmd[320];
	std::snprintf(cmd, sizeof(cmd), "grim -g \"%d,%d 1x1\" /tmp/wisp_eyedrop.png 2>/dev/null", x, y);
	if (std::system(cmd) != 0) return fallback;

	int w = 0, h = 0, n = 0;
	unsigned char* data = stbi_load("/tmp/wisp_eyedrop.png", &w, &h, &n, 4);
	if (!data) return fallback;
	glm::vec4 col{ data[0] / 255.0f, data[1] / 255.0f, data[2] / 255.0f, data[3] / 255.0f };
	stbi_image_free(data);
	std::remove("/tmp/wisp_eyedrop.png");
	return col;
}

struct ColourPickerState
{
	glm::vec4 Colour{ 1.0f, 1.0f, 1.0f, 1.0f };
	bool EyedropArmed = false; // the crosshair + screen-pick are live
	std::function<void(const glm::vec4&)> OnChanged;    // continuous (drag/type)
	std::function<void(const glm::vec4&)> OnCommitted;  // once, popup closed
	bool Open = false;
	bool LockAlpha = false; // ColourMode::RGB - every mutation clamps a=1

	// The palette (recent + saved swatches), shared across pickers so every
	// picker in the app sees the same saved colours. Owned by the app layer.
	std::shared_ptr<class ColourPaletteData> Palette;

	// Popup control handles, for in-place refresh when the colour changes
	// from another control. Non-owning; cleared by the popup's destructor
	// before the tree frees them.
	TValueBox<float>* BoxR = nullptr;
	TValueBox<float>* BoxG = nullptr;
	TValueBox<float>* BoxB = nullptr;
	TValueBox<float>* BoxA = nullptr;      // RGBA mode only
	TValueBox<float>* OpacityBox = nullptr; // 0-100, the opacity slider's box
	TValueBox<float>* HueBox = nullptr;    // 0-360
	TValueBox<float>* SatBox = nullptr;    // 0-100
	TValueBox<float>* BriBox = nullptr;    // 0-100
	TValueBox<float>* CyanBox = nullptr;   // 0-100
	TValueBox<float>* MagentaBox = nullptr; // 0-100
	TValueBox<float>* YellowBox = nullptr;  // 0-100
	TValueBox<float>* KeyBox = nullptr;     // 0-100
	EditableText* HexField = nullptr;
};

inline void UpdateColourPickerControls(ColourPickerState& s)
{
	auto setBox = [](TValueBox<float>* box, float value) {
		if (box) box->SetValue(value);
	};
	setBox(s.BoxR, std::round(s.Colour.r * 255.0f));
	setBox(s.BoxG, std::round(s.Colour.g * 255.0f));
	setBox(s.BoxB, std::round(s.Colour.b * 255.0f));
	setBox(s.BoxA, std::round(s.Colour.a * 255.0f));
	setBox(s.OpacityBox, std::round(s.Colour.a * 100.0f));
	glm::vec3 hsv = RgbToHsv(glm::vec3(s.Colour));
	setBox(s.HueBox, std::fmod(std::round(hsv.x * 360.0f), 360.0f));
	setBox(s.SatBox, std::round(hsv.y * 100.0f));
	setBox(s.BriBox, std::round(hsv.z * 100.0f));
	// The HSB indicator swatches track the live values: H shows the pure
	// hue, S the hue at the current saturation (full brightness), B the
	// pure hue at the current brightness.
	if (s.HueBox) s.HueBox->SetColourIndicator({ HsvToRgb({ hsv.x, 1.0f, 1.0f }), 1.0f });
	if (s.SatBox) s.SatBox->SetColourIndicator({ HsvToRgb({ hsv.x, hsv.y, 1.0f }), 1.0f });
	if (s.BriBox) s.BriBox->SetColourIndicator({ HsvToRgb({ hsv.x, 1.0f, hsv.z }), 1.0f });
	glm::vec4 cmyk = RgbToCmyk(glm::vec3(s.Colour));
	setBox(s.CyanBox, std::round(cmyk.x * 100.0f));
	setBox(s.MagentaBox, std::round(cmyk.y * 100.0f));
	setBox(s.YellowBox, std::round(cmyk.z * 100.0f));
	setBox(s.KeyBox, std::round(cmyk.w * 100.0f));
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

// ---- Triangle + hue ring wheel ----------------------------------------------
//
// The classic Unreal-style wheel: a hue RING around a triangle whose three
// corners are the pure hue (top), black (bottom-left) and white
// (bottom-right). Dragging on the ring sets hue; clicking inside the
// triangle sets S/V (linear barycentric interpolation, rendered exactly by
// the batcher's per-vertex-colour triangles).

class ColourPickerWheel : public Widget
{
public:
	WISP_WIDGET_TYPE(ColourPickerWheel)

	explicit ColourPickerWheel(std::shared_ptr<ColourPickerState> state)
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

		float side = std::min(size.x, size.y);
		glm::vec2 centre = absPos + size * 0.5f;
		float outerR = side * 0.5f - 2.0f;
		float ringW = std::max(9.0f, side * 0.08f);
		float triR = outerR - ringW;
		m_Centre = centre;
		m_RingW = ringW;
		m_TriR = triR;

		glm::vec3 hsv = RgbToHsv(glm::vec3(m_State->Colour));

		// Hue ring (segmented, clockwise from 12 o'clock).
		const int N = 60;
		for (int i = 0; i < N; i++)
		{
			float t0 = (float)i / N, t1 = (float)(i + 1) / N;
			float a0 = t0 * 6.2831853f - 1.5707963f;
			float a1 = t1 * 6.2831853f - 1.5707963f;
			glm::vec4 c0{ HsvToRgb({ t0, 1.0f, 1.0f }), 1.0f };
			glm::vec4 c1{ HsvToRgb({ t1, 1.0f, 1.0f }), 1.0f };
			glm::vec2 in0 = centre + glm::vec2{ std::cos(a0), std::sin(a0) } * triR;
			glm::vec2 in1 = centre + glm::vec2{ std::cos(a1), std::sin(a1) } * triR;
			glm::vec2 out0 = centre + glm::vec2{ std::cos(a0), std::sin(a0) } * outerR;
			glm::vec2 out1 = centre + glm::vec2{ std::cos(a1), std::sin(a1) } * outerR;
			drawList.AddTriangle(layerId, in0, c0, out0, c0, out1, c1, clip);
			drawList.AddTriangle(layerId, in0, c0, out1, c1, in1, c1, clip);
		}

		// The SV triangle: hue / black / white corners.
		glm::vec4 hueC{ HsvToRgb({ hsv.x, 1.0f, 1.0f }), 1.0f };
		glm::vec2 top = centre + glm::vec2{ 0.0f, -triR };
		glm::vec2 bl = centre + glm::vec2{ -triR * 0.8660254f, triR * 0.5f };
		glm::vec2 br = centre + glm::vec2{ triR * 0.8660254f, triR * 0.5f };
		m_Top = top; m_BL = bl; m_BR = br;
		drawList.AddTriangle(layerId, top, hueC, bl, { 0.0f,0.0f,0.0f,1.0f }, br, { 1.0f,1.0f,1.0f,1.0f }, clip);

		// Hue marker on the ring.
		float ha = hsv.x * 6.2831853f - 1.5707963f;
		glm::vec2 hm = centre + glm::vec2{ std::cos(ha), std::sin(ha) } * (triR + ringW * 0.5f);
		DrawMarker(drawList, layerId + 1, hm, 5.0f, clip);
		// S/V marker inside the triangle.
		DrawMarker(drawList, layerId + 1, TriPoint(hsv.y, hsv.z), 4.5f, clip);
		return layerId + 2;
	}

	Reply OnMouseButtonDown(const glm::vec2& screenPos, int button) override
	{
		if (button != 0) return Reply::Unhandled();
		m_Dragging = true;
		UpdateFromPosition(screenPos);
		return Reply::Handled_().CaptureMouse();
	}

	Reply OnMouseMove(const glm::vec2& screenPos) override
	{
		if (!m_Dragging) return Reply::Unhandled();
		UpdateFromPosition(screenPos);
		return Reply::Handled_();
	}

	Reply OnMouseButtonUp(const glm::vec2&, int button) override
	{
		if (button != 0) return Reply::Unhandled();
		m_Dragging = false;
		return Reply::Handled_().ReleaseCapture();
	}

private:
	// The triangle point for (S, V): S*V*hue + (1-V)*black + (1-S)*V*white.
	glm::vec2 TriPoint(float s, float v) const
	{
		return m_Top * (s * v) + m_BL * (1.0f - v) + m_BR * ((1.0f - s) * v);
	}

	void DrawMarker(DrawList& dl, int layer, const glm::vec2& c, float r, int clip) const
	{
		std::vector<glm::vec2> outer, inner;
		for (int i = 0; i <= 12; i++)
		{
			float a = (float)i / 12.0f * 6.2831853f;
			glm::vec2 d{ std::cos(a), std::sin(a) };
			outer.push_back(c + d * r);
			inner.push_back(c + d * (r - 1.0f));
		}
		dl.AddPolyline(layer, outer, 2.0f, { 1.0f,1.0f,1.0f,1.0f }, clip);
		dl.AddPolyline(layer, inner, 1.0f, { 0.0f,0.0f,0.0f,0.9f }, clip);
	}

	// The router delivers ABSOLUTE screen coordinates (see the old plane's
	// fix) - convert to local before mapping.
	void UpdateFromPosition(const glm::vec2& screenPos)
	{
		if (m_CachedSize.x < 1.0f || m_CachedSize.y < 1.0f || m_TriR <= 0.0f) return;
		glm::vec2 local = screenPos - m_CachedPos;
		glm::vec2 v = local - m_CachedSize * 0.5f;
		float dist = glm::length(v);
		glm::vec3 hsv = RgbToHsv(glm::vec3(m_State->Colour));

		bool changed = false;
		if (dist > m_TriR + 1.0f && dist <= m_TriR + m_RingW + 1.0f)
		{
			// Ring: angle -> hue.
			float a = std::atan2(v.y, v.x) + 1.5707963f;
			if (a < 0.0f) a += 6.2831853f;
			hsv.x = a / 6.2831853f;
			changed = true;
		}
		else
		{
			// Triangle: barycentric -> S/V. `v` is centre-relative; the
			// vertices are stored absolute - convert them to the same
			// centre-relative space first (the bug that made every
			// triangle click reject).
			const glm::vec2 a = m_Top - m_Centre;
			const glm::vec2 b = m_BL - m_Centre;
			const glm::vec2 c = m_BR - m_Centre;
			float denom = (b.y - c.y) * (a.x - c.x) + (c.x - b.x) * (a.y - c.y);
			if (std::abs(denom) > 1e-6f)
			{
				float wa = ((b.y - c.y) * (v.x - c.x) + (c.x - b.x) * (v.y - c.y)) / denom;
				float wb = ((c.y - a.y) * (v.x - c.x) + (a.x - c.x) * (v.y - c.y)) / denom;
				float wc = 1.0f - wa - wb;
				if (wa >= -0.01f && wb >= -0.01f && wc >= -0.01f)
				{
					float sum = wa + wc;
					hsv.y = sum > 1e-4f ? wa / sum : 0.0f;
					hsv.z = wa + wc;
					changed = true;
				}
			}
		}

		if (!changed) return;
		glm::vec3 rgb = HsvToRgb(hsv);
		glm::vec4 next{ rgb.r, rgb.g, rgb.b, m_State->Colour.a };
		if (next != m_State->Colour)
		{
			m_State->Colour = next;
			NotifyColourPickerChanged(m_State);
		}
	}

	std::shared_ptr<ColourPickerState> m_State;
	glm::vec2 m_Desired{ 180.0f, 180.0f };
	mutable glm::vec2 m_CachedPos{ 0, 0 };
	mutable glm::vec2 m_CachedSize{ 0, 0 };
	mutable glm::vec2 m_Centre{ 0, 0 };
	mutable glm::vec2 m_Top{ 0, 0 }, m_BL{ 0, 0 }, m_BR{ 0, 0 };
	mutable float m_RingW = 14.0f;
	mutable float m_TriR = 70.0f;
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
	// Same absolute->local conversion as the plane (see its comment).
	void UpdateFromPosition(float screenX)
	{
		if (m_CachedSize.x < 1.0f) return;
		float local = screenX - m_CachedPos.x;
		float v = std::clamp(local / m_CachedSize.x, 0.0f, 1.0f);
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

// ---- Eyedropper ------------------------------------------------------------
//
// One click arms a global "next click anywhere samples the OS pixel under
// the cursor" mode (the app's widget-pick mode; the popup stays open).
// The sampled colour lands in the shared state and every control refreshes.

class ColourPickerEyedropper : public Widget
{
public:
	WISP_WIDGET_TYPE(ColourPickerEyedropper)

	explicit ColourPickerEyedropper(std::shared_ptr<ColourPickerState> state)
		: m_State(std::move(state)) {}

	glm::vec2 ComputeDesiredSize() const override { return { 22.0f, 20.0f }; }
	void ArrangeChildren(const Geometry&, ArrangedChildren&) const override {}

	int OnPaint(const PaintArgs& args, const Geometry& geom, DrawList& drawList, int layerId, const WidgetStyleContext&) const override
	{
		glm::vec2 absPos = geom.LocalToAbsolute({ 0, 0 });
		int clip = args.InheritedClipIndex;
		if (m_Hovered)
			drawList.AddBox(layerId, absPos, geom.LocalSize, { 0.2f, 0.35f, 0.6f, 1.0f }, clip);

		// Pipette icon: a bulb (circle) + a tip (line) pointing top-right.
		glm::vec2 bulb{ absPos.x + 8.0f, absPos.y + 12.0f };
		std::vector<glm::vec2> ring;
		for (int i = 0; i <= 12; i++)
		{
			float a = (float)i / 12.0f * 6.2831853f;
			ring.push_back(bulb + glm::vec2{ std::cos(a), std::sin(a) } * 4.0f);
		}
		drawList.AddPolyline(layerId + 1, ring, 1.5f, { 0.85f, 0.85f, 0.9f, 1.0f }, clip);
		drawList.AddLine(layerId + 1, { bulb.x + 3.0f, bulb.y - 3.0f }, { absPos.x + 16.0f, absPos.y + 2.0f },
			2.0f, { 0.85f, 0.85f, 0.9f, 1.0f }, clip);
		return layerId + 2;
	}

	Reply OnMouseEnter() override { m_Hovered = true; return Reply::Unhandled(); }
	Reply OnMouseLeave() override { m_Hovered = false; return Reply::Unhandled(); }

	Reply OnMouseButtonDown(const glm::vec2&, int button) override
	{
		if (button != 0) return Reply::Unhandled();
		if (!UIApplication::IsValid()) return Reply::Unhandled();
		UIApplication& app = UIApplication::Get();
		// The next click ANYWHERE (the popup stays open - the screen-pick
		// capture consumes it before popup dismissal) samples the OS pixel
		// under the cursor; the cursor becomes a crosshair while armed. If
		// the popup closed before that click, the armed capture is a no-op
		// (and still releases the crosshair).
		auto state = m_State;
		state->EyedropArmed = true;
		app.SetCursorOverride(CursorType::Crosshair);
		app.SetScreenPickCallback([state]() {
			state->EyedropArmed = false;
			if (UIApplication::IsValid()) UIApplication::Get().ClearCursorOverride();
			if (!state->Open) return;
			glm::vec4 c = SampleScreenPixelAtCursor();
			if (c.a >= 0.0f)
			{
				state->Colour = c;
				NotifyColourPickerChanged(state);
			}
		});
		return Reply::Handled_();
	}

private:
	std::shared_ptr<ColourPickerState> m_State;
	mutable bool m_Hovered = false;
};

// ---- Palette side panel -----------------------------------------------------
//
// Two vertical strips: the recent swatches (click = select, drag = copy into
// the saved bar) and the saved bar (click a segment = select, right-click =
// remove, drop = insert at the segment). Recent and saved live in the shared
// ColourPaletteData, persisted by the app layer.

class ColourPickerPalette : public Widget
{
public:
	WISP_WIDGET_TYPE(ColourPickerPalette)

	ColourPickerPalette(std::shared_ptr<ColourPickerState> state, std::shared_ptr<ColourPaletteData> palette)
		: m_State(std::move(state)), m_Palette(std::move(palette)) {}

	glm::vec2 ComputeDesiredSize() const override { return m_Desired; }
	void SetDesiredSize(const glm::vec2& s) { m_Desired = s; }
	void ArrangeChildren(const Geometry&, ArrangedChildren&) const override {}

	int OnPaint(const PaintArgs& args, const Geometry& geom, DrawList& drawList, int layerId, const WidgetStyleContext&) const override
	{
		glm::vec2 absPos = geom.LocalToAbsolute({ 0, 0 });
		int clip = args.InheritedClipIndex;

		// Recent: fixed vertical line of swatches.
		float cell = std::min(geom.LocalSize.x - 6.0f, 20.0f);
		float x = absPos.x + 2.0f;
		float y = absPos.y + 2.0f;
		m_RecentRects.clear();		const int cap = 10;
		for (int i = 0; i < (int)m_Palette->Recent.size() && i < cap; i++)
		{
			m_RecentRects.push_back({ x, y, cell, cell });
			PaintColourSwatch(drawList, layerId, { x, y }, { cell, cell }, m_Palette->Recent[i],
				1.0f, { 0.45f, 0.45f, 0.55f, 1.0f }, clip);
			y += cell + 2.0f;
		}

		// Saved: a vertical bar subdivided into segments.
		float sx = x + cell + 6.0f;
		float sw = std::max(6.0f, geom.LocalSize.x - (sx - absPos.x) - 2.0f);
		float sh = geom.LocalSize.y - 4.0f;
		m_SavedRects.clear();
		if (m_Palette->Saved.empty())
		{
			drawList.AddBox(layerId, { sx, absPos.y + 2.0f }, { sw, sh }, { 0.12f, 0.12f, 0.17f, 1.0f }, clip);
		}
		else
		{
			float segH = sh / (float)m_Palette->Saved.size();
			for (int i = 0; i < (int)m_Palette->Saved.size(); i++)
			{
				glm::vec4 r{ sx, absPos.y + 2.0f + segH * (float)i, sw, segH - 1.0f };
				m_SavedRects.push_back(r);
				drawList.AddBox(layerId, { r.x, r.y }, { r.z, std::max(1.0f, r.w) }, m_Palette->Saved[i], clip);
			}
			if (m_DragIndex >= 0 && m_DragIndex < (int)m_SavedRects.size())
			{
				const glm::vec4& r = m_SavedRects[m_DragIndex];
				drawList.AddBox(layerId + 1, { r.x, r.y }, { r.z, r.w }, { 1.0f, 1.0f, 1.0f, 0.35f }, clip);
			}
		}
		return layerId + 2;
	}

	Reply OnMouseButtonDown(const glm::vec2& absPos, int button) override
	{
		if (button == 0)
		{
			int recent = HitRecent(absPos);
			if (recent >= 0 && recent < (int)m_Palette->Recent.size())
			{
				m_State->Colour = m_Palette->Recent[recent];
				NotifyColourPickerChanged(m_State);
				glm::vec4 colour = m_Palette->Recent[recent];
				return Reply::Handled_().CaptureMouse().DetectDrag([colour]() {
					DragPayload pl;
					pl.Type = "ColourSwatch";
					pl.Data = colour;
					pl.PaintGhost = [colour](DrawList& dl, int layer, const glm::vec2& cursor) {
						dl.AddBox(layer, cursor + glm::vec2(2.0f, 2.0f), { 14.0f, 14.0f }, colour, -1);
						float radii[4] = { 2.0f, 2.0f, 2.0f, 2.0f };
						dl.AddRoundedBox(layer + 1, cursor + glm::vec2(1.0f, 1.0f), { 16.0f, 16.0f },
							{ 0, 0, 0, 0 }, radii, 1.0f, { 1.0f, 1.0f, 1.0f, 1.0f }, -1);
					};
					return pl;
				});
			}
			int saved = HitSaved(absPos);
			if (saved >= 0 && saved < (int)m_Palette->Saved.size())
			{
				m_State->Colour = m_Palette->Saved[saved];
				NotifyColourPickerChanged(m_State);
				return Reply::Handled_();
			}
		}
		else if (button == 1)
		{
			int saved = HitSaved(absPos);
			if (saved >= 0 && saved < (int)m_Palette->Saved.size())
			{
				m_Palette->Saved.erase(m_Palette->Saved.begin() + saved);
				if (m_Palette->Persist) m_Palette->Persist();
				return Reply::Handled_();
			}
		}
		return Reply::Unhandled();
	}

	Reply OnDragOver(DragOperation& op) override
	{
		if (op.Payload.Type != "ColourSwatch") return Reply::Unhandled();
		m_DragIndex = SavedIndexAt(op.CurrentPos);
		return Reply::Handled_();
	}

	Reply OnDragLeave(DragOperation& op) override
	{
		(void)op;
		m_DragIndex = -1;
		return Reply::Unhandled();
	}

	Reply OnDrop(DragOperation& op) override
	{
		if (op.Payload.Type != "ColourSwatch") return Reply::Unhandled();
		glm::vec4 colour = std::any_cast<glm::vec4>(op.Payload.Data);
		int idx = SavedIndexAt(op.CurrentPos);
		if (idx < 0 || m_Palette->Saved.empty())
			m_Palette->Saved.push_back(colour);
		else
			m_Palette->Saved.insert(m_Palette->Saved.begin() + idx, colour);
		if (m_Palette->Persist) m_Palette->Persist();
		m_DragIndex = -1;
		return Reply::Handled_();
	}

private:
	int HitRecent(const glm::vec2& p) const
	{
		for (int i = 0; i < (int)m_RecentRects.size(); i++)
		{
			const glm::vec4& r = m_RecentRects[i];
			if (p.x >= r.x && p.x <= r.x + r.z && p.y >= r.y && p.y <= r.y + r.w)
				return i;
		}
		return -1;
	}
	int HitSaved(const glm::vec2& p) const
	{
		for (int i = 0; i < (int)m_SavedRects.size(); i++)
		{
			const glm::vec4& r = m_SavedRects[i];
			if (p.x >= r.x && p.x <= r.x + r.z && p.y >= r.y && p.y <= r.y + r.w)
				return i;
		}
		return -1;
	}
	int SavedIndexAt(const glm::vec2& p) const
	{
		if (m_SavedRects.empty()) return -1;
		if (p.y <= m_SavedRects.front().y) return 0;
		if (p.y >= m_SavedRects.back().y + m_SavedRects.back().w) return (int)m_SavedRects.size();
		for (int i = 0; i < (int)m_SavedRects.size(); i++)
		{
			const glm::vec4& r = m_SavedRects[i];
			if (p.y >= r.y && p.y <= r.y + r.w) return i;
		}
		return (int)m_SavedRects.size();
	}

	std::shared_ptr<ColourPickerState> m_State;
	std::shared_ptr<ColourPaletteData> m_Palette;
	glm::vec2 m_Desired{ 48.0f, 220.0f };
	mutable std::vector<glm::vec4> m_RecentRects;
	mutable std::vector<glm::vec4> m_SavedRects;
	mutable int m_DragIndex = -1;
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
		// this destructor body), then report the final colour.
		m_State->BoxR = m_State->BoxG = m_State->BoxB = m_State->BoxA = m_State->OpacityBox = nullptr;
		m_State->HueBox = m_State->SatBox = m_State->BriBox = nullptr;
		m_State->CyanBox = m_State->MagentaBox = m_State->YellowBox = m_State->KeyBox = nullptr;
		m_State->HexField = nullptr;
		m_State->Open = false;
		// An eyedropper still armed at close time (e.g. Escape) must not
		// linger: drop the screen-pick and restore the cursor.
		if (m_State->EyedropArmed && UIApplication::IsValid())
		{
			UIApplication& app = UIApplication::Get();
			app.CancelScreenPick();
			app.ClearCursorOverride();
			m_State->EyedropArmed = false;
		}
		// Recent palette: the committed colour moves to the front (deduped,
		// capped) and the app layer persists.
		if (m_State->Palette)
		{
			auto& recent = m_State->Palette->Recent;
			recent.erase(std::remove_if(recent.begin(), recent.end(),
				[&](const glm::vec4& c) { return c == m_State->Colour; }), recent.end());
			recent.insert(recent.begin(), m_State->Colour);
			if (recent.size() > 12) recent.resize(12);
			if (m_State->Palette->Persist) m_State->Palette->Persist();
		}
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

	Reply OnKeyDown(int key, int) override
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

	// The shared recent/saved palette (per-app; see ColourPaletteData).
	ColourPicker& SetPalette(std::shared_ptr<ColourPaletteData> palette)
	{
		m_State->Palette = std::move(palette);
		return *this;
	}

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

	Reply OnKeyDown(int key, int) override
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

		auto hbox = std::make_unique<HorizontalBox>(BoxOrientation::Horizontal);

		// ---- Main column ----
		auto mainCol = std::make_unique<VerticalBox>(BoxOrientation::Vertical);
		{
			// The wheel: hue ring around the SV triangle.
			auto wheel = std::make_unique<ColourPickerWheel>(state);
			wheel->SetDesiredSize(m_Style.WheelSize);
			auto wheelSize = std::make_unique<SizeBox>();
			wheelSize->SetWidthOverride(m_Style.WheelSize.x);
			wheelSize->SetHeightOverride(m_Style.WheelSize.y);
			wheelSize->SetChild(std::move(wheel));
			mainCol->AddChild(std::move(wheelSize));

			// Preview + eyedropper + hex.
			mainCol->AddChild(std::make_unique<Spacer>(glm::vec2(1.0f, 6.0f)));
			auto infoRow = std::make_unique<HorizontalBox>(BoxOrientation::Horizontal);
			{
				auto previewSize = std::make_unique<SizeBox>();
				previewSize->SetWidthOverride(64.0f);
				previewSize->SetHeightOverride(20.0f);
				previewSize->SetChild(std::make_unique<ColourPickerPreview>(state));
				infoRow->AddChild(std::move(previewSize));
				infoRow->AddChild(std::make_unique<Spacer>(glm::vec2(4.0f, 1.0f)));
				infoRow->AddChild(std::make_unique<ColourPickerEyedropper>(state));
				if (m_ShowHex)
				{
					infoRow->AddChild(std::make_unique<Spacer>(glm::vec2(4.0f, 1.0f)));
					auto hex = std::make_unique<EditableText>(m_FontCache, m_Font, m_FontSize, RgbaToHex(state->Colour));
					hex->SetOnTextChanged([state](const std::string& text) {
						glm::vec4 parsed = state->Colour;
						if (HexToRgba(text, parsed))
						{
							if (parsed != state->Colour)
							{
								state->Colour = parsed;
								NotifyColourPickerChanged(state);
							}
						}
					});
					state->HexField = hex.get();
					auto hexSize = std::make_unique<SizeBox>();
					hexSize->SetWidthOverride(88.0f);
					hexSize->SetChild(std::move(hex));
					infoRow->AddChild(std::move(hexSize));
				}
			}
			mainCol->AddChild(std::move(infoRow));

			// ---- Sub-panel: RGBA + CMYK raw rows (slightly smaller,
			// "behind" the main part) ----
			{
				mainCol->AddChild(std::make_unique<Spacer>(glm::vec2(1.0f, 6.0f)));
				auto sub = std::make_unique<Border>();
				sub->SetBrush({ glm::vec4(0.065f, 0.065f, 0.10f, 1.0f) });
				sub->SetPadding(4);
				auto subV = std::make_unique<VerticalBox>(BoxOrientation::Vertical);

				float smallFont = m_FontSize * 0.85f;
				auto makeChannelBox = [&](std::unique_ptr<HorizontalBox>& row, const char* label,
					const glm::vec4& tint, float minV, float maxV, float init,
					std::function<void(float)> commit, TValueBox<float>*& outHandle) {
					auto box = std::make_unique<TValueBox<float>>(m_FontCache, m_Font, smallFont, init);
					box->SetShowButtons(false);
					box->SetStep(1.0f);
					box->SetClampRange(minV, maxV);
					box->SetColourIndicator(tint);
					box->SetIndicatorLabel(label);
					box->SetMinimumWidth(46.0f);
					box->SetOnValueCommitted([state, commit](float v) { commit(v); });
					outHandle = box.get();
					row->AddChild(std::move(box));
					row->AddChild(std::make_unique<Spacer>(glm::vec2(3.0f, 1.0f)));
				};

				// RGBA — 0-255 per channel.
				if (m_ShowRgb)
				{
					auto row = std::make_unique<HorizontalBox>(BoxOrientation::Horizontal);
					makeChannelBox(row, "R", { 0.9f,0.25f,0.25f,1.0f }, 0.0f, 255.0f, state->Colour.r * 255.0f,
						[state](float v) {
							if (state->Colour.r != v / 255.0f) { state->Colour.r = v / 255.0f; NotifyColourPickerChanged(state); }
						}, state->BoxR);
					makeChannelBox(row, "G", { 0.25f,0.85f,0.3f,1.0f }, 0.0f, 255.0f, state->Colour.g * 255.0f,
						[state](float v) {
							if (state->Colour.g != v / 255.0f) { state->Colour.g = v / 255.0f; NotifyColourPickerChanged(state); }
						}, state->BoxG);
					makeChannelBox(row, "B", { 0.3f,0.45f,0.95f,1.0f }, 0.0f, 255.0f, state->Colour.b * 255.0f,
						[state](float v) {
							if (state->Colour.b != v / 255.0f) { state->Colour.b = v / 255.0f; NotifyColourPickerChanged(state); }
						}, state->BoxB);
					if (m_ShowAlpha)
					{
						makeChannelBox(row, "A", { 1.0f,1.0f,1.0f,1.0f }, 0.0f, 255.0f, state->Colour.a * 255.0f,
							[state](float v) {
								if (state->Colour.a != v / 255.0f) { state->Colour.a = v / 255.0f; NotifyColourPickerChanged(state); }
							}, state->BoxA);
					}
					subV->AddChild(std::move(row));
				}

				// CMYK — 0-100 per channel.
				{
					auto row = std::make_unique<HorizontalBox>(BoxOrientation::Horizontal);
					glm::vec4 cmyk = RgbToCmyk(glm::vec3(state->Colour));
					auto cmykCommit = [state](glm::vec4::length_type channel, float v) {
						glm::vec4 cur = RgbToCmyk(glm::vec3(state->Colour));
						cur[channel] = v / 100.0f;
						glm::vec3 rgb = CmykToRgb(cur);
						state->Colour = { rgb.r, rgb.g, rgb.b, state->Colour.a };
						NotifyColourPickerChanged(state);
					};
					makeChannelBox(row, "C", { 0.2f,0.8f,0.9f,1.0f }, 0.0f, 100.0f, cmyk.x * 100.0f,
						[cmykCommit](float v) { cmykCommit(0, v); }, state->CyanBox);
					makeChannelBox(row, "M", { 0.9f,0.2f,0.7f,1.0f }, 0.0f, 100.0f, cmyk.y * 100.0f,
						[cmykCommit](float v) { cmykCommit(1, v); }, state->MagentaBox);
					makeChannelBox(row, "Y", { 0.95f,0.85f,0.2f,1.0f }, 0.0f, 100.0f, cmyk.z * 100.0f,
						[cmykCommit](float v) { cmykCommit(2, v); }, state->YellowBox);
					makeChannelBox(row, "K", { 0.35f,0.35f,0.4f,1.0f }, 0.0f, 100.0f, cmyk.w * 100.0f,
						[cmykCommit](float v) { cmykCommit(3, v); }, state->KeyBox);
					subV->AddChild(std::move(row));
				}

				sub->SetChild(std::move(subV));
				mainCol->AddChild(std::move(sub));
			}

			// ---- Sliders: colour (hue), opacity (alpha), then HSB with
			// value boxes on the right ----
			auto sliderRow = [&](std::unique_ptr<ColourPickerBar> bar, float barW, std::unique_ptr<TValueBox<float>> box) {
				auto row = std::make_unique<HorizontalBox>(BoxOrientation::Horizontal);
				auto barSize = std::make_unique<SizeBox>();
				barSize->SetWidthOverride(barW);
				barSize->SetHeightOverride(m_Style.BarThickness);
				barSize->SetChild(std::move(bar));
				row->AddChild(std::move(barSize));
				row->AddChild(std::make_unique<Spacer>(glm::vec2(6.0f, 1.0f)));
				row->AddChild(std::move(box));
				return row;
			};
			auto channelBox = [&](TValueBox<float>*& handle, const char* label, const glm::vec4& tint,
				float minV, float maxV, float init, std::function<void(float)> commit) {
				auto box = std::make_unique<TValueBox<float>>(m_FontCache, m_Font, m_FontSize, init);
				box->SetShowButtons(false);
				box->SetStep(1.0f);
				box->SetClampRange(minV, maxV);
				box->SetColourIndicator(tint);
				box->SetIndicatorLabel(label);
				box->SetMinimumWidth(58.0f);
				box->SetOnValueCommitted([state, commit](float v) { commit(v); });
				handle = box.get();
				return box;
			};

			// Colour slider (hue).
			mainCol->AddChild(std::make_unique<Spacer>(glm::vec2(1.0f, 6.0f)));
			{
				auto hueBar = std::make_unique<ColourPickerBar>(
					[state]() { return RgbToHsv(glm::vec3(state->Colour)).x; },
					[state](float h) {
						glm::vec3 hsv = RgbToHsv(glm::vec3(state->Colour));
						hsv.x = h;
						glm::vec3 rgb = HsvToRgb(hsv);
						glm::vec4 next{ rgb.r, rgb.g, rgb.b, state->Colour.a };
						if (next != state->Colour) { state->Colour = next; NotifyColourPickerChanged(state); }
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
				auto size = std::make_unique<SizeBox>();
				size->SetWidthOverride(272.0f);
				size->SetHeightOverride(m_Style.BarThickness);
				size->SetChild(std::move(hueBar));
				mainCol->AddChild(std::move(size));
			}

			// Opacity (alpha) slider + value box.
			if (m_ShowAlpha)
			{
				mainCol->AddChild(std::make_unique<Spacer>(glm::vec2(1.0f, 4.0f)));
				auto alphaBar = std::make_unique<ColourPickerBar>(
					[state]() { return state->Colour.a; },
					[state](float a) {
						if (state->Colour.a != a) { state->Colour.a = a; NotifyColourPickerChanged(state); }
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
								glm::vec2 sz{ std::min(tile, size.x - (p.x - pos.x)), std::min(tile, size.y - (p.y - pos.y)) };
								dl.AddBox(layer, p, sz, { 0.58f, 0.58f, 0.63f, 1.0f }, clip);
							}
						}
						glm::vec3 rgb(state->Colour);
						dl.AddGradient(layer + 1, pos, size,
							{ rgb.r, rgb.g, rgb.b, 0.0f }, { rgb.r, rgb.g, rgb.b, 0.0f },
							{ rgb.r, rgb.g, rgb.b, 1.0f }, { rgb.r, rgb.g, rgb.b, 1.0f }, clip);
					},
					m_Style.BarThumb);
				auto box = channelBox(state->OpacityBox, "A", { 1.0f,1.0f,1.0f,1.0f }, 0.0f, 100.0f, state->Colour.a * 100.0f,
					[state](float v) {
						if (state->Colour.a != v / 100.0f) { state->Colour.a = v / 100.0f; NotifyColourPickerChanged(state); }
					});
				mainCol->AddChild(sliderRow(std::move(alphaBar), 190.0f, std::move(box)));
			}

			// HSB sliders with value boxes on the right.
			{
				mainCol->AddChild(std::make_unique<Spacer>(glm::vec2(1.0f, 4.0f)));
				glm::vec3 hsv = RgbToHsv(glm::vec3(state->Colour));
				auto hsvCommit = [state](int channel, float v) {
					glm::vec3 cur = RgbToHsv(glm::vec3(state->Colour));
					cur[channel] = v;
					glm::vec3 rgb = HsvToRgb(cur);
					glm::vec4 next{ rgb.r, rgb.g, rgb.b, state->Colour.a };
					if (next != state->Colour) { state->Colour = next; NotifyColourPickerChanged(state); }
				};

				auto hueRow = std::make_unique<ColourPickerBar>(
					[state]() { return RgbToHsv(glm::vec3(state->Colour)).x; },
					[hsvCommit](float v) { hsvCommit(0, v); },
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
				auto hBox = channelBox(state->HueBox, "H", { 1.0f,0.55f,0.15f,1.0f }, 0.0f, 360.0f, hsv.x * 360.0f,
					[hsvCommit](float v) { hsvCommit(0, v / 360.0f); });
				mainCol->AddChild(sliderRow(std::move(hueRow), 190.0f, std::move(hBox)));

				mainCol->AddChild(std::make_unique<Spacer>(glm::vec2(1.0f, 4.0f)));
				auto satRow = std::make_unique<ColourPickerBar>(
					[state]() { return RgbToHsv(glm::vec3(state->Colour)).y; },
					[hsvCommit](float v) { hsvCommit(1, v); },
					[](const glm::vec4&) {},
					[state](DrawList& dl, int layer, const glm::vec2& pos, const glm::vec2& size, int clip) {
						glm::vec3 hsv = RgbToHsv(glm::vec3(state->Colour));
						glm::vec3 pure = HsvToRgb({ hsv.x, 1.0f, hsv.z });
						glm::vec4 c0{ 1,1,1,1 }, c1{ pure.r, pure.g, pure.b, 1.0f };
						dl.AddGradient(layer, pos, size, c0, c1, c1, c0, clip);
					},
					m_Style.BarThumb);
				auto sBox = channelBox(state->SatBox, "S", { 0.4f,0.85f,1.0f,1.0f }, 0.0f, 100.0f, hsv.y * 100.0f,
					[hsvCommit](float v) { hsvCommit(1, v / 100.0f); });
				mainCol->AddChild(sliderRow(std::move(satRow), 190.0f, std::move(sBox)));

				mainCol->AddChild(std::make_unique<Spacer>(glm::vec2(1.0f, 4.0f)));
				auto briRow = std::make_unique<ColourPickerBar>(
					[state]() { return RgbToHsv(glm::vec3(state->Colour)).z; },
					[hsvCommit](float v) { hsvCommit(2, v); },
					[](const glm::vec4&) {},
					[state](DrawList& dl, int layer, const glm::vec2& pos, const glm::vec2& size, int clip) {
						glm::vec3 hsv = RgbToHsv(glm::vec3(state->Colour));
						glm::vec3 pure = HsvToRgb({ hsv.x, 1.0f, 1.0f });
						glm::vec4 c0{ 0,0,0,1 }, c1{ pure.r, pure.g, pure.b, 1.0f };
						dl.AddGradient(layer, pos, size, c0, c0, c1, c1, clip);
					},
					m_Style.BarThumb);
				auto bBox = channelBox(state->BriBox, "B", { 0.75f,0.8f,0.9f,1.0f }, 0.0f, 100.0f, hsv.z * 100.0f,
					[hsvCommit](float v) { hsvCommit(2, v / 100.0f); });
				mainCol->AddChild(sliderRow(std::move(briRow), 190.0f, std::move(bBox)));
			}
		}
		// ---- Left: the palette side panel (recent + saved bars) ----
		// Sized to the main column's full height, so the saved bar grows
		// down with the picker instead of leaving dead space.
		if (m_State->Palette)
		{
			float mainH = mainCol->ComputeDesiredSize().y;
			auto side = std::make_unique<Border>();
			side->SetBrush({ glm::vec4(0.07f, 0.07f, 0.11f, 1.0f) });
			side->SetPadding(2);
			auto pal = std::make_unique<ColourPickerPalette>(state, m_State->Palette);
			auto palSize = std::make_unique<SizeBox>();
			palSize->SetWidthOverride(48.0f);
			palSize->SetHeightOverride(std::max(120.0f, mainH - 4.0f));
			palSize->SetChild(std::move(pal));
			side->SetChild(std::move(palSize));
			hbox->AddChild(std::move(side));
			hbox->AddChild(std::make_unique<Spacer>(glm::vec2(8.0f, 1.0f)));
		}

		hbox->AddChild(std::move(mainCol));

		// Sync the indicator swatches + hex once so the popup opens with
		// live values (the HSB indicators track the current H/S/B).
		UpdateColourPickerControls(*state);

		border->SetChild(std::move(hbox));
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
