#pragma once
#include "Wisp/Core.h"
#include "Wisp/DrawList.h"

namespace wisp {

// ---- Overlay ---------------------------------------------------------------

class Overlay : public Widget
{
public:
	WISP_WIDGET_TYPE(Overlay)

	template<typename T, typename... Args> T* AddChild(Args&&... args)
	{
		auto child = std::make_unique<T>(std::forward<Args>(args)...);
		T* ptr = child.get();
		SetupParentChild(this, child.get());
		m_Children.push_back(std::move(child));
		InvalidateLayout();
		return ptr;
	}
	void AddChild(std::unique_ptr<Widget> child) { SetupParentChild(this, child.get()); m_Children.push_back(std::move(child)); InvalidateLayout(); }
	size_t GetChildCount() const { return m_Children.size(); }

	// Slot for + syntax
	struct OSlot
	{
		std::unique_ptr<Widget> Child;
		OSlot&& operator[](std::unique_ptr<Widget> c) && { Child = std::move(c); return std::move(*this); }
		template<typename T> OSlot&& operator[](WidgetRef<T>& r) && { return std::move(*this)[std::unique_ptr<Widget>(std::move(r))]; }
	};
	OSlot Slot() { return { nullptr }; }
	Overlay& operator+(OSlot&& s) { if (s.Child) AddChild(std::move(s.Child)); return *this; }

	glm::vec2 ComputeDesiredSize() const override
	{
		glm::vec2 max = { 0, 0 };
		for (auto& c : m_Children) { if (!IsVisibleForLayout(c->GetVisibility())) continue; glm::vec2 d = c->ComputeDesiredSize(); c->SetCachedDesiredSize(d); max = glm::max(max, d); }
		return max;
	}

	void ArrangeChildren(const Geometry& geom, ArrangedChildren& out) const override
	{
		for (auto& c : m_Children)
			if (IsVisibleForLayout(c->GetVisibility()))
				out.Add(c.get(), geom);
	}

	int OnPaint(const PaintArgs& args, const Geometry& geom, DrawList& drawList, int layerId, const WidgetStyleContext& style) const override
	{
		glm::vec2 absPos = geom.LocalToAbsolute({0,0});
		PaintArgs childArgs = args;
		childArgs.InheritedClipIndex = ApplyWidgetClipping(this, drawList, args.InheritedClipIndex, absPos, geom.LocalSize);
		int nextLayer = layerId;
		WidgetStyleContext cs = style;
		for (size_t i = 0; i < m_Children.size(); ++i)
		{
			auto& c = m_Children[i];
			// Collapsed children are arranged out of layout/hit-testing AND
			// out of paint - an invisible overlay slot (the collapsed
			// console) must not draw over the widgets beneath it.
			if (!IsVisibleForPaint(c->GetVisibility()))
				continue;
			cs.ColorAndOpacity = style.ColorAndOpacity * c->GetRenderTint();
			cs.ColorAndOpacity.a *= c->GetRenderOpacity();
			nextLayer = c->OnPaint(childArgs, geom, drawList, nextLayer, cs);
			nextLayer += 10;
		}
		return nextLayer;
	}

	void ForEachChild(std::function<void(Widget*)> fn) override { for (auto& c : m_Children) fn(c.get()); }

private:
	std::vector<std::unique_ptr<Widget>> m_Children;
};


} // namespace wisp
