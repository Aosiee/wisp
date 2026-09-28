#include "Wisp/RendererVulkan.h"
#include "Wisp/FontCache.h"

#include "Platform/Vulkan/VulkanCommon.h"
#include "Platform/Vulkan/VulkanDevice.h"
#include "Platform/Vulkan/VulkanDeviceRegistry.h"
#include "Platform/Vulkan/VulkanPipeline.h"
#include "Platform/Vulkan/VulkanShaderModule.h"
#include "assetio/VulkanShaderCompiler.h"
#include "Platform/Vulkan/VulkanShaderReflection.h"
#include "Platform/Vulkan/VulkanDescriptor.h"
#include "Platform/Vulkan/VulkanMainSwapchainInfo.h"
#include "Platform/Vulkan/VulkanFramebuffer.h"
#include "Platform/Vulkan/VulkanFrameContext.h"

#include <cstring>
#include <glm/gtc/matrix_transform.hpp>

using namespace TinyEngine;

namespace wisp {

// ---- Embedded GLSL (compiled at runtime via VulkanShaderCompiler) -----------

static const char* kRectVertexGLSL = R"(
#version 450
layout(push_constant) uniform PC { mat4 projection; } pc;
layout(location = 0) in vec3 aPos;
layout(location = 1) in vec2 aUV;
layout(location = 2) in vec4 aCol;
layout(location = 0) out vec4 vCol;
layout(location = 1) out vec2 vUV;
void main() { gl_Position = pc.projection * vec4(aPos, 1.0); vCol = aCol; vUV = aUV; }
)";

static const char* kRectFragmentGLSL = R"(
#version 450
layout(location = 0) in vec4 vCol;
layout(location = 1) in vec2 vUV;
layout(location = 0) out vec4 outCol;
void main() { outCol = vCol; }
)";

static const char* kTextVertexGLSL = R"(
#version 450
layout(push_constant) uniform PC { mat4 projection; } pc;
layout(location = 0) in vec3 aPos;
layout(location = 1) in vec2 aUV;
layout(location = 2) in vec4 aCol;
layout(location = 0) out vec4 vCol;
layout(location = 1) out vec2 vUV;
void main() { gl_Position = pc.projection * vec4(aPos, 1.0); vCol = aCol; vUV = aUV; }
)";

static const char* kTextFragmentGLSL = R"(
#version 450
layout(binding = 0) uniform sampler2D uAtlas;
layout(location = 0) in vec4 vCol;
layout(location = 1) in vec2 vUV;
layout(location = 0) out vec4 outCol;
void main() { float a = texture(uAtlas, vUV).a; outCol = vCol * vec4(1.0, 1.0, 1.0, a); }
)";

static const char* kImageVertexGLSL = R"(
#version 450
layout(push_constant) uniform PC { mat4 projection; } pc;
layout(location = 0) in vec3 aPos;
layout(location = 1) in vec2 aUV;
layout(location = 2) in vec4 aCol;
layout(location = 0) out vec4 vCol;
layout(location = 1) out vec2 vUV;
void main() { gl_Position = pc.projection * vec4(aPos, 1.0); vCol = aCol; vUV = aUV; }
)";

static const char* kImageFragmentGLSL = R"(
#version 450
layout(binding = 0) uniform sampler2D uTex;
layout(location = 0) in vec4 vCol;
layout(location = 1) in vec2 vUV;
layout(location = 0) out vec4 outCol;
void main() { outCol = texture(uTex, vUV) * vCol; }
)";

static const char* kRoundedBoxVertexGLSL = R"(
#version 450
layout(push_constant) uniform PC {
    mat4 projection;
    layout(offset=64) vec4 radii;
    layout(offset=80) float outlineWidth;
    layout(offset=96) vec4 outlineColour;
    layout(offset=112) vec2 halfSize;
} pc;
layout(location = 0) in vec3 aPos;
layout(location = 1) in vec2 aUV;
layout(location = 2) in vec4 aCol;
layout(location = 0) out vec4 vCol;
layout(location = 1) out vec2 vUV;
layout(location = 2) flat out vec4 vRadii;
layout(location = 3) flat out float vOutlineWidth;
layout(location = 4) flat out vec4 vOutlineColour;
layout(location = 5) flat out vec2 vHalfSize;
void main() {
    gl_Position = pc.projection * vec4(aPos, 1.0);
    vCol = aCol; vUV = aUV;
    vRadii = pc.radii; vOutlineWidth = pc.outlineWidth; vOutlineColour = pc.outlineColour;
    vHalfSize = pc.halfSize;
}
)";

static const char* kRoundedBoxFragmentGLSL = R"(
#version 450
layout(location = 0) in vec4 vCol;
layout(location = 1) in vec2 vUV;
layout(location = 2) flat in vec4 vRadii;
layout(location = 3) flat in float vOutlineWidth;
layout(location = 4) flat in vec4 vOutlineColour;
layout(location = 5) flat in vec2 vHalfSize;
layout(location = 0) out vec4 outCol;

float sdRoundedBox(vec2 p, vec2 b, vec4 r) {
    r.xy = (p.x > 0.0) ? r.yw : r.xz;
    r.x  = (p.y > 0.0) ? r.x  : r.y;
    vec2 q = abs(p) - b + r.x;
    return min(max(q.x, q.y), 0.0) + length(max(q, 0.0)) - r.x;
}

void main() {
    vec2 p = (vUV - 0.5) * 2.0 * vHalfSize;
    vec4 r = vRadii;
    float m = min(vHalfSize.x, vHalfSize.y);
    r = min(r, vec4(m));
    float d = sdRoundedBox(p, vHalfSize, r);

    // Fill first (AA coverage), then paint the outline ring over it.
    // The previous outline branch inverted the fill/outline mix and zeroed
    // the interior alpha, so any rounded box drawn with an outline rendered
    // ONLY its outline ring and never its fill (the "zero alpha" swatches).
    float aa = fwidth(d);
    float fillAlpha = 1.0 - smoothstep(-aa, aa, d);
    outCol = vCol;
    outCol.a *= fillAlpha;

    if (vOutlineWidth > 0.0) {
        float outer = abs(d) - vOutlineWidth;
        float outerAA = fwidth(outer);
        float ring = 1.0 - smoothstep(-outerAA, outerAA, outer);
        outCol = mix(outCol, vOutlineColour, ring);
    }
}
)";

