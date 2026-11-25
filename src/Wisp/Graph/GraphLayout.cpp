// The two-sided layout implementation.

#include "Wisp/Graph/GraphLayout.h"

#include <algorithm>
#include <cmath>
#include <deque>
#include <map>
#include <unordered_map>

namespace wisp {
namespace graph {

	namespace {

		constexpr float kNodeWidth = 160.0f;
		constexpr float kNodeHeight = 96.0f; // the content zone + the two text lines
		constexpr float kColumnGap = 120.0f;
		constexpr float kRowGap = 24.0f;

		// The splitmix64-ish mix so the occurrence ids stay stable and
		// distinct (the schema maps them back the same way).
		NodeId HashId(NodeId assetId, uint64_t pathIndex)
		{
			uint64_t x = assetId ^ (pathIndex * 0x9E3779B97F4A7C15ull);
			x = (x ^ (x >> 30)) * 0xBF58476D1CE4E5B9ull;
			x = (x ^ (x >> 27)) * 0x94D049BB133111EBull;
			x = x ^ (x >> 31);
			return x == 0 ? 1 : x;
		}

		// The side sign: the left (the dependencies) steps the columns
		// toward -inf, the right (the dependents) toward +inf.
		int SideSign(bool leftSide)
		{
			return leftSide ? -1 : 1;
		}

		std::vector<NodeId> SideChildren(const RefGraphQuery& query, bool leftSide, NodeId id)
		{
			return leftSide ? query.Dependencies(id) : query.Dependents(id);
		}

		// The deterministic child order key: the category, the title, the
		// id (the resolvable sorts before the broken).
		bool ChildLess(const RefGraphQuery& query, NodeId a, NodeId b)
		{
			RefNodeInfo ia, ib;
			const bool ra = query.Resolve(a, ia);
			const bool rb = query.Resolve(b, ib);
			if (ra != rb) return ra;
			if (ia.Category != ib.Category) return ia.Category < ib.Category;
			if (ia.Title != ib.Title) return ia.Title < ib.Title;
			return a < b;
		}

		// A BackEdge is a cycle or a cross-link: the head sits at the same
		// or a closer column than the tail measured along the side's flow
		// (the left side flows outward, the right side flows inward toward
		// the root), or the head is the root itself. The root's own
		// references and the cross-side spans are normal edges.
		bool IsBackEdge(int tailCol, int headCol)
		{
			if (tailCol == 0) return false;
			if (tailCol < 0)
				return headCol >= tailCol && headCol <= 0;
			return headCol >= tailCol;
		}

		// The internal node record the layout builds before the packing
		// (the ViewNodes carry the positions; this carries the columns).
		struct PlacedNode
		{
			NodeId Id = 0;
			NodeId Asset = 0;
			int Column = 0;
			uint32_t Flags = 0;
			bool Overflow = false;
			int Hidden = 0;
			float X = 0.0f;
			float Y = 0.0f;
		};

		// The DAG-mode BFS on one side with the breadth cap: the min depth
		// per asset and the (parent, side) hidden-child counts.
		struct SideDag
		{
			std::unordered_map<NodeId, int> Depth;        // the asset -> the min depth (1-based; the root = 0)
			std::unordered_map<NodeId, int> HiddenCount;  // the parent -> the children beyond the breadth limit
		};

		void RunSideDag(const RefGraphQuery& query, const LayoutParams& params,
			bool leftSide, SideDag& out)
		{
			out.Depth[params.Root] = 0;
			std::vector<NodeId> frontier;
			frontier.push_back(params.Root);
			for (int depth = 1; depth <= (int)params.MaxDepth && !frontier.empty(); depth++)
			{
				std::vector<NodeId> next;
				for (NodeId parent : frontier)
				{
					std::vector<NodeId> children = SideChildren(query, leftSide, parent);
					std::sort(children.begin(), children.end(), [&](NodeId a, NodeId b) { return ChildLess(query, a, b); });
					const size_t shown = std::min<size_t>(children.size(), (size_t)params.MaxBreadth);
					for (size_t i = 0; i < shown; i++)
					{
						const NodeId child = children[i];
						if (child == params.Root || out.Depth.count(child))
							continue;
						out.Depth[child] = depth;
						next.push_back(child);
					}
					if (children.size() > shown)
					{
						// The hidden children that are not already visible at a
						// shallower depth through another path (those would show
						// nothing new, so they stay out of the +N count).
						int hidden = 0;
						for (size_t i = shown; i < children.size(); i++)
							if (!out.Depth.count(children[i]))
								hidden++;
						if (hidden > 0)
							out.HiddenCount[parent] = hidden;
					}
				}
				frontier = std::move(next);
			}
		}

