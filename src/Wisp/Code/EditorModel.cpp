#include "Wisp/Code/EditorModel.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <climits>

namespace wisp::code {

	namespace {

		double Now()
		{
			using namespace std::chrono;
			return duration<double>(steady_clock::now().time_since_epoch()).count();
		}

		enum class CharClass : uint8_t { Space, Word, Punct };

		CharClass ClassOf(char c)
		{
			const unsigned char u = (unsigned char)c;
			if (c == ' ' || c == '\t')
				return CharClass::Space;
			if (std::isalnum(u) || c == '_' || u >= 0x80)
				return CharClass::Word;
			return CharClass::Punct;
		}

		std::string Utf8(uint32_t cp)
		{
			std::string s;
			if (cp < 0x80)
				s.push_back((char)cp);
			else if (cp < 0x800)
			{
				s.push_back((char)(0xC0 | (cp >> 6)));
				s.push_back((char)(0x80 | (cp & 0x3F)));
			}
			else if (cp < 0x10000)
			{
				s.push_back((char)(0xE0 | (cp >> 12)));
				s.push_back((char)(0x80 | ((cp >> 6) & 0x3F)));
				s.push_back((char)(0x80 | (cp & 0x3F)));
			}
			else
			{
				s.push_back((char)(0xF0 | (cp >> 18)));
				s.push_back((char)(0x80 | ((cp >> 12) & 0x3F)));
				s.push_back((char)(0x80 | ((cp >> 6) & 0x3F)));
				s.push_back((char)(0x80 | (cp & 0x3F)));
			}
			return s;
		}

		char CloserOf(char open)
		{
			switch (open)
			{
			case '(': return ')';
			case '[': return ']';
			case '{': return '}';
			case '"': return '"';
			case '\'': return '\'';
			default: return 0;
			}
		}

		bool EqualsAt(const std::string& line, size_t at, std::string_view needle, bool matchCase)
		{
			if (at + needle.size() > line.size())
				return false;
			for (size_t i = 0; i < needle.size(); i++)
			{
				const char a = line[at + i];
				const char b = needle[i];
				if (matchCase ? a != b : std::tolower((unsigned char)a) != std::tolower((unsigned char)b))
					return false;
			}
			return true;
		}

		size_t LeadingWhitespace(const std::string& line)
		{
			size_t n = 0;
			while (n < line.size() && (line[n] == ' ' || line[n] == '\t'))
				n++;
			return n;
		}

		// Where `p` lands after an edit that replaced [.., oldEnd) with text
		// ending at newEnd, for a `p` at or after oldEnd.
		TextPos Shift(TextPos p, TextPos oldEnd, TextPos newEnd)
		{
			if (p.Line == oldEnd.Line)
				return { newEnd.Line, newEnd.Col + (p.Col - oldEnd.Col) };
			return { p.Line + (newEnd.Line - oldEnd.Line), p.Col };
		}

		// Does the text before a newline open a block (one more indent)?
		bool OpensBlock(std::string_view before)
		{
			while (!before.empty() && (before.back() == ' ' || before.back() == '\t'))
				before.remove_suffix(1);
			if (before.empty())
				return false;
			const char last = before.back();
			if (last == '{' || last == '(' || last == '[')
				return true;
			for (std::string_view word : { "then", "do", "else", "repeat" })
				if (before.size() >= word.size() && before.substr(before.size() - word.size()) == word &&
					(before.size() == word.size() || ClassOf(before[before.size() - word.size() - 1]) != CharClass::Word))
					return true;
			// `function(...)` / `local function f(a)` - a header ending at its ')'.
			return last == ')' && before.find("function") != std::string_view::npos;
		}

	} // namespace

	// ---- state ------------------------------------------------------------

	void EditorModel::SetText(std::string_view text)
	{
		m_Doc.SetText(text);
		m_Carets = { Caret{} };
		m_Primary = 0;
		m_Undo.clear();
		m_Redo.clear();
		m_InStep = false;
		m_CoalesceOpen = false;
		m_CleanDepth = 0;
		m_Folded.clear();
		m_VisibleDirty = true;
	}

	bool EditorModel::IsDirty() const
	{
		return m_CleanDepth != (long long)m_Undo.size();
	}

	void EditorModel::MarkClean()
	{
		m_CleanDepth = (long long)m_Undo.size();
		m_CoalesceOpen = false;
	}

	// ---- steps --------------------------------------------------------------

	void EditorModel::Begin(Kind kind)
	{
		if (m_InStep)
			return;
		m_Pending = Step{};
		m_Pending.Before = m_Carets;
		m_Pending.Type = kind;
		m_InStep = true;
	}

	TextPos EditorModel::Apply(const TextRange& range, std::string_view text)
	{
		Edit edit;
		edit.Start = m_Doc.Clamp(range.Start);
		edit.Removed = m_Doc.GetRange(range);
		edit.Inserted = std::string(text);
		const int oldLines = m_Doc.Clamp(range.End).Line - edit.Start.Line;
		const TextPos end = m_Doc.Replace(range, text);
		ShiftFolds(edit.Start.Line, edit.Start.Col, (end.Line - edit.Start.Line) - oldLines);
		m_Pending.Edits.push_back(std::move(edit));
		return end;
	}

	void EditorModel::Commit()
	{
		m_InStep = false;
		Normalize();
		m_Pending.After = m_Carets;
		if (m_Pending.Edits.empty())
			return;
		const Kind kind = m_Pending.Type;
		const double now = Now();
		const bool coalesce = m_Pending.Type != Kind::Other && m_CoalesceOpen && !m_Undo.empty() &&
		                      m_Undo.back().Type == m_Pending.Type && now - m_LastEditTime < 1.0 &&
		                      m_CleanDepth != (long long)m_Undo.size();
		if (coalesce)
		{
			Step& last = m_Undo.back();
			last.Edits.insert(last.Edits.end(), m_Pending.Edits.begin(), m_Pending.Edits.end());
			last.After = m_Pending.After;
		}
		else
		{
			if (m_CleanDepth > (long long)m_Undo.size())
				m_CleanDepth = -1; // the clean state was on the discarded redo branch
			m_Undo.push_back(std::move(m_Pending));
		}
		m_Redo.clear();
		m_CoalesceOpen = kind != Kind::Other;
		m_LastEditTime = now;
	}

