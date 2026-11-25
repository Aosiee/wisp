// The size map's implementation.

#include "Wisp/Graph/SizeMap.h"

#include <algorithm>
#include <cmath>
#include <functional>
#include <unordered_set>

namespace wisp {
namespace graph {

	namespace {

		struct LayoutItem
		{
			NodeId Id = 0;
			uint64_t Size = 0;
			glm::vec4 Rect{ 0.0f, 0.0f, 0.0f, 0.0f };
			int Depth = 0;
			bool Aggregate = false;
			uint32_t AggregateCount = 0;
		};

		// The squarified row layout (Bruls et al.): the rows run along
		// the rectangle's longer side, each item's length proportional to
		// its size, the row grown while the worst aspect ratio improves.
		// The output carries each item's rect (the caller decides which
		// become cells and which recurse).
		void LayoutList(const std::vector<LayoutItem>& items, size_t begin,
			const glm::vec2& pos, const glm::vec2& size, std::vector<LayoutItem>& out)
		{
			if (begin >= items.size() || size.x <= 0.0f || size.y <= 0.0f)
				return;
			uint64_t grandTotal = 0;
			for (size_t i = begin; i < items.size(); i++)
				grandTotal += items[i].Size;
			if (grandTotal == 0)
				return;

			const bool horizontal = size.x >= size.y;
			const float rowLength = horizontal ? size.x : size.y;
			const float area = size.x * size.y;

			size_t end = begin + 1;
			uint64_t rowTotal = items[begin].Size;
			auto worstFor = [&](size_t newEnd, uint64_t newTotal) {
				const float thickness = area * (float)newTotal / (float)grandTotal / rowLength;
				float worst = 0.0f;
				for (size_t i = begin; i < newEnd; i++)
				{
					const float len = rowLength * (float)items[i].Size / (float)newTotal;
					worst = std::max(worst, std::max(thickness / len, len / thickness));
				}
				return worst;
			};
			float bestWorst = worstFor(end, rowTotal);
			for (size_t i = begin + 1; i < items.size(); i++)
			{
				const size_t newEnd = i + 1;
				const uint64_t newTotal = rowTotal + items[i].Size;
				const float w = worstFor(newEnd, newTotal);
				if (w <= bestWorst)
				{
					bestWorst = w;
					end = newEnd;
					rowTotal = newTotal;
				}
				else
					break;
			}

			const float thickness = area * (float)rowTotal / (float)grandTotal / rowLength;
			float offset = 0.0f;
			for (size_t i = begin; i < end; i++)
			{
				const float len = rowLength * (float)items[i].Size / (float)rowTotal;
				glm::vec2 cellPos = pos;
				glm::vec2 cellSize;
				if (horizontal)
				{
					cellPos.x += offset;
					cellSize = { len, thickness };
				}
				else
				{
					cellPos.y += offset;
					cellSize = { thickness, len };
				}
				LayoutItem laid = items[i];
				laid.Rect = { cellPos.x, cellPos.y, cellSize.x, cellSize.y };
				out.push_back(laid);
				offset += len;
			}

			glm::vec2 restPos = pos;
			glm::vec2 restSize = size;
			if (horizontal)
			{
				restPos.y += thickness;
				restSize.y -= thickness;
			}
			else
			{
				restPos.x += thickness;
				restSize.x -= thickness;
			}
			LayoutList(items, end, restPos, restSize, out);
		}

	} // namespace

