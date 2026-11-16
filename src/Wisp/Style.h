#pragma once

#include "Wisp/DrawList.h"
#include "Wisp/StyleTokens.h"
#include <glm/glm.hpp>

namespace wisp {

// ---- Macros ----------------------------------------------------------------

#define WISP_STYLE_TYPE(StyleName) \
	static constexpr const char* StyleTypeName = #StyleName;

// ---- Margin (reusable padding type) ----------------------------------------

struct Margin
{
	float Left = 0, Top = 0, Right = 0, Bottom = 0;
};

// ---- ButtonStyle -----------------------------------------------------------

struct ButtonStyle
{
	WISP_STYLE_TYPE(ButtonStyle)
	Brush Normal;
	Brush Hovered;
	Brush Pressed;
	Brush Disabled;
	Margin ContentPadding{ 8, 4, 8, 4 };
	glm::vec2 PressedContentOffset{ 0, 0 };
	// Rounded by default; 0 = a flat row.
	float CornerRadius = tokens::RadiusSmall;
	bool operator==(const ButtonStyle&) const = default;
};

// ---- CheckBoxStyle ---------------------------------------------------------

struct CheckBoxStyle
{
	WISP_STYLE_TYPE(CheckBoxStyle)
	Brush BoxUnchecked;
	Brush BoxChecked;
	Brush BoxHovered;
	Brush CheckMark;
	float BoxSize = 15;
	Margin LabelPadding{ 6, 0, 0, 0 };
	bool operator==(const CheckBoxStyle&) const = default;
};

// ---- SliderStyle -----------------------------------------------------------

struct SliderStyle
{
	WISP_STYLE_TYPE(SliderStyle)
	Brush Track;
	Brush Fill;
	Brush Thumb;
	Brush ThumbHovered;
	Brush ThumbDragged;
	float TrackHeight = 4;
	float ThumbSize = 13;
	bool operator==(const SliderStyle&) const = default;
};

// ---- ProgressBarStyle ------------------------------------------------------

struct ProgressBarStyle
{
	WISP_STYLE_TYPE(ProgressBarStyle)
	Brush Track;
	Brush Fill;
	Brush Marquee;
	bool operator==(const ProgressBarStyle&) const = default;
};

// ---- EditableTextStyle -----------------------------------------------------

struct EditableTextStyle
{
	WISP_STYLE_TYPE(EditableTextStyle)
	Brush Background;
	Brush HoveredBackground;
	Brush FocusedBackground;
	glm::vec4 SelectionTint = tokens::WithAlpha(tokens::Accent, 0.38f);
	glm::vec4 CaretTint = tokens::TextPrimary;
	glm::vec4 BorderTint = tokens::Border;
	glm::vec4 BorderHoveredTint = tokens::BorderStrong;
	glm::vec4 FocusTint = tokens::Accent;
	glm::vec4 TextTint = tokens::TextPrimary;
	glm::vec4 PlaceholderTint = tokens::TextMuted;
	bool operator==(const EditableTextStyle&) const = default;
};

// ---- PanelStyle (used by Border) -------------------------------------------

struct PanelStyle
{
	WISP_STYLE_TYPE(PanelStyle)
	Brush Background;
	Margin Padding{ 8, 8, 8, 8 };
	bool operator==(const PanelStyle&) const = default;
};

// ---- TabStyle (dock TabWell / DockTab) --------------------------------------

struct TabStyle
{
	WISP_STYLE_TYPE(TabStyle)
	Brush Active;
	Brush Inactive;
	Brush Hovered;
	Brush ContentBackground;
	glm::vec4 LabelTint{ 0.8f, 0.8f, 0.85f, 1.0f };
	glm::vec4 LabelActiveTint{ 1.0f, 1.0f, 1.0f, 1.0f };
	Brush CloseButton;
	glm::vec4 CloseTint{ 0.6f, 0.6f, 0.65f, 1.0f };
	float TabHeight = 34.0f;
	Margin TabPadding{ 10, 0, 10, 0 };
	bool operator==(const TabStyle&) const = default;
};

// ---- DockStyle (dividers, floating panels, drop targets) -------------------

struct DockStyle
{
	WISP_STYLE_TYPE(DockStyle)
	Brush Divider;
	float DividerThickness = 5.0f;
	float DividerGrabPad = 6.0f;
	Brush FloatingTitleBar;
	Brush FloatingTitleBarInactive;
	glm::vec4 FloatingTitleTint{ 0.8f, 0.8f, 0.85f, 1.0f };
	float FloatingTitleHeight = 30.0f;
	glm::vec4 DropPreviewTint{ 0.2f, 0.45f, 0.9f, 0.30f };
	Brush DiamondIdle;
	Brush DiamondHovered;
	glm::vec4 DiamondTint{ 0.8f, 0.8f, 0.85f, 1.0f };
	glm::vec2 FloatingMinSize{ 240.0f, 160.0f };
	bool operator==(const DockStyle&) const = default;
};

// ---- ColourPickerStyle ------------------------------------------------------

struct ColourPickerStyle
{
	WISP_STYLE_TYPE(ColourPickerStyle)
	glm::vec4 SwatchBorder{ 0.45f, 0.45f, 0.55f, 1.0f };
	glm::vec4 SwatchBorderHovered{ 0.7f, 0.7f, 0.8f, 1.0f };
	glm::vec4 SwatchBorderPressed{ 0.95f, 0.95f, 1.0f, 1.0f };
	float SwatchBorderWidth = 1.0f;
	glm::vec4 PopupBackground{ 0.09f, 0.09f, 0.13f, 1.0f };
	glm::vec4 BarThumb{ 1.0f, 1.0f, 1.0f, 1.0f };
	float BarThickness = 14.0f;
	glm::vec2 PlaneSize{ 160.0f, 160.0f };
	glm::vec2 WheelSize{ 180.0f, 180.0f };
	float PopupPadding = 8.0f;
	glm::vec4 LabelTint{ 0.8f, 0.8f, 0.85f, 1.0f };
	bool operator==(const ColourPickerStyle&) const = default;
};

} // namespace wisp
