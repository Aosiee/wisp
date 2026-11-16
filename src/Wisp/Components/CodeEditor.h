#pragma once

// The code editor widget - paints a
// wisp::code::EditorModel (virtualised: only the visible lines and the
// minimap's slice) and maps input onto its commands. It knows no language:
// syntax colour comes from an ITokenizer, diagnostics from SetMarkers.
//
// Layout (left to right): the gutter (line numbers, diagnostic dots, fold
// chevrons), the text (a monospace grid: x = visual column * the advance),
// the minimap (its own scroll, a viewport box; click or drag to scroll).
//
// Keys (mods 1 = Shift, 2 = Ctrl): arrows/Home/End/PageUp/PageDown (Shift
// extends, Ctrl by word / document), Backspace/Delete (Ctrl: a word),
// Enter, Tab / Shift+Tab, Ctrl+A/C/X/V/Z/Y (Ctrl+Shift+Z redo), Ctrl+D
// (next occurrence), Ctrl+/ (comment), Ctrl+Shift+D (duplicate),
// Ctrl+Shift+Up/Down (move lines), Ctrl+Shift+[ / ] (fold / unfold here),
// Escape (one caret), and the host's Ctrl+S / Ctrl+F / Ctrl+H / Ctrl+G.
// Every navigation key is consumed, so focus never leaves on Tab or an arrow.
//
// The language features, served by the host.
// A completion list (requested on '.', ':', the first character of a word
// and Ctrl+Space; filtered locally while typing; Up / Down / PageUp /
// PageDown, Enter or Tab accepts, Escape closes), a hover card after the
// pointer rests on a word (the diagnostics under it plus the host's text),
// and a signature card (requested on '(' and ','). The host answers through
// ShowCompletions / ShowHover / ShowSignature and calls Update once a frame.
//
// Each line's first diagnostic is painted
// after its code (and ticked in the minimap); F8 / Shift+F8 step through the
// markers; the identifier under a collapsed caret highlights its other
// whole-word occurrences; Ctrl+wheel and Ctrl+= / Ctrl+- / Ctrl+0 zoom
// (SetOnZoom persists it); fold chevrons show while the pointer is over the
// gutter. SetOnKey sees every key first (the host's tab commands).

#include "Wisp/Application.h"
#include "Wisp/Code/EditorModel.h"
#include "Wisp/Code/Tokenizer.h"
#include "Wisp/Core.h"
#include "Wisp/FontCache.h"

