#include "Wisp/RendererVulkan.h"
#include "TinyEngine/CVar/EngineCVars.h"
#include "Wisp/FontCache.h"

#include "TinyEngine/RHI/RHI.h"
#include "TinyEngine/Core/Application.h"
#include "TinyEngine/RHI/RhiDevice.h"
#include "TinyEngine/Profiler/Profiler.h"
#include "TinyEngine/Asset/Load/TinyAssetLoader.h"
#ifndef TE_NO_RENDERER
#include "TinyEngine/Asset/AssetManager.h"
#else
#include "TinyEngine/Asset/AssetPaths.h"
#include "TinyEngine/Core/Project.h"
#endif

#include <cstring>
#include <glm/gtc/matrix_transform.hpp>

namespace wisp {

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

// The text/image pipelines push the texture's bindless index past the mat4
// (the fragment reads it via nonuniformEXT - the mesh-family precedent).
struct TexturedPushConstants {
	glm::mat4 projection;
	uint32_t  texIndex;
};

struct StagingBuffers
{
	TinyEngine::rhi::BufferHandle VB;
	TinyEngine::rhi::BufferHandle IB;
	uint32_t VBCapacity = 0;
	uint32_t IBCapacity = 0;
};

// The RHI-backed implementation. No raw backend identifiers here or
// anywhere in Wisp; everything goes through rhi::Device + handles.
// The committed/cooked UI shaders (no runtime compile).
static std::string WispShaderPath(const std::string& name)
{
	const std::string cooked = "shaders/vulkan/" + name + ".ctasset";
	TinyEngine::Engine& engine = TinyEngine::Application::Get().GetEngine();
#ifndef TE_NO_RENDERER
	return engine.Get<TinyEngine::AssetManager>().ResolvePluginAsset("wisp", cooked);
#else
	// A TE_TOOL app has no AssetManager (and no pack): the cooked file
	// next to the build, else the committed one.
	return TinyEngine::AssetPaths::ResolvePluginAsset(engine.Get<TinyEngine::Project>(), nullptr, "wisp", cooked);
#endif
}



struct WispVulkanImpl
{
	TinyEngine::rhi::Device* Device = nullptr;

	TinyEngine::rhi::ShaderHandle RectVertexShader;
	TinyEngine::rhi::ShaderHandle RectFragmentShader;
	TinyEngine::rhi::PipelineHandle RectPipeline;
	bool RectPipelineInit = false;

	TinyEngine::rhi::ShaderHandle TextVertexShader;
	TinyEngine::rhi::ShaderHandle TextFragmentShader;
	TinyEngine::rhi::PipelineHandle TextPipeline;
	bool TextPipelineInit = false;

	TinyEngine::rhi::ShaderHandle RoundedBoxVertexShader;
	TinyEngine::rhi::ShaderHandle RoundedBoxFragmentShader;
	TinyEngine::rhi::PipelineHandle RoundedBoxPipeline;
	bool RoundedBoxPipelineInit = false;

	TinyEngine::rhi::ShaderHandle ImageVertexShader;
	TinyEngine::rhi::ShaderHandle ImageFragmentShader;
	TinyEngine::rhi::PipelineHandle ImagePipeline;
	bool ImagePipelineInit = false;

	// Atlas texture (nearest-sampled font atlas, single level - rebuilt when
	// the font cache dirties it).
	TinyEngine::rhi::TextureHandle Atlas;
	uint32_t AtlasWidth = 0;
	uint32_t AtlasHeight = 0;
	bool AtlasCreated = false;

	struct WispTexture
	{
		TinyEngine::rhi::TextureHandle Handle;
		uint32_t Width = 0, Height = 0;
	};
	std::unordered_map<uint64_t, WispTexture> Textures;
	// Texture ids start above the batcher's reserved pipeline ids
	// (1 = Text, 2 = RoundedBox) - id 1 or 2 would route the draw into
	// the wrong pipeline.
	uint64_t NextTextureId = 3;

