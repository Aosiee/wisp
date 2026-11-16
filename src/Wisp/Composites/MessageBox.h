#pragma once

#include "Wisp/Widgets.h"
#include "Wisp/Components/Button.h"
#include "Wisp/Components/TextBlock.h"
#include "Wisp/Components/Border.h"
#include "Wisp/Components/BoxPanel.h"
#include "Wisp/Components/Spacer.h"
#include "Wisp/Application.h"
#include <functional>
#include <string>
#include "Wisp/StyleTokens.h"

namespace wisp {

// ---- MessageBox — reusable modal dialog composite --------------------------
//
// Usage:
//   MessageBox::Show(fc, fh, fontSize, "Title", "Message text");
//   MessageBox::ShowOkCancel(fc, fh, fontSize, "Title", "Are you sure?", []{ ... });

class MessageBox
{
public:
	// Single OK button — auto-dismisses on click
	static void Show(FontCache* fc, FontHandle fh, float fontSize,
		const std::string& title, const std::string& message);

	// OK / Cancel buttons
	static void ShowOkCancel(FontCache* fc, FontHandle fh, float fontSize,
		const std::string& title, const std::string& message,
		std::function<void()> onOk,
		std::function<void()> onCancel = nullptr);

	// Yes / No buttons
	static void ShowConfirm(FontCache* fc, FontHandle fh, float fontSize,
		const std::string& title, const std::string& message,
		std::function<void()> onConfirm,
		std::function<void()> onDecline = nullptr);

	// Base brush for the modal background constants (they were writable
	// statics nothing ever wrote).
	static constexpr Brush BackgroundBrush{ wisp::tokens::Popup };
	// Button brushes (shared defaults)
	static constexpr Brush ButtonNormal{ wisp::tokens::Control };
	static constexpr Brush ButtonHover{ wisp::tokens::Hover };
	static constexpr Brush ButtonPressed{ wisp::tokens::Pressed };

private:
	static std::unique_ptr<Widget> BuildContent(FontCache* fc, FontHandle fh, float fontSize,
		const std::string& title, const std::string& message,
		std::unique_ptr<Widget> buttons);
};

} // namespace wisp
