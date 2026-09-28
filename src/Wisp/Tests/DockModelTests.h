#pragma once

// DockModel headless tests: layout normalization, transforms, the size
// solver, drop semantics, floating-drop guards, serialization (+ v1
// migration), multi-window invariants, and the seeded fuzz loop. Migrated
// to doctest: one TEST_CASE per named check; scenario helpers rebuild the
// layout for each case. Runs from tinytest.

#include "Wisp/Docking/DockModel.h"
#include <doctest/doctest.h>

#include <cmath>
#include <cstdio>
#include <cstring>
#include <functional>
#include <set>
#include <string>
#include <vector>

namespace wisp { namespace dock {

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

namespace {

// ---- TestNormalization ----

struct NormalizationResults
{
	bool emptyDefaultValidates = false;
	bool rootPlaceholderWell = false;
	bool lastTabRemovalLeavesPlaceholder = false;
	bool placeholderValidates = false;
	bool rootBecameSplit = false;
	bool collapsedBackToWell = false;
	bool postCollapseValidates = false;
	bool flattenedTo3 = false;
	bool flattenedValidates = false;
	bool activeIndexClamped = false;
};

NormalizationResults RunNormalizationScenario()
{
	NormalizationResults r;
	// Root placeholder on empty default
	{
		DockLayout l = MakeDefaultLayout({});
		std::vector<InvariantViolation> v;
		r.emptyDefaultValidates = ValidateLayout(l, {}, v);
		r.rootPlaceholderWell = l.MainWindow().Main.Kind == DockNode::Kind::TabWell && l.MainWindow().Main.Tabs.empty();
	}
	// RemoveTab on the only tab -> placeholder survives
	{
		DockLayout l = MakeDefaultLayout({ "A" });
		NodeId well = l.MainWindow().Main.Id;
		RemoveTab(l, well, 0);
		r.lastTabRemovalLeavesPlaceholder = l.MainWindow().Main.Kind == DockNode::Kind::TabWell && l.MainWindow().Main.Tabs.empty();
		std::vector<InvariantViolation> v;
		r.placeholderValidates = ValidateLayout(l, {}, v);
	}
	// Split of a single well, then empty one half
	{
		DockLayout l = MakeDefaultLayout({ "A", "B" });
		NodeId well = l.MainWindow().Main.Id;
		SplitWell(l, well, Edge::East, "C", 0.5f);
		r.rootBecameSplit = l.MainWindow().Main.Kind == DockNode::Kind::Split;
		std::vector<NodeId> wells = AllWellIds(l);
		// find the well containing C
		NodeId cWell = 0;
		for (NodeId w : wells)
		{
			const DockNode* n = FindNode(l, w);
			if (FindTabIndex(*n, "C") >= 0) { cWell = w; break; }
		}
		RemoveTab(l, cWell, FindTabIndex(*FindNode(l, cWell), "C"));
		r.collapsedBackToWell = l.MainWindow().Main.Kind == DockNode::Kind::TabWell;
		std::vector<InvariantViolation> v;
		r.postCollapseValidates = ValidateLayout(l, {}, v);
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
		r.flattenedTo3 = l.MainWindow().Main.Kind == DockNode::Kind::Split && l.MainWindow().Main.Children.size() == 3;
		std::vector<InvariantViolation> v;
		r.flattenedValidates = ValidateLayout(l, {}, v);
	}
	// ActiveIndex clamp
	{
		DockLayout l = MakeDefaultLayout({ "A", "B" });
		l.MainWindow().Main.ActiveIndex = 99;
		NormalizeLayout(l);
		r.activeIndexClamped = l.MainWindow().Main.ActiveIndex == 1;
	}
	return r;
}

TEST_CASE("dock: normalization — empty default layout validates")
{
	CHECK_MESSAGE(RunNormalizationScenario().emptyDefaultValidates, "empty default layout validates");
}

TEST_CASE("dock: normalization — root placeholder is an empty well")
{
	CHECK_MESSAGE(RunNormalizationScenario().rootPlaceholderWell, "root placeholder is an empty well");
}

TEST_CASE("dock: normalization — removing last root tab leaves placeholder")
{
	CHECK_MESSAGE(RunNormalizationScenario().lastTabRemovalLeavesPlaceholder, "removing last root tab leaves placeholder");
}

TEST_CASE("dock: normalization — placeholder layout validates")
{
	CHECK_MESSAGE(RunNormalizationScenario().placeholderValidates, "placeholder layout validates");
}

TEST_CASE("dock: normalization — root became a split")
{
	CHECK_MESSAGE(RunNormalizationScenario().rootBecameSplit, "root became a split");
}

TEST_CASE("dock: normalization — single-child split collapsed back to a well")
{
	CHECK_MESSAGE(RunNormalizationScenario().collapsedBackToWell, "single-child split collapsed back to a well");
}

TEST_CASE("dock: normalization — post-collapse layout validates")
{
	CHECK_MESSAGE(RunNormalizationScenario().postCollapseValidates, "post-collapse layout validates");
}

TEST_CASE("dock: normalization — same-axis splits flattened to 3 siblings")
{
	CHECK_MESSAGE(RunNormalizationScenario().flattenedTo3, "same-axis splits flattened to 3 siblings");
}

TEST_CASE("dock: normalization — flattened layout validates")
{
	CHECK_MESSAGE(RunNormalizationScenario().flattenedValidates, "flattened layout validates");
}

TEST_CASE("dock: normalization — ActiveIndex clamped")
{
	CHECK_MESSAGE(RunNormalizationScenario().activeIndexClamped, "ActiveIndex clamped");
}

// ---- TestTransforms ----

struct TransformsResults
{
	bool reordersWithinWell = false;
	bool movedAcrossWells = false;
	bool postMoveValidates = false;
	bool singletonRemovedFromOriginal = false;
	bool singletonInFloating = false;
	bool singletonValidates = false;
	bool ratioChild0 = false;
	bool ratioNeighbour = false;
	bool dockFloatingMerged = false;
	bool floatingEmptied = false;
};

TransformsResults RunTransformsScenario()
{
	TransformsResults r;
	// MovePanel reorder within one well
	DockLayout l = MakeDefaultLayout({ "A", "B", "C" });
	NodeId well = l.MainWindow().Main.Id;
	MovePanel(l, well, 0, well, 2);
	r.reordersWithinWell = FindTabIndex(l.MainWindow().Main, "A") == 2 && FindTabIndex(l.MainWindow().Main, "B") == 0;
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
	r.movedAcrossWells = cw && FindTabIndex(*cw, "A") >= 0;
	std::vector<InvariantViolation> v;
	r.postMoveValidates = ValidateLayout(l2, {}, v);
	// Singleton enforcement across Main + Floating
	DockLayout l3 = MakeDefaultLayout({ "X", "Y" });
	NodeId w = l3.MainWindow().Main.Id;
	FloatPanel(l3, w, FindTabIndex(*FindNode(l3, w), "Y"), { 10, 10 });
	// insert singleton X into the floating well
	NodeId fWell = l3.MainWindow().Floating.back().FloatChild->Id;
	InsertTab(l3, fWell, "X", -1, true);
	const DockNode* xw = FindNode(l3, FindNode(l3, w)->Id);
	r.singletonRemovedFromOriginal = xw && FindTabIndex(*xw, "X") < 0;
	const DockNode* fwn = FindNode(l3, fWell);
	r.singletonInFloating = fwn && FindTabIndex(*fwn, "X") >= 0;
	r.singletonValidates = ValidateLayout(l3, { "X" }, v);
	// SetSplitRatio two-pane semantics
	DockLayout l4 = MakeDefaultLayout({ "A", "B", "C" });
	NodeId w4 = l4.MainWindow().Main.Id;
	SplitWell(l4, w4, Edge::East, "D", 0.5f);
	NodeId split = l4.MainWindow().Main.Id;
	// split of A|B and D: children [A|B well, D well]
	SetSplitRatio(l4, split, 0, 0.25f);
	r.ratioChild0 = std::abs(l4.MainWindow().Main.Ratios[0] - 0.25f) < 1e-3f;
	r.ratioNeighbour = std::abs(l4.MainWindow().Main.Ratios[1] - (1.0f - 0.25f)) < 1e-3f;
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
	r.dockFloatingMerged = w5n && w5n->Tabs.size() == 2;
	r.floatingEmptied = l6.MainWindow().Floating.empty();
	return r;
}

TEST_CASE("dock: transforms — MovePanel reorders within a well")
{
	CHECK_MESSAGE(RunTransformsScenario().reordersWithinWell, "MovePanel reorders within a well");
}

TEST_CASE("dock: transforms — MovePanel moved tab across wells")
{
	CHECK_MESSAGE(RunTransformsScenario().movedAcrossWells, "MovePanel moved tab across wells");
}

TEST_CASE("dock: transforms — post-move layout validates")
{
	CHECK_MESSAGE(RunTransformsScenario().postMoveValidates, "post-move layout validates");
}

TEST_CASE("dock: transforms — singleton removed from original well")
{
	CHECK_MESSAGE(RunTransformsScenario().singletonRemovedFromOriginal, "singleton removed from original well");
}

TEST_CASE("dock: transforms — singleton present in floating well")
{
	CHECK_MESSAGE(RunTransformsScenario().singletonInFloating, "singleton present in floating well");
}

TEST_CASE("dock: transforms — singleton layout validates")
{
	CHECK_MESSAGE(RunTransformsScenario().singletonValidates, "singleton layout validates");
}

TEST_CASE("dock: transforms — SetSplitRatio set child 0 to 0.25")
{
	CHECK_MESSAGE(RunTransformsScenario().ratioChild0, "SetSplitRatio set child 0 to 0.25");
}

TEST_CASE("dock: transforms — SetSplitRatio neighbour got the rest")
{
	CHECK_MESSAGE(RunTransformsScenario().ratioNeighbour, "SetSplitRatio neighbour got the rest");
}

TEST_CASE("dock: transforms — DockFloating merged tabs back into the well")
{
	CHECK_MESSAGE(RunTransformsScenario().dockFloatingMerged, "DockFloating merged tabs back into the well");
}

TEST_CASE("dock: transforms — floating list emptied after re-dock")
{
	CHECK_MESSAGE(RunTransformsScenario().floatingEmptied, "floating list emptied after re-dock");
}

// ---- TestSolver ----

struct SolverResults
{
	bool share07 = false;
	bool share03 = false;
	bool minLifted = false;
	bool sumPreserved = false;
	bool proportionalToMin = false;
	bool threeRects = false;
	bool cAtX = false;
	bool cWidth = false;
};

SolverResults RunSolverScenario()
{
	SolverResults r;
	// Hand-computed: two panes, 1000px avail, 5px divider, ratios 0.7/0.3, no mins
	{
		std::vector<float> sizes = SolveSizes({ 0.7f, 0.3f }, 1000.0f, { 0.0f, 0.0f }, 5.0f);
		float avail = 1000.0f - 5.0f;
		r.share07 = std::abs(sizes[0] - 0.7f * avail) < 1e-3f;
		r.share03 = std::abs(sizes[1] - 0.3f * avail) < 1e-3f;
	}
	// Min clamp lifts a pane and borrows from the other
	{
		std::vector<float> sizes = SolveSizes({ 0.5f, 0.5f }, 300.0f, { 200.0f, 0.0f }, 0.0f);
		r.minLifted = sizes[0] >= 199.9f;
		r.sumPreserved = std::abs((sizes[0] + sizes[1]) - 300.0f) < 1e-3f;
	}
	// Below sum-of-mins -> proportional-to-min
	{
		std::vector<float> sizes = SolveSizes({ 0.7f, 0.3f }, 100.0f, { 60.0f, 40.0f }, 0.0f);
		r.proportionalToMin = std::abs(sizes[0] - 60.0f) < 1e-3f && std::abs(sizes[1] - 40.0f) < 1e-3f;
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
		r.threeRects = rects.size() == 3;
		// find C's rect (right half)
		const NodeRect* cr = nullptr;
		for (const auto& rr : rects)
			if (rr.Id == cWell) cr = &rr;
		r.cAtX = cr && std::abs(cr->Rect.x - 402.5f) < 1e-2f;
		r.cWidth = cr && std::abs(cr->Rect.z - 397.5f) < 1e-2f;
	}
	return r;
}

TEST_CASE("dock: solver — 0.7 share")
{
	CHECK_MESSAGE(RunSolverScenario().share07, "solver: 0.7 share");
}

TEST_CASE("dock: solver — 0.3 share")
{
	CHECK_MESSAGE(RunSolverScenario().share03, "solver: 0.3 share");
}

TEST_CASE("dock: solver — min lifted pane 0")
{
	CHECK_MESSAGE(RunSolverScenario().minLifted, "solver: min lifted pane 0");
}

TEST_CASE("dock: solver — sum preserved under clamp")
{
	CHECK_MESSAGE(RunSolverScenario().sumPreserved, "solver: sum preserved under clamp");
}

TEST_CASE("dock: solver — proportional-to-min below sum-of-mins")
{
	CHECK_MESSAGE(RunSolverScenario().proportionalToMin, "solver: proportional-to-min below sum-of-mins");
}

TEST_CASE("dock: solver — 3 well rects")
{
	CHECK_MESSAGE(RunSolverScenario().threeRects, "solver: 3 well rects");
}

TEST_CASE("dock: solver — C well at x=402.5 (400 + divider)")
{
	CHECK_MESSAGE(RunSolverScenario().cAtX, "solver: C well at x=402.5 (400 + divider)");
}

TEST_CASE("dock: solver — C well width = 397.5 (800/2 - 2.5)")
{
	CHECK_MESSAGE(RunSolverScenario().cWidth, "solver: C well width = 397.5 (800/2 - 2.5)");
}

// ---- TestApplyDrop ----

struct ApplyDropResults
{
	bool centerInserted = false;
	bool dryRunUntouched = false;
	bool eastFlattened = false;
	bool missFloated = false;
	bool sourceRemoved = false;
	bool floatingRemoved = false;
	bool floatingMerged = false;
};

ApplyDropResults RunApplyDropScenario()
{
	ApplyDropResults r;
	// Center zone = InsertTab
	{
		DockLayout l = MakeDefaultLayout({ "A" });
		DropTarget t;
		t.KindId = DropTarget::Kind::WellZone;
		t.Well = l.MainWindow().Main.Id;
		t.ZoneId = Zone::Center;
		DropPayload p;
		p.Panel = "B";
		DockLayout rr = ApplyDrop(l, l.MainWindow().Id, t, p);
		r.centerInserted = rr.MainWindow().Main.Tabs.size() == 2;
		r.dryRunUntouched = l.MainWindow().Main.Tabs.size() == 1;
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
		DockLayout rr = ApplyDrop(l, l.MainWindow().Id, t, p);
		r.eastFlattened = rr.MainWindow().Main.Kind == DockNode::Kind::Split && rr.MainWindow().Main.Children.size() == 3;
	}
	// Miss = FloatPanel
	{
		DockLayout l = MakeDefaultLayout({ "A" });
		DropTarget t;
		t.KindId = DropTarget::Kind::Miss;
		t.MissPos = { 300.0f, 200.0f };
		DropPayload p;
		p.Panel = "B";
		DockLayout rr = ApplyDrop(l, l.MainWindow().Id, t, p);
		r.missFloated = rr.MainWindow().Floating.size() == 1;
		r.sourceRemoved = rr.MainWindow().Main.Tabs.size() == 1;
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
		DockLayout rr = ApplyDrop(l, l.MainWindow().Id, t, p);
		r.floatingRemoved = rr.MainWindow().Floating.empty();
		r.floatingMerged = rr.MainWindow().Main.Tabs.size() == 2;
	}
	return r;
}

TEST_CASE("dock: apply-drop — ApplyDrop center inserted tab")
{
	CHECK_MESSAGE(RunApplyDropScenario().centerInserted, "ApplyDrop center inserted tab");
}

TEST_CASE("dock: apply-drop — dry-run copy untouched")
{
	CHECK_MESSAGE(RunApplyDropScenario().dryRunUntouched, "dry-run copy untouched");
}

TEST_CASE("dock: apply-drop — ApplyDrop east flattened to 3 siblings")
{
	CHECK_MESSAGE(RunApplyDropScenario().eastFlattened, "ApplyDrop east flattened to 3 siblings");
}

TEST_CASE("dock: apply-drop — ApplyDrop miss floated the panel")
{
	CHECK_MESSAGE(RunApplyDropScenario().missFloated, "ApplyDrop miss floated the panel");
}

TEST_CASE("dock: apply-drop — source tab removed from the well")
{
	CHECK_MESSAGE(RunApplyDropScenario().sourceRemoved, "source tab removed from the well");
}

TEST_CASE("dock: apply-drop — floating source removed on drop")
{
	CHECK_MESSAGE(RunApplyDropScenario().floatingRemoved, "floating source removed on drop");
}

TEST_CASE("dock: apply-drop — floating tabs merged back")
{
	CHECK_MESSAGE(RunApplyDropScenario().floatingMerged, "floating tabs merged back");
}

// ---- TestFloatingDropGuard ----

struct FloatingGuardResults
{
	bool selfDropKept = false;
	bool selfDropUnchanged = false;
	bool selfDropValidates = false;
	bool dropOnDockedRemoved = false;
	bool floatingMergedIntoDocked = false;
	bool removeFloatingEmptied = false;
	bool postRemoveValidates = false;
};

FloatingGuardResults RunFloatingGuardScenario()
{
	FloatingGuardResults r;
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
	r.selfDropKept = after.MainWindow().Floating.size() == 1;
	r.selfDropUnchanged = LayoutsEquivalentIgnoringIds(after, l);
	std::vector<InvariantViolation> v;
	r.selfDropValidates = ValidateLayout(after, {}, v);

	// Dropping the floating onto the docked well merges and removes it.
	DropTarget t2;
	t2.KindId = DropTarget::Kind::WellZone;
	t2.Well = w;
	t2.ZoneId = Zone::Center;
	DockLayout after2 = ApplyDrop(l, l.MainWindow().Id, t2, p);
	r.dropOnDockedRemoved = after2.MainWindow().Floating.empty();
	const DockNode* wn = FindNode(after2, w);
	r.floatingMergedIntoDocked = wn && wn->Tabs.size() == 2;

	// RemoveFloating
	DockLayout l3 = MakeDefaultLayout({ "A", "B" });
	FloatPanel(l3, l3.MainWindow().Main.Id, 1, { 10, 10 });
	NodeId f3 = l3.MainWindow().Floating.back().Id;
	RemoveFloating(l3, f3);
	r.removeFloatingEmptied = l3.MainWindow().Floating.empty();
	r.postRemoveValidates = ValidateLayout(l3, {}, v);
	return r;
}

TEST_CASE("dock: floating-guard — self-drop kept the floating panel")
{
	CHECK_MESSAGE(RunFloatingGuardScenario().selfDropKept, "self-drop kept the floating panel");
}

TEST_CASE("dock: floating-guard — self-drop leaves the layout unchanged")
{
	CHECK_MESSAGE(RunFloatingGuardScenario().selfDropUnchanged, "self-drop leaves the layout unchanged");
}

TEST_CASE("dock: floating-guard — self-drop layout validates")
{
	CHECK_MESSAGE(RunFloatingGuardScenario().selfDropValidates, "self-drop layout validates");
}

TEST_CASE("dock: floating-guard — drop onto docked well removed the floating node")
{
	CHECK_MESSAGE(RunFloatingGuardScenario().dropOnDockedRemoved, "drop onto docked well removed the floating node");
}

TEST_CASE("dock: floating-guard — floating tab merged into the docked well")
{
	CHECK_MESSAGE(RunFloatingGuardScenario().floatingMergedIntoDocked, "floating tab merged into the docked well");
}

TEST_CASE("dock: floating-guard — RemoveFloating emptied the floating list")
{
	CHECK_MESSAGE(RunFloatingGuardScenario().removeFloatingEmptied, "RemoveFloating emptied the floating list");
}

TEST_CASE("dock: floating-guard — post-remove layout validates")
{
	CHECK_MESSAGE(RunFloatingGuardScenario().postRemoveValidates, "post-remove layout validates");
}

// ---- TestSerialization ----

struct SerializationResults
{
	bool roundTripParses = false;
	bool roundTripDeepEqual = false;
	bool roundTripValidates = false;
	bool version2 = false;
	bool windowsArray = false;
	bool unknownIdParses = false;
	bool unknownIdDropped = false;
	bool unknownIdWarning = false;
	bool corruptRejected = false;
	bool newerRejected = false;
	bool splitEmptyWellParses = false;
	bool normalizationCollapsed = false;
	bool v1Migrates = false;
	bool v1SingleMainWindow = false;
	bool v1TreePreserved = false;
	bool migratedValidates = false;
};

SerializationResults RunSerializationScenario()
{
	SerializationResults r;
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
	r.roundTripParses = ok;
	r.roundTripDeepEqual = LayoutsEquivalentIgnoringIds(l, l2);
	std::vector<InvariantViolation> v;
	r.roundTripValidates = ValidateLayout(l2, {}, v);

	// v2 file is version 2 with a windows array
	r.version2 = json.find("\"version\": 2") != std::string::npos;
	r.windowsArray = json.find("\"windows\"") != std::string::npos;

	// Unknown panel ids dropped with warning (v2 shape)
	DockLayout l3;
	ok = ParseLayout(R"({"version":2,"windows":[{"bMain":true,"size":[1280,720],"main":{"kind":"tabwell","active":0,"tabs":["Known","Ghost"]},"floating":[]}]})",
		l3,
		[](const PanelId& id) { return id == "Known"; },
		[&](const std::string& s) { warnings.push_back(s); });
	r.unknownIdParses = ok;
	r.unknownIdDropped = l3.MainWindow().Main.Tabs.size() == 1 && l3.MainWindow().Main.Tabs[0].Id == "Known";
	r.unknownIdWarning = !warnings.empty();

	// Corrupt JSON fails
	DockLayout l4;
	r.corruptRejected = !ParseLayout("{not json", l4, [](const PanelId&) { return true; },
		[&](const std::string&) {});

	// Newer version fails
	DockLayout l5;
	r.newerRejected = !ParseLayout(R"({"version":3,"windows":[]})", l5,
		[](const PanelId&) { return true; }, [&](const std::string&) {});

	// Empty wells collapse on load (post-load normalization)
	DockLayout l6;
	ok = ParseLayout(R"({"version":2,"windows":[{"bMain":true,"size":[1280,720],"main":{"kind":"split","axis":"H","ratios":[0.5,0.5],"children":[{"kind":"tabwell","active":0,"tabs":["A"]},{"kind":"tabwell","active":0,"tabs":[]}]},"floating":[]}]})",
		l6, [](const PanelId&) { return true; }, [&](const std::string&) {});
	r.splitEmptyWellParses = ok;
	r.normalizationCollapsed = l6.MainWindow().Main.Kind == DockNode::Kind::TabWell && l6.MainWindow().Main.Tabs.size() == 1;

	// v1 migration: a v1 file wraps its tree as the single main window.
	DockLayout l7;
	ok = ParseLayout(R"({"version":1,"main":{"kind":"split","axis":"H","ratios":[0.5,0.5],"children":[{"kind":"tabwell","active":0,"tabs":["A"]},{"kind":"tabwell","active":0,"tabs":["B"]}]},"floating":[]})",
		l7, [](const PanelId&) { return true; }, [&](const std::string&) {});
	r.v1Migrates = ok;
	r.v1SingleMainWindow = l7.Windows.size() == 1 && l7.Windows[0].bMain;
	r.v1TreePreserved = l7.MainWindow().Main.Kind == DockNode::Kind::Split && l7.MainWindow().Main.Children.size() == 2;
	r.migratedValidates = ValidateLayout(l7, {}, v);
	return r;
}

TEST_CASE("dock: serialization — round-trip parses")
{
	CHECK_MESSAGE(RunSerializationScenario().roundTripParses, "round-trip parses");
}

TEST_CASE("dock: serialization — round-trip deep-equal (ignoring NodeIds)")
{
	CHECK_MESSAGE(RunSerializationScenario().roundTripDeepEqual, "round-trip deep-equal (ignoring NodeIds)");
}

TEST_CASE("dock: serialization — round-tripped layout validates")
{
	CHECK_MESSAGE(RunSerializationScenario().roundTripValidates, "round-tripped layout validates");
}

TEST_CASE("dock: serialization — serialized as version 2")
{
	CHECK_MESSAGE(RunSerializationScenario().version2, "serialized as version 2");
}

TEST_CASE("dock: serialization — serialized with windows array")
{
	CHECK_MESSAGE(RunSerializationScenario().windowsArray, "serialized with windows array");
}

TEST_CASE("dock: serialization — unknown-id layout parses")
{
	CHECK_MESSAGE(RunSerializationScenario().unknownIdParses, "unknown-id layout parses");
}

TEST_CASE("dock: serialization — unknown id dropped")
{
	CHECK_MESSAGE(RunSerializationScenario().unknownIdDropped, "unknown id dropped");
}

TEST_CASE("dock: serialization — unknown id produced a warning")
{
	CHECK_MESSAGE(RunSerializationScenario().unknownIdWarning, "unknown id produced a warning");
}

TEST_CASE("dock: serialization — corrupt json rejected")
{
	CHECK_MESSAGE(RunSerializationScenario().corruptRejected, "corrupt json rejected");
}

TEST_CASE("dock: serialization — newer version rejected")
{
	CHECK_MESSAGE(RunSerializationScenario().newerRejected, "newer version rejected");
}

TEST_CASE("dock: serialization — split-with-empty-well parses")
{
	CHECK_MESSAGE(RunSerializationScenario().splitEmptyWellParses, "split-with-empty-well parses");
}

TEST_CASE("dock: serialization — post-load normalization collapsed the empty well")
{
	CHECK_MESSAGE(RunSerializationScenario().normalizationCollapsed, "post-load normalization collapsed the empty well");
}

TEST_CASE("dock: serialization — v1 layout migrates")
{
	CHECK_MESSAGE(RunSerializationScenario().v1Migrates, "v1 layout migrates");
}

TEST_CASE("dock: serialization — v1 wrapped into a single main window")
{
	CHECK_MESSAGE(RunSerializationScenario().v1SingleMainWindow, "v1 wrapped into a single main window");
}

TEST_CASE("dock: serialization — v1 tree preserved through migration")
{
	CHECK_MESSAGE(RunSerializationScenario().v1TreePreserved, "v1 tree preserved through migration");
}

TEST_CASE("dock: serialization — migrated layout validates")
{
	CHECK_MESSAGE(RunSerializationScenario().migratedValidates, "migrated layout validates");
}

// ---- TestWindows ----

struct WindowsResults
{
	bool openReturnedId = false;
	bool openAddedWindow = false;
	bool mainStaysFirst = false;
	bool bLeftMain = false;
	bool bWindowIsNonMain = false;
	bool postOpenValidates = false;
	bool drainRemovedWindow = false;
	bool postDrainValidates = false;
	bool closePerPanel = false;
	bool windowRemoved = false;
	bool panelDropped = false;
	bool postCloseValidates = false;
	bool closeMerge = false;
	bool removedAfterMerge = false;
	bool mergedBack = false;
	bool postMergeValidates = false;
	bool closeMainNoop = false;
	bool mainSurvives = false;
	bool moveClosesWindow = false;
	bool bLanded = false;
	bool postMoveValidates = false;
	bool singletonRemovedFromMain = false;
	bool singletonInOtherWindow = false;
	bool crossWindowSingletonValidates = false;
	bool posSerialized = false;
	bool posOmitted = false;
	bool withPosParses = false;
	bool withPosDeepEqual = false;
	bool withoutPosParses = false;
	bool posNullopt = false;
	bool withoutPosValidates = false;
	bool duplicateMainDemoted = false;
	bool postDemoteValidates = false;
	bool v1ShapedParses = false;
	bool v1Identical = false;
};

WindowsResults RunWindowsScenario()
{
	WindowsResults r;
	std::vector<InvariantViolation> v;

	// OpenWindow: tab leaves its well and lands in a new non-main window.
	{
		DockLayout l = MakeDefaultLayout({ "A", "B" });
		NodeId w = l.MainWindow().Main.Id;
		NodeId winId = OpenWindow(l, w, 1);
		r.openReturnedId = winId != 0;
		r.openAddedWindow = l.Windows.size() == 2;
		r.mainStaysFirst = !l.Windows[1].bMain && l.Windows[0].bMain;
		r.bLeftMain = FindTabIndex(l.MainWindow().Main, "B") < 0;
		const WindowNode* nw = FindWindowContainingNode(l, FindWellContaining(l, "B"));
		r.bWindowIsNonMain = nw && !nw->bMain;
		r.postOpenValidates = ValidateLayout(l, {}, v);
	}

	// Invariant 11: a window emptied of panels is removed.
	{
		DockLayout l = MakeDefaultLayout({ "A", "B" });
		NodeId w = l.MainWindow().Main.Id;
		OpenWindow(l, w, 1);
		NodeId nwWell = FindWellContaining(l, "B");
		MovePanel(l, nwWell, 0, w);
		r.drainRemovedWindow = l.Windows.size() == 1;
		r.postDrainValidates = ValidateLayout(l, {}, v);
	}

	// CloseWindow(PerPanel) drops the window and its panels.
	{
		DockLayout l = MakeDefaultLayout({ "A", "B" });
		NodeId w = l.MainWindow().Main.Id;
		NodeId winId = OpenWindow(l, w, 1);
		r.closePerPanel = CloseWindow(l, winId, ClosePolicy::PerPanel);
		r.windowRemoved = l.Windows.size() == 1;
		r.panelDropped = FindWellContaining(l, "B") == 0;
		r.postCloseValidates = ValidateLayout(l, {}, v);
	}

	// CloseWindow(MergeToMain) appends tabs to the main window's default well.
	{
		DockLayout l = MakeDefaultLayout({ "A", "B" });
		NodeId w = l.MainWindow().Main.Id;
		NodeId winId = OpenWindow(l, w, 1);
		r.closeMerge = CloseWindow(l, winId, ClosePolicy::MergeToMain);
		r.removedAfterMerge = l.Windows.size() == 1;
		r.mergedBack = FindTabIndex(l.MainWindow().Main, "B") >= 0;
		r.postMergeValidates = ValidateLayout(l, {}, v);
	}

	// The main window is never closed (invariant 10).
	{
		DockLayout l = MakeDefaultLayout({ "A" });
		NodeId mainId = l.MainWindow().Id;
		r.closeMainNoop = !CloseWindow(l, mainId, ClosePolicy::PerPanel);
		r.mainSurvives = l.Windows.size() == 1 && l.Windows[0].Id == mainId;
	}

	// MoveToWindow: the command and the drop are the same transform.
	{
		DockLayout l = MakeDefaultLayout({ "A", "B" });
		NodeId mainWell = l.MainWindow().Main.Id;
		OpenWindow(l, mainWell, 1);
		NodeId nwWell = FindWellContaining(l, "B");
		DropTarget t;
		t.KindId = DropTarget::Kind::WellZone;
		t.Well = mainWell;
		t.ZoneId = Zone::Center;
		DockLayout rr = MoveToWindow(l, nwWell, 0, l.MainWindow().Id, t);
		r.moveClosesWindow = rr.Windows.size() == 1;
		r.bLanded = FindTabIndex(rr.MainWindow().Main, "B") >= 0;
		r.postMoveValidates = ValidateLayout(rr, {}, v);
	}

	// Singleton uniqueness across windows (invariant 12).
	{
		DockLayout l = MakeDefaultLayout({ "A", "B" });
		NodeId mainWell = l.MainWindow().Main.Id;
		OpenWindow(l, mainWell, 1);
		NodeId nwWell = FindWellContaining(l, "B");
		InsertTab(l, nwWell, "A", -1, true);
		r.singletonRemovedFromMain = FindTabIndex(l.MainWindow().Main, "A") < 0;
		r.singletonInOtherWindow = FindTabIndex(*FindNode(l, nwWell), "A") >= 0;
		r.crossWindowSingletonValidates = ValidateLayout(l, { "A" }, v);
	}

	// Ser/de with positions: includePositions=false omits pos (invariant 13).
	{
		DockLayout l = MakeDefaultLayout({ "A", "B" });
		NodeId mainWell = l.MainWindow().Main.Id;
		OpenWindow(l, mainWell, 1);
		l.Windows[1].Pos = glm::vec2(100.0f, 50.0f);

		std::string withPos = SerializeLayout(l, false, true);
		r.posSerialized = withPos.find("\"pos\"") != std::string::npos;
		std::string withoutPos = SerializeLayout(l, false, false);
		r.posOmitted = withoutPos.find("\"pos\"") == std::string::npos;

		DockLayout l2;
		r.withPosParses = ParseLayout(withPos, l2, [](const PanelId&) { return true; }, [](const std::string&) {});
		r.withPosDeepEqual = LayoutsEquivalentIgnoringIds(l, l2);

		DockLayout l3;
		r.withoutPosParses = ParseLayout(withoutPos, l3, [](const PanelId&) { return true; }, [](const std::string&) {});
		r.posNullopt = !l3.Windows[1].Pos.has_value();
		r.withoutPosValidates = ValidateLayout(l3, {}, v);
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
		r.duplicateMainDemoted = mains == 1 && l.Windows[0].bMain;
		r.postDemoteValidates = ValidateLayout(l, {}, v);
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
		r.v1ShapedParses = ok;
		r.v1Identical = LayoutsEquivalentIgnoringIds(v1, migrated);
	}
	return r;
}

TEST_CASE("dock: windows — OpenWindow returned a window id")
{
	CHECK_MESSAGE(RunWindowsScenario().openReturnedId, "OpenWindow returned a window id");
}

TEST_CASE("dock: windows — OpenWindow added a window")
{
	CHECK_MESSAGE(RunWindowsScenario().openAddedWindow, "OpenWindow added a window");
}

TEST_CASE("dock: windows — main stays [0], new window is non-main")
{
	CHECK_MESSAGE(RunWindowsScenario().mainStaysFirst, "main stays [0], new window is non-main");
}

TEST_CASE("dock: windows — B left the main well")
{
	CHECK_MESSAGE(RunWindowsScenario().bLeftMain, "B left the main well");
}

TEST_CASE("dock: windows — B's window is the non-main window")
{
	CHECK_MESSAGE(RunWindowsScenario().bWindowIsNonMain, "B's window is the non-main window");
}

TEST_CASE("dock: windows — post-open layout validates")
{
	CHECK_MESSAGE(RunWindowsScenario().postOpenValidates, "post-open layout validates");
}

TEST_CASE("dock: windows — empty non-main window removed after its last tab moved out")
{
	CHECK_MESSAGE(RunWindowsScenario().drainRemovedWindow, "empty non-main window removed after its last tab moved out");
}

TEST_CASE("dock: windows — post-drain layout validates")
{
	CHECK_MESSAGE(RunWindowsScenario().postDrainValidates, "post-drain layout validates");
}

TEST_CASE("dock: windows — CloseWindow(PerPanel) succeeded")
{
	CHECK_MESSAGE(RunWindowsScenario().closePerPanel, "CloseWindow(PerPanel) succeeded");
}

TEST_CASE("dock: windows — window removed")
{
	CHECK_MESSAGE(RunWindowsScenario().windowRemoved, "window removed");
}

TEST_CASE("dock: windows — panel dropped with the window")
{
	CHECK_MESSAGE(RunWindowsScenario().panelDropped, "panel dropped with the window");
}

TEST_CASE("dock: windows — post-close layout validates")
{
	CHECK_MESSAGE(RunWindowsScenario().postCloseValidates, "post-close layout validates");
}

TEST_CASE("dock: windows — CloseWindow(MergeToMain) succeeded")
{
	CHECK_MESSAGE(RunWindowsScenario().closeMerge, "CloseWindow(MergeToMain) succeeded");
}

TEST_CASE("dock: windows — window removed after merge")
{
	CHECK_MESSAGE(RunWindowsScenario().removedAfterMerge, "window removed after merge");
}

TEST_CASE("dock: windows — B merged back into the main well")
{
	CHECK_MESSAGE(RunWindowsScenario().mergedBack, "B merged back into the main well");
}

TEST_CASE("dock: windows — post-merge layout validates")
{
	CHECK_MESSAGE(RunWindowsScenario().postMergeValidates, "post-merge layout validates");
}

TEST_CASE("dock: windows — CloseWindow on main is a no-op")
{
	CHECK_MESSAGE(RunWindowsScenario().closeMainNoop, "CloseWindow on main is a no-op");
}

TEST_CASE("dock: windows — main window survives")
{
	CHECK_MESSAGE(RunWindowsScenario().mainSurvives, "main window survives");
}

TEST_CASE("dock: windows — moving the last tab out closes the window")
{
	CHECK_MESSAGE(RunWindowsScenario().moveClosesWindow, "moving the last tab out closes the window");
}

TEST_CASE("dock: windows — B landed in the target window's well")
{
	CHECK_MESSAGE(RunWindowsScenario().bLanded, "B landed in the target window's well");
}

TEST_CASE("dock: windows — post-move layout validates")
{
	CHECK_MESSAGE(RunWindowsScenario().postMoveValidates, "post-move layout validates");
}

TEST_CASE("dock: windows — singleton removed from the main window")
{
	CHECK_MESSAGE(RunWindowsScenario().singletonRemovedFromMain, "singleton removed from the main window");
}

TEST_CASE("dock: windows — singleton lives in the other window")
{
	CHECK_MESSAGE(RunWindowsScenario().singletonInOtherWindow, "singleton lives in the other window");
}

TEST_CASE("dock: windows — cross-window singleton layout validates")
{
	CHECK_MESSAGE(RunWindowsScenario().crossWindowSingletonValidates, "cross-window singleton layout validates");
}

TEST_CASE("dock: windows — positions serialized when allowed")
{
	CHECK_MESSAGE(RunWindowsScenario().posSerialized, "positions serialized when allowed");
}

TEST_CASE("dock: windows — positions omitted when platform cannot place windows")
{
	CHECK_MESSAGE(RunWindowsScenario().posOmitted, "positions omitted when platform cannot place windows");
}

TEST_CASE("dock: windows — with-pos round-trip parses")
{
	CHECK_MESSAGE(RunWindowsScenario().withPosParses, "with-pos round-trip parses");
}

TEST_CASE("dock: windows — with-pos round-trip deep-equal")
{
	CHECK_MESSAGE(RunWindowsScenario().withPosDeepEqual, "with-pos round-trip deep-equal");
}

TEST_CASE("dock: windows — without-pos round-trip parses")
{
	CHECK_MESSAGE(RunWindowsScenario().withoutPosParses, "without-pos round-trip parses");
}

TEST_CASE("dock: windows — omitted pos reads back as nullopt")
{
	CHECK_MESSAGE(RunWindowsScenario().posNullopt, "omitted pos reads back as nullopt");
}

TEST_CASE("dock: windows — without-pos layout validates")
{
	CHECK_MESSAGE(RunWindowsScenario().withoutPosValidates, "without-pos layout validates");
}

TEST_CASE("dock: windows — duplicate bMain demoted, first window stays main")
{
	CHECK_MESSAGE(RunWindowsScenario().duplicateMainDemoted, "duplicate bMain demoted, first window stays main");
}

TEST_CASE("dock: windows — post-demote layout validates")
{
	CHECK_MESSAGE(RunWindowsScenario().postDemoteValidates, "post-demote layout validates");
}

TEST_CASE("dock: windows — v1-shaped json parses")
{
	CHECK_MESSAGE(RunWindowsScenario().v1ShapedParses, "v1-shaped json parses");
}

TEST_CASE("dock: windows — v1 migration produces an identical single-window v2 model")
{
	CHECK_MESSAGE(RunWindowsScenario().v1Identical, "v1 migration produces an identical single-window v2 model");
}

// ---- TestFuzz ----

struct FuzzResults
{
	bool invariantsHeld = true;
	std::string firstViolation;
	bool parseAlwaysSucceeded = true;
	bool roundTripEquivalent = true;
	bool deserializedValidates = true;
};

FuzzResults RunFuzzScenario()
{
	FuzzResults r;
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
			r.invariantsHeld = false;
			r.firstViolation = "fuzz op " + std::to_string(op) + ": " + v[0].What;
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
				r.parseAlwaysSucceeded = false;
				break;
			}
			r.roundTripEquivalent = r.roundTripEquivalent && LayoutsEquivalentIgnoringIds(l, l2);
			std::vector<InvariantViolation> v2;
			r.deserializedValidates = r.deserializedValidates && ValidateLayout(l2, singletons, v2);
			l = std::move(l2);
		}
	}
	return r;
}

TEST_CASE("dock: fuzz — every op keeps the layout valid")
{
	FuzzResults r = RunFuzzScenario();
	INFO(r.firstViolation);
	CHECK_MESSAGE(r.invariantsHeld, "fuzz: every op keeps the layout valid");
}

TEST_CASE("dock: fuzz — ser/de parses at every checkpoint")
{
	CHECK_MESSAGE(RunFuzzScenario().parseAlwaysSucceeded, "fuzz ser/de parse failed");
}

TEST_CASE("dock: fuzz — ser/de round-trip equivalent")
{
	CHECK_MESSAGE(RunFuzzScenario().roundTripEquivalent, "fuzz ser/de round-trip equivalent");
}

TEST_CASE("dock: fuzz — deserialized layout validates")
{
	CHECK_MESSAGE(RunFuzzScenario().deserializedValidates, "fuzz deserialized layout validates");
}

} // namespace

}} // namespace wisp::dock
