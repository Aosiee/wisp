#pragma once

#include <cstdint>
#include <glm/glm.hpp>

#include "TinyEngine/Memory/FrameArenaAllocator.h"

#include "TinyEngine/Renderer/UIImageHandle.h"

namespace TinyEngine::rhi { class CommandList; }

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
	// The batch output builds into the frame arena (per frame
	// slot) - the arena resets when the slot is reused.
	std::vector<BatchedVertex, TinyEngine::FrameArenaAllocator<BatchedVertex>> Vertices;
	std::vector<uint32_t,      TinyEngine::FrameArenaAllocator<uint32_t>>      Indices;
	std::vector<BatchedBatch,  TinyEngine::FrameArenaAllocator<BatchedBatch>>  Batches;
	std::vector<glm::vec4,     TinyEngine::FrameArenaAllocator<glm::vec4>>     ClipRects; // x, y, w, h (logical, scaled by DpiScale for scissor)
	float                      DpiScale = 1.0f;
};

// ---- IUIRenderer -----------------------------------------------------------

class IUIRenderer
{
public:
	virtual ~IUIRenderer() = default;

	// Create an immutable 2D texture from RGBA pixel data. Returns an opaque handle.
	virtual uint64_t CreateTexture(uint32_t width, uint32_t height, const uint8_t* rgbaData) = 0;
	// Wrap an engine-acquired
	// UIImageHandle (a Framebuffer's colour attachment or a Texture2D's
	// image) as a samplable texture. The source image stays owned by the
	// caller; DestroyTexture releases only this wrapper's view/descriptor
	// set. The format comes from the handle, so both BGRA8 render targets
	// and RGBA8 textures display correctly.
	virtual uint64_t WrapUIImageHandle(const TinyEngine::UIImageHandle& handle) = 0;

	// Update a sub-region of an existing texture.
	virtual void UpdateTexture(uint64_t handle, uint32_t x, uint32_t y,
		uint32_t width, uint32_t height, const uint8_t* rgbaData) = 0;

	// Destroy a texture.
	virtual void DestroyTexture(uint64_t handle) = 0;

	// Record batched draw data into the pass's command list (its
	// FrameSlot() is the record slot). Called once per frame.
	virtual void RenderBatches(const BatchedDrawData& data, const glm::mat4& orthoProjection, TinyEngine::rhi::CommandList& commandList) = 0;
};

} // namespace wisp
