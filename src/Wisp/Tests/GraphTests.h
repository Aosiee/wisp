// The graph view model's pure-piece tests (the two-sided layout in
// both modes + the hit test). Registered in tinytest.

#pragma once

#include "doctest/doctest.h"
#include "Wisp/Graph/GraphCanvas.h"
#include "Wisp/Graph/GraphLayout.h"
#include "Wisp/Graph/GraphView.h"
#include "Wisp/Graph/SizeMap.h"

#include <algorithm>
#include <cstdio>
#include <string>
#include <unordered_map>
#include <vector>

namespace wisp {
namespace graph_tests {

	using namespace wisp::graph;

	namespace {

		// The synthetic domain: the small int ids, the title/category
		// metadata, and the reference table (the asset -> the assets it
		// references).
		struct SyntheticQuery : RefGraphQuery
		{
			std::unordered_map<NodeId, RefNodeInfo> Info;
			std::unordered_map<NodeId, std::vector<NodeId>> References;

			void Add(NodeId id, const std::string& title, const std::string& category = "Asset")
			{
				RefNodeInfo info;
				info.Title = title;
				info.Category = category;
				Info[id] = info;
			}

			void Ref(NodeId from, NodeId to) { References[from].push_back(to); }

			std::vector<NodeId> Dependencies(NodeId id) const override
			{
				auto it = References.find(id);
				return it == References.end() ? std::vector<NodeId>{} : it->second;
			}

			std::vector<NodeId> Dependents(NodeId id) const override
			{
				std::vector<NodeId> out;
				for (const auto& [from, refs] : References)
					if (std::find(refs.begin(), refs.end(), id) != refs.end())
						out.push_back(from);
				return out;
			}

			bool Resolve(NodeId id, RefNodeInfo& out) const override
			{
				auto it = Info.find(id);
				if (it == Info.end())
					return false;
				out = it->second;
				return true;
			}
		};

		// The "aNN" title keeps the string sort in the numeric order.
		void AddNumbered(SyntheticQuery& query, NodeId id)
		{
			char title[16];
			snprintf(title, sizeof(title), "a%02llu", (unsigned long long)id);
			query.Add(id, title);
		}

		const ViewNode* FindNode(const GraphView& view, NodeId id)
		{
			for (const ViewNode& node : view.Nodes)
				if (node.Id == id)
					return &node;
			return nullptr;
		}

		const ViewNode* RequireNode(const GraphView& view, NodeId id)
		{
			const ViewNode* node = FindNode(view, id);
			REQUIRE(node != nullptr);
			return node;
		}

		const ViewEdge* FindEdge(const GraphView& view, NodeId from, NodeId to)
		{
			for (const ViewEdge& edge : view.Edges)
				if (edge.From == from && edge.To == to)
					return &edge;
			return nullptr;
		}

		const ViewEdge* RequireEdge(const GraphView& view, NodeId from, NodeId to)
		{
			const ViewEdge* edge = FindEdge(view, from, to);
			REQUIRE(edge != nullptr);
			return edge;
		}

		std::vector<const ViewNode*> NodesOfAsset(const GraphView& view, const NodeMap& map, NodeId asset)
		{
			std::vector<const ViewNode*> out;
			for (const ViewNode& node : view.Nodes)
			{
				auto it = map.find(node.Id);
				if (it != map.end() && it->second == asset)
					out.push_back(&node);
			}
			return out;
		}

		GraphView Layout(SyntheticQuery& query, NodeId root, uint32_t maxDepth, uint32_t maxBreadth,
			bool showDuplicates, NodeMap& map)
		{
			GraphView view;
			LayoutParams params;
			params.Root = root;
			params.MaxDepth = maxDepth;
			params.MaxBreadth = maxBreadth;
			params.ShowDuplicates = showDuplicates;
			TwoSidedLayout layout;
			layout.Layout(view, map, query, params);
			return view;
		}

		void CheckSameGraph(const GraphView& a, const GraphView& b)
		{
			REQUIRE(a.Nodes.size() == b.Nodes.size());
			REQUIRE(a.Edges.size() == b.Edges.size());
			for (size_t i = 0; i < a.Nodes.size(); i++)
			{
				CHECK(a.Nodes[i].Id == b.Nodes[i].Id);
				CHECK(a.Nodes[i].Position == b.Nodes[i].Position);
				CHECK(a.Nodes[i].Flags == b.Nodes[i].Flags);
				CHECK(a.Nodes[i].Title == b.Nodes[i].Title);
			}
			for (size_t i = 0; i < a.Edges.size(); i++)
			{
				CHECK(a.Edges[i].From == b.Edges[i].From);
				CHECK(a.Edges[i].To == b.Edges[i].To);
				CHECK(a.Edges[i].Flags == b.Edges[i].Flags);
			}
		}

		// The layout's node/column constants (kNodeWidth + kColumnGap).
		constexpr float kColumnStride = 280.0f;

	} // namespace