		// The overflow node for a parent (the +N more node on one side).
		PlacedNode MakeOverflow(NodeId parentId, NodeId parentAsset, bool leftSide, int column, int hidden)
		{
			PlacedNode overflow;
			overflow.Id = TwoSidedLayout::OverflowId(parentId, leftSide);
			overflow.Asset = parentAsset;
			overflow.Column = column;
			overflow.Flags = NodeFlags::Overflow | (leftSide ? NodeFlags::OverflowLeft : NodeFlags(0));
			overflow.Overflow = true;
			overflow.Hidden = hidden;
			return overflow;
		}

		// The DAG mode: one node per asset at its smaller side depth (the
		// tie to the dependency side), every reference among the visible
		// nodes as an edge, and the per-side overflow nodes.
		void LayoutDag(const RefGraphQuery& query, const LayoutParams& params,
			std::vector<PlacedNode>& nodes, std::vector<ViewEdge>& edges, NodeMap& nodeToAsset)
		{
			SideDag left, right;
			RunSideDag(query, params, true, left);
			RunSideDag(query, params, false, right);

			std::unordered_map<NodeId, int> column;

			PlacedNode root;
			root.Id = params.Root;
			root.Asset = params.Root;
			root.Flags = NodeFlags::Root;
			nodes.push_back(root);
			nodeToAsset[root.Id] = params.Root;
			column[params.Root] = 0;

			// The canonical creation order (the child sort key), so the
			// barycentre ties break on the child order, not the hash order.
			const auto sortedAssets = [&](const SideDag& side, NodeId skip) {
				std::vector<NodeId> assets;
				for (const auto& [asset, depth] : side.Depth)
					if (asset != skip)
						assets.push_back(asset);
				std::sort(assets.begin(), assets.end(), [&](NodeId a, NodeId b) { return ChildLess(query, a, b); });
				return assets;
			};

			for (NodeId asset : sortedAssets(left, params.Root))
			{
				PlacedNode node;
				node.Id = asset;
				node.Asset = asset;
				const auto rightIt = right.Depth.find(asset);
				const int depth = left.Depth.at(asset);
				if (rightIt != right.Depth.end())
				{
					node.Flags = NodeFlags::BothSides;
					node.Column = depth <= rightIt->second ? -depth : rightIt->second;
				}
				else
				{
					node.Column = -depth;
				}
				column[asset] = node.Column;
				nodes.push_back(node);
				nodeToAsset[node.Id] = asset;
			}
			for (NodeId asset : sortedAssets(right, params.Root))
			{
				if (column.count(asset))
					continue;
				PlacedNode node;
				node.Id = asset;
				node.Asset = asset;
				node.Column = right.Depth.at(asset);
				column[asset] = node.Column;
				nodes.push_back(node);
				nodeToAsset[node.Id] = asset;
			}

			// The edges: EVERY reference among the visible nodes (the tree
			// edges are the forward ones; the cross-links and the cycles
			// become the back edges).
			for (const PlacedNode& node : nodes)
			{
				if (node.Overflow)
					continue;
				std::vector<NodeId> deps = query.Dependencies(node.Asset);
				std::sort(deps.begin(), deps.end(), [&](NodeId a, NodeId b) { return ChildLess(query, a, b); });
				for (NodeId dep : deps)
				{
					if (dep == node.Asset)
						continue;
					const auto depIt = column.find(dep);
					if (depIt == column.end())
						continue;
					ViewEdge edge;
					edge.Kind = EdgeKind::Hard;
					edge.From = node.Id;
					edge.To = dep;
					if (node.Id == params.Root || column.at(node.Asset) < 0)
						edge.Flags |= EdgeFlags::Outgoing;
					if (IsBackEdge(column.at(node.Asset), depIt->second))
						edge.Flags |= EdgeFlags::BackEdge;
					edges.push_back(edge);
				}
			}

			// The overflow nodes (one per node per side that hid children).
			for (const PlacedNode& node : nodes)
			{
				for (bool leftSide : { true, false })
				{
					const SideDag& side = leftSide ? left : right;
					const auto hiddenIt = side.HiddenCount.find(node.Asset);
					if (hiddenIt == side.HiddenCount.end())
						continue;
					const int depth = side.Depth.at(node.Asset);
					const PlacedNode overflow = MakeOverflow(node.Id, node.Asset, leftSide,
						SideSign(leftSide) * (depth + 1), hiddenIt->second);
					nodes.push_back(overflow);
					nodeToAsset[overflow.Id] = node.Asset;

					ViewEdge edge;
					edge.Kind = EdgeKind::Hard;
					if (leftSide)
					{
						edge.From = node.Id;
						edge.To = overflow.Id;
						edge.Flags = EdgeFlags::Outgoing;
					}
					else
					{
						edge.From = overflow.Id;
						edge.To = node.Id;
					}
					edges.push_back(edge);
				}
			}
		}

