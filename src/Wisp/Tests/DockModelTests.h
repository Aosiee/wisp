#pragma once

#include "Wisp/Docking/DockModel.h"
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace wisp { namespace dock {

struct DockTestHarness
{
	int Pass = 0, Fail = 0;
	std::vector<std::string> Failures;

	void Check(bool cond, const char* what)
	{
		if (cond) { Pass++; }
		else { Fail++; Failures.push_back(what); }
	}
};

static void LogDockTest(const std::string& msg)
{
	std::fprintf(stderr, "DOCKTEST: %s\n", msg.c_str());
}

// Deep comparison ignoring NodeIds (ser/de does not persist ids).
static bool NodesEquivalentIgnoringIds(const DockNode& a, const DockNode& b)
{
	if (a.Kind != b.Kind) return false;
	switch (a.Kind)
	{
	case DockNode::Kind::TabWell:
	{
		if (a.Tabs.size() != b.Tabs.size()) return false;
		for (size_t i = 0; i < a.Tabs.size(); i++)
			if (a.Tabs[i].Id != b.Tabs[i].Id) return false;
		return a.ActiveIndex == b.ActiveIndex;
	}
	case DockNode::Kind::Split:
	{
		if (a.SplitAxis != b.SplitAxis || a.Children.size() != b.Children.size()) return false;
		for (size_t i = 0; i < a.Children.size(); i++)
		{
			if (!NodesEquivalentIgnoringIds(a.Children[i], b.Children[i])) return false;
			if (std::abs(a.Ratios[i] - b.Ratios[i]) > 1e-4f) return false;
		}
		return true;
	}
	case DockNode::Kind::Floating:
	{
		if (glm::length(a.FloatPos - b.FloatPos) > 1e-4f) return false;
		if (glm::length(a.FloatSize - b.FloatSize) > 1e-4f) return false;
		return (!!a.FloatChild == !!b.FloatChild) && (!a.FloatChild || NodesEquivalentIgnoringIds(*a.FloatChild, *b.FloatChild));
	}
	}
	return false;
}

static bool LayoutsEquivalentIgnoringIds(const DockLayout& a, const DockLayout& b)
{
	if (a.Windows.size() != b.Windows.size()) return false;
	for (size_t i = 0; i < a.Windows.size(); i++)
	{
		const WindowNode& wa = a.Windows[i];
		const WindowNode& wb = b.Windows[i];
		if (wa.bMain != wb.bMain) return false;
		if (glm::length(wa.Size - wb.Size) > 1e-4f) return false;
		if (wa.Pos.has_value() != wb.Pos.has_value()) return false;
		if (wa.Pos && glm::length(*wa.Pos - *wb.Pos) > 1e-4f) return false;
		if (!NodesEquivalentIgnoringIds(wa.Main, wb.Main)) return false;
		if (wa.Floating.size() != wb.Floating.size()) return false;
		for (size_t j = 0; j < wa.Floating.size(); j++)
			if (!NodesEquivalentIgnoringIds(wa.Floating[j], wb.Floating[j])) return false;
	}
	return true;
}

static void CollectWellIds(const DockNode& n, std::vector<NodeId>& out)
{
	if (n.Kind == DockNode::Kind::TabWell) out.push_back(n.Id);
	else if (n.Kind == DockNode::Kind::Split)
		for (const auto& c : n.Children) CollectWellIds(c, out);
	else if (n.Kind == DockNode::Kind::Floating && n.FloatChild) CollectWellIds(*n.FloatChild, out);
}
static void CollectSplitIds(const DockNode& n, std::vector<NodeId>& out)
{
	if (n.Kind == DockNode::Kind::Split)
	{
		out.push_back(n.Id);
		for (const auto& c : n.Children) CollectSplitIds(c, out);
	}
	else if (n.Kind == DockNode::Kind::Floating && n.FloatChild) CollectSplitIds(*n.FloatChild, out);
}
static std::vector<NodeId> AllWellIds(const DockLayout& l)
{
	std::vector<NodeId> out;
	for (const WindowNode& w : l.Windows)
	{
		CollectWellIds(w.Main, out);
		for (const auto& f : w.Floating) CollectWellIds(f, out);
	}
	return out;
}
static std::vector<NodeId> AllSplitIds(const DockLayout& l)
{
	std::vector<NodeId> out;
	for (const WindowNode& w : l.Windows)
	{
		CollectSplitIds(w.Main, out);
		for (const auto& f : w.Floating) CollectSplitIds(f, out);
	}
	return out;
}

// Deterministic LCG for the fuzz loop.
struct Lcg
{
	uint32_t State = 0x12345678u;
	uint32_t Next() { State = State * 1664525u + 1013904223u; return State; }
	uint32_t RandBelow(uint32_t n) { return n == 0 ? 0 : Next() % n; }
	float Unit() { return (float)Next() / 4294967296.0f; }
};

static void TestNormalization(DockTestHarness& h)
{
	// Root placeholder on empty default
	{
		DockLayout l = MakeDefaultLayout({});
		std::vector<InvariantViolation> v;
		h.Check(ValidateLayout(l, {}, v), "empty default layout validates");
		h.Check(l.MainWindow().Main.Kind == DockNode::Kind::TabWell && l.MainWindow().Main.Tabs.empty(), "root placeholder is an empty well");
	}
	// RemoveTab on the only tab -> placeholder survives
	{
		DockLayout l = MakeDefaultLayout({ "A" });
		NodeId well = l.MainWindow().Main.Id;
		RemoveTab(l, well, 0);
		h.Check(l.MainWindow().Main.Kind == DockNode::Kind::TabWell && l.MainWindow().Main.Tabs.empty(), "removing last root tab leaves placeholder");
		std::vector<InvariantViolation> v;
		h.Check(ValidateLayout(l, {}, v), "placeholder layout validates");
	}
	// Split of a single well, then empty one half
	{
		DockLayout l = MakeDefaultLayout({ "A", "B" });
		NodeId well = l.MainWindow().Main.Id;
		SplitWell(l, well, Edge::East, "C", 0.5f);
		h.Check(l.MainWindow().Main.Kind == DockNode::Kind::Split, "root became a split");
		std::vector<NodeId> wells = AllWellIds(l);
		// find the well containing C
		NodeId cWell = 0;
		for (NodeId w : wells)
		{
			const DockNode* n = FindNode(l, w);
			if (FindTabIndex(*n, "C") >= 0) { cWell = w; break; }
		}
		RemoveTab(l, cWell, FindTabIndex(*FindNode(l, cWell), "C"));
		h.Check(l.MainWindow().Main.Kind == DockNode::Kind::TabWell, "single-child split collapsed back to a well");
		std::vector<InvariantViolation> v;
		h.Check(ValidateLayout(l, {}, v), "post-collapse layout validates");
	}
	// Same-axis flattening: split A|B east, then split A again east
	{
		DockLayout l = MakeDefaultLayout({ "A", "B" });
		NodeId well = l.MainWindow().Main.Id;
		SplitWell(l, well, Edge::East, "C", 0.5f);
		NodeId aWell = 0;
		for (NodeId w : AllWellIds(l))
		{
			const DockNode* n = FindNode(l, w);
			if (FindTabIndex(*n, "A") >= 0) { aWell = w; break; }
		}
		SplitWell(l, aWell, Edge::East, "D", 0.5f);
		h.Check(l.MainWindow().Main.Kind == DockNode::Kind::Split && l.MainWindow().Main.Children.size() == 3,
			"same-axis splits flattened to 3 siblings");
		std::vector<InvariantViolation> v;
		h.Check(ValidateLayout(l, {}, v), "flattened layout validates");
	}
	// ActiveIndex clamp
	{
		DockLayout l = MakeDefaultLayout({ "A", "B" });
		l.MainWindow().Main.ActiveIndex = 99;
		NormalizeLayout(l);
		h.Check(l.MainWindow().Main.ActiveIndex == 1, "ActiveIndex clamped");
	}
}

static void TestTransforms(DockTestHarness& h)
	{
		// MovePanel reorder within one well
		DockLayout l = MakeDefaultLayout({ "A", "B", "C" });
		NodeId well = l.MainWindow().Main.Id;
		MovePanel(l, well, 0, well, 2);
		h.Check(FindTabIndex(l.MainWindow().Main, "A") == 2 && FindTabIndex(l.MainWindow().Main, "B") == 0, "MovePanel reorders within a well");
		// MovePanel across wells
		DockLayout l2 = MakeDefaultLayout({ "A", "B" });
		NodeId w0 = l2.MainWindow().Main.Id;
		SplitWell(l2, w0, Edge::East, "C", 0.5f);
		NodeId cWell = 0;
		for (NodeId w : AllWellIds(l2))
		{
			const DockNode* n = FindNode(l2, w);
			if (FindTabIndex(*n, "C") >= 0) { cWell = w; break; }
		}
		MovePanel(l2, w0, FindTabIndex(*FindNode(l2, w0), "A"), cWell);
		const DockNode* cw = FindNode(l2, cWell);
		h.Check(cw && FindTabIndex(*cw, "A") >= 0, "MovePanel moved tab across wells");
		std::vector<InvariantViolation> v;
		h.Check(ValidateLayout(l2, {}, v), "post-move layout validates");
		// Singleton enforcement across Main + Floating
		DockLayout l3 = MakeDefaultLayout({ "X", "Y" });
		NodeId w = l3.MainWindow().Main.Id;
		FloatPanel(l3, w, FindTabIndex(*FindNode(l3, w), "Y"), { 10, 10 });
		NodeId floatId = l3.MainWindow().Floating.back().Id;
		// insert singleton X into the floating well
		NodeId fWell = l3.MainWindow().Floating.back().FloatChild->Id;
		InsertTab(l3, fWell, "X", -1, true);
		const DockNode* xw = FindNode(l3, FindNode(l3, w)->Id);
		h.Check(xw && FindTabIndex(*xw, "X") < 0, "singleton removed from original well");
		const DockNode* fwn = FindNode(l3, fWell);
		h.Check(fwn && FindTabIndex(*fwn, "X") >= 0, "singleton present in floating well");
		h.Check(ValidateLayout(l3, { "X" }, v), "singleton layout validates");
		(void)floatId;
		// SetSplitRatio two-pane semantics
		DockLayout l4 = MakeDefaultLayout({ "A", "B", "C" });
		NodeId w4 = l4.MainWindow().Main.Id;
		SplitWell(l4, w4, Edge::East, "D", 0.5f);
		NodeId split = l4.MainWindow().Main.Id;
		// split of A|B and D: children [A|B well, D well]
		float before0 = l4.MainWindow().Main.Ratios[0];
		float before1 = l4.MainWindow().Main.Ratios[1];
		SetSplitRatio(l4, split, 0, 0.25f);
		h.Check(std::abs(l4.MainWindow().Main.Ratios[0] - 0.25f) < 1e-3f, "SetSplitRatio set child 0 to 0.25");
		h.Check(std::abs(l4.MainWindow().Main.Ratios[1] - (1.0f - 0.25f)) < 1e-3f, "SetSplitRatio neighbour got the rest");
		(void)before0; (void)before1;
		// FloatPanel then DockFloating round trip
		DockLayout l5 = MakeDefaultLayout({ "A", "B" });
		NodeId w5 = l5.MainWindow().Main.Id;
		FloatPanel(l5, w5, FindTabIndex(*FindNode(l5, w5), "B"), { 50, 50 }, { 300, 200 });
		NodeId fid = l5.MainWindow().Floating.back().Id;
		DropTarget t;
		t.KindId = DropTarget::Kind::WellZone;
		t.Well = w5;
		t.ZoneId = Zone::Center;
		DockLayout l6 = DockFloating(l5, fid, t);
		const DockNode* w5n = FindNode(l6, w5);
		h.Check(w5n && w5n->Tabs.size() == 2, "DockFloating merged tabs back into the well");
		h.Check(l6.MainWindow().Floating.empty(), "floating list emptied after re-dock");
	}

static void TestSolver(DockTestHarness& h)
{
	// Hand-computed: two panes, 1000px avail, 5px divider, ratios 0.7/0.3, no mins
	{
		std::vector<float> sizes = SolveSizes({ 0.7f, 0.3f }, 1000.0f, { 0.0f, 0.0f }, 5.0f);
		float avail = 1000.0f - 5.0f;
		h.Check(std::abs(sizes[0] - 0.7f * avail) < 1e-3f, "solver: 0.7 share");
		h.Check(std::abs(sizes[1] - 0.3f * avail) < 1e-3f, "solver: 0.3 share");
	}
	// Min clamp lifts a pane and borrows from the other
	{
		std::vector<float> sizes = SolveSizes({ 0.5f, 0.5f }, 300.0f, { 200.0f, 0.0f }, 0.0f);
		h.Check(sizes[0] >= 199.9f, "solver: min lifted pane 0");
		h.Check(std::abs((sizes[0] + sizes[1]) - 300.0f) < 1e-3f, "solver: sum preserved under clamp");
	}
	// Below sum-of-mins -> proportional-to-min
	{
		std::vector<float> sizes = SolveSizes({ 0.7f, 0.3f }, 100.0f, { 60.0f, 40.0f }, 0.0f);
		h.Check(std::abs(sizes[0] - 60.0f) < 1e-3f && std::abs(sizes[1] - 40.0f) < 1e-3f,
			"solver: proportional-to-min below sum-of-mins");
	}
	// ComputeNodeRects on a 3-level layout
	{
		DockLayout l = MakeDefaultLayout({ "A", "B" });
		NodeId w = l.MainWindow().Main.Id;
		SplitWell(l, w, Edge::East, "C", 0.5f);
		NodeId cWell = 0;
		for (NodeId wid : AllWellIds(l))
		{
			const DockNode* n = FindNode(l, wid);
			if (FindTabIndex(*n, "C") >= 0) { cWell = wid; break; }
		}
		SplitWell(l, cWell, Edge::South, "D", 0.5f);
		MinSizeFn mins = [](NodeId) { return glm::vec2(0.0f, 0.0f); };
		auto rects = ComputeNodeRects(l, l.MainWindow().Id, { 800.0f, 600.0f }, mins, 5.0f);
		h.Check(rects.size() == 3, "solver: 3 well rects");
		// find C's rect (right half)
		const NodeRect* cr = nullptr;
		for (const auto& r : rects)
			if (r.Id == cWell) cr = &r;
		h.Check(cr && std::abs(cr->Rect.x - 402.5f) < 1e-2f, "solver: C well at x=402.5 (400 + divider)");
		h.Check(cr && std::abs(cr->Rect.z - 397.5f) < 1e-2f, "solver: C well width = 397.5 (800/2 - 2.5)");
	}
}

static void TestApplyDrop(DockTestHarness& h)
{
	// Center zone = InsertTab
	{
		DockLayout l = MakeDefaultLayout({ "A" });
		DropTarget t;
		t.KindId = DropTarget::Kind::WellZone;
		t.Well = l.MainWindow().Main.Id;
		t.ZoneId = Zone::Center;
		DropPayload p;
		p.Panel = "B";
		DockLayout r = ApplyDrop(l, l.MainWindow().Id, t, p);
		h.Check(r.MainWindow().Main.Tabs.size() == 2, "ApplyDrop center inserted tab");
		h.Check(l.MainWindow().Main.Tabs.size() == 1, "dry-run copy untouched");
	}
	// East zone = SplitWell, sibling semantics
	{
		DockLayout l = MakeDefaultLayout({ "A", "B" });
		NodeId w = l.MainWindow().Main.Id;
		SplitWell(l, w, Edge::East, "C", 0.5f);
		NodeId aWell = 0;
		for (NodeId wid : AllWellIds(l))
		{
			const DockNode* n = FindNode(l, wid);
			if (FindTabIndex(*n, "A") >= 0) { aWell = wid; break; }
		}
		DropTarget t;
		t.KindId = DropTarget::Kind::WellZone;
		t.Well = aWell;
		t.ZoneId = Zone::East;
		DropPayload p;
		p.Panel = "D";
		DockLayout r = ApplyDrop(l, l.MainWindow().Id, t, p);
		h.Check(r.MainWindow().Main.Kind == DockNode::Kind::Split && r.MainWindow().Main.Children.size() == 3,
			"ApplyDrop east flattened to 3 siblings");
	}
	// Miss = FloatPanel
	{
		DockLayout l = MakeDefaultLayout({ "A" });
		DropTarget t;
		t.KindId = DropTarget::Kind::Miss;
		t.MissPos = { 300.0f, 200.0f };
		DropPayload p;
		p.Panel = "B";
		DockLayout r = ApplyDrop(l, l.MainWindow().Id, t, p);
		h.Check(r.MainWindow().Floating.size() == 1, "ApplyDrop miss floated the panel");
		h.Check(r.MainWindow().Main.Tabs.size() == 1, "source tab removed from the well");
	}
	// Floating source merge on center
	{
		DockLayout l = MakeDefaultLayout({ "A", "B" });
		NodeId w = l.MainWindow().Main.Id;
		FloatPanel(l, w, 1, { 50, 50 });
		NodeId fid = l.MainWindow().Floating.back().Id;
		DropTarget t;
		t.KindId = DropTarget::Kind::WellZone;
		t.Well = w;
		t.ZoneId = Zone::Center;
		DropPayload p;
		p.SourceFloating = fid;
		DockLayout r = ApplyDrop(l, l.MainWindow().Id, t, p);
		h.Check(r.MainWindow().Floating.empty(), "floating source removed on drop");
		h.Check(r.MainWindow().Main.Tabs.size() == 2, "floating tabs merged back");
	}
}

static void TestFloatingDropGuard(DockTestHarness& h)
{
	// Dropping a floating panel onto a well inside itself must be a no-op.
	DockLayout l = MakeDefaultLayout({ "A", "B" });
	NodeId w = l.MainWindow().Main.Id;
	FloatPanel(l, w, 1, { 50, 50 }, { 300, 200 });
	NodeId fid = l.MainWindow().Floating.back().Id;
	NodeId innerWell = l.MainWindow().Floating.back().FloatChild->Id;

	DropTarget t;
	t.KindId = DropTarget::Kind::WellZone;
	t.Well = innerWell;
	t.ZoneId = Zone::Center;
	DropPayload p;
	p.Panel = "B";
	p.SourceFloating = fid;
	DockLayout after = ApplyDrop(l, l.MainWindow().Id, t, p);
	h.Check(after.MainWindow().Floating.size() == 1, "self-drop kept the floating panel");
	h.Check(LayoutsEquivalentIgnoringIds(after, l), "self-drop leaves the layout unchanged");
	std::vector<InvariantViolation> v;
	h.Check(ValidateLayout(after, {}, v), "self-drop layout validates");

	// Dropping the floating onto the docked well merges and removes it.
	DropTarget t2;
	t2.KindId = DropTarget::Kind::WellZone;
	t2.Well = w;
	t2.ZoneId = Zone::Center;
	DockLayout after2 = ApplyDrop(l, l.MainWindow().Id, t2, p);
	h.Check(after2.MainWindow().Floating.empty(), "drop onto docked well removed the floating node");
	const DockNode* wn = FindNode(after2, w);
	h.Check(wn && wn->Tabs.size() == 2, "floating tab merged into the docked well");

	// RemoveFloating
	DockLayout l3 = MakeDefaultLayout({ "A", "B" });
	FloatPanel(l3, l3.MainWindow().Main.Id, 1, { 10, 10 });
	NodeId f3 = l3.MainWindow().Floating.back().Id;
	RemoveFloating(l3, f3);
	h.Check(l3.MainWindow().Floating.empty(), "RemoveFloating emptied the floating list");
	h.Check(ValidateLayout(l3, {}, v), "post-remove layout validates");
}

static void TestSerialization(DockTestHarness& h)
{
	DockLayout l = MakeDefaultLayout({ "A", "B" });
	NodeId w = l.MainWindow().Main.Id;
	SplitWell(l, w, Edge::East, "C", 0.4f);
	FloatPanel(l, w, FindTabIndex(*FindNode(l, w), "B"), { 10, 20 }, { 300, 200 });

	std::string json = SerializeLayout(l);
	DockLayout l2;
	std::vector<std::string> warnings;
	bool ok = ParseLayout(json, l2,
		[](const PanelId&) { return true; },
		[&](const std::string& s) { warnings.push_back(s); });
	h.Check(ok, "round-trip parses");
	h.Check(LayoutsEquivalentIgnoringIds(l, l2), "round-trip deep-equal (ignoring NodeIds)");
	std::vector<InvariantViolation> v;
	h.Check(ValidateLayout(l2, {}, v), "round-tripped layout validates");

	// v2 file is version 2 with a windows array
	h.Check(json.find("\"version\": 2") != std::string::npos, "serialized as version 2");
	h.Check(json.find("\"windows\"") != std::string::npos, "serialized with windows array");

	// Unknown panel ids dropped with warning (v2 shape)
	DockLayout l3;
	ok = ParseLayout(R"({"version":2,"windows":[{"bMain":true,"size":[1280,720],"main":{"kind":"tabwell","active":0,"tabs":["Known","Ghost"]},"floating":[]}]})",
		l3,
		[](const PanelId& id) { return id == "Known"; },
		[&](const std::string& s) { warnings.push_back(s); });
	h.Check(ok, "unknown-id layout parses");
	h.Check(l3.MainWindow().Main.Tabs.size() == 1 && l3.MainWindow().Main.Tabs[0].Id == "Known", "unknown id dropped");
	h.Check(!warnings.empty(), "unknown id produced a warning");

	// Corrupt JSON fails
	DockLayout l4;
	h.Check(!ParseLayout("{not json", l4, [](const PanelId&) { return true; },
		[&](const std::string&) {}), "corrupt json rejected");

	// Newer version fails
	DockLayout l5;
	h.Check(!ParseLayout(R"({"version":3,"windows":[]})", l5,
		[](const PanelId&) { return true; }, [&](const std::string&) {}), "newer version rejected");

	// Empty wells collapse on load (post-load normalization)
	DockLayout l6;
	ok = ParseLayout(R"({"version":2,"windows":[{"bMain":true,"size":[1280,720],"main":{"kind":"split","axis":"H","ratios":[0.5,0.5],"children":[{"kind":"tabwell","active":0,"tabs":["A"]},{"kind":"tabwell","active":0,"tabs":[]}]},"floating":[]}]})",
		l6, [](const PanelId&) { return true; }, [&](const std::string&) {});
	h.Check(ok, "split-with-empty-well parses");
	h.Check(l6.MainWindow().Main.Kind == DockNode::Kind::TabWell && l6.MainWindow().Main.Tabs.size() == 1,
		"post-load normalization collapsed the empty well");

	// v1 migration: a v1 file wraps its tree as the single main window.
	DockLayout l7;
	ok = ParseLayout(R"({"version":1,"main":{"kind":"split","axis":"H","ratios":[0.5,0.5],"children":[{"kind":"tabwell","active":0,"tabs":["A"]},{"kind":"tabwell","active":0,"tabs":["B"]}]},"floating":[]})",
		l7, [](const PanelId&) { return true; }, [&](const std::string&) {});
	h.Check(ok, "v1 layout migrates");
	h.Check(l7.Windows.size() == 1 && l7.Windows[0].bMain, "v1 wrapped into a single main window");
	h.Check(l7.MainWindow().Main.Kind == DockNode::Kind::Split && l7.MainWindow().Main.Children.size() == 2,
		"v1 tree preserved through migration");
	h.Check(ValidateLayout(l7, {}, v), "migrated layout validates");
}

static void TestWindows(DockTestHarness& h)
{
	std::vector<InvariantViolation> v;

	// OpenWindow: tab leaves its well and lands in a new non-main window.
	{
		DockLayout l = MakeDefaultLayout({ "A", "B" });
		NodeId w = l.MainWindow().Main.Id;
		NodeId winId = OpenWindow(l, w, 1);
		h.Check(winId != 0, "OpenWindow returned a window id");
		h.Check(l.Windows.size() == 2, "OpenWindow added a window");
		h.Check(!l.Windows[1].bMain && l.Windows[0].bMain, "main stays [0], new window is non-main");
		h.Check(FindTabIndex(l.MainWindow().Main, "B") < 0, "B left the main well");
		const WindowNode* nw = FindWindowContainingNode(l, FindWellContaining(l, "B"));
		h.Check(nw && !nw->bMain, "B's window is the non-main window");
		h.Check(ValidateLayout(l, {}, v), "post-open layout validates");
	}

	// Invariant 11: a window emptied of panels is removed.
	{
		DockLayout l = MakeDefaultLayout({ "A", "B" });
		NodeId w = l.MainWindow().Main.Id;
		NodeId winId = OpenWindow(l, w, 1);
		NodeId nwWell = FindWellContaining(l, "B");
		MovePanel(l, nwWell, 0, w);
		h.Check(l.Windows.size() == 1, "empty non-main window removed after its last tab moved out");
		(void)winId;
		h.Check(ValidateLayout(l, {}, v), "post-drain layout validates");
	}

	// CloseWindow(PerPanel) drops the window and its panels.
	{
		DockLayout l = MakeDefaultLayout({ "A", "B" });
		NodeId w = l.MainWindow().Main.Id;
		NodeId winId = OpenWindow(l, w, 1);
		h.Check(CloseWindow(l, winId, ClosePolicy::PerPanel), "CloseWindow(PerPanel) succeeded");
		h.Check(l.Windows.size() == 1, "window removed");
		h.Check(FindWellContaining(l, "B") == 0, "panel dropped with the window");
		h.Check(ValidateLayout(l, {}, v), "post-close layout validates");
	}

	// CloseWindow(MergeToMain) appends tabs to the main window's default well.
	{
		DockLayout l = MakeDefaultLayout({ "A", "B" });
		NodeId w = l.MainWindow().Main.Id;
		NodeId winId = OpenWindow(l, w, 1);
		h.Check(CloseWindow(l, winId, ClosePolicy::MergeToMain), "CloseWindow(MergeToMain) succeeded");
		h.Check(l.Windows.size() == 1, "window removed after merge");
		h.Check(FindTabIndex(l.MainWindow().Main, "B") >= 0, "B merged back into the main well");
		h.Check(ValidateLayout(l, {}, v), "post-merge layout validates");
	}

	// The main window is never closed (invariant 10).
	{
		DockLayout l = MakeDefaultLayout({ "A" });
		NodeId mainId = l.MainWindow().Id;
		h.Check(!CloseWindow(l, mainId, ClosePolicy::PerPanel), "CloseWindow on main is a no-op");
		h.Check(l.Windows.size() == 1 && l.Windows[0].Id == mainId, "main window survives");
	}

	// MoveToWindow: the command and the drop are the same transform.
	{
		DockLayout l = MakeDefaultLayout({ "A", "B" });
		NodeId mainWell = l.MainWindow().Main.Id;
		NodeId winId = OpenWindow(l, mainWell, 1);
		NodeId nwWell = FindWellContaining(l, "B");
		DropTarget t;
		t.KindId = DropTarget::Kind::WellZone;
		t.Well = mainWell;
		t.ZoneId = Zone::Center;
		DockLayout r = MoveToWindow(l, nwWell, 0, l.MainWindow().Id, t);
		h.Check(r.Windows.size() == 1, "moving the last tab out closes the window");
		h.Check(FindTabIndex(r.MainWindow().Main, "B") >= 0, "B landed in the target window's well");
		h.Check(ValidateLayout(r, {}, v), "post-move layout validates");
	}

	// Singleton uniqueness across windows (invariant 12).
	{
		DockLayout l = MakeDefaultLayout({ "A", "B" });
		NodeId mainWell = l.MainWindow().Main.Id;
		NodeId winId = OpenWindow(l, mainWell, 1);
		NodeId nwWell = FindWellContaining(l, "B");
		InsertTab(l, nwWell, "A", -1, true);
		h.Check(FindTabIndex(l.MainWindow().Main, "A") < 0, "singleton removed from the main window");
		h.Check(FindTabIndex(*FindNode(l, nwWell), "A") >= 0, "singleton lives in the other window");
		h.Check(ValidateLayout(l, { "A" }, v), "cross-window singleton layout validates");
		(void)winId;
	}

	// Ser/de with positions: includePositions=false omits pos (invariant 13).
	{
		DockLayout l = MakeDefaultLayout({ "A", "B" });
		NodeId mainWell = l.MainWindow().Main.Id;
		OpenWindow(l, mainWell, 1);
		l.Windows[1].Pos = glm::vec2(100.0f, 50.0f);

		std::string withPos = SerializeLayout(l, false, true);
		h.Check(withPos.find("\"pos\"") != std::string::npos, "positions serialized when allowed");
		std::string withoutPos = SerializeLayout(l, false, false);
		h.Check(withoutPos.find("\"pos\"") == std::string::npos, "positions omitted when platform cannot place windows");

		DockLayout l2;
		h.Check(ParseLayout(withPos, l2, [](const PanelId&) { return true; }, [](const std::string&) {}), "with-pos round-trip parses");
		h.Check(LayoutsEquivalentIgnoringIds(l, l2), "with-pos round-trip deep-equal");

		DockLayout l3;
		h.Check(ParseLayout(withoutPos, l3, [](const PanelId&) { return true; }, [](const std::string&) {}), "without-pos round-trip parses");
		h.Check(!l3.Windows[1].Pos.has_value(), "omitted pos reads back as nullopt");
		h.Check(ValidateLayout(l3, {}, v), "without-pos layout validates");
	}

	// Hand-edited two bMains: normalize demotes all but the first (invariant 10).
	{
		DockLayout l = MakeDefaultLayout({ "A" });
		NodeId mainWell = l.MainWindow().Main.Id;
		OpenWindow(l, mainWell, 0);
		l.Windows[1].bMain = true;
		NormalizeLayout(l);
		int mains = 0;
		for (const WindowNode& w : l.Windows) if (w.bMain) mains++;
		h.Check(mains == 1 && l.Windows[0].bMain, "duplicate bMain demoted, first window stays main");
		h.Check(ValidateLayout(l, {}, v), "post-demote layout validates");
	}

	// v1 layout migrates to an identical single-window v2 model (explicit
	// equivalence: the migrated model must deep-equal the same tree built
	// through v2 transforms).
	{
		DockLayout v1 = MakeDefaultLayout({ "A", "B" });
		NodeId w = v1.MainWindow().Main.Id;
		SplitWell(v1, w, Edge::East, "C", 0.5f);

		const char* v1json =
			R"({"version":1,"main":{"kind":"split","axis":"H","ratios":[0.5,0.5],"children":[{"kind":"tabwell","active":0,"tabs":["A","B"]},{"kind":"tabwell","active":0,"tabs":["C"]}]},"floating":[]})";
		DockLayout migrated;
		bool ok = ParseLayout(v1json, migrated, [](const PanelId&) { return true; }, [](const std::string&) {});
		h.Check(ok, "v1-shaped json parses");
		h.Check(LayoutsEquivalentIgnoringIds(v1, migrated), "v1 migration produces an identical single-window v2 model");
	}
}

static void TestFuzz(DockTestHarness& h)
{
	std::vector<PanelId> panels = { "A", "B", "C", "D" };
	std::set<PanelId> singletons = { "A" };
	DockLayout l = MakeDefaultLayout({ "A", "B" });
	Lcg rng;

	std::function<void(DockLayout&, uint32_t)> step = [&](DockLayout& lay, uint32_t op) {
		auto wells = AllWellIds(lay);
		auto splits = AllSplitIds(lay);
		(void)splits;
		switch (op % 17)
		{
		case 0:
			if (!wells.empty())
			{
				PanelId id = panels[rng.RandBelow((uint32_t)panels.size())];
				InsertTab(lay, wells[rng.RandBelow((uint32_t)wells.size())], id, -1, id == "A");
			}
			break;
		case 1:
			if (!wells.empty())
			{
				NodeId w = wells[rng.RandBelow((uint32_t)wells.size())];
				const DockNode* n = FindNode(lay, w);
				if (n && !n->Tabs.empty())
					RemoveTab(lay, w, (int)rng.RandBelow((uint32_t)n->Tabs.size()));
			}
			break;
		case 2:
			if (!wells.empty())
			{
				PanelId id = panels[rng.RandBelow((uint32_t)panels.size())];
				Edge e = (Edge)rng.RandBelow(4);
				SplitWell(lay, wells[rng.RandBelow((uint32_t)wells.size())], e, id,
					0.2f + rng.Unit() * 0.6f, id == "A");
			}
			break;
		case 3:
			if (!wells.empty())
			{
				NodeId w = wells[rng.RandBelow((uint32_t)wells.size())];
				const DockNode* n = FindNode(lay, w);
				if (n && !n->Tabs.empty())
					FloatPanel(lay, w, (int)rng.RandBelow((uint32_t)n->Tabs.size()),
						{ rng.Unit() * 1000.0f, rng.Unit() * 700.0f },
						{ 200.0f + rng.Unit() * 400.0f, 150.0f + rng.Unit() * 300.0f });
			}
			break;
		case 4:
			if (!lay.MainWindow().Floating.empty() && !wells.empty())
			{
				NodeId fid = lay.MainWindow().Floating[rng.RandBelow((uint32_t)lay.MainWindow().Floating.size())].Id;
				DropTarget t;
				t.KindId = DropTarget::Kind::WellZone;
				t.Well = wells[rng.RandBelow((uint32_t)wells.size())];
				t.ZoneId = (Zone)rng.RandBelow(5);
				lay = DockFloating(lay, fid, t);
			}
			break;
		case 5:
			if (!wells.empty())
			{
				NodeId w = wells[rng.RandBelow((uint32_t)wells.size())];
				const DockNode* n = FindNode(lay, w);
				if (n && !n->Tabs.empty())
					SetActiveTab(lay, w, (int)rng.RandBelow((uint32_t)n->Tabs.size()));
			}
			break;
		case 6:
			{
				auto s = AllSplitIds(lay);
				if (!s.empty())
				{
					NodeId sid = s[rng.RandBelow((uint32_t)s.size())];
					const DockNode* n = FindNode(lay, sid);
					if (n && n->Children.size() >= 2)
						SetSplitRatio(lay, sid, (int)rng.RandBelow((uint32_t)(n->Children.size() - 1)),
							0.1f + rng.Unit() * 0.8f);
				}
			}
			break;
		case 7:
			if (wells.size() >= 2)
			{
				NodeId from = wells[rng.RandBelow((uint32_t)wells.size())];
				NodeId to = wells[rng.RandBelow((uint32_t)wells.size())];
				const DockNode* fn = FindNode(lay, from);
				if (fn && !fn->Tabs.empty())
					MovePanel(lay, from, (int)rng.RandBelow((uint32_t)fn->Tabs.size()), to, -1);
			}
			break;
		case 8:
			if (!lay.MainWindow().Floating.empty())
				MoveFloating(lay, lay.MainWindow().Floating[rng.RandBelow((uint32_t)lay.MainWindow().Floating.size())].Id,
					{ rng.Unit() * 1000.0f, rng.Unit() * 700.0f });
			break;
		case 9:
			if (!lay.MainWindow().Floating.empty())
				ResizeFloating(lay, lay.MainWindow().Floating[rng.RandBelow((uint32_t)lay.MainWindow().Floating.size())].Id,
					{ 100.0f + rng.Unit() * 500.0f, 80.0f + rng.Unit() * 400.0f });
			break;
		case 10:
			if (!lay.MainWindow().Floating.empty())
				BringFloatingToFront(lay, lay.MainWindow().Floating[rng.RandBelow((uint32_t)lay.MainWindow().Floating.size())].Id);
			break;
		case 11:
		{
			// Miss drop against a RANDOM target window (cross-window float).
			DropTarget t;
			t.KindId = DropTarget::Kind::Miss;
			t.MissPos = { rng.Unit() * 1000.0f, rng.Unit() * 700.0f };
			DropPayload p;
			p.Panel = panels[rng.RandBelow((uint32_t)panels.size())];
			p.SourceWell = 0;
			p.Singleton = (p.Panel == "A");
			NodeId tw = lay.Windows[rng.RandBelow((uint32_t)lay.Windows.size())].Id;
			lay = ApplyDrop(lay, tw, t, p);
			break;
		}
		case 12:
			if (!lay.MainWindow().Floating.empty())
				RemoveFloating(lay, lay.MainWindow().Floating[rng.RandBelow((uint32_t)lay.MainWindow().Floating.size())].Id);
			break;
		case 13:
		{
			// OpenWindow: tear a random tab into a new OS window.
			if (!wells.empty())
			{
				NodeId w = wells[rng.RandBelow((uint32_t)wells.size())];
				const DockNode* n = FindNode(lay, w);
				if (n && n->Tabs.size() >= 1)
					OpenWindow(lay, w, (int)rng.RandBelow((uint32_t)n->Tabs.size()));
			}
			break;
		}
		case 14:
		{
			// MoveToWindow across a random window pair.
			if (lay.Windows.size() >= 1 && !wells.empty())
			{
				NodeId from = wells[rng.RandBelow((uint32_t)wells.size())];
				const DockNode* fn = FindNode(lay, from);
				const WindowNode& tw = lay.Windows[rng.RandBelow((uint32_t)lay.Windows.size())];
				NodeId twFirstWell = 0;
				if (tw.Main.Kind == DockNode::Kind::TabWell) twFirstWell = tw.Main.Id;
				else if (const DockNode* fw = FindFirstWell(const_cast<DockNode&>(tw.Main))) twFirstWell = fw->Id;
				if (fn && !fn->Tabs.empty() && twFirstWell)
				{
					DropTarget t;
					t.KindId = DropTarget::Kind::WellZone;
					t.Well = twFirstWell;
					t.ZoneId = Zone::Center;
					lay = MoveToWindow(lay, from, (int)rng.RandBelow((uint32_t)fn->Tabs.size()), tw.Id, t);
				}
			}
			break;
		}
		case 15:
		{
			// CloseWindow on a random non-main window, random policy.
			std::vector<NodeId> nonMain;
			for (const WindowNode& w : lay.Windows)
				if (!w.bMain) nonMain.push_back(w.Id);
			if (!nonMain.empty())
				CloseWindow(lay, nonMain[rng.RandBelow((uint32_t)nonMain.size())],
					(rng.RandBelow(2) == 0) ? ClosePolicy::PerPanel : ClosePolicy::MergeToMain);
			break;
		}
		case 16:
		{
			// Cross-window well-zone drop via ApplyDrop directly.
			if (!wells.empty())
			{
				DropTarget t;
				t.KindId = DropTarget::Kind::WellZone;
				t.Well = wells[rng.RandBelow((uint32_t)wells.size())];
				t.ZoneId = (Zone)rng.RandBelow(5);
				DropPayload p;
				p.Panel = panels[rng.RandBelow((uint32_t)panels.size())];
				p.SourceWell = 0;
				p.Singleton = (p.Panel == "A");
				NodeId tw = lay.Windows[rng.RandBelow((uint32_t)lay.Windows.size())].Id;
				lay = ApplyDrop(lay, tw, t, p);
			}
			break;
		}
		}
	};

	for (int op = 0; op < 1000; op++)
	{
		step(l, rng.Next());
		std::vector<InvariantViolation> v;
		if (!ValidateLayout(l, singletons, v))
		{
			h.Check(false, ("fuzz op " + std::to_string(op) + ": " + v[0].What).c_str());
			LogDockTest("fuzz layout at failure: " + SerializeLayout(l, false));
			break;
		}
		// Ser/de round-trip every 100 ops, continue on the deserialized copy
		if ((op + 1) % 100 == 0)
		{
			std::string json = SerializeLayout(l);
			DockLayout l2;
			if (!ParseLayout(json, l2, [](const PanelId&) { return true; }, [](const std::string&) {}))
			{
				h.Check(false, "fuzz ser/de parse failed");
				break;
			}
			h.Check(LayoutsEquivalentIgnoringIds(l, l2), "fuzz ser/de round-trip equivalent");
			std::vector<InvariantViolation> v2;
			h.Check(ValidateLayout(l2, singletons, v2), "fuzz deserialized layout validates");
			l = std::move(l2);
		}
	}
}

static int RunDockModelTests()
{
	DockTestHarness h;
	TestNormalization(h);
	TestTransforms(h);
	TestFloatingDropGuard(h);
	TestSolver(h);
	TestApplyDrop(h);
	TestSerialization(h);
	TestWindows(h);
	TestFuzz(h);

	LogDockTest("SUMMARY pass=" + std::to_string(h.Pass) + " fail=" + std::to_string(h.Fail));
	for (const auto& f : h.Failures)
		LogDockTest("FAIL: " + f);
	std::fprintf(stderr, "DOCK MODEL TESTS: %d pass / %d fail\n", h.Pass, h.Fail);
	return h.Fail;
}

}} // namespace wisp::dock
