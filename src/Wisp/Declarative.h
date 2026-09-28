#pragma once

#include "Wisp/Widgets.h"
#include <memory>
#include <type_traits>

namespace wisp {

// ---- S-prefix type aliases ------------------------------------------------

using SSpacer       = Spacer;
using STextBlock    = TextBlock;
using SImage        = Image;
using SBorder       = Border;

struct SVerticalBox   : BoxPanel { SVerticalBox()   : BoxPanel(BoxOrientation::Vertical)   {} };
struct SHorizontalBox : BoxPanel { SHorizontalBox() : BoxPanel(BoxOrientation::Horizontal) {} };

using SOverlay      = Overlay;
using SCanvas       = Canvas;
using SScrollBox    = ScrollBox;
using SSizeBox      = SizeBox;
using SScaleBox     = ScaleBox;
using SButton       = Button;
using SCheckBox     = CheckBox;
using SEditableText  = EditableText;
using SSlider        = Slider;
using SProgressBar   = ProgressBar;
using SMenu          = Menu;
using SMenuBar       = MenuBar;

// ---- WidgetRef — holds unique_ptr, supports -> chaining, converts to unique_ptr ---

template<typename T>
class WidgetRef
{
	std::unique_ptr<T> m_Ptr;

public:
	template<typename... Args>
	explicit WidgetRef(Args&&... args)
		: m_Ptr(std::make_unique<T>(std::forward<Args>(args)...)) {}

	WidgetRef(std::unique_ptr<T>&& p) : m_Ptr(std::move(p)) {}

	T* operator->() { return m_Ptr.get(); }
	const T* operator->() const { return m_Ptr.get(); }
	T& operator*() { return *m_Ptr; }
	const T& operator*() const { return *m_Ptr; }

	// Convert to unique_ptr for moving into containers
	operator std::unique_ptr<T>() && { return std::move(m_Ptr); }
	operator std::unique_ptr<Widget>() && { return std::move(m_Ptr); }
};

// WispNew — creates a WidgetRef<T> that supports -> for method access
// and converts to unique_ptr<T> when moved into slots/containers.
#define WispNew(T, ...) wisp::WidgetRef<wisp::T>(__VA_ARGS__)

// ---- Slot builders ---------------------------------------------------------

struct VBoxSlot
{
	Slot Data;
	BoxPanel* Owner = nullptr;

	VBoxSlot& Padding(float u)          { Data.SetPadding(u); return *this; }
	VBoxSlot& Padding(float h, float v) { Data.SetPadding(h, v); return *this; }
	VBoxSlot& HAlign(HAlign h)          { Data.Align.H = h; return *this; }
	VBoxSlot& VAlign(VAlign v)          { Data.Align.V = v; return *this; }

	void operator[](std::unique_ptr<Widget> child)
	{
		if (!Owner) return;
		size_t idx = Owner->GetChildCount();
		Owner->PushChild(std::move(child));
		if (idx < Owner->GetChildCount())
			Owner->GetSlot(idx) = Data;
	}

	// Accept WidgetRef by converting to unique_ptr
	template<typename T>
	void operator[](WidgetRef<T>&& ref) { (*this)[std::unique_ptr<Widget>(std::move(ref))]; }
};

} // namespace wisp