		// The tree-mode occurrence.
		struct TreeOcc
		{
			uint64_t Index = 0;    // the path index (the OccurrenceId second argument)
			NodeId Id = 0;
			NodeId Asset = 0;
			int Column = 0;
			int64_t Parent = -1;   // the index into the occurrence list (the path)
		};

		// The tree mode: one side's full path expansion. Every occurrence
		// is its own node; a child that already appears on the current
		// path closes the cycle onto the ancestor occurrence with a
		// BackEdge; the children beyond the breadth limit collapse into
		// the per-occurrence overflow node.
		void RunSideTree(const RefGraphQuery& query, const LayoutParams& params, bool leftSide,
			std::vector<TreeOcc>& occs, std::vector<PlacedNode>& nodes,
			std::vector<ViewEdge>& edges, NodeMap& nodeToAsset, uint64_t& nextIndex)
		{
			std::deque<size_t> queue;
			queue.push_back(0);
			while (!queue.empty())
			{
				const size_t oi = queue.front();
				queue.pop_front();
				const TreeOcc occ = occs[oi];
				const int depth = std::abs(occ.Column);
				if (depth >= (int)params.MaxDepth)
					continue;

				std::vector<NodeId> children = SideChildren(query, leftSide, occ.Asset);
				std::sort(children.begin(), children.end(), [&](NodeId a, NodeId b) { return ChildLess(query, a, b); });
				const size_t shown = std::min<size_t>(children.size(), (size_t)params.MaxBreadth);
				for (size_t i = 0; i < shown; i++)
				{
					const NodeId child = children[i];
					if (child == occ.Asset)
						continue;

					// The cycle: the nearest ancestor occurrence with the
					// same asset (the paths are duplicate-free, so at most
					// one matches).
					int64_t ancestor = -1;
					for (int64_t up = (int64_t)oi; up >= 0; up = occs[up].Parent)
					{
						if (occs[up].Asset == child)
						{
							ancestor = up;
							break;
						}
					}
					if (ancestor >= 0)
					{
						ViewEdge edge;
						edge.Kind = EdgeKind::Hard;
						edge.Flags = EdgeFlags::BackEdge | (leftSide ? EdgeFlags::Outgoing : EdgeFlags(0));
						if (leftSide)
						{
							edge.From = occ.Id;
							edge.To = occs[ancestor].Id;
						}
						else
						{
							edge.From = occs[ancestor].Id;
							edge.To = occ.Id;
						}
						edges.push_back(edge);
						continue;
					}

					TreeOcc childOcc;
					childOcc.Index = nextIndex++;
					childOcc.Id = TwoSidedLayout::OccurrenceId(child, childOcc.Index);
					childOcc.Asset = child;
					childOcc.Column = occ.Column + SideSign(leftSide);
					childOcc.Parent = (int64_t)oi;
					occs.push_back(childOcc);

					PlacedNode node;
					node.Id = childOcc.Id;
					node.Asset = child;
					node.Column = childOcc.Column;
					nodes.push_back(node);
					nodeToAsset[node.Id] = child;

					ViewEdge edge;
					edge.Kind = EdgeKind::Hard;
					if (leftSide)
					{
						edge.From = occ.Id;
						edge.To = childOcc.Id;
						edge.Flags = EdgeFlags::Outgoing;
					}
					else
					{
						edge.From = childOcc.Id;
						edge.To = occ.Id;
					}
					edges.push_back(edge);
					queue.push_back(occs.size() - 1);
				}

				if (children.size() > shown)
				{
					PlacedNode overflow = MakeOverflow(occ.Id, occ.Asset, leftSide,
						occ.Column + SideSign(leftSide), (int)(children.size() - shown));
					nodes.push_back(overflow);
					nodeToAsset[overflow.Id] = occ.Asset;

					ViewEdge edge;
					edge.Kind = EdgeKind::Hard;
					if (leftSide)
					{
						edge.From = occ.Id;
						edge.To = overflow.Id;
						edge.Flags = EdgeFlags::Outgoing;
					}
					else
					{
						edge.From = overflow.Id;
						edge.To = occ.Id;
					}
					edges.push_back(edge);
				}
			}
		}