	bool EditorModel::Undo()
	{
		if (m_Undo.empty())
			return false;
		Step step = std::move(m_Undo.back());
		m_Undo.pop_back();
		for (auto it = step.Edits.rbegin(); it != step.Edits.rend(); ++it)
		{
			const TextPos end = EndOfInsert(it->Start, it->Inserted);
			const TextPos after = m_Doc.Replace({ it->Start, end }, it->Removed);
			ShiftFolds(it->Start.Line, it->Start.Col, (after.Line - it->Start.Line) - (end.Line - it->Start.Line));
		}
		m_Carets = step.Before;
		m_Primary = std::min(m_Primary, m_Carets.size() - 1);
		m_Redo.push_back(std::move(step));
		m_CoalesceOpen = false;
		Normalize();
		return true;
	}

	bool EditorModel::Redo()
	{
		if (m_Redo.empty())
			return false;
		Step step = std::move(m_Redo.back());
		m_Redo.pop_back();
		for (const Edit& edit : step.Edits)
		{
			const TextPos end = EndOfInsert(edit.Start, edit.Removed);
			const TextPos after = m_Doc.Replace({ edit.Start, end }, edit.Inserted);
			ShiftFolds(edit.Start.Line, edit.Start.Col, (after.Line - edit.Start.Line) - (end.Line - edit.Start.Line));
		}
		m_Carets = step.After;
		m_Primary = std::min(m_Primary, m_Carets.size() - 1);
		m_Undo.push_back(std::move(step));
		m_CoalesceOpen = false;
		Normalize();
		return true;
	}

	void EditorModel::ReplaceEachCaret(const std::vector<std::string>& texts, const std::vector<TextRange>& ranges, bool selectInserted)
	{
		// Bottom-up: an edit never moves the text above it; each one shifts
		// the carets already placed below it.
		std::vector<Caret> placed(m_Carets.size());
		for (size_t i = ranges.size(); i-- > 0;)
		{
			const TextPos newEnd = Apply(ranges[i], texts[i]);
			for (size_t j = i + 1; j < placed.size(); j++)
			{
				placed[j].Pos = Shift(placed[j].Pos, ranges[i].End, newEnd);
				placed[j].Anchor = Shift(placed[j].Anchor, ranges[i].End, newEnd);
			}
			placed[i].Pos = newEnd;
			placed[i].Anchor = selectInserted ? ranges[i].Start : newEnd;
		}
		m_Carets = std::move(placed);
	}

	// ---- carets -------------------------------------------------------------

	void EditorModel::Normalize()
	{
		const TextPos primaryPos = m_Carets[std::min(m_Primary, m_Carets.size() - 1)].Pos;
		for (Caret& caret : m_Carets)
		{
			caret.Pos = m_Doc.Clamp(caret.Pos);
			caret.Anchor = m_Doc.Clamp(caret.Anchor);
		}
		std::sort(m_Carets.begin(), m_Carets.end(), [](const Caret& a, const Caret& b) {
			return a.Selection().Start < b.Selection().Start;
		});
		std::vector<Caret> merged;
		for (const Caret& caret : m_Carets)
		{
			if (!merged.empty())
			{
				Caret& last = merged.back();
				const TextRange a = last.Selection();
				const TextRange b = caret.Selection();
				if (b.Start < a.End || b.Start == a.Start || (a.Empty() && b.Empty() && a.Start == b.Start))
				{
					const bool forward = last.Pos >= last.Anchor;
					const TextPos lo = std::min(a.Start, b.Start);
					const TextPos hi = std::max(a.End, b.End);
					last.Anchor = forward ? lo : hi;
					last.Pos = forward ? hi : lo;
					continue;
				}
			}
			merged.push_back(caret);
		}
		m_Carets = std::move(merged);
		m_Primary = m_Carets.size() - 1;
		for (size_t i = 0; i < m_Carets.size(); i++)
		{
			const TextRange range = m_Carets[i].Selection();
			if (primaryPos >= range.Start && primaryPos <= range.End)
			{
				m_Primary = i;
				break;
			}
		}
		for (const Caret& caret : m_Carets)
			UnfoldContaining(caret.Pos.Line);
	}

