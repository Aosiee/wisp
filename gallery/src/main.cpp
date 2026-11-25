// wisp_gallery: every base widget, the dock (with a panel in a window of its
// own), the code editor, a graph canvas, the charts and the document view,
// as one applet. Windowed it is something to look at and use; offscreen it
// is Wisp's capture check:
//
//     wisp_gallery --display-mode offscreen --render-target 1280x800 --capture-ui gallery.png
//
// `--widgets immediate` builds the Widgets panel with wisp::im instead: the
// same widgets described every frame, which must capture the same image.

#include "Wisp/Applet/Applet.h"

#include "Wisp/Application.h"
#include "Wisp/Widgets.h"
#include "Wisp/Code/Tokenizer.h"
#include "Wisp/Components/CodeEditor.h"
#include "Wisp/Components/ColourPicker.h"
#include "Wisp/Components/DocView.h"
#include "Wisp/Composites/ToolbarButton.h"
#include "Wisp/Docking/DockWindowManager.h"
#include "Wisp/Graph/GraphCanvas.h"
#include "Wisp/Graph/GraphSchema.h"
#include "Wisp/Icons.h"
#include "Wisp/Im/Immediate.h"
#include "Wisp/Kit/PanelKit.h"
#include "Wisp/StyleTokens.h"
#include "Wisp/Version.h"

#include "hearth/App.h"

#include <cmath>
#include <memory>
#include <string>
#include <vector>

namespace gallery {

	namespace tokens = wisp::tokens;

	constexpr const char* kCode = R"(-- A Luau module, highlighted by Wisp's tokenizer.
local Walker = {}
Walker.__index = Walker

function Walker.new(speed: number)
	return setmetatable({ Speed = speed, Distance = 0 }, Walker)
end

function Walker:Step(dt: number)
	self.Distance += self.Speed * dt
	if self.Distance > 100 then
		self.Distance = 0 -- around again
	end
end

return Walker
)";

	constexpr const char* kDocument = R"(# Wisp

A **retained-mode** UI library: widgets, layout, popups and menus, drag and
drop, animation and a docking system with torn-out windows.

## What this gallery shows

- every base widget, in the *Widgets* panel
- the code editor and the graph canvas
- line graphs, bar charts and sparklines
- this page: Wisp's own Markdown view

> Wisp draws nothing by itself: the program's renderer records its batches.

