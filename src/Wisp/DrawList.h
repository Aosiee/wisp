#pragma once

#include <cstdint>
#include <glm/glm.hpp>
#include <string>
#include <vector>

namespace wisp {

// ---- Draw Element Kinds ----------------------------------------------------

enum class DrawElementKind : uint8_t
{
	Box,         // solid colour quad
	Text,        // glyph quads (atlas UVs + offsets)
	Image,       // textured quad
	RoundedBox,  // SDF rounded rectangle
	NineSlice,   // nine-slice panel (expands to 9 textured quads)
	Line,        // single line segment (extruded to quad)
	Polyline,    // connected line strips (extruded with bevel joins)
	Gradient,    // quad with 4 per-vertex colours
	Triangle,    // 3 per-vertex colours, linear interpolation
};

// ---- Brush (for Image / Border backgrounds) --------------------------------

struct WispBrush
{
	glm::vec4  Tint          = { 1.0f, 1.0f, 1.0f, 1.0f };
	glm::vec2  UVMin         = { 0.0f, 0.0f };
	glm::vec2  UVMax         = { 1.0f, 1.0f };
	uint64_t   TextureId     = 0;
	bool       HasTexture    = false;
	bool operator==(const WispBrush&) const = default;
};

using Brush = WispBrush; // alias for compatibility

// ---- Nine-slice margins -----------------------------------------------------

struct NineSliceMargins
{
	float Left = 0.0f;   // UV fraction
	float Top = 0.0f;
	float Right = 0.0f;
	float Bottom = 0.0f;
};

// ---- Line style ------------------------------------------------------------

enum class LineStyle : uint8_t
{
	Solid,
	Dashed,
	Dot,
	DashDot,
};

// ---- Draw Element ----------------------------------------------------------

struct DrawElement
{
	DrawElementKind Kind = DrawElementKind::Box;
	int             LayerId = 0;
	glm::vec2       Position = { 0.0f, 0.0f };
	glm::vec2       Size = { 0.0f, 0.0f };
	glm::vec4       Tint = { 1.0f, 1.0f, 1.0f, 1.0f };
	int             ClipIndex = -1; // index into DrawList's clip stack; -1 = no clip

	// Box / Image
	Brush           ImageBrush;

	// Text
	std::string     TextString;      // copy at paint time
	glm::vec2       TextAtlasUV[4] = { { 0.0f, 0.0f }, { 0.0f, 0.0f }, { 0.0f, 0.0f }, { 0.0f, 0.0f } }; // glyph quad UVs in atlas
	float           TextAdvance = 0.0f;
	float           TextHeight = 0.0f;   // glyph quad height in pixels

	// RoundedBox
	float           CornerRadii[4] = { 0.0f, 0.0f, 0.0f, 0.0f }; // TL, TR, BR, BL
	float           OutlineWidth = 0.0f;
	glm::vec4       OutlineColour = { 0.0f, 0.0f, 0.0f, 0.0f };

	// NineSlice
	NineSliceMargins NineSliceMargin;

	// Line
	glm::vec2       LineP0 = { 0.0f, 0.0f };
	glm::vec2       LineP1 = { 0.0f, 0.0f };
	float           LineThickness = 1.0f;
	LineStyle       LineStroke = LineStyle::Solid;

	// Polyline
	std::vector<glm::vec2> PolyPoints;
	float           PolyThickness = 1.0f;
	LineStyle       PolyStroke = LineStyle::Solid;

	// Gradient
	glm::vec4       GradientColorTL = { 1.0f, 1.0f, 1.0f, 1.0f };
	glm::vec4       GradientColorTR = { 1.0f, 1.0f, 1.0f, 1.0f };
	glm::vec4       GradientColorBR = { 1.0f, 1.0f, 1.0f, 1.0f };
	glm::vec4       GradientColorBL = { 1.0f, 1.0f, 1.0f, 1.0f };

