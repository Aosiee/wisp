#pragma once

#include "Wisp/Core.h"
#include "Wisp/DrawList.h"
#include "Wisp/FontCache.h"
#include "Wisp/Style.h"
#include "Wisp/Theme.h"
#include "Wisp/Application.h"
#include "Wisp/Docking/DockModel.h"
#include "Wisp/Docking/DockState.h"
#include "Wisp/Docking/TabWell.h"
#include "Wisp/Docking/FloatingPanel.h"
#include "Wisp/Components/Splitter.h"
#include "Wisp/Components/Border.h"
#include "Wisp/Components/BoxPanel.h"
#include "Wisp/Components/Button.h"
#include "Wisp/Components/TextBlock.h"
#include "Wisp/Components/SizeBox.h"
#include "Wisp/Components/WidgetReflector.h"
#include "TinyEngine/Core/Application.h"
#include "TinyEngine/Core/Log.h"
#include <cstring>
#include <functional>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace wisp {

// DockSpace is the per-window PROJECTION of the shared DockState:
// each OS window hosts one DockSpace rendering the WindowNode whose id
// matches this widget's window id. Panels, the content cache, and the
// multi-window layout live in DockState - so panels move between windows,
// a window close routes through CloseWindow on the model, and every
// projection rebuilds from the same data.
//
// Construction stays compatible with the single-window shape: a
// DockSpace that is never handed a shared state (or a window id) creates
// and owns its own state and projects the main window - existing call
// sites (Sandbox demo) are unchanged.
class DockSpace : public Widget
{
public:
	WISP_WIDGET_TYPE(DockSpace)

	using PanelDef = DockState::PanelDef;

	DockSpace(FontCache* fc, FontHandle font, float fontSize)
		: m_FontCache(fc), m_Font(font), m_FontSize(fontSize)
	{
		m_Style = GetDefaultStyle();
		m_State = std::make_shared<DockState>();
		InstallTraceSink();
	}

	~DockSpace()
	{
		if (m_State)
			m_State->RemoveTraceSink(this);
	}

	static DockStyle GetDefaultStyle()
	{
		DockStyle s;
		s.Divider.Tint = { 0.16f, 0.16f, 0.24f, 1.0f };
		s.FloatingTitleBar.Tint = { 0.14f, 0.14f, 0.22f, 1.0f };
		s.FloatingTitleBarInactive.Tint = { 0.1f, 0.1f, 0.16f, 1.0f };
		s.DiamondIdle.Tint = { 0.1f, 0.1f, 0.16f, 1.0f };
		s.DiamondHovered.Tint = { 0.25f, 0.4f, 0.6f, 1.0f };
		return s;
	}

	// ---- Shared state / window binding ----
	// Adopt a shared state (multi-window) or keep the self-owned one.
	void SetState(std::shared_ptr<DockState> state)
	{
		if (!state) return;
		if (m_State)
			m_State->RemoveTraceSink(this);
		m_State = std::move(state);
		InstallTraceSink();
		InvalidateLayout();
	}
	std::shared_ptr<DockState> GetState() const { return m_State; }

	// Which WindowNode this widget projects (0 = the main window).
	void SetWindowId(dock::NodeId windowId) { m_WindowId = windowId; InvalidateLayout(); }
	dock::NodeId GetWindowId() const { return m_WindowId; }

	// The window node this widget projects (resolves 0 → bMain).
	const dock::WindowNode* GetWindowNode() const
	{
		const dock::DockLayout& model = m_State->GetModel();
		if (m_WindowId != 0)
		{
			for (const dock::WindowNode& w : model.Windows)
				if (w.Id == m_WindowId) return &w;
		}
		for (const dock::WindowNode& w : model.Windows)
			if (w.bMain) return &w;
		return nullptr;
	}

	// ---- Panel registry (delegates to the shared state) ----
	void RegisterPanel(const dock::PanelId& id, PanelDef def)
	{
		m_State->RegisterPanel(id, std::move(def));
	}

	// ---- Layout / model (delegates; each mutation invalidates this
	// widget's chrome, and the shared state version bump invalidates every
	// other window's projection too) ----
	void SetLayout(const dock::DockLayout& model)
	{
		m_State->SetLayout(model);
		InvalidateLayout();
	}

	const dock::DockLayout& GetModel() const { return m_State->GetModel(); }
	void RestoreSnapshot(const dock::DockLayout& snap)
	{
		m_State->RestoreSnapshot(snap);
		InvalidateLayout();
	}

	void OpenPanel(const dock::PanelId& id)
	{
		m_State->OpenPanel(id);
		InvalidateLayout();
	}
	void ClosePanel(const dock::PanelId& id)
	{
		m_State->ClosePanel(id);
		InvalidateLayout();
	}
	void ActivateTab(dock::NodeId wellId, int index)
	{
		m_State->ActivateTab(wellId, index);
		InvalidateLayout();
	}
	void ActivatePanel(const dock::PanelId& id)
	{
		m_State->ActivatePanel(id);
	}

	// Window close routes to the model — delegates to the shared state;
	// the OS window itself is destroyed by the projection sync
	// (DockWindowManager) once the node is gone. The main window is never
	// closed (invariant 10).
	void CloseWindow(dock::ClosePolicy policy)
	{
		const dock::WindowNode* win = GetWindowNode();
		if (!win) return;
		m_State->CloseWindow(win->Id, policy);
		InvalidateLayout();
	}

