// The graph view model's implementation.

#include "Wisp/Graph/GraphView.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace wisp {
namespace graph {

	namespace {

		const ViewNode* FindNode(const GraphView& view, NodeId id)
		{
			for (const ViewNode& node : view.Nodes)
				if (node.Id == id)
					return &node;
			return nullptr;
		}

		glm::vec2 BezierPoint(glm::vec2 p0, glm::vec2 p1, glm::vec2 p2, glm::vec2 p3, float t)
		{
			const float u = 1.0f - t;
			return u * u * u * p0 + 3.0f * u * u * t * p1 + 3.0f * u * t * t * p2 + t * t * t * p3;
		}

		float PointSegmentDistance(glm::vec2 p, glm::vec2 a, glm::vec2 b)
		{
			const glm::vec2 ab = b - a;
			const float lengthSq = glm::dot(ab, ab);
			if (lengthSq <= 0.0f)
				return glm::length(p - a);
			const float t = std::max(0.0f, std::min(1.0f, glm::dot(p - a, ab) / lengthSq));
			return glm::length(p - (a + ab * t));
		}

	} // namespace

	namespace {

		// Where the ray from a rect's centre along `dir` leaves the rect.
		glm::vec2 RectExit(const ViewNode& node, const glm::vec2& centre, const glm::vec2& dir)
		{
			const glm::vec2 half = node.Size * 0.5f;
			const float tx = std::abs(dir.x) > 1e-6f ? half.x / std::abs(dir.x) : 1e30f;
			const float ty = std::abs(dir.y) > 1e-6f ? half.y / std::abs(dir.y) : 1e30f;
			return centre + dir * std::min(tx, ty);
		}

		// Which side of a node the centre-to-centre line toward `toward`
		// leaves through: 0 left, 1 right, 2 top, 3 bottom.
		int ExitSide(const ViewNode& node, const glm::vec2& toward)
		{
			const glm::vec2 half = node.Size * 0.5f;
			const glm::vec2 dir = toward - (node.Position + half);
			const float tx = std::abs(dir.x) > 1e-6f ? half.x / std::abs(dir.x) : 1e30f;
			const float ty = std::abs(dir.y) > 1e-6f ? half.y / std::abs(dir.y) : 1e30f;
			if (tx <= ty)
				return dir.x < 0.0f ? 0 : 1;
			return dir.y < 0.0f ? 2 : 3;
		}

		// Where an edge meets one of its nodes: the centre line's exit,
		// moved along that side so every edge meeting the side has its own
		// point - ordered by where its other end lies (no crossings at the
		// node), a two-way pair each on the right of its own direction.
		glm::vec2 EdgePort(const GraphView& view, size_t edgeIndex, bool head)
		{
			const ViewEdge& edge = view.Edges[edgeIndex];
			const ViewNode* node = FindNode(view, head ? edge.To : edge.From);
			const ViewNode* other = FindNode(view, head ? edge.From : edge.To);
			const glm::vec2 centre = node->Position + node->Size * 0.5f;
			const glm::vec2 otherCentre = other->Position + other->Size * 0.5f;
			const int side = ExitSide(*node, otherCentre);
			const glm::vec2 axis = side < 2 ? glm::vec2(0.0f, 1.0f) : glm::vec2(1.0f, 0.0f);

			struct Port
			{
				float Key;
				float Preference;
				size_t Index;
			};
			std::vector<Port> ports;
			for (size_t i = 0; i < view.Edges.size(); i++)
			{
				const ViewEdge& e = view.Edges[i];
				if (e.From == e.To || (e.From != node->Id && e.To != node->Id))
					continue;
				const ViewNode* far = FindNode(view, e.From == node->Id ? e.To : e.From);
				const ViewNode* from = FindNode(view, e.From);
				const ViewNode* to = FindNode(view, e.To);
				if (!far || !from || !to)
					continue;
				const glm::vec2 farCentre = far->Position + far->Size * 0.5f;
				if (ExitSide(*node, farCentre) != side)
					continue;
				const glm::vec2 dir = (to->Position + to->Size * 0.5f) - (from->Position + from->Size * 0.5f);
				const glm::vec2 right = glm::length(dir) > 1e-4f ? glm::vec2(-dir.y, dir.x) / glm::length(dir) : glm::vec2(0.0f);
				ports.push_back({ glm::dot(farCentre, axis), std::round(glm::dot(right, axis) * 4.0f), i });
			}
			std::stable_sort(ports.begin(), ports.end(), [](const Port& a, const Port& b) {
				if (std::abs(a.Key - b.Key) > 0.5f)
					return a.Key < b.Key;
				if (a.Preference != b.Preference)
					return a.Preference < b.Preference;
				return a.Index < b.Index;
			});
			int rank = 0;
			for (size_t k = 0; k < ports.size(); k++)
				if (ports[k].Index == edgeIndex)
					rank = (int)k;
			const float sideLength = side < 2 ? node->Size.y : node->Size.x;
			const int count = (int)ports.size();
			const float spacing = count > 1 ? std::min(14.0f, sideLength * 0.8f / (float)(count - 1)) : 0.0f;
			const float offset = ((float)rank - (float)(count - 1) * 0.5f) * spacing;

			glm::vec2 dir = otherCentre - centre;
			const float length = glm::length(dir);
			dir = length > 1e-4f ? dir / length : glm::vec2(1.0f, 0.0f);
			glm::vec2 point = RectExit(*node, centre, dir) + axis * offset;
			// Kept on the side, clear of the rounded corners.
			const float lo = glm::dot(node->Position, axis) + 6.0f;
			const float hi = glm::dot(node->Position + node->Size, axis) - 6.0f;
			const float along = std::clamp(glm::dot(point, axis), std::min(lo, hi), std::max(lo, hi));
			point += axis * (along - glm::dot(point, axis));
			return point;
		}

	}

