#pragma once

// The code editor widget - paints a wisp::code::EditorModel (virtualised:
// only the visible lines and the minimap's slice) and maps input onto its
// commands. It knows no language: syntax colour comes from an ITokenizer,
// diagnostics from SetMarkers.
//
// Layout (left to right): the gutter (line numbers, diagnostic dots, fold
// chevrons), the text (a monospace grid: x = visual column * the advance),
// the minimap (its own scroll, a viewport box; click or drag to scroll).
//
// Keys (mods 1 = Shift, 2 = Ctrl): arrows/Home/End/PageUp/PageDown (Shift
// extends, Ctrl by word / document), Backspace/Delete (Ctrl: a word), Enter,
// Tab / Shift+Tab, Ctrl+A/C/X/V/Z/Y (Ctrl+Shift+Z redo), Ctrl+D (next
// occurrence), Ctrl+/ (comment), Ctrl+Shift+D (duplicate), Ctrl+Shift+Up/Down
// (move lines), Ctrl+Shift+[ / ] (fold / unfold here), Escape (one caret),
// and the host's Ctrl+S / Ctrl+F / Ctrl+H / Ctrl+G. Every navigation key is
// consumed, so focus never leaves on Tab or an arrow.

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

		struct Marker
		{
			int Line = 0;
			bool Error = true; // else a warning
			std::string Message;
		};
		void SetMarkers(std::vector<Marker> markers) { m_Markers = std::move(markers); }
		// The find bar's needle, every match painted (empty = none).
		void SetHighlight(std::string needle, bool matchCase) { m_Highlight = std::move(needle); m_HighlightCase = matchCase; }
		void SetShowMinimap(bool show) { m_ShowMinimap = show; }
		// Scrolls so the primary caret is in view at the next paint.
		void RevealCaret() { m_RevealCaret = true; }

		// The host's commands (the Script Editor panel's).
		void SetOnSave(std::function<void()> fn) { m_OnSave = std::move(fn); }
		void SetOnFind(std::function<void(bool replace)> fn) { m_OnFind = std::move(fn); }
		void SetOnGoToLine(std::function<void()> fn) { m_OnGoToLine = std::move(fn); }

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
		void OnFocusLost() override { m_HasFocus = false; m_Drag = Drag::None; }
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

		FontCache* m_FontCache;
		FontHandle m_Font;
		float m_FontSize;
		std::shared_ptr<const code::ITokenizer> m_Tokenizer;
		std::shared_ptr<code::EditorModel> m_Model = std::make_shared<code::EditorModel>();

		std::vector<Marker> m_Markers;
		std::string m_Highlight;
		bool m_HighlightCase = false;
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

		bool m_HasFocus = false;
		float m_LastActivity = -1.0f; // the anim time of the last edit / move (the caret stays solid)
		Drag m_Drag = Drag::None;
		code::TextPos m_DragAnchor;
		int m_ClickCount = 0;
		float m_LastClickTime = -10.0f;
		glm::vec2 m_LastClickPos{ 0.0f };
	};

} // namespace wisp