```lua
local ui = require("wisp")
ui.Button("Hello")
```
)";

	// A small state machine, laid out by hand.
	class Schema final : public wisp::graph::GraphSchema
	{
	public:
		void Build(wisp::graph::GraphView& out, wisp::graph::NodeMap& nodes) override
		{
			struct Node { const char* Title; const char* Subtitle; float X, Y; uint32_t Flags; };
			const Node list[] = {
				{ "Entry", "", 0.0f, 120.0f, wisp::graph::Pill | wisp::graph::Root },
				{ "Idle", "loop", 180.0f, 40.0f, wisp::graph::Active },
				{ "Walk", "blend 1D", 400.0f, 40.0f, 0 },
				{ "Jump", "one shot", 400.0f, 200.0f, 0 },
				{ "Land", "", 180.0f, 200.0f, wisp::graph::NodeWarning },
			};
			out.Shape = wisp::graph::EdgeShape::Straight;
			for (size_t i = 0; i < std::size(list); i++)
			{
				wisp::graph::ViewNode node;
				node.Id = i + 1;
				node.Title = list[i].Title;
				node.Subtitle = list[i].Subtitle;
				node.Position = { list[i].X, list[i].Y };
				node.Size = { 140.0f, 56.0f };
				node.Flags = list[i].Flags;
				out.Nodes.push_back(node);
				nodes[node.Id] = node.Id;
			}
			const std::pair<uint64_t, uint64_t> edges[] = { { 1, 2 }, { 2, 3 }, { 3, 2 }, { 2, 4 }, { 4, 5 }, { 5, 2 } };
			for (const auto& [from, to] : edges)
			{
				wisp::graph::ViewEdge edge;
				edge.From = from;
				edge.To = to;
				out.Edges.push_back(edge);
			}
			out.Edges[0].Flags = wisp::graph::Emphasis;
			out.Edges[3].Label = "jump";
		}
		bool NodesMovable() const override { return true; }
	};

	class Gallery
	{
	public:
		Gallery(wisp::Applet& applet, bool immediateWidgets)
			: m_Applet(applet), m_ImmediateWidgets(immediateWidgets)
		{
			m_Fonts = &applet.Fonts();
			m_Font = applet.GetHouseFaces().Regular;

			m_State = std::make_shared<wisp::DockState>();
			Register("Widgets", "Widgets", wisp::icons::Sliders,
				[this] { return m_ImmediateWidgets ? BuildWidgetsImmediate() : BuildWidgets(); });
			Register("Code", "Code", wisp::icons::Code, [this] { return BuildCode(); });
			Register("Document", "Document", wisp::icons::File, [this] { return BuildDocument(); });
			Register("Graph", "Graph", wisp::icons::Nodes, [this] { return BuildGraph(); });
			Register("Charts", "Charts", wisp::icons::BarChart, [this] { return BuildCharts(); });
			Register("Colour", "Colour", wisp::icons::Image, [this] { return BuildColour(); });
			m_State->SetLayout(DefaultLayout());

			auto dock = std::make_unique<wisp::DockSpace>(m_Fonts, m_Font, tokens::FontBody);
			dock->SetState(m_State);
			wisp::DockSpace* dockRaw = dock.get();

			auto column = std::make_unique<wisp::VerticalBox>(wisp::BoxOrientation::Vertical);
			*column
				+ column->AddSlot()[BuildHeader()]
				+ column->AddSlot().Fill(1.0f)[std::move(dock)];
			auto root = std::make_unique<wisp::Border>();
			root->SetBrush({ tokens::Backdrop }).SetPadding(0.0f).SetChild(std::move(column));
			applet.SetRoot(std::move(root));

			m_Windows.Attach(m_State, dockRaw, m_Fonts, m_Font, tokens::FontBody, &applet);
			// The dock's windows and the charts' data, once a frame before
			// the presents.
			m_Client = applet.GetApp().GetLoop().Add(hearth::Phase::Update, 200, "gallery.update",
				[this](const hearth::Frame& frame) { Update(frame); });
		}

		~Gallery()
		{
			m_Applet.GetApp().GetLoop().Remove(m_Client);
			m_Windows.Shutdown();
		}

	private:
		void Register(const char* id, const char* name, const char* icon, std::function<std::unique_ptr<wisp::Widget>()> build)
		{
			wisp::DockState::PanelDef def;
			def.DisplayName = name;
			def.Factory = [build = std::move(build)]() -> std::shared_ptr<wisp::Widget> { return build(); };
			def.Singleton = true;
			m_State->RegisterPanel(id, std::move(def));
			m_State->SetPanelIcon(id, icon);
		}

		// Widgets on the left; Code and Document over Graph and Charts on the
		// right; Colour in a second window.
		wisp::dock::DockLayout DefaultLayout()
		{
			namespace dock = wisp::dock;
			dock::DockLayout layout;
			layout.Windows.clear();
			dock::WindowNode main;
			main.Id = layout.NextNodeId++;
			main.bMain = true;
			main.Size = { 1280.0f, 800.0f };
			dock::DockNode widgets = dock::MakeTabWell(layout, { { "Widgets" } }, 0);
			dock::DockNode top = dock::MakeTabWell(layout, { { "Code" }, { "Document" } }, 0);
			dock::DockNode bottom = dock::MakeTabWell(layout, { { "Graph" }, { "Charts" } }, 0);
			std::vector<dock::DockNode> rightChildren;
			rightChildren.push_back(std::move(top));
			rightChildren.push_back(std::move(bottom));
			dock::DockNode right = dock::MakeSplit(layout, dock::Axis::Vertical, std::move(rightChildren), { 0.55f, 0.45f });
			std::vector<dock::DockNode> mainChildren;
			mainChildren.push_back(std::move(widgets));
			mainChildren.push_back(std::move(right));
			main.Main = dock::MakeSplit(layout, dock::Axis::Horizontal, std::move(mainChildren), { 0.36f, 0.64f });
			layout.Windows.push_back(std::move(main));

			dock::WindowNode second;
			second.Id = layout.NextNodeId++;
			second.Size = { 420.0f, 460.0f };
			second.Main = dock::MakeTabWell(layout, { { "Colour" } }, 0);
			layout.Windows.push_back(std::move(second));
			dock::NormalizeLayout(layout);
			return layout;
		}

		std::unique_ptr<wisp::Widget> Text(const std::string& text, const glm::vec4& colour = tokens::TextPrimary, float size = tokens::FontBody)
		{
			return std::make_unique<wisp::TextBlock>(m_Fonts, m_Font, size, text, colour);
		}

		std::unique_ptr<wisp::Widget> BuildHeader()
		{
			auto row = std::make_unique<wisp::HorizontalBox>(wisp::BoxOrientation::Horizontal);
			const wisp::FontHandle semiBold = wisp::UIApplication::FontOr(wisp::FontRole::SemiBold, m_Font);
			*row
				+ row->AddSlot().VAlign(wisp::VAlign::Center).Padding(10.0f, 0.0f)[
					std::make_unique<wisp::TextBlock>(m_Fonts, semiBold, tokens::FontTitle, "Wisp Gallery", tokens::TextPrimary)]
				+ row->AddSlot().VAlign(wisp::VAlign::Center)[wisp::kit::MakePill(m_Fonts, m_Font, WISP_VERSION_STRING, tokens::Accent)]
				+ row->AddSlot().Fill(1.0f)[std::make_unique<wisp::Spacer>(glm::vec2(1.0f, 1.0f))];
			for (const auto& [icon, tip] : { std::pair{ wisp::icons::Play, "Play" }, std::pair{ wisp::icons::Save, "Save" },
					 std::pair{ wisp::icons::Settings, "Settings" } })
			{
				wisp::ToolbarButtonDesc desc;
				desc.Icon = icon;
				desc.Tooltip = tip;
				*row + row->AddSlot().VAlign(wisp::VAlign::Center).Padding(2.0f, 0.0f)[wisp::MakeToolbarButton(m_Fonts, m_Font, desc, [] {})];
			}
			auto bar = std::make_unique<wisp::Border>();
			bar->SetBrush({ tokens::Raised }).SetPadding(4.0f, 4.0f).SetChild(std::move(row));
			return bar;
		}

		void Section(wisp::VerticalBox& column, const std::string& title)
		{
			wisp::kit::AddSectionHeading(column, m_Fonts, m_Font, title);
		}

		void Row(wisp::VerticalBox& column, const std::string& label, std::unique_ptr<wisp::Widget> widget)
		{
			auto row = std::make_unique<wisp::HorizontalBox>(wisp::BoxOrientation::Horizontal);
			auto key = std::make_unique<wisp::SizeBox>();
			key->SetWidthOverride(120.0f);
			key->SetChild(Text(label, tokens::TextSecondary));
			*row
				+ row->AddSlot().VAlign(wisp::VAlign::Center)[std::move(key)]
				+ row->AddSlot().Fill(1.0f).VAlign(wisp::VAlign::Center)[std::move(widget)];
			column + column.AddSlot().Padding(10.0f, 4.0f)[std::move(row)];
		}

		std::unique_ptr<wisp::Widget> BuildWidgets()
		{
			auto column = std::make_unique<wisp::VerticalBox>(wisp::BoxOrientation::Vertical);

			Section(*column, "Buttons");
			{
				auto buttons = std::make_unique<wisp::HorizontalBox>(wisp::BoxOrientation::Horizontal);
				for (const char* label : { "Apply", "Cancel" })
				{
					auto button = std::make_unique<wisp::Button>();
					button->SetChild(Text(label));
					*buttons + buttons->AddSlot().Padding(0.0f, 0.0f)[std::move(button)];
					*buttons + buttons->AddSlot()[std::make_unique<wisp::Spacer>(glm::vec2(8.0f, 1.0f))];
				}
				Row(*column, "Button", std::move(buttons));
			}
			{
				auto check = std::make_unique<wisp::CheckBox>();
				check->SetLabel("Snap to grid");
				check->SetChecked(true);
				Row(*column, "CheckBox", std::move(check));
			}
			{
				auto radios = std::make_unique<wisp::RadioButtons>(m_Fonts, m_Font, tokens::FontBody);
				radios->SetOptions({ "Local", "World", "Parent" });
				radios->SetSelectedIndex(1);
				Row(*column, "RadioButtons", std::move(radios));
			}

			Section(*column, "Values");
			{
				auto slider = std::make_unique<wisp::Slider>();
				slider->SetValue(0.35f);
				Row(*column, "Slider", std::move(slider));
			}
			{
				auto value = std::make_unique<wisp::SFloatBox>(m_Fonts, m_Font, tokens::FontBody, 1.25f);
				value->SetStep(0.05f);
				value->SetColourIndicator(tokens::Error);
				value->SetIndicatorLabel("X");
				Row(*column, "Float", std::move(value));
			}
			{
				auto value = std::make_unique<wisp::SIntBox>(m_Fonts, m_Font, tokens::FontBody, 42);
				value->SetClampRange(0, 100);
				Row(*column, "Int", std::move(value));
			}
			{
				auto progress = std::make_unique<wisp::ProgressBar>();
				progress->SetValue(0.62f);
				Row(*column, "ProgressBar", std::move(progress));
			}
			{
				auto throbber = std::make_unique<wisp::Throbber>();
				throbber->SetColour(tokens::Accent);
				Row(*column, "Throbber", std::move(throbber));
			}

			Section(*column, "Text");
			{
				auto field = std::make_unique<wisp::EditableText>(m_Fonts, m_Font, tokens::FontBody);
				field->SetPlaceholder("Search assets...");
				Row(*column, "EditableText", std::move(field));
			}
			{
				auto field = std::make_unique<wisp::EditableText>(m_Fonts, m_Font, tokens::FontBody, "Crate_01");
				Row(*column, "With text", std::move(field));
			}
			{
				auto dropdown = std::make_unique<wisp::Dropdown>(m_Fonts, m_Font, tokens::FontBody);
				dropdown->SetOptions({ "Linear", "Nearest", "Anisotropic" });
				dropdown->SetSelectedIndex(0);
				Row(*column, "Dropdown", std::move(dropdown));
			}

			Section(*column, "Read-outs");
			{
				auto pills = std::make_unique<wisp::HorizontalBox>(wisp::BoxOrientation::Horizontal);
				const std::pair<const char*, glm::vec4> list[] = { { "Ready", tokens::Success }, { "2 warnings", tokens::Warning },
					{ "Error", tokens::Error } };
				for (const auto& [label, colour] : list)
				{
					*pills + pills->AddSlot()[wisp::kit::MakePill(m_Fonts, m_Font, label, colour)];
					*pills + pills->AddSlot()[std::make_unique<wisp::Spacer>(glm::vec2(6.0f, 1.0f))];
				}
				Row(*column, "Pills", std::move(pills));
			}
			int band = 0;
			for (const auto& [key, value] : { std::pair{ "Frame", "16.6 ms" }, std::pair{ "Draw calls", "128" },
					 std::pair{ "Triangles", "1.2 M" } })
			{
				if (wisp::TextBlock* text = wisp::kit::AddKeyValue(*column, m_Fonts, m_Font, key, band++))
					text->SetText(value);
			}
			*column + column->AddSlot()[std::make_unique<wisp::Separator>()];

			auto scroll = std::make_unique<wisp::ScrollBox>();
			scroll->SetFitWidth(true);
			scroll->SetChild(std::move(column));
			return Panel(std::move(scroll));
		}

		// The Widgets panel again, described every frame: each row is the
		// retained one's call, and the widgets wisp::im has no call for
		// (the float box with its indicator, the throbber, the pills) are
		// placed as retained widgets.
		std::unique_ptr<wisp::Widget> BuildWidgetsImmediate()
		{
			auto host = std::make_unique<wisp::ImmediateHost>(m_Fonts, m_Font, [this](wisp::im::Context& ui) {
				ui.Next().Fill(1.0f);
				ui.BeginScroll("scroll");

				ui.Heading("Buttons");
				ui.BeginField("Button");
				ui.BeginRow();
				for (const char* label : { "Apply", "Cancel" })
				{
					ui.Button(label);
					ui.Spacing(8.0f, 1.0f);
				}
				ui.EndRow();
				ui.EndField();
				ui.BeginField("CheckBox");
				ui.Checkbox("Snap to grid", &m_Snap);
				ui.EndField();
				ui.BeginField("RadioButtons");
				ui.Radio("##space", &m_Space, { "Local", "World", "Parent" });
				ui.EndField();

				ui.Heading("Values");
				ui.BeginField("Slider");
				ui.Slider("##slider", &m_Amount, 0.0f, 1.0f);
				ui.EndField();
				ui.BeginField("Float");
				ui.Widget<wisp::SFloatBox>("float", [this] {
					auto value = std::make_unique<wisp::SFloatBox>(m_Fonts, m_Font, tokens::FontBody, 1.25f);
					value->SetStep(0.05f);
					value->SetColourIndicator(tokens::Error);
					value->SetIndicatorLabel("X");
					return value;
				});
				ui.EndField();
				ui.BeginField("Int");
				ui.DragInt("##int", &m_Count, 0, 100);
				ui.EndField();
				ui.BeginField("ProgressBar");
				ui.ProgressBar(0.62f);
				ui.EndField();
				ui.BeginField("Throbber");
				ui.Widget<wisp::Throbber>("throbber", [] {
					auto throbber = std::make_unique<wisp::Throbber>();
					throbber->SetColour(tokens::Accent);
					return throbber;
				});
				ui.EndField();

				ui.Heading("Text");
				ui.BeginField("EditableText");
				ui.InputText("##search", &m_Search, "Search assets...");
				ui.EndField();
				ui.BeginField("With text");
				ui.InputText("##name", &m_Name);
				ui.EndField();
				ui.BeginField("Dropdown");
				ui.Dropdown("##filter", &m_Filter, { "Linear", "Nearest", "Anisotropic" });
				ui.EndField();

				ui.Heading("Read-outs");
				ui.BeginField("Pills");
				ui.BeginRow();
				const std::pair<const char*, glm::vec4> pills[] = { { "Ready", tokens::Success }, { "2 warnings", tokens::Warning },
					{ "Error", tokens::Error } };
				for (const auto& [label, colour] : pills)
				{
					ui.Widget<wisp::Border>(label, [&] { return wisp::kit::MakePill(m_Fonts, m_Font, label, colour); });
					ui.Spacing(6.0f, 1.0f);
				}
				ui.EndRow();
				ui.EndField();
				ui.KeyValue("Frame", "16.6 ms");
				ui.KeyValue("Draw calls", "128");
				ui.KeyValue("Triangles", "1.2 M");
				ui.Separator();

				ui.EndScroll();
			});
			return Panel(std::move(host));
		}

		std::unique_ptr<wisp::Widget> Panel(std::unique_ptr<wisp::Widget> content)
		{
			auto panel = std::make_unique<wisp::Border>();
			panel->SetBrush({ tokens::Panel }).SetPadding(0.0f).SetChild(std::move(content));
			return panel;
		}

		std::unique_ptr<wisp::Widget> BuildCode()
		{
			const wisp::FontHandle mono = wisp::UIApplication::FontOr(wisp::FontRole::Mono, m_Font);
			auto editor = std::make_unique<wisp::CodeEditor>(m_Fonts, mono, tokens::FontBody,
				std::make_shared<wisp::code::LuauTokenizer>());
			editor->SetText(kCode);
			return Panel(std::move(editor));
		}

		std::unique_ptr<wisp::Widget> BuildDocument()
		{
			wisp::DocView::Host host;
			host.Tokenizer = [](const std::string&) -> std::shared_ptr<const wisp::code::ITokenizer> {
				return std::make_shared<wisp::code::LuauTokenizer>();
			};
			auto view = std::make_unique<wisp::DocView>(m_Fonts, host);
			view->SetMarkdown(kDocument);
			return Panel(std::move(view));
		}

		std::unique_ptr<wisp::Widget> BuildGraph()
		{
			auto canvas = std::make_unique<wisp::graph::GraphCanvas>(m_Fonts, m_Font, &m_Schema, "gallery.graph");
			canvas->SetDrawDotGrid(true);
			canvas->Rebuild();
			return Panel(std::move(canvas));
		}

		std::unique_ptr<wisp::Widget> BuildCharts()
		{
			auto column = std::make_unique<wisp::VerticalBox>(wisp::BoxOrientation::Vertical);
			auto graph = std::make_unique<wisp::LineGraph>(m_Fonts, m_Font, tokens::FontSmall);
			graph->SetData(m_Data);
			graph->SetVisibleWindow(240);
			*column + column->AddSlot().Fill(1.0f).Padding(8.0f)[std::move(graph)];

			auto bars = std::make_unique<wisp::BarChart>(m_Fonts, m_Font, tokens::FontSmall);
			bars->SetBars({ { "Shadows", 1.8f, tokens::Accent }, { "Lighting", 3.1f, tokens::Success },
				{ "Post", 0.9f, tokens::Warning }, { "UI", 0.4f, tokens::Error } });
			*column + column->AddSlot().Fill(0.6f).Padding(8.0f)[std::move(bars)];

			auto spark = std::make_unique<wisp::Sparkline>();
			spark->SetSeries(&m_Data->Series("Frame"));
			spark->SetColor(tokens::Accent);
			spark->SetWindow(120);
			auto sparkBox = std::make_unique<wisp::SizeBox>();
			sparkBox->SetHeightOverride(28.0f);
			sparkBox->SetChild(std::move(spark));
			*column + column->AddSlot().Padding(8.0f)[std::move(sparkBox)];
			return Panel(std::move(column));
		}

		std::unique_ptr<wisp::Widget> BuildColour()
		{
			auto state = std::make_shared<wisp::ColourPickerState>();
			state->Colour = tokens::Accent;
			auto wheel = std::make_unique<wisp::ColourPickerWheel>(state);
			wheel->SetDesiredSize({ 240.0f, 240.0f });
			auto column = std::make_unique<wisp::VerticalBox>(wisp::BoxOrientation::Vertical);
			*column
				+ column->AddSlot().HAlign(wisp::HAlign::Center).Padding(12.0f)[std::move(wheel)]
				+ column->AddSlot().Padding(12.0f, 4.0f)[std::make_unique<wisp::ColourPickerPreview>(state)];
			return Panel(std::move(column));
		}

		void Update(const hearth::Frame& frame)
		{
			// Deterministic series (by frame, not by time), so a capture
			// repeats exactly.
			const double t = (double)frame.Index / 60.0;
			m_Data->Series("Frame").Push((float)(16.0 + 3.0 * std::sin(t * 2.1) + 1.5 * std::sin(t * 7.3)));
			m_Data->Series("GPU").Push((float)(11.0 + 2.0 * std::sin(t * 1.3 + 1.0)));
			m_Windows.Update();
		}

		wisp::Applet& m_Applet;
		bool m_ImmediateWidgets = false;
		// The immediate Widgets panel's values.
		bool m_Snap = true;
		int m_Space = 1;
		float m_Amount = 0.35f;
		int m_Count = 42;
		std::string m_Search;
		std::string m_Name = "Crate_01";
		int m_Filter = 0;
		wisp::FontCache* m_Fonts = nullptr;
		wisp::FontHandle m_Font = wisp::InvalidFont;
		std::shared_ptr<wisp::DockState> m_State;
		wisp::DockWindowManager m_Windows;
		Schema m_Schema;
		std::shared_ptr<wisp::GraphData> m_Data = std::make_shared<wisp::GraphData>();
		hearth::ClientId m_Client = 0;
	};

} // namespace gallery

int main(int argc, char** argv)
{
	hearth::AppDesc defaults;
	defaults.Title = "Wisp Gallery";
	defaults.Width = 1280;
	defaults.Height = 800;
	defaults.MinWidth = 640;
	defaults.MinHeight = 400;
	defaults.AppName = "wisp_gallery";
	hearth::AppDesc desc = hearth::App::ParseArgs(argc, argv, defaults);
	bool immediateWidgets = false;
	for (size_t i = 0; i + 1 < desc.Args.size(); i++)
		if (desc.Args[i] == "--widgets")
			immediateWidgets = desc.Args[i + 1] == "immediate";
	hearth::App app(std::move(desc));
	wisp::Applet ui(app);
	ui.AddHouseFaces();
	gallery::Gallery gallery(ui, immediateWidgets);
	return app.Run();
}
