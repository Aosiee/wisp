// The graph view model's implementation.

#include "tepch.h"
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