// ---- PIMPL -----------------------------------------------------------------

struct RoundedBoxPushConstants {
	glm::mat4 projection;    // offset 0
	float radii[4];          // offset 64
	float outlineWidth;      // offset 80
	float _pad[3];           // offset 84, pad to 96
	float outlineColour[4];   // offset 96
	float halfSize[2];       // offset 112
	float _pad2[2];          // pad to 128
};

struct StagingBuffers
{
	VkBuffer      VB = VK_NULL_HANDLE;
	VmaAllocation VBAlloc = VK_NULL_HANDLE;
	VkBuffer      IB = VK_NULL_HANDLE;
	VmaAllocation IBAlloc = VK_NULL_HANDLE;
	uint32_t      VBCapacity = 0;
	uint32_t      IBCapacity = 0;
};

struct WispVulkanImpl
{
	VulkanDevice*           Device = nullptr;

	// Rect pipeline (solid colour quads, no texture)
	VulkanShaderModule      RectVertexShader;
	VulkanShaderModule      RectFragmentShader;
	VulkanPipeline          RectPipeline;
	bool                    RectPipelineInit = false;

	// Text pipeline (atlas-textured quads)
	VulkanShaderModule      TextVertexShader;
	VulkanShaderModule      TextFragmentShader;
	VulkanPipeline          TextPipeline;
	VulkanDescriptorSetLayout TextDescLayout;
	VulkanDescriptorAllocator TextDescAllocator;
	VkDescriptorSet         TextDescSets[VulkanDevice::TotalFrameSlots] = {};
	bool                    TextPipelineInit = false;

	// RoundedBox pipeline (SDF rounded corners, no texture)
	VulkanShaderModule      RoundedBoxVertexShader;
	VulkanShaderModule      RoundedBoxFragmentShader;
	VulkanPipeline          RoundedBoxPipeline;
	bool                    RoundedBoxPipelineInit = false;

// Atlas texture
	VkImage                 AtlasImage = VK_NULL_HANDLE;
	VmaAllocation           AtlasAlloc = VK_NULL_HANDLE;
	VkImageView             AtlasView = VK_NULL_HANDLE;
	VkSampler               AtlasSampler = VK_NULL_HANDLE;
	VkImageLayout           AtlasLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
	bool                    AtlasCreated = false;

	// Image pipeline (per-texture sampled quads - IUIRenderer::CreateTexture)
	VulkanShaderModule      ImageVertexShader;
	VulkanShaderModule      ImageFragmentShader;
	VulkanPipeline          ImagePipeline;
	VulkanDescriptorSetLayout ImageDescLayout;
	VulkanDescriptorAllocator ImageDescAllocator;
	VkSampler               ImageSampler = VK_NULL_HANDLE;
	bool                    ImagePipelineInit = false;

	struct WispTexture
	{
		VkImage Image = VK_NULL_HANDLE;
		VmaAllocation Alloc = VK_NULL_HANDLE;
		VkImageView View = VK_NULL_HANDLE;
		VkDescriptorSet DescSet = VK_NULL_HANDLE;
		uint32_t Width = 0, Height = 0;
		bool OwnsImage = true; // false = wrapped native image, never freed
	};
	std::unordered_map<uint64_t, WispTexture> Textures;
	// Texture ids start above the batcher's reserved pipeline ids
	// (1 = Text, 2 = RoundedBox) - id 1 or 2 would route the draw into
	// the wrong pipeline.
	uint64_t NextTextureId = 3;

	const FontCache*        FontCachePtr = nullptr;
};

// Per-window transient state: staging buffers, the command buffer the
// window is being recorded into, and its viewport. One per WindowContext;
// the shared WispVulkanImpl holds everything windows reuse. The staging
// buffers are per-frame-slot: with only two frames in flight, frame N+1's
// fence wait only covers
// frame N-1, so a single shared pair was being overwritten while frame N's
// GPU read of it could still be in flight (a HOST-write → GPU-read race
// that tears the UI geometry during interaction).
struct WispWindowRenderState
{
	StagingBuffers          Staging[VulkanDevice::FramesInFlight];
	VkCommandBuffer         CommandBuffer = VK_NULL_HANDLE;
	uint32_t                ViewportW = 1920;
	uint32_t                ViewportH = 1080;
};

// ---- RendererVulkan --------------------------------------------------------

RendererVulkan::RendererVulkan()
	: m_Impl(std::make_unique<WispVulkanImpl>()), m_DefaultState(std::make_unique<WispWindowRenderState>()) {}
RendererVulkan::~RendererVulkan() { Shutdown(); }

void RendererVulkan::SetCommandBuffer(void* vkCB)
{
	m_DefaultState->CommandBuffer = (VkCommandBuffer)vkCB;
}

void RendererVulkan::SetViewportSize(uint32_t width, uint32_t height)
{
	m_DefaultState->ViewportW = width;
	m_DefaultState->ViewportH = height;
}

WispWindowRenderState* RendererVulkan::CreateWindowState()
{
	return new WispWindowRenderState();
}

void RendererVulkan::SetWindowCommandBuffer(WispWindowRenderState& state, void* vkCB)
{
	state.CommandBuffer = (VkCommandBuffer)vkCB;
}

void RendererVulkan::SetWindowViewport(WispWindowRenderState& state, uint32_t width, uint32_t height)
{
	state.ViewportW = width;
	state.ViewportH = height;
}

