#pragma once

#include "Wisp/Core.h"
#include "Wisp/DrawList.h"
#include "Wisp/FontCache.h"
#include "Wisp/Application.h"
#include "Wisp/Style.h"
#include "Wisp/Theme.h"
#include "Wisp/Components/EditableText.h"
#include "Wisp/Components/Button.h"
#include "Wisp/Components/TextBlock.h"
#include <functional>
#include <string>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace wisp {

template<typename T>
class TValueBox : public Widget
{
public:
	WISP_WIDGET_TYPE(ValueBox)

	TValueBox(FontCache* fc, FontHandle font, float fontSize, T initialValue = T{})
		: m_FontCache(fc), m_Font(font), m_FontSize(fontSize)
	{
		m_BgStyle.Background.Tint = { 0.08f, 0.08f, 0.12f, 1.0f };
		SetSupportsFocus(true);
		m_EditText = std::make_unique<EditableText>(fc, font, fontSize);
		SetupParentChild(this, m_EditText.get());
		m_EditText->SetOnTextChanged([this](const std::string& txt) {
			m_EditTextDirty = true;
		});

		m_DecBtn = std::make_unique<Button>();
		SetupParentChild(this, m_DecBtn.get());
		m_DecBtn->SetSupportsFocus(false);
		m_DecBtn->SetOnClicked([this]() { Decrement(); });
		m_DecBtn->SetNormalBrush({glm::vec4(0.15f,0.15f,0.22f,1)});
		m_DecBtn->SetHoveredBrush({glm::vec4(0.22f,0.22f,0.3f,1)});
		m_DecBtn->SetPressedBrush({glm::vec4(0.1f,0.1f,0.16f,1)});
		auto decLabel = std::make_unique<TextBlock>(fc, font, fontSize * 0.7f, "-", glm::vec4(0.8f,0.8f,0.8f,1));
		SetupParentChild(m_DecBtn.get(), decLabel.get());
		m_DecBtn->SetChild(std::move(decLabel));

		m_IncBtn = std::make_unique<Button>();
		SetupParentChild(this, m_IncBtn.get());
		m_IncBtn->SetSupportsFocus(false);
		m_IncBtn->SetOnClicked([this]() { Increment(); });
		m_IncBtn->SetNormalBrush({glm::vec4(0.15f,0.15f,0.22f,1)});
		m_IncBtn->SetHoveredBrush({glm::vec4(0.22f,0.22f,0.3f,1)});
		m_IncBtn->SetPressedBrush({glm::vec4(0.1f,0.1f,0.16f,1)});
		auto incLabel = std::make_unique<TextBlock>(fc, font, fontSize * 0.7f, "+", glm::vec4(0.8f,0.8f,0.8f,1));
		SetupParentChild(m_IncBtn.get(), incLabel.get());
		m_IncBtn->SetChild(std::move(incLabel));

		SetValue(initialValue);
	}

	T    GetValue() const { return m_Value; }
	void SetValue(T val)
	{
		m_Value = ClampValue(val);
		m_CommittedValue = m_Value;
		m_EditText->SetText(FormatValue());
		m_EditTextDirty = false;
		m_IsError = false;
		InvalidateLayout();
	}

	void SetStep(T step) { m_Step = step; }
	T    GetStep() const { return m_Step; }

	void SetClampRange(T min, T max) { m_MinValue = min; m_MaxValue = max; m_HasMin = true; m_HasMax = true; }
	void SetMinValue(T min) { m_MinValue = min; m_HasMin = true; }
	void SetMaxValue(T max) { m_MaxValue = max; m_HasMax = true; }
	void ClearClamp() { m_HasMin = false; m_HasMax = false; }
	bool HasMin() const { return m_HasMin; }
	bool HasMax() const { return m_HasMax; }

	void SetShowButtons(bool show) { m_ShowButtons = show; InvalidateLayout(); }
	bool GetShowButtons() const { return m_ShowButtons; }
	void SetEnableExpressionEval(bool enable) { m_EnableExpressionEval = enable; }
	bool GetEnableExpressionEval() const { return m_EnableExpressionEval; }

	void SetOnValueChanged(std::function<void(T)> cb)  { m_OnChanged = std::move(cb); }
	void SetOnValueCommitted(std::function<void(T)> cb) { m_OnCommitted = std::move(cb); }

