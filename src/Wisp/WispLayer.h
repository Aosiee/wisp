#pragma once

#include "TinyEngine.h"
#include "TinyEngine/Core/Layer.h"
#include "TinyEngine/Events/MouseEvent.h"
#include "TinyEngine/Events/KeyEvent.h"
#include "TinyEngine/Events/ApplicationEvent.h"
#include "Wisp/Application.h"

namespace wisp {

// WispLayer — bridges TinyEngine's event system into Wisp's input API.
// Pushed as a regular layer (not overlay); event routing is per-window.
// Wisp is the sole UI layer now.
//
// Multi-window: every event carries the GLFWwindow* it originated in
// (stamped by the windowing layer), so each event routes to that window's
// WindowContext - hover/capture/popups/cursor are per window, focus stays
// globally single. MouseMove → SetMousePosition(window, pos); buttons/keys/
// text/wheel route per window; window focus in/out drives the focused
// context and its panel-layer recall.
//
// Wisp does NOT use a block-events flag — instead, it marks events
// as handled when a Wisp widget consumed them (e.g., button click, scrollbar drag).

class WispLayer : public TinyEngine::Layer
{
public:
	WispLayer();
	virtual ~WispLayer() = default;

	virtual void OnAttach() override;
	virtual void OnDetach() override;
	virtual void OnUpdate(TinyEngine::Timestep ts) override;
	virtual void OnEvent(TinyEngine::Event& event) override;

	// Shared engine-event → Wisp routing: the layer uses it for the
	// main window's events; DockWindowManager uses it for each secondary
	// window's own callback. Resolves the target context from the event's
	// window handle, so both call sites are identical.
	static void RouteEvent(TinyEngine::Event& event);
};

} // namespace wisp
