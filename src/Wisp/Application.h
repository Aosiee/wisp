#pragma once

#include "Wisp/Core.h"
#include "Wisp/DrawList.h"
#include "Wisp/Batcher.h"
#include "Wisp/FontCache.h"
#include "TinyEngine/Renderer/UIImageHandle.h"
#include "TinyEngine/Renderer/DebugDrawCommon.h" // the screen-overlay scratch's element type
#include <chrono>
#include <functional>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <unordered_map>
#include <vector>

namespace wisp {

class FontCache;
class RendererVulkan;
struct WispWindowRenderState;

// One OS window's worth of Wisp state. Everything that used to live on
// UIApplication as a single-window member splits into: per-context here
// (input, hover, capture, popups, cursor, DPI, draw list, drag *press*
// state, tooltip, animating widgets) vs global-by-design on UIApplication
// (focus, focus layers, the drag *operation* router, pick-mode state -
// see there).
struct WindowContext
{
	void* Window = nullptr;                  // GLFWwindow* (opaque - no GLFW include here)
	std::unique_ptr<Widget> Root;            // per-window root (DockSpace or shell VBox)
	WispWindowRenderState* RenderState = nullptr; // per-window staging/command buffer/viewport

	// Per-window draw list + arrange cache (Tick is per context).
	::wisp::DrawList DrawList;
	::wisp::ArrangedChildren ArrangedChildren;

	// T-F-B.3: the update (extract, main thread) builds + stores the
	// frame's batches; the draw callback (record, Loom worker) replays
	// this stored copy - it never touches the widget tree. T-F-C-B:
	// double-buffered by the frame slot - the pipelined update (frame N)
	// writes slot N%2 while the record (frame N-1) reads slot (N-1)%2.
	BatchedDrawData LastBatches[2];

	// Input / hover.
	glm::vec2 MousePos{ -1, -1 };
	glm::vec2 MouseScale{ 1, 1 };
	bool MouseButtonDown[8] = {};
	std::vector<Widget*> HoverPath;
	std::vector<Widget*> LastHoverPath;
	Widget* CaptureWidget = nullptr;

	// Popups are window-local: they render in this window's overlay
	// and clip to it.
	std::unique_ptr<Widget> PopupRoot;
	int PopupContentCount = 0;
	// The MODAL popup count (the dropdowns/menus/dialogs - not the
	// tooltips): while > 0 a click outside every popup's content
	// dismisses the popups instead of reaching the widget beneath.
	int ModalPopupCount = 0;

	// Cursor (GLFW cursors are per-window objects).
	CursorType CurrentCursor = CursorType::Default;
	void* CachedCursor = nullptr;

	// Tooltip dwell (per window - hover differs per window).
	Widget* TooltipTarget = nullptr;
	float TooltipDwell = 0.0f;
	bool TooltipShown = false;

	// Pick candidate (per window): while a widget
	// pick is active app-wide, each window tracks the candidate under ITS
	// cursor, and the click commits whichever window it lands in.
	Widget* PickedWidget = nullptr;

	// Pending drag press (per window). The live DragOperation itself is
	// global (one drag at a time across the app; the router records which
	// context owns it).
	Widget* PendingDragWidget = nullptr;
	std::function<DragPayload()> PendingDragFactory;
	glm::vec2 PendingDragStart{ 0, 0 };
	bool PendingDragCaptured = false;

	// Animating widgets (per window).
	std::set<Widget*> AnimatingWidgets;
	std::set<Widget*> NextAnimatingWidgets;

	// Viewport / DPI.
	uint32_t ViewportW = 0, ViewportH = 0;
	float ContentScale = 1.0f;

	// The layer name of this window's last-active panel (focus-in restores
	// it via SwitchToLayer - "back to where you were in that window").
	std::string LastActivePanelLayer;

	// Most-recent-focus stamp: cross-window drag hit-tests candidate
	// windows in MRU order, most-recently-focused first.
	uint64_t FocusStamp = 0;
};

// The named faces (the theme fonts in their smallest form). The app
// registers them after Init; an unregistered role answers the Regular face.
enum class FontRole : uint8_t { Regular, Medium, SemiBold, Mono, Icons, Count };

class UIApplication
{
public:
	// Init/Shutdown — creates/destroys the renderer and registers the draw callback.
	// The app only needs to call these once; everything else is automatic.
	static void Init(FontCache* fontCache);
	static void Shutdown();
	// Font hot reload (re-parse + atlas rebuild + relayout).
	static bool ReloadFont(const std::string& assetPath);
	static UIApplication& Get() { return *s_Instance; }
	static bool IsValid() { return s_Instance != nullptr; }

