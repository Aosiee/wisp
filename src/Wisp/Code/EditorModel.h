#pragma once

// The code editor's headless model - the document, the carets
// (multi-caret: each a position + an anchor), every editing command the
// widget maps keys to, undo/redo (typing coalesces), find/replace, and
// indentation folding with the visible-line mapping. wisp::CodeEditor
// paints it and feeds it input; the tests drive it directly.
//
// Columns are bytes; a "visual" column expands tabs to kTabWidth. Carets
// are kept sorted, non-overlapping and off folded lines. Multi-caret edits
// apply bottom-up so earlier positions stay valid, shifting the carets
// already edited.

#include "Wisp/Code/TextDocument.h"

#include <set>
#include <string>
#include <string_view>
#include <vector>

namespace wisp::code {

	struct Caret
	{
		TextPos Pos;
		TextPos Anchor;
		int PreferredVisualCol = -1; // Up/Down keep it (-1 = take the caret's)

		bool HasSelection() const { return Pos != Anchor; }
		TextRange Selection() const { return TextRange::Of(Pos, Anchor); }
	};

	struct FoldRange
	{
		int StartLine = 0; // stays visible (the header)
		int EndLine = 0;   // the last hidden line
	};

	enum class CaretMove : uint8_t
	{
		Left, Right, WordLeft, WordRight, Up, Down,
		LineStart, LineEnd, DocStart, DocEnd, PageUp, PageDown,
	};

	class EditorModel
	{
	public:
		static constexpr int kTabWidth = 4;

		EditorModel() = default;
		explicit EditorModel(std::string_view text) { SetText(text); }

		// The language's rules - Ctrl+/'s line comment, and whether Enter
		// closes a Luau block header with `end` (cluau's braces pair
		// instead).
		struct LanguageRules
		{
			std::string LineComment = "--";
			bool LuauBlockClose = true;
			// C-style blocks (cluau): Enter after an unclosed `{` adds the
			// `}`, and a `}` typed on a blank line steps back one indent.
			bool BraceBlocks = false;
		};
		void SetLanguage(LanguageRules rules) { m_Language = std::move(rules); }
		const LanguageRules& Language() const { return m_Language; }

		// Replaces everything; clears undo, folds and carets (one at 0,0); clean.
		void SetText(std::string_view text);
		const TextDocument& Doc() const { return m_Doc; }
		std::string GetText() const { return m_Doc.GetText(); }

		bool IsDirty() const;
		void MarkClean();

		// ---- carets ----------------------------------------------------
		const std::vector<Caret>& Carets() const { return m_Carets; }
		const Caret& PrimaryCaret() const { return m_Carets[m_Primary]; }
		void MoveCarets(CaretMove move, bool extend, int pageLines = 20);
		void SetCaret(TextPos pos, bool extend);
		void AddCaret(TextPos pos);
		void CollapseCarets(); // one caret (the primary), no selection
		void SelectAll();
		void SelectWordAt(TextPos pos);
		void SelectLine(int line, bool extend);
		// Ctrl+D: select the word at the primary caret, else add a caret on
		// the next occurrence of the primary selection.
		void AddNextOccurrence();

		// ---- editing (each one undoable step) ---------------------------
		void InsertText(std::string_view text); // paste: N lines onto N carets go one each
		void TypeCodePoint(uint32_t codePoint); // bracket/quote pairing, wrapping a selection
		void InsertNewline();                   // auto-indent
		void Backspace(bool word);
		void Delete(bool word);
		void Indent();  // Tab: lines when a selection spans lines, else a tab
		void Outdent(); // Shift+Tab: the carets' lines
		void ToggleComment(std::string_view prefix = {}); // empty: the language's line comment
		void DuplicateLines();
		void MoveLines(int direction); // -1 up, +1 down
		std::string CopySelection() const; // no selection: the carets' whole lines
		std::string Cut();
		bool Undo();
		bool Redo();
		void BreakUndoGroup() { m_CoalesceOpen = false; }
		// An accepted completion - `range` becomes
		// `text` in one step, leaving one caret `caretOffset` bytes into the
		// inserted text (-1 = its end).
		void ReplaceRange(const TextRange& range, std::string_view text, int caretOffset = -1);