	TEST_CASE("graph layout: dag mode places the dependency and the dependent columns")
	{
		SyntheticQuery query;
		for (NodeId id = 1; id <= 6; id++)
			AddNumbered(query, id);
		query.Ref(1, 2);
		query.Ref(1, 3);
		query.Ref(2, 5);
		query.Ref(4, 1);
		query.Ref(6, 4);

		NodeMap map;
		GraphView view = Layout(query, 1, 3, 20, false, map);

		CHECK(RequireNode(view, 1)->Position.x == doctest::Approx(0.0f));
		CHECK(RequireNode(view, 2)->Position.x == doctest::Approx(-kColumnStride));
		CHECK(RequireNode(view, 3)->Position.x == doctest::Approx(-kColumnStride));
		CHECK(RequireNode(view, 5)->Position.x == doctest::Approx(-2.0f * kColumnStride));
		CHECK(RequireNode(view, 4)->Position.x == doctest::Approx(kColumnStride));
		CHECK(RequireNode(view, 6)->Position.x == doctest::Approx(2.0f * kColumnStride));

		// The two depth-1 dependencies share the column (the different rows).
		CHECK(RequireNode(view, 2)->Position.x == RequireNode(view, 3)->Position.x);
		CHECK(RequireNode(view, 2)->Position.y != RequireNode(view, 3)->Position.y);

		// The edges: the dependencies carry the Outgoing flag.
		CHECK((RequireEdge(view, 1, 2)->Flags & EdgeFlags::Outgoing) != 0);
		CHECK((RequireEdge(view, 1, 3)->Flags & EdgeFlags::Outgoing) != 0);
		CHECK((RequireEdge(view, 2, 5)->Flags & EdgeFlags::Outgoing) != 0);
		CHECK((RequireEdge(view, 4, 1)->Flags & EdgeFlags::Outgoing) == 0);
		CHECK((RequireEdge(view, 6, 4)->Flags & EdgeFlags::Outgoing) == 0);

		// The node -> the asset map is filled for every node.
		CHECK(map.at(1) == 1);
		CHECK(map.at(2) == 2);
		CHECK(map.at(6) == 6);
	}

	TEST_CASE("graph layout: dag mode dedupes at the minimum depth and marks the cross-link")
	{
		SyntheticQuery query;
		for (NodeId id = 1; id <= 3; id++)
			AddNumbered(query, id);
		query.Ref(1, 2);
		query.Ref(1, 3);
		query.Ref(3, 2);

		NodeMap map;
		GraphView view = Layout(query, 1, 3, 20, false, map);

		// The asset 2 is once, at its minimum depth 1 (not 2 via the 3).
		CHECK(NodesOfAsset(view, map, 2).size() == 1);
		CHECK(RequireNode(view, 2)->Position.x == doctest::Approx(-kColumnStride));

		// The 3 -> 2 edge is a cross-link at the same column (still on the
		// dependency side, so it keeps the Outgoing family).
		const ViewEdge* edge = RequireEdge(view, 3, 2);
		CHECK((edge->Flags & EdgeFlags::BackEdge) != 0);
		CHECK((edge->Flags & EdgeFlags::Outgoing) != 0);
	}

	TEST_CASE("graph layout: dag mode flags the both-sides asset once at its smaller depth")
	{
		SyntheticQuery query;
		for (NodeId id = 1; id <= 4; id++)
			AddNumbered(query, id);
		query.Ref(1, 2);   // the 2 at the left depth 1
		query.Ref(4, 1);   // the 4 at the right depth 1
		query.Ref(2, 4);   // the 2 also at the right depth 2

		NodeMap map;
		GraphView view = Layout(query, 1, 3, 20, false, map);

		// One node, on the dependency side (the depth 1 beats the depth 2).
		const std::vector<const ViewNode*> nodes = NodesOfAsset(view, map, 2);
		REQUIRE(nodes.size() == 1);
		CHECK((nodes[0]->Flags & NodeFlags::BothSides) != 0);
		CHECK(nodes[0]->Position.x == doctest::Approx(-kColumnStride));

		// The 2 -> 4 cross-side span and the 4 -> 1 chain are normal edges.
		CHECK((RequireEdge(view, 2, 4)->Flags & EdgeFlags::BackEdge) == 0);
		CHECK((RequireEdge(view, 4, 1)->Flags & EdgeFlags::BackEdge) == 0);
	}

	TEST_CASE("graph layout: dag mode ties the both-sides asset to the dependency side")
	{
		SyntheticQuery query;
		for (NodeId id = 1; id <= 2; id++)
			AddNumbered(query, id);
		query.Ref(1, 2);
		query.Ref(2, 1); // the 2-cycle

		NodeMap map;
		GraphView view = Layout(query, 1, 3, 20, false, map);

		CHECK(NodesOfAsset(view, map, 2).size() == 1);
		CHECK(RequireNode(view, 2)->Position.x == doctest::Approx(-kColumnStride));
		CHECK((RequireNode(view, 2)->Flags & NodeFlags::BothSides) != 0);
		CHECK((RequireEdge(view, 1, 2)->Flags & EdgeFlags::BackEdge) == 0);
		CHECK((RequireEdge(view, 2, 1)->Flags & EdgeFlags::BackEdge) != 0);
	}

