#pragma once

// The code editor's text - a vector of lines ('\n'-separated; a loaded
// "\r\n" becomes '\n'), edited through one primitive, Replace(range,
// text). Positions are (line, byte column) and columns sit on UTF-8 code
// point boundaries. A line vector, not a piece table: an edit touches one
// line (a newline moves the line vector), which edits a 10k-line file in
// microseconds and stays simple to get right.

#include <compare>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace wisp::code {

	struct TextPos
	{
		int Line = 0;
		int Col = 0; // byte offset within the line

		auto operator<=>(const TextPos&) const = default;
	};

	struct TextRange
	{
		TextPos Start;
		TextPos End; // exclusive; Start <= End

		bool Empty() const { return Start == End; }
		static TextRange Of(TextPos a, TextPos b) { return a <= b ? TextRange{ a, b } : TextRange{ b, a }; }
	};

	// Where `text` ends when inserted at `start`.
	TextPos EndOfInsert(TextPos start, std::string_view text);

	// UTF-8 code point lengths at / before a byte column (1 for a stray byte).
	int NextCodePointLength(const std::string& line, int col);
	int PrevCodePointLength(const std::string& line, int col);

	class TextDocument
	{
	public:
		explicit TextDocument(std::string_view text = {}) { SetText(text); }

		void SetText(std::string_view text);
		std::string GetText() const;

		int LineCount() const { return (int)m_Lines.size(); }
		const std::string& Line(int index) const { return m_Lines[(size_t)index]; }
		std::string GetRange(const TextRange& range) const;
		TextPos Clamp(TextPos pos) const;
		TextPos End() const { return { LineCount() - 1, (int)m_Lines.back().size() }; }

		// Replaces `range` with `text`; returns the end of the inserted text.
		TextPos Replace(const TextRange& range, std::string_view text);

		// Bumped by every change - caches (tokens, folds) key on it.
		uint64_t Version() const { return m_Version; }
		// The lowest line changed since the last call (INT32_MAX: none) - the
		// tokenizer's restart point.
		int TakeFirstChangedLine();

	private:
		std::vector<std::string> m_Lines{ std::string() };
		uint64_t m_Version = 1;
		int m_FirstChanged = 0;
	};

} // namespace wisp::code
