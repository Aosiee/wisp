#include "Wisp/Components/CodeEditor.h"
#include "Wisp/Application.h"
#include "Wisp/Clipboard.h"
#include "Wisp/DrawList.h"
#include "Wisp/Icons.h"
#include "Wisp/StyleTokens.h"

#include <algorithm>
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
			kKeypadEnter = 335,
		};
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
		layout.TextRight = layout.MinimapLeft - 4.0f;
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

		for (int v = firstVisible; v <= lastVisible; v++)
		{
			const int line = m_Model->LineForVisible(v);
			const std::string& text = doc.Line(line);
			const float y = L.Origin.y + (float)v * L.LineHeight - m_ScrollY;

			if (line == primary.Pos.Line && !primary.HasSelection())
				dl.AddBox(layerId + 1, { L.Origin.x + L.GutterWidth, y }, { L.TextRight - L.Origin.x - L.GutterWidth, L.LineHeight }, tokens::CodeCurrentLine, clip);

			// Find matches.
			if (!m_Highlight.empty())
			{
				for (size_t at = 0; at + m_Highlight.size() <= text.size(); at++)
				{
					bool match = true;
					for (size_t i = 0; i < m_Highlight.size() && match; i++)
					{
						const char a = text[at + i], b = m_Highlight[i];
						match = m_HighlightCase ? a == b : std::tolower((unsigned char)a) == std::tolower((unsigned char)b);
					}
					if (!match)
						continue;
					const float x0 = xOf(line, (int)at);
					const float x1 = xOf(line, (int)(at + m_Highlight.size()));
					dl.AddBox(layerId + 2, { x0, y }, { x1 - x0, L.LineHeight }, tokens::CodeFindMatch, textClip);
					at += m_Highlight.size() - 1;
				}
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

			// Indentation guides on non-blank lines.
			const int indent = code::EditorModel::IndentWidth(text);
			for (int level = code::EditorModel::kTabWidth; level < indent; level += code::EditorModel::kTabWidth)
			{
				const float gx = L.TextLeft - m_ScrollX + (float)level * L.CharWidth;
				dl.AddBox(layerId + 2, { std::round(gx), y }, { 1.0f, L.LineHeight }, tokens::CodeIndentGuide, textClip);
			}

			// The text, coloured by token.
			const std::vector<code::Token>& lineTokens = m_Tokens[(size_t)line];
			size_t tokenIndex = 0;
			int visual = 0;
			for (int col = 0; col < (int)text.size();)
			{
				int length = 1;
				const uint32_t cp = DecodeAt(text, col, length);
				const int width = cp == '\t' ? code::EditorModel::kTabWidth - visual % code::EditorModel::kTabWidth : 1;
				const float x = L.TextLeft - m_ScrollX + (float)visual * L.CharWidth;
				if (cp != '\t' && cp != ' ' && x + L.CharWidth >= L.TextLeft && x <= L.TextRight)
				{
					while (tokenIndex < lineTokens.size() && lineTokens[tokenIndex].Start + lineTokens[tokenIndex].Length <= col)
						tokenIndex++;
					const code::TokenKind kind = tokenIndex < lineTokens.size() && lineTokens[tokenIndex].Start <= col
						? lineTokens[tokenIndex].Kind : code::TokenKind::Plain;
					if (const GlyphInfo* glyph = m_FontCache ? m_FontCache->GetGlyph(m_Font, cp, m_FontSize) : nullptr)
						AddGlyph(dl, layerId + 3, { std::round(x), std::round(y + L.Ascent) }, *glyph, ColourOf(kind), textClip);
				}
				visual += width;
				col += length;
			}

			// A folded header shows an ellipsis box after its text.
			if (m_Model->IsFolded(line))
			{
				const float x = xOf(line, (int)text.size()) + L.CharWidth;
				const float r[4] = { tokens::RadiusSmall, tokens::RadiusSmall, tokens::RadiusSmall, tokens::RadiusSmall };
				dl.AddRoundedBox(layerId + 2, { x, y + 3.0f }, { L.CharWidth * 3.0f, L.LineHeight - 6.0f }, tokens::Control, r, 0.0f, tokens::Control, textClip);
				if (const GlyphInfo* dot = m_FontCache ? m_FontCache->GetGlyph(m_Font, 0x2026, m_FontSize) : nullptr)
					AddGlyph(dl, layerId + 3, { std::round(x + L.CharWidth), std::round(y + L.Ascent) }, *dot, tokens::TextMuted, textClip);
			}

			// Diagnostics: a gutter dot and an underline.
			for (const Marker& marker : m_Markers)
			{
				if (marker.Line != line)
					continue;
				const glm::vec4 tint = marker.Error ? tokens::Error : tokens::Warning;
				const float r[4] = { 3.0f, 3.0f, 3.0f, 3.0f };
				dl.AddRoundedBox(layerId + 4, { L.Origin.x + 5.0f, y + L.LineHeight * 0.5f - 3.0f }, { 6.0f, 6.0f }, tint, r, 0.0f, tint, clip);
				const int start = (int)std::min(text.size(), text.find_first_not_of(" \t") == std::string::npos ? text.size() : text.find_first_not_of(" \t"));
				const float x0 = xOf(line, start);
				const float x1 = std::max(x0 + L.CharWidth, xOf(line, (int)text.size()));
				dl.AddLine(layerId + 4, { x0, y + L.LineHeight - 2.0f }, { x1, y + L.LineHeight - 2.0f }, 1.0f, tint, textClip);
			}

			// The gutter: the line number and the fold chevron.
			const std::string number = std::to_string(line + 1);
			const glm::vec4 numberTint = line == primary.Pos.Line ? tokens::CodeLineNumberActive : tokens::CodeLineNumber;
			float nx = L.Origin.x + L.GutterWidth - kFoldColumn - 6.0f - (float)number.size() * L.CharWidth;
			for (char digit : number)
			{
				if (const GlyphInfo* glyph = m_FontCache ? m_FontCache->GetGlyph(m_Font, (uint32_t)digit, m_FontSize) : nullptr)
					AddGlyph(dl, layerId + 3, { std::round(nx), std::round(y + L.Ascent) }, *glyph, numberTint, clip);
				nx += L.CharWidth;
			}
			if (m_Model->FoldStartingAt(line))
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
			const float viewTop = L.Origin.y + (m_ScrollY / L.LineHeight - minimapFirst) * kMinimapLine;
			dl.AddBox(layerId + 3, { L.MinimapLeft, viewTop }, { kMinimapWidth, L.Size.y / L.LineHeight * kMinimapLine }, tokens::CodeMinimapView, clip);
		}

		// The border focus line.
		if (focused)
			dl.AddBox(layerId + 5, { L.Origin.x, L.Origin.y }, { 2.0f, L.Size.y }, tokens::WithAlpha(tokens::Accent, 0.7f), clip);
		return layerId + 6;
	}

	// ---- input --------------------------------------------------------------

	Reply CodeEditor::OnMouseButtonDown(const glm::vec2& pos, int button)
	{
		if (button != 0)
			return Reply::Handled_().SetFocus();
		const Layout& L = m_Layout;
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

	Reply CodeEditor::OnMouseWheel(const glm::vec2&, float delta)
	{
		m_ScrollY -= delta * m_Layout.LineHeight * 3.0f;
		ClampScroll();
		return Reply::Handled_();
	}

	Reply CodeEditor::OnKeyDown(int key, int mods)
	{
		const bool shift = (mods & kShift) != 0;
		const bool ctrl = (mods & kCtrl) != 0;
		using code::CaretMove;
		auto moved = [&](CaretMove move) {
			m_Model->MoveCarets(move, shift, PageLines());
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
		m_Model->TypeCodePoint(codepoint);
		Touch();
		return Reply::Handled_();
	}

} // namespace wisp
