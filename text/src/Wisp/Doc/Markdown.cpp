#include "Wisp/Doc/Markdown.h"

#include <algorithm>
#include <cctype>

namespace wisp::doc {

	namespace {

		bool IsBlank(std::string_view line)
		{
			return line.find_first_not_of(" \t\r") == std::string_view::npos;
		}

		int IndentOf(std::string_view line)
		{
			int n = 0;
			while (n < (int)line.size() && line[n] == ' ')
				n++;
			return n;
		}

		std::string Trim(std::string_view s)
		{
			const size_t a = s.find_first_not_of(" \t\r");
			if (a == std::string_view::npos)
				return {};
			const size_t b = s.find_last_not_of(" \t\r");
			return std::string(s.substr(a, b - a + 1));
		}

		// Removes up to `n` leading spaces.
		std::string Dedent(std::string_view line, int n)
		{
			int i = 0;
			while (i < n && i < (int)line.size() && line[i] == ' ')
				i++;
			return std::string(line.substr(i));
		}

		bool IsFence(std::string_view line, char& ch, int& length, std::string& info)
		{
			const int indent = IndentOf(line);
			if (indent > 3 || indent >= (int)line.size())
				return false;
			const char c = line[indent];
			if (c != '`' && c != '~')
				return false;
			int n = 0;
			while (indent + n < (int)line.size() && line[indent + n] == c)
				n++;
			if (n < 3)
				return false;
			ch = c;
			length = n;
			info = Trim(line.substr(indent + n));
			if (c == '`' && info.find('`') != std::string::npos)
				return false;
			if (const size_t space = info.find(' '); space != std::string::npos)
				info.resize(space);
			return true;
		}

		bool IsHeading(std::string_view line, int& level, std::string& text)
		{
			const int indent = IndentOf(line);
			if (indent > 3)
				return false;
			int n = 0;
			while (indent + n < (int)line.size() && line[indent + n] == '#')
				n++;
			if (n < 1 || n > 6)
				return false;
			if (indent + n < (int)line.size() && line[indent + n] != ' ' && line[indent + n] != '\t')
				return false;
			level = n;
			text = Trim(line.substr(indent + n));
			// An optional closing run of '#'.
			size_t end = text.size();
			while (end > 0 && text[end - 1] == '#')
				end--;
			if (end < text.size() && (end == 0 || text[end - 1] == ' '))
				text = Trim(std::string_view(text).substr(0, end));
			return true;
		}

		bool IsRule(std::string_view line)
		{
			if (IndentOf(line) > 3)
				return false;
			char ch = 0;
			int count = 0;
			for (char c : line)
			{
				if (c == ' ' || c == '\t' || c == '\r')
					continue;
				if (c != '-' && c != '*' && c != '_')
					return false;
				if (ch && c != ch)
					return false;
				ch = c;
				count++;
			}
			return count >= 3;
		}

		bool IsQuote(std::string_view line)
		{
			const int indent = IndentOf(line);
			return indent <= 3 && indent < (int)line.size() && line[indent] == '>';
		}

		struct ListMarker
		{
			bool Ordered = false;
			char Delimiter = '-'; // the bullet, or '.' / ')'
			int Start = 1;
			int Indent = 0;        // the marker's column
			int ContentIndent = 0; // the item's content column
		};

		bool IsListMarker(std::string_view line, ListMarker& out)
		{
			const int indent = IndentOf(line);
			if (indent > 3 || indent >= (int)line.size())
				return false;
			int at = indent;
			ListMarker m;
			m.Indent = indent;
			const char c = line[at];
			if (c == '-' || c == '*' || c == '+')
			{
				m.Delimiter = c;
				at++;
			}
			else if (std::isdigit((unsigned char)c))
			{
				int digits = 0, value = 0;
				while (at < (int)line.size() && std::isdigit((unsigned char)line[at]) && digits < 9)
				{
					value = value * 10 + (line[at] - '0');
					at++;
					digits++;
				}
				if (at >= (int)line.size() || (line[at] != '.' && line[at] != ')'))
					return false;
				m.Ordered = true;
				m.Delimiter = line[at];
				m.Start = value;
				at++;
			}
			else
				return false;
			if (at < (int)line.size() && line[at] != ' ' && line[at] != '\t')
				return false;
			int spaces = 0;
			while (at + spaces < (int)line.size() && line[at + spaces] == ' ')
				spaces++;
			if (at + spaces >= (int)line.size())
				spaces = 1; // an empty item
			else if (spaces > 4)
				spaces = 1; // indented code inside an item reads as one space
			m.ContentIndent = at + std::max(1, spaces);
			out = m;
			return true;
		}

