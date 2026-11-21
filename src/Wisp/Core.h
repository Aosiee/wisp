#pragma once

#include <any>
#include <array>
#include <cstdint>
#include <functional>
#include <glm/glm.hpp>
#include <memory>
#include <string>
#include <vector>

#define WISP_WIDGET_TYPE(T) \
	const char* GetTypeName() const override { return #T; }

// Theme.h declares ResolveStyle; its definition lives at the bottom of
// this file (after the Widget class, or -Winvalid-use-of-incomplete-type)
// - the Theme type is included up front (OUTSIDE the
// namespace - the old placement nested every Theme symbol under
// wisp::wisp) so the body sees it complete too.
#include "Wisp/Theme.h"

namespace wisp {

enum class CursorType { Default, Hand, TextEditBeam, ResizeEW, ResizeNS, Crosshair };

// ---- Forward declarations --------------------------------------------------

class Widget;
struct Slot;
class Theme;
struct DrawList;

// ---- Geometry --------------------------------------------------------------

struct Geometry
{
	glm::mat3x2 AccumulatedTransform = glm::mat3x2(1.0f); // scale + translate
	glm::vec2   LocalSize             = { 0.0f, 0.0f };

	Geometry MakeChild(const glm::vec2& offset, const glm::vec2& size) const
	{
		Geometry child;
		child.AccumulatedTransform = AccumulatedTransform;
		child.AccumulatedTransform[2] += offset;
		child.LocalSize = size;
		return child;
	}

	glm::vec2 LocalToAbsolute(const glm::vec2& local) const
	{
		return glm::vec2(AccumulatedTransform * glm::vec3(local, 1.0f));
	}

	glm::vec2 AbsoluteToLocal(const glm::vec2& absolute) const
	{
		glm::mat3 inv = glm::inverse(glm::mat3(AccumulatedTransform));
		glm::vec3 result = inv * glm::vec3(absolute, 1.0f);
		return glm::vec2(result);
	}

	bool IsUnderLocation(const glm::vec2& absolutePoint) const
	{
		glm::vec2 local = AbsoluteToLocal(absolutePoint);
		return local.x >= 0.0f && local.y >= 0.0f
			&& local.x <= LocalSize.x && local.y <= LocalSize.y;
	}
};

// ---- Visibility ------------------------------------------------------------

enum class Visibility : uint8_t
{
	Visible,
	Hidden,               // takes space, doesn't paint or hit-test
	Collapsed,            // takes no space
	HitTestInvisible,     // paints, ignores input
	SelfHitTestInvisible, // children still hit-test
};

inline bool IsVisibleForLayout(Visibility v)  { return v != Visibility::Collapsed; }
inline bool IsVisibleForPaint(Visibility v)   { return v == Visibility::Visible || v == Visibility::HitTestInvisible || v == Visibility::SelfHitTestInvisible; }
inline bool IsVisibleForHitTest(Visibility v) { return v == Visibility::Visible || v == Visibility::SelfHitTestInvisible; }

// ---- Alignment -------------------------------------------------------------

enum class HAlign : uint8_t { Fill, Left, Center, Right };
enum class VAlign : uint8_t { Fill, Top, Center, Bottom };

struct Alignment { HAlign H = HAlign::Fill; VAlign V = VAlign::Fill; };

// Shared alignment resolution — given a slot rect and child desired size,
// produce the child's offset and size within the slot.
inline void ResolveAlignment(const glm::vec2& slotPos, const glm::vec2& slotSize,
	const glm::vec2& desiredSize, HAlign h, VAlign v,
	glm::vec2& outPos, glm::vec2& outSize)
{
	outSize = slotSize;
	if (h == HAlign::Left || h == HAlign::Center || h == HAlign::Right)
		outSize.x = desiredSize.x;
	if (v == VAlign::Top || v == VAlign::Center || v == VAlign::Bottom)
		outSize.y = desiredSize.y;

	outPos = slotPos;
	if (h == HAlign::Center) outPos.x += (slotSize.x - desiredSize.x) * 0.5f;
	if (h == HAlign::Right)  outPos.x += slotSize.x - desiredSize.x;
	if (v == VAlign::Center) outPos.y += (slotSize.y - desiredSize.y) * 0.5f;
	if (v == VAlign::Bottom) outPos.y += slotSize.y - desiredSize.y;
}

// ---- Slot ------------------------------------------------------------------

struct Slot
{
	float PaddingLeft = 0.0f, PaddingTop = 0.0f, PaddingRight = 0.0f, PaddingBottom = 0.0f;
	Alignment Align;
	float FillWeight = 0.0f;    // 0 = auto, >0 = fill with weight
	float MinWidth = -1.0f, MinHeight = -1.0f;
	float MaxWidth = -1.0f, MaxHeight = -1.0f;

	void SetPadding(float uniform)   { PaddingLeft = PaddingTop = PaddingRight = PaddingBottom = uniform; }
	void SetPadding(float h, float v) { PaddingLeft = PaddingRight = h; PaddingTop = PaddingBottom = v; }

	glm::vec2 PaddingSize() const { return { PaddingLeft + PaddingRight, PaddingTop + PaddingBottom }; }

	void ClampSize(glm::vec2& size) const
	{
		if (MinWidth  > 0) size.x = std::max(size.x, MinWidth);
		if (MinHeight > 0) size.y = std::max(size.y, MinHeight);
		if (MaxWidth  > 0) size.x = std::min(size.x, MaxWidth);
		if (MaxHeight > 0) size.y = std::min(size.y, MaxHeight);
	}
};

// ---- ArrangedChildren ------------------------------------------------------

struct ArrangedChild
{
	Widget*   WidgetPtr = nullptr;
	Geometry  Geom;
};

struct ArrangedChildren
{
	std::vector<ArrangedChild> Children;

	void Add(Widget* w, const Geometry& g) { Children.push_back({ w, g }); }
	void Clear() { Children.clear(); }
	size_t Size() const { return Children.size(); }
	const ArrangedChild& operator[](size_t i) const { return Children[i]; }

	auto begin() { return Children.begin(); }
	auto end()   { return Children.end(); }
	auto begin() const { return Children.begin(); }
	auto end()   const { return Children.end(); }
};

// ---- Drag & Drop ------------------------------------------------------------

struct DragOperation;

// Owned by value; payload data lives in `Data` (std::any). All widget
// references the router keeps are re-resolved per frame — the payload must
// never hold raw widget pointers across frames (see Application.cpp routing).
struct DragPayload
{
	std::string Type;
	std::any    Data;

	// Ghost decorator painted by the router on the drag overlay layer.
	std::function<void(DrawList&, int layer, const glm::vec2& cursor)> PaintGhost;
	// Per-frame while dragging (cursor pos + deepest widget under it).
	std::function<void(const glm::vec2& cursor, Widget* deepest)> OnDragged;
	// Called on Esc-cancel: the payload restores whatever it needs to.
	std::function<void()> OnCancelled;
	// Released outside every Wisp window (cross-window drag only). The
	// payload decides what that means (docking: tear out to a new window).
	std::function<void(struct DragOperation& op)> OnDroppedOutside;
};

struct DragOperation
{
	DragPayload Payload;
	Widget*     SourceWidget = nullptr;      // raw, re-validated via SourceAlive
	std::function<bool()> SourceAlive;       // optional liveness check
	glm::vec2   StartPos{ 0, 0 };
	Widget*     CurrentTarget = nullptr;     // deepest widget under cursor
	std::vector<Widget*> CurrentPath;        // hit path (root→leaf), for enter/leave diffing

	// Cross-window routing: computed ONCE at drag start from the platform
	// caps (a single capability gate; Tier-0 platforms run the
	// single-window loop unchanged). When true, RouteDrag resolves which
	// Wisp window the global cursor is over and routes the drag there.
	bool CrossWindowRouting = false;

	// The cursor position in the CURRENT TARGET window's local space (the
	// space widgets receive; equals the source window's local pos when
	// routing single-window). Consumers (DockSpace hit tests) read this
	// instead of the app's focused-window mouse position.
	glm::vec2 CurrentPos{ 0, 0 };
};

// ---- Reply -----------------------------------------------------------------

struct Reply
{
	bool Handled = false;
	bool ShouldCaptureMouse = false;
	bool ShouldReleaseCapture = false;
	bool ShouldSetFocus = false;
	bool ShouldReleaseFocus = false;
	std::function<DragPayload()> DragPayloadFactory;

	static Reply Unhandled() { return {}; }
	static Reply Handled_()   { Reply r; r.Handled = true; return r; }

	Reply& CaptureMouse()    { ShouldCaptureMouse = true; Handled = true; return *this; }
	Reply& ReleaseCapture()  { ShouldReleaseCapture = true; Handled = true; return *this; }
	Reply& SetFocus()        { ShouldSetFocus = true; Handled = true; return *this; }
	Reply& ReleaseFocus()    { ShouldReleaseFocus = true; Handled = true; return *this; }
	Reply& DetectDrag(std::function<DragPayload()> factory)
	{
		DragPayloadFactory = std::move(factory);
		Handled = true;
		return *this;
	}
};

// ---- Focus Navigation -------------------------------------------------------

enum class NavigationReply
{
	Escape,    // let spatial search run
	Stop,      // consumed, don't move focus
	Wrap,      // wrap to other side
};

struct FocusEvent
{
	enum class Cause { Navigation, Mouse, SetDirectly, Restoration };
	Cause Reason = Cause::Navigation;
	int   Direction = -1; // 0=Left,1=Right,2=Up,3=Down, -1=unknown
};

// ---- PaintArgs -------------------------------------------------------------

struct PaintArgs
{
	uint64_t FrameIndex = 0;
	int      InheritedClipIndex = -1;  // clip index from parent container
	float    DpiScale = 1.0f;          // physical/logical ratio for scissor
};

// ---- WidgetStyleContext ----------------------------------------------------

struct WidgetStyleContext
{
	glm::vec4 ColorAndOpacity = { 1.0f, 1.0f, 1.0f, 1.0f };
};

// ---- SizeMode -------------------------------------------------------------

enum class SizeMode : uint8_t
{
	Fixed,       // use explicit/wrapped size (default)
	AutoWidth,   // size width to content
	AutoHeight,  // size height to content
	AutoBoth,    // size both axes to content
};

// ---- Clipping -------------------------------------------------------------

enum class Clipping : uint8_t
{
	Inherit,                       // pass through parent's clip
	ClipToBounds,                  // clip to self bounds, intersect with parent
	ClipToBoundsWithoutIntersecting, // clip to self bounds, ignore parent (new clip state)
	ClipToBoundsAlways,            // hard barrier: always clip, cannot be overridden
	OnDemand,                      // clip only when desired size > allocated size
};

// ---- WrapMode --------------------------------------------------------------

enum class WrapMode : uint8_t
{
	None,       // no wrapping, text overflows
	Word,       // break at word boundaries (spaces)
	Character,  // break at any character
};

// ---- Widget (abstract base) ------------------------------------------------

class Widget
{
public:
	Widget() = default;
	virtual ~Widget()
	{
		// The per-frame animation sets hold raw pointers (double-buffered
		// in WindowContext); a composed-content panel that rebuilds while
		// one of its buttons is mid hover-animation would otherwise leave
		// a dangling entry for the next TickAnimation (the inspector's
		// write->rebuild path exposed this - a hover + fast click on the
		// Use-Selected arrow). The IsValid guard makes this a safe no-op
		// once the app is gone (shutdown order either way).
		CancelAnimationTick();
		// The app's raw references to this widget go with it - the focus
		// layer stack held a freed widget when a menu action rebuilt a panel,
		// and the menu's dismiss popped that layer and called into it.
		ForgetSelf();
	}

	// --- Layout ---
	virtual glm::vec2 ComputeDesiredSize() const = 0;
	virtual void ArrangeChildren(const Geometry& allottedGeometry, ArrangedChildren& outChildren) const = 0;

	// --- Painting ---
	virtual int OnPaint(const PaintArgs& args, const Geometry& geom,
		DrawList& drawList, int layerId, const WidgetStyleContext& style) const = 0;

	// --- Events ---
	virtual Reply OnMouseButtonDown(const glm::vec2&, int) { return Reply::Unhandled(); }
	virtual Reply OnMouseButtonUp(const glm::vec2&, int)   { return Reply::Unhandled(); }
	virtual Reply OnMouseMove(const glm::vec2&)                    { return Reply::Unhandled(); }
	virtual Reply OnMouseEnter()  { return Reply::Unhandled(); }
	virtual Reply OnMouseLeave()  { return Reply::Unhandled(); }
	virtual Reply OnMouseWheel(const glm::vec2&, float)      { return Reply::Unhandled(); }
	virtual Reply OnKeyDown(int, int)  { return Reply::Unhandled(); }
	virtual Reply OnKeyUp(int, int)    { return Reply::Unhandled(); }
	virtual Reply OnTextInput(uint32_t) { return Reply::Unhandled(); }
	virtual void  OnFocusReceived()  {}
	virtual void  OnFocusLost()      {}
	virtual NavigationReply OnNavigation(const FocusEvent&) { return NavigationReply::Escape; }

	// --- Drag & drop (router-driven; see Application.cpp) ---
	virtual Reply OnDragEnter(DragOperation&) { return Reply::Unhandled(); }
	virtual Reply OnDragLeave(DragOperation&) { return Reply::Unhandled(); }
	virtual Reply OnDragOver(DragOperation&)  { return Reply::Unhandled(); }
	virtual Reply OnDrop(DragOperation&)      { return Reply::Unhandled(); }
	virtual void  OnDragCancelled() {}
	virtual void  OnDragDetected() {} // source notification at threshold crossing

	// Animation — widgets that are animating register for per-frame ticks
	virtual void TickAnimation(float) {}
	void RequestAnimationTick();
	void CancelAnimationTick();
	void ForgetSelf();

	// --- State ---
	virtual bool SupportsFocus() const { return m_SupportsFocus; }
	void SetSupportsFocus(bool b) { m_SupportsFocus = b; }
	virtual const char* GetTypeName() const { return "Widget"; }
	virtual void ForEachChild(std::function<void(Widget*)>) {}

	Widget* GetParent() const { return m_Parent; }

	Visibility GetVisibility() const { return m_Visibility; }
	void SetVisibility(Visibility v) { m_Visibility = v; InvalidateLayout(); }

	// A widget whose geometry covers an area but captures input only over
	// part of it (the console band over the dock): returning true for a
	// point removes the widget from the hit/hover test at that point, so
	// the widgets beneath it receive the input.
	virtual bool IsHitTestExcluded(const glm::vec2&) const { return false; }

	bool IsEnabled() const { return m_Enabled; }
	void SetEnabled(bool e) { m_Enabled = e; }

	glm::vec4 GetRenderTint() const { return m_RenderTint; }
	void SetRenderTint(const glm::vec4& t) { m_RenderTint = t; }

	float GetRenderOpacity() const { return m_RenderOpacity; }
	void SetRenderOpacity(float o) { m_RenderOpacity = o; }

	Clipping GetClipping() const { return m_Clipping; }
	void SetClipping(Clipping c) { m_Clipping = c; }

	SizeMode GetSizeMode() const { return m_SizeMode; }
	void SetSizeMode(SizeMode m) { m_SizeMode = m; InvalidateLayout(); }

	// Tooltip — factory function that creates the tooltip widget on demand
	using ToolTipFactory = std::function<std::unique_ptr<Widget>()>;
	void SetToolTip(ToolTipFactory factory) { m_ToolTipFactory = std::move(factory); }
	bool HasToolTip() const { return !!m_ToolTipFactory; }
	std::unique_ptr<Widget> CreateToolTip() const { return m_ToolTipFactory ? m_ToolTipFactory() : nullptr; }

	// The doc id F1 opens while the pointer is over this widget (the
	// deepest tagged widget of the hover path wins).
	void SetHelpId(std::string id) { m_HelpId = std::move(id); }
	const std::string& GetHelpId() const { return m_HelpId; }

	// Cursor
	void SetCursor(CursorType c) { m_Cursor = c; }
	CursorType GetCursor() const { return m_Cursor; }

	void SetTheme(std::shared_ptr<Theme> theme) { m_Theme = std::move(theme); InvalidateStyle(); }
	std::shared_ptr<Theme> GetTheme() const { return m_Theme; }

	void SetNavigationLeft(Widget* w)  { m_NavLinks[0] = w; }
	void SetNavigationRight(Widget* w) { m_NavLinks[1] = w; }
	void SetNavigationUp(Widget* w)    { m_NavLinks[2] = w; }
	void SetNavigationDown(Widget* w)  { m_NavLinks[3] = w; }
	Widget* GetNavigationLink(int dir) const { return (dir >= 0 && dir < 4) ? m_NavLinks[dir] : nullptr; }

	// Cache / invalidation
	glm::vec2 GetCachedDesiredSize() const { return m_CachedDesiredSize; }
	void SetCachedDesiredSize(const glm::vec2& s) const { m_CachedDesiredSize = s; m_LayoutDirty = false; }
	bool IsLayoutDirty() const { return m_LayoutDirty; }
	void InvalidateLayout() const
	{
		m_LayoutDirty = true;
		if (m_Parent) m_Parent->InvalidateLayout();
	}

	void InvalidateStyle() const
	{
		// A theme (or variation) change must restyle the whole
		// subtree, not just this widget - styles resolve lazily on the
		// per-widget dirty flag, so mark every descendant dirty too.
		// (Previously this only set m_StyleDirty on self, so swapping a
		// subtree's theme after the first paint left descendants stale.)
		std::function<void(const Widget*)> mark = [&](const Widget* w) {
			w->m_StyleDirty = true; // mutable
			const_cast<Widget*>(w)->ForEachChild([&](Widget* c) { mark(c); });
		};
		mark(this);
	}

#if WISP_DEBUG
	const char* DebugName = nullptr;
	const char* CreatedFile = nullptr;
	int         CreatedLine = 0;
	void SetDebugName(const char* name) { DebugName = name; }
	Geometry    LastArrangedGeometry;
	uint64_t    LastArrangedFrame = 0;
#endif

protected:
	Widget*    m_Parent = nullptr;
	Visibility m_Visibility = Visibility::Visible;
	bool       m_Enabled = true;
	glm::vec4  m_RenderTint = { 1.0f, 1.0f, 1.0f, 1.0f };
	float      m_RenderOpacity = 1.0f;
	Clipping   m_Clipping = Clipping::Inherit;
	SizeMode   m_SizeMode = SizeMode::Fixed;
	ToolTipFactory m_ToolTipFactory;
	CursorType  m_Cursor = CursorType::Default;
	std::string m_HelpId;
	std::shared_ptr<Theme> m_Theme;
	Widget* m_NavLinks[4] = {};
	bool    m_SupportsFocus = false;
	mutable glm::vec2 m_CachedDesiredSize = { 0.0f, 0.0f };
	mutable bool      m_LayoutDirty = true;
	mutable bool      m_StyleDirty = true;

	friend class UIApplication;
	friend void SetupParentChild(Widget* parent, Widget* child);
	friend void ClearParent(Widget* child);
};

inline void SetupParentChild(Widget* parent, Widget* child)
{
	child->m_Parent = parent;
}

// Detaches a widget from its parent pointer. Panel content cached by the
// dock survives its host chrome (TabWell) being destroyed, and must not
// keep a dangling parent - theme resolution walks the parent chain.
inline void ClearParent(Widget* child)
{
	child->m_Parent = nullptr;
}

// The ResolveStyle definition (declared in Theme.h) - here Widget is
// complete, so the parent-chain walk parses cleanly (no
// -Winvalid-use-of-incomplete-type).
template<typename S, typename W>
S ResolveStyle(const W* widget, const S& fallback)
{
	for (const Widget* w = widget; w; w = w->GetParent())
	{
		if (auto* theme = w->GetTheme().get())
		{
			for (const Theme* t = theme; t; t = t->GetParentTheme().get())
			{
				if (const S* s = t->GetStyle<S>())
					return *s;
			}
		}
	}
	return fallback;
}

} // namespace wisp