void RendererVulkan::DestroyWindowState(WispWindowRenderState* state)
{
	if (!state) return;
	WispVulkanImpl& impl = *m_Impl;
	if (impl.Device)
	{
		VmaAllocator allocator = impl.Device->GetAllocator();
		// A window close is rare enough to stall for: wait until the queue
		// has finished with any in-flight frame that references these
		// buffers before freeing them (VUID-vkDestroyBuffer-00922).
		bool hasStaging = false;
		for (const StagingBuffers& s : state->Staging)
			if (s.VB != VK_NULL_HANDLE || s.IB != VK_NULL_HANDLE) { hasStaging = true; break; }
		if (hasStaging)
			vkDeviceWaitIdle(impl.Device->GetDevice());
		for (StagingBuffers& s : state->Staging)
		{
			if (s.VB) vmaDestroyBuffer(allocator, s.VB, s.VBAlloc);
			if (s.IB) vmaDestroyBuffer(allocator, s.IB, s.IBAlloc);
			s = {};
		}
	}
	delete state;
}

static void EnsureStagingBuffers(WispVulkanImpl& impl, StagingBuffers& s, uint32_t vbBytes, uint32_t ibBytes)
{
	VulkanDevice& device = *impl.Device;
	VmaAllocator allocator = device.GetAllocator();

	// Vertex buffer
	if (vbBytes > 0 && (!s.VB || s.VBCapacity < vbBytes))
	{
		if (s.VB) { vmaDestroyBuffer(allocator, s.VB, s.VBAlloc); }
		VkBufferCreateInfo ci{};
		ci.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
		ci.size = vbBytes;
		ci.usage = VK_BUFFER_USAGE_VERTEX_BUFFER_BIT;
		VmaAllocationCreateInfo aci{};
		aci.usage = VMA_MEMORY_USAGE_CPU_TO_GPU;
		aci.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT;
		vmaCreateBuffer(allocator, &ci, &aci, &s.VB, &s.VBAlloc, nullptr);
		s.VBCapacity = vbBytes;
	}

	// Index buffer
	if (ibBytes > 0 && (!s.IB || s.IBCapacity < ibBytes))
	{
		if (s.IB) { vmaDestroyBuffer(allocator, s.IB, s.IBAlloc); }
		VkBufferCreateInfo ci{};
		ci.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
		ci.size = ibBytes;
		ci.usage = VK_BUFFER_USAGE_INDEX_BUFFER_BIT;
		VmaAllocationCreateInfo aci{};
		aci.usage = VMA_MEMORY_USAGE_CPU_TO_GPU;
		aci.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT;
		vmaCreateBuffer(allocator, &ci, &aci, &s.IB, &s.IBAlloc, nullptr);
		s.IBCapacity = ibBytes;
	}
}

static void UploadToStaging(VmaAllocator allocator, VmaAllocation alloc, const void* data, uint32_t size)
{
	void* mapped;
	vmaMapMemory(allocator, alloc, &mapped);
	std::memcpy(mapped, data, size);
	vmaUnmapMemory(allocator, alloc);
}

static void CreateAtlasTexture(WispVulkanImpl& impl)
{
	const FontCache* fc = impl.FontCachePtr;
	if (!fc || !fc->GetAtlasData()) return;

	VulkanDevice& device = *impl.Device;
	VmaAllocator allocator = device.GetAllocator();
	uint32_t w = fc->GetAtlasWidth();
	uint32_t h = fc->GetAtlasHeight();

	// Destroy old atlas — must wait for in-flight frames first
	if (impl.AtlasView || impl.AtlasSampler || impl.AtlasAlloc)
		vkDeviceWaitIdle(device.GetDevice());

	if (impl.AtlasView)    { vkDestroyImageView(device.GetDevice(), impl.AtlasView, nullptr); impl.AtlasView = VK_NULL_HANDLE; }
	if (impl.AtlasSampler) { vkDestroySampler(device.GetDevice(), impl.AtlasSampler, nullptr); impl.AtlasSampler = VK_NULL_HANDLE; }
	if (impl.AtlasAlloc)   { vmaDestroyImage(allocator, impl.AtlasImage, impl.AtlasAlloc); impl.AtlasImage = VK_NULL_HANDLE; impl.AtlasAlloc = VK_NULL_HANDLE; }

	VkImageCreateInfo imgCI{};
	imgCI.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
	imgCI.imageType = VK_IMAGE_TYPE_2D;
	imgCI.format = VK_FORMAT_R8G8B8A8_UNORM;
	imgCI.extent = { w, h, 1 };
	imgCI.mipLevels = 1;
	imgCI.arrayLayers = 1;
	imgCI.samples = VK_SAMPLE_COUNT_1_BIT;
	imgCI.tiling = VK_IMAGE_TILING_OPTIMAL;
	imgCI.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
	imgCI.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;

	VmaAllocationCreateInfo allocCI{};
	allocCI.usage = VMA_MEMORY_USAGE_GPU_ONLY;
	vmaCreateImage(allocator, &imgCI, &allocCI, &impl.AtlasImage, &impl.AtlasAlloc, nullptr);

	// Image view
	VkImageViewCreateInfo viewCI{};
	viewCI.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
	viewCI.image = impl.AtlasImage;
	viewCI.viewType = VK_IMAGE_VIEW_TYPE_2D;
	viewCI.format = VK_FORMAT_R8G8B8A8_UNORM;
	viewCI.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
	vkCreateImageView(device.GetDevice(), &viewCI, nullptr, &impl.AtlasView);

	// Sampler
	VkSamplerCreateInfo samplerCI{};
	samplerCI.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
	samplerCI.magFilter = VK_FILTER_NEAREST;
	samplerCI.minFilter = VK_FILTER_NEAREST;
	samplerCI.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
	samplerCI.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
	samplerCI.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
	samplerCI.maxLod = 1.0f;
	vkCreateSampler(device.GetDevice(), &samplerCI, nullptr, &impl.AtlasSampler);

	// Staging buffer for upload
	VkBuffer stagingBuf;
	VmaAllocation stagingAlloc;
	VkBufferCreateInfo sCI{};
	sCI.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
	sCI.size = w * h * 4;
	sCI.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
	VmaAllocationCreateInfo sACI{};
	sACI.usage = VMA_MEMORY_USAGE_CPU_ONLY;
	sACI.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT;
	vmaCreateBuffer(allocator, &sCI, &sACI, &stagingBuf, &stagingAlloc, nullptr);

	void* mapped;
	vmaMapMemory(allocator, stagingAlloc, &mapped);
	std::memcpy(mapped, fc->GetAtlasData(), w * h * 4);
	vmaUnmapMemory(allocator, stagingAlloc);

	// One-time submit via the device's transient command pool - never the
	// shared per-frame-in-flight pools, which VulkanContext resets wholesale
	// each frame and (with more than one window) may reset while another
	// window's submission using that pool is still in flight.
	VkCommandBuffer cb = device.BeginSingleTimeCommands();

	// UNDEFINED → TRANSFER_DST
	VkImageMemoryBarrier2 toDst{};
	toDst.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2;
	toDst.srcStageMask = VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT;
	toDst.srcAccessMask = 0;
	toDst.dstStageMask = VK_PIPELINE_STAGE_2_COPY_BIT;
	toDst.dstAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
	toDst.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
	toDst.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
	toDst.image = impl.AtlasImage;
	toDst.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
	VkDependencyInfo dep1{};
	dep1.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
	dep1.imageMemoryBarrierCount = 1;
	dep1.pImageMemoryBarriers = &toDst;
	vkCmdPipelineBarrier2(cb, &dep1);

	VkBufferImageCopy region{};
	region.imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
	region.imageExtent = { w, h, 1 };
	vkCmdCopyBufferToImage(cb, stagingBuf, impl.AtlasImage, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);

	// TRANSFER_DST → SHADER_READ_ONLY
	VkImageMemoryBarrier2 toShader{};
	toShader.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2;
	toShader.srcStageMask = VK_PIPELINE_STAGE_2_COPY_BIT;
	toShader.srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
	toShader.dstStageMask = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT;
	toShader.dstAccessMask = VK_ACCESS_2_SHADER_READ_BIT;
	toShader.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
	toShader.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
	toShader.image = impl.AtlasImage;
	toShader.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
	VkDependencyInfo dep2{};
	dep2.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
	dep2.imageMemoryBarrierCount = 1;
	dep2.pImageMemoryBarriers = &toShader;
	vkCmdPipelineBarrier2(cb, &dep2);

	device.EndSingleTimeCommands(cb);
	vmaDestroyBuffer(allocator, stagingBuf, stagingAlloc);

	impl.AtlasCreated = true;
	impl.AtlasLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
}

