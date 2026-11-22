#pragma once

// The editor's colours read, the crash surfaces' included: every text
// token on every surface text sits on at WCAG's 4.5:1, icons at 3:1, plus
// the crash surfaces' own pairs. A token change that drops a pair below
// fails here; a new surface or text token gets a row.

#include "Wisp/StyleTokens.h"

#include <doctest/doctest.h>

#include <string>

namespace wisp_contrast_tests {

	namespace tokens = wisp::tokens;

	struct Pair
	{
		const char* Name;
		glm::vec4 Fg, Bg;
	};

	// A chip: TextPrimary on its tint (kit::MakePill's 0.16 alpha) over the
	// surface it sits on.
	inline Pair Chip(const char* name, const glm::vec4& tint, const glm::vec4& surface)
	{
		return { name, tokens::TextPrimary, tokens::Over(tokens::WithAlpha(tint, 0.16f), surface) };
	}

TEST_CASE("wisp tokens: the contrast ratio is WCAG's")
{
	CHECK(tokens::ContrastRatio(tokens::Hex(0xFFFFFF), tokens::Hex(0x000000)) == doctest::Approx(21.0f).epsilon(0.001));
	CHECK(tokens::ContrastRatio(tokens::Hex(0x777777), tokens::Hex(0x777777)) == doctest::Approx(1.0f));
	// Order doesn't matter; a translucent foreground is composited first.
	CHECK(tokens::ContrastRatio(tokens::Panel, tokens::TextPrimary) == doctest::Approx(tokens::ContrastRatio(tokens::TextPrimary, tokens::Panel)));
	CHECK(tokens::ContrastRatio(tokens::WithAlpha(tokens::TextPrimary, 0.0f), tokens::Panel) == doctest::Approx(1.0f));
}

TEST_CASE("wisp tokens: the crash reporter's text reads at 4.5:1")
{
	const glm::vec4 matchedLine = tokens::Over(tokens::CodeFindMatch, tokens::Field);
	const Pair text[] = {
		// The surfaces' body text.
		{ "primary on panel", tokens::TextPrimary, tokens::Panel },
		{ "primary on the header band", tokens::TextPrimary, tokens::Raised },
		{ "primary on a field (the stack, the log)", tokens::TextPrimary, tokens::Field },
		// Secondary text is 4.0:1 on the selection: a selected row paints its
		// secondary text in TextPrimary.
		{ "primary on the selected row", tokens::TextPrimary, tokens::Selection },
		{ "primary on a button", tokens::TextPrimary, tokens::Control },
		{ "primary on a hovered button", tokens::TextPrimary, tokens::Hover },
		{ "primary on a toast", tokens::TextPrimary, tokens::Popup },
		{ "secondary on panel", tokens::TextSecondary, tokens::Panel },
		{ "secondary on the header band", tokens::TextSecondary, tokens::Raised },
		{ "secondary on a field (system frames, log lines)", tokens::TextSecondary, tokens::Field },
		{ "secondary on a band (Details)", tokens::TextSecondary, tokens::PanelAlt },
		{ "a placeholder in a field", tokens::TextMuted, tokens::Field },
		// The crash site, the log's levels, the failure line.
		{ "the crash site", tokens::Warning, tokens::Field },
		{ "the selected crash site", tokens::Warning, tokens::Selection },
		{ "a warning line", tokens::Warning, tokens::Field },
		{ "an error line", tokens::Error, tokens::Field },
		{ "the failure status", tokens::Error, tokens::Panel },
		{ "a matched log line", tokens::TextPrimary, matchedLine },
		// The primary button, at rest and hovered.
		{ "the primary button", tokens::TextOnAccent, tokens::AccentFill },
		{ "the hovered primary button", tokens::TextOnAccent, tokens::AccentFillHover },
		// The chips on every surface they sit on.
		Chip("an error chip on the header band", tokens::Error, tokens::Raised),
		Chip("an info chip on the header band", tokens::Info, tokens::Raised),
		Chip("a warning chip on the header band", tokens::Warning, tokens::Raised),
		Chip("a time chip on the header band", tokens::TextSecondary, tokens::Raised),
		Chip("the crash-site pill", tokens::Warning, tokens::Field),
		Chip("the crash-site pill, selected", tokens::Warning, tokens::Selection),
		Chip("a module pill", tokens::TextSecondary, tokens::Field),
		Chip("a module pill, selected", tokens::TextSecondary, tokens::Selection),
		Chip("an app pill in the Crashes panel", tokens::Info, tokens::Panel),
		Chip("an app pill on the selected crash", tokens::Info, tokens::Selection),
		Chip("a recurrence pill", tokens::Warning, tokens::Panel),
	};
	for (const Pair& p : text)
	{
		INFO(std::string(p.Name) << ": " << tokens::ContrastRatio(p.Fg, p.Bg));
		CHECK(tokens::ContrastRatio(p.Fg, p.Bg) >= 4.5f);
	}
}

TEST_CASE("wisp tokens: the crash reporter's icons read at 3:1")
{
	const Pair icons[] = {
		{ "the crash icon on the header band", tokens::Error, tokens::Raised },
		{ "an error toast's icon", tokens::Error, tokens::Popup },
		{ "a success toast's icon", tokens::Success, tokens::Popup },
		{ "the crash badge in the status bar", tokens::Error, tokens::Backdrop },
		{ "a chevron", tokens::TextSecondary, tokens::Field },
	};
	for (const Pair& p : icons)
	{
		INFO(std::string(p.Name) << ": " << tokens::ContrastRatio(p.Fg, p.Bg));
		CHECK(tokens::ContrastRatio(p.Fg, p.Bg) >= 3.0f);
	}
}


// The editor matrix.
TEST_CASE("wisp tokens: every text token reads on every surface text sits on")
{
	struct Named { const char* Name; glm::vec4 Colour; };
	const Named surfaces[] = {
		{ "Backdrop", tokens::Backdrop }, { "Panel", tokens::Panel }, { "PanelAlt", tokens::PanelAlt },
		{ "Raised", tokens::Raised }, { "Popup", tokens::Popup }, { "Control", tokens::Control },
		{ "Field", tokens::Field }, { "CodeBackground", tokens::CodeBackground },
	};
	// TextDisabled is the exemption: disabled controls and decorative icons
	// (WCAG 1.4.3 / 1.4.11 exempt inactive components) - never readable text.
	const Named text[] = {
		{ "TextPrimary", tokens::TextPrimary }, { "TextSecondary", tokens::TextSecondary }, { "TextMuted", tokens::TextMuted },
		{ "TextLink", tokens::TextLink }, { "Success", tokens::Success }, { "Warning", tokens::Warning },
		{ "Error", tokens::Error }, { "Info", tokens::Info }, { "DocText", tokens::DocText },
		{ "DocHeading", tokens::DocHeading }, { "DocCodeText", tokens::DocCodeText }, { "DocLinkDead", tokens::DocLinkDead },
	};
	for (const Named& t : text)
		for (const Named& s : surfaces)
		{
			INFO(std::string(t.Name) << " on " << s.Name << ": " << tokens::ContrastRatio(t.Colour, s.Colour));
			CHECK(tokens::ContrastRatio(t.Colour, s.Colour) >= 4.5f);
		}
	// The code gutter's numbers on the code background.
	CHECK(tokens::ContrastRatio(tokens::CodeLineNumber, tokens::CodeBackground) >= 4.5f);
	CHECK(tokens::ContrastRatio(tokens::CodeLineNumberActive, tokens::CodeBackground) >= 4.5f);
}

TEST_CASE("wisp tokens: a selected row's text and the accent fills read")
{
	// A selected row paints primary and secondary text (coloured or muted
	// text goes primary there).
	for (const glm::vec4& selection : { tokens::Selection, tokens::SelectionInactive })
	{
		CHECK(tokens::ContrastRatio(tokens::TextPrimary, selection) >= 4.5f);
		CHECK(tokens::ContrastRatio(tokens::TextSecondary, selection) >= 4.5f);
	}
	// A popup's hovered row (Selection) and pressed row (AccentFillPressed)
	// under TextPrimary; PopupMenu's hot row under white and its 90 % white
	// sub-line; a Toggle chip's TextPrimary on its accent tint.
	CHECK(tokens::ContrastRatio(tokens::TextPrimary, tokens::AccentFillPressed) >= 4.5f);
	CHECK(tokens::ContrastRatio(tokens::WithAlpha(tokens::TextOnAccent, 0.9f), tokens::AccentFill) >= 4.5f);
	for (const glm::vec4& surface : { tokens::Panel, tokens::Raised, tokens::Popup })
	{
		CHECK(tokens::ContrastRatio(tokens::TextPrimary, tokens::Over(tokens::WithAlpha(tokens::Accent, 0.22f), surface)) >= 4.5f);
		CHECK(tokens::ContrastRatio(tokens::TextPrimary, tokens::Over(tokens::WithAlpha(tokens::Accent, 0.32f), surface)) >= 4.5f);
	}
	// White text sits on the fill accent only.
	for (const glm::vec4& fill : { tokens::AccentFill, tokens::AccentFillHover, tokens::AccentFillPressed })
	{
		INFO(tokens::ContrastRatio(tokens::TextOnAccent, fill));
		CHECK(tokens::ContrastRatio(tokens::TextOnAccent, fill) >= 4.5f);
	}
}

TEST_CASE("wisp tokens: the icon colours read at 3:1 on the panel surfaces")
{
	struct Named { const char* Name; glm::vec4 Colour; };
	const Named icons[] = {
		{ "Accent", tokens::Accent }, { "AccentHover", tokens::AccentHover }, { "Success", tokens::Success },
		{ "Warning", tokens::Warning }, { "Error", tokens::Error }, { "Info", tokens::Info },
		{ "Play", tokens::Play }, { "Stop", tokens::Stop }, { "TypeFolder", tokens::TypeFolder },
		{ "TypeScene", tokens::TypeScene }, { "TypeMesh", tokens::TypeMesh }, { "TypeMaterial", tokens::TypeMaterial },
		{ "TypeTexture", tokens::TypeTexture }, { "TypeShader", tokens::TypeShader }, { "TypeScript", tokens::TypeScript },
		{ "TypeFont", tokens::TypeFont }, { "TypeAudio", tokens::TypeAudio }, { "TypeInput", tokens::TypeInput },
		{ "TypeCubemap", tokens::TypeCubemap }, { "TypeOther", tokens::TypeOther }, { "TypeAnimation", tokens::TypeAnimation },
	};
	for (const Named& i : icons)
		for (const glm::vec4& surface : { tokens::Panel, tokens::Raised, tokens::Field })
		{
			INFO(std::string(i.Name) << ": " << tokens::ContrastRatio(i.Colour, surface));
			CHECK(tokens::ContrastRatio(i.Colour, surface) >= 3.0f);
		}
}

} // namespace wisp_contrast_tests
