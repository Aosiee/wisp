#pragma once

// Wisp as a program of its own, on hearth::App: the application with Hearth's
// GPU renderer, the main window's UI as one of its presenter's frame
// clients, the platform's events routed to Wisp by their window, and the
// dock's window host (a torn-out panel gets its own window and presenter).
//
//     int main(int argc, char** argv)
//     {
//         hearth::App app(hearth::App::ParseArgs(argc, argv, { .Title = "Hello", .Width = 640, .Height = 400 }));
//         wisp::Applet ui(app);
//         ui.AddHouseFaces();
//         ui.SetRoot(BuildMyUi());
//         return app.Run();
//     }
//
// `--input-script <file.json>` plays recorded input into the main window:
// { "events": [ { "frame": N, "type": "move" | "click" | "down" | "up" |
// "wheel" | "key" | "text", "x", "y", "button", "delta", "key", "mods",
// "text" } ] } ("down" and "up" move the pointer and press or release, for
// a drag over several frames). Each event goes in as the platform's would,
// at the start of its frame (the pointer reaches a click's, a press's or a
// release's position a frame ahead, so the widget under it is hovered), and
// a capture waits for the script's last frame.
//
// The Applet must be destroyed before its App (declared after it), and
// whatever holds dock windows (a DockWindowManager) before the Applet.

#include "Wisp/Docking/WindowHost.h"
#include "Wisp/FontCache.h"
#include "Wisp/Fonts/HouseFaces.h"
#include "Wisp/Rhi/RhiFrameClient.h"

#include "hearth/App.h"

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace hearth::platform {
	class Window;
	struct Event;
}

namespace wisp {

	class Widget;

	class Applet final : public WindowHost
	{
	public:
		explicit Applet(hearth::App& app);
		~Applet() override;
		Applet(const Applet&) = delete;
		Applet& operator=(const Applet&) = delete;

		hearth::App& GetApp() { return m_App; }
		FontCache& Fonts() { return m_Fonts; }
		// Adds the house faces (wisp::fonts) and makes them the font roles.
		const fonts::HouseFaces& AddHouseFaces();
		const fonts::HouseFaces& GetHouseFaces() const { return m_Faces; }

		// The main window's root, replacing the previous one.
		void SetRoot(std::unique_ptr<Widget> root);
		// The main window's key in Wisp (its platform window; null offscreen).
		void* MainWindow() const;

		// The last frame the input script plays (0 = no script).
		uint64_t ScriptEnd() const { return m_ScriptEnd; }

		// WindowHost
		bool CanOpenWindows() const override;
		std::unique_ptr<HostWindow> OpenWindow(const std::string& title, uint32_t width, uint32_t height,
			std::function<void()> onCloseRequested) override;

	private:
		class Window;
		struct ScriptEvent
		{
			uint64_t Frame = 0;
			std::string Type;
			float X = 0.0f, Y = 0.0f, Delta = 0.0f;
			int Button = 0, Key = 0, Mods = 0;
			std::string Text;
		};

		bool Route(const hearth::platform::Event& event);
		bool LoadScript(const std::string& path);
		void PostScript(uint64_t frame);

		hearth::App& m_App;
		FontCache m_Fonts;
		fonts::HouseFaces m_Faces;
		RhiFrameClient m_Client;
		hearth::App::HandlerId m_Handler = 0;
		std::vector<hearth::ClientId> m_LoopClients;
		// The open dock windows' close requests, by their platform window.
		std::unordered_map<const hearth::platform::Window*, std::function<void()>> m_CloseRequests;
		std::vector<ScriptEvent> m_Script;
		uint64_t m_ScriptEnd = 0;
	};

} // namespace wisp