	const FontCache* FontCachePtr = nullptr;
};

// Per-window transient state: staging buffers and the viewport the window
// is being recorded into (the command list is the pass's, handed to
// RenderBatches). One per WindowContext; the shared WispVulkanImpl holds
// everything windows reuse. The staging buffers are per-frame-slot: with
// only two frames in flight, frame N+1's fence wait only covers frame N-1,
// so a single shared pair was being overwritten while frame N's GPU read of
// it could still be in flight (a HOST-write -> GPU-read race that tears the
// UI geometry during interaction).
struct WispWindowRenderState
{
	StagingBuffers          Staging[2];
	uint32_t                ViewportW = 1920;
	uint32_t                ViewportH = 1080;
};

// ---- RendererVulkan --------------------------------------------------------

RendererVulkan::RendererVulkan()
	: m_Impl(std::make_unique<WispVulkanImpl>()), m_DefaultState(std::make_unique<WispWindowRenderState>()) {}
RendererVulkan::~RendererVulkan() { Shutdown(); }

void RendererVulkan::SetViewportSize(uint32_t width, uint32_t height)
{
	m_DefaultState->ViewportW = width;
	m_DefaultState->ViewportH = height;
}

WispWindowRenderState* RendererVulkan::CreateWindowState()
{
	return new WispWindowRenderState();
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
		// P-D: the staging buffers are RHI handles - DestroyBuffer queues
		// them into the retire queue (no device stall needed here anymore).
		for (StagingBuffers& s : state->Staging)
		{
			if (TinyEngine::rhi::IsValid(s.VB))
			{
				impl.Device->DestroyBuffer(s.VB);
				s.VB = {};
			}
			if (TinyEngine::rhi::IsValid(s.IB))
			{
				impl.Device->DestroyBuffer(s.IB);
				s.IB = {};
			}
		}
	}
	delete state;
}

static void EnsureStagingBuffers(WispVulkanImpl& impl, StagingBuffers& s, uint32_t vbBytes, uint32_t ibBytes)
{
	TinyEngine::rhi::Device& device = *impl.Device;

	if (vbBytes > 0 && (!TinyEngine::rhi::IsValid(s.VB) || s.VBCapacity < vbBytes))
	{
		if (TinyEngine::rhi::IsValid(s.VB))
			device.DestroyBuffer(s.VB);
		TinyEngine::rhi::BufferDesc desc;
		desc.Size = vbBytes;
		desc.HostVisible = true;
		desc.Usage = TinyEngine::rhi::BufferUsage::Vertex | TinyEngine::rhi::BufferUsage::Index | TinyEngine::rhi::BufferUsage::TransferDst; // the old fixed flags
		s.VB = device.CreateBuffer(desc, "Wisp UI Vertex Staging");
		s.VBCapacity = vbBytes;
	}

	if (ibBytes > 0 && (!TinyEngine::rhi::IsValid(s.IB) || s.IBCapacity < ibBytes))
	{
		if (TinyEngine::rhi::IsValid(s.IB))
			device.DestroyBuffer(s.IB);
		TinyEngine::rhi::BufferDesc desc;
		desc.Size = ibBytes;
		desc.HostVisible = true;
		desc.Usage = TinyEngine::rhi::BufferUsage::Vertex | TinyEngine::rhi::BufferUsage::Index | TinyEngine::rhi::BufferUsage::TransferDst; // the old fixed flags
		s.IB = device.CreateBuffer(desc, "Wisp UI Index Staging");
		s.IBCapacity = ibBytes;
	}
}