	TextPos EditorModel::MoveOne(TextPos pos, CaretMove move, int& preferred, int pageLines) const
	{
		const std::string& line = m_Doc.Line(pos.Line);
		auto prevVisible = [&](int l) {
			int v = VisibleForLine(l);
			return v > 0 ? LineForVisible(v - 1) : -1;
		};
		auto nextVisible = [&](int l) {
			int v = VisibleForLine(l);
			return v + 1 < VisibleLineCount() ? LineForVisible(v + 1) : -1;
		};
		auto vertical = [&](int target) {
			if (preferred < 0)
				preferred = VisualColumn(line, pos.Col);
			return TextPos{ target, ByteForVisualColumn(m_Doc.Line(target), preferred) };
		};
		switch (move)
		{
		case CaretMove::Left:
			preferred = -1;
			if (pos.Col > 0)
				return { pos.Line, pos.Col - PrevCodePointLength(line, pos.Col) };
			if (const int up = prevVisible(pos.Line); up >= 0)
				return { up, (int)m_Doc.Line(up).size() };
			return pos;
		case CaretMove::Right:
			preferred = -1;
			if (pos.Col < (int)line.size())
				return { pos.Line, pos.Col + NextCodePointLength(line, pos.Col) };
			if (const int down = nextVisible(pos.Line); down >= 0)
				return { down, 0 };
			return pos;
		case CaretMove::WordLeft:
		{
			preferred = -1;
			if (pos.Col == 0)
				return MoveOne(pos, CaretMove::Left, preferred, pageLines);
			int col = pos.Col;
			while (col > 0 && ClassOf(line[(size_t)col - 1]) == CharClass::Space)
				col--;
			if (col > 0)
			{
				const CharClass cls = ClassOf(line[(size_t)col - 1]);
				while (col > 0 && ClassOf(line[(size_t)col - 1]) == cls)
					col--;
			}
			return { pos.Line, col };
		}
		case CaretMove::WordRight:
		{
			preferred = -1;
			if (pos.Col >= (int)line.size())
				return MoveOne(pos, CaretMove::Right, preferred, pageLines);
			int col = pos.Col;
			const CharClass cls = ClassOf(line[(size_t)col]);
			while (col < (int)line.size() && ClassOf(line[(size_t)col]) == cls)
				col++;
			while (col < (int)line.size() && ClassOf(line[(size_t)col]) == CharClass::Space)
				col++;
			return { pos.Line, col };
		}
		case CaretMove::Up:
			if (const int up = prevVisible(pos.Line); up >= 0)
				return vertical(up);
			preferred = -1;
			return { pos.Line, 0 };
		case CaretMove::Down:
			if (const int down = nextVisible(pos.Line); down >= 0)
				return vertical(down);
			preferred = -1;
			return { pos.Line, (int)line.size() };
		case CaretMove::PageUp:
		case CaretMove::PageDown:
		{
			const int step = move == CaretMove::PageUp ? -pageLines : pageLines;
			const int target = std::clamp(VisibleForLine(pos.Line) + step, 0, VisibleLineCount() - 1);
			return vertical(LineForVisible(target));
		}
		case CaretMove::LineStart:
		{
			// Smart home: the first non-blank, then the column 0.
			preferred = -1;
			const int indent = (int)LeadingWhitespace(line);
			return { pos.Line, pos.Col == indent ? 0 : indent };
		}
		case CaretMove::LineEnd:
			preferred = -1;
			return { pos.Line, (int)line.size() };
		case CaretMove::DocStart:
			preferred = -1;
			return { 0, 0 };
		case CaretMove::DocEnd:
			preferred = -1;
			return m_Doc.End();
		}
		return pos;
	}

	void EditorModel::MoveCarets(CaretMove move, bool extend, int pageLines)
	{
		for (Caret& caret : m_Carets)
		{
			if (!extend && caret.HasSelection() && (move == CaretMove::Left || move == CaretMove::Right))
			{
				const TextRange range = caret.Selection();
				caret.Pos = caret.Anchor = move == CaretMove::Left ? range.Start : range.End;
				caret.PreferredVisualCol = -1;
				continue;
			}
			caret.Pos = MoveOne(caret.Pos, move, caret.PreferredVisualCol, pageLines);
			if (!extend)
				caret.Anchor = caret.Pos;
		}
		m_CoalesceOpen = false;
		Normalize();
	}

	void EditorModel::SetCaret(TextPos pos, bool extend)
	{
		Caret caret;
		caret.Pos = m_Doc.Clamp(pos);
		caret.Anchor = extend ? PrimaryCaret().Anchor : caret.Pos;
		m_Carets = { caret };
		m_Primary = 0;
		m_CoalesceOpen = false;
		Normalize();
	}

	void EditorModel::AddCaret(TextPos pos)
	{
		Caret caret;
		caret.Pos = caret.Anchor = m_Doc.Clamp(pos);
		m_Carets.push_back(caret);
		m_Primary = m_Carets.size() - 1;
		Normalize();
	}

	void EditorModel::CollapseCarets()
	{
		Caret caret = PrimaryCaret();
		caret.Anchor = caret.Pos;
		m_Carets = { caret };
		m_Primary = 0;
	}

	void EditorModel::SelectAll()
	{
		Caret caret;
		caret.Anchor = { 0, 0 };
		caret.Pos = m_Doc.End();
		m_Carets = { caret };
		m_Primary = 0;
	}

	void EditorModel::SelectWordAt(TextPos pos)
	{
		pos = m_Doc.Clamp(pos);
		const std::string& line = m_Doc.Line(pos.Line);
		int start = pos.Col, end = pos.Col;
		if (!line.empty())
		{
			const int probe = pos.Col < (int)line.size() ? pos.Col : pos.Col - 1;
			const CharClass cls = ClassOf(line[(size_t)probe]);
			start = end = probe;
			while (start > 0 && ClassOf(line[(size_t)start - 1]) == cls)
				start--;
			while (end < (int)line.size() && ClassOf(line[(size_t)end]) == cls)
				end++;
		}
		Caret caret;
		caret.Anchor = { pos.Line, start };
		caret.Pos = { pos.Line, end };
		m_Carets = { caret };
		m_Primary = 0;
	}

	void EditorModel::SelectLine(int line, bool extend)
	{
		line = std::clamp(line, 0, m_Doc.LineCount() - 1);
		const TextPos start{ line, 0 };
		const TextPos end = line + 1 < m_Doc.LineCount() ? TextPos{ line + 1, 0 } : TextPos{ line, (int)m_Doc.Line(line).size() };
		Caret caret;
		if (extend)
		{
			const TextPos anchor = PrimaryCaret().Anchor;
			caret.Anchor = anchor;
			caret.Pos = anchor <= start ? end : start;
		}
		else
		{
			caret.Anchor = start;
			caret.Pos = end;
		}
		m_Carets = { caret };
		m_Primary = 0;
		Normalize();
	}

	void EditorModel::AddNextOccurrence()
	{
		const Caret primary = PrimaryCaret();
		if (!primary.HasSelection())
		{
			if (m_Carets.size() == 1)
				SelectWordAt(primary.Pos);
			return;
		}
		const TextRange selection = primary.Selection();
		if (selection.Start.Line != selection.End.Line)
			return;
		const std::string needle = m_Doc.GetRange(selection);
		TextPos from = m_Carets.back().Selection().End;
		for (size_t attempt = 0; attempt < m_Carets.size() + 1; attempt++)
		{
			TextRange found;
			if (!FindFrom(from, needle, true, false, found))
				return;
			const bool taken = std::any_of(m_Carets.begin(), m_Carets.end(), [&](const Caret& c) { return c.Selection().Start == found.Start; });
			if (!taken)
			{
				Caret caret;
				caret.Anchor = found.Start;
				caret.Pos = found.End;
				m_Carets.push_back(caret);
				m_Primary = m_Carets.size() - 1;
				Normalize();
				return;
			}
			from = found.End;
		}
	}

