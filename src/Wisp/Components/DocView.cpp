#include "Wisp/Components/DocView.h"
#include "Wisp/Application.h"
#include "Wisp/StyleTokens.h"

#include <algorithm>
#include <cmath>

namespace wisp {

	namespace {

		enum Key : int { kHome = 268, kEnd = 269, kPageUp = 266, kPageDown = 267, kUp = 265, kDown = 264 };
		constexpr float kWheelStep = 48.0f;
		constexpr float kScrollbarWidth = 4.0f;
		constexpr float kScrollbarGutter = 10.0f;

		uint32_t Decode(const std::string& s, size_t& at)
		{
			const unsigned char c = (unsigned char)s[at];
			int length = c < 0x80 ? 1 : (c >> 5) == 0x6 ? 2 : (c >> 4) == 0xE ? 3 : (c >> 3) == 0x1E ? 4 : 1;
			if (at + length > s.size())
				length = 1;
			const unsigned char* p = (const unsigned char*)s.data() + at;
			at += length;
			switch (length)
			{
			case 2: return ((p[0] & 0x1Fu) << 6) | (p[1] & 0x3Fu);
			case 3: return ((p[0] & 0x0Fu) << 12) | ((p[1] & 0x3Fu) << 6) | (p[2] & 0x3Fu);
			case 4: return ((p[0] & 0x07u) << 18) | ((p[1] & 0x3Fu) << 12) | ((p[2] & 0x3Fu) << 6) | (p[3] & 0x3Fu);
			default: return p[0];
			}
		}

		glm::vec4 SyntaxColour(code::TokenKind kind)
		{
			switch (kind)
			{
			case code::TokenKind::Keyword: return tokens::SyntaxKeyword;
			case code::TokenKind::Literal: return tokens::SyntaxLiteral;
			case code::TokenKind::Number: return tokens::SyntaxNumber;
			case code::TokenKind::String: return tokens::SyntaxString;
			case code::TokenKind::Comment: return tokens::SyntaxComment;
			case code::TokenKind::Operator: return tokens::SyntaxOperator;
			case code::TokenKind::Function: return tokens::SyntaxFunction;
			case code::TokenKind::Global: return tokens::SyntaxGlobal;
			default: return tokens::DocCodeText;
			}
		}

		glm::vec4 CalloutColour(const std::string& kind)
		{
			if (kind == "note" || kind == "important")
				return tokens::DocNote;
			if (kind == "tip")
				return tokens::DocTip;
			if (kind == "warning" || kind == "caution")
				return tokens::DocWarning;
			return tokens::DocQuoteBar;
		}

		std::string CalloutTitle(const std::string& kind)
		{
			if (kind.empty())
				return {};
			std::string title = kind;
			title[0] = (char)std::toupper((unsigned char)title[0]);
			return title;
		}

		FontHandle Face(FontRole role)
		{
			return UIApplication::FontOr(role, UIApplication::FontOr(FontRole::Regular, 0));
		}

	} // namespace

	DocView::DocView(FontCache* fontCache, Host host)
		: m_FontCache(fontCache)
		, m_Host(std::move(host))
	{
		SetSupportsFocus(true);
	}

	void DocView::SetDocument(doc::Document document, bool keepPlace)
	{
		m_PendingBlock = -1;
		m_PendingAnchor.clear();
		if (keepPlace && !m_BlockTops.empty())
		{
			// The top block at or above the view's top, and how far into it.
			int block = 0;
			for (int i = 0; i < (int)m_BlockTops.size(); i++)
				if (m_BlockTops[i] <= m_Scroll)
					block = i;
			m_PendingBlock = block;
			m_PendingOffset = m_Scroll - m_BlockTops[block];
		}
		else
			m_Scroll = 0.0f;
		m_Document = std::move(document);
		m_LayoutWidth = -1.0f;
		m_HoveredLink = -1;
		m_PressedLink = -1;
	}