static void CreateAtlasTexture(WispVulkanImpl& impl)
{
	const FontCache* fc = impl.FontCachePtr;
	if (!fc || !fc->GetAtlasData()) return;

	TinyEngine::rhi::Device& device = *impl.Device;
	uint32_t w = fc->GetAtlasWidth();
	uint32_t h = fc->GetAtlasHeight();

	// P-D: the old atlas retires through the queue (no device stall - the
	// stamp keeps it alive until the referencing frames complete); the new
	// atlas gets a fresh bindless slot + a nearest-sampled single-level
	// upload (the fixed sampler's maxLod-0 clamp reproduces Wisp's old
	// NEAREST + maxLod 1.0 + mipLevels 1 exactly).
	if (TinyEngine::rhi::IsValid(impl.Atlas))
	{
		device.DestroyTexture(impl.Atlas);
		impl.Atlas = {};
	}

	TinyEngine::rhi::TextureDesc desc;
	desc.Width = w;
	desc.Height = h;
	desc.MipLevels = 1;
	desc.Sampler = TinyEngine::rhi::SamplerKind::Nearest;
	impl.Atlas = device.CreateTexture(desc, fc->GetAtlasData(), (uint64_t)w * h * 4, "Wisp Font Atlas");
	impl.AtlasWidth = w;
	impl.AtlasHeight = h;
	impl.AtlasCreated = true;
}

static const TinyEngine::rhi::PipelineStateDesc::Attribute* WispVertexAttributes()
{
	static const TinyEngine::rhi::PipelineStateDesc::Attribute attributes[] = {
		{ 0, TinyEngine::rhi::Format::R32G32B32_SFLOAT, offsetof(BatchedVertex, pos) },
		{ 1, TinyEngine::rhi::Format::R32G32_SFLOAT, offsetof(BatchedVertex, uv) },
		{ 2, TinyEngine::rhi::Format::R8G8B8A8_UNORM, offsetof(BatchedVertex, col) },
	};
	return attributes;
}

