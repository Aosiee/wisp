#pragma once

#include "Wisp/Core.h"
#include "Wisp/StyleTokens.h"
#include "Wisp/DrawList.h"
#include "Wisp/Docking/DockModel.h"
#include <functional>
#include <memory>
#include <vector>

namespace wisp {

enum class SplitterOrientation { Horizontal, Vertical };

class Splitter : public Widget
{
public:
	WISP_WIDGET_TYPE(Splitter)

	explicit Splitter(SplitterOrientation o = SplitterOrientation::Horizontal)
		: m_Orientation(o)
	{
		SetCursor(o == SplitterOrientation::Horizontal ? CursorType::ResizeEW : CursorType::ResizeNS);
	}

	struct SplitSlot
	{
		std::unique_ptr<Widget> Child;
		Slot SlotProps;
		float Fraction = 1.0f;
		float MainMinSize = 30.0f; // px along the split axis (clamped in arrange + drag)
	};

	SplitSlot& AddSlot(std::unique_ptr<Widget> w, float fraction = 1.0f)
	{
		m_Slots.push_back({std::move(w), Slot(), fraction});
		SetupParentChild(this, m_Slots.back().Child.get());
		// NOTE: no per-push normalization — fractions are meaningful per slot
		// (dock model ratios). SolveSizes/drag/SetFractions normalize at use.
		InvalidateLayout();
		return m_Slots.back();
	}

	SplitSlot& AddChild(std::unique_ptr<Widget> w, float fraction = 1.0f) { return AddSlot(std::move(w), fraction); }

	float GetSplitFraction(int idx) const { return (idx >= 0 && idx < (int)m_Slots.size()) ? m_Slots[idx].Fraction : 0.0f; }
	float GetSlotMinSize(int idx) const { return (idx >= 0 && idx < (int)m_Slots.size()) ? m_Slots[idx].MainMinSize : 0.0f; }
	void SetSlotMinSize(int idx, float px)
	{
		if (idx >= 0 && idx < (int)m_Slots.size())
		{
			m_Slots[idx].MainMinSize = std::max(0.0f, px);
			InvalidateLayout();
		}
	}

	// Set from the dock model (fractions renormalize; min sizes per slot).
	void SetFractions(const std::vector<float>& fractions)
	{
		for (size_t i = 0; i < m_Slots.size() && i < fractions.size(); i++)
			m_Slots[i].Fraction = fractions[i];
		NormalizeFractions();
		InvalidateLayout();
	}

	void SetSplitFraction(int idx, float f)
	{
		if (idx >= 0 && idx < (int)m_Slots.size())
		{
			m_Slots[idx].Fraction = f;
			NormalizeFractions();
			InvalidateLayout();
		}
	}

	// Optional callback: fired once on divider mouse-up with the committed
	// child index + ratio (dock integration commits via SetSplitRatio here).
	void SetOnDividerCommitted(std::function<void(int, float)> cb) { m_OnDividerCommitted = std::move(cb); }

	Splitter& SetDividerThickness(float t) { m_DividerThickness = t; InvalidateLayout(); return *this; }

	// Dock model node tagging.
	void SetNodeId(dock::NodeId id) { m_NodeId = id; }
	dock::NodeId GetNodeId() const { return m_NodeId; }

	glm::vec2 ComputeDesiredSize() const override
	{
		float main = 0, cross = 0;
		for (auto& s : m_Slots)
		{
			if (!s.Child || !IsVisibleForLayout(s.Child->GetVisibility())) continue;
			glm::vec2 d = s.Child->ComputeDesiredSize();
			s.Child->SetCachedDesiredSize(d);
			if (m_Orientation == SplitterOrientation::Horizontal)
			{
				main += d.x;
				cross = std::max(cross, d.y);
			}
			else
			{
				main += d.y;
				cross = std::max(cross, d.x);
			}
		}
		main += m_DividerThickness * (float)std::max(0, (int)m_Slots.size() - 1);
		return m_Orientation == SplitterOrientation::Horizontal
			? glm::vec2(main, cross) : glm::vec2(cross, main);
	}