	void ComputeSizes(const SizeQuery& query, NodeId root, std::unordered_map<NodeId, SizeEntry>& out)
	{
		out.clear();

		// The closure SETS (each shared dependency counted once - the
		// union, not the per-branch sum). The in-progress placeholder
		// (`closures.count(id) != 0` before the recursion) is the cycle
		// guard.
		std::unordered_map<NodeId, std::unordered_set<NodeId>> closures;
		std::unordered_map<NodeId, uint32_t> referencers;

		std::function<void(NodeId)> compute = [&](NodeId id) {
			if (closures.count(id))
				return;
			closures[id] = { id };
			for (NodeId dep : query.Dependencies(id))
			{
				compute(dep);
				closures[id].insert(closures[dep].begin(), closures[dep].end());
				referencers[dep] += 1;
			}
		};
		compute(root);

		for (auto& [id, closureSet] : closures)
		{
			SizeEntry entry;
			for (NodeId member : closureSet)
			{
				const uint64_t size = query.CookedSize(member).value_or(0);
				if (member == id)
					entry.Exclusive = size;
				entry.Inclusive += size;
			}
			entry.SharedCount = referencers.count(id) && referencers[id] > 1 ? referencers[id] - 1 : 0;
			out[id] = entry;
		}
	}

	void Squarify(const SizeQuery& query, const std::unordered_map<NodeId, SizeEntry>& sizes,
		NodeId root, const glm::vec2& size, bool useInclusive, std::vector<TreemapCell>& out)
	{
		out.clear();
		std::unordered_set<NodeId> placed;
		placed.insert(root);

		std::function<void(NodeId, const glm::vec2&, const glm::vec2&, int)> visit =
			[&](NodeId id, const glm::vec2& pos, const glm::vec2& size, int depth)
		{
			std::vector<LayoutItem> items;
			auto selfIt = sizes.find(id);
			const uint64_t selfSize = (selfIt != sizes.end()) ? selfIt->second.Exclusive : 0;
			if (selfSize > 0)
				items.push_back({ id, selfSize, glm::vec4{}, depth, false, 0 });
			for (NodeId dep : query.Dependencies(id))
			{
				if (placed.count(dep))
					continue;
				placed.insert(dep);
				auto depIt = sizes.find(dep);
				const uint64_t depSize = (depIt != sizes.end())
					? (useInclusive ? depIt->second.Inclusive : depIt->second.Exclusive)
					: 0;
				if (depSize == 0)
					continue;
				items.push_back({ dep, depSize, glm::vec4{}, depth + 1, false, 0 });
			}

			// The aggregation: the sub-4 px^2 cells fold into one "other"
			// cell (the node's own cell never aggregates).
			{
				const float area = size.x * size.y;
				uint64_t total = 0;
				for (const LayoutItem& item : items)
					total += item.Size;
				if (total > 0 && area > 0.0f)
				{
					std::vector<LayoutItem> merged;
					uint64_t smallSum = 0;
					uint32_t smallCount = 0;
					for (const LayoutItem& item : items)
					{
						const float cellArea = area * (float)item.Size / (float)total;
						if (item.Id != id && cellArea < kMinCellAreaPx)
						{
							smallSum += item.Size;
							smallCount++;
						}
						else
							merged.push_back(item);
					}
					if (smallCount > 0)
						merged.push_back({ 0, smallSum, glm::vec4{}, depth + 1, true, smallCount });
					items = std::move(merged);
				}
			}

			// The deterministic order: the size descending, the id ascending.
			std::sort(items.begin(), items.end(), [](const LayoutItem& a, const LayoutItem& b) {
				if (a.Size != b.Size) return a.Size > b.Size;
				return a.Id < b.Id;
			});

			std::vector<LayoutItem> laid;
			LayoutList(items, 0, pos, size, laid);

			// The self becomes a cell; the dependencies recurse into
			// their rects; the aggregates become cells.
			for (const LayoutItem& item : laid)
			{
				if (item.Id == id)
				{
					out.push_back({ item.Id, item.Rect, item.Depth, false, 0 });
					continue;
				}
				if (item.Id == 0)
				{
					out.push_back({ 0, item.Rect, item.Depth, true, item.AggregateCount });
					continue;
				}
				visit(item.Id, { item.Rect.x, item.Rect.y }, { item.Rect.z, item.Rect.w }, depth + 1);
			}
		};

		visit(root, { 0.0f, 0.0f }, size, 0);
	}

} // namespace graph
} // namespace wisp