	bool DocView::ScrollToAnchor(const std::string& slug)
	{
		if (m_LayoutWidth < 0.0f)
		{
			m_PendingAnchor = slug;
			for (const doc::Block& b : m_Document.Blocks)
				if (b.Kind == doc::BlockKind::Heading && doc::Slug(doc::PlainText(b.Inlines)) == slug)
					return true;
			return false;
		}
		for (const auto& [anchor, y] : m_Anchors)
			if (anchor == slug)
			{
				m_Scroll = y;
				ClampScroll();
				return true;
			}
		return false;
	}

	// ---- measurement --------------------------------------------------------

	float DocView::MeasureWord(const std::string& text, FontHandle font, float size) const
	{
		if (!m_FontCache)
			return (float)text.size() * size * 0.5f;
		float width = 0.0f;
		uint32_t prev = 0;
		for (size_t at = 0; at < text.size();)
		{
			const uint32_t cp = Decode(text, at);
			if (const GlyphInfo* g = m_FontCache->GetGlyph(font, cp, size))
				width += g->AdvanceX;
			if (prev)
				width += m_FontCache->GetKerning(font, prev, cp, size);
			prev = cp;
		}
		return width;
	}

	float DocView::LineHeightOf(FontHandle font, float size) const
	{
		const float base = m_FontCache ? m_FontCache->GetMetrics(font, size).LineHeight : size * 1.2f;
		return std::round(base * 1.32f);
	}

	float DocView::AscentOf(FontHandle font, float size) const
	{
		if (!m_FontCache)
			return size;
		const FontMetrics m = m_FontCache->GetMetrics(font, size);
		return std::round((LineHeightOf(font, size) - m.LineHeight) * 0.5f + m.Ascent);
	}

	// ---- layout -------------------------------------------------------------

	void DocView::Flatten(const std::vector<doc::Inline>& inlines, const Style& style, bool measureOnly,
		std::vector<std::pair<std::string, Style>>& out) const
	{
		for (const doc::Inline& in : inlines)
		{
			Style s = style;
			switch (in.Kind)
			{
			case doc::InlineKind::Text: out.emplace_back(in.Text, style); break;
			case doc::InlineKind::Emphasis:
				s.Font = Face(FontRole::Medium);
				s.Colour = style.Link >= 0 ? style.Colour : tokens::DocHeading;
				Flatten(in.Children, s, measureOnly, out);
				break;
			case doc::InlineKind::Strong:
				s.Font = Face(FontRole::SemiBold);
				s.Colour = style.Link >= 0 ? style.Colour : tokens::DocHeading;
				Flatten(in.Children, s, measureOnly, out);
				break;
			case doc::InlineKind::Code:
				s.Font = Face(FontRole::Mono);
				s.Size = std::min(style.Size, tokens::DocFontCode);
				s.Colour = style.Link >= 0 ? style.Colour : tokens::DocCodeText;
				s.Code = m_NextCodeSpan++;
				out.emplace_back(in.Text, s);
				break;
			case doc::InlineKind::Link:
				if (!measureOnly)
				{
					s.Link = (int)m_Links.size();
					m_Links.push_back({ in.Target, false });
				}
				s.Colour = tokens::TextLink;
				Flatten(in.Children, s, measureOnly, out);
				break;
			case doc::InlineKind::DocLink:
			{
				std::string label = in.Text;
				if (label.empty() && m_Host.LinkLabel)
					label = m_Host.LinkLabel(in.Target);
				if (label.empty())
					label = in.Target;
				const bool resolves = !m_Host.LinkResolves || m_Host.LinkResolves(in.Target);
				if (!measureOnly && resolves)
				{
					s.Link = (int)m_Links.size();
					m_Links.push_back({ in.Target, true });
				}
				s.Colour = resolves ? tokens::TextLink : tokens::DocLinkDead;
				out.emplace_back(label, s);
				break;
			}
			case doc::InlineKind::Image:
				s.Colour = tokens::TextMuted;
				out.emplace_back("[" + (in.Text.empty() ? in.Target : in.Text) + "]", s);
				break;
			}
		}
	}

