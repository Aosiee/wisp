#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <type_traits>
#include <vector>
#include <glm/glm.hpp>

namespace wisp {

// ---- Frame memory ------------------------------------------------------------

// Where a frame's batch output is built: memory handed out per frame index
// and taken back all at once when that frame's slot is reused. The host
// owns it; with no arena, the batches use the global allocator.
class FrameArena
{
public:
	virtual ~FrameArena() = default;
	// Takes back everything the frame's slot holds, before Wisp builds a
	// new frame into it (only when it does).
	virtual void ResetFrame(uint32_t frameIndex) = 0;
	virtual void* Allocate(uint32_t frameIndex, size_t size, size_t alignment) = 0;
};

// A std allocator over a FrameArena's frame (nothing is freed one by one:
// the memory returns with the slot's reset).
template <typename T>
class FrameArenaAllocator
{
public:
	using value_type = T;
	using propagate_on_container_move_assignment = std::true_type;
	using is_always_equal = std::false_type;

	FrameArenaAllocator() = default;
	FrameArenaAllocator(FrameArena* arena, uint32_t frameIndex) : m_Arena(arena), m_FrameIndex(frameIndex) {}

	template <typename U>
	FrameArenaAllocator(const FrameArenaAllocator<U>& other) noexcept : m_Arena(other.m_Arena), m_FrameIndex(other.m_FrameIndex) {}

	T* allocate(std::size_t n)
	{
		if (m_Arena)
			return static_cast<T*>(m_Arena->Allocate(m_FrameIndex, n * sizeof(T), alignof(T)));
		return std::allocator<T>().allocate(n);
	}

	void deallocate(T* p, std::size_t n) noexcept
	{
		if (!m_Arena)
			std::allocator<T>().deallocate(p, n);
	}

	template <typename U>
	bool operator==(const FrameArenaAllocator<U>& other) const noexcept { return m_Arena == other.m_Arena && m_FrameIndex == other.m_FrameIndex; }
	template <typename U>
	bool operator!=(const FrameArenaAllocator<U>& other) const noexcept { return !(*this == other); }

private:
	template <typename U>
	friend class FrameArenaAllocator;
	FrameArena* m_Arena = nullptr;
	uint32_t m_FrameIndex = 0;
};

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
	// The batch output builds into the host's frame arena (per frame
	// slot) - the arena resets when the slot is reused.
	std::vector<BatchedVertex, FrameArenaAllocator<BatchedVertex>> Vertices;
	std::vector<uint32_t,      FrameArenaAllocator<uint32_t>>      Indices;
	std::vector<BatchedBatch,  FrameArenaAllocator<BatchedBatch>>  Batches;
	std::vector<glm::vec4,     FrameArenaAllocator<glm::vec4>>     ClipRects; // x, y, w, h (logical, scaled by DpiScale for scissor)
	float                      DpiScale = 1.0f;
	// The frame's UI settings (UIApplication's at the batch build), carried
	// with the batches so the renderer reads the frame's own values.
	float                      UiScale = 1.0f;
	bool                       GpuTimeBatches = false;
};

// ---- The renderer ------------------------------------------------------------

// A renderer's own state for one window (its staging buffers, its
// viewport): Wisp holds it in the window's context and hands it back.
class WindowRenderState
{
public:
	virtual ~WindowRenderState() = default;
};

// What Wisp needs from the program's renderer. The renderer also records
// each window's batches into its frame (UIApplication::GetFrameBatches);
// how is the renderer's own business, so that part is not here.
class IUIRenderer
{
public:
	virtual ~IUIRenderer() = default;

	// An immutable 2D texture from RGBA pixel data; 0 on failure.
	virtual uint64_t CreateTexture(uint32_t width, uint32_t height, const uint8_t* rgbaData) = 0;
	// Update a sub-region of an existing texture.
	virtual void UpdateTexture(uint64_t handle, uint32_t x, uint32_t y,
		uint32_t width, uint32_t height, const uint8_t* rgbaData) = 0;
	// Destroy a texture (a no-op for 0).
	virtual void DestroyTexture(uint64_t handle) = 0;

	// One per window context.
	virtual std::unique_ptr<WindowRenderState> CreateWindowState() = 0;

	// After a frame's batches are built (the thread that called
	// UIApplication::UpdateWindow): whatever the recording of that frame
	// will need (the font atlas rebuilt, staging grown), so the record
	// creates no device objects.
	virtual void PrepareFrame(const BatchedDrawData& data, WindowRenderState& state, uint64_t frameIndex) = 0;
};

} // namespace wisp
