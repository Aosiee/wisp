#pragma once

#include "Wisp/Renderer.h"
#include <memory>

namespace wisp {

class FontCache;

struct WispVulkanImpl;       // PIMPL — hides Vulkan headers from Wisp headers
struct WispWindowRenderState; // PIMPL — per-window staging + command buffer + viewport

class RendererVulkan : public IUIRenderer
{
public:
	RendererVulkan();
	~RendererVulkan() override;

	// Must be called after Vulkan device is available. Creates pipeline,
	// shaders, descriptor pool, atlas texture (shared by every window).
	bool Init(const FontCache* fontCache);

	// Must be called before the Vulkan device is destroyed.
	void Shutdown();

	// One per OS window: owns that window's staging buffers and holds
	// the command buffer/viewport it is currently being recorded into, so
	// two windows recording in the same frame never stomp each other's
	// transient state. Pipelines/atlas/descriptor sets stay shared.
	WispWindowRenderState* CreateWindowState();
	void DestroyWindowState(WispWindowRenderState* state);

	// Render into a specific window's state.
	void RenderBatches(const BatchedDrawData& data, const glm::mat4& orthoProjection, WispWindowRenderState& state);

	// Per-window state plumbing: set which command buffer/viewport a
	// window's state is being recorded into.
	void SetWindowCommandBuffer(WispWindowRenderState& state, void* vkCommandBuffer);
	void SetWindowViewport(WispWindowRenderState& state, uint32_t width, uint32_t height);

	// IUIRenderer — legacy single-state path (renders into the internal
	// default state; SetCommandBuffer/SetViewportSize set it).
	void RenderBatches(const BatchedDrawData& data, const glm::mat4& orthoProjection) override;

	// Call once per frame. Resets internal state for new draw data.
	void BeginFrame();

	// Must be called before each RenderBatches() to provide the active command buffer.
	void SetCommandBuffer(void* vkCommandBuffer);

	// Set viewport dimensions (should match swapchain extent).
	void SetViewportSize(uint32_t width, uint32_t height);

	// IUIRenderer
	uint64_t CreateTexture(uint32_t width, uint32_t height, const uint8_t* rgbaData) override;
	uint64_t WrapUIImageHandle(const TinyEngine::UIImageHandle& handle) override;
	void UpdateTexture(uint64_t handle, uint32_t x, uint32_t y, uint32_t width, uint32_t height, const uint8_t* rgbaData) override;
	void DestroyTexture(uint64_t handle) override;

private:
	std::unique_ptr<WispVulkanImpl> m_Impl;
	std::unique_ptr<WispWindowRenderState> m_DefaultState;
};

} // namespace wisp
