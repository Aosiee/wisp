#include "Wisp/Code/TextDocument.h"

#include <algorithm>
#include <climits>

namespace wisp::code {

	TextPos EndOfInsert(TextPos start, std::string_view text)
	{
		const size_t lastBreak = text.rfind('\n');
		if (lastBreak == std::string_view::npos)
			return { start.Line, start.Col + (int)text.size() };
		const int breaks = (int)std::count(text.begin(), text.end(), '\n');
		return { start.Line + breaks, (int)(text.size() - lastBreak - 1) };
	}

	int NextCodePointLength(const std::string& line, int col)
	{
		if (col >= (int)line.size())
			return 0;
		const unsigned char lead = (unsigned char)line[(size_t)col];
		int length = lead < 0x80 ? 1 : (lead >> 5) == 0x6 ? 2 : (lead >> 4) == 0xE ? 3 : (lead >> 3) == 0x1E ? 4 : 1;
		return std::min(length, (int)line.size() - col);
	}

	int PrevCodePointLength(const std::string& line, int col)
	{
		if (col <= 0)
			return 0;
		int length = 1;
		while (length < 4 && col - length > 0 && ((unsigned char)line[(size_t)(col - length)] & 0xC0) == 0x80)
			length++;
		return length;
	}

	void TextDocument::SetText(std::string_view text)
	{
		m_Lines.clear();
		std::string line;
		for (size_t i = 0; i < text.size(); i++)
		{
			const char c = text[i];
			if (c == '\r' && i + 1 < text.size() && text[i + 1] == '\n')
				continue;
			if (c == '\n')
			{
				m_Lines.push_back(std::move(line));
				line.clear();
			}
			else
				line.push_back(c);
		}
		m_Lines.push_back(std::move(line));
		m_Version++;
		m_FirstChanged = 0;
	}

	std::string TextDocument::GetText() const
	{
		std::string text;
		for (size_t i = 0; i < m_Lines.size(); i++)
		{
			if (i)
				text.push_back('\n');
			text += m_Lines[i];
		}
		return text;
	}

	TextPos TextDocument::Clamp(TextPos pos) const
	{
		pos.Line = std::clamp(pos.Line, 0, LineCount() - 1);
		pos.Col = std::clamp(pos.Col, 0, (int)m_Lines[(size_t)pos.Line].size());
		return pos;
	}

	std::string TextDocument::GetRange(const TextRange& range) const
	{
		const TextPos a = Clamp(range.Start);
		const TextPos b = Clamp(range.End);
		if (a.Line == b.Line)
			return m_Lines[(size_t)a.Line].substr((size_t)a.Col, (size_t)(b.Col - a.Col));
		std::string text = m_Lines[(size_t)a.Line].substr((size_t)a.Col);
		for (int line = a.Line + 1; line < b.Line; line++)
		{
			text.push_back('\n');
			text += m_Lines[(size_t)line];
		}
		text.push_back('\n');
		text += m_Lines[(size_t)b.Line].substr(0, (size_t)b.Col);
		return text;
	}

	TextPos TextDocument::Replace(const TextRange& range, std::string_view text)
	{
		const TextPos a = Clamp(range.Start);
		const TextPos b = Clamp(range.End);
		const std::string head = m_Lines[(size_t)a.Line].substr(0, (size_t)a.Col);
		const std::string tail = m_Lines[(size_t)b.Line].substr((size_t)b.Col);

		std::vector<std::string> inserted;
		std::string current = head;
		for (char c : text)
		{
			if (c == '\r')
				continue;
			if (c == '\n')
			{
				inserted.push_back(std::move(current));
				current.clear();
			}
			else
				current.push_back(c);
		}
		const TextPos end{ a.Line + (int)inserted.size(), (int)current.size() };
		current += tail;
		inserted.push_back(std::move(current));

		m_Lines.erase(m_Lines.begin() + a.Line, m_Lines.begin() + b.Line + 1);
		m_Lines.insert(m_Lines.begin() + a.Line, std::make_move_iterator(inserted.begin()), std::make_move_iterator(inserted.end()));
		m_Version++;
		m_FirstChanged = std::min(m_FirstChanged, a.Line);
		return end;
	}

	int TextDocument::TakeFirstChangedLine()
	{
		const int first = m_FirstChanged;
		m_FirstChanged = INT_MAX;
		return first;
	}

} // namespace wisp::code