bool RendererVulkan::Init(const FontCache* fontCache)
{
	WispVulkanImpl& impl = *m_Impl;
	impl.Device = &TinyEngine::Application::Get().GetEngine().Get<TinyEngine::RhiDevice>().Get();
	impl.FontCachePtr = fontCache;

	TinyEngine::rhi::Device& device = *impl.Device;
	// Wisp draws only into the SWAPCHAIN, whose colour format is the fixed
	// B8G8R8A8_UNORM - the pipelines declare it directly (no raw
	// swapchain-info reach into Wisp). Framebuffer targets' formats are
	// the renderers' concern (they take them from the target); the UI
	// samples framebuffers through WrapUIImageHandle's format mapping.
	const TinyEngine::rhi::Format kUiColourFormat = TinyEngine::rhi::Format::B8G8R8A8_UNORM;

	// --- Rect pipeline ---
	{
		std::vector<uint32_t> vertSpv = TinyEngine::TinyAssetLoader::LoadShaderSPIRV(WispShaderPath("ui_rect.vert"));
		std::vector<uint32_t> fragSpv = TinyEngine::TinyAssetLoader::LoadShaderSPIRV(WispShaderPath("ui_rect.frag"));
		impl.RectVertexShader = device.CreateShader(vertSpv, TinyEngine::rhi::ShaderStage::Vertex, "ui_rect.vert");
		impl.RectFragmentShader = device.CreateShader(fragSpv, TinyEngine::rhi::ShaderStage::Fragment, "ui_rect.frag");

		TinyEngine::rhi::PipelineStateDesc spec;
		spec.VertexShader = impl.RectVertexShader;
		spec.FragmentShader = impl.RectFragmentShader;
		spec.VertexStride = sizeof(BatchedVertex);
		spec.VertexAttributes = std::span(WispVertexAttributes(), 3);
		spec.PushConstantSize = sizeof(glm::mat4);
		spec.ColorAttachmentFormats[0] = kUiColourFormat;
		spec.EnableBlend = true;
		spec.DepthTestEnable = false;
		spec.DepthWriteEnable = false;
		spec.DepthAttachmentFormat = TinyEngine::rhi::Format::D32_SFLOAT;
		impl.RectPipeline = device.CreatePipeline(spec, "Wisp Rect Pipeline");
		impl.RectPipelineInit = true;
	}

	// --- Text pipeline (atlas-textured quads) ---
	{
		std::vector<uint32_t> vertSpv = TinyEngine::TinyAssetLoader::LoadShaderSPIRV(WispShaderPath("ui_text.vert"));
		std::vector<uint32_t> fragSpv = TinyEngine::TinyAssetLoader::LoadShaderSPIRV(WispShaderPath("ui_text.frag"));
		impl.TextVertexShader = device.CreateShader(vertSpv, TinyEngine::rhi::ShaderStage::Vertex, "ui_text.vert");
		impl.TextFragmentShader = device.CreateShader(fragSpv, TinyEngine::rhi::ShaderStage::Fragment, "ui_text.frag");

		TinyEngine::rhi::PipelineStateDesc spec;
		spec.VertexShader = impl.TextVertexShader;
		spec.FragmentShader = impl.TextFragmentShader;
		spec.VertexStride = sizeof(BatchedVertex);
		spec.VertexAttributes = std::span(WispVertexAttributes(), 3);
		spec.PushConstantSize = sizeof(TexturedPushConstants);
		spec.PushConstantStages = 1 | 16; // VERTEX|FRAGMENT - the frag reads the index
		spec.ColorAttachmentFormats[0] = kUiColourFormat;
		spec.EnableBlend = true;
		spec.DepthTestEnable = false;
		spec.DepthWriteEnable = false;
		spec.DepthAttachmentFormat = TinyEngine::rhi::Format::D32_SFLOAT;
		impl.TextPipeline = device.CreatePipeline(spec, "Wisp Text Pipeline");
		impl.TextPipelineInit = true;
	}

	// --- RoundedBox pipeline ---
	{
		std::vector<uint32_t> vertSpv = TinyEngine::TinyAssetLoader::LoadShaderSPIRV(WispShaderPath("ui_roundedbox.vert"));
		std::vector<uint32_t> fragSpv = TinyEngine::TinyAssetLoader::LoadShaderSPIRV(WispShaderPath("ui_roundedbox.frag"));
		impl.RoundedBoxVertexShader = device.CreateShader(vertSpv, TinyEngine::rhi::ShaderStage::Vertex, "ui_roundedbox.vert");
		impl.RoundedBoxFragmentShader = device.CreateShader(fragSpv, TinyEngine::rhi::ShaderStage::Fragment, "ui_roundedbox.frag");

		TinyEngine::rhi::PipelineStateDesc spec;
		spec.VertexShader = impl.RoundedBoxVertexShader;
		spec.FragmentShader = impl.RoundedBoxFragmentShader;
		spec.VertexStride = sizeof(BatchedVertex);
		spec.VertexAttributes = std::span(WispVertexAttributes(), 3);
		spec.PushConstantSize = sizeof(RoundedBoxPushConstants);
		spec.ColorAttachmentFormats[0] = kUiColourFormat;
		spec.EnableBlend = true;
		spec.DepthTestEnable = false;
		spec.DepthWriteEnable = false;
		spec.DepthAttachmentFormat = TinyEngine::rhi::Format::D32_SFLOAT;
		impl.RoundedBoxPipeline = device.CreatePipeline(spec, "Wisp RoundedBox Pipeline");
		impl.RoundedBoxPipelineInit = true;
	}

	// --- Image pipeline (per-texture sampled quads) ---
	{
		std::vector<uint32_t> vertSpv = TinyEngine::TinyAssetLoader::LoadShaderSPIRV(WispShaderPath("ui_image.vert"));
		std::vector<uint32_t> fragSpv = TinyEngine::TinyAssetLoader::LoadShaderSPIRV(WispShaderPath("ui_image.frag"));
		impl.ImageVertexShader = device.CreateShader(vertSpv, TinyEngine::rhi::ShaderStage::Vertex, "ui_image.vert");
		impl.ImageFragmentShader = device.CreateShader(fragSpv, TinyEngine::rhi::ShaderStage::Fragment, "ui_image.frag");

		TinyEngine::rhi::PipelineStateDesc spec;
		spec.VertexShader = impl.ImageVertexShader;
		spec.FragmentShader = impl.ImageFragmentShader;
		spec.VertexStride = sizeof(BatchedVertex);
		spec.VertexAttributes = std::span(WispVertexAttributes(), 3);
		spec.PushConstantSize = sizeof(TexturedPushConstants);
		spec.PushConstantStages = 1 | 16; // VERTEX|FRAGMENT
		spec.ColorAttachmentFormats[0] = kUiColourFormat;
		spec.EnableBlend = true;
		spec.DepthTestEnable = false;
		spec.DepthWriteEnable = false;
		spec.DepthAttachmentFormat = TinyEngine::rhi::Format::D32_SFLOAT;
		impl.ImagePipeline = device.CreatePipeline(spec, "Wisp Image Pipeline");
		impl.ImagePipelineInit = true;
	}

	CreateAtlasTexture(impl);

	return true;
}

