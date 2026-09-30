#include "Wisp/RendererVulkan.h"
#include "Wisp/FontCache.h"

#include "TinyEngine/RHI/RHI.h"
#include "TinyEngine/Core/Application.h"
#include "TinyEngine/Core/RhiDevice.h"
#include "TinyEngine/Debug/Profiler.h"
#include "assetio/VulkanShaderCompiler.h"

#include <cstring>
#include <glm/gtc/matrix_transform.hpp>

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
#extension GL_EXT_nonuniform_qualifier : require
layout(push_constant) uniform PC { mat4 projection; uint texIndex; } pc;
layout(location = 0) in vec4 vCol;
layout(location = 1) in vec2 vUV;
layout(location = 0) out vec4 outCol;
layout(binding = 0) uniform sampler2D u_Textures[];
void main() { float a = texture(u_Textures[nonuniformEXT(int(pc.texIndex))], vUV).a; outCol = vCol * vec4(1.0, 1.0, 1.0, a); }
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
#extension GL_EXT_nonuniform_qualifier : require
layout(push_constant) uniform PC { mat4 projection; uint texIndex; } pc;
layout(location = 0) in vec4 vCol;
layout(location = 1) in vec2 vUV;
layout(location = 0) out vec4 outCol;
layout(binding = 0) uniform sampler2D u_Textures[];
void main() { outCol = texture(u_Textures[nonuniformEXT(int(pc.texIndex))], vUV) * vCol; }
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

// P-D: the RHI-backed implementation. No raw backend identifiers here or
// anywhere in Wisp (the P-E grep rule); everything goes through rhi::Device
// + handles.
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

// Per-window transient state: staging buffers, the command buffer the
// window is being recorded into, and its viewport. One per WindowContext;
// the shared WispVulkanImpl holds everything windows reuse. The staging
// buffers are per-frame-slot: with only two frames in flight, frame N+1's
// fence wait only covers frame N-1, so a single shared pair was being
// overwritten while frame N's GPU read of it could still be in flight (a
// HOST-write -> GPU-read race that tears the UI geometry during interaction).
struct WispWindowRenderState
{
	StagingBuffers          Staging[2];
	void*                   CommandBuffer = nullptr;
	uint32_t                ViewportW = 1920;
	uint32_t                ViewportH = 1080;
};

// ---- RendererVulkan --------------------------------------------------------

RendererVulkan::RendererVulkan()
	: m_Impl(std::make_unique<WispVulkanImpl>()), m_DefaultState(std::make_unique<WispWindowRenderState>()) {}
RendererVulkan::~RendererVulkan() { Shutdown(); }