	float DocView::FlowInlines(const std::vector<doc::Inline>& inlines, float x, float width, float y, const Style& base, bool measureOnly,
		float* naturalWidth, float* longestWord) const
	{
		std::vector<std::pair<std::string, Style>> segments;
		Flatten(inlines, base, measureOnly, segments);
		const float lh = LineHeightOf(base.Font, base.Size);
		const float ascent = AscentOf(base.Font, base.Size);
		const float right = x + width;
		float penX = x;
		float top = y;
		float widest = 0.0f;
		bool pendingSpace = false;
		float pendingSpaceWidth = 0.0f;
		int lastCode = -1;
		int lastCodeBox = -1;
		float lastCodeTop = -1.0f;
		bool lineEmpty = true;

		float longest = 0.0f;
		auto placeWord = [&](const std::string& word, const Style& s) {
			const float w = MeasureWord(word, s.Font, s.Size);
			longest = std::max(longest, w);
			float at = penX;
			if (pendingSpace && !lineEmpty)
				at += pendingSpaceWidth;
			if (!lineEmpty && at + w > right)
			{
				widest = std::max(widest, penX - x);
				top += lh;
				at = x;
				lastCode = -1;
			}
			const bool sameCodeSpan = s.Code >= 0 && s.Code == lastCode && lastCodeTop == top;
			if (!measureOnly)
			{
				Run run;
				run.Pen = { at, top + ascent };
				run.Text = word;
				run.Font = s.Font;
				run.Size = s.Size;
				run.Colour = s.Colour;
				run.Link = s.Link;
				run.Width = w;
				m_Runs.push_back(std::move(run));
				if (s.Link >= 0)
					m_LinkRects.push_back({ { at, top }, { w + (pendingSpace && !lineEmpty && at > x ? 0.0f : 0.0f), lh }, s.Link });
				if (s.Code >= 0)
				{
					const float boxH = std::round(lh * 0.78f);
					const float boxTop = std::round(top + (lh - boxH) * 0.5f + 1.0f);
					if (sameCodeSpan && lastCodeBox >= 0)
						m_Boxes[lastCodeBox].Size.x = at + w + 3.0f - m_Boxes[lastCodeBox].Pos.x;
					else
					{
						lastCodeBox = (int)m_Boxes.size();
						m_Boxes.push_back({ { at - 3.0f, boxTop }, { w + 6.0f, boxH }, tokens::DocInlineCode, tokens::RadiusSmall });
					}
				}
			}
			if (s.Code >= 0)
			{
				lastCode = s.Code;
				lastCodeTop = top;
			}
			penX = at + w;
			pendingSpace = false;
			lineEmpty = false;
		};

		for (const auto& [text, style] : segments)
		{
			std::string word;
			for (size_t i = 0; i <= text.size(); i++)
			{
				const bool end = i == text.size();
				const char c = end ? ' ' : text[i];
				if (c == ' ' || c == '\t')
				{
					if (!word.empty())
					{
						placeWord(word, style);
						word.clear();
					}
					if (!end)
					{
						pendingSpace = true;
						pendingSpaceWidth = MeasureWord(" ", style.Font, style.Size);
					}
					continue;
				}
				word += c;
			}
		}
		widest = std::max(widest, penX - x);
		if (naturalWidth)
			*naturalWidth = widest;
		if (longestWord)
			*longestWord = longest;
		return lineEmpty ? 0.0f : top - y + lh;
	}

