#pragma once

#include "Wisp/DrawList.h"
#include "Wisp/Renderer.h"
#include <algorithm>
#include <cstring>

namespace wisp {

inline void MakeVertex(BatchedVertex& v, float x, float y, float u, float vv,
	uint8_t r, uint8_t g, uint8_t b, uint8_t a)
{
	v.pos[0] = x; v.pos[1] = y; v.pos[2] = 0.0f;
	v.uv[0] = u; v.uv[1] = vv;
	v.col[0] = r; v.col[1] = g; v.col[2] = b; v.col[3] = a;
}

inline BatchedDrawData BatchDrawList(const DrawList& drawList, TinyEngine::FrameAllocator* arena, uint32_t slot)
{
	// The per-frame batch output builds into the frame arena
	// (one slot's worth - the slot is reset by the caller before the
	// build). A null arena falls back to the global allocator.
	using VA = TinyEngine::FrameArenaAllocator<BatchedVertex>;
	using IA = TinyEngine::FrameArenaAllocator<uint32_t>;
	using BA = TinyEngine::FrameArenaAllocator<BatchedBatch>;
	using CA = TinyEngine::FrameArenaAllocator<glm::vec4>;
	BatchedDrawData output{
		std::vector<BatchedVertex, VA>(VA(arena, slot)),
		std::vector<uint32_t, IA>(IA(arena, slot)),
		std::vector<BatchedBatch, BA>(BA(arena, slot)),
		std::vector<glm::vec4, CA>(CA(arena, slot)),
		1.0f,
	};
	output.ClipRects.assign(drawList.ClipRects.begin(), drawList.ClipRects.end());

	std::vector<int> indices;
	for (size_t i = 0; i < drawList.Elements.size(); ++i)
		indices.push_back((int)i);

	std::stable_sort(indices.begin(), indices.end(), [&](int a, int b) {
		const auto& ea = drawList.Elements[a];
		const auto& eb = drawList.Elements[b];
		auto texGroup = [](DrawElementKind k) -> uint64_t {
			if (k == DrawElementKind::Text) return 2;
			if (k == DrawElementKind::RoundedBox) return 1;
			return 0; // Box, Image, NineSlice, Line, Polyline, Gradient
		};
		uint64_t ta = texGroup(ea.Kind);
		uint64_t tb = texGroup(eb.Kind);
		if (ea.LayerId != eb.LayerId) return ea.LayerId < eb.LayerId;
		if (ta != tb) return ta < tb;
		return false;
	});


	uint64_t currentTexture = UINT64_MAX;
	int      currentClip = -1;
	SdfParams currentSdf{};
	BatchedBatch currentBatch;

	for (int idx : indices)
	{
		const auto& el = drawList.Elements[idx];
		uint64_t tex;
		if (el.Kind == DrawElementKind::Text) tex = 1;
		else if (el.Kind == DrawElementKind::RoundedBox) tex = 2;
		else if (el.Kind == DrawElementKind::Image && el.ImageBrush.TextureId != 0) tex = el.ImageBrush.TextureId;
		else tex = 0;

		bool sdfDiffers = false;
		if (el.Kind == DrawElementKind::RoundedBox)
		{
			// All fields must be set before the memcmp - Size was
			// previously left uninitialised, so mixed-size rounded boxes
			// (e.g. a 20x16 picker swatch and a 104x24 popup preview) could
			// merge into one batch carrying the FIRST element's halfSize
			// push constants, making the others render their SDF at the
			// wrong size (invisible fills).
			SdfParams s{};
			for (int i = 0; i < 4; i++) s.CornerRadii[i] = el.CornerRadii[i];
			s.OutlineWidth = el.OutlineWidth;
			s.OutlineColour[0] = el.OutlineColour.r; s.OutlineColour[1] = el.OutlineColour.g;
			s.OutlineColour[2] = el.OutlineColour.b; s.OutlineColour[3] = el.OutlineColour.a;
			s.Size[0] = el.Size.x;
			s.Size[1] = el.Size.y;
			if (tex != currentTexture || el.ClipIndex != currentClip ||
				memcmp(&s, &currentSdf, sizeof(SdfParams)) != 0)
				sdfDiffers = true;
		}

		if (tex != currentTexture || el.ClipIndex != currentClip || sdfDiffers)
		{
			if (currentBatch.IndexCount > 0)
				output.Batches.push_back(currentBatch);

			currentTexture = tex;
			currentClip = el.ClipIndex;
			currentBatch.TextureId = tex;
			currentBatch.ClipIndex = el.ClipIndex;
			currentBatch.IndexOffset = (uint32_t)output.Indices.size();
			currentBatch.IndexCount = 0;
			currentBatch.Sdf = {};
			if (el.Kind == DrawElementKind::RoundedBox)
			{
				for (int i = 0; i < 4; i++) currentBatch.Sdf.CornerRadii[i] = el.CornerRadii[i];
				currentBatch.Sdf.OutlineWidth = el.OutlineWidth;
				currentBatch.Sdf.OutlineColour[0] = el.OutlineColour.r;
				currentBatch.Sdf.OutlineColour[1] = el.OutlineColour.g;
				currentBatch.Sdf.OutlineColour[2] = el.OutlineColour.b;
				currentBatch.Sdf.OutlineColour[3] = el.OutlineColour.a;
				currentBatch.Sdf.Size[0] = el.Size.x;
				currentBatch.Sdf.Size[1] = el.Size.y;
				memcpy(&currentSdf, &currentBatch.Sdf, sizeof(SdfParams));
			}
		}

		auto emitQuad = [&](float x0, float y0, float x1, float y1,
			float uMin, float vMin, float uMax, float vMax,
			uint8_t cTL_r, uint8_t cTL_g, uint8_t cTL_b, uint8_t cTL_a,
			uint8_t cTR_r, uint8_t cTR_g, uint8_t cTR_b, uint8_t cTR_a,
			uint8_t cBR_r, uint8_t cBR_g, uint8_t cBR_b, uint8_t cBR_a,
			uint8_t cBL_r, uint8_t cBL_g, uint8_t cBL_b, uint8_t cBL_a)
		{
			uint32_t baseVertex = (uint32_t)output.Vertices.size();
			BatchedVertex v[4];
			MakeVertex(v[0], x0, y0, uMin, vMin, cTL_r, cTL_g, cTL_b, cTL_a);
			MakeVertex(v[1], x1, y0, uMax, vMin, cTR_r, cTR_g, cTR_b, cTR_a);
			MakeVertex(v[2], x1, y1, uMax, vMax, cBR_r, cBR_g, cBR_b, cBR_a);
			MakeVertex(v[3], x0, y1, uMin, vMax, cBL_r, cBL_g, cBL_b, cBL_a);
			output.Vertices.push_back(v[0]);
			output.Vertices.push_back(v[1]);
			output.Vertices.push_back(v[2]);
			output.Vertices.push_back(v[3]);
			uint32_t i6[6] = { baseVertex, baseVertex+1, baseVertex+2, baseVertex, baseVertex+2, baseVertex+3 };
			for (int i = 0; i < 6; ++i) output.Indices.push_back(i6[i]);
			currentBatch.IndexCount += 6;
		};

		switch (el.Kind)
		{
		case DrawElementKind::NineSlice:
		{
			float x0 = el.Position.x, y0 = el.Position.y;
			float w = el.Size.x, h = el.Size.y;
			float u0 = el.ImageBrush.UVMin.x, v0 = el.ImageBrush.UVMin.y;
			float u1 = el.ImageBrush.UVMax.x, v1 = el.ImageBrush.UVMax.y;
			float du = u1 - u0, dv = v1 - v0;
			float ml = el.NineSliceMargin.Left  * du;
			float mt = el.NineSliceMargin.Top   * dv;
			float mr = el.NineSliceMargin.Right * du;
			float mb = el.NineSliceMargin.Bottom * dv;
			float sl = el.NineSliceMargin.Left  * w;
			float st = el.NineSliceMargin.Top   * h;
			float sr = el.NineSliceMargin.Right * w;
			float sb = el.NineSliceMargin.Bottom * h;

			uint8_t R = (uint8_t)(el.Tint.r * 255.0f);
			uint8_t G = (uint8_t)(el.Tint.g * 255.0f);
			uint8_t B = (uint8_t)(el.Tint.b * 255.0f);
			uint8_t A = (uint8_t)(el.Tint.a * 255.0f);

			// 9 regions: [row][col] — each region gets tint multiplied with brush tint
			float xs[4] = { x0, x0 + sl, x0 + w - sr, x0 + w };
			float ys[4] = { y0, y0 + st, y0 + h - sb, y0 + h };
			float us[4] = { u0, u0 + ml, u1 - mr, u1 };
			float vs[4] = { v0, v0 + mt, v1 - mb, v1 };

			for (int row = 0; row < 3; row++)
			for (int col = 0; col < 3; col++)
			{
				float rx0 = xs[col], ry0 = ys[row];
				float rx1 = xs[col+1], ry1 = ys[row+1];
				float ru0 = us[col], rv0 = vs[row];
				float ru1 = us[col+1], rv1 = vs[row+1];
				if (rx1 <= rx0 || ry1 <= ry0) continue;
				emitQuad(rx0, ry0, rx1, ry1, ru0, rv0, ru1, rv1, R,G,B,A, R,G,B,A, R,G,B,A, R,G,B,A);
			}
			break;
		}
		case DrawElementKind::Line:
		{
			uint8_t R = (uint8_t)(el.Tint.r * 255.0f);
			uint8_t G = (uint8_t)(el.Tint.g * 255.0f);
			uint8_t B = (uint8_t)(el.Tint.b * 255.0f);
			uint8_t A = (uint8_t)(el.Tint.a * 255.0f);

			glm::vec2 d = el.LineP1 - el.LineP0;
			float len = glm::length(d);
			if (len < 0.001f) break;
			d /= len;
			float halfT = el.LineThickness * 0.5f;
			glm::vec2 n = { -d.y, d.x };

			uint32_t base = (uint32_t)output.Vertices.size();
			BatchedVertex vtx[4];
			MakeVertex(vtx[0], el.LineP0.x + n.x*halfT, el.LineP0.y + n.y*halfT, 0,0, R,G,B,A);
			MakeVertex(vtx[1], el.LineP1.x + n.x*halfT, el.LineP1.y + n.y*halfT, 1,0, R,G,B,A);
			MakeVertex(vtx[2], el.LineP1.x - n.x*halfT, el.LineP1.y - n.y*halfT, 1,1, R,G,B,A);
			MakeVertex(vtx[3], el.LineP0.x - n.x*halfT, el.LineP0.y - n.y*halfT, 0,1, R,G,B,A);
			for (int i = 0; i < 4; i++) output.Vertices.push_back(vtx[i]);
			uint32_t i6[6] = { base, base+1, base+2, base, base+2, base+3 };
			for (int i = 0; i < 6; i++) output.Indices.push_back(i6[i]);
			currentBatch.IndexCount += 6;
			break;
		}
		case DrawElementKind::Polyline:
		{
			uint8_t R = (uint8_t)(el.Tint.r * 255.0f);
			uint8_t G = (uint8_t)(el.Tint.g * 255.0f);
			uint8_t B = (uint8_t)(el.Tint.b * 255.0f);
			uint8_t A = (uint8_t)(el.Tint.a * 255.0f);

			const auto& pts = el.PolyPoints;
			if (pts.size() < 2) break;
			float halfT = el.PolyThickness * 0.5f;

			// Emit a triangle (3 vertices, clockwise)
			auto emitTri = [&](float x0,float y0, float x1,float y1, float x2,float y2)
			{
				uint32_t base = (uint32_t)output.Vertices.size();
				BatchedVertex vt[3];
				MakeVertex(vt[0], x0,y0, 0,0, R,G,B,A);
				MakeVertex(vt[1], x1,y1, 0,0, R,G,B,A);
				MakeVertex(vt[2], x2,y2, 0,0, R,G,B,A);
				for (int i=0;i<3;i++) output.Vertices.push_back(vt[i]);
				uint32_t ti[3] = { base, base+1, base+2 };
				for (int i=0;i<3;i++) output.Indices.push_back(ti[i]);
				currentBatch.IndexCount += 3;
			};

			for (size_t i = 0; i + 1 < pts.size(); i++)
			{
				glm::vec2 d = pts[i+1] - pts[i];
				float len = glm::length(d);
				if (len < 0.001f) continue;
				d /= len;
				glm::vec2 n = { -d.y, d.x };

				uint32_t base = (uint32_t)output.Vertices.size();
				BatchedVertex vtx[4];
				MakeVertex(vtx[0], pts[i].x + n.x*halfT,   pts[i].y + n.y*halfT,   0,0, R,G,B,A);
				MakeVertex(vtx[1], pts[i+1].x + n.x*halfT, pts[i+1].y + n.y*halfT, 1,0, R,G,B,A);
				MakeVertex(vtx[2], pts[i+1].x - n.x*halfT, pts[i+1].y - n.y*halfT, 1,1, R,G,B,A);
				MakeVertex(vtx[3], pts[i].x - n.x*halfT,   pts[i].y - n.y*halfT,   0,1, R,G,B,A);
				for (int j=0;j<4;j++) output.Vertices.push_back(vtx[j]);
				uint32_t i6[6] = { base, base+1, base+2, base, base+2, base+3 };
				for (int j=0;j<6;j++) output.Indices.push_back(i6[j]);
				currentBatch.IndexCount += 6;
			}

			// Bevel joins
			for (size_t i = 1; i + 1 < pts.size(); i++)
			{
				glm::vec2 prev = pts[i] - pts[i-1];
				glm::vec2 next = pts[i+1] - pts[i];
				float plen = glm::length(prev);
				float nlen = glm::length(next);
				if (plen < 0.001f || nlen < 0.001f) continue;
				prev /= plen;
				next /= nlen;
				glm::vec2 pn = { -prev.y, prev.x };
				glm::vec2 nn = { -next.y, next.x };
				float cx = pts[i].x, cy = pts[i].y;

				float cross = prev.x * next.y - prev.y * next.x;
				if (cross > 0.0f)
				{
					emitTri(cx, cy,
						cx + pn.x*halfT, cy + pn.y*halfT,
						cx + nn.x*halfT, cy + nn.y*halfT);
				}
				else
				{
					emitTri(cx, cy,
						cx - nn.x*halfT, cy - nn.y*halfT,
						cx - pn.x*halfT, cy - pn.y*halfT);
				}
			}
			break;
		}
		case DrawElementKind::Gradient:
		{
			float x0 = el.Position.x, y0 = el.Position.y;
			float x1 = x0 + el.Size.x, y1 = y0 + el.Size.y;
			emitQuad(x0, y0, x1, y1, 0.0f, 0.0f, 1.0f, 1.0f,
				(uint8_t)(el.GradientColorTL.r*255), (uint8_t)(el.GradientColorTL.g*255), (uint8_t)(el.GradientColorTL.b*255), (uint8_t)(el.GradientColorTL.a*255),
				(uint8_t)(el.GradientColorTR.r*255), (uint8_t)(el.GradientColorTR.g*255), (uint8_t)(el.GradientColorTR.b*255), (uint8_t)(el.GradientColorTR.a*255),
				(uint8_t)(el.GradientColorBR.r*255), (uint8_t)(el.GradientColorBR.g*255), (uint8_t)(el.GradientColorBR.b*255), (uint8_t)(el.GradientColorBR.a*255),
				(uint8_t)(el.GradientColorBL.r*255), (uint8_t)(el.GradientColorBL.g*255), (uint8_t)(el.GradientColorBL.b*255), (uint8_t)(el.GradientColorBL.a*255));
			break;
		}
		case DrawElementKind::Triangle:
		{
			// Emitted as a quad whose fourth vertex repeats the third
			// (degenerate second triangle) - the surviving triangle
			// interpolates the three vertex colours linearly, which is
			// exactly the classic hue/white/black picker triangle.
			uint32_t baseVertex = (uint32_t)output.Vertices.size();
			BatchedVertex v[4];
			auto c = [](const glm::vec4& col, int ch) -> uint8_t { return (uint8_t)(col[ch] * 255.0f); };
			MakeVertex(v[0], el.TriP[0].x, el.TriP[0].y, 0.0f, 0.0f, c(el.TriC[0],0), c(el.TriC[0],1), c(el.TriC[0],2), c(el.TriC[0],3));
			MakeVertex(v[1], el.TriP[1].x, el.TriP[1].y, 1.0f, 0.0f, c(el.TriC[1],0), c(el.TriC[1],1), c(el.TriC[1],2), c(el.TriC[1],3));
			MakeVertex(v[2], el.TriP[2].x, el.TriP[2].y, 1.0f, 1.0f, c(el.TriC[2],0), c(el.TriC[2],1), c(el.TriC[2],2), c(el.TriC[2],3));
			MakeVertex(v[3], el.TriP[2].x, el.TriP[2].y, 1.0f, 1.0f, c(el.TriC[2],0), c(el.TriC[2],1), c(el.TriC[2],2), c(el.TriC[2],3));
			output.Vertices.push_back(v[0]);
			output.Vertices.push_back(v[1]);
			output.Vertices.push_back(v[2]);
			output.Vertices.push_back(v[3]);
			uint32_t i6[6] = { baseVertex, baseVertex+1, baseVertex+2, baseVertex, baseVertex+2, baseVertex+3 };
			for (int i = 0; i < 6; ++i) output.Indices.push_back(i6[i]);
			currentBatch.IndexCount += 6;
			break;
		}
		case DrawElementKind::Box:
		case DrawElementKind::Image:
		case DrawElementKind::Text:
		case DrawElementKind::RoundedBox:
		default:
		{
			uint32_t baseVertex = (uint32_t)output.Vertices.size();

			uint8_t r = (uint8_t)(el.Tint.r * 255.0f);
			uint8_t g = (uint8_t)(el.Tint.g * 255.0f);
			uint8_t b = (uint8_t)(el.Tint.b * 255.0f);
			uint8_t a = (uint8_t)(el.Tint.a * 255.0f);

			float x0 = el.Position.x, y0 = el.Position.y;
			float x1, y1;

			if (el.Kind == DrawElementKind::Text)
			{
				x1 = x0 + el.TextAdvance;
				y1 = y0 + el.TextHeight;
			}
			else
			{
				x1 = x0 + el.Size.x;
				y1 = y0 + el.Size.y;
			}

			float u0 = el.Kind == DrawElementKind::Text ? el.TextAtlasUV[0].x : (el.Kind == DrawElementKind::RoundedBox ? 0.0f : el.ImageBrush.UVMin.x);
			float v0 = el.Kind == DrawElementKind::Text ? el.TextAtlasUV[0].y : (el.Kind == DrawElementKind::RoundedBox ? 0.0f : el.ImageBrush.UVMin.y);
			float u1 = el.Kind == DrawElementKind::Text ? el.TextAtlasUV[1].x : (el.Kind == DrawElementKind::RoundedBox ? 1.0f : el.ImageBrush.UVMax.x);
			// Top-right's V is the SAME as top-left's (both top edge) -
			// using UVMax.y here collapsed the first triangle into a
			// diagonal smear across the whole image.
			float v1 = el.Kind == DrawElementKind::Text ? el.TextAtlasUV[1].y : (el.Kind == DrawElementKind::RoundedBox ? 0.0f : el.ImageBrush.UVMin.y);
			float u2 = el.Kind == DrawElementKind::Text ? el.TextAtlasUV[2].x : (el.Kind == DrawElementKind::RoundedBox ? 1.0f : el.ImageBrush.UVMax.x);
			float v2 = el.Kind == DrawElementKind::Text ? el.TextAtlasUV[2].y : (el.Kind == DrawElementKind::RoundedBox ? 1.0f : el.ImageBrush.UVMax.y);
			float u3 = el.Kind == DrawElementKind::Text ? el.TextAtlasUV[3].x : (el.Kind == DrawElementKind::RoundedBox ? 0.0f : el.ImageBrush.UVMin.x);
			float v3 = el.Kind == DrawElementKind::Text ? el.TextAtlasUV[3].y : (el.Kind == DrawElementKind::RoundedBox ? 1.0f : el.ImageBrush.UVMax.y);

			BatchedVertex v[4];
			MakeVertex(v[0], x0, y0, u0, v0, r, g, b, a);
			MakeVertex(v[1], x1, y0, u1, v1, r, g, b, a);
			MakeVertex(v[2], x1, y1, u2, v2, r, g, b, a);
			MakeVertex(v[3], x0, y1, u3, v3, r, g, b, a);

			output.Vertices.push_back(v[0]);
			output.Vertices.push_back(v[1]);
			output.Vertices.push_back(v[2]);
			output.Vertices.push_back(v[3]);

			uint32_t indices6[6] = { baseVertex, baseVertex+1, baseVertex+2, baseVertex, baseVertex+2, baseVertex+3 };
			for (int i = 0; i < 6; ++i) output.Indices.push_back(indices6[i]);
			currentBatch.IndexCount += 6;
			break;
		}
		}
	}

	if (currentBatch.IndexCount > 0)
		output.Batches.push_back(currentBatch);

	return output;
}

} // namespace wisp
