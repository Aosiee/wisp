#pragma once

// The toolbar vocabulary - icon buttons (with an optional label and
// dropdown chevron) in four variants, a group separator, and the shared
// tooltip look. Every editor toolbar (the main bar, the viewport overlay,
// panel headers) builds from these.

#include "Wisp/Application.h"
#include "Wisp/Composites/PopupMenu.h"
#include "Wisp/Components/BoxPanel.h"
#include "Wisp/Components/Button.h"
#include "Wisp/Components/Spacer.h"
#include "Wisp/Components/TextBlock.h"
#include "Wisp/Icons.h"
#include "Wisp/StyleTokens.h"

#include <functional>
#include <memory>
#include <string>

namespace wisp {

enum class ToolbarButtonVariant : uint8_t
{
	Ghost,   // transparent until hovered (the default toolbar look)
	Filled,  // a Control-coloured chip
	Accent,  // the primary action (accent fill, white content)
	Toggle,  // Ghost, but `Active` paints the accent-subtle chip + accent icon
};

struct ToolbarButtonDesc
{
	std::string Icon;
	std::string Label;
	std::string Tooltip;
	std::string Shortcut;            // shown in the tooltip
	glm::vec4 IconTint = tokens::TextSecondary;
	ToolbarButtonVariant Variant = ToolbarButtonVariant::Ghost;
	bool Active = false;             // Toggle: the on state
	bool Enabled = true;
	bool Chevron = false;            // a trailing dropdown chevron
	float IconSize = tokens::IconSize;
	float Height = 28.0f;
};

// The tooltip card: a small Popup-coloured frame, the text, and the
// shortcut in a muted run.
inline std::unique_ptr<Widget> MakeTooltip(FontCache* fc, FontHandle font, const std::string& text, const std::string& shortcut = {})
{
	auto row = std::make_unique<HorizontalBox>(BoxOrientation::Horizontal);
	*row + row->AddSlot().VAlign(VAlign::Center)[ std::make_unique<TextBlock>(fc, font, tokens::FontSmall, text, tokens::TextPrimary) ];
	if (!shortcut.empty())
	{
		*row
			+ row->AddSlot()[ std::make_unique<Spacer>(glm::vec2(10.0f, 1.0f)) ]
			+ row->AddSlot().VAlign(VAlign::Center)[ std::make_unique<TextBlock>(fc, font, tokens::FontSmall, shortcut, tokens::TextMuted) ];
	}
	return std::make_unique<PopupFrame>(std::move(row), 0.0f, 7.0f);
}

inline std::unique_ptr<Button> MakeToolbarButton(FontCache* fc, FontHandle font, const ToolbarButtonDesc& d,
	std::function<void()> onClick)
{
	auto btn = std::make_unique<Button>();
	glm::vec4 normal = tokens::WithAlpha(tokens::Control, 0.0f);
	glm::vec4 hovered = tokens::Hover;
	glm::vec4 pressed = tokens::Pressed;
	glm::vec4 content = tokens::TextPrimary;
	glm::vec4 iconTint = d.IconTint;
	switch (d.Variant)
	{
	case ToolbarButtonVariant::Filled:
		normal = tokens::Control;
		break;
	case ToolbarButtonVariant::Accent:
		normal = tokens::Accent;
		hovered = tokens::AccentHover;
		pressed = tokens::AccentPressed;
		content = tokens::TextOnAccent;
		iconTint = tokens::TextOnAccent;
		break;
	case ToolbarButtonVariant::Toggle:
		if (d.Active)
		{
			normal = tokens::WithAlpha(tokens::Accent, 0.22f);
			hovered = tokens::WithAlpha(tokens::Accent, 0.32f);
			iconTint = tokens::AccentHover;
			content = tokens::TextPrimary;
		}
		break;
	case ToolbarButtonVariant::Ghost:
		break;
	}
	if (!d.Enabled)
	{
		hovered = normal;
		pressed = normal;
		content = tokens::TextDisabled;
		iconTint = tokens::TextDisabled;
	}
	btn->SetNormalBrush({ normal });
	btn->SetHoveredBrush({ hovered });
	btn->SetPressedBrush({ pressed });
	btn->SetCornerRadius(tokens::RadiusSmall + 1.0f);

	const bool iconOnly = d.Label.empty() && !d.Chevron;
	const float vpad = std::max(2.0f, (d.Height - std::max(d.IconSize, tokens::FontBody)) * 0.5f);
	btn->SetContentPadding({ iconOnly ? vpad : 8.0f, vpad, iconOnly ? vpad : (d.Chevron ? 6.0f : 10.0f), vpad });

	const FontHandle iconFont = UIApplication::FontOr(FontRole::Icons, font);
	auto row = std::make_unique<HorizontalBox>(BoxOrientation::Horizontal);
	if (!d.Icon.empty())
		*row + row->AddSlot().VAlign(VAlign::Center)[ std::make_unique<TextBlock>(fc, iconFont, d.IconSize, d.Icon, iconTint) ];
	if (!d.Label.empty())
	{
		if (!d.Icon.empty())
			*row + row->AddSlot()[ std::make_unique<Spacer>(glm::vec2(6.0f, 1.0f)) ];
		// A button label never wraps (an auto-wrap measured at a transient
		// narrow width otherwise sticks: "Capture All" on two lines).
		auto label = std::make_unique<TextBlock>(fc, font, tokens::FontBody, d.Label, content);
		label->SetWrapMode(WrapMode::None);
		*row + row->AddSlot().VAlign(VAlign::Center)[ std::move(label) ];
	}
	if (d.Chevron)
	{
		*row
			+ row->AddSlot()[ std::make_unique<Spacer>(glm::vec2(4.0f, 1.0f)) ]
			+ row->AddSlot().VAlign(VAlign::Center)[ std::make_unique<TextBlock>(fc, iconFont, 11.0f, icons::ChevronDown,
				d.Enabled ? tokens::TextMuted : tokens::TextDisabled) ];
	}
	btn->SetChild(std::move(row));

	if (d.Enabled && onClick)
		btn->SetOnClicked(std::move(onClick));
	if (!d.Tooltip.empty())
	{
		std::string tip = d.Tooltip, shortcut = d.Shortcut;
		btn->SetToolTip([fc, font, tip, shortcut]() { return MakeTooltip(fc, font, tip, shortcut); });
	}
	return btn;
}

// A 1 px vertical rule between toolbar groups.
class ToolbarSeparator : public Widget
{
public:
	WISP_WIDGET_TYPE(ToolbarSeparator)
	explicit ToolbarSeparator(float height = 18.0f, float margin = 6.0f) : m_Height(height), m_Margin(margin) {}
	glm::vec2 ComputeDesiredSize() const override { return { 1.0f + m_Margin * 2.0f, m_Height }; }
	void ArrangeChildren(const Geometry&, ArrangedChildren&) const override {}
	int OnPaint(const PaintArgs& args, const Geometry& geom, DrawList& dl, int layerId, const WidgetStyleContext&) const override
	{
		const glm::vec2 pos = geom.LocalToAbsolute({ 0, 0 });
		const float h = std::min(m_Height, geom.LocalSize.y);
		dl.AddBox(layerId, { std::round(pos.x + m_Margin), std::round(pos.y + (geom.LocalSize.y - h) * 0.5f) }, { 1.0f, h }, tokens::Border, args.InheritedClipIndex);
		return layerId + 1;
	}
private:
	float m_Height;
	float m_Margin;
};

} // namespace wisp