#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace wisp {

	class CodeEditor : public Widget
	{
	public:
		WISP_WIDGET_TYPE(CodeEditor)

		CodeEditor(FontCache* fontCache, FontHandle font, float fontSize, std::shared_ptr<const code::ITokenizer> tokenizer);

		code::EditorModel& Model() { return *m_Model; }
		const code::EditorModel& Model() const { return *m_Model; }
		// Replaces the text (undo cleared, clean) and scrolls to the top.
		void SetText(const std::string& text);
		// Shows another document (a host's tab): its undo, carets and folds
		// travel with the model; the scroll is the host's to keep.
		void SetModel(std::shared_ptr<code::EditorModel> model, glm::vec2 scroll = { 0.0f, 0.0f });
		glm::vec2 GetScroll() const { return { m_ScrollX, m_ScrollY }; }
		// The syntax colouring (a host's per-document language).
		void SetTokenizer(std::shared_ptr<const code::ITokenizer> tokenizer)
		{
			m_Tokenizer = std::move(tokenizer);
			m_TokensValidTo = -1;
		}

		struct Marker
		{
			int Line = 0;
			bool Error = true; // else a warning
			std::string Message;
			int Col = -1;     // -1 = the whole line (from its first non-blank)
			int EndLine = -1; // with Col: the range's end (-1 = Line)
			int EndCol = -1;
		};
		void SetMarkers(std::vector<Marker> markers) { m_Markers = std::move(markers); }

		enum class CompletionKind : uint8_t { Variable, Function, Method, Property, Field, Keyword, Type, Module, Snippet, Value };
		struct Completion
		{
			std::string Label;
			std::string Detail;
			std::string InsertText; // "$0" marks the caret; empty = the label
			CompletionKind Kind = CompletionKind::Variable;
		};
		// The word being completed starts at (line, startCol). Ignored when
		// the caret has left that word since the request; empty closes.
		void ShowCompletions(int line, int startCol, std::vector<Completion> items);
		void CloseCompletions() { m_Completion.Open = false; }
		bool IsCompletionOpen() const { return m_Completion.Open; }
		// The hover card's host part, for the word at `at` (shown only while
		// the pointer still rests on that word).
		void ShowHover(code::TextPos at, std::string text, std::string documentation);
		// The signature card, anchored on the call's '(' at `open` (shown
		// while the caret stays on that line).
		void ShowSignature(code::TextPos open, std::string label, std::vector<std::pair<int, int>> params, int active);
		void HideSignature() { m_Signature.Visible = false; }

		void SetOnCompletionRequest(std::function<void(code::TextPos caret)> fn) { m_OnCompletionRequest = std::move(fn); }
		void SetOnHoverRequest(std::function<void(code::TextPos at)> fn) { m_OnHoverRequest = std::move(fn); }
		void SetOnSignatureRequest(std::function<void(code::TextPos caret)> fn) { m_OnSignatureRequest = std::move(fn); }
		// The hover timer; the host calls it once a frame.
		void Update();
		// The find bar's needle, every match painted (empty = none).
		void SetHighlight(std::string needle, bool matchCase, bool wholeWord = false)
		{
			m_Highlight = std::move(needle);
			m_HighlightCase = matchCase;
			m_HighlightWord = wholeWord;
		}
		void SetShowMinimap(bool show) { m_ShowMinimap = show; }
		// Scrolls so the primary caret is in view at the next paint.
		void RevealCaret() { m_RevealCaret = true; }

		// The host's commands (the Script Editor panel's).
		void SetOnSave(std::function<void()> fn) { m_OnSave = std::move(fn); }
		void SetOnFind(std::function<void(bool replace)> fn) { m_OnFind = std::move(fn); }
		void SetOnGoToLine(std::function<void()> fn) { m_OnGoToLine = std::move(fn); }
		// A right-click (the caret moves to it unless it lands in the
		// selection); the host shows its menu at the pointer.
		void SetOnContextMenu(std::function<void()> fn) { m_OnContextMenu = std::move(fn); }
		// Consulted before the editor's own key map; true = handled.
		void SetOnKey(std::function<bool(int key, int mods)> fn) { m_OnKey = std::move(fn); }
		// The text size (zoom); SetOnZoom hears the user's changes.
		void SetFontSize(float size);
		float GetFontSize() const { return m_FontSize; }
		void SetOnZoom(std::function<void(float size)> fn) { m_OnZoom = std::move(fn); }
		static constexpr float kMinFontSize = 8.0f;
		static constexpr float kMaxFontSize = 32.0f;
		// F8 / Shift+F8: the caret to the next / previous marker (wrapping);
		// false when there is none.
		bool GoToMarker(bool backwards);

		glm::vec2 ComputeDesiredSize() const override { return { 240.0f, 160.0f }; }
		void ArrangeChildren(const Geometry&, ArrangedChildren&) const override {}
		int OnPaint(const PaintArgs& args, const Geometry& geom, DrawList& drawList, int layerId, const WidgetStyleContext& style) const override;

		Reply OnMouseButtonDown(const glm::vec2& pos, int button) override;
		Reply OnMouseButtonUp(const glm::vec2& pos, int button) override;
		Reply OnMouseMove(const glm::vec2& pos) override;
		Reply OnMouseWheel(const glm::vec2& pos, float delta) override;
		Reply OnKeyDown(int key, int mods) override;
		Reply OnTextInput(uint32_t codepoint) override;
		void OnFocusReceived() override { m_HasFocus = true; m_LastActivity = UIApplication::GetAnimTime(); }
		void OnFocusLost() override { m_HasFocus = false; m_Drag = Drag::None; m_Completion.Open = false; m_Signature.Visible = false; }
		Reply OnMouseEnter() override { m_Hovered = true; return Reply::Unhandled(); }
		Reply OnMouseLeave() override { m_Hovered = false; return Reply::Unhandled(); }
		NavigationReply OnNavigation(const FocusEvent&) override { return NavigationReply::Stop; }

	private:
		struct Layout
		{
			glm::vec2 Origin{ 0.0f };
			glm::vec2 Size{ 0.0f };
			float GutterWidth = 0.0f;
			float TextLeft = 0.0f;   // absolute x of visual column 0 (before scrolling)
			float TextRight = 0.0f;
			float MinimapLeft = 0.0f;
			float CharWidth = 8.0f;
			float LineHeight = 18.0f;
			float Ascent = 13.0f;
		};
		enum class Drag : uint8_t { None, Text, Minimap, Gutter };

		void UpdateLayout(const Geometry& geom) const;
		void UpdateTokens(int lastLine) const;
		code::TextPos PosAt(const glm::vec2& absolute) const;
		void ScrollToCaret() const;
		void ClampScroll() const;
		float MinimapFirstLine(int visibleCount) const;
		void Touch() { m_LastActivity = UIApplication::GetAnimTime(); m_RevealCaret = true; }
		int PageLines() const { return std::max(1, (int)(m_Layout.Size.y / m_Layout.LineHeight) - 1); }

		// The language features.
		void RequestCompletion();
		void RefilterCompletions();
		void AcceptCompletion();
		bool CaretInCompletionWord(int& caretCol) const;
		void PaintPopups(DrawList& dl, int layerId, int clip) const;
		float PaintRun(DrawList& dl, int layer, float x, float baseline, const std::string& text, const glm::vec4& tint, int clip) const;
		// A type or signature, coloured: names before ':' plain, type names in
		// the type colour, keywords and punctuation in theirs.
		float PaintTypeText(DrawList& dl, int layer, float x, float baseline, const std::string& text, int clip,
			int highlightBegin = -1, int highlightEnd = -1) const;
		void PaintCard(DrawList& dl, int layer, glm::vec2 pos, glm::vec2 size, int clip) const;
		bool OccurrenceWord(std::string& word) const;
		void PaintLineText(DrawList& dl, int layer, int line, float y, int clip) const;
		void PaintLineNumber(DrawList& dl, int layer, int line, float y, const glm::vec4& tint, int clip) const;
		std::vector<int> StickyHeaders() const;
		// The next find-bar match in `text` at or after `from` (npos = none).
		size_t FindHighlight(const std::string& text, size_t from) const;
		void Zoom(float size);
		std::vector<const Marker*> MarkersAt(code::TextPos pos) const;

		FontCache* m_FontCache;
		FontHandle m_Font;
		float m_FontSize;
		std::shared_ptr<const code::ITokenizer> m_Tokenizer;
		std::shared_ptr<code::EditorModel> m_Model = std::make_shared<code::EditorModel>();

		std::vector<Marker> m_Markers;
		std::string m_Highlight;
		bool m_HighlightCase = false;
		bool m_HighlightWord = false;
		std::function<bool(int, int)> m_OnKey;
		std::function<void()> m_OnContextMenu;
		mutable std::vector<int> m_BracketDepth; // the bracket depth at each line's start (with the tokens)
		std::function<void(float)> m_OnZoom;
		bool m_GutterHovered = false;
		bool m_ShowMinimap = true;
		std::function<void()> m_OnSave;
		std::function<void(bool)> m_OnFind;
		std::function<void()> m_OnGoToLine;

		// Paint-time state (OnPaint is const).
		mutable Layout m_Layout;
		mutable float m_ScrollY = 0.0f;
		mutable float m_ScrollX = 0.0f;
		mutable bool m_RevealCaret = false;
		mutable std::vector<std::vector<code::Token>> m_Tokens; // per line
		mutable std::vector<uint32_t> m_TokenState;            // the state each line starts in
		mutable int m_TokensValidTo = -1;                      // lines [0, this] are current
		mutable uint64_t m_TokensVersion = 0;

		struct CompletionState
		{
			bool Open = false;
			int Line = 0;
			int StartCol = 0;
			std::vector<Completion> Items;
			std::vector<int> Filtered; // indices into Items
			int Selected = 0;
			mutable int Scroll = 0; // the paint keeps the selection in view
		};
		CompletionState m_Completion;
		struct HoverState
		{
			code::TextPos Pos{ -1, -1 };
			int WordStart = -1;
			float Since = 0.0f;
			bool Requested = false;
			bool HasHost = false;
			std::string Text;
			std::string Documentation;
		};
		HoverState m_Hover;
		struct SignatureState
		{
			bool Visible = false;
			int Line = 0;
			int OpenCol = 0;
			std::string Label;
			std::vector<std::pair<int, int>> Params;
			int Active = -1;
		};
		SignatureState m_Signature;
		std::function<void(code::TextPos)> m_OnCompletionRequest;
		std::function<void(code::TextPos)> m_OnHoverRequest;
		std::function<void(code::TextPos)> m_OnSignatureRequest;
		mutable glm::vec4 m_CompletionRect{ 0.0f }; // x, y, w, h (the last paint)
		mutable std::vector<int> m_StickyLines;     // the sticky headers the last paint showed
		mutable float m_CompletionRowHeight = 18.0f;
		bool m_Hovered = false;

		bool m_HasFocus = false;
		float m_LastActivity = -1.0f; // the anim time of the last edit / move (the caret stays solid)
		Drag m_Drag = Drag::None;
		code::TextPos m_DragAnchor;
		int m_ClickCount = 0;
		float m_LastClickTime = -10.0f;
		glm::vec2 m_LastClickPos{ 0.0f };
	};

} // namespace wisp