	FontCache* GetFontCache() const { return m_FontCache; }
	// (A registered flag, not InvalidFont: FontCache hands out handle 0 to
	// its first face.)
	void SetFont(FontRole role, FontHandle font) { m_Fonts[(size_t)role] = font; m_FontSet[(size_t)role] = true; }
	FontHandle GetFont(FontRole role) const
	{
		return m_FontSet[(size_t)role] ? m_Fonts[(size_t)role] : m_Fonts[(size_t)FontRole::Regular];
	}
	// The registered handle, or `fallback` when Wisp is not up / the role
	// was never registered (lets widgets ask without a null check).
	static FontHandle FontOr(FontRole role, FontHandle fallback)
	{
		if (!s_Instance || !s_Instance->m_FontSet[(size_t)role])
			return fallback;
		return s_Instance->m_Fonts[(size_t)role];
	}

	// ---- Window contexts ----

	// The default context (created by Init, bound to the first window that
	// draws) - the main window's context in every app shape today.
	WindowContext* GetDefaultContext() const { return m_Contexts.empty() ? nullptr : m_Contexts[0].get(); }
	WindowContext* GetWindowContext(void* window) const;
	WindowContext* GetFocusedContext() const;

	// Creates a context for a secondary OS window (each WindowNode
	// projects into one). The app owns the GLFWwindow; Wisp owns the root.
	WindowContext* CreateWindowContext(void* window, std::unique_ptr<Widget> root, float contentScale = 1.0f);

	// Tears a context down (its root, popups, capture, cursor object). The
	// caller routes the semantic side (CloseWindow on the dock model)
	// before/after this. Returns false if the window is unknown.
	bool DestroyWindowContext(void* window);

	// The context whose root subtree contains `w`, or nullptr.
	WindowContext* GetContextContaining(Widget* w) const;

	// OS window focus moved: remember the focused context and restore its
	// last-active panel layer (global single focus, per-window recall).
	// WindowLostFocus keeps the last state; a close calls
	// DestroyWindowContext instead.
	void OnWindowFocusChanged(void* window, bool focused);

	// Cursor entered/left a window: the authoritative "which window is the
	// pointer over" signal (cross-window pick tracking - GLFW cursor
	// positions alone are unreliable on Wayland for non-hovered windows).
	void OnCursorEntered(void* window, bool entered);

	// Root access - default context keeps the historical single-window API.
	void SetRootWidget(std::unique_ptr<Widget> root);
	Widget* GetRootWidget() const; // focused context's root (fallback: default)

	// Polls every context's OS cursor position (cheap; keeps static-mouse
	// windows in sync without relying on move events).
	void SyncMousePositions();

	// Popup system — window-local; the default context keeps the
	// historical no-window signatures. modal popups (the default -
	// dropdowns, menus, dialogs) dismiss on an outside click and block
	// it; non-modal ones (tooltips) paint only and never capture input.
	void ShowPopup(std::unique_ptr<Widget> popup, WindowContext* ctx = nullptr);
	void ShowPopupAt(std::unique_ptr<Widget> popup, const glm::vec2& screenPos, WindowContext* ctx = nullptr, bool modal = true);
	// Dismisses every popup beyond the first keepCount content canvases
	// (the dropdown's submenu replacement - the main popup stays).
	void DismissPopupsBeyond(int keepCount, WindowContext* ctx = nullptr);
	void DismissPopups(WindowContext* ctx = nullptr);
	bool HasOpenPopups(const WindowContext* ctx = nullptr) const;

	// Deferred dismissal: a popup widget must NEVER destroy
	// its own popup tree from inside its click handler - the caller still
	// touches the widget after the callback returns (Button clears its
	// pressed state, etc.). Popup-closing widgets call RequestDismissPopups
	// instead; UIApplication flushes it after the click dispatch completes.
	void RequestDismissPopups() { m_PendingDismissPopups = true; }