		bool SameList(const ListMarker& a, const ListMarker& b)
		{
			return a.Ordered == b.Ordered && a.Delimiter == b.Delimiter;
		}

		std::vector<std::string> SplitRow(std::string_view line)
		{
			std::string row = Trim(line);
			if (!row.empty() && row.front() == '|')
				row.erase(row.begin());
			if (!row.empty() && row.back() == '|' && (row.size() < 2 || row[row.size() - 2] != '\\'))
				row.pop_back();
			std::vector<std::string> cells;
			std::string cell;
			bool inCode = false;
			bool inDocLink = false; // a [[id|label]] keeps its bar
			for (size_t i = 0; i < row.size(); i++)
			{
				const char c = row[i];
				if (c == '\\' && i + 1 < row.size() && row[i + 1] == '|')
				{
					cell += '|';
					i++;
					continue;
				}
				if (c == '`')
					inCode = !inCode;
				if (!inCode && c == '[' && i + 1 < row.size() && row[i + 1] == '[')
					inDocLink = true;
				else if (inDocLink && c == ']' && i + 1 < row.size() && row[i + 1] == ']')
					inDocLink = false;
				if (c == '|' && !inCode && !inDocLink)
				{
					cells.push_back(Trim(cell));
					cell.clear();
					continue;
				}
				cell += c;
			}
			cells.push_back(Trim(cell));
			return cells;
		}

		bool IsTableDelimiter(std::string_view line, std::vector<Align>& aligns)
		{
			if (line.find('-') == std::string_view::npos)
				return false;
			aligns.clear();
			for (const std::string& cell : SplitRow(line))
			{
				if (cell.empty())
					return false;
				const bool left = cell.front() == ':';
				const bool right = cell.back() == ':';
				const std::string dashes = cell.substr(left ? 1 : 0, cell.size() - (left ? 1 : 0) - (right && cell.size() > 1 ? 1 : 0));
				if (dashes.empty() || dashes.find_first_not_of('-') != std::string::npos)
					return false;
				aligns.push_back(left && right ? Align::Centre : right ? Align::Right : left ? Align::Left : Align::Default);
			}
			return !aligns.empty();
		}

		bool IsTableStart(const std::vector<std::string>& lines, size_t i)
		{
			std::vector<Align> aligns;
			return lines[i].find('|') != std::string::npos && i + 1 < lines.size() && IsTableDelimiter(lines[i + 1], aligns);
		}

		bool StartsBlock(const std::vector<std::string>& lines, size_t i)
		{
			char ch;
			int length, level;
			std::string info, text;
			ListMarker marker;
			const std::string& line = lines[i];
			return IsFence(line, ch, length, info) || IsHeading(line, level, text) || IsRule(line) || IsQuote(line)
				|| (IsListMarker(line, marker) && !IsBlank(line.substr(marker.ContentIndent < (int)line.size() ? marker.ContentIndent : line.size())))
				|| IsTableStart(lines, i);
		}

		void ParseBlocks(const std::vector<std::string>& lines, std::vector<Block>& out);

		// A list starting at lines[i]; advances i past it.
		Block ParseList(const std::vector<std::string>& lines, size_t& i, const ListMarker& first)
		{
			Block list;
			list.Kind = BlockKind::List;
			list.Ordered = first.Ordered;
			list.Start = first.Start;
			ListMarker marker = first;
			while (i < lines.size())
			{
				std::vector<std::string> body;
				const std::string& head = lines[i];
				body.push_back(marker.ContentIndent < (int)head.size() ? head.substr(marker.ContentIndent) : std::string());
				i++;
				bool sawBlank = false;
				while (i < lines.size())
				{
					const std::string& line = lines[i];
					if (IsBlank(line))
					{
						sawBlank = true;
						body.emplace_back();
						i++;
						continue;
					}
					if (IndentOf(line) >= marker.ContentIndent)
					{
						if (sawBlank)
							list.Tight = false;
						sawBlank = false;
						body.push_back(Dedent(line, marker.ContentIndent));
						i++;
						continue;
					}
					ListMarker next;
					if (IsListMarker(line, next))
						break; // the next item, or another list
					if (!sawBlank && !StartsBlock(lines, i))
					{
						body.push_back(Trim(line)); // a lazy paragraph continuation
						i++;
						continue;
					}
					break;
				}
				// Trailing blank lines belong to the gap, not the item.
				while (!body.empty() && IsBlank(body.back()))
					body.pop_back();
				Block item;
				item.Kind = BlockKind::ListItem;
				ParseBlocks(body, item.Children);
				list.Children.push_back(std::move(item));

				if (i >= lines.size())
					break;
				ListMarker next;
				if (!IsListMarker(lines[i], next) || !SameList(next, marker))
					break;
				if (i > 0 && IsBlank(lines[i - 1]))
					list.Tight = false;
				marker = next;
			}
			return list;
		}

