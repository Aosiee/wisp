#include "Wisp/Components/CodeEditor.h"
#include "Wisp/Application.h"
#include "Wisp/Clipboard.h"
#include "Wisp/DrawList.h"
#include "Wisp/Icons.h"
#include "Wisp/StyleTokens.h"

#include <algorithm>
#include <cctype>
#include <cmath>

namespace wisp {

	namespace {

		// GLFW key codes (Wisp passes the raw ints).
		enum Key : int
		{
			kSpace = 32, kSlash = 47, kA = 65, kC = 67, kD = 68, kF = 70, kG = 71, kH = 72, kS = 83, kV = 86, kX = 88, kY = 89, kZ = 90,
			kBracketLeft = 91, kBracketRight = 93,
			kEscape = 256, kEnter = 257, kTab = 258, kBackspace = 259, kDelete = 261,
			kRight = 262, kLeft = 263, kDown = 264, kUp = 265, kPageUp = 266, kPageDown = 267, kHome = 268, kEnd = 269,
			kKeypadEnter = 335, kF8 = 297, kMinus = 45, kZero = 48, kEqual = 61, kKeypadSubtract = 333, kKeypadAdd = 334,
		};
		constexpr float kDefaultFontSize = 13.0f; // Ctrl+0
		constexpr int kShift = 1;
		constexpr int kCtrl = 2;

		constexpr float kMinimapWidth = 84.0f;
		constexpr float kMinimapLine = 2.0f;   // px per line
		constexpr float kMinimapChar = 1.0f;   // px per column
		constexpr float kFoldColumn = 14.0f;
		constexpr float kTextPad = 6.0f;

