// The Thread Pool model's pure-piece tests (the density compaction + the
// task aggregation). Registered in tinytest.

#pragma once

#include "doctest/doctest.h"
#include "Wisp/ThreadPoolModel.h"

#include <cstdint>
#include <vector>

namespace TinyEngine {
namespace threadpool_model_tests {

	using namespace WispThreadPoolModel;

	TEST_CASE("threadpool model: density — a wide zone passes through individually")
	{
		// 2 px at 1 px/ms = 2 ms = 2,000,000 ns.
		std::vector<ZoneBlock> zones;
		zones.push_back({ 1000000, 3000000, 7, 0 });
		std::vector<RenderBlock> blocks = MergeDensity(zones, 1.0 / 1e6);
		REQUIRE(blocks.size() == 1);
		CHECK(blocks[0].NameId == 7);
		CHECK(blocks[0].StartNs == 1000000);
		CHECK(blocks[0].EndNs == 3000000);
		CHECK(blocks[0].Count == 1);
		CHECK(blocks[0].IsDensity == false);
	}

	TEST_CASE("threadpool model: density — a sub-pixel run merges with the count and the sum")
	{
		// Four 100 us zones (0.1 px each at 1 px/ms) in a row.
		std::vector<ZoneBlock> zones;
		for (uint64_t i = 0; i < 4; i++)
			zones.push_back({ i * 200000, i * 200000 + 100000, 5, 0 });
		std::vector<RenderBlock> blocks = MergeDensity(zones, 1.0 / 1e6);
		REQUIRE(blocks.size() == 1);
		CHECK(blocks[0].IsDensity == true);
		CHECK(blocks[0].Count == 4);
		CHECK(blocks[0].SumNs == 400000);
		CHECK(blocks[0].NameId == 5);
	}

	TEST_CASE("threadpool model: density — the isolated sub-pixel zone keeps a 1 px minimum")
	{
		std::vector<ZoneBlock> zones;
		zones.push_back({ 1000000, 1100000, 3, 0 }); // 0.1 px at 1 px/ms
		std::vector<RenderBlock> blocks = MergeDensity(zones, 1.0 / 1e6);
		REQUIRE(blocks.size() == 1);
		CHECK(blocks[0].IsDensity == false);
		CHECK(blocks[0].Count == 1);
		CHECK(blocks[0].SumNs == 100000);
		CHECK(blocks[0].EndNs > blocks[0].StartNs);
	}

	TEST_CASE("threadpool model: density — the dominant task colours the group")
	{
		// Two tiny zones of task A (300 us total) + one tiny of task B (100 us).
		std::vector<ZoneBlock> zones;
		zones.push_back({ 0, 100000, 1, 0 });
		zones.push_back({ 150000, 300000, 1, 0 });
		zones.push_back({ 350000, 450000, 2, 0 });
		std::vector<RenderBlock> blocks = MergeDensity(zones, 1.0 / 1e6);
		REQUIRE(blocks.size() == 1);
		CHECK(blocks[0].Count == 3);
		CHECK(blocks[0].NameId == 1); // task 1 dominates (300 us vs 100 us)
	}

	TEST_CASE("threadpool model: density — a wide zone splits the runs")
	{
		// tiny, tiny, WIDE (2 ms), tiny, tiny -> three blocks.
		std::vector<ZoneBlock> zones;
		zones.push_back({ 0, 100000, 1, 0 });
		zones.push_back({ 200000, 300000, 1, 0 });
		zones.push_back({ 1000000, 3000000, 2, 0 });
		zones.push_back({ 4000000, 4100000, 3, 0 });
		zones.push_back({ 4200000, 4300000, 3, 0 });
		std::vector<RenderBlock> blocks = MergeDensity(zones, 1.0 / 1e6);
		REQUIRE(blocks.size() == 3);
		CHECK(blocks[0].IsDensity == true);
		CHECK(blocks[0].Count == 2);
		CHECK(blocks[1].IsDensity == false);
		CHECK(blocks[1].NameId == 2);
		CHECK(blocks[2].IsDensity == true);
		CHECK(blocks[2].Count == 2);
		CHECK(blocks[2].NameId == 3);
	}

	TEST_CASE("threadpool model: density — a long run splits at each pixel")
	{
		// Forty 100 us zones: at 1 px/ms the group closes every ~10 zones
		// (the 1 px span), so the run splits into ~4 blocks, not one giant.
		std::vector<ZoneBlock> zones;
		for (uint64_t i = 0; i < 40; i++)
			zones.push_back({ i * 120000, i * 120000 + 100000, 9, 0 });
		std::vector<RenderBlock> blocks = MergeDensity(zones, 1.0 / 1e6);
		CHECK(blocks.size() >= 2);
		uint32_t total = 0;
		for (const RenderBlock& b : blocks)
		{
			CHECK(b.IsDensity == true);
			total += b.Count;
		}
		CHECK(total == 40);
	}