	// Tab context menu (command-driven window ops): right-click a
	// tab for Move to New Window / Move to Window ▸ / (non-main windows)
	// Merge Window into Main. Popup is window-local; menu callbacks capture
	// value data only (the popup may outlive chrome rebuilds).
	void ShowTabContextMenu(TabWell* well, int tabIndex, const glm::vec2& cursor)
	{
		dock::NodeId wellId = well->GetNodeId();
		const dock::DockNode* node = dock::FindNode(m_State->GetModel(), wellId);
		if (!node || tabIndex < 0 || tabIndex >= (int)node->Tabs.size()) return;
		dock::PanelId panel = node->Tabs[tabIndex].Id;
		const dock::WindowNode* win = GetWindowNode();

		auto list = std::make_unique<VerticalBox>(BoxOrientation::Vertical);
		auto addItem = [&](const std::string& label, std::function<void()> fn) {
			auto row = std::make_unique<Button>();
			row->SetNormalBrush({ glm::vec4(0.1f, 0.1f, 0.16f, 1) });
			row->SetHoveredBrush({ glm::vec4(0.25f, 0.5f, 0.8f, 0.8f) });
			row->SetPressedBrush({ glm::vec4(0.15f, 0.3f, 0.5f, 1) });
			// Deferred dismissal: the action may destroy this very Button's
			// tree (DismissPopups frees it), and the Button's own code runs
			// after the callback returns - so the click handler only
			// REQUESTS dismissal; UIApplication flushes it after dispatch.
			row->SetOnClicked([fn = std::move(fn)]() {
				if (fn) fn();
				UIApplication::Get().RequestDismissPopups();
			});
			row->SetChild(std::make_unique<TextBlock>(m_FontCache, m_Font, m_FontSize, label, glm::vec4(0.9f, 0.9f, 0.95f, 1)));
			list->AddChild(std::move(row));
		};

		addItem("Move to New Window", [this, wellId, tabIndex, panel]() {
			dock::NodeId newWin = dock::OpenWindow(m_State->GetModelMutable(), wellId, tabIndex);
			Trace("OpenWindow well=" + std::to_string(wellId) + " panel=" + panel + " -> window=" + std::to_string(newWin));
		});

		// One entry per other window (title = its active tab's display name).
		for (const dock::WindowNode& w : m_State->GetModel().Windows)
		{
			if (win && w.Id == win->Id) continue;
			std::string title = m_State->WindowTitleOf(w.Id);
			addItem("Move to Window: " + title, [this, wellId, tabIndex, panel, target = w.Id]() {
				const dock::WindowNode* tw = nullptr;
				for (const dock::WindowNode& w2 : m_State->GetModel().Windows)
					if (w2.Id == target) { tw = &w2; break; }
				if (!tw) return;
				dock::NodeId targetWell = m_State->FirstWellOf(target);
				if (targetWell == 0) return;
				dock::DropTarget t;
				t.KindId = dock::DropTarget::Kind::WellZone;
				t.Well = targetWell;
				t.ZoneId = dock::Zone::Center;
				m_State->GetModelMutable() = dock::MoveToWindow(m_State->GetModel(), wellId, tabIndex, target, t);
				Trace("MoveToWindow panel=" + panel + " well=" + std::to_string(wellId) + " -> window=" + std::to_string(target));
			});
		}

		if (win && !win->bMain)
		{
			dock::NodeId windowId = win->Id;
			addItem("Merge Window into Main", [this, windowId]() {
				m_State->CloseWindow(windowId, dock::ClosePolicy::MergeToMain);
			});
		}

		auto border = std::make_unique<Border>();
		border->SetBrush({ glm::vec4(0.1f, 0.1f, 0.16f, 0.97f) });
		border->SetPadding(2);
		border->SetChild(std::move(list));

		float menuW = 120.0f;
		for (const dock::WindowNode& w : m_State->GetModel().Windows)
		{
			if (win && w.Id == win->Id) continue;
			float tw = m_FontCache->MeasureText(m_Font, "Move to Window: " + m_State->WindowTitleOf(w.Id), m_FontSize).x + 24.0f;
			menuW = std::max(menuW, tw);
		}

		auto sized = std::make_unique<SizeBox>();
		sized->SetWidthOverride(menuW);
		sized->SetChild(std::move(border));

		UIApplication::Get().ShowPopupAt(std::move(sized), cursor);
	}

	bool SaveLayout(const std::string& path) const { return m_State->SaveLayout(path); }
	bool LoadLayout(const std::string& path)
	{
		bool ok = m_State->LoadLayout(path, m_LastSize);
		if (ok) InvalidateLayout();
		return ok;
	}
	void ResetLayout()
	{
		m_State->ResetLayout();
		InvalidateLayout();
	}
	std::string DumpModel() const { return m_State->DumpModel(); }
	void SetTraceEnabled(bool on) { m_Trace = on; }

	uint64_t GetChromeGeneration() const { return m_ChromeGeneration; }

	void Trace(const std::string& msg) const
	{
		if (m_Trace)
			TE_CORE_INFO(TinyEngine::LogCore, "DOCK: {0}", msg);
	}

	void MoveFloatingPanel(dock::NodeId floatingId, const glm::vec2& pos)
	{
		if (dock::MoveFloating(m_State->GetModelMutable(), floatingId, pos))
			Trace("MoveFloating " + std::to_string(floatingId));
		InvalidateLayout();
	}

	void ResizeFloatingPanel(dock::NodeId floatingId, const glm::vec2& size)
	{
		if (dock::ResizeFloating(m_State->GetModelMutable(), floatingId, size))
			Trace("ResizeFloating " + std::to_string(floatingId));
		InvalidateLayout();
	}

	dock::PanelId FloatingActivePanel(dock::NodeId floatingId) const
	{
		const dock::WindowNode* win = GetWindowNode();
		if (!win) return "";
		for (const auto& f : win->Floating)
		{
			if (f.Id != floatingId || !f.FloatChild) continue;
			if (f.FloatChild->Kind == dock::DockNode::Kind::TabWell)
			{
				const auto& tw = *f.FloatChild;
				if (tw.ActiveIndex >= 0 && tw.ActiveIndex < (int)tw.Tabs.size())
					return tw.Tabs[tw.ActiveIndex].Id;
			}
		}
		return "";
	}

