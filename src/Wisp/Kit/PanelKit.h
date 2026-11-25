#pragma once

// The shared vocabulary of read-out panels in every Wisp app (shared
// outside the editor so a tool app - the crash reporter - reads
// like the editor): section headings, the banded key/value row (muted key,
// monospace value), the status pill, the centred empty state, the end and
// middle ellipses, a transient Toast and a Disclosure.

#include "Wisp/Application.h"
#include "Wisp/Components/BoxPanel.h"
#include "Wisp/Components/Border.h"
#include "Wisp/Components/Button.h"
#include "Wisp/Components/SizeBox.h"
#include "Wisp/Components/Spacer.h"
#include "Wisp/Components/TextBlock.h"
#include "Wisp/DrawList.h"
#include "Wisp/FontCache.h"
#include "Wisp/Icons.h"
#include "Wisp/StyleTokens.h"

#include <algorithm>
#include <functional>
#include <memory>
#include <string>

namespace wisp::kit {


	// A muted section heading and the slot it sits in (a little air above,
	// less below); AddSectionHeading adds both to a column.
	struct SlottedWidget
	{
		std::unique_ptr<Widget> Content;
		Slot SlotData;
	};

	inline SlottedWidget MakeSectionHeading(FontCache* fc, FontHandle font, const std::string& text)
	{
		SlottedWidget out;
		out.Content = std::make_unique<TextBlock>(fc,
			UIApplication::FontOr(FontRole::SemiBold, font), tokens::FontSmall, text, tokens::TextMuted);
		out.SlotData.SetPadding(10.0f, 0.0f);
		out.SlotData.PaddingTop = 12.0f;
		out.SlotData.PaddingBottom = 4.0f;
		return out;
	}

	inline void AddSectionHeading(VerticalBox& vbox, FontCache* fc, FontHandle font, const std::string& text)
	{
		SlottedWidget heading = MakeSectionHeading(fc, font, text);
		const size_t index = vbox.GetChildCount();
		vbox.PushChild(std::move(heading.Content));
		vbox.GetSlot(index) = heading.SlotData;
	}

	// A banded key/value row (in its default slot); `value` receives the
	// value label for the per-frame refresh.
	inline std::unique_ptr<Widget> MakeKeyValue(FontCache* fc, FontHandle font, const std::string& key,
		int band, TextBlock** value, float keyWidth = 150.0f, bool monoKey = false)
	{
		const FontHandle mono = UIApplication::FontOr(FontRole::Mono, font);
		auto row = std::make_unique<HorizontalBox>(BoxOrientation::Horizontal);
		auto keyBox = std::make_unique<SizeBox>();
		keyBox->SetWidthOverride(keyWidth);
		keyBox->SetChild(std::make_unique<TextBlock>(fc, monoKey ? mono : font,
			monoKey ? tokens::FontMono * 0.95f : tokens::FontBody * 0.95f, key, tokens::TextSecondary));
		auto text = std::make_unique<TextBlock>(fc, mono, tokens::FontMono * 0.95f, "--", tokens::TextPrimary);
		if (value)
			*value = text.get();
		*row
			+ row->AddSlot().VAlign(VAlign::Center)[ std::move(keyBox) ]
			+ row->AddSlot().Fill(1.0f).VAlign(VAlign::Center)[ std::move(text) ];
		auto bandBox = std::make_unique<Border>();
		bandBox->SetBrush({ (band % 2) ? tokens::PanelAlt : tokens::Panel });
		bandBox->SetPadding(10.0f, 4.0f);
		bandBox->SetChild(std::move(row));
		return bandBox;
	}

	// A banded key/value row added to a column; returns the value label for
	// the per-frame refresh (the panel keeps the content alive, the pointer
	// stays valid).
	inline TextBlock* AddKeyValue(VerticalBox& vbox, FontCache* fc, FontHandle font, const std::string& key,
		int band, float keyWidth = 150.0f, bool monoKey = false)
	{
		TextBlock* out = nullptr;
		vbox.AddChild(MakeKeyValue(fc, font, key, band, &out, keyWidth, monoKey));
		return out;
	}

	// A rounded, tinted status chip ("Baked", "Over budget"); Set() recolours
	// it in place.
	struct Pill
	{
		Border* Frame = nullptr;
		TextBlock* Text = nullptr;