		glm::vec4 ColourOf(code::TokenKind kind)
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
			default: return tokens::SyntaxPlain;
			}
		}

		bool IsWordChar(unsigned char c) { return std::isalnum(c) || c == '_'; }

		uint32_t DecodeAt(const std::string& line, int col, int& length)
		{
			length = std::max(1, code::NextCodePointLength(line, col));
			const unsigned char* p = (const unsigned char*)line.data() + col;
			switch (length)
			{
			case 2: return ((p[0] & 0x1Fu) << 6) | (p[1] & 0x3Fu);
			case 3: return ((p[0] & 0x0Fu) << 12) | ((p[1] & 0x3Fu) << 6) | (p[2] & 0x3Fu);
			case 4: return ((p[0] & 0x07u) << 18) | ((p[1] & 0x3Fu) << 12) | ((p[2] & 0x3Fu) << 6) | (p[3] & 0x3Fu);
			default: return p[0];
			}
		}

	} // namespace

	CodeEditor::CodeEditor(FontCache* fontCache, FontHandle font, float fontSize, std::shared_ptr<const code::ITokenizer> tokenizer)
		: m_FontCache(fontCache)
		, m_Font(font)
		, m_FontSize(fontSize)
		, m_Tokenizer(std::move(tokenizer))
	{
		SetSupportsFocus(true);
		SetCursor(CursorType::TextEditBeam);
	}

	void CodeEditor::SetText(const std::string& text)
	{
		m_Model->SetText(text);
		m_ScrollX = m_ScrollY = 0.0f;
		m_TokensValidTo = -1;
		m_TokensVersion = m_Model->Doc().Version();
		m_Markers.clear();
	}

	void CodeEditor::SetModel(std::shared_ptr<code::EditorModel> model, glm::vec2 scroll)
	{
		m_Model = model ? std::move(model) : std::make_shared<code::EditorModel>();
		m_ScrollX = scroll.x;
		m_ScrollY = scroll.y;
		m_TokensValidTo = -1;
		m_TokensVersion = m_Model->Doc().Version();
		m_Markers.clear();
		m_Drag = Drag::None;
	}

	void CodeEditor::SetFontSize(float size)
	{
		m_FontSize = std::clamp(size, kMinFontSize, kMaxFontSize);
		m_RevealCaret = true;
	}

	void CodeEditor::Zoom(float size)
	{
		const float before = m_FontSize;
		SetFontSize(size);
		if (m_FontSize != before && m_OnZoom)
			m_OnZoom(m_FontSize);
	}

	bool CodeEditor::GoToMarker(bool backwards)
	{
		if (m_Markers.empty())
			return false;
		std::vector<code::TextPos> starts;
		for (const Marker& marker : m_Markers)
			starts.push_back({ marker.Line, std::max(0, marker.Col) });
		std::sort(starts.begin(), starts.end());
		starts.erase(std::unique(starts.begin(), starts.end()), starts.end());
		const code::TextPos caret = m_Model->PrimaryCaret().Pos;
		code::TextPos target = backwards ? starts.back() : starts.front();
		if (backwards)
		{
			for (auto it = starts.rbegin(); it != starts.rend(); ++it)
				if (*it < caret)
				{
					target = *it;
					break;
				}
		}
		else
		{
			for (const code::TextPos& start : starts)
				if (caret < start)
				{
					target = start;
					break;
				}
		}
		m_Model->SetCaret(target, false);
		m_Completion.Open = false;
		Touch();
		return true;
	}

	bool CodeEditor::OccurrenceWord(std::string& word) const
	{
		const code::Caret& caret = m_Model->PrimaryCaret();
		if (m_Model->Carets().size() != 1 || caret.HasSelection() || !m_Highlight.empty())
			return false;
		const std::string& line = m_Model->Doc().Line(caret.Pos.Line);
		int start = caret.Pos.Col, end = caret.Pos.Col;
		while (start > 0 && IsWordChar((unsigned char)line[(size_t)start - 1]))
			start--;
		while (end < (int)line.size() && IsWordChar((unsigned char)line[(size_t)end]))
			end++;
		if (end - start < 2 || std::isdigit((unsigned char)line[(size_t)start]))
			return false;
		word = line.substr((size_t)start, (size_t)(end - start));
		return true;
	}

	// ---- layout / tokens ----------------------------------------------------

	void CodeEditor::UpdateLayout(const Geometry& geom) const
	{
		Layout& layout = m_Layout;
		layout.Origin = geom.LocalToAbsolute({ 0.0f, 0.0f });
		layout.Size = geom.LocalSize;
		if (m_FontCache)
		{
			const FontMetrics metrics = m_FontCache->GetMetrics(m_Font, m_FontSize);
			layout.LineHeight = std::round(metrics.LineHeight + 3.0f);
			layout.Ascent = std::round(metrics.Ascent + 1.5f);
			if (const GlyphInfo* m = m_FontCache->GetGlyph(m_Font, 'M', m_FontSize))
				layout.CharWidth = std::max(1.0f, std::round(m->AdvanceX));
		}
		int digits = 1;
		for (int n = m_Model->Doc().LineCount(); n >= 10; n /= 10)
			digits++;
		layout.GutterWidth = std::max(3, digits) * layout.CharWidth + kFoldColumn + 18.0f;
		const bool minimap = m_ShowMinimap && layout.Size.x > 420.0f;
		layout.MinimapLeft = layout.Origin.x + layout.Size.x - (minimap ? kMinimapWidth : 0.0f);
		layout.TextLeft = layout.Origin.x + layout.GutterWidth + kTextPad;
		layout.TextRight = layout.MinimapLeft - (minimap ? 4.0f : 12.0f); // no minimap: the scrollbar's strip
	}

	void CodeEditor::UpdateTokens(int lastLine) const
	{
		const code::TextDocument& doc = m_Model->Doc();
		if (m_TokensVersion != doc.Version())
		{
			// The edit invalidates from its first line down (the carried
			// state may change everything below it).
			const int first = const_cast<code::TextDocument&>(doc).TakeFirstChangedLine();
			m_TokensValidTo = std::min(m_TokensValidTo, first - 1);
			m_TokensVersion = doc.Version();
		}
		const int count = doc.LineCount();
		m_Tokens.resize((size_t)count);
		m_TokenState.resize((size_t)count);
		m_BracketDepth.resize((size_t)count);
		lastLine = std::min(lastLine, count - 1);
		if (!m_Tokenizer)
		{
			for (auto& line : m_Tokens)
				line.clear();
			m_TokensValidTo = count - 1;
			return;
		}
		for (int line = m_TokensValidTo + 1; line <= lastLine; line++)
		{
			const uint32_t state = line == 0 ? 0 : m_TokenState[(size_t)line];
			m_TokenState[(size_t)line] = state;
			std::vector<code::Token>& tokens = m_Tokens[(size_t)line];
			tokens.clear();
			const uint32_t next = m_Tokenizer->TokenizeLine(doc.Line(line), state, tokens);
			if (line + 1 < count)
				m_TokenState[(size_t)line + 1] = next;
			if (line == 0)
				m_BracketDepth[0] = 0;
			int depth = m_BracketDepth[(size_t)line];
			const std::string& text = doc.Line(line);
			for (const code::Token& token : tokens)
				if (token.Kind == code::TokenKind::Operator)
					for (int c = token.Start; c < token.Start + token.Length; c++)
					{
						const char ch = text[(size_t)c];
						depth += (ch == '(' || ch == '[' || ch == '{') ? 1 : (ch == ')' || ch == ']' || ch == '}') ? -1 : 0;
					}
			if (line + 1 < count)
				m_BracketDepth[(size_t)line + 1] = std::max(0, depth);
			m_TokensValidTo = line;
		}
	}

	code::TextPos CodeEditor::PosAt(const glm::vec2& absolute) const
	{
		const Layout& layout = m_Layout;
		const int visible = (int)std::floor((absolute.y - layout.Origin.y + m_ScrollY) / layout.LineHeight);
		const int line = m_Model->LineForVisible(std::max(0, visible));
		const float x = absolute.x - layout.TextLeft + m_ScrollX;
		const int column = std::max(0, (int)std::round(x / layout.CharWidth));
		return { line, code::EditorModel::ByteForVisualColumn(m_Model->Doc().Line(line), column) };
	}

	void CodeEditor::ClampScroll() const
	{
		const float content = (float)(m_Model->VisibleLineCount() - 1) * m_Layout.LineHeight;
		m_ScrollY = std::clamp(m_ScrollY, 0.0f, std::max(0.0f, content));
		m_ScrollX = std::max(0.0f, m_ScrollX);
	}

	void CodeEditor::ScrollToCaret() const
	{
		const Layout& layout = m_Layout;
		const code::TextPos pos = m_Model->PrimaryCaret().Pos;
		const float y = (float)m_Model->VisibleForLine(pos.Line) * layout.LineHeight;
		const float viewH = std::max(layout.LineHeight, layout.Size.y - layout.LineHeight);
		if (y < m_ScrollY)
			m_ScrollY = y;
		else if (y > m_ScrollY + viewH)
			m_ScrollY = y - viewH;
		const float x = (float)code::EditorModel::VisualColumn(m_Model->Doc().Line(pos.Line), pos.Col) * layout.CharWidth;
		const float viewW = std::max(layout.CharWidth * 4.0f, layout.TextRight - layout.TextLeft - layout.CharWidth * 2.0f);
		if (x < m_ScrollX)
			m_ScrollX = std::max(0.0f, x - layout.CharWidth * 4.0f);
		else if (x > m_ScrollX + viewW)
			m_ScrollX = x - viewW + layout.CharWidth * 4.0f;
		ClampScroll();
	}

	float CodeEditor::MinimapFirstLine(int visibleCount) const
	{
		const float capacity = m_Layout.Size.y / kMinimapLine;
		if ((float)visibleCount <= capacity)
			return 0.0f;
		const float content = std::max(1.0f, (float)(visibleCount - 1) * m_Layout.LineHeight);
		return (m_ScrollY / content) * ((float)visibleCount - capacity);
	}

	// ---- paint --------------------------------------------------------------

	int CodeEditor::OnPaint(const PaintArgs& args, const Geometry& geom, DrawList& dl, int layerId, const WidgetStyleContext&) const
	{
		UpdateLayout(geom);
		const Layout& L = m_Layout;
		if (m_RevealCaret)
		{
			ScrollToCaret();
			m_RevealCaret = false;
		}
		ClampScroll();

		const int clip = dl.PushClip({ L.Origin.x, L.Origin.y, L.Size.x, L.Size.y }, args.InheritedClipIndex);
		const int textClip = dl.PushClip({ L.Origin.x + L.GutterWidth, L.Origin.y, L.TextRight - L.Origin.x - L.GutterWidth, L.Size.y }, clip);
		dl.AddBox(layerId, L.Origin, L.Size, tokens::CodeBackground, clip);
		dl.AddBox(layerId, L.Origin, { L.GutterWidth, L.Size.y }, tokens::CodeGutter, clip);

		const code::TextDocument& doc = m_Model->Doc();
		const int visibleCount = m_Model->VisibleLineCount();
		const int firstVisible = std::max(0, (int)std::floor(m_ScrollY / L.LineHeight));
		const int lastVisible = std::min(visibleCount - 1, (int)std::ceil((m_ScrollY + L.Size.y) / L.LineHeight));
		const float minimapFirst = MinimapFirstLine(visibleCount);
		const int minimapLast = std::min(visibleCount - 1, (int)(minimapFirst + L.Size.y / kMinimapLine) + 1);
		UpdateTokens(m_Model->LineForVisible(std::max(lastVisible, L.MinimapLeft < L.Origin.x + L.Size.x ? minimapLast : 0)));

		const code::Caret& primary = m_Model->PrimaryCaret();
		const bool focused = m_HasFocus;
		const float now = UIApplication::GetAnimTime();
		const bool caretOn = focused && (now - m_LastActivity < 0.6f || std::fmod(now, 1.06f) < 0.6f);
		code::TextPos bracketOpen, bracketClose;
		const bool brackets = focused && m_Model->MatchingBracket(bracketOpen, bracketClose);

		auto xOf = [&](int line, int col) {
			return L.TextLeft - m_ScrollX + (float)code::EditorModel::VisualColumn(doc.Line(line), col) * L.CharWidth;
		};
		std::string occurrence;
		const bool hasOccurrence = focused && OccurrenceWord(occurrence);
		int activeGuideLevel = -1, activeGuideStart = 0, activeGuideEnd = -1;
		for (const code::FoldRange& fold : m_Model->FoldRanges())
			if (fold.StartLine <= primary.Pos.Line && fold.EndLine >= primary.Pos.Line && fold.StartLine >= activeGuideStart)
			{
				activeGuideStart = fold.StartLine;
				activeGuideEnd = fold.EndLine;
				activeGuideLevel = code::EditorModel::IndentWidth(doc.Line(fold.StartLine));
			}

		for (int v = firstVisible; v <= lastVisible; v++)
		{
			const int line = m_Model->LineForVisible(v);
			const std::string& text = doc.Line(line);
			const float y = L.Origin.y + (float)v * L.LineHeight - m_ScrollY;

			if (line == primary.Pos.Line && !primary.HasSelection())
				dl.AddBox(layerId + 1, { L.Origin.x + L.GutterWidth, y }, { L.TextRight - L.Origin.x - L.GutterWidth, L.LineHeight }, tokens::CodeCurrentLine, clip);

			// Find matches.
			if (!m_Highlight.empty())
				for (size_t at = 0; (at = FindHighlight(text, at)) != std::string::npos; at += m_Highlight.size())
				{
					const float x0 = xOf(line, (int)at);
					const float x1 = xOf(line, (int)(at + m_Highlight.size()));
					dl.AddBox(layerId + 2, { x0, y }, { x1 - x0, L.LineHeight }, tokens::CodeFindMatch, textClip);
				}

			// The other occurrences of the identifier under the caret.
			if (hasOccurrence)
				for (size_t at = text.find(occurrence); at != std::string::npos; at = text.find(occurrence, at + 1))
				{
					if ((at > 0 && IsWordChar((unsigned char)text[at - 1]))
						|| (at + occurrence.size() < text.size() && IsWordChar((unsigned char)text[at + occurrence.size()])))
						continue;
					const float x0 = xOf(line, (int)at);
					const float x1 = xOf(line, (int)(at + occurrence.size()));
					const float r[4] = { 2.0f, 2.0f, 2.0f, 2.0f };
					dl.AddRoundedBox(layerId + 2, { x0, y + 1.0f }, { x1 - x0, L.LineHeight - 2.0f }, tokens::CodeOccurrence, r, 0.0f, tokens::CodeOccurrence, textClip);
				}

			// Selections (a line the selection continues past shows one more cell).
			for (const code::Caret& caret : m_Model->Carets())
			{
				if (!caret.HasSelection())
					continue;
				const code::TextRange range = caret.Selection();
				if (line < range.Start.Line || line > range.End.Line)
					continue;
				const int from = line == range.Start.Line ? range.Start.Col : 0;
				const int to = line == range.End.Line ? range.End.Col : (int)text.size();
				const float x0 = xOf(line, from);
				const float x1 = xOf(line, to) + (line < range.End.Line ? L.CharWidth * 0.5f : 0.0f);
				const glm::vec4 tint = focused ? tokens::CodeSelection : tokens::SelectionInactive;
				dl.AddBox(layerId + 2, { x0, y }, { std::max(1.0f, x1 - x0), L.LineHeight }, tint, textClip);
			}

			// Indentation guides on non-blank lines; the caret's block's own
			// guide (under its header) brighter.
			const int indent = code::EditorModel::IndentWidth(text);
			for (int level = code::EditorModel::kTabWidth; level < indent; level += code::EditorModel::kTabWidth)
			{
				const float gx = L.TextLeft - m_ScrollX + (float)level * L.CharWidth;
				const bool active = level == activeGuideLevel && line > activeGuideStart && line <= activeGuideEnd;
				dl.AddBox(layerId + 2, { std::round(gx), y }, { 1.0f, L.LineHeight }, active ? tokens::CodeIndentGuideActive : tokens::CodeIndentGuide, textClip);
			}

			PaintLineText(dl, layerId + 3, line, y, textClip);

			// A folded header shows an ellipsis box after its text.
			if (m_Model->IsFolded(line))
			{
				const float x = xOf(line, (int)text.size()) + L.CharWidth;
				const float r[4] = { tokens::RadiusSmall, tokens::RadiusSmall, tokens::RadiusSmall, tokens::RadiusSmall };
				dl.AddRoundedBox(layerId + 2, { x, y + 3.0f }, { L.CharWidth * 3.0f, L.LineHeight - 6.0f }, tokens::Control, r, 0.0f, tokens::Control, textClip);
				if (const GlyphInfo* dot = m_FontCache ? m_FontCache->GetGlyph(m_Font, 0x2026, m_FontSize) : nullptr)
					AddGlyph(dl, layerId + 3, { std::round(x + L.CharWidth), std::round(y + L.Ascent) }, *dot, tokens::TextMuted, textClip);
			}

			// Diagnostics: a gutter dot on the first line, a wavy underline
			// under the range (a whole-line marker from the first non-blank).
			for (int pass = 0; pass < 2; pass++)
				for (const Marker& marker : m_Markers)
				{
					// Warnings first, so an error's colour wins the dot.
					if (marker.Error != (pass == 1))
						continue;
					const int endLine = marker.Col >= 0 && marker.EndLine >= marker.Line ? marker.EndLine : marker.Line;
					if (line < marker.Line || line > endLine)
						continue;
					const glm::vec4 tint = marker.Error ? tokens::Error : tokens::Warning;
					if (line == marker.Line)
					{
						const float r[4] = { 3.0f, 3.0f, 3.0f, 3.0f };
						dl.AddRoundedBox(layerId + 4 + pass, { L.Origin.x + 5.0f, y + L.LineHeight * 0.5f - 3.0f }, { 6.0f, 6.0f }, tint, r, 0.0f, tint, clip);
					}
					const size_t firstText = text.find_first_not_of(" \t");
					int from = firstText == std::string::npos ? (int)text.size() : (int)firstText;
					int to = (int)text.size();
					if (marker.Col >= 0)
					{
						if (line == marker.Line)
							from = std::min(marker.Col, (int)text.size());
						if (line == endLine)
							to = std::min(marker.EndCol >= 0 ? marker.EndCol : marker.Col, (int)text.size());
					}
					const float x0 = xOf(line, from);
					const float x1 = std::max(x0 + L.CharWidth, xOf(line, std::max(from, to)));
					const float base = y + L.LineHeight - 2.5f;
					for (float x = x0; x < x1; x += 2.0f)
					{
						const float up = std::fmod(std::floor((x - x0) / 2.0f), 2.0f) == 0.0f ? -1.0f : 1.0f;
						dl.AddLine(layerId + 4 + pass, { x, base + up }, { std::min(x1, x + 2.0f), base - up }, 1.0f, tint, textClip);
					}
				}

			// The line's first diagnostic after its code (an error over a
			// warning), on a faint band of its colour; whole characters only.
			{
				const Marker* lead = nullptr;
				for (const Marker& marker : m_Markers)
					if (marker.Line == line && (!lead || (marker.Error && !lead->Error)))
						lead = &marker;
				if (lead && !lead->Message.empty())
				{
					const glm::vec4 tint = lead->Error ? tokens::Error : tokens::Warning;
					dl.AddBox(layerId + 1, { L.Origin.x + L.GutterWidth, y }, { L.TextRight - L.Origin.x - L.GutterWidth, L.LineHeight },
						tokens::WithAlpha(tint, tokens::CodeDiagnosticBandAlpha), clip);
					std::string message = lead->Message.substr(0, lead->Message.find('\n'));
					const float x0 = std::max(xOf(line, (int)text.size()) + L.CharWidth * 3.0f, L.TextLeft);
					const int fit = (int)((L.TextRight - x0) / L.CharWidth);
					if (fit > 8)
					{
						size_t glyphs = 0, cut = 0;
						for (; cut < message.size(); cut++)
							if (((unsigned char)message[cut] & 0xC0u) != 0x80u && ++glyphs > (size_t)fit - 1)
								break;
						if (cut < message.size())
							message = message.substr(0, cut) + "\xE2\x80\xA6";
						PaintRun(dl, layerId + 3, x0, std::round(y + L.Ascent), message, tokens::WithAlpha(tint, tokens::CodeInlineDiagnosticAlpha), textClip);
					}
				}
			}

			// The gutter: the line number and the fold chevron.
			PaintLineNumber(dl, layerId + 3, line, y, line == primary.Pos.Line ? tokens::CodeLineNumberActive : tokens::CodeLineNumber, clip);
			if (m_Model->FoldStartingAt(line) && (m_GutterHovered || m_Model->IsFolded(line)))
			{
				const uint32_t chevron = m_Model->IsFolded(line) ? icons::cp::ChevronRight : icons::cp::ChevronDown;
				if (const GlyphInfo* glyph = m_FontCache ? m_FontCache->GetGlyph(m_Font, chevron, m_FontSize * 0.85f) : nullptr)
					AddGlyph(dl, layerId + 3, { std::round(L.Origin.x + L.GutterWidth - kFoldColumn - 2.0f), std::round(y + L.Ascent) }, *glyph, tokens::CodeFoldMarker, clip);
			}

			// The matching bracket pair.
			if (brackets)
				for (const code::TextPos& at : { bracketOpen, bracketClose })
					if (at.Line == line)
					{
						const float r[4] = { 2.0f, 2.0f, 2.0f, 2.0f };
						dl.AddRoundedBox(layerId + 2, { xOf(line, at.Col), y + 1.0f }, { L.CharWidth, L.LineHeight - 2.0f },
							glm::vec4(0.0f), r, 1.0f, tokens::CodeBracketMatch, textClip);
					}

			// The carets.
			if (caretOn)
				for (const code::Caret& caret : m_Model->Carets())
					if (caret.Pos.Line == line)
						dl.AddBox(layerId + 5, { std::round(xOf(line, caret.Pos.Col)), y + 1.0f }, { 2.0f, L.LineHeight - 2.0f }, tokens::CodeCaret, textClip);
		}

		// Sticky scroll: the enclosing block headers pinned at the top.
		m_StickyLines = StickyHeaders();
		for (size_t i = 0; i < m_StickyLines.size(); i++)
		{
			const int line = m_StickyLines[i];
			const float y = L.Origin.y + (float)i * L.LineHeight;
			dl.AddBox(layerId + 6, { L.Origin.x, y }, { L.TextRight - L.Origin.x, L.LineHeight }, tokens::CodeBackground, clip);
			dl.AddBox(layerId + 6, { L.Origin.x, y }, { L.TextRight - L.Origin.x, L.LineHeight }, tokens::CodeCurrentLine, clip);
			PaintLineNumber(dl, layerId + 6, line, y, tokens::CodeLineNumber, clip);
			PaintLineText(dl, layerId + 6, line, y, textClip);
		}
		if (!m_StickyLines.empty())
		{
			const float bottom = L.Origin.y + (float)m_StickyLines.size() * L.LineHeight;
			dl.AddBox(layerId + 6, { L.Origin.x, bottom }, { L.TextRight - L.Origin.x, 1.0f }, tokens::BorderStrong, clip);
			dl.AddBox(layerId + 6, { L.Origin.x, bottom + 1.0f }, { L.TextRight - L.Origin.x, 3.0f }, tokens::WithAlpha(tokens::Shadow, 0.25f), clip);
		}

		// The minimap: one 2 px row per visible line, a box per token.
		if (L.MinimapLeft < L.Origin.x + L.Size.x)
		{
			dl.AddBox(layerId + 1, { L.MinimapLeft, L.Origin.y }, { kMinimapWidth, L.Size.y }, tokens::CodeMinimap, clip);
			const int first = (int)minimapFirst;
			for (int v = first; v <= minimapLast; v++)
			{
				const int line = m_Model->LineForVisible(v);
				const std::string& text = doc.Line(line);
				const float y = L.Origin.y + ((float)v - minimapFirst) * kMinimapLine;
				for (const code::Token& token : m_Tokens[(size_t)line])
				{
					const int c0 = code::EditorModel::VisualColumn(text, token.Start);
					const int c1 = code::EditorModel::VisualColumn(text, token.Start + token.Length);
					const float x = L.MinimapLeft + 4.0f + (float)c0 * kMinimapChar;
					if (x > L.Origin.x + L.Size.x - 2.0f)
						break;
					glm::vec4 tint = ColourOf(token.Kind);
					tint.a = 0.55f;
					dl.AddBox(layerId + 2, { x, y }, { std::max(1.0f, (float)(c1 - c0) * kMinimapChar), kMinimapLine * 0.75f }, tint, clip);
				}
			}
			// The find matches, as accent marks on their rows.
			if (!m_Highlight.empty())
				for (int v = first; v <= minimapLast; v++)
				{
					const int line = m_Model->LineForVisible(v);
					const std::string& text = doc.Line(line);
					for (size_t at = 0; (at = FindHighlight(text, at)) != std::string::npos; at += m_Highlight.size())
					{
						const float x = L.MinimapLeft + 4.0f + (float)code::EditorModel::VisualColumn(text, (int)at) * kMinimapChar;
						const float y = L.Origin.y + ((float)v - minimapFirst) * kMinimapLine;
						dl.AddBox(layerId + 3, { x - 1.0f, y - 1.0f }, { std::max(3.0f, (float)m_Highlight.size() * kMinimapChar + 2.0f), kMinimapLine + 2.0f },
							tokens::WithAlpha(tokens::Warning, 0.85f), clip);
					}
				}
			// The diagnostics, as ticks on the minimap's right edge.
			for (const Marker& marker : m_Markers)
			{
				const float ty = L.Origin.y + ((float)m_Model->VisibleForLine(marker.Line) - minimapFirst) * kMinimapLine;
				if (ty < L.Origin.y - 2.0f || ty > L.Origin.y + L.Size.y)
					continue;
				dl.AddBox(layerId + 3, { L.MinimapLeft + kMinimapWidth - 6.0f, ty - 1.0f }, { 5.0f, 3.0f }, marker.Error ? tokens::Error : tokens::Warning, clip);
			}
			const float viewTop = L.Origin.y + (m_ScrollY / L.LineHeight - minimapFirst) * kMinimapLine;
			dl.AddBox(layerId + 3, { L.MinimapLeft, viewTop }, { kMinimapWidth, L.Size.y / L.LineHeight * kMinimapLine }, tokens::CodeMinimapView, clip);
		}

		// No minimap (a narrow editor): a thin scrollbar thumb, the
		// diagnostics ticked beside it.
		if (L.MinimapLeft >= L.Origin.x + L.Size.x)
		{
			const float content = (float)visibleCount * L.LineHeight + L.Size.y - L.LineHeight;
			if (content > L.Size.y)
			{
				const float thumbH = std::max(18.0f, L.Size.y * L.Size.y / content);
				const float thumbY = L.Origin.y + (L.Size.y - thumbH) * (m_ScrollY / std::max(1.0f, content - L.Size.y));
				const float tr[4] = { 2.0f, 2.0f, 2.0f, 2.0f };
				dl.AddRoundedBox(layerId + 5, { L.Origin.x + L.Size.x - 8.0f, thumbY }, { 5.0f, thumbH }, tokens::CodeScrollThumb, tr, 0.0f, tokens::CodeScrollThumb, clip);
			}
			for (const Marker& marker : m_Markers)
			{
				const float ty = L.Origin.y + L.Size.y * (float)m_Model->VisibleForLine(marker.Line) / (float)std::max(1, visibleCount);
				dl.AddBox(layerId + 5, { L.Origin.x + L.Size.x - 10.0f, ty - 1.0f }, { 9.0f, 3.0f }, marker.Error ? tokens::Error : tokens::Warning, clip);
			}
		}

		// The border focus line.
		if (focused)
			dl.AddBox(layerId + 5, { L.Origin.x, L.Origin.y }, { 2.0f, L.Size.y }, tokens::WithAlpha(tokens::Accent, 0.7f), clip);
		PaintPopups(dl, layerId + 7, clip);
		return layerId + 17;
	}

	// ---- input --------------------------------------------------------------

	Reply CodeEditor::OnMouseButtonDown(const glm::vec2& pos, int button)
	{
		if (m_Completion.Open)
		{
			const glm::vec4 r = m_CompletionRect;
			if (pos.x >= r.x && pos.x < r.x + r.z && pos.y >= r.y && pos.y < r.y + r.w)
			{
				const int row = m_Completion.Scroll + (int)((pos.y - r.y - 2.0f) / m_CompletionRowHeight);
				if (row >= 0 && row < (int)m_Completion.Filtered.size())
				{
					m_Completion.Selected = row;
					if (button == 0)
						AcceptCompletion();
				}
				return Reply::Handled_().SetFocus();
			}
			m_Completion.Open = false;
		}
		m_Signature.Visible = false;
		const Layout& L = m_Layout;
		if (button == 1 && pos.x >= L.TextLeft - kTextPad && pos.x < L.TextRight)
		{
			const code::TextPos at = PosAt(pos);
			const code::Caret& caret = m_Model->PrimaryCaret();
			const code::TextRange selection = caret.Selection();
			if (!caret.HasSelection() || at < selection.Start || selection.End < at)
				m_Model->SetCaret(at, false);
			m_LastActivity = UIApplication::GetAnimTime();
			if (m_OnContextMenu)
				m_OnContextMenu();
			return Reply::Handled_().SetFocus();
		}
		if (button != 0)
			return Reply::Handled_().SetFocus();
		// A sticky header: jump to it.
		if (!m_StickyLines.empty() && pos.x < L.TextRight && pos.y < L.Origin.y + (float)m_StickyLines.size() * L.LineHeight)
		{
			const int line = m_StickyLines[(size_t)std::clamp((int)((pos.y - L.Origin.y) / L.LineHeight), 0, (int)m_StickyLines.size() - 1)];
			const std::string& text = m_Model->Doc().Line(line);
			const size_t first = text.find_first_not_of(" \t");
			m_Model->SetCaret({ line, first == std::string::npos ? 0 : (int)first }, false);
			m_ScrollY = std::max(0.0f, (float)m_Model->VisibleForLine(line) * L.LineHeight);
			ClampScroll();
			m_LastActivity = UIApplication::GetAnimTime();
			return Reply::Handled_().SetFocus();
		}
		const int mods = UIApplication::Get().GetHeldModifiers();
		const bool shift = (mods & kShift) != 0;
		const bool ctrl = (mods & kCtrl) != 0;
		m_LastActivity = UIApplication::GetAnimTime();

		if (pos.x >= L.MinimapLeft)
		{
			m_Drag = Drag::Minimap;
			OnMouseMove(pos);
			return Reply::Handled_().SetFocus().CaptureMouse();
		}
		const code::TextPos at = PosAt(pos);
		if (pos.x < L.Origin.x + L.GutterWidth)
		{
			// The fold column toggles a fold; the numbers select lines.
			if (pos.x >= L.Origin.x + L.GutterWidth - kFoldColumn - 4.0f && m_Model->FoldStartingAt(at.Line))
			{
				m_Model->ToggleFold(at.Line);
				return Reply::Handled_().SetFocus();
			}
			m_Model->SelectLine(at.Line, shift);
			m_DragAnchor = m_Model->PrimaryCaret().Anchor;
			m_Drag = Drag::Gutter;
			return Reply::Handled_().SetFocus().CaptureMouse();
		}

		const float now = UIApplication::GetAnimTime();
		const bool repeat = now - m_LastClickTime < 0.45f && glm::length(pos - m_LastClickPos) < 4.0f;
		m_ClickCount = repeat ? m_ClickCount % 3 + 1 : 1;
		m_LastClickTime = now;
		m_LastClickPos = pos;
		if (m_ClickCount == 2)
			m_Model->SelectWordAt(at);
		else if (m_ClickCount == 3)
			m_Model->SelectLine(at.Line, false);
		else if (ctrl)
			m_Model->AddCaret(at);
		else
			m_Model->SetCaret(at, shift);
		m_DragAnchor = m_Model->PrimaryCaret().Anchor;
		m_Drag = m_ClickCount == 1 && !ctrl ? Drag::Text : Drag::None;
		return Reply::Handled_().SetFocus().CaptureMouse();
	}

	Reply CodeEditor::OnMouseMove(const glm::vec2& pos)
	{
		const Layout& L = m_Layout;
		if (m_Drag == Drag::Minimap)
		{
			const int visibleCount = m_Model->VisibleLineCount();
			const float line = MinimapFirstLine(visibleCount) + (pos.y - L.Origin.y) / kMinimapLine;
			m_ScrollY = (line - L.Size.y / L.LineHeight * 0.5f) * L.LineHeight;
			ClampScroll();
			return Reply::Handled_();
		}
		if (m_Drag == Drag::Text || m_Drag == Drag::Gutter)
		{
			// Past the edges: scroll toward the pointer.
			if (pos.y < L.Origin.y)
				m_ScrollY -= L.LineHeight;
			else if (pos.y > L.Origin.y + L.Size.y)
				m_ScrollY += L.LineHeight;
			ClampScroll();
			code::TextPos at = PosAt(pos);
			if (m_Drag == Drag::Gutter)
				at = at.Line + 1 < m_Model->Doc().LineCount() && at >= m_DragAnchor ? code::TextPos{ at.Line + 1, 0 } : code::TextPos{ at.Line, 0 };
			m_Model->SetCaret(m_DragAnchor, false);
			m_Model->SetCaret(at, true);
			return Reply::Handled_();
		}
		return Reply::Unhandled();
	}

	Reply CodeEditor::OnMouseButtonUp(const glm::vec2&, int)
	{
		m_Drag = Drag::None;
		return Reply::Handled_().ReleaseCapture();
	}

	Reply CodeEditor::OnMouseWheel(const glm::vec2& pos, float delta)
	{
		const glm::vec4 r = m_CompletionRect;
		if (m_Completion.Open && pos.x >= r.x && pos.x < r.x + r.z && pos.y >= r.y && pos.y < r.y + r.w)
		{
			const int rows = std::max(1, (int)((r.w - 4.0f) / m_CompletionRowHeight));
			const int last = std::max(0, (int)m_Completion.Filtered.size() - rows);
			m_Completion.Scroll = std::clamp(m_Completion.Scroll - (int)std::round(delta * 3.0f), 0, last);
			return Reply::Handled_();
		}
		if ((UIApplication::Get().GetHeldModifiers() & kCtrl) != 0)
		{
			Zoom(m_FontSize + (delta > 0.0f ? 1.0f : -1.0f));
			return Reply::Handled_();
		}
		m_ScrollY -= delta * m_Layout.LineHeight * 3.0f;
		ClampScroll();
		return Reply::Handled_();
	}

	Reply CodeEditor::OnKeyDown(int key, int mods)
	{
		const bool shift = (mods & kShift) != 0;
		const bool ctrl = (mods & kCtrl) != 0;
		using code::CaretMove;
		if (m_OnKey && m_OnKey(key, mods))
			return Reply::Handled_();
		if (key == kF8)
		{
			GoToMarker(shift);
			return Reply::Handled_();
		}
		if (ctrl && (key == kEqual || key == kKeypadAdd))
		{
			Zoom(m_FontSize + 1.0f);
			return Reply::Handled_();
		}
		if (ctrl && (key == kMinus || key == kKeypadSubtract))
		{
			Zoom(m_FontSize - 1.0f);
			return Reply::Handled_();
		}
		if (ctrl && key == kZero)
		{
			Zoom(kDefaultFontSize);
			return Reply::Handled_();
		}
		if (m_Completion.Open && !ctrl)
		{
			const int count = (int)m_Completion.Filtered.size();
			const int page = 8;
			switch (key)
			{
			case kUp: m_Completion.Selected = (m_Completion.Selected + count - 1) % std::max(1, count); return Reply::Handled_();
			case kDown: m_Completion.Selected = (m_Completion.Selected + 1) % std::max(1, count); return Reply::Handled_();
			case kPageUp: m_Completion.Selected = std::max(0, m_Completion.Selected - page); return Reply::Handled_();
			case kPageDown: m_Completion.Selected = std::min(count - 1, m_Completion.Selected + page); return Reply::Handled_();
			case kEnter:
			case kKeypadEnter:
			case kTab:
				if (!shift)
				{
					AcceptCompletion();
					return Reply::Handled_();
				}
				break;
			case kEscape:
				m_Completion.Open = false;
				return Reply::Handled_();
			default:
				break;
			}
		}
		if (key == kSpace && ctrl)
		{
			RequestCompletion();
			return Reply::Handled_();
		}
		if (key == kEscape && m_Signature.Visible)
		{
			m_Signature.Visible = false;
			return Reply::Handled_();
		}
		auto moved = [&](CaretMove move) {
			m_Model->MoveCarets(move, shift, PageLines());
			m_Completion.Open = false;
			if (m_Signature.Visible && m_Model->PrimaryCaret().Pos.Line != m_Signature.Line)
				m_Signature.Visible = false;
			Touch();
			return Reply::Handled_();
		};
		switch (key)
		{
		case kLeft: return moved(ctrl ? CaretMove::WordLeft : CaretMove::Left);
		case kRight: return moved(ctrl ? CaretMove::WordRight : CaretMove::Right);
		case kUp:
			if (ctrl && shift)
			{
				m_Model->MoveLines(-1);
				Touch();
				return Reply::Handled_();
			}
			if (ctrl)
			{
				m_ScrollY -= m_Layout.LineHeight;
				ClampScroll();
				return Reply::Handled_();
			}
			return moved(CaretMove::Up);
		case kDown:
			if (ctrl && shift)
			{
				m_Model->MoveLines(1);
				Touch();
				return Reply::Handled_();
			}
			if (ctrl)
			{
				m_ScrollY += m_Layout.LineHeight;
				ClampScroll();
				return Reply::Handled_();
			}
			return moved(CaretMove::Down);
		case kHome: return moved(ctrl ? CaretMove::DocStart : CaretMove::LineStart);
		case kEnd: return moved(ctrl ? CaretMove::DocEnd : CaretMove::LineEnd);
		case kPageUp: return moved(CaretMove::PageUp);
		case kPageDown: return moved(CaretMove::PageDown);
		case kEnter:
		case kKeypadEnter:
			m_Model->InsertNewline();
			m_Signature.Visible = false;
			Touch();
			return Reply::Handled_();
		case kTab:
			if (shift)
				m_Model->Outdent();
			else
				m_Model->Indent();
			Touch();
			return Reply::Handled_();
		case kBackspace:
			m_Model->Backspace(ctrl);
			Touch();
			if (m_Completion.Open)
				RefilterCompletions();
			return Reply::Handled_();
		case kDelete:
			m_Model->Delete(ctrl);
			Touch();
			return Reply::Handled_();
		case kSpace:
			// The character arrives as OnTextInput; consuming the keydown
			// keeps the router's "Space clicks the focused widget" off.
			return Reply::Handled_();
		case kEscape:
			if (m_Model->Carets().size() > 1 || m_Model->PrimaryCaret().HasSelection())
			{
				m_Model->CollapseCarets();
				return Reply::Handled_();
			}
			return Reply::Unhandled();
		default:
			break;
		}
		if (!ctrl)
			return Reply::Unhandled(); // printable keys arrive as OnTextInput
		switch (key)
		{
		case kA: m_Model->SelectAll(); return Reply::Handled_();
		case kC:
			Clipboard::Set(m_Model->CopySelection());
			return Reply::Handled_();
		case kX:
			Clipboard::Set(m_Model->Cut());
			Touch();
			return Reply::Handled_();
		case kV:
			m_Model->InsertText(Clipboard::Get());
			Touch();
			return Reply::Handled_();
		case kZ:
			if (shift ? m_Model->Redo() : m_Model->Undo())
				Touch();
			return Reply::Handled_();
		case kY:
			if (m_Model->Redo())
				Touch();
			return Reply::Handled_();
		case kD:
			if (shift)
				m_Model->DuplicateLines();
			else
				m_Model->AddNextOccurrence();
			Touch();
			return Reply::Handled_();
		case kSlash:
			m_Model->ToggleComment();
			Touch();
			return Reply::Handled_();
		case kBracketLeft:
		case kBracketRight:
			if (shift)
			{
				// Fold / unfold the innermost fold holding the caret's line.
				const int line = m_Model->PrimaryCaret().Pos.Line;
				const code::FoldRange* best = nullptr;
				for (const code::FoldRange& fold : m_Model->FoldRanges())
					if (fold.StartLine <= line && fold.EndLine >= line && (!best || fold.StartLine > best->StartLine))
						best = &fold;
				if (best && m_Model->IsFolded(best->StartLine) == (key == kBracketRight))
					m_Model->ToggleFold(best->StartLine);
			}
			return Reply::Handled_();
		case kS:
			if (m_OnSave)
				m_OnSave();
			return Reply::Handled_();
		case kF:
			if (m_OnFind)
				m_OnFind(false);
			return Reply::Handled_();
		case kH:
			if (m_OnFind)
				m_OnFind(true);
			return Reply::Handled_();
		case kG:
			if (m_OnGoToLine)
				m_OnGoToLine();
			return Reply::Handled_();
		default:
			return Reply::Unhandled();
		}
	}

	Reply CodeEditor::OnTextInput(uint32_t codepoint)
	{
		if (codepoint < 32 || codepoint == 127)
			return Reply::Unhandled();
		const code::TextPos before = m_Model->PrimaryCaret().Pos;
		const std::string& line = m_Model->Doc().Line(before.Line);
		const bool wordStart = before.Col == 0 || !IsWordChar((unsigned char)line[(size_t)before.Col - 1]);
		m_Model->TypeCodePoint(codepoint);
		Touch();
		if (m_Model->Carets().size() != 1)
			return Reply::Handled_();

		const bool ident = codepoint < 128 && IsWordChar((unsigned char)codepoint);
		if (codepoint == '.' || codepoint == ':')
			RequestCompletion();
		else if (ident && m_Completion.Open)
			RefilterCompletions();
		else if (ident && wordStart && !std::isdigit((int)codepoint))
			RequestCompletion();
		else if (!ident)
			m_Completion.Open = false;

		if (codepoint == '(' || codepoint == ',')
		{
			if (m_OnSignatureRequest)
				m_OnSignatureRequest(m_Model->PrimaryCaret().Pos);
		}
		else if (codepoint == ')')
			m_Signature.Visible = false;
		return Reply::Handled_();
	}

	// ---- the language features -----------------------------------------------

	void CodeEditor::RequestCompletion()
	{
		if (m_OnCompletionRequest && m_Model->Carets().size() == 1)
			m_OnCompletionRequest(m_Model->PrimaryCaret().Pos);
	}

	bool CodeEditor::CaretInCompletionWord(int& caretCol) const
	{
		const code::Caret& caret = m_Model->PrimaryCaret();
		if (m_Model->Carets().size() != 1 || caret.HasSelection() || caret.Pos.Line != m_Completion.Line || caret.Pos.Col < m_Completion.StartCol)
			return false;
		const std::string& line = m_Model->Doc().Line(caret.Pos.Line);
		for (int c = m_Completion.StartCol; c < caret.Pos.Col; c++)
			if (c >= (int)line.size() || !IsWordChar((unsigned char)line[(size_t)c]))
				return false;
		caretCol = caret.Pos.Col;
		return true;
	}

	void CodeEditor::ShowCompletions(int line, int startCol, std::vector<Completion> items)
	{
		m_Completion.Line = line;
		m_Completion.StartCol = startCol;
		m_Completion.Items = std::move(items);
		m_Completion.Selected = 0;
		m_Completion.Scroll = 0;
		m_Completion.Open = true;
		RefilterCompletions();
	}

	void CodeEditor::RefilterCompletions()
	{
		int caretCol = 0;
		if (!CaretInCompletionWord(caretCol) || m_Completion.Items.empty())
		{
			m_Completion.Open = false;
			return;
		}
		const std::string& line = m_Model->Doc().Line(m_Completion.Line);
		std::string word = line.substr((size_t)m_Completion.StartCol, (size_t)(caretCol - m_Completion.StartCol));
		for (char& c : word)
			c = (char)std::tolower((unsigned char)c);
		// Prefix matches first, then the rest whose letters appear in order;
		// the host's order within each group.
		std::vector<int> prefix, fuzzy;
		for (int i = 0; i < (int)m_Completion.Items.size(); i++)
		{
			const std::string& label = m_Completion.Items[(size_t)i].Label;
			size_t at = 0;
			bool isPrefix = label.size() >= word.size();
			for (size_t k = 0; k < word.size() && isPrefix; k++)
				isPrefix = std::tolower((unsigned char)label[k]) == word[k];
			if (isPrefix)
			{
				prefix.push_back(i);
				continue;
			}
			for (char c : label)
				if (at < word.size() && std::tolower((unsigned char)c) == word[at])
					at++;
			if (at == word.size() && !word.empty())
				fuzzy.push_back(i);
		}
		prefix.insert(prefix.end(), fuzzy.begin(), fuzzy.end());
		// A lone exact match is already typed: nothing to offer.
		if (prefix.empty() || (prefix.size() == 1 && m_Completion.Items[(size_t)prefix[0]].Label == line.substr((size_t)m_Completion.StartCol, (size_t)(caretCol - m_Completion.StartCol))))
		{
			m_Completion.Open = false;
			return;
		}
		m_Completion.Filtered = std::move(prefix);
		m_Completion.Selected = std::clamp(m_Completion.Selected, 0, (int)m_Completion.Filtered.size() - 1);
		m_Completion.Open = true;
	}

	void CodeEditor::AcceptCompletion()
	{
		int caretCol = 0;
		if (!m_Completion.Open || m_Completion.Filtered.empty() || !CaretInCompletionWord(caretCol))
		{
			m_Completion.Open = false;
			return;
		}
		const Completion& item = m_Completion.Items[(size_t)m_Completion.Filtered[(size_t)m_Completion.Selected]];
		std::string text = item.InsertText.empty() ? item.Label : item.InsertText;
		// A multi-line snippet continues at the line's indentation.
		const std::string& line = m_Model->Doc().Line(m_Completion.Line);
		const std::string indent = line.substr(0, line.find_first_not_of(" \t") == std::string::npos ? line.size() : line.find_first_not_of(" \t"));
		for (size_t at = text.find('\n'); at != std::string::npos; at = text.find('\n', at + 1 + indent.size()))
			text.insert(at + 1, indent);
		int caretOffset = -1;
		if (const size_t marker = text.find("$0"); marker != std::string::npos)
		{
			caretOffset = (int)marker;
			text.erase(marker, 2);
		}
		m_Completion.Open = false;
		m_Model->ReplaceRange({ { m_Completion.Line, m_Completion.StartCol }, { m_Completion.Line, caretCol } }, text, caretOffset);
		Touch();
		// A call's parentheses: its signature.
		const code::TextPos caret = m_Model->PrimaryCaret().Pos;
		const std::string& after = m_Model->Doc().Line(caret.Line);
		if (caret.Col > 0 && after[(size_t)caret.Col - 1] == '(' && m_OnSignatureRequest)
			m_OnSignatureRequest(caret);
	}

	void CodeEditor::ShowHover(code::TextPos at, std::string text, std::string documentation)
	{
		if (at != m_Hover.Pos)
			return;
		m_Hover.HasHost = !text.empty();
		m_Hover.Text = std::move(text);
		m_Hover.Documentation = std::move(documentation);
	}

	void CodeEditor::ShowSignature(code::TextPos open, std::string label, std::vector<std::pair<int, int>> params, int active)
	{
		if (m_Model->PrimaryCaret().Pos.Line != open.Line || label.empty())
		{
			m_Signature.Visible = false;
			return;
		}
		m_Signature = { true, open.Line, open.Col, std::move(label), std::move(params), active };
	}

	std::vector<const CodeEditor::Marker*> CodeEditor::MarkersAt(code::TextPos pos) const
	{
		std::vector<const Marker*> out;
		for (const Marker& marker : m_Markers)
		{
			if (marker.Col < 0)
			{
				if (marker.Line == pos.Line)
					out.push_back(&marker);
				continue;
			}
			const int endLine = marker.EndLine >= marker.Line ? marker.EndLine : marker.Line;
			const int endCol = marker.EndCol >= 0 ? marker.EndCol : marker.Col;
			const code::TextPos start{ marker.Line, marker.Col };
			const code::TextPos end{ endLine, std::max(endCol, endLine == marker.Line ? marker.Col + 1 : endCol) };
			if (!(pos < start) && pos < end)
				out.push_back(&marker);
		}
		return out;
	}

	void CodeEditor::Update()
	{
		const Layout& L = m_Layout;
		const glm::vec2 mouse = UIApplication::Get().GetMousePos();
		const glm::vec4 r = m_CompletionRect;
		const bool overList = m_Completion.Open && mouse.x >= r.x && mouse.x < r.x + r.z && mouse.y >= r.y && mouse.y < r.y + r.w;
		const bool inText = mouse.x >= L.TextLeft && mouse.x < L.TextRight && mouse.y >= L.Origin.y && mouse.y < L.Origin.y + L.Size.y;
		m_GutterHovered = m_Hovered && mouse.x >= L.Origin.x && mouse.x < L.Origin.x + L.GutterWidth;
		code::TextPos at{ -1, -1 };
		int wordStart = -1;
		if (m_Hovered && m_Drag == Drag::None && inText && !overList && m_Model->Doc().LineCount() > 0)
		{
			at = PosAt(mouse);
			const std::string& line = m_Model->Doc().Line(at.Line);
			const float lineEnd = L.TextLeft - m_ScrollX + (float)code::EditorModel::VisualColumn(line, (int)line.size()) * L.CharWidth;
			if (mouse.x > lineEnd + L.CharWidth * 0.5f)
				at = { -1, -1 };
			else
			{
				// The column under the pointer (PosAt rounds to the nearest gap).
				const float x = mouse.x - L.TextLeft + m_ScrollX;
				at.Col = code::EditorModel::ByteForVisualColumn(line, std::max(0, (int)std::floor(x / L.CharWidth)));
				wordStart = at.Col;
				while (wordStart > 0 && IsWordChar((unsigned char)line[(size_t)wordStart - 1]))
					wordStart--;
			}
		}
		if (at.Line < 0)
		{
			m_Hover = HoverState{};
			return;
		}
		if (at.Line != m_Hover.Pos.Line || wordStart != m_Hover.WordStart)
		{
			m_Hover = HoverState{};
			m_Hover.Pos = at;
			m_Hover.WordStart = wordStart;
			m_Hover.Since = UIApplication::GetAnimTime();
			return;
		}
		if (!m_Hover.Requested && UIApplication::GetAnimTime() - m_Hover.Since > 0.45f)
		{
			m_Hover.Requested = true;
			if (m_OnHoverRequest)
				m_OnHoverRequest(m_Hover.Pos);
		}
	}

	float CodeEditor::PaintRun(DrawList& dl, int layer, float x, float baseline, const std::string& text, const glm::vec4& tint, int clip) const
	{
		for (int col = 0; col < (int)text.size();)
		{
			int length = 1;
			const uint32_t cp = DecodeAt(text, col, length);
			if (cp != ' ' && cp != '\t')
				if (const GlyphInfo* glyph = m_FontCache ? m_FontCache->GetGlyph(m_Font, cp, m_FontSize) : nullptr)
					AddGlyph(dl, layer, { std::round(x), std::round(baseline) }, *glyph, tint, clip);
			x += m_Layout.CharWidth;
			col += length;
		}
		return x;
	}

	size_t CodeEditor::FindHighlight(const std::string& text, size_t from) const
	{
		const size_t n = m_Highlight.size();
		for (size_t at = from; n > 0 && at + n <= text.size(); at++)
		{
			bool match = true;
			for (size_t i = 0; i < n && match; i++)
			{
				const char a = text[at + i], b = m_Highlight[i];
				match = m_HighlightCase ? a == b : std::tolower((unsigned char)a) == std::tolower((unsigned char)b);
			}
			if (match && m_HighlightWord)
				match = (at == 0 || !IsWordChar((unsigned char)text[at - 1]))
					&& (at + n >= text.size() || !IsWordChar((unsigned char)text[at + n]));
			if (match)
				return at;
		}
		return std::string::npos;
	}

	void CodeEditor::PaintLineText(DrawList& dl, int layer, int line, float y, int clip) const
	{
		const Layout& L = m_Layout;
		const std::string& text = m_Model->Doc().Line(line);
		const std::vector<code::Token>& lineTokens = m_Tokens[(size_t)line];
		static constexpr glm::vec4 kBrackets[3] = { tokens::SyntaxBracket1, tokens::SyntaxBracket2, tokens::SyntaxBracket3 };
		int depth = line < (int)m_BracketDepth.size() ? m_BracketDepth[(size_t)line] : 0;
		size_t tokenIndex = 0;
		int visual = 0;
		for (int col = 0; col < (int)text.size();)
		{
			int length = 1;
			const uint32_t cp = DecodeAt(text, col, length);
			const int width = cp == '\t' ? code::EditorModel::kTabWidth - visual % code::EditorModel::kTabWidth : 1;
			const float x = L.TextLeft - m_ScrollX + (float)visual * L.CharWidth;
			while (tokenIndex < lineTokens.size() && lineTokens[tokenIndex].Start + lineTokens[tokenIndex].Length <= col)
				tokenIndex++;
			const code::TokenKind kind = tokenIndex < lineTokens.size() && lineTokens[tokenIndex].Start <= col
				? lineTokens[tokenIndex].Kind : code::TokenKind::Plain;
			glm::vec4 tint = ColourOf(kind);
			// Bracket pairs (outside strings and comments): a colour per depth.
			if (kind == code::TokenKind::Operator)
			{
				if (cp == '(' || cp == '[' || cp == '{')
					tint = kBrackets[depth++ % 3];
				else if (cp == ')' || cp == ']' || cp == '}')
					tint = kBrackets[(depth = std::max(0, depth - 1)) % 3];
			}
			if (cp != '\t' && cp != ' ' && x + L.CharWidth >= L.TextLeft && x <= L.TextRight)
				if (const GlyphInfo* glyph = m_FontCache ? m_FontCache->GetGlyph(m_Font, cp, m_FontSize) : nullptr)
					AddGlyph(dl, layer, { std::round(x), std::round(y + L.Ascent) }, *glyph, tint, clip);
			visual += width;
			col += length;
		}
	}

	void CodeEditor::PaintLineNumber(DrawList& dl, int layer, int line, float y, const glm::vec4& tint, int clip) const
	{
		const Layout& L = m_Layout;
		const std::string number = std::to_string(line + 1);
		float nx = L.Origin.x + L.GutterWidth - kFoldColumn - 6.0f - (float)number.size() * L.CharWidth;
		for (char digit : number)
		{
			if (const GlyphInfo* glyph = m_FontCache ? m_FontCache->GetGlyph(m_Font, (uint32_t)digit, m_FontSize) : nullptr)
				AddGlyph(dl, layer, { std::round(nx), std::round(y + L.Ascent) }, *glyph, tint, clip);
			nx += L.CharWidth;
		}
	}

	std::vector<int> CodeEditor::StickyHeaders() const
	{
		// The headers of the (indentation) folds that contain the first
		// fully visible line - innermost three, outermost first - skipping
		// the module table's `return {`.
		std::vector<int> headers;
		const int firstVisible = (int)std::ceil(m_ScrollY / m_Layout.LineHeight - 0.001f);
		if (firstVisible <= 0 || firstVisible >= m_Model->VisibleLineCount())
			return headers;
		for (int pass = 0; pass < 2; pass++)
		{
			// The sticky rows cover lines: look below them for the context.
			const int top = m_Model->LineForVisible(std::min(m_Model->VisibleLineCount() - 1, firstVisible + (int)headers.size()));
			headers.clear();
			for (const code::FoldRange& fold : m_Model->FoldRanges())
			{
				if (fold.StartLine >= top || fold.EndLine < top)
					continue;
				const std::string& header = m_Model->Doc().Line(fold.StartLine);
				const size_t first = header.find_first_not_of(" \t");
				if (first == std::string::npos || header.compare(first, std::string::npos, "return {") == 0)
					continue;
				headers.push_back(fold.StartLine);
			}
			std::sort(headers.begin(), headers.end());
			if (headers.size() > 3)
				headers.erase(headers.begin(), headers.end() - 3);
		}
		return headers;
	}

	float CodeEditor::PaintTypeText(DrawList& dl, int layer, float x, float baseline, const std::string& text, int clip,
		int highlightBegin, int highlightEnd) const
	{
		// Words: a name followed by ':' (a parameter or a field) or the type
		// names; the keywords of a type; everything else punctuation.
		static constexpr const char* kKeywords[] = { "local", "upvalue", "function", "nil", "true", "false", "typeof", "any", "never", "unknown" };
		for (size_t i = 0; i < text.size();)
		{
			size_t j = i;
			glm::vec4 tint = tokens::SyntaxOperator;
			if (IsWordChar((unsigned char)text[i]) || (unsigned char)text[i] >= 0x80)
			{
				while (j < text.size() && (IsWordChar((unsigned char)text[j]) || (unsigned char)text[j] >= 0x80))
					j++;
				const std::string word = text.substr(i, j - i);
				size_t next = j;
				while (next < text.size() && text[next] == ' ')
					next++;
				const bool named = next < text.size() && text[next] == ':' && (next + 1 >= text.size() || text[next + 1] != ':');
				const bool keyword = std::any_of(std::begin(kKeywords), std::end(kKeywords), [&](const char* k) { return word == k; });
				tint = keyword ? tokens::SyntaxKeyword : named ? tokens::SyntaxParameter
					: std::isdigit((unsigned char)word[0]) ? tokens::SyntaxNumber : tokens::SyntaxType;
			}
			else if (text[i] == '"' || text[i] == '\'')
			{
				const char quote = text[i];
				j = text.find(quote, i + 1);
				j = j == std::string::npos ? text.size() : j + 1;
				tint = tokens::SyntaxString;
			}
			else
				j = i + 1;
			if (highlightBegin >= 0 && (int)i >= highlightBegin && (int)i < highlightEnd)
			{
				// The active parameter: its own span, accent-tinted, underlined.
				const size_t stop = std::min(j, (size_t)highlightEnd);
				const float x0 = x;
				x = PaintRun(dl, layer, x, baseline, text.substr(i, stop - i), tokens::Accent, clip);
				dl.AddBox(layer, { x0, baseline + 3.0f }, { x - x0, 1.0f }, tokens::Accent, clip);
				i = stop;
				continue;
			}
			if (highlightBegin >= 0 && (int)i < highlightBegin && (int)j > highlightBegin)
				j = (size_t)highlightBegin;
			x = PaintRun(dl, layer, x, baseline, text.substr(i, j - i), tint, clip);
			i = j;
		}
		return x;
	}

	void CodeEditor::PaintCard(DrawList& dl, int layer, glm::vec2 pos, glm::vec2 size, int clip) const
	{
		const float radius[4] = { tokens::RadiusMedium, tokens::RadiusMedium, tokens::RadiusMedium, tokens::RadiusMedium };
		dl.AddRoundedBox(layer, pos + glm::vec2(0.0f, 3.0f), size + glm::vec2(2.0f, 2.0f), tokens::Shadow, radius, 0.0f, tokens::Shadow, clip);
		dl.AddRoundedBox(layer, pos, size, tokens::Popup, radius, 1.0f, tokens::BorderStrong, clip);
	}

	void CodeEditor::PaintPopups(DrawList& dl, int layerId, int clip) const
	{
		const Layout& L = m_Layout;
		const float lineH = L.LineHeight;
		const float charW = L.CharWidth;
		auto lineTop = [&](int line) { return L.Origin.y + (float)m_Model->VisibleForLine(line) * lineH - m_ScrollY; };
		auto xOf = [&](int line, int col) {
			return L.TextLeft - m_ScrollX + (float)code::EditorModel::VisualColumn(m_Model->Doc().Line(line), col) * charW;
		};
		auto clampX = [&](float x, float w) {
			return std::clamp(x, L.Origin.x + 4.0f, std::max(L.Origin.x + 4.0f, L.Origin.x + L.Size.x - w - 6.0f));
		};
		auto glyphCount = [](const std::string& text) {
			size_t n = 0;
			for (char c : text)
				n += ((unsigned char)c & 0xC0u) != 0x80u;
			return n;
		};
		// Wrap to whole characters (the cards are monospace).
		auto wrap = [](const std::string& text, size_t width) {
			std::vector<std::string> lines;
			size_t start = 0;
			while (start <= text.size())
			{
				size_t end = text.find('\n', start);
				std::string para = text.substr(start, end == std::string::npos ? std::string::npos : end - start);
				while (para.size() > width)
				{
					size_t cut = para.rfind(' ', width);
					if (cut == std::string::npos || cut == 0)
						cut = width;
					lines.push_back(para.substr(0, cut));
					para = para.substr(cut + (para[cut] == ' ' ? 1 : 0));
				}
				lines.push_back(para);
				if (end == std::string::npos)
					break;
				start = end + 1;
			}
			return lines;
		};
		const size_t maxChars = (size_t)std::max(20.0f, std::min(100.0f, (L.Size.x - 32.0f) / charW));
		const float pad = 10.0f;

		// The signature card, above the caret's line.
		if (m_Signature.Visible && !m_Signature.Label.empty())
		{
			const int line = m_Signature.Line;
			const std::string label = glyphCount(m_Signature.Label) > maxChars ? m_Signature.Label.substr(0, maxChars) : m_Signature.Label;
			const float w = (float)glyphCount(label) * charW + pad * 2.0f;
			const float h = lineH + 8.0f;
			float y = lineTop(line) - h - 4.0f;
			if (y < L.Origin.y)
				y = lineTop(line) + lineH + 4.0f;
			const size_t paren = label.find('(');
			const float x = clampX(xOf(line, m_Signature.OpenCol) - (float)(paren == std::string::npos ? 0 : paren) * charW - pad, w);
			PaintCard(dl, layerId, { x, y }, { w, h }, clip);
			int begin = -1, end = -1;
			if (m_Signature.Active >= 0 && m_Signature.Active < (int)m_Signature.Params.size())
			{
				begin = m_Signature.Params[(size_t)m_Signature.Active].first;
				end = m_Signature.Params[(size_t)m_Signature.Active].second;
			}
			PaintTypeText(dl, layerId + 1, x + pad, std::round(y + 4.0f + L.Ascent), label, clip, begin, end);
		}

		// The completion list, below the caret (above when it would not fit),
		// with a footer naming the selected item in full.
		m_CompletionRect = glm::vec4(0.0f);
		if (m_Completion.Open && !m_Completion.Filtered.empty())
		{
			const float rowH = lineH + 3.0f;
			m_CompletionRowHeight = rowH;
			const int count = (int)m_Completion.Filtered.size();
			const int rows = std::min(count, 10);
			size_t labelChars = 14, detailChars = 0;
			for (int index : m_Completion.Filtered)
			{
				labelChars = std::max(labelChars, glyphCount(m_Completion.Items[(size_t)index].Label));
				detailChars = std::max(detailChars, glyphCount(m_Completion.Items[(size_t)index].Detail));
			}
			labelChars = std::min<size_t>(labelChars, 34);
			detailChars = std::min<size_t>(detailChars, 30);
			const float w = std::min(L.Size.x - 12.0f, (float)(labelChars + detailChars + 7) * charW + 20.0f);
			const Completion& selected = m_Completion.Items[(size_t)m_Completion.Filtered[(size_t)m_Completion.Selected]];
			const std::string footerText = selected.Kind == CompletionKind::Snippet ? selected.Label + " - insert the hook"
				: selected.Detail.empty() ? std::string() : selected.Label + ": " + selected.Detail;
			const std::vector<std::string> footer = footerText.empty() ? std::vector<std::string>{}
				: wrap(footerText, (size_t)std::max(10.0f, (w - pad * 2.0f) / charW));
			const float footerH = footer.empty() ? 0.0f : (float)std::min<size_t>(footer.size(), 3) * lineH + 9.0f;
			const float listH = (float)rows * rowH + 6.0f;
			const float h = listH + footerH;
			float y = lineTop(m_Completion.Line) + lineH + 3.0f;
			if (y + h > L.Origin.y + L.Size.y && lineTop(m_Completion.Line) - h - 3.0f >= L.Origin.y)
				y = lineTop(m_Completion.Line) - h - 3.0f;
			const float x = clampX(xOf(m_Completion.Line, m_Completion.StartCol) - charW * 2.5f - 8.0f, w);
			PaintCard(dl, layerId + 3, { x, y }, { w, h }, clip);
			m_CompletionRect = { x, y, w, listH };

			int& scroll = m_Completion.Scroll;
			if (m_Completion.Selected < scroll)
				scroll = m_Completion.Selected;
			else if (m_Completion.Selected >= scroll + rows)
				scroll = m_Completion.Selected - rows + 1;
			scroll = std::clamp(scroll, 0, std::max(0, count - rows));

			int caretCol = m_Completion.StartCol;
			CaretInCompletionWord(caretCol);
			const size_t typed = (size_t)std::max(0, caretCol - m_Completion.StartCol);
			const std::string& editLine = m_Model->Doc().Line(m_Completion.Line);
			for (int r = 0; r < rows; r++)
			{
				const int index = scroll + r;
				const Completion& item = m_Completion.Items[(size_t)m_Completion.Filtered[(size_t)index]];
				const float ry = y + 3.0f + (float)r * rowH;
				if (index == m_Completion.Selected)
				{
					const float sr[4] = { tokens::RadiusSmall, tokens::RadiusSmall, tokens::RadiusSmall, tokens::RadiusSmall };
					dl.AddRoundedBox(layerId + 4, { x + 3.0f, ry }, { w - 6.0f, rowH }, tokens::CodeSelection, sr, 0.0f, tokens::CodeSelection, clip);
				}
				const float baseline = std::round(ry + 1.5f + L.Ascent);
				// The kind chip: a tinted rounded badge with the kind's letter.
				static constexpr const char* kLetters = "vfmpfktMsc";
				glm::vec4 kindTint = tokens::SyntaxParameter;
				switch (item.Kind)
				{
				case CompletionKind::Function:
				case CompletionKind::Method: kindTint = tokens::SyntaxFunction; break;
				case CompletionKind::Property:
				case CompletionKind::Field: kindTint = tokens::SyntaxGlobal; break;
				case CompletionKind::Keyword: kindTint = tokens::SyntaxKeyword; break;
				case CompletionKind::Type: kindTint = tokens::SyntaxLiteral; break;
				case CompletionKind::Module: kindTint = tokens::SyntaxString; break;
				case CompletionKind::Snippet: kindTint = tokens::Accent; break;
				default: break;
				}
				const float chip = std::round(lineH - 4.0f);
				const float cr[4] = { 3.0f, 3.0f, 3.0f, 3.0f };
				dl.AddRoundedBox(layerId + 5, { x + 8.0f, ry + (rowH - chip) * 0.5f }, { chip, chip },
					tokens::WithAlpha(kindTint, tokens::CodeKindChipAlpha), cr, 0.0f, kindTint, clip);
				PaintRun(dl, layerId + 6, std::round(x + 8.0f + (chip - charW) * 0.5f), baseline, std::string(1, kLetters[(size_t)item.Kind]), kindTint, clip);
				// The label, its typed prefix in the accent colour.
				const std::string label = glyphCount(item.Label) > labelChars ? item.Label.substr(0, labelChars - 1) + "\xE2\x80\xA6" : item.Label;
				float pen = x + 8.0f + chip + 8.0f;
				bool isPrefix = typed <= item.Label.size();
				for (size_t k = 0; k < typed && isPrefix; k++)
					isPrefix = std::tolower((unsigned char)item.Label[k]) == std::tolower((unsigned char)editLine[(size_t)m_Completion.StartCol + k]);
				const size_t lit = isPrefix ? std::min(typed, label.size()) : 0;
				pen = PaintRun(dl, layerId + 6, pen, baseline, label.substr(0, lit), tokens::Accent, clip);
				PaintRun(dl, layerId + 6, pen, baseline, label.substr(lit), item.Kind == CompletionKind::Keyword ? tokens::SyntaxKeyword : tokens::TextPrimary, clip);
				// The detail, right-aligned and muted.
				if (!item.Detail.empty() && detailChars > 0 && item.Kind != CompletionKind::Snippet)
				{
					const std::string detail = glyphCount(item.Detail) > detailChars ? item.Detail.substr(0, detailChars - 1) + "\xE2\x80\xA6" : item.Detail;
					PaintRun(dl, layerId + 6, x + w - pad - (float)glyphCount(detail) * charW, baseline, detail, tokens::TextMuted, clip);
				}
			}
			if (count > rows)
			{
				const float trackH = listH - 6.0f;
				const float thumbH = std::max(10.0f, trackH * (float)rows / (float)count);
				const float thumbY = y + 3.0f + (trackH - thumbH) * (float)scroll / (float)std::max(1, count - rows);
				const float tr[4] = { 1.5f, 1.5f, 1.5f, 1.5f };
				dl.AddRoundedBox(layerId + 6, { x + w - 5.0f, thumbY }, { 3.0f, thumbH }, tokens::BorderStrong, tr, 0.0f, tokens::BorderStrong, clip);
			}
			if (!footer.empty())
			{
				dl.AddBox(layerId + 5, { x + 1.0f, y + listH }, { w - 2.0f, 1.0f }, tokens::Separator, clip);
				for (size_t i = 0; i < footer.size() && i < 3; i++)
					PaintTypeText(dl, layerId + 6, x + pad, std::round(y + listH + 4.0f + (float)i * lineH + L.Ascent), footer[i], clip);
			}
		}

		// The hover card: the diagnostics under the pointer, then the host's
		// text (coloured as a declaration) and its documentation.
		if (m_Hover.Requested && m_Hover.Pos.Line >= 0 && !m_Completion.Open)
		{
			enum class RowKind : uint8_t { Error, Warning, Code, Doc };
			struct Row
			{
				std::string Text;
				RowKind Kind;
			};
			std::vector<Row> rows;
			for (const Marker* marker : MarkersAt(m_Hover.Pos))
				for (const std::string& part : wrap(marker->Message, maxChars))
					rows.push_back({ part, marker->Error ? RowKind::Error : RowKind::Warning });
			if (m_Hover.HasHost)
			{
				for (const std::string& part : wrap(m_Hover.Text, maxChars))
					rows.push_back({ part, RowKind::Code });
				if (!m_Hover.Documentation.empty())
					for (const std::string& part : wrap(m_Hover.Documentation, maxChars))
						rows.push_back({ part, RowKind::Doc });
			}
			if (!rows.empty())
			{
				size_t widest = 0;
				for (const Row& row : rows)
					widest = std::max(widest, glyphCount(row.Text));
				const bool split = rows.front().Kind <= RowKind::Warning && rows.back().Kind >= RowKind::Code;
				const float w = (float)widest * charW + pad * 2.0f + 14.0f;
				const float h = (float)rows.size() * lineH + 10.0f + (split ? 7.0f : 0.0f);
				float y = lineTop(m_Hover.Pos.Line) - h - 4.0f;
				if (y < L.Origin.y)
					y = lineTop(m_Hover.Pos.Line) + lineH + 4.0f;
				const float x = clampX(xOf(m_Hover.Pos.Line, std::max(0, m_Hover.WordStart)) - pad, w);
				PaintCard(dl, layerId + 7, { x, y }, { w, h }, clip);
				float rowY = y + 5.0f;
				for (size_t i = 0; i < rows.size(); i++)
				{
					const Row& row = rows[i];
					if (i > 0 && row.Kind >= RowKind::Code && rows[i - 1].Kind <= RowKind::Warning)
					{
						dl.AddBox(layerId + 8, { x + 1.0f, rowY + 3.0f }, { w - 2.0f, 1.0f }, tokens::Separator, clip);
						rowY += 7.0f;
					}
					const float baseline = std::round(rowY + L.Ascent);
					switch (row.Kind)
					{
					case RowKind::Error:
					case RowKind::Warning:
					{
						const glm::vec4 tint = row.Kind == RowKind::Error ? tokens::Error : tokens::Warning;
						const float dr[4] = { 3.0f, 3.0f, 3.0f, 3.0f };
						dl.AddRoundedBox(layerId + 8, { x + pad, rowY + lineH * 0.5f - 3.0f }, { 6.0f, 6.0f }, tint, dr, 0.0f, tint, clip);
						PaintRun(dl, layerId + 8, x + pad + 14.0f, baseline, row.Text, tokens::TextPrimary, clip);
						break;
					}
					case RowKind::Code: PaintTypeText(dl, layerId + 8, x + pad, baseline, row.Text, clip); break;
					case RowKind::Doc: PaintRun(dl, layerId + 8, x + pad, baseline, row.Text, tokens::TextSecondary, clip); break;
					}
					rowY += lineH;
				}
			}
		}
	}

} // namespace wisp
