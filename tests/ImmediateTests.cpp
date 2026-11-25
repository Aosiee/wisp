// The immediate-mode layer without a window: the host's frame run by hand,
// the widgets' input fired at them directly.

#include "Wisp/Im/Immediate.h"

#include "Wisp/Widgets.h"
#include "Wisp/Kit/PanelKit.h"

#include "hamr/test.h"

#include <chrono>
#include <cstdio>
#include <string>
#include <vector>

namespace wisp::immediate_tests {

	using Frame = std::function<void(im::Context&)>;

	struct Harness
	{
		Frame Body;
		ImmediateHost Host;

		Harness()
			: Host(nullptr, InvalidFont, [this](im::Context& ui) {
				if (Body)
					Body(ui);
			})
		{
		}

		void Run(Frame body)
		{
			Body = std::move(body);
			Host.RunFrame();
		}

		BoxPanel& Content() { return Host.GetContent(); }
		Widget* Child(size_t i) { return Content().GetChild(i); }
	};

	void Click(Widget* widget)
	{
		widget->OnMouseEnter();
		widget->OnMouseButtonDown({ 0.0f, 0.0f }, 0);
		widget->OnMouseButtonUp({ 0.0f, 0.0f }, 0);
	}

	TEST_CASE("wisp im: the first frame creates a widget per call, in call order")
	{
		Harness h;
		float value = 0.5f;
		h.Run([&](im::Context& ui) {
			ui.Text("Hello");
			ui.Button("Go");
			ui.Slider("Amount", &value, 0.0f, 1.0f);
		});
		REQUIRE(h.Content().GetChildCount() == 3u);
		CHECK(std::string(h.Child(0)->GetTypeName()) == "TextBlock");
		CHECK(std::string(h.Child(1)->GetTypeName()) == "Button");
		CHECK(std::string(h.Child(2)->GetTypeName()) == "Slider");
		CHECK(static_cast<Slider*>(h.Child(2))->GetValue() == hamr::test::Approx(0.5f));
		CHECK(h.Host.GetContext().GetNodeCount() == 3u);
		CHECK(h.Child(0)->GetParent() == &h.Content());
	}

	TEST_CASE("wisp im: later frames update the same widgets")
	{
		Harness h;
		int frame = 0;
		const auto body = [&](im::Context& ui) { ui.Text("Frame {}", frame); };
		h.Run(body);
		Widget* first = h.Child(0);
		frame = 7;
		h.Run(body);
		REQUIRE(h.Content().GetChildCount() == 1u);
		CHECK(h.Child(0) == first);
		CHECK(static_cast<TextBlock*>(first)->GetText() == "Frame 7");
	}

	TEST_CASE("wisp im: a widget no call reached is destroyed, with its subtree's nodes")
	{
		Harness h;
		bool more = true;
		const auto body = [&](im::Context& ui) {
			ui.Text("Always");
			if (more)
			{
				ui.BeginRow("row");
				ui.Button("A");
				ui.Button("B");
				ui.EndRow();
			}
		};
		h.Run(body);
		CHECK(h.Content().GetChildCount() == 2u);
		CHECK(h.Host.GetContext().GetNodeCount() == 4u);
		more = false;
		h.Run(body);
		CHECK(h.Content().GetChildCount() == 1u);
		CHECK(h.Host.GetContext().GetNodeCount() == 1u);
		more = true;
		h.Run(body);
		CHECK(h.Content().GetChildCount() == 2u);
		CHECK(h.Host.GetContext().GetNodeCount() == 4u);
	}

	TEST_CASE("wisp im: a click is reported once, by the next call")
	{
		Harness h;
		std::vector<bool> results;
		const auto body = [&](im::Context& ui) { results.push_back(ui.Button("Go")); };
		h.Run(body);
		Click(h.Child(0));
		h.Run(body);
		h.Run(body);
		CHECK(results == std::vector<bool>{ false, true, false });
	}

	TEST_CASE("wisp im: an edit reaches the bound value; a program change reaches the widget")
	{
		Harness h;
		bool snap = false;
		std::vector<bool> changed;
		const auto body = [&](im::Context& ui) { changed.push_back(ui.Checkbox("Snap", &snap)); };
		h.Run(body);
		auto* box = static_cast<CheckBox*>(h.Child(0));
		CHECK_FALSE(box->IsChecked());
		Click(box);
		h.Run(body);
		CHECK(snap);
		CHECK(changed == std::vector<bool>{ false, true });
		snap = false;
		h.Run(body);
		CHECK_FALSE(box->IsChecked());
		CHECK(changed.back() == false);
	}

