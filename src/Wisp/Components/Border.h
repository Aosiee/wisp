#pragma once
#include "Wisp/Core.h"
#include "Wisp/DrawList.h"
#include "Wisp/Theme.h"
#include "Wisp/Style.h"
#include "Wisp/StyleTokens.h"

namespace wisp {
	template <typename T> class WidgetRef; // the SetChild(WidgetRef&) overload
}

namespace wisp {

// ---- Border -----------------------------------------------------------------

class Border : public Widget
{
public:
	WISP_WIDGET_TYPE(Border)

	Border() { m_Style = GetDefaultStyle(); }

	static PanelStyle GetDefaultStyle()
	{
		PanelStyle s;
		s.Background.Tint = tokens::Panel;
		return s;
	}

	Border& SetBrush(const Brush& b) { m_Style.Background = b; m_Background = b; m_HasUserStyle = true; return *this; }
	Border& SetPadding(float uniform) { m_Padding.SetPadding(uniform); return *this; }
	Border& SetPadding(float h, float v) { m_Padding.SetPadding(h, v); return *this; }
	Border& SetCornerRadii(float r) { for (int i = 0; i < 4; i++) m_CornerRadii[i] = r; return *this; }
	Border& SetCornerRadii(float tl, float tr, float br, float bl) { m_CornerRadii[0]=tl; m_CornerRadii[1]=tr; m_CornerRadii[2]=br; m_CornerRadii[3]=bl; return *this; }
	// A 1 px-class outline (cards, popups, fields); drawn through the
	// SDF rounded box, radius 0 included.
	Border& SetOutline(float width, const glm::vec4& colour) { m_OutlineWidth = width; m_OutlineColour = colour; return *this; }
	Border& SetChild(std::unique_ptr<Widget> child)
	{
		m_Child = std::move(child);
		if (m_Child) SetupParentChild(this, m_Child.get());
		return *this;
	}
	template<typename T> Border& SetChild(WidgetRef<T>& ref) { return SetChild(std::unique_ptr<Widget>(std::move(ref))); }
	Widget* GetChild() const { return m_Child.get(); }

	glm::vec2 ComputeDesiredSize() const override
	{
		if (!m_Child || !IsVisibleForLayout(m_Child->GetVisibility())) return m_Padding.PaddingSize();
		glm::vec2 childDesired = m_Child->ComputeDesiredSize();
		m_Child->SetCachedDesiredSize(childDesired);
		return childDesired + m_Padding.PaddingSize();
	}

	void ArrangeChildren(const Geometry& geom, ArrangedChildren& out) const override
	{
		if (!m_Child || !IsVisibleForLayout(m_Child->GetVisibility())) return;
		glm::vec2 pad = m_Padding.PaddingSize();
		glm::vec2 childPos = { m_Padding.PaddingLeft, m_Padding.PaddingTop };
		glm::vec2 childSize = geom.LocalSize - pad;
		out.Add(m_Child.get(), geom.MakeChild(childPos, childSize));
	}

	int OnPaint(const PaintArgs& args, const Geometry& geom, DrawList& drawList, int layerId, const WidgetStyleContext& style) const override
	{
		if (m_StyleDirty && !m_HasUserStyle)
		{
			m_Style = ResolveStyle(this, GetDefaultStyle());
			m_StyleDirty = false;
			m_Background = m_Style.Background; // sync legacy field
		}
		glm::vec2 absPos = geom.LocalToAbsolute({ 0, 0 });
		bool hasRadius = (m_CornerRadii[0] > 0 || m_CornerRadii[1] > 0 || m_CornerRadii[2] > 0 || m_CornerRadii[3] > 0);
		// An explicit SetBrush wins; only a default-styled Border takes the
		// theme's Panel.Background (it used to override every brush).
		glm::vec4 tint = (m_Background.HasTexture || m_HasUserStyle) ? m_Background.Tint
			: ResolveThemeColour(this, "Panel.Background", m_Background.Tint);
		if (hasRadius || m_OutlineWidth > 0.0f)
			drawList.AddRoundedBox(layerId, absPos, geom.LocalSize, tint, m_CornerRadii, m_OutlineWidth, m_OutlineColour, args.InheritedClipIndex);
		else if (m_Background.HasTexture)
			drawList.AddImage(layerId, absPos, geom.LocalSize, m_Background, args.InheritedClipIndex);
		else
			drawList.AddBox(layerId, absPos, geom.LocalSize, tint, args.InheritedClipIndex);

		// Paint children with inherited clip and scale
		ArrangedChildren arranged;
		ArrangeChildren(geom, arranged);
		int nextLayer = layerId + 1;
		WidgetStyleContext childStyle = style;
		PaintArgs childArgs = args;
		childArgs.InheritedClipIndex = ApplyWidgetClipping(this, drawList, args.InheritedClipIndex, absPos, geom.LocalSize);
		for (auto& child : arranged)
		{
			childStyle.ColorAndOpacity = style.ColorAndOpacity * child.WidgetPtr->GetRenderTint();
			childStyle.ColorAndOpacity.a *= child.WidgetPtr->GetRenderOpacity();
			nextLayer = child.WidgetPtr->OnPaint(childArgs, child.Geom, drawList, nextLayer, childStyle);
		}
		return nextLayer;
	}

	void ForEachChild(std::function<void(Widget*)> fn) override { if (m_Child) fn(m_Child.get()); }

private:
	mutable PanelStyle m_Style;
	bool m_HasUserStyle = false;
	mutable Brush m_Background;
	Slot  m_Padding;
	float m_CornerRadii[4] = {};
	float m_OutlineWidth = 0.0f;
	glm::vec4 m_OutlineColour{ 0.0f };
	std::unique_ptr<Widget> m_Child;
};


} // namespace wisp