	TEST_CASE("graph layout: dag mode keeps the cycles bounded")
	{
		SyntheticQuery query;
		for (NodeId id = 1; id <= 3; id++)
			AddNumbered(query, id);
		query.Ref(1, 2);
		query.Ref(2, 3);
		query.Ref(3, 2); // the 2 <-> 3 cycle

		NodeMap map;
		GraphView view = Layout(query, 1, 3, 20, false, map);

		CHECK(view.Nodes.size() == 3);
		CHECK(RequireNode(view, 3)->Position.x == doctest::Approx(-2.0f * kColumnStride));
		const ViewEdge* edge = RequireEdge(view, 3, 2);
		CHECK((edge->Flags & EdgeFlags::BackEdge) != 0);
	}

	TEST_CASE("graph layout: dag mode collapses the breadth overflow")
	{
		SyntheticQuery query;
		AddNumbered(query, 1);
		for (NodeId id = 2; id <= 26; id++)
		{
			AddNumbered(query, id);
			query.Ref(1, id); // the 25 children
		}

		NodeMap map;
		GraphView view = Layout(query, 1, 3, 20, false, map);

		// The first 20 children (the sorted titles) + the overflow node.
		CHECK(NodesOfAsset(view, map, 2).size() == 1);
		CHECK(NodesOfAsset(view, map, 21).size() == 1);
		CHECK(NodesOfAsset(view, map, 22).empty());
		CHECK(NodesOfAsset(view, map, 26).empty());

		const ViewNode* overflow = nullptr;
		for (const ViewNode& node : view.Nodes)
			if (node.Flags & NodeFlags::Overflow)
				overflow = &node;
		REQUIRE(overflow != nullptr);
		CHECK(overflow->Title == "+5 more");
		CHECK(overflow->Position.x == doctest::Approx(-kColumnStride));
		CHECK((overflow->Flags & NodeFlags::OverflowLeft) != 0);
		CHECK(map.at(overflow->Id) == 1);

		CHECK(FindEdge(view, 1, overflow->Id) != nullptr);
	}

	TEST_CASE("graph layout: dag mode overflows the dependent side to the right")
	{
		SyntheticQuery query;
		AddNumbered(query, 1);
		for (NodeId id = 2; id <= 26; id++)
		{
			AddNumbered(query, id);
			query.Ref(id, 1); // the 25 dependents
		}

		NodeMap map;
		GraphView view = Layout(query, 1, 3, 20, false, map);

		const ViewNode* overflow = nullptr;
		for (const ViewNode& node : view.Nodes)
			if (node.Flags & NodeFlags::Overflow)
				overflow = &node;
		REQUIRE(overflow != nullptr);
		CHECK(overflow->Position.x == doctest::Approx(kColumnStride));
		CHECK((overflow->Flags & NodeFlags::OverflowLeft) == 0);
		// The dependent-side overflow edge points at the parent.
		const ViewEdge* edge = RequireEdge(view, overflow->Id, 1);
		CHECK((edge->Flags & EdgeFlags::Outgoing) == 0);
	}

	TEST_CASE("graph layout: tree mode expands every path")
	{
		SyntheticQuery query;
		for (NodeId id = 1; id <= 4; id++)
			AddNumbered(query, id);
		query.Ref(1, 2);
		query.Ref(1, 3);
		query.Ref(2, 4);
		query.Ref(3, 4); // the diamond

		NodeMap map;
		GraphView view = Layout(query, 1, 3, 20, true, map);

		// The asset 4 appears twice, both at the column -2, with the
		// distinct occurrence ids.
		const std::vector<const ViewNode*> fours = NodesOfAsset(view, map, 4);
		REQUIRE(fours.size() == 2);
		CHECK(fours[0]->Id != fours[1]->Id);
		CHECK(fours[0]->Position.x == doctest::Approx(-2.0f * kColumnStride));
		CHECK(fours[1]->Position.x == doctest::Approx(-2.0f * kColumnStride));

		CHECK(view.Nodes.size() == 5); // the root, the 2, the 3, the two 4s
	}

	TEST_CASE("graph layout: tree mode closes the cycle onto the ancestor occurrence")
	{
		SyntheticQuery query;
		for (NodeId id = 1; id <= 3; id++)
			AddNumbered(query, id);
		query.Ref(1, 2);
		query.Ref(2, 3);
		query.Ref(3, 2); // the 2 <-> 3 cycle

		NodeMap map;
		GraphView view = Layout(query, 1, 3, 20, true, map);

		// No duplicate occurrences: the cycle is a back edge to the
		// ancestor occurrence of the asset 2.
		CHECK(view.Nodes.size() == 3);
		CHECK(NodesOfAsset(view, map, 2).size() == 1);

		const ViewNode* three = nullptr;
		const ViewNode* twoNode = nullptr;
		for (const ViewNode& node : view.Nodes)
		{
			if (map.at(node.Id) == 2) twoNode = &node;
			if (map.at(node.Id) == 3) three = &node;
		}
		REQUIRE(twoNode != nullptr);
		REQUIRE(three != nullptr);
		const ViewEdge* edge = FindEdge(view, three->Id, twoNode->Id);
		REQUIRE(edge != nullptr);
		CHECK((edge->Flags & EdgeFlags::BackEdge) != 0);
	}