		void ParseBlocks(const std::vector<std::string>& lines, std::vector<Block>& out)
		{
			size_t i = 0;
			while (i < lines.size())
			{
				const std::string& line = lines[i];
				if (IsBlank(line))
				{
					i++;
					continue;
				}
				char fenceChar;
				int fenceLength, level;
				std::string info, text;
				if (IsFence(line, fenceChar, fenceLength, info))
				{
					const int indent = IndentOf(line);
					Block code;
					code.Kind = BlockKind::Code;
					code.Info = info;
					i++;
					while (i < lines.size())
					{
						const std::string& l = lines[i];
						const std::string t = Trim(l);
						if (IndentOf(l) <= 3 && t.size() >= (size_t)fenceLength && t.find_first_not_of(fenceChar) == std::string::npos)
						{
							i++;
							break;
						}
						code.Text += Dedent(l, indent);
						code.Text += '\n';
						i++;
					}
					if (!code.Text.empty())
						code.Text.pop_back();
					out.push_back(std::move(code));
					continue;
				}
				if (IsHeading(line, level, text))
				{
					Block heading;
					heading.Kind = BlockKind::Heading;
					heading.Level = level;
					heading.Text = text;
					heading.Inlines = ParseInlines(text);
					out.push_back(std::move(heading));
					i++;
					continue;
				}
				if (IsRule(line))
				{
					Block rule;
					rule.Kind = BlockKind::Rule;
					out.push_back(rule);
					i++;
					continue;
				}
				if (IsQuote(line))
				{
					std::vector<std::string> inner;
					while (i < lines.size() && IsQuote(lines[i]))
					{
						std::string l = lines[i].substr(IndentOf(lines[i]) + 1);
						if (!l.empty() && l[0] == ' ')
							l.erase(l.begin());
						inner.push_back(std::move(l));
						i++;
					}
					Block quote;
					quote.Kind = BlockKind::Quote;
					if (!inner.empty())
					{
						const std::string first = Trim(inner.front());
						if (first.size() > 3 && first.rfind("[!", 0) == 0 && first.back() == ']')
						{
							for (size_t c = 2; c + 1 < first.size(); c++)
								quote.Info += (char)std::tolower((unsigned char)first[c]);
							inner.erase(inner.begin());
						}
					}
					ParseBlocks(inner, quote.Children);
					out.push_back(std::move(quote));
					continue;
				}
				ListMarker marker;
				if (IsListMarker(line, marker))
				{
					out.push_back(ParseList(lines, i, marker));
					continue;
				}
				if (IsTableStart(lines, i))
				{
					Block table;
					table.Kind = BlockKind::Table;
					IsTableDelimiter(lines[i + 1], table.Aligns);
					auto addRow = [&](const std::string& l) {
						std::vector<std::vector<Inline>> row;
						for (const std::string& cell : SplitRow(l))
							row.push_back(ParseInlines(cell));
						row.resize(table.Aligns.size());
						table.Rows.push_back(std::move(row));
					};
					addRow(lines[i]);
					i += 2;
					while (i < lines.size() && !IsBlank(lines[i]) && lines[i].find('|') != std::string::npos)
						addRow(lines[i++]);
					out.push_back(std::move(table));
					continue;
				}
				// A paragraph: until a blank line or another block's start.
				std::string paragraph = Trim(line);
				i++;
				while (i < lines.size() && !IsBlank(lines[i]) && !StartsBlock(lines, i))
				{
					paragraph += '\n';
					paragraph += Trim(lines[i]);
					i++;
				}
				Block p;
				p.Kind = BlockKind::Paragraph;
				p.Text = paragraph;
				p.Inlines = ParseInlines(paragraph);
				out.push_back(std::move(p));
			}
		}

		// ---- inlines ----------------------------------------------------

		bool IsPunct(char c)
		{
			return std::ispunct((unsigned char)c) != 0;
		}

		void PushText(std::vector<Inline>& out, std::string_view text)
		{
			if (text.empty())
				return;
			if (!out.empty() && out.back().Kind == InlineKind::Text)
			{
				out.back().Text += text;
				return;
			}
			Inline run;
			run.Text = std::string(text);
			out.push_back(std::move(run));
		}