	// Triangle
	glm::vec2       TriP[3] = { {0,0}, {0,0}, {0,0} };
	glm::vec4       TriC[3] = { {1,1,1,1}, {1,1,1,1}, {1,1,1,1} };
};

// ---- Draw List -------------------------------------------------------------

struct DrawList
{
	std::vector<DrawElement> Elements;
	std::vector<glm::vec4>   ClipRects; // x, y, w, h per clip index

	void AddBox(int layerId, const glm::vec2& pos, const glm::vec2& size,
		const glm::vec4& tint, int clipIndex)
	{
		DrawElement e;
		e.Kind = DrawElementKind::Box;
		e.LayerId = layerId;
		e.Position = pos;
		e.Size = size;
		e.Tint = tint;
		e.ClipIndex = clipIndex;
		Elements.push_back(e);
	}

	void AddImage(int layerId, const glm::vec2& pos, const glm::vec2& size,
		const Brush& brush, int clipIndex)
	{
		DrawElement e;
		e.Kind = DrawElementKind::Image;
		e.LayerId = layerId;
		e.Position = pos;
		e.Size = size;
		e.Tint = brush.Tint;
		e.ImageBrush = brush;
		e.ClipIndex = clipIndex;
		Elements.push_back(e);
	}

	void AddText(int layerId, const glm::vec2& pos, const std::string& text,
		const glm::vec2 atlasUVs[4], float advance, float height, const glm::vec4& tint, int clipIndex)
	{
		DrawElement e;
		e.Kind = DrawElementKind::Text;
		e.LayerId = layerId;
		e.Position = pos;
		e.Size = { advance, height };
		e.Tint = tint;
		e.TextString = text;
		for (int i = 0; i < 4; ++i) e.TextAtlasUV[i] = atlasUVs[i];
		e.TextAdvance = advance;
		e.TextHeight = height;
		e.ClipIndex = clipIndex;
		Elements.push_back(e);
	}

	void AddRoundedBox(int layerId, const glm::vec2& pos, const glm::vec2& size,
		const glm::vec4& tint, const float radii[4], float outlineWidth,
		const glm::vec4& outlineColour, int clipIndex)
	{
		DrawElement e;
		e.Kind = DrawElementKind::RoundedBox;
		e.LayerId = layerId;
		e.Position = pos;
		e.Size = size;
		e.Tint = tint;
		for (int i = 0; i < 4; ++i) e.CornerRadii[i] = radii[i];
		e.OutlineWidth = outlineWidth;
		e.OutlineColour = outlineColour;
		e.ClipIndex = clipIndex;
		Elements.push_back(e);
	}

	void AddNineSlice(int layerId, const glm::vec2& pos, const glm::vec2& size,
		const Brush& brush, const NineSliceMargins& margins, const glm::vec4& tint, int clipIndex)
	{
		DrawElement e;
		e.Kind = DrawElementKind::NineSlice;
		e.LayerId = layerId;
		e.Position = pos;
		e.Size = size;
		e.Tint = tint;
		e.ImageBrush = brush;
		e.NineSliceMargin = margins;
		e.ClipIndex = clipIndex;
		Elements.push_back(e);
	}

	void AddLine(int layerId, const glm::vec2& p0, const glm::vec2& p1,
		float thickness, const glm::vec4& tint, int clipIndex,
		LineStyle style = LineStyle::Solid)
	{
		if (style == LineStyle::Solid)
		{
			DrawElement e;
			e.Kind = DrawElementKind::Line;
			e.LayerId = layerId;
			e.Tint = tint;
			e.LineP0 = p0;
			e.LineP1 = p1;
			e.LineThickness = thickness;
			e.LineStroke = style;
			e.ClipIndex = clipIndex;
			Elements.push_back(e);
		}
		else
		{
			float dashLen, gapLen;
			GetDashParams(thickness, style, dashLen, gapLen);
			float totalLen = glm::length(p1 - p0);
			if (totalLen < 0.001f) return;
			glm::vec2 dir = (p1 - p0) / totalLen;
			float t = 0.0f;
			bool on = true;
			while (t < totalLen)
			{
				float segLen = on ? dashLen : gapLen;
				if (t + segLen > totalLen) segLen = totalLen - t;
				if (on && segLen > 0.001f)
				{
					DrawElement e;
					e.Kind = DrawElementKind::Line;
					e.LayerId = layerId;
					e.Tint = tint;
					e.LineP0 = p0 + dir * t;
					e.LineP1 = p0 + dir * (t + segLen);
					e.LineThickness = thickness;
					e.LineStroke = LineStyle::Solid;
					e.ClipIndex = clipIndex;
					Elements.push_back(e);
				}
				t += segLen;
				on = !on;
			}
		}
	}