	void DocView::LayoutCode(const doc::Block& block, float x, float width, float& y) const
	{
		const FontHandle mono = Face(FontRole::Mono);
		const float size = tokens::DocFontCode;
		const float lh = std::round((m_FontCache ? m_FontCache->GetMetrics(mono, size).LineHeight : size * 1.2f) * 1.25f);
		const float ascent = AscentOf(mono, size) - std::round((LineHeightOf(mono, size) - lh) * 0.5f);
		std::vector<std::string> lines;
		for (size_t at = 0;;)
		{
			const size_t nl = block.Text.find('\n', at);
			lines.push_back(block.Text.substr(at, nl == std::string::npos ? std::string::npos : nl - at));
			if (nl == std::string::npos)
				break;
			at = nl + 1;
		}
		const float pad = tokens::DocCodePad;
		const int box = (int)m_Boxes.size();
		m_Boxes.push_back({ { x, y }, { width, lines.size() * lh + pad * 2.0f }, tokens::CodeBackground, tokens::RadiusMedium, 1.0f, tokens::Border });
		std::shared_ptr<const code::ITokenizer> tokenizer = m_Host.Tokenizer ? m_Host.Tokenizer(block.Info) : nullptr;
		uint32_t state = 0;
		std::vector<code::Token> tokens;
		for (size_t i = 0; i < lines.size(); i++)
		{
			const std::string& line = lines[i];
			const float baseline = y + pad + i * lh + ascent;
			tokens.clear();
			if (tokenizer)
				state = tokenizer->TokenizeLine(line, state, tokens);
			float penX = x + pad;
			int col = 0;
			auto emit = [&](int from, int to, glm::vec4 colour) {
				if (to <= from)
					return;
				const std::string text = line.substr(from, to - from);
				const float w = MeasureWord(text, mono, size);
				if (text.find_first_not_of(' ') != std::string::npos)
				{
					Run run;
					run.Pen = { penX, baseline };
					run.Text = text;
					run.Font = mono;
					run.Size = size;
					run.Colour = colour;
					run.Width = w;
					run.Clip = box;
					m_Runs.push_back(std::move(run));
				}
				penX += w;
			};
			for (const code::Token& t : tokens)
			{
				emit(col, t.Start, tokens::DocCodeText);
				emit(t.Start, t.Start + t.Length, SyntaxColour(t.Kind));
				col = t.Start + t.Length;
			}
			emit(col, (int)line.size(), tokens::DocCodeText);
		}
		y += m_Boxes[box].Size.y + tokens::DocBlockGap;
	}

	void DocView::LayoutTable(const doc::Block& block, float x, float width, float& y) const
	{
		const size_t columns = block.Aligns.size();
		if (columns == 0 || block.Rows.empty())
			return;
		const float pad = tokens::DocCellPad;
		Style body{ Face(FontRole::Regular), tokens::DocFontBody, tokens::DocText };
		Style head{ Face(FontRole::SemiBold), tokens::DocFontBody, tokens::DocHeading };
		std::vector<float> natural(columns, 0.0f);
		std::vector<float> minimum(columns, 0.0f); // the longest word: a column never wraps inside one
		for (size_t r = 0; r < block.Rows.size(); r++)
			for (size_t c = 0; c < columns && c < block.Rows[r].size(); c++)
			{
				float w = 0.0f, word = 0.0f;
				FlowInlines(block.Rows[r][c], 0.0f, 1.0e6f, 0.0f, r == 0 ? head : body, true, &w, &word);
				natural[c] = std::max(natural[c], w + pad * 2.0f);
				minimum[c] = std::max(minimum[c], word + pad * 2.0f + 8.0f);
			}
		float total = 0.0f;
		for (float w : natural)
			total += w;
		std::vector<float> widths = natural;
		if (total > width)
		{
			// Each column keeps its longest word; the room left over goes to
			// the columns in proportion to how much more they would take.
			float floor = 0.0f, extra = 0.0f;
			for (size_t c = 0; c < columns; c++)
			{
				minimum[c] = std::min(minimum[c], natural[c]);
				floor += minimum[c];
				extra += natural[c] - minimum[c];
			}
			const float room = std::max(0.0f, width - floor);
			for (size_t c = 0; c < columns; c++)
				widths[c] = minimum[c] + (extra > 0.0f ? (natural[c] - minimum[c]) * std::min(1.0f, room / extra) : 0.0f);
			total = 0.0f;
			for (float w : widths)
				total += w;
		}
		const float tableTop = y;
		for (size_t r = 0; r < block.Rows.size(); r++)
		{
			const Style& style = r == 0 ? head : body;
			float rowH = 0.0f;
			for (size_t c = 0; c < columns && c < block.Rows[r].size(); c++)
				rowH = std::max(rowH, FlowInlines(block.Rows[r][c], 0.0f, widths[c] - pad * 2.0f, 0.0f, style, true));
			rowH = std::max(rowH, LineHeightOf(style.Font, style.Size)) + pad * 1.2f;
			if (r == 0)
				m_Boxes.push_back({ { x, y }, { total, rowH }, tokens::Raised, 0.0f });
			else if (r % 2 == 0)
				m_Boxes.push_back({ { x, y }, { total, rowH }, tokens::PanelAlt, 0.0f });
			float cx = x;
			for (size_t c = 0; c < columns; c++)
			{
				if (c < block.Rows[r].size())
				{
					const float avail = widths[c] - pad * 2.0f;
					float natural = 0.0f;
					FlowInlines(block.Rows[r][c], 0.0f, avail, 0.0f, style, true, &natural);
					float offset = 0.0f;
					if (natural < avail && block.Aligns[c] == doc::Align::Right)
						offset = avail - natural;
					else if (natural < avail && block.Aligns[c] == doc::Align::Centre)
						offset = std::round((avail - natural) * 0.5f);
					FlowInlines(block.Rows[r][c], cx + pad + offset, avail, y + pad * 0.6f, style, false);
				}
				cx += widths[c];
				if (c + 1 < columns)
					m_Boxes.push_back({ { cx, y }, { 1.0f, rowH }, tokens::Separator, 0.0f });
			}
			y += rowH;
			if (r + 1 < block.Rows.size())
				m_Boxes.push_back({ { x, y }, { total, 1.0f }, r == 0 ? tokens::BorderStrong : tokens::Separator, 0.0f });
		}
		m_Boxes.push_back({ { x, tableTop }, { total, y - tableTop }, glm::vec4(0.0f), tokens::RadiusSmall, 1.0f, tokens::Border });
		y += tokens::DocBlockGap;
	}