	// Live arranged geometry vs the pure solver.
	std::unordered_map<dock::NodeId, glm::vec4> GetLiveNodeRects() const
	{
		if (m_BuiltVersion != m_State->GetModel().Version) RebuildChrome();
		std::unordered_map<dock::NodeId, glm::vec4> out;
		for (auto& c : m_Children)
			CollectLiveRects(c.get(), out);
		const dock::WindowNode* win = GetWindowNode();
		if (win)
			for (const auto& f : win->Floating)
				out[f.Id] = { f.FloatPos.x, f.FloatPos.y, f.FloatSize.x, f.FloatSize.y };
		return out;
	}
	void CollectLiveRects(Widget* w, std::unordered_map<dock::NodeId, glm::vec4>& out) const
	{
		if (!w) return;
		if (std::strcmp(w->GetTypeName(), "TabWell") == 0)
			out[static_cast<TabWell*>(w)->GetNodeId()] = static_cast<TabWell*>(w)->GetLastAbsRect();
		w->ForEachChild([&](Widget* c) { CollectLiveRects(c, out); });
	}

	std::vector<dock::NodeRect> GetSolverRects() const
	{
		dock::MinSizeFn mins = [this](dock::NodeId id) {
			const dock::DockNode* n = dock::FindNode(m_State->GetModel(), id);
			if (!n || n->Kind != dock::DockNode::Kind::TabWell) return glm::vec2(60.0f, 60.0f);
			glm::vec2 m(60.0f, 60.0f);
			for (const auto& t : n->Tabs)
			{
				auto it = m_State->GetRegistry().find(t.Id);
				if (it != m_State->GetRegistry().end())
					m = glm::max(m, it->second.MinSize);
			}
			return m;
		};
		const dock::WindowNode* win = GetWindowNode();
		if (!win) return {};
		return dock::ComputeNodeRects(m_State->GetModel(), win->Id, m_LastSize, mins, m_Style.DividerThickness);
	}

	// ---- Widget ----
	glm::vec2 ComputeDesiredSize() const override
	{
		if (m_BuiltVersion != m_State->GetModel().Version)
			RebuildChrome();
		float maxW = 0.0f, maxH = 0.0f;
		for (auto& c : m_Children)
		{
			if (!c) continue;
			glm::vec2 d = c->ComputeDesiredSize();
			c->SetCachedDesiredSize(d);
			maxW = std::max(maxW, d.x);
			maxH = std::max(maxH, d.y);
		}
		return { maxW, maxH };
	}

	void ArrangeChildren(const Geometry& geom, ArrangedChildren& out) const override
	{
		if (m_BuiltVersion != m_State->GetModel().Version)
			RebuildChrome();
		m_LastSize = geom.LocalSize;
		m_LastAbsPos = geom.LocalToAbsolute({ 0, 0 });

		if (!m_Children.empty() && m_Children[0])
			out.Add(m_Children[0].get(), geom);

		const dock::WindowNode* win = GetWindowNode();
		size_t floatCount = win ? win->Floating.size() : 0;
		for (size_t i = 1; i < m_Children.size() && i <= floatCount; i++)
		{
			const auto& fl = win->Floating[i - 1];
			glm::vec2 pos = fl.FloatPos;
			glm::vec2 size = fl.FloatSize;
			out.Add(m_Children[i].get(), geom.MakeChild(pos, size));
		}
	}

	int OnPaint(const PaintArgs& args, const Geometry& geom, DrawList& drawList, int layerId, const WidgetStyleContext& style) const override
	{
		if (m_StyleDirty)
		{
			m_Style = ResolveStyle(this, GetDefaultStyle());
			m_StyleDirty = false;
		}
		if (m_BuiltVersion != m_State->GetModel().Version)
			RebuildChrome();
		m_LastSize = geom.LocalSize;
		m_LastAbsPos = geom.LocalToAbsolute({ 0, 0 });

		ArrangedChildren arranged;
		ArrangeChildren(geom, arranged);
		int nextLayer = layerId;
		WidgetStyleContext cs = style;
		for (auto& child : arranged)
		{
			cs.ColorAndOpacity = style.ColorAndOpacity * child.WidgetPtr->GetRenderTint();
			cs.ColorAndOpacity.a *= child.WidgetPtr->GetRenderOpacity();
			nextLayer = child.WidgetPtr->OnPaint(args, child.Geom, drawList, nextLayer, cs);
		}

		// Paint the drop preview here (drag overlay band).
		PaintDropPreview(drawList);

		return nextLayer;
	}

	void ForEachChild(std::function<void(Widget*)> fn) override
	{
		if (m_BuiltVersion != m_State->GetModel().Version)
			RebuildChrome();
		for (auto& c : m_Children)
			if (c) fn(c.get());
	}

	// ---- Chrome generation (projects THIS widget's window node) ----
	void RebuildChrome() const
	{
		m_ChromeGeneration++;
		m_BuiltVersion = m_State->GetModel().Version;
		m_Children.clear();
		m_ChildNodeIds.clear();

		// The old chrome dies here; anything the input system cached about it
		// (last-frame hover path, mouse capture) must not outlive it. Reset
		// this window's context, not just the focused one.
		if (UIApplication::IsValid())
			UIApplication::Get().ResetTransientInputState(UIApplication::Get().GetContextContaining(const_cast<DockSpace*>(this)));

		Trace("rebuild chrome v=" + std::to_string(m_State->GetModel().Version));
		const dock::WindowNode* win = GetWindowNode();
		if (!win) return;
		m_Children.push_back(BuildNodeWidget(win->Main));
		m_ChildNodeIds.push_back(win->Main.Id);
		if (m_Children.back())
			SetupParentChild(const_cast<DockSpace*>(this), m_Children.back().get());
		for (const auto& f : win->Floating)
		{
			auto fp = std::make_unique<FloatingPanel>();
			fp->SetDockSpace(const_cast<DockSpace*>(this), f.Id);
			fp->SetFont(m_FontCache, m_Font, m_FontSize);
			fp->SetTitle(FloatingTitleOf(f));
			fp->SetActive(&f == &win->Floating.back());
			fp->SetContent(BuildNodeWidget(*f.FloatChild));
			m_Children.push_back(std::move(fp));
			m_ChildNodeIds.push_back(f.Id);
			SetupParentChild(const_cast<DockSpace*>(this), m_Children.back().get());
		}

		m_State->RefreshFocusLayers();
	}