	void ArrangeChildren(const Geometry& geom, ArrangedChildren& out) const override
	{
		int n = (int)m_Slots.size();
		if (n == 0) return;
		float totalMain = m_Orientation == SplitterOrientation::Horizontal ? geom.LocalSize.x : geom.LocalSize.y;
		float crossSize = m_Orientation == SplitterOrientation::Horizontal ? geom.LocalSize.y : geom.LocalSize.x;

		std::vector<float> ratios(n), mins(n);
		for (int i = 0; i < n; i++)
		{
			ratios[i] = m_Slots[i].Fraction;
			mins[i] = m_Slots[i].MainMinSize;
		}
		// Shared solver = single source of truth with the dock model.
		// Non-destructive: rendered sizes clamp, model ratios are untouched.
		std::vector<float> sizes = dock::SolveSizes(ratios, totalMain, mins, m_DividerThickness);

		float offset = 0;
		for (int i = 0; i < n; i++)
		{
			if (!m_Slots[i].Child || !IsVisibleForLayout(m_Slots[i].Child->GetVisibility()))
			{
				offset += sizes[i] + m_DividerThickness;
				continue;
			}
			glm::vec2 childPos, childSize;
			if (m_Orientation == SplitterOrientation::Horizontal)
			{
				childPos = { offset, 0 };
				childSize = { sizes[i], crossSize };
			}
			else
			{
				childPos = { 0, offset };
				childSize = { crossSize, sizes[i] };
			}
			out.Add(m_Slots[i].Child.get(), geom.MakeChild(childPos, childSize));
			offset += sizes[i] + m_DividerThickness;
		}

		// Cache divider rects for hit-testing
		m_DividerRects.clear();
		offset = 0;
		for (int i = 0; i < n - 1; i++)
		{
			offset += sizes[i];
			glm::vec2 absMin, absMax;
			if (m_Orientation == SplitterOrientation::Horizontal)
			{
				absMin = geom.LocalToAbsolute({offset, 0});
				absMax = geom.LocalToAbsolute({offset + m_DividerThickness, crossSize});
			}
			else
			{
				absMin = geom.LocalToAbsolute({0, offset});
				absMax = geom.LocalToAbsolute({crossSize, offset + m_DividerThickness});
			}
			m_DividerRects.push_back({absMin, absMax});
			offset += m_DividerThickness;
		}
	}

	int OnPaint(const PaintArgs& args, const Geometry& geom, DrawList& drawList, int layerId, const WidgetStyleContext& style) const override
	{
		m_CachedSize = geom.LocalSize;

		// Paint dividers
		for (auto& dr : m_DividerRects)
		{
			glm::vec4 col = tokens::Backdrop;
			drawList.AddBox(layerId, dr.Min, {dr.Max.x - dr.Min.x, dr.Max.y - dr.Min.y}, col, args.InheritedClipIndex);
		}

		// Paint children
		ArrangedChildren arranged;
		ArrangeChildren(geom, arranged);
		int nextLayer = layerId + 1;
		WidgetStyleContext cs = style;
		for (auto& child : arranged)
		{
			cs.ColorAndOpacity = style.ColorAndOpacity * child.WidgetPtr->GetRenderTint();
			cs.ColorAndOpacity.a *= child.WidgetPtr->GetRenderOpacity();
			nextLayer = child.WidgetPtr->OnPaint(args, child.Geom, drawList, nextLayer, cs);
		}
		return nextLayer;
	}