		// The packing: the barycentre sort (the one pass per column, the
		// root outward) and the row packing (the columns vertically
		// centered on the root's midline).
		void PackAndFill(GraphView& out, const RefGraphQuery& query,
			std::vector<PlacedNode>& nodes, std::vector<ViewEdge>& edges)
		{
			std::map<int, std::vector<PlacedNode*>> columns;
			for (PlacedNode& node : nodes)
				columns[node.Column].push_back(&node);

			std::unordered_map<NodeId, int> row;
			for (auto& [col, list] : columns)
				for (size_t i = 0; i < list.size(); i++)
					row[list[i]->Id] = (int)i;

			const int maxDepth = std::max(std::abs(columns.begin()->first), std::abs(columns.rbegin()->first));

			// The parent adjacency (built once): the incoming = the
			// To -> Froms (the left side's parents), the outgoing = the
			// From -> Tos (the right side's parents - the dependent edges
			// point child -> parent). The per-sort edge scans used to be
			// O(N log N x E); the adjacency keeps each pass O(total edges).
			std::unordered_map<NodeId, std::vector<NodeId>> incoming, outgoing;
			for (const ViewEdge& edge : edges)
			{
				incoming[edge.To].push_back(edge.From);
				outgoing[edge.From].push_back(edge.To);
			}

			for (int depth = 1; depth <= maxDepth; depth++)
			{
				for (int sign = -1; sign <= 1; sign += 2)
				{
					auto it = columns.find(sign * depth);
					if (it == columns.end())
						continue;
					std::vector<PlacedNode*>& list = it->second;
					const auto& adjacency = sign < 0 ? incoming : outgoing;
					std::stable_sort(list.begin(), list.end(), [&](PlacedNode* a, PlacedNode* b) {
						auto bary = [&](PlacedNode* n) {
							const auto it = adjacency.find(n->Id);
							if (it == adjacency.end())
								return 0.0f;
							float sum = 0.0f;
							int count = 0;
							for (NodeId parentId : it->second)
							{
								auto rowIt = row.find(parentId);
								if (rowIt == row.end())
									continue;
								sum += (float)rowIt->second;
								count++;
							}
							return count > 0 ? sum / (float)count : 0.0f;
						};
						const float ba = bary(a), bb = bary(b);
						if (ba != bb) return ba < bb;
						return false;
					});
					for (size_t i = 0; i < list.size(); i++)
						row[list[i]->Id] = (int)i;
				}
			}

			for (auto& [col, list] : columns)
			{
				const float x = (float)col * (kNodeWidth + kColumnGap);
				const float totalH = (float)list.size() * kNodeHeight + (float)(list.size() - 1) * kRowGap;
				float y = kNodeHeight * 0.5f - totalH * 0.5f;
				for (PlacedNode* node : list)
				{
					node->X = x;
					node->Y = y;
					y += kNodeHeight + kRowGap;
				}
			}

			out.Nodes.clear();
			out.Nodes.reserve(nodes.size());
			for (const PlacedNode& node : nodes)
			{
				ViewNode view;
				view.Id = node.Id;
				view.Position = { node.X, node.Y };
				view.Size = { kNodeWidth, kNodeHeight };
				view.Flags = node.Flags;
				if (node.Overflow)
				{
					view.Title = "+" + std::to_string(node.Hidden) + " more";
					view.Subtitle = "raise the breadth limit";
				}
				else
				{
					RefNodeInfo info;
					if (query.Resolve(node.Asset, info))
					{
						view.Title = info.Title;
						view.Subtitle = info.Category;
					}
					else
					{
						view.Title = "Missing asset";
						view.Subtitle = std::to_string(node.Asset);
						view.Flags |= NodeFlags::Broken;
					}
				}
				out.Nodes.push_back(view);
			}

			// The deterministic draw order (the later nodes paint on top):
			// the column, then the row.
			std::sort(out.Nodes.begin(), out.Nodes.end(), [](const ViewNode& a, const ViewNode& b) {
				if (a.Position.x != b.Position.x) return a.Position.x < b.Position.x;
				return a.Position.y < b.Position.y;
			});

			// The deterministic edge order (the hit-test returns the index).
			std::sort(edges.begin(), edges.end(), [](const ViewEdge& a, const ViewEdge& b) {
				if (a.From != b.From) return a.From < b.From;
				if (a.To != b.To) return a.To < b.To;
				return a.Flags < b.Flags;
			});
		}

	} // namespace