	// ---- editing ------------------------------------------------------------

	void EditorModel::InsertText(std::string_view text)
	{
		std::string clean;
		clean.reserve(text.size());
		for (char c : text)
			if (c != '\r')
				clean.push_back(c);
		std::vector<TextRange> ranges;
		for (const Caret& caret : m_Carets)
			ranges.push_back(caret.Selection());
		std::vector<std::string> texts(m_Carets.size(), clean);
		// A paste of exactly one line per caret goes one line each.
		if (m_Carets.size() > 1)
		{
			std::string_view body = clean;
			if (!body.empty() && body.back() == '\n')
				body.remove_suffix(1);
			std::vector<std::string> lines;
			size_t start = 0;
			for (size_t i = 0; i <= body.size(); i++)
				if (i == body.size() || body[i] == '\n')
				{
					lines.emplace_back(body.substr(start, i - start));
					start = i + 1;
				}
			if (lines.size() == m_Carets.size())
				texts = std::move(lines);
		}
		const bool typing = clean.size() <= 4 && clean.find('\n') == std::string::npos && !clean.empty();
		Begin(typing ? Kind::Typing : Kind::Other);
		ReplaceEachCaret(texts, ranges);
		Commit();
	}

	void EditorModel::TypeCodePoint(uint32_t codePoint)
	{
		if (codePoint < 32 && codePoint != '\t')
			return;
		const std::string text = Utf8(codePoint);
		const char c = codePoint < 0x80 ? (char)codePoint : 0;
		const bool anySelection = std::any_of(m_Carets.begin(), m_Carets.end(), [](const Caret& k) { return k.HasSelection(); });

		// Typing a closer over the same closer just steps over it.
		if ((c == ')' || c == ']' || c == '}' || c == '"' || c == '\'') && !anySelection &&
			std::all_of(m_Carets.begin(), m_Carets.end(), [&](const Caret& k) {
				const std::string& line = m_Doc.Line(k.Pos.Line);
				return k.Pos.Col < (int)line.size() && line[(size_t)k.Pos.Col] == c;
			}))
		{
			for (Caret& caret : m_Carets)
				caret.Pos = caret.Anchor = { caret.Pos.Line, caret.Pos.Col + 1 };
			m_CoalesceOpen = false;
			return;
		}

		const char closer = CloserOf(c);
		if (closer && anySelection)
		{
			// Wrap every selection, keeping the inner text selected.
			std::vector<TextRange> ranges;
			std::vector<std::string> texts;
			for (const Caret& caret : m_Carets)
			{
				ranges.push_back(caret.Selection());
				texts.push_back(std::string(1, c) + m_Doc.GetRange(caret.Selection()) + std::string(1, closer));
			}
			Begin(Kind::Other);
			ReplaceEachCaret(texts, ranges, true);
			for (Caret& caret : m_Carets)
			{
				caret.Anchor = { caret.Anchor.Line, caret.Anchor.Col + 1 };
				caret.Pos = { caret.Pos.Line, caret.Pos.Col - 1 };
			}
			Commit();
			return;
		}

		if (closer && !anySelection &&
			std::all_of(m_Carets.begin(), m_Carets.end(), [&](const Caret& k) {
				const std::string& line = m_Doc.Line(k.Pos.Line);
				const bool nextFree = k.Pos.Col >= (int)line.size() || ClassOf(line[(size_t)k.Pos.Col]) == CharClass::Space ||
				                      std::string_view(")]},;").find(line[(size_t)k.Pos.Col]) != std::string_view::npos;
				// A quote after a word is an apostrophe / a closing quote.
				const bool prevFree = (c != '"' && c != '\'') || k.Pos.Col == 0 || ClassOf(line[(size_t)k.Pos.Col - 1]) != CharClass::Word;
				return nextFree && prevFree;
			}))
		{
			const std::string pair = std::string(1, c) + std::string(1, closer);
			std::vector<TextRange> ranges;
			for (const Caret& caret : m_Carets)
				ranges.push_back(caret.Selection());
			Begin(Kind::Typing);
			ReplaceEachCaret(std::vector<std::string>(m_Carets.size(), pair), ranges);
			for (Caret& caret : m_Carets)
				caret.Pos = caret.Anchor = { caret.Pos.Line, caret.Pos.Col - 1 };
			Commit();
			return;
		}
		InsertText(text);
	}

	void EditorModel::InsertNewline()
	{
		std::vector<TextRange> ranges;
		std::vector<std::string> texts;
		std::vector<int> caretLineOffset; // the caret's line within the inserted text
		std::vector<int> caretCol;
		for (const Caret& caret : m_Carets)
		{
			const TextRange range = caret.Selection();
			const std::string& line = m_Doc.Line(range.Start.Line);
			const size_t indentLength = std::min(LeadingWhitespace(line), (size_t)range.Start.Col);
			std::string indent = line.substr(0, indentLength);
			const std::string_view before = std::string_view(line).substr(0, (size_t)range.Start.Col);
			const std::string& endLine = m_Doc.Line(range.End.Line);
			const char after = range.End.Col < (int)endLine.size() ? endLine[(size_t)range.End.Col] : 0;
			const char beforeChar = range.Start.Col > 0 ? line[(size_t)range.Start.Col - 1] : 0;
			ranges.push_back(range);
			if (beforeChar && after && CloserOf(beforeChar) == after && after != '"' && after != '\'')
			{
				// Between a pair: the closer drops to its own line, the caret
				// sits indented in between.
				texts.push_back("\n" + indent + "\t\n" + indent);
				caretLineOffset.push_back(1);
				caretCol.push_back((int)indent.size() + 1);
				continue;
			}
			if (OpensBlock(before))
				indent.push_back('\t');
			texts.push_back("\n" + indent);
			caretLineOffset.push_back(-1);
			caretCol.push_back(0);
		}
		Begin(Kind::Other);
		ReplaceEachCaret(texts, ranges);
		for (size_t i = 0; i < m_Carets.size() && i < caretLineOffset.size(); i++)
			if (caretLineOffset[i] >= 0)
			{
				// The caret ended after the closer's indent: one line up, in the middle.
				m_Carets[i].Pos = m_Carets[i].Anchor = { m_Carets[i].Pos.Line - 1, caretCol[i] };
			}
		Commit();
	}

