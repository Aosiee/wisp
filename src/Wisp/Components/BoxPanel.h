#pragma once
#include "Wisp/Core.h"
#include "Wisp/DrawList.h"

namespace wisp {
	template <typename T> class WidgetRef; // the SlotBuilder operator[] overloads
}

namespace wisp {

// ---- BoxPanel (VerticalBox / HorizontalBox) ---------------------------------

enum class BoxOrientation { Vertical, Horizontal };

class BoxPanel : public Widget
{
public:
	WISP_WIDGET_TYPE(BoxPanel)

	BoxPanel(BoxOrientation o) : m_Orientation(o) {}

	// Slots for children — simple add with padding + alignment
	template<typename T, typename... Args>
	T* AddChild(Args&&... args)
	{
		auto child = std::make_unique<T>(std::forward<Args>(args)...);
		T* ptr = child.get();
		SetupParentChild(this, child.get());
		m_Children.push_back({ std::move(child), {} });
		InvalidateLayout();
		return ptr;
	}

	BoxPanel& PushChild(std::unique_ptr<Widget> child)
	{
		SetupParentChild(this, child.get());
		m_Children.push_back({ std::move(child), {} });
		InvalidateLayout();
		return *this;
	}

	template<typename T>
	BoxPanel& PushChild(WidgetRef<T>&& ref)
	{
		return PushChild(std::unique_ptr<Widget>(std::move(ref)));
	}

	void AddChild(std::unique_ptr<Widget> child)
	{
		SetupParentChild(this, child.get());
		m_Children.push_back({ std::move(child), {} });
		InvalidateLayout();
	}


	Slot& GetSlot(size_t index)
	{
		return m_Children[index].SlotData;
	}

	size_t GetChildCount() const { return m_Children.size(); }

	// Slot builder — child is stored, added by operator+ on the container
	struct SlotBuilder
	{
		BoxPanel* Owner;
		Slot      Data;
		std::unique_ptr<Widget> PendingChild;

		SlotBuilder& Padding(float u)          { Data.SetPadding(u); return *this; }
		SlotBuilder& Padding(float h, float v) { Data.SetPadding(h, v); return *this; }
		SlotBuilder& HAlign(HAlign h)          { Data.Align.H = h; return *this; }
		SlotBuilder& VAlign(VAlign v)          { Data.Align.V = v; return *this; }
		SlotBuilder& Fill(float w)             { Data.FillWeight = w; return *this; }
		SlotBuilder& MinWidth(float w)         { Data.MinWidth = w; return *this; }
		SlotBuilder& MinHeight(float h)        { Data.MinHeight = h; return *this; }
		SlotBuilder& MaxWidth(float w)         { Data.MaxWidth = w; return *this; }
		SlotBuilder& MaxHeight(float h)        { Data.MaxHeight = h; return *this; }

		SlotBuilder operator[](std::unique_ptr<Widget> child) &&
		{
			PendingChild = std::move(child);
			return std::move(*this);
		}
		SlotBuilder operator[](std::unique_ptr<Widget> child) &
		{
			PendingChild = std::move(child);
			return std::move(*this);
		}

		template<typename T>
		SlotBuilder operator[](WidgetRef<T>&& ref) &&
		{
			return std::move(*this)[std::unique_ptr<Widget>(std::move(ref))];
		}

		// Accept WidgetRef lvalue — no std::move needed at call site
		template<typename T>
		SlotBuilder operator[](WidgetRef<T>& ref) &&
		{
			return std::move(*this)[std::unique_ptr<Widget>(std::move(ref))];
		}

		template<typename T>
		SlotBuilder operator[](WidgetRef<T>& ref) &
		{
			PendingChild = std::unique_ptr<Widget>(std::move(ref));
			return std::move(*this);
		}
	};

	SlotBuilder AddSlot() { return { this, {}, nullptr }; }

	// operator+ takes a SlotBuilder, adds the stored child to this container
	BoxPanel& operator+(SlotBuilder&& sb)
	{
		if (sb.PendingChild)
		{
			SetupParentChild(this, sb.PendingChild.get());
			size_t idx = m_Children.size();
			m_Children.push_back({ std::move(sb.PendingChild), {} });
			InvalidateLayout();
			if (idx < m_Children.size())
				m_Children[idx].SlotData = sb.Data;
		}
		return *this;
	}
	BoxPanel& operator+(const SlotBuilder&) { return *this; }


