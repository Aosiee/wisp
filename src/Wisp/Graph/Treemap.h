// The Treemap widget.
//
// The generic Wisp widget over a SizeQuery + the computed sizes: the
// squarified cells (the query's colours/labels), the depth nesting, the
// Exclusive/Inclusive sizing toggle, the hover + the tooltip, the click
// callback (the Size Map cell -> the Graph tab recenter).

#pragma once

#include "Wisp/Application.h"
#include "Wisp/Components/Border.h"
#include "Wisp/Components/SizeBox.h"
#include "Wisp/Components/TextBlock.h"
#include "Wisp/Core.h"
#include "Wisp/DrawList.h"
#include "Wisp/FontCache.h"
#include "Wisp/Graph/SizeMap.h"

#include <cmath>
#include <functional>
#include <string>
#include <unordered_map>
#include <vector>

namespace wisp {
namespace graph {

	class Treemap : public Widget
	{
	public:
		WISP_WIDGET_TYPE(Treemap)

		Treemap(FontCache* fontCache, FontHandle font, const SizeQuery* query)
			: m_FontCache(fontCache), m_Font(font), m_Query(query)
		{
			SetSupportsFocus(true);
			SetClipping(Clipping::ClipToBounds);
			SetToolTip([this]() -> std::unique_ptr<Widget> {
				if (!m_HasHover || !OnCellTooltip)
					return nullptr;
				const std::string text = OnCellTooltip(m_Hovered);
				if (text.empty())
					return nullptr;
				auto block = std::make_unique<wisp::TextBlock>(m_FontCache, m_Font, 13.0f,
					text, glm::vec4(0.95f, 0.95f, 1.0f, 1.0f));
				block->SetWrapMode(wisp::WrapMode::Word);
				block->SetAutoWrap(true);
				auto border = std::make_unique<wisp::Border>();
				border->SetBrush({ glm::vec4(0.1f, 0.1f, 0.16f, 0.97f) });
				border->SetPadding(8.0f, 6.0f);
				border->SetChild(std::move(block));
				auto sized = std::make_unique<wisp::SizeBox>();
				sized->SetWidthOverride(420.0f);
				sized->SetChild(std::move(border));
				return sized;
			});
		}

		void SetRoot(NodeId root)
		{
			m_Root = root;
			// The cells change under the (possibly stationary) cursor -
			// the hover must re-resolve.
			m_LastPolledMouse = glm::vec2(-1e9f, -1e9f);
		}
		void SetSizes(const std::unordered_map<NodeId, SizeEntry>* sizes)
		{
			m_Sizes = sizes;
			m_LastPolledMouse = glm::vec2(-1e9f, -1e9f);
		}
		void SetUseInclusive(bool useInclusive) { m_UseInclusive = useInclusive; }
		void SetFooterNote(const std::string& note) { m_FooterNote = note; }

		// 0 = the aggregated "other" cell.
		std::function<void(NodeId)> OnCellClicked;
		std::function<std::string(NodeId)> OnCellTooltip;

		glm::vec2 ComputeDesiredSize() const override { return { 640.0f, 480.0f }; }
		void ArrangeChildren(const Geometry&, ArrangedChildren&) const override {}

		int OnPaint(const PaintArgs& args, const Geometry& geom, DrawList& dl,
			int layerId, const WidgetStyleContext&) const override
		{
			m_Origin = geom.LocalToAbsolute({ 0, 0 });
			m_Size = geom.LocalSize;

			dl.AddBox(layerId, m_Origin, m_Size, { 0.05f, 0.05f, 0.09f, 1.0f }, args.InheritedClipIndex);
			const int clip = dl.PushClip({ m_Origin.x, m_Origin.y, m_Size.x, m_Size.y }, args.InheritedClipIndex);

			m_Cells.clear();
			if (m_Query && m_Sizes)
				Squarify(*m_Query, *m_Sizes, m_Root, m_Size, m_UseInclusive, m_Cells);

			for (const TreemapCell& cell : m_Cells)
			{
				// The depth inset is the nesting separator (the background
				// shows through as the cell border).
				const float inset = 2.0f + (float)cell.Depth * 1.5f;
				const glm::vec2 pos = m_Origin + glm::vec2(cell.Rect.x, cell.Rect.y) + glm::vec2(inset);
				const glm::vec2 size = glm::vec2(cell.Rect.z, cell.Rect.w) - glm::vec2(inset) * 2.0f;
				if (size.x <= 1.0f || size.y <= 1.0f)
					continue;

				glm::vec4 colour;
				if (cell.Aggregate)
					colour = { 0.28f, 0.28f, 0.34f, 1.0f };
				else
					colour = m_Query->Colour(cell.Id);
				// The softened tint (the saturated type colours blend
				// toward the canvas background).
				colour = colour * glm::vec4(0.82f, 0.82f, 0.82f, 1.0f)
					+ glm::vec4(0.05f, 0.05f, 0.09f, 0.0f) * 0.18f;
				if (m_HasHover && cell.Id == m_Hovered)
					colour = colour * glm::vec4(1.22f, 1.22f, 1.22f, 1.0f);
				dl.AddBox(layerId + 1, pos, size, colour, clip);

				if (size.x < 34.0f || size.y < 15.0f)
					continue;
				std::string label = cell.Aggregate
					? ("other (" + std::to_string(cell.AggregateCount) + ")")
					: m_Query->Label(cell.Id);
				const float maxW = size.x - 6.0f;
				if (m_FontCache->MeasureText(m_Font, label, 12.0f).x > maxW)
				{
					const float ellipsis = m_FontCache->MeasureText(m_Font, "...", 12.0f).x;
					while (!label.empty() && m_FontCache->MeasureText(m_Font, label, 12.0f).x + ellipsis > maxW)
						label.pop_back();
					label += "...";
				}
				PaintText(dl, layerId + 2, clip, { pos.x + 3.0f, pos.y + 3.0f }, label, { 1.0f, 1.0f, 1.0f, 0.95f });
			}

			// The footer note (the caller's source/config caveat).
			if (!m_FooterNote.empty())
			{
				const float noteH = 20.0f;
				dl.AddBox(layerId + 1, { m_Origin.x, m_Origin.y + m_Size.y - noteH },
					{ m_Size.x, noteH }, { 0.07f, 0.07f, 0.12f, 0.95f }, clip);
				PaintText(dl, layerId + 2, clip,
					{ m_Origin.x + 6.0f, m_Origin.y + m_Size.y - noteH + 4.0f },
					m_FooterNote, { 0.5f, 0.5f, 0.58f, 1.0f });
			}

			UpdateHover();
			return layerId + 3;
		}