	TEST_CASE("wisp im: a slider maps its range both ways")
	{
		Harness h;
		float exposure = 1.0f;
		int steps = 3;
		const auto body = [&](im::Context& ui) {
			ui.Slider("Exposure", &exposure, -4.0f, 4.0f);
			ui.Slider("Steps", &steps, 0, 10);
		};
		h.Run(body);
		CHECK(static_cast<Slider*>(h.Child(0))->GetValue() == hamr::test::Approx(0.625f));
		CHECK(static_cast<Slider*>(h.Child(1))->GetValue() == hamr::test::Approx(0.3f));
		exposure = -4.0f;
		steps = 10;
		h.Run(body);
		CHECK(static_cast<Slider*>(h.Child(0))->GetValue() == hamr::test::Approx(0.0f));
		CHECK(static_cast<Slider*>(h.Child(1))->GetValue() == hamr::test::Approx(1.0f));
	}

	TEST_CASE("wisp im: reordered calls move their widgets, not rebuild them")
	{
		Harness h;
		bool swapped = false;
		const auto body = [&](im::Context& ui) {
			ui.Button(swapped ? "Y" : "X");
			ui.Button(swapped ? "X" : "Y");
		};
		h.Run(body);
		Widget* x = h.Child(0);
		Widget* y = h.Child(1);
		swapped = true;
		h.Run(body);
		CHECK(h.Child(0) == y);
		CHECK(h.Child(1) == x);
		CHECK(h.Host.GetContext().GetNodeCount() == 2u);
	}

	TEST_CASE("wisp im: PushId keeps a loop's items apart, and an item keeps its widget when another goes")
	{
		Harness h;
		std::vector<int> items = { 10, 20, 30 };
		int deleted = -1;
		const auto body = [&](im::Context& ui) {
			for (int item : items)
			{
				ui.PushId(item);
				if (ui.Button("Delete"))
					deleted = item;
				ui.PopId();
			}
		};
		h.Run(body);
		REQUIRE(h.Content().GetChildCount() == 3u);
		Widget* third = h.Child(2);
		Click(h.Child(1));
		h.Run(body);
		CHECK(deleted == 20);
		items.erase(items.begin() + 1);
		h.Run(body);
		REQUIRE(h.Content().GetChildCount() == 2u);
		CHECK(h.Child(1) == third);
	}

	TEST_CASE("wisp im: a repeated label gets its own widget")
	{
		Harness h;
		h.Run([](im::Context& ui) {
			ui.Button("OK");
			ui.Button("OK");
		});
		CHECK(h.Content().GetChildCount() == 2u);
		CHECK(h.Child(0) != h.Child(1));
	}

	TEST_CASE("wisp im: a label's ## part is its key, not its text")
	{
		Harness h;
		h.Run([](im::Context& ui) {
			ui.Button("Save##one");
			ui.Button("Save##two");
		});
		REQUIRE(h.Content().GetChildCount() == 2u);
		auto text = [&](size_t i) {
			std::string out;
			h.Child(i)->ForEachChild([&](Widget* w) { out = static_cast<TextBlock*>(w)->GetText(); });
			return out;
		};
		CHECK(text(0) == "Save");
		CHECK(text(1) == "Save");
	}

	TEST_CASE("wisp im: a closed tree node destroys its body; opening it builds it again")
	{
		Harness h;
		bool open = false;
		const auto body = [&](im::Context& ui) {
			open = ui.TreeNode("Details", true);
			if (open)
			{
				ui.Text("a");
				ui.Text("b");
				ui.TreePop();
			}
		};
		h.Run(body);
		CHECK(open);
		CHECK(h.Host.GetContext().GetNodeCount() == 3u);
		auto* disclosure = static_cast<kit::Disclosure*>(h.Child(0));
		disclosure->SetOpen(false);
		h.Run(body);
		CHECK_FALSE(open);
		CHECK(h.Host.GetContext().GetNodeCount() == 1u);
		disclosure->SetOpen(true);
		h.Run(body);
		CHECK(h.Host.GetContext().GetNodeCount() == 3u);
	}