bool RendererVulkan::Init(const FontCache* fontCache)
{
	WispVulkanImpl& impl = *m_Impl;
	impl.Device = &GetVulkanDevice();
	impl.FontCachePtr = fontCache;

	VulkanDevice& device = *impl.Device;
	const VulkanMainSwapchainInfo& swapchain = GetMainVulkanSwapchainInfo();

	// --- Rect pipeline ---
	{
		std::vector<uint32_t> vertSpv = VulkanShaderCompiler::Compile(kRectVertexGLSL, ShaderStage::Vertex, "ui_rect.vert");
		std::vector<uint32_t> fragSpv = VulkanShaderCompiler::Compile(kRectFragmentGLSL, ShaderStage::Fragment, "ui_rect.frag");
		impl.RectVertexShader.Init(device, vertSpv, VK_SHADER_STAGE_VERTEX_BIT, "Wisp Rect VS");
		impl.RectFragmentShader.Init(device, fragSpv, VK_SHADER_STAGE_FRAGMENT_BIT, "Wisp Rect FS");

		VulkanPipelineSpec spec;
		spec.VertexShader = &impl.RectVertexShader;
		spec.FragmentShader = &impl.RectFragmentShader;
		spec.VertexStride = sizeof(BatchedVertex);
		spec.VertexAttributes = {
			{ 0, VK_FORMAT_R32G32B32_SFLOAT, offsetof(BatchedVertex, pos) },
			{ 1, VK_FORMAT_R32G32_SFLOAT,    offsetof(BatchedVertex, uv) },
			{ 2, VK_FORMAT_R8G8B8A8_UNORM,   offsetof(BatchedVertex, col) },
		};
		spec.DescriptorSetLayout = VK_NULL_HANDLE;
		spec.PushConstantSize = sizeof(glm::mat4);
		spec.ColorAttachmentFormat = swapchain.ColourFormat;
		spec.EnableBlend = true;
		spec.DepthTestEnable = false;
		spec.DepthWriteEnable = false;
		spec.DepthAttachmentFormat = kSharedDepthFormat;
		impl.RectPipeline.Init(device, spec, "Wisp Rect Pipeline");
		impl.RectPipelineInit = true;
	}

	// --- Text pipeline ---
	{
		std::vector<uint32_t> vertSpv = VulkanShaderCompiler::Compile(kTextVertexGLSL, ShaderStage::Vertex, "ui_text.vert");
		std::vector<uint32_t> fragSpv = VulkanShaderCompiler::Compile(kTextFragmentGLSL, ShaderStage::Fragment, "ui_text.frag");
		impl.TextVertexShader.Init(device, vertSpv, VK_SHADER_STAGE_VERTEX_BIT, "Wisp Text VS");
		impl.TextFragmentShader.Init(device, fragSpv, VK_SHADER_STAGE_FRAGMENT_BIT, "Wisp Text FS");

		impl.TextDescLayout.InitSingleCombinedImageSampler(device, VK_SHADER_STAGE_FRAGMENT_BIT, "Wisp Text Layout");
		impl.TextDescAllocator.Init(device, VulkanDevice::TotalFrameSlots, "Wisp Text Pool", 1);

		for (uint32_t i = 0; i < VulkanDevice::TotalFrameSlots; ++i)
			impl.TextDescSets[i] = impl.TextDescAllocator.Allocate(impl.TextDescLayout.GetHandle(), "Wisp Text DS " + std::to_string(i));

		VulkanPipelineSpec spec;
		spec.VertexShader = &impl.TextVertexShader;
		spec.FragmentShader = &impl.TextFragmentShader;
		spec.VertexStride = sizeof(BatchedVertex);
		spec.VertexAttributes = {
			{ 0, VK_FORMAT_R32G32B32_SFLOAT, offsetof(BatchedVertex, pos) },
			{ 1, VK_FORMAT_R32G32_SFLOAT,    offsetof(BatchedVertex, uv) },
			{ 2, VK_FORMAT_R8G8B8A8_UNORM,   offsetof(BatchedVertex, col) },
		};
		spec.DescriptorSetLayout = impl.TextDescLayout.GetHandle();
		spec.PushConstantSize = sizeof(glm::mat4);
		spec.ColorAttachmentFormat = swapchain.ColourFormat;
		spec.EnableBlend = true;
		spec.DepthTestEnable = false;
		spec.DepthWriteEnable = false;
		spec.DepthAttachmentFormat = kSharedDepthFormat;
		impl.TextPipeline.Init(device, spec, "Wisp Text Pipeline");
		impl.TextPipelineInit = true;
	}

	// --- RoundedBox pipeline ---
	{
		std::vector<uint32_t> vertSpv = VulkanShaderCompiler::Compile(kRoundedBoxVertexGLSL, ShaderStage::Vertex, "ui_roundedbox.vert");
		std::vector<uint32_t> fragSpv = VulkanShaderCompiler::Compile(kRoundedBoxFragmentGLSL, ShaderStage::Fragment, "ui_roundedbox.frag");
		impl.RoundedBoxVertexShader.Init(device, vertSpv, VK_SHADER_STAGE_VERTEX_BIT, "Wisp RoundedBox VS");
		impl.RoundedBoxFragmentShader.Init(device, fragSpv, VK_SHADER_STAGE_FRAGMENT_BIT, "Wisp RoundedBox FS");

		VulkanPipelineSpec spec;
		spec.VertexShader = &impl.RoundedBoxVertexShader;
		spec.FragmentShader = &impl.RoundedBoxFragmentShader;
		spec.VertexStride = sizeof(BatchedVertex);
		spec.VertexAttributes = {
			{ 0, VK_FORMAT_R32G32B32_SFLOAT, offsetof(BatchedVertex, pos) },
			{ 1, VK_FORMAT_R32G32_SFLOAT,    offsetof(BatchedVertex, uv) },
			{ 2, VK_FORMAT_R8G8B8A8_UNORM,   offsetof(BatchedVertex, col) },
		};
		spec.DescriptorSetLayout = VK_NULL_HANDLE;
		spec.PushConstantSize = sizeof(RoundedBoxPushConstants);
		spec.ColorAttachmentFormat = swapchain.ColourFormat;
		spec.EnableBlend = true;
		spec.DepthTestEnable = false;
		spec.DepthWriteEnable = false;
		spec.DepthAttachmentFormat = kSharedDepthFormat;
		impl.RoundedBoxPipeline.Init(device, spec, "Wisp RoundedBox Pipeline");
		impl.RoundedBoxPipelineInit = true;
	}

	// --- Image pipeline (sampled textures) ---
	{
		std::vector<uint32_t> vertSpv = VulkanShaderCompiler::Compile(kImageVertexGLSL, ShaderStage::Vertex, "ui_image.vert");
		std::vector<uint32_t> fragSpv = VulkanShaderCompiler::Compile(kImageFragmentGLSL, ShaderStage::Fragment, "ui_image.frag");
		impl.ImageVertexShader.Init(device, vertSpv, VK_SHADER_STAGE_VERTEX_BIT, "Wisp Image VS");
		impl.ImageFragmentShader.Init(device, fragSpv, VK_SHADER_STAGE_FRAGMENT_BIT, "Wisp Image FS");

		impl.ImageDescLayout.InitSingleCombinedImageSampler(device, VK_SHADER_STAGE_FRAGMENT_BIT, "Wisp Image Layout");
		impl.ImageDescAllocator.Init(device, 64, "Wisp Image Pool", 1);

		VkSamplerCreateInfo samplerCI{};
		samplerCI.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
		samplerCI.magFilter = VK_FILTER_LINEAR;
		samplerCI.minFilter = VK_FILTER_LINEAR;
		samplerCI.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
		samplerCI.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
		samplerCI.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
		samplerCI.maxLod = 1.0f;
		VK_CHECK(vkCreateSampler(device.GetDevice(), &samplerCI, nullptr, &impl.ImageSampler));

		VulkanPipelineSpec spec;
		spec.VertexShader = &impl.ImageVertexShader;
		spec.FragmentShader = &impl.ImageFragmentShader;
		spec.VertexStride = sizeof(BatchedVertex);
		spec.VertexAttributes = {
			{ 0, VK_FORMAT_R32G32B32_SFLOAT, offsetof(BatchedVertex, pos) },
			{ 1, VK_FORMAT_R32G32_SFLOAT,    offsetof(BatchedVertex, uv) },
			{ 2, VK_FORMAT_R8G8B8A8_UNORM,   offsetof(BatchedVertex, col) },
		};
		spec.DescriptorSetLayout = impl.ImageDescLayout.GetHandle();
		spec.PushConstantSize = sizeof(glm::mat4);
		spec.ColorAttachmentFormat = swapchain.ColourFormat;
		spec.EnableBlend = true;
		spec.DepthTestEnable = false;
		spec.DepthWriteEnable = false;
		spec.DepthAttachmentFormat = kSharedDepthFormat;
		impl.ImagePipeline.Init(device, spec, "Wisp Image Pipeline");
		impl.ImagePipelineInit = true;
	}

	// Create atlas and write descriptor sets
	CreateAtlasTexture(impl);
	if (impl.AtlasCreated)
	{
		for (uint32_t i = 0; i < VulkanDevice::TotalFrameSlots; ++i)
			WriteCombinedImageSamplerDescriptor(device, impl.TextDescSets[i], 0, impl.AtlasView, impl.AtlasSampler, impl.AtlasLayout);
	}

	return true;
}