	void EditorModel::Backspace(bool word)
	{
		std::vector<TextRange> ranges;
		for (const Caret& caret : m_Carets)
		{
			if (caret.HasSelection())
			{
				ranges.push_back(caret.Selection());
				continue;
			}
			const TextPos pos = caret.Pos;
			const std::string& line = m_Doc.Line(pos.Line);
			if (word)
			{
				int preferred = -1;
				ranges.push_back(TextRange::Of(MoveOne(pos, CaretMove::WordLeft, preferred, 0), pos));
				continue;
			}
			if (pos.Col == 0)
			{
				ranges.push_back(pos.Line > 0 ? TextRange{ { pos.Line - 1, (int)m_Doc.Line(pos.Line - 1).size() }, pos } : TextRange{ pos, pos });
				continue;
			}
			const char before = line[(size_t)pos.Col - 1];
			const bool pair = CloserOf(before) && pos.Col < (int)line.size() && line[(size_t)pos.Col] == CloserOf(before);
			ranges.push_back({ { pos.Line, pos.Col - PrevCodePointLength(line, pos.Col) }, { pos.Line, pos.Col + (pair ? 1 : 0) } });
		}
		Begin(Kind::Deleting);
		ReplaceEachCaret(std::vector<std::string>(ranges.size()), ranges);
		Commit();
	}

	void EditorModel::Delete(bool word)
	{
		std::vector<TextRange> ranges;
		for (const Caret& caret : m_Carets)
		{
			if (caret.HasSelection())
			{
				ranges.push_back(caret.Selection());
				continue;
			}
			const TextPos pos = caret.Pos;
			const std::string& line = m_Doc.Line(pos.Line);
			if (word)
			{
				int preferred = -1;
				ranges.push_back(TextRange::Of(pos, MoveOne(pos, CaretMove::WordRight, preferred, 0)));
				continue;
			}
			if (pos.Col >= (int)line.size())
			{
				ranges.push_back(pos.Line + 1 < m_Doc.LineCount() ? TextRange{ pos, { pos.Line + 1, 0 } } : TextRange{ pos, pos });
				continue;
			}
			ranges.push_back({ pos, { pos.Line, pos.Col + NextCodePointLength(line, pos.Col) } });
		}
		Begin(Kind::Deleting);
		ReplaceEachCaret(std::vector<std::string>(ranges.size()), ranges);
		Commit();
	}

	std::vector<int> EditorModel::CaretLines() const
	{
		std::set<int> lines;
		for (const Caret& caret : m_Carets)
		{
			const TextRange range = caret.Selection();
			int last = range.End.Line;
			if (range.End.Line > range.Start.Line && range.End.Col == 0)
				last--; // a selection ending at a line start leaves that line out
			for (int line = range.Start.Line; line <= last; line++)
				lines.insert(line);
		}
		return std::vector<int>(lines.begin(), lines.end());
	}

	namespace {

		// Line-local edits (no line count change), applied bottom-up with the
		// carets on each line shifted.
		struct LineEdit
		{
			int Line;
			int Col;
			int RemoveLength;
			std::string Insert;
		};

	}

	void EditorModel::Indent()
	{
		const bool multiLine = std::any_of(m_Carets.begin(), m_Carets.end(), [](const Caret& k) {
			return k.Selection().Start.Line != k.Selection().End.Line;
		});
		if (!multiLine)
		{
			InsertText("\t");
			return;
		}
		Begin(Kind::Other);
		std::vector<int> lines = CaretLines();
		for (auto it = lines.rbegin(); it != lines.rend(); ++it)
		{
			if (m_Doc.Line(*it).empty())
				continue;
			Apply({ { *it, 0 }, { *it, 0 } }, "\t");
			for (Caret& caret : m_Carets)
			{
				if (caret.Pos.Line == *it)
					caret.Pos.Col++;
				if (caret.Anchor.Line == *it && (caret.Anchor.Col > 0 || caret.Anchor > caret.Pos))
					caret.Anchor.Col++;
			}
		}
		Commit();
	}

	void EditorModel::Outdent()
	{
		Begin(Kind::Other);
		std::vector<int> lines = CaretLines();
		for (auto it = lines.rbegin(); it != lines.rend(); ++it)
		{
			const std::string& line = m_Doc.Line(*it);
			int remove = 0;
			if (!line.empty() && line[0] == '\t')
				remove = 1;
			else
				while (remove < kTabWidth && remove < (int)line.size() && line[(size_t)remove] == ' ')
					remove++;
			if (remove == 0)
				continue;
			Apply({ { *it, 0 }, { *it, remove } }, "");
			for (Caret& caret : m_Carets)
			{
				if (caret.Pos.Line == *it)
					caret.Pos.Col = std::max(0, caret.Pos.Col - remove);
				if (caret.Anchor.Line == *it)
					caret.Anchor.Col = std::max(0, caret.Anchor.Col - remove);
			}
		}
		Commit();
	}