	void SetColourIndicator(const glm::vec4& c) { m_HasIndicator = true; m_IndicatorColour = c; }
	void SetIndicatorLabel(const std::string& label) { m_IndicatorLabel = label; }
	void ClearColourIndicator() { m_HasIndicator = false; }

	// Overrides the minimum width (default 100 - the historical floor).
	// Content-sized hosts lower it so the box hugs its text; localization-
	// safe sizing.
	void SetMinimumWidth(float minWidth)
	{
		m_MinimumWidth = minWidth;
		if (m_EditText)
			m_EditText->SetMinimumWidth(minWidth);
		InvalidateLayout();
	}

	glm::vec2 ComputeDesiredSize() const override
	{
		float btnW = m_ShowButtons ? 48.0f : 0.0f;
		float editW = m_EditText->ComputeDesiredSize().x;
		float h = std::max(m_EditText->ComputeDesiredSize().y, 24.0f);
		float indictW = m_HasIndicator ? h : 0.0f;
		return { std::max(editW + btnW + indictW + 8.0f, m_MinimumWidth), h };
	}

	void ArrangeChildren(const Geometry& geom, ArrangedChildren& out) const override
	{
		float w = geom.LocalSize.x, h = geom.LocalSize.y;
		float btnW = 22.0f, btnOff = 2.0f;
		float indictW = m_HasIndicator ? h : 0.0f;
		float x = indictW;

		if (m_ShowButtons)
		{
			out.Add(m_DecBtn.get(), geom.MakeChild({x + btnOff, btnOff}, {btnW, h - btnOff * 2}));
			x += btnW + btnOff + 2.0f;
		}
		float editStart = x + 2.0f;
		float editW = w - editStart;
		if (m_ShowButtons) editW -= (btnW + btnOff + 4.0f);
		out.Add(m_EditText.get(), geom.MakeChild({editStart, 1.0f}, {std::max(editW, 10.0f), h - 2.0f}));

		if (m_ShowButtons)
		{
			float incX = w - btnW - btnOff;
			out.Add(m_IncBtn.get(), geom.MakeChild({incX, btnOff}, {btnW, h - btnOff * 2}));
		}
	}