	void DocView::LayoutBlock(const doc::Block& block, float x, float width, float& y, bool tight) const
	{
		const Style body{ Face(FontRole::Regular), tokens::DocFontBody, tokens::DocText };
		switch (block.Kind)
		{
		case doc::BlockKind::Heading:
		{
			const float sizes[] = { tokens::DocFontH1, tokens::DocFontH2, tokens::DocFontH3, tokens::DocFontH4 };
			const float margins[] = { 22.0f, 18.0f, 12.0f, 8.0f };
			const int level = std::clamp(block.Level, 1, 4);
			if (y > 0.0f)
				y += margins[level - 1];
			m_Anchors.emplace_back(doc::Slug(doc::PlainText(block.Inlines)), std::max(0.0f, y - 8.0f));
			Style s{ Face(FontRole::SemiBold), sizes[level - 1], tokens::DocHeading };
			y += FlowInlines(block.Inlines, x, width, y, s, false);
			if (level <= 2)
			{
				y += 4.0f;
				m_Boxes.push_back({ { x, y }, { width, 1.0f }, tokens::Separator, 0.0f });
				y += 10.0f;
			}
			else
				y += 6.0f;
			break;
		}
		case doc::BlockKind::Paragraph:
		{
			Brush brush;
			glm::vec2 size{ 0.0f };
			if (block.Inlines.size() == 1 && block.Inlines[0].Kind == doc::InlineKind::Image && m_Host.Image
				&& m_Host.Image(block.Inlines[0].Target, brush, size) && size.x > 0.0f)
			{
				if (size.x > width)
					size = size * (width / size.x);
				m_Images.push_back({ { x, y }, size, brush });
				y += size.y + tokens::DocBlockGap;
				break;
			}
			y += FlowInlines(block.Inlines, x, width, y, body, false);
			y += tight ? 3.0f : tokens::DocBlockGap;
			break;
		}
		case doc::BlockKind::Code: LayoutCode(block, x, width, y); break;
		case doc::BlockKind::Table: LayoutTable(block, x, width, y); break;
		case doc::BlockKind::Rule:
			y += 6.0f;
			m_Boxes.push_back({ { x, y }, { width, 1.0f }, tokens::Separator, 0.0f });
			y += 6.0f + tokens::DocBlockGap;
			break;
		case doc::BlockKind::List:
		{
			const float indent = tokens::DocListIndent;
			const float ascent = AscentOf(body.Font, body.Size);
			for (size_t i = 0; i < block.Children.size(); i++)
			{
				const std::string marker = block.Ordered ? std::to_string(block.Start + (int)i) + "." : "\xE2\x80\xA2";
				const float w = MeasureWord(marker, body.Font, body.Size);
				Run run;
				run.Pen = { x + indent - w - 7.0f, y + ascent };
				run.Text = marker;
				run.Font = body.Font;
				run.Size = body.Size;
				run.Colour = tokens::TextMuted;
				run.Width = w;
				m_Runs.push_back(std::move(run));
				const float before = y;
				LayoutBlocks(block.Children[i].Children, x + indent, width - indent, y, block.Tight);
				if (y == before)
					y += LineHeightOf(body.Font, body.Size);
			}
			y += block.Tight ? tokens::DocBlockGap - 3.0f : 0.0f;
			break;
		}
		case doc::BlockKind::Quote:
		{
			const bool callout = !block.Info.empty();
			const glm::vec4 colour = CalloutColour(block.Info);
			const float top = y;
			const int background = (int)m_Boxes.size();
			m_Boxes.push_back({ { x, top }, { width, 0.0f }, callout ? tokens::WithAlpha(colour, tokens::DocCalloutAlpha) : glm::vec4(0.0f),
				tokens::RadiusSmall });
			m_Boxes.push_back({ { x, top }, { 3.0f, 0.0f }, colour, 0.0f });
			const float inset = callout ? 16.0f : 14.0f;
			y += callout ? 8.0f : 2.0f;
			if (callout)
			{
				Style title{ Face(FontRole::SemiBold), body.Size, colour };
				y += FlowInlines(doc::ParseInlines(CalloutTitle(block.Info)), x + inset, width - inset, y, title, false) + 2.0f;
			}
			LayoutBlocks(block.Children, x + inset, width - inset - (callout ? 10.0f : 0.0f), y, false);
			y -= tokens::DocBlockGap;
			y += callout ? 8.0f : 2.0f;
			m_Boxes[background].Size.y = y - top;
			m_Boxes[background + 1].Size.y = y - top;
			y += tokens::DocBlockGap;
			break;
		}
		case doc::BlockKind::ListItem: LayoutBlocks(block.Children, x, width, y, tight); break;
		}
	}

