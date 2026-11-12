#include "Wisp/Composites/MessageBox.h"
#include "Wisp/Declarative.h"
#include "Wisp/Components/Spacer.h"

namespace wisp {

static WidgetRef<SButton> MakeModalButton(FontCache* fc, FontHandle fh, float fs,
	const std::string& label, std::function<void()> onClick)
{
	auto btn = WispNew(SButton);
	btn->SetNormalBrush(MessageBox::ButtonNormal)
		.SetHoveredBrush(MessageBox::ButtonHover)
		.SetPressedBrush(MessageBox::ButtonPressed)
		.SetOnClicked(std::move(onClick));
	btn->SetChild(WispNew(STextBlock, fc, fh, fs, label, glm::vec4(1,1,1,1)));
	return btn;
}

std::unique_ptr<Widget> MessageBox::BuildContent(FontCache* fc, FontHandle fh, float fs,
	const std::string& title, const std::string& message, std::unique_ptr<Widget> buttons)
{
	auto content = WispNew(SVerticalBox);
	*content
		+ content->AddSlot().Padding(0, 0)[ WispNew(STextBlock, fc, fh, fs + 2.0f, title, glm::vec4(1.0f, 0.9f, 0.3f, 1.0f)) ]
		+ content->AddSlot().Padding(20, 12).Fill(1.0f)[ WispNew(STextBlock, fc, fh, fs, message, glm::vec4(0.8f, 0.8f, 0.85f, 1.0f)) ]
		+ content->AddSlot().Padding(0, 0)[ std::move(buttons) ];

	auto box = WispNew(SBorder);
	box->SetBrush(BackgroundBrush).SetPadding(20, 16).SetChild(content);

	auto size = WispNew(SSizeBox);
	size->SetMinWidth(360.0f).SetMinHeight(200.0f);
	size->SetChild(std::move(box));
	return size;
}

void MessageBox::Show(FontCache* fc, FontHandle fh, float fs,
	const std::string& title, const std::string& message)
{
	auto btn = MakeModalButton(fc, fh, fs, "OK", []() {
		UIApplication::Get().RequestDismissPopups();
	});

	auto row = WispNew(SHorizontalBox);
	*row
		+ row->AddSlot().Fill(1.0f)[ WispNew(SSpacer, glm::vec2(0,0)) ]
		+ row->AddSlot()[ btn ]
		+ row->AddSlot().Fill(1.0f)[ WispNew(SSpacer, glm::vec2(0,0)) ];

	UIApplication::Get().ShowPopup(BuildContent(fc, fh, fs, title, message, std::move(row)));
}

void MessageBox::ShowOkCancel(FontCache* fc, FontHandle fh, float fs,
	const std::string& title, const std::string& message,
	std::function<void()> onOk, std::function<void()> onCancel)
{
	auto okBtn = MakeModalButton(fc, fh, fs, "OK", [onOk]() {
		if (onOk) onOk();
		UIApplication::Get().RequestDismissPopups();
	});
	auto cancelBtn = MakeModalButton(fc, fh, fs, "Cancel", [onCancel]() {
		if (onCancel) onCancel();
		UIApplication::Get().RequestDismissPopups();
	});

	auto row = WispNew(SHorizontalBox);
	*row
		+ row->AddSlot().Fill(1.0f)[ okBtn ]
		+ row->AddSlot().Fill(1.0f)[ cancelBtn ];

	UIApplication::Get().ShowPopup(BuildContent(fc, fh, fs, title, message, std::move(row)));
}

void MessageBox::ShowConfirm(FontCache* fc, FontHandle fh, float fs,
	const std::string& title, const std::string& message,
	std::function<void()> onConfirm, std::function<void()> onDecline)
{
	auto yesBtn = MakeModalButton(fc, fh, fs, "Yes", [onConfirm]() {
		if (onConfirm) onConfirm();
		UIApplication::Get().RequestDismissPopups();
	});
	auto noBtn = MakeModalButton(fc, fh, fs, "No", [onDecline]() {
		if (onDecline) onDecline();
		UIApplication::Get().RequestDismissPopups();
	});

	auto row = WispNew(SHorizontalBox);
	*row
		+ row->AddSlot().Fill(1.0f)[ yesBtn ]
		+ row->AddSlot().Fill(1.0f)[ noBtn ];

	UIApplication::Get().ShowPopup(BuildContent(fc, fh, fs, title, message, std::move(row)));
}

} // namespace wisp
