#include "Wisp/Code/Tokenizer.h"

#include <algorithm>
#include <cctype>

namespace wisp::code {

	namespace {

		// The carried state: kind in the high byte, the long bracket's level
		// (the count of '=') below it.
		constexpr uint32_t kLongString = 1u << 24;
		constexpr uint32_t kLongComment = 2u << 24;

		bool IsIdentStart(char c) { return std::isalpha((unsigned char)c) || c == '_' || (unsigned char)c >= 0x80; }
		bool IsIdent(char c) { return IsIdentStart(c) || std::isdigit((unsigned char)c); }

		bool IsKeyword(std::string_view word)
		{
			for (std::string_view keyword : { "and", "break", "do", "else", "elseif", "end", "for", "function", "if", "in",
			                                  "local", "not", "or", "repeat", "return", "then", "until", "while", "continue",
			                                  "type", "export" })
				if (word == keyword)
					return true;
			return false;
		}

		bool IsLiteral(std::string_view word)
		{
			return word == "true" || word == "false" || word == "nil" || word == "self";
		}

		// The level of a long bracket opening at `at` ("[", "=" * level, "["),
		// or -1.
		int LongBracketLevel(std::string_view line, size_t at)
		{
			if (at >= line.size() || line[at] != '[')
				return -1;
			size_t i = at + 1;
			int level = 0;
			while (i < line.size() && line[i] == '=')
			{
				level++;
				i++;
			}
			return i < line.size() && line[i] == '[' ? level : -1;
		}

		// The end of the long bracket of `level` closing at or after `from`
		// (one past its last ']'), or npos.
		size_t FindLongClose(std::string_view line, size_t from, int level)
		{
			for (size_t i = from; i < line.size(); i++)
			{
				if (line[i] != ']')
					continue;
				size_t j = i + 1;
				int equals = 0;
				while (j < line.size() && line[j] == '=')
				{
					equals++;
					j++;
				}
				if (equals == level && j < line.size() && line[j] == ']')
					return j + 1;
			}
			return std::string_view::npos;
		}

	} // namespace

	LuauTokenizer::LuauTokenizer(std::unordered_set<std::string> globals)
		: m_Globals(std::move(globals))
	{
	}

	uint32_t LuauTokenizer::TokenizeLine(std::string_view line, uint32_t state, std::vector<Token>& out) const
	{
		size_t i = 0;
		auto emit = [&](size_t start, size_t end, TokenKind kind) {
			if (end > start)
				out.push_back({ (int)start, (int)(end - start), kind });
		};

		// A long string or comment carried over from the line above.
		if (state != 0)
		{
			const int level = (int)(state & 0xFFFF);
			const TokenKind kind = (state & 0xFF000000u) == kLongComment ? TokenKind::Comment : TokenKind::String;
			const size_t close = FindLongClose(line, 0, level);
			if (close == std::string_view::npos)
			{
				emit(0, line.size(), kind);
				return state;
			}
			emit(0, close, kind);
			i = close;
		}

		while (i < line.size())
		{
			const char c = line[i];
			if (c == ' ' || c == '\t')
			{
				i++;
				continue;
			}
			// Comments: `--` to the end of the line, or `--[==[ ... ]==]`.
			if (c == '-' && i + 1 < line.size() && line[i + 1] == '-')
			{
				const int level = LongBracketLevel(line, i + 2);
				if (level >= 0)
				{
					const size_t close = FindLongClose(line, i + 2 + (size_t)level + 2, level);
					if (close == std::string_view::npos)
					{
						emit(i, line.size(), TokenKind::Comment);
						return kLongComment | (uint32_t)level;
					}
					emit(i, close, TokenKind::Comment);
					i = close;
					continue;
				}
				emit(i, line.size(), TokenKind::Comment);
				return 0;
			}
			// Long strings `[==[ ... ]==]`.
			if (c == '[')
			{
				const int level = LongBracketLevel(line, i);
				if (level >= 0)
				{
					const size_t close = FindLongClose(line, i + (size_t)level + 2, level);
					if (close == std::string_view::npos)
					{
						emit(i, line.size(), TokenKind::String);
						return kLongString | (uint32_t)level;
					}
					emit(i, close, TokenKind::String);
					i = close;
					continue;
				}
			}
			// Quoted and interpolated strings (an unterminated one ends at the
			// line's end, as Luau reports it).
			if (c == '"' || c == '\'' || c == '`')
			{
				size_t j = i + 1;
				while (j < line.size() && line[j] != c)
					j += line[j] == '\\' ? 2 : 1;
				const size_t end = std::min(line.size(), j + 1);
				emit(i, end, TokenKind::String);
				i = end;
				continue;
			}
			// Numbers: decimal (with '_' separators, a fraction, an exponent),
			// hex and binary.
			if (std::isdigit((unsigned char)c) || (c == '.' && i + 1 < line.size() && std::isdigit((unsigned char)line[i + 1])))
			{
				size_t j = i;
				if (c == '0' && j + 1 < line.size() && (line[j + 1] == 'x' || line[j + 1] == 'X' || line[j + 1] == 'b' || line[j + 1] == 'B'))
					j += 2;
				while (j < line.size() && (std::isalnum((unsigned char)line[j]) || line[j] == '_' || line[j] == '.' ||
				                           ((line[j] == '+' || line[j] == '-') && (line[j - 1] == 'e' || line[j - 1] == 'E'))))
					j++;
				emit(i, j, TokenKind::Number);
				i = j;
				continue;
			}
			if (IsIdentStart(c))
			{
				size_t j = i + 1;
				while (j < line.size() && IsIdent(line[j]))
					j++;
				const std::string_view word = line.substr(i, j - i);
				TokenKind kind = TokenKind::Plain;
				if (IsKeyword(word))
					kind = TokenKind::Keyword;
				else if (IsLiteral(word))
					kind = TokenKind::Literal;
				else if (m_Globals.count(std::string(word)))
					kind = TokenKind::Global;
				else
				{
					size_t k = j;
					while (k < line.size() && (line[k] == ' ' || line[k] == '\t'))
						k++;
					if (k < line.size() && line[k] == '(')
						kind = TokenKind::Function;
				}
				emit(i, j, kind);
				i = j;
				continue;
			}
			// Everything else is an operator or punctuation, one byte each.
			emit(i, i + 1, TokenKind::Operator);
			i++;
		}
		return 0;
	}

} // namespace wisp::code