	NodeId TwoSidedLayout::OccurrenceId(NodeId assetId, uint64_t pathIndex)
	{
		return HashId(assetId, pathIndex);
	}

	NodeId TwoSidedLayout::OverflowId(NodeId parentNodeId, bool leftSide)
	{
		return OccurrenceId(parentNodeId, leftSide ? kOverflowTagLeft : kOverflowTagRight);
	}

	void TwoSidedLayout::Layout(GraphView& out, NodeMap& nodeToAsset,
		const RefGraphQuery& query, const LayoutParams& params) const
	{
		out.Nodes.clear();
		out.Edges.clear();
		nodeToAsset.clear();

		std::vector<PlacedNode> nodes;
		std::vector<ViewEdge> edges;

		if (params.ShowDuplicates)
		{
			// The two side trees share the root occurrence.
			std::vector<TreeOcc> occs;
			TreeOcc rootOcc;
			rootOcc.Index = 0;
			rootOcc.Id = OccurrenceId(params.Root, 0);
			rootOcc.Asset = params.Root;
			occs.push_back(rootOcc);

			PlacedNode root;
			root.Id = rootOcc.Id;
			root.Asset = params.Root;
			root.Flags = NodeFlags::Root;
			nodes.push_back(root);
			nodeToAsset[root.Id] = params.Root;

			uint64_t nextIndex = 1;
			RunSideTree(query, params, true, occs, nodes, edges, nodeToAsset, nextIndex);
			RunSideTree(query, params, false, occs, nodes, edges, nodeToAsset, nextIndex);
		}
		else
		{
			LayoutDag(query, params, nodes, edges, nodeToAsset);
		}

		PackAndFill(out, query, nodes, edges);
		out.Edges = std::move(edges);
	}

} // namespace graph
} // namespace wisp
