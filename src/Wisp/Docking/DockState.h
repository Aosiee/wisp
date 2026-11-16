#pragma once

#include "Wisp/Core.h"
#include "Wisp/Application.h"
#include "Wisp/Docking/DockModel.h"
#include "Platform/WindowCaps.h"
#include "TinyEngine/Core/Application.h"
#include "TinyEngine/Core/Log.h"
#include <cstring>
#include <fstream>
#include <functional>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace wisp {

class DockSpace;

// The shared dock model for the whole app: panel registry, panel content
// cache, the multi-window DockLayout, and every pure model/panel
// transform. One DockState exists per application; each OS window hosts a
// DockSpace widget that PROJECTS the WindowNode matching its window id.
// This is what lets panels move between windows, lets a window close route
// through CloseWindow, and keeps the content cache single-owner (a panel's
// widget exists once, hosted by whichever window's well holds its tab).
class DockState : public std::enable_shared_from_this<DockState>
{
public:
	struct PanelDef
	{
		std::string DisplayName;
		std::function<std::shared_ptr<Widget>()> Factory;
		bool Singleton = false;
		bool KeepAlive = false;
		glm::vec2 MinSize{ 140.0f, 100.0f };
		bool TickWhenHidden = false;
	};

	// Trace sinks - DockSpace projections install their own (trace is a
	// widget concern; the state itself only owns data + transforms). Multiple
	// projections share one state, so the sink set is a registry keyed by
	// owner: a second window's trace-off projection must NOT silence the
	// main window's traces.
	void AddTraceSink(const void* owner, std::function<void(const std::string&)> sink)
	{
		m_TraceSinks[owner] = std::move(sink);
	}
	void RemoveTraceSink(const void* owner)
	{
		m_TraceSinks.erase(owner);
	}

	void Trace(const std::string& msg) const
	{
		for (const auto& [owner, sink] : m_TraceSinks)
			if (sink)
				sink(msg);
	}

	// ---- Panel registry ----
	void RegisterPanel(const dock::PanelId& id, PanelDef def)
	{
		m_Registry[id] = std::move(def);
		m_RegistrationOrder.push_back(id);
	}

	// ---- Layout / model ----
	void SetLayout(const dock::DockLayout& model)
	{
		m_Model = dock::CloneLayout(model);
		m_Model.Version++;
		Trace("SetLayout " + dock::SerializeLayout(m_Model, false));
	}

	const dock::DockLayout& GetModel() const { return m_Model; }
	dock::DockLayout& GetModelMutable() { return m_Model; }

	void RestoreSnapshot(const dock::DockLayout& snap)
	{
		m_Model = dock::CloneLayout(snap);
		m_Model.Version++;
		Trace("restore snapshot");
	}

	// ---- Panel open/close/activate ----
	// Default well rule: searches the MAIN window. The optional targetWindow
	// lets "open in window X" place it elsewhere.
	void OpenPanel(const dock::PanelId& id, dock::NodeId targetWindow = 0)
	{
		auto defIt = m_Registry.find(id);
		if (defIt == m_Registry.end()) return;
		const PanelDef& def = defIt->second;

		if (def.Singleton)
		{
			dock::NodeId existing = FindPanelWell(id);
			if (existing != 0)
			{
				const dock::DockNode* well = dock::FindNode(m_Model, existing);
				if (well)
				{
					int idx = dock::FindTabIndex(*well, id);
					if (idx >= 0)
					{
						dock::SetActiveTab(m_Model, existing, idx);
						ActivatePanel(id);
						return;
					}
				}
			}
		}

		dock::NodeId well = (targetWindow != 0) ? FirstWellOf(targetWindow) : DefaultWell();
		if (well == 0) well = DefaultWell();
		dock::InsertTab(m_Model, well, id, -1, def.Singleton);
		dock::SetActiveTab(m_Model, well, (int)dock::FindNode(m_Model, well)->Tabs.size() - 1);
		ActivatePanel(id);
		Trace("OpenPanel " + id);
	}

	void ClosePanel(const dock::PanelId& id)
	{
		auto defIt = m_Registry.find(id);
		if (defIt == m_Registry.end()) return;

		dock::NodeId well = FindPanelWell(id);
		if (well != 0)
		{
			const dock::DockNode* wn = dock::FindNode(m_Model, well);
			if (wn)
			{
				int idx = dock::FindTabIndex(*wn, id);
				if (idx >= 0)
					dock::RemoveTab(m_Model, well, idx);
			}
		}

		ApplyPanelClosePolicy(id);
		if (m_CurrentPanelId == id)
			m_CurrentPanelId.clear();
		Trace("ClosePanel " + id);
	}

