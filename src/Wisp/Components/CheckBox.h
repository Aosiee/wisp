#pragma once
#include "Wisp/Core.h"
#include "Wisp/DrawList.h"
#include "Wisp/Style.h"
#include "Wisp/Theme.h"
#include <functional>

namespace wisp {

// ---- CheckBox --------------------------------------------------------------

class CheckBox : public Widget
{
public:
	WISP_WIDGET_TYPE(CheckBox)

	CheckBox() { m_Style = GetDefaultStyle(); SetSupportsFocus(true); }

	static CheckBoxStyle GetDefaultStyle()
	{
		CheckBoxStyle s;
		s.BoxUnchecked.Tint = { 0.18f, 0.18f, 0.22f, 1.0f };
		s.BoxChecked.Tint   = { 0.2f, 0.35f, 0.6f, 1.0f };
		s.BoxHovered.Tint   = { 0.3f, 0.3f, 0.35f, 1.0f };
		s.CheckMark.Tint    = { 0.4f, 0.8f, 0.4f, 1.0f };
		return s;
	}

	void SetLabel(const std::string& label) { m_Label = label; }
	bool IsChecked() const { return m_Checked; }
	void SetChecked(bool c) { m_Checked = c; }
	void SetOnStateChanged(std::function<void(bool)> cb) { m_OnStateChanged = std::move(cb); }

	glm::vec2 ComputeDesiredSize() const override
	{
		return { m_Style.BoxSize + m_LabelWidth + m_Style.LabelPadding.Left, m_Style.BoxSize + 4.0f };
	}

	void ArrangeChildren(const Geometry&, ArrangedChildren&) const override {}

	int OnPaint(const PaintArgs& args, const Geometry& geom, DrawList& drawList, int layerId, const WidgetStyleContext& style) const override
	{
		if (m_StyleDirty)
		{
			m_Style = ResolveStyle(this, GetDefaultStyle());
			m_StyleDirty = false;
		}
		glm::vec2 absPos = geom.LocalToAbsolute({ 0, 0 });
		float boxY = absPos.y + 2.0f;
		float bs = m_Style.BoxSize;

		glm::vec4 bgCol = m_IsHovered ? m_Style.BoxHovered.Tint : m_Style.BoxUnchecked.Tint;
		drawList.AddBox(layerId, { absPos.x, boxY }, { bs, bs }, bgCol, args.InheritedClipIndex);

		if (m_Checked)
			drawList.AddBox(layerId + 1, { absPos.x + 3.0f, boxY + 3.0f }, { bs - 6.0f, bs - 6.0f }, m_Style.CheckMark.Tint, args.InheritedClipIndex);

		return layerId + 2;
	}

	Reply OnMouseEnter() override { m_IsHovered = true; return Reply::Unhandled(); }
	Reply OnMouseLeave() override { m_IsHovered = false; return Reply::Unhandled(); }

	Reply OnMouseButtonDown(const glm::vec2&, int button) override
	{
		if (button != 0) return Reply::Unhandled();
		return Reply::Handled_().CaptureMouse();
	}

	Reply OnMouseButtonUp(const glm::vec2&, int button) override
	{
		if (button != 0) return Reply::Unhandled();
		if (m_IsHovered)
		{
			m_Checked = !m_Checked;
			if (m_OnStateChanged) m_OnStateChanged(m_Checked);
		}
		return Reply::Handled_().ReleaseCapture();
	}

	bool SupportsFocus() const override { return true; }

	Reply OnKeyDown(int key, int mods) override
	{
		if (key == 32) { m_Checked = !m_Checked; if (m_OnStateChanged) m_OnStateChanged(m_Checked); return Reply::Handled_(); }
		return Reply::Unhandled();
	}

private:
	mutable CheckBoxStyle m_Style;
	std::string m_Label;
	bool m_Checked = false;
	float m_LabelWidth = 0.0f;
	mutable bool m_IsHovered = false;
	std::function<void(bool)> m_OnStateChanged;
};


} // namespace wisp