void RendererVulkan::Shutdown()
{
	WispVulkanImpl& impl = *m_Impl;
	if (!impl.Device) return;

	TinyEngine::rhi::Device& device = *impl.Device;

	// Default window state's staging (all frame slots)
	for (StagingBuffers& s : m_DefaultState->Staging)
	{
		if (TinyEngine::rhi::IsValid(s.VB)) device.DestroyBuffer(s.VB);
		if (TinyEngine::rhi::IsValid(s.IB)) device.DestroyBuffer(s.IB);
		s = {};
	}

	if (TinyEngine::rhi::IsValid(impl.Atlas))
		device.DestroyTexture(impl.Atlas);
	impl.Atlas = {};
	impl.AtlasCreated = false;

	for (auto& [id, tex] : impl.Textures)
		device.DestroyTexture(tex.Handle);
	impl.Textures.clear();

	if (impl.TextPipelineInit) { device.DestroyPipeline(impl.TextPipeline); impl.TextPipelineInit = false; }
	if (impl.RectPipelineInit) { device.DestroyPipeline(impl.RectPipeline); impl.RectPipelineInit = false; }
	if (impl.RoundedBoxPipelineInit) { device.DestroyPipeline(impl.RoundedBoxPipeline); impl.RoundedBoxPipelineInit = false; }
	if (impl.ImagePipelineInit) { device.DestroyPipeline(impl.ImagePipeline); impl.ImagePipelineInit = false; }
	if (TinyEngine::rhi::IsValid(impl.TextFragmentShader)) device.DestroyShader(impl.TextFragmentShader);
	if (TinyEngine::rhi::IsValid(impl.TextVertexShader)) device.DestroyShader(impl.TextVertexShader);
	if (TinyEngine::rhi::IsValid(impl.RectFragmentShader)) device.DestroyShader(impl.RectFragmentShader);
	if (TinyEngine::rhi::IsValid(impl.RectVertexShader)) device.DestroyShader(impl.RectVertexShader);
	if (TinyEngine::rhi::IsValid(impl.RoundedBoxFragmentShader)) device.DestroyShader(impl.RoundedBoxFragmentShader);
	if (TinyEngine::rhi::IsValid(impl.RoundedBoxVertexShader)) device.DestroyShader(impl.RoundedBoxVertexShader);
	if (TinyEngine::rhi::IsValid(impl.ImageFragmentShader)) device.DestroyShader(impl.ImageFragmentShader);
	if (TinyEngine::rhi::IsValid(impl.ImageVertexShader)) device.DestroyShader(impl.ImageVertexShader);

	impl.Device = nullptr;
}

void RendererVulkan::BeginFrame() {}