	std::unique_ptr<Widget> BuildNodeWidget(const dock::DockNode& node) const
	{
		if (node.Kind == dock::DockNode::Kind::TabWell)
		{
			auto tw = std::make_unique<TabWell>();
			tw->SetDockSpace(const_cast<DockSpace*>(this), node.Id);
			tw->SetFont(m_FontCache, m_Font, m_FontSize);
			std::vector<std::string> names;
			for (const auto& t : node.Tabs)
				names.push_back(m_State->DisplayNameOf(t.Id));
			tw->SetTabs(node.Tabs, names, node.ActiveIndex);
			if (node.ActiveIndex >= 0 && node.ActiveIndex < (int)node.Tabs.size())
				tw->SetContent(m_State->GetContent(node.Tabs[node.ActiveIndex].Id));
			return tw;
		}
		if (node.Kind == dock::DockNode::Kind::Split)
		{
			auto sp = std::make_unique<Splitter>(
				node.SplitAxis == dock::Axis::Horizontal ? SplitterOrientation::Horizontal : SplitterOrientation::Vertical);
			sp->SetNodeId(node.Id);
			sp->SetDividerThickness(m_Style.DividerThickness);
			sp->SetOnDividerCommitted([this, splitId = node.Id](int childIndex, float ratio) {
				dock::SetSplitRatio(m_State->GetModelMutable(), splitId, childIndex, ratio);
				Trace("SetSplitRatio split=" + std::to_string(splitId) + " idx=" + std::to_string(childIndex));
			});
			for (int i = 0; i < (int)node.Children.size(); i++)
			{
				float ratio = i < (int)node.Ratios.size() ? node.Ratios[i] : 1.0f;
				auto& slot = sp->AddSlot(BuildNodeWidget(node.Children[i]), ratio);
				slot.MainMinSize = SubtreeMin(node.Children[i], node.SplitAxis);
			}
			return sp;
		}
		return std::make_unique<Border>();
	}

	float SubtreeMin(const dock::DockNode& node, dock::Axis axis) const
	{
		if (node.Kind == dock::DockNode::Kind::TabWell)
		{
			float m = 60.0f;
			for (const auto& t : node.Tabs)
			{
				auto it = m_State->GetRegistry().find(t.Id);
				if (it != m_State->GetRegistry().end())
					m = std::max(m, axis == dock::Axis::Horizontal ? it->second.MinSize.x : it->second.MinSize.y);
			}
			return m;
		}
		if (node.Kind == dock::DockNode::Kind::Split)
		{
			float total = 0.0f;
			for (const auto& c : node.Children)
			{
				float m = SubtreeMin(c, axis);
				total = node.SplitAxis == axis ? total + m : std::max(total, m);
			}
			return total;
		}
		return 60.0f;
	}

	std::string FloatingTitleOf(const dock::DockNode& f) const
	{
		if (f.FloatChild && f.FloatChild->Kind == dock::DockNode::Kind::TabWell)
		{
			const auto& w = *f.FloatChild;
			if (w.ActiveIndex >= 0 && w.ActiveIndex < (int)w.Tabs.size())
				return m_State->DisplayNameOf(w.Tabs[w.ActiveIndex].Id);
			if (!w.Tabs.empty())
				return m_State->DisplayNameOf(w.Tabs[0].Id);
		}
		return "Floating";
	}

	void CloseFloating(dock::NodeId floatingId)
	{
		if (dock::RemoveFloating(m_State->GetModelMutable(), floatingId))
			Trace("RemoveFloating " + std::to_string(floatingId));
		InvalidateLayout();
	}

	void BringFloatingToFrontPublic(dock::NodeId floatingId)
	{
		if (dock::BringFloatingToFront(m_State->GetModelMutable(), floatingId))
			Trace("BringFloatingToFront " + std::to_string(floatingId));
		InvalidateLayout();
	}

	DragPayload CreateFloatingDragPayload(dock::NodeId floatingId, const dock::PanelId& panel)
	{
		DockSpace* self = this;
		DragPayload pl;
		pl.Type = "DockTab";
		pl.Data = DockTabDragData{ this, panel, 0, floatingId };
		pl.PaintGhost = [self, panel](DrawList& dl, int layer, const glm::vec2& cursor) {
			glm::vec2 pos = cursor + glm::vec2(10.0f, 12.0f);
			std::string name = self->m_State->DisplayNameOf(panel);
			glm::vec2 size = { std::max(96.0f, dockpaint::MeasureWidth(self->m_FontCache, self->m_Font, name, self->m_FontSize, 1.0f) + 16.0f), 30.0f };
			dl.AddBox(layer, pos, size, { 0.16f, 0.22f, 0.34f, 0.9f }, -1);
			dl.AddBox(layer, pos, { 2.0f, size.y }, { 0.9f, 0.6f, 0.2f, 1.0f }, -1);
			float top, bottom;
			dockpaint::MeasureInkBox(self->m_FontCache, self->m_Font, name, self->m_FontSize, 1.0f, top, bottom);
			dockpaint::PaintText(dl, self->m_FontCache, self->m_Font, self->m_FontSize, layer + 1,
				{ pos.x + 8.0f, pos.y + (size.y - (bottom - top)) * 0.5f - top }, name,
				{ 0.9f, 0.9f, 1.0f, 1.0f }, -1, 1.0f);
		};
		pl.OnCancelled = [self, snap = std::make_shared<dock::DockLayout>(dock::CloneLayout(self->m_State->GetModel()))]() {
			self->m_State->RestoreSnapshot(*snap);
			self->InvalidateLayout();
			self->Trace("floating drag cancelled - snapshot restored");
		};
		return pl;
	}

	dock::NodeId DefaultWell() const { return m_State->DefaultWell(); }

	// Drag & drop (router-driven; see Application.cpp).
	struct DockTabDragData
	{
		DockSpace* Space = nullptr;
		dock::PanelId Panel;
		dock::NodeId SourceWell = 0;
		dock::NodeId SourceFloating = 0;
	};

