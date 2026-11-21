#pragma once

// The manual's Markdown, parsed into a document tree
// that both wisp::DocView (the editor) and tinydoc (the site) render - one
// parser, so a page never differs between them. Headless and engine-free.
//
// The subset: ATX headings, paragraphs, *emphasis* / _emphasis_,
// **strong**, `code`, fenced code (``` or ~~~, with an info string), `-` /
// `*` / `+` and `1.` lists (nested by indentation), block quotes (a first
// line `[!NOTE]` / `[!TIP]` / `[!WARNING]` makes a callout), pipe tables
// with an alignment row, `---` rules, [links](url), ![images](path) and the
// doc links `[[id]]` / `[[id|label]]`. Backslash escapes any punctuation.
// Raw HTML is text.

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace wisp::doc {

	enum class InlineKind : uint8_t
	{
		Text,
		Emphasis,
		Strong,
		Code,
		Link,    // Target = the url, Children = the label
		DocLink, // Target = the doc id, Text = the label ("" = the host's name for the id)
		Image,   // Target = the path, Text = the alt text
	};

	struct Inline
	{
		InlineKind Kind = InlineKind::Text;
		std::string Text;
		std::string Target;
		std::vector<Inline> Children;
	};

	enum class BlockKind : uint8_t
	{
		Heading,   // Level 1-6, Inlines
		Paragraph, // Inlines
		Code,      // Info (the language), Text
		List,      // Ordered, Start, Children = the items
		ListItem,  // Children
		Quote,     // Info = "" or the callout kind ("note", "tip", "warning"), Children
		Table,     // Rows (the first is the header), Aligns
		Rule,
	};

	enum class Align : uint8_t { Default, Left, Centre, Right };

	struct Block
	{
		BlockKind Kind = BlockKind::Paragraph;
		int Level = 0;
		bool Ordered = false;
		int Start = 1;
		bool Tight = true; // a list with no blank line between its items
		std::string Info;
		std::string Text;
		std::vector<Inline> Inlines;
		std::vector<Block> Children;
		std::vector<std::vector<std::vector<Inline>>> Rows; // row -> cell -> inlines
		std::vector<Align> Aligns;
	};

	struct Document
	{
		std::vector<Block> Blocks;
	};

	Document Parse(std::string_view markdown);
	std::vector<Inline> ParseInlines(std::string_view text);

	// The inlines as plain text (a heading's anchor text, a search snippet).
	std::string PlainText(const std::vector<Inline>& inlines);
	// A heading's anchor: lower case, runs of other characters as one '-'.
	std::string Slug(std::string_view text);

} // namespace wisp::doc
