// The graph view model.
//
// The pure, headless-testable half of the graph canvas: exactly what the
// canvas draws and hit-tests, nothing more. No graph DATA model lives
// here (a future visual-script / shader / dialogue graph is an asset with
// its own reflected, serialized, server-evaluated model - the server
// never links Wisp). The canvas projects a domain through a GraphSchema
// (the implementations live with their consumer) into this view.

#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include <glm/glm.hpp>

namespace wisp {
namespace graph {

	using NodeId = uint64_t; // opaque to the canvas; the schema maps it to its domain

	enum class EdgeKind : uint8_t { Hard, Soft }; // Soft reserved for SDEP; styled dashed

	enum NodeFlags : uint32_t
	{
		Selected     = 1 << 0,
		Broken       = 1 << 1, // the reference resolves to nothing (the lint L7 shape)
		Overflow     = 1 << 2, // the "+N more" breadth-limit node
		BothSides    = 1 << 3, // reachable as both a dependency and a dependent
		Root         = 1 << 4,
		OverflowLeft = 1 << 5, // the Overflow node whose hidden children sit on the dependency side
		Active       = 1 << 6, // live (a playing state): a lit outline and a glow
		NodeWarning  = 1 << 7, // a badge in the corner (the schema's tooltip says why)
		Pill         = 1 << 8, // fully rounded ends (an entry or an any-state marker)
	};

	enum EdgeFlags : uint32_t
	{
		BackEdge = 1 << 0, // the head is at the same or a closer column than the tail (cycles, cross-links)
		Outgoing = 1 << 1, // the reference direction away from the root (else incoming)
		Highlighted = 1 << 2, // the schema's selected edge (drawn in the accent, thicker)
		Emphasis = 1 << 3, // drawn in the accent at rest (an entry arrow)
		EdgeWarning = 1 << 4, // drawn in the warning colour (the schema's tooltip says why)
	};

	struct ViewNode
	{
		NodeId Id = 0;
		glm::vec2 Position{}; // the top-left, in the graph space
		glm::vec2 Size{};
		std::string Title;
		std::string Subtitle;
		glm::vec4 Fill{ 0.16f, 0.16f, 0.22f, 1.0f };
		glm::vec4 Accent{ 0.35f, 0.5f, 0.75f, 1.0f };
		uint64_t Thumbnail = 0; // the Wisp texture id (0 = none)
		uint32_t Flags = 0;
	};

	struct ViewEdge
	{
		NodeId From = 0, To = 0;
		EdgeKind Kind = EdgeKind::Hard;
		uint32_t Flags = 0;
		std::string Label; // drawn as a chip at the middle of a Straight edge
	};

	// Curved: the column layout's right-edge-to-left-edge beziers (the
	// reference graphs). Straight: border-to-border lines between the nodes,
	// the edges meeting one side of a node spread along it (state machines,
	// where nodes sit anywhere and transitions run both ways).
	enum class EdgeShape : uint8_t { Curved, Straight };

	struct GraphView
	{
		std::vector<ViewNode> Nodes;
		std::vector<ViewEdge> Edges;
		EdgeShape Shape = EdgeShape::Curved;
	};

	// The discrete zoom table: continuous zoom would rasterize every glyph
	// at a fresh pixel size per wheel tick into the shared FontCache atlas.
	// The index is the ZoomLevel.
	inline const float kZoomLevels[15] = {
		0.10f, 0.125f, 0.15f, 0.20f, 0.25f, 0.33f, 0.50f, 0.67f,
		0.75f, 0.875f, 1.0f, 1.25f, 1.5f, 1.75f, 2.0f
	};
	constexpr int kDefaultZoomLevel = 10; // the 1.0x

	inline float ZoomScale(int zoomLevel)
	{
		if (zoomLevel < 0) return kZoomLevels[0];
		if (zoomLevel >= 15) return kZoomLevels[14];
		return kZoomLevels[zoomLevel];
	}

	struct ViewTransform
	{
		glm::vec2 Pan{};   // the screen-space offset: screen = world * scale + pan
		int ZoomLevel = kDefaultZoomLevel;
	};

	struct HitResult
	{
		enum class Kind : uint8_t { None, Canvas, Node, Edge } Kind = Kind::None;
		NodeId Node = 0;
		size_t Edge = 0; // the index into the Edges (valid when Kind == Edge)
	};

	// The pure hit test: node > edge > canvas. The screen position maps
	// into the graph space through the transform; the edge distance uses
	// the cubic-bezier sampling the canvas also uses for the draw.
	HitResult HitTest(const GraphView& view, const ViewTransform& transform, glm::vec2 screenPos);

	// The shared bezier helpers (the hit test and the Phase-2 painter agree
	// on the curve shape): the control points for the edge between the
	// from-node's right edge and the to-node's left edge.
	void EdgeControlPoints(const GraphView& view, size_t edgeIndex,
		glm::vec2& p0, glm::vec2& p1, glm::vec2& p2, glm::vec2& p3);

	// A Straight edge's lane: 0 when it is the only edge between its two
	// nodes, else 1, 2, ... - its label is pushed to the right of its
	// direction. (The lines themselves stay apart because every edge meeting
	// a node's side gets its own point on it.)
	int EdgeLane(const GraphView& view, size_t edgeIndex);

	// The distance from the point to the bezier (the sampled polyline).
	float EdgeDistance(const GraphView& view, size_t edgeIndex, glm::vec2 point, int samples);

	// The nearest edge within `band` of a graph-space point; -1 when none.
	int NearestEdge(const GraphView& view, glm::vec2 world, float band);

	// Where the line from the node's centre toward `toward` leaves its rect.
	glm::vec2 NodeBorderPoint(const ViewNode& node, const glm::vec2& toward);

	// Inside the node and within `rim` of its border (where a connect drag
	// starts).
	bool InNodeRim(const ViewNode& node, glm::vec2 world, float rim);

	// The point on the grid nearest `p` (a grid of 0 leaves it).
	glm::vec2 SnapToGrid(glm::vec2 p, float grid);

	// The minimap's mapping: the nodes' bounds, widened toward the visible
	// rect (x, y, w, h) by at most a margin, fitted into `maxSize` with the
	// aspect kept.
	// minimap = (world - WorldMin) * Scale.
	struct MinimapLayout
	{
		bool Valid = false;
		glm::vec2 WorldMin{};
		float Scale = 1.0f;
		glm::vec2 Size{};
	};
	MinimapLayout ComputeMinimap(const GraphView& view, const glm::vec4& visibleWorld, glm::vec2 maxSize);

	// The first `fraction` of a polyline by length, and the point it ends at.
	std::vector<glm::vec2> PolylinePrefix(const std::vector<glm::vec2>& points, float fraction);

} // namespace graph
} // namespace wisp
