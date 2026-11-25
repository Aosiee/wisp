#include "Wisp/Rhi/RhiRenderer.h"

#include "Wisp/FontCache.h"
#include "Wisp/Rhi/Shaders/UiShaders.h"

#include "hearth/Assert.h"
#include "hearth/rhi/RHI.h"

#include <cstddef>
#include <cstring>
#include <span>
#include <unordered_map>

namespace wisp {

namespace rhi = hearth::rhi;

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
	rhi::BufferHandle VB;
	rhi::BufferHandle IB;
	uint32_t VBCapacity = 0;
	uint32_t IBCapacity = 0;
};

struct RhiRendererImpl
{
	rhi::Device* Device = nullptr;

	rhi::ShaderHandle RectVertexShader;
	rhi::ShaderHandle RectFragmentShader;
	rhi::PipelineHandle RectPipeline;
	bool RectPipelineInit = false;

	rhi::ShaderHandle TextVertexShader;
	rhi::ShaderHandle TextFragmentShader;
	rhi::PipelineHandle TextPipeline;
	bool TextPipelineInit = false;

	rhi::ShaderHandle RoundedBoxVertexShader;
	rhi::ShaderHandle RoundedBoxFragmentShader;
	rhi::PipelineHandle RoundedBoxPipeline;
	bool RoundedBoxPipelineInit = false;

	rhi::ShaderHandle ImageVertexShader;
	rhi::ShaderHandle ImageFragmentShader;
	rhi::PipelineHandle ImagePipeline;
	bool ImagePipelineInit = false;

	// Atlas texture (nearest-sampled font atlas, single level - rebuilt when
	// the font cache dirties it).
	rhi::TextureHandle Atlas;
	uint32_t AtlasWidth = 0;
	uint32_t AtlasHeight = 0;
	bool AtlasCreated = false;

	struct WispTexture
	{
		rhi::TextureHandle Handle;
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
// RenderBatches). One per WindowContext; the shared RhiRendererImpl holds
// everything windows reuse. The staging buffers are per-frame-slot: with
// only two frames in flight, frame N+1's fence wait only covers frame N-1,
// so a single shared pair was being overwritten while frame N's GPU read of
// it could still be in flight (a HOST-write -> GPU-read race that tears the
// UI geometry during interaction).
struct RhiWindowRenderState : WindowRenderState
{
	// The device its staging was made on (null once the renderer has shut
	// down): the buffers go back to the retire queue with the state.
	rhi::Device*            Device = nullptr;
	StagingBuffers          Staging[2];
	uint32_t                ViewportW = 1920;
	uint32_t                ViewportH = 1080;