		void Set(const std::string& text, const glm::vec4& colour) const
		{
			if (!Frame || !Text)
				return;
			Text->SetText(text);
			Text->SetColour(colour);
			Frame->SetBrush({ tokens::WithAlpha(colour, 0.16f) });
		}
	};

	inline std::unique_ptr<Border> MakePill(FontCache* fc, FontHandle font, const std::string& text,
		const glm::vec4& colour, Pill* out = nullptr)
	{
		auto label = std::make_unique<TextBlock>(fc, UIApplication::FontOr(FontRole::Medium, font),
			tokens::FontSmall, text, colour);
		label->SetWrapMode(WrapMode::None);
		auto frame = std::make_unique<Border>();
		frame->SetBrush({ tokens::WithAlpha(colour, 0.16f) });
		frame->SetCornerRadii(9.0f);
		frame->SetPadding(8.0f, 2.0f);
		if (out)
		{
			out->Frame = frame.get();
			out->Text = label.get();
		}
		frame->SetChild(std::move(label));
		return frame;
	}

	// Fits `text` into `maxWidth`, ending it with an ellipsis when it does
	// not fit (whole UTF-8 code points are dropped, then trailing spaces) -
	// a clipped column reads as truncated instead of as a cut-off word.
	inline std::string Ellipsize(FontCache* fc, FontHandle font, float size, const std::string& text, float maxWidth)
	{
		if (!fc || maxWidth <= 0.0f)
			return {};
		if (fc->MeasureText(font, text, size).x <= maxWidth)
			return text;
		const std::string ellipsis = "\u2026";
		std::string fit = text;
		while (!fit.empty())
		{
			size_t n = fit.size();
			while (n > 0 && (static_cast<unsigned char>(fit[n - 1]) & 0xC0) == 0x80)
				--n;
			fit.resize(n > 0 ? n - 1 : 0);
			while (!fit.empty() && fit.back() == ' ')
				fit.pop_back();
			if (fc->MeasureText(font, fit + ellipsis, size).x <= maxWidth)
				return fit + ellipsis;
		}
		return ellipsis;
	}

	// The centred icon + title + hint block an empty panel shows.
	inline void AddEmptyState(VerticalBox& vbox, FontCache* fc, FontHandle font, const char* icon,
		const std::string& title, const std::string& hint, float padTop = 40.0f)
	{
		auto add = [&](std::unique_ptr<TextBlock> w, float top) {
			w->SetWrapMode(WrapMode::None);
			auto slot = vbox.AddSlot();
			slot.HAlign(HAlign::Center);
			slot.Data.PaddingTop = top;
			vbox + std::move(slot)[ std::move(w) ];
		};
		add(std::make_unique<TextBlock>(fc, UIApplication::FontOr(FontRole::Icons, font), 30.0f,
			icon, tokens::TextDisabled), padTop);
		add(std::make_unique<TextBlock>(fc, UIApplication::FontOr(FontRole::Medium, font), tokens::FontBody,
			title, tokens::TextSecondary), 12.0f);
		if (!hint.empty())
		{
			// The hint wraps at a fixed width (an unwrapped line set a
			// docked panel's width and pushed the whole block off-centre).
			auto text = std::make_unique<TextBlock>(fc, font, tokens::FontSmall, hint, tokens::TextMuted);
			text->SetWrapMode(WrapMode::Word);
			text->SetAutoWrap(false);
			text->SetWrapWidth(320.0f); // fewer one-word last lines
			auto slot = vbox.AddSlot();
			slot.HAlign(HAlign::Center);
			slot.Data.PaddingTop = 4.0f;
			vbox + std::move(slot)[ std::move(text) ];
		}
	}