	glm::vec2 ComputeDesiredSize() const override
	{
		glm::vec2 desired = { 0, 0 };
		for (const auto& entry : m_Children)
		{
			if (!IsVisibleForLayout(entry.WidgetPtr->GetVisibility())) continue;
			glm::vec2 childDesired = entry.WidgetPtr->ComputeDesiredSize();
			entry.WidgetPtr->SetCachedDesiredSize(childDesired);
			glm::vec2 pad = entry.SlotData.PaddingSize();
			if (m_Orientation == BoxOrientation::Vertical)
			{
				desired.x = std::max(desired.x, childDesired.x + pad.x);
				desired.y += childDesired.y + pad.y;
			}
			else
			{
				desired.x += childDesired.x + pad.x;
				desired.y = std::max(desired.y, childDesired.y + pad.y);
			}
		}
		return desired;
	}

	void ArrangeChildren(const Geometry& geom, ArrangedChildren& out) const override
	{
		bool vertical = (m_Orientation == BoxOrientation::Vertical);
		float totalSpace = vertical ? geom.LocalSize.y : geom.LocalSize.x;
		float totalFillWeight = 0.0f;
		float totalAutoSize  = 0.0f;

		// First pass: measure auto-sized children and sum fill weights
		for (const auto& entry : m_Children)
		{
			if (!IsVisibleForLayout(entry.WidgetPtr->GetVisibility())) continue;
			glm::vec2 childDesired = entry.WidgetPtr->GetCachedDesiredSize();
			const Slot& slot = entry.SlotData;
			glm::vec2 pad = slot.PaddingSize();

			if (slot.FillWeight > 0)
				totalFillWeight += slot.FillWeight;
			else
				totalAutoSize += vertical ? (childDesired.y + pad.y) : (childDesired.x + pad.x);
		}

		float remaining = std::max(0.0f, totalSpace - totalAutoSize);
		float offset = 0.0f;

		for (const auto& entry : m_Children)
		{
			if (!IsVisibleForLayout(entry.WidgetPtr->GetVisibility())) continue;
			glm::vec2 childDesired = entry.WidgetPtr->GetCachedDesiredSize();
			const Slot& slot = entry.SlotData;
			glm::vec2 pad = slot.PaddingSize();
			glm::vec2 slotSize, childPos, childSize;

			if (vertical)
			{
				if (slot.FillWeight > 0 && totalFillWeight > 0)
					slotSize.y = (remaining * slot.FillWeight / totalFillWeight);
				else
					slotSize.y = childDesired.y + pad.y;
				slotSize.x = geom.LocalSize.x;
				childPos = { 0, offset + slot.PaddingTop };
			}
			else
			{
				if (slot.FillWeight > 0 && totalFillWeight > 0)
					slotSize.x = (remaining * slot.FillWeight / totalFillWeight);
				else
					slotSize.x = childDesired.x + pad.x;
				slotSize.y = geom.LocalSize.y;
				childPos = { offset + slot.PaddingLeft, 0 };
			}

			// Apply slot min/max
			glm::vec2 contentSize = slotSize - pad;
			slot.ClampSize(contentSize);
			slotSize = contentSize + pad;

			ResolveAlignment(childPos, slotSize - pad, childDesired,
				slot.Align.H, slot.Align.V, childPos, childSize);

			out.Add(entry.WidgetPtr.get(), geom.MakeChild(childPos, childSize));
			if (vertical) offset += slotSize.y; else offset += slotSize.x;
		}
	}

	int OnPaint(const PaintArgs& args, const Geometry& geom, DrawList& drawList, int layerId, const WidgetStyleContext& style) const override
	{
		glm::vec2 absPos = geom.LocalToAbsolute({0,0});
		PaintArgs childArgs = args;
		childArgs.InheritedClipIndex = ApplyWidgetClipping(this, drawList, args.InheritedClipIndex, absPos, geom.LocalSize);
		ArrangedChildren arranged;
		ArrangeChildren(geom, arranged);
		int nextLayer = layerId;
		WidgetStyleContext childStyle = style;
		for (auto& child : arranged)
		{
			childStyle.ColorAndOpacity = style.ColorAndOpacity * child.WidgetPtr->GetRenderTint();
			childStyle.ColorAndOpacity.a *= child.WidgetPtr->GetRenderOpacity();
			nextLayer = child.WidgetPtr->OnPaint(childArgs, child.Geom, drawList, nextLayer, childStyle);
		}
		return nextLayer;
	}

	void ForEachChild(std::function<void(Widget*)> fn) override
	{
		for (auto& entry : m_Children)
			fn(entry.WidgetPtr.get());
	}

private:
	struct ChildEntry
	{
		std::unique_ptr<Widget> WidgetPtr;
		Slot SlotData;
	};

	BoxOrientation m_Orientation;
	std::vector<ChildEntry> m_Children;
};

using VerticalBox   = BoxPanel;
using HorizontalBox = BoxPanel;

} // namespace wisp
