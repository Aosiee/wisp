#include "Wisp/Composites/MessageBox.h"
#include "Wisp/Declarative.h"
#include "Wisp/Components/Spacer.h"
#include "Wisp/StyleTokens.h"
#include "Wisp/Composites/PopupMenu.h"
#include "Wisp/Icons.h"

namespace wisp {

// The dialog buttons - the primary action in the accent, the others
// neutral; fixed-width so a row reads as a group.
static WidgetRef<SButton> MakeModalButton(FontCache* fc, FontHandle fh, float fs,
	const std::string& label, std::function<void()> onClick, bool primary = false)
{
	auto btn = WispNew(SButton);
	btn->SetNormalBrush(primary ? Brush{ tokens::AccentFill } : MessageBox::ButtonNormal)
		.SetHoveredBrush(primary ? Brush{ tokens::AccentFillHover } : MessageBox::ButtonHover)
		.SetPressedBrush(primary ? Brush{ tokens::AccentFillPressed } : MessageBox::ButtonPressed)
		.SetOnClicked(std::move(onClick));
	btn->SetContentPadding({ 18.0f, 5.0f, 18.0f, 5.0f });
	auto text = WispNew(STextBlock, fc, UIApplication::FontOr(FontRole::Medium, fh), fs, label,
		primary ? tokens::TextOnAccent : tokens::TextPrimary);
	btn->SetChild(std::move(text));
	return btn;
}

static std::unique_ptr<Widget> ButtonRowOf(std::vector<std::unique_ptr<Widget>> list)
{
	auto row = WispNew(SHorizontalBox);
	*row + row->AddSlot().Fill(1.0f)[ WispNew(SSpacer, glm::vec2(0,0)) ];
	for (size_t i = 0; i < list.size(); i++)
	{
		auto sized = WispNew(SSizeBox);
		sized->SetMinWidth(88.0f);
		sized->SetChild(std::move(list[i]));
		auto slot = row->AddSlot();
		slot.Data.PaddingLeft = i == 0 ? 0.0f : 8.0f;
		*row + std::move(slot)[ std::move(sized) ];
	}
	return std::unique_ptr<Widget>(std::move(row));
}

template<typename... Buttons>
static std::unique_ptr<Widget> ButtonRow(Buttons&&... buttons)
{
	std::vector<std::unique_ptr<Widget>> list;
	(list.push_back(std::unique_ptr<Widget>(std::move(buttons))), ...);
	return ButtonRowOf(std::move(list));
}

std::unique_ptr<Widget> MessageBox::BuildContent(FontCache* fc, FontHandle fh, float fs,
	const std::string& title, const std::string& message, std::unique_ptr<Widget> buttons)
{
	// The dialog card - an info glyph + the SemiBold title, the message
	// in the secondary tone, the buttons right-aligned at the bottom.
	auto titleRow = WispNew(SHorizontalBox);
	*titleRow
		+ titleRow->AddSlot().VAlign(VAlign::Center)[ WispNew(STextBlock, fc, UIApplication::FontOr(FontRole::Icons, fh), 18.0f, icons::Info, tokens::AccentHover) ]
		+ titleRow->AddSlot()[ WispNew(SSpacer, glm::vec2(10.0f, 1.0f)) ]
		+ titleRow->AddSlot().VAlign(VAlign::Center).Fill(1.0f)[ WispNew(STextBlock, fc, UIApplication::FontOr(FontRole::SemiBold, fh), tokens::FontTitle, title, tokens::TextPrimary) ];

	auto content = WispNew(SVerticalBox);
	auto messageSlot = content->AddSlot();
	messageSlot.Data.PaddingTop = 12.0f;
	messageSlot.Data.PaddingBottom = 20.0f;
	messageSlot.Data.PaddingLeft = 28.0f;
	*content
		+ content->AddSlot()[ std::move(titleRow) ]
		+ std::move(messageSlot).Fill(1.0f)[ WispNew(STextBlock, fc, fh, fs, message, tokens::TextSecondary) ]
		+ content->AddSlot()[ std::move(buttons) ];

	auto size = WispNew(SSizeBox);
	size->SetMinWidth(400.0f).SetMaxWidth(560.0f);
	size->SetChild(std::move(content));
	return std::make_unique<PopupFrame>(std::unique_ptr<Widget>(std::move(size)), 0.0f, 20.0f);
}

void MessageBox::Show(FontCache* fc, FontHandle fh, float fs,
	const std::string& title, const std::string& message)
{
	auto btn = MakeModalButton(fc, fh, fs, "OK", []() {
		UIApplication::Get().RequestDismissPopups();
	}, true);
	UIApplication::Get().ShowPopup(BuildContent(fc, fh, fs, title, message, ButtonRow(std::move(btn))));
}

void MessageBox::ShowOkCancel(FontCache* fc, FontHandle fh, float fs,
	const std::string& title, const std::string& message,
	std::function<void()> onOk, std::function<void()> onCancel)
{
	auto okBtn = MakeModalButton(fc, fh, fs, "OK", [onOk]() {
		if (onOk) onOk();
		UIApplication::Get().RequestDismissPopups();
	}, true);
	auto cancelBtn = MakeModalButton(fc, fh, fs, "Cancel", [onCancel]() {
		if (onCancel) onCancel();
		UIApplication::Get().RequestDismissPopups();
	});

	UIApplication::Get().ShowPopup(BuildContent(fc, fh, fs, title, message, ButtonRow(std::move(cancelBtn), std::move(okBtn))));
}

void MessageBox::ShowConfirm(FontCache* fc, FontHandle fh, float fs,
	const std::string& title, const std::string& message,
	std::function<void()> onConfirm, std::function<void()> onDecline)
{
	auto yesBtn = MakeModalButton(fc, fh, fs, "Yes", [onConfirm]() {
		if (onConfirm) onConfirm();
		UIApplication::Get().RequestDismissPopups();
	}, true);
	auto noBtn = MakeModalButton(fc, fh, fs, "No", [onDecline]() {
		if (onDecline) onDecline();
		UIApplication::Get().RequestDismissPopups();
	});

	UIApplication::Get().ShowPopup(BuildContent(fc, fh, fs, title, message, ButtonRow(std::move(noBtn), std::move(yesBtn))));
}

void MessageBox::ShowSaveDiscardCancel(FontCache* fc, FontHandle fh, float fs,
	const std::string& title, const std::string& message,
	std::function<void()> onSave, std::function<void()> onDiscard, std::function<void()> onCancel)
{
	auto saveBtn = MakeModalButton(fc, fh, fs, "Save", [onSave]() {
		UIApplication::Get().RequestDismissPopups();
		if (onSave) onSave();
	}, true);
	auto discardBtn = MakeModalButton(fc, fh, fs, "Don't Save", [onDiscard]() {
		UIApplication::Get().RequestDismissPopups();
		if (onDiscard) onDiscard();
	});
	auto cancelBtn = MakeModalButton(fc, fh, fs, "Cancel", [onCancel]() {
		UIApplication::Get().RequestDismissPopups();
		if (onCancel) onCancel();
	});
	UIApplication::Get().ShowPopup(BuildContent(fc, fh, fs, title, message,
		ButtonRow(std::move(discardBtn), std::move(cancelBtn), std::move(saveBtn))));
}

void MessageBox::ShowChoices(FontCache* fc, FontHandle fh, float fs,
	const std::string& title, const std::string& message, std::vector<Choice> choices)
{
	std::vector<std::unique_ptr<Widget>> buttons;
	for (Choice& choice : choices)
		buttons.push_back(std::unique_ptr<Widget>(MakeModalButton(fc, fh, fs, choice.Label, [onClick = std::move(choice.OnClick)]() {
			UIApplication::Get().RequestDismissPopups();
			if (onClick) onClick();
		}, choice.Primary)));
	UIApplication::Get().ShowPopup(BuildContent(fc, fh, fs, title, message, ButtonRowOf(std::move(buttons))));
}

} // namespace wisp
