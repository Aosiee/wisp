#pragma once

#include "Wisp/Docking/DockState.h"
#include "Wisp/Log.h"
#include "Wisp/Docking/DockSpace.h"
#include "Wisp/Docking/WindowHost.h"
#include "hearth/platform/Window.h"
#include <memory>
#include <string>
#include <unordered_map>

namespace wisp {

// Window manager: the OS windows are a PROJECTION of the DockLayout's
// WindowNodes (the data-first rule extended to windows themselves). Each
// Update() reconciles model <-> windows:
//   - deferred WM-close requests route through CloseWindow(PerPanel)
//   - nodes without an OS window get one spawned (WindowNode.Size, no
//     position request - tiling WMs tile it)
//   - OS windows whose node is gone are destroyed (context + window)
//   - each window's title tracks its active tab's display name
//   - secondary windows present their own frame (the program pumps every
//     window's events once a frame)
// The OS windows come from the program's WindowHost (Attach).
// The main window stays owned by the engine loop; its DockSpace shares
// the same DockState and rebuilds off the shared model version.
class DockWindowManager
{
public:
	DockWindowManager() = default;

	// Bind the shared state + the main window's projection. Secondary
	// windows are spawned on the next Update for every non-main node.
	void Attach(std::shared_ptr<DockState> state, DockSpace* mainProjection,
		FontCache* fontCache, FontHandle font, float fontSize, WindowHost* host)
	{
		m_Host = host;
		m_State = std::move(state);
		m_MainProjection = mainProjection;
		m_FontCache = fontCache;
		m_Font = font;
		m_FontSize = fontSize;
	}

	void SetTraceEnabled(bool on) { m_Trace = on; }

	std::shared_ptr<DockState> GetState() const { return m_State; }
	size_t GetSecondaryWindowCount() const { return m_Windows.size(); }

	// The native window hosting `panel` (a PIE client's
	// input window): `mainNative` when the panel is in the main window, the
	// projected OS window's handle otherwise, null when the panel is closed
	// or its window node is not projected (yet, or ever - offscreen).
	void* FindNativeWindowOfPanel(const dock::PanelId& panel, void* mainNative) const
	{
		if (!m_State) return nullptr;
		const dock::NodeId well = m_State->FindPanelWell(panel);
		if (well == 0) return nullptr;
		const dock::WindowNode* window = dock::FindWindowContainingNode(m_State->GetModel(), well);
		if (!window) return nullptr;
		if (window->bMain) return mainNative;
		auto it = m_Windows.find(window->Id);
		return it != m_Windows.end() && it->second->OSWindow ? it->second->OSWindow->PlatformWindow() : nullptr;
	}

	// Call once per application frame (from the app layer's OnUpdate).
	void Update()
	{
		if (!m_State) return;
		const dock::DockLayout& model = m_State->GetModel();

		// 1. Deferred WM-close requests → model (PerPanel).
		for (auto& [id, e] : m_Windows)
		{
			if (!e->CloseRequested) continue;
			e->CloseRequested = false;
			m_State->CloseWindow(id, dock::ClosePolicy::PerPanel);
			Trace("WM close window=" + std::to_string(id));
		}

		// 2. Spawn OS windows for nodes that lack one.
		// A program that opens no windows (an offscreen run, or no host:
		// the dock tests) has nothing to project secondary nodes into - so
		// the nodes stay in the model, unprojected.
		const bool offscreen = !m_Host || !m_Host->CanOpenWindows();
		for (const dock::WindowNode& w : model.Windows)
		{
			if (w.bMain || m_Windows.count(w.Id)) continue;
			if (offscreen)
			{
				if (m_Host && !m_LoggedOffscreenSkip)
				{
					m_LoggedOffscreenSkip = true;
					HEARTH_LOG_INFO(::wisp::LogWisp, "DOCK: offscreen display mode - secondary dock windows are not opened (the layout keeps them).");
				}
				continue;
			}
			SpawnWindow(w);
		}

		// 3. Destroy OS windows whose node is gone (closed, merged, or
		//    emptied - the projection follows the data).
		for (auto it = m_Windows.begin(); it != m_Windows.end();)
		{
			bool gone = !dock::FindWindowContainingNode(model, it->first);
			if (gone)
			{
				DestroyEntry(*it->second);
				Trace("destroy OS window id=" + std::to_string(it->first));
				it = m_Windows.erase(it);
			}
			else
			{
				++it;
			}
		}

		// 4. Live titles: active tab's display name (cheap, high value on
		//    tiling WMs - what bars/switchers show).
		for (auto& [id, e] : m_Windows)
		{
			std::string title = m_State->WindowTitleOf(id);
			if (title != e->Title)
			{
				e->Title = title;
				if (e->OSWindow)
					e->OSWindow->SetTitle(title);
			}
		}

		// 5. Present each secondary window (its own frame). The program pumps
		// every window's events once a frame, so this only presents.
		for (auto& [id, e] : m_Windows)
			if (e->OSWindow)
				e->OSWindow->Present();
	}

