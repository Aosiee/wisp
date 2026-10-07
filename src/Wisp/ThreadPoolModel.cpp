// The Thread Pool model's implementation (the pure density compaction +
// the aggregation + the incremental build).

#include "tepch.h"
#include "Wisp/ThreadPoolModel.h"

#include <algorithm>
#include <cstring>

namespace TinyEngine {
namespace WispThreadPoolModel {

	std::vector<RenderBlock> MergeDensity(const std::vector<ZoneBlock>& zones, double pxPerNs)
	{
		std::vector<RenderBlock> blocks;
		blocks.reserve(zones.size());

		auto emitGroup = [&](uint64_t startNs, uint64_t endNs, uint64_t sumNs, uint32_t count, uint32_t dominant) {
			if (count == 0)
				return;
			RenderBlock b;
			b.StartNs = startNs;
			b.EndNs = std::max(endNs, startNs + 1); // the non-degenerate guard (the paint enforces the 1 px minimum)
			b.NameId = dominant;
			b.Count = count;
			b.SumNs = sumNs;
			b.IsDensity = count > 1;
			blocks.push_back(b);
		};

		// The open density group (the run of the sub-pixel zones).
		uint64_t groupStart = 0, groupEnd = 0, groupSum = 0;
		uint32_t groupCount = 0;
		uint32_t groupDominant = 0;
		uint64_t groupDominantSum = 0;
		std::unordered_map<uint32_t, uint64_t> groupTotals;

		auto openGroup = [&](const ZoneBlock& z) {
			groupStart = z.StartNs;
			groupEnd = z.EndNs;
			groupSum = z.EndNs - z.StartNs;
			groupCount = 1;
			groupDominant = z.NameId;
			groupDominantSum = z.EndNs - z.StartNs;
			groupTotals.clear();
			groupTotals[z.NameId] = z.EndNs - z.StartNs;
		};
		auto addToGroup = [&](const ZoneBlock& z) {
			groupEnd = z.EndNs;
			groupSum += z.EndNs - z.StartNs;
			groupCount++;
			uint64_t& total = groupTotals[z.NameId];
			total += z.EndNs - z.StartNs;
			if (total > groupDominantSum)
			{
				groupDominantSum = total;
				groupDominant = z.NameId;
			}
		};
		auto closeGroup = [&]() {
			emitGroup(groupStart, groupEnd, groupSum, groupCount, groupDominant);
			groupCount = 0;
			groupTotals.clear();
		};

		for (const ZoneBlock& z : zones)
		{
			const double widthPx = (double)(z.EndNs - z.StartNs) * pxPerNs;
			if (widthPx >= 1.0)
			{
				if (groupCount > 0)
					closeGroup();
				RenderBlock b;
				b.StartNs = z.StartNs;
				b.EndNs = z.EndNs;
				b.NameId = z.NameId;
				b.Window = z.Window;
				b.SumNs = z.EndNs - z.StartNs;
				blocks.push_back(b);
				continue;
			}

			// The sub-pixel zone: join the open group; when the group's span
			// reaches ~1 px it closes and the next sub-pixel zone opens a
			// fresh group (the density stays readable instead of one giant
			// run for the whole lane).
			if (groupCount == 0)
				openGroup(z);
			else
				addToGroup(z);
			if (groupCount > 0 && (double)(groupEnd - groupStart) * pxPerNs >= 1.0)
				closeGroup();
		}
		if (groupCount > 0)
			closeGroup();
		return blocks;
	}

	std::vector<TaskRow> AggregateTasks(const std::vector<ZoneBlock>& zones, uint64_t totalBusyNs)
	{
		std::unordered_map<uint32_t, TaskRow> byName;
		for (const ZoneBlock& z : zones)
		{
			TaskRow& row = byName[z.NameId];
			row.NameId = z.NameId;
			row.Count++;
			const uint64_t duration = z.EndNs - z.StartNs;
			row.TotalNs += duration;
			row.MaxNs = std::max(row.MaxNs, duration);
		}
		std::vector<TaskRow> rows;
		rows.reserve(byName.size());
		for (auto& [nameId, row] : byName)
		{
			row.Share = totalBusyNs > 0 ? (double)row.TotalNs / (double)totalBusyNs : 0.0;
			rows.push_back(row);
		}
		std::sort(rows.begin(), rows.end(), [](const TaskRow& a, const TaskRow& b) {
			if (a.TotalNs != b.TotalNs)
				return a.TotalNs > b.TotalNs;
			return a.NameId < b.NameId;
		});
		return rows;
	}