uint64_t RendererVulkan::CreateTexture(uint32_t width, uint32_t height, const uint8_t* rgbaData)
{
	WispVulkanImpl& impl = *m_Impl;
	if (!impl.Device || !rgbaData || width == 0 || height == 0)
		return 0;

	TinyEngine::rhi::Device& device = *impl.Device;

	TinyEngine::rhi::TextureDesc desc;
	desc.Width = width;
	desc.Height = height;
	desc.MipLevels = 1;
	desc.Sampler = TinyEngine::rhi::SamplerKind::Linear;

	WispVulkanImpl::WispTexture tex;
	tex.Handle = device.CreateTexture(desc, rgbaData, (uint64_t)width * height * 4, "Wisp UI Texture");
	tex.Width = width;
	tex.Height = height;

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
	impl.Device->SetTextureData(it->second.Handle, rgbaData, (uint64_t)width * height * 4);
}

void RendererVulkan::DestroyTexture(uint64_t handle)
{
	WispVulkanImpl& impl = *m_Impl;
	auto it = impl.Textures.find(handle);
	if (it == impl.Textures.end())
		return;
	// P-D: deferred - the retire queue keeps the image (or wrapped view)
	// alive until the referencing frames complete (no device stall).
	impl.Device->DestroyTexture(it->second.Handle);
	impl.Textures.erase(it);
}

// Wrap an engine-acquired UIImageHandle (an offscreen render target's
// colour attachment or a loaded texture's image, both resting in
// SHADER_READ_ONLY_OPTIMAL when Wisp samples them) as a Wisp-samplable
// texture. The image is NOT owned - DestroyTexture frees only this
// wrapper's view + bindless slot, never the image itself. The image view's
// format comes from the handle, so BGRA8 framebuffer attachments and RGBA8
// textures each get the correct format.
uint64_t RendererVulkan::WrapUIImageHandle(const TinyEngine::UIImageHandle& handle)
{
	WispVulkanImpl& impl = *m_Impl;
	if (!impl.Device || !handle.NativeImage || handle.Width == 0 || handle.Height == 0)
		return 0;

	TinyEngine::rhi::Format format;
	switch (handle.Format)
	{
		case TinyEngine::UIImageHandleFormat::BGRA8: format = TinyEngine::rhi::Format::B8G8R8A8_UNORM; break;
		case TinyEngine::UIImageHandleFormat::RGBA8: format = TinyEngine::rhi::Format::R8G8B8A8_UNORM; break;
		default: return 0;
	}

	WispVulkanImpl::WispTexture tex;
	tex.Handle = impl.Device->CreateTextureFromNativeImage(handle.NativeImage, handle.Width, handle.Height, format);
	tex.Width = handle.Width;
	tex.Height = handle.Height;

	uint64_t id = impl.NextTextureId++;
	impl.Textures[id] = tex;
	return id;
}

void RendererVulkan::RenderBatches(const BatchedDrawData& data, const glm::mat4& orthoProjection, TinyEngine::rhi::CommandList& commandList)
{
	RenderBatches(data, orthoProjection, *m_DefaultState, commandList);
}

void RendererVulkan::EnsureFrameResources(const BatchedDrawData& data, WispWindowRenderState& state, uint32_t frameIndex)
{
	WispVulkanImpl& impl = *m_Impl;
	if (!impl.Device)
		return;

	// Rebuild the atlas when dirty (glyphs rasterised during this frame's
	// layout at extract time). Cleared AFTER the rebuild so freshly
	// rasterised glyphs upload once; a glyph rasterised later re-dirties.
	const FontCache* fc = impl.FontCachePtr;
	if (fc && fc->IsAtlasDirty())
	{
		CreateAtlasTexture(impl);
		if (impl.AtlasCreated)
			fc->ClearAtlasDirty();
	}

	// Grow the staging buffers to cover this frame's batch data (the slot
	// matches RenderBatches' pick - same frame, same CurrentFrameIndex).
	StagingBuffers& staging = state.Staging[frameIndex % 2];
	uint32_t vbBytes = (uint32_t)(data.Vertices.size() * sizeof(BatchedVertex));
	uint32_t ibBytes = (uint32_t)(data.Indices.size() * sizeof(uint32_t));
	EnsureStagingBuffers(impl, staging, vbBytes, ibBytes);
}

