#pragma once

#include "Wisp/Docking/DockState.h"
#include "Wisp/Docking/DockSpace.h"
#include "Wisp/WispLayer.h"
#include "Platform/WindowCaps.h"
#include "TinyEngine/Core/Application.h"
#include "TinyEngine/Core/Window.h"
#include "TinyEngine/Events/ApplicationEvent.h"
#include <GLFW/glfw3.h>
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
//   - secondary windows poll + present their own frame
// The main window stays owned by the engine loop; its DockSpace shares
// the same DockState and rebuilds off the shared model version.
class DockWindowManager
{
public:
	DockWindowManager() = default;

	// Bind the shared state + the main window's projection. Secondary
	// windows are spawned on the next Update for every non-main node.
	void Attach(std::shared_ptr<DockState> state, DockSpace* mainProjection,
		FontCache* fontCache, FontHandle font, float fontSize)
	{
		m_State = std::move(state);
		m_MainProjection = mainProjection;
		m_FontCache = fontCache;
		m_Font = font;
		m_FontSize = fontSize;
	}

	void SetTraceEnabled(bool on) { m_Trace = on; }

	std::shared_ptr<DockState> GetState() const { return m_State; }
	size_t GetSecondaryWindowCount() const { return m_Windows.size(); }

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
		for (const dock::WindowNode& w : model.Windows)
		{
			if (w.bMain || m_Windows.count(w.Id)) continue;
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
				glfwSetWindowTitle((GLFWwindow*)e->OSWindow->GetNativeWindow(), title.c_str());
			}
		}

		// 5. Poll + present each secondary window (its own frame).
		for (auto& [id, e] : m_Windows)
			e->OSWindow->OnUpdate();
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
		TinyEngine::Scope<TinyEngine::Window> OSWindow;
		WindowContext* Context = nullptr; // owned by UIApplication
		std::string Title;
		bool CloseRequested = false;
	};

	void Trace(const std::string& msg)
	{
		if (m_Trace)
			TE_CORE_INFO(TinyEngine::LogCore, "DOCK: {0}", msg);
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
		entry->OSWindow = TinyEngine::Window::Create(
			TinyEngine::WindowProps(entry->Title, (uint32_t)std::max(1.0f, w.Size.x), (uint32_t)std::max(1.0f, w.Size.y)));
		if (w.Pos.has_value())
			TinyEngine::TrySetWindowPos((GLFWwindow*)entry->OSWindow->GetNativeWindow(), (int)w.Pos->x, (int)w.Pos->y);

		// Per-window event routing: WM close is DEFERRED (this callback runs
		// inside the window's own glfwPollEvents - destroying it here would
		// return into SwapBuffers on freed state). Everything else routes
		// through the shared Wisp event router, which resolves this window's
		// context via the handle.
		WindowEntry* raw = entry.get();
		entry->OSWindow->SetEventCallback([raw](TinyEngine::Event& e) {
			if (e.GetEventType() == TinyEngine::EventType::WindowClose)
			{
				raw->CloseRequested = true;
				return;
			}
			WispLayer::RouteEvent(e);
		});

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

		void* handle = entry->OSWindow->GetNativeWindow();
		entry->Context = UIApplication::Get().CreateWindowContext(handle, std::move(root));

		Trace("spawn OS window id=" + std::to_string(w.Id) + " title=\"" + entry->Title + "\"");
		m_Windows[w.Id] = std::move(entry);
	}

	void DestroyEntry(WindowEntry& e)
	{
		if (e.OSWindow)
			UIApplication::Get().DestroyWindowContext(e.OSWindow->GetNativeWindow());
		e.OSWindow.reset();
		e.Context = nullptr;
	}

	std::shared_ptr<DockState> m_State;
	DockSpace* m_MainProjection = nullptr;
	FontCache* m_FontCache = nullptr;
	FontHandle m_Font = 0;
	float m_FontSize = 13.0f;
	bool m_Trace = false;

	std::unordered_map<dock::NodeId, std::unique_ptr<WindowEntry>> m_Windows;
};

} // namespace wisp