	TEST_CASE("graph layout: tree mode shares the root between the two side trees")
	{
		SyntheticQuery query;
		for (NodeId id = 1; id <= 3; id++)
			AddNumbered(query, id);
		query.Ref(1, 2);
		query.Ref(3, 1);

		NodeMap map;
		GraphView view = Layout(query, 1, 3, 20, true, map);

		int rootCount = 0;
		for (const ViewNode& node : view.Nodes)
			if (node.Flags & NodeFlags::Root)
				rootCount++;
		CHECK(rootCount == 1);
		CHECK(view.Nodes.size() == 3);
		const std::vector<const ViewNode*> twos = NodesOfAsset(view, map, 2);
		const std::vector<const ViewNode*> threes = NodesOfAsset(view, map, 3);
		REQUIRE(twos.size() == 1);
		REQUIRE(threes.size() == 1);
		CHECK(twos[0]->Position.x == doctest::Approx(-kColumnStride));
		CHECK(threes[0]->Position.x == doctest::Approx(kColumnStride));
	}

	TEST_CASE("graph layout: tree mode collapses the per-occurrence overflow")
	{
		SyntheticQuery query;
		AddNumbered(query, 1);
		AddNumbered(query, 2);
		for (NodeId id = 3; id <= 24; id++)
		{
			AddNumbered(query, id);
			query.Ref(2, id); // the 22 children of the 2
		}
		query.Ref(1, 2);

		NodeMap map;
		GraphView view = Layout(query, 1, 3, 20, true, map);

		const ViewNode* overflow = nullptr;
		for (const ViewNode& node : view.Nodes)
			if (node.Flags & NodeFlags::Overflow)
				overflow = &node;
		REQUIRE(overflow != nullptr);
		CHECK(overflow->Title == "+2 more");
		CHECK(overflow->Position.x == doctest::Approx(-2.0f * kColumnStride));
		CHECK(map.at(overflow->Id) == 2);
	}

	TEST_CASE("graph layout: the layout is deterministic in both modes")
	{
		SyntheticQuery query;
		for (NodeId id = 1; id <= 8; id++)
			AddNumbered(query, id);
		query.Ref(1, 2);
		query.Ref(1, 3);
		query.Ref(1, 4);
		query.Ref(2, 5);
		query.Ref(3, 5);
		query.Ref(5, 2);
		query.Ref(6, 1);
		query.Ref(7, 6);
		query.Ref(8, 3);

		for (bool duplicates : { false, true })
		{
			NodeMap mapA, mapB;
			GraphView a = Layout(query, 1, 3, 20, duplicates, mapA);
			GraphView b = Layout(query, 1, 3, 20, duplicates, mapB);
			CheckSameGraph(a, b);
			CHECK(mapA == mapB);
		}
	}

	TEST_CASE("graph layout: dag mode orders the dependent column by the parent barycentre")
	{
		SyntheticQuery query;
		for (NodeId id = 1; id <= 5; id++)
			AddNumbered(query, id);
		query.Ref(2, 1);
		query.Ref(3, 1);
		query.Ref(4, 2); // the 4 references both the parents
		query.Ref(4, 3);
		query.Ref(5, 2); // the 5 references the first parent only

		NodeMap map;
		GraphView view = Layout(query, 1, 3, 20, false, map);

		// The insertion order is 4 then 5 (the 4 sorts before the 5 in the
		// parent's child order), but the barycentre pulls the 5 (0.0) above
		// the 4 (0.5) in the dependent column.
		CHECK(RequireNode(view, 5)->Position.y < RequireNode(view, 4)->Position.y);
	}

	TEST_CASE("graph hit test: the node beats the edge beats the canvas")
	{
		GraphView view;
		ViewNode a;
		a.Id = 1;
		a.Position = { 0.0f, 0.0f };
		a.Size = { 100.0f, 60.0f };
		ViewNode b;
		b.Id = 2;
		b.Position = { 300.0f, 0.0f };
		b.Size = { 100.0f, 60.0f };
		ViewEdge edge;
		edge.From = 1;
		edge.To = 2;
		view.Nodes = { a, b };
		view.Edges = { edge };
		ViewTransform transform;

		CHECK(HitTest(view, transform, { 50.0f, 30.0f }).Kind == HitResult::Kind::Node);
		CHECK(HitTest(view, transform, { 50.0f, 30.0f }).Node == 1);
		// The same-row bow arcs the edge's middle to ~y 15, so the probe
		// tracks the curve.
		CHECK(HitTest(view, transform, { 150.0f, 15.0f }).Kind == HitResult::Kind::Edge);
		CHECK(HitTest(view, transform, { 150.0f, 15.0f }).Edge == 0);
		CHECK(HitTest(view, transform, { 250.0f, 150.0f }).Kind == HitResult::Kind::Canvas);

		// Inside the node B and 3 px from the edge's end: the node wins.
		const HitResult result = HitTest(view, transform, { 303.0f, 30.0f });
		CHECK(result.Kind == HitResult::Kind::Node);
		CHECK(result.Node == 2);
	}

