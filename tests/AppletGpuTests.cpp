// wisp::Applet on a real device (Mesa's lavapipe in CI), offscreen: a root
// drawn through the renderer's pipelines into hearth::App's offscreen image
// and read back, and the input script driving a widget.

#include "Wisp/Applet/Applet.h"

#include "Wisp/Components/Border.h"
#include "Wisp/Components/Button.h"
#include "Wisp/Components/Spacer.h"
#include "Wisp/Im/Immediate.h"

#include "hearth/App.h"
#include "hearth/rhi/vulkan/WindowPresenter.h"

#include "hamr/test.h"

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <vector>

namespace wisp::applet_gpu_tests {

	hearth::AppDesc Offscreen(uint32_t width, uint32_t height)
	{
		hearth::AppDesc desc;
		desc.Title = "wisp applet test";
		desc.Display = hearth::DisplayMode::Offscreen;
		desc.RenderTargetWidth = width;
		desc.RenderTargetHeight = height;
		desc.Workers = 2;
		return desc;
	}

	glm::u8vec4 Pixel(const std::vector<uint8_t>& rgba, uint32_t width, uint32_t x, uint32_t y)
	{
		const size_t i = ((size_t)y * width + x) * 4;
		return { rgba[i], rgba[i + 1], rgba[i + 2], rgba[i + 3] };
	}

	bool Near(glm::u8vec4 a, glm::u8vec4 b)
	{
		for (int c = 0; c < 4; c++)
			if (std::abs((int)a[c] - (int)b[c]) > 1)
				return false;
		return true;
	}

	TEST_CASE("wisp applet gpu: a root's brush is what the offscreen image holds")
	{
		hearth::AppDesc desc = Offscreen(64, 32);
		desc.FrameLimit = 3;
		hearth::App app(desc);
		std::vector<uint8_t> rgba;
		{
			Applet ui(app);
			ui.AddHouseFaces();
			auto root = std::make_unique<Border>();
			root->SetBrush({ glm::vec4(1.0f, 0.0f, 0.0f, 1.0f) });
			ui.SetRoot(std::move(root));
			CHECK(app.Run() == 0);
			REQUIRE(app.GetPresenter().ReadOffscreenImage(rgba));
		}
		REQUIRE(rgba.size() == 64u * 32u * 4u);
		CHECK(Near(Pixel(rgba, 64, 0, 0), { 255, 0, 0, 255 }));
		CHECK(Near(Pixel(rgba, 64, 32, 16), { 255, 0, 0, 255 }));
		CHECK(Near(Pixel(rgba, 64, 63, 31), { 255, 0, 0, 255 }));
	}

	TEST_CASE("wisp applet gpu: the input script clicks a button, and the capture waits for it")
	{
		const std::filesystem::path dir = std::filesystem::temp_directory_path() / "wisp_applet_gpu";
		std::filesystem::create_directories(dir);
		const std::filesystem::path script = dir / "script.json";
		// A press and a release in separate frames, as a hand makes them,
		// then a click (both in one frame).
		std::ofstream(script) << R"({ "events": [ { "frame": 6, "type": "down", "x": 20, "y": 20, "button": 0 },
			{ "frame": 7, "type": "up", "x": 20, "y": 20, "button": 0 },
			{ "frame": 10, "type": "click", "x": 30, "y": 30, "button": 0 } ] })";

		hearth::AppDesc desc = Offscreen(64, 64);
		desc.CaptureUiPath = (dir / "capture.png").string();
		desc.FrameLimit = 100;
		desc.Args = { "--input-script", script.string() };
		hearth::App app(desc);
		std::vector<uint64_t> clickFrames;
		{
			Applet ui(app);
			ui.AddHouseFaces();
			CHECK(ui.ScriptEnd() == 10u);
			auto button = std::make_unique<Button>();
			button->SetOnClicked([&] { clickFrames.push_back(app.GetLoop().FrameIndex()); });
			button->SetChild(std::make_unique<Spacer>(glm::vec2(40.0f, 40.0f)));
			auto root = std::make_unique<Border>();
			root->SetPadding(0.0f).SetChild(std::move(button));
			ui.SetRoot(std::move(root));
			CHECK(app.Run() == hearth::kExitCaptured);
		}
		CHECK(clickFrames == std::vector<uint64_t>{ 7u, 10u });
		const std::string sidecar = [&] {
			std::ifstream in(desc.CaptureUiPath + ".json");
			return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
		}();
		// The script's last frame, then two frames to show it.
		CHECK(sidecar.find("\"frames\": 12") != std::string::npos);
		std::filesystem::remove_all(dir);
	}

	TEST_CASE("wisp applet gpu: an immediate host runs every frame, and its button reports the script's click once")
	{
		const std::filesystem::path dir = std::filesystem::temp_directory_path() / "wisp_applet_gpu_im";
		std::filesystem::create_directories(dir);
		const std::filesystem::path script = dir / "script.json";
		std::ofstream(script) << R"({ "events": [ { "frame": 10, "type": "click", "x": 8, "y": 8, "button": 0 } ] })";

		hearth::AppDesc desc = Offscreen(96, 64);
		desc.CaptureUiPath = (dir / "capture.png").string();
		desc.FrameLimit = 100;
		desc.Args = { "--input-script", script.string() };
		hearth::App app(desc);
		std::vector<uint64_t> clickFrames;
		uint64_t frames = 0;
		{
			Applet ui(app);
			ui.AddHouseFaces();
			ui.SetRoot(std::make_unique<ImmediateHost>(&ui.Fonts(), ui.GetHouseFaces().Regular, [&](im::Context& im) {
				frames++;
				im.Next().HAlign(HAlign::Left);
				if (im.Button("Go"))
					clickFrames.push_back(app.GetLoop().FrameIndex());
			}));
			CHECK(app.Run() == hearth::kExitCaptured);
		}
		// The click's events dispatch before the frame's UI update, so the
		// call that frame sees it.
		CHECK(clickFrames == std::vector<uint64_t>{ 10u });
		CHECK(frames >= 12u);
		std::filesystem::remove_all(dir);
	}

} // namespace wisp::applet_gpu_tests