void RendererVulkan::RenderBatches(const BatchedDrawData& data, const glm::mat4& orthoProjection, WispWindowRenderState& state, TinyEngine::rhi::CommandList& commandList)
{
	WispVulkanImpl& impl = *m_Impl;
	if (!impl.Device || data.Vertices.empty() || !impl.RectPipelineInit)
		return;

	const uint32_t frameIndex = commandList.FrameSlot();

	TinyEngine::rhi::Device& device = *impl.Device;

	// Upload staging - per-frame-slot pair (see WispWindowRenderState):
	// the slot this window is recording right now was fence-waited by
	// AcquireNextImage, so overwriting it cannot race any in-flight GPU
	// read (frame N+1 waits frame N-1's fence, so slots alternate and
	// each slot is safe to write when its turn comes around again).
	// The per-window slot is the frame counter % 2 (the window-slot base
	// in the old frame index was always even, so it contributed nothing).
	// T-F-B.3: the buffers themselves were ensured by EnsureFrameResources
	// on the extract side - recording never creates device objects.
	StagingBuffers& staging = state.Staging[frameIndex % 2];
	uint32_t vbBytes = (uint32_t)(data.Vertices.size() * sizeof(BatchedVertex));
	uint32_t ibBytes = (uint32_t)(data.Indices.size() * sizeof(uint32_t));
	TE_CORE_ASSERT(TinyEngine::rhi::IsValid(staging.VB) && staging.VBCapacity >= vbBytes,
		"RenderBatches ran without EnsureFrameResources covering this frame's vertex batch!");
	TE_CORE_ASSERT(TinyEngine::rhi::IsValid(staging.IB) && staging.IBCapacity >= ibBytes,
		"RenderBatches ran without EnsureFrameResources covering this frame's index batch!");
	if (vbBytes) device.SetBufferData(staging.VB, data.Vertices.data(), vbBytes);
	if (ibBytes) device.SetBufferData(staging.IB, data.Indices.data(), ibBytes);

	// Record straight into the pass's list (already recording - no
	// Begin/End; its slot picks the bindless set and the timestamp pool).
	// The list may track a bound layout from earlier calls in the pass:
	// every batch below binds its pipeline before its
	// BindGlobalSet/PushConstants (the viewport, scissor and labels need
	// no layout), so nothing reads a stale one.

	// Viewport (positive-height — sampling pass)
	commandList.SetViewport(0.0f, 0.0f, (float)state.ViewportW, (float)state.ViewportH, 0.0f, 1.0f);

	const uint32_t atlasIndex = TinyEngine::rhi::IsValid(impl.Atlas) ? device.GetBindlessIndex(impl.Atlas) : 0;

	// One timed GPU scope for the whole UI pass, plus a label per batch
	// naming its pipeline (the rounded-box SDFs, the glyph atlas text, the
	// sampled images) for RenderDoc/Nsight. The batch labels are untimed
	// unless profiler.gpuTimeUiBatches is on: a full-width panel draws
	// hundreds of batches, whose timestamp pairs filled the per-frame pool
	// and left the renderer's own passes untimed .
	commandList.BeginLabel("Wisp UI");
	const bool timeBatches = TinyEngine::cvars::profiler_gpu_time_ui_batches.Get();

	for (const auto& batch : data.Batches)
	{
		if (batch.IndexCount == 0) continue;
		// A pipeline not built yet skips the batch BEFORE its label opens
		// (a skip after it left the label stack unbalanced - F1).
		if ((batch.TextureId == 0 && !impl.RectPipelineInit)
			|| (batch.TextureId == 2 && !impl.RoundedBoxPipelineInit)
			|| (batch.TextureId == 1 && !impl.TextPipelineInit))
			continue;

		// Apply scissor from clip rect (scale logical->physical)
		if (batch.ClipIndex >= 0 && batch.ClipIndex < (int)data.ClipRects.size())
		{
			const glm::vec4& cr = data.ClipRects[batch.ClipIndex];
			// The logical clip rects convert to the physical
			// scissor (the DPI scale + the UI scale).
			const float clipScale = data.DpiScale * TinyEngine::cvars::ui_scale.Get();
			commandList.SetScissor((int32_t)(cr.x * clipScale), (int32_t)(cr.y * clipScale),
				(uint32_t)(cr.z * clipScale), (uint32_t)(cr.w * clipScale));
		}
		else
		{
			commandList.SetScissor(0, 0, state.ViewportW, state.ViewportH);
		}

		const char* batchLabel = "Wisp UI: Rect";
		if (batch.TextureId == 2)
			batchLabel = "Wisp UI: RoundedBox SDF";
		else if (batch.TextureId == 1)
			batchLabel = "Wisp UI: Text Atlas";
		else if (batch.TextureId >= 3)
			batchLabel = "Wisp UI: Image";
		commandList.BeginLabel(batchLabel, timeBatches);

		if (batch.TextureId == 0)
		{
			commandList.BindPipeline(impl.RectPipeline);
			commandList.BindGlobalSet();
			commandList.PushConstants(1, 0, &orthoProjection, sizeof(glm::mat4));
		}
		else if (batch.TextureId == 2)
		{
			commandList.BindPipeline(impl.RoundedBoxPipeline);
			commandList.BindGlobalSet();

			RoundedBoxPushConstants pc;
			pc.projection = orthoProjection;
			std::memcpy(pc.radii, batch.Sdf.CornerRadii, sizeof(pc.radii));
			pc.outlineWidth = batch.Sdf.OutlineWidth;
			std::memcpy(pc.outlineColour, batch.Sdf.OutlineColour, sizeof(pc.outlineColour));
			pc.halfSize[0] = batch.Sdf.Size[0] * 0.5f;
			pc.halfSize[1] = batch.Sdf.Size[1] * 0.5f;
			pc._pad2[0] = 0; pc._pad2[1] = 0;
			commandList.PushConstants(1, 0, &pc, sizeof(pc));
		}
		else if (batch.TextureId == 1)
		{
			commandList.BindPipeline(impl.TextPipeline);
			commandList.BindGlobalSet();

			TexturedPushConstants pc;
			pc.projection = orthoProjection;
			pc.texIndex = atlasIndex;
			commandList.PushConstants(1 | 16, 0, &pc, sizeof(pc));
		}
		else
		{
			// Sampled image (IUIRenderer::CreateTexture handles).
			auto texIt = impl.Textures.find(batch.TextureId);
			if (texIt == impl.Textures.end() || !impl.ImagePipelineInit)
			{
				// Keep the pixel-correct fallback for a dangling texture id:
				// draw the quad as solid colour through the rect pipeline.
				commandList.BindPipeline(impl.RectPipeline);
				commandList.BindGlobalSet();
				commandList.PushConstants(1, 0, &orthoProjection, sizeof(glm::mat4));
			}
			else
			{
				commandList.BindPipeline(impl.ImagePipeline);
				commandList.BindGlobalSet();

				TexturedPushConstants pc;
				pc.projection = orthoProjection;
				pc.texIndex = device.GetBindlessIndex(texIt->second.Handle);
				commandList.PushConstants(1 | 16, 0, &pc, sizeof(pc));
			}
		}

		commandList.BindVertexBuffer(staging.VB, 0);
		commandList.BindIndexBuffer(staging.IB, 0);
		commandList.DrawIndexed(batch.IndexCount, 1, batch.IndexOffset, 0, 0);

		commandList.EndLabel();
	}

	commandList.EndLabel();
}

} // namespace wisp
