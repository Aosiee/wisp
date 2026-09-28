#pragma once

#include <glm/glm.hpp>
#include <algorithm>
#include <cassert>
#include <cmath>
#include <cstdint>
#include <memory>
#include <functional>
#include <optional>
#include <set>
#include <string>
#include <vector>

#include <rapidjson/document.h>
#include <rapidjson/prettywriter.h>
#include <rapidjson/stringbuffer.h>

namespace wisp { namespace dock {

using PanelId = std::string;
using NodeId  = uint32_t;

enum class Axis { Horizontal, Vertical };
enum class Zone { Center, North, South, East, West };
enum class Edge { North, South, East, West };

constexpr float kRatioEpsilon = 1e-4f;

// ---- Node -------------------------------------------------------------------

struct TabEntry
{
	PanelId Id;
	bool operator==(const TabEntry& o) const { return Id == o.Id; }
};

struct DockNode
{
	NodeId Id = 0;

	enum class Kind { Split, TabWell, Floating } Kind = Kind::TabWell;

	// Split
	Axis                   SplitAxis = Axis::Horizontal;
	std::vector<DockNode>  Children;
	std::vector<float>     Ratios;   // parallel to Children; sums to ~1

	// TabWell
	std::vector<TabEntry>  Tabs;
	int                    ActiveIndex = 0;

	// Floating
	glm::vec2 FloatPos{ 0, 0 };
	glm::vec2 FloatSize{ 400, 300 };
	std::unique_ptr<DockNode> FloatChild; // only meaningful when Kind == Floating

	bool operator==(const DockNode& o) const
	{
		if (Id != o.Id || Kind != o.Kind) return false;
		if (Kind == Kind::Split)
			return SplitAxis == o.SplitAxis && Children == o.Children && Ratios == o.Ratios;
		if (Kind == Kind::TabWell)
			return Tabs == o.Tabs && ActiveIndex == o.ActiveIndex;
		return FloatPos == o.FloatPos && FloatSize == o.FloatSize && (!!FloatChild == !!o.FloatChild) && (!FloatChild || *FloatChild == *o.FloatChild);
	}
	bool operator!=(const DockNode& o) const { return !(*this == o); }
};

// ---- Layout -----------------------------------------------------------------

// One OS window's share of the dock layout: the window's dock tree, its
// fake floating panels, its restored size, and - only where window
// placement exists at all - its restored position. `Pos` is optional on
// purpose: on Wayland it is never written and never read; a layout saved
// on Windows and loaded on Hyprland just ignores it (tiling WMs re-place
// every launch).
struct WindowNode
{
	NodeId   Id = 0;
	bool     bMain = false;             // exactly one true across the layout
	glm::vec2 Size{ 1280.0f, 720.0f };  // content size; restored on open
	std::optional<glm::vec2> Pos;       // valid only when the platform can place windows
	DockNode Main;                      // the window's dock tree
	std::vector<DockNode> Floating;     // per-window fake floating panels

	bool operator==(const WindowNode& o) const
	{
		if (Id != o.Id || bMain != o.bMain) return false;
		if (glm::length(Size - o.Size) > 1e-4f) return false;
		if (Pos.has_value() != o.Pos.has_value()) return false;
		if (Pos && glm::length(*Pos - *o.Pos) > 1e-4f) return false;
		return Main == o.Main && Floating == o.Floating;
	}
	bool operator!=(const WindowNode& o) const { return !(*this == o); }
};

struct DockLayout
{
	// Windows[0] is the main window by convention; `bMain` is the truth
	// (invariant 10: exactly one, demoted duplicates on normalize).
	std::vector<WindowNode> Windows;
	uint32_t               NextNodeId = 1; // global - NodeIds unique across ALL windows
	uint64_t               Version = 0;

	// A default-constructed layout is a *valid* layout: it owns its single
	// main window whose tree is the root placeholder well (invariant 8).
	// Invariant 10 is therefore a property of the type, not of construction
	// order - no code path can observe a windowless DockLayout.
	DockLayout()
	{
		WindowNode wn;
		wn.Id = NextNodeId++;
		wn.bMain = true;
		wn.Main.Kind = DockNode::Kind::TabWell;
		wn.Main.Id = NextNodeId++;
		wn.Main.ActiveIndex = 0;
		Windows.push_back(std::move(wn));
	}

	// The single main window. Cheap scan, never cached (Windows can grow
	// and shrink; callers must not hold the reference across a transform).
	WindowNode& MainWindow()
	{
		for (WindowNode& w : Windows)
			if (w.bMain) return w;
		assert(false && "DockLayout has no main window!");
		return Windows[0];
	}
	const WindowNode& MainWindow() const
	{
		for (const WindowNode& w : Windows)
			if (w.bMain) return w;
		assert(false && "DockLayout has no main window!");
		return Windows[0];
	}