	// Input — per window; events carry their GLFWwindow*. The default
	// context keeps the historical no-window signatures for compat.
	void SetMousePosition(const glm::vec2& screenPos);
	void SetMousePosition(void* window, const glm::vec2& screenPos);
	void SetMouseScale(void* window, float sx, float sy);
	glm::vec2 GetMousePos() const; // focused context's mouse pos
	bool OnMouseButtonDown(int button);
	bool OnMouseButtonDown(void* window, int button);
	bool OnMouseButtonUp(int button);
	bool OnMouseButtonUp(void* window, int button);
	void OnMouseWheel(float delta);
	void OnMouseWheel(void* window, float delta);
	bool OnKeyDown(int key, int mods);
	bool OnKeyDown(void* window, int key, int mods);
	bool OnKeyUp(int key, int mods);
	bool OnKeyUp(void* window, int key, int mods);
	bool OnTextInput(uint32_t codepoint);
	bool OnTextInput(void* window, uint32_t codepoint);

	void TickAnimation(float dt);
	static float GetAnimTime() { return s_Instance ? s_Instance->m_AnimTime : 0.0f; }
	// The modifiers held now (1 = Shift, 2 = Ctrl, 4 = Alt, the key events' bits),
	// tracked from the Shift/Ctrl keys' own down/up - mouse events carry none, so a
	// Shift- or Ctrl-click asks here.
	int GetHeldModifiers() const { return m_HeldModifiers; }
	bool IsDragActive() const { return m_DragOp.has_value(); }

	// The in-flight drag's operation, or nullptr when nothing is dragging.
	// Read-only: the router owns the drag. Widgets read this at PAINT time
	// for global feedback (e.g. a slot row dimming itself while an
	// incompatible payload is being dragged anywhere over the app) - the
	// enter/leave/over callbacks only reach widgets under the cursor.
	const DragOperation* GetDragOperation() const { return m_DragOp ? &*m_DragOp : nullptr; }

	// Focus navigation — global single focus; navigation operates on the
	// FOCUSED window's registry.
	void SetFocus(Widget* widget);
	Widget* GetFocusWidget() const { return m_FocusWidget; }
	void ClearFocus()
	{
		if (m_FocusWidget) m_FocusWidget->OnFocusLost();
		m_FocusWidget = nullptr;
	}

	// Clears transient input state that may reference widgets destroyed by a
	// tree regeneration (dock chrome rebuilds): last-frame hover path, capture,
	// and the current drag's hit path (which dies with the old chrome). The
	// *pending* drag survives — its payload factory holds only value captures.
	void ResetTransientInputState(WindowContext* ctx);

	// Composed-content panels destroy and rebuild a whole subtree while the
	// cursor (or focus, or an active drag) may still reference widgets in
	// it. Call this BEFORE destroying `subtree`: it drops every hover/capture/
	// focus/tooltip/drag reference that points inside the subtree, so
	// DiffHoverPaths and friends never fire events on freed widgets.
	void ClearInputReferencesTo(Widget* subtree);
	// Drops every raw reference the app holds to this one widget (focus, the
	// focus layer stack and memory, hover/capture/tooltip/drag/pick state).
	// Called by ~Widget; never calls into the widget.
	void ForgetWidget(const Widget* w);

	// True while any mouse button is held - the
	// per-panel rebuild gates on it (a rebuild mid-press destroys the
	// pressed button and its mouse-up lands on nothing).
	bool IsAnyMouseButtonDown() const;

	// Drops the global focus (firing OnFocusLost) when the focused widget
	// lives inside `subtree`. Used by whole-tree teardowns (window context
	// destruction, root replacement) that reset the tree directly instead
	// of going through ClearInputReferencesTo's per-panel discipline.
	void ClearFocusIfInside(Widget* subtree);

	struct FocusEntry { Widget* widget; glm::vec2 min, max; int scopeId = 0; };
	const std::vector<FocusEntry>& GetFocusRegistry() const { return m_FocusRegistry; }
	// Instance state, not a static (no process-global mutable state) -
	// callers reach it through the app,
	// UIApplication::Get().EnableFocusDebugOverlay(...).
	void EnableFocusDebugOverlay(bool enable) { m_FocusDebugOverlay = enable; }
	bool IsFocusDebugOverlayEnabled() const { return m_FocusDebugOverlay; }

	// Focus layers
	enum class FocusEntryPolicy { RestorePrevious, Clear, KeepCurrent };

	int  PushFocusLayer(const std::string& name = "", FocusEntryPolicy policy = FocusEntryPolicy::RestorePrevious);
	void PopFocusLayer();
	int  GetCurrentLayerId() const { return m_CurrentLayerId; }
	const std::string& GetCurrentLayerName() const { return m_CurrentLayerName; }
	void SwitchToLayer(const std::string& name, FocusEntryPolicy policy = FocusEntryPolicy::RestorePrevious);
	int  GetFocusLayerId(const std::string& name) const;
	void RemoveFocusLayer(const std::string& name);

