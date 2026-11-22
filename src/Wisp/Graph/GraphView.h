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
	};

	enum EdgeFlags : uint32_t
	{
		BackEdge = 1 << 0, // the head is at the same or a closer column than the tail (cycles, cross-links)
		Outgoing = 1 << 1, // the reference direction away from the root (else incoming)
		Highlighted = 1 << 2, // the schema's selected edge (drawn in the accent, thicker)
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
	};

	struct GraphView
	{
		std::vector<ViewNode> Nodes;
		std::vector<ViewEdge> Edges;
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

	// The distance from the point to the bezier (the sampled polyline).
	float EdgeDistance(const GraphView& view, size_t edgeIndex, glm::vec2 point, int samples);

} // namespace graph
} // namespace wisp