	void EditorModel::ToggleComment(std::string_view prefix)
	{
		const std::vector<int> lines = CaretLines();
		bool allCommented = true;
		size_t minIndent = SIZE_MAX;
		for (int index : lines)
		{
			const std::string& line = m_Doc.Line(index);
			const size_t indent = LeadingWhitespace(line);
			if (indent == line.size())
				continue;
			minIndent = std::min(minIndent, indent);
			if (line.compare(indent, prefix.size(), prefix) != 0)
				allCommented = false;
		}
		if (minIndent == SIZE_MAX)
			return; // only blank lines
		Begin(Kind::Other);
		for (auto it = lines.rbegin(); it != lines.rend(); ++it)
		{
			const std::string& line = m_Doc.Line(*it);
			const size_t indent = LeadingWhitespace(line);
			if (indent == line.size())
				continue;
			int col = 0, delta = 0;
			if (allCommented)
			{
				int remove = (int)prefix.size();
				if (indent + prefix.size() < line.size() && line[indent + prefix.size()] == ' ')
					remove++;
				col = (int)indent;
				Apply({ { *it, col }, { *it, col + remove } }, "");
				delta = -remove;
			}
			else
			{
				col = (int)minIndent;
				const std::string insert = std::string(prefix) + " ";
				Apply({ { *it, col }, { *it, col } }, insert);
				delta = (int)insert.size();
			}
			for (Caret& caret : m_Carets)
			{
				if (caret.Pos.Line == *it && caret.Pos.Col >= col)
					caret.Pos.Col = std::max(col, caret.Pos.Col + delta);
				if (caret.Anchor.Line == *it && caret.Anchor.Col >= col)
					caret.Anchor.Col = std::max(col, caret.Anchor.Col + delta);
			}
		}
		Commit();
	}

	void EditorModel::DuplicateLines()
	{
		// Contiguous runs of caret lines, each copied below itself.
		const std::vector<int> lines = CaretLines();
		std::vector<std::pair<int, int>> blocks;
		for (int line : lines)
		{
			if (!blocks.empty() && blocks.back().second + 1 == line)
				blocks.back().second = line;
			else
				blocks.emplace_back(line, line);
		}
		Begin(Kind::Other);
		for (auto it = blocks.rbegin(); it != blocks.rend(); ++it)
		{
			const auto [first, last] = *it;
			const std::string text = m_Doc.GetRange({ { first, 0 }, { last, (int)m_Doc.Line(last).size() } });
			const TextPos at{ last, (int)m_Doc.Line(last).size() };
			Apply({ at, at }, "\n" + text);
			const int size = last - first + 1;
			for (Caret& caret : m_Carets)
			{
				if (caret.Pos.Line >= first)
					caret.Pos.Line += size;
				if (caret.Anchor.Line >= first)
					caret.Anchor.Line += size;
			}
		}
		Commit();
	}

	void EditorModel::MoveLines(int direction)
	{
		const std::vector<int> lines = CaretLines();
		if (lines.empty())
			return;
		std::vector<std::pair<int, int>> blocks;
		for (int line : lines)
		{
			if (!blocks.empty() && blocks.back().second + 1 == line)
				blocks.back().second = line;
			else
				blocks.emplace_back(line, line);
		}
		if ((direction < 0 && blocks.front().first == 0) || (direction > 0 && blocks.back().second + 1 >= m_Doc.LineCount()))
			return;
		Begin(Kind::Other);
		auto moveBlock = [&](int first, int last) {
			const std::string block = m_Doc.GetRange({ { first, 0 }, { last, (int)m_Doc.Line(last).size() } });
			if (direction < 0)
			{
				const std::string above = m_Doc.Line(first - 1);
				Apply({ { first - 1, 0 }, { last, (int)m_Doc.Line(last).size() } }, block + "\n" + above);
			}
			else
			{
				const std::string below = m_Doc.Line(last + 1);
				Apply({ { first, 0 }, { last + 1, (int)below.size() } }, below + "\n" + block);
			}
			for (Caret& caret : m_Carets)
			{
				if (caret.Pos.Line >= first && caret.Pos.Line <= last)
					caret.Pos.Line += direction;
				if (caret.Anchor.Line >= first && caret.Anchor.Line <= last)
					caret.Anchor.Line += direction;
			}
		};
		if (direction < 0)
			for (const auto& [first, last] : blocks)
				moveBlock(first, last);
		else
			for (auto it = blocks.rbegin(); it != blocks.rend(); ++it)
				moveBlock(it->first, it->second);
		Commit();
	}

	std::string EditorModel::CopySelection() const
	{
		const bool anySelection = std::any_of(m_Carets.begin(), m_Carets.end(), [](const Caret& k) { return k.HasSelection(); });
		std::string text;
		if (anySelection)
		{
			for (size_t i = 0; i < m_Carets.size(); i++)
			{
				if (!m_Carets[i].HasSelection())
					continue;
				if (!text.empty())
					text.push_back('\n');
				text += m_Doc.GetRange(m_Carets[i].Selection());
			}
			return text;
		}
		for (int line : CaretLines())
			text += m_Doc.Line(line) + "\n";
		return text;
	}

	std::string EditorModel::Cut()
	{
		const std::string text = CopySelection();
		const bool anySelection = std::any_of(m_Carets.begin(), m_Carets.end(), [](const Caret& k) { return k.HasSelection(); });
		if (anySelection)
		{
			std::vector<TextRange> ranges;
			for (const Caret& caret : m_Carets)
				ranges.push_back(caret.Selection());
			Begin(Kind::Other);
			ReplaceEachCaret(std::vector<std::string>(ranges.size()), ranges);
			Commit();
			return text;
		}
		// Whole lines, bottom-up.
		const std::vector<int> lines = CaretLines();
		Begin(Kind::Other);
		for (auto it = lines.rbegin(); it != lines.rend(); ++it)
		{
			if (*it + 1 < m_Doc.LineCount())
				Apply({ { *it, 0 }, { *it + 1, 0 } }, "");
			else if (*it > 0)
				Apply({ { *it - 1, (int)m_Doc.Line(*it - 1).size() }, { *it, (int)m_Doc.Line(*it).size() } }, "");
			else
				Apply({ { 0, 0 }, { 0, (int)m_Doc.Line(0).size() } }, "");
		}
		const int line = std::min(lines.front(), m_Doc.LineCount() - 1);
		m_Carets = { Caret{ { line, 0 }, { line, 0 }, -1 } };
		m_Primary = 0;
		Commit();
		return text;
	}

	// ---- find -----------------------------------------------------------------