	uint64_t LaneSortKey(const std::string& name, uint32_t firstSight)
	{
		uint64_t category = 3;
		uint64_t numeric = 0;
		if (name.empty() || name == "main")
			category = 0;
		else if (name.rfind("Frame Submit", 0) == 0)
			category = 1;
		else if (name.rfind("loom-worker-", 0) == 0)
		{
			category = 2;
			numeric = (uint64_t)std::strtoull(name.c_str() + std::strlen("loom-worker-"), nullptr, 10);
		}
		else if (name.rfind("loom-", 0) == 0)
			category = 3;
		// The category dominates, then the numeric suffix, then the
		// first-sight order (the stable tie-break).
		return (category << 56) | ((numeric & 0xFFFF) << 40) | (firstSight & 0xFFFFFFFFFFull);
	}

	namespace {

		Lane& LaneFor(Model& model, uint32_t threadIdx)
		{
			auto it = model.ThreadToLane.find(threadIdx);
			if (it != model.ThreadToLane.end())
				return model.Lanes[it->second];
			Lane lane;
			lane.ThreadIdx = threadIdx;
			model.Lanes.push_back(std::move(lane));
			const uint32_t idx = (uint32_t)model.Lanes.size() - 1;
			model.ThreadToLane[threadIdx] = idx;
			model.ThreadOrder[threadIdx] = model.NextThreadOrder++;
			return model.Lanes[idx];
		}

	} // namespace

	void AppendChunks(Model& model, const std::vector<std::shared_ptr<TinyEngine::ProfilerFrameChunk>>& snapshot)
	{
		// The flight recorder evicted old chunks (its budget FIFO-drops the
		// oldest) - re-anchor everything (the pairing state included).
		if ((uint32_t)snapshot.size() < model.ProcessedChunks)
		{
			model.ProcessedChunks = 0;
			model.OpenBegins.clear();
			model.WaitOpen.clear();
			for (Lane& lane : model.Lanes)
			{
				lane.Zones.clear();
				lane.Waits.clear();
			}
		}

		for (size_t ci = model.ProcessedChunks; ci < snapshot.size(); ci++)
		{
			const auto& chunk = snapshot[ci];

			for (const auto& [id, name] : chunk->StringDelta)
			{
				auto it = model.Names.find(id);
				if (it == model.Names.end())
					model.Names.emplace(id, name);
			}
			for (const auto& [id, name] : chunk->ThreadDelta)
			{
				Lane& lane = LaneFor(model, id);
				if (lane.Name.empty() && !name.empty())
					lane.Name = name;
			}

			for (const auto& [threadIdx, events] : chunk->ThreadEvents)
			{
				auto& openBegins = model.OpenBegins[threadIdx];
				for (size_t i = 0; i < events.size(); i++)
				{
					const TinyEngine::ProfilerEvent& e = events[i];
					switch (e.Kind)
					{
					case TinyEngine::ProfilerEventKind::ZoneBegin:
						// The begin handle = the per-chunk event index + 1
						// (the same contract the engine's ZoneStack uses).
						openBegins.push_back({ (uint64_t)i + 1, e });
						break;
					case TinyEngine::ProfilerEventKind::ZoneEnd:
					{
						// The LIFO match (the engine's ZoneStack contract);
						// the loom zones are flat, so the top is the pair.
						if (!openBegins.empty() && openBegins.back().first == e.Value)
						{
							const TinyEngine::ProfilerEvent& b = openBegins.back().second;
							Lane& lane = LaneFor(model, threadIdx);
							ZoneBlock zone;
							zone.StartNs = b.TimeNs;
							zone.EndNs = e.TimeNs;
							zone.NameId = b.NameIdx;
							zone.Window = (uint8_t)b.Value; // the window tag (ZoneBeginNamed)
							lane.Zones.push_back(zone);
							openBegins.pop_back();
						}
						break;
					}
					case TinyEngine::ProfilerEventKind::ThreadStateMark:
					{
						if (e.Value == 0)
							model.WaitOpen[threadIdx] = e.TimeNs;
						else if (e.Value == 2)
						{
							auto wIt = model.WaitOpen.find(threadIdx);
							if (wIt != model.WaitOpen.end())
							{
								Lane& lane = LaneFor(model, threadIdx);
								lane.Waits.push_back({ wIt->second, e.TimeNs });
								model.WaitOpen.erase(wIt);
							}
						}
						break;
					}
					default:
						break;
					}
				}
			}
		}
		model.ProcessedChunks = snapshot.size();
	}