	DragPayload CreateTabDragPayload(dock::NodeId well, const dock::PanelId& panel)
	{
		const dock::DockNode* wn = dock::FindNode(m_State->GetModel(), well);
		if (!wn || dock::FindTabIndex(*wn, panel) < 0)
			return {};
		DockSpace* self = this;
		DragPayload pl;
		pl.Type = "DockTab";
		pl.Data = DockTabDragData{ this, panel, well };
		pl.PaintGhost = [self, panel](DrawList& dl, int layer, const glm::vec2& cursor) {
			glm::vec2 pos = cursor + glm::vec2(10.0f, 12.0f);
			std::string name = self->m_State->DisplayNameOf(panel);
			glm::vec2 size = { std::max(96.0f, dockpaint::MeasureWidth(self->m_FontCache, self->m_Font, name, self->m_FontSize, 1.0f) + 16.0f), 30.0f };
			dl.AddBox(layer, pos, size, { 0.16f, 0.22f, 0.34f, 0.9f }, -1);
			dl.AddBox(layer, pos, { 2.0f, size.y }, { 0.3f, 0.55f, 0.9f, 1.0f }, -1);
			float top, bottom;
			dockpaint::MeasureInkBox(self->m_FontCache, self->m_Font, name, self->m_FontSize, 1.0f, top, bottom);
			dockpaint::PaintText(dl, self->m_FontCache, self->m_Font, self->m_FontSize, layer + 1,
				{ pos.x + 8.0f, pos.y + (size.y - (bottom - top)) * 0.5f - top }, name,
				{ 0.9f, 0.9f, 1.0f, 1.0f }, -1, 1.0f);
		};
		pl.OnCancelled = [self, snap = std::make_shared<dock::DockLayout>(dock::CloneLayout(self->m_State->GetModel()))]() {
			self->m_State->RestoreSnapshot(*snap);
			self->InvalidateLayout();
			self->Trace("drag cancelled - snapshot restored");
		};
		// Tear-out: released outside every window → OpenWindow, spawned
		// under the cursor where the platform can place windows.
		pl.OnDroppedOutside = [self, well, panel](DragOperation& op) {
			dock::DockLayout& model = self->m_State->GetModelMutable();
			const dock::DockNode* wn = dock::FindNode(model, well);
			int idx = wn ? dock::FindTabIndex(*wn, panel) : -1;
			if (idx < 0) return;
			dock::NodeId newWin = dock::OpenWindow(model, well, idx);
			self->Trace("OpenWindow well=" + std::to_string(well) + " panel=" + panel
				+ " -> window=" + std::to_string(newWin) + " (tear-out)");
			if (newWin && TinyEngine::DetectWindowCaps().CanPlaceWindows)
			{
				WindowContext* ctx = UIApplication::IsValid()
					? UIApplication::Get().GetContextContaining(self) : nullptr;
				if (ctx)
				{
					auto wp = TinyEngine::TryGetWindowPos((GLFWwindow*)ctx->Window);
					if (wp)
					{
						glm::vec2 global = { (float)wp->first + op.CurrentPos.x, (float)wp->second + op.CurrentPos.y };
						for (dock::WindowNode& w : model.Windows)
							if (w.Id == newWin)
								w.Pos = global - w.Size * 0.5f;
					}
				}
			}
			self->InvalidateLayout();
		};
		return pl;
	}

	Reply OnDragOver(DragOperation& op) override
	{
		if (op.Payload.Type != "DockTab") return Reply::Unhandled();
		auto* data = std::any_cast<DockTabDragData>(&op.Payload.Data);
		// Cross-window drags land on a DIFFERENT projection widget; the
		// guard is the shared DockState, not the projection identity.
		if (!data || !data->Space || data->Space->GetState() != m_State) return Reply::Unhandled();

		m_DragPanel = data->Panel;
		m_DragSourceWell = data->SourceWell;
		m_DragSourceFloating = data->SourceFloating;
		m_DragHasData = true;

		// The cursor position in THIS window's local space (the router
		// converts across windows).
		const glm::vec2 cursor = op.CurrentPos;
		QueryDropTarget(cursor, m_ActiveDropTarget, m_ActivePreviewRect);

		// In-bar reorder: cursor over the source well's tab bar (well sources
		// only — a floating panel's title drag never reorders).
		if (data->SourceFloating == 0
			&& m_ActiveDropTarget.KindId == dock::DropTarget::Kind::WellZone
			&& m_ActiveDropTarget.Well == data->SourceWell
			&& m_ActiveDropTarget.ZoneId == dock::Zone::Center)
		{
			if (TabWell* tw = FindTabWellWidget(data->SourceWell))
			{
				int idx = tw->HitTestTabs(cursor);
				const dock::DockNode* wn = dock::FindNode(m_State->GetModel(), data->SourceWell);
				if (idx >= 0 && wn)
				{
					int cur = dock::FindTabIndex(*wn, data->Panel);
					if (cur >= 0 && idx != cur)
					{
						dock::MovePanel(m_State->GetModelMutable(), data->SourceWell, cur, data->SourceWell, idx);
						Trace("MovePanel " + data->Panel + " -> " + std::to_string(idx));
					}
				}
			}
		}
		return Reply::Handled_();
	}

	// The drag left this window (a window switch is a target switch): the
	// per-window drop preview must not outlive the hover.
	Reply OnDragLeave(DragOperation& op) override
	{
		(void)op;
		ClearDragState();
		return Reply::Unhandled();
	}

	void ClearDragState()
	{
		m_DragHasData = false;
		m_ActivePreviewRect = { 0, 0, 0, 0 };
		m_DragPanel.clear();
		m_DragSourceWell = 0;
		m_DragSourceFloating = 0;
	}