void RendererVulkan::Shutdown()
{
	WispVulkanImpl& impl = *m_Impl;
	if (!impl.Device) return;

	VulkanDevice& device = *impl.Device;
	vkDeviceWaitIdle(device.GetDevice());
	VmaAllocator allocator = device.GetAllocator();

	// Default window state's staging (all frame slots)
	{
		for (StagingBuffers& s : m_DefaultState->Staging)
		{
			if (s.VB) vmaDestroyBuffer(allocator, s.VB, s.VBAlloc);
			if (s.IB) vmaDestroyBuffer(allocator, s.IB, s.IBAlloc);
			s = {};
		}
	}

	// Atlas
	if (impl.AtlasView)    vkDestroyImageView(device.GetDevice(), impl.AtlasView, nullptr);
	if (impl.AtlasSampler) vkDestroySampler(device.GetDevice(), impl.AtlasSampler, nullptr);
	if (impl.AtlasAlloc)   vmaDestroyImage(allocator, impl.AtlasImage, impl.AtlasAlloc);

	// Textures (images/views/allocations; the descriptor sets live in the
	// allocator pool and die with it)
	for (auto& [id, tex] : impl.Textures)
	{
		if (tex.View) vkDestroyImageView(device.GetDevice(), tex.View, nullptr);
		if (tex.Alloc) vmaDestroyImage(allocator, tex.Image, tex.Alloc);
	}
	impl.Textures.clear();

	// Descriptor sets & layout
	impl.TextDescAllocator.Shutdown();
	impl.TextDescLayout.Shutdown();
	impl.ImageDescAllocator.Shutdown();
	impl.ImageDescLayout.Shutdown();
	if (impl.ImageSampler) vkDestroySampler(device.GetDevice(), impl.ImageSampler, nullptr);

	// Pipelines & shaders
	if (impl.TextPipelineInit) { impl.TextPipeline.Shutdown(); impl.TextPipelineInit = false; }
	if (impl.RectPipelineInit) { impl.RectPipeline.Shutdown(); impl.RectPipelineInit = false; }
	if (impl.RoundedBoxPipelineInit) { impl.RoundedBoxPipeline.Shutdown(); impl.RoundedBoxPipelineInit = false; }
	if (impl.ImagePipelineInit) { impl.ImagePipeline.Shutdown(); impl.ImagePipelineInit = false; }
	impl.TextFragmentShader.Shutdown();
	impl.TextVertexShader.Shutdown();
	impl.RectFragmentShader.Shutdown();
	impl.RectVertexShader.Shutdown();
	impl.RoundedBoxFragmentShader.Shutdown();
	impl.RoundedBoxVertexShader.Shutdown();
	impl.ImageFragmentShader.Shutdown();
	impl.ImageVertexShader.Shutdown();

	impl.Device = nullptr;
}