	void AddPolyline(int layerId, const std::vector<glm::vec2>& points,
		float thickness, const glm::vec4& tint, int clipIndex,
		LineStyle style = LineStyle::Solid)
	{
		if (style == LineStyle::Solid)
		{
			DrawElement e;
			e.Kind = DrawElementKind::Polyline;
			e.LayerId = layerId;
			e.Tint = tint;
			e.PolyPoints = points;
			e.PolyThickness = thickness;
			e.PolyStroke = style;
			e.ClipIndex = clipIndex;
			Elements.push_back(e);
		}
		else
		{
			float dashLen, gapLen;
			GetDashParams(thickness, style, dashLen, gapLen);
			// Break each segment independently into dash/gap sub-segments
			for (size_t i = 0; i + 1 < points.size(); i++)
			{
				glm::vec2 seg = points[i+1] - points[i];
				float segLen = glm::length(seg);
				if (segLen < 0.001f) continue;
				glm::vec2 dir = seg / segLen;
				float t = 0.0f;
				bool on = true;
				while (t < segLen)
				{
					float step = on ? dashLen : gapLen;
					if (t + step > segLen) step = segLen - t;
					if (on && step > 0.001f)
					{
						std::vector<glm::vec2> dash = { points[i] + dir * t, points[i] + dir * (t + step) };
						DrawElement e;
						e.Kind = DrawElementKind::Polyline;
						e.LayerId = layerId;
						e.Tint = tint;
						e.PolyPoints = dash;
						e.PolyThickness = thickness;
						e.PolyStroke = LineStyle::Solid;
						e.ClipIndex = clipIndex;
						Elements.push_back(e);
					}
					t += step;
					on = !on;
				}
			}
		}
	}

private:
	static void GetDashParams(float thickness, LineStyle style, float& dashLen, float& gapLen)
	{
		float base = std::max(thickness * 4.0f, 4.0f);
		switch (style)
		{
		case LineStyle::Dashed:  dashLen = base * 2.0f; gapLen = base * 1.0f; break;
		case LineStyle::Dot:     dashLen = base * 0.4f; gapLen = base * 0.8f; break;
		case LineStyle::DashDot: dashLen = base * 2.0f; gapLen = base * 0.8f; break;
		default: dashLen = 0; gapLen = 0; break;
		}
	}

public:

	void AddGradient(int layerId, const glm::vec2& pos, const glm::vec2& size,
		const glm::vec4& colorTL, const glm::vec4& colorTR,
		const glm::vec4& colorBR, const glm::vec4& colorBL, int clipIndex)
	{
		DrawElement e;
		e.Kind = DrawElementKind::Gradient;
		e.LayerId = layerId;
		e.Position = pos;
		e.Size = size;
		e.GradientColorTL = colorTL;
		e.GradientColorTR = colorTR;
		e.GradientColorBR = colorBR;
		e.GradientColorBL = colorBL;
		e.ClipIndex = clipIndex;
		Elements.push_back(e);
	}

	// A triangle with per-vertex colours (linear interpolation - the
	// colour-picker wheel's hue/white/black triangle renders exactly).
	void AddTriangle(int layerId,
		const glm::vec2& p0, const glm::vec4& c0,
		const glm::vec2& p1, const glm::vec4& c1,
		const glm::vec2& p2, const glm::vec4& c2, int clipIndex)
	{
		DrawElement e;
		e.Kind = DrawElementKind::Triangle;
		e.LayerId = layerId;
		e.TriP[0] = p0; e.TriC[0] = c0;
		e.TriP[1] = p1; e.TriC[1] = c1;
		e.TriP[2] = p2; e.TriC[2] = c2;
		e.ClipIndex = clipIndex;
		Elements.push_back(e);
	}

