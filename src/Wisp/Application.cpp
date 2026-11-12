#include "Wisp/Application.h"
#include "Wisp/Batcher.h"
#include "Wisp/RendererVulkan.h"
#include "Math/Projection.h" // the one ortho convention
#include "TinyEngine/Core/HotReloadClient.h"
#include "TinyEngine/Asset/TinyAssetLoader.h"
#include "DebugDraw.h"
#include "TinyEngine/Renderer/DebugDrawCommon.h"
#include "TinyEngine/Renderer/RendererDebug.h"
#include "Wisp/Widgets.h"
#include "TinyEngine/Renderer/RenderPassContext.h"
#include "TinyEngine/Renderer/GraphicsContext.h"
#include "TinyEngine/Core/Application.h"
#include "TinyEngine/Core/RhiDevice.h"
#include "TinyEngine/CVar/EngineCVars.h" // for GetWindow()
#include "TinyEngine/Memory/MemorySubsystem.h"
#include "TinyEngine/Core/Input.h"
#include <GLFW/glfw3.h>
#include <glm/gtc/matrix_transform.hpp>
#include <algorithm>
#include <chrono>
#include <unordered_set>

namespace wisp {

UIApplication* UIApplication::s_Instance = nullptr;

UIApplication::UIApplication()  = default;
UIApplication::~UIApplication() = default;

void UIApplication::Init(FontCache* fontCache)
{
	if (s_Instance) return;
	s_Instance = new UIApplication();
	s_Instance->m_FontCache = fontCache;

	s_Instance->m_Renderer = std::make_unique<RendererVulkan>();
	s_Instance->m_Renderer->Init(fontCache);

	// The shader-reload routing (the engine cannot include Wisp
	// headers, so the backend registers itself here). The hot-reload
	// client itself is stripped from TE_CONF_DIST builds.
#if !defined(TE_CONF_DIST)
	TinyEngine::HotReload::SetShaderReloadCallback([](const std::string& path) {
		return UIApplication::ReloadShader(path);
	});
	TinyEngine::HotReload::SetFontReloadCallback([](const std::string& path) {
		return UIApplication::ReloadFont(path);
	});
#endif // !TE_CONF_DIST

	// The default (main-window) context: created unbound; the first window
	// that reaches the draw callback binds it.
	auto ctx = std::make_unique<WindowContext>();
	ctx->RenderState = s_Instance->m_Renderer->CreateWindowState();
	s_Instance->m_FocusedContext = ctx.get();
	s_Instance->m_Contexts.push_back(std::move(ctx));

	// The hooks are the RhiDevice subsystem's (registered here on the
	// game thread, cleared in Shutdown).
	TinyEngine::FrameDrawHooks& hooks = TinyEngine::Application::Get().GetEngine().Get<TinyEngine::RhiDevice>().GetFrameDrawHooks();

	// T-F-B.3: the update runs on the main thread at the top of
	// SwapBuffers (tick + paint + batch build + staging/atlas
	// pre-creation + the screen-overlay submit); the draw callback
	// below then records only, on a Loom worker.
	hooks.SetWispUpdate([](const TinyEngine::RenderPassContext& context) {
		UIApplication& app = Get();

		WindowContext* ctx = app.GetWindowContext(context.WindowHandle);
		if (!ctx)
		{
			// First frame of the main window: bind the default context.
			ctx = app.GetDefaultContext();
			if (!ctx || ctx->Window != nullptr) return;
			ctx->Window = context.WindowHandle;
		}
		if (!ctx->Root) return;

		app.TickAnimation(1.0f / 60.0f);

		uint32_t w = context.Width;
		uint32_t h = context.Height;

		// The widget tree arranges in the LOGICAL space (the physical
		// size divided by the scale); the renderer's ortho then scales
		// the result back onto the full window.
		const float uiScale = std::max(0.25f, TinyEngine::cvars::ui_scale.Get());
		app.Tick(*ctx, (uint32_t)((float)w / uiScale), (uint32_t)((float)h / uiScale));

		// Per-window mouse scale (logical -> physical) and content scale.
		GLFWwindow* win = (GLFWwindow*)context.WindowHandle;
		int logicalW = (int)context.Width, logicalH = (int)context.Height;
		float csx = 1.0f, csy = 1.0f;
		// The Offscreen mode has no GLFW window - the virtual size (the
		// render context's) stands in for the window size and the
		// content scale stays 1.
		if (win)
		{
			glfwGetWindowSize(win, &logicalW, &logicalH);
			glfwGetWindowContentScale(win, &csx, &csy);
		}
		ctx->MouseScale = { logicalW > 0 ? (float)w / logicalW : 1.0f,
		                    logicalH > 0 ? (float)h / logicalH : 1.0f };
		ctx->ContentScale = csx;

		// The batch output builds into the engine's frame arena
		// (slot = frame index % 2); the slot is reset before the build.
		TinyEngine::FrameAllocator* arena = nullptr;
		if (TinyEngine::Application::TryGet())
		{
			TinyEngine::Engine& engine = TinyEngine::Application::Get().GetEngine();
			if (TinyEngine::MemorySubsystem* memory = engine.TryGet<TinyEngine::MemorySubsystem>())
				arena = &memory->GetFrameAllocator();
		}
		BatchedDrawData& lastBatches = ctx->LastBatches[context.FrameIndex % 2];
		if (arena)
			arena->ResetSlot(context.FrameIndex);
		lastBatches = BatchDrawList(ctx->DrawList, arena, context.FrameIndex);
		if (!lastBatches.Vertices.empty())
		{
			lastBatches.DpiScale = 1.0f; // physical coords, no DPI scaling needed
			// T-F-C: the staging slot pick keys on the frame index - the
			// update (extract) and the record must agree (passed explicitly,
			// never stored on the shared state).
			app.m_Renderer->EnsureFrameResources(lastBatches, *ctx->RenderState, context.FrameIndex);
		}

		// Debug-draw SCREEN sub-batch: build + submit this window's screen
		// primitives (focus overlay, reflector highlights) - recorded by
		// the engine after ALL UI.
		{
			// The per-frame scratch is an app member, not a function-local
			// static (no process-global mutable state) - the submit copies the
			// vertices, so only the capacity carries over. Game thread (every
			// window's update runs here in turn).
			std::vector<TinyEngine::DebugDrawVertex>& screenVerts = app.m_ScreenOverlayScratch;
			screenVerts.clear();
			debug_draw::BuildScreen({ (float)logicalW, (float)logicalH }, screenVerts);
			TinyEngine::DebugDrawVertices geometry;
			geometry.Vertices = screenVerts.data();
			geometry.VertexCount = static_cast<uint32_t>(screenVerts.size());
			TinyEngine::RendererDebug::SubmitDebugScreenOverlay(geometry, { (float)logicalW, (float)logicalH }, context.FrameIndex);
		}
	});

	// Register draw callback — Wisp handles its own rendering, per window.
	// The context is backend-neutral: the command buffer travels as an
	// opaque handle into RendererVulkan (the backend), the window handle
	// resolves the per-window context. Record-only: the update above owns
	// the tick/paint.
	hooks.SetWispDraw([](const TinyEngine::RenderPassContext& context) {
		UIApplication& app = Get();

		WindowContext* ctx = app.GetWindowContext(context.WindowHandle);
		if (!ctx || !ctx->Root) return;

		uint32_t w = context.Width;
		uint32_t h = context.Height;

		// The logical layout space shrinks by the scale; the ortho maps
		// the logical rects back onto the full physical window - the
		// whole surface (text + widgets + spacing) scales uniformly at
		// this single boundary.
		const float uiScale = std::max(0.25f, TinyEngine::cvars::ui_scale.Get());
		const float logicalW = (float)w / uiScale;
		const float logicalH = (float)h / uiScale;

		const BatchedDrawData& batched = ctx->LastBatches[context.FrameIndex % 2];
		if (!batched.Vertices.empty())
		{
			glm::mat4 ortho = TinyEngine::Math::Orthographic(0.0f, logicalW, 0.0f, logicalH, -1.0f, 1.0f);

			// T-F-C: the staging slot pick keys on the frame index (the
			// record fills it from its pass's command-list slot).
			app.m_Renderer->SetWindowCommandBuffer(*ctx->RenderState, context.CommandBuffer);
			app.m_Renderer->SetWindowViewport(*ctx->RenderState, w, h);
			app.m_Renderer->RenderBatches(batched, ortho, *ctx->RenderState, context.FrameIndex);
		}
	});
}

bool UIApplication::ReloadShader(const std::string& assetPath)
{
	if (!s_Instance || !s_Instance->m_Renderer)
		return false;
	return s_Instance->m_Renderer->ReloadShader(assetPath);
}

bool UIApplication::ReloadFont(const std::string& assetPath)
{
	if (!s_Instance || !s_Instance->m_FontCache)
		return false;

	std::vector<uint8_t> bytes = TinyEngine::TinyAssetLoader::LoadFont(assetPath);
	if (bytes.empty())
	{
		TE_CORE_WARN(TinyEngine::LogCore, "Wisp: font reload failed - keeping the current font.");
		return true;
	}
	return s_Instance->m_FontCache->ReloadFontByPath(assetPath, bytes.data(), bytes.size());
}

void UIApplication::Shutdown()
{
	if (!s_Instance) return;
	{
		TinyEngine::FrameDrawHooks& hooks = TinyEngine::Application::Get().GetEngine().Get<TinyEngine::RhiDevice>().GetFrameDrawHooks();
		hooks.SetWispUpdate(nullptr);
		hooks.SetWispDraw(nullptr);
	}

	// Per-window render state and roots must die before the renderer (they
	// hold device resources), and the renderer before device shutdown.
	for (auto& ctx : s_Instance->m_Contexts)
	{
		ctx->Root.reset();
		ctx->PopupRoot.reset();
		if (ctx->RenderState)
		{
			s_Instance->m_Renderer->DestroyWindowState(ctx->RenderState);
			ctx->RenderState = nullptr;
		}
	}
	s_Instance->m_Renderer.reset();
	s_Instance->m_FocusedContext = nullptr;
	s_Instance->m_Contexts.clear();

	delete s_Instance;
	s_Instance = nullptr;
}

// ---- Window contexts --------------------------------------------------------

WindowContext* UIApplication::GetWindowContext(void* window) const
{
	for (auto& ctx : m_Contexts)
		if (ctx->Window == window)
			return ctx.get();
	return nullptr;
}

WindowContext* UIApplication::GetFocusedContext() const
{
	if (m_FocusedContext) return m_FocusedContext;
	return m_Contexts.empty() ? nullptr : m_Contexts[0].get();
}

WindowContext* UIApplication::CreateWindowContext(void* window, std::unique_ptr<Widget> root, float contentScale)
{
	// The push_back can
	// reallocate the list every window's draw callback walks on a record
	// worker (GetWindowContext) - park every window's record first.
	TinyEngine::GraphicsContext::WaitForAllWindowsRecordIdle();
	auto ctx = std::make_unique<WindowContext>();
	ctx->Window = window;
	ctx->Root = std::move(root);
	ctx->ContentScale = contentScale;
	ctx->RenderState = m_Renderer->CreateWindowState();
	WindowContext* raw = ctx.get();
	m_Contexts.push_back(std::move(ctx));
	return raw;
}

bool UIApplication::DestroyWindowContext(void* window)
{
	// The erase, and the dying
	// context's RenderState delete below, run under the same park - the
	// dying window's own in-flight record may still be inside its draw
	// callback (the OS window and its submit thread die after this call).
	TinyEngine::GraphicsContext::WaitForAllWindowsRecordIdle();
	for (auto it = m_Contexts.begin(); it != m_Contexts.end(); ++it)
	{
		if ((*it)->Window != window) continue;
		WindowContext* ctx = it->get();

		// Clear anything the input system still references for this window
		// before its widgets die (same discipline as chrome rebuilds).
		ResetTransientInputState(ctx);
		ClearFocusIfInside(ctx->Root.get());
		ClearFocusIfInside(ctx->PopupRoot.get());
		if (m_DragContext == ctx)
		{
			// A drag cannot survive its source window closing - cancel it.
			if (m_DragOp) FinishDrag(ctx->MousePos, true);
			m_DragContext = nullptr;
		}
		else if (m_DragOp)
		{
			// The dying window may be the drag's current TARGET: the hit
			// path may reference its widgets. Drop the path and re-point
			// the routing at the source; the next RouteDrag re-resolves.
			m_DragOp->CurrentPath.clear();
			m_DragOp->CurrentTarget = nullptr;
			if (m_DragTargetContext == ctx)
				m_DragTargetContext = m_DragContext;
		}
		ctx->Root.reset();
		ctx->PopupRoot.reset();
		ctx->PopupContentCount = 0;
		ctx->LastHoverPath.clear();
		ctx->HoverPath.clear();
		ctx->CaptureWidget = nullptr;
		ctx->TooltipTarget = nullptr;
		ctx->TooltipShown = false;
		if (ctx->RenderState)
		{
			m_Renderer->DestroyWindowState(ctx->RenderState);
			ctx->RenderState = nullptr;
		}

		if (m_FocusedContext == ctx)
		{
			m_FocusedContext = (m_Contexts.size() > 1) ? m_Contexts[0].get() : nullptr;
			if (m_FocusedContext && !m_FocusedContext->LastActivePanelLayer.empty())
				SwitchToLayer(m_FocusedContext->LastActivePanelLayer, FocusEntryPolicy::RestorePrevious);
		}

		m_Contexts.erase(it);
		return true;
	}
	return false;
}

WindowContext* UIApplication::GetContextContaining(Widget* w) const
{
	for (auto& ctx : m_Contexts)
	{
		if (!ctx->Root) continue;
		bool found = false;
		std::function<void(Widget*)> walk = [&](Widget* node) {
			if (!node || found) return;
			if (node == w) { found = true; return; }
			node->ForEachChild([&](Widget* c) { walk(c); });
		};
		walk(ctx->Root.get());
		if (found) return ctx.get();
	}
	return nullptr;
}

void UIApplication::ForEachContext(std::function<void(WindowContext&)> fn)
{
	for (auto& ctx : m_Contexts)
		fn(*ctx);
}

void UIApplication::SetPickMode(PickMode m)
{
	m_PickMode = m;
	for (auto& ctx : m_Contexts)
		ctx->PickedWidget = nullptr;
	m_PickedWidget = nullptr;
}

uint64_t UIApplication::CreateTexture(uint32_t width, uint32_t height, const uint8_t* rgbaData)
{
	return m_Renderer ? m_Renderer->CreateTexture(width, height, rgbaData) : 0;
}

uint64_t UIApplication::WrapUIImageHandle(const TinyEngine::UIImageHandle& handle)
{
	return m_Renderer ? m_Renderer->WrapUIImageHandle(handle) : 0;
}

void UIApplication::UpdateTexture(uint64_t handle, uint32_t x, uint32_t y, uint32_t width, uint32_t height, const uint8_t* rgbaData)
{
	if (m_Renderer)
		m_Renderer->UpdateTexture(handle, x, y, width, height, rgbaData);
}

void UIApplication::DestroyTexture(uint64_t handle)
{
	if (m_Renderer)
		m_Renderer->DestroyTexture(handle);
}

void UIApplication::SetSelectionHighlight(Widget* target, const glm::vec4& color)
{
	m_HighlightTarget = target;
	m_HighlightColor = color;
}

void UIApplication::ClearSelectionHighlight()
{
	m_HighlightTarget = nullptr;
	m_HighlightColor = { 0, 0, 0, 0 };
}

void UIApplication::OnCursorEntered(void* window, bool entered)
{
	if (entered)
	{
		WindowContext* ctx = GetWindowContext(window);
		if (ctx)
			m_ActivePointerContext = ctx;
	}
	else if (GetWindowContext(window) == m_ActivePointerContext)
	{
		m_ActivePointerContext = nullptr;
	}
}

void UIApplication::OnWindowFocusChanged(void* window, bool focused)
{
	WindowContext* ctx = GetWindowContext(window);
	if (focused)
	{
		if (ctx)
		{
			m_FocusedContext = ctx;
			// MRU ordering for cross-window hit-testing.
			ctx->FocusStamp = ++m_FocusCounter;
			// OS focus-in restores this window's last-active panel layer
			// (per-layer memory = "back to where you were in that window").
			if (!ctx->LastActivePanelLayer.empty())
				SwitchToLayer(ctx->LastActivePanelLayer, FocusEntryPolicy::RestorePrevious);
		}
	}
	else if (m_FocusedContext == ctx)
	{
		// Keep the context as the fallback; another focus event (or a close)
		// will re-point it. Navigation meanwhile routes to the last known.
	}
}

void UIApplication::SetRootWidget(std::unique_ptr<Widget> root)
{
	WindowContext* ctx = GetDefaultContext();
	if (!ctx) return;

	// The old root (and every widget in it) dies with this call - drop
	// every input reference into it first, or the next frame's hover diff
	// walks freed widgets.
	ClearFocusIfInside(ctx->Root.get());
	ctx->Root.reset();
	ResetTransientInputState(ctx);
	ctx->HoverPath.clear();
	ctx->LastHoverPath.clear();
	ctx->CaptureWidget = nullptr;
	ctx->TooltipTarget = nullptr;
	ctx->TooltipShown = false;
	ctx->PendingDragWidget = nullptr;
	ctx->PendingDragFactory = nullptr;
	if (m_DragOp)
		FinishDrag(ctx->MousePos, true);

	ctx->Root = std::move(root);
}

Widget* UIApplication::GetRootWidget() const
{
	WindowContext* ctx = GetFocusedContext();
	if (ctx && ctx->Root) return ctx->Root.get();
	for (auto& c : m_Contexts)
		if (c->Root) return c->Root.get();
	return nullptr;
}

// ---- Popups (window-local) ---------------------------------------------------

void UIApplication::ShowPopup(std::unique_ptr<Widget> popup, WindowContext* ctx)
{
	if (!ctx) ctx = GetFocusedContext();
	if (!ctx) return;
	if (!ctx->PopupRoot)
	{
		ctx->PopupRoot = std::make_unique<Overlay>();
		auto catcher = std::make_unique<Border>();
		catcher->SetBrush({ glm::vec4(0,0,0,0) });
		catcher->SetVisibility(Visibility::SelfHitTestInvisible);
		static_cast<Overlay*>(ctx->PopupRoot.get())->AddChild(std::move(catcher));
	}
	auto canvas = std::make_unique<Canvas>();
	// The popup canvas spans the whole viewport - hit-test ONLY its
	// arranged slot content (the dialog), never the canvas itself, or a
	// second popup's canvas would shadow the first's content and the
	// dialog's surroundings would swallow the hover (the dropdown
	// submenu freeze). The outside-click dismissal is the explicit
	// modal check in OnMouseButtonDown.
	canvas->SetVisibility(Visibility::SelfHitTestInvisible);
	auto& slot = canvas->AddChild(std::move(popup));
	slot.Anchor = { 0.5f, 0.5f };
	slot.Alignment = { 0.5f, 0.5f };
	static_cast<Overlay*>(ctx->PopupRoot.get())->AddChild(std::move(canvas));
	ctx->PopupContentCount++;
	ctx->ModalPopupCount++;
	// The focus layer follows the MODAL count (a lone non-modal tooltip
	// must not clear the keyboard focus).
	if (ctx->ModalPopupCount == 1) PushFocusLayer("popup", FocusEntryPolicy::Clear);
}

void UIApplication::ShowPopupAt(std::unique_ptr<Widget> popup, const glm::vec2& screenPos, WindowContext* ctx, bool modal)
{
	if (!ctx) ctx = GetFocusedContext();
	if (!ctx) return;
	if (!ctx->PopupRoot)
	{
		ctx->PopupRoot = std::make_unique<Overlay>();
		auto catcher = std::make_unique<Border>();
		catcher->SetBrush({ glm::vec4(0,0,0,0) });
		catcher->SetVisibility(Visibility::SelfHitTestInvisible);
		static_cast<Overlay*>(ctx->PopupRoot.get())->AddChild(std::move(catcher));
	}
	auto canvas = std::make_unique<Canvas>();
	// Content-only hit-testing (see ShowPopup) - the non-modal tooltip
	// paints without ever capturing input.
	canvas->SetVisibility(modal ? Visibility::SelfHitTestInvisible : Visibility::HitTestInvisible);
	auto& slot = canvas->AddChild(std::move(popup));
	slot.Position = screenPos;
	static_cast<Overlay*>(ctx->PopupRoot.get())->AddChild(std::move(canvas));
	ctx->PopupContentCount++;
	if (modal)
	{
		ctx->ModalPopupCount++;
		if (ctx->ModalPopupCount == 1) PushFocusLayer("popup", FocusEntryPolicy::Clear);
	}
}

void UIApplication::DismissPopupsBeyond(int keepCount, WindowContext* ctx)
{
	if (!ctx) ctx = GetFocusedContext();
	if (!ctx || !ctx->PopupRoot) return;
	if (keepCount >= ctx->PopupContentCount) return;
	// Collect the removed subtrees' widgets first - after the truncation
	// the parent chains still reach the (kept) overlay and can no longer
	// distinguish removed from kept.
	Overlay* overlay = static_cast<Overlay*>(ctx->PopupRoot.get());
	std::unordered_set<Widget*> removedSet;
	std::function<void(Widget*)> collect = [&](Widget* w) {
		removedSet.insert(w);
		w->ForEachChild(collect);
	};
	for (size_t i = (size_t)keepCount + 1; i < overlay->GetChildCount(); i++)
		collect(overlay->GetChildAt(i));
	// The catcher is the first overlay child; keep it + the first
	// keepCount content canvases (the dropdown's main popup).
	overlay->TruncateChildren((size_t)keepCount + 1);
	ctx->PopupContentCount = keepCount;
	ctx->ModalPopupCount = std::min(keepCount, ctx->ModalPopupCount);
	// Drop every input reference into the removed subtrees (a pressed
	// submenu leaf captures the mouse; the enter-driven dismissal must
	// not leave it dangling for the next OnMouseButtonUp).
	auto inRemoved = [&](Widget* w) { return w && removedSet.count(w) != 0; };
	ctx->HoverPath.erase(std::remove_if(ctx->HoverPath.begin(), ctx->HoverPath.end(), inRemoved), ctx->HoverPath.end());
	ctx->LastHoverPath.erase(std::remove_if(ctx->LastHoverPath.begin(), ctx->LastHoverPath.end(), inRemoved), ctx->LastHoverPath.end());
	if (inRemoved(ctx->CaptureWidget))
		ctx->CaptureWidget = nullptr;
	if (inRemoved(m_FocusWidget))
		m_FocusWidget = nullptr;
	if (m_DragOp)
	{
		m_DragOp->CurrentPath.erase(std::remove_if(m_DragOp->CurrentPath.begin(), m_DragOp->CurrentPath.end(), inRemoved), m_DragOp->CurrentPath.end());
		if (inRemoved(m_DragOp->CurrentTarget))
			m_DragOp->CurrentTarget = nullptr;
	}
}

void UIApplication::DismissPopups(WindowContext* ctx)
{
	if (!ctx) ctx = GetFocusedContext();
	if (!ctx) return;
	// The focus layer follows the MODAL count (the push above).
	int popupLayers = ctx->ModalPopupCount > 0 ? 1 : 0;
	ctx->PopupRoot.reset();
	ctx->PopupContentCount = 0;
	ctx->ModalPopupCount = 0;
	ctx->TooltipShown = false;
	m_FocusWidget = nullptr;
	// The popup tree is gone - drop every input reference to it. HoverPath
	// is the dangerous one: with a popup open it can hold the popup Canvas,
	// and a same-frame mouse-up after a click-dismiss iterates it.
	ctx->LastHoverPath.clear();
	ctx->HoverPath.clear();
	// A drag in flight may hold a hit path into the popup tree - drop it
	// or the next RouteDrag diffs against freed widgets.
	if (m_DragOp)
	{
		m_DragOp->CurrentPath.clear();
		m_DragOp->CurrentTarget = nullptr;
	}
	for (int i = 0; i < popupLayers; i++)
		PopFocusLayer();
}

bool UIApplication::HasOpenPopups(const WindowContext* ctx) const
{
	if (!ctx) ctx = GetFocusedContext();
	// The MODAL popups only - the tooltips are
	// non-modal (the PopupContentCount includes them) and must not gate
	// the editor's shortcuts (a hovering tooltip blocked Ctrl+Z/Y, the
	// Delete, F2).
	return ctx ? ctx->ModalPopupCount > 0 : false;
}

// ---- Input ------------------------------------------------------------------

void UIApplication::SetMousePosition(const glm::vec2& screenPos)
{
	SetMousePosition(nullptr, screenPos);
}

void UIApplication::SetMousePosition(void* window, const glm::vec2& screenPos)
{
	WindowContext* ctx = window ? GetWindowContext(window) : GetFocusedContext();
	if (!ctx) return;
	// The physical position divides back into the logical layout space
	// the widget tree arranges in.
	const float uiScale = std::max(0.25f, TinyEngine::cvars::ui_scale.Get());
	ctx->MousePos = { screenPos.x * ctx->MouseScale.x / uiScale, screenPos.y * ctx->MouseScale.y / uiScale };
	if (m_DragOp)
	{
		RouteDrag(ctx->MousePos);
		return;
	}
	if (ctx->PendingDragWidget)
	{
		float dx = ctx->MousePos.x - ctx->PendingDragStart.x;
		float dy = ctx->MousePos.y - ctx->PendingDragStart.y;
		float threshold = 4.0f * ctx->MouseScale.x;
		if (dx * dx + dy * dy >= threshold * threshold)
			StartDrag(*ctx, ctx->MousePos);
		else if (ctx->CaptureWidget)
			ctx->CaptureWidget->OnMouseMove(ctx->MousePos);
		return;
	}
	if (ctx->CaptureWidget)
		ctx->CaptureWidget->OnMouseMove(ctx->MousePos);
}

void UIApplication::SetMouseScale(void* window, float sx, float sy)
{
	WindowContext* ctx = window ? GetWindowContext(window) : GetFocusedContext();
	if (ctx) ctx->MouseScale = { sx, sy };
}

glm::vec2 UIApplication::GetMousePos() const
{
	WindowContext* ctx = GetFocusedContext();
	return ctx ? ctx->MousePos : glm::vec2(-1, -1);
}

void UIApplication::SyncMousePositions()
{
	for (auto& ctx : m_Contexts)
	{
		if (!ctx->Window) continue;
		// Camera-look fix: use the warp-immune event position (Input's
		// cursor callback caches GLFW's virtual position) rather than
		// polling the REAL pointer - while the cursor is disabled for
		// camera look, the real pointer teleports between the user's
		// position and GLFW's warp target, which made the Wisp hover path
		// (and anything keyed off it) flicker every frame.
		glm::vec2 pos = TinyEngine::Input::GetMousePosition();
		// The same logical-space conversion the event path applies - this
		// per-frame refresh overwrites the event position, so it must
		// divide too or the hover/click coordinates land in the physical
		// space while the layout is the logical (the 1.5-scale
		// "everything's off" bug).
		const float uiScale = std::max(0.25f, TinyEngine::cvars::ui_scale.Get());
		ctx->MousePos = { pos.x * ctx->MouseScale.x / uiScale, pos.y * ctx->MouseScale.y / uiScale };
		if (m_DragOp && m_DragContext == ctx.get())
			RouteDrag(ctx->MousePos);
		else if (ctx->PendingDragWidget)
		{
			float dx = ctx->MousePos.x - ctx->PendingDragStart.x;
			float dy = ctx->MousePos.y - ctx->PendingDragStart.y;
			float threshold = 4.0f * ctx->MouseScale.x;
			if (dx * dx + dy * dy >= threshold * threshold)
				StartDrag(*ctx, ctx->MousePos);
		}
	}
}

void UIApplication::TickAnimation(float dt)
{
	m_AnimTime += dt;

	// Tick per-widget animations using double-buffered sets, per context:
	// widgets re-register themselves during TickAnimation if still playing.
	for (auto& ctx : m_Contexts)
	{
		std::swap(ctx->AnimatingWidgets, ctx->NextAnimatingWidgets);
		ctx->NextAnimatingWidgets.clear();
		for (Widget* w : ctx->AnimatingWidgets)
			w->TickAnimation(dt);
		ctx->AnimatingWidgets.clear();
	}
}

void UIApplication::RequestAnimationTick(Widget* w)
{
	WindowContext* ctx = GetContextContaining(w);
	if (ctx) ctx->NextAnimatingWidgets.insert(w);
}

void UIApplication::CancelAnimationTick(Widget* w)
{
	for (auto& ctx : m_Contexts)
	{
		ctx->AnimatingWidgets.erase(w);
		ctx->NextAnimatingWidgets.erase(w);
	}
}

// Widget animation registration — out-of-line in Application.cpp since Widget
// needs to call UIApplication which isn't visible from Core.h
void Widget::RequestAnimationTick()
{
	if (UIApplication::IsValid())
		UIApplication::Get().RequestAnimationTick(this);
}

void Widget::CancelAnimationTick()
{
	if (UIApplication::IsValid())
		UIApplication::Get().CancelAnimationTick(this);
}

bool UIApplication::OnMouseButtonDown(int button) { return OnMouseButtonDown(nullptr, button); }
bool UIApplication::OnMouseButtonUp(int button)   { return OnMouseButtonUp(nullptr, button); }
void UIApplication::OnMouseWheel(float delta)      { OnMouseWheel(nullptr, delta); }
bool UIApplication::OnKeyDown(int key, int mods)   { return OnKeyDown(nullptr, key, mods); }
bool UIApplication::OnKeyUp(int key, int mods)     { return OnKeyUp(nullptr, key, mods); }
bool UIApplication::OnTextInput(uint32_t cp)       { return OnTextInput(nullptr, cp); }

bool UIApplication::OnMouseButtonDown(void* window, int button)
{
	if (button < 0 || button >= 8) return false;
	WindowContext* ctx = window ? GetWindowContext(window) : GetFocusedContext();
	if (!ctx) return false;
	ctx->MouseButtonDown[button] = true;

	// Screen pick (position-independent capture, e.g. the eyedropper):
	// consumes the click and fires, popups and all. Runs before the
	// widget-pick / popup-dismiss paths so an open popup neither eats the
	// click nor closes.
	if (m_ScreenPickCallback)
	{
		auto cb = std::move(m_ScreenPickCallback);
		m_ScreenPickCallback = nullptr;
		cb();
		return true;
	}

	// Widget picking: consume the click and report the picked widget. The
	// candidate is THIS window's (the click lands wherever the cursor is,
	// cross-window included - the reflector works from any window).
	// Clicks on the pick-exclusion subtree (e.g. the reflector itself)
	// pass through so its own controls stay clickable.
	if (m_PickMode != PickMode::None)
	{
		Widget* candidate = ctx->PickedWidget;
		bool excluded = false;
		if (m_PickExclusion && candidate)
		{
			for (Widget* w = candidate; w; w = w->GetParent())
				if (w == m_PickExclusion) { excluded = true; break; }
		}
		if (!excluded)
		{
			auto cb = std::move(m_PickCallback);
			m_PickCallback = nullptr;
			Widget* picked = candidate;
			m_PickMode = PickMode::None;
			for (auto& c : m_Contexts)
				c->PickedWidget = nullptr;
			m_PickedWidget = nullptr;
			if (cb && picked)
				cb(picked);
			FlushPendingPopupDismiss();
			return true;
		}
	}

	// A click outside every modal popup's content dismisses the popups
	// and consumes the click (the modal convention) - the hover path's
	// widgets must not belong to the popup tree. Runs BEFORE the widget
	// walk so the widget beneath never receives the dismissing click.
	if (HasOpenPopups(ctx) && ctx->ModalPopupCount > 0)
	{
		bool hitPopupContent = false;
		for (auto* w : ctx->HoverPath)
		{
			for (Widget* p = w; p; p = p->GetParent())
				if (p == ctx->PopupRoot.get()) { hitPopupContent = true; break; }
			if (hitPopupContent)
				break;
		}
		if (!hitPopupContent)
		{
			DismissPopups(ctx);
			return true;
		}
	}

	// The handlers can erase from ctx->HoverPath (the
	// DismissPopupsBeyond submenu replacement, the ClearInputReferencesTo)
	// - iterate a COPY so the mutation can't invalidate the walk.
	std::vector<Widget*> hoverPathCopy = ctx->HoverPath;
	for (auto it = hoverPathCopy.rbegin(); it != hoverPathCopy.rend(); ++it)
	{
		Reply reply = (*it)->OnMouseButtonDown(ctx->MousePos, button);
		if (reply.Handled)
		{
			if (reply.ShouldCaptureMouse) ctx->CaptureWidget = *it;
			if (reply.ShouldSetFocus)
			{
				if (m_FocusWidget && m_FocusWidget != *it)
					m_FocusWidget->OnFocusLost();
				m_FocusWidget = *it;
				m_FocusWidget->OnFocusReceived();
			}
			if (reply.DragPayloadFactory)
			{
				ctx->PendingDragWidget = *it;
				ctx->PendingDragFactory = std::move(reply.DragPayloadFactory);
				ctx->PendingDragStart = ctx->MousePos;
				ctx->PendingDragCaptured = reply.ShouldCaptureMouse;
			}
			FlushPendingPopupDismiss();
			return true;
		}
	}

	// Click on empty space — clear focus
	if (m_FocusWidget)
	{
		m_FocusWidget->OnFocusLost();
		m_FocusWidget = nullptr;
	}

	// Deferred popup dismissal flush (see RequestDismissPopups): popup
	// widgets must not destroy their own tree mid-click - the caller still
	// touches the widget after the callback returns.
	FlushPendingPopupDismiss();
	return false;
}

bool UIApplication::OnMouseButtonUp(void* window, int button)
{
	if (button < 0 || button >= 8) return false;
	WindowContext* ctx = window ? GetWindowContext(window) : GetFocusedContext();
	if (!ctx) return false;
	ctx->MouseButtonDown[button] = false;

	if (button == 0 && m_DragOp)
	{
		FinishDrag(ctx->MousePos, false);
		return true;
	}
	if (button == 0 && ctx->PendingDragWidget)
	{
		ctx->PendingDragWidget = nullptr;
		ctx->PendingDragFactory = nullptr;
	}

	Widget* target = ctx->CaptureWidget;
	if (target)
	{
		Reply reply = target->OnMouseButtonUp(ctx->MousePos, button);
		if (reply.ShouldReleaseCapture || !reply.ShouldCaptureMouse)
			ctx->CaptureWidget = nullptr;
		FlushPendingPopupDismiss();
		return reply.Handled;
	}

	// Iterate a copy (the handlers can erase from the
	// path - the DismissPopupsBeyond cleanup).
	std::vector<Widget*> hoverPathCopy = ctx->HoverPath;
	for (auto it = hoverPathCopy.rbegin(); it != hoverPathCopy.rend(); ++it)
	{
		Reply reply = (*it)->OnMouseButtonUp(ctx->MousePos, button);
		if (reply.Handled)
		{
			FlushPendingPopupDismiss();
			return true;
		}
	}
	FlushPendingPopupDismiss();
	return false;
}

void UIApplication::FlushPendingPopupDismiss()
{
	if (!m_PendingDismissPopups) return;
	m_PendingDismissPopups = false;
	DismissPopups();
}

void UIApplication::OnMouseWheel(void* window, float delta)
{
	WindowContext* ctx = window ? GetWindowContext(window) : GetFocusedContext();
	if (!ctx) return;
	std::vector<Widget*> hoverPathCopy = ctx->HoverPath;
	for (auto it = hoverPathCopy.rbegin(); it != hoverPathCopy.rend(); ++it)
	{
		Reply reply = (*it)->OnMouseWheel(ctx->MousePos, delta);
		if (reply.Handled) return;
	}
}

bool UIApplication::OnKeyDown(void* window, int key, int mods)
{
	WindowContext* ctx = window ? GetWindowContext(window) : GetFocusedContext();
	if (!ctx) return false;
	// Escape cancels an active drag (payload restores its snapshot)
	if (key == 256 && m_DragOp)
	{
		FinishDrag(ctx->MousePos, true);
		return true;
	}

	// Escape exits widget picking, committing the hovered widget so the
	// reflector focuses it; plain cancel when nothing is hovered. The
	// candidate is the freshest across windows (the cursor's window).
	if (key == 256 && m_PickMode != PickMode::None)
	{
		auto cb = std::move(m_PickCallback);
		m_PickCallback = nullptr;
		Widget* picked = m_PickedWidget;
		m_PickMode = PickMode::None;
		for (auto& c : m_Contexts)
			c->PickedWidget = nullptr;
		m_PickedWidget = nullptr;

		bool excluded = false;
		if (m_PickExclusion && picked)
		{
			for (Widget* w = picked; w; w = w->GetParent())
				if (w == m_PickExclusion) { excluded = true; break; }
		}

		if (cb && picked && !excluded)
			cb(picked);
		return true;
	}

	if (m_FocusWidget)
	{
		Reply reply = m_FocusWidget->OnKeyDown(key, mods);
		if (reply.ShouldReleaseFocus)
		{
			m_FocusWidget->OnFocusLost();
			m_FocusWidget = nullptr;
		}
		if (reply.Handled) return true;
	}

	// Enter / Space → click focused widget
	if ((key == 257 || key == 32) && m_FocusWidget)
	{
		ClickFocusedWidget();
		return true;
	}

	// Tab / Shift-Tab → next/prev in registry
	if (key == 258 && !m_FocusRegistry.empty())
	{
		bool shift = (mods & 1) != 0;
		int currentIdx = -1;
		for (size_t i = 0; i < m_FocusRegistry.size(); i++)
			if (m_FocusRegistry[i].widget == m_FocusWidget) { currentIdx = (int)i; break; }
		int nextIdx = -1;
		if (shift)
		{
			for (int i = currentIdx - 1; i >= 0; i--)
				if (m_FocusRegistry[i].scopeId == m_CurrentLayerId) { nextIdx = i; break; }
			if (nextIdx < 0)
				for (int i = (int)m_FocusRegistry.size() - 1; i >= 0; i--)
					if (m_FocusRegistry[i].scopeId == m_CurrentLayerId) { nextIdx = i; break; }
		}
		else
		{
			for (size_t i = currentIdx + 1; i < m_FocusRegistry.size(); i++)
				if (m_FocusRegistry[i].scopeId == m_CurrentLayerId) { nextIdx = (int)i; break; }
			if (nextIdx < 0)
				for (size_t i = 0; i < m_FocusRegistry.size(); i++)
					if (m_FocusRegistry[i].scopeId == m_CurrentLayerId) { nextIdx = (int)i; break; }
		}
		if (nextIdx >= 0) SetFocus(m_FocusRegistry[nextIdx].widget);
		return true;
	}

	// Arrow keys → directional navigation
	int dir = -1;
	if (key == 263) dir = 0; // Left
	else if (key == 262) dir = 1; // Right
	else if (key == 265) dir = 2; // Up
	else if (key == 264) dir = 3; // Down

	if (dir >= 0 && !m_FocusRegistry.empty())
	{
		if (!m_FocusWidget)
		{
			for (auto& e : m_FocusRegistry)
				if (e.scopeId == m_CurrentLayerId) { SetFocus(e.widget); break; }
		}
		else NavigateFocus(dir);
		return true;
	}
	std::vector<Widget*> hoverPathCopy = ctx->HoverPath;
	for (auto it = hoverPathCopy.rbegin(); it != hoverPathCopy.rend(); ++it)
	{
		Reply reply = (*it)->OnKeyDown(key, mods);
		if (reply.Handled) return true;
	}
	return false;
}

bool UIApplication::OnKeyUp(void* window, int key, int mods)
{
	WindowContext* ctx = window ? GetWindowContext(window) : GetFocusedContext();
	if (!ctx) return false;

	if (m_FocusWidget)
	{
		Reply reply = m_FocusWidget->OnKeyUp(key, mods);
		if (reply.Handled) return true;
	}
	std::vector<Widget*> hoverPathCopy = ctx->HoverPath;
	for (auto it = hoverPathCopy.rbegin(); it != hoverPathCopy.rend(); ++it)
	{
		Reply reply = (*it)->OnKeyUp(key, mods);
		if (reply.Handled) return true;
	}
	return false;
}

bool UIApplication::OnTextInput(void* window, uint32_t codepoint)
{
	WindowContext* ctx = window ? GetWindowContext(window) : GetFocusedContext();
	if (!ctx) return false;
	if (m_FocusWidget)
	{
		Reply reply = m_FocusWidget->OnTextInput(codepoint);
		if (reply.ShouldReleaseFocus)
		{
			m_FocusWidget->OnFocusLost();
			m_FocusWidget = nullptr;
		}
		if (reply.Handled) return true;
	}
	std::vector<Widget*> hoverPathCopy = ctx->HoverPath;
	for (auto it = hoverPathCopy.rbegin(); it != hoverPathCopy.rend(); ++it)
	{
		Reply reply = (*it)->OnTextInput(codepoint);
		if (reply.Handled) return true;
	}
	return false;
}

Widget* UIApplication::PickAbsoluteRecursive(WindowContext& ctx, Widget* w, const Geometry& geom, const glm::vec2& pt)
{
	if (!w) return nullptr;
	if (w->IsHitTestExcluded(pt)) return nullptr;

	// Deepest child first (back-to-front)
	ArrangedChildren arranged;
	w->ArrangeChildren(geom, arranged);
	for (int i = (int)arranged.Size() - 1; i >= 0; --i)
	{
		Widget* hit = PickAbsoluteRecursive(ctx, arranged[i].WidgetPtr, arranged[i].Geom, pt);
		if (hit) return hit;
	}

	if (geom.IsUnderLocation(pt)) return w;
	return nullptr;
}

// ---- Drag router ------------------------------------------------------------

void UIApplication::CollectHitPath(WindowContext& ctx, Widget* widget, const Geometry& geom, const glm::vec2& absPoint,
	std::vector<Widget*>& outPath)
{
	if (!widget) return;
	if (!IsVisibleForHitTest(widget->GetVisibility())) return;
	if (!geom.IsUnderLocation(absPoint)) return;

	ArrangedChildren kids;
	widget->ArrangeChildren(geom, kids);

	bool childHit = false;
	for (int i = (int)kids.Size() - 1; i >= 0; --i)
	{
		size_t before = outPath.size();
		CollectHitPath(ctx, kids[i].WidgetPtr, kids[i].Geom, absPoint, outPath);
		if (outPath.size() != before) { childHit = true; break; }
	}
	if (childHit)
	{
		outPath.insert(outPath.begin(), widget);
		return;
	}
	if (widget->GetVisibility() == Visibility::Visible)
		outPath.push_back(widget);
}

void UIApplication::StartDrag(WindowContext& ctx, const glm::vec2& pos)
{
	std::function<DragPayload()> factory = std::move(ctx.PendingDragFactory);
	Widget* source = ctx.PendingDragWidget;
	ctx.PendingDragWidget = nullptr;
	ctx.PendingDragFactory = nullptr;

	// The router owns input from here — no widget holds capture during a drag.
	ctx.CaptureWidget = nullptr;

	DragPayload payload = factory ? factory() : DragPayload();
	if (payload.Type.empty()) return;

	// Best-effort source notification — the source may have been destroyed by
	// a rebuild between press and threshold crossing, so verify tree
	// membership first.
	if (source)
	{
		bool alive = false;
		if (ctx.Root)
		{
			std::function<void(Widget*)> walk = [&](Widget* w) {
				if (!w || alive) return;
				if (w == source) { alive = true; return; }
				w->ForEachChild([&](Widget* c) { walk(c); });
			};
			walk(ctx.Root.get());
		}
		if (alive)
			source->OnDragDetected();
	}

	DragOperation op;
	op.Payload = std::move(payload);
	op.SourceWidget = source;
	op.StartPos = pos;
	op.CurrentPos = pos;
	// The single capability gate: checked once at drag start. Tier-0
	// platforms keep the single-window loop; Tier-1 routes the drag across
	// windows.
	op.CrossWindowRouting = TinyEngine::DetectWindowCaps().CanTrackGlobalCursor;
	m_DragOp = std::move(op);
	m_DragContext = &ctx;
	m_DragTargetContext = &ctx;
	m_DragOutsideAllWindows = false;
	RouteDrag(pos);
}

void UIApplication::RouteDrag(const glm::vec2& pos)
{
	if (!m_DragOp) return;
	DragOperation& op = *m_DragOp;
	WindowContext* srcCtx = m_DragContext ? m_DragContext : GetFocusedContext();
	if (!srcCtx) return;

	// ---- Resolve which window the drag routes into this frame ----
	WindowContext* targetCtx = srcCtx;
	glm::vec2 targetLocal = pos;
	bool outsideAll = false;

	if (op.CrossWindowRouting)
	{
		// Global cursor math: the source window keeps the implicit grab and
		// receives motion with out-of-bounds local coords; global = source
		// window pos + local. Candidate windows are hit in
		// most-recently-focused order (GLFW exposes no stacking order; MRU
		// matches stacking in practice).
		auto srcPos = TinyEngine::TryGetWindowPos((GLFWwindow*)srcCtx->Window);
		if (srcPos)
		{
			glm::vec2 global = { (float)srcPos->first + pos.x, (float)srcPos->second + pos.y };

			// Candidates include the SOURCE window (hovering back over it
			// restores in-window docking); hit in MRU order.
			std::vector<WindowContext*> candidates;
			for (auto& c : m_Contexts)
				if (c->Window)
					candidates.push_back(c.get());
			std::sort(candidates.begin(), candidates.end(),
				[](WindowContext* a, WindowContext* b) { return a->FocusStamp > b->FocusStamp; });

			targetCtx = nullptr;
			for (WindowContext* cand : candidates)
			{
				auto wp = TinyEngine::TryGetWindowPos((GLFWwindow*)cand->Window);
				if (!wp) continue;
				int fw = 0, fh = 0;
				glfwGetFramebufferSize((GLFWwindow*)cand->Window, &fw, &fh);
				if (global.x >= (float)wp->first && global.x <= (float)wp->first + (float)fw
					&& global.y >= (float)wp->second && global.y <= (float)wp->second + (float)fh)
				{
					targetCtx = cand;
					targetLocal = { global.x - (float)wp->first, global.y - (float)wp->second };
					break;
				}
			}

			if (!targetCtx)
			{
				// Over no Wisp window at all: tear-out state (the drop
				// releases outside; the payload decides the semantics).
				outsideAll = true;
				targetCtx = srcCtx;
				targetLocal = pos;
			}
		}
	}

	// ---- Hit-test the target window's tree with the converted position --
	std::vector<Widget*> newPath;
	if (!outsideAll && targetCtx->Root)
	{
		Geometry rootGeom;
		rootGeom.LocalSize = { (float)targetCtx->ViewportW, (float)targetCtx->ViewportH };
		// Popups render (and receive input) ABOVE the main tree - hit-test
		// them first so drag targets inside a popup (the colour picker's
		// saved-colours bar) resolve. Without this, a drag that starts in
		// a popup and stays over it finds no target and the drop never
		// fires.
		if (targetCtx->PopupRoot && targetCtx->PopupContentCount > 0)
		{
			ArrangedChildren popArranged;
			targetCtx->PopupRoot->ArrangeChildren(rootGeom, popArranged);
			for (int i = (int)popArranged.Size() - 1; i >= 0; --i)
			{
				CollectHitPath(*targetCtx, popArranged[i].WidgetPtr, popArranged[i].Geom, targetLocal, newPath);
				if (!newPath.empty()) break;
			}
		}
		if (newPath.empty())
		{
			ArrangedChildren arranged;
			targetCtx->Root->ArrangeChildren(rootGeom, arranged);
			for (int i = (int)arranged.Size() - 1; i >= 0; --i)
			{
				CollectHitPath(*targetCtx, arranged[i].WidgetPtr, arranged[i].Geom, targetLocal, newPath);
				if (!newPath.empty()) break;
			}
		}
	}

	// Enter/leave diff against the previous frame's path (a window switch
	// is just a target switch - the whole old path leaves, the new one
	// enters; DockSpace::OnDragLeave clears per-window preview state).
	size_t common = 0;
	while (common < op.CurrentPath.size() && common < newPath.size()
		&& op.CurrentPath[common] == newPath[common])
		common++;
	for (size_t i = op.CurrentPath.size(); i > common; --i)
		op.CurrentPath[i - 1]->OnDragLeave(op);
	for (size_t i = common; i < newPath.size(); ++i)
		newPath[i]->OnDragEnter(op);
	op.CurrentPath = std::move(newPath);
	op.CurrentTarget = op.CurrentPath.empty() ? nullptr : op.CurrentPath.back();
	op.CurrentPos = targetLocal;
	m_DragTargetContext = targetCtx;
	m_DragOutsideAllWindows = outsideAll;

	if (outsideAll) return; // no widgets to bubble over

	// OnDragOver bubbles leaf-first
	for (auto it = op.CurrentPath.rbegin(); it != op.CurrentPath.rend(); ++it)
	{
		Reply r = (*it)->OnDragOver(op);
		if (r.Handled) break;
	}
	if (op.Payload.OnDragged)
		op.Payload.OnDragged(targetLocal, op.CurrentTarget);
}

void UIApplication::FinishDrag(const glm::vec2& pos, bool cancel)
{
	if (!m_DragOp) return;
	RouteDrag(pos); // refresh the hit path against the current tree (it may
	                // have been rebuilt since the last move)

	bool outsideAll = m_DragOutsideAllWindows;
	DragOperation op = std::move(*m_DragOp);
	m_DragOp.reset();
	m_DragContext = nullptr;
	m_DragTargetContext = nullptr;
	m_DragOutsideAllWindows = false;

	for (auto it = op.CurrentPath.rbegin(); it != op.CurrentPath.rend(); ++it)
		(*it)->OnDragLeave(op);

	if (cancel)
	{
		if (op.Payload.OnCancelled)
			op.Payload.OnCancelled();
		return;
	}

	// Released outside every window (cross-window drag) — the payload
	// decides (docking tears out to a new window).
	if (outsideAll && op.CrossWindowRouting && op.Payload.OnDroppedOutside)
	{
		op.Payload.OnDroppedOutside(op);
		return;
	}

	// Drop bubbles leaf-first; first Handled wins.
	for (auto it = op.CurrentPath.rbegin(); it != op.CurrentPath.rend(); ++it)
	{
		Reply r = (*it)->OnDrop(op);
		if (r.Handled) break;
	}
}

void UIApplication::UpdateHover(WindowContext& ctx, const glm::vec2& screenPos)
{
	ctx.HoverPath.clear();

	// Check popup layer first
	if (ctx.PopupRoot && ctx.PopupContentCount > 0)
	{
		ArrangedChildren popupKids;
		Geometry popupGeom;
		popupGeom.LocalSize = { (float)ctx.ViewportW, (float)ctx.ViewportH };
		ctx.PopupRoot->ArrangeChildren(popupGeom, popupKids);
		for (int i = (int)popupKids.Size() - 1; i >= 0; --i)
		{
			if (HitTestRecursive(ctx, popupKids[i].WidgetPtr, popupKids[i].Geom, screenPos))
				break;
		}
		if (!ctx.HoverPath.empty())
		{
			DiffHoverPaths(ctx);
			ctx.LastHoverPath = ctx.HoverPath;
			return;
		}
	}

	if (!ctx.Root) return;

	for (int i = (int)ctx.ArrangedChildren.Size() - 1; i >= 0; --i)
	{
		HitTestRecursive(ctx, ctx.ArrangedChildren[i].WidgetPtr, ctx.ArrangedChildren[i].Geom, screenPos);
		if (!ctx.HoverPath.empty()) break;
	}

	DiffHoverPaths(ctx);
	ctx.LastHoverPath = ctx.HoverPath;

	// Update this window's pick candidate (cross-window fix: every window
	// tracks its own candidate; the global mirrors the cursor's window).
	if (m_PickMode != PickMode::None)
	{
		ctx.PickedWidget = nullptr;
		switch (m_PickMode)
		{
		case PickMode::HitTest:
			if (!ctx.HoverPath.empty())
				ctx.PickedWidget = ctx.HoverPath.back();
			break;
		case PickMode::Focusable:
			for (auto it = ctx.HoverPath.rbegin(); it != ctx.HoverPath.rend(); ++it)
			{
				if ((*it)->SupportsFocus())
				{
					ctx.PickedWidget = *it;
					break;
				}
			}
			break;
		case PickMode::Absolute:
		{
			Geometry rootGeom;
			rootGeom.LocalSize = { (float)ctx.ViewportW, (float)ctx.ViewportH };
			if (ctx.PopupRoot && ctx.PopupContentCount > 0)
				ctx.PickedWidget = PickAbsoluteRecursive(ctx, ctx.PopupRoot.get(), rootGeom, screenPos);
			if (!ctx.PickedWidget && ctx.Root)
				ctx.PickedWidget = PickAbsoluteRecursive(ctx, ctx.Root.get(), rootGeom, screenPos);
			break;
		}
		default: break;
		}
		if (&ctx == m_ActivePointerContext)
			m_PickedWidget = ctx.PickedWidget;
	}

	// Tooltip dwell tracking (suppressed while dragging)
	if (m_DragOp)
	{
		ctx.TooltipTarget = nullptr;
		ctx.TooltipDwell = 0.0f;
		if (ctx.TooltipShown) { DismissPopups(&ctx); ctx.TooltipShown = false; }
	}
	else if (!ctx.HoverPath.empty())
	{
		Widget* target = ctx.HoverPath.back();
		if (target != ctx.TooltipTarget)
		{
			ctx.TooltipTarget = target;
			ctx.TooltipDwell = 0.0f;
			if (ctx.TooltipShown) { DismissPopups(&ctx); ctx.TooltipShown = false; }
		}
		else if (target->HasToolTip() && !ctx.TooltipShown)
		{
			// The real elapsed frame time (the old
			// 1/60 assumed the 60 fps - at 120 fps the tooltip popped at
			// 0.25 s, at 30 fps at 1 s).
			const auto now = std::chrono::steady_clock::now();
			if (!m_TooltipLastTickTime)
				m_TooltipLastTickTime = now;
			const float dt = std::min(std::chrono::duration<float>(now - *m_TooltipLastTickTime).count(), 0.1f);
			m_TooltipLastTickTime = now;
			ctx.TooltipDwell += dt;
			if (ctx.TooltipDwell >= 0.5f)
			{
				auto tip = target->CreateToolTip();
				if (tip)
				{
					// Non-modal: a tooltip paints only - it never captures
					// input and never dismisses the click beneath it.
					ShowPopupAt(std::move(tip), ctx.MousePos + glm::vec2(12, 16), &ctx, false);
					ctx.TooltipShown = true;
				}
			}
		}
	}
	else
	{
		ctx.TooltipTarget = nullptr;
		ctx.TooltipDwell = 0.0f;
		if (ctx.TooltipShown) { DismissPopups(&ctx); ctx.TooltipShown = false; }
	}
}

bool UIApplication::HitTestRecursive(WindowContext& ctx, Widget* widget, const Geometry& geom, const glm::vec2& absPoint)
{
	if (!widget) return false;
	if (!IsVisibleForHitTest(widget->GetVisibility())) return false;
	if (widget->IsHitTestExcluded(absPoint)) return false;
	if (!geom.IsUnderLocation(absPoint)) return false;

	ArrangedChildren kids;
	widget->ArrangeChildren(geom, kids);

	bool childHit = false;
	for (int i = (int)kids.Size() - 1; i >= 0; --i)
	{
		if (HitTestRecursive(ctx, kids[i].WidgetPtr, kids[i].Geom, absPoint))
		{
			childHit = true;
			break;
		}
	}

	if (childHit)
	{
		ctx.HoverPath.insert(ctx.HoverPath.begin(), widget);
		return true;
	}

	if (widget->GetVisibility() == Visibility::Visible)
	{
		ctx.HoverPath.push_back(widget);
		return true;
	}
	return false;
}

void UIApplication::DiffHoverPaths(WindowContext& ctx)
{
	size_t commonLen = 0;
	while (commonLen < ctx.LastHoverPath.size() && commonLen < ctx.HoverPath.size()
		&& ctx.LastHoverPath[commonLen] == ctx.HoverPath[commonLen])
		commonLen++;

	// Iterate COPIES - the enter handlers can erase
	// from the paths (the DismissPopupsBeyond submenu replacement's
	// cleanup, the ClearInputReferencesTo) mid-dispatch.
	std::vector<Widget*> lastCopy = ctx.LastHoverPath;
	std::vector<Widget*> hoverCopy = ctx.HoverPath;
	for (size_t i = lastCopy.size(); i > commonLen; --i)
		lastCopy[i - 1]->OnMouseLeave();

	for (size_t i = commonLen; i < hoverCopy.size(); ++i)
		hoverCopy[i]->OnMouseEnter();
}

void UIApplication::Tick(WindowContext& ctx, uint32_t viewportWidth, uint32_t viewportHeight)
{
	if (!ctx.Root) return;

	ctx.DrawList.Clear();
	ctx.ArrangedChildren.Clear();

	// Measure
	glm::vec2 rootDesired = ctx.Root->ComputeDesiredSize();
	ctx.Root->SetCachedDesiredSize(rootDesired);

	// Arrange
	Geometry rootGeom;
	rootGeom.LocalSize = { (float)viewportWidth, (float)viewportHeight };
	ctx.Root->ArrangeChildren(rootGeom, ctx.ArrangedChildren);

	// Paint
	PaintArgs args;
	WidgetStyleContext style;
	style.ColorAndOpacity = ctx.Root->GetRenderTint();
	style.ColorAndOpacity.a *= ctx.Root->GetRenderOpacity();
	ctx.Root->OnPaint(args, rootGeom, ctx.DrawList, 0, style);

	// Paint popups on top (higher layer, same geometry, window-local)
	if (ctx.PopupRoot && ctx.PopupContentCount > 0)
	{
		ctx.PopupRoot->ComputeDesiredSize();
		ctx.PopupRoot->ArrangeChildren(rootGeom, ctx.ArrangedChildren);
		ctx.PopupRoot->OnPaint(args, rootGeom, ctx.DrawList, 10000, style);
	}

	// Drag ghost / overlay — above popups (layer 90000); painted into the
	// context the drag currently routes into (the source window when
	// single-window, the hovered window when cross-window).
	if (m_DragOp && m_DragTargetContext == &ctx && m_DragOp->Payload.PaintGhost)
		m_DragOp->Payload.PaintGhost(ctx.DrawList, 90000, m_DragOp->CurrentPos);

	// Cache viewport for popup hit-testing
	ctx.ViewportW = viewportWidth;
	ctx.ViewportH = viewportHeight;

	// Update hover after paint
	if (ctx.MousePos.x >= 0)
		UpdateHover(ctx, ctx.MousePos);
	else if (m_PickMode != PickMode::None)
		ctx.PickedWidget = nullptr;

	// Build focus registry for navigation (focused window only - navigation
	// routes to the OS-focused window).
	if (&ctx == GetFocusedContext())
	{
		m_FocusRegistry.clear();
		BuildFocusRegistry(ctx, ctx.Root.get(), rootGeom, 0);
		if (ctx.PopupRoot && ctx.PopupContentCount > 0)
		{
			Geometry popupGeom;
			popupGeom.LocalSize = { (float)viewportWidth, (float)viewportHeight };
			BuildFocusRegistry(ctx, ctx.PopupRoot.get(), popupGeom, m_CurrentLayerId);
		}

		// Validate focused widget — restore if destroyed/collapsed/hidden
		if (m_FocusWidget)
		{
			bool valid = false;
			for (auto& e : m_FocusRegistry)
				if (e.widget == m_FocusWidget) { valid = true; break; }
			if (!valid)
			{
				// The widget left the registry (a rebuild freed it) - its
				// OnFocusLost would run on the freed memory: null WITHOUT
				// calling (the full fix is a Widget::~Widget ->
				// ForgetWidget(this) registration).
				m_FocusWidget = nullptr;
				if (!m_FocusRegistry.empty())
				{
					m_FocusWidget = m_FocusRegistry[0].widget;
					m_FocusWidget->OnFocusReceived();
				}
			}
		}

		// Focus debug overlay — emitted through the debug_draw SCREEN batch
		// (drawn after all UI; the focused widget sits at a higher layer).
		if (m_FocusDebugOverlay)
		{
			for (auto& e : m_FocusRegistry)
			{
				glm::vec4 col = glm::vec4(0.3f, 0.5f, 0.8f, 0.4f);
				int layer = 1;
				if (e.widget == m_FocusWidget)
				{
					col = glm::vec4(0.2f, 0.9f, 0.3f, 0.6f);
					layer = 2;
				}

				debug_draw::DrawRect2D(e.min, e.max, col, { 1.0f, 0.0f, layer });
			}
		}
	}

	// Cross-window selection highlight (reflector): painted on the window
	// that CONTAINS the target - NOT gated on OS focus, so a widget
	// picked/selected in any window is outlined in ITS window.
	if (m_HighlightTarget)
	{
		Geometry hlGeom;
		hlGeom.LocalSize = { (float)ctx.ViewportW, (float)ctx.ViewportH };
		ArrangedChildren hlKids;
		ctx.Root->ArrangeChildren(hlGeom, hlKids);
		std::function<bool(Widget*, const Geometry&)> findHl = [&](Widget* w, const Geometry& g) -> bool {
			if (!w) return false;
			if (w == m_HighlightTarget)
			{
				glm::vec2 selMin = g.LocalToAbsolute({0,0});
				glm::vec2 selSize = g.LocalSize;
				debug_draw::DrawRectOutline2D(selMin, selMin + selSize, m_HighlightColor, { 2.0f, 0.0f, 3 });
				return true;
			}
			ArrangedChildren ac;
			w->ArrangeChildren(g, ac);
			for (auto& c : ac)
				if (findHl(c.WidgetPtr, c.Geom)) return true;
			return false;
		};
		for (auto& c : hlKids)
			if (findHl(c.WidgetPtr, c.Geom)) break;
	}

	// Update cursor based on this window's hovered widget (GLFW cursors are
	// per-window objects); the app-level override (eyedropper crosshair)
	// wins while set.
	RefreshCursor();
}

void UIApplication::ApplyCursor(WindowContext& ctx, CursorType desired)
{
	if (desired == ctx.CurrentCursor) return;
	ctx.CurrentCursor = desired;
	GLFWwindow* win = (GLFWwindow*)ctx.Window;
	if (!win) return;

	if (ctx.CachedCursor)
		glfwDestroyCursor((GLFWcursor*)ctx.CachedCursor);
	ctx.CachedCursor = nullptr;

	if (desired == CursorType::Default)
	{
		glfwSetCursor(win, nullptr);
		return;
	}

	int shape = GLFW_ARROW_CURSOR;
	switch (desired)
	{
	case CursorType::Default:       shape = GLFW_ARROW_CURSOR; break;
	case CursorType::TextEditBeam:  shape = GLFW_IBEAM_CURSOR; break;
	case CursorType::Hand:          shape = GLFW_HAND_CURSOR; break;
	case CursorType::ResizeEW:      shape = GLFW_HRESIZE_CURSOR; break;
	case CursorType::ResizeNS:      shape = GLFW_VRESIZE_CURSOR; break;
	case CursorType::Crosshair:     shape = GLFW_CROSSHAIR_CURSOR; break;
	}
	ctx.CachedCursor = glfwCreateStandardCursor(shape);
	glfwSetCursor(win, (GLFWcursor*)ctx.CachedCursor);
}

void UIApplication::RefreshCursor()
{
	WindowContext* ctx = GetFocusedContext();
	if (!ctx) return;
	CursorType desired = (m_CursorOverride != CursorType::Default)
		? m_CursorOverride
		: (!ctx->HoverPath.empty() ? ctx->HoverPath.back()->GetCursor() : CursorType::Default);
	ApplyCursor(*ctx, desired);
}

void UIApplication::SetFocus(Widget* widget)
{
	if (!widget || !widget->SupportsFocus()) return;
	if (m_FocusWidget == widget) return;
	if (m_FocusWidget)
		m_FocusWidget->OnFocusLost();
	m_FocusWidget = widget;
	m_FocusWidget->OnFocusReceived();
}

void UIApplication::ClickFocusedWidget()
{
	if (!m_FocusWidget) return;
	glm::vec2 zero(0, 0);
	m_FocusWidget->OnMouseButtonDown(zero, 0);
	m_FocusWidget->OnMouseButtonUp(zero, 0);
}

int UIApplication::PushFocusLayer(const std::string& name, FocusEntryPolicy policy)
{
	int id = m_NextLayerId++;
	if (!name.empty())
	{
		auto idIt = m_LayerIds.find(name);
		if (idIt != m_LayerIds.end())
			id = idIt->second;
		else
			m_LayerIds[name] = id;
	}

	m_FocusLayerStack.push_back({m_CurrentLayerName, m_FocusWidget, m_CurrentLayerId, policy});

	if (!m_CurrentLayerName.empty() && m_FocusWidget)
		m_LayerFocusMemory[m_CurrentLayerName] = m_FocusWidget;

	m_CurrentLayerId = id;
	m_CurrentLayerName = name;
	if (m_FocusWidget) { m_FocusWidget->OnFocusLost(); }
	m_FocusWidget = nullptr;

	if (policy == FocusEntryPolicy::RestorePrevious)
	{
		auto it = m_LayerFocusMemory.find(name);
		if (it != m_LayerFocusMemory.end() && it->second && it->second->SupportsFocus())
		{
			m_FocusWidget = it->second;
			m_FocusWidget->OnFocusReceived();
		}
	}

	// Record which window's panel layer this is, for focus-in recall.
	WindowContext* ctx = GetFocusedContext();
	if (ctx && !name.empty() && name.rfind("panel:", 0) == 0)
		ctx->LastActivePanelLayer = name;

	return id;
}

void UIApplication::PopFocusLayer()
{
	if (m_FocusLayerStack.empty()) return;

	if (!m_CurrentLayerName.empty() && m_FocusWidget)
		m_LayerFocusMemory[m_CurrentLayerName] = m_FocusWidget;

	if (m_FocusWidget) m_FocusWidget->OnFocusLost();
	auto prev = m_FocusLayerStack.back();
	m_FocusLayerStack.pop_back();
	m_CurrentLayerId = prev.layerId;
	m_CurrentLayerName = prev.name;
	if (prev.focusedWidget && prev.focusedWidget->SupportsFocus())
	{
		m_FocusWidget = prev.focusedWidget;
		m_FocusWidget->OnFocusReceived();
	}
	else
	{
		m_FocusWidget = nullptr;
	}
}

void UIApplication::SwitchToLayer(const std::string& name, FocusEntryPolicy policy)
{
	if (!m_CurrentLayerName.empty() && m_FocusWidget)
		m_LayerFocusMemory[m_CurrentLayerName] = m_FocusWidget;

	// Record which window's panel layer this is, for focus-in recall.
	WindowContext* ctx = GetFocusedContext();
	if (ctx && !name.empty() && name.rfind("panel:", 0) == 0)
		ctx->LastActivePanelLayer = name;

	for (size_t i = 0; i < m_FocusLayerStack.size(); i++)
	{
		if (m_FocusLayerStack[i].name == name)
		{
			while (m_FocusLayerStack.size() > i + 1)
			{
				if (m_FocusWidget) m_FocusWidget->OnFocusLost();
				m_FocusLayerStack.pop_back();
			}
			auto& target = m_FocusLayerStack.back();
			// Move the state OUT before the pop destroys it - the reads
			// below (target.name in particular) would otherwise dereference
			// the freed element (an ASAN-visible use-after-free).
			FocusLayerState targetState = std::move(target);
			m_FocusLayerStack.pop_back();
			m_CurrentLayerId = targetState.layerId;
			m_CurrentLayerName = targetState.name;
			if (m_FocusWidget) m_FocusWidget->OnFocusLost();
			m_FocusWidget = nullptr;

			if (policy == FocusEntryPolicy::RestorePrevious)
			{
				auto it = m_LayerFocusMemory.find(name);
				if (it != m_LayerFocusMemory.end() && it->second && it->second->SupportsFocus())
				{
					m_FocusWidget = it->second;
					m_FocusWidget->OnFocusReceived();
				}
			}
			return;
		}
	}

	PushFocusLayer(name, policy);
}

int UIApplication::GetFocusLayerId(const std::string& name) const
{
	if (name == m_CurrentLayerName) return m_CurrentLayerId;
	for (const auto& s : m_FocusLayerStack)
		if (s.name == name) return s.layerId;
	return -1;
}

void UIApplication::RemoveFocusLayer(const std::string& name)
{
	m_LayerFocusMemory.erase(name);

	// A removed panel layer also stops being a window's recall target.
	for (auto& ctx : m_Contexts)
		if (ctx->LastActivePanelLayer == name)
			ctx->LastActivePanelLayer.clear();

	if (name == m_CurrentLayerName)
	{
		if (m_FocusWidget) { m_FocusWidget->OnFocusLost(); m_FocusWidget = nullptr; }
		if (!m_FocusLayerStack.empty())
		{
			auto prev = m_FocusLayerStack.back();
			m_FocusLayerStack.pop_back();
			m_CurrentLayerId = prev.layerId;
			m_CurrentLayerName = prev.name;
			if (prev.focusedWidget && prev.focusedWidget->SupportsFocus())
			{
				m_FocusWidget = prev.focusedWidget;
				m_FocusWidget->OnFocusReceived();
			}
		}
		else
		{
			m_CurrentLayerId = 0;
			m_CurrentLayerName.clear();
		}
		return;
	}

	for (auto it = m_FocusLayerStack.begin(); it != m_FocusLayerStack.end(); ++it)
	{
		if (it->name == name)
		{
			m_FocusLayerStack.erase(it);
			return;
		}
	}
}

void UIApplication::BuildFocusRegistry(WindowContext& ctx, Widget* widget, const Geometry& geom, int layerId)
{
	if (!widget || widget->GetVisibility() == Visibility::Collapsed) return;

	auto rootIt = m_FocusLayerRoots.find(widget);
	if (rootIt != m_FocusLayerRoots.end())
		layerId = rootIt->second;

	if (widget->SupportsFocus())
	{
		glm::vec2 pos = geom.LocalToAbsolute({0, 0});
		glm::vec2 size = geom.LocalSize;
		m_FocusRegistry.push_back({widget, pos, pos + size, layerId});
	}

	ArrangedChildren arranged;
	widget->ArrangeChildren(geom, arranged);
	for (auto& child : arranged)
		BuildFocusRegistry(ctx, child.WidgetPtr, child.Geom, layerId);
}

bool UIApplication::NavigateFocus(int direction)
{
	if (m_FocusRegistry.empty()) return false;

	if (m_FocusWidget)
	{
		FocusEvent ev;
		ev.Direction = direction;
		ev.Reason = FocusEvent::Cause::Navigation;
		NavigationReply navReply = m_FocusWidget->OnNavigation(ev);
		if (navReply == NavigationReply::Stop)
			return true;
		if (navReply == NavigationReply::Wrap)
		{
			SetFocus(direction == 0 || direction == 2 ? m_FocusRegistry.back().widget : m_FocusRegistry[0].widget);
			return true;
		}
	}

	if (m_FocusWidget)
	{
		Widget* explicitTarget = m_FocusWidget->GetNavigationLink(direction);
		if (explicitTarget && explicitTarget->SupportsFocus())
		{
			SetFocus(explicitTarget);
			return true;
		}
	}

	glm::vec2 curMin, curMax;
	bool hasCurrent = false;
	for (auto& e : m_FocusRegistry)
	{
		if (e.widget == m_FocusWidget)
		{
			curMin = e.min; curMax = e.max;
			hasCurrent = true;
			break;
		}
	}

	if (!hasCurrent)
	{
		SetFocus(m_FocusRegistry[0].widget);
		return true;
	}

	glm::vec2 curCenter = (curMin + curMax) * 0.5f;

	struct Candidate { int idx; float score; };
	std::vector<Candidate> candidates;

	bool isHorizontal = (direction == 0 || direction == 1);
	bool positive = (direction == 1 || direction == 3);

	for (int i = 0; i < (int)m_FocusRegistry.size(); i++)
	{
		auto& e = m_FocusRegistry[i];
		if (e.widget == m_FocusWidget) continue;
		if (!e.widget->SupportsFocus()) continue;
		if (e.scopeId != m_CurrentLayerId) continue;

		glm::vec2 c = (e.min + e.max) * 0.5f;

		float primary = isHorizontal ? c.x : c.y;
		float farEdge = isHorizontal ? (positive ? curMax.x : curMin.x) : (positive ? curMax.y : curMin.y);

		if (positive && primary <= farEdge + 1.0f) continue;
		if (!positive && primary >= farEdge - 1.0f) continue;

		float nearEdge = isHorizontal ? (positive ? e.min.x : e.max.x) : (positive ? e.min.y : e.max.y);
		float primaryDist = positive ? (nearEdge - farEdge) : (farEdge - nearEdge);
		float orthoDist = std::abs((isHorizontal ? c.y : c.x) - (isHorizontal ? curCenter.y : curCenter.x));
		float score = primaryDist + 2.5f * orthoDist;

		candidates.push_back({i, score});
	}

	if (candidates.empty()) return false;

	std::sort(candidates.begin(), candidates.end(), [](auto& a, auto& b) { return a.score < b.score; });
	SetFocus(m_FocusRegistry[candidates[0].idx].widget);
	return true;
}

void UIApplication::ResetTransientInputState(WindowContext* ctx)
{
	if (!ctx) ctx = GetFocusedContext();
	if (!ctx) return;
	ctx->LastHoverPath.clear();
	ctx->CaptureWidget = nullptr;
	if (m_DragOp && m_DragContext == ctx)
	{
		m_DragOp->CurrentPath.clear();
		m_DragOp->CurrentTarget = nullptr;
	}
}

void UIApplication::ClearFocusIfInside(Widget* subtree)
{
	if (!m_FocusWidget || !subtree) return;
	for (Widget* p = m_FocusWidget; p; p = p->GetParent())
		if (p == subtree)
		{
			ClearFocus();
			return;
		}
}

bool UIApplication::IsAnyMouseButtonDown() const
{
	WindowContext* ctx = GetFocusedContext();
	if (!ctx)
		return false;
	for (bool down : ctx->MouseButtonDown)
		if (down) return true;
	return false;
}

void UIApplication::ClearInputReferencesTo(Widget* subtree)
{
	if (!subtree) return;

	auto inside = [&](Widget* w) {
		for (Widget* p = w; p; p = p->GetParent())
			if (p == subtree) return true;
		return false;
	};

	for (auto& c : m_Contexts)
	{
		std::erase_if(c->HoverPath, inside);
		std::erase_if(c->LastHoverPath, inside);
		if (c->CaptureWidget && inside(c->CaptureWidget))
			c->CaptureWidget = nullptr;
		if (c->TooltipTarget && inside(c->TooltipTarget))
		{
			c->TooltipTarget = nullptr;
			c->TooltipShown = false;
		}
		if (c->PendingDragWidget && inside(c->PendingDragWidget))
		{
			c->PendingDragWidget = nullptr;
			c->PendingDragFactory = nullptr;
		}
	}
	if (m_FocusWidget && inside(m_FocusWidget))
		m_FocusWidget = nullptr;
	if (m_DragOp)
	{
		std::erase_if(m_DragOp->CurrentPath, inside);
		if (m_DragOp->CurrentTarget && inside(m_DragOp->CurrentTarget))
			m_DragOp->CurrentTarget = nullptr;
	}
}

const std::vector<Widget*>& UIApplication::GetHoverPath() const
{
	static const std::vector<Widget*> kEmpty;
	WindowContext* ctx = GetFocusedContext();
	return ctx ? ctx->HoverPath : kEmpty;
}

glm::vec2 UIApplication::GetViewportSize() const
{
	WindowContext* ctx = GetFocusedContext();
	if (!ctx) return { 0, 0 };
	return { (float)ctx->ViewportW, (float)ctx->ViewportH };
}

} // namespace wisp