	int EdgeLane(const GraphView& view, size_t edgeIndex)
	{
		if (edgeIndex >= view.Edges.size())
			return 0;
		const ViewEdge& edge = view.Edges[edgeIndex];
		int rank = 0, same = 0;
		bool reverse = false;
		for (size_t i = 0; i < view.Edges.size(); i++)
		{
			const ViewEdge& other = view.Edges[i];
			if (other.From == edge.From && other.To == edge.To)
			{
				same++;
				if (i < edgeIndex)
					rank++;
			}
			else if (other.From == edge.To && other.To == edge.From)
				reverse = true;
		}
		return same > 1 || reverse ? rank + 1 : 0;
	}

	void EdgeControlPoints(const GraphView& view, size_t edgeIndex,
		glm::vec2& p0, glm::vec2& p1, glm::vec2& p2, glm::vec2& p3)
	{
		p0 = p1 = p2 = p3 = glm::vec2(0.0f);
		if (edgeIndex >= view.Edges.size())
			return;
		const ViewEdge& edge = view.Edges[edgeIndex];
		const ViewNode* from = FindNode(view, edge.From);
		const ViewNode* to = FindNode(view, edge.To);
		if (!from || !to)
			return;

		if (view.Shape == EdgeShape::Straight)
		{
			p0 = EdgePort(view, edgeIndex, false);
			p3 = EdgePort(view, edgeIndex, true);
			p1 = glm::mix(p0, p3, 1.0f / 3.0f);
			p2 = glm::mix(p0, p3, 2.0f / 3.0f);
			return;
		}

		// The right edge of the from-node to the left edge of the to-node,
		// with the horizontal control offsets (the canvas's painter uses
		// the same curve).
		const float fromX = from->Position.x + from->Size.x;
		const float toX = to->Position.x;
		const float fromY = from->Position.y + from->Size.y * 0.5f;
		const float toY = to->Position.y + to->Size.y * 0.5f;

		p0 = { fromX, fromY };
		p3 = { toX, toY };
		const float dx = std::max(40.0f, (toX - fromX) * 0.5f);
		p1 = { fromX + dx, fromY };
		p2 = { toX - dx, toY };
		// The same-row edges bow the middle of the curve UP over the gap
		// between the cards so the connector reads as an arc, not a
		// floating straight line.
		if (std::abs(toY - fromY) < 1.0f)
		{
			p1.y -= 20.0f;
			p2.y -= 20.0f;
		}
	}

	float EdgeDistance(const GraphView& view, size_t edgeIndex, glm::vec2 point, int samples)
	{
		if (edgeIndex >= view.Edges.size())
			return std::numeric_limits<float>::max();
		const ViewEdge& edge = view.Edges[edgeIndex];
		if (!FindNode(view, edge.From) || !FindNode(view, edge.To))
			return std::numeric_limits<float>::max();

		glm::vec2 p0, p1, p2, p3;
		EdgeControlPoints(view, edgeIndex, p0, p1, p2, p3);

		float best = std::numeric_limits<float>::max();
		glm::vec2 prev = p0;
		for (int i = 1; i <= samples; i++)
		{
			const glm::vec2 current = BezierPoint(p0, p1, p2, p3, (float)i / (float)samples);
			best = std::min(best, PointSegmentDistance(point, prev, current));
			prev = current;
		}
		return best;
	}

	int NearestEdge(const GraphView& view, glm::vec2 world, float band)
	{
		int best = -1;
		float bestDistance = band;
		for (size_t i = 0; i < view.Edges.size(); i++)
		{
			const float distance = EdgeDistance(view, i, world, 16);
			if (distance <= bestDistance)
			{
				bestDistance = distance;
				best = (int)i;
			}
		}
		return best;
	}