	TEST_CASE("graph hit test: the pan offsets and the zoom scales the pick band")
	{
		GraphView view;
		ViewNode a;
		a.Id = 1;
		a.Position = { 0.0f, 0.0f };
		a.Size = { 100.0f, 60.0f };
		ViewNode b;
		b.Id = 2;
		b.Position = { 300.0f, 0.0f };
		b.Size = { 100.0f, 60.0f };
		ViewEdge edge;
		edge.From = 1;
		edge.To = 2;
		view.Nodes = { a, b };
		view.Edges = { edge };

		// The screen = world + pan at the default zoom.
		ViewTransform transform;
		ViewTransform panned;
		panned.Pan = { 100.0f, 50.0f };
		CHECK(HitTest(view, panned, { 150.0f, 80.0f }).Kind == HitResult::Kind::Node);
		CHECK(HitTest(view, panned, { 60.0f, 80.0f }).Kind == HitResult::Kind::Canvas);

		// The zoom level table.
		CHECK(ZoomScale(kDefaultZoomLevel) == doctest::Approx(1.0f));
		CHECK(ZoomScale(0) == doctest::Approx(0.10f));

		// The world point 30 units above the edge line: inside the pick
		// band (60 world units) at the 0.10 zoom, outside (6 world units)
		// at the 1.0 zoom.
		ViewTransform zoomedOut;
		zoomedOut.ZoomLevel = 0;
		const HitResult far = HitTest(view, zoomedOut, { 15.0f, 6.0f });
		CHECK(far.Kind == HitResult::Kind::Edge);
		const HitResult near = HitTest(view, transform, { 150.0f, 60.0f });
		CHECK(near.Kind == HitResult::Kind::Canvas);
	}

	TEST_CASE("graph canvas: the LOD tiers follow the zoom table")
	{
		CHECK(LodTierFor(10) == LodTier::Full);      // 1.0
		CHECK(LodTierFor(6) == LodTier::Full);       // 0.50
		CHECK(LodTierFor(7) == LodTier::Full);       // 0.67
		CHECK(LodTierFor(5) == LodTier::TitleOnly);  // 0.33
		CHECK(LodTierFor(4) == LodTier::TitleOnly);  // 0.25
		CHECK(LodTierFor(3) == LodTier::Boxes);      // 0.20
		CHECK(LodTierFor(0) == LodTier::Boxes);      // 0.10
	}

	TEST_CASE("graph canvas: the zoom anchor keeps the cursor point fixed")
	{
		ViewTransform t;
		t.Pan = { 100.0f, 50.0f };
		t.ZoomLevel = 10; // 1.0x
		const glm::vec2 cursor = { 200.0f, 100.0f };

		// The world point under the cursor before the zoom...
		const glm::vec2 before = (cursor - t.Pan) / ZoomScale(t.ZoomLevel);
		CHECK(before == glm::vec2(100.0f, 50.0f));

		// ...stays under the cursor at the new level.
		const glm::vec2 pan = ZoomAnchorPan(t, cursor, 12); // 1.5x
		CHECK((cursor - pan) / ZoomScale(12) == before);
	}

	TEST_CASE("graph canvas: the edge sample count is bounded")
	{
		CHECK(EdgeSampleCount(10.0f) == 8);
		CHECK(EdgeSampleCount(100.0f) == 17);
		CHECK(EdgeSampleCount(10000.0f) == 64);
	}

	TEST_CASE("graph canvas: the frame transform fits the nodes")
	{
		GraphView view;
		ViewNode a;
		a.Id = 1;
		a.Position = { 0.0f, 0.0f };
		a.Size = { 100.0f, 60.0f };
		ViewNode b;
		b.Id = 2;
		b.Position = { 300.0f, 0.0f };
		b.Size = { 100.0f, 60.0f };
		view.Nodes = { a, b };

		// The fit scale = min(800/400, 600/60) * 0.9 = 1.8 -> the largest
		// level at or below it: 13 (1.75).
		const ViewTransform framed = FrameTransform(view, {}, { 800.0f, 600.0f });
		CHECK(framed.ZoomLevel == 13);
		const glm::vec2 centre = (glm::vec2(0.0f) + glm::vec2(400.0f, 60.0f)) * 0.5f;
		CHECK(framed.Pan == glm::vec2(400.0f, 300.0f) - centre * ZoomScale(13));

		// The subset frames the second node only.
		const ViewTransform subset = FrameTransform(view, { 2 }, { 800.0f, 600.0f });
		CHECK(subset.ZoomLevel >= 0);
		const glm::vec2 subCentre = glm::vec2(350.0f, 30.0f);
		CHECK(subset.Pan == glm::vec2(400.0f, 300.0f) - subCentre * ZoomScale(subset.ZoomLevel));
	}