	void DocView::LayoutBlocks(const std::vector<doc::Block>& blocks, float x, float width, float& y, bool tight) const
	{
		for (const doc::Block& block : blocks)
			LayoutBlock(block, x, width, y, tight);
	}

	void DocView::Relayout(float width) const
	{
		m_Runs.clear();
		m_Boxes.clear();
		m_Images.clear();
		m_Links.clear();
		m_LinkRects.clear();
		m_Anchors.clear();
		m_BlockTops.clear();
		m_NextCodeSpan = 0;
		float y = 0.0f;
		for (const doc::Block& block : m_Document.Blocks)
		{
			m_BlockTops.push_back(y);
			LayoutBlock(block, 0.0f, width, y, false);
		}
		m_ContentHeight = y + tokens::DocPad * 2.0f;
		m_LayoutWidth = width;
	}

	// ---- paint --------------------------------------------------------------

	bool DocView::ThumbRect(glm::vec2& pos, glm::vec2& size) const
	{
		if (m_ContentHeight <= m_ViewHeight || m_ViewHeight <= 0.0f)
			return false;
		const float h = std::max(24.0f, m_ViewHeight * m_ViewHeight / m_ContentHeight);
		const float t = MaxScroll() > 0.0f ? m_Scroll / MaxScroll() : 0.0f;
		pos = { m_Origin.x + m_ViewSize.x - kScrollbarWidth - 3.0f, m_Origin.y + t * (m_ViewHeight - h) };
		size = { kScrollbarWidth, h };
		return true;
	}

