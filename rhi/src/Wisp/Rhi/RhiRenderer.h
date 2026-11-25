#pragma once

// Wisp's renderer on Hearth's GPU layer: the four UI pipelines (rects,
// rounded boxes, text from the font atlas, sampled images), the atlas, the
// textures, and each window's staging. Wisp builds a window's batches
// (UIApplication::UpdateWindow, which calls PrepareFrame on the loop
// thread); RenderBatches records them into a pass's command list, on any
// thread, without creating device objects.

#include "Wisp/Renderer.h"

#include "hearth/rhi/Device.h"

#include <glm/glm.hpp>

#include <cstdint>
#include <memory>

namespace wisp {

	class FontCache;

	struct RhiRendererImpl;

	class RhiRenderer : public IUIRenderer
	{
	public:
		RhiRenderer();
		~RhiRenderer() override;

		// After the device exists: the pipelines (their shaders are the
		// SPIR-V built into Wisp, named by their source files - "ui_rect.vert"
		// - so a device's reload by name reaches them) and the atlas, shared
		// by every window. The UI draws into B8G8R8A8 colour with a D32
		// depth attachment it neither tests nor writes.
		bool Init(hearth::rhi::Device& device, const FontCache* fontCache);
		// Before the device is destroyed.
		void Shutdown();

		// IUIRenderer
		uint64_t CreateTexture(uint32_t width, uint32_t height, const uint8_t* rgbaData) override;
		void UpdateTexture(uint64_t handle, uint32_t x, uint32_t y, uint32_t width, uint32_t height, const uint8_t* rgbaData) override;
		void DestroyTexture(uint64_t handle) override;
		// One per window: its own staging buffers and the viewport it is
		// recorded into, so two windows recording in the same frame never
		// share transient state.
		std::unique_ptr<WindowRenderState> CreateWindowState() override;
		// The loop thread's preparation: rebuilds the font atlas when dirty
		// and grows the window's staging to cover the batches, so
		// RenderBatches never creates device objects.
		void PrepareFrame(const BatchedDrawData& data, WindowRenderState& state, uint64_t frameIndex) override;

		// A program's image (a render target's or a texture's, resting in
		// the shader-read layout whenever the UI samples it) as a texture
		// Wisp can draw. The caller keeps the image: DestroyTexture frees
		// only the wrapper. `nativeImage` is the backend's image handle.
		uint64_t WrapImage(void* nativeImage, uint32_t width, uint32_t height, hearth::rhi::Format format);

		// The window's viewport for its next record.
		void SetWindowViewport(WindowRenderState& state, uint32_t width, uint32_t height);
		// Records a window's batches into the pass's command list. The
		// list's FrameSlot() picks the staging the update prepared (the
		// update and the record agree on it through the list, never through
		// shared state).
		void RenderBatches(const BatchedDrawData& data, const glm::mat4& orthoProjection, WindowRenderState& state, hearth::rhi::CommandList& commandList);

	private:
		std::unique_ptr<RhiRendererImpl> m_Impl;
	};

} // namespace wisp
