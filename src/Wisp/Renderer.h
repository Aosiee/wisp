#pragma once

#include <cstdint>
#include <glm/glm.hpp>

namespace wisp {

// ---- Batched Draw Data -----------------------------------------------------

struct BatchedVertex
{
	float pos[3];
	float uv[2];
	uint8_t col[4]; // RGBA, UNORM
};

struct SdfParams
{
	float CornerRadii[4] = {}; // TL, TR, BR, BL
	float OutlineWidth = 0.0f;
	float OutlineColour[4] = {}; // RGBA
	float Size[2] = {};        // box width, height
};

struct BatchedBatch
{
	uint64_t TextureId = 0;
	int      ClipIndex = -1;
	uint32_t IndexOffset = 0;
	uint32_t IndexCount = 0;
	SdfParams Sdf = {};
};

struct BatchedDrawData
{
	std::vector<BatchedVertex> Vertices;
	std::vector<uint32_t>      Indices;
	std::vector<BatchedBatch>  Batches;
	std::vector<glm::vec4>     ClipRects; // x, y, w, h (logical, scaled by DpiScale for scissor)
	float                      DpiScale = 1.0f;
};

// ---- IUIRenderer -----------------------------------------------------------

class IUIRenderer
{
public:
	virtual ~IUIRenderer() = default;

	// Create an immutable 2D texture from RGBA pixel data. Returns an opaque handle.
	virtual uint64_t CreateTexture(uint32_t width, uint32_t height, const uint8_t* rgbaData) = 0;
	// Wrap an existing native
	// GPU image (opaque backend handle) as a samplable texture. The image
	// stays owned by the caller; DestroyTexture releases only the wrapper.
	virtual uint64_t WrapNativeImage(void* nativeImage, uint32_t width, uint32_t height) = 0;

	// Update a sub-region of an existing texture.
	virtual void UpdateTexture(uint64_t handle, uint32_t x, uint32_t y,
		uint32_t width, uint32_t height, const uint8_t* rgbaData) = 0;

	// Destroy a texture.
	virtual void DestroyTexture(uint64_t handle) = 0;

	// Submit batched draw data for rendering. Called once per frame.
	virtual void RenderBatches(const BatchedDrawData& data, const glm::mat4& orthoProjection) = 0;
};

} // namespace wisp