	// Per-subtree layer roots: BuildFocusRegistry assigns these widgets'
	// subtrees the given scope id (panel content isolation for docking).
	void RegisterFocusLayerRoot(Widget* w, int layerId) { m_FocusLayerRoots[w] = layerId; }
	void UnregisterFocusLayerRoot(Widget* w) { m_FocusLayerRoots.erase(w); }

	// Animation tick registration — widgets call these during OnPaint/events
	void RequestAnimationTick(Widget* w);
	void CancelAnimationTick(Widget* w);
	// A widget whose tooltip depends on WHERE it is hovered (a painted list's
	// row) restarts it when that changes: the shown tooltip goes, the dwell
	// starts again (what a change of hovered widget does).
	void ResetToolTip(const Widget* w);

	// Widget picking — persistent pick mode with live hover tracking.
	// While active, GetPickedWidget() returns the widget under the cursor
	// (per mode, tracked in whichever window the cursor is over). The click
	// callback fires on the next mouse press and then pick mode exits.
	enum class PickMode { None, HitTest, Absolute, Focusable };
	void SetPickMode(PickMode m);
	PickMode GetPickMode() const { return m_PickMode; }
	bool IsPickModeActive() const { return m_PickMode != PickMode::None; }
	Widget* GetPickedWidget() const { return m_PickedWidget; }
	void SetWidgetPickCallback(std::function<void(Widget*)> cb) { m_PickCallback = std::move(cb); }
	void CancelWidgetPick() { m_PickCallback = nullptr; }

	// A simpler "next click anywhere" capture (no widget candidate needed -
	// e.g. the colour picker's eyedropper). Fires on the next mouse-down in
	// any Wisp window, consuming the click (popups stay open).
	void SetScreenPickCallback(std::function<void()> cb) { m_ScreenPickCallback = std::move(cb); }
	void CancelScreenPick() { m_ScreenPickCallback = nullptr; }
	bool HasScreenPick() const { return (bool)m_ScreenPickCallback; }

	// An app-level cursor override (the eyedropper arms a crosshair). While
	// set it wins over the hovered widget's cursor; applied immediately.
	void SetCursorOverride(CursorType c) { m_CursorOverride = c; RefreshCursor(); }
	void ClearCursorOverride() { m_CursorOverride = CursorType::Default; RefreshCursor(); }
	CursorType GetCursorOverride() const { return m_CursorOverride; }
	void RefreshCursor();
	void ApplyCursor(WindowContext& ctx, CursorType desired);
	void SetPickExclusion(Widget* w) { m_PickExclusion = w; }
	Widget* GetPickExclusion() const { return m_PickExclusion; }
	const std::vector<Widget*>& GetHoverPath() const; // focused context's hover path
	glm::vec2 GetViewportSize() const;                // focused context's viewport

	// Cross-window selection highlight (reflector): painted on the window
	// that contains the target, so a widget picked/selected in any window
	// is outlined in ITS window.
	void SetSelectionHighlight(Widget* target, const glm::vec4& color);
	void ClearSelectionHighlight();

	// Iterates every context that has a root (the reflector's tree spans
	// all windows).
	void ForEachContext(std::function<void(WindowContext&)> fn);

	// Textured quads (Wisp images): app-managed textures on the shared
	// renderer. Returns 0 on failure; DestroyTexture is a no-op for 0.
	uint64_t CreateTexture(uint32_t width, uint32_t height, const uint8_t* rgbaData);
	// Wrap an engine-acquired
	// UIImageHandle (a framebuffer colour attachment or a texture's image)
	// as a samplable texture; the caller keeps ownership of the image
	// (DestroyTexture frees only the wrapper).
	uint64_t WrapUIImageHandle(const TinyEngine::UIImageHandle& handle);
	void UpdateTexture(uint64_t handle, uint32_t x, uint32_t y, uint32_t width, uint32_t height, const uint8_t* rgbaData);
	void DestroyTexture(uint64_t handle);

private:
	UIApplication();
	~UIApplication();

