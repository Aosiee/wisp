#pragma once

// The manual's Markdown parser - the block structure (headings, fences,
// nested lists, callouts, tables), the inline spans (emphasis, code,
// links, doc links, images, escapes).

#include "doctest/doctest.h"
#include "Wisp/Doc/Markdown.h"

namespace wisp_markdown_tests {

	using namespace wisp::doc;

TEST_CASE("markdown: headings, paragraphs and rules")
{
	const Document doc = Parse("# Title #\n\nFirst line\nsecond line.\n\n---\n### Small");
	REQUIRE(doc.Blocks.size() == 4);
	CHECK(doc.Blocks[0].Kind == BlockKind::Heading);
	CHECK(doc.Blocks[0].Level == 1);
	CHECK(PlainText(doc.Blocks[0].Inlines) == "Title");
	CHECK(doc.Blocks[1].Kind == BlockKind::Paragraph);
	CHECK(PlainText(doc.Blocks[1].Inlines) == "First line second line.");
	CHECK(doc.Blocks[2].Kind == BlockKind::Rule);
	CHECK(doc.Blocks[3].Level == 3);
	CHECK(Parse("#NoSpace").Blocks[0].Kind == BlockKind::Paragraph);
}

TEST_CASE("markdown: fenced code keeps its text and language")
{
	const Document doc = Parse("Before\n```cluau\nlet x = 1\n\n  // indented\n```\nAfter");
	REQUIRE(doc.Blocks.size() == 3);
	CHECK(doc.Blocks[1].Kind == BlockKind::Code);
	CHECK(doc.Blocks[1].Info == "cluau");
	CHECK(doc.Blocks[1].Text == "let x = 1\n\n  // indented");
	// An unclosed fence runs to the end; ~~~ fences too.
	CHECK(Parse("~~~\na\n").Blocks[0].Text == "a\n");
}

TEST_CASE("markdown: lists nest by indentation and track tightness")
{
	const Document doc = Parse("- one\n- two\n  - inner\n  - inner2\n- three\n\n1. a\n2. b");
	REQUIRE(doc.Blocks.size() == 2);
	const Block& bullets = doc.Blocks[0];
	CHECK(bullets.Kind == BlockKind::List);
	CHECK_FALSE(bullets.Ordered);
	CHECK(bullets.Tight);
	REQUIRE(bullets.Children.size() == 3);
	const Block& two = bullets.Children[1];
	REQUIRE(two.Children.size() == 2);
	CHECK(two.Children[1].Kind == BlockKind::List);
	CHECK(two.Children[1].Children.size() == 2);
	const Block& numbers = doc.Blocks[1];
	CHECK(numbers.Ordered);
	CHECK(numbers.Start == 1);
	CHECK(numbers.Children.size() == 2);

	const Document loose = Parse("- a\n\n- b\n");
	CHECK_FALSE(loose.Blocks[0].Tight);
	// A lazy continuation joins the item's paragraph.
	const Document lazy = Parse("- first\ncontinued");
	CHECK(PlainText(lazy.Blocks[0].Children[0].Children[0].Inlines) == "first continued");
	// A marker without a space is text.
	CHECK(Parse("-not a list").Blocks[0].Kind == BlockKind::Paragraph);
}

TEST_CASE("markdown: block quotes and callouts")
{
	const Document doc = Parse("> [!WARNING]\n> Careful.\n\n> plain");
	REQUIRE(doc.Blocks.size() == 2);
	CHECK(doc.Blocks[0].Kind == BlockKind::Quote);
	CHECK(doc.Blocks[0].Info == "warning");
	CHECK(PlainText(doc.Blocks[0].Children[0].Inlines) == "Careful.");
	CHECK(doc.Blocks[1].Info.empty());
}

TEST_CASE("markdown: pipe tables with alignment and escaped pipes")
{
	const Document doc = Parse("| Name | Value |\n|:---|---:|\n| `a|b` | x \\| y |\n| one |\n\nafter");
	REQUIRE(doc.Blocks.size() == 2);
	const Block& table = doc.Blocks[0];
	CHECK(table.Kind == BlockKind::Table);
	REQUIRE(table.Aligns.size() == 2);
	CHECK(table.Aligns[0] == Align::Left);
	CHECK(table.Aligns[1] == Align::Right);
	REQUIRE(table.Rows.size() == 3);
	CHECK(table.Rows[1][0][0].Kind == InlineKind::Code);
	CHECK(table.Rows[1][0][0].Text == "a|b");
	CHECK(PlainText(table.Rows[1][1]) == "x | y");
	CHECK(table.Rows[2].size() == 2); // a short row is padded
	// A doc link's label bar is not a cell bar.
	const Document linked = Parse("| a | b |\n|---|---|\n| [[member:T.x|x]] | y |");
	REQUIRE(linked.Blocks[0].Rows.size() == 2);
	CHECK(linked.Blocks[0].Rows[1][0][0].Kind == InlineKind::DocLink);
	CHECK(linked.Blocks[0].Rows[1][0][0].Text == "x");
	CHECK(PlainText(linked.Blocks[0].Rows[1][1]) == "y");
}

TEST_CASE("markdown: inline spans")
{
	const std::vector<Inline> in = ParseInlines("a *em* **strong** `co*de` [link](http://x \"t\") ![alt](img.png) [[type:Foo]] [[cvar:r.x|the knob]] \\*lit\\* snake_case_name");
	std::vector<InlineKind> kinds;
	for (const Inline& i : in)
		if (i.Kind != InlineKind::Text)
			kinds.push_back(i.Kind);
	REQUIRE(kinds.size() == 7);
	CHECK(kinds[0] == InlineKind::Emphasis);
	CHECK(kinds[1] == InlineKind::Strong);
	CHECK(kinds[2] == InlineKind::Code);
	CHECK(kinds[3] == InlineKind::Link);
	CHECK(kinds[4] == InlineKind::Image);
	CHECK(kinds[5] == InlineKind::DocLink);
	CHECK(kinds[6] == InlineKind::DocLink);
	const Inline* link = nullptr;
	const Inline* named = nullptr;
	for (const Inline& i : in)
	{
		if (i.Kind == InlineKind::Link)
			link = &i;
		if (i.Kind == InlineKind::DocLink && i.Target == "cvar:r.x")
			named = &i;
	}
	REQUIRE(link);
	CHECK(link->Target == "http://x");
	CHECK(PlainText(link->Children) == "link");
	REQUIRE(named);
	CHECK(named->Text == "the knob");
	CHECK(PlainText(in).find("*lit*") != std::string::npos);
	CHECK(PlainText(in).find("snake_case_name") != std::string::npos);
	// Unmatched delimiters stay literal; strong nests inside emphasis.
	CHECK(PlainText(ParseInlines("2 * 3 and a_b")) == "2 * 3 and a_b");
	const std::vector<Inline> nested = ParseInlines("*a **b** c*");
	REQUIRE(nested.size() == 1);
	CHECK(nested[0].Kind == InlineKind::Emphasis);
	CHECK(nested[0].Children.size() == 3);
}

TEST_CASE("markdown: slugs")
{
	CHECK(Slug("Moving around the viewport") == "moving-around-the-viewport");
	CHECK(Slug("  A/B -- mode!") == "a-b-mode");
}

} // namespace wisp_markdown_tests
