// The glyph atlas packs every size it is asked for without writing past
// its buffer - a code editor's zoom rasterizes each size anew, and a
// tall glyph on a shelf near the atlas bottom was written past it (a
// SIGABRT under the dev build's bounds assertions). The test bakes a
// real face at every zoom size and beyond, through the growth and the
// recycle at the maximum size.

#include "hamr/test.h"
#include "Wisp/FontCache.h"

#include <filesystem>
#include <fstream>
#include <iterator>
#include <vector>

TEST_CASE("wisp font cache: every size packs inside the atlas - growth, then the recycle at the maximum")
{
	// Beside this file, named as the build named it: absolute in Wisp's
	// own build, relative to the directory the suite runs from in a host's.
	const std::filesystem::path path = std::filesystem::path(__FILE__).parent_path() / "fonts" / "AdwaitaMono-Regular.ttf";
	std::ifstream in(path, std::ios::binary);
	REQUIRE_MESSAGE(in, "the test face is missing: " << path.string());
	const std::vector<uint8_t> ttf((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());

	// The glyphs rasterize at the cache's UI scale, 1 unless the host sets it.
	wisp::FontCache cache;
	REQUIRE(cache.GetUiScale() == hamr::test::Approx(1.0f));
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
