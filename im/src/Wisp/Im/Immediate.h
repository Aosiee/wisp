#pragma once

// wisp::im - the immediate-mode layer: code that describes its UI every
// frame and reads the results back, on the retained widgets.
//
//     auto host = std::make_unique<wisp::ImmediateHost>(fonts, font, [&](wisp::im::Context& ui) {
//         ui.Text("Frame {:.1f} ms", frameMs);
//         ui.Slider("Exposure", &exposure, -4.0f, 4.0f);
//         if (ui.Button("Reset"))
//             exposure = 0.0f;
//     });
//
// An ImmediateHost is an ordinary widget. Its frame function runs once a
// frame, at the start of its window's update (after the frame's events,
// before layout). Each call is matched by its id to the widget the host
// made for it: the first call creates the widget, later ones update it,
// and a widget no call reached this frame is destroyed. The widgets are
// Wisp's own (Button, Slider, EditableText, ...), so they look and behave
// like every other.
//
// Ids: a call's label is its id within the enclosing scope ("Label##key"
// shows "Label" and keys on the whole string); PushId/PopId separate the
// items of a loop. Label-less calls (Text, Separator, Spacing, ProgressBar,
// Image, table rows) key on their position among the calls of their kind
// in the scope, so a text that changes every frame keeps its widget.
//
// Results are what the widget saw since the last frame: Button returns true
// once per click, and a value call writes the edited value through its
// pointer and returns true on the frame after the edit. A value the program
// changed is shown by the widget.

#include "Wisp/Core.h"
#include "Wisp/DrawList.h"
#include "Wisp/FontCache.h"
#include "Wisp/StyleTokens.h"

#include <cstdint>
#include <format>
#include <functional>
#include <initializer_list>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

namespace wisp {
	class BoxPanel;
	class ImmediateHost;
}

namespace wisp::im {

	using Id = uint64_t;

	// The slot the next item takes in its box (Context::Next).
	struct SlotOptions
	{
		Slot Data;
		bool IsSet = false;

		SlotOptions& Padding(float uniform) { Data.SetPadding(uniform); IsSet = true; return *this; }
		SlotOptions& Padding(float h, float v) { Data.SetPadding(h, v); IsSet = true; return *this; }
		SlotOptions& Padding(float left, float top, float right, float bottom)
		{
			Data.PaddingLeft = left;
			Data.PaddingTop = top;
			Data.PaddingRight = right;
			Data.PaddingBottom = bottom;
			IsSet = true;
			return *this;
		}
		SlotOptions& Fill(float weight = 1.0f) { Data.FillWeight = weight; IsSet = true; return *this; }
		SlotOptions& HAlign(wisp::HAlign h) { Data.Align.H = h; IsSet = true; return *this; }
		SlotOptions& VAlign(wisp::VAlign v) { Data.Align.V = v; IsSet = true; return *this; }
		SlotOptions& MinWidth(float w) { Data.MinWidth = w; IsSet = true; return *this; }
		SlotOptions& MinHeight(float h) { Data.MinHeight = h; IsSet = true; return *this; }
		SlotOptions& MaxWidth(float w) { Data.MaxWidth = w; IsSet = true; return *this; }
		SlotOptions& MaxHeight(float h) { Data.MaxHeight = h; IsSet = true; return *this; }
	};

	// The measurements the layout calls use (Context::GetStyle).
	struct LayoutStyle
	{
		float FontSize = tokens::FontBody;
		// BeginField's key column and the field row's padding.
		float FieldKeyWidth = 120.0f;
		float FieldPaddingX = 10.0f;
		float FieldPaddingY = 4.0f;
		// TreeNode's body and Indent's default.
		float IndentWidth = 16.0f;
		// Plot's line.
		glm::vec4 PlotColour = tokens::Accent;
	};

	class Context
	{
	public:
		Context(FontCache* fonts, FontHandle font);
		~Context();
		Context(const Context&) = delete;
		Context& operator=(const Context&) = delete;