	TEST_CASE("graph canvas: the frame transform on an empty view is the default")
	{
		GraphView view;
		const ViewTransform framed = FrameTransform(view, {}, { 800.0f, 600.0f });
		CHECK(framed.ZoomLevel == kDefaultZoomLevel);
		CHECK(framed.Pan == glm::vec2(0.0f));
	}

	TEST_CASE("graph view: straight edges run border to border, each with its own point on a node's side")
	{
		GraphView view;
		view.Shape = EdgeShape::Straight;
		ViewNode a;
		a.Id = 1;
		a.Position = { 0.0f, 0.0f };
		a.Size = { 100.0f, 40.0f };
		ViewNode b = a;
		b.Id = 2;
		b.Position = { 300.0f, 0.0f };
		ViewNode c = a;
		c.Id = 3;
		c.Position = { 0.0f, 200.0f };
		view.Nodes = { a, b, c };
		view.Edges = { { 1, 2 }, { 2, 1 }, { 1, 3 }, { 1, 3 }, { 3, 2 } };

		glm::vec2 p0, p1, p2, p3;
		// c -> b leaves c through its top edge, which a -> c twice meets too:
		// ordered by where their other ends lie, it takes the third point,
		// one spacing right of the centre line's exit (80).
		EdgeControlPoints(view, 4, p0, p1, p2, p3);
		CHECK(EdgeLane(view, 4) == 0);
		CHECK(p0.y == doctest::Approx(200.0f));
		CHECK(p0.x == doctest::Approx(94.0f));
		CHECK(p1 == glm::mix(p0, p3, 1.0f / 3.0f));

		// A two-way pair: each on the right of its own direction, so the two
		// lines run parallel and apart.
		CHECK(EdgeLane(view, 0) == 1);
		CHECK(EdgeLane(view, 1) == 1);
		glm::vec2 q0, q1, q2, q3;
		EdgeControlPoints(view, 0, p0, p1, p2, p3);
		EdgeControlPoints(view, 1, q0, q1, q2, q3);
		CHECK(p0.x == doctest::Approx(100.0f));
		CHECK(p3.x == doctest::Approx(300.0f));
		CHECK(p0.y == doctest::Approx(27.0f)); // right of +x is +y
		CHECK(p3.y == doctest::Approx(27.0f));
		CHECK(q0.y == doctest::Approx(13.0f));

		// Repeated edges in one direction: lanes 1 and 2.
		CHECK(EdgeLane(view, 2) == 1);
		CHECK(EdgeLane(view, 3) == 2);
		EdgeControlPoints(view, 2, p0, p1, p2, p3);
		EdgeControlPoints(view, 3, q0, q1, q2, q3);
		CHECK(std::abs(p0.x - q0.x) == doctest::Approx(14.0f));
		CHECK(std::abs(p3.x - q3.x) == doctest::Approx(14.0f)); // parallel, not crossing
	}

	TEST_CASE("graph canvas: the frame transform keeps to its zoom range")
	{
		GraphView view;
		ViewNode node;
		node.Id = 1;
		node.Size = { 2000.0f, 100.0f };
		view.Nodes = { node };
		// Too wide for the viewport at any level the range allows: the floor.
		CHECK(FrameTransform(view, {}, { 400.0f, 300.0f }, 6, 10).ZoomLevel == 6);
		CHECK(FrameTransform(view, {}, { 400.0f, 300.0f }).ZoomLevel < 6);
		// A small graph is not blown up past the ceiling.
		view.Nodes[0].Size = { 20.0f, 10.0f };
		CHECK(FrameTransform(view, {}, { 400.0f, 300.0f }, 6, 10).ZoomLevel == 10);
		CHECK(FrameTransform(view, {}, { 400.0f, 300.0f }).ZoomLevel == 14);
	}

	TEST_CASE("graph view: the same-row edge control points lift the curve")
	{
		// Two nodes at the same row: the edge must lift its middle (the
		// degenerate straight line reads as a floating bar, not a
		// connector).
		GraphView view;
		ViewNode a;
		a.Id = 1;
		a.Position = { 0.0f, 0.0f };
		a.Size = { 100.0f, 60.0f };
		ViewNode b;
		b.Id = 2;
		b.Position = { 300.0f, 0.0f };
		b.Size = { 100.0f, 60.0f };
		view.Nodes = { a, b };
		ViewEdge edge;
		edge.From = 1;
		edge.To = 2;
		view.Edges = { edge };

		glm::vec2 p0, p1, p2, p3;
		EdgeControlPoints(view, 0, p0, p1, p2, p3);
		CHECK(p0.y == doctest::Approx(30.0f));
		CHECK(p1.y < p0.y); // the middle bows up over the gap
		CHECK(p2.y < p3.y);
	}

	namespace {

		// The synthetic size domain: the small-int ids, the fixed byte
		// sizes, the reference table.
		struct SyntheticSizeQuery : SizeQuery
		{
			std::unordered_map<NodeId, std::optional<uint64_t>> Sizes;
			std::unordered_map<NodeId, std::vector<NodeId>> References;