	void RebuildWindow(Model& model, uint64_t windowStartNs, uint64_t windowEndNs, double pxPerNs)
	{
		model.WindowStartNs = windowStartNs;
		model.WindowEndNs = windowEndNs;

		uint64_t totalBusyNs = 0;
		for (Lane& lane : model.Lanes)
		{
			// The straddling spans (the zones whose end is inside the window)
			// stay; the fully-expired head drops. The deques are ordered by
			// the append, and the zones are near-monotonic per thread.
			while (!lane.Zones.empty() && lane.Zones.front().EndNs <= windowStartNs)
				lane.Zones.pop_front();
			while (!lane.Waits.empty() && lane.Waits.front().second <= windowStartNs)
				lane.Waits.pop_front();

			// The zones within the window (the head may straddle the left
			// edge - it stays for the pairing continuity; the paint clamps).
			std::vector<ZoneBlock> windowZones;
			windowZones.reserve(lane.Zones.size());
			lane.BusyNs = 0;
			lane.WaitNs = 0;
			lane.TaskCount = 0;
			lane.LastNameId = 0;
			lane.LastEndNs = 0;
			for (const ZoneBlock& z : lane.Zones)
			{
				if (z.StartNs >= windowEndNs)
					break;
				const uint64_t start = std::max(z.StartNs, windowStartNs);
				const uint64_t end = std::min(z.EndNs, windowEndNs);
				if (end <= start)
					continue;
				ZoneBlock clipped = z;
				clipped.StartNs = start;
				clipped.EndNs = end;
				windowZones.push_back(clipped);
				lane.BusyNs += end - start;
				lane.TaskCount++;
				if (end > lane.LastEndNs)
				{
					lane.LastEndNs = end;
					lane.LastNameId = z.NameId;
				}
			}
			for (const auto& [start, end] : lane.Waits)
			{
				const uint64_t s = std::max(start, windowStartNs);
				const uint64_t e = std::min(end, windowEndNs);
				if (e > s)
					lane.WaitNs += e - s;
			}
			totalBusyNs += lane.BusyNs;
			lane.Blocks = MergeDensity(windowZones, pxPerNs);
		}

		// The deterministic lane order: the main thread, the Frame Submit
		// threads, the workers by index.
		std::sort(model.Lanes.begin(), model.Lanes.end(), [&](const Lane& a, const Lane& b) {
			return LaneSortKey(a.Name, model.ThreadOrder[a.ThreadIdx]) <
				LaneSortKey(b.Name, model.ThreadOrder[b.ThreadIdx]);
		});

		// The task table: the aggregation over the CLIPPED zones (the same
		// denominators as the per-lane BusyNs, so the shares sum to 1.0).
		std::vector<ZoneBlock> allZones;
		for (const Lane& lane : model.Lanes)
		{
			for (const ZoneBlock& z : lane.Zones)
			{
				if (z.StartNs >= windowEndNs)
					break;
				if (z.EndNs <= windowStartNs)
					continue;
				ZoneBlock clipped = z;
				clipped.StartNs = std::max(z.StartNs, windowStartNs);
				clipped.EndNs = std::min(z.EndNs, windowEndNs);
				allZones.push_back(clipped);
			}
		}
		model.TaskRows = AggregateTasks(allZones, totalBusyNs);
	}

} // namespace WispThreadPoolModel
} // namespace TinyEngine