	int OnPaint(const PaintArgs& args, const Geometry& geom, DrawList& drawList, int layerId, const WidgetStyleContext& style) const override
	{
		if (m_StyleDirty)
		{
			m_BgStyle = ResolveStyle(this, m_BgStyle);
			m_StyleDirty = false;
		}

		glm::vec2 absPos = geom.LocalToAbsolute({0, 0});
		float w = geom.LocalSize.x, h = geom.LocalSize.y;
		int clipIdx = args.InheritedClipIndex;

		glm::vec4 base = m_BgStyle.Background.Tint;
		glm::vec4 bgCol = m_IsError ? glm::vec4(0.18f, 0.06f, 0.06f, 1.0f)
			: (m_EditText->HasFocus() ? base * 1.5f
			: (m_IsHovered ? base * 1.25f : base));
		drawList.AddBox(layerId, absPos, {w, h}, bgCol, clipIdx);
		drawList.AddBox(layerId + 1, absPos, {w, 1}, glm::vec4(0.2f, 0.2f, 0.3f, 1.0f), clipIdx);
		drawList.AddBox(layerId + 1, {absPos.x, absPos.y + h - 1}, {w, 1}, glm::vec4(0.2f, 0.2f, 0.3f, 1.0f), clipIdx);

		if (m_HasIndicator)
		{
			drawList.AddBox(layerId + 2, absPos, {h, h}, m_IndicatorColour, clipIdx);
			if (!m_IndicatorLabel.empty() && m_FontCache)
			{
				FontMetrics fm = m_FontCache->GetMetrics(m_Font, m_FontSize * 0.7f);
				float labelW = m_FontCache->MeasureText(m_Font, m_IndicatorLabel, m_FontSize * 0.7f).x;
				float lx = absPos.x + (h - labelW) * 0.5f;
				float ly = absPos.y + (h - fm.LineHeight) * 0.5f;
				const uint8_t* p = (const uint8_t*)m_IndicatorLabel.data();
				const uint8_t* e = p + m_IndicatorLabel.size();
				while (p < e)
				{
					uint32_t cp;
					if((*p&0x80)==0){cp=*p;p++;}else if((*p&0xE0)==0xC0&&p+1<e){cp=((*p&0x1F)<<6)|(p[1]&0x3F);p+=2;}else if((*p&0xF0)==0xE0&&p+2<e){cp=((*p&0x0F)<<12)|((p[1]&0x3F)<<6)|(p[2]&0x3F);p+=3;}else{p++;continue;}
					const GlyphInfo* g = m_FontCache->GetGlyph(m_Font, cp, m_FontSize * 0.7f);
					if(g)
					{
						glm::vec2 uv[4]={{g->AtlasU0,g->AtlasV0},{g->AtlasU1,g->AtlasV0},{g->AtlasU1,g->AtlasV1},{g->AtlasU0,g->AtlasV1}};
						// Outlined so the letter stays legible over any
						// channel-tinted indicator swatch.
						drawList.AddTextOutlined(layerId + 3, {lx + g->BearingX, ly + fm.Ascent + g->BearingY},
							std::string(1,(char)cp), uv, g->AdvanceX, g->Height, glm::vec4(1,1,1,1),
							glm::vec4(0.0f,0.0f,0.0f,0.9f), 1.0f, clipIdx);
						lx += g->AdvanceX;
					}
				}
			}
		}

		// Track focus loss for commit
		bool hadFocus = m_EditHadFocus;
		m_EditHadFocus = m_EditText->HasFocus();
		if (hadFocus && !m_EditHadFocus && m_EditTextDirty)
			CommitEdit();

		// Paint children
		PaintArgs childArgs = args;
		childArgs.InheritedClipIndex = ApplyWidgetClipping(this, drawList, args.InheritedClipIndex, absPos, geom.LocalSize);
		ArrangedChildren arranged;
		ArrangeChildren(geom, arranged);
		int nextLayer = layerId + 3;
		WidgetStyleContext cs = style;
		for (auto& child : arranged)
		{
			cs.ColorAndOpacity = style.ColorAndOpacity * child.WidgetPtr->GetRenderTint();
			cs.ColorAndOpacity.a *= child.WidgetPtr->GetRenderOpacity();
			nextLayer = child.WidgetPtr->OnPaint(childArgs, child.Geom, drawList, nextLayer, cs);
		}
		return nextLayer;
	}

	Reply OnMouseEnter() override { m_IsHovered = true; return Reply::Unhandled(); }
	Reply OnMouseLeave() override { m_IsHovered = false; return Reply::Unhandled(); }

	Reply OnMouseWheel(const glm::vec2&, float delta) override
	{
		if (m_EditText->HasFocus())
		{
			if (delta > 0) Increment();
			else if (delta < 0) Decrement();
			return Reply::Handled_();
		}
		return Reply::Unhandled();
	}

	void ForEachChild(std::function<void(Widget*)> fn) override
	{
		if (m_DecBtn) fn(m_DecBtn.get());
		if (m_EditText) fn(m_EditText.get());
		if (m_IncBtn) fn(m_IncBtn.get());
	}

	bool SupportsFocus() const override { return m_SupportsFocus; }

private:
	static T DefaultStep()
	{
		if constexpr (std::is_same_v<T, float>) return T(0.1);
		else return T(1);
	}

	std::string FormatValue() const
	{
		char buf[64];
		if constexpr (std::is_same_v<T, float>)
		{
			snprintf(buf, sizeof(buf), "%.4f", (double)m_Value);
			char* p = buf + strlen(buf) - 1;
			while (*p == '0' && p > buf) p--;
			if (*p == '.') p--;
			*(p + 1) = '\0';
		}
		else if constexpr (std::is_same_v<T, int>)
			snprintf(buf, sizeof(buf), "%d", m_Value);
		return std::string(buf);
	}

	T ParseValue(const std::string& text) const
	{
		if constexpr (std::is_same_v<T, float>)
			return (T)strtof(text.c_str(), nullptr);
		else if constexpr (std::is_same_v<T, int>)
			return (T)strtol(text.c_str(), nullptr, 10);
		return T{};
	}