		Reply OnMouseButtonDown(const glm::vec2& absPos, int button) override
		{
			if (button != 0)
				return Reply::Unhandled();
			for (size_t i = m_Cells.size(); i-- > 0;)
			{
				const TreemapCell& cell = m_Cells[i];
				const glm::vec2 pos = m_Origin + glm::vec2(cell.Rect.x, cell.Rect.y);
				if (absPos.x >= pos.x && absPos.x <= pos.x + cell.Rect.z
					&& absPos.y >= pos.y && absPos.y <= pos.y + cell.Rect.w)
				{
					if (OnCellClicked)
						OnCellClicked(cell.Id);
					return Reply::Handled_().SetFocus();
				}
			}
			return Reply::Unhandled();
		}

	private:
		void PaintText(DrawList& dl, int layer, int clip, const glm::vec2& at,
			const std::string& text, const glm::vec4& colour) const
		{
			const float size = 12.0f;
			const FontMetrics metrics = m_FontCache->GetMetrics(m_Font, size);
			const uint8_t* p = (const uint8_t*)text.data();
			const uint8_t* end = p + text.size();
			float penX = at.x;
			uint32_t prevCp = 0;
			while (p < end)
			{
				uint32_t cp;
				if ((*p & 0x80) == 0) { cp = *p; p += 1; }
				else if ((*p & 0xE0) == 0xC0 && p + 1 < end) { cp = ((*p & 0x1F) << 6) | (p[1] & 0x3F); p += 2; }
				else if ((*p & 0xF0) == 0xE0 && p + 2 < end) { cp = ((*p & 0x0F) << 12) | ((p[1] & 0x3F) << 6) | (p[2] & 0x3F); p += 3; }
				else { p++; continue; }

				const GlyphInfo* gi = m_FontCache->GetGlyph(m_Font, cp, size);
				if (!gi)
					continue;
				float advance = gi->AdvanceX;
				if (prevCp)
					advance += m_FontCache->GetKerning(m_Font, prevCp, cp, size);
				const glm::vec2 uvs[4] = {
					{ gi->AtlasU0, gi->AtlasV0 }, { gi->AtlasU1, gi->AtlasV0 },
					{ gi->AtlasU1, gi->AtlasV1 }, { gi->AtlasU0, gi->AtlasV1 } };
				dl.AddText(layer, { penX + gi->BearingX, at.y + metrics.Ascent + gi->BearingY },
					std::string(1, (char)cp), uvs, gi->AdvanceX, gi->Height, colour, clip);
				penX += std::round(advance);
				prevCp = cp;
			}
		}

		void UpdateHover() const
		{
			NodeId newHover = 0;
			bool hasHover = false;
			if (UIApplication::IsValid())
			{
				const glm::vec2 mouse = UIApplication::Get().GetMousePos();
				if (mouse == m_LastPolledMouse)
					return;
				m_LastPolledMouse = mouse;
				if (mouse.x >= m_Origin.x && mouse.x <= m_Origin.x + m_Size.x
					&& mouse.y >= m_Origin.y && mouse.y <= m_Origin.y + m_Size.y)
				{
					for (size_t i = m_Cells.size(); i-- > 0;)
					{
						const TreemapCell& cell = m_Cells[i];
						const glm::vec2 pos = m_Origin + glm::vec2(cell.Rect.x, cell.Rect.y);
						if (mouse.x >= pos.x && mouse.x <= pos.x + cell.Rect.z
							&& mouse.y >= pos.y && mouse.y <= pos.y + cell.Rect.w)
						{
							newHover = cell.Id;
							hasHover = true;
							break;
						}
					}
				}
			}
			if (newHover != m_Hovered)
			{
				m_Hovered = newHover;
				if (UIApplication::IsValid() && UIApplication::Get().HasOpenPopups())
					UIApplication::Get().RequestDismissPopups();
			}
			m_HasHover = hasHover;
		}

		FontCache* m_FontCache;
		FontHandle m_Font;
		const SizeQuery* m_Query = nullptr;
		const std::unordered_map<NodeId, SizeEntry>* m_Sizes = nullptr;
		NodeId m_Root = 0;
		bool m_UseInclusive = true;
		std::string m_FooterNote;

		mutable std::vector<TreemapCell> m_Cells;
		mutable glm::vec2 m_Origin{};
		mutable glm::vec2 m_Size{};
		mutable NodeId m_Hovered = 0;
		mutable bool m_HasHover = false;
		mutable glm::vec2 m_LastPolledMouse{ -1e9f, -1e9f };
	};

} // namespace graph
} // namespace wisp
