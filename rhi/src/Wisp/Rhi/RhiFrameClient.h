#pragma once

// A window's UI as one of its presenter's frame clients: the update builds
// the window's batches on the loop thread (layout, paint, the renderer's
// preparation), and a "Wisp UI" pass over the swapchain records them. A
// program that draws the UI somewhere of its own (an offscreen target)
// calls Draw from its own pass.

#include "hearth/rhi/vulkan/WindowPresenter.h"

#include <cstdint>
#include <functional>

namespace hearth::platform { class Window; }
namespace hearth::rhi { class CommandList; }

namespace wisp {

	class FrameArena;
	class UIApplication;

	class RhiFrameClient : public hearth::rhi::FrameClient
	{
	public:
		// Runs before every window's update (the program's settings into
		// the application). Optional.
		void SetBeforeUpdate(std::function<void(UIApplication&)> beforeUpdate) { m_BeforeUpdate = std::move(beforeUpdate); }
		// The arena the batches build into (optional: without one they
		// allocate on the heap). It must outlive every frame's batches.
		void SetFrameArena(FrameArena* arena) { m_Arena = arena; }

		void Update(const hearth::rhi::FrameUpdate& update) override;
		void RecordSwapchain(hearth::rhi::SwapchainGraph& swapchain, const hearth::rhi::FrameRecord& record) override;

		// Draws the window's batches from its update into the pass's open
		// rendering scope; the list's FrameSlot() picks the frame. `window`
		// is null for the offscreen window. The logical layout space shrinks
		// by the UI scale and the projection maps it back onto the whole
		// target, so the surface scales uniformly here. Record threads.
		void Draw(hearth::rhi::CommandList& list, hearth::platform::Window* window, uint32_t width, uint32_t height);

	private:
		std::function<void(UIApplication&)> m_BeforeUpdate;
		FrameArena* m_Arena = nullptr;
	};

} // namespace wisp