		// ---- find / replace / go to ------------------------------------
		// wholeWord: no word character may touch the match.
		bool FindNext(std::string_view needle, bool matchCase, bool backwards = false, bool wholeWord = false);
		bool ReplaceNext(std::string_view needle, std::string_view replacement, bool matchCase, bool wholeWord = false);
		int ReplaceAll(std::string_view needle, std::string_view replacement, bool matchCase, bool wholeWord = false);
		int CountMatches(std::string_view needle, bool matchCase, bool wholeWord = false) const;
		// The 1-based position of the primary selection among the matches
		// (the find bar's "3 of 22"); 0 when it is not on one.
		int MatchIndex(std::string_view needle, bool matchCase, bool wholeWord = false) const;
		void GoToLine(int line);

		// ---- folding (by indentation) ------------------------------------
		const std::vector<FoldRange>& FoldRanges() const;
		const FoldRange* FoldStartingAt(int line) const;
		bool IsFolded(int startLine) const { return m_Folded.count(startLine) != 0; }
		void ToggleFold(int startLine);
		void UnfoldAll() { m_Folded.clear(); m_VisibleDirty = true; }
		bool IsHidden(int line) const;
		int VisibleLineCount() const;
		int LineForVisible(int visibleIndex) const;
		int VisibleForLine(int line) const;

		// ---- columns ------------------------------------------------------
		static int VisualColumn(const std::string& line, int byteCol);
		static int ByteForVisualColumn(const std::string& line, int visualCol);
		static int IndentWidth(const std::string& line); // -1 for a blank line

		// The bracket pairing the primary caret touches ({ -1 } lines when none).
		bool MatchingBracket(TextPos& open, TextPos& close) const;

	private:
		struct Edit
		{
			TextPos Start;
			std::string Removed;
			std::string Inserted;
		};
		enum class Kind : uint8_t { Other, Typing, Deleting };
		struct Step
		{
			std::vector<Edit> Edits;
			std::vector<Caret> Before;
			std::vector<Caret> After;
			Kind Type = Kind::Other;
		};

		// One undoable step: Begin, Apply (any number), Commit.
		void Begin(Kind kind);
		TextPos Apply(const TextRange& range, std::string_view text);
		void Commit();
		// Per-caret replacement, bottom-up, the carets placed at each end.
		void ReplaceEachCaret(const std::vector<std::string>& texts, const std::vector<TextRange>& ranges, bool selectInserted = false);

		void Normalize(); // sort, merge overlaps, keep the primary, unfold carets' lines
		void UnfoldContaining(int line);
		std::vector<int> CaretLines() const; // the distinct lines the carets cover, ascending
		TextPos MoveOne(TextPos pos, CaretMove move, int& preferred, int pageLines) const;
		bool FindFrom(TextPos from, std::string_view needle, bool matchCase, bool backwards, TextRange& out, bool wholeWord = false) const;
		// An edit at (line, col) changed the line count by lineDelta.
		void ShiftFolds(int line, int col, int lineDelta);

		TextDocument m_Doc;
		LanguageRules m_Language;
		std::vector<Caret> m_Carets{ Caret{} };
		size_t m_Primary = 0;

		std::vector<Step> m_Undo;
		std::vector<Step> m_Redo;
		Step m_Pending;
		bool m_InStep = false;
		bool m_CoalesceOpen = false;
		double m_LastEditTime = 0.0;
		long long m_CleanDepth = 0; // m_Undo.size() when clean; -1 = unreachable

		std::set<int> m_Folded; // fold start lines
		mutable std::vector<FoldRange> m_Folds;
		mutable uint64_t m_FoldsVersion = 0;
		mutable std::vector<int> m_VisibleLines;
		mutable bool m_VisibleDirty = true;
		mutable uint64_t m_VisibleVersion = 0;
	};

} // namespace wisp::code
