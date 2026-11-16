#pragma once

// The design tokens - the one palette and the one set of metrics every Wisp
// default style and every editor panel paints with. Immutable by
// construction (constexpr; the global-state rule's static-const category).
// Colours are sRGB as written (Wisp composites them unconverted). A future
// theme (WISP TE1) overrides styles through Theme, not by mutating these.

#include <glm/glm.hpp>
#include <cstdint>

namespace wisp::tokens {

constexpr glm::vec4 Hex(uint32_t rgb, float a = 1.0f)
{
	return glm::vec4(((rgb >> 16) & 0xFF) / 255.0f, ((rgb >> 8) & 0xFF) / 255.0f, (rgb & 0xFF) / 255.0f, a);
}

constexpr glm::vec4 WithAlpha(const glm::vec4& c, float a) { return glm::vec4(c.r, c.g, c.b, a); }

// ---- Surfaces (darkest -> lightest) -------------------------------------------
inline constexpr glm::vec4 Backdrop      = Hex(0x111113); // app background, dock gutters, menu bar, tab strips
inline constexpr glm::vec4 Panel         = Hex(0x1B1B1E); // panel content
inline constexpr glm::vec4 PanelAlt      = Hex(0x1F1F23); // alternating rows, subtle bands
inline constexpr glm::vec4 Raised        = Hex(0x242428); // panel headers, toolbars, section headers
inline constexpr glm::vec4 Popup         = Hex(0x26262B); // menus, dropdown lists, tooltips
inline constexpr glm::vec4 Control       = Hex(0x2C2C31); // buttons at rest
inline constexpr glm::vec4 Hover         = Hex(0x35353B); // hovered rows/buttons
inline constexpr glm::vec4 Pressed       = Hex(0x1E1E22);
inline constexpr glm::vec4 Field         = Hex(0x131315); // text/number field backgrounds
inline constexpr glm::vec4 FieldHover    = Hex(0x18181B);
inline constexpr glm::vec4 Selection     = Hex(0x23426E); // the selected row (accent-tinted)
inline constexpr glm::vec4 SelectionInactive = Hex(0x2D3440);
inline constexpr glm::vec4 Overlay       = Hex(0x18181B, 0.86f); // viewport overlays (translucent)
inline constexpr glm::vec4 Scrim         = Hex(0x000000, 0.45f); // modal backdrop

// ---- Lines ----------------------------------------------------------------------
inline constexpr glm::vec4 Border        = Hex(0x303035);
inline constexpr glm::vec4 BorderStrong  = Hex(0x3E3E45);
inline constexpr glm::vec4 Separator     = Hex(0x2A2A2F);
inline constexpr glm::vec4 Shadow        = Hex(0x000000, 0.35f);

// ---- Text -----------------------------------------------------------------------
inline constexpr glm::vec4 TextPrimary   = Hex(0xE2E2E6);
inline constexpr glm::vec4 TextSecondary = Hex(0xA2A2AB);
inline constexpr glm::vec4 TextMuted     = Hex(0x7D7D86);
inline constexpr glm::vec4 TextDisabled  = Hex(0x5E5E66);
inline constexpr glm::vec4 TextOnAccent  = Hex(0xFFFFFF);
inline constexpr glm::vec4 TextLink      = Hex(0x6EA8FF);

// ---- Accent + semantic ------------------------------------------------------------
inline constexpr glm::vec4 Accent        = Hex(0x3F86F2);
inline constexpr glm::vec4 AccentHover   = Hex(0x5596F5);
inline constexpr glm::vec4 AccentPressed = Hex(0x2F70D6);
inline constexpr glm::vec4 AccentSubtle  = Hex(0x3F86F2, 0.18f);
inline constexpr glm::vec4 Success       = Hex(0x3FB950);
inline constexpr glm::vec4 Warning       = Hex(0xE0A82E);
inline constexpr glm::vec4 Error         = Hex(0xE5534B);
inline constexpr glm::vec4 Info          = Hex(0x58A6FF);
inline constexpr glm::vec4 Play          = Hex(0x48C267);
inline constexpr glm::vec4 Stop          = Hex(0xE5534B);

inline constexpr glm::vec4 AxisX         = Hex(0xD94A4A);
inline constexpr glm::vec4 AxisY         = Hex(0x69B33E);
inline constexpr glm::vec4 AxisZ         = Hex(0x3C7BE0);

// ---- Asset type colours (the Content Browser's type stripes) -----------------------
inline constexpr glm::vec4 TypeFolder    = Hex(0xC2A066);
inline constexpr glm::vec4 TypeScene     = Hex(0xE8913A);
inline constexpr glm::vec4 TypeMesh      = Hex(0x3CC3D4);
inline constexpr glm::vec4 TypeMaterial  = Hex(0x63B54B);
inline constexpr glm::vec4 TypeTexture   = Hex(0xD24A63);
inline constexpr glm::vec4 TypeShader    = Hex(0x9A72DD);
inline constexpr glm::vec4 TypeScript    = Hex(0xE0C341);
inline constexpr glm::vec4 TypeFont      = Hex(0x8C9DB8);
inline constexpr glm::vec4 TypeAudio     = Hex(0xEE7A42);
inline constexpr glm::vec4 TypeInput     = Hex(0xD566B0);
inline constexpr glm::vec4 TypeCubemap   = Hex(0xE6B653);
inline constexpr glm::vec4 TypeOther     = Hex(0x8A8A93);

// ---- Metrics ----------------------------------------------------------------------
inline constexpr float RadiusSmall   = 3.0f;
inline constexpr float RadiusMedium  = 5.0f;
inline constexpr float RadiusLarge   = 8.0f;

inline constexpr float Space1 = 4.0f;
inline constexpr float Space2 = 8.0f;
inline constexpr float Space3 = 12.0f;
inline constexpr float Space4 = 16.0f;

inline constexpr float RowHeight      = 24.0f;
inline constexpr float FieldHeight    = 22.0f;
inline constexpr float TabHeight      = 30.0f;
inline constexpr float ToolbarHeight  = 34.0f;
inline constexpr float MenuBarHeight  = 30.0f;
inline constexpr float StatusBarHeight = 24.0f;
inline constexpr float IconSize       = 16.0f;
inline constexpr float IndentWidth    = 16.0f;

// Font pixel sizes (pixel size = ascent - descent; the vendored faces put
// that at 1.1 em, so FontBody 14 is a ~12.7 px em).
inline constexpr float FontBody    = 14.0f;
inline constexpr float FontSmall   = 12.5f;
inline constexpr float FontHeading = 14.0f;
inline constexpr float FontTitle   = 16.5f;
inline constexpr float FontMono    = 13.5f;

} // namespace wisp::tokens
