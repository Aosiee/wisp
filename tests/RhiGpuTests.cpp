// wisp::rhi on a real device (Mesa's lavapipe in CI), offscreen: the
// renderer's pipelines from the built-in SPIR-V (the validation layers
// check them against their layout) and its textures.

#include "Wisp/Rhi/RhiRenderer.h"

#include "hearth/rhi/vulkan/VulkanDevice.h"
#include "hearth/rhi/vulkan/VulkanRhiDevice.h"
#include "hearth/rhi/vulkan/VulkanWindowContext.h"

#include "hamr/test.h"

#include <vector>

namespace wisp::rhi_gpu_tests {

	using namespace hearth::rhi;

	class TestWindow final : public VulkanWindowContext
	{
	public:
		uint32_t Slot = 0;
		bool IsMainWindow() const override { return true; }
		uint32_t GetCurrentFrameSlot() const override { return Slot; }
	};

	// An offscreen device with one window slot, torn down in order.
	class Gpu
	{
	public:
		Gpu()
		{
			Rhi.SetPipelineCacheFile("");
			Rhi.EnsureDevice(nullptr);
			VulkanDevice& vulkan = *Rhi.GetVulkan();
			SlotBase = vulkan.AllocateWindowSlot(&Window);
			Rhi.NoteSlotExpected(SlotBase);
			vulkan.SetMainContext(&Window);
			Window.Slot = SlotBase;
			Rhi.NoteSlotReset(Window.Slot);
			Rhi.ApplyDescriptorLog(Window.Slot);
		}
		~Gpu()
		{
			Rhi.WaitIdle();
			VulkanDevice& vulkan = *Rhi.GetVulkan();
			vulkan.ClearMainContext(&Window);
			vulkan.ReleaseWindowSlot(SlotBase);
			Rhi.ShutdownDevice();
		}

		VulkanRhiDevice Rhi;
		TestWindow Window;
		uint32_t SlotBase = 0;
	};

	TEST_CASE("wisp rhi gpu: the renderer builds its pipelines and keeps its textures")
	{
		Gpu gpu;
		RhiRenderer renderer;
		REQUIRE(renderer.Init(gpu.Rhi, nullptr));

		const std::vector<uint8_t> pixels(8 * 8 * 4, 0xff);
		const uint64_t texture = renderer.CreateTexture(8, 8, pixels.data());
		CHECK(texture >= 3u); // 0..2 are the batcher's pipeline ids
		renderer.UpdateTexture(texture, 0, 0, 8, 8, pixels.data());
		const uint64_t second = renderer.CreateTexture(8, 8, pixels.data());
		CHECK(second != texture);
		renderer.DestroyTexture(texture);
		renderer.DestroyTexture(second);
		CHECK(renderer.CreateTexture(0, 8, pixels.data()) == 0u);

		renderer.Shutdown();
	}

} // namespace wisp::rhi_gpu_tests