	bool operator==(const DockLayout& o) const { return Windows == o.Windows; }
	bool operator!=(const DockLayout& o) const { return !(*this == o); }
};

inline DockNode CloneNode(const DockNode& n)
{
	DockNode c;
	c.Id = n.Id;
	c.Kind = n.Kind;
	c.SplitAxis = n.SplitAxis;
	for (const auto& ch : n.Children) c.Children.push_back(CloneNode(ch));
	c.Ratios = n.Ratios;
	c.Tabs = n.Tabs;
	c.ActiveIndex = n.ActiveIndex;
	c.FloatPos = n.FloatPos;
	c.FloatSize = n.FloatSize;
	if (n.FloatChild) c.FloatChild = std::make_unique<DockNode>(CloneNode(*n.FloatChild));
	return c;
}

inline DockLayout CloneLayout(const DockLayout& l)
{
	DockLayout c;
	c.Windows.clear();
	for (const WindowNode& w : l.Windows)
	{
		WindowNode wc;
		wc.Id = w.Id;
		wc.bMain = w.bMain;
		wc.Size = w.Size;
		wc.Pos = w.Pos;
		wc.Main = CloneNode(w.Main);
		for (const auto& f : w.Floating) wc.Floating.push_back(CloneNode(f));
		c.Windows.push_back(std::move(wc));
	}
	c.NextNodeId = l.NextNodeId;
	c.Version = l.Version;
	return c;
}

// ---- Drop vocabulary --------------------------------------------------------

struct DropTarget
{
	enum class Kind { WellZone, RootEdge, Miss };
	Kind      KindId = Kind::Miss;
	NodeId    Well = 0;              // WellZone
	Zone      ZoneId = Zone::Center; // WellZone
	Edge      RootEdge = Edge::East; // RootEdge
	glm::vec2 MissPos{ 0, 0 };       // Miss (cursor pos, floating placement)
};

struct DropPayload
{
	PanelId Panel;
	NodeId  SourceWell = 0;      // 0 = not from a well
	NodeId  SourceFloating = 0;  // 0 = not from a floating panel
	bool    Singleton = false;
};

// ---- Lookup helpers ----------------------------------------------------------

inline DockNode* FindNode(DockNode& n, NodeId id)
{
	if (n.Id == id) return &n;
	for (auto& c : n.Children)
		if (DockNode* f = FindNode(c, id)) return f;
	if (n.Kind == DockNode::Kind::Floating && n.FloatChild)
		if (DockNode* f = FindNode(*n.FloatChild, id)) return f;
	return nullptr;
}
inline const DockNode* FindNode(const DockNode& n, NodeId id)
{
	if (n.Id == id) return &n;
	for (const auto& c : n.Children)
		if (const DockNode* f = FindNode(c, id)) return f;
	if (n.Kind == DockNode::Kind::Floating && n.FloatChild)
		if (const DockNode* f = FindNode(*n.FloatChild, id)) return f;
	return nullptr;
}
// ---- Window-aware lookup (NodeIds are globally unique across all windows) --

inline WindowNode* FindWindowContainingNode(DockLayout& l, NodeId id)
{
	std::function<WindowNode*()> find = [&]() -> WindowNode* {
		for (WindowNode& w : l.Windows)
		{
			if (FindNode(w.Main, id)) return &w;
			for (auto& f : w.Floating)
				if (FindNode(f, id)) return &w;
			if (w.Id == id) return &w;
		}
		return nullptr;
	};
	return find();
}
inline const WindowNode* FindWindowContainingNode(const DockLayout& l, NodeId id)
{
	for (const WindowNode& w : l.Windows)
	{
		if (FindNode(w.Main, id)) return &w;
		for (const auto& f : w.Floating)
			if (FindNode(f, id)) return &w;
		if (w.Id == id) return &w;
	}
	return nullptr;
}

inline DockNode* FindNode(DockLayout& l, NodeId id)
{
	for (WindowNode& w : l.Windows)
	{
		if (DockNode* f = FindNode(w.Main, id)) return f;
		for (auto& fl : w.Floating)
			if (DockNode* found = FindNode(fl, id)) return found;
	}
	return nullptr;
}
inline const DockNode* FindNode(const DockLayout& l, NodeId id)
{
	for (const WindowNode& w : l.Windows)
	{
		if (const DockNode* f = FindNode(w.Main, id)) return f;
		for (const auto& fl : w.Floating)
			if (const DockNode* found = FindNode(fl, id)) return found;
	}
	return nullptr;
}

// Parent node of `id` plus child index (Floating child = -1). Returns nullptr
// when `id` is the root node of `n` itself or not found.
inline DockNode* FindParentSlot(DockNode& n, NodeId id, int* outIndex)
{
	for (int i = 0; i < (int)n.Children.size(); i++)
	{
		if (n.Children[i].Id == id) { *outIndex = i; return &n; }
		DockNode* f = FindParentSlot(n.Children[i], id, outIndex);
		if (f) return f;
	}
	if (n.Kind == DockNode::Kind::Floating && n.FloatChild)
	{
		if (n.FloatChild->Id == id) { *outIndex = -1; return &n; }
		return FindParentSlot(*n.FloatChild, id, outIndex);
	}
	return nullptr;
}

inline DockNode* FindParentSlotAny(DockLayout& l, NodeId id, int* outIndex)
{
	for (WindowNode& w : l.Windows)
	{
		if (DockNode* p = FindParentSlot(w.Main, id, outIndex)) return p;
		for (auto& f : w.Floating)
			if (DockNode* p = FindParentSlot(f, id, outIndex)) return p;
	}
	return nullptr;
}

// First TabWell in depth-first order (default-well rule).
inline DockNode* FindFirstWell(DockNode& n)
{
	if (n.Kind == DockNode::Kind::TabWell) return &n;
	if (n.Kind == DockNode::Kind::Split)
		for (auto& c : n.Children)
			if (DockNode* w = FindFirstWell(c)) return w;
	if (n.Kind == DockNode::Kind::Floating && n.FloatChild) return FindFirstWell(*n.FloatChild);
	return nullptr;
}

inline int FindTabIndex(const DockNode& well, const PanelId& id)
{
	for (int i = 0; i < (int)well.Tabs.size(); i++)
		if (well.Tabs[i].Id == id) return i;
	return -1;
}

// First well containing a given panel id anywhere in the layout (any window).
inline NodeId FindWellContaining(const DockLayout& l, const PanelId& id)
{
	std::function<NodeId(const DockNode&)> search = [&](const DockNode& n) -> NodeId {
		if (n.Kind == DockNode::Kind::TabWell)
			return FindTabIndex(n, id) >= 0 ? n.Id : 0;
		if (n.Kind == DockNode::Kind::Split)
		{
			for (const auto& c : n.Children)
			{
				NodeId f = search(c);
				if (f) return f;
			}
		}
		else if (n.FloatChild)
			return search(*n.FloatChild);
		return 0;
	};
	for (const WindowNode& w : l.Windows)
	{
		NodeId found = search(w.Main);
		if (found) return found;
		for (const auto& f : w.Floating)
		{
			found = search(f);
			if (found) return found;
		}
	}
	return 0;
}

inline DockNode* FindFloatingNode(DockLayout& l, NodeId id)
{
	for (WindowNode& w : l.Windows)
		for (auto& f : w.Floating)
			if (f.Id == id) return &f;
	return nullptr;
}
inline const DockNode* FindFloatingNode(const DockLayout& l, NodeId id)
{
	for (const WindowNode& w : l.Windows)
		for (const auto& f : w.Floating)
			if (f.Id == id) return &f;
	return nullptr;
}

// ---- Node construction -------------------------------------------------------

inline DockNode MakeTabWell(DockLayout& l, const std::vector<TabEntry>& tabs, int active)
{
	DockNode n;
	n.Id = l.NextNodeId++;
	n.Kind = DockNode::Kind::TabWell;
	n.Tabs = tabs;
	n.ActiveIndex = active;
	return n;
}

inline DockNode MakeFloating(DockLayout& l, const glm::vec2& pos, const glm::vec2& size, DockNode child)
{
	DockNode n;
	n.Id = l.NextNodeId++;
	n.Kind = DockNode::Kind::Floating;
	n.FloatPos = pos;
	n.FloatSize = size;
	n.FloatChild = std::make_unique<DockNode>(std::move(child));
	return n;
}

inline DockNode MakeSplit(DockLayout& l, Axis axis, std::vector<DockNode> children, std::vector<float> ratios)
{
	DockNode n;
	n.Id = l.NextNodeId++;
	n.Kind = DockNode::Kind::Split;
	n.SplitAxis = axis;
	n.Children = std::move(children);
	n.Ratios = std::move(ratios);
	return n;
}

// Two-child split builder for move-only children (initializer-list vectors
// require copyable elements).
inline DockNode MakeSplit2(DockLayout& l, Axis axis, bool firstA, DockNode a, DockNode b, float ratioA, float ratioB)
{
	DockNode n;
	n.Id = l.NextNodeId++;
	n.Kind = DockNode::Kind::Split;
	n.SplitAxis = axis;
	if (firstA)
	{
		n.Children.push_back(std::move(a));
		n.Children.push_back(std::move(b));
		n.Ratios = { ratioA, ratioB };
	}
	else
	{
		n.Children.push_back(std::move(b));
		n.Children.push_back(std::move(a));
		n.Ratios = { ratioB, ratioA };
	}
	return n;
}

// ---- Normalization -----------------------------------------------------------

inline void Renormalize(std::vector<float>& ratios)
{
	float sum = 0.0f;
	for (float r : ratios) sum += std::max(0.0f, r);
	if (sum <= 0.0f)
	{
		if (ratios.empty()) return;
		float v = 1.0f / (float)ratios.size();
		for (auto& r : ratios) r = v;
		return;
	}
	for (auto& r : ratios) r = std::max(0.0f, r) / sum;
}

// Normalize a node subtree in place: prune empty TabWells, collapse
// single-child splits, clamp ActiveIndex. Returns true when the subtree
// contains no tabs at all (fully degenerate) and should be removed by the
// caller. Survivor NodeIds are preserved; removed nodes' ids are freed.
inline bool NormalizeNode(DockNode& n)
{
	switch (n.Kind)
	{
	case DockNode::Kind::TabWell:
		if (n.ActiveIndex < 0) n.ActiveIndex = 0;
		if (n.ActiveIndex >= (int)n.Tabs.size()) n.ActiveIndex = std::max(0, (int)n.Tabs.size() - 1);
		return n.Tabs.empty();
	case DockNode::Kind::Floating:
		if (!n.FloatChild || NormalizeNode(*n.FloatChild)) return true; // empty floating panel: remove entirely
		return false;
	case DockNode::Kind::Split:
		break;
	}

	std::vector<DockNode> kept;
	std::vector<float> keptRatios;
	for (int i = 0; i < (int)n.Children.size(); i++)
	{
		bool degenerate = NormalizeNode(n.Children[i]);
		if (!degenerate)
		{
			kept.push_back(std::move(n.Children[i]));
			keptRatios.push_back(i < (int)n.Ratios.size() ? n.Ratios[i] : 1.0f);
		}
	}
	n.Children = std::move(kept);
	n.Ratios = std::move(keptRatios);

	if (n.Children.empty()) return true;      // whole split had no tabs
	if (n.Children.size() == 1)
	{
		DockNode survivor = std::move(n.Children[0]);
		n = std::move(survivor);              // survivor replaces the split (its id survives)
		return false;
	}
	Renormalize(n.Ratios);
	return false;
}

// Merge same-axis child splits into their parent (invariant 3).
inline void FlattenSameAxis(DockNode& n)
{
	if (n.Kind == DockNode::Kind::TabWell) return;
	if (n.Kind == DockNode::Kind::Floating) { if (n.FloatChild) FlattenSameAxis(*n.FloatChild); return; }

	for (auto& c : n.Children) FlattenSameAxis(c);

	bool any = false;
	for (const auto& c : n.Children)
		if (c.Kind == DockNode::Kind::Split && c.SplitAxis == n.SplitAxis) { any = true; break; }
	if (!any) return;

	std::vector<DockNode> flat;
	std::vector<float> flatRatios;
	for (int i = 0; i < (int)n.Children.size(); i++)
	{
		DockNode& c = n.Children[i];
		float share = i < (int)n.Ratios.size() ? n.Ratios[i] : 1.0f;
		if (c.Kind == DockNode::Kind::Split && c.SplitAxis == n.SplitAxis)
		{
			for (int j = 0; j < (int)c.Children.size(); j++)
			{
				float sub = j < (int)c.Ratios.size() ? c.Ratios[j] : 1.0f;
				flat.push_back(std::move(c.Children[j]));
				flatRatios.push_back(share * sub);
			}
		}
		else
		{
			flat.push_back(std::move(c));
			flatRatios.push_back(share);
		}
	}
	n.Children = std::move(flat);
	n.Ratios = std::move(flatRatios);
	Renormalize(n.Ratios);
}

inline void NormalizeLayout(DockLayout& l)
{
	// Per-window normalization: prune each window's tree; enforce invariant
	// 11 (a non-main window whose tree normalizes to nothing is removed -
	// its OS window closes on the next projection). The root-placeholder
	// rule (invariant 8) applies to the main window only.
	for (size_t wi = 0; wi < l.Windows.size();)
	{
		WindowNode& w = l.Windows[wi];
		NodeId rootId = w.Main.Id;

		for (auto it = w.Floating.begin(); it != w.Floating.end();)
		{
			if (it->Kind != DockNode::Kind::Floating || NormalizeNode(*it))
				it = w.Floating.erase(it);
			else
			{
				FlattenSameAxis(*it);
				++it;
			}
		}

		bool degenerate = NormalizeNode(w.Main);
		FlattenSameAxis(w.Main);

		if (degenerate || (w.Main.Kind == DockNode::Kind::TabWell && w.Main.Tabs.empty()))
		{
			w.Main = DockNode();
			w.Main.Id = rootId;
			w.Main.Kind = DockNode::Kind::TabWell;
			w.Main.ActiveIndex = 0;
		}

		if (!w.bMain && w.Main.Kind == DockNode::Kind::TabWell && w.Main.Tabs.empty() && w.Floating.empty())
		{
			l.Windows.erase(l.Windows.begin() + wi);
			continue;
		}
		++wi;
	}

	// Invariant 10: exactly one bMain. Zero (hand-built layouts) promotes
	// the first window; duplicates demote all but the first.
	int mains = 0;
	for (WindowNode& w : l.Windows)
	{
		if (w.bMain) mains++;
	}
	if (mains != 1)
	{
		bool promoted = false;
		for (WindowNode& w : l.Windows)
		{
			if (mains == 0 && !promoted) { w.bMain = true; promoted = true; continue; }
			if (mains > 1 && w.bMain && !promoted) { promoted = true; continue; }
			w.bMain = false;
		}
	}
}

// ---- Invariant validation (tests + post-load) --------------------------------

struct InvariantViolation { std::string What; };

inline void CollectWellPaths(const DockNode& n, const std::vector<NodeId>& path,
	std::vector<std::vector<NodeId>>& out)
{
	if (n.Kind == DockNode::Kind::TabWell)
		out.push_back(path);
	else if (n.Kind == DockNode::Kind::Split)
	{
		for (const auto& c : n.Children)
		{
			auto p = path;
			p.push_back(c.Id);
			CollectWellPaths(c, p, out);
		}
	}
	else if (n.Kind == DockNode::Kind::Floating && n.FloatChild)
	{
		auto p = path;
		p.push_back(n.FloatChild->Id);
		CollectWellPaths(*n.FloatChild, p, out);
	}
}

inline bool ValidateLayout(const DockLayout& l, const std::set<PanelId>& singletons,
	std::vector<InvariantViolation>& violations)
{
	violations.clear();

	if (l.Windows.empty()) { violations.push_back({ "no windows" }); return false; }

	// Invariant 10: exactly one bMain window.
	int mains = 0;
	for (const WindowNode& w : l.Windows)
		if (w.bMain) mains++;
	if (mains != 1) violations.push_back({ "exactly one bMain window required" });

	std::vector<std::vector<NodeId>> wells;
	for (const WindowNode& w : l.Windows)
	{
		CollectWellPaths(w.Main, { w.Main.Id }, wells);
		for (const auto& f : w.Floating)
		{
			auto p = std::vector<NodeId>{ f.Id };
			CollectWellPaths(f, p, wells);
		}
	}

	for (const auto& path : wells)
	{
		const DockNode* w = FindNode(l, path.back());
		if (!w) { violations.push_back({ "dangling well path" }); continue; }
		if (w->Kind != DockNode::Kind::TabWell) { violations.push_back({ "well path ends on non-well" }); continue; }
		if (w->Tabs.empty())
		{
			// The root placeholder rule (invariant 8) applies to any KEPT
			// window's root: the main window's root is required to survive
			// being emptied (invariant 10 - the main window is never removed),
			// and a non-main window's root can be empty while the window
			// still hosts floating panels (it is removed only when its tree
			// normalizes to *nothing* - invariant 11).
			const WindowNode* owner = FindWindowContainingNode(l, path.front());
			bool isWindowRoot = owner && path.size() == 1 && path[0] == owner->Main.Id;
			if (!isWindowRoot)
				violations.push_back({ "empty TabWell that is not a window's root placeholder" });
		}
		else if (w->ActiveIndex < 0 || w->ActiveIndex >= (int)w->Tabs.size())
			violations.push_back({ "ActiveIndex out of range" });
	}

	for (const WindowNode& w : l.Windows)
	{
		for (const auto& f : w.Floating)
			if (f.Kind != DockNode::Kind::Floating)
				violations.push_back({ "non-Floating node in Floating list" });
	}

	// Invariant 12: singleton panels unique across ALL windows (extends
	// invariant 6, which only knew one window).
	if (!singletons.empty())
	{
		std::set<PanelId> seen;
		for (const auto& path : wells)
		{
			const DockNode* w = FindNode(l, path.back());
			for (const auto& t : w->Tabs)
			{
				if (singletons.count(t.Id))
				{
					if (seen.count(t.Id)) violations.push_back({ "duplicate singleton panel" });
					seen.insert(t.Id);
				}
			}
		}
	}

	std::set<NodeId> ids;
	std::function<void(const DockNode&)> collectIds = [&](const DockNode& n) {
		if (n.Id == 0) violations.push_back({ "NodeId 0 in use" });
		if (ids.count(n.Id)) violations.push_back({ "duplicate NodeId" });
		ids.insert(n.Id);
		for (const auto& c : n.Children) collectIds(c);
		if (n.Kind == DockNode::Kind::Floating && n.FloatChild) collectIds(*n.FloatChild);
	};
	for (const WindowNode& w : l.Windows)
	{
		if (w.Id == 0) violations.push_back({ "WindowNode id 0 in use" });
		if (ids.count(w.Id)) violations.push_back({ "duplicate WindowNode id" });
		ids.insert(w.Id);
		collectIds(w.Main);
		for (const auto& f : w.Floating) collectIds(f);
	}

	return violations.empty();
}

// ---- Transforms --------------------------------------------------------------

inline void Bump(DockLayout& l) { l.Version++; }

// Remove every occurrence of a panel id anywhere in a subtree (singleton rule).
// Returns true if anything was removed.
inline bool RemovePanelRecursive(DockNode& n, const PanelId& id, NodeId exceptWell)
{
	if (n.Kind == DockNode::Kind::TabWell)
	{
		if (n.Id == exceptWell) return false;
		for (int i = 0; i < (int)n.Tabs.size(); i++)
		{
			if (n.Tabs[i].Id == id)
			{
				n.Tabs.erase(n.Tabs.begin() + i);
				if (n.ActiveIndex >= (int)n.Tabs.size())
					n.ActiveIndex = std::max(0, (int)n.Tabs.size() - 1);
				return true;
			}
		}
		return false;
	}
	if (n.Kind == DockNode::Kind::Split)
	{
		for (auto& c : n.Children)
			if (RemovePanelRecursive(c, id, exceptWell)) return true;
		return false;
	}
	return n.FloatChild && RemovePanelRecursive(*n.FloatChild, id, exceptWell);
}

// Remove every occurrence of a panel id across ALL windows (singleton rule,
// invariant 12 - uniqueness is layout-global, not per window).
inline void RemovePanelEverywhere(DockLayout& l, const PanelId& id, NodeId exceptWell)
{
	for (WindowNode& w : l.Windows)
	{
		for (auto& f : w.Floating)
			while (RemovePanelRecursive(f, id, exceptWell)) {}
		while (RemovePanelRecursive(w.Main, id, exceptWell)) {}
	}
}

inline bool InsertTab(DockLayout& l, NodeId wellId, const PanelId& id, int index = -1, bool singleton = false)
{
	DockNode* well = FindNode(l, wellId);
	if (!well || well->Kind != DockNode::Kind::TabWell) return false;

	if (singleton)
		RemovePanelEverywhere(l, id, 0);

	TabEntry e{ id };
	if (index < 0 || index > (int)well->Tabs.size())
		well->Tabs.push_back(e);
	else
		well->Tabs.insert(well->Tabs.begin() + index, e);
	if (well->Tabs.size() == 1) well->ActiveIndex = 0;

	NormalizeLayout(l);
	Bump(l);
	return true;
}

inline bool RemoveTab(DockLayout& l, NodeId wellId, int tabIndex)
{
	DockNode* well = FindNode(l, wellId);
	if (!well || well->Kind != DockNode::Kind::TabWell) return false;
	if (tabIndex < 0 || tabIndex >= (int)well->Tabs.size()) return false;
	well->Tabs.erase(well->Tabs.begin() + tabIndex);
	if (well->ActiveIndex >= (int)well->Tabs.size())
		well->ActiveIndex = std::max(0, (int)well->Tabs.size() - 1);
	NormalizeLayout(l);
	Bump(l);
	return true;
}

// SplitWell — ratio semantics: sibling-insert on same-axis parents,
// wrap on cross-axis, root split at the top.
inline bool SplitWell(DockLayout& l, NodeId wellId, Edge edge, const PanelId& id,
	float ratio = 0.5f, bool singleton = false)
{
	DockNode* well = FindNode(l, wellId);
	if (!well || well->Kind != DockNode::Kind::TabWell) return false;
	ratio = std::clamp(ratio, 0.05f, 0.95f);
	Axis splitAxis = (edge == Edge::North || edge == Edge::South) ? Axis::Vertical : Axis::Horizontal;
	bool newFirst = (edge == Edge::North || edge == Edge::West);

	DockNode newWell = MakeTabWell(l, { TabEntry{ id } }, 0);

	WindowNode* owner = FindWindowContainingNode(l, wellId);
	if (!owner) return false;

	if (owner->Main.Id == wellId)
	{
		DockNode oldWell = std::move(owner->Main);
		owner->Main = MakeSplit2(l, splitAxis, newFirst, std::move(newWell), std::move(oldWell), ratio, 1.0f - ratio);
	}
	else
	{
		int idx = -1;
		DockNode* parent = FindParentSlotAny(l, wellId, &idx);
		if (!parent)
		{
			// Not found under any parent — abort.
			return false;
		}

		if (idx >= 0 && parent->Kind == DockNode::Kind::Split && parent->SplitAxis == splitAxis)
		{
			// Sibling insert on the edge side; only the target's share splits.
			float targetShare = parent->Ratios[idx];
			int insertAt = newFirst ? idx : idx + 1;
			parent->Children.insert(parent->Children.begin() + insertAt, std::move(newWell));
			parent->Ratios.insert(parent->Ratios.begin() + insertAt, ratio * targetShare);
			int targetNow = (insertAt == idx) ? idx + 1 : idx;
			parent->Ratios[targetNow] = (1.0f - ratio) * targetShare;
			Renormalize(parent->Ratios);
		}
		else if (idx >= 0 && parent->Kind == DockNode::Kind::Split)
		{
			// Cross-axis: wrap the target in a new split inheriting its share.
			float targetShare = parent->Ratios[idx];
			DockNode oldWell = std::move(parent->Children[idx]);
			DockNode wrap = MakeSplit2(l, splitAxis, newFirst, std::move(newWell), std::move(oldWell), ratio, 1.0f - ratio);
			parent->Children[idx] = std::move(wrap);
			parent->Ratios[idx] = targetShare;
		}
		else if (idx == -1 && parent->Kind == DockNode::Kind::Floating)
		{
			DockNode oldWell = std::move(*parent->FloatChild);
			parent->FloatChild = std::make_unique<DockNode>(MakeSplit2(l, splitAxis, newFirst, std::move(newWell), std::move(oldWell), ratio, 1.0f - ratio));
		}
		else
			return false;
	}

	if (singleton)
	{
		// Remove the singleton from everywhere except the new well it just
		// landed in (including the split target well, which may have held it).
		RemovePanelEverywhere(l, id, newWell.Id);
	}

	NormalizeLayout(l);
	Bump(l);
	return true;
}

// FloatPanel — moves a tab out of a well into a new floating panel inside
// the same window that owns the well.
inline bool FloatPanel(DockLayout& l, NodeId wellId, int tabIndex, const glm::vec2& pos,
	const glm::vec2& size = { 400, 300 })
{
	DockNode* well = FindNode(l, wellId);
	if (!well || well->Kind != DockNode::Kind::TabWell) return false;
	WindowNode* owner = FindWindowContainingNode(l, wellId);
	if (!owner) return false;
	if (tabIndex < 0 || tabIndex >= (int)well->Tabs.size()) return false;

	TabEntry moved = well->Tabs[tabIndex];
	well->Tabs.erase(well->Tabs.begin() + tabIndex);
	if (well->ActiveIndex >= (int)well->Tabs.size())
		well->ActiveIndex = std::max(0, (int)well->Tabs.size() - 1);

	DockNode content = MakeTabWell(l, { moved }, 0);
	owner->Floating.push_back(MakeFloating(l, pos, size, std::move(content)));
	NormalizeLayout(l);
	Bump(l);
	return true;
}

inline bool SetActiveTab(DockLayout& l, NodeId wellId, int index)
{
	DockNode* well = FindNode(l, wellId);
	if (!well || well->Kind != DockNode::Kind::TabWell) return false;
	if (index < 0 || index >= (int)well->Tabs.size()) return false;
	if (well->ActiveIndex == index) return true; // no-op — no rebuild churn
	well->ActiveIndex = index;
	Bump(l);
	return true;
}

// Two-pane semantics (matches a divider drag between childIndex and
// childIndex+1): childIndex gets `ratio` of their combined share, the
// neighbour gets the rest, all other siblings untouched.
inline bool SetSplitRatio(DockLayout& l, NodeId splitId, int childIndex, float ratio)
{
	DockNode* split = FindNode(l, splitId);
	if (!split || split->Kind != DockNode::Kind::Split) return false;
	if (childIndex < 0 || childIndex + 1 >= (int)split->Children.size()) return false;
	ratio = std::clamp(ratio, 0.001f, 0.999f);

	float a = split->Ratios[childIndex];
	float b = split->Ratios[childIndex + 1];
	float combined = a + b;
	if (combined <= 1e-6f) return false;
	split->Ratios[childIndex] = combined * ratio;
	split->Ratios[childIndex + 1] = combined * (1.0f - ratio);
	Renormalize(split->Ratios);
	Bump(l);
	return true;
}

// MovePanel — transactional Remove+Insert; both endpoints resolved up front;
// same-well moves are reorders. Re-resolves the destination after removal in
// case normalization restructured the tree above it.
inline bool MovePanel(DockLayout& l, NodeId fromWellId, int tabIndex, NodeId toWellId, int toIndex = -1)
{
	DockNode* from = FindNode(l, fromWellId);
	DockNode* to = FindNode(l, toWellId);
	if (!from || !to || from->Kind != DockNode::Kind::TabWell || to->Kind != DockNode::Kind::TabWell)
		return false;
	if (tabIndex < 0 || tabIndex >= (int)from->Tabs.size()) return false;

	TabEntry moved = from->Tabs[tabIndex];

	if (from->Id == to->Id)
	{
		// Reorder within the same well.
		if (toIndex < 0) toIndex = (int)from->Tabs.size() - 1;
		from->Tabs.erase(from->Tabs.begin() + tabIndex);
		if (toIndex > (int)from->Tabs.size()) toIndex = (int)from->Tabs.size();
		from->Tabs.insert(from->Tabs.begin() + toIndex, moved);
		if (from->ActiveIndex >= (int)from->Tabs.size())
			from->ActiveIndex = std::max(0, (int)from->Tabs.size() - 1);
		Bump(l);
		return true;
	}

	from->Tabs.erase(from->Tabs.begin() + tabIndex);
	if (from->ActiveIndex >= (int)from->Tabs.size())
		from->ActiveIndex = std::max(0, (int)from->Tabs.size() - 1);

	// The source removal can collapse its well and restructure the tree above
	// the destination — re-resolve by id.
	DockNode* toNow = FindNode(l, toWellId);
	if (!toNow || toNow->Kind != DockNode::Kind::TabWell)
	{
		// Destination well destroyed by normalization (shouldn't happen for a
		// *different* well, but guard cheaply): restore the tab and abort.
		DockNode* fallback = nullptr;
		for (WindowNode& w : l.Windows)
		{
			if ((fallback = FindFirstWell(w.Main))) break;
			for (auto& f : w.Floating)
				if ((fallback = FindFirstWell(f))) break;
			if (fallback) break;
		}
		if (!fallback)
		{
			from->Tabs.insert(from->Tabs.begin() + tabIndex, moved);
			NormalizeLayout(l);
			Bump(l);
			return false;
		}
		toNow = fallback;
	}

	if (toIndex < 0 || toIndex > (int)toNow->Tabs.size())
		toIndex = (int)toNow->Tabs.size();
	toNow->Tabs.insert(toNow->Tabs.begin() + toIndex, moved);
	if (toNow->ActiveIndex >= (int)toNow->Tabs.size())
		toNow->ActiveIndex = std::max(0, (int)toNow->Tabs.size() - 1);
	NormalizeLayout(l);
	Bump(l);
	return true;
}

inline bool MoveFloating(DockLayout& l, NodeId floatingId, const glm::vec2& pos)
{
	if (DockNode* f = FindFloatingNode(l, floatingId))
	{
		f->FloatPos = pos;
		Bump(l);
		return true;
	}
	return false;
}

inline bool ResizeFloating(DockLayout& l, NodeId floatingId, const glm::vec2& size)
{
	if (DockNode* f = FindFloatingNode(l, floatingId))
	{
		f->FloatSize = glm::max(size, glm::vec2(10.0f, 10.0f));
		Bump(l);
		return true;
	}
	return false;
}

inline bool RemoveFloating(DockLayout& l, NodeId floatingId)
{
	for (WindowNode& w : l.Windows)
	{
		for (auto it = w.Floating.begin(); it != w.Floating.end(); ++it)
		{
			if (it->Id == floatingId)
			{
				w.Floating.erase(it);
				NormalizeLayout(l); // the window may now be empty (invariant 11)
				Bump(l);
				return true;
			}
		}
	}
	return false;
}

inline bool BringFloatingToFront(DockLayout& l, NodeId floatingId)
{
	for (WindowNode& w : l.Windows)
	{
		for (auto it = w.Floating.begin(); it != w.Floating.end(); ++it)
		{
			if (it->Id == floatingId)
			{
				DockNode n = std::move(*it);
				w.Floating.erase(it);
				w.Floating.push_back(std::move(n));
				Bump(l);
				return true;
			}
		}
	}
	return false;
}

// ---- ApplyDrop (single code path for preview dry-run + real drop) ------------

// Insert a whole node subtree (well or split tree) next to `targetWell` on the
// `edge` side, using the ratio semantics. `targetWell` must live in
// `owner` (the drop's target window).
inline bool InsertSubtreeNextTo(WindowNode& owner, DockLayout& l, NodeId targetWell, Edge edge, DockNode content)
{
	DockNode* well = FindNode(l, targetWell);
	if (!well || well->Kind != DockNode::Kind::TabWell) return false;
	Axis splitAxis = (edge == Edge::North || edge == Edge::South) ? Axis::Vertical : Axis::Horizontal;
	bool newFirst = (edge == Edge::North || edge == Edge::West);

	if (owner.Main.Id == targetWell)
	{
		DockNode oldMain = std::move(owner.Main);
		owner.Main = MakeSplit2(l, splitAxis, newFirst, std::move(content), std::move(oldMain), 0.5f, 0.5f);
		return true;
	}

	int idx = -1;
	DockNode* parent = FindParentSlotAny(l, targetWell, &idx);
	if (!parent) return false;

	if (idx >= 0 && parent->Kind == DockNode::Kind::Split && parent->SplitAxis == splitAxis)
	{
		float targetShare = parent->Ratios[idx];
		int insertAt = newFirst ? idx : idx + 1;
		parent->Children.insert(parent->Children.begin() + insertAt, std::move(content));
		parent->Ratios.insert(parent->Ratios.begin() + insertAt, 0.5f * targetShare);
		int targetNow = (insertAt == idx) ? idx + 1 : idx;
		parent->Ratios[targetNow] = 0.5f * targetShare;
		Renormalize(parent->Ratios);
	}
	else if (idx >= 0 && parent->Kind == DockNode::Kind::Split)
	{
		float targetShare = parent->Ratios[idx];
		DockNode oldWell = std::move(parent->Children[idx]);
		DockNode wrap = MakeSplit2(l, splitAxis, newFirst, std::move(content), std::move(oldWell), 0.5f, 0.5f);
		parent->Children[idx] = std::move(wrap);
		parent->Ratios[idx] = targetShare;
	}
	else if (idx == -1 && parent->Kind == DockNode::Kind::Floating)
	{
		DockNode oldWell = std::move(*parent->FloatChild);
		parent->FloatChild = std::make_unique<DockNode>(MakeSplit2(l, splitAxis, newFirst, std::move(content), std::move(oldWell), 0.5f, 0.5f));
	}
	else
		return false;
	return true;
}

inline void MergeTabsInto(DockNode& target, const DockNode& content)
{
	std::function<void(const DockNode&)> collect = [&](const DockNode& n) {
		if (n.Kind == DockNode::Kind::TabWell)
			for (const auto& t : n.Tabs) target.Tabs.push_back(t);
		else if (n.Kind == DockNode::Kind::Split)
			for (const auto& c : n.Children) collect(c);
		else if (n.Kind == DockNode::Kind::Floating && n.FloatChild)
			collect(*n.FloatChild);
	};
	collect(content);
	if (target.ActiveIndex < 0 || target.ActiveIndex >= (int)target.Tabs.size())
		target.ActiveIndex = std::max(0, (int)target.Tabs.size() - 1);
}

// Pure: copy, remove source (well tab or whole floating panel, anywhere in
// the layout), apply the zone insert against `targetWindow`'s tree,
// normalize. The single code path for preview dry-run, real drops, and the
// "move to window" command.
inline DockLayout ApplyDrop(const DockLayout& in, NodeId targetWindow, const DropTarget& target, const DropPayload& p)
{
	DockLayout l = CloneLayout(in);

	// Self-drop guard: a WellZone target inside the floating panel being
	// dragged (or already missing) makes the drop a no-op — the source
	// subtree would otherwise be extracted and then lost.
	if (target.KindId == DropTarget::Kind::WellZone && p.SourceFloating != 0)
	{
		const DockNode* tgt = FindNode(in, target.Well);
		const DockNode* src = FindFloatingNode(in, p.SourceFloating);
		bool insideSource = false;
		if (tgt && src)
		{
			std::function<bool(const DockNode&)> within = [&](const DockNode& n) -> bool {
				if (n.Id == tgt->Id) return true;
				for (const auto& c : n.Children)
					if (within(c)) return true;
				if (n.FloatChild && within(*n.FloatChild)) return true;
				return false;
			};
			insideSource = within(*src);
		}
		if (!tgt || insideSource)
			return CloneLayout(in);
	}

	// Extract the dragged content first.
	DockNode content;          // floating subtree when the source is floating
	bool hasFloatingSource = false;
	glm::vec2 floatOrigPos{ 0, 0 }, floatOrigSize{ 400, 300 };
	if (p.SourceFloating != 0)
	{
		if (DockNode* f = FindFloatingNode(l, p.SourceFloating))
		{
			floatOrigPos = f->FloatPos;
			floatOrigSize = f->FloatSize;
			content = std::move(*f->FloatChild);
			// remove the floating node (search every window)
			for (WindowNode& w : l.Windows)
			{
				w.Floating.erase(std::remove_if(w.Floating.begin(), w.Floating.end(),
					[&](const DockNode& n) { return n.Id == p.SourceFloating; }), w.Floating.end());
			}
			hasFloatingSource = true;
		}
		else
		{
			return CloneLayout(in); // source already gone — no-op
		}
	}
	else if (p.SourceWell != 0)
	{
		DockNode* well = FindNode(l, p.SourceWell);
		if (!well || well->Kind != DockNode::Kind::TabWell) return CloneLayout(in);
		int idx = FindTabIndex(*well, p.Panel);
		if (idx < 0) return CloneLayout(in);
		well->Tabs.erase(well->Tabs.begin() + idx);
		if (well->ActiveIndex >= (int)well->Tabs.size())
			well->ActiveIndex = std::max(0, (int)well->Tabs.size() - 1);
	}

	if (p.Singleton)
		RemovePanelEverywhere(l, p.Panel, 0);

	// Resolve the target window in the working copy (a target in a window
	// the extraction just removed is already a no-op via NormalizeLayout's
	// window pruning, but guard the reference anyway).
	WindowNode* tw = nullptr;
	for (WindowNode& w : l.Windows)
		if (w.Id == targetWindow) { tw = &w; break; }
	if (!tw)
		return CloneLayout(in); // target window gone — no-op

	switch (target.KindId)
	{
	case DropTarget::Kind::WellZone:
	{
		if (target.ZoneId == Zone::Center)
		{
			if (hasFloatingSource)
			{
				if (DockNode* w = FindNode(l, target.Well))
					if (w->Kind == DockNode::Kind::TabWell)
						MergeTabsInto(*w, content);
			}
			else
				InsertTab(l, target.Well, p.Panel, -1, p.Singleton);
		}
		else
		{
			Edge edge = (target.ZoneId == Zone::North) ? Edge::North
				: (target.ZoneId == Zone::South) ? Edge::South
				: (target.ZoneId == Zone::East) ? Edge::East : Edge::West;
			if (hasFloatingSource)
				InsertSubtreeNextTo(*tw, l, target.Well, edge, std::move(content));
			else
				SplitWell(l, target.Well, edge, p.Panel, 0.5f, p.Singleton);
		}
		break;
	}
	case DropTarget::Kind::RootEdge:
	{
		if (tw->Main.Kind == DockNode::Kind::TabWell && tw->Main.Tabs.empty())
		{
			tw->Main = hasFloatingSource ? std::move(content) : MakeTabWell(l, { TabEntry{ p.Panel } }, 0);
		}
		else
		{
			DockNode newContent = hasFloatingSource ? std::move(content) : MakeTabWell(l, { TabEntry{ p.Panel } }, 0);
			DockNode oldMain = std::move(tw->Main);
			Axis axis = (target.RootEdge == Edge::North || target.RootEdge == Edge::South) ? Axis::Vertical : Axis::Horizontal;
			bool newFirst = (target.RootEdge == Edge::North || target.RootEdge == Edge::West);
			tw->Main = MakeSplit2(l, axis, newFirst, std::move(newContent), std::move(oldMain), 0.5f, 0.5f);
		}
		break;
	}
	case DropTarget::Kind::Miss:
	{
		if (hasFloatingSource)
		{
			// Just reposition the floating panel under the cursor.
			glm::vec2 pos = target.MissPos - floatOrigSize * 0.5f;
			tw->Floating.push_back(MakeFloating(l, pos, floatOrigSize, std::move(content)));
		}
		else
		{
			DockNode newContent = MakeTabWell(l, { TabEntry{ p.Panel } }, 0);
			glm::vec2 pos = target.MissPos - glm::vec2(200.0f, 150.0f);
			tw->Floating.push_back(MakeFloating(l, pos, { 400, 300 }, std::move(newContent)));
		}
		break;
	}
	}

	NormalizeLayout(l);
	Bump(l);
	return l;
}

// DockFloating — re-dock a floating panel: routes through ApplyDrop against
// the window that owns the floating panel.
inline DockLayout DockFloating(const DockLayout& in, NodeId floatingId, const DropTarget& target)
{
	NodeId ownerWindow = 0;
	const WindowNode* owner = FindWindowContainingNode(in, floatingId);
	if (!owner)
		return CloneLayout(in);
	ownerWindow = owner->Id;

	DropPayload p;
	p.SourceFloating = floatingId;
	return ApplyDrop(in, ownerWindow, target, p);
}

// ---- Window transforms ------------------------------------------------------

enum class ClosePolicy
{
	PerPanel,   // panels destroyed per their registry close policy (model: dropped)
	MergeToMain // tabs appended to the main window's default well
};

// OpenWindow — remove a tab from its well and host it in a brand-new
// non-main window. Returns the new window's id (0 on failure).
inline NodeId OpenWindow(DockLayout& l, NodeId sourceWell, int tabIndex,
	const glm::vec2& size = { 1280.0f, 720.0f })
{
	DockNode* well = FindNode(l, sourceWell);
	if (!well || well->Kind != DockNode::Kind::TabWell) return 0;
	if (tabIndex < 0 || tabIndex >= (int)well->Tabs.size()) return 0;

	TabEntry moved = well->Tabs[tabIndex];
	well->Tabs.erase(well->Tabs.begin() + tabIndex);
	if (well->ActiveIndex >= (int)well->Tabs.size())
		well->ActiveIndex = std::max(0, (int)well->Tabs.size() - 1);

	DockNode content = MakeTabWell(l, { moved }, 0);

	WindowNode wn;
	wn.Id = l.NextNodeId++;
	wn.bMain = false;
	wn.Size = size;
	wn.Main = std::move(content);
	l.Windows.push_back(std::move(wn));

	NormalizeLayout(l);
	Bump(l);
	return l.Windows.back().Id;
}

// MoveToWindow — the command-driven gesture and the cross-window drop are
// the same transform: extract the tab and apply the target against the
// destination window's tree via ApplyDrop.
inline DockLayout MoveToWindow(const DockLayout& in, NodeId fromWell, int tabIndex,
	NodeId targetWindow, const DropTarget& target)
{
	const DockNode* from = FindNode(in, fromWell);
	if (!from || from->Kind != DockNode::Kind::TabWell) return CloneLayout(in);
	if (tabIndex < 0 || tabIndex >= (int)from->Tabs.size()) return CloneLayout(in);

	DropPayload p;
	p.Panel = from->Tabs[tabIndex].Id;
	p.SourceWell = fromWell;
	return ApplyDrop(in, targetWindow, target, p);
}

// CloseWindow — PerPanel drops the window and everything in it (panel
// destroy/cache policy lives above the model); MergeToMain appends every
// tab in the window to the main window's default well first. The main
// window is never removed (invariant 10).
inline bool CloseWindow(DockLayout& l, NodeId windowId, ClosePolicy policy)
{
	WindowNode* target = nullptr;
	for (WindowNode& w : l.Windows)
		if (w.Id == windowId) { target = &w; break; }
	if (!target || target->bMain) return false;

	if (policy == ClosePolicy::MergeToMain)
	{
		WindowNode& main = l.MainWindow();
		DockNode* dest = nullptr;
		if (main.Main.Kind == DockNode::Kind::TabWell)
			dest = &main.Main;
		else if (DockNode* first = FindFirstWell(main.Main))
			dest = first;

		if (dest)
		{
			std::function<void(const DockNode&)> collect = [&](const DockNode& n) {
				if (n.Kind == DockNode::Kind::TabWell)
					for (const auto& t : n.Tabs) dest->Tabs.push_back(t);
				else if (n.Kind == DockNode::Kind::Split)
					for (const auto& c : n.Children) collect(c);
				else if (n.Kind == DockNode::Kind::Floating && n.FloatChild)
					collect(*n.FloatChild);
			};
			collect(target->Main);
			for (const auto& f : target->Floating) collect(f);
			if (dest->ActiveIndex < 0 || dest->ActiveIndex >= (int)dest->Tabs.size())
				dest->ActiveIndex = std::max(0, (int)dest->Tabs.size() - 1);
		}
	}

	for (auto it = l.Windows.begin(); it != l.Windows.end(); ++it)
	{
		if (it->Id == windowId)
		{
			l.Windows.erase(it);
			break;
		}
	}

	NormalizeLayout(l);
	Bump(l);
	return true;
}

// ---- Geometry solver (mirrors Splitter arrange math incl. min clamping) ------

using MinSizeFn = std::function<glm::vec2(NodeId)>;

struct NodeRect
{
	NodeId   Id;
	glm::vec4 Rect; // x, y, w, h
};

// Shared size solver: distribute `avail` main-axis space among ratios with
// per-slot mins. Non-destructive clamp; below sum-of-mins → proportional-to-min.
// THE SPLITTER WIDGET USES THIS SAME FUNCTION (single source of truth).
inline std::vector<float> SolveSizes(const std::vector<float>& ratios, float avail,
	const std::vector<float>& mins, float dividerThickness)
{
	int n = (int)ratios.size();
	float dividerTotal = dividerThickness * (float)std::max(0, n - 1);
	avail = std::max(0.0f, avail - dividerTotal);

	std::vector<float> out(n, 0.0f);
	if (n == 0) return out;

	float sumMin = 0.0f;
	for (float m : mins) sumMin += std::max(0.0f, m);

	if (sumMin >= avail && sumMin > 0.0f)
	{
		for (int i = 0; i < n; i++)
			out[i] = (std::max(0.0f, mins[i]) / sumMin) * avail;
		return out;
	}

	float sumRatios = 0.0f;
	for (float r : ratios) sumRatios += std::max(0.0f, r);
	if (sumRatios <= 0.0f)
	{
		for (int i = 0; i < n; i++) out[i] = avail / (float)n;
	}
	else
	{
		for (int i = 0; i < n; i++)
			out[i] = std::max(0.0f, ratios[i]) / sumRatios * avail;
	}

	// Lift panes up to their mins by borrowing proportionally from others.
	for (int iter = 0; iter < 64; iter++)
	{
		bool changed = false;
		for (int i = 0; i < n; i++)
		{
			float deficit = std::max(0.0f, mins[i]) - out[i];
			if (deficit <= 1e-4f) continue;
			float borrowable = 0.0f;
			for (int j = 0; j < n; j++)
				if (j != i) borrowable += std::max(0.0f, out[j] - std::max(0.0f, mins[j]));
			if (borrowable <= 1e-4f) continue;
			float take = std::min(deficit, borrowable);
			for (int j = 0; j < n; j++)
			{
				if (j == i) continue;
				float jBorrow = std::max(0.0f, out[j] - std::max(0.0f, mins[j]));
				float jTake = take * (jBorrow / borrowable);
				out[j] -= jTake;
				out[i] += jTake;
			}
			changed = true;
		}
		if (!changed) break;
	}
	return out;
}

inline void SolveNodeRects(const DockNode& n, const glm::vec2& pos, const glm::vec2& size,
	const MinSizeFn& mins, float dividerThickness, std::vector<NodeRect>& out)
{
	if (n.Kind == DockNode::Kind::Split)
	{
		std::vector<float> slotMins;
		for (const auto& c : n.Children)
		{
			glm::vec2 m = mins(c.Id);
			slotMins.push_back(n.SplitAxis == Axis::Horizontal ? m.x : m.y);
		}
		bool horizontal = (n.SplitAxis == Axis::Horizontal);
		float avail = horizontal ? size.x : size.y;
		std::vector<float> sizes = SolveSizes(n.Ratios, avail, slotMins, dividerThickness);
		float offset = 0.0f;
		for (int i = 0; i < (int)n.Children.size(); i++)
		{
			glm::vec2 childPos = pos, childSize = size;
			if (horizontal) { childPos.x += offset; childSize.x = sizes[i]; }
			else { childPos.y += offset; childSize.y = sizes[i]; }
			SolveNodeRects(n.Children[i], childPos, childSize, mins, dividerThickness, out);
			offset += sizes[i] + dividerThickness;
		}
	}
	else if (n.Kind == DockNode::Kind::TabWell)
	{
		out.push_back({ n.Id, { pos.x, pos.y, size.x, size.y } });
	}
}

inline std::vector<NodeRect> ComputeNodeRects(const DockLayout& l, NodeId windowId, const glm::vec2& avail,
	const MinSizeFn& mins, float dividerThickness)
{
	const WindowNode* w = nullptr;
	for (const WindowNode& c : l.Windows)
		if (c.Id == windowId) { w = &c; break; }
	std::vector<NodeRect> out;
	if (!w) return out;
	if (!(w->Main.Kind == DockNode::Kind::TabWell && w->Main.Tabs.empty()))
		SolveNodeRects(w->Main, { 0, 0 }, avail, mins, dividerThickness, out);
	else
		out.push_back({ w->Main.Id, { 0, 0, avail.x, avail.y } });
	for (const auto& f : w->Floating)
		out.push_back({ f.Id, { f.FloatPos.x, f.FloatPos.y, f.FloatSize.x, f.FloatSize.y } });
	return out;
}

// ---- Serialization -----------------------------------------------------------

inline void WriteNode(rapidjson::Value& out, const DockNode& n, rapidjson::Document::AllocatorType& a)
{
	out.SetObject();
	switch (n.Kind)
	{
	case DockNode::Kind::Split:
	{
		out.AddMember("kind", "split", a);
		out.AddMember("axis", n.SplitAxis == Axis::Horizontal ? "H" : "V", a);
		rapidjson::Value ratios(rapidjson::kArrayType);
		for (float r : n.Ratios) ratios.PushBack(r, a);
		out.AddMember("ratios", ratios, a);
		rapidjson::Value children(rapidjson::kArrayType);
		for (const auto& c : n.Children)
		{
			rapidjson::Value child(rapidjson::kObjectType);
			WriteNode(child, c, a);
			children.PushBack(child, a);
		}
		out.AddMember("children", children, a);
		break;
	}
	case DockNode::Kind::TabWell:
	{
		out.AddMember("kind", "tabwell", a);
		out.AddMember("active", n.ActiveIndex, a);
		rapidjson::Value tabs(rapidjson::kArrayType);
		for (const auto& t : n.Tabs)
		{
			rapidjson::Value v;
			v.SetString(t.Id.c_str(), (rapidjson::SizeType)t.Id.size(), a);
			tabs.PushBack(v, a);
		}
		out.AddMember("tabs", tabs, a);
		break;
	}
	case DockNode::Kind::Floating:
	{
		out.AddMember("kind", "floating", a);
		rapidjson::Value pos(rapidjson::kArrayType);
		pos.PushBack(n.FloatPos.x, a);
		pos.PushBack(n.FloatPos.y, a);
		out.AddMember("pos", pos, a);
		rapidjson::Value size(rapidjson::kArrayType);
		size.PushBack(n.FloatSize.x, a);
		size.PushBack(n.FloatSize.y, a);
		out.AddMember("size", size, a);
		rapidjson::Value child(rapidjson::kObjectType);
		WriteNode(child, *n.FloatChild, a);
		out.AddMember("child", child, a);
		break;
	}
	}
}

inline std::string SerializeLayout(const DockLayout& l, bool pretty = true, bool includePositions = true)
{
	rapidjson::Document doc;
	doc.SetObject();
	auto& a = doc.GetAllocator();
	doc.AddMember("version", 2, a);

	rapidjson::Value windows(rapidjson::kArrayType);
	for (const WindowNode& w : l.Windows)
	{
		rapidjson::Value wv(rapidjson::kObjectType);
		wv.AddMember("bMain", w.bMain, a);
		rapidjson::Value size(rapidjson::kArrayType);
		size.PushBack(w.Size.x, a);
		size.PushBack(w.Size.y, a);
		wv.AddMember("size", size, a);

		// Invariant 13 (write-side guard): positions are serialized only when
		// the platform can actually place windows - a layout saved on Wayland
		// (or for a compositor-managed window) never carries one, so layouts
		// stay portable across machines/platforms.
		if (includePositions && w.Pos.has_value())
		{
			rapidjson::Value pos(rapidjson::kArrayType);
			pos.PushBack(w.Pos->x, a);
			pos.PushBack(w.Pos->y, a);
			wv.AddMember("pos", pos, a);
		}

		rapidjson::Value main(rapidjson::kObjectType);
		WriteNode(main, w.Main, a);
		wv.AddMember("main", main, a);
		rapidjson::Value floating(rapidjson::kArrayType);
		for (const auto& f : w.Floating)
		{
			rapidjson::Value v(rapidjson::kObjectType);
			WriteNode(v, f, a);
			floating.PushBack(v, a);
		}
		wv.AddMember("floating", floating, a);
		windows.PushBack(wv, a);
	}
	doc.AddMember("windows", windows, a);

	rapidjson::StringBuffer sb;
	if (pretty)
	{
		rapidjson::PrettyWriter<rapidjson::StringBuffer> w(sb);
		doc.Accept(w);
	}
	else
	{
		rapidjson::Writer<rapidjson::StringBuffer> w(sb);
		doc.Accept(w);
	}
	return sb.GetString();
}

inline bool ReadNode(const rapidjson::Value& v, DockLayout& l, DockNode& out,
	const std::function<bool(const PanelId&)>& panelKnown,
	const std::function<void(const std::string&)>& warn)
{
	if (!v.IsObject()) return false;
	auto kindIt = v.FindMember("kind");
	if (kindIt == v.MemberEnd() || !kindIt->value.IsString()) return false;
	std::string kind = kindIt->value.GetString();

	if (kind == "split")
	{
		out.Kind = DockNode::Kind::Split;
		out.Id = l.NextNodeId++;
		auto axisIt = v.FindMember("axis");
		out.SplitAxis = (axisIt != v.MemberEnd() && axisIt->value.IsString() && std::string(axisIt->value.GetString()) == "V")
			? Axis::Vertical : Axis::Horizontal;
		auto cIt = v.FindMember("children");
		if (cIt == v.MemberEnd() || !cIt->value.IsArray()) return false;
		for (const auto& c : cIt->value.GetArray())
		{
			DockNode child;
			if (!ReadNode(c, l, child, panelKnown, warn)) return false;
			out.Children.push_back(std::move(child));
		}
		auto rIt = v.FindMember("ratios");
		if (rIt != v.MemberEnd() && rIt->value.IsArray())
			for (const auto& r : rIt->value.GetArray())
				if (r.IsNumber()) out.Ratios.push_back(r.GetFloat());
		while (out.Ratios.size() < out.Children.size()) out.Ratios.push_back(1.0f);
		out.Ratios.resize(out.Children.size());
		Renormalize(out.Ratios);
		return true;
	}
	if (kind == "tabwell")
	{
		out.Kind = DockNode::Kind::TabWell;
		out.Id = l.NextNodeId++;
		auto tIt = v.FindMember("tabs");
		if (tIt != v.MemberEnd() && tIt->value.IsArray())
		{
			for (const auto& t : tIt->value.GetArray())
			{
				if (!t.IsString()) continue;
				PanelId id(t.GetString(), t.GetStringLength());
				if (!panelKnown(id))
				{
					warn("dock layout: dropping unknown panel id '" + id + "'");
					continue;
				}
				out.Tabs.push_back({ id });
			}
		}
		auto aIt = v.FindMember("active");
		out.ActiveIndex = (aIt != v.MemberEnd() && aIt->value.IsInt()) ? aIt->value.GetInt() : 0;
		return true;
	}
	if (kind == "floating")
	{
		out.Kind = DockNode::Kind::Floating;
		out.Id = l.NextNodeId++;
		auto pIt = v.FindMember("pos");
		if (pIt != v.MemberEnd() && pIt->value.IsArray() && pIt->value.Size() >= 2)
		{
			out.FloatPos.x = pIt->value[0].GetFloat();
			out.FloatPos.y = pIt->value[1].GetFloat();
		}
		auto sIt = v.FindMember("size");
		if (sIt != v.MemberEnd() && sIt->value.IsArray() && sIt->value.Size() >= 2)
		{
			out.FloatSize.x = sIt->value[0].GetFloat();
			out.FloatSize.y = sIt->value[1].GetFloat();
		}
		auto cIt = v.FindMember("child");
		if (cIt == v.MemberEnd()) return false;
		DockNode fc;
		if (!ReadNode(cIt->value, l, fc, panelKnown, warn)) return false;
		out.FloatChild = std::make_unique<DockNode>(std::move(fc));
		return true;
	}
	return false;
}

// ParseLayout — empty/corrupt/newer-version → false (caller falls back to
// default). v2 (windows[]) loads directly; v1 (main/floating) migrates by
// wrapping its tree as the single main WindowNode (one-shot migration, no
// dual-path maintenance). Loaded layouts are normalized + validated.
inline bool ParseLayout(const std::string& json, DockLayout& out,
	const std::function<bool(const PanelId&)>& panelKnown,
	const std::function<void(const std::string&)>& warn)
{
	rapidjson::Document doc;
	doc.Parse(json.c_str(), json.size());
	if (doc.HasParseError() || !doc.IsObject()) return false;

	int version = 1;
	auto vIt = doc.FindMember("version");
	if (vIt != doc.MemberEnd() && vIt->value.IsInt())
		version = vIt->value.GetInt();
	if (version > 2)
		return false;

	DockLayout l;
	l.Windows.clear();

	if (version == 2)
	{
		auto wIt = doc.FindMember("windows");
		if (wIt == doc.MemberEnd() || !wIt->value.IsArray())
			return false;
		for (const auto& wv : wIt->value.GetArray())
		{
			if (!wv.IsObject()) return false;
			WindowNode wn;
			wn.Id = l.NextNodeId++;
			auto bm = wv.FindMember("bMain");
			if (bm != wv.MemberEnd() && bm->value.IsBool())
				wn.bMain = bm->value.GetBool();
			auto sz = wv.FindMember("size");
			if (sz != wv.MemberEnd() && sz->value.IsArray() && sz->value.Size() >= 2)
			{
				wn.Size.x = sz->value[0].GetFloat();
				wn.Size.y = sz->value[1].GetFloat();
			}
			auto pos = wv.FindMember("pos");
			if (pos != wv.MemberEnd() && pos->value.IsArray() && pos->value.Size() >= 2)
				wn.Pos = glm::vec2{ pos->value[0].GetFloat(), pos->value[1].GetFloat() };
			auto mIt = wv.FindMember("main");
			if (mIt == wv.MemberEnd() || !ReadNode(mIt->value, l, wn.Main, panelKnown, warn))
				return false;
			auto fIt = wv.FindMember("floating");
			if (fIt != wv.MemberEnd() && fIt->value.IsArray())
			{
				for (const auto& f : fIt->value.GetArray())
				{
					DockNode n;
					if (ReadNode(f, l, n, panelKnown, warn) && n.Kind == DockNode::Kind::Floating)
						wn.Floating.push_back(std::move(n));
				}
			}
			l.Windows.push_back(std::move(wn));
		}
	}
	else // version 1 — one-shot migration: wrap as the single main window
	{
		WindowNode wn;
		wn.Id = l.NextNodeId++;
		wn.bMain = true;
		auto mIt = doc.FindMember("main");
		if (mIt == doc.MemberEnd() || !ReadNode(mIt->value, l, wn.Main, panelKnown, warn))
			return false;
		auto fIt = doc.FindMember("floating");
		if (fIt != doc.MemberEnd() && fIt->value.IsArray())
		{
			for (const auto& f : fIt->value.GetArray())
			{
				DockNode n;
				if (ReadNode(f, l, n, panelKnown, warn) && n.Kind == DockNode::Kind::Floating)
					wn.Floating.push_back(std::move(n));
			}
		}
		l.Windows.push_back(std::move(wn));
	}

	NormalizeLayout(l);
	out = std::move(l);
	return true;
}

inline DockLayout MakeDefaultLayout(const std::vector<PanelId>& initialPanels)
{
	DockLayout l;
	l.Windows.clear();
	WindowNode wn;
	wn.Id = l.NextNodeId++;
	wn.bMain = true;
	wn.Main.Kind = DockNode::Kind::TabWell;
	wn.Main.Id = l.NextNodeId++;
	for (const auto& id : initialPanels)
		wn.Main.Tabs.push_back({ id });
	wn.Main.ActiveIndex = 0;
	l.Windows.push_back(std::move(wn));
	NormalizeLayout(l);
	return l;
}

}} // namespace wisp::dock