	void UpdateHover(WindowContext& ctx, const glm::vec2& screenPos);
	bool HitTestRecursive(WindowContext& ctx, Widget* widget, const Geometry& geom, const glm::vec2& absPoint);
	void DiffHoverPaths(WindowContext& ctx);
	void CollectHitPath(WindowContext& ctx, Widget* widget, const Geometry& geom, const glm::vec2& absPoint, std::vector<Widget*>& outPath);
	void StartDrag(WindowContext& ctx, const glm::vec2& pos);
	void RouteDrag(const glm::vec2& pos);
	void FinishDrag(const glm::vec2& pos, bool cancel);

	// Internal tick: called from the draw callback per window.
	// measure → arrange → paint for one context.
	void Tick(WindowContext& ctx, uint32_t viewportWidth, uint32_t viewportHeight);

	void BuildFocusRegistry(WindowContext& ctx, Widget* root, const Geometry& geom, int layerId);
	void FlushPendingPopupDismiss();
	bool NavigateFocus(int direction); // 0=Left,1=Right,2=Up,3=Down
	void ClickFocusedWidget();

	std::vector<FocusEntry> m_FocusRegistry;

	struct FocusLayerState { std::string name; Widget* focusedWidget = nullptr; int layerId = 0; FocusEntryPolicy entryPolicy = FocusEntryPolicy::RestorePrevious; };
	std::vector<FocusLayerState> m_FocusLayerStack;
	std::unordered_map<std::string, Widget*> m_LayerFocusMemory; // per-named-layer last focused widget
	std::unordered_map<std::string, int> m_LayerIds;            // stable ids for named layers
	std::unordered_map<Widget*, int> m_FocusLayerRoots;         // per-subtree layer roots
	int m_CurrentLayerId = 0;
	std::string m_CurrentLayerName;
	int m_NextLayerId = 1;

	static UIApplication* s_Instance;

	bool m_FocusDebugOverlay = false;

	// The tooltip dwell's last tick (UpdateHover; formerly a function-local
	// static). Empty until the first dwell tick, which takes its time as
	// the baseline - so that tick's dt is 0, as the static's first-use
	// initialisation gave. One per app, shared by every window's context,
	// as before. Game thread.
	std::optional<std::chrono::steady_clock::time_point> m_TooltipLastTickTime;

	// One context per OS window; [0] is the default (main) context.
	std::vector<std::unique_ptr<WindowContext>> m_Contexts;
	WindowContext* m_FocusedContext = nullptr;

	// One renderer for the whole app (shared pipelines/atlas/descriptor
	// sets); per-window transient state lives in WindowContext::RenderState.
	std::unique_ptr<RendererVulkan> m_Renderer;

	FontCache* m_FontCache = nullptr;
	FontHandle m_Fonts[(size_t)FontRole::Count] = {};
	bool m_FontSet[(size_t)FontRole::Count] = {};

	// Global by design: one focused widget in the whole app (living in the
	// OS-focused window), one drag operation at a time (the router records
	// its owning context).
	Widget*  m_FocusWidget = nullptr;
	std::optional<DragOperation> m_DragOp;
	WindowContext* m_DragContext = nullptr;
	WindowContext* m_DragTargetContext = nullptr; // which window the drag routes into this frame
	bool m_DragOutsideAllWindows = false;
	uint64_t m_FocusCounter = 0;

	// Pick mode (global flag; the picked widget tracks the focused window's
	// hover - the reflector lives in one window and picks in it).
	std::function<void(Widget*)> m_PickCallback;
	std::function<void()> m_ScreenPickCallback;
	CursorType m_CursorOverride = CursorType::Default;
	PickMode m_PickMode = PickMode::None;
	Widget*  m_PickedWidget = nullptr;   // freshest candidate = cursor's window's
	Widget*  m_PickExclusion = nullptr;
	WindowContext* m_ActivePointerContext = nullptr; // window the cursor is over

	// Cross-window selection highlight (reflector).
	Widget* m_HighlightTarget = nullptr;
	glm::vec4 m_HighlightColor{ 0, 0, 0, 0 };

	// Deferred popup dismissal (see RequestDismissPopups).
	bool m_PendingDismissPopups = false;

	Widget* PickAbsoluteRecursive(WindowContext& ctx, Widget* w, const Geometry& geom, const glm::vec2& pt);

	// Animation
	float  m_AnimTime = 0.0f;
	int    m_HeldModifiers = 0; // GetHeldModifiers

	// The debug-draw screen sub-batch's per-frame build scratch (Init's
	// update callback; a member, not a function-local static). Game thread
	// only; reused for its capacity.
	std::vector<TinyEngine::DebugDrawVertex> m_ScreenOverlayScratch;
};

} // namespace wisp