void RendererVulkan::BeginFrame() {}

static void UploadTextureData(WispVulkanImpl& impl, WispVulkanImpl::WispTexture& tex, uint32_t w, uint32_t h, const uint8_t* rgbaData)
{
	VulkanDevice& device = *impl.Device;
	VmaAllocator allocator = device.GetAllocator();

	VkDeviceSize size = (VkDeviceSize)w * h * 4;
	VkBuffer stagingBuf = VK_NULL_HANDLE;
	VmaAllocation stagingAlloc = VK_NULL_HANDLE;
	VkBufferCreateInfo sCI{};
	sCI.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
	sCI.size = size;
	sCI.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
	VmaAllocationCreateInfo sACI{};
	sACI.usage = VMA_MEMORY_USAGE_CPU_TO_GPU;
	sACI.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT;
	vmaCreateBuffer(allocator, &sCI, &sACI, &stagingBuf, &stagingAlloc, nullptr);

	void* mapped;
	vmaMapMemory(allocator, stagingAlloc, &mapped);
	std::memcpy(mapped, rgbaData, size);
	vmaUnmapMemory(allocator, stagingAlloc);

	VkCommandBuffer cb = device.BeginSingleTimeCommands();

	VkImageMemoryBarrier2 toDst{};
	toDst.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2;
	toDst.srcStageMask = VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT;
	toDst.srcAccessMask = 0;
	toDst.dstStageMask = VK_PIPELINE_STAGE_2_COPY_BIT;
	toDst.dstAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
	toDst.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
	toDst.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
	toDst.image = tex.Image;
	toDst.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
	VkDependencyInfo dep1{};
	dep1.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
	dep1.imageMemoryBarrierCount = 1;
	dep1.pImageMemoryBarriers = &toDst;
	vkCmdPipelineBarrier2(cb, &dep1);

	VkBufferImageCopy region{};
	region.imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
	region.imageExtent = { w, h, 1 };
	vkCmdCopyBufferToImage(cb, stagingBuf, tex.Image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);

	VkImageMemoryBarrier2 toShader{};
	toShader.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2;
	toShader.srcStageMask = VK_PIPELINE_STAGE_2_COPY_BIT;
	toShader.srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
	toShader.dstStageMask = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT;
	toShader.dstAccessMask = VK_ACCESS_2_SHADER_READ_BIT;
	toShader.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
	toShader.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
	toShader.image = tex.Image;
	toShader.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
	VkDependencyInfo dep2{};
	dep2.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
	dep2.imageMemoryBarrierCount = 1;
	dep2.pImageMemoryBarriers = &toShader;
	vkCmdPipelineBarrier2(cb, &dep2);

	device.EndSingleTimeCommands(cb);
	vmaDestroyBuffer(allocator, stagingBuf, stagingAlloc);
}