		// The end of a code span opening at `at` (a run of n backticks), or npos.
		size_t CodeSpanEnd(std::string_view s, size_t at, size_t& runLength)
		{
			size_t n = 0;
			while (at + n < s.size() && s[at + n] == '`')
				n++;
			runLength = n;
			for (size_t i = at + n; i < s.size();)
			{
				if (s[i] != '`')
				{
					i++;
					continue;
				}
				size_t m = 0;
				while (i + m < s.size() && s[i + m] == '`')
					m++;
				if (m == n)
					return i;
				i += m;
			}
			return std::string_view::npos;
		}

		// The ']' closing the '[' at `open` (nested brackets and code spans skipped).
		size_t BracketEnd(std::string_view s, size_t open)
		{
			int depth = 0;
			for (size_t i = open; i < s.size(); i++)
			{
				const char c = s[i];
				if (c == '\\')
				{
					i++;
					continue;
				}
				if (c == '`')
				{
					size_t run;
					const size_t end = CodeSpanEnd(s, i, run);
					if (end != std::string_view::npos)
					{
						i = end + run - 1;
						continue;
					}
				}
				if (c == '[')
					depth++;
				else if (c == ']' && --depth == 0)
					return i;
			}
			return std::string_view::npos;
		}

		// `(target)` right after a ']' at `close`: the target and the ')' index.
		bool ParenTarget(std::string_view s, size_t close, std::string& target, size_t& end)
		{
			if (close + 1 >= s.size() || s[close + 1] != '(')
				return false;
			const size_t rparen = s.find(')', close + 2);
			if (rparen == std::string_view::npos)
				return false;
			target = Trim(s.substr(close + 2, rparen - close - 2));
			// A title ("...") after the url is dropped.
			if (const size_t space = target.find(' '); space != std::string::npos)
				target.resize(space);
			if (target.size() >= 2 && target.front() == '<' && target.back() == '>')
				target = target.substr(1, target.size() - 2);
			end = rparen;
			return true;
		}

		// The closing delimiter run for an emphasis opener of `n` × `ch` at
		// `from`: not preceded by whitespace, not inside a code span.
		size_t EmphasisEnd(std::string_view s, size_t from, char ch, size_t n)
		{
			for (size_t i = from; i + n <= s.size(); i++)
			{
				const char c = s[i];
				if (c == '\\')
				{
					i++;
					continue;
				}
				if (c == '`')
				{
					size_t run;
					const size_t end = CodeSpanEnd(s, i, run);
					if (end != std::string_view::npos)
						i = end + run - 1;
					continue;
				}
				if (c != ch || i == from)
					continue;
				size_t run = 0;
				while (i + run < s.size() && s[i + run] == ch)
					run++;
				if (std::isspace((unsigned char)s[i - 1]))
				{
					i += run - 1;
					continue;
				}
				if (ch == '_' && i + run < s.size() && std::isalnum((unsigned char)s[i + run]))
				{
					i += run - 1;
					continue; // intraword underscores never close
				}
				if (run == n || (n == 1 && run >= 3) || (n == 2 && run >= 3))
					return i;
				if (n == 1 && run == 2)
				{
					// A strong run inside emphasis: skip past its close.
					const size_t inner = EmphasisEnd(s, i + 2, ch, 2);
					if (inner != std::string_view::npos)
					{
						i = inner + 1;
						continue;
					}
				}
				i += run - 1;
			}
			return std::string_view::npos;
		}