	int DocView::OnPaint(const PaintArgs& args, const Geometry& geom, DrawList& dl, int layerId, const WidgetStyleContext&) const
	{
		m_Origin = geom.LocalToAbsolute({ 0.0f, 0.0f });
		m_ViewSize = geom.LocalSize;
		m_ViewHeight = m_ViewSize.y;
		const float width = std::max(80.0f, std::min(m_ViewSize.x - tokens::DocPad * 2.0f - kScrollbarGutter, tokens::DocMeasure));
		if (width != m_LayoutWidth)
			Relayout(width);
		if (m_PendingBlock >= 0 && m_PendingBlock < (int)m_BlockTops.size())
			m_Scroll = m_BlockTops[m_PendingBlock] + m_PendingOffset;
		m_PendingBlock = -1;
		if (!m_PendingAnchor.empty())
		{
			for (const auto& [anchor, y] : m_Anchors)
				if (anchor == m_PendingAnchor)
					m_Scroll = y;
			m_PendingAnchor.clear();
		}
		ClampScroll();

		const int clip = dl.PushClip({ m_Origin.x, m_Origin.y, m_ViewSize.x, m_ViewSize.y }, args.InheritedClipIndex);
		dl.AddBox(layerId, m_Origin, m_ViewSize, tokens::Panel, clip);
		const glm::vec2 base = m_Origin + glm::vec2(tokens::DocPad, tokens::DocPad - m_Scroll);
		const float visibleTop = m_Scroll - tokens::DocPad - 64.0f;
		const float visibleBottom = m_Scroll + m_ViewHeight + 64.0f;
		auto visible = [&](float top, float bottom) { return bottom >= visibleTop && top <= visibleBottom; };

		for (const Box& box : m_Boxes)
		{
			if (!visible(box.Pos.y, box.Pos.y + box.Size.y) || box.Size.x <= 0.0f || box.Size.y <= 0.0f)
				continue;
			const glm::vec2 pos = glm::round(base + box.Pos);
			if (box.Radius > 0.0f || box.OutlineWidth > 0.0f)
			{
				const float r[4] = { box.Radius, box.Radius, box.Radius, box.Radius };
				dl.AddRoundedBox(layerId + 1, pos, box.Size, box.Colour, r, box.OutlineWidth, box.OutlineColour, clip);
			}
			else
				dl.AddBox(layerId + 1, pos, box.Size, box.Colour, clip);
		}
		for (const ImageRun& image : m_Images)
			if (visible(image.Pos.y, image.Pos.y + image.Size.y))
				dl.AddImage(layerId + 2, glm::round(base + image.Pos), image.Size, image.ImageBrush, clip);

		std::vector<int> boxClips(m_Boxes.size(), -1);
		for (const Run& run : m_Runs)
		{
			if (!visible(run.Pen.y - run.Size * 1.5f, run.Pen.y + run.Size))
				continue;
			int runClip = clip;
			if (run.Clip >= 0)
			{
				if (boxClips[run.Clip] < 0)
				{
					const Box& b = m_Boxes[run.Clip];
					const glm::vec2 p = base + b.Pos;
					boxClips[run.Clip] = dl.PushClip({ p.x + 1.0f, p.y, b.Size.x - tokens::DocCodePad * 0.5f, b.Size.y }, clip);
				}
				runClip = boxClips[run.Clip];
			}
			float x = base.x + run.Pen.x;
			const float baseline = std::round(base.y + run.Pen.y);
			uint32_t prev = 0;
			for (size_t at = 0; at < run.Text.size();)
			{
				const uint32_t cp = Decode(run.Text, at);
				if (prev && m_FontCache)
					x += m_FontCache->GetKerning(run.Font, prev, cp, run.Size);
				if (const GlyphInfo* g = m_FontCache ? m_FontCache->GetGlyph(run.Font, cp, run.Size) : nullptr)
				{
					if (cp != ' ')
						AddGlyph(dl, layerId + 3, { std::round(x), baseline }, *g, run.Colour, runClip);
					x += g->AdvanceX;
				}
				prev = cp;
			}
			if (run.Link >= 0 && run.Link == m_HoveredLink)
				dl.AddBox(layerId + 3, { std::round(base.x + run.Pen.x), baseline + 2.0f }, { std::round(run.Width), 1.0f }, run.Colour, runClip);
		}

		glm::vec2 thumbPos, thumbSize;
		if (ThumbRect(thumbPos, thumbSize))
		{
			const float r[4] = { 2.0f, 2.0f, 2.0f, 2.0f };
			dl.AddRoundedBox(layerId + 4, thumbPos, thumbSize, tokens::CodeScrollThumb, r, 0.0f, tokens::CodeScrollThumb, clip);
		}
		return layerId + 5;
	}

