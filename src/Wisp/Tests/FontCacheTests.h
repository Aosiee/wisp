#pragma once

// The glyph atlas packs every size it is asked for without writing past
// its buffer - the Script Editor's zoom rasterizes each size anew, and a
// tall glyph on a shelf near the atlas bottom was written past it (a
// SIGABRT under the dev build's bounds assertions). The test bakes a
// real face at every zoom size and beyond, through the growth and the
// recycle at the maximum size.

#include "doctest/doctest.h"
#include "Wisp/FontCache.h"
#include "TinyEngine/CVar/CVarRegistry.h"
#include "TinyEngine/CVar/EngineCVars.h"
#include "TinyEngine/Core/Engine.h"

#include <filesystem>
#include <fstream>
#include <iterator>
#include <vector>

namespace wisp_font_tests {

	inline std::filesystem::path WorkspaceFile(const char* relative)
	{
		for (std::filesystem::path dir = std::filesystem::current_path(); !dir.empty(); dir = dir.parent_path())
		{
			if (std::filesystem::exists(dir / ".tinyengine-workspace"))
				return dir / relative;
			if (dir == dir.parent_path())
				break;
		}
		return relative;
	}

} // namespace wisp_font_tests

TEST_CASE("wisp font cache: every size packs inside the atlas - growth, then the recycle at the maximum")
{
	const std::filesystem::path path = wisp_font_tests::WorkspaceFile("plugins/engine/engine/content/fonts/adwaita/AdwaitaMono-Regular.ttf");
	std::ifstream in(path, std::ios::binary);
	REQUIRE_MESSAGE(in, "the test face is missing: " << path.string());
	const std::vector<uint8_t> ttf((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());

	// The glyphs rasterize at ui.scale (a registered cvar - unregistered it
	// reads 0 and every bitmap is empty).
	struct CVarEngine
	{
		TinyEngine::Engine engine{ TinyEngine::EngineDesc{
			.ReflectionRegistrars = {},
			.SubsystemRegistrars = { [](TinyEngine::Engine& e) { e.Add<TinyEngine::cvar::CVarRegistry>(); } },
			.CVarRegistrars = { &TinyEngine::cvar::RegisterEngineCVars },
		} };
		CVarEngine() { engine.Init(); }
		~CVarEngine() { engine.Shutdown(); }
	} cvars;
	REQUIRE(TinyEngine::cvars::ui_scale.Get() == doctest::Approx(1.0f));

	wisp::FontCache cache;
	const wisp::FontHandle font = cache.AddFont(ttf.data(), ttf.size());
	int missing = 0, outside = 0;
	auto bake = [&](uint32_t cp, float size) {
		const wisp::GlyphInfo* glyph = cache.GetGlyph(font, cp, size);
		if (!glyph)
			missing++;
		else if (glyph->AtlasU1 > 1.0f || glyph->AtlasV1 > 1.0f)
			outside++; // packed past the atlas edge (its pixels were written past the buffer)
	};
	// Small and large sizes interleaved in a fixed pseudo-random order
	// (a deterministic LCG), so a shelf of small glyphs near the bottom is
	// followed by a tall one, through the growth and past the maximum.
	uint32_t state = 12345u;
	auto next = [&]() { state = state * 1664525u + 1013904223u; return state >> 8; };
	for (int i = 0; i < 40000; i++)
	{
		const uint32_t cp = 33u + next() % 94u;
		const float size = (next() % 4u == 0u) ? 40.0f + (float)(next() % 160u) : 8.0f + (float)(next() % 25u) + 0.25f * (float)(next() % 4u);
		bake(cp, size);
	}
	CHECK(missing == 0);
	CHECK(outside == 0);
	CHECK(cache.GetAtlasWidth() <= 4096u);
}