		LayoutStyle& GetStyle() { return m_Style; }

		// ---- Ids ----
		void PushId(std::string_view id);
		void PushId(int64_t id);
		void PopId();

		// ---- Layout ----
		// The slot of the next item placed in a box (padding, fill,
		// alignment, min/max); consumed by that item.
		SlotOptions& Next();
		// Left padding for the following items of this scope.
		void Indent(float width = -1.0f);
		void Unindent(float width = -1.0f);

		void BeginRow(std::string_view id = {});
		void EndRow();
		void BeginColumn(std::string_view id = {});
		void EndColumn();
		// A vertical scroll box (its content fitted to its width); `height`
		// fixes the viewport height (0: as tall as its slot).
		void BeginScroll(std::string_view id = {}, float height = 0.0f);
		void EndScroll();
		// A row with the label in a key column, then the field's items
		// (filling the rest by default).
		void BeginField(std::string_view label);
		void EndField();
		// A collapsible titled section, open the first time; returns whether
		// it is open - call End only then.
		bool Begin(std::string_view title);
		void End();
		// A collapsible, indented node; returns whether it is open - call
		// TreePop only then.
		bool TreeNode(std::string_view label, bool defaultOpen = false);
		void TreePop();
		// Rows of `columns` equal cells: TableNextRow before each row's
		// cells (a row with fewer cells is padded).
		void BeginTable(std::string_view id, int columns);
		void TableNextRow();
		void EndTable();

		void Separator();
		void Spacing(float width, float height);

		// ---- Read-outs ----
		void Text(std::string_view text);
		template <typename Arg, typename... Args>
		void Text(std::format_string<Arg, Args...> format, Arg&& arg, Args&&... args)
		{
			m_Scratch.clear();
			std::format_to(std::back_inserter(m_Scratch), format, std::forward<Arg>(arg), std::forward<Args>(args)...);
			Text(std::string_view(m_Scratch));
		}
		void TextColoured(const glm::vec4& colour, std::string_view text);
		// A muted section heading.
		void Heading(std::string_view text);
		// A banded key/value row (the bands alternate through the scope).
		void KeyValue(std::string_view key, std::string_view value);
		void ProgressBar(float fraction);
		// A line of `values` (oldest first), `height` tall.
		void Plot(std::string_view id, std::span<const float> values, float height = 40.0f);
		void Image(const Brush& brush, const glm::vec2& size);

		// ---- Controls ----
		bool Button(std::string_view label);
		bool Checkbox(std::string_view label, bool* value);
		bool Radio(std::string_view id, int* index, std::span<const std::string> options);
		bool Radio(std::string_view id, int* index, std::initializer_list<std::string_view> options);
		bool Slider(std::string_view id, float* value, float min, float max);
		bool Slider(std::string_view id, int* value, int min, int max);
		bool DragFloat(std::string_view id, float* value, float step = 0.1f);
		bool DragInt(std::string_view id, int* value, int min = 0, int max = 0);
		bool InputText(std::string_view id, std::string* text, std::string_view placeholder = {});
		bool Dropdown(std::string_view id, int* index, std::span<const std::string> options);
		bool Dropdown(std::string_view id, int* index, std::initializer_list<std::string_view> options);
		bool ColourEdit(std::string_view id, glm::vec4* colour);

		// Any retained widget: `factory` makes it the first time (it returns a
		// std::unique_ptr<T> or one of a type derived from T); later frames
		// return the same widget for the caller to update.
		template <typename T, typename Factory>
		T* Widget(std::string_view id, Factory&& factory)
		{
			return static_cast<T*>(PlaceCustom(id, TypeTag<T>(),
				[&]() -> std::unique_ptr<wisp::Widget> { return std::unique_ptr<wisp::Widget>(factory()); }));
		}

