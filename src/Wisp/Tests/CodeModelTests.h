#pragma once

// The code editor's headless model - the line document, caret movement,
// editing (auto-indent, pairs, multi-caret, line operations), undo
// coalescing and the dirty state, find/replace, indentation folding with
// the visible-line mapping, and the Luau tokenizer's carried state.

#include "doctest/doctest.h"
#include "Wisp/Code/EditorModel.h"
#include "Wisp/Code/Tokenizer.h"

#include <string>
#include <vector>

namespace wisp_code_tests {

	using namespace wisp::code;

	inline TextPos P(int line, int col) { return { line, col }; }

TEST_CASE("code model: the document splits lines, normalises CRLF and replaces across lines")
{
	TextDocument doc("a\r\nbc\nd");
	CHECK(doc.LineCount() == 3);
	CHECK(doc.Line(1) == "bc");
	CHECK(doc.GetRange({ P(0, 0), P(2, 1) }) == "a\nbc\nd");
	const TextPos end = doc.Replace({ P(0, 1), P(1, 1) }, "X\nY");
	CHECK(end == P(1, 1));
	CHECK(doc.GetText() == "aX\nYc\nd");
	CHECK(EndOfInsert(P(3, 2), "ab\ncde") == P(4, 3));
	CHECK(NextCodePointLength("\xC3\xA9x", 0) == 2);
	CHECK(PrevCodePointLength("x\xC3\xA9", 3) == 2);
}

TEST_CASE("code model: movement - code points, words, smart home, vertical with the preferred column across tabs")
{
	EditorModel model("\tlocal foo = 1\n\xC3\xA9t\nshort");
	model.SetCaret(P(0, 0), false);
	model.MoveCarets(CaretMove::LineStart, false);
	CHECK(model.PrimaryCaret().Pos == P(0, 1)); // the first non-blank
	model.MoveCarets(CaretMove::LineStart, false);
	CHECK(model.PrimaryCaret().Pos == P(0, 0));
	model.SetCaret(P(0, 1), false);
	model.MoveCarets(CaretMove::WordRight, false);
	CHECK(model.PrimaryCaret().Pos == P(0, 7)); // past "local "
	model.MoveCarets(CaretMove::WordRight, true);
	CHECK(model.PrimaryCaret().Selection().Start == P(0, 7));
	model.SetCaret(P(1, 0), false);
	model.MoveCarets(CaretMove::Right, false);
	CHECK(model.PrimaryCaret().Pos == P(1, 2)); // one code point (2 bytes)
	// Visual column 8 (tab = 4) on line 0 -> line 2 ends at 5.
	model.SetCaret(P(0, 5), false);
	model.MoveCarets(CaretMove::Down, false);
	model.MoveCarets(CaretMove::Down, false);
	CHECK(model.PrimaryCaret().Pos == P(2, 5));
	CHECK(EditorModel::VisualColumn("\tab", 2) == 5);
	CHECK(EditorModel::ByteForVisualColumn("\tab", 5) == 2);
}

TEST_CASE("code model: typing auto-indents blocks, pairs brackets and quotes, and steps over closers")
{
	EditorModel model("\tif x then");
	model.SetCaret(P(0, 10), false);
	model.InsertNewline();
	CHECK(model.GetText() == "\tif x then\n\t\t");
	model.TypeCodePoint('f');
	model.TypeCodePoint('(');
	CHECK(model.GetText() == "\tif x then\n\t\tf()");
	CHECK(model.PrimaryCaret().Pos == P(1, 4));
	model.TypeCodePoint(')');
	CHECK(model.GetText() == "\tif x then\n\t\tf()");
	CHECK(model.PrimaryCaret().Pos == P(1, 5));

	EditorModel braces("t = {}");
	braces.SetCaret(P(0, 5), false);
	braces.InsertNewline();
	CHECK(braces.GetText() == "t = {\n\t\n}");
	CHECK(braces.PrimaryCaret().Pos == P(1, 1));

	EditorModel quotes("x");
	quotes.SetCaret(P(0, 1), false);
	quotes.TypeCodePoint('"'); // after a word: no pair
	CHECK(quotes.GetText() == "x\"");
	quotes.SelectAll();
	quotes.TypeCodePoint('(');
	CHECK(quotes.GetText() == "(x\")");
	CHECK(quotes.PrimaryCaret().Selection().Start == P(0, 1));
	CHECK(quotes.PrimaryCaret().Selection().End == P(0, 3));

	EditorModel pair("()");
	pair.SetCaret(P(0, 1), false);
	pair.Backspace(false);
	CHECK(pair.GetText() == "");
}

TEST_CASE("code model: multi-caret - Ctrl+D occurrences, a typed character on each, a paste distributed line per caret")
{
	EditorModel model("foo bar foo\nfoo");
	model.SetCaret(P(0, 1), false);
	model.AddNextOccurrence(); // selects the word
	CHECK(model.PrimaryCaret().Selection().End == P(0, 3));
	model.AddNextOccurrence();
	model.AddNextOccurrence();
	REQUIRE(model.Carets().size() == 3);
	model.TypeCodePoint('z');
	CHECK(model.GetText() == "z bar z\nz");
	model.InsertText("a\nb\nc\n");
	CHECK(model.GetText() == "za bar zb\nzc");
	model.CollapseCarets();
	CHECK(model.Carets().size() == 1);
}

TEST_CASE("code model: undo coalesces typing, the dirty state follows undo/redo and a discarded clean branch")
{
	EditorModel model("x");
	CHECK(!model.IsDirty());
	model.SetCaret(P(0, 1), false);
	for (char c : std::string("abc"))
		model.TypeCodePoint((uint32_t)c);
	CHECK(model.IsDirty());
	REQUIRE(model.Undo());
	CHECK(model.GetText() == "x"); // one step
	CHECK(!model.IsDirty());
	REQUIRE(model.Redo());
	CHECK(model.GetText() == "xabc");
	model.MarkClean();
	model.Undo();
	model.TypeCodePoint('Q'); // the clean state was on the discarded redo branch
	CHECK(model.IsDirty());
	model.Undo();
	CHECK(model.IsDirty());
}

TEST_CASE("code model: line operations - indent/outdent, toggle comment, duplicate, move, cut whole lines")
{
	EditorModel model("a\nb\nc");
	model.SetCaret(P(0, 0), false);
	model.SetCaret(P(1, 1), true);
	model.Indent();
	CHECK(model.GetText() == "\ta\n\tb\nc");
	model.Outdent();
	CHECK(model.GetText() == "a\nb\nc");
	model.ToggleComment();
	CHECK(model.GetText() == "-- a\n-- b\nc");
	model.ToggleComment();
	CHECK(model.GetText() == "a\nb\nc");
	model.SetCaret(P(2, 0), false);
	model.MoveLines(-1);
	CHECK(model.GetText() == "a\nc\nb");
	CHECK(model.PrimaryCaret().Pos.Line == 1);
	model.DuplicateLines();
	CHECK(model.GetText() == "a\nc\nc\nb");
	CHECK(model.PrimaryCaret().Pos.Line == 2);
	CHECK(model.Cut() == "c\n");
	CHECK(model.GetText() == "a\nc\nb");
	CHECK(model.CopySelection() == "b\n");
}

TEST_CASE("code model: find wraps, replace next and all, go to line")
{
	EditorModel model("Spin spin\nSPIN");
	model.SetCaret(P(1, 4), false);
	REQUIRE(model.FindNext("spin", false));
	CHECK(model.PrimaryCaret().Selection().Start == P(0, 0)); // wrapped
	REQUIRE(model.FindNext("spin", true));
	CHECK(model.PrimaryCaret().Selection().Start == P(0, 5));
	CHECK(model.CountMatches("spin", false) == 3);
	model.ReplaceNext("spin", "run", true);
	CHECK(model.GetText() == "Spin run\nSPIN");
	CHECK(model.ReplaceAll("SPIN", "go", false) == 2);
	CHECK(model.GetText() == "go run\ngo");
	model.Undo();
	CHECK(model.GetText() == "Spin run\nSPIN");
	model.GoToLine(99);
	CHECK(model.PrimaryCaret().Pos == P(1, 0));
}

TEST_CASE("code model: indentation folds hide their body, the visible mapping skips it and edits keep the fold")
{
	EditorModel model("function f()\n\tlocal a = 1\n\n\tif a then\n\t\tb()\n\tend\nend\nx = 1");
	const std::vector<FoldRange>& folds = model.FoldRanges();
	REQUIRE(folds.size() == 2);
	CHECK(folds[0].StartLine == 0);
	CHECK(folds[0].EndLine == 5);
	CHECK(folds[1].StartLine == 3);
	CHECK(folds[1].EndLine == 4);
	model.SetCaret(P(4, 2), false);
	model.ToggleFold(0);
	CHECK(model.IsFolded(0));
	CHECK(model.PrimaryCaret().Pos.Line == 0); // moved out of the fold
	CHECK(model.VisibleLineCount() == 3);
	CHECK(model.LineForVisible(1) == 6);
	CHECK(model.IsHidden(3));
	model.SetCaret(P(7, 0), false);
	model.InsertNewline(); // below the fold: it stays folded
	CHECK(model.IsFolded(0));
	model.SetCaret(P(0, 0), false);
	model.InsertText("--\n"); // above it: it moves down a line
	CHECK(model.IsFolded(1));
	model.GoToLine(3); // inside: it opens
	CHECK(!model.IsFolded(1));
}

TEST_CASE("code model: a 10k-line document edits and maps lines without trouble")
{
	std::string text;
	for (int i = 0; i < 10000; i++)
		text += "\tlocal value" + std::to_string(i) + " = " + std::to_string(i) + "\n";
	EditorModel model(text);
	model.SetCaret(P(5000, 1), false);
	for (int i = 0; i < 200; i++)
	{
		model.InsertNewline();
		model.TypeCodePoint('x');
	}
	CHECK(model.Doc().LineCount() == 10201);
	CHECK(model.VisibleLineCount() == 10201);
	CHECK(model.CountMatches("local", true) == 10000);
}

TEST_CASE("code tokenizer: Luau keywords, literals, numbers, strings, globals, calls and long comments across lines")
{
	LuauTokenizer tokenizer({ "World", "vec3" });
	std::vector<Token> tokens;
	CHECK(tokenizer.TokenizeLine("local t = World.Spawn(\"x\", 0x1F) -- go", 0, tokens) == 0);
	auto kindAt = [&](int col) {
		for (const Token& t : tokens)
			if (col >= t.Start && col < t.Start + t.Length)
				return t.Kind;
		return TokenKind::Plain;
	};
	CHECK(kindAt(0) == TokenKind::Keyword);
	CHECK(kindAt(10) == TokenKind::Global);
	CHECK(kindAt(16) == TokenKind::Function); // Spawn(
	CHECK(kindAt(23) == TokenKind::String);
	CHECK(kindAt(28) == TokenKind::Number);
	CHECK(kindAt(34) == TokenKind::Comment);

	tokens.clear();
	const uint32_t open = tokenizer.TokenizeLine("x = nil --[==[ start", 0, tokens);
	CHECK(open != 0);
	CHECK(kindAt(4) == TokenKind::Literal);
	tokens.clear();
	CHECK(tokenizer.TokenizeLine("still ]] inside", open, tokens) == open);
	CHECK(kindAt(0) == TokenKind::Comment);
	tokens.clear();
	CHECK(tokenizer.TokenizeLine("done ]==] y", open, tokens) == 0);
	CHECK(kindAt(0) == TokenKind::Comment);
	CHECK(kindAt(10) == TokenKind::Plain);
}

} // namespace wisp_code_tests