	T ClampValue(T val) const
	{
		if (m_HasMin && val < m_MinValue) val = m_MinValue;
		if (m_HasMax && val > m_MaxValue) val = m_MaxValue;
		return val;
	}

	T EvaluateExpression(const std::string& text, bool& ok) const
	{
		ok = false;
		if (text.empty()) return m_CommittedValue;
		const char* ptr = text.c_str();
		double result = 0.0;
		ok = EvalExpr(ptr, result);
		if (ok) return (T)result;
		T parsed = ParseValue(text);
		ok = true;
		return parsed;
	}

	static bool EvalExpr(const char*& s, double& out);
	static bool EvalTerm(const char*& s, double& out);
	static bool EvalFactor(const char*& s, double& out);

	void CommitEdit() const
	{
		m_EditTextDirty = false;
		std::string text = m_EditText->GetText();
		if (text.empty()) return;
		bool exprOk = false;
		T newVal = m_EnableExpressionEval ? EvaluateExpression(text, exprOk) : ParseValue(text);
		if (!exprOk && m_EnableExpressionEval) { m_IsError = true; return; }
		T clamped = ClampValue(newVal);
		if (clamped != m_Value) { m_Value = clamped; if (m_OnChanged) m_OnChanged(m_Value); }
		m_CommittedValue = m_Value;
		m_EditText->SetText(FormatValue());
		m_IsError = false;
		if (m_OnCommitted) m_OnCommitted(m_Value);
		InvalidateLayout();
	}

	void Increment()
	{
		m_Value = ClampValue(m_Value + m_Step);
		m_CommittedValue = m_Value;
		m_EditText->SetText(FormatValue());
		m_EditTextDirty = false;
		m_IsError = false;
		if (m_OnChanged) m_OnChanged(m_Value);
		if (m_OnCommitted) m_OnCommitted(m_Value);
		InvalidateLayout();
	}

	void Decrement()
	{
		m_Value = ClampValue(m_Value - m_Step);
		m_CommittedValue = m_Value;
		m_EditText->SetText(FormatValue());
		m_EditTextDirty = false;
		m_IsError = false;
		if (m_OnChanged) m_OnChanged(m_Value);
		if (m_OnCommitted) m_OnCommitted(m_Value);
		InvalidateLayout();
	}

	FontCache* m_FontCache;
	FontHandle m_Font;
	float      m_FontSize;
	mutable PanelStyle m_BgStyle{};
	mutable T m_Value{};
	mutable T m_CommittedValue{};
	T          m_Step = DefaultStep();
	T          m_MinValue{}, m_MaxValue{};
	bool       m_HasMin = false, m_HasMax = false;
	bool       m_ShowButtons = true;
	float      m_MinimumWidth = 100.0f;
	bool       m_EnableExpressionEval = true;
	mutable bool m_EditTextDirty = false;
	mutable bool m_IsError = false;
	bool       m_IsHovered = false;
	mutable bool m_HasIndicator = false;
	mutable glm::vec4 m_IndicatorColour{1.0f};
	std::string m_IndicatorLabel;
	std::unique_ptr<EditableText> m_EditText;
	std::unique_ptr<Button> m_DecBtn;
	std::unique_ptr<Button> m_IncBtn;
	mutable bool m_EditHadFocus = false;
	std::function<void(T)> m_OnChanged;
	std::function<void(T)> m_OnCommitted;
};

// String specialization — EditableText only, no buttons, no eval
template<>
class TValueBox<std::string> : public Widget
{
public:
	WISP_WIDGET_TYPE(ValueBox)

	TValueBox(FontCache* fc, FontHandle font, float fontSize, std::string initialValue = "")
		: m_FontCache(fc), m_Font(font), m_FontSize(fontSize)
	{
		m_BgStyle.Background.Tint = { 0.08f, 0.08f, 0.12f, 1.0f };
		SetSupportsFocus(true);
		m_EditText = std::make_unique<EditableText>(fc, font, fontSize);
		SetupParentChild(this, m_EditText.get());
		m_EditText->SetOnTextChanged([this](const std::string&) { m_EditTextDirty = true; });
		SetValue(std::move(initialValue));
	}

	std::string GetValue() const { return m_Value; }
	void SetValue(std::string val)
	{
		m_Value = std::move(val);
		m_CommittedValue = m_Value;
		m_EditText->SetText(m_Value);
		m_EditTextDirty = false;
		InvalidateLayout();
	}