	// Close semantics, shared by ClosePanel and by
	// DockSpace::CloseWindow(PerPanel): destroyed by default, cached when
	// KeepAlive (the panel's widget + focus layer survive to reopen).
	void ApplyPanelClosePolicy(const dock::PanelId& id)
	{
		auto defIt = m_Registry.find(id);
		if (defIt == m_Registry.end() || defIt->second.KeepAlive) return;

		auto cacheIt = m_Cache.find(id);
		if (cacheIt == m_Cache.end()) return;

		if (UIApplication::IsValid())
		{
			UIApplication& app = UIApplication::Get();
			if (Widget* fw = app.GetFocusWidget())
			{
				bool inside = false;
				for (Widget* p = fw; p; p = p->GetParent())
					if (p == cacheIt->second.get()) { inside = true; break; }
				if (inside)
					app.ClearFocus();
			}
			app.UnregisterFocusLayerRoot(cacheIt->second.get());
			app.RemoveFocusLayer("panel:" + id);
		}
		m_Cache.erase(cacheIt);
	}

	void ActivateTab(dock::NodeId wellId, int index)
	{
		const dock::DockNode* well = dock::FindNode(m_Model, wellId);
		if (!well || well->Kind != dock::DockNode::Kind::TabWell) return;
		if (index < 0 || index >= (int)well->Tabs.size()) return;
		dock::SetActiveTab(m_Model, wellId, index);
		ActivatePanel(well->Tabs[index].Id);
		Trace("SetActiveTab well=" + std::to_string(wellId) + " idx=" + std::to_string(index));
	}

	void ActivatePanel(const dock::PanelId& id)
	{
		m_CurrentPanelId = id;
		if (!UIApplication::IsValid()) return;
		UIApplication& app = UIApplication::Get();
		std::string name = "panel:" + id;
		if (app.GetFocusLayerId(name) < 0)
			app.PushFocusLayer(name, UIApplication::FocusEntryPolicy::RestorePrevious);
		else
			app.SwitchToLayer(name, UIApplication::FocusEntryPolicy::RestorePrevious);
	}

	// ---- Persistence ----
	bool SaveLayout(const std::string& path) const
	{
		std::ofstream f(path, std::ios::binary);
		if (!f) return false;
		// Invariant 13: positions only persist where the platform can place
		// windows. A Wayland save simply omits them.
		std::string json = dock::SerializeLayout(m_Model, true, TinyEngine::DetectWindowCaps().CanPlaceWindows);
		f.write(json.data(), (std::streamsize)json.size());
		return !!f;
	}