	Reply OnDrop(DragOperation& op) override
	{
		if (op.Payload.Type != "DockTab") return Reply::Unhandled();
		auto* data = std::any_cast<DockTabDragData>(&op.Payload.Data);
		if (!data || !data->Space || data->Space->GetState() != m_State) return Reply::Unhandled();

		const glm::vec2 cursor = op.CurrentPos;
		dock::DropTarget target;
		glm::vec4 preview;
		QueryDropTarget(cursor, target, preview);

		const dock::WindowNode* win = GetWindowNode();

		dock::DropPayload p;
		p.Panel = data->Panel;
		p.SourceWell = data->SourceWell;
		p.SourceFloating = data->SourceFloating;
		auto regIt = m_State->GetRegistry().find(data->Panel);
		p.Singleton = regIt != m_State->GetRegistry().end() && regIt->second.Singleton;

		m_State->GetModelMutable() = dock::ApplyDrop(m_State->GetModel(), win ? win->Id : 0, target, p);
		Trace("ApplyDrop panel=" + data->Panel + " target=" + std::to_string((int)target.KindId)
			+ " srcFloating=" + std::to_string(data->SourceFloating));
		if (data->SourceFloating == 0)
			m_State->ActivatePanel(data->Panel);
		InvalidateLayout();
		return Reply::Handled_();
	}

	TabWell* FindTabWellWidget(dock::NodeId id) const
	{
		TabWell* found = nullptr;
		std::function<void(Widget*)> walk = [&](Widget* w) {
			if (!w || found) return;
			if (std::strcmp(w->GetTypeName(), "TabWell") == 0 && static_cast<TabWell*>(w)->GetNodeId() == id)
			{
				found = static_cast<TabWell*>(w);
				return;
			}
			w->ForEachChild([&](Widget* c) { walk(c); });
		};
		for (auto& c : m_Children)
			walk(c.get());
		return found;
	}

	void QueryDropTarget(const glm::vec2& cursorPos, dock::DropTarget& outTarget, glm::vec4& outPreviewRect) const
	{
		outTarget.KindId = dock::DropTarget::Kind::Miss;
		outTarget.MissPos = cursorPos;
		outPreviewRect = { 0, 0, 0, 0 };

		glm::vec2 dockPos = m_LastAbsPos;
		glm::vec2 dockSize = m_LastSize;
		if (dockSize.x <= 0 || dockSize.y <= 0) return;
		const float edgeBand = 24.0f;

		bool insideDock = (cursorPos.x >= dockPos.x && cursorPos.x <= dockPos.x + dockSize.x
			&& cursorPos.y >= dockPos.y && cursorPos.y <= dockPos.y + dockSize.y);
		bool inStrip = insideDock
			&& ((cursorPos.y < dockPos.y + edgeBand) || (cursorPos.y > dockPos.y + dockSize.y - edgeBand)
				|| (cursorPos.x < dockPos.x + edgeBand) || (cursorPos.x > dockPos.x + dockSize.x - edgeBand));
		dock::Edge stripEdge = dock::Edge::East;
		if (cursorPos.y < dockPos.y + edgeBand) stripEdge = dock::Edge::North;
		else if (cursorPos.y > dockPos.y + dockSize.y - edgeBand) stripEdge = dock::Edge::South;
		else if (cursorPos.x < dockPos.x + edgeBand) stripEdge = dock::Edge::West;

		dock::NodeId hoverWell = 0;
		glm::vec4 hoverRect{ 0, 0, 0, 0 };
		{
			float bestArea = 1e30f;
			for (auto& [id, r] : GetLiveNodeRects())
			{
				if (cursorPos.x >= r.x && cursorPos.x <= r.x + r.z && cursorPos.y >= r.y && cursorPos.y <= r.y + r.w)
				{
					float area = r.z * r.w;
					if (area < bestArea) { bestArea = area; hoverWell = id; hoverRect = r; }
				}
			}
		}

		bool overTabBar = false;
		if (hoverWell)
		{
			if (TabWell* tw = FindTabWellWidget(hoverWell))
				overTabBar = tw->HitTestTabs(cursorPos) >= 0;
		}

		if (hoverWell && overTabBar)
		{
			outTarget.KindId = dock::DropTarget::Kind::WellZone;
			outTarget.Well = hoverWell;
			outTarget.ZoneId = dock::Zone::Center;
			outPreviewRect = hoverRect;
		}
		else if (inStrip)
		{
			outTarget.KindId = dock::DropTarget::Kind::RootEdge;
			outTarget.RootEdge = stripEdge;
			outPreviewRect = PreviewRectFor(dock::DropTarget{ dock::DropTarget::Kind::RootEdge, 0, dock::Zone::Center, stripEdge, {} });
		}
		else if (hoverWell)
		{
			outTarget.KindId = dock::DropTarget::Kind::WellZone;
			outTarget.Well = hoverWell;
			float dx = hoverRect.z > 0 ? (cursorPos.x - hoverRect.x) / hoverRect.z : 0.5f;
			float dy = hoverRect.w > 0 ? (cursorPos.y - hoverRect.y) / hoverRect.w : 0.5f;
			const float m = 0.28f;
			if (dx >= m && dx <= 1.0f - m && dy >= m && dy <= 1.0f - m)
				outTarget.ZoneId = dock::Zone::Center;
			else
			{
				float dl = dx, dr = 1.0f - dx, dt = dy, db = 1.0f - dy;
				float dmin = std::min(std::min(dl, dr), std::min(dt, db));
				if (dmin == dt) outTarget.ZoneId = dock::Zone::North;
				else if (dmin == db) outTarget.ZoneId = dock::Zone::South;
				else if (dmin == dl) outTarget.ZoneId = dock::Zone::West;
				else outTarget.ZoneId = dock::Zone::East;
			}
			outPreviewRect = PreviewRectFor(outTarget);
		}
	}

