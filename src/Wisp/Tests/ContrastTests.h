#pragma once

// The crash reporter's colours read - every text/background token pair
// tinycrashreport and the editor's crash surfaces (the Crashes panel, the
// status-bar notice) paint, at WCAG's 4.5:1 for text and 3:1 for icons. A
// token change that drops a pair below fails here; a new pair in those
// surfaces gets a row.

#include "Wisp/StyleTokens.h"

#include <doctest/doctest.h>

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
		{ "the primary button", tokens::TextOnAccent, tokens::AccentStrong },
		{ "the hovered primary button", tokens::TextOnAccent, tokens::AccentPressed },
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

} // namespace wisp_contrast_tests
