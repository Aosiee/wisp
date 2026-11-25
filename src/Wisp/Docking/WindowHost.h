#pragma once

// What Wisp needs from the program to open OS windows of its own (a torn-out
// dock window): the program makes the window and whatever renders into it,
// routes the window's input to Wisp, and tells Wisp when the user asks to
// close it.

#include <cstdint>
#include <functional>
#include <memory>
#include <string>

namespace hearth::platform { class Window; }

namespace wisp {

	// One window the program opened for Wisp. Destroying it closes it.
	class HostWindow
	{
	public:
		virtual ~HostWindow() = default;
		// The platform window: the window's identity in Wisp (its context
		// key) and where its size, position and title live.
		virtual hearth::platform::Window* PlatformWindow() const = 0;
		virtual void SetTitle(const std::string& title) = 0;
		// Renders and presents the window's frame (the program pumps every
		// window's events once a frame; this only presents).
		virtual void Present() = 0;
	};

	class WindowHost
	{
	public:
		virtual ~WindowHost() = default;
		// False where the program opens no windows (an offscreen run): the
		// dock keeps such windows in its layout, unprojected.
		virtual bool CanOpenWindows() const = 0;
		// A new window, `onCloseRequested` called (from the program's event
		// drain, never inside its pump) when the user asks to close it.
		virtual std::unique_ptr<HostWindow> OpenWindow(const std::string& title, uint32_t width, uint32_t height,
			std::function<void()> onCloseRequested) = 0;
	};

} // namespace wisp