	bool EditorModel::FindFrom(TextPos from, std::string_view needle, bool matchCase, bool backwards, TextRange& out) const
	{
		if (needle.empty() || needle.find('\n') != std::string_view::npos)
			return false;
		const int lines = m_Doc.LineCount();
		from = m_Doc.Clamp(from);
		for (int step = 0; step <= lines; step++)
		{
			const int index = backwards ? ((from.Line - step) % lines + lines) % lines : (from.Line + step) % lines;
			const std::string& line = m_Doc.Line(index);
			if (!backwards)
			{
				const size_t start = step == 0 ? (size_t)from.Col : 0;
				for (size_t at = start; at + needle.size() <= line.size(); at++)
					if (EqualsAt(line, at, needle, matchCase))
					{
						out = { { index, (int)at }, { index, (int)(at + needle.size()) } };
						return true;
					}
				if (step == lines) // wrapped back to the start line: its head
					break;
			}
			else
			{
				long long at = step == 0 ? (long long)from.Col - (long long)needle.size() : (long long)line.size() - (long long)needle.size();
				for (; at >= 0; at--)
					if (EqualsAt(line, (size_t)at, needle, matchCase))
					{
						out = { { index, (int)at }, { index, (int)at + (int)needle.size() } };
						return true;
					}
			}
		}
		return false;
	}

	bool EditorModel::FindNext(std::string_view needle, bool matchCase, bool backwards)
	{
		const TextRange selection = PrimaryCaret().Selection();
		TextRange found;
		if (!FindFrom(backwards ? selection.Start : selection.End, needle, matchCase, backwards, found))
			return false;
		Caret caret;
		caret.Anchor = found.Start;
		caret.Pos = found.End;
		m_Carets = { caret };
		m_Primary = 0;
		Normalize();
		return true;
	}

	bool EditorModel::ReplaceNext(std::string_view needle, std::string_view replacement, bool matchCase)
	{
		const Caret primary = PrimaryCaret();
		const TextRange selection = primary.Selection();
		const std::string selected = m_Doc.GetRange(selection);
		if (selection.Start.Line == selection.End.Line && selected.size() == needle.size() && EqualsAt(selected, 0, needle, matchCase))
		{
			m_Carets = { primary };
			m_Primary = 0;
			Begin(Kind::Other);
			const TextPos end = Apply(selection, replacement);
			m_Carets = { Caret{ end, end, -1 } };
			Commit();
		}
		return FindNext(needle, matchCase);
	}

	int EditorModel::ReplaceAll(std::string_view needle, std::string_view replacement, bool matchCase)
	{
		if (needle.empty())
			return 0;
		std::vector<TextRange> matches;
		for (int index = 0; index < m_Doc.LineCount(); index++)
		{
			const std::string& line = m_Doc.Line(index);
			for (size_t at = 0; at + needle.size() <= line.size();)
			{
				if (EqualsAt(line, at, needle, matchCase))
				{
					matches.push_back({ { index, (int)at }, { index, (int)(at + needle.size()) } });
					at += needle.size();
				}
				else
					at++;
			}
		}
		if (matches.empty())
			return 0;
		Begin(Kind::Other);
		for (auto it = matches.rbegin(); it != matches.rend(); ++it)
			Apply(*it, replacement);
		const TextPos first = EndOfInsert(matches.front().Start, replacement);
		m_Carets = { Caret{ first, first, -1 } };
		m_Primary = 0;
		Commit();
		return (int)matches.size();
	}

	int EditorModel::CountMatches(std::string_view needle, bool matchCase) const
	{
		if (needle.empty() || needle.find('\n') != std::string_view::npos)
			return 0;
		int count = 0;
		for (int index = 0; index < m_Doc.LineCount(); index++)
		{
			const std::string& line = m_Doc.Line(index);
			for (size_t at = 0; at + needle.size() <= line.size();)
			{
				if (EqualsAt(line, at, needle, matchCase))
				{
					count++;
					at += needle.size();
				}
				else
					at++;
			}
		}
		return count;
	}

	void EditorModel::GoToLine(int line)
	{
		line = std::clamp(line, 0, m_Doc.LineCount() - 1);
		const int col = (int)LeadingWhitespace(m_Doc.Line(line));
		m_Carets = { Caret{ { line, col }, { line, col }, -1 } };
		m_Primary = 0;
		Normalize();
	}

	// ---- folding --------------------------------------------------------------

	int EditorModel::IndentWidth(const std::string& line)
	{
		int width = 0;
		for (char c : line)
		{
			if (c == ' ')
				width++;
			else if (c == '\t')
				width += kTabWidth - width % kTabWidth;
			else
				return width;
		}
		return -1;
	}

	const std::vector<FoldRange>& EditorModel::FoldRanges() const
	{
		if (m_FoldsVersion == m_Doc.Version())
			return m_Folds;
		m_FoldsVersion = m_Doc.Version();
		m_Folds.clear();
		// A line opens a fold when the next non-blank line is indented
		// deeper; the fold runs to the last deeper non-blank line (the
		// closing `end` / `}` stays visible). One pass with a stack.
		std::vector<std::pair<int, int>> open; // (line, indent)
		int previousNonBlank = -1;
		auto close = [&](int indent) {
			while (!open.empty() && open.back().second >= indent)
			{
				if (previousNonBlank > open.back().first)
					m_Folds.push_back({ open.back().first, previousNonBlank });
				open.pop_back();
			}
		};
		for (int index = 0; index < m_Doc.LineCount(); index++)
		{
			const int indent = IndentWidth(m_Doc.Line(index));
			if (indent < 0)
				continue;
			close(indent);
			open.emplace_back(index, indent);
			previousNonBlank = index;
		}
		close(INT_MIN);
		std::sort(m_Folds.begin(), m_Folds.end(), [](const FoldRange& a, const FoldRange& b) { return a.StartLine < b.StartLine; });
		return m_Folds;
	}

	const FoldRange* EditorModel::FoldStartingAt(int line) const
	{
		const std::vector<FoldRange>& folds = FoldRanges();
		auto it = std::lower_bound(folds.begin(), folds.end(), line, [](const FoldRange& f, int l) { return f.StartLine < l; });
		return it != folds.end() && it->StartLine == line ? &*it : nullptr;
	}