		void ParseInto(std::string_view s, std::vector<Inline>& out)
		{
			size_t textStart = 0;
			auto flush = [&](size_t upTo) {
				if (upTo > textStart)
					PushText(out, s.substr(textStart, upTo - textStart));
			};
			for (size_t i = 0; i < s.size();)
			{
				const char c = s[i];
				if (c == '\\' && i + 1 < s.size() && IsPunct(s[i + 1]))
				{
					flush(i);
					PushText(out, s.substr(i + 1, 1));
					i += 2;
					textStart = i;
					continue;
				}
				if (c == '\n')
				{
					flush(i);
					PushText(out, " ");
					i++;
					textStart = i;
					continue;
				}
				if (c == '`')
				{
					size_t run;
					const size_t end = CodeSpanEnd(s, i, run);
					if (end == std::string_view::npos)
					{
						i += run;
						continue;
					}
					flush(i);
					std::string code(s.substr(i + run, end - i - run));
					std::replace(code.begin(), code.end(), '\n', ' ');
					if (code.size() >= 2 && code.front() == ' ' && code.back() == ' ' && code.find_first_not_of(' ') != std::string::npos)
						code = code.substr(1, code.size() - 2);
					Inline span;
					span.Kind = InlineKind::Code;
					span.Text = std::move(code);
					out.push_back(std::move(span));
					i = end + run;
					textStart = i;
					continue;
				}
				if (c == '[' && i + 1 < s.size() && s[i + 1] == '[')
				{
					const size_t close = s.find("]]", i + 2);
					if (close != std::string_view::npos && s.substr(i + 2, close - i - 2).find('\n') == std::string_view::npos)
					{
						flush(i);
						const std::string body = Trim(s.substr(i + 2, close - i - 2));
						Inline link;
						link.Kind = InlineKind::DocLink;
						const size_t bar = body.find('|');
						link.Target = Trim(std::string_view(body).substr(0, bar));
						if (bar != std::string::npos)
							link.Text = Trim(std::string_view(body).substr(bar + 1));
						out.push_back(std::move(link));
						i = close + 2;
						textStart = i;
						continue;
					}
				}
				if ((c == '[') || (c == '!' && i + 1 < s.size() && s[i + 1] == '['))
				{
					const bool image = c == '!';
					const size_t open = image ? i + 1 : i;
					const size_t close = BracketEnd(s, open);
					std::string target;
					size_t end = 0;
					if (close != std::string_view::npos && ParenTarget(s, close, target, end))
					{
						flush(i);
						Inline link;
						link.Kind = image ? InlineKind::Image : InlineKind::Link;
						link.Target = std::move(target);
						const std::string_view label = s.substr(open + 1, close - open - 1);
						if (image)
							link.Text = PlainText(ParseInlines(label));
						else
							ParseInto(label, link.Children);
						out.push_back(std::move(link));
						i = end + 1;
						textStart = i;
						continue;
					}
				}
				if (c == '*' || c == '_')
				{
					size_t run = 0;
					while (i + run < s.size() && s[i + run] == c)
						run++;
					const bool opens = i + run < s.size() && !std::isspace((unsigned char)s[i + run])
						&& (c == '*' || i == 0 || !std::isalnum((unsigned char)s[i - 1]));
					if (opens)
					{
						const size_t n = run >= 2 ? 2 : 1;
						const size_t end = EmphasisEnd(s, i + n, c, n);
						if (end != std::string_view::npos)
						{
							flush(i);
							// A run of three opens strong-in-emphasis: the
							// extra delimiter stays literal inside.
							Inline span;
							span.Kind = n == 2 ? InlineKind::Strong : InlineKind::Emphasis;
							ParseInto(s.substr(i + n, end - i - n), span.Children);
							out.push_back(std::move(span));
							i = end + n;
							textStart = i;
							continue;
						}
					}
					i += run;
					continue;
				}
				i++;
			}
			flush(s.size());
		}

	} // namespace

	std::vector<Inline> ParseInlines(std::string_view text)
	{
		std::vector<Inline> out;
		ParseInto(text, out);
		return out;
	}

	Document Parse(std::string_view markdown)
	{
		std::vector<std::string> lines;
		size_t at = 0;
		while (at <= markdown.size())
		{
			const size_t nl = markdown.find('\n', at);
			std::string line(markdown.substr(at, nl == std::string_view::npos ? std::string_view::npos : nl - at));
			if (!line.empty() && line.back() == '\r')
				line.pop_back();
			// Tabs as four spaces (the indentation rules count columns).
			std::string expanded;
			for (char c : line)
				if (c == '\t')
					expanded.append(4 - expanded.size() % 4, ' ');
				else
					expanded += c;
			lines.push_back(std::move(expanded));
			if (nl == std::string_view::npos)
				break;
			at = nl + 1;
		}
		Document doc;
		ParseBlocks(lines, doc.Blocks);
		return doc;
	}

	std::string PlainText(const std::vector<Inline>& inlines)
	{
		std::string out;
		for (const Inline& in : inlines)
		{
			switch (in.Kind)
			{
				case InlineKind::Emphasis:
				case InlineKind::Strong:
				case InlineKind::Link: out += PlainText(in.Children); break;
				case InlineKind::DocLink: out += in.Text.empty() ? in.Target : in.Text; break;
				default: out += in.Text; break;
			}
		}
		return out;
	}

	std::string Slug(std::string_view text)
	{
		std::string out;
		bool dash = false;
		for (char c : text)
		{
			if (std::isalnum((unsigned char)c))
			{
				if (dash && !out.empty())
					out += '-';
				dash = false;
				out += (char)std::tolower((unsigned char)c);
			}
			else
				dash = true;
		}
		return out;
	}

} // namespace wisp::doc
