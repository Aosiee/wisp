#include "Wisp/WispLayer.h"

#include <GLFW/glfw3.h>

namespace wisp {

WispLayer::WispLayer() : Layer("WispLayer") {}

void WispLayer::OnAttach() {}
void WispLayer::OnDetach() {}

void WispLayer::OnUpdate(TinyEngine::Timestep ts)
{
	(void)ts;
	// Poll each window's OS cursor position every frame (polled, not
	// event-driven, so a static mouse still tracks; per window since each
	// window owns its hover state). Guarded: apps that haven't
	// initialised Wisp yet (the editor before its dock is first enabled)
	// must not touch the singleton.
	if (UIApplication::IsValid())
		UIApplication::Get().SyncMousePositions();
}

void WispLayer::OnEvent(TinyEngine::Event& event)
{
	RouteEvent(event);
}

void WispLayer::RouteEvent(TinyEngine::Event& event)
{
	// Apps that haven't initialised Wisp yet (the editor before its dock
	// is first enabled) still receive window events - the very first
	// cursor-enter/move arrives before any dock exists. Nothing to route.
	if (!UIApplication::IsValid())
		return;

	UIApplication& app = UIApplication::Get();

	// The window this event came from (stamped by the windowing layer);
	// null for synthetic events, which route to the focused window.
	void* window = event.GetNativeWindow();

	TinyEngine::EventDispatcher dispatcher(event);

	dispatcher.Dispatch<TinyEngine::MouseButtonPressedEvent>([&app, window](TinyEngine::MouseButtonPressedEvent& e) -> bool {
		return app.OnMouseButtonDown(window, e.GetMouseButton());
	});

	dispatcher.Dispatch<TinyEngine::MouseButtonReleasedEvent>([&app, window](TinyEngine::MouseButtonReleasedEvent& e) -> bool {
		return app.OnMouseButtonUp(window, e.GetMouseButton());
	});

	dispatcher.Dispatch<TinyEngine::MouseMovedEvent>([&app, window](TinyEngine::MouseMovedEvent& e) -> bool {
		app.SetMousePosition(window, { e.GetX(), e.GetY() });
		return false;
	});

	dispatcher.Dispatch<TinyEngine::MouseScrolledEvent>([&app, window](TinyEngine::MouseScrolledEvent& e) -> bool {
		app.OnMouseWheel(window, e.GetYOffset());
		return false;
	});

	dispatcher.Dispatch<TinyEngine::KeyPressedEvent>([&app, window](TinyEngine::KeyPressedEvent& e) -> bool {
		// Modifier state comes from the event's own window (per-window input
		// state - the main-window-only TinyEngine::Input is not consulted).
		GLFWwindow* win = (GLFWwindow*)window;
		int mods = 0;
		if (win)
		{
			if (glfwGetKey(win, TE_KEY_LEFT_SHIFT) == GLFW_PRESS || glfwGetKey(win, TE_KEY_RIGHT_SHIFT) == GLFW_PRESS)
				mods |= 1;
			if (glfwGetKey(win, TE_KEY_LEFT_CONTROL) == GLFW_PRESS || glfwGetKey(win, TE_KEY_RIGHT_CONTROL) == GLFW_PRESS)
				mods |= 2;
		}
		return app.OnKeyDown(window, e.GetKeyCode(), mods);
	});

	dispatcher.Dispatch<TinyEngine::KeyTypedEvent>([&app, window](TinyEngine::KeyTypedEvent& e) -> bool {
		return app.OnTextInput(window, e.GetKeyCode());
	});

	dispatcher.Dispatch<TinyEngine::KeyReleasedEvent>([&app, window](TinyEngine::KeyReleasedEvent& e) -> bool {
		// Same per-window modifier translation as the press path (on
		// release the lifted key is already up in glfwGetKey, so the mods
		// bit clears correctly for shift/ctrl releases).
		GLFWwindow* win = (GLFWwindow*)window;
		int mods = 0;
		if (win)
		{
			if (glfwGetKey(win, TE_KEY_LEFT_SHIFT) == GLFW_PRESS || glfwGetKey(win, TE_KEY_RIGHT_SHIFT) == GLFW_PRESS)
				mods |= 1;
			if (glfwGetKey(win, TE_KEY_LEFT_CONTROL) == GLFW_PRESS || glfwGetKey(win, TE_KEY_RIGHT_CONTROL) == GLFW_PRESS)
				mods |= 2;
		}
		return app.OnKeyUp(window, e.GetKeyCode(), mods);
	});

	dispatcher.Dispatch<TinyEngine::WindowFocusEvent>([&app, window](TinyEngine::WindowFocusEvent&) -> bool {
		app.OnWindowFocusChanged(window, true);
		return false;
	});

	dispatcher.Dispatch<TinyEngine::WindowLostFocusEvent>([&app, window](TinyEngine::WindowLostFocusEvent&) -> bool {
		app.OnWindowFocusChanged(window, false);
		return false;
	});

	dispatcher.Dispatch<TinyEngine::WindowCursorEnterEvent>([&app, window](TinyEngine::WindowCursorEnterEvent&) -> bool {
		app.OnCursorEntered(window, true);
		return false;
	});

	dispatcher.Dispatch<TinyEngine::WindowCursorLeaveEvent>([&app, window](TinyEngine::WindowCursorLeaveEvent&) -> bool {
		app.OnCursorEntered(window, false);
		return false;
	});
}

} // namespace wisp