	void SetOnValueCommitted(std::function<void(std::string)> cb) { m_OnCommitted = std::move(cb); }

	glm::vec2 ComputeDesiredSize() const override
	{
		float editW = m_EditText->ComputeDesiredSize().x;
		float h = std::max(m_EditText->ComputeDesiredSize().y, 24.0f);
		return { std::max(editW + 8.0f, 100.0f), h };
	}

	void ArrangeChildren(const Geometry& geom, ArrangedChildren& out) const override
	{
		float w = geom.LocalSize.x, h = geom.LocalSize.y;
		out.Add(m_EditText.get(), geom.MakeChild({2.0f, 1.0f}, {std::max(w - 4.0f, 10.0f), h - 2.0f}));
	}

	int OnPaint(const PaintArgs& args, const Geometry& geom, DrawList& drawList, int layerId, const WidgetStyleContext& style) const override
	{
		if (m_StyleDirty)
		{
			m_BgStyle = ResolveStyle(this, m_BgStyle);
			m_StyleDirty = false;
		}

		glm::vec2 absPos = geom.LocalToAbsolute({0, 0});
		float w = geom.LocalSize.x, h = geom.LocalSize.y;
		int clipIdx = args.InheritedClipIndex;

		glm::vec4 base = m_BgStyle.Background.Tint;
		glm::vec4 bgCol = m_EditText->HasFocus() ? base * 1.5f
			: (m_IsHovered ? base * 1.25f : base);
		drawList.AddBox(layerId, absPos, {w, h}, bgCol, clipIdx);
		drawList.AddBox(layerId + 1, absPos, {w, 1}, glm::vec4(0.2f, 0.2f, 0.3f, 1.0f), clipIdx);
		drawList.AddBox(layerId + 1, {absPos.x, absPos.y + h - 1}, {w, 1}, glm::vec4(0.2f, 0.2f, 0.3f, 1.0f), clipIdx);

		bool hadFocus = m_EditHadFocus;
		m_EditHadFocus = m_EditText->HasFocus();
		if (hadFocus && !m_EditHadFocus && m_EditTextDirty)
			CommitEdit();

		PaintArgs childArgs = args;
		childArgs.InheritedClipIndex = ApplyWidgetClipping(this, drawList, args.InheritedClipIndex, absPos, geom.LocalSize);
		ArrangedChildren arranged;
		ArrangeChildren(geom, arranged);
		int nextLayer = layerId + 2;
		WidgetStyleContext cs = style;
		for (auto& child : arranged)
		{
			cs.ColorAndOpacity = style.ColorAndOpacity * child.WidgetPtr->GetRenderTint();
			cs.ColorAndOpacity.a *= child.WidgetPtr->GetRenderOpacity();
			nextLayer = child.WidgetPtr->OnPaint(childArgs, child.Geom, drawList, nextLayer, cs);
		}
		return nextLayer;
	}

	Reply OnMouseEnter() override { m_IsHovered = true; return Reply::Unhandled(); }
	Reply OnMouseLeave() override { m_IsHovered = false; return Reply::Unhandled(); }

	void ForEachChild(std::function<void(Widget*)> fn) override { if (m_EditText) fn(m_EditText.get()); }
	bool SupportsFocus() const override { return m_SupportsFocus; }

private:
	void CommitEdit() const
	{
		m_EditTextDirty = false;
		m_Value = m_EditText->GetText();
		m_CommittedValue = m_Value;
		if (m_OnCommitted) m_OnCommitted(m_Value);
		InvalidateLayout();
	}

	FontCache* m_FontCache;
	FontHandle m_Font;
	float      m_FontSize;
	mutable PanelStyle m_BgStyle{};
	mutable std::string m_Value;
	mutable std::string m_CommittedValue;
	mutable bool m_EditTextDirty = false;
	bool       m_IsHovered = false;
	std::unique_ptr<EditableText> m_EditText;
	mutable bool m_EditHadFocus = false;
	std::function<void(std::string)> m_OnCommitted;
};

using SFloatBox  = TValueBox<float>;
using SIntBox    = TValueBox<int>;
using SStringBox = TValueBox<std::string>;

} // namespace wisp