			std::vector<NodeId> Dependencies(NodeId id) const override
			{
				auto it = References.find(id);
				return it == References.end() ? std::vector<NodeId>{} : it->second;
			}

			std::optional<uint64_t> CookedSize(NodeId id) const override
			{
				auto it = Sizes.find(id);
				return it == Sizes.end() ? std::nullopt : it->second;
			}
		};

	} // namespace

	TEST_CASE("size map: the inclusive closure counts the shared dependencies once")
	{
		// The diamond: 1 -> 2,3; 2 -> 4; 3 -> 4. The 4 counts ONCE in the
		// root's inclusive (the union closure), not once per branch.
		SyntheticSizeQuery query;
		query.Sizes = { { 1, 10 }, { 2, 20 }, { 3, 30 }, { 4, 40 } };
		query.References = { { 1, { 2, 3 } }, { 2, { 4 } }, { 3, { 4 } } };

		std::unordered_map<NodeId, SizeEntry> sizes;
		ComputeSizes(query, 1, sizes);

		CHECK(sizes.at(4).Inclusive == 40);
		CHECK(sizes.at(2).Inclusive == 60);
		CHECK(sizes.at(3).Inclusive == 70);
		CHECK(sizes.at(1).Inclusive == 100); // 10 + 20 + 30 + 40, the 4 once
		CHECK(sizes.at(1).Exclusive == 10);
		CHECK(sizes.at(4).SharedCount == 1); // referenced by both the 2 and the 3
	}

	TEST_CASE("size map: the broken reference contributes no bytes")
	{
		SyntheticSizeQuery query;
		query.Sizes = { { 1, 10 } }; // the 2 is unknown -> the nullopt
		query.References = { { 1, { 2 } } };

		std::unordered_map<NodeId, SizeEntry> sizes;
		ComputeSizes(query, 1, sizes);

		CHECK(sizes.at(2).Exclusive == 0);
		CHECK(sizes.at(2).Inclusive == 0);
		CHECK(sizes.at(1).Inclusive == 10);
	}

	TEST_CASE("size map: the squarify areas are proportional within epsilon")
	{
		SyntheticSizeQuery query;
		query.Sizes = { { 1, 100 }, { 2, 200 }, { 3, 300 } };
		query.References = { { 1, { 2, 3 } } };

		std::unordered_map<NodeId, SizeEntry> sizes;
		ComputeSizes(query, 1, sizes);

		std::vector<TreemapCell> cells;
		Squarify(query, sizes, 1, { 300.0f, 200.0f }, true, cells);

		// The top-level cells: the self (100) + the two deps (200 + 300).
		// The total area = 60000; each cell's area ~ its share.
		REQUIRE(cells.size() == 3);
		float totalArea = 0.0f;
		for (const TreemapCell& cell : cells)
			totalArea += cell.Rect.z * cell.Rect.w;
		CHECK(totalArea == doctest::Approx(60000.0f).epsilon(0.001));

		for (const TreemapCell& cell : cells)
		{
			if (cell.Id == 2)
				CHECK(cell.Rect.z * cell.Rect.w == doctest::Approx(20000.0f).epsilon(0.01));
			else if (cell.Id == 3)
				CHECK(cell.Rect.z * cell.Rect.w == doctest::Approx(30000.0f).epsilon(0.01));
		}
	}

	TEST_CASE("size map: the squarify is deterministic")
	{
		SyntheticSizeQuery query;
		for (NodeId id = 1; id <= 6; id++)
			query.Sizes[id] = 100 * id;
		query.References = { { 1, { 2, 3, 4 } }, { 2, { 5 } }, { 3, { 6 } } };

		std::unordered_map<NodeId, SizeEntry> sizes;
		ComputeSizes(query, 1, sizes);

		std::vector<TreemapCell> a, b;
		Squarify(query, sizes, 1, { 400.0f, 300.0f }, true, a);
		Squarify(query, sizes, 1, { 400.0f, 300.0f }, true, b);
		REQUIRE(a.size() == b.size());
		for (size_t i = 0; i < a.size(); i++)
		{
			CHECK(a[i].Id == b[i].Id);
			CHECK(a[i].Rect == b[i].Rect);
			CHECK(a[i].Depth == b[i].Depth);
			CHECK(a[i].Aggregate == b[i].Aggregate);
		}
	}

	TEST_CASE("size map: the squarify aggregates the tiny cells")
	{
		SyntheticSizeQuery query;
		query.Sizes[1] = 1000000;
		query.References[1] = {};
		for (NodeId id = 2; id <= 40; id++)
		{
			query.Sizes[id] = 1; // the tiny cells
			query.References[1].push_back(id);
		}

		std::unordered_map<NodeId, SizeEntry> sizes;
		ComputeSizes(query, 1, sizes);

		std::vector<TreemapCell> cells;
		Squarify(query, sizes, 1, { 100.0f, 100.0f }, true, cells);

		// The tiny 1-byte cells are far below the 4 px^2 floor -> one
		// "other" cell with the count.
		bool foundAggregate = false;
		for (const TreemapCell& cell : cells)
		{
			if (cell.Aggregate)
			{
				foundAggregate = true;
				CHECK(cell.AggregateCount == 39);
			}
		}
		CHECK(foundAggregate);
	}

