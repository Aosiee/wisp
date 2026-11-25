#include "Wisp/Rhi/RhiFrameClient.h"

#include "Wisp/Application.h"
#include "Wisp/Rhi/RhiRenderer.h"

#include "hearth/rhi/vulkan/VulkanRhiDevice.h"

#include "tmath/Projection.h"

#include <algorithm>

namespace wisp {

	namespace rhi = hearth::rhi;
	namespace rg = hearth::rg;

	void RhiFrameClient::Update(const rhi::FrameUpdate& update)
	{
		UIApplication& app = UIApplication::Get();
		if (m_BeforeUpdate)
			m_BeforeUpdate(app);
		app.UpdateWindow(update.Window, update.Width, update.Height, update.FrameSlot, m_Arena);
	}

	void RhiFrameClient::RecordSwapchain(rhi::SwapchainGraph& swapchain, const rhi::FrameRecord&)
	{
		rhi::SwapchainGraph* graph = &swapchain;
		auto& pass = swapchain.Graph().AddPass("Wisp UI");
		pass.Write(swapchain.Image(), rhi::TextureState::RenderTarget);
		pass.Write(swapchain.Depth(), rhi::TextureState::DepthWrite);
		pass.SetExecute([this, graph](rhi::CommandList& list, const rg::PassResources&) {
			rhi::VulkanCommandList& vkList = static_cast<rhi::VulkanCommandList&>(list);
			graph->BeginRendering(vkList, false);
			const VkExtent2D extent = graph->Extent();
			Draw(list, graph->Window(), extent.width, extent.height);
			vkCmdEndRendering(vkList.GetCommandBuffer());
		});
	}

	void RhiFrameClient::Draw(rhi::CommandList& list, hearth::platform::Window* window, uint32_t width, uint32_t height)
	{
		WindowRenderState* state = nullptr;
		const BatchedDrawData* batched = UIApplication::Get().GetFrameBatches(window, list.FrameSlot(), &state);
		if (!batched || !state)
			return;
		const float uiScale = std::max(0.25f, batched->UiScale);
		const float logicalW = (float)width / uiScale;
		const float logicalH = (float)height / uiScale;
		const glm::mat4 ortho = tmath::Orthographic(0.0f, logicalW, 0.0f, logicalH, -1.0f, 1.0f);
		RhiRenderer& renderer = static_cast<RhiRenderer&>(UIApplication::Get().GetRenderer());
		renderer.SetWindowViewport(*state, width, height);
		renderer.RenderBatches(*batched, ortho, *state, list);
	}

} // namespace wisp