uint64_t RendererVulkan::CreateTexture(uint32_t width, uint32_t height, const uint8_t* rgbaData)
{
	WispVulkanImpl& impl = *m_Impl;
	if (!impl.Device || !rgbaData || width == 0 || height == 0)
		return 0;

	VulkanDevice& device = *impl.Device;
	VmaAllocator allocator = device.GetAllocator();

	WispVulkanImpl::WispTexture tex;
	tex.Width = width;
	tex.Height = height;

	VkImageCreateInfo ci{};
	ci.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
	ci.imageType = VK_IMAGE_TYPE_2D;
	ci.format = VK_FORMAT_R8G8B8A8_UNORM;
	ci.extent = { width, height, 1 };
	ci.mipLevels = 1;
	ci.arrayLayers = 1;
	ci.samples = VK_SAMPLE_COUNT_1_BIT;
	ci.tiling = VK_IMAGE_TILING_OPTIMAL;
	ci.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
	ci.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
	VmaAllocationCreateInfo aci{};
	aci.usage = VMA_MEMORY_USAGE_GPU_ONLY;
	VK_CHECK(vmaCreateImage(allocator, &ci, &aci, &tex.Image, &tex.Alloc, nullptr));

	VkImageViewCreateInfo vci{};
	vci.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
	vci.image = tex.Image;
	vci.viewType = VK_IMAGE_VIEW_TYPE_2D;
	vci.format = VK_FORMAT_R8G8B8A8_UNORM;
	vci.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
	VK_CHECK(vkCreateImageView(device.GetDevice(), &vci, nullptr, &tex.View));

	tex.DescSet = impl.ImageDescAllocator.Allocate(impl.ImageDescLayout.GetHandle(), "Wisp Image DS " + std::to_string(impl.NextTextureId));
	WriteCombinedImageSamplerDescriptor(device, tex.DescSet, 0, tex.View, impl.ImageSampler, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);

	UploadTextureData(impl, tex, width, height, rgbaData);

	uint64_t id = impl.NextTextureId++;
	impl.Textures[id] = tex;
	return id;
}

void RendererVulkan::UpdateTexture(uint64_t handle, uint32_t x, uint32_t y, uint32_t width, uint32_t height, const uint8_t* rgbaData)
{
	WispVulkanImpl& impl = *m_Impl;
	auto it = impl.Textures.find(handle);
	if (it == impl.Textures.end() || !rgbaData)
		return;
	// Full-image replacement only (sufficient for the current consumers);
	// partial rectangle updates would need a region-sized staging copy.
	if (x != 0 || y != 0 || width != it->second.Width || height != it->second.Height)
		return;
	UploadTextureData(impl, it->second, width, height, rgbaData);
}

void RendererVulkan::DestroyTexture(uint64_t handle)
{
	WispVulkanImpl& impl = *m_Impl;
	auto it = impl.Textures.find(handle);
	if (it == impl.Textures.end())
		return;
	VulkanDevice& device = *impl.Device;
	VmaAllocator allocator = device.GetAllocator();
	vkDeviceWaitIdle(device.GetDevice());
	if (it->second.View) vkDestroyImageView(device.GetDevice(), it->second.View, nullptr);
	if (it->second.OwnsImage) vmaDestroyImage(allocator, it->second.Image, it->second.Alloc);
	impl.Textures.erase(it);
}

// Wrap an engine-acquired UIImageHandle (an offscreen render target's
// colour attachment or a loaded texture's image, both resting in
// SHADER_READ_ONLY_OPTIMAL when Wisp samples them) as a Wisp-samplable
// texture. The image is NOT owned - DestroyTexture frees only this
// wrapper's view/descriptor set, never the image itself. The image view's
// format comes from the handle, so BGRA8 framebuffer attachments and RGBA8
// textures each get the correct format (the pre-handle hook hardcoded
// BGRA8).
uint64_t RendererVulkan::WrapUIImageHandle(const TinyEngine::UIImageHandle& handle)
{
	WispVulkanImpl& impl = *m_Impl;
	VkImage image = (VkImage)handle.NativeImage;
	if (!impl.Device || !image || handle.Width == 0 || handle.Height == 0)
		return 0;

	VkFormat format = VK_FORMAT_UNDEFINED;
	switch (handle.Format)
	{
		case TinyEngine::UIImageHandleFormat::BGRA8: format = VK_FORMAT_B8G8R8A8_UNORM; break;
		case TinyEngine::UIImageHandleFormat::RGBA8: format = VK_FORMAT_R8G8B8A8_UNORM; break;
		default: return 0;
	}

	VulkanDevice& device = *impl.Device;

	WispVulkanImpl::WispTexture tex;
	tex.Image = image;
	tex.Width = handle.Width;
	tex.Height = handle.Height;
	tex.OwnsImage = false;

	VkImageViewCreateInfo vci{};
	vci.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
	vci.image = image;
	vci.viewType = VK_IMAGE_VIEW_TYPE_2D;
	vci.format = format;
	vci.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
	VK_CHECK(vkCreateImageView(device.GetDevice(), &vci, nullptr, &tex.View));

	tex.DescSet = impl.ImageDescAllocator.Allocate(impl.ImageDescLayout.GetHandle(), "Wisp Wrapped Image DS " + std::to_string(impl.NextTextureId));
	WriteCombinedImageSamplerDescriptor(device, tex.DescSet, 0, tex.View, impl.ImageSampler, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);

	uint64_t id = impl.NextTextureId++;
	impl.Textures[id] = tex;
	return id;
}

void RendererVulkan::RenderBatches(const BatchedDrawData& data, const glm::mat4& orthoProjection)
{
	RenderBatches(data, orthoProjection, *m_DefaultState);
}

