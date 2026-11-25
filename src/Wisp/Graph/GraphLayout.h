// The graph layout.
//
// The pure layout layer: the schema provides a RefGraphQuery (the domain
// projection: the dependencies, the dependents, the node resolution), and
// the layout assigns the positions/sizes/flags into the GraphView. The
// two-sided tree/DAG layout is the Reference Viewer's; the IGraphLayout
// hook is the seam the future layered layouts (the RenderGraph viewer's
// F-1) plug into.

#pragma once

#include "Wisp/Graph/GraphView.h"

#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

namespace wisp {
namespace graph {

	// The node's display metadata for the sort keys (the deterministic
	// (category, title, id) ordering).
	struct RefNodeInfo
	{
		std::string Title;
		std::string Category; // the type (the sort key)
	};

	// The domain's reference-graph projection. The schema implements this
	// (a reference viewer over an asset registry, say); the suite drives
	// the layout with the synthetic implementations.
	struct RefGraphQuery
	{
		virtual ~RefGraphQuery() = default;
		virtual std::vector<NodeId> Dependencies(NodeId id) const = 0;
		virtual std::vector<NodeId> Dependents(NodeId id) const = 0;
		virtual bool Resolve(NodeId id, RefNodeInfo& out) const = 0;
	};

	struct LayoutParams
	{
		NodeId Root = 0;
		uint32_t MaxDepth = 2;    // the 1..3 (the per-side depth)
		uint32_t MaxBreadth = 20; // the per-node per-side child limit (the overflow node beyond it)
		bool ShowDuplicates = false;
	};

	// The view node id -> the domain asset id the layout fills for every
	// node it creates (the tree-mode occurrence ids and the overflow ids
	// are opaque hashes; the schema maps the clicks and the tooltips back
	// through this map - the hashes are not invertible).
	using NodeMap = std::unordered_map<NodeId, NodeId>;

	class IGraphLayout
	{
	public:
		virtual ~IGraphLayout() = default;
		virtual void Layout(GraphView& out, NodeMap& nodeToAsset,
			const RefGraphQuery& query, const LayoutParams& params) const = 0;
	};

	// The two-sided tree. The root at the column 0, the dependencies to
	// the left (the columns -1..-D), the dependents to the right (the
	// +1..+D).
	//
	// The DAG mode (the ShowDuplicates off): each asset appears once, at
	// its minimum BFS depth on the side of its smaller depth (the tie
	// breaks to the dependency side); the reachable-on-both-sides assets
	// carry BothSides; the edge set is EVERY reference among the visible
	// nodes, arrowed referencer -> referenced, and an edge whose head is
	// at the same or a closer column on the same side - or the root
	// itself - is a BackEdge (the cycles and the cross-links).
	//
	// The tree mode (on): every path expands; each occurrence is its own
	// view node (the NodeId = OccurrenceId(assetId, pathIndex), the
	// pathIndex being the per-layout occurrence counter); the expansion
	// stops at the depth D or when the asset already appears on the
	// current path (the cycle closes onto the ancestor occurrence with a
	// BackEdge).
	//
	// The breadth limit: each expanded node shows at most MaxBreadth
	// children per side; the rest collapse into one Overflow node (the
	// NodeId = OverflowId(parentId, side)) per side.
	class TwoSidedLayout : public IGraphLayout
	{
	public:
		void Layout(GraphView& out, NodeMap& nodeToAsset,
			const RefGraphQuery& query, const LayoutParams& params) const override;

		// The occurrence id for the tree mode (the layout and the schema
		// agree on it so the schema can map the occurrence back to the
		// asset through the NodeMap the layout fills).
		static NodeId OccurrenceId(NodeId assetId, uint64_t pathIndex);

		// The reserved path tags (the OccurrenceId second argument) for
		// the overflow nodes: the leftSide picks the side the hidden
		// children would have expanded into.
		static constexpr uint64_t kOverflowTagLeft = ~0ull;
		static constexpr uint64_t kOverflowTagRight = ~0ull - 1;
		static NodeId OverflowId(NodeId parentNodeId, bool leftSide);
	};

} // namespace graph
} // namespace wisp