	// Tear every secondary window down (app shutdown, before the Wisp
	// renderer/device go away).
	void Shutdown()
	{
		for (auto& [id, e] : m_Windows)
			DestroyEntry(*e);
		m_Windows.clear();
		m_State.reset();
		m_MainProjection = nullptr;
	}

private:
	struct WindowEntry
	{
		dock::NodeId NodeId = 0;
		std::unique_ptr<HostWindow> OSWindow;
		WindowContext* Context = nullptr; // owned by UIApplication
		std::string Title;
		bool CloseRequested = false;
	};


	void Trace(const std::string& msg)
	{
		if (m_Trace)
			HEARTH_LOG_INFO(::wisp::LogWisp, "DOCK: {0}", msg);
	}

	void SpawnWindow(const dock::WindowNode& w)
	{
		auto entry = std::make_unique<WindowEntry>();
		entry->NodeId = w.Id;
		entry->Title = m_State->WindowTitleOf(w.Id);

		// No position request: tiling WMs tile the new window next to the
		// editor; stacking WMs use WM defaults. Where the platform CAN
		// place windows (X11/Windows) and the node carries a
		// persisted/tear-out position, use it.
		// The host routes the window's input to Wisp; a WM close is
		// DEFERRED to the next Update (destroying the window from the
		// program's event drain would pull it out from under its frame).
		WindowEntry* raw = entry.get();
		entry->OSWindow = m_Host->OpenWindow(entry->Title, (uint32_t)std::max(1.0f, w.Size.x), (uint32_t)std::max(1.0f, w.Size.y),
			[raw]() { raw->CloseRequested = true; });
		if (!entry->OSWindow)
			return;
		if (w.Pos.has_value())
			if (hearth::platform::Window* platformWindow = entry->OSWindow->PlatformWindow())
				platformWindow->SetPosition((int)w.Pos->x, (int)w.Pos->y);

		// The window's Wisp root: a DockSpace projecting this node.
		auto dock = std::make_unique<DockSpace>(m_FontCache, m_Font, m_FontSize);
		dock->SetState(m_State);
		dock->SetWindowId(w.Id);
		auto panel = std::make_unique<Border>();
		panel->SetBrush({ glm::vec4(0.08f, 0.08f, 0.12f, 0.92f) }).SetPadding(0).SetChild(std::move(dock));
		auto bg = std::make_unique<Border>();
		bg->SetBrush({ glm::vec4(0.06f, 0.06f, 0.10f, 0.90f) });
		auto root = std::make_unique<Overlay>();
		*root
			+ root->Slot()[ std::move(bg) ]
			+ root->Slot()[ std::move(panel) ];

		void* handle = entry->OSWindow->PlatformWindow();
		entry->Context = UIApplication::Get().CreateWindowContext(handle, std::move(root));

		Trace("spawn OS window id=" + std::to_string(w.Id) + " title=\"" + entry->Title + "\"");
		m_Windows[w.Id] = std::move(entry);
	}

	void DestroyEntry(WindowEntry& e)
	{
		if (e.OSWindow)
			UIApplication::Get().DestroyWindowContext(e.OSWindow->PlatformWindow());
		e.OSWindow.reset();
		e.Context = nullptr;
	}

	std::shared_ptr<DockState> m_State;
	DockSpace* m_MainProjection = nullptr;
	WindowHost* m_Host = nullptr;
	FontCache* m_FontCache = nullptr;
	FontHandle m_Font = 0;
	float m_FontSize = 13.0f;
	bool m_Trace = false;
	bool m_LoggedOffscreenSkip = false;

	std::unordered_map<dock::NodeId, std::unique_ptr<WindowEntry>> m_Windows;
};

} // namespace wisp
