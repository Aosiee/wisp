// The size map.
//
// The pure, headless-testable half of the Size Map: the closure-size
// computation (the inclusive = the full dependency closure summed once
// per shared dependency) and the squarified treemap layout. The
// Reference Viewer's schema implements the SizeQuery over the
// AssetRegistry; the tinytest drives both with the synthetic queries.

#pragma once

#include "Wisp/Graph/GraphView.h"

#include <cstdint>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace wisp {
namespace graph {

	// The domain's size projection (the dependencies, the cooked sizes,
	// the cell label/colour).
	struct SizeQuery
	{
		virtual ~SizeQuery() = default;
		virtual std::vector<NodeId> Dependencies(NodeId id) const = 0;
		virtual std::optional<uint64_t> CookedSize(NodeId id) const = 0;
		virtual std::string Label(NodeId id) const { return std::to_string(id); }
		virtual glm::vec4 Colour(NodeId) const { return { 0.4f, 0.5f, 0.6f, 1.0f }; }
	};

	struct SizeEntry
	{
		uint64_t Exclusive = 0;    // the own cooked bytes (0 for the broken refs)
		uint64_t Inclusive = 0;    // the own bytes + the full closure (each shared once)
		uint32_t SharedCount = 0;  // "also referenced by N" - the other closure referencers
	};

	// Computes the sizes for the root's full closure (one entry per
	// visited asset; the cycles are safe - the in-progress placeholder
	// stops the recursion).
	void ComputeSizes(const SizeQuery& query, NodeId root, std::unordered_map<NodeId, SizeEntry>& out);

	struct TreemapCell
	{
		NodeId Id = 0;                        // 0 = the aggregated "other" cell
		glm::vec4 Rect{ 0.0f, 0.0f, 0.0f, 0.0f }; // x, y, w, h (the widget space)
		int Depth = 0;
		bool Aggregate = false;
		uint32_t AggregateCount = 0;          // the cells folded into the "other"
	};

	// The sub-4 px^2 cells aggregate into one "other" cell.
	constexpr float kMinCellAreaPx = 4.0f;

	// The squarified layout: each node's rectangle = its own exclusive
	// cell + one sub-rectangle per dependency subtree (the inclusive
	// bytes; the exclusive when useInclusive is false), recursing through
	// the full closure - a shared dependency draws once, under its first
	// parent in the deterministic visit order (the dependencies in the
	// order the query returns them).
	void Squarify(const SizeQuery& query, const std::unordered_map<NodeId, SizeEntry>& sizes,
		NodeId root, const glm::vec2& size, bool useInclusive, std::vector<TreemapCell>& out);

} // namespace graph
} // namespace wisp