	void EditorModel::ToggleFold(int startLine)
	{
		const FoldRange* fold = FoldStartingAt(startLine);
		if (!fold)
			return;
		if (!m_Folded.erase(startLine))
		{
			m_Folded.insert(startLine);
			// Carets inside the fold move to its header.
			for (Caret& caret : m_Carets)
			{
				if (caret.Pos.Line > fold->StartLine && caret.Pos.Line <= fold->EndLine)
					caret.Pos = caret.Anchor = { fold->StartLine, (int)m_Doc.Line(fold->StartLine).size() };
				if (caret.Anchor.Line > fold->StartLine && caret.Anchor.Line <= fold->EndLine)
					caret.Anchor = caret.Pos;
			}
		}
		m_VisibleDirty = true;
	}

	void EditorModel::UnfoldContaining(int line)
	{
		bool changed = false;
		for (auto it = m_Folded.begin(); it != m_Folded.end();)
		{
			const FoldRange* fold = FoldStartingAt(*it);
			if (!fold)
			{
				it = m_Folded.erase(it);
				changed = true;
			}
			else if (line > fold->StartLine && line <= fold->EndLine)
			{
				it = m_Folded.erase(it);
				changed = true;
			}
			else
				++it;
		}
		if (changed)
			m_VisibleDirty = true;
	}

	void EditorModel::ShiftFolds(int line, int col, int lineDelta)
	{
		if (lineDelta == 0 || m_Folded.empty())
			return;
		std::set<int> shifted;
		for (int start : m_Folded)
		{
			// Lines inserted at the very start of a header push it down too.
			if (start < line || (start == line && !(col == 0 && lineDelta > 0)))
				shifted.insert(start);
			else if (lineDelta < 0 && start <= line - lineDelta)
				continue; // its header was deleted
			else
				shifted.insert(start + lineDelta);
		}
		m_Folded = std::move(shifted);
		m_VisibleDirty = true;
	}

	bool EditorModel::IsHidden(int line) const
	{
		return VisibleForLine(line) >= 0 && LineForVisible(VisibleForLine(line)) != line;
	}

	int EditorModel::VisibleLineCount() const
	{
		if (m_VisibleDirty || m_VisibleVersion != m_Doc.Version())
		{
			m_VisibleLines.clear();
			int hiddenUntil = -1;
			for (int index = 0; index < m_Doc.LineCount(); index++)
			{
				if (index <= hiddenUntil)
					continue;
				m_VisibleLines.push_back(index);
				if (m_Folded.count(index))
					if (const FoldRange* fold = FoldStartingAt(index))
						hiddenUntil = fold->EndLine;
			}
			m_VisibleDirty = false;
			m_VisibleVersion = m_Doc.Version();
		}
		return (int)m_VisibleLines.size();
	}

	int EditorModel::LineForVisible(int visibleIndex) const
	{
		const int count = VisibleLineCount();
		return m_VisibleLines[(size_t)std::clamp(visibleIndex, 0, count - 1)];
	}

	int EditorModel::VisibleForLine(int line) const
	{
		VisibleLineCount();
		auto it = std::upper_bound(m_VisibleLines.begin(), m_VisibleLines.end(), line);
		return std::max(0, (int)(it - m_VisibleLines.begin()) - 1);
	}

	// ---- columns ------------------------------------------------------------

	int EditorModel::VisualColumn(const std::string& line, int byteCol)
	{
		int visual = 0;
		for (int i = 0; i < byteCol && i < (int)line.size();)
		{
			if (line[(size_t)i] == '\t')
			{
				visual += kTabWidth - visual % kTabWidth;
				i++;
			}
			else
			{
				visual++;
				i += std::max(1, NextCodePointLength(line, i));
			}
		}
		return visual;
	}

	int EditorModel::ByteForVisualColumn(const std::string& line, int visualCol)
	{
		int visual = 0;
		int i = 0;
		while (i < (int)line.size())
		{
			const int width = line[(size_t)i] == '\t' ? kTabWidth - visual % kTabWidth : 1;
			if (visual + width > visualCol)
			{
				// The nearer edge of the character.
				if (visualCol - visual > width / 2)
					i += line[(size_t)i] == '\t' ? 1 : std::max(1, NextCodePointLength(line, i));
				return i;
			}
			visual += width;
			i += line[(size_t)i] == '\t' ? 1 : std::max(1, NextCodePointLength(line, i));
		}
		return (int)line.size();
	}

	bool EditorModel::MatchingBracket(TextPos& open, TextPos& close) const
	{
		const TextPos pos = PrimaryCaret().Pos;
		const std::string& line = m_Doc.Line(pos.Line);
		auto bracketAt = [&](int col) -> char {
			if (col < 0 || col >= (int)line.size())
				return 0;
			const char c = line[(size_t)col];
			return std::string_view("()[]{}").find(c) != std::string_view::npos ? c : 0;
		};
		int col = pos.Col;
		char c = bracketAt(col);
		if (!c)
		{
			col = pos.Col - 1;
			c = bracketAt(col);
		}
		if (!c)
			return false;
		const bool forward = c == '(' || c == '[' || c == '{';
		const char partner = forward ? CloserOf(c) : (c == ')' ? '(' : c == ']' ? '[' : '{');
		int depth = 0;
		int l = pos.Line, k = col;
		for (int scanned = 0; scanned < 200000; scanned++)
		{
			const std::string& text = m_Doc.Line(l);
			if (k >= 0 && k < (int)text.size())
			{
				const char here = text[(size_t)k];
				if (here == c)
					depth++;
				else if (here == partner && --depth == 0)
				{
					const TextPos a{ pos.Line, col };
					const TextPos b{ l, k };
					open = forward ? a : b;
					close = forward ? b : a;
					return true;
				}
			}
			if (forward)
			{
				if (++k >= (int)text.size())
				{
					if (++l >= m_Doc.LineCount())
						return false;
					k = -1;
				}
			}
			else if (--k < 0)
			{
				if (--l < 0)
					return false;
				k = (int)m_Doc.Line(l).size();
			}
		}
		return false;
	}

} // namespace wisp::code