	Reply OnMouseButtonDown(const glm::vec2& absPos, int button) override
	{
		if (button != 0) return Reply::Unhandled();
		for (int i = 0; i < (int)m_DividerRects.size(); i++)
		{
			auto& dr = m_DividerRects[i];
			float hitPad = 3.0f;
			if (absPos.x >= dr.Min.x - hitPad && absPos.x <= dr.Max.x + hitPad &&
				absPos.y >= dr.Min.y - hitPad && absPos.y <= dr.Max.y + hitPad)
			{
				m_DragIndex = i;
				m_DragAbsStart = absPos;
				m_DragFracStart[0] = m_Slots[i].Fraction;
				m_DragFracStart[1] = m_Slots[i + 1].Fraction;
				return Reply::Handled_().CaptureMouse();
			}
		}
		return Reply::Unhandled();
	}

	Reply OnMouseMove(const glm::vec2& absPos) override
	{
		if (m_DragIndex < 0) return Reply::Unhandled();
		float delta = m_Orientation == SplitterOrientation::Horizontal
			? absPos.x - m_DragAbsStart.x : absPos.y - m_DragAbsStart.y;
		float totalMain = m_Orientation == SplitterOrientation::Horizontal
			? m_CachedSize.x : m_CachedSize.y;
		float dividerTotal = m_DividerThickness * (float)((int)m_Slots.size() - 1);
		float avail = std::max(0.1f, totalMain - dividerTotal);
		if (avail <= 0.1f) return Reply::Handled_();

		// The two adjacent panes share a FIXED combined fraction — only the
		// split between them changes, so all other panes stay put (this is
		// what keeps a 3-pane divider drag from shifting the third pane).
		float combined = m_DragFracStart[0] + m_DragFracStart[1];
		if (combined <= 1e-5f) return Reply::Handled_();
		float minFrac0 = std::max(0.0f, m_Slots[m_DragIndex].MainMinSize) / avail;
		float minFrac1 = std::max(0.0f, m_Slots[m_DragIndex + 1].MainMinSize) / avail;
		float f0 = m_DragFracStart[0] + delta / avail;
		f0 = std::clamp(f0, minFrac0, std::max(minFrac0, combined - minFrac1));
		m_Slots[m_DragIndex].Fraction = f0;
		m_Slots[m_DragIndex + 1].Fraction = combined - f0;
		InvalidateLayout();
		return Reply::Handled_();
	}

	Reply OnMouseButtonUp(const glm::vec2&, int button) override
	{
		if (button != 0) return Reply::Unhandled();
		if (m_DragIndex < 0) return Reply::Unhandled();
		if (m_OnDividerCommitted)
		{
			float a = m_Slots[m_DragIndex].Fraction;
			float b = m_Slots[m_DragIndex + 1].Fraction;
			float pairRatio = (a + b) > 1e-6f ? a / (a + b) : 0.5f;
			m_OnDividerCommitted(m_DragIndex, pairRatio);
		}
		m_DragIndex = -1;
		return Reply::Handled_().ReleaseCapture();
	}

	void ForEachChild(std::function<void(Widget*)> fn) override
	{
		for (auto& s : m_Slots)
			if (s.Child && IsVisibleForLayout(s.Child->GetVisibility())) fn(s.Child.get());
	}

private:
	void NormalizeFractions()
	{
		float sum = 0;
		for (auto& s : m_Slots) sum += s.Fraction;
		if (sum <= 0) sum = 1;
		for (auto& s : m_Slots) s.Fraction /= sum;
	}

	struct DividerRect { glm::vec2 Min, Max; };

	SplitterOrientation m_Orientation;
	std::vector<SplitSlot> m_Slots;
	float m_DividerThickness = 5.0f;
	mutable std::vector<DividerRect> m_DividerRects;
	mutable int m_DragIndex = -1;
	mutable glm::vec2 m_DragAbsStart{0,0};
	mutable float m_DragFracStart[2] = {0,0};
	mutable glm::vec2 m_CachedSize{0,0};
	std::function<void(int, float)> m_OnDividerCommitted;
	dock::NodeId m_NodeId = 0;
};

using SSplitter = Splitter;

} // namespace wisp
