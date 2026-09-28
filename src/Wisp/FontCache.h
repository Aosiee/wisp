#pragma once

#include "Wisp/Core.h"
#include <glm/glm.hpp>
#include <string>
#include <vector>
#include <unordered_map>
#include <cstdint>

namespace wisp {

// ---- Glyph Info ------------------------------------------------------------

struct GlyphInfo
{
	uint32_t Codepoint = 0;
	float    AdvanceX  = 0.0f;   // advance width in pixels
	float    BearingX  = 0.0f;   // left side bearing
	float    BearingY  = 0.0f;   // top side bearing
	float    Width     = 0.0f;   // bitmap width in pixels
	float    Height    = 0.0f;   // bitmap height in pixels
	float    AtlasU0   = 0.0f, AtlasV0 = 0.0f; // atlas UV top-left
	float    AtlasU1   = 0.0f, AtlasV1 = 0.0f; // atlas UV bottom-right
};

// ---- Font Handle -----------------------------------------------------------

using FontHandle = uint32_t;
constexpr FontHandle InvalidFont = 0;

// ---- Font Metrics ----------------------------------------------------------

struct FontMetrics
{
	float Ascent  = 0.0f;
	float Descent = 0.0f;
	float LineGap = 0.0f;
	float LineHeight = 0.0f;  // ascent - descent + lineGap
};

// ---- FontCache -------------------------------------------------------------
//
// Loads TTF/OTF fonts (via stb_truetype), rasterizes glyphs on demand into a
// growing atlas texture (shelf bin-packing), and provides text measurement.

class FontCache
{
public:
	FontCache();
	~FontCache();

	// Load a font from memory. Returns a handle for subsequent API calls.
	FontHandle AddFont(const uint8_t* ttfData, size_t ttfSize);

	// Retrieves or rasterizes the glyph for a codepoint at the given pixel
	// size. The size is part of the cache key - callers always pass the
	// size they intend to draw/measure at (the face no longer carries a
	// mutable "current" size that cross-talked between widgets).
	const GlyphInfo* GetGlyph(FontHandle font, uint32_t codepoint, float pixelSize);

	// Measure text without rasterizing missing glyphs.
	glm::vec2 MeasureText(FontHandle font, const std::string& text, float pixelSize, float wrapWidth = -1.0f, WrapMode wrapMode = WrapMode::Character);

	// Atlas image data (RGBA, width * height * 4 bytes).
	// Valid after at least one glyph has been rasterized.
	const uint8_t* GetAtlasData() const { return m_AtlasData.data(); }
	uint32_t     GetAtlasWidth() const  { return m_AtlasWidth; }
	uint32_t     GetAtlasHeight() const { return m_AtlasHeight; }
	bool     IsAtlasDirty()   const { return m_AtlasDirty; }
	// The renderer must clear the dirty flag after rebuilding the GPU
	// atlas, or every glyph rasterisation leaves it set forever and the
	// atlas rebuilds (with a full vkDeviceWaitIdle) every single frame.
	void     ClearAtlasDirty() const { m_AtlasDirty = false; }

	// Font metrics for the given size.
	FontMetrics GetMetrics(FontHandle font, float pixelSize);
	float GetKerning(FontHandle font, uint32_t left, uint32_t right, float pixelSize);

private:
	struct FontFace
	{
		std::vector<uint8_t> TtfData;
		int                  StbFontOffset = 0; // stb_truetype font index
	};

	struct GlyphKey
	{
		FontHandle Font;
		uint32_t   Codepoint;
		float      PixelSize;
		bool operator==(const GlyphKey& o) const { return Font == o.Font && Codepoint == o.Codepoint && PixelSize == o.PixelSize; }
	};
	struct GlyphKeyHash { size_t operator()(const GlyphKey& k) const { return (size_t)k.Font ^ (k.Codepoint << 2) ^ (size_t)(k.PixelSize * 1000.0f); } };

	FontFace* GetFace(FontHandle handle);

	bool RasterizeGlyph(FontHandle font, uint32_t codepoint, float pixelSize, GlyphInfo& outInfo);
	bool PackRect(uint32_t w, uint32_t h, uint32_t& outX, uint32_t& outY);
	void GrowAtlas();

	// Internal stb_truetype state is stored per face via the TtfData bytes

	std::vector<FontFace> m_Faces;
	std::unordered_map<GlyphKey, GlyphInfo, GlyphKeyHash> m_GlyphCache;

	// Atlas
	std::vector<uint8_t> m_AtlasData;
	uint32_t m_AtlasWidth  = 512;
	uint32_t m_AtlasHeight = 512;
	mutable bool m_AtlasDirty  = false;

	// Shelf packing state
	uint32_t m_ShelfY = 0;
	uint32_t m_ShelfMaxHeight = 0;
	uint32_t m_ShelfX = 0;
};

} // namespace wisp
