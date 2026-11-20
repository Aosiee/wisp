// The Thread Pool panel's data model.
//
// The pure, Wisp-free half of the panel (the editor's ThreadPoolPanel
// paints it): it turns the profiler's flight-recorder chunks into the
// per-thread lanes (the named zone blocks + the wait spans), compacts the
// sub-pixel zones into density blocks (the Tracy-style), and aggregates the
// per-task statistics for the table. The pure pieces (MergeDensity,
// AggregateTasks) are the tinytest targets - they live here, in the wisp
// plugin, because the tinytest links the wisp but not the editor plugin.

#pragma once

#include "TinyEngine/Profiler/Profiler.h"

#include <cstdint>
#include <deque>
#include <string>
#include <unordered_map>
#include <vector>

namespace TinyEngine {
namespace WispThreadPoolModel {

	// One named task execution (the zone begin/end pair), within the lane.
	struct ZoneBlock
	{
		uint64_t StartNs = 0;
		uint64_t EndNs = 0;
		uint32_t NameId = 0;  // the profiler's interned name id (0 = unnamed)
		uint8_t  Window = 0;  // the window-slot tag
	};

	// The density-merged render block (the sub-pixel zones compacted).
	struct RenderBlock
	{
		uint64_t StartNs = 0;
		uint64_t EndNs = 0;
		uint32_t NameId = 0;     // the individual zone, or the group's dominant task
		uint8_t  Window = 0;
		uint32_t Count = 1;      // the merged zone count (1 = an individual zone)
		uint64_t SumNs = 0;      // the summed zone duration (the density groups)
		bool     IsDensity = false;
	};

	// The per-task aggregate row (the panel's table).
	struct TaskRow
	{
		uint32_t NameId = 0;
		uint32_t Count = 0;
		uint64_t TotalNs = 0;
		uint64_t MaxNs = 0;
		double   Share = 0.0; // the share of the total busy time (0..1)
	};

	// One thread's lane: the rolling window of the zones + the waits, and
	// the rebuilt render blocks + the aggregates.
	struct Lane
	{
		uint32_t ThreadIdx = 0;
		std::string Name;
		std::deque<ZoneBlock> Zones;                    // the visible window (+ the straddle margin)
		std::deque<std::pair<uint64_t, uint64_t>> Waits; // the wait spans
		std::vector<RenderBlock> Blocks;                // the density-merged output
		uint64_t BusyNs = 0;   // the summed zone time in the window
		uint64_t WaitNs = 0;   // the summed wait time in the window
		uint32_t TaskCount = 0;
		uint32_t LastNameId = 0;
		uint64_t LastEndNs = 0;
	};

	struct Model
	{
		std::vector<Lane> Lanes;   // the deterministic order (the rebuild sorts)
		std::vector<TaskRow> TaskRows; // the sorted-by-cost table
		uint64_t WindowStartNs = 0;
		uint64_t WindowEndNs = 0;

		// The incremental pairing state (persists across the rebuilds). The
		// ingest cursor is the next chunk FrameIndex - never a recorder
		// position, which shifts as the recorder evicts .
		uint64_t NextFrameIndex = 0;
		bool Started = false;
		std::unordered_map<uint32_t, std::vector<std::pair<uint64_t, TinyEngine::ProfilerEvent>>> OpenBegins; // the per-thread LIFO (the begin handle -> the event)
		std::unordered_map<uint32_t, uint64_t> WaitOpen; // the per-thread open wait (the state-0 time)
		std::unordered_map<uint32_t, uint32_t> ThreadToLane; // the profiler thread id -> the lane index
		std::unordered_map<uint32_t, std::string> Names;     // the interned id -> the name (the chunk StringDeltas)
		uint32_t NextThreadOrder = 0; // the first-sight order (the fallback sort key)
		std::unordered_map<uint32_t, uint32_t> ThreadOrder;
	};

	// ---- The pure pieces (the tinytest targets) -------------------------------

	// Compacts the sorted zones into the render blocks: the zones wider than
	// 1 px pass through individually; the narrower runs merge into the
	// density blocks (the dominant task, the count, the summed duration),
	// with a 1 px minimum for the isolated sub-pixel zones.
	std::vector<RenderBlock> MergeDensity(const std::vector<ZoneBlock>& zones, double pxPerNs);

	// Aggregates the zones by the name id: the count, the total, the max,
	// and the share of the total busy time, sorted by the total descending.
	std::vector<TaskRow> AggregateTasks(const std::vector<ZoneBlock>& zones, uint64_t totalBusyNs);

	// ---- The incremental build ------------------------------------------------

	// Processes the chunks past the model's FrameIndex cursor (the zone/wait
	// pairing into the lanes) - a whole recorder snapshot or just the new
	// chunks (Profiler::CollectChunksSince) both work.
	void AppendChunks(Model& model, const std::vector<std::shared_ptr<TinyEngine::ProfilerFrameChunk>>& snapshot);

	// Rebuilds the window: trims the lanes to [windowStartNs, windowEndNs],
	// re-runs the density merge at the current px/ns, and refreshes the
	// aggregates + the task table. The lanes end in the deterministic order
	// (the main thread, the submit threads by name, the workers by index).
	void RebuildWindow(Model& model, uint64_t windowStartNs, uint64_t windowEndNs, double pxPerNs);

	// The lane sort key: 0 = the main/app thread, 1 = the "Frame Submit"
	// dedicated threads, 2 = the "loom-worker-N" workers, 3 = the rest.
	// The tie-break: the numeric suffix, then the first-sight order.
	uint64_t LaneSortKey(const std::string& name, uint32_t firstSight);

} // namespace WispThreadPoolModel
} // namespace TinyEngine