	bool LoadLayout(const std::string& path, const glm::vec2& clampSize = { 0, 0 })
	{
		std::ifstream f(path, std::ios::binary);
		if (!f) return false;
		std::string json((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
		dock::DockLayout loaded;
		bool ok = dock::ParseLayout(json, loaded,
			[this](const dock::PanelId& id) { return m_Registry.count(id) > 0; },
			[this](const std::string& w) { Trace(w); });
		if (!ok) { Trace("LoadLayout failed - keeping current"); return false; }
		// Clamp floating rects into view (they are absolute).
		if (clampSize.x > 0 || clampSize.y > 0)
		{
			for (auto& w : loaded.Windows)
			{
				for (auto& fl : w.Floating)
				{
					fl.FloatPos.x = std::max(0.0f, std::min(fl.FloatPos.x, std::max(0.0f, clampSize.x - fl.FloatSize.x)));
					fl.FloatPos.y = std::max(0.0f, std::min(fl.FloatPos.y, std::max(0.0f, clampSize.y - fl.FloatSize.y)));
				}
			}
		}
		m_Model = std::move(loaded);
		m_Model.Version++;
		m_CurrentPanelId.clear();
		Trace("LoadLayout " + path);
		return true;
	}

	void ResetLayout()
	{
		m_Model = BuildDefaultModel();
		m_Model.Version++;
		m_CurrentPanelId.clear();
		Trace("ResetLayout");
	}

	std::string DumpModel() const { return dock::SerializeLayout(m_Model, true); }

	// ---- Model / cache access for the projection widgets ----
	dock::NodeId FindPanelWell(const dock::PanelId& id) const
	{
		std::function<dock::NodeId(const dock::DockNode&)> search = [&](const dock::DockNode& n) -> dock::NodeId {
			if (n.Kind == dock::DockNode::Kind::TabWell)
				return dock::FindTabIndex(n, id) >= 0 ? n.Id : 0;
			if (n.Kind == dock::DockNode::Kind::Split)
			{
				for (const auto& c : n.Children)
				{
					dock::NodeId found = search(c);
					if (found) return found;
				}
			}
			else if (n.Kind == dock::DockNode::Kind::Floating && n.FloatChild)
				return search(*n.FloatChild);
			return 0;
		};
		for (const dock::WindowNode& w : m_Model.Windows)
		{
			dock::NodeId found = search(w.Main);
			if (found) return found;
			for (const auto& f : w.Floating)
			{
				found = search(f);
				if (found) return found;
			}
		}
		return 0;
	}

	// An optional icon string per panel (an icons:: glyph) - the tab
	// paints it ahead of the title.
	void SetPanelIcon(const dock::PanelId& id, std::string icon) { m_Icons[id] = std::move(icon); }
	std::string IconOf(const dock::PanelId& id) const
	{
		auto it = m_Icons.find(id);
		return it != m_Icons.end() ? it->second : std::string();
	}

	std::string DisplayNameOf(const dock::PanelId& id) const
	{
		auto it = m_Registry.find(id);
		return it != m_Registry.end() ? it->second.DisplayName : id;
	}

	std::shared_ptr<Widget> GetContent(const dock::PanelId& id)
	{
		auto it = m_Cache.find(id);
		if (it != m_Cache.end()) return it->second;
		auto defIt = m_Registry.find(id);
		if (defIt == m_Registry.end() || !defIt->second.Factory) return nullptr;
		auto content = defIt->second.Factory();
		m_Cache[id] = content;
		return content;
	}

	// Peek without instantiating: the cached content if present (may be an
	// empty shared_ptr after a non-KeepAlive close), or a null shared_ptr if
	// never created. Lets app-level code observe panel destruction without
	// accidentally re-running the factory.
	std::shared_ptr<Widget> PeekContent(const dock::PanelId& id)
	{
		auto it = m_Cache.find(id);
		return it != m_Cache.end() ? it->second : std::shared_ptr<Widget>();
	}

	dock::NodeId DefaultWell() const
	{
		const dock::WindowNode& main = m_Model.MainWindow();
		if (main.Main.Kind == dock::DockNode::Kind::TabWell && main.Main.Tabs.empty())
			return main.Main.Id;
		if (dock::DockNode* w = dock::FindFirstWell(const_cast<dock::DockNode&>(m_Model.MainWindow().Main)))
			return w->Id;
		return m_Model.MainWindow().Main.Id;
	}

	// First well of a given window (DFS), 0 if the window has none.
	dock::NodeId FirstWellOf(dock::NodeId windowId) const
	{
		const dock::WindowNode* win = nullptr;
		for (const dock::WindowNode& w : m_Model.Windows)
			if (w.Id == windowId) { win = &w; break; }
		if (!win) return 0;
		if (win->Main.Kind == dock::DockNode::Kind::TabWell)
			return win->Main.Id;
		if (dock::DockNode* w = dock::FindFirstWell(const_cast<dock::DockNode&>(win->Main)))
			return w->Id;
		return 0;
	}

	// The window's title: its first well's active tab's display name (what
	// tiling-WM bars/switchers show).
	std::string WindowTitleOf(dock::NodeId windowId) const
	{
		const dock::WindowNode* win = nullptr;
		for (const dock::WindowNode& w : m_Model.Windows)
			if (w.Id == windowId) { win = &w; break; }
		if (!win) return "Window";
		if (win->Main.Kind == dock::DockNode::Kind::TabWell)
		{
			const auto& tw = win->Main;
			if (tw.ActiveIndex >= 0 && tw.ActiveIndex < (int)tw.Tabs.size())
				return DisplayNameOf(tw.Tabs[tw.ActiveIndex].Id);
			if (!tw.Tabs.empty())
				return DisplayNameOf(tw.Tabs[0].Id);
			return "Window";
		}
		if (dock::DockNode* w = dock::FindFirstWell(const_cast<dock::DockNode&>(win->Main)))
		{
			if (w->ActiveIndex >= 0 && w->ActiveIndex < (int)w->Tabs.size())
				return DisplayNameOf(w->Tabs[w->ActiveIndex].Id);
		}
		return "Window";
	}

	// Close semantics: PerPanel applies the panel close policy to every
	// panel hosted by the window (destroy by default, cache KeepAlive),
	// then drops the WindowNode - the OS window closes on the next
	// projection sync. MergeToMain appends the tabs to the main window's
	// default well. The main window is never closed (invariant 10).
	bool CloseWindow(dock::NodeId windowId, dock::ClosePolicy policy)
	{
		const dock::WindowNode* win = nullptr;
		for (const dock::WindowNode& w : m_Model.Windows)
			if (w.Id == windowId) { win = &w; break; }
		if (!win || win->bMain)
			return false;

		if (policy == dock::ClosePolicy::PerPanel)
		{
			std::vector<dock::PanelId> panels;
			std::function<void(const dock::DockNode&)> collect = [&](const dock::DockNode& n) {
				if (n.Kind == dock::DockNode::Kind::TabWell)
					for (const auto& t : n.Tabs) panels.push_back(t.Id);
				else if (n.Kind == dock::DockNode::Kind::Split)
					for (const auto& c : n.Children) collect(c);
				else if (n.Kind == dock::DockNode::Kind::Floating && n.FloatChild)
					collect(*n.FloatChild);
			};
			collect(win->Main);
			for (const auto& f : win->Floating) collect(f);
			for (const dock::PanelId& id : panels)
				ApplyPanelClosePolicy(id);
		}

		bool ok = dock::CloseWindow(m_Model, windowId, policy);
		if (ok)
			Trace("CloseWindow id=" + std::to_string(windowId) + " policy="
				+ std::string(policy == dock::ClosePolicy::PerPanel ? "PerPanel" : "MergeToMain"));
		return ok;
	}

	dock::DockLayout BuildDefaultModel() const
	{
		dock::DockLayout l = dock::MakeDefaultLayout({});
		for (const auto& id : m_RegistrationOrder)
			dock::InsertTab(l, l.MainWindow().Main.Id, id, -1, false);
		if (!m_RegistrationOrder.empty())
			dock::SetActiveTab(l, l.MainWindow().Main.Id, 0);
		return l;
	}

	// Ensure a focus layer exists for every active panel across ALL windows
	// and (re)register per-subtree layer roots; then make the current
	// panel's layer current. Called by any DockSpace projection after a
	// chrome rebuild or model change.
	void RefreshFocusLayers()
	{
		if (!UIApplication::IsValid()) return;
		UIApplication& app = UIApplication::Get();

		std::vector<dock::PanelId> activePanels;
		std::function<void(const dock::DockNode&)> visit = [&](const dock::DockNode& n) {
			if (n.Kind == dock::DockNode::Kind::TabWell)
			{
				if (n.ActiveIndex >= 0 && n.ActiveIndex < (int)n.Tabs.size())
					activePanels.push_back(n.Tabs[n.ActiveIndex].Id);
			}
			else if (n.Kind == dock::DockNode::Kind::Split)
				for (const auto& c : n.Children) visit(c);
			else if (n.Kind == dock::DockNode::Kind::Floating && n.FloatChild)
				visit(*n.FloatChild);
		};
		for (const dock::WindowNode& w : m_Model.Windows)
		{
			visit(w.Main);
			for (const auto& f : w.Floating) visit(f);
		}

		for (const auto& id : activePanels)
		{
			std::string name = "panel:" + id;
			if (app.GetFocusLayerId(name) < 0)
				app.PushFocusLayer(name, UIApplication::FocusEntryPolicy::RestorePrevious);
		}

		std::function<void(const dock::DockNode&)> registerWells = [&](const dock::DockNode& n) {
			if (n.Kind == dock::DockNode::Kind::TabWell)
			{
				if (n.ActiveIndex >= 0 && n.ActiveIndex < (int)n.Tabs.size())
				{
					const dock::PanelId& id = n.Tabs[n.ActiveIndex].Id;
					std::string name = "panel:" + id;
					int lid = app.GetFocusLayerId(name);
					if (lid >= 0)
					{
						auto content = GetContent(id);
						if (content)
							app.RegisterFocusLayerRoot(content.get(), lid);
					}
				}
			}
			else if (n.Kind == dock::DockNode::Kind::Split)
				for (const auto& c : n.Children) registerWells(c);
			else if (n.Kind == dock::DockNode::Kind::Floating && n.FloatChild)
				registerWells(*n.FloatChild);
		};
		for (const dock::WindowNode& w : m_Model.Windows)
		{
			registerWells(w.Main);
			for (const auto& f : w.Floating) registerWells(f);
		}

		if (m_CurrentPanelId.empty() && !activePanels.empty())
			m_CurrentPanelId = activePanels[0];
		if (!m_CurrentPanelId.empty())
		{
			std::string name = "panel:" + m_CurrentPanelId;
			if (app.GetCurrentLayerName() != name)
				app.SwitchToLayer(name, UIApplication::FocusEntryPolicy::RestorePrevious);
		}
	}

	const std::unordered_map<dock::PanelId, PanelDef>& GetRegistry() const { return m_Registry; }
	const std::string& GetCurrentPanelId() const { return m_CurrentPanelId; }
	std::string& GetCurrentPanelIdMutable() { return m_CurrentPanelId; }

private:
	std::unordered_map<dock::PanelId, PanelDef> m_Registry;
	std::unordered_map<dock::PanelId, std::string> m_Icons;
	std::vector<dock::PanelId> m_RegistrationOrder;
	dock::DockLayout m_Model;
	std::unordered_map<dock::PanelId, std::shared_ptr<Widget>> m_Cache;
	std::string m_CurrentPanelId;
	std::unordered_map<const void*, std::function<void(const std::string&)>> m_TraceSinks;
};

} // namespace wisp