	// Dry-run ApplyDrop + solver: the exact rect the drop will produce.
	glm::vec4 PreviewRectFor(const dock::DropTarget& target) const
	{
		if (target.KindId == dock::DropTarget::Kind::Miss) return { 0, 0, 0, 0 };
		if (!m_DragHasData) return { 0, 0, 0, 0 };
		dock::DropPayload p;
		p.Panel = m_DragPanel;
		p.SourceWell = m_DragSourceWell;
		p.SourceFloating = m_DragSourceFloating;
		const dock::WindowNode* win = GetWindowNode();
		dock::DockLayout preview = dock::ApplyDrop(m_State->GetModel(), win ? win->Id : 0, target, p);
		dock::MinSizeFn mins = [this, &preview](dock::NodeId id) {
			const dock::DockNode* n = dock::FindNode(preview, id);
			if (!n || n->Kind != dock::DockNode::Kind::TabWell) return glm::vec2(60.0f, 60.0f);
			glm::vec2 mm(60.0f, 60.0f);
			for (const auto& t : n->Tabs)
			{
				auto it = m_State->GetRegistry().find(t.Id);
				if (it != m_State->GetRegistry().end())
					mm = glm::max(mm, it->second.MinSize);
			}
			return mm;
		};
		const dock::WindowNode* pwin = nullptr;
		for (const dock::WindowNode& w : preview.Windows)
			if (win && w.Id == win->Id) { pwin = &w; break; }
		if (!pwin && !preview.Windows.empty()) pwin = &preview.MainWindow();
		if (!pwin) return { 0, 0, 0, 0 };
		auto rects = dock::ComputeNodeRects(preview, pwin->Id, m_LastSize, mins, m_Style.DividerThickness);
		dock::NodeId panelWell = dock::FindWellContaining(preview, m_DragPanel);
		for (const auto& r : rects)
			if (r.Id == panelWell)
				return r.Rect;
		return { 0, 0, 0, 0 };
	}

	void PaintDropPreview(DrawList& drawList) const
	{
		if (!UIApplication::IsValid() || !UIApplication::Get().IsDragActive()) return;
		if (m_ActivePreviewRect.z <= 0 || m_ActivePreviewRect.w <= 0) return;

		glm::vec4 r = m_ActivePreviewRect;
		glm::vec4 tint = m_Style.DropPreviewTint;
		drawList.AddBox(90100, { r.x, r.y }, { r.z, r.w }, tint, -1);
		drawList.AddBox(90101, { r.x, r.y }, { r.z, 1.0f }, { 0.3f, 0.6f, 1.0f, 0.7f }, -1);
		drawList.AddBox(90101, { r.x, r.y + r.w - 1.0f }, { r.z, 1.0f }, { 0.3f, 0.6f, 1.0f, 0.7f }, -1);
		drawList.AddBox(90101, { r.x, r.y }, { 1.0f, r.w }, { 0.3f, 0.6f, 1.0f, 0.7f }, -1);
		drawList.AddBox(90101, { r.x + r.z - 1.0f, r.y }, { 1.0f, r.w }, { 0.3f, 0.6f, 1.0f, 0.7f }, -1);

		if (m_ActiveDropTarget.KindId == dock::DropTarget::Kind::WellZone)
		{
			glm::vec2 c = { r.x + r.z * 0.5f, r.y + r.w * 0.5f };
			const float sz = 8.0f, gap = 10.0f;
			auto cell = [&](float cx, float cy, bool hot) {
				drawList.AddBox(90102, { cx - sz * 0.5f, cy - sz * 0.5f }, { sz, sz },
					hot ? m_Style.DiamondHovered.Tint : m_Style.DiamondIdle.Tint, -1);
			};
			dock::Zone z = m_ActiveDropTarget.ZoneId;
			cell(c.x, c.y, z == dock::Zone::Center);
			cell(c.x, c.y - gap - sz, z == dock::Zone::North);
			cell(c.x, c.y + gap + sz, z == dock::Zone::South);
			cell(c.x - gap - sz, c.y, z == dock::Zone::West);
			cell(c.x + gap + sz, c.y, z == dock::Zone::East);
		}
	}

	// Keep API stable.
	void CommitDrop(const dock::DropTarget& target, const dock::DropPayload& payload)
	{
		const dock::WindowNode* win = GetWindowNode();
		m_State->GetModelMutable() = dock::ApplyDrop(m_State->GetModel(), win ? win->Id : 0, target, payload);
		Trace("ApplyDrop panel=" + payload.Panel + " target=" + std::to_string((int)target.KindId)
			+ " srcFloating=" + std::to_string(payload.SourceFloating));
		InvalidateLayout();
	}
	void DragReorderPanel(const dock::PanelId& id, dock::NodeId wellId, int targetIndex)
	{
		const dock::DockNode* wn = dock::FindNode(m_State->GetModel(), wellId);
		if (!wn) return;
		int cur = dock::FindTabIndex(*wn, id);
		if (cur >= 0)
			dock::MovePanel(m_State->GetModelMutable(), wellId, cur, wellId, targetIndex);
	}

	FontCache* GetFontCacheForDrag() const { return m_FontCache; }
	FontHandle GetFontForDrag() const { return m_Font; }
	float GetFontSizeForDrag() const { return m_FontSize; }

private:
	// This projection's trace sink on the shared state: multiple projections
	// share one state, so sinks accumulate per owner and only log when THIS
	// projection has tracing enabled.
	void InstallTraceSink()
	{
		m_State->AddTraceSink(this, [this](const std::string& msg) {
			if (m_Trace)
				TE_CORE_INFO(TinyEngine::LogCore, "DOCK: {0}", msg);
		});
	}

	FontCache* m_FontCache;
	FontHandle m_Font;
	float m_FontSize;

	// Shared model + registry + cache (one per app); this widget projects
	// the WindowNode with m_WindowId (0 = the main window).
	std::shared_ptr<DockState> m_State;
	dock::NodeId m_WindowId = 0;

	mutable uint64_t m_ChromeGeneration = 0;
	mutable uint64_t m_BuiltVersion = ~0ull;
	mutable std::vector<std::unique_ptr<Widget>> m_Children;
	mutable std::vector<dock::NodeId> m_ChildNodeIds;
	mutable bool m_Trace = false;
	mutable DockStyle m_Style;
	mutable bool m_StyleDirty = true;

