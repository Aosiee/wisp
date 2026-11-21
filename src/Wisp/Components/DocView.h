#pragma once

// The Markdown view - paints a wisp::doc::Document (Wisp/Doc/Markdown.h, the
// parser tinydoc's site shares) as a scrolling page: headings, wrapped
// paragraphs, lists, callouts, tables and code blocks (coloured by the host's
// tokenizer for their language). Wisp knows no engine API: the host names doc
// links, says which resolve, opens them and supplies images. Every glyph goes
// through AddGlyph, every colour and metric through StyleTokens.
//
// Input: the wheel and the scrollbar thumb scroll; a click on a link calls
// the host's OnLink; with the focus, Up / Down / PageUp / PageDown / Home /
// End scroll.

#include "Wisp/Code/Tokenizer.h"
#include "Wisp/Core.h"
#include "Wisp/Doc/Markdown.h"
#include "Wisp/DrawList.h"
#include "Wisp/FontCache.h"

#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace wisp {

	class DocView : public Widget
	{
	public:
		WISP_WIDGET_TYPE(DocView)

		struct Host
		{
			// A doc link's text when the Markdown gives no label (`[[id]]`).
			std::function<std::string(const std::string& id)> LinkLabel;
			// False paints the link as dead.
			std::function<bool(const std::string& id)> LinkResolves;
			// A click: a doc id (`docLink`) or a url.
			std::function<void(const std::string& target, bool docLink)> OnLink;
			// The colouring for a fenced block's language (null = plain).
			std::function<std::shared_ptr<const code::ITokenizer>(const std::string& language)> Tokenizer;
			// An image's brush and pixel size (false = the alt text in a box).
			std::function<bool(const std::string& path, Brush& brush, glm::vec2& size)> Image;
		};

		explicit DocView(FontCache* fontCache, Host host = {});

		void SetHost(Host host)
		{
			m_Host = std::move(host);
			m_LayoutWidth = -1.0f;
		}
		// A new page; `keepPlace` holds the reader's place (the same top
		// block at the same offset - a reload of the open page).
		void SetDocument(doc::Document document, bool keepPlace = false);
		void SetMarkdown(std::string_view markdown, bool keepPlace = false) { SetDocument(doc::Parse(markdown), keepPlace); }
		const doc::Document& GetDocument() const { return m_Document; }

		// Scrolls a heading (its doc::Slug) to the top; false when the page has none.
		bool ScrollToAnchor(const std::string& slug);
		float GetScroll() const { return m_Scroll; }
		void SetScroll(float y) { m_Scroll = y; }
		// The laid-out page height (the last paint's width).
		float GetContentHeight() const { return m_ContentHeight; }

		glm::vec2 ComputeDesiredSize() const override { return { 320.0f, 200.0f }; }
		void ArrangeChildren(const Geometry&, ArrangedChildren&) const override {}
		int OnPaint(const PaintArgs& args, const Geometry& geom, DrawList& drawList, int layerId, const WidgetStyleContext& style) const override;

		Reply OnMouseButtonDown(const glm::vec2& pos, int button) override;
		Reply OnMouseButtonUp(const glm::vec2& pos, int button) override;
		Reply OnMouseMove(const glm::vec2& pos) override;
		Reply OnMouseWheel(const glm::vec2& pos, float delta) override;
		Reply OnMouseLeave() override;
		Reply OnKeyDown(int key, int mods) override;

	private:
		struct Style
		{
			FontHandle Font = 0;
			float Size = 14.0f;
			glm::vec4 Colour{ 1.0f };
			int Link = -1;
			int Code = -1; // the inline code span's id (its background box)
		};
		struct Run
		{
			glm::vec2 Pen{ 0.0f }; // content space: x, baseline
			std::string Text;
			FontHandle Font = 0;
			float Size = 14.0f;
			glm::vec4 Colour{ 1.0f };
			int Link = -1;
			float Width = 0.0f;
			int Clip = -1; // a box the run is clipped to (a code block's)
		};
		struct Box
		{
			glm::vec2 Pos{ 0.0f };
			glm::vec2 Size{ 0.0f };
			glm::vec4 Colour{ 1.0f };
			float Radius = 0.0f;
			float OutlineWidth = 0.0f;
			glm::vec4 OutlineColour{ 0.0f };
		};
		struct ImageRun
		{
			glm::vec2 Pos{ 0.0f };
			glm::vec2 Size{ 0.0f };
			Brush ImageBrush;
		};
		struct LinkTarget
		{
			std::string Target;
			bool Doc = false;
		};
		struct LinkRect
		{
			glm::vec2 Pos{ 0.0f };
			glm::vec2 Size{ 0.0f };
			int Link = -1;
		};

		// Layout (paint-time, cached by width).
		void Relayout(float width) const;
		void LayoutBlocks(const std::vector<doc::Block>& blocks, float x, float width, float& y, bool tight) const;
		void LayoutBlock(const doc::Block& block, float x, float width, float& y, bool tight) const;
		// Flows inlines into [x, x + width) from the line at `y` (its top);
		// returns the height used. `measureOnly` emits nothing.
		float FlowInlines(const std::vector<doc::Inline>& inlines, float x, float width, float y, const Style& base, bool measureOnly,
			float* naturalWidth = nullptr, float* longestWord = nullptr) const;
		void Flatten(const std::vector<doc::Inline>& inlines, const Style& style, bool measureOnly,
			std::vector<std::pair<std::string, Style>>& out) const;
		void LayoutCode(const doc::Block& block, float x, float width, float& y) const;
		void LayoutTable(const doc::Block& block, float x, float width, float& y) const;
		float MeasureWord(const std::string& text, FontHandle font, float size) const;
		float LineHeightOf(FontHandle font, float size) const;
		float AscentOf(FontHandle font, float size) const;
		int LinkAt(const glm::vec2& absolute) const;
		float MaxScroll() const { return std::max(0.0f, m_ContentHeight - m_ViewHeight); }
		void ClampScroll() const { m_Scroll = std::clamp(m_Scroll, 0.0f, MaxScroll()); }
		bool ThumbRect(glm::vec2& pos, glm::vec2& size) const;

		FontCache* m_FontCache;
		Host m_Host;
		doc::Document m_Document;

		// The last layout.
		mutable float m_LayoutWidth = -1.0f;
		mutable std::vector<Run> m_Runs;
		mutable std::vector<Box> m_Boxes;
		mutable std::vector<ImageRun> m_Images;
		mutable std::vector<LinkTarget> m_Links;
		mutable std::vector<LinkRect> m_LinkRects;
		mutable std::vector<std::pair<std::string, float>> m_Anchors; // slug, y
		mutable std::vector<float> m_BlockTops;                       // each top-level block's y
		mutable float m_ContentHeight = 0.0f;
		mutable int m_NextCodeSpan = 0;

		// The view (paint-time).
		mutable glm::vec2 m_Origin{ 0.0f };
		mutable glm::vec2 m_ViewSize{ 0.0f };
		mutable float m_ViewHeight = 0.0f;
		mutable float m_Scroll = 0.0f;
		mutable int m_PendingBlock = -1; // keepPlace: restore this block's top...
		mutable float m_PendingOffset = 0.0f;
		mutable std::string m_PendingAnchor;

		int m_HoveredLink = -1;
		int m_PressedLink = -1;
		bool m_DraggingThumb = false;
		float m_DragGrab = 0.0f;
	};

} // namespace wisp