	// Fits `text` into `maxWidth` by dropping its MIDDLE ("Tiny…Layer"), so
	// both a long name's start and its end stay visible (a C++ frame's
	// namespace and its function).
	inline std::string MiddleEllipsize(FontCache* fc, FontHandle font, float size, const std::string& text, float maxWidth)
	{
		if (!fc || maxWidth <= 0.0f)
			return {};
		if (fc->MeasureText(font, text, size).x <= maxWidth)
			return text;
		const std::string ellipsis = "\u2026";
		auto boundary = [&](size_t at) { // back to a code point's first byte
			while (at > 0 && at < text.size() && (static_cast<unsigned char>(text[at]) & 0xC0) == 0x80)
				--at;
			return at;
		};
		// Keep as much as fits, split evenly between the two ends.
		size_t lo = 0, hi = text.size();
		std::string best = ellipsis;
		while (lo <= hi)
		{
			const size_t keep = (lo + hi) / 2;
			const size_t head = boundary(keep - keep / 2);
			const size_t tailStart = boundary(text.size() - keep / 2);
			const std::string candidate = text.substr(0, head) + ellipsis + text.substr(std::max(tailStart, head));
			if (fc->MeasureText(font, candidate, size).x <= maxWidth)
			{
				best = candidate;
				lo = keep + 1;
			}
			else
			{
				if (keep == 0)
					break;
				hi = keep - 1;
			}
		}
		return best;
	}

	// A transient confirmation ("Copied", "Couldn't open the folder: ..."):
	// a rounded chip that shows for `seconds`, then fades out. Place it in an
	// overlay (bottom centre); Show() restarts it. Colour-coded AND worded.
	class Toast : public Widget
	{
	public:
		WISP_WIDGET_TYPE(Toast)

		enum class Tone : uint8_t { Info, Success, Error };

		Toast(FontCache* fc, FontHandle font) : m_FontCache(fc), m_Font(font) { SetVisibility(Visibility::HitTestInvisible); }

		void Show(std::string text, Tone tone = Tone::Success, float seconds = 2.5f)
		{
			m_Text = std::move(text);
			m_Tone = tone;
			m_ShownAt = UIApplication::GetAnimTime();
			m_Seconds = seconds;
			InvalidateLayout();
			RequestAnimationTick();
		}
		const std::string& GetText() const { return m_Text; }
		bool IsShowing() const { return !m_Text.empty() && UIApplication::GetAnimTime() - m_ShownAt < m_Seconds + kFade; }

		glm::vec2 ComputeDesiredSize() const override
		{
			if (m_Text.empty() || !m_FontCache)
				return { 0.0f, 0.0f };
			const glm::vec2 text = m_FontCache->MeasureText(m_Font, Label(), tokens::FontBody);
			return { text.x + 32.0f, text.y + 16.0f };
		}
		void ArrangeChildren(const Geometry&, ArrangedChildren&) const override {}
		void TickAnimation(float) override
		{
			if (!IsShowing())
				CancelAnimationTick();
		}
		int OnPaint(const PaintArgs& args, const Geometry& geom, DrawList& dl, int layer, const WidgetStyleContext&) const override
		{
			if (!IsShowing() || !m_FontCache)
				return layer;
			const float age = UIApplication::GetAnimTime() - m_ShownAt;
			const float alpha = age < m_Seconds ? 1.0f : std::max(0.0f, 1.0f - (age - m_Seconds) / kFade);
			const glm::vec4 accent = m_Tone == Tone::Error ? tokens::Error : m_Tone == Tone::Success ? tokens::Success : tokens::Info;
			const glm::vec2 pos = geom.LocalToAbsolute({ 0.0f, 0.0f });
			const float r[4] = { tokens::RadiusMedium, tokens::RadiusMedium, tokens::RadiusMedium, tokens::RadiusMedium };
			const int clip = args.InheritedClipIndex;
			dl.AddRoundedBox(layer, pos, geom.LocalSize, tokens::WithAlpha(tokens::Popup, 0.97f * alpha), r, 1.0f,
				tokens::WithAlpha(accent, 0.8f * alpha), clip);
			// The text: the face's glyphs at the pixel-snapped baseline.
			const FontMetrics metrics = m_FontCache->GetMetrics(m_Font, tokens::FontBody);
			float x = std::round(pos.x + 16.0f);
			const float baseline = std::round(pos.y + (geom.LocalSize.y - metrics.LineHeight) * 0.5f + metrics.Ascent);
			const std::string label = Label();
			for (size_t i = 0; i < label.size();)
			{
				uint32_t cp = (unsigned char)label[i];
				int len = cp < 0x80 ? 1 : (cp >> 5) == 0x6 ? 2 : (cp >> 4) == 0xE ? 3 : 4;
				if (len == 2) cp = ((cp & 0x1F) << 6) | (label[i + 1] & 0x3F);
				else if (len == 3) cp = ((cp & 0x0F) << 12) | ((label[i + 1] & 0x3F) << 6) | (label[i + 2] & 0x3F);
				else if (len == 4) cp = ((cp & 0x07) << 18) | ((label[i + 1] & 0x3F) << 12) | ((label[i + 2] & 0x3F) << 6) | (label[i + 3] & 0x3F);
				if (const GlyphInfo* g = m_FontCache->GetGlyph(m_Font, cp, tokens::FontBody))
				{
					const glm::vec4 tint = tokens::WithAlpha(i < IconBytes() ? accent : tokens::TextPrimary, alpha);
					AddGlyph(dl, layer + 1, { x, baseline }, *g, tint, clip);
					x += g->AdvanceX;
				}
				i += (size_t)len;
			}
			const_cast<Toast*>(this)->RequestAnimationTick();
			return layer + 2;
		}