	mutable glm::vec2 m_LastSize{ 0, 0 };
	mutable glm::vec2 m_LastAbsPos{ 0, 0 };

	// Drag state (per-frame, refreshed by OnDragOver)
	mutable dock::DropTarget m_ActiveDropTarget;
	mutable glm::vec4 m_ActivePreviewRect{ 0, 0, 0, 0 };
	mutable dock::PanelId m_DragPanel;
	mutable dock::NodeId m_DragSourceWell = 0;
	mutable dock::NodeId m_DragSourceFloating = 0;
	mutable bool m_DragHasData = false;
};

// ---- TabWell methods that need the complete DockSpace type ----------------

inline Reply TabWell::OnMouseButtonDown(const glm::vec2& absPos, int button)
{
	if (button == 0)
	{
		int scrollDir = HitTestScrollArrow(absPos);
		if (scrollDir != 0)
		{
			m_ScrollX = std::clamp(m_ScrollX + scrollDir * 64.0f, 0.0f, m_MaxScrollX);
			return Reply::Handled_();
		}
		int tab = HitTestTabs(absPos);
		if (tab >= 0)
		{
			if (HitTestClose(absPos, tab))
			{
				if (tab < (int)m_Tabs.size() && m_DockSpace)
					m_DockSpace->ClosePanel(m_Tabs[tab].Id);
				return Reply::Handled_();
			}
			m_PressedTab = tab;
			if (tab < (int)m_Tabs.size() && m_DockSpace)
			{
				dock::NodeId well = m_NodeId;
				dock::PanelId id = m_Tabs[tab].Id;
				m_DockSpace->ActivateTab(well, tab);
				// Value captures only — the factory runs at drag-threshold time,
				// possibly after a rebuild destroyed this TabWell.
				return Reply::Handled_().CaptureMouse().DetectDrag([space = m_DockSpace, well, id]() {
					return space->CreateTabDragPayload(well, id);
				});
			}
			return Reply::Handled_().CaptureMouse();
		}
		return Reply::Unhandled();
	}
	if (button == 1)
	{
		// Right-click: tab context menu.
		int tab = HitTestTabs(absPos);
		if (tab >= 0 && tab < (int)m_Tabs.size() && m_DockSpace)
		{
			m_DockSpace->ShowTabContextMenu(this, tab, absPos);
			return Reply::Handled_();
		}
		return Reply::Unhandled();
	}
	if (button == 2)
	{
		int tab = HitTestTabs(absPos);
		if (tab >= 0 && tab < (int)m_Tabs.size() && m_DockSpace)
		{
			m_DockSpace->ClosePanel(m_Tabs[tab].Id);
			return Reply::Handled_();
		}
	}
	return Reply::Unhandled();
}

inline Reply TabWell::OnMouseButtonUp(const glm::vec2&, int button)
{
	if (button != 0) return Reply::Unhandled();
	if (m_PressedTab < 0) return Reply::Unhandled();
	m_PressedTab = -1;
	return Reply::Handled_().ReleaseCapture();
}

// ---- FloatingPanel methods that need the complete DockSpace type -----------

inline Reply FloatingPanel::OnMouseButtonDown(const glm::vec2& absPos, int button)
{
	if (button != 0) return Reply::Unhandled();
	if (IsCloseArea(absPos))
	{
		if (m_DockSpace)
			m_DockSpace->CloseFloating(m_FloatingId);
		return Reply::Handled_();
	}
	if (IsResizeGrip(absPos))
	{
		m_PressMode = 2;
		m_PressStart = absPos;
		m_ResizeStart = { m_LastAbsRect.z, m_LastAbsRect.w };
		if (m_DockSpace)
			m_DockSpace->BringFloatingToFrontPublic(m_FloatingId);
		return Reply::Handled_().CaptureMouse();
	}
	if (IsTitleArea(absPos))
	{
		m_PressMode = 1;
		m_PressStart = absPos;
		m_ModelPosStart = { m_LastAbsRect.x, m_LastAbsRect.y };
		if (m_DockSpace)
			m_DockSpace->BringFloatingToFrontPublic(m_FloatingId);
		if (m_DockSpace)
		{
			DockSpace* space = m_DockSpace;
			dock::NodeId fid = m_FloatingId;
			dock::PanelId panel = m_DockSpace->FloatingActivePanel(fid);
			return Reply::Handled_().CaptureMouse().DetectDrag([space, fid, panel]() {
				return space->CreateFloatingDragPayload(fid, panel);
			});
		}
		return Reply::Handled_().CaptureMouse();
	}
	return Reply::Unhandled();
}

inline Reply FloatingPanel::OnMouseMove(const glm::vec2& absPos)
{
	if (m_PressMode == 1)
	{
		m_DragDelta = absPos - m_PressStart;
		InvalidateLayout();
		return Reply::Handled_();
	}
	if (m_PressMode == 2)
	{
		glm::vec2 delta = absPos - m_PressStart;
		glm::vec2 s = glm::max(m_ResizeStart + delta, glm::vec2(120.0f, 90.0f));
		m_DragDelta = s - m_ResizeStart;
		InvalidateLayout();
		return Reply::Handled_();
	}
	return Reply::Unhandled();
}

inline Reply FloatingPanel::OnMouseButtonUp(const glm::vec2&, int button)
{
	if (button != 0) return Reply::Unhandled();
	if (m_PressMode == 0) return Reply::Unhandled();

	int mode = m_PressMode;
	glm::vec2 delta = m_DragDelta;
	glm::vec2 modelStart = m_ModelPosStart;
	glm::vec2 resizeStart = m_ResizeStart;
	ResetGestureState();

	if (m_DockSpace)
	{
		if (mode == 1 && (delta.x != 0.0f || delta.y != 0.0f))
			m_DockSpace->MoveFloatingPanel(m_FloatingId, modelStart + delta);
		else if (mode == 2)
			m_DockSpace->ResizeFloatingPanel(m_FloatingId, resizeStart + delta);
	}
	return Reply::Handled_().ReleaseCapture();
}

} // namespace wisp