void RendererVulkan::RenderBatches(const BatchedDrawData& data, const glm::mat4& orthoProjection, WispWindowRenderState& state)
{
	WispVulkanImpl& impl = *m_Impl;
	VkCommandBuffer cb = state.CommandBuffer;
	if (!impl.Device || !cb || data.Vertices.empty() || !impl.RectPipelineInit)
		return;

	VulkanDevice& device = *impl.Device;
	VmaAllocator allocator = device.GetAllocator();

	// Update atlas if dirty
	const FontCache* fc = impl.FontCachePtr;
	if (fc && fc->IsAtlasDirty())
	{
		CreateAtlasTexture(impl);
		if (impl.AtlasCreated)
		{
			for (uint32_t i = 0; i < VulkanDevice::TotalFrameSlots; ++i)
				WriteCombinedImageSamplerDescriptor(device, impl.TextDescSets[i], 0, impl.AtlasView, impl.AtlasSampler, impl.AtlasLayout);
			// Clear AFTER the rebuild so this frame's freshly rasterised
			// glyphs are uploaded; a glyph rasterised later re-dirties for
			// next frame. Without this the atlas rebuilt every frame.
			fc->ClearAtlasDirty();
		}
	}

	// Upload staging - per-frame-slot pair (see WispWindowRenderState):
	// the slot this window is recording right now was fence-waited by
	// AcquireNextImage, so overwriting it cannot race any in-flight GPU
	// read (frame N+1 waits frame N-1's fence, so slots alternate and
	// each slot is safe to write when its turn comes around again).
	uint32_t frameIndex = GetVulkanCurrentFrameIndex();
	TE_CORE_ASSERT(frameIndex < VulkanDevice::TotalFrameSlots, "Wisp frame index out of range!");
	StagingBuffers& staging = state.Staging[frameIndex % VulkanDevice::FramesInFlight];
	uint32_t vbBytes = (uint32_t)(data.Vertices.size() * sizeof(BatchedVertex));
	uint32_t ibBytes = (uint32_t)(data.Indices.size() * sizeof(uint32_t));
	EnsureStagingBuffers(impl, staging, vbBytes, ibBytes);
	UploadToStaging(allocator, staging.VBAlloc, data.Vertices.data(), vbBytes);
	UploadToStaging(allocator, staging.IBAlloc, data.Indices.data(), ibBytes);

	// Viewport (positive-height — sampling pass)
	VkViewport viewport{};
	viewport.x = 0.0f;
	viewport.y = 0.0f;
	viewport.width  = (float)state.ViewportW;
	viewport.height = (float)state.ViewportH;
	viewport.minDepth = 0.0f;
	viewport.maxDepth = 1.0f;
	vkCmdSetViewport(cb, 0, 1, &viewport);

	VkRect2D scissor{};
	scissor.extent = { state.ViewportW, state.ViewportH };
	vkCmdSetScissor(cb, 0, 1, &scissor);

	// Bind vertex/index buffers
	VkDeviceSize vbOffset = 0;
	vkCmdBindVertexBuffers(cb, 0, 1, &staging.VB, &vbOffset);
	vkCmdBindIndexBuffer(cb, staging.IB, 0, VK_INDEX_TYPE_UINT32);

	// Slot-encoded frame index: the window recording right now pointed the
	// global at its own slot (VulkanContext::SwapBuffers), so this reads the
	// descriptor set belonging to this window's frame, not another window's.
	// (frameIndex fetched above, right before the staging upload.)

	for (const auto& batch : data.Batches)
	{
		if (batch.IndexCount == 0) continue;

		// Apply scissor from clip rect (scale logical→physical)
		if (batch.ClipIndex >= 0 && batch.ClipIndex < (int)data.ClipRects.size())
		{
			const glm::vec4& cr = data.ClipRects[batch.ClipIndex];
			VkRect2D clipScissor;
			clipScissor.offset.x = (int32_t)(cr.x * data.DpiScale);
			clipScissor.offset.y = (int32_t)(cr.y * data.DpiScale);
			clipScissor.extent.width  = (uint32_t)(cr.z * data.DpiScale);
			clipScissor.extent.height = (uint32_t)(cr.w * data.DpiScale);
			vkCmdSetScissor(cb, 0, 1, &clipScissor);
		}
		else
		{
			VkRect2D fullScissor{};
			fullScissor.extent = { state.ViewportW, state.ViewportH };
			vkCmdSetScissor(cb, 0, 1, &fullScissor);
		}

		VulkanPipeline* pipeline = nullptr;
		if (batch.TextureId == 0)
		{
			pipeline = &impl.RectPipeline;
			vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline->GetHandle());
			vkCmdPushConstants(cb, pipeline->GetLayout(), VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof(glm::mat4), &orthoProjection);
		}
		else if (batch.TextureId == 2)
		{
			pipeline = &impl.RoundedBoxPipeline;
			vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline->GetHandle());

			RoundedBoxPushConstants pc;
			pc.projection = orthoProjection;
			std::memcpy(pc.radii, batch.Sdf.CornerRadii, sizeof(pc.radii));
			pc.outlineWidth = batch.Sdf.OutlineWidth;
			std::memcpy(pc.outlineColour, batch.Sdf.OutlineColour, sizeof(pc.outlineColour));
			pc.halfSize[0] = batch.Sdf.Size[0] * 0.5f;
			pc.halfSize[1] = batch.Sdf.Size[1] * 0.5f;
			pc._pad2[0] = 0; pc._pad2[1] = 0;
			vkCmdPushConstants(cb, pipeline->GetLayout(), VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof(pc), &pc);
		}
		else if (batch.TextureId == 1)
		{
			pipeline = &impl.TextPipeline;
			vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline->GetHandle());
			vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline->GetLayout(),
				0, 1, &impl.TextDescSets[frameIndex], 0, nullptr);
			vkCmdPushConstants(cb, pipeline->GetLayout(), VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof(glm::mat4), &orthoProjection);
		}
		else
		{
			// Sampled image (IUIRenderer::CreateTexture handles).
			auto texIt = impl.Textures.find(batch.TextureId);
			if (texIt == impl.Textures.end() || !impl.ImagePipelineInit)
			{
				pipeline = &impl.RectPipeline;
				vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline->GetHandle());
				vkCmdPushConstants(cb, pipeline->GetLayout(), VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof(glm::mat4), &orthoProjection);
			}
			else
			{
				pipeline = &impl.ImagePipeline;
				vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline->GetHandle());
				vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline->GetLayout(),
					0, 1, &texIt->second.DescSet, 0, nullptr);
				vkCmdPushConstants(cb, pipeline->GetLayout(), VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof(glm::mat4), &orthoProjection);
			}
		}

		vkCmdDrawIndexed(cb, batch.IndexCount, 1, batch.IndexOffset, 0, 0);
	}
}

} // namespace wisp