	TEST_CASE("graph: the edge hover finds the nearest edge in its band")
	{
		GraphView view;
		view.Shape = EdgeShape::Straight;
		ViewNode a; a.Id = 1; a.Position = { 0.0f, 0.0f }; a.Size = { 100.0f, 40.0f };
		ViewNode b; b.Id = 2; b.Position = { 300.0f, 0.0f }; b.Size = { 100.0f, 40.0f };
		ViewNode c; c.Id = 3; c.Position = { 300.0f, 200.0f }; c.Size = { 100.0f, 40.0f };
		view.Nodes = { a, b, c };
		ViewEdge ab; ab.From = 1; ab.To = 2;
		ViewEdge ac; ac.From = 1; ac.To = 3;
		view.Edges = { ab, ac };
		// On the a -> b line (y = 20), 3 units off it.
		CHECK(NearestEdge(view, { 200.0f, 23.0f }, 6.0f) == 0);
		// Far from both.
		CHECK(NearestEdge(view, { 200.0f, 300.0f }, 6.0f) == -1);
	}

	TEST_CASE("graph: the rim, the snap and the border point")
	{
		ViewNode node; node.Position = { 0.0f, 0.0f }; node.Size = { 100.0f, 50.0f };
		CHECK(InNodeRim(node, { 3.0f, 25.0f }, 8.0f));
		CHECK(InNodeRim(node, { 50.0f, 46.0f }, 8.0f));
		CHECK_FALSE(InNodeRim(node, { 50.0f, 25.0f }, 8.0f)); // the middle
		CHECK_FALSE(InNodeRim(node, { -3.0f, 25.0f }, 8.0f)); // outside
		CHECK(SnapToGrid({ 23.0f, -9.0f }, 16.0f) == glm::vec2(16.0f, -16.0f));
		CHECK(SnapToGrid({ 23.0f, -9.0f }, 0.0f) == glm::vec2(23.0f, -9.0f));
		const glm::vec2 right = NodeBorderPoint(node, { 500.0f, 25.0f });
		CHECK(right.x == doctest::Approx(100.0f));
		CHECK(right.y == doctest::Approx(25.0f));
		const glm::vec2 up = NodeBorderPoint(node, { 50.0f, -400.0f });
		CHECK(up.y == doctest::Approx(0.0f));
	}

	TEST_CASE("graph: the minimap fits the nodes and the view, aspect kept")
	{
		GraphView view;
		ViewNode a; a.Id = 1; a.Position = { 0.0f, 0.0f }; a.Size = { 100.0f, 50.0f };
		ViewNode b; b.Id = 2; b.Position = { 900.0f, 250.0f }; b.Size = { 100.0f, 50.0f };
		view.Nodes = { a, b };
		// The view covers only the left part: the bounds are the nodes'.
		const MinimapLayout layout = ComputeMinimap(view, { 0.0f, 0.0f, 400.0f, 200.0f }, { 200.0f, 200.0f });
		REQUIRE(layout.Valid);
		CHECK(layout.WorldMin == glm::vec2(0.0f));
		CHECK(layout.Scale == doctest::Approx(0.2f)); // 1000 wide into 200
		CHECK(layout.Size.x == doctest::Approx(200.0f));
		CHECK(layout.Size.y == doctest::Approx(60.0f));
		// A view reaching past the nodes widens the bounds, by a margin at most.
		const MinimapLayout wide = ComputeMinimap(view, { -100.0f, 0.0f, 400.0f, 200.0f }, { 200.0f, 200.0f });
		CHECK(wide.WorldMin.x == doctest::Approx(-100.0f));
		const MinimapLayout far = ComputeMinimap(view, { -5000.0f, 0.0f, 400.0f, 200.0f }, { 200.0f, 200.0f });
		CHECK(far.WorldMin.x == doctest::Approx(-190.0f)); // 15% of 1000 + 40
		CHECK_FALSE(ComputeMinimap(GraphView{}, { 0.0f, 0.0f, 1.0f, 1.0f }, { 10.0f, 10.0f }).Valid);
	}

	TEST_CASE("graph: a polyline prefix ends where its share of the length does")
	{
		const std::vector<glm::vec2> line = { { 0.0f, 0.0f }, { 10.0f, 0.0f }, { 10.0f, 10.0f } };
		const std::vector<glm::vec2> half = PolylinePrefix(line, 0.5f);
		REQUIRE(half.size() == 2);
		CHECK(half.back() == glm::vec2(10.0f, 0.0f));
		const std::vector<glm::vec2> quarter = PolylinePrefix(line, 0.25f);
		CHECK(quarter.back() == glm::vec2(5.0f, 0.0f));
		const std::vector<glm::vec2> all = PolylinePrefix(line, 2.0f);
		CHECK(all.back() == glm::vec2(10.0f, 10.0f));
		CHECK(PolylinePrefix({}, 0.5f).empty());
	}

} // namespace graph_tests
} // namespace wisp