	glm::vec2 NodeBorderPoint(const ViewNode& node, const glm::vec2& toward)
	{
		const glm::vec2 centre = node.Position + node.Size * 0.5f;
		glm::vec2 dir = toward - centre;
		const float length = glm::length(dir);
		dir = length > 1e-4f ? dir / length : glm::vec2(1.0f, 0.0f);
		return RectExit(node, centre, dir);
	}

	bool InNodeRim(const ViewNode& node, glm::vec2 world, float rim)
	{
		const glm::vec2 lo = world - node.Position;
		const glm::vec2 hi = node.Position + node.Size - world;
		if (lo.x < 0.0f || lo.y < 0.0f || hi.x < 0.0f || hi.y < 0.0f)
			return false;
		return std::min(std::min(lo.x, lo.y), std::min(hi.x, hi.y)) <= rim;
	}

	glm::vec2 SnapToGrid(glm::vec2 p, float grid)
	{
		if (grid <= 0.0f)
			return p;
		return glm::round(p / grid) * grid;
	}

	MinimapLayout ComputeMinimap(const GraphView& view, const glm::vec4& visibleWorld, glm::vec2 maxSize)
	{
		MinimapLayout layout;
		if (view.Nodes.empty() || maxSize.x <= 0.0f || maxSize.y <= 0.0f)
			return layout;
		glm::vec2 lo(std::numeric_limits<float>::max()), hi(std::numeric_limits<float>::lowest());
		for (const ViewNode& node : view.Nodes)
		{
			lo = glm::min(lo, node.Position);
			hi = glm::max(hi, node.Position + node.Size);
		}
		// The view may reach a little past the nodes (a margin), not further:
		// a view panned far away would shrink the graph to a dot.
		const glm::vec2 margin = (hi - lo) * 0.15f + glm::vec2(40.0f);
		lo = glm::max(lo - margin, glm::min(lo, glm::vec2(visibleWorld.x, visibleWorld.y)));
		hi = glm::min(hi + margin, glm::max(hi, glm::vec2(visibleWorld.x + visibleWorld.z, visibleWorld.y + visibleWorld.w)));
		const glm::vec2 extent = glm::max(hi - lo, glm::vec2(1.0f));
		layout.Scale = std::min(maxSize.x / extent.x, maxSize.y / extent.y);
		layout.WorldMin = lo;
		layout.Size = extent * layout.Scale;
		layout.Valid = true;
		return layout;
	}

	std::vector<glm::vec2> PolylinePrefix(const std::vector<glm::vec2>& points, float fraction)
	{
		std::vector<glm::vec2> out;
		if (points.empty())
			return out;
		float total = 0.0f;
		for (size_t i = 1; i < points.size(); i++)
			total += glm::length(points[i] - points[i - 1]);
		float remaining = std::clamp(fraction, 0.0f, 1.0f) * total;
		out.push_back(points.front());
		for (size_t i = 1; i < points.size(); i++)
		{
			const float segment = glm::length(points[i] - points[i - 1]);
			if (segment >= remaining)
			{
				out.push_back(segment > 0.0f ? glm::mix(points[i - 1], points[i], remaining / segment) : points[i]);
				return out;
			}
			remaining -= segment;
			out.push_back(points[i]);
		}
		return out;
	}

	HitResult HitTest(const GraphView& view, const ViewTransform& transform, glm::vec2 screenPos)
	{
		HitResult result;
		const float scale = ZoomScale(transform.ZoomLevel);
		// The canvas maps the world -> screen as screen = world * scale + pan,
		// so the inverse is world = (screen - pan) / scale.
		const glm::vec2 world = (screenPos - transform.Pan) / scale;

		// Nodes first (the reverse draw order: the later nodes paint on
		// top, so they win the hit).
		for (size_t i = view.Nodes.size(); i-- > 0;)
		{
			const ViewNode& node = view.Nodes[i];
			if (world.x >= node.Position.x && world.x <= node.Position.x + node.Size.x
				&& world.y >= node.Position.y && world.y <= node.Position.y + node.Size.y)
			{
				result.Kind = HitResult::Kind::Node;
				result.Node = node.Id;
				return result;
			}
		}

		// Then the edges (the ~6 px screen-space pick band).
		const float pickBand = 6.0f / scale;
		for (size_t i = 0; i < view.Edges.size(); i++)
		{
			if (EdgeDistance(view, i, world, 16) <= pickBand)
			{
				result.Kind = HitResult::Kind::Edge;
				result.Edge = i;
				return result;
			}
		}

		result.Kind = HitResult::Kind::Canvas;
		return result;
	}

} // namespace graph
} // namespace wisp