	TEST_CASE("threadpool model: tasks — the aggregate counts, totals, max and share")
	{
		std::vector<ZoneBlock> zones;
		zones.push_back({ 0, 2000000, 1, 0 });     // task 1: 2 ms, one hit
		zones.push_back({ 3000000, 4000000, 2, 0 }); // task 2: 1 ms
		zones.push_back({ 5000000, 7000000, 2, 0 }); // task 2: 2 ms (max)
		std::vector<TaskRow> rows = AggregateTasks(zones, 5000000);
		REQUIRE(rows.size() == 2);
		// Sorted by the total descending: task 2 (3 ms) first.
		CHECK(rows[0].NameId == 2);
		CHECK(rows[0].Count == 2);
		CHECK(rows[0].TotalNs == 3000000);
		CHECK(rows[0].MaxNs == 2000000);
		CHECK(rows[0].Share == doctest::Approx(0.6));
		CHECK(rows[1].NameId == 1);
		CHECK(rows[1].Count == 1);
		CHECK(rows[1].TotalNs == 2000000);
		CHECK(rows[1].MaxNs == 2000000);
		CHECK(rows[1].Share == doctest::Approx(0.4));
	}

	TEST_CASE("threadpool model: chunks — the zone pairs and the waits build a lane")
	{
		// A synthetic chunk mimicking the real recorder shape: one thread
		// with a zone begin/end pair (the handle = the per-chunk index +
		// 1) and a wait pair (the state marks 0 -> 2).
		using TinyEngine::ProfilerFrameChunk;
		using TinyEngine::ProfilerEvent;
		using TinyEngine::ProfilerEventKind;
		auto chunk = std::make_shared<ProfilerFrameChunk>();
		chunk->FrameStartNs = 1000000;
		chunk->FrameEndNs = 2000000;
		chunk->StringDelta.push_back({ 3, "SyntheticPass" });
		chunk->ThreadDelta.push_back({ 5, "loom-worker-2" });

		std::vector<ProfilerEvent> events;
		ProfilerEvent b;
		b.Kind = ProfilerEventKind::ZoneBegin;
		b.NameIdx = 3;
		b.TimeNs = 1100000;
		events.push_back(b);
		ProfilerEvent e;
		e.Kind = ProfilerEventKind::ZoneEnd;
		e.Value = 1; // the begin's handle = the index + 1
		e.TimeNs = 1400000;
		events.push_back(e);
		ProfilerEvent w0;
		w0.Kind = ProfilerEventKind::ThreadStateMark;
		w0.Value = 0;
		w0.TimeNs = 1500000;
		events.push_back(w0);
		ProfilerEvent w2;
		w2.Kind = ProfilerEventKind::ThreadStateMark;
		w2.Value = 2;
		w2.TimeNs = 1600000;
		events.push_back(w2);
		chunk->ThreadEvents.push_back({ 5, events });

		Model model;
		std::vector<std::shared_ptr<ProfilerFrameChunk>> snapshot;
		snapshot.push_back(chunk);
		AppendChunks(model, snapshot);
		RebuildWindow(model, 1000000, 2000000, 1.0 / 1e6);

		REQUIRE(model.Lanes.size() == 1);
		CHECK(model.Lanes[0].Name == "loom-worker-2");
		CHECK(model.Lanes[0].Zones.size() == 1);
		CHECK(model.Lanes[0].Zones[0].NameId == 3);
		CHECK(model.Lanes[0].Zones[0].StartNs == 1100000);
		CHECK(model.Lanes[0].Zones[0].EndNs == 1400000);
		CHECK(model.Lanes[0].Waits.size() == 1);
		CHECK(model.Lanes[0].BusyNs == 300000);
		CHECK(model.Lanes[0].WaitNs == 100000);
		CHECK(model.TaskRows.size() == 1);
		CHECK(model.TaskRows[0].NameId == 3);
		CHECK(model.TaskRows[0].Count == 1);
		CHECK(model.TaskRows[0].TotalNs == 300000);
		REQUIRE(model.Lanes[0].Blocks.size() == 1);
		CHECK(model.Lanes[0].Blocks[0].NameId == 3);
	}

} // namespace threadpool_model_tests
} // namespace TinyEngine