void RendererVulkan::SetCommandBuffer(void* commandBuffer)
{
	m_DefaultState->CommandBuffer = commandBuffer;
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

void RendererVulkan::SetWindowCommandBuffer(WispWindowRenderState& state, void* commandBuffer)
{
	state.CommandBuffer = commandBuffer;
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
	// The swapchain + framebuffer colour format is fixed B8G8R8A8_UNORM
	// across the engine, so the pipelines declare it directly (no raw
	// swapchain-info reach into Wisp).
	const TinyEngine::rhi::Format kUiColourFormat = TinyEngine::rhi::Format::B8G8R8A8_UNORM;

	// --- Rect pipeline ---
	{
		std::vector<uint32_t> vertSpv = TinyEngine::VulkanShaderCompiler::Compile(kRectVertexGLSL, TinyEngine::ShaderStage::Vertex, "ui_rect.vert");
		std::vector<uint32_t> fragSpv = TinyEngine::VulkanShaderCompiler::Compile(kRectFragmentGLSL, TinyEngine::ShaderStage::Fragment, "ui_rect.frag");
		impl.RectVertexShader = device.CreateShader(vertSpv, TinyEngine::rhi::ShaderStage::Vertex, "Wisp Rect VS");
		impl.RectFragmentShader = device.CreateShader(fragSpv, TinyEngine::rhi::ShaderStage::Fragment, "Wisp Rect FS");

		TinyEngine::rhi::PipelineStateDesc spec;
		spec.VertexShader = impl.RectVertexShader;
		spec.FragmentShader = impl.RectFragmentShader;
		spec.VertexStride = sizeof(BatchedVertex);
		spec.VertexAttributes = std::span(WispVertexAttributes(), 3);
		spec.PushConstantSize = sizeof(glm::mat4);
		spec.ColorAttachmentFormat = kUiColourFormat;
		spec.EnableBlend = true;
		spec.DepthTestEnable = false;
		spec.DepthWriteEnable = false;
		spec.DepthAttachmentFormat = TinyEngine::rhi::Format::D32_SFLOAT;
		impl.RectPipeline = device.CreatePipeline(spec, "Wisp Rect Pipeline");
		impl.RectPipelineInit = true;
	}

	// --- Text pipeline (atlas-textured quads) ---
	{
		std::vector<uint32_t> vertSpv = TinyEngine::VulkanShaderCompiler::Compile(kTextVertexGLSL, TinyEngine::ShaderStage::Vertex, "ui_text.vert");
		std::vector<uint32_t> fragSpv = TinyEngine::VulkanShaderCompiler::Compile(kTextFragmentGLSL, TinyEngine::ShaderStage::Fragment, "ui_text.frag");
		impl.TextVertexShader = device.CreateShader(vertSpv, TinyEngine::rhi::ShaderStage::Vertex, "Wisp Text VS");
		impl.TextFragmentShader = device.CreateShader(fragSpv, TinyEngine::rhi::ShaderStage::Fragment, "Wisp Text FS");

		TinyEngine::rhi::PipelineStateDesc spec;
		spec.VertexShader = impl.TextVertexShader;
		spec.FragmentShader = impl.TextFragmentShader;
		spec.VertexStride = sizeof(BatchedVertex);
		spec.VertexAttributes = std::span(WispVertexAttributes(), 3);
		spec.PushConstantSize = sizeof(TexturedPushConstants);
		spec.PushConstantStages = 1 | 16; // VERTEX|FRAGMENT - the frag reads the index
		spec.ColorAttachmentFormat = kUiColourFormat;
		spec.EnableBlend = true;
		spec.DepthTestEnable = false;
		spec.DepthWriteEnable = false;
		spec.DepthAttachmentFormat = TinyEngine::rhi::Format::D32_SFLOAT;
		impl.TextPipeline = device.CreatePipeline(spec, "Wisp Text Pipeline");
		impl.TextPipelineInit = true;
	}

	// --- RoundedBox pipeline ---
	{
		std::vector<uint32_t> vertSpv = TinyEngine::VulkanShaderCompiler::Compile(kRoundedBoxVertexGLSL, TinyEngine::ShaderStage::Vertex, "ui_roundedbox.vert");
		std::vector<uint32_t> fragSpv = TinyEngine::VulkanShaderCompiler::Compile(kRoundedBoxFragmentGLSL, TinyEngine::ShaderStage::Fragment, "ui_roundedbox.frag");
		impl.RoundedBoxVertexShader = device.CreateShader(vertSpv, TinyEngine::rhi::ShaderStage::Vertex, "Wisp RoundedBox VS");
		impl.RoundedBoxFragmentShader = device.CreateShader(fragSpv, TinyEngine::rhi::ShaderStage::Fragment, "Wisp RoundedBox FS");

		TinyEngine::rhi::PipelineStateDesc spec;
		spec.VertexShader = impl.RoundedBoxVertexShader;
		spec.FragmentShader = impl.RoundedBoxFragmentShader;
		spec.VertexStride = sizeof(BatchedVertex);
		spec.VertexAttributes = std::span(WispVertexAttributes(), 3);
		spec.PushConstantSize = sizeof(RoundedBoxPushConstants);
		spec.ColorAttachmentFormat = kUiColourFormat;
		spec.EnableBlend = true;
		spec.DepthTestEnable = false;
		spec.DepthWriteEnable = false;
		spec.DepthAttachmentFormat = TinyEngine::rhi::Format::D32_SFLOAT;
		impl.RoundedBoxPipeline = device.CreatePipeline(spec, "Wisp RoundedBox Pipeline");
		impl.RoundedBoxPipelineInit = true;
	}

	// --- Image pipeline (per-texture sampled quads) ---
	{
		std::vector<uint32_t> vertSpv = TinyEngine::VulkanShaderCompiler::Compile(kImageVertexGLSL, TinyEngine::ShaderStage::Vertex, "ui_image.vert");
		std::vector<uint32_t> fragSpv = TinyEngine::VulkanShaderCompiler::Compile(kImageFragmentGLSL, TinyEngine::ShaderStage::Fragment, "ui_image.frag");
		impl.ImageVertexShader = device.CreateShader(vertSpv, TinyEngine::rhi::ShaderStage::Vertex, "Wisp Image VS");
		impl.ImageFragmentShader = device.CreateShader(fragSpv, TinyEngine::rhi::ShaderStage::Fragment, "Wisp Image FS");

		TinyEngine::rhi::PipelineStateDesc spec;
		spec.VertexShader = impl.ImageVertexShader;
		spec.FragmentShader = impl.ImageFragmentShader;
		spec.VertexStride = sizeof(BatchedVertex);
		spec.VertexAttributes = std::span(WispVertexAttributes(), 3);
		spec.PushConstantSize = sizeof(TexturedPushConstants);
		spec.PushConstantStages = 1 | 16; // VERTEX|FRAGMENT
		spec.ColorAttachmentFormat = kUiColourFormat;
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

void RendererVulkan::RenderBatches(const BatchedDrawData& data, const glm::mat4& orthoProjection)
{
	RenderBatches(data, orthoProjection, *m_DefaultState);
}

void RendererVulkan::RenderBatches(const BatchedDrawData& data, const glm::mat4& orthoProjection, WispWindowRenderState& state)
{
	WispVulkanImpl& impl = *m_Impl;
	if (!impl.Device || !state.CommandBuffer || data.Vertices.empty() || !impl.RectPipelineInit)
		return;

	TinyEngine::rhi::Device& device = *impl.Device;

	// Update atlas if dirty
	const FontCache* fc = impl.FontCachePtr;
	if (fc && fc->IsAtlasDirty())
	{
		CreateAtlasTexture(impl);
		// Clear AFTER the rebuild so this frame's freshly rasterised
		// glyphs are uploaded; a glyph rasterised later re-dirties for
		// next frame. Without this the atlas rebuilt every frame. Only
		// cleared when the rebuild actually produced an atlas (the guard
		// in CreateAtlasTexture can skip it, and a missed rebuild must
		// retry next frame, not be dropped).
		if (impl.AtlasCreated)
			fc->ClearAtlasDirty();
	}

	// Upload staging - per-frame-slot pair (see WispWindowRenderState):
	// the slot this window is recording right now was fence-waited by
	// AcquireNextImage, so overwriting it cannot race any in-flight GPU
	// read (frame N+1 waits frame N-1's fence, so slots alternate and
	// each slot is safe to write when its turn comes around again).
	// The per-window slot is the frame counter % 2 (the window-slot base
	// in the old frame index was always even, so it contributed nothing).
	StagingBuffers& staging = state.Staging[TinyEngine::Profiler::Get().CurrentFrameIndex() % 2];
	uint32_t vbBytes = (uint32_t)(data.Vertices.size() * sizeof(BatchedVertex));
	uint32_t ibBytes = (uint32_t)(data.Indices.size() * sizeof(uint32_t));
	EnsureStagingBuffers(impl, staging, vbBytes, ibBytes);
	if (vbBytes) device.SetBufferData(staging.VB, data.Vertices.data(), vbBytes);
	if (ibBytes) device.SetBufferData(staging.IB, data.Indices.data(), ibBytes);

	// P-D: record through an adopted CommandList (the frame loop's live
	// command buffer - no Begin/End).
	std::unique_ptr<TinyEngine::rhi::CommandList> commandList(device.AdoptCommandBuffer(state.CommandBuffer));

	// Viewport (positive-height — sampling pass)
	commandList->SetViewport(0.0f, 0.0f, (float)state.ViewportW, (float)state.ViewportH, 0.0f, 1.0f);

	const uint32_t atlasIndex = TinyEngine::rhi::IsValid(impl.Atlas) ? device.GetBindlessIndex(impl.Atlas) : 0;

	for (const auto& batch : data.Batches)
	{
		if (batch.IndexCount == 0) continue;

		// Apply scissor from clip rect (scale logical->physical)
		if (batch.ClipIndex >= 0 && batch.ClipIndex < (int)data.ClipRects.size())
		{
			const glm::vec4& cr = data.ClipRects[batch.ClipIndex];
			commandList->SetScissor((int32_t)(cr.x * data.DpiScale), (int32_t)(cr.y * data.DpiScale),
				(uint32_t)(cr.z * data.DpiScale), (uint32_t)(cr.w * data.DpiScale));
		}
		else
		{
			commandList->SetScissor(0, 0, state.ViewportW, state.ViewportH);
		}

		if (batch.TextureId == 0)
		{
			commandList->BindPipeline(impl.RectPipeline);
			commandList->BindGlobalSet();
			commandList->PushConstants(1, 0, &orthoProjection, sizeof(glm::mat4));
		}
		else if (batch.TextureId == 2)
		{
			commandList->BindPipeline(impl.RoundedBoxPipeline);
			commandList->BindGlobalSet();

			RoundedBoxPushConstants pc;
			pc.projection = orthoProjection;
			std::memcpy(pc.radii, batch.Sdf.CornerRadii, sizeof(pc.radii));
			pc.outlineWidth = batch.Sdf.OutlineWidth;
			std::memcpy(pc.outlineColour, batch.Sdf.OutlineColour, sizeof(pc.outlineColour));
			pc.halfSize[0] = batch.Sdf.Size[0] * 0.5f;
			pc.halfSize[1] = batch.Sdf.Size[1] * 0.5f;
			pc._pad2[0] = 0; pc._pad2[1] = 0;
			commandList->PushConstants(1, 0, &pc, sizeof(pc));
		}
		else if (batch.TextureId == 1)
		{
			commandList->BindPipeline(impl.TextPipeline);
			commandList->BindGlobalSet();

			TexturedPushConstants pc;
			pc.projection = orthoProjection;
			pc.texIndex = atlasIndex;
			commandList->PushConstants(1 | 16, 0, &pc, sizeof(pc));
		}
		else
		{
			// Sampled image (IUIRenderer::CreateTexture handles).
			auto texIt = impl.Textures.find(batch.TextureId);
			if (texIt == impl.Textures.end() || !impl.ImagePipelineInit)
			{
				// Keep the pixel-correct fallback for a dangling texture id:
				// draw the quad as solid colour through the rect pipeline.
				commandList->BindPipeline(impl.RectPipeline);
				commandList->BindGlobalSet();
				commandList->PushConstants(1, 0, &orthoProjection, sizeof(glm::mat4));
			}
			else
			{
				commandList->BindPipeline(impl.ImagePipeline);
				commandList->BindGlobalSet();

				TexturedPushConstants pc;
				pc.projection = orthoProjection;
				pc.texIndex = device.GetBindlessIndex(texIt->second.Handle);
				commandList->PushConstants(1 | 16, 0, &pc, sizeof(pc));
			}
		}

		commandList->BindVertexBuffer(staging.VB, 0);
		commandList->BindIndexBuffer(staging.IB, 0);
		commandList->DrawIndexed(batch.IndexCount, 1, batch.IndexOffset, 0, 0);
	}
}

} // namespace wisp