	// ---- input --------------------------------------------------------------

	int DocView::LinkAt(const glm::vec2& absolute) const
	{
		const glm::vec2 p = absolute - (m_Origin + glm::vec2(tokens::DocPad, tokens::DocPad - m_Scroll));
		for (const LinkRect& r : m_LinkRects)
			if (p.x >= r.Pos.x && p.x < r.Pos.x + r.Size.x && p.y >= r.Pos.y && p.y < r.Pos.y + r.Size.y)
				return r.Link;
		return -1;
	}

	Reply DocView::OnMouseButtonDown(const glm::vec2& pos, int button)
	{
		if (button != 0)
			return Reply::Unhandled();
		glm::vec2 thumbPos, thumbSize;
		if (ThumbRect(thumbPos, thumbSize) && pos.x >= thumbPos.x - 4.0f && pos.x < thumbPos.x + thumbSize.x + 4.0f)
		{
			m_DraggingThumb = true;
			m_DragGrab = pos.y >= thumbPos.y && pos.y < thumbPos.y + thumbSize.y ? pos.y - thumbPos.y : thumbSize.y * 0.5f;
			OnMouseMove(pos);
			return Reply::Handled_().CaptureMouse().SetFocus();
		}
		m_PressedLink = LinkAt(pos);
		return Reply::Handled_().SetFocus();
	}

	Reply DocView::OnMouseButtonUp(const glm::vec2& pos, int button)
	{
		if (button != 0)
			return Reply::Unhandled();
		if (m_DraggingThumb)
		{
			m_DraggingThumb = false;
			return Reply::Handled_().ReleaseCapture();
		}
		const int link = LinkAt(pos);
		if (link >= 0 && link == m_PressedLink && link < (int)m_Links.size() && m_Host.OnLink)
		{
			const LinkTarget target = m_Links[link]; // the handler may replace the page
			m_PressedLink = -1;
			m_Host.OnLink(target.Target, target.Doc);
			return Reply::Handled_();
		}
		m_PressedLink = -1;
		return Reply::Handled_();
	}

	Reply DocView::OnMouseMove(const glm::vec2& pos)
	{
		if (m_DraggingThumb)
		{
			glm::vec2 thumbPos, thumbSize;
			if (ThumbRect(thumbPos, thumbSize) && m_ViewHeight > thumbSize.y)
			{
				const float t = (pos.y - m_DragGrab - m_Origin.y) / (m_ViewHeight - thumbSize.y);
				m_Scroll = std::clamp(t, 0.0f, 1.0f) * MaxScroll();
			}
			return Reply::Handled_();
		}
		m_HoveredLink = LinkAt(pos);
		SetCursor(m_HoveredLink >= 0 ? CursorType::Hand : CursorType::Default);
		return Reply::Unhandled();
	}

	Reply DocView::OnMouseWheel(const glm::vec2&, float delta)
	{
		if (m_ContentHeight <= m_ViewHeight)
			return Reply::Unhandled();
		m_Scroll -= delta * kWheelStep;
		ClampScroll();
		return Reply::Handled_();
	}

	Reply DocView::OnMouseLeave()
	{
		m_HoveredLink = -1;
		SetCursor(CursorType::Default);
		return Reply::Unhandled();
	}

	Reply DocView::OnKeyDown(int key, int)
	{
		const float page = std::max(40.0f, m_ViewHeight - 48.0f);
		switch (key)
		{
		case kUp: m_Scroll -= kWheelStep; break;
		case kDown: m_Scroll += kWheelStep; break;
		case kPageUp: m_Scroll -= page; break;
		case kPageDown: m_Scroll += page; break;
		case kHome: m_Scroll = 0.0f; break;
		case kEnd: m_Scroll = MaxScroll(); break;
		default: return Reply::Unhandled();
		}
		ClampScroll();
		return Reply::Handled_();
	}

} // namespace wisp