	~RhiWindowRenderState() override
	{
		if (!Device)
			return;
		// DestroyBuffer queues them into the retire queue (no device stall).
		for (StagingBuffers& s : Staging)
		{
			if (rhi::IsValid(s.VB))
				Device->DestroyBuffer(s.VB);
			if (rhi::IsValid(s.IB))
				Device->DestroyBuffer(s.IB);
			s = {};
		}
	}
};

// ---- RhiRenderer ----------------------------------------------------------

RhiRenderer::RhiRenderer() : m_Impl(std::make_unique<RhiRendererImpl>()) {}
RhiRenderer::~RhiRenderer() { Shutdown(); }

std::unique_ptr<WindowRenderState> RhiRenderer::CreateWindowState()
{
	auto state = std::make_unique<RhiWindowRenderState>();
	state->Device = m_Impl->Device;
	return state;
}

void RhiRenderer::SetWindowViewport(WindowRenderState& state, uint32_t width, uint32_t height)
{
	auto& window = static_cast<RhiWindowRenderState&>(state);
	window.ViewportW = width;
	window.ViewportH = height;
}

static void EnsureStagingBuffers(RhiRendererImpl& impl, StagingBuffers& s, uint32_t vbBytes, uint32_t ibBytes)
{
	rhi::Device& device = *impl.Device;

	if (vbBytes > 0 && (!rhi::IsValid(s.VB) || s.VBCapacity < vbBytes))
	{
		if (rhi::IsValid(s.VB))
			device.DestroyBuffer(s.VB);
		rhi::BufferDesc desc;
		desc.Size = vbBytes;
		desc.HostVisible = true;
		desc.Usage = rhi::BufferUsage::Vertex | rhi::BufferUsage::Index | rhi::BufferUsage::TransferDst; // the old fixed flags
		s.VB = device.CreateBuffer(desc, "Wisp UI Vertex Staging");
		s.VBCapacity = vbBytes;
	}

	if (ibBytes > 0 && (!rhi::IsValid(s.IB) || s.IBCapacity < ibBytes))
	{
		if (rhi::IsValid(s.IB))
			device.DestroyBuffer(s.IB);
		rhi::BufferDesc desc;
		desc.Size = ibBytes;
		desc.HostVisible = true;
		desc.Usage = rhi::BufferUsage::Vertex | rhi::BufferUsage::Index | rhi::BufferUsage::TransferDst; // the old fixed flags
		s.IB = device.CreateBuffer(desc, "Wisp UI Index Staging");
		s.IBCapacity = ibBytes;
	}
}

static void CreateAtlasTexture(RhiRendererImpl& impl)
{
	const FontCache* fc = impl.FontCachePtr;
	if (!fc || !fc->GetAtlasData()) return;

	rhi::Device& device = *impl.Device;
	uint32_t w = fc->GetAtlasWidth();
	uint32_t h = fc->GetAtlasHeight();

	// The old atlas retires through the queue (no device stall - the
	// stamp keeps it alive until the referencing frames complete); the new
	// atlas gets a fresh bindless slot + a nearest-sampled single-level
	// upload (the fixed sampler's maxLod-0 clamp reproduces Wisp's old
	// NEAREST + maxLod 1.0 + mipLevels 1 exactly).
	if (rhi::IsValid(impl.Atlas))
	{
		device.DestroyTexture(impl.Atlas);
		impl.Atlas = {};
	}

	rhi::TextureDesc desc;
	desc.Width = w;
	desc.Height = h;
	desc.MipLevels = 1;
	desc.Sampler = rhi::SamplerKind::Nearest;
	impl.Atlas = device.CreateTexture(desc, fc->GetAtlasData(), (uint64_t)w * h * 4, "Wisp Font Atlas");
	impl.AtlasWidth = w;
	impl.AtlasHeight = h;
	impl.AtlasCreated = true;
}

static const rhi::PipelineStateDesc::Attribute* WispVertexAttributes()
{
	static const rhi::PipelineStateDesc::Attribute attributes[] = {
		{ 0, rhi::Format::R32G32B32_SFLOAT, offsetof(BatchedVertex, pos) },
		{ 1, rhi::Format::R32G32_SFLOAT, offsetof(BatchedVertex, uv) },
		{ 2, rhi::Format::R8G8B8A8_UNORM, offsetof(BatchedVertex, col) },
	};
	return attributes;
}

bool RhiRenderer::Init(rhi::Device& device_, const FontCache* fontCache)
{
	RhiRendererImpl& impl = *m_Impl;
	impl.Device = &device_;
	impl.FontCachePtr = fontCache;

	rhi::Device& device = *impl.Device;
	// Wisp draws only into the SWAPCHAIN, whose colour format is the fixed
	// B8G8R8A8_UNORM - the pipelines declare it directly (no raw
	// swapchain-info reach into Wisp). Framebuffer targets' formats are
	// the renderers' concern (they take them from the target); the UI
	// samples render targets through WrapImage.
	const rhi::Format kUiColourFormat = rhi::Format::B8G8R8A8_UNORM;

	// --- Rect pipeline ---
	{
		const std::span<const uint32_t> vertSpv = shaders::ui_rect_vert;
		const std::span<const uint32_t> fragSpv = shaders::ui_rect_frag;
		impl.RectVertexShader = device.CreateShader(vertSpv, rhi::ShaderStage::Vertex, "ui_rect.vert");
		impl.RectFragmentShader = device.CreateShader(fragSpv, rhi::ShaderStage::Fragment, "ui_rect.frag");

		rhi::PipelineStateDesc spec;
		spec.VertexShader = impl.RectVertexShader;
		spec.FragmentShader = impl.RectFragmentShader;
		spec.VertexStride = sizeof(BatchedVertex);
		spec.VertexAttributes = std::span(WispVertexAttributes(), 3);
		spec.PushConstantSize = sizeof(glm::mat4);
		spec.ColorAttachmentFormats[0] = kUiColourFormat;
		spec.EnableBlend = true;
		spec.DepthTestEnable = false;
		spec.DepthWriteEnable = false;
		spec.DepthAttachmentFormat = rhi::Format::D32_SFLOAT;
		impl.RectPipeline = device.CreatePipeline(spec, "Wisp Rect Pipeline");
		impl.RectPipelineInit = true;
	}

	// --- Text pipeline (atlas-textured quads) ---
	{
		const std::span<const uint32_t> vertSpv = shaders::ui_text_vert;
		const std::span<const uint32_t> fragSpv = shaders::ui_text_frag;
		impl.TextVertexShader = device.CreateShader(vertSpv, rhi::ShaderStage::Vertex, "ui_text.vert");
		impl.TextFragmentShader = device.CreateShader(fragSpv, rhi::ShaderStage::Fragment, "ui_text.frag");

		rhi::PipelineStateDesc spec;
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
		spec.DepthAttachmentFormat = rhi::Format::D32_SFLOAT;
		impl.TextPipeline = device.CreatePipeline(spec, "Wisp Text Pipeline");
		impl.TextPipelineInit = true;
	}

	// --- RoundedBox pipeline ---
	{
		const std::span<const uint32_t> vertSpv = shaders::ui_roundedbox_vert;
		const std::span<const uint32_t> fragSpv = shaders::ui_roundedbox_frag;
		impl.RoundedBoxVertexShader = device.CreateShader(vertSpv, rhi::ShaderStage::Vertex, "ui_roundedbox.vert");
		impl.RoundedBoxFragmentShader = device.CreateShader(fragSpv, rhi::ShaderStage::Fragment, "ui_roundedbox.frag");

		rhi::PipelineStateDesc spec;
		spec.VertexShader = impl.RoundedBoxVertexShader;
		spec.FragmentShader = impl.RoundedBoxFragmentShader;
		spec.VertexStride = sizeof(BatchedVertex);
		spec.VertexAttributes = std::span(WispVertexAttributes(), 3);
		spec.PushConstantSize = sizeof(RoundedBoxPushConstants);
		spec.ColorAttachmentFormats[0] = kUiColourFormat;
		spec.EnableBlend = true;
		spec.DepthTestEnable = false;
		spec.DepthWriteEnable = false;
		spec.DepthAttachmentFormat = rhi::Format::D32_SFLOAT;
		impl.RoundedBoxPipeline = device.CreatePipeline(spec, "Wisp RoundedBox Pipeline");
		impl.RoundedBoxPipelineInit = true;
	}

	// --- Image pipeline (per-texture sampled quads) ---
	{
		const std::span<const uint32_t> vertSpv = shaders::ui_image_vert;
		const std::span<const uint32_t> fragSpv = shaders::ui_image_frag;
		impl.ImageVertexShader = device.CreateShader(vertSpv, rhi::ShaderStage::Vertex, "ui_image.vert");
		impl.ImageFragmentShader = device.CreateShader(fragSpv, rhi::ShaderStage::Fragment, "ui_image.frag");

		rhi::PipelineStateDesc spec;
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
		spec.DepthAttachmentFormat = rhi::Format::D32_SFLOAT;
		impl.ImagePipeline = device.CreatePipeline(spec, "Wisp Image Pipeline");
		impl.ImagePipelineInit = true;
	}

	CreateAtlasTexture(impl);

	return true;
}

void RhiRenderer::Shutdown()
{
	RhiRendererImpl& impl = *m_Impl;
	if (!impl.Device) return;

	rhi::Device& device = *impl.Device;

	if (rhi::IsValid(impl.Atlas))
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
	if (rhi::IsValid(impl.TextFragmentShader)) device.DestroyShader(impl.TextFragmentShader);
	if (rhi::IsValid(impl.TextVertexShader)) device.DestroyShader(impl.TextVertexShader);
	if (rhi::IsValid(impl.RectFragmentShader)) device.DestroyShader(impl.RectFragmentShader);
	if (rhi::IsValid(impl.RectVertexShader)) device.DestroyShader(impl.RectVertexShader);
	if (rhi::IsValid(impl.RoundedBoxFragmentShader)) device.DestroyShader(impl.RoundedBoxFragmentShader);
	if (rhi::IsValid(impl.RoundedBoxVertexShader)) device.DestroyShader(impl.RoundedBoxVertexShader);
	if (rhi::IsValid(impl.ImageFragmentShader)) device.DestroyShader(impl.ImageFragmentShader);
	if (rhi::IsValid(impl.ImageVertexShader)) device.DestroyShader(impl.ImageVertexShader);

	impl.Device = nullptr;
}

uint64_t RhiRenderer::CreateTexture(uint32_t width, uint32_t height, const uint8_t* rgbaData)
{
	RhiRendererImpl& impl = *m_Impl;
	if (!impl.Device || !rgbaData || width == 0 || height == 0)
		return 0;

	rhi::Device& device = *impl.Device;

	rhi::TextureDesc desc;
	desc.Width = width;
	desc.Height = height;
	desc.MipLevels = 1;
	desc.Sampler = rhi::SamplerKind::Linear;

	RhiRendererImpl::WispTexture tex;
	tex.Handle = device.CreateTexture(desc, rgbaData, (uint64_t)width * height * 4, "Wisp UI Texture");
	tex.Width = width;
	tex.Height = height;

	uint64_t id = impl.NextTextureId++;
	impl.Textures[id] = tex;
	return id;
}

void RhiRenderer::UpdateTexture(uint64_t handle, uint32_t x, uint32_t y, uint32_t width, uint32_t height, const uint8_t* rgbaData)
{
	RhiRendererImpl& impl = *m_Impl;
	auto it = impl.Textures.find(handle);
	if (it == impl.Textures.end() || !rgbaData)
		return;
	// Full-image replacement only (sufficient for the current consumers);
	// partial rectangle updates would need a region-sized staging copy.
	if (x != 0 || y != 0 || width != it->second.Width || height != it->second.Height)
		return;
	impl.Device->SetTextureData(it->second.Handle, rgbaData, (uint64_t)width * height * 4);
}

void RhiRenderer::DestroyTexture(uint64_t handle)
{
	RhiRendererImpl& impl = *m_Impl;
	auto it = impl.Textures.find(handle);
	if (it == impl.Textures.end())
		return;
	// Deferred: the retire queue keeps the image (or wrapped view)
	// alive until the referencing frames complete (no device stall).
	impl.Device->DestroyTexture(it->second.Handle);
	impl.Textures.erase(it);
}

// The image is not owned: DestroyTexture frees only the wrapper's view and
// bindless slot. The view's format is the caller's (a BGRA8 render target
// and an RGBA8 texture each display correctly).
uint64_t RhiRenderer::WrapImage(void* nativeImage, uint32_t width, uint32_t height, rhi::Format format)
{
	RhiRendererImpl& impl = *m_Impl;
	if (!impl.Device || !nativeImage || width == 0 || height == 0)
		return 0;

	RhiRendererImpl::WispTexture tex;
	tex.Handle = impl.Device->CreateTextureFromNativeImage(nativeImage, width, height, format);
	tex.Width = width;
	tex.Height = height;

	uint64_t id = impl.NextTextureId++;
	impl.Textures[id] = tex;
	return id;
}

void RhiRenderer::PrepareFrame(const BatchedDrawData& data, WindowRenderState& windowState, uint64_t frameIndex)
{
	auto& state = static_cast<RhiWindowRenderState&>(windowState);
	RhiRendererImpl& impl = *m_Impl;
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

void RhiRenderer::RenderBatches(const BatchedDrawData& data, const glm::mat4& orthoProjection, WindowRenderState& windowState, hearth::rhi::CommandList& commandList)
{
	auto& state = static_cast<RhiWindowRenderState&>(windowState);
	RhiRendererImpl& impl = *m_Impl;
	if (!impl.Device || data.Vertices.empty() || !impl.RectPipelineInit)
		return;

	const uint32_t frameIndex = commandList.FrameSlot();

	rhi::Device& device = *impl.Device;

	// Upload staging - per-frame-slot pair (see RhiWindowRenderState):
	// the slot this window is recording right now was fence-waited by
	// AcquireNextImage, so overwriting it cannot race any in-flight GPU
	// read (frame N+1 waits frame N-1's fence, so slots alternate and
	// each slot is safe to write when its turn comes around again).
	// The per-window pick is the frame slot % 2 (a window's slot base is
	// even, so it contributes nothing).
	// The buffers were grown by PrepareFrame on the loop thread: recording
	// never creates device objects.
	StagingBuffers& staging = state.Staging[frameIndex % 2];
	uint32_t vbBytes = (uint32_t)(data.Vertices.size() * sizeof(BatchedVertex));
	uint32_t ibBytes = (uint32_t)(data.Indices.size() * sizeof(uint32_t));
	HEARTH_ASSERT(rhi::IsValid(staging.VB) && staging.VBCapacity >= vbBytes,
		"RenderBatches ran without PrepareFrame covering this frame's vertex batch!");
	HEARTH_ASSERT(rhi::IsValid(staging.IB) && staging.IBCapacity >= ibBytes,
		"RenderBatches ran without PrepareFrame covering this frame's index batch!");
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

	const uint32_t atlasIndex = rhi::IsValid(impl.Atlas) ? device.GetBindlessIndex(impl.Atlas) : 0;

	// One timed GPU scope for the whole UI pass, plus a label per batch
	// naming its pipeline (the rounded-box SDFs, the glyph atlas text, the
	// sampled images) for RenderDoc/Nsight. The batch labels are untimed
	// unless the batches ask for it (UIApplication::SetGpuTimeBatches): a
	// full-width panel draws hundreds of batches, whose timestamp pairs would
	// fill the per-frame pool and leave the program's own passes untimed.
	commandList.BeginLabel("Wisp UI");
	const bool timeBatches = data.GpuTimeBatches;

	for (const auto& batch : data.Batches)
	{
		if (batch.IndexCount == 0) continue;
		// A pipeline not built yet skips the batch BEFORE its label opens
		// (a skip after it would leave the label stack unbalanced).
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
			const float clipScale = data.DpiScale * data.UiScale;
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