	TEST_CASE("wisp im: Widget<T> makes its widget once and hands it back")
	{
		Harness h;
		int made = 0;
		Throbber* seen = nullptr;
		const auto body = [&](im::Context& ui) {
			seen = ui.Widget<Throbber>("spinner", [&] {
				made++;
				return std::make_unique<Throbber>();
			});
		};
		h.Run(body);
		Throbber* first = seen;
		h.Run(body);
		CHECK(made == 1);
		CHECK(seen == first);
		CHECK(h.Child(0) == first);
	}

	TEST_CASE("wisp im: fields, tables and slots lay out like their retained forms")
	{
		Harness h;
		h.Run([](im::Context& ui) {
			ui.BeginField("Name");
			ui.Text("value");
			ui.EndField();
			ui.BeginTable("grid", 3);
			ui.TableNextRow();
			ui.Text("a");
			ui.TableNextRow();
			ui.Text("b");
			ui.Text("c");
			ui.EndTable();
			ui.Next().Fill(2.0f).Padding(3.0f);
			ui.Separator();
		});
		REQUIRE(h.Content().GetChildCount() == 3u);
		auto* field = static_cast<BoxPanel*>(h.Child(0));
		REQUIRE(field->GetChildCount() == 2u);
		CHECK(std::string(field->GetChild(0)->GetTypeName()) == "SizeBox");
		CHECK(field->GetSlot(1).FillWeight == 1.0f);
		CHECK(field->GetSlot(1).Align.V == VAlign::Center);
		CHECK(h.Content().GetSlot(0).PaddingLeft == 10.0f);
		CHECK(h.Content().GetSlot(0).PaddingTop == 4.0f);
		auto* table = static_cast<BoxPanel*>(h.Child(1));
		REQUIRE(table->GetChildCount() == 2u);
		CHECK(static_cast<BoxPanel*>(table->GetChild(0))->GetChildCount() == 3u);
		CHECK(static_cast<BoxPanel*>(table->GetChild(1))->GetChildCount() == 3u);
		CHECK(h.Content().GetSlot(2).FillWeight == 2.0f);
		CHECK(h.Content().GetSlot(2).PaddingTop == 3.0f);
	}

	TEST_CASE("wisp im: a scope the frame left open is closed for it")
	{
		Harness h;
		h.Run([](im::Context& ui) {
			ui.BeginRow();
			ui.BeginColumn();
			ui.Text("deep");
		});
		CHECK(h.Host.GetContext().GetUnbalancedScopes() == 2u);
		CHECK(h.Host.GetContext().GetNodeCount() == 3u);
		h.Run([](im::Context& ui) { ui.Text("flat"); });
		CHECK(h.Host.GetContext().GetUnbalancedScopes() == 0u);
		CHECK(h.Host.GetContext().GetNodeCount() == 1u);
	}

	TEST_CASE("wisp im: a 500-widget scope's frame is cheap")
	{
		Harness h;
		std::vector<float> values(100, 0.25f);
		std::vector<bool> flags(100, false);
		int counter = 0;
		const auto body = [&](im::Context& ui) {
			for (int i = 0; i < 100; i++)
			{
				ui.PushId(i);
				ui.BeginRow();
				ui.Text("Row {} at {}", i, counter);
				bool flag = flags[(size_t)i];
				ui.Checkbox("Enabled", &flag);
				ui.Slider("Amount", &values[(size_t)i], 0.0f, 1.0f);
				ui.Button("Reset");
				ui.EndRow();
				ui.PopId();
			}
		};
		h.Run(body);
		CHECK(h.Host.GetContext().GetNodeCount() == 500u);
		for (int i = 0; i < 20; i++)
			h.Run(body);
		constexpr int kFrames = 200;
		const auto start = std::chrono::steady_clock::now();
		for (int i = 0; i < kFrames; i++)
		{
			counter = i;
			h.Run(body);
		}
		const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count() / kFrames;
		std::printf("wisp im: 500 widgets, %.4f ms a frame\n", ms);
		CHECK(h.Host.GetContext().GetNodeCount() == 500u);
#if defined(NDEBUG) && !defined(WISP_SANITIZED)
		CHECK(ms < 0.5);
#endif
	}

} // namespace wisp::immediate_tests