	private:
		static constexpr float kFade = 0.4f;
		const char* Icon() const { return m_Tone == Tone::Error ? icons::Error : icons::Success; }
		size_t IconBytes() const { return std::string(Icon()).size(); }
		std::string Label() const { return std::string(Icon()) + "  " + m_Text; }

		FontCache* m_FontCache;
		FontHandle m_Font;
		std::string m_Text;
		Tone m_Tone = Tone::Success;
		float m_ShownAt = -100.0f;
		float m_Seconds = 2.5f;
	};

	// A header row with a chevron that opens and closes a body: the open
	// state shows (the chevron points down), unlike a plain toggle button.
	class Disclosure : public VerticalBox
	{
	public:
		WISP_WIDGET_TYPE(Disclosure)

		Disclosure(FontCache* fc, FontHandle font, const std::string& title, std::unique_ptr<Widget> body, bool open = false)
			: VerticalBox(BoxOrientation::Vertical)
		{
			auto header = std::make_unique<Button>();
			header->SetNormalBrush({ tokens::WithAlpha(tokens::Panel, 0.0f) });
			header->SetHoveredBrush({ tokens::Hover });
			header->SetPressedBrush({ tokens::Pressed });
			header->SetCornerRadius(tokens::RadiusSmall);
			header->SetContentPadding({ 4.0f, 3.0f, 8.0f, 3.0f });
			auto row = std::make_unique<HorizontalBox>(BoxOrientation::Horizontal);
			auto chevron = std::make_unique<TextBlock>(fc, UIApplication::FontOr(FontRole::Icons, font), 12.0f,
				open ? icons::ChevronDown : icons::ChevronRight, tokens::TextSecondary);
			m_Chevron = chevron.get();
			*row + row->AddSlot().VAlign(VAlign::Center)[ std::move(chevron) ];
			*row + row->AddSlot()[ std::make_unique<Spacer>(glm::vec2(6.0f, 1.0f)) ];
			*row + row->AddSlot().VAlign(VAlign::Center)[ std::make_unique<TextBlock>(fc, UIApplication::FontOr(FontRole::Medium, font),
				tokens::FontBody, title, tokens::TextPrimary) ];
			header->SetChild(std::move(row));
			header->SetOnClicked([this]() { SetOpen(!m_Open); });
			*this + AddSlot().HAlign(HAlign::Left)[ std::move(header) ];
			m_Body = body.get();
			*this + AddSlot()[ std::move(body) ];
			SetOpen(open);
		}

		void SetOpen(bool open)
		{
			m_Open = open;
			m_Body->SetVisibility(open ? Visibility::Visible : Visibility::Collapsed);
			m_Chevron->SetText(open ? icons::ChevronDown : icons::ChevronRight);
			if (m_OnToggle)
				m_OnToggle(open);
		}
		bool IsOpen() const { return m_Open; }
		void SetOnToggle(std::function<void(bool)> fn) { m_OnToggle = std::move(fn); }

	private:
		TextBlock* m_Chevron = nullptr;
		Widget* m_Body = nullptr;
		bool m_Open = false;
		std::function<void(bool)> m_OnToggle;
	};

} // namespace wisp::kit
