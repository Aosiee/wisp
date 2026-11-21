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
// ---- Code (wisp::CodeEditor) --------------------------------
inline constexpr glm::vec4 CodeBackground   = Hex(0x161618);
inline constexpr glm::vec4 CodeGutter       = Hex(0x161618);
inline constexpr glm::vec4 CodeLineNumber   = Hex(0x55555D);
inline constexpr glm::vec4 CodeLineNumberActive = Hex(0xA2A2AB);
inline constexpr glm::vec4 CodeCurrentLine  = Hex(0xFFFFFF, 0.035f);
// The debugger's gutter and the paused line; the drop caret.
inline constexpr glm::vec4 CodeBreakpoint   = Hex(0xE5484D);
inline constexpr glm::vec4 CodeBreakpointDisabled = Hex(0x8A8A93);
inline constexpr glm::vec4 CodeLogpoint     = Hex(0xD4A72C);
inline constexpr glm::vec4 CodeExecutionLine = Hex(0xE0C341, 0.16f);
inline constexpr glm::vec4 CodeExecutionArrow = Hex(0xF2D04B);
inline constexpr glm::vec4 CodeDropCaret    = Hex(0x3D8BFD);
// The version control change markers.
inline constexpr glm::vec4 CodeChangeAdded    = Hex(0x3FA35B);
inline constexpr glm::vec4 CodeChangeModified = Hex(0x3D8BFD);
inline constexpr glm::vec4 CodeChangeDeleted  = Hex(0xE5484D);
inline constexpr glm::vec4 CodeSelection    = Hex(0x264F78);
inline constexpr glm::vec4 CodeFindMatch    = Hex(0xE0A82E, 0.22f);
inline constexpr glm::vec4 CodeIndentGuide  = Hex(0xFFFFFF, 0.06f);
inline constexpr glm::vec4 CodeBracketMatch = Hex(0xFFFFFF, 0.35f);
inline constexpr glm::vec4 CodeCaret        = Hex(0xE2E2E6);
inline constexpr glm::vec4 CodeMinimap      = Hex(0x131315);
inline constexpr glm::vec4 CodeMinimapView  = Hex(0xFFFFFF, 0.07f);
inline constexpr glm::vec4 CodeFoldMarker   = Hex(0x7D7D86);
inline constexpr glm::vec4 SyntaxPlain      = Hex(0xD4D4D8);
inline constexpr glm::vec4 SyntaxKeyword    = Hex(0xC586C0);
inline constexpr glm::vec4 SyntaxLiteral    = Hex(0x569CD6);
inline constexpr glm::vec4 SyntaxNumber     = Hex(0xB5CEA8);
inline constexpr glm::vec4 SyntaxString     = Hex(0xCE9178);
inline constexpr glm::vec4 SyntaxComment    = Hex(0x6A9955);
inline constexpr glm::vec4 SyntaxOperator   = Hex(0xA2A2AB);
inline constexpr glm::vec4 SyntaxFunction   = Hex(0xDCDCAA);
inline constexpr glm::vec4 SyntaxGlobal     = Hex(0x4EC9B0);
// The cards' type text, the occurrence boxes, the inline diagnostics'
// line band and the completion kind chips.
inline constexpr glm::vec4 SyntaxType       = Hex(0x4EC9B0);
inline constexpr glm::vec4 SyntaxParameter  = Hex(0x9CDCFE);
inline constexpr glm::vec4 CodeOccurrence   = Hex(0xFFFFFF, 0.09f);
inline constexpr float CodeDiagnosticBandAlpha = 0.07f;
inline constexpr float CodeInlineDiagnosticAlpha = 0.78f;
inline constexpr float CodeKindChipAlpha = 0.18f;
inline constexpr glm::vec4 CodeIndentGuideActive = Hex(0xFFFFFF, 0.22f);
inline constexpr glm::vec4 CodeScrollThumb  = Hex(0xFFFFFF, 0.24f);
// Bracket pairs cycle three colours by depth. The semantic colours (by
// binding) and inlay hints.
inline constexpr glm::vec4 SyntaxLocal      = Hex(0x9CDCFE);
inline constexpr glm::vec4 SyntaxParam      = Hex(0xD7BA7D);
inline constexpr glm::vec4 SyntaxUpvalue    = Hex(0x75BEFF);
inline constexpr glm::vec4 SyntaxProperty   = Hex(0xBFD9EE);
inline constexpr glm::vec4 SyntaxBuiltin    = Hex(0x4FC1FF);
inline constexpr glm::vec4 CodeInlayText    = Hex(0x8B8B94);
inline constexpr glm::vec4 CodeInlayChip    = Hex(0xFFFFFF, 0.05f);
inline constexpr glm::vec4 CodePeekFocus    = Hex(0xE0A82E, 0.14f);
inline constexpr glm::vec4 SyntaxBracket1   = Hex(0xFFD700);
inline constexpr glm::vec4 SyntaxBracket2   = Hex(0xDA70D6);
inline constexpr glm::vec4 SyntaxBracket3   = Hex(0x179FFF);

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

// The doc view: the manual and reference pages.
inline constexpr float DocFontBody  = 14.5f;
inline constexpr float DocFontCode  = 13.0f;
inline constexpr float DocFontH1    = 24.0f;
inline constexpr float DocFontH2    = 19.0f;
inline constexpr float DocFontH3    = 16.0f;
inline constexpr float DocFontH4    = 14.5f;
inline constexpr float DocMeasure   = 860.0f; // the widest text column
inline constexpr float DocPad       = 20.0f;
inline constexpr float DocBlockGap  = 10.0f;
inline constexpr float DocListIndent = 22.0f;
inline constexpr float DocCodePad   = 10.0f;
inline constexpr float DocCellPad   = 8.0f;
inline constexpr glm::vec4 DocText        = Hex(0xCDCDD3);
inline constexpr glm::vec4 DocHeading     = Hex(0xEDEDF0);
inline constexpr glm::vec4 DocInlineCode  = Hex(0xFFFFFF, 0.07f);
inline constexpr glm::vec4 DocCodeText    = Hex(0xDADADF);
inline constexpr glm::vec4 DocLinkDead    = Hex(0xE5534B, 0.85f);
inline constexpr glm::vec4 DocQuoteBar    = Hex(0x4A4A52);
inline constexpr glm::vec4 DocNote        = Hex(0x58A6FF);
inline constexpr glm::vec4 DocTip         = Hex(0x3FB950);
inline constexpr glm::vec4 DocWarning     = Hex(0xE0A82E);
inline constexpr float DocCalloutAlpha    = 0.08f;

} // namespace wisp::tokens