		// For tests and tools: the nodes the host keeps, the frames run and
		// the scopes the last frame left open (closed for it).
		size_t GetNodeCount() const { return m_Nodes.size(); }
		uint64_t GetFrameCount() const { return m_Frame; }
		uint32_t GetUnbalancedScopes() const { return m_UnbalancedScopes; }

	private:
		friend class wisp::ImmediateHost;

		enum class Kind : uint8_t
		{
			Root, Text, Heading, KeyValue, Progress, Plot, Image, Separator, Spacing,
			Button, Checkbox, Radio, SliderFloat, SliderInt, DragFloat, DragInt, InputText, Dropdown, Colour,
			Custom, Row, Column, Scroll, Field, Section, Tree, Table, TableRow, Count
		};
		struct Node;
		struct Scope;

		template <typename T>
		static const void* TypeTag()
		{
			static const char tag = 0;
			return &tag;
		}

		void BeginFrame(BoxPanel* root);
		void EndFrame();

		Id MakeId(Kind kind, std::string_view label);
		Id MakePositionalId(Kind kind);
		Node* Find(Id id, Kind kind);
		Node* Create(Id id, Kind kind, std::unique_ptr<wisp::Widget> outer);
		void Reach(Node* node, const Slot* preferred);
		void OpenScope(Node* container, Kind kind);
		void CloseScope(Kind kind);
		void CloseRow();
		void Reconcile(Scope& scope);
		void ClearChildren(Node* container);
		void Detach(Node* node);
		void Forget(Node* node);
		bool Options(Node* node, std::span<const std::string> options);
		wisp::Widget* PlaceCustom(std::string_view id, const void* tag, const std::function<std::unique_ptr<wisp::Widget>()>& factory);
		Node* PlaceContainer(Kind kind, std::string_view id, const Slot* preferred,
			const std::function<std::unique_ptr<wisp::Widget>(Node&)>& make);
		bool PlaceDisclosure(Kind kind, std::string_view label, bool defaultOpen);

		FontCache* m_Fonts = nullptr;
		FontHandle m_Font = InvalidFont;
		LayoutStyle m_Style;
		std::unordered_map<Id, std::unique_ptr<Node>> m_Nodes;
		std::unique_ptr<Node> m_Root;
		std::vector<Scope> m_Scopes; // the open scopes, [0] the root; reused frame to frame
		size_t m_Depth = 0;
		std::vector<Id> m_IdStack;
		SlotOptions m_Next;
		std::string m_Scratch;
		std::vector<std::string> m_OptionScratch;
		uint64_t m_Frame = 0;
		uint32_t m_UnbalancedScopes = 0;
	};

} // namespace wisp::im

namespace wisp {

	// The widget that holds an immediate scope: its frame function describes
	// its content every frame. It lays out as its content (a vertical box).
	class ImmediateHost : public Widget
	{
	public:
		WISP_WIDGET_TYPE(ImmediateHost)

		using FrameFn = std::function<void(im::Context&)>;

		ImmediateHost(FontCache* fonts, FontHandle font, FrameFn frame);
		~ImmediateHost() override;

		// Runs the frame function once. The host's frame callback calls it
		// every frame of its window; a test or a host with no window calls it
		// directly.
		void RunFrame();
		im::Context& GetContext() { return *m_Context; }
		BoxPanel& GetContent() { return *m_Root; }

		glm::vec2 ComputeDesiredSize() const override;
		void ArrangeChildren(const Geometry& geom, ArrangedChildren& out) const override;
		int OnPaint(const PaintArgs& args, const Geometry& geom, DrawList& drawList, int layerId,
			const WidgetStyleContext& style) const override;
		void ForEachChild(std::function<void(Widget*)> fn) override;

	private:
		std::unique_ptr<BoxPanel> m_Root;
		std::unique_ptr<im::Context> m_Context;
		FrameFn m_Frame;
		uint32_t m_Callback = 0;
	};

} // namespace wisp