	// Glyph with an 8-direction outline behind it (classic text outline).
	// The outline copies draw on `layerId`, the glyph on top at layerId+1.
	void AddTextOutlined(int layerId, const glm::vec2& pos, const std::string& text,
		const glm::vec2 atlasUVs[4], float advance, float height,
		const glm::vec4& tint, const glm::vec4& outlineTint, float outlineThickness, int clipIndex)
	{
		static const float dirs[8][2] = {
			{ 1, 0 }, { 1, 1 }, { 0, 1 }, { -1, 1 },
			{ -1, 0 }, { -1, -1 }, { 0, -1 }, { 1, -1 }
		};
		for (const auto& d : dirs)
			AddText(layerId, { pos.x + d[0] * outlineThickness, pos.y + d[1] * outlineThickness },
				text, atlasUVs, advance, height, outlineTint, clipIndex);
		AddText(layerId + 1, pos, text, atlasUVs, advance, height, tint, clipIndex);
	}

	void AddDropShadow(int layerId, const glm::vec2& pos, const glm::vec2& size,
		const glm::vec4& tint, float blurRadius, const glm::vec2& offset, int clipIndex)
	{
		float radii[4] = { blurRadius, blurRadius, blurRadius, blurRadius };
		AddRoundedBox(layerId, pos + offset, size, tint, radii, 0.0f, {0,0,0,0}, clipIndex);
	}

	int PushClip(const glm::vec4& rect)
	{
		return PushClip(rect, -1);
	}

	// Push a clip rect intersected with the inherited clip (pass -1 for none).
	// Clips must nest: a child's scissor is always the intersection of its own
	// rect and its ancestors', otherwise content inside a clipped container
	// can paint outside that container (e.g. scrolled content bleeding over a
	// neighbouring dock pane).
	int PushClip(const glm::vec4& rect, int inheritedIdx)
	{
		glm::vec4 final = rect;
		if (inheritedIdx >= 0 && inheritedIdx < (int)ClipRects.size())
		{
			const glm::vec4& p = ClipRects[inheritedIdx];
			float x0 = std::max(rect.x, p.x);
			float y0 = std::max(rect.y, p.y);
			float x1 = std::min(rect.x + rect.z, p.x + p.z);
			float y1 = std::min(rect.y + rect.w, p.y + p.w);
			final = { x0, y0, std::max(0.0f, x1 - x0), std::max(0.0f, y1 - y0) };
		}
		int idx = (int)ClipRects.size();
		ClipRects.push_back(final);
		return idx;
	}

	void Clear()
	{
		Elements.clear();
		ClipRects.clear();
	}

	bool IsEmpty() const { return Elements.empty(); }
};

// ---- Clipping helper -------------------------------------------------------

inline int ApplyWidgetClipping(const Widget* widget, DrawList& drawList, int inheritedIdx,
	const glm::vec2& absPos, const glm::vec2& size)
{
	Clipping c = widget->GetClipping();
	if (c == Clipping::Inherit) return inheritedIdx;

	bool shouldClip = (c != Clipping::OnDemand);
	if (c == Clipping::OnDemand)
	{
		glm::vec2 desired = widget->GetCachedDesiredSize();
		shouldClip = (desired.x > size.x || desired.y > size.y);
	}
	if (!shouldClip) return inheritedIdx;

	if (c == Clipping::ClipToBoundsWithoutIntersecting)
		return drawList.PushClip({ absPos.x, absPos.y, size.x, size.y }, -1);
	// ClipToBounds / ClipToBoundsAlways / OnDemand: intersect with parent.
	return drawList.PushClip({ absPos.x, absPos.y, size.x, size.y }, inheritedIdx);
}

} // namespace wisp
